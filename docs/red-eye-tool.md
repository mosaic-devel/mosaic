# Eye retouch — the universal eye tool (flash red-eye → sclera de-redding → procedural reconstruction)

> **Status: Tiers 1 and 2 BUILT (S38-b, 2026-07-28), feedback round 1 folded in the same day
> (§9.8). Tier 3 remains deferred research.** §1–§7 are the original design note and are kept
> as the record of *why*; **§9 is the build log** — what actually shipped, where it lives,
> and every place the code disagreed with this note. Read §9 before changing anything: three of the
> note's factual claims about the tree turned out to be wrong, and the code won each time — and then
> §9.8 corrected three more claims that only real photographs could have caught.
>
> *Original framing, for the record:* **research + design note. Greenfield — NOT an extension of
> shipped code.** There was no Red Eye tool in the tree (`rg 'RedEye' src/` → zero hits; the ToolId
> enum at `src/ui/tool.hpp:20` listed 22 tools and none was red-eye). `PLAN.md:2021`/`:2546` carries
> an unchecked **S38-b — Red Eye tool** line under the (also unchecked) S38 Stamp/Clone parent, and
> `docs/icons-needed.md:44` reserves an icon name — that was the entire existing footprint *inside*
> Mosaic. This note designs the whole feature, including the flash-red-eye mode, from scratch.

---

## 1. Vision — one tool, three problems, two kinds of target

The user wants to grow a targeted "kill flash red-eye" tool into a **universal eye tool** that also
addresses the *white* of the eye — bloodshot eyes, broken/red veins in the corners, diffuse pink-eye
(conjunctivitis) redness — and, in the extreme, reconstructs an eye that is destroyed in the frame.
That is a good product instinct, but it bundles **three technically distinct problems** whose only
commonality is *"a human points at an eye."* The spine of this design is refusing to paper over that:
the three problems differ in the one variable that decides which algorithm is honest — **what the
correct output pixels *are*.**

- **Flash red-eye** — the red/orange pupil glow from a flash reflecting off the retina in a dark-
  adapted eye. The pupil's correct appearance is **a known near-constant**: a near-black disc with a
  specular catchlight. Removal is *artifact deletion over a region whose target is a constant.* No
  reconstruction — you are not inventing detail, you are removing a glow that shouldn't be there and
  restoring a value you already know. This is the classic, tractable, one-click case.

- **Sclera redness** (bloodshot / corner veins / diffuse pink-eye) — this lives on the **white of the
  eye**, whose correct output is **not a constant**. A healthy sclera is a plausible, slightly-shaded,
  faintly-vascular off-white — subtle subsurface shading toward the lids, a few soft real vessels,
  warm in the corners. "Desaturate and darken to neutral" (the red-eye move) looks *awful* here: it
  produces flat, grey, dead eyes. This is a **retouch / harmonization** problem — far closer to skin
  spot-healing than to red-eye removal. It **harmonizes existing pixels**; it does not reconstruct.

- **A destroyed eye** — a solid subconjunctival hemorrhage (a uniform blood patch) or an eye so
  bloodshot that no healthy sclera survives anywhere in the frame. Here the correct output pixels
  **do not exist in the source at all** and cannot be harmonized from what's there. This is the only
  case that needs genuine **reconstruction**, and — as §3.3 argues — the only honest classical route
  to it is **procedural eye synthesis**, not copying pixels from elsewhere.

So the design is **three tiers**, not one algorithm with a bigger brush:

| Tier | Problem | What the target IS | Operation class | Maturity |
| --- | --- | --- | --- | --- |
| **1** | Flash red-eye | a known near-constant (dark pupil + catchlight) | **artifact deletion** | shippable near-term |
| **2** | Sclera redness (bloodshot / corner veins / pink-eye) | a plausible faintly-vascular white | **selective de-redding / harmonization** | the practical headline feature |
| **3** | Destroyed eye (blood patch, no surviving sclera) | pixels that are **not in the source** | **procedural synthesis** (parametric) | ambitious research tier; not built (§6.3) |

**The recommendation up front:** ship Tiers 1 and 2 as one tool with two modes sharing a slot and a
scoping model; scope Tier 3 honestly as deferred research. Do **not** merge the three algorithms into
one auto-magic "fix eye" button — they want different math, and the automatic eye detection a
one-button tool implies is exactly what this design rules out (§4, §6).

---

## 2. Infrastructure we'd build on (what already exists)

Because this is greenfield, §2 is *not* "what the tool does today" — it is the inventory of **real,
shipped primitives** a new eye tool would compose. Every path below was verified on disk. The
punchline: **Tiers 1 and 2 are almost entirely assembly of existing parts**; nearly nothing needs a
new core algorithm.

### 2.1 Adjustment-layer colour math + the luminance-preserve toolbox (Tiers 1 & 2)

- `AdjustmentKind` (`src/core/layer.hpp:474`) already includes **HueSaturation** and **ColorBalance**;
  the typed schema lives in `src/core/adjustments.hpp` and the per-pixel math in
  `src/render/compositor.cpp` — `scalarAdjustConsts` at `:702`, the hexcone HSL helpers
  (`rgbToHsl`/`hslToRgb`) at `:786`, HueSaturation apply near `:1291` (`v.s = clamp01(v.s*satScale)`),
  ColorBalance apply at `:1309` (a smoothstep partition of unity over luma, per-channel delta, then an
  optional luminosity restore).
- **The reusable jewel is `src/core/blend_math.hpp`** (`namespace mosaic::core::detail`): the W3C
  non-separable colour toolbox — `lum(c) = 0.30r+0.59g+0.11b` (`:36`), `clipColor` (`:39`),
  **`setLum(c, l)`** (`:55`, gamut-clip a colour to a target luminance while preserving hue), `sat`,
  `setSat`, and `blendNonSeparable` (Hue/Saturation/Color/Luminosity, `:75`). This is precisely the
  math a "shift these pixels toward the surrounding sclera tone *without changing their brightness*"
  operation needs — the catchlight and the eyeball shading are luminance structure we must keep.
  `compositor.cpp:843` has `linearLum` (Rec.709) for the linear-light cases.

### 2.2 The ΔE / weighted-colour-distance seam (Tier 2 redness targeting)

`src/core/selection.hpp:186` — `wandColorDistance(Color8 a, Color8 b, bool useAlpha)` (impl
`selection.cpp:520`): a luma-weighted RGBA distance normalized to `[0,1]` (weights `0.30/0.59/0.11`;
colour vs alpha `0.80/0.20`). The header notes this is *the swappable metric seam* the planned S43-b
managed-CIELAB ΔE replaces — i.e. Mosaic already has one blessed place where "how far is this pixel's
colour from a reference" is defined, and it's slated to become perceptual. A hue-targeted de-redding
weight (how *red/magenta* is this pixel relative to the local sclera mean) is the same shape of
computation and should ride the same seam. `magicWandSelection` (`selection.hpp:194`) consumes it.

### 2.3 The S33 blur family (Tier 2 frequency-separation vein suppression)

`src/render/blur.hpp` (`namespace mosaic::render::fx`) operates on a straight-alpha `ImageF&` in place:
`gaussianBlurImage(img, sigma)` (`:28`) and — the load-bearing one — **`surfaceBlurImage(img, radius,
threshold01)`** (`:49`), an **edge-preserving separable bilateral** (Pham & van Vliet), spatial
σ = radius/2, range σ on premultiplied luma. Edge-preserving smoothing *is* the base-layer estimator
that frequency separation is built on (§3.2). Single-plane primitives (`gaussianBlur`, `boxBlurApprox`,
`extractAlpha`) live in `src/render/effect_primitives.hpp`; a GPU parity lane exists in
`src/render/blur_gpu.*`.

### 2.4 Selection / scoping (all three tiers' region model)

`src/core/selection.hpp` — `class Selection` is a document-sized 8-bit coverage mask (0..255 with AA)
with `combine(a,b,SelectOp)` (`:56`), `inverted` (`:72`), morphology `grown`/`shrunk` (`:86–87`),
`feathered` (`:90`), `smoothed` (`:93`, EDT + separable Gaussian), and `bounds` (`:112`). The
**paint-to-select engine** is `src/core/brush/mask_stroke.hpp` — `MaskStroke` with
`MaskStrokeParams{diameter, hardness, flow, opacity, spacing}` and `toSelection()` (`:61`),
coverage-only, no image analysis. `src/core/edge_grow.hpp` (`edgeGrowSelection`) is the L1 edge-aware
geodesic grow. Together these are enough to scope *any* eye operation to exactly where the user
brushed, with a soft feathered boundary — **without any face or eye detector.** (That absence is a
feature; see §4 and §6.)

### 2.5 Tool registration, host callbacks, and the command model

- Tool metadata is one row in `kToolDefs` (`src/ui/tool.cpp:28–56`), `{ToolId, name, shortcut,
  ToolGroup, ToolSlot}`; consecutive same-`ToolSlot` rows become flyout variants (the natural home for
  a two-mode eye tool). Per-tool controls are a `case` in `defaultOptionsFor` (`tool.cpp:171`); icons
  a `case` in `defaultIconSvg` (`src/ui/icon_pack.cpp:209`). Free shortcut letters include **R**
  (in use: A B C E G I J K L M T U V W Z; free: D F H N O P Q R S X Y).
- The pointer-driven pattern is a `XxxHost { std::function<…> }` on the canvas
  (`src/ui/vulkan_canvas.hpp`) with a `setXxxHost()`; the canvas owns pointer→doc-point mapping and
  the gesture, the app (`src/ui/app_window.cpp`) wires the lambda that reads the active layer,
  runs the op, and pushes **one** command (the magic-wand precedent: `setMagicWandHost` at
  `app_window.cpp:611` → `magicWandSelection` → one `SetSelectionCommand`).
- Commands (`src/core/commands.hpp`): **`SetLayerPixelsCommand`** has a **region-scoped** overload
  (`:343`, `(LayerId, Image regionPixels, long ox, long oy)` — stores/patches only the bbox), which
  is the undoable way to land a localized pixel correction; plus `FillCommand` (`:363`),
  `SetSelectionCommand` (`:592`), `CompositeCommand` (`:29`, bundle mask+pixels into one undo step),
  `SetMaskPixelsCommand` (`:433`).

### 2.6 The classical inpaint engine — and why it is NOT the reconstruction path

`src/core/inpaint/inpaint_engine.hpp` (`InpaintEngine::run`, `makeDefaultEngine`) dispatches to
classical `IInpaintBackend`s in `src/core/inpaint/backends/`: `pde/` (Telea / Bertalmío
Navier–Stokes, the current default), `he_sun/` (He & Sun offset-statistics graph completion),
`resynth/` (Harrison texture synthesis). It is invoked as a `FillFn` via
`src/core/retarget/inpaint_fill.cpp`. **The ML/script backend (`backends/script/`) is out of scope for
this feature and is not referenced anywhere in this design.** As §3.3 explains, the classical engine
is the right tool for a *localized* blemish with healthy neighbours, but it is the **wrong** tool for
a destroyed eye — and Tier 3 does not route to it.

### 2.7 What does NOT exist (and must not be quietly assumed)

`rg 'detectFace|landmark|haar|dlib|opencv|eyeDetect|pupilDetect|glint|catchlight' src/` → **nothing
real.** Mosaic has **no** face detector, **no** eye/iris/sclera segmenter, **no** pupil/disc/blob
finder, **no** catchlight locator. A new eye tool must either supply its own detection *or be purely
click/brush-scoped.* This design chooses **brush-scoped**, and keeping every trace of automatic
eye/iris/sclera detection out of the tool is a hard, deliberate constraint (§4, §6) — not a gap
waiting to be filled.

---

## 3. Technical decomposition — the three tiers and their concrete primitives

### 3.1 Tier 1 — flash red-eye (artifact deletion over a known-constant target)

**Problem shape.** Within a user-indicated pupil region, some pixels carry an unnatural red/orange
glow. The correct pupil is a dark, near-neutral disc with one bright specular catchlight to keep. This
is the textbook case and the algorithm is 20+ years old at its core.

**The operation (all from §2 parts):**
1. **Scope.** The user drags a small ellipse (or brushes) over the pupil → a feathered `Selection`
   via `MaskStroke::toSelection()` / a marquee. No detection.
2. **Redness mask inside the scope.** Per pixel, a redness score `r = R − max(G,B)` (or the
   ΔE-seam distance to a reference red, §2.2) gates a soft coverage: red-dominant pixels are "the
   glow," the (already dark or already neutral) rest is not. A single fixed threshold with a ramp —
   **no trained classifier** (§6.1).
3. **Correction — the classical move.** Where the glow mask is hot, **collapse chroma toward
   neutral while preserving luminance**, then pull luminance down toward the dark-pupil target *except*
   at the specular catchlight (highest-luma sub-region), which is preserved. Two long-published
   formulations to design from:
   - reduce chrominance toward zero (achromatic pupil) while **preserving luminance** to keep the
     catchlight — a CIELab-redness-mask correction (the early-2000s formulation);
   - soft, probability-weighted red replacement toward the green/blue average,
     `R' = (1−p)·R + p·avg(G,B)` (the mid-2000s formulation).
   Both are implementable directly from `blend_math.hpp`'s `setLum`/`lum` (§2.1): compute the target
   neutral, `setLum` it to the (possibly lowered) local luminance, blend by the mask.
4. **Commit.** One region-scoped `SetLayerPixelsCommand` (`:343`).

**Why it's clean:** the target is a constant, so there is nothing to invent; the whole tier is a
masked colour transform + a preserved highlight, entirely inside existing primitives.

### 3.2 Tier 2 — selective sclera de-redding / vein suppression (harmonization)

**Problem shape.** The white of the eye is too red — diffuse (pink-eye), streaky (broken corner
veins), or globally pink (bloodshot). The correct output is a *plausible faintly-vascular white*, not
a constant and not a flat grey. Over-correction to a pure white is the "dead-fish / uncanny" failure
mode; real eye retouch **leaves faint vascularity** and keeps the corner warmth. This is a retouch
problem, and the design offers **two composable sub-operations**, both brush-scoped:

**(a) Hue-targeted desaturation + lightening, pulled toward the local sclera tone.** Constrain the
effect to reds/magentas (a hue-window weight, or the ΔE-seam distance from the *local sclera mean*
sampled just outside the reddest pixels), then, per pixel and weighted by an **amount** slider:
- reduce saturation toward the surrounding sclera's saturation (not to zero);
- raise luminance toward the local sclera luminance;
- optionally nudge hue from red/magenta toward the sclera's slightly-warm neutral.
All three are `HueSaturation`-class + `setLum` operations (§2.1) restricted by a hue/ΔE gate (§2.2).
The "pull toward the *neighbourhood* value, not toward a global constant" is what makes it
harmonization rather than a flat wash — the same instinct as the sky PhotometricMatch grade.

**(b) Frequency-separation vein suppression.** Veins are **high-frequency red detail over a smooth
low-frequency base.** Separate the eye region into a base (edge-preserving smooth) and a detail
(residual) layer, then **attenuate the red component of the detail** while keeping luminance detail
intact:
1. `base = surfaceBlurImage(region, radius, threshold)` (§2.3) — edge-preserving so it doesn't smear
   the lid/iris boundary; `detail = region − base`.
2. Scale down the **chroma / red-axis** part of `detail` by the amount (keep the luma part of the
   detail so the eye still reads as a real, textured surface — this is the anti-dead-fish rule baked
   into the math, not just the UI).
3. Recompose `base + attenuated_detail`, blend by the brushed mask.
Frequency separation for skin/blemish retouch is decades-old published DSP practice; doing it on the
red/chroma channel only is a standard variation.

**The vascularity floor (a first-class control, not a nicety).** Both sub-operations are clamped by a
**"keep vascularity" floor**: the amount slider cannot drive the region past a configured residual of
its original saturation/detail. The default under-corrects. The floor is what separates believable eye
retouch from the plastic look, and it doubles as a safety property (§4): the tool *cannot* fully
whiten an eye, so it can't turn a medically-red eye into a fake-healthy one in a single drag.

**Primitives used:** `surfaceBlurImage` + `gaussianBlur` (§2.3), `blend_math` `setLum`/`setSat`/`sat`
(§2.1), the ΔE seam (§2.2), `MaskStroke`/`Selection` scoping (§2.4), region-scoped
`SetLayerPixelsCommand` (§2.5). **No new core algorithm** — Tier 2 is the practical headline feature
and it is assembly.

### 3.3 Tier 3 — destroyed eye → procedural eye synthesis (parametric, algorithmic)

**Problem shape.** A solid subconjunctival hemorrhage (uniform blood patch) or an eye so bloodshot
that no healthy sclera survives. The correct pixels **are not in the image.**

**Why inpainting is ruled out.** Exemplar / patch / offset-statistics inpainting (§2.6) completes a
hole by **copying and stitching pixels that exist elsewhere in the source** (or a donor). For a
destroyed eye there is *no clean donor sclera anywhere* — if both eyes are bloodshot, the "best match"
patches are themselves red. Classical inpaint can only smear the same bad colour across the patch; it
cannot invent a healthy sclera, an iris with the subject's pattern, a pupil, or a catchlight, because
none of those exist to copy. The PDE and offset-statistics backends are honest for a *small* defect
with healthy neighbours (a stray reflection, a lash) — **but Tier 3 does not route to them**, and the
tool must not pretend otherwise.

**Why ML is ruled out.** Project policy: Mosaic ships no bundled ML. (It is also not obviously
*better* here — a generative model would hallucinate an iris that isn't the subject's — but policy
settles it regardless.)

**Therefore the only faithful classical route is procedural eye synthesis:** build a plausible eye
*algorithmically from a parametric model*, fit it to the destroyed eye's geometry and the scene, and
composite it in. The model components:

- **Sclera base** — an off-white base tone (sampled from any surviving sclera, or from the
  contralateral eye, or a user swatch), shaded for **eyeball curvature** (a sphere-normal falloff,
  darker toward the lids and the far canthus) with mild subsurface warmth in the corners.
- **A controlled sparse vascularity layer** — a *procedurally generated* faint vein network on the
  sclera (a few soft, low-contrast branching curves), tuned to the same "faint, not absent"
  vascularity floor as Tier 2. This is what keeps the synthesized eye from looking like a marble.
- **Iris** — matched to the subject's iris **colour and pattern**, taken from the contralateral
  healthy eye where one exists, else a user-chosen colour with a **procedurally generated** radial
  fiber / crypt / collarette texture. **Invariant: the iris is generated algorithmically, never
  sampled-and-merged from a database of other people's eyes** — that distinction is load-bearing and
  is restated as a design constraint in §6.3. Sized to the fitted limbus.
- **Pupil** — a dark disc sized to the scene brightness, concentric with the iris.
- **Catchlight** — one specular highlight placed from the **scene light direction** (inferred from the
  other eye's catchlight, or other specular cues, or a user handle) on the fitted corneal sphere.

**The fitting problem (the hard, honest part):**
1. **Eye geometry** — the eye opening (lid contour) and the eyeball sphere must be recovered to place
   everything. With no detector in the tree (§2.7), Tier 3 leans on **user-placed control geometry**
   (drag the lid outline / limbus circle / pupil centre) rather than automatic landmarking — the same
   no-detector posture the shipping tiers keep (§6.3).
2. **Iris colour / pattern match** — transfer hue + pattern statistics from the contralateral eye, or
   accept a user swatch; mirror horizontally as needed.
3. **Lighting match** — shade the sclera and iris and place the catchlight to agree with the scene's
   dominant light so the inserted eye doesn't read as pasted; this is classical relighting from a
   handful of cues, not a learned model.
4. **Compositing seam** — blend the synthesized eye into the surrounding lids with a gradient-domain /
   seamless-cloning seam (Pérez, Gangnet & Blake, "Poisson Image Editing," SIGGRAPH 2003) so the
   transition to real skin is invisible.

**Honest scoping.** Tier 3 is a **research tier**, not a near-term deliverable: it is genuinely hard
(parametric-model fit + relight + seam is a small research project of its own), and it is subject to
the design constraints in §6.3. It is documented here so the vision is on record, **not** because it
is buildable today.

---

## 4. Proposed tool shape + UX

**One tool, one slot, two shipping modes (Tier 3 deferred).** Register a single `ToolId::RedEye`
(shortcut **R** is free, §2.5) in the retouch/heal group, presenting as a small flyout with two modes:

- **Mode 1 — Red-Eye (flash).** Fast, deterministic, one-drag. Drag an ellipse (or click-brush) over
  the pupil; on release, Tier 1 runs and lands one undoable command. Controls: **size** (the scoped
  region), **strength**, **darken** (how far toward the dark-pupil target), **keep catchlight**
  (on by default). No auto-scan of the image; the correction only ever touches where the user pointed.

- **Mode 2 — De-redden / Whiten (sclera).** A **brush-scoped** selective-desaturation mode. The user
  paints over the bloodshot white; the effect accrues under the brush (Tier 2). Controls: **amount**
  (0 → the floor), an explicit **keep vascularity** floor slider (default high — under-correct),
  **method** (harmonize-only / +vein-suppression), **spread** (feather), and a **corner-warmth
  protect** toggle (leave the canthus tint alone). Amount is capped by the floor; it is *not possible*
  to slam it to a dead white.

**Why brush-scoped, not full-auto, is the default (three reasons, all real):**
1. **Medical-honesty / safety.** An always-on "fix eyes" pass would silently "correct" a genuinely
   red eye (illness, injury, a portrait where the redness is the point) and could bleed onto eyelid
   skin or lip. A human deciding *where* is the correct default for a destructive-looking edit.
2. **Quality.** Real eye retouch is a judgment call about *how much* to leave. The vascularity floor
   plus a human amount beats any auto-target.
3. **⚠ INVARIANT — no automatic eye/iris/sclera detection, anywhere in this tool.** The *user*
   supplies the region; the tool never localizes, segments or classifies an eye, an iris, a sclera or
   an iris–sclera border, and never scans the image for candidates. This is a hard constraint on the
   whole feature (restated per tier in §6), not an implementation shortcut — adding "snap to the eye"
   later would be a deliberate reversal of a settled design decision, and would have to be argued as
   one.

**Authenticity / under-correct default.** Across all modes the tool ships tuned to *under*-correct:
Tier 1 leaves the catchlight and a hint of pupil structure; Tier 2 leaves faint veins and corner
warmth; a Tier 3 (if ever built) synthesized eye carries the sparse vascularity floor. The house
default is "a believable eye," never "a whitened cartoon."

---

## 6. Design constraints per tier

The recurring lever is the same one the UX already wants: **user-brushed scope, no automatic
detection, standard published image-processing math.** Each constraint below is an invariant of the
shipped design, not a preference — the T-codes are referenced from the build log (§9).

### 6.1 Tier 1 (flash red-eye)

- **T1-a — User-scoped, no auto image scan.** The user indicates the pupil; the tool corrects only
  the brushed region. No whole-image candidate search, no face detector, and no adaptive "decide
  whether to detect or correct first" logic.
- **T1-b — Fixed-threshold redness gate, NO trained classifier.** The glow mask is a simple
  hue/redness threshold with a ramp inside the brushed region — **no** machine-learning classifier,
  **no** learned false-positive rejection, **no** degradation-class routing to different correction
  strategies.
- **T1-c — Colour-only correction from published technique.** Use the classical formulations of §3.1
  (reduce chroma, preserve luminance, keep the catchlight; soft red → avg(G,B)), built from
  `blend_math`'s `setLum`/`lum`. Cite the lineage in the source header.
- **T1-d — No object classification, no glint *detector*.** The mask is hue-based within the scope;
  do not classify candidate objects by YUV chroma-vs-luma inequality counts, and do not *detect* the
  pupil by intensity-peak thresholding — the user placed the region; the catchlight is preserved as
  the local luma max, never used to *find* the eye.

### 6.2 Tier 2 (sclera de-redding / vein suppression)

- **T2-a — No automatic eye/iris/sclera detection or segmentation; no iris–sclera border detection.**
  The user brushes the sclera. The tool computes **no** eye-region localization, **no** sclera
  segmentation, and **no** pupil↔iris/sclera **border by luminance**. This is the single most
  load-bearing constraint on the whole feature (§4-3) and every spatial estimator the tool grew since
  (§9.3, §9.8, §9.9) was built to respect it.
- **T2-b — Colour op from published technique.** Brightening + chroma-reduction of the sclera toward a
  plausible white is a long-published operation. Pull toward the **local** sclera tone
  (harmonization), not a fixed target.
- **T2-c — Frequency separation from standard published DSP.** Base/detail split via an
  edge-preserving base-layer estimator, attenuating the **red/chroma detail** only. Keep it
  **user-scoped** — it must never become part of an automatic beautification pass.
- **T2-d — No biometric vessel extraction / template.** The vein handling is *suppression* for
  appearance; the tool never segments the sclera to **extract a vessel pattern as an identity
  template**, and never stores or matches one.
- **T2-e — The vascularity floor is a hard limit, not a taste default.** The tool cannot fully whiten
  an eye, and it never inserts a glint by locating the pupil border. At a floor of 1.0 it is a
  byte-exact no-op, and that ceiling is pinned by a test (§9.3).

### 6.3 Tier 3 (procedural synthesis) — not built

Tier 3 is **deferred research**: the parametric-model fit + relight + seam is a small research
project of its own (§3.3), and there is no stub in the tree, deliberately — a stub is a design
commitment, and Tier 3 does not have one to make yet. If it is ever built, these constraints carry
over from the shipping tiers:

- **H1 — Geometry from user-placed control handles, not from fitting a model to the damaged eye.**
  In the destroyed-eye case there is no intact eye in the image to fit to; the geometry comes from
  the user's handles and the iris from the **contralateral** healthy eye or a **user swatch**.
- **H2 — Procedural iris, never a database-patch-merge.** Generate the iris **algorithmically**
  (radial fibers / crypts / collarette, e.g. from the Lefohn 2003 layered ocularist method) rather
  than sampling and merging patches from a collection of real eyes (§3.3).
- **H3 — Flat/painted composite, not multi-component differential rendering.** No corneal
  un-refraction, no per-component physical eye rendering.
- **H4 — Catchlight from scene-light geometry, not pupil-border glint insertion.** Place the
  highlight from the inferred scene light on the corneal sphere; never by detecting the pupil/iris
  border and inserting a glint there.

The classical published literature (Lefohn, Budge, Shirley, Caruso & Reinhard, "An Ocularist's Approach to
Human Iris Synthesis," IEEE CG&A Nov 2003; Blanz & Vetter's 3D Morphable Model, SIGGRAPH 1999; Pérez,
Gangnet & Blake, "Poisson Image Editing," SIGGRAPH 2003; the LeGrand two-sphere eye model) is the
design vocabulary if and when Tier 3 is picked up.

---

## 7. Open questions

**Open design questions for Tier 3** (all independent of whether it is ever scheduled): how the
geometry is user-specified without a detector (§3.3-1); iris colour/pattern transfer from the
contralateral eye when only one eye is healthy; the lighting/relight cue extraction; the seam blend
into moving lid skin.

**Open question that touches scope regardless of tier — do we want any *assisted* scoping at all?**
The whole design rests on *no automatic eye/sclera detection* (§4-3, T2-a). A future release adding
face-aware "snap to the eye" would be reversing that, not extending it; it is a posture decision for
the user, not an implementation one.

---

## 8. Build phasing

A small first slice, then the headline feature, then research.

- **Phase 1 — Tier 1, flash red-eye (the small first slice). BUILT, S38-b — see §9.2.** Register
  `ToolId::RedEye` (shortcut R — in the event, **Y**; see §9.5-1)
  + the flyout scaffolding + Mode-1 controls (size/strength/darken/keep-catchlight); the redness-gate
  + the classical colour correction (§3.1); one region-scoped `SetLayerPixelsCommand`. Tests modeled on
  `tests/test_adjustments.cpp` + `test_commands.cpp` (golden pupil-disc corrections, catchlight
  preserved, byte-identical outside the scope). This is a self-contained, low-risk, genuinely useful
  tool on its own.
- **Phase 2 — Tier 2, sclera de-redding (the headline feature). BUILT, S38-b — see §9.3.** Mode-2
  brush + amount + the
  **vascularity floor** + harmonize sub-op (§3.2a); then the frequency-separation vein-suppression
  sub-op (§3.2b) reusing `surfaceBlurImage` (*in the event, re-implemented in core — §9.5-2*). Tests
  modeled on `test_blur_kernels.cpp` +
  `test_mask_stroke.cpp` (floor is respected, corner warmth protected, luma detail preserved). This
  is where the "universal eye tool" value actually lands for real photos.
- **Phase 3 — Tier 3, procedural synthesis (research, under §6.3's constraints). NOT BUILT, no
  stub.** Not scheduled. If it is ever picked up, prototype the parametric eye model + fit + seam
  from the published classical literature (Lefohn 2003, Blanz–Vetter, Poisson blending, LeGrand).
  Until then it stays a documented vision, not a backlog item.

**One-line summary:** ship Tiers 1 and 2 as a single brush-scoped, under-correcting eye tool built
from standard published image-processing art, with **no automatic eye detection anywhere**; keep
Tier 3 procedural eye synthesis as deferred research.

---

## 9. Build log — S38-b, 2026-07-28 (Tiers 1 and 2)

**Phase 1 and Phase 2 are both BUILT. Phase 3 is untouched and has no stub anywhere in the tree** —
deliberately, per §6.3: a stub is a design commitment, and Tier 3 does not have one to make yet.

### 9.1 What shipped, and where

| Piece | Lives in |
| --- | --- |
| The whole colour engine, both tiers | `src/core/red_eye.hpp` / `.cpp` (`mosaic::core`) |
| Gesture math — mode/options/scope resolution | `src/ui/red_eye_gesture.hpp` / `.cpp` (FLTK-free) |
| Tool registration + the two option sets | `src/ui/tool.hpp`, `src/ui/tool.cpp` |
| Canvas gesture (scope stroke, ring, preview) | `src/ui/vulkan_canvas.hpp` / `.cpp` |
| Host wiring (layer, clip, command) | `src/ui/app_window.cpp` (`MainWindow::redEyeApply`) |
| Tests | `tests/test_red_eye.cpp` |

`core::retouchEye(src, scope, params)` is the single entry point. It is pure, FLTK-free,
render-free, and returns a `RetouchPatch` — the changed rectangle plus its layer-local origin, which
is exactly what the region-scoped `SetLayerPixelsCommand` overload (§2.5) takes. **An unchanged
result returns an EMPTY patch**, so a click that found nothing red lands no undo step at all and the
status bar says so instead.

### 9.2 Tier 1 as built (§3.1)

1. **Scope.** A `core::brush::MaskStroke` painted by the user (§2.4's engine), intersected with the
   document's own selection, resampled onto the active raster layer's pixel grid.
2. **Redness gate.** `flashGlowScore()` — two fixed ramps multiplied: an absolute **red excess**
   `R − (0.70·G + 0.30·B)` over `[0.16, 0.38]`, times a **purity** term `excess / R` over
   `[0.30, 0.55]`. Both constants are compile-time (T1-b). The green-weighted excess (rather than
   the textbook `R − max(G,B)`) is what makes it work on *magenta* red-eye, which is common and on
   which the textbook form reads ≈ 0. The purity term is what rejects skin outright.
3. **Correction.** `avg(G,B)` replaces the red (§3.1's second formulation) to recover the pupil's
   honest luminance; the chroma is then collapsed and the luminance set with `blend_math`'s
   `setLum` (§3.1's first formulation); the luminance is pulled toward `0.06 + 0.25·lBase` by
   **Darken**, and
   that pull alone is withheld over the specular highlight by a fixed luminance rolloff
   `[0.55, 0.80]` (T1-d — a highlight is *preserved*, never used to *find* anything).
4. **Commit.** One region-scoped `SetLayerPixelsCommand` per gesture.

Idempotence is structural rather than bolted on: a neutral colour scores exactly zero, so a fully
corrected pixel is an exact fixed point and clicking twice cannot keep darkening.

### 9.3 Tier 2 as built (§3.2)

> **⚠ Superseded in part by §9.8 (2026-07-28).** The reference tone is now the least-red of the
> scope's *brighter half*, the frequency-separation sub-op has been replaced by a local-white field,
> and the harmonization gained a saturation ceiling, a hue nudge and a whitening licence. What
> follows is the original build and is kept as the record of what was tried; where it disagrees with
> §9.8, §9.8 is the code.

`scleraReference()` computes the scope's own tone as the **mean of its least-red 30%** (a 64-bin
histogram over the red-excess axis, gathered at ≥128 coverage and falling back to >0 for an
all-soft stroke). It is a summary statistic of the region the user painted — not a segmentation, and
it never looks outside the scope.

- **(b) Vein suppression runs first.** The base layer is an edge-preserving separable bilateral over
  the scope padded by the filter support; `detail = src − base` is split into its luma and chroma
  halves and **only the chroma half is attenuated**, by `coverage · amount · (1 − floor)`. The luma
  detail is untouched, which is the anti-dead-fish rule expressed as math (`tests/test_red_eye.cpp`
  asserts the vessel stays darker than the white beside it).
- **(a) Harmonization then grades what survives.** Weight =
  `coverage · amount · scleraRednessScore`, damped by the corner-warmth window. Saturation moves
  toward the reference's, floored at `floor · s`; luminance is lifted toward the reference's, never
  lowered.
- **The relative-luminance gate is new** (not in §3.2, added from the photographs): the redness
  score is multiplied by `smoothstep(0.45, 0.72, lum(pixel) / lum(reference))`. An iris, a lash and
  a pupil sit far below the sclera around them *at any exposure*, which an absolute threshold cannot
  express — a night-flash sclera is darker than a studio-lit iris. This is what keeps a brush that
  strays over the iris from de-reddening it.
- **The corner-warmth protect is a hue window on `G − B`.** Measured across the conjunctivitis and
  hemorrhage photographs, scleral injection reads red-to-*magenta* (`G − B` ≈ −0.08…+0.10, because
  the sclera beneath is blue-white) while skin and the canthus read red-to-*orange* (`G − B` ≈
  +0.06…+0.16). The distributions overlap, so it is a soft damp (up to 75%), not a gate.
- **The vascularity floor binds every half of the effect** — the desaturation, the luminance lift
  *and* the vein attenuation. At a floor of 1.0 the tool is a byte-exact no-op. That is both the
  quality property (§3.2) and the hard limit T2-e states (§6.2), and it is pinned by a test.

### 9.4 The tool's final shape

`ToolId::RedEye` ("Red Eye") and `ToolId::RedEyeSclera` ("De-redden Eye") share `ToolSlot::RedEye`
in the PaintFill group, i.e. one toolbar button with a two-row flyout — §4's "one tool, one slot,
two shipping modes". Both modes are one gesture: paint a scope, release, one undoable command.
The brush ring **is** the scope, so what the ring covers is what a click corrects.

- **Red Eye (flash):** Size · Strength · Darken · Keep catchlight.
- **De-redden Eye (sclera):** Size · Amount · Keep veins · Method (Harmonize / Harmonize + veins) ·
  Corner warmth · Spread (secondary).

Nothing is computed or displayed during the stroke beyond the raw painted trail and the ring — the
same release-only posture the L1 edge brush keeps, and for the same reason: no region of the image
is ever "identified" for the user.

### 9.5 Where this note and the code disagreed — the code won

1. **§2.5 says shortcut "R" is free. It is not.** `VulkanCanvas::onKeyDown` claims bare `r` for the
   canvas rotate gesture (and double-tap-R for reset) whenever the canvas has focus, which is
   normal, so a tool bound to R would fire unreliably. The tool ships on **Y** ("eYe"), which is
   genuinely free. X and D are also taken (swap / default colours) — §2.5's free-letter list is
   stale in both directions.
2. **§3.2b says to reuse `render::fx::surfaceBlurImage`. Core cannot.** `mosaic_render` *depends on*
   `mosaic_core`, so a core module calling into render inverts the dependency. The base-layer
   estimator is therefore re-implemented in `core/red_eye.cpp` — same construction, same Pham & van
   Vliet lineage, cited in the header — exactly as `core/selection.cpp` carries its own separable
   Gaussian + EDT rather than reaching into render. The range sigma is 0.30 (set from the
   photographs: a vessel is ~0.2–0.35 apart in luma from the sclera, the limbus ~0.5–0.6, so 0.30
   smooths across the vessels and stops at the limbus).
3. **§4's "retouch/heal group" does not exist.** `ToolGroup` has five values and none of them is a
   retouch cluster; the tool sits in `PaintFill` beside the Inpaint brush, which is where the
   nearest shipped neighbour already lives.
4. **The sclera variant is owed its own icon.** Both variants currently resolve to the pack's
   reserved `red_eye` art. Giving the second mode its own glyph means a new SVG, a new key in
   `src/ui/CMakeLists.txt`'s embed list, a bump of the 39-key census in `tests/test_icon_pack.cpp`,
   and a `docs/credits.md` row — deferred rather than rushed, and noted in `ui::iconKeyFor`.

### 9.6 What the real photographs changed

Every constant above was set against a local corpus of real flash-red-eye, conjunctivitis and
subconjunctival-hemorrhage photographs (never copied into the repo; the tests use synthetic figures
generated in-test). Three design changes came straight out of looking at them:

- **The redness axis was re-weighted.** `R − max(G,B)` — the form §3.1 suggests — measures ≈ 0.16 on
  a magenta retinal reflection, i.e. below any usable threshold, while the same pupil measures 0.29
  on the green-weighted axis. Flash red-eye is *frequently* magenta or purple, not red.
- **The purity term was added.** On the absolute axis alone, warm skin (0.19–0.22) and a mild glow
  (0.27–0.29) overlap. Normalised by the pixel's own red they separate cleanly (skin 0.2–0.3, glow
  0.5–0.95).
- **The relative-luminance gate replaced an absolute one** (§9.3), because sclera luminance across
  the corpus spans 0.22–0.55 depending on exposure — no fixed level separates sclera from iris
  across that range, and a ratio against the scope's own reference does.

The corpus also settled a limit worth stating plainly: **a saturated red garment and a retinal
reflection are the same colour**, and no per-pixel metric can separate them. `tests/test_red_eye.cpp`
asserts that the metric fires on both, and asserts separately that the garment survives untouched
*because it is outside the scope*. The tool is safe because the user says where (§6.1 T1-a), not
because the metric is clever — which is exactly the property the whole design rests on. "Fixing"
this with automatic detection is not an option: it would break T1-a/T2-a, the invariant the feature
is built around (§4-3).

### 9.7 Still owed

- A visual pass on both modes **in the real app**. Both modes have now been driven over the real
  corpus headlessly (§9.8) and checked at 1:1, but not yet with a hand-painted stroke on a canvas.
- The sclera variant's own icon (§9.5-4).
- Tier 3: unchanged. Deferred research (§6.3).

### 9.8 Feedback round 1 — the red rim, and "de-redden barely works" (2026-07-28)

Both defects the user reported, and both were real. Everything below was measured on the local
corpus (`~/Desktop/redeye`, never copied into the repo) and the tests use synthetic figures built
from those measurements.

**(1) "Red-eye tool leaves a red rim around the iris."** Reproduced exactly. A glow does not end at
a threshold — it *fades* into the iris over a pixel or two — and `flashGlowScore` multiplies two
steep ramps, so those transition pixels score ~0 while the disc inside them scores 1. The radial
profile of a real corrected pupil (glow ends at r=33): residual red excess **0.00 at r=33, +0.10 at
r=34, +0.16 at r=35, +0.04 at r=36**. That bump *is* the ring — red that comes back as you walk
outward, which the eye reads as a drawn circle rather than a fade.

Fix: **hysteresis** (Canny 1986) on the score field, inside the scope. The shipped ramps stay
exactly as they were and remain the *strict* pair; a second, permissive pair
(`kFlashWeakExcess/Purity*`) is admitted only where the strict one already fired nearby, measured by
blurring the strict score over `rimReach` (derived from the tip, clamped 2–24 px). The discriminator
that keeps this off the iris is **purity**, not excess: a rim pixel spends its red on being red
(0.7–0.9) while a brown iris spends it on being bright (0.2–0.4). Result on the same pupil: 0.00 at
r=34 and r=35, and r=36 (the iris proper, excess 0.041) untouched. Verified across five flash
photographs including brown, grey, green and blue irises — no iris damage in any of them. The pass
is insensitive to `rimReach` (identical output from 3 px to 30 px), because the ramps, not the
reach, decide what the core may vouch for.

**(2) "De-redden eye works pretty bad; multiple passes at 100% amount and 0% keep-veins only makes
the veins a dull colour."** Three independent causes, all confirmed by measurement:

- **The reference tone was landing on the lashes.** `scleraReference` took the least-red 30% of the
  brushed pixels — but red excess falls to zero on anything dark, so the least-red pixels of a real
  bloodshot eye are the lid shadow, the lashes and the iris. On `conjunctivitis_disease.jpg` it
  returned **luminance 0.51 for an eye whose sclera measures 0.87**. Every downstream term is
  relative to that tone, and the luminance lift is `max(lPix, lRef)` — so with `lRef` *below* the
  sclera it lifted nothing at all. Fixed: the least-red of the **brighter half**, which returns
  0.79–0.81 on the same frame.
- **Only the chroma half of the detail was attenuated.** That is precisely "the veins become a dull
  colour": the vessel keeps its full darkness and lands as a grey streak on a white sclera. What the
  user asked for — *remove some vessel and put the white back* — needs the luminance too.
- **Harmonization aimed at the region's own least-red tone**, which on a thoroughly injected eye is
  itself pink. So repeated passes converge to pink and stop, exactly as reported.

Fix: a **local white field** replaces the frequency-separation base. Per pixel it is the normalized
weighted mean (Knutsson & Westin 1993) of the neighbours that vote as plausibly-unvascular sclera —
bright enough, no redder than the scope's reference, not warm — reaching `4 × veinRadius`, together
with an `evidence` term saying how much such vote there was. A vessel is then **replaced** by that
white, colour *and* darkness, weighted by how much redder than it the pixel is; the diffuse pinkness
left over is graded toward the same white, now with a saturation ceiling and a hue nudge so it comes
out white rather than beige. Rejected on the way: a per-channel morphological closing (Serra 1982)
— correct in principle, but a dense vessel *mat* is wider than any structuring element that spares
the iris, and it invents colours by mixing one neighbour's red with another's green; and a
luminance-ordered closing, same reach problem.

**The whitening licence (`scleraWhiteness`) is new and is a safety property, not a quality one.**
The obvious version of the ceiling turns a subconjunctival hemorrhage into a flat, confident grey
patch — it desaturates blood without any white to lift it toward. So every whitening step is scaled
by how plausible the region's own reference is as an eye white (two absolute ramps on saturation and
red excess). Conjunctivitis references score 0.99–1.00 and the tool corrects hard; the hemorrhage
frames score **0.00** and it declines. That keeps §1's line intact — Tier 2 harmonizes what is
there, and reconstructing an eye that shows no white is Tier 3, which is not built.

**The iris gate is a red-channel ratio, not luminance.** Gating the replacement on `lum/localWhite`
left the darkest vessel cores uncorrected as **red speckle** while the rest of the vessel went white
— invisible in a downscaled preview, obvious at 1:1. Blood absorbs green and blue and reflects red,
so a vessel is dark in luminance while its red channel stays near the sclera's; an iris or a lash is
dark in every channel. Measured against the local white, the red ratio separates the two populations
**four times better** (10th/90th-percentile gap +0.27 vs +0.07). Switching the gate removed the
speckle without touching the iris.

**The §6 constraints are unchanged and, on one axis, tighter.** Both new spatial filters run
strictly inside the painted scope, seeded only by the user's stroke and compile-time ramps; nothing
is detected, segmented, localized, stored or matched (T1-a/T1-b/T2-a/T2-d all restated in the source
header). The whitening licence is a second and stricter limit of the same kind as the vascularity
floor (T2-e): the tool cannot whiten beyond what the region itself demonstrates.

Tests: 24 cases in `tests/test_red_eye.cpp` (six new), each pinned to a measurement rather than a
round number — the rim assertion is stated over every pixel at least as red as the ring that was
being left, and the "no white anywhere" case asserts the hemorrhage is *not* corrected.

### 9.9 Feedback round 2 — the rim was three defects, not one (2026-07-28)

Round 1 fixed the *gate*'s contribution to the rim and the user reported it still there. It was: the
gate was only one of three causes, and the other two were invisible to round 1's verification
because that verification fed the module the **dataset masks** as the scope. In the app the *brush
ring is the scope*, so its own edge lands on the pupil — the harness has to paint a real
`MaskStroke` click, and once it did, all three showed up immediately.

**(1) The scope's coverage was SCALING the correction.** A soft brush edge multiplied into `w`
leaves a half-corrected annulus wherever the ring's shoulder crosses live glow — and since the tool
asks you to size the ring to the pupil, that is exactly where it lands. The tool was *drawing* the
ring, not leaving it. In this mode coverage is now a **gate** (`kFlashScopeGate`): full correction
above 35% coverage, none below, sub-pixel AA at the boundary only. Half-corrected red is never the
answer here — the corrected pupil is idempotent, so a crisp boundary costs nothing. (The sclera mode
keeps coverage as a true weight: there the soft edge is what blends a graded retouch into the rest
of the white.)

**(2) The ring was promising more than the scope delivered.** At the shipped tip hardness of 0.92, a
70 px ring's coverage is already past half by r = 33.3 and gone by r = 34.5 — the outer ~2 px of the
circle the user dragged corrected weakly or not at all. Line the ring up with the glow and those
2 px *are* the glow's edge. The flash tip is now **hard**; `MaskStroke` still anti-aliases at
hardness 1, so this costs no jaggedness, and the ring now means what it shows (measured: a 140 px
ring's scope reaches r = 69.75 of 70, against ~67 before).

**(3) Correcting to neutral is not enough, because an iris is not neutral.** The deepest of the
three. Tier 1's target is a dark *neutral* disc, which is right for a pupil — but the fade at its
edge is not pupil, it is **iris with a glow over it**, and a neutral band on a blue-green iris still
reads as a warm circle drawn on it. Measured on the corpus: after round 1 the band sat at excess
+0.02…+0.03 against an iris of **−0.105** — a step of 0.13, nearly neutral in absolute terms and
plainly a ring in context. So Tier 1 gained the same estimator Tier 2 has: a **local iris tone**
(the tone of the brushed pixels that are not glow), and the correction target's chroma now slides
from that tone to neutral as the pixel becomes pupil, with the darkening on the same slider — the
disc goes dark, its edge only loses its red. The permissive half of the hysteresis is measured
against it too (`kRimAboveIrisLo/Hi`), which is what finally makes it safe: "redder than the iris
beside me" fires on a glow's tail over any iris colour and is flat zero on a brown iris, which no
absolute threshold can manage — a brown iris and a glow's tail measure the same red excess.

Two details that cost a round each. The iris estimate must **exclude the fade from its own vote**
(the fade is the thing being measured against it; letting it vote drags the reference red and the
rim survives at half strength — measured, exactly that), which the support field already computed
answers. And the estimate must be allowed to **read past the scope** — the case that matters most is
a ring sized to the pupil, where the only iris to aim at is just outside the ring the user drew.
That is the same "a read, never a write" the shipped Tier 2 took for its base-layer support: only
the ROI is returned and only the ROI is ever corrected.

Result on the corpus, per eye, measured as *how far the corrected rim sits above that eye's own
iris*: **0 of 19 eyes above 0.06** (it was every one of them), and **0 non-red pixels moved** in any
photograph. The one case that still shows red is a ring drawn *smaller* than the glow — that is
leftover glow outside the scope, which is the documented contract (§4: "what the ring covers is what
a click corrects") and not a rim the tool drew. **Open design question for the user:** whether a
correction should be allowed to follow a glow a bounded distance past the ring. It would fix that
last case, and seed-growing from a user-supplied region is classical, not a detector — but it
changes T1-a's "the tool corrects only the brushed region", which is a posture decision, not an
implementation one.

Tests: 27 cases (three more), including one that paints a real `MaskStroke` and asserts the scope
reaches what the ring shows, one that a soft scope edge cannot draw a ring of its own, and one that
the fade lands on the iris's own tone rather than on neutral.
