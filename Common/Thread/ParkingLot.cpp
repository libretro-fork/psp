#include "Common/Thread/ParkingLot.h"

static constexpr int PARKING_SHARDS = 64;

namespace {

struct ParkingLotShards {
	retro_eventcount_t ec[PARKING_SHARDS];
	ParkingLotShards() {
		for (int i = 0; i < PARKING_SHARDS; i++)
			retro_eventcount_init(&ec[i]);
	}
	~ParkingLotShards() {
		for (int i = 0; i < PARKING_SHARDS; i++)
			retro_eventcount_free(&ec[i]);
	}
};

// Function-local so it exists before any static constructor that might wait.
ParkingLotShards &Shards() {
	static ParkingLotShards shards;
	return shards;
}

}  // namespace

retro_eventcount_t *ParkingLotFor(const void *addr) {
	uintptr_t h = (uintptr_t)addr;
	h ^= h >> 17;
	h *= (uintptr_t)0x9E3779B97F4A7C15ULL;
	h ^= h >> 29;
	return &Shards().ec[h & (PARKING_SHARDS - 1)];
}
