@echo off
rem Build the 32-bit Windows XP build of pdflibdrv.dll.
rem
rem Three things differ from build-mt.cmd:
rem   * arch=x86 and compiler.toolset=v141_xp (see
rem     profiles\win-x86-xp-static-release);
rem   * the Visual Studio generator, because the XP toolset is an MSBuild
rem     concept - Ninja cannot select it;
rem   * every dependency is rebuilt from source again, this time for x86.
rem
rem Requirements: the Visual Studio "Windows XP Support for C++" component
rem and the Windows 7.1A SDK. Both are checked by the compiler step, so a
rem missing one fails loudly rather than silently producing a Vista+ DLL.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
if errorlevel 1 ( echo [ERR] vcvars32 failed & exit /b 1 )
set "PATH=C:\Program Files\CMake\bin;D:\Python313\Scripts;%PATH%"
set "PYTHON=D:\Python313\python.exe"
cd /d E:\github\pdflib

echo [1/5] conan install (x86 + XP toolset + static CRT)
rem Host profile is the 32-bit XP one; the BUILD profile stays native
rem (x86_64) so build tools like ninja/meson/pkgconf resolve to cached
rem binaries instead of being recompiled for x86 - compiling ninja for
rem i686 is pointless and needs its sources downloaded.
"%PYTHON%" tools\conan_main.py install . -of build\xp ^
  -pr:h profiles\win-x86-xp-static-release -pr:b default ^
  -o with_libharu=False -o with_podofo=True --build=missing
if errorlevel 1 exit /b 1

rem The install appended this layout to CMakeUserPresets.json; two layouts
rem that both define "conan-release"/"conan-default" make EVERY
rem `cmake --preset` call fail. Keep the all-backends layout listed; this
rem script configures with explicit -G/-A/-T flags, not presets.
"%PYTHON%" tools\pin_user_presets.py build/all-backends/build/Release/generators/CMakePresets.json
if errorlevel 1 exit /b 1

echo [2/5] configure (Win32 + v141_xp, static CRT required)
cmake -S . -B build\xp-cmake -G "Visual Studio 15 2017" -A Win32 -T v141_xp ^
  -DCMAKE_TOOLCHAIN_FILE=build/xp/build/generators/conan_toolchain.cmake ^
  -DPDFLIB_WITH_LIBHARU=OFF -DPDFLIB_WITH_PODOFO=ON ^
  -DPDFLIB_BUILD_TESTS=ON -DPDFLIB_BUILD_EXAMPLES=OFF ^
  -DPDFLIB_BUILD_DRIVER=ON -DPDFLIB_DRIVER_STATIC_CRT=ON
if errorlevel 1 exit /b 1

echo [3/5] build
cmake --build build\xp-cmake --config Release
if errorlevel 1 exit /b 1

echo [4/5] tests
ctest --test-dir build\xp-cmake -C Release --output-on-failure
if errorlevel 1 exit /b 1

echo [5/5] XP import audit
rem Machine/subsystem version plus an import-table audit: no dependency
rem that XP lacks, and no function from the known post-XP API set.
rem vcvars32 does not necessarily put dumpbin on PATH, so address it
rem through VCToolsInstallDir (set by vcvars) with a PATH fallback.
set "DUMPBIN=%VCToolsInstallDir%bin\HostX86\x86\dumpbin.exe"
if not exist "%DUMPBIN%" set "DUMPBIN=dumpbin"
"%PYTHON%" tools\check_xp_imports.py "%DUMPBIN%" build\xp-cmake\Release\pdflibdrv.dll
if errorlevel 1 exit /b 1
echo [OK] x86 + Windows XP build verified
