# Eyedropper (colour picker) + loupe — design (S24)

> Owns the **Eyedropper tool** (sample a colour into the fg/bg swatch) and its **loupe** (a circular
> GPU magnifier that follows the cursor). Companion to `docs/research-selection.md` (the Magic Wand's
> source-image resolution, which the eyedropper *reuses*) and `docs/compositor.md` / the present-pass
> shader (`shaders/canvas_present.comp`, where the loupe is drawn).

---

## 1. What shipped vs. the pre-existing scaffolding

**Already present before S24** (do not reinvent):
- `ToolId::Eyedropper` registered with shortcut **I**, `ToolGroup::Sample`, `ToolSlot::Eyedropper`
  (`src/ui/tool.cpp`), and its **options** already defined: a **Sample** choice
  (Point / 3×3 / 5×5 / 11×11) and a **Source** choice (Active Layer / All Layers).
- The status bar's live **cursor colour readout** (S13-b): a chip + hex + RGBA sampled from
  `m_lastComposite` on hover (`onCanvasCursor`). Unchanged; the loupe is additive.
- The Magic Wand's document-space **source resolvers** — `activeLayerDocImage` (active-layer pixels,
  inverse-sampled through the layer transform) and `wandMergedSource` (the merged composite). The
  eyedropper reuses these verbatim, so the two tools' *Source* choices can never drift.
- The GPU present-pass overlay machinery: per-family SSBOs (lasso/reticle at binding 4, text at 5,
  keep-chips at 8, tip-SDF at 9) + the crisp-pixel **texel-centre snap** and the **transparency
  checker**, both already in `canvas_present.comp` (S19-c).

**The gap S24 filled:** the tool was **inert** — no FL_PUSH/FL_DRAG path, no sampling into the swatch,
and no loupe at all. S24 added:
1. **Sampling** — `MainWindow::eyedropperSample(docPt)` reads the options, resolves the source image
   (active layer / composite) the wand's way, and averages the pixel(s) with the new pure
   `core::sampleColor` (`src/core/color_sample.{hpp,cpp}`, unit-tested in `tests/test_color_sample.cpp`).
2. **Commit** — the canvas `EyedropperHost.commit` writes the resolved colour into the foreground
   swatch; **Alt or the right button** targets the background. A press samples; a **drag keeps
   sampling live** (Photoshop convention). No undo step — a swatch change is app state, not a document
   command.
3. **The loupe** — a circular GPU magnifier (§3), centred on the cursor, drawn by the present pass.

---

## 2. Sampling arithmetic (`core::sampleColor`)

Pure and headless-testable; the UI owns *which* image, this owns the maths.
- **Sample sizes** → radii: Point 0, 3×3 → 1, 5×5 → 2, 11×11 → 5 (window side `2r+1`).
- **Average** each channel **R, G, B, A independently** in straight (non-premultiplied) 8-bit space —
  the value the eye reads on screen, which is the colour-picker convention (Photoshop/Krita average
  the *displayed* pixel values, not a premultiplied blend). Round to nearest.
- **Edge clipping:** a window hanging off the image averages only the in-bounds pixels rather than
  padding with zeros (which would darken an edge sample toward transparent black).
- **Nothing to pick** → `std::nullopt`: the pointer is off the pixels, the image is empty, or (under
  *Active Layer*) the active layer is non-raster and owns no pixels of its own. The tool then silently
  does nothing and the loupe drops its swatch + readout.

## 3. The loupe (GPU-magnified)

The spec: *a circular popup magnifier following the cursor showing the nearest pixels with a grid
(GPU-magnified)*, **distinct from** the S19-c View pixel grid (which fades in on the whole canvas at
~1000% zoom). The loupe has its **own** grid at its **own** magnification, always on while sampling.

**Rendered entirely in `canvas_present.comp`** — no CPU blit — riding the same present pass as the
brush reticle and lasso line. It is its own SSBO channel, **binding 10** (`WindowRenderer::setLoupe`),
a fixed 48-byte struct (no flexible array): `{active, radius, mag, readout, center, sampleDoc,
sampleRgba}`. The shader `loupe()` function, drawn **last** (it is the cursor for its tool, and never
coexists with the reticle):
- **Nearest-neighbour magnify:** each screen pixel inside the disk maps to `sampleDoc + d/mag` in doc
  space; it samples `uDoc` (the on-screen composite) at the **texel centre** — the identical
  crisp-pixel snap the main present path uses — so the loupe shows discrete, single-colour texels.
  Transparency shows the same screen-space **checker** as the canvas.
- **Own pixel grid:** a faint hairline on texel boundaries (distance to the nearest edge × `mag`),
  luminance-keyed so it reads on light and dark content.
- **Centre cell:** `sampleDoc = floor(cursorDoc) + 0.5`, so the sampled texel is the central square of
  side `mag`, outlined **box-blue** over the chrome family's tight always-on casing (`chromeShadow`)
  — the exact picked pixel is unmistakable amongst its neighbours, even on box-blue content.
- **Colour-comparison ring** (2026-07-16 redesign, the pro-picker convention): the frame is an
  annulus (~11.5% of the radius wide) whose **top arc shows the resolved sample** — what a click
  right now would pick, honouring *Source* + the sample-size average — and whose **bottom arc shows
  the swatch that pick would replace** (fg, or bg under Alt/right; the `previous` host callback).
  New above, current below: the comparison is read in place, without glancing at the toolbar. Off
  the pixels the top arc goes neutral graphite instead of lying. The arcs sit in **neutral
  near-black hairlines** (inner edge, outer edge, and the two horizontal seams) — a coloured frame
  would tint the very colours being judged, so box-blue is reserved for the centre cell — and the
  silhouette wears `chromeShadow` so the disk reads on any content. This replaced the old in-disk
  swatch band (which ate content area) and the old box-blue outer ring.
- A **hex/RGB readout tile** anchored by the ring (reusing the shared overlay text tile,
  `uOverlayText`, since the dial/crop/Move HUDs are never active alongside the eyedropper). Appears
  only when there *is* a resolved sample.

**Design choices worth recording:**
- **Centred on the cursor**, not floating offset. The centre cell *is* the sampled pixel, so hiding the
  OS pointer (the brush's `want == 20` path, reused) leaves the loupe as the sole, precise cursor —
  no second crosshair to reconcile.
- **The loupe magnifies the composite (`uDoc`)** regardless of the *Source* option, because a
  magnifier that shows *what is under the cursor on screen* is the honest, intuitive behaviour (and
  uploading the active layer as a second texture just to magnify it is not worth it). The **swatch +
  readout** show the *resolved* sample, which honours *Source* + the sample-size average — so under
  *Active Layer* the two can legitimately differ, and the readout is the source of truth for what will
  be picked.
- **Magnification is the loupe's own** (`kLoupeMagLogical`), independent of the canvas zoom — the point
  of a loupe. Radius/mag are logical px; the renderer applies the HiDPI content scale on upload, like
  the reticle.

### 3.1 Ctrl = the temporary eyedropper (brush family)

Holding **Ctrl** with a stroke tool active (paint brush / eraser / inpaint brush) engages the
eyedropper for exactly as long as the key is held — the Space-pan convention (a temporary mode,
nothing latched): the brush reticle gives way to the loupe at the tracked cursor position, a click
(or live drag) samples into the foreground swatch (**Alt / right button → background**, as always),
and releasing Ctrl returns the brush exactly as it was. The pick honours the Eyedropper tool's own
options (Sample size + Source) — the options are read off the tool whether or not it is the active
one. Never mid-stroke: a stroke in flight keeps the pointer, and Ctrl only takes effect once it
ends. Binding audit (2026-07-16): Ctrl bound **nothing** on the canvas for the stroke family, so
the mode is free there; everywhere else Ctrl keeps its meaning (the selection tools' boolean op,
the wand's op badges, Smart-Resize's Ctrl-drag keep-chip, the text session's Ctrl accelerators)
because `temporaryEyedropperActive()` is gated on the stroke family being active.

## 4. Lineage, and what is deliberately not done

Colour sampling from an image and a magnifier loupe are both **decades-old, ubiquitous** techniques,
running back to the earliest raster editors:

- **Eyedropper / colour pick from a pixel** — pick the colour under the cursor into the active swatch.
  Lineage: the MacPaint / Deluxe Paint eyedropper tools, 1980s.
- **Neighbourhood averaging (3×3 / 5×5 / 11×11 sample size)** — a plain box-average of a pixel window.
  A box filter is textbook image processing (Gonzalez & Woods; the technique predates any editor).
- **Circular magnifier / loupe with a pixel grid** — magnifying a region under the cursor and drawing
  cell boundaries. Lineage: hardware/OS screen magnifiers, the photographic loupe, and grid-overlaid
  pixel zoom in editors well before 1995. Nearest-neighbour magnification with grid lines is the
  obvious, only-sensible way to show "the pixels".
- **Rendering the loupe on the GPU** — implementation detail (a compute shader samples a texture at the
  texel centre) rather than a technique of its own.

**⚠ Deliberately NOT done, and not to be added:** nothing here does image *analysis* to decide what
to sample (no edge-aware / object-aware "smart" pick); there is no perceptual colour-space transform
on the sample (the average is in the stored encoding, matching the compositor and the Fill dialog);
and the loupe shows the composite verbatim — no enhancement, denoise or upsampling filter. These are
design boundaries, not gaps.

## 5. Owed a user visual / interactive pass

The GPU loupe cannot be pixel-golden'd headlessly (the present pass needs a surface/swapchain; the
project verifies present-pass overlays by smoke + CPU-side unit tests, the reticle's precedent). The
CPU sampler is unit-tested; the loupe's *appearance* — magnification factor, radius, grid weight,
swatch band proportion, readout placement, and the hidden-cursor feel — is owed a live pass:
- confirm the magnification (`kLoupeMagLogical = 14` logical px/texel, ~8 texels across) reads well;
- confirm the readout pill placement (below the ring, flipping above near the viewport bottom);
- confirm Alt / right-button → background feels right, and the live-drag foreground update is pleasant;
- the checker under transparent texels + the centre-cell box-blue on light content;
- the colour-comparison ring (top = live sample, bottom = current swatch): band width, the neutral
  hairline casing, the graphite top arc off the pixels, and the Alt-hover flip to the bg swatch;
- the Ctrl temporary eyedropper over a brush (§3.1): the reticle↔loupe swap on the key transition
  (motionless pointer included), sampling by click and by drag, and the return to the brush.
