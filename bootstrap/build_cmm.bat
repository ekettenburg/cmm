@echo off
REM Build the native cmm compiler on Windows with zig.
REM   build_cmm.bat            -> builds cmm.exe   (needs zig: pip install ziglang)
REM   set CC=<compiler>        -> override (e.g. set CC=python -m ziglang cc)
setlocal
cd /d "%~dp0"
if "%CC%"=="" set "CC=zig cc"

echo [1/2] embedding runtime -^> embedded_runtime.h
%CC% -std=c99 -O2 embed.c -o embed_tool.exe
embed_tool.exe ..\runtime\cmm_runtime.h ..\runtime\cmm_runtime.c embedded_runtime.h
del embed_tool.exe 2>nul

echo [2/2] compiling cmm
%CC% -std=c99 -O2 cmm.c -o cmm.exe

echo built cmm.exe
cmm.exe version
