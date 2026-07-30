# Layer Effects — design

> **Scope.** This note settles the design of Mosaic's **Layer Effects** — non-destructive,
> per-layer styles (stroke, colour/gradient/pattern overlay, drop/inner shadow, outer/inner glow,
> bevel/emboss, satin) reached from **`Layer ▸ Layer Effects…`** (the first item in the Layer menu).
> It is the source of truth for the **Layer-Effects arc (LE-a…g)** and the companion for
> **S30-e** (the 3D-text per-face integration, already scheduled in `docs/type-tool.md` §12). It
> follows the project discipline: *design from the public-domain technique, bake the technique
> lineage into the source, design CPU-first with a permanent CPU-only lane and GPU-residency-aware
> from day one, resolution-independent where the data allows.*
>
> **A sibling, not a section.** The **Texture Generator** (`Layer ▸ Texture Generator…`) is a
> *separate* feature — scoped in **`docs/texture-generator.md`** — **not** a layer effect (user call
> 2026-07-03). Layer Effects does not generate skies/paper; it styles existing layer pixels.
>
> **No bundled pattern tiles.** **DECIDED 2026-07-04, Mosaic ships NO bundled pattern tiles**
> (weight + procedural coverage; see `docs/le-d2-image-patterns.md` §5). The GIMP-import / "curated
> slice" language below (§7.3) is **SUPERSEDED** — image patterns are user-supplied only. No
> third-party pattern assets are distributed, so there is nothing to licence-audit.

---

## 1. What was settled (design conversation, 2026-07-03)

1. **Effects live on the `Layer` base — every kind carries them.** `std::optional<LayerEffects>` on
   `core::Layer` (`src/core/layer.hpp:58`), beside the optional `RasterMask` (`:40`), so raster,
   vector, text, group, **and 3D text** all inherit effects with one model (§3). Non-destructive:
   the effects are parameters, the pixels are a derived composite.
2. **A modal, two-pane surface — not a floating panel.** The Type-panel-style non-modal popover was
   rejected (*"too big and annoying and covers up too much of the canvas"*). Layer Effects is a
   **modal `Fl_Double_Window`** built on the **Settings-dialog** substrate (`src/ui/settings_dialog.cpp`
   — the two-pane rail + parallel panes + child-sub-window ordering that already dodges the FLTK
   traps), with a **live in-modal preview pane** (primary — see the result without moving the modal)
   **plus a live canvas preview** (the modal is movable so you can see it in full document context;
   commit on OK, revert on Cancel — the Fill-dialog transactional pattern, `src/ui/fill_dialog.cpp`).
   **No on-canvas handles for any effect** — all editing is inside the modal (§6).
3. **Left = fixed catalog; right = the live editable stack.** The **left pane** is the fixed effect
   catalogue in canonical order, each row `[✓] Stroke      − [1] +` (a checkbox + a **stack stepper**
   for stackable effects). The **right pane** is a **scrollable column of collapsible instance
   panels** (`DisclosureButton` headers, `src/ui/type_panel.cpp:157`): enabling Stroke ×3 yields
   three independently-editable *Stroke* panels stacked there. Clicking a left row scrolls the right
   pane to it (§6.2). The catalogue is **not user-reorderable** — the render order is canonical (§5).
4. **Overlays are independent, each with its own blend mode + opacity.** The earlier "Color and
   Gradient overlay can't coexist" idea was **overturned** in favour of the Photoshop/Affinity model:
   Colour / Gradient / Pattern overlay are three independent rows, each composited with its own blend
   mode + opacity, so a tint-over-gradient is legal. We only forbid a *literal* no-op (§5.3).
5. **CPU-first, with a permanent CPU-only lane.** *"We need a CPU-only mode… so we implement this on
   the CPU whether we like it or not; speed it up when S60 comes."* Effects get the **dual-lane
   discipline** of the extrude renderer: a CPU reference lane that must **always** exist, GPU
   acceleration layered on at **S60**. Blurs are separable-Gaussian / box-approx on CPU; the live
   preview uses a low-res proxy during drags (§8).
6. **Full canon + Satin.** v1 ships the complete set: **Stroke** (stackable), **Colour / Gradient /
   Pattern Overlay**, **Drop Shadow** (stackable), **Inner Shadow** (stackable), **Outer Glow**,
   **Inner Glow**, **Bevel & Emboss**, **Satin** (§5). Split across as many sessions as it needs (§2).
7. **Patterns split into two honest categories** (§7): **Procedural** — resolution-independent,
   freely scalable, **colour-editable** (the flagship); and **Image** — fixed-resolution bitmap tiles
   (a curated, licence-checked slice of the **GIMP** pattern set + *Make Pattern from Selection* +
   *Import*), scaled with an **honest quality ceiling** (crisp at ≤ native, soft above, native size
   shown). High-quality *scalable materials* (paper, fabric…) are the **Texture Generator's** job, not
   GIMP's ~100 px legacy tiles.
8. **Gradients reuse machinery we need anyway** (§8 of `docs/vector-model.md`, §7.4 here): the
   gradient *renderer* already exists (linear/radial/conic + pad/repeat/reflect + multi-stop,
   `src/core/vector/raster.cpp:28-75`); this feature adds the **Gradient flyout** — a multi-stop stop
   editor with **in-flyout handles** (a draggable preview *inside* the flyout, never on the canvas).
   The flyout owns **stops + spread**; the gradient **type** (linear/radial/conic) is owned by the
   *parent* control (the Gradient-Overlay row here; the future Gradient tool's context bar elsewhere).
9. **Fill-opacity + group effects.** A **Fill-opacity** knob (distinct from layer opacity — it dims
   the layer's own pixels while leaving effects at full strength; unlocks stroke-only / knockout
   looks) and **effects on groups** (a shadow around a whole group — the group already composites to
   an isolated buffer) are both in scope.
10. **3D text is per-face, not a smear (S30-e).** Overlays on 3D text are evaluated in the glyph's
    2D **design space** and mapped onto the **front cap**; the sides get a shaded continuation, not a
    re-projected copy. The mesh already bakes the domain this needs (§9).
11. **"Break/smash through the ground"** is **parked** as its own future doc — a bespoke
    displacement / Voronoi-fracture set-piece, closer to a filter than a style. Not in this arc.
12. **Split deliberately** — as many sessions as it takes; nothing crammed (§2).

---

## 2. Session breakdown (split deliberately)

Sequenced so every session is independently **shippable and headless-testable** (Claude runs the
headless golden harness; the user does visual verification). Labels are `LE-*`; they slot into
PLAN's **Phase 5 (Layer effects, filters, masks)** — final numbering when PLAN is reconciled.

| Session | Scope | Key deliverable |
|---|---|---|
| **LE-a** | **Model + render seam + primitives + first effect** | `LayerEffects` on the base (§3); the `renderLayer → effects → walkStep` pipeline + `effectsBounds()` (§4); the shared **blur** (separable Gaussian / fast-box) and **signed-distance** primitives (shared with S33 blurs); **Stroke** end-to-end headless (proves the stack model + concentric stacking). Golden tests. |
| **LE-b** | **The modal two-pane UI** | The modal on the Settings substrate; left catalogue (checkbox + the **net-new `−[n]+` stepper** widget); right scrollable instance stack (`DisclosureButton` panels); **live preview** (Fill-dialog pattern) + low-res proxy; the **fx badge** on `LayerRow` (open/reopen); `SetLayerEffectsCommand` coalesced undo. Wires Stroke visually. |
| **LE-c** | **Overlays I: Colour + Gradient** | Colour Overlay; **Gradient Overlay** + the reusable **Gradient flyout** (in-flyout handles, §7.4); **Fill-opacity**. |
| **LE-d** | **Overlays II: Patterns** | The `Pattern` paint type (Procedural + Image, §7); the **Pattern flyout** (two categories); the **procedural pattern library** + **GIMP import** + **Make-Pattern-from-Selection**; Pattern Overlay. |
| **LE-e** | **Shadows & glows** | Drop Shadow (stackable), Inner Shadow (stackable), Outer Glow, Inner Glow — the blur tier (§5). |
| **LE-f** | **Bevel/Emboss + Satin** | The shading tier: height-field-from-alpha → normal map → raked Blinn/Oren-Nayar light (Bevel/Emboss); Satin's offset-blur interference (§5). |
| **LE-g** | **Polish + interop** | Copy/Paste Layer Style; **Global Light** (shared angle across shadow/bevel); Scale-Effects; **`.mosaic` round-trip** (S48 dep); **Rasterize / Rasterize-down** (ties to S36 — rasterize-down bakes effects into the layer below). |
| **S30-e** | **3D-text per-face effects** | Front-cap design-space mapping + wiring 3D text into the pipeline (§9). Depends on LE-a…c; already scheduled (`docs/type-tool.md` §12). |

> LE-a/-b are the load-bearing pair (model+seam, then UI); LE-c…f are additive effect tiers that can
> reorder or merge in practice. The only genuinely-deferred item is the smash-through-ground set-piece
> (§1.11), and even that is named, not parked silently.

---

## 3. The data model

Effects attach to the **`Layer` base** (`src/core/layer.hpp:58`) so *every* kind inherits them with
one code path — the same choice the base already makes for `opacity`/`blendMode`/`RasterMask`.

```cpp
// src/core/layer_effects.hpp  (LE-a)  — std::optional<LayerEffects> on core::Layer
namespace mosaic::core {

// A run's/effect's paint reuses the vector Paint (solid/gradient/pattern) — docs/vector-model.md
// §2.3, extended with Pattern in §7 here. So overlays and strokes share the fill machinery.
using vec::Paint;   // NoPaint | SolidPaint | Gradient | Pattern (new, §7)

struct StrokeEffect {                    // STACKABLE (concentric)
    float  width = 3.0f;                  // px in layer space
    enum class Align { Inside, Center, Outside } align = Align::Outside;
    Paint  paint;                         // solid / gradient / pattern
    BlendMode blend = BlendMode::Normal;  float opacity = 1.0f;
    bool enabled = true;
};
struct OverlayEffect {                    // Colour / Gradient / Pattern share this shape
    Paint  paint;                         // the kind of paint IS the kind of overlay
    BlendMode blend = BlendMode::Normal;  float opacity = 1.0f;
    bool enabled = false;
};
struct ShadowEffect {                     // Drop + Inner (STACKABLE)
    ColorF color{0,0,0,1};  float opacity = 0.75f;  BlendMode blend = BlendMode::Multiply;
    float angleDeg = 120.0f, distance = 6.0f;       // Global-Light aware (§ LE-g)
    float spread = 0.0f, size = 6.0f;               // choke + blur radius
    bool enabled = false;
};
struct GlowEffect {                       // Outer + Inner (single)
    Paint  paint;  float opacity = 0.75f;  BlendMode blend = BlendMode::Screen;
    float choke = 0.0f, size = 8.0f;
    enum class Source { Edge, Center } source = Source::Edge;   // Inner glow only
    bool enabled = false;
};
struct BevelEffect {                      // Bevel & Emboss (single)
    enum class Style { OuterBevel, InnerBevel, Emboss, PillowEmboss } style = Style::InnerBevel;
    float depth = 1.0f, size = 5.0f, soften = 0.0f, angleDeg = 120.0f, altitudeDeg = 30.0f;
    ColorF highlight{1,1,1,1};  float highlightOpacity = 0.75f;
    ColorF shadow{0,0,0,1};     float shadowOpacity    = 0.75f;
    bool enabled = false;
};
struct SatinEffect {                      // single
    ColorF color{0,0,0,1};  float opacity = 0.5f;  BlendMode blend = BlendMode::Multiply;
    float angleDeg = 19.0f, distance = 11.0f, size = 14.0f;  bool invert = true;
    bool enabled = false;
};

struct LayerEffects {
    float fillOpacity = 1.0f;                 // §1.9 — dims the layer's OWN pixels only
    std::vector<ShadowEffect> dropShadows;    // stackable
    GlowEffect  outerGlow;
    OverlayEffect colorOverlay, gradientOverlay, patternOverlay;   // §5.3 independent
    SatinEffect satin;
    std::vector<ShadowEffect> innerShadows;   // stackable
    GlowEffect  innerGlow;
    BevelEffect bevel;
    std::vector<StrokeEffect> strokes;        // stackable, concentric
    bool empty() const;                       // no enabled effect & fillOpacity==1 ⇒ skip entirely
};
}
```

- **Stacking** is *within a type* — `std::vector<StrokeEffect>` / `dropShadows` / `innerShadows`.
  The `−[n]+` stepper adds/removes vector entries; each renders in order (strokes concentric-outward:
  index 0 innermost). Non-stackable effects are single fields (checkbox only).
- **`empty()`** lets the compositor short-circuit a layer with no effects to today's exact path (a
  hard requirement — untouched layers must stay byte-identical).
- **Serialization** — `LayerEffects` round-trips to the native `.mosaic` format (S48) and, best-
  effort, to SVG filters on export (drop-shadow/blur map to `<filter>`; overlays bake). LE-g.
- **Undo** — one coalesced `SetLayerEffectsCommand` per gesture (drag = one step), mirroring
  `SetTextCommand`; the footprint is the layer id + its `effectsBounds` region (S36-b friendly).

---

## 4. The render pipeline — the compositor seam

The compositor already hands us exactly what effects need. `renderLayer()`
(`src/render/compositor.cpp:726`) renders each leaf/group into its **own isolated document-space RGBA
`ImageF` with straight alpha**, *before* `walkStep()` (`:678`) blends it with the layer's
opacity/blend/mask. Effects insert **between those two**, in that one seam (shared by the full walk
*and* the `DragCompositeCache` replay, so they must stay bit-identical):

```
renderLayer(L)  ->  ImageF layerRGBA (with alpha A)          // unchanged
      │
      ▼   applyEffects(layerRGBA, L.effects()):              // NEW (LE-a)
      │     below  = ⊕ DropShadow(A)…, OuterGlow(A)          // painted UNDER the layer
      │     mid    = layerRGBA × fillOpacity                 // the layer's own pixels
      │              ⊕ ColorOverlay ⊕ GradientOverlay ⊕ PatternOverlay   (clipped to A)
      │              ⊕ Satin ⊕ InnerShadow(A)… ⊕ InnerGlow(A)            (clipped to A)
      │              ⊕ Bevel(heightFromA)                    // shades mid using A's height field
      │     above  = ⊕ Stroke(A)…                            // concentric, may extend beyond A
      │     result = below ⊕ mid ⊕ above                     // one straight-alpha ImageF
      ▼
walkStep(L, result, blend)   ->  acc                         // unchanged: layer opacity/blend/mask
```

- **Canonical z-order** (fixed, not user-reorderable — matches the fixed catalogue): drop shadow →
  outer glow → *layer fill* → colour → gradient → pattern overlay → satin → inner shadow → inner glow
  → bevel → stroke. This is the Photoshop/Affinity convention; the exact interleave is a tunable
  constant, but the *set* and *direction* are fixed (§1.3).
- **`effectsBounds()`** — a new `Layer` query = `contentBounds()` ⊕ (max drop-shadow/glow/outside-
  stroke reach). The dirty-region recomposite (S60) and the isolated-buffer allocation must grow by
  this so an effect drawn *outside* the content box isn't clipped. For a group it composes over the
  children's effect bounds.
- **Shared primitives (LE-a):** a separable **Gaussian blur** and a **fast box-blur** triple-pass
  approximation for large radii (Kovesi, *Fast Almost-Gaussian Filtering*), plus a **signed-distance
  transform** of the alpha (Felzenszwalb–Huttenlocher 2004) — the common engine behind stroke
  (offset the SDF band), glow/shadow (blur the alpha), and bevel (height = clamped SDF). These are
  **the same blur** S33 needs, so LE-a builds it once and S33 reuses it.
- **CPU-only lane is the contract.** `applyEffects` is pure CPU float math (like the vector
  rasterizer); a future Vulkan compute lane (S60) mirrors it behind a `render::*` override with
  parity tests, exactly like `ExtrudeRenderOverride` (`extrude_render.hpp:92`). Never GPU-only.

---

## 5. The effect catalogue

Each effect below reads the layer's isolated RGBA + its alpha `A` and emits float RGBA. All fills
reuse `vec::Paint`, so **solid / gradient / pattern** work in strokes and overlays uniformly.

### 5.1 Stroke — *stackable, concentric*
Offset the alpha edge by ±`width` from the SDF of `A`, fill the resulting band with `paint`. `Inside`
clips to `A`; `Outside` extends beyond it (grows `effectsBounds`); `Center` straddles. **Stacked**
strokes render index-0 innermost, each offset outward by the cumulative width — the white/black/white
concentric look the user described. *Lineage: SDF-from-coverage is textbook (Danielsson 1980 EDT;
Felzenszwalb–Huttenlocher 2004).*

### 5.2 Drop / Inner Shadow — *stackable*
**Drop:** blur `A` by `size`, choke by `spread`, offset by (`angle`,`distance`), colourise, place
**below** the layer. **Inner:** blur `(1−A)`, offset inward, **clip to `A`**, place above. Both are
Global-Light-aware (§ LE-g). *Lineage: gaussian blur — textbook.*

### 5.3 Colour / Gradient / Pattern Overlay — *single each, independent*
Fill `A` with the overlay's `paint`, composite over the layer with the overlay's own **blend +
opacity**. Independent (a colour tint can sit over a gradient). The only forbidden config is a
**literal no-op** — two `Normal @ 100 %`-opaque fills where the top fully hides the bottom — which the
UI simply prevents from reading as "both do something" (a soft notice, not a hard lock). Gradient uses
the existing renderer (`raster.cpp:54-75`); Pattern uses §7.

### 5.4 Outer / Inner Glow — *single each*
**Outer:** blur `A` outward by `size`, colourise with `paint`, place below (or additively around).
**Inner:** blur inward from the edge (or centre), clip to `A`. *Lineage: gaussian blur — textbook.*

### 5.5 Bevel & Emboss — *single*
Height field `h = clamp(SDF(A)/size)`; normal map `n = normalize(∇h, 1)` (single-pass Sobel);
raked light at (`angle`,`altitude`) → highlight where `n·L > 0`, shadow where `< 0`, composited with
the highlight/shadow colours. *Lineage: Blinn 1978 bump-mapping — public domain. **Single-pass**
normal-from-height only; iterative/pyramid variants are deliberately excluded (see §10), and the
Texture Generator holds the same line.*

### 5.6 Satin — *single*
Duplicate `A`, offset by (`angle`,`distance`), blur by `size`, XOR/interfere with itself (`invert`),
clip to `A`, colourise — the folded-fabric sheen. *Lineage: classic image-op composition — textbook.*

---

## 6. The modal UI

### 6.1 Shape & substrate
A modal `Fl_Double_Window` cloned from **`SettingsDialog`** (`src/ui/settings_dialog.cpp:786`) — the
proven two-pane host with a rail, parallel content, `set_modal()`, and the **child-sub-window
creation order** (ColorFlyout / DropdownPopup / ContextMenu built *before* `end()`/`show()`) that
avoids the FLTK stray-top-level trap. Opened from **`Layer ▸ Layer Effects…`** (a `menu->add()`
inserted **before** `app_window.cpp:219` so it's the first Layer item; a `cbLayerEffects` thunk near
`:4204` → new `MainWindow::openLayerEffects()`, modelled on `openSettings()` `:1622`).

### 6.2 Layout — catalogue · preview · stack
Three zones: a slim **left catalogue rail**, a **preview pane** pinned at the top of the content area,
and the scrollable **instance stack** below it.

- **Left — the catalogue.** A fixed, ordered list (a `LayerRow`-style row, `layer_panel.hpp:47`) of
  every effect type: a `ui::CheckBox` (`widgets.hpp:116`) to enable, the name, and — for stackable
  types — the **`− [n] +` stepper** (net-new; no spinner widget exists today). The stepper adds/
  removes instances. Clicking a row scrolls the stack to that effect.
- **Preview pane (in-modal, primary).** A pinned live thumbnail at the top of the content area that
  renders the **selected layer with the pending effect stack** through the same `applyEffects`
  path (§4) — so you never *have* to move the modal to see the result. It shows the layer framed
  against a checkerboard (to read shadows/glows/transparency) with a fit/actual toggle, and updates on
  every control change via the low-res proxy (§8). This is the same in-dialog preview idea as the Fill
  dialog's `PreviewPane` (`fill_dialog.cpp:82`) and the Texture-Generator modal's gizmo/preview pane.
- **Stack — the live editable effects.** A `ScrollView` (`widgets.hpp:326`) of **collapsible instance
  panels** headed by `DisclosureButton` (`type_panel.cpp:157`). One panel per *enabled instance*, so
  Stroke ×3 = `Stroke 1 / 2 / 3`, each independently editable. Each panel's controls are the Type-
  panel idiom: captioned `ScrubSlider`s, `Dropdown`s (incl. `addBlendModeItems`), a
  `SwatchChip`→`ColorFlyout` colour line (`widgets.hpp:149`, `color_flyout.hpp:28`), and — for
  gradient/pattern — the flyouts of §7. Edits route through a host funnel (the
  `setOnBlockEdit`/`applyControl` pattern, `type_panel.cpp:740`) with per-control coalesce ids so a
  drag is one undo step.

### 6.3 Live preview + the fx badge
- **Two previews, both live.** The **in-modal preview pane** (§6.2) is the primary — it always shows
  the result without moving the modal. Additionally, the document itself composites *with* the pending
  effects **on the canvas** (transactional, the Fill-dialog preview-commit pattern), so sliding the
  modal aside shows the effect in full document context. Commit on **OK**, revert on **Cancel**. Both
  are driven by the same `applyEffects` result, so they never disagree.
- **Cost:** compositing is CPU, so both previews use a **low-res proxy** during continuous drags and
  settle crisp on release (§8). The in-modal thumbnail is cheap (small); the canvas proxy is the
  expensive one and is what the proxy path chiefly protects.
- **The fx badge:** a layer with effects shows a small **fx badge** on its `LayerRow`; clicking it
  **re-opens** the modal for that layer. (There are **no on-canvas handles** — the badge is the only
  affordance beyond the menu.) A later nicety (LE-g): a Photoshop-style expandable effect sub-list
  under the row.

### 6.4 Colour-type bridge
The colour UI speaks `Color8`; effect paints speak `ColorF` (`src/common/image.hpp:24`). LE-a adds the
one `Color8 ↔ ColorF` bridge the `ColorFlyout`-backed controls need (lcms2-aware where a working-space
conversion is warranted, `core/color_management.*`).

---

## 7. Patterns (the `Pattern` paint type + the flyout)

`vec::Paint` reserves `Pattern` (`src/core/vector/paint.hpp:44,49`) but nothing implements it. LE-d
adds it as a **variant of two honest kinds** (user call 2026-07-03):

```cpp
// src/core/vector/pattern.hpp  (LE-d)  — added to the Paint variant
struct ProceduralPattern {               // resolution-independent, scalable, colour-editable
    enum class Kind { Dots, Grid, Lines, Hatch, CrossHatch, Checker, Herringbone,
                      Scales, Halftone, Grain, Bricks, Triangles } kind = Kind::Dots;
    ColorF fg{0,0,0,1}, bg{0,0,0,0};     // fully colour-editable
    float scale = 32.0f, angleDeg = 0.0f, weight = 0.5f;   // feature size in layer units
    std::map<std::string,float> params;  // per-kind extras (dot radius, brick offset…)
};
struct ImagePattern {                    // fixed-resolution bitmap tile
    ImageId tile;                        // a bundled/imported/from-selection tile
    float scale = 1.0f, angleDeg = 0.0f; Vec2 offset{0,0};   // scale is a MULTIPLIER of native px
    // native tile size is shown in the flyout so the quality ceiling is visible
};
using Pattern = std::variant<ProceduralPattern, ImagePattern>;
```

### 7.1 Procedural — the flagship
Regenerated at the **document's pixel resolution**, so crisp at any `scale` and any zoom (in a raster
document the *viewport* still magnifies composited pixels — true view-res re-eval is the Vector-
document story, S30-b — but the pattern is never a baked low-res tile). Fully **colour-editable**,
always seamless by construction, tiny on disk, deterministic. Several of the worthwhile *geometric*
GIMP tiles (bricks, weave, squares) are **reimplemented here procedurally** rather than imported —
strictly better, because they become resolution-independent.

### 7.2 Image — fixed-resolution, honest ceiling
Bitmap tiles come **only from the user** — **Make Pattern from Selection** + **Import image**. `scale`
is an honest **tile multiplier** — crisp at ≤ native (downsampled), softening above native (standard
bitmap-pattern behaviour, as in Photoshop's pattern fills). The flyout **shows the native tile size**
so the quality ceiling is visible. High-quality *scalable* materials (paper, fabric, the "expensive
cardstock") come from the **Texture Generator**, not here. The tile sampler, the make-from-selection /
import constructors, and the tile-store / serialization plan are detailed in
**`docs/le-d2-image-patterns.md`** (LE-d2).

> **SUPERSEDED (2026-07-04):** the earlier "curated slice of the GIMP pattern set" is **dropped** —
> Mosaic bundles **no** pattern tiles (1 MB/tile vs a ~5 MB binary; procedural covers the built-in
> geometric need). See `docs/le-d2-image-patterns.md` §5. Everything below about GIMP tiles / their
> licence is historical.

### 7.3 GIMP licence diligence
GIMP is GPL, so its patterns are broadly redistributable, but the stock set is a grab-bag of mixed
provenance. **We bundle only a curated, per-file licence-checked subset**, record each tile's origin
and licence in `docs/third-party-licenses.md`, and drop anything with murky origins. (This was the one
real diligence task in this doc — see §10.)

### 7.4 The Gradient flyout (reusable machinery)
A multi-stop stop editor: a **stop strip** (add/drag/delete stops, per-stop `ColorFlyout`), a
**spread** control (pad/repeat/reflect), and an **in-flyout interactive preview** whose handles drag
the gradient's start/end (linear), centre/radius (radial), or angle (conic) — **inside the flyout,
never on the canvas** (§1.8). It writes `Gradient::transform`; the renderer already consumes it
(`raster.cpp:67-75`). The flyout owns **stops + spread only**; the **type** is the parent's (the
Gradient-Overlay row here; the future Gradient tool's context bar later). This is the reusable
gradient machinery both features share.

---

## 8. Performance & the CPU-only lane

- **CPU reference is permanent, not a stopgap.** `applyEffects` is pure float CPU math. At 24–36 MP a
  full effect stack (several blurs) costs on the order of seconds — acceptable for commit, painful for
  a live drag, hence:
- **Low-res proxy preview.** During a continuous slider/handle drag, preview at a downsampled
  resolution (the text-perf lever, mosaic-project memory item 11-b) and settle crisp on release. The
  blur radius scales with the proxy, so the look is representative.
- **GPU lane at S60.** The seam is designed so `applyEffects` gets a Vulkan-compute sibling behind a
  `render::*` override with CPU↔GPU parity golden tests (the extrude-lane pattern). Rotate/recolour/
  reblur are shader-uniform-cheap once the compositor is GPU-resident. **Never** GPU-only.
- **Short-circuit.** `LayerEffects::empty()` ⇒ the layer takes today's exact `renderLayer→walkStep`
  path, byte-identical. No effect, no cost.

---

## 9. 3D text — per-face effects (S30-e)

The 3D-text integration is **already scheduled** (`docs/type-tool.md` §12) and depends on LE-a…c. The
mesh already bakes everything it needs: per-vertex **design-space UVs** normalised over
`ExtrudeMesh::designBounds` and a per-triangle **`cap` flag** (`extrude_mesh.hpp:23`), plus the
front-cap homography **`ExtrudePlaneMap`** (`extrude_render.hpp:57`, `from/project/unproject`).

- **Overlays** (colour/gradient/pattern) are evaluated in the glyph's **2D design space** and mapped
  onto the **front cap** via the UVs; the walls/bevel inherit a shaded continuation (not a re-
  projected copy — the "looks insanely weird" outcome the user named). An optional *wrap-to-sides*
  mode exists, off by default.
- **Stroke** on 3D text is a true edge treatment on the projected silhouette; **shadow/glow** operate
  on the composited 2D result (after the 3D pass), so they need nothing special.
- S30-e builds the per-face mapping + the wiring; the model here guarantees the sane sampling domain
  it draws on. It waits on LE-a…c, not on a design question.

---

## 10. Technique lineage & licences

- **Layer effects are long-published UX.** Photoshop 5.0 shipped layer effects in **1998**; the whole
  catalogue (drop shadow, bevel, glow, stroke, overlay, satin) is 27-year-old published UX. No effect
  *concept* here is novel.
- **Every technique is textbook / public domain:** gaussian & box blur; signed-distance transforms
  (Danielsson 1980; Felzenszwalb–Huttenlocher 2004); Blinn 1978 bump-mapping (bevel); the W3C
  Compositing & Blending blend math already in `src/render/blend.hpp`. Design from these, not from any
  vendor's pipeline; **no machine learning** anywhere.
- **⚠ INVARIANT — normal-from-height must be single-pass Sobel** for the bevel. *Iterative* and
  *pyramid* height-to-normal variants are deliberately not used and must not be introduced; the
  Texture Generator holds the same line, so the two stay consistent.
- **Procedural pattern noise** (if any Grain/organic kind uses noise): classic Perlin (1985) or
  simplex. **Wavelet noise is deliberately excluded** and must not be added.
- **Licence diligence** was only ever about the bundled **GIMP pattern tiles** (§7.3) — now moot,
  because Mosaic bundles none. If a third-party asset ever does ship, its per-file provenance and
  licence go in `docs/third-party-licenses.md`.
- **Source-header lineage** (per project precedent) on the effects renderer:
  ```
  // Layer effects. Technique lineage:
  //   - Blend math: W3C Compositing & Blending L1 (src/render/blend.hpp).
  //   - Blur: separable Gaussian + fast-box (Kovesi). SDF: Felzenszwalb–Huttenlocher 2004.
  //   - Bevel: Blinn 1978 bump-mapping; SINGLE-PASS Sobel normals only.
  //   - Effects as a concept: Photoshop 5.0 (1998). No ML.
  ```

---

## 11. Decisions (2026-07-03) & residual questions

**Resolved this round (all with the user):**
1. **Surface** — modal two-pane on the Settings substrate, live canvas preview, reopened via the fx
   badge, **no on-canvas handles** (§1.2, §6).
2. **Panes** — left fixed catalogue (checkbox + `−[n]+` stepper), right scrollable instance stack
   (§1.3, §6.2).
3. **Overlays** — independent, each with blend + opacity (§1.4, §5.3).
4. **Rendering** — CPU-first with a permanent CPU-only lane; GPU at S60 (§1.5, §8).
5. **Canon** — full set **+ Satin** (§1.6, §5).
6. **Patterns** — Procedural (scalable, editable) + Image (curated GIMP tiles + import + from-
   selection, honest scale ceiling); advanced materials → Texture Generator (§1.7, §7).
7. **Gradients** — reusable flyout with in-flyout handles; type owned by the parent (§1.8, §7.4).
8. **Fill-opacity + group effects** — both in scope (§1.9).
9. **Texture Generator** — a *separate* feature (`docs/texture-generator.md`), not an effect (§1).
10. **Smash-through-ground** — parked as its own future doc (§1.11).

**Residual (small, resolve in-session, not blocking):**
- The **exact category labels** in the pattern flyout ("Procedural" / "Image" vs alternatives) — a
  wording call during LE-d.
- The **canonical interleave constant** (e.g. bevel-vs-overlay order) — verify against PS/Affinity
  during LE-e/-f; the *set* and *direction* are fixed.
- Whether the **fx badge** also gets the Photoshop expandable-sub-list under the layer row — a LE-g
  nicety, not v1-blocking.
- The **`.mosaic`/SVG serialization** schema for effects — settled in LE-g against S48.

---

## 12. References

- **Lineage:** Adobe Photoshop 5.0 (1998) — layer effects; Affinity Photo — Layer Effects (fx).
- **Blur:** P. Kovesi, *Fast Almost-Gaussian Filtering* (2010). **SDF:** Felzenszwalb & Huttenlocher,
  *Distance Transforms of Sampled Functions* (2004); Danielsson, *Euclidean Distance Mapping* (1980).
- **Bevel:** Blinn, *Simulation of Wrinkled Surfaces*, SIGGRAPH 1978.
- **Blend math:** *Compositing and Blending Level 1*, W3C — already in `src/render/blend.hpp`.
- **Companions:** `docs/type-tool.md` (§12 the 3D per-face contract), `docs/vector-model.md` (§2.3 the
  `Paint` seam, §4 the gradient renderer), `docs/texture-generator.md` (the sibling generator),
  `docs/third-party-licenses.md` (GIMP-pattern provenance), **PLAN.md** Phase 5.
