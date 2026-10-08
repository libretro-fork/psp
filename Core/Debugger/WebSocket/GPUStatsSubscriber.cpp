// Copyright (c) 2021- PPSSPP Project.

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

#include <cstring>
#include <vector>

#include <retro_atomic.h>

#include "Common/Data/Text/StringWriter.h"
#include "Common/Thread/MpscQueue.h"
#include "Core/Debugger/WebSocket/GPUStatsSubscriber.h"
#include "Core/Core.h"
#include "Core/HW/Display.h"
#include "Core/System.h"

struct CollectedStats {
	float vps;
	float fps;
	float actual_fps;
	char statbuf[4096];
	std::vector<float> frameTimes;
	std::vector<float> sleepTimes;
	int frameTimePos;
};

struct DebuggerGPUStatsEvent {
	const CollectedStats &s;
	const std::string &ticket;

	operator std::string() {
		JsonWriter j;
		j.begin();
		j.writeString("event", "gpu.stats.get");
		if (!ticket.empty())
			j.writeRaw("ticket", ticket);
		j.pushDict("fps");
		j.writeFloat("actual", s.actual_fps);
		j.writeFloat("target", s.fps);
		j.pop();
		j.pushDict("vblanksPerSecond");
		j.writeFloat("actual", s.vps);
		j.writeFloat("target", 60.0 / 1.001);
		j.pop();
		j.writeString("info", s.statbuf);
		j.pushDict("timing");
		j.pushArray("frames");
		for (double t : s.frameTimes)
			j.writeFloat(t);
		j.pop();
		j.pushArray("sleep");
		for (double t : s.sleepTimes)
			j.writeFloat(t);
		j.pop();
		j.writeInt("pos", s.frameTimePos);
		j.pop();
		j.end();
		return j.str();
	}
};

struct WebSocketGPUStatsState : public DebuggerSubscriber {
	WebSocketGPUStatsState();
	~WebSocketGPUStatsState();
	void Get(DebuggerRequest &req);
	void Feed(DebuggerRequest &req);

	void Broadcast(net::WebSocketServer *ws) override;

	static void FlipForwarder(void *thiz);
	void FlipListener();

protected:
	// The WebSocket thread's.
	bool forced_ = false;
	std::string lastTicket_;
	// Set by the WebSocket thread, read on the emu thread at each flip.
	retro_atomic_int_t sendNext_{ 0 };
	retro_atomic_int_t sendFeed_{ 0 };
	// Collected on the emu thread, sent from the WebSocket thread.
	MpscQueue<CollectedStats> pendingStats_;
};

DebuggerSubscriber *WebSocketGPUStatsInit(DebuggerEventHandlerMap &map) {
	auto p = new WebSocketGPUStatsState();
	map["gpu.stats.get"] = [p](DebuggerRequest &req) { p->Get(req); };
	map["gpu.stats.feed"] = [p](DebuggerRequest &req) { p->Feed(req); };

	return p;
}

WebSocketGPUStatsState::WebSocketGPUStatsState() {
	// The flip listeners belong to the CPU thread.
	Core_RunOnCPUThread([this] { __DisplayListenFlip(&WebSocketGPUStatsState::FlipForwarder, this); });
}

WebSocketGPUStatsState::~WebSocketGPUStatsState() {
	// PSP_ForceDebugStats bumps a plain counter, so do it on the CPU thread that owns it - see
	// Core_RunOnCPUThread() in Core.h.
	const bool forced = forced_;
	Core_RunOnCPUThread([this, forced] {
		if (forced) {
			PSP_ForceDebugStats(false);
		}
		__DisplayForgetFlip(&WebSocketGPUStatsState::FlipForwarder, this);
	});
}

void WebSocketGPUStatsState::FlipForwarder(void *thiz) {
	WebSocketGPUStatsState *p = (WebSocketGPUStatsState *)thiz;
	p->FlipListener();
}

void WebSocketGPUStatsState::FlipListener() {
	if (!retro_atomic_load_acquire_int(&sendNext_) && !retro_atomic_load_acquire_int(&sendFeed_))
		return;

	// Okay, collect the data (we'll actually send at next Broadcast.)
	CollectedStats stats;

	__DisplayGetFPS(&stats.vps, &stats.fps, &stats.actual_fps);

	StringWriter w(stats.statbuf);
	__DisplayGetDebugStats(w);

	int valid;
	float *sleepHistory;
	float *history = __DisplayGetFrameTimes(&valid, &stats.frameTimePos, &sleepHistory);

	stats.frameTimes.resize(valid);
	stats.sleepTimes.resize(valid);
	if (valid > 0) {
		memcpy(&stats.frameTimes[0], history, sizeof(float) * valid);
		memcpy(&stats.sleepTimes[0], sleepHistory, sizeof(float) * valid);
	}

	pendingStats_.Push(std::move(stats));
	retro_atomic_store_release_int(&sendNext_, 0);
	mailbox->Wake();
}

// Get next GPU stats (gpu.stats.get)
//
// No parameters.
//
// Response (same event name):
//  - fps: object with "actual" and "target" properties, representing frames per second.
//  - vblanksPerSecond: object with "actual" and "target" properties, for vblank cycles.
//  - info: string, representation of backend-dependent statistics.
//  - timing: object with properties:
//     - frames: array of numbers, each representing the time taken for a frame.
//     - sleep: array of numbers, each representing the delay time waiting for next frame.
//     - pos: number, index of the current frame (not always last.)
//
// Note: stats are returned after the next flip completes (paused if CPU or GPU in break.)
// Note: info and timing may not be accurate if certain settings are disabled.
// Note: sending this event with no ticket will not trigger a response! (TODO: maybe fix this?)
void WebSocketGPUStatsState::Get(DebuggerRequest &req) {
	if (PSP_GetBootState() != BootState::Complete)
		return req.Fail("CPU not started");

	const JsonNode *value = req.data.get("ticket");
	lastTicket_ = value ? json_stringify(value) : "";
	retro_atomic_store_release_int(&sendNext_, 1);
}

// Setup GPU stats feed (gpu.stats.feed)
//
// Parameters:
//  - enable: optional boolean, pass false to stop the feed.
//
// No immediate response (only a "deferred" event, if the client asked for those via
// client.config.set).  Events sent each frame (as gpu.stats.get.)
//
// Note: info and timing will be accurate after the first frame.
void WebSocketGPUStatsState::Feed(DebuggerRequest &req) {
	if (PSP_GetBootState() != BootState::Complete)
		return req.Fail("CPU not started");
	bool enable = true;
	if (!req.ParamBool("enable", &enable, DebuggerParamType::OPTIONAL))
		return;

	retro_atomic_store_release_int(&sendFeed_, enable ? 1 : 0);
	if (forced_ != enable) {
		Core_RunOnCPUThread([enable] { PSP_ForceDebugStats(enable); });
		forced_ = enable;
	}
}

void WebSocketGPUStatsState::Broadcast(net::WebSocketServer *ws) {
	if (pendingStats_.Empty()) {
		return;
	}
	std::vector<CollectedStats> pending;
	pendingStats_.Drain([&](CollectedStats &&stats) {
		pending.push_back(std::move(stats));
	});

	const bool sendFeed = retro_atomic_load_acquire_int(&sendFeed_) != 0;
	if (lastTicket_.empty() && !sendFeed) {
		return;
	}

	// To be safe, make sure we only send one if we're doing a get.
	if (!sendFeed && pending.size() > 1)
		pending.resize(1);

	for (size_t i = 0; i < pending.size(); ++i) {
		ws->Send(DebuggerGPUStatsEvent{ pending[i], lastTicket_ });
		lastTicket_.clear();
	}
}
