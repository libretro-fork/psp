// Copyright (c) 2018- PPSSPP Project.

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
#include <vector>

#include "Common/Data/Format/PNGLoad.h"
#include "Common/Data/Encoding/Base64.h"
#include "Common/StringUtils.h"
#include "Core/Debugger/WebSocket/GPUBufferSubscriber.h"
#include "Core/Debugger/WebSocket/WebSocketUtils.h"
#include "Core/MIPS/MIPSDebugInterface.h"
#include "Core/Screenshot.h"
#include "GPU/Debugger/Stepping.h"

DebuggerSubscriber *WebSocketGPUBufferInit(DebuggerEventHandlerMap &map) {
	// No need to bind or alloc state, these are all global.
	map["gpu.buffer.screenshot"] = &WebSocketGPUBufferScreenshot;
	map["gpu.buffer.renderColor"] = &WebSocketGPUBufferRenderColor;
	map["gpu.buffer.renderDepth"] = &WebSocketGPUBufferRenderDepth;
	map["gpu.buffer.renderStencil"] = &WebSocketGPUBufferRenderStencil;
	map["gpu.buffer.texture"] = &WebSocketGPUBufferTexture;
	map["gpu.buffer.clut"] = &WebSocketGPUBufferClut;

	return nullptr;
}

// Note: Calls req.Respond().  Other data can be added afterward.
static bool StreamBufferToDataURI(DebuggerRequest &req, const GPUDebugBuffer &buf, bool isFramebuffer, bool includeAlpha, int stackWidth) {
	u8 *flipbuffer = nullptr;
	u32 w = (u32)-1;
	u32 h = (u32)-1;
	const u8 *buffer = ConvertBufferToScreenshot(buf, includeAlpha, flipbuffer, w, h);
	if (!buffer) {
		req.Fail("Internal error converting buffer for PNG encode");
		return false;
	}

	if (stackWidth > 0) {
		u32 totalPixels = w * h;
		// stackWidth is client-supplied and otherwise unbounded; since totalPixels
		// is the real (small) buffer size, clamping to it bounds this loop to at
		// most totalPixels iterations instead of up to ~2 billion for a huge value.
		// Keep it at least 1 to avoid a divide by zero below.
		w = std::max<u32>(1, std::min((u32)stackWidth, totalPixels));
		while (w > 1 && (totalPixels % w) != 0)
			--w;
		h = totalPixels / w;
	}

	std::vector<uint8_t> png;
	const bool encoded = pngEncode(&png, buffer, w, h, includeAlpha ? PNGFormat::RGBA8888 : PNGFormat::RGB888);
	delete [] flipbuffer;
	if (!encoded) {
		req.Fail("Internal error encoding PNG");
		return false;
	}

	auto &json = req.Respond();
	json.writeInt("width", w);
	json.writeInt("height", h);
	if (isFramebuffer) {
		json.writeBool("isFramebuffer", isFramebuffer);
	}

	// Start a value...
	json.writeRaw("uri", "");
	req.Flush();
	// Now we'll write it directly to the stream, in pieces that are a multiple of 3 bytes
	// so the base64 has no padding until the end.
	req.ws->AddFragment(false, "\"data:image/png;base64,");
	const size_t piece = 3 * 16384;
	for (size_t pos = 0; pos < png.size(); pos += piece) {
		req.ws->AddFragment(false, Base64Encode(png.data() + pos, std::min(piece, png.size() - pos)));
	}

	// End the string.
	req.ws->AddFragment(false, "\"");
	return true;
}

static std::string DescribeFormat(GPUDebugBufferFormat fmt) {
	switch (fmt) {
	case GPU_DBG_FORMAT_565: return "B5G6R5_UNORM_PACK16";
	case GPU_DBG_FORMAT_5551: return "A1B5G5R5_UNORM_PACK16";
	case GPU_DBG_FORMAT_4444: return "A4B4G4R4_UNORM_PACK16";
	case GPU_DBG_FORMAT_8888: return "R8G8B8A8_UNORM";

	case GPU_DBG_FORMAT_565_REV: return "R5G6B5_UNORM_PACK16";
	case GPU_DBG_FORMAT_5551_REV: return "R5G5B5A1_UNORM_PACK16";
	case GPU_DBG_FORMAT_4444_REV: return "R4G4B4A4_UNORM_PACK16";

	case GPU_DBG_FORMAT_5551_BGRA: return "A1R5G5B5_UNORM_PACK16";
	case GPU_DBG_FORMAT_4444_BGRA: return "A4R4G4B4_UNORM_PACK16";
	case GPU_DBG_FORMAT_8888_BGRA: return "B8G8R8A8_UNORM";

	case GPU_DBG_FORMAT_FLOAT: return "D32F";
	case GPU_DBG_FORMAT_16BIT: return "D16";
	case GPU_DBG_FORMAT_8BIT: return "S8";
	case GPU_DBG_FORMAT_24BIT_8X: return "D24_X8";
	case GPU_DBG_FORMAT_24X_8BIT: return "X24_S8";

	case GPU_DBG_FORMAT_888_RGB: return "R8G8B8_UNORM";

	case GPU_DBG_FORMAT_INVALID:
	case GPU_DBG_FORMAT_BRSWAP_FLAG:
	default:
		return "UNDEFINED";
	}
}

// Note: Calls req.Respond().  Other data can be added afterward.
static bool StreamBufferToBase64(DebuggerRequest &req, const GPUDebugBuffer &buf, bool isFramebuffer) {
	size_t length = buf.GetStride() * buf.GetHeight();

	auto &json = req.Respond();
	json.writeInt("width", buf.GetStride());
	json.writeInt("height", buf.GetHeight());
	json.writeBool("flipped", buf.GetFlipped());
	json.writeString("format", DescribeFormat(buf.GetFormat()));
	if (isFramebuffer) {
		json.writeBool("isFramebuffer", isFramebuffer);
	}

	// Start a value without any actual data yet...
	json.writeRaw("base64", "");
	req.Flush();

	// Now we'll write it directly to the stream.
	req.ws->AddFragment(false, "\"");
	// 65535 is an "even" number of base64 characters.
	static const size_t CHUNK_SIZE = 65535;
	for (size_t i = 0; i < length; i += CHUNK_SIZE) {
		size_t left = std::min(length - i, CHUNK_SIZE);
		req.ws->AddFragment(false, Base64Encode(buf.GetData() + i, left));
	}
	req.ws->AddFragment(false, "\"");

	return true;
}

static void GenericStreamBuffer(DebuggerRequest &req, std::function<bool(const GPUDebugBuffer *&, bool *isFramebuffer)> func) {
	if (!currentDebugMIPS->isAlive()) {
		return req.Fail("CPU not started");
	}
	if (coreState != CORE_STEPPING_CPU && !GPUStepping::IsStepping()) {
		return req.Fail("Neither CPU or GPU is stepping");
	}

	bool includeAlpha = false;
	if (!req.ParamBool("alpha", &includeAlpha, DebuggerParamType::OPTIONAL))
		return;
	u32 stackWidth = 0;
	if (!req.ParamU32("stackWidth", &stackWidth, false, DebuggerParamType::OPTIONAL))
		return;
	std::string type = "uri";
	if (!req.ParamString("type", &type, DebuggerParamType::OPTIONAL))
		return;
	if (type != "uri" && type != "base64")
		return req.Fail("Parameter 'type' must be either 'uri' or 'base64'");

	const GPUDebugBuffer *buf = nullptr;
	bool isFramebuffer = false;
	if (!func(buf, &isFramebuffer)) {
		return req.Fail("Could not download output");
	}
	if (!buf) {
		return req.Fail("No output available");
	}

	if (type == "base64") {
		StreamBufferToBase64(req, *buf, isFramebuffer);
	} else if (type == "uri") {
		StreamBufferToDataURI(req, *buf, isFramebuffer, includeAlpha, stackWidth);
	} else {
		return req.Fail("Unexpected output type");
	}
}

// Retrieve a screenshot (gpu.buffer.screenshot)
//
// Parameters:
//  - type: either 'uri' or 'base64' (optional, defaults to 'uri').
//  - alpha: boolean to include the alpha channel for 'uri' type (not normally useful for screenshots.)
//  - stackWidth: optional, forced width for 'uri' type (increases height.)
//
// Response (same event name) for 'uri' type:
//  - width: numeric width of screenshot.
//  - height: numeric height of screenshot.
//  - isFramebuffer: optional, present and true if this came from a hardware framebuffer.
//  - uri: data: URI of PNG image for display.
//
// Response (same event name) for 'base64' type:
//  - width: numeric width of screenshot (also stride, in pixels, of binary data.)
//  - height: numeric height of screenshot.
//  - flipped: boolean to indicate whether buffer is vertically flipped.
//  - format: string indicating format, such as 'R8G8B8A8_UNORM' or 'B8G8R8A8_UNORM'.
//  - isFramebuffer: optional, present and true if this came from a hardware framebuffer.
//  - base64: base64 encode of binary data.
void WebSocketGPUBufferScreenshot(DebuggerRequest &req) {
	GenericStreamBuffer(req, [](const GPUDebugBuffer *&buf, bool *isFramebuffer) {
		*isFramebuffer = false;
		if (GPUStepping::GPU_GetOutputFramebuffer(buf)) {
			return true;
		}
		// Without a backbuffer (headless Vulkan), the displayed PSP framebuffer is the closest thing.
		*isFramebuffer = true;
		return GPUStepping::GPU_GetCurrentFramebuffer(buf, GPU_DBG_FRAMEBUF_DISPLAY);
	});
}

// Retrieve current color render buffer (gpu.buffer.renderColor)
//
// Parameters:
//  - type: either 'uri' or 'base64' (optional, defaults to 'uri').
//  - alpha: boolean to include the alpha channel for 'uri' type.
//  - stackWidth: optional, forced width for 'uri' type (increases height.)
//
// Response (same event name) for 'uri' type:
//  - width: numeric width of render buffer (may include stride.)
//  - height: numeric height of render buffer.
//  - isFramebuffer: optional, present and true if this came from a hardware framebuffer.
//  - uri: data: URI of PNG image for display.
//
// Response (same event name) for 'base64' type:
//  - width: numeric width of render buffer (also stride, in pixels, of binary data.)
//  - height: numeric height of render buffer.
//  - flipped: boolean to indicate whether buffer is vertically flipped.
//  - format: string indicating format, such as 'R8G8B8A8_UNORM' or 'B8G8R8A8_UNORM'.
//  - isFramebuffer: optional, present and true if this came from a hardware framebuffer.
//  - base64: base64 encode of binary data.
void WebSocketGPUBufferRenderColor(DebuggerRequest &req) {
	GenericStreamBuffer(req, [](const GPUDebugBuffer *&buf, bool *isFramebuffer) {
		*isFramebuffer = false;
		return GPUStepping::GPU_GetCurrentFramebuffer(buf, GPU_DBG_FRAMEBUF_RENDER);
	});
}

// Retrieve current depth render buffer (gpu.buffer.renderDepth)
//
// Parameters:
//  - type: either 'uri' or 'base64' (optional, defaults to 'uri').
//  - alpha: true to use alpha to encode depth, otherwise red for 'uri' type.
//  - stackWidth: optional, forced width for 'uri' type (increases height.)
//
// Response (same event name) for 'uri' type:
//  - width: numeric width of render buffer (may include stride.)
//  - height: numeric height of render buffer.
//  - isFramebuffer: optional, present and true if this came from a hardware framebuffer.
//  - uri: data: URI of PNG image for display.
//
// Response (same event name) for 'base64' type:
//  - width: numeric width of render buffer (also stride, in pixels, of binary data.)
//  - height: numeric height of render buffer.
//  - flipped: boolean to indicate whether buffer is vertically flipped.
//  - format: string indicating format, such as 'D16', 'D24_X8' or 'D32F'.
//  - isFramebuffer: optional, present and true if this came from a hardware framebuffer.
//  - base64: base64 encode of binary data.
void WebSocketGPUBufferRenderDepth(DebuggerRequest &req) {
	GenericStreamBuffer(req, [](const GPUDebugBuffer *&buf, bool *isFramebuffer) {
		*isFramebuffer = false;
		return GPUStepping::GPU_GetCurrentDepthbuffer(buf);
	});
}

// Retrieve current stencil render buffer (gpu.buffer.renderStencil)
//
// Parameters:
//  - type: either 'uri' or 'base64' (optional, defaults to 'uri').
//  - alpha: true to use alpha to encode stencil, otherwise red for 'uri' type.
//  - stackWidth: optional, forced width for 'uri' type (increases height.)
//
// Response (same event name) for 'uri' type:
//  - width: numeric width of render buffer (may include stride.)
//  - height: numeric height of render buffer.
//  - isFramebuffer: optional, present and true if this came from a hardware framebuffer.
//  - uri: data: URI of PNG image for display.
//
// Response (same event name) for 'base64' type:
//  - width: numeric width of render buffer (also stride, in pixels, of binary data.)
//  - height: numeric height of render buffer.
//  - flipped: boolean to indicate whether buffer is vertically flipped.
//  - format: string indicating format, such as 'X24_S8' or 'S8'.
//  - isFramebuffer: optional, present and true if this came from a hardware framebuffer.
//  - base64: base64 encode of binary data.
void WebSocketGPUBufferRenderStencil(DebuggerRequest &req) {
	GenericStreamBuffer(req, [](const GPUDebugBuffer *&buf, bool *isFramebuffer) {
		*isFramebuffer = false;
		return GPUStepping::GPU_GetCurrentStencilbuffer(buf);
	});
}

// Retrieve current texture (gpu.buffer.texture)
//
// Parameters:
//  - type: either 'uri' or 'base64' (optional, defaults to 'uri').
//  - alpha: boolean to include the alpha channel for 'uri' type.
//  - level: optional texture mip level, default 0.
//  - stackWidth: optional, forced width for 'uri' type (increases height.)
//
// Response (same event name) for 'uri' type:
//  - width: numeric width of the texture (often wider than visual.)
//  - height: numeric height of the texture (often wider than visual.)
//  - isFramebuffer: optional, present and true if this came from a hardware framebuffer.
//  - uri: data: URI of PNG image for display.
//
// Response (same event name) for 'base64' type:
//  - width: numeric width and stride of the texture (often wider than visual.)
//  - height: numeric height of the texture (often wider than visual.)
//  - flipped: boolean to indicate whether buffer is vertically flipped.
//  - format: string indicating format, such as 'R8G8B8A8_UNORM' or 'B8G8R8A8_UNORM'.
//  - isFramebuffer: optional, present and true if this came from a hardware framebuffer.
//  - base64: base64 encode of binary data.
void WebSocketGPUBufferTexture(DebuggerRequest &req) {
	u32 level = 0;
	if (!req.ParamU32("level", &level, false, DebuggerParamType::OPTIONAL))
		return;
	// Sanity check the level, to avoid overflow hacks. Also it just can't be very high,
	// we currently support 12 levels for replacement (the PSP only supports 8).
	if (level > 12) {
		return req.Fail("Invalid level");
	}

	GenericStreamBuffer(req, [level](const GPUDebugBuffer *&buf, bool *isFramebuffer) {
		return GPUStepping::GPU_GetCurrentTexture(buf, level, isFramebuffer);
	});
}

// Retrieve current CLUT (gpu.buffer.clut)
//
// Parameters:
//  - type: either 'uri' or 'base64' (optional, defaults to 'uri').
//  - alpha: boolean to include the alpha channel for 'uri' type.
//  - stackWidth: optional, forced width for 'uri' type (increases height.)
//
// Response (same event name) for 'uri' type:
//  - width: numeric width of CLUT.
//  - height: numeric height of CLUT.
//  - isFramebuffer: optional, present and true if this came from a hardware framebuffer.
//  - uri: data: URI of PNG image for display.
//
// Response (same event name) for 'base64' type:
//  - width: number of pixels in CLUT.
//  - height: always 1.
//  - flipped: boolean to indicate whether buffer is vertically flipped.
//  - format: string indicating format, such as 'R8G8B8A8_UNORM' or 'B8G8R8A8_UNORM'.
//  - isFramebuffer: optional, present and true if this came from a hardware framebuffer.
//  - base64: base64 encode of binary data.
void WebSocketGPUBufferClut(DebuggerRequest &req) {
	GenericStreamBuffer(req, [](const GPUDebugBuffer *&buf, bool *isFramebuffer) {
		// TODO: Or maybe it could be?
		*isFramebuffer = false;
		return GPUStepping::GPU_GetCurrentClut(buf);
	});
}
