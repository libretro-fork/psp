#include <cctype>
#include <set>
#include <algorithm>  // for sort
#include <cstdio>
#include <cstring>
#include <vector>

#include <encodings/crc32.h>
#include <encodings/deflate.h>
#include <zip/rzip_archive.h>

#include "Common/Common.h"
#include "Common/Log.h"
#include "Common/File/VFS/ZipFileReader.h"
#include "Common/StringUtils.h"

// Sanity cap on a single zip entry's declared uncompressed size, so a corrupt or
// malicious zip can't drive an implausible (or, near UINT64_MAX, wrapping) allocation.
static constexpr uint64_t MAX_ZIP_ENTRY_SIZE = 1ULL << 32;  // 4GB, generous for any real asset.

static std::string AsciiLower(std::string_view s) {
	std::string out(s);
	for (char &c : out) {
		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
	}
	return out;
}

// Heap-allocated so the context rzip holds survives moves of the container.
struct ZipFileSource {
	FILE *file;
};

ZipContainer::ZipContainer(const Path &path) {
	FILE *file = File::OpenCFile(path, "rb");
	if (!file)
		return;
	const int64_t size = File::GetFileSize(file);
	source_ = new ZipFileSource{ file };
	read_ = &FileRead;
	userdata_ = source_;
	if (size > 0)
		Open((uint64_t)size);
	if (!zip_)
		close();
}

ZipContainer::ZipContainer(ReadFunc read, void *userdata, uint64_t size) : read_(read), userdata_(userdata) {
	Open(size);
}

void ZipContainer::Open(uint64_t size) {
	rzip_archive_t *zip = nullptr;
	if (rzip_archive_open(&zip, nullptr, size, read_, userdata_) != RZIP_OK)
		return;
	zip_ = zip;
	// One pass to make name lookups cheap. First of any duplicates wins, like libzip.
	const uint32_t count = rzip_archive_num_entries(zip_);
	lowerIndex_.reserve(count);
	for (uint32_t i = 0; i < count; i++)
		lowerIndex_.emplace(AsciiLower(rzip_archive_entry(zip_, i)->name), (int)i);
}

int64_t ZipContainer::FileRead(void *userdata, uint64_t off, void *dst, size_t len) {
	FILE *file = ((ZipFileSource *)userdata)->file;
	if (File::Fseek(file, (int64_t)off, SEEK_SET) != 0)
		return -1;
	return (int64_t)fread(dst, 1, len, file);
}

ZipContainer::ZipContainer(ZipContainer &&other) noexcept {
	*this = std::move(other);
}

ZipContainer &ZipContainer::operator=(ZipContainer &&other) noexcept {
	if (this == &other)
		return *this;
	close();
	std::swap(zip_, other.zip_);
	std::swap(source_, other.source_);
	std::swap(read_, other.read_);
	std::swap(userdata_, other.userdata_);
	lowerIndex_ = std::move(other.lowerIndex_);
	return *this;
}

ZipContainer::~ZipContainer() {
	close();
}

void ZipContainer::close() noexcept {
	if (zip_) {
		rzip_archive_close(zip_);
		zip_ = nullptr;
	}
	if (source_) {
		fclose(source_->file);
		delete source_;
		source_ = nullptr;
	}
	read_ = nullptr;
	userdata_ = nullptr;
	lowerIndex_.clear();
}

int ZipContainer::NumEntries() const {
	return zip_ ? (int)rzip_archive_num_entries(zip_) : 0;
}

const char *ZipContainer::Name(int index) const {
	const rzip_entry_t *e = zip_ ? rzip_archive_entry(zip_, (uint32_t)index) : nullptr;
	return e ? e->name : nullptr;
}

uint64_t ZipContainer::Size(int index) const {
	const rzip_entry_t *e = zip_ ? rzip_archive_entry(zip_, (uint32_t)index) : nullptr;
	return e ? e->size : 0;
}

bool ZipContainer::IsDirectory(int index) const {
	const rzip_entry_t *e = zip_ ? rzip_archive_entry(zip_, (uint32_t)index) : nullptr;
	return e && e->is_dir;
}

int ZipContainer::Find(std::string_view name, bool nocase) const {
	if (!zip_)
		return -1;
	auto it = lowerIndex_.find(AsciiLower(name));
	if (it == lowerIndex_.end())
		return -1;
	if (!nocase && name != Name(it->second))
		return rzip_archive_find(zip_, std::string(name).c_str());
	return it->second;
}

bool ZipContainer::ExtractInto(int index, uint8_t *dst, size_t dstSize) const {
	if (!zip_)
		return false;
	size_t len = 0;
	return rzip_archive_extract_into(zip_, (uint32_t)index, dst, dstSize, &len) == RZIP_OK && len == Size(index);
}

bool ZipContainer::ExtractTo(int index, const std::function<bool(const uint8_t *, size_t)> &sink) const {
	const rzip_entry_t *e = zip_ ? rzip_archive_entry(zip_, (uint32_t)index) : nullptr;
	if (!e || (e->method != RZIP_METHOD_STORED && e->method != RZIP_METHOD_DEFLATE))
		return false;
	const size_t CHUNK = 256 * 1024;
	std::vector<uint8_t> in(CHUNK);
	uint32_t crc = 0;
	uint64_t total = 0;
	uint64_t inPos = 0;
	if (e->method == RZIP_METHOD_STORED) {
		while (inPos < e->csize) {
			const size_t n = (size_t)std::min<uint64_t>(CHUNK, e->csize - inPos);
			if (read_(userdata_, e->data_off + inPos, in.data(), n) != (int64_t)n)
				return false;
			inPos += n;
			crc = encoding_crc32(crc, in.data(), n);
			total += n;
			if (!sink(in.data(), n))
				return false;
		}
		return total == e->size && crc == e->crc;
	}

	void *inf = rinflate_new(-15);
	if (!inf)
		return false;
	std::vector<uint8_t> out(CHUNK);
	bool done = false, ok = true;
	while (ok && !done && inPos < e->csize) {
		const size_t n = (size_t)std::min<uint64_t>(CHUNK, e->csize - inPos);
		if (read_(userdata_, e->data_off + inPos, in.data(), n) != (int64_t)n) {
			ok = false;
			break;
		}
		inPos += n;
		rinflate_set_in(inf, in.data(), n);
		for (;;) {
			rinflate_set_out(inf, out.data(), CHUNK);
			size_t rd = 0, wr = 0;
			const int status = rinflate_process(inf, &rd, &wr);
			if (wr) {
				crc = encoding_crc32(crc, out.data(), wr);
				total += wr;
				if (!sink(out.data(), wr)) {
					ok = false;
					break;
				}
			}
			if (status == RDEFLATE_PROCESS_END) {
				done = true;
				break;
			}
			if (status != RDEFLATE_PROCESS_NEXT) {
				ok = false;
				break;
			}
			// Room left over means this piece of input is used up.
			if (wr < CHUNK)
				break;
		}
	}
	rinflate_free(inf);
	return ok && done && total == e->size && crc == e->crc;
}

ZipFileReader *ZipFileReader::Create(const Path &zipFile, std::string_view inZipPath, bool logErrors) {
	// The inZipPath is supposed to be a folder, and internally in this class, we suffix
	// folder paths with '/', matching how zip files name things.
	std::string path(inZipPath);
	if (!path.empty() && path.back() != '/') {
		path.push_back('/');
	}

	ZipContainer zip(zipFile);
	if (!zip) {
		if (logErrors) {
			ERROR_LOG(Log::IO, "Failed to open %s as a zip file", zipFile.c_str());
		}
		return nullptr;
	}

	return new ZipFileReader(std::move(zip), zipFile, path);
}

ZipFileReader::~ZipFileReader() {
	std::lock_guard<std::mutex> guard(lock_);
	zip_file_.close();
}

uint8_t *ZipFileReader::ReadFile(std::string_view path, size_t *size) {
	std::string temp_path = join(inZipPath_, path);

	const int index = zip_file_.Find(temp_path);
	if (index < 0) {
		ERROR_LOG(Log::IO, "Error opening %s from ZIP", temp_path.c_str());
		return 0;
	}
	const uint64_t entrySize = zip_file_.Size(index);
	// Sanity check the declared size before trusting it for an allocation.
	if (entrySize > MAX_ZIP_ENTRY_SIZE) {
		ERROR_LOG(Log::IO, "Zip entry %s claims an implausible size (%llu), refusing to read", temp_path.c_str(), (unsigned long long)entrySize);
		return 0;
	}
	uint8_t *contents = new uint8_t[entrySize + 1];
	bool ok;
	{
		std::lock_guard<std::mutex> guard(lock_);
		ok = zip_file_.ExtractInto(index, contents, (size_t)entrySize);
	}
	if (!ok) {
		ERROR_LOG(Log::IO, "Error reading %s from ZIP", temp_path.c_str());
		delete[] contents;
		return 0;
	}
	contents[entrySize] = 0;
	*size = (size_t)entrySize;
	return contents;
}

bool ZipFileReader::GetFileListing(std::string_view orig_path, std::vector<File::FileInfo> *listing, const char *filter = 0) {
	std::string path = join(inZipPath_, orig_path);
	if (!path.empty() && path.back() != '/') {
		path.push_back('/');
	}

	std::set<std::string> filters;
	std::string tmp;
	if (filter) {
		while (*filter) {
			if (*filter == ':') {
				filters.emplace("." + tmp);
				tmp.clear();
			} else {
				tmp.push_back(*filter);
			}
			filter++;
		}
	}

	if (tmp.size())
		filters.emplace("." + tmp);

	// We just loop through the whole ZIP file and deduce what files are in this directory, and what subdirectories there are.
	std::set<std::string> files;
	std::set<std::string> directories;
	bool success = GetZipListings(path, files, directories);
	if (!success) {
		// This means that no file prefix matched the path.
		return false;
	}

	listing->clear();

	const std::string relativePath = path.substr(inZipPath_.size());

	listing->reserve(directories.size() + files.size());
	for (const auto &dir : directories) {
		File::FileInfo info;
		info.name = dir;

		// Remove the "inzip" part of the fullname.
		info.fullName = Path(relativePath + dir);
		info.exists = true;
		info.isWritable = false;
		info.isDirectory = true;
		listing->push_back(info);
	}

	for (const auto &fiter : files) {
		File::FileInfo info;
		info.name = fiter;
		info.fullName = Path(relativePath + fiter);
		info.exists = true;
		info.isWritable = false;
		info.isDirectory = false;
		std::string ext = info.fullName.GetFileExtension();
		if (filter) {
			if (filters.find(ext) == filters.end()) {
				continue;
			}
		}
		listing->push_back(info);
	}

	std::sort(listing->begin(), listing->end());
	return true;
}

// path here is from the root, so inZipPath needs to already be added.
bool ZipFileReader::GetZipListings(const std::string &path, std::set<std::string> &files, std::set<std::string> &directories) {
	_dbg_assert_(path.empty() || path.back() == '/');

	// The directory is parsed at open and never changes, so this needs no lock.
	const int numFiles = zip_file_.NumEntries();
	bool anyPrefixMatched = false;
	for (int i = 0; i < numFiles; i++) {
		const char *name = zip_file_.Name(i);
		if (!name)
			continue;  // shouldn't happen, I think
		if (startsWith(name, path)) {
			if (strlen(name) == path.size()) {
				// Don't want to return the same folder.
				continue;
			}
			const char *slashPos = strchr(name + path.size(), '/');
			if (slashPos != 0) {
				anyPrefixMatched = true;
				// A directory. Let's pick off the only part we care about.
				size_t offset = path.size();
				std::string dirName = std::string(name + offset, slashPos - (name + offset));
				// We might get a lot of these if the tree is deep. The std::set deduplicates.
				directories.insert(dirName);
			} else {
				anyPrefixMatched = true;
				// It's a file.
				const char *fn = name + path.size();
				files.emplace(fn);
			}
		}
	}
	return anyPrefixMatched;
}

bool ZipFileReader::GetFileInfo(std::string_view path, File::FileInfo *info) {
	std::string temp_path = join(inZipPath_, path);

	// Clear some things to start.
	info->isDirectory = false;
	info->isWritable = false;
	info->size = 0;

	const int index = zip_file_.Find(temp_path);
	if (index < 0) {
		// ZIP files do not have real directories, so we'll end up here if we
		// try to stat one. For now that's fine.
		info->exists = false;
		return false;
	}

	// Zips usually don't contain directory entries, but they may.
	info->isDirectory = zip_file_.IsDirectory(index);
	info->size = zip_file_.Size(index);
	info->fullName = Path(path);
	info->exists = true;
	return true;
}

class ZipFileReaderFileReference : public VFSFileReference {
public:
	int zi;
};

// Members are decoded whole: straight into the caller's buffer when the first
// read asks for all of it, which is how files are read here, else into data.
class ZipFileReaderOpenFile : public VFSOpenFile {
public:
	ZipFileReaderFileReference *reference;
	size_t size = 0;
	size_t pos = 0;
	bool decoded = false;
	std::vector<uint8_t> data;
};

VFSFileReference *ZipFileReader::GetFile(std::string_view path) {
	int zi = zip_file_.Find(path);
	if (zi < 0) {
		// Not found.
		return nullptr;
	}
	ZipFileReaderFileReference *ref = new ZipFileReaderFileReference();
	ref->zi = zi;
	return ref;
}

bool ZipFileReader::GetFileInfo(VFSFileReference *vfsReference, File::FileInfo *fileInfo) {
	ZipFileReaderFileReference *reference = (ZipFileReaderFileReference *)vfsReference;
	*fileInfo = File::FileInfo{};
	fileInfo->size = zip_file_.Size(reference->zi);
	return fileInfo->size != 0;
}

void ZipFileReader::ReleaseFile(VFSFileReference *vfsReference) {
	ZipFileReaderFileReference *reference = (ZipFileReaderFileReference *)vfsReference;
	// Don't do anything other than deleting it.
	delete reference;
}

VFSOpenFile *ZipFileReader::OpenFileForRead(VFSFileReference *vfsReference, size_t *size) {
	ZipFileReaderFileReference *reference = (ZipFileReaderFileReference *)vfsReference;
	*size = 0;
	const uint64_t entrySize = zip_file_.Size(reference->zi);
	if (entrySize > MAX_ZIP_ENTRY_SIZE) {
		WARN_LOG(Log::G3D, "File with index %d in zip has an implausible size", reference->zi);
		return nullptr;
	}
	ZipFileReaderOpenFile *openFile = new ZipFileReaderOpenFile();
	openFile->reference = reference;
	openFile->size = (size_t)entrySize;
	*size = openFile->size;
	return openFile;
}

void ZipFileReader::Rewind(VFSOpenFile *vfsOpenFile) {
	ZipFileReaderOpenFile *file = (ZipFileReaderOpenFile *)vfsOpenFile;
	_assert_(file);
	file->pos = 0;
}

size_t ZipFileReader::Read(VFSOpenFile *vfsOpenFile, void *buffer, size_t length) {
	ZipFileReaderOpenFile *file = (ZipFileReaderOpenFile *)vfsOpenFile;
	_assert_(file);
	if (file->pos >= file->size)
		return 0;
	if (!file->decoded && file->pos == 0 && length >= file->size) {
		// The whole member in one read: decode it where it's going.
		std::lock_guard<std::mutex> guard(lock_);
		if (!zip_file_.ExtractInto(file->reference->zi, (uint8_t *)buffer, file->size))
			return 0;
		file->pos = file->size;
		return file->size;
	}
	if (!file->decoded) {
		file->data.resize(file->size);
		std::lock_guard<std::mutex> guard(lock_);
		if (!zip_file_.ExtractInto(file->reference->zi, file->data.data(), file->size))
			return 0;
		file->decoded = true;
	}
	const size_t n = std::min(length, file->size - file->pos);
	memcpy(buffer, file->data.data() + file->pos, n);
	file->pos += n;
	return n;
}

void ZipFileReader::CloseFile(VFSOpenFile *vfsOpenFile) {
	ZipFileReaderOpenFile *file = (ZipFileReaderOpenFile *)vfsOpenFile;
	_assert_(file);
	delete file;
}

bool ReadSingleFileFromZip(Path zipFile, const char *path, std::string *data, std::mutex *mutex) {
	ZipContainer zip(zipFile);
	if (!zip) {
		return false;
	}

	const int index = zip.Find(path);
	if (index < 0) {
		return false;
	}
	const uint64_t size = zip.Size(index);
	if (size > MAX_ZIP_ENTRY_SIZE) {
		ERROR_LOG(Log::IO, "Zip entry %s claims an implausible size (%llu), refusing to read", path, (unsigned long long)size);
		return false;
	}
	std::string contents;
	contents.resize((size_t)size);
	if (!zip.ExtractInto(index, (uint8_t *)&contents[0], contents.size())) {
		return false;
	}
	if (mutex) {
		mutex->lock();
	}
	data->swap(contents);
	if (mutex) {
		mutex->unlock();
	}
	return true;
}
