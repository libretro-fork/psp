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

#include <algorithm>
#include <cstring>

#include "Common/Thread/ThreadUtil.h"
#include "Common/Log.h"
#include "Core/FileLoaders/RamCachingFileLoader.h"

// Takes ownership of backend.
RamCachingFileLoader::RamCachingFileLoader(FileLoader *backend)
	: ProxiedFileLoader(backend) {
	filesize_ = backend->FileSize();
	if (filesize_ > 0) {
		InitCache();
	}
}

RamCachingFileLoader::~RamCachingFileLoader() {
	if (filesize_ > 0) {
		ShutdownCache();
	}
}

bool RamCachingFileLoader::Exists() {
	if (exists_ == -1) {
		exists_ = ProxiedFileLoader::Exists() ? 1 : 0;
	}
	return exists_ == 1;
}

bool RamCachingFileLoader::ExistsFast() {
	if (exists_ == -1) {
		return ProxiedFileLoader::ExistsFast();
	}
	return exists_ == 1;
}

bool RamCachingFileLoader::IsDirectory() {
	if (isDirectory_ == -1) {
		isDirectory_ = ProxiedFileLoader::IsDirectory() ? 1 : 0;
	}
	return isDirectory_ == 1;
}

s64 RamCachingFileLoader::FileSize() {
	return filesize_;
}

size_t RamCachingFileLoader::ReadAt(s64 absolutePos, size_t bytes, void *data, Flags flags) {
	if (cache_ == nullptr) {
		// No cache, so no worker: the backend is ours.
		return backend_->ReadAt(absolutePos, bytes, data, flags);
	}
	if ((flags & Flags::HINT_UNCACHED) != 0) {
		return RunRequest(absolutePos, bytes, data, flags);
	}

	size_t readSize = ReadFromCache(absolutePos, bytes, data);
	// While in case the cache size is too small for the entire read.
	while (readSize < bytes) {
		RunRequest(absolutePos + readSize, bytes - readSize, nullptr, flags);
		size_t bytesFromCache = ReadFromCache(absolutePos + readSize, bytes - readSize, (u8 *)data + readSize);
		readSize += bytesFromCache;
		if (bytesFromCache == 0) {
			// We can't read any more.
			break;
		}
	}

	StartReadAhead(absolutePos + readSize);
	return readSize;
}

void RamCachingFileLoader::InitCache() {
	u32 blockCount = (u32)((filesize_ + BLOCK_SIZE - 1) >> BLOCK_SHIFT);
	// Overallocate for the last block.
	cache_ = (u8 *)malloc((size_t)blockCount << BLOCK_SHIFT);
	if (cache_ == nullptr) {
		ERROR_LOG(Log::IO, "Failed to allocate cache for Cache full ISO in RAM! Will fall back to regular reads.");
		return;
	}
	aheadRemaining_ = blockCount;
	blockCount_ = blockCount;
	blocks_.reset(new retro_atomic_int_t[blockCount_]);
	for (size_t i = 0; i < blockCount_; ++i) {
		retro_atomic_int_init(&blocks_[i], 0);
	}
	worker_ = Thread([this] {
		WorkerLoop();
	});
}

void RamCachingFileLoader::ShutdownCache() {
	Cancel();

	retro_atomic_store_release_int(&quit_, 1);
	wake_.Notify();
	// Waits out any backend read in progress.
	if (worker_.joinable()) {
		worker_.join();
	}

	blocks_.reset();
	blockCount_ = 0;
	if (cache_ != nullptr) {
		free(cache_);
		cache_ = nullptr;
	}
}

void RamCachingFileLoader::Cancel() {
	retro_atomic_store_release_int(&aheadCancel_, 1);
	ProxiedFileLoader::Cancel();
}

size_t RamCachingFileLoader::ReadFromCache(s64 pos, size_t bytes, void *data) {
	// Clamp bytes to what's actually available.
	if (pos >= filesize_ || bytes == 0) {
		return 0;
	}
	if (pos + (s64)bytes > filesize_) {
		bytes = (size_t)(filesize_ - pos);
	}

	s64 cacheStartPos = pos >> BLOCK_SHIFT;
	s64 cacheEndPos = (pos + bytes - 1) >> BLOCK_SHIFT;
	if ((size_t)cacheEndPos >= blockCount_) {
		cacheEndPos = blockCount_ - 1;
	}

	size_t readSize = 0;
	size_t offset = (size_t)(pos - (cacheStartPos << BLOCK_SHIFT));
	u8 *p = (u8 *)data;

	for (s64 i = cacheStartPos; i <= cacheEndPos; ++i) {
		if (!BlockReady((size_t)i)) {
			return readSize;
		}

		size_t toRead = std::min(bytes - readSize, (size_t)BLOCK_SIZE - offset);
		s64 cachePos = (i << BLOCK_SHIFT) + offset;
		memcpy(p + readSize, &cache_[cachePos], toRead);
		readSize += toRead;

		// Don't need an offset after the first read.
		offset = 0;
	}
	return readSize;
}

size_t RamCachingFileLoader::RunRequest(s64 pos, size_t bytes, void *data, Flags flags) {
	reqPos_ = pos;
	reqBytes_ = bytes;
	reqData_ = data;
	reqFlags_ = flags;
	const int seq = (int)((unsigned)reqSeqLocal_ + 1);
	reqSeqLocal_ = seq;
	retro_atomic_store_release_int(&reqSeq_, seq);
	wake_.Notify();
	ParkingLotWait(&doneSeq_, [&] {
		return retro_atomic_load_acquire_int(&doneSeq_) == seq;
	});
	return reqResult_;
}

u32 RamCachingFileLoader::SaveIntoCache(s64 pos, size_t bytes, Flags flags) {
	s64 cacheStartPos = pos >> BLOCK_SHIFT;
	if (bytes == 0 || (size_t)cacheStartPos >= blockCount_) {
		return 0;
	}
	s64 cacheEndPos = (pos + bytes - 1) >> BLOCK_SHIFT;
	if ((size_t)cacheEndPos >= blockCount_) {
		cacheEndPos = blockCount_ - 1;
	}

	// Stop at the first loaded block: the reader may be copying it, so it is never rewritten.
	size_t blocksToRead = 0;
	for (s64 i = cacheStartPos; i <= cacheEndPos; ++i) {
		if (BlockReady((size_t)i)) {
			break;
		}
		++blocksToRead;
		if (blocksToRead >= MAX_BLOCKS_PER_READ) {
			break;
		}
	}
	if (blocksToRead == 0) {
		return 0;
	}

	s64 cacheFilePos = cacheStartPos << BLOCK_SHIFT;
	size_t bytesRead = backend_->ReadAt(cacheFilePos, blocksToRead << BLOCK_SHIFT, &cache_[cacheFilePos], flags);

	// In case there was an error, let's not mark blocks that failed to read as read.
	// Round up only for a genuine short read exactly at the true end of the file -
	// cache_ is deliberately over-allocated to a full BLOCK_SIZE for the last block,
	// so its unwritten tail past filesize_ is never read back. Any other short read
	// (e.g. a dropped Remote ISO connection mid-file) must not be rounded up, or the
	// unwritten (uninitialized, since cache_ is malloc'd) rest of that block would be
	// served as if it were real file data.
	u32 blocksActuallyRead = (u32)(bytesRead >> BLOCK_SHIFT);
	if ((bytesRead & (BLOCK_SIZE - 1)) != 0 && cacheFilePos + (s64)bytesRead == filesize_) {
		++blocksActuallyRead;
	}

	for (size_t i = 0; i < blocksActuallyRead; ++i) {
		retro_atomic_store_release_int(&blocks_[(size_t)cacheStartPos + i], 1);
	}
	if (aheadRemaining_ != 0) {
		aheadRemaining_ -= blocksActuallyRead;
		if (aheadRemaining_ == 0) {
			retro_atomic_store_release_int(&allLoaded_, 1);
		}
	}
	return blocksActuallyRead;
}

void RamCachingFileLoader::StartReadAhead(s64 pos) {
	if (retro_atomic_load_acquire_int(&allLoaded_)) {
		return;
	}
	s64 block = pos >> BLOCK_SHIFT;
	if ((size_t)block > blockCount_) {
		block = (s64)blockCount_;
	}
	retro_atomic_store_release_int(&aheadCancel_, 0);
	retro_atomic_store_release_int(&aheadBlock_, (int)block);
	wake_.Notify();
}

void RamCachingFileLoader::WorkerLoop() {
	SetCurrentThreadName("FileLoaderReadAhead");

	int done = 0;
	bool readingAhead = false;
	for (;;) {
		const int seen = wake_.Seen();
		if (retro_atomic_load_acquire_int(&quit_)) {
			break;
		}

		const int seq = retro_atomic_load_acquire_int(&reqSeq_);
		if (seq != done) {
			if (reqData_) {
				reqResult_ = backend_->ReadAt(reqPos_, reqBytes_, reqData_, reqFlags_);
			} else {
				SaveIntoCache(reqPos_, reqBytes_, reqFlags_);
			}
			done = seq;
			retro_atomic_store_release_int(&doneSeq_, seq);
			ParkingLotNotify(&doneSeq_);
			continue;
		}

		// If a read posted a position, go forward from there, otherwise from the start.
		const int ahead = retro_atomic_exchange_int(&aheadBlock_, -1);
		if (ahead >= 0) {
			readingAhead = true;
		}
		if (readingAhead && aheadRemaining_ != 0 && !retro_atomic_load_acquire_int(&aheadCancel_)) {
			const u32 cacheStartPos = NextAheadBlock(ahead >= 0 ? (u32)ahead : 0);
			// A failed read stops reading ahead rather than retrying it in a loop.
			if (cacheStartPos != 0xFFFFFFFF && SaveIntoCache((s64)cacheStartPos << BLOCK_SHIFT, BLOCK_SIZE * BLOCK_READAHEAD, Flags::NONE) != 0) {
				continue;
			}
		}
		// Done until the next read posts a position.
		readingAhead = false;
		wake_.Wait(seen);
	}
}

u32 RamCachingFileLoader::NextAheadBlock(u32 startFrom) {
	for (u32 i = startFrom; i < blockCount_; ++i) {
		if (!BlockReady(i)) {
			return i;
		}
	}
	return 0xFFFFFFFF;
}
