@echo off
set "PATH=D:\Python313\Scripts;%PATH%"
echo --- where meson.exe ---
where meson.exe
echo --- path head ---
echo %PATH:~0,60%
echo --- try direct ---
D:\Python313\Scripts\meson.exe --version
