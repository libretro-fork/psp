#include <cstring>
#include <string>

#include <encodings/crc32.h>

#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Common/File/VFS/ZipFileReader.h"
#include "Core/Loaders.h"
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
	return true;
}
