#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// zlib-family codecs on libretro-common's rinflate/rdeflate.
// windowBits follow zlib: -15 raw deflate, 15 zlib, 31 gzip, 47 zlib or gzip.

// Decodes one complete stream into out[0..outLen). Returns the bytes produced, or -1
// if the stream is bad, truncated, or doesn't fit. *consumed receives the input used.
int64_t InflateBuffer(int windowBits, const uint8_t *in, size_t inLen, uint8_t *out, size_t outLen, size_t *consumed = nullptr);

// The same through a stream from rinflate_new(), reset here; for callers that decode
// many small streams and shouldn't allocate for each one.
int64_t InflateBuffer(void *stream, int windowBits, const uint8_t *in, size_t inLen, uint8_t *out, size_t outLen, size_t *consumed = nullptr);

// RFC 1950 Adler-32, zlib's adler32() (start with 1).
uint32_t Adler32(uint32_t adler, const uint8_t *buf, size_t len);

// Whole-string helpers. compress_string writes zlib by default (31 for gzip);
// decompress_string accepts zlib or gzip.
bool compress_string(const std::string &str, std::string *dest, int compressionlevel = 9, int windowBits = 15);
bool decompress_string(const std::string &str, std::string *dest);
