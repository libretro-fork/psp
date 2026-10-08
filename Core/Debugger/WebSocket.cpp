// Copyright (c) 2017- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <vector>

#include <retro_atomic.h>

#include "Common/Net/Cancel.h"
#include "Common/Thread/MpscQueue.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/TimeUtil.h"
#include "Core/Core.h"
#include "Core/Debugger/WebSocket.h"
#include "Core/Debugger/WebSocket/WebSocketUtils.h"
#include "Core/HLE/sceCtrl.h"

// This WebSocket (connected through the same port as disc sharing) allows API/debugger access to PPSSPP.
// Currently, the only subprotocol "debugger.ppsspp.org" uses a simple JSON based interface.
//
// Messages to and from PPSSPP follow the same basic format:
//    { "event": "NAME", ... }
//
// And are primarily of these types:
//  * Events from the debugger/client (you) to PPSSPP
//    If there's a response, it will generally use the same name.  It may not be immedate - it's an event.
//  * Spontaneous events from PPSSPP
//    Things like logs, breakpoint hits, etc. not directly requested.
//
// Otherwise you may see error events which indicate PPSSPP couldn't understand or failed internally:
//  - "event": "error"
//  - "message": A string describing what happened.
//  - "level": Integer severity level. (1 = NOTICE, 2 = ERROR, 3 = WARN, 4 = INFO, 5 = DEBUG, 6 = VERBOSE)
//  - "ticket": Optional, present if in response to an event with a "ticket" field, simply repeats that value.
//
// At start, please send a "version" event.  See WebSocket/GameSubscriber.cpp for more details.
//
// For other events, look inside Core/Debugger/WebSocket/ for details on each event.

#include "Core/Debugger/WebSocket/GameBroadcaster.h"
#include "Core/Debugger/WebSocket/InputBroadcaster.h"
#include "Core/Debugger/WebSocket/LogBroadcaster.h"
#include "Core/Debugger/WebSocket/SteppingBroadcaster.h"

#include "Core/Debugger/WebSocket/BreakpointSubscriber.h"
#include "Core/Debugger/WebSocket/CPUCoreSubscriber.h"
#include "Core/Debugger/WebSocket/DisasmSubscriber.h"
#include "Core/Debugger/WebSocket/GameSubscriber.h"
#include "Core/Debugger/WebSocket/GPUBufferSubscriber.h"
#include "Core/Debugger/WebSocket/GPUDisasmSubscriber.h"
#include "Core/Debugger/WebSocket/GPURecordSubscriber.h"
#include "Core/Debugger/WebSocket/GPUStatsSubscriber.h"
#include "Core/Debugger/WebSocket/HLEKernelObjectSubscriber.h"
#include "Core/Debugger/WebSocket/HLESubscriber.h"
#include "Core/Debugger/WebSocket/InputSubscriber.h"
#include "Core/Debugger/WebSocket/LogConfigSubscriber.h"
#include "Core/Debugger/WebSocket/MemoryInfoSubscriber.h"
#include "Core/Debugger/WebSocket/MemorySubscriber.h"
#include "Core/Debugger/WebSocket/ReplaySubscriber.h"
#include "Core/Debugger/WebSocket/SteppingSubscriber.h"
#include "Core/Debugger/WebSocket/ClientConfigSubscriber.h"

typedef DebuggerSubscriber *(*SubscriberInit)(DebuggerEventHandlerMap &map);
static const std::vector<SubscriberInit> subscribers({
	&WebSocketBreakpointInit,
	&WebSocketCPUCoreInit,
	&WebSocketDisasmInit,
	&WebSocketGameInit,
	&WebSocketGPUBufferInit,
	&WebSocketGPUDisasmInit,
	&WebSocketGPURecordInit,
	&WebSocketGPUStatsInit,
	&WebSocketHLEKernelObjectInit,
	&WebSocketHLEInit,
	&WebSocketInputInit,
	&WebSocketLogConfigInit,
	&WebSocketMemoryInfoInit,
	&WebSocketMemoryInit,
	&WebSocketReplayInit,
	&WebSocketSteppingInit,
	&WebSocketClientConfigInit,
});

// Threads: each connection runs on its own thread (see NewThreadExecutor). Handlers do all
// emulator-state access inside Core_RunOnCPUThread(); anything the emulator produces on its own
// reaches the connection through its DebuggerEventSink. See docs/DebuggerThreading.md.

// A log-only breakpoint in a hot loop can produce events far faster than a connection sends them.
// Past this many queued, new ones are dropped; cpu.breakpoint.hit carries a sequence number so a
// client can tell exactly how many it missed.
static constexpr int MAX_PENDING_EVENTS = 4096;

// Per-connection mailbox. Events the CPU thread produces (cpu.stepping, game.start, input, ...) are
// formatted there and posted here, so a connection's own thread never reads emulator state.
// Refcounted: the connection and the CPU thread's g_sinks each hold one reference.
class DebuggerEventSink final : public DebuggerMailbox {
public:
	void Post(const char *category, std::string json) override {
		if (retro_atomic_fetch_add_int(&pendingCount_, 1) >= MAX_PENDING_EVENTS) {
			retro_atomic_fetch_sub_int(&pendingCount_, 1);
			return;
		}
		pending_.Push(PendingEvent{ category, std::move(json) });
		Wake();
	}

	void Wake() override {
		// Only the first wake since the connection last looked touches the socket.
		if (retro_atomic_exchange_int(&wakePending_, 1) == 0) {
			wake_.Wake();
		}
	}

	void WatchPress(int pressId, std::string json) override {
		pressInbox_.Push(PendingPress{ pressId, std::move(json) });
	}

	// Connection thread: right after waking, before looking at anything that wakes it.
	void Rearm() {
		wake_.Drain();
		retro_atomic_exchange_int(&wakePending_, 0);
	}

	// Connection thread.
	template <class F>
	void TakeEvents(F f) {
		int n = 0;
		pending_.Drain([&](PendingEvent &&ev) {
			n++;
			f(ev.category, ev.json);
		});
		if (n != 0) {
			retro_atomic_fetch_sub_int(&pendingCount_, n);
		}
	}

	intptr_t WakeFd() const { return wake_.Fd(); }

	// CPU thread: posts the timed presses that are over.
	void CheckPresses() {
		pressInbox_.Drain([&](PendingPress &&press) {
			presses_.push_back(std::move(press));
		});
		for (size_t i = 0; i < presses_.size(); ) {
			if (!__CtrlPressActive(presses_[i].id)) {
				Post(nullptr, std::move(presses_[i].json));
				presses_.erase(presses_.begin() + i);
			} else {
				++i;
			}
		}
	}

	void Release() {
		if (retro_atomic_fetch_sub_int(&refs_, 1) == 1) {
			delete this;
		}
	}

	// Set by the connection when it leaves.
	retro_atomic_int_t closed{ 0 };

	// CPU thread only. A debugger that connects while the CPU is already stopped still wants to
	// hear about it.
	bool needsSteppingPrime = true;
	InputBroadcaster input;

private:
	struct PendingEvent {
		const char *category;
		std::string json;
	};
	struct PendingPress {
		int id;
		std::string json;
	};

	net::WakeSocket wake_;
	retro_atomic_int_t wakePending_{ 0 };
	retro_atomic_int_t refs_{ 2 };
	MpscQueue<PendingEvent> pending_;
	retro_atomic_int_t pendingCount_{ 0 };
	MpscQueue<PendingPress> pressInbox_;
	// CPU thread only.
	std::vector<PendingPress> presses_;
};

// The CPU thread's list of live connections; new ones arrive through g_newSinks.
static std::vector<DebuggerEventSink *> g_sinks;
static MpscQueue<DebuggerEventSink *> g_newSinks;
// Connected debuggers, so the breakpoint path can check "is anyone listening" with one load.
static retro_atomic_int_t g_sinkCount{ 0 };

static void RegisterSink(DebuggerEventSink *sink) {
	retro_atomic_fetch_add_int(&g_sinkCount, 1);
	g_newSinks.Push(sink);
}

// The connection's last use of sink.
static void UnregisterSink(DebuggerEventSink *sink) {
	retro_atomic_store_release_int(&sink->closed, 1);
	retro_atomic_fetch_sub_int(&g_sinkCount, 1);
	sink->Release();
	// A CPU thread stopped in the debugger may want to know nobody's left to resume it.
	Core_WakeCPUThread();
}

// CPU thread.
static void UpdateSinks() {
	g_newSinks.Drain([](DebuggerEventSink *sink) {
		g_sinks.push_back(sink);
	});
	for (size_t i = 0; i < g_sinks.size(); ) {
		if (retro_atomic_load_acquire_int(&g_sinks[i]->closed)) {
			g_sinks[i]->Release();
			g_sinks.erase(g_sinks.begin() + i);
		} else {
			++i;
		}
	}
}

bool WebSocketDebuggerHasClients() {
	return retro_atomic_load_relaxed_int(&g_sinkCount) != 0;
}

void WebSocketNotifyBreakpointHit(const BreakpointHit &hit) {
	// Counts hits produced, not hits delivered, so a gap in what a client receives tells it how
	// many were dropped by the cap in Post().
	static uint64_t g_hitSequence = 0;

	if (!WebSocketDebuggerHasClients())
		return;
	UpdateSinks();
	if (g_sinks.empty())
		return;

	JsonWriter j;
	j.begin();
	j.writeString("event", "cpu.breakpoint.hit");
	j.writeFloat("sequence", (double)++g_hitSequence);
	WriteBreakpointHit(j, hit);
	j.end();
	const std::string json = j.str();

	for (DebuggerEventSink *sink : g_sinks)
		sink->Post("breakpoint", json);
}

void WebSocketDebuggerTick() {
	// Poll unconditionally, even with nothing connected: these track transitions, and skipping them
	// would let the "previous" state go stale and fire a bogus event at whoever connects next.
	const std::string gameEvent = GameBroadcaster::PollChange();
	const std::string steppingEvent = SteppingBroadcaster::PollChange();

	UpdateSinks();
	if (g_sinks.empty())
		return;

	std::string steppingPrime;
	std::vector<std::string> inputEvents;
	for (DebuggerEventSink *sink : g_sinks) {
		sink->CheckPresses();

		inputEvents.clear();
		sink->input.Poll(&inputEvents);
		for (std::string &ev : inputEvents)
			sink->Post("input", std::move(ev));

		if (sink->needsSteppingPrime) {
			sink->needsSteppingPrime = false;
			// Only format it if somebody actually needs it.
			if (steppingPrime.empty())
				steppingPrime = SteppingBroadcaster::CurrentState();
			if (!steppingPrime.empty())
				sink->Post("stepping", steppingPrime);
			continue;
		}
		if (!gameEvent.empty())
			sink->Post("game", gameEvent);
		if (!steppingEvent.empty())
			sink->Post("stepping", steppingEvent);
	}
}

void HandleDebuggerRequest(const http::ServerRequest &request, const net::CancelToken *stop) {
	SetCurrentThreadName("WebSocketDebugger");

	net::WebSocketServer *ws = net::WebSocketServer::CreateAsUpgrade(request, "debugger.ppsspp.org");
	if (!ws) {
		return;
	}

	WebSocketClientInfo client_info;
	auto& disallowed_config = client_info.disallowed;
	// Seed every broadcaster category. broadcast.config.set only accepts keys that already exist
	// here (so a typo is rejected rather than silently ignored), and these otherwise only appear
	// as a side effect of operator[] the first time each category actually broadcasts - which
	// meant "game" and "stepping" were rejected as unsupported until one happened to fire, even
	// though they're documented and valid. Keep in sync with the categories posted to the sink.
	for (const char *category : { "logger", "input", "game", "stepping", "breakpoint" })
		disallowed_config[category] = false;

	DebuggerEventSink *sink = new DebuggerEventSink();
	RegisterSink(sink);

	{
		LogBroadcaster logger(sink);

		DebuggerEventHandlerMap eventHandlers;
		std::vector<DebuggerSubscriber *> subscriberData;
		for (auto init : subscribers) {
			DebuggerSubscriber *sub = init(eventHandlers);
			if (sub) {
				sub->mailbox = sink;
			}
			subscriberData.push_back(sub);
		}

		ws->SetTextHandler([&](const std::string &t) {
			JsonReader reader(t.c_str(), t.size());
			if (!reader.ok()) {
				ws->Send(DebuggerErrorEvent("Bad message: invalid JSON", LogLevel::LERROR));
				return;
			}

			const JsonGet root = reader.root();
			const char *event = root ? root.getStringOr("event", nullptr) : nullptr;
			if (!event) {
				ws->Send(DebuggerErrorEvent("Bad message: no event property", LogLevel::LERROR, root));
				return;
			}

			DEBUG_LOG(Log::Debugger, "WS: Handling '%s'", event);

			DebuggerRequest req(event, ws, root, &client_info);
			auto eventFunc = eventHandlers.find(event);
			if (eventFunc != eventHandlers.end()) {
				eventFunc->second(req);
				if (!req.Finish()) {
					// The handler arranged something that finishes later - a step, a resume, a stats
					// feed - rather than answering now. A client that asked for it gets told so, so it
					// can tell "accepted, wait for the event" from "dropped on the floor" without
					// carrying a hardcoded list of the events that don't answer. Everyone else sees
					// exactly what they saw before; see client.config.set for why it can't be the
					// default.
					if (client_info.acknowledgeDeferred)
						ws->Send(DebuggerDeferredEvent(event, root));
				}
			} else {
				req.Fail("Bad message: unknown event");
			}
		});

		ws->SetBinaryHandler([&](const std::vector<uint8_t> &d) {
			ERROR_LOG(Log::Debugger, "Received binary WebSocket frame, not supported");
			ws->Send(DebuggerErrorEvent("Bad message: binary WebSocket frames are not supported", LogLevel::LERROR));
		});

		// Blocks until the client sends something, the socket drains, the sink is woken, or the
		// server stops. Once we've reacted to stop, its fd stays readable, so leave it out.
		const intptr_t wakeFds[2] = { sink->WakeFd(), stop ? stop->WakeFd() : -1 };
		bool stopping = false;
		while (ws->Process(wakeFds, stopping ? 1 : 2)) {
			sink->Rearm();

			// The client can explicitly ask not to be notified about some events.
			if (!disallowed_config["logger"]) {
				logger.Broadcast(ws);
			} else {
				logger.Discard();
			}

			sink->TakeEvents([&](const char *category, const std::string &json) {
				if (!category || !disallowed_config[category]) {
					ws->Send(json);
				}
			});

			for (size_t i = 0; i < subscribers.size(); ++i) {
				if (subscriberData[i]) {
					subscriberData[i]->Broadcast(ws);
				}
			}

			if (!stopping && stop && stop->IsCancelled()) {
				stopping = true;
				ws->Close(net::WebSocketClose::GOING_AWAY);
			}
		}

		for (size_t i = 0; i < subscribers.size(); ++i) {
			delete subscriberData[i];
		}
	}

	UnregisterSink(sink);

	delete ws;
	request.In()->Discard();
}
