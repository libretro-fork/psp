#pragma once

#include "Common/GPU/thin3d.h"

// See big comment in the CPP file.

namespace Draw {
class DrawContext;
}

class FrameTiming {
public:
	void ComputePresentMode(Draw::DrawContext *draw, bool fastForward);

	bool FastForwardNeedsSkipFlip() const {
		return fastForwardSkipFlip_;
	}
	Draw::PresentMode PresentMode() const {
		return presentMode_;
	}

private:
	// For use on the next Present. These two are set by ComputePresentMode.
	Draw::PresentMode presentMode_;
	bool fastForwardSkipFlip_;
};

extern FrameTiming g_frameTiming;
