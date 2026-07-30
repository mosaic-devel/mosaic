#!/usr/bin/env bash
# Provision Vulkan for the macOS cross-build (PLAN.md S58): the Vulkan-Loader + Vulkan-Headers
# (built per arch with osxcross so find_package(Vulkan) resolves) and the prebuilt MoltenVK ICD
# (the Metal translation layer, staged for bundling). MoltenVK itself is NOT cross-built -- its
# build system is Xcode-only -- so we take the official universal prebuilt dylib.
#
# Same env contract as build-deps.sh:
#   OSXCROSS_ROOT, MOSAIC_MAC_ARCH (arm64|x86_64), MOSAIC_MAC_PREFIX  (required)
#   MOSAIC_MAC_WORK           (optional) build scratch
#   MOSAIC_MAC_VULKAN_STAGE   (optional) where the universal libMoltenVK.dylib + icd json are staged
#                             for the app bundle (default: <prefix>/../vulkan-runtime)
#   JOBS                      (optional) parallel jobs
set -euo pipefail

: "${OSXCROSS_ROOT:?set OSXCROSS_ROOT}"
: "${MOSAIC_MAC_ARCH:?set MOSAIC_MAC_ARCH to arm64 or x86_64}"
: "${MOSAIC_MAC_PREFIX:?set MOSAIC_MAC_PREFIX}"
JOBS="${JOBS:-$(nproc)}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
PREFIX="$MOSAIC_MAC_PREFIX"
WORK="${MOSAIC_MAC_WORK:-$(dirname "$PREFIX")/work-$MOSAIC_MAC_ARCH}"
STAGE="${MOSAIC_MAC_VULKAN_STAGE:-$(dirname "$PREFIX")/vulkan-runtime}"
SRC="$WORK/src"
STAMPS="$PREFIX/.stamps"
mkdir -p "$SRC" "$STAMPS" "$STAGE"

VULKAN_SDK_TAG="vulkan-sdk-1.4.350.1"
MOLTENVK_TAG="v1.4.2-rc1"
CMAKE_TC="$REPO/cmake/toolchains/osxcross.cmake"

# The osxcross clang wrappers find their companion ld64/as on PATH; without this the wrapper falls
# back to the host GNU ld and the compiler check fails ("ld: unrecognised emulation mode").
export PATH="$OSXCROSS_ROOT/bin:$PATH"

fetch() { local url="$1" out="$SRC/$2"; [ -f "$out" ] && { echo "$out"; return; }
  echo "  fetch $url" >&2; curl -fL --retry 3 -o "$out.part" "$url"; mv "$out.part" "$out"; echo "$out"; }

# ---- Vulkan-Headers (arch-independent, installed into the per-arch prefix) ---
if [ ! -f "$STAMPS/vulkan-headers" ]; then
  echo "  [headers] Vulkan-Headers $VULKAN_SDK_TAG"
  t=$(fetch "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/$VULKAN_SDK_TAG.tar.gz" vkheaders.tar.gz)
  tar -xf "$t" -C "$SRC"
  cmake -S "$SRC/Vulkan-Headers-$VULKAN_SDK_TAG" -B "$SRC/Vulkan-Headers-$VULKAN_SDK_TAG/_b" \
    -G Ninja -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
  cmake --install "$SRC/Vulkan-Headers-$VULKAN_SDK_TAG/_b" >/dev/null
  touch "$STAMPS/vulkan-headers"
fi

# ---- Vulkan-Loader (per arch -> libvulkan.dylib, so find_package(Vulkan) resolves) ----
if [ ! -f "$STAMPS/vulkan-loader" ]; then
  echo "  [loader]  Vulkan-Loader $VULKAN_SDK_TAG ($MOSAIC_MAC_ARCH)"
  t=$(fetch "https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/$VULKAN_SDK_TAG.tar.gz" vkloader.tar.gz)
  tar -xf "$t" -C "$SRC"
  ld="$SRC/Vulkan-Loader-$VULKAN_SDK_TAG"
  rm -rf "$ld/_b"
  cmake -S "$ld" -B "$ld/_b" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TC" -DMOSAIC_OSX_ARCH="$MOSAIC_MAC_ARCH" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
    -DVULKAN_HEADERS_INSTALL_DIR="$PREFIX" -DBUILD_TESTS=OFF \
    -DBUILD_WSI_XCB_SUPPORT=OFF -DBUILD_WSI_XLIB_SUPPORT=OFF -DBUILD_WSI_WAYLAND_SUPPORT=OFF >/dev/null
  cmake --build "$ld/_b" -j"$JOBS" >/dev/null
  cmake --install "$ld/_b" >/dev/null
  touch "$STAMPS/vulkan-loader"
fi

# ---- MoltenVK (prebuilt universal ICD; staged for the app bundle) -----------
if [ ! -f "$STAGE/.moltenvk-stamp" ]; then
  echo "  [moltenvk] MoltenVK $MOLTENVK_TAG (universal prebuilt)"
  t=$(fetch "https://github.com/KhronosGroup/MoltenVK/releases/download/$MOLTENVK_TAG/MoltenVK-macos.tar" moltenvk.tar)
  rm -rf "$SRC/MoltenVK"
  tar -xf "$t" -C "$SRC"
  dylib=$(find "$SRC/MoltenVK" -name 'libMoltenVK.dylib' | head -1)
  icd=$(find "$SRC/MoltenVK" -name 'MoltenVK_icd.json' | head -1)
  [ -n "$dylib" ] || { echo "libMoltenVK.dylib not found in tarball" >&2; exit 1; }
  cp "$dylib" "$STAGE/libMoltenVK.dylib"
  # The staged ICD manifest points the loader at the co-located dylib (bundle rewrites the path).
  cat > "$STAGE/MoltenVK_icd.json" <<'JSON'
{
  "file_format_version": "1.0.0",
  "ICD": {
    "library_path": "./libMoltenVK.dylib",
    "api_version": "1.4.0",
    "is_portability_driver": true
  }
}
JSON
  # Also copy MoltenVK's headers (moltenVK config) if present -- harmless if absent.
  touch "$STAGE/.moltenvk-stamp"
fi

echo "== vulkan ready for $MOSAIC_MAC_ARCH: loader+headers in $PREFIX, MoltenVK staged in $STAGE =="
