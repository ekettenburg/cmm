#!/usr/bin/env bash
# Stage a portable Windows package: dist/cmm-win64/
#
#   bash make_win_package.sh
#
# Cross-builds cmm.exe with zig and assembles the tree (stdlib, examples, docs,
# vendored mbedTLS, and a src/ build kit). zig is the only toolchain.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=dist/cmm-win64
rm -rf "$ROOT"; mkdir -p "$ROOT"/examples

# resolve a zig command
if command -v zig >/dev/null 2>&1; then ZIG="zig"
elif python3 -m ziglang version >/dev/null 2>&1; then ZIG="python3 -m ziglang"
else echo "zig not found — install with: pip install ziglang" >&2; exit 1; fi

echo "[1/3] embedding runtime"
$ZIG cc -std=c99 -O2 embed.c -o embed_tool
./embed_tool ../runtime/cmm_runtime.h ../runtime/cmm_runtime.c embedded_runtime.h
rm -f embed_tool

echo "[2/3] cross-building cmm.exe (zig -> x86_64-windows-gnu)"
$ZIG cc -target x86_64-windows-gnu -std=c99 -O2 cmm.c -o "$ROOT/cmm.exe"

echo "[3/3] assembling tree"
cp ../examples/*.cmm "$ROOT/examples/"
mkdir -p "$ROOT/stdlib"; cp ../stdlib/*.cmm "$ROOT/stdlib/"
mkdir -p "$ROOT/docs";   cp ../docs/*.md    "$ROOT/docs/"
echo "      bundling third_party/ (vendored mbedTLS for out-of-the-box TLS)"
mkdir -p "$ROOT/third_party"
cp -r ../third_party/mbedtls "$ROOT/third_party/"
cp ../third_party/cmm_ca_certs.h "$ROOT/third_party/"
cp ../third_party/cmm_mbedtls_config.h "$ROOT/third_party/" 2>/dev/null || true
cp ../third_party/cmm_ca_bundle.pem "$ROOT/third_party/" 2>/dev/null || true

# ---- source build kit (rebuild cmm.exe on Windows with zig) ----
mkdir -p "$ROOT/src/runtime"
cp cmm.c embed.c embedded_runtime.h "$ROOT/src/"
cp ../runtime/cmm_runtime.h ../runtime/cmm_runtime.c "$ROOT/src/runtime/"
cat > "$ROOT/src/build_cmm.bat" <<'BAT'
@echo off
REM Rebuild cmm.exe from source with zig (pip install ziglang).
setlocal
cd /d "%~dp0"
if "%CC%"=="" set "CC=zig cc"
echo [1/2] embedding runtime
%CC% -std=c99 -O2 embed.c -o embed_tool.exe || goto :err
embed_tool.exe runtime\cmm_runtime.h runtime\cmm_runtime.c embedded_runtime.h || goto :err
del embed_tool.exe 2>nul
echo [2/2] compiling cmm
%CC% -std=c99 -O2 cmm.c -o cmm.exe || goto :err
echo built cmm.exe
cmm.exe version
exit /b 0
:err
echo build failed  (need zig on PATH: pip install ziglang, or set CC=python -m ziglang cc)
exit /b 1
BAT

cat > "$ROOT/README.txt" <<'TXT'
C-- (cmm) — portable Windows package
====================================

cmm compiles your .cmm program to C and then to a native .exe with zig. zig is
the ONLY external requirement — one self-contained toolchain, no admin, no DLLs.

1) Install zig
--------------
  pip install ziglang
  (or download zig from https://ziglang.org, put it on PATH, or set CMMC_ZIG)

2) Run a program
----------------
  cmm run examples\Hello.cmm
  cmm build examples\Hello.cmm -o hello.exe
  cmm run examples\Args.cmm -- one two three

cmm resolves its backend as: --cc <cmd>  ->  %CMMC_CC%  ->  zig (on PATH,
%CMMC_ZIG%, or `python -m ziglang`). Run with -v to see the exact command.

3) Rebuild cmm.exe from source (optional)
------------------------------------------
Everything needed is in src\. With zig installed:
  cd src
  build_cmm.bat
Then copy src\cmm.exe next to stdlib\ to replace the shipped one.

4) Build for AWS Lambda (Amazon Linux 2023)
-------------------------------------------
  cmm build handler.cmm -o bootstrap --target al2023
Produces a Linux binary (glibc 2.34) for provided.al2023 — cross-built on
Windows with working HTTPS. See docs\build-amazon-linux.md.

Notes
-----
  * HTTPS is certificate-verified against a bundled CA store (DER-packed Mozilla
    roots) using a trimmed TLS 1.2 mbedTLS client. Set CMMC_TLS_INSECURE=1 to
    skip verification; build --no-verify to omit the roots entirely.
  * TLS works out of the box for every target (host, Amazon Linux, Windows) —
    zig compiles the vendored mbedTLS for whatever you target; the first build
    per target is a one-time step, then cached.
TXT

echo "done -> $ROOT"
