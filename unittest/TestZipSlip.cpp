#include <cstring>
#include <string>

#include <encodings/crc32.h>

#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Common/File/VFS/ZipFileReader.h"
#include "Core/Loaders.h"
#include "Core/Util/GameManager.h"
#include "Core/Util/PathUtil.h"

#include "UnitTest.h"

static bool TestHasParentDirComponent() {
	EXPECT_TRUE(HasParentDirComponent("../../evil.txt"));
	EXPECT_TRUE(HasParentDirComponent("game/../../evil.txt"));
	EXPECT_TRUE(HasParentDirComponent(".."));
	EXPECT_TRUE(HasParentDirComponent("sub/.."));
	EXPECT_TRUE(HasParentDirComponent("..\\evil.txt"));
	EXPECT_TRUE(HasParentDirComponent("a/b/../.."));
	EXPECT_FALSE(HasParentDirComponent("normal.txt"));
	EXPECT_FALSE(HasParentDirComponent("game/evil.txt"));
	EXPECT_FALSE(HasParentDirComponent("a.b/c.d"));
	EXPECT_FALSE(HasParentDirComponent(""));
	EXPECT_FALSE(HasParentDirComponent("/absolute/path.txt"));
	return true;
}

// Creates a zip archive at the given path with one stored entry of the given name.
static void Put16(std::string &s, uint32_t v) { s += (char)(v & 0xFF); s += (char)((v >> 8) & 0xFF); }
static void Put32(std::string &s, uint32_t v) { Put16(s, v & 0xFFFF); Put16(s, v >> 16); }

static bool CreateZipWithEntry(const Path &zipPath, const std::string &entryName, const std::string &contents) {
	const uint32_t crc = encoding_crc32(0, (const uint8_t *)contents.data(), contents.size());
	std::string zip;
	Put32(zip, 0x04034b50); Put16(zip, 20); Put16(zip, 0); Put16(zip, 0); Put16(zip, 0); Put16(zip, 0);
	Put32(zip, crc); Put32(zip, (uint32_t)contents.size()); Put32(zip, (uint32_t)contents.size());
	Put16(zip, (uint32_t)entryName.size()); Put16(zip, 0);
	zip += entryName;
	zip += contents;
	const uint32_t cdOffset = (uint32_t)zip.size();
	Put32(zip, 0x02014b50); Put16(zip, 20); Put16(zip, 20); Put16(zip, 0); Put16(zip, 0); Put16(zip, 0); Put16(zip, 0);
	Put32(zip, crc); Put32(zip, (uint32_t)contents.size()); Put32(zip, (uint32_t)contents.size());
	Put16(zip, (uint32_t)entryName.size()); Put16(zip, 0); Put16(zip, 0); Put16(zip, 0); Put16(zip, 0);
	Put32(zip, 0); Put32(zip, 0);
	zip += entryName;
	const uint32_t cdSize = (uint32_t)zip.size() - cdOffset;
	Put32(zip, 0x06054b50); Put16(zip, 0); Put16(zip, 0); Put16(zip, 1); Put16(zip, 1);
	Put32(zip, cdSize); Put32(zip, cdOffset); Put16(zip, 0);
	return File::WriteDataToFile(false, zip.data(), zip.size(), zipPath);
}

// Crafts a zip with a parent-directory entry and verifies ExtractZipContents
// refuses to write outside the destination directory (Zip Slip).
static bool TestZipSlipExtraction() {
	Path tempRoot = Path("unittest_zip_slip_test");
	File::DeleteDirRecursively(tempRoot);
	EXPECT_TRUE(File::CreateDir(tempRoot));

	Path destDir = tempRoot / "dest";
	EXPECT_TRUE(File::CreateDir(destDir));

	Path zipPath = tempRoot / "bad.zip";
	EXPECT_TRUE(CreateZipWithEntry(zipPath, "../evil.txt", "should not escape"));

	ZipContainer z(zipPath);
	EXPECT_TRUE((bool)z);

	ZipFileInfo info;
	info.numFiles = 1;
	info.stripChars = 0;
	info.ignoreMetaFiles = false;

	GameManager manager;
	EXPECT_TRUE(manager.ExtractZipContents(z, destDir, info, true));
	z.close();

	// The malicious file must not have been written outside destDir.
	// A naive "dest / ../evil.txt" would land here.
	EXPECT_FALSE(File::Exists(tempRoot / "evil.txt"));
	// And the actual file should not exist inside destDir either.
	EXPECT_FALSE(File::Exists(destDir / "evil.txt"));

	// Clean up.
	File::Delete(zipPath);
	File::DeleteDirRecursively(tempRoot);
	return true;
}

static bool TestIsSafePathComponent() {
	EXPECT_TRUE(IsSafePathComponent("ULUS10041"));
	EXPECT_TRUE(IsSafePathComponent("NPJH-50465"));
	EXPECT_TRUE(IsSafePathComponent("a.b"));
	EXPECT_TRUE(IsSafePathComponent("My Game"));
	EXPECT_FALSE(IsSafePathComponent(""));
	EXPECT_FALSE(IsSafePathComponent("."));
	EXPECT_FALSE(IsSafePathComponent(".."));
	EXPECT_FALSE(IsSafePathComponent("..."));
	EXPECT_FALSE(IsSafePathComponent(".. "));
	EXPECT_FALSE(IsSafePathComponent("../ULUS10041"));
	EXPECT_FALSE(IsSafePathComponent("..\\ULUS10041"));
	EXPECT_FALSE(IsSafePathComponent("C:"));
	EXPECT_FALSE(IsSafePathComponent("ULUS10041:stream"));
	EXPECT_FALSE(IsSafePathComponent(std::string_view("ULUS\0/..", 8)));
	EXPECT_FALSE(IsSafePathComponent("ULUS\n10041"));
	return true;
}

bool TestZipSlip() {
	if (!TestHasParentDirComponent())
		return false;
	if (!TestIsSafePathComponent())
		return false;
	if (!TestZipSlipExtraction())
		return false;
	return true;
}
