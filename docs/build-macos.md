# Building Mosaic for macOS

**Status: BUILT (S58/S59).** Mosaic cross-compiles from Linux to a universal (arm64 + x86_64)
macOS app and a drag-to-Applications `.dmg`, using osxcross + MoltenVK. Runtime is validated on a
real Mac by the user; the cross-build itself is compile-/link-clean.

The full, reproducible pipeline and its environment contract live in
[`packaging/macos/README.md`](../packaging/macos/README.md). This document records the design and
the decisions behind it.

## Approach

- **Toolchain:** osxcross (Clang/LLVM + cctools/ld64 + the macOS SDK sysroot). No Xcode, no Apple
  hardware to build. Toolchain file: `cmake/toolchains/osxcross.cmake` (driven by `OSXCROSS_ROOT`,
  arch via `-DMOSAIC_OSX_ARCH`). Presets: `macos-arm64`, `macos-x86_64`.
- **Vulkan:** MoltenVK translates Vulkan to Metal. We ship the official prebuilt **universal**
  `libMoltenVK.dylib` as an ICD (its build system is Xcode-only, so it is not cross-built) plus a
  cross-built **Vulkan-Loader** so the standard loader path — layers, portability enumeration — is
  intact. The app points the loader at the bundled ICD at startup (`main.cpp`, `VK_ICD_FILENAMES`).
- **Surface:** `VK_EXT_metal_surface`. `platform/native_window_macos.mm` attaches a `CAMetalLayer`
  to the FLTK window's `NSView` (the pattern FLTK's own Cocoa GL driver uses); `window_renderer.cpp`
  creates the surface and sets the MoltenVK portability-enumeration instance flag.
- **Dependencies:** the third-party stack (FLTK-Cocoa, freetype, harfbuzz, fontconfig, lcms2,
  libpng/jpeg/lz4/zstd, expat, libhyphen, gettext/libintl, libjxl (+brotli, highway), spdlog) is cross-built from source per
  arch and linked statically; only the Vulkan loader + MoltenVK are dylibs in the bundle.
  **gettext is on that list for macOS and not for Linux**: on glibc `gettext()` is part of libc, so
  `find_package(Intl)` succeeds for free there. macOS has no libintl at all — without the
  cross-built one `MOSAIC_HAVE_GETTEXT` stays undefined and the app is English-only no matter how
  many catalogs the bundle carries.
- **Translations:** `Contents/Resources/locale/<lang>/LC_MESSAGES/*.mo`, compiled from `po/` by
  `make-dmg.sh` (`.mo` files are architecture-independent, so they are built once rather than
  lifted out of a per-arch tree). `common/i18n.cpp` resolves that directory from the executable
  path, because the compiled-in `MOSAIC_LOCALEDIR` names a cross-build prefix that does not exist
  on the user's Mac.
- **Packaging:** a universal `Mosaic.app`, then an HFS+ `.dmg` built entirely on Linux
  (`newfs_hfs` + `hfsplus` + `dmg`) with the angled low-opacity app-icon background and a
  drag-to-Applications layout (Finder `.DS_Store` authored via `ds_store`/`mac_alias`).

## Decisions

- **Minimum macOS 13.3 (Ventura).** libc++ gates floating-point `std::to_chars`/`std::from_chars`
  behind 13.3; Mosaic uses charconv throughout. `CMAKE_OSX_DEPLOYMENT_TARGET=13.3`. (`from_chars`
  for floating point is additionally *absent* from libc++, so `common/charconv_compat.hpp` wraps it
  with a strtod fallback on libc++ while staying `std::from_chars` on libstdc++.)
- **Unsigned / not notarized.** No Apple Developer ID. Gatekeeper shows the usual first-run prompt;
  users Open-anyway once. Deliberate — see `packaging/macos/README.md`.
- **SDK licensing.** osxcross needs the macOS SDK, which you extract from Xcode / the Command Line
  Tools yourself; Apple's license restricts third-party *redistribution* of the SDK, not building an
  app against it or shipping that app.

## v1 feature gaps on macOS (Linux-only this pass)

- **Tablet pressure** — native: `ui/tablet_input_macos.mm` reads pressure/tilt/rotation off the
  NSEvent stream through a local monitor (no XInput2/`zwp_tablet_v2`, same sample ring).
- **Hyphenation** — native: `core/text/hyphenator_macos.mm` calls CoreFoundation's
  `CFStringGetHyphenationLocationBeforeIndex`, so no libhyphen dictionaries are bundled.
- **System dark-mode detection** — native: `platform/system_theme_macos.mm` (`NSAppearance` +
  `NSColor.controlAccentColor`), with live switching via `AppleInterfaceThemeChangedNotification`.
- **Spell-check** — uses the native **`NSSpellChecker`** (no enchant/glib, no bundled dictionaries).
- **Menus** — native: the system menu bar at the top of the screen, with About / Settings / Quit in
  the application menu (S58-b). The in-window menu row and the motivational-ticker easter egg that
  lived in it do not exist on macOS.
- **Thumbnails and the space-bar preview** — native: two Quick Look extensions (S58-e),
  `Contents/PlugIns/MosaicQuickLook.appex` and `MosaicQuickLookPreview.appex`, both reading the same
  PRVW chunk the freedesktop thumbnailer and the KIO plugin read on Linux. Two bundles because
  `NSExtensionPointIdentifier` holds one value and thumbnails and previews are separate extension
  points; they embed the same executable and name different principal classes. Neither composites
  the document — that would put the layer stack and a compositor inside an extension — so the
  preview is as sharp as the stored 256 px PRVW and no sharper, captioned with the canvas size
  read from the manifest.
- **Tablet pressure is UNTESTED on macOS** and will stay that way for now: the hardware here needs
  a vendor kernel driver that is not part of the OS, and installing one to test is not a reasonable
  ask. The code path is compile-verified and follows the NSEvent tablet-subtype contract; treat it
  as unproven, not as working.

## Known i18n limitations

- **The application-menu strings are new msgids with no catalog entries.** "About Mosaic",
  "Settings…", "Services", "Hide Mosaic", "Hide Others", "Show All" and "Quit Mosaic" are wired
  through `_()` and reach a translator the moment the catalogs are regenerated, but until that pass
  they render in English on every locale. The Window menu, which FLTK builds from hard-coded
  English and never routes through the catalogs, is switched OFF for exactly this reason.
- **History entry names** are marked with `N_()` in core and translated where the History panel
  builds each row (S58-f), so they are extractable now; they too await a catalog pass. Names a
  command assembles at run time (ones embedding a layer's own name) have no msgid and stay as-is
  by design.
- **The window-title chrome** ("Untitled", "unsaved", "for", and the min/sec abbreviations) is
  marked, but the strings are passed INTO `formatWindowTitle` through `UnsavedTitleFormat` rather
  than looked up inside it — the formatter is pure and golden-tested, and a function that read the
  active locale could not be tested without the tests inheriting whatever catalog was bound. Three
  of those msgids are a word or less, so each carries a `TRANSLATORS:` note on the line above its
  `_()`; xgettext attaches a comment only to the call directly below it.
- ⚠ **Generated document titles do not inflect.** The New Document dialog composes a title from a
  fixed adjective plus a rotating noun. That is grammatical in English, where an adjective never
  agrees with its noun, and ungrammatical in the many languages where it must agree in gender,
  number or case: one adjective form cannot serve nouns of different genders. Translators can only
  choose which form to lose. Fixing it properly means either giving each noun its own agreeing
  adjective form in the catalog, or dropping the adjective+noun construction for something
  agreement-free. **Deliberately deferred** — it is a catalog-shaped problem across every inflected
  language in the set, not a code fix, and the current catalogs are an explicitly partial pass.
