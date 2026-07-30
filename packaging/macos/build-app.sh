#!/usr/bin/env bash
# Configure + build Mosaic for one macOS architecture with osxcross (PLAN.md S58).
# Exports the env the osxcross wrappers need (PATH to the toolchain bin, the per-arch dependency
# prefix) so `cmake --preset` / `cmake --build` work without the caller setting anything up.
#
# Required env:
#   OSXCROSS_ROOT          the osxcross 'target' dir
#   MOSAIC_MAC_DEPS_ROOT   root holding per-arch dep prefixes: <root>/<arch> (built by build-deps.sh
#                          + fetch-vulkan.sh)
# Args: <arm64|x86_64>
set -euo pipefail

: "${OSXCROSS_ROOT:?set OSXCROSS_ROOT}"
: "${MOSAIC_MAC_DEPS_ROOT:?set MOSAIC_MAC_DEPS_ROOT (holds <arch>/ dep prefixes)}"
ARCH="${1:?usage: build-app.sh <arm64|x86_64>}"
JOBS="${JOBS:-$(nproc)}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

export PATH="$OSXCROSS_ROOT/bin:$PATH"
export MOSAIC_MAC_PREFIX="$MOSAIC_MAC_DEPS_ROOT/$ARCH"

echo "== building Mosaic for macos-$ARCH (deps: $MOSAIC_MAC_PREFIX) =="
cmake --preset "macos-$ARCH" -S "$REPO"
cmake --build "$REPO/build/macos-$ARCH" -j"$JOBS"
echo "== built: $REPO/build/macos-$ARCH/bin/mosaic =="
