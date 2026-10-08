# Adding HLE modules

How the HLE module tables work, the savestate-compatibility rules for adding to them, and the list of
build files a new source file has to be added to.

HLE module implementations live in `Core/HLE/sce<ModuleName>.cpp` / `.h` (e.g. `sceOpenPSID.cpp`, `scePauth.cpp` are good
small examples to copy from). A module is a `const HLEFunction <name>[]` table of
`{nid, &WrapX_YYY<func>, "funcName", retChar, argString}` entries, registered via
`RegisterHLEModule("<name>", ARRAY_SIZE(table), table)` inside a `Register_<name>()` function declared in the header.

- `FunctionWrappers.h` has generic `WrapX_YYY<func>` templates for common signatures (return type X, args YYY) - add new
  wrappers there if you need a new signature.
- Format string legend for the `retmask`/argmask chars: `x` = u32 (shown as hex), `i` = int/s32, `f` = float, `X` = u64,
  `I` = s64, `v` = void.
- For functions of genuinely unknown purpose (only known by NID), name them `<moduleName>_<NID>` and stub them with
  `return hleLogError(Log::HLE, 0, "UNIMPL");` - an established pattern (see `scePauth.cpp`, `sceOpenPSID.cpp`).
- **New modules must be registered at the very end** of the registration function in `Core/HLE/HLETables.cpp` (look for
  the `// add new modules here.` comment near the end of that function) - not inserted alphabetically/logically among
  the existing `Register_*()` calls. Module registration order affects numeric IDs used in savestates, so inserting a
  new module earlier in that list would break save-state compatibility for saves made with older builds.
- **The same rule applies one level down: new entries in an *existing* module's `const HLEFunction <name>[]` table must
  go at the very end of that array too, never inserted alphabetically or next to a "related" existing entry.** A
  resolved import gets written directly into guest RAM as a syscall opcode that encodes the entry's array position
  (`modulenum<<18 | funcindex<<6`, see `GetSyscallOp()` in `Core/HLE/HLE.cpp`), and a savestate captures that opcode
  verbatim - inserting a new entry before an existing one shifts every later entry's index, so loading an older
  savestate after such a change resolves those later entries to the *wrong* function. Adding an all-new table (a new
  module, including a new alias-module `..._driver[]` variant of an existing one) is unaffected, since no old
  savestate could reference indices into a table that didn't exist yet - only *existing* tables need this care.
- Remember to add any new `.cpp`/`.c` file to both `Core/CMakeLists.txt` and `libretro/Makefile.common`, and
  new `.h` files to `Core/CMakeLists.txt`. Double check each against how an existing neighboring file (e.g.
  `sceVaudio.cpp`) is listed, and build both the CMake and the Makefile build.
