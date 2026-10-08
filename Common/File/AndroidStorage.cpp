#include <inttypes.h>

#include "Common/File/AndroidStorage.h"
#include "Common/StringUtils.h"
#include "Common/Log.h"
#include "Common/TimeUtil.h"
#include "Common/System/System.h"


// These strings should never appear except on Android.
// Very hacky.
std::string g_extFilesDir = "(IF YOU SEE THIS THERE'S A BUG)";
std::string g_externalDir = "(IF YOU SEE THIS THERE'S A BUG (2))";

