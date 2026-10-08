#include "ppsspp_config.h"

#if PPSSPP_PLATFORM(WINDOWS)

#include "Common/CommonWindows.h"

#define TLS_SUPPORTED

#elif defined(__ANDROID__)

#define TLS_SUPPORTED

#endif

// TODO: Many other platforms also support TLS, in fact probably nearly all that we support
// these days.

#include <cstring>
#include <cstdint>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "Common/Log.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/Data/Encoding/Utf8.h"

#ifdef TLS_SUPPORTED
static thread_local const char *curThreadName;
#endif

#ifdef __MINGW32__
#include <pshpack8.h>
typedef struct {
	DWORD dwType;
	LPCSTR szName;
	DWORD dwThreadID;
	DWORD dwFlags;
} THREADNAME_INFO;
#include <poppack.h>

static EXCEPTION_DISPOSITION NTAPI ignore_handler(EXCEPTION_RECORD *rec,
                                                  void *frame, CONTEXT *ctx,
                                                  void *disp)
{
	return ExceptionContinueExecution;
}
#endif

#if PPSSPP_PLATFORM(WINDOWS) && !PPSSPP_PLATFORM(UWP)
typedef HRESULT (WINAPI *TSetThreadDescription)(HANDLE, PCWSTR);

// Resolved once by whichever thread gets there first; any number may race to do it.
// Holds the function, or the address of g_noSetThreadDescription when it doesn't exist.
static retro_atomic_ptr_t g_setThreadDescription;
static char g_noSetThreadDescription;

static TSetThreadDescription GetSetThreadDescription() {
	void *fn = retro_atomic_load_acquire_ptr(&g_setThreadDescription);
	if (!fn) {
		HMODULE hKernel32 = GetModuleHandle(L"kernelbase.dll");
		// Missing on Windows versions that are too old.
		fn = hKernel32 ? (void *)GetProcAddress(hKernel32, "SetThreadDescription") : nullptr;
		if (!fn)
			fn = &g_noSetThreadDescription;
		retro_atomic_store_release_ptr(&g_setThreadDescription, fn);
	}
	return fn == &g_noSetThreadDescription ? nullptr : (TSetThreadDescription)fn;
}

void SetCurrentThreadNameThroughException(const char *threadName);
#endif

const char *GetCurrentThreadName() {
#ifdef TLS_SUPPORTED
	return curThreadName;
#else
	return "";
#endif
}

void SetCurrentThreadName(const char *threadName) {
#if PPSSPP_PLATFORM(WINDOWS) && !PPSSPP_PLATFORM(UWP)
	TSetThreadDescription setThreadDescription = GetSetThreadDescription();
	if (setThreadDescription) {
		// Use the modern API
		wchar_t buffer[256];
		ConvertUTF8ToWString(buffer, ARRAY_SIZE(buffer), threadName);
		setThreadDescription(GetCurrentThread(), buffer);
	} else {
		// Use the old exception hack.
		SetCurrentThreadNameThroughException(threadName);
	}
#elif PPSSPP_PLATFORM(WINDOWS)
	wchar_t buffer[256];
	ConvertUTF8ToWString(buffer, ARRAY_SIZE(buffer), threadName);
	SetThreadDescription(GetCurrentThread(), buffer);
#else
	sthread_setname(threadName);
#endif

	// Set the locally known threadname using a thread local variable.
#ifdef TLS_SUPPORTED
	curThreadName = threadName;
#endif
}

#if PPSSPP_PLATFORM(WINDOWS)

void SetCurrentThreadNameThroughException(const char *threadName) {
	// Set the debugger-visible threadname through an unholy magic hack
	static const DWORD MS_VC_EXCEPTION = 0x406D1388;

#if defined(__MINGW32__)
	// Thread information for VS compatible debugger. -1 sets current thread.
	THREADNAME_INFO ti;
	ti.dwType = 0x1000;
	ti.szName = threadName;
	ti.dwThreadID = -1;

	// Push an exception handler to ignore all following exceptions
	NT_TIB *tib = ((NT_TIB*)NtCurrentTeb());
	EXCEPTION_REGISTRATION_RECORD rec;
	rec.Next = tib->ExceptionList;
	rec.Handler = ignore_handler;
	tib->ExceptionList = &rec;

	// Visual Studio and compatible debuggers receive thread names from the
	// program through a specially crafted exception
	RaiseException(MS_VC_EXCEPTION, 0, sizeof(ti) / sizeof(ULONG_PTR),
	               (ULONG_PTR*)&ti);

	// Pop exception handler
	tib->ExceptionList = tib->ExceptionList->Next;
#else
#pragma pack(push,8)
	struct THREADNAME_INFO {
		DWORD dwType; // must be 0x1000
		LPCSTR szName; // pointer to name (in user addr space)
		DWORD dwThreadID; // thread ID (-1=caller thread)
		DWORD dwFlags; // reserved for future use, must be zero
	} info;
#pragma pack(pop)

	info.dwType = 0x1000;
	info.szName = threadName;
	info.dwThreadID = -1; //dwThreadID;
	info.dwFlags = 0;

	__try
	{
		RaiseException(MS_VC_EXCEPTION, 0, sizeof(info)/sizeof(ULONG_PTR), (ULONG_PTR*)&info);
	}
	__except(EXCEPTION_CONTINUE_EXECUTION)
	{}
#endif
}
#endif

void AssertCurrentThreadName(const char *threadName) {
#ifdef TLS_SUPPORTED
	if (curThreadName && strcmp(curThreadName, threadName) != 0) {
		ERROR_LOG(Log::System, "Thread name assert failed: Expected %s, was %s", threadName, curThreadName);
		_dbg_assert_msg_(false, "Thread name assert failed: Expected %s, was %s", threadName, curThreadName);
	}
#endif
}

int GetCurrentThreadIdForDebug() {
	return (int)sthread_get_current_thread_id();
}
