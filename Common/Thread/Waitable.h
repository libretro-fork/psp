#pragma once

#include <retro_atomic.h>

#include "Common/Thread/ParkingLot.h"
#include "Common/Thread/ThreadManager.h"

class LimitedWaitable : public Waitable {
public:
	LimitedWaitable() {
		retro_atomic_int_init(&triggered_, 0);
	}

	void Wait() override {
		ParkingLotWait(this, [this] { return Ready(); });
	}

	void Notify() {
		retro_atomic_store_release_int(&triggered_, 1);
		ParkingLotNotify(this);
	}

	// For simple polling.
	bool Ready() const {
		return retro_atomic_load_acquire_int(const_cast<retro_atomic_int_t *>(&triggered_)) != 0;
	}

private:
	retro_atomic_int_t triggered_;
};
