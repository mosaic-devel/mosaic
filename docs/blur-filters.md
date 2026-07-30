# S33 — Blur filters (the blur gallery + Depth of Field)

Status: BUILT 2026-07-17 (same-day design → build; awaiting the user visual pass). Companion to
`docs/adjustment-layers.md` (S32 owns the framework this rides on). Scope: the blur family of
non-destructive filter layers — the classic gallery plus a Depth-of-Field filter with on-canvas
handles. Everything below shipped: the seven kinds end-to-end, the CPU reference kernels
(`render/blur.{hpp,cpp}`), the reach/region/group-buffer machinery (§5, region == crop(full)
byte-exact test-pinned), the blur canvas gizmo — DoF band + Radial crosshair (§6), and the
Vulkan compute lane (§8:
gaussian/surface/lens/DoF in `render/blur_gpu.{hpp,cpp}` + `shaders/blur_*.comp`, parity-pinned
against the CPU lane, which stays the golden reference).

## 1. What ships

Seven new `AdjustmentKind`s, all created from **Filter ▸ Blur ▸ …**, all editable in the S32
pinned-popover editor, all masked/reordered/toggled like any adjustment layer:

| Kind | Menu label | Parameters |
| --- | --- | --- |
| `GaussianBlur` | Gaussian Blur… | radius (px) |
| `BoxBlur` | Box Blur… | radius (px) |
| `MotionBlur` | Motion Blur… | angle (deg), distance (px) |
| `RadialBlur` | Radial Blur… | mode (Spin/Zoom), amount, center x/y (px) |
| `SurfaceBlur` | Surface Blur… | radius (px), threshold (%) |
| `LensBlur` | Lens Blur… | radius (px), blades, curvature (%), rotation (deg), boost, boost threshold (%) |
| `DofBlur` | Depth of Field… | radius (px), band (px), feather (px), angle (deg), center x/y (px), bokeh (Soft/Iris) |

The user's DoF workflow this is built for: cut the subject out (or mask it out — filter layers
already take masks), drop the DoF layer above the background, and drag the canvas handles until
the focus band sits where the subject stands. The blur field does the depth story; the mask does
the subject isolation. The two compose because S31/S32 already made them compose.

Defaults are deliberately VISIBLE (radius 10-ish, not 0): a Filter-menu item that inserts a
do-nothing layer is the "broken promise" `adjustmentImplemented` exists to prevent. This is the
sanctioned deviation from the S32 identity-at-defaults convention, same class as Threshold /
Posterize ("inherently visible kinds"); the identity early-out keys off the *effective* params
(amount ≤ 0), not off default-equality.

## 2. Semantics: what a blur layer means

A blur adjustment blurs **the composited backdrop below it, in its scope** — exactly the S32
scoping (inside its group, or globally at root; clip-to-below restricts by the clip base's
alpha). Two deliberate departures from the color kinds:

- **Alpha diffuses.** A color adjustment recolors coverage; a blur *moves* it. The blurred result
  spreads and softens the backdrop's alpha along with its color. (Blurring RGB under a frozen
  alpha produces the classic hard-cut halo — wrong for every real use.)
- **Premultiplied math.** `ImageF` is straight-alpha; every kernel premultiplies, convolves,
  un-premultiplies. Straight-space convolution would bleed the RGB of fully-transparent pixels
  (undefined color) into visible ones.

The mask/opacity/clip blend then happens per pixel in premultiplied space:
`out = unpremul(lerp(premul(orig), premul(blurred), opacity × mask × clipCoverage))`. A masked-out
pixel is byte-identical to the unblurred backdrop — that invariant is test-pinned, it is what
makes the "mask the subject out" workflow honest.

All px-dimensioned parameters (radius, distance, band, feather) are in the adjustment's
**parent-space pixels** — document px at root, group-local px inside a group, so a blur scales
with its group's transform exactly like layer effects scale with theirs. Centers (`center_x/y`)
are parent-space px too (NOT normalized): stable under region cropping and preview scaling, and
what the canvas handles read/write directly. The Filter-menu insert seeds centers to the document
center (the schema default 0,0 never survives an insert).

## 3. Kernels (CPU reference lane)

All in `render/effect_primitives` (the LE-a/S33 shared home), pure CPU float, no FLTK/GPU.
Working space: premultiplied RGBA planes. Edge policy: clamp-to-edge (replicate) — blurring a
composite must not vignette at the canvas edge; region equivalence is unaffected (§5).

- **Gaussian** — the existing separable `gaussianBlur` per plane, σ = radius/2 (radius reads as
  the visually-apparent extent). Reach 3σ.
- **Box** — one exact separable box pass (running sum, O(1)/px), half-width = radius. The flat,
  "cheap" look is the point; it is NOT the Kovesi 3-pass (that approximates Gaussian).
- **Motion** — uniform line integral: N bilinear taps along ±distance/2 at `angle`,
  N = ceil(distance)+1. Symmetric about the pixel.
- **Radial / Spin** — per pixel, average taps along the circular arc about the center,
  arc = amount degrees (amount ≤ 100), tap count ∝ arc length at that radius.
- **Radial / Zoom** — per pixel, average taps along the segment from the pixel toward the center,
  length = amount% of the pixel's center distance.
- **Surface** — edge-preserving bilateral (Tomasi–Manduchi 1998 lineage; see §7 — the guided
  filter stays out per the standing C-B1 rule), separable two-pass approximation (Pham &
  van Vliet 2005): spatial σ = radius/2, range σ = threshold/100 on encoded luma, range weight
  against the pass's center pixel. Documented as an approximation; the exact windowed form is the
  GPU lane's job if the approximation ever visibly disagrees.
- **Lens** — aperture-mask gather in **linear light** (decode → gather → encode): kernel =
  the normalized coverage of an N-blade polygon (3–8) rotated by `rotation`, morphed toward a
  disc by `curvature` (SDF lerp), radius in px. Optional specular boost: a separate global
  pre-pass that gain-boosts pixels whose linear luma exceeds the **single lower**
  `boost_threshold` (constraints B-1/B-2 — never a threshold *pair*, never highlight-dependent
  gather weights), and never un-boosts — bright sources bloom into bokeh shapes the way
  clipped SDR content otherwise can't.
- **DoF** — a per-pixel blur field driving interpolation across a pyramid of pre-blurred levels
  (Potmesil–Chakravarty 1981 defocus model; pyramid-interpolation per Demers, GPU Gems 2004):
  levels at radii {0, r/4, r/2, 3r/4, r}, **each level blurred independently from the source**
  (constraint G-3) and the render strictly level-interpolation, never per-pixel variable-radius
  kernels (constraint G-2); per-pixel lerp between the two adjacent levels. Field:
  signed distance `d` from the focus line (center + angle), `f = clamp((|d| − band)/feather, 0, 1)`,
  per-pixel radius = `f × radius`. Bokeh choice picks the level kernel: Soft = Gaussian,
  Iris = the lens gather (fixed hexagon in v1). Inside the band the field is 0 ⇒ level-0 (the
  untouched backdrop) — the focus band is byte-identical to no blur, test-pinned; at the plateau
  the field is 1 ⇒ the top level exactly.

Live drags (`liveDrag` composites) may subsample gather taps (lens/DoF iris) — the settled
composite always runs the full kernel, S30-draft-style.

## 4. Compositor integration

`applyAdjustment` gains a spatial branch ahead of the per-pixel color loop:
blur kinds funnel to `applyBlurAdjustment(acc, adj, coverage, pre)` and return. The color-kind
loop is untouched — byte-identity for every existing document is trivially preserved (new enum
values only).

`pre` (the walk's placement transform, now threaded into the walk state) supplies:
- **scale** = `maxAxisScale(pre)` — multiplies every px parameter, so the ≤96px scope previews
  (`adjustmentScopeComposite` renders at preview resolution) blur proportionally instead of at
  full-canvas strength. Group-local walks pass identity (their buffers are 1:1 with local units —
  nested blurs were already resolution-correct).
- **placement** — maps parent-space centers/lines into buffer px, so a cropped region buffer or
  a scaled preview sees the same geometry the full composite does.

## 5. Non-local support: reach, regions, group buffers

A blur reads neighbors and spreads content — the two things the S60-a region path and the group
local-buffer sizing assume never happen. The model:

`blurAdjustmentReach(adj, contentDiag)` = the kernel's support radius in application-space px
(Gaussian/Surface 1.5×radius; Box/Lens radius; Motion distance/2; DoF radius×1.5; Spin/Zoom
bounded via the content diagonal — their displacement grows with distance from center, so the
bound is conservative by construction). `descendantAdjustmentReach(group, scale)` **sums**
visible descendants' reaches (stacked blurs compound: support radii add; max would under-grow),
scaled down the transform chain like `descendantEffectsReach`.

- **`groupLocalExtent`** grows BOTH rects by the group's descendant adjustment reach: the content
  rect (blurred content spills outward, like effects) AND the visible-window pullback (pixels
  near the window edge need source content beyond it — the *read* direction, which effects never
  had).
- **`compositeRegion`** expands the ROI by the root walk's adjustment reach, clamps to the
  canvas, composites, and crops back to the requested rect. Interior pixels then have full kernel
  support with content identical to the full composite; at the canvas edge both buffers end at
  the same physical pixel, so the edge policy fires identically. **Region == crop(full) stays
  byte-exact** — the invariant the whole scheme exists for, and the money test of the session.
  (A huge reach — zoom blur across a 4k canvas — degenerates the region path toward a full
  composite. Correctness first; the region path's win survives for the common small-radius case,
  and keystroke-sized regions under a blur layer stay cheap.)
- The drag-cache replay runs `applyAdjustment` on canvas-sized buffers — already correct, no
  change; heavy kernels ride the liveDrag subsampling.

## 6. UI

- **Editor**: the S32 schema-generated popover — no new dialog (user ruling 2026-07-17:
  popover-not-dialog for filter editors). Blur schemas are plain Scalar/Choice rows.
- **Filter ▸ Blur ▸ …** submenu above Adjustments, `cbNewAdjustment<Kind>` + the center-seeding
  hook.
- **Blur canvas gizmo** (the session's novel chrome): a single layer-bound, tool-INDEPENDENT
  gizmo on binding 11, keyed by kind:
  - **DoF band** (active layer is a `DofBlur`): the canvas draws the focus-band geometry — center
    line, two band-edge lines, two feather lines, a move knob on the center, a rotate knob at the
    line's end — in Mosaic's existing handle language (hairlines + square knobs, the text-box
    chrome family). Drags stream through `applyAdjustmentField` with per-handle coalesce ids
    (`dof:center/angle/band/feather`, one undo step per gesture); PUSH/DRAG/RELEASE claimed as a
    pair (the standing FLTK rule). A DoF layer carries exactly ONE band — no second band/pin/
    ellipse instance into the same field, ever (constraints G-1/G-2).
  - **Radial crosshair** (active layer is a `RadialBlur`): a single target mark at the blur
    center — a thin ring with a hairline cross, distinct from the DoF square knob so "center
    point" reads at a glance. One handle, move-only (`radial:center`); it drags Spin's pivot /
    Zoom's vanishing point. Shares the whole gizmo pipeline (`DofGizmoState::kind`, the same
    provider/edit/hit/drag/overlay path, the binding-11 SSBO's `dofKind` lane, the same draft-on-
    scrub settle).

  - **Ring** (active layer is an S35 `Vignette`): the centre knob plus a **radius** knob on a
    hairline arm — the square move knob and the round knob the DoF band already uses, so a vignette
    reads as the same family. The arm points along the adjustment's parent **+x**, mapped through
    the placement (so it rotates with a transformed group) rather than along the pointer: the knob
    then rests at one predictable compass point, while the drag reads `|cursor − centre|` in parent
    space so a pull in *any* direction sizes the radius. Two handles, no guide lines
    (`ring:center` / `ring:radius`). A radius that would put the knob inside the centre knob's own
    grab box is pushed out to the rotate knob's stand-off distance, or radius 0 could never be
    dragged back open. It rides the **band** shader path with the four band/feather guides parked
    outside the viewport — the GPU side needs no new kind, and the shader's per-guide distance skip
    drops them for free.

  Extending the pipeline to a new kind is: a `BlurGizmoKind` value, a branch in `dofScreenGeom`
  (screen placement), `hitDofHandle` (which handles exist), `dragDofHandle` (what a drag writes),
  `dofCursorState` (the hover cursor) and `syncDofOverlay` (what the shader is handed). The host
  side is two lambdas in `app_window.cpp` — the provider maps the layer's params bag into the
  state, the edit maps the state back through `applyAdjustmentFieldOn` with the coalesce id as the
  undo key. The edit clamps against **the layer's own** `adjustmentParamSchema`, read from the
  layer: the gizmo *shape* no longer identifies the kind now that Crosshair serves two of them.

  **Which kinds wear which:** `DofBlur` → Band; `RadialBlur` → Crosshair; `Vignette` → Ring;
  `Wave` → Crosshair **only in Ripple mode**. Plain Wave is a directional displacement that never
  reads `center_x`/`center_y`, so it shows no gizmo at all — a handle whose drag changes nothing
  visible is worse than no handle.

  For all of them, amounts (and Radial's mode, and Wave's) are scrubbed in the popover, NEVER on the
  canvas (constraint U-1) — the gizmo edits geometry only. The Vignette's *radius* is geometry in the
  same sense the DoF band's half-width is (an extent placed on the image), not an amount; its
  exposure, feather % and roundness stay in the popover.

## 7. Standing design constraints (the S33 B/G/U series)

**Every future blur/DoF change must respect these.** They are deliberate architectural limits, each
costing something in capability or speed, and they are hard constraints on this feature — not
oversights to be "improved" away.

- **B-1** — highlight boost takes a **single lower threshold** (or a thresholdless luminance
  curve). NEVER a user-input upper+lower "light range" pair.
- **B-2** — gather weights are uniform aperture coverage, never a function of a tap's
  highlight-ness / background-ness / clipping-recovery estimate. The boost is a separate global
  luminance-keyed pre-pass on pixel values.
- **B-3** — blur amount NEVER derives from estimated depth / disparity / dual-pixel /
  segmentation — user mask + user geometry only.
- **G-1** — **one blur-geometry instance per filter layer**; never combine ≥2 widget instances
  (pins/bands/ellipses) into one per-pixel field. Separate layers applying sequentially are fine —
  there is no shared combined mask.
- **G-2** — no pin-dropped field blur, not even ONE pin; and the render is **interpolation among
  independently pre-blurred levels — never "apply a blur kernel per pixel according to a per-pixel
  radius mask"**. This binds the GPU lane too: no exact per-pixel variable-radius gather "upgrade".
- **G-3** — pyramid levels are each built **from the source** (or a Burt–Adelson mip chain) —
  never a constant-resolution step-dilated kernel cascade applied to the previous level's result.
- **G-4** — no user-adjustable "bleed" control on masked blur; the boundary policy is fixed in
  code (which the house no-toggle rule wanted anyway).
- **U-1** — no pin-centered rotatable ring/dial scrubbing an amount on-image. Square knobs +
  hairline guides; amounts scrub in the popover.
- **M-1** — a curved "path blur" (user-drawn trajectory, per-pixel curved kernels) is out of scope
  and needs its own design pass before anyone starts one.

⚠ **The guided filter stays out** (standing C-B1), and so do learned bilateral-grid pipelines
(HDRnet-style); either would be its own design surface, not an upgrade to Surface blur.

**Technique lineage.** Classic blurs: Burt 1981, Crow SAT 1984, Heckbert 1986, Potmesil 1983,
Deriche 1987 / Young–van Vliet 1995. Bilateral: Aurich–Weule 1995, SUSAN 1997, Tomasi–Manduchi
1998, Durand–Dorsey 2002. Lens/bokeh: Potmesil–Chakravarty 1981, Buhler–Wexler 2002, McIntosh 2012.
Spatially-varying / DoF field: Potmesil 1981, Rokita 1993, Riguer/Scheuermann 2003-04, Demers,
*GPU Gems* (2004), Kraus–Strengert 2007.

## 8. Vulkan compute lane

The heavy gathers (lens, DoF iris, exact surface) get a compute lane behind an app-registered
override seam (the `setTextureRenderOverride` pattern from S55-h): the CPU reference stays the
golden/test lane, the GPU serves interactive sessions, per-call CPU fallback. RADV/ACO shader
traps (loop miscompiles, reciprocal-division) are pinned in the S55-h shader comments — same
care here. Parity test budget: cover-diff sliver + small mean delta, the extrude-GPU pattern.

## 9. Verification

Headless (the standing division of labor): schema well-formedness sweep picks up the new kinds;
identity-at-zero byte-exact per kind; analytic kernel signatures (delta → gaussian profile / flat
box / line / arc / radial streak / aperture polygon); surface preserves a step edge; DoF band
byte-identical + plateau == top level; alpha diffusion; mask gating byte-exact; **region == full
crop byte-exact** with root-level and group-nested blurs; preview-scale proportionality
(tolerance); golden pins per kind on a fixed composite; asan + gui-frames smoke. The user's
visual pass covers look/feel of every kernel, the DoF handles, and popover ergonomics.
