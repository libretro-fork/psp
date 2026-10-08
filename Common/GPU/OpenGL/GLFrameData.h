#pragma once

#include <vector>
#include <string>
#include <set>

#include <retro_atomic.h>

#include "Common/GPU/OpenGL/GLCommon.h"

class GLRShader;
class GLRBuffer;
class GLRTexture;
class GLRInputLayout;
class GLRFramebuffer;
class GLPushBuffer;
class GLRProgram;
class GLRenderManager;

class GLDeleter {
public:
	void Perform(GLRenderManager *renderManager, bool skipGLCalls);

	bool IsEmpty() const {
		return shaders.empty() && programs.empty() && buffers.empty() && textures.empty() && inputLayouts.empty() && framebuffers.empty() && pushBuffers.empty();
	}

	void Take(GLDeleter &other);

	std::vector<GLRShader *> shaders;
	std::vector<GLRProgram *> programs;
	std::vector<GLRBuffer *> buffers;
	std::vector<GLRTexture *> textures;
	std::vector<GLRInputLayout *> inputLayouts;
	std::vector<GLRFramebuffer *> framebuffers;
	std::vector<GLPushBuffer *> pushBuffers;
};

struct GLQueueProfileContext {
	bool enabled;
	double cpuStartTime;
	double cpuEndTime;
	std::string passesString;
	int commandCounts[25];  // Can't grab count from the enum as it would mean a circular include. Might clean this up later.
};


// Per-frame data, round-robin so we can overlap submission with execution of the previous frame.
struct GLFrameData {
	bool skipSwap = false;

	// Frames need unique IDs to wait for present on, let's keep them here.
	// Also used for indexing into the frame timing history buffer.
	uint64_t frameId;

	// Set by the render thread once it has presented this frame, cleared by BeginFrame.
	retro_atomic_int_t readyForFence{ 1 };

	// Swapchain.
	bool hasBegun = false;

	GLDeleter deleter;
	GLDeleter deleter_prev;
	// Render thread only.
	std::set<GLPushBuffer *> activePushBuffers;

	GLQueueProfileContext profile;
};
