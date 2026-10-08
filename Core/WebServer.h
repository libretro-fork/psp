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

#pragma once

// The headless tools' debugger server: the WebSocket debugger and its web UI, on
// g_Config.iRemoteISOPort.

bool StartWebServer();
void ShutdownWebServer();
bool WebServerRunning();

// By default, if the port is taken we quietly fall back to any free port. That's the wrong
// behavior for --debugger=PORT: an automation client was told to connect to that exact port,
// so landing on a different one leaves it connecting to nothing (or, worse, to some other
// PPSSPP instance that got there first). Set this to make the bind failure fatal instead.
void WebServerSetRequireExactPort(bool require);

// Blocks until the server thread has either started listening or given up, and returns whether
// it's actually listening. Only meaningful right after StartWebServer().
bool WebServerWaitForStartup();

int WebServerPort();
