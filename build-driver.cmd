@echo off
rem Build pdflibdrv.dll (the WDK-facing C ABI facade) + its contract
rem harness, reusing the conan-provided backend packages in
rem build\all-backends. Requires: conan install already run.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [ERR] vcvars64 failed & exit /b 1 )

rem vcvarsall's Windows SDK detection is broken on this machine - it
rem reports 'include\<ver>\um not found or was incomplete' and leaves
rem INCLUDE/LIB with only the VC paths, so cl cannot find windows.h.
rem The SDK is installed, so add it explicitly instead of depending on
rem whatever the ambient environment happened to inherit.
set "SDKROOT=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.17763.0"
if not exist "%SDKROOT%\Include\%SDKVER%\um\windows.h" ( echo [ERR] Windows SDK %SDKVER% not found & exit /b 1 )
set "INCLUDE=%SDKROOT%\Include\%SDKVER%\ucrt;%SDKROOT%\Include\%SDKVER%\shared;%SDKROOT%\Include\%SDKVER%\um;%SDKROOT%\Include\%SDKVER%\winrt;%INCLUDE%"
set "LIB=%SDKROOT%\Lib\%SDKVER%\ucrt\x64;%SDKROOT%\Lib\%SDKVER%\um\x64;%LIB%"

set "PATH=C:\Program Files\CMake\bin;D:\Python313\Scripts;%PATH%"
cd /d E:\github\pdflib

echo [1/3] configure (all backends + driver)
cmake --preset all-backends -DPDFLIB_BUILD_DRIVER=ON
if errorlevel 1 exit /b 1

echo [2/3] build
cmake --build --preset all-backends
if errorlevel 1 exit /b 1

echo [3/3] driver contract tests
cd build\all-backends
ctest --output-on-failure -R "pdflibdrv_harness"
