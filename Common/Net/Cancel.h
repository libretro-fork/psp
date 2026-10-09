#pragma once

#include <cstdint>

#include <retro_atomic.h>

namespace net {

// A socket that another thread can make readable, to wake a select() that
// includes it. It is a loopback UDP socket connected to itself. If one can't
// be made, Fd() is -1 and waits requesting that wake channel return FAILED.
class WakeSocket {
public:
	WakeSocket();
	~WakeSocket();
	WakeSocket(const WakeSocket &) = delete;
	WakeSocket &operator=(const WakeSocket &) = delete;

	// Any thread. Wakes stack up into one readable state until drained.
	void Wake();
	// Called by the waiting thread after it wakes, before it looks at its state.
	void Drain();
	intptr_t Fd() const { return fd_; }

private:
	intptr_t fd_ = -1;
};

// A cancel flag that socket waits block on. Cancel() sets the flag and
// wakes its socket for good, so every wait that includes it returns at once.
class CancelToken {
public:
	// Any thread.
	void Cancel();
	bool IsCancelled() const { return retro_atomic_load_acquire_int(const_cast<retro_atomic_int_t *>(&cancelled_)) != 0; }
	intptr_t WakeFd() const { return wake_.Fd(); }

private:
	retro_atomic_int_t cancelled_{ 0 };
	WakeSocket wake_;
};

enum class WaitResult {
	READY,
	TIMEOUT,
	CANCELLED,  // or, for the WakeSocket form, woken
	FAILED,
};

// Blocks until sock is readable (or writable), cancel is set, or timeout
// seconds pass. A negative timeout waits for the first two only.
WaitResult WaitSocket(uintptr_t sock, bool forWrite, double timeout, const CancelToken *cancel);

// The same over several sockets. ready[i] says which ones are ready on READY.
WaitResult WaitSockets(const uintptr_t *socks, bool *ready, int count, bool forWrite, double timeout, const CancelToken *cancel);

// For event loops: also returns CANCELLED when wake is woken. Any of socks
// may be -1 (skipped); count may be 0, to wait only for a wake or the timeout.
WaitResult WaitSocketsOrWake(const uintptr_t *socks, bool *ready, int count, bool forWrite, double timeout, WakeSocket *wake);

}  // namespace net
