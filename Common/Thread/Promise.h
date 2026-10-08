#pragma once

#include <functional>

#include "Common/Log.h"
#include "Common/Thread/Channel.h"
#include "Common/Thread/ThreadManager.h"

// Nobody needs to wait for this (except threadpool shutdown).
template<class T>
class IndependentTask : public Task {
public:
	IndependentTask(TaskType type, TaskPriority prio, T func) : func_(std::move(func)), type_(type), prio_(prio) {}
	TaskType Type() const override { return type_; }
	TaskPriority Priority() const override { return prio_; }
	void Run() override {
		func_();
	}
private:
	T func_;
	TaskType type_;
	TaskPriority prio_;
};

template<class T>
class PromiseTask : public Task {
public:
	PromiseTask(std::function<T ()> fun, Mailbox<T> *tx, TaskType t, TaskPriority p)
		: fun_(fun), tx_(tx), type_(t), priority_(p) {
		tx_->AddRef();
	}
	~PromiseTask() {
		tx_->Release();
	}

	TaskType Type() const override {
		return type_;
	}

	TaskPriority Priority() const override {
		return priority_;
	}

	void Run() override {
		if (tx_->Cancelled()) {
			INFO_LOG(Log::System, "PromiseTask skipped after cancellation");
			return;
		}
		T value = fun_();
		if (!tx_->Send(value)) {
			INFO_LOG(Log::System, "PromiseTask ended after cancellation");
		}
	}

	bool Cancellable() const override {
		return true;
	}

	void Cancel() override {
		INFO_LOG(Log::System, "PromiseTask cancelled");
		tx_->CancelAndRelease();
	}

	std::function<T ()> fun_;
	Mailbox<T> *tx_;
	const TaskType type_;
	const TaskPriority priority_;
};

// Represents pending or actual data.
// Has ownership over the data. Single use.
// Poll/BlockUntilReady may be called from any number of threads at once; the value is
// immutable once delivered. Cancel and destruction belong to the owner.
// TODO: Make movable?
template<class T>
class Promise {
public:
	// Never fails.
	static Promise<T> *Spawn(ThreadManager *threadman, std::function<T()> fun, TaskType taskType, TaskPriority taskPriority = TaskPriority::NORMAL) {
		Promise<T> *promise = new Promise<T>();
		promise->rx_ = new Mailbox<T>();
		threadman->EnqueueTask(new PromiseTask<T>(fun, promise->rx_, taskType, taskPriority));
		return promise;
	}

	static Promise<T> *AlreadyDone(T data) {
		Promise<T> *promise = new Promise<T>();
		promise->data_ = data;
		return promise;
	}

	static Promise<T> *CreateEmpty() {
		Promise<T> *promise = new Promise<T>();
		promise->rx_ = new Mailbox<T>();
		return promise;
	}

	// Allow an empty promise to spawn, too, in case we want to delay it.
	void SpawnEmpty(ThreadManager *threadman, std::function<T()> fun, TaskType taskType, TaskPriority taskPriority = TaskPriority::NORMAL) {
		threadman->EnqueueTask(new PromiseTask<T>(fun, rx_, taskType, taskPriority));
	}

	~Promise() {
		// A promise should have been fulfilled (or cancelled) before it's destroyed.
		T unused;
		_assert_(!rx_ || rx_->Poll(&unused));
		if (rx_)
			rx_->Release();
		sentinel_ = 0xeeeeeeee;
	}

	// Returns T if the data is ready, nullptr if it's not.
	// Obviously, can only be used if T is nullable, otherwise it won't compile.
	T Poll() {
		uint32_t sentinel = sentinel_;
		_assert_msg_(sentinel == 0xffc0ffee, "%08x", sentinel);
		if (!rx_)
			return data_;
		T data;
		if (rx_->Poll(&data))
			return data;
		return nullptr;
	}

	T BlockUntilReady() {
		uint32_t sentinel = sentinel_;
		_assert_msg_(sentinel == 0xffc0ffee, "%08x", sentinel);
		if (!rx_)
			return data_;
		return rx_->Wait();
	}

	// For outside injection of data, when not using Spawn.
	void Post(T data) {
		rx_->Send(data);
	}

	void Cancel() {
		if (rx_)
			rx_->CancelAndRelease();
	}

private:
	Promise() {}

	// Promise can only be constructed in Spawn (or AlreadyDone).
	T data_{};               // only for AlreadyDone
	Mailbox<T> *rx_ = nullptr;  // shared with the task; outlives every waiter
	uint32_t sentinel_ = 0xffc0ffee;
};
