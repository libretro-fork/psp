// Copyright (c) 2012- PPSSPP Project.

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

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include <retro_atomic.h>

#include "Common/CommonTypes.h"
#include "Core/ConfigValues.h"

class GraphicsContext;

// For platforms that don't call Run
void Core_SetGraphicsContext(GraphicsContext *ctx);

// Returns false when an UI exit state is detected.
void Core_Stop();

// X11, sigh.
#ifdef None
#undef None
#endif

enum class CPUStepType {
	None,
	Into,
	Over,
	Out,
	Frame,
};

// Must be set when breaking.
enum class BreakReason {
	None,
	AssertChoice,
	DebugBreak,
	DebugStep,
	DebugStepInto,
	UIFocus,
	AfterFrame,
	MemoryException,
	CpuException,
	BreakInstruction,
	SavestateLoad,
	SavestateSave,
	SavestateRewind,
	SavestateCrash,
	MemoryBreakpoint,
	CpuBreakpoint,
	RegBreakpoint,
	MemoryAccess,  // ???
	JitBranchDebug,
	BreakOnBoot,
	RABreak,
	AddBreakpoint,
	FrameAdvance,
	UIPause,
	HLEDebugBreak,
	RunUntilTime,
};
const char *BreakReasonToString(BreakReason reason);

enum class BreakpointKind {
	None,
	Exec,
	Memory,
	Register,
};

// What tripped a breakpoint, captured where it happened.
//
// All three kinds know a lot more than the single address Core_Break() carries - which register,
// which byte of a watched range, read or write, how big - and until this existed it was formatted
// straight into a log line and discarded, so a debugger over the wire couldn't see any of it. For
// a memcheck in particular the address that reached the client was the start of the watched range,
// not the address actually touched.
struct BreakpointHit {
	BreakpointKind kind = BreakpointKind::None;
	u32 pc = 0;            // The instruction responsible.
	u32 address = 0;       // Exec: the instruction itself. Memory: the address actually accessed.
	int size = 0;          // Memory only, in bytes.
	bool write = false;    // Memory only.
	int reg = -1;          // Register only: a GPR index.
	// Which breakpoint this was, so a client can match it against cpu.breakpoint.list and friends.
	// For a memcheck that's the watched range, which is exactly what 'address' is not.
	u32 rangeStart = 0;
	u32 rangeEnd = 0;
	u32 numHits = 0;
	bool logged = false;   // Had the LOG action.
	bool paused = false;   // Had the PAUSE action, so the CPU stopped for it.
	std::string condition; // Empty when unconditional.
	// Memory only: who performed the access - "interpret", "CPU", "HLE", or an allocation tag.
	// Copied rather than kept as a pointer; callers pass buffers that are gone by the time the
	// event gets formatted.
	std::string source;
};

// The stepping functions below are CPU thread only; other threads use Core_RunOnCPUThread().
// hit is optional detail for the breakpoint kinds, forwarded to the debugger. Only stored when
// the break actually takes effect, so a rejected Core_Break() can't leave a stale one behind.
void Core_Break(BreakReason reason, u32 relatedAddress = 0, const BreakpointHit *hit = nullptr);

// Resumes execution. Works both when stepping the CPU and the GE.
void Core_Resume();

BreakReason Core_BreakReason();

// Can fail if too many steps are queued already.
// stepSize is always in instructions (4 bytes each), never bytes - see Core_PerformCPUStep in Core.cpp.
bool Core_RequestCPUStep(CPUStepType stepType);

bool Core_NextFrame();
void Core_SwitchToGe();  // Switches from CPU emulation to GE display list execution.

// Changes every time we enter stepping.
int Core_GetSteppingCounter();
struct SteppingReason {
	BreakReason reason;
	u32 relatedAddress = 0;
	// Only filled in when the break came from a breakpoint - kind is None otherwise.
	BreakpointHit hit;
};
SteppingReason Core_GetSteppingReason();

enum class CoreLifecycle {
	STARTING,
	// Note: includes failure cases.  Guaranteed call after STARTING.
	START_COMPLETE,
	STOPPING,
	// Guaranteed call after STOPPING.
	STOPPED,

	// Sometimes called for save states.  Guaranteed sequence, and never during STARTING or STOPPING.
	MEMORY_REINITING,
	MEMORY_REINITED,
};

// RUNNING must be at 0, NEXTFRAME must be at 1.
enum CoreState {
	// Emulation is running normally.
	CORE_RUNNING_CPU = 0,
	// Emulation was running normally, just reached the end of a frame.
	CORE_NEXTFRAME,
	// Set this when running to bounce out from the dispatcher and just go back in again. Useful for things like cache clears.
	CORE_REENTER_DISPATCH,
	// Emulation is paused, CPU thread is sleeping.
	CORE_STEPPING_CPU,  // Can be used for recoverable runtime errors (ignored memory exceptions)
	// Core is not running.
	CORE_POWERDOWN,
	// Unrecoverable runtime error. Recoverable errors should use CORE_STEPPING.
	CORE_RUNTIME_ERROR,
	// Stepping the GPU. When done, will switch over to STEPPING_CPU.
	CORE_STEPPING_GE,
	// Running the GPU. When done, will switch over to RUNNING_CPU.
	CORE_RUNNING_GE,
};
const char *CoreStateToString(CoreState state);

// Callback is called on the Emu thread.
typedef void (* CoreLifecycleFunc)(CoreLifecycle stage);
void Core_ListenLifecycle(CoreLifecycleFunc func);
void Core_NotifyLifecycle(CoreLifecycle stage);

bool Core_IsStepping();

bool Core_IsActive();
bool Core_IsInactive();

// Warning: these are only used on Windows - debugger integration.
void Core_StateProcessed();

void Core_SetPowerSaving(bool mode);
bool Core_GetPowerSaving();

void Core_RunLoopUntil(u64 globalticks);
void Core_ReenterDispatcher();  // If you've done things that mess with caches, call this so we can run deferred operations.

// Runs a function on the CPU thread - the thread that calls Core_RunLoopUntil (and thus, indirectly,
// NativeFrame). Useful for code running on unrelated threads (like the WebSocket debugger) that needs to
// safely touch state that's otherwise only ever touched from that thread (breakpoints, stepping, etc.),
// instead of poking at it directly from wherever the call happens to come from.
//
// Safe to call from any thread, including the CPU thread itself (in which case func just runs immediately).
// Blocks the calling thread until func has actually run, so don't call this from the CPU thread with
// something that would itself try to wait on the CPU thread - that'll deadlock.
//
// Drained at the top of every Core_RunLoopUntil() iteration, so at least once per call (about once
// per host frame), and by Core_WaitForCPUWork() while the CPU thread has nothing else to do.
void Core_RunOnCPUThread(std::function<void()> func);

// Drains the queue Core_RunOnCPUThread() feeds. Called from the top of every Core_RunLoopUntil()
// iteration, on the CPU thread only.
void Core_ProcessCPUQueue();

// For a CPU thread with nothing to do (stopped in the debugger, or waiting for another thread to
// finish): read the counter, check your condition, then wait. Returns once something was queued
// for the CPU thread or Core_WakeCPUThread() was called since. The caller then drains the queue.
int Core_CPUWorkSeen();
void Core_WaitForCPUWork(int seen);
// Any thread. Wakes a Core_WaitForCPUWork() so it rechecks its condition.
void Core_WakeCPUThread();


// Written on the CPU thread, read anywhere. The jit reads it in place, so the value has to be the
// first (and only) member.
struct SharedCoreState {
	retro_atomic_int_t value;

	operator CoreState() const {
		return (CoreState)retro_atomic_load_acquire_int(const_cast<retro_atomic_int_t *>(&value));
	}
	SharedCoreState &operator=(CoreState state) {
		retro_atomic_store_release_int(&value, (int)state);
		return *this;
	}
};

static_assert(sizeof(SharedCoreState) == 4, "The jit accesses coreState as a 32-bit int");

extern SharedCoreState coreState;
// Set by Core_UpdateState until the CPU loop picks the change up.
extern retro_atomic_int_t coreStatePending;

void Core_UpdateState(CoreState newState);

enum class MemoryExceptionType {
	NONE,
	UNKNOWN,
	READ_WORD,
	WRITE_WORD,
	HLE_READ,
	HLE_WRITE,
	READ_BLOCK,
	WRITE_BLOCK,
	ALIGNMENT,
};
enum class ExecExceptionType {
	JUMP,
	THREAD,
	PERM,  // trying to execute kernel space instructions in user space
	ILLEGAL,
};
// The IEEE 754 exceptions the FPU can raise. Only the ones we actually detect are listed;
// they all live in fcr31 in the standard MIPS bit positions, see FCR31_* in MIPS.h.
enum class FPUExceptionType {
	DIVIDE_BY_ZERO,
};

void Core_MemoryException(u32 address, u32 accessSize, u32 pc, MemoryExceptionType type, std::string_view additionalInfo = "");
void Core_ExecException(u32 address, u32 pc, ExecExceptionType type);
void Core_BreakException(u32 pc);
// Only called for exceptions the game has unmasked in fcr31 - a masked one just sets the flag bit
// and produces the IEEE default result, without coming through here.
void Core_FPUException(u32 pc, FPUExceptionType type);
// Call when loading save states, etc.
void Core_ResetException();

// Used by headless/pspautotest to collect data for the diffs. Crash reports are also sent here.
// Log level is only used if the listener is not registered.
enum class LogLevel : int;

enum GEBufferFormat : uint8_t;
struct DebugScreenshotDesc {
	const uint8_t *data;
	u32 stride;
	u32 height;
	GEBufferFormat format;
};
// Which of the host's output streams a piece of emulated output belongs on.
enum class DebugOutputChannel {
	Debug,   // The "emulator:" devctl channel, plus our own messages about the run. This is what pspautotests uses.
	StdOut,  // sceIoWrite() to fd 1, and to a tty device.
	StdErr,  // sceIoWrite() to fd 2.
};

void Core_SendDebugOutput(LogLevel level, std::string_view string);
// Offers raw output from the emulated program to the listener, if any. Returns false if there was
// none, in which case the caller should log it instead - that's what the normal app does.
bool Core_SendHostOutput(DebugOutputChannel channel, std::string_view string);
void Core_SendDebugScreenshot(const DebugScreenshotDesc &desc);
void Core_RegisterDebugOutputListeners(std::function<void(DebugOutputChannel, std::string_view)> listener, std::function<void(const DebugScreenshotDesc &)> screenshotListener);

class MIPSState;
// Shortcut, just calls Core_MemoryException with automatically determined parameters (function name, etc).
void Core_MemoryExceptionHLE(MIPSState *mips, u32 address, u32 accessSize, MemoryExceptionType type);

enum class MIPSExceptionType {
	NONE,
	MEMORY,
	BREAK,
	BAD_EXEC_ADDR,
	FPU,
};

struct MIPSExceptionInfo {
	MIPSExceptionType type;
	std::string info;
	std::string stackTrace;  // if available.

	// Memory exception info
	MemoryExceptionType memory_type;
	uint32_t pc;
	uint32_t address;
	uint32_t accessSize;
	uint32_t ra = 0;

	// Reuses pc and address from memory type, where address is the failed destination.
	ExecExceptionType exec_type;

	// FPU exception info. Only pc is meaningful alongside it.
	FPUExceptionType fpu_type;
};

const MIPSExceptionInfo &Core_GetExceptionInfo();

const char *ExceptionTypeAsString(MIPSExceptionType type);
const char *MemoryExceptionTypeAsString(MemoryExceptionType type);
const char *ExecExceptionTypeAsString(ExecExceptionType type);
const char *FPUExceptionTypeAsString(FPUExceptionType type);
