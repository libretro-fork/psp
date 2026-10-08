#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "Common/File/VFS/VFS.h"
#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"

struct rzip_archive;
struct ZipFileSource;

// A zip archive read with libretro-common's rzip_archive. The bytes come through
// a positioned read: from a file opened here, or from the caller's own source.
class ZipContainer {
public:
	// Positioned read of len bytes at off; returns bytes read, or negative on error.
	typedef int64_t (*ReadFunc)(void *userdata, uint64_t off, void *dst, size_t len);

	ZipContainer() noexcept {}
	explicit ZipContainer(const Path &path);
	ZipContainer(ReadFunc read, void *userdata, uint64_t size);
	~ZipContainer();
	ZipContainer(const ZipContainer &) = delete;
	ZipContainer(ZipContainer &&other) noexcept;
	ZipContainer &operator=(const ZipContainer &) = delete;
	ZipContainer &operator=(ZipContainer &&other) noexcept;
	void close() noexcept;

	explicit operator bool() const noexcept { return zip_ != nullptr; }
	rzip_archive *Raw() const noexcept { return zip_; }

	int NumEntries() const;
	const char *Name(int index) const;
	uint64_t Size(int index) const;
	bool IsDirectory(int index) const;
	// Exact match, or ASCII case-insensitive when nocase is set. -1 if absent.
	int Find(std::string_view name, bool nocase = true) const;
	// The member decoded straight into dst, which has room for Size(index) bytes.
	bool ExtractInto(int index, uint8_t *dst, size_t dstSize) const;
	// The member decoded a piece at a time, each piece handed to sink as it comes out
	// (sink returns false to stop), for members too big to hold. The CRC is checked.
	bool ExtractTo(int index, const std::function<bool(const uint8_t *, size_t)> &sink) const;

private:
	void Open(uint64_t size);
	static int64_t FileRead(void *userdata, uint64_t off, void *dst, size_t len);

	rzip_archive *zip_ = nullptr;
	ZipFileSource *source_ = nullptr;
	ReadFunc read_ = nullptr;
	void *userdata_ = nullptr;
	std::unordered_map<std::string, int> lowerIndex_;
};

class ZipFileReader : public VFSBackend {
public:
	static ZipFileReader *Create(const Path &zipFile, std::string_view inZipPath, bool logErrors = true);
	~ZipFileReader();

	bool IsValid() const { return (bool)zip_file_; }

	// use delete[] on the returned value.
	uint8_t *ReadFile(std::string_view path, size_t *size) override;

	VFSFileReference *GetFile(std::string_view path) override;
	bool GetFileInfo(VFSFileReference *vfsReference, File::FileInfo *fileInfo) override;
	void ReleaseFile(VFSFileReference *vfsReference) override;

	VFSOpenFile *OpenFileForRead(VFSFileReference *vfsReference, size_t *size) override;
	void Rewind(VFSOpenFile *vfsOpenFile) override;
	size_t Read(VFSOpenFile *vfsOpenFile, void *buffer, size_t length) override;
	void CloseFile(VFSOpenFile *vfsOpenFile) override;

	bool GetFileListing(std::string_view path, std::vector<File::FileInfo> *listing, const char *filter) override;
	bool GetFileInfo(std::string_view path, File::FileInfo *info) override;
	std::string toString() const override {
		std::string retval = zipPath_.ToVisualString();
		if (!inZipPath_.empty()) {
			retval += ": ";
			retval += inZipPath_;
		}
		return retval;
	}

private:
	ZipFileReader(ZipContainer &&zip, const Path &zipPath, const std::string &inZipPath) : zip_file_(std::move(zip)), inZipPath_(inZipPath), zipPath_(zipPath) {}
	// Path has to be either an empty string, or a string ending with a /.
	bool GetZipListings(const std::string &path, std::set<std::string> &files, std::set<std::string> &directories);

	// The directory is parsed at open and never changes, and every read goes through
	// a handle of the reading thread's own, so readers on any thread need no lock.
	ZipContainer zip_file_;
	std::string inZipPath_;
	Path zipPath_;
};

// When you just want a single file from a ZIP, and don't care about accurate error reporting, use this.
bool ReadSingleFileFromZip(Path zipFile, const char *path, std::string *data);
