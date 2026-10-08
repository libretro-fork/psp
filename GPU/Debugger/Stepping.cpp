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

#include <retro_atomic.h>

#include "Common/Log.h"
#include "Common/Thread/MpscQueue.h"
#include "Common/Thread/ParkingLot.h"
#include "Common/Thread/ThreadUtil.h"
#include "Core/Core.h"
#include "Core/HW/Display.h"
#include "GPU/GPUCommon.h"
#include "GPU/Debugger/Stepping.h"
#include "GPU/GPUState.h"

namespace GPUStepping {

enum PauseAction {
	PAUSE_CONTINUE,
	PAUSE_BREAK,
	PAUSE_GETOUTPUTBUF,
	PAUSE_GETFRAMEBUF,
	PAUSE_GETDEPTHBUF,
	PAUSE_GETSTENCILBUF,
	PAUSE_GETTEX,
	PAUSE_GETCLUT,
	PAUSE_SETCMDVALUE,
	PAUSE_FLUSHDRAW,
};

enum RequestState {
	REQUEST_PENDING,
	REQUEST_RUNNING,
	REQUEST_DONE,
	REQUEST_WITHDRAWN,
};

// One debugger request for the emu thread. Shared by the requester and the queue, freed by the last.
struct PauseRequest {
	PauseAction action;
	GPUDebugFramebufferType bufferType = GPU_DBG_FRAMEBUF_RENDER;
	int bufferLevel = 0;
	u32 setCmdValue = 0;
	int epoch = 0;
	// Written by the emu thread before REQUEST_DONE.
	bool result = false;
	bool wasFramebuffer = false;
	retro_atomic_int_t state{ REQUEST_PENDING };
	retro_atomic_int_t refs{ 1 };
};

static retro_atomic_int_t isStepping{ 0 };
// Number of times we've entered stepping, to detect a resume asynchronously.
static retro_atomic_int_t stepCounter{ 0 };
// Set to resume running the GE (PAUSE_CONTINUE), cleared on entering stepping (PAUSE_BREAK).
static retro_atomic_int_t continueRequested{ 1 };
// Bumped on resume and reset: requests from before are withdrawn rather than run at some later break.
static retro_atomic_int_t requestEpoch{ 0 };
// Requests posted by debugger threads, drained on the emu thread.
static MpscQueue<PauseRequest *> requests;
// Threads waiting in RequestPauseAction(); also the address they park on.
static retro_atomic_int_t requestsWaiting{ 0 };

// Many things need to run on the GPU thread.  For example, reading the framebuffer.
// A message system is used to achieve this (temporarily "unpausing" the thread.)
// Below are the results of actions, written on the emu thread.

static GPUDebugBuffer bufferFrame;
static GPUDebugBuffer bufferDepth;
static GPUDebugBuffer bufferStencil;
static GPUDebugBuffer bufferTex;
static GPUDebugBuffer bufferClut;

// This is used only to highlight differences. Should really be owned by the debugger.
static GEState lastGState;

const char *PauseActionToString(PauseAction action) {
	switch (action) {
	case PAUSE_CONTINUE: return "CONTINUE";
	case PAUSE_BREAK: return "BREAK";
	case PAUSE_GETOUTPUTBUF: return "GETOUTPUTBUF";
	case PAUSE_GETFRAMEBUF: return "GETFRAMEBUF";
	case PAUSE_GETDEPTHBUF: return "GETDEPTHBUF";
	case PAUSE_GETSTENCILBUF: return "GETSTENCILBUF";
	case PAUSE_GETTEX: return "GETTEX";
	case PAUSE_GETCLUT: return "GETCLUT";
	case PAUSE_SETCMDVALUE: return "SETCMDVALUE";
	case PAUSE_FLUSHDRAW: return "FLUSHDRAW";
	default: return "N/A";
	}
}

static void ReleaseRequest(PauseRequest *req) {
	if (retro_atomic_fetch_sub_int(&req->refs, 1) == 1) {
		delete req;
	}
}

static bool CanRunActions() {
	// During CPU stepping, the GE isn't inside a list, so it's safe to run actions then too.
	return coreState == CORE_STEPPING_GE || coreState == CORE_STEPPING_CPU;
}

// Emu thread.
static void RunPauseAction(PauseRequest &req) {
	DEBUG_LOG(Log::GeDebugger, "RunPauseAction: %s", PauseActionToString(req.action));

	switch (req.action) {
	case PAUSE_GETOUTPUTBUF:
		req.result = gpu->GetOutputFramebuffer(bufferFrame);
		break;

	case PAUSE_GETFRAMEBUF:
		req.result = gpu->GetCurrentFramebuffer(bufferFrame, req.bufferType);
		break;

	case PAUSE_GETDEPTHBUF:
		req.result = gpu->GetCurrentDepthbuffer(bufferDepth);
		break;

	case PAUSE_GETSTENCILBUF:
		req.result = gpu->GetCurrentStencilbuffer(bufferStencil);
		break;

	case PAUSE_GETTEX:
		req.result = gpu->GetCurrentTexture(bufferTex, req.bufferLevel, &req.wasFramebuffer);
		break;

	case PAUSE_GETCLUT:
		req.result = gpu->GetCurrentClut(bufferClut);
		break;

	case PAUSE_SETCMDVALUE:
		gpu->SetCmdValue(req.setCmdValue);
		break;

	case PAUSE_FLUSHDRAW:
		gpu->Flush();
		break;

	default:
		ERROR_LOG(Log::GeDebugger, "Unsupported pause action, forgot to add it to the switch.");
		break;
	}
}

// Emu thread. Runs the queued requests, or withdraws them all (or the stale ones).
static void RunPauseRequests(bool withdrawAll) {
	const int epoch = retro_atomic_load_acquire_int(&requestEpoch);
	const bool any = requests.Drain([&](PauseRequest *&&req) {
		if (!withdrawAll && req->epoch == epoch && retro_atomic_cas_int(&req->state, REQUEST_PENDING, REQUEST_RUNNING)) {
			RunPauseAction(*req);
			retro_atomic_store_release_int(&req->state, REQUEST_DONE);
		} else {
			retro_atomic_cas_int(&req->state, REQUEST_PENDING, REQUEST_WITHDRAWN);
		}
		ReleaseRequest(req);
	});
	if (any) {
		ParkingLotNotify(&requestsWaiting);
	}
}

// Requests an action from the emu thread and waits for it to run. Returns false if stepping ended first
// (resume, game shutdown), in which case the action is withdrawn.
static bool RequestPauseAction(PauseRequest *req) {
	_dbg_assert_(strcmp(GetCurrentThreadName(), "EmuThread") != 0);

	req->epoch = retro_atomic_load_acquire_int(&requestEpoch);
	retro_atomic_fetch_add_int(&requestsWaiting, 1);
	retro_atomic_fetch_add_int(&req->refs, 1);
	requests.Push(req);
	// The emu thread may be stopped, waiting for work.
	Core_WakeCPUThread();

	// Leaving stepping wakes us through WakeStaleRequests().
	auto finished = [req] {
		const int state = retro_atomic_load_acquire_int(&req->state);
		return state == REQUEST_DONE || state == REQUEST_WITHDRAWN;
	};
	ParkingLotWait(&requestsWaiting, [&] {
		return finished() || !CanRunActions();
	});
	// Nobody will run it. Don't leave it for a later break to run at some unrelated point.
	if (!retro_atomic_cas_int(&req->state, REQUEST_PENDING, REQUEST_WITHDRAWN)) {
		// Already running: it finishes without us.
		ParkingLotWait(&requestsWaiting, finished);
	}
	retro_atomic_fetch_sub_int(&requestsWaiting, 1);
	return retro_atomic_load_acquire_int(&req->state) == REQUEST_DONE;
}

bool ProcessStepping() {
	_dbg_assert_(gpu);

	if (coreState == CORE_STEPPING_CPU) {
		RunPauseRequests(false);
		return true;
	}
	if (coreState != CORE_STEPPING_GE) {
		// Not stepping any more, don't try.
		return false;
	}

	if (retro_atomic_load_acquire_int(&continueRequested)) {
		// This is fine, can just mean to run to the next breakpoint/event.
		DEBUG_LOG(Log::GeDebugger, "Continuing...");
		coreState = CORE_RUNNING_GE;
		return false;
	}

	RunPauseRequests(false);
	return true;
}

// Emu thread.
bool EnterStepping(CoreState coreState) {
	_dbg_assert_(gpu);

	if (coreState == CORE_STEPPING_GE) {
		// Already there. Should avoid this happening, I think.
		return true;
	}
	if (coreState != CORE_RUNNING_CPU && coreState != CORE_RUNNING_GE) {
		// ?? Shutting down, don't try to step.
		return false;
	}

	// StartStepping
	if (lastGState.cmdmem[1] == 0) {
		lastGState = gstate;
		// Play it safe so we don't keep resetting.
		lastGState.cmdmem[1] |= 0x01000000;
	}

	retro_atomic_store_release_int(&isStepping, 1);
	retro_atomic_fetch_add_int(&stepCounter, 1);

	// Just to be sure.
	retro_atomic_store_release_int(&continueRequested, 0);

	::coreState = CORE_STEPPING_GE;
	return true;
}

void ResumeFromStepping() {
	lastGState = gstate;
	retro_atomic_store_release_int(&isStepping, 0);
	retro_atomic_fetch_add_int(&requestEpoch, 1);
	retro_atomic_store_release_int(&continueRequested, 1);
}

// Emu thread.
void Reset() {
	retro_atomic_store_release_int(&isStepping, 0);
	retro_atomic_store_release_int(&continueRequested, 1);
	retro_atomic_fetch_add_int(&requestEpoch, 1);
	lastGState = {};
	RunPauseRequests(true);
	WakeStaleRequests();
}

void WakeStaleRequests() {
	if (retro_atomic_load_relaxed_int(&requestsWaiting) == 0 || CanRunActions())
		return;
	ParkingLotNotify(&requestsWaiting);
}

bool IsStepping() {
	return retro_atomic_load_acquire_int(&isStepping) != 0;
}

int GetSteppingCounter() {
	return retro_atomic_load_acquire_int(&stepCounter);
}

static bool CanRequest() {
	return IsStepping() || coreState == CORE_STEPPING_CPU;
}

static PauseRequest *NewRequest(PauseAction action) {
	PauseRequest *req = new PauseRequest();
	req->action = action;
	return req;
}

// NOTE: This can't be called on the EmuThread!
static bool GetBuffer(const GPUDebugBuffer *&buffer, PauseRequest *req, const GPUDebugBuffer &resultBuffer, bool *wasFramebuffer = nullptr) {
	bool result = false;
	if (CanRequest() && RequestPauseAction(req)) {
		buffer = &resultBuffer;
		result = req->result;
	}
	if (wasFramebuffer) {
		*wasFramebuffer = req->wasFramebuffer;
	}
	ReleaseRequest(req);
	return result;
}

bool GPU_GetOutputFramebuffer(const GPUDebugBuffer *&buffer) {
	return GetBuffer(buffer, NewRequest(PAUSE_GETOUTPUTBUF), bufferFrame);
}

bool GPU_GetCurrentFramebuffer(const GPUDebugBuffer *&buffer, GPUDebugFramebufferType type) {
	PauseRequest *req = NewRequest(PAUSE_GETFRAMEBUF);
	req->bufferType = type;
	return GetBuffer(buffer, req, bufferFrame);
}

bool GPU_GetCurrentDepthbuffer(const GPUDebugBuffer *&buffer) {
	return GetBuffer(buffer, NewRequest(PAUSE_GETDEPTHBUF), bufferDepth);
}

bool GPU_GetCurrentStencilbuffer(const GPUDebugBuffer *&buffer) {
	return GetBuffer(buffer, NewRequest(PAUSE_GETSTENCILBUF), bufferStencil);
}

bool GPU_GetCurrentTexture(const GPUDebugBuffer *&buffer, int level, bool *isFramebuffer) {
	PauseRequest *req = NewRequest(PAUSE_GETTEX);
	req->bufferLevel = level;
	return GetBuffer(buffer, req, bufferTex, isFramebuffer);
}

bool GPU_GetCurrentClut(const GPUDebugBuffer *&buffer) {
	return GetBuffer(buffer, NewRequest(PAUSE_GETCLUT), bufferClut);
}

static bool RunRequest(PauseRequest *req) {
	bool result = CanRequest() && RequestPauseAction(req);
	ReleaseRequest(req);
	return result;
}

bool GPU_SetCmdValue(u32 op) {
	PauseRequest *req = NewRequest(PAUSE_SETCMDVALUE);
	req->setCmdValue = op;
	return RunRequest(req);
}

bool GPU_FlushDrawing() {
	return RunRequest(NewRequest(PAUSE_FLUSHDRAW));
}

const GEState &LastState() {
	return lastGState;
}

}  // namespace
