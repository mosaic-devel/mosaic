# Building Mosaic for Windows

**Status: BUILT for x86_64 (S57/S59); Windows-on-ARM is cross-built but unverified.** Mosaic
cross-compiles from Linux to a portable zip and a per-user MSI using **MinGW-w64**. There is
intentionally **no Visual Studio build, ever** — the Windows target is a Linux cross-build exactly
like the macOS one, so one machine builds all three platforms and nothing about the project needs a
second host to be reproducible.

The full, reproducible pipeline and its environment contract live in
[`packaging/windows/README.md`](../packaging/windows/README.md). This document records the design and
the decisions behind it.

## Approach

- **Toolchains — two of them, deliberately.** Toolchain file `cmake/toolchains/mingw-w64.cmake`,
  arch selected with `-DMOSAIC_WIN_ARCH=x86_64|aarch64`. Presets: `windows-x86_64`, `windows-arm64`.
  - `x86_64` uses the **system mingw-w64 GCC** (`x86_64-w64-mingw32-gcc`; Arch/CachyOS:
    `pacman -S mingw-w64-gcc`, Debian: `apt install g++-mingw-w64-x86-64`, Fedora:
    `dnf install mingw64-gcc-c++`). Same compiler family and version as the Linux build, so this
    `-Werror`-clean codebase stays clean with no new diagnostics to suppress.
  - `aarch64` uses **llvm-mingw** (`/opt/llvm-mingw`, or `MOSAIC_LLVM_MINGW`). Not a preference: the
    GNU mingw-w64 toolchain has **no aarch64 target at all**, so LLVM is the only way to build
    Windows-on-ARM from Linux. Clang raises several `-Wall` diagnostics GCC does not; `MosaicHelpers`
    silences exactly the same four the macOS build needed, keyed on the *compiler* rather than on
    `APPLE`.
- **Windows API floor: 10 1809** (`_WIN32_WINNT=0x0A00`). The Pointer Input Stack (Windows Ink),
  `ISpellChecker`, `UISettings`/immersive dark mode and per-monitor-v2 DPI awareness are all at or
  below it, and it is the oldest release Microsoft still services.
- **Vulkan:** the standard Windows loader and the vendor's own ICDs — no translation layer, unlike
  macOS's MoltenVK. The surface is `VK_KHR_win32_surface`; `platform/native_window_win32.cpp` reads
  the `HWND` back out of FLTK, which (unlike macOS) needs nothing *created*, because the HWND FLTK
  already made is presentable. ⚠ **`vulkan-1.dll` is not shipped** — it is a system component the GPU
  driver installs, and it is the piece that knows where the machine's ICDs are registered. The loader
  is cross-built for its **import library only**.
- **Dependencies:** the third-party stack (FLTK-Win32/GDI, freetype, harfbuzz, fontconfig, expat,
  lcms2, libpng/jpeg-turbo/zlib/lz4/zstd, libhyphen, gettext/libintl, libjxl (+brotli, highway),
  libwebp, libtiff, giflib, libaom+libavif, spdlog, Vulkan headers+loader) is cross-built from source
  per arch by `packaging/windows/build-deps.sh`. **All shared:** Mosaic's own modules link statically
  into `mosaic.exe`, the third-party stack ships as DLLs beside it. That is the ordinary layout for a
  MinGW application, it keeps the link fast, and it lets one library be replaced without relinking.
  - **gettext is on that list for Windows and not for Linux**, for the same reason it is for macOS: on
    glibc `gettext()` *is* libc, so `find_package(Intl)` succeeds for free. Windows has no libintl —
    without the cross-built one `MOSAIC_HAVE_GETTEXT` stays undefined and the app is English-only
    however many catalogs the package carries.
  - **fontconfig is kept rather than replaced with DirectWrite.** `platform/font_db.cpp` stays one
    code path on all three platforms; S58 made the same call for macOS. The cost is that the payload
    has to carry fontconfig's `etc/fonts` tree and that `main.cpp` has to point `FONTCONFIG_PATH` at
    it — the config path compiled into the DLL names the Linux cross-build prefix.
  - **libhyphen is kept, with its dictionaries bundled.** macOS has a system hyphenator
    (CoreFoundation) and needs neither; Windows has *no* hyphenation API at all and no
    `/usr/share/hyphen`, so the `hyph_*.dic` files ship inside the payload and
    `core/text/hyphenator.cpp` resolves `installedDataDir()/"hyphen"` first.
- **Everything is found relative to `mosaic.exe`**, via `GetModuleFileNameW` — the runtime data, the
  catalogs, the hyphenation dictionaries, the fontconfig tree. Nothing is compiled in, because every
  compiled-in prefix names a directory on the Linux *cross-build host*. That is also what makes the
  portable zip work: unzip anywhere and run.
- **Packaging:** one staging tree, two artifacts — a portable `.zip` and a per-user `.msi` built with
  **`msitools`' `wixl`** (not NSIS). Both ship the same tree, so the portable copy cannot depend on
  anything the installer would have written. The DLL list is **derived** from the PE import tables as
  a transitive closure, never hardcoded.

## Building

```bash
export MOSAIC_WIN_PREFIX=/scratch/win/deps/x86_64
MOSAIC_WIN_ARCH=x86_64 bash packaging/windows/build-deps.sh    # once per arch
bash packaging/windows/make-package.sh x86_64
# -> build/windows-package/Mosaic-<version>-windows-x86_64.{zip,msi}
```

`packaging/windows/README.md` has the full environment table, the prerequisite packages
(`msitools`, `python-pillow`, `librsvg`, `gettext`) and the payload layout.

## Decisions

- **Per-user MSI, not per-machine.** It needs no administrator rights and raises **no UAC prompt at
  all** — which matters more than usual for an unsigned package, since an elevation prompt for an
  unsigned MSI reads "Unknown publisher", and asking someone to approve that is asking them to ignore
  the one signal Windows gives them. Installs to `%LOCALAPPDATA%\Programs\Mosaic`. The cost: the
  shortcut and the file association belong to the installing user, so on a shared machine each account
  installs its own copy.
- **Unsigned — and the reason differs from macOS.** Windows needs no signature to *run* anything: an
  unsigned `.exe` and an unsigned `.msi` both work, on x86_64 and arm64 alike. What a downloaded copy
  gets is **SmartScreen** once — "Windows protected your PC", cleared with **More info → Run
  anyway**. macOS is the opposite shape: the Apple Silicon kernel refuses to *execute* an unsigned
  arm64 binary, so the `.app` **is** signed — ad-hoc, with `rcodesign`, which runs on Linux and needs
  no Apple account — and what it skips is the paid Developer ID signature and notarization, which is
  why Gatekeeper still asks. So macOS needs a signature to start and a paid one to be quiet; Windows
  needs none to start and a paid one to be quiet. Neither platform is CA-signed, by user decision.
- **GUI subsystem plus `AttachConsole`.** `mosaic.exe` links as a WINDOWS-subsystem image
  (`WIN32_EXECUTABLE`, i.e. `-mwindows`) so opening a document from Explorer does not flash a console
  window. The price is that such a process starts with **no standard handles**, so `mosaic --version`
  from `cmd.exe` would print into nothing — and Mosaic really is a command-line-driving program
  (`--version`, `--help`, `--gui-frames N`, `--bench`, a positional file argument, and the headless
  harness that depends on all of them). `main.cpp` calls `AttachConsole(ATTACH_PARENT_PROCESS)` first
  thing and reopens the C streams on `CONOUT$`/`CONIN$` when it succeeds; when it fails (Explorer, the
  Start menu, a file association) it does nothing at all, and in particular never `AllocConsole()`.
  Two residual quirks are documented at the call site: `cmd.exe` does not wait for a GUI-subsystem
  child, so its prompt returns before our output (`start /wait mosaic --version` orders it), and the
  console output *code page* is deliberately left alone because a change to it would outlive the
  process. MinGW-w64's own `WinMain` shim means plain `int main(int, char**)` still works unchanged.
- **A real resource section.** `packaging/windows/mosaic.rc.in` is configured into the build tree and
  compiled with windres: the multi-resolution icon (`make-ico.py`, 16…256 with the 256
  PNG-compressed), `VERSIONINFO`, and the application manifest. The binary `FILEVERSION` fields can
  only carry the frozen 0.2.17; the *string* fields carry `0.2.17+g<hash>`, which is the real build
  id, and `VS_FF_PRERELEASE` says "alpha" in the shell's own Properties dialog.
  - ⚠ The `.rc` lives in its own CMake **OBJECT library**. GNU windres parses its own command line
    rather than forwarding it and dies on `-Wall` with "invalid option -- 'W'", so a `.rc` cannot sit
    on a target that carries the project's warning flags. llvm-windres tolerates them, which would
    have made this an x86_64-only failure.
- **The manifest is not optional.** It declares per-monitor-v2 DPI awareness (Mosaic is a canvas
  application: a DPI-unaware process gets its output bitmap-stretched, so a 1:1 zoom stops being 1:1
  and the overlay hairlines blur), ComCtl32 v6 (or every shell dialog draws unthemed), UTF-8 as the
  process active code page, and `asInvoker`. FLTK also sets per-monitor awareness at run time, but
  only if the process is still unaware by the time its display opens — and it detects the manifest
  correctly (`GetProcessDpiAwareness` reports `PROCESS_PER_MONITOR_DPI_AWARE` for a PerMonitorV2
  process, because the legacy enum has no V2 value) and stands down. The manifest simply gets there
  first, before the first window exists. FLTK 1.4.5 embeds no manifest of its own, so there is nothing
  to collide with.
- **UTF-8 in, UTF-16 at the syscall.** `UNICODE`/`_UNICODE` are on, so the W-suffixed entry points are
  the default, and `src/common/fs_path.hpp` is the one crossing between Mosaic's UTF-8 `std::string`
  paths and `std::filesystem::path`, which is `wchar_t`-based here. The manifest's
  `activeCodePage=UTF-8` is for the **dependencies** — fontconfig's cache paths, libpng/libjpeg's
  `fopen`, gettext's catalog lookup — not for our own code, which is correct with or without it.
- **One C runtime for the whole payload.** Not tidiness: a `FILE*`, an `errno` or a locale crossing a
  CRT boundary is undefined behaviour. It is also what makes `_putenv_s` in `mosaic.exe` visible to
  `getenv` inside `libintl-8.dll` and `libfontconfig-1.dll`, which is how `$MOSAIC_LANG` and
  `$FONTCONFIG_PATH` reach them at all.

## Smoke testing

**Wine is installed on this host** (`wine-10.0`) and is the available test, but be clear about what it
proves:

```bash
cd build/windows-package/Mosaic-*-windows-x86_64
wine mosaic.exe --version
wine mosaic.exe --help
```

That is evidence the executable **loads and links** — which is precisely what a cross-build gets
wrong, and a missing DLL surfaces immediately as a modal "could not be found". It is **not** evidence
the GUI is correct. Wine's Vulkan is not Windows' Vulkan (a different ICD story with a different
driver behind it), its GDI and per-monitor DPI handling are its own approximations, the application
manifest is interpreted differently or not at all, and none of the shell integration — the file
association, the Start-menu shortcut, the Explorer thumbnail handler, the MSI itself — exists under
Wine in a form worth trusting. Those need real Windows.

## v1 status on Windows

Native, not degraded:

- **Windowing** — FLTK's own Win32/GDI backend (`FLTK_BACKEND_X11=OFF`, `FLTK_BACKEND_WAYLAND=OFF`).
- **Vulkan surface** — `VK_KHR_win32_surface` (`platform/native_window_win32.cpp`).
- **File dialogs** — the shell's Common Item Dialog (`IFileDialog`) via
  `platform/file_dialog_win32.cpp`, rather than `Fl_Native_File_Chooser`, whose Windows driver still
  calls the legacy `GetOpenFileName`.
- **Dark mode** — `platform/system_theme_win32.cpp` reads the registry keys the shell itself acts on,
  with live switching through `RegNotifyChangeKeyValue`.
- **Spell-check** — the OS Spell Checking API (`ISpellChecker`, Windows 8+), so no enchant, no glib
  and no bundled dictionaries. macOS uses `NSSpellChecker` for the same reason.
- **Tablet pressure** — the Pointer Input Stack (Windows Ink), `platform/tablet_win32.cpp`.
- **Explorer thumbnails** — `MosaicThumbnail.dll`, an in-process `IThumbnailProvider` COM server
  reading the same PRVW chunk the freedesktop thumbnailer, the KIO plugin and the macOS Quick Look
  extensions read. Registered declaratively by the MSI (`packaging/windows/mosaic.wxs`) so that an
  uninstall really removes it.
- **The in-window menu row stays.** Unlike macOS, which moves the menus to the system bar at the top
  of the screen and loses the motivational-ticker easter egg with them, Windows keeps Mosaic's own
  menu row exactly as Linux has it.

Bundled rather than taken from the OS, because Windows has no equivalent:

- **Hyphenation dictionaries** (`data/hyphen/hyph_*.dic`) — no system hyphenation API.
- **fontconfig's rule tree** (`etc/fonts/`) — no system fontconfig.
- **The gettext catalogs** (`data/locale/`) and **libintl** — no system libintl.

## Known limitations

- ⚠ **Windows on ARM is unverified.** It cross-builds (llvm-mingw) and is packaged the same way, but
  no ARM64 Windows machine has run it. Two specifics: the system mingw-w64 GCC cannot target the arch
  at all, so llvm-mingw is the only toolchain for it and the only one exercising the clang diagnostics
  path on Windows; and `wixl --arch` has no `arm64` value in msitools 0.106, so the arm64 MSI is built
  as `x64` and retagged afterwards with `msibuild -s`. That fixup is itself unverified, and harmless
  if it fails — Windows does not check that a package's declared template matches the machine code
  inside it, and ARM64 Windows accepts x64 packages.
- **Paths past `MAX_PATH`** work only where the machine's `LongPathsEnabled` policy is on. The
  manifest declares `longPathAware`, which is opportunistic rather than a guarantee.
- **`activeCodePage=UTF-8` needs Windows 10 1903+.** Below that, the dependencies fall back to the
  legacy ANSI code page and a non-ASCII path can defeat the ones that use narrow `fopen`. Mosaic's own
  path handling is unaffected either way.
- **Nothing is stripped.** The exe and the dependency DLLs keep their symbol tables — a bigger
  download in exchange for crash reports that can be turned back into function names, which is the
  right trade for an alpha.
- **`libhyphen` builds static** in this dependency stack despite `--enable-shared`, so it links into
  `mosaic.exe` rather than shipping as a DLL. Harmless; noted because it is confusing to go looking
  for a `libhyphen-0.dll` that was never built.
