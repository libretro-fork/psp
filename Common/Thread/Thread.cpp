#include <functional>
#include <rthreads/rthreads.h>

#include "Common/Log.h"
#include "Common/Thread/Thread.h"

static void ThreadEntry(void *arg) {
	std::function<void()> *func = (std::function<void()> *)arg;
	(*func)();
	delete func;
}

Thread::Thread(std::function<void()> func) {
	std::function<void()> *heapFunc = new std::function<void()>(std::move(func));
	thread_ = sthread_create(&ThreadEntry, heapFunc);
	_assert_msg_(thread_, "Failed to start a thread");
}

Thread::~Thread() {
	_assert_msg_(!thread_, "Thread destroyed while still joinable");
}

Thread &Thread::operator=(Thread &&other) noexcept {
	_assert_msg_(!thread_, "Thread assigned over while still joinable");
	thread_ = other.thread_;
	other.thread_ = nullptr;
	return *this;
}

void Thread::join() {
	_assert_(thread_);
	sthread_join(thread_);
	thread_ = nullptr;
}

void Thread::detach() {
	_assert_(thread_);
	sthread_detach(thread_);
	thread_ = nullptr;
}

uintptr_t CurrentThreadId() {
	return sthread_get_current_thread_id();
}
