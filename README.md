> [!WARNING]
> **Development paused indefinitely.**
>
> Alpha/dev state, a lot of bugs and quality issues. Including but not limited to:
>
> - The **Windows** port is buggy. Running it under Wine causes rapid flashing — **do not run it if you have epilepsy.**
> - The **macOS** Quick Look / thumbnail extensions may not work and can cause Finder issues. They install automatically when you open the DMG (Apple's design, not a choice Mosaic makes).
> - **Performance issues on every platform**, most noticeable on large documents.
>
> **This project is not sponsored or endorsed by Anthropic.**

# Mosaic, the image editor written by Claude, and designed by a human

![Mosaic running on Linux, Windows and macOS](docs/mosaic-platforms.gif)

Features include:

* Vulkan
* Linux compiles for Linux/Windows/macOS
* An inpainting engine
* Native `.mosaic` file format — Reed–Solomon error correction, salvage past a damaged frame, full undo history inside the file, and a recovery journal that survives a hard kill
* Krita's brush engine, transcribed — ~90% compatible with the default set of brushes
* Cross-platform tablet support (only tested on Wayland and X11 so far, also worked on Wine)
* Advanced text engine including vertical text / RTL support, advanced 3D text, bending text, text conformance to paths and shapes
* Layer effects — outlines, shadows, glows, colour/gradient/pattern overlays
* Extensive vector pattern gallery (raster pattern support half implemented — no way to add your own so far)
* Texture Generator including photorealistic day/night sky with volumetric clouds, including a map (some widgets/controls may be buggy)
* Basic raster format export support, including JPEG XL
* Generous vector shape gallery, paths, pen tool
* Advanced crop tool
* Dark/light mode, with system theme change listening
* Questionable select brush
* Red eye tool with support for making your pinkeye look slightly better
* No telemetry

### On the file format

Every mainstream editor format — PSD, PSB, XCF, KRA, ORA — stores your work with no error
correction and no history. A corrupted byte in the wrong place costs you the file, and your
undo stack dies with the process.

`.mosaic` carries Reed–Solomon parity, can salvage past a damaged frame rather than giving up
at it, keeps the undo history in the document, and backs unsaved work with an app-owned
recovery journal that survives a hard kill without ever touching your file until you press
Save. Saves cost O(current content + new history) rather than O(session length), because
already-checkpointed history is verify-then-copied byte-verbatim instead of re-encoded.

It was designed against measurements, not intuition: six container designs benchmarked for
corruption resilience, save speed and compression; four-plus undo-history designs benchmarked
for storage, timing and corruption behaviour; roughly 500 adversarial checks across both. The
shipped format detects 11 of 11 mutations in its corruption battery.

Interface translated into 74 languages. Licensed under the **GNU GPLv3**.

## Building

Everything is built **from Linux**, including the Windows and macOS targets. There is no
Visual Studio build and no Mac is required.

Mosaic prefers **system-installed** dependencies. You need a C++20 toolchain, CMake ≥ 3.25,
Ninja, and the Vulkan loader + headers. The configure step prints exactly which optional
libraries it found or is missing.

### 1. Install the build tools

```bash
# Arch / CachyOS
sudo pacman -S --needed base-devel cmake ninja git vulkan-headers vulkan-icd-loader
# recommended for development (GPU validation + shader compiler):
sudo pacman -S --needed vulkan-validation-layers shaderc

# Debian / Ubuntu
sudo apt install build-essential cmake ninja-build git libvulkan-dev \
                 vulkan-validationlayers-dev glslang-tools

# Fedora
sudo dnf install gcc-c++ cmake ninja-build git vulkan-loader-devel vulkan-headers \
                 vulkan-validation-layers glslang
```

### 2. Configure, build, and test

```bash
git clone <repo-url> Mosaic && cd Mosaic
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

Run it:

```bash
./build/linux-debug/bin/mosaic --version
```

Use `linux-release` for an optimized build, or `linux-asan` for an AddressSanitizer/UBSan
build.

### Runtime dependencies

All GPLv3-compatible, all system packages: the Vulkan loader + validation layers,
shaderc/glslang, **FLTK 1.4**, lcms2, LibRaw, libpng, libjpeg-turbo, libtiff, libwebp,
giflib, OpenEXR/Imath, libjxl, libavif, FreeType, HarfBuzz, fontconfig, gettext, spdlog,
pugixml and libzip. Optional: libheif, OpenCV, and ONNX Runtime.

### Cross-compiling for Windows

Via **MinGW-w64**. Two architectures: **x86_64** through the system mingw-w64 GCC, and
**arm64** (Windows-on-ARM) through **llvm-mingw**, which is the only Linux-hosted toolchain
that can target it. Third-party libraries are cross-built as DLLs that ship beside
`mosaic.exe`. See [`docs/build-windows.md`](docs/build-windows.md) for the dependency set,
environment variables, and packaging (portable zip + MSI).

```bash
cmake --preset windows-x86_64        # or windows-arm64
cmake --build --preset windows-x86_64
```

### Cross-compiling for macOS

Via **osxcross + MoltenVK**, into a universal (arm64 + x86_64) `Mosaic.app` and a
drag-to-Applications `.dmg` — built entirely on Linux, with no Mac and no root.
**Ad-hoc signed**, because the Apple Silicon kernel refuses to execute an unsigned arm64
binary at all. It is not notarized and carries no Developer ID, so Gatekeeper will still
ask. See [`docs/build-macos.md`](docs/build-macos.md) and
[`packaging/macos/README.md`](packaging/macos/README.md).

```bash
cmake --preset macos-arm64           # or macos-x86_64
```

## Repository layout

[`PLAN.md`](PLAN.md) is the development record — architecture, dependency and licence
matrix, coding conventions, and the session-by-session history. [`docs/`](docs) holds a
design note per subsystem.

The application icon is a single SVG at [`assets/app_icon.svg`](assets/app_icon.svg),
rasterized to the platform icon formats at build time. Replace that one file to rebrand.

## Licence

Mosaic is licensed under the **GNU General Public License v3.0** — see
[`LICENSE`](LICENSE). Third-party components and their (GPLv3-compatible) licences are
listed in [`docs/third-party-licenses.md`](docs/third-party-licenses.md).
