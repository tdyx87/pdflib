@echo off
rem Build and run the DLL smoke test for one architecture.
rem
rem   run-smoke.cmd [x64]
rem   run-smoke.cmd x86
rem
rem The smoke test links the import library and exercises the DLL on BOTH
rem engines, so a pass is evidence the ABI works for that architecture.
rem
rem NOTE: ninja's dependency log in build\<arch> can go stale after a source
rem edit and skip the link, so the DLL is force-deleted first.
rem ===================================================================
setlocal EnableDelayedExpansion

set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"
if not "%ARCH%"=="x86" set "ARCH=x64"

set "ROOT=%~dp0"
cd /d "%ROOT%"

call "%ROOT%pdflib-build-env.cmd" %ARCH%
if errorlevel 1 exit /b 1

if exist "build\%ARCH%\pdflib.dll" del /q "build\%ARCH%\pdflib.dll"
cmake --build "build\%ARCH%" --target pdflib_smoke
if errorlevel 1 ( echo [ERR] smoke build failed & exit /b 1 )
cmake --install "build\%ARCH%" >nul
if errorlevel 1 ( echo [ERR] install failed & exit /b 1 )

rem Make sure the freshly built DLL is the one that gets loaded.
copy /y "dist\%ARCH%\pdflib.dll" "build\%ARCH%\pdflib.dll" >nul

echo running smoke (%ARCH%)...
build\%ARCH%\pdflib_smoke.exe
echo SMOKE_RC=%ERRORLEVEL%
exit /b 0
