#pragma once

#include <cstdint>
#include <functional>
#include <utility>

struct sthread;

// A joinable thread on libretro-common's rthreads, in place of std::thread.
// Like std::thread, a thread still joinable when destroyed or assigned over is a bug.
class Thread {
public:
	Thread() {}
	explicit Thread(std::function<void()> func);
	template <class F, class A, class... Args>
	Thread(F &&f, A &&a, Args &&...args)
		: Thread(std::function<void()>(std::bind(std::forward<F>(f), std::forward<A>(a), std::forward<Args>(args)...))) {}
	~Thread();

	Thread(const Thread &) = delete;
	Thread &operator=(const Thread &) = delete;
	Thread(Thread &&other) noexcept : thread_(other.thread_) { other.thread_ = nullptr; }
	Thread &operator=(Thread &&other) noexcept;

	bool joinable() const { return thread_ != nullptr; }
	void join();
	void detach();

private:
	struct sthread *thread_ = nullptr;
};

// An id for the calling thread, comparable with what another thread got from it.
uintptr_t CurrentThreadId();
