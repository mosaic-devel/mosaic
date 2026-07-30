# Image operations (S53-a)

The Image menu's whole-document operations — **Canvas Size**, **Image Size**, the four
**orientation** ops, **Rotate Arbitrary**, and **Trim to Content** — plus the resampler they and
the Crop tool share.

Code: `src/render/resample.{hpp,cpp}` (kernels + samplers), `src/render/document_ops.{hpp,cpp}`
(the command builders), `render::buildDocumentRemapCommand` in `src/render/compositor.cpp` (the
shared engine), `core::SetGuidesCommand` and `core::Selection::{remapped,scaled}`.

## 1. One engine, not six

`render::buildCropCommand` already was a whole-document canvas-resize-with-anchor-and-rotate
engine: it emitted one `CompositeCommand` holding a `ResizeCanvasCommand`, a per-layer transform
rebase (pushed *into* unmasked groups, conjugated, so nothing resamples that did not have to),
an optional delete-mode pixel bake, the expansion fill, and the selection crop. Its `x`/`y` were
allowed to be negative and its `w`/`h` to exceed the old canvas, so "canvas resize with an anchor"
was already inside it.

So S53-a **generalised** it rather than duplicating it. `buildDocumentRemapCommand(doc, newW,
newH, worldToNew, deletePixels, fill, label, bakeFilter)` is the same function with "the crop
shift" replaced by "any invertible affine", and `buildCropCommand` is now a four-line wrapper that
builds the un-rotate-then-shift matrix and calls it. Every document operation therefore inherits,
for free and identically:

* the group push-down and the masked/singular-group rule (a linked group mask lives in group-local
  space, so a masked group rebases as a unit);
* the canvas-locked `TextureLayer` rule (a procedural fill re-anchors to the new canvas instead of
  sliding, and its document-window mask is remapped with it);
* the delete-mode bake, which flattens an eligible raster's transform to identity;
* the expansion fill's two routes — the byte-exact extend-in-place Background blit, and the
  fallback bottom fill layer for every other stack shape;
* the single-undo-step guarantee, and `dirtyRegion() == nullopt` (whole document), which is the
  correct answer for all of them.

Duplicating any of that would have meant six copies of five special cases drifting apart. The cost
of generalising is one indirection and a handful of predicates on `worldToNew`
(`isIntegerTranslation`, `isGridWindow`, `isOriginScale`) that decide which fast path is honest.

## 2. The anchor model and its rounding rule

`canvasRectFor(oldW, oldH, newW, newH, anchor)` returns **where the old canvas lands inside the new
one**: `{x, y}` is the old canvas's top-left in NEW-canvas coordinates and `{w, h}` is the old
size. Growing gives a non-negative offset, shrinking a negative one. The document remap is exactly
`translation(x, y)`, and the equivalent crop window in old coordinates is `(-x, -y, newW, newH)`.
The Canvas Size dialog's preview frames the same rectangle, which is why the function is pure and
lives in the header rather than inside the command builder.

The enum is row-major from the top-left, so `int(anchor) % 3` is the column and `/ 3` the row.

**Rounding.** The centred axes compute `(newW - oldW) / 2` in integer arithmetic, i.e. truncation
**toward zero**. An odd difference therefore always puts the extra pixel on the **right / bottom**,
and — this is the point — it does so *in both directions*: growing 100 → 101 adds the column on the
right, and shrinking 101 → 100 removes it from the right. Flooring would have been asymmetric
(growing biases right, shrinking biases left), which is the kind of thing that shows up as a
one-pixel drift when a user nudges a size up and back down. Pinned by
`tests/test_document_ops.cpp`, "canvasRectFor centres an odd difference toward the right and
bottom, both directions".

## 3. Lossless orientation, and why the filter must not reach it

`buildOrientCommand` composes a **signed axis permutation with an integer translation**:

| op | remap `(x, y) →` | canvas |
|---|---|---|
| Rotate 90 CW  | `(H − y, x)` | w/h swapped |
| Rotate 90 CCW | `(y, W − x)` | w/h swapped |
| Rotate 180    | `(W − x, H − y)` | unchanged |
| Flip Horizontal | `(W − x, y)` | unchanged |
| Flip Vertical   | `(x, H − y)` | unchanged |

Every one satisfies `render::isLosslessGrid`, so `chooseAutoFilter` resolves the placement to
`Nearest`: each destination texel's centre inverse-maps onto a source texel *centre*, and the
result is byte-identical to a pure index permutation however many times the op is applied. That is
why `buildOrientCommand` is non-destructive (nothing is baked — there would be nothing to gain) and
still exact.

**The filter must never reach it.** `resolveFilter` deliberately honours an *explicit* kernel
whatever the placement — the user's pick is not second-guessed — so handing an orientation the
UI's current resample quality (say Lanczos3) would silently convolve a 90° turn and blur it.
`buildOrientCommand` therefore takes **no `ResampleFilter` parameter at all**: the type system,
not a comment, is what keeps the UI's setting out of it.

The selection follows as an exact index permutation (`Selection::remapped`) rather than being
cleared: it is a document-sized coverage sheet, and a grid permutation moves whole bytes. Guides
swap axes (a row becomes a column under a quarter turn). No fill is ever needed, because the turned
canvas covers itself exactly.

## 4. The guides rebase (a pre-existing bug)

Guides are stored per document in **document pixels** (`core::Guide`). Until S53-a *nothing*
rebased them: every crop resized the canvas and moved every layer, and left the guides sitting at
their old coordinates — so a guide the user had aligned to a feature ended up somewhere else, or
off-canvas entirely. That was a plain bug, not a deliberate limitation.

The fix lives in the shared engine, so the Crop tool inherits it. `remapGuides` maps a guide
through `worldToNew`:

* **axis-preserving** remaps (translate / scale / flip) keep each guide's orientation and map its
  single coordinate;
* **axis-swapping** remaps (a 90° turn) exchange horizontal and vertical;
* anything else — an arbitrary rotation — would turn a guide into a slanted line, which the model
  cannot represent, so those guides are **dropped** rather than silently left behind at stale
  coordinates.

A guide pushed outside the new canvas is dropped too. Ids are preserved, and the whole list is
replaced through the new `core::SetGuidesCommand` (capture-old-on-first-apply, mirroring
`ClearGuidesCommand`), so undo restores every dropped guide exactly.

Note that a 90° *crop* (`buildCropCommand` with `angle = π/2`) is **not** an axis-swapping remap in
this sense: its matrix comes from `std::cos`/`std::sin`, so `m00` is 6e-17 rather than 0 and the
guides are dropped. That is the honest answer for a floating-point rotation; the exact quarter-turn
lives in `buildOrientCommand`, which builds its matrix from integers.

## 5. Image Size: what resamples and what does not

The decision: **plain raster pixels are physically resampled; everything else rides its
transform.** Concretely, `buildImageResizeCommand` runs the engine with `deletePixels = true` and
the caller's kernel as the bake filter, which splits the tree exactly the way the Crop tool's
"Delete Cropped Pixels" already splits it:

* an **unmasked `RasterLayer` under an all-identity ancestor chain** is resampled to the new
  resolution through the chosen kernel and lands at the identity transform;
* **everything else** — masked rasters, `MagicLayer`s (whose entire point is keeping the
  full-resolution source), vector / text / texture layers, and anything under a transformed
  ancestor — takes the scale into its affine and stays resolution-independent.

The conservative alternative was to carry *every* layer by transform and resample nothing. It was
rejected for two reasons. First, the dialog has a resample-quality dropdown; with a transform-only
scale that dropdown would control nothing, because the actual resampling would then happen later,
at composite time, with whatever `CompositeOptions::resampleFilter` the canvas happened to be using
— a setting the user was not thinking about when they picked "Lanczos 3" in the Image Size dialog.
Second, a transform-carried scale re-resamples the layer on *every* composite, from the original
pixels, forever; baking once is both faster and what "resize the image" means.

The consequences, stated out loud:

* a physically resampled raster is **clipped to the new canvas** (the same boundary Merge Down and
  "Delete Cropped Pixels" already accept);
* the scale is lossy in the ordinary raster sense — undo restores the pixels, but scaling down and
  back up does not;
* a document that mixes rasters and vectors ends up half-baked and half-transform-scaled. That is
  correct: the vector really is resolution-independent and re-rendering it at the new size is
  strictly better than resampling its rasterisation.

**Selection.** It scales with the document through `core::Selection::scaled(newW, newH)`, which is
new. It deliberately takes **no `ResampleFilter`**: that enum lives in `render`, and `render`
depends on `core`, not the other way round — putting it in `Selection`'s signature would invert the
module dependency for the sake of one argument. The kernel is fixed and appropriate: a footprint
one destination pixel wide in source units, so growing is linear interpolation (an anti-aliased
edge stays smooth) and shrinking is a box average (a thin selected sliver keeps proportional
coverage instead of being point-sampled away). Edge taps clamp, so a selection touching the canvas
border does not gain a soft fringe from outside it. A coverage mask does not want a ringing kernel.

Guides scale with the document too (the axis-preserving branch of §4).

## 6. Rotate Arbitrary and Trim to Content

**Rotate Arbitrary** turns the document about the canvas centre and grows the canvas to the
rotated bounding box, rounded out to whole pixels (the sub-pixel slack sits at the right/bottom
edge). An arbitrary angle genuinely resamples, so it bakes through the caller's filter on the same
split as Image Size, and `fill` paints the newly exposed corner wedges through the very same
`fillExpansion` the rotated crop uses. The selection is **cleared** — its pixel geometry does not
survive the resample, the rule S16-f already settled for rotated crops — and guides are dropped
(§4). A whole number of turns is a no-op and returns `nullptr`.

**Trim to Content** unions every *visible* layer's `contentBounds()` mapped through its ancestor
chain into document space, rounds out to whole pixels, and clamps to the current canvas: Trim only
ever **shrinks**. Content spilling past a canvas edge is not a reason to grow — that is what Canvas
Size is for. Nothing visible, or a box that already is the canvas, returns `nullptr`.

## 7. The resampler extraction

`src/render/resample.{hpp,cpp}` is the compositor's kernel bank and sampling passes, moved verbatim
out of `compositor.cpp`'s anonymous namespace so Image Size, Rotate, the export resize stage and
the tests can reach them. Nothing about the math changed: same formulas, same constants, same
premultiplied accumulation in `double`, same weight-sum normalisation, same `parallelFor` split.
`compositor.cpp` keeps `rasteriseLayerInto` — the fused leaf pass that converts 8-bit → float
*through* the transform with the layer's mask folded at the sampled position — because that one is
the compositor's own, not a general image operation.

The public surface is `kernelRadius`, `kernelWeight`, `cubicKernel`, `chooseAutoFilter`,
`resolveFilter`, `isLosslessGrid`, the `Fetch`-templated passes (`bilinearPremul`, `convolveInto`,
`supersampleInto`, `resampleInto`), and four whole-image entry points: `transformImage(F)` (place a
source through an affine) and `resampleImage(F)` (scale it to a target size). `compositor.hpp`
includes `resample.hpp`, so every existing caller of `chooseAutoFilter` / `cubicKernel` compiles
unchanged.

`shaders/composite_tile.comp` mirrors these kernels line for line and
`tests/test_composite_tile_parity.cpp` holds the two lanes to each other — a change to a formula
here must land in both.

## 8. Known limit: `kMaxFootprintRadius = 8.0`

`convolveInto` widens each kernel's source-space support by the minification factor so a reduction
low-passes instead of aliasing — and then **caps** the resulting radius at 8 source texels. Without
the cap a heavy shrink costs `radius × reduction` taps per pixel per axis: a 40× reduction with
Lanczos3 would be 120 texels of support, i.e. tens of thousands of taps per output pixel, which is
a multi-second (on a big canvas, multi-minute) freeze on a commit.

The visible consequence is that an **extreme** minification aliases slightly: source detail beyond
8 texels of a destination pixel's centre simply does not contribute. `tests/test_resample.cpp`
pins this behaviourally — at 40×, a bright band 100 source columns away from the first destination
pixel contributes exactly nothing, while one inside the capped window contributes everything.

The proper fix is mip-style pre-downsampling (successive halvings until the remaining reduction is
within the cap, then the real kernel), which is a follow-up, not a slice of S53-a. Until then, the
practical guidance is that reductions past ~8× per step are better done in two passes.
