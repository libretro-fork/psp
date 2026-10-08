// Reads an image (ISO, CSO, CHD, or an ISO inside a zip) through the loader chain
// and block devices, and compares every block with the plain ISO.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "Common/File/Path.h"
#include "Core/Loaders.h"
#include "Core/FileSystems/BlockDevices.h"

static std::vector<uint8_t> ReadAll(const char *path) {
	std::ifstream f(path, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char **argv) {
	if (argc < 3) { printf("usage: blocktest ref.iso image\n"); return 2; }
	std::vector<uint8_t> ref = ReadAll(argv[1]);
	const u32 refBlocks = (u32)(ref.size() / 2048);
	std::string err;
	IdentifiedFileType type;
	FileLoader *loader = ResolveFileLoaderTarget(ConstructFileLoader(Path(argv[2])), &type, &err);
	std::unique_ptr<FileLoader> loaderOwner(loader);
	if (!loader || !loader->Exists()) { printf("FAIL open %s: %s\n", argv[2], err.c_str()); return 1; }
	std::unique_ptr<BlockDevice> dev(ConstructBlockDevice(loader, &err));
	if (!dev) { printf("FAIL device %s: %s\n", argv[2], err.c_str()); return 1; }
	if (dev->GetNumBlocks() != refBlocks) { printf("FAIL blocks %u vs %u\n", dev->GetNumBlocks(), refBlocks); return 1; }
	std::vector<uint8_t> buf(2048 * 64);
	int bad = 0;
	// Sequential, mixed run lengths.
	std::mt19937 rng(42);
	for (u32 b = 0; b < refBlocks;) {
		int n = 1 + rng() % 40;
		if (b + n > refBlocks) n = refBlocks - b;
		if (!dev->ReadBlocks(b, n, buf.data()) || memcmp(buf.data(), &ref[(size_t)b * 2048], (size_t)n * 2048) != 0) {
			if (bad++ < 5) printf("mismatch seq at %u (+%d)\n", b, n);
		}
		b += n;
	}
	// Random single and multi-block reads.
	for (int i = 0; i < 4000; i++) {
		u32 b = rng() % refBlocks;
		int n = (i & 1) ? 1 : 1 + rng() % 64;
		if (b + n > refBlocks) n = refBlocks - b;
		bool ok = n == 1 ? dev->ReadBlock(b, buf.data()) : dev->ReadBlocks(b, n, buf.data());
		if (!ok || memcmp(buf.data(), &ref[(size_t)b * 2048], (size_t)n * 2048) != 0) {
			if (bad++ < 10) printf("mismatch rnd at %u (+%d)\n", b, n);
		}
	}
	// Byte ranges at any alignment, as file reads ask for them.
	std::vector<uint8_t> bytes(300000);
	for (int i = 0; i < 2000; i++) {
		const u64 total = (u64)refBlocks * 2048;
		u64 off = ((u64)rng() << 16 ^ rng()) % total;
		size_t len = (i % 3 == 0) ? rng() % 64 : rng() % bytes.size();
		if (off + len > total) len = (size_t)(total - off);
		if (!dev->ReadBytes(off, len, bytes.data()) || memcmp(bytes.data(), &ref[off], len) != 0) {
			if (bad++ < 15) printf("mismatch bytes at %llu (+%zu)\n", (unsigned long long)off, len);
		}
	}
	printf("%s %s: %u blocks, %d bad\n", bad ? "FAIL" : "PASS", argv[2], refBlocks, bad);
	return bad ? 1 : 0;
}
