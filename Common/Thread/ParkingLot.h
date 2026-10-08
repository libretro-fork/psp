#pragma once

#include <cstdint>

#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>

// Parking for one-shot and counter waits, keyed by address.
//
// The eventcounts live for the whole process, sharded by address, so the object being
// waited on never owns one. A notifier only hashes the address after publishing, it never
// dereferences it, so a waiter may free the object the moment its predicate holds.
// Shards are shared, so a wakeup may be for someone else: callers always loop on their
// own predicate (the helpers below do).

retro_eventcount_t *ParkingLotFor(const void *addr);

inline void ParkingLotNotify(const void *addr) {
	retro_eventcount_notify(ParkingLotFor(addr));
}

// Blocks until pred() is true.
template <class Pred>
inline void ParkingLotWait(const void *addr, Pred pred) {
	if (pred())
		return;
	retro_eventcount_t *ec = ParkingLotFor(addr);
	for (;;) {
		const int key = retro_eventcount_prepare_wait(ec);
		if (pred()) {
			retro_eventcount_cancel_wait(ec);
			return;
		}
		retro_eventcount_commit_wait(ec, key);
		if (pred())
			return;
	}
}

// Grace period for read-mostly pointers: readers bracket each use with
// Enter/Exit, a writer unpublishes and then Drain()s before freeing.
// Readers never wait. Drain waits only for readers already inside.
struct ReaderGate {
	ReaderGate() {
		retro_atomic_int_init(&readers_, 0);
		retro_atomic_int_init(&draining_, 0);
	}

	void Enter() {
		retro_atomic_fetch_add_int(&readers_, 1);
		retro_atomic_thread_fence_seq_cst();
	}

	void Exit() {
		if (retro_atomic_fetch_sub_int(&readers_, 1) == 1) {
			retro_atomic_thread_fence_seq_cst();
			if (retro_atomic_load_relaxed_int(&draining_))
				ParkingLotNotify(this);
		}
	}

	// Call after unpublishing. Only one drainer at a time.
	void Drain() {
		retro_atomic_store_relaxed_int(&draining_, 1);
		retro_atomic_thread_fence_seq_cst();
		ParkingLotWait(this, [this] { return retro_atomic_load_acquire_int(&readers_) == 0; });
		retro_atomic_store_relaxed_int(&draining_, 0);
	}

private:
	retro_atomic_int_t readers_;
	retro_atomic_int_t draining_;
};
