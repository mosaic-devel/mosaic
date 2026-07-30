# S35 — Artistic / stylize filters

Status: BUILT 2026-07-29 (awaiting the user visual pass). Companion to
`docs/adjustment-layers.md` (S32 owns the schema/editor framework this rides on) and
`docs/blur-filters.md` (S33 owns the spatial/reach machinery it reuses). Scope: PLAN §9 S35 —
*"Sharpen/unsharp, noise/denoise, posterize, pixelate, emboss, oil/wave, vignette."* Posterize
already shipped in S32, so nine new kinds land here, all as non-destructive **adjustment layers**.

Everything below shipped: the nine kinds end to end, the CPU reference kernels
(`render/stylize_kernels.{hpp,cpp}`), the compositor seam (`render/stylize.{hpp,cpp}` — three
functions, one branch in `applyAdjustment`), the reach/region behaviour (§5) and the analytic
test batteries (§8). No GPU lane yet (§9).

## 1. What ships

| Kind | Menu label | Parameters | Spatial? |
| --- | --- | --- | --- |
| `Sharpen` | Sharpen… | amount (%) | yes (1 px) |
| `UnsharpMask` | Unsharp Mask… | radius (px), amount (%), threshold (levels) | yes |
| `AddNoise` | Add Noise… | amount (%), distribution (Gaussian/Uniform), monochromatic, seed | no |
| `Denoise` | Denoise… | radius (px), noise level (%) | yes |
| `Pixelate` | Pixelate… | cell size (px) | yes |
| `Emboss` | Emboss… | angle (°), height (px), amount (%) | yes |
| `OilPaint` | Oil Paint… | radius (px) | yes |
| `Wave` | Wave… | mode (Wave/Ripple), amplitude (px), wavelength (px), angle (°), phase (°), centre x/y (px) | yes |
| `Vignette` | Vignette… | exposure (EV), radius (px), feather (%), roundness, centre x/y (px) | no |
| `HighPass` **(S34-a)** | High Pass… | radius (px) | yes |

All created from **Filter ▸ Stylize ▸ …**, all editable in the S32 pinned-popover editor, all
masked / reordered / toggled / clipped like any adjustment layer, all automasked to an active
selection through the shared insert path (`docs/adjustment-layers.md` §5).

Defaults are deliberately **VISIBLE** (a 12 px pixelate cell, a −1.2 EV vignette), not identity —
the same sanctioned deviation from the S32 identity-at-defaults convention the blur family takes,
and for the same reason: a Filter-menu item that inserts a do-nothing layer is the broken promise
`adjustmentImplemented` exists to prevent. The identity early-out keys off the **effective**
parameters (amount ≤ 0, amplitude ≤ 0, exposure = 0, a sub-pixel cell), never off
default-equality — `StylizeOp::effective` is the single place that decides, and when it is false
the compositor returns before even copying the backdrop, so those settings are byte-identical to
no layer at all.

## 2. Semantics: what a stylize layer means

Same scoping as every adjustment: it restyles **the composited backdrop below it, in its scope**
(inside its group, or globally at root; clip-to-below restricts by the clip base's alpha). The
mask/opacity/clip blend is the blur family's, verbatim:
`out = unpremul(lerp(premul(orig), premul(styled), opacity × mask × clipCoverage))`, with an
`amt ≥ 1` fast path that keeps the unmodulated case **byte-equal to the raw kernel output** and
an `amt ≤ 0` skip that keeps a masked-out pixel **byte-identical to the unstyled backdrop**.

**Alpha is split by intent, not by convenience.** Three kinds genuinely *resample coverage* the
way a blur does and so move alpha with the colour — **Pixelate** (a mosaic cell averages
everything in the cell, alpha included), **Oil Paint** (the winning quadrant's mean *is* the
pixel, alpha included) and **Wave** (a displacement relocates the pixel, alpha included). The
other six leave alpha exactly as they found it: sharpening, unsharp, emboss, denoise, noise and
vignette **recolour** the backdrop, they add and move no coverage. (Sharpening alpha in
particular carves visible halos into an anti-aliased edge — a bug, not a look.)

All kernels work in **premultiplied** space and un-premultiply on the way out; straight-space
filtering bleeds the arbitrary RGB of fully transparent pixels into visible ones. A pixel that
comes out with (near) zero coverage keeps the RGB it went in with — un-premultiplying by zero has
no answer, and leaving it alone keeps transparent pixels byte-identical instead of flushing them
to black. RGB is floored at 0 on the way out (a sharpening undershoot is not a colour) but
deliberately **not** capped at 1: the working buffer carries HDR headroom and the 8-bit
conversion clamps at the end anyway.

**Units.** Every px-dimensioned parameter is in the adjustment's **parent-space pixels** (document
px at root, group-local px inside a group), exactly like the blur family. Two different mappings
follow from that, and which one a kind uses is the thing to know about it:

- **Buffer-space kinds** — Sharpen, Unsharp, Denoise, Emboss, Oil Paint — scale their lengths by
  the placement's axis scale, so a 96 px scope preview stylises *proportionally* instead of at
  full-canvas strength (docs/blur-filters.md §4).
- **Parent-space kinds** — Pixelate, Wave, Vignette — do their whole geometry in parent space and
  map only the final sample position back through the placement. That is what makes their output
  **region- and pan-stable to the byte**: the mosaic lattice, the wave's crests and the vignette's
  falloff are properties of the document, not of whatever buffer happens to be rendering it.

Centres (`center_x/center_y`, used by Wave/Ripple and Vignette) are parent-space px too, **not**
normalised, and the Filter-menu insert seeds them to the document centre through the existing
S33 hook. The Vignette additionally has its `radius` seeded from the canvas diagonal at insert
(§3), because a fixed px default cannot know how big the document is any more than the centres
could.

## 3. The maths, filter by filter

All in `render/stylize_kernels.{hpp,cpp}`, pure CPU float, no FLTK/GPU. Edge policy throughout:
**clamp-to-edge (replicate)** — a stylize filter must not vignette or ring at the canvas edge,
and a region buffer's edge taps then land on the same physical pixel the full composite's do.

### Sharpen — `amount`
`out = p + amount·(p − mean₄(p))` on premultiplied RGB, where `mean₄` is the four-neighbour (von
Neumann) mean. At `amount = 100 %` this is *exactly* the textbook
`[[0,−1,0],[−1,5,−1],[0,−1,0]]` convolution — the one-knob, one-pixel sharpener. Range 0–300 %,
default 100 %. Reach 1 px.

### Unsharp Mask — `radius`, `amount`, `threshold`
`out = p + amount·(p − Gσ(p))`, σ = **radius/2** (the blur family's reading of "radius" as the
visually apparent extent, so an Unsharp radius means what a Gaussian Blur radius means).
Separable Gaussian, support 3σ. The **threshold** gate is per pixel on the *luma* of the
difference: `|0.30·d_r + 0.59·d_g + 0.11·d_b| < threshold` → the pixel is left alone, so film
grain and skin texture stay put while real edges take the boost. Threshold is in **8-bit levels**
(0–255, the unit the control has been stated in since the darkroom crossed over to software);
the math divides by 255. Ranges: radius 0.1–250 px (default 2), amount 0–500 % (default 100),
threshold 0–255 (default 0). Reach 1.5 × radius (= 3σ).

A live drag truncates the Gaussian's support from 3σ to 2σ. That is the **only** draft lane the
S35 family has, because every other kernel here is O(1) per pixel; the settled composite always
runs full quality (the S30-draft pattern).

### High Pass (S34-a) — `radius`
`out = ½ + (p − G_σ(p))`, σ = **radius/2** — the same Gaussian, the same reading of "radius", the
same 3σ support and the same 2σ draft lane as Unsharp Mask above. It is literally that filter's
difference term drawn *without* the add-back, which is why it lives here rather than growing a
second Gaussian in `compositor.cpp`, and why it inherits Unsharp's reach row unchanged
(1.5 × radius). Mid-grey is the zero of the difference — which is exactly why the result
composites through Overlay / Soft Light, both of which leave 0.5 alone, and why this is the
missing half of frequency separation (the low band is a Gaussian Blur layer; this is the rest).

The difference is taken on **premultiplied** planes and un-premultiplied *before* the ½ bias is
added, so the bias is a straight-space constant rather than something divided by coverage; a pixel
under (near) zero coverage keeps the RGB it came in with, the shared `writeBack` rule. The result
**is** clamped to [0,1] — the one place in this family where the HDR-headroom exception does not
apply, because a high pass is a display-space read around a fixed mid-grey. Alpha untouched.
Range 0.1–250 px (default 10). Deliberately visible at defaults, for the blur family's reason: a
zero-radius high pass is a flat grey field, not an identity. Full write-up:
`docs/adjustment-layers.md` §2.9; that document's §7.2 carries its constraints.

### Add Noise — `amount`, `distribution`, `monochromatic`, `seed`
Adds IID noise of std-dev σ = amount/100 to the **encoded** channel values (film grain is a
display-space texture, and that is where the Amount numbers are legible), floored at 0 and left
uncapped so HDR headroom survives. Gaussian via Box–Muller from two hashes; **Uniform** is scaled
to the *same variance* (half-width σ√3) so the Amount slider means one thing in both modes.
`monochromatic` draws one sample per pixel instead of one per channel. Alpha untouched. Reach 0.
Ranges: amount 0–100 % (default 12), seed 0–9999 (default 1).

**Determinism (the requirement, and how it is met).** A filter that reshuffles on every
recomposite is a bug — and a filter that reshuffles between a *full* composite and a *dirty-rect*
one is a worse bug, because the seam shows. The sample is therefore a **pure hash** of
`(seed, ⌊parent_x⌋, ⌊parent_y⌋, channel)` — never a running RNG, never a buffer index. Consequences,
all of them intentional:

- Two composites of the same document are bit-identical.
- `compositeRegion(roi)` is bit-identical to the matching window of the full composite (test-pinned).
- The grain is pinned to the **document**: it does not swim under pan, zoom or a canvas crop.
- Under a scaled preview the lattice is sampled sparsely (many buffer pixels can land on one
  parent cell, or skip cells) — the honest consequence of anchoring the grain to the document
  rather than to the screen, and the same thing a real film grain would do under a loupe.

The mixer is the public-domain `lowbias32` 32-bit finalizer (three xorshift-multiply rounds,
C. Wellons 2018), applied to the seed and each coordinate in turn. Signed coordinates convert to
unsigned modulo 2³² (well-defined), so an off-canvas region buffer or a negatively translated
group hashes as cleanly as the origin.

### Denoise — `radius`, `noise level`
**Lee's local linear MMSE filter** (J.-S. Lee, *Digital image enhancement and noise filtering by
use of local statistics*, IEEE TPAMI 2(2), 1980): over a (2r+1)² window,

```
m   = local mean            var = max(0, E[p²] − m²)
k   = max(0, var − σ²ₙ) / var
out = m + k·(p − m)
```

Flat areas (var ≤ σ²ₙ — all noise) collapse to the local mean; structured areas (var ≫ σ²ₙ) pass
through untouched, which is what stops it smearing edges. O(1) per pixel via the separable
running-sum box means. Alpha untouched. Ranges: radius 1–16 px (default 3), noise level 0–50 %
(default 8). Reach = radius.

*What is deliberately not here* (§7's S-3 states it as a standing rule): no non-local means, no
BM3D, no wavelet shrinkage, no learned denoiser. Median filtering is **deferred** for cost —
an honest median at a useful radius is O(r²·log) per pixel per channel and there is no way to
offer a 16 px radius row that silently clamps to 3 without lying to the user. Lee 1980 gets the
same job done at O(1).

### Pixelate — `cell size`
Every pixel takes the mean of the cell it falls in, in premultiplied RGBA (alpha included). The
lattice is `cell = ⌊parent_coord / size⌋` — **anchored in parent space**, so the blocks are a
property of the document. Range 1–512 px (default 12); a cell under ~1.5 buffer px cannot read as
a block, so below that the kernel is a declared no-op rather than a shimmering near-identity.
Reach = size (a cell reaches exactly one whole cell past any pixel inside it).

The accumulation pass is **serial on purpose**. A banded parallel reduction would sum a cell's
pixels in an order that depends on the band split — and so on the buffer's height — while a
region composite's blocks have to be byte-identical to the full composite's. Row-major over the
buffer visits a cell's pixels in the same relative order in *any* buffer that fully contains the
cell, so the float sum is the same bits. It is one streaming add per pixel; the write-back is
parallel.

Crystallize (Voronoi cells) is **deferred**: it is a different data structure (a seed set plus a
nearest-site query), not a variation on the cell mean, and it would not have stayed cheap.

### Emboss — `angle`, `height`, `amount`
`out.rgb = clamp(0.5 + amount·(L(p + off/2) − L(p − off/2)))` where `L` is the **premultiplied**
luma plane and `off` is the `height`-long vector at `angle`, mapped through the placement. The
result is grey: a relief map is a height read, not a recolouring. Alpha untouched, so an embossed
transparent region stays transparent — and reading the relief off *premultiplied* luma means the
shape's own alpha edge embosses like any other edge (straight luma there is arbitrary).
Ranges: angle ±180° (default 45), height 0.5–32 px (default 2), amount 1–500 % (default 100).
Reach = height/2 + 1.

The tap separation is floored at **one buffer pixel**: a scaled-down preview would otherwise
sample the same pixel twice and render flat mid-grey, which reads as "the filter is broken"
rather than "the preview is small". Deliberate deviation from strict proportional scaling, and
the only one in the family.

### Oil Paint — `radius`
The **Kuwahara filter** (M. Kuwahara, K. Hachimura, S. Haruyama, M. Kinoshita, *Processing of
RI-angiocardiographic images*, in *Digital Processing of Biomedical Images*, Plenum Press 1976):
the window is split into four overlapping quadrants and the pixel takes the **mean of the
quadrant with the smallest luminance variance**. Interiors flatten into paint-like patches while
edges stay exactly where they are — the reason it reads as paint and not as blur.

Implemented in O(1) per pixel: a box mean of half-width `h` read at the offset (±h, ±h) *is* the
mean of the corresponding quadrant of the (4h+1)² window centred on the pixel, so four box means
of the luma and its square give all four variances, and four more give the channel means. The
winning quadrant is stored as one byte per pixel so the four channel passes never hold four
full-resolution mean planes alive at once. `h = round(radius·scale / 2)`, so the effective window
is ≈ 2 × radius. Range 1–32 px (default 4). Reach = radius + 1. Alpha takes its quadrant's mean
with everything else.

Exactly the **original 1976 four-quadrant form** — not the anisotropic/structure-tensor
generalisation, not a polynomial-weighted variant (§7).

### Wave / Ripple — `mode`, `amplitude`, `wavelength`, `angle`, `phase`, `centre`
A sinusoidal displacement resample, computed entirely in parent space:

- **Wave**: `d(p) = amplitude · sin(2π (p·n)/λ + φ) · dir`, with `dir = (cos angle, sin angle)`
  and `n` its perpendicular. At angle 0 each row slides horizontally by a sine of its `y` — the
  shape everyone means by "wave".
- **Ripple**: `d(p) = amplitude · sin(2π |p − c|/λ + φ) · (p − c)/|p − c|`, concentric rings about
  the centre. The exact centre has no radial direction and is left put.

**Pull** semantics: the output pixel takes the colour that sat `d` away from it. Source reads are
bilinear on premultiplied RGBA. Alpha is resampled with the colour.

**Edge policy — decided and documented**: reads outside the buffer **clamp to the edge**
(replicate). The alternatives were rejected on purpose: *wrap* makes the top of the image appear
at the bottom of a region buffer (and a region buffer's "edges" are not the document's, so wrap
is not even well defined under a dirty-rect recomposite); *transparent* punches holes along the
canvas border that no other filter in the app punches, and turns "wave a background layer" into
"wave a background layer and then repair its edges". Clamp keeps a full-bleed image full-bleed,
and it is the same policy the blur family already committed to, so the two compose without a
seam. Ranges: amplitude 0–500 px (default 20), wavelength 1–4000 px (default 80), angle/phase
±180°. Reach = amplitude + 1.

### Vignette — `exposure`, `radius`, `feather`, `roundness`, `centre`
Per pixel, on the parent-space offset from the centre:

```
q    = ( |dx/r|ⁿ + |dy/r|ⁿ )^(1/n)          n = 2·2^(roundness/100)
t    = smoothstep(0, 1, (q − 1)/(outer − 1))    outer = 1 + feather/100
gain = 2^(exposure · t)                          applied in LINEAR light
```

`q ≤ 1` returns the backdrop **byte-identically** — the kernel skips the pixel outright, so the
un-vignetted core carries no decode/encode round-trip error at all. Outside it, the falloff is a
**light-level scale**, so it happens in linear light through the sRGB LUT pair and comes back
through the encode (the Exposure-kind precedent). Alpha untouched: a vignette dims the backdrop,
it does not erase it. Reach 0 (position-dependent, but per pixel).

`roundness` doubles the superellipse exponent per +100 and halves it per −100: 0 → 2 (a plain
circle/ellipse), +100 → 4 (squarer, hugs a rectangular frame), −100 → 1 (a diamond). Smooth,
monotonic, and the familiar shape at the default.

The falloff is **circular by default**, not frame-fitted, because that is what a lens actually
does — the image circle is a circle, and on a wide frame it darkens the left and right edges more
than the top and bottom. Roundness is there for when the stylistic answer beats the physical one.

Ranges: exposure ±5 EV (default −1.2), radius 0–16384 px (schema default 300, **seeded at insert
to 0.55 × half the canvas diagonal**), feather 0–200 % (default 60), roundness ±100 (default 0).

## 4. Compositor integration

`render/stylize.{hpp,cpp}` exposes exactly three functions and the compositor gains exactly one
branch:

- `isStylizeKind(kind)` — asked by `applyAdjustment` **before** its own scalar/spatial split; true
  routes to `applyStylizeAdjustment` and returns.
- `applyStylizeAdjustment(acc, adj, coverage, pre, maskDomain, liveDrag)` — resolves the schema
  into a `StylizeOp` (§2 units), copies the backdrop, runs one kernel over the copy, and blends it
  back under opacity × mask × clip coverage with `applyBlurAdjustment`'s loop.
- `stylizeAdjustmentReach(adj, domain)` — forwarded to from `blurAdjustmentReach`, which is what
  plugs the family into the region/group-buffer machinery for free. The `domain` argument is
  unused on purpose: no S35 kind's support grows with the pixel's distance from a centre the way
  Radial Blur's spin/zoom taps do, so every bound is a constant of the parameters.

The mask helpers (`adjustmentMaskAt`, `adjustmentMaskDomain`, `isIdentity`, `maxAxisScale`) are
restated inside `stylize.cpp` rather than exported from `compositor.cpp`: keeping the coupling to
a single branch is worth four small functions, and `docs/adjustment-layers.md` §2 is the contract
both copies implement. The sRGB LUT pair is duplicated for the same reason.

## 5. Reach, regions and group buffers

Same model as the blur family (`docs/blur-filters.md` §5): `stylizeAdjustmentReach` reports the
support radius in application-space px, `descendantAdjustmentReach` **sums** it across a stack (a
denoise under a pixelate compounds), `groupLocalExtent` grows both the content rect and the
visible-window pullback by it, and `compositeRegion` expands the ROI, composites and crops back.

| Kind | Reach | Why |
| --- | --- | --- |
| Sharpen | 1 | the 3×3 kernel |
| UnsharpMask | 1.5 × radius | 3σ, σ = radius/2 |
| HighPass (S34-a) | 1.5 × radius | the same Gaussian |
| AddNoise | 0 | per pixel |
| Denoise | radius | the box window half-width |
| Pixelate | size | one whole cell past any pixel inside it |
| Emboss | height/2 + 1 | the tap separation, plus the bilinear tap |
| OilPaint | radius + 1 | the window half-width |
| Wave | amplitude + 1 | the largest displacement |
| Vignette | 0 | per pixel |

**Region == crop(full).** Three of the nine are provably byte-exact by construction, and they are
the three that would break most visibly if they were not: Add Noise and Vignette are pure
functions of the pixel's parent-space position, and Pixelate's lattice is parent-anchored with a
one-cell reach and a deterministic (serial) cell sum. All three are test-pinned, at an interior
ROI and at one touching the canvas edge. The windowed kinds (unsharp, denoise, oil paint, emboss)
inherit the blur family's argument: clamp-to-edge at a border that is ≥ reach away from every
requested pixel, and at the canvas edge both buffers end at the same physical pixel — with the
same caveat the blur family already carries, that a running-sum box mean's last bits depend on
where the row started, so the equivalence there is "matching to the byte in practice, exact in
the reach argument".

The seven spatial kinds must be declared spatial in `adjustmentIsSpatial` for any of this to fire
(the reach walk only asks spatial kinds); Add Noise and Vignette must **not** be, so they never
grow a buffer they do not need. Being spatial also opts a kind into the editor's draft-scrub
settle, which is what Unsharp's draft lane wants.

## 6. UI

- **Editor**: the S32 schema-generated popover — no new dialog. Every S35 row is a plain
  Scalar/Choice/Toggle/Angle, so nothing new was needed.
- **Filter ▸ Stylize ▸ …**, below Blur, grouped by what the filter does to the image: *sharpen*
  (Sharpen, Unsharp Mask, **High Pass**) | *grain* (Add Noise, Denoise) | *abstraction* (Pixelate,
  Emboss, Oil Paint) | *geometry & light* (Wave, Vignette), with `FL_MENU_DIVIDER` between the
  groups. High Pass joins the sharpen group because it is the same Gaussian difference, and
  because "the layer you put on Overlay" is what people reach for it next to.
- **No canvas gizmo in this slice.** Vignette and Wave/Ripple both carry a centre and would read
  well as on-canvas handles, and the S33 blur gizmo (binding 11, `DofGizmoState::kind`) is exactly
  the pipeline to hang them on — but that file is a UI file and the gizmo is a session of its own.
  Recorded as owed work, not as a gap in the filters.

## 7. Technique lineage and the standing constraints

Every kernel here is long-published image processing, and where a filter deliberately ships the
*simpler* of two forms, that is recorded as a standing constraint rather than left to be
rediscovered.

- **Sharpen (3×3 Laplacian).** Discrete Laplacian / high-pass sharpening is 1960s–70s textbook
  work (Prewitt 1970; Rosenfeld & Kak 1976).
- **Unsharp Mask.** The technique is *photographic*, from the 1930s German printing trade (the
  blurred-negative mask), digitised in the 1970s–80s. The three-knob amount/radius/**threshold**
  form dates to 1990-era image editors. We ship the plain form: one Gaussian, one difference, one
  luma-threshold gate. **No deconvolution, no PSF estimation, no camera-shake inversion** — see
  S-4.
- **Add Noise (IID Gaussian/uniform, hash-seeded).** Box–Muller is 1958. Deliberately **not**
  shipped: parametric *film-grain synthesis* — this is IID noise with a documented distribution,
  no grain model, no template database, no transmission-side parameterisation. See S-5.
- **Denoise — simplified on purpose.** We ship **Lee 1980** local-statistics MMSE. The
  formulations deliberately not shipped: **non-local means** (Buades/Coll/Morel, CVPR 2005),
  **BM3D** (Dabov/Foi/Katkovnik/Egiazarian 2006–07), wavelet shrinkage, and every learned
  denoiser. Ship the simpler one and say so — which is what §3 says. See S-3.
- **Pixelate (block-mean mosaic).** Mosaic/block-averaging effects date to 1970s video hardware.
  (We ship a *filter*, not a redaction workflow, and claim nothing about irreversibility.)
- **Emboss (directional difference + mid-grey bias).** A two-tap directional derivative is
  Sobel/Prewitt-era work (Sobel 1968); the "add 0.5 and show it grey" presentation is documented
  in late-1980s paint packages.
- **Oil Paint (Kuwahara 1976).** We ship the **original 1976 four-quadrant form**. Deliberately
  **not** shipped: the *anisotropic* Kuwahara (Kyprianidis, Kang & Döllner, CGF 2009) and its
  polynomial-weighted successor (NPAR 2010) — the classic form is what "oil paint" means anyway.
  See S-2.
- **Wave / Ripple (sinusoidal displacement resample).** Image warping by an analytic displacement
  function is textbook geometry (Wolberg, *Digital Image Warping*, 1990; the sine displacement
  itself is older than the field).
- **Vignette (radial exposure falloff).** Darkening the edges of a picture is a darkroom technique
  (burning-in) and an optical property of lenses; the digital form is a per-pixel gain as a
  function of radius. See S-1.

**Standing constraints (the S35 S-series — same status as the S33 B/G/U-series; every future
stylize change must respect them):**

- **S-1** (vignette): plain linear-light exposure scale only. No highlight-priority /
  contrast-protecting variant, no tone-dependent falloff, no lens-profile auto-correction.
- **S-2** (oil paint): the original 1976 four-quadrant Kuwahara only. No structure-tensor,
  anisotropic, or polynomial-weighted generalisation.
- **S-3** (denoise): classical local-statistics / order-statistic filters only. Non-local means,
  BM3D-family block matching, wavelet shrinkage and learned denoisers each need their own design
  pass before they may be considered.
- **S-4** (sharpen): no deconvolution, PSF estimation, or camera-shake inversion under the
  Sharpen/Unsharp/**High Pass** kinds — that is its own session if ever. (High Pass's own write-up
  is `docs/adjustment-layers.md` §7.2, on Prewitt 1970 / Rosenfeld & Kak 1976 and the unsharp-
  masking lineage above.)
- **S-5** (add noise): IID noise with a published distribution only. No parametric film-grain
  *model* (template/AR grain parameterisation).

## 8. Verification

Headless (the standing division of labour), in `tests/test_stylize.cpp` (framework + composite
level) and `tests/test_stylize_kernels.cpp` (analytic kernel signatures):

- schema well-formedness across the nine kinds (unique keys, min ≤ def ≤ max, choice ranges);
- `isStylizeKind` / `adjustmentImplemented` / `adjustmentIsSpatial` agree with the family table;
- reach values, including a hostile out-of-range parameter clamped by the schema read;
- **identity**: every zero-amount setting composites byte-identically to no layer; every kind at
  opacity 0 composites byte-identically to no layer;
- **region == crop(full)**, byte-exact, for Add Noise, Vignette and Pixelate, at an interior ROI
  and at one touching the canvas edge;
- analytic kernel pins: the 3×3 sharpen's exact overshoot/undershoot on binary-exact values, the
  unsharp threshold as a real gate (byte-exact pass-through above it, a widening step below it),
  the noise's *translation* invariance in parent space (shift the placement, the grain follows the
  document), the mosaic lattice following the parent lattice, Kuwahara holding a step edge with
  no intermediate values minted, emboss flat-field ≡ exact mid-grey plus a lit/shadowed edge pair,
  a whole-pixel wave displacement being an exact shift, and the vignette core being byte-exact.

The user's visual pass covers look and feel of every kernel and the popover ergonomics.

## 9. Not in this slice

- **No GPU lane.** The blur family's `setBlurRenderOverride` seam is the pattern to copy
  (`docs/blur-filters.md` §8) and every kernel here is compute-shader-shaped, but the CPU lane is
  the golden reference and it is fast enough (everything except unsharp is O(1) per pixel).
- **No canvas gizmo** for the Vignette / Ripple centres (§6).
- **Crystallize** (Voronoi cells) and a **median** denoise method (§3), both deferred for cost.
- **Frequency separation** stays where `docs/adjustment-layers.md` §8 put it: a retouching
  workflow, not an adjustment kind.
