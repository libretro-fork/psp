#pragma once

#include <retro_atomic.h>

#include "Common/Thread/ParkingLot.h"
#include "Common/Thread/ThreadManager.h"

struct Event : public Waitable {
public:
	Event() {
		retro_atomic_int_init(&triggered_, 0);
	}

	void Wait() override {
		ParkingLotWait(this, [this] { return retro_atomic_load_acquire_int(&triggered_) != 0; });
	}

	void Notify() {
		retro_atomic_store_release_int(&triggered_, 1);
		ParkingLotNotify(this);
	}

private:
	retro_atomic_int_t triggered_;
};
