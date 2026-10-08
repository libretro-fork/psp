#include <zip/rzip_archive.h>

#include "Core/FileLoaders/LocalFileLoader.h"
#include "Core/FileLoaders/ZipFileLoader.h"

int64_t ZipFileLoader::BackendRead(void *userdata, uint64_t off, void *dst, size_t len) {
	FileLoader *backend = (FileLoader *)userdata;
	return (int64_t)backend->ReadAt((s64)off, len, dst);
}

ZipFileLoader::ZipFileLoader(FileLoader *sourceLoader)
	: ProxiedFileLoader(sourceLoader) {
	if (!backend_ || !backend_->Exists() || backend_->IsDirectory()) {
		return;
	}
	zip_ = ZipContainer(&BackendRead, backend_, (uint64_t)backend_->FileSize());
	if (!zip_) {
		ERROR_LOG(Log::IO, "Failed to open ZIP archive: %s", backend_->GetPath().c_str());
	}
}

ZipFileLoader::~ZipFileLoader() {
	if (seek_) {
		rzip_seek_free(seek_);
	}
}

bool ZipFileLoader::Initialize(int fileIndex) {
	_dbg_assert_(!initialized_);

	if (!zip_) {
		ERROR_LOG(Log::IO, "Cannot initialize: ZIP archive is null");
		return false;
	}

	const rzip_entry_t *entry = rzip_archive_entry(zip_.Raw(), (uint32_t)fileIndex);
	if (!entry) {
		ERROR_LOG(Log::IO, "Failed to get file index %d", fileIndex);
		return false;
	}
	// Callers (Identify_File and friends) compare against lowercase extensions, like
	// Path::GetFileExtension returns. Not using tolower to avoid the Turkish I problem.
	fileExtension_ = KeepIncludingLast(entry->name, '.');
	for (size_t i = 0; i < fileExtension_.size(); i++) {
		char c = fileExtension_[i];
		if (c >= 'A' && c <= 'Z') {
			fileExtension_[i] = c + ('a' - 'A');
		}
	}

	dataFileSize_ = (s64)entry->size;
	if (entry->method == RZIP_METHOD_STORED) {
		if (entry->csize != entry->size) {
			ERROR_LOG(Log::IO, "Stored ZIP member %d has mismatched sizes", fileIndex);
			return false;
		}
		stored_ = true;
		dataOffset_ = entry->data_off;
	} else {
		seek_ = rzip_seek_new(zip_.Raw(), (uint32_t)fileIndex, 0);
		if (!seek_) {
			ERROR_LOG(Log::IO, "Can't read ZIP member %d (method %u)", fileIndex, entry->method);
			return false;
		}
	}
	initialized_ = true;
	return true;
}

size_t ZipFileLoader::ReadAt(s64 absolutePos, size_t bytes, void *data, Flags flags) {
	if (!initialized_ || absolutePos < 0 || absolutePos >= dataFileSize_) {
		return 0;
	}

	if (absolutePos + (s64)bytes > dataFileSize_) {
		bytes = dataFileSize_ - absolutePos;
	}

	if (stored_) {
		return backend_->ReadAt((s64)dataOffset_ + absolutePos, bytes, data, flags);
	}

	// Index on until the range is covered, then decode from the nearest point.
	const uint64_t end = (uint64_t)absolutePos + bytes;
	while (rzip_seek_covered(seek_) < end) {
		const int state = rzip_seek_build(seek_, end);
		if (state < 0) {
			ERROR_LOG(Log::IO, "ZIP member is corrupt (%d)", state);
			return 0;
		}
		if (state == 1)
			break;
	}
	if (rzip_seek_read(seek_, (uint64_t)absolutePos, (uint8_t *)data, bytes) != RZIP_OK) {
		ERROR_LOG(Log::IO, "ZIP read failed at %lld", (long long)absolutePos);
		return 0;
	}
	return bytes;
}
