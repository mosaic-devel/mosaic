Mosaic is a GPU-accelerated image editor for Linux, Windows and macOS, written in C++20 against
Vulkan. **This is the first published release** — everything before it existed only as a source
tree. It is alpha software: the feature surface is broad but the polish is not, and the warnings
below are real rather than boilerplate.

**This project is not sponsored or endorsed by Anthropic.**

## Before you download

- **The Windows build is buggy.** Under Wine it flashes rapidly — **do not run it if you have
  epilepsy.** It has never been run on real Windows hardware by the author.
- **The macOS build is unnotarized** and its Quick Look extensions may misbehave; they install
  automatically when you open the DMG (Apple's design, not a choice Mosaic makes), and have been
  observed to make Finder hang.
- **Performance degrades on large documents** on every platform.
- Nothing here is signed by a certificate authority. Every OS will say so, once, per platform
  instructions below.

## Which file do I want?

| Platform | File | Notes |
| --- | --- | --- |
| Linux x86-64 | `Mosaic-0.3.0-x86_64.AppImage` | `chmod +x`, then run |
| Linux ARM64 | `Mosaic-0.3.0-aarch64.AppImage` | `chmod +x`, then run |
| Windows x64 | `Mosaic-0.3.0-windows-x86_64.msi` | per-user installer |
| Windows x64 | `Mosaic-0.3.0-windows-x86_64.zip` | no installer; unzip and run |
| Windows ARM64 | `Mosaic-0.3.0-windows-aarch64.{msi,zip}` | **never tested on hardware** |
| macOS | `Mosaic-0.3.0-macos-universal.dmg` | one image, Apple Silicon + Intel |
| Source | `mosaic-0.3.0-source.tar.gz` | GPLv3; see README for build instructions |

Verify a download against `SHA256SUMS`.

## Requirements

A **Vulkan 1.2** driver is required on Linux and Windows; macOS goes through MoltenVK, which is
bundled, and needs **macOS 11 Big Sur or newer** (the Quick Look space-bar preview extension needs
12.0 and simply does not load below that; thumbnails and the app itself do not). The Linux AppImages are built against **glibc 2.39**, so they run on Ubuntu 24.04+,
Debian 13+, Fedora 40+ and the rolling distributions. They deliberately do not bundle the GPU
stack — your own Mesa or proprietary driver is used.

## First launch

**Linux** — `chmod +x Mosaic-0.3.0-*.AppImage && ./Mosaic-0.3.0-*.AppImage`.

**Windows** — SmartScreen will say "Windows protected your PC" once, because the binary carries no
Authenticode signature. **More info → Run anyway**. Windows needs no signature to *execute*
anything; what is missing only affects that first prompt.

**macOS** — the app is **ad-hoc signed**, which is what lets it start at all (the Apple Silicon
kernel refuses to execute an unsigned arm64 binary outright). It is *not* notarized and carries no
Developer ID, so Gatekeeper will still object: right-click the app → **Open**, or **System
Settings → Privacy & Security → Open Anyway**.

## Known limitations

- **Windows:** fractional HiDPI (125/150/200%) renders into the top-left of the framebuffer; the
  file picker blocks Mosaic's own timers while open; OpenEXR, LibRaw and libzip are absent.
- **macOS:** no pen-tablet pressure; libtiff, libwebp, OpenEXR and LibRaw are absent. Hyphenation
  and spell-check use the system services instead.
- **Linux:** `xdg_dialog_v1` modal parenting is not implemented (FLTK 1.4.5 does not expose the
  dialog's `xdg_toplevel`), so a dialog on native Wayland is not dimmed or prevented from raising.
- Batch export is not implemented; export presets are fixed built-ins.
- The interface is translated into 74 languages, but only a fraction have been reviewed by a
  speaker.

## Licence

GNU GPLv3. Third-party components and their licences are listed in
`docs/third-party-licenses.md`.
