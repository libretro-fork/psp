#pragma once

#include <cstdint>
#include <cstddef>

#include "Common/File/Path.h"

// Positioned reads of one file from any number of threads, without a lock: each thread
// reads through a handle of its own, opened on first use and cached. A thread reading
// sequentially doesn't seek, which keeps whatever the stream buffers.
class PositionalFile {
public:
	explicit PositionalFile(const Path &path);
	~PositionalFile();
	PositionalFile(const PositionalFile &) = delete;
	PositionalFile &operator=(const PositionalFile &) = delete;

	bool IsOpen() const { return size_ >= 0; }
	int64_t Size() const { return size_; }
	const Path &GetPath() const { return path_; }

	// Returns the bytes read, 0 at the end or on error.
	size_t ReadAt(uint64_t offset, void *dst, size_t len);

private:
	Path path_;
	uint32_t id_;
	int64_t size_ = -1;
};
