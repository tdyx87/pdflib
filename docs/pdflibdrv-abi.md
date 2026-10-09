# pdflibdrv - the C ABI a WDK print driver links against

This is the concrete first cut of the layer discussed earlier: a thin
facade DLL (`pdflibdrv.dll`) that exposes pdflib to a user-mode print
driver through a small, frozen, C-only ABI, and a harness that tests the
contract from the *consumer* side.

Nothing here has been run inside a real WDK 7600 / Win7 print pipeline.
What it *does* have is a build and a set of executable assertions, so the
parts that would crash `spoolsv.exe` / `PrintIsolationHost.exe` are
tested before they get anywhere near a printer.

## Files

| File | Role |
|---|---|
| [driver/pdflibdrv.h](../driver/pdflibdrv.h) | the ABI header; pure C, the only thing a driver includes |
| [driver/pdflibdrv.def](../driver/pdflibdrv.def) | the authoritative export list |
| [driver/pdflibdrv.cpp](../driver/pdflibdrv.cpp) | facade implementation (C++ TU so PoDoFo exceptions stop here) |
| [driver/drv_harness.c](../driver/drv_harness.c) | consumer-side contract test, resolves exports via `GetProcAddress` |
| [driver/c_link_check.c](../driver/c_link_check.c) | plain-C consumer that links the import library directly |
| [driver/check_exports.cmake](../driver/check_exports.cmake) | post-build export-table guard |
| [build-driver.cmd](../build-driver.cmd) | configure + build + test in the `all-backends` config (/MD, x64) |
| [build-mt.cmd](../build-mt.cmd) | static-CRT x64 build (`profiles/win-x64-static-release`) |
| [build-x86-xp.cmd](../build-x86-xp.cmd) | 32-bit Windows XP build (`profiles/win-x86-xp-static-release`) |
| [tools/check_xp_imports.py](../tools/check_xp_imports.py) | import-table audit for XP (no missing DLL, no post-XP API) |

## Design in one screen

```
driver's sink  <-- pd_write_fn (returns int; non-zero = permanent failure)
      ^
      |  sink_trampoline(): maps non-zero -> PDF_ERR_IO, latches PD_ERR_SINK
      |
   pdflib core  <-- pdf_doc_create_streamed(pdf_stream_write_fn, ...)
      |
   PoDoFo (static)
```

* Only the 14 names in `pdflibdrv.def` are exported. The core's ~80
  `pdf_*` entry points are **not** reachable: the facade is a separate
  target and never defines `PDFLIB_SHARED`, so nothing is
  `dllexport`ed.
* Handles are opaque. The facade keeps its own live-handle registry and
  unlinks a job *before* touching it, so a forged, NULL or already-used
  handle is a returned error, not a dereference.
* Every entry point is wrapped in `catch(...)`.
* No work in `DllMain`; `PdflibDrvStartup` is the explicit init point.
* Standard-font indices (`PD_FONT_*`), paint modes (`PD_PAINT_*`) and the
  `pd_status` values are compile-time asserted equal to the core's
  (`static_assert` block at the top of `pdflibdrv.cpp`), so the two
  layers cannot drift silently.

## Is it really a C interface?

Yes, and it is checked two independent ways, because the two halves of
"a C interface" fail differently:

1. **Link-time / calling convention** - [driver/c_link_check.c](../driver/c_link_check.c)
is a `.c` file compiled by the C compiler. It includes only
`pdflibdrv.h`, calls the functions **by name**, and is linked against
`pdflibdrv.lib` (the `.def` import library). There is no
`GetProcAddress` and there is no C++ anywhere in the path. If an export
were C++-mangled, `__stdcall`-decorated (`_Name@N`) or absent from the
import library, this would fail to link.
2. **Runtime** - [driver/drv_harness.c](../driver/drv_harness.c) resolves
every one of the 14 exports by its exact literal name through
`GetProcAddress`, which would fail on any decoration.

Both run in both configurations:

```
ok: linked against pdflibdrv.lib from C, abi 1.0, pdflib 1.0.0,
    760 sink call(s), 2074 byte(s)
```

Supporting properties, all verified:

* `pdflibdrv.h` includes nothing but `<stddef.h>` and is wrapped in
  `extern "C"` - no C++ type, no class, no template, no `bool`.
* No handle is a struct the driver can see; every one is `void *`.
* The DLL exports **exactly** the 14 names in the `.def`, verified by
  `dumpbin /exports` and by a build-time guard.
* Even though the *implementation* is C++ (so PoDoFo exceptions stop at
  the boundary), the `/MT` DLL imports no C++ runtime at all - no
  `MSVCP140.dll` - so nothing C++-shaped leaks onto the driver.

## Build

There are two configurations, and the difference is the C runtime.

**/MD (quick, development only)** - reuses the prebuilt conan backends:

```sh
# from a VS2017 x64 prompt (or just run build-driver.cmd)
cmake --preset all-backends -DPDFLIB_BUILD_DRIVER=ON
cmake --build --preset all-backends
cd build/all-backends && ctest -R pdflibdrv_harness --output-on-failure
```

**/MT x64 (what a driver package ships)** - rebuilds every backend with
the static CRT; see the next section. Just run `build-mt.cmd`.

**x86 + Windows XP** - 32-bit and XP-targeted; just run
`build-x86-xp.cmd`, described below.

`PDFLIB_BUILD_DRIVER` defaults to **OFF**, so existing builds and the
`core-only` preset are unaffected (verified: a `-DPDFLIB_BUILD_DRIVER=OFF`
`libharu=OFF podofo=OFF` configure + build succeeds).

## Static CRT (/MT) - `build-mt.cmd`

A print driver must not depend on the VC++ redistributable being present
inside `PrintIsolationHost.exe`. Two things are needed, and neither is
a linker flag you can just add at the end:

1. **Every static library has to be rebuilt with the static CRT.**
   libharu/PoDoFo/freetype/libpng/zlib/bzip2/brotli were all built `/MD`,
   so linking them into a `/MT` DLL fails with `LNK2038`
   (`RuntimeLibrary` mismatch). `build-mt.cmd` therefore runs
   `conan install` with
   [profiles/win-x64-static-release](../profiles/win-x64-static-release)
   (`compiler.runtime=static`) and `--build=missing`, which recompiles
   the whole chain from source. libharu is dropped entirely
   (`with_libharu=False`) since the facade only exposes PoDoFo.
2. **`PDFLIB_DRIVER_STATIC_CRT=ON`** makes the post-build guard run
   `dumpbin /dependents` and *fail the build* if any
   `VCRUNTIME/MSVCP/MSVCR/UCRTBASE/api-ms-win-crt*` import remains.

Verified result (`build/mt-cmake/pdflibdrv.dll`):

```
Image has the following dependencies:
    WS2_32.dll
    KERNEL32.dll
```

Compare the `/MD` build, which imports `MSVCP140.dll`,
`VCRUNTIME140.dll` and ten `api-ms-win-crt-*` forwarders.

| | /MT | /MD |
|---|---|---|
| DLL dependencies | WS2_32, KERNEL32 | + 12 CRT DLLs |
| size | 2,155,520 B | 1,893,888 B |
| exports | 14 | 14 |
| ctest | 5/5 | 5/5 |
| contract harness | 42 checks, 0 fail | 42 checks, 0 fail |
| C link check | pass | pass |

### Gotchas that were actually hit while doing this

* **conan's `win-x64-static-release-2017` profile is unusable** with conan
  2.25: it sets `compiler.toolset=v141`, and the only accepted toolset
  values are the `*_xp` variants (`Invalid setting 'v141'`).
* **libjpeg does not need to be built at all.** It is pulled in by
  PoDoFo's `with_jpeg` default, and its bundled `jpeg.sln` targets the
  *Windows 8.1 SDK*, which need not be installed (`MSB8036`). Worse,
  PoDoFo 0.9.7 then fails to compile against libjpeg's header
  (`error C2375: jpeg_memory_src: redefinition with different linkage`).
  `with_jpeg`/`with_tiff` are now pinned to `False` in
  [conanfile.py](../conanfile.py) so the dynamic and static graphs cannot
  diverge. (This also matches the package that the pre-existing
  all-backends build already used.)
* **Do not put `tools.microsoft:winsdk_version` in the profile.** It
  makes conan emit the `CMAKE_GENERATOR_PLATFORM` block unconditionally
  (`GenericSystemBlock`), and `cmake -G Ninja` then dies with
  "Ninja does not support platform specification". It only existed to
  work around the libjpeg SDK problem, which is gone.
* **`tools.cmake.cmaketoolchain:generator=Ninja` *is* required** - it is
  what makes conan emit `set(CMAKE_C_COMPILER cl)` instead of VS
  platform/toolset variables.
* **Two conan layouts cannot coexist in `CMakeUserPresets.json`.** Each
  install appends its generators file, and both define a preset called
  `conan-release`, after which *every* `cmake --preset` fails with
  `Duplicate preset`. `build-mt.cmd` calls
  [tools/pin_user_presets.py](../tools/pin_user_presets.py) to keep the
  list to layouts that coexist.
* **`conan.exe` itself does not run here** (its launcher points at a
  Python that is not installed), so the scripts go through
  [tools/conan_main.py](../tools/conan_main.py).
* **`vcvarsall.bat` cannot find the Windows SDK on this machine.** It
  prints `[ERROR:winsdk.bat] Windows SDK ...: 'include\<ver>\um' not
  found or was incomplete` - with an *empty* `%WindowsSdkDir%` in the
  message - and leaves `INCLUDE`/`LIB` holding only the VC paths, so
  `cl` fails with `C1083: cannot open include file 'windows.h'`. The SDK
  itself is installed and complete. Pre-setting `WindowsSdkDir` before
  the call does not help. Both Ninja builds therefore append the SDK
  include/lib paths explicitly. This is invisible to the XP build
  because MSBuild resolves the SDK itself. It is also why these scripts
  were quietly dependent on the ambient environment until it changed.

## 32-bit, Windows XP - `build-x86-xp.cmd`

Three things have to be true at once, and each one is checked:

1. **the toolset** must be `v141_xp`. That is an MSBuild concept, so this
   build uses the Visual Studio generator - `cmake -G "Visual Studio 15
   2017" -A Win32 -T v141_xp` - not Ninja. It is what brings in
   `_USING_V110_SDK71_`, the Windows **7.1A** SDK include/lib paths and
   `/SUBSYSTEM:5.01`.
2. **the runtime** must be static. A VS2015+ VC++ redistributable does not
   install on XP, so `/MD` is not an option.
3. **nothing may import a post-XP API.** The 7.1A import libraries
   themselves contain Vista/Win7 symbols, so "it links" proves nothing;
   what matters is which functions the final image actually imports.

Prerequisites (checked below, and both outside a stock VS2017 install):

```
Common7\IDE\VC\VCTargets\Platforms\Win32\PlatformToolsets\v141_xp
C:\Program Files (x86)\Microsoft SDKs\Windows\v7.1A\Lib\kernel32.Lib
```

### Verified result - `build/xp-cmake/Release/pdflibdrv.dll`

```
14C machine (x86)
5.01 operating system version
5.01 subsystem version

14 number of functions / 14 number of names
  1 PdflibDrvGetAbiVersion     6 PdflibDrvPageAdd
  2 PdflibDrvJobAbort          7 PdflibDrvPageDrawLine
  3 PdflibDrvJobCreate         8 PdflibDrvPageDrawRect
  4 PdflibDrvJobFinish         9 PdflibDrvPageDrawText
  5 PdflibDrvJobLastError     10 PdflibDrvPageSetRotation
 11 PdflibDrvShutdown         13 PdflibDrvStatusString
 12 PdflibDrvStartup          14 PdflibDrvVersionString

machine: x86
version: os 5.01 / subsystem 5.01
checks : imports from 2 module(s)
  WS2_32.dll   ok
  KERNEL32.dll ok
post-XP API usage
  none of the 99 known post-XP functions are imported
RESULT: no post-XP dependency or API found
```

**This is also where the undecorated-`__stdcall` claim is finally proven.**
On x86 a `__stdcall` function exported with `__declspec(dllexport)` alone
would appear as `_PdflibDrvJobCreate@16`; the four 0-argument functions
would be `_PdflibDrvStartup@0` and so on. All fourteen names above are
bare, which is what putting them in the `.def` buys. `ctest`: 5/5, and
both the `GetProcAddress` harness and the plain-C link check pass on the
32-bit DLL.

### The XP audit - `tools/check_xp_imports.py`

It reports the machine type, the OS/subsystem version, every dependent
DLL and any imported function from a curated set of post-XP APIs. It is
a heuristic, not a proof - it cannot catch an API that exists on XP but
behaves differently there. **It does catch the failure that actually
happened here:** the first version of the facade called
`InitOnceExecuteOnce` for its one-time init, which is Vista+, and the
audit flagged it immediately. [driver/pdflibdrv.cpp](../driver/pdflibdrv.cpp)
now uses the same `InterlockedCompareExchange` state machine as
`pdf_ensure_backends()` in the core, which is XP-safe (and the x64 `/MT`
DLL was rebuilt and re-audited clean afterwards).

## What is verified (this checkout, VS2017 14.16 x64, PoDoFo 0.9.x)

**Export surface** - `dumpbin /exports build/all-backends/pdflibdrv.dll`
reports `14 number of functions / 14 number of names`, all undecorated:
`PdflibDrvGetAbiVersion, JobAbort, JobCreate, JobFinish, JobLastError,
PageAdd, PageDrawLine, PageDrawRect, PageDrawText, PageSetRotation,
Shutdown, Startup, StatusString, VersionString`.

* The export set is exactly the `.def` list - no more, no less - and the
  post-build guard reproduces that automatically
  (`check_exports: 14 exported name(s) verified against pdflibdrv.def`).
* The guard is not vacuous: injecting a bogus name into the `.def` makes
  it fail with `missing: PdflibDrvNotReal`.
* LF-only line endings in the `.def` work fine with this `link.exe`
  (this was previously an open question).

**Contract harness** - `drv_harness.exe` loads the DLL with
`LoadLibraryA` and resolves every export with `GetProcAddress`:
`40 check(s), 1 failure(s)` -> after fixing the harness itself,
`42 check(s), 0 failure(s)`, and it passes under `ctest`. The checks
cover:

| Area | Result |
|---|---|
| all 14 exports resolvable by name | pass |
| ABI version handshake (1.0), `Startup` idempotent | pass |
| healthy 3-page streamed job | pass |
| output starts `%PDF-`, contains `%%EOF` | pass |
| `startxref` offset points at an `xref` table | pass |
| sink fails on 3rd call -> `JobFinish` = `PD_ERR_SINK` | pass |
| **zero further sink deliveries after a failure** | pass |
| abort: bytes delivered before, none after | pass |
| forged/NULL handle -> `PD_ERR_INVALID_HANDLE`, no crash | pass |
| double `JobFinish` / `JobAbort` -> rejected | pass |
| `Shutdown` releases an unfinished job; `Startup` works again | pass |

**Sink call granularity** (worth knowing before writing the driver's
sink): a 3-page document produced **926 sink calls for 2968 bytes** -
about 3 bytes per call. PoDoFo's streaming writer emits token by token.
The driver's sink is therefore on a very hot path and must not do
per-call allocation or a syscall; buffer it.

**Full suite** - `ctest` in `build/all-backends` and `build/mt-cmake`:
5/5 pass (`pdflib_tests`, `pdflib_thread_tests`, `pdflib_complex_tests`,
`pdflibdrv_harness`, `pdflibdrv_c_link_check`).

## Claims from the earlier write-up that this corrected

1. **`/MT` cannot be had by changing the facade alone.** The conan
   backends were built with `compiler.runtime=dynamic`, so the facade
   linked `/MD` and the DLL imported `MSVCP140/VCRUNTIME140/ucrtbase`.
   Linking the facade `/MT` against those libraries is not a flag you
   can add: it fails with `LNK2038` (`RuntimeLibrary` mismatch). Fixing
   it means rebuilding the entire dependency chain with
   `compiler.runtime=static` - now done, see "Static CRT (/MT)" above.
2. **`.def` alone is enough**; the header deliberately carries no
   `dllexport`/`dllimport`. `LIBRARY pdflibdrv` matches the output name,
   so there is no `LNK4070`.

## Known gaps / not verified

* **No WDK 7600, no Win7 print stack.** The DLL has never been loaded
  into `spoolsv.exe` or `PrintIsolationHost.exe`, and has not been built
  with the WDK 7600-era `link.exe` (9.00) or the VS2008 compiler.
  `stdint.h`/`snprintf` availability under VC9 is still an assumption.
* **Never executed on XP itself.** The x86 DLL is *targeted* at XP
  (machine x86, subsystem 5.01, only WS2_32/KERNEL32, no post-XP API on
  the denylist) and the full test suite does execute it here, but under
  WOW64 on Windows 10 - not on XP. This checkout has no XP machine, so
  "it loads and prints on XP" remains unverified.
* **The XP audit is a denylist.** 99 functions is not the complete
  post-XP API surface of kernel32/advapi32/user32/ws2_32.
* **Dependencies are not built with the XP toolset.** freetype goes
  through meson, which has no notion of `v141_xp`; the workaround is the
  profile's `tools.build:defines=["_USING_V110_SDK71_"]` plus the fact
  that the final link resolves imports against the 7.1A SDK libraries.
  It is verified by the audit, not by construction.
* **Zero-length sink calls are accepted but never exercised** - PoDoFo
  sends none in these tests (`0 zero-length` calls reported).
* **Abort is slightly wasteful**: `PdflibDrvJobAbort` currently drops
  the job through `pdf_doc_free`, which internally invokes
  `pdf_doc_finish`; the first write is rejected because the job is
  marked aborted, so the file is not completed, but PoDoFo still runs
  its finalization path. Harmless, but a `pdf_doc_abort`-style core
  entry point would be cleaner.
* **Signature / packaging** (catalog file, WHQL, `DriverIsolation`) is
  untouched - it is a process question, not a code one.
* **Not built into the CMake `install()` set**; the DLL is currently a
  build-tree artifact only.

## Natural next steps

1. **Run the x86 DLL on a real XP SP3 box** (or an XP VM) and print
   through it - the audit says it *should* load, but that is not the
   same as it having loaded.
2. Add the XPSDrv fidelity gaps the facade still lacks: glyph runs
   (GID + advance) and clip paths (`clip`/`shading` are absent from
   `pdf_api.h`).
3. Give the core a real per-document backend selector or accept that
   `pdflib_set_backend` remains process-global (the facade serialises
   creation to compensate).
