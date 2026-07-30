# Third-party licenses

Mosaic is **GPLv3**. Every bundled or linked dependency is GPLv3-compatible. This file
lists what is **currently vendored** in the repository; the complete (forward-looking)
dependency and license matrix — including the system libraries Mosaic links as features
land — is maintained in [`PLAN.md` §6 and §7](../PLAN.md).

## Vendored (in `third_party/`)

| Component | Version | License | Location | Used for |
|-----------|---------|---------|----------|----------|
| doctest | 2.4.11 | MIT | `third_party/doctest/` (`doctest.h`, `LICENSE.txt`) | unit tests |
| nanosvg | master (vendored 2026-06) | zlib | `third_party/nanosvg/` (`nanosvg.h`, `nanosvgrast.h`, `LICENSE.txt`) | SVG rasterization (app icon now; tool icons in S52) |
| nlohmann/json | 3.11.3 | MIT | `third_party/nlohmann/` (`json.hpp`, `json_fwd.hpp`, `LICENSE.MIT`) | JSON for the settings store (S5); later the `.mosaic` manifest, `.ora`/`.kpp` |
| earcut.hpp | master (vendored 2026-07) | ISC | `third_party/earcut/` (`earcut.hpp`, `LICENSE`) | glyph-contour triangulation for the Type 3D extrude caps (S30-c) |
| VulkanMemoryAllocator (VMA) | 3.3.0 | MIT | `third_party/vma/` (`vk_mem_alloc.h`, `LICENSE.txt`) | Vulkan device-memory allocation (GPU compositor, S7-b onward) |
| xxHash | 0.8.3 | BSD-2-Clause | `third_party/xxhash/` (`xxhash.h`, `LICENSE`) | `.mosaic` fast checksum tier (xxh3-64, single-header `XXH_INLINE_ALL`) |
| BLAKE3 (C, portable lanes) | 1.8.2 | CC0-1.0 OR Apache-2.0 (taken under CC0) | `third_party/blake3/` (5 files, `LICENSE_CC0`, `LICENSE_A2`) | `.mosaic` strong checksum tier (ROOT chunks). Vendored so niche distros without `libblake3` still build; SIMD lanes deliberately omitted (ROOT payloads are KB-sized) |
| pugixml | 1.16 | MIT | `third_party/pugixml/` (`pugixml.hpp`, `pugixml.cpp`, `pugiconfig.hpp`, `LICENSE.md`) | XML under the brush-preset formats (`docs/brushes.md` §7): sensor `<params>` fragments, the `.kpp` preset document, `.bundle` manifests. XPath compiled out (`PUGIXML_NO_XPATH`) |
| Hosek-Wilkie sky model (RGB lane) | 1.4a (2013) | BSD-3-Clause | `third_party/hosekwilkie/` (`ArHosekSkyModel.h`, `ArHosekSkyModel.c`, `ArHosekSkyModelData_RGB.h`, `LICENSE`) | analytic sky-dome radiance for the Texture Generator (S55-b; `docs/texture-generator.md` §4.1). Trimmed to the RGB lane; the ~580 KB spectral/XYZ datasets and alien-world/spectral-solar functions deliberately omitted |

Each vendored component keeps its upstream license file beside it. When adding a vendored
library, (1) include its `LICENSE`, (2) confirm GPLv3 compatibility, and (3) add a row
here.

## Notable compliance notes (see PLAN.md §7 for detail)

- **FreeType** (linked, system) is taken under its **GPLv2+** arm of the FTL-or-GPLv2+ dual
  license — GPLv3-compatible, and it sidesteps the FTL advertising/attribution clause. Used by
  the Type tool (S29) for glyph outlines and colour-glyph rasterization. **HarfBuzz** (MIT,
  shaping) and **fontconfig** (MIT-like, Linux font enumeration/fallback) are both permissive and
  GPLv3-compatible.
- **LibRaw** will be used under its **LGPL-2.1** option (not CDDL, which is not
  GPL-compatible).
- **libhyphen** (linked, system; Type-tool hyphenation, deferred §1) is tri-licensed
  GPL2/LGPL-2.1/MPL-1.1 — taken under its **LGPL-2.1 / MPL** arms for GPLv3 compatibility.
  Liang's line-breaking algorithm was published in **1983**. Its pattern
  dictionaries (`hyph_*.dic`) are read from the system dir (`/usr/share/hyphen`); bundling the
  common languages for Windows/macOS is a later packaging step (deferred §1).
- **enchant-2** (linked, system; Type-tool spell-checking, deferred §2) is **LGPL-2.1+** — used
  under that license, GPLv3-compatible. It is a thin front-end that unifies the
  hunspell/nuspell/aspell providers and locates the system spell dictionaries
  (`/usr/share/hunspell/*.dic`). Dictionaries are read
  from the system; Windows uses the native `ISpellChecker` and macOS `NSSpellChecker` (deferred §2),
  so enchant is a Linux-only dependency. Bundling dictionaries for packaging is a later step.
- **libcanberra** (linked, system, **OPTIONAL**; the AskOrTell dialog's alert sounds) is
  **LGPL-2.1+** — used under that license, GPLv3-compatible. It resolves freedesktop Sound Naming
  Specification event ids (`dialog-warning`, …) against the user's own XDG sound theme and honours
  their desktop event-sounds switch, so Mosaic ships no audio of its own and holds no sound-asset
  licences. Probed with `pkg_check_modules(... QUIET)`: a build without it simply has silent
  dialogs. Linux only — macOS uses `NSBeep` (AppKit) and Windows `MessageBeep`, both stock OS
  calls with no dependency.
- **The M4 export codecs** (linked, system, **all OPTIONAL**; `src/io/CMakeLists.txt` probes each
  and compiles inert stubs when it is absent) are permissive and GPLv3-compatible: **libwebp** and
  **libwebpmux** (BSD-3, Google), **libavif** (BSD-2) over **libaom** (BSD-2 + the AOMedia patent
  licence) or **SVT-AV1** (BSD-3 + AOMedia), **libtiff** (BSD-like, "libtiff licence"), and
  **giflib** (MIT). **AVIF's AV1 encoder is chosen explicitly at runtime — libaom first,
  SVT-AV1 second, and the format is not offered when neither is present.** `libavif`'s "AUTO"
  codec choice would resolve to whatever the distribution linked, which on some distributions
  includes a Rust encoder Mosaic neither ships nor invokes; the choice is therefore enforced in
  `src/io/avif.cpp`, where it is observable, rather than left to packaging.
- **No PatchMatch / Content-Aware Fill implementation, ever.** Mosaic's inpainting is Telea /
  Navier–Stokes plus an opt-in local model, by design; a patch-search synthesiser is not to be added.
- **HEVC/HEIF: Mosaic bundles no codec.** HEIF support is optional and delegates to a system codec
  only (`docs/heic-strategy.md`). **AVIF** is the preferred modern/HDR format.
