#pragma once

#include <atomic>
#include <cstdint>

namespace net {

// A socket that another thread can make readable, to wake a select() that
// includes it. It is a loopback UDP socket connected to itself. If one can't
// be made, Fd() is -1 and waits only see a wake when they return anyway.
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
	bool IsCancelled() const { return cancelled_.load(std::memory_order_acquire); }
	intptr_t WakeFd() const { return wake_.Fd(); }

private:
	std::atomic<bool> cancelled_{ false };
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
