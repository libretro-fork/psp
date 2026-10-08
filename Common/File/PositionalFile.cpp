#include <cstdio>

#include <retro_atomic.h>

#include "Common/File/FileUtil.h"
#include "Common/File/PositionalFile.h"

namespace {

// Ids are never reused, so a handle cached for a file that has since gone away
// can't be mistaken for one of a newer file.
retro_atomic_int_t g_nextId;

struct Handle {
	uint32_t id;
	FILE *file;
	int64_t pos;  // where file is; -1 when unknown
	uint32_t lastUse;
};

// This thread's handles: a few files at once, least recently used goes first.
struct HandleCache {
	enum { SLOTS = 8 };
	Handle slots[SLOTS]{};
	uint32_t clock = 0;

	~HandleCache() {
		for (Handle &h : slots) {
			if (h.file)
				fclose(h.file);
		}
	}

	Handle *Get(uint32_t id, const Path &path) {
		Handle *victim = &slots[0];
		for (Handle &h : slots) {
			if (h.file && h.id == id) {
				h.lastUse = ++clock;
				return &h;
			}
			if (!h.file || h.lastUse < victim->lastUse)
				victim = &h;
		}
		FILE *file = File::OpenCFile(path, "rb");
		if (!file)
			return nullptr;
		if (victim->file)
			fclose(victim->file);
		*victim = Handle{ id, file, 0, ++clock };
		return victim;
	}

	void Forget(uint32_t id) {
		for (Handle &h : slots) {
			if (h.file && h.id == id) {
				fclose(h.file);
				h = Handle{};
			}
		}
	}
};

thread_local HandleCache t_handles;

}  // namespace

PositionalFile::PositionalFile(const Path &path) : path_(path) {
	id_ = (uint32_t)retro_atomic_fetch_add_int(&g_nextId, 1) + 1;
	Handle *h = t_handles.Get(id_, path_);
	if (h)
		size_ = File::GetFileSize(h->file);
}

PositionalFile::~PositionalFile() {
	// Other threads' handles for this file close when they're evicted or the thread ends.
	t_handles.Forget(id_);
}

size_t PositionalFile::ReadAt(uint64_t offset, void *dst, size_t len) {
	if (len == 0 || size_ < 0)
		return 0;
	Handle *h = t_handles.Get(id_, path_);
	if (!h)
		return 0;
	if (h->pos != (int64_t)offset) {
		if (File::Fseek(h->file, (int64_t)offset, SEEK_SET) != 0) {
			h->pos = -1;
			return 0;
		}
		h->pos = (int64_t)offset;
	}
	const size_t got = fread(dst, 1, len, h->file);
	h->pos = got == len ? h->pos + (int64_t)got : -1;
	return got;
}
