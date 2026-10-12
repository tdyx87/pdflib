@echo off
rem ===================================================================
rem build-deps.cmd - build the static libharu + PoDoFo dependency trees
rem for one architecture, into build\<arch>-deps via conan.
rem
rem   build-deps.cmd x64     (default)
rem   build-deps.cmd x86
rem
rem Both engines are built as STATIC libraries with the STATIC CRT (/MT),
rem which is what lets them be linked into a single redistributable-free
rem pdflib.dll.
rem ===================================================================
setlocal EnableDelayedExpansion

set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"

set "ROOT=%~dp0"
cd /d "%ROOT%"

call "%ROOT%pdflib-build-env.cmd" %ARCH%
if errorlevel 1 exit /b 1

if /i "%ARCH%"=="x64" (
    set "PROFILE=profiles/win-x64-static-release"
    set "DEPSDIR=build/x64-deps"
) else (
    set "PROFILE=profiles/win-x86-static-release"
    set "DEPSDIR=build/x86-deps"
)

echo [deps] exporting the bundled libharu and podofo recipes
conan export recipes/libharu/all
if errorlevel 1 exit /b 1
conan export recipes/podofo/all
if errorlevel 1 exit /b 1

echo [deps] conan install (%ARCH%, static, /MT)
conan install . -s build_type=Release -pr:a "%PROFILE%" ^
    -o "podofo-local/*:with_jpeg=False" ^
    -o "podofo-local/*:with_png=True" ^
    -o "podofo-local/*:with_openssl=False" ^
    -o "podofo-local/*:with_libidn=False" ^
    -o "libharu-local/*:shared=False" ^
    --build=missing -of "%DEPSDIR%" ^
    -c tools.files.download:verify=False
if errorlevel 1 exit /b 1

echo.
echo [OK] dependencies for %ARCH% are in %DEPSDIR%
endlocal
exit /b 0
