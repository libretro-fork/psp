// Copyright (c) 2013- PPSSPP Project.

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

#include <functional>
#include <string>

#include "Common/System/Application.h"

// Runs the headless tools' frame loop, on a separate emu thread when the graphics context needs
// one (this thread then becomes its render thread).

class GraphicsContext;
struct WindowDesc;

// Doesn't take ownership of the graphicsContext, you have to delete it.
// NOTE: Does take ownership over Application (which is just a wrapper for NativeInitGraphics/NativeShutdownGraphics/NativeFrame).
// On failure (which currently only means the graphics surface couldn't be initialized), errorMessage
// gets the reason - pass it on to the user, it's the only place it's available.
bool MainThreadFunc(GraphicsContext * graphicsContext, Application *application, const WindowDesc &windowDesc, std::function<bool(GraphicsContext *)> frame, std::string *errorMessage);
