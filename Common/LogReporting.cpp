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

#include <cstdarg>
#include <cstdio>
#include <cstdint>

#include <retro_atomic.h>

#include "Common/LogReporting.h"

namespace Reporting {

// Keeps track of report-only-once identifiers.  Since they're always constants, a pointer is okay.
// Fixed open-addressed table: slots are claimed with CAS and never freed.
enum { LOG_N_TIMES_SLOTS = 1024 };
struct LogNTimesSlot {
	retro_atomic_ptr_t key;
	retro_atomic_int_t count;
};
static LogNTimesSlot logNTimes[LOG_N_TIMES_SLOTS];

AllowedCallback allowedCallback = nullptr;
MessageCallback messageCallback = nullptr;

static LogNTimesSlot *FindSlot(const char *identifier) {
	uintptr_t h = (uintptr_t)identifier;
	h ^= h >> 15;
	h *= (uintptr_t)0x9E3779B97F4A7C15ULL;
	for (int probe = 0; probe < LOG_N_TIMES_SLOTS; probe++) {
		LogNTimesSlot &slot = logNTimes[(h + probe) & (LOG_N_TIMES_SLOTS - 1)];
		void *key = retro_atomic_load_acquire_ptr(&slot.key);
		if (key == identifier)
			return &slot;
		if (!key) {
			if (retro_atomic_cas_ptr(&slot.key, nullptr, (void *)identifier))
				return &slot;
			if (retro_atomic_load_acquire_ptr(&slot.key) == identifier)
				return &slot;
		}
	}
	return nullptr;
}

bool ShouldLogNTimes(const char *identifier, int count) {
	LogNTimesSlot *slot = FindSlot(identifier);
	if (!slot)
		return false;
	for (;;) {
		const int c = retro_atomic_load_relaxed_int(&slot->count);
		if (c >= count)
			return false;
		if (retro_atomic_cas_int(&slot->count, c, c + 1))
			return true;
	}
}

void ResetCounts() {
	for (int i = 0; i < LOG_N_TIMES_SLOTS; i++)
		retro_atomic_store_relaxed_int(&logNTimes[i].count, 0);
}

void SetupCallbacks(AllowedCallback allowed, MessageCallback message) {
	allowedCallback = allowed;
	messageCallback = message;
}

void ReportMessage(const char *message, ...) {
	const int MESSAGE_BUFFER_SIZE = 65536;

	va_list args;
	va_start(args, message);
	char *temp = new char[MESSAGE_BUFFER_SIZE];
	vsnprintf(temp, MESSAGE_BUFFER_SIZE - 1, message, args);
	temp[MESSAGE_BUFFER_SIZE - 1] = '\0';
	va_end(args);

	if (!allowedCallback || !messageCallback) {
		ERROR_LOG(Log::System, "Reporting not initialized, skipping: %s", temp);
		delete[] temp;
		return;
	}

	if (!allowedCallback()) {
		delete[] temp;
		return;
	}

	messageCallback(message, temp);

	delete[] temp;
}

void ReportMessageFormatted(const char *message, const char *formatted) {
	if (!allowedCallback || !messageCallback) {
		ERROR_LOG(Log::System, "Reporting not initialized, skipping: %s", formatted);
		return;
	}

	if (!allowedCallback())
		return;
	messageCallback(message, formatted);
}

}  // namespace
