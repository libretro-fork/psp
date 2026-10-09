// Checks the deflate, zstd, PNG and zip code against vectors made by make_vectors.py.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <encodings/crc32.h>
#include <encodings/deflate.h>
#include <encodings/rzstd.h>

#include "Common/Data/Encoding/Compression.h"
#include "Common/Data/Format/PNGLoad.h"
#include "Common/File/VFS/ZipFileReader.h"

static std::string Read(const std::string &p) {
	std::ifstream f(p, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv) {
	std::string dir = argv[1];
	const char *names[] = { "text", "rand", "zero", "small", "empty" };
	void *reused = rinflate_new(-15);
	CHECK(reused, "allocate reused inflate stream");
	for (const char *n : names) {
		std::string raw = Read(dir + "/in/" + n + ".raw");
		struct { const char *ext; int wb; } fmts[] = { { "deflate", -15 }, { "fixed", -15 }, { "zlib", 15 }, { "gz", 31 }, { "zlib", 47 }, { "gz", 47 } };
		for (auto &f : fmts) {
			std::string c = Read(dir + "/in/" + n + "." + f.ext);
			std::vector<uint8_t> out(raw.size() + 16);
			size_t used = 0;
			int64_t got = InflateBuffer(f.wb, (const uint8_t *)c.data(), c.size(), out.data(), out.size(), &used);
			CHECK(got == (int64_t)raw.size() && !memcmp(out.data(), raw.data(), raw.size()), "inflate %s.%s wb=%d got=%lld", n, f.ext, f.wb, (long long)got);
			CHECK(used == c.size(), "inflate %s.%s consumed %zu of %zu", n, f.ext, used, c.size());
			// Reuse across fixed/dynamic/stored blocks and wrapper changes. Fixed
			// tables may survive reset, but dynamic/error paths must invalidate them.
			if (reused) {
				for (int pass = 0; pass < 3; ++pass) {
					rinflate_reset(reused, f.wb);
					rinflate_set_in(reused, (const uint8_t *)c.data(), c.size());
					rinflate_set_out(reused, out.data(), out.size());
					size_t read = 0, wrote = 0;
					int result = rinflate_process(reused, &read, &wrote);
					CHECK(result == RDEFLATE_PROCESS_END && wrote == raw.size() &&
						!memcmp(out.data(), raw.data(), raw.size()), "reused inflate %s.%s pass=%d", n, f.ext, pass);
				}
			}
			if (!raw.empty()) {
				// One byte short must fail, not truncate.
				int64_t shortGot = InflateBuffer(f.wb, (const uint8_t *)c.data(), c.size(), out.data(), raw.size() - 1);
				CHECK(shortGot < 0, "inflate %s.%s short output accepted", n, f.ext);
				int64_t trunc = InflateBuffer(f.wb, (const uint8_t *)c.data(), c.size() / 2, out.data(), out.size());
				CHECK(trunc < 0, "inflate %s.%s truncated input accepted", n, f.ext);
			}
		}
		std::string s;
		CHECK(decompress_string(Read(dir + "/in/" + n + ".zlib"), &s) && s == raw, "decompress_string zlib %s", n);
		if (!raw.empty()) {
			CHECK(decompress_string(Read(dir + "/in/" + n + ".gz"), &s) && s == raw, "decompress_string gz %s", n);
			std::string z, back;
			CHECK(compress_string(raw, &z, 9) && decompress_string(z, &back) && back == raw, "zlib roundtrip %s", n);
			CHECK(compress_string(raw, &z, 6, 31) && decompress_string(z, &back) && back == raw, "gzip roundtrip %s", n);
			std::ofstream(dir + "/out_" + n + ".gz", std::ios::binary) << z;
		}
		unsigned adler = 0, crc = 0;
		sscanf(Read(dir + "/in/" + n + ".adler").c_str(), "%x", &adler);
		sscanf(Read(dir + "/in/" + n + ".crc").c_str(), "%x", &crc);
		CHECK(Adler32(1, (const uint8_t *)raw.data(), raw.size()) == adler, "adler32 %s", n);
		CHECK(encoding_crc32(0, (const uint8_t *)raw.data(), raw.size()) == crc, "crc32 %s", n);
		for (int lvl : { 1, 3, 9, 19 }) {
			std::string c = Read(dir + "/in/" + n + ".l" + std::to_string(lvl) + ".zst");
			std::vector<uint8_t> out(raw.size() + 1);
			size_t wrote = 0;
			int r = rzstd_decode(out.data(), out.size(), (const uint8_t *)c.data(), c.size(), &wrote);
			CHECK(r == RZSTD_PROCESS_END && wrote == raw.size() && !memcmp(out.data(), raw.data(), raw.size()), "rzstd decode %s l%d", n, lvl);
		}
		{
			std::vector<uint8_t> enc(rzstd_compress_bound(raw.size()));
			size_t encLen = 0;
			int r = rzstd_encode(enc.data(), enc.size(), (const uint8_t *)raw.data(), raw.size(), 3, &encLen);
			CHECK(r == RZSTD_PROCESS_END, "rzstd encode %s", n);
			std::ofstream(dir + "/out_" + n + ".zst", std::ios::binary).write((const char *)enc.data(), encLen);
			std::vector<uint8_t> back(raw.size() + 1);
			size_t wrote = 0;
			r = rzstd_decode(back.data(), back.size(), enc.data(), encLen, &wrote);
			CHECK(r == RZSTD_PROCESS_END && wrote == raw.size() && !memcmp(back.data(), raw.data(), raw.size()), "rzstd roundtrip %s", n);
		}
	}
	rinflate_free(reused);
	const char *pngs[] = { "rgba", "rgb", "gray", "graya", "pal", "paltrns", "solid", "gray16" };
	for (const char *n : pngs) {
		std::string p = Read(dir + "/png/" + n + ".png");
		std::string want = Read(dir + "/png/" + n + ".png.rgba");
		int w = 0, h = 0;
		unsigned char *img = nullptr;
		int ok = pngLoadPtr((const unsigned char *)p.data(), p.size(), &w, &h, &img);
		CHECK(ok == 1 && (size_t)w * h * 4 == want.size() && !memcmp(img, want.data(), want.size()), "png decode %s (%d %dx%d)", n, ok, w, h);
		if (ok == 1) {
			std::vector<uint8_t> enc;
			CHECK(pngEncode(&enc, img, w, h), "png encode %s", n);
			int w2 = 0, h2 = 0;
			unsigned char *img2 = nullptr;
			CHECK(pngLoadPtr(enc.data(), enc.size(), &w2, &h2, &img2) == 1 && w2 == w && h2 == h && !memcmp(img, img2, (size_t)w * h * 4), "png roundtrip %s", n);
			std::ofstream(dir + "/out_" + n + ".png", std::ios::binary).write((const char *)enc.data(), enc.size());
			free(img2);
			// Too large for the caller's limits is refused.
			CHECK(pngLoadPtr((const unsigned char *)p.data(), p.size(), &w2, &h2, &img2, w - 1, h) == 0, "png limit %s", n);
		}
		free(img);
		// Truncated file fails cleanly.
		CHECK(pngLoadPtr((const unsigned char *)p.data(), p.size() / 2, &w, &h, &img) == 0, "png truncated %s", n);
	}
	{
		ZipFileReader *z = ZipFileReader::Create(Path(dir + "/vfs.zip"), "", true);
		CHECK(z != nullptr, "zip open");
		if (z) {
			size_t sz = 0;
			uint8_t *d = z->ReadFile("dir/sub/deflated.txt", &sz);
			std::string text = Read(dir + "/in/text.raw");
			CHECK(d && sz == text.size() && !memcmp(d, text.data(), sz), "zip deflated read");
			delete[] d;
			d = z->ReadFile("DIR/stored.BIN", &sz);
			std::string rnd = Read(dir + "/in/rand.raw").substr(0, 5000);
			CHECK(d && sz == 5000 && !memcmp(d, rnd.data(), sz), "zip stored nocase read");
			delete[] d;
			std::vector<File::FileInfo> list;
			CHECK(z->GetFileListing("dir", &list, nullptr) && list.size() == 2, "zip listing %zu", list.size());
			File::FileInfo info;
			CHECK(z->GetFileInfo("textures.ini", &info) && info.size == 9, "zip info");
			CHECK(!z->GetFileInfo("missing", &info), "zip missing");
			VFSFileReference *ref = z->GetFile("dir/sub/deflated.txt");
			CHECK(ref, "zip ref");
			if (ref) {
				size_t size = 0;
				VFSOpenFile *of = z->OpenFileForRead(ref, &size);
				std::string part(1000, 0), rest(size, 0);
				CHECK(z->Read(of, &part[0], 1000) == 1000 && part == text.substr(0, 1000), "zip partial read");
				CHECK(z->Read(of, &rest[0], size) == size - 1000 && rest.substr(0, size - 1000) == text.substr(1000), "zip rest read");
				z->Rewind(of);
				CHECK(z->Read(of, &rest[0], size) == size && rest == text, "zip rewind read");
				z->CloseFile(of);
				of = z->OpenFileForRead(ref, &size);
				CHECK(z->Read(of, &rest[0], size) == size && rest == text, "zip whole read");
				z->CloseFile(of);
				z->ReleaseFile(ref);
			}
			delete z;
		}
	}
	printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
