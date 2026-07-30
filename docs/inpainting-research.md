# Inpainting Engine — Research and Implementation Plan

**Status:** research note / design doc — this is the deliverable that **S37** (PLAN.md's inpainting entry, *"Research note `docs/inpainting-research.md` must be written first"*) is required to produce before any inpainting code lands.
**Scope:** the default built-in inpainting **backend**, based on He & Sun, *"Image Completion Approaches Using the Statistics of Similar Patches"* (IEEE TPAMI 2014; preliminary version ECCV 2012), plus the **pluggable-backend architecture** that lets a future Lua script (S40) register its own provider (e.g. an ML model). Target: a C++ engine inside the GPLv3 editor **Mosaic**.

> **Supersedes the preliminary assumptions in PLAN.md's inpainting entry.** PLAN.md was written before this research and assumed a *classical-only* built-in (Telea / Navier–Stokes). This note **revises that**: the foundational exemplar/patch-based method (Criminisi et al., 2004) is long published, so a higher-quality offset-statistics backend is the sensible default, with PDE methods kept as an optional fast fallback. Treat this document, not PLAN's bullet list, as the current source of truth for the inpainting approach.

> **Design from the paper, not from any product.** The engine is built from He & Sun's published method and its own parameters — `τ = max(w,h)/15`, region = 3× hole bbox, downsample ≤ 800×600, 8×8 statistic patches, σ=√2 histogram smoothing, 9×9 peak window, K=60. Where a later product's implementation recipe differs (reflection padding, downscale-to-~100×75, Laplacian-edge-mask blending), Mosaic deliberately does not follow it. §5 lists the standing constraints this places on the implementation; several of them are load-bearing and are pinned by tests.

---

## 1. What the technique actually is

The paper has two interchangeable solvers built on one shared idea:

1. **Offset statistics (shared front-end).** Match each known patch to its most-similar *other* known patch under a non-locality constraint `|s| > τ`, collect the relative offsets `s = (u,v)`, build a 2-D histogram, smooth it, and keep the `K ≈ 60` dominant peaks. These peaks describe how the image repeats itself.
2. **Graph-based solver (recommended by the authors).** Treat completion as a photomontage: stack `K` shifted copies of the image and pick, per pixel, which shifted copy to copy from, by minimizing an MRF energy (validity data term + seam-coherence smoothness term) with **multi-label graph cuts**, then **Poisson-blend** the seams. Two-scale for speed.
3. **Matching-based solver (alternative).** Optimize the coherence measure but restrict the per-patch offset to the same `K` dominant offsets, via EM + voting, multi-scale.

The components that matter for the implementation are therefore: **(a)** the nearest-neighbor field (NNF) used to gather offsets, **(b)** the offset-statistics-driven completion idea, **(c)** multi-label graph-cut optimization, and **(d)** Poisson blending. §3 records the design notes for each.

---

## 2. Where it fits in Mosaic — the pluggable-backend architecture

Mosaic does **not** hard-wire one inpainting algorithm. PLAN.md requires that *"all inpaint entry points call the same engine API; that API is designed with a registerable backend hook (default = the classical engine) so the S40 example can plug in."* This note formalizes that as a small backend interface.

### 2.1 Engine, backends, and entry points

```
mosaic::core::inpaint::InpaintEngine          // orchestrator + backend registry + dispatch
        │  used by every inpaint entry point:
        ├── Heal tool                 (S38 — heal/spot)
        ├── Inpaint brush             (S39)
        └── Edit → Fill… → Inpaint    (S39, uses current Selection)
        │
        ▼  dispatches to the selected backend (an IInpaintBackend)
   ┌──────────────────────────┬──────────────────────────┬───────────────────────────┐
   │ OffsetStatisticsBackend   │ PdeBackend (optional)     │ ScriptBackend (S40)       │
   │ He & Sun graph solver     │ Telea / Navier–Stokes     │ shim → Lua-registered      │
   │ ← THIS DOC, default       │ fast fallback / preview   │ provider (user's ML model) │
   └──────────────────────────┴──────────────────────────┴───────────────────────────┘
```

- **`IInpaintBackend`** is a pure-virtual interface (§7.3). A backend takes an `InpaintRequest` (image + hole mask + params + a progress/cancel callback) and returns the completed pixels. Backends are pure compute — no UI, no document mutation. The engine wraps the result in an undoable command (the existing `mosaic::core` command system).
- **`OffsetStatisticsBackend`** is the He & Sun graph-based solver described in §7.4–§7.6. It is the **default built-in** and the quality baseline.
- **`PdeBackend`** is an *optional* lightweight backend — fast, tiny, good for small scratches and live previews. ⚠ **INVARIANT — it implements exactly two formulations and no others: Telea Fast-Marching (2004) and the original Bertalmío Navier–Stokes (2000/2001).** No third scheme, and no modified/extended variant of those two, may be added to this backend. It is a fallback, not the headline path; the offset-statistics backend is the recommended default for object removal.
- **`ScriptBackend`** is the **scripting shim** (the second backend the architecture is built around): it does no inpainting itself. It marshals the `InpaintRequest` across the **S40 Lua inpaint-hook API** to a provider the user registered from a script, then marshals the pixels back. This is the *entire* mechanism by which ML inpainting (LaMa, MAT, a diffusion model, …) reaches Mosaic — **the user brings the dependency, weights, and runtime; Mosaic bundles none of it** (PLAN.md, the 2026-06-17 "ML dropped → S40 scripting" decision). The S40 deliverable ships one **example script** that registers such a provider as the reference path.

### 2.2 Why this shape

- **Single API, many fillers.** Heal/brush/menu all call `InpaintEngine::run(request)`; swapping algorithms is a backend selection, not a rewrite of three tools.
- **The ML story is just a backend.** Because the Lua hook is a backend like any other, "no built-in ML" costs us nothing architecturally — `ScriptBackend` is symmetric with the C++ ones.
- **Containment.** All of the offset-statistics machinery lives entirely inside `OffsetStatisticsBackend`; the engine, the interface, and the other backends are independent of it and of the design notes in §3.
- **Testability.** The engine is exercised through the headless op-runner (`mosaic --headless`, PLAN.md §3.15) with golden-image diffs in `tests/golden/` — same as every other filter. The interface lets tests register a trivial deterministic backend.

The backend selection (default `OffsetStatisticsBackend`, optional `PdeBackend`, any number of `ScriptBackend`s) is exposed as a per-call choice and a setting; built-ins register at startup, Lua providers register at script-load time.

---

## 3. Component design notes

Each subsection records how one component of the engine is built and why it is built that way.
Subsection numbering is historical and deliberately left stable — other documents and source
comments cite these numbers.

### 3.7.1 The COMBINATION (graph-cut source-selection + gradient-domain blend)

He & Sun's graph-based solver *is* a photomontage: multi-label graph-cut source selection plus a Poisson seam blend.

**Lineage.** That combination was published by **Agarwala et al., *Interactive Digital Photomontage* (SIGGRAPH 2004)**, itself building on **Kwatra et al., *Graphcut Textures* (SIGGRAPH 2003)**; He & Sun (ECCV 2012 / TPAMI 2014) is the completion method built on top of it. The boundary seam terms and the two-scale coarse-to-fine cut come straight from that line.

**What we build, and the exclusions that define it.** The engine implements the **published He & Sun method** — a translation-only multi-label graph cut (our own α-expansion) plus a **plain** Poisson seam blend. The following are deliberate standing exclusions, not gaps waiting to be filled: no quadtree-accelerated gradient-domain compositing (the blend stays a plain per-pixel solve), no object segmentation or symmetry-axis geometric matching, no video/temporal completion, no PatchMatch-style patch optimization, no geometric or photometric (rotation/scale/gain/bias) candidate search, and none of the reflection-pad → downscale-to-~100×75 → Laplacian-edge-mask recipe (§5).

### 3.7.2 Two-scale FINE seam refinement (coarse cut → banded fine re-solve)

The default backend's quality on **large holes** is limited by the two-scale graph cut: to keep the α-expansion bounded it solves the labeling on a heavily downsampled hole (factor ~14 for a ~1.1 M-pixel hole) and **nearest-upsamples** the labels. Seam placement is therefore decided on a *blurred* image and lands on coarse-block boundaries that slice through fine features (e.g. cloud edges in smooth sky); the plain Poisson blend cannot hide them because they are *structural* (two genuinely different textures meet), not level offsets — verified empirically (200 vs 1500 Poisson sweeps were pixel-identical; a full-resolution cut removed the seams). The fix is the **fine seam-refinement** step He & Sun describe and our §7.5 already specifies but the first implementation omitted: keep the fast coarse global cut, then **re-solve labels at full resolution in a narrow band along the seams**, each band pixel choosing among a *small candidate set* (its own upsampled offset + its seam-neighbours' offsets) by the same α-expansion + seam-coherence cost.

- **Lineage — what it decomposes into:** (a) **α-expansion** (Boykov, Veksler & Zabih, 2001); (b) **translation-only offset labels + seam-coherence (SSD) cost** — He & Sun (2012) / Graphcut-Textures (Kwatra et al. 2003); (c) **two-scale coarse-to-fine + band-localized re-solve** — **He & Sun, ECCV 2012, Sec. 4.2** (the exact "refine near seams with a restricted candidate set" step), and independently **Liu & Caselles, *Exemplar-Based Image Inpainting Using Multiscale Graph Cuts*, IEEE TIP 2013** (multiscale graph-cut inpainting over an offset map). Narrow-band restriction of an optimization is older still (level-set narrow-band methods, 1990s). It introduces **no element beyond what He & Sun already published**.
- **As specified:** the same shape as the coarse two-scale cut already shipped (§10.2) — the refinement is the *fine* half of the same published method. Implemented as a separate banded α-expansion pass over full-resolution pixels within ~l/2 of a seam — our own code, no vendored graph-cut source, translation offsets only.
- **As built (2026-07-02, second pass):** the first implementation realized this lever as per-pixel ICM only, which cannot move a seam FRONT (every single-pixel step of a front move raises the energy) — the stair-stepped horizon stayed. The pass now matches this section's specification literally: for each label pair meeting along a seam (most-contested first, capped), the A/B-labeled pixels within a band of that seam are re-solved as an **exact binary min-cut** over the same energy (submodular: pairwise(A,A)=pairwise(B,B)=0; out-of-band neighbours enter the unary as fixed labels), which is one α-expansion/αβ-swap move — the same machinery. Pixels whose candidate sources all land back in the hole are **excluded**: their pairwise samples would be the REMOVED content (§3.7.4), and an exact cut acting on meaningless energy flips whole regions (observed before the guard). The ICM pass stays as the polish for short seams and 3-label meets.
- **Worst-seam escalation (2026-07-02, with §3.7.7):** after the pairwise pass, the seam with
  the highest residual cost per pixel gets ONE wide-corridor re-solve (r ≈ 64, node-capped) with
  a SMALL multi-label set — the labels populous in the corridor, capped at 6 — via the generic
  α-expansion. This is where a §3.7.7 boundary-driven candidate the coarse cut missed can still
  enter at full resolution (a structural junction often needs a *third*, transitional sheet the
  two-label bands cannot introduce). The result is adopted only if it lowers the subproblem
  energy of the current labeling. Same machinery; bounded to one corridor.

### 3.7.3 Seam objective: "colours & gradients" + first-order (E1) boundary anchoring

User-reported artifact (2026-07-02, seen both in casual inpainting and in Smart Recompose):
**edges through a healed region come out misaligned** — a horizon steps where the fill meets the
photo or where two offset regions meet. Root cause in our implementation: the seam pairwise term
was **plain colour SSD** (blind to structure — two views of a slightly-shifted edge still "match"
in colour near the seam), and the hole↔known boundary term was **zeroth-order only** (the filled
pixel had to match its known neighbour, but the label's *view of the ring itself* was never
checked — which is actually **He & Sun's own E1 form**, so the fix partly *restores the paper's
energy*). Fix, applied identically to the coarse cut (§7.5) and the fine refinement (§3.7.2) so
the ICM pass keeps minimizing the same energy:

- **Pairwise:** + gradient-mismatch term — each label's view of the edge across a seam pair
  (difference of its two samples) must agree with the other's; weight 1.0 (equal footing, as
  published). Seams now route along matching structure instead of across misaligned edges.
- **Boundary data term:** + E1 anchoring — the label's view **of the known neighbour**
  (`n + offset`) must reproduce the known ring, pinning structure (a horizon's height) at the
  hole boundary.

**Lineage.** The "colours & gradients" seam objective is **Kwatra et al., *Graphcut Textures*
(SIGGRAPH 2003)** and **Agarwala et al., *Interactive Digital Photomontage* (SIGGRAPH 2004)** —
20+ years published. The E1 boundary term is **He & Sun (ECCV 2012) verbatim** — the method this
backend implements. The Poisson stage is unchanged by this work: it stays the plain red-black
Gauss–Seidel solve (§10.3). No new ingredient.

### 3.7.4 Deep-interior copy-chain resolution

User-visible failure it fixes (reproduced on the Wikipedia seam-carving beach photo): filling a
hole **wider than any candidate offset** used to copy the ORIGINAL (removed) content for deep-
interior pixels — the graph cut is forced to give them labels whose sources land back inside the
hole, and synthesis copied what was there. Removing the tower **rebuilt a slimmer tower** out of
its own pixels; through Smart Recompose the same mechanism left a half-blended turret-cap ghost
in the output (the artifact the user reported as "part of the castle visible after recompose").

Fix: after label correction, each in-hole copy chain `p → p+o(p) → …` is composed to its KNOWN
endpoint (`resolveEffectiveOffsets`, memoized walk, O(hole), deterministic; cycles/dead ends
resolve to no-source = neighbour-fill + Poisson composite fallback). Synthesis and the Poisson
guidance both consume the per-pixel effective offsets, so no removed pixel is ever copied.

**Lineage.** This is mechanical closure of the copy graph the He & Sun MRF already decided; the
label set and its optimization are untouched (K dominant offsets). Copying from already-filled
hole content is the mechanism of **Criminisi, Pérez & Toyama's exemplar inpainting (IEEE TIP
2004)** and of every onion-peel filler since. No new ingredient enters the engine.

**Addendum (2026-07-02, the "strips in the bottom right" fix):** a chain that FAILS (cycle /
runaway — typical where the hole meets the frame edge, where no known ring anchors the walk) no
longer falls straight to neighbour-fill diffusion, which smeared directional streaks along the
edge. The failed pixel now **inherits a resolved 4-neighbour's effective offset** when that
offset is valid from its own position (the neighbour's coherent sheet extends by one pixel);
deterministic sweeps, commit-after-sweep, diffusion stays the last resort. Same shape as the
chain composition itself: a deterministic post-process of the decided labeling — no offset is
optimized, no energy is evaluated, nothing enters any objective. Verified on the Broadway-tower
recompose repro: the bottom-band anisotropy (strips) fell from 5.0× to 1.07× (isotropic) and the
no-source population at the frame edge halved; unit-pinned by the hand-crafted cycle test in
`test_inpaint.cpp`.

### 3.7.5 Full-resolution offset refinement (de-quantization)

The offsets are gathered on the downsampled working region and rescaled, so every candidate is
quantized to a multiple of the region scale — ±scale/2 per axis of misalignment that NO seam
objective can repair (no candidate can express the true shift; on a 36-MP photo scale is ~7, and
a treeline carried across the hole lands several pixels off). Fix: each rescaled offset is
re-anchored at full resolution by **exhaustive local block matching** — every integer offset
within a small radius scored by mean patch SSD over a deterministic sample grid near the hole,
argmin wins (`refineOffsetsFullRes`, unit-tested; duplicates dedup).

**Lineage and invariant.** Exhaustive full-search block matching is 1980s video-coding technique
(motion estimation), evaluated independently and exactly per candidate: ⚠ **no propagation
between pixels** — a candidate offset is never seeded from a neighbour's result and never
perturbed-and-retested (the standing NNF constraint, §5). He & Sun's own offsets are
full-resolution — this step *removes a deviation* our working-region downsample introduced,
converging toward the published method.

### 3.7.6 Multigrid Poisson pre-solve + guidance hygiene

Fixed-sweep relaxation kills high-frequency error fast but low-frequency error slowly — on a
hole hundreds of pixels across, the offset sheets keep slightly wrong DC levels (the user's
"banded sky" / "clouds don't blend"), and no affordable sweep count fixes it. The blend now runs
**geometric multigrid** on the correction field c = u − composite (whose right-hand side is
nonzero essentially only along seams and needs no offset sampling at coarse levels): mask-aware
V-cycles with red-black Gauss–Seidel smoothing, half-sum restriction, and cell-centered bilinear
prolongation whose amplitude is set per level by an exact **line search** (α minimizing the fine
residual — a one-dimensional subspace correction, so a coarse level can never make the solution
worse; this replaces any hand-tuned inter-level scale, which cannot be a single constant for a
rhs that mixes line-concentrated seam sources with the coarse level's receding Dirichlet wall).
The projected SOR polish (with the [0,1] box constraint) then enforces the exact offset-based
guidance in a few dozen sweeps. Small holes (< 20k px) keep the plain SOR path unchanged.

Two guidance corrections landed with it (the multigrid solves the system it is given *exactly*,
which surfaced both):

- **A guidance view must never read removed content.** The source-space gradient samples now
  read the **virtual completed source** — known pixels from the image, hole pixels from the
  COMPOSITE (their chain-resolved fill, § 3.7.4), fixed for the whole solve (deterministic,
  race-free). Previously a view whose sample fell back inside the hole read the original
  (removed) pixels; under-converged solves hid the poison, the multigrid faithfully
  reconstructed it (a smooth dark ghost of the removed object on the Skagen church photo).
- The `views == 0` fallback (both views out of bounds) stays the composite's own gradient, as
  before.
- **Crisp-seam rule (2026-07-02):** when a seam edge's two source views STRUCTURALLY disagree
  (max-channel difference of the two view gradients above a fixed threshold — one sheet sees
  flat sky, the other a canopy edge), averaging them invents a gradient neither sheet contains
  and the solve diffuses the conflict into a smudge (the grey wedge at a structural junction).
  Past the threshold the guidance keeps the COMPOSITE's own gradient, so the transition stays a
  crisp content edge. Below it, the antisymmetric average blends DC mismatches exactly as
  before (agreeing views — e.g. the banded sky — are unaffected; both endpoints take the same
  branch, so the field stays conservative). Still plain selective mixed guidance in the Pérez
  2003 sense.

**Lineage and invariant.** Geometric multigrid is classical numerical analysis (Fedorenko 1964;
Brandt 1977; textbook material for decades), the same category as the SOR acceleration (§10.3;
Young/Frankel, 1950s), and the line search is textbook steepest-descent (Cauchy, 19th century).
⚠ It is deliberately **NOT** an adaptive quadtree meshing of the domain (cells growing away from
the seams to cut one solve's DOF count) — here every multigrid level is a FULL regular grid over
the same domain, a solver schedule rather than an adaptive discretization, and the fine-level
system it converges to is the plain per-pixel Poisson solve of §3.7.1. That is a hard constraint
on this stage, not an optimisation nobody got round to. No new ingredient.

### 3.7.7 Boundary-driven candidate offsets

The Skagen treeline junction exposed a *candidate* limitation, not a seam one: the K dominant
offsets are chosen by GLOBAL frequency voting, so the locally-precious offset that would map the
photo's one tall→short treeline transition segment into the junction never makes the ballot.
Fix: augment the K frequency-voted offsets with a few TARGETED candidates — for fully-known
patches adjacent to the hole boundary (the ring), find each one's best-matching other known
patch (the same exact KD-tree k-NN, patches as queries), vote the resulting offsets across ring
queries, and append the top few distinct winners to the label set. The graph cut and the banded
re-cuts then have the *option* of the continuation content; the existing energy accepts it only
where it genuinely costs less. Candidate generation only — the label optimization, seam
objective, chain resolution and blend are untouched.

**Lineage.** The mechanism is that of **Criminisi, Pérez & Toyama** (IEEE TIP 2004) — matching boundary
patches against the known image to decide what continues into the hole. Ours differs only in what
the match feeds: label candidates for one global cut, not a greedy per-patch fill order, which is
the He & Sun / Liu & Caselles (IEEE TIP 2013) framing. ⚠ The ring queries stay **independent,
exact k-NN lookups — no propagation, no random search** (§5).

### 3.7.8 Outpaint (canvas-expansion) tuning — structure penalty, shift ladder, donor bands (2026-07-02)

Filling OUTWARD (the S16-f crop expansion's ring, or a manual expand-then-heal) is a different
problem from interior holes: on the Broadway-tower repro the engine duplicated the tower
wholesale into the strip (a verbatim copy has no interior seams and the ring's boundary terms
cannot see its interior) and cloned the person; the Resynthesizer floated grass shelves into the
sky. All tuning below is gated on `isOutpaintHole` (`core/inpaint/outpaint.{hpp,cpp}`: ≥25% of
the image-frame perimeter is hole) — **interior heals never build any of it and stay
byte-identical**.

- **Structure penalty (offset statistics).** Per-(pixel,label) data cost `w·S(source)`, S =
  structure-tensor anisotropy (λ1−λ2 of the box-blurred tensor, r=6) of the full-res image,
  normalised at the 98th percentile, ZEROED within (blur+dilation) reach of the hole boundary
  (the image-against-empty-ring step edge is the strongest "structure" in the frame and its halo
  taxed exactly the adjacent columns an outpaint should copy), then max-dilated (r≈min(W,H)/50)
  so an object's interior carries its edges' cost. Applied in lockstep in the coarse cut (map
  max-pooled per coarse block — recomputing on the downsampled image loses the fine structure),
  the banded re-cuts, the corridor escalation and the ICM polish. w = 0.25
  (`MOSAIC_INPAINT_OUTPAINT_W` overrides; 0 disables).
- **Shift-candidate ladder (offset statistics).** Per mostly-hole frame side, geometric rungs of
  axis-aligned INWARD shifts from (strip depth + patch) up to the region extent — the far strip
  needs candidates whose sources lie BEYOND any foreground object, and the frequency vote rarely
  supplies them (it finds object symmetries — the duplicating labels). Full-hole scan lines are
  excluded from the depth estimate (a ring's corner-crossing lines are hole end-to-end).
- **Banded donor draws (Resynthesizer).** Each ring pixel's RANDOM donor draws restrict to a band
  at its own depth along the edge it extends (axis from per-pixel directional distance to known);
  coherence candidates stay free (they are local continuation by construction).
  `MOSAIC_INPAINT_OUTPAINT_BAND` overrides; ≤0 disables.

**Results (Broadway 1428×968 → 1920×1080):** offset-stats — tower duplication and person clone
GONE; a faint fragment persists in the far top-right corner (corner pixels have the fewest valid
sources; the clean far-sky alternatives carry cloud-anisotropy cost — next levers: damp
low-energy anisotropy, corner-specific diagonal candidates). Resynthesizer — no imported
structures; sky shelves REDUCED but not eliminated (coherence chains can still walk content off
the frame; next lever: depth-banding the coherence proposals too). Swan-photo expansion stays
seamless. Both levers are textbook energy-model tuning atop the existing engines (§3.7.1, §3.10):
saliency/structure-weighted MRF data terms and restricted sampling windows, no new mechanism, and
nothing content-aware added to the CROP feature itself — the crop-axis guardrails in
`docs/smart-resize-research.md` are untouched, and the engine tunes itself on hole GEOMETRY,
never on how the hole was made.

**⚠ Addendum (2026-07-11) — the shipped shift ladder was DEAD, plus both "next levers" built.**

- **The wiring bug.** The commit that shipped the ladder (`7bcd5c2`) appended its rungs to the
  region-unit candidate list *after* the loop that rescales that list to full resolution — so no
  ladder candidate ever reached the solver, and every shipped outpaint since ran on the
  structure penalty alone. The paragraph above therefore stopped describing the shipped binary:
  re-running the Broadway repro on the pre-fix build reproduces a wholesale tower duplicate in
  the right strip, battlement ghosts along the top strip, and a tower-base copy + smear blobs in
  the bottom-right (the results above were presumably measured from a working tree whose ladder
  sat before the rescale). Fixed by appending the candidates before the rescale; the ladder is
  extracted to `outpaintShiftCandidates()` (offset_statistics, unit-tested), and an end-to-end
  regression pins the wiring itself: a horizontal-gradient image (all self-similarity vertical)
  whose right strip only a leftward rung can legally source must come back as verbatim ladder
  copies, not boundary diffusion — the test fails on the pre-fix engine.
- **Corner diagonal rungs** (the "corner-specific diagonal candidates" lever). Where two
  adjacent frame sides are both strips, the corner block's axis escapes each land in the *other*
  strip, so no axis rung is ever valid there. Up to 6 diagonal rungs per such corner pair the
  two sides' ladders (both magnitudes clear their own strip depth simultaneously, ×1.55
  geometric). Candidate generation only — no new mechanism.
- **Low-energy anisotropy damping** (the "damp low-energy anisotropy" lever). λ1−λ2 is
  energy-weighted but the normalisation is frame-relative, so faint-but-coherent cloud edges
  still tax the clean far sky after dilation. The normalised map is now multiplied by
  trace/(trace + f·T98) — trace = λ1+λ2, T98 its robust frame scale, f = 0.05
  (`MOSAIC_INPAINT_OUTPAINT_DAMP`; ≤0 restores the old map bit-for-bit). Textbook
  structure-tensor coherence weighting (Förstner 1986 / Weickert 1990s); still the same
  structure-weighted data term.
- **Weight retune 0.25 → 0.15.** The old weight was tuned with the ladder dead — the penalty
  had to fight duplication alone. With real candidates on the ballot it over-punished legitimate
  donors: grass hugging the horizon carries the horizon edge's dilated anisotropy halo, so the
  right strip's below-horizon band went to cheap sky and the hill fell off a cliff at the old
  frame edge. Swept 2026-07-11: 0.10 and 0.15 both hold the horizon with zero duplication;
  0.15 ships (more anti-duplication margin).
- **Results (same Broadway repro, new engine):** tower duplicate, top-strip battlement ghosts,
  bottom-right tower-base copy, far-corner fragment — all gone; the horizon continues level to
  the frame edge and grass fills the bottom-right corner. Residuals (recorded): a faint vertical
  tone smear at the old right frame edge (a guidance-mixing seam, worst through the grass) and a
  soft dark cloud wisp near the old top-right corner. Swan expansion stays seamless and drops a
  swan-butt fragment the pre-fix engine left at the far-left edge (near-white/near-black outlier
  counts 18/30 → 0). Interior heals verified **byte-identical** to the pre-fix engine end to end
  (bench `cmp` on the pexels-building heal) — the gate contract holds. ~9.8 s at 1920×1080
  (691k hole px, release), same cost class as before despite the larger label set.

**Second addendum (2026-07-11, same day) — the recorded residuals run to ground.**

> **⚠ STATUS (2026-07-11, later the same day): BOTH LEVERS BELOW ARE NOW OFF BY DEFAULT.** The
> user's testing on real photos found regressions the Broadway/swan repros did not show: the
> **Skagen church** expansion runs much longer and fills the strips with **sky only**, and on
> **Broadway tower** the fill favors sky near the ground, reading as **trenches** along the
> image sides. Both levers therefore moved behind opt-in Backend Settings flags
> (`Params::outpaintBoundaryCrisp`, `Params::outpaintDeviationTax` — "Expansion: crisp edge
> blending / avoid atypical donors (experimental)", advanced group, default off); the default
> engine reproduces the first-addendum behaviour byte-for-byte (bench-`cmp` verified on the
> Broadway repro). The env vars below still override either way (the sweep tools). **Root
> cause deliberately not investigated yet** — the analysis and results below describe the
> levers as built and remain the record to start from when they are picked back up.

Both residuals (and the wisp's twin at the old TOP edge) turned out to live on the **hole↔known
boundary**: the pre-blend composite dumps (`MOSAIC_INPAINT_DEBUG_PPM` now also writes
`<prefix>composite.ppm`) show crisp faint-cloud chunks and DC-mismatched sheets that the blend
half-erases into smudges. Two root causes, one lever each; both **outpaint-gated** — interior
heals re-verified byte-identical (bench `cmp`, levers force-enabled-off vs default, interior
circle heal).

- **Boundary-crisp guidance rule** (`poissonSeamBlend`, default threshold **0.08**,
  `MOSAIC_INPAINT_OUTPAINT_BCRISP` overrides; ≤ 0 restores the historical guidance bit-for-bit).
  At a hole↔known edge the known pixel has no label, so the guidance took the sheet's view
  unopposed — and the correction the solve must then diffuse inward is exactly the sheet's E1
  ring residual, which texture (grass) and sheet-only content (a pale cloud) never zero. Along a
  canvas-expansion ring the boundary is a long straight line, so those residuals' harmonic
  extension IS the recorded vertical tone smear, and it is what half-erased the boundary-hugging
  cloud chunk into a grey ghost. The rule is the **§3.7.6 crisp-seam rule applied to one more
  edge class**: when the sheet's view of a boundary edge disagrees with the composite's own
  gradient there (their difference is precisely the ring residual) beyond the threshold, the
  guidance keeps the composite's gradient and the boundary stays a content edge. Small
  mismatches — smooth-sky DC offsets — stay below the threshold and keep bridging exactly as
  before. **Mechanism:** unchanged from §3.7.6 — the same two candidate gradients already in the
  function, the same per-edge threshold selection, still plain *selective mixed guidance* in the
  Pérez 2003 sense. Pinned end-to-end in `test_inpaint.cpp` (a brightened ring
  patch no donor has must stay a content edge, not smear into the strip).
- **Local-deviation donor tax** (`buildStructurePenalty` gains `devFrac`, default **1.5**,
  `MOSAIC_INPAINT_OUTPAINT_DEV` overrides; ≤ 0 restores the anisotropy-only map bit-for-bit).
  The structure tensor is blind to smooth non-uniform content — a faint cloud's interior has
  λ1−λ2 ≈ 0 — so cloud-carrying donor sheets stayed cheap in strip sky (the §3.7.8 damping had
  even un-taxed their halos), and their half-blended remains were the horizon smudge and corner
  wisp. New term: **band-pass luma energy** |box_6(L) − box_R(L)| (R ≈ min(W,H)/60), i.e. a
  difference of two box means — flat sky scores 0, fine texture (grass) scores 0 (killed by the
  first blur), cloud-scale blobs light up whole. Robustly normalised at its own 98th percentile,
  same near-boundary zero guard, then **max-combined AFTER the anisotropy dilation**: no
  already-taxed pixel changes (the horizon halo keeps its exact cost, so the w=0.15 tuning
  cannot regress), the term only raises the floor where the tensor is blind; no dilation
  of its own (a band-pass response covers a blob's interior by construction). Weight swept
  0.5/1.0/1.5/2.0 — the response is **non-monotonic** (1.0 leaves verbatim sky blocks on the
  horizon line, 1.5 clears them, 2.0 over-taxes legitimate donors and the blocks return), so 1.5
  is pinned by sweep. **Lineage:** the same structure/saliency-weighted MRF data-term family as
  the anisotropy penalty (§3.7.8 base note) — unsharp-mask / difference-of-Gaussians band-pass is
  textbook signal processing (1930s photographic unsharp masking; Marr–Hildreth 1980), and
  saliency/priority-weighted completion goes back to Criminisi's own gradient-based priority term
  (2004). No new mechanism, no new search, nothing content-aware added outside the already-gated
  outpaint path.
  Unit-pinned via the now-exported `buildStructurePenalty` (a tensor-blind smooth blob must gain
  cost, fine texture must not, max-combine may only raise, the boundary guard covers the term).
- **Results (Broadway repro):** the horizon cloud-ghost smudge, the on-horizon verbatim sky
  blocks, the vertical tone smear at the old right frame edge, and the top-right wisp are gone
  or reduced to faint haze (smudge-zone luma σ 15.3→10.3‰; horizon runs smooth and level end to
  end). Swan expansion stays seamless (0/0 outliers). ~15 s at 1920×1080 release — same cost
  class (the two extra box blurs cost ~40 ms). Remaining (recorded, minor): a faint pale haze
  band right above the horizon in the strip and a very faint wisp remnant at the old top edge —
  both now crisp content rather than smears, and both plausibly readable as natural sky haze.
- **Test-suite note:** the pre-existing "Poisson blend interpolates a gradient" unit test used a
  12×1 synthetic whose 4-cell hole is 33 % of a 1-px-tall image's frame perimeter — it tripped
  the outpaint gate and inherited the boundary-crisp rule, which correctly keeps that flat copy
  crisp. The test pins INTERIOR blend semantics, so it was widened to 20×1 (20 % < the 25 %
  gate) with an explicit `REQUIRE_FALSE(isOutpaintHole(...))`; behaviour it verifies is
  unchanged.

### 3.10 Resynthesizer backend (Harrison 2001/2005)

The optional `ResynthBackend` (user request: a second engine choice; the He & Sun backend stays
the default) implements **Paul Harrison's best-fit per-pixel texture re-synthesis**: WSCG 2001
("A Non-hierarchical Procedure for Re-synthesis of Complex Textures" — constraint-ordered
per-pixel growth, robust weighted match metric) plus the practical refinements of his PhD thesis
("Image Texture Tools", Monash 2005) and the long-lived GIMP plugin: neighbour-coherence
candidates, a Cauchy-based robust metric, multi-pass refinement. Clean-room C++ from the
publications — no plugin code is ported (the plugin is GPL and licence-compatible anyway; the
point is engine-only, no GIMP adapters). Deterministic by fixed-seed sampling.

- **Lineage.** Published 2001 (paper) and 2005 (thesis), with the implementation publicly
  distributed since the early 2000s. It belongs to the Garber 1981 / Efros–Leung 1999 /
  Wei–Levoy 2000 / Ashikhmin 2001 generation of neighbourhood-matching texture synthesis, and the
  neighbour-coherence candidate search is Harrison's and Ashikhmin's (both 2001).
- ⚠ **INVARIANT — the random candidates stay uniform draws over the donor pool, never
  perturbations of an existing mapping.** Perturb-around-current-best sampling must never be
  added to this backend (the guardrail is repeated in the header). The absence of a
  perturbation phase is deliberate and is a hard constraint on this file, not an oversight.
- **Not a super-resolution pipeline.** No resolution change and no frequency decomposition
  (split a low-res texture into bands, resynthesise the high-frequency signal, interpolate,
  recombine) belongs in this backend — it re-synthesises at the image's own resolution, full
  stop.

## 5. Standing implementation constraints

Baked into the implementation in §7. These are **hard constraints on the engine, not preferences.**
Each one is deliberate, each costs something, and several are pinned by tests. The obvious
"improvement" to several of them is exactly the thing not to do.

1. **NNF without PatchMatch.** Use an exact/approximate KD-tree over patch descriptors
   (`nanoflann`, header-only, BSD) or brute force on the downsampled region. We need only
   *statistics*, and the region is downsampled (≤ 800×600, 8×8 patches), so a KD-tree is plenty
   fast. ⚠ **No propagation + random-search loop.**
2. **Translation only.** ⚠ Never search rotated, reflected, scaled, or photometrically adjusted
   (gain/bias) candidate patches. Translation-only is a load-bearing property of this engine and
   the single constraint most often proposed for relaxation — the answer is no.
3. **Graph cuts from scratch.** α-expansion + a from-scratch max-flow (~250–400 lines).
   ⚠ Do not vendor **GCO**'s source: it is distributed for research use only. Note that
   Kolmogorov's `maxflow-v3.0x` is a *different* library and ships under the **GPLv3** — the two
   are routinely confused because gco-v3.0 bundles a copy of the BK sources under GCO's terms.
   **Provenance decides the licence, not the filename.**
4. **Blending.** Solve the Poisson seam ourselves over a small sparse solver, or fall back to
   multiband/feather. (See §6 on whether to vendor Eigen vs. hand-roll.)
5. **He & Sun's own parameters.** `τ = max(w,h)/15`, region = 3× hole bbox, downsample ≤ 800×600,
   8×8 statistic patches, σ=√2 histogram smoothing, 9×9 peak window, K=60. Do not substitute
   another implementation's recipe — in particular not reflection padding, not a
   downscale-to-~100×75, and not a Laplacian-edge-mask blend.
6. **The matching-based solver is not built at all** (hardened 2026-07-11 from "optional / off by
   default" — §7.7). The graph-based solver is the authors' own recommendation and the one this
   engine ships.
7. **`PdeBackend`, if shipped, implements plain Telea / Navier–Stokes** — those two formulations
   and no other.

---

## 6. Dependency choices (license-checked, Mosaic-specific)

Mosaic is **GPLv3** (`LICENSE` = verbatim GPLv3; see `docs/third-party-licenses.md` and PLAN §6–§7). Compatibility rule: permissive (BSD/MIT/Apache-2.0), LGPL, MPL2, GPLv3, GPLv2-or-later, AGPLv3 are all OK to incorporate. Avoid only **GPLv2-*only*** (one-way incompatible with GPLv3) and **non-free** ("research/non-commercial use only").

**Reuse what Mosaic already has — do not reinvent or add stb_image.** Mosaic already provides the pixel containers and I/O:
- `mosaic::common::Image` (8-bit RGBA) and `mosaic::common::ImageF` (float RGBA, straight alpha) in `src/common/image.hpp` — use `ImageF` as the engine's working representation (it is the compositor's float buffer). **Do not introduce a private `Image`/`Mask` type.**
- `mosaic::core::Selection` in `src/core/selection.hpp` — the document's 8-bit coverage mask (`coverage > 0` = the hole to fill). This **is** the inpaint mask; the engine takes a `Selection` (or its `data()` span), not a bespoke mask.
- Image load/save lives in `src/io/`; the engine never touches files.

New dependencies the engine *may* introduce (both vendored header-only, both GPLv3-compatible; add per the `third_party/` + `docs/third-party-licenses.md` + PLAN §6 discipline — drop in the upstream `LICENSE`, add a row to the table):

| Need | Recommended | License | Notes |
|---|---|---|---|
| KD-tree ANN (offset NNF) | **nanoflann** (vendored, header-only) | BSD-2 | New vendored dep; or hand-roll a small KD-tree to add nothing |
| Sparse solve (Poisson seam) | **hand-rolled** small CG/multigrid over the band, **or** Eigen `SimplicialLDLT` | n/a / MPL2 | Mosaic currently vendors **no** linear-algebra lib. Per PLAN §6.3's "implement ourselves to avoid the dep" stance, prefer a ~150-line hand-rolled solver over the seam band; vendor Eigen only if the Poisson solve becomes a bottleneck |
| Graph cut | **write our own** α-expansion + max-flow | n/a (our code) | Avoids the GCO/maxflow research license; ~250–400 lines of our own code |

**Off-limits regardless of Mosaic's license:** `gco-v3.0` and Kolmogorov `maxflow` (non-free, research/non-commercial only) — and any **GPLv2-only** code (one-way incompatible with GPLv3). Ordinary GPL (v3 or v2-or-later), LGPL, MPL2, and permissive Poisson/NNF code *is* fine to incorporate; writing our own is preferred for cleanliness, not license necessity.

The optional `OpenCV` dep (PLAN §6.3, Apache-2.0) already ships Telea/NS — if `PdeBackend` is built and OpenCV is present, it can borrow those; otherwise implement plain FMM/NS ourselves — those two formulations and no other (§5).

---

## 7. Proposed C++ implementation

Designed so Claude Code can implement it directly, matching Mosaic conventions: **snake_case filenames**, namespace **`mosaic::core::inpaint`**, `mosaic::common` image types, doctest unit tests, golden-image diffs via the headless op-runner.

### 7.1 Directory layout (real Mosaic tree)

```
src/core/inpaint/                      // inpainting is a core document op (lives by command/document/selection)
  inpaint_engine.hpp / .cpp            // InpaintEngine: backend registry + dispatch + entry-point API
  inpaint_backend.hpp                  // IInpaintBackend + InpaintRequest / InpaintResult / Params
  backends/
    offset_stats_backend.hpp / .cpp    // DEFAULT built-in: He & Sun graph-based solver (orchestrates the stages below)
    offset_statistics.hpp / .cpp       // stage 1: NNF + histogram + peaks
    nnf.hpp / .cpp                     // KD-tree nearest-neighbor field (NO PatchMatch)
    graph_completion.hpp / .cpp        // stage 2a: MRF build + alpha-expansion + 2-scale
    graph_cut.hpp / .cpp               // generic multi-label alpha-expansion + from-scratch max-flow
    poisson_blend.hpp / .cpp           // seam hiding
    match_completion.hpp / .cpp        // stage 2b (optional, EM + voting; off by default)
    pde_backend.hpp / .cpp             // optional: Telea/NS fast fallback (plain formulation only)
    script_backend.hpp / .cpp          // S40: shim → Lua-registered provider via the inpaint-hook API
docs/
  inpainting-research.md               // this file
tests/
  test_inpaint_offset_statistics.cpp   // doctest; flat in tests/, registered in tests/CMakeLists.txt
  test_inpaint_graph_cut.cpp
  test_inpaint_engine.cpp              // backend registry / dispatch / a dummy backend
tests/golden/inpaint_*.ppm             // golden references for the headless op-runner diffs
```

Add `src/core/inpaint/` to `src/core/CMakeLists.txt` (or a nested `CMakeLists.txt`). The `ScriptBackend` is compiled but inert until S40 wires the Lua host.

### 7.2 Citation header (every engine source file)

```cpp
// Inpainting engine for Mosaic.
// Algorithm: K. He and J. Sun, "Image Completion Approaches Using the
// Statistics of Similar Patches," IEEE TPAMI 36(12), 2014 (prelim. ECCV 2012).
// Clean-room implementation from the publications; no PatchMatch propagation+
// random-search loop is used (see docs/inpainting-research.md §5).
```

### 7.3 Backend interface + engine (`inpaint_backend.hpp`, `inpaint_engine.hpp`)

```cpp
namespace mosaic::core::inpaint {

struct Params {
    int    patchSize        = 8;       // w for offset statistics
    int    matchPatchSize   = 9;       // w' for the optional matching solver
    int    K                = 60;      // number of dominant offsets
    double tauFraction      = 1.0/15;  // tau = tauFraction * max(regionW, regionH)
    int    maxRegionW       = 800;     // downsample target for the working region
    int    maxRegionH       = 600;
    double histSmoothSigma  = 1.41421356; // sqrt(2)
    int    peakWindow       = 9;       // local-max window for peak picking
    bool   useGraphSolver   = true;    // false => matching-based (off by default)
    bool   poissonBlend     = true;
};

// Pure-compute request/result. No document mutation, no UI, no file I/O.
struct InpaintRequest {
    const common::ImageF& image;       // working float RGBA (straight alpha)
    const core::Selection& holeMask;   // coverage > 0 == pixels to fill
    Params params{};
};
struct InpaintResult {
    common::ImageF image;              // completed pixels (full image; only the hole changed)
    bool ok = true;
    std::string detail;                // diagnostics / why it bailed
};

using ProgressFn = std::function<bool(float)>; // 0..1; return false to cancel

// One filler. Implementations: OffsetStatisticsBackend (default), PdeBackend, ScriptBackend.
class IInpaintBackend {
public:
    virtual ~IInpaintBackend() = default;
    virtual std::string id()   const = 0;   // stable id, e.g. "offset-stats", "pde", "lua:<name>"
    virtual std::string name() const = 0;   // human label
    virtual InpaintResult run(const InpaintRequest&, const ProgressFn&) = 0;
};

// Registry + dispatch. Entry points (Heal/brush/menu) call run(); the engine wraps the
// result in an undoable command via the existing mosaic::core command system.
class InpaintEngine {
public:
    void registerBackend(std::shared_ptr<IInpaintBackend>);     // built-ins at startup; Lua at script-load
    [[nodiscard]] std::vector<std::string> backendIds() const;
    void setActiveBackend(std::string id);                      // default "offset-stats"
    InpaintResult run(const InpaintRequest&, const ProgressFn& = {}) const;
};

} // namespace mosaic::core::inpaint
```

The **`ScriptBackend`** (S40) implements `IInpaintBackend` by marshaling `request.image` / `request.holeMask` into Lua tables/userdata across the sandboxed inpaint-hook API, invoking the registered Lua provider, and marshaling the returned pixels back into an `InpaintResult`. The user's script owns the model, weights, and any heavy runtime; Mosaic ships none. The S40 example script registers exactly one such provider as the reference path.

### 7.4 Stage 1 — Offset statistics (`offset_statistics`, `nnf`)

```
function computeDominantOffsets(image, holeMask, p) -> vector<Offset>:
    region   = boundingBox(holeMask coverage>0) expanded to 3x in each dim, clamped to image
    scale    = max(1, ceil(max(region.w/p.maxRegionW, region.h/p.maxRegionH)))
    work     = downsample(image within region, scale)
    tau      = p.tauFraction * max(work.w, work.h)

    # descriptors: each KNOWN patch's top-left pixel -> 3*w*w vector (RGB)
    # (top-left coordinate representation, per the paper, avoids the patch-size
    #  restriction of the original NNF library)
    desc     = collectKnownPatchDescriptors(work, p.patchSize)

    # NNF: for each known patch, nearest OTHER known patch with |offset|>tau.
    # KD-tree (nanoflann) over desc; query k nearest, take the first candidate
    # whose spatial offset magnitude exceeds tau. Deliberately NOT PatchMatch;
    # statistics are insensitive to NNF approximation (per He & Sun).
    offsets  = []
    for each known patch P at x:
        for cand in kdtree.knn(desc[P], k = 8):
            s = pos(cand) - x
            if |s| > tau: offsets.push(s); break

    H        = histogram2D(offsets)                      # bins indexed by (u,v)
    Hs       = gaussianSmooth(H, p.histSmoothSigma)
    peaks    = localMaxima(Hs, window = p.peakWindow)    # 9x9
    topK     = take K highest peaks(peaks, p.K)
    return [ offset * scale for offset in topK ]         # rescale to full resolution
```

`Offset = {int u, v;}`; the histogram is a dense 2-D array sized to the offset range `[-W..W] x [-H..H]`.

### 7.5 Stage 2a — Graph-based completion (`graph_completion`, `graph_cut`)

The recommended path. Two scales for speed (paper, Sec. 4.2).

```
function graphComplete(image, holeMask, offsets, p) -> ImageF:
    # ---- coarse scale ----
    (cImg, cMask, l) = downsampleToWorking(image, holeMask, p)   # scale factor l
    cOffsets = [o / l for o in offsets]
    labels   = [ (0,0) ]            # s0 valid ONLY on the 1px-expanded hole boundary
    labels  += cOffsets             # s1..sK

    # MRF energy:
    #   data:  E_d(x,a) = 0 if (x + s_a) is known, else +inf
    #          (s0 allowed only when x is on the expanded hole boundary)
    #   smooth (4-connected neighbors x,x'):
    #          E_s(a,b) = ||I(x+s_a) - I(x+s_b)||^2 + ||I(x'+s_a) - I(x'+s_b)||^2
    L = alphaExpansion(labelSet=labels, dataCost=Ed, smoothCost=Es,
                       neighbors=fourConnected(holeRegion))

    # ---- upsample + seam refinement (paper, Sec. 4.2) ----
    Lf = nearestUpsample(L, l)                    # multiply chosen offsets by l
    refine pixels within l/2 of seams, each allowed 5 candidate offsets:
        { s, (u +/- l/2, v), (u, v +/- l/2) }     # for its upsampled s=(u,v)
    out = copyByLabels(image, Lf)                 # out(x) = image(x + s_{L(x)})

    if p.poissonBlend: out = poissonBlend(out, seamMask(Lf))
    return out
```

`graph_cut` is a **generic multi-label α-expansion**:
- `alphaExpansion(labels, dataCost, smoothCost, neighbors)`: standard expansion-move loop. For each label α, build a 2-terminal graph over the variable pixels, solve a min-cut, accept if energy decreases, iterate to convergence.
- Min-cut: a **from-scratch max-flow** (BK-style augmenting paths or Dinic's). ~250–400 lines; our own code → no library-license issue.
- Seam costs aren't guaranteed submodular for arbitrary label pairs; use the standard truncation fallback (or note that with these costs the energy is metric enough for expansion in practice, matching the paper).

### 7.6 Seam blending (`poisson_blend`)

```
function poissonBlend(img, seamMask) -> ImageF:
    # Per channel, solve Laplacian(f) = div(guidance) inside the seam band,
    # Dirichlet boundary = surrounding known pixels.
    # Build the sparse 5-point Laplacian over band pixels; solve A f = b with a
    # small hand-rolled CG/multigrid (or Eigen SimplicialLDLT if vendored). 3 channels.
    # Fallback if disabled / solver fails: feather or multiband blend.
```

### 7.7 Stage 2b — Matching-based (`match_completion`) — **NOT BUILT, do not build**

⚠ Never implemented, and hardened 2026-07-11 from "optional / off by default" to **excluded**.
The matching-based solver is a multi-scale pyramid whose per-patch source choices propagate from
a low-resolution solve up to the full-resolution fill, driven by Wexler-style EM + voting. It has
**zero quality upside** — He & Sun themselves recommend the graph solver (§1), which is what this
engine ships — so there is nothing to weigh against the cost of carrying a second solver.

⚠ Note the shape of what is excluded, because a future optimisation can drift into it without
anyone deciding to build "the matching solver": **do not carry per-patch SOURCE LOCATIONS across
a scale boundary.** A set of candidate translations may cross; a spatially indexed map of where
each output position draws from may not. The original sketch is kept below for the record only;
the `Params::useGraphSolver` flag was removed with this verdict.

Original sketch (historical): multi-scale pyramid (factor 2, coarsest ≥100×100), init from the
graph-based result; E-step: best of the `K` offsets by SSD per unknown patch (exhaustive — not
PatchMatch); M-step: reconstruct colors by averaging overlapping patch votes; iterate
(paper: 20 coarsest / 5 mid / 2 finest).

### 7.8 Notes for Claude Code (do / don't)

- **DO** implement the NNF as KD-tree/brute-force ANN. **DON'T** implement a PatchMatch-style propagate-then-random-search loop.
- **DON'T** search over rotation, scale, reflection, gain, or bias. Translation offsets only.
- **DON'T** vendor or copy GCO or Kolmogorov maxflow (research-only). GPL (v3 / v2-or-later), LGPL, MPL2, and permissive Poisson/NNF code *is* fine for GPLv3 Mosaic; only **GPLv2-only** is off-limits. We write graph-cut + blend ourselves for cleanliness.
- **DON'T** substitute another implementation's recipe (reflection-pad → downscale to ~100×75 → Laplacian edge-mask blend) — see §5. Use He & Sun's parameters and a generic blend.
- **DON'T** add any other PDE scheme to `PdeBackend` — plain Telea / Navier–Stokes only, and no modified variant of either.
- **DO** reuse `mosaic::common::ImageF` and `mosaic::core::Selection`; don't invent private image/mask types or pull in stb_image.
- **DO** put the citation header (§7.2) in every engine file.

---

## 8. Can I bundle the paper in the repo?

**Short answer: no — don't commit the PDF. Cite it and link a freely available copy instead.** (Same discipline Mosaic already applies to third-party assets in `docs/third-party-licenses.md`.)

Why:
- The TPAMI version is **© IEEE**; its footer permits personal use but says *"republication/redistribution requires IEEE permission."* IEEE policy (Author Center; PSPB Operations Manual §8.1.9) is explicit: **third parties may not post IEEE-copyrighted material without a license from IEEE.** The right to post is granted to *authors and their employers* (and only the **accepted manuscript**) — not transferable to Mosaic, which is a third party. Committing the PDF is exactly the "redistribution to servers" that requires permission.
- The ECCV 2012 version is **© Springer** (LNCS) with equivalent restrictions.

What to do instead:
1. **Cite it** with full bibliographic detail + DOI (below).
2. **Link** the authors' freely hosted copies (linking ≠ redistributing the file):
   - TPAMI version (author copy): `https://people.csail.mit.edu/kaiming/publications/pami14completion.pdf`
   - ECCV 2012 version (author copy): `https://people.csail.mit.edu/kaiming/publications/eccv12completion.pdf`

### Citation (for `docs/` and source headers)

```
K. He and J. Sun, "Image Completion Approaches Using the Statistics of Similar
Patches," IEEE Transactions on Pattern Analysis and Machine Intelligence,
vol. 36, no. 12, pp. 2423–2435, Dec. 2014. doi:10.1109/TPAMI.2014.2330611.

Preliminary version: K. He and J. Sun, "Statistics of Patch Offsets for Image
Completion," in Proc. European Conf. on Computer Vision (ECCV), 2012, pp. 16–29.
doi:10.1007/978-3-642-33709-3_2.
```

---

## 9. References

Method and lineage:
- **K. He and J. Sun**, *Statistics of Patch Offsets for Image Completion*, ECCV 2012; extended as
  *Image Completion Approaches Using the Statistics of Similar Patches*, IEEE TPAMI 36(12), 2014.
  The method this engine implements (Sec. 4.2 of the ECCV paper is the two-scale coarse-to-fine
  cut and fine seam refinement).
- **A. Criminisi, P. Perez, K. Toyama**, *Region Filling and Object Removal by Exemplar-Based
  Image Inpainting*, IEEE TIP 13(9), 2004 - the foundational exemplar/patch-based method.
- **Y. Liu and V. Caselles**, *Exemplar-Based Image Inpainting Using Multiscale Graph Cuts*,
  IEEE TIP 22(5), 2013 - multiscale graph-cut completion.
- **P. Perez, M. Gangnet, A. Blake**, *Poisson Image Editing*, SIGGRAPH 2003 - the seam blend.
- **Y. Boykov, O. Veksler, R. Zabih**, *Fast Approximate Energy Minimization via Graph Cuts*,
  IEEE TPAMI 23(11), 2001 - alpha-expansion.
- **A. Goldberg and R. Tarjan**, *A New Approach to the Maximum-Flow Problem*, JACM 35(4), 1988;
  **B. Cherkassky and A. Goldberg**, *On Implementing Push-Relabel with Global Relabeling and Gap
  Heuristics*, Algorithmica 19, 1997 - the max-flow solver and its two heuristics.
- **P. Harrison**, *A Non-Hierarchical Procedure for Re-Synthesis of Complex Textures*, WSCG 2001
  - the lineage of the optional Resynthesizer backend (§3.10).

Third-party source licensing:
- **GCO** (multi-label graph-cuts library, UWO/Veksler) - **research use only. Do not vendor.**
- **Kolmogorov `maxflow-v3.0x`** - **GPLv3** (its own `graph.h` says so; the tarball carries
  GPL.TXT). This is compatible with Mosaic and is *not* the research-only library - a long-standing
  confusion in this project's own comments, corrected 2026-07-12. gco-v3.0 bundles a copy of the
  same BK sources under **GCO's** terms; provenance decides the licence, not the filename. Mosaic
  ships its own solver regardless, for engineering reasons.

Publisher policy (why the papers are not vendored - see §8):
- IEEE Author Center, *Post-Publication Policies*; IEEE PSPB Operations Manual §8.1.9 - third-party
  posting of IEEE-copyrighted material requires IEEE permission.

---

## 10. Performance optimization pass (2026-06-20) — what changed, and why

The first correct implementation was unusably slow: on a 4912×7360 photo with a ~1.4 M-pixel hole it ran for **tens of minutes to hours** (and froze the UI thread, since the engine runs synchronously on it). Profiling with per-stage timers (`InpaintResult::timings`; set `MOSAIC_INPAINT_TIMING=1` for a stderr breakdown) found three cost drivers, each fixed below. **Every change is either a parallelization of identical math, a deterministic data-reduction, or one of He & Sun's own published speed techniques — none introduces a new algorithm, touches PatchMatch, or copies research-licensed code.** Measured end-to-end on the test photo (8-core machine), full resolution: **~154 s → ~16 s**; and, crucially, all three stages are now **bounded independently of image/hole size** (a knob each), so the cost no longer explodes with the photo.

| Stage | Before | After | Lever | Lineage / notes |
|---|---|---|---|---|
| Offset-statistics NNF | O(n²); ~14 s at 154×230, **hours** at full res | ~6 s, ~flat in image size | deterministic decimation to `nnfMaxPatches` + multithreaded queries | exact independent KD-tree NN; no propagation, no random search |
| Graph-cut (α-expansion) | 46 s at 614×920; **130 s** at full res | ~4 s, bounded | proper Dinic blocking flow; precomputed costs; **node-cap two-scale** (`graphCutMaxNodes`); cycle cap | Dinic (1970) + α-expansion (Boykov–Veksler–Zabih 2001) + He & Sun two-scale |
| Poisson seam blend | unbounded; ~17 s at full res | ~5 s, bounded | **red-black SOR** (parallel) + iteration cap (`poissonIterations`) | plain Poisson (Pérez et al. 2003) + SOR (Young/Frankel, 1950s); **not** a quadtree variant |

### 10.1 Offset-statistics NNF — deterministic decimation + multithreading
The exact k-nearest-neighbour over `3·8·8 = 192`-dimensional patch descriptors cannot prune in such high dimensions, so the KD-tree degraded to ~O(n²) in the patch count n, and n grows with the working-region size — the dominant cost. Fix: (a) **uniformly decimate the patch set to `nnfMaxPatches` (default 16 000) by a fixed raster stride** — deterministic, no RNG, so the result is reproducible — which bounds the match cost so it no longer grows with image size; (b) **run the independent queries across hardware threads** (read-only tree; per-thread histograms summed — order-independent, so identical for any thread count).
- ⚠ **Invariant.** The matching stays an **independent, exact per-patch nearest-neighbour lookup**: no patch's offset is ever seeded from a neighbour's, and no candidate offset is ever perturbed-and-retested (§5). The decimation is a generic Monte-Carlo-style data reduction made **deterministic on purpose** — it is not an algorithm and not a search strategy. He & Sun explicitly note the offset statistics are insensitive to the NNF (their Secs. 3.1 and 5.1). Multithreading is pure parallelization.

### 10.2 Graph-cut — efficient Dinic, precomputed costs, node-bounded two-scale
Three sub-fixes: (a) the max-flow was re-entered from the source after *every* augmenting path, re-walking the shared prefix each time — rewritten as a proper **single-pass Dinic blocking flow** (textbook 1970 algorithm); (b) the per-(node,label) **data costs and shifted samples are precomputed once, in parallel**, instead of being recomputed through `std::function` on every label sweep (the move loop is now a header template so the cost callables inline); (c) the decisive one — **the auto two-scale now downsamples the labeling problem to keep the coarse node count ≤ `graphCutMaxNodes` (default 6 000)**, so one max-flow-per-(label×cycle) stays bounded regardless of hole size, and the α-expansion sweep count is capped (`graphCutMaxCycles`). Coarse labels are nearest-upsampled and the seams are Poisson-blended.
- **Lineage.** Still Dinic's max-flow, implemented from scratch (the project's deliberate choice over BK, which also keeps us clear of the research-only `maxflow` library); α-expansion is Boykov–Veksler–Zabih (2001) and the submodular construction is Kolmogorov–Zabih. Two-scale coarse-to-fine graph cut is straight from the He & Sun / Graphcut-Textures academic line (§3.7.1). Precompute, templating, threading, and the caps are implementation efficiency with no algorithmic novelty. No GCO/maxflow source is vendored.

### 10.3 Poisson seam blend — red-black SOR
The blend solved a full-hole Gauss-Seidel whose global low-frequency mode converges very slowly, so it ran its whole iteration cap over every hole pixel (unbounded in hole area). Fix: **red-black (checkerboard) ordering** — a hole pixel's 4-neighbours are all the opposite colour, so each colour updates with no cross-dependency and runs across threads — plus **SOR over-relaxation** (`poissonOmega ≈ 1.9`) to accelerate convergence, capped at `poissonIterations` (default 200). Measured: at 200 SOR sweeps the blend is *better*-converged than the old 800 plain Gauss-Seidel sweeps were, at ~⅕ the cost.
- **Lineage and invariant.** This is the **basic Poisson/gradient-domain reconstruction** (Pérez et al., 2003); red-black ordering and SOR (Young/Frankel, 1950s) are ancient numerical-analysis methods, and changing the relaxation **order/step** does not change the linear system being solved. ⚠ It deliberately remains the plain full-region solve — **not** a quadtree-along-seams acceleration (§3.7.6).

### 10.4 Notes / follow-ups
- **Determinism preserved.** No step uses randomness; outputs are reproducible (the decimation stride, the red-black sweep, and the threaded reductions are all order-independent). This keeps the golden-image verification valid.
- **Tunables** are all on `Params` (`nnfMaxPatches`, `graphCutMaxNodes`/`graphCutMaxScale`/`graphCutMaxCycles`, `poissonIterations`/`poissonOmega`) — raise them to trade speed for quality.
- **Still on the UI thread.** These changes cut the compute cost; they do **not** move the work off the UI thread. The synchronous freeze is a separate concern (run the engine on a worker with the existing `ProgressFn` cancel hook) — the planned S39-b async/progress work.
- **`tools/inpaint_bench`** (off by default, `-DMOSAIC_BUILD_INPAINT_BENCH=ON`) drives the engine on image+mask paths given on the command line, so no large test images are vendored. It is the harness these numbers came from.

### 10.5 Second speed pass (2026-06-21) — exact NNF early-out + graph-cut convergence early-out

A follow-up cut the engine ~2× again (building x4 **8.67 s → 4.27 s**) with two changes, both **implementation efficiency in the §10 sense** — no new method, no algorithmic novelty, same category as §10's parallelization / precompute / caps:

- **NNF partial-distance early-out.** The KD-tree's squared-distance accumulation bails the instant it reaches the current k-th-best, so a far candidate is rejected after a handful of the 3·8·8 = 192 descriptor dims instead of all of them. This is **partial-distance / partial-distortion search (Bei & Gray, 1985)** — a textbook vector-quantization acceleration, ubiquitously re-implemented. It is **EXACT**: the result is byte-identical to the full-distance k-NN (the skipped dims only ever confirm a candidate is already ≥ the bound). offset-stats 4226 → 1664 ms.
- **Graph-cut cycle-convergence early-out.** α-expansion stops once a full label sweep improves the energy by < 2 % of the first sweep's gain — it converges by cycle 2, so the 4-cycle cap was sweeping K max-flows for negligible change. This is just **fewer iterations of the same α-expansion**; "stop when converged" is generic numerical practice, not a method. graph-cut 3862 → 2014 ms; output differs from the full-cap result by 0.02 % of pixels (meanAbsDiff 0.005).
- **Considered and NOT shipped (quality):** a box-downsampled (192 → 48-dim) match descriptor — standard multi-resolution patch matching / image pyramids, and consistent with He & Sun's stated NNF-insensitivity — gave a further ~3× NNF speedup but altered textured-image output (≈13 % of pixels on a rock wall, still clean), so it was left out on quality grounds. Free to revisit if wanted.

**Net:** both shipped changes are exact/near-exact accelerations of existing components; neither introduces a method.

### 10.6 Third speed pass (2026-07-02) — stencil-form blend + single-cut seam bands; the GPU question

Ran together with the §3.7.5/§3.7.6 quality pass; measured on the user's 36-MP Skagen fill and
the Broadway-tower recompose heal. Both changes are exact (outputs byte-identical up to single-
LSB float rounding, verified numerically):

- **Guidance hoisting (the stencil refactor).** The Poisson guidance never depends on the
  evolving solution, so the offset-sampling half of every sweep was hoisted into a one-pass
  per-pixel constant (`Rv`, `nn`); every SOR/multigrid sweep is now a plain 5-point stencil.
  Blend stage: 7.5 s → **1.6 s** on the 36-MP fill. Pure implementation efficiency (loop-
  invariant code motion); no method change.
- **Single min-cut per seam band.** The §3.7.2 banded re-cut subproblem is binary and
  submodular, so one Kolmogorov–Zabih min-cut yields its exact global optimum; the α-expansion
  loop (4–6 max-flows + energy re-evaluations per pair) was replaced accordingly. Seam-refine:
  9.1 s → **6.1 s**. Same machinery, fewer invocations of it.
- **Net wall-clock:** 36-MP fill ~21 s → **~13 s**; tower recompose heal ~16 s → **~12 s** —
  on top of the same session's quality gains.

**The GPU question (user ask, 2026-07-02) — assessed, deliberately deferred.** After the
stencil refactor the profile is max-flow dominated (coarse cut + seam bands ≈ 9 s of 13; Dinic
augmenting paths are inherently sequential — a GPU max-flow is a research project, not a port).
The two GPU-viable kernels are now cleanly isolated and would buy roughly 3.5 s more: the blend
is in exact compute-shader form (mask + rhs + 5-point stencil; ~1.8 s → ~0.3 s) and the NNF
could run as brute-force exact k-NN on GPU (~3 s → ~0.3 s; still propagation-free, so the §5
NNF constraint holds unchanged). Verdict: not worth adding a Vulkan-compute dependency to core
for ~25 % of wall-clock this session; revisit as its own session if the remaining latency
matters after the quality pass lands. (Any future GPU path must keep the CPU solver as the
deterministic test/reference path.)

---

### TL;DR

- **Architecture:** one `InpaintEngine` + an `IInpaintBackend` interface. The He & Sun **graph-based solver is the default built-in backend** (`OffsetStatisticsBackend`); an optional `PdeBackend` (plain Telea/NS) is a fast fallback; a **`ScriptBackend` is the shim** that lets a future S40 Lua script register an ML provider — Mosaic bundles no model. All entry points (Heal S38, Inpaint brush S39, Edit→Fill→Inpaint S39) call the same engine. Uses Mosaic's existing `mosaic::common::ImageF` + `mosaic::core::Selection`; snake_case files in `src/core/inpaint/`; namespace `mosaic::core::inpaint`.
- **Build the graph solver from the paper**, translation-only, with our own KD-tree NNF (no propagation, no random search), our own α-expansion + max-flow, and our own/permissive Poisson blend. Writing all three ourselves also keeps the research-only graph-cut/maxflow code licences out of the tree.
- **The standing engine constraints are in §5** — translation-only candidates, an exact NNF, He & Sun's own parameters, plain Telea/NS in `PdeBackend`, no vendored research-licensed solver. They are hard constraints, several of them pinned by tests.
- **Don't bundle the paper.** It's IEEE/Springer-copyrighted and Mosaic is a third party. Cite it + link the authors' free copies.
