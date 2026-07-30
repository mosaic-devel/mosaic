# Brushes — Engine Rework, Preset Conformance & Imports

> **Scope.** This note settles the design of Mosaic's brush engine rework: the dab/accumulation
> model, the preset data model, importers for third-party brush formats, the preset library, the
> brush editor, and the on-canvas reticle. It is the source of truth for the remainder of
> **S19-a** (engine core + presets, whose *base* shipped 2026-06-19) and for the **Eraser**, which
> does not exist yet and is built here. Tablet/stylus input is scoped separately in
> **`docs/tablet.md`** (the S19-b research note); the two land together.
>
> Promised by `PLAN.md` §3.12. Follows the house discipline: *design from the public-domain
> technique, bake the technique lineage into the source, keep the core FLTK- and Vulkan-free so it is
> unit-tested headlessly.*
>
> **Every format fact in §3 was verified directly against a shipped file** — chunks decompressed,
> XML parsed, bundles enumerated — not taken from documentation. Where two readings disagreed, the
> source settled it, and the disagreement is noted.

---

## 1. Scope

**In:**
- Rework `core::brush::BrushEngine`'s accumulation and dab model so it can faithfully express the
  dominant preset family.
- Preset data model, on-disk library, and importers (`.kpp`, `.gbr`, `.gih`, `.abr`, `.png`/`.svg`
  tips, `.myb` best-effort, `.bundle`).
- Ship a licence-vetted default preset set (§4).
- Brush editor: a modal with a paintable live preview, and the import entry point.
- Brush preview control in the tool context bar.
- Preset section in the right dock, shown for the Brush tool.
- **Implement the Eraser.** It does not exist today (§2.3).
- Settings: brush/eraser preferences.

**Out of this arc:**
- GPU stamping — stays behind the engine API (user decision 2026-06-19, `PLAN.md` §S19-a).
- The technique families of §5.
- A second, non-dab stroke primitive (§6.7).
- Windows/macOS tablet backends — designed in `docs/tablet.md`, built at S57/S58.

---

## 2. What exists today

### 2.1 The engine (`src/core/brush/brush_engine.{hpp,cpp}`)
Correct and well-built for what it does. `begin/extendTo/composite/restore/dirtyBounds`; a bounded
working rect grown in 256 px tiles; a pristine base snapshot taken per-pixel on first touch; a
region-scoped recomposite. `dabCoverage(d, R, hardness)` is an analytic circle with a smoothstep
shoulder and a guaranteed ~0.75 px anti-aliased rim. Accumulation is `dst += a·(1−dst)` gated by
`flow`, and the whole coverage is composited once at `opacity` — so a stroke never exceeds its
opacity where it crosses itself. That is the Photoshop/Krita flow-vs-opacity model and it is
preserved exactly.

### 2.2 The central structural problem
`m_coverage` is a **single-channel `std::vector<float>`**, and colour enters only at `composite()`
from one fixed `m_params.color`. This makes the following *inexpressible*, not merely unimplemented:

- per-dab colour — needed by image-stamp and lightness-map tips, and by colour dynamics
- smudge of any kind (requires reading the destination per dab)
- blend modes
- erase

> ⚠ **Only smudge is still inexpressible.** *(Updated 2026-07-09, twice.)* Blend modes and erase
> turned out not to need the colour axis at all — both act at `composite()`, against the pristine
> base snapshot; they shipped with `PaintMode`/`StrokeMode` on the `Uniform` accumulator (§6.1).
> **Per-dab colour then shipped as the `Colored` accumulator** (§6.1): premultiplied RGBA beside
> the coverage, entered through the `BrushDynamics::dabColor` seam. **Smudge remains inexpressible**
> — it needs to read the destination per dab, which no accumulator here does.

Further limits: `BrushParams` is frozen at `begin()` (no live mid-stroke change); `BrushDynamics` is
two booleans plus the per-dab colour hook; the dab is isotropic (no angle, ratio, scatter, texture).
*(**Segment interpolation is no longer linear** — that limit used to be
listed here too, and it was the one users actually felt: dabs walked a straight chord between
samples, so a 60 Hz mouse stroke came out a literal 60-gon. The walk now lays dabs along a
**centripetal Catmull-Rom curve THROUGH the samples** — `core/brush/stroke_path.hpp`. It
**interpolates**: the curve passes exactly through every point the user made, which is what keeps it
a different thing from a rope stabilizer. **Interpolate; do not filter** — a standing rule on this
path, see §5.)* *(A further limit used to sit here
— spacing computed from the nominal diameter rather than the pressure-scaled one — until step 7
landed: spacing now keys off the effective size, re-resolved at every dab, with
`useAutoSpacing`/`autoSpacingCoeff` beside it. See §6.2.)*

Both `begin()` and `extendTo()` call sites pass `pressure = 1.0` as a literal.

### 2.3 Three things that are not what they look like
1. **~~The Eraser does nothing.~~ Fixed at step 8 (§8.4, 2026-07-10):** `eraserToolActive()` joined
   `strokeToolActive()`, the engine had grown destination-out (`StrokeMode::Erase`) at step 5, and
   the two eraser-tie settings landed (size tie live in `ToolManager`; the preset tie schema-only
   until presets exist). *(The original finding, for the record: the tool was registered with
   options but had no canvas path — selecting it was inert.)*
2. **`hardness` and `flow` are unreachable in the UI.** They are declared `secondary(...)`, and
   `tool_options.cpp` skips every non-primary option when building the bar. The "More…" panel they
   were demoted for was never built.
3. **The right dock is not two stacked panels.** `HistoryPanel` is a *child* of `LayerPanel`, and the
   two are **tabbed** — one at a time, each filling the dock.

### 2.4 Infrastructure gaps
- **No curve-editor widget** anywhere in the tree.
- **No PNG text-chunk access.** `src/io/png.cpp` uses libpng's *simplified* API
  (`png_image_begin_read_from_memory`), which cannot see chunks.
- **No XML parser.**
- **No runtime resource directory.** Assets are compile-time embedded via `EmbedAssets`; the only
  user path concept is `common::configDir()`.
- **FLTK 1.4.5 has no tablet API** — see `docs/tablet.md`.

**Foundations to build on:** the bounded-footprint stamping loop; the region-scoped
`SetLayerPixelsCommand`; the `BrushToolHost` callback seam; `LayerEffectsDialog` as a modal template;
`PaintSwatch` (`tool_options.cpp`) as the custom-widget-in-the-bar precedent; `line_style_preview.cpp`
as the CPU-preview-renderer idiom; the data-driven settings rail.

---

## 3. The target format

### 3.1 `.kpp` container
A PNG. Verified by decompressing a shipped preset: 200×200, 8-bit, colour type 6 (RGBA); a chunk
keyed `preset` holding the XML (59 KB in the stock pixel-brush preset); a `tEXt` chunk keyed
`version` with value `2.2`. Accepted versions are `2.2` and `5.0` only.

> ⚠ **The `preset` chunk is not always `zTXt`.** *(Corrected 2026-07-09.)* An earlier revision said
> `zTXt`. In the shipped bundles it is **`zTXt` in 213 presets and an uncompressed `tEXt` in 35** —
> 22 of the 117 in `Krita_4_Default_Resources`, 13 of the 131 in `Krita_3`. Both spell `version` as
> `2.2`, so the version string does not predict the chunk type. A reader that matches only `zTXt`
> silently drops 19 % of the default set — and, because the dropped presets are not random, skews
> every histogram computed from it. Match on the **keyword**, then decompress iff the type is `zTXt`.

**The PNG raster *is* the preset icon.** Importing a preset therefore imports its thumbnail; there is
no separate artwork to source.

Root element `<Preset name paintopid embedded_resources>`, children `<param name type>value</param>`.
Types: `string` (CDATA), `color`, `bytearray` (base64), `internal` (numbers/bools). Version 5.0 may
also embed `<resources><resource type md5sum name filename>base64</resource></resources>`.

### 3.2 Property surface
The stock pixel-brush preset carries **94 params**. The shape is regular. For each dynamic base name
`X`, a fixed seven-key family:

| key | type | meaning |
|---|---|---|
| `Pressure{X}` | bool | option enabled *(the name is historical — it gates the whole option, not just pressure)* |
| `{X}Sensor` | string | sensor XML (§3.3) |
| `{X}Value` | double | strength |
| `{X}UseCurve` | bool | apply the response curve |
| `{X}UseSameCurve` | bool | one curve shared by all sensors (default true) |
| `{X}curveMode` | int | how multiple sensors combine (§3.3) |
| `{X}commonCurve` | string | the shared curve |

Bases on the pixel brush: `Opacity, Flow, Size, Ratio, Softness, Rotation, Sharpness,
LightnessStrength, Scatter, Darken, Mix, Rate, Mirror, Spacing, h, s, v`, plus `Texture/Strength/`.
`Opacity` and `Flow` are always-on; the rest are checkable. Legacy files also carry `Curve{X}` /
`Custom{X}`.

**Always-on means the `Pressure{X}` bit is written but ignored.** Shipped files carry
`PressureOpacity=true` in only 7 of the 82 and `PressureFlow=true` in *none* — yet the reader forces
both options on regardless, because their checkability says so. Do not infer "enabled" from the bit
for these two. (The §3.10 tables below read the raw bit; see the note there.)

**`{X}UseCurve` gates the SENSORS, not merely their curves.** With it off, the option collapses to
its constant `{X}Value` and no sensor is consulted at all. The key name says otherwise.

**Two different prefixing rules, and they do not agree** — both verified in
`d)_Ink-7_Brush_Rough.kpp`:
- `Texture/Strength/` is a *base name that contains slashes*. Its keys are the ordinary ones, so the
  enable key is **`PressureTexture/Strength/`** — `Pressure` in front of the whole thing, trailing
  slash and all.
- `MaskingBrush/Preset/` (§6.2) is a *nested property table*: inside it the option names are plain,
  so the same key reads **`MaskingBrush/Preset/PressureSize`**, with the prefix outside. A reader
  handles it by prefix-stripping the lookup, not by mangling base names.

Also note that `Texture/Strength/commonCurve` is simply absent from shipped files, so that option's
shared curve seeds from the last sensor its XML defined.

Flat options: `CompositeOp` (blend mode), `EraserMode`, `ColorSource/Type`
(`plain|gradient|uniform_random|total_random|pattern|lockedpattern`), `Scattering/Axis{X,Y}`,
`Sharpness/{alignoutline,softness,threshold}`, `Spacing/Isotropic`, `Texture/Pattern/*`,
`{Horizontal,Vertical}MirrorEnabled`, `PaintOpAction` (§3.8), `MaskingBrush/*`.

**Importer traps**, each verified in a shipped file:
- `KisPresisionOption/precisionLevel` — **the misspelling is in the shipped presets**; the modern key
  is `KisPrecisionOption/…`. Accept both.
- Airbrush: `PaintOpSettings/{isAirbrushing,rate,ignoreSpacing}` (rate default 20). ⚠⚠ **NOT the
  legacy `AirbrushOption/{isAirbrushing,rate}` — that prefix has no reader upstream at all, and this
  line said "accept both" until 2026-07-28, which froze the program (§6.6i).** A shipped file that
  spells only the old keys is not an airbrush; six of them do.
- Legacy mask generators write `radius=` where modern ones write `diameter=`.
- Legacy `paintopid` values `eraser`, `smudge`, `complex` appear in shipped defaults; the eraser maps
  to the pixel brush + `EraserMode`. Our mapper must do the same.
- `BrushVersion="1"` predefined tips have their `scale` **doubled** on load.
- Texture patterns embed as base64 PNG in `Texture/Pattern/Pattern`, so textured presets import
  self-contained.
- **Attribute order is not fixed.** A `name=`-first regex silently mis-parses 64 of 82 presets. Use a
  real XML parser.

### 3.3 Sensors and curves
One sensor:
```xml
<params id="pressure"><curve>0,0;1,1;</curve></params>
```
Several:
```xml
<params id="sensorslist">
  <ChildSensor id="speed"><curve>0,1;1,0;</curve></ChildSensor>
  <ChildSensor id="pressure"><curve>0,0.753769;1,1;</curve></ChildSensor>
</params>
```

**Parser traps.** The `<curve>` child is **omitted entirely when the curve is the identity**, so a
bare `<params id="pressure"/>` means "pressure, identity curve" — not "no curve". A sensor's
*activeness* is implied by its presence in the XML, never by an attribute.

Three more, found by parsing all **215** sensor params in the 118 presets of the CC-0 default bundle
(2026-07-09), none of them visible from the writer alone:

- **Every fragment opens with `<!DOCTYPE params>`**, then a space, then the root. All 215. A parser
  that expects the root element first rejects the entire default set.
- **`id` is not always the first attribute** — `<params rotationModeEnabled="0" id="fuzzy">` ships in
  6 of them. §3.2 warns about this for the preset document; it is equally true one level down. So the
  fragment cannot be sniffed with a `<params id="…"` prefix match either.
- **`rotationModeEnabled` is vestigial.** It appears on sensor roots in shipped files and has *no
  reader anywhere in the current source*. Unknown attributes must be ignored, not rejected.

Note also that the shipped files *do* write `<curve>0,0;1,1;</curve>` explicitly in many places, even
though the current writer omits an identity curve. Both forms are in the wild and both mean the same
thing; only the reading direction has a rule.

**Combination is not always multiply.** `{X}curveMode` selects `0 = multiply (default)`, `1 = add`,
`2 = max`, `3 = min`, `4 = difference` — but it governs only *one* of three sensor classes, and
`curveMode` applies solely when **two or more** scaling sensors are active (a lone scaling sensor is
taken verbatim, so `curveMode` is dead on 1-sensor options — the overwhelming majority).

> **Corrected 2026-07-09.** An earlier revision of this section said "additive sensors (drawing angle,
> rotation) bypass this and sum into a separate component." That is wrong twice over, and it matters:
> the two sensors it misfiles, `fuzzy` and `drawingangle`, are the 2nd- and 3rd-most-used sensors in
> the default set (§3.10). The classification below was read off the sensor class hierarchy —
> `isAdditive()` / `isAbsoluteRotation()` overrides, and their use in `computeValueComponents` — the
> same evidence standard as the rest of §3.

| class | sensors | value range | curve is applied… | how the results combine |
|---|---|---|---|---|
| **scaling** | the other eleven | [0,1] | directly | per `{X}curveMode`, into the multiplicative factor |
| **additive** | `rotation`, `ascension`, `fuzzy`, `fuzzystroke` | [−1,1] | in scaling space — map `0.5·(1+x)` in, `−1+2y` out | **summed** into a separate additive component; `curveMode` is bypassed |
| **absolute rotation** | `drawingangle` *(alone)* | [0,1) | with a half-turn wrap either side: `wrap(v+0.5)` in, `wrap(y+0.5)` out | **overwrites** a separate absolute-offset component (last active sensor wins); `curveMode` is bypassed |

So `drawingangle` is *not* additive — it replaces rather than sums, and its curve sees a rotated
domain. And three sensors beyond `rotation` *are* additive. A dab consumes all three components:
the scaling factor multiplies, the additive component adds, the absolute offset sets.

**Curves** are `"x,y;x,y;"` control points of a cubic spline over [0,1]², with an optional third
token `is_corner` per point (`0,1;0.5,0,is_corner;1,1;`). Default `"0,0;1,1;"`.

**Sixteen sensors exist:** `pressure, pressurein, tangentialpressure, drawingangle, xtilt, ytilt,
ascension` (tilt direction), `declination` (tilt elevation), `rotation, fuzzy` (per-dab random),
`fuzzystroke` (per-stroke random), `speed, fade, distance, time, perspective`. Three carry extra
attributes: `fade`/`distance` take `periodic` + `length`; `time` takes `periodic` + **`duration`**
(not `length`); `drawingangle` takes `fanCornersEnabled`, `fanCornersStep`, `angleOffset`,
`lockedAngleMode`. Their defaults when the attribute is absent: `periodic` false everywhere;
`length` **1000** on `fade` but **30** on `distance`; `duration` 30; `fanCornersEnabled` false,
`fanCornersStep` 30, `angleOffset` 0°, `lockedAngleMode` false.

**The three lengths are in three different units**, which the defaults alone do not reveal: `fade`
counts **dabs**, `distance` counts document **px**, `time` counts **milliseconds**. So `time`'s
default of 30 is a 30 ms opening ramp, not half a minute. `periodic` turns each from a saturating
ramp into a sawtooth. (Editor ranges, for calibration: `fanCornersStep` 5–90°, `angleOffset` ±180°,
`duration` 1–10000 ms.)

**An absent or empty `{X}Sensor` is not "no sensor".** When the XML names no sensor we can resolve,
the option falls back to **`pressure` with an identity curve** — the same floor the reference
implementation applies ("at least one sensor needs to be active"). Combined with the omitted-`<curve>`
trap above, this means a preset can drive an option from pressure without the string `pressure`
appearing anywhere in it.

### 3.4 Procedural tips
```xml
<Brush type="auto_brush" spacing angle randomness density useAutoSpacing autoSpacingCoeff BrushVersion="2">
  <MaskGenerator id type diameter ratio hfade vfade spikes antialiasEdges [softness_curve]/>
</Brush>
```
`id ∈ {default, soft, gauss}` × `type ∈ {circle, rect}` = **six generators**. `soft` carries a
`softness_curve`. `hfade`/`vfade` are stored internally as half their attribute value.

Three facts about these six, established 2026-07-09 by transcribing all six formulas and comparing
them against Mosaic's implementation over 12.5 M samples across 864 parameter combinations (agreement
within quantization, ≤ 2/255, everywhere the reference is not dividing by zero):

- **`hfade`/`vfade` are HARDNESS, not softness.** `hfade = 1` is a hard tip whose solid core reaches
  the rim; smaller values pull the shoulder inward. Confirmed twice: the `default` fade coefficient is
  `2 / (fade · width)`, so a larger fade puts the shoulder further out; and `gauss` derives its
  Gaussian width from `1 − (hfade + vfade)/2`, pointing the same way through completely different
  algebra. Confirmed a third time by the shipped set — Krita's plain hard round (`b)_Basic-5_Size_
  default`) is `hfade=1`, the chisel marker `0.93`, while `d)_Ink-1_Precision`'s gauss tip at
  `hfade=0.04` is so soft it carries **5 % coverage at 80 % of its radius**.
- **The `soft` falloff's `softness_curve` IS the profile**, evaluated on the *squared* normalized
  radius, and it must therefore **descend** — 1 at the centre, 0 at the rim. All 34 shipped soft tips
  do. The identity curve would build an inside-out tip, so it is the wrong default for a missing
  attribute; a linear `0,1;1,0;` is the right one.
- **The `Softness` curve option does nothing to a `gauss` tip.** The reference's two gauss generators
  never override the softness hook, so a preset pairing an enabled `Softness` option with a gauss mask
  is silently static. Faithful, and something the importer must report (§6.4) rather than paper over.

Two smaller quirks, both reproduced: the `default` **rect** alone folds `x` into the right half-plane
before applying `spikes`, mirroring its spikes relative to the other five generators (unobservable in
the shipped set — all four `spikes>2` presets are circles); and the `default` generators divide by
zero along the degenerate lines where the shoulder coincides with the rim, which Mosaic returns 0
coverage for rather than a NaN.

### 3.5 Predefined tips
Referenced by `filename` + `md5sum`, not embedded (in 2.2):
```xml
<Brush type="gbr_brush" filename md5sum spacing angle scale
       brushApplication ColorAsMask AdjustmentMidPoint BrightnessAdjustment ContrastAdjustment .../>
```
`brushApplication` is `ALPHAMASK=0, IMAGESTAMP=1, LIGHTNESSMAP=2, GRADIENTMAP=3` — checked directly in
the source, because two independent readings of this enum disagreed on the ordering. **Everything
except `ALPHAMASK` is per-dab colour**, i.e. exactly what §2.2 says our buffer cannot express.

Effective tip diameter is `scale × baseSize`, where **`baseSize` is `max(w, h)` of the tip raster** —
the long side, so a 40×10 chisel at `scale=1` is a 40 px brush.

**`BrushVersion` doubles `scale`, and its default is the doubling one.** The attribute is compared
against the string `"1"` with `"1"` as the fallback, so a tip element that omits `BrushVersion`
entirely is treated as legacy and has its scale doubled. Every one of the 47 predefined-tip references
in the 82 pixel-brush presets writes `BrushVersion="2"`, so no doubling happens in the default set and
a reader that gets this backwards will not notice until it opens a third-party preset.

**Preset `spacing` overrides the tip file's embedded spacing — and so does its absence.** GBR headers
carry a spacing field and PNG tips a `brush_spacing` text chunk; both are loaded into the brush and
then unconditionally overwritten by the `spacing` attribute, which itself defaults to `0.25`. So an
element with no `spacing` attribute yields 0.25, *not* the value in the tip file. The embedded spacing
is only ever a default for a tip opened outside a preset.

Legacy `ColorAsMask` is a **red herring**: the rule yields `IMAGESTAMP` only if the tip image *itself*
has colour **and** transparency. `ColorAsMask="0"` on a grayscale tip still means alpha mask.

⚠ **"Has colour" is a test on pixel content, not on the file's colour type**, and the distinction is
not academic: of the 17 distinct `.png` tips the pixel-brush presets reference, **12 are stored as RGB,
RGBA or indexed** and every one of them is grey in content. Transparency is likewise a content test —
"does any pixel have `alpha != 255`" — so an RGBA tip whose alpha is uniformly opaque counts as having
none. An importer that reads the PNG header instead of the pixels turns 12 grayscale alpha masks into
lightness maps, i.e. into per-dab colour, and silently promotes the whole default set out of the
`Uniform` accumulator's fast path.

**The three adjustments apply to every application *except* `IMAGESTAMP`** — not to `LIGHTNESSMAP`
alone, which is the natural guess. They are a piecewise-linear transfer curve on the greyscale value,
hinged at `(midpoint, 127 + brightness·128)`, with contrast steepening or flattening both limbs about
that hinge; alpha is untouched. `AdjustmentMidPoint` is **0…255** (default 127) while
`BrightnessAdjustment` and `ContrastAdjustment` are **−1…+1** (default 0), so the three do not share
a scale. Even when all three are neutral, a non-`IMAGESTAMP` tip that came from a colour raster is
still **desaturated** to grey before use; `IMAGESTAMP` is the sole application that keeps its colour.
None of the 47 predefined-tip references in the default set carries any of the three attributes.

`AutoAdjustMidPoint` replaces the midpoint with **the tip image's own average grey**, so the transfer
curve hinges on the image rather than on 127. It is a per-*image* quantity: the cells of one hose have
different averages and cannot share a transfer table. Its XML default is `0` — note that the *brush
model* defaults the same field to `true`, so the two layers disagree and only the XML default governs
a loaded preset.

**Two adjustment attributes are a migration, not data.** `AdjustmentVersion` defaults to `1`, and a
tip carrying version < 2 *and* no `AutoAdjustMidPoint` attribute — which is every tip in the default
set — has its three adjustments rewritten on load: the midpoint's offset from 127 is doubled,
brightness and contrast are doubled, and a negative contrast is remapped through
`1/(1−c) − 1`. (Older releases applied the curve twice; the doubling is an approximate undo, since
composing a piecewise-linear function with itself is quadratic and cannot be inverted by scaling.)
All three defaults are neutral, so the migration is a no-op for the whole default set — and will stay
invisible until an importer meets a third-party preset that sets them.

### 3.6 Tip file formats
| Format | Essentials |
|---|---|
| **GBR** | big-endian `{header_size, version, w, h, bytes, magic 'GIMP', spacing}`; `bytes=1` → 8-bit coverage; `bytes=4` → RGBA. v1 uses a shorter header and has no `spacing`. |
| **GIH** | `name\n`, `<ncells> <parasite>\n`, then N concatenated GBR blobs. Parasite: space-separated `key:value` (§3.6.2). |
| **ABR** | big-endian; v1/v2 and v6.1/6.2; **sampled brushes only** (computed/parametric are skipped); PackBits RLE per scanline. |
| **PNG/SVG** | raster/vector tip; colour → lightness map, grey → alpha mask (but see §3.6.1 — the test is on *content*). |

**ABR, GBR and GIH are *tip* formats, not engine formats** — they carry pixels. See §6.7.

#### 3.6.1 GBR and PNG store coverage the opposite way round
*(Corrected 2026-07-09. An earlier revision of the table said a `bytes=1` GBR holds "a grey mask
stored as `255−v`". That is exactly backwards, and the mistake renders every GBR and GIH tip as its
own photo-negative — a black square with a hole in it where the bristles should be.)*

There is one intermediate convention and one inversion, and it helps to name both. Call the decoded
tip raster the **tip image**: greyscale, where **white (255) is *no* paint**, like ink on paper. Every
loader's job is to produce that. Coverage is then derived exactly once, at mask time:

```
coverage = (255 − grey) · alpha / 255
```

What each loader must do to land in that convention differs, and *this* is where the sign lives:

| source | what the bytes hold | to reach "tip image" |
|---|---|---|
| **GBR `bytes=1`** | **coverage: 255 = full paint** | invert: `grey = 255 − raw`, `alpha = 255` |
| **GBR `bytes=4`** | straight RGBA | nothing (colour tip; see §3.5) |
| **GIH cell** | a whole GBR — so, as above | as above |
| **PNG** | **grey: 255 = white = no paint** | nothing |

So a `bytes=1` GBR is inverted **twice** on the way to coverage — once by the loader, once at mask
time — and the two cancel: **a GBR's raw byte *is* its coverage.** A PNG's is not; it is `255 − grey`.
A reader that applies "the" inversion uniformly gets exactly one of the two formats right.

Verified against the shipped files rather than reasoned about, because the round trip is self-
consistent in either direction. Rendering raw bytes as an intensity ramp: `chisel_knife.gbr` reaches
255 along the blade and 0 in the surround; `bristles_grouped.gbr`, `vegetal.gbr` and cell 0 of
`bamboo_leaves_random.gih` are likewise bright exactly where the tip has bristles. `hair.png` is the
mirror image — a 255 field with the hairs cut into it at 0. (`bokey_circle.gbr`'s centre reads *lower*
than its rim, which is not a counter-example: it is a ring.)

#### 3.6.2 The image hose parasite, and three shipped files that punish a trusting reader
The parasite is space-separated `key:value` tokens. Only four keys are read: **`ncells`**, **`dim`**
(clamped to 1…4), **`rank<i>`** and **`sel<i>`**. The other five that GIMP writes — `cellwidth`,
`cellheight`, `step`, `cols`, `rows`, `placement` — have **no reader at all** and must be ignored, not
rejected; 97 of the 100 shipped `.gih` carry all of them. `sel ∈ {constant, incremental, angular,
random, pressure, xtilt, ytilt, velocity}`, and **any unrecognised string means `constant`** (so does
the literal `constant`, which is never matched explicitly). `dim` must be parsed *before* `rank<i>` and
`sel<i>`, which are validated against it as they are read.

The flat cell index is a mixed-radix sum over the dimensions, `Σᵢ stride[i] · indexᵢ`, where the stride
is derived by **integer division of `ncells`**, not by multiplying ranks:
`stride[0] = ncells / rank[0]`, `stride[i] = stride[i−1] / rank[i]`. The sum is then taken **modulo the
number of cells that actually loaded**. Every one of those three details is load-bearing, because all
three of the following ship:

- **`fairy-dust.gih` (Krita_4, referenced by a pixel-brush preset) declares `ncells:4` and contains
  exactly one cell.** The file simply ends. A reader that trusts `ncells` reads off the end of the
  buffer; a reader that stops when the next cell does not fit registers 1 frame, and the final modulo
  turns every `random` draw in 0…3 into cell 0. **`ncells` is a claim, not a count.**
- **`A_bamboo-leaves.gih` (Krita_3) declares `ncells:3` with `rank0:5`.** `stride[0] = 3/5 = 0`, so
  `index₀ · 0 = 0` for all five draws: the hose has three cells and paints only the first, forever.
  Nothing sanitizes this — the guard upstream only fires when `rank` is *zero* and the mode is
  `incremental` or `angular`. Reproduce the integer division and you reproduce the file.
- **Three of the eight modes index one past the last cell at full scale**, and the final modulo is
  what saves them. `xtilt`/`ytilt` compute `round(t/2·rank) + rank/2`, which reaches `rank` at
  `t = +1`; `velocity` scales by `(rank−1) + 0.5`, i.e. `rank − 0.5`, which rounds to `rank`. So full
  speed and full tilt both wrap to cell 0 rather than landing on the last cell. `pressure`, sitting
  right beside them, scales by `rank − 1` and stays in range — the asymmetry is real, and it reads
  like a slip. It is nonetheless the arithmetic a hose was authored against. Do not "fix" any of the
  three with a clamp; that changes which cell paints at the top of the range.

Measured over both bundles (**101** `.gih` — 31 in Krita_4 + 70 in Krita_3, agreeing with both
manifests; an earlier revision said 100): **`dim` is 1 in every one**; `ncells` runs 3…24; `sel0` is
`random` **88** (was 87 — the same off-by-one), `incremental` 10, `pressure` 3. *(Re-measured
2026-07-10 through the implemented reader; every other figure reproduced exactly.)* Restricted to
the tips the 82 pixel-brush presets of Krita_4 actually reference, it is `random` 20 and
`incremental` 4 — **`pressure` is referenced by no preset in either bundle**, so `random` and
`incremental` are the whole of the shipped requirement.

### 3.7 `.bundle`
A ZIP: stored-first `mimetype` = `application/x-krita-resourcebundle`; `META-INF/manifest.xml` (ODF
namespace, `media-type` is the resource *folder* name, not a MIME type); `meta.xml` (Dublin Core plus
a user-defined license field); `preview.png`; payload folders `brushes/`, `paintoppresets/`,
`patterns/`.

### 3.8 `PaintOpAction` — a second accumulation model we don't have
`1 = BUILDUP`, `2 = WASH`. The stock pixel-brush preset ships `2`.

- **WASH** — opacity is a *cap*: overlapping dabs within one stroke build toward it and stop. This is
  what `BrushEngine::composite()` implements today.
- **BUILDUP** — no cap: each dab composites directly, so a stroke crossing itself keeps darkening.

BUILDUP is not expressible as a parameter of the current design — the coverage cap *is* the design.
It must be a mode (§6.1).

### 3.9 What the default set uses — the tiering evidence
Histogram over the 117 presets in the CC-0 default bundle:

| paintop | presets |
|---|---|
| pixel brush (`paintbrush`) | **82** |
| `colorsmudge` | 15 |
| `spraybrush` | 4 |
| `deformbrush` | 3 |
| `sketchbrush`, `filter` | 2 each |
| 9 others | 1 each |

Tips in the same bundle: **39 `.png`, 31 `.gih`, 6 `.gbr`, 3 `.svg`**. One engine buys 70 %; adding
`colorsmudge` and `roundmarker` reaches **84 %**. The animated image hose (`.gih`) is far more
load-bearing than `.gbr`.

### 3.10 Inside the 70 %: what the 82 pixel-brush presets actually use
**Tips** — `auto_brush` 35, `gbr_brush` 27, `png_brush` 19, `svg_brush` 1. So **47 of 82 are bitmap
tips**; of those 27 "gbr" references, **23 are `.gih`** files (the hose rides the GBR factory). Mask
generators: `default` 14, `gauss` 14, `soft` 7; shape `circle` 32, `rect` 3.

**Every bitmap tip in the whole default set is a grayscale alpha mask.** All 31 `.gih` files carry
`bytes=1` sub-brushes; the PNGs are `Grayscale`/`GrayscaleAlpha`; the GBRs are `bytes=1`. Zero image
stamps, zero lightness maps.

**GIH frame selection** — only two of the eight modes appear: `random` (27 files) and `incremental`
(4). `ncells` ranges 3…23.

**Enabled curve options** (count of presets): `Size` 59, `Rotation` 35, `Mirror` 11, `Scatter` 8,
`Opacity` 7, `Softness` 3, `Sharpness` 3, `Ratio` 2, `Spacing` 2, `h` 1, `v` 1. (`Texture/Strength/`
reads as 79, but real texturing is gated by `Texture/Pattern/Enabled`, on in only **16**.)

**Live sensors** — intersected with *enabled* options, because every option serializes a sensor even
when off: `pressure` in 70 presets, `fuzzy` in 23, `drawingangle` in 14, then a thin tail
(`ascension` 4, `declination` 2, `speed` 2, `tangentialpressure` 2, `fade` 1, `time` 1).
**Three sensors cover the set.**

> **Both tables count the raw `Pressure{X}` bit** *(clarified 2026-07-09, when the implemented reader
> reproduced every other number here exactly)*. For the two always-on options that bit is dead (§3.2),
> so the tables understate them: `Opacity` is enabled in **all 82**, not 7; `Flow` is enabled in all 82
> and is missing from the row entirely because its bit is never set; and `pressure` is consequently
> live in **all 82**, not 70 — every option floors to a pressure sensor when its XML names none.
>
> Nothing downstream changes. The conclusion is only reinforced: `pressure`, `fuzzy` (23) and
> `drawingangle` (14) cover the set, and the checkable options' counts — `Size` 59, `Rotation` 35,
> `Mirror` 11, `Scatter` 8, `Softness` 3, `Sharpness` 3, `Ratio` 2, `Spacing` 2, `h` 1, `v` 1,
> `Texture/Strength/` 79 — and the whole sensor tail reproduce to the unit.
>
> *One more footnote (2026-07-10, found reproducing the table through the preset library):* the
> two `tangentialpressure` presets carry it on options whose `{X}UseCurve` is **false** — the
> sensor is serialized but disabled at runtime (§3.2: `UseCurve` off collapses the option to its
> constant). Counted as serialized-on-enabled-options the row is 2, as this table does; counted
> as *actually live* it is 0, and the truly-live tail is thinner still.

**Blend modes are needed**: `normal` 74, `erase` 3, one each of `color`, `dodge`, `lighten`,
`multiply`, `overlay`. **`PaintOpAction`**: WASH 78, BUILDUP 4. `ColorSource/Type` is `plain` in all
82.

**`MaskingBrush/Enabled` is true in 6 of the 82** — a second brush painting a stroke-scoped mask
that modifies the stroke's accumulated alpha (§6.2 — and **not by multiply**: the six shipped
presets use `subtract` and `linear_dodge`, three each). Implemented; it also covers Photoshop's
dual-brush on import.

---

## 4. Bundled content & licensing

Findings, quoted from the upstream tree:

- `Krita_4_Default_Resources.bundle` → `meta.xml`: license **`CC-0`**; author *"Deevad with
  derivations of the brushes of Ramon Miranda, Razvanc, Radian, Wolthera, Storm, Scottyp and other."*
- `Krita_3_Default_Resources.bundle` → **`CC-0`**.
- `RGBA_brushes.bundle` → license field is literally `"Same as default brushes in Krita  "`.
  **Under-specified. Not shipped.**
- `Krita_Artists_SeExpr_examples.bundle` → `CC-BY-SA` (out of scope).
- `plugins/paintops/defaultpresets/defaultpresets.qrc` → `SPDX-License-Identifier: CC0-1.0`.
- The upstream `data/README` (covering the *loose* tip files): **CC-BY 3.0**, attributed to David
  Revoy, Blender Foundation and Ramon Miranda, *with an explicit exception*: "If you are a developer
  of an open source software, you can use them for the default preset in your code for your software
  without attributing."

**Decision.** Mosaic ships the `Krita_4_Default_Resources` presets and tips (CC-0), attributing
Revoy / Miranda / Blender in `docs/credits.md` anyway — courtesy, and insurance against a CC-BY loose
file leaking in. `RGBA_brushes` is not shipped. Per-file provenance is recorded at import so we can
always prove what came from where.

*This supersedes the earlier `PLAN.md` §3.12 / §7 stance ("we never ship Krita's GPL brush content"),
which rested on a false premise: the content is CC-0 / CC-BY, not GPL — and GPL content would have
been compatible with GPLv3 Mosaic regardless.*

### 4.1 Imported content: our posture
**Importing third-party brushes is a first-class goal.** Two problems get conflated here; only one is
ours.

**What a user opens is not our concern; what we redistribute is.** Reading `.abr` is reading a
long-reverse-engineered format that other free software has read for two decades, and file
formats are not copyrightable. Making import first-class changes nothing about what Mosaic itself
implements: the `Colored` accumulator is premultiplied source-over (Porter–Duff 1984); colour
dynamics and texture-grain compositing are equally old; ABR "computed" brushes are round/square tips
with hardness and roundness, i.e. our mask generators. §5's exclusions are untouched by import.

**Copyright in the brushes themselves is the user's business, and must stay that way.** The only
thing we answer for is **what we redistribute.**

1. **We ship only content whose licence we have vetted** (§4).
2. **We never inspect, report, or restrict what a user imports.** No telemetry, no allowlist, no
   "this brush may be copyrighted" nag. `PresetProvenance` (§6.4) exists to be honest about
   *fidelity* — "this was an ABR; its scatter was approximated". **It must never become a copyright
   cop.**
3. **We preserve whatever attribution a source file carries** (bundle `meta.xml` author/license
   fields, ABR brush names). That serves the user rather than surveilling them.

> ⚠ **Importing is neutral; publishing is not.** If Mosaic ever grows "export a brush pack" or a
> curated in-app brush browser, *we* become the redistributor and that content needs the same
> per-file licence vetting the default set got. This comment belongs beside any future pack-export
> code, not only here.

---

## 5. Technique families Mosaic will not implement

*This section records **Mosaic's own** implementation choices and the boundaries that constrain them.
It is not an assessment of any other project's code, and none is implied.*

> ### ⚠⚠ STANDING RULING — THE BRUSH-FACING GATES ARE WITHDRAWN *(maintainer, 2026-07-27)*
>
> **Nothing brush-facing is gated, as long as it is transcribed from the reference's published
> source.** This generalizes the 2026-07-18 opacity/flow decision to the whole brush arc: transcribe
> exactly, ship enabled by default, and credit the dated published GPL source recorded in
> `docs/brush-opacity-prior-art.md` — which is now the provenance register for **every** transcribed
> brush mechanic, and which the same ruling makes load-bearing rather than decorative. A mechanic
> shipped under this posture without a row there is a mechanic whose provenance was never written
> down.
>
> **What it withdrew, on the day:** the bristle-brush exclusion (below), the ⛔ screentone hold on
> `hatchingbrush` (§6.6b), and the standing hold on the §6.6(b) painters. All four remaining exotics
> were built the same day — §6.6g.
>
> **What it also unblocks but which is NOT built and is sequenced separately:** the per-pixel
> paint-load channel under smudge (upstream's `PaintThickness`) and the rope / pulled-string
> stabilizer. Each is its own design surface, not part of the engine arc.
>
> **What it does NOT change.** ⚠ This project still never characterises what a third-party project
> practises — in docs, code comments or commit messages. The exclusions below stay in the document as
> the record of what each technique family IS, and because they still say true things about
> mechanisms Mosaic does not implement.

- **Physical bristle simulation** (stiffness–height mapping; mass-spring bristle deformation).
  Mosaic ships no bristle simulation.
  > ⚠ **`d)_Ink-8_Sumi-e` WAS LISTED UNDER THIS EXCLUSION AND IS NOT AN INSTANCE OF IT. Maintainer's
  > ruling, 2026-07-27: the exclusion is LIFTED and the preset is built (§6.6g).** The exclusion's own
  > words are *stiffness–height mapping* and *mass-spring bristle deformation*; read against the
  > algorithm, neither describes it. What the hairy engine does, factually: rasterize the tip once
  > and turn every opaque pixel into a "bristle" (a fixed offset plus that pixel's alpha); per stroke
  > segment, put every bristle through **one** affine transform — a pressure-driven shear, a random
  > jitter, the size scale, the rotation — and draw a straight line from where it was to where it now
  > is; and count the marks each bristle has laid, reading a transfer curve at that count to make the
  > stroke dry out. There is **no mass, no spring, no stiffness, no height field and no bristle
  > physics state of any kind** — a bristle carries a previous position and an integer counter. It is
  > a scatter of correlated 1 px lines with a per-line ageing term. The exclusion was **name-driven**:
  > it described something the code never answered to. Mosaic still ships no bristle simulation, and
  > the exclusion above still stands for the technique it actually names.
- **Localized fluid / wet-paint simulation.** Mosaic ships no fluid engine. *Consequence:* `PLAN.md`
  §3.12's "Wet Edges" needs rethinking — a coverage-derived edge-darkening post-pass is a different
  mechanism from fluid simulation, and that is the shape any implementation here has to take.
- **Paint-load / "fill channel" colour spaces.** ⚠ **This one touches our own design.** Smudge is
  implemented as plain destination-sample-and-blend (Painter-era practice) and **not** by adding a
  per-pixel "paint load" channel to the accumulation buffer. That is the route taken deliberately.

> ⚠ **The paint-load caveat above is WITHDRAWN as a gate by the 2026-07-27 ruling.** It is still not
> built: a per-pixel paint-load channel is a change to the accumulation buffers themselves, with its
> own design surface, and it is sequenced as its own slice rather than folded into the engine arc.
> Until it is, upstream's `PaintThickness` stays dropped-and-badged (§6.6c) — for want of an
> implementation.

**Sequenced separately, and still not built:** the stabilizer ("pulled string" / rope smoothing).
⚠ Note that path INTERPOLATION (§2) is **not** a stabilizer and must not be confused with one: a
curve fitted *through the user's own samples* has no anchor, no lag and no filter. **Interpolate; do
not filter** — that is the standing rule on the walk. Texture and dual-brush compositing are likewise
sequenced on their own.

---

## 6. Engine rework

### 6.1 Accumulation — two axes
**Colour axis** — a `StrokeAccumulator` interface with two implementations:
- **`Uniform`** — today's coverage float + one colour. The fast path; the default hard round brush
  stays exactly as fast as it is now, and the opacity-cap invariant is unchanged.
- **`Colored`** — premultiplied RGBA float + coverage. Dabs deposit premultiplied colour weighted by
  dab alpha; `composite()` normalizes and applies the same opacity cap.

Chosen automatically from the preset (any tip application other than `ALPHAMASK`, or any active
`h`/`s`/`v`/`Mix`/`Darken`), and reported in the editor.

**`Colored` is in Arc A, not deferred.** The default set is 100 % grayscale `ALPHAMASK` with
`ColorSource/Type=plain`, so `Uniform` alone would ship all 82 presets at full fidelity. But
third-party import is a first-class goal (§4.1), and third-party packs are the opposite of our
defaults: ABR dual-brush and colour-dynamics presets, Procreate coloured grain, `.gih` hoses with
RGBA cells all stamp colour per dab. The perf objection is weak — `Uniform` remains the
auto-selected fast path, and the preset itself tells us which accumulator to use.

**Accumulation axis** — `PaintMode { Wash, Buildup }` (§3.8). `Buildup` bypasses the coverage cap and
composites each dab straight into the target; the coverage buffer is still maintained, because the
Inpaint brush reads it as a mask.

Erase becomes `StrokeMode { Paint, Erase }` (destination-out against the base snapshot). Blend mode
applies at composite via the existing `core::BlendMode` / `core/blend_math.hpp`.

All four combinations are reachable. **`Uniform × Wash` must stay byte-identical to today's output,
and that is a test.**

*(Built 2026-07-09: the modes first, on the `Uniform` accumulator, then `Colored`. Eight notes,
each of which cost something to find.)*

- **The blend math is in `core/blend_math.hpp`, not `render/blend.hpp`.** *(Corrected — an earlier
  revision of the line above said `render/blend.hpp`.)* `mosaic_render` finds Vulkan and links
  `mosaic::core`, so the engine core cannot include it without inverting the layering. The formulas
  are arithmetic on colours, not rendering, so they moved down to `core/`; `render/blend.hpp` is now
  a re-export and there is still exactly one copy of each formula.
- **`Buildup` accumulates beside the coverage rather than compositing each dab into the target.**
  For `Normal` the two are the same — repeated source-over of alpha `aᵢ·cap` gives
  `1 − Π(1 − aᵢ·cap)` either way — and accumulating keeps the region-refresh and `restore()`
  machinery, which both read from a pristine base. A true per-dab composite would also re-quantize
  to 8 bits at every dab.
- **That accumulator stores the CAPPED value where the coverage stores the uncapped one.** It has
  to: `1 − Π(1 − aᵢ·cap)` is not a function of the coverage `1 − Π(1 − aᵢ)`. The consequence is that
  a *single* dab — `a·cap` in both modes in real arithmetic — can land **one 8-bit level apart**
  between `Wash` and `Buildup` (6026 of 4.0 × 10⁹ sampled `(a, cap)` pairs disagree, all at very low
  alpha; the first at `cap = 0.0085`). Making the coverage round the same way would move `Wash`'s
  pinned bytes, so the difference stays. Both the identity and its one-level bound are tests.
- **`Normal` must not be routed through the blend path.** `(1 − ba)·fr + ba·blendChannel(Normal, br,
  fr)` is the identity in exact arithmetic, but `blendChannel` is float, so the paint colour makes a
  round-trip through `float`. A sweep of every 8-bit source channel against every backdrop alpha
  finds **58278 combinations where that flips an output byte** — so `composite()` short-circuits
  `Normal` to the double expression, and a golden case pins one of the 58278.
- **The eraser's ceiling is `opacity` alone**, not `opacity × color.a`: a semi-transparent swatch
  must not make the eraser weaker. Destination-out leaves the colour un-premultiplied, so erasing to
  alpha 0 and painting back does not drag a black fringe in.
- **`Colored`'s deposit weight mirrors the mode's own alpha accumulator** — `a` beside the Wash
  coverage, `a·cap` beside the Buildup accumulation — so normalizing the premultiplied buffer by its
  own alpha channel recovers exactly the colour that mode's compositing implies. Weigh Buildup's
  colours the Wash way and a two-colour stroke normalizes to the wrong mix (the exact two-dab pin:
  red at alpha 1 then green at 0.5 must land 153/102/0 at cap ½, not 128/128/0).
- **Normalization divides per channel; it must not multiply by a reciprocal.** IEEE guarantees
  `x/x == 1.0` exactly, while `x·(1/x)` can land one ulp under it — and that ulp is the difference
  between "a constant pure-colour `Colored` stroke is byte-identical to `Uniform`" (a test, across
  paint modes and the blend path) and a one-level flicker on whatever pixel happens to sit on a
  rounding boundary.
- **The per-dab colour hook's index is a property of the stroke's geometry**: it advances for dabs
  clipped off the document, so a stroke near an edge keeps the colours it would have had anywhere
  else. The test that pins this runs the identical stroke on two document sizes with a hook whose
  colour is a **non-periodic** function of the index — a cycling palette is blind to any index shift
  that is a multiple of its period, and the first version of the test (period 3) survived exactly
  that mutation.

### 6.2 Dab pipeline
```
sample → StrokeState (distance, time, speed EMA, drawing angle, fuzzy/fuzzystroke RNG streams)
       → evaluate each CurveOption  (sensors → curves → combine per curveMode)
       → Dab { center, diameter, ratio, angle, flow, mirror }   (core/brush/dab.hpp)
       → DabMask  (from BrushTip)
       → stamp                      (the masking brush is a SECOND dab walk -- see below)
```

> **Built 2026-07-12 (Arc D's engine wiring).** The sensors, the curves and the StrokeState had all
> been built and tested, and *nothing read them*. The dab walk does now.
>
> **The block is `Dab`, not `DabPlacement`.** An earlier revision of the line above called it that;
> `dab_mask.hpp` has owned the name `DabPlacement` since 2026-07-09 and it means something else
> entirely — *where a rasterized mask lands* (an integer corner and a quantized sub-pixel phase).
> This is what a dab **is**; that is where it **goes**. The doc is corrected to the code.
>
> **Two fields of the old list are deliberately absent from `Dab`, not forgotten.** `opacity` and
> `color` are not properties of a dab's shape; each changes the stroke's *accumulation model* rather
> than its placement. The per-dab opacity is DRIVEN now *(2026-07-14)* — but as the engine's own
> per-dab state beside the dab (`resolveDab` evaluates it, `DabDeposit` carries it), never as a Dab
> field. Colour still rides the existing `BrushDynamics::dabColor` seam until the colour-dynamics
> options land; it still needs its own goldens and its own commit.
>
> #### The per-dab opacity CEILING, transcribed *(landed 2026-07-14)*
>
> *(Upstream lineage, source files, licenses and publication dates for this step are recorded in
> `docs/brush-opacity-prior-art.md`.)*
>
> The reference's wash mode is **indirect painting**: dabs composite into a stroke-scoped temporary
> at `ALPHA_DARKEN` (its default *creamy* parameterization), and the temporary composites over the
> layer at the stroke's static opacity. Mosaic's coverage channel **is** that temporary's alpha, so
> the accumulation step for one dab at a pixel — mask value `m`, per-dab opacity `o` (sensors only,
> `useStrength=false`; the strength is the stroke cap), per-dab flow `f`, and the stroke's running
> average `avg` — is, with `lerp(a,b,t) = a + (b−a)·t`:
>
>     src  = m·o
>     full = avg > o ? (avg > A ? lerp(src, avg, A/avg) : A)     -- strive toward what the stroke
>                    : (o  > A ? lerp(A, o, m)          : A)        earned; NEVER step down
>     A'   = (f == 1) ? full : lerp(A, full, f)                  -- creamy zero-flow alpha = A
>
> `avg` is a **one-sided EMA** over the dabs laid so far: it rises to a louder dab instantly and
> decays toward a quieter one at 0.1 per dab (`avg' = avg < o ? o : 0.1·o + 0.9·avg`, from 0). It is
> what keeps a fading stroke from carving a staircase down through its own paint — and both branches
> refuse to lower `A`, which the *returning* stroke needs as much as the fading one (the plain
> branch's guard only runs when `avg ≤ o`, i.e. on a steady or rising stroke crossing paint it laid
> when the average had already decayed — a fading stroke exercises only the average branch, and a
> test that drove one could not see the other's guard at all).
>
> ⚠ **At `o = 1` the step equals the static accumulation `A + f·m·(1−A)` in real arithmetic but NOT
> in float grouping** — so the engine takes the transcribed path only when a preset's Opacity is
> genuinely dynamic (`optionIsDynamic`, the same predicate the importer's honesty contract reads),
> and the static path's expression is untouched to the bit. ⚠ WASH ONLY: Buildup is direct painting
> with its own untranscribed per-dab composite; the gate stays shut there and the import badge says
> so. ⚠ The per-dab value and the average advance once per dab, in `resolveDab`, clipped and
> zero-flow dabs included — they are properties of the stroke's geometry, like the dab counter, and
> they run across a span rewind like the random streams. `softness` is absent for a different reason — see the option table.
>
> **Twenty-six option bases ride the pipeline today** (`core::brush::kDrivenOptions`): `Size` (59 of
> the 82 shipped presets), `Flow` (all 82), `Rotation` (35), `Ratio` (2), `Softness` (3), `Opacity`
> (82), the smudge trio (§6.6c), the two positional ones — `Scatter` and `Mirror` (§6.6d) —
> `Spacing` and `Sharpness` (§6.6e), the colour trio `h`/`s`/`v` (§6.6f), the sketch/curve four and
> the hatching four (§6.6g), and `Texture/Strength/` + `Rate` (§6.6h). Three bases are still out and
> the table says why. The count is not the point; the pipeline is. The table keeps each option's
> landing record (struck through) or the fact it still waits for:
>
> | option | presets | why it waits |
> |---|---|---|
> | ~~`Softness`~~ | 3 | **Landed 2026-07-12, with the tip.** It scales a *mask generator's* softness ("1 = as authored"), and the engine used to walk an analytic falloff parameterized by **hardness** — a different quantity, so wiring the option meant inventing the map between the two. The engine stamps through a real generator now, so the option scales the thing it was written for and there is no map to invent. |
> | ~~`Opacity`~~ | **82** | **Landed 2026-07-14, transcribed — and it was worth more than the rest put together (11 → 42 Exact).** A per-dab opacity is a per-dab **ceiling** — not a per-dab flow, which builds up *within* a stroke — and the reference's indirect (wash) painting splits the option exactly in two: the *static strength* is the whole stroke's ceiling (`BrushParams::opacity`, applied at composite, unchanged since S14), and the *sensor value alone* (`useStrength=false` — folding the strength in twice would square it) rides every dab into the wash accumulation, which now takes the transcribed per-dab-ceiling step (`washAlphaDarkenAlpha`, below). ⚠ WASH ONLY: Buildup is the direct path, whose own per-dab opacity/flow composite is not transcribed — a dynamic Opacity there keeps the static ceiling, badged (2 shipped presets). ⚠ The static path's accumulation expression is untouched, deliberately: at opacity 1 the transcribed step equals it in *real* arithmetic but not in float grouping, and every wash golden pins the static bytes. |
> | ~~`Scatter`~~ | 11 | **Landed 2026-07-14, transcribed (§6.6d).** `jitter = (2·rand01 − 1) × max(maskW, maskH) × sizeLikeValue` per enabled axis (`Scattering/AxisX`/`AxisY`, default true) — two independent draws when both, one draw laid along the drawing angle (X) or its normal (Y) when one. Strength spans **[0,5]**; the pre-2.x `Scattering/Amount` is the strength when `ScatterValue` is absent. Rides BOTH walks — the reference's colorsmudge scatters its dab position too. |
> | ~~`Mirror`~~ | 11 | **Landed 2026-07-14, transcribed (§6.6d).** One size-like draw per dab; `≥ 0.5` flips every enabled axis (`{Horizontal,Vertical}MirrorEnabled`, default false). The dab's angle passes through UNCHANGED — the reference's "negate the rotation, then flip the raster" equals this pipeline's "flip the tip in its own frame, then rotate", exactly, because a flip conjugates a rotation into its inverse. ⚠ NEVER under the smudge walk: the reference's colorsmudge ignores its Mirror option (widget offered, paintop never constructs it), so the mapper drops it there — badge-free, because dropping it IS the faithful stroke. |
> | ~~`Spacing`~~ | 4 | **Landed 2026-07-18, transcribed (§6.6e).** A per-dab CADENCE scale, not a dab-shape option: `computeSizeLikeValue` WITH strength over [0,1], multiplying the WHOLE spacing interval (both axes, every branch) exactly as the reference's `spacing *= extraScale`, BEFORE the half-pixel floor. Rides both walks. ⚠ NOT `Spacing/Isotropic`, the static ellipse opt-out (§3.2) that shares only its prefix. |
> | ~~`Sharpness`~~ | 3 | **Landed 2026-07-18, transcribed (§6.6e).** Two effects off one per-dab value: an alpha THRESHOLD on the mask (`sharpnessThreshold`, checked → 8-bit `tolerance = 255 − v·255`, hard 1-bit at value 1) and a pixel-grid SNAP of the centre (only when `Sharpness/alignoutline` + strength > 0). ⚠ NEVER under smudge (colorsmudge installs no sharpness option — badge-free drop, like Mirror). Krita 6 reads `Sharpness/softness` (default 0); the Krita-4 `Sharpness/threshold` key is dead. |
> | ~~`h`/`s`/`v`~~ | 1 | **Landed 2026-07-18, transcribed (§6.6f).** The three HSV adjustments to the paint COLOUR (not the tip). Hue is a `rotationLikeValue` — a half-turn hue rotation (`h += value·180`°); saturation and value are size-like values through the reference's remap `val = 2·(rawWithStrength·strength + (0.5 − 0.5·strength)) − 1`, landing in [−strength, +strength] (the strength squared, on purpose — the reference multiplies it in twice). Evaluated in the reference's order — hue, saturation, value — each drawing from the random stream ONLY when checked, and resolving the Colored accumulator's per-dab colour (`applyColorDynamics` → `hsvAdjust`, the non-compatibility `HSVTransform<HSVPolicy>` branch, run in `float` and quantized to 8-bit per dab). ⚠ NEVER under smudge: the walk paints the stroke's own colour, so any colour-dynamics option is dropped there and BADGED (not a faithful drop like Mirror — the reference's colorsmudge does adjust the colour). Only `v)_Texture_Impressionism` (h + v, both fuzzy) drives any of them; `s` is unused across the whole shipped set. |
> | ~~`Density`, `Line width`, `Offset scale`, `Curves opacity`~~ | 3 | **Landed 2026-07-27 (§6.6g).** The SKETCH and CURVE engines' own, read through the reference's `KisStandardOption::apply` like every other option (checked → size-like value WITH strength, unchecked → exactly 1.0) — what makes them different is their CONSUMER: only those two `StrokePainter`s read them, so an active one on the wrong paintop is a dropped option, badged. That is the smudge trio's caveat with a different walk. ⚠ Three of the four base names CONTAIN A SPACE (`PressureLine width`, `Line widthValue`, …) — the format's, not a typo. ⚠ `Line width` is shared by BOTH engines, and upstream gives it two different consumer ranges ([0,1] on sketch, [0.1,1] on curve); one base, one spec, and the wider range wins. No shipped preset checks any of the four, so they cost no census move. |
> | ~~`Angle`, `Crosshatching`, `Separation`, `Thickness`~~ | 1 | **Landed 2026-07-27 (§6.6g).** The HATCHING engine's four — and hatching is a DAB engine, so unlike the row above these ride `resolveDab` beside the smudge quartet rather than a painter. Drawn LAST among the per-dab draws (appended after the colour dynamics, so every prior golden's stream is byte-identical), in the reference's own assignment order angle → crosshatching → separation → thickness. Their three CHECKED gates freeze at `begin()`, because the reference's PASS SELECTION reads the checkboxes and not the values. Same single-consumer caveat: honoured by the hatching stencil alone, badged anywhere else. |
> | ~~`Texture/Strength/`~~ | **21** | **Landed 2026-07-28 (§6.6h) — the largest single census move left, and it was worth 20 presets.** The strength of a document-locked pattern composited into every dab's own alpha, AFTER the Sharpness threshold and nothing else. ⚠ The base name CONTAINS SLASHES and ends in one (`PressureTexture/Strength/`, `Texture/Strength/Value`), like the three bases that contain a space. The static half is a separate `Texture/Pattern/*` block baked ONCE into an 8-bit mask (luma over white, brightness, contrast, invert, a two-segment neutral point, two cutoff policies) and tiled across the DOCUMENT, so the grain sits still while the brush moves. Rides BOTH walks — the reference installs its texture option on the brush-based paintop base `colorsmudge` derives from, so a textured smudge really does texture, which is the exact opposite of Mirror's and Sharpness's drop-on-smudge. Two of sixteen modes transcribed (multiply, subtract); the other fourteen import as multiply, badged. |
> | ~~`Rate`~~ | 10 | **Landed 2026-07-28 (§6.6h).** Not a dab option at all: it scales the AIRBRUSH's timed dab interval (`1000/rate`, divided by this value, floored at 0.5 ms), so the stroke walk gains a second cadence and lays a dab at whichever of the two comes first. ⚠ A rate value of 0 is "never", not "instantly". Drawn LAST among the per-dab draws, for the reason the Spacing scale is drawn late — the reference re-computes its timing after laying a dab, to size the interval to the next one. The cadence's clock is the SAMPLES' OWN timestamps, never a wall clock, so the mark stays a pure function of the sample stream; the wall-clock half (synthesizing a sample while the pointer rests) is the reference's TOOL half and is owed to the canvas. |
> | `Mix`, `Darken` | — | The two colour-dynamics bases still out: `Mix` needs the background colour and an alpha-weighted mix; `Darken` a Lab-16 round-trip. No shipped preset drives either. |
> | `LightnessStrength` | — | A later tier: it scales how hard a LIGHTNESS-MAP tip's own lightness drives the deposit, and no shipped preset carries a lightness-map tip at all. |
>
> ⚠ **The list of what a dab reads is `core::brush::kDrivenOptions` (dab.hpp), and it is ONE list.**
> The importer's honesty contract (`optionSupported`, which decides whether a preset imports as Exact)
> and the preset→engine mapping (`preset_brush.cpp`, which decides what a stroke is laid with) both
> read it. They did not, once: the importer's copy named `Scatter`, `Mirror` and `Spacing` — three
> options no dab has ever read — and presets carrying them imported as **Exact and painted without
> them**. The biconditional (`driven ⟺ has a slot`) is now a test, base by base, and it has to be:
> neither side can see the other's failure alone.
>
> **An absent option is not a disabled one.** It contributes exactly the identity, and `x * 1.0 == x`
> to the bit — which is what keeps a stroke with no options byte-for-byte the stroke the engine laid
> before any of this existed. An option that is present but **unchecked** contributes the identity
> too, **not its strength**; `Rotation` needs that gate more than `Size` does, because a rotation-like
> value is re-centred on zero, so a leak there does not merely resize the dab, it spins it a half turn.
>
> **A rotation-like value is in HALF turns.** `rotationLikeValue` doubles the canvas angle into its
> [-1,1) space (a normalized base angle of 0.25 — a quarter turn — reads as 0.5), so π, not 2π,
> carries it into radians. An ellipse has a period of π, so a full-strength sweep turning the dab a
> half turn each way is its *whole* range.

#### The dab walk lags the sample stream, and the state has to lag with it
The walk is **one sample behind** the stream: fitting a curve *through* a sample means knowing where
the path goes next (§6.2's `stroke_path`), so the span ending at the newest sample is not stamped
until its successor arrives. `StrokeState` is folded per **sample**, as samples arrive — so by the
time a span is stamped, the live state has already advanced *past* it.

Evaluate a dab against the live state and every `distance` / `speed` / `time` / `drawingangle` sensor
reads **one sample into the future**. So `m_path` keeps a `StrokeSnapshot` beside every sample, and
the walk brackets each span: rewind the state to the dab's own point (`lerpSnapshot` lands *between*
two samples, which is where almost every dab actually is), stamp, put the live state back so
`strokeState()` still reports the pointer rather than the last dab. **The lag itself stays** — it is
what makes the curve interpolate.

A snapshot carries only what is a property of a *point* on the stroke. The two random streams, the dab
counter and the latched drawing angle are properties of the **stroke**, and keep running across a
rewind — or `fuzzy` would repeat itself once per span.

A dab's **heading is the curve's local tangent** where it sits, read off the flattened polyline — not
a blend of the two bracketing samples' chord directions. On a straight span the two are bit-identical,
which is exactly why **only a curved stroke can tell them apart**: a mutant that dropped the tangent
survived every straight-line test in the file until one was written that curves. Measured on a
quarter-circle, a heading-following nib sweeps a 4.25 px ribbon; blend the chords instead and it cants
off the path and sweeps 5.75.

The spacing cadence reads the diameter the dab was **resolved** at rather than re-deriving it from the
pressure. Re-deriving would re-run the option pipeline for a dab already laid — drawing a second
`fuzzy` and advancing the dab counter twice, so `fade` would ramp at double speed.

#### The tip lands in the engine
*(Built 2026-07-12.)* `MaskGenerator`, `BitmapTip`, `DabMask` and `DabMaskCache` were all built and
unit-tested in **Arc A**, and the engine stamped through *none* of them: it walked its own analytic
circle, parameterized by `hardness`. Every preset in the shipped set — 35 procedural tips, 47 bitmap
ones — would have painted as the same smooth disc.

`BrushParams::tip` is a shared `BrushTip` (`core/brush/brush_tip.hpp`): one of the six procedural
generators, or a decoded bitmap. **NULL is not "no falloff"** — it is the analytic circle every
brush-family tool that is not driving a preset still paints with, and which every golden in the suite
was laid by. So the goldens did not move.

- The two paths differ in **where a pixel's coverage comes from and in nothing else**, so they share
  one `deposit()`. Three accumulators, a first-touch base snapshot and two bboxes are not a thing to
  keep two copies of.
- **The mask is rendered from the cache's KEY, never from the raw request.** The cache is exactly
  transparent only because the dab is quantized *first*; two dabs in one size bin are the same dab,
  byte for byte. That is a test.
- **A real tip supersedes `hardness` entirely.** Sweep hardness across its range under a fixed tip and
  the bytes must not move — also a test.
- **An animated tip's cell is chosen once per dab, in the once-per-dab step, *before* the dab can be
  clipped away.** A `Random` hose dimension draws from the same stream the `fuzzy` sensors do; choose
  it after the off-document return and a stroke at the edge lays a different cell sequence than the
  same stroke in the middle of the canvas.
- A bitmap tip stamps **coverage only**. `BitmapTip` stores 8-bit coverage planes and nothing else,
  whatever its `application` says — so an ImageStamp tip stamps its *shape* and deposits the stroke's
  colour. Per-pixel dab colour is an accumulator that does not exist (the colour hook is per *dab*).

#### Giving the tip a shape, without moving a pixel
`BrushParams` gains `ratio` (height/width) and `angleRad`. `stamp()` maps each pixel into the tip's own
frame — undo the rotation, then the aspect — which turns any ellipse back into the circle the falloff
is written for, so one `dabCoverage` serves every shape.

**At ratio 1 and angle 0 that map is the identity in IEEE arithmetic**, not merely to within a rounding
error: `cos(0)` is exactly 1, `sin(0)` exactly 0, `x * 1.0 == x`, and the sums are only ever squared.
So a circular unrotated dab reduces to `sqrt(dx*dx + dy*dy)` — the very expression the loop evaluated
before the tip had a shape — and lays **bit-identical bytes**. Every wash golden, every straight-path
golden, `Uniform × Wash` and every mouse stroke are untouched. There is deliberately **no `if
(circular)` fast path**: a branch would be provably equivalent to the arithmetic, which makes it dead
weight to reason about and a mutant no test could ever kill.

The dab's bounding box follows the tip's real semi-axes. Sizing it from the width alone is merely
wasteful for a *squashed* tip — and **clips a tall one** (`ratio > 1`), shearing its ends flat.

> ⚠ **The masking brush is a stroke-level modifier, not a per-dab mask multiply.** *(Corrected
> 2026-07-09 against the upstream renderer and all six shipped masking presets. An earlier revision
> of the pipeline above said the dab mask is "optionally multiplied by a masking-brush tip", and
> §3.10 said the mask "multiplies the first's dab" — wrong twice, in mechanism and in operator.)*
>
> - The masking brush paints its **own stroke** — the same path, but its own tip, spacing, and
>   size/rotation/scatter options with their own sensors — as a stroke-scoped grayscale mask
>   (white, full opacity). Nothing happens per *primary* dab; the two dab walks don't even share a
>   cadence.
> - At projection the mask modifies the paint stroke's **accumulated alpha** per pixel:
>   `alpha' = op(mask, alpha)`, applied **before** the stroke's opacity ceiling.
> - The op key is **`MaskingBrush/MaskingCompositeOp`** (not the guessable `…/CompositeOp`),
>   default `multiply`. The reference accepts ten ids — `multiply, darken, overlay, dodge, burn,
>   linear_burn, linear_dodge, hard_mix_photoshop, hard_mix_softer_photoshop, subtract` — and an
>   **unknown id falls back to `multiply`**.
> - **No shipped preset uses multiply.** The six use `subtract` (3: Dry_Bristles_Eroded and both
>   Waterpaints carve the mask texture *out* of the stroke) and `linear_dodge` (3: the Charcoal
>   pencils *add* it). All six set `MaskingBrush/UseMasterSize=true`, so the masking tip's size is
>   `MasterSizeCoeff` × the master size (coeffs 0.45…1.15), resolved at load — the engine sees an
>   absolute size. Upstream caps it at `min(15000, 3 × max-brush-size)`.
> - **The mask must never paint alone.** A pixel only the masking stroke touched has no paint
>   colour under it; the reference's `linear_dodge` explicitly zero-guards that case, but its
>   `hard_mix_photoshop` does not (mask 1 over stroke 0 fires, depositing the temp device's
>   transparent black) — an artifact no shipped preset reaches, because every one keeps the masking
>   tip at or below the primary's size. Mosaic gates on the paint stroke's own accumulation instead,
>   which subsumes the zero-guard and closes the corner for every op.
> - **Masking × Buildup is unreachable upstream** — the masking renderer requires the indirect
>   (wash) painting path, and the stroke strategy asserts as much. Mosaic defines the combination as
>   the op applied to whatever accumulated alpha the mode produces (Buildup's is per-dab-capped, so
>   the op lands post-cap there; Wash's lands pre-cap, matching upstream).
> - Mosaic accumulates the mask with the engine's own wash accumulation; upstream paints it with
>   `ALPHA_DARKEN`. The difference shows only where the masking stroke overlaps itself — a flow
>   nuance for the Arc B conformance pass to measure, not guess at.
>
> Mosaic implements `multiply` + `subtract` + `linear_dodge` now — the default plus everything the
> shipped set uses. The other seven ids wait for the importer, where each gets the transcribed-oracle
> treatment (`dodge`/`burn` in particular sit behind clamp-policy template indirection that must be
> swept, not skimmed); until then they import as `multiply` with a fidelity note, exactly as an
> unknown id would.
>
> **The masking walk stamps the REAL nested tip** *(built 2026-07-14; it stamped a round analytic
> disc parameterized by a derived `hardness` before that, which very nearly deleted
> `g)_Dry_Bristles_Eroded` — an eroded-texture SUBTRACT mask stamped as a solid disc subtracts the
> whole nib: 74 marked preview pixels of 14,080, against 2,722 with the real tip)*. An **auto**
> nested tip builds its generator at map time (`resolveMasking`); a **predefined** one resolves in
> the library through the same md5 → filename chain and the same decoded/built caches as the
> primary's, with `application = AlphaMask` unconditionally — a masking stroke is a grayscale
> value, so there is no colour for a content verdict to find a use for. A miss keeps the analytic
> disc, badged (`maskingTipsResolved`/`maskingTipsFallback` count it). The shipped six: four auto
> circles (the three charcoals + Waterpaint_Soft's softness-curve `soft` generator) and two
> bitmaps — the eroded-debris `.gbr` at **153 × 64 (not square)** and the rough-square `.png` at
> 454 × 448. The masking dab rasterizes through the same dab cache as the primary (its own raster
> id keeps the two apart), frame 0, no mirror, softness as authored: the nested table's per-dab
> options (Rotation, Scatter, Opacity curves) are still not driven — only the PressureSize /
> PressureFlow gates are, as before.

`BrushTip` is a variant:
- `MaskGenerator { kind: Circle|Rect, falloff: Default|Soft|Gauss, diameter, ratio, hFade, vFade,
  spikes, antialias, softnessCurve, angle, randomness, density }` — the six generators of §3.4,
  analytic, evaluated per pixel.
- `BitmapTip { frames[], application: AlphaMask|ImageStamp|LightnessMap|GradientMap, selection:
  Constant|Incremental|Angular|Random|Pressure|TiltX|TiltY|Velocity, adjustments{midpoint,brightness,
  contrast} }` — covers gbr/gih/png/svg/abr.

**`DabMask` LRU cache**, keyed on (tip id, quantized width, height, angle, softness, sub-pixel phase,
frame, both mirror flags). Without it the mask-generator path is O(area) trigonometry *per dab*.

*(Built 2026-07-09. The key above is the one that shipped: an earlier revision of this line listed
"diameter, ratio" — width and height carry the same information more directly — and omitted
**softness** and the two **mirror** flags, all three of which change the raster. `Softness` is a live
option on 3 presets and `Mirror` on 11, so a key without them returns the wrong mask.)*

**The cache is exactly transparent, and that is a requirement rather than a happy property.** The
dab's continuous parameters are quantized *first*, and the mask is then rendered from the quantized
values — so a hit and a miss return the same bytes, and disabling the cache changes performance and
nothing else. A stroke may not look different because memory happened to be tight.

Two consequences worth stating, because both are easy to violate later:
- **Quantization is a fidelity decision, taken once**, not a caching detail. It is also what buys the
  hit rate: a stroke at constant size and angle has only `subPixelSteps²` distinct masks however many
  dabs it lays.
- **A key is meaningless without the quantization that minted it** — the same step counts decode to a
  different shape under a different one. The cache therefore owns its `DabQuantization`, and changing
  it empties the cache. A tip's brightness/contrast adjustments are baked into its coverage planes at
  construction and are *not* in the key, so `tipId` must identify a tip's raster rather than the tip
  object: editing a tip in place while its id stays fixed is the one remaining way to make the cache
  lie.

Behaviour changes to call out explicitly:
- **Spacing keys off the effective (pressure-scaled) size**, plus `useAutoSpacing`/`autoSpacingCoeff`.
  *(Built 2026-07-10. The step to the next dab is re-resolved after EVERY dab from that dab's own
  pressure — the reference updates its spacing per dab, not per segment — and the last dab's
  pressure carries across segment boundaries. Auto-spacing is an ABSOLUTE step:
  `coeff * sqrt(diameter)` px for tips ≥ 1 px, linear below — so a bigger brush lays relatively
  denser dabs; it ignores the fractional `spacing`. The masking walk gained the same pair and keys
  off its own pressure-scaled diameter, resolved by the same helper that sets its stamp radius.
  This re-blessed exactly the one promised golden — "pressure drives size and flow" — and no other.)*

#### ⚠ SPACING IS AN ELLIPSE, NOT A SCALAR *(fixed 2026-07-12, 15th session — a USER-REPORTED bug)*

The cadence above keyed off the dab's **width alone**, and that is wrong for every tip that is not
round. The interval is a fraction of **both** of the dab's extents, and the step actually taken is
that ellipse's **radius in the direction of travel**:

    sx = spacing · width      sy = spacing · height        (rotated by the dab's own angle)
    step(θ) = 1 / √( (cos φ / sx)² + (sin φ / sy)² )   ,   φ = θ − angle

`i)_Wet_Knife` is the case the user found: a **75 × 15** dab (ratio 0.2) turned a quarter turn, with
`spacing="0.08"`. It should step **6 px along its blade and 1.2 px across it**. We stepped 6 px in
every direction — **five times too sparse across the thin axis**, which is exactly what a knife
dragged sideways looked like. All 22 non-circular shipped tips had it.

- ⚠ **A ROUND TIP IS A DELIBERATE BRANCH (`sx == sy` → return `sx`), NOT A FAST PATH.**
  `1/√(cos²+sin²)` is *not* exactly 1.0 for an arbitrary angle, so putting a round tip through the
  ellipse arithmetic moves its dabs by an ulp — and **every golden in the suite was laid by a round
  tip**. Note this is the exact **opposite** of the tip-frame map in `stamp()`, where the general
  arithmetic *is* the bit-exact identity at ratio 1 and a branch would be an unkillable mutant. Here
  the branch is load-bearing and an equality test pins it. *(This is why **not one golden moved**
  when the fix landed — which is not reassurance, it is the reason the new cases had to be written.)*
- ⚠ **The extents come from the TIP (`tipDabShape`), never from `(diameter, diameter · ratio)`.** A
  bitmap tip's `ratio` is **1** by construction — its frame's aspect lives *inside* the envelope — so
  that pair is a **square for every bitmap preset in the corpus**, and a cadence derived from it is
  round for all of them. The tip is the only thing that knows the frame.
- ⚠ **The half-pixel floor is per AXIS, not on the step.** The two agree exactly on either axis and
  differ **only off-axis**: a 64 × 0.5 nib steps 0.71 px at 45° with a floored minor axis, but 0.50
  if you floor the step instead — half again too dense. **A test that drags along the two axes passes
  on both.** The axes are the backstop; the **diagonal is the primary check**. (Third time this arc.)
- **Auto-spacing is applied per axis**, so it is an ellipse too.
- **`Spacing/Isotropic`** (§3.2, default false) is the author's opt-out: the cadence reads the
  **larger** extent and is the same in every direction, and the angle goes with it. **2 shipped
  presets set it**, both bitmap chalks. It is a static flag, *not* the per-dab `Spacing` **option**
  (still unsupported) — so honouring it costs no fidelity.
- **Where we deliberately differ from the reference.** The reference derives its two intervals from
  the **axis-aligned bounding box** of the rotated tip and then passes only the *per-dab* rotation to
  the walk — so a tip whose **authored** angle is not a multiple of 90° loses its ellipse's
  orientation entirely (a 5:1 nib at 45° gets a **square** box, hence an isotropic cadence). We carry
  the ellipse in the **tip's own frame**, which agrees with the reference exactly wherever the
  reference is right — **every shipped preset**, whose authored angles are all 0 or a quarter/half
  turn — and is strictly better where it degenerates. This is a *documented* divergence, not a guess.
- The **masking** walk followed *(2026-07-14, with its tip)*: its cadence is the masking tip's own
  ellipse read along the local tangent, through the same `spacingStepAlong`. With no tip (or a
  round one) both extents are the one diameter, so the scalar branch fires and the analytic
  masking walk steps bit-for-bit as it always did — but the eroded-debris masking bitmap is
  **153 × 64**, and a scalar cadence dragged across its thin axis would under-stamp it 2.4×, the
  very bug this section exists to record.
- Bitmap tips need **bilinear resample at sub-pixel phase**; `docs/tablet.md` finally supplies
  sub-pixel positions.
- Segment interpolation stays linear in v1; Catmull-Rom behind a flag.

### 6.3 GPU, and where the shader work actually is
Stamping stays CPU, behind the engine API (`PLAN.md` §S19-a). But `MaskGenerator::coverageAt(x, y,
params)` is factored as a *pure function* so it can later be mirrored formula-for-formula in GLSL —
the CPU/GPU parity-lane idiom already proven by `extrude_render` vs `extrude_raster.comp`, with a
parity test.

**The shader work that is in scope now is the reticle.** `canvas_present.comp` draws a circular size
ring. **The reticle always traces the actual tip** — there is no outline-style setting, because a
circle over an elliptical, rotated or textured tip is simply a lie. Upload the next dab's mask, trace
its iso-contour at coverage ≈ 0.5, luminance-keyed like the existing lasso line, reusing the
lock-punchout logic already in the shader.

> **It traces the tip's SHAPE, at the tip's CONFIGURED size. It does NOT track live pressure.**
> Settled with the user 2026-07-11, and it is what Krita, Photoshop, GIMP and Clip Studio all do: the
> outline is a *targeting* aid — "where will paint land if I commit" — and a ring that shrinks and
> grows with every pressure sample is noise you cannot aim with. (It would also have to collapse to
> nothing on hover, where pressure is 0.) No setting: one answer is correct.
>
> ✅ **PAID 2026-07-12**, in the commit after the one that gave a tip a `ratio`/`angle` — the debt
> this note booked. The ring is an **ellipse**: the canvas reads the tip's shape out of the very
> `BrushParams` the engine is handed, so the two cannot disagree, and `ui::reticleShape()` turns that
> into a screen-space ellipse. The view's zoom is isotropic (`canvas_view.hpp`'s
> `docToScreen = Rc · (T · S · T)`), so it scales both semi-axes alike and cannot shear a circle into
> an ellipse; the view's rotation simply **adds** to the tip's own angle, and there is no mirror term
> to flip its sense.
>
> The shader's `tipDist()` is **exact for a circle, by construction** — at `a == b` the squash factor
> is exactly 1.0, the metric correction divides by exactly 1.0, and a rotation by 0 is the identity —
> so it reduces to `length(d) - R`, and the pixel-tuned antialiasing of the round reticle (every brush
> that ships today) is bit-for-bit what it was. A test pins that with 125 exact-equality checks.
> The metric correction is the part a naive "squash it into a circle and measure there" gets wrong:
> that map is not an isometry, so a thin nib's ring would visibly thin out at its flat ends.
>
> The ruling itself is unchanged: the reticle traces the tip's **configured** shape, it does **not**
> track live pressure (a ring that breathed with the pen would be useless as the size gauge that is its
> only job), and it is **not a setting**. What a user reads as "the reticle does not follow the tip" is
> either (a) pressure thinning the dab under a ring that deliberately does not follow it, or (b) a SOFT
> tip, whose ink visibly fades well inside its own diameter — both correct, both what every comparable
> editor does.

> ⚠⚠ **AN ELLIPSE WAS NOT ENOUGH, AND THE USER SAID SO.** *(Corrected 2026-07-12, the day the note
> above was written, against a real complaint: pick `i)_Wet_Bristles_Rough` and a perfect oval is drawn
> over a bristly tip.)* The note above paid the debt for **elliptical** tips and quietly assumed every
> tip is one. **47 of the 82 shipped pixel-brush presets carry a bitmap tip, and not one of them is an
> ellipse.** An oval over a bristle, a spatter, a ring or a spiked star is the same lie the *circle*
> was, in a smaller size — and it is a lie about the one thing the reticle exists to tell.
>
> **The reticle now traces the tip's real contour.** The whole ring — the antialiasing, the 3×3
> supersampling, the three Settings→Appearance line styles, the lock punch-out — is built on one
> function, `tipDist(d, a, b, θ)`: the **signed distance in screen px** to the tip's outline. That
> contract is kept, and `tipDist` simply gains a **second implementation**, so nothing downstream had
> to change.
>
> - **`core/brush/tip_outline.{hpp,cpp}`** (pure, FLTK-free, Vulkan-free, unit-tested) builds a
>   **signed distance field** of the tip's silhouette: rasterize the tip at its own envelope
>   (`tipDabShape` — a bitmap tip's box is its *frame's*, not `diameter × diameter·ratio`), threshold,
>   and run an **exact** Euclidean distance transform (Felzenszwalb & Huttenlocher's two-pass squared
>   EDT). A chamfer's error is anisotropic — the ring would sit tighter on the diagonals than on the
>   axes — so the exact transform is the requirement, not the optimization.
> - **The threshold is `coverage != 0`, never 0.5.** Any coverage at all is inside. A soft tip fades
>   to nothing well before its rim; trace it at half coverage and the ring lands deep inside the tip's
>   real extent and **understates the brush's size**, which destroys the one job it has. (It is also
>   what the reference does, and it is why a traced outline is a shade *generous* — the right side to
>   err on.)
> - **`tipNeedsSdf`** decides who traces, and is a test. **False** — the analytic ellipse, evaluated in
>   closed form — for a **NULL tip** and for a **plain circle generator with `spikes ≤ 2`**: for those
>   the ellipse *is* the outline, exactly, and rasterizing it would only approximate what the shader
>   already draws perfectly. ⚠ The NULL-tip branch is not an optimization: it is what keeps every golden
>   and the 125 bit-exact antialiasing equalities intact. **True** for every **bitmap** tip, every
>   **spiked** generator and every **rect** generator (the shader's closed form knows only ellipses; a
>   ring drawn for one over a rectangle would be the same lie again).
> - **The field lives in the TIP'S OWN frame** and rides a **new storage binding (9)** — not a tail on
>   the reticle's binding 4, whose struct ends in a flexible array member (`lassoPts[]`) that nothing
>   can follow. The diameter, the zoom, the view's rotation and the cursor's position are all applied
>   when the shader **samples** it, so none of them rebuilds it: it is re-traced only when the tip's
>   **raster** (`BrushTip::id`) or the dab's **ratio** changes — once per preset pick, not once per
>   mouse move.
> - ⚠ **The `ratio` is in that key and looks as though it should not be.** The squash is an affine map
>   of the tip's box, so for a bitmap tip and five of the six generators the field *could* be built once
>   and stretched. Not for a **spiked** one: it folds the **raw, un-normalized** offset into an angular
>   wedge *before* the falloff normalizes it, and folding commutes with an isotropic scale and not with
>   an anisotropic one. At ratio 1 the fold is a rotation, which preserves `x² + y²` — so a five-spiked
>   circle's silhouette **is** a circle, star or no star. Squash it and the star appears. A field built
>   at ratio 1 and stretched afterwards would draw a smooth oval over the one tip whose entire point is
>   that it is not one.
> - **The shader converts the stored (normalized) distance back to screen px by dividing by the
>   magnitude of the field's screen-space gradient**, finite-differenced through the same mapping —
>   the *same* metric correction the analytic path applies, and for the same reason: the tip-to-screen
>   map is anisotropic whenever the dab is squashed or the tip's box is not square, and without it a
>   flat-ended nib's ring visibly thins out at its ends. ⚠ That gradient **vanishes on the field's
>   medial axis** (a disc's centre, the spine of a gap between two bristles), so it is floored at the
>   smallest value it can honestly take — `min(k)/boxW` — not at an epsilon, which would turn a
>   perfectly good distance into 4·10⁷ px.
> - **Fallbacks**, both from the reference: a tip below ~1.5 px on screen does **not** trace (its
>   contour is a blob that says less than a plain ring; the ellipse takes over, and the *field is not
>   rebuilt* — the size gate is a per-frame decision about what to draw, not a different tip). An empty
>   or oversized field falls back to the ellipse too, rather than being clamped: half an outline is
>   worse than an honest oval.
>
> ⭐ **STILL OWED: the visual pass, and it is now TWO checks, because two different code paths draw
> the ring.**
>   1. **The TRACED contour** — the new path, and the one the complaint named. Nothing about it is
>      verifiable headlessly: the C++ half (the field, the decision, and a line-for-line mirror of the
>      shader's sampler, gradient correction and extrapolation) is under test; the GLSL that consumes
>      it is not. **Pick `i)_Wet_Bristles_Rough` and look.** Worth also checking a `.gih` hose (the
>      ring draws frame 0's silhouette by design) and a shaped tip at high zoom, where the 128 px
>      build grid would show up first if it is too coarse.
>   2. **The ANALYTIC ellipse** — the OLD path, still owed from the 13th session and still unseen. A
>      plain oval nib does not take the traced path at all (`tipNeedsSdf` is false for it), so
>      check 1 does not cover it. 11 shipped presets have a genuinely elliptical tip (both knives at
>      ratio 0.2, the Watercolor Fringe at 0.17, three charcoals at 0.8) and 11 more carry an
>      authored angle (the chisel Markers and flat Bristles at a quarter turn, the Tilted Pencil at a
>      half). **Pick `i)_Wet_Knife` and look at the ring.**

Two details this forces:
- **Multi-frame (`.gih`) tips.** 27 of the 31 shipped hose tips select frames at `random`, so "the
  next dab's mask" is not knowable before the dab happens. Draw frame 0's silhouette at the current
  size/angle rather than letting the reticle flicker per motion event. `incremental` tips (4) can show
  the true next frame.
- **Tilt indicator** waits for tablet input — there is no tilt to show until then. *(Still true: the
  ring now **turns** with tilt, below, but nothing draws the pen's LEAN as a glyph.)*

#### ⚠ THE RETICLE FOLLOWS DIRECTION AND IGNORES MAGNITUDE *(fixed 2026-07-12, 16th session — a USER-REPORTED bug)*

The user reported that the tip "does not respond to direction". **Half of that was true, and it was
the visible half.** The *stamp* already followed the stroke's heading and the pen's tilt, end to end,
on both backends, with a test. **The ring never did.** `brush_reticle.hpp` computed its angle as
`tip's authored angle + canvas view rotation` and stopped — it never saw the stroke state, never
evaluated the `Rotation` option, never saw the pen. So a knife that turns to follow the stroke sat
under a ring showing its authored slant. *(The other half of the impression is authorship, not a bug:
only **14 of the 82** native presets ask to follow the stroke. The other 68 genuinely do not rotate.)*

**The ruling: the reticle turns with every sensor that is a DIRECTION and none that is a MAGNITUDE.**
§6.3 already forbids the ring from breathing with the pen — *"a ring that breathed with the pen would
be unusable as a size gauge, which is the one job it has"* — and that argument says **nothing whatever
about an orientation**, which is precisely a statement about *where the paint is going to land*.

⚠ **The split costs no judgement call, because the SENSOR CLASSIFICATION already draws it**
(`sensors.cpp`): `drawingangle` is the sole `AbsoluteRotation` sensor; `rotation` (barrel) and
`ascension` (tilt **bearing**) are `Additive`; and every magnitude — pressure, speed, `declination`
(tilt **angle**), fade, distance, time — is `Scaling`. So the whole rule is `rotationLikeValue` with
the scaling part switched off, a lever the arithmetic already had. A **random** rotation
(`fuzzy`/`fuzzystroke`, 13 presets) is not a direction either and is refused: a ring re-rolling the
dice on every motion event would be true and useless, and evaluating it would draw from the stroke's
random stream.

⚠ **ONE RULE, ONE FUNCTION.** `dabAngle` is factored out of `evaluateDab` and the reticle calls the
same function, because **the bug WAS a drift between two copies of the rule**. A test pins the
biconditional: for any heading, what the ring draws *is* what the dab takes.

⚠ **`HoverHeading` measures TRAVEL ON SCREEN and the HEADING IN THE DOCUMENT.** The two frames are not
interchangeable. The noise being filtered is screen-space input jitter (a mouse reports **integer**
positions at 60 Hz), so a document-space threshold would demand a 2 px hand tremor at 10 % zoom and
wave a 200 px sweep through at 1000 %. And the angle must be a *document* angle, because the view's
rotation is added back when the ring is drawn — measure it on screen and it is applied **twice**. Both
mutants die.

### 6.4 Conformance tiers
| paintop | presets | tier | approach |
|---|---|---|---|
| pixel brush | 82 | **Native** | full fidelity |
| `colorsmudge` | 15 | Approximate | destination-sample + blend; **no paint-load channel** (§5). Map `SmudgeRate`, `SmudgeRadius`, `ColorRate`; drop `PaintThickness`. |
| `roundmarker` | 1 | Native | trivial |
| `spraybrush` | 4 | Approximate | particle scatter within radius |
| `filter` | 2 | Approximate | reuse `render/effect_primitives` |
| `sketchbrush` | 2 | Native | the `StrokePainter` web engine (§6.6g); both presets keep only the Buildup-Opacity caveat |
| `hairybrush` | 1 | **Native** | the `StrokePainter` bristle engine (§6.6g) — `d)_Ink-8_Sumi-e`, Exact |
| `curvebrush` | 1 | Native | the `StrokePainter` sliding-window engine (§6.6g); Buildup-Opacity caveat only |
| `particlebrush` | 1 | **Native** | the `StrokePainter` simulation engine (§6.6g) — Exact |
| `experimentbrush` | 1 | **Native** | the `StrokePainter` whole-stroke fill (§6.6g) — Exact; displacement / speed / smoothing badged |
| `hatchingbrush` | 1 | Native | a DAB engine, not a painter (§6.6g): a procedural lattice stencilled by the tip mask |
| 4 others | 6 | **Substituted** | import → nearest pixel-brush equivalent, flagged |

Every imported preset carries
`PresetProvenance { sourceFormat, sourcePaintop, fidelity: Exact|Approximated|Substituted, droppedOptions[] }`,
surfaced as a badge in the editor and a corner dot in the preset grid — the same honesty-counter
discipline as the `docio` layer.

*"Substituted" means Mosaic's engine has no native equivalent: the preset is imported as its nearest
pixel-brush approximation and flagged. The column describes Mosaic's own engine coverage, nothing
else.*

### 6.5 What the non-pixel-brush 30 % costs
Not a uniform 30 %. `paintbrush + colorsmudge + spray + filter + roundmarker` = **104/117 = 89 %**;
`deformbrush` (3) belongs to a future Liquify tool and `duplicate` (1) to S38 Clone Stamp, taking it
to 92 %. Nine presets are dropped. *(2026-07-27, §6.6g: the whole `StrokePainter` tier plus
`hatchingbrush` landed, so **111/117 = 95 %** have a native engine before Liquify and Clone Stamp,
and **99 %** after. Exactly TWO presets are then left with no home at all — `k)_Blender_Pixelize`
(`gridbrush`, Tier 5) and `w)_Texture_Normal_Map` (`tangentnormal`, Tier 3) — and both are scope
calls with a costed plan, not blocked on anything.)*

### 6.6 Why those nine — four different reasons, not one
The tell is which method an engine overrides. A dab-stamper implements `paintAt()` and lets the base
class walk the path at `spacing`; anything overriding `paintLine()` is doing its own thing with the
stroke.

**(a) ~~Excluded by policy — 1.~~ NOT EXCLUDED — BUILT 2026-07-27 (§6.6g).** The bristle-brush preset
(`d)_Ink-8_Sumi-e`) was listed here because §5 excludes bristle simulation. **Read against the
algorithm, it is not one** — no mass, no spring, no stiffness–height mapping, no bristle physics state
at all — and the maintainer lifted the exclusion on 2026-07-27. §5 carries the factual account; §6.6g
is the transcription. The exclusion itself stands for the technique it actually names.

**(b) ~~Not dab engines at all — 5.~~ ALL FIVE BUILT 2026-07-27 (§6.6g).** These override
`paintLine()`; the stroke's *history or state* is the drawing input, not a path to stamp along:
- ~~`sketchbrush` (×2)~~ — **BUILT.** Keeps a point history and, per new sample, draws Wu/DDA **lines
  back to earlier points within a threshold distance**. The web of connections *is* the mark.
- ~~`curvebrush`~~ — **BUILT.** Keeps a sliding window of the last N points, paints a quadratic path
  through them.
- ~~`particlebrush`~~ — **BUILT.** A **persistent particle simulator** — initial position set once,
  then the particles chase the cursor each segment. State, not geometry.
- ~~`experimentbrush`~~ — **BUILT.** Accumulates the **entire stroke** into one path and fills it on
  release. A whole-stroke polygon op; it cannot be previewed incrementally — and it is not, which is
  the one deviation §6.6g records for it.

`BrushEngine` walks a path stamping masks. None of these is expressible in that model. Supporting them
means a parallel `StrokePainter` abstraction — its own live preview, option pages and settings model —
to serve five presets. **Not because they are hard individually**; because they are a second engine
kind. *(That abstraction exists as of 2026-07-27 — §6.6g. It is not "parallel" in the end: a painter
writes into the engine's own accumulation, so only the mark-making is a second kind, and the three
remaining presets here are S each on top of it.)*

**(c) Dab-based, but the dab's content is a canvas-aligned procedural pattern — 2, of which 1 is
BUILT (§6.6g).** `gridbrush` builds a lattice of cells at `diameter × grid_scale`; ~~`hatchingbrush`
derives from the brush-based base class and hatches lines at an angle/separation into the dab~~ —
**BUILT 2026-07-27**, and this paragraph's prediction is exactly what it turned out to need. Both
**do** fit our pipeline if a dab may source "procedural pattern clipped by the tip mask" rather than
only a mask; hatching now does, and `gridbrush` (Tier 5) can reuse the same seam.

**(d) A value call, not a cost one — 1.** `tangentnormal` derives from the brush-based base class: an
**ordinary pixel brush whose dab colour is computed from stylus tilt/direction/rotation**. Both
ingredients — the `Colored` accumulator and tilt sensors — are on the plan for other reasons, so it is
close to free once they land. It only makes sense for normal-map / 3D-texturing workflows. Worth
revisiting if Mosaic grows a texturing story.

> ⚠ **These reasons do not share a cause.** Only (a) came off §5's exclusion list. (b), (c) and (d)
> were scope calls. *(2026-07-27: the standing ruling in §5 withdrew every brush-facing hold for anything
> transcribed from the reference's published source, and (a), (b) and the gated half of (c) were all
> built on the strength of it — §6.6g. What remains unbuilt here is unbuilt for scope: `gridbrush`
> (Tier 5) and `tangentnormal` (Tier 3). The provenance register that the posture rests on is
> `docs/brush-opacity-prior-art.md`.)*

### 6.6b The paintop arc — real engines for the 15 exotics *(scoped 2026-07-12, 16th session)*

**There is no cull.** The user was offered one and refused it twice — *"I don't want to remove Ink-8
Sumi-e… Let's just implement support for those brushes… And implement support for the rest"*, and then
explicitly kept the two filter presets as well. All 117 stay; the 15 that import as plain pixel brushes
are **owed real engines**. This is the plan, and it is ordered by leverage, not by taste.

#### ⭐ THE ONE FACT THAT REORDERS EVERYTHING: `m_base` ALREADY EXISTS

`BrushEngine` already holds **`m_base` — the pristine PRE-STROKE pixels over the working rect**,
lazily grown tile-aligned by `ensureCovers()` and filled on first touch in `deposit()`. That is
*exactly* the "old data" semantics every canvas-reading paintop needs: they all sample the destination
**as it was before the stroke began**, never the live buffer.

So **"let the dab walk READ the destination"** — which every prior estimate treated as a large new
subsystem — is not one. It is: snapshot the **full dab footprint** before stamping (today `deposit()`
snapshots only the pixels it actually touches), and hand `stampTipDab()` a read-only bilinear view of
`m_base` under the dab. Call it **`DabSource`**.

⚠ **`DabSource` reads `m_base`, NEVER `m_target`.** Reading live pixels inside a stroke makes the mark
depend on composite cadence, and breaks the goldens, undo replay and the incremental-refresh contract
in one stroke. This is the guard rail; it is not negotiable.

⚠⚠ **BUT "as it was before the stroke began" was WRONG for colorsmudge, and the correction is
§6.6c** *(landed 2026-07-14)*. Read against the reference source: colorsmudge reads and writes the
**live layer device per dab** — the smear CHAIN, each dab reading what the previous dabs wrote, *is*
the mechanism, and a base-only read carries paint at most one dab-step before dropping it. The
guard rail's **intent** (never read `m_target`; no cadence dependence) survives intact: the smudge
walk keeps a **stroke-local state buffer** seeded from `m_base` (the DabSource snapshot, bulk form:
`seedSmudge` + `m_baseFilled`), reads and writes *that*, and `composite()` copies it out. Same
chain, fully deterministic — the composite-cadence test pins it byte-for-byte. The base-only
reading stays exactly right for `deformbrush`/`filter`/`duplicate`/`gridbrush`, which genuinely
sample pre-stroke data.

#### 🔥 TIER 1 — `DabSource` unlocks **22 presets** on one commit

| paintop | presets | what it reads for |
|---|---|---|
| `colorsmudge` | **15** | ✅ **DONE 2026-07-14 (§6.6c)** — the patch under the previous dab (smear) or an average colour (dulling), via the stroke-state buffer above |
| `deformbrush` | 3 | a bilinear sample at an inverse-warped coordinate |
| `filter` | 2 | the region under the dab, to run a blur/unsharp kernel over |
| `duplicate` | 1 | a patch at a document-space **offset** (the clone anchor) |
| `gridbrush` | 1 | the canvas colour under each lattice tile's centre (that *is* the pixelize) |
| | **22** | |

**Build this first, build it once.** Cost: **S–M**, not L — because `m_base` is already there.
*(Built: the snapshot primitive + the smudge consumer shipped together; the bilinear pre-stroke
read view for the other four is a small addition on the same seeded machinery.)*

#### TIER 2 — three of these are TOOLS, not brushes *(S each, after Tier 1)*

The user already plans separate Blur/Dodge/Burn tools. **Route these there; do not write brush engines
for them.** Each is `DabSource` plus something the tree already has:

- **Blur / Sharpen** (2 presets) — `DabSource` → an existing `render/effect_primitives` kernel →
  composite through the dab mask. The upstream `filterop` is literally *"run a filter, stencil it with
  a brush tip"*. §6.4 already planned to reuse the primitives.
- **Clone Stamp** (1) — `DabSource` at a document-space offset + a persistent anchor. This **is** S38.
- **Liquify** (3) — `DabSource` + bilinear + a per-dab inverse warp. §6.5 already said `deformbrush`
  belongs to a future Liquify tool.

#### TIER 3 — `tangentnormal` (1 preset) — the cheapest thing on the list

Not a new engine at all: it converts the pen's tilt bearing/elevation into a unit 3-vector and
**swizzles it into the paint colour's RGB**, then stamps an ordinary dab. Every ingredient already
exists — the four orientation sensors (`Ascension`/`Declination`/`Rotation`/`DrawingAngle`), the
`Colored` accumulator, and the `dabColor` seam. It is **a colour-dynamics option**, ~60 lines, and it
rides along free the day colour dynamics land. §6.6(d) called it low-*value*; it is low-**cost**, which
is a different axis.

#### TIER 4 — `StrokePainter`: the second engine kind → **6 presets, incl. Sumi-e**

> ⭐ **THE WHOLE TIER IS BUILT — 2026-07-27, see §6.6g.** Scaffold + all six presets:
> `sketchbrush` ×2, `hairybrush`/Sumi-e, `curvebrush`, `particlebrush` and `experimentbrush`. The
> paragraphs below are the SCOPING as written in the 16th session; where the build disagreed with
> them, §6.6g says so and §6.6g wins. (Two things it got right that were worth the scoping: the
> `finish()` seam `experimentbrush` needed, and the prediction that a painter would ride the
> existing accumulation rather than a parallel one.)

The five remaining exotics do not stamp a mask along a path — they **draw their own geometry**. One
abstraction serves all of them: a painter that gets `(StrokeSnapshot a, StrokeSnapshot b)` per span
from `walkSpan()` (which already produces exactly that polyline) and writes into the **existing**
`m_coverage`/`m_colored` accumulation — so Wash/Buildup/Erase, blending, masking and undo all keep
working untouched. Two shared rasterizers fall out: a **DDA/Wu line with a 2×2 bilinear splat** (hairy,
sketch, grid) and a **stroked path** (curve, experiment).

Cheapest-first *within* the tier: **scaffold + line rasterizer** (no presets — pure enabling) →
**`hairybrush`/Sumi-e** (the one the user asked for) → `sketchbrush` (2) → `curvebrush` (1) →
`particlebrush` (1) → `experimentbrush` (1, **last** — it fills the *whole accumulated stroke* as one
polygon and so fights `composite()`'s incremental pending-region model).

⚠ **§6.7's verdict still holds and must stay in the doc: `StrokePainter` buys ≈ 0 import breadth.**
Price it on these 6 presets plus Mosaic's own future stroke-history tools, never on format coverage.

##### ✅ SUMI-E: THE §5 EXCLUSION WAS NAME-DRIVEN, AND IT WAS LIFTED *(maintainer's ruling, 2026-07-27)*

§5 excludes *"physical bristle simulation (stiffness–height mapping; mass-spring bristle deformation)"*,
and §6.6(a) excluded `d)_Ink-8_Sumi-e` on that basis. **Read against the actual algorithm, that
description does not match.** What the source does: rasterize the tip once and turn **every opaque
pixel into a bristle** (position + a length from its alpha); per stroke *segment*, put each bristle
through **a single affine transform** (rotate · scale · jitter · a pressure-driven shear); draw a line
from that bristle's previous position to its new one; and run a per-bristle **ink-depletion counter**
through a transfer curve, which is what makes the stroke dry out. **There is no mass-spring model and
no stiffness–height mapping anywhere in it** — there is no bristle physics state at all beyond a
previous position and an ink counter.

That was reported in the 16th session as a fact about the mechanism, with the note that the exclusion
appeared to have been written against a description the code does not answer to. **The maintainer
settled it on 2026-07-27: the exclusion is lifted, and the preset is built (§6.6g).** §5 carries the
same factual account beside the exclusion, which still stands for the technique it actually names.

#### TIER 5 — `gridbrush` (1)

A lattice **phase-locked to the document origin** (not the dab), subdivided, one shape per sub-tile.
Half of it — the per-tile canvas read — is **free after Tier 1**, dropping it from M to S/M. Build it
if someone wants a Pixelize *brush*; the Pixelize *filter* already covers the use case.

#### ✅ ~~NOT BUILT — ⛔ GATED~~ — `hatchingbrush` (1 preset: `y)_Screentone_Moire`), BUILT 2026-07-27

Document-locked parallel lines at an angle and separation, with a second pass at a differing angle to
produce the moiré. ~~This IS the screentone/halftone mechanic, and it was held back pending its own
design pass; no implementation may be written until that comes back.~~ **The §5 ruling of
2026-07-27 withdrew that hold along with every other brush-facing one, and the paintop is
transcribed — §6.6g.**

⚠ **It is NOT a `StrokePainter`, and establishing that was the first thing the build did.** It
derives from the reference's brush-**based** paintop base and overrides `paintAt`, not `paintLine` —
so it is a dab engine, it rides the dab walk and the spacing cadence untouched, and the only new
thing it needs is precisely what paragraph (c) of §6.6 predicted: a dab may source a procedural
pattern clipped by the tip mask. Cost: S, on the dab pipeline, not M on the painter scaffold.

⚠ Note the other two screentone presets — `y)_Screentone_Pressure` and `y)_Screentones_Regular` — are
ordinary `paintbrush`es that already worked: they are *textured pixel brushes*, not a tone engine. Do
not confuse the three.

#### Scoreboard

| after… | natively supported | cost |
|---|---|---|
| **Tier 1 — `DabSource`** | +0 directly, **unlocks 22** | **S–M** ← *first* |
| Tier 2 — Blur/Sharpen, Clone, Liquify **as tools** | 6/15, + colorsmudge's 15 promoted | S each |
| Tier 3 — `tangentnormal` | 7/15 | S |
| ✅ Tier 4 — the whole `StrokePainter` tier: scaffold + `sketchbrush` ×2 + **Sumi-e** + `curvebrush` + `particlebrush` + `experimentbrush` *(2026-07-27, §6.6g)* | **6/15 done** | M scaffold + S each |
| ✅ `hatchingbrush` — a DAB engine, not a painter *(2026-07-27, §6.6g)* | **7/15 done** | S |
| Tier 5 — `gridbrush` | 14/15 | S/M |

*(After 2026-07-27 the list of shipped presets with no native engine is **6**: `deformbrush` ×3
→ Liquify, `duplicate` → Clone Stamp, `gridbrush` → Tier 5, `tangentnormal` → Tier 3. Every one of
those has a home on the plan already; none of them is blocked on anything.)*

⚠ **§6.6's four-reason taxonomy is structurally correct and held up against the source — but its COST
estimates are now stale.** `m_base` landed after §6.6 was written, and that single fact moves 22
presets from "medium-large" to "small, once".

⚠ **The smudge caveat from §5 stands and is not softened by any of this.** `DabSource` — reading the
destination — is generic and unconstrained. What is held back is the **per-pixel paint-load channel**.
Build smudge as **plain destination-sample-and-blend** and **do not add a load channel to
`m_coverage`/`m_colored`**. Upstream's `PaintThickness` option is exactly the thing §5 sets aside:
**drop it, do not map it.**

### 6.6c The smudge engine — colorsmudge, transcribed *(landed 2026-07-14, 19th session)*

**The 15 `colorsmudge` presets run a real smudge engine now** (`SmudgeParams` +
`BrushEngine::stampSmudgeDab`), transcribed from the reference's colorsmudge paintop in its
**legacy** parameterization — the strategy every shipped preset actually selects (grayscale
alpha-mask tips; `SmudgeRateUseNewEngine` absent → false). VERIFIED format facts, from source:

- **Strategy selection**: LIGHTNESSMAP tips → lightness strategy; `useNewEngine` + ALPHAMASK →
  the new mask strategy; IMAGESTAMP/GRADIENTMAP → stamp; **everything else → MaskLegacy** — all 15.
- **Legacy constants** (each an override in the legacy strategy): smear op = **COPY at rate 1**
  (a verbatim patch copy, `smearAlpha` notwithstanding); dulling rate = **1** (a solid fill);
  final blt = COPY when `smearAlpha` (default **true** — alpha lerps DOWN too, a smudge eats
  paint) else OVER, at painter opacity `smudgeRate × opacity`; colour-rate opacity =
  `clamp01(lerp(0, max(1 − maxSmudgeRate, 0.2), colorRate × opacity))` — the **0.2 floor** is what
  keeps a full-rate Wet paint wet.
- **The per-dab values** (`computeSizeLikeValue`, strength INCLUDED — colorsmudge is DIRECT
  painting, so the Opacity strength rides per dab and there is NO stroke-level cap; the exact
  inverse of §6.2's wash split): `smudgeRate` = checked ? strength×sensor : **1.0**; `colorRate` =
  checked ? strength×sensor : **0.0** (what makes a blender a blender); `smudgeRadius` = checked ?
  strength×sensor : 0.0, range [0,3]; `maxSmudgeRate` = the SmudgeRate STRENGTH, read regardless
  of its checkbox.
- **The first dab paints nothing** (`m_firstRun`): it plants the anchor. The source patch is the
  dst dab rect translated by `round(prevRectCenter − curRectCenter)` — INTEGER, rect centres, not
  cursor points. **Smearing mode disables sub-pixel precision** for the whole stroke (upstream bug
  327235: the smear must copy aligned areas) — dulling keeps it.
- **Dulling samples** the legacy `AveragedSampleWrapper`: a plain mean (≡ componentwise mean of
  premultiplied RGBA) over **Halton-sequence** points (bases 2/3, the integer generator
  transcribed verbatim) of `blowRect(srcRect, 0.5·(radius−1))` ∪ the centre pixel —
  `min(n, max(64, 2% of n))` samples, then 16-sample batches until the straight-space mean moves
  ≤ 2 8-bit levels. `blowRect` TRUNCATES toward zero, so the radius migration matters (below).
- **`SmudgeRadiusVersion` < 2 stores PERCENT** (every Krita-4 preset; the key itself is absent):
  divide the RAW strength by 100, then cap at the old engine's 3.0 — **raw first, clamp after**:
  clamp-first turns an authored 300 into 0.03 instead of 3.0. The mapper owns the migration; the
  [0,3] spec range clamps at evaluation.
- **`KoCompositeOpCopy2`** (the final blt) = channels lerped PREMULTIPLIED, then unpremultiplied
  by the lerped alpha — which is why the engine's stroke-state buffer is premultiplied float: the
  whole blt is a componentwise lerp there, and the dulling mean is a plain mean.

**The Mosaic shape** — and the one structural deviation, argued in §6.6b: the reference reads and
writes the live device per dab (the smear CHAIN is the mechanism); Mosaic keeps a stroke-local
premultiplied float **state buffer** (`m_smudgeState`) seeded from `m_base` via the DabSource bulk
snapshot (`seedSmudge` + `m_baseFilled` — the explicit fill flag, because the smudge walk reads
pixels it never deposits into, so coverage can't be the gate), evolved by the dabs, copied out by
`composite()` wherever coverage > 0. Never reads `m_target`; composite cadence is pinned
byte-for-byte. Coverage stays the plain GEOMETRIC mask (the Inpaint contract). `restore()` still
answers from `m_base`. Smaller deviations, all deliberate and documented in the code: float state
vs the reference's per-dab U16 round-trip; out-of-layer reads are transparent (the reference's
unbounded device keeps off-canvas smear within a stroke); at state alpha 0 the composite writes
base RGB verbatim (premul lost the RGB; the engine's erase convention).

**`SmudgeParams::enabled` supersedes the axes**: `begin()` normalizes its params copy to Wash /
Uniform / no masking / Normal blend (the reference has none of those axes under colorsmudge), and
requires a REAL tip + Paint mode. The mapper badges everything the legacy transcription does not
implement: `smearAlpha` off, the new engine, overlay (`MergedPaint`), **PaintThickness (DROPPED,
never mapped — §5's paint-load exclusion)**, a non-`normal` CompositeOp, a masking brush, EraserMode.
The smudge trio joined `kDrivenOptions` with the mirror-image of Opacity's caveat: honoured by the
smudge walk alone, badged on any other paintop.

⚠ Engine traps, each earned by a killed (or first-survived) mutant this session:
- **A smear dab must copy its source patch BEFORE writing** (`m_smudgeScratch`): src and dst
  overlap whenever spacing < dab size, and an in-place walk reads pixels the same dab already
  wrote. The property tests are too loose to see it — the byte-golden kills it.
- **The dulling boundary-mix, the drag-chain, the alpha-eats tests are PROPERTY tests**; the
  byte-golden (FNV-1a over the image, blessed) carries the arithmetic: grouping, offsets, stream
  discipline. Its options deliberately carry a `fuzzy` sensor so any mutant that double-evaluates
  or skips an option shifts every later draw and moves the bytes.
- **The blenders are invisible on uniform canvases** — six presets honestly change NOTHING on flat
  grey (colour rate 0, nothing to smear). Every smudge test canvas needs STRUCTURE (a patch, a
  boundary); the preview strip lays ink bars for the same reason (stroke_preview.cpp).
- `smudgeSampleRect`'s over-shrink guard was DELETED as a dead branch: truncation cannot remove
  more than half an extent per side for radius > 0 (`w − 2·floor(βw) ≥ 1`, β < 0.5). An
  unreachable guard is an unkillable mutant.

What is NOT here, and stays badged: the new-engine strategies (Mask/Lightness/Stamp), overlay
mode, Buildup-style direct colour rate under other composite ops, the paint-load channel (§5, held
back). The dulling sampler and smear copy read only `m_smudgeState` — the §6.6b guard rail held.

### 6.6d Scatter and Mirror — the positional options, transcribed *(landed 2026-07-14, 20th session)*

**The two options that move a dab rather than shape it** — the last §3.10 bases blocking a
double-digit slice of the census (15 presets flipped Exact, 48 → 63). VERIFIED format facts, read
from the reference's scatter/mirror options and both call sites (pixel brush and colorsmudge):

- **Scatter** (`KisScatterOption::apply`): `jitter = (2·rand01 − 1) × qMax(maskW, maskH) ×
  computeSizeLikeValue(info)` — the dims are the **mask raster's** (the scaled, rotated dab's
  AABB), the value reads WITH strength, and the strength's consumer range is **[0,5]**. Both axes
  on (the default): two INDEPENDENT draws, X first. One axis: ONE draw, laid along the drawing
  angle (X) or its normal `(−sin, cos)` (Y). Unchecked or axis-less: returns before the random
  source — an inert scatter perturbs nothing, stream included. Applied to the CURSOR POSITION
  after size/rotation/ratio resolve and before the dab renders; the spacing cadence keys off the
  path, never the jitter. **Colorsmudge scatters too** (same formula, same call shape), and its
  smear anchor tracks the scattered rect centres — Mosaic's anchor does exactly that already.
- **Mirror** (`KisMirrorOption::apply`): ONE size-like draw per dab; `result = value ≥ 0.5` flips
  every enabled axis (`{Horizontal,Vertical}MirrorEnabled`, both default false), XOR'd with the
  canvas mirror state (Mosaic has none — the same fact `dabAngle` records). Exactly one axis
  flipped additionally negates the dab's rotation (`2π − θ`) before the mask renders; the rendered
  dab is then pixel-flipped (`dab->mirror(h, v)`).
- **The conjugation, and why Mosaic's angle passes through UNCHANGED**: the reference composes
  `flip ∘ R(−θ)`; Mosaic's `TipFrameMap` composes `R(θ) ∘ flip` (the tip flips in its OWN frame,
  before rotation — dab_mask.hpp's documented order). A single-axis flip conjugates a rotation
  into its inverse — `M·R(−θ) = R(θ)·M` — so the two pipelines produce the SAME map with no angle
  edit at all; both axes flipped is a half-turn in both forms and the reference negates nothing
  there either. The AABB extents agree the same way (|cos|/|sin| are even), so placement, spacing
  and the dab-cache key see one geometry. Pinned in bytes: a stroke whose Mirror flips every dab
  equals the same stroke laid with a pre-flipped tip, byte for byte (test_brush_options.cpp).
- **Mirror is DEAD under colorsmudge in the reference**: the settings widget offers the option,
  the paintop never constructs it, and only the pixel brush's dab executor wires mirror
  postprocessing into a dab cache. So the mapper drops Mirror on smudge presets **without a
  badge** — the dropped stroke IS the reference's stroke. No shipped colorsmudge preset carries
  it; the rule is pinned by a unit test, not the census.
- **Legacy key**: pre-2.x files store the scatter strength as `Scattering/Amount`; it wins only
  when the modern `ScatterValue` is ABSENT (the reference's `valueFixUpReadCallback`, verbatim).
  Axis gates are plain static properties beside the curve option, carried on `BrushPreset`.
- **Randomness**: the reference's stream is `boost::taus88` seeded from a global RNG per stroke —
  upstream strokes are not even self-reproducible, so the transcription target is the FORMULA and
  the uniform-[0,1] distribution, with determinism supplied by Mosaic's own stroke-seeded
  splitmix64 (`StrokeState::nextRandom`, the `fuzzy` stream). Draw order is pinned as part of the
  replay contract: the two positional options draw LAST in `resolveDab` (after the wash pair, the
  smudge quartet and the frame selection — appending keeps every pre-§6.6d golden byte-identical),
  scatter before mirror, X before Y. The reference's own intra-dab order differs (scatter's draws
  precede opacity's) and cannot be observed through its random seeding; the deviation is
  deliberate and this sentence is its record.
- **Scatter's extents come from THIS dab's frame** (the hose cell it will stamp): the reference
  reads its pipe brush's *current* mask dims, which is the PREVIOUS dab's frame — an off-by-one
  the transcription does not reproduce. Same fact, correct dab.

Engine shape: `ScatterOption`/`MirrorOption` wrap a `CurveOption` plus their axis gates (dab.hpp);
`applyScatter`/`applyMirror` own the formulas and the inert-no-draw contract (dab.cpp, unit-tested
with exact-equality draw predictions); `resolveDab` supplies the tip extents (`dabExtent` of the
rotated `tipDabShape`, diameter × ratio envelope tipless) and the pinned order. `Dab::mirrorH/V`
and the mask pipeline (TipTransform, cache key, both stamps) predate the options unchanged — the
options only finally drive them. Mutation battery: 15 mutants (formula terms, axis frames, the
threshold's `≥`, both inert gates, both engine calls, both extents arms, the smudge-Mirror
honesty, the Amount fix-up), **all 15 killed first run** — the first battery in seven sessions
with no survivor. Two FNV goldens carry the extents claims (a rotated 16×4 nib ≈ 14.14 px
amplitude vs 16 for either wrong source; a tipless ratio-2 envelope), blessed identical on debug
and release.

### 6.6e Spacing and Sharpness — the cadence and pixel-grid options *(both landed 2026-07-18, 21st session)*

The last of the plain §3.10 curve options, read from the reference's `KisSpacingOption` and
`KisSharpnessOption` and both call sites.

- **Spacing** (`KisSpacingOption::apply`): returns `computeSizeLikeValue(info)` WITH strength when
  checked (range **[0,1]**), 1.0 when not. It is a *cadence* option, not a dab-shape one: the
  reference's `effectiveSpacing` multiplies the WHOLE spacing distance by it — `spacing *=
  extraScale` — in EVERY branch (auto or manual step, isotropic or anisotropic), so both axes scale
  together and a value below 1 lays dabs denser. Mosaic evaluates it once per dab in `resolveDab`
  (LAST among the draws — the reference computes the option after laying the dab, to size the step to
  the next one — so it can draw from the random streams without disturbing the positional options'
  pinned order) and stores `m_dabSpacingScale`; `dabSpacingEllipse` multiplies each axis's interval
  by it, BEFORE the half-pixel floor (so a scale toward zero cannot spin the dab loop). It rides both
  walks, exactly as the reference's colorsmudge spaces its dabs through the same option; the masking
  walk drives no options and passes 1.0. Absent or unchecked it is exactly 1.0, and `interval * 1.0
  == interval`, so every spacing golden holds. ⚠ `Spacing/Isotropic` is a SEPARATE static flag
  (§3.2, already honoured as `BrushParams::isotropicSpacing`), not this per-dab option — the two
  share the "Spacing" prefix in the file and nothing else. Census: 4 carriers, 3 with no other drop
  flip to Exact (i)_Wet_Bristles, i)_Wet_Smear, h)_Chalk_Grainy; 48+15+3 → **66 Exact / 38 / 13**).
- **Sharpness** (`KisSharpnessOption`) does TWO things off ONE per-dab value (`computeSizeLikeValue`
  WITH strength, [0,1]), and Mosaic draws it once (in `resolveDab`, after Scatter/Mirror — the
  reference snaps the *scattered* position) and shares it:
  - **The threshold** (`applyThreshold`) hardens the dab's mask alpha, whenever the option is
    CHECKED. Transcribed as `sharpnessThreshold` on the 8-bit mask in `stampTipDab`, in exact integer
    arithmetic: `tolerance = (uint32)(255 − threshold·255)`; `v > tolerance` → opaque; `v ≤
    (100 − softness)·tolerance/100` (integer division) → transparent; otherwise KEPT. At value 1 the
    band collapses (tolerance 0) → a hard 1-bit edge, whatever `Sharpness/softness` says. This is the
    pixel-art look. Gated on the frozen `m_sharpnessActive`, so an unchecked option leaves the loop
    byte-identical.
  - **The coordinate snap** (`apply`/`calculateDabRect`) aligns the dab to the pixel grid — but only
    when `Sharpness/alignoutline` is set AND the static strength is > 0 (the reference's own gate,
    frozen as `m_sharpnessSnap`). The reference snaps the mask's top-left `pt = center − halfExtent`
    via `s·round(pt) + (1−s)·pt`; the centre delta is `s·(round(pt) − pt)` per axis (`applySharpnessSnap`),
    and shifting the centre by it makes `placeDab` land on the snapped top-left — zero sub-pixel phase
    at s = 1. The extents are the rotated tip's, measured exactly as Scatter reads them.
  ⚠ **Krita-4 vs Krita-6 keys**: the 3 shipped `u)_Pixel_Art*` presets store the pre-6 `Sharpness/threshold`
  (4/40/1) and no `alignoutline`; Krita 6 reads `Sharpness/softness` (absent → 0) and ignores the old
  key, so they load softness 0, alignOutline false — the threshold alone (their `SharpnessValue` = 1
  gives the hard 1-bit cut). We transcribe *Krita 6's* behaviour: `Sharpness/softness` default 0. ⚠
  **NEVER under smudge**: the reference's colorsmudge installs no sharpness option at all, so dropping
  it there is the faithful stroke — badge-free, like Mirror (`preset_brush.cpp` never wires it under a
  smudge preset). Census: 3 carriers, 2 with no other drop flip to Exact (u)_Pixel_Art, u)_Pixel_Art_Fill;
  u)_Pixel_Art_Dithering keeps a Texture drop) → **68 Exact / 36 / 13**. Mutation-tested; unit tests
  pin the threshold (pure + a soft rim gone 1-bit) and the snap (pure + a fractional dab matching its
  snapped-integer twin).

### 6.6f HSV colour dynamics — h/s/v adjust the paint colour *(landed 2026-07-18, 22nd session)*

The first of the **colour** options (§6.1's `Colored` axis, which had existed since S19 step 5b but
which no option filled). Read from the reference's `KisHSVOption`, `KisBrushOpResources` (the call
site) and its `hsv_adjustment` transformation, transcribed for a single paint-colour triple.

- **The mechanism**: on the pixel brush the three options (`h`, `s`, `v`) build ONE HSV colour
  transformation and apply it to the *colour source* — for a plain-colour brush, the paint colour —
  before the dab is stamped. So a colour-dynamics dab does not vary per pixel; it varies the one
  colour the whole dab deposits. That maps exactly onto the Colored accumulator's per-dab colour:
  Mosaic resolves the adjusted colour ONCE per dab in `resolveDab` (`applyColorDynamics`, LAST among
  the per-dab draws — appended after the spacing scale so every prior golden's random stream is
  byte-identical), stores `m_dabDynColor`, and `beginDeposit` deposits it in place of the flat colour.
- **The values** (per channel, only when its `Pressure{X}` bit is set):
  - **Hue** `h` = `computeRotationLikeValue(info, 0, false, 1.0, hovering)` — a `rotationLikeValue` in
    [−1,1), fed into the transform as `h += value·180`° (a half-turn rotation at ±1).
  - **Saturation** `s` / **Value** `v` = the reference's remap of `computeSizeLikeValue(info)` (WITH
    strength): `halfV = strength·0.5; val = size·strength + (0.5 − halfV); val = 2·val − 1`. The
    strength is applied TWICE deliberately (the reference multiplies the already-strength-scaled size
    value by `strengthValue()` again), so the channel's span is `strength²` and its neutral point (0)
    sits at a size-like value of 0.5. `s` scales chroma (with a nonlinear boost above 0), `v` drives
    value toward black (dv < 0) or white (dv > 0).
- **The pixel transform** (`hsvAdjust` → the anonymous `hsvTransformRgb`): the reference's
  `HSVTransform<HSVPolicy>` — NOT the legacy `RGBToHSV`/`HSVToRGB` path, because `KisHSVOption` sets
  the transformation's compatibility flag OFF (type = HSV, colorize = off). Run in `float` exactly as
  the reference does, then quantized to 8-bit with round-to-nearest (`KoColorSpaceMaths<float,quint8>`,
  which the paint colour's own 8-bit depth pins for a standard RGBA8 document). Straight (non-
  premultiplied) RGB in and out; the alpha passes through.
- **Draw order + inertness**: hue, then saturation, then value — the reference's `hsvOptions` order —
  and each draws from the stroke's random streams ONLY when checked (the reference's `apply()` returns
  before the sensors on an unchecked option). With NO channel checked the reference builds no
  transformation at all, so `applyColorDynamics` returns the base colour untouched and draws nothing —
  the inert contract, kept byte-exact (an all-identity HSV round trip is only *near*-exact in 8-bit).
- ⚠ **NEVER under smudge**: Mosaic's smudge walk paints the stroke's own colour with no per-dab
  colour source, and its normalization forces the `Uniform` accumulator, so `colored()` is already
  false there. The mapper drops any colour-dynamics option on a smudge preset and BADGES it — unlike
  Mirror/Sharpness, this is *not* a faithful drop (the reference's colorsmudge DOES adjust the
  colour); it is an honest fidelity note. No shipped colorsmudge preset drives colour dynamics.
- **`Mix` and `Darken` are deliberately NOT transcribed**: neither is driven by any shipped preset,
  and each is separate machinery (Mix a background-colour + alpha-weighted mix through the colour
  source; Darken a Lab-16 `createDarkenAdjustment` round-trip). They stay unsupported (dropped +
  badged) until a real need appears.
- **Census**: exactly ONE shipped preset drives any colour dynamics — `v)_Texture_Impressionism`
  (`h` + `v`, both `fuzzy` at strength 1; `s`, `Mix`, `Darken` unused everywhere). Its every other
  option (Size, Rotation, Scatter, Mirror) had already landed and its `Texture/Strength/` is inert
  (`Texture/Pattern/Enabled=false`), so honouring h/v was its last drop: it flips to Exact →
  **69 Exact / 35 / 13**. Unit tests pin `hsvAdjust` (hand-computed primary rotations, black/white
  value pushes, full desaturation), the s/v remap and the hue path through deterministic Pressure
  sensors, the hue→saturation→value draw order off a twin stroke, and the end-to-end Colored stroke.

### 6.6g `StrokePainter` — the second engine kind, with the sketch and Sumi-e engines on it *(landed 2026-07-27, 23rd session)*

**Tier 4's scaffold, plus 3 of its 6 presets** (`v)_Sketching-1_Chrome_Thin`,
`v)_Sketching-2_Chrome_Large`, `d)_Ink-8_Sumi-e`). Everything the engine has painted for four arcs is
a mask stamped along a path at a spacing cadence; these three are not that, and no choice of mask
makes them that. Transcribed from the reference's sketch and hairy paintops and its painter's line
rasterizers.

#### The abstraction, and the one decision that shaped it

`StrokePainter` is **not a parallel engine**. `BrushEngine::stampSpan` either runs the dab walk or
hands the span to a painter; a painter draws through a `StrokeCanvas`, whose single method lands one
integer pixel in the very buffers `deposit()` lands in. Everything downstream is therefore untouched
and unconditionally shared: Wash/Buildup, `StrokeMode::Erase`, the blend path, the masking walk (which
still runs beside a painter), the coverage buffer the Inpaint brush reads, the bounded working rect,
the pristine base snapshot, `restore()`, `composite()`'s incremental pending region, and the undo
replay that rests on all of it. A painter that owned a buffer would have had to own all of them.

- **A painter is DATA on `BrushParams`, not an object.** `StrokePainterParams { kind, sketch, hairy }`
  rides the params (which are copied per stroke and shared const); `begin()` builds the painter, and
  `end()` drops it. So a stroke is described wholly by its params — which is what makes it replay —
  and two strokes of one preset never share a point history, a bristle field or a random cursor.
- **ONE CALL PER FLATTENED EDGE, and that IS the transcription.** The reference's own
  `paintBezierCurve` subdivides a curved segment to flatness and calls `paintLine()` on each piece
  with interpolated paint information; handing a painter the engine's own flattened polyline, edge by
  edge, with the state interpolated by ARC LENGTH exactly as `walkSpan` interpolates it, gives its
  painters precisely the input they were written against. On a straight span the flattener emits no
  interior point, so the painter gets the span's own two endpoints, once.
- **The stroke is rewound to the edge's END before the call**, because every reference painter
  evaluates its options against the segment's *second* paint information. `beginPainterSegment()` then
  advances what `resolveDab` advances once per dab — the dab counter (`fade` reads it), the dab index,
  and the dynamic-opacity pair — so a painter's span is a dab as far as the stroke's bookkeeping goes.
- **Randomness comes from `StrokeState`'s seeded streams**, never a clock. Anything that is a property
  of the BRUSH rather than of the stroke (the bristle layout) is drawn from its own fixed-seed stream,
  exactly as the reference's is.
- **The engine-kind list is ONE list**, `painterKindForPaintop` (stroke_painter.hpp), read by the
  importer's fidelity floor (`mapper.cpp` — does this preset start Exact?) and by the preset→engine
  mapping (`preset_brush.cpp` — does this stroke get a painter?). That is `kDrivenOptions`' rule one
  level up, and for the same reason: promising an engine the stroke does not get is the badge lying in
  exactly the direction §6.4 exists to prevent. **The biconditional is a test**, unit-level
  (test_brush_preset_brush.cpp — it can see a paintop the list names and the mapping forgets) and over
  the corpus (test_brush_library_census.cpp).

#### The two shared rasterizers, transcribed

- **`rasterizeDdaLine`** (the reference painter's `drawDDALine`): floor both endpoints, step one pixel
  along the major axis, round the minor one, every pixel at full weight. ⚠ Its `lockAxis` flag is
  load-bearing and not tidy-up: a purely HORIZONTAL line leaves the gradient at 0 **with the flag
  set**, and a purely VERTICAL one sets the gradient to 2 so the walk takes the y branch and the flag
  then zeroes its step. Collapse the two and a vertical line either skews or collapses to one pixel.
  ⚠ Its cursors are `float`, not double, and its seeds are the FLOORED corners — a DDA line is a
  function of the two pixel corners alone, not of the sub-pixel endpoints.
- **`rasterizeThickLine`** (the reference painter's `drawLine(width, antialias)`): not a walk at all
  but a **distance field over the line's bounding box** — perpendicular distance inside the
  projection range, distance to the nearer endpoint outside it (which is what rounds the caps),
  against `halfWidth = width·0.5 + subPixel`, where `subPixel` is the fractional part of the start
  point **along the major axis only** and it *widens* the band rather than shifting it. ⚠ **A line
  whose two endpoints floor to the same pixel draws NOTHING** — the reference returns before its loop,
  and the sketch engine asks for exactly that line on every span (its history always contains the
  point it is connecting from). ⚠ The antialiased rim's factor is TRUNCATED into a byte and applied as
  an 8-bit multiply, so the ramp is quantized to 1/255 and its top step is 255/255, not 256/256.
  Mosaic intersects the scan box with the document (an unclipped box over a long diagonal is quadratic
  in its length); the emitted pixels are identical either way.
- **`linearTrajectory`** (the reference's hairy `Trajectory::getLinearTrajectory`) is a THIRD walk and
  not either of the above: it emits the exact start, then one point per integer step of the major axis
  carrying the FRACTIONAL minor coordinate, then the exact end. ⚠ It TRUNCATES its endpoints where the
  two line rasterizers FLOOR them. A degenerate line yields exactly two coincident points, which is
  what makes a bristle that did not move lay exactly one splat.
- All three carry a `kMaxLinePixels` backstop. Nothing in a preset file bounds a stroke's coordinates
  and all three are `while (x != x2)` loops upstream; a NaN-poisoned endpoint must cost a bounded
  amount of work, not a hung stroke.

#### The SKETCH engine (`sketchbrush` ×2)

The mark is the stroke's **history**. Each span appends its END point (one point per span, never two)
to a growing list, then the painter walks the *whole* list and draws a line from the new point to
every earlier point within a threshold distance. VERIFIED facts, from source:

- **The reach** is `(radius · Size)²`, compared against a SQUARED distance — no square roots in the
  loop. ⚠ `radius` is **latched on the first painted span** (`m_count == 0`) from the brush's natural
  size and never re-measured; every later span scales that latch by its own `Size` value.
- **The draw test** is `rand01() >= 1 − Density·probability`, with `probability` replaced by
  `distance / (threshold·Density·probability)` when `distanceDensity` is on — so a far candidate is
  drawn rarely and a near one almost always. A probability of 0 makes the threshold exactly 1.0 and a
  uniform draw on [0,1) is never ≥ 1, so the web switches fully off.
- **`magnetify`** draws between the two POINTS, each pulled toward the other by
  `diff · OffsetScale · Sketch/offset · 0.01`; without it the connection is a short stroke centred on
  the NEW point — a tuft rather than a web.
- ⚠ **TWO PIECES OF PAINTER STATE SURVIVE A SPAN, and both are the reference's.** The line's OPACITY
  and its COLOUR are set inside the connection loop and never restored, so the *segment* line at the
  head of the NEXT span is drawn in whatever the last connection of the previous span left behind.
  Tidying that away changes the mark.
- ⚠ **`distanceOpacity` is a HARD CUT, not a fade**: the reference ROUNDS `1 − distance/threshold`,
  whose argument is in (0,1], so a connection is either fully opaque (within half the threshold) or
  invisible. Its own source comments the rounding as questionable; the behaviour is what ships.
- ⚠ **A dab too small to rasterize returns BEFORE the span counter advances** — so the next span still
  latches the radius — and the history point has already been appended by then.
- **`randomRGB`** scales each channel of the paint colour by its own draw (three draws, in r/g/b
  order, pulled into named locals in the reference *because argument evaluation order is unspecified*
  — the order is part of Mosaic's replay contract too). It is the third input to §6.1's accumulator
  rule: a per-mark colour needs `Colored`, and `chooseAccumulator` now takes it.
- **NOT transcribed, and badged**: the MASK connection test (the non-`simpleMode` branch, where a
  candidate must land on an opaque pixel of the tip rasterized at the new point — Mosaic falls back to
  the radius test, whose radius is the same measurement), and the antialiased **1 px** connection (the
  reference draws it with a Wu line; Mosaic draws the hard DDA line there). Both shipped presets
  author simple mode and no anti-aliasing, so neither badge fires on the corpus.
- **The press point is seeded into the history at `begin()`.** Upstream reaches it through `paintAt`,
  which appends it *and* draws its own zero-length self-connection — at most one pixel, since a thick
  line whose ends share a pixel draws nothing. Mosaic draws nothing at the press; this sentence is the
  record of the difference.

#### The HAIRY / SUMI-E engine (`hairybrush` ×1)

§5's exclusion was lifted on 2026-07-27 (maintainer's ruling; §5 and §6.6b carry the factual account
of why the exclusion's words never described the algorithm). VERIFIED facts, from source:

- **Every opaque pixel of the tip is a bristle.** Rasterize once — Mosaic at the preset's authored
  master size, the same raster a stamped dab of this preset would use, so a bristle lands exactly
  where that tip pixel would land — and store, per non-zero pixel, its offset from the raster's centre
  and its alpha. That alpha is the bristle's "length", and it is the *whole* of what a bristle knows
  about itself. ⚠ The centre is an INTEGER, truncated (`width * 0.5` assigned to an int), so on an odd
  raster the field sits half a pixel off centre — reproduced, because it is the difference between a
  bristle landing on its own tip pixel and half a pixel beside it.
- **The density subset is drawn from a FIXED stream** (the reference constructs its random source with
  seed 0): the layout is a property of the BRUSH, identical on every stroke and every replay, and it
  never consumes the stroke's own stream. ⚠ At density 1 the reference short-circuits BEFORE the draw,
  so a full-density brush pulls no random numbers at all — the same inert-option discipline the dab
  pipeline keeps.
- **One affine transform per bristle per segment**, in the reference's composition order: **shear,
  then the jitter translation, then the size scale, then the rotation** (Qt's transform builders
  pre-multiply, so the call order `rotate · scale · translate · shear` applies to a point in reverse).
  `shear = pressure · HairyBristle/shear`, jitter `= (2·rand01 − 1) · HairyBristle/random` per axis,
  scale `= Size · HairyBristle/scale`. ⚠ **`pressure` here is TWICE the sample's pressure** — range
  [0,2], deliberately doubled upstream.
- **The bristle draws a line from where it was to where it is.** With `isConnected` off (the shipped
  preset) the line runs from `previousOffset + segmentStart` to `newOffset + segmentEnd` — a line
  *parallel* to the segment, freshly jittered; with it on, from the bristle's remembered offset.
  ⚠ The memory is the OFFSET, not the document position, which is what lets a connected path follow a
  moving stroke. ⚠ The `threshold` skip (`length < 1 − pressure`) happens AFTER the two random draws
  and AFTER the memory is written: a bristle too short for the current pressure has still moved.
- **Ink depletion** is a per-bristle mark COUNTER read into a transfer curve sampled `HairyInk/
  inkAmount` times over [0,1] (`Curve::toLut` is exactly the reference's `floatTransfer`). ⚠⚠ **The
  first mark of a depleting brush lays NOTHING**, and that is the reference's arithmetic, not a bug
  here: a bristle's stored ink amount starts at **zero** and is written only *after* a mark is laid,
  while a mark's opacity is `length × inkAmount`. First mark transparent, second full, then down the
  curve. With depletion off the opacity is the bristle's length, unchanged, for every mark.
  ⚠ Three of the four ink WEIGHTS are divided by 100 at load and the fourth (`inkDepletionWeight`) is
  **not** — the reference's own asymmetry, reproduced rather than tidied.
- ⚠⚠ **THE PER-SEGMENT SCRATCH, and it is the transcription rather than an optimization.** The
  reference composites a segment's bristle marks into a temporary device and blits the RESULT over the
  layer, so within one segment the marks combine **additively, saturating at full**, and only the
  segment's total goes "over" what came before. Depositing each splat straight into the stroke's
  coverage would combine them "over" each other instead — measurably lighter wherever a bristle
  doubles back, which is most of a dry-brush mark. So the painter keeps its own 8-bit alpha scratch
  over the segment's box, exactly as the paintop keeps its temporary; the ENGINE's accumulation is
  still the one and only one. The three combination laws the reference can take are all expressible on
  it and all implemented: add-saturate (antialias, no compositing — the shipped preset), source-over
  (compositing), and max (`darkenPixel`, no antialias). A single hair walking one pixel makes them
  visibly different — 128, 112 and 64 at the shared cell — and that is the test.
- **The splat** is a 2×2 bilinear of the mark's 8-bit opacity. ⚠ TRUNCATION, then an ABSOLUTE
  fraction, both the reference's: on the negative side of the origin the pair puts the heavier weight
  on the far cell. ⚠ With antialiasing the LAST trajectory point is dropped ("avoid overlapping
  bristle caps"), so a bristle's end cap is laid by the next segment's start.
- **The rotation is Mosaic's `dabAngle`**, applied as the tip→document map the mask pipeline uses. The
  reference reaches the same geometry through its own opposite-signed angle convention and an inverse
  rotation inside the transform; transcribing *that* rather than the resulting geometry would turn the
  bristle field the wrong way against every other mark Mosaic makes. Same fact, Mosaic's frame — the
  same argument §6.6d makes for Mirror.
- **NOT transcribed, and badged**: saturation depletion (the reference runs the bristle colour through
  an **HSL** — not HSV — transformation, which the engine has no branch for) and soaked ink (per-
  bristle colour sampled from the layer under the press). Both would give a bristle a colour of its
  OWN; without them every bristle deposits the stroke's colour, which is also what keeps this engine
  on the `Uniform` accumulator. Neither is authored by the shipped preset.
- Backstops: `kMaxBristles` (a 1000 px tip's million opaque pixels is not a brush, it is a hang) and
  `kMaxHairySpanCells` on the scratch. The scratch box is a CONSERVATIVE geometric bound on the
  transformed reach — measuring it would need a second pass over the bristles, and the random draws
  can only happen once.
- ⚠ **The bristle loop runs whether or not any of it lands on the document.** Every bristle draws its
  two randoms, advances its previous position and ages its counter — properties of the STROKE, exactly
  like the dab walk's per-dab draws, which advance for clipped dabs too. Skipping an off-canvas
  segment would give a stroke near an edge a different mark from the same stroke in the middle.

#### The CURVE engine (`curvebrush` ×1)

The mark is a SLIDING WINDOW of the history. Each span appends its end point to a deque capped at
`Curve/strokeHistorySize`; once the window is full, ONE curve is stroked through it — a quadratic
through the window's MIDPOINT when `smoothing` is on, a cubic through its thirds when it is not — at
its own opacity, and optionally the segment itself at full opacity. So the ribbon trailing the cursor
is not paint laid along the path: it is the same curve REDRAWN every span from a window that has slid
one point along, and the overlap of those redraws is the mark. VERIFIED facts:

- ⚠ **Nothing is drawn until the window is FULL.** A short stroke lays only its connection lines —
  and with `makeConnection` off, nothing at all.
- ⚠ The cubic's controls are `points[step]` and `points[2·step]` with `step = maxPoints / 3`
  (INTEGER), while its endpoint is the window's LAST element rather than index `3·step` — so an
  unevenly divisible window leans on its own tail. Reproduced.
- ⚠ **The reference reader passes NO defaults**, so an absent `strokeHistorySize` reads 0 and its own
  path build then reads the front of an empty list. Mosaic reads the reader's STRUCT defaults and
  floors the window at 1: a zero window is not a shape.
- **The stroked path is ONE MASK.** The reference hands Qt one path and one pen; chaining the
  transcribed thick line per flattened edge and depositing each straight would double-darken every
  join, so the painter combines the edges in its scratch by MAX and deposits once. ⚠ A 1 px pen still
  goes through the thick line, not the DDA walk — the sketch engine's `lineWidth == 1` special case
  is that paintop's own, not a property of drawing lines.
- **NOT transcribed**: Qt's pen stroker itself. The joins are the difference; the flattening
  tolerance is Mosaic's (`flattenQuadratic` / `flattenCubic`).

#### The PARTICLE engine (`particlebrush` ×1)

The mark is a persistent SIMULATION. VERIFIED facts:

- **Planted once, at the press**, all on top of one another; the spread comes entirely from their
  DIFFERING accelerations, a plain ramp `(i + iterations)·0.5` over the particle index.
- ⚠ **This engine draws NOTHING RANDOM.** No jitter, no scatter, no stream — two strokes over the
  same path are identical whatever the seed, and that is a test.
- Per segment, stepped `iterations` times; per step each particle is pulled toward the cursor by
  `(cursor − pos)` scaled per axis and by its own acceleration, the accumulator is damped by
  `gravity`, and the particle moves by a constant sliver (`0.000030`) of it. The constant is not a
  physical time: it is what the equation was tuned against, and the whole feel is in it.
- ⚠ **The equation is deliberately unstable** at a negative scale or gravity, and the reference says
  so in its own comment. It guards the divergence by dropping diverged particles rather than by
  stabilizing the equation, and only when a coefficient is negative. Reproduced, guard and all.
- Its Wu particle FLOORS its position and takes a SIGNED fraction — where the bristle engine's
  otherwise identical-looking splat TRUNCATES and takes an absolute one. Two engines, two
  conventions, both reproduced.

#### The EXPERIMENT / SHAPE-FILL engine (`experimentbrush` ×1)

The mark is the WHOLE STROKE as one closed polygon, filled once — the only engine of either kind
whose output is not a function of any prefix of the stroke.

- ⚠ **THIS IS WHAT `finish()` IS FOR**, and it is the one place §6.6b's warning bit: the reference
  repaints its growing shape progressively (diffing painter paths, or fanning triangles from the
  stroke's first point) at up to 25 fps, and Mosaic fills once, at `end()`. **The shape is identical;
  the live preview of it is the deviation**, and it is the deviation §6.6b predicted.
- ⚠ **`windingFill` is not decoration.** The stroke is a self-crossing scribble and the two fill
  rules disagree at every one of its crossings: non-zero winding fills the enclosed blob, the
  alternate rule punches the overlaps back out. The shipped preset uses winding.
- The edge is antialiased UNLESS `hardEdge` — the reference's own inversion.
- **NOT transcribed, and badged**: the outward DISPLACEMENT of the accumulated path (a per-point push
  with a path-simplification pass behind it), the SPEED correction (a filtered stand-in position that
  outruns the pointer), and the path SMOOTHING (quadratics through a running midpoint). The shipped
  preset authors none of the three. Qt's polygon rasterizer is not reproduced either: Mosaic
  supersamples the fill rule 4×4, which agrees on the interior and differs on the edge ramp.

#### The HATCHING engine (`hatchingbrush` ×1) — a DAB engine, not a painter

⚠ **The first thing this build did was establish which kind it is**, and the answer is not the one
its position in §6.6(b)'s neighbourhood suggests: it derives from the reference's BRUSH-BASED paintop
base and overrides `paintAt`, so it rides the dab walk, the spacing cadence and the dab cache
untouched. §6.6(c) predicted exactly what it needs — "a dab may source procedural pattern clipped by
the tip mask rather than only a mask" — and that is the whole of the new machinery: `stampTipDab`
multiplies the tip's 8-bit coverage by a per-dab lattice. Cost S on the dab pipeline, not M on the
painter scaffold. VERIFIED facts:

- ⚠ **THE LATTICE IS PHASE-LOCKED TO THE DOCUMENT, not to the dab**, via `Hatching/origin_x`/`_y`:
  the line through the origin and the line through the dab's own top-left corner give two intercepts,
  and their remainder modulo the inter-line spacing is where this dab's nearest lattice line falls
  inside it. That is what makes two overlapping dabs CONTINUE one another's lines instead of each
  starting its own, and it is the single property the whole engine rests on. It is a test, both ways:
  a dab shifted by one period gives an identical stencil, and one shifted by half a period does not.
- **The pass order is load-bearing**: the crosshatch block first, then the Angle option's pass, and
  the "base" pass ONLY when the style is not moiré and the Angle option is unchecked. So the shipped
  moiré preset — Crosshatching CHECKED with a `fuzzy` sensor — lays exactly ONE pass per dab, at
  `spinAngle(value·360)`, and no base pass at all. That is what makes the pattern beat against
  itself as the sensor moves rather than being two fixed passes crossing.
- ⚠ **The style is a PRIORITY CHAIN, not an enum, and its first key defaults TRUE.** A file that
  mentions none of the five `bool_*` keys hatches once; a file that sets `bool_moirepattern` without
  clearing `bool_nocrosshatching` is NOT a moiré preset. Pinned by a test, because a reader that
  treated the five as an enum would look correct and be wrong on exactly one shipped preset.
- ⚠ **`spinAngle` already adds the preset's own angle**, so the Angle option's call site — which
  passes `value·360 + angle` — adds it TWICE. That is the reference's arithmetic at that exact call
  site, reproduced rather than corrected.
- `separationAsFunctionOfParameter` steps the separation by whole powers of two around the middle
  bucket of `separationintervals` equal buckets; outside 2..7 intervals it passes the separation
  through unchanged (with a debug complaint), and that pass-through is reproduced rather than clamped.
- The thickness is ROUNDED to a whole pixel and floored at 1. Only two of the four box edges may
  include their limits when testing where a line crosses the dab — the reference is explicit about
  why (a corner-to-corner line would otherwise intersect all four and still count as inner) — and a
  single intersection is a CORNER, at which it deliberately draws nothing.
- **NOT transcribed, and badged**: the antialiased hatch line (a dedicated varying-width Wu
  rasterizer; Mosaic draws the transcribed distance-field thick line with antialiasing on, and the
  difference is confined to the edge ramp — the NON-antialiased branch is exact), and the opaque
  background (it floods the dab with the background colour, which the dab pipeline does not carry).

#### Census — re-derived, and the trap in re-deriving it

**72 Exact / 39 Approximated / 6 Substituted** (was 69/35/13). Re-derived INDEPENDENTLY from the raw
preset XML with a scratch probe that replays the mapper's rules over the bundle's PNG preset chunks:

- ⚠⚠ **THE `<param>` ATTRIBUTE ORDER IS NOT FIXED IN THE CORPUS.** Some files write `type=` before
  `name=` and some after. A probe that pins one order loses 28 presets' `brush_definition` silently
  and lands on 54/50/13 without complaining about anything. The probe is trusted only because, with
  every new engine switched OFF, it reproduces the pinned 69/35/13 exactly.
- Four presets go straight to **Exact**, with no remaining drop at all: `d)_Ink-8_Sumi-e`
  (`hairybrush` — Size and Rotation ride the painter, Opacity is static under Wash, ink depletion is
  off, no saturation depletion and no soaked ink), `t)_Shapes_Fill` (`experimentbrush` — winding fill,
  soft edge, and none of the three untranscribed features), `v)_Experimental_Webs` (`particlebrush` —
  every one of its six knobs is honoured) and, from the earlier half of the day, nothing else.
- Three keep **exactly one** drop: `v)_Sketching-1_Chrome_Thin`, `v)_Sketching-2_Chrome_Large` and
  `v)_Sketching-3_Leaky` all carry a dynamic Opacity under **Buildup** (`PaintOpAction=1`), the same
  caveat the two Buildup pixel-brush presets carry, because the direct path's own per-mark opacity
  composite is still untranscribed.
- `y)_Screentone_Moire` keeps **two**: that same Buildup-Opacity caveat, and its antialiased hatch
  line.
- The 33 Approximated presets that have an engine family histogram as Texture 21, Airbrush 7,
  Opacity-under-Buildup 6, SmudgeRate-on-paintbrush 5, Hatching anti-aliasing 1; plus 6
  no-engine-family (spraybrush 4, filter 2). **The 6 Substituted are the ones with a home elsewhere
  on the plan**: `deformbrush` ×3 (Liquify), `duplicate` (Clone Stamp), `gridbrush` (Tier 5),
  `tangentnormal` (Tier 3).

#### What this chunk deliberately did NOT do

- **`gridbrush` and `tangentnormal`** — Tier 5 and Tier 3, both scope calls with costed plans, and
  neither is a stroke-history engine. `gridbrush` can now reuse the hatching stencil's seam.
- **The per-pixel paint-load channel** under smudge and **the rope/pulled-string stabilizer**. The §5
  ruling unblocks both, and neither is part of this arc: the first changes the accumulation buffers
  themselves, the second changes the input path. Each is its own slice, with its own transcription
  and its own row in the provenance register.
- **Qt's rasterizers**, in three places, each recorded above: the pen stroker (curve joins), the
  polygon rasterizer (fill edge), and the varying-width Wu line (hatch antialiasing — the only one of
  the three that costs a badge, because it is the one a shipped preset asks for).
- **The transcription record is what this section owes, and pays.** The provenance for
  every engine above is in `docs/brush-opacity-prior-art.md`, which the §5 ruling makes load-bearing.

### 6.6h Texture and Airbrush — the grain on the paper and the clock on the walk *(both landed 2026-07-28, 24th session)*

**The two biggest remaining drop reasons, and by a wide margin**: 21 shipped presets were Approximated
for `Texture` and 10 for `Airbrush`. Both are now transcribed end-to-end, and the census moves
**72 / 39 / 6 → 94 Exact / 17 Approximated / 6 Substituted**.

They are in one section because they land in one commit, not because they are alike. They are not:
one is a per-pixel composite over a dab's mask, the other is a second cadence on the stroke walk.

#### The TEXTURE option — a document-locked pattern, composited into every dab's alpha

Read from the reference's texture option, its mask builder, its embedded-pattern data block and the
**alpha-source** half of its masking-brush composite ops. VERIFIED format facts, from source:

- **The key set is `Texture/Pattern/*` plus the curve option `Texture/Strength/`**, and the two halves
  are read by different things. The pattern block is *static* — scale, brightness, contrast, neutral
  point, invert, the cutoff triple, the two offsets and their two random flags, the texturing mode,
  the soft-texturing flag, the enable gate and the embedded pattern payload — and every one of them
  is baked into an 8-bit mask ONCE. `Texture/Strength/` is an ordinary checkable curve option; ⚠ its
  BASE NAME contains slashes and ends in one, so its enable key is `PressureTexture/Strength/`, the
  same shape as the three sketch/curve bases whose names contain a space.
- ⚠ **Two keys are read by nobody, and both look like settings.** `Texture/Pattern/MaximumOffsetX`/`Y`
  are skipped by the reference's own reader with a note that its widget recomputes them from the
  loaded pattern — a file's stored pair is stale data. And `Texture/Pattern/Strength` is a **dead
  Krita-2 key**: 19 of the 21 shipped texture presets still carry it, and reading it would apply the
  strength twice.
- **The bake** (`KisTextureMaskInfo::recalculateMask`), in its own order and with its own clamps:
  the pattern is resampled by `scale`, each pixel's `qGray` (`(r·11 + g·16 + b·5)/32`, INTEGER — the
  weights are not equal) is composited over WHITE by the pixel's alpha, `brightness` is SUBTRACTED,
  `contrast` pivots on 0.5, the value is clamped, inverted if asked, and then re-centred by the
  **neutral point through TWO straight segments** — `[0,n] → [0,½]` and `[n,1] → [½,1]` — so neither
  half clips. The cutoff policy then bands it: 1 sends everything outside `[left,right]` to
  transparent, 2 to opaque. ⚠ At `scale` exactly 1 (and exactly 0) the reference resamples **nothing**;
  four shipped presets author scale 1 and their mask is the image's own luminance, byte for byte.
- **The composite** is the *alpha-source* form of the reference's masking-brush ops, in their
  with-strength parameterizations, and it is 8-bit integer arithmetic that TRUNCATES:
  `Multiply` hard is `mul(src, dst, strength)`, `Multiply` soft is `mul(union(src, inv(strength)), dst)`,
  `Subtract` hard is `max(0, dst − (src + inv(strength)))`, `Subtract` soft is `max(0, dst − mul(src, strength))`.
  ⚠ The soft flag INVERTS what the strength scales — the pattern rather than the dab — so a
  zero-strength hard multiply erases the dab and a zero-strength soft multiply leaves it alone.
- ⚠ **THE PATTERN IS TILED ACROSS THE DOCUMENT, NOT ACROSS THE DAB**: the value at a pixel is
  `mask[(y − offY) mod H][(x − offX) mod W]` in document coordinates (a MATHEMATICAL modulo — a
  stroke crossing x = 0 must see the grain continue, not fold). That is what makes the grain sit
  still while the brush moves over it, and it is the same document-locked phase §6.6g's hatching
  lattice needs, reached the same way. It is a test both ways: a stroke shifted by a whole pattern
  period lays the same mark translated, and one shifted by half a period does not.
- **The offsets are per STROKE.** `isRandomOffsetX`/`Y` draw from the reference's *per-stroke* random
  source under the keys `texture_offset_x`/`texture_offset_y`, whose values are fixed for the whole
  stroke however many dabs read them. Mosaic draws them from `StrokeState::strokeRandom` — the same
  object, keyed the same way — at `begin()`, so they cost no per-dab draw and cannot disturb any
  option's pinned order.
- **Where it applies**: `postProcessDab`, after the **Sharpness threshold** and nothing else. The
  order matters: a texture applied first would be thresholded back to 1-bit and vanish. Mosaic's
  `stampTipDab` composites it in exactly that position.
- ⚠ **IT RIDES THE SMUDGE WALK TOO**, and that is the opposite of Mirror and Sharpness. The reference
  installs its texture option on the brush-**based** paintop base that `colorsmudge` derives from, and
  the smudge dab's alpha-8 mask goes through the very same post-processing step — so the 5 shipped
  textured `colorsmudge` presets really do texture. (Sharpness does not: nothing installs a sharpness
  option under colorsmudge at all, which is why only one of the two appears in `stampSmudgeDab`.)
- **Two of sixteen modes are transcribed**: `Multiply` (the format's default) and `Subtract` — the only
  two the whole shipped set uses. The other fourteen import as multiply with a fidelity note, exactly
  as the masking brush's seven unimplemented ops do; four of them (`Lightness`, `Gradient` and the two
  height families) modify the dab's **colour** rather than its alpha and are a different mechanism, not
  a different constant.
- **The one deviation, and it costs no badge**: the pattern's `scale` resample. The reference uses its
  toolkit's smooth scaler; Mosaic uses a separable tent of radius `max(1, 1/scale)` source pixels —
  exactly bilinear when magnifying, a triangle-filtered downscale when minifying. That is the same
  deviation the whole tip pipeline already carries (a bitmap tip is minified through Mosaic's own mip
  chain and the 47 bitmap-tip presets are Exact on that basis), so it is recorded here and not badged.
- **Where the pattern comes from**: every one of the 21 shipped presets EMBEDS its pattern in
  `Texture/Pattern/Pattern`. ⚠ **The payload is base64-encoded TWICE** — the XML layer leaves a
  `bytearray` param encoded, and what that decodes to is the producer's own base64 string of the
  pattern file — so a reader that stops after one decode gets ASCII beginning `iVBORw0KGgo`, which is
  the base64 *of* a PNG signature and fails the image decoder for entirely the wrong reason. Decoding
  and baking happen in the **library**, beside tip resolution and for the same reason (the mapper is a
  pure function of the parsed document and decodes no images), with the same honesty counters:
  `texturesResolved` / `texturesFallback`.

#### The AIRBRUSH — a second cadence, and why it is still deterministic

⚠ **This was the risky half, and the risk turned out to be in the framing.** A wall-clock-driven dab
cadence is not reproducible, and the engine's goldens, its stroke preview and the "a mouse stroke is
byte-identical" invariant all rest on the mark being a function of the input samples. Read against the
source, the reference splits the airbrush in exactly two, and only one half is in the engine:

1. **The cadence, which is deterministic.** The reference's walk asks two questions between two
   samples — *has the brush travelled a spacing?* and *has a timed interval elapsed?* — and lays a dab
   at whichever comes first (`min(distanceFactor, timeFactor)`). The second question is answered from
   **the samples' own timestamps**, which the walk is handed as `startTime`/`endTime`; no clock is read
   inside it. Mosaic's `StrokeInput` has carried `timeUs` since Arc C and `StrokeSnapshot::elapsedMs`
   interpolates it, so the timed cadence reads exactly the quantity the `time`, `speed` and
   `fade`-over-time sensors have always read. **The airbrush therefore adds no new class of
   nondeterminism**: same samples in, same bytes out — and that is a test, with its converse (move only
   the timestamps and the bytes must move) beside it, because the first half alone is vacuous.
2. **The stationary pump, which is a WALL CLOCK — and which is not the engine's.** While the pointer is
   held, the reference's *tool* runs a timer that synthesizes a fresh paint-information sample at the
   previous position with the current stroke time and speed 0, and feeds it through the ordinary paint
   path. It is INPUT GENERATION, not mark-making. In Mosaic that belongs to the canvas
   (`ui::VulkanCanvas`), exactly as it belongs to the tool upstream; the engine needs no change for it,
   and a recorded stroke replays byte-identically because the synthesized samples are samples.

So there was no design decision to hand back: the engine half is built, and the owed slice is a UI one
(**synthesize a sample at the previous position every `0.5 × 1000/rate` ms while the pointer is down and
the preset airbrushes** — the reference's own factor, which pumps at twice the rate for responsiveness
and lets the engine's own cadence do the placing).

The engine facts:

- **The interval** is `1000 / rate` ms, DIVIDED by the per-dab `Rate` option value and floored at the
  reference's own 0.5 ms. ⚠ A rate value of **zero is "never"**, not "instantly" — the reference
  substitutes a length of time no stroke will last. Inverting that sense would make a zero-Rate
  airbrush the fastest one there is, and would spin the walk.
- ⚠⚠ **`rate` IS DABS PER SECOND AND THE AUTHORABLE MAXIMUM IS 1000**, i.e. a dab every millisecond.
  The reference's own slider spans `[1, 1000]` with a default of 20. That is not a corner case to be
  defended against — it is a real authored value, and it is what makes the *next* fact matter as much
  as it does.
- ⚠⚠ **ONLY `PaintOpSettings/*` IS READ, AND THE KRITA-2-ERA `AirbrushOption/*` SPELLING IS DEAD**
  *(corrected 2026-07-28 — this section originally said "both ship", and honouring the second was the
  freeze; see §6.6i)*. The reference's airbrush reader reads exactly three keys —
  `PaintOpSettings/isAirbrushing` (default false), `PaintOpSettings/rate` (default 20) and
  `PaintOpSettings/ignoreSpacing` (default false) — and **nothing anywhere in it reads the
  `AirbrushOption/` prefix at all**. So a file that spells only the old keys does not airbrush there,
  and six of the shipped set's ten "carriers" are exactly that: `b)_Airbrush_Soft`,
  `e)_Marker_Details` and four `l)_Adjust_*`. **Four presets airbrush**: `y)_Texture_Spray` and the
  three `spraybrush`es.
- **`Rate` is a plain checkable [0,1] curve option** read through `KisStandardOption::apply` (checked →
  size-like value WITH strength, unchecked → exactly 1.0). It is resolved once per dab in `resolveDab`,
  **last of all** — appended after the hatching quartet, so every prior golden's random stream is
  byte-identical — and for the same reason the Spacing scale is resolved late: the reference re-computes
  its timing AFTER laying a dab, to size the interval to the next one.
- **`ignoreSpacing` switches the DISTANCE cadence off entirely** (`distanceSpacingEnabled =
  !ignoreSpacing`), and only while the airbrush is on. With it set, the timed cadence places every dab.
- **Both remainders reset at a dab**, whichever cadence placed it — the reference's own
  `resetAccumulators()`. Mosaic carries the timed remainder in `m_timeCarry` beside the distance
  remainder `m_carry`, across spans, exactly as the distance one carries.
- ⚠ **The carry line has an airbrush branch and it is not tidiness.** The inert expression is preserved
  to the character (`total − (pos − spacingPx)`, not the arc the last dab sat at) because `(a + b) − b`
  is not `a` in IEEE doubles and **every spacing golden in the suite was laid by that line**. With the
  airbrush live the timed cadence can place a dab at an arc that expression does not name at all, so
  that case reads the arc the walk really stamped.
- ⚠ **A held pointer needed one more thing**: `extendTo` ABSORBS a sample that did not move (feeding a
  duplicate to the spline would put a zero-length knot span in the middle of a curve), and the walk
  returns early on a span with no travel. Under the airbrush both are wrong — a sample that did not
  move is exactly the sample the timed cadence exists to paint through — so the duplicate is **pushed**
  and the zero-travel span is **pumped** at its own point (`pumpStationarySpan`). Both changes are
  gated on the frozen airbrush flag, so with it off the walk is byte-for-byte the walk it always was.
  This is the reference's own structure: its tool feeds synthesized still-pointer samples through the
  ordinary path, and its walk then runs the timed test on a degenerate segment.
- **It rides the smudge walk** (the reference's colorsmudge computes its effective timing the same way)
  and **never a painter** — a painter's mark is a span, not a dab on a cadence, and the reference's
  painters override the very line walk the cadence lives in.
- ⚠ **An inert airbrush draws NOTHING**, including from the random streams. The reference evaluates its
  rate option even with the airbrush off (harmlessly, since the value is then unused); Mosaic gates the
  draw, because a draw that moved every existing preset's `fuzzy` stream by one would move every golden.

#### ⚠⚠ THE AIRBRUSH FROZE THE PROGRAM, AND THE CAUSE WAS NOT WHERE IT LOOKED *(user-reported, fixed 2026-07-28)*

**Symptom**: "airbrushing freezes the program permanently, or at least for a very long time."

⚠⚠ **AND IT STILL FROZE AFTER THIS — the budget below is right and was not the whole story. Read
§6.6i.** Everything here stands: the walk is bounded, the arithmetic was simulated, and a stall
cannot dump its backlog. What none of it could fix is that the preset the user reaches for **is not
an airbrush in the reference at all** and should never have been running a timed cadence.

**The suspected cause was the arc conversion, and it is not.** The timed cadence is carried into the
walk's arc-length frame as `posT = arcBase + interval / msPerArc`, and the obvious reading is that a
span with almost no travel drives `msPerArc → ∞`, so the arc step collapses and the loop spins.
Simulated against the exact arithmetic, that does not happen: the step is
`interval · total / elapsed`, so it scales *with* the travel and the count comes out at
`elapsed / interval` **whatever the travel is** — 17 dabs per span at rate 1000 on a 60 Hz stream,
identically for a 12 px drag, a 0.01 px crawl and a span one nanometre long. Near-zero travel is not
the case that explodes.

**The actual cause is that `elapsed / interval` has two unbounded inputs and the engine controlled
neither.**

- The **interval** is one millisecond on the shipped set: `b)_Airbrush_Soft`, the four
  `l)_Adjust_*` and `y)_Texture_Spray` all author rate **1000**.
- The **elapsed** is a delta of the caller's monotonic clock, and nothing bounded it. A two-second
  stall between two samples — a window drag, a debugger break, a swap-in, a driver stutter — asks
  for **2000 dabs inside one `extendTo`**; a twenty-second one asks for 20,000; a sample stream
  whose first timestamp is 0 while the rest carry a real clock asks for ~10¹². Each is a full option
  evaluation, tip rasterization and blit of a large soft brush.

⚠ **And the 100,000-dab backstop was not a safety net — it *was* the freeze.** It was the only thing
ending the loop, it fired once per span (i.e. per input event), and 100,000 dab blits per event is
indistinguishable from a hang.

**The fix: bound the cadence by the span's own TIME BUDGET, consumed as dabs are laid.**

    budget = min(carried remainder, kMaxSpanBudgetMs) + min(elapsed, kMaxSpanBudgetMs), capped again
    a dab charges the budget for the time it consumed; a TIMED dab costs exactly one interval
    the timed candidate arms only while `budget >= interval`

The stop is derived from the same quantity that authorises a dab, so the two cannot disagree — a
counter bolted on beside the arc walk would be a second opinion about the same question. The budget
is **non-increasing** (the charge is clamped at zero, because an overdue distance carry can place a
dab behind the span's start and a negative charge would hand the loop back what it just spent), and
a timed dab always removes at least `kMinTimedIntervalMs`, so the loop's own accounting terminates
it. `kMaxTimedDabsPerSpan` is now **derived** — `kMaxSpanBudgetMs / kMinTimedIntervalMs + 2` = 502 —
so reaching it means the budget arithmetic is wrong, never that a stroke was long.

- **`kMaxSpanBudgetMs` = 250.** Past it the paint a stall "earned" is deliberately **dropped rather
  than dumped**. That is a saturation, and it is a clamp on an *input*, not a clock read — so the
  mark stays a pure function of the sample stream and replay is still exact. A 2-second stall and a
  20-second stall now lay the identical mark, and that is a test.
- **The carried remainder is clamped to the same budget**, so a run of stalls cannot bank credit for
  a later span to pay out. The per-span count under repeated stalls must be **flat, not climbing** —
  also a test, and the one thing a single-span assertion cannot see.
- **`Rate` cannot reopen it.** Its value is a size-like value clamped to [0,1], so it can only make
  the interval *longer*; the 0.5 ms floor is reachable only above rate 2000, and the budget bounds
  that case too.
- Nothing below the budget changed: every existing cadence, replay and byte-identity case passes
  unaltered, because no real sample interval comes near 250 ms.

⚠ **What the existing airbrush cases were blind to, and why.** They asserted *relative* dab counts —
more dabs with the airbrush on, more when the stroke is slower, fewer at half Rate — and every one
of those holds just as well at 2000 dabs per span as at 17. None of them fed the engine a span longer
than 200 ms, so none ever crossed the region where the count stops being reasonable. The regression
case is therefore an **absolute** bound swept over travel *and* elapsed time, including the stall
that caused the report.

⚠ **A wall-clock assertion would have been the wrong test** and is deliberately not used: it passes
on a fast machine, flakes on a loaded one, and says nothing about why. Every new case asserts a
**count**.

⚠⚠ **THE OWED UI SLICE CAN FLOOD INDEPENDENTLY, AND NEEDS ITS OWN BOUND.** The canvas half —
synthesizing a sample at the previous position every `0.5 × 1000/rate` ms while the pointer is held —
is a *wall clock*, and the engine's budget cannot protect it: the budget bounds one span, and the
pump's output is one **span each**. A pump written as "catch up on every missed tick"
(`while (now - last >= interval) emit(...)`) turns a two-second stall into 2000 synthesized samples,
2000 spans and 2000 `extendTo` calls — the same freeze, one level up, with the engine behaving
perfectly throughout. **The bound it needs: emit AT MOST ONE synthesized sample per timer callback,
never a catch-up loop** (FLTK's `Fl::repeat_timeout` drops missed ticks, which is the behaviour
wanted — the reference's own timer does the same). A stall then produces one sample carrying a large
timestamp delta, which is exactly the case the engine's budget already saturates.

#### Census — re-derived, with the same probe and the same trap

**94 Exact / 17 Approximated / 6 Substituted** (was 72/39/6). Re-derived INDEPENDENTLY from the raw
preset XML by the §6.6g probe, which ⚠ **still reproduces the pinned 72/39/6 exactly with both new
options switched off** — that, and the attribute-order-blind parse (§6.6g: pinning `name=` before
`type=` silently loses 28 presets), is what makes the new numbers worth anything.

- **Texture: 21 carriers, 20 flip to Exact** — 15 `paintbrush` and 5 `colorsmudge`. The 21st,
  `c)_Pencil-5_Tilted`, keeps the Opacity-under-Buildup caveat it already had.
- **Airbrush: 10 carriers, 2 flip** — `e)_Marker_Details` and `y)_Texture_Spray`. `b)_Airbrush_Soft`
  and the four `l)_Adjust_*` keep a `SmudgeRate` drop (an active smudge option on a paintbrush), and
  the 3 `spraybrush` ones keep their paintop's own drop.
  *(⚠ 2026-07-28, §6.6i: only 4 of those 10 carriers airbrush in the reference at all — the other 6
  spell a dead key. The spread above is unaffected, because a key the reference does not read costs
  no fidelity either way; `e)_Marker_Details` is Exact whether the airbrush is transcribed or not.
  What DID move is those five stale-key presets' `SmudgeRate` badge, and it moved for the same
  reason.)*
- The 11 Approximated presets that DO have an engine family now histogram as **Opacity-under-Buildup 6,
  SmudgeRate-on-paintbrush 5, Hatching anti-aliasing 1** — Texture 0, Airbrush 0 — plus the 6
  no-engine-family (`spraybrush` 4, `filter` 2). The 6 Substituted are unchanged.
- ⚠ Neither `Texture/Strength/` nor `Rate` costs a badge when its consumer is off, and that is a
  FAITHFUL badge-free drop rather than a kindness: the reference reads its texture strength only from
  inside the texture composite (which returns before it when texturing is disabled) and its rate only
  from inside the timing step (whose enable flag is false without an airbrush). An option nothing
  consumes is an option the reference does not apply either — the same shape as Mirror on a smudge
  preset, not the smudge trio's badged drop.

#### What this chunk deliberately did NOT do

- **The other fourteen texturing modes**, above — and in particular the two that need a per-pixel
  COLOUR path (`Lightness` scales a lightness-map dab's own lightness; `Gradient` looks the mask value
  up in the canvas's active gradient). Both want machinery the dab pipeline does not have, and no
  shipped preset asks for either.
- **The UI's stationary pump**, above: the one piece of the airbrush that is a wall clock, and the one
  piece that is not the engine's. It is an S-sized canvas slice and it changes nothing here.
- **`LightnessStrength`**, the last option base still outside `kDrivenOptions`. It scales how hard a
  LIGHTNESS-MAP tip's own lightness drives the deposit, and no shipped preset carries a lightness-map
  tip at all.

### 6.6i Dead keys, the per-stroke seed, and BUILDUP's half of Opacity *(landed 2026-07-28, 25th session)*

Two user-reported bugs and one slice, and the first two turned out to share a root cause with a third
of the census: **Mosaic was honouring keys the reference does not read.**

#### ⚠⚠ THE AIRBRUSH FROZE THE PROGRAM BECAUSE THE PRESET WAS NEVER AN AIRBRUSH *(user-reported)*

**Symptom**: *"airbrush brush strokes freeze the program and yield thick strokes — like if the
airbrush were dabbed 100k times."*

§6.6h's own hang fix (the per-span time budget) was correct and is untouched: the walk is bounded,
the arithmetic was simulated, and no span can lay more dabs than its elapsed time paid for. It was
also not the problem, because **the cadence should never have been running.**

- `b)_Airbrush_Soft` — the preset anyone reaching for an airbrush picks, first in the Basics tab —
  spells `AirbrushOption/isAirbrushing` and `AirbrushOption/rate`, the **Krita-2-era** prefix. The
  reference has no reader for that prefix (§6.6h), so it is a plain distance-cadence brush there.
- Mosaic's importer read it, and read `rate = 1000` with it: **a dab every millisecond, under a
  600 px soft nib.** That is ~360,000 pixel deposits per dab and ~360 **million** per second of
  stroke, synchronously, on the UI thread — and in wash mode a thousand overlapping soft dabs a
  second drive the alpha-darken accumulation straight to its ceiling, which is the "absurdly thick"
  half of the report. Both halves, one cause.
- Five more presets were in the same state (`e)_Marker_Details` and four `l)_Adjust_*`), four of them
  also at rate 1000.

**The fix is at the importer: read the three keys the reference reads and no others.** Nothing in the
engine changed. ⚠ The engine could not have saved this and it is worth being explicit about why: a
budget bounds a cadence, and no bound makes a cadence the preset never asked for correct.

⚠ **The old test PINNED THE BUG AS THE CONTRACT** — it asserted 10 airbrushing presets and explained
in a comment that *"reading only the modern keys would leave them all disabled"*, which is precisely
what the reference does. It now asserts the four, names them, and asserts that `b)_Airbrush_Soft` is
**not** an airbrush and keeps the reader's default rate.

#### ⚠⚠ EVERY STROKE OF A PRESET DREW THE SAME RANDOM NUMBERS *(user-reported)*

**Symptom**, on `t)_Shapes_Mecha`: *"single dabs don't take the next shape, it's one shape only on
dabs"* — a preset badged **Exact**.

The frame-selection transcription was right, and reading it against the source is what located the
real fault. `shapes_mech_random.gih`'s parasite is `dim:1 ncells:23 rank0:23 sel0:random`; the
reference's pipe brush draws a **fresh uniform index per dab** from the paint information's random
source (its `selectPost` for `Random`, called from `updateBrushIndexes` once per dab), and Mosaic's
`HoseState::selectFrame` does exactly that from `StrokeState::nextRandom()`.

**The fault was one level up: `BrushParams::seed` was 0 for every stroke the application ever laid.**

- A preset is resolved **once**, when it is selected (`ui::BrushPresetStore` — and that rule is right,
  because re-resolving mints a fresh tip raster id and a permanently cold dab cache). The `seed`
  travels with those shared params, so every stroke re-seeded the stream to the same number.
- Within a stroke the stream advances, so a *drag* looked correct and hid it. **A tap is exactly one
  first draw**, and the first draw of a fixed stream is a constant — so tapping stamped one of the 23
  cells, forever. The same constant governed every `fuzzy` rotation (23 presets), every scatter
  jitter and every mirror flip in the set.
- The reference seeds its per-stroke random source from a **global** generator (`KisRandomSource`'s
  default constructor takes `QRandomGenerator::global()->generate()`), so upstream a tap really does
  walk the cells.

**The fix: the engine derives the stroke's effective seed from `BrushParams::seed` AND the stroke's
own FIRST SAMPLE** (`strokeSeedFor`: position, pressure, both tilts and `timeUs`, by bit pattern,
through a splitmix64 finalizer), gated on `BrushParams::seedFromFirstSample`.

- ⚠ **It costs the replay contract nothing, and that is the whole reason it is shaped this way.** The
  seed stays a pure function of `(params, samples)` — same stream in, same bytes out — so goldens,
  undo replay and the preview all still reproduce exactly. **A clock-read seed would have to be
  RECORDED to replay at all**; the sample stream already carries this one.
- ⚠ `timeUs` is what separates two taps on the *same pixel*: it is the engine's own monotonic ingest
  clock (docs/tablet.md §5) and always moves between two presses. A caller that feeds no timestamps
  and does not move gets the same seed twice — which is honest, because that is the same stroke.
- ⚠ **The flag defaults OFF, and the two answers are not a preference.** A live canvas stroke must
  vary; a repaint of a dock preview card must NOT (a card that reshuffled its `fuzzy` dabs on every
  expose would be a different brush every frame). So `BrushPresetStore` sets it on the params it
  mints and `renderStrokePreview` clears it — and clearing `seed` alone would not have been enough,
  because the derivation folds the preview path's own first sample in.
- ⚠ Defaulting OFF is also what keeps **every existing golden byte-identical**: no test sets the flag,
  so nothing moved.

**Census honesty**: `t)_Shapes_Mecha` stays **Exact**, and the badge was not lying — the option
transcription really is complete, and the defect was in what the *application* handed the engine, not
in what the engine did with it. That is a distinction worth keeping: the fidelity badge measures the
preset→engine mapping, and it cannot see a host that reuses one seed. *(A preset that is Exact and
visibly wrong is still the worst outcome there is; this one is now Exact and right.)*

#### The slice: BUILDUP's half of the Opacity option

§3.10's census named the biggest remaining reason outright — **Opacity-under-Buildup, 6 presets**,
more than any other — and it is the deliberately-unfinished half of a mechanism already half
transcribed: per-dab Opacity landed for WASH only, because Wash is the reference's *indirect*
painting and that is the path whose composite was read.

The two halves are one option and two mechanisms, and the reference splits them in one place:

- **WASH = indirect painting.** Dabs composite into a stroke temp at ALPHA_DARKEN and the temp
  composites at the stroke's static opacity, so the per-dab value is a **ceiling the accumulation
  strives toward** (`washAlphaDarkenAlpha`, §6.2), and the option is read **without** the strength.
- **BUILDUP = direct painting.** There is no temp to strive toward: the option is applied to **each
  dab's own composite**, and the reference reads it **with** the strength
  (`computeSizeLikeValue(info, !indirectPaintingActive)` — one expression, one flag, both modes).

In Mosaic's accumulation that is a one-line difference, and the line already existed: the Buildup
accumulator's per-dab share was `a * cap`, and it becomes `a * opacity * cap`.

- ⚠ **The strength is `m_cap`, not the option's own `strength`, and reading it the other way would
  SQUARE it** — the same trap the Wash split documents from the other end. `m_cap` is where a
  preset's authored strength lands *and* where the context bar's Opacity slider lands, so
  `sensor × m_cap` is the reference's with-strength value with the ceiling the user can actually see
  standing in for the authored constant. Folding the option's strength in as well would paint a
  preset authored at 0.5 at 0.25.
- ⚠ **At a per-dab value of exactly 1 the new share is `1.0 * m_cap`, which IS `m_cap` in IEEE
  doubles** — so a full-pressure stroke through an identity curve is byte-identical to the static
  accumulation, and no Buildup golden moved. The gate is `optionIsDynamic`, the same shared predicate
  the importer's honesty contract reads, so a static option keeps the untouched path entirely.
- ⚠ **The COVERAGE buffer must not move.** Buildup composites out of its own accumulation, so an
  implementation that scaled the coverage instead would produce the identical image and corrupt only
  the Inpaint brush's hole mask. `buildCap` is **one function**, read by the Buildup alpha and by the
  Colored buffer's deposit weight — §6.1's rule that the two must agree, and two copies of it would
  drift — and the coverage is left alone. Both directions are tests.
- **It reaches the painters for free**: a `StrokePainter`'s mark resolves the opacity pair once per
  span exactly as a dab does once per dab, and lands in the same `deposit()`. That is what the three
  BUILDUP painter presets were badged for.

#### The third of the census that was never a missing mechanism

Chasing the airbrush's dead key exposed the same shape one row down. **`SmudgeRate` on a `paintbrush`
is read by nothing in the reference either**: its pixel brush constructs size, ratio, rate, softness,
lightness-strength, spacing, scatter, sharpness, rotation and opacity options, plus mirror and
precision, and **no smudge option at all**. Mosaic's engine reads the trio only when the smudge walk
is live, so the two strokes are the same stroke — and a badge measures the *difference*.

So the trio's drop outside the smudge family is **badge-free**, exactly like Mirror on a colorsmudge
preset (§6.6d), and the five presets that paid for it are the very same Krita-2-era files whose dead
`AirbrushOption/*` spelling froze the program: `b)_Airbrush_Soft` and the four `l)_Adjust_*`.

⚠ **The sketch trio (`Density` / `Line width` / `Offset scale`) deliberately keeps its badge.** The
same reading very likely applies — nothing but the sketch painter reads them upstream either — but
**no shipped preset trips that rule**, so there is no corpus evidence to move it on, and a badge that
overstates a loss is the safe direction to be wrong in. Recorded here so the asymmetry is a decision
and not a drift.

#### Census — 104 Exact / 7 Approximated / 6 Substituted *(was 94/17/6)*

Both sets were re-derived independently from the raw preset XML with §6.6g's attribute-order-safe
probe, which reproduces the pinned 94/17/6 with both changes switched off:

- **Buildup-Opacity: 6 carriers, 5 flip.** `c)_Pencil-5_Tilted`, `f)_Bristles-5_Flat`,
  `v)_Sketching-1_Chrome_Thin`, `v)_Sketching-2_Chrome_Large` and `v)_Sketching-3_Leaky` had no other
  drop. The 6th, `y)_Screentone_Moire`, keeps its antialiased-hatch-line drop. *(The other four
  Buildup presets in the set author `OpacityUseCurve=false` and were never badged — a static option
  takes the static path in either mode.)*
- **SmudgeRate-on-a-paintbrush: 5 carriers, all 5 flip.**
- What is left is **exactly one preset with an engine family** — `y)_Screentone_Moire`, for its
  hatch-line anti-aliasing — **plus the six with none**: `spraybrush` 4, `filter` 2. The census test
  now names all seven rather than only counting them: a total can be reached by two mistakes that
  cancel.
- The 6 Substituted are unchanged (`deformbrush` 3 → Liquify, `duplicate` → Clone Stamp, `gridbrush`
  → Tier 5, `tangentnormal` → Tier 3).

#### What this chunk deliberately did NOT do

- **The UI's stationary airbrush pump** (§6.6h): still owed, still an S-sized canvas slice, and still
  carrying its own bound — *emit at most one synthesized sample per timer callback, never a catch-up
  loop*. With only four presets airbrushing and none of them a 600 px nib, it is also much less
  urgent than it looked.
- **Moving the sketch trio's badge**, above.
- **A per-stroke seed for the tools that do not go through the preset store** (the plain round nib,
  the Inpaint brush). They carry no options and no hose, so nothing of theirs is random; the flag is
  set where the randomness is.

### 6.7 Would a `StrokePainter` widen third-party format support?
**No — its import value is ≈ 0.** Verified against the upstream class hierarchy:

- **`.abr`, `.gbr`, `.gih`, `.png`, `.svg` are *tip* formats, not engine formats.** Their reader
  classes all descend from the brush-**tip** base, never from the paintop base. They carry pixels.
  Supporting them needs **bitmap tips** (§6.2) and nothing more.
- **`.myb` (MyPaint) is the only other engine-carrying format, and it is a dab stamper.** Its paintop
  implements `paintAt()` + `updateSpacingImpl()` and never overrides `paintLine()`. Its substance is a
  sensor→curve model over dab radius/opacity/hardness/offsets/smudge — i.e. our `CurveOption`
  machinery. Importing it is a **sensor-name remapping table** (`speed1`, `speed2`, `stroke`,
  `direction`, `tilt_declination`, `tilt_ascension`…), not a new engine.
- Photoshop's engine is a stamper (tip + dynamics); Procreate brushsets are shape + grain stamps.

So `StrokePainter` would serve exactly the four engines of §6.6(b) and nothing else importable. Its
payoff is instead in **Mosaic's own future tools** whose mark depends on stroke history or state: a
sketchy/web/tendril decorative family, a calligraphy or ribbon tool, an inking aid that connects
nearby strokes. Those are features we would author, not presets we would import.

---

## 7. Preset library and formats

New `src/io/brush/`: `kpp` (chunk walker + XML), `gbr`/`gih`/`abr`/`myb`, `bundle` (zip), `mapper`
(paintop → our model + fidelity), `preset_json`.

- **PNG chunks:** rather than rewriting `png.cpp` onto libpng's low-level API, add a standalone chunk
  walker (length/type/data/CRC; zlib inflate for `zTXt`, a raw read for `tEXt` — §3.1, both carry
  `preset`). zlib is already present via libpng. The existing simplified reader still decodes the
  thumbnail pixels.
- **XML:** vendor **pugixml** (MIT, two files). The nested `brush_definition` sub-documents, CDATA,
  the legacy-vs-modern key variants and the non-fixed attribute order (§3.2) make a hand-rolled
  scanner a false economy.
- **Native preset `.mbp`** = PNG + `iTXt` keyed `mosaic-preset` carrying nlohmann JSON. Thumbnails
  appear free in file managers, and the reader is symmetric with `.kpp`. *(**It has a consumer since
  2026-07-29**: the brush editor's Save writes one into `dataDir()/brushes` and the store scans loose
  ones back — §8.3. ⚠ **Mosaic still writes no `.kpp` and reads no `.bundle` it did not first copy**:
  export to a third-party format is not a goal this slice took on, and the reasoning is in §8.3 ②.)*
- **Storage:** add `common::dataDir()` = `$XDG_DATA_HOME/mosaic` (`~/.local/share/mosaic`); user
  presets in `dataDir()/brushes/`. Bundled defaults are read-only from an install prefix. **This is
  the project's first runtime resource directory** — everything today is compile-time embedded — so it
  needs a `MOSAIC_DATA_DIR` compile definition plus a dev fallback to the source tree. (117 presets ×
  a 200×200 RGBA PNG is ~17 MB; embedding that in the binary is not an option.)
- The never-touch-user-files hard rule is respected: presets are app-owned data, never sidecars next
  to a document.

*(Built 2026-07-10, the first four Arc B slices: `png_text` (the chunk walker), `preset_xml`, `kpp` +
`mapper` + `preset` (the model + PresetProvenance), and `tip_io` + `abr` (the bitmap tip readers).
`bundle`, `common::dataDir()` and the preset library are still owed. Notes, each of which cost
something:)*

- **The `spacing` attribute's default is 1.0 on an `auto_brush` element and 0.25 on a predefined
  one.** The two factories genuinely disagree; §3.5's "absent means 0.25" is the predefined half of
  the truth. Pinned by a test and by the corpus.
- **Legacy `radius=` is a synonym for `diameter=`** — the same number under the 2.2 misnomer, *not*
  a half of it — and it wins when both attributes are present.
- **The `hasColorAndTransparency` verdicts are per-format quirks, not one rule**: a `bytes=4` GBR's
  is "any non-grey pixel" with alpha playing no part; a PNG's is likewise `!allGray`, but only after
  the mask-vs-image split, which *does* consult alpha; `bytes=1` anything and every ABR sample is
  simply false. The name promises more than any loader delivers.
- **`brushApplication` out of range must fall back to AlphaMask, not clamp** — clamping turns a
  foreign value into GradientMap, the least likely intent of the four.
- The walker needs a **per-file text budget** beside the per-chunk inflate cap: 64 chunks × 16 MB is
  a gigabyte of amplification out of a ~1 MB file.
- **ABR's computed-brush skip is broken in the reference** (it seeks `pos + next` where `next` is
  already absolute), losing every brush after the first computed one; Mosaic skips correctly. Its
  PackBits decoder also trusts the scanline lengths — ours bounds every write, and the unbounded
  mutant dies under ASan.
- The §3.9/§3.10 tables and the tip census reproduce **to the unit** through the full implemented
  chain (the one correction: §3.6.2's gih count). `pressure` reads as live in all 82 pixel-brush
  presets — the flooring the §3.10 correction block predicts.

*(Built 2026-07-10, the closing Arc B slices: `zip` + `bundle` (the container), `md5`,
`common::dataDir()`/`installedDataDir()`, the preset `library`, and the shipped CC-0 set
(`data/brushes/`, `docs/credits.md`, the project's first install rule). Notes, same discipline:)*

- **The ZIP truth lives in the central directory.** `RGBA_brushes.bundle` sets the
  data-descriptor flag on every entry, so its local headers declare 0/0/0 for crc and sizes —
  a reader trusting local headers loses that whole archive. It also **deflates its `mimetype`
  mid-archive** while Krita_4 stores it first; enforcing ODF's stored-first custom would reject
  a bundle Krita itself ships. The reader is hand-rolled over zlib (already a direct dependency;
  the `png_text` precedent) — stored + deflate is all real bundles use.
- **The manifest is a claim, not a listing.** Krita_4's own manifest carries 131 brush rows over
  79 distinct files — **52 files listed twice** (duplicate rows agreeing on md5sum); Krita_3
  ships one file no manifest row lists. Resources are enumerated from the zip CONTENT; the
  manifest contributes md5sum claims and four honesty counters (manifest-only, unlisted,
  pathless, duplicate — last row wins).
- **MD5 hex comparison must be case-insensitive**: two rows of Krita_4's manifest are uppercase
  (a different tool wrote them). All 1018 claims across the four system bundles match the
  computed digests — the corpus is the md5 implementation's real KAT.
- **No `.kpp` in the shipped corpus carries a tip `md5sum`** — the 2.2 format never writes it, so
  the whole default set resolves by filename and the md5 index (looked up FIRST, per upstream's
  `bestMatch`) serves v5 presets and third-party bundles. An md5 claimed but matched by filename
  only is a provenance note, not a fidelity loss — upstream warns on exactly that fetch.
- **An SVG tip is rendered 1000 px wide** with the height derived from the document's own aspect
  (integer division upstream, floor here), **over white, then greyed — the render IS the tip
  image**, uninverted, like a PNG. Spacing 0.25, mask type. The upstream has no size guard;
  `kMaxTipPixels` bounds a hostile aspect ratio here.
- **The six masking presets' absolute diameters verify by hand**: coeff × (auto diameter |
  post-`BrushVersion` scale × max(w, h)) reproduces 16 / 60⁄11 / 3 / 120 / 77.18 / 110 px from
  the raw XML and raw tip headers — the census test pins those independently derived numbers.
- The library census over the shipped bundle: 117/117 presets, 47 bitmap tips (23 hoses + the
  one 1000×1000 svg, `z)_Stamp_Leaves`), zero fallbacks — and **81 Uniform + 1 Colored**:
  `v)_Texture_Impressionism` carries *both* of §3.10's `h`/`v` colour-dynamics singletons, so its
  Colored verdict is the mapper's pinned rule, not a library decision.
- **Fidelity: 104 Exact / 7 Approximated / 6 Substituted** *(2026-07-28, §6.6i — the running history
  below stops at the fourth move; §6.6e/f/g/h/i carry the five after it, and the census test carries
  the derivation for every one)*. *(Moved a FOURTH time on 2026-07-14,
  when Scatter and Mirror were transcribed (§6.6d): 15 presets whose only drop was one or both of
  them came back Exact — re-derived independently from the raw bundle XML (PNG preset chunks →
  mapper-reason replay): of the 11 Scatter carriers and 11 Mirror carriers (3 presets carry both),
  the 35 remaining pixel+smudge Approximated keep honest reasons — Texture 21, Airbrush 7,
  SmudgeRate-on-paintbrush 5, Spacing 4, Sharpness 3, Opacity-under-Buildup 2, h 1, v 1.)*
  *(Third move, same day, when the smudge engine landed (§6.6c): `colorsmudge` maps to a real
  engine, so its 15 pay only for what they actually drop — 6 came back Exact (`i)_Wet_Circle`,
  `i)_Wet_Knife`, `j)_Watercolor_Fringe`, `k)_Blender_Knife_Edge`, `k)_Blender_Rake`,
  `k)_Blender_Smear`) and 9 stayed Approximated on real remaining losses: Scatter 3, Spacing 2,
  Texture 5 — at the fourth move the Scatter drops vanished entirely: `Wet_Bristles_Rough` and
  `Blender_Blur` went Exact, and `i)_Wet_Bristles` remains Approximated on its Spacing alone.)*
  *(Earlier the same day, 11 → 42: the wash accumulation drives a live-sensor Opacity end-to-end —
  §6.2's transcribed per-dab ceiling — so 31 presets whose only loss was Opacity came back Exact.
  History: 55/49/13 until 2026-07-12 — the mapper's "supported options" list was a claim the
  engine could not back (it named `Scatter`, `Mirror` and `Spacing`, and counted a pressure-driven
  `Opacity` as honoured while the engine drove only its static strength); then 11/93/13 with the
  honest badge, until the mechanisms themselves landed.)*
  *The then-42/41 pixel-family split was re-derived independently from the raw XML, and its 41
  Approximated reproduced §3.10's own counts to the unit: Mirror 11, Scatter 8, Sharpness 3,
  Spacing 2, h 1, v 1, real Texture 16, Airbrush 7 — plus the 2 BUILDUP presets whose live Opacity
  curve keeps the static ceiling (`c)_Pencil-5_Tilted`, `f)_Bristles-5_Flat`; the direct path's
  own composite is untranscribed, §6.2). The Mirror 11 and Scatter 8 rows are the ones §6.6d
  cleared.*

*(Built 2026-07-10, the Arc B leftovers: `myb` (the §6.7 remap importer) and `preset_json`
(the native `.mbp` container) + `io::encodePng`. Notes, same discipline:)*

- **`opaque_multiply`'s default is a LIVE pressure ramp, not its 0.0 base.** The consumer loads a
  file over a defaults-initialized brush (`from_defaults`, which installs pressure (0,0)→(1,1) on
  exactly this one setting) and the JSON updater overwrites **only the inputs the file names** —
  so a `.myb` that never mentions `opaque_multiply.pressure` keeps the ramp, while an **empty
  pressure array is an explicit clear**. Importing absence as constant 0 makes an invisible brush
  the producer never draws. Verified in `mypaint-brush.c`'s update path and the
  defaults-then-file load order of the reference consumer.
- **Duplicate-x knots are STEPS, and a knot union collapses them.** The opaque × opaque_multiply
  product is resampled at the union of both factors' knots; the shipped Ink pen gates pressure
  with `[[0,0],[0.015,0],[0.015,1],[1,1]]`, and `std::unique` on the x's turns that hard
  threshold into a half-range ramp. Every knot is hugged (x ± 1e-6) so a step in either factor
  survives as a near-vertical ramp — the same nudge `curveFromSamples` applies to duplicate x's.
- **`opaque_linearize` defaults to 0.9**, so an all-defaults `.myb` still imports Approximated
  with the linearization note: the producer's *default* behaviour includes a dab-overlap
  correction our engine doesn't have. Only a file that zeroes it can import Exact.
- MyPaint's per-dab alpha (opaque × opaque_multiply, clamped, **no stroke-level cap**) is our
  **Flow under Buildup**, never Opacity; Opacity imports as a constant 1.
- **Size dynamics ADD in log space = multiply the radius**: the static maxima fold into the tip
  diameter (with the producer's [0.2, 1000] px radius clamp) and each sensor keeps
  `exp(f − f_max)` sampled at every knot plus midpoint subdivision (chord error ≲ 0.15 %, below
  dab quantization — why a pressure-driven radius still counts Exact).
- **`direction` folds mod 180**, so the Rotation curve repeats on both half-turns of
  DrawingAngle, with hugging pairs at any wrap through 0/360 so interpolation never sweeps the
  long way round.
- The in-tree `.myb` fixtures are **synthetic** — Krita's shipped MyPaint presets carry no
  per-file license declaration upstream, so real files stay out of the repo; the suite replays
  the system corpus (`/usr/share/krita/paintoppresets`) when present, asserting every preset
  loads VISIBLE (the ramp rule's payoff).
- **`.mbp` reads strictly** — it is OUR format, so a missing field, foreign enum or newer schema
  is a loud load failure, the opposite of the total-function treatment third-party formats get —
  and the round trip is **byte-stable** (nlohmann's sorted keys + shortest-round-trip doubles;
  curves through `Curve::toString`). The preset chunk is matched by iTXt **keyword**, never chunk
  type (the `.kpp` lesson), spliced immediately before `IEND`; `io::encodePng` is savePng's
  encode into memory, **byte-identical** (pinned by test), with the output vector on the heap
  because a local mutated between `setjmp` and `longjmp` is indeterminate at the jump target.

---

## 8. UI

> **Reconciled against the tree 2026-07-12 (third pass, same day — §8.2 is now BUILT).** The library
> is CONSUMED: `ui::BrushPresetStore` scans `installedDataDir()/brushes` then `dataDir()/brushes` at
> startup, and the real binary logs `presets: 117 loaded, 0 failed; tips: 0 by md5, 54 by name, 0
> fallback`. A preset is resolved **when the user picks it, never per stroke** — resolving builds the
> tip and mints its raster id, and a fresh id every stroke is a permanently cold dab cache.
>
> - ✅ **§8.2 IS BUILT, and the stand-in `Choice` is GONE.** The right dock is a real container
>   (`ui::RightDock`) owning the tabbed Layers|History panel above a **Brush-preset grid**, with a
>   draggable horizontal splitter and a remembered height. The dock-*width* splitter moved off
>   `LayerPanel` onto `RightDock`'s own left edge (the Vulkan-sub-window rule that put it there is
>   unchanged). The grid is Brush-tool-only, has a search field, and decodes thumbnails lazily.
>   *(The `Choice` of 117 names was always a deliberate stand-in; the user called it in and it is out.
>   Its removal is pinned by a test, so it cannot creep back onto the bar.)*
> - ⚠ **The grid is ONE widget, not 118.** A keystroke re-filters with no widget churn, and "which
>   cells are visible" is arithmetic rather than FLTK clipping — which is what makes the layout, the
>   hit-testing and the filter pure functions, and therefore testable at all.
> - **The bar's primary set is Size + Opacity + Smoothing** (three — the preset is not on the bar).
> - **A preset SEEDS Size and Opacity rather than overriding them** — they stay the user's to steer.
> - **`hardness` is not merely homeless now, it is INERT over a real tip**: a tip carries its own edge
>   (§6.2). It belongs on the editor's tip page, where it can *rebuild* the tip that owns it — not on a
>   bar slider that would silently do nothing. Same for `flow`, which a preset carries as its always-on
>   Flow option's own strength. **§8.1's chip is what finally gives them a home.**
> - **The curve editor already exists** (`ui/curve_editor`) — §8.3 calls it "the one net-new widget".
>   What §8.3 still owes is the editor *shell*, the option pages and the paintable preview surface.
>
> So what is left of §8 is: **the chip (8.1), the editor shell (8.3) and the eraser's preset tie
> (8.4)**. The engine under all of it is done (§6.2) and the reticle it owed is paid (§6.3) — and
> §6.3's two visual passes (the traced contour, and the analytic ellipse) are now reachable from the
> grid, so both are owed.
>
> ✅ **§8.3 IS BUILT** *(2026-07-29)* — `ui/brush_editor.{hpp,cpp}`, the modal editor with the
> checkable option rail, the option pages and the paintable preview surface; and with it the
> **user-preset write path** (`dataDir()/brushes/*.mbp`, adopted back on the next scan) and the
> **Tools → Eraser preset-tie checkbox** §8.4 owed. **What is left of §8 is the chip (8.1)** and the
> visual passes.
>
> ⭐ **AND THE USER'S FIRST FEEDBACK ROUND ON IT IS FOLDED IN** *(2026-07-29, seven items)* — the
> scratchpad reads the **pen**, the rail's presets carry their **own stroke**, user presets wear a
> **badge** and come **first** in the dock, the scratchpad paints in the **preview's ink**, and there
> is **Delete** (confirmed, user presets only) and **Export**. §8.3's own section has the rulings;
> §8.2 carries the two that are the dock's.

### 8.1 Context-bar preview control (`ui::BrushPresetChip`)
About 150 px wide: a 32×32 thumbnail, the elided preset name, and a "modified" dot. Clicking opens
the editor.

**Thumbnail rule:**
- Preset **unmodified and imported** → its own **embedded PNG** (§3.1). No separate asset sourcing.
- Preset **modified (dirty), or native/procedural** → **generated**: an illustrative tool glyph
  (brush / pen / marker / airbrush / eraser — nanosvg, per `PLAN.md` §3.13's colourful-illustrative
  icon identity) with a **live stroke rendered beneath it** by the real engine at small scale.

Implementation: a new `ToolOptionKind::Preset` keeps `tool.hpp` FLTK-free and lets `tool_options.cpp`
map it to the widget — the precedent set by `ToolOptionKind::Font` and the special-cased `PaintSwatch`.
Expose the widget pointer as the editor's anchor, as `designerButton()` does.

This is also what finally gives `hardness` and `flow` a home (§2.3).

### 8.2 Preset section in the right dock
Below layers/history, **only when the Brush tool is active**. Two findings shape the work: Layers and
History are *tabbed inside one panel*, not stacked; and there is no tool-conditional docked chrome
anywhere in the codebase (`TypePanel`/`Type3dPanel` are `Popover` sub-windows).

So: a new `RightDock` container owning `[ tabbed Layers|History ]` above `[ Brush Presets ]`, with a
draggable splitter and a remembered height — rather than nesting a third region inside `LayerPanel`,
whose sub-regions are laid out with pixel constants.

**Presets belong to the Brush and the Eraser.** The other brush-family tools — Inpaint brush, Heal,
Clone, Smudge, Select brush — keep a plain circular tip with size/hardness, exactly as today. A
scatter-and-rotation tip on a *healing* brush is not a feature, and pretending otherwise would cost a
`usesBrushPresets` flag, per-tool preset state, and a preset-vs-tip mismatch every time a preset used
an unusable option.

#### ⚠ TWO CORPORA, AND THE SPLIT IS SEMANTIC *(built 2026-07-12, 16th session — the user's ruling)*

The dock shows **one of two corpora**, decided by the active tool, and they **partition** the library:

| corpus | shown to | rule | shipped count |
|---|---|---|---|
| `Brush` | the Brush tool | every preset that is **not** an eraser, plus the `Default round` cell | **114** |
| `Eraser` | the Eraser tool | **only** the erasers. No `Default round` — an eraser's "plain round" is its own | **3** |

⚠ **A preset is an eraser because it carries `CompositeOp=erase`** (→ `BrushPreset::eraserMode` →
`StrokeMode::Erase`, already honoured by the mapper), **not because it is named `a)_`.** The prefix
happens to agree on the three shipped ones; it is a filing convention, not the fact. A preset that
erases belongs to the Eraser wherever it sorts.

⚠ **The store holds TWO selections, not one.** Reaching for the Eraser must not silently re-point the
brush you were painting with, and reaching back must not re-point it again. Persisted separately
(`Settings::brushPreset` / `Settings::eraserPreset`), both **by name**.

⚠ **Showing the Eraser a picker it did not actually USE would have been decorative.**
`currentBrushParams` now takes the Eraser's own preset — its tip, its options, its spacing — and still
carves with them, because *an eraser carves whatever nib it is holding*. With no preset (a null tip)
flow stays at 1.0, exactly as it always did, so the old behaviour is the identity it always was.

#### Tabs — from the PREFIX, and NOT from the bundle's own tags *(built 2026-07-12)*

117 presets in one flat grid is a heap. The search field made it usable; it did not organize it.

⚠ **The bundle ships 9 tags of its own** (Digital, Sketch, Textures, Paint, Ink, FX, ★ My Favorites,
Erasers, Pixel Art — 187 assignments on 114 of the 117 preset entries in `META-INF/manifest.xml`).
**We still do not read them, on purpose: they OVERLAP.** 49 presets carry two or more; 3 carry none.
Tags are a *cloud*; tabs need a **partition**, or a preset appears in three places and answers to none
of them, and three presets appear nowhere. The **letter prefix** does partition it — and the grid was
*already sorted by it*, silently, since `presetDisplayName()` strips it off every label. The tabs make
a taxonomy that was already load-bearing **visible**.

| tab | prefixes | shipped (Brush corpus) |
|---|---|---|
| All | — | 114 |
| Basics | `b)` | 8 |
| Draw | `c) d) e)` | 15 |
| Paint | `f) g) h) j)` | 21 |
| Blend | `i) k)` | 15 |
| Texture | `y) z)` | 29 |
| Effects | `l) x)` | 7 |
| Special | `t) u) v) w)` + **catch-all** | 19 |
| User | *(by source, not prefix)* | 0 — **appears only once the user installs one** |

⚠ **A test walks every shipped preset and asserts it lands in EXACTLY ONE media tab.** A tab set that
is not a partition loses things. `Special` is the deliberate **catch-all** (`Other`, and an `a)`-named
preset that does not actually erase) so that no preset can ever be in the library and in no tab — a
preset the user cannot find is worse than one filed oddly.

⚠ **`isUserPreset` is an INDEX RANGE** against the boundary `scan()` draws between the shipped
directory and the user's — and a bare `scanDir()` (every test; any future importer) draws **no**
boundary. Without the `>= 0` guard, `index >= -1` is true for *every* preset and the whole shipped
bundle files itself under the user's own brushes. **That guard survived a mutant until it got its own
case.**

The strip is **one widget**, scrolls horizontally (drag or wheel — eight tabs do not fit a 280 px
dock), and draws an arrow at whichever edge still has tabs beyond it. ⚠ **A drag that MOVED is a
scroll and must not also pick the tab it began over.**

The **list itself drags too** *(2026-07-14)*, the strip's mechanic turned vertical, and a drag
released at speed **flings** — both of them. The decay is analytic (`presetFlingStep`: one 32 ms
tick lands exactly where two 16 ms ticks do, so a janky timer delays a fling but never lengthens
it), and the release velocity is read from the drag's own last 120 ms (`PresetFlingTracker`) — ⚠ **a
pointer that STOPS before it lets go is a HOLD and releases dead**, which is what a finger on glass
does. The grid's pick therefore **moved from PUSH to RELEASE**: a press is not a click until it
proves it did not move. And a card wears **rings, not fills** *(both the user's call)*: hover is
2 px of `controlActive`, selection 2 px of accent, and the two differ by ink alone — a full-width
row turning lighter under the cursor OR under the selection was a slab of light sweeping the dock
("busy", the user's word). The dense grid keeps its *hover* fill, where a small tile tinting reads
as a highlight rather than a slab; its selection is the ring too.

The header's denominator is the **corpus**, not the library: a Brush grid reading "117" would be
counting brushes it will not show you, and "12 of 117" could never reach its own total.

#### ⭐ THE USER'S OWN PRESETS COME FIRST *(the user's call, 2026-07-29)*

`presets()` used to be *"the shipped scan, then whatever the user installed"* — a brush you made
yourself sat behind 114 you did not. It is now **the user's own, then everything Mosaic ships**, and
**within each half nothing moved**: the library's bundles still lead the loose files and each keeps
its scan order, so the reorder moves the user's run and nothing else.

Three things read that order, and all three had to move with it:

- ⚠ **`isUserPreset` was a SUFFIX range and is now a PREFIX one.** It is still an index range and not
  a path comparison (a path comparison would have to re-derive `installedDataDir()` here and agree
  with it forever), and it still carries **the `-1 = no boundary was drawn` guard** — which now lives
  in `shippedLibCount()`/`shippedLooseCount()`, where it reads *"no boundary means the whole run is
  SHIPPED"*. Get that backwards and the whole bundle files itself under the user's own brushes **and**
  jumps to the front of the dock.
- ⚠ **Every index a write HANDS BACK goes through the inversion.** `writeUserPreset` used to return
  `libCount + looseSlot`, which is where the preset sits in the *storage*, not where it sits in the
  *corpus*. `mergedIndexOfLib` / `mergedIndexOfLoose` are the one place the ordering is inverted, and
  `looseSlotOf` is the way back (which preset has a file of its own to overwrite or delete).
- ⚠ **`Selection` already carried its NAME beside its index**, which is what makes a reorder free:
  `rebuildAll()` re-points from the name, so a runtime import — or a save that pushes the shipped run
  along by one — cannot silently re-point the brush you were painting with.

⚠ **Index −1 did not move, and must not.** It is still the one cache key whose meaning depends on the
**corpus** (the Brush's `Default round` paints, the Eraser's carves), it is still slot 0 of `All` and
`Basics`, and switching corpus still evicts exactly that key. A test pins the two facts together:
after the reorder the first *library* cell is the user's brush, and −1 still flips its stroke mode
with the corpus.

#### The fidelity badge stops understating *(2026-07-12)*

⚠ **"Approximated" is not always the same size of lie.** For most presets it means what it says: some
*options* were dropped and the rest paints. But for **source paintops Mosaic has no engine at
all** — `spraybrush` (4 presets), `filter` (2) — the preset imports as a plain pixel brush wearing
that paintop's tip, and the generic line reads as a bug report. Those carry the specific sentence
("No spray engine yet: this stamps its tip. It does not scatter particles."). See §6.6b for the arc
that fixes them. *(`colorsmudge` — 15 presets, "a Blender that does not blend" — left this list on
2026-07-14 when the smudge engine landed, §6.6c: its presets really smear now, and whatever they
still drop is listed like any pixel brush's.)*

*(**Built 2026-07-12** — `ui/right_dock.{hpp,cpp}` + `ui/brush_preset_panel.{hpp,cpp}`. The stand-in
`Fl_Choice` of 117 names is **gone** (`tool.cpp` no longer publishes a `preset` option at all, and a
test pins its absence, so it cannot creep back onto the bar).*

*What the build settled:*
- ***A grid, and a search field.*** The combobox was not merely off-spec, it was unusable: 117 names
  in a pull-down cannot be hunted through by eye, and a name is not what a brush looks like. The two
  things that make the panel work are the **filter** and each preset's **own embedded PNG** (§3.1).
  The filter matches on a NORMALIZED name (`i)_Wet_Knife` → `i wet knife`), token-wise, so "wet knife"
  and "knife wet" both find it — nobody is going to type the parenthesis. The same normalization is
  why a cell's LABEL sheds the corpus's sort prefix ("Eraser Circle"), while the tooltip keeps the
  exact name.
- ***The grid is ONE widget, not 118.*** A widget per cell would rebuild the whole child list on every
  keystroke, and would leave "which cells are visible" — the question the lazy decode hangs on — to
  FLTK's clipping rather than to arithmetic that can be tested. Layout, hit-testing and filtering are
  pure free functions.
- ⚠ ***Thumbnails decode lazily, and that is load-bearing.*** Only a cell that actually paints asks for
  its icon (the `fl_not_clipped` gate in `PresetGrid::draw`), and the panel caches what it decoded,
  downscaled to the cell. Decoding all 117 up front is ~19 MB and a stalled startup. A decode MISS is
  cached too, so a broken `.kpp` is not re-opened every frame.
- ***The fidelity badge is a DOT, not a glyph.*** 106 of the 117 shipped presets are Approximated or
  Substituted (§6.4) — a letter on nearly every tile would be noise, not signal. Amber = approximated,
  red = substituted; the tooltip carries the paintop and what was actually dropped.
- ⭐ ***And since 2026-07-29 there is a SECOND badge: the USER mark*** *(the user's call — "user
  presets have no badge to indicate they are user presets")*. It has to be told apart from the
  fidelity dot at a glance, so it differs on **all three axes a 13 px mark has**: the **opposite
  corner** (top-left, where a fidelity dot can never be), the **accent** ink (never amber or red,
  which mean *something was lost*), and a **ring** rather than a filled disc. Colour alone would not
  have done it — the two would have read as one badge in two moods. The tooltip says it in words too
  ("Your own preset"), because a coloured ring is a hint and a sentence is an answer. The same mark
  appears on the editor's rail rows (§8.3): one convention, two surfaces.
  ⚠ **It doubled the number of corners the frame-order bug can happen in** (`962053b`): both badges
  are opaque 13×13 blits whose ground is the thumbnail's own PIXELS, which know nothing of a frame
  painted over them. **The framing hairline is painted LAST**, after both.
- ***"Default round" is a real cell*** (slot 0, index −1) = NO preset = the engine's own analytic
  circle. Getting back to plain has to be a click, not a mystery.
- ***The dock-width splitter moved to `RightDock`***, which now owns the dock's left edge — the panel
  is no longer the dock. It is still a grab band on the container's own edge, for the reason it always
  was: `VulkanCanvas` is an `Fl_Window` and no sibling widget may paint over a child sub-window.
- ***Persisted*** as window LAYOUT state, like `dockWidth`: `Settings::brushPresetHeight` and
  `Settings::brushPreset` — **by name, never by index**. The library's order is a directory scan, so an
  index would silently point at a different brush the moment a bundle is added or renamed.
- ⚠ ***A pick is not "an option changed".*** The panel reports an index; `MainWindow` calls
  `BrushPresetStore::select()` — the ONE place a preset is resolved, because it mints the tip's raster
  id and a fresh id per stroke is a permanently cold dab cache — and then SEEDS the bar's Size and
  Opacity from it. Seeding, not overriding: those two stay the user's to steer.*

#### ⭐ CARDS — the dock's default: the brush's own STROKE, not its picture *(built 2026-07-12, 17th session)*

The grid answers *"what does this brush's picture look like?"*, which is **nobody's question**. The
question everyone has is *"what mark does it make?"* — so the dock's **default** mode is now a list of
**cards**, one preset per row: the tip icon on the left (with its fidelity badge), the name, and a
long strip carrying **a stroke of that very brush, laid by the real `BrushEngine`**. Grid stays, as
the denser mode for reaching a brush you already know by sight; the two are chosen in
**Settings → Tools → Brush**, on `OptionCard`s (the widget the Move tool's multi-selection chooser
uses), persisted by NAME as `Settings::brushPresetDisplay`.

**The tip icon is the SAME BOX as the strip** — same top, same bottom, same height. Two boxes side by
side that *almost* line up read as a mistake, and they were one.

**The `Default round` cell belongs to BOTH corpora** *(the user's call)*. ⚠ It is **not the same cell**:
the Brush's plain nib **paints** and the Eraser's **carves**, which is exactly what each tool does when
it holds no preset — a card previewing the eraser's plain round as a stroke of paint would be
advertising the wrong tool. ⚠ Index `-1` is therefore **the one cache key whose meaning depends on the
corpus**, and both caches are keyed by index alone, so switching corpus evicts exactly that key.

**The dock opens on `Basics`, not on `All`** *(the user's call, and it is right: 114 cards is not a
place to start, and the six Basic presets plus the plain round nib are)*. **The `Default round` cell
rides `All` and `Basics`, first in both** — the plain round nib is the most basic thing in the library,
and a Basics tab without it would open on a page that cannot get you back to plain. It rides no other
media tab: repeating it in seven places would be noise in seven places. ⚠ The Eraser corpus has no
`Basics` tab at all, so the opening tab **falls back to `All`** rather than opening on an empty grid —
which `PresetTabStrip::setTabs` already did for any tab a corpus does not have. ⚠ `kDefaultPresetTab`
is **one** constant: the panel and the strip each used to carry their own copy, and they were not even
equals — `rebuildTabs()` overwrites the panel's from the strip's, so the panel's was **dead code**, and
a mutant that changed it did not move a pixel.

**The renderer is `core/brush/stroke_preview.{hpp,cpp}`** — pure, FLTK-free, and it drives the REAL
engine over a plain `common::Image`. There is no second engine and there must never be one: a preview
drawn by a lookalike is a drawing of a brush we do not have. §8.3's editor preview and §8.1's chip
are the same renderer, for the same reason — **two previews of one brush that disagreed would be
worse than no preview at all.**

**THE PAPER AND INK FOLLOW THE THEME** *(the user's call, and a reversal of what this first shipped
as)*. **Light theme: black on white**, which is right there and stays. **Dark theme: the paper is the
panel's own ground and the ink is the muted text colour** — a white slab per row on a dark UI is a line
of lightboxes in a dark dock, and it hurts to look at. `core` never learns what a palette is; the style
is a parameter, and `ui::presetStrokeStyle` is the UI choosing it.

⚠ **THE PAPER MUST BE OPAQUE.** **Three presets are ERASERS**; an eraser lays nothing on an empty
canvas, it can only take away. Opaque paper is what turns a carve into something you can see. *(The
card blits with alpha, so the hole shows the dock through it.)*

⚠⚠ **AND AN ERASER'S PAPER IS NEVER THE DOCK'S OWN GROUND — which is exactly what the dark theme's
paper is.** What shows through the hole an eraser bites is **the dock**, so a panel-coloured paper
would carve a panel-coloured hole in a panel-coloured card: **perfectly invisible, in the theme the
user actually uses.** An eraser's paper is therefore the muted ink itself — *a slab of paint, which the
eraser removes to reveal the dock behind it*. That reads in **both** themes; the old
white-paper-on-a-dark-dock only ever read in one, and it read **by accident**.

⚠⚠ **AND FIVE PRESETS CANNOT MARK WHITE PAPER WITH BLACK INK AT ALL — measured, not guessed.**
*(⚠ In the DARK theme the ink is brighter than the paper in every channel, so the blind set would flip
to `Darken`/`Multiply`/`Burn` — and the shipped corpus **has none of those**: measured, **nothing is
blind in the dark theme.** That asymmetry is precisely why the fallback triggers on the **result** and
not on a list. A corpus case now checks every preset against **every palette we ship**.)*
`l)_Adjust_Lighten`, `_Dodge`, `_Color`, `_Overlay_Burn` and `y)_Texture_Starfield` leave white paper
**exactly as they found it**: every one of them paints through a BLEND MODE, and Lighten / ColorDodge /
Color / Overlay / Screen are all **the identity for black over white**. They are not broken — white
under black is simply a pair they cannot move — and left alone they would sit in the dock as five blank
cards looking like five bugs. **A stroke that leaves the paper completely untouched is therefore re-laid
once on a mid-grey paper under a saturated ink.**

⚠ **AND IT IS THE PAIR, NOT THE PAPER.** Swapping the paper to grey and keeping the black ink fixes
**nothing**: black is darker than every paper in *every channel*, so `Lighten` is `max(x, 0) == x`
against all of them. A blend mode can only move a pixel when the ink is brighter than the paper
somewhere and darker somewhere else. *(That is not a hypothesis — it is what the first cut of the
fallback did, and the test caught it.)* ⚠ **The trigger is the RESULT, never a list of preset names:** a
hard-coded list of the five would be a sixth bug waiting for the next blend mode to ship.

⚠ **THE STRIP IS RENDERED WIDE AND DRAWN CROPPED — never scaled.** A stroke preview costs **~1.7 ms**
through the real engine, a thousand times a thumbnail's box filter. A strip whose rendered width
tracked the dock pixel-for-pixel would re-render every visible card on **every frame of a width drag**
— the same bug the dock was dug out of the session before, in a form a hundred times more expensive.
So the render width is **quantized to a 32 px bucket** and the middle of it is cropped into the strip.
Cropped, not scaled: a scaled stroke lies about the brush's size, which is the one thing the card
exists to be honest about. A test drags the dock across a bucket's width and demands **zero**
re-renders — asserting on `strokeRenders()`, an **EVENT** count, because a cache SIZE cannot witness a
re-render (a re-render refills the cache to exactly the size it had).

⚠ **THE PATH INSETS ITSELF BY THE BRUSH'S RADIUS** *(a user-reported bug: "the stroke previews like to
go outside the bounds of their box")*. The path runs through the dabs' **centres**, so a curve laid out
against the full box hangs **half a nib over every edge of it** — and the wider the brush, the further
over. The curve is laid inside a box shrunk by the radius on all four sides; a brush wider than its box
collapses to a line down the middle and fills it, which is the honest picture of a brush wider than its
box. ⚠ The card's diameter ceiling (28 px in a 58 px strip) is set **against** that: a ceiling anywhere
near the strip's own height leaves the S-curve no room to swing and flattens it into a fat straight
sausage. *(⚠ A test on `strokePreviewPath` alone cannot see this — the mutant that computed a beautiful
inset path and then asked the renderer for an un-inset one sailed straight through it. The unit was
pinned and the WIRING was not.)*

⚠ **THE DIAMETER CEILING SCALES THE MASKING BRUSH TOO, and that is not a detail.** The masking tip's
size is authored as a *coefficient of the master size* (`MaskingBrush/UseMasterSize`) and only resolved
to an absolute at load. Cap the primary and leave the mask where it was and you have not drawn the
same brush smaller — you have drawn a **different brush**, wearing a mask several times too big for it.
Measured on `g)_Dry_Bristles_Eroded` (a 120 px nib under a 120 px SUBTRACT mask): capping the nib to
40 px and leaving the mask at 120 took the preview from 74 marked pixels to **exactly 0**.

⚠ **AND THE PREVIEW FOUND A REAL ONE — now FIXED (2026-07-14).** `g)_Dry_Bristles_Eroded` previewed
**almost blank even at true scale with its authored ratio** — 74 marked pixels out of 14,080; with
masking off, 5,531. That was not a preview fault: the masking walk **did not stamp a tip, it stamped a
round analytic disc**, so a mask authored as an *eroded texture* (grain and holes) subtracted a solid
disc covering the whole nib and very nearly deleted the stroke. The near-blank card was the truth
about the canvas too. The fails-on-purpose exception case that pinned it did exactly what it was built
to do the day the masking walk learned to stamp the real tip (§6.2): it failed, it was deleted, and
the preset is back in the corpus case — previewing at 2,722 marked pixels, with a positive pin that
the mask keeps biting. (An exception list that cannot fail is an exception list that rots. This one
could, and did.)

*The preview path is §8.3's, exactly: one cubic Bézier S-curve, pressure ramped 0 → 1, with speed and
tilt ramped along it too. It must be a CURVE (a straight span makes the curve's tangent and a chord
bit-identical, so all 14 heading-following presets would preview as if they ignored the stroke) and the
pressure must reach BOTH ends (`z)_Stamp_Shoujo_Bubbles` has an inverted size curve — press harder,
paint smaller — so a ramp that only pressed hard renders a working preset blank). A corpus test paints
all 117 and demands not one blank card.*

### 8.3 The brush editor — modal, with a paintable live preview
Structurally a clone of `LayerEffectsDialog`: fixed `size_range`, `set_modal()` after `end()`, left
rail + control stack, its own themed dropdown/flyout child sub-windows (a modal is its own top level).
**But no `applyLive`** — the document is never touched. `commit()` writes the preset, not pixels.

Layout, ~1040×640:
- **Header:** thumbnail │ name + source engine + fidelity badge + dirty dot │ Save / Save As / Import…
- **Left rail:** preset list (also the eraser's browse surface) above a checkable option list, grouped
  General / Colour / Texture / Tip.
- **Centre:** option page stack (`ScrubSlider`, `Dial` for angle, the new curve editor).
- **Right — the preview surface.** One canvas, two jobs: it renders the **auto stroke** on every
  settings change, *and it is paintable* — you can draw on it with the brush you are editing. A
  **Reset** button restores the auto stroke. It is a plain CPU `common::Image` driven by the same
  `BrushEngine`; it never becomes a layer, never enters the undo stack, and never reaches the document.
- **Bottom:** the two eraser-tie checkboxes.

Merging preview and scratchpad is what makes a *modal* editor tolerable: the reason to want non-modal
is "test it on something real," and a paintable surface answers most of that without ever risking the
artwork.

**Net-new widget: a curve editor.** Nothing comparable exists (`GradientFlyout` is the nearest model
for small direct manipulation). Add/drag/remove control points, corner-vs-smooth nodes, [0,1] domain,
serializing to `"x,y;x,y;"` so imported curves round-trip byte-exactly.

*(**Built 2026-07-11**, `ui/curve_editor.{hpp,cpp}` — ahead of Arc D, because Settings → Tablet's
pressure curve needed it first (`docs/tablet.md` §8) and one widget serves both. It edits a
`core::brush::Curve` itself rather than its own point type, which is what makes an imported preset's
curve survive a visit byte-exactly. Drag a point; click empty space to add one (and keep dragging —
one gesture, not two); right-click to remove, **never an endpoint**, because a curve whose domain
does not span [0,1] silently clamps flat past its last knot; double-click to toggle a **corner**,
drawn as a diamond against a smooth point's circle. Endpoints are pinned in x. A dragged point is
clamped strictly between its neighbours: `Curve` DROPS a point sharing an x with an earlier one, so
without the clamp a drag onto a neighbour deletes the point under the cursor mid-gesture — and for
the same reason the drag index is re-derived from the curve's OWN view after every commit, since
`Curve` is free to sort and de-duplicate underneath the editor. The plot is composed into an RGB
buffer with software AA coverage and blitted once; `fl_line` is not anti-aliased. Tested headlessly:
an `Fl_Widget` needs no display to take an event, and `Fl::e_x`/`e_y`/`e_keysym` are public — 8 cases,
5/5 mutants.)*

**The stroke-preview path** — ***BUILT 2026-07-12** as `core/brush/stroke_preview.{hpp,cpp}`, and the
dock's card mode (§8.2) already draws through it; the editor reuses it rather than growing a second
one* — is a single cubic Bézier S-curve — endpoints at `(cx−0.45w, cy+0.20h)` and
`(cx+0.40w, cy−0.20h)`, control points `(cx, cy−h)` and `(cx, cy+h)` — with pressure ramped 0→1 along
it. One rise and one fall shows the taper, the thin end, the thick end and both curvature directions
exactly once; a multi-period sine spends most of its arc length repeating itself and crowds the thick
end. Two deliberate improvements over the reference implementation: ramp **speed and tilt** along the
path too, so speed- and tilt-driven presets preview truthfully; and render at **true brush scale**,
clipping the preview box, rather than clamping size into a 3–25 px window that makes large brushes lie.

#### ⭐ AS BUILT — `ui::BrushEditorDialog`, 1040×640 *(2026-07-29, 26th session)*

`ui/brush_editor.{hpp,cpp}`. The spec above is what shipped; what follows is what building it
settled, and the four decisions the build owned.

**① THE EDIT MODEL: a mutable COPY, and a cancel that has nothing to undo.** The dialog holds
`io::brush::LibraryPreset m_working` (params + options + the tip reference, already resolved) beside
`m_original`, and a `dirty` flag. Nothing is applied anywhere — there is no `applyLive` and no
document seam at all — so **a cancel is a close**, and "the store is byte-identical" is a property of
the *structure* rather than of a revert path that could get one field wrong. `Save` is the only thing
that writes, and it writes a preset. ⚠ Browsing the rail's preset list is a **re-seed**, and it drops
a dirty working copy exactly as Close does; the dirty dot is the whole warning. *(A confirm sheet on
a dirty close is owed — it needs a second modal over a modal, which is its own piece of work.)*

**② WHERE A USER PRESET IS SAVED — `dataDir()/brushes/<stem>.mbp`, and NOT a `.kpp`.** ⚠ **Say this
plainly: Mosaic does not write `.kpp`.** Round-tripping one means a PNG-chunk writer plus an XML
serializer for 94 params whose defaults, spellings and two prefixing rules (§3.2) all have to be
reproduced exactly or the file loads *differently somewhere else*. The native `.mbp` container
(§7, `io/brush/preset_json.hpp`) already existed, already round-trips a `BrushPreset` losslessly,
already has a strict reader with a test suite — and **had no consumer**. It has one now.

`BrushPresetStore` therefore scans **loose `.mbp` and `.kpp` files** beside the `.bundle`s, in both
directories, and `presets()` is the library's run followed by the loose one. ⚠ **The corpus is TWO
RUNS, so the shipped|user boundary is TWO numbers** (`m_libUserSplit`, `m_looseUserSplit`) — a single
index would silently assume nothing loose was ever installed into the *shipped* directory. Both keep
the `-1 = no boundary was drawn` guard that a bare `scanDir()` relies on (§8.2).

⚠⚠ **A LOOSE PRESET IS RESOLVED BY ADOPTION, NEVER BY RE-DERIVATION.** A `.kpp` inside a bundle sits
next to the tip files and texture payloads it references, and `PresetLibrary` decodes and **bakes**
them on the way in. A loose file references the same resources and ships none of them. So
`adoptLoosePreset` reuses a build the library *already made*: the `BitmapTip` built for the same
resolved tip **file name** (with the application and all three adjustments agreeing — a donor built
for a different application is a different tip wearing the same name), and the texture mask baked
from the identical embedded payload under identical bake parameters. Nothing here re-runs §3.5's
content test or §6.6h's bake; a second copy of either rule would drift the first time the original
moved. **What finds no donor degrades exactly as the library degrades an unresolvable bundle
reference** — the round tip / texturing off, a line in `provenance.droppedOptions`, fidelity floored
to Approximated. A preset never disappears, and it never looks intact when it is not.

The consequence, stated so nobody is surprised by it: **a user preset derived from a shipped one
carries its tip and grain for as long as that bundle is installed, and falls back (badged) if it is
removed.** Within the session that saved it there is no fallback at all — the store keeps the
editor's own fully-resolved copy, so the brush you just saved paints exactly what you previewed.

Two smaller rules the save depends on, both pure and both pinned:
- `uniquePresetName` — a save never shadows an existing name (" copy", " copy 2", …), because the
  settings persist the selection **by name** and `indexOfName` takes the first match: two presets
  answering to one name make the restored brush a coin toss between the user's edit and the file
  they edited it from. An empty/whitespace name becomes `Brush`; `""` is how the settings spell NO
  PRESET and `indexOfName` refuses to match it.
- `presetFileStem` — the name reduced to ASCII alphanumerics and `-`, runs collapsed to one `_`,
  capped at 64. Deliberately **not injective**; collisions are settled by a numeric suffix at write
  time, because a file name is not an identity and the preset's real name lives in the container.

⚠ **A shipped bundle is READ-ONLY.** There is no `.bundle` writer and there must not be one that
edits the set Mosaic ships, so `Save` on a shipped preset always creates a user preset beside it.
`writeUserPreset` refuses an in-place overwrite of anything that is not one of the user's own loose
files, rather than quietly minting a second brush under the same name.

**③ THE PREVIEW SURFACE, and the §8.2 lesson restated one layer up.** One `ScratchCanvas`: a plain
CPU `common::Image`, the auto stroke from `core/brush/stroke_preview.hpp` (the dock's renderer — there
is still exactly one), and the real `BrushEngine` painting into it on a drag. ⚠ **A settings change
re-renders the auto stroke only while the surface still IS the auto stroke.** Once you have painted on
it, it is a scratchpad, and wiping your marks on every tick of a slider drag would make the scratchpad
useless exactly when it is most wanted; `Reset` brings the auto stroke back, rendered with the settings
as they now stand. ⚠⚠ **Both the preview render AND the params rebuild are COALESCED onto one
timeout.** A preview stroke costs ~1.0–1.7 ms and `presetBrushParams()` **re-mints the tip's raster
id** (a cold dab cache); a `ScrubSlider` fires `FL_WHEN_CHANGED` on every motion event, so doing either
inline would put both on the drag's critical path — which is precisely the bug the dock's stroke strips
were dug out of. One rebuild per tick, whatever the drag did. The scratch half sets
`seedFromFirstSample` (a live stroke must vary, §6.6i) where the auto half pins the seed; a mouse
reports pressure 1 and nothing else, which is honest — the *auto* stroke is where pressure, tilt and
speed are ramped.

**④ THE RAIL'S OPTION GROUPING IS A PARTITION**, and a test walks `kDrivenOptions` demanding exactly
one group each — §8.2's tab rule one level down, for the same reason: an option in two groups appears
twice and answers to neither. General = Size/Opacity/Flow/Spacing/Rate/Sharpness; Colour = h/s/v and
the smudge trio (the engine moving colour that is *already* on the canvas); Texture =
`Texture/Strength/`; Tip = the nib's own geometry, the positional options, and the mark geometry of
the second engine kind. An **unknown** base files under Tip rather than vanishing. ⚠ **Only the
preset's OWN options get rows** — an absent option is not a disabled one (§6.2), and a control for one
would invent an option the file does not carry. ⚠ **Opacity and Flow get no checkbox**: their
`Pressure{X}` bit is written to shipped files and ignored by every reader (§3.2), so a box there would
be a control that does nothing. The row's checkbox *is* the format's enable bit — the editor exposes a
bit that was always there rather than inventing state.

Smaller things the build settled:
- **Both rail lists are ONE widget each**, not one per row — the preset grid's rule at a smaller
  scale, so a working-copy change churns no children and hit-testing is arithmetic a test can drive.
- ⚠⚠ **Several page controls change what the page HOLDS** (picking a different sensor, switching one
  on, sharing one curve), and rebuilding the page from inside such a control's callback would
  `Fl_Group::clear()` the very widget whose `handle()` is still on the stack. The rebuild goes
  through a **zero-length timeout**. Both timeouts are removed in the destructor.
- ⚠ **The per-sensor controls look their sensor up BY ID, never through a captured `Sensor*`.** The
  "Active" box pushes into the very vector such a pointer would point into; a reallocation leaves it
  dangling for exactly as long as the deferred rebuild takes.
- **The sensor picker is one `Dropdown` over all sixteen, with the active ones DOTTED** in the open
  list (`setMarkedItems`, which exists for this shape of question) plus an "Active" box. ⚠ It never
  goes to zero sensors: an empty list makes `optionIsDynamic` read the option as *static*, which is
  what the "Let the sensors drive it" box already says in words.
- ⚠ **The Diameter control writes the resolved master size AND the auto tip's own generator
  diameter** — the reload reads the generator (the generator *is* the tip) — **and re-resolves the
  masking brush through `resolveMasking`**, because the mask's size is a coefficient of the master
  size (§6.2) and a resized nib would otherwise wear a mask several times too big for it.
- **The header thumbnail is the brush's own stroke**, per §8.1's rule that a *modified* preset shows a
  generated stroke rather than its embedded PNG — and inside the editor every preset is modified by
  definition, so there is no icon-decode path here to keep in step with the dock's.
- **The launch point is the DOCK, not the menu bar**: an "Edit…" button beside the search field
  (greyed on `Default round`, which is the *absence* of a preset and has nothing to edit) and a
  **double-click** on a card. A menu item was declined on purpose — the menu tree is all-or-nothing
  across 74 catalogs (`docs/i18n`), and this feature does not need to move it.
- **Import…** takes `.mbp`, `.kpp` and `.bundle`, copies the file into `dataDir()/brushes` so the
  import survives a restart, and runs a colliding name through `uniquePresetName` **and rewrites the
  file under the new name** — or the next scan would import the collision all over again.

#### ⭐ FEEDBACK ROUND 1 — seven items, all the user's *(2026-07-29, 27th session)*

The editor landed and the user used it. Seven findings, in their order.

**① THE SCRATCHPAD NOW READS THE PEN.** *"Scratchpad does not care about tablet pressure."* It
pinned `pressure = 1.0` and nothing else — so the one surface that exists to show a preset's dynamics
was the one surface that could not, and most of the corpus is pressure-driven. It previewed every one
of them as a dead constant while the stylus was reporting perfectly well two windows over.

- ⚠ **TABLET DELIVERY IS PER-WINDOW ON BOTH PLATFORMS.** A backend brought up on the canvas sees
  *nothing* while the pen hovers this dialog — which is the entire time the dialog is up. The dialog
  therefore **watches itself** on `show()` and **unwatches on `hide()` AND in the destructor** (which
  `~Fl_Window`'s own `hide()` cannot do for us: it is called from a base destructor, where the
  override no longer exists). This is exactly what Settings → Tablet's test area already had to do.
- ⚠ **THE POSITION IS FLTK'S, THE DYNAMICS ARE THE DEVICE'S — and they are not the same question.**
  FLTK has already routed the event to this widget (on Wayland the tablet wiring *synthesizes* that
  routing, `docs/tablet.md` §4), so the event's x/y is the coordinate that is certainly right for this
  canvas; a `TabletSample::pos` is surface-local to whatever surface it came from and would need the
  whole gate `VulkanCanvas` has. Pressure, tilt and rotation have no such ambiguity — they are
  properties of the nib, not of a window.
- **ONE sample per event**, not the ~200 Hz segment the canvas drains: a scratchpad interpolates
  between motion events like a mouse stroke does. What it must not do is paint at a pressure the pen
  is not making. *(A mouse still reads 1 — never 0, §3.2, or size/flow dynamics collapse the stroke.)*
- The seam is three `std::function`s on `BrushEditorHost` (`tabletReading` / `tabletWatchWindow` /
  `tabletUnwatchWindow`), the shape `SettingsHost` already uses, so the dialog never reaches into the
  canvas. The reading is **post-policy** (§7 of `docs/tablet.md`): what the scratchpad paints with is
  what the engine gets on the real canvas.

**② THE RAIL'S PRESET LIST CARRIES EACH BRUSH'S OWN STROKE.** *"Presets have no previews unlike the
dock."* A name is not what a brush looks like — the finding §8.2's Cards mode was built on — and a
browse list inside the *editor* had no business restating a question the dock already answered. Same
renderer (`core/brush/stroke_preview`), same rules, one level down: a 46 px row is a name line over a
26 px strip, **rendered at the next 32 px bucket up and drawn CROPPED, never scaled**, with the
diameter ceiling at the card's own ratio (12 px of nib in a 26 px strip — a ceiling near the strip's
height flattens the S-curve into a fat sausage). Params are built **once per preset** (it mints the
tip's raster id) and strips are rendered **lazily, only for rows that actually paint** — four of them,
not 114. ⚠ **The cache is dropped WHOLE** on a save, an import, a delete or a re-theme: every key
names an index that just moved, or a paper that just changed. A test paints the list twice and demands
**zero** re-renders on the second — asserting on an **EVENT** count, because a cache SIZE cannot
witness a re-render.

**③ THE USER BADGE.** *"User presets have no badge to indicate they are user presets."* See §8.2:
opposite corner, accent ink, a **ring** rather than a filled dot — distinct on all three axes, because
colour alone would have made it the fidelity badge in another mood. It appears on the dock's cells and
on the rail's rows, and it says so in words in the tooltip.

**④ ⭐ THE SCRATCHPAD PAINTS IN THE PREVIEW'S INK, NOT THE APP'S FOREGROUND — the ruling.** *"Scratchpad
takes foreground color rather than the one that the brush stroke is drawn with."* The two halves of
this surface share ONE paper, so they must share one ink. Three reasons, and the third is the one that
settles it:

1. **The paper follows the theme** (§8.2). In the dark theme it is the panel's own ground — a
   foreground of `#1a1a1a` paints an invisible stroke on it, and a foreground that happens to equal
   `panelBg` paints nothing at all.
2. **An eraser's paper is a slab of the ink itself** (§8.2), because what shows through the hole it
   bites is the dock. Painting that slab in some unrelated colour makes the carve and the paint two
   different pictures of two different brushes.
3. ⚠⚠ **The fallback swaps BOTH paper and ink.** Five shipped presets cannot mark white with black,
   and come back on a mid-grey paper under a saturated red. A scratchpad laying the app's foreground
   over *that* contradicts the picture beneath it in precisely the cases the fallback exists for. So
   `renderStrokePreview` grew a **resolved** form — `renderStrokePreviewResolved`, which reports the
   pair it actually **landed on** — and the surface paints in *that*. (The old entry point is a thin
   wrapper over it; there is still exactly one renderer.)

The editor never touches the document, so *"which colour will I paint with"* is not the question this
surface answers. *"What mark does this brush make"* is. `BrushEditorHost::foreground` survives unread,
for a future "preview in my paint colour" control.

**⑤ DELETE, WITH A CONFIRMATION, AND ONLY THE USER'S OWN.** ⚠ **TWO refusals, not one.** A shipped
preset is read-only for the reason `Save` never overwrites one. But **a preset inside a
user-installed `.bundle` is refused too**: it has no file of its own, and the only thing "delete"
could mean there is *"remove the archive and the other 40 presets in it"*, which is not what the
button says. `deleteRefusal` is the sentence for whichever applied — "the button is greyed out" is not
an answer to "why can I not delete this".

- ⚠ **THE FILE GOES FIRST**, and a failure there fails the whole operation: dropping the entry and
  leaving the file would resurrect the preset on the next scan — a delete that un-deletes itself.
- ⚠⚠ **A SELECTION POINTING AT THE DELETED BRUSH DROPS ITS PARAMS, NOT JUST ITS INDEX.** `rebuildAll`
  already re-pointed by name; until a preset could be deleted a name never *left* the corpus, so the
  index alone was enough. Now it can, and an index reading −1 while `activeParams()` still handed out
  the deleted preset's tip is a tool painting with a brush that does not exist. Name, index and params
  are cleared together.
- The confirmation is `AskOrTellDialog` — a **second modal over a modal**, which it is built for: its
  own top-level window (no sub-window promotion trap), `set_modal()` of its own, and a `run()` that
  pumps `Fl::wait()` so the app keeps painting behind it. *(This is also the shape the owed
  dirty-close confirm will take.)*
- After a delete the store, the dock and the active tool all end up on something valid: the preset
  that took the deleted one's place, or `Default round`. With nothing left to edit, the editor closes.

**⑥ EXPORT, AND WHY IMPORT DID NOT GLOB.** Export writes the native `.mbp` and **nothing else** —
§8.3 ② still stands, and export is not a `.kpp` writer in disguise. `writePresetFile` is **one
writer with two callers** (the user-directory save and the export), because two serializers would
drift and an export that did not produce the bytes the app reads back is a bug waiting.

⚠ **The brace list was NOT the bug, and this is worth writing down because the obvious fix is wrong.**
`*.{mbp,kpp,bundle}` is understood by FLTK's own `fl_filename_match`, and FLTK's kdialog/zenity
drivers *expand* it (`"Name (*.a *.b)"`). **Which backend answers** is the bug:
`platform::initNativeFileDialog()` turns FLTK's **kdialog** driver on process-wide when running KDE —
which is the user's session — and that driver hands kdialog its filter as newline-separated Qt-style
entries, a syntax kdialog does not read. Nothing matched.

So the editor stopped using a bare `Fl_Native_File_Chooser` at all and went through
**`platform::showOpenDialog` / `showSaveDialog`**, the seam File▸Open and Export already use: the XDG
portal first, Mosaic's **own** kdialog command second (`<globs>|<label>`, the syntax kdialog actually
reads), the native chooser only as a last resort. It also takes globs as a **list**, so the three
extensions are three globs rather than one pattern a backend has to parse. *(Two other dialogs in the
tree still call `Fl_Native_File_Chooser` directly for `.icc` — same latent bug, not this session's
files.)*

**⑦ USER PRESETS FIRST IN THE DOCK.** See §8.2's own section — the ordering, the prefix-range
`isUserPreset`, the two index inversions, and the untouched index −1.

*Owed: the visual pass on all of it (⚠ a `SettingsDialog`-class problem — an unshown `Fl_Window`
renders BLACK to an `Fl_Image_Surface`, so this dialog cannot be shot headlessly and only a human
running the app can judge it), the dirty-close confirm, and a `.kpp` WRITER if third-party export ever
becomes a goal. ⭐ And one thing only a human with a TABLET can confirm: that the scratchpad's
pressure, tilt and rotation actually arrive over this window on their session — the wiring is the same
one Settings → Tablet's test area proves out, but the proof is a pen on glass.*

### 8.4 Eraser
Built here (§2.3): `StrokeMode::Erase` in the engine; `eraserToolActive()` added to
`strokeToolActive()`.
- `Settings::eraserSizeFollowsBrush` — default **true**.
- `Settings::eraserPresetFollowsBrush` — default **false**; the eraser keeps its own preset,
  defaulting to a hard circle.

Both are genuine user preferences (Photoshop ties size; Krita does not), so neither trips the
no-toggle-for-strictly-better rule.

*(Built 2026-07-10. What building it settled: the size tie lives in `ToolManager` — the mirrored
pair is synced inside `notifyOptionsChanged()`, so every options surface and future edit path is
covered and the bar can never show a size a stroke won't use; the pair is seeded from the Brush at
construction too, because the registered defaults differ (24 vs 40) and a tie that starts split is
a lie. The Eraser bar has no Flow, so its Opacity alone caps the carve — matching the engine's
erase ceiling, which never reads a colour's alpha. The preset tie is a Settings field only; there
is no UI to show until presets exist.)*

**⚠ THE ERASER'S PRESET NOW EXISTS AND IS REAL** *(2026-07-12, 16th session — the user's ruling)*.
The dock's grid is **no longer Brush-only**: it serves the Eraser too, showing the eraser corpus and
nothing else (§8.2). So the eraser's preset is **not** browsed inside the editor, as the paragraph
above once planned — it is browsed exactly where the brush's is, and the editor is not on its critical
path any more.

- `BrushPresetStore` holds **two selections**, `select()` and `selectEraser()`; the pick is routed by
  the **corpus the grid was showing**, not by the active tool — the corpus is what the user was
  looking at when they clicked, and it cannot be out of step with the cell they hit.
- `Settings::eraserPreset`, by name, alongside `Settings::brushPreset`.
- `VulkanCanvas::currentBrushParams` takes the eraser's preset (tip, options, spacing) and *still*
  sets `StrokeMode::Erase` — an eraser carves whatever nib it is holding. A **null tip** (no preset)
  keeps `flow = 1.0`, so the pre-existing behaviour is bit-for-bit what it was.
- `eraserPresetFollowsBrush` **remains schema-only, and is now nearly moot**: the two corpora do not
  overlap, so "follow the brush's preset" would have to mean "follow it to a preset the Eraser is not
  offered". Whatever it comes to mean, it is no longer the thing standing between the eraser and a
  preset. ⭐ Still owed: the eraser's own visual pass.

*(**The Tools→Eraser checkbox is BUILT** — 2026-07-29, `SettingsHost::setEraserPresetFollowsBrush`
beside the size tie, with the caption saying out loud that the two corpora do not overlap so the
setting can at most point the eraser at a nib its own picker never offers. It is carried because it
is a genuine preference — Photoshop ties the pair, Krita does not — and because the schema has held
it since §8.4 landed, so a control that reads it is better than a field nothing shows. The **same two
ties also sit in the brush editor's footer** (§8.3), which is where a user is actually thinking about
the pair.)*

---

## 9. Settings

**`Tools → Brush` sub-tab** (Tools already has a `SubTabBar`) — ***BUILT 2026-07-12**, carrying the
**preset-dock display chooser**: two `OptionCard`s, **Grid | Cards**, Cards the default (§8.2);
persisted by NAME as `Settings::brushPresetDisplay`, so an unrecognised value — including the `""` a
settings file written before the field existed reads back as — lands on the default rather than on
whatever an enum's zero happens to be.* *There is no brush-outline toggle* — the reticle always traces
the tip (§6.3).

**The two eraser ties are BUILT** *(2026-07-29)*, both on **`Tools → Eraser`** rather than split
across two sub-tabs: they are one pair and they belong on one page. `eraserSizeFollowsBrush` had been
there since §8.4; `eraserPresetFollowsBrush` joined it (§8.4's as-built note). Both are mirrored in
the brush editor's footer (§8.3). Still owed here: cursor style; "temporarily keep tweaks per
preset".

The new **Tablet** category is specified in `docs/tablet.md` §8.

---

## 10. Build sequence

- **Arc A — engine, no UI.** curves + sensors → mask generators (pure `coverageAt`) → bitmap tips +
  cache → accumulator rework (`Uniform`/`Colored`, `Wash`/`Buildup`) + erase + blend modes → masking
  brush → spacing/distance/stroke state → golden images.
- **Arc B — formats.** PNG chunk walker → XML → `.kpp` reader + mapper + fidelity → gbr/gih/abr →
  bundle → `dataDir()` + library → default set + credits.
- **Arc C — input.** `docs/tablet.md` → platform tablet API + XI2 + ring drain → **Wayland spike** →
  dynamics wiring → smoothing/stabilizer → Settings → Tablet.
- **Arc D — UI.** curve editor → preset chip → dock section (Brush-only) → modal editor shell → option
  pages → paintable preview surface → reticle shapes → eraser ties.
  *(2026-07-29: everything on this line is built except the **preset chip** — the editor shell, the
  option pages, the paintable preview surface and both eraser ties landed together, §8.3. The chip is
  the last piece of Arc D, and §8.3's editor is reachable without it.)*

A and B are fully headless-testable and can land before any UI exists, keeping the tree green. C's
Wayland spike should run early — it is the riskiest unknown. D depends on A.

---

## 11. Risks

1. **Wayland tablet routing** (`docs/tablet.md` §4) — FLTK hides its `wl_seat`; the Vulkan subsurface
   may or may not receive tablet events. Spike before designing around it.
2. **Stroke latency.** Big-doc painting is already ~53 ms/frame at 1920×1080 (`PLAN.md` §S19-a), and
   that is S60's dirty-tile debt, not the brush's. Richer dabs will make it worse. Measure before and
   after with the `Uniform` fast path so this arc is not blamed for pre-existing debt.
3. **`Colored` accumulation memory** — coverage float *plus* RGBA float over the working rect. The
   bounded rect keeps it sane; still, cap it.
4. **Two net-new UI surfaces with no precedent** — the curve editor and the dock split.
5. **Default-set size** (~17 MB) forces the runtime data dir; it cannot ride `EmbedAssets`.
