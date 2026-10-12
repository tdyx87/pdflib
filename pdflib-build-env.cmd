@echo off
rem ===================================================================
rem pdflib-build-env.cmd - self-contained MSVC build environment.
rem
rem Why not vcvarsall/vcvars64: those scripts shell out to reg.exe to
rem locate the Windows SDK. The sandbox blocks reg.exe, so they leave
rem INCLUDE/LIB incomplete (no SDK paths) and every link fails with
rem LNK1181 "cannot open kernel32.lib". The repo's own build-*.cmd also
rem hardcodes a VS2017 path that does not exist on this machine
rem (VS2017 is at C:\2017\Community, not under Program Files (x86)).
rem
rem So this script sets the whole environment by hand. Pass the target
rem architecture as %1: x64 (default) or x86.
rem
rem Usage:  call pdflib-build-env.cmd x64
rem ===================================================================
setlocal EnableDelayedExpansion

set "PDFLIB_TARGET=%~1"
if "%PDFLIB_TARGET%"=="" set "PDFLIB_TARGET=x64"

set "VSROOT=C:\2017\Community"
set "VCTOOLS=%VSROOT%\VC\Tools\MSVC\14.16.27023"
if not exist "%VCTOOLS%\bin\Hostx64\x64\cl.exe" (
    echo [ERR] MSVC tools not found at %VCTOOLS%
    exit /b 1
)

set "SDKROOT=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER="
for /f "delims=" %%v in ('dir /b /o-n "%SDKROOT%\Include" 2^>nul ^| findstr /r "^10\."') do (
    if not defined SDKVER (
        if exist "%SDKROOT%\Include\%%v\um\windows.h" set "SDKVER=%%v"
    )
)
if not defined SDKVER (
    echo [ERR] no usable Windows 10 SDK found
    exit /b 1
)
if not exist "%SDKROOT%\Lib\%SDKVER%\um\%PDFLIB_TARGET%\kernel32.lib" (
    set "SDKVER=10.0.19041.0"
)
if not exist "%SDKROOT%\Lib\%SDKVER%\um\%PDFLIB_TARGET%\kernel32.lib" (
    echo [ERR] no Windows 10 SDK with %PDFLIB_TARGET% libs found
    exit /b 1
)

rem --- 32-bit vs 64-bit host/target selection ---------------------------
rem NOTE for x86: the compiler that targets x86 is bin\Hostx86\x86\cl.exe
rem (an x86 image) - NOT the Hostx64\x86 one. Sharing one .tools directory
rem between architectures therefore broke x86 outright: the x64 cl.exe
rem cannot load Hostx86\x86\c1.dll and dies with D8027. So the shim copies
rem are refreshed for the selected arch every time.
if /i "%PDFLIB_TARGET%"=="x64" (
    set "VCHOST=Hostx64\x64"
    set "VCLIB=%VCTOOLS%\lib\x64"
) else (
    set "VCHOST=Hostx86\x86"
    set "VCLIB=%VCTOOLS%\lib\x86"
)

set "INCLUDE=%VCTOOLS%\include;%SDKROOT%\Include\%SDKVER%\ucrt;%SDKROOT%\Include\%SDKVER%\shared;%SDKROOT%\Include\%SDKVER%\um;%SDKROOT%\Include\%SDKVER%\winrt"
set "LIB=%VCLIB%;%SDKROOT%\Lib\%SDKVER%\ucrt\%PDFLIB_TARGET%;%SDKROOT%\Lib\%SDKVER%\um\%PDFLIB_TARGET%"
rem SDK bin\%arch% supplies rc.exe (CMake's vs_link_exe needs it) and mt.exe.
set "SDKBIN=%SDKROOT%\bin\%SDKVER%\%PDFLIB_TARGET%"
if not exist "%SDKBIN%\rc.exe" set "SDKBIN=%SDKROOT%\bin\10.0.19041.0\%PDFLIB_TARGET%"

rem .tools holds copies of cl/link only. It is prepended so that the real
rem MSVC cl and linker win over any other cl.exe/link.exe on PATH (notably
rem Git's coreutils 'link', which makes Meson's guess_win_linker crash with
rem "AttributeError: 'NoneType' object has no attribute 'split'" while
rem building freetype). The directory deliberately has no spaces or
rem parentheses so tools that write PATH into a .bat do not choke on it.
rem
rem Do NOT add rc.exe or mt.exe here. The SDK's copies are special
rem launchers that stop working the moment they are moved out of their own
rem directory: they exit with 0xC0000135 (STATUS_DLL_NOT_FOUND), which
rem surfaces as LNK1327 "rc.exe failed" on any 32-bit link. They must be
rem resolved through %SDKBIN% on PATH instead.
rem
rem The copies are REFRESHED on every call, because the cl.exe that targets
rem x86 is bin\Hostx86\x86\cl.exe - a different image from the x64 one. A
rem single shared copy would let Hostx64\x64\cl.exe try (and fail) to load
rem Hostx86\x86\c1.dll: cl error D8027.
set "PDFLIB_SHIM=%~dp0.tools"
if not exist "%PDFLIB_SHIM%" mkdir "%PDFLIB_SHIM%" 2>nul
if not exist "%PDFLIB_SHIM%" set "PDFLIB_SHIM="
if defined PDFLIB_SHIM (
    copy /y "%VCTOOLS%\bin\%VCHOST%\cl.exe"   "%PDFLIB_SHIM%\cl.exe"   >nul 2>&1
    copy /y "%VCTOOLS%\bin\%VCHOST%\link.exe" "%PDFLIB_SHIM%\link.exe" >nul 2>&1
    if exist "%PDFLIB_SHIM%\rc.exe" del /q "%PDFLIB_SHIM%\rc.exe"  >nul 2>&1
    if exist "%PDFLIB_SHIM%\mt.exe" del /q "%PDFLIB_SHIM%\mt.exe"  >nul 2>&1
    if not exist "%PDFLIB_SHIM%\link.exe" set "PDFLIB_SHIM="
)
set "PATH=%PDFLIB_SHIM%;%VCTOOLS%\bin\%VCHOST%;%SDKBIN%;C:\Program Files (x86)\CMake\bin;C:\Strawberry\c\bin;%PATH%"

rem Force MSVC/clang to emit ENGLISH diagnostics. A localized cl.exe/link.exe
rem prints its banner in the OEM code page (e.g. CP936), and build tools such
rem as Meson decode tool output as UTF-8 - the mismatch aborts linker
rem detection with "UnicodeDecodeError: 'utf-8' codec can't decode byte 0xce".
rem 1033 = en-US. This also keeps build logs greppable.
set "VSLANG=1033"
set "PDFLIB_VCTOOLS=%VCTOOLS%"
set "PDFLIB_SDKROOT=%SDKROOT%"
set "PDFLIB_SDKVER=%SDKVER%"

endlocal & (
    set "INCLUDE=%INCLUDE%"
    set "LIB=%LIB%"
    set "PATH=%PATH%"
    set "VSLANG=1033"
    set "PDFLIB_TARGET=%PDFLIB_TARGET%"
    set "PDFLIB_VCTOOLS=%PDFLIB_VCTOOLS%"
    set "PDFLIB_SDKROOT=%PDFLIB_SDKROOT%"
    set "PDFLIB_SDKVER=%PDFLIB_SDKVER%"
)
exit /b 0
