# Select brush + Select menu — Research & Build Plan (S18)

> **Research-first note for PLAN S18** (the §0 rule: research-heavy sessions write/refresh their
> `docs/` note before any code). This note owns the **select brush** (paint-to-select), the
> **Select menu** (grow/shrink/feather/smooth + the S31 *Mask from selection* entry), and the
> **marching-ants direction experiment**. It is a *companion* to `docs/research-selection.md`
> (written concurrently, owns the selection-mask representation, colour-distance / tolerance
> semantics, flood fill, anti-aliased coverage, and feather-as-a-primitive) — **cross-referenced,
> not duplicated**.
>
> **§6 draws the ship/decline line** between the plain paint-to-select brush S18 builds and the
> edge-aware "quick"/"magic" select brush it deliberately does not. That line is a settled design
> decision, not a to-do.

---

## 1. Scope — this note vs `docs/research-selection.md`

`research-selection.md` is the **shared foundation** and owns: the 8-bit coverage-mask
representation, colour-distance metric + colour space, tolerance semantics, contiguous vs global
flood fill, anti-aliased edge coverage, and **feather as a mask primitive**. This note owns:

- **§3** — the select brush: paint-to-select with soft edges, reusing the existing dab/stamp/
  spacing machinery; how a dab accumulates into an 8-bit coverage mask; add/subtract, hardness,
  size, flow; one `SetSelectionCommand` per stroke with a live frame-coalesced preview.
- **§4** — the Select menu: All / None / Inverse (exist), **Grow / Shrink / Feather / Smooth**
  (new), and **Mask from selection** (absent until S31).
- **§5** — the marching-ants direction experiment (`antsCirculate` hidden setting).
- **§6** — the edge-aware / quick-selection line: what S18 ships and what it declines.
- **§7** — the commit-sequenced build plan; **§8** decisions; **§9** open forks for the user.

---

## 2. What exists today (verified against the tree)

### 2.1 The selection model — `src/core/selection.{hpp,cpp}`
`core::Selection` is an **8-bit coverage mask** the size of the document: `0` unselected, `255`
selected, intermediate = AA/feather coverage (`selection.hpp:9-19`). **"No selection" is an empty
(default-constructed) `Selection`** — distinct from an all-zero mask, which is an *active* selection
of nothing (`selection.hpp:11-14`). It already provides everything the select brush and Select menu
need to *land* a result:

- **Boolean combine in coverage semantics** — `combine(a, b, op)` with `SelectOp {Replace, Add,
  Subtract, Intersect}`: **Add = max, Subtract = a·(255−b)/255, Intersect = min** per pixel
  (`selection.hpp:24`, `selection.cpp:110-149`). This is exactly the add/subtract a select-brush
  stroke needs — no new mask algebra.
- **`inverted()`** — coverage complement `255−v`; inverting an *empty* selection returns empty
  ("no selection has no complement"), and the UI already gates it (`selection.cpp:161-168`).
- **`bounds()`** — tight integer bbox of coverage > 0 (`selection.cpp:190-212`).
- **An AA polygon rasteriser** — `polygon()` with `kSubScan=8` sub-scanlines and analytic
  horizontal spans (`selection.cpp:27-86`); the lasso/ellipse building block. Not directly reused
  by the brush, but it establishes the AA-coverage convention the menu ops must preserve.

`SetSelectionCommand` (`commands.hpp:425-436`) replaces the document mask; it **stores the full old
+ new masks** — heavy for big docs, "acceptable until tiled storage (S60-c)", and explicitly notes
**a drag gesture's intermediate updates should be UI-coalesced, not pushed per mouse move**. That is
the precedent the select brush must follow: one command per *stroke*.

### 2.2 The gesture → single-command precedent (mirror this exactly)
The S14 marquee/lasso path is the template for "a live drag that commits one undoable selection":
- `ui::SelectionGesture` (`selection_gesture.{hpp,cpp}`) is **pure, FLTK-free, unit-tested**; the
  canvas feeds it pointer events in *document* coords, and its header states the contract verbatim:
  *"the live preview goes straight to the canvas mask (frame-coalesced, never the command stack …)
  and the host pushes the single SetSelectionCommand with what finish() returns"*
  (`selection_gesture.hpp:10-15`).
- Frame coalescing is a dirty flag: events set `m_previewDirty`; the frame loop consumes it and
  rebuilds the preview **at most once per rendered frame** (`selection_gesture.hpp:117-120`).
- `finish(base, w, h)` returns `optional<Selection>` = the shape combined onto `base` by the
  press-time op, or `nullopt` when nothing should be pushed (a no-op combine isn't worth an undo
  step) (`selection_gesture.cpp:267-285`).
- Canvas side: `finishSelectionGesture()` calls `m_commitSelection(std::move(*result))`
  (`vulkan_canvas.cpp:382-390`); `m_commitSelection` is the host callback that *"push[es] the
  SetSelectionCommand"* (`vulkan_canvas.hpp:796`). Host side: each Select action pushes a
  `SetSelectionCommand` then `syncSelection()` (`app_window.cpp:3642-3664`), and `syncSelection()`
  hands the mask to the marching-ants pass on a **selection-revision guard** so unchanged masks skip
  the re-upload (`app_window.cpp:5633-5646`).

**The select brush plugs into this same three-part structure** (pure engine → canvas preview → host
command). It does *not* invent a new commit path.

### 2.3 The brush engine + dab machinery — `src/core/brush/`
The S19 rework (Arc A landed 2026-07-09) gives us a **coverage-first** stamping pipeline that is
already the right shape for painting a mask:

- `brush::BrushEngine` (`brush_engine.hpp`): `begin/extendTo/composite/restore/dirtyBounds`, a
  **bounded working rect grown in tiles** (never document-sized allocations), spacing-walked dabs
  carrying the sub-interval remainder, and — critically — a **per-pixel float coverage buffer**
  exposed via `coverage()` over `coverageOrigin/Width/Height` (`brush_engine.hpp:85-94`). The header
  spells out the reuse precedent: *"The Inpaint brush (S39) reads this as the hole mask — the stroke
  paints a red overlay AND records the region in one pass."* A **select brush is the same pattern
  minus the visible paint**: the stroke's coverage *is* the selection contribution.
- `dabCoverage(d, R, hardness)` (`brush_engine.hpp:48`): analytic circle, smoothstep shoulder, a
  guaranteed ~0.75 px AA rim even at hardness 1 — the soft edge comes for free and is already
  unit-tested pure.
- The Arc-A dab primitives make a colourless mask stamp first-class: `DabMask` is **8-bit coverage,
  255 = full paint** (`dab_mask.hpp:38-51`); `placeDab()` / `renderDabMask()` split placement from
  raster so the dab cache is transparent (`dab_mask.hpp:73-99`); `StrokeState` + sensors drive
  spacing/dynamics (`stroke_state.hpp`). `docs/brushes.md` §6.2 already describes a **masking brush**
  whose tip *is* a coverage mask — the select brush is that primitive with the mask as the output
  rather than a multiplier.
- Canvas wiring to copy: `strokeToolActive()` (`vulkan_canvas.cpp:1781`) gates the paint path;
  `m_brushEngine.begin(img.width, img.height, img, currentBrushParams(), …)`
  (`vulkan_canvas.cpp:1878`) starts a stroke onto the active layer's image and composites the first
  dab immediately (`:1881`); a stroke commits one region-scoped `SetLayerPixelsCommand`
  (`commands.hpp:308-331`). The `FillCommand` pattern (`commands.hpp:337-342`) shows the house move
  for ops whose pixels are computed in `render/` (off-limits to `core`) and patched by a `core`
  command — relevant to §4's blur-based menu ops.

**Reuse verdict:** the select brush should stamp dabs into an 8-bit coverage buffer over a bounded
rect (reusing `dabCoverage` + the spacing walk, or the Arc-A `DabMask` path), then combine that
buffer into the document `Selection` via `Selection::combine`. See §3.2 for the exact seam.

### 2.4 The Select menu today — `src/ui/app_window.cpp`
`buildMenu()` currently declares **only** `Select → Select All / Deselect / Inverse`
(`app_window.cpp:288-290`), wired to `selectAll/deselect/invertSelection`
(`app_window.cpp:3642-3664`). **Grow / Shrink / Feather / Smooth do not exist.** `ToolId` has no
`SelectBrush` — the enum lists `RectMarquee, EllipseMarquee, Lasso, PolygonLasso, Brush, Eraser,
InpaintBrush` (`tool.hpp:19-28`). The wand's own **Feather** slider is a *tool option*
(`tool.cpp:254-258`), applied at selection-creation time (that lives in `research-selection.md`);
the Select-menu **Feather** command (feather an *existing* selection) is separate and owned here.

**House rule — "a greyed item is a promise."** The project's convention (memory:
`mosaic-layer-dock`; PLAN's per-item disable machinery is reserved for real-but-contextually-
unavailable actions, e.g. the S16-f inpaint-while-rotated grey-out) is **absent-until-real**. So
*Mask from selection* is **not added at all** in S18 — it appears when S31 wires it, not as a greyed
placeholder. See §4.5.

### 2.5 The marching-ants present pass — `shaders/canvas_present.comp`
`ants(s, doc, base)` (`:224-235`) is the whole animation. It samples membership via
`maskSelected(doc)` — **NEAREST at the texel centre, thresholded `>= 0.5`** (`:213-217`), so the ants
ride the **crisp per-pixel staircase** ("selected ≥ 50 %" boundary, Photoshop convention; bilinear
rounded corners and waved the contour, rejected 2026-06-17). Edge = a selected pixel with any
unselected 4-neighbour, neighbours taken as **`doc ± stepX/stepY`** where `stepX/stepY` are the
doc-space image of one *screen* pixel (`:227-230`) — so the outline stays 1 screen-px at any
zoom/rotation. The dash: `mod(s.x + s.y + pc.ants.y, ANTS_PERIOD)` (`:233`), i.e. **screen-space
`x+y`** with `pc.ants.y` the time-advancing phase (`ANTS_PERIOD = 8` px, `:110`; phase fed by
`setAntsPhase(fmod(now·12, 8))` at `vulkan_canvas.cpp:3010,139`; `kAntsDashPeriodPx=8`
`window_renderer.hpp:38`). This is the "diagonal crawl toward one corner" §5 replaces behind a flag.
There is **no `antsCirculate` anywhere in the tree today** (verified: `grep` finds nothing in
`src/`, `shaders/`, `docs/`).

---

## 3. The select brush

### 3.1 UX + model
Paint-to-select with a soft brush: dragging deposits selection coverage under the tip; the marching
ants update live. Options ride the S11-b options bar and reuse the brush's own controls:

- **Size** (tip diameter, doc px), **Hardness** (0 soft cone .. 1 hard-but-AA edge — `dabCoverage`),
  **Flow** (paint per dab, so overlapped passes within one stroke build toward full within the
  stroke), and a per-stroke **opacity/strength cap** (the whole stroke tops out at one coverage
  value even where it crosses itself — the flow-vs-opacity model `BrushParams` already encodes,
  `brush_engine.hpp:15-18`).
- **Add / Subtract via modifier**, following the S14 press-time-modifier convention
  (`selectOpForModifiers`, `selection_gesture.cpp:50-58`): the default op accumulates, a modifier
  subtracts. Default op is an **open fork** (§9-B) — Photoshop's Quick Selection defaults to *Add*.
- **Anti-aliased soft edge** falls out of `dabCoverage`; no new edge code.

This is a **coverage painter**, nothing more. It performs **no image analysis** — it does not read
the layer's pixels, detect edges, or grow to a boundary. That is a deliberate line, not an omission
(§3.3, §6).

### 3.2 Reuse plan (the exact seam)
Two viable seams; recommend (b):

- **(a) Scratch-target reuse.** Call `BrushEngine::begin` with a throwaway RGBA target sized to the
  stroke footprint, stamp as usual, read `coverage()` at `end()`, discard the RGBA. Zero new engine
  code, but allocates/compositess colour it never uses.
- **(b) Coverage-only stroke (recommended).** A thin `brush::MaskStroke` (or a `begin()` overload
  that takes no `Image`) that runs the same spacing walk + `dabCoverage`/`DabMask` accumulation into
  the bounded float `coverage()` buffer and skips the colour composite entirely. This is the
  masking-brush primitive `brushes.md` §6.2 already scopes; it shares `stroke_state` + `dab_mask`
  with the paint brush and is **headless-testable** (canned sample stream → coverage buffer →
  golden), matching the engine's existing test posture.

Commit path (mirrors §2.2 exactly):
1. **Live preview:** each frame, if the stroke's coverage changed, compute `preview =
   Selection::combine(baseSelection, strokeCoverageAsMask, op)` into the **canvas mask only**
   (never the command stack) — reusing `syncSelection`/`setSelectionMask`. Frame-coalesced via the
   same dirty-flag pattern as `SelectionGesture::previewDirty()`. The marching ants follow for free.
2. **On mouse-up:** convert the final `coverage()` (float [0,1], bounded rect at `coverageOrigin`)
   to an 8-bit document-sized contribution, `combine(base, contribution, op)`, and — if the result
   differs from base — hand it to `m_commitSelection` → **one `SetSelectionCommand` for the whole
   stroke** (`vulkan_canvas.cpp:382-390`, `app_window.cpp`'s selection-push helper). A degenerate
   stroke (single click that selects nothing new) pushes nothing, per `finish()`'s precedent.

Note on cost: `SetSelectionCommand` stores full old+new masks (`commands.hpp:422`); a select-brush
stroke that touches a small area still costs two document-sized mask copies per undo step until
S60-c tiling. Acceptable and identical to what the marquee tools already pay — call it out, don't
fix it here.

### 3.3 Edge-aware vs plain — the decision
A **plain soft-brush mask painter** (§6.2) is mask painting, decades old, and is literally
`Selection::combine` over `BrushEngine::coverage()` — both already in the tree. An **edge-aware
"quick selection"** brush — one that reads the underlying pixels, builds a foreground/background
appearance model *from the stroke*, and grows the selection to image edges as you drag — is a
different and much larger thing (§6.3–§6.4).

> ⚠ **S18 ships the plain paint-to-select brush and *declines* edge-aware quick selection** — the
> combination of a **stroke-derived appearance model** plus **live grow-to-edge feedback during the
> stroke** is out, not behind a flag. A *seed-based graph-cut* selection (explicit fg/bg seed
> strokes, one global cut) is a separate, more classical shape and is discussed in §6.4; it is
> **not** attempted in S18 either (§9-E).

---

## 4. The Select menu (S18)

The four new commands operate on the **existing** document `Selection` and push one
`SetSelectionCommand` each (like `invertSelection`, `app_window.cpp:3658-3664`). All four must
**preserve the AA-coverage semantics** the mask carries: the ants ride the ≥0.5 staircase but fills
read fractional coverage, so an op that hard-thresholds to binary would visibly de-AA every fill.

**Architecture note.** `Selection` and `SetSelectionCommand` live in `core`, which **may not include
`render/`** (where the separable Gaussian `render::gaussianBlur(plane,w,h,sigma)` lives,
`effect_primitives.hpp:18`). Two clean options: put the morphology ops as **pure `core` functions on
the 8-bit mask** (a ~40-line separable box/Gaussian + a distance transform, no `render` dep — keeps
the op undoable purely in `core`), or follow the `FillCommand` pattern (compute in `render`, patch
via a `core` command). **Recommend pure-`core`** for grow/shrink/feather/smooth: they are small,
mask-only, and keeping them in `core` means the command is self-contained and headless-testable
without a GPU.

### 4.1 Select All / None / Inverse
Exist (`app_window.cpp:3642-3664`). No change beyond menu grouping.

### 4.2 Grow / Shrink — **recommend signed-distance-field threshold**, not iterative dilate/erode
Two implementations:
- **Iterative 3×3 dilate/erode**, N passes for N px. O(N·pixels), and — the killer — it operates on
  a **binarised** mask, so it destroys the AA edge and grows in blocky octagons (chamfer artefacts).
- **SDF threshold (recommended).** Compute a signed Euclidean distance from the 0.5 iso-contour of
  the coverage mask (a two-pass exact EDT — Felzenszwalb–Huttenlocher 2004 or Meijster 2000, both
  classical), then re-derive coverage as a **1-px linear ramp across the shifted level**
  `d = ±N`. This is an *exact* N-px offset for any N in one pass, **preserves a clean AA edge**, and
  keeps fractional coverage for fills. It also composes: Shrink is Grow with a negated offset.

Argument for SDF: the whole point of the mask being 8-bit is AA fills; a grow/shrink that quantises
to binary throws that away. The EDT is pure `core`, ~60 lines, and is reusable by Feather/Smooth.

### 4.3 Feather — separable Gaussian on the coverage mask
Feather an *existing* selection = **blur the 8-bit coverage** by a user radius (feather px ≈ blur
sigma), leaving the result in coverage semantics directly (the mask is *already* fractional; the
ants ride ≥0.5, fills read the ramp). Algorithm is the same separable Gaussian
`research-selection.md` specifies for the wand's create-time feather — **cross-reference it**; do not
re-derive. `render::gaussianBlur` (`effect_primitives.hpp:18`) is the existing kernel to mirror, but
per the architecture note the Select-menu command should carry a small `core` separable blur to stay
`render`-free. (If the two ever risk drifting, factor one separable-blur helper into `common`.)

### 4.4 Smooth — **recommend blur + re-threshold**, morphological open/close as the alternative
Smooth rounds jagged selection edges and removes speckle (Photoshop's *Smooth* is a radius
operation). Options:
- **Morphological open-then-close** (or close-then-open) at radius r on the binarised mask: removes
  islands/necks smaller than r and rounds corners, but operates on binary → needs an explicit
  re-AA pass afterward, and the result is only as smooth as the structuring element.
- **Blur + re-threshold (recommended).** Gaussian-blur the coverage by r, threshold at 0.5, keep a
  1-px linear ramp across the crossing. Features smaller than r wash out; the edge comes back clean
  and AA'd. It reuses the §4.3 blur and the §4.2 ramp — one code path serves Feather, Grow/Shrink,
  and Smooth. The visual difference from morphology is negligible at the radii users pick, and it is
  strictly less code.

### 4.5 Mask from selection — **absent until S31**
S31 is *"Layer masks + Mask from selection"* (PLAN `:1698-1700`); the entry is wired there, not here.
Per the greyed-is-a-promise rule (§2.4) the item is **not added to the menu in S18** — it materialises
when S31 lands. (If the user wants a visible affordance sooner, that is a UI fork — §9-D — but the
house default is absent.)

> **Wired 2026-07-16 (S31)**, exactly as promised: Select ▸ Mask from Selection appears below the
> morphology group (divider after Smooth) and pushes one `SetLayerMaskCommand` built by
> `core::maskFromSelection`; refusals (no selection / no layer / locked / text layer) are narrated
> in the status bar, Merge-Down style.

---

## 5. The marching-ants direction experiment (`antsCirculate`)

**PLAN's spec (`:1439-1448`).** The present pass dashes along screen-space `x+y`, so ants drift
uniformly toward one corner instead of *circulating* around the boundary like Photoshop (which
animates dash offset along the stroked contour). The proposed cheap variant: the pass already samples
the 4 neighbours, so the mask gradient is "nearly free" — rotate it 90° for a local tangent and dash
along `dot(p, tangent)`, which circulates clockwise on any boundary (corners cheat). It lands behind
a **documented hidden setting `antsCirculate`** (no UI entry; listed in `docs/settings-and-logging.md`,
the key doubling as the experiment flag). **The default stays the diagonal crawl** — calm, uniform,
liked; if the tangent variant shimmers on AA edges it simply stays hidden.

**What the actual shader does (verified, `:213-235`), and the one correction to the plan.** The 4
neighbour taps in `ants()` go through `maskSelected()`, which returns a **bool** (nearest, `>= 0.5`),
**not raw coverage.** So a central-difference gradient built from those existing taps,
`g = (m(+x)−m(−x), m(+y)−m(−y))`, has components in `{−1,0,+1}` → only **8 distinct directions**. A
tangent snapped to 8 directions is coarse: along a diagonal *staircase* edge the membership gradient
flips between horizontal and vertical every step, so `dot(s, tangent)` becomes piecewise and
**adjacent boundary pixels get inconsistent dash phase → broken/shimmering dashes** — exactly the AA
failure mode PLAN anticipates. To get a *smooth* tangent you must add **raw-coverage taps** (sample
the R8 value, not the thresholded bool) at the same `doc ± stepX/stepY` offsets; ~4 extra `texture()`
loads, genuinely cheap.

**Is the plan sound? Yes, with that correction, and the geometry composes cleanly.** Because
`stepX/stepY` are the doc-space image of one *screen* pixel (`:227-228`), the raw-coverage
central difference is already expressed in a **screen-aligned basis** (each component is coverage-
change per one screen px along screen x/y). So `g` is effectively the **screen-space** coverage
gradient; rotate 90° for a screen-space tangent; dash `mod(dot(s, tangent) + pc.ants.y, ANTS_PERIOD)`
in **screen px** → screen-uniform dash spacing that follows the local boundary tangent, and the same
`pc.ants.y` time-phase makes it crawl *along* the boundary (circulate). Keep the **edge test
unchanged** (crisp ≥0.5 staircase) so ants still ride pixel boundaries; only the dash *direction*
changes. Guard the normalize with an epsilon (interior pixels never reach here — they fail the edge
test — but a lone selected pixel has zero gradient).

**Failure mode on AA edges (why it stays hidden by default).** Even with raw-coverage taps: (1) on a
**diagonal AA edge** the visible boundary is a staircase (edge test is binary) while the dash
direction comes from the smooth coverage gradient — the two disagree, so dashes can appear to
*slide across* the staircase rather than run along it; (2) near coverage ≈ 0.5, sub-pixel motion or
mask AA noise swings the gradient direction frame-to-frame → **direction shimmer**; (3) at **90°
corners** the gradient is ill-defined (averages diagonally) → the "corners cheat a little" PLAN
already accepts. On a **binary rectangle** marquee the gradient is clean on the straight runs
(exactly horizontal/vertical) and only wobbles at corners — so the variant looks *best* on
axis-aligned marquees and *worst* on feathered/lasso/ellipse edges, which is precisely why the
diagonal crawl remains the default and this is a hidden A/B key.

**Hidden-setting mechanics.** `common::Settings` is a plain struct of **UI-free serializable fields**
(`settings-and-logging.md` §"Settings store"; `theme`/`logLevel`/`language` + `kSchemaVersion`),
loaded/saved atomically, unknown keys ignored. Add a `bool antsCirculate = false` there (no settings
UI, no menu), thread it to the present pass as a push-constant bit (there is spare room in `pc.ants`
— `.x` is a 0/1 active flag, `.z` a mode enum, `.w` handle size; a bit or a dedicated lane fits),
and **document it in `docs/settings-and-logging.md`** as the sole record of the key. Default `false`
= diagonal crawl. This is the only settings write S18 makes.

---

## 6. The edge-aware / quick-selection line

What S18 ships, what it declines, and the lineage each option descends from. The declines are
settled design decisions, not open questions.

### 6.1 The line
- **Ships (S18): plain paint-to-select** — a soft brush depositing coverage into the selection mask,
  no image analysis (§6.2).
- **Declines (S18): edge-aware "quick selection"** — a brush that derives a fg/bg appearance model
  from the stroke and grows the selection to image edges via graph-cut / iterated graph-cut (§6.3).

### 6.2 Plain soft-brush paint-to-select — what S18 builds
Painting an alpha/coverage value under a brush tip, accumulated by flow and capped by opacity, then
OR/AND/subtracted into a stored mask, is **generic raster-mask painting** — published and shipped
since early-1990s digital paint programs, and mechanically identical to what Mosaic already does in
`BrushEngine` + `Selection::combine`. No image content is read; there is no segmentation,
region-growing, edge affinity, energy minimisation, or appearance model. This is the shippable S18
brush.

### 6.3 Graph-cut brush selection with a stroke-derived appearance model — **DECLINE**
The natural "quick"/"magic" select-brush UX — *paint a stroke inside the object and it grows to the
edges*, building a colour/texture model from the painted region and solving a graph cut with regional
+ boundary costs, with live feedback while brushing — is the combination S18 declines.

⚠ **INVARIANT for S18: Mosaic does not build a select brush that (a) derives a foreground/background
colour or texture model from the user's brush stroke and (b) feeds that model to an energy
minimisation that grows the selection to image edges, with live feedback during the stroke.** Not
behind a flag. It is one of the two or three most obviously desirable features in this space and it
is deliberately absent — treat any proposal to add it as a reversal to be argued, not a gap to fill.

### 6.4 The *base* interactive graph-cut segmentation — a different, more classical shape
The foundational "mark seeds → global min-cut with boundary + region terms" method is
**Boykov & Jolly, *Interactive Graph Cuts for Optimal Boundary & Region Segmentation of Objects in
N-D Images*, ICCV 2001** — explicit object and background seeds, one global cut.

Mosaic also already owns the **solver**: a clean-room **Dinic max-flow + α-expansion** written for
the inpainting engine. **No research-licensed graph-cut source is vendored** anywhere in the tree.

So a **seed-based** graph-cut selection (*paint foreground seeds, paint background seeds → one global
min-cut*) is describable entirely from the published Boykov–Jolly formulation plus Mosaic's own
solver — available in principle. **But the line to §6.3 is narrow:** a UX that derives the appearance
model **from the brush stroke** and gives **live grow-to-edge feedback during brushing** has drifted
across it. **Verdict:** do **not** attempt edge-aware selection in S18. If it is ever revisited,
build it strictly to the seed-based formulation — explicit fg/bg seed strokes, one global cut, *no*
stroke-derived statistical model driving the cut, no live during-stroke growth (§9-E).

### 6.5 Iterated-graph-cut foreground extraction (GrabCut family) — **DECLINE the technique**
Iterated graph-cut with per-region Gaussian-mixture colour models (the "draw a box, it extracts the
foreground" family — Rother, Kolmogorov & Blake, SIGGRAPH 2004) is a distinct technique **not on
Mosaic's roadmap**: no box-prompt extraction, no iterated GMM. Declined as a technique family.

### 6.6 Live-wire / intelligent-scissors boundary tracing — out of S18 scope (magnetic lasso)
Interactive shortest-path boundary snapping ("magnetic lasso") is the Mortensen–Barrett **Intelligent
Scissors** method (SIGGRAPH **1995**). It is a **different tool** (a lasso that snaps to edges, not a
paint-to-select brush) and is **not part of S18** — recorded here only so the boundary of this note
is explicit.

### 6.7 Summary
| Technique family | Lineage | S18 posture |
|---|---|---|
| Raster-mask painting (plain paint-to-select) | 1990s paint programs; in-tree `BrushEngine` + `Selection::combine` | **SHIP** |
| Base interactive graph-cut segmentation (seeds→min-cut) | Boykov & Jolly, ICCV 2001 | not built; the only edge-aware shape ever worth revisiting (§6.4) |
| α-expansion graph-cut solver | Mosaic's own clean-room Dinic/α-expansion (inpainting engine) | already in-tree, reusable |
| Graph-cut brush selection w/ stroke-derived model + live grow | — | **DECLINE** (§6.3), not behind a flag |
| Iterated-graph-cut / GMM extraction (GrabCut) | Rother, Kolmogorov & Blake, SIGGRAPH 2004 | DECLINE technique |
| Live-wire boundary tracing (magnetic lasso) | Mortensen & Barrett, SIGGRAPH 1995 | out of S18 scope |

---

## 7. Commit-sequenced build plan (one logical change each)

Format after `docs/spell-check-plan.md`. Each commit builds green on all presets; pure logic stays
in `core`/`ui` headless-testable, GPU/interaction is manual-verified.

1. **Select-menu morphology in `core` (no UI yet).** Add pure functions on `core::Selection`:
   `grown(px)` / `shrunk(px)` via an exact EDT + 1-px ramp (§4.2), `feathered(radius)` via separable
   Gaussian on the 8-bit mask (§4.3), `smoothed(radius)` via blur+threshold+ramp (§4.4). One shared
   separable-blur + EDT helper. **Unit tests:** AA preservation (a blurred/grown mask keeps
   fractional edge values), exact-offset (grow N then shrink N ≈ identity within AA), empty-input
   returns empty. No `render` dependency.
2. **Wire Grow/Shrink/Feather/Smooth into the Select menu.** Add four items after Inverse in
   `buildMenu` (`app_window.cpp:288-290`); each opens a small numeric prompt (reuse the existing
   dialog pattern — §9-C) and pushes **one `SetSelectionCommand`** via the existing selection-push
   helper + `syncSelection` (`:3658-3664`). Gate each item on a non-empty selection (like Inverse).
   *Do not* add *Mask from selection* (§4.5). **Verify:** headless op-runner round-trips
   (select-all → grow 8 → bounds); interactive ants update.
3. **Coverage-only brush stroke in `core::brush`.** The `MaskStroke` path (§3.2b): spacing walk +
   `dabCoverage`/`DabMask` accumulation into the bounded float coverage buffer, no colour composite.
   **Unit tests:** canned sample stream → coverage golden; hardness/flow/opacity-cap invariants
   (reuse `test_brush_engine` posture); spacing independence of sample density.
4. **Register `ToolId::SelectBrush` + options + canvas wiring.** Add the tool (`tool.hpp:19-28`) in
   the marquee/lasso *selection* family (not PaintFill — it produces `SetSelectionCommand`); options
   bar = size/hardness/flow + op (§9-A/B). Canvas: a `strokeToolActive`-style path that runs the
   MaskStroke, previews `combine(base, stroke, op)` to the **canvas mask only** frame-coalesced
   (§3.2 step 1), and on mouse-up calls `m_commitSelection` with the combined result (§3.2 step 2).
   Reuse the brush reticle for the size ring. **Verify:** interactive paint-to-select add/subtract;
   one undo step per stroke; ASan-clean `--gui-frames`.
5. **Marching-ants `antsCirculate` experiment.** Add `bool antsCirculate=false` to
   `common::Settings` + document it in `docs/settings-and-logging.md` (§5). Add raw-coverage neighbour
   taps + tangent dash to `ants()` behind a `pc.ants` bit; default path byte-unchanged. **Verify:**
   with the key off, ants are pixel-identical to today (golden/​eyeball); with it on, user eyeballs
   circulation on a rect marquee and the documented shimmer on a feathered/lasso edge. **Default
   stays diagonal** (settled, §8).

---

## 8. Decisions (settled)

- **D1. S18 ships plain paint-to-select only; edge-aware quick selection is declined** (§6.3).
  Not behind a flag.
- **D2. The select brush reuses the coverage-first engine** (§3.2b `MaskStroke`), not a bespoke
  painter, and commits **one `SetSelectionCommand` per stroke** with a live frame-coalesced preview
  (the `SelectionGesture`/`m_commitSelection` precedent, §2.2).
- **D3. Grow/Shrink = SDF/EDT threshold; Smooth = blur+threshold; Feather = separable Gaussian** —
  all AA-preserving, all pure `core`, one shared blur+EDT helper (§4). Iterative dilate/erode is
  rejected for de-AA'ing the mask.
- **D4. *Mask from selection* is absent until S31**, per greyed-is-a-promise (§4.5).
- **D5. `antsCirculate` defaults OFF; the diagonal crawl stays the default** (PLAN mandate, §5); the
  circulating variant needs **raw-coverage** neighbour taps (the existing taps are binary, §5) and
  ships as a documented hidden key only.

## 9. Open questions (genuine forks for the user — UI-consequential, not decided here)

- **A. Tool slot / icon / shortcut.** Does the select brush get its own left-toolbar slot, or share a
  flyout with the S17 magic wand (both are "sample-to-select")? *Recommend:* a flyout variant beside
  the wand (Photoshop groups Quick Select + Magic Wand under one slot), so the selection toolbar
  stays compact. Needs an icon (`docs/icons-needed.md`) on the one-ink 16px grid.
- **B. Default op + subtract modifier.** *Recommend:* default **Add** (a select brush that
  accumulates reads naturally), **Alt** = Subtract, matching the S14 press-time-modifier convention.
  Alternative: default **Replace** (each stroke starts fresh) — less useful for building a selection.
- **C. Grow/Shrink/Feather/Smooth entry UI.** A modal numeric prompt ("Expand by N px", Photoshop
  style) vs a live options-bar `ScrubSlider` with canvas preview. *Recommend:* the small numeric
  prompt for S18 (cheap, reuses the dialog family); a live scrub is a later polish.
- **D. Any visible affordance for *Mask from selection* before S31?** *Recommend:* **no** — absent
  until real (D4). Flagged only because it is a visible menu-shape choice.
- **E. Is a future *seed-based* graph-cut selection (post-Boykov–Jolly) worth scoping?** It is the
  only edge-aware route worth revisiting at all (§6.4), and it sits uncomfortably close to the
  combination §6.3 rules out — a design line that would have to be held very deliberately.
  *Recommend:* park it; do **not** scope it into S18. Decide later whether to research it at all.

## 10. References

- Boykov & Jolly, *Interactive Graph Cuts for Optimal Boundary & Region Segmentation of Objects in
  N-D Images*, ICCV 2001 — seed-based graph-cut segmentation (§6.4).
- Boykov, Veksler & Zabih, *Fast Approximate Energy Minimization via Graph Cuts*, PAMI 2001 —
  α-expansion; the lineage of Mosaic's own clean-room Dinic/α-expansion solver.
- Rother, Kolmogorov & Blake, *"GrabCut": Interactive Foreground Extraction using Iterated Graph
  Cuts*, SIGGRAPH 2004 — the iterated-GMM family S18 declines as a technique (§6.5).
- Mortensen & Barrett, *Intelligent Scissors for Image Composition*, SIGGRAPH 1995 — live-wire
  boundary tracing (magnetic lasso lineage, §6.6).
