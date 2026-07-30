# Document & layer model

Mosaic's core data model: a document is a **tree of layers**, and the on-screen image is
*derived* from it by the compositor (S7). The tree is the single source of truth — every edit
is non-destructive and re-playable (PLAN §3.7). The model (layers, document, geometry) landed in
S6-a; the command/undo system in S6-b ("Commands & undo" below).

Everything here lives in `mosaic::core` (`src/core/`) and is pure data/logic — **no GPU**, so it
is fully exercisable headlessly and unit-tested without a display.

## Layers

`Layer` (`core/layer.hpp`) is the polymorphic base. Every layer carries the shared chrome:

| Property | Notes |
|----------|-------|
| `id` (`LayerId`) | stable, per-document, minted monotonically; 0 is never valid. Commands & serialization refer to layers by id, not pointer. |
| `name` | display name. |
| `visible` | hidden layers are skipped by the compositor. |
| `opacity` | `[0,1]`, clamped by the setter. |
| `blendMode` | `core::BlendMode` (`blend_mode.hpp`); the compositor implements each (S7). |
| `transform` | `common::Affine2D` mapping layer space → document space. |
| `clipToBelow` | clip to the layer beneath (clipping mask). |
| `locked` | edit lock. |
| `mask` | optional `RasterMask` (8-bit coverage; `enabled` + `linked`, both wired in S31 — see the masks section below). Vector-geometry masks: a future S25-stack feature. |
| `parent` | the owning `GroupLayer` (or null for the root / a detached layer). |

Down-cast safely with `layer.as<RasterLayer>()` (RTTI is on, PLAN §3.1).

### Kinds

`LayerKind` = `Group · Raster · Vector · Text · Adjustment · Magic`. Several payloads are
intentionally thin in S6 and grow in their own sessions:

- **GroupLayer** — owns child layers (`std::unique_ptr<Layer>`), the recursion point of the
  tree; `expanded` is the panel's collapse state.
- **RasterLayer** — holds a `common::Image`. Pixel storage is 8-bit RGBA for now; it migrates
  to the float/tiled representation with the compositor and the `.mosaic` tile store (S7/S48).
- **VectorLayer** — placeholder; path/shape geometry + fills/strokes arrive in S25.
- **TextLayer** — holds a plain string; rich runs/font/AA model arrive in S29.
- **AdjustmentLayer** — non-destructive filter; `AdjustmentKind` + a `name→double` param bag,
  interpreted through the S32 **typed parameter schema** (`core/adjustments.hpp`: declared
  key/label/range/default per kind — the bag stays the storage, the schema is how the
  compositor, the Filter-menu insert, and the param editor agree on its meaning; see
  docs/adjustment-layers.md). Scoping ("affects layers below within the group, else
  globally downward") is a *compositor* behavior (S7) driven purely by tree position.
- **MagicLayer** — the linked/smart-source layer: keeps the **original full-resolution pixels**
  and a transform, resampling from source to minimize loss (resampling logic: S50). "Files
  opened as layers are magic layers" (S50).

### Layer masks (S31)

Every kind can carry an optional `RasterMask` — 8-bit coverage (255 = fully visible) that the
compositor multiplies into the layer's alpha (docs/compositor.md has the fold sites).

- **The mask grid is the layer's LOCAL grid.** A raster/magic layer's mask covers its **source
  image** (1 mask px per image px — the compositor's proportional fold is then the identity);
  every other kind's covers the **document window** (1 mask px per layer-local unit — exactly how
  group/vector/adjustment masks fold). `core::revealAllMask` builds the right grid per kind.
- **`linked`** (default true): the mask rides the layer's transform. **Unlinked**, the mask stays
  put in the layer's **parent space** while the pixels slide under it — the compositor folds it
  after placement instead of before. **`enabled`** false = the compositor ignores the mask but its
  pixels are kept (the non-destructive "view without the mask" toggle).
- **Selection ⇄ mask** (`core/selection.hpp`): `maskFromSelection` resamples the doc-space
  selection through `worldTransform(layer)` onto the mask grid, so the mask reveals exactly the
  document pixels the selection covered whatever the transform (Select ▸ Mask from Selection, the
  dock's Add Mask with an active selection); `selectionFromLayerMask` is the inverse gesture
  (Shift-click the mask thumbnail), sampling where the compositor folds — through the layer's
  transform when linked, the parent chain alone when unlinked.
- **Editing**: `Layer::maskRevision()` advances on every mask change (set/clear and, via
  `bumpMaskRevision()`, in-place coverage/flag mutation) — the layer panel's mask-thumbnail cache
  key. Mask painting (the Brush/Eraser mask lane, `ui/vulkan_canvas.cpp`) previews by mutating the
  coverage in place and lands one region-scoped `SetMaskPixelsCommand` per stroke.
- **Text layers refuse masks for now**: their pixels live in a renderer-filled cache whose grid
  moves with the text, so a raster mask glued to it would swim. (The compositor folds a text
  layer's mask over the cache grid for hostile/loaded documents; the UI just never offers one.)

### Child ordering (important)

`GroupLayer` children are ordered **bottom (index 0) → top (last)**: the compositor draws
front-to-back, and "an adjustment affects the layers below it" means the lower indices. The
layer panel (S10) displays this **reversed** (top of the stack at the top of the list).

Tree mutation primitives: `insert(index, layer)` / `addOnTop(layer)` (set parent) and
`removeAt(index)` (returns ownership, clears parent). These are what the undoable commands
(S6-b) and tests build on.

### Coverage partitions

Cutting a selection splits **one** surface into two layers along its coverage `m`: the lifted
fragment keeps `A·m`, the residual keeps `A·(1−m)`. Their sub-pixel coverages *tile* each pixel —
they are disjoint, not independent — but Porter-Duff `over` assumes independence and charges
`m·(1−m)` to an overlap that does not exist. Recombining the halves therefore yields `A − r·f`
instead of `A`: **up to 25 % of the alpha missing at `m = 0.5`**, which is the translucent rim a
feathered (or merely anti-aliased) cut shows when pasted back in place.

There is no `over`-based repair. Solving `a + b(1−a) = A` for either half forces `(1−a)(1−b) = 0`,
i.e. one edge must be hard — an aliased fragment, or a hole that leaves a full-opacity fringe of
the cut content behind. So the halves instead **record that they are a partition**
(`core::CoveragePartition`), and while that link is live the compositor rewrites the *lower* half's
alpha to

```
b = r / (1 − f_effective)
```

so plain `over` reconstructs the surface exactly. Because `over` is **associative**, this holds
wherever the fragment ends up above the hole — directly on top, several layers up, or nested in
pass-through groups; layers in between simply occlude the reconstructed surface as they would have
occluded the uncut original. `f_effective` folds the fragment's own opacity, any enclosing
pass-through group's, and a linked mask, so fading the piece slides continuously from "fully
reassembled" to "the bare soft hole" with no cliff.

The link carries no invalidation hooks: `partitionPairLive()` re-derives every condition from the
live tree, and liveness keys on the coverage's **fingerprint** (`RasterLayer::alphaFingerprint()`),
not on `contentRevision()` — undo restores byte-identical pixels through a *fresh* command, so a
counter comparison would leave the partition retired forever on the way back through history.

It retires — falling back to plain `over`, rim and all, which is then the honest answer — when
either half is repainted, moved relative to the other, placed off the integer pixel grid, given
layer effects or clip-to-below, or given a **blend mode**. That last one is a genuine limitation
rather than an oversight: alpha compositing is blend-independent, so the alpha *would* reconstruct,
but colour is not — the piece would blend against the filled hole in the feather band and against
the real backdrop in the fully-cut core, and that discontinuity draws its own ring (with Subtract,
a strikingly dark one). Trading an alpha rim for a colour ring is not a trade. The lower half
additionally must reach the composite unattenuated (full opacity, no mask), since it is the half
being rewritten.

Two paths deliberately do **not** reconstruct: isolated renders (thumbnails, Rasterize) show the
true soft hole the layer actually stores, because the rewrite is a compositing-time reading that
only means anything with the piece overhead. **Merge Down** is the exception among bakes — it
recombines the halves with the disjoint operator directly, since baking `over` there would freeze
the rim into the pixels permanently.

## Document

`Document` (`core/document.hpp`) is the canvas + tree root + color state + id allocator:

- **Canvas:** `width`/`height`, `dpi`. **Color (PLAN §3.6):** `ColorSpace` and `Precision`
  (`U8/U16/F16/F32`, **F16 default**). The lcms2-backed color management arrives in S12; these
  are the model-level enums set at creation (the new-document dialog is S9).
- **Tree:** `root()` is a `GroupLayer` holding the top-level layers.
- **Ids:** `mintLayerId()`.
- **Queries:** `find(id)` (whole tree, root excluded), `locate(id)` → `{parent, index}`,
  `layerCount()`.
- **Factories:** `makeRaster/makeGroup/makeVector/makeText/makeAdjustment/makeMagic` mint an id
  and return an un-inserted `unique_ptr<Derived>`; the caller inserts it (directly, or via an
  AddLayer command in S6-b).
- **State:** `title`, `filePath`, and a `dirty` flag (drives the tab close / save prompt, S49;
  commands set it, saving clears it).

## Geometry (`common/geometry.hpp`)

Shared 2D math (lives in `common` per PLAN §4, reused by the canvas viewport in S8): `Vec2`,
`Rect` (union/intersection/contains), and `Affine2D` — a 2×3 affine transform with
`translation/scaling/rotation/trs`, `apply` (point) vs `applyVector` (direction), composition
(`operator*`, matrix order), `inverse`, and `mapBounds` (axis-aligned bbox of a transformed
rect). Header-only and constexpr-friendly.

## Editing discipline

UI/tools must mutate documents **only through commands** (S6-b) so every change is undoable and
scriptable by the headless op-runner (PLAN §3.15). The direct setters/tree mutators here are the
building blocks those commands use (and what tests drive directly).

## Commands & undo (S6-b)

Every edit is a `Command` (`core/command.hpp`) with `apply(Document&)` / `undo(Document&)` that
must be exact inverses (and `apply` re-runnable, since redo calls it again). A `CommandStack`,
**owned by the `Document`** (`doc.commands()`), records them:

- `push(cmd)` applies it, marks the document **dirty**, and clears the redo branch.
- `undo()` / `redo()` walk the two stacks; `undoName()` / `redoName()` feed the menu labels.

### Coalescing

`push()` offers the new command to the current top via `Command::tryMergeWith(next)`. A command
returns true to **absorb** `next` so a continuous gesture (an opacity scrub, a transform drag)
becomes a single undo step. The convention: a coalescing command carries a `coalesceId`; two
commands merge iff they are the same concrete type, target the same `LayerId`, and share the
**same non-zero** `coalesceId` (one gesture). A tool uses one id per gesture; `0` never merges.
When merging, the top keeps its captured *old* value and adopts the incoming *new* value (which
is already applied to the document).

### Commands implemented

| Command | Notes |
|---------|-------|
| `AddLayerCommand` | inserts a pre-built layer at `(parentId, index)`; holds it in a `unique_ptr` while undone, preserving identity. |
| `RemoveLayerCommand` | captures `(parent, index)` on apply; undo re-inserts the same object. |
| `MoveLayerCommand` | reorder and/or reparent. **`newIndex` is the position among the destination's children *after* the moved layer is removed from its old location.** |
| `SetName` / `SetVisible` / `SetBlendMode` / `SetClipToBelow` | scalar setters; capture the old value on first apply. |
| `SetOpacity` / `SetTransform` | scalar setters that **coalesce** (carry a `coalesceId`). |
| `SetLayerMaskCommand` | set (or clear, with `nullopt`) a layer's whole raster mask in one step — Add/Delete Mask, Mask from Selection (S31); caller-supplied History label. |
| `SetMaskEnabledCommand` / `SetMaskLinkedCommand` | flag flips on an existing mask (S31); symmetric no-ops on a maskless layer. |
| `SetMaskPixelsCommand` | region-scoped coverage patch (the mask-paint stroke, S31); `dirtyRegion` maps mask px through the fold transform, so it rides the S60-a scoped recomposite. |
| `CompositeCommand` | bundles sub-commands (built un-applied via `add()`) into one undo step — the basis for multi-layer ops like Group (S10). |

Structural commands address layers by `LayerId` (resolved with `Document::find` / `locate` /
`groupById`, the last including the root), and move layer ownership in and out of the tree via
`GroupLayer::insert` / `removeAt`, so identity survives undo/redo. Coverage:
`tests/test_commands.cpp`. The headless op-runner (§3.15) will drive these same commands.
