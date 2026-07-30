# Gradient Tool (S22) — design

This note covers **S22 — the interactive Gradient tool**. It is *not* greenfield: the vector value
model (`core::vec::Gradient`), the CPU rasteriser that fills linear/radial/conic gradients, the
`GradientFlyout` stop editor, and the `VectorLayer` container all shipped with S25 and are described
in `docs/vector-model.md`. S22 adds the **tool**: drag-to-author geometry, an on-canvas handle gizmo,
per-segment **blend curves**, and the "gradient layer" as an editable, maskable object.

---

## 1. What S22 builds on top of the existing pieces

| Piece | Where | Status before S22 |
|---|---|---|
| `vec::Gradient` (type + stops + transform + spread) | `core/vector/paint.hpp` | shipped (S25) |
| Gradient sampler (linear/radial/conic) | `core/vector/raster.cpp` `gradientParam` | shipped (S25) |
| `GradientFlyout` (stops + spread editor) | `ui/gradient_flyout.*` | shipped (LE / Fill dialog) |
| `VectorLayer` holding one `vec::Object` | `core/layer.hpp` | shipped (S25) |
| `SetVectorObjectCommand`, `AddLayerCommand` | `core/commands.*` | shipped (S25/S26) |

S22's new code: `ui/gradient_gesture.*` (pure authoring math), the `GradientToolHost` +
push/drag/finish gesture in `VulkanCanvas`, the app-side host wiring in `app_window.cpp`, the
context-bar "Type"/"Stops…"/"Dithering"/"Opacity" options, and two model fields —
`GradientStop::midpoint` for blend curves and `Gradient::dither` for banding control (§7).

---

## 2. The gradient-layer representation (design decision)

**A gradient layer is a `VectorLayer` whose `vec::Object` is a full-bleed `RectShape` filled with a
`vec::Gradient`.** This is exactly what `docs/vector-model.md` §1 prescribed ("a 'gradient layer' =
a VectorObject whose geometry is a full-bleed RectShape and whose fill is a Gradient"). Consequences,
all of which the task requires and none of which needed new machinery:

- **Editable / re-openable.** The whole geometry lives in the gradient's `transform` (an `Affine2D`
  from gradient-unit-space to object-local). Re-selecting the tool on the layer reconstructs the
  handles from that transform (`gradientHandles`), so a re-drag edits *the same gradient*, never a
  fresh copy.
- **Maskable like every layer.** It carries the base `Layer`'s optional `RasterMask` for free — you
  mask a gradient exactly as you mask a raster or shape layer. The full-bleed rect + mask is *the*
  way to shape a gradient's extent (no bespoke "gradient bounds").
- **Precision-independent.** The compositor renders vector layers at target resolution, so the
  gradient is band-free and crisp at any zoom, and it serialises through the existing docjson vector
  path (`io/mosaic/docjson.cpp`).
- **One undo step per gesture.** Authoring detaches the live preview layer and re-adds it through one
  `AddLayerCommand` (the Affinity-style live-preview pattern shared with the Shape tool); handle
  re-drags coalesce into one `SetVectorObjectCommand` per gesture.

**The four shapes → three model types.** The tool offers Linear / Radial / **Elliptical** / Conic.
The model has only `Linear/Radial/Conic`, because **Elliptical is a Radial with an anisotropic
transform**: the unit circle maps to an ellipse when the transform's x- and y-axis scales differ. On
re-edit, `gradientShapeOf` tells circular Radial from Elliptical by comparing the two axis lengths.
No new enum, no new rasteriser path. `tests/test_gradient_paint.cpp` carries the proof obligation
that comes with collapsing four shapes into three types: that an elliptical authoring's transform
maps the unit *circle* onto the intended ellipse, and that the evaluated iso-parameter contours are
that ellipse — checked pointwise, per kind, under every `SpreadMethod`.

**The "Type" choice is live on the bound layer.** Picking a kind on the context bar retypes the
gradient layer currently open for editing, in place (`retypeGradient`): the centre and the primary
axis you dragged are re-read from the transform and re-authored under the new kind, so Linear →
Radial → Conic keeps the same geometry instead of throwing it away and waiting for a fresh drag.
Retyping *to* Elliptical from a circular transform adopts the default 0.5 aspect (an ellipse whose
`ry` equals its `rx` is a circle, and would read straight back as Radial); retyping *away* from it
restores an isotropic scale. Re-entering the tool on a gradient layer syncs the choice the other way,
from the layer's own shape. With no layer bound, the choice is simply what the next drag authors.

**The on-canvas gizmo** reuses the existing connector-plus-handles overlay lane (`setLineGizmo`): a
square handle at each end of the gradient's axis (start↔end for Linear; centre↔edge for the radial
family and Conic), a round midpoint handle to slide the whole gradient, and — for Elliptical — a
fourth handle on the **minor axis**, which sets `ry` and nothing else (only the component along the
minor axis counts, so sliding it lengthways can't squash the ellipse). The radial family also draws
its **shape outline**: the circle a Radial (or a Conic's sweep) covers, the ellipse an Elliptical
does, as a hairline ring on the overlay-line lane. The ring is the exact affine image of the
gradient's unit circle — built from the handle anchors, so it stays right under a rotated view or a
transformed layer — and Linear draws none, its axis line already being its whole extent.
Hit-testing and handle math are pure (`hitGradientHandle`, `dragGradientHandle`,
`gradientRingDistance`) and unit-tested; drawing rides lanes the present pass already ships.

**The ring is a curve, not a polygon (fix, S22 follow-up).** It was first built as a *chorded*
polyline pushed one segment at a time onto the guide-line lane, with the chord count from a
quarter-pixel sag target, `n = π·√(2R)`. That does not fit: the lane
(`render::kGuideLineMax`) is **64 entries for the whole channel**, shared with the document's ruler
guides and the Move tool's smart guides. `n` therefore had to be clamped to 64 — which is the
polygon: 64 chords sag 0.72 px on a 600 px-radius ring and 1.2 px at 1000 px, and the clamp bit for
anything past a ~207 px on-screen radius, i.e. essentially always. Worse, whenever the ring *was* up
it filled the entire lane, so the document's own guides silently stopped drawing. A second, smaller
defect compounded it: the count came from the **major** axis alone, so an ellipse whose minor handle
had been pulled out past the major was under-sampled where it curves hardest.

The fix is a **curve primitive**, not a bigger budget. The ring is now drawn **analytically** in the
present pass: the canvas pushes the centre and the two basis vectors as **two** lane entries and
`canvas_present.comp`'s `gradientRing()` evaluates the first-order (Taubin) distance to the ellipse
per screen pixel — `F/|∇F|` of `|M⁻¹(p − c)|² = 1`, which is zero exactly on the curve and correct
to first order beside it, all a 1 px hairline needs. There is no chord count, so the ring is exactly
smooth at every zoom, and 62 of the lane's 64 slots stay free for real guides.
`ui::gradientRingDistance` is the same expression on the CPU and is unit-tested against a
brute-force distance-to-ellipse, including the radius-independence that a polyline can never have.
One wart, flagged where it happens: the lane carries **no kind tag** — `WindowRenderer` hard-writes
the trailing `std430` float as zero — so the two-entry pair is marked by an *impossible* colour, a
negative red. That is the same out-of-range-sentinel idiom the present pass already uses for the
Move anchor and the Type bend tab, but the tidier home for it is a real `kind` field on
`WindowRenderer::GuideLine`.

**Draw order: the handles win.** The guide lane draws *over* the gizmo lane (guides are meant to sit
on top of chrome), so once the ring rode that lane it painted straight over whichever handle sat on
it — and a handle you cannot see is a handle you cannot grab. The present pass now defers the
line/gradient gizmo (mode 5, and only that mode) until **after** the guide lane, so the handles come
out over both the ring and any document guide crossing them. Every other gizmo mode keeps its
original order.

---

## 3. Lineage — the gradient math

**Parametric colour gradients are among the oldest techniques in raster/vector graphics, and every
ingredient here is decades old.**

- **Linear / radial gradients + a spread/repeat method.** PostScript shadings and the axial/radial
  fill model date to the mid-1980s (Adobe PostScript, 1984–85); SVG 1.1 (W3C REC, 2003) standardised
  `linearGradient`/`radialGradient`/`spreadMethod` (pad/repeat/reflect) — an open, royalty-free web
  standard. Photoshop's gradient tool shipped in **Photoshop 1.0 (1990)**. Our `applySpread` /
  `gradientParam` implement exactly the SVG-standard math.
- **Conic (angular) gradients.** Angular/"sweep" gradients date to ~1990s DTP (CorelDRAW's
  conical fountain fill) and are today a royalty-free web standard (CSS Images Level 4
  `conic-gradient`; SVG-native only as our extension, which is why the model flags it "no SVG 1.1
  export"). The evaluation is a plain `atan2`.
- **Elliptical gradients** are just a radial gradient under a non-uniform affine — an affine change of
  variables, standard in every vector renderer (SVG `gradientTransform`, PDF shadings).
- **Blend curves (the per-stop `midpoint`).** The colour "midpoint" — where two adjacent stops mix
  50/50 — is the diamond marker of Photoshop's Gradient Editor (**mid-1990s**) and the CSS
  `<color-hint>` of CSS Images Level 4 (a royalty-free W3C standard). Our `applyMidpoint` uses the
  published exponent remap `blend = f^(log 0.5 / log m)`, which yields 0 at f=0, 1 at f=1, and 0.5 at
  f=m — the same curve the CSS spec describes. Default `m = 0.5` is an exact identity, so pre-S22
  gradients render byte-for-byte unchanged.

## 4. Lineage — the interactive tool

**Built from generic direct manipulation, and nothing more.**

- **Drag-to-define a gradient's direction/extent.** Photoshop's gradient tool has worked this way
  since **1990**; CorelDRAW's **Interactive Fill tool (CorelDRAW 7, 1996)** put draggable
  start/end/centre handles *on the canvas* to edit a fountain fill's geometry. On-canvas handles that
  move a gradient's endpoints/radius/angle are therefore well-established 1990s practice. Our gizmo
  is plain direct manipulation of the transform's basis vectors.
- **⚠ What we deliberately do NOT build: gradient *mesh*.** A mesh gradient is a lattice of colour
  nodes with per-patch interpolation. Mosaic ships **none**: our gradients are the four parametric
  axial/radial/conic/elliptical kinds only. Mesh gradients are out of scope for S22 and are not to be
  added without their own design pass.
- **Live in-place preview** (the layer updates as you drag) is generic immediate-mode UI, universal
  across editors.

## 5. Out of scope, and staying that way

**Mesh gradients**, **freeform / diffusion-curve gradients**, and a **magnetic / edge-snapping**
gradient annotator are each their own design surface. None is in S22, and none is a small addition
to it.

## 6. Owed a user visual / interactive pass

- The **elliptical minor-axis handle** and the circle/ellipse **outline overlay** (§2) are built and
  unit-tested; their on-screen weight — ring hairline vs. handle size, and whether the ring wants a
  dimmer treatment than the axis line — has never been looked at by a human. The analytic ring's
  own line weight (it is drawn from a smooth distance field with **no** supersampling, unlike the
  straight guides' 3×3) is part of that pass.
- The **layer-dock badge** (§8) — a framed ramp chip in the panel's one-ink idiom — is code-drawn
  like every other type mark and has not been seen on screen at 11 px on either theme.
- The **Dithering** control (§7): which kind should be the *default* is a look question, not a
  correctness one. It ships **None** so nothing existing moves; whether Blue noise deserves to be
  the default for new gradients is a call for a human looking at a real ramp.
- The `GradientFlyout` gaining **midpoint diamonds** on its preview strip, so blend curves are
  editable in the UI as well as in the model/serialisation (they render and persist today; the flyout
  UI for them is the remaining piece).
- General visual polish of the gizmo (handle sizes, hover cursors) and the four-shape feel.
- Elliptical is a *tool* shape, not a model type, so the surfaces that pick a `vec::Gradient` type
  directly — Edit ▸ Fill and the Gradient Overlay layer effect — still offer three kinds
  (linear/radial/conic). They author a gradient into a fixed content box rather than by dragging an
  axis, so there is no fourth handle for the aspect to hang off; the Gradient *tool* is the surface
  that authors ellipses.

---

## 7. Dithering (the "Dithering" context-bar control)

A smooth ramp is the classic case where 8-bit output bands: once a ramp is spread over more pixels
than it has quantisation steps, the eye reads the step boundaries as contour lines. The cure is
older than the problem is annoying — perturb the value by a fraction of one quantisation step before
it is quantised, so each band edge dissolves into texture.

### 7.1 Where the setting lives

**On the gradient paint, not on the tool.** `core::vec::Gradient` gains a `DitherKind dither`
alongside `SpreadMethod spread`, and follows that field's precedent exactly: it is authored from the
bar, carried by `buildGradient`/`retypeGradient`/`dragGradientHandle` through every edit, serialised
by `io/mosaic/docjson.cpp`, and re-read on load, so a reopened document renders identically. The one
difference from `spread` is that the JSON key is **optional**: it is written only when the kind is
not `None`, so every gradient authored before this serialises byte-identically and every older file
loads as `None`. An *unrecognised* token is a hard parse error, like every other enum in the format.

Evaluation rides `vec::sampleAt`, the single paint evaluator the CPU rasteriser, the layer-effects
overlays and the region fill all share — so the compositor, a Gradient Overlay effect and an
Edit ▸ Fill of the same paint agree by construction rather than by three copies staying in step.

**One API change was unavoidable.** Dithering is a *device-space* operation: its whole point is
scattering quantisation error across neighbouring **output** pixels, so the pattern has to be keyed
to the destination pixel. It cannot be derived from `sampleAt`'s `localPt`, because the callers do
not agree on what that means — the rasteriser passes layer-local px, while the layer-effects
renderer and the region fill pass the content box normalised to `[0,1]²` (that normalisation is what
makes a gradient span a shape, and it is correct). `sampleAt` therefore takes an optional
`SamplePixel` key. Callers that own a pixel grid pass theirs; the default ("no pixel") evaluates the
exact ramp, which is what point queries want — hit tests, the flyout's colour probes, SVG export.

### 7.2 The kinds, and why these

Four entries, three of them genuinely different images of the same ramp. A longer menu of near
duplicates would be worse than useless: it would ask the user to choose between things they cannot
tell apart.

| Kind | What it is | Why it earns a slot |
|---|---|---|
| **None** | the exact ramp | The default, and bit-identical to a pre-dither Mosaic: no existing golden image moves, and nothing about the old behaviour is reachable only through a setting. |
| **Ordered** | Bayer's recursive 8×8 threshold matrix | Deterministic, tiling, free, and *structured* — its fine cross-hatch is the recognisable "dithered" look, and on flat or synthetic artwork the regularity reads as intentional texture rather than dirt. |
| **Blue noise** | a 64×64 void-and-cluster threshold tile | The best-looking of the three: noise whose energy sits at high spatial frequencies, so the eye integrates it away instead of resolving it. No repeating figure, no chroma speckle, no grid. It is what you want on photographic work. |
| **Noise** | per-channel triangular-PDF white noise | Structureless and flat-spectrum, and *not* tiled — the only kind with no period at all. Coarser-looking than blue noise by design (that is film grain), and it reuses `common::ditherTPDF`, the formula already shipping in the sky renderer's own banding fix. |

Details that matter and are unit-tested:

- **Ordered and Blue noise use one threshold per pixel, shared by all four channels** — a
  luminance-only dither, the prepress convention, so a grey ramp gains no chroma speckle. **Noise**
  is independent per channel, which is what a TPDF dither is *for*.
- Each kind is **zero-mean over its tile**, so dithering moves band edges and never the ramp's
  average level.
- Offsets are in units of one 8-bit step: `[-0.5, 0.5)` for the two threshold kinds, `[-1, 1]` for
  the TPDF. The result is clamped to `[0,1]`, so a saturated end of a ramp cannot fizz.
- All four channels are perturbed, **alpha included** — the tool's own default ramp fades foreground
  → transparent, so its banding is entirely in alpha.

### 7.3 Error diffusion: dropped, and why

Floyd–Steinberg (and Jarvis, Stucki, Sierra, Riemersma) are deliberately **absent**. This was
considered as a post-pass over the rasterized region rather than faked as a point function, and
rejected on four independent grounds, any one of which is sufficient:

1. **It is not a point function.** A pixel's output depends on error pushed from its already-decided
   neighbours. It cannot ride `sampleAt`, which is the exact seam that makes the compositor, layer
   effects and region fill agree. Putting it anywhere else means the same gradient dithers
   differently depending on which surface drew it.
2. **There is nothing to diffuse against.** The gradient is evaluated into a **float** buffer and
   composited in float; the 8-bit quantisation that causes the banding happens much later, in the
   final composite (possibly on the GPU). Error diffusion is only meaningful *at* the quantisation
   step — diffusing residuals against a float target diffuses nothing. The three kinds we do ship
   are pre-quantisation perturbations, which is precisely why they survive whatever quantisation
   happens downstream, at any bit depth.
3. **It is serial, and the compositor is going tiled.** Error crosses pixels in scan order, so a
   tile boundary is a visible seam and a parallel or dirty-region re-render does not reproduce the
   previous frame. S60-a is moving the compositor to resident tiles; adopting an algorithm that
   cannot be tiled would be walking into that.
4. **It is not resolution-stable.** The same document re-rendered at a different zoom or export size
   gives a different diffusion pattern — fine for a one-shot print pipeline, wrong for a live
   non-destructive editor where the gradient is re-evaluated constantly.

If a future export pipeline ever wants error diffusion, its home is the **export quantiser** —
one place, one bit depth, one pass — not the paint model.

### 7.4 Lineage — the best-documented trail in this project

Halftoning is one of the best-documented public techniques in imaging, and every ingredient here is
decades old:

- **Ordered / Bayer matrix.** B. E. Bayer, *"An optimum method for two-level rendition of
  continuous-tone pictures"*, IEEE International Conference on Communications, **1973** — the
  recursive threshold matrix, published, and the textbook definition ever since.
- **Screen-cell tiling of a threshold array.** Holladay, **1978**.
- **Halftone screens as a rendering primitive.** Adobe PostScript, **1985** — `setscreen` /
  threshold-array halftones shipped as a public language feature forty years ago.
- **Noise-based dithering and its spectral analysis, incl. blue noise.** R. Ulichney, *Digital
  Halftoning*, MIT Press, **1987**.
- **The blue-noise tile generator we implement.** R. Ulichney, *"The void-and-cluster method for
  dither array generation"*, Proc. SPIE 1913, **1993** — published, and implemented here straight
  from the paper's description (a Gaussian-filtered occupancy field, remove the tightest cluster /
  fill the largest void, rank in three phases; on a torus with a symmetric kernel the paper's phases
  2 and 3 are the same operation, which is why the implementation runs them as one loop).
- **TPDF dither.** Summing two uniforms to get a triangular PDF is standard signal-processing
  practice and is already in the tree (`common/dither.hpp`, from the S55 sky renderer).

Nothing in this feature is designed from anything but the published sources above. As everywhere
else in these notes, nothing here asserts anything about what any third-party project does or does
not practise.

---

## 8. Two smaller S22 follow-ups

### 8.1 A gradient layer looks like a gradient layer in the dock

`docs/vector-model.md` §1's "one `LayerKind::Vector`" decision has a visible cost: a gradient layer
*is* a `VectorLayer` (a full-bleed rect whose fill is a `Gradient`), so the layer dock badged it with
the generic square-plus-circle "shapes" mark and it was indistinguishable from a rectangle. It now
gets `LayerRow::TypeBadge::Gradient` — a framed **ramp chip**, an 11 px swatch whose ink fades to the
row ground left to right. That follows the panel's existing badge machinery exactly (a code-drawn
one-ink mark riding after the name, sized through `typeBadgeWidth()`); it simply uses the one ink at
eight strengths instead of one, because a ramp is the only mark that says "colour ramp" at that size.
Like its siblings it is a placeholder for the real icon set (`docs/icons-needed.md`, S52).

### 8.2 The Shape tool stops binding gradient layers

The Shape tool's select-to-edit (`docs/vector-model.md` §7.1) bound **any** vector layer whose
geometry had a `ShapeKind` — and a gradient layer's geometry is a plain rect, so it bound those too.
That is not merely useless, it is destructive: the Shape bar carries no gradient control at all, and
its colour-swatch recolour (`recoloredObject`) would replace the gradient with a flat fill. It also
made it impossible to draw a shape *on top of* a full-bleed gradient layer, since the click always
landed on the gradient.

The split is now one pure predicate pair, `ui::gradientToolBinds` / `ui::shapeToolBinds`, keyed on
the **paint** rather than the geometry, and unit-tested in both directions: an object whose fill is a
`vec::Gradient` is the Gradient tool's, everything else with a `ShapeKind` is the Shape tool's. The
reverse direction was already correct and is now pinned by a test — the Gradient tool binds nothing
without a gradient fill, so a plain shape layer never captures its bar.

---

## 9. Automask to the selection

With an active selection, a gradient drag lays down a layer **already masked to it** — the same
"select, then apply" reflex `Filter ▸ Adjustments` has (`docs/adjustment-layers.md` §5), and the
same machinery: `core::maskFromSelection`, the resampling `Select ▸ Mask from Selection` uses.

- **Where it happens: `MainWindow::previewGradient`, on the frame the live layer is born.** A
  gradient layer is authored by the drag itself, so there is no menu item to hang the mask on; the
  preview layer *is* the eventual layer, inserted outside the command stack. Masking it at birth
  means **the drag preview shows the masked result from its first frame** — a preview that ignored
  the mask would lie about what release produces — and, because the mask lives on the layer, it
  rides into `commitGradient`'s `AddLayerCommand` for free: **one History step**, undo removes layer
  and mask together, redo brings both back, and an abandoned drag (`cancelGradient`) drops both.
  The mask is built **once per drag**, never per frame, so the live re-render path is exactly as
  cheap as it was (object + transform + opacity).
- **The placement is the load-bearing part.** A gradient layer is a **full-bleed `VectorLayer`**, so
  by the grid contract (`core/layer.hpp`, `RasterMask`) its mask sheet is the **document window**
  placed by `toLocal` = the inverse of the layer's world transform *captured at build time*. That
  transform is not the identity — the full-bleed rect is authored around the local origin and the
  layer transform translates it to the document centre — so the mask must be built **after**
  `setTransform`. Built there, `maskToDocument` is the identity, `maskFromSelection` takes its 1:1
  fast path and the sheet is the selection's coverage verbatim, feather and AA edge intact. Built
  before, the sheet is pinned half a document away from the pixels it was made from: the exact bug
  that erased three quadrants of every shape layer when masks first shipped.
- **The gate is `Selection::anySelected()` alone**, like adjustments: false for "no selection" (a
  reveal-all mask would be invisible in the composite but a lie in the dock) and false for an active
  selection of nothing (a gradient painting zero pixels reads as "the drag did nothing"). The
  selection is deliberately **left active** — the ants then read as "this is what got masked" — and
  the status bar says `Masked to the selection`, the adjustment automask's msgid verbatim.
- There is **no setting for this**: one answer is clearly correct, so it is simply the behaviour.
- Pinned headlessly in `tests/test_gradient_automask.cpp`: byte-identical to no gradient outside the
  selection, byte-identical to the unmasked gradient inside it, halfway under half coverage; the
  sheet's placement (including a case that shows what capturing it before `setTransform` would do);
  the single undo step in both directions; and the gate's two rejected states.
