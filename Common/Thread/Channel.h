#pragma once

#include <cassert>

#include <retro_atomic.h>

#include "Common/Thread/ParkingLot.h"

// Named Channel.h because I originally intended to support a multi item channel as
// well as a simple blocking mailbox. Let's see if we get there.

// Single item mailbox. Any number of threads may Wait or Poll; the first Send wins.
// T is copyable. Often T will itself just be a pointer or smart pointer of some sort.
template<class T>
struct Mailbox {
	Mailbox() {
		retro_atomic_int_init(&state_, EMPTY);
		retro_atomic_int_init(&cancelled_, 0);
		retro_atomic_int_init(&refcount_, 1);
	}
	~Mailbox() {
		assert(retro_atomic_load_acquire_int(&refcount_) == 0);
	}

	T Wait() {
		ParkingLotWait(this, [this] { return retro_atomic_load_acquire_int(&state_) == FULL; });
		return data_;
	}

	bool Poll(T *data) {
		if (retro_atomic_load_acquire_int(&state_) == FULL) {
			*data = data_;
			return true;
		}
		return false;
	}

	bool Send(T data) {
		// Claim the slot, write it, then publish.
		if (!retro_atomic_cas_int(&state_, EMPTY, WRITING))
			return false;  // Already has value.
		data_ = data;
		retro_atomic_store_release_int(&state_, FULL);
		ParkingLotNotify(this);
		return true;
	}

	// Asks the producer to stop and releases waiters with a default value.
	void CancelAndRelease() {
		retro_atomic_store_release_int(&cancelled_, 1);
		Send(T{});
	}

	bool Cancelled() const {
		return retro_atomic_load_acquire_int(const_cast<retro_atomic_int_t *>(&cancelled_)) != 0;
	}

	void AddRef() {
		retro_atomic_fetch_add_int(&refcount_, 1);
	}

	void Release() {
		if (retro_atomic_fetch_sub_int(&refcount_, 1) == 1) {  // was definitely decreased to 0
			delete this;
		}
	}

private:
	enum : int { EMPTY = 0, WRITING = 1, FULL = 2 };

	T data_{};
	retro_atomic_int_t state_;
	retro_atomic_int_t cancelled_;
	retro_atomic_int_t refcount_;
};
