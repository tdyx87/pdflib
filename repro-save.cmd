@echo off
rem Minimal save-path repro.
rem   repro-save.cmd [arch] [backend]
setlocal
set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"
if not "%ARCH%"=="x86" set "ARCH=x64"
set "BE=%~2"
if "%BE%"=="" set "BE=podofo"

set "ROOT=%~dp0"
cd /d "%ROOT%"
call "%ROOT%pdflib-build-env.cmd" %ARCH%

if exist "build\%ARCH%\pdflib.dll" del /q "build\%ARCH%\pdflib.dll"
cmake --build "build\%ARCH%" --target pdflib >nul
if errorlevel 1 ( echo [ERR] dll build & exit /b 1 )
copy /y "build\%ARCH%\pdflib.dll" "build\%ARCH%\pdflib.dll" >nul

cl /nologo /EHsc /W3 /I include /I . /Fo:build\%ARCH%\rsave.obj standalone\repro-save.c /Fe:build\%ARCH%\rsave.exe /link /LIBPATH:build\%ARCH% pdflib.lib
if errorlevel 1 ( echo [ERR] compile & exit /b 1 )
echo --- run %BE% on %ARCH% ---
build\%ARCH%\rsave.exe %BE%
echo RC=%ERRORLEVEL%
endlocal
exit /b 0
