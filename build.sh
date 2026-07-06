#!/usr/bin/env bash
# Build (and optionally run) a .cmm program.
#   ./build.sh path/to/Main.cmm          -> compiles to ./Main
#   ./build.sh run path/to/Main.cmm      -> compiles and runs
set -euo pipefail
cd "$(dirname "$0")"

if [ "${1:-}" = "run" ]; then
    shift
    exec python3 -m cmmc run "$@"
fi
exec python3 -m cmmc build "$@"
