#pragma once

#include <cstddef>

#include <retro_atomic.h>

#include "Common/Thread/ParkingLot.h"

// Similar to C++20's std::barrier. Reusable: each round bumps the generation.
class CountingBarrier {
public:
	CountingBarrier(size_t count) : threadCount_((int)count) {
		retro_atomic_int_init(&arrived_, 0);
		retro_atomic_int_init(&generation_, 0);
	}

	void Arrive() {
		const int gen = retro_atomic_load_acquire_int(&generation_);
		if (retro_atomic_fetch_add_int(&arrived_, 1) + 1 == threadCount_) {
			// Last one in: reset for the next round, then release everyone.
			retro_atomic_store_relaxed_int(&arrived_, 0);
			retro_atomic_fetch_add_int(&generation_, 1);
			ParkingLotNotify(this);
			return;
		}
		ParkingLotWait(this, [&] { return retro_atomic_load_acquire_int(&generation_) != gen; });
	}

private:
	retro_atomic_int_t arrived_;
	retro_atomic_int_t generation_;
	const int threadCount_;
};
