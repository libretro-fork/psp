#pragma once

#include <functional>

#include <retro_atomic.h>

#include "Common/Thread/ParkingLot.h"
#include "Common/Thread/ThreadManager.h"

// Same as the latch from C++20.
struct WaitableCounter : public Waitable {
public:
	WaitableCounter(int count) {
		retro_atomic_int_init(&count_, count);
	}

	void Count() {
		for (;;) {
			const int count = retro_atomic_load_acquire_int(&count_);
			if (count == 0)
				return;
			if (retro_atomic_cas_int(&count_, count, count - 1)) {
				if (count == 1) {
					// We were the last one; the waiter may free us as soon as it sees zero.
					ParkingLotNotify(this);
				}
				return;
			}
		}
	}

	void Wait() override {
		ParkingLotWait(this, [this] { return retro_atomic_load_acquire_int(&count_) == 0; });
	}

	retro_atomic_int_t count_;
};

// Note that upper bounds are non-inclusive: range is [lower, upper)
WaitableCounter *ParallelRangeLoopWaitable(ThreadManager *threadMan, const std::function<void(int, int)> &loop, int lower, int upper, int minSize, TaskPriority priority);

// Note that upper bounds are non-inclusive: range is [lower, upper)
void ParallelRangeLoop(ThreadManager *threadMan, const std::function<void(int, int)> &loop, int lower, int upper, int minSize, TaskPriority priority = TaskPriority::NORMAL);

// Common utilities for large (!) memory copies.
// Will only fall back to threads if it seems to make sense.
void ParallelMemcpy(ThreadManager *threadMan, void *dst, const void *src, size_t bytes, TaskPriority priority = TaskPriority::NORMAL);

template<class T>
class SimpleParallelTask : public Task {
public:
	SimpleParallelTask(WaitableCounter *counter, T func, int index, int count, TaskPriority p)
		: counter_(counter), func_(func), index_(index), count_(count), priority_(p) {
	}

	TaskType Type() const override {
		return TaskType::CPU_COMPUTE;
	}

	TaskPriority Priority() const override {
		return priority_;
	}

	void Run() override {
		func_(index_, count_);
		counter_->Count();
	}

	// Cancellable so a Teardown() racing with an in-flight parallel loop still
	// counts down the waiter's counter instead of leaving it blocked forever.
	bool Cancellable() const override {
		return true;
	}

	void Cancel() override {
		counter_->Count();
	}

	T func_;
	WaitableCounter *counter_;

	int index_;
	int count_;
	const TaskPriority priority_;
};

template<class T>
WaitableCounter *RunParallel(ThreadManager *threadMan, T func, int count, TaskPriority priority = TaskPriority::NORMAL) {
	if (count == 1) {
		func(0, 1);
		return nullptr;
	}

	WaitableCounter *counter = new WaitableCounter(count);

	for (int i = 0; i < count; i++) {
		threadMan->EnqueueTaskOnThread(i, new SimpleParallelTask<T>(counter, func, i, count, priority));
	}

	return counter;
}

// To wait for all tasks to finish: if (counter) counter->WaitAndRelease();
