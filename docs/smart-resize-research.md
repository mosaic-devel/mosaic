# Smart Resize (Image Retargeting) — Research and Implementation Plan

**Status:** research note / scoping doc. This is the deliverable for the retargeting slice of **S16-f** (Crop tool). No code lands until this is agreed. Scoping only.
**Feature name (settled with user, 2026-07-01):** **Smart Resize**. Friendly, self-explanatory, non-trademarked, and future-proof if the operator set ever grows. "Image retargeting" / "content-aware image resizing" are the academic terms; "Smart Resize" is the user-facing name.
**Home (settled):** a **toggle in the Crop tool** (not a separate menu command, not a 3-way dropdown). See §5.
**Scope (settled):** this document scopes the **complete/final** feature (v1 = vFinal), not a stepping-stone. The user's steer: *the best quality we can get; if content deformation is disliked, don't ship it.*

> **Relationship to the rest of S16-f.** S16-f has two independent axes that share the word "crop": (a) **crop-and-fill / canvas expansion** — cropping *beyond* the canvas, then filling the new area (Transparent/White/…/Inpaint); that is the "expand" axis (§3.10). (b) **Smart Resize** — this document — is **crop-*in* exclusive**: it only ever *removes* content to fit a smaller/different frame, never adds canvas. The two never overlap; retargeting is meaningless for expansion (there is no content to intelligently discard when you are adding blank area). Keep them mentally separate.

---

## 1. What image retargeting is, and what the benchmark actually told us

**Retargeting** = change an image's dimensions/aspect ratio while preserving its important content and its structure, and while limiting visual artifacts (Shamir & Sorkine's three objectives). The operators studied in the field:

| Operator | What it does | Artifacts |
|---|---|---|
| **Scaling (SCL)** | Non-uniform stretch/squeeze to the new size | Distorts everything (aspect change) |
| **Cropping (CR)** | Pick a sub-window of the target aspect; discard the rest | **None** — it only removes, never deforms |
| **Seam carving (SC)** | Remove/insert minimal-energy 1-px "seams" iteratively | Breaks lines/edges, deforms objects |
| **Warping (WARP / SNS)** | Deform a mesh: squeeze unimportant regions more than important ones | Deformation of proportions/structure |
| **Multi-operator (MULTIOP)** | Optimize a *sequence* of crop+scale+seam operations | Inherits the operators it uses |

**The benchmark the user supplied** — Rubinstein, Gutierrez, Sorkine & Shamir, *"A Comparative Study of Image Retargeting"* (SIGGRAPH Asia 2010; the **RetargetMe** benchmark, still the reference perceptual study) — ran a 210-participant, 9324-vote paired-comparison user study over 8 methods. Its findings are the single most important input to this design:

1. **Cropping (CR) is a top-3 method.** It ranks statistically *indistinguishable* from the two best content-aware methods (SV, MULTIOP) and clearly above seam carving, scaling, warping. "The only operator that, by definition, does not create any artifacts." **Users prefer losing content (cropping) over introducing deformation.**
2. **Seam carving (SC) ranks *dead last*** — 8th of 8, both with and without a reference image. Rejected primarily for "lines/edges distorted" and "people/objects deformed." The paper: *"simple operators such as uniform scaling or seam carving are better suited for small amounts of change."*
3. **Deformation is the cardinal sin.** The top three rejection reasons across all images: *people/faces were squeezed*, *geometric structures were distorted*, *proportions were changed*. Viewers are highly sensitive to deformation of **faces**, **clear geometric structure**, and **symmetry**.
4. **MULTIOP wins by using cropping+scaling with only a *little* seam carving** (measured operator mix ≈ 57% scale, 32% seam, 11% crop) — i.e. it is good *despite* seam carving, not because of it, and it leans on the artifact-free operators.
5. The paper's closing line: *"the search for an optimal cropping window, which was somewhat abandoned by researchers, could often be favorable and should not be overlooked."*

**Conclusion that writes itself.** The best-ranked and artifact-free operator is **cropping**. It is also literally what the Crop tool already does. So Smart Resize is a **content-aware automatic cropping** operator: given a target aspect/size, it finds and stages the crop window that keeps the most important content, composed well, then applies through the existing crop command. This is not a compromise — it is independently the operator the benchmark says users like best.

---

## 2. The decision: what Smart Resize is, and what it deliberately is not

**Smart Resize = content-aware crop-window optimization** (+ optional uniform downscale to a target pixel size). The ingredients (§3):

- an **importance map** built from classical signals (gradient/edge energy + Viola-Jones face detection + a photographic composition prior);
- a **crop-window search** that maximizes retained importance for the chosen target aspect, keeps salient blobs and faces whole, and respects composition (rule of thirds / centering) — the Suh 2003 / Liu-Gleicher 2006 lineage;
- optional **uniform scaling** of the chosen window to a target size (artifact-free; never non-uniform).

**Deliberately excluded, with rationale:**

- **Seam carving — GUILLOTINE (§3.1).** Ranked **last** of eight in the benchmark, rejected for distorted lines/edges and deformed people. Excluding it costs us nothing on quality and removes the field's most complicated technique. **Invariant: Smart Resize computes no energy seams and performs no seam removal or insertion**, anywhere, ever.
- **Warping / mesh deformation — EXCLUDED for v1(=final), by design.** It introduces exactly the deformation the benchmark says users reject. The user's steer was explicit: *if deformation is disliked, don't ship it.* Warp is scoped as a **documented, non-built appendix** (§10) so the ceiling is understood and the door is not bricked shut — but it is not part of the plan unless the user later overrides.
- **Learned saliency — EXCLUDED.** Gradient energy + faces + a composition prior is enough (§4.1), and a boring, explicable map is a feature in a tool whose whole job is to justify where it framed. No CNN, no global-contrast saliency model.

**What this buys us:** a feature that is (a) the perceptual near-top of the field, (b) artifact-free, (c) a small delta on the existing Crop tool, and (d) honest — we can proudly credit the people whose *findings* (the benchmark) told us to do exactly this.

---

## 3. Design constraints — what Smart Resize deliberately does not do

Read §3.1 and the guardrails in §3.8/§3.9/§3.10 first. Every constraint below is an invariant of the
shipped design and is referenced from §4 and §5; none of them is a preference.

### 3.1 Seam carving — **GUILLOTINE**

Seam carving removes or inserts minimal-energy 1-px connected paths ("seams") through the image.
**Invariant: Smart Resize computes no energy seams and performs no seam removal or insertion.** The
reason is the benchmark's own: RetargetMe ranks seam carving **last of eight**, with and without a
reference image, rejected primarily for "lines/edges distorted" and "people/objects deformed." This
exclusion is the crux of the whole design — everything else follows from committing to an operator
that only *removes* content and never deforms it.

Related: **do not borrow hybrid crop-plus-carve schemes either.** The hybrid is not a way back in.

### 3.2 Cropping — the whole feature

Plain rectangular cropping — selecting a sub-window and discarding the rest — is textbook editing
that every image editor since the 1990s has shipped, and the seam-carving literature expressly
distinguishes it ("cropping is limited because it can only remove pixels from the image periphery,"
whereas seam removal works throughout the image).

The *automatic / content-aware* selection of the crop window is the published lineage we design from
and cite:

- **Suh, Ling, Bederson & Jacobs, "Automatic thumbnail cropping and its effectiveness" (UIST 2003)** —
  crop to the most salient region, using a saliency map *or* a face detector. This is precisely our
  approach.
- **Chen et al., "A visual attention model for adapting images on small displays" (2003)**;
  **Liu & Gleicher, "Automatic image retargeting…" / video pan-and-scan (2006)** — saliency-driven
  optimal crop-window search.

Design from those papers, cite them, and respect the §3.8 guardrails.

### 3.3 Face detection (a quality lever for the #1 category)

Faces are the benchmark's most-protected content (top rejection reason: "people/faces were
squeezed"). A face detector that lets the crop optimizer *never cut or de-center a face* is the
single biggest quality lever available — **Viola & Jones, "Robust Real-Time Face Detection," IJCV
2004** (the Haar/AdaBoost cascade). We can implement clean-room or use an existing
permissively-licensed Haar/LBP cascade; **detector *models* / cascade files must be licence-checked
per file** (§4.2). *(In the event this lever was dropped — F1, §4.2.)*

### 3.4 Saliency — no learned or global-contrast model

**Invariant: the importance map uses only gradient/edge magnitude (Sobel/finite-difference), local
edge density / entropy, optional Viola-Jones faces, and a centre / rule-of-thirds prior.** No CNN,
no element-uniqueness/spatial-distribution superpixel saliency, no boundary-region covariance
saliency, no global-contrast (HC/RC) salient-region detection. This is a robust, defensible,
boring-on-purpose importance map, and it is what makes the tool's framing explicable to a user.

### 3.5 Multi-operator (MULTIOP) framework — not implemented

The MULTIOP "resizing space" (Rubinstein, Shamir & Avidan, SIGGRAPH 2009) optimizes a **sequence of
crop + scale + seam-carving** operations. It requires seam carving (§3.1), so we do not implement the
framework. A *crop-only* optimizer that also considers uniform scale is the artifact-free subset
(essentially "optimal pan-and-scan," Liu-Gleicher 2006 territory) and needs none of MULTIOP's
machinery.

### 3.6 Warp / mesh deformation — **EXCLUDED** (see §10)

Scale-and-Stretch (Wang, Tai, Sorkine & Lee 2008), non-homogeneous warping (Wolf, Guttmann &
Cohen-Or 2007) and Axis-Aligned Deformation (Panozzo, Weber & Sorkine 2012) are the field's warp
lineage. Warp is **excluded on quality grounds** (§2): it introduces exactly the deformation the
benchmark says users reject. §3.9 records the constraints that would bind a warp tier if the user
ever reverses that call.

### 3.8 The three guardrails (bake into all crop work)

Each is independently load-bearing; Smart Resize respects all three, in code and in UI.

1. **One fused importance map.** All signals (gradient, edge density, centre prior — and the face
   boost if it ever returns) are summed into a single `W` *inside the map builder*; the crop-window
   search consumes only `W` and **must never be handed separate per-signal maps**. Constructing
   several differently-typed saliency maps and scoring windows from them is out.
2. **One suggestion.** Smart Resize computes a single best window and seeds the staged rect with it.
   **Never** a ranked gallery of alternative crops, **never** clustering of candidates, and **never**
   a "Re-suggest" that cycles to the *next-best non-overlapping* window. Re-running because an input
   changed (ratio, image, toggle) is fine; "give me a different one" is not.
3. **No learned aesthetics.** Weights and priors are hand-set constants from photographic convention
   — never established by analyzing a collection of images predefined as visually pleasing. (This
   also keeps the map inside §3.4.)

### 3.9 Deformation & recomposition — the F3 follow-up

Run at the user's request after the crop-only v1 landed: the person/field/tower case ("remove the
boring middle, bring the subjects together") is the wow the feature was conceived for, and §10's F3
deferred it. **Headline: the wow case has a buildable path — and it is not warping.**

**A. Recomposition (Setlur, Takagi, Raskar, Gleicher & Gooch, *Automatic Image Retargeting*, MUM
2005 — 10-year-impact-award winner) is the path.** The recipe is literally the person/field/tower
one: segment the image into regions → build an importance map → **cut the important regions out** →
construct/fill the background → **scale the background** to the target → **paste the important
regions back, maintaining their relative spatial relationships**. Mosaic already owns every hard
ingredient: selection tools (the user marks the keep-regions — which sidesteps the auto-segmentation
quality problem outright), the S37 inpaint engine (fill the holes the cut subjects leave), uniform
scaling, layer compositing. Deformation-free by construction: subjects move **rigidly**; only the
(boring) background is scaled/cropped — consistent with the benchmark's finding and this doc's
governing steer.

**B. If a warp tier is ever built, these constraints bind it** (they are what separate a
Panozzo-faithful implementation from the naive one, and they are not negotiable):

- **No 1D-projection scaling.** Do not project the 2D saliency map to 1D horizontal/vertical
  profiles and scale rows/columns by those profiles. Panozzo 2012 keeps the 2D map inside a grid
  energy optimization and never projects first; that is the distinction that matters.
- **No distortion-threshold gating and no automatic crop-vs-warp fallback.** A warp tier must be a
  **user-invoked pure warp**: no "if the mesh distortion exceeds X, silently crop instead" branch,
  and no automatic operator-mixing decision of any kind.
- **Stills only, hand-set weights.**

**C. Verdicts.**
- **Recomposition tier (the person/field/tower wow): BUILD.** Setlur-2005 recipe, user-marked
  keep-regions, rigid subject placement, inpaint-filled seams.
- **Warp tier: still excluded** on the benchmark's own quality finding (§2, §3.6).
- The §3.8 guardrails extend naturally with a fourth: **no automatic operator mixing** — the user
  chooses crop vs recompose; the software never auto-decides via a quality/size threshold.

### 3.10 The crop-and-fill (canvas EXPANSION) axis

S16-f's *other* axis (crop beyond the canvas, fill the added area — see the note under §0) shipped
2026-07-02 **with a working integrated Inpaint fill mode**. The fill runs the already-built S37
engines on a different mask — no new engine surface. Solid-colour/transparent expansion is
decades-old (canvas resize in every editor since the '90s).

**The three expansion-axis guardrails (bake into ALL future crop work):**
1. **No operation-preview chooser, ever.** Never present crop-vs-crop-and-fill (or any operation
   alternatives) as simultaneously-shown preview images, and never make clicking a preview image the
   selection mechanism. A single result preview of the one already-chosen operation —
   review-then-confirm, like Recompose — stays fine; it is *plural operation previews as the chooser*
   that is out.
2. **Fill only on an explicit Apply of a persistently pre-chosen mode.** The fill mode is a
   **persistent options-bar dropdown** (present before, during and after frame placement) and the
   fill runs on the ordinary explicit **Apply**. No post-gesture prompt offering to fill, no
   automatic fill decision.
3. **Inpaint fill and image rotation are mutually exclusive.** When the Crop tool gains
   rotate/straighten, the Inpaint fill entry greys while the staged crop carries any rotation; solid
   fills stay available. **Never content-aware-fill the wedges left by a rotation in response to a
   rotation input.**

---

## 4. Algorithm design (the quality lives here)

A crop-based retargeter is only as good as its **importance map** and its **crop-window objective**. Both are modest amounts of code; both are FLTK-free, deterministic, and unit-testable headless.

### 4.1 Importance map `W(x,y)` — classical ingredients only

A single-channel float map at (down-sampled) working resolution, a weighted sum of:

1. **Gradient/edge energy** — `|∂I/∂x| + |∂I/∂y|` (L1 gradient magnitude, luminance or per-channel max). Captures the benchmark's *lines/edges*, *texture*, and *geometric structure* categories. Textbook. *(This is the same energy function seam carving computes — the energy image is not the objectionable part; what we do with it, a crop search rather than a seam, is both the constraint of §3.1 and the higher-quality answer.)*
2. **Face boost** — run the (§3.3) Viola-Jones cascade; add a strong Gaussian-weighted bump over each detected face box. Directly serves the #1 protected category. Off gracefully if no detector/model is available (map degrades to gradient+prior, still fine).
3. **Composition prior** — a mild center weighting + rule-of-thirds emphasis (photographic convention). Keeps the optimizer from hugging a corner and helps *foreground objects* / *symmetry*.
4. *(optional, cheap)* **Local edge density / entropy** in a small window — a texture-vs-flat discriminator that helps the search prefer to crop *flat* regions (sky/water/grass), matching the benchmark's note that content-aware methods "work best where content can be disposed of."

Weights are a few constants (tunable, with a "balanced" default) — mirror the Inpaint `settingsSchema` pattern (curated controls, sensible defaults, most internals hidden). **No learned saliency, no ML.** **Guardrail (§3.8-1): the signals are fused into the single map W inside the builder; the crop search consumes only W** — separate per-signal maps must never reach the window scoring. (The face boost was scoped as F1 and **dropped 2026-07-02**; if a detector is ever revisited, its boxes join the same fusion.)

### 4.2 Face detector — build/dependency note (a fork; see §10) — **RESOLVED: DROPPED (a)**

> **F1 resolved 2026-07-02 (user):** option **(a) — no faces**. The energy/object selection is
> good enough in practice, and every build-out (bundled cascade data, per-file model licence
> checks, or training our own) adds shipping weight for a marginal lever. The §3.3 groundwork
> stays valid if this is ever revisited; the `faceRects` hook in `keep_regions` and the
> `Source::Face` slot remain dormant in code.

Face-aware cropping needs *a* detector + *a* cascade model. Options, cheapest → best:
- **(a) No faces in the map** — gradient + composition only. Simplest; loses the top quality lever.
- **(b) Clean-room Haar cascade + a permissively-licensed cascade model** (or ship our own trained/relicensed cascade). Moderate; the *model file's* licence must be clean (many OpenCV cascades are BSD, but confirm per-file).
- **(c) A small existing permissively-licensed face-detect lib.** Adds a dependency; Mosaic prefers system deps / header-only (see [[mosaic-project]]).

Recommendation: **(b)** — a compact Haar/LBP detector with a licence-clean cascade — because faces are where the perceptual quality is. This is the main "how much engineering" fork in §10.

### 4.3 The crop-window objective and search

Given the source image `I` (W×H) and a **target aspect** `a = w/h` (from the Crop tool's Ratio combo) — and optionally a target pixel size — find the axis-aligned rectangle `R` of aspect `a` (any position, any scale ≤ the max that fits) maximizing:

```
score(R) = Σ_{p∈R} W(p)                      // retained importance ("keep the good stuff")
         − λ_face · (faces clipped by ∂R)     // hard penalty: never cut a face
         − λ_cut  · (importance mass on the boundary ∂R)  // don't slice through structure
         + λ_comp · composition(R)            // rule-of-thirds / salient-blob placement
```

Search strategy (all cheap, deterministic):
- **Coarse-to-fine over a down-sampled `W`** (multi-scale — the one architecture point the user's other-chatbot list got right and that we keep). Evaluate candidate windows via a **summed-area table (integral image)** of `W`, so each window score is O(1); sweep positions/scales on a grid, then refine the best few at full resolution. Whole search is milliseconds.
- **Salient-blob wholeness**: threshold `W` into connected components (faces + high-energy blobs); prefer windows that contain the important blobs *entirely* rather than clipping them (the benchmark: users hate half-objects).
- **Slack/scale**: if the target aspect is close to the source, allow a slightly *scaled* window (uniform) so we crop less — "optimal pan-and-scan." Uniform scale only; never non-uniform.

Output = a single `common::Rect` in document space, which seeds the Crop tool's staged rect (the user can then nudge it; §5). Because the output is just a rect, **the apply path is the existing `render::buildCropCommand` — no new command, no new undo type.**

### 4.4 Determinism & performance

Everything is integer/float image ops + an integral-image sweep + one cascade pass — **fully deterministic** (project requirement; cf. inpaint) and **interactive** (target < ~50 ms at working res on the loaded-system bench; it re-runs when the target aspect changes). No async job needed for v1 (unlike inpaint's multi-second solve) — but keep the compute FLTK-free so it *could* move to a worker if a huge image ever makes it laggy (reuse the `onFrame`-polled pattern if so).

---

## 5. Architecture & integration with the Crop tool

Smart Resize is a **thin, well-contained** addition: a new FLTK-free core module + one Crop-tool toggle + an "auto-place" action. It reuses the staged-rect gesture, the Ratio combo, and the apply command wholesale.

### 5.1 Core module — `src/core/retarget/`

Mirror `src/core/inpaint/`'s shape (FLTK-free, unit-tested, deterministic), but leaner because there is one operator:

- `importance_map.{hpp,cpp}` — builds `W(x,y)` (§4.1); gradient + composition always, faces if a detector is present.
- `face_detect.{hpp,cpp}` — the §4.2 detector behind a small interface (so the model/impl choice is swappable and testable with a stub).
- `smart_crop.{hpp,cpp}` — the objective + integral-image search (§4.3); pure function `Rect chooseCropWindow(image, targetAspect, options)`.
- `retarget_info.hpp` — a `RetargetInfo`/`BackendInfo`-style struct (authors/paper/summary/what-we-did-and-didn't; §7) surfaced in Settings → Tools → Crop (§6).
- *(Engine seam, optional.)* v1 has one operator, so a full `RetargetEngine` registry (à la `InpaintEngine`) is over-engineering. Keep the module engine-*shaped* (a single `SmartCropOperator` behind a tiny interface) so a future cleared operator could register — but do not build the registry now.

### 5.2 Crop-tool wiring (`src/ui/tool.cpp`, `app_window.cpp`, `vulkan_canvas.cpp`)

The Crop options (tool.cpp `case ToolId::Crop`) gain **one toggle**: `toggle("smartResize", _("Smart Resize"), false, _(…))`. Interaction:

- **When ON** and a target aspect is chosen (Ratio combo — reuse the existing Free/1:1/4:3/16:9/3:2/Custom machinery), Smart Resize runs `chooseCropWindow` and **seeds the staged crop rect** with the optimal window. The user sees a proposed crop they can accept (Apply/Enter) or nudge (the existing move/resize handles still work — Smart Resize just picks the *starting* rect intelligently instead of "full canvas / centered").
- Re-running: changing the Ratio recomputes (an input changed). **Guardrail (§3.8-2):** there is deliberately no "Re-suggest for a different answer" affordance — Smart Resize is deterministic and single-suggestion, and cycling to a next-best non-overlapping window is exactly what guardrail 2 forbids. Toggling OFF reverts to today's behaviour (manual crop / full-canvas framing per the S16-q setting).
- **Apply** = unchanged `render::buildCropCommand` (the staged rect is applied exactly like a manual crop). Delete-cropped-pixels, guides, custom ratio, HUD, view-rotation overlay — all reused unchanged.
- **"Delete Cropped Pixels"** interplay: Smart Resize is inherently destructive-of-view (it's retargeting), but it still routes through the same toggle; default follows the tool default.

Because retargeting only *chooses the rect*, it slots under the existing crop gesture/overlay/command with almost no new surface. The genuinely new code is the core `retarget/` module + the toggle + the "seed the staged rect from `chooseCropWindow`" call.

### 5.3 What it does NOT touch

No new command type, no new document-model field, no compositor/shader change, no canvas-expansion code (that's the *other* S16-f axis). The importance-map overlay preview (§6) can reuse the present-pass overlay lane if we want to *show* what it's protecting, but that is optional polish.

---

## 6. UX / UI (hand-holding, per the feature's stated goal)

- **Discoverability & copy.** The toggle label is "Smart Resize"; its tooltip hand-holds: *"When on, changing the crop's aspect ratio automatically keeps the most important parts of the picture (faces, edges, subjects) instead of just chopping the sides."* First-use could show a one-line hint. No Unicode glyphs in labels ([[mosaic-ui-gotchas]]).
- **The proposed crop is editable.** Never a black-box one-shot: Smart Resize *suggests*, the user *disposes*. The staged rect + handles make this natural.
- **Optional "protect" affordance (nice-to-have, scope as a follow-up).** Like Photoshop's Content-Aware Scale "protect": let the user paint/mark a must-keep region the crop window is forced to contain, and/or a "skin/face protect" that's on by default. v1 can ship with automatic face-protect only.
- **Optional importance preview.** A toggle (or momentary) to tint the importance map / show detected faces, so the user understands *why* it framed where it did — matches the "show sampled area" affordance the Inpaint backend already offers.
- **Home for tunables + credits — Settings → Tools → Crop (settled with user, 2026-07-01).** Unlike Inpaint (a sub-system with a natural settings dialog), Smart Resize is one toggle, so its non-bar controls and its credits need a deliberate home. That home is the **existing Crop tab under Settings → Tools** (where the S16-q crop-initial-framing and S16-p auto-switch settings already live — coherent per [[mosaic-design-decisions]]). It houses any Smart Resize tunables (F1 face-detection on/off, a quality preset, F2 protect defaults) and, **at the very bottom of the Crop tab, a small "About Smart Resize" note** — the friendly summary + the lineage (§7) + the honest "what it deliberately doesn't do, and why." A tiny "About…" affordance by the bar toggle can jump there. This turns the "how do we credit all these brilliant people" worry into a placed, coherent feature (the source-file cites in §7 are the always-on floor regardless).

---

## 7. Crediting & attribution (the user's stated worry, turned into a plan)

Two layers, mirroring how Inpaint handles it (`BackendInfo`: authors/paper/summary/deviations/augmentations):

1. **In source.** Every file in `src/core/retarget/` carries a header crediting the lineage it implements, and a one-line cite at each borrowed idea. Concretely:
   - *Cropping-as-best-operator* → **Rubinstein, Gutierrez, Sorkine & Shamir, "A Comparative Study of Image Retargeting," SIGGRAPH Asia 2010** (the benchmark that told us to do this).
   - *Automatic saliency/face crop-window* → **Suh, Ling, Bederson & Jacobs, UIST 2003**; **Chen et al. 2003**; **Liu & Gleicher 2006**.
   - *Face detection* → ~~Viola & Jones~~ — **not shipped** (F1 dropped 2026-07-02; no detector, no cascade, so no credit line needed).
   - *Gradient energy* → textbook (Sobel); note it is the seam-carving energy function used for a **crop** search, not a seam.
2. **In-app — at the very bottom of Settings → Tools → Crop (§6).** A `RetargetInfo` struct surfaced there: the friendly summary, the authors/papers above, and — honestly — a short *"What Smart Resize deliberately does not do, and why"* note (no seam carving / no warping — the benchmark's own finding that they are the less-liked operators). This credits the seam/warp researchers whose benchmark shaped our choice **and** tells the user what the tool is refusing to do on their behalf. `docs/third-party-licenses.md` gets a row if a face-cascade model is bundled.

This makes the attribution a first-class, user-visible feature rather than an afterthought — the same move that made the Inpaint Settings gallery work.

---

## 8. Testing (headless, per the verification division)

Claude does headless only ([[mosaic-working-with-claude]]); the user does the visual pass. Headless coverage:

- **Importance map** — golden `W` for small fixtures (a face fixture, an edge/texture fixture); deterministic byte-for-byte.
- **Face detector** — a couple of fixed fixtures with known face boxes (or a stub detector for CI so results don't depend on a bundled cascade).
- **`chooseCropWindow`** — synthetic images with a known "right answer" (importance mass in one quadrant → the window centers there; a face at an edge → the window never clips it; symmetric image → symmetric crop). Assert the *rect*, deterministically.
- **Integral-image search** — verify O(1) window scores equal brute-force sums (correctness of the summed-area table).
- **Integration** — toggling Smart Resize seeds the staged rect; Apply routes through `buildCropCommand` producing the expected canvas size (reuse existing crop-command tests). `--gui-frames N` smoke stays validation-clean.

No golden *pixel* output is needed for the crop itself (it's a plain crop); the tests target the *decision* (the chosen rect) and the map.

---

## 9. Proposed commit sequence (build plan)

One logical change per commit ([[mosaic-working-with-claude]]); headless-verify each; push at session end.

1. **`docs`** — this note (separate from code).
2. **Importance map core** — `retarget/importance_map.{hpp,cpp}` (gradient + composition; no faces yet) + tests. FLTK-free, no UI. Output-inert.
3. **Crop-window search** — `retarget/smart_crop.{hpp,cpp}` (objective + integral-image search over `W`) + tests. Still no UI (test-only reachable).
4. **Crop-tool toggle + wiring** — the `smartResize` toggle in tool.cpp; `app_window`/`vulkan_canvas` call `chooseCropWindow` to seed the staged rect on toggle/aspect-change; apply unchanged. **First user-visible point → user visual-verify.**
5. **Face detection** — ~~`retarget/face_detect.{hpp,cpp}`~~ **DROPPED 2026-07-02** (F1 resolved (a); see §4.2).
6. **Crediting gallery + copy** — the "About Smart Resize" panel (`RetargetInfo`) — **DONE 2026-07-02**: `core/retarget/credits.{hpp,cpp}` renders through the Inpainting page's `SpecPanel` at the bottom of Settings → Tools → Crop; no licence row (nothing bundled). Tooltips already live on the toggle/chips; a separate first-use hint was skipped — the chips + the offer-gated Recompose button are the discoverability.
7. *(optional follow-ups)* importance-preview tint; user "protect" region.

Steps 2–3 are safe to land early (output-inert, like inpaint's pure core). Step 4 is the first thing to eyeball.

---

## 10. Open forks for the user

The two forks the user pre-settled (name = **Smart Resize**; home = **Crop-tool toggle**) are baked in above. Remaining decisions worth the user's call before/**during** implementation:

- **F1 — Face detection depth (§4.2).** **RESOLVED 2026-07-02: (a) skip faces** (user call — cascade data/training is shipping weight the marginal quality doesn't justify; the recommendation (b) was declined with eyes open). Hooks stay dormant in `keep_regions`.
- **F2 — "Protect region" affordance (§6).** **RESOLVED 2026-07-02: shipped as keep-region chips** — auto chips with click-to-toggle plus **Ctrl-drag user chips** (recompose plan F-d). The deeper editing ideas (resize handles, "any selection tool adds a chip") are **dropped**: Ctrl-drag is enough for the purpose (user call).
- **F3 — The warp question (the big one).** v1 is **crop-only** per the benchmark + the user's "no deformation" steer. Confirm we are *not* doing warp. The **cost of reversing** later: implement a salient-preserving mesh/axis-aligned warp under §3.9-B's constraints — sizeable, and it re-introduces the deformation users disliked. The one case pure-crop can't serve well: important content at *opposite* edges with a large aspect change (must sacrifice one side). Recommend staying crop-only unless that case proves common in practice.

### Appendix — the excluded warp ceiling (documented, not built)

For completeness (so the ceiling is understood): the field's *very* top methods (SV streaming-video warp, MULTIOP) beat pure cropping on a minority of images by warping — squeezing homogeneous regions so content at both edges survives. If F3 is ever reversed, the deformation-minimizing choice would be an **axis-aligned deformation** (Panozzo 2012) constrained to *never* deform face/high-importance cells and bound by §3.9-B — but it fights the benchmark's core finding. It stays here as a note, not a plan.

---

## 11. References

**Benchmark & methods**
- Rubinstein, Gutierrez, Sorkine, Shamir. *A Comparative Study of Image Retargeting.* SIGGRAPH Asia 2010. (RetargetMe — the supplied paper.) http://people.csail.mit.edu/mrub/retargetme
- Avidan, Shamir. *Seam Carving for Content-Aware Image Resizing.* SIGGRAPH 2007. (MERL TR2007-087.)
- Rubinstein, Shamir, Avidan. *Multi-operator Media Retargeting.* SIGGRAPH 2009.
- Wang, Tai, Sorkine, Lee. *Optimized Scale-and-Stretch for Image Resizing.* SIGGRAPH Asia 2008.
- Wolf, Guttmann, Cohen-Or. *Non-homogeneous Content-driven Video-retargeting.* ICCV 2007.
- Panozzo, Weber, Sorkine. *Robust Image Retargeting via Axis-Aligned Deformation.* Eurographics 2012.

**Cropping / saliency / faces**
- Suh, Ling, Bederson, Jacobs. *Automatic Thumbnail Cropping and its Effectiveness.* UIST 2003.
- Chen et al. *A Visual Attention Model for Adapting Images on Small Displays.* 2003.
- Liu, Gleicher. *Automatic Image Retargeting / Video pan-and-scan.* 2006.
- Viola, Jones. *Robust Real-Time Face Detection.* IJCV 2004.

**Recomposition**
- Setlur, Takagi, Raskar, Gleicher, Gooch. *Automatic Image Retargeting.* MUM 2005.
