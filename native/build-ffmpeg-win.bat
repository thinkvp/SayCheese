@echo off
rem Builds static FFmpeg into native\build\ffmpeg-out. Needs VS 2022 Build Tools and MSYS2 (make, nasm).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "MSYS2_PATH_TYPE=inherit"
set "CHERE_INVOKING=1"
C:\msys64\usr\bin\bash.exe -l "%~dp0build-ffmpeg-win.sh"
