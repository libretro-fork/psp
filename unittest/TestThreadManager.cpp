#include <vector>
#include <cstdio>

#include "Common/Log.h"
#include "Common/TimeUtil.h"
#include "Common/Thread/Barrier.h"
#include "Common/Thread/Event.h"
#include "Common/Thread/MpscQueue.h"
#include "Common/Thread/ParkingLot.h"
#include "Common/Thread/ThreadManager.h"
#include "Common/Thread/Channel.h"
#include "Common/Thread/Promise.h"
#include "Common/Thread/ParallelLoop.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/Thread/Waitable.h"

#include "UnitTest.h"
#include "Common/Thread/Thread.h"

struct ResultObject {
	bool ok;
};

// Holds the result back until the parallel loop test is done, so BlockUntilReady really waits.
static Event *g_resultGate;

ResultObject *ResultProducer() {
	g_resultGate->Wait();
	printf("result produced: thread %d\n", GetCurrentThreadIdForDebug());
	return new ResultObject{ true };
}

bool TestMailbox() {
	Mailbox<ResultObject *> *mailbox = new Mailbox<ResultObject *>();
	mailbox->Send(new ResultObject{ true });
	ResultObject *data;
	data = mailbox->Wait();
	_assert_(data && data->ok);
	delete data;
	mailbox->Release();
	return true;
}

void rangeFunc(int lower, int upper) {
	printf(" - range %d-%d (thread %d)\n", lower, upper, GetCurrentThreadIdForDebug());
}

// This always passes unless something is badly broken, the interesting thing is the
// logged output.
bool TestParallelLoop(ThreadManager *threadMan) {
	printf("tester thread ID: %d\n", GetCurrentThreadIdForDebug());

	printf("waitable test\n");
	WaitableCounter *waitable = ParallelRangeLoopWaitable(threadMan, rangeFunc, 0, 7, 1, TaskPriority::HIGH);
	// Can do stuff here if we like.
	waitable->WaitAndRelease();
	// Now it's done.

	// Try a loop with stragglers.
	printf("blocking test #1 [0-65)\n");
	ParallelRangeLoop(threadMan, rangeFunc, 0, 65, 1);
	// Try a loop with a relatively large minimum size.
	printf("blocking test #2 [0-100)\n");
	ParallelRangeLoop(threadMan, rangeFunc, 0, 100, 40);
	// Try a loop with minimum size larger than range.
	printf("waitable test [10-30)\n");
	WaitableCounter *waitable2 = ParallelRangeLoopWaitable(threadMan, rangeFunc, 10, 30, 40, TaskPriority::LOW);
	waitable2->WaitAndRelease();
	return true;
}

const size_t THREAD_COUNT = 9;
const size_t ITERATIONS = 40000;

static retro_atomic_int_t g_atomicCounter{ 0 };
static ThreadManager *g_threadMan;
static CountingBarrier g_barrier(THREAD_COUNT + 1);

class IncrementTask : public Task {
public:
	IncrementTask(TaskType type, LimitedWaitable *waitable) : type_(type), waitable_(waitable) {}
	~IncrementTask() {}
	TaskType Type() const override { return type_; }
	TaskPriority Priority() const override {
		return TaskPriority::NORMAL;
	}
	void Run() override {
		retro_atomic_fetch_add_int(&g_atomicCounter, 1);
		waitable_->Notify();
	}
private:
	TaskType type_;
	LimitedWaitable *waitable_;
};

void ThreadFunc() {
	for (int i = 0; i < ITERATIONS; i++) {
		auto threadWaitable = new LimitedWaitable();
		g_threadMan->EnqueueTask(new IncrementTask((i & 1) ? TaskType::CPU_COMPUTE : TaskType::IO_BLOCKING, threadWaitable));
		threadWaitable->WaitAndRelease();
	}
	g_barrier.Arrive();
}

bool TestMultithreadedScheduling() {
	retro_atomic_store_release_int(&g_atomicCounter, 0);

	auto start = Instant::Now();

	std::vector<Thread> threads;
	for (int i = 0; i < THREAD_COUNT; i++) {
		threads.push_back(Thread(ThreadFunc));
	}

	// Just testing the barrier
	g_barrier.Arrive();
	// OK, all are done.

	EXPECT_EQ_INT(retro_atomic_load_acquire_int(&g_atomicCounter), THREAD_COUNT * ITERATIONS);

	for (int i = 0; i < THREAD_COUNT; i++) {
		threads[i].join();
	}

	threads.clear();

	printf("Stress test elapsed: %0.2f", start.ElapsedSeconds());

	return true;
}

struct QueueItem {
	int producer;
	int seq;
};

static const int QUEUE_PRODUCERS = 4;
static const int QUEUE_ITEMS = 20000;

static void QueueProducer(MpscQueue<QueueItem> *queue, EventCounter *pushed, int producer) {
	for (int i = 0; i < QUEUE_ITEMS; i++) {
		queue->Push(QueueItem{ producer, i });
		pushed->Notify();
	}
}

// Several producers against one consumer, twice, so the second round runs on recycled nodes.
// Each producer's items must come out complete and in order.
bool TestMpscQueue() {
	MpscQueue<QueueItem> queue;
	EventCounter pushed;
	for (int round = 0; round < 2; round++) {
		int next[QUEUE_PRODUCERS]{};
		bool ordered = true;
		int received = 0;
		std::vector<Thread> producers;
		for (int i = 0; i < QUEUE_PRODUCERS; i++) {
			producers.push_back(Thread(QueueProducer, &queue, &pushed, i));
		}
		while (received < QUEUE_PRODUCERS * QUEUE_ITEMS) {
			const int seen = pushed.Seen();
			const bool any = queue.Drain([&](QueueItem &&item) {
				if (item.seq != next[item.producer]) {
					ordered = false;
				}
				next[item.producer] = item.seq + 1;
				received++;
			});
			if (!any) {
				pushed.Wait(seen);
			}
		}
		for (auto &t : producers) {
			t.join();
		}
		EXPECT_TRUE(ordered);
		EXPECT_EQ_INT(received, QUEUE_PRODUCERS * QUEUE_ITEMS);
		EXPECT_TRUE(queue.Empty());
	}
	return true;
}

bool TestThreadManager() {
	ThreadManager manager;
	manager.Init(8, 1);

	g_threadMan = &manager;

	Event resultGate;
	g_resultGate = &resultGate;
	Promise<ResultObject *> *object(Promise<ResultObject *>::Spawn(&manager, &ResultProducer, TaskType::IO_BLOCKING));

	const bool loopOk = TestParallelLoop(&manager);
	resultGate.Notify();

	ResultObject *result = object->BlockUntilReady();
	if (result) {
		printf("Got result back!\n");
	}

	delete object;
	if (!loopOk) {
		return false;
	}

	if (!TestMailbox()) {
		return false;
	}

	if (!TestMultithreadedScheduling()) {
		return false;
	}

	if (!TestMpscQueue()) {
		return false;
	}

	return true;
}
