#!/bin/sh
# build.sh: cross-compile talkip with Retro68. Output: build/*.dsk, *.bin, *.APPL.
# TOOLCHAIN: path to retro68.toolchain.cmake (default: a Retro68 build in ../Retro68-build next to this repo).
set -eu
cd "$(dirname "$0")/.."
TOOLCHAIN=${TOOLCHAIN:-$(cd "$(dirname "$0")/.." && pwd)/../Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake}
[ -f "$TOOLCHAIN" ] || { echo "no Retro68 toolchain at $TOOLCHAIN" >&2; exit 1; }
[ "${1:-}" = "--clean" ] && rm -rf build
mkdir -p build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=MinSizeRel >/dev/null
make -j"$(nproc)" 2>&1
ls -l *.dsk *.bin 2>/dev/null
