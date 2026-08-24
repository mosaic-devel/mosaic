#!/usr/bin/env bash
# Cross-build Mosaic's third-party dependencies for one Windows architecture using MinGW-w64.
#
# Machine-independent: every path comes from the environment, nothing host-specific is baked in.
#
# Required env:
#   MOSAIC_WIN_ARCH    x86_64 | aarch64
#   MOSAIC_WIN_PREFIX  install prefix for this arch (e.g. .../deps/x86_64)
# Optional env:
#   MOSAIC_WIN_WORK    download + build scratch (default: $MOSAIC_WIN_PREFIX/../work-$ARCH)
#   MOSAIC_LLVM_MINGW  llvm-mingw root for aarch64 (default /opt/llvm-mingw)
#   JOBS               parallel make jobs (default: nproc)
#   MOSAIC_WIN_SKIP    space-separated stamp names to skip (e.g. "aom libavif")
#
# Everything is built SHARED (.dll + import library), which is the S57 decision: Mosaic's own
# modules link statically into mosaic.exe, the third-party stack ships as DLLs beside it. That is
# the ordinary layout for a MinGW application, it keeps the executable small enough to link
# quickly, and it means a single library can be replaced without relinking the program.
#
# Idempotent: each library drops a stamp in $PREFIX/.stamps; re-running skips finished ones.
# Run once per arch (they use separate prefixes and may run concurrently).
set -euo pipefail

: "${MOSAIC_WIN_ARCH:?set MOSAIC_WIN_ARCH to x86_64 or aarch64}"
: "${MOSAIC_WIN_PREFIX:?set MOSAIC_WIN_PREFIX to the per-arch install prefix}"
JOBS="${JOBS:-$(nproc)}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SKIP_LIST=" ${MOSAIC_WIN_SKIP:-} "

case "$MOSAIC_WIN_ARCH" in
  x86_64)
    TRIPLE="x86_64-w64-mingw32"
    TOOLBIN=""                       # the GNU toolchain lives on PATH under its triple prefix
    ;;
  aarch64)
    TRIPLE="aarch64-w64-mingw32"
    LLVMROOT="${MOSAIC_LLVM_MINGW:-/opt/llvm-mingw}"
    [ -x "$LLVMROOT/bin/$TRIPLE-clang" ] || {
      echo "llvm-mingw not found at $LLVMROOT (needed for aarch64)" >&2; exit 1; }
    TOOLBIN="$LLVMROOT/bin/"
    ;;
  *) echo "MOSAIC_WIN_ARCH must be x86_64 or aarch64" >&2; exit 1 ;;
esac

PREFIX="$MOSAIC_WIN_PREFIX"
WORK="${MOSAIC_WIN_WORK:-$(dirname "$PREFIX")/work-$MOSAIC_WIN_ARCH}"
STAMPS="$PREFIX/.stamps"
SRC="$WORK/src"
mkdir -p "$PREFIX/lib/pkgconfig" "$PREFIX/bin" "$STAMPS" "$SRC"

if [ "$MOSAIC_WIN_ARCH" = aarch64 ]; then
  export PATH="$LLVMROOT/bin:$PATH"
  export CC="$TRIPLE-clang" CXX="$TRIPLE-clang++"
  export AR="$LLVMROOT/bin/llvm-ar" RANLIB="$LLVMROOT/bin/llvm-ranlib"
  export STRIP="$LLVMROOT/bin/llvm-strip" DLLTOOL="$LLVMROOT/bin/llvm-dlltool"
  export RC="$TRIPLE-windres"
else
  export CC="$TRIPLE-gcc" CXX="$TRIPLE-g++"
  export AR="$TRIPLE-ar" RANLIB="$TRIPLE-ranlib"
  export STRIP="$TRIPLE-strip" DLLTOOL="$TRIPLE-dlltool"
  export RC="$TRIPLE-windres"
fi

# -D_WIN32_WINNT must match the toolchain file's floor: a dependency compiled against an older
# Windows header set can pick a different struct layout for the same API.
export CPPFLAGS="-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -I$PREFIX/include"
export CFLAGS="-O2 -I$PREFIX/include"
export CXXFLAGS="-O2 -I$PREFIX/include"
export LDFLAGS="-L$PREFIX/lib"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
unset PKG_CONFIG_SYSROOT_DIR
CMAKE_TC="$REPO/cmake/toolchains/mingw-w64.cmake"

echo "== deps for $MOSAIC_WIN_ARCH ($TRIPLE) -> $PREFIX =="

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
done_stamp() {
  [ -f "$STAMPS/$1" ] && return 0
  case "$SKIP_LIST" in *" $1 "*) echo "  [skip*] $1 (MOSAIC_WIN_SKIP)"; return 0 ;; esac
  return 1
}
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

# Per-package build logs, and the tail of one printed on failure. NOT >/dev/null: ninja reports a
# compile failure on STDOUT, so discarding stdout turns every build error into a bare "exit 1" with
# no clue which library or which file -- one wasted round-trip per failure, and this script has a
# dozen libraries that can each fail for their own reason.
LOGDIR="$WORK/logs"
mkdir -p "$LOGDIR"
run_logged() {  # run_logged <logname> <cmd...>
  local name="$1"; shift
  if ! "$@" >>"$LOGDIR/$name.log" 2>&1; then
    echo "  !! $name failed -- last 40 lines of $LOGDIR/$name.log:" >&2
    tail -40 "$LOGDIR/$name.log" >&2
    return 1
  fi
}

cmake_build() {  # cmake_build <srcdir> <stamp> [extra -D...]
  local dir="$1" name="$2"; shift 2
  done_stamp "$name" && { echo "  [skip] $name"; return; }
  echo "  [cmake] $name"
  rm -rf "$dir/_b" ; : >"$LOGDIR/$name.log"
  run_logged "$name" cmake -S "$dir" -B "$dir/_b" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TC" -DMOSAIC_WIN_ARCH="$MOSAIC_WIN_ARCH" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_BINDIR=bin -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_SHARED_LIBS=ON "$@"
  run_logged "$name" cmake --build "$dir/_b" -j"$JOBS"
  run_logged "$name" cmake --install "$dir/_b"
  stamp "$name"
}

auto_build() {  # auto_build <srcdir> <stamp> [extra ./configure args...]
  local dir="$1" name="$2"; shift 2
  done_stamp "$name" && { echo "  [skip] $name"; return; }
  echo "  [auto]  $name"
  # ⚠ Do NOT add -no-undefined to LDFLAGS here, however many mingw guides suggest it. It is a
  # LIBTOOL flag, not a compiler one, and both consumers of LDFLAGS reject it: gcc fails outright
  # ("unrecognized command-line option"), which kills configure's own "C compiler cannot create
  # executables" probe before a single dependency configures, and CMake inherits the same variable
  # from the environment and fails its compiler test identically. A package that genuinely needs the
  # flag needs it in its own libfoo_la_LDFLAGS; libtool otherwise infers PE's no-undefined rule from
  # the host triple on its own. If some dependency silently produces no .dll, that is the thing to
  # patch -- per package, not globally.
  # MAKE_INSTALL_ARGS lets one package override a make variable at install time (fontconfig needs
  # it; see there). Deliberately not a configure argument -- these are Makefile variables.
  : >"$LOGDIR/$name.log"
  ( cd "$dir" \
    && run_logged "$name" ./configure --host="$TRIPLE" --prefix="$PREFIX" \
         --enable-shared --disable-static "$@" \
    && run_logged "$name" make -j"$JOBS" \
    && run_logged "$name" make install ${MAKE_INSTALL_ARGS:-} )
  stamp "$name"
}

# Some deps (fontconfig) run a HOST build tool during their build. Provide it without touching the
# system: build it with the host compiler into a private host-tools prefix.
HOSTTOOLS="$WORK/hosttools"
ensure_gperf() {
  command -v gperf >/dev/null 2>&1 && return
  [ -x "$HOSTTOOLS/bin/gperf" ] && { export PATH="$HOSTTOOLS/bin:$PATH"; return; }
  echo "  [host]  gperf (build tool)"
  local t; t=$(fetch "https://ftp.gnu.org/gnu/gperf/gperf-3.1.tar.gz" gperf.tar.gz); unpack "$t"
  ( cd "$SRC/gperf-3.1" && env -u AR -u RANLIB -u STRIP -u CC -u CXX -u RC -u DLLTOOL \
      CFLAGS=-O2 CXXFLAGS=-O2 CPPFLAGS= LDFLAGS= ./configure --prefix="$HOSTTOOLS" >/dev/null && \
    make -j"$JOBS" >/dev/null && make install >/dev/null )
  export PATH="$HOSTTOOLS/bin:$PATH"
}

# ---- zlib -------------------------------------------------------------------
t=$(fetch "https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz" zlib.tar.gz); unpack "$t"
cmake_build "$SRC/zlib-1.3.1" zlib -DZLIB_BUILD_EXAMPLES=OFF

# ⚠ zlib's CMake build contradicts its OWN pkg-config file on Windows: it installs the import library
# as `libzlib.dll.a` (its CMake target is named `zlib`), while the `zlib.pc` it installs alongside
# advertises `Libs: ... -lz`. So every consumer that finds zlib through pkg-config -- which is how
# src/io/CMakeLists.txt does it -- emits `-lz` and the link dies with "cannot find -lz" *after* the
# whole tree has compiled. The alias is the fix distributions apply too.
#
# Deliberately OUTSIDE the stamped build so it is re-applied on every run: it repairs a prefix that
# was populated before this step existed, which is worth more than skipping one file copy.
if [ -f "$PREFIX/lib/libzlib.dll.a" ] && [ ! -f "$PREFIX/lib/libz.dll.a" ]; then
  cp "$PREFIX/lib/libzlib.dll.a" "$PREFIX/lib/libz.dll.a"
fi

# ---- lz4 --------------------------------------------------------------------
t=$(fetch "https://github.com/lz4/lz4/releases/download/v1.10.0/lz4-1.10.0.tar.gz" lz4.tar.gz); unpack "$t"
cmake_build "$SRC/lz4-1.10.0/build/cmake" lz4 -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF

# ---- zstd -------------------------------------------------------------------
t=$(fetch "https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz" zstd.tar.gz); unpack "$t"
cmake_build "$SRC/zstd-1.5.6/build/cmake" zstd \
  -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_SHARED=ON -DZSTD_BUILD_STATIC=OFF

# ---- libpng (needs zlib) ----------------------------------------------------
t=$(fetch "https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.44.tar.gz" libpng.tar.gz); unpack "$t"
cmake_build "$SRC/libpng-1.6.44" libpng \
  -DPNG_SHARED=ON -DPNG_STATIC=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
  -DZLIB_ROOT="$PREFIX" -DZLIB_INCLUDE_DIR="$PREFIX/include"

# ---- libjpeg-turbo ----------------------------------------------------------
# SIMD needs a HOST assembler on x86_64 (nasm/yasm emitting win64 objects); aarch64 uses NEON
# intrinsics through the C compiler and needs none. have_asm is shared with libaom at the bottom.
have_asm() { command -v nasm >/dev/null 2>&1 || command -v yasm >/dev/null 2>&1; }
if [ "$MOSAIC_WIN_ARCH" = aarch64 ] || have_asm; then JPEG_SIMD=ON; else JPEG_SIMD=OFF; fi
t=$(fetch "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.0.4/libjpeg-turbo-3.0.4.tar.gz" jpeg.tar.gz); unpack "$t"
cmake_build "$SRC/libjpeg-turbo-3.0.4" jpeg-turbo \
  -DENABLE_SHARED=ON -DENABLE_STATIC=OFF -DWITH_TURBOJPEG=ON -DWITH_SIMD="$JPEG_SIMD"

# ---- lcms2 (autotools) ------------------------------------------------------
t=$(fetch "https://github.com/mm2/Little-CMS/releases/download/lcms2.16/lcms2-2.16.tar.gz" lcms2.tar.gz); unpack "$t"
auto_build "$SRC/lcms2-2.16" lcms2 --without-jpeg --without-tiff

# ---- freetype (CMake; zlib+png, no harfbuzz/brotli to avoid the cycle) -------
t=$(fetch "https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz" freetype.tar.xz); unpack "$t"
cmake_build "$SRC/freetype-2.13.3" freetype \
  -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON \
  -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON \
  -DZLIB_ROOT="$PREFIX" -DPNG_ROOT="$PREFIX"

# ---- harfbuzz (CMake; with freetype) ----------------------------------------
t=$(fetch "https://github.com/harfbuzz/harfbuzz/releases/download/8.5.0/harfbuzz-8.5.0.tar.xz" harfbuzz.tar.xz); unpack "$t"
cmake_build "$SRC/harfbuzz-8.5.0" harfbuzz \
  -DHB_HAVE_FREETYPE=ON -DHB_BUILD_UTILS=OFF -DHB_BUILD_TESTS=OFF \
  -DFREETYPE_LIBRARY="$PREFIX/lib/libfreetype.dll.a" \
  -DFREETYPE_INCLUDE_DIRS="$PREFIX/include/freetype2"

# ---- expat (CMake) ----------------------------------------------------------
t=$(fetch "https://github.com/libexpat/libexpat/releases/download/R_2_6_4/expat-2.6.4.tar.xz" expat.tar.xz); unpack "$t"
cmake_build "$SRC/expat-2.6.4" expat \
  -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF -DEXPAT_SHARED_LIBS=ON

# ---- fontconfig (autotools; needs freetype + expat) -------------------------
# WINDOWSFONTDIR and LOCAL_APPDATA_FONTCONFIG_CACHE are fontconfig's OWN Windows tokens: it expands
# them at runtime against the real shell folders, so nothing here bakes in a C:\ path. Mosaic keeps
# fontconfig on every platform rather than adding a DirectWrite backend -- font_db.cpp is one code
# path, and the S58 macOS port made the same call.
#
# ⚠ baseconfigdir takes a REAL PATH, not one of those tokens (I tried a token first: `fonts.conf` and
# `conf.d` were installed into a literal directory *named* `WINDOWSTEMPDIR_FONTCONFIG_CACHE` inside
# the build tree, and the prefix got no config at all). It becomes the compiled-in default config
# location, which is a Linux path that will not exist on the target -- that is fine and covered twice
# over: make-package.sh ships this etc/fonts tree beside the exe and the app points
# FONTCONFIG_PATH at it, and if that ever fails fontconfig falls back to a config compiled INTO the
# DLL which already scans WINDOWSFONTDIR. The externally-shipped conf.d is what adds the 40
# conf.avail rules (generic-family aliases, hinting/antialias defaults) that the built-in fallback
# has no way to carry -- without them FcFontMatch cannot resolve "sans-serif" to a real family.
#
# ⚠ RUN_FC_CACHE_TEST=false is not optional. fontconfig's `install-data-local` runs the fc-cache it
# just built to seed the font cache, and its cross-compile guard is `test -z "$(DESTDIR)"` -- which
# is TRUE for a prefix install, so it fires even though the binary it is about to run is a Windows
# .exe. On a host with a Wine binfmt handler registered (this one has) that .exe actually LAUNCHES,
# then dies on a missing libgcc_s_seh-1.dll, and `make install` fails after everything has already
# built correctly. The Makefile ships the override commented out one line above the live definition.
# Seeding a font cache at build time would be wrong regardless: the cache belongs to the machine the
# app runs on, and fontconfig resolves it from LOCAL_APPDATA_FONTCONFIG_CACHE there.
ensure_gperf
t=$(fetch "https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.14.2.tar.xz" fontconfig.tar.xz \
      "https://ftp.osuosl.org/pub/blfs/conglomeration/fontconfig/fontconfig-2.14.2.tar.xz"); unpack "$t"
MAKE_INSTALL_ARGS="RUN_FC_CACHE_TEST=false" \
auto_build "$SRC/fontconfig-2.14.2" fontconfig \
  --disable-docs --disable-nls --with-expat="$PREFIX" \
  --with-default-fonts=WINDOWSFONTDIR \
  --with-cache-dir=LOCAL_APPDATA_FONTCONFIG_CACHE \
  --with-baseconfigdir="$PREFIX/etc/fonts" \
  FREETYPE_CFLAGS="-I$PREFIX/include/freetype2" \
  FREETYPE_LIBS="-L$PREFIX/lib -lfreetype"

# ---- libhyphen (autotools; tiny, no deps) -----------------------------------
# Windows has no system hyphenation API (macOS answers with CoreFoundation, S58), so libhyphen
# ships on Windows exactly as it does on Linux -- with its dictionaries bundled into the payload,
# since there is no /usr/share/hyphen to read them from.
t=$(fetch "https://github.com/hunspell/hyphen/archive/refs/tags/v2.8.8.tar.gz" hyphen.tar.gz); unpack "$t"
HDIR="$SRC/hyphen-2.8.8"
# Upstream 2.8.8 lacks hnj_hyphen_load_data (load a dict from memory) -- it is a distro patch the
# Linux build relies on (hyphenator.cpp), so the symbol has to exist here too or core/ fails to link.
#
# ⚠ Unlike the macOS build, this canNOT use fmemopen: that is a POSIX-2008 stdio extension and
# mingw-w64 has no such function (GCC 14+ turns the implicit declaration into a hard error, which is
# how it surfaced). tmpfile() is plain ISO C, exists everywhere, and returns a handle the OS deletes
# on close -- so the buffer round-trips through the system temp directory and never through anything
# the user owns. The cost is one small write per dictionary load, which happens once per language.
if ! done_stamp hyphen && ! grep -q hnj_hyphen_load_data "$HDIR/hyphen.h"; then
  sed -i 's|\(HyphenDict \*hnj_hyphen_load_file (FILE \*f);\)|\1\nHyphenDict *hnj_hyphen_load_data (const char *fdata, size_t flen);|' "$HDIR/hyphen.h"
  cat >> "$HDIR/hyphen.c" <<'EOF'

/* Mosaic S57: load a dictionary from an in-memory buffer (distro-patch parity). Windows has no
   fmemopen, so the buffer goes through an anonymous tmpfile() the C runtime removes on close. */
HyphenDict *hnj_hyphen_load_data (const char *fdata, size_t flen) {
  FILE *f = tmpfile();
  HyphenDict *result;
  if (f == NULL) return NULL;
  if (flen != 0 && fwrite(fdata, 1, flen, f) != flen) { fclose(f); return NULL; }
  rewind(f);
  result = hnj_hyphen_load_file(f);
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
# On glibc gettext IS libc, so find_package(Intl) succeeds for free and the Linux build never needed
# this. Windows has no libintl: without it MOSAIC_HAVE_GETTEXT is 0 and the app is English-only no
# matter how many catalogs the package ships (S54). Only the `gettext-runtime` sub-package is built
# -- the outer tree also carries xgettext/msgfmt, host developer tools we already have.
t=$(fetch "https://ftp.gnu.org/gnu/gettext/gettext-0.22.5.tar.xz" gettext.tar.xz); unpack "$t"
auto_build "$SRC/gettext-0.22.5/gettext-runtime" gettext-runtime \
  --disable-java --disable-csharp --disable-libasprintf --disable-openmp --without-emacs \
  --disable-c++ --with-included-gettext

# ---- brotli + highway (libjxl's two hard dependencies) ----------------------
t=$(fetch "https://github.com/google/brotli/archive/refs/tags/v1.1.0.tar.gz" brotli.tar.gz); unpack "$t"
cmake_build "$SRC/brotli-1.1.0" brotli -DBROTLI_BUILD_TOOLS=OFF -DBROTLI_DISABLE_TESTS=ON

t=$(fetch "https://github.com/google/highway/archive/refs/tags/1.2.0.tar.gz" highway.tar.gz); unpack "$t"
cmake_build "$SRC/highway-1.2.0" highway \
  -DHWY_ENABLE_TESTS=OFF -DHWY_ENABLE_EXAMPLES=OFF -DHWY_ENABLE_CONTRIB=OFF -DBUILD_TESTING=OFF

# ---- libjxl (JPEG XL) -------------------------------------------------------
# Version-matched to the Linux build (0.11.2). Colour management comes from the lcms2 above rather
# than the bundled skcms, so the payload has ONE CMS.
t=$(fetch "https://github.com/libjxl/libjxl/archive/refs/tags/v0.11.2.tar.gz" libjxl.tar.gz); unpack "$t"
cmake_build "$SRC/libjxl-0.11.2" libjxl \
  -DBUILD_TESTING=OFF \
  -DJPEGXL_ENABLE_TOOLS=OFF -DJPEGXL_ENABLE_BENCHMARK=OFF -DJPEGXL_ENABLE_EXAMPLES=OFF \
  -DJPEGXL_ENABLE_MANPAGES=OFF -DJPEGXL_ENABLE_JNI=OFF -DJPEGXL_ENABLE_SJPEG=OFF \
  -DJPEGXL_ENABLE_OPENEXR=OFF -DJPEGXL_ENABLE_SKCMS=OFF -DJPEGXL_ENABLE_PLUGINS=OFF \
  -DJPEGXL_ENABLE_DEVTOOLS=OFF -DJPEGXL_ENABLE_DOXYGEN=OFF -DJPEGXL_ENABLE_TRANSCODE_JPEG=ON \
  -DJPEGXL_FORCE_SYSTEM_BROTLI=ON -DJPEGXL_FORCE_SYSTEM_HWY=ON -DJPEGXL_FORCE_SYSTEM_LCMS2=ON \
  -DJPEGXL_BUNDLE_LIBPNG=OFF -DJPEGXL_ENABLE_AVX512=OFF

# ---- libwebp (+mux: the ICCP/EXIF chunks live in the extended container) -----
t=$(fetch "https://github.com/webmproject/libwebp/archive/refs/tags/v1.4.0.tar.gz" libwebp.tar.gz); unpack "$t"
cmake_build "$SRC/libwebp-1.4.0" libwebp \
  -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
  -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF \
  -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=ON -DWEBP_BUILD_EXTRAS=OFF \
  -DWEBP_BUILD_LIBWEBPMUX=ON

# ---- libtiff ----------------------------------------------------------------
t=$(fetch "https://download.osgeo.org/libtiff/tiff-4.6.0.tar.gz" tiff.tar.gz); unpack "$t"
cmake_build "$SRC/tiff-4.6.0" libtiff \
  -Dtiff-tools=OFF -Dtiff-tests=OFF -Dtiff-contrib=OFF -Dtiff-docs=OFF \
  -Dlzma=OFF -Dwebp=OFF -Dzstd=ON -Djbig=OFF -Dlerc=OFF

# ---- giflib -----------------------------------------------------------------
# giflib has no CMake and its Makefile's shared-library rules are Linux-only (.so soname flags), so
# the DLL is assembled directly from the six sources that make up the library. There is nothing to
# configure -- it is a pure C library with no dependencies at all.
if ! done_stamp giflib; then
  echo "  [make]  giflib"
  # NB the SourceForge mirror path is /giflib/<file>, NOT /project/giflib/<file> -- the latter 404s.
  t=$(fetch "https://downloads.sourceforge.net/giflib/giflib-5.2.2.tar.gz" giflib.tar.gz); unpack "$t"
  GD="$SRC/giflib-5.2.2"
  ( cd "$GD" && $CC $CFLAGS -shared -o "$PREFIX/bin/libgif-7.dll" \
      -Wl,--out-implib,"$PREFIX/lib/libgif.dll.a" \
      dgif_lib.c egif_lib.c gifalloc.c gif_err.c gif_font.c gif_hash.c openbsd-reallocarray.c \
      -Wl,--export-all-symbols )
  cp "$GD/gif_lib.h" "$PREFIX/include/"
  stamp giflib
fi

# ---- spdlog (CMake; bundles fmt) --------------------------------------------
t=$(fetch "https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.tar.gz" spdlog.tar.gz); unpack "$t"
cmake_build "$SRC/spdlog-1.14.1" spdlog \
  -DSPDLOG_BUILD_SHARED=ON -DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_BUILD_TESTS=OFF -DSPDLOG_INSTALL=ON

# ---- Vulkan headers + loader ------------------------------------------------
# Pinned to the same vulkan-sdk tag as the Linux host and the macOS port: Mosaic calls Vulkan 1.4
# core names (VkPhysicalDeviceHostImageCopyFeatures), so older headers do not compile.
#
# The loader is built for its IMPORT LIBRARY only. vulkan-1.dll is a Windows system component that
# the GPU driver installs into System32; shipping our own beside the exe would shadow the one that
# actually knows about the machine's ICDs. make-package.sh therefore does not copy it.
t=$(fetch "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz" vkheaders.tar.gz); unpack "$t"
cmake_build "$SRC/Vulkan-Headers-vulkan-sdk-1.4.350.1" vulkan-headers

t=$(fetch "https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz" vkloader.tar.gz); unpack "$t"
cmake_build "$SRC/Vulkan-Loader-vulkan-sdk-1.4.350.1" vulkan-loader \
  -DVULKAN_HEADERS_INSTALL_DIR="$PREFIX" -DUPDATE_DEPS=OFF \
  -DENABLE_WERROR=OFF -DUSE_MASM=OFF -DBUILD_TESTS=OFF

# ---- FLTK (CMake; Windows/GDI backend) --------------------------------------
# FLTK bundles its own jpeg/png/zlib by default; point it at ours so the payload has one of each.
t=$(fetch "https://github.com/fltk/fltk/releases/download/release-1.4.5/fltk-1.4.5-source.tar.gz" fltk.tar.gz); unpack "$t"
cmake_build "$SRC/fltk-1.4.5" fltk \
  -DFLTK_BUILD_TEST=OFF -DFLTK_BUILD_EXAMPLES=OFF -DFLTK_BUILD_FLUID=OFF \
  -DFLTK_BUILD_SHARED_LIBS=ON -DFLTK_BUILD_GL=OFF \
  -DFLTK_BACKEND_X11=OFF -DFLTK_BACKEND_WAYLAND=OFF \
  -DFLTK_USE_SYSTEM_LIBJPEG=ON -DFLTK_USE_SYSTEM_LIBPNG=ON -DFLTK_USE_SYSTEM_ZLIB=ON \
  -DFLTK_USE_PANGO=OFF -DFLTK_GRAPHICS_CAIRO=OFF
# ---- libaom + libavif -------------------------------------------------------
# AVIF's encoder choice is a hard constraint set in src/io/avif.cpp, which refuses
# AVIF_CODEC_CHOICE_AUTO and names AOM or SVT explicitly. libaom is what we cross-build, so libaom
# is what the Windows payload can encode with. This is the longest build in the script; skip it with
# MOSAIC_WIN_SKIP="aom libavif" when iterating on something else.
#
# ⚠ libaom's x86 assembly needs a HOST assembler (nasm or yasm) and refuses to configure without
# one. Rather than fail the whole dependency stack over an optional codec, fall back to
# AOM_TARGET_CPU=generic -- which builds a correct but SIMD-less encoder. That distinction is
# user-visible: AV1 encoding is expensive even vectorized, and the generic build makes AVIF export
# slow enough to look broken. So the fallback is announced loudly rather than taken silently, with
# the one-command fix, and re-running after installing an assembler picks it up (delete the stamp).
AOM_ARGS=()
if [ "$MOSAIC_WIN_ARCH" != aarch64 ] && ! have_asm; then
  echo "  ⚠ no nasm/yasm on this host: building libaom WITHOUT SIMD (AVIF export will be slow)."
  echo "    Install one (Arch: pacman -S nasm), then: rm $STAMPS/aom && re-run this script."
  AOM_ARGS+=(-DAOM_TARGET_CPU=generic)
fi
t=$(fetch "https://storage.googleapis.com/aom-releases/libaom-3.9.1.tar.gz" aom.tar.gz); unpack "$t"
cmake_build "$SRC/libaom-3.9.1" aom \
  -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_DOCS=OFF -DENABLE_TOOLS=OFF \
  -DCONFIG_AV1_HIGHBITDEPTH=1 -DBUILD_SHARED_LIBS=ON "${AOM_ARGS[@]}"

t=$(fetch "https://github.com/AOMediaCodec/libavif/archive/refs/tags/v1.1.1.tar.gz" libavif.tar.gz); unpack "$t"
cmake_build "$SRC/libavif-1.1.1" libavif \
  -DAVIF_CODEC_AOM=SYSTEM -DAVIF_BUILD_APPS=OFF -DAVIF_BUILD_TESTS=OFF \
  -DAVIF_LIBYUV=OFF -DAVIF_JPEG=OFF -DAVIF_ZLIBPNG=OFF


echo "== done: deps for $MOSAIC_WIN_ARCH in $PREFIX =="
