# Mesh Warp and Perspective Warp (S35-b)

*Built 2026-07-29. `ToolId::MeshWarp` and `ToolId::PerspectiveWarp`, shortcut **Q**, sharing
`ToolSlot::Warp` in the `SelectTransform` group, immediately after Crop.*

---

## 1. The model, in one paragraph

**A warp deforms one layer's pixels through a hand-authored lattice, and nothing about the lattice is
derived from the image.** Mesh Warp lays a `cols × rows` grid of control points over the layer's
content and interpolates a **Catmull–Rom** surface through them; Perspective Warp puts four corner
handles on it and solves the single **homography** that carries them where they were dragged. Both
run **inverse-mapped** — for each *destination* pixel, find its source coordinate and sample it — and
both resample through the compositor's own kernel bank. Apply bakes the deformed pixels into the
layer as one undo step and **stores the grid on the layer**, so re-entering the tool finds the
handles where they were left.

---

## 2. Where the code lives

| Piece | Lives in |
| --- | --- |
| The model: `WarpKind`, `WarpGrid`, the layer's slot + `warpRevision()` | `src/core/layer.{hpp,cpp}` (`mosaic::core`) |
| The kernel: the Catmull–Rom surface, the triangle walk, the homography | `src/render/warp.{hpp,cpp}` (`mosaic::render`) |
| The undoable edit | `core::SetLayerWarpCommand` (`src/core/commands.{hpp,cpp}`) |
| Options→values, the handle hit-test, the drag, the drawn grid | `src/ui/warp_gesture.{hpp,cpp}` (FLTK-free) |
| Tool registration + the option set | `src/ui/tool.{hpp,cpp}` |
| Icon key | `src/ui/icon_pack.cpp` (`iconKeyFor`) |
| The gesture: binding, the live preview, the overlay, the cursor | `src/ui/vulkan_canvas.{hpp,cpp}` |
| Host wiring (the active layer, the command, the refusals) | `src/ui/app_window.cpp` |
| Persistence | `src/io/mosaic/docjson.cpp` + `docio.cpp` (`"warp"`) |
| Tests | `tests/test_warp.cpp` |

The split is the one every tool since S26 uses: pure maths in `render`/`core`, pure gesture maths in a
`*_gesture` unit, pointer handling in the canvas, and everything that needs the document in the host.

---

## 3. The kernel

### 3.1 Inverse mapping, always

Every path walks the **destination** and asks *where did this pixel come from*, never the source
asking *where does this pixel go*. A forward walk scatters: a magnification leaves the gaps between
landed pixels empty and a reduction piles several sources onto one destination, so a forward warp
needs a hole-filling pass afterwards and still cannot low-pass. The inverse walk has neither problem
— it is O(output), it covers every output pixel exactly once, and it hands each one a source
*coordinate*, which is precisely what a reconstruction kernel wants.

### 3.2 Mesh — a subdivided Catmull–Rom surface, rasterised as triangles

The lattice defines a piecewise Catmull–Rom surface, so the deformation is **C¹ across patch
boundaries** instead of creasing at every control point. `warpSurfacePoint(g, u, v)` evaluates it at
lattice parameter `(u, v)`, `u ∈ [0, cols-1]`, `v ∈ [0, rows-1]`, with one ring of phantom control
points extrapolated as `P[-1] = 2·P[0] − P[1]` so an edge patch bends like its neighbours instead of
flattening.

A useful consequence: a **2-point axis degenerates exactly to linear interpolation** (the phantom
extrapolation cancels the quadratic and cubic terms), so a 2×2 mesh is a plain bilinear quad. That is
also why Perspective is a separate engine and not "a 2×2 mesh" — see §3.3.

The walk is:

1. evaluate the surface **once for the whole lattice** into a fine vertex grid of
   `(cols-1)·steps + 1` by `(rows-1)·steps + 1` vertices, each carrying a destination position *and*
   a source position;
2. split every fine cell into two triangles and rasterise them, interpolating the source coordinate
   **barycentrically**.

Two things that look like details and are not:

* **The vertex grid is computed for the whole surface, not per patch.** Two adjacent patches
  evaluating their shared boundary independently agree only to the last bit, and that draws a
  hairline seam down every patch edge. Sharing the vertex entry makes the seam impossible rather
  than unlikely.
* **A triangle's destination→source map is exactly an affine** (barycentric interpolation of three
  points), so the inverse comes for free, the walk cannot leave holes, and the affine's *column
  lengths* are the exact source-texels-per-destination-texel figures the footprint widening needs
  (§3.4) — per triangle, measured rather than guessed.

Cells are small enough that an affine over each half follows the surface to well under a pixel: the
crease a triangulation costs sits at the *subdivision* scale, not at the patch scale, which is the
whole reason the surface is subdivided before it is rasterised.

The two triangles sharing a cell diagonal both claim the pixels **on** that diagonal (an inclusive
barycentric test with a hair of tolerance). The duplicated write is harmless — both sides agree about
the source there — while a crack is a visible defect.

A **fold** (the mesh dragged over itself) is not refused: later triangles simply overwrite earlier
ones. That is the honest picture of what the lattice says, and unlike a projective fold it has no
horizon behind which the result becomes a ghost.

### 3.3 Perspective — one homography, and never a triangulated one

`solveHomography(from, to)` builds the 8×8 linear system in the eight free coefficients (with
`m[8] = 1`) from the four corner correspondences and solves it by **Gaussian elimination with partial
pivoting**. The pivoting is not decoration: the rows mix plain coordinates with coordinate
*products*, so on a large canvas the columns differ by four orders of magnitude and an unpivoted
elimination loses the small ones. Four points make the system square, so it is solved directly rather
than least-squared through an SVD.

Each destination pixel is then inverse-mapped through `H⁻¹` with the proper **per-pixel `w` divide**.

**A homography is never triangulated.** A projective map is not affine, so splitting the quad into
two triangles replaces it with two different affines that agree only along the shared diagonal — the
classic visible-diagonal-crease implementation, and the single most common way this feature is got
wrong.

Two refusals, both returning `ok == false` so the gesture can decline rather than emit garbage:

* a **non-convex or degenerate** quad (`convexQuad`) — a projective map onto one folds the plane back
  over itself;
* a **singular** system (the pivot falls below tolerance), which is what a collapsed or collinear
  quad produces.

Two further guards inside the walk:

* the homogeneous divisor's **sign** is compared against its value at the quad's centre; a pixel on
  the far side of the map's horizon line has no honest pre-image, and drawing it anyway is the
  mirrored ghost a naive homography shows beyond the vanishing line;
* a destination pixel whose pre-image lands outside the **source** quad (plus one pixel of slack) is
  skipped, so a re-edit — where the source quad is a sub-region of the image rather than the whole of
  it — cannot drag unrelated content into the bounding box's corners.

### 3.4 Sampling: the compositor's kernels, in premultiplied alpha

Nothing new is invented here. `render/resample.hpp`'s kernel bank — `kernelRadius`, `kernelWeight`,
`bilinearPremul`, `kMaxFootprintRadius` — is the one the compositor and
`shaders/composite_tile.comp` share, and it was extracted to a public header in S53-a precisely so
callers like this one can use it. A second kernel bank in the tree would be a second thing to keep in
step with the shader.

`warp.cpp` supplies only the *per-point* form of `convolveInto`'s accumulation
(`sampleKernelPremul`), because a warp has no single inverse affine to hand `convolveInto`: the map
varies per triangle (mesh) or per pixel (perspective). The taps, the weights, the `double`
accumulation, the weight-sum normalisation and the un-premultiply-once are the same lines.

**Alpha is premultiplied before filtering and un-premultiplied after.** Filtering straight RGBA
bleeds a transparent texel's arbitrary RGB into its covered neighbours, and every stretched edge
picks up a dark halo. Sampling outside the source reads **transparent**, not clamped: the area
outside a layer genuinely is nothing (`render::EdgeMode::Transparent`, the compositor's own layer
policy), and clamping there would grow a border of invented colour wherever the warp pulls content
past the edge.

The **footprint widens with the local minification** so a squeezed region low-passes instead of
aliasing — from the triangle's own affine columns in the mesh path, and from the analytic Jacobian of
`H⁻¹` in the perspective path (a projective map's scale changes across the quad, so one constant
figure would be wrong at both ends of a strong perspective).

### 3.5 Known limit: `kMaxFootprintRadius = 8.0` is inherited

This path samples through the same capped footprint `docs/image-operations.md` §8 records, and it
inherits the same behaviour: `convolveInto`'s widening is capped at **8 source texels**, so an
**extreme** local minification — a mesh region squeezed past roughly 8× — aliases slightly, because
source detail beyond 8 texels of a destination pixel's centre does not contribute. The cap is what
keeps a heavy squeeze from taking tens of thousands of taps per output pixel. The proper fix is
mip-style pre-downsampling, and it is the same follow-up there as here.

Two further caps of this file's own:

* the output extent is refused past **1 << 28 px** (`core::kMaxLayerCells`'s number): a warp can grow
  its own extent without limit, so the guard is real, not a formality;
* Rows / Columns are capped at **12** per axis (`ui::kWarpMaxNodes`). That is a budget as well as
  taste: the overlay draws every isoline through the shared content-keyed polyline lane, whose SSBO
  holds `render::kLassoMaxVerts` vertices.

### 3.6 Auto, and the draft/final split

A warp has no single affine to hand `chooseAutoFilter`, so `ResampleFilter::Auto` is bucketed the
same way from the overall lattice area change: an identity deformation is lossless (**Nearest**, which
also keeps pixel art crisp), a reduction box-averages (**Area**, no ringing), an enlarge or a bend
takes the sharp kernel (**Lanczos3**).

`WarpQuality` is the *same algorithm* at two settings, not two code paths:

| | patch subdivision | kernel |
| --- | --- | --- |
| `Draft` (a live handle drag) | 4 steps per patch edge | forced to Bilinear (Nearest survives) |
| `Final` (release, Apply) | 12 steps per patch edge | the user's choice, Auto resolved |

That is the compositor's own `liveDrag`/commit split, in a tool that has to re-warp real pixels
sixty times a second.

---

## 4. The tool, the options bar, and the gesture

Two `ToolId`s share `ToolSlot::Warp` with a flyout, exactly the Marquee / Lasso / Shape variant
precedent. **Q** is the shortcut, and it is genuinely free: W belongs to the Magic Wand, A to the
Select Brush, R to the canvas's own rotate gesture and P to the Pen, so the word *warp* offers no
letter of its own. H and O are deliberately left for the tools whose names do start with them and
whose art the icon pack already reserves (`hand`, `dodge`/`burn`).

The options bar, with the Crop tool's precedent verbatim for the actions:

| Option | Kind | Notes |
| --- | --- | --- |
| Rows | `Number` | 2…12. **Disabled** under Perspective |
| Columns | `Number` | 2…12. **Disabled** under Perspective |
| Quality | `Choice` | the Move tool's "Anti-aliasing" list verbatim, Auto first |
| Show grid | `Toggle` | the handles stay grabbable either way |
| Apply | `Button`, `Affirmative` | Enter does the same |
| Cancel | `Button`, `None` | Esc does the same |

Rows and Columns are *meaningless* for Perspective — one homography has four corners and no interior
control points — so they are built and **deactivated** there rather than shown as controls that lie
about what they do.

### 4.1 The overlay

* The mesh grid is drawn as **smooth curves**, sampling the *same* `warpSurfacePoint` the pixels ride
  (~12 samples per patch edge). Straight chords between control points would draw a grid that does
  not bend the way the image does, which is the one thing a warp overlay exists to show.
* The **outer boundary reads a touch heavier** than the interior lines. The shared lane draws exactly
  one line weight and the release overlay channel budget is 12, so a second weight cannot be bought
  with a new descriptor binding: instead the boundary is emitted twice more, 0.4 px either side of
  its own normal (`ui::thickenPolyline`). Two lines a fraction of a pixel apart read as one thicker
  line. The offset is in **screen** space, so the extra weight does not scale with the zoom.
* Control points are the **Pen tool's own anchor squares** (`WindowRenderer::PenMark` kind 0), so the
  app has one handle vocabulary: hollow when idle, accent-filled on hover and during a drag.
* For Perspective only the four corners are handles, with the two **diagonals drawn faint** as the
  perspective read.
* Everything rides the **existing** content-keyed polyline lane and the pen chrome lane. No new
  descriptor binding: the release overlay channel budget is 12, and this file's history says a new
  binding is also where a use-after-free lives.
* The hit radius is in **screen px** (`kWarpHandleHitPx = 9`), so handles stay grabbable at every
  zoom. A document-space radius would be ungrabbable zoomed out and absurdly sticky zoomed in.

### 4.2 Modifiers

* **Shift** constrains a handle drag to one axis — whichever the drag has travelled further along,
  re-decided as it grows, so a drag that turns a corner follows the turn.
* **Alt** drags the whole lattice rigidly, which is how a warped region is repositioned without
  touching its shape.

Nothing else. A modifier nobody can justify is a modifier nobody can discover.

### 4.3 The live pixel preview

A handle drag writes the deformed pixels **straight into the bound layer** and re-places it, exactly
as a brush stroke previews by painting into the live image, and `cancelWarpSession()` puts the
pre-warp pixels and placement back. Three rules make that safe:

1. **Every bake starts from the session's pristine base** (`m_warpBase`, captured at bind time), so a
   drag can never compound its own resampling however many frames it runs for.
2. **The bake is frame-coalesced.** `FL_DRAG` only records; `flushWarpDrag()` runs one Draft bake per
   frame tick. Baking per motion event is exactly what made the S15 Move drag lag behind the pointer.
3. **The release pays for the real kernel** — one Final bake — and Apply bakes once more from the
   base, so the committed pixels are never whatever a draft frame happened to leave behind.

**Esc** (and the bar's Cancel) puts the handles back where the layer's *stored* warp left them and
the preview pixels with them — it re-frames, it does not leave the tool, which is exactly what Esc on
the Crop tool does. **Leaving the tool discards a staged deformation**, because the preview was never
in the document: nothing was committed, so there is nothing to keep. That differs from the Crop tool
on purpose — a staged crop rect is a cheap piece of geometry worth remembering, while a staged warp is
a rendered image of the layer, and keeping one alive across tools would mean the document on screen
disagreed with the document in the command stack.

### 4.4 Coordinates

Every grid in the gesture lives in the bound layer's **base-local** pixel space — the space
`m_warpBase` is indexed in — and never in the layer's *current* local space. The two differ the
moment a preview bake lands, because a bake re-homes the pixel origin and post-translates the
transform to absorb it. `m_warpBaseWorld` is captured once at bind time and is the only
document↔grid map any of it uses.

---

## 5. What gets warped, and the honest limit

The lattice frames the layer's **content bounds**, not its (usually document-sized) pixel grid:
handles parked in a transparent margin are handles on nothing, and `contentBounds()` is the same box
the Move tool's gizmo frames, so the two tools agree about where the layer is.

Warping **bakes**. `render::warpImage`'s output is tight to the deformed content, so the layer's
image is replaced at a new extent and its transform is post-translated by the reported offset — not
one pixel already on the canvas moves. Which means:

* **Raster and Magic layers** are the two kinds that can be warped: their pixels *are* the layer
  (`RasterLayer::image()`, `MagicLayer::source()`).
* **Repeated warps compound resampling loss.** Within one editing session there is none at all — every
  bake reads the pristine base — but leaving the tool and coming back gives the next session a base
  that has already been resampled once. That is the honest limit of a design that does not add a warp
  stage to the compositor, and it is stated here rather than glossed.
* **The grid is stored on the layer regardless.** Re-entering the tool restores the handles where the
  user left them, and `render::warpImage`'s two-grid form then applies exactly the **difference**
  between the stored grid and the edited one — the only reading under which nudging one handle after
  re-entry does what it looks like it does, instead of applying the stored displacement a second
  time. That is the re-editability the PLAN asks for, without compositor surgery.

### 5.1 The named follow-up

The fully non-destructive route is a **warp stage in the compositor**, folded where the mask already
is: the layer's stored grid would deform its source at sampling time and the pixels would never be
baked at all. For **Magic layers** that is the natural home — the compositor already resamples a
magic layer from `source()` every frame, so a compositor-side warp of one is lossless however many
times it is edited. It is a separate slice: it touches the CPU walk, `shaders/composite_tile.comp`,
the region/extent machinery and the resident tile lane, all of which are S60's ground.

### 5.2 The refusals

Six specific messages, each naming the way forward — the S36 rule, where four specific refusals
replaced one generic one. A tool that says "cannot warp that" has told the user nothing they could
not already see.

| Situation | The message says |
| --- | --- |
| No active layer | pick a layer; the Layers panel's active row is what gets warped |
| Locked layer | unlock it (Layer ▸ Lock Layer) |
| Group | no pixels of its own — warp the layers inside it |
| Adjustment | no pixel grid — it is re-evaluated over whatever is beneath it |
| Vector | **Rasterize** the shape first (Layer ▸ Rasterize) |
| Text / Texture | **Rasterize** first: those pixels are a cache re-drawn from the block / regenerated from the parameters, so a warp of them would not survive |
| A layer with an enabled mask | apply or delete the mask first |
| A perspective quad folded inside-out | drag the corners back into a convex shape |

The **mask** refusal is a limit, not a principle: a mask sheet rides a raster layer's pixel grid 1:1,
so deforming the pixels without deforming the coverage would slide the mask off what it was masking.
Warping the mask with them is a follow-up (the same grid produces the same extent and offset, so it is
mechanically straightforward); refusing is the honest answer until it lands.

---

## 6. The command

`core::SetLayerWarpCommand` carries **six** values — old and new pixels, old and new placement, old
and new grid — because a warp is three things at once and undo has to reverse all three together or
the layer comes back wrong. It stores them rather than trying to invert the deformation, which in
general has no inverse at all.

**Coalescing is deliberately absent.** One Apply is one history entry. A warp is a framed, accepted
act, not a continuous scrub, and merging two of them would mean the intermediate state — which the
user looked at and accepted — could never be returned to.

`dirtyRegion` is `nullopt` on purpose (`GrowAndPaintLayerCommand`'s reasoning): a warp moves the
layer's *extent* and its *placement*, so the region needing a recomposite is wherever the layer used
to be plus wherever it now is. The caller's whole-document fallback is the correct answer.

---

## 7. Persistence

An additive, optional per-layer `"warp"` node in the `.mosaic` manifest. The **manifest schema
version stays 1** — it is purely additive — and the reader is **strict**: absent is fine,
present-but-malformed refuses the file. That is the established rule for the per-layer `"exif"` node,
and this node copies its shape.

```json
"warp": {
  "kind": "mesh",                 // or "perspective"
  "cols": 4, "rows": 4,
  "src": [0, 0, 512, 512],        // the framed source rect: x, y, w, h
  "pts": [[0, 0], [170.7, 0]]     // rows*cols entries, ROW-MAJOR, each [x, y]
}
```

`src` is a flat 4-array and each point is a 2-array, matching the manifest's existing
`vec2ToJson` spelling — the format has one way to write a point and this node does not invent a
second.

`pts` must hold exactly `cols * rows` entries, `cols`/`rows` must be within 2…12, `perspective`
must be exactly 2×2, and `src` must be non-degenerate. Anything else is a malformed node and the
load is refused — a native format that guesses is a native format that corrupts.

---

## 8. Lineage, and what these tools deliberately do not do

What ships is old published mathematics applied by hand.

**The lineage, by name and date.**

* **Catmull & Rom, "A class of local interpolating splines", 1974** — the spline the mesh surface is
  built from. It is also already in this tree, interpolating brush strokes
  (`core/brush/stroke_path.cpp`).
* **Heckbert, *Fundamentals of Texture Mapping and Image Warping*, UC Berkeley, 1989** — the inverse
  mapping discipline, the reconstruction-filter treatment, and the reason a forward map is the wrong
  shape for this problem.
* **Wolberg, *Digital Image Warping*, IEEE Computer Society Press, 1990** — the mesh-warp literature
  proper: control-lattice deformation, the separable two-pass formulation, and the standard
  treatment of holes, folds and antialiasing in a warp.
* **The projective transform and the 4-point solve** — 19th-century projective geometry; the modern
  exposition is **Hartley & Zisserman, *Multiple View Geometry in Computer Vision*, 2000/2004**. The
  8×8 linear system, its normalisation and its degeneracies are textbook there.

Nothing above is novel.

**Technique families Mosaic declines to build.** These are design exclusions, not commentary on
anyone else's product, and each one is a hard boundary on these tools:

1. **Any automatic plane, vanishing-point or camera estimation derived from image content.** The
   quad's four corners come from the user's hands and from nowhere else. Nothing in this tool reads
   the image to decide where a plane is, where the horizon is, or what the focal length was.
2. **A multi-quad *linked* perspective network that jointly solves several planes** — several quads
   sharing edges, solved together so that dragging one re-solves its neighbours to keep a building's
   faces consistent. What ships is a **single-quad homography**. A second quad would be a second
   independent warp of a second layer, never a joint solve.
3. **Pin / puppet deformation over a triangulated mesh with automatic rigidity weighting** — pins
   placed on content, a triangulation derived from it, and a weighted as-rigid-as-possible solve
   distributing the pull. What ships is a **hand-authored lattice** whose control points the user
   moves individually; the subdivision is uniform and the interpolation is a fixed spline basis with
   no weighting derived from anything.
4. **Liquify-style push / bloat / pucker / twirl brushes** — a brushed displacement field. That is a
   separate PLAN backlog item and it is not reachable from this tool; the option bar carries no
   control that could motivate one.

What ships is a single-quad homography and a hand-authored mesh. **Nothing is derived from the
image.**

---

## 9. Owed

* **A visual pass.** The lattice's line weights, the boundary's doubling offset, the handle size
  against the Pen's, and the faint diagonals under Perspective are all first-cut numbers.
* **A `warp_perspective` icon.** Both variants currently wear the pack's reserved `warp` art, so a
  flyout row is told apart by its name — the same debt `red_eye_sclera` carries.
* **Mask warping** (§5.2) and the **compositor warp stage** (§5.1).
* **Perf measurement of the live preview.** A Draft bake plus a whole-document recomposite per frame
  is the drag's cost, and it has not been measured on a large canvas. The two mitigations already in
  place are the frame coalescing and the Draft downgrade; a third — previewing at a reduced
  *resolution* rather than a reduced *kernel* — is the obvious next lever if it is needed.
