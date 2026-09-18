@echo off
setlocal
cd /d "%~dp0"

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto :mingw

call "%VCVARS%" >nul 2>&1
rc /nologo /fo capturefix.res capturefix.rc || goto :fail
cl /nologo /TC /std:c17 /O2 /MT /W4 /permissive- /D_CRT_SECURE_NO_WARNINGS ^
   capturefix.c theme.c ^
   /link capturefix.res /SUBSYSTEM:WINDOWS /OUT:capturefix.exe ^
   user32.lib gdi32.lib comctl32.lib shell32.lib advapi32.lib ole32.lib uxtheme.lib ^
   /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA || goto :fail
del /q capturefix.obj theme.obj capturefix.res >nul 2>&1
echo Built capturefix.exe with MSVC.
goto :eof

:mingw
where gcc >nul 2>&1 || goto :fail
rem --codepage=65001: the .rc is UTF-8 and carries a copyright sign
windres --codepage=65001 capturefix.rc -O coff -o capturefix.res.o || goto :fail
rem mingw-w64 always links its own default-manifest.o, which collides with
rem ours ("multiple non-default manifests"); shadow it with an empty object
rem found first via -B.
break > empty.c
gcc -c empty.c -o default-manifest.o || goto :fail
gcc -std=c17 -O2 -municode -mwindows -B. -Wall -Wextra ^
    capturefix.c theme.c capturefix.res.o -o capturefix.exe ^
    -luser32 -lgdi32 -lcomctl32 -lshell32 -ladvapi32 -lole32 -luxtheme || goto :fail
del /q capturefix.res.o empty.c default-manifest.o >nul 2>&1
echo Built capturefix.exe with MinGW.
goto :eof

:fail
echo BUILD FAILED
exit /b 1
