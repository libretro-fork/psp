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

#include <memory>

#include <retro_atomic.h>

#include "Common/CommonTypes.h"
#include "Core/Loaders.h"
#include "Common/Thread/ParkingLot.h"
#include "Common/Thread/Thread.h"

// ReadAt from one thread at a time. Every backend read happens on the loader's own worker
// thread, so the backend is only ever read from that one thread.
class CachingFileLoader : public ProxiedFileLoader {
public:
	CachingFileLoader(FileLoader *backend);
	~CachingFileLoader();

	bool Exists() override;
	bool ExistsFast() override;
	bool IsDirectory() override;
	s64 FileSize() override;

	size_t ReadAt(s64 absolutePos, size_t bytes, size_t count, void *data, Flags flags = Flags::NONE) override {
		return ReadAt(absolutePos, bytes * count, data, flags) / bytes;
	}
	size_t ReadAt(s64 absolutePos, size_t bytes, void *data, Flags flags = Flags::NONE) override;

private:
	void Prepare();
	void InitCache();
	void ShutdownCache();
	size_t ReadFromCache(s64 pos, size_t bytes, void *data);
	// Reader side: makes room, then has the worker read at least one block into the cache.
	void RequestIntoCache(s64 pos, size_t bytes, Flags flags);
	size_t RequestUncached(s64 pos, size_t bytes, void *data, Flags flags);
	size_t RunRequest();
	// Worker only.
	void SaveIntoCache(s64 pos, size_t bytes, Flags flags, bool readingAhead = false);
	void ReadAhead(s64 block);
	void WorkerLoop();
	void MakeCacheSpaceFor(size_t blocks);
	void StartReadAhead(s64 pos);

	u8 *BlockAt(s64 block) {
		return (u8 *)retro_atomic_load_acquire_ptr(&slots_[(size_t)block]);
	}

	enum {
		BLOCK_SIZE = 65536,
		BLOCK_SHIFT = 16,
		MAX_BLOCKS_PER_READ = 16,
		MAX_BLOCKS_CACHED = 4096, // 256 MB
		BLOCK_READAHEAD = 4,
	};

	// Reader thread only, except filesize_ and numBlocks_, fixed before the worker starts.
	bool prepared_ = false;
	s64 filesize_ = 0;
	s64 numBlocks_ = 0;
	int exists_ = -1;
	int isDirectory_ = -1;
	u64 generation_ = 0;
	u64 oldestGeneration_ = 0;
	std::unique_ptr<u64[]> generations_;

	// One slot per block. Only the worker fills an empty slot, only the reader empties one,
	// and the worker never touches a block's data once published.
	std::unique_ptr<retro_atomic_ptr_t[]> slots_;
	retro_atomic_int_t cachedCount_{ 0 };

	// One request in flight at a time: the reader fills req*, publishes reqSeq_, waits for doneSeq_.
	s64 reqPos_ = 0;
	size_t reqBytes_ = 0;
	void *reqData_ = nullptr;
	Flags reqFlags_ = Flags::NONE;
	size_t reqResult_ = 0;
	int reqSeqLocal_ = 0;
	retro_atomic_int_t reqSeq_{ 0 };
	retro_atomic_int_t doneSeq_{ 0 };
	// Next block to read ahead from, or -1.
	retro_atomic_int_t aheadBlock_{ -1 };
	retro_atomic_int_t quit_{ 0 };
	EventCounter wake_;
	Thread worker_;
};
