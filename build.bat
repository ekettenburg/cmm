@echo off
REM Build (and optionally run) a .cmm program on Windows.
REM   build.bat path\to\Main.cmm        -> compiles to Main.exe
REM   build.bat run path\to\Main.cmm    -> compiles and runs
REM
REM Requires: Python 3 on PATH, and a C compiler (clang, gcc/MinGW-w64,
REM or MSVC `cl` from a Developer Command Prompt). cmmc auto-detects.

setlocal
cd /d "%~dp0"

if /I "%~1"=="run" (
    shift
    python -m cmmc run %*
    goto :eof
)

python -m cmmc build %*
