#!/usr/bin/env bash
# Stage a portable Windows package skeleton: dist/cmm-win64/
#
#   bash make_win_package.sh
#
# Produces cmmc.exe (cross-built if x86_64-w64-mingw32-gcc is present, else you
# build it on Windows with build_cmmc.bat) plus the bin/ lib/ include/ layout
# documented in PACKAGING-windows.md. You then drop a portable MinGW-w64 (or
# TinyCC) into bin/ (see that doc) to make it fully self-contained.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=dist/cmm-win64
rm -rf "$ROOT"; mkdir -p "$ROOT"/bin "$ROOT"/lib "$ROOT"/include "$ROOT"/examples

echo "[1/3] embedding runtime"
cc -std=c99 -O2 embed.c -o embed_tool
./embed_tool ../runtime/cmm_runtime.h ../runtime/cmm_runtime.c embedded_runtime.h
rm -f embed_tool

echo "[2/3] building cmmc.exe"
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
  x86_64-w64-mingw32-gcc -std=c99 -O2 cmmc.c -o "$ROOT/cmmc.exe"
  echo "      cross-built cmmc.exe"
else
  echo "      (no MinGW cross-compiler here — build cmmc.exe on Windows with build_cmmc.bat,"
  echo "       then copy it into $ROOT/)"
fi

echo "[3/3] assembling tree"
cp ../examples/*.cmm "$ROOT/examples/"
mkdir -p "$ROOT/stdlib"
cp ../stdlib/*.cmm "$ROOT/stdlib/"
mkdir -p "$ROOT/docs"
cp ../docs/*.md "$ROOT/docs/"
# vendored mbedTLS + CA bundle so cmmc.exe can build TLS out of the box
echo "      bundling third_party/ (vendored mbedTLS for out-of-the-box TLS)"
mkdir -p "$ROOT/third_party"
cp -r ../third_party/mbedtls "$ROOT/third_party/"
cp ../third_party/cmm_ca_certs.h "$ROOT/third_party/"
cp ../third_party/cmm_ca_bundle.pem "$ROOT/third_party/" 2>/dev/null || true
printf "Optional: drop gcc.exe (+DLLs) or tcc.exe here to bundle a compiler.\nSee ..\\README.txt step 1. Or just add a compiler to PATH.\n" > "$ROOT/bin/PUT_COMPILER_HERE.txt"

# ---- source build kit (so the package can be rebuilt on Windows) ----
mkdir -p "$ROOT/src/runtime"
cp cmmc.c embed.c embedded_runtime.h "$ROOT/src/"
cp ../runtime/cmm_runtime.h ../runtime/cmm_runtime.c "$ROOT/src/runtime/"
cat > "$ROOT/src/build_cmmc.bat" <<'BAT'
@echo off
REM Rebuild cmmc.exe from source. Works with MinGW gcc or MSVC cl.
setlocal
cd /d "%~dp0"

where gcc >nul 2>&1 && goto :gcc
where cl  >nul 2>&1 && goto :cl
echo No C compiler found on PATH.
echo Install MinGW (w64devkit / winlibs) so 'gcc' is on PATH, or open a
echo "x64 Native Tools Command Prompt for VS" so 'cl' is on PATH.
exit /b 1

:gcc
echo [1/2] embedding runtime (gcc)
gcc -std=c99 -O2 embed.c -o embed_tool.exe || goto :err
embed_tool.exe runtime\cmm_runtime.h runtime\cmm_runtime.c embedded_runtime.h || goto :err
del embed_tool.exe 2>nul
echo [2/2] compiling cmmc (gcc)
gcc -std=c99 -O2 cmmc.c -o cmmc.exe || goto :err
goto :done

:cl
echo [1/2] embedding runtime (cl)
cl /nologo /O2 embed.c /Fe:embed_tool.exe >nul || goto :err
embed_tool.exe runtime\cmm_runtime.h runtime\cmm_runtime.c embedded_runtime.h || goto :err
del embed_tool.exe embed_tool.obj 2>nul
echo [2/2] compiling cmmc (cl)
cl /nologo /O2 cmmc.c /Fe:cmmc.exe >nul || goto :err
del cmmc.obj 2>nul
goto :done

:done
echo built cmmc.exe  (copy it next to stdlib\ in the package root to use it)
cmmc.exe version
exit /b 0
:err
echo build failed
exit /b 1
BAT

cat > "$ROOT/README.txt" <<'TXT'
C-- (cmm) — portable Windows package
====================================

cmm compiles your .cmm program to C and then to a native .exe, so you need a
C compiler available. The compiler is the ONLY external requirement.

1) Get a C compiler  (zig recommended — one tool, also builds for AWS Lambda)
----------------------------------------------------------------------------
  * zig (recommended). If you have Python:   pip install ziglang
    That's it — cmmc finds it automatically. zig is a single self-contained
    compiler and it's also what lets you cross-compile for Amazon Linux (see
    section 4). No admin, no PATH editing, no DLLs.
    (Or download zig from https://ziglang.org and put it on PATH, or set
     CMMC_ZIG to its path.)
  * MinGW-w64 (alternative). A portable build such as w64devkit
    (https://github.com/skeeto/w64devkit) or winlibs (https://winlibs.com):
    add its bin\ to PATH, or copy gcc.exe (+DLLs) into this package's bin\.
  * MSVC: open an "x64 Native Tools Command Prompt for VS" (provides cl.exe).

2) Run a program
----------------
  cmmc run examples\Hello.cmm
  cmmc build examples\Hello.cmm -o hello.exe
  cmmc run examples\Args.cmm -- one two three

cmmc finds a compiler in this order: --cc <cmd>  ->  %CMMC_CC%  ->  zig
(installed / bin\zig.exe / `python -m ziglang`)  ->  this package's
bin\{gcc,clang,cc,tcc}.exe  ->  your PATH (gcc/clang/cl).
Run with -v to see which compiler was chosen, e.g. `cmmc build foo.cmm -v`.

3) Rebuild cmmc.exe from source (optional)
------------------------------------------
Everything needed is in src\. With gcc or cl on PATH:
  cd src
  build_cmmc.bat
Then copy src\cmmc.exe to this folder (next to stdlib\) to replace the shipped one.

4) Build for AWS Lambda (Amazon Linux 2023)
-------------------------------------------
With zig installed (pip install ziglang):
  cmmc build handler.cmm -o bootstrap --target al2023
This produces a Linux binary (glibc 2.34) that runs on provided.al2023 — built
right here on Windows, with working HTTPS. The first TLS build for a target is a
one-time ~15s step (a vendored mbedTLS is compiled for that target and cached);
later builds are instant. See docs\build-amazon-linux.md for arm64/static
variants and deployment.

Notes
-----
  * HTTPS is certificate-verified against a bundled CA store (vendored mbedTLS,
    TLS 1.3 capable). Set CMMC_TLS_INSECURE=1 to skip verification.
  * TLS works out of the box for every target (host, Amazon Linux, Windows) —
    no OpenSSL or system packages needed. zig compiles the vendored mbedTLS for
    whatever you target; the first build per target is a one-time ~15s step,
    then cached under your user cache dir.
  * If you see "no C compiler found", do step 1 (pip install ziglang).
TXT

echo "done -> $ROOT"
find "$ROOT" -maxdepth 2 | sort
