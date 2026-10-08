#pragma once

#include "Common/GPU/OpenGL/GLCommon.h"
#include "libretro/LibretroGraphicsContext.h"
#include "Common/GPU/OpenGL/GLRenderManager.h"

class LibretroGLContext : public LibretroHWRenderContext {
public:
	LibretroGLContext()
#ifdef USING_GLES2
		: LibretroHWRenderContext(RETRO_HW_CONTEXT_OPENGLES2)
#else
		: LibretroHWRenderContext(RETRO_HW_CONTEXT_OPENGL)
#endif
	{
		hw_render_.bottom_left_origin = true;
	}

   bool NeedsSeparateEmuThread() const override { return true; }

	bool InitAPI(void *wnd, std::string *deviceName, std::string *error_message) override;
	void CreateDrawContext() override;
	void DestroyDrawContext() override;
	// The render thread uses the framebuffer, so it fetches it: on the emu thread when there's no
	// separate one, else at the start of each render thread pass.
	void SetRenderTarget() override {
		if (!Libretro::useEmuThread) {
			UpdateDefaultFBO();
		}
	}

	void ThreadStart() override { renderManager_->ThreadStart(draw_); }
	bool ThreadFrame() override {
		UpdateDefaultFBO();
		return renderManager_->ThreadFrame();
	}
	bool ThreadRunUntilPaused() override {
		UpdateDefaultFBO();
		return renderManager_->ThreadRunUntilPaused();
	}
	void NotifyEmuThreadPaused() override { renderManager_->NotifyEmuThreadPaused(); }
	void ThreadEnd() override { renderManager_->ThreadEnd(); }

	GPUCore GetGPUCore() override { return GPUCORE_GLES; }
	const char *Ident() override { return "OpenGL"; }

   // Call from emu thread
   void NotifyEmuThreadExit() override;

private:
	void UpdateDefaultFBO() {
		extern GLuint g_defaultFBO;
		g_defaultFBO = hw_render_.get_current_framebuffer();
	}

	GLRenderManager *renderManager_ = nullptr;
	bool glewInitDone = false;
};
