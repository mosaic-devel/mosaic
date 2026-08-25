#!/usr/bin/env bash
# Build a relocatable Mosaic AppImage for one architecture (PLAN.md S59).
#
# Machine-independent: every path comes from the environment, nothing host-specific is baked in.
#
# Optional env:
#   MOSAIC_APPIMAGE_ARCH   x86_64 | aarch64        (default: uname -m)
#   MOSAIC_APPIMAGE_OUT    output directory        (default: build/linux-appimage)
#   MOSAIC_APPIMAGETOOL    path to appimagetool    (default: downloaded next to the output)
#   MOSAIC_APPIMAGETOOL_VERSION  appimagetool release to fetch (default: the pin below)
#   MOSAIC_SKIP_BUILD=1    reuse an existing build/linux-release tree
#   JOBS                   parallel build jobs     (default: nproc)
#
#   ⚠ BUILD HOST MATTERS. An AppImage's glibc floor is the glibc it was BUILT against -- glibc is
#   the one library an AppImage must never bundle (the dynamic loader and libc must be the host's,
#   or nothing loads). Building this on a rolling distro produces an image that runs on that
#   rolling distro and nowhere else. CI builds it on Ubuntu 24.04 (glibc 2.39) deliberately; see
#   .github/workflows/release.yml.
set -euo pipefail

ARCH="${MOSAIC_APPIMAGE_ARCH:-$(uname -m)}"
# PINNED, not "continuous". appimagetool's continuous tag is rebuilt in place, so the packer that
# produced a shipped release could not be identified afterwards, and a rebuild of an old tag would
# silently use a newer packer. Bump deliberately.
APPIMAGETOOL_VERSION="${MOSAIC_APPIMAGETOOL_VERSION:-1.9.1}"
JOBS="${JOBS:-$(nproc)}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="${MOSAIC_APPIMAGE_OUT:-$REPO/build/linux-appimage}"
BUILD="$REPO/build/linux-release"
APPDIR="$OUT/Mosaic.AppDir"

case "$ARCH" in
  x86_64|aarch64) ;;
  *) echo "unsupported arch '$ARCH' (want x86_64 or aarch64)" >&2; exit 1 ;;
esac

mkdir -p "$OUT"
rm -rf "$APPDIR"

# ---- 1. build ---------------------------------------------------------------
# CMAKE_INSTALL_PREFIX is /usr because that is the layout an AppImage expects inside the AppDir.
# The absolute paths this bakes into the binary (MOSAIC_DATA_DIR, MOSAIC_LOCALEDIR) name /usr/...,
# which is NOT where the mounted image lives -- AppRun overrides both with $APPDIR-relative values.
if [ "${MOSAIC_SKIP_BUILD:-0}" != 1 ]; then
  cmake -S "$REPO" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DMOSAIC_BUILD_TESTS=OFF
  cmake --build "$BUILD" -j "$JOBS"
fi

# MOSAIC_VERSION overrides for a release build, matching the Windows and macOS packagers.
VERSION="${MOSAIC_VERSION:-$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\+\([0-9]\+\.[0-9]\+\.[0-9]\+\).*/\1/p' \
            "$REPO/CMakeLists.txt" | head -1)}"
[ -n "$VERSION" ] || { echo "could not read project version from CMakeLists.txt" >&2; exit 1; }
echo "== Mosaic $VERSION AppImage for $ARCH =="

# ---- 2. install into the AppDir ---------------------------------------------
DESTDIR="$APPDIR" cmake --install "$BUILD" >/dev/null
[ -x "$APPDIR/usr/bin/mosaic" ] || { echo "install produced no usr/bin/mosaic" >&2; exit 1; }

# ---- 3. bundle the shared-library closure -----------------------------------
# Everything ldd resolves, MINUS the set that must always come from the host. Two different
# reasons live in that exclusion list and they are worth keeping straight:
#
#   glibc + the loader   an AppImage runs under the HOST's ld.so; a bundled libc would be a second,
#                        mismatched C library. This is why the build host sets the glibc floor.
#   the GPU stack        libGL/libEGL/libdrm/libgbm and friends are the driver, which is hardware-
#                        and kernel-specific. Also libstdc++/libgcc: Mesa's Vulkan drivers (radv,
#                        lavapipe) link libLLVM, which needs the HOST's libstdc++ -- bundling ours
#                        in front of it on LD_LIBRARY_PATH is a known way to make the GPU vanish.
#                        Every host new enough to satisfy the glibc floor already has a libstdc++
#                        at least as new as the one we built against.
#   the display protocol libX11/libxcb/libwayland speak to the running server and are present on
#                        every graphical system; bundling them buys nothing and risks a mismatch.
#
# libvulkan.so.1 IS bundled: it is the loader, not the driver -- it finds the host's ICDs through
# /usr/share/vulkan/icd.d, which AppRun leaves pointing at the host.
LIBDIR="$APPDIR/usr/lib"
mkdir -p "$LIBDIR"

is_excluded() {
  case "$1" in
    ld-linux*|libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|libutil.so.*) return 0 ;;
    libresolv.so.*|libnsl.so.*|libanl.so.*|libnss_*|libcrypt.so.*|libthread_db.so.*)  return 0 ;;
    libstdc++.so.*|libgcc_s.so.*)                                                     return 0 ;;
    libGL.so.*|libEGL.so.*|libGLX.so.*|libGLdispatch.so.*|libOpenGL.so.*|libglapi.so.*) return 0 ;;
    libdrm.so.*|libgbm.so.*|libxcb-dri*|libxcb-glx*|libxcb-present*|libxcb-sync*)     return 0 ;;
    libX11.so.*|libX11-xcb.so.*|libxcb.so.*|libxcb-*|libXau.so.*|libXdmcp.so.*)       return 0 ;;
    libXext.so.*|libXi.so.*|libXrandr.so.*|libXrender.so.*|libXfixes.so.*)            return 0 ;;
    libXcursor.so.*|libXinerama.so.*|libXss.so.*|libxkbcommon*.so.*)                  return 0 ;;
    libwayland-*.so.*)                                                                return 0 ;;
    *) return 1 ;;
  esac
}

# Transitive closure: seed with the binaries, then follow each newly copied library's own deps.
declare -A seen=()
queue=("$APPDIR/usr/bin/mosaic")
if [ -x "$APPDIR/usr/bin/mosaic-thumbnailer" ]; then
  queue+=("$APPDIR/usr/bin/mosaic-thumbnailer")
fi

while [ ${#queue[@]} -gt 0 ]; do
  obj="${queue[0]}"; queue=("${queue[@]:1}")
  while read -r soname path; do
    [ -n "$path" ] && [ -e "$path" ] || continue
    is_excluded "$soname" && continue
    [ -n "${seen[$soname]:-}" ] && continue
    seen[$soname]=1
    cp -L "$path" "$LIBDIR/$soname"
    queue+=("$LIBDIR/$soname")
  done < <(ldd "$obj" 2>/dev/null | awk '$2=="=>" && $3 ~ /^\// {print $1, $3}')
done
echo "   bundled ${#seen[@]} shared libraries"

# ---- 4. AppRun --------------------------------------------------------------
# The three environment overrides are not optional. installedDataDir() and resolveLocaleDir()
# (src/common/settings.cpp, src/common/i18n.cpp) otherwise return the compile-time /usr/... paths,
# which inside a mounted AppImage name the HOST's /usr -- so the brush bundle, the CMYK press
# profile and all 74 catalogs would silently be missing.
cat > "$APPDIR/AppRun" <<'RUN'
#!/bin/sh
APPDIR="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$APPDIR/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export MOSAIC_DATA_DIR="${MOSAIC_DATA_DIR:-$APPDIR/usr/share/mosaic}"
export MOSAIC_LOCALEDIR="${MOSAIC_LOCALEDIR:-$APPDIR/usr/share/locale}"
# The .desktop/icon/MIME data the app itself looks up (its own document icon, the mimetype entry).
export XDG_DATA_DIRS="$APPDIR/usr/share${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}:/usr/local/share:/usr/share"
exec "$APPDIR/usr/bin/mosaic" "$@"
RUN
chmod +x "$APPDIR/AppRun"

# ---- 5. desktop entry + icon at the AppDir root -----------------------------
# appimagetool requires both at the top level; the icon basename must equal the .desktop's Icon=.
cp "$APPDIR/usr/share/applications/mosaic.desktop" "$APPDIR/mosaic.desktop"
cp "$APPDIR/usr/share/icons/hicolor/scalable/apps/mosaic.svg" "$APPDIR/mosaic.svg"
cp "$APPDIR/mosaic.svg" "$APPDIR/.DirIcon"

# ---- 6. pack ----------------------------------------------------------------
TOOL="${MOSAIC_APPIMAGETOOL:-}"
if [ -z "$TOOL" ]; then
  # NOT in $OUT itself: that directory is what CI uploads as the release artifact, and a glob of
  # *.AppImage there would ship appimagetool alongside Mosaic as though it were part of the release.
  mkdir -p "$OUT/.tools"
  TOOL="$OUT/.tools/appimagetool-$ARCH.AppImage"
  if [ ! -x "$TOOL" ]; then
    echo "   fetching appimagetool for $ARCH"
    curl -fL --retry 3 -o "$TOOL" \
      "https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL_VERSION/appimagetool-$ARCH.AppImage"
    chmod +x "$TOOL"
  fi
fi

IMAGE="$OUT/Mosaic-$VERSION-$ARCH.AppImage"
rm -f "$IMAGE"
# --appimage-extract-and-run: CI containers have no FUSE, so appimagetool cannot mount itself.
# --no-appstream: Mosaic ships no AppStream metainfo yet (still open under S59).
ARCH="$ARCH" "$TOOL" --appimage-extract-and-run --no-appstream "$APPDIR" "$IMAGE"

echo "== $IMAGE =="
