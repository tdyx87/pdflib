@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [ERR] vcvars64 failed & exit /b 1 )
set "PATH=C:\Program Files\CMake\bin;%PATH%"
cd /d E:\github\pdflib

echo [1/3] configure (core + dummy)
cmake -S . -B build\core -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release ^
  -DPDFLIB_WITH_LIBHARU=OFF -DPDFLIB_WITH_PODOFO=OFF ^
  -DPDFLIB_BUILD_TESTS=ON -DPDFLIB_BUILD_EXAMPLES=ON
if errorlevel 1 exit /b 1

echo [2/3] build
cmake --build build\core
if errorlevel 1 exit /b 1

echo [3/3] test
cd build\core
ctest --output-on-failure
