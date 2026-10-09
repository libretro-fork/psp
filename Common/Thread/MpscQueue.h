// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include <new>
#include <utility>

#include <queues/mpsc_stack.h>

// Unbounded queue on libretro-common's mpsc_stack: any thread pushes, one thread drains,
// oldest first. Nodes are recycled: a drained node goes onto a free stack shared by every
// queue of the same T, and a producer takes that stack whole when its own thread's list is
// empty, so once warmed up a push doesn't allocate.
template <class T>
class MpscQueue {
public:
	MpscQueue() {
		mpsc_stack_init(&stack_);
	}
	~MpscQueue() {
		Drain([](T &&) {});
	}
	MpscQueue(const MpscQueue &) = delete;
	MpscQueue &operator=(const MpscQueue &) = delete;

	// Any thread.
	void Push(T value) {
		Node *node = TakeNode();
		new (node->storage) T(std::move(value));
		mpsc_stack_push(&stack_, &node->link);
	}

	// Any thread, as a hint; exact for the consumer.
	bool Empty() {
		return mpsc_stack_empty(&stack_);
	}

	// Consumer only. Hands every queued item to f, oldest first. Returns whether there were any.
	template <class F>
	bool Drain(F f) {
		mpsc_stack_node_t *node = mpsc_stack_reverse(mpsc_stack_drain(&stack_));
		const bool any = node != nullptr;
		while (node) {
			mpsc_stack_node_t *next = node->next;
			Node *item = (Node *)node;
			T *value = item->Value();
			f(std::move(*value));
			value->~T();
			mpsc_stack_push(&freeNodes_, &item->link);
			node = next;
		}
		return any;
	}

private:
	// Standard layout with the stack node first, so a drained node converts back to its Node.
	struct Node {
		mpsc_stack_node_t link;
		alignas(T) unsigned char storage[sizeof(T)];
		T *Value() { return reinterpret_cast<T *>(storage); }
	};

	// Nodes this thread took from freeNodes_. Only this thread touches it.
	struct LocalNodes {
		mpsc_stack_node_t *head = nullptr;
		~LocalNodes() {
			while (head) {
				mpsc_stack_node_t *next = head->next;
				delete (Node *)head;
				head = next;
			}
		}
	};

	static Node *TakeNode() {
		static thread_local LocalNodes local;
		if (!local.head) {
			local.head = mpsc_stack_drain(&freeNodes_);
		}
		if (!local.head) {
			return new Node();
		}
		Node *node = (Node *)local.head;
		local.head = local.head->next;
		return node;
	}

	mpsc_stack_t stack_;
	// Zero-initialized, which is an empty stack.
	static mpsc_stack_t freeNodes_;
};

template <class T>
mpsc_stack_t MpscQueue<T>::freeNodes_;
