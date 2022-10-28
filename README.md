# An ffmpeg and SDL Tutorial or How to Write a Video Player in Less Than 1000 Lines

The original version including explanations can be found [here](http://dranger.com/ffmpeg/).

## UPDATED 2020-2022 source files

This repo contains build information on Windows as well as the updated and commented source files
for the amazing tutorial by dranger, which is based on previous work by Martin Bohme and
FFplay code examples that are part of [FFmpeg](https://ffmpeg.org).
Some parts of the code were adapted from [ffmpeg-video-player](https://github.com/rambodrahmani/ffmpeg-video-player).

The files were tested to work with
[ffmpeg-4.3.1](https://git.ffmpeg.org/gitweb/ffmpeg.git/commit/6b6b9e593dd4d3aaf75f48d40a13ef03bdef9fd)
and [SDL2-2.0.12]() using mingw as well as
with [ffmpeg-5.1.2](https://git.ffmpeg.org/gitweb/ffmpeg.git/commit/eacfcbae690f914a4b1b4ad06999f138540cc3d8)
(you can use the prebuilt version for Windows
[ffmpeg-5.1.2-full_build-shared.(7z|zip)](https://github.com/GyanD/codexffmpeg/releases/tag/5.1.2))
and [SDL2-2.24.1](https://github.com/libsdl-org/SDL/releases/tag/release-2.24.1) (use SDL2-devel-2.24.1-VC.zip)
with cl.exe distributed with VS2022.

### Build information

See `build_info.txt` on how to build the source files, which includes a very short outline
of how to build ffmpeg using mingw.
For the VC builds you can use `build_vc.bat tut01.c`, which expects the ffmpeg
header files in `ffmpeg-5.1.2-full_build-shared\include` and SDL2 headers in `SDL2-2.24.1\include`.
Libs are assumed to be in `ffmpeg-5.1.2-full_build-shared\lib` and `SDL2-2.24.1\lib\x64`.
Since the ffmpeg libs are not completely static, you can then use `run_with_dll_path.bat tut01.exe`
to add the ffmpeg DLL location at `ffmpeg-5.1.2-full_build-shared\bin` to the PATH and then
run the file.

