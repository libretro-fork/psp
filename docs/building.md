# Building and testing PPSSPP

Agent-oriented notes on the various build systems and test suites. This is the long version of the
"Build and validation" / "Testing" sections in [AGENTS.md](../AGENTS.md).

## Build and Validation

The supported targets are the libretro core and the headless tools (PPSSPPHeadless and the unit
tests). For the core, `make -C libretro -j32` (`platform=` picks the target, see `libretro/Makefile`)
or `./b.sh --libretro`; for the headless tools, `./b.sh --headless --unittest PPSSPPHeadless
PPSSPPUnitTest`.

In addition to the pspautotests runner (test.py), there is a separate binary with C++ unit tests
in the /unittest subdirectory. After substantial changes (at the end of a chunk of work, not
necessarily after every edit), run these too:

- Configure with `-DUNITTEST=ON`, then run `build/PPSSPPUnitTest all`

This runs all tests in `availableTests` in unittest/UnitTest.cpp. You can run one or more
specific tests by passing their names instead of `all` (space-separated, e.g. `PPSSPPUnitTest
CmdLine Path Utf8`); no arguments lists the available tests.

The libretro Makefile tracks C/C++ header dependencies. Build clean when changing toolchains or
compiler flags, or when migrating objects produced before dependency sidecars were enabled. Mixing
stale objects with changed class layouts can produce crashes from disagreeing translation units.

More generally, **when you are bisecting a behavioural change, confirm the binary actually changed
before you believe the result** - check the executable's mtime, or have the code you just added log
something you can grep for. A stale binary is indistinguishable from a real regression, and it lies
consistently, so a bisect on top of one produces a confident, entirely fictional answer.

Known environment-specific issue: in at least one sandboxed dev environment, the `Jit` test
(`unittest/JitHarness.cpp`) hangs indefinitely specifically during the `CPUCore::JIT_IR`
phase - confirmed unrelated to source changes (reproduces identically on unmodified checkouts)
and not a memory-access fault (`Memory::HandleFault` is never entered). Root cause wasn't
pinned down further (would need a native debugger attached to the hung process, not available
in that environment) but is very likely specific to that sandbox rather than a real PPSSPP
bug, since CI runs `PPSSPPUnitTest all` on every commit across multiple
platforms without apparent issue. If `all`/`Jit` hangs in your environment, run every other
test by name instead (skip `Jit`) to still get real coverage.

## libretro core build (Windows)

Canonical instructions are in `libretro/README_WINDOWS.txt` - read that first, this is a summary plus
agent-specific gotchas. The libretro core (`ppsspp_libretro.dll`) is built with a real `make` - it uses `cl.exe`/`link.exe` as the compiler/linker (via
`platform=windows_msvc2019_desktop_x64`), but orchestrated through GNU Make running inside an MSYS2
shell (a plain MSYS2 install, not "Git Bash" - typically at `C:\msys64`, needs `pacman -S make`).

```sh
cd libretro
make DEBUG=1 platform=windows_msvc2019_desktop_x64 -j32
```

(drop `DEBUG=1` for a release build; `-j` count doesn't need to match logical CPUs exactly). To test the
result, copy `ppsspp_libretro.*` into wherever the local RetroArch install reads cores from (e.g. its
`cores/` directory) and load it from within RetroArch.

An agent can drive this non-interactively by invoking `C:\msys64\usr\bin\bash.exe -lc "..."` directly
as a subprocess (the `-l` login-shell flag matters - it's what sets up MSYS2's own `PATH`, `make`,
`cygpath`, etc. correctly). In a sandboxed/agentic invocation (as opposed to a normal interactive MSYS2
terminal a human opens), two Windows environment variables the Makefile's VS-detection logic depends on
may not be inherited by the spawned process - `COMSPEC` (breaks the `cmd //c "bash VSWhere.sh ..."` call
used to locate Visual Studio) and `ProgramFiles(x86)` (which `VSWhere.sh` itself needs to find
`vswhere.exe`). If VS auto-detection fails this way, skip it by overriding `VsInstallRoot` directly on
the `make` command line (GNU Make command-line variables take precedence over the Makefile's own `:=`
assignment of the same name):

```sh
make VsInstallRoot="/c/Program Files/Microsoft Visual Studio/<year>/<edition>" DEBUG=1 platform=windows_msvc2019_desktop_x64 -j32
```

(path in MSYS2/cygpath POSIX form, not a raw Windows path; find the real value via `vswhere -latest
-property installationPath` if unsure of `<year>/<edition>`). This is a real full compile+link - prefer
it over trying to syntax-check libretro-specific files with a standalone `cl.exe /Zs` invocation, which
can miss real bugs (e.g. an include-order issue that leaves a platform macro like
`VK_USE_PLATFORM_WIN32_KHR` undefined before `vulkan.h`'s first, include-guarded inclusion, since a
narrower manual include-path/define set used for a syntax-only check may not reproduce the actual build
step's ordering).

## Headless and unittest builds

We have additional PPSSPPHeadless and unit test builds (/headless and /unittest), that have their own separate
main functions (and also stub out most of the System_ functions as needed). Take these into account
when making cross platform changes.

New unit tests are added by listing them in availableTests in unittest.cpp. If they are large, put them in
separate files in the unittest subdirectory, listed in `CMakeLists.txt`.

A unit test is often the first thing to call a given function from outside its own .cpp: MSVC links an
`inline` function defined in a .cpp anyway, clang correctly does not, so a test can link on one and not the
other, with an undefined symbol pointing at a header line. Fix it by dropping the bogus `inline` from the
definition, not by avoiding the call.

The compilers also disagree about floating point contraction, which matters for any test asserting that a JIT
is bit-identical to its C++ reference. Clang folds `a * b + c` into a single fused multiply-add by default;
MSVC never does, under `/fp:precise`, in Debug or Release. So on arm64, where the JITs emit `FMLA`, a
reference written as `a * b + c` matches on Mac, Linux and Android and is off by one ULP on Windows on ARM.
Don't leave it to the compiler: write `fmaf(a, b, c)` when the fused result is wanted (MSVC compiles it to a
single `fmadd`), and `a * b + c` when it isn't. `PrescaleUV` in `GPU/Common/VertexDecoderCommon.cpp` picks per
architecture, matching what each JIT does. x86 doesn't have the problem, since the SSE2 baseline has no FMA
instruction to contract into.

pspautotests are a large set of tests of the PSP OS's API surface, and thus tests our HLE implementation.

**To check for regressions, run them exactly the way CI does** (see `.github/workflows/build.yml`):

```bash
python test.py -g --graphics=software
```

**The `-g` matters.** `test.py` keeps two lists: `tests_good` (the regression set - these pass and must keep
passing, ~314 of them) and `tests_next` (work-in-progress tests that are *expected* to fail, i.e. the to-do list).
`-g` runs only `tests_good`; with no flag you get `tests_next + tests_good` and around a hundred failures that mean
nothing is wrong. Don't go hunting those, and don't report them as regressions - the only meaningful result from
`-g` is `0 tests failed`. (`-b` runs only `tests_next`; `-m` prefix-filters whichever list is selected.)

See docs/pspautotests.md for a workflow for running pspautotests and improving PPSSPP with the results.

### LoongArch64 and RISC-V JITs under qemu

CI tests these JITs by cross-building headless and running pspautotests under qemu-user, and you can do the
same on Linux (packages: `gcc-14-loongarch64-linux-gnu g++-14-loongarch64-linux-gnu qemu-user`, or the riscv64
equivalents):

```bash
./b.sh --loongarch64 PPSSPPHeadless     # builds into build-loongarch64/
mkdir -p build-qemu
printf '#!/bin/bash\nexec qemu-loongarch64 -L /usr/loongarch64-linux-gnu "$(dirname "$0")/../build-loongarch64/PPSSPPHeadless" "$@"\n' > build-qemu/PPSSPPHeadless
chmod +x build-qemu/PPSSPPHeadless
python3 test.py -g --graphics=software --cpu=jit-ir --timeout=60 --known-failures=loongarch64
```

`test.py` runs the most recently modified `build*/PPSSPPHeadless`, so `touch` the shim after rebuilding the real
binary, or it runs your native build instead. The full suite takes a couple of minutes.

qemu reports LSX and LASX as present, so CI only exercises the LoongArch vector paths.
The scalar fallbacks never run here, so passing tells you nothing about them.

Without a cross toolchain (on macOS, say) two things still help:

- **A syntax check of the other JITs.** The RISC-V and LoongArch backends (`Core/MIPS/RiscV`,
  `Core/MIPS/LoongArch64` and their emitters) have no architecture guards, so the host compiler can check
  them: `clang++ -fsyntax-only` per file, with the `-I`/`-D`/`-std`/`-isystem` flags of any Core file from
  `compile_commands.json`, plus `-I<repo> -I<repo>/ext -I<repo>/Common` (the database's include paths are
  relative to the build directory). Check that a copy with a deliberate syntax error does report errors: zsh
  doesn't word-split a `$FLAGS` variable, which once made a broken check look clean. This proves the code
  compiles, nothing about its output.
- **The portable software-renderer paths.** riscv64 and loongarch64 run the software renderer's non-SSE,
  non-NEON C++ paths, so their test failures reproduce natively: wrap the two NEON defines in the
  `__aarch64__` block of `ppsspp_config.h` in `#ifndef PPSSPP_TEST_NO_SIMD`, configure a separate
  `build-nosimd` with `-DHEADLESS=ON` and `-DPPSSPP_TEST_NO_SIMD` in `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS`,
  and build `PPSSPPHeadless` there. Delete it and revert `ppsspp_config.h` afterwards, since `test.py` keeps
  picking the newest binary. A failure that's the same on both architectures points at the portable path, not
  the JITs.

## Quick rebuild on Linux

You don't need to do ./b.sh --debug to verify every single little change, instead use this shortcut:

```bash
cd build ; make -j32; cd ..
```
