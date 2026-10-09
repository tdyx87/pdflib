@echo off
rem Build pdflibdrv.dll with the STATIC CRT (/MT), so a print driver
rem package does not need the VC++ redistributable inside the print
rem isolation host.
rem
rem Every dependency is rebuilt from source with compiler.runtime=static
rem by conan (profile: profiles\win-x64-static-release). That is the
rem slow part; the pdflib build itself is quick.
rem
rem Conan is invoked through tools\conan_main.py because the conan.exe
rem launcher on this machine points at a Python that is not installed.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [ERR] vcvars64 failed & exit /b 1 )

rem vcvarsall's Windows SDK detection is broken on this machine (see the
rem same block in build-driver.cmd); add the installed SDK explicitly.
set "SDKROOT=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.17763.0"
if not exist "%SDKROOT%\Include\%SDKVER%\um\windows.h" ( echo [ERR] Windows SDK %SDKVER% not found & exit /b 1 )
set "INCLUDE=%SDKROOT%\Include\%SDKVER%\ucrt;%SDKROOT%\Include\%SDKVER%\shared;%SDKROOT%\Include\%SDKVER%\um;%SDKROOT%\Include\%SDKVER%\winrt;%INCLUDE%"
set "LIB=%SDKROOT%\Lib\%SDKVER%\ucrt\x64;%SDKROOT%\Lib\%SDKVER%\um\x64;%LIB%"

set "PATH=C:\Program Files\CMake\bin;D:\Python313\Scripts;%PATH%"
set "PYTHON=D:\Python313\python.exe"
cd /d E:\github\pdflib

echo [1/4] conan install (static runtime, podofo only)
"%PYTHON%" tools\conan_main.py install . -of build\mt ^
  -pr:a profiles\win-x64-static-release ^
  -o with_libharu=False -o with_podofo=True --build=missing
if errorlevel 1 exit /b 1

rem The install above appends this layout to CMakeUserPresets.json, which
rem then lists two preset files that both define "conan-release" and makes
rem EVERY `cmake --preset` call fail. Keep the all-backends layout listed
rem and drop the rest; this script configures with -D flags, not presets.
"%PYTHON%" tools\pin_user_presets.py build/all-backends/build/Release/generators/CMakePresets.json
if errorlevel 1 exit /b 1

echo [2/4] configure (static CRT required)
cmake -S . -B build\mt-cmake -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=build/mt/build/Release/generators/conan_toolchain.cmake ^
  -DPDFLIB_WITH_LIBHARU=OFF -DPDFLIB_WITH_PODOFO=ON ^
  -DPDFLIB_BUILD_TESTS=ON -DPDFLIB_BUILD_EXAMPLES=ON ^
  -DPDFLIB_BUILD_DRIVER=ON -DPDFLIB_DRIVER_STATIC_CRT=ON
if errorlevel 1 exit /b 1

echo [3/4] build
cmake --build build\mt-cmake
if errorlevel 1 exit /b 1

echo [4/4] tests (includes the export table + no-dynamic-CRT guard)
cd build\mt-cmake
ctest --output-on-failure
