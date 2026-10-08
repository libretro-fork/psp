#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <formats/image.h>
#include <formats/rpng.h>
#include <libretro.h>  // RETRO_VFS_FILE_ACCESS_*
#include <streams/interface_stream.h>

#include "Common/Data/Format/PNGLoad.h"
#include "Common/Log.h"
#include "Common/File/FileUtil.h"

// *image_data_ptr should be deleted with free()
// return value of 1 == success.
int pngLoad(const char *file, int *pwidth, int *pheight, unsigned char **image_data_ptr) {
	*image_data_ptr = nullptr;
	std::string data;
	if (!File::ReadBinaryFileToString(Path(file), &data) || data.empty()) {
		WARN_LOG(Log::IO, "pngLoad: can't read %s", file);
		return 0;
	}
	return pngLoadPtr((const unsigned char *)data.data(), data.size(), pwidth, pheight, image_data_ptr);
}

int pngLoadPtr(const unsigned char *input_ptr, size_t input_len, int *pwidth, int *pheight, unsigned char **image_data_ptr, int maxWidth, int maxHeight) {
	*image_data_ptr = nullptr;
	if (input_len < sizeof(PNGHeaderPeek))
		return 0;

	// Reject images larger than the caller's limits before anything is allocated,
	// so attacker-controlled dimensions can't drive a huge allocation.
	PNGHeaderPeek peek;
	memcpy(&peek, input_ptr, sizeof(peek));
	if (!peek.IsValidPNGHeader() || peek.Width() > maxWidth || peek.Height() > maxHeight) {
		DEBUG_LOG(Log::IO, "PNG rejected: %dx%d (max %dx%d)", peek.Width(), peek.Height(), maxWidth, maxHeight);
		return 0;
	}

	rpng_t *rpng = rpng_alloc();
	if (!rpng)
		return 0;
	// rpng only reads through this pointer.
	uint32_t *data = nullptr;
	unsigned w = 0, h = 0;
	int ret = IMAGE_PROCESS_ERROR;
	if (rpng_set_buf_ptr(rpng, (void *)input_ptr, input_len) && rpng_start(rpng)) {
		while (rpng_iterate_image(rpng)) {
		}
		if (rpng_is_valid(rpng)) {
			// RGBA byte order, which is what everything here wants.
			do {
				ret = rpng_process_image(rpng, (void **)&data, input_len, &w, &h, true);
			} while (ret == IMAGE_PROCESS_NEXT);
		}
	}
	rpng_free(rpng);

	if (ret != IMAGE_PROCESS_END || !data || (int)w != peek.Width() || (int)h != peek.Height()) {
		free(data);
		ERROR_LOG(Log::IO, "PNG decode failed");
		return 0;
	}
	*pwidth = (int)w;
	*pheight = (int)h;
	*image_data_ptr = (unsigned char *)data;
	return 1;
}

bool PNGHeaderPeek::IsValidPNGHeader() const {
	if (magic != 0x474e5089 || ihdrTag != 0x52444849) {
		return false;
	}
	// Keep the peeker's limit consistent with pngLoadPtr(). A replacement
	// texture is decoded into an uncompressed RGBA buffer, so accepting one
	// oversized dimension would still allow a decompression bomb.
	const int width = Width();
	const int height = Height();
	if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
		return false;
	}
	return true;
}

// rpng's encoder writes to an intfstream; a memory one sized past the worst case
// holds the whole file, which then goes out in one write.
bool pngEncode(std::vector<uint8_t> *out, const void *buffer, int w, int h, PNGFormat format) {
	const uint8_t *src = (const uint8_t *)buffer;
	std::vector<uint8_t> bgr;
	enum rpng_pixfmt fmt;
	int bpp;
	switch (format) {
	case PNGFormat::RGBA8888: fmt = RPNG_PIXFMT_RGBA32; bpp = 4; break;
	// ARGB32 as 32-bit words is B, G, R, A in memory on little-endian hosts.
	case PNGFormat::BGRA8888: fmt = RPNG_PIXFMT_ARGB32; bpp = 4; break;
	case PNGFormat::RGB888:
	default:
		// rpng takes 24-bit pixels as BGR.
		bgr.resize((size_t)w * h * 3);
		for (size_t i = 0; i < bgr.size(); i += 3) {
			bgr[i + 0] = src[i + 2];
			bgr[i + 1] = src[i + 1];
			bgr[i + 2] = src[i + 0];
		}
		src = bgr.data();
		fmt = RPNG_PIXFMT_BGR24;
		bpp = 3;
		break;
	}
	const size_t raw = (size_t)w * h * bpp;
	const size_t bound = raw + raw / 16 + (size_t)h * 8 + 4096;
	out->resize(bound);
	intfstream_t *stream = intfstream_open_memory(out->data(), RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE, bound);
	if (!stream) {
		out->clear();
		return false;
	}
	const bool ok = rpng_save_image_stream_fmt(src, stream, w, h, w * bpp, fmt, nullptr);
	const int64_t written = intfstream_get_ptr(stream);
	intfstream_close(stream);
	free(stream);
	if (!ok || written <= 0) {
		out->clear();
		return false;
	}
	out->resize((size_t)written);
	return true;
}

bool pngSave(const Path &filename, const void *buffer, int w, int h, int bytesPerPixel) {
	std::vector<uint8_t> png;
	if (!pngEncode(&png, buffer, w, h, bytesPerPixel == 4 ? PNGFormat::RGBA8888 : PNGFormat::RGB888) ||
		!File::WriteDataToFile(false, png.data(), png.size(), filename)) {
		ERROR_LOG(Log::IO, "PNG encode failed: %s", filename.c_str());
		return false;
	}
	return true;
}
