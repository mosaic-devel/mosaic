# Smart Recompose (object-preserving retargeting) — Scoping & Build Plan

**Status:** scoping doc. The second tier of Smart Resize (S16-f); no code lands until agreed.
**Parent:** `docs/smart-resize-research.md` — the benchmark analysis (§1), the crop tier (§4–§5, built),
and the design constraints (§3.8 crop, **§3.9 deformation/recomposition** — read §3.9 first).
**The case it serves (user's founding scenario, 2026-07-01):** important content at opposite edges
with a large aspect change — person left, tower right, expendable field between. A crop must
sacrifice one side; Smart Recompose removes the middle instead: subjects move **rigidly** closer,
the background is cropped, seams are inpainted. No deformation of anything that matters.
**Lineage:** Setlur, Takagi, Raskar, Gleicher & Gooch, *Automatic Image Retargeting* (MUM 2005;
10-year-impact award). We rebuild the 2005 architecture with 2020s components Mosaic already
owns: the S16-f importance map, the S37 He & Sun inpaint engine, selection tools, compositing.

> ⚠ Guardrails from §3.8/§3.9 apply throughout, and each is a hard constraint: ONE fused importance
> map; ONE suggestion; NO learned aesthetics; **NO automatic operator mixing** — the software may
> *offer* Recompose, only the user *invokes* it. That last one is deliberate, and it matches the UX
> philosophy anyway: the user has eyes.

---

## 1. UX (settled with user, 2026-07-01)

**Automatic by default; manual marking is refinement, not a hidden mode** (user's discoverability
concern about marquee-only marking — agreed and inverted):

1. **Keep-regions are detected automatically** when Smart Resize is ON: the salient blobs the crop
   search already extracts from the fused importance map (`smart_crop.cpp` `findBlobs` — bbox +
   mass). ~~plus Viola-Jones face boxes once F1 lands~~ — **F1 dropped 2026-07-02** (parent doc
   §4.2/§10; the `faceRects` hook stays dormant). The 2005 Setlur architecture describes exactly
   this automatic importance→regions step (§3.4/§3.9).
2. **The regions are visible** — outlined chips on the canvas overlay while Smart Resize is ON (the
   §6 "importance preview" idea, made load-bearing). Seeing "this is what it's keeping" is the
   discoverability: clicking a chip toggles/removes it; **Ctrl-drag adds one** (F-d). The scoped
   extensions — resize handles, "any selection tool adds a chip" — are **dropped 2026-07-02**:
   Ctrl-drag is enough for the purpose (user call). Region edits re-run the ONE suggestion.
   **Chip visual state is LIVE against the staged rect (user feedback 2026-07-02 — chips that
   ignored the crop box "felt confusing"):** chips re-fetch per frame (undo/redo/edits move them
   immediately) and are coloured by their fate under the *current* rect — **green + wash** = fully
   kept (the inpaint-preview green, not the crop box-blue, which clashed), **amber** = the rect
   edge slices it, **dim red** = marked but fully outside (this crop loses it), **dim grey** =
   user-toggled-off. The crop box stays boss — chips only report; Apply is never blocked.
3. **The Recompose offer:** when the marked regions cannot all fit any crop window at the chosen
   aspect (the §4.3 search reports it — protect rects unsatisfiable), the crop suggestion still
   stages (best effort), and a **"Recompose" button enables** in the options bar. The user clicks
   it; nothing switches automatically. When everything fits, the button stays greyed with a tooltip
   ("the crop already keeps everything marked").
4. **Preview + apply:** Recompose stages a full preview (subjects placed, background retargeted,
   seams inpainted — the inpaint runs async with progress, like the Inpaint brush). The user can
   nudge region placements before Apply. Apply lands ONE undo step. Esc returns to the crop
   suggestion.

Copy hand-holds like the crop toggle; no Unicode glyphs in labels ([[mosaic-ui-gotchas]]).

## 2. Pipeline (all FLTK-free core; `src/core/retarget/` grows)

1. **Regions** — from the fused map's blobs + faces + user edits: `KeepRegion { rect, mask?, mass }`.
2. **Cut** — lift each region from the flattened composite with a **generous feathered margin**
   (dilated blob support). Precise segmentation is deliberately NOT required for v1: the method's
   own applicability precondition (§3.9 — the background must survive surgery, i.e. be homogeneous-
   ish) is exactly the condition under which a loose cut + inpainted donut is invisible. *(Fork
   F-a: GrabCut-style bbox-seeded mask refinement as a v2 upgrade.)*
3. **Background retarget** — fill the cut holes (existing inpaint engine), then fit the healed
   background to the target aspect. **As built (2026-07): this is a plain CROP** — the single-map
   crop-window search locked to the exact target box (`recompose.cpp`, `scaleSteps = 0`), never a
   scale. The plan allowed a uniform-scale + crop mix (crop+scale of the background is Setlur's own
   2005 recipe, not a warp/crop auto-decision), but the shipped path crops only: strictly more
   conservative and simpler. (Uniform downscale still exists, but as a
   crop-tier option applied to the whole result, not to the background in isolation.)
4. **Rigid placement solver** — place regions onto the retargeted background: preserve relative
   ordering and approximate relative offsets (Setlur's "maintain relative spatial relationships"),
   minimize total displacement, no region overlaps, all regions inside frame, composition prior as
   a tiebreak. Small, deterministic optimization (regions are few; exhaustive/greedy + local
   refine is plenty). Subjects NEVER scale independently in v1 (rigid = zero deformation).
5. **Seam blend** — regions composite back with their feathered margins; residual seams go through
   the inpaint engine's Poisson blend stage. Deterministic given identical inputs.
6. **Output** — a new document state: canvas at target size, result as the baked composite.
   *(Fork F-b, output mode: (a) new document; (b) canvas resize + flattened result layer, original
   layers preserved beneath/hidden; (c) destructive bake like Delete-Cropped-Pixels. Recommend
   **(b)** — non-destructive spirit, one undo step, mirrors crop's canvas-resize semantics.)*

Perf: stages 1/3(crop)/4 are ms; the inpaint fills dominate (seconds) → async job with progress +
cancel, reusing the app_window InpaintJob pattern verbatim.

## 3. Design boundaries

The pipeline is deliberately **rigid**: regions are cut, moved and repasted whole, and the
background is cropped. Three things follow, and each is a hard constraint on this feature, not a
gap waiting to be closed:

- **No per-pixel offset-vector warping**, and no saliency-scaled deformation of any kind. Rigid
  recomposition computes no per-pixel offsets, and adding one would make it a different feature.
- **Our own detectors only** — the textbook blob detector of §1.1 plus morphological dilation. No
  learned saliency detector, no learned foreground extractor.
- **No automatic operator mixing.** The software may *offer* Recompose; only the user *invokes* it.

## 4. Testing (headless; user does visual)

- **Region extraction:** synthetic fixtures → expected blob set (already partially covered by the
  crop tests); face-fixture gating like §8 of the parent doc.
- **Placement solver:** synthetic cases with known answers — ordering preserved, no overlap,
  displacement minimal, degenerate cases (one region → centers/thirds; regions already fit → near-
  identity placement).
- **End-to-end person/tower fixture:** two textured blobs on a flat field, 2:1 → 1:1; assert both
  blobs' pixels survive byte-identical (rigidity!), their order preserved, output size correct.
- **Determinism:** identical inputs → identical output, twice.
- **UI contract:** Recompose button enables exactly when the crop search reports unsatisfiable
  protect rects; `--gui-frames` smoke stays clean.

## 5. Commit sequence (one logical change each; headless-verify each)

1. **docs** — this plan.
2. **Region model + auto-extraction** — `retarget/keep_regions.{hpp,cpp}` (blobs→regions; face
   hook stubbed until F1) + tests. Output-inert.
3. **Placement solver** — `retarget/recompose.{hpp,cpp}` (cut/placement/assembly orchestration,
   inpaint-engine injected) + tests. Output-inert.
4. **Crop-tier integration** — keep-region chips overlay + region editing + protect-rect feed into
   the existing crop search (this alone improves the crop tier). **User visual point.**
5. **Recompose UI** — the button + async job + preview + apply (fork F-b resolved first).
   **User visual point.**
6. **Credits** — extend the §7 attribution plan: Setlur et al. 2005 joins the "About Smart Resize"
   gallery + source headers. **DONE 2026-07-02** — `core/retarget/credits.{hpp,cpp}`
   (`RetargetInfo`), drawn by the generalized `SpecPanel` at the bottom of Settings → Tools →
   Crop; source headers already carried the lineage (recompose.hpp cites Setlur).

## 6. Forks — RESOLVED (2026-07-02; recommendations adopted, user AFK — revisit on request)

- **F-a mask depth:** v1 loose feathered cuts (as recommended); GrabCut refinement stays a v2
  fork (verify the EP member first).
- **F-b output mode:** **(b)** — canvas resized to target, the flattened result a new top
  layer, original layers preserved (hidden) beneath, ONE undo step. As built in commit 5.
- **F-c naming:** **"Recompose"** (the bar button; shown only while Smart Resize is ON).
- **F-d chip editing:** **Ctrl-drag adds** a User chip (no resize handles in v1); clicking a
  user chip removes it outright (no detector to fall back to), auto chips keep click-to-toggle.
  **CLOSED 2026-07-02:** the deeper editing ideas (resize handles, "any selection tool adds a
  chip") are dropped, not deferred — Ctrl-drag is enough for the purpose (user call). F1 faces
  dropped the same day (parent doc §4.2); chips are importance-blobs + user rects, final.

## 7. As built (commit 4 + 5 addenda)

- Commit 4 landed as `63c23dc` (chips: live-fate colours, click-toggle, off = mass-masked,
  Free = smart trim). Commit 5 adds: the offer (button enables exactly when the enabled chips'
  union bbox cannot fit the max-fit window at the ratio AND solvePlacements is non-empty —
  feasibility, never a promise of beauty), the async RecomposeJob (InpaintJob pattern; the S37
  engine heals through `retarget/inpaint_fill`), and the REVIEW: the assembled preview replaces
  the canvas display, placements drag as kept-green chips (each nudge re-assembles from the
  staged pipeline state — prepare/assemble split, the heal never re-runs), Enter/Apply lands the
  one CompositeCommand, Esc/Cancel (or any document change / tool switch / ask-relevant crop-
  option change) drops back to the crop suggestion.
- First visual pass (2026-07-02) surfaced two artifact classes; root causes were pinned by
  reproducing the user's exact scenario headless on the seam-carving beach photo (person +
  Broadway Tower → 1:1):
  - **The "castle ghost" was NOT chip under-coverage** — the padded cut covered the turret tops.
    It was the engine's deep-interior behaviour: filling a hole wider than any offset copied the
    REMOVED content back in (removing the tower rebuilt a slimmer tower), and a half-blended
    fragment of that echo survived into the recompose output. Fixed by copy-chain resolution
    (docs/inpainting-research.md §3.7.4); the discriminative red-block echo test pins it.
  - Misaligned edges through healed regions → the seam objective gained the published
    colours-&-gradients term + He & Sun's own E1 boundary anchoring (§3.7.3).
  - Chips made honest anyway: bounded GEODESIC support dilation (a protrusion sits near the
    strict mass; a cloud bank connects at support level but extends far — bbox unions cannot
    tell them apart), and the mass floor lowered (with a cell-count speckle guard) so the
    founding scenario's PERSON earns a chip (0.0029 of total mass on the beach photo). Verified
    end-to-end: person + tower chipped, both placed rigidly, no ghost, no echo.
  - Second user pass ("pasted-in square", strip through clouds/ground, one-sided horizon) →
    **step 5 WIRED (2026-07-02)**: `blendPieceBand` re-solves each pad band's colours in the
    gradient domain (Dirichlet at the hard snug core + surrounding background; guidance =
    feather-weighted mix of piece and destination gradients; fixed-sweep red-black GS —
    Pérez-2003 mixed cloning; the quadtree variant stays out). The live nudge
    stays feather-only (`assembleRecompose(staged, false)`), the worker's first assembly and
    Apply run blended. The background window also EXCLUDES the healed holes' importance
    (`SmartCropOptions::excludeRects`), so the frame prefers original pixels over synthesis.
    Verified by eye on the fixture: no pasted square, horizon connects both sides of the person,
    the cloud strip is gone.
  - Known remaining blemishes, both minor: a thin smear band where a healed hole touches the
    FRAME edge (no known ring to anchor there — lever: edge-aware label correction) and the
    coarse two-scale cut's stair-stepped horizon inside VERY large fills (§3.7.2's documented
    budget trade-off; more ICM sweeps converge without fixing it — the lever is a banded
    full-res re-cut).
