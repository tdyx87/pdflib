@echo off
rem Full build: pdflib + libharu backend + podofo backend (no openssl).
rem Requires: conan install already run into build\all-backends.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [ERR] vcvars64 failed & exit /b 1 )
set "PATH=C:\Program Files\CMake\bin;D:\Python313\Scripts;%PATH%"
cd /d E:\github\pdflib

echo [1/3] configure
cmake --preset all-backends
if errorlevel 1 exit /b 1

echo [2/3] build
cmake --build --preset all-backends
if errorlevel 1 exit /b 1

echo [3/3] test
cd build\all-backends
ctest --output-on-failure
