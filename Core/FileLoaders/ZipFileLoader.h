#pragma once

#include "Common/CommonTypes.h"
#include "Common/Log.h"
#include "Common/File/Path.h"
#include "Common/File/VFS/ZipFileReader.h"
#include "Common/StringUtils.h"
#include "Core/Loaders.h"

struct rzip_seek;

// Exposes a single (chosen) file from a zip file as another file loader.
// Useful in a bunch of possible chains.
// A stored member is read straight from the backend at its offset. A deflated one
// goes through an rzip_seek index, built as far as reads reach: restart points
// every megabyte and a few decoded spans, instead of the whole member in memory.
class ZipFileLoader : public ProxiedFileLoader {
public:
	ZipFileLoader(FileLoader *sourceLoader);
	~ZipFileLoader() override;

	const ZipContainer &GetZip() const {
		return zip_;
	}

	bool Initialize(int fileIndex);

	bool Exists() override {
		return initialized_;
	}

	bool IsDirectory() override {
		return false;
	}

	s64 FileSize() override {
		return dataFileSize_;
	}

	Path GetPath() const override {
		return backend_->GetPath();
	}

	size_t ReadAt(s64 absolutePos, size_t bytes, size_t count, void *data, Flags flags = Flags::NONE) override {
		return ReadAt(absolutePos, bytes * count, data, flags) / bytes;
	}
	size_t ReadAt(s64 absolutePos, size_t bytes, void *data, Flags flags = Flags::NONE) override;

	std::string GetFileExtension() const override {
		return fileExtension_;
	}

private:
	static int64_t BackendRead(void *userdata, uint64_t off, void *dst, size_t len);

	ZipContainer zip_;
	bool initialized_ = false;
	bool stored_ = false;
	u64 dataOffset_ = 0;          // stored members: where the bytes start in the zip
	struct rzip_seek *seek_ = nullptr;  // deflated members
	s64 dataFileSize_ = 0;
	std::string fileExtension_;
};
