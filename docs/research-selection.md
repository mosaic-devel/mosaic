# Colour-based selection — research and build plan (S17 magic wand)

**Status:** research note / design doc — the deliverable **S17** (PLAN §0: *"Research-heavy sessions
(S17, S18, …) must begin by writing/refreshing their `docs/` research note before any code"*; PLAN §9
S17 entry) is required to produce before any magic-wand code lands.

**Scope.** PLAN scopes `docs/research-selection.md` as the **shared foundation note for both S17
(magic wand) and S18 (select brush)** (PLAN §9 S17/S18; the S16 gotcha block, *"S18's select brush
shares the note — scope it for both"*). This file therefore covers the **shared selection substrate**
(mask representation, colour space, distance metrics, feather, anti-aliasing, the boolean-op
conventions, the canvas→host commit path) in full, and covers the **magic wand** end-to-end. S18's
**brush-specific** material (paint-to-select dab model, soft-brush coverage accumulation) and the
**Select menu** (all/none/inverse, grow/shrink/feather/smooth, Mask-from-selection, the marching-ants
direction experiment) live in a **separate** note, `docs/research-select-brush.md` — this file
cross-references it rather than duplicating it.

> **§7 records the lineage this wand descends from and the modern "smart-selection / refine-edge /
> matting" families it deliberately excludes.** Those exclusions are settled design decisions.

---

## 1. The current selection substrate — what actually exists

Everything the wand produces is a `core::Selection`, and that type already exists and is already the
document's single selection representation. **The wand needs no extension to it.** Grounding:

- **Representation.** `core::Selection` is a **flat, document-sized `std::vector<std::uint8_t>`
  coverage mask**: `0` = unselected, `255` = fully selected, intermediate = anti-aliased / feathered
  fractional coverage (`src/core/selection.hpp:9-19`, members at `:76-79`). It carries `m_width` /
  `m_height` and nothing else — no offset, no tiling.
- **"No selection" ≠ "selected nothing".** A default-constructed, empty `Selection` (`isEmpty()`,
  `src/core/selection.hpp:49`) means *"everything is editable"*; an all-zero same-size mask is an
  active selection of nothing and actively blocks every edit. The gesture layer is careful to land
  "no selection" rather than an all-zero mask whenever a result covers no pixels
  (`src/ui/selection_gesture.cpp:278-279`). **The wand must obey the same rule.**
- **Boolean ops, in coverage semantics** (`src/core/selection.cpp:110-149`): `Replace` adopts `b`;
  `Add = max(a,b)`; `Subtract = a·(255−b)/255`; `Intersect = min(a,b)`. An empty `a` acts as a zero
  mask; combine requires equal dimensions.
- **Producers already in `core`** — the wand joins these, mirroring their shape: `Selection::rectangle`
  (`:14`), `Selection::ellipse` (anti-aliased, `:88`), `Selection::polygon` (analytic-horizontal /
  8× sub-scanline-vertical AA fill, `:27-86`), and the free function `selectionFromLayerPixels`
  (samples a layer's alpha through its transform, `:214-251`). All are **pure, GPU-free, headless-
  unit-tested** — the model the wand's core engine copies.
- **`bounds()`** returns the tight integer bbox of coverage>0 (`:190-212`); `anySelected()` at `:157`;
  `inverted()`, `cropped()` exist. **The wand needs no new query.**
- **Tiling is explicitly deferred.** Storage is "a flat document-sized buffer for now — tiling arrives
  with the S60-c storage work" (`src/core/selection.hpp:19`). The wand does **not** pull that
  forward; it allocates the same flat mask every other producer does.

**Verdict: the substrate is sufficient as-is.** The 8-bit channel already expresses AA and feather;
`bounds()` already exists; the boolean ops already exist. The wand is a new *producer* of `Selection`,
nothing more. The one memory reality to record (not a change): the flood needs a **visited buffer**
distinct from the coverage output (a rejected pixel and an untested pixel both read coverage 0, so the
output cannot double as the visited set) — a `std::vector<bool>` (1 bit/px) or equivalent. Global
(whole-image) match needs no visited buffer at all.

---

## 2. Colour space and distance metric

### 2.1 What space the pixels are in *today*

The compositor and the document's pixels are in the document's **encoded (gamma) space**, RGBA8:
"Compositing is in the document's encoded (gamma) space for now — S12-b brings **picker-level** colour
management only (lcms2 in the picker + the gamut warning); the compositor's **linear-light + ICC
re-plumb is S43-b**" (PLAN §3.6, quoted at PLAN lines 560-563; model pixels stay RGBA8, float
migration S43-a). Concretely, the wand's two possible pixel sources are both encoded RGBA8:

- the active layer's `RasterLayer::image()` (a `common::Image`, 8-bit RGBA — `src/common/image.hpp:51-69`);
- the raw composite `m_lastComposite` (`common::Image`, produced with `checkerboard=false`, real alpha —
  the same source Copy Merged flattens from, `src/ui/app_window.cpp:3676-3683`).

The lcms2 `ColorEngine` **does exist** but is **picker-only**: `toLab(Color8)` runs a per-call lcms2
transform of a *single* pixel (`src/core/color_management.cpp:224-229`). Running that per pixel over a
40-megapixel canvas is out of the question for an interactive click.

### 2.2 Metric options

| Metric | Cost | Perceptual quality | Notes |
|---|---|---|---|
| **RGB Euclidean** (or per-channel max/abs) | trivial | poor (green over-weighted, blue under) | what a naïve wand does; the literal Photoshop-0-255 "difference" is a per-channel threshold in this class |
| **Weighted-RGB Euclidean** (luma weights, or "redmean" low-cost approximation) | trivial | decent; cheap approximation of perceptual distance in an *encoded* buffer | recommended for **today** |
| **CIELAB ΔE\*76** | needs RGB→Lab; then plain Euclidean-in-Lab | good; **monotonic, smooth, cheap once in Lab** | recommended metric for **S43-b** |
| **CIELAB ΔE\*94 / ΔE\*00 (CIEDE2000)** | needs Lab + a heavier formula (ΔE00 has hue/chroma rotation terms and known discontinuities) | best perceptual accuracy | more than a *tolerance* control needs; overkill for a threshold whose absolute units the user never reads |

### 2.3 Recommendation + migration path

- **Today (encoded space): weighted-Euclidean RGB in the document's encoded space.** Rationale: (1) it
  is the space every other sampling tool already reads (eyedropper, bucket fill), so the wand agrees
  with what the user sees and picks; (2) no lcms2 in the hot loop; (3) luma-weighting (or redmean)
  buys most of the perceptual win of Lab for none of the cost. Distance over **RGBA** (include alpha)
  so a click on a transparent region forms its own region (see §8, open question OQ-4).
- **At S43-b (linear-light + ICC): move to ΔE on managed CIELAB.** Once pixels are linear/managed,
  convert the *sampled region* to Lab **in one batch** (not per-pixel-per-call): either a single
  `ColorEngine` batch transform of the source image (lcms2 transforms an array in one call) or a
  coarse 3-D LUT. Then measure **ΔE\*76** (Euclidean-in-Lab) by default — monotonic and cheap — with
  **CIEDE2000 as an optional high-accuracy mode**. ΔE00's cost and its non-smoothness make it a poor
  default for a slider-driven threshold; ΔE76 is the right accuracy/behaviour trade for tolerance.
- **Structure the metric behind a pure function / small interface now** (`float distance(PixA, PixB)`),
  so the S43-b swap (weighted-RGB → ΔE-on-Lab) touches the metric only, never the flood engine. This
  is the same "design the seam so the later re-plumb is local" posture PLAN takes for S43.

CIEDE2000 / ΔE94 / ΔE76 are published CIE/ISO standards (ISO/CIE 11664-6:2022), freely
implementable.

---

## 3. Tolerance semantics

- **What the number means.** Tolerance is a **threshold `T` on the distance metric** (§2): a candidate
  pixel is *in* iff `distance(candidate, seed) ≤ T` (contiguous flood also requires connectivity, §4).
  The seed colour is the clicked pixel's colour in the chosen source (active layer / merged).
- **Units the user sees.** Expose it as a **0–100 slider** (`ToolOptionKind::Slider`,
  `src/ui/tool.hpp:76-84,102-137`) mapped onto the metric's natural range (for encoded weighted-RGB,
  0–100 → 0…max-representable-distance). This keeps the control metric-agnostic so the S43-b swap does
  not renumber the user's muscle memory. (Photoshop uses 0–255 because its wand thresholds 8-bit
  channel differences directly; we deliberately abstract that.)
- **What makes it feel linear.** On natural images, *selected area* grows **super-linearly** with `T`
  (a small threshold bump past an edge floods a whole new region), so a slider that is linear in `T`
  feels "nothing… nothing… everything." Two levers, both already supported by the option model:
  - keep the slider **linear in `T`** (predictable, matches every other editor's wand; recommended
    default), or
  - apply a **response curve** — `ToolOption` already carries `ResponseCurve{Linear,Gamma,Log}` +
    `curveK` (`src/ui/tool.hpp:95,131-133`) — to give the low end more travel.
  - **Recommendation:** ship **linear-in-`T`** (OQ-2); the response curve is a one-line follow-up if
    users report the "everything at once" cliff. Do not silently curve it — a linear tolerance is what
    the class expects, and "strictly-better" does not apply to a UX-consequential feel decision.

---

## 4. Contiguous (flood fill) vs global (whole-image match)

- **Contiguous** (Photoshop default): select the connected region of within-tolerance pixels reachable
  from the seed. This is a classic **seed / flood fill**.
- **Global** ("select all matching", Contiguous off): select **every** pixel in the image within
  tolerance of the seed colour, ignoring connectivity — a single linear scan, O(N), no visited buffer.

Expose the choice as a **`Contiguous` Toggle** in the options bar, default **on**.

**Flood algorithm.** Use **span/scanline flood fill** (Heckbert-style): push seed span, pop a span,
fill its run, scan the rows above/below for new within-tolerance runs, push those. This visits each
pixel O(1) times, is cache-friendly (row runs), and keeps the worklist proportional to the number of
*spans* rather than *pixels* (far smaller than a per-pixel BFS queue). A plain 4-neighbour BFS/DFS is
simpler and also correct; the span version matters only at the 40-MP scale below.

- **Connectivity: 4-connected** (recommended, OQ-3). 8-connected leaks the selection through
  single-pixel diagonal touches (JPEG/noise artefacts bridge regions) — 4-connected matches Photoshop
  and is the safer default.
- **Complexity / memory on large canvases.** Worst case visits all N = W·H pixels: O(N) time. Memory:
  the output `Selection` mask is N bytes (e.g. **40 MB for a 5k×8k = 40-MP layer**, the size PLAN
  already flags for Move-tool lag, PLAN §2), plus a **visited buffer** (1 bit/px ≈ 5 MB at 40 MP;
  distinct from coverage, per §1). The `SetSelectionCommand` then stores **old + new** full masks
  (`src/core/commands.hpp:420-436`, whose own comment warns *"heavy for huge documents, acceptable
  until tiled storage (S60-c)"*) — ~80 MB per undo step at 40 MP. This is the **stack-depth-cap**
  pressure PLAN's S17 gotcha block anticipates (PLAN lines 588-591): with the History panel now
  visible, an unbounded stack of full-mask commands is memory-heavy. If we cap, **evict oldest from
  the bottom and say so in the panel** (record the choice here — see OQ-8). No new storage work for
  S17; this is a *documented* consequence, and the tiled fix is S60-c.

---

## 5. Anti-aliased edge coverage

A binary in/out test gives a jagged 1-bit boundary. To get fractional coverage (the "Anti-alias"
checkbox), turn the hard threshold into a **soft distance ramp** at the boundary:

```
coverage(d) = 1                       for d ≤ T − f      (solidly inside tolerance)
            = (T + f − d) / (2f)      for T − f < d < T + f   (linear AA band)
            = 0                       for d ≥ T + f      (solidly outside)
```

`d` is the metric distance to the seed, `T` the tolerance, `f` a small fixed half-width (the AA band,
~ a few metric units / a fraction of a slider step). This produces exactly the fractional 8-bit
coverage `Selection::polygon`/`ellipse` already emit (`src/core/selection.cpp:79-83`), so it is
type-identical to the marquee output.

**Two subtleties specific to a *contiguous* flood:**

1. **Connectivity must gate on the hard predicate, not the soft one**, or the flood leaks through the
   AA band into neighbouring regions. Recommended structure: **flood on `d ≤ T`** (hard), producing the
   solid interior; then in a **one-pixel boundary post-pass**, for each selected pixel's *unselected*
   neighbours compute `coverage(d)` from the ramp and write the fractional value. This yields a clean
   ~1-px anti-aliased edge exactly where Photoshop's "Anti-alias" puts it, without letting the soft
   band bridge regions. (Global match has no connectivity, so it can apply the ramp directly to every
   pixel.)
2. **Default: Anti-alias on** (OQ-5), matching the class. Expose as an `Anti-alias` Toggle.

**Interaction with the marching-ants pass — none required.** The ants present pass already consumes
8-bit coverage and renders the boundary at the **≥ 0.5 bilinear iso-contour** (the selection_gesture
preview comment documents the "bilinear ≥ 0.5" test, `src/ui/selection_gesture.hpp:112-116`). AA
coverage therefore draws ants along the 50 % contour automatically; feather (below) widens the soft
band but the ants still track the 50 % iso-line. The ants shader is untouched by S17. (The ants
*direction* experiment is S18's, per PLAN §9 S18 — cross-ref `docs/research-select-brush.md`.)

---

## 6. Feather

Two ways to soften a selection edge over a radius `r`:

- **(a) Post-process Gaussian blur of the coverage mask.** Convolve the (binary or AA) mask with a
  separable Gaussian of radius `r`; the boundary becomes a smooth coverage ramp. This is exactly
  Photoshop's "Feather" semantics and is cheap (separable, O(N·r) or O(N) with a box-blur
  approximation).
- **(b) Signed-distance-field growth.** Compute the signed Euclidean distance to the boundary
  (Danielsson 1980 / Felzenszwalb–Huttenlocher 2004 exact EDT — published, §7.1), then
  remap through a `smoothstep` of width `2r`. Gives an exact-radius, symmetric feather and is the
  natural primitive for **grow/shrink** (expand/contract by N px = threshold the EDT).

**Recommendation.** For the **wand's own feather amount**, use **(a) Gaussian blur** — it *is* the
expected "Feather" look, composes correctly with the boolean ops and the ants, and is the **same
operation S18's `Select → Feather` needs**, so build it once and share it. Reserve **(b) the EDT** for
S18's `Select → Grow/Shrink` (where an exact pixel radius is the point). **Where feather lives is a
scope fork (OQ-6):** the wand can expose a `Feather` amount now (cheap, reusing the blur), or ship
tolerance+AA only and leave *all* feather to the S18 Select menu. The **Select-menu feather/grow/
shrink/smooth UI, and the shared blur/EDT helpers, are specified in `docs/research-select-brush.md`**
(S18) — this note recommends the wand *reuse* them rather than grow its own.

---

## 7. Technique lineage and deliberate exclusions

What Mosaic's wand is built from, and the modern families it declines to build. Per project rule
these are records of what *Mosaic* does — **not** assessments of any other project's code, and no
third-party editor is named as practising any technique.

### 7.1 What Mosaic implements

- **Seed / flood fill (scanline span fill; 4-connected).** Region-filling by connected traversal is
  textbook computer graphics from the **1960s–1970s** (span/scanline seed fill; Smith, *"Tint Fill,"*
  SIGGRAPH 1979).
- **The magic wand itself (seed + colour tolerance + flood, contiguous/global).** Shipped in
  commodity raster editors since **1990** and re-implemented ubiquitously since. "Click a pixel,
  select the connected/all pixels within a colour tolerance" is 35+ years of dense, published
  practice.
- **Global colour match ("select by colour" / colour-range threshold).** Same era, same status.
- **Colour-distance metrics.** RGB Euclidean and weighted-RGB (incl. the "redmean" low-cost
  approximation) are trivial arithmetic. **CIELAB ΔE\*76 (1976), ΔE\*94 (1994), and CIEDE2000** are
  **published CIE/ISO international standards** (ISO/CIE 11664-6:2022), freely implementable, with
  public reference implementations (e.g. Sharma's CIEDE2000 notes).
- **Anti-aliased edge coverage via a soft distance-to-tolerance ramp.** Producing fractional coverage
  by linearly ramping a threshold is elementary and mid-1990s-old (the wand "Anti-alias" checkbox).
- **Feather.** Gaussian blur of a mask, and the exact **Euclidean distance transform** (Danielsson
  1980; Felzenszwalb–Huttenlocher 2004), are both classical published technique.

### 7.2 Technique families Mosaic declines to build

These are the modern "smart selection / refine-edge / matting" families. Mosaic's wand builds **none**
of them; it is a stateless per-click flood on a fixed tolerance with a plain distance-ramp AA and an
isotropic feather. Recording the exclusions keeps the design legible and deliberate.

1. ⚠ **Coherent / learned-region interactive segmentation with an on-canvas adjustment marker** (the
   "paint-to-select by region statistics / graph-cut quick-select + centroid handle" family).
   **Mosaic declines the whole family:** the wand does **no** coherent-region classification and
   places **no** on-canvas selection-adjustment marker or handle at a mask centroid. *(The same
   constraint binds S18's select brush, which is likewise brush-stroke-driven — see
   `docs/research-select-brush.md`; that brush must stay a plain soft-coverage dab, never a
   learned-region graph-cut segmenter.)*
2. ⚠ **Automatic edge-detected matting-region refinement / hybrid level-set hard+soft selection**
   (the "Refine Edge / Select-and-Mask"-class boundary matting). **Mosaic declines the whole
   family:** our boundary is a plain distance-to-tolerance AA ramp (§5) and our feather is isotropic
   blur / EDT growth (§6) — **no alpha matting, no level set, no automatically-defined matting band
   along detected object edges, no learned edge model.**
3. ⚠ **Guidance-image ("guided filter") / joint-bilateral edge-aware feather refinement.**
   **Mosaic's feather is deliberately the plain isotropic kind** — Gaussian blur or Euclidean
   distance growth (§6) — which needs no guidance image and is decades-old public-domain math. We
   neither need nor build guided / joint-bilateral edge-aware feathering for the wand. **Guardrail:
   if S18/S31 ever want edge-aware feather, that is a deliberate reversal of this exclusion, and it
   is to be raised as one — do not just reach for a guidance-image filter.**

### 7.3 Headline

The magic wand's every ingredient — flood fill, colour-tolerance thresholding, contiguous/global
match, weighted-RGB / CIE-ΔE distance, distance-ramp AA, blur/EDT feather — is **classical,
published, decades-old technique.** The whole modern **smart-selection / refine-edge / matting**
space (§7.2) is **declined by design** — S17 ships a classical wand, not a learned or matting-based
selector.

---

## 8. Where it plugs in (verified against the code)

The wand is a **selection tool** and reuses the S13/S14 selection plumbing wholesale.

- **Tool registration.** Add `ToolId::MagicWand` in group `ToolGroup::SelectTransform`
  (`src/ui/tool.hpp:19-49`) in its **own** slot `ToolSlot::MagicWand` (OQ-1: own slot recommended — it
  is a click tool with a distinct interaction, not a marquee/lasso drag variant; the marquee and lasso
  slots hold drag-gesture variants only, `src/ui/tool.cpp:102-126`). Suggested shortcut **"W"** (free).
  Register in `kToolDefs` alongside the others (`src/ui/tool.cpp:102-126`).
- **Options bar (S11-b)** — publish `ToolOption`s (`src/ui/tool.hpp:102-137`): `tolerance` Slider
  (0–100), `contiguous` Toggle (default on), `antialias` Toggle (default on), `source` Choice
  {Active Layer, All Layers} (default 0) — **mirroring the Eyedropper's exact "Source" option**,
  `src/ui/tool.cpp:359-362` — and optionally `feather` Number (OQ-6).
- **The click → one `SetSelectionCommand` funnel.** The wand is a single click, but it commits through
  the **identical** canvas→host path the marquee/lasso already use (PLAN S17 gotcha: *"produce ONE
  `SetSelectionCommand` per click through the existing canvas→host commit path"*):
  1. `VulkanCanvas` maps the click to a document point (`eventDocPoint()`,
     `src/ui/vulkan_canvas.cpp:362-364`);
  2. reads the seed pixel and the source image (active layer or merged, §8.1), computes the flood/global
     `Selection` in `core`, and **combines it onto the base** with the press-time `SelectOp`
     (`Selection::combine`, `src/core/selection.cpp:110`), landing "no selection" if the result covers
     nothing (§1);
  3. previews to the canvas mask (optional for a click — it is instant), then calls
     `m_commitSelection` — the second callback of `setSelectionHost`
     (`src/ui/app_window.cpp:450-454`) — which routes to `commitToolSelection`, pushing the single
     `SetSelectionCommand` and re-syncing the ants (`src/ui/app_window.cpp:4658-4662`). This is the
     exact funnel `finishSelectionGesture()` uses (`src/ui/vulkan_canvas.cpp:382-390`).
- **Press-time-modifier boolean ops — VERIFIED.** `selectOpForModifiers(shift, ctrl, alt)`
  (`src/ui/selection_gesture.cpp:50-58`) returns: **Shift = Add, Ctrl = Subtract, Shift+Ctrl *or* Alt =
  Intersect, none = Replace.** The canvas reads `Fl::event_state()` for Shift/Ctrl/Alt at press and
  calls it (`src/ui/vulkan_canvas.cpp:394-397`). The wand's click handler reads the same state and
  calls the same function — the modifier semantics are shared, not reimplemented.
- **Cursors.** The op-badge cursors are reusable: `ui::selectionCursor(core::SelectOp op, int scale)`
  (`src/ui/cursors.hpp:24`; canvas precedent `src/ui/vulkan_canvas.cpp:2809`) already draws the fine
  crosshair with the +/−/∩ badge per op. The wand uses it unchanged.

### 8.1 Sample-merged / sample-all-layers — which layer the wand reads

Mirror the Eyedropper "Source" precedent (`src/ui/tool.cpp:359-362`):

- **Active Layer** — read the active `RasterLayer::image()` (`src/core/layer.hpp` /
  `src/core/selection.cpp:216-219`). For a **transformed** layer, sample document→layer through the
  layer's inverse transform exactly as `selectionFromLayerPixels` does (identity + doc-sized → the 1:1
  fast path; else `inverse().apply({x+0.5,y+0.5})`, nearest — `src/core/selection.cpp:227-250`). A
  non-raster active layer (group/vector/text/adjustment) has no pixels → the wand is a no-op there
  (same as `selectionFromLayerPixels` returning `nullopt`).
- **All Layers ("sample merged")** — read the **raw composite** `m_lastComposite` (the checkerboard-free
  composite the cursor-readout already keeps current); recompute via `render::composite(*doc,
  {checkerboard=false}, Backend::Cpu)` when stale, exactly the staleness guard Copy-Merged / Smart
  analysis use (`src/ui/app_window.cpp:3676-3683`, `:4750-4764`). Use the **raw** composite, never the
  displayed (checkerboarded) one, so transparent areas carry real alpha (PLAN S17 gotcha lines
  583-587).

The **produced `Selection` is always document-sized** regardless of source (the source only decides the
seed colour and the per-pixel colours compared).

---

## 9. Build plan (commit-sequenced)

Format follows `docs/spell-check-plan.md` — one logical change per commit, `core` logic first and
headless-tested, the FLTK/GPU shell manually/visually verified last.

1. **Core flood/global engine + metric + AA + tests (no UI).** Add a pure `core` producer — a free
   function `magicWandSelection(const common::Image& src, common::Vec2 seedDocPt, const WandParams&)
   → core::Selection` (or `Selection::fromColorSelect(...)`), living beside `Selection::polygon`
   (`src/core/selection.{hpp,cpp}`). Implements: seed read, weighted-RGBA distance behind a swappable
   `distance()` (§2.3), 4-connected scanline flood (contiguous) and linear global scan, the
   hard-flood + boundary-ramp AA (§5), and the "no coverage → empty Selection" rule (§1). **Pure,
   GPU-free, doctest** with small hand-built images (solid regions, a diagonal-touch case to pin
   4-connectivity, an AA-edge coverage assertion, a global-vs-contiguous divergence case) — the same
   test posture `Selection::polygon`/`ellipse` already have.
2. **Feather + shared mask helpers** (or defer per OQ-6). If shipping wand feather now: a separable
   Gaussian-blur-of-mask helper in `core` (shared with the S18 Select menu), unit-tested. If deferring:
   skip this commit and cross-ref `docs/research-select-brush.md`; the wand ships tolerance+AA only.
3. **Tool registration + options bar.** `ToolId::MagicWand` / `ToolSlot::MagicWand` + `kToolDefs` entry
   + the option set (§8). Toolbar slot + options-bar rendering are existing machinery; no new widget.
   Wire `toolForShortcut('W')`. (Pure registry — unit-testable via `ToolManager`.)
4. **Canvas wiring.** In `VulkanCanvas`: on a click with the wand active, read the seed + source
   (active/merged, §8.1), call the core engine, combine with the base by the press-time `SelectOp`
   (§8), commit **one** `SetSelectionCommand` through `m_commitSelection`. Op-badge cursor via
   `selectionCursor`. Manually/visually verified by the user (per the headless-vs-visual division).
5. **Polish (optional).** Tolerance response curve (§3) if the linear feel disappoints; the
   scrub-to-tolerance gesture (OQ-7); and the stack-depth-cap decision surfaced by
   full-mask commands at scale (OQ-8, §4) — implement only if the user chooses to cap.

**Testing posture.** All flood/metric/AA/feather logic stays in `core` (doctest, synthetic images);
the canvas click→commit shell and the marching-ants render are the thin, user-visually-verified layer.
Golden-image diffs (`tests/golden/`, PLAN §3.15) can pin a wand result on a fixed test photo once
S18-b's real-image open path is exercised.

---

## 10. Decisions

- **D1. The substrate is sufficient as-is.** The wand produces a plain `core::Selection`; no extension
  to the type, no tiling, no new query. 8-bit coverage already carries AA + feather; `bounds()` exists.
  (§1)
- **D2. Distance today = weighted-Euclidean in the document's encoded RGBA space**, behind a swappable
  pure `distance()`; **at S43-b, swap to ΔE on managed CIELAB (ΔE\*76 default, CIEDE2000 optional)**
  via a one-shot batch/LUT transform, engine untouched. (§2)
- **D3. Tolerance = a 0–100 threshold on the metric, linear in `T` by default** (response curve is a
  follow-up lever, not shipped by default). (§3)
- **D4. Contiguous (4-connected scanline flood) is the default; global is a toggle.** (§4)
- **D5. Anti-alias via a hard-flood-then-boundary-ramp; on by default; the ants pass is untouched**
  (it already reads 8-bit coverage at the 0.5 iso-contour). (§5)
- **D6. Feather = Gaussian blur of the coverage mask, shared with the S18 Select menu; the EDT is
  reserved for grow/shrink.** (§6)
- **D7. Reuse the whole S14 commit funnel:** one `SetSelectionCommand` per click via
  `m_commitSelection`; press-time modifiers = boolean op via `selectOpForModifiers` (verified);
  op-badge cursors via `ui::selectionCursor`. (§8)
- **D8. Source option mirrors the Eyedropper** (Active Layer / All Layers); merged reads the raw
  checkerboard-free composite. (§8.1)
- **D9. Scope verdict.** Every ingredient is classical, decades-old published technique; the modern
  smart-selection / refine-edge / matting families are excluded by design (§7).
- **D10. Scope boundary:** brush dab model + Select-menu (grow/shrink/feather/smooth, Mask-from-
  selection, ants-direction experiment) are **S18**, specified in `docs/research-select-brush.md`; this
  note owns the shared substrate + the wand only.

## 11. Open questions (for the user — genuine forks, UI-consequential)

- **OQ-1 — Toolbar slot.** Own slot `ToolSlot::MagicWand` (**recommended**: distinct click
  interaction; Photoshop groups the wand with quick-select in its own slot) vs. a variant inside the
  marquee/lasso flyout. *Rec: own slot.*
- **OQ-2 — Tolerance response.** Linear-in-`T` slider (**recommended**, matches the class) vs. a
  Gamma/Log response curve out of the box. *Rec: linear; curve as a later lever.*
- **OQ-3 — Connectivity.** 4-connected (**recommended**, no diagonal leaks) vs. 8-connected. *Rec: 4.*
- **OQ-4 — Alpha in the distance.** Compare over **RGBA** so a click on transparent forms its own
  region (**recommended**, Photoshop-like) vs. RGB-only. *Rec: RGBA.*
- **OQ-5 — Anti-alias default.** On (**recommended**) vs. off. *Rec: on.*
- **OQ-6 — Feather home.** Ship a wand `Feather` amount now, reusing the shared blur (cheap) vs. leave
  *all* feather to the S18 `Select → Feather` menu. *Rec: leave the menu to S18; a wand feather field
  is a low-cost add if the user wants it in the options bar.*
- **OQ-7 — Scrub-to-set-tolerance gesture** (drag horizontally after the click to grow/shrink the
  selection live). Nice-to-have. *Rec: defer to a polish commit.*
- **OQ-8 — Undo-stack depth cap.** Full-mask `SetSelectionCommand`s (and `SetLayerPixels`) are heavy at
  40 MP (~80 MB/step); with the History panel visible this may warrant a cap. If capping: evict oldest
  from the bottom and show it in the panel (§4). *Rec: defer until it bites; document, don't pre-cap.
  Real fix is tiled storage (S60-c).* (This is a cross-cutting decision, not wand-specific.)

---

## 12. Cross-references

- `docs/research-select-brush.md` — **S18**: the select brush (paint-to-select dab / soft-coverage
  model) and the **Select menu** (all/none/inverse, grow/shrink/feather/smooth, Mask-from-selection),
  plus the marching-ants direction experiment. Shares this note's substrate (§1–§6) and boolean-op /
  commit conventions (§8); the shared blur/EDT feather helpers are specified there.
- `docs/document-model.md` — the layer/document/command model the `Selection` and `SetSelectionCommand`
  sit in.
- PLAN §3.6 (colour-space status, S43-a/-b), §9 (S17/S18 specs), and the S16 "Notes / gotchas for the
  next session (S17 — Magic wand, research-first)" block.
