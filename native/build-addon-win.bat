@echo off
rem Builds saycheese.uxpaddon and saycheese-cli.exe into native\build\out, and copies
rem the addon to plugin\win\x64.
rem Needs VS 2022 Build Tools and the static FFmpeg from build-ffmpeg-win.bat.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "HERE=%~dp0"
set "FF=%HERE%build\ffmpeg-out"
set "OUT=%HERE%build\out"
set "OBJ=%HERE%build\obj"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"

set CFLAGS=/nologo /std:c++17 /O2 /MT /EHsc /W3 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I"%FF%\include" /I"%HERE%uxp\src" /I"%HERE%uxp\src\utilities" /I"%HERE%src"
set LIBS="%FF%\lib\avformat.lib" "%FF%\lib\avcodec.lib" "%FF%\lib\swscale.lib" "%FF%\lib\avutil.lib" bcrypt.lib ws2_32.lib secur32.lib ole32.lib user32.lib

set CORE=frame_reader face eye_model jpeg_writer job
for %%f in (%CORE%) do (
  cl %CFLAGS% /c "%HERE%src\%%f.cpp" /Fo"%OBJ%\%%f.obj" || exit /b 1
)
set OBJS="%OBJ%\frame_reader.obj" "%OBJ%\face.obj" "%OBJ%\eye_model.obj" "%OBJ%\jpeg_writer.obj" "%OBJ%\job.obj"

cl %CFLAGS% /LD "%HERE%src\module_saycheese.cpp" "%HERE%uxp\src\utilities\UxpAddon.cpp" "%HERE%uxp\src\utilities\UxpTask.cpp" "%HERE%uxp\src\utilities\UxpValue.cpp" %OBJS% /Fo"%OBJ%\\" /Fe"%OUT%\saycheese.dll" /link %LIBS% || exit /b 1
move /y "%OUT%\saycheese.dll" "%OUT%\saycheese.uxpaddon" >nul
rem The panel loads it from here; it is committed so releases needn't rebuild FFmpeg.
rem A copy Premiere has loaded can't be overwritten, but it can be renamed; the
rem new one loads when Premiere next starts.
if not exist "%HERE%..\plugin\win\x64" mkdir "%HERE%..\plugin\win\x64"
del /q "%HERE%..\plugin\win\x64\*.old" 2>nul
if exist "%HERE%..\plugin\win\x64\saycheese.uxpaddon" move /y "%HERE%..\plugin\win\x64\saycheese.uxpaddon" "%HERE%..\plugin\win\x64\saycheese.uxpaddon.%RANDOM%.old" >nul
copy /y "%OUT%\saycheese.uxpaddon" "%HERE%..\plugin\win\x64\" >nul || exit /b 1

cl %CFLAGS% "%HERE%src\cli.cpp" %OBJS% /Fo"%OBJ%\\" /Fe"%OUT%\saycheese-cli.exe" /link %LIBS% || exit /b 1
rem The CLI loads MediaPipe and the model from its own folder (npm run deps puts them in plugin\).
if exist "%HERE%..\plugin\win\x64\libmediapipe.dll" copy /y "%HERE%..\plugin\win\x64\libmediapipe.dll" "%OUT%\" >nul
if exist "%HERE%..\plugin\models\face_landmarker.task" copy /y "%HERE%..\plugin\models\face_landmarker.task" "%OUT%\" >nul
echo ADDON_BUILD_DONE
