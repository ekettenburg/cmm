#!/usr/bin/env bash
# Build the native `cmm` compiler with zig (one toolchain for every target).
#
#   ./build_cmm.sh            # builds ./cmm (prefers zig)
#   CC="gcc" ./build_cmm.sh   # override the compiler
#
# Step 1 embeds the runtime sources (../runtime/cmm_runtime.{c,h}) into a
# generated header so the resulting binary is fully self-contained.
set -euo pipefail
cd "$(dirname "$0")"

# One toolchain: zig. CC overrides; fall back to cc only if zig is absent.
if [ -z "${CC:-}" ]; then
  if command -v zig >/dev/null 2>&1; then CC="zig cc"
  elif python3 -m ziglang version >/dev/null 2>&1; then CC="python3 -m ziglang cc"
  elif command -v cc >/dev/null 2>&1; then CC="cc"
  else echo "no compiler found. install zig:  pip install ziglang" >&2; exit 1; fi
fi

echo "[1/2] embedding runtime -> embedded_runtime.h  (CC=$CC)"
$CC -std=c99 -O2 embed.c -o embed_tool
./embed_tool ../runtime/cmm_runtime.h ../runtime/cmm_runtime.c embedded_runtime.h
rm -f embed_tool

echo "[2/2] compiling cmm"
$CC -std=c99 -O2 cmm.c -o cmm

echo "built ./cmm"
./cmm version
