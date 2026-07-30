# Vector Layer Model — design

> **Scope.** This note settles the data model for Mosaic's vector stack (S25 — *Vector
> layer infrastructure*) and picks the GPU **rendering** algorithm that will draw it. It is
> the source of truth for S25–S30 (vector infra, Shape, Line, Pen, Type) and the vector
> parts of S31 (masks) and S36 (rasterize). It follows the house discipline: *design from
> the public-domain technique, bake the technique lineage into the source.*

---

## 1. What was settled (design conversation, 2026-06-23)

The vision: vector is a **first-class pillar** of Mosaic ("why use Inkscape when I can do
this 10× faster here"), not a supporting feature. That raises the bar — design the object
model generously and **SVG-export-aware** from day one — but it does **not** change the
three fundamentals:

1. **One `LayerKind::Vector`.** Shape, path, and gradient-fill are *object/paint variants*,
   not separate layer kinds. The kind is just a dispatch tag; capability lives in the
   object's variant, so a new capability is a new variant case, never a new layer class.
   This matches the existing `layer.hpp` and costs nothing in expressiveness (vector
   capability is bounded by *being vector geometry*, identical whether there are 1 or 5
   kinds).
2. **One object per layer.** Composition is the layer stack + groups; there is no second
   sub-stack hiding inside a layer. A *compound path* (donut, boolean result) is **one**
   `Path` object with multiple subpaths, so this does not block compound geometry.
3. **`Text` stays its own kind.** Shaping (HarfBuzz), per-run styling, layout, 3D extrude
   (S30) are genuinely different machinery. But text *emits the same outline geometry*
   everything else does, so rasterize / convert-to-path / 3D-extrude operate on the shared
   `Contours` representation — reuse without coupling.

Other decisions:

- **Gradient is a *paint*, not a geometry.** A "gradient layer" = a `VectorObject` whose
  geometry is a full-bleed `RectShape` and whose `fill` is a `Gradient`. "Convert a
  gradient to a path" only converts its *container* rect; the gradient paint can only
  rasterize (or export natively to SVG/PDF, which carry gradient defs).
- **Vector masks (S31) reuse the seam.** A mask gains optional geometry, flattened to
  coverage and combined with the existing `RasterMask`. The geometry model must therefore
  be a **standalone type, not welded to `VectorLayer`** (it already is — see §2).
- **Resize-vs-transform is a tool toggle, not a model decision.** Dragging a shape's
  bbox handle edits the *parameters* by default (size grows, stroke stays uniform — the
  intuitive Figma/Affinity behavior); a **contextbar toggle** switches to transform-scale
  (stroke distorts). The data model supports both (geometry in a canonical local frame +
  layer transform places it); S26 owns the UX. The existing Move/Resize/Rotate gizmo
  already operates on `layer.transform()`, so vector layers move/rotate from day one.

---

## 2. The data model

All coordinates are **layer-local**; `layer.transform()` maps the object into document
space, exactly as `RasterLayer::image()` does. Built on the existing `common::Vec2`,
`Rect`, `Affine2D`, `ColorF` (float RGBA — chosen over `Color8` so gradients don't band
and the model is precision-independent).

### 2.1 The seam — the one thing everything routes through

```cpp
struct Contour { std::vector<Vec2> points; bool closed = false; };   // flattened polyline
using Contours = std::vector<Contour>;

// Subdivide béziers to `tolerancePx` *device* px (pass world→device so flatness tracks
// zoom). THE chokepoint: fill, stroke, hit-test, mask coverage, bounds, SVG export all
// consume Contours. Get this right and every later feature is additive.
Contours flatten(const Geometry&, double tolerancePx, const Affine2D& toDevice);

// Arc-length walk used by BOTH dashed strokes (dash offset = a distance) and text-on-path
// (glyph placement = position + tangent at a distance). Build once, reuse three times.
struct PathSample { Vec2 pos; Vec2 tangent; };
PathSample samplePathAt(const Contours&, double arcDistance);
```

### 2.2 Geometry — closed variant, `BooleanCompound` built

```cpp
// Editable path (Pen tool, S28) — Illustrator/Inkscape node model.
struct Node {                       // coords layer-local, ABSOLUTE
    Vec2 anchor;
    Vec2 inHandle;                  // == anchor  ⇒ straight on that side
    Vec2 outHandle;
    enum class Type { Corner, Smooth, Symmetric } type = Type::Corner;  // S28 edit hint; flatten ignores
};
struct SubPath { std::vector<Node> nodes; bool closed = false; };       // cubic i→i+1 = outHandle[i], inHandle[i+1]
enum class FillRule { NonZero, EvenOdd };
struct Path { std::vector<SubPath> subpaths; FillRule fillRule = FillRule::NonZero; };

// Parametric primitives (Shape tool, S26), defined centered at the local origin.
struct RectShape    { Vec2 size; double cornerRadius = 0; };
struct EllipseShape { Vec2 radii; };
struct PolygonShape { int sides = 5;   double radius = 1; double cornerRadius = 0; };
struct StarShape    { int points = 5;  double outerRadius = 1, innerRadius = 0.5; double cornerRadius = 0; };
struct LineShape    { Vec2 a, b; };                                     // open, stroke-only
using ParametricShape = std::variant<RectShape, EllipseShape, PolygonShape, StarShape, LineShape>;

using Geometry = std::variant<Path, ParametricShape, BooleanCompound>;   // S28, §9
```

`BooleanCompound { BoolOp op; std::vector<Object> children; }` is the third case, for
**live/non-destructive booleans** (S28). It is **built** — see §9. The reservation paid off
exactly as designed: `Geometry` stayed a closed variant, `flatten` was already recursion-ready,
and the case dropped in without a rewrite. Operands stay live *inside one object on one layer* —
no multi-object layer needed.

One thing the reservation did **not** buy, and it is worth naming here rather than only in §9.2:
a compound on a layer is a shape **no tool binds**, because every tool predicate names a concrete
alternative (`vec::Path` for the Pen, `ParametricShape` for the Shape tool). So `Layer ▸ Combine
Paths` commits a **baked `Path`**, and the compound is reached only by a mode that knows what to do
with one. A new `Geometry` alternative is cheap for `flatten()` and expensive for the *tools* —
that is the real cost of widening this variant, and it is the thing to budget for next time.

### 2.3 Paint — where "gradient" lives

```cpp
struct GradientStop { double offset; ColorF color; };                  // offset ∈ [0,1]
enum class GradientType { Linear, Radial, Conic };                     // Conic = our extra; degrades on SVG export
enum class SpreadMethod { Pad, Repeat, Reflect };                      // == SVG spreadMethod
enum class DitherKind { None, Ordered, BlueNoise, Noise };             // S22: kills ramp banding
struct Gradient { GradientType type; std::vector<GradientStop> stops; Affine2D transform; SpreadMethod spread = SpreadMethod::Pad; DitherKind dither = DitherKind::None; };
struct SolidPaint { ColorF color; };
struct NoPaint {};
using Paint = std::variant<NoPaint, SolidPaint, Gradient /*, Pattern, ImageFill later */>;
```

### 2.4 Stroke

```cpp
enum class LineCap  { Butt, Round, Square };
enum class LineJoin { Miter, Round, Bevel };
enum class StrokeAlign { Center, Inside, Outside };                    // our extra; SVG is center-only (outline/clipPath on export)
struct Stroke {
    Paint  paint = NoPaint{};                                         // strokes can be gradients too
    double width = 1.0, miterLimit = 4.0, dashOffset = 0.0;
    LineCap  cap  = LineCap::Butt;
    LineJoin join = LineJoin::Miter;
    StrokeAlign align = StrokeAlign::Center;
    std::vector<double> dashArray;
    bool enabled = false;
};
```

### 2.5 The object and the layer

```cpp
struct VectorObject {
    Geometry geometry;
    Paint  fill   = NoPaint{};
    Stroke stroke;
    enum class PaintOrder { FillThenStroke, StrokeThenFill } paintOrder = PaintOrder::FillThenStroke;
};

class VectorLayer : public Layer {        // ONE object; layer transform places it (no per-object xform)
    std::optional<VectorObject> m_object; // contentBounds() = flatten(...).bounds(), cached + revisioned like RasterLayer
};
```

---

## 3. Conversions & operations

| From | Rasterize | Convert to path |
|---|---|---|
| Raster | n/a | ✗ — auto-trace/vectorize is a separate heavy feature, not this |
| Path | ✓ | n/a |
| Shape | ✓ | ✓ — flatten parametric → editable `Path` |
| Text | ✓ | ✓ — glyph outlines (FreeType) → `Path` |
| Gradient (fill) | ✓ | container rect → path only; the paint bakes |

- **Merge Down = lowest-common-denominator.** Vector ∪ Vector → one compound `Path`
  (shapes promote to paths; losing parametric editability is the honest, universal cost).
  Anything ∪ Raster → **Raster**. Text → convert-to-path when merging into vectors, else
  rasterize. The UI warns when a merge will rasterize.
- **Booleans (S28, BUILT — §9).** Add / Subtract / Intersect / **Exclude (XOR)** — each
  yields **one** object. This section originally called for shipping them *live by default*
  (a `BooleanCompound`) with an explicit "Flatten to path" to bake. **What shipped is the
  other way round** (§9.2): `Layer ▸ Combine Paths` **commits a baked `Path`**, because a
  compound on a layer is a layer no tool binds — not the Pen (`vec::Path` only), not the
  Shape tool (`ParametricShape` only). The live model, its kernel and its serialization are
  all built and stay built; what is missing for the non-destructive version is the *tool*
  side (a compound-aware selection/edit mode), not the geometry. Note: *live booleans and
  Selective Undo (S36-b) are different axes* — live booleans keep operands editable in the
  *present*; Selective Undo edits the *past*. You want both; neither substitutes.
- **Divide** (Affinity's 5th op — cut into all sub-regions, the stained-glass case) is the
  only boolean that produces *multiple* objects. Deferred and low-priority; when built it
  **emits its shards into a freshly-spawned group of single-object vector layers**, which
  preserves "one object per layer" perfectly (the group *is* the multi-object container).

---

## 4. Text integration (S29/S30) — no text-specific geometry needed

Text-on-path needs only to *walk a curve by arc length* and read **position + tangent** per
glyph — that's `samplePathAt` on top of `flatten` (§2.1). Because **any** `Geometry`
flattens to `Contours`, text flows uniformly along a `Path`, an `EllipseShape`, a
`StarShape` — so "text along a path" and "text around a circle / another shape" are the
**same feature**; text-on-ellipse is free once text-on-path works. The baseline is either
intrinsic to the `TextLayer` or a **`LayerId` reference** to a vector layer (the "drag text
onto a path layer" of S30) — being a reference, moving the path re-flows the text
non-destructively. **Dashed strokes use the identical arc-length walk**, so the sampler is
built once and pays off for dashes, text-on-path, and any future "draw along path."

---

## 5. Renderer — the technique landscape

The **data model above is technique-neutral** (SVG-shaped structs; SVG's model is an open
standard). All the interesting choices are in the GPU *rendering algorithm*. Three premier
techniques exist, and two of the three became freely available in 2026, which transforms
the picture.

### 5.1 Findings

- **Loop–Blinn — resolution-independent GPU curve rendering — FREE TO IMPLEMENT.**
  Loop & Blinn, SIGGRAPH 2005. Direct GPU rendering of quadratic curves via the implicit
  `u² − v` test. **Design from the 2005 paper, never from a later enhancement of it** —
  the same rule as the NVIDIA case below.

- **Slug — per-pixel Bézier coverage (premium text/shape quality) — DEDICATED TO THE
  PUBLIC DOMAIN (now usable).** Eric Lengyel / Terathon dedicated it to the public domain
  in March 2026; reference shaders are released **MIT**, and an open implementation exists
  as **HarfBuzz GPU** (Behdad Esfahbod). Relevant to S29 text quality and to high-quality
  direct-from-outline shape fills.

- **Generic stencil-then-cover (two-pass winding/parity fill) — CLASSIC, PUBLISHED.**
  The "draw the triangle fan into the stencil with `GL_INVERT` (even-odd) or
  increment/decrement (non-zero), then cover" technique is **OpenGL Red Book Ch. 13**
  ("Drawing Filled, Concave Polygons Using the Stencil Buffer"), well-known-in-the-art since
  the 1990s. The fill rules map to stencil ops directly.

- **⚠ NVIDIA `NV_path_rendering`'s SPECIFIC stencil-then-cover enhancements — DO NOT
  IMPLEMENT.** Shared-edge handling, NVIDIA's particular cover-geometry generation, and the
  GL extension surface are all deliberately out of scope. **Use the generic Red Book method;
  do not implement those specific optimisations.** This is a hard constraint on the renderer.

- **Triangulation libraries — license-clean, algorithmically classic.**
  **libtess2** (refactored GLU tessellator) — SGI Free Software License B v2.0, which the
  FSF treats as X11/MIT-equivalent → **GPL-compatible**. **earcut.hpp** (Mapbox) — **ISC**,
  GPL-compatible; ear-clipping is classic (Eberly; Held's FIST). Both safe to vendor.

- **SDF / MSDF text — the foundational technique is published, the generators permissive.**
  Chris Green (Valve), *"Improved Alpha-Tested Magnification…"*, SIGGRAPH 2007. **msdfgen**
  (Chlumský) is open (MIT). ⚠ Implement **Green 2007's basic method**, not one of the
  recent narrow "generate an SDF image of text" pipelines.

- **Anti-aliasing — free.** Hardware **MSAA** or **analytic scanline coverage** (Anti-Grain
  Geometry, Shemanarev; FreeType smooth rasterizer signed-area) — classic and public,
  GPL-compatible licenses.

### 5.2 Summary table

| Technique | Source | Owner | Use in Mosaic |
|---|---|---|---|
| Loop–Blinn GPU curves | Loop & Blinn, SIGGRAPH 2005 | — | **Usable** (direct curve fill) |
| Slug per-pixel coverage | Lengyel; public domain 2026, MIT shaders | Lengyel | **Usable** (premium text/shape) |
| Generic stencil-then-cover | OpenGL Red Book Ch. 13 | — | **Usable** (concave fill) |
| NV stencil-then-cover (specific) | `NV_path_rendering` | NVIDIA | **⚠ Do not implement**; the generic method is what we use |
| Triangulation | libtess2 / earcut.hpp | SGI / Mapbox (SGI-B / ISC) | **Vendor either** |
| SDF / MSDF text | Green 2007 / msdfgen (MIT) | Valve / Chlumský | **Usable** (design from Green) |
| Analytic AA | AGG / FreeType smooth (classic / BSD-like) | — | **Usable** |

### 5.3 Recommended lane

**Conservative default (build first):** CPU `flatten` → **triangulate (libtess2 or
earcut)** → GPU fill with **MSAA** (or analytic coverage); **strokes** via **CPU
stroke-to-outline** (expand to a fill outline with caps/joins; dashes = `samplePathAt`
arc-length walk) → fill through the same path. Every piece here is classic/public-domain and
license-clean, and robust for concave/self-intersecting paths and both fill rules.

> **Built (S25 CPU floor):** the stroke outline is a **union of filled pieces** (segment quad +
> per-end cap + per-vertex join), each winding-normalized so a NonZero fill unions overlaps — this
> deliberately **sidesteps single-offset-outline self-intersection** on tight turns. `StrokeAlign`
> Inside/Outside is therefore **not** realised by geometric offsetting (which would reintroduce that
> fragility) but by **coverage-clipping**: rasterize a *centred, double-width* stroke, then `min()`
> it against the shape's fill coverage (Inside) or its complement (Outside). Robust on any path,
> reuses the one rasterizer, and maps cleanly onto the GPU path later (intersect in the stencil
> buffer) and onto SVG export (a `clipPath`). Open paths have no inside → alignment degrades to
> Center. The CPU rasterizer also **bounds every fill/stroke pass to the object's clamped pixel
> bbox** (the full-window result buffer stays — that allocation is shared by all layers and is
> S60-c tiling territory).

**Headroom:** the **generic stencil fill** avoids CPU triangulation; **Loop–Blinn** does
direct GPU curve fills without flattening; **Slug** gives per-pixel coverage quality for
text (S29) and crisp shape edges. These are *upgrades we can adopt later* — the
conservative lane is the floor, not the ceiling.

**Full-GPU-residency target (project goal).** Mosaic is heading toward keeping geometry
GPU-resident and avoiding per-frame CPU round-trips. That steers the *eventual* renderer
toward **generic stencil-then-cover** and/or **Loop–Blinn** — upload the path's control
points / cover geometry once, evaluate fill (and the implicit curve test) in shaders, and
re-stencil only the dirty region on edit. CPU-triangulate-per-frame is the *opposite* of
residency (it re-triangulates whenever a node moves), so it is the headless/test/export
path and the bring-up renderer, **not** the long-term live one. Conveniently, the two
GPU-resident techniques are exactly the two that came free in 2026. The §2 data model
is residency-neutral: `flatten` serves CPU consumers (hit-test, bounds, export, the bring-up
renderer), while the GPU path can consume raw control points directly from the same
`Geometry`.

**The one rule:** do not implement NVIDIA's *specific* stencil-then-cover enhancements. The
generic Red Book method covers our needs.

---

## 6. Technique lineage (source-header convention)

Every vector renderer source file carries a short header naming the published technique it
implements, so the lineage ("this is the Red Book stencil method / Loop–Blinn / Slug, by
date") is legible in the code:

```
// Vector rasterization. Technique lineage (see docs/vector-model.md §5):
//   - Concave fill: generic stencil two-pass winding/parity — OpenGL Red Book Ch.13.
//     NOT NVIDIA's NV_path_rendering specifics.
//   - Triangulation: libtess2 (SGI Free SW License B) / earcut (ISC).
//   - Curves (optional): Loop-Blinn, SIGGRAPH 2005.
//   - AA: MSAA / analytic scanline coverage (AGG / FreeType — classic, public).
```

---

## 7. Shape tool & shape-designer (S26)

> **Status.** §7.1 authoring is **built** (S26-a). The **model extensions §7.3 + the
> concave/bevel flattening §7.6 are now built** (S26-b, 2026-06-23): `RectShape` carries per-corner
> `cornerRadius[4]` + `cornerStyle[4]` (with `RectShape::uniform()` for the common case),
> `EllipseShape` gains `startAngle/endAngle/arcMode` (Open/Chord/Pie arcs), `PolygonShape` a
> `cornerStyle`, `StarShape` split `pointRadius`/`valleyRadius`; `flatten()` emits all four corner
> styles via a shared `emitCorner` engine (unit-tested in `tests/test_vector.cpp`). Authoring also
> **pixel-snaps** the drag box (crisp axis-aligned edges). **§7.1 select-to-edit is built**: clicking
> an existing shape with the Shape tool picks it (`core::topmostVectorLayerAt`), switches the slot to
> its kind, reflects its parameters into the options bar, and edits it live via a coalesced
> `SetVectorObjectCommand` (the pure model↔options bridge — `shapeKindOf`/`readShapeOptions`/
> `editedObject` — is unit-tested). **§7.2 paint-preview swatch is built** (a custom `PaintSwatch`
> the options bar shows left of the Paint combo for shape tools; reads the live paint mode + fg/bg,
> redraws on a mode or colour change). **§7.1 resize-vs-transform is built**: a selected shape now
> shows the Move tool's selection box + 8 handles (reusing `setTransformHandles`); dragging a handle
> RESIZES the size parameters by default (`ui::resizeShape` -- pure + unit-tested; Rect/Ellipse/Line
> anisotropic, Polygon/Star uniform; the stroke stays uniform and the opposite handle is pinned via a
> re-anchoring layer shift), the body moves it and the corner band rotates it (rigid, via the layer
> transform); a **"Scale stroke"** options-bar toggle (Illustrator's "Scale Strokes & Effects"; the
> internal option id stays `transform`) switches a handle drag to transform-scale (writes
> `layer.transform()`, so the stroke scales with the box). The resize is atomic + undoable: `SetVectorObjectCommand`
> gained an optional transform so the params and the re-anchor land (and undo) in one coalesced step.
> **§7.5 line paint modes are built** (2026-06-24): `LineShape` carries a `paint` (Solid / Hollow /
> Outlined) + a `borderWidth`; the line colour is the Object's Stroke paint (fg, the weight/cap live
> there too), an Outlined line's contrasting border is the FILL slot (bg) -- a line has no interior.
> `rasterizeObjectF` renders Hollow/Outlined as a coverage RING (the thick region at a larger width
> minus a smaller one), so the border never shows the region's internal quad/cap seams; Solid is the
> ordinary centreline stroke, untouched. A dash array carries through to Hollow/Outlined too (the
> region strokes copy it), and the designer offers Solid/Dashed/Dotted/Dash-dot for EVERY shape's
> outline (a shared stroke-dash control). The line is now a full participant in the options bar
> (a Solid/Hollow/Outlined paint choice + a Border slider) and the paint swatch, no longer special-cased.
> **§7.4 the shape-designer popover is built** (2026-06-24, first cut): an "Edit shape…" button on
> the Shape options bar opens an anchored `ui::ShapeDesigner` (a child sub-window like the colour
> flyout) for the selected shape -- a live rendered preview plus per-kind controls reaching every
> §7.3 parameter (a rect's 4 per-corner radii + styles with a "Link corners" toggle; an ellipse's
> start/end angle + arc mode; a polygon's corner radius + style; a star's point/valley rounding; a
> line's border + dash editor). Edits a working copy and reports each change; the host lands it as a
> coalesced `SetVectorObjectCommand` (the same live/undoable path as select-to-edit, sharing the
> canvas's coalesce sequence) and re-reflects into the basic bar. **Deferred:** the spec's on-diagram
> DRAGGING (picking a rect corner, dragging the ellipse arc handles) -- the controls reach every
> parameter; the diagram is currently a non-interactive preview. **S26-b §7 is now fully built.**

> **S26-c (2026-07-27) — the Shape tool stopped authoring outlines, and draws as one.** Two changes,
> both user-directed.
>
> **(1) No tool-authored stroke.** The Paint (Fill / Outline / Fill + Outline) picker, the Stroke
> width slider, the line's Hollow/Outlined modes + Border slider, and the **§7.2 paint-preview
> swatch** are all RETIRED from the options bar. The Shape tool authors a **filled** shape in the
> foreground colour; an outline is a **Layer Effects `StrokeEffect`** — stackable, concentric,
> Inside/Center/Outside, with its own blend + opacity — added through the ordinary Layer Effects UI
> exactly as on any other layer. The Shape bar gains **no** route of its own to it: that
> infrastructure already has an entry point, and a second one on the bar would be redundant.
> `ShapeOptions` lost `paint`, `background`, `strokeWidth` and `borderWidth`; `strokeWidth` became
> `lineWidth`. **The LINE is the one exception:** it has no interior, so a stroke is the only way it
> can exist — Weight + Cap stay tool options and `buildShapeDraft` still writes its stroke.
> **`vec::Object` keeps its `Stroke`, and `LineShape` keeps `paint`/`borderWidth`,** so every
> document and `.mosaic` file written before this loads, renders, resizes and recolours exactly as it
> did (`recoloredObject` still gives a legacy fill+stroke object's outline the `bg` swatch). §7.2 and
> §7.5 below therefore describe MODEL state and retired UI, not the current bar.
>
> **(2) Outline while dragging, fill on release.** The in-flight shape is now a **wireframe**: the
> canvas flattens the live draft (`ui::shapeOutlinePolyline`, pure + unit-tested) and draws its
> silhouette on the overlay's polyline lane — the same channel the lasso path and the Type frame ride
> — and the document is not touched at all until the pointer comes up. On release the host spawns the
> real filled `VectorLayer` from that same draft and commits it, still exactly one undo step. This
> replaces the Affinity-style live-layer preview, which re-composited the whole document on every
> drag frame. The lane carries one polyline, so a multi-contour shape previews its largest contour.

> **S26-c designer refinements (2026-07-28) — angles get a DIAL, handles conform to rounded corners.**
> Two user reports, both about the wrong instrument for the job.
>
> **(1) Every angle is a rotary knob now.** An angle is *cyclic*: a linear degree slider has
> arbitrary endpoints, cannot express wrap-around, and turns "point the tail down-left" into an
> arithmetic problem. The five angle parameters in the designer — the ellipse's `startAngle` /
> `endAngle`, the ring's `startAngle` / `endAngle`, and the callout's `tailAngle` — are edited on
> **`ui::Dial`** (widgets.hpp), the app's existing rotary knob, extended for the job: an in-face
> numeric readout (`setShowReadout`, which turns the needle into a rim tick so the digits have
> room), a configurable Shift-snap grid defaulting to the conventional **15°**
> (`setSnapIncrement` / `kDefaultSnapDeg`), arrow-key nudge (Shift walks the snap grid, Home/End
> jump to the rest angle and its opposite) on click-to-focus, and a hand cursor. `setZeroOffset(90)`
> reconciles the model's "0 = +x, y-down" convention with the knob's native 12-o'clock zero, so
> `value()` stays in the units the document stores. The knob never integrates a delta — it reads
> the cursor's direction each event — which is what makes dragging round and round wrap rather than
> pile up at an endpoint. Both routes into a sweep endpoint (the dial and the on-diagram handle)
> share one `applySweepAngle`, so 359° → 1° is a one-degree nudge on either, and the arc can never
> flip inside-out across the 0/2π seam. Every dial still reads its value through the designer's
> single `numericValue(obj, role, idx)`, so a dragged handle and its knob cannot drift.
> Deliberately **not** converted: nothing else in the model stores an angle — a polygon's or star's
> orientation and a banner's/arrow's are the *layer transform*'s job (§7.1), not a parameter.
>
> **(2) On-diagram handles ride the rounded outline.** A handle placed at a shape's raw parameter
> box sits where the *sharp* corner would be, so on a rounded corner it floats off the shape. The
> corner engine's public half is now `core/vector/corner.hpp`: the vertex rings each cornered
> primitive is built from (`rectPolygon` / `polygonPolygon` / `starPolygon` / `crossPolygon` /
> `bannerPolygon` / `calloutBodyPolygon` — **`flatten()` builds from these**, so there is one
> definition), plus `cornerPointAt`, which resolves a vertex into the corner flatten() actually
> emits: the two tangent points, the effective radius after the half-shorter-edge clamp, the
> ceiling that clamp imposes (`maxRadius`), and the **apex** — the emitted corner's midpoint, which
> lies ON the outline for a convex fillet, a concave scoop, a chamfer, and (degenerately) a sharp
> vertex alike. Handles are placed at the apex and dragged through `cornerRadiusForPoint`, its exact
> inverse, so a knob stays under the cursor for the whole drag and **stops dead** at the clamp
> instead of running on invisibly. Reflex vertices — a cross's four inner corners, a star's valleys —
> need no special case: the apex rides the bisector of the two *edge directions*, so its sign
> inverts on its own. The radius sliders take their ranges from `maxCornerRadius` for the same
> reason (a cross's twelve corners sit on short edges, so "half the shorter side" was a wild
> over-estimate). Both halves of the gesture are pure and exported from `ui/shape_designer.hpp`
> (`shapeHandlePoints`, `shapeAfterHandleDrag`); the tests assert each handle's *distance to the
> flattened outline*, at radius 0, mid-travel, saturated, and on a concave fillet.

### 7.1 Authoring & editing model

- **Authoring (built, S26-a).** The Shape tool drags out a parametric object; `ui::buildShapeDraft`
  puts the size in the shape's PARAMETERS (centred on the local origin) and the placement in a rigid
  translation, so Move rotates/scales without distorting the shape or stroke. Shift = square / circle
  / equal-radius (45°-snapped line); Alt = from-centre. Five variants share the toolbar slot.
- **Editing = "select-to-edit" with the Shape tool** (chosen over an always-on layer-context bar,
  2026-06-23). Clicking an existing vector shape with the Shape tool (`hitTest`) selects it **and**
  loads its parameters into the Shape options bar; editing any control rewrites that layer's object
  live via a *coalesced* `SetVectorObjectCommand`. The **Move tool only ever transforms** the layer
  (`layer.transform()`). So moving with Move never loses the shape controls — you pick the Shape tool
  and click the shape to edit it again.
- The options bar (and the designer popover) has **two states from one set of widgets**: *no shape
  selected* → the controls set the DEFAULTS for the next authored shape; *a shape selected* → they
  reflect + edit that shape. A single "edit target" `LayerId` switches between them.
- **Resize-vs-transform (S26-b).** With the Shape tool + a selected shape, dragging a bbox handle
  edits the size PARAMETER by default (stroke stays uniform — the Figma/Affinity default); a
  contextbar toggle switches to transform-scale (writes `layer.transform()`, stroke distorts).

### 7.2 Paint preview swatch (options bar) — RETIRED S26-c

> Historical: the paint modes this swatch previewed no longer exist (see the S26-c note
> above). Kept because the **colour convention** below still governs `recoloredObject`,
> which is how a shape authored before S26-c recolours from the swatch.

A custom swatch sits to the **left of the Paint combo** (in the corner), previewing the colours the
current paint mode applies (the two active swatches are foreground `fg` and background `bg`). The
three modes are labelled **Fill / Outline / Fill + Outline** (clearer than Solid/Stroke/Both).

**Colour convention (settled 2026-06-23, user call).** The **fill is the primary element → `fg`**;
the **outline is the secondary accent → `bg`**. This keeps the fill on `fg` across Fill *and*
Fill + Outline (Photoshop's fg/bg model maps "fg = the primary colour"; pro vector tools instead use
*independent* fill/stroke swatches — a possible future evolution of this swatch). So:

- **Fill** — an `fg`-filled square.
- **Outline** — an `fg` square with a diagonal-hatched inset meaning "no fill" (a lone outline is the
  primary element, so it takes `fg`).
- **Fill + Outline** — an `fg`-filled square with a `bg` inset/border (fill = `fg` *under* a `bg`
  outline).

When a shape is selected for editing, a colour-swatch change recolours it live (`recoloredObject`):
the fill (and a lone outline) take `fg`; when both are present the outline takes `bg`. Implemented as
a small custom `Fl_Box` the options bar places before the Fill `Fl_Choice`; it redraws when the mode
or either colour changes.

### 7.3 Model extensions (`core/vector/geometry.hpp`)

The designer needs richer parameters than S25 shipped. Each is a **superset** of today's struct
(old call sites get a `::uniform(...)` helper or all-equal defaults, so nothing breaks):

```cpp
enum class CornerStyle { Round, Inverse, Bevel, None };   // convex / concave scoop / chamfer / sharp

struct RectShape {
    Vec2 size;
    std::array<double, 4>      cornerRadius{0,0,0,0};                       // TL, TR, BR, BL
    std::array<CornerStyle, 4> cornerStyle{Round,Round,Round,Round};
    static RectShape uniform(Vec2 s, double r, CornerStyle st = CornerStyle::Round);
};

struct EllipseShape {
    Vec2 radii;
    double startAngle = 0.0, endAngle = 2*M_PI;            // a sweep < full == an arc
    enum class ArcMode { Open, Chord, Pie } arcMode = ArcMode::Open;  // how a partial sweep closes
};

struct PolygonShape {                                     // cornerRadius already present (S25)
    int sides = 5; double radius = 1; double cornerRadius = 0;
    CornerStyle cornerStyle = CornerStyle::Round;
};

struct StarShape {                                        // split the single S25 cornerRadius
    int points = 5; double outerRadius = 1, innerRadius = 0.5;
    double pointRadius = 0;   // rounding at the outer tips
    double valleyRadius = 0;  // rounding at the inner valleys (independent)
};

struct LineShape {                                        // its "design" lives in Stroke + paint mode
    Vec2 a, b;
    enum class Paint { Solid, Hollow, Outlined } paint = Paint::Solid;  // §7.5
    double borderWidth = 1.0;                             // Hollow/Outlined contrasting edge
};
```

### 7.4 The shape-designer popover

An anchored popover (reusing the `ui::Popover` child-sub-window host, like the colour flyout),
opened from an **"Edit shape…"** button in the Shape options bar. It shows an **interactive diagram
of the live shape** plus kind-specific controls; every edit is a coalesced `SetVectorObjectCommand`
(undoable, live). Per shape:

- **Rectangle** — the diagram's 4 corners are individually selectable (+ a **"link all"** toggle);
  the selected corner(s) get a **radius** slider and a **style** toggle: *Round* (convex, normal),
  *Inverse* (concave scoop), *Bevel* (straight chamfer), *None* (sharp). Multi-select edits in step.
- **Star** — sliders for **points**, **inner %** (`innerRadius/outerRadius`), **point rounding**
  (outer tips) and **valley rounding** (inner) — independent, the thing a star uniquely wants.
- **Polygon** — **sides**, **corner radius**, **corner style** (Round / Bevel / None).
- **Ellipse** — **start/end angle** (drag two handles on the diagram) + **arc mode** (Open / Chord /
  Pie). A full sweep is an ordinary ellipse; a partial sweep becomes an arc, a chord segment, or a
  pie slice.
- **Line** — **width**, **caps** (Butt / Round / Square), a **dash** editor, and the **paint mode**
  (§7.5). Joins are moot for a 2-point line.

The basic options bar keeps the *common* controls (paint mode + swatch, the one hot parameter per
shape — corner radius / sides / points / weight); the popover is the "everything" surface (the
"More…" bridge already used by the brush, S19).

### 7.5 Line paint modes (the line also gets a "fill") — UI RETIRED S26-c

> Historical: `LineShape::paint` / `borderWidth` and the Hollow/Outlined rasterisation are
> still in the model and still render, so older documents are unchanged -- but the Shape
> tool no longer authors them. A line's outline is a Stroke layer effect now (S26-c note
> above), which is what Hollow/Outlined were reaching for and does it with alignment and
> stacking they never had.

The line variant gains a three-way paint mode, reinterpreted for a 1-D primitive and labelled
**Solid / Hollow / Outlined** (disambiguated so "outline" can't be read two ways):

- **Solid** — the thick stroke is filled (today's behaviour).
- **Hollow** — only the *border* of the thick line is drawn, empty inside: stroke the stroke-outline
  contour (`strokeOutline(line)` gives the thick-line region; draw its boundary as a thin stroke).
  For "whoever needs just the outline."
- **Outlined** — a solid line **with** an added contrasting border (filled + outlined).

### 7.6 Flattening concave/bevel corners

`flatten` already emits convex rounded corners (S25). The new styles, at a corner whose two edges
meet at vertex `V` with the rounding tangent points `P0`, `P1` (each `r` along an edge from `V`):

- **Round** (convex) — a quarter-ish arc tangent to both edges, centred at the inset point (today).
- **Inverse** (concave) — the same tangent points `P0`, `P1`, but the arc is **centred at `V`** and
  bulges toward it, biting a scoop out of the shape (radius `r`, swept the other way).
- **Bevel** — a straight segment `P0`→`P1` (a chamfer).
- **None** — the sharp corner `V` (radius ignored).

This is local to the corner-emission loop; the seam (`Contours`), fill, stroke, hit-test and the
GPU path are all unchanged — concave corners are just more polyline points.

### 7.7 The widened shape library (S26-c)

Six parametric primitives join the S26-a five, each following the same two model rules — the real
size lives in the shape's own parameters, centred on the local origin; the placement stays a rigid
transform — and each expressed through `flatten()` as ordinary polyline contours, so no renderer,
mask, hit-test or export path changes. They are **filled** shapes: an outline is a layer-effects
Stroke, not something the tool authors.

| Kind | Parameters | Notes |
|---|---|---|
| `CalloutShape` | `size`, `body` (rounded box / ellipse), `cornerRadius`, `tail` (pointer / bubbles), `tailAngle`, `tailLength`, `tailWidth`, `tailSkew`, `bubbleCount` | The headline addition. A **pointer** tail is *spliced into* the body ring (one closed contour, so a stroke traces the balloon with no seam across the body edge); a **bubbles** tail is the thought-balloon trail of shrinking discs, genuinely separate contours. |
| `ArrowShape` | `size` (length × head width), `shaftRatio`, `headRatio`, `notchRatio`, `doubleHeaded` | Drawn along local +x; rotation is the layer transform, as for every other primitive. |
| `RingShape` | `radii`, `innerRatio`, `startAngle`, `endAngle` | Donut, ring segment, pie wedge and disc are one shape. A full annulus emits two rings of **opposite winding**, so the NonZero rule leaves the hole. |
| `CrossShape` | `size`, `armRatio`, `cornerRadius`, `cornerStyle` | Twelve vertices through the shared corner engine, so it rounds/bevels/scoops like everything else (the four inner corners get concave fillets for free). |
| `HeartShape` | `size`, `lobe`, `cleft` | Four cubics. The shoulder controls are solved so the lobes peak exactly on the box's top edge — the heart is *tight* in its bounds. |
| `BannerShape` | `size`, `style` (chevron / swallow-tail), `pointRatio`, `notchTail`, `cornerRadius` | Four useful looks from two parameters: breadcrumb chevron, pennant, ribbon banner, flag. |

Proportions that must survive a resize (an arrow's head, a cross's arms, a banner's point) are
stored as **ratios of the size**; only genuine distances (corner radii, a callout's tail) are
absolute. Corner radii stay absolute through a resize (the Figma convention); a callout's tail is
the one exception — it scales with the shape, because unlike a corner radius it is *inside the
shape's bounding box* and leaving it fixed would drag the resize anchor off.

`pathFromShape` converts all six. Five convert to exact cubics/corner nodes; the callout converts to
the spliced outline as a polyline (the price of the seamless ring — noted where it happens).

The **shape designer** is the surface for all of this: a large live diagram with on-diagram handles
(§7.4's deferred dragging, now built), a **kind gallery** that re-shapes the selected object in
place (`ui::convertedShape`, keeping its footprint and paint), and the per-kind controls grouped
under section headers with the dash editor behind a disclosure. The library kinds have no toolbar
`ToolId` of their own yet — the gallery is how they are reached; `ui::shapeKindCatalog()` is the one
list that names and pictures them, and adding toolbar variants is a matter of giving each an id.

---

## 8. Pen / custom path tool (S28)

> **Status.** Bézier authoring, on-canvas node/handle editing and the custom stroke are **built**
> (2026-07-28), as is the node/handle **chrome** on its own overlay lane (same day, second pass).
> **Boolean ops are built too** — they landed after this section was first written; see §9.
> Conversion needs nothing new: **Layer ▸ Convert to Path** (S26/§3) already promotes a shape or a
> text block to the exact cubic `Path` this tool opens, and **Rasterize** bakes one back to pixels.

The tool is two states of one thing, and which one you are in is simply whether a committed path is
bound: with nothing bound a press **places nodes**; with a path bound a press **grabs one of its
nodes, handles or segments**. Both halves are pure and live in `ui/pen_gesture.{hpp,cpp}`
(FLTK-free, unit-tested in `tests/test_pen_gesture.cpp`); `VulkanCanvas` owns only the pointer
plumbing and the overlay, and `app_window` only the document side.

**Authoring** (`ui::PenGesture`, DOCUMENT space). Click = a **Corner** node (both handles collapsed
onto the anchor, so the segment is straight on both sides); click-and-drag = a **Symmetric** node
whose mirrored handles are pulled out live; releasing continues the path; clicking the first anchor
**closes** it (and the closing drag may still shape that node's handles). `Shift` snaps the next
anchor, and a handle pull, to 45°; `Alt` during a pull breaks the pair into a **cusp**; `Backspace`
takes the last node back. **Enter / Escape / a double-click / a tool switch all FINISH the path** --
Illustrator's rule that Escape *ends* a path rather than discarding it; Undo is how you take a
finished path back, and a document swap is the one event that drops a half-drawn one.

Nothing reaches the document until the path is finished: the in-flight path (plus a rubber-band
segment showing the real cubic the next click would commit) rides the overlay's polyline lane, the
S26-c wireframe discipline. On finish it lands as a `ShapeDraft` -- geometry re-centred on the
layer's local origin, placement a rigid translation -- through the **same spawn-then-commit pair the
Shape tool uses**, so the whole session, however many clicks it took, is exactly **one undo step**.

**Editing.** Clicking a path binds it. Then: click an anchor to select and drag it (its handles ride
with it -- a move re-places the node, it never re-shapes the curve); drag **any** node's handle --
every node's handles are drawn *and* grabbable, because the chrome now has an overlay lane of its
own (binding 6, 512 knobs + 512 stems) and no longer competes with the document's guides for the
64-entry line lane; `Alt` while dragging a handle breaks the pair; **`Alt`-click an anchor** toggles
cusp ↔ smooth; **`Ctrl`-click an anchor** deletes it (the Pen is free to own `Ctrl` -- the temporary
eyedropper is gated on the stroke tools); **click a segment** to insert a node there. The insertion is
a **de Casteljau split**, so not one drawn pixel moves -- which is the whole point of adding a node
*on* a segment rather than near one -- and the drag is handed straight to the new anchor, so one
gesture both adds and places it. Each gesture is its own coalesced `SetVectorObjectCommand`.

`penHitTest`'s priority is therefore **the selected node's handles → any anchor → any node's handles
→ the segments**. The selected node's handles come first because a handle pulled only a little way
out otherwise sits inside its own anchor's pick disc and could never be taken; the *other* nodes'
handles sit **below** anchors so a handle parked over a neighbouring anchor never steals that
anchor's grab. Grabbing any handle also moves the selection onto its node, so the filled knob is
always the one being worked on.

**The chrome (S28, second pass).** It is drawn by `canvas_present.comp`'s `penChrome()` on binding 6,
in the **crop/transform chrome language**, deliberately and to the letter -- that is what makes
on-canvas chrome read as one tool rather than a pile of features:

* **shape carries meaning**, as on the DoF gizmo: a **square** knob is a cusp (a corner you can steer
  each side of independently), a **round** knob is a smooth/symmetric node, and a **smaller round**
  knob (0.8 H) is a handle tip -- small enough that it never reads as an anchor you could drag the
  curve by;
* **fill carries selection**: hollow/white is unselected, `kBoxColor`-filled is selected -- the
  universal vector-editor convention;
* **border carries hover**: a hovered knob swaps its black border for box-blue and, if it is also
  selected, lifts its fill toward white, so hover reads in both states;
* every knob and every stem wears `chromeShadow`, the tight always-on casing fixed-colour chrome
  needs (box-blue over box-blue content is otherwise invisible). Its absence is exactly why the
  borrowed guide lane looked weak: that lane draws a flat `mix(col, lineCol, cov)` with no casing and
  no knob idiom at all.

Every size hangs off the transform handles' own `H`, so the chrome is **DPI-correct by construction**
rather than by a second scaling rule -- a pen anchor and a Move handle carry identical weight at
every DPI. Stems stay a single hue over the casing (no white-core/black-halo two-tone on a long line;
that froze into a barber pole on rotated edges).

A **closing-loop ring** is drawn at `kPenCloseScreenPx` around the first node whenever the pointer is
inside it, while an open path with ≥2 nodes is being authored. It is the one place the tool can say
"click here and it shuts": the close radius was otherwise an invisible affordance, a pure hit test.
`ui::penCloseTarget` is pure, so the ring and the close test can be pinned against each other.

The **path spine is drawn in edit mode too**, not only while authoring. Edit mode used to show the
node marks and nothing else, so a path you were shaping had no curve of its own beyond whatever the
layer happened to paint -- and a fill-only path with no stroke had nothing at all. That was the
single largest hole in the tool.

`penPathPolyline` now emits **all** contours, separated by a `kPolylineBreak` sentinel (a vertex with
`x <= kPolylineBreakX`); the shader skips any segment touching one, so the single-polyline lane can
carry a multi-subpath path without a bogus chord bridging one contour's end to the next one's start.
The sentinel is duplicated in three places that cannot include each other -- `ui::kPolylineBreakX`,
`render::kPolylineBreakX`, and `canvas_present.comp`'s `kPolyBreak` -- and they must agree. This is
also the mechanism a live `BooleanCompound` preview needs (§9), since a boolean result is normally
several rings. Note that `ui::shapeOutlinePolyline` keeps only the **largest** contour: that is the
Shape tool's silhouette rule, it is deliberately *not* shared, and it does not generalise.

**Two known follow-ups**, reported when the boolean kernel landed (§9):

* `vulkan_canvas.cpp`'s text-on-path wrap heuristic is `baked.size() == 1 && front().closed`, so a
  **multi-contour** path silently loses wrap-around -- text fitted to a boolean result gets the
  non-wrapping branch with no diagnostic. It predates §9 and was correct when every fittable path had
  one contour.
* `render::mergeDownVector` refuses a pair whose **stroked content bounds** overlap (it concatenates
  contours instead of combining regions) and refuses a pair whose **fill rules** differ. Both
  refusals exist precisely because there was no boolean kernel; there is one now, and relaxing them
  is a real option rather than a wish.

**Custom stroke.** The Pen is the one vector tool that still authors a stroke (S26-c retired the
Shape tool's in favour of the layer-effects `StrokeEffect`), for the same reason the Line is the
other exception: **an open path has no interior**, so a stroke is the only way most pen paths can
exist. The bar carries S27's set -- Fill, Stroke, Weight, Cap, Join, Dash -- and with both Fill and
Stroke switched off `penPaintedObject` forces the stroke back on rather than leaving an invisible
layer behind. Colours follow the shared convention (`recoloredObject`): the fill is the primary
element (`fg`), an outline drawn over it the secondary accent (`bg`), a lone stroke primary. An
outline *around* the whole path is still a Stroke layer effect, as for every other kind.

**Hit tolerance is a SCREEN measure.** The grab radius is 6 logical screen px, divided by the view
zoom into document px and then mapped through the layer's inverse world transform into layer-local
px. Derived in that order it is a constant number of pixels on screen at 5% and at 6400%; left in
document units it would be unusable at both ends. `tests/test_pen_gesture.cpp` pins the arithmetic at
three zooms.

**⚠ Events are rooted in the CANVAS, never the window.** Every pen point comes from
`VulkanCanvas::eventDocPoint()`, which is honest only inside the canvas widget's own `handle()` --
FLTK translates the event pair into a child sub-window's frame for exactly that call and restores it
on the way out. Driving the pen from `MainWindow::handle()`, or reading `Fl::event_x/y` from the
frame loop, would displace the whole path by the canvas's origin inside the top-level (the menu bar +
options bar above it, the tool rail to its left), scaled by the zoom. `tests/test_pen_canvas.cpp`
drives the real widget through a chrome offset and asserts the landed path's document coordinates,
so a regression that reinstates a window-frame read fails rather than merely looking odd.

**Boolean ops: now built, in §9.** They were held back from the first S28 landing on the grounds
that half-landing a boolean that is wrong on touching edges is worse than not having one. The
kernel that landed answers that directly: it snap-rounds onto an integer lattice so that
"touching" is an exact arithmetic fact rather than a tolerance, and coincident edges, vertex-on-
edge, self-intersection and mixed winding each have a named handler and a test.

---

## 9. Boolean operations (S28)

> **Status.** **BUILT.** `core/vector/boolean.{hpp,cpp}` + the `BooleanCompound` `Geometry`
> alternative; taught to `flatten()`, `pathFromGeometry()`, `fillRuleOf()` and the `.mosaic`
> writer/reader. Unit-tested in `tests/test_vector_boolean.cpp`. `Layer ▸ Combine Paths` calls
> `makeBooleanObject`, which **commits a baked `Path`** — see §9.2. The compound model stays in the
> tree for a future live mode and is reachable through `makeLiveBooleanObject`.

### 9.1 The model

```cpp
enum class BoolOp { Union, Subtract, Intersect, Exclude };   // "Add" is the menu's word for Union
struct BooleanCompound {
    BoolOp op = BoolOp::Union;
    std::vector<Object> children;      // rebased into the HOST's local frame at build time
    bool operator==(const BooleanCompound&) const;   // NOT defaulted -- see below
};
using Geometry = std::variant<Path, ParametricShape, BooleanCompound>;
```

Two consequences worth naming.

**`Geometry` is now recursive** (`Object` → `Geometry` → `vector<Object>`), which is the point:
an operand is an ordinary `Object` with its own paint and its own geometry, and it may itself be
a compound, so `(A ∪ B) − C` is one object on one layer. `std::vector` of an incomplete type is
well-formed, so `geometry.hpp` forward-declares `Object` and everything else follows. The one
thing that could not be defaulted is `BooleanCompound::operator==`: a defaulted comparison would
have to be analysed against `std::vector<Object>` with `Object` still incomplete at that point.
It is declared in the header and **defined in `boolean.cpp`**, where `object.hpp` has made
`Object` complete. Equality matters — several call sites compare whole objects to suppress no-op
commands — so this is load-bearing, not tidiness.

**Children are rebased, not transformed.** A compound carries no per-child transform. When the
operands come off N different layers, `makeLiveBooleanObject` maps each into the host layer's local
frame once, at build time (`inverse(hostWorld) * operandWorld`), using `transformedPath` — exact,
because cubics are affine-invariant. A `ParametricShape` child promotes to a `Path` on the way
(its parameters *are* its size, centred on the local origin, so it cannot carry an arbitrary
transform); a compound child recurses, so nesting stays live rather than baking. The result keeps
the **first** operand's fill/stroke/paint-order — the host's appearance wins, the Illustrator rule.

### 9.2 The seam: why a boolean never clips a Bézier

Everything downstream of §2.1 consumes `Contours`. So the compound's `flatten()` arm recurses into
its children **at the same tolerance and the same device transform the caller passed**, and runs
the op on the resulting polylines. Fill, stroke, hit-test, `contentBounds`, thumbnails, the
rasterizer and text-on-path then all become correct with **zero curve fitting** — there is no
Bézier–Bézier clipper anywhere in the tree and there does not need to be. Curve smoothness still
tracks zoom, because the tolerance and transform ride through the recursion unchanged.

The price is the honest one: **baking a compound gives a polyline** `Path` (nodes whose handles
equal their anchors), because the outline is *computed* rather than parametric. That is exactly the
concession `CalloutShape`'s spliced outline already makes (§7.7). There is no Bézier refitting and
there is not meant to be.

**`Layer ▸ Combine Paths` commits that baked path — not a live compound.** `makeBooleanObject`
folds the operands (via `makeLiveBooleanObject`) and then bakes, so the object the command puts on
the undo stack holds a `vec::Path`, full stop. This is not a hedge about the compound being
unfinished; it is what the result *is*. A committed boolean is a region computed from operands that
no longer exist as layers, and a third `Geometry` alternative on a layer is a layer **no tool
owns**: `ui::penToolBinds` requires `std::holds_alternative<vec::Path>` and the Shape tool requires
a `ParametricShape`, so a compound is selectable by neither, badges as the generic shape mark in
the Layers panel, and cannot be edited node by node. As a path it is picked by the Pen, badged as a
path, node-editable, and serialized with no side-car.

The baked path is well formed **for editing**, which is stronger than "draws correctly": every
subpath closed with ≥ 3 nodes, holes keeping the negative winding the NonZero normalization gave
them, no two consecutive anchors coincident (the wrap-around pair included — an editor showing two
draggable nodes stacked at one point is a defect), and every `Node::type` `Corner`, because a
polyline vertex is one.

**The baking tolerance is derived, not fixed.** A live compound re-flattens every frame at whatever
tolerance the frame asks for, so `0.25` costs it nothing; a bake is *permanent*, so the same
constant would be wrong twice over — far too coarse for a shape whose layer is scaled up (0.25
**local** units can be tens of document pixels) and gratuitously fine for one that is enormous in
local units. `bakeToleranceFor` takes the **finer** of two readings of one requirement: 0.25
*document* pixels read back through the host's world scale (the max column norm of `hostWorld`),
and 1/4000 of the operands' own extent. The two agree at ~1000 document pixels of extent and each
dominates on its own side of it. That is then capped at 0.25 so a bake is never coarser than the
old default, and floored at 1/100000 of the extent — a node *budget* of roughly 500 nodes for a
full circle at any size, deliberately allowed to beat the cap, because the kernel is O(E²) and an
"editable" path with ten thousand nodes is not an improvement on a compound.

**An empty region is refused, not committed.** Subtracting something that covers the host, or
excluding two identical shapes, resolves to no area. `makeBooleanObject` returns `nullopt` there
rather than an empty path: committing one would delete every consumed operand layer and leave the
host holding an invisible, unpickable shape — an undoable wipe that reads to the user as the
command having failed. The caller's "these shapes cannot be combined" status says so instead.
`makeLiveBooleanObject` has no such refusal; an empty compound is still live and still editable.

### 9.3 The kernel

`Contours booleanContours(BoolOp, const Contours& a, const Contours& b)` and its n-ary fold
`booleanContours(BoolOp, const std::vector<Contours>&)` (first operand = the host; a third
overload takes a per-operand `FillRule`). `normalizedContours(cs, rule)` is the one-operand case —
that is how an EvenOdd operand is read as EvenOdd and re-emitted as NonZero.

Pipeline:

1. **Snap-round** every point onto a fixed integer lattice — nominally **1/1024 of a layer-local
   unit**, backed off by powers of two if the input's magnitude would threaten the exactness
   budget. `flatten()` subdivides to `tolerancePx` *device* pixels, so the lattice is ~8× finer
   than the flattening tolerance up to about 3200% zoom and still finer than it up to ~25600% —
   the boolean is never the coarse step at a zoom anyone can reach. Coordinates are stored
   **doubled**, which costs one bit and makes every fragment midpoint an exact integer (step 4
   evaluates windings at midpoints).
2. **Refine** the edge set until three invariants hold: *(I1)* no two fragments properly cross,
   *(I2)* no vertex lies in the interior of a fragment, *(I3)* two collinear fragments either
   coincide exactly or share at most an endpoint. Splitting at a snapped intersection can create
   fresh work, so the rounds repeat (bounded) until a round finds nothing.

   **A computed crossing is not a lattice point**, and that is the whole mechanism rather than an
   inconvenience. The intersection of two lattice segments is rational; it is rounded to the
   nearest lattice vertex and **both** chains are then re-routed through that one vertex, which
   displaces each of them off its original line by up to half a cell. The invariant that has to
   hold is that the two chains *agree on the vertex* — never that the vertex stayed collinear with
   the edges it came from. Demanding the latter is a defect the kernel shipped with and carried
   until 2026-07-28: an `onSegment` test on the rounded point silently discarded every crossing
   that was not *already* lattice-exact, so `Layer ▸ Combine Paths` was correct only for
   axis-aligned operands and for shapes whose boundaries happened to meet on a shared vertex, and
   wrong — non-planar arrangement, rings that fail to close, area missing — for everything else.
   Two regular 8-gons of radius 5 with centres 5 apart unioned to *58.2*, less than one of them.
   The one thing rounding may not do is carry a split **outside its own segment** (two fragments
   that overlap and double back); when it does, the endpoint it ran past becomes the shared vertex
   instead. Split points are ordered along their edge by exact projection (`dot`) for the same
   reason: a rounded point is only *near* the segment, so a dominant-axis comparison is not a total
   order along a near-axis-aligned edge.
3. **Merge** fragments sharing an undirected endpoint pair into one representative carrying a
   per-operand **net winding delta**. This is where coincident edges are resolved — by arithmetic,
   not by a tie-break — and it is why "two squares sharing a whole edge" unions into a seamless
   rectangle rather than a rectangle with an internal hairline.
4. **Classify**: for each representative, evaluate every operand's winding number infinitesimally
   off each side and keep the fragment only where the op's predicate **disagrees** across it. The
   infinitesimal is *symbolic* — a sign, never a number — so ties in the ray test are broken by
   which side is being asked about, which is precisely what "just off this edge" means. Surviving
   fragments are emitted with the interior on a consistent side.
5. **Link and normalize**: walk the survivors into rings (at a pinch, take the turn that hugs the
   interior, so two lobes meeting at a point become two simple rings, not one self-touching one),
   classify by containment depth, force outers positive / holes negative, straighten collinear
   runs, and rotate each ring to start at its lexicographically smallest vertex.

**The predicate.** Snap rounding is what buys exactness: once every coordinate is a bounded
lattice integer, `orient2d` is a 2×2 integer determinant that `int64` evaluates **exactly** — no
adaptive expansion, no epsilon, no floating-point filter, and 6 bits of headroom on the magnitude
bound. That is why the kernel carries an integer predicate rather than a floating-point one.
Shewchuk's adaptive predicates (1997) are recorded as the fallback lineage if the lattice is ever
dropped for a full-precision kernel.

**NonZero normalization is a contract, not a convention.** The result is always closed rings,
outers wound positive (visually clockwise in this y-down space) and holes negative, so it is
unambiguously correct under **NonZero** — which is why `fillRuleOf()` answers NonZero for a
compound, full stop, and why nothing downstream ever has to negotiate a fill rule with a boolean
result. Ring order and each ring's start vertex are canonical, so two runs on the same input are
byte-identical.

**Open contours close for the area test** rather than being dropped — the same rule
`rasterizeCoverage` and `hit.cpp`'s `contains()` already follow (and SVG's). An operand is never
silently discarded for being open.

**Technique lineage** (the §6 source-header convention; recorded in `boolean.hpp`):

| Piece | Source |
|---|---|
| Region boolean by winding-number edge classification | Vatti, *A generic solution to polygon clipping*, CACM 35(7):56–63, **1992** |
| Segment-intersection reporting / the x-order prune | Bentley & Ottmann, IEEE Trans. Computers C-28(9):643–647, **1979** |
| Point classification (winding-number ray test) | Sunday's form of the classic Jordan-curve test — already this repo's rule (`hit.cpp`) |
| Snap rounding onto a fixed lattice | Greene & Yao, FOCS **1986**; Hobby, *Comput. Geom.* 13(4):199–214, **1999** |
| Exact predicates (recorded fallback) | Shewchuk, *Discrete & Comput. Geom.* 18(3):305–363, **1997** |
| Outer/hole classification by containment depth | the standard even-odd nesting test, as in `core/text/extrude_mesh.cpp` |

The kernel is designed from those publications and from nothing else; both load-bearing
families (Vatti 1992, Bentley–Ottmann 1979) predate 1995.

### 9.4 Serialization — forward compatible, no format-version bump

Since §9.2, a **committed** boolean is an ordinary `Path` and serializes as one: `"type": "path"`,
no side-car, nothing to negotiate. Everything below therefore applies only to a **live**
`BooleanCompound` — a document saved by the build that briefly committed compounds, or whatever a
future live mode writes. It is load-bearing for those and stays exactly as it is.

`kFormatVersion` **stays at 1**. The reader dispatches on a string `"type"` token, so writing
`"type": "boolean"` would make a build that predates S28 reject the payload — and `docio`'s
tolerance rule then counts it and inserts an **empty** vector layer, at which point re-saving from
that build destroys the shape for good. So the compound is written the other way round:

```json
{ "type": "path", "fill_rule": "nonzero",
  "subpaths": [ /* the BAKED outline */ ],
  "boolean": { "op": "subtract", "children": [ /* full vector objects */ ] } }
```

An older build reads a shape it fully understands (it simply stops being live); a current build
sees the `"boolean"` side-car and prefers it, discarding the baked subpaths. Because the fallback
route works, the version does not have to move — which is the whole point of writing it this way.
Parsing stays **strict**, per this file's contract: a *malformed* side-car is a reject, not a quiet
downgrade to the baked outline, because silently dropping the operands would look like a
successful load while losing live editability with no counter moving.

### 9.5 Known limits

- **A committed Combine Paths result is a polyline** at the baking tolerance (§9.2) — the curves
  that went in do not come back out as curves, because the kernel resolves flattened contours and
  there is no Bézier refitting. The derived tolerance makes the facet imperceptible at the scale
  the shape was authored at; it does not make the path *parametric* again. Curve fitting a resolved
  region is the follow-up if it is ever wanted, and it is a separate piece of work from the kernel.
  The **live** compound keeps full curve fidelity at every zoom — which is the argument for
  finishing a non-destructive mode rather than for un-baking the menu command.
- **Complexity is O(E²)** in the flattened edge count, with an x-order prune on the pair sweep and
  a y-range reject in the winding scan. That is the right trade at editor scale (a boolean of two
  flattened primitives is a few hundred edges), but a compound of two very dense paths re-flattened
  every frame at extreme zoom will show up in a profile before anything else does. The fix, when
  it is wanted, is a real Bentley–Ottmann status structure and/or caching the resolved contours per
  (tolerance, transform) — neither changes the *result*.
- **The refinement loop is bounded** (twelve rounds; it converges in one or two in practice).
  A pathological input that has not converged still produces output and never crashes, but its
  topology near the offending spot is not guaranteed.
- **A crossing that rounds onto an existing vertex of *both* edges** — it has to fall within half a
  lattice cell of an end of each, so the two ends are a cell apart and describing the same place —
  is resolved onto one of them, which can leave the other chain a sub-cell spur. The region is
  right to within the lattice; the spurious node is not worth a second rounding pass to remove.
  The general answer, if the case ever shows up in artwork rather than in reasoning, is Hobby's
  hot-pixel formulation: make every rounded crossing a *pixel* and route every segment that
  crosses that pixel through its centre, instead of rounding each crossing on its own.
- **The lattice is fixed at 1/1024 of a local unit**, not adapted to the device transform. Past
  roughly 25600% zoom the lattice becomes the coarse step rather than the flattening tolerance.
- **Divide** (§3's fifth op, the stained-glass case) is still out — it is the only boolean that
  produces *multiple* objects, and it wants the group-of-layers treatment described there.

## 10. References

- Loop & Blinn, *Resolution Independent Curve Rendering using Programmable Graphics
  Hardware*, SIGGRAPH 2005.
- Lengyel, *Slug* — **dedicated to the public domain (2026)**; reference shaders MIT;
  HarfBuzz GPU. https://sluglibrary.com/ · https://terathon.com/blog/decade-slug.html
- NVIDIA `NV_path_rendering` — ⚠ its specific enhancements are deliberately not implemented.
- OpenGL Programming Guide (Red Book), Ch. 13, *Drawing Filled, Concave Polygons Using the
  Stencil Buffer* — the classic published technique.
- libtess2 (SGI Free Software License B v2.0) — https://github.com/memononen/libtess2
- earcut.hpp (ISC) — https://github.com/mapbox/earcut.hpp
- Green, *Improved Alpha-Tested Magnification for Vector Textures and Special Effects*,
  SIGGRAPH 2007 (Valve) — SDF foundation. msdfgen (MIT) — https://github.com/Chlumsky/msdfgen
</content>
</invoke>
