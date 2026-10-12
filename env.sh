#!/usr/bin/env bash
# Replicates pdflib-build-env.cmd for a bash shell.
#   source env.sh [x64|x86]
#
# NOTE: PATH entries must be MSYS-style (/c/...) - a Windows-style
# "C:/Program Files (x86)/..." entry is not handed to native tools in a
# usable form, which makes rc.exe/mt.exe invisible to link.exe (LNK1158 /
# LNK1327 on any link that embeds a manifest). INCLUDE and LIB keep the
# Windows form because MSVC parses those itself.
#
# NOTE: for x86 the compiler is bin/Hostx86/x86/cl.exe - an x86 image, NOT
# Hostx64/x86. Mixing them makes cl.exe fail to load c1.dll (D8027).
ARCH="${1:-x64}"
if [ "$ARCH" != "x86" ]; then ARCH=x64; fi

VCTOOLS_WIN="C:/2017/Community/VC/Tools/MSVC/14.16.27023"
SDKROOT_WIN="C:/Program Files (x86)/Windows Kits/10"
SDKVER="10.0.19041.0"

if [ "$ARCH" = "x64" ]; then
    VCHOST="Hostx64/x64"
else
    VCHOST="Hostx86/x86"
fi

export INCLUDE="$VCTOOLS_WIN/include;$SDKROOT_WIN/Include/$SDKVER/ucrt;$SDKROOT_WIN/Include/$SDKVER/shared;$SDKROOT_WIN/Include/$SDKVER/um;$SDKROOT_WIN/Include/$SDKVER/winrt"
export LIB="$VCTOOLS_WIN/lib/$ARCH;$SDKROOT_WIN/Lib/$SDKVER/ucrt/$ARCH;$SDKROOT_WIN/Lib/$SDKVER/um/$ARCH"

export PATH="/c/2017/Community/VC/Tools/MSVC/14.16.27023/bin/$VCHOST:/c/Program Files (x86)/Windows Kits/10/bin/$SDKVER/$ARCH:/c/Program Files (x86)/CMake/bin:/c/Strawberry/c/bin:$PATH"

export VSLANG=1033
export PDFLIB_ARCH="$ARCH"
export CL_EXE="$VCTOOLS_WIN/bin/$VCHOST/cl.exe"
export DUMPBIN_EXE="$VCTOOLS_WIN/bin/Hostx64/x64/dumpbin.exe"
