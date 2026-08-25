#!/usr/bin/env bash
# Assemble the Windows payload for one architecture and emit BOTH shipping artifacts (PLAN.md S57):
#
#   Mosaic-<version>-windows-<arch>.zip   portable -- unzip anywhere and run, no installer, no
#                                         registry, nothing written outside the folder
#   Mosaic-<version>-windows-<arch>.msi   per-USER install: Start-menu shortcut, Add/Remove
#                                         Programs entry, .mosaic file association
#
# Both ship the SAME staging tree, so there is one layout to reason about rather than two, and the
# portable copy cannot depend on anything the installer would have written.
#
# The payload is UNSIGNED, by decision. Unlike macOS -- where the Apple Silicon kernel refuses to
# execute an unsigned arm64 binary at all, so the .app is ad-hoc signed with rcodesign -- Windows has
# no signature requirement to RUN anything: an unsigned .exe and an unsigned .msi both work on x86_64
# and on arm64. What is missing is an Authenticode signature, so a freshly DOWNLOADED copy gets
# SmartScreen's "Windows protected your PC" once (More info -> Run anyway). See
# packaging/windows/README.md.
#
# Required env:
#   MOSAIC_WIN_PREFIX  the dependency prefix for THIS arch (same variable build-deps.sh takes)
# Optional env:
#   MOSAIC_WIN_OUT         output directory        (default: <repo>/build/windows-package)
#   MOSAIC_WIN_HYPHEN_DIR  hyph_*.dic source dir   (default: /usr/share/hyphen, then $PREFIX/share/hyphen)
#   MOSAIC_WIN_PYTHON      a python3 with Pillow   (default: python3)
#   MOSAIC_WIN_OBJDUMP     objdump for reading PE imports (default: derived from the arch)
#   MOSAIC_LLVM_MINGW      llvm-mingw root for aarch64 (default /opt/llvm-mingw)
#   MOSAIC_SKIP_BUILD=1    reuse the existing build/windows-<arch> tree
#   JOBS                   parallel build jobs
#
# Args: <x86_64|aarch64>   (arm64 accepted as an alias)
set -euo pipefail

# ⚠ No apostrophe in that message -- see the same line in build-app.sh for why it is a parse error.
: "${MOSAIC_WIN_PREFIX:?set MOSAIC_WIN_PREFIX to the dependency prefix for this arch (see build-deps.sh)}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="${MOSAIC_WIN_OUT:-$REPO/build/windows-package}"
PREFIX="$MOSAIC_WIN_PREFIX"
PY="${MOSAIC_WIN_PYTHON:-python3}"

# Three names for two CPUs; see build-app.sh for why. ARCH is the toolchain/deps spelling, PRESET the
# CMake one, LABEL the one Windows users recognise and the one that goes in the artifact name.
case "${1:-}" in
    x86_64|amd64|x64)
        ARCH=x86_64; PRESET=windows-x86_64; LABEL=x86_64; TRIPLE=x86_64-w64-mingw32 ;;
    aarch64|arm64)
        ARCH=aarch64; PRESET=windows-arm64; LABEL=arm64;  TRIPLE=aarch64-w64-mingw32 ;;
    *) echo "usage: make-package.sh <x86_64|aarch64>" >&2; exit 1 ;;
esac
LLVMROOT="${MOSAIC_LLVM_MINGW:-/opt/llvm-mingw}"

# ---- payload layout ----------------------------------------------------------
# ⚠ These two lines are the CONTRACT with the code that reads the payload at run time, and they must
# agree with it exactly or the app ships data it will never find:
#   DATASUB   common::installedDataDir()          (src/common/settings.cpp, _WIN32 branch)
#             -> brushes/, presets/, icc-profiles/, hyphen/, locale/
#   FONTCONF  $FONTCONFIG_PATH                    (src/app/main.cpp, pointFontconfigAtPayload)
# The exe and every DLL sit at the ROOT of the tree. That is not tidiness lost: Windows' loader
# search order starts with the directory of the executable image, and a bin/ level would mean either
# a manifest-based redirection or a PATH edit for no gain.
DATASUB="data"
FONTCONF="etc/fonts"

# ---- tooling -----------------------------------------------------------------
# objdump reads the PE import tables (the DLL closure below). Per arch on purpose: a binutils
# objdump only knows the targets it was configured for, so the x86_64 one cannot necessarily parse an
# ARM64 PE, while llvm-objdump parses every target it was built with. Both print "DLL Name:".
if [ -n "${MOSAIC_WIN_OBJDUMP:-}" ]; then
    OBJDUMP="$MOSAIC_WIN_OBJDUMP"
elif [ "$ARCH" = aarch64 ]; then
    OBJDUMP="$LLVMROOT/bin/llvm-objdump"
else
    OBJDUMP="$(command -v "$TRIPLE-objdump" || command -v llvm-objdump || true)"
fi
[ -x "$OBJDUMP" ] || { echo "no objdump for $ARCH (tried '$OBJDUMP'); set MOSAIC_WIN_OBJDUMP" >&2; exit 1; }

# wixl (msitools) builds the .msi. Not full WiX -- a useful subset of WiX v3 syntax -- see mosaic.wxs
# and the README for which constructs are deliberately avoided. wixl-heat generates the file/component
# fragment; hand-writing ~250 <File> rows is not a thing a person does twice.
#
# Checked UP FRONT rather than at the MSI step. This script emits both artifacts, and finding out that
# half of them are impossible after compiling the app and staging 250 files is not a better experience
# than being told now.
WIXL="${MOSAIC_WIXL:-$(command -v wixl || true)}"
WIXLHEAT="${MOSAIC_WIXL_HEAT:-$(command -v wixl-heat || true)}"
if [ ! -x "$WIXL" ] || [ ! -x "$WIXLHEAT" ]; then
    echo "wixl / wixl-heat not found -- the .msi cannot be built." >&2
    echo "  Arch/CachyOS: sudo pacman -S msitools     Debian: sudo apt install msitools" >&2
    echo "  (see packaging/windows/README.md)" >&2
    exit 1
fi

# ⚠ The `-dirty` suffix is part of the identity, not decoration. CMakeLists.txt appends it to
# MOSAIC_GIT_REV for an uncommitted tree, so the EXE already reports it -- but this function used to
# drop it, which meant two different uncommitted builds on the same base commit produced two packages
# with the SAME NAME, the second silently overwriting the first. That cost a real round trip: a tester
# ran a build believing it contained a fix that had been written an hour later, and the screenshots
# were read as evidence against code that was never in the binary. Same derivation as CMake's, so the
# archive name and `mosaic.exe --version` always agree.
version() {
    # A RELEASE build passes MOSAIC_VERSION and gets exactly that, with no +g<rev>: the artifact
    # name becomes part of a download URL (where "+" is at best %2B), the release page has to agree
    # with it, and on macOS CFBundleShortVersionString must be numeric anyway. The git rev stays the
    # default for every dev build, where the commit IS the identifier (PLAN item 9).
    if [ -n "${MOSAIC_VERSION:-}" ]; then echo "$MOSAIC_VERSION"; return; fi
    local v rev
    v=$(grep -E "^\s*VERSION [0-9]" "$REPO/CMakeLists.txt" | head -1 | grep -oE "[0-9]+\.[0-9]+\.[0-9]+")
    rev=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || true)
    [ -z "$rev" ] && { echo "$v"; return; }
    git -C "$REPO" diff --quiet HEAD -- 2>/dev/null || rev="${rev}-dirty"
    echo "${v}+g${rev}"
}
VERSION="$(version)"
# The FULL identifier, always with +g<rev>, whatever MOSAIC_VERSION says. The artifact NAME wants
# the clean release number, but the MSI's Comments field is the one place the commit can live (its
# ProductVersion is numeric-only), so stripping the rev from both left a release .msi that could
# not say which build it was.
FULLVERSION="$(MOSAIC_VERSION= version)"
# The MSI's ProductVersion field is numeric-only (major.minor.build, compared field by field), so the
# git rev cannot live there. It goes in the package Comments instead, and in the exe's VERSIONINFO
# strings (packaging/windows/mosaic.rc.in). See mosaic.wxs for what that costs at upgrade time.
NUMVERSION="${VERSION%%+*}"
NAME="Mosaic-$VERSION-windows-$LABEL"

# ---- 1. build ----------------------------------------------------------------
if [ -z "${MOSAIC_SKIP_BUILD:-}" ]; then
    MOSAIC_WIN_PREFIX="$PREFIX" bash "$HERE/build-app.sh" "$ARCH"
fi
EXE="$REPO/build/$PRESET/bin/mosaic.exe"
[ -f "$EXE" ] || { echo "missing $EXE -- build first (unset MOSAIC_SKIP_BUILD)" >&2; exit 1; }

echo "== staging $NAME =="
STAGE="$OUT/$NAME"
rm -rf "$STAGE"
mkdir -p "$STAGE/$DATASUB" "$STAGE/$FONTCONF"
cp "$EXE" "$STAGE/mosaic.exe"

# MosaicThumbnail.dll: the in-process COM server that draws a .mosaic file's own picture in Explorer
# (src/thumbnailer/shell_thumbnail_win32.cpp), the Windows counterpart of the freedesktop thumbnailer
# and the macOS Quick Look extensions. It goes BESIDE mosaic.exe because that is the only directory
# its own dependent DLLs will be found in when the shell loads it -- an in-process server inherits
# Explorer's search path, not ours. mosaic.wxs registers it; the MSI is the registration path.
THUMBDLL="$REPO/build/$PRESET/bin/MosaicThumbnail.dll"
if [ -f "$THUMBDLL" ]; then
    cp "$THUMBDLL" "$STAGE/MosaicThumbnail.dll"
else
    echo "WARNING: no MosaicThumbnail.dll -- .mosaic files will show a generic icon in Explorer," >&2
    echo "         and the MSI's handler registration will point at a file that is not there." >&2
fi

# ---- 2. the DLL closure ------------------------------------------------------
# DERIVED, never hard-coded. A fixed list of "the ~22 DLLs Mosaic needs" is wrong the first time a
# dependency gains an import -- and the failure mode is the worst kind: the program builds, packages,
# zips and then dies on the user's machine with a modal "libfoo-1.dll was not found" before a single
# line of ours runs. So: read mosaic.exe's import table, stage what it names, and repeat over
# everything staged until nothing new appears. A DLL has its own imports; the closure is transitive.
#
# This also means the payload contains exactly what is reachable and nothing else. $PREFIX/bin holds
# a dozen command-line tools the dependencies install (fc-cache.exe, cjpeg.exe, webpmux.exe) and
# libraries nothing here links (libwebpdecoder.dll, libharfbuzz-subset), and `cp $PREFIX/bin/*.dll`
# would ship all of it.

# Directories a DLL name may resolve to, most specific first. The MinGW RUNTIME DLLs are not in the
# dependency prefix -- they come with the compiler -- and their names differ per toolchain
# (libgcc_s_seh-1/libstdc++-6/libwinpthread-1 for GNU; libc++/libunwind/libwinpthread-1 for
# llvm-mingw), which is exactly why they are RESOLVED rather than listed.
SEARCH=("$PREFIX/bin" "$PREFIX/lib")
if [ "$ARCH" = aarch64 ]; then
    SEARCH+=("$LLVMROOT/$TRIPLE/bin")
else
    # /usr/<triple>/bin on a distro-packaged toolchain; and wherever libgcc actually lives, which on
    # Debian is a versioned /usr/lib/gcc/<triple>/<ver>/ directory instead.
    _gcc="$(command -v "$TRIPLE-gcc" || true)"
    [ -n "$_gcc" ] && SEARCH+=("$(dirname "$_gcc")/../$TRIPLE/bin")
    _libgcc="$("$TRIPLE-gcc" -print-libgcc-file-name 2>/dev/null || true)"
    [ -n "$_libgcc" ] && SEARCH+=("$(dirname "$_libgcc")")
fi

# DLLs that belong to WINDOWS. Shipping any of these is a BUG, not an optimisation: a private copy
# beside the exe wins the loader's search and shadows the machine's own, which is how a program ends
# up running against a comctl32 or a UCRT that does not match the OS it is on.
#
# ⚠ vulkan-1.dll is the one that looks like a mistake and is not. It is a system component installed
# by the GPU DRIVER, and it is the piece that knows where that machine's ICDs are registered. A copy
# from our build would enumerate no devices at all. build-deps.sh cross-builds the loader for its
# IMPORT LIBRARY only, and this is the other half of that decision.
SYSTEM_DLLS=(
    # Core Win32
    kernel32.dll kernelbase.dll ntdll.dll user32.dll gdi32.dll gdi32full.dll advapi32.dll
    sechost.dll rpcrt4.dll combase.dll ole32.dll oleaut32.dll shell32.dll shlwapi.dll shcore.dll
    comctl32.dll comdlg32.dll version.dll setupapi.dll cfgmgr32.dll userenv.dll psapi.dll
    imm32.dll winmm.dll uxtheme.dll dwmapi.dll oleacc.dll propsys.dll msimg32.dll
    winspool.drv wtsapi32.dll powrprof.dll avrt.dll mscms.dll usp10.dll normaliz.dll
    # C runtime. UCRT and its API sets live in the OS on every Windows we support (10 1809 floor).
    msvcrt.dll ucrtbase.dll
    # Crypto / networking, pulled in by TLS-capable and hashing dependencies
    bcrypt.dll bcryptprimitives.dll ncrypt.dll crypt32.dll wintrust.dll secur32.dll
    ws2_32.dll wsock32.dll mswsock.dll iphlpapi.dll dnsapi.dll netapi32.dll mpr.dll
    # Graphics stacks the OS owns
    opengl32.dll glu32.dll d3d9.dll d3d11.dll d3d12.dll dxgi.dll dcomp.dll
    dwrite.dll d2d1.dll windowscodecs.dll
    # GDI+ -- FLTK's Windows driver uses it to scale images (Fl_GDI_Graphics_Driver's
    # draw_scaled_image path). It has shipped in System32 since XP SP1, so it is as much an OS
    # component as gdi32 itself; the redistributable that used to exist was for Windows 2000/98.
    # Reported as an unresolved import on the first real packaging run, which is exactly what this
    # list is for: an OS DLL missing from it is a false alarm, and shipping a copy would be the bug.
    gdiplus.dll
    # ⚠ see the note above -- installed by the GPU driver, never by us
    vulkan-1.dll
)
# api-ms-win-* / ext-ms-win-* are API SETS: not real files at all, just names the loader redirects to
# whichever OS DLL implements them on this build of Windows. There is nothing to copy.
is_system_dll() {
    local n="$1"
    case "$n" in api-ms-win-*|ext-ms-win-*) return 0 ;; esac
    local s
    for s in "${SYSTEM_DLLS[@]}"; do [ "$n" = "$s" ] && return 0; done
    return 1
}

# The import names a PE file lists. Case-SENSITIVE "DLL Name:" on purpose: llvm-objdump also prints
# " DLL name:" (lower-case n) for the file's own EXPORT table, and matching that would make every
# library import itself.
imports_of() { "$OBJDUMP" -p "$1" 2>/dev/null | sed -n 's/^[[:space:]]*DLL Name:[[:space:]]*//p'; }

# PE import names carry whatever case the linker felt like ("KERNEL32.dll", "libpng16.dll"), and
# nothing on Windows cares -- so resolution is case-insensitive, which on a case-sensitive Linux
# filesystem has to be asked for explicitly.
resolve_dll() {
    local n="$1" d hit
    for d in "${SEARCH[@]}"; do
        [ -d "$d" ] || continue
        hit=$(find "$d" -maxdepth 1 -iname "$n" -type f 2>/dev/null | head -1)
        [ -n "$hit" ] && { echo "$hit"; return 0; }
    done
    return 1
}

declare -A DONE=()          # lower-cased DLL name -> "" (handled); keeps the walk finite
UNRESOLVED=()
# Seeded with EVERY image we ship, not just the exe. MosaicThumbnail.dll links the same static Mosaic
# modules but is a separate PE with its own import table, and the shell loads it while Mosaic is not
# running -- so anything only it needs still has to be in the payload.
QUEUE=("$STAGE/mosaic.exe")
[ -f "$STAGE/MosaicThumbnail.dll" ] && QUEUE+=("$STAGE/MosaicThumbnail.dll")
qi=0
while [ "$qi" -lt "${#QUEUE[@]}" ]; do
    cur="${QUEUE[$qi]}"; qi=$((qi + 1))
    while read -r dep; do
        [ -n "$dep" ] || continue
        key="${dep,,}"
        [ -n "${DONE[$key]+x}" ] && continue
        DONE[$key]=""
        # System check FIRST, so a name that happens to also exist in the dependency prefix still
        # cannot be shipped.
        is_system_dll "$key" && continue
        if src=$(resolve_dll "$dep"); then
            cp -f "$src" "$STAGE/$(basename "$src")"
            QUEUE+=("$STAGE/$(basename "$src")")
        else
            UNRESOLVED+=("$dep")
        fi
    done < <(imports_of "$cur")
done
_ndll=$(find "$STAGE" -maxdepth 1 -iname '*.dll' | wc -l)
echo "  staged $_ndll DLL(s) by transitive closure"
if [ "${#UNRESOLVED[@]}" -gt 0 ]; then
    # Neither resolvable nor known-system. Either a genuinely missing dependency (the package is
    # broken and will fail to start) or a Windows DLL absent from SYSTEM_DLLS above (harmless, and
    # the list wants the name added). Deliberately not fatal: a false alarm must not destroy an
    # otherwise good package, and the names are right here to check.
    echo "  ⚠ UNRESOLVED imports -- check each one before shipping:" >&2
    printf '      %s\n' "${UNRESOLVED[@]}" >&2
fi

# ---- 3. fontconfig's configuration tree -------------------------------------
# Mosaic uses fontconfig on Windows as it does everywhere (platform/font_db.cpp), so the payload
# carries fontconfig's RULES and not just its DLL. main.cpp points $FONTCONFIG_PATH here at start-up;
# without it libfontconfig-1.dll looks for the Linux cross-build prefix compiled into it, finds
# nothing, and falls back to a built-in configuration that scans C:\Windows\Fonts but has none of the
# conf.d alias rules -- so a request for "sans-serif" resolves to nothing in particular.
#
# ⚠ -L, not -R. fontconfig's etc/fonts/conf.d is 30-odd ABSOLUTE SYMLINKS into
# $PREFIX/share/fontconfig/conf.avail. A plain copy preserves them, and they point at a Linux path
# that does not exist on the target -- so the tree would look complete and load zero rules. (And a
# zip cannot carry a symlink to Windows in the first place.) fonts.conf itself needs no rewriting: it
# reaches the font dir and the cache through fontconfig's own WINDOWSFONTDIR /
# LOCAL_APPDATA_FONTCONFIG_CACHE tokens, and includes conf.d by a RELATIVE path.
if [ -f "$PREFIX/etc/fonts/fonts.conf" ]; then
    cp -L "$PREFIX/etc/fonts/fonts.conf" "$STAGE/$FONTCONF/fonts.conf"
    if [ -d "$PREFIX/etc/fonts/conf.d" ]; then
        mkdir -p "$STAGE/$FONTCONF/conf.d"
        cp -RL "$PREFIX/etc/fonts/conf.d/." "$STAGE/$FONTCONF/conf.d/"
    fi
    echo "  bundled fontconfig config ($(find "$STAGE/$FONTCONF" -name '*.conf' | wc -l) file(s))"
else
    echo "WARNING: no $PREFIX/etc/fonts/fonts.conf -- generic font families will not resolve" >&2
fi
# No font CACHE is shipped. It is keyed to the machine's own font set and gets built on first run
# into %LOCALAPPDATA% (build-deps.sh configured --with-cache-dir=LOCAL_APPDATA_FONTCONFIG_CACHE).

# ---- 4. runtime data --------------------------------------------------------
# The CC-0 brush set, the New-Document templates, and the vendored default CMYK press profile --
# the same three things the macOS bundle carries, read through the same installedDataDir().
mkdir -p "$STAGE/$DATASUB/brushes" "$STAGE/$DATASUB/presets" "$STAGE/$DATASUB/icc-profiles"
cp -R "$REPO/data/brushes/." "$STAGE/$DATASUB/brushes/" 2>/dev/null || true
cp -R "$REPO/data/presets/." "$STAGE/$DATASUB/presets/" 2>/dev/null || true
cp "$REPO/third_party/icc-profiles/"*.icc "$STAGE/$DATASUB/icc-profiles/" 2>/dev/null || true

# Hyphenation dictionaries. Windows has no system hyphenator (macOS answers with CoreFoundation, so
# it bundles none) and no /usr/share/hyphen to read, so the .dic files ship in the payload;
# core/text/hyphenator.cpp looks in installedDataDir()/"hyphen" first for exactly this reason.
# Sources are UNIONED and never overwritten: libhyphen installs its own en_US into the prefix, and
# the build host's dictionary package -- if it has one -- adds the rest.
mkdir -p "$STAGE/$DATASUB/hyphen"
for hd in "${MOSAIC_WIN_HYPHEN_DIR:-}" /usr/share/hyphen "$PREFIX/share/hyphen"; do
    [ -n "$hd" ] && [ -d "$hd" ] || continue
    cp -n "$hd"/hyph_*.dic "$STAGE/$DATASUB/hyphen/" 2>/dev/null || true
done
_ndic=$(find "$STAGE/$DATASUB/hyphen" -name 'hyph_*.dic' | wc -l)
if [ "$_ndic" -eq 0 ]; then
    echo "WARNING: no hyph_*.dic found -- justified text will not hyphenate. Install the host's" >&2
    echo "         hyphenation dictionaries (Arch: hyphen-en hyphen-de ...) or set" >&2
    echo "         MOSAIC_WIN_HYPHEN_DIR." >&2
else
    echo "  bundled $_ndic hyphenation dictionary/ies"
fi

# Translation catalogs. Compiled straight from po/ rather than lifted out of the build tree: .mo
# files are architecture-independent, so both arches' packages get byte-identical ones, and this
# script does not have to know whether the CMake side happened to build them.
if command -v msgfmt >/dev/null 2>&1 && [ -f "$REPO/po/LINGUAS" ]; then
    _catalogs=0
    while read -r lang; do
        case "$lang" in ''|'#'*) continue ;; esac
        for domain in mosaic motivate; do
            po="$REPO/po/$lang/$domain.po"
            [ -f "$po" ] || continue
            mkdir -p "$STAGE/$DATASUB/locale/$lang/LC_MESSAGES"
            msgfmt --check -o "$STAGE/$DATASUB/locale/$lang/LC_MESSAGES/$domain.mo" "$po"
            _catalogs=$((_catalogs + 1))
        done
    done < "$REPO/po/LINGUAS"
    echo "  bundled $_catalogs translation catalog(s)"
else
    echo "msgfmt not found (or no po/LINGUAS): the package will be English-only" >&2
fi

# GPLv3, as .txt because that is the extension Windows knows how to open. Kept LF-only: Notepad has
# read LF files correctly since Windows 10 1809, which is this build's floor anyway.
cp "$REPO/LICENSE" "$STAGE/LICENSE.txt"

# ---- 5. icons ---------------------------------------------------------------
# Two .ico files, from the two SVGs the Linux desktop integration installs (see the top-level
# CMakeLists install() rules): the application icon, and the document-flavoured variant that Linux
# registers as image-x-mosaic and macOS ships for the org.mosaic.document UTI.
#
#   mosaic.ico      is compiled INTO mosaic.exe (src/app/CMakeLists.txt -> mosaic.rc), so it is not
#                   copied here; it is built again into $OUT only because the MSI needs a file on
#                   disk for its Add/Remove-Programs icon.
#   mosaic-doc.ico  ships as a payload FILE, because the .mosaic association's DefaultIcon has to
#                   name something. Pointing it at "mosaic.exe",1 instead would depend on the shell's
#                   icon-INDEX ordering over the exe's icon resources -- an ordering we would be
#                   guessing at from Linux, to save 50 KB.
"$PY" "$HERE/make-ico.py" "$REPO/assets/app_icon.svg" "$OUT/mosaic.ico"
"$PY" "$HERE/make-ico.py" "$REPO/assets/mimetype_icon.svg" "$STAGE/mosaic-doc.ico"

# Nothing is STRIPPED. mosaic.exe and the dependency DLLs keep their symbol tables, which costs
# download size and buys the one thing an alpha needs most: a crash report from a user that can be
# turned back into function names.

# ---- 6. the portable zip -----------------------------------------------------
# Everything is under a single top-level folder named after the artifact, so extracting into
# Downloads does not scatter 25 DLLs across it.
echo "== $NAME.zip =="
rm -f "$OUT/$NAME.zip"
if command -v zip >/dev/null 2>&1; then
    ( cd "$OUT" && zip -q -r -9 "$NAME.zip" "$NAME" )
else
    # No zip(1) on the host. Python's zipfile module is not a fallback so much as the same thing
    # spelled differently -- it deflates, and it takes the directory name as the member prefix, which
    # is the property that matters here.
    ( cd "$OUT" && "$PY" -m zipfile -c "$NAME.zip" "$NAME" )
fi

# ---- 7. the MSI --------------------------------------------------------------
# wixl-heat turns the staging tree into the Directory/Component/File fragment; mosaic.wxs supplies
# the product, the feature, the shortcut and the file association. Splitting it that way is not
# stylistic: the payload is ~250 files, and its component IDs and GUIDs must be STABLE across builds
# or every upgrade leaves orphans behind. wixl-heat derives both from the file's relative path (an
# MD5, then Guid="*" -> a name-based UUID from the component's key path), so the same tree always
# produces the same identifiers.
echo "== $NAME.msi =="
#
# `sort` is not cosmetic either: `find` walks in directory order, so without it the generated
# fragment's element ORDER would change with the filesystem even when the tree does not, and two
# packages of the same payload would stop being comparable.
FILESWXS="$OUT/$NAME-files.wxs"
( cd "$STAGE" && find . -type f | sort ) \
    | "$WIXLHEAT" --directory-ref=INSTALLDIR --component-group=CG_MosaicPayload \
                  --var=var.MosaicStage --prefix=./ --win64 > "$FILESWXS"

# --arch: wixl writes the MSI summary-information "Template", i.e. which processors the package
# declares itself installable on, and it accepts only x86/intel/intel64/ia64/x64 -- there is no arm64
# in msitools 0.106. x64 is therefore what BOTH arches get built with, and the arm64 package is fixed
# up afterwards (below).
#
# Win64 is a variable rather than a literal because that is how wixl-heat --win64 emits it: every
# generated component says Win64="$(var.Win64)" and the value has to arrive from the command line.
"$WIXL" --arch x64 -o "$OUT/$NAME.msi" \
        -D Win64=yes \
        -D MosaicStage="$STAGE" \
        -D MosaicVersion="$NUMVERSION" \
        -D MosaicBuild="$FULLVERSION" \
        -D MosaicIcon="$OUT/mosaic.ico" \
        "$HERE/mosaic.wxs" "$FILESWXS"

if [ "$ARCH" = aarch64 ]; then
    # Correct the summary-information Template to Arm64 after the fact, since wixl cannot write it.
    # msibuild -s takes all four fields at once (name, author, template, package code), so all four
    # are given; the package code must be a fresh GUID per .msi FILE, which is what uuidgen is for.
    #
    # ⚠ UNVERIFIED, and non-fatal on purpose. Without it the package still installs -- Windows does
    # not check that the declared template matches the machine code inside, and ARM64 Windows accepts
    # x64 packages -- so a failure here costs correctness of a metadata field, not the artifact.
    if command -v msibuild >/dev/null 2>&1 && command -v uuidgen >/dev/null 2>&1; then
        pkgcode="{$(uuidgen | tr 'a-f' 'A-F')}"
        if msibuild "$OUT/$NAME.msi" -s "Mosaic $VERSION" "The Mosaic project" "Arm64;1033" \
                                     "$pkgcode" 2>/dev/null; then
            echo "  summary-info Template set to Arm64;1033"
        else
            echo "  note: could not retag the package as Arm64 (it stays x64, which installs)" >&2
        fi
    fi
fi

echo "== done =="
ls -la "$OUT/$NAME.zip" "$OUT/$NAME.msi"
