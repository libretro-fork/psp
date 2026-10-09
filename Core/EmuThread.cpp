#include "ppsspp_config.h"

#include <functional>
#include <retro_atomic.h>

#include "Common/System/System.h"
#include "Common/System/Request.h"
#include "Common/System/Application.h"
#include "Common/Data/Text/I18n.h"
#include "Common/Input/InputState.h"
#include "Common/Data/Encoding/Utf8.h"
#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Common/GPU/GraphicsContext.h"
#include "Common/Thread/Thread.h"
#include "Common/Thread/ThreadUtil.h"

#include "Core/EmuThread.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"

enum class EmuThreadState {
	RUNNING,
	QUIT_REQUESTED,
	STOPPED,
};

static retro_atomic_int_t g_emuThreadState{ (int)EmuThreadState::STOPPED };

static EmuThreadState GetEmuThreadState() {
	return (EmuThreadState)retro_atomic_load_acquire_int(&g_emuThreadState);
}

static void SetEmuThreadState(EmuThreadState state) {
	retro_atomic_store_release_int(&g_emuThreadState, (int)state);
}

static void EmuThreadFunc(GraphicsContext *graphicsContext, Application *application, std::function<bool (GraphicsContext *)> frame) {
	INFO_LOG(Log::G3D, "Entering separate emu thread");
	SetCurrentThreadName("EmuThread");

	SetEmuThreadState(EmuThreadState::RUNNING);


	// This normally calls NativeInitGraphics()
	if (!application->InitGraphics(graphicsContext)) {
		_assert_msg_(false, "NativeInitGraphics failed, might as well bail");
		// If this fails, which it normally shouldn't, let's bail.
		SetEmuThreadState(EmuThreadState::QUIT_REQUESTED);
	} else {
		INFO_LOG(Log::G3D, "EmuThread: Entering loop");
	}

	while (GetEmuThreadState() != EmuThreadState::QUIT_REQUESTED) {
		// We're here again, so the game quit.  Restart Run() which controls the UI.
		// This way they can load a new game.
		// This normally calls NativeFrame()
		if (!frame(graphicsContext)) {
			SetEmuThreadState(EmuThreadState::QUIT_REQUESTED);
		}
	}

	INFO_LOG(Log::System, "emuThreadState was set to QUIT_REQUESTED, left EmuThreadFunc loop. Setting state to STOPPED.");

	// This normally calls NativeShutdownGraphics()
	application->ShutdownGraphics(graphicsContext);
	delete application;

	INFO_LOG(Log::System, "Leaving separate emu thread");

	SetEmuThreadState(EmuThreadState::STOPPED);
}

static Thread EmuThread_Start(GraphicsContext *graphicsContext, Application *application, std::function<bool(GraphicsContext *)> frame) {
	INFO_LOG(Log::System, "EmuTread_Start");
	_dbg_assert_(GetEmuThreadState() == EmuThreadState::STOPPED);
	Thread emuThread(&EmuThreadFunc, graphicsContext, application, frame);
	graphicsContext->ThreadStart();
	return emuThread;
}

// This is useful when the render thread is in control.
static void EmuThread_RequestExit() {
	INFO_LOG(Log::System, "EmuTread_RequestExit");
	if (GetEmuThreadState() == EmuThreadState::RUNNING) {
		SetEmuThreadState(EmuThreadState::QUIT_REQUESTED);
	} else {
		INFO_LOG(Log::System, "EmuTread_RequestExit: g_emuThreadState was not RUNNING, so not requesting exit.");
	}
}

static void EmuThread_Join(GraphicsContext *graphicsContext, Thread &emuThread) {
	INFO_LOG(Log::System, "EmuTread_Join");
	if (graphicsContext->NeedsSeparateEmuThread()) {
		EmuThread_RequestExit();
		while (graphicsContext->ThreadFrame()) {}
	}
	emuThread.join();
	graphicsContext->ThreadEnd();
}

static bool RunMainLoop(GraphicsContext *graphicsContext, Application *application, std::function<bool(GraphicsContext *)> frame) {
	// This is the main loop for graphics context that handle their own threading.
	// InitFromRenderThread/ShutdownFromRenderThread are not used.

	application->InitGraphics(graphicsContext);

	while (frame(graphicsContext)) {}

	// NOTE: Don't call stuff like Core_Stop here. On Android, we fully shut down graphics when you switch away from the app,
	// then boot it up again when returning. That means stopping this thread and restarting it.

	// Process the shutdown.  Without this, non-GL delays 800ms on shutdown. TODO: is this still an issue?
	Core_StateProcessed();

	application->ShutdownGraphics(graphicsContext);
	delete application;
	return true;
}

// Call InitAPI and ShutdownAPI outside this!
bool MainThreadFunc(GraphicsContext *graphicsContext, Application *application, const WindowDesc &windowDesc, std::function<bool(GraphicsContext *)> frame, std::string *errorMessage) {
	// This is now the render thread, and will spawn the emu thread below.
	if (!graphicsContext->InitSurface(windowDesc.winsys, windowDesc.data1, windowDesc.data2, errorMessage)) {
		ERROR_LOG(Log::G3D, "MainThreadFunc: InitSurface failed: %s", errorMessage->c_str());
		delete application;
		return false;
	}
	if (graphicsContext->NeedsSeparateEmuThread()) {
		SetCurrentThreadName("RenderThread");

		Thread emuThread = EmuThread_Start(graphicsContext, application, frame);
		graphicsContext->ThreadStart();
		// This thread becomes the render thread. EmuThread will tell it when to quit by sending a message.
		while (graphicsContext->ThreadFrame()) {}
		EmuThread_Join(graphicsContext, emuThread);
			graphicsContext->ThreadEnd();

		INFO_LOG(Log::System, "RenderThread - joined");

	} else {
		SetCurrentThreadName("MainThread");

		RunMainLoop(graphicsContext, application, frame);
	}
	graphicsContext->ShutdownSurface();
	return true;
}
