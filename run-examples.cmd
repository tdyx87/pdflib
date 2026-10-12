@echo off
rem Build and run the shipped examples against the freshly built DLL.
rem   run-examples.cmd [arch]
call "F:/project/product/pdflib/pdflib-build-env.cmd" %~1
if errorlevel 1 exit /b 1
cd /d F:/project/product/pdflib

set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"
if not "%ARCH%"=="x86" set "ARCH=x64"

if not exist "dist\%ARCH%\pdflib.dll" (
    echo [ERR] dist\%ARCH%\pdflib.dll missing; run build-pdflib-dll.cmd %ARCH% first
    exit /b 1
)

for %%e in (hello_pdf streamed_pdf feature_pdf) do (
    if exist "examples\%%e.c" (
        echo ================= %%e =================
        cl /nologo /EHsc /W3 /I include /I . /Fo:build\%ARCH%\%%e.obj "examples\%%e.c" /Fe:build\%ARCH%\%%e.exe /link /LIBPATH:build\%ARCH% pdflib.lib >nul
        if errorlevel 1 ( echo [ERR] %%e compile failed & exit /b 1 )
        copy /y "dist\%ARCH%\pdflib.dll" "build\%ARCH%\pdflib.dll" >nul
        build\%ARCH%\%%e.exe
        if errorlevel 1 echo [WARN] %%e returned %%ERRORLEVEL%%
    )
)
echo [OK] examples built and run (%ARCH%)
exit /b 0
