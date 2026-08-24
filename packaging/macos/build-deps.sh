#!/usr/bin/env bash
# Cross-build Mosaic's third-party dependencies for one macOS architecture using osxcross.
#
# Machine-independent: every path comes from the environment, nothing host-specific is baked in.
#
# Required env:
#   OSXCROSS_ROOT      the osxcross 'target' directory (has bin/, SDK/)
#   MOSAIC_MAC_ARCH    arm64 | x86_64
#   MOSAIC_MAC_PREFIX  install prefix for this arch (e.g. .../deps/arm64)
# Optional env:
#   MOSAIC_MAC_WORK    download + build scratch (default: $MOSAIC_MAC_PREFIX/../work-$ARCH)
#   JOBS               parallel make jobs (default: nproc)
#   MOSAIC_MACOS_MIN   deployment target (default: 11.0)
#
# Idempotent: each library drops a stamp in $PREFIX/.stamps; re-running skips finished ones.
# Run once per arch (they use separate prefixes and may run concurrently).
set -euo pipefail

: "${OSXCROSS_ROOT:?set OSXCROSS_ROOT to the osxcross target dir}"
: "${MOSAIC_MAC_ARCH:?set MOSAIC_MAC_ARCH to arm64 or x86_64}"
: "${MOSAIC_MAC_PREFIX:?set MOSAIC_MAC_PREFIX to the per-arch install prefix}"
JOBS="${JOBS:-$(nproc)}"
MOSAIC_MACOS_MIN="${MOSAIC_MACOS_MIN:-11.0}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

case "$MOSAIC_MAC_ARCH" in
  arm64)  CLANG=oa64-clang;  TRIPLE=aarch64-apple-darwin ;;
  x86_64) CLANG=o64-clang;   TRIPLE=x86_64-apple-darwin  ;;
  *) echo "MOSAIC_MAC_ARCH must be arm64 or x86_64" >&2; exit 1 ;;
esac

PREFIX="$MOSAIC_MAC_PREFIX"
WORK="${MOSAIC_MAC_WORK:-$(dirname "$PREFIX")/work-$MOSAIC_MAC_ARCH}"
STAMPS="$PREFIX/.stamps"
SRC="$WORK/src"
mkdir -p "$PREFIX/lib/pkgconfig" "$STAMPS" "$SRC"

# The darwinNN tool triple that cctools installed (e.g. arm64-apple-darwin24.5-ar).
CC_FULL=$(ls "$OSXCROSS_ROOT"/bin/x86_64-apple-darwin*-clang | head -1)
DARWIN=$(basename "$CC_FULL" | sed -E 's/^x86_64-(apple-darwin[0-9.]+)-clang$/\1/')
TOOLTRIPLE="$MOSAIC_MAC_ARCH-$DARWIN"
[ "$MOSAIC_MAC_ARCH" = arm64 ] && TOOLTRIPLE="arm64-$DARWIN"
SYSROOT=$(ls -d "$OSXCROSS_ROOT"/SDK/MacOSX*.sdk | head -1)

export PATH="$OSXCROSS_ROOT/bin:$PATH"
export CC="$CLANG"
export CXX="${CLANG}++"
export AR="$OSXCROSS_ROOT/bin/${TOOLTRIPLE}-ar"
export RANLIB="$OSXCROSS_ROOT/bin/${TOOLTRIPLE}-ranlib"
export STRIP="$OSXCROSS_ROOT/bin/${TOOLTRIPLE}-strip"
export CFLAGS="-mmacosx-version-min=$MOSAIC_MACOS_MIN -O2 -I$PREFIX/include"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-mmacosx-version-min=$MOSAIC_MACOS_MIN -L$PREFIX/lib"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
CMAKE_TC="$REPO/cmake/toolchains/osxcross.cmake"

echo "== deps for $MOSAIC_MAC_ARCH -> $PREFIX (min $MOSAIC_MACOS_MIN, sdk $(basename "$SYSROOT")) =="

fetch() {  # fetch <url> <outfile> [mirror-url...]
  # Extra URLs are MIRRORS, tried in order when the one before them fails. Upstream hosting is a
  # single point of failure for the whole dependency stack: a CI run lost every architecture to
  # www.freedesktop.org being unreachable from the runner (four connect timeouts at ~135s each,
  # while the same URL served fine elsewhere), which is a bad way to lose an hour of cross-builds.
  # --connect-timeout keeps a dead host from eating minutes before the fallback is even tried.
  local url="$1" out="$SRC/$2"; shift 2
  [ -f "$out" ] && { echo "$out"; return; }
  local u
  for u in "$url" "$@"; do
    echo "  fetch $u" >&2
    if curl -fL --retry 3 --connect-timeout 20 --no-progress-meter -o "$out.part" "$u"; then
      mv "$out.part" "$out"
      echo "$out"
      return
    fi
    rm -f "$out.part"
    echo "  ... unreachable; trying the next mirror" >&2
  done
  echo "all mirrors failed for $2" >&2
  return 1
}
unpack() { tar -xf "$1" -C "$SRC"; }
done_stamp() { [ -f "$STAMPS/$1" ]; }
stamp() { touch "$STAMPS/$1"; }

# apply_patch <srcdir> <patchfile-in-patches/>. unpack() re-extracts on every run, so patches are
# re-applied every run too; --forward makes an already-patched tree a no-op instead of a prompt.
# A patch that does not apply is fatal: it means the dependency moved and the fix is silently gone.
apply_patch() {
  local dir="$1" p="$HERE/patches/$2"
  [ -f "$p" ] || { echo "missing patch $p" >&2; exit 1; }
  patch -p1 -d "$dir" --forward --silent <"$p" && return
  patch -p1 -d "$dir" --dry-run --reverse --silent <"$p" >/dev/null 2>&1 && return
  echo "failed to apply $2 to $dir" >&2; exit 1
}

cmake_build() {  # cmake_build <srcdir> <stamp> [extra -D...]
  local dir="$1" name="$2"; shift 2
  done_stamp "$name" && { echo "  [skip] $name"; return; }
  echo "  [cmake] $name"
  rm -rf "$dir/_b"
  cmake -S "$dir" -B "$dir/_b" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TC" -DMOSAIC_OSX_ARCH="$MOSAIC_MAC_ARCH" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF "$@" >/dev/null
  cmake --build "$dir/_b" -j"$JOBS" >/dev/null
  cmake --install "$dir/_b" >/dev/null
  stamp "$name"
}

# Some deps (fontconfig) run a HOST build tool (gperf) during their build. Provide it without
# touching the system: build it with the host compiler into a private host-tools prefix.
HOSTTOOLS="$WORK/hosttools"
ensure_gperf() {
  command -v gperf >/dev/null 2>&1 && return
  [ -x "$HOSTTOOLS/bin/gperf" ] && { export PATH="$HOSTTOOLS/bin:$PATH"; return; }
  echo "  [host]  gperf (build tool)"
  local t; t=$(fetch "https://ftp.gnu.org/gnu/gperf/gperf-3.1.tar.gz" gperf.tar.gz); unpack "$t"
  ( cd "$SRC/gperf-3.1" && env -u AR -u RANLIB -u STRIP CC=cc CXX=c++ CFLAGS=-O2 CXXFLAGS=-O2 \
      LDFLAGS= ./configure --prefix="$HOSTTOOLS" >/dev/null && \
    make -j"$JOBS" >/dev/null && make install >/dev/null )
  export PATH="$HOSTTOOLS/bin:$PATH"
}

auto_build() {  # auto_build <srcdir> <stamp> [extra ./configure args...]
  local dir="$1" name="$2"; shift 2
  done_stamp "$name" && { echo "  [skip] $name"; return; }
  echo "  [auto]  $name"
  ( cd "$dir" && ./configure --host="$TRIPLE" --prefix="$PREFIX" \
      --enable-static --disable-shared "$@" >/dev/null && \
    make -j"$JOBS" >/dev/null && make install >/dev/null )
  stamp "$name"
}

# ---- zlib (its own configure; honours CC/CFLAGS, no --host) ------------------
if ! done_stamp zlib; then
  echo "  [zlib]  zlib"
  t=$(fetch "https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz" zlib.tar.gz); unpack "$t"
  ( cd "$SRC/zlib-1.3.1" && ./configure --prefix="$PREFIX" --static >/dev/null && \
    make -j"$JOBS" >/dev/null && make install >/dev/null )
  stamp zlib
fi

# ---- lz4 --------------------------------------------------------------------
t=$(fetch "https://github.com/lz4/lz4/releases/download/v1.10.0/lz4-1.10.0.tar.gz" lz4.tar.gz); unpack "$t"
cmake_build "$SRC/lz4-1.10.0/build/cmake" lz4 -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF

# ---- zstd -------------------------------------------------------------------
t=$(fetch "https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz" zstd.tar.gz); unpack "$t"
cmake_build "$SRC/zstd-1.5.6/build/cmake" zstd \
  -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON

# ---- libpng (needs zlib) ----------------------------------------------------
t=$(fetch "https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.44.tar.gz" libpng.tar.gz); unpack "$t"
cmake_build "$SRC/libpng-1.6.44" libpng \
  -DPNG_SHARED=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
  -DZLIB_ROOT="$PREFIX" -DZLIB_LIBRARY="$PREFIX/lib/libz.a" -DZLIB_INCLUDE_DIR="$PREFIX/include"

# ---- libjpeg-turbo ----------------------------------------------------------
t=$(fetch "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.0.4/libjpeg-turbo-3.0.4.tar.gz" jpeg.tar.gz); unpack "$t"
cmake_build "$SRC/libjpeg-turbo-3.0.4" jpeg-turbo \
  -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=ON -DWITH_SIMD=OFF

# ---- lcms2 (autotools) ------------------------------------------------------
t=$(fetch "https://github.com/mm2/Little-CMS/releases/download/lcms2.16/lcms2-2.16.tar.gz" lcms2.tar.gz); unpack "$t"
auto_build "$SRC/lcms2-2.16" lcms2 --without-jpeg --without-tiff

# ---- freetype (CMake; zlib+png, no harfbuzz/brotli to avoid the cycle) -------
t=$(fetch "https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz" freetype.tar.xz); unpack "$t"
cmake_build "$SRC/freetype-2.13.3" freetype-noHb \
  -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON \
  -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON \
  -DZLIB_ROOT="$PREFIX" -DPNG_ROOT="$PREFIX"

# ---- harfbuzz (CMake; with freetype) ----------------------------------------
t=$(fetch "https://github.com/harfbuzz/harfbuzz/releases/download/8.5.0/harfbuzz-8.5.0.tar.xz" harfbuzz.tar.xz); unpack "$t"
cmake_build "$SRC/harfbuzz-8.5.0" harfbuzz \
  -DHB_HAVE_FREETYPE=ON -DHB_BUILD_UTILS=OFF -DHB_BUILD_TESTS=OFF \
  -DFREETYPE_LIBRARY="$PREFIX/lib/libfreetype.a" -DFREETYPE_INCLUDE_DIRS="$PREFIX/include/freetype2"

# ---- (optionally) rebuild freetype WITH harfbuzz for autohinting -------------
# Skipped for v1: Mosaic does not require the ft<->hb autohint coupling.

# ---- expat (CMake) ----------------------------------------------------------
t=$(fetch "https://github.com/libexpat/libexpat/releases/download/R_2_6_4/expat-2.6.4.tar.xz" expat.tar.xz); unpack "$t"
cmake_build "$SRC/expat-2.6.4" expat \
  -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF -DEXPAT_SHARED_LIBS=OFF

# ---- fontconfig (autotools; needs freetype + expat) -------------------------
# macOS font search paths baked in; no fc-cache is run at build time (cross).
ensure_gperf
t=$(fetch "https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.14.2.tar.xz" fontconfig.tar.xz \
      "https://ftp.osuosl.org/pub/blfs/conglomeration/fontconfig/fontconfig-2.14.2.tar.xz"); unpack "$t"
auto_build "$SRC/fontconfig-2.14.2" fontconfig \
  --disable-docs --disable-nls \
  --with-expat="$PREFIX" \
  --with-default-fonts=/System/Library/Fonts \
  --with-add-fonts=/Library/Fonts,~/Library/Fonts \
  FREETYPE_CFLAGS="-I$PREFIX/include/freetype2" \
  FREETYPE_LIBS="-L$PREFIX/lib -lfreetype -lpng16 -lz -lbz2"

# ---- libhyphen (autotools; tiny, no deps) -----------------------------------
# GitHub archive tarballs ship no generated ./configure, so regenerate it first.
t=$(fetch "https://github.com/hunspell/hyphen/archive/refs/tags/v2.8.8.tar.gz" hyphen.tar.gz); unpack "$t"
HDIR="$SRC/hyphen-2.8.8"
# Upstream 2.8.8 lacks hnj_hyphen_load_data (load a dict from memory) -- it is a distro patch the
# Linux build relies on (hyphenator.cpp:128). Add it via fmemopen (macOS 10.13+), which is exactly
# what that patch does; wraps the existing fgets-based hnj_hyphen_load_file.
if ! done_stamp hyphen && ! grep -q hnj_hyphen_load_data "$HDIR/hyphen.h"; then
  sed -i 's|\(HyphenDict \*hnj_hyphen_load_file (FILE \*f);\)|\1\nHyphenDict *hnj_hyphen_load_data (const char *fdata, size_t flen);|' "$HDIR/hyphen.h"
  cat >> "$HDIR/hyphen.c" <<'EOF'

/* Mosaic S58: load a dictionary from an in-memory buffer (distro-patch parity, via fmemopen). */
HyphenDict *hnj_hyphen_load_data (const char *fdata, size_t flen) {
  FILE *f = fmemopen((void*)fdata, flen, "r");
  if (f == NULL) return NULL;
  HyphenDict *result = hnj_hyphen_load_file(f);
  fclose(f);
  return result;
}
EOF
fi
if ! done_stamp hyphen && [ ! -x "$HDIR/configure" ]; then
  ( cd "$HDIR" && autoreconf -fi >/dev/null 2>&1 )
fi
auto_build "$HDIR" hyphen

# ---- gettext runtime (libintl) ----------------------------------------------
# On glibc, gettext IS libc, so find_package(Intl) succeeds for free and the Linux build never
# needed this. macOS has no libintl at all: without it MOSAIC_HAVE_GETTEXT is 0 and the .app is
# English-only no matter how many catalogs the DMG bundles (S54/S59).
#
# Only the `gettext-runtime` sub-package is built -- the outer tree also carries xgettext/msgfmt,
# which are developer tools that run on the HOST and would have to be cross-built for nothing.
t=$(fetch "https://ftp.gnu.org/gnu/gettext/gettext-0.22.5.tar.xz" gettext.tar.xz); unpack "$t"
auto_build "$SRC/gettext-0.22.5/gettext-runtime" gettext-runtime \
  --disable-java --disable-csharp --disable-libasprintf --disable-openmp --without-emacs \
  --disable-c++ --with-included-gettext

# ---- brotli + highway (libjxl's two hard dependencies) ----------------------
# Built as real installed libraries with their own .pc files rather than left to libjxl's bundled
# copies: those are compiled in-tree and never installed, so a static libjxl would link against
# archives that do not exist by the time Mosaic links.
t=$(fetch "https://github.com/google/brotli/archive/refs/tags/v1.1.0.tar.gz" brotli.tar.gz); unpack "$t"
cmake_build "$SRC/brotli-1.1.0" brotli -DBROTLI_BUILD_TOOLS=OFF -DBROTLI_DISABLE_TESTS=ON

t=$(fetch "https://github.com/google/highway/archive/refs/tags/1.2.0.tar.gz" highway.tar.gz); unpack "$t"
cmake_build "$SRC/highway-1.2.0" highway \
  -DHWY_ENABLE_TESTS=OFF -DHWY_ENABLE_EXAMPLES=OFF -DHWY_ENABLE_CONTRIB=OFF -DBUILD_TESTING=OFF

# ---- libjxl (JPEG XL) -------------------------------------------------------
# JPEG XL is a FIRST-CLASS Mosaic format (File -> Quick Export as JPEG XL, and the Export dialog),
# and io/CMakeLists.txt treats libjxl as optional -- so without this the macOS build silently shipped
# without it. Version-matched to the Linux build (0.11.2). Everything but the library is switched
# off: the tools, plugins and benchmarks are developer surface, and the codec takes colour management
# from the lcms2 built above rather than its bundled skcms, so the .app has ONE CMS.
t=$(fetch "https://github.com/libjxl/libjxl/archive/refs/tags/v0.11.2.tar.gz" libjxl.tar.gz); unpack "$t"
cmake_build "$SRC/libjxl-0.11.2" libjxl \
  -DBUILD_TESTING=OFF \
  -DJPEGXL_ENABLE_TOOLS=OFF -DJPEGXL_ENABLE_BENCHMARK=OFF -DJPEGXL_ENABLE_EXAMPLES=OFF \
  -DJPEGXL_ENABLE_MANPAGES=OFF -DJPEGXL_ENABLE_JNI=OFF -DJPEGXL_ENABLE_SJPEG=OFF \
  -DJPEGXL_ENABLE_OPENEXR=OFF -DJPEGXL_ENABLE_SKCMS=OFF -DJPEGXL_ENABLE_PLUGINS=OFF \
  -DJPEGXL_ENABLE_DEVTOOLS=OFF -DJPEGXL_ENABLE_DOXYGEN=OFF -DJPEGXL_ENABLE_TRANSCODE_JPEG=ON \
  -DJPEGXL_FORCE_SYSTEM_BROTLI=ON -DJPEGXL_FORCE_SYSTEM_HWY=ON -DJPEGXL_FORCE_SYSTEM_LCMS2=ON \
  -DJPEGXL_BUNDLE_LIBPNG=OFF -DJPEGXL_ENABLE_AVX512=OFF

# ---- spdlog (CMake; bundles fmt) --------------------------------------------
t=$(fetch "https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.tar.gz" spdlog.tar.gz); unpack "$t"
cmake_build "$SRC/spdlog-1.14.1" spdlog \
  -DSPDLOG_BUILD_SHARED=OFF -DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_BUILD_TESTS=OFF -DSPDLOG_INSTALL=ON

# ---- FLTK (CMake; Cocoa backend) --------------------------------------------
t=$(fetch "https://github.com/fltk/fltk/releases/download/release-1.4.5/fltk-1.4.5-source.tar.gz" fltk.tar.gz); unpack "$t"
# Open panel crashes on an item with no filesystem path (see the patch header for the trace).
apply_patch "$SRC/fltk-1.4.5" fltk-1.4.5-open-panel-nil-path.patch
cmake_build "$SRC/fltk-1.4.5" fltk \
  -DFLTK_BUILD_TEST=OFF -DFLTK_BUILD_EXAMPLES=OFF -DFLTK_BUILD_FLUID=OFF \
  -DFLTK_BUILD_SHARED_LIBS=OFF -DFLTK_BACKEND_X11=OFF -DFLTK_BACKEND_WAYLAND=OFF \
  -DFLTK_USE_SYSTEM_LIBJPEG=OFF -DFLTK_USE_SYSTEM_LIBPNG=OFF -DFLTK_USE_SYSTEM_ZLIB=OFF \
  -DFLTK_USE_PANGO=OFF -DFLTK_GRAPHICS_CAIRO=OFF

echo "== done: deps for $MOSAIC_MAC_ARCH in $PREFIX =="
