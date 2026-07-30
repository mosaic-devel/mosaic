#!/usr/bin/env bash
# Configure + build mosaic.exe for one Windows architecture (PLAN.md S57).
#
# Deliberately tiny, like its macOS twin: it exports the environment the toolchain file and
# pkg-config need, then runs the preset. Everything else -- which compiler, which sysroot, which
# Windows API level -- is decided in cmake/toolchains/mingw-w64.cmake, where a person looking for it
# will actually look.
#
# Required env:
#   MOSAIC_WIN_PREFIX  the dependency prefix for THIS arch, exactly as packaging/windows/build-deps.sh
#                      takes it (per-arch, not a root of prefixes -- the two scripts share one
#                      variable on purpose, so a working deps command line is a working build one)
# Optional env:
#   MOSAIC_LLVM_MINGW  llvm-mingw root for aarch64 (default /opt/llvm-mingw)
#   JOBS               parallel build jobs (default: nproc)
#
# Args: <x86_64|aarch64>   (arm64 is accepted as an alias for aarch64 -- see below)
#
# MOSAIC_SKIP_BUILD is NOT read here. make-package.sh owns that decision and simply does not call
# this script, which is how packaging/macos/make-dmg.sh does it too.
set -euo pipefail

# ⚠ No apostrophe in that message, ever. The `word` of ${var:?word} is parsed with quoting rules even
# inside double quotes, so a lone ' opens a string bash then hunts for to end of file: the script dies
# at parse time with "unexpected EOF while looking for matching `''" and never runs at all.
: "${MOSAIC_WIN_PREFIX:?set MOSAIC_WIN_PREFIX to the dependency prefix for this arch (see build-deps.sh)}"
JOBS="${JOBS:-$(nproc)}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

# ⚠ THREE names for two CPUs, and they are not interchangeable:
#   aarch64  the toolchain triple and MOSAIC_WIN_ARCH spelling (build-deps.sh, mingw-w64.cmake)
#   arm64    what the CMake preset is called, and what Windows itself calls the architecture
#   x86_64   the same in both worlds
# The argument takes either ARM spelling and the script maps to whichever each consumer wants.
case "${1:-}" in
    x86_64|amd64|x64) ARCH=x86_64; PRESET=windows-x86_64 ;;
    aarch64|arm64)    ARCH=aarch64; PRESET=windows-arm64 ;;
    *) echo "usage: build-app.sh <x86_64|aarch64>" >&2; exit 1 ;;
esac

if [ "$ARCH" = aarch64 ]; then
    # llvm-mingw's bin/ on PATH. The toolchain file names the compilers absolutely, so this is not
    # what makes the build work -- it is what makes the SUPPORTING tools (llvm-objdump for
    # make-package.sh's DLL closure, llvm-strip) resolve to the ones that understand ARM64 PE.
    export PATH="${MOSAIC_LLVM_MINGW:-/opt/llvm-mingw}/bin:$PATH"
fi

echo "== building Mosaic for windows-$ARCH (deps: $MOSAIC_WIN_PREFIX) =="
cmake --preset "$PRESET" -S "$REPO"
cmake --build "$REPO/build/$PRESET" -j"$JOBS"
echo "== built: $REPO/build/$PRESET/bin/mosaic.exe =="
