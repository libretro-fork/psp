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

#include <utility>

#include <queues/mpsc_stack.h>

// Unbounded queue on libretro-common's mpsc_stack: any thread pushes, one thread drains,
// oldest first. Every item is one heap node, so keep it off per-sample paths.
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
		Node *node = new Node(std::move(value));
		mpsc_stack_push(&stack_, &node->link.node);
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
			Node *item = (Node *)((Link *)node)->owner;
			f(std::move(item->value));
			delete item;
			node = next;
		}
		return any;
	}

private:
	// Standard layout with the stack node first, so a drained node converts back to its Link.
	struct Link {
		mpsc_stack_node_t node;
		void *owner;
	};
	struct Node {
		explicit Node(T &&v) : value(std::move(v)) {
			link.node.next = nullptr;
			link.owner = this;
		}
		Link link;
		T value;
	};

	mpsc_stack_t stack_;
};
