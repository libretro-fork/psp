// Copyright (c) 2014- PPSSPP Project.

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
#include <string_view>

#include <retro_atomic.h>

#include "Common/Net/Cancel.h"
#include "Common/Net/HTTPServer.h"
#include "Common/Net/Sinks.h"
#include "Common/Thread/ParkingLot.h"
#include "Common/Thread/Thread.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/Log.h"
#include "Common/File/Path.h"
#include "Common/File/VFS/VFS.h"
#include "Common/StringUtils.h"
#include "Core/Config.h"
#include "Core/Core.h"
#include "Core/WebServer.h"
#include "Core/Debugger/WebSocket.h"

enum class ServerStatus {
	STOPPED,
	STARTING,
	RUNNING,
	STOPPING,
	FINISHED,
};

// serverThread and serverStop belong to the thread calling Start/ShutdownWebServer.
static Thread serverThread;
// Set by ShutdownWebServer(); wakes the accept loop and every connection. One per server run,
// deleted only after the server thread (and with it every connection thread) is joined.
static net::CancelToken *serverStop;
static retro_atomic_int_t serverStatus{ (int)ServerStatus::STOPPED };
// See WebServerSetRequireExactPort().
static retro_atomic_int_t serverRequireExactPort{ 0 };

static void UpdateStatus(ServerStatus s) {
	retro_atomic_store_release_int(&serverStatus, (int)s);
	ParkingLotNotify(&serverStatus);
	// ShutdownWebServer() may be waiting on the CPU thread's queue for this.
	Core_WakeCPUThread();
}

static ServerStatus RetrieveStatus() {
	return (ServerStatus)retro_atomic_load_acquire_int(&serverStatus);
}

static bool ServeAssetFile(const http::ServerRequest &request) {
	// Skip the slash at the start of the resource path.
	std::string_view filename = request.resource().substr(1);
	if (filename.find("..") != std::string_view::npos) {
		// Don't allow directory traversal.
		return false;
	}

	std::string ext = Path(filename).GetFileExtension();
	std::string_view mimeType = "text/plain";
	if (ext == ".html") {
		mimeType = "text/html";
	} else if (ext == ".ico") {
		mimeType = "image/x-icon";
	} else if (ext == ".js") {
		mimeType = "application/javascript";
	} else if (ext == ".svg") {
		mimeType = "image/svg+xml";
	} else if (ext == ".png") {
		mimeType = "image/png";
	} else if (ext == ".css") {
		mimeType = "text/css";
	}

	size_t size;
	// TODO: ReadFile should take a string_view.
	uint8_t *data = g_VFS.ReadFile(std::string(filename).c_str(), &size);
	if (!data) {
		// Try appending index.html
		data = g_VFS.ReadFile((std::string(filename) + "/index.html").c_str(), &size);
		mimeType = "text/html";
		if (!data) {
			return false;
		}
		INFO_LOG(Log::HTTP, "Redirected to /index.html");
	}

	std::string html = std::string((const char *)data, size);
	delete[] data;

	request.WriteHttpResponseHeader("1.0", 200, html.size(), std::string(mimeType).c_str());
	request.Out()->Push(html.data(), html.size());

	return true;
}

static void RedirectToDebugger(const http::ServerRequest &request) {
	static const std::string payload = "Redirecting to debugger UI...\r\n";
	request.WriteHttpResponseHeader("1.0", 301, payload.size(), "text/plain", "Location: /debugger/index.html\r\n");
	request.Out()->Push(payload);
}

// TODO: Allow registering ServeAssetFile roots as well.
static void HandleFallback(const http::ServerRequest &request) {
	SetCurrentThreadName("HandleFallback");

	if (request.resource() == "/debugger/" || request.resource() == "/") {
		RedirectToDebugger(request);
		return;
	}
	if (startsWith(request.resource(), "/debugger/") && ServeAssetFile(request)) {
		return;
	}

	static const std::string payload = "404 not found\r\n";
	request.WriteHttpResponseHeader("1.0", 404, payload.size(), "text/plain");
	request.Out()->Push(payload);
}

static void ForwardDebuggerRequest(const http::ServerRequest &request) {
	SetCurrentThreadName("ForwardDebuggerRequest");


	// Check if this is a websocket request...
	std::string upgrade;
	if (!request.GetHeader("upgrade", &upgrade)) {
		upgrade.clear();
	}

	// Yes - proceed with the socket.
	if (strcasecmp(upgrade.c_str(), "websocket") == 0) {
		HandleDebuggerRequest(request, serverStop);
	} else {
		RedirectToDebugger(request);
	}
}

static void WebServerThread() {
	SetCurrentThreadName("HTTPServer");

	auto http = new http::Server(new NewThreadExecutor());
	http->SetFallbackHandler(&HandleFallback);
	http->RegisterHandler("/debugger", &ForwardDebuggerRequest);

	if (!http->Listen(g_Config.iRemoteISOPort, "debugger-webserver")) {
		if (retro_atomic_load_acquire_int(&serverRequireExactPort)) {
			// Someone asked for this specific port (--debugger=PORT) - see
			// WebServerSetRequireExactPort(). Coming up on a different one would just look like
			// success while nothing can connect, so give up loudly instead.
			ERROR_LOG(Log::FileSystem, "Unable to listen on port %d, and it was explicitly requested (debugger - webserver). Is another PPSSPP instance already using it?", g_Config.iRemoteISOPort);
			delete http;
			UpdateStatus(ServerStatus::FINISHED);
			return;
		}
		if (!http->Listen(0, "debugger-webserver")) {
			ERROR_LOG(Log::FileSystem, "Unable to listen on any port (debugger - webserver)");
			delete http;
			UpdateStatus(ServerStatus::FINISHED);
			return;
		}
	}

	// Before RUNNING, so WebServerWaitForStartup() callers can read the port.
	g_Config.iRemoteISOPort = http->Port();
	UpdateStatus(ServerStatus::RUNNING);

	// NOTICE rather than INFO on purpose: with --debugger=0 the port is picked for us, and this
	// line is the only way a client can find out which one it got.
	NOTICE_LOG(Log::HTTP, "Entering web server loop. Listening on port %d", g_Config.iRemoteISOPort);

	while (!serverStop->IsCancelled()) {
		http->RunSlice(serverStop);
	}
	INFO_LOG(Log::HTTP, "Leaving web server loop.");

	http->Stop();
	// Joins the connection threads, which see serverStop too.
	delete http;

	UpdateStatus(ServerStatus::FINISHED);
	INFO_LOG(Log::HTTP, "Left web server loop.");
}

bool StartWebServer() {
	switch (RetrieveStatus()) {
	case ServerStatus::FINISHED:
		serverThread.join();
		delete serverStop;
		serverStop = nullptr;
		[[fallthrough]];

	case ServerStatus::STOPPED:
		serverStop = new net::CancelToken();
		UpdateStatus(ServerStatus::STARTING);
		serverThread = Thread(&WebServerThread);
		return true;

	default:
		return false;
	}
}

void ShutdownWebServer() {
	if (retro_atomic_cas_int(&serverStatus, (int)ServerStatus::RUNNING, (int)ServerStatus::STOPPING)) {
		ParkingLotNotify(&serverStatus);
	}
	if (serverStop) {
		serverStop->Cancel();
	}
	if (serverThread.joinable()) {
		// Connections finishing up may still need the CPU thread (Core_RunOnCPUThread), and this
		// is it, so keep serving them until the server thread is done.
		for (;;) {
			const int seen = Core_CPUWorkSeen();
			Core_ProcessCPUQueue();
			if (RetrieveStatus() == ServerStatus::FINISHED || RetrieveStatus() == ServerStatus::STOPPED) {
				break;
			}
			Core_WaitForCPUWork(seen);
		}
		serverThread.join();
	}
	delete serverStop;
	serverStop = nullptr;
	UpdateStatus(ServerStatus::STOPPED);
}

bool WebServerRunning() {
	return RetrieveStatus() == ServerStatus::RUNNING;
}

int WebServerPort() {
	return g_Config.iRemoteISOPort;
}

void WebServerSetRequireExactPort(bool require) {
	retro_atomic_store_release_int(&serverRequireExactPort, require ? 1 : 0);
}

bool WebServerWaitForStartup() {
	ParkingLotWait(&serverStatus, [] { return RetrieveStatus() != ServerStatus::STARTING; });
	return RetrieveStatus() == ServerStatus::RUNNING;
}
