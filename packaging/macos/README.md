# Building the Mosaic macOS `.dmg` from Linux (S58/S59)

Mosaic is cross-compiled for macOS from Linux with **osxcross** (a Clang/LLVM toolchain + the
macOS SDK sysroot) and **MoltenVK** (Vulkan-on-Metal). No Apple hardware is needed to *build*;
the result is an unsigned universal (arm64 + x86_64) `Mosaic.app` inside a drag-to-Applications
disk image.

> **Unsigned, by design.** We do not pay for an Apple Developer ID, so the app is not signed or
> notarized. On first launch users clear Gatekeeper once: right-click the app → **Open**, or
> **System Settings → Privacy & Security → Open Anyway**. Everything below produces a clean app;
> only Apple's signature is absent.

Everything keys off environment variables — no host paths are baked into the repo.

## One-time setup

1. **osxcross** — clone <https://github.com/tpoechtrager/osxcross>, drop a macOS SDK tarball
   (e.g. `MacOSX15.5.sdk.tar.xz`) into its `tarballs/`, and build:
   ```
   UNATTENDED=1 OSX_VERSION_MIN=13.3 ./build.sh
   export OSXCROSS_ROOT=/path/to/osxcross/target
   ```
   The SDK is Apple's; extract it from Xcode / the Command Line Tools per osxcross's `README.SDK.md`
   (its license forbids third-party *redistribution* of the SDK — building your own app against it,
   and shipping that app, is fine).

2. **Dependencies** — cross-build the third-party stack for each arch (FLTK-Cocoa, freetype,
   harfbuzz, fontconfig, lcms2, libpng/jpeg/lz4/zstd, expat, libhyphen, gettext/libintl, libjxl (+brotli, highway), spdlog):
   ```
   export MOSAIC_MAC_DEPS_ROOT=/scratch/mac/deps        # holds <root>/arm64, <root>/x86_64
   for a in arm64 x86_64; do
     OSXCROSS_ROOT=$OSXCROSS_ROOT MOSAIC_MAC_ARCH=$a \
       MOSAIC_MAC_PREFIX=$MOSAIC_MAC_DEPS_ROOT/$a bash build-deps.sh
   done
   ```

3. **Vulkan** — the Vulkan-Loader + headers (per arch) and the prebuilt universal MoltenVK ICD:
   ```
   export MOSAIC_MAC_VULKAN_STAGE=/scratch/mac/vulkan-runtime
   for a in arm64 x86_64; do
     OSXCROSS_ROOT=$OSXCROSS_ROOT MOSAIC_MAC_ARCH=$a \
       MOSAIC_MAC_PREFIX=$MOSAIC_MAC_DEPS_ROOT/$a bash fetch-vulkan.sh
   done
   ```

4. **DMG tooling** (Linux, no root, no Mac). Build these once and point the vars at them:
   - `newfs_hfs` / `mkfs.hfsplus` — create the HFS+ volume. From `hfsprogs` (Arch AUR: `hfsprogs`;
     Debian: `apt install hfsprogs`), or build diskdev_cmds from source.
   - `hfsplus` — inject files + the `/Applications` symlink without mounting. From
     <https://github.com/planetbeing/libdmg-hfsplus> (`hfs/hfsplus`).
   - `dmg` — compress the HFS+ image to a `.dmg`. From
     <https://github.com/fanquake/libdmg-hfsplus> (`dmg/dmg`).
   - a Python with `ds_store` + `mac_alias` + `Pillow` (for the `.DS_Store` layout, the background
     alias, and the `.icns`): `python3 -m venv venv && venv/bin/pip install ds_store mac_alias Pillow`.
   - `rcodesign` — **ad-hoc code signing** (from <https://github.com/indygreg/apple-platform-rs>).
     NOT optional: Apple Silicon's kernel refuses to run an unsigned arm64 binary ("invalid
     application"), independent of Gatekeeper. Ad-hoc signing needs no Apple account.
   ```
   export MOSAIC_HFS_NEWFS=/path/to/newfs_hfs
   export MOSAIC_HFS_HFSPLUS=/path/to/hfsplus
   export MOSAIC_HFS_DMG=/path/to/dmg
   export MOSAIC_DSSTORE_PYTHON=/path/to/venv/bin/python
   export MOSAIC_CODESIGN=/path/to/rcodesign
   ```

## Building the DMG

```
bash make-dmg.sh
# -> build/macos-dmg/Mosaic.dmg   (+ Mosaic.app)
```

`make-dmg.sh` compiles both arches (`build-app.sh arm64` / `x86_64`), `lipo`s them into a universal
`Mosaic.app`, bundles the Vulkan loader (`Contents/Frameworks/libvulkan.1.dylib`) and MoltenVK ICD
(`Contents/Resources/vulkan/icd.d/`), writes `Info.plist` + `mosaic.icns`, fixes install-names so the
bundle is relocatable, then builds the HFS+ image with the angled-icon background and the
drag-to-Applications layout. Set `MOSAIC_SKIP_BUILD=1` to reuse existing `build/macos-<arch>` trees.

## Files here

| file | role |
|------|------|
| `build-app.sh`      | configure + build one arch (`arm64`\|`x86_64`) with the osxcross toolchain |
| `build-deps.sh`     | cross-build the third-party dependency stack for one arch |
| `patches/`          | fixes applied to a dependency's source after unpacking (see below) |
| `fetch-vulkan.sh`   | Vulkan-Loader + headers (per arch) + prebuilt MoltenVK ICD |
| `make-dmg.sh`       | the end-to-end pipeline (build → universal `.app` → `.dmg`) |
| `make-background.py`| render the angled low-opacity app-icon DMG background from `assets/app_icon.svg` |
| `dmg_dsstore.py`    | author the Finder `.DS_Store` (window, big icons, background, icon positions) |
| `Info.plist.in`     | app bundle metadata (`@MOSAIC_VERSION@` is substituted at build time) |

## Notes / limits (v1)

- **Minimum macOS 13.3** (Ventura). libc++ gates floating-point `std::to_chars`/`from_chars` there.
- Runtime is verified on a real Mac by the user; the cross-build is compile-/link-verified here.
- Pen-tablet pressure, hyphenation, dark-mode detection, spell-check and the menu bar are all
  **native** on macOS (NSEvent / CoreFoundation / NSAppearance / NSSpellChecker / the system menu
  bar) rather than degraded — see `docs/build-macos.md`.
- **Optional image formats still absent on macOS:** libtiff, libwebp, OpenEXR, LibRaw. PNG, JPEG,
  JPEG XL and `.mosaic` are all present. Add one by giving it a `cmake_build`/`auto_build` line in
  `build-deps.sh` — the CMake side already probes for each and disables cleanly when missing.
- **Dependency patches** live in `patches/` and are applied by `apply_patch` right after `unpack`,
  on every run (`unpack` re-extracts the tarball each time). Each patch carries its own header
  explaining what it fixes and when it can be dropped. A patch that no longer applies is **fatal**
  rather than skipped — a version bump must not silently drop a fix. Deleting the dependency's
  stamp in `<prefix>/.stamps` is what forces the rebuild after adding or editing one.

### Quick Look thumbnails: what is unverified

`Contents/PlugIns/MosaicQuickLook.appex` is compile-/link-verified and structurally complete
(principal class exported, `NSExtensionMain` referenced, plist keys set, executable bit restored in
the image, signed recursively with the host). What no amount of building on Linux can answer is
whether macOS **loads** it: extensions are registered by LaunchServices when the app is installed
and run, and this app is ad-hoc-signed and not notarized. If thumbnails do not appear, check in
this order on the Mac:

```
pluginkit -mAvv -p com.apple.quicklook.thumbnail | grep -i mosaic   # is it registered at all?
qlmanage -r && qlmanage -r cache                                    # kick Quick Look
mdimport -r /Applications/Mosaic.app                                # re-scan the app's types
codesign -dvvv /Applications/Mosaic.app/Contents/PlugIns/MosaicQuickLook.appex
```

A file saved by a Mosaic older than S48-b carries no PRVW chunk and will never thumbnail — that is
by design, not a failure (see `src/thumbnailer/quicklook_macos.mm`). One Save fixes such a file.
