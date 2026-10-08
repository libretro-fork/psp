#include <cstdio>
#include <algorithm>
#include <vector>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>

#include "Common/Log.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/Thread/ThreadManager.h"

// Threads and task scheduling
//
// * The threadpool should contain a number of threads that's the the number of cores,
//   plus a fixed number more for I/O-limited background tasks.
// * Parallel compute-limited loops should use as many threads as there are cores.
//   They should always be scheduled to the first N threads.
// * For some tasks, splitting the input values up linearly between the threads
//   is not fair. However, we ignore that for now.
//
// Nothing here takes a lock:
// * Each pool (compute, I/O) has a bounded MPMC ring per priority that any worker of the
//   pool takes from. A full ring spills into an MPSC stack that one worker drains whole.
// * Each worker has an MPSC stack per priority for tasks pinned to it.
// * Idle workers park on their own eventcount. A worker publishes its idle bit before it
//   re-checks for work, and a producer claims an idle bit after publishing, with a
//   seq_cst fence on both sides, so either the producer finds the bit or the worker finds
//   the task.

#if !defined(RETRO_ATOMIC_HAS_PTR)
#error "The thread manager needs pointer atomics."
#endif

const int MIN_IO_BLOCKING_THREADS = 4;
static constexpr int TASK_PRIORITY_COUNT = (int)TaskPriority::COUNT;
static constexpr int RING_SIZE = 1024;
static constexpr int RING_MASK = RING_SIZE - 1;
static constexpr int IDLE_BITS_PER_WORD = 31;

ThreadManager g_threadManager;

static inline Task *TaskFromNode(mpsc_stack_node_t *node) {
	return ((TaskLink *)node)->self;
}

static inline int WrapAdd(int a, int b) {
	return (int)((unsigned)a + (unsigned)b);
}

// Bounded multi-producer multi-consumer ring of tasks (per-cell sequence numbers).
struct TaskRing {
	struct Cell {
		retro_atomic_int_t seq;
		Task *task;
	};

	Cell cells[RING_SIZE];
	alignas(64) retro_atomic_int_t enqueuePos;
	alignas(64) retro_atomic_int_t dequeuePos;

	TaskRing() {
		for (int i = 0; i < RING_SIZE; i++) {
			retro_atomic_int_init(&cells[i].seq, i);
			cells[i].task = nullptr;
		}
		retro_atomic_int_init(&enqueuePos, 0);
		retro_atomic_int_init(&dequeuePos, 0);
	}

	bool Push(Task *task) {
		int pos = retro_atomic_load_relaxed_int(&enqueuePos);
		Cell *cell;
		for (;;) {
			cell = &cells[pos & RING_MASK];
			const int seq = retro_atomic_load_acquire_int(&cell->seq);
			const int diff = (int)((unsigned)seq - (unsigned)pos);
			if (diff == 0) {
				if (retro_atomic_cas_int(&enqueuePos, pos, WrapAdd(pos, 1)))
					break;
				pos = retro_atomic_load_relaxed_int(&enqueuePos);
			} else if (diff < 0) {
				return false;  // full
			} else {
				pos = retro_atomic_load_relaxed_int(&enqueuePos);
			}
		}
		cell->task = task;
		retro_atomic_store_release_int(&cell->seq, WrapAdd(pos, 1));
		return true;
	}

	Task *Pop() {
		int pos = retro_atomic_load_relaxed_int(&dequeuePos);
		Cell *cell;
		for (;;) {
			cell = &cells[pos & RING_MASK];
			const int seq = retro_atomic_load_acquire_int(&cell->seq);
			const int diff = (int)((unsigned)seq - (unsigned)WrapAdd(pos, 1));
			if (diff == 0) {
				if (retro_atomic_cas_int(&dequeuePos, pos, WrapAdd(pos, 1)))
					break;
				pos = retro_atomic_load_relaxed_int(&dequeuePos);
			} else if (diff < 0) {
				return nullptr;  // empty
			} else {
				pos = retro_atomic_load_relaxed_int(&dequeuePos);
			}
		}
		Task *task = cell->task;
		retro_atomic_store_release_int(&cell->seq, WrapAdd(pos, RING_SIZE));
		return task;
	}

	bool HasItems() {
		const int pos = retro_atomic_load_acquire_int(&dequeuePos);
		const int seq = retro_atomic_load_acquire_int(&cells[pos & RING_MASK].seq);
		return seq == WrapAdd(pos, 1);
	}
};

struct TaskPool {
	TaskRing ring[TASK_PRIORITY_COUNT];
	mpsc_stack_t overflow[TASK_PRIORITY_COUNT];
	std::vector<TaskThreadContext *> workers;
	// One bit per worker (31 per word), set while the worker is parked or about to park.
	retro_atomic_int_t *idle = nullptr;
	int idleWords = 0;

	TaskPool() {
		for (int p = 0; p < TASK_PRIORITY_COUNT; p++)
			mpsc_stack_init(&overflow[p]);
	}
	~TaskPool() {
		delete[] idle;
	}
};

struct GlobalThreadContext {
	TaskPool pools[2];  // CPU_COMPUTE, IO_BLOCKING
	std::vector<TaskThreadContext *> threads_;

	retro_atomic_int_t dedicatedLive;
	retro_eventcount_t dedicatedDone;
};

struct LocalFifo {
	mpsc_stack_node_t *head = nullptr;
	mpsc_stack_node_t *tail = nullptr;

	void Append(mpsc_stack_node_t *oldestFirst) {
		if (!oldestFirst)
			return;
		if (tail)
			tail->next = oldestFirst;
		else
			head = oldestFirst;
		mpsc_stack_node_t *last = oldestFirst;
		while (last->next)
			last = last->next;
		tail = last;
	}

	Task *Pop() {
		mpsc_stack_node_t *node = head;
		if (!node)
			return nullptr;
		head = node->next;
		if (!head)
			tail = nullptr;
		node->next = nullptr;
		return TaskFromNode(node);
	}
};

struct TaskThreadContext {
	sthread_t *thread = nullptr;
	GlobalThreadContext *global = nullptr;
	TaskPool *pool = nullptr;
	int index = 0;      // global index
	int poolIndex = 0;  // index within its pool
	TaskType type = TaskType::CPU_COMPUTE;
	char name[16]{};

	mpsc_stack_t inbox[TASK_PRIORITY_COUNT];  // pinned tasks, any producer
	LocalFifo local[TASK_PRIORITY_COUNT];     // worker-private, in order
	retro_eventcount_t wake;
	retro_atomic_int_t cancelled;
};

static TaskPool &PoolFor(GlobalThreadContext *global, TaskType type) {
	return global->pools[type == TaskType::CPU_COMPUTE ? 0 : 1];
}

ThreadManager::ThreadManager() : global_(new GlobalThreadContext()) {
	retro_atomic_int_init(&global_->dedicatedLive, 0);
	retro_eventcount_init(&global_->dedicatedDone);
}

ThreadManager::~ThreadManager() {
	// Make sure the worker threads are stopped and joined before we free global_ -
	// otherwise, if Teardown() was never called (e.g. an early return between Init()
	// and the caller's normal shutdown path), the still-running threads would go on
	// touching global_'s queues after they're freed here.
	if (IsInitialized()) {
		Teardown();
	}
	retro_eventcount_free(&global_->dedicatedDone);
	delete global_;
}

static Task *TakeTask(TaskThreadContext *thread) {
	TaskPool *pool = thread->pool;
	for (int p = 0; p < TASK_PRIORITY_COUNT; ++p) {
		Task *task = thread->local[p].Pop();
		if (task)
			return task;
		if (!mpsc_stack_empty(&thread->inbox[p])) {
			thread->local[p].Append(mpsc_stack_reverse(mpsc_stack_drain(&thread->inbox[p])));
			if ((task = thread->local[p].Pop()) != nullptr)
				return task;
		}
		if ((task = pool->ring[p].Pop()) != nullptr)
			return task;
		if (!mpsc_stack_empty(&pool->overflow[p])) {
			thread->local[p].Append(mpsc_stack_reverse(mpsc_stack_drain(&pool->overflow[p])));
			if ((task = thread->local[p].Pop()) != nullptr)
				return task;
		}
	}
	return nullptr;
}

static bool HasWork(TaskThreadContext *thread) {
	TaskPool *pool = thread->pool;
	for (int p = 0; p < TASK_PRIORITY_COUNT; ++p) {
		if (thread->local[p].head || !mpsc_stack_empty(&thread->inbox[p]) ||
			pool->ring[p].HasItems() || !mpsc_stack_empty(&pool->overflow[p]))
			return true;
	}
	return false;
}

static void WorkerThreadFunc(void *arg) {
	TaskThreadContext *thread = (TaskThreadContext *)arg;
	if (thread->type == TaskType::CPU_COMPUTE) {
		snprintf(thread->name, sizeof(thread->name), "PoolW %d", thread->index);
	} else {
		_assert_(thread->type == TaskType::IO_BLOCKING);
		snprintf(thread->name, sizeof(thread->name), "PoolW IO %d", thread->index);
	}
	SetCurrentThreadName(thread->name);


	TaskPool *pool = thread->pool;
	retro_atomic_int_t *idleWord = &pool->idle[thread->poolIndex / IDLE_BITS_PER_WORD];
	const int idleBit = 1 << (thread->poolIndex % IDLE_BITS_PER_WORD);

	while (!retro_atomic_load_acquire_int(&thread->cancelled)) {
		Task *task = TakeTask(thread);
		if (task) {
			// The task itself takes care of notifying anyone waiting on it.
			task->Run();
			task->Release();
			continue;
		}

		// Nothing to do: publish idleness, then re-check before sleeping.
		retro_atomic_fetch_or_int(idleWord, idleBit);
		retro_atomic_thread_fence_seq_cst();
		const int key = retro_eventcount_prepare_wait(&thread->wake);
		if (HasWork(thread) || retro_atomic_load_acquire_int(&thread->cancelled)) {
			retro_eventcount_cancel_wait(&thread->wake);
		} else {
			retro_eventcount_commit_wait(&thread->wake, key);
		}
		retro_atomic_fetch_and_int(idleWord, ~idleBit);
	}

}

// Wakes one parked worker of the pool, if there is one. Call after publishing the task.
static void WakeOneIdle(TaskPool &pool) {
	retro_atomic_thread_fence_seq_cst();
	for (int w = 0; w < pool.idleWords; w++) {
		int bits = retro_atomic_load_acquire_int(&pool.idle[w]);
		while (bits) {
			const int bit = bits & -bits;
			const int old = retro_atomic_fetch_and_int(&pool.idle[w], ~bit);
			if (old & bit) {
				int worker = w * IDLE_BITS_PER_WORD;
				for (int b = bit; b > 1; b >>= 1)
					worker++;
				retro_eventcount_notify(&pool.workers[worker]->wake);
				return;
			}
			bits = old & ~bit;
		}
	}
}

void ThreadManager::Teardown() {
	for (TaskThreadContext *threadCtx : global_->threads_) {
		retro_atomic_store_release_int(&threadCtx->cancelled, 1);
		retro_eventcount_notify(&threadCtx->wake);
	}

	for (TaskThreadContext *threadCtx : global_->threads_) {
		sthread_join(threadCtx->thread);
		threadCtx->thread = nullptr;
	}

	// No worker picks up anything once cancelled, so whatever is still queued will never
	// run. Cancel and release it so it doesn't leak (and waiters on it are released).
	for (TaskThreadContext *threadCtx : global_->threads_) {
		for (int p = 0; p < TASK_PRIORITY_COUNT; ++p) {
			threadCtx->local[p].Append(mpsc_stack_reverse(mpsc_stack_drain(&threadCtx->inbox[p])));
			while (Task *task = threadCtx->local[p].Pop())
				TeardownTask(task);
		}
	}
	for (TaskPool &pool : global_->pools) {
		for (int p = 0; p < TASK_PRIORITY_COUNT; ++p) {
			while (Task *task = pool.ring[p].Pop())
				TeardownTask(task);
			mpsc_stack_node_t *node = mpsc_stack_reverse(mpsc_stack_drain(&pool.overflow[p]));
			while (node) {
				mpsc_stack_node_t *next = node->next;
				node->next = nullptr;
				TeardownTask(TaskFromNode(node));
				node = next;
			}
		}
		pool.workers.clear();
		delete[] pool.idle;
		pool.idle = nullptr;
		pool.idleWords = 0;
	}

	for (TaskThreadContext *threadCtx : global_->threads_) {
		retro_eventcount_free(&threadCtx->wake);
		delete threadCtx;
	}
	global_->threads_.clear();

	// Dedicated threads are detached; wait until they're all done with their tasks so
	// none of them outlives the core.
	for (;;) {
		if (retro_atomic_load_acquire_int(&global_->dedicatedLive) == 0)
			break;
		const int key = retro_eventcount_prepare_wait(&global_->dedicatedDone);
		if (retro_atomic_load_acquire_int(&global_->dedicatedLive) == 0) {
			retro_eventcount_cancel_wait(&global_->dedicatedDone);
			break;
		}
		retro_eventcount_commit_wait(&global_->dedicatedDone, key);
	}
}

void ThreadManager::TeardownTask(Task *task) {
	if (!task)
		return;

	if (task->Cancellable()) {
		task->Cancel();
	} else {
		WARN_LOG(Log::System, "ThreadManager::Teardown() dropping a non-cancellable task that will never run");
	}
	task->Release();
}

void ThreadManager::Init(int numRealCores, int numLogicalCoresPerCpu) {
	if (IsInitialized()) {
		Teardown();
	}

	numComputeThreads_ = numRealCores * numLogicalCoresPerCpu;
	// Double it for the IO blocking threads.
	int numThreads = numComputeThreads_ + std::max(MIN_IO_BLOCKING_THREADS, numComputeThreads_);
	numThreads_ = numThreads;

	INFO_LOG(Log::System, "ThreadManager::Init(compute threads: %d, all: %d)", numComputeThreads_, numThreads_);

	for (int i = 0; i < numThreads; i++) {
		TaskThreadContext *thread = new TaskThreadContext();
		thread->global = global_;
		thread->type = i < numComputeThreads_ ? TaskType::CPU_COMPUTE : TaskType::IO_BLOCKING;
		thread->index = i;
		thread->pool = &PoolFor(global_, thread->type);
		thread->poolIndex = (int)thread->pool->workers.size();
		for (int p = 0; p < TASK_PRIORITY_COUNT; ++p)
			mpsc_stack_init(&thread->inbox[p]);
		retro_eventcount_init(&thread->wake);
		retro_atomic_int_init(&thread->cancelled, 0);
		thread->pool->workers.push_back(thread);
		global_->threads_.push_back(thread);
	}

	for (TaskPool &pool : global_->pools) {
		pool.idleWords = ((int)pool.workers.size() + IDLE_BITS_PER_WORD - 1) / IDLE_BITS_PER_WORD;
		pool.idle = pool.idleWords ? new retro_atomic_int_t[pool.idleWords] : nullptr;
		for (int w = 0; w < pool.idleWords; w++)
			retro_atomic_int_init(&pool.idle[w], 0);
	}

	// Start the threads only once every context and pool is complete.
	for (TaskThreadContext *thread : global_->threads_) {
		thread->thread = sthread_create(&WorkerThreadFunc, thread);
		_assert_msg_(thread->thread, "ThreadManager: failed to start worker %d", thread->index);
	}
}

struct DedicatedStart {
	GlobalThreadContext *global;
	Task *task;
};

static void DedicatedThreadFunc(void *arg) {
	DedicatedStart *start = (DedicatedStart *)arg;
	GlobalThreadContext *global = start->global;
	Task *task = start->task;
	delete start;

	SetCurrentThreadName("DedicatedThreadTask");
	task->Run();
	task->Release();

	if (retro_atomic_fetch_sub_int(&global->dedicatedLive, 1) == 1)
		retro_eventcount_notify(&global->dedicatedDone);
}

void ThreadManager::EnqueueTask(Task *task) {
	if (task->Type() == TaskType::DEDICATED_THREAD) {
		DedicatedStart *start = new DedicatedStart{ global_, task };
		retro_atomic_fetch_add_int(&global_->dedicatedLive, 1);
		sthread_t *th = sthread_create(&DedicatedThreadFunc, start);
		if (th) {
			sthread_detach(th);
		} else {
			// Couldn't start a thread; run it here rather than lose it.
			DedicatedThreadFunc(start);
		}
		return;
	}

	_assert_msg_(IsInitialized(), "ThreadManager not initialized");

	const int queueIndex = (int)task->Priority();
	TaskPool &pool = PoolFor(global_, task->Type());
	_assert_msg_(!pool.workers.empty(), "ThreadManager: no threads available for task type %d (was Init() called with 0 compute threads?)", (int)task->Type());

	if (!pool.ring[queueIndex].Push(task)) {
		// Ring full - never block the producer, spill into the overflow stack instead.
		mpsc_stack_push(&pool.overflow[queueIndex], &task->link_.node);
	}
	WakeOneIdle(pool);
}

void ThreadManager::EnqueueTaskOnThread(int threadNum, Task *task) {
	_assert_msg_(task->Type() != TaskType::DEDICATED_THREAD, "Dedicated thread tasks can't be put on specific threads");

	_assert_msg_(threadNum >= 0 && threadNum < (int)global_->threads_.size(), "Bad threadnum %d(/%d) or not initialized", threadNum, (int)global_->threads_.size());
	TaskThreadContext *thread = global_->threads_[threadNum];
	mpsc_stack_push(&thread->inbox[(int)task->Priority()], &task->link_.node);
	retro_eventcount_notify(&thread->wake);
}

int ThreadManager::GetNumLooperThreads() const {
	return numComputeThreads_;
}

bool ThreadManager::IsInitialized() const {
	return !global_->threads_.empty();
}
