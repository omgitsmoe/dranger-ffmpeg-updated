@echo off
REM remember to run this in the VS developer console
REM or run C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat x64
cl %* /I .\ffmpeg-5.1.2-full_build-shared\include\ /I .\SDL2-2.24.1\include\ /link /libpath:ffmpeg-5.1.2-full_build-shared\lib\ avcodec.lib avutil.lib avformat.lib swscale.lib swresample.lib /libpath:SDL2-2.24.1\lib\x64\ SDL2.lib SDL2main.lib /SUBSYSTEM:CONSOLE
