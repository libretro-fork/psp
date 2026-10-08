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
#include <array>
#include <cstring>
#include <limits>
#include <vector>

#include "Common/Data/Text/I18n.h"
#include "Common/System/OSD.h"
#include "Common/Log.h"
#include "Common/Swap.h"
#include "Common/Data/Text/Parsers.h"
#include "Common/File/FileUtil.h"
#include "Common/File/DirListing.h"
#include "Common/StringUtils.h"
#include "Core/Loaders.h"
#include "Core/FileSystems/BlockDevices.h"
#include "Core/FileSystems/ISOFileSystem.h"
#include "Core/Util/PathUtil.h"
#include <formats/rchd.h>

#include <encodings/deflate.h>

#include "Common/Data/Encoding/Compression.h"
#include "ext/libkirk/amctrl.h"
#include "ext/libkirk/kirk_engine.h"

static u16 ReadLE16(const u8 *ptr) {
	return ptr[0] | (ptr[1] << 8);
}

static u32 ReadLE32(const u8 *ptr) {
	return ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
}

static std::string DecodeUDFFileName(const u8 *data, size_t size) {
	if (size == 0)
		return "";

	std::string result;
	if (data[0] == 8) {
		result.assign((const char *)(data + 1), size - 1);
	} else if (data[0] == 16) {
		for (size_t i = 1; i + 1 < size; i += 2) {
			result.push_back((char)data[i + 1]);
		}
	}
	return result;
}

struct UDFShortAd {
	u32 length = 0;
	u32 position = 0;
};

struct UDFLongAd {
	u32 length = 0;
	u32 position = 0;
	u16 partition = 0;
};

static bool ReadDescriptorSector(FileLoader *fileLoader, u32 sector, std::array<u8, 2048> *out) {
	return fileLoader->ReadAt((u64)sector * 2048, 1, out->size(), out->data()) == out->size();
}

static bool ParseUDFLongAd(const u8 *data, UDFLongAd *out) {
	out->length = ReadLE32(data);
	out->position = ReadLE32(data + 4);
	out->partition = ReadLE16(data + 8);
	return true;
}

static bool ParseUDFShortAd(const u8 *data, UDFShortAd *out) {
	out->length = ReadLE32(data) & 0x3FFFFFFF;
	out->position = ReadLE32(data + 4);
	return out->length != 0;
}

static bool ParseUDFFileEntryExtent(FileLoader *fileLoader, u32 sector, UDFShortAd *extent) {
	std::array<u8, 2048> block{};
	if (!ReadDescriptorSector(fileLoader, sector, &block))
		return false;
	if (ReadLE16(block.data()) != 0x0105)
		return false;

	u32 extAttrLen = ReadLE32(block.data() + 0xA8);
	u32 allocDescLen = ReadLE32(block.data() + 0xAC);
	u32 allocDescOffset = 0xB0 + extAttrLen;
	if (allocDescLen < 8 || allocDescOffset + 8 > block.size())
		return false;

	return ParseUDFShortAd(block.data() + allocDescOffset, extent);
}

static bool ParseUDFRootDirectory(FileLoader *fileLoader, u32 sector, u32 partitionStart, std::vector<u8> *dirData) {
	UDFShortAd extent{};
	if (!ParseUDFFileEntryExtent(fileLoader, sector, &extent))
		return false;
	if (extent.length == 0)
		return false;

	dirData->resize(extent.length);
	const u64 offset = (u64)(partitionStart + extent.position) * 2048;
	return fileLoader->ReadAt(offset, 1, dirData->size(), dirData->data()) == dirData->size();
}

static bool FindUDFRootFileEntry(FileLoader *fileLoader, u32 *rootSector, u32 *partitionStart) {
	std::array<u8, 2048> avdp{};
	if (!ReadDescriptorSector(fileLoader, 256, &avdp))
		return false;
	if (ReadLE16(avdp.data()) != 0x0002)
		return false;

	u32 mvdsLength = ReadLE32(avdp.data() + 16);
	u32 mvdsLocation = ReadLE32(avdp.data() + 20);
	if (mvdsLength < 2048)
		return false;

	std::array<u8, 2048> block{};
	bool foundPartition = false;
	bool foundRoot = false;
	u32 fsdLocation = 0;
	u32 fsdPartition = 0;

	for (u32 sector = mvdsLocation; sector < mvdsLocation + mvdsLength / 2048; ++sector) {
		if (!ReadDescriptorSector(fileLoader, sector, &block))
			return false;

		switch (ReadLE16(block.data())) {
		case 0x0005:
			// Partition Descriptor.
			fsdPartition = ReadLE32(block.data() + 188);
			foundPartition = true;
			break;
		case 0x0006:
			// Logical Volume Descriptor. The file set descriptor sequence is stored
			// in logicalVolumeContentsUse as an extent_ad.
			fsdLocation = ReadLE32(block.data() + 252);
			foundRoot = true;
			break;
		default:
			break;
		}
	}

	if (!foundPartition || !foundRoot)
		return false;

	std::array<u8, 2048> fsd{};
	if (!ReadDescriptorSector(fileLoader, fsdPartition + fsdLocation, &fsd))
		return false;
	if (ReadLE16(fsd.data()) != 0x0100)
		return false;

	UDFLongAd rootIcb{};
	ParseUDFLongAd(fsd.data() + 400, &rootIcb);
	if (rootIcb.partition != 0)
		return false;

	*partitionStart = fsdPartition;
	*rootSector = fsdPartition + rootIcb.position;
	return true;
}

static bool FindUDFLayerFileEntrySectors(FileLoader *fileLoader, u32 rootSector, u32 partitionStart, u32 *layer0Sector, u32 *layer1Sector) {
	std::vector<u8> dirData;
	if (!ParseUDFRootDirectory(fileLoader, rootSector, partitionStart, &dirData))
		return false;

	bool found0 = false;
	bool found1 = false;
	for (size_t offset = 0; offset + 16 <= dirData.size();) {
		u16 tag = ReadLE16(&dirData[offset]);
		u16 crcLen = ReadLE16(&dirData[offset + 10]);
		size_t entryLen = 16 + crcLen;
		entryLen = (entryLen + 3) & ~size_t(3);
		if (entryLen == 0 || offset + entryLen > dirData.size())
			break;

		if (tag == 0x0101 && crcLen >= 20) {
			u8 fileIdLen = dirData[offset + 19];
			u16 implUseLen = ReadLE16(&dirData[offset + 36]);
			size_t nameOffset = offset + 38 + implUseLen;
			if (nameOffset + fileIdLen <= dirData.size()) {
				std::string name = DecodeUDFFileName(&dirData[nameOffset], fileIdLen);
				UDFLongAd icb{};
				ParseUDFLongAd(&dirData[offset + 20], &icb);
				if (icb.partition == 0) {
					if (name == "USER_L0.IMG") {
						*layer0Sector = partitionStart + icb.position;
						found0 = true;
					} else if (name == "USER_L1.IMG") {
						*layer1Sector = partitionStart + icb.position;
						found1 = true;
					}
				}
			}
		}

		offset += entryLen;
	}

	return found0 || found1;
}

BlockDevice *ConstructBlockDevice(FileLoader *fileLoader, std::string *errorString) {
	if (!fileLoader->Exists()) {
		// Shouldn't get here really.
		*errorString = "File not readable or doesn't exist";
		return nullptr;
	}
	if (fileLoader->IsDirectory()) {
		*errorString = "Can't open directory directly as block device: ";
		*errorString += fileLoader->GetPath().ToString();
		return nullptr;
	}
	if (fileLoader->FileSize() < 8) {
		*errorString = "File is too small to read the header.";
		return nullptr;
	}

	char buffer[8]{};
	size_t size = fileLoader->ReadAt(0, 1, 8, buffer);
	if (size != 8) {
		// Bad or empty file
		*errorString = "Failed to read 8-byte header";
		return nullptr;
	}

	BlockDevice *device = nullptr;

	// Check for CISO
	if (!memcmp(buffer, "CISO", 4)) {
		device = new CISOFileBlockDevice(fileLoader);
	} else if (!memcmp(buffer, "\x00PBP", 4)) {
		uint32_t psarOffset = 0;
		size = fileLoader->ReadAt(0x24, 1, 4, &psarOffset);
		if (size == 4 && psarOffset < fileLoader->FileSize()) {
			device = new NPDRMDemoBlockDevice(fileLoader);
		}
	} else if (!memcmp(buffer, "MComprHD", 8)) {
		device = new CHDFileBlockDevice(fileLoader);
	}

	if (!device) {
		device = new UDFFileBlockDevice(fileLoader);
		if (!device->IsOK()) {
			delete device;
			device = nullptr;
		}
	}

	if (!device) {
		device = new ISOContainerFileBlockDevice(fileLoader);
		if (!device->IsOK()) {
			delete device;
			device = nullptr;
		}
	}

	// No check above passed, should be just a regular ISO file. Let's open it as a plain block device and let the other systems take over.
	if (!device) {
		device = new FileBlockDevice(fileLoader);
	}

	if (!device->IsOK()) {
		*errorString = device->ErrorString();
		delete device;
		return nullptr;
	}

	return device;
}

void BlockDevice::NotifyReadError() {
	if (!reportedError_) {
		auto err = GetI18NCategory(I18NCat::ERRORS);
		g_OSD.Show(OSDType::MESSAGE_WARNING, err->T("Game disc read error - ISO corrupt"), GetFriendlyPath(fileLoader_->GetPath()), 6.0f);
		reportedError_ = true;
	}
}

FileBlockDevice::FileBlockDevice(FileLoader *fileLoader)
	: BlockDevice(fileLoader) {
	filesize_ = fileLoader->FileSize();
}

FileBlockDevice::~FileBlockDevice() {}

bool FileBlockDevice::ReadBlock(int blockNumber, u8 *outPtr, bool uncached) {
	FileLoader::Flags flags = uncached ? FileLoader::Flags::HINT_UNCACHED : FileLoader::Flags::NONE;
	const u64 offset = (u64)blockNumber * (u64)GetBlockSize();
	size_t retval = fileLoader_->ReadAt(offset, 1, 2048, outPtr, flags);
	if (retval != 2048) {
		// Not every image is a whole number of sectors. Tools that build pre-patched ISOs do write
		// images that stop in the middle of their last sector, with a file legitimately ending
		// there. The bytes that are present are real, so zero the rest of the sector and report
		// success. Failing instead loses them: callers substitute an all-zero sector, which
		// quietly corrupts whatever was in that tail.
		if (retval > 0 && offset < filesize_) {
			memset(outPtr + retval, 0, 2048 - retval);
			return true;
		}
		DEBUG_LOG(Log::FileSystem, "Could not read 2048 byte block, at block offset %d. Only got %d bytes", blockNumber, (int)retval);
		return false;
	}
	return true;
}

bool BlockDevice::ReadBytes(u64 offset, size_t len, u8 *out) {
	const size_t blockSize = GetBlockSize();
	u32 block = (u32)(offset / blockSize);
	const size_t skip = (size_t)(offset % blockSize);
	bool ok = true;
	if (skip != 0 && len > 0) {
		u8 sector[2048];
		const size_t n = std::min(len, blockSize - skip);
		// A failed read must not hand out uninitialized stack.
		if (!ReadBlock(block++, sector)) {
			memset(sector, 0, sizeof(sector));
			ok = false;
		}
		memcpy(out, sector + skip, n);
		out += n;
		len -= n;
	}
	if (len >= blockSize) {
		const u32 count = (u32)(len / blockSize);
		ok = ReadBlocks(block, count, out) && ok;
		block += count;
		out += (size_t)count * blockSize;
		len -= (size_t)count * blockSize;
	}
	if (len > 0) {
		u8 sector[2048];
		if (!ReadBlock(block, sector)) {
			memset(sector, 0, sizeof(sector));
			ok = false;
		}
		memcpy(out, sector, len);
	}
	return ok;
}

bool FileBlockDevice::ReadBytes(u64 offset, size_t len, u8 *out) {
	// A plain image is the bytes themselves: one read, no sector staging.
	const size_t got = fileLoader_->ReadAt(offset, len, out);
	if (got < len) {
		memset(out + got, 0, len - got);
		return false;
	}
	return true;
}

bool FileBlockDevice::ReadBlocks(u32 minBlock, int count, u8 *outPtr) {
	size_t retval = fileLoader_->ReadAt((u64)minBlock * (u64)GetBlockSize(), 2048, count, outPtr);
	if (retval != (size_t)count) {
		ERROR_LOG(Log::FileSystem, "Could not read %d blocks, at block offset %d. Only got %d blocks", count, minBlock, (int)retval);
		return false;
	}
	return true;
}

UDFFileBlockDevice::UDFFileBlockDevice(FileLoader *fileLoader)
	: BlockDevice(fileLoader) {
	u32 partitionStart = 0;
	u32 rootSector = 0;
	if (!FindUDFRootFileEntry(fileLoader, &rootSector, &partitionStart)) {
		errorString_ = "Not a supported UDF disc image";
		return;
	}

	u32 layer0Sector = 0;
	u32 layer1Sector = 0;
	if (!FindUDFLayerFileEntrySectors(fileLoader, rootSector, partitionStart, &layer0Sector, &layer1Sector) || layer0Sector == 0) {
		errorString_ = "Not a PSP UDF disc image";
		return;
	}

	UDFShortAd layer0Extent{};
	if (!ParseUDFFileEntryExtent(fileLoader, layer0Sector, &layer0Extent)) {
		errorString_ = "Failed to read USER_L0.IMG entry";
		return;
	}
	layer0_.startBlock = partitionStart + layer0Extent.position;
	layer0_.numBlocks = layer0Extent.length / GetBlockSize();
	numBlocks_ = layer0_.numBlocks;

	if (layer1Sector != 0) {
		UDFShortAd layer1Extent{};
		if (ParseUDFFileEntryExtent(fileLoader, layer1Sector, &layer1Extent)) {
			layer1_.startBlock = partitionStart + layer1Extent.position;
			layer1_.numBlocks = layer1Extent.length / GetBlockSize();
			numBlocks_ += layer1_.numBlocks;
		}
	}

	if (numBlocks_ == 0) {
		errorString_ = "UDF disc image had no readable UMD layers";
		return;
	}

	DEBUG_LOG(Log::Loader, "Detected PSP DVD-R wrapper: USER_L0=%u blocks at %u, USER_L1=%u blocks at %u",
		layer0_.numBlocks, layer0_.startBlock, layer1_.numBlocks, layer1_.startBlock);
}

UDFFileBlockDevice::~UDFFileBlockDevice() = default;

bool UDFFileBlockDevice::ReadBlock(int blockNumber, u8 *outPtr, bool uncached) {
	if ((u32)blockNumber >= numBlocks_) {
		memset(outPtr, 0, GetBlockSize());
		return false;
	}

	u32 sourceBlock = 0;
	if ((u32)blockNumber < layer0_.numBlocks) {
		sourceBlock = layer0_.startBlock + blockNumber;
	} else {
		u32 layer1Block = (u32)blockNumber - layer0_.numBlocks;
		if (layer1Block >= layer1_.numBlocks) {
			memset(outPtr, 0, GetBlockSize());
			return false;
		}
		sourceBlock = layer1_.startBlock + layer1Block;
	}

	FileLoader::Flags flags = uncached ? FileLoader::Flags::HINT_UNCACHED : FileLoader::Flags::NONE;
	size_t retval = fileLoader_->ReadAt((u64)sourceBlock * (u64)GetBlockSize(), 1, GetBlockSize(), outPtr, flags);
	if (retval != GetBlockSize()) {
		DEBUG_LOG(Log::FileSystem, "Could not read UDF-wrapped block %d from source block %u", blockNumber, sourceBlock);
		return false;
	}
	return true;
}

bool UDFFileBlockDevice::ReadBlocks(u32 minBlock, int count, u8 *outPtr) {
	for (int i = 0; i < count; ++i) {
		if (!ReadBlock(minBlock + i, outPtr)) {
			return false;
		}
		outPtr += GetBlockSize();
	}
	return true;
}

ISOContainerFileBlockDevice::ISOContainerFileBlockDevice(FileLoader *fileLoader)
	: BlockDevice(fileLoader) {
	outerBlockDevice_ = std::make_shared<FileBlockDevice>(fileLoader);
	if (!outerBlockDevice_->IsOK()) {
		errorString_ = outerBlockDevice_->ErrorString();
		outerBlockDevice_.reset();
		return;
	}

	SequentialHandleAllocator alloc;
	ISOFileSystem iso(&alloc, outerBlockDevice_);
	if (!iso.Error().empty()) {
		errorString_ = iso.Error();
		outerBlockDevice_.reset();
		return;
	}

	PSPFileInfo layer0Info = iso.GetFileInfo("/USER_L0.IMG");
	if (!layer0Info.exists) {
		errorString_ = "Not a PSP ISO container image";
		outerBlockDevice_.reset();
		return;
	}

	layer0_.startBlock = layer0Info.startSector;
	layer0_.numBlocks = (u32)((layer0Info.size + GetBlockSize() - 1) / GetBlockSize());
	numBlocks_ = layer0_.numBlocks;

	PSPFileInfo layer1Info = iso.GetFileInfo("/USER_L1.IMG");
	if (layer1Info.exists) {
		layer1_.startBlock = layer1Info.startSector;
		layer1_.numBlocks = (u32)((layer1Info.size + GetBlockSize() - 1) / GetBlockSize());
		numBlocks_ += layer1_.numBlocks;
	}

	if (numBlocks_ == 0) {
		errorString_ = "ISO container image had no readable UMD layers";
		outerBlockDevice_.reset();
		return;
	}

	DEBUG_LOG(Log::Loader, "Detected PSP ISO wrapper: USER_L0=%u blocks at %u, USER_L1=%u blocks at %u",
		layer0_.numBlocks, layer0_.startBlock, layer1_.numBlocks, layer1_.startBlock);
}

ISOContainerFileBlockDevice::~ISOContainerFileBlockDevice() = default;

bool ISOContainerFileBlockDevice::ReadBlock(int blockNumber, u8 *outPtr, bool uncached) {
	if ((u32)blockNumber >= numBlocks_ || !outerBlockDevice_) {
		memset(outPtr, 0, GetBlockSize());
		return false;
	}

	u32 sourceBlock = 0;
	if ((u32)blockNumber < layer0_.numBlocks) {
		sourceBlock = layer0_.startBlock + blockNumber;
	} else {
		u32 layer1Block = (u32)blockNumber - layer0_.numBlocks;
		if (layer1Block >= layer1_.numBlocks) {
			memset(outPtr, 0, GetBlockSize());
			return false;
		}
		sourceBlock = layer1_.startBlock + layer1Block;
	}

	return outerBlockDevice_->ReadBlock(sourceBlock, outPtr, uncached);
}

bool ISOContainerFileBlockDevice::ReadBlocks(u32 minBlock, int count, u8 *outPtr) {
	for (int i = 0; i < count; ++i) {
		if (!ReadBlock(minBlock + i, outPtr)) {
			return false;
		}
		outPtr += GetBlockSize();
	}
	return true;
}

// .CSO format

// compressed ISO(9660) header format
typedef struct ciso_header
{
	unsigned char magic[4];         // +00 : 'C','I','S','O'
	u32_le header_size;             // +04 : header size (==0x18)
	u64_le total_bytes;             // +08 : number of original data size
	u32_le block_size;              // +10 : number of compressed block size
	unsigned char ver;              // +14 : version 01
	unsigned char align;            // +15 : align of index value
	unsigned char rsv_06[2];        // +16 : reserved
#if 0
	// INDEX BLOCK
	unsigned int index[0];          // +18 : block[0] index
	unsigned int index[1];          // +1C : block[1] index
	:
	:
	unsigned int index[last];       // +?? : block[last]
	unsigned int index[last+1];     // +?? : end of last data point
	// DATA BLOCK
	unsigned char data[];           // +?? : compressed or plain sector data
#endif
} CISO_H;


// TODO: Need much better error handling.

static const u32 CSO_READ_BUFFER_SIZE = 256 * 1024;
// The frame size decides how big readBuffer and zlibBuffer are, straight from the header, so
// without a ceiling a 96-byte file can ask us for a couple of gigabytes. Real images use 2KB
// through 64KB; this leaves a lot of room above that and still bounds what a header can cost us.
static const u32 CSO_MAX_FRAME_SIZE = 16 * 1024 * 1024;

CISOFileBlockDevice::CISOFileBlockDevice(FileLoader *fileLoader)
	: BlockDevice(fileLoader)
{
	// CISO format is fairly simple, but most tools do not write the header_size.
	// NOTE: CSOv2 isn't actually a thing. It was partially implemented in maxcso but it has never been in active use.

	CISO_H hdr;
	size_t readSize = fileLoader->ReadAt(0, sizeof(CISO_H), 1, &hdr);
	if (readSize != 1 || memcmp(hdr.magic, "CISO", 4) != 0) {
		errorString_ = "Invalid CSO!";
		return;
	}
	if (hdr.ver > 1) {
		errorString_ = "CSO version too high!";
		return;
	}

	frameSize = hdr.block_size;
	if ((frameSize & (frameSize - 1)) != 0) {
		errorString_ = StringFromFormat("CSO block size %i unsupported, must be a power of two", frameSize);
		return;
	} else if (frameSize < 0x800) {
		errorString_ = StringFromFormat("CSO block size %i unsupported, must be at least one sector", frameSize);
		return;
	} else if (frameSize > CSO_MAX_FRAME_SIZE) {
		errorString_ = StringFromFormat("CSO block size %u unsupported, too large", frameSize);
		return;
	}

	// Determine the translation from block to frame.
	blockShift = 0;
	for (u32 i = frameSize; i > 0x800; i >>= 1)
		++blockShift;

	indexShift = hdr.align;
	// Sanity check the index shift: index entries are u32 masked to 31 bits,
	// and are shifted up by indexShift to get byte offsets.  Values above a
	// couple dozen would be bogus and could overflow the buffer size math.
	if (indexShift > 20) {
		errorString_ = StringFromFormat("CSO index alignment %i unsupported", indexShift);
		return;
	}
	const u64 totalSize = hdr.total_bytes;
	// Compute the counts without overflowing the 64-bit total size, then
	// validate them before truncating to the 32-bit fields used by the CSO
	// block reader. In particular, numFrames + 1 must remain representable.
	const u64 numFrames64 = totalSize / frameSize + (totalSize % frameSize != 0);
	const u64 numBlocks64 = totalSize / GetBlockSize();
	if (numFrames64 >= 0xFFFFFFFFull || numBlocks64 > 0xFFFFFFFFull) {
		errorString_ = "Invalid CSO header (image is too large)";
		return;
	}
	numFrames = (u32)numFrames64;
	numBlocks = (u32)numBlocks64;
	VERBOSE_LOG(Log::Loader, "CSO numBlocks=%i numFrames=%i align=%i", numBlocks, numFrames, indexShift);

	// numFrames and numBlocks are independently truncated to 32 bits from the same
	// attacker-controlled 64-bit total_bytes, using different divisors (frameSize vs.
	// the fixed 2048-byte block size). With extreme total_bytes/block_size values these
	// can disagree so that numBlocks describes more blocks than numFrames actually has
	// frames for - ReadBlock() would then index the numFrames+1-sized `index` array
	// (via frameNumber+1, with frameNumber derived from a blockNumber < numBlocks) out
	// of bounds. Reject any header where that could happen.
	if ((u64)numBlocks > (u64)numFrames << blockShift) {
		errorString_ = "Invalid CSO header (block/frame size mismatch)";
		return;
	}

	const size_t headerEnd = hdr.ver > 1 ? (size_t)hdr.header_size : sizeof(hdr);
	const u64 indexSize64 = numFrames64 + 1;
	const u64 indexBytes = indexSize64 * sizeof(u32);
	const u64 fileSize = fileLoader->FileSize();
	if (indexBytes > (u64)std::numeric_limits<size_t>::max() ||
		headerEnd > fileSize || indexBytes > fileSize - headerEnd) {
		errorString_ = "Invalid CSO header (index table is truncated)";
		return;
	}

	// We might read a bit of alignment too, so be prepared.
	readBufferSize = frameSize + (1u << indexShift);
	if (readBufferSize < CSO_READ_BUFFER_SIZE)
		readBufferSize = CSO_READ_BUFFER_SIZE;
	readBuffer = new u8[readBufferSize];
	zlibBuffer = new u8[frameSize + (1u << indexShift)];
	inflate_ = rinflate_new(-15);
	zlibBufferFrame = numFrames;

	const u32 indexSize = (u32)indexSize64;

#if COMMON_LITTLE_ENDIAN
	index = new u32[indexSize];
	if (fileLoader->ReadAt(headerEnd, sizeof(u32), indexSize, index) != indexSize) {
		NotifyReadError();
		memset(index, 0, indexSize * sizeof(u32));
	}
#else
	index = new u32[indexSize];
	u32_le *indexTemp = new u32_le[indexSize];

	if (fileLoader->ReadAt(headerEnd, sizeof(u32), indexSize, indexTemp) != indexSize) {
		NotifyReadError();
		memset(indexTemp, 0, indexSize * sizeof(u32_le));
	}

	for (u32 i = 0; i < indexSize; i++)
		index[i] = indexTemp[i];

	delete[] indexTemp;
#endif

	ver_ = hdr.ver;

	// Double check that the CSO is not truncated.  In most cases, this will be the exact size.
	u64 lastIndexPos = index[indexSize - 1] & 0x7FFFFFFF;
	u64 expectedFileSize = lastIndexPos << indexShift;
	if (expectedFileSize > fileSize) {
		errorString_ = StringFromFormat("CSO file incomplete: expected %s, but is %s", NiceSizeFormat(expectedFileSize).c_str(), NiceSizeFormat(fileSize).c_str());
		return;
	}

	// Index entries must be monotonically non-decreasing, otherwise ReadBlock()
	// would compute a negative (underflowed) compressed read size from two
	// adjacent entries.  Reject such files rather than reading into a fixed buffer.
	for (u32 i = 0; i < indexSize - 1; i++) {
		if ((index[i] & 0x7FFFFFFF) > (index[i + 1] & 0x7FFFFFFF)) {
			errorString_ = StringFromFormat("CSO index is not monotonic at entry %d", i);
			return;
		}
	}

	// all ok.
	_dbg_assert_(errorString_.empty());
}

CISOFileBlockDevice::~CISOFileBlockDevice()
{
	delete [] index;
	delete [] readBuffer;
	delete [] zlibBuffer;
	if (inflate_)
		rinflate_free(inflate_);
}

bool CISOFileBlockDevice::ReadBlock(int blockNumber, u8 *outPtr, bool uncached)
{
	FileLoader::Flags flags = uncached ? FileLoader::Flags::HINT_UNCACHED : FileLoader::Flags::NONE;
	if ((u32)blockNumber >= numBlocks) {
		memset(outPtr, 0, GetBlockSize());
		return false;
	}

	const u32 frameNumber = blockNumber >> blockShift;
	const u32 idx = index[frameNumber];
	const u32 indexPos = idx & 0x7FFFFFFF;
	const u32 nextIndexPos = index[frameNumber + 1] & 0x7FFFFFFF;

	const u64 compressedReadPos = (u64)indexPos << indexShift;
	const u64 compressedReadEnd = (u64)nextIndexPos << indexShift;
	// A single frame's compressed data must fit in readBuffer.  Guard against
	// crafted index entries with huge gaps (index[i+1] >> index[i]).
	const size_t compressedReadSize = std::min<size_t>((size_t)(compressedReadEnd - compressedReadPos), readBufferSize);
	const u32 compressedOffset = (blockNumber & ((1 << blockShift) - 1)) * GetBlockSize();

	bool plain = (idx & 0x80000000) != 0;
	if (ver_ >= 2) {
		// CSO v2+ requires blocks be uncompressed if large enough to be.  High bit means other things.
		plain = compressedReadSize >= frameSize;
	}
	if (plain) {
		int readSize = (u32)fileLoader_->ReadAt(compressedReadPos + compressedOffset, 1, GetBlockSize(), outPtr, flags);
		if (readSize < GetBlockSize())
			memset(outPtr + readSize, 0, GetBlockSize() - readSize);
	} else if (zlibBufferFrame == frameNumber) {
		// We already have it.  Just apply the offset and copy.
		memcpy(outPtr, zlibBuffer + compressedOffset, GetBlockSize());
	} else {
		const u32 readSize = (u32)fileLoader_->ReadAt(compressedReadPos, 1, compressedReadSize, readBuffer, flags);

		// A one-block frame inflates straight into the caller's buffer.
		u8 *dst = frameSize == (u32)GetBlockSize() ? outPtr : zlibBuffer;
		const int64_t produced = inflate_ ? InflateBuffer(inflate_, -15, readBuffer, readSize, dst, frameSize) : -1;
		if (produced != (int64_t)frameSize) {
			ERROR_LOG(Log::Loader, "block %d: inflate failed (%d of %d bytes)", blockNumber, (int)produced, frameSize);
			NotifyReadError();
			if (dst == zlibBuffer)
				zlibBufferFrame = numFrames;  // partly overwritten
			memset(outPtr, 0, GetBlockSize());
			return false;
		}

		if (frameSize != (u32)GetBlockSize()) {
			zlibBufferFrame = frameNumber;
			memcpy(outPtr, zlibBuffer + compressedOffset, GetBlockSize());
		}
	}
	return true;
}

bool CISOFileBlockDevice::ReadBlocks(u32 minBlock, int count, u8 *outPtr) {
	if (count == 1) {
		return ReadBlock(minBlock, outPtr);
	}
	if (minBlock >= numBlocks) {
		memset(outPtr, 0, GetBlockSize() * count);
		return false;
	}

	const u32 lastBlock = std::min(minBlock + count, numBlocks) - 1;
	const u32 missingBlocks = count - (lastBlock + 1 - minBlock);
	if (lastBlock < minBlock + count) {
		memset(outPtr + GetBlockSize() * (count - missingBlocks), 0, GetBlockSize() * missingBlocks);
	}

	const u32 minFrameNumber = minBlock >> blockShift;
	const u32 lastFrameNumber = lastBlock >> blockShift;
	const u32 afterLastIndexPos = index[lastFrameNumber + 1] & 0x7FFFFFFF;
	const u64 totalReadEnd = (u64)afterLastIndexPos << indexShift;

	if (!inflate_) {
		ERROR_LOG(Log::Loader, "Unable to initialize inflate");
		return false;
	}

	u64 readBufferStart = 0;
	u64 readBufferEnd = 0;
	u32 block = minBlock;
	const u32 blocksPerFrame = 1 << blockShift;
	for (u32 frame = minFrameNumber; frame <= lastFrameNumber; ++frame) {
		const u32 idx = index[frame];
		const u32 indexPos = idx & 0x7FFFFFFF;
		const u32 nextIndexPos = index[frame + 1] & 0x7FFFFFFF;

		const u64 frameReadPos = (u64)indexPos << indexShift;
		const u64 frameReadEnd = (u64)nextIndexPos << indexShift;
		// A single frame's compressed data must fit in readBuffer.
		const u32 frameReadSize = (u32)std::min<size_t>((size_t)(frameReadEnd - frameReadPos), readBufferSize);
		const u32 frameBlockOffset = block & ((1 << blockShift) - 1);
		const u32 frameBlocks = std::min(lastBlock - block + 1, blocksPerFrame - frameBlockOffset);

		if (frameReadEnd > readBufferEnd) {
			const s64 maxNeeded = totalReadEnd - frameReadPos;
			const size_t chunkSize = (size_t)std::min<s64>(std::min(maxNeeded, (s64)std::max(frameReadSize, CSO_READ_BUFFER_SIZE)), (s64)readBufferSize);

			const u32 readSize = (u32)fileLoader_->ReadAt(frameReadPos, 1, chunkSize, readBuffer);
			if (readSize < chunkSize) {
				memset(readBuffer + readSize, 0, chunkSize - readSize);
			}

			readBufferStart = frameReadPos;
			readBufferEnd = frameReadPos + readSize;
		}

		u8 *rawBuffer = &readBuffer[frameReadPos - readBufferStart];
		bool plain = (idx & 0x80000000) != 0;
		if (ver_ >= 2) {
			// CSO v2+ requires blocks be uncompressed if large enough to be.  High bit means other things.
			plain = frameReadSize >= frameSize;
		}

		if (plain) {
			memcpy(outPtr, rawBuffer + frameBlockOffset * GetBlockSize(), frameBlocks * GetBlockSize());
		} else {
			// Whole frames inflate straight into the caller's buffer.
			u8 *dst = frameBlocks == blocksPerFrame ? outPtr : zlibBuffer;
			const int64_t produced = InflateBuffer(inflate_, -15, rawBuffer, frameReadSize, dst, frameSize);
			if (produced != (int64_t)frameSize) {
				ERROR_LOG(Log::Loader, "Inflate frame %d: failed (%d of %d bytes)", frame, (int)produced, frameSize);
				NotifyReadError();
				if (dst == zlibBuffer)
					zlibBufferFrame = numFrames;  // partly overwritten
				memset(outPtr, 0, frameBlocks * GetBlockSize());
			} else if (frameBlocks != blocksPerFrame) {
				memcpy(outPtr, zlibBuffer + frameBlockOffset * GetBlockSize(), frameBlocks * GetBlockSize());
				// In case we end up reusing it in a single read later.
				zlibBufferFrame = frame;
			}
		}

		block += frameBlocks;
		outPtr += frameBlocks * GetBlockSize();
	}

	return true;
}

NPDRMDemoBlockDevice::NPDRMDemoBlockDevice(FileLoader *fileLoader)
	: BlockDevice(fileLoader)
{
	MAC_KEY mkey;
	CIPHER_KEY ckey;
	u8 np_header[256];
	u32 tableOffset_, tableSize_;

	fileLoader_->ReadAt(0x24, 1, 4, &psarOffset);
	if (psarOffset >= fileLoader_->FileSize() - 256) {
		errorString_ = "Unexpected psarOffset";
		return;
	}

	size_t readSize = fileLoader_->ReadAt(psarOffset, 1, 256, &np_header);
	if (readSize != 256) {
		errorString_ = "Invalid NPUMDIMG header!";
		return;
	}

	// Check np_header
	if (memcmp(np_header, "NPUMDIMG", 8) != 0) {
		// This is not something we can deal with here. Might be an oversized/misdetected
		// regular PBP.
		errorString_ = "Not a NPDRM PBP ISO";
		return;
	}

	u32 psar_id;
	fileLoader->ReadAt(psarOffset, 4, 1, &psar_id);

	INFO_LOG(Log::Loader, "NPDRM: PSAR ID: %08x", psar_id);
	// PS1 PSAR begins with "PSISOIMG0000"
	if (psar_id == 'SISP') {
		lbaSize_ = 0;  // Mark invalid
		ERROR_LOG(Log::Loader, "PSX not supported! Should have been caught earlier.");
		errorString_ = "PSX ISOs not supported!";
		return;
	}

	// Local kirk instance to not clash with other block devices and other decryption things.
	kirk_init(&kirk_);

	// getkey
	sceDrmBBMacInit(&mkey, 3);
	sceDrmBBMacUpdate(&kirk_, &mkey, np_header, 0xc0);
	bbmac_getkey(&kirk_, &mkey, np_header+0xc0, vkey);

	// decrypt NP header
	memcpy(hkey, np_header+0xa0, 0x10);
	sceDrmBBCipherInit(&kirk_, &ckey, 1, 2, hkey, vkey, 0);
	sceDrmBBCipherUpdate(&kirk_, &ckey, np_header+0x40, 0x60);
	sceDrmBBCipherFinal(&ckey);

	u32 lbaStart = *(u32*)(np_header+0x54); // LBA start
	u32 lbaEnd   = *(u32*)(np_header+0x64); // LBA end
	if (lbaEnd < lbaStart) {
		errorString_ = "Bad LBA range in header";
		return;
	}
	lbaSize_     = (lbaEnd - lbaStart + 1); // LBA size of ISO
	blockLBAs_   = *(u32*)(np_header+0x0c); // block size in LBA

	char psarStr[5]{};
	memcpy(psarStr, &psar_id, 4);

	// Protect against a badly decrypted header, and send information through the assert about what's being played (implicitly).
	_dbg_assert_msg_(blockLBAs_ > 0 && blockLBAs_ <= 4096, "Bad blockLBAs in header: %08x (%s) psar: %s", blockLBAs_, fileLoader->GetPath().ToVisualString().c_str(), psarStr);

	// When we remove the above assert, let's just try to survive.
	// blockLBAs_ also must not be zero, or we'd divide by zero below.
	if (blockLBAs_ <= 0 || blockLBAs_ > 4096) {
		errorString_ = StringFromFormat("Bad blockLBAs in header: %08x (%s) psar: %s", blockLBAs_, GetFriendlyPath(fileLoader->GetPath()).c_str(), psarStr);
		return;
	}

	blockSize_ = blockLBAs_ * 2048;
	numBlocks_ = (lbaSize_ + blockLBAs_ - 1) / blockLBAs_; // total blocks;

	blockBuf_ = new u8[blockSize_];
	tempBuf_  = new u8[blockSize_];

	tableOffset_ = *(u32*)(np_header + 0x6c); // table offset
	if (tableOffset_ > fileLoader_->FileSize()) {
		errorString_ = "Invalid table offset";
		return;
	}

	// Computed in 64-bit and sanity checked against the file size, since numBlocks_
	// could otherwise be large enough that numBlocks_ * sizeof(table_info) wraps
	// around in 32-bit, causing us to only actually read (and XOR-descramble) a
	// small prefix of the `numBlocks_`-sized table_ allocation, leaving the rest
	// as uninitialized heap memory that ReadBlock() would later trust.
	u64 tableSize64 = (u64)numBlocks_ * sizeof(table_info);
	if (tableSize64 == 0 || tableSize64 > (u64)fileLoader_->FileSize()) {
		errorString_ = "Invalid NPUMDIMG table size";
		return;
	}
	tableSize_ = (u32)tableSize64;
	table_ = new table_info[numBlocks_];

	readSize = fileLoader_->ReadAt(psarOffset + tableOffset_, 1, tableSize_, table_);
	if (readSize != tableSize_){
		errorString_ = "Invalid NPUMDIMG table!";
		return;
	}

	u32 *p = (u32*)table_;
	u32 i, k0, k1, k2, k3;
	for (i = 0; i < numBlocks_; i++){
		k0 = p[0]^p[1];
		k1 = p[1]^p[2];
		k2 = p[0]^p[3];
		k3 = p[2]^p[3];
		p[4] ^= k3;
		p[5] ^= k1;
		p[6] ^= k2;
		p[7] ^= k0;
		p += 8;
	}

	currentBlock_ = -1;
	_dbg_assert_(errorString_.empty());
}

NPDRMDemoBlockDevice::~NPDRMDemoBlockDevice() {
	delete [] table_;
	delete [] tempBuf_;
	delete [] blockBuf_;
}

int lzrc_decompress(void *out, int out_len, void *in, int in_len);

bool NPDRMDemoBlockDevice::ReadBlock(int blockNumber, u8 *outPtr, bool uncached) {
	FileLoader::Flags flags = uncached ? FileLoader::Flags::HINT_UNCACHED : FileLoader::Flags::NONE;

	if (blockSize_ == 0) {
		// Wasn't opened successfully.
		return false;
	}

	int lba = blockNumber - currentBlock_;
	if (lba >= 0 && lba < blockLBAs_){
		memcpy(outPtr, blockBuf_ + lba*2048, 2048);
		return true;
	}

	int block = blockNumber / blockLBAs_;
	lba = blockNumber % blockLBAs_;
	currentBlock_ = block * blockLBAs_;

	// blockNumber comes from the caller (ultimately from guest-controlled reads, e.g.
	// via /sce_lbn.../_size... on a raw sector open) and isn't otherwise guaranteed to be
	// within table_'s bounds, so bounds-check it before indexing.
	if (block < 0 || (u32)block >= numBlocks_) {
		return false;
	}

	if (table_[block].unk_1c != 0) {
		if((u32)block == (numBlocks_ - 1))
			return true; // demos make by fake_np
		else
			return false;
	}

	// table_[block].size comes straight from the (only reversibly-scrambled, not
	// otherwise validated) table in the PBP file, so a malicious/corrupt file could
	// claim a size larger than blockSize_ here - refuse rather than overflowing
	// blockBuf_/tempBuf_ (both exactly blockSize_ bytes) via ReadAt/CipherUpdate below.
	if (table_[block].size < 0 || table_[block].size > blockSize_) {
		return false;
	}

	u8 *readBuf;
	if (table_[block].size < blockSize_)
		readBuf = tempBuf_;
	else
		readBuf = blockBuf_;

	size_t readSize = fileLoader_->ReadAt(psarOffset+table_[block].offset, 1, table_[block].size, readBuf, flags);
	if (readSize != (size_t)table_[block].size){
		if((u32)block==(numBlocks_-1))
			return true;
		else
			return false;
	}

	if ((table_[block].flag & 1) == 0) {
		// skip mac check
	}

	if ((table_[block].flag & 4) == 0) {
		CIPHER_KEY ckey;
		sceDrmBBCipherInit(&kirk_, &ckey, 1, 2, hkey, vkey, table_[block].offset>>4);
		sceDrmBBCipherUpdate(&kirk_, &ckey, readBuf, table_[block].size);
		sceDrmBBCipherFinal(&ckey);
	}

	if (table_[block].size < blockSize_) {
		// The decompressed block is always blockSize_ bytes; blockBuf_ is exactly
		// that big. Pass the real size so the decompressor can't write past it.
		int lzsize = lzrc_decompress(blockBuf_, blockSize_, readBuf, table_[block].size);
		if(lzsize != blockSize_){
			ERROR_LOG(Log::Loader, "LZRC decompress error! lzsize=%d\n", lzsize);
			NotifyReadError();
			return false;
		}
	}

	memcpy(outPtr, blockBuf_+lba*2048, 2048);
	return true;
}

// rchd does no I/O: it names the byte ranges it needs and is fed them.
// Those ranges go straight from the loader into one scratch buffer the
// decoder borrows, so a hunk's compressed bytes are never copied twice.
static bool CHDServe(rchd_t *chd, FileLoader *loader, std::vector<u8> &scratch, const rchd_request_t &req, bool borrow) {
	if (req.source != RCHD_SOURCE_SELF)
		return false;
	if (scratch.size() < req.length)
		scratch.resize(req.length);
	const size_t got = loader->ReadAt(req.offset, req.length, scratch.data());
	if (got == 0)
		return false;
	if (borrow)
		return rchd_feed_borrow(chd, req.offset, req.source, scratch.data(), got) == RCHD_OK;
	return rchd_feed(chd, scratch.data(), got) == RCHD_OK;
}

static const char *CHDErrorString(int err) {
	switch (err) {
	case RCHD_ERROR_DATA: return "invalid data";
	case RCHD_ERROR_PARAM: return "invalid parameter";
	case RCHD_ERROR_MEM: return "out of memory";
	case RCHD_ERROR_UNSUPPORTED: return "unsupported format";
	case RCHD_ERROR_CRC: return "checksum mismatch";
	case RCHD_ERROR_NO_PARENT: return "parent CHD not supported";
	case RCHD_ERROR_STATE: return "invalid state";
	default: return "error";
	}
}

CHDFileBlockDevice::CHDFileBlockDevice(FileLoader *fileLoader)
	: BlockDevice(fileLoader) {
	chd_ = rchd_new();
	if (!chd_) {
		errorString_ = "CHD error: out of memory";
		return;
	}

	int err;
	rchd_request_t req;
	while ((err = rchd_open_step(chd_, &req)) == RCHD_PENDING) {
		if (!CHDServe(chd_, fileLoader, scratch_, req, false)) {
			err = req.source == RCHD_SOURCE_SELF ? RCHD_ERROR_DATA : RCHD_ERROR_NO_PARENT;
			break;
		}
	}
	const rchd_info_t *info = err == RCHD_OK ? rchd_info(chd_) : nullptr;
	if (info && info->has_parent)
		err = RCHD_ERROR_NO_PARENT;
	if (!info || err != RCHD_OK || info->unit_bytes != (u32)GetBlockSize()) {
		if (err == RCHD_OK)
			err = RCHD_ERROR_UNSUPPORTED;
		errorString_ = StringFromFormat("CHD error: %s: %s", fileLoader->GetPath().c_str(), CHDErrorString(err));
		rchd_free(chd_);
		chd_ = nullptr;
		return;
	}

	numBlocks = (u32)(info->logical_bytes / info->unit_bytes);
	_dbg_assert_(errorString_.empty());
}

CHDFileBlockDevice::~CHDFileBlockDevice() {
	rchd_free(chd_);
}

bool CHDFileBlockDevice::ReadBlock(int blockNumber, u8 *outPtr, bool uncached) {
	return ReadBlocks((u32)blockNumber, 1, outPtr);
}

bool CHDFileBlockDevice::ReadRange(u64 offset, size_t len, u8 *out) {
	int err = rchd_read_begin(chd_, offset, out, len);
	rchd_request_t req;
	if (err == RCHD_OK) {
		while ((err = rchd_read_step(chd_, &req)) == RCHD_PENDING) {
			if (!CHDServe(chd_, fileLoader_, scratch_, req, true)) {
				err = RCHD_ERROR_DATA;
				break;
			}
		}
	}
	if (err != RCHD_OK) {
		ERROR_LOG(Log::Loader, "CHD read failed at %llu (%d bytes): %s", (unsigned long long)offset, (int)len, CHDErrorString(err));
		NotifyReadError();
		return false;
	}
	return true;
}

bool CHDFileBlockDevice::ReadBytes(u64 offset, size_t len, u8 *out) {
	// rchd addresses bytes, so a file read decodes straight into place.
	if (!chd_ || offset + len > (u64)numBlocks * GetBlockSize())
		return BlockDevice::ReadBytes(offset, len, out);
	return ReadRange(offset, len, out);
}

bool CHDFileBlockDevice::ReadBlocks(u32 minBlock, int count, u8 *outPtr) {
	const size_t blockSize = GetBlockSize();
	if (!chd_) {
		ERROR_LOG(Log::Loader, "ReadBlocks: CHD not open. %s", fileLoader_->GetPath().c_str());
		memset(outPtr, 0, blockSize * count);
		return false;
	}
	if (minBlock >= numBlocks || count <= 0) {
		memset(outPtr, 0, blockSize * std::max(count, 0));
		return false;
	}
	// The tail past the image reads as zeros, like the other devices.
	const u32 avail = std::min((u32)count, numBlocks - minBlock);
	if (avail < (u32)count)
		memset(outPtr + avail * blockSize, 0, (count - avail) * blockSize);

	if (!ReadRange((u64)minBlock * blockSize, avail * blockSize, outPtr))
		return false;
	return avail == (u32)count;
}
