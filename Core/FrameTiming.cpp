// Frame timing
//
// The frontend paces frames, so the core never waits: this only picks the present mode.

#include <algorithm>
#include "ppsspp_config.h"
#include "Common/Profiler/Profiler.h"
#include "Common/Log.h"
#include "Common/System/Display.h"
#include "Common/TimeUtil.h"

#include "Core/RetroAchievements.h"
#include "Core/CoreParameter.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "Core/Config.h"
#include "Core/HW/Display.h"
#include "Core/HLE/sceNet.h"
#include "Core/FrameTiming.h"

FrameTiming g_frameTiming;

void FrameTiming::ComputePresentMode(Draw::DrawContext *draw, bool fastForward) {
	if (!draw) {
		// This happens in headless mode.
		fastForwardSkipFlip_ = true;
		presentMode_ = Draw::PresentMode::FIFO;
		return;
	}

	_dbg_assert_(draw->GetDeviceCaps().presentModesSupported != (Draw::PresentMode)0);

	if (draw->GetDeviceCaps().presentModesSupported == Draw::PresentMode::FIFO) {
		// Only FIFO mode is supported (like on iOS and some GLES backends).
		fastForwardSkipFlip_ = true;
		presentMode_ = Draw::PresentMode::FIFO;
		return;
	}

	// More than one present mode is supported. Use careful logic.

	// The user has requested vsync off.
	if (!g_Config.bVSync) {
		if (draw->GetDeviceCaps().presentModesSupported & Draw::PresentMode::IMMEDIATE) {
			// Use immediate mode, whether fast-forwarding or not.
			presentMode_ = Draw::PresentMode::IMMEDIATE;
			fastForwardSkipFlip_ = false;
			return;
		}
		// Inconsistent state - vsync is off but immediate mode is not supported.
		// We will simply force on VSync.
		g_Config.bVSync = true;
	}

	// At this point, vsync is always on. What decides now is whether MAILBOX or IMMEDIATE is available,
	// and also if we need an unsynced mode.

	// OK, vsync is requested (or immediate is not available). If mailbox is supported, it works the same as IMMEDIATE above.

	if (g_Config.bLowLatencyPresent) {
		// Use mailbox if available. It works fine for both fast-forward and normal.
		if (draw->GetDeviceCaps().presentModesSupported & Draw::PresentMode::MAILBOX) {
			presentMode_ = Draw::PresentMode::MAILBOX;
			fastForwardSkipFlip_ = false;
			return;
		}
		// We could force off lowLatencyPresent here, but it's no good when changing between backends.
		// So let's leave it on in the background, maybe the user just went from Vulkan to OpenGL.
	}

	// At this point, low-latency mode is not available, and vsync is on. We see if we can use INSTANT
	// mode for fast-forwarding, or if we need to resort to frameskipping.
	if (draw->GetDeviceCaps().presentInstantModeChange) {
		if (fastForward) {
			presentMode_ = Draw::PresentMode::IMMEDIATE;
			fastForwardSkipFlip_ = false;
			return;
		}
	}

	// Finally, fallback to FIFO mode, with skip-flip in fast-forward.
	presentMode_ = Draw::PresentMode::FIFO;
	fastForwardSkipFlip_ = true;
}
