@echo off
REM Compila zelda3.exe con Visual Studio 2022 Build Tools (x64, ottimizzato).
REM Uso: build_msvc.bat   -> produce zelda3.exe nella cartella corrente.
setlocal
set SDL2=third_party\SDL2-2.28.2
set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call %VCVARS% >nul || exit /b 1

if not exist build_obj mkdir build_obj

rc /nologo /fo build_obj\zelda3.res src\platform\win32\zelda3.rc || exit /b 1

cl /nologo /O2 /MT /std:c11 /W3 /wd4244 /wd4018 /wd4996 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
  /I. /I%SDL2%\include /Fobuild_obj\ /Fezelda3.exe ^
  src\*.c src\platform\win32\volume_control.c snes\*.c ^
  third_party\gl_core\gl_core_3_1.c third_party\opus-1.3.1-stripped\opus_decoder_amalgam.c ^
  build_obj\zelda3.res ^
  /link /SUBSYSTEM:CONSOLE %SDL2%\lib\x64\SDL2.lib %SDL2%\lib\x64\SDL2main.lib ^
  kernel32.lib user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib advapi32.lib || exit /b 1

if not exist SDL2.dll copy %SDL2%\lib\x64\SDL2.dll . >nul
echo BUILD OK
