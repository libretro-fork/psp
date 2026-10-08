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

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>

#include "Common/Net/HTTPServer.h"
#include "Common/Net/Sinks.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/Log.h"
#include "Common/File/Path.h"
#include "Common/File/VFS/VFS.h"
#include "Common/StringUtils.h"
#include "Core/Config.h"
#include "Core/WebServer.h"
#include "Core/Debugger/WebSocket.h"

enum class ServerStatus {
	STOPPED,
	STARTING,
	RUNNING,
	STOPPING,
	FINISHED,
};

static std::thread serverThread;
static ServerStatus serverStatus;
static std::mutex serverStatusLock;
static std::condition_variable serverStatusCond;
// See WebServerSetRequireExactPort(). Atomic because it's set from whichever thread parsed the
// command line, and read from the server thread.
static std::atomic<bool> serverRequireExactPort;

static void UpdateStatus(ServerStatus s) {
	{
		std::lock_guard<std::mutex> guard(serverStatusLock);
		serverStatus = s;
	}
	serverStatusCond.notify_all();
}

static ServerStatus RetrieveStatus() {
	std::lock_guard<std::mutex> guard(serverStatusLock);
	return serverStatus;
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
		HandleDebuggerRequest(request);
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
		if (serverRequireExactPort) {
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

	while (RetrieveStatus() == ServerStatus::RUNNING) {
		constexpr double webServerSliceSeconds = 0.2f;
		http->RunSlice(webServerSliceSeconds);
	}
	INFO_LOG(Log::HTTP, "Leaving web server loop.");

	http->Stop();
	StopAllDebuggers();
	delete http;

	UpdateStatus(ServerStatus::FINISHED);
	INFO_LOG(Log::HTTP, "Left web server loop.");
}

bool StartWebServer() {
	std::lock_guard<std::mutex> guard(serverStatusLock);
	switch (serverStatus) {
	case ServerStatus::FINISHED:
		serverThread.join();
		[[fallthrough]];

	case ServerStatus::STOPPED:
		serverStatus = ServerStatus::STARTING;
		serverThread = std::thread(&WebServerThread);
		return true;

	default:
		return false;
	}
}

void ShutdownWebServer() {
	{
		std::lock_guard<std::mutex> guard(serverStatusLock);
		if (serverStatus == ServerStatus::RUNNING)
			serverStatus = ServerStatus::STOPPING;
	}
	if (serverThread.joinable())
		serverThread.join();
	serverStatus = ServerStatus::STOPPED;
}

bool WebServerRunning() {
	return RetrieveStatus() == ServerStatus::RUNNING;
}

int WebServerPort() {
	return g_Config.iRemoteISOPort;
}

void WebServerSetRequireExactPort(bool require) {
	serverRequireExactPort = require;
}

bool WebServerWaitForStartup() {
	std::unique_lock<std::mutex> guard(serverStatusLock);
	serverStatusCond.wait(guard, [] { return serverStatus != ServerStatus::STARTING; });
	return serverStatus == ServerStatus::RUNNING;
}
