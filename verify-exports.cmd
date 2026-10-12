@echo off
rem Verify the export table of pdflib.dll for an architecture.
rem The point of the check is x86 name decoration: __cdecl symbols normally
rem come out as "_pdf_doc_create", so anything decorated means every
rem GetProcAddress("pdf_doc_create") caller fails.
rem
rem   verify-exports.cmd build/x64/pdflib.dll   [x64]
rem   verify-exports.cmd build/x86/pdflib.dll   [x86]
rem ===================================================================
setlocal EnableDelayedExpansion

set "DLL=%~1"
if "%DLL%"=="" (
    echo usage: verify-exports.cmd ^<dll^> [arch]
    exit /b 1
)

set "ARCH=%~2"
if "%ARCH%"=="" set "ARCH=x64"
if not "%ARCH%"=="x86" set "ARCH=x64"

set "ROOT=%~dp0"
cd /d "%ROOT%"

call "%ROOT%pdflib-build-env.cmd" %ARCH%
if errorlevel 1 exit /b 1

if not exist "%DLL%" (
    echo [ERR] %DLL% not found
    exit /b 1
)

set "DUMPBIN=%PDFLIB_VCTOOLS%\bin\Hostx64\x64\dumpbin.exe"
if not exist "%DUMPBIN%" (
    for /f "delims=" %%p in ('where dumpbin 2^>nul') do if not defined GOT set "DUMPBIN=%%p" & set GOT=1
)
if not exist "%DUMPBIN%" (
    echo [ERR] dumpbin not found
    exit /b 1
)

echo [check] exports of %DLL% (%ARCH%)
"%DUMPBIN%" /nologo /exports "%DLL%" > "%TEMP%\pdflib_exports.txt" 2>&1
if errorlevel 1 (
    echo [ERR] dumpbin failed; see %TEMP%\pdflib_exports.txt
    exit /b 1
)

python "tools\check_exports.py" "%TEMP%\pdflib_exports.txt" "src\pdflib.def"
if errorlevel 1 exit /b 1
endlocal
exit /b 0
