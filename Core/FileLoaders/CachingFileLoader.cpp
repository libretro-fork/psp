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

#include <algorithm>
#include <cstring>

#include "Common/Thread/ThreadUtil.h"
#include "Core/FileLoaders/CachingFileLoader.h"

// Takes ownership of backend.
CachingFileLoader::CachingFileLoader(FileLoader *backend)
	: ProxiedFileLoader(backend) {
}

void CachingFileLoader::Prepare() {
	if (prepared_) {
		return;
	}
	prepared_ = true;
	filesize_ = ProxiedFileLoader::FileSize();
	if (filesize_ > 0) {
		InitCache();
	}
}

CachingFileLoader::~CachingFileLoader() {
	if (filesize_ > 0) {
		ShutdownCache();
	}
}

bool CachingFileLoader::Exists() {
	if (exists_ == -1) {
		exists_ = ProxiedFileLoader::Exists() ? 1 : 0;
	}
	return exists_ == 1;
}

bool CachingFileLoader::ExistsFast() {
	if (exists_ == -1) {
		return ProxiedFileLoader::ExistsFast();
	}
	return exists_ == 1;
}

bool CachingFileLoader::IsDirectory() {
	if (isDirectory_ == -1) {
		isDirectory_ = ProxiedFileLoader::IsDirectory() ? 1 : 0;
	}
	return isDirectory_ == 1;
}

s64 CachingFileLoader::FileSize() {
	Prepare();
	return filesize_;
}

size_t CachingFileLoader::ReadAt(s64 absolutePos, size_t bytes, void *data, Flags flags) {
	Prepare();
	if (absolutePos >= filesize_) {
		bytes = 0;
	} else if (absolutePos + (s64)bytes >= filesize_) {
		bytes = (size_t)(filesize_ - absolutePos);
	}

	if (!slots_) {
		// Empty or unknown size: no cache and no worker.
		return (flags & Flags::HINT_UNCACHED) != 0 ? backend_->ReadAt(absolutePos, bytes, data, flags) : 0;
	}

	size_t readSize = 0;
	if ((flags & Flags::HINT_UNCACHED) != 0) {
		readSize = RequestUncached(absolutePos, bytes, data, flags);
	} else {
		readSize = ReadFromCache(absolutePos, bytes, data);
		// While in case the cache size is too small for the entire read.
		while (readSize < bytes) {
			RequestIntoCache(absolutePos + readSize, bytes - readSize, flags);
			size_t bytesFromCache = ReadFromCache(absolutePos + readSize, bytes - readSize, (u8 *)data + readSize);
			readSize += bytesFromCache;
			if (bytesFromCache == 0) {
				// We can't read any more.
				break;
			}
		}

		StartReadAhead(absolutePos + readSize);
	}

	return readSize;
}

void CachingFileLoader::InitCache() {
	oldestGeneration_ = 0;
	generation_ = 0;
	numBlocks_ = (filesize_ + BLOCK_SIZE - 1) >> BLOCK_SHIFT;
	slots_.reset(new retro_atomic_ptr_t[(size_t)numBlocks_]);
	for (s64 i = 0; i < numBlocks_; ++i) {
		retro_atomic_ptr_init(&slots_[(size_t)i], nullptr);
	}
	generations_.reset(new u64[(size_t)numBlocks_]());
	worker_ = Thread([this] {
		WorkerLoop();
	});
}

void CachingFileLoader::ShutdownCache() {
	retro_atomic_store_release_int(&quit_, 1);
	wake_.Notify();
	// Waits out any backend read in progress.
	if (worker_.joinable()) {
		worker_.join();
	}

	for (s64 i = 0; i < numBlocks_; ++i) {
		delete [] BlockAt(i);
	}
	slots_.reset();
}

size_t CachingFileLoader::ReadFromCache(s64 pos, size_t bytes, void *data) {
	if (bytes == 0) {
		return 0;
	}
	s64 cacheStartPos = pos >> BLOCK_SHIFT;
	s64 cacheEndPos = (pos + bytes - 1) >> BLOCK_SHIFT;
	// TODO: Smarter.
	size_t readSize = 0;
	size_t offset = (size_t)(pos - (cacheStartPos << BLOCK_SHIFT));
	u8 *p = (u8 *)data;

	for (s64 i = cacheStartPos; i <= cacheEndPos; ++i) {
		const u8 *block = BlockAt(i);
		if (!block) {
			return readSize;
		}
		generations_[(size_t)i] = generation_;

		size_t toRead = std::min(bytes - readSize, (size_t)BLOCK_SIZE - offset);
		memcpy(p + readSize, block + offset, toRead);
		readSize += toRead;

		// Don't need an offset after the first read.
		offset = 0;
	}
	return readSize;
}

size_t CachingFileLoader::RunRequest() {
	const int seq = (int)((unsigned)reqSeqLocal_ + 1);
	reqSeqLocal_ = seq;
	retro_atomic_store_release_int(&reqSeq_, seq);
	wake_.Notify();
	ParkingLotWait(&doneSeq_, [&] {
		return retro_atomic_load_acquire_int(&doneSeq_) == seq;
	});
	return reqResult_;
}

size_t CachingFileLoader::RequestUncached(s64 pos, size_t bytes, void *data, Flags flags) {
	reqPos_ = pos;
	reqBytes_ = bytes;
	reqData_ = data;
	reqFlags_ = flags;
	return RunRequest();
}

void CachingFileLoader::RequestIntoCache(s64 pos, size_t bytes, Flags flags) {
	s64 cacheStartPos = pos >> BLOCK_SHIFT;
	s64 cacheEndPos = std::min((pos + (s64)bytes - 1) >> BLOCK_SHIFT, numBlocks_ - 1);

	size_t blocksToRead = 0;
	for (s64 i = cacheStartPos; i <= cacheEndPos; ++i) {
		if (BlockAt(i)) {
			break;
		}
		++blocksToRead;
		if (blocksToRead >= MAX_BLOCKS_PER_READ) {
			break;
		}
	}
	if (blocksToRead == 0) {
		return;
	}

	MakeCacheSpaceFor(blocksToRead);
	reqPos_ = pos;
	reqBytes_ = bytes;
	reqData_ = nullptr;
	reqFlags_ = flags;
	RunRequest();
	++generation_;
}

void CachingFileLoader::WorkerLoop() {
	SetCurrentThreadName("FileLoaderReadAhead");

	int done = 0;
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

		const int ahead = retro_atomic_exchange_int(&aheadBlock_, -1);
		if (ahead >= 0) {
			ReadAhead(ahead);
			continue;
		}

		wake_.Wait(seen);
	}
}

void CachingFileLoader::ReadAhead(s64 cacheStartPos) {
	s64 cacheEndPos = std::min(cacheStartPos + BLOCK_READAHEAD - 1, numBlocks_ - 1);
	for (s64 i = cacheStartPos; i <= cacheEndPos; ++i) {
		if (!BlockAt(i)) {
			SaveIntoCache(i << BLOCK_SHIFT, BLOCK_SIZE * BLOCK_READAHEAD, Flags::NONE, true);
			break;
		}
	}
}

void CachingFileLoader::SaveIntoCache(s64 pos, size_t bytes, Flags flags, bool readingAhead) {
	s64 cacheStartPos = pos >> BLOCK_SHIFT;
	s64 cacheEndPos = (pos + bytes - 1) >> BLOCK_SHIFT;
	// Read-ahead can ask for blocks past the end of the file.
	cacheEndPos = std::min(cacheEndPos, numBlocks_ - 1);

	// Only this thread fills slots, so the empty ones counted here stay empty until we fill them.
	size_t blocksToRead = 0;
	for (s64 i = cacheStartPos; i <= cacheEndPos; ++i) {
		if (BlockAt(i)) {
			break;
		}
		++blocksToRead;
		if (blocksToRead >= MAX_BLOCKS_PER_READ) {
			break;
		}
	}

	if (blocksToRead == 0) {
		return;
	}
	// Read-ahead never evicts; the reader makes room before a demand read.
	if (readingAhead && (size_t)retro_atomic_load_acquire_int(&cachedCount_) + blocksToRead > MAX_BLOCKS_CACHED) {
		return;
	}

	if (blocksToRead == 1) {
		u8 *buf = new u8[BLOCK_SIZE];
		size_t readBytes = backend_->ReadAt(cacheStartPos << BLOCK_SHIFT, BLOCK_SIZE, buf, flags);

		// Only cache a block we actually fully read - a short/failed read (e.g. a
		// dropped connection on a Remote ISO) must not be cached as if valid, or
		// every later read of this block would silently return the uninitialized
		// tail of `buf` as if it were real file data. The last block of the file
		// is shorter, and complete if the read reached the end.
		if (readBytes == BLOCK_SIZE || (readBytes > 0 && (cacheStartPos << BLOCK_SHIFT) + (s64)readBytes == filesize_)) {
			retro_atomic_store_release_ptr(&slots_[(size_t)cacheStartPos], buf);
			retro_atomic_fetch_add_int(&cachedCount_, 1);
		} else {
			delete [] buf;
		}
	} else {
		u8 *wholeRead = new u8[blocksToRead << BLOCK_SHIFT];
		size_t readBytes = backend_->ReadAt(cacheStartPos << BLOCK_SHIFT, blocksToRead << BLOCK_SHIFT, wholeRead, flags);
		size_t wholeBlocksRead = readBytes >> BLOCK_SHIFT;
		if ((readBytes & (BLOCK_SIZE - 1)) != 0 && (cacheStartPos << BLOCK_SHIFT) + (s64)readBytes == filesize_) {
			// The short last block of the file.
			wholeBlocksRead++;
		}

		for (size_t i = 0; i < wholeBlocksRead; ++i) {
			u8 *buf = new u8[BLOCK_SIZE];
			memcpy(buf, wholeRead + (i << BLOCK_SHIFT), BLOCK_SIZE);
			retro_atomic_store_release_ptr(&slots_[(size_t)cacheStartPos + i], buf);
			retro_atomic_fetch_add_int(&cachedCount_, 1);
		}
		delete[] wholeRead;
	}
}

void CachingFileLoader::MakeCacheSpaceFor(size_t blocks) {
	size_t goal = MAX_BLOCKS_CACHED - blocks;

	while ((size_t)retro_atomic_load_acquire_int(&cachedCount_) > goal) {
		u64 minGeneration = generation_;

		for (s64 i = 0; i < numBlocks_; ++i) {
			u8 *block = BlockAt(i);
			if (!block) {
				continue;
			}
			const u64 generation = generations_[(size_t)i];
			// Check for the minimum seen generation.
			// TODO: Do this smarter?
			if (generation != 0 && generation < minGeneration) {
				minGeneration = generation;
			}

			// 0 means it was never used yet or was the first read (e.g. block descriptor.)
			if (generation == oldestGeneration_ || generation == 0) {
				// The worker never reads a published block, so it can go right away.
				retro_atomic_store_release_ptr(&slots_[(size_t)i], nullptr);
				generations_[(size_t)i] = 0;
				delete [] block;
				if ((size_t)(retro_atomic_fetch_sub_int(&cachedCount_, 1) - 1) <= goal) {
					break;
				}
			}
		}

		// If we didn't find any, update to the lowest we did find.
		oldestGeneration_ = minGeneration;
	}
}

void CachingFileLoader::StartReadAhead(s64 pos) {
	s64 cacheStartPos = pos >> BLOCK_SHIFT;
	if (cacheStartPos >= numBlocks_) {
		return;
	}
	if ((size_t)retro_atomic_load_acquire_int(&cachedCount_) + BLOCK_READAHEAD > MAX_BLOCKS_CACHED) {
		// Not enough space to readahead.
		return;
	}
	s64 cacheEndPos = std::min(cacheStartPos + BLOCK_READAHEAD - 1, numBlocks_ - 1);
	for (s64 i = cacheStartPos; i <= cacheEndPos; ++i) {
		if (!BlockAt(i)) {
			// A newer position replaces one the worker hasn't picked up yet.
			retro_atomic_store_release_int(&aheadBlock_, (int)cacheStartPos);
			wake_.Notify();
			return;
		}
	}
}
