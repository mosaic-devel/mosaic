# libmosaicformats — the curated-pro codecs (M5)

Milestone **M5** of the Export & I/O arc (`docs/export-system-plan.md` §2.2, §3, §10 item 5): six
hand-rolled, dependency-free codecs behind the always-on **curated pro** export tier — **BMP, TGA,
PNM/PAM, QOI, ICO** and **Radiance HDR** — each with an encoder, a decoder, a real `FormatCaps` row
and a real `OptionsSchema`.

```
src/formats/                     target mosaic::formats, namespace mosaicfmt -- LINKS NOTHING
  formats.{hpp,cpp}   ImageView / Bitmap / IndexedView, the limits, the byte reader+writer,
                      sniff(), and the box downscaler the ICO writer needs
  bmp.{hpp,cpp}       BMP + the bare DIB (ICO payloads reuse this parser)
  tga.{hpp,cpp}       Targa v2, including the extension area's attributes type
  pnm.{hpp,cpp}       PBM / PGM / PPM / PAM, plain and raw
  qoi.{hpp,cpp}       QOI, clean-room from the published specification
  ico.{hpp,cpp}       the icon container (directory + entry selection)
  hdr.{hpp,cpp}       Radiance RGBE
src/io/backends/mosaicformats_backend.cpp    the six FormatBackends + the open/decode adapter
```

Nothing here has anything to do with `src/io/mosaic/`, which is the native `.mosaic` document
container (`docs/mosaic-native-format.md`). Adjacent names, unrelated code.

---

## 1. Why the library links nothing

§2.2 asks for a *standalone* library, and §11 asks for it to be "tested standalone, no Mosaic deps".
Both are properties of `src/formats/CMakeLists.txt` having an empty `DEPS` list, so the fence is
enforced by the build rather than by good intentions:

- the library speaks only `mosaicfmt::ImageView` (a borrowed `const uint8_t*` plus dimensions) and
  `mosaicfmt::Bitmap` (an owned one). No `common::Image`, no `mosaic::io`, no system codec, no FLTK;
- everything Mosaic-shaped happens in **one** adapter translation unit,
  `src/io/backends/mosaicformats_backend.cpp`: the `common::Image` conversion, the palette for an
  indexed BMP (`io::quantize`), the ICC bytes for a BMP V5, and libpng for an ICO's PNG payloads;
- the six codec test files (`tests/test_formats_{bmp,tga,pnm,qoi,ico,hdr}.cpp`) include
  `formats/*.hpp` and nothing else of Mosaic's. `tests/test_formats_backends.cpp` is the only one
  that knows the registry exists.

The two consequences worth stating plainly: **a codec here cannot own colour policy** (choosing a
palette is a decision the loss banner quotes numbers from, so it belongs to `io::quantize`, which
has its own tests), and **a codec here cannot reach for another codec** (an ICO entry that holds a
PNG is decoded by the adapter, not by growing a PNG reader down here).

## 2. Hostile input

Every decoder is an internet-facing parser, and all six follow `src/io/exif.cpp`'s discipline
exactly, because that file is the house reference for it:

- hard caps on declared dimensions (`kMaxDim` 30000, `kMaxPixels` 2^28 — the same pair as
  `io::detail`) checked **before the first allocation**, and a payload-size ceiling;
- every offset and length validated against the **actual buffer**, never against another declared
  field. `biSizeImage`, a TGA's `xOrigin`, a PNM's `MAXVAL` and a BMP's `bfSize` are all read past
  and never trusted;
- a **structural lie rejects the file whole** (a pixel offset outside the file, a run that overruns
  its row, a palette index with no entry, an RLE stream with no end marker); an **absurd value in a
  decorative field drops that field alone** (an unreadable ICO AND mask, an out-of-range Radiance
  `EXPOSURE`, one broken slot in an icon directory that has four good ones);
- nothing allocates in proportion to a declared count. The only count-proportional allocation in
  the library is a TGA colour map, whose count is a 16-bit field.

Each format's test file carries a **truncation sweep on exact-size heap allocations** — every prefix
length of a real file, each in its own exactly-sized `std::vector`, so ASan sees a one-byte overread
— plus a scrambled-payload case. The suite runs under ASan/UBSan.

## 3. What each format actually does

### BMP
**Writes** 32-bit BGRA (V4/V5, with an explicit alpha mask), 24-bit BGR, 16-bit 5-6-5, and 8-bit
palettised with optional **BI_RLE8**; header **V3 / V4 / V5**; bottom-up or top-down rows;
`biXPelsPerMeter` density; and a **V5 embedded ICC profile** (`PROFILE_EMBEDDED`, with the
`bV5ProfileData` offset counted from the start of the DIB header, which is the field everyone gets
wrong).
**Reads** BITMAPCOREHEADER and every INFOHEADER spelling through V5; 1/4/8/16/24/32-bit;
`BI_RGB`, `BI_BITFIELDS`, `BI_RLE8` and `BI_RLE4`; masks from either place they can live; either row
order.
**Not built:** `BI_JPEG` / `BI_PNG` payloads (another codec inside a BMP — refused with a message),
RLE4 *encoding* (decode only), and OS/2 2.x's extra header fields beyond the first 40 bytes.
**Three details that are bugs if wrong**, all with a test: rows pad to four bytes; 16-bit `BI_RGB`
is **5-5-5**, not 5-6-5; and a 32-bit `BI_RGB` file's fourth byte is *formally undefined*, so it is
read as alpha only when it is non-zero somewhere — the reading every decoder converged on, and the
difference between an image and an invisible one. A V4/V5 header carries mask fields whether or not
the compression uses them, and writers leave them zero under `BI_RGB`: honouring those would sample
every channel as 0 and hand back a uniformly black picture, so masks are used only under
`BI_BITFIELDS`.

### TGA
**Writes** Targa v2: 32-bit BGRA, 24-bit BGR or 16-bit 5-5-5-1, plain or RLE (type 2/10), either
origin, plus the **495-byte extension area and the v2 footer** — the extension area's last byte, the
*attributes type*, is the only standard place a TGA states whether its alpha is straight (3) or
premultiplied (4). Choosing premultiplied really premultiplies the pixels; relabelling them without
multiplying would be a lie about the file (the rule `TiffSaveOptions::premultipliedAlpha` already
follows).
**Reads** image types 1, 2, 3, 9, 10 and 11 (colour-mapped, truecolour and greyscale, plain or RLE)
at 8/15/16/24/32 bits, either origin, either scan direction, colour-map entries of 15/16/24/32 bits;
un-premultiplies a type-4 file so callers only ever see straight alpha.
**Not built:** *writing* colour-mapped or greyscale TGAs (there is no user story for either that
BMP-8 does not serve better), the postage-stamp thumbnail, the colour-correction table, and image
types 32/33 (Huffman + delta, which nothing writes).
**TGA has no magic number**, so `mosaicfmt::sniff()` accepts one only when the whole 18-byte header
is self-consistent *and* everything it declares fits the buffer, and only after every format with a
real signature has been tried. That deliberately rejects a few unusual-but-legal files (a v1 file
whose header is odd) rather than claiming somebody else's bytes; a test feeds it prose, a PNG and a
JPEG.

### PNM / PAM
**Writes** PPM (P6/P3), PGM (P5/P2), PBM (P4/P1) and **PAM** (P7) with the tuple types
`RGB_ALPHA`, `RGB`, `GRAYSCALE_ALPHA` and `GRAYSCALE`. Greyscale is Rec. 601 luma, integer, so it is
reproducible. PBM is a plain threshold with **no dither** — a bilevel export wants dithering chosen
deliberately, not hidden inside a codec.
**Reads** P1–P7, 8- and 16-bit samples, comments anywhere whitespace is legal.
**Not built:** 16-bit *encoding* (the source is an 8-bit flatten, so it would be upsampling with no
new information), PFM (float — the HDR tier), and multi-image PNM streams (concatenated images:
first one wins).
The family's inversion is worth stating once: **in PBM, 1 means black**; in PGM/PPM 0 means black;
and PAM's own `BLACKANDWHITE` tuple inverts PBM again (1 = white). All three spellings have a test.

### QOI
**The published specification** (Dominic Szablewski, 2021 — MIT-licensed spec document),
implemented clean-room in both directions: the `INDEX`/`DIFF`/`LUMA`/`RUN`/`RGB`/`RGBA` chunks, the
`(r*3 + g*5 + b*7 + a*11) % 64` hash, the wrapping 8-bit signed deltas, 3- or 4-channel headers, the
colourspace tag, the mandatory 8-byte end marker (required on read — it is the format's only
integrity signal).
Two details are load-bearing and invisible to a round-trip test, so both are pinned against
**hand-computed byte streams**: the hash index starts at `{0,0,0,0}` while the running pixel starts
at `{0,0,0,255}`, and a run may reach 62 (not 64) because the two 8-bit tags take the top two
values. Getting either wrong yields files only our own decoder can read.
The colourspace byte is a **tag, not a profile**: QOI never converts anything and neither do we, so
the caps row claims no ICC.

### ICO
**Writes** a directory of several sizes off one exported image: 32-bit BGRA DIB payloads with the
**doubled `biHeight`** and the 1-bit **AND mask** derived from alpha, or whole **PNG** payloads
supplied by the adapter (libpng), chosen per entry by the `payload` option (`auto` uses PNG for the
256-pixel entry and bitmaps below it).
**Reads** the directory, picks the entry a viewer would show (largest area, then depth, then payload
size), and decodes a DIB payload through the same BMP parser; a PNG payload is reported as such so
the adapter can hand it to libpng.
**Two traps, both with a test.** `biHeight` spans the XOR image *and* the mask, so an entry's bitmap
declares twice its height — write the real height and the icon looks correct everywhere except in
Explorer, which is the only place anyone looks at an icon. And a **256-pixel side is recorded as 0**,
because the directory has one byte for it. On read, the AND mask is honoured only when the XOR image
carries no usable alpha, which is what Windows does: some writers leave a legacy mask filled with
ones beside a perfectly good alpha channel, and obeying it there would erase the icon.
**Not built:** CUR and ANI. §3's Tier 3 lists `CUR/ANI` in the *exotic* tier, so the hotspot and the
animation belong to M7, not here.

### Radiance HDR
**Writes** `#?RADIANCE` / `FORMAT=32-bit_rle_rgbe`, the `-Y h +X w` resolution line, and scanlines
either flat or in Radiance's **adaptive per-component RLE**.
**Reads** flat scanlines, the **old** run-length spelling (`(1,1,1,n)` repeats the previous pixel,
shifting by 8 bits per consecutive record) and the adaptive one; honours an `EXPOSURE` header by
dividing it back out; accepts `-Y h +X w` and its vertically flipped `+Y` variant.
**Not built:** `32-bit_rle_xyze` (CIE XYZ with a shared exponent — converting it needs the file's
primaries, so it is refused rather than guessed at), the transposed resolution spellings
(`+X w -Y h`), and the `PRIMARIES` / `PIXASPECT` / `GAMMA` headers.

**⚠ The honest part, and it is the important part.** `common::Image` is 8-bit RGBA and
`render::composite()` collapses to it at `toImage8Parallel` (plan §5's high-bit note), so **this
encoder cannot write dynamic range the document never produced**. No tone mapper was invented. What
was chosen instead is a documented, self-consistent convention:

- **encode**: each 8-bit sRGB value is decoded through the sRGB transfer function to **linear
  light**, and that linear value is what RGBE stores. Radiance files are linear-light by convention,
  so writing the encoded value verbatim would make every HDR viewer show a washed-out picture;
- **decode**: the linear value is **clamped to [0,1]** — an exposure-1 clamp, no auto-exposure — and
  re-encoded to 8-bit sRGB.

So the pair round-trips, and a real HDR file loses exactly what an 8-bit buffer cannot hold. The
caps row therefore reads `maxBitDepth = 8, floatPixels = false` on a high-dynamic-range format,
which is the caps rule (`io/caps.hpp`: "a field states what THIS BACKEND WRITES TODAY") doing its
job: the banner must not promise headroom the file will not receive. **When the "tap the `ImageF`
accumulator before the 8-bit conversion" slice lands (§5 / S43-a), this backend needs a float entry
point and those two fields flipped — and nothing else.**

RGBE shares one exponent across a pixel's three channels, so a dark channel beside a bright one
loses precision *by construction*. The tests measure that honestly: equality is asserted only
between two encodings of the same pixels (coded and flat, where the quantisation is common and only
the coding differs), and the round-trip cases use tolerances that are the format's, not the code's.

## 4. The one capability the caps model cannot express yet

Four of these formats have an option that changes what the file can carry:

| Format | Option | At the default | If changed |
|---|---|---|---|
| BMP | depth 24/16/8, or header V3 | 32-bit V5: straight alpha + ICC | alpha composited onto the matte; 8-bit also quantizes |
| PNM | variant | PPM: no alpha | PAM: straight alpha |
| QOI | channels | 4: straight alpha | 3: alpha composited onto the matte |
| TGA | transparency = "Not used" | straight alpha (attributes type 3) | composited onto the matte |

`FormatCaps` describes a **backend**, not a backend-plus-option-bag, so it cannot say "alpha
survives, except under this one setting". This is the same gap that keeps **JPEG-in-TIFF** out of M4
(§3.1a), and the rule it forces is §4.1's: **the banner must never over-promise.** So every caps row
above states the **default**, which means:

- BMP / QOI / TGA claim alpha, and a user who deliberately picks a narrower depth gets no warning
  about the transparency they just chose to drop. The option's own help text says so in as many
  words — that is the mitigation, and it is a weaker one than a banner;
- PNM claims **no** alpha, so picking PAM *over*-warns ("transparency will be lost" when it will
  not). That is the safe direction of wrong: over-warning is a nuisance, over-promising is a lie
  about the file the user is about to write.

A caps model that can express a per-option capability fixes BMP's depth, PNM's variant, QOI's
channel count, TGA's attributes type, JPEG-in-TIFF, WebP animation and GIF animation **in one
stroke**. It is the single highest-value follow-up this milestone leaves behind.

## 5. Deviations from the plan document

1. **Path.** §2.2 names `src/core/io/mosaicformats/`. That line predates the reverted
   `src/io → src/core/io` move (§0, §2.1) and the library is at **`src/formats/`**. Target
   `mosaic::formats`, namespace `mosaicfmt`, exactly as §2.2 specifies for those two.
2. **The ICO writer scales.** §2.1 says io never resizes, and §5 puts the resize before `encode()`.
   An icon, though, is a *set of mip levels of one picture*, and the pipeline hands `encode()` a
   single image — so the ICO backend derives its smaller entries itself, with a box-average
   downscaler that works in premultiplied alpha (`mosaicfmt::downscaleBox` / `fitSquare`, ~60 lines,
   in the library so it has no Mosaic dependency). It **never upscales**: a requested size larger
   than the source is skipped rather than invented, and a non-square source is fitted and centred on
   a transparent square rather than squashed. §12.2's "multi-size export (icons)" remains a
   different, UI-level feature; this is one file's internal structure.
3. **Appendix A options not offered**, each because a schema may only describe knobs `encode()`
   really honours: BMP's `BI_RLE4` and 555/palette-in-truecolour spellings; TGA's origin-x/y and
   postage stamp; PNM's 16-bit `MAXVAL`; ICO's per-entry depth (every entry is 32-bit) and CUR's
   hotspot; and **all four Radiance header variables** (`EXPOSURE`, `GAMMA`, `PRIMARIES`,
   `PIXASPECT`) — each is a *statement about the pixels*, and with an 8-bit source there is nothing
   true to say with them. Writing `EXPOSURE=2` without having scaled anything makes every reader
   divide by two.
4. **`io::sniffImageFormat` was not extended.** The curated formats sniff themselves
   (`mosaicfmt::sniff`), and `io::decodeSniffed` falls through to that one call, so `ImageFormat`
   gained no members. The visible cost: `io::probeImageDimensions` (the New-Document recents cards'
   size labels) does not answer for these six, exactly as it already declines for TIFF and AVIF.
5. **PNM's primary extension is `pnm`** for every variant, because `extensions()` is static data
   while the variant is an option. A user who chooses PAM and lets the dialog append `.pnm` gets a
   correct file with a family extension, which netpbm reads; the mismatch a variant-aware suggestion
   would fix needs `extensions()` to see the option bag.

## 6. Provenance

BMP, TGA, PNM, ICO, QOI and Radiance HDR are all on the **ship freely** list in
`docs/export-system-plan.md` §3.2, and this milestone adds nothing to it. Every codec here is
written from the format's own published specification. QOI's specification document is MIT-licensed;
Radiance's RGBE encoding was published by Greg Ward in *Graphics Gems II* (1991) and the format's
own reader/writer has been public since. The RLE schemes here (BMP's, TGA's, Radiance's) are all
byte-oriented run-length coding described in each format's published specification.

## 7. Verification

Headless, and all of it in `ctest`:

| File | What it pins |
|---|---|
| `tests/test_formats_qoi.cpp` | hand-computed chunk streams (DIFF, LUMA, INDEX, run splitting at 62), the index's initial state, exact round trips, the mandatory end marker, header rejections, truncation sweep, scrambled stream |
| `tests/test_formats_bmp.cpp` | exact 32-bit V4/V5 round trip, row-order equivalence, the matte fallback for 24-bit and V3, 16-bit within its own quantisation, indexed + RLE8 exact round trip (RLE smaller), the V5 ICC offsets, four structural rejections, two truncation sweeps |
| `tests/test_formats_tga.cpp` | exact round trip plain and RLE, the v2 footer + attributes type, premultiply/un-premultiply, the matte fallback, 16-bit's single alpha bit, row order, `sniff()` refusing prose/PNG/JPEG, rejections + truncation sweep |
| `tests/test_formats_pnm.cpp` | PPM raw and plain, PAM's alpha, PAM greyscale tuples, PGM luma, PBM's threshold and its 1-means-black bit, comments mid-header, 16-bit sample scaling, nine header rejections, truncation sweeps |
| `tests/test_formats_hdr.cpp` | the header a reader looks for, coded-vs-flat byte-exact equivalence, a 300-pixel uniform scanline, grey-ramp and bounded-colour tolerances, the matte, the OLD run-length spelling, five rejections (XYZE included), truncation sweeps |
| `tests/test_formats_ico.cpp` | **the doubled height**, the 256-as-zero rule, multi-size selection, transparency from the AND mask (hand-built 24-bit entry), `fitSquare` padding rather than stretching, three encode rejections, a broken directory slot beside a good one, the PNG-payload refusal, truncation sweep |
| `tests/test_formats_backends.cpp` | registry entries + extension resolution, the curated run's position and ordering in `exportOrder()`, every caps claim above, the loss diff over the six rows, one encode-and-read-back per format through the backends, the ICO PNG payload through libpng, empty-image refusal, and the open path through `io::decodeImageBytes` |

`tests/test_export_registry.cpp`'s cross-cutting invariants cover the six new backends for free:
`validateSchema()` over every registered schema, `defaults()` surviving its own `coerce()`, and the
caps-consistency rules.
