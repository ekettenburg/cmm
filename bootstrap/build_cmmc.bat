@echo off
REM Build the native cmmc compiler on Windows with MSVC (cl).
REM   build_cmmc.bat            -> builds cmmc.exe
REM For MinGW gcc instead, run:  build_cmmc.sh  under a bash shell,
REM   or: gcc -std=c99 -O2 embed.c -o embed.exe && embed.exe ..\runtime\cmm_runtime.h ..\runtime\cmm_runtime.c embedded_runtime.h && gcc -std=c99 -O2 cmmc.c -o cmmc.exe
setlocal
cd /d "%~dp0"

echo [1/2] embedding runtime -^> embedded_runtime.h
cl /nologo /O2 embed.c /Fe:embed_tool.exe >nul
embed_tool.exe ..\runtime\cmm_runtime.h ..\runtime\cmm_runtime.c embedded_runtime.h
del embed_tool.exe embed_tool.obj 2>nul

echo [2/2] compiling cmmc
cl /nologo /O2 cmmc.c /Fe:cmmc.exe
del cmmc.obj 2>nul

echo built cmmc.exe
cmmc.exe version
