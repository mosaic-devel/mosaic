# Building the Mosaic Windows package from Linux (S57)

Mosaic is cross-compiled for Windows from Linux with **MinGW-w64** — there is no Visual Studio
build and no Windows machine in the loop. The result is a portable **zip** and a per-user **MSI**,
for two architectures:

| arch | toolchain | status |
|------|-----------|--------|
| `x86_64` | the system mingw-w64 GCC (`x86_64-w64-mingw32-gcc`) | the one that is exercised |
| `aarch64` | llvm-mingw (`/opt/llvm-mingw`) | **unverified** — see "Windows on ARM" below |

Everything keys off environment variables; no host paths are committed.

> **Unsigned, by decision.** Windows requires no signature to *run* anything: an unsigned `.exe` and
> an unsigned `.msi` both work, on x86_64 and on arm64 alike. What is missing is an **Authenticode**
> signature, so a freshly *downloaded* copy trips **SmartScreen** once — "Windows protected your PC",
> cleared with **More info → Run anyway**. After that first launch Windows remembers it.
>
> This is a different situation from macOS, and the difference is worth keeping straight. There, a
> signature is required to *execute*: the Apple Silicon kernel refuses to run an unsigned arm64
> Mach-O outright, so `packaging/macos/make-dmg.sh` **ad-hoc signs** the bundle with `rcodesign`
> (which runs on Linux and needs no Apple account). What macOS skips is the *Developer ID* signature
> and notarization, which is why Gatekeeper still asks. So: macOS needs a signature to start and a
> paid one to be quiet; Windows needs none to start and a paid one to be quiet. Neither is
> CA-signed, by user decision.

## One-time setup

1. **Toolchains.**
   ```bash
   # x86_64 (Arch/CachyOS)
   sudo pacman -S --needed mingw-w64-gcc
   # Windows on ARM: llvm-mingw, into /opt/llvm-mingw (or set MOSAIC_LLVM_MINGW)
   sudo pacman -S --needed llvm-mingw
   ```

2. **Packaging tools.**
   ```bash
   sudo pacman -S --needed msitools      # wixl + wixl-heat + msibuild (builds the .msi)
   sudo pacman -S --needed python-pillow librsvg   # make-ico.py
   sudo pacman -S --needed gettext       # msgfmt, for the 74 translation catalogs
   sudo pacman -S --needed hyphen-en hyphen-de     # optional: more hyph_*.dic to bundle
   ```
   `zip` is used if present and Python's `zipfile` module stands in when it is not, so the portable
   archive needs no extra package. Every one of these is checked with an actionable message rather
   than a traceback — `make-package.sh` refuses to start without `wixl`, and `make-ico.py` says which
   package to install.

3. **Dependencies** — cross-build the third-party stack, once per arch:
   ```bash
   export MOSAIC_WIN_PREFIX=/scratch/win/deps/x86_64
   MOSAIC_WIN_ARCH=x86_64 bash build-deps.sh
   ```
   `build-deps.sh` is the authoritative list of what exists and where. Everything is built **shared**
   (`.dll` + import library): Mosaic's own modules link statically into `mosaic.exe`, and the
   third-party stack ships as DLLs beside it.

## Building the package

```bash
export MOSAIC_WIN_PREFIX=/scratch/win/deps/x86_64
bash make-package.sh x86_64
# -> build/windows-package/Mosaic-<version>-windows-x86_64.zip
#    build/windows-package/Mosaic-<version>-windows-x86_64.msi
```

`make-package.sh` compiles the app (`build-app.sh`), assembles one staging tree, and emits **both**
artifacts from it — so the portable copy cannot depend on anything the installer would have written.
`MOSAIC_SKIP_BUILD=1` reuses an existing `build/windows-<arch>` tree.

### Environment

| variable | role |
|----------|------|
| `MOSAIC_WIN_PREFIX` | **required** — the dependency prefix for this arch (same variable `build-deps.sh` takes) |
| `MOSAIC_WIN_OUT` | output directory (default `build/windows-package`) |
| `MOSAIC_WIN_HYPHEN_DIR` | where to find `hyph_*.dic` (default: `/usr/share/hyphen`, then `$MOSAIC_WIN_PREFIX/share/hyphen`) |
| `MOSAIC_WIN_PYTHON` | a python3 with Pillow (default `python3`) |
| `MOSAIC_WIN_OBJDUMP` | objdump used to read PE imports (default: derived from the arch) |
| `MOSAIC_LLVM_MINGW` | llvm-mingw root for aarch64 (default `/opt/llvm-mingw`) |
| `MOSAIC_SKIP_BUILD` | `1` reuses the existing build tree |
| `MOSAIC_WIXL`, `MOSAIC_WIXL_HEAT` | override the msitools binaries |

## Payload layout

The same tree is the zip's contents and the MSI's `INSTALLDIR`:

```
mosaic.exe
MosaicThumbnail.dll         the Explorer IThumbnailProvider COM server (registered by the MSI)
mosaic-doc.ico              the .mosaic document icon (the association's DefaultIcon)
LICENSE.txt
*.dll                       ~22-25 third-party + MinGW runtime DLLs, all at the root
data/                       <- common::installedDataDir()
  brushes/                  the CC-0 default brush set
  presets/                  New-Document templates
  icc-profiles/             the vendored default CMYK press profile
  hyphen/                   hyph_*.dic  (core/text/hyphenator.cpp)
  locale/<lang>/LC_MESSAGES/{mosaic,motivate}.mo
etc/fonts/                  <- $FONTCONFIG_PATH
  fonts.conf
  conf.d/*.conf
```

Three facts about that tree are load-bearing:

- **Everything is found relative to `mosaic.exe`**, via `GetModuleFileNameW`. Nothing is compiled in:
  the prefix baked into the binaries names a directory on the Linux *cross-build host*. That is what
  makes the portable mode work — unzip anywhere, run, no installer, no registry, nothing written
  outside the folder (settings and state still go to `%APPDATA%` / `%LOCALAPPDATA%`, as they do for an
  installed copy).
- **The DLLs sit beside the exe, not in a `bin/`.** The Windows loader searches the directory of the
  executable image first; a `bin/` level would need either a PATH edit or a manifest redirection to
  buy nothing.
- **`data/` is `installedDataDir()`** (`src/common/settings.cpp`, `_WIN32` branch) and
  `data/locale` is the gettext catalog directory (`src/common/i18n.cpp`). `etc/fonts` is deliberately
  *not* under `data/`: it is fontconfig's own tree, copied verbatim from
  `$MOSAIC_WIN_PREFIX/etc/fonts` so it can be diffed against upstream, and `data/` stays Mosaic's.

### fontconfig

Mosaic uses fontconfig on Windows exactly as on Linux (`src/platform/font_db.cpp`) rather than
growing a DirectWrite backend, so the payload has to carry fontconfig's **rules** as well as its DLL.
`main.cpp` sets `FONTCONFIG_PATH` to `<exedir>/etc/fonts` at start-up, never overriding a value the
user set. The shipped `fonts.conf` needs no rewriting: it reaches the font directory and the cache
through fontconfig's own `WINDOWSFONTDIR` and `LOCAL_APPDATA_FONTCONFIG_CACHE` tokens, and includes
`conf.d` by a *relative* path.

⚠ `conf.d` is 30-odd **absolute symlinks** into `$MOSAIC_WIN_PREFIX/share/fontconfig/conf.avail`, so
the copy dereferences (`cp -RL`). A plain copy would preserve links pointing at a Linux path — the
tree would look complete and load zero rules.

No font **cache** is shipped; it belongs to the machine the app runs on and is built on first launch
into `%LOCALAPPDATA%`.

*Safety net, not the plan:* if `FONTCONFIG_PATH` somehow does not take, fontconfig falls back to a
configuration compiled into the DLL which still scans `C:\Windows\Fonts` — so the app finds real
fonts, but without `conf.d` it loses the alias rules that resolve a generic family like `sans-serif`
to an installed one.

## The DLL list is derived, never written down

`make-package.sh` reads `mosaic.exe`'s PE import table with `objdump -p`, stages what it names, and
repeats over everything staged until nothing new appears — a real transitive closure, because a DLL
has its own imports.

A hardcoded list rots the first time a dependency gains an import, and the failure mode is the worst
kind: the program builds, packages and zips, then dies on the user's machine with a modal
"`libfoo-1.dll` was not found" before a line of ours runs. The closure also means the payload holds
*only* what is reachable: `$MOSAIC_WIN_PREFIX/bin` additionally contains a dozen command-line tools
the dependencies install (`fc-cache.exe`, `cjpeg.exe`, `webpmux.exe`) and libraries nothing here links
(`libwebpdecoder.dll`, `libharfbuzz-subset`), and `cp $PREFIX/bin/*.dll` would ship all of it.

The script keeps an explicit skip-list of **system** DLLs. Shipping any of those is a *bug*, not an
optimisation: a private copy beside the exe wins the loader's search and shadows the machine's own.
`api-ms-win-*` and `ext-ms-win-*` are skipped by pattern — they are API *sets*, not files.

⚠ **`vulkan-1.dll` is on the skip-list and that is not a mistake.** It is a system component
installed by the GPU *driver*, and it is the piece that knows where that machine's ICDs are
registered; our own copy would enumerate no devices. `build-deps.sh` cross-builds the Vulkan loader
for its **import library only**.

An import that is neither resolvable nor known-system is reported as `⚠ UNRESOLVED` and does **not**
abort the run — it is either a genuinely missing dependency (the package is broken) or a Windows DLL
missing from the skip-list (harmless; add the name). Reviewing that list is part of a packaging pass.

### MinGW runtime DLLs

Resolved from the toolchain, not listed, because the names differ per toolchain:

| toolchain | location | DLLs |
|-----------|----------|------|
| mingw-w64 GCC (x86_64) | `/usr/x86_64-w64-mingw32/bin` | `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll` |
| llvm-mingw (aarch64) | `/opt/llvm-mingw/aarch64-w64-mingw32/bin` | `libc++.dll`, `libunwind.dll`, `libwinpthread-1.dll` |

(Both directories also hold DLLs Mosaic does not use — `libgomp-1`, `libomp`, `libatomic-1`,
`libquadmath-0` — which is exactly why the closure decides rather than a list.)

Whatever the set, the whole payload must agree on **one C runtime**. This is not a tidiness rule: a
`FILE*`, an `errno` or a locale crossing a CRT boundary is undefined behaviour, and it is also what
makes `_putenv_s` in `mosaic.exe` visible to `getenv` inside `libintl-8.dll` and
`libfontconfig-1.dll` (see the ⚠⚠ comment in `src/common/i18n.cpp`). The GCC toolchain here is
UCRT-based, so `ucrtbase.dll` comes from Windows.

*Note:* `libhyphen` builds **static** in this stack (`lib/libhyphen.a`, no DLL) despite
`--enable-shared`, so it is linked into `mosaic.exe` and never appears in the closure. Harmless, but
worth knowing before hunting for a missing `libhyphen-0.dll`.

## The MSI

Built with **`wixl`** from `msitools`, not NSIS.

- **Per-user**, not per-machine (`Package InstallScope="perUser"`). It needs no administrator rights
  and raises **no UAC prompt at all**, which matters more than usual for an unsigned package — an
  elevation prompt for an unsigned MSI reads "Unknown publisher", and asking someone to approve that
  is asking them to ignore the one signal Windows gives them. It installs to
  `%LOCALAPPDATA%\Programs\Mosaic`, the established per-user application location on Windows 10+.
  The cost: the shortcut and the file association exist for the **installing user only**, so on a
  shared machine each account installs its own copy.
- **Start-menu shortcut** directly in `ProgramMenuFolder` — no single-shortcut sub-folder, which is a
  wasted click on a Start menu that has been a flat searchable list since Windows 10. Non-advertised,
  so it never triggers MSI self-repair.
- **Uninstall entry** comes free with MSI; `ARPPRODUCTICON` gives it Mosaic's icon and `ARPNOMODIFY`
  hides a "Modify" button that would open a feature tree with one feature in it.
- **`.mosaic` association**: a `Mosaic.Document` ProgId with a `DefaultIcon` and a
  `shell\open\command` of `"<exe>" "%1"`, plus `OpenWithProgids` and
  `Applications\mosaic.exe\SupportedTypes` so "Open with" behaves. It needs no code, because the app
  already takes a **positional** file argument. `Content Type` carries the same `image/x-mosaic` MIME
  string Linux registers (`data/desktop/mosaic.xml`), so all three platforms name the format the same.
- **Explorer thumbnails**: the MSI writes `MosaicThumbnail.dll`'s COM registration (its CLSID +
  `InprocServer32`, and the `IThumbnailProvider` handler slot under both
  `SystemFileAssociations\.mosaic\ShellEx` and `.mosaic\ShellEx`). Registry rows rather than a
  `regsvr32` custom action, because rows belong to a component: MSI removes them on uninstall and
  rolls them back on a failed install, which a custom action cannot do.
  ⚠ They go under **`HKCU\Software\Classes`**, while `shell_thumbnail_win32.cpp`'s own
  `DllRegisterServer` writes `HKEY_CLASSES_ROOT` and its comment assumes a machine-wide installer.
  Per-user shell extensions under `HKCU\Software\Classes` are ordinary (HKCR is the merged HKLM+HKCU
  view), so the same relative paths just live in the user's hive — but the two spellings of this
  handler's registration must be kept in step by hand. They are one grep for the CLSID apart.
  A thumbnail already cached as a generic icon survives the install; wiping it is a separate step
  (`Disk Cleanup → Thumbnails`, or delete `%LOCALAPPDATA%\Microsoft\Windows\Explorer\thumbcache_*.db`).
- **Uninstall leaves user data alone** — settings, recent files, the font cache and the recovery
  journal all live outside `INSTALLDIR` and are not the installer's to delete.

### GUIDs

- **`UpgradeCode` is a fixed constant** in `mosaic.wxs`. It is the only thing that lets a new MSI
  recognise an older Mosaic as the same product; an installer that generates its own UpgradeCode can
  never upgrade anything, only accumulate side-by-side copies.
- **`ProductCode` is `Id="*"`** — fresh per build.
- **Component GUIDs are stable across builds**, which is what upgrades need. The two hand-written
  components carry literal GUIDs. The ~250 generated file components use `Guid="*"`, which wixl fills
  from a *name-based* UUID of the component's key path — verified by building the same tree twice and
  diffing `msiinfo export <msi> Component`: identical.
- ⚠ Because `project(VERSION)` is frozen at 0.2.17, **every** MSI has the same `ProductVersion`
  (MSI's version field is numeric-only; the git hash cannot live there — it goes in the summary
  Comments and in the exe's `VERSIONINFO` strings). So `MajorUpgrade AllowSameVersionUpgrades="yes"`
  is **required**: without it, installing a newer build over an older one leaves *two* Add/Remove
  Programs entries pointing at one directory.

### wixl is not full WiX

`wixl` 0.106 implements a useful subset of WiX v3. Deliberately avoided, having checked what its
parser actually supports:

| avoided | why |
|---------|-----|
| `<ProgId>` / `<Extension>` / `<Verb>` | supported, but wixl's `ProgId` has **no `Icon`/`IconIndex`** attribute, so `DefaultIcon` needs a `RegistryValue` regardless — and `OpenWithProgids` / `SupportedTypes` have no WiX element at all. Half-declarative is worse than explicit. |
| `Root="HKMU"` | ⚠⚠ **broken in msitools 0.106.** HKMU should emit `Root = -1` (HKCU for a per-user install, HKLM for per-machine); wixl writes its own enum value **4**, which is not a root Windows Installer knows. Verified with a probe package and `msiinfo export <msi> Registry`. Every association row therefore says `Root="HKCU"` explicitly, and flipping the package to per-machine is a two-line change (`InstallScope`, plus those roots to `HKLM`). |
| `<UI>` / `<Dialog>` / a custom wizard | wixl has the elements, but a hand-built dialog set is a large unverifiable surface for a package whose only choice is "install". The default MSI UI is what runs. |
| `<Condition>` launch conditions (e.g. a minimum-OS check) | `VersionNT` is manifest-gated on Windows 10+ and reports the wrong thing for un-shimmed apps. A launch condition that misfires is worse than none. |
| `<WixVariable>`, `<Merge>`/`<MergeRef>`, `<SetProperty>`, WiX v4 syntax | not implemented by wixl at all. |

**Not verifiable from Linux** (they build clean and produce the right table rows; whether *Windows*
does the intended thing with them is a question for the interactive pass):

- `InstallScope="perUser"` emits no `ALLUSERS` property — it sets the summary-information Word Count
  "does not require elevated privileges" bit instead (`Source: 10` in `msiinfo suminfo`). `ALLUSERS`
  absent *is* a per-user install, so this looks right; it is not the `ALLUSERS=2` +
  `MSIINSTALLPERUSER=1` dual-purpose pattern WiX v3 emits.
- `LocalAppDataFolder` / `ProgramMenuFolder` / `ProgramsFolder` resolving as intended for a per-user
  install (they are standard MSI directory properties; wixl just writes Directory rows).
- The `[INSTALLDIR]mosaic.exe` formatted string in a non-advertised `Shortcut.Target`.
- Whether Explorer actually picks up the association without a shell-change notification.

### Windows on ARM

`wixl --arch` accepts only `x86`, `intel`, `intel64`, `ia64`, `x64` — there is **no arm64** in
msitools 0.106. Both arches are therefore built with `--arch x64`, and the aarch64 package is
retagged afterwards with `msibuild -s ... "Arm64;1033"`. That fixup is **unverified and non-fatal**:
Windows does not check that the declared template matches the machine code inside, and ARM64 Windows
accepts x64 packages, so a failure costs the correctness of a metadata field rather than the
artifact.

The arm64 build as a whole is unverified — the system mingw-w64 GCC cannot target it at all (x86_64
only), which is why llvm-mingw is used there, and no ARM64 Windows machine has run it.

## Smoke test

Wine is enough to prove the exe **loads and links**, which is the thing a cross-build gets wrong:

```bash
cd build/windows-package/Mosaic-*-windows-x86_64
wine mosaic.exe --version
wine mosaic.exe --help
```

A missing DLL shows up here immediately, as a modal "could not be found". What Wine does **not**
prove: Vulkan (its ICD story is not Windows'), GDI and per-monitor DPI behaviour, the manifest's
effect, the shell integration, or anything the MSI does. Those need real Windows.

## Files here

| file | role |
|------|------|
| `build-deps.sh`    | cross-build the third-party dependency stack for one arch |
| `build-app.sh`     | configure + build `mosaic.exe` for one arch |
| `make-package.sh`  | the end-to-end pipeline (build → staging tree → zip + MSI) |
| `mosaic.wxs`       | the wixl source: product, feature, shortcut, `.mosaic` association |
| `mosaic.rc.in`     | Win32 resource script — icon, `VERSIONINFO`, the manifest |
| `mosaic.manifest`  | application manifest — PerMonitorV2 DPI, ComCtl32 v6, UTF-8 ACP, asInvoker |
| `make-ico.py`      | multi-resolution `.ico` from an SVG (16…256, the 256 PNG-compressed) |

`mosaic.rc.in` and `mosaic.manifest` are consumed by `src/app/CMakeLists.txt`, not by the packaging
scripts: they are compiled *into* `mosaic.exe`. The `.rc` deliberately sits in its own CMake OBJECT
library — GNU windres parses its own command line and dies on `-Wall` ("invalid option -- 'W'"), so a
`.rc` cannot live on a target that carries the project's warning flags. llvm-windres tolerates them,
which would have made this an x86_64-only failure.

Nothing is **stripped**. `mosaic.exe` and the dependency DLLs keep their symbol tables, which costs
download size and buys the one thing an alpha needs most: a crash report that can be turned back into
function names.
