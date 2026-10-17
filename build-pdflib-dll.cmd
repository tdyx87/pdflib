@echo off
rem ===================================================================
rem build-pdflib-dll.cmd - build pdflib.dll for one architecture.
rem
rem   build-pdflib-dll.cmd x64     (default)
rem   build-pdflib-dll.cmd x86
rem
rem Prerequisites (one-time):
rem   * conan installed and the dependencies present in the local cache
rem     for the chosen arch (see build-deps.cmd).
rem
rem Output: build\<arch>\pdflib.dll, pdflib.lib, include\pdflib.h
rem ===================================================================
setlocal EnableDelayedExpansion

set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"

set "ROOT=%~dp0"
cd /d "%ROOT%"

rem --- 1) MSVC environment (self-contained; does not call vcvars) -----
call "%ROOT%pdflib-build-env.cmd" %ARCH%
if errorlevel 1 exit /b 1

rem --- 2) conan profile for this arch --------------------------------
if /i "%ARCH%"=="x64" (
    set "PROFILE=profiles/win-x64-static-release"
    set "DEPSDIR=build/x64-deps"
) else (
    set "PROFILE=profiles/win-x86-static-release"
    set "DEPSDIR=build/x86-deps"
)

if not exist "%DEPSDIR%\build\Release\generators\conan_toolchain.cmake" (
    echo [ERR] dependencies for %ARCH% are not installed.
    echo       run:  build-deps.cmd %ARCH%
    exit /b 1
)

set "BUILDDIR=build\%ARCH%"
if exist "%BUILDDIR%" rmdir /s /q "%BUILDDIR%" 2>nul

echo [1/2] configure (%ARCH%)
cmake -S standalone -B "%BUILDDIR%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_TOOLCHAIN_FILE="%ROOT%%DEPSDIR%\build\Release\generators\conan_toolchain.cmake" ^
    -DCMAKE_PREFIX_PATH="%ROOT%%DEPSDIR%\build\Release\generators" ^
    -DCMAKE_INSTALL_PREFIX="%ROOT%dist\%ARCH%"
if errorlevel 1 exit /b 1

echo [2/2] build + install (%ARCH%)
rem Build the compat facade too: the install rules ship hpdf.lib and
rem pdflib_hpdf.dll, so building only `pdflib` would make `cmake --install`
rem fail on the missing files.
cmake --build "%BUILDDIR%" --target pdflib hpdf pdflib_hpdf --config Release
if errorlevel 1 exit /b 1
cmake --install "%BUILDDIR%" --config Release
if errorlevel 1 exit /b 1

echo.
echo [OK] dist\%ARCH%\pdflib.dll
endlocal
exit /b 0
