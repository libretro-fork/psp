// Copyright (c) 2018- PPSSPP Project.

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

#include <retro_atomic.h>

#include "Common/Log/LogManager.h"
#include "Common/StringUtils.h"
#include "Common/Thread/MpscQueue.h"
#include "Common/TimeUtil.h"
#include "Core/Debugger/WebSocket/LogBroadcaster.h"
#include "Core/Debugger/WebSocket/WebSocketUtils.h"

// Log() runs on whichever thread logged; Take() on the connection's thread.
class DebuggerLogListener {
public:
	explicit DebuggerLogListener(DebuggerMailbox *mailbox) : mailbox_(mailbox) {}

	void Log(const LogMessage &msg) {
		// A source that logs faster than the connection sends - a log-only breakpoint hit thousands
		// of times in a tight loop is a real example, see docs/VSHBootInvestigation.md - would grow
		// the queue without bound. Past the cap new messages are counted and reported as dropped,
		// so it doesn't just look like "my breakpoint only logged a few hits".
		if (retro_atomic_fetch_add_int(&count_, 1) >= BUFFER_SIZE) {
			retro_atomic_fetch_sub_int(&count_, 1);
			retro_atomic_fetch_add_int(&dropped_, 1);
		} else {
			messages_.Push(msg);
		}
		mailbox_->Wake();
	}

	std::vector<LogMessage> Take() {
		std::vector<LogMessage> results;
		const int droppedCount = retro_atomic_exchange_int(&dropped_, 0);
		if (droppedCount > 0) {
			LogMessage dropped;
			dropped.level = LogLevel::LWARNING;
			dropped.log = "Debugger";
			GetCurrentTimeFormatted(dropped.timestamp);
			truncate_cpy(dropped.header, "LogBroadcaster: queue overflow");
			dropped.msg = StringFromFormat("%d log message(s) dropped - connection too slow for this volume\n", droppedCount);
			results.push_back(dropped);
		}
		int n = 0;
		messages_.Drain([&](LogMessage &&msg) {
			results.push_back(std::move(msg));
			n++;
		});
		if (n != 0) {
			retro_atomic_fetch_sub_int(&count_, n);
		}
		return results;
	}

private:
	enum { BUFFER_SIZE = 1024 };
	DebuggerMailbox *mailbox_;
	MpscQueue<LogMessage> messages_;
	retro_atomic_int_t count_{ 0 };
	retro_atomic_int_t dropped_{ 0 };
};

static void BroadcastCallback(const LogMessage &message, void *userdata) {
	DebuggerLogListener *listener = (DebuggerLogListener *)userdata;
	listener->Log(message);
}

LogBroadcaster::LogBroadcaster(DebuggerMailbox *mailbox) {
	listener_ = new DebuggerLogListener(mailbox);
	// One of these exists per open connection, so it registers alongside any other client's
	// rather than replacing it - see AddExternalLogCallback().
	callbackHandle_ = g_logManager.AddExternalLogCallback(&BroadcastCallback, (void *)listener_);
}

LogBroadcaster::~LogBroadcaster() {
	// Returns only once no log call is inside our callback, so the listener is safe to delete.
	g_logManager.RemoveExternalLogCallback(callbackHandle_);
	delete listener_;
}

struct DebuggerLogEvent {
	const LogMessage &l;

	operator std::string() {
		JsonWriter j;
		j.begin();
		j.writeString("event", "log");
		j.writeString("timestamp", l.timestamp);
		j.writeString("header", l.header);
		j.writeString("message", l.msg);
		j.writeInt("level", (int)l.level);
		j.writeString("channel", l.log);
		j.end();
		return j.str();
	}
};

// Log message (log)
//
// Sent unexpectedly with these properties:
//  - timestamp: string timestamp of event.
//  - header: string header information about the event (including file/line.)
//  - message: actual log message as a string.
//  - level: number severity level (1 = highest.)
//  - channel: string describing log channel / grouping.
void LogBroadcaster::Broadcast(net::WebSocketServer *ws) {
	std::vector<LogMessage> messages = listener_->Take();
	for (const LogMessage &msg : messages) {
		ws->Send(DebuggerLogEvent{msg});
	}
}

void LogBroadcaster::Discard() {
	listener_->Take();
}
