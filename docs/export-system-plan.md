# Export & I/O System — Scoping Plan

Covers **S41** (I/O framework + loss-warning system), **S42** (common raster formats), **S44**
(extended formats), the pending half of **S18-b** (dev-grade save/export slice), and feeds **S56**
(export presets/batch). It does **not** cover **S48** (`.mosaic` native *Save*) — that has its own
spec (`docs/file-format-mosaic.md`). This note is the spec the build sessions follow; research
behind every library/API claim was verified against primary sources on 2026-07-04.

---

## 0. Decisions locked (2026-07-04, with the user)

- **Three File verbs, kept distinct** (§1): **Save** (`.mosaic`, work — blocked on S48), **Export
  As…** (custom two-pane modal, raster+vector), **Quick Export → PNG** (fast, system dialog titled
  "Export"). Export is *not* saving your work.
- **Dirty state = GIMP-style.** Only a real `.mosaic` Save clears the command-stack "saved" marker.
  Export sets a separate *exported-to-X* state. Until S48 lands, the window title honestly reads
  `• unsaved`. **S18-d** ships its title/duration machinery on these semantics — **✅ DONE 2026-07-04,
  commit `fb98be4`**: dirty is a *saved-position marker* on the command stack (`CommandStack::isSaved`
  / `markSaved`; `Document::dirty()` derives from it — no boolean to desync), the title reads
  `<doc> • unsaved[ for N min] — Mosaic`, and the "how long unsaved" / "include seconds" toggles live
  under **Settings → Annoyances**. `markSaved()` is wired but not yet *called* — a real `.mosaic` Save
  is what clears dirty, so that stays deferred to S48. (The earlier shorthand "S42 closes S18-d" was a
  slip — S42 closes **S18-b**, the dev-grade open/save slice.)
- **AVIF is IN**, via `libaom`/`SVT-AV1` (**never `rav1e`** — it is Rust). JXL remains the primary
  modern codec.
- **HEIC/HEVC encode = PARKED** for a dedicated design pass (§12.1). Not built in this arc.
- **JPEG XL is a first-class "common" format** (pulled forward from S43-c). 8-bit/lossy/lossless JXL
  needs nothing from the float pipeline; only HDR/float JXL waits for S43-a.
- **Format breadth = curated pro set (always on) + exotic tier behind a Settings flag.** The
  hand-rolled, dependency-free codecs compile into a standalone **`libmosaicformats`** (§2.2). QOI
  is included (always-on). Exotic formats gate on **Settings → General → "Show all export
  formats"**.
- **We already exceed GIMP** by exporting **OpenEXR, JPEG 2000 (Part-1), Radiance HDR, PAM** — all
  import-only in GIMP 2.10 *and* 3.0.
- **Milestone 1 = Quick Export → PNG** (the first user-verifiable feature) — **✅ DONE 2026-07-04,
  commit `4b692c5`** (§7). S60 GPU-residency is deprioritized (perf acceptable after recent commits).
- ~~**`src/io` relocates to `src/core/io`** (pure, FLTK-free; genuinely core) while it is still
  tiny.~~ **REVERSED 2026-07-27 (M2).** It never happened, and it must not: `mosaic_io` now
  depends on `mosaic::core` (the `.mosaic` document↔container bridge, S48), so nesting it under
  `core` inverts the dependency. The module is already pure and FLTK-free, which was the whole
  point. It stays `src/io/`, `mosaic::io`, `io/io.hpp`. See §2.1.

---

## 1. Vocabulary

| Verb | Shortcut | Writes | Dialog | Clears dirty? | Blocked on |
|---|---|---|---|---|---|
| **Save** | `Ctrl+S` | `.mosaic` (layers, effects, vector, history) | system Save | **yes** | S48 |
| **Save As…** | `Ctrl+Shift+S` | `.mosaic` to new path | system Save | yes | S48 |
| **Export As…** | `Ctrl+Shift+E` | flattened raster/vector | **custom modal** (this doc) | no (sets *exported*) | this arc |
| **Export to `<file>`** / **Overwrite** | — | re-run last export, no dialog | none | no | after first export |
| **Quick Export → PNG** | `Ctrl+Alt+E` (tent.) | PNG, no warnings | system Save titled "Export" | no | **milestone 1** |

Until S48, `Save`/`Save As…` stay greyed with a tooltip ("Native .mosaic save arrives with S48");
`Ctrl+S` does **not** silently become an export (that would train wrong muscle memory). Quick Export
is the fast path meanwhile.

---

## 2. Module & library architecture

### 2.1 `io` — the format registry (thin adapters over system libs)

> **⚠️ The `src/io → src/core/io` relocation did NOT happen, and should not.** §0 and §7.1 record
> it as done; it never was, and the tree has since made it wrong. `mosaic_io` **depends on
> `mosaic::core`** (the `.mosaic` document↔container bridge, `io/mosaic/docio.cpp`, S48), so
> making `io` a subdirectory of the `core` module would be a dependency inversion. The module
> already satisfies the only property the move was for — it is pure and FLTK-free. Namespace
> `mosaic::io`, target `mosaic::io`, include path `io/io.hpp`. Structure **as built** (M2):

```
src/io/
  io.hpp/.cpp            loadImage()/loadImageWithMetadata() + savePng/saveJpeg/saveJxl (the codecs)
  png.cpp jpeg.cpp jxl.cpp   the libpng / libjpeg-turbo / libjxl translation units
  exif.{hpp,cpp}         the EXIF READ slice (§7b)
  options_schema.{hpp,cpp}   OptionValue(s), OptionsSchema, descriptors, validateSchema  ← M2
  document_profile.{hpp,cpp} DocumentProfile + profileDocument() extraction              ← M2
  caps.{hpp,cpp}         FormatCaps, Severity, LossCode, LossWarning, diff()             ← M2
  format_backend.hpp     the FormatBackend interface (identity + caps + schema + encode)  ← M2
  format_registry.{hpp,cpp}  the registry: lookup, tiers, combobox order                  ← M2
  backends/
    backends.hpp         one factory per backend (explicit, never self-registering)       ← M2
    png_backend.cpp                                                                       ← M2
    jpeg.cpp  jxl.cpp  webp.cpp  avif.cpp  tiff.cpp  gif.cpp   (M4: heavy, system libs)
    openexr.cpp  jpeg2000.cpp  pdf.cpp  svg.cpp  eps.cpp       (M6/HDR tier)
    mosaicformats_backend.cpp                                  (M5: adapter → libmosaicformats)
  brush/  mosaic/        unrelated pre-existing sub-modules (.abr/.kpp presets; the native format)
```

`DocumentProfile` lives in its own header rather than in `caps.hpp` (the sketch put them
together) so that `diff()` can be exercised against a hand-built profile with no `core::Document`
in sight — the extraction and the diff fail independently, and their tests do too.

**Registration is explicit, not self-registering.** `mosaic_io` is a *static library*, and a
translation unit whose only contribution is a namespace-scope registrar object is exactly what
the linker may drop without `--whole-archive`. `FormatRegistry::instance()` therefore builds its
list by calling one factory per backend (`backends/backends.hpp`), which also gives the combobox
a deterministic order and lets a test build its own registry over stubs.

**The `FormatBackend` interface** (the load-bearing abstraction — makes "as advanced as they get"
scale to ~40 formats without 40 bespoke UI panels):

```cpp
struct FormatBackend {
  FormatId       id;                       // Png, Jpeg, Jxl, …
  FormatTier     tier;                     // Common | CuratedPro | ExoticFlagged
  const char*    displayName;              // "PNG image"
  vector<string> extensions;              // {"png"}
  string         mimeType;                 // "image/png"

  FormatCaps     caps() const;             // pure data — what the format can carry (§4)
  OptionsSchema  optionsSchema() const;    // pure data — typed controls the panel renders (§6)

  // encode the finished, resized RGBA(+float) buffer with the chosen options.
  EncodeResult   encode(const RenderInput&, const OptionValues&, ProgressFn) const;
  bool           available() const;        // runtime probe (TIFFIsCODECConfigured, libavif codec, …)
};
```

`OptionsSchema` is a list of typed descriptors (`Slider{min,max,step,label}`, `IntStepper`,
`Enum{labels}`, `Bool`, `Group{collapsedByDefault}`) so the right-pane "backend settings" panel is
**rendered generically from data**, not coded per format. Adding a format = one backend file + a
registry line; the UI needs no change.

**As built (M2)** — the vocabulary was designed against Appendix A's real per-format lists, not
in the abstract, so it covers `Bool` (interlace, progressive, lossless, exact), `Int`+range
(compression 0–9, quality 0–100, method 0–6, speed 0–10, palette 2–256), `Real`+range (JXL
distance 0–25, the one genuinely fractional knob), `Enum`, and `Text` (PNG text chunks, HDR
header variables). Three shape decisions:

- **Groups are a flat table, not nested descriptors.** Each `OptionDesc` names a `group` (empty =
  the always-visible common section) and the schema lists `OptionGroup{id, label,
  collapsedByDefault}`. The panel renders top-to-bottom, so a flat list plus membership is both
  simpler to render and trivially testable; a nested tree bought nothing.
- **Enum values are stable string ids, never indices.** Indices renumber the instant a backend
  inserts a choice, and these values are *persisted* (export presets, S56) and *read by
  `diff()`*. `EnumChoice{id, label, help}`: `id` is the stored value, `label` is the English
  source string M3 hands to gettext.
- **Dependent visibility is data**: `OptionCondition{key, values, negate}`, AND-ed. This is the
  piece that would otherwise force bespoke panel code — JXL's distance is meaningless when
  lossless is on, TIFF's JPEG-quality knob only applies to `compression == "jpeg"`, OpenEXR's DWA
  level only to `dwaa`/`dwab`, WebP's near-lossless only when lossless is on. Hiding is a UI
  affordance only: the value stays in the bag, so `encode()` must consult `visible()` too.

`OptionsSchema::coerce()` makes a value bag a valid, *complete* answer to the schema — fills
defaults, drops keys the schema does not declare (so a preset aimed at another format cannot leak
through), replaces wrong-typed arms, clamps and step-snaps numbers, snaps an unknown enum id back
to the default. After it, every typed read inside `encode()` is safe without further checking.
`validateSchema()` returns the structural problems in a schema (duplicate keys, out-of-range or
wrong-typed defaults, an enum default that is not a choice, a condition on an unknown/self/Real
key, an undeclared group) and is run over **every registered backend** by the test suite — the
cheapest possible guard against a future backend shipping a broken panel.

`available()` is probed at startup to populate the combobox — codec availability is build/runtime
dependent (`TIFFIsCODECConfigured()`, libavif's `codecChoice`, libheif presence).

### 2.2 `libmosaicformats` — hand-rolled, dependency-free codecs

A **standalone static library** (`src/formats/`, target `mosaic::formats`,
namespace `mosaicfmt`) that depends **only** on a minimal pixel-buffer view (its own tiny struct,
**not** `common::Image`), *not* on the rest of Mosaic — so it is independently testable and, in
principle, reusable/spinnable-out. Its purpose: **reduce third-party dependencies** by implementing
simple formats ourselves. Split across as many sessions as needed.

⚠ **The path above was corrected 2026-07-29.** It read `src/core/io/mosaicformats/` from the 2026-07-04
scoping until M5 was built. That was written when M1 had just relocated `src/io` → `src/core/io`; that
relocation was **reverted** by `97cca43` (pure I/O under `core/` inverts the dependency) and the code
lives at `src/io/`. A library that must depend on nothing cannot sit inside the target it is meant to
be independent of, and `src/io/mosaic/` is already taken by the **native `.mosaic` format** — an
unrelated thing a neighbouring `mosaicformats/` would be permanently confused with. Hence a top-level
`src/formats/`, which is what the name promised in the first place. The pixel-buffer view is the
library's own struct: taking `common::Image` would re-import the dependency §11 says it is tested
without.

| Always-on (Curated) | Exotic (flagged) |
|---|---|
| BMP (V4/V5, alpha, RLE, ICC), TGA (v2, RLE, straight/premul attr), PNM/PBM/PGM/PPM/**PAM**, **QOI**, ICO/CUR, Radiance **HDR** (RGBE) | PCX, XPM, XBM, SGI/IRIS, Sun Raster, Farbfeld, ICNS, CEL (KISS), PIX (Alias), FITS, C-source, C-header, HTML-table, MNG*, DICOM* |

\* MNG/DICOM are heavier; treat as "on demand" even within the exotic tier.

Heavy formats stay on system libs (they are not worth re-implementing and re-implementing a JPEG or
JXL encoder is out of scope). `libmosaicformats` is exposed to `core/io` through one adapter backend
that fans out by `FormatId`.

### 2.3 System-library backends (Arch `extra`, all C/C++, **zero Rust**)

| Format | Library | Arch pkg | License | Link |
|---|---|---|---|---|
| PNG | libpng (+ optional libspng) | `libpng` | libpng-2.0 | already linked |
| JPEG | libjpeg-turbo (+ optional mozjpeg for trellis) | `libjpeg-turbo` | BSD-3/IJG | already linked |
| **JPEG XL** | **libjxl** | `libjxl` | BSD-3 | `-ljxl -ljxl_threads` |
| WebP | libwebp (+ libwebpmux) | `libwebp` | BSD-3 | new |
| AVIF | libavif + **libaom** (or SVT-AV1) | `libavif` `aom` | BSD | new; `codecChoice=AOM`, **not rav1e** |
| TIFF | libtiff | `libtiff` | BSD-like | new |
| GIF | giflib (+ our own quantizer/dither) | `giflib` | MIT | new |
| OpenEXR | openexr + imath | `openexr` | BSD-3 | new (HDR tier) |
| JPEG 2000 | OpenJPEG (**Part-1 only**) | `openjpeg2` | BSD-2 | new |
| PDF / EPS / PS | cairo (+ PoDoFo for OCG layers) | `cairo` `podofo` | LGPL | new (vector tier) |
| SVG | **hand-emit XML** (no lib) | — | — | cairo's SVG surface rasterizes text — unusable |

⚠️ **Arch's `libavif` lists `rav1e` as a hard runtime dep**, so `pacman -S libavif` drops a prebuilt
Rust binary on the system even though we never invoke it. Force `codecChoice = AOM/SVT`; if we ever
vendor libavif, build with `-DAVIF_CODEC_RAV1E=OFF`. **Skip the Rust PNG optimizers**
(pngquant/libimagequant≥3, oxipng) — use optipng/zopfli if a post-pass is ever wanted.

### 2.3a As built (M4, 2026-07-28) — how the four new libraries are wired

All four are **optional**, on the libjxl pattern already in `src/io/CMakeLists.txt`: no `REQUIRED`,
a `MOSAIC_HAVE_<FMT>` compile definition when the probe succeeds, and the codec `.cpp` compiling an
inert stub when it does not. A distribution missing any of them still builds a working Mosaic —
the format is simply not offered, because the backend's `available()` answers the same probe and
`FormatRegistry::exportOrder()` skips an unavailable backend. Registration stays unconditional, so
a `.webp` path still resolves to a backend that can *explain* itself instead of to nothing.

| Library | Probe | Define | Notes |
|---|---|---|---|
| libwebp + libwebpmux | `pkg_check_modules(WEBP … libwebp libwebpmux)` | `MOSAIC_HAVE_WEBP` | the mux is not optional-within-optional: the bare library writes a simple-format bitstream with nowhere to put ICCP/EXIF |
| libavif | `pkg_check_modules(AVIF … libavif)` | `MOSAIC_HAVE_AVIF` | see the codec rule below |
| libtiff | `pkg_check_modules(TIFF … libtiff-4)` | `MOSAIC_HAVE_TIFF` | its *codecs* are probed again at runtime, per compression |
| giflib | `find_path`/`find_library` (**no `.pc` file exists**) | `MOSAIC_HAVE_GIF` | header + `libgif`, both or neither |

**The AVIF codec rule is enforced in code, not in packaging.** `src/io/avif.cpp` never lets
`AVIF_CODEC_CHOICE_AUTO` stand: it asks `avifCodecName()` for an AOM encoder, then an SVT encoder,
sets that choice explicitly on the encoder, and answers `avifSupported() == false` when neither
exists — so the format disappears from the combobox rather than being encoded by whatever the
distribution linked. Packaging cannot make this guarantee (Arch's `libavif` hard-depends on a Rust
encoder we never call); a runtime choice can, and it is observable in one place.

**libtiff's compressions are a per-build fact**, not a per-library one: JPEG, ZSTD and LZMA are all
individually optional inside libtiff. `io::tiffCompressionAvailable()` wraps
`TIFFIsCODECConfigured()`, and the TIFF backend **builds its Enum choice list from it** — the only
schema in the app that is not a compile-time constant. An unavailable compression is never offered,
and is refused with a reason if a stale preset asks for it anyway.

---

## 3. Format catalogue (three tiers)

Combobox: **Common** at top in popularity order, a divider, then **Curated + (if flag on) Exotic**
alphabetically. Height-locked + scrollable (reuse the existing custom `Dropdown` widget).

**Tier 1 — Common (top of list):** PNG · JPEG · **JPEG XL** · WebP · GIF · TIFF · AVIF · PDF · SVG
**Tier 2 — Curated pro (always on, below divider):** BMP · EPS/PostScript · ICO · JPEG 2000 ·
OpenEXR · PAM/PNM/PGM/PPM/PBM · QOI · Radiance HDR · TGA
**Tier 3 — Exotic (behind "Show all export formats"):** CEL · C-source/header · DICOM ·
Farbfeld · FITS · HTML-table · ICNS · MNG · PCX · PIX · SGI · Sun Raster · XBM · XPM · CUR/ANI
**Excluded:** HEIC/HEVC encode (parked, §12.1).

### 3.1 Capability matrix (abridged — drives the loss system)

Channels: G gray · GA gray+α · RGB · RGBA · Idx · CMYK.

| Format | Channels | Max depth / HDR | Alpha | ICC | Metadata | Lossy/Lossless | Anim |
|---|---|---|---|---|---|---|---|
| PNG | G,GA,RGB,RGBA,Idx | 1–16 int; no float | straight | ✅ | XMP,eXIf,text | lossless | APNG |
| JPEG | G,RGB,**CMYK** | 8 (12/16 turbo) | none | ✅ | EXIF,XMP,IPTC | lossy | — |
| **JXL** | G,GA,RGB,RGBA,**CMYK**,+extra | 1–32 int, **16/32 float** HDR | straight/**premul (records)** | ✅ | EXIF,XMP,JUMBF | **both** | ✅ |
| WebP | RGB,RGBA | 8 | straight | ✅ | EXIF,XMP | **both** (+near-lossless) | ✅ |
| AVIF | mono,RGB(YUV),α | **8/10/12** HDR | straight/**premul** | ✅ | EXIF,XMP | **both** | ✅ |
| TIFF | G,GA,RGB,RGBA,Idx,**CMYK**,Lab | 1–32 int, **16/32 float** HDR | assoc/unassoc | ✅ | EXIF,XMP,IPTC | **both** | multi-page |
| GIF | Idx | ≤8-bit palette | 1-bit | ✕ | comment/XMP | lossless LZW | ✅ |
| OpenEXR | arbitrary AOVs | **16/32 float** HDR core | premul convention | ✕ (chroma) | typed attrs | both | multi-part |
| Radiance HDR | RGBE/XYZE | shared-exp ≈ float HDR | none | ✕ | header vars | RLE | — |
| JP2 (Part-1) | G,RGB,RGBA,Idx,multi | 1–16 practical | straight/premul | ✅ | XML/EXIF/XMP | **both** (rev/irrev) | — |
| BMP | RGB,Idx | 1–32 | straight (V4/V5) | V5 | — | lossless (+RLE) | — |
| TGA | G,Idx,RGB,RGBA | 8–32 | straight/**premul (attr)** | ✕ | v2 ext | lossless (+RLE) | — |
| PNM/PAM | bilevel..RGB(+α PAM) | 1–16 | PAM straight | ✕ | comment | lossless | — |
| QOI | RGB,RGBA | 8 | straight | ✕ (lin/sRGB tag) | — | lossless | — |
| ICO/CUR | BMP/PNG entries | 1–32/entry | 8-bit or mask | ✕ | — | lossless | ANI |

Full per-format detail (all columns + every encoder option) is in Appendix A.

### 3.1a As built (M4) — what the caps rows actually claim

The table above is the **format**; `FormatCaps` is **this encoder**, and the two differ on purpose
(`src/io/caps.hpp`: "a field states what THIS BACKEND WRITES TODAY"). The gaps, so nobody has to
diff the code to find them:

| Format | Claims | Does **not** claim, and why |
|---|---|---|
| PNG | 8-bit RGBA, straight α, ICC, **eXIf + pHYs** (new in M4) | 16-bit, palette, APNG — the encoder writes colour-type 6 only |
| WebP | 8-bit RGB/RGBA, straight α, ICC, EXIF, both modes | animation (`WebPAnimEncoder` unused), XMP, and *chroma subsampling* — lossy WebP is always 4:2:0 with no ratio knob, so there is no control to warn about and `LossyEncode` already covers it |
| AVIF | 8-bit RGB/RGBA, straight α, ICC, EXIF, both modes, subsampling | 10/12-bit HDR (waits on the §5 float tap), premultiplied α, gain maps, sequences |
| TIFF | 8-bit RGBA, `Either` α (the file records which), ICC, DPI, **lossless only** | CMYK/Lab, 16/32-bit + float, multi-page (`layers = false`), **EXIF** (a TIFF stores it as a private sub-directory — a second write pass and an offset back-patch), and **JPEG-in-TIFF** |
| GIF | Indexed, 1-bit α, comment text, `maxColors = 256` | ICC, animation (one frame), and any per-*option* palette ceiling — `maxColors` is the format's, not the slider's |

> **Superseded in part by §7d (M5).** JPEG and JXL are absent from the table above because in M4
> they claimed no metadata and no ICC at all. Both now claim `icc` and `MetadataKind::Exif` (JPEG
> also `Dpi`), because both encoders really write them — see §7d for the per-container detail and
> for the one row that deliberately still says **no**, TIFF's EXIF.

**Why JPEG-in-TIFF is absent** (Appendix A lists it): it is the one TIFF compression that is both
lossy *and* alpha-dropping, and `FormatCaps` has no way to say "alpha survives, except under this
one option". Offering it would make the loss banner lie in the one place the plan says it must
never (§4). It waits for a caps model that can express a per-option capability — at which point
JPEG-in-TIFF, WebP animation and GIF animation all become expressible together.

**Two loss-diff corrections rode with M4**, both in `src/io/caps.cpp` and both about not lying:

- `kOptNearLossless` (new well-known key). libwebp's near-lossless is a preprocessing pass that
  runs *inside* the lossless mode, so `lossless = true` with `near-lossless < 100` is a lossy
  encode wearing a lossless label. `encodeIsLossless()` now checks it **before** the lossless flag.
- A **lossless encode no longer raises `ChromaSubsampled`.** AVIF forces 4:4:4 in its lossless
  mode and the schema hides the ratio there — but the value stays in the bag (hiding is a UI
  affordance, not a deletion), and `diff()` was reading it regardless. It now suppresses the
  warning when `encodeIsLossless()` holds, which is true by construction: a lossless encode cannot
  be subsampling anything.

### 3.2 Shipping constraints (hard rules on the catalogue)

- **Ship freely:** PNG, JPEG, JXL, WebP/VP8, TIFF, GIF/LZW, BMP, TGA, PNM, ICO, QOI, OpenEXR,
  Radiance HDR, JP2, DDS/BCn.
- **⚠ JPEG 2000 is Part-1 ONLY.** JPX and HTJ2K are deliberately out of scope and must not be added.
- **⚠ DDS/BCn: BC6H and BC7 only, and never a machine-learned mode-selection pass.** Encoder mode
  selection stays with the stock library heuristics.
- **AVIF/AV1 is INCLUDED**, shipped via `libaom` / `SVT-AV1`.
- **⚠ HEIC/HEVC is EXCLUDED.** Mosaic ships **zero** HEVC code and bundles no HEVC plugin; the only
  path is delegation to a codec the operating system already provides. Parked for its own design
  pass — §12.1 and `docs/heic-strategy.md`.

---

## 4. The loss-warning system (S41 core)

Pure, FLTK-free, golden-testable. Two data structs + one function:

```cpp
struct DocumentProfile {   // what the doc ACTUALLY uses (probed from the model)
  bool  hasAlpha, hasMultipleLayers, hasVector, hasText, hasEffects, hasExtrude3d;
  bool  hasICC, hasNonSrgbSpace;
  int   maxColorsIfIndexed;         // -1 = truecolor
  Precision precision;              // U8/U16/F16/F32 (intent; see §5 note)
  bool  usesConicGradient, usesStrokeAlign, usesBlendModes, usesSoftMask;
  bool  hasEXIF, hasXMP;
};
struct FormatCaps { /* channel models, max depth, alpha kind, ICC, metadata, anim, lossy/lossless,
                       maxColors, layers/pages, subsampling — see §2.1 */ };

enum class Severity { Fine, Lossy, HardLoss };
struct LossWarning { Severity sev; string feature; string consequence; };
vector<LossWarning> diff(const DocumentProfile&, const FormatCaps&, const OptionValues&);
```

**Doc-feature → model location** (for `DocumentProfile` extraction). The 2026-07-04 column was
stale by the time M2 built against it — the model has moved a lot — so it was **re-verified
line by line on 2026-07-27**; the numbers below are the current ones.

| Feature | Where (verified 2026-07-27) | Warn when target lacks it |
|---|---|---|
| Layers / tree | `core/document.hpp:90` `root()`; `layer.hpp:39` `LayerKind` | all raster (flatten) |
| Opacity / 23 blend modes | `layer.hpp:117` `opacity()`, `:120` `blendMode()`; `blend_mode.hpp:11` | EPS/PS (flatten) |
| Masks | `layer.hpp:46-63` `RasterMask`; `:164-188` accessors | flatten; GIF (soft → 1-bit) |
| Vector shapes / **conic gradient** / stroke align | `core/vector/object.hpp:12` `Object`; `vector/paint.hpp:35` `GradientType::Conic` ("no SVG 1.1 export"), `:77` `StrokeAlign`, `:86` `Stroke::align` | SVG (rasterize conic), all (stroke-align→outline) |
| Text / type | `layer.hpp:386` `TextLayer`; `text/text_model.hpp:165` `TextBlock` | raster (rasterize) |
| Layer Effects (stroke/overlay/shadow/glow/bevel/satin) | `layer_effects.hpp:39-157`; `LayerEffects::empty()` is the "anything enabled?" predicate | raster (bake) |
| 3D text | `core/text/extrude.hpp:60` `Extrude`; hung off `text_model.hpp:195` `TextBlock::extrude` | raster (bake) |
| Alpha | `common/image.hpp:51` `Image` (RGBA; there is no `hasAlpha` flag — it is **scanned**) | JPEG, GIF, BMP-baseline, PNM, HDR, QOI |
| Precision / bit depth | `document.hpp:28` `Precision{U8,U16,F16,F32}`, `:53` `precision()` | 8-bit-only formats |
| Color space / ICC / CMYK | `document.hpp:24` `ColorSpace`, `:51` `colorSpace()`, `:62` `iccProfilePath()`; `core/color_management.hpp:44` `ColorEngine` (lcms2) | GIF, BMP<V5, TGA, PNM, QOI, HDR |
| DPI | `document.hpp:55` `dpi()` | formats w/o density |

**Two traps the extraction had to design around:**

1. **There is no `hasAlpha` in the model.** `common::Image` is always RGBA. So
   `profileDocument(doc)` answers *conservatively* — "yes, unless some visible, unclipped,
   unmasked, effect-free, untransformed, full-opacity `RasterLayer` provably seals the whole
   canvas opaquely" (`documentIsProvablyOpaque`; alpha only ever grows as `over` composites, so
   one such layer settles it). The overload `profileDocument(doc, flattened)` replaces the guess
   with an exact scan, and *that* is the one the export pipeline uses, because §5 composites
   first anyway. The colour count (`distinctColors`, for the palette formats) is likewise only
   exact with the flatten, and bails out at a cap instead of hashing a photograph.
2. **A default document's `Precision` is `F16`.** Warning "HDR will be clipped" off `precision`
   alone would light the banner red on literally every export. So the profile carries
   `precision` (the document's declared *intent*) **separately** from `sourceBitDepth` /
   `sourceIsFloat` (what the pipeline can actually hand an encoder — 8 and false today, per the
   §5 high-bit note). `diff()` reads only the latter pair, so when the `ImageF` tap lands
   (S43-a) the extractor raises them and the depth warnings go live **with no change to
   `diff()`**.

**Severity → UI:** HardLoss (alpha dropped, layers flattened, HDR→8-bit, >256 colors quantized) is a
red banner; Lossy (quality<100, subsampling≠4:4:4) is amber; Fine is a quiet green check. The banner
updates live as the format/options change — the visible payoff of the caps system, and it teaches.

### 4.1 As built (M2, 2026-07-27) — `src/io/caps.{hpp,cpp}`, `src/io/document_profile.{hpp,cpp}`

Five decisions the sketch above did not settle, all of them load-bearing:

- **`Severity::Fine` is never carried by a warning.** `diff()` returns *only* Lossy and HardLoss
  entries; an **empty vector is the green check**, and `worstSeverity({})` answers `Fine`. That is
  what lets a golden test assert an exact ordered set instead of filtering noise out first.
- **The severity line is "picture or editability" vs "written lossily / side-car lost".** Red:
  alpha dropped or crushed to 1 bit, layers flattened, vector/text/effects/3D baked, depth or HDR
  reduced, colours quantized, blend modes flattened into a vector target. Amber: lossy encode,
  chroma subsampling, ICC/working-space dropped, EXIF/XMP dropped, DPI dropped, stroke alignment
  outlined (it reproduces exactly, it just stops being a stroke).
- **`FormatCaps` states what the BACKEND WRITES, never what the container permits.** PNG's
  container has APNG, 16-bit, iCCP/eXIf/pHYs; `io::encodePng` writes an 8-bit RGBA image and
  nothing else, so `PngBackend::caps()` reads `maxBitDepth = 8, animation = false, icc = false,
  metadata = None`. The banner is the one surface in the app that must never over-promise, and it
  is a checked invariant (`tests/test_export_registry.cpp`). Each field flips on as the encoder
  earns it.
- **A shared option vocabulary is how `diff()` stays format-agnostic.** A backend with one of
  these concepts must spell it this way: `quality` (Int 0–100, higher better), `distance` (Real
  ≥ 0, butteraugli, 0 = lossless), `lossless` (Bool), `subsampling` (Enum of `"4:4:4"`,
  `"4:2:2"`, `"4:2:0"`, `"4:4:0"`, `"4:1:1"`). `encodeIsLossless()` reads them in that
  precedence. **A format with no lossless mode is never lossless — not even at quality 100**,
  which is still DCT-quantised; §4's "quality<100" is the amber *trigger*, not the underlying
  fact, so a JPEG export is amber at every quality. That is a deliberate, honest deviation.
- **Warning text is untranslated English + a stable `LossCode`.** `core/io` is gettext-free and
  translating inside `diff()` would make the goldens locale-dependent; the M3 banner translates
  by code and the strings are the fallback and the test oracle.

---

## 5. The Export render pipeline

**Source of truth:** `render::composite(doc, opts, backend)` (`render/compositor.cpp:988`) →
`common::Image` (8-bit straight-alpha RGBA), `checkerboard=false` so real alpha survives. This is the
whole-doc flatten — the export render calls it directly.

**Three-stage cache** (keeps the "real render" snappy):

```
composite(doc)         ─ invalidated by any doc edit
   → resize(outW,outH,AA)   ─ invalidated by output-size / AA change
      → encode(format,opts) ─ invalidated by format / option change  ⇒ preview bytes + exact size
```

Dragging a quality slider only re-encodes; changing output size only re-resizes. The **encode stage
is the single source of both the preview image and the exact file-size number** (a real trial-encode
to a memory buffer — consistent with "actual render, not estimate"; debounced + cancellable so a
100 MP JXL at effort 9 doesn't re-encode on every slider tick).

**Two dependencies to pull forward:**

1. **`render::resizeImage(src, outW, outH, ResampleFilter)`** — the kernels exist but are file-local
   in `compositor.cpp` (`sampleTransformed`/`resampleInto`/`lanczosKernel`, coupled to affine layer
   placement). Promote a clean arbitrary-resize entry point to a header. This is a **slice of
   S53-a**; the export "resize AA" dropdown reuses `render::ResampleFilter`
   (`Auto,Nearest,Bilinear,Bicubic,Mitchell,Lanczos2,Lanczos3,Area,Gaussian,Supersample`,
   `render/render.hpp:32`) and its Auto-resolver (`chooseAutoFilter` already picks Area on minify) —
   verbatim reuse of the Move-tool selector. **Auto is the default.**

   > **Still owed at the end of M2** — deliberately not built there, because
   > `render/compositor.cpp` is being rewritten concurrently for GPU residency (S60-a). The
   > signature the export pipeline wants, to go beside `chooseAutoFilter` in
   > `render/compositor.hpp:93`:
   >
   > ```cpp
   > [[nodiscard]] common::Image resizeImage(const common::Image& src, std::uint32_t outW,
   >                                         std::uint32_t outH,
   >                                         ResampleFilter filter = ResampleFilter::Auto);
   > ```
   >
   > Semantics: 8-bit straight-alpha RGBA in and out; sampling runs in **premultiplied** alpha and
   > un-premultiplies on the way out (the compositor's own rule, so a resize cannot fringe a soft
   > edge); `Auto` resolves through `chooseAutoFilter(Affine2D::scaling(outW/srcW, outH/srcH),
   > /*liveDrag=*/false)`; an empty source or a zero output dimension returns an empty `Image`;
   > `outW == src.width && outH == src.height` returns a bit-exact copy whatever the filter.
   >
   > **`core/io` needs no seam for it.** Module deps run `io → core`, never `io → render`, and
   > §2.1's contract already says `encode()` is handed *"the finished, resized buffer"* —
   > `io::RenderInput::pixels`. So the hookup is one line in M3's export pipeline, between the
   > composite and the encode: `img = render::resizeImage(composited, outW, outH, filter);`.
   >
   > **STILL OWED after M3 (2026-07-28).** `render/**` was still owned by the S60-a compositor
   > rewrite, so M3 did not add it either. The export pipeline calls exactly one function —
   > `resizeForExport(src, outW, outH, render::ResampleFilter)` in `ui/export_dialog.cpp`, marked
   > `THE render::resizeImage SEAM` — whose body is the **one line to swap**. Until then the seam
   > honours the contract with what it can do locally: `Auto` resolves through the **real**
   > `render::chooseAutoFilter`, `Nearest` point-samples, an exact-size request returns the source
   > unchanged, and every smooth kernel falls to a local premultiplied triangle filter. So the
   > **resize-quality dropdown is only partly live**: Auto and Nearest are genuinely different,
   > Bilinear…Supersample all currently produce the triangle result. Wiring `render::resizeImage`
   > makes the whole dropdown real with no other change.
2. **Color-on-export (lcms2)** — convert-to-sRGB / embed working profile / assign, via the existing
   `core::ColorEngine`. A "Color" row in the modal.

**High-bit note (gates the HDR tier):** `composite()` collapses to 8-bit at `toImage8Parallel`
(`compositor.cpp:981`). OpenEXR / Radiance HDR / float-TIFF / HDR-JXL are only *meaningful* with
>8-bit pixels, so they need a small **"tap the `ImageF` accumulator before 8-bit conversion"** slice
(a sub-slice of S43-a). Offer these formats only once that tap exists; before then they would write
8-bit-upsampled data (pointless). Sequence them into the **HDR tier**, after milestone 1–3.

**Async:** an `ExportJob` cloned from the `InpaintJob` template (`app_window.cpp:4292`): worker
`std::thread` runs composite→resize→encode, publishes progress/preview under a mutex + `atomic done`,
UI polls from a re-armed `Fl::add_timeout` off `onFrame`. The worker **never touches FLTK**. Cancel =
`atomic cancelRequested` + join (RAII). Same pattern as InpaintJob / RecomposeJob / SpellCheckWorker.

---

## 6. The Export… modal (professional **and** intuitive)

> User requirement: *user-friendly and intuitive while remaining professional.* The levers below are
> how the modal serves a beginner and a pro from the same surface.

**Layout:** big left pane = the **actual async render** on a checker bg (fit / 1:1 toggle,
zoom/pan, "rendering…" spinner while the 3-stage pipeline recomputes; last good render stays visible
during recompute). Slightly slimmer right pane = scrollable settings, top-to-bottom:

1. **Presets** (Web PNG · Web JPEG · Print PDF · Lossless Archive · …) — one click sets
   format+options. Beginners never open the advanced panel; pros ignore presets. (Pulls the
   friendliness lever of S56.)
2. **Format** — the height-locked scrollable combobox (§3). Switching format re-renders the loss
   banner instantly.
3. **Output size** — radio **% / px**; a percentage slider; two linked value fields (W/H, aspect
   lock toggle); **changing a field flips the radio to it**; a DPI field. Feeds `resize()`.
4. **Resize AA** — dropdown = the `ResampleFilter` range incl. **Auto** (default). *For the resize
   only* — distinct from the canvas/Move-tool AA (which is chosen in the Move tool).
5. **Backend settings** — its own scrollable sub-panel, **rendered from the backend's
   `OptionsSchema`**. Common knob (quality) always visible; **"Advanced" collapsed by default**
   (reuse the Type panel's disclosure widget) so the panel is uncluttered but "as advanced as they
   get" is one click away.
6. **Color** — convert-to-sRGB / embed profile (lcms2); **Matte** color for alpha-less formats
   (what fills transparency in JPEG/etc.); **Metadata** toggle (strip/keep EXIF/XMP/ICC — privacy +
   size).
7. **Info block** — output dimensions, **exact expected file size** (from the trial encode), color
   model / bit depth, and the **live loss-warning banner** (§4).
8. **Path row** — a text field + **`[...]`** button (opens the system picker to set it), **empty by
   default**.
9. **Export** / Cancel.

**Path behavior (your empty-default idea, refined):** empty on first export → clicking **Export**
opens the system Save dialog, writes, closes the modal. After the first export, **remember
path+format for that document's session** (so re-export is one click) but **never leak the path to
the next document** (the annoyance to avoid). This enables the GIMP-style **"Export to `foo.png`" /
"Overwrite"** menu items = one-click re-export, no dialog. Per-document export state is persisted
into `.mosaic` at S48.

### 6.1 As built (M3, 2026-07-28) — the path policy, and the two bugs it closes

Two user reports drove this out of "a few lines in the modal" and into its own pure, unit-tested
module, `src/io/export_path.{hpp,cpp}` (`tests/test_export_path.cpp`):

1. **The default export path was the process working directory** — wherever the binary happened to
   be launched from. Cause: *no caller ever set a start folder*, and the modal's path field was
   pre-filled with a BARE FILE NAME (`untitled.png`), so a plain `Export` wrote a relative path.
   Each picker backend then invented its own answer for the missing folder, and FLTK's kdialog
   driver literally `getcwd()`s (`Fl_Native_File_Chooser_Kdialog.cxx`: `preset = (directory ?
   directory : getcwd()) + "/" + preset_file`).
2. **`http://asdf.png` in the KDE save picker's name field** — see §8.1a.

**The policy** (`io::exportStartFolder`, pure): the directory of this document's **last export** →
the directory the **document itself** came from → the OS **pictures** folder (documents, for a
`.mosaic` save/open) → `$HOME` → nothing. A candidate that is **not absolute is skipped, never
resolved**, because resolving a relative one means resolving against the working directory — the
bug. `resolveExportPath(typed, folder)` applies the same rule to a hand-typed field value. The
test asserts directly that the cwd is unreachable from every input shape.

**Where the memory lives:** `io::ExportTarget{path, formatId, values}` on the app's per-document
session (`MainWindow::m_exportTarget`, spilled into `DocumentSession` on a tab switch, cleared by
`presentDocument`) — **app state, never a sidecar beside the user's file** (the standing project
rule). `presentDocument`'s one-line `m_exportTarget.clear()` IS the "never leaks to the next
document" guarantee. It also carries the **option values**, so "Export to `<file>`" repeats the
encode the user chose rather than silently reverting a JPEG to the default quality; §6 only asked
for path+format, and that would have been a worse lie than no re-export.

**Menu:** `File ▸ Export to <file>` was added beside `Export As…`, greyed (labelled "Export to Last
File") until this document has been exported once. Its label + enabled state are reconciled from
`updateWindowTitle()` — a string compare unless the target changed — so every path that switches,
opens or closes a document keeps it correct with no hook of its own. A **Quick Export also sets the
target**, so `Ctrl+Alt+E` then `Export to …` works.

---

## 7. Milestone 1 — Quick Export → PNG

> **✅ DONE 2026-07-04 (commit `4b692c5`).** 680 tests green (debug/release/asan); the live X11
> title reads through and a real PNG lands on disk. The portal dialog itself can't be tested
> headless — it wants the interactive X11 + Wayland pass. As-built notes inline below.

The smallest real end-to-end slice; builds the foundation everything else reuses.

1. ❌ ~~Relocate `src/io → src/core/io`~~ — **this ✅ was wrong; the move was never made** (found
   2026-07-27 while building M2). It is now also the wrong move — see §0 and §2.1. The include
   path is and stays `io/io.hpp`.
2. ✅ First **encoder**: `io::savePng(image, path, opts)` via libpng (already linked); straight-alpha
   RGBA, compression + interlace options (`PngSaveOptions`). *As-built:* **8-bit only** — the source
   is the 8-bit `common::Image` flatten, so 16-bit waits for the >8-bit tap (§5 high-bit note / the
   HDR tier). Uses libpng's **full write API** (not the simplified one) so the compression/interlace
   knobs are real; the row-pointer table is built **before** `setjmp` so a libpng longjmp can't leak
   it. Lossless ⇒ encode→decode is bit-exact.
3. ✅ The **shared native Save dialog** (§8) — Quick Export = system Save dialog titled "Export";
   the same `platform::FileDialog` **also now backs `File → Open`**, fixing the GTK-picker complaint.
4. ✅ Wire `File → Quick Export as PNG` (`Ctrl+Alt+E`) → `composite()` (CPU, `checkerboard=false`,
   the canvas's own resample filter) → `savePng()`, path picked via the portal. *As-built:* the
   menu also relabels the modal item to **`Export As…`** (still `cbTodo`, the M3 modal) and stamps
   `doc.setFilePath(path)` on Open (so the window title shows the filename + extension, S18-d). An
   export is **not** a Save — it never touches the command-stack saved marker (§9).
5. ✅ Headless goldens (`tests/test_io.cpp`): savePng round-trip (bit-exact) + a compression/interlace
   round-trip + the full **open→edit→composite→export→reload** pipeline (asserts the reload matches
   the composited bytes). *As-built:* driven directly in the test with the command stack, not through
   the op-runner (the op-runner has no window; the pipeline is exercised end to end regardless).

Verifiable win: a real PNG on disk from a real edit, plus a native KDE dialog. **Delivered.**

---

## 7b. EXIF READ slice (landed early, 2026-07-16)

The read half of this arc's metadata story, pulled forward of M2 because the sky generator's
"Estimate from layer" needs `FocalLengthIn35mmFilm` (FOV is unrecoverable from pixels) and can
prefill its date & place from `DateTimeOriginal` + GPS. Deliberately **not** the S41/S42
framework: one typed struct, plumbed end to end.

- **What's read** (`common::ExifData`, `src/common/exif.hpp`): Orientation (274), FocalLength
  (37386), FocalLengthIn35mmFilm (41989), DateTimeOriginal (36867, validated calendar date),
  GPSLatitude/Longitude + refs (→ signed decimal degrees, range-validated), Make (271), Model
  (272). Every field `std::optional`; a malformed tag stays absent, never guessed.
- **Parser** (`src/io/exif.{hpp,cpp}`): hand-rolled TIFF/IFD walk — hand-rolled over vendored
  because the tag set is tiny and a ~350-line parser can be exhaustively hostile-tested
  (`tests/test_exif.cpp`). Caps: 1 MiB payload, 512 entries/IFD, 128-byte printable-ASCII
  strings. Visits at most IFD0 → Exif IFD + IFD0 → GPS IFD (no recursion, next-IFD pointers
  never followed → offset loops cannot hang it); every offset/count is bounds-checked before
  any read; nothing allocates in proportion to a declared count. Containers: JPEG APP1
  `Exif\0\0` (segment scan) + PNG `eXIf` (chunk walk, standardized 2017).
- **Where it lands**: `io::loadImageWithMetadata` returns pixels + optional `ExifData`
  (`loadImage` = the same with metadata dropped); the source layer carries it
  (`core::Layer::exif()`/`setExif`, base chrome beside effects); the `.mosaic` manifest
  persists it as the optional per-layer `"exif"` node (schema 1, additive-growth rule: absent
  = none, present-but-malformed = refused) with the same value contracts as the parser.
- **Orientation is baked at load**: sideways-shot photos arrive upright (the 8 transforms in
  `io::applyExifOrientation`), and the stored metadata then records orientation 1 — the pixels
  ARE upright, re-applying would be a bug. No toggle (strictly-better rule).
- **Deferred to S41/S42**: EXIF write-back on export, full metadata preservation (XMP/IPTC,
  unknown-tag passthrough), the strip-metadata privacy toggle (§6), TIFF-container EXIF.

### 7c. EXIF WRITE + ICC embedding (M4, 2026-07-28) — the owed half

`src/io/exif_write.{hpp,cpp}` serialises `common::ExifData` back into the one wire format every
container wants: a little-endian TIFF header, IFD0, an Exif sub-IFD and a GPS sub-IFD. The **same
payload feeds every backend** — a PNG `eXIf` chunk, a WebP `EXIF` mux chunk, an AVIF Exif item all
store exactly these bytes (a JPEG APP1 would additionally need the `Exif\0\0` prefix, which is the
container's job, not the serialiser's).

The contract it is tested against is a round trip through the **existing reader**:
`parseExif(buildExifPayload(d)) == d`, up to the wire format's own resolution (focal length in
thousandths of a millimetre, GPS seconds in ten-thousandths). Its range tests deliberately **mirror
`io/exif.cpp`'s**, field for field: writing a value our own reader would drop is a silent
round-trip hole, so an out-of-range field is simply not written, and a record with nothing valid in
it produces an *empty* payload, which every caller reads as "write no metadata".

Plumbing: `io::EmbeddedMetadata { exif, icc, dpi }` rides in each `*SaveOptions`;
`io::buildMetadata(RenderInput)` (`format_registry.cpp`) is the single translation point, so §6's
**strip-metadata toggle is honoured in exactly one place** rather than once per backend.
`io::readIccProfile()` validates a profile structurally (declared size matches the file, `acsp` at
offset 36) before any of it reaches a codec — "the user picked a file" is not evidence that it is a
colour profile. Where a codec validates further and refuses, the **chunk is dropped and the picture
still ships**: libpng rejects a non-RGB profile for an RGBA PNG as a hard `png_error`, so
`writePngMetadata` brackets `png_set_iCCP` in `png_set_benign_errors`, and losing a profile stays a
metadata problem instead of becoming a failed export.

**Still deferred:** XMP/IPTC, unknown-tag passthrough, and EXIF *reading* from the four M4
containers (each needs its own container walk and its own hostile-input suite; the write half
already serves all of them).

### 7d. Metadata + ICC, END TO END (M5, 2026-07-29) — the two controls that were lies

M4 built the payload (`exif_write`, `readIccProfile`, `EmbeddedMetadata`, PNG's three chunks) and
then wired **nothing** to it. `ExportRequest` carried no `common::ExifData`, `RenderInput`'s ICC seam
was unread app-wide, and both the modal's Metadata toggle and its Colour row therefore described
behaviour that did not exist. M5 closed that. Five decisions carry it:

**1. Where the exported EXIF comes from — the provenance rule.** EXIF lives *per layer*
(`core::Layer::exif()`, stamped by the open path) and an export is a *flatten of many layers*, so
this is a decision the model does not make for us. `io::documentExif(doc)`: among the layers that
could be where a photograph ENTERED this document — a **Raster or Magic** layer, the only two kinds
`loadImageWithMetadata` ever stamps — that are **effectively visible** (own flag *and* every
enclosing group's) and carry a non-empty record, take the one with the **smallest layer id**.
Nothing qualifies ⇒ no EXIF is written.

Smallest id = *earliest minted* (ids are monotonic and never reused), so for the ordinary case —
File ▸ Open on a photo, which creates exactly one layer before any edit can happen — it names
precisely the layer the document was opened from. It was chosen over the obvious alternative, *the
bottom-most visible one*, because stack **position** is something a user rearranges freely in the
layer dock, and dragging a layer must not silently rewrite which camera the exported file claims to
have come from. Visibility is required for the opposite reason: metadata describing a photo that
contributes nothing to the exported pixels is worse than none.

**2. Orientation is always written as 1.** `io::exifForExport()` forces it, and it is a *correctness*
rule, not a nicety: the load baked the rotation into the pixels (§7b), so writing the original 6
back out tells every reader to rotate an already-upright picture a second time — the class of bug
that only ever appears in somebody else's viewer. An **absent** orientation stays absent (an export
must not invent metadata, and absent already means 1). Asserted in `tests/test_export_metadata.cpp`,
both on the record and through a real container.

**3. `RenderInput` carries the profile as BYTES, not a path.** The field was
`std::string iccProfilePath`; it is now `std::vector<std::uint8_t> iccProfile`. Two reasons, and both
are structural: the profile a document exports is **not always a file** — a working space with no
custom `.icc` is serialised out of lcms2 in memory (`core::documentIccProfile`, new) — and the trial
encode behind the size readout runs on a **worker thread**, off a debounced keystroke, which is the
wrong place to re-read a two-megabyte press profile and a worse place to discover its path stopped
resolving. Resolve and validate once, at the UI; hand the bytes down. `io::readIccProfile()` remains
the file→bytes validator, and `core::documentIccProfile()` is the document→bytes one (which
additionally *parses* through lcms2 and rejects a non-RGB profile — a real parse, not the two
structural checks a dependency-free reader can afford).

A **plain sRGB document embeds nothing**, deliberately: sRGB is what an untagged file is read as
everywhere, so three kilobytes to say so in every export is cost with no benefit. Non-sRGB working
spaces embed a *built* profile (same primaries + curve the picker converts through, so the file and
the numbers can never disagree), with the description tag overwritten so another application shows
"Display P3" rather than lcms2's generic "RGB built-in".

**4. The loss banner reports what the FORMAT cannot carry, never what the USER chose to omit.**
`diff()` gained a fourth, defaulted argument, `io::MetadataRequest{keepMetadata, embedIcc}`. Dropping
EXIF the user asked to strip is not a loss — it is the request being honoured — and a banner that
warned about it would teach the user that the banner is noise, which is the one thing §4 says must
never happen. Because the defaults are "carry everything", the struct can only ever *remove*
warnings, so every M2 golden still holds unchanged. (The modal also answers `hasICC` from *the bytes
the encoder will actually be handed*, not from `Document::iccProfilePath()`: a user may pick a
profile for an export of a document that has none of its own, and the banner has to know.)

**5. A control the format cannot honour is disabled WITH A STATED REASON.** The modal's Colour &
metadata section is reconciled entirely from `FormatCaps` (`refreshMetadataSection`): a row that
means nothing is hidden (the matte, for a format that has alpha), a row that means something the
format cannot do is shown, greyed, and says why in its own note. Nothing in `ui/` names a format —
it could not; the dialog has no way to ask. The metadata note is assembled from caps *bits*
("Stores: camera and lens data · cannot store: the print resolution"), so a backend that earns a
bit starts appearing there with no UI change at all. The profile row's item labels are fixed source
strings and the profile's real *name* goes in the note beneath, because a profile description
routinely contains a `/` ("Coated FOGRA39 / ISO 12647-2") and `Fl_Menu_` reads a `/` in an item
label as a submenu separator — pasting a name into a menu item is exactly how that standing trap
fires.

**Per container, what M5 actually writes** (and the caps bits that flipped with it):

| Format | EXIF | ICC | Density | Note |
|---|---|---|---|---|
| PNG | `eXIf` (M4) | `iCCP` (M4) | `pHYs` (M4) | unchanged; the benign-error bracketing stays |
| **JPEG** | **`APP1` `Exif\0\0`** | **`APP2` `ICC_PROFILE\0`, multi-segment** | **JFIF density** | `icc`, `Exif`, `Dpi` all newly true |
| **JXL** | **`Exif` box** | **`JxlEncoderSetICCProfile`** | — | `icc`, `Exif` newly true; no `Dpi` (that is the codestream's intrinsic size, which we do not set) |
| WebP | `EXIF` mux chunk (M4) | `ICCP` mux chunk (M4) | — | unchanged |
| AVIF | Exif item (M4) | `colr` (M4) | — | a payload libavif refuses now costs the payload, not the export |
| TIFF | **no** — see below | `TIFFTAG_ICCPROFILE` (M4) | resolution tags (M4) | unchanged |
| GIF | no | **cannot, ever** | no | `IccDropped` is a real warning, not a silent no-op |

**The JPEG multi-segment split is the part that is easy to get wrong.** A segment's length is a
16-bit word that counts itself, so the payload ceiling is 65533 bytes — and an ICC profile
*routinely* exceeds one segment (a display profile is a few kilobytes; the vendored press profile is
1.8 MB). The spec's answer is a numbered sequence of `APP2` segments, each repeating the 12-byte
identifier and carrying a **1-based** chunk number plus the total count. Writing one oversized
segment instead does not fail loudly — it produces a file whose profile no colour-managed reader
will use. The count byte is 8 bits, so 255 segments is the hard ceiling; a profile past it is
dropped rather than truncated, because half a profile is worse than none. The test reassembles the
sequence the way a reader does and compares byte for byte.

The segments are **spliced into the finished bitstream** rather than written by the encoder, because
the app talks to libjpeg-turbo through its `tj3` API, which has no marker-writing entry point at all
(`jpeg_write_marker` belongs to the lower-level libjpeg API this module does not link). That is not
a workaround: a marker segment is position-independent by construction, and inserting one is exactly
what every metadata tool does to a JPEG that already exists. Insertion point: immediately after the
SOI, past a leading JFIF `APP0`.

**Why TIFF still has no EXIF, restated with the new reason.** §3.1a already recorded the cost (a
TIFF carries EXIF as a *private sub-directory* — `TIFFCreateEXIFDirectory` / `TIFFTAG_EXIFIFD` —
which means a second write pass, a `TIFFWriteCustomDirectory`, and a `TIFFRewriteDirectory` to
back-patch IFD0). M5 adds the decisive one: **it cannot be tested here.** Our own `extractExif`
walks PNG and JPEG only, and libtiff is a PRIVATE dependency of the `io` module, so a test cannot
link `tiffio.h` to read the sub-directory back. Writing an untestable, fiddly, offset-back-patching
pass into the one code path that must not corrupt a user's file is the wrong trade. The honest
alternative — teaching `extractExif` to treat a whole TIFF file as an EXIF payload, which it nearly
is — needs `parseExif`'s 1 MiB payload cap raised, and that cap is load-bearing in a hostile-input
parser. So the caps row keeps saying no, `diff()` keeps raising `ExifDropped`, the modal's note says
"cannot store: camera and lens data", and a test asserts the *bytes* agree with the claim in both
directions. That is the whole point of the caps architecture: an absent capability is a stated
capability.

**Still deferred after M5:** XMP/IPTC, unknown-tag passthrough, EXIF *reading* from the four M4
containers, TIFF EXIF write (above), and JXL intrinsic dimensions (the density it would take to
claim `MetadataKind::Dpi`).

---

## 8. Native file dialog (fixes Open; powers Export/Quick/Save)

> **✅ DONE 2026-07-04 (commit `4b692c5`)** as `platform::FileDialog` (`src/platform/file_dialog.*`):
> `showSaveDialog` / `showOpenDialog` + `initNativeFileDialog` (the Phase-0 call, made once at
> start-up). Both phases below shipped. sd-bus comes from `libsystemd` (PRIVATE to the platform
> module). The parent handle is read via the existing `nativeSurfaceHandle` (X11 xid → `x11:<hex>`)
> so the TU needs no Xlib include; native Wayland passes `""` (the `zxdg_exporter_v2` handle is
> still deferred — the dialog opens unparented). The Response is pumped through `Fl::add_fd` inside a
> nested `Fl::wait()` loop, so the call is synchronous to the caller but keeps the canvas painting;
> a portal that is unreachable (no session bus / method error) falls back to `Fl_Native_File_Chooser`.

FLTK lands on the GTK picker because its zenity/kdialog backends are opt-in and GTK is default-on.
Two phases:

- **Phase 0 (stopgap + fallback, 1 line):** `Fl::option(Fl::OPTION_FNFC_USES_KDIALOG, true)` on KDE
  → real Plasma dialog immediately. Kept as the fallback when the portal is unreachable.
- **Phase 1 (proper, shared `platform::FileDialog`):** call the **XDG Desktop Portal FileChooser**
  (`org.freedesktop.portal.FileChooser` `OpenFile`/`SaveFile`) over **sd-bus** (already present on
  every systemd box; no GTK/GLib; matches the existing `gdbus` portal precedent in
  `platform/system_theme.cpp`). Works identically on XWayland + native Wayland + GNOME + KDE.

**Call flow (SaveFile):** build `parent_window` — `"x11:"+hex(fl_x11_xid(win))` on X11/XWayland,
`"wayland:"+handle` via `zxdg_exporter_v2` on native Wayland (async; pass `""` if not implemented yet
— dialog still opens); random `handle_token`; `sd_bus_add_match` on the Request's `Response` signal
**before** the call; `SaveFile(parent, "Export", {accept_label:"_Export", modal:true,
current_name:"untitled.png", filters:[("PNG",[(0,"*.png"),(1,"image/png")]),("All files",[(0,"*")])]})`;
pump via `Fl::add_fd(sd_bus_get_fd(bus))`; on `Response` (0=ok,1=cancel,2=other) decode `uris[0]`
(URI → path; may be a `/run/user/<uid>/doc/...` FUSE path — always URI-decode). Title, accept label,
default name, filters all set through the portal options dict. Never block the FLTK loop.

This one dialog powers Quick Export, the modal's `[...]`, Open, and future Save.

### 8.1a The `http://asdf.png` bug (fixed 2026-07-28) — and the start-folder contract

**Report:** on X11, opening the save picker showed `http://asdf.png` in the file-name field.

**Cause, exactly.** On X11 + KDE the picker is our directly-spawned `kdialog --attach` (§8.1 part
3). `runKdialogAttached` built its positional start-location argument as
`startFolder + suggestedName`, and **no call site ever set `startFolder`** — so kdialog was handed
the bare string `asdf.png`. kdialog resolves that argument with **`QUrl::fromUserInput()`**, whose
order is: absolute path → `QUrl::fromLocalFile`; otherwise (with no working directory supplied,
which is kdialog's case) the *"the user typed a web address"* heuristic, which prepends `http://`
to anything that parses as a host. `asdf.png` parses as a host, so the URL became
`http://asdf.png` and the dialog pasted it into the name field. Verified against Qt 6 directly:
`QUrl.fromUserInput('asdf.png')` → `http://asdf.png`; `QUrl.fromUserInput('/home/u/asdf.png')` →
`file:///home/u/asdf.png`. The other old spelling, `"."` (the no-name case), is no better:
`QUrl.fromUserInput('.')` is an **empty** URL, so the start location was silently dropped — and
`.` meant the working directory in the first place.

**Fix — all three backends, both directions.** `platform::FileDialogRequest` now has a hard
contract: `suggestedName` is a **base name** and `startFolder` is an **absolute directory**; the
two are never concatenated by a caller. `file_dialog.cpp` enforces it:

- `baseNameOf()` reduces any path or URI-looking `suggestedName` to its last component before it
  reaches the portal's `current_name`, kdialog's argument, or `preset_file`;
- `absoluteStartFolder()` returns the request's folder when absolute, else `$HOME`, **never
  `getcwd()`** and never `.`;
- **kdialog** (Open *and* Save) now gets one absolute path — `<dir>/` for an Open, `<dir>/<name>`
  for a Save — which takes `QUrl::fromUserInput`'s `isAbsolutePath` branch and cannot be
  reinterpreted;
- the **portal** now also sends `current_folder` (type `ay`, a NUL-terminated byte array — not a
  string and not a URI), so it obeys the same policy instead of its own remembered default;
- the **`Fl_Native_File_Chooser` fallback** now always sets `directory()`, which is what stops
  FLTK's kdialog driver reaching its `getcwd()` branch. And when the portal is unavailable **on
  KDE** we now spawn our own (unattached) kdialog first, because FLTK's driver also drops the
  start location entirely for an Open and passes it to the shell unquoted.

Every `showOpenDialog` / `showSaveDialog` call site in `app_window.cpp` now sets `startFolder` from
the §6.1 policy.

**Still needs the user's interactive pass** (a picker cannot be exercised headlessly): X11 + KDE
(the kdialog path — the actual bug), X11 + non-KDE and Wayland (the portal path, where
`current_folder` is new), and the native-chooser fallback (`XDG_SESSION_TYPE` with no portal).

### 8.1 Modality (why the picker is truly modal — and why it takes three parts)

User report: the Open/Save pickers were only *"more or less"* modal — they floated and the main
window still reacted. The XDG portal **always runs the file dialog out of process** (xdg-desktop-
portal-kde spawns it); you cannot avoid that with the portal, so "an extra dialog process" is
inherent, not a Mosaic quirk. What we *can* control is whether that out-of-process dialog is truly
modal. It takes three cooperating pieces, and no single one is sufficient.

**How Qt/Krita do it (studied from source).** Krita just opens an in-process `QFileDialog`
(`KoFileDialog` → `KisPreviewFileDialog`), passes the main window as parent, sets `Qt::WindowModal`
for Save/Import, and blocks via `exec()` — it adds **no** Wayland/portal/kdialog code
(`libs/widgetutils/KoFileDialog.cpp:138-254`, `:301`). Everything else is delegated to Qt. Qt's
portal integration then:

- builds the parent token exactly the way we do — `"x11:"+hex(winId)`
  (`qtbase qxcbintegration.cpp:576`) or `"wayland:"+<xdg_foreign handle>` obtained by
  `zxdg_exporter_v2.export_toplevel` + a blocking round-trip
  (`qwaylandplatformservices.cpp:42-52`, `qwaylandxdgshell.cpp:684-704`);
- passes `modal: windowModality != NonModal` in the FileChooser options
  (`qxdgdesktopportalfiledialog.cpp:169`);
- but for the **portal (out-of-process) dialog Qt does NOT block its own parent** — `QDialogPrivate::
  setVisible` early-returns the moment the native (portal) helper takes over
  (`qdialog.cpp:771-772`: `if (... canBeNativeDialog() && setNativeDialogVisible(visible)) return;`),
  *before* the widget-level modal machinery that would disable other windows ever runs; the helper's
  own `exec()` is only a nested `QEventLoop` (`qxdgdesktopportalfiledialog.cpp:402-413`, the token
  passed via `portalWindowIdentifier(parent)` at `:276`). So Qt relies entirely on the compositor
  honouring `parent_window`+`modal`. (It only issues `xdg_dialog_v1.set_modal` for its *own*
  in-process toplevels, `qwaylandxdgshell.cpp:49-59` — you cannot call it on a surface you don't own,
  which the portal's dialog is.)

That last point is the crux. The `xdg-dialog-v1` protocol spec says outright: *"Clients must
implement the logic to filter events in the parent toplevel on their own. Compositors may choose any
policy in event delivery to the parent toplevel, from delivering all events unfiltered to using them
for internal consumption."* So `parent_window`+`modal:true` is **necessary but not sufficient** — a
spec-compliant compositor is allowed to keep feeding your parent events. This is *why* a Qt app's
picker can itself float non-modal on Wayland+KDE.

**Mosaic's three parts (`file_dialog.cpp`, `app_window.cpp`):**

1. **Correct parent handle.** `runPortal` passes `WaylandForeignExport`'s `"wayland:<handle>"` on
   native Wayland (`zxdg_exporter_v2`, kept alive for the whole call so the handle isn't revoked) and
   `"x11:<xid>"` on X11/XWayland — byte-for-byte the Qt strings above — plus `modal:true`. This gets
   the compositor/WM to stack + present the dialog transient-for our window.
2. **Client-side input backstop** (the piece Qt's *portal* path skips). `fileDialogInputGuard`, an
   `Fl::event_dispatch` hook active while `fileDialogInFlight()`, swallows every reachable input
   event to our windows — PUSH/DRAG/RELEASE/MOVE/WHEEL/KEYBOARD/KEYUP/SHORTCUT, **and refuses
   drag-drop** (returns 0 to DND_ENTER/DRAG/LEAVE/RELEASE so no `FL_PASTE` URI-list follows) so a
   file dropped on the canvas can't open a document behind the picker. `DialogGuard.deactivate()`
   only greys the window (FLTK gates *drawing* on `active_r()` but *event delivery* on
   `takesevents()`), so the dispatch hook — not deactivation — is what actually blocks input. The
   quit paths (`cbQuit`, the window CLOSE callback) also consult `fileDialogInFlight()` so the WM
   close button / a stray Escape can't hide the parent out from under the portal's nested wait loop.
3. **kdialog `--attach` on X11+KDE only.** On a Plasma **Wayland** session with Mosaic running as an
   **XWayland** client — which is what the old `preferX11BackendIfUnset` default made everyone — the
   portal's dialog is a native-Wayland Plasma window that KWin will not parent to our XWayland
   surface from an `x11:<xid>` token — it floats. A directly-spawned `kdialog` forced onto the xcb
   platform and given `--attach <xid>` is a real XWayland→XWayland transient the WM honours
   (`WM_TRANSIENT_FOR` + `_NET_WM_STATE_MODAL`). We prefer it there and fall back to the portal if
   kdialog is missing. ⚠ **Since S59-a the default backend is native Wayland**
   (`preferWaylandBackendIfUnset`, `docs/wayland.md`), so `activeBackend() == WindowSystem::X11` is
   **false** for a default Plasma session and this branch is no longer the one a KDE user takes —
   see below.

**The kdialog fork: retirement condition MET, removal pending one interactive check.** With part 2
in place the app is already *safe* behind a floating portal dialog (it can't be driven), but on what
used to be the most common Linux config (Plasma Wayland + XWayland apps) the portal dialog still
*looked* non-modal — exactly the original complaint. kdialog `--attach` was the only thing that gave
proper modal stacking/focus there, and it is no more "out of process" than the portal itself. This
paragraph used to say it could be dropped **once Mosaic gains a native-Wayland canvas: then the
`"wayland:<handle>"` path (part 1) is the norm on Plasma and the XWayland mismatch disappears.**

That condition is now satisfied. The native-Wayland canvas landed at S11-c and **became the default
in S59-a**, so a default Plasma session already goes through part 1 with a real `zxdg_exporter_v2`
handle. What is left is the check that no headless harness can make: that the portal picker really
does stack modal there (below). Until someone has looked, the fork stays — deliberately, not by
neglect — for two reasons: it is the only modal picker a **pure-Xorg** or `FLTK_BACKEND=x11` KDE user
gets, and removing it before the interactive pass would trade a verified behaviour for an assumed
one. The gate in `file_dialog.cpp::run()` carries the same note.

**Needs live verification (cannot be checked headlessly — requires a Wayland/KDE compositor):** that
on a native-Wayland Plasma session — now simply *starting Mosaic*, no `FLTK_BACKEND=wayland` needed —
the portal picker actually stacks modal over the main window and the main window cannot be clicked;
and that on XWayland+Plasma (`FLTK_BACKEND=x11`) the kdialog transient is still modal. Headless
builds only prove no crash/regression. **This is the check the fork's removal waits on**, and it is
item 5 of `docs/wayland.md` §5.

---

## 9. Menu & dirty-state wiring

`File` (built in `app_window.cpp:204`): New · Open… · Open as Layer… · [Recent] · — · **Save**
(greyed till S48) · **Save As…** (greyed) · — · **Export As…** `Ctrl+Shift+E` · **Export to
`<file>`** (after first export) · **Quick Export → PNG** `Ctrl+Alt+E` · — · Quit. The `Export…` item
+ `Ctrl+Shift+E` already exist wired to `cbTodo` (`app_window.cpp:211`).

Dirty state = command-stack saved-position marker (GIMP-style, §0): moved only by `.mosaic` Save;
Export sets a separate *exported* flag used for the "Export to `<file>`" affordance and a title hint
(`exported`), never clearing `• unsaved`. S18-d's title formatting + duration ride this.

---

## 10. Sequencing (session breakdown)

1. ✅ **M1 — Quick Export → PNG** (§7) + the portal dialog Phase 0/1 (§8). *First verifiable.*
   **DONE 2026-07-04 (`4b692c5`); S18-d unsaved-title rode with it (`fb98be4`). → M2 is next.**
2. ✅ **M2 — I/O framework (S41):** `FormatBackend`/registry/`OptionsSchema`, `FormatCaps` +
   `diff()` loss system, `DocumentProfile` extraction. **BUILT 2026-07-27** — `src/io/`
   `options_schema.*`, `document_profile.*`, `caps.*`, `format_backend.hpp`, `format_registry.*`,
   `backends/{backends.hpp,png_backend.cpp}`; four headless test files
   (`tests/test_export_{options,loss,profile,registry}.cpp`). As-built notes are inline in §2.1
   (structure, registration, schema vocabulary) and §4.1 (the diff's severity rules). **Two items
   from this line were deliberately NOT built:** `render::resizeImage` (the exact signature and
   semantics are specced in §5 item 1; `render/compositor.cpp` was mid-rewrite for S60-a) and the
   generic options *panel* (M3 owns the modal; M2 built and unit-tested only its data model).
3. ✅ **M3 — Export… modal shell (S56 UX): BUILT 2026-07-28.** The re-plumb landed as planned —
   `src/ui/export_dialog.{hpp,cpp}` no longer knows a single format name. What it now drives from
   M2: the **format list** is `FormatRegistry::exportOrder()` (with the Common-tier divider, and
   JXL simply absent when `available()` is false); the **options panel is generated from
   `optionsSchema()`** — one widget kind per `OptionType`, `visibleWhen` conditions re-evaluated on
   every edit, groups behind a disclosure header collapsed per `collapsedByDefault`; the **encode**
   goes through `FormatBackend::encode()` / `encodeToFile()`; the **live loss banner** is
   `diff(profile, caps, values)`, translated by `LossCode` at the UI (§4.1's rule) and coloured
   red / amber / quiet green off `worstSeverity`; the **Matte row shows itself** off
   `caps().alpha == None` rather than off "is this JPEG". Also new: a **preset row**, the
   **resize-quality dropdown** (§6.4), the §6.1 **path policy**, and the async pipeline below.
   **Two backends were adapted, not written:** `backends/jpeg_backend.cpp` and
   `backends/jxl_backend.cpp` wrap the `io::encodeJpeg` / `io::encodeJxl` encoders that already
   shipped — without them the modal would have LOST the two formats it already offered. The rest
   of M4's list (WebP, AVIF, TIFF, GIF) is untouched.
   *Async + cache as built:* one `PipelineJob` (the InpaintJob template — worker `std::thread`,
   atomic `done`, cancellation through the encoder's `ProgressFn`, a re-armed `Fl::add_timeout`
   poll, no `Fl::awake`, the worker never touches FLTK), driven by a **pipeline key** naming
   format + options + size + filter + matte, so an unchanged key costs nothing and a quality-slider
   drag re-runs **only** the encode stage (the resize is cached and handed to the job). The encode
   is debounced 250 ms and its bytes are the source of **both** the exact file size and — decoded
   back through the new `io::decodeImageBytes` — the preview, so a JPEG preview shows its own
   artefacts. It carries **no mutex** (unlike InpaintJob) because no encoder reports intermediate
   progress: every result is written before `done` and read after the join.
   *Owed:* the visual pass, and `render::resizeImage` (§5 item 1) — see the seam note there.
4. ✅ **M4 — Common raster (S42): BUILT 2026-07-28.** JPEG and JXL rode in with M3; this line added
   **WebP, AVIF, TIFF and GIF** — codec, decoder, `FormatCaps`, `OptionsSchema` and registry entry
   each. As-built detail in §2.3a below (dependencies + gating), §3.1a (what the caps rows actually
   claim) and §11's verification list. Also landed with it, because the four formats need them:
   **EXIF write-back** (`src/io/exif_write.{hpp,cpp}` — §7b's owed half), **ICC embedding** on
   export, a dependency-free **quantizer** (`src/io/quantize.{hpp,cpp}`), and PNG's own
   eXIf/iCCP/pHYs chunks. **Not built, deliberately:** JPEG-in-TIFF (see §3.1a), animation for any
   format, and EXIF *reading* from the four new containers (the write half serves all of them).
5. **M5 — `libmosaicformats` curated (S42/S44):** BMP, TGA, PNM/PAM, QOI, ICO, Radiance HDR.
6. **M6 — Vector (feeds S56):** SVG (hand-emit), PDF (cairo + PoDoFo for OCG), EPS/PS (cairo) with
   the transparency-flatten warnings.
7. **M7 — Exotic tier + the Settings flag:** the rest of `libmosaicformats`, gated on
   Settings → General → "Show all export formats".
8. **HDR tier (with/after S43-a):** the ImageF tap → OpenEXR, Radiance-HDR-float, float-TIFF,
   JPEG 2000, HDR/float JXL.

Each milestone: build all three presets + `ctest` + `mosaic --gui-frames N` clean; user does the
visual pass.

---

## 11. Verification (headless)

- Loss `diff()` + `DocumentProfile` extraction + `OptionsSchema` = pure unit tests (goldens).
  **Built at M2**, four files, all headless and display-free:
  `tests/test_export_loss.cpp` pins the **exact ordered LossCode set** for every rule against
  test-local §3.1 capability rows (PNG/JPEG/GIF/TIFF/QOI/SVG/EPS), and exercises each rule against
  a format that *does* support the feature — so a `diff()` that always warns fails;
  `tests/test_export_profile.cpp` drives every profile field off a real `core::Document` (this is
  the file that fails when the model moves under §4's table);
  `tests/test_export_options.cpp` covers defaults / clamping / step-snapping / type-mismatch /
  unknown-key pruning / idempotence / conditional visibility, and asserts `validateSchema()`
  names each class of mistake;
  `tests/test_export_registry.cpp` covers lookup, combobox ordering, availability gating,
  duplicate rejection, a real PNG encode→decode round-trip *through the registry*, and the
  cross-cutting invariants (**every** registered backend's schema validates and its caps are
  internally consistent). M3 added the JPEG/JXL adapters to it: caps honesty (JPEG is never
  lossless, not even at quality 100), the shared option vocabulary through `encodeIsLossless`,
  JXL's `distance`-hidden-by-`lossless` condition as *data*, and a JPEG encode where quality 20
  really is smaller than quality 95.
  **`tests/test_export_path.cpp` (M3)** pins the §6.1 path policy: the four-step folder priority,
  that a relative candidate is skipped rather than resolved, that the seed keeps the folder and the
  base name apart (the `http://` bug's precondition), that a re-export re-offers the file's own
  name, `resolveExportPath` / `withExtension` / `fileNameOf` / `isAbsolutePath`, and — explicitly —
  that **`std::filesystem::current_path()` is unreachable from every input shape**. That last case
  is the only automated guard either picker bug can have: the dialogs themselves need a display.
- Encoder round-trips: encode → decode → compare (bit-exact for lossless; SSIM band for lossy).
  **Built at M4**, three files, all headless: `tests/test_io_formats.cpp` pins the four new
  signatures (including that a **HEIC brand must NOT sniff as anything we claim to decode**), a
  bit-exact round trip per format — WebP lossless+exact, AVIF lossless, TIFF once per *configured*
  compression × predictor, GIF for a picture that fits its palette — plus the lossy counterpart
  being genuinely smaller, GIF's one-bit alpha cut against the matte, the registry wiring
  (`available()` agreeing with the codec probe, `validateSchema()` over each new schema) and the
  caps/loss goldens for the two corrections above. Every case is gated on its own `*Supported()`
  probe, so a machine without libavif still runs green. Hostile input: each format's own encoded
  bytes truncated at five fractions and scrambled in the payload tail, asserting only that the
  decoder never crashes, hangs or returns a buffer inconsistent with its own dimensions;
  `tests/test_io_quantize.cpp` covers the palette (budget never exceeded, the transparent slot,
  exactness carve-out, determinism, a mean-error bound on a ramp);
  `tests/test_io_exif_write.cpp` covers `buildExifPayload` → `parseExif` per field, the
  out-of-range drops, and the whole thing through a real PNG — including that an ICC profile
  **libpng refuses costs the chunk, never the export**.
- Decoder hardening: `detail::dimensionsPlausible()` bounds width, height **and area** before the
  first allocation. `kMaxDim` alone does not: 30000 × 30000 passes both per-side checks and asks
  for 3.6 GB, which is an out-of-memory kill rather than an error message.
- `render::resizeImage` per-kernel goldens.
- Portal dialog cannot be unit-tested headless — user does the interactive X11 + Wayland pass.
- `libmosaicformats` tested standalone (no Mosaic deps) against reference files where available.

---

## 12. Parked / future

### 12.1 HEIC/HEVC strategy — **designed in `docs/heic-strategy.md`** (2026-07-04)

Delegation-only: Mosaic ships zero HEVC code and reads/writes `.heic` only via a system codec —
**macOS** ImageIO (nil-check on `CGImageDestinationCreateWithURL`), **Windows** WIC over the Media
Foundation HEVC MFT (`MFTEnumEx(MFVideoFormat_HEVC)` probe + user-installed HEVC Video Extensions),
**Linux** libheif's runtime plugin system (`heif_have_{decoder,encoder}_for_format`; we link libheif,
ship no HEVC plugin). Always visible + probed; declines with the snark (open message; export = same
with *open*→*export*) and gives **no acquisition instructions**. The same delegation spine is kept
general (could serve AVIF/other OS-provided codecs later) but we delegate only formats we refuse to
bundle — today just HEIC. **Parked out of the main arc**; lands after the framework.

### 12.2 Other future work
- **JXL HDR/float**, OpenEXR, float-TIFF, JP2 → the HDR tier, gated on the S43-a ImageF tap.
- **S56 proper:** export presets (save/load custom), **batch export**, multi-size export (icons).
- **Export selection / active layer only** (GIMP parity) — defer.
- **DDS/BCn** (BC1-3 via libsquish, BC7 via bc7enc_rdo, BC6H via DirectXTex — all no-Rust) — game-
  asset tier; add on demand.
- **PSD write** is S47 (separate), not this arc.

---

## Appendix A — Per-format encoder options (backend-settings source)

**PNG** (libpng): compression 0–9, zlib strategy (default/filtered/huffman/rle/fixed), filter
(none/sub/up/avg/paeth/adaptive), interlace (Adam7/none), bit depth 8/16, palette, iCCP/sRGB/gAMA,
text chunks, eXIf, pHYs(DPI).
**JPEG** (libjpeg-turbo/+mozjpeg): quality 0–100, subsampling 4:4:4/4:2:2/4:2:0/4:4:0/4:1:1,
progressive, optimize-huffman, arithmetic (warn: rarely decodable), restart interval, DCT method,
smoothing; mozjpeg: trellis (+DC/EOB/Q), optimize-scans, overshoot-deringing, quant-table preset.
**JXL** (libjxl): distance 0–25 **or** quality 0–100, effort 1–10 (11 needs
`JxlEncoderAllowExpertOptions`), modular/VarDCT/auto, lossless (needs `SetFrameLossless(true)` **and**
`uses_original_profile=true` — distance 0 alone is insufficient), progressive DC/AC, responsive,
bit depth incl. float, alpha (+premultiply), animation, **lossless JPEG transcode**
(`StoreJPEGMetadata` before frames + `AddJPEGFrame`), photon-noise, decoding-speed tier,
brotli-effort, EXIF/XMP/JUMBF boxes, codestream level 5/10 (10 = CMYK/32-bit, less-supported).
**WebP** (libwebp): quality, lossless, near-lossless, method 0–6, alpha quality/compression/filter,
exact, filter strength/sharpness, sns, segments, pass, target size/PSNR, preset; anim via
`WebPAnimEncoder`.
**AVIF** (libavif+aom): qcolor/qalpha 0–100, speed 0–10, depth 8/10/12, yuv 444/422/420/400,
lossless, range, tiling/autotiling, **codec=aom|svt (never rav1e)**, CICP/ICC, premultiply,
grid/layered/progressive, anim, aom passthrough (end-usage/cq-level/tune/aq-mode/…).
**TIFF** (libtiff): BigTIFF, compression None/LZW/Deflate(ZIP)/JPEG/PackBits/CCITT/LZMA/ZSTD/WebP
(+ per-codec quality/level), predictor none/horizontal/float, strips vs tiles, planar config,
photometric/sampleformat, assoc-vs-unassoc alpha, ICC/EXIF/XMP/IPTC. Query `TIFFIsCODECConfigured()`.
**GIF** (giflib): GIF89a, global/local palette, palette size 2–256, interlace, disposal,
transparency index, per-frame delay, loop (NETSCAPE2.0). We supply the quantizer (Wu/NeuQuant) +
Floyd–Steinberg dither.
**OpenEXR** (openexr): compression NONE/RLE/ZIPS/ZIP/PIZ/PXR24/B44/B44A/DWAA/DWAB(+level), pixel type
HALF/FLOAT/UINT per channel, scanline/tiled(+mip), line order, data/display window, chromaticities.
**JPEG 2000** (openjpeg, **Part-1**): reversible 5/3 (lossless) vs irreversible 9/7 (lossy),
rate/quality layers, resolution levels, code-block, precincts, tiles, progression order (LRCP/…),
error-resilience, ROI, MCT, J2K vs JP2 container.
**BMP** (mosaicfmt): header V3/V4/V5, BI_RGB/RLE8/RLE4/BITFIELDS, top-down/bottom-up, 555/565/BGRA32,
palette, V5 ICC.
**TGA** (mosaicfmt): RLE on/off, depth 16/24/32, alpha bits, attributes-type (straight/premul),
origin, v2 extension area.
**PNM/PAM** (mosaicfmt): variant PBM/PGM/PPM/PAM, ASCII vs binary, maxval 8/16-bit, PAM TUPLTYPE.
**QOI** (mosaicfmt): channels 3/4, colorspace tag.
**ICO/CUR** (mosaicfmt): which sizes, per-entry depth, PNG-compress vs raw DIB per entry, AND-mask;
CUR hotspot.
**Radiance HDR** (mosaicfmt): RLE on/off, EXPOSURE, GAMMA, PRIMARIES, PIXASPECT.

## Appendix B — GIMP export parity checklist (match-or-exceed target)

GIMP 2.10/3.0 EXPORT: PNG, JPEG, JXL(3.0), WebP, TIFF, BMP, GIF, TGA, PCX, PNM/PBM/PGM/PPM/PFM,
PSD/PSB, DDS, XPM, XBM, ICO, CUR(3.0), ANI(3.0), ICNS(3.0), XMC, SGI, Sun-Raster, FITS, FLI/FLC,
CEL, PIX, TIM(3.0), DICOM, raw-data, Farbfeld(3.0), QOI(3.0), MNG(export-only), PDF, PS, EPS,
SVG(paths-only), HTML-table(export-only), XCF(native), ORA, C-source, C-header, ASCII-art, GBR/GIH/
PAT, GEGL, GPL. **Import-only in GIMP (we EXCEED by exporting): OpenEXR, JPEG 2000, PAM.** HEIC/AVIF
in GIMP are encoder-gated and an HEVC encoder is usually absent on Linux.

## Appendix C — JXL encode sequence (libjxl)

`JxlEncoderMake` → `SetParallelRunner`(resizable runner, `SuggestThreads(w,h)`) → `[StoreJPEGMetadata]`
→ `[UseContainer]` → `[SetCodestreamLevel]` → `InitBasicInfo`/`SetBasicInfo` (bits, exponent_bits for
float, `uses_original_profile`) → `SetColorEncoding` **or** `SetICCProfile` → `FrameSettingsCreate`
→ per-frame `SetFrameDistance`/`SetFrameLossless`/`FrameSettingsSetOption(EFFORT/MODULAR/…)` →
`AddImageFrame` (or `AddJPEGFrame` for transcode) → `[UseBoxes`/`AddBox`/`CloseBoxes]` → `CloseInput`
→ drain `ProcessOutput`. Gotchas: lossless needs `SetFrameLossless(true)`+`uses_original_profile`;
effort 11 needs `AllowExpertOptions`; EXIF box payload prefixed with 4-byte TIFF offset; `RESPONSIVE`
is the Modular "Squeeze" knob. License BSD-3, Arch `libjxl`, **no Rust**.
