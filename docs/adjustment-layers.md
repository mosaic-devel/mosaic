# Adjustment layers — the non-destructive filter framework (S32) + the colour filters (S34)

S32 turns the long-standing `AdjustmentLayer` skeleton (S6 model, S7-a scoping proof) into the
real non-destructive filter framework: a **typed parameter system** in core, **real math for
every scalar kind**, a **Filter-menu insert flow**, a **live schema-generated editor**, and the
dock affordances that make adjustment layers first-class citizens.

**S34 (2026-07-29) closes the colour slice**: **Curves** end to end (storage, per-channel LUT
math, the plot editor — the one kind S32 deliberately left unimplemented), plus four new kinds —
**Shadows/Highlights**, **Defringe**, **Matte Removal** and **Haze Removal**. `adjustmentImplemented`
now answers true for every kind. **Frequency Separation is deferred** (§8): it is a *workflow*
(one command splitting a layer into a blurred low-frequency layer and a high-frequency
difference layer), not an adjustment kind, and it belongs with the retouching session rather than
riding along on the Curves slice.

**S34-a (2026-07-29) is the cheap, high-value remainder of the galleries**: the **backdrop
histogram behind the Curves plot** that S34 left owed (§5, and per channel, not just luma), plus
four kinds — **Gradient Map** (§2.6), **Vibrance** (§2.7), **Photo Filter** (§2.8) and **High
Pass** (§2.9, which lands in the S35 stylize module because it *is* the unsharp mask's Gaussian
difference). Nothing here invents a mechanism: three of the four are per-pixel transfers on the
existing scalar switch, Gradient Map reuses `core::vec::Gradient` and its one ramp evaluator, and
High Pass reuses `stylize_kernels.cpp`'s Gaussian. The standing constraints are in §7.2.

## 1. The typed parameter system (`core/adjustments.{hpp,cpp}`)

Storage is unchanged: `AdjustmentLayer` carries a `name -> double` params bag, which is what
docio round-trips. What S32 adds is a **declared schema** over that bag — per kind, a table of
`AdjustmentParamDesc { key, label, min, max, def, step, suffix, type }` (type = `Scalar`,
`Toggle`, or `Choice` — a Choice row carries its option labels and stores the option INDEX as a
double, so docio needs nothing new). The schema is the single source of truth for three
consumers:

- **The compositor** reads through it (`adjustmentParamValue`): an absent key falls back to the
  declared default, a present one clamps to the declared range — a hostile `.mosaic` file can
  never feed the math garbage.
- **The Filter-menu insert** seeds a fresh layer from it (`seedAdjustmentDefaults`).
- **The editor** generates its controls from it — a `ScrubSlider` per scalar, a `CheckBox` per
  toggle. Adding a parameter to a kind is one table row, not four call sites.

`adjustmentImplemented(kind)` answers whether the compositor has real math for a kind; the
Filter menu offers only implemented kinds (a menu item that inserts a do-nothing layer is a
broken promise). Since S34 every kind answers true. PhotometricMatch (the S55 sky-harmonization
grade) is implemented and schema-covered — so the grade the sky estimator lands is hand-tunable
from the dock — but it is created by the sky flow, not the menu; its measurement-derived keys
(`mu_log`, the night tint) deliberately stay off the schema.

**Identity-at-defaults rule:** every schema's defaults are a mathematical no-op unless the kind
is inherently visible (Threshold cuts at mid-gray, Posterize quantizes). The compositor keys an
**early-out** off this: a defaults (or empty) bag returns before touching a pixel, so inserting
a fresh adjustment layer composites **byte-identically** to no layer at all — pinned by test.

## 2. The math (`render/compositor.cpp` `applyAdjustment`)

All kinds operate on the accumulated backdrop in the encoded working space, except Exposure
(photographic exposure is physical, so it works in linear light through the compositor's
existing sRGB LUT pair — the PhotometricMatch precedent). Alpha is never touched: an adjustment
recolors the backdrop, it does not add coverage. Opacity, layer masks (folded on the backdrop
grid, transform-free) and clip-to-below gate the effect per pixel exactly as they did for the
S7-a kinds.

| Kind | Parameters | Transfer |
|---|---|---|
| Brightness/Contrast | brightness, contrast (−1..1) | unchanged S7-a formula (byte-compat) |
| Levels | in_black, in_white, gamma, out_black, out_white | input-window remap → `pow(t, 1/gamma)` → output remap, per channel |
| Exposure | exposure (EV), offset, gamma | linear light: `v·2^EV + offset`, then `pow(v, 1/gamma)`, re-encode |
| Hue/Saturation | hue (°), saturation, lightness (±100) | hexcone HSL: rotate h, scale s; lightness fades toward white/black (PS-style) |
| Color Balance | 3 bands × cyan-red/magenta-green/yellow-blue (±100), preserve_luminosity | smooth partition of unity over the tonal axis (shadows fade out by mid-gray, highlights fade in from it); per-band channel deltas at 0.4 strength; optional W3C `setLum` restore |
| Threshold | level | W3C luma ≥ level → white else black |
| Posterize | levels (2..32) | `round(v·(n−1))/(n−1)` per channel |
| Grayscale | method (Choice), strength (%), grays (2..256) | the chosen projection, presented in the editor as three grouped families (luminance projections / colour filters & channel reads / halftone & threshold, separated by dividers) — **Luminance (linear)** (zero the chroma, keep the Rec 709 linear-light luminance; renamed from "No chrominance"), **Luma (gamma-weighted)** (default: the 0.30/0.59/0.11 pre-S32 formula on the encoded values, byte-compat; renamed from "Luma (classic)"), a Red/Green/Blue **filter** (the photographer's mono reads; the first cut's Average/Lightness/Value were dropped as near-indistinguishable, user 2026-07-17), **Max channel** (per-pixel `max(R,G,B)`), **Min channel** (per-pixel `min(R,G,B)`, the dark low-key complement of Max channel), **Dithered** (Luma → 1-bit Floyd–Steinberg error diffusion), **Adaptive threshold** (binarize against a local-window mean − a small bias) — optionally quantized onto an N-gray **palette** ("show this image with 3 grays"; 256 = continuous = byte-identical off), mixed into the original by strength. **The grouped/renamed DISPLAY order is editor-only** (`AdjustmentParamDesc::choiceOrder` / `choiceDivider`): the bag still stores the `GrayscaleMethod` enum INDEX unchanged (Min channel stays index 8, appended so the earlier indices are byte-stable), so regrouping the list never rewrites a saved document and Luma stays the default |
| Invert | — | unchanged S7-a formula |
| Photometric Match | S55 §6.2 keys | unchanged (docs/research-sky-estimate-from-layer.md) |
| Curves (S34) | channel (Choice) + four stored curves | per-channel curve then the composite curve, composed into one 256-entry LUT per channel (§2.1) |
| Shadows/Highlights (S34) | shadows, shadows_tone, highlights, highlights_tone, radius | SPATIAL: a blurred local-background luma mask drives a per-pixel exponent (§2.2) |
| Defringe (S34) | purple, green, threshold, ca_red, ca_blue, center_x/y | SPATIAL: radial per-channel rescale (lateral CA) + hue-band chroma suppression (§2.3) |
| Matte Removal (S34) | mode (Choice) | compositing algebra against a known matte / the pixel's own alpha (§2.4) |
| Haze Removal (S34) | amount, airlight, tint, saturation | airlight unmixing at a CONSTANT transmission (§2.5) |
| Gradient Map (S34-a) | reverse (Toggle) + one stored ramp | luma indexes a 256-entry ramp LUT; the stop ALPHA is the per-tone strength (§2.6) |
| Vibrance (S34-a) | vibrance (±100) | `s' = s·(1 + k·(1 − s))` in hexcone HSL — the gain fades out as chroma rises (§2.7) |
| Photo Filter (S34-a) | filter (Choice), density, preserve_luminosity, color_r/g/b | LINEAR light: multiply by the filter colour, mix by density, optionally restore the source luminance (§2.8) |
| High Pass (S34-a) | radius | SPATIAL: `½ + (p − G_σ(p))`, σ = radius/2 — the unsharp difference on its own (§2.9) |

BrightnessContrast and PhotometricMatch keep their original **raw** `param()` reads (their
behavior predates the schema and stays byte-identical — PhotometricMatch's estimator can write
values outside the editor's slider ranges, and the compositor must honor them); the schema read
with its clamping applies to the six new kinds.

**Grayscale's two SPATIAL methods (Dithered / Adaptive threshold).** Most Grayscale methods are
per-pixel, but two decide each output pixel from its neighbours, so `adjustmentIsSpatial` gained a
**per-instance** overload (`adjustmentIsSpatial(const AdjustmentLayer&)`) that consults the chosen
method — a spatiality that depends on a *parameter*, not just the kind. The compositor routes these
through a whole-buffer pass (`applyGrayscaleSpatial`) instead of the per-pixel color loop; the five
older methods (and Max channel) stay per-pixel and byte-identical, and a Luma-default / empty bag
renders exactly as before. The gray SOURCE for both is the classic Luma projection, clamped to
`[0,1]` (both are 1-bit decisions). Fixed defaults, no schema knobs (kept off the table so the
editor stays a clean three rows): adaptive uses a 12 px local-mean window (scaled by the placement)
minus a 0.03 (~8/255) bias; Dithered is always 1-bit and ignores the Grays palette.

- **Adaptive threshold** has a finite window reach (`blurAdjustmentReach`) and reads with
  clamp-to-edge, so the S60-a dirty-rect path stays **byte-identical to the full composite**
  (region == crop(full), the blur family's money invariant, test-pinned).
- **Floyd–Steinberg dither** diffuses quantization error serially from the raster's **top-left
  across the whole image** — an unbounded up/left dependency the finite-reach machinery cannot
  express (its correct region would be "all rows 0..y, full width", not a symmetric radius, and a
  whole-image reach is unsafe on the unclamped off-canvas preview path). It therefore reports
  **zero reach** and diffuses over whatever contiguous buffer it is handed. **The full composite —
  every saved/exported image and every ordinary redraw — is always the exact FS result.** The one
  documented limitation: an incremental *sub-region* repaint under a dither layer (e.g. a brush
  stroke on a layer below it) re-seeds the diffusion within that rect, so the dither *pattern*
  there can differ from the full composite until the next full recomposite. The kernel is the exact
  textbook FS (7/16 right, 3/16 down-left, 5/16 down, 1/16 down-right; edge error dropped).

### 2.1 Curves (S34) — the kind S32 left open

**Storage decision.** A curve is not a double, and the params bag is `std::map<std::string,
double>`. Two designs were on the table: (a) extend the layer model with a second, string-valued
bag holding `core::brush::Curve::toString()`'s `"x,y;x,y;"` spelling, or (b) keep the one bag and
store the knots as **indexed doubles**. **(b) shipped.** The reasoning:

- Every consumer of the bag then needed **zero** new code. `SetAdjustmentParamsCommand` still
  stores the whole old/new bag, so undo, redo and per-control coalescing work unchanged; the
  editor funnel (`applyAdjustmentField`) still hands out a `map<string,double>&` to mutate;
  `.mosaic` still writes `{"params": {...}}` as a JSON number map and reads it back. A second bag
  would have had to be threaded through the command, the funnel signature, docio's writer and
  reader, and the loader's validation — four files owned by three different concerns, for a
  storage format that is strictly *less* precise.
- **Round-trip precision is better, not worse.** A JSON number round-trips its double exactly
  (the serializer prints the shortest round-trippable form); `Curve::toString()` is
  six-significant-digit `%g` — the right choice for the brush-preset *interchange* format it was
  built for, but a lossy one for our own document.
- Nothing was invented: the curve **type** is still `core::brush::Curve` — the same natural cubic
  spline with corner knots the brush dynamics and `ui::CurveEditor` already speak, so the editor
  edits the stored object itself and the spline maths has exactly one implementation.

The keys (per channel, `<prefix>` = `curve_rgb` | `curve_r` | `curve_g` | `curve_b`):

| Key | Meaning |
|---|---|
| `<prefix>_n` | knot count (decode clamps to `[0, kMaxCurvePoints]` = 64) |
| `<prefix>_<i>_x`, `<prefix>_<i>_y` | knot *i*, clamped into the unit square on decode |
| `<prefix>_<i>_c` | `1.0` when knot *i* is a spline **corner**; **absent means smooth** |

**Absent IS the identity.** An identity curve writes no keys at all (`setAdjustmentCurve` erases
the channel and returns), so a freshly inserted Curves layer carries only its `channel` row, the
compositor's early-out fires, and it composites **byte-identically to no layer** — the §1 rule,
test-pinned. A document written before S34 has no curve keys and therefore loads as four identity
curves; the round trip of a document *with* curves is byte-exact because the encoder is
deterministic (same keys, same order, corner flags only where true) and the decoder is its
inverse. A hostile file cannot hurt the math: a non-finite or missing coordinate drops that knot,
the rest clamp into `[0,1]`, and fewer than two surviving knots reads as the identity (a curve
that cannot span `[0,1]` is not a tone curve — "a corrupt file silently flattens the image" is
nobody's idea of graceful).

**Channels.** A **composite** curve applied to all three channels plus one curve **per channel** —
the shape every editor has shipped since Photoshop 4. Composition order: the per-channel curve
runs **first**, then the composite curve on its result.

**Compositor maths.** `curvesConsts` composes each pair into a **256-entry LUT once per
composite** (never a spline evaluation per pixel); the pixel loop samples it linearly between
lattice points. A channel whose pair is the identity is marked **inactive** and passed through
**verbatim** rather than through a nominally-identity lookup — a lerp between two float lattice
values is not bit-exact, so editing only the red curve must leave green and blue byte-identical,
and it does. Values outside `[0,1]` clamp in: Curves' domain *is* the unit square the user drew
in, the same way Levels inherently clamps its input window.

**The `channel` row is a real, stored parameter** (a Choice), not editor state. It costs one
number, it makes the schema non-empty (an empty schema is how the corner-panel arbiter decides a
kind has no editor), and "the channel I was working on" survives save/load and rides undo like
every other row. It is the only schema row Curves has: the knots are deliberately **not** schema
rows, which is why **Reset erases them** instead of re-seeding a default.

**Editor.** A `ui::CurveEditor` plot (the existing widget — drag/add/right-click-remove/
double-click-corner, editing a `core::brush::Curve` directly) under a hand-built channel picker.
The picker is hand-built rather than schema-generated because switching channel must **re-seed the
plot in the same callback**; a generic Choice row only writes the bag. Edits stream through the
ordinary funnel with **one undo step per gesture**: the coalesce id carries a gesture counter that
bumps on every event that is not `FL_DRAG`, so a point drag is one step but two drags never merge.
`syncValues` re-seeds the plot from the bag, so undo/redo/Reset move the curve on screen —
`setCurve` deliberately does not fire the change callback, so that can never loop back into the
funnel. **S34-a added the owed backdrop histogram behind the plot** — and made it per channel; see
§5.

### 2.2 Shadows/Highlights (S34) — SPATIAL

Local tone recovery: an inverted, low-passed, monochrome copy of the backdrop drives a **per-pixel
exponent**. This is the classical **non-linear masking** form (N. Moroney, *Local Color Correction
Using Non-Linear Masking*, IS&T/SID CIC 8, 2000 — §7). Reading the *local
background* luminance rather than the pixel's own is exactly what separates it from a global
gamma: the same mid-dark pixel is lifted inside a shadow and left alone against a bright sky.

`e = (1 + 2·highlights·w_h) / (1 + 2·shadows·w_s)`, applied per channel as `pow(v, e)`; `w_s` and
`w_h` are smoothstep weights of the **mask** over the tonal ranges the two `_tone` sliders set.
At a full slider with the mask fully in-band the exponent reaches 1/3 (a strong lift) or 3 (a
strong recovery). A pixel whose weights are both zero yields `e == 1` exactly and is **skipped**,
so the untouched parts of the frame stay byte-identical without relying on `pow(x, 1)`.

The mask is the encoded luma blurred by `fx::boxBlurApprox` (three box passes, Kovesi 2010) at
σ = `radius`/3, in **buffer** px through the walk's placement scale, so a ≤96 px scope preview
masks proportionally. Reads clamp to the edge and `blurAdjustmentReach` reports `radius + 8`
(0 when both amounts are 0), so **region == crop(full) stays byte-exact** — the blur family's
invariant, test-pinned here too.

### 2.3 Defringe (S34) — SPATIAL

Two independent repairs for the same complaint, both identity at defaults:

1. **Lateral chromatic aberration** — a **radial rescale of the red and blue channels** about an
   optical centre (green is the reference and never moves), `ca_red`/`ca_blue` in percent of the
   pixel's distance from that centre. Bilinear taps read **premultiplied** colour and divide by
   the interpolated alpha, so a transparent neighbour cannot bleed its undefined RGB in; edges
   clamp. `center_x/center_y` are parent-space px like the blur family's centres, which also earns
   them the Filter-menu insert's document-centre seeding for free.
2. **Axial (purple/green) fringing** — the halo is a **hue**, so it is suppressed as one: pixels
   whose hue falls in the purple (~285°) or green (120°) band, and whose chroma clears
   `threshold`, are desaturated toward their own lightness. `w == 0` skips the HSL round trip
   entirely, so an unaffected pixel is byte-identical.

**There is deliberately no edge term anywhere in this kind** — no "distance to a high-contrast
edge", no detection score, no multi-resolution pyramid. That is a standing constraint (D-2, §7), and
it also happens to make the tool honest: a fringe *is* a colour, and gating on hue is what a user can
reason about. `blurAdjustmentReach` bounds the CA displacement by the domain's farthest corner
from the centre (the spin/zoom shape), and returns 0 when both CA sliders are 0.

### 2.4 Matte Removal (S34)

Pure compositing algebra (Porter & Duff 1984; Smith & Blinn 1996) on the pixel's **own alpha**,
per pixel, alpha never touched:

| Mode | Transfer |
|---|---|
| Remove white matte | `C = (Cm − (1 − a)) / a` — undo a composite over white |
| Remove black matte | `C = Cm / a` |
| Unpremultiply (divide by alpha) | `C = C / a` — the same algebra as the row above, under the name the compositing trade uses for it; both ship because both are what someone reaches for |
| Premultiply (multiply by alpha) | `C = C · a` — bake the coverage into the colour, the fix for content stored the other way round |

An "inherently visible" kind in the Threshold/Posterize sense (no identity-at-defaults early-out):
every mode does real work wherever there is partial coverage below. Over a fully opaque backdrop
all four *are* the identity, which is the honest answer — there is no matte to remove. Fully
transparent pixels are left alone in the three dividing modes: there is no colour to recover.

### 2.5 Haze Removal (S34)

Koschmieder's 1924 atmospheric-scattering model inverted about a user-set airlight colour, at a
**constant** transmission: `J = A + (I − A)/t`, `t = 1 − 0.9·amount` (so a full slider leaves
t = 0.1 — a strong stretch that never divides by zero). `airlight` sets the assumed brightness of
the haze, `tint` swings its colour warm (+) or blue (−) around neutral, `saturation` restores the
chroma the atmosphere ate. Per pixel, per channel; nothing is estimated from the image.

**This is the deliberately narrowed formulation, not the one the literature is famous for.** The
dark-channel prior and every other *transmission-estimation* scheme are excluded by design (H-1,
§7); what is
left — inverting the scattering model at a user-chosen density — is a per-channel affine map about
the airlight point, which is a linear contrast stretch wearing a physical hat. It reads as haze
removal because the airlight *direction* is what it works along, which is the part Levels cannot
express.

### 2.6 Gradient Map (S34-a)

The backdrop's **luma** (the W3C 0.30/0.59/0.11 read, the same one Threshold and Grayscale-Luma
use) indexes a user-authored colour ramp, and the sampled colour replaces the pixel. Photoshop 4
(1996) shipped it; §7.2 has the standing constraints.

**Storage decision — the Curves precedent, verbatim.** A gradient is not a double either, and the
answer is the same one §2.1 argued: **indexed stops in the existing `map<string,double>` bag**, so
`SetAdjustmentParamsCommand`, the editor funnel, docio's writer/reader and the loader's validation
all needed **zero** new code, and the `.mosaic` round trip is byte-exact because the encoder is
deterministic and the decoder is its inverse (pinned by test). Keys:

| Key | Meaning |
|---|---|
| `gm_n` | stop count (decode clamps to `[0, kMaxGradientMapStops]` = 32) |
| `gm_<i>_t` | stop *i*'s offset along the ramp, clamped into `[0,1]` |
| `gm_<i>_r`, `_g`, `_b`, `_a` | its colour, each clamped into `[0,1]` |
| `gm_<i>_m` | its blend **midpoint**; **absent means 0.5** (the straight linear blend) |

**Only the stops are stored.** A gradient map has no geometry, so the `vec::Gradient`'s
type/transform/spread are fixed (Linear, identity, Pad) and never written — and **dither is fixed
at `None` on purpose**: `ditherOffsetLsb` is keyed to the *destination* pixel, so enabling it here
would make a dirty-rect recomposite disagree with the full composite, and region == crop(full) is
not negotiable. The ramp **type** is still `core::vec::Gradient`, so `ui::GradientFlyout` edits the
stored object itself and the ramp maths (midpoints included) has exactly one implementation:
`sampleStops` in `core/vector/raster.cpp`, reached through `vec::sampleAt`.

**Absent is the DEFAULT ramp, not an identity.** Unlike Curves, this kind has no identity setting
to spell — mapping luma through black→white is a real (and useful) grayscale conversion — so
Gradient Map is **inherently visible**, the Threshold/Posterize/Matte Removal class, and there is
no identity early-out. Writing the default ramp *erases* the keys, so a freshly inserted layer
carries only its `reverse` row, a pre-S34-a document loads as the classic black-to-white map, and
**Reset erases the stops** exactly the way it erases Curves' knots. A hostile file cannot hurt the
maths: a stop with a missing or non-finite field is dropped, the rest clamp, the list is sorted
(`sampleStops` requires order), and fewer than two survivors reads as the default ramp.

**Compositor maths.** `gradientMapConsts` builds **one 256-entry RGBA lookup per composite** (never
a ramp evaluation per pixel), with `reverse` baked into the table rather than flipped per pixel;
the loop samples it linearly between lattice points, `curveSample`'s twin. The stop **alpha is the
per-tone strength**: `out = lerp(backdrop, rampRGB, rampA)`, so a stop at α = 0 leaves its tone
**byte-identical** and one ramp can recolour the shadows while letting the highlights through. The
pixel's own alpha is never written — an adjustment recolours the backdrop, it adds no coverage.

### 2.7 Vibrance (S34-a)

Saturation weighted by how saturated the pixel **already** is, in the same hexcone HSL
Hue/Saturation uses:

```
s' = clamp01( s · (1 + k·(1 − s)) )        k = vibrance/100 ∈ [−1, 1]
```

At `s = 0` a neutral has no chroma to scale and comes out byte-identical; at `s = 1` the weight
`(1 − s)` is exactly zero, so `s' == s`, **the HSL round trip is skipped outright** and an
already-vivid colour is byte-identical too — that is what "protects vivid colour" means here, and
it is exact rather than approximate (test-pinned). In between, a full positive slider nearly
doubles a small saturation and a full negative one squares it (`s' = s²`), which collapses the
muted end first. Identity at the default (0), the §1 rule: which way a photograph needs to go is
its own business.

**One row on purpose.** The *linear* saturation scale already ships on Hue/Saturation; what makes
this kind worth having is precisely that its scale is not linear. There is deliberately **no
hue-band "skin tone" protection** — see constraint V-1 in §7.2.

### 2.8 Photo Filter (S34-a)

A coloured gel over the lens **absorbs**, so the transfer is a multiply in **linear light** (the
Exposure/Vignette precedent), mixed in by density and optionally re-normalised to the backdrop's
own luminance:

```
lin      = srgbToLinear(c)
filtered = lerp(lin, lin · filterLinear, density)
if preserve_luminosity:  filtered ·= linearLum(lin) / linearLum(filtered)
out      = srgbToEncoded(filtered)
```

At density 100 % with the luminance *not* restored, white light comes out as **exactly the
filter's own colour** — the physical statement the maths makes, and the sharpest available pin
(test). `preserve_luminosity` (on by default, as everywhere) is what stops a dense gel from simply
darkening the frame; a black pixel has no luminance to restore and is left where the filter put it
rather than scaled by 1/0.

Fourteen named presets — the standard photographic filter designations, grouped warming | cooling
| colour filters | Sepia & Underwater — plus **Custom**, whose colour lives in three
schema-declared `color_r/g/b` rows *stored in 8-bit levels*. Those three rows are schema-declared
so the clamp/seed/Reset machinery covers them, but the editor **owns** them and shows one swatch
instead of three sliders (§5).

**Deliberately visible at defaults** (Warming (85) at 25 % density), the S33/S35 deviation: that is
what every editor opens this control on, and a menu item that inserts a do-nothing layer is the
broken promise `adjustmentImplemented` exists to prevent. The early-out keys off the **effective**
parameter — `density == 0` — never off default-equality.

### 2.9 High Pass (S34-a) — SPATIAL, and it lives in the S35 module

`out = ½ + (p − G_σ(p))` per channel, σ = **radius/2** — the blur family's reading of "radius", so
a High Pass radius means what a Gaussian Blur radius and an Unsharp radius mean. This is literally
the unsharp mask's own difference term drawn without the add-back, which is why it ships in
`render/stylize_kernels.cpp` next to `unsharpMaskImage` and routes through
`render::isStylizeKind` rather than growing a second Gaussian in `compositor.cpp`. It is the
missing half of frequency separation: the low band is a Gaussian Blur layer, this is what is left
over, and mid-grey is the zero of the difference — which is exactly why the result composites
through Overlay / Soft Light (both leave 0.5 alone).

The difference is taken on **premultiplied** planes (a transparent neighbour must not bleed its
arbitrary RGB in) and un-premultiplied *before* the ½ bias is added, so the bias is a
straight-space constant rather than something divided by coverage; a pixel under (near) zero
coverage keeps the RGB it came in with. The result is clamped to `[0,1]` — a high pass is a
display-space read and HDR headroom has no meaning around a fixed mid-grey. **Alpha untouched.**
Reads clamp to the edge and `stylizeAdjustmentReach` reports `1.5 × radius` (= 3σ), so
**region == crop(full) is byte-exact** — test-pinned, like the rest of the family.

Deliberately visible at defaults (10 px), for the blur family's reason: a zero-radius high pass is
a flat grey field, not an identity.

## 3. Scoping (unchanged, recap)

Scoping is purely positional and lives in the compositor walk (docs/compositor.md): an
adjustment layer modifies the **accumulated backdrop** at its tree position, so inside a group
it affects **only the layers below it within that group**, and at the root it applies
**globally downward**. Reorder/toggle/mask/opacity all work through the ordinary layer
machinery because an adjustment *is* a layer.

## 4. Undo (`SetAdjustmentParamsCommand`)

One new command on the `SetTextureCommand` template: stores the whole old/new bag (small value
data), captures the old bag on first apply, coalesces on a non-zero `coalesceId`. The editor
streams one of these per control edit; `applyAdjustmentField` bumps the coalesce id whenever the
edited control changes, so a slider run reads as one History entry ("Edit Levels") and switching
sliders starts the next.

## 5. UI

- **Filter ▸ Adjustments ▸ …** inserts the chosen kind **above the active layer, inside its
  group** (`AddLayerCommand`, one undo step) and makes it active — which is what opens the
  editor. Menu labels escape literal slashes (`Brightness\/Contrast` — FLTK splits menu paths
  on `/`).
- **Automask to the selection.** With an active selection the new layer is **born masked to
  it**: `insertAdjustmentLayer` builds the mask with `core::maskFromSelection` (the same
  resampling Select ▸ Mask from Selection uses) and hands it to the layer *before* the
  `AddLayerCommand`, so the mask rides into that command and the whole thing stays **one History
  step** — undo takes the layer and its mask together, redo brings both back. A brand-new
  adjustment carries the identity transform, so the mask is the selection's coverage verbatim,
  feather and AA edge included. The gate is `Selection::anySelected()` alone, which is false
  both for "no selection" (a reveal-all mask would be a lie in the dock) and for an active
  selection of nothing (a layer that grades zero pixels reads as "the menu item did nothing").
  The selection is deliberately **left active** — the ants then read as "this is what got
  masked" — and the status bar says so. This applies to Filter ▸ Blur too: those are adjustment
  layers through the same entry point.
- **Pro controls** (user 2026-07-17: "we can do better than 9 sliders"): kinds whose parameters
  have a natural visual form get purpose-built controls instead of generic rows, all streaming
  through the same funnel with per-control coalesce keys. **Color Balance** shows three
  tone-band **`ui::ToneWheel`s** (Shadows/Midtones/Highlights — an AA hue disc, neutral centre,
  draggable puck, double-click recentres); the puck's 2D chroma offset maps onto the
  cyan-red/magenta-green/yellow-blue axes (120° apart in the chroma plane) via
  `core::colorBalanceToPlane/FromPlane`, with the achromatic MEAN preserved so a hand-authored
  equal shift survives a wheel drag. **Levels** shows the classic histogram with draggable
  black/gamma/white handles (the gamma handle sits at `t = 0.5^gamma`, the input that maps to
  half output) plus a black-to-white output ramp with its two handles; **Threshold** shows the
  histogram with the single cut handle. The histogram plots the adjustment's **backdrop**
  (`render::adjustmentBackdrop` — the scope composite *without* the adjustment's own step),
  refreshed when the panel changes target. **Hue/Saturation** keeps sliders but their tracks
  carry value ramps (`ScrubSlider::setTrackFill`): the spectrum under hue, gray-to-vivid under
  saturation, black-to-white under lightness. **Curves** (S34) shows the `ui::CurveEditor` plot
  under a channel picker and has no generic rows at all (§2.1). **Gradient Map** (S34-a) shows a
  `ui::PaintChip` ramp preview above its Reverse toggle, and **Photo Filter** (S34-a) shows one
  `ui::SwatchChip` under its preset picker instead of three colour sliders.
- **The Curves histogram** (S34-a, the item §2.1 owed). `ui::CurveEditor` gained
  `setHistogram(bins01, tint)` — 256 values already normalised by the caller — and draws them as
  bars under the grid, the identity diagonal and the curve, at ~38 % coverage so all three stay
  legible over a peak. The data path is the **existing** one: the panel's `HistoStrip` helper was
  generalised from `lumaHistogram` to `backdropHistograms`, which fills **four** 256-bin
  alpha-weighted, sqrt-normalised histograms — luma, then R/G/B, indexed by `core::CurveChannel` —
  in the *same single pass* over the same `render::adjustmentBackdrop` image the Levels/Threshold
  strips already asked for. That is what makes "show the channel's own distribution" as cheap as
  luma-only: the strips take `[0]`, the plot takes the channel its picker is on, and the bars are
  tinted (muted for composite, a desaturated R/G/B for the channels) so the tint says which
  distribution you are looking at without a legend. The set is refreshed when the panel changes
  **target** (a live drag cannot change the backdrop), and pushed into the plot only when the shown
  channel actually changes — the channel dropdown does it in its own callback so the plot never
  waits a frame. ⚠ **Constraint C-1 still binds**: this *draws* a distribution. Nothing may derive a
  curve from it.
- **Occlusion fade**: while the panel's rect overlaps the **visible document image** and the
  pointer is elsewhere, it drops to ~45% opacity so the graded pixels beneath stay visible;
  pointing at it (or a drag in flight on its controls) restores full opacity, and a panel
  sitting on the canvas apron never fades. Child sub-windows get no compositor alpha on X11, so
  the translucency is composited by hand: the host reconstructs what the canvas shows beneath
  the panel (apron color + the CPU composite through the view transform over the present
  shader's 8px screen-space checker), the panel renders its ground + children into an
  `Fl_Image_Surface` (children individually — an `Fl_Window` snapshots BLACK, the settings-dialog
  trap), blends, and caches the raster for draw() to blit. Two round-3 hard rules: the blend is
  built **outside draw()** (an `Fl_Image_Surface` created inside a draw() corrupts the active
  graphics context — the round-3 crash), and it is invalidated by a **fingerprint of everything
  it shows** — composite revision AND view zoom/pan/rotation AND the panel rect (pan/zoom/rotate
  never recomposite, so the composite revision alone left stale misaligned ghosts) — plus a
  content-dirty flag from the panel's own syncs. All image blits here and in the ToneWheel disc
  are **depth-3 RGB** — a depth-4 `fl_draw_image` is interpreted inconsistently across drawing
  surfaces (the round-3 magenta artifact).
- **The editor** (`ui/adjustment_panel.{hpp,cpp}`) is a **pinned corner popover** — the
  Type panel / "3D…" pattern, *not* a modal (user call 2026-07-17: the transactional
  OK/Cancel dialog was overkill for a handful of sliders) — **triggered by the adjustment
  layer being the active layer**: `updateAdjustmentPanel` (onFrame) watches active-layer
  transitions; selecting an adjustment layer with parameters opens the panel on it, selecting
  anything else closes it, and an Esc-dismissed panel stays closed until the selection changes
  again. Rows are generated from the schema (`ScrubSlider`/`CheckBox`/`Dropdown`, shared
  precision ruler); a **Choice** row's dropdown honours the descriptor's optional `choiceOrder` /
  `choiceDivider` so it can be **grouped and reordered for display** (Grayscale's method list) with
  `FL_MENU_DIVIDER` separators, while the widget position is mapped back to the stored enum index
  — the bag value never moves. The Grayscale editor also **hides the "Grays" palette row while the
  method is Dithered** (a 1-bit result has no palette to size), tracking the method live through
  the per-frame `syncValues`. Every edit streams through `applyAdjustmentField` — the
  `applyTextBlockField` twin — pushing
  one `SetAdjustmentParamsCommand` per edit with **per-control coalescing** (consecutive edits
  of the same control merge into one undo step; switching controls starts a new one), so the
  canvas is the live preview and undo composes for free. There is no OK/Cancel — the layer IS
  the state and undo is the escape hatch; **Reset** re-seeds the schema defaults as one
  ordinary undoable edit. While shown, the panel re-syncs each frame from the live bag, so
  undo/redo moves its sliders.
- **The two host-owned editor bubbles** (S34-a). Gradient Map's ramp chip opens the host's shared
  `ui::GradientFlyout` on itself, and Photo Filter's swatch opens the host's shared `ColorFlyout`
  through the Type/Image-ops `setOnEditColor` contract, the pick coming back via
  `AdjustmentPanel::setPickedColor`. Both follow the **ImageOpsPanel Fill rule**: the flyout's
  `onChange` is re-pointed on every open, so one bubble safely serves several openers, and the
  panel never *owns* a sub-window (build-before-show ordering stays the host's business). Both
  degrade honestly if the host never wires them — the chip is inert and the swatch is a read-only
  preview, while the layer still renders, still round-trips and still has its fourteen presets.
  A ramp edit streams through the ordinary funnel with **one undo step per gesture** (the Curves
  rule: the coalesce id carries a counter that bumps on any event that is not `FL_DRAG`), and a
  colour pick writes the `filter` row to **Custom** together with the three colour rows in one
  edit, so the swatch can never show a colour the compositor is not using.
- **The dock** badges adjustment rows with a passive **half-filled-circle** type mark in the
  texture chip's full-contrast one-ink (white on dark / black on light — an adjustment is a
  bigger deal than a shape or text mark), NOT clickable: it began as a framed chip, but the chip
  language means "opens an editor" and the panel opens by the layer simply being active. There
  is deliberately **no context-menu item either** (round 3: a right-click selects the row, which
  opens the panel anyway); after an Esc dismissal, re-selecting the layer reopens it.
- **Panel exclusivity — the corner-panel ARBITER** (`ui/panel_arbiter.{hpp,cpp}`, round 5): the
  rounds of hand-rolled show/hide/exclusion fights (buried windows, latched buttons, stolen
  focus) ended in a unified model, per the user's spec. Every corner panel registers with ONE
  decision core under two trigger kinds: **explicit** (button-toggled — Style…/3D…, with a
  `valid()` predicate so a request evaporates when its text session dies or its anchor is
  rebuilt away; toggling one explicit panel replaces another, the shared-corner exclusivity for
  free) and **conditional** (state-driven — the adjustment editor's `wants()` returns the active
  adjustment layer's id as a context token). An explicit request always outranks conditions, and
  the **queue** falls out of re-resolution: close Style… and the arbiter lands back on the
  adjustment panel because its condition still holds. Esc on a conditional panel suppresses
  exactly its current token (stays away until the selection moves — the old seen-latch, now a
  rule instead of ad-hoc state). The host's `syncCornerPanels()` (onFrame + after every button
  toggle) **reconciles widget reality against the arbiter's answer**, so external hides — theme
  closes, bar rebuilds, Esc — self-heal instead of desyncing per-panel state. The arbiter is
  pure logic (no FLTK), so every fight class that used to need interactive repro is pinned
  headlessly in `tests/test_panel_arbiter.cpp`. Auto-opens still **preserve keyboard focus**
  (`openFor` restores `Fl::focus`) — a selection-opened panel must never silence typing.
- **The row thumbnail** is a live preview of the layers the adjustment AFFECTS with the effect
  applied (`render::adjustmentPreview`): the `compositeChildren` walk truncated at the adjustment
  itself, so the accumulator after its step is exactly "the backdrop as this adjustment sees it,
  with it applied" — it agrees pixel-for-pixel with a group thumbnail containing the adjustment
  (which already ran the same walk). Rendered at a small doc-proportional resolution (max side
  96 px; the siblings raster at preview size, so cost is bounded by the preview, not the canvas)
  and cached behind a **scope fingerprint** (`adjustmentScopeRevision`: the adjustment's own
  params/mask/opacity/visibility plus `subtreeRevision` of every sibling below it), so an edit
  anywhere under the adjustment refreshes it and nothing else does. During live slider drags the
  refresh rides the text-thumbnail **settle timer** — the canvas recomposites per frame, the
  thumbnail once at rest. An invisible adjustment previews the plain backdrop (the document shows
  no effect either), and one with nothing below it shows transparency — both honest answers.

## 6. Tests

`tests/test_adjustments.cpp` pins analytic invariants, not golden pixels — the formulas are the
spec: schema well-formedness, seeded-defaults/clamp/fallback reads, command apply/undo/coalesce
boundaries, identity-at-defaults **byte-exactness** for the four identity kinds + Curves,
Levels window/gamma/output endpoints, Exposure's +1 EV = ×2 linear (expectation computed from
the analytic IEC 61966-2-1 curves in-test), hue rotation landing red→green→blue, saturation
−100 = HSL lightness, Color Balance's luminosity preservation, Threshold binarity, Posterize's
level lattice, and opacity gating on the new kinds. Two cases pin the **automask**: an
adjustment born with a `maskFromSelection` mask grades exactly the selected set — byte-identical
to no-adjustment outside, byte-identical to the unmasked result inside, and *halfway* under
half coverage (the AA/feather ramp must not harden into a binary cut-out) — and the
`anySelected()` gate rejects both an empty selection and an active selection of nothing.
`tests/test_adjustment_panel.cpp` builds the editor headlessly (the Type3d-panel pattern) and
pins the schema-to-widget census across a kind switch, the per-control field ids streaming
through the funnel, drift re-sync firing no edits, and Reset re-seeding defaults through the
funnel. The S7-a scoping and S55
PhotometricMatch batteries continue to pass unchanged.

**S34 adds**, all analytic:

- **Curves storage** — knots round-trip through the double bag exactly (doubles, so `==`, not
  `Approx`); re-encoding a decoded curve reproduces the same bag byte-for-byte, which *is* the
  `.mosaic` round trip; corner flags survive and a smooth knot writes no flag key; a shorter
  curve leaves no tail; writing the identity **erases**; the `curve_r` / `curve_rgb` prefixes do
  not collide; and a hostile bag (absurd `_n`, out-of-unit-square knots, a missing knot) decodes
  to a sane curve rather than a flattening constant.
- **Curves math** — a seeded layer, an empty bag, a pre-S34 document's stale keys and an
  explicitly-stored identity all composite **byte-identically to no layer**; the composite curve
  moves every channel alike and pins the endpoints; a per-channel curve leaves the other two
  channels **byte-identical** (the untouched-channel rule); the composition ORDER is pinned by a
  case where per-channel-then-composite and composite-then-per-channel land 40 levels apart; and
  opacity gates the result halfway.
- **Curves panel** — the layout census (a plot + a channel picker, zero scalar rows), `reflect()`
  seeding the plot from the stored channel, the picker re-seeding the plot *and* streaming one
  `adjust:channel` edit, a synthesized plot click writing knots through the funnel, a second
  gesture taking a **new** coalesce id while a drag within a gesture keeps it, and Reset erasing
  the knots.
- **Shadows/Highlights** — the locality proof: the *same* mid-gray patch in a dark surround and a
  bright surround, where one lifts hard and the other comes out **byte-identical** (weight zero ⇒
  exponent exactly 1); the highlight arm as its independent mirror; and a half-covering mask
  where every pixel equals either the full result or the untouched backdrop, byte-for-byte.
- **Defringe** — the purple band collapses a violet patch onto its own lightness while pure blue
  (0.125 turns away, outside the band) stays **byte-identical**; the green slider is independent;
  the chroma threshold gates a low-chroma pixel off entirely; and the lateral-CA rescale moves
  **only** red when only `ca_red` is set (green is the reference channel).
- **Matte Removal** — each mode against the algebra restated in the test at `a = 128/255`, alpha
  unchanged, `RemoveBlack == Unpremultiply` pixel-for-pixel, and all four modes byte-identical
  over an opaque backdrop.
- **Haze Removal** — amount 0 is byte-exact; amount 50 lands the mid-gray where
  `0.95 + (I − 0.95)/0.55` says it should; white and black clip to themselves; a warm tint breaks
  the neutral.
- **region == crop(full)** — the blur family's money invariant re-run for the two new spatial
  kinds: Shadows/Highlights' blurred mask, Defringe with an off-centre lateral CA (a
  centre-dependent reach), and a stack of a Gaussian plus Shadows/Highlights whose reaches sum.

**S34-a adds**, all analytic, in the same two files:

- **Gradient Map storage** — stops round-trip through the double bag exactly (doubles, so `==`);
  re-encoding a decoded ramp reproduces the same bag byte-for-byte (which *is* the `.mosaic` round
  trip); a straight blend writes no midpoint key and a biased one does; a shorter ramp leaves no
  tail; writing the default ramp **erases**; and a hostile bag (absurd `gm_n`, an out-of-range
  colour, stops stored **out of order**, a missing stop) decodes to a sane sorted ramp rather than
  a flattening constant.
- **Gradient Map math** — the default ramp is *visible* and lands every pixel neutral with the two
  ends pinned exactly (black→0, white→255); a black→red ramp pins both endpoints byte-exactly;
  `reverse` swaps which end each tone reads; a **fully transparent ramp is byte-identical to no
  layer** (the stop alpha *is* the per-tone strength); and opacity gates the result halfway.
- **Vibrance** — identity at the default; a muted patch's channel spread widens while a fully
  saturated red and a neutral grey come out **byte-identical** (weight exactly zero ⇒ the HSL
  round trip is skipped); and a full negative slider collapses the muted end (`s' = s²`) while
  still leaving pure red exactly alone.
- **Photo Filter** — density 0 is byte-exact; density 100 without luminosity preservation turns
  white into **exactly the filter's own colour**; seeded defaults are visible; warming and cooling
  push red/blue opposite ways and Custom reads the three colour rows; and Preserve luminosity
  holds the patch's **linear** luminance (computed in-test from the analytic sRGB curve) where
  switching it off drops it below 60 % of the source.
- **High Pass** — a flat field lands on mid-grey to within a level (the 8-bit boundary sits exactly
  at 0.5), a hard edge overshoots above it on the light side and undershoots below it on the dark
  side while settling back far away, alpha is untouched, the reach is exactly `1.5 × radius`, and
  **region == crop(full)** is byte-exact at an interior ROI.
- **Panel** — the Curves plot's histogram follows the channel picker (a red-only backdrop peaks at
  74 for luma, 200 for red and 20 for blue) and is empty with no backdrop provider; the Gradient
  Map layout is a ramp chip + the Reverse toggle with **no** sliders, seeds the chip from the bag,
  and Reset erases the stops; the Photo Filter layout is a picker + **one** slider + one toggle +
  one swatch (never three colour sliders), the swatch previews the named preset's colour, a pick
  writes Custom *and* the three rows as one edit, and a pick offered while a kind without a swatch
  is reflected is ignored.

## 7. Design constraints, per kind

Everything in S32 is decades-old textbook image processing: levels/gamma remap, photographic
exposure compensation, HSL hue rotation (Foley–van Dam era), tonal-band colour balance (shipped
in every editor since the early 90s), threshold, posterize. The *adjustment-layer concept itself*
dates to Photoshop 4 (1996).

The rules below are **standing constraints on this feature**. Each is deliberate and each costs
capability. Two kinds — **Defringe** and **Haze Removal** — are **narrower by design than the
textbook algorithm**; that narrowing IS the design, not a gap waiting to be filled in.

### 7.1 S34 kinds

- **Curves.** A user-authored interpolating tone curve applied as a per-channel lookup, over a
  spline literature that predates software entirely (Schoenberg 1946; de Boor 1978). Mosaic
  derives nothing: the knots are the user's.
  **⚠ C-1 (standing): never add an "Auto" button that computes the curve from image statistics.**
- **Shadows/Highlights.** The exact mechanism shipped — an inverted, low-passed, monochrome mask
  in the *exponent* of a per-channel power function — is N. Moroney, *Local Color Correction Using
  Non-Linear Masking* (IS&T/SID CIC 8, 2000).
  **⚠ S-1 (standing): no automatic histogram-moment or "analyze the photo and set the sliders"
  mode.**
- **Defringe — DELIBERATELY NARROWER THAN THE OBVIOUS ALGORITHM.** The obvious implementation —
  detect edges, then gate chroma suppression by edge proximity — is **not** what ships, and must
  not be built here.
  - **⚠ D-2 (standing): Defringe never computes an edge measure, an edge-proximity/locality score,
    or a per-pixel "is this a fringe" detection score, and never runs over a multi-resolution
    pyramid.** What ships instead is hue-band chroma suppression — a fringe is a colour, so it is
    selected by *hue and chroma alone*. That is targeted-hue desaturation, the same shape as
    Photoshop's own Hue/Saturation hue-range control (1993–94) and Replace Color. It is a real
    capability cost, taken on purpose.
  - **⚠ D-1 (standing): the lateral-CA scale factors are always the user's — Mosaic never estimates
    them from the image, from a test pattern, or from a lens database.** Bare radial per-channel
    scaling by a user-supplied factor is long-published work in its own right (Willson & Shafer
    1991; Boult & Wolberg, *Correcting chromatic aberrations using image warping*, CVPR 1992;
    Panorama Tools' manual per-channel radial scale, 1998, GPL).
- **Matte removal.** Porter & Duff, *Compositing Digital Images* (SIGGRAPH 1984) is the algebra
  verbatim; Smith & Blinn, *Blue Screen Matting* (SIGGRAPH 1996) the matte-recovery case; "Remove
  White/Black Matte" shipped in Photoshop 3.0 (1994). Nothing is estimated — the matte colour is
  named by the mode, and the alpha is the document's own.
- **Haze removal — DELIBERATELY NARROWER THAN THE FAMOUS FORMULATION.** The dark-channel prior is
  **not implemented**, and neither is any other transmission-estimation scheme.
  - **⚠ H-1 (standing): Haze Removal uses a spatially CONSTANT transmission. It must never derive
    a per-pixel or per-patch transmission/depth/airlight estimate — no dark channel, no patch
    minimum over channels, no local-contrast maximisation, no colour-line or haze-line prior, no
    learned transmission, and no automatic airlight estimation from the image's brightest pixels.**

  What ships is the classical remainder: Koschmieder's 1924 scattering model inverted at a user-set
  density about a user-set airlight colour — a per-channel affine map, i.e. a linear contrast
  stretch along the airlight direction (Rosenfeld & Kak 1976 for the stretch; Koschmieder 1924 /
  Middleton 1952 for the model). The estimation step the dehazing literature is really about is
  simply absent, and that absence is the design.
- **Frequency Separation — deferred (§8).** The low/high split itself is a difference-of-blur,
  which is textbook; the surrounding workflow is what is deferred.

### 7.2 S34-a kinds

- **Gradient Map.** Mapping an image's luminance through a user-authored colour ramp via a
  256-entry lookup dates to Photoshop 4 (1996), over a much older prepress / scientific-
  visualisation lineage (false-colour colormaps, duotone separation).
  **⚠ GM-1 (standing): the ramp is always the user's. Mosaic never derives, proposes,
  auto-generates or "suggests" a gradient map from image statistics** — the same shape as C-1.
  (The compositor's fixed `DitherKind::None` is a separate correctness constraint — §2.6.)
- **Vibrance.** Scaling chroma by a weight that falls as existing chroma rises is decades-old
  colour-appearance arithmetic; there is nothing inventive in `s·(1 + k·(1 − s))`, and `k` is a
  slider — Mosaic measures nothing.
  - **⚠ V-1 (standing): no automatic or "Auto Vibrance" mode that derives the amount from image
    statistics** (the C-1/S-1 shape again), **and no hue-band skin-tone detector.** Chroma
    weighting alone is the mechanism; a hue-gated "protect skin" term would be its own design
    surface.
- **Photo Filter.** A coloured filter over a photograph is an optical accessory older than the
  software industry, and the digital form — multiply the linear-light signal by the filter colour,
  blend by a density, optionally restore the original luminance — is the physically obvious
  transcription of it. Luminosity-preserving colour operations are the W3C/PDF `setLum` algebra
  already shipping in Color Balance. The named filter designations are the industry's own
  numbering, used descriptively.
  **⚠ P-1 (standing): the filter colour and the density are always the user's — no white balance
  estimation, no illuminant or colour-temperature detection from the image, no "auto-warm this
  photo".** Auto white balance is out of scope for this kind.
- **High Pass.** `p − G_σ(p) + ½` is the unsharp mask's own difference term drawn without the
  add-back; high-pass filtering by subtracting a low-passed copy is 1960s–70s textbook signal
  processing (Prewitt 1970; Rosenfeld & Kak, *Digital Picture Processing*, Academic Press 1976).
  **⚠ S-4** — no deconvolution, PSF estimation or camera-shake inversion (`docs/filters-stylize.md`
  §7) — already binds this kind: it is the sharpen family's sibling and lives in the same module.

## 8. Deferred out of S34 — Frequency Separation

Frequency Separation is a **workflow**, not an adjustment kind: one undoable command that
duplicates the active layer twice, blurs the lower copy, and makes the upper copy the
high-frequency difference (linear-light subtract, or the 8-bit "apply image" variant) in a group.
Nothing about it fits the params-bag/`applyAdjustment` machinery this document describes — it
creates layers, it does not grade a backdrop — so building it here would have meant a second,
unrelated command-and-layer-plumbing slice riding on the session whose headline was Curves.
It is deferred deliberately, with the Curves/S34 maths complete, and it needs: the command, the
group/naming/blend-mode convention, and a decision on 8-bit vs float subtraction.

**S34-a narrows what is left.** The two *bands* now both exist as ordinary layers: the low one is
a Gaussian Blur adjustment, the high one is **High Pass** (§2.9), and a High Pass layer on Overlay
over a Gaussian Blur layer is the split done by hand. What Frequency Separation would still add is
the one-command *workflow* — duplicate, blur, difference, group, name, set the blend mode — plus
the destructive-subtract decision. Nothing about that changed; the deferral stands, but whoever
picks it up starts from two working halves instead of none. High Pass's own constraints are in §7.2.
