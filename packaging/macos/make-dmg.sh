#!/usr/bin/env bash
# Build the Mosaic macOS .dmg end to end (PLAN.md S58/S59): compile both arches, lipo into a
# universal Mosaic.app (bundling the Vulkan loader + MoltenVK ICD), then assemble a
# drag-to-Applications HFS+ disk image with the angled-icon background -- entirely on Linux, no Mac.
#
# The app is UNSIGNED by design: users approve it once past Gatekeeper (right-click -> Open, or
# System Settings -> Privacy & Security -> Open Anyway). No notarization.
#
# Required env:
#   OSXCROSS_ROOT           osxcross 'target' dir
#   MOSAIC_MAC_DEPS_ROOT    root of per-arch dep prefixes (<root>/arm64, <root>/x86_64)
#   MOSAIC_MAC_VULKAN_STAGE dir with the universal libMoltenVK.dylib + MoltenVK_icd.json
# Tooling (from these env vars, else looked up on PATH):
#   MOSAIC_HFS_NEWFS   newfs_hfs   (create HFS+)    -- hfsprogs / diskdev_cmds
#   MOSAIC_HFS_HFSPLUS hfsplus     (inject, no mount) -- planetbeing libdmg-hfsplus
#   MOSAIC_HFS_DMG     dmg         (compress .dmg)  -- fanquake libdmg-hfsplus
#   MOSAIC_DSSTORE_PYTHON  a python with `ds_store` + `mac_alias` installed (else: python3)
# Optional: MOSAIC_MAC_OUT (default: <repo>/build/macos-dmg), JOBS, MOSAIC_SKIP_BUILD=1
set -euo pipefail

: "${OSXCROSS_ROOT:?set OSXCROSS_ROOT}"
: "${MOSAIC_MAC_DEPS_ROOT:?set MOSAIC_MAC_DEPS_ROOT}"
: "${MOSAIC_MAC_VULKAN_STAGE:?set MOSAIC_MAC_VULKAN_STAGE}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="${MOSAIC_MAC_OUT:-$REPO/build/macos-dmg}"
DSPY="${MOSAIC_DSSTORE_PYTHON:-python3}"
VOL="Mosaic"

NEWFS="${MOSAIC_HFS_NEWFS:-$(command -v newfs_hfs || command -v mkfs.hfsplus || true)}"
HFSPLUS="${MOSAIC_HFS_HFSPLUS:-$(command -v hfsplus || true)}"
DMGTOOL="${MOSAIC_HFS_DMG:-$(command -v dmg || true)}"
for t in NEWFS HFSPLUS DMGTOOL; do
    [ -x "${!t}" ] || { echo "missing HFS+ tool ($t); see packaging/macos/README.md" >&2; exit 1; }
done
# rcodesign ad-hoc-signs the bundle. This is NOT optional on Apple Silicon: the kernel refuses to
# execute an unsigned arm64 binary ("invalid application"), independent of Gatekeeper. Ad-hoc
# signing needs no Apple account.
RCODESIGN="${MOSAIC_CODESIGN:-$(command -v rcodesign || true)}"

# osxcross Mach-O tools (the darwinNN triple is discovered, not hard-coded).
CC0=$(ls "$OSXCROSS_ROOT"/bin/x86_64-apple-darwin*-clang | head -1)
DARWIN=$(basename "$CC0" | sed -E 's/^x86_64-(apple-darwin[0-9.]+)-clang$/\1/')
LIPO="$OSXCROSS_ROOT/bin/x86_64-$DARWIN-lipo"
INT="$OSXCROSS_ROOT/bin/x86_64-$DARWIN-install_name_tool"
OTOOL="$OSXCROSS_ROOT/bin/x86_64-$DARWIN-otool"

version() {
    local v rev
    v=$(grep -E "^\s*VERSION [0-9]" "$REPO/CMakeLists.txt" | head -1 | grep -oE "[0-9]+\.[0-9]+\.[0-9]+")
    rev=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || true)
    [ -n "$rev" ] && echo "${v}+g${rev}" || echo "$v"
}
VERSION="$(version)"

# ---- 1. build both arches -----------------------------------------------------
if [ -z "${MOSAIC_SKIP_BUILD:-}" ]; then
    for arch in arm64 x86_64; do
        MOSAIC_MAC_DEPS_ROOT="$MOSAIC_MAC_DEPS_ROOT" OSXCROSS_ROOT="$OSXCROSS_ROOT" \
            bash "$HERE/build-app.sh" "$arch"
    done
fi
for arch in arm64 x86_64; do
    [ -x "$REPO/build/macos-$arch/bin/mosaic" ] || {
        echo "missing build/macos-$arch/bin/mosaic -- build first (unset MOSAIC_SKIP_BUILD)" >&2; exit 1; }
done

# ---- 2. universal Mosaic.app --------------------------------------------------
echo "== assembling Mosaic.app ($VERSION) =="
APP="$OUT/Mosaic.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" \
         "$APP/Contents/Resources/vulkan/icd.d"

# Fix each single-arch binary's rpaths BEFORE lipo: CMake baked in the per-arch dependency prefix
# (.../deps/<arch>/lib) as an rpath, which differs per arch -- and install_name_tool -delete_rpath
# is atomic across a fat binary's arches, so it must be done while each slice is still on its own.
for arch in arm64 x86_64; do
    cp "$REPO/build/macos-$arch/bin/mosaic" "$OUT/mosaic.$arch"
    for rp in $("$OTOOL" -l "$OUT/mosaic.$arch" \
                | awk '/LC_RPATH/{r=1} r&&/ path /{print $2; r=0}' | sort -u); do
        case "$rp" in
            @executable_path/*|@loader_path/*) ;;
            *) "$INT" -delete_rpath "$rp" "$OUT/mosaic.$arch" 2>/dev/null || true ;;
        esac
    done
done
"$LIPO" -create "$OUT/mosaic.arm64" "$OUT/mosaic.x86_64" -output "$APP/Contents/MacOS/mosaic"
rm -f "$OUT/mosaic.arm64" "$OUT/mosaic.x86_64"
# Each arch ships libvulkan.1.dylib -> libvulkan.1.<ver>.dylib; lipo follows the symlinks so the two
# single-arch loaders fuse into one universal dylib (globbing would also match the symlink -> a
# duplicate-arch error).
"$LIPO" -create "$MOSAIC_MAC_DEPS_ROOT/arm64/lib/libvulkan.1.dylib" \
                "$MOSAIC_MAC_DEPS_ROOT/x86_64/lib/libvulkan.1.dylib" \
    -output "$APP/Contents/Frameworks/libvulkan.1.dylib"
cp "$MOSAIC_MAC_VULKAN_STAGE/libMoltenVK.dylib" "$APP/Contents/Resources/vulkan/icd.d/"
cp "$MOSAIC_MAC_VULKAN_STAGE/MoltenVK_icd.json" "$APP/Contents/Resources/vulkan/icd.d/"

sed "s/@MOSAIC_VERSION@/$VERSION/g" "$HERE/Info.plist.in" > "$APP/Contents/Info.plist"

# ---- 2a. the Quick Look thumbnail extension (S58-e) ---------------------------
# .mosaic files show their own picture in Finder / the Open panel / Spotlight. macOS loads this
# out-of-process from Contents/PlugIns once LaunchServices has seen the installed app; the .appex
# is an ordinary bundle whose Mach-O hands control to Foundation's NSExtensionMain (see
# src/thumbnailer/quicklook_macos.mm). Nothing to fix up beyond the per-arch rpaths -- it links the
# static libraries and system frameworks only, no Vulkan.
# TWO bundles from ONE binary: NSExtensionPointIdentifier holds a single value, so thumbnails and
# space-bar previews are separate extension points needing separate .appex bundles. They embed the
# same executable and differ only in which principal class their plist names.
QLAPP="$APP/Contents/PlugIns/MosaicQuickLook.appex"
QLPREVIEW="$APP/Contents/PlugIns/MosaicQuickLookPreview.appex"
if [ -x "$REPO/build/macos-arm64/bin/mosaic-quicklook" ] &&
   [ -x "$REPO/build/macos-x86_64/bin/mosaic-quicklook" ]; then
    for arch in arm64 x86_64; do
        cp "$REPO/build/macos-$arch/bin/mosaic-quicklook" "$OUT/ql.$arch"
        for rp in $("$OTOOL" -l "$OUT/ql.$arch" \
                    | awk '/LC_RPATH/{r=1} r&&/ path /{print $2; r=0}' | sort -u); do
            case "$rp" in
                @executable_path/*|@loader_path/*) ;;
                *) "$INT" -delete_rpath "$rp" "$OUT/ql.$arch" 2>/dev/null || true ;;
            esac
        done
    done
    "$LIPO" -create "$OUT/ql.arm64" "$OUT/ql.x86_64" -output "$OUT/ql.universal"
    rm -f "$OUT/ql.arm64" "$OUT/ql.x86_64"
    # bundle dir : executable name : plist template
    for spec in "$QLAPP:MosaicQuickLook:QuickLook-Info.plist.in" \
                "$QLPREVIEW:MosaicQuickLookPreview:QuickLookPreview-Info.plist.in"; do
        dir="${spec%%:*}"; rest="${spec#*:}"; exe="${rest%%:*}"; plist="${rest#*:}"
        mkdir -p "$dir/Contents/MacOS"
        cp "$OUT/ql.universal" "$dir/Contents/MacOS/$exe"
        sed "s/@MOSAIC_VERSION@/$VERSION/g" "$HERE/$plist" > "$dir/Contents/Info.plist"
    done
    rm -f "$OUT/ql.universal"
    echo "bundled the Quick Look thumbnail + preview extensions"
else
    echo "WARNING: mosaic-quicklook missing -- the .app will ship without .mosaic thumbnails" >&2
fi

# App icon: render the SVG at 1024 and let Pillow write the multi-size .icns.
"$DSPY" - "$REPO/assets/app_icon.svg" "$APP/Contents/Resources/mosaic.icns" <<'PY'
import subprocess, sys, tempfile, os
from PIL import Image
svg, out = sys.argv[1], sys.argv[2]
with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as t: png = t.name
subprocess.run(["rsvg-convert", "-w", "1024", "-h", "1024", svg, "-o", png], check=True)
img = Image.open(png).convert("RGBA")
img.save(out, format="ICNS")
os.unlink(png)
print("icns ok")
PY

# ---- 2b. runtime data (installedDataDir() -> Contents/Resources) --------------
# The CC-0 brush set, document templates, and the vendored default CMYK press profile. common/
# settings.cpp resolves installedDataDir() to Contents/Resources inside a .app, and
# core/color_management.cpp reads the CMYK profile from icc-profiles/ there.
mkdir -p "$APP/Contents/Resources/brushes" "$APP/Contents/Resources/presets" \
         "$APP/Contents/Resources/icc-profiles"
cp -R "$REPO/data/brushes/." "$APP/Contents/Resources/brushes/" 2>/dev/null || true
cp -R "$REPO/data/presets/." "$APP/Contents/Resources/presets/" 2>/dev/null || true
cp "$REPO/third_party/icc-profiles/"*.icc "$APP/Contents/Resources/icc-profiles/" 2>/dev/null || true

# ---- 2c. translation catalogs (Contents/Resources/locale) --------------------
# .mo files are architecture-independent, so they are compiled straight from po/ rather than
# lifted out of one of the per-arch build trees. common/i18n.cpp resolves the catalog directory to
# Contents/Resources/locale inside a .app for the same reason installedDataDir() does: the
# compiled-in MOSAIC_LOCALEDIR names a cross-build prefix that does not exist on the user's Mac.
if command -v msgfmt >/dev/null 2>&1 && [ -f "$REPO/po/LINGUAS" ]; then
    _catalogs=0
    while read -r lang; do
        case "$lang" in ''|'#'*) continue;; esac
        for domain in mosaic motivate; do
            po="$REPO/po/$lang/$domain.po"
            [ -f "$po" ] || continue
            mkdir -p "$APP/Contents/Resources/locale/$lang/LC_MESSAGES"
            msgfmt --check -o "$APP/Contents/Resources/locale/$lang/LC_MESSAGES/$domain.mo" "$po"
            _catalogs=$((_catalogs + 1))
        done
    done < "$REPO/po/LINGUAS"
    echo "bundled $_catalogs translation catalog(s)"
else
    echo "msgfmt not found (or no po/LINGUAS): the .app will be English-only" >&2
fi

# ---- 3. install-name fixups (self-contained, relocatable) ---------------------
"$INT" -id "@rpath/libvulkan.1.dylib" "$APP/Contents/Frameworks/libvulkan.1.dylib"
vkdep=$("$OTOOL" -L "$APP/Contents/MacOS/mosaic" | awk '/libvulkan/{print $1; exit}')
[ -n "$vkdep" ] && "$INT" -change "$vkdep" "@rpath/libvulkan.1.dylib" "$APP/Contents/MacOS/mosaic"
"$INT" -add_rpath "@executable_path/../Frameworks" "$APP/Contents/MacOS/mosaic" 2>/dev/null || true

# ---- 3b. ad-hoc code signing (REQUIRED to run on Apple Silicon) ----------------
# Must come after every install_name_tool edit -- signing seals the Mach-O, and any later change
# invalidates it. rcodesign signs a bundle RECURSIVELY, so the nested dylibs and the Quick Look
# .appex are covered by this one call (an unsigned nested bundle would invalidate the host's own
# signature, so recursion is not a convenience here -- it is the requirement).
if [ -x "$RCODESIGN" ]; then
    "$RCODESIGN" sign "$APP" >/dev/null 2>&1 && echo "ad-hoc signed Mosaic.app"
else
    echo "WARNING: no rcodesign found -- the app will NOT launch on Apple Silicon (unsigned arm64" >&2
    echo "         is rejected by the kernel). Install rcodesign or set MOSAIC_CODESIGN." >&2
fi

# ---- 4. DMG staging (background + .DS_Store; the Applications symlink is added into the volume) ---
echo "== building $VOL.dmg =="
STAGE="$OUT/stage"
rm -rf "$STAGE"; mkdir -p "$STAGE/.background"
cp -R "$APP" "$STAGE/"
# Background MUST be .background/background.tiff -- the .DS_Store alias resolves that exact name.
"$DSPY" "$HERE/make-background.py" "$REPO/assets/app_icon.svg" "$STAGE/.background/background.tiff"
# Window 660x400, 128px icons; app left / Applications right, aligned with the background arrow.
"$DSPY" "$HERE/dmg_dsstore.py" "$STAGE/.DS_Store" "$VOL" 660 400 128 180 205 480 205

# ---- 5. HFS+ image -> compressed .dmg ----------------------------------------
RAW="$OUT/$VOL.hfs"
kb=$(du -sk "$STAGE" | cut -f1)
mb=$(( kb / 1024 + 20 ))            # content + 20 MB slack
rm -f "$RAW"
dd if=/dev/zero of="$RAW" bs=1M count="$mb" status=none
"$NEWFS" -v "$VOL" "$RAW" >/dev/null
"$HFSPLUS" "$RAW" addall "$STAGE" >/dev/null
# hfsplus addall drops the executable bit (everything lands 0644); LaunchServices refuses to run a
# non-executable main binary ("cannot open application"). Restore +x on it. Filesystem metadata
# only -- the embedded ad-hoc signature is untouched.
"$HFSPLUS" "$RAW" chmod 0755 Mosaic.app/Contents/MacOS/mosaic >/dev/null
# The extensions' executables need it too -- an .appex the system cannot exec is an .appex that
# silently never produces a thumbnail or a preview.
[ -d "$QLAPP" ] && "$HFSPLUS" "$RAW" chmod 0755 \
    Mosaic.app/Contents/PlugIns/MosaicQuickLook.appex/Contents/MacOS/MosaicQuickLook >/dev/null
[ -d "$QLPREVIEW" ] && "$HFSPLUS" "$RAW" chmod 0755 \
    Mosaic.app/Contents/PlugIns/MosaicQuickLookPreview.appex/Contents/MacOS/MosaicQuickLookPreview \
    >/dev/null
"$HFSPLUS" "$RAW" symlink Applications /Applications >/dev/null
rm -f "$OUT/$VOL.dmg"
"$DMGTOOL" "$RAW" "$OUT/$VOL.dmg" >/dev/null
rm -f "$RAW"

echo "== done: $OUT/$VOL.dmg =="
ls -la "$OUT/$VOL.dmg"
