#!/usr/bin/env bash
# Build the native `cmmc` compiler. Requires only a C compiler (no Python).
#
#   ./build_cmmc.sh            # builds ./cmmc
#   CC=clang ./build_cmmc.sh   # pick a compiler
#
# Step 1 embeds the runtime sources (../runtime/cmm_runtime.{c,h}) into a
# generated header so the resulting binary is fully self-contained.
set -euo pipefail
cd "$(dirname "$0")"
CC="${CC:-cc}"

echo "[1/2] embedding runtime -> embedded_runtime.h"
"$CC" -std=c99 -O2 embed.c -o embed_tool
./embed_tool ../runtime/cmm_runtime.h ../runtime/cmm_runtime.c embedded_runtime.h
rm -f embed_tool

echo "[2/2] compiling cmmc"
"$CC" -std=c99 -O2 cmmc.c -o cmmc

echo "built ./cmmc"
./cmmc version
