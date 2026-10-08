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

#include <encodings/crc32.h>

#include "Common/CommonTypes.h"
#include "Common/Data/Encoding/Compression.h"
#include "Core/Debugger/MemBlockInfo.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/sceDeflt.h"
#include "Core/MemMap.h"

// All the decompress functions are identical with only differing window bits.
static int CommonDecompress(int windowBits, u32 OutBuffer, int OutBufferLength, u32 InBuffer, u32 Crc32Addr) {
	if (!Memory::IsValidAddress(OutBuffer) || !Memory::IsValidAddress(InBuffer)) {
		return hleLogError(Log::HLE, 0, "bad address");
	}

	auto crc32Addr = PSPPointer<u32_le>::Create(Crc32Addr);
	if (Crc32Addr && !crc32Addr.IsValid()) {
		return hleLogError(Log::HLE, 0, "bad crc32 address");
	}

	u8 *outBufferPtr = Memory::GetPointerWriteOrException(OutBuffer);
	const u8 *inBufferPtr = Memory::GetPointerOrException(InBuffer);
	// We don't know the available length, just let it use as much as it wants.
	const size_t inAvail = Memory::ClampValidSizeAt(InBuffer, Memory::g_MemorySize);
	const size_t outAvail = OutBufferLength > 0 ? Memory::ClampValidSizeAt(OutBuffer, (u32)OutBufferLength) : 0;

	size_t totalIn = 0;
	const int64_t totalOut = InflateBuffer(windowBits, inBufferPtr, inAvail, outBufferPtr, outAvail, &totalIn);
	if (totalOut < 0) {
		return hleLogError(Log::HLE, 0, "inflate failed");
	}
	if (crc32Addr.IsValid()) {
		*crc32Addr = encoding_crc32(0, outBufferPtr, (size_t)totalOut);
	}

	if (MemBlockInfoDetailed((u32)totalIn, (u32)totalOut)) {
		char tagData[128];
		size_t tagSize = FormatMemWriteTagAt(tagData, sizeof(tagData), "sceDeflt/", InBuffer, (u32)totalIn);
		NotifyMemInfo(MemBlockFlags::READ, InBuffer, (u32)totalIn, tagData, tagSize);
		NotifyMemInfo(MemBlockFlags::WRITE, OutBuffer, (u32)totalOut, tagData, tagSize);
	}

	return hleLogDebug(Log::HLE, (int)totalOut);
}

static int sceDeflateDecompress(u32 OutBuffer, int OutBufferLength, u32 InBuffer, u32 Crc32Addr) {
	return CommonDecompress(-15, OutBuffer, OutBufferLength, InBuffer, Crc32Addr);
}

static int sceGzipDecompress(u32 OutBuffer, int OutBufferLength, u32 InBuffer, u32 Crc32Addr) {
	return CommonDecompress(31, OutBuffer, OutBufferLength, InBuffer, Crc32Addr);
}

static int sceZlibDecompress(u32 OutBuffer, int OutBufferLength, u32 InBuffer, u32 Crc32Addr) {
	return CommonDecompress(15, OutBuffer, OutBufferLength, InBuffer, Crc32Addr);
}

const HLEFunction sceDeflt[] = {
	{0X0BA3B9CC, nullptr,                            "sceGzipGetCompressedData", '?', ""    },
	{0X106A3552, nullptr,                            "sceGzipGetName",           '?', ""    },
	{0X1B5B82BC, nullptr,                            "sceGzipIsValid",           '?', ""    },
	{0X2EE39A64, nullptr,                            "sceZlibAdler32",           '?', ""    },
	{0X44054E03, &WrapI_UIUU<sceDeflateDecompress>,  "sceDeflateDecompress",     'i', "xixp"},
	{0X6A548477, nullptr,                            "sceZlibGetCompressedData", '?', ""    },
	{0X6DBCF897, &WrapI_UIUU<sceGzipDecompress>,     "sceGzipDecompress",        'i', "xixp"},
	{0X8AA82C92, nullptr,                            "sceGzipGetInfo",           '?', ""    },
	{0XA9E4FB28, &WrapI_UIUU<sceZlibDecompress>,     "sceZlibDecompress",        'i', "xixp"},
	{0XAFE01FD3, nullptr,                            "sceZlibGetInfo",           '?', ""    },
	{0XB767F9A0, nullptr,                            "sceGzipGetComment",        '?', ""    },
	{0XE46EB986, nullptr,                            "sceZlibIsValid",           '?', ""    },
};

void Register_sceDeflt() {
	RegisterHLEModule("sceDeflt", ARRAY_SIZE(sceDeflt), sceDeflt);
}
