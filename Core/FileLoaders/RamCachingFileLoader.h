// Copyright (c) 2015- PPSSPP Project.

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

#include <memory>

#include <retro_atomic.h>

#include "Common/CommonTypes.h"
#include "Core/Loaders.h"
#include "Common/Thread/ParkingLot.h"
#include "Common/Thread/Thread.h"

// ReadAt from one thread at a time. While the cache exists, every backend read happens on
// the loader's own worker thread, so the backend is only ever read from that one thread.
class RamCachingFileLoader : public ProxiedFileLoader {
public:
	RamCachingFileLoader(FileLoader *backend);
	~RamCachingFileLoader();

	bool Exists() override;
	bool ExistsFast() override;
	bool IsDirectory() override;
	s64 FileSize() override;

	size_t ReadAt(s64 absolutePos, size_t bytes, size_t count, void *data, Flags flags = Flags::NONE) override {
		return ReadAt(absolutePos, bytes * count, data, flags) / bytes;
	}
	size_t ReadAt(s64 absolutePos, size_t bytes, void *data, Flags flags = Flags::NONE) override;

	void Cancel() override;

private:
	void InitCache();
	void ShutdownCache();
	size_t ReadFromCache(s64 pos, size_t bytes, void *data);
	size_t RunRequest(s64 pos, size_t bytes, void *data, Flags flags);
	void StartReadAhead(s64 pos);
	// Worker only.
	void WorkerLoop();
	// Returns how many blocks it loaded.
	u32 SaveIntoCache(s64 pos, size_t bytes, Flags flags);
	u32 NextAheadBlock(u32 startFrom);

	bool BlockReady(size_t block) {
		return retro_atomic_load_acquire_int(&blocks_[block]) != 0;
	}

	enum {
		BLOCK_SIZE = 65536,
		BLOCK_SHIFT = 16,
		MAX_BLOCKS_PER_READ = 16,
		BLOCK_READAHEAD = 4,
	};

	// Fixed before the worker starts.
	s64 filesize_ = 0;
	u8 *cache_ = nullptr;
	size_t blockCount_ = 0;
	// Reader thread only.
	int exists_ = -1;
	int isDirectory_ = -1;

	// Only the worker writes cache_ data and sets a block, with a release store after the data.
	std::unique_ptr<retro_atomic_int_t[]> blocks_;
	// Worker only.
	u32 aheadRemaining_ = 0;
	retro_atomic_int_t allLoaded_{ 0 };

	// One request in flight at a time: the reader fills req*, publishes reqSeq_, waits for doneSeq_.
	s64 reqPos_ = 0;
	size_t reqBytes_ = 0;
	void *reqData_ = nullptr;
	Flags reqFlags_ = Flags::NONE;
	size_t reqResult_ = 0;
	int reqSeqLocal_ = 0;
	retro_atomic_int_t reqSeq_{ 0 };
	retro_atomic_int_t doneSeq_{ 0 };
	// Block to read ahead from next, or -1. Reading ahead starts with the first one posted.
	retro_atomic_int_t aheadBlock_{ -1 };
	retro_atomic_int_t aheadCancel_{ 0 };
	retro_atomic_int_t quit_{ 0 };
	EventCounter wake_;
	Thread worker_;
};
