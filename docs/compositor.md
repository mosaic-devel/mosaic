# Compositor

The compositor turns a `core::Document`'s **layer tree** into a flat image (PLAN §3.7). The tree
is the single source of truth; the on-screen/exported image is *derived* from it, re-rendered when
the document changes. This is the basis of non-destructive editing.

S7 is split:

- **S7-a (done):** the CPU reference compositor — deterministic, GPU-free, fully headless and
  unit-tested. It defines the compositing model and the blend math, and is the golden reference the
  GPU path is checked against.
- **S7-b (done):** the Vulkan compute blend kernel (VMA + a GLSL mirror of the blend math),
  integrated as the GPU backend of `render::composite`, verified to match the CPU reference for
  every blend mode (see "GPU path" below).
- **S7-c (done):** the composite is **shown on the live canvas** (`render::WindowRenderer`) —
  uploaded to a texture and blitted centered + aspect-fit over the canvas background (see "Canvas
  presentation" below).
- **Deferred to S60 (perf):** GPU buffer residency across a composite (today `blendOver` round-trips
  per layer) and **tiled dirty regions** (re-composite only changed tiles) — both matter once
  editing re-composites frequently.

Everything here lives in `mosaic::render` (`src/render/compositor.*`, `src/render/blend.hpp`,
`src/render/gpu_compositor.*`, `shaders/composite_blend.comp`, `src/render/window_renderer.cpp`).

## The working buffer

Compositing happens in a **float RGBA buffer** with straight (non-premultiplied) alpha,
`common::ImageF` (`common/image.hpp`). Float avoids the banding/clamping of repeated 8-bit rounds;
the final buffer is converted back to an 8-bit `common::Image` (`toImage8`) for display/export.
`toFloat`/`toImage8` convert between the two. Layer pixel storage in the model stays 8-bit RGBA for
now — the float/tiled migration *begins* here and completes with the `.mosaic` tile store (S48).

## The model

For each **group**, children are composited **bottom (index 0) → top** into a fresh document-space
buffer (`compositeChildren`). Each child is turned into a document-space buffer (`renderLayer`) and
blended onto the accumulator with its **opacity**, **blend mode**, optional **mask**, and
**clip-to-below**:

- **Leaf layers** (Raster, Magic) rasterise their pixels through the layer **transform**
  (`sampleTransformed`, nearest-neighbour; bilinear/quality and magic-layer resampling are later —
  S50). Vector (S25) and Text (S29) layers have no pixels yet and contribute nothing.
- **Groups** composite their own children first, so the group's opacity/blend/mask apply to the
  group **as a unit** — and an adjustment layer inside a group is naturally scoped to that group.
- **Masks** are 8-bit coverage that multiplies the layer's alpha. A **linked** mask (the default)
  shares the layer's space and folds **before** the transform: leaf sources sample it at the source
  pixel (proportionally when resolutions differ), a group folds it onto its content-extent local
  buffer in group-local space (the buffer offset threaded through the fold — correct under region
  composites too), and a vector layer folds it at target resolution through `place⁻¹` (S31). An
  **unlinked** mask folds **after** placement, fixed in the layer's *parent* space (1 mask px per
  parent unit, zero coverage beyond the sheet): move the layer and the pixels slide under a
  stationary mask (`foldMaskThrough` / `foldUnlinkedMask`). A disabled mask is ignored everywhere;
  merge-down and the drag cache follow the same semantics (the cache refuses unlinked masks — that
  fold depends on the live transform). Adjustment-layer masks fold against the backdrop grid,
  transform-free — S32 confirmed that as their permanent story (an adjustment has no pixels of its
  own to carry a grid; the mask gates where the backdrop is affected).
- **Clip-to-below** (`clipToBelow`): a clipped layer's alpha is multiplied by the **clip base** —
  the alpha of the nearest non-clipped layer beneath it — so it only shows where the base is opaque.
- **Adjustment/Filter layers** modify the *accumulated backdrop* (the layers already composited
  below them). Because each group composites into its own buffer, an adjustment affects **only the
  layers below it within its group**, or **globally downward** at the root — exactly the brief's
  scoping (PLAN §3.7), driven purely by tree position. S7-a wired `Invert`, `Grayscale`, and
  `BrightnessContrast` to prove the mechanism; **S32 wired the full scalar set** (Levels, Exposure
  in linear light, Hue/Saturation, Color Balance, Threshold, Posterize — docs/adjustment-layers.md;
  a defaults bag is a byte-level no-op by early-out). S34 closed Curves (per-channel LUTs) and added Shadows/Highlights, Defringe, Matte Removal and Haze Removal; further
  filter kinds are S33–S35.

## Blend modes (`blend.hpp`)

All 23 `core::BlendMode`s are implemented as header-only pure functions, following the W3C
"Compositing and Blending Level 1" spec (which matches Photoshop/Krita). The header is shared by the
CPU compositor and the unit tests, and will be **mirrored in GLSL** by the GPU compositor (S7-b):

- `blendChannel(mode, b, s)` — the separable (per-channel) modes.
- `blendNonSeparable(mode, b, s)` — the four HSL modes (Hue/Saturation/Color/Luminosity) via the
  spec's `Lum`/`Sat`/`ClipColor`/`SetLum`/`SetSat` helpers.
- `compositeOver(mode, backdrop, source, opacity)` — the full source-over-with-blend: the blended
  color is weighted into the source by the backdrop alpha, then Porter-Duff "over".

### Color space (current simplification)

The document is composited in its **encoded (gamma) space** — the same space Photoshop blends in by
default — so blend-mode results match user expectations. PLAN §3.6's *linear-light* compositing and
ICC transforms arrive with color management (**S12**); this note will be revisited then.

## GPU path (S7-b)

`render::composite(doc, opts, backend)` runs the per-pixel **source-over-with-blend** — the one
hot, embarrassingly-parallel step — on the GPU when a device is available. The rest of the walk
(tree recursion, transforms, masks, clip, adjustments, checkerboard) is the shared CPU code, swapped
in via a pluggable `BlendFn`. So every blend mode runs through the **`composite_blend.comp`** compute
shader — a line-for-line GLSL mirror of `blend.hpp` — while the model code is reused unchanged.

- **`render::GpuCompositor`** (`gpu_compositor.*`) is a persistent Vulkan compute context (built on
  the headless `VulkanContext`) holding the pipeline and canvas-sized `rgba32f` storage images.
  `blendOver(acc, src, mode, opacity)` uploads the two operand buffers, dispatches the shader, and
  reads the accumulator back. Device memory is allocated through **VMA** (vendored, MIT) — the
  project's first allocator user.
- **Backends:** `Backend::Auto` (the default) prefers the GPU and falls back to CPU; `Cpu` forces
  the reference path; `Gpu`/`GpuCompute` require the GPU (and error if it is unavailable). If a
  single GPU blend fails mid-composite, that step falls back to the CPU blend so a result is still
  produced.
- **Parity:** the GLSL and C++ blend math are kept identical (the non-separable `setSat` uses the
  same vector form in both). Tests composite every mode on both backends and assert the GPU output
  matches the CPU reference within 1/255; on the dev GPU (RX 6600 XT / RADV) the demo is in fact
  byte-exact.

This is the correctness/parity milestone. It is deliberately simple: each blend uploads and reads
back full-canvas buffers, so it is **not yet the fast path**. Keeping buffers resident on the GPU
across a composite and re-compositing only **dirty tiles** is the follow-up (S7-c), along with
showing the result on the live canvas.

## The resident tiled path (S60-a)

S7-b's split — GPU does the blend, CPU does everything else — is *why* the GPU compositor is
slower than the CPU one: the walk has to come back to host memory for every layer to apply its
transform, mask and clip, so each layer costs two uploads and a readback. S60-a replaces it with a
**fused** kernel and a **resident** accumulator. Neither half works without the other.

- **`shaders/composite_tile.comp`** does the WHOLE per-layer step in one dispatch: inverse
  transform → resample (all nine kernels) → mask fold (linked proportional, linked affine,
  unlinked) → clip-to-below → blend (all 23 modes) → store. It also *publishes* the clip base a
  clipped layer above will read, so a whole clip run stays on the device. Vulkan 1.0 floor: 2
  storage images, 3 sampled images, 124 bytes of push constants, an 8×8 workgroup, no extensions.
- **`render::TileCompositor`** (`tile_compositor.*`) is the host half: a macrotile **atlas** of
  `R16G16B16A16_SFLOAT` accumulator slots (one atlas image where the document fits
  `maxImageDimension2D`, several where it does not — a 5000×8000 document cannot be one image on a
  1.0 device, so tiling is a *correctness* requirement, not only a speed one), a per-layer source
  cache budgeted through `TileResidency` + `VK_EXT_memory_budget`, and a dirty set on the
  `.mosaic` store's own 64 px grid projected up to `64 << k` macrotiles for dispatch.
- **Residency is the measurable property.** A composite of an unchanged document issues zero
  dispatches and uploads zero bytes; a layer uploads when its `contentRevision` / `maskRevision`
  moves, not per frame; and the accumulator is never read back as a side effect of compositing.
  `readback()` is the explicit, named seam, and `docs/s60-readback-consumers.md` is the list of who
  may call it.
- **Tiling invariance.** Every sample point is evaluated at its TARGET pixel — the macrotile origin
  travels as a separate integer rather than being baked into the inverse affine — so one
  recomposited macrotile is *bit-identical* to the same pixels inside a whole-canvas dispatch. That
  is what makes the macrotile size a free knob and a dirty-tile recomposite seamless.
- **The lane refuses what it cannot do exactly** (groups, adjustment layers, layer effects, live
  coverage partitions, non-raster leaves, layers over `maxImageDimension2D`, and the checkerboard,
  which belongs to the present pass) and the caller takes the CPU lane. `TileRefusal` names each
  one, so "why is this document on the CPU path" has a one-line answer.
- **Parity:** `tests/test_composite_tile_parity.cpp` holds the kernel to
  `render::composite(..., Backend::Cpu)` per blend mode × per resample filter at 1/255;
  `tests/test_tile_compositor.cpp` holds the whole lane to it end to end and asserts the residency
  and dirty-set properties, which no wall-clock measurement can see.

### From the accumulator to the screen, without host memory

The composite is only resident if it *reaches the screen* resident. Two pieces do that.

- **The device.** `TileCompositor` is built on the **presenting device**, because a `VkImage` does
  not cross a `VkDevice` and external memory is not in the Vulkan 1.0 floor. `WindowRenderer` owns
  that device (it is the only code with a surface, and a present-capable queue family cannot be
  chosen without one), so it wraps its own handles with `VulkanContext::adopt()` and hands the lane
  the same borrowing context type every other lane already uses — `WindowRenderer::computeContext()`.
  Headless callers keep `VulkanContext::shared()`; it is one code path either way. ⚠ An adopted
  context shares the queue with its adopter and its mutex cannot see the adopter's own submits, so
  both must submit from one thread (the UI thread, where both already run).
- **The resolve pass.** `shaders/tile_resolve.comp` converts one macrotile from the rgba16f
  accumulator into the 8-bit document texture the present pass already samples, and
  `TileCompositor::resolve(ResolveTarget&)` runs it over the macrotiles recomposited since the last
  resolve. On the app path that texture *is* `WindowRenderer`'s canvas texture
  (`prepareResidentCanvas` / `residentCanvasImage` / `residentCanvasLayout` /
  `noteResidentCanvasWritten`), so a frame that changes one dab costs one small dispatch and **zero
  bytes on the bus in either direction**. What it replaces: region composite → patch a doc-sized CPU
  mirror → copy the sub-rect → copy it again → memcpy into staging → `vkCmdCopyBufferToImage`.
  A composite that changed nothing resolves nothing and does not even submit.
- **Why not sample the atlas from `canvas_present.comp` directly?** Because the atlas is several
  images on a device whose `maxImageDimension2D` cannot hold the document, so the present shader
  would need slot arithmetic and a runtime-sized image array — the descriptor-indexing machinery of
  the still-open item 10 — and the whole overlay stack that reads `uDoc` (the loupe, the drag pass)
  would go through it too. The resolve saves 100% of the bus traffic, which is what the win is, and
  costs one full-screen conversion pass, which is not where the time goes.
- **Readback is now a named seam.** `render::CompositeReadback` (`composite_readback.*`) is the only
  route from the resident composite to CPU pixels: `pinMirror`/`peek` for the per-event consumers
  (the cursor readout, the eyedropper), `request()` for the once-per-action ones, and a checked-in
  consumer vocabulary so nothing can read the composite without declaring itself.
  `docs/s60-readback-consumers.md` §10 is the routing table, including the five consumers that
  deliberately keep the CPU walk.

The CPU reference in `compositor.cpp` is untouched and remains the golden lane. Export, save and
the `.mosaic` PRVW thumbnail deliberately keep it: they need byte-determinism the fp16 device
accumulator cannot promise.

## Canvas presentation (S7-c)

The composite is shown on the live canvas by `render::WindowRenderer` (`window_renderer.cpp`):

- `setCanvasImage(const common::Image&)` stores the composite; on the next frame it is uploaded
  into a device-local **VMA-allocated** `R8G8B8A8` texture (recreated only when the size changes,
  so it survives window resizes).
- Each frame, `drawFrame` clears the swapchain to the themed canvas background, then
  `vkCmdBlitImage`-es the texture **centered and aspect-fit** into it (`render::fitCentered`, a pure
  unit-tested helper). Blit maps components by semantic (R→R…), so an `R8G8B8A8` source presents
  correctly to a `B8G8R8A8` swapchain with no manual channel swap.
- `ui::VulkanCanvas::setDocumentImage()` holds the image until the lazily-created renderer exists,
  then forwards it. The app composites a **placeholder document** (`ui::buildPlaceholderDocument`,
  built directly in `app_window.cpp`) once on the CPU and shows it — a stand-in until real
  open/new-document (S9/S50). Live re-compositing on edits, kept efficient via GPU residency + dirty
  tiles, is the S60 perf pass.

## Transparency checkerboard

`CompositeOptions::checkerboard` flattens the result over the standard light/dark checkerboard,
yielding an opaque image where transparency is visible. Off by default (the raw composite keeps real
alpha for export/format I/O).

## Verifying without a display

The compositor needs no window (the GPU path uses the headless offscreen context):

```bash
# Composite the built-in demo document and write it as a PPM (Auto picks the GPU if present):
./build/linux-debug/bin/mosaic --composite-demo --export /tmp/demo.ppm
# Prints e.g. pixel(32,32) = 195,144,175,255 (blue x red multiply, green screen 50%, inverted).
# Force a backend with --cpu / --gpu-compute.
```

Tests live in `tests/test_compositor.cpp`: per-blend-mode reference values, the source-over alpha
math, and integration cases for stacking, opacity, masks, clip-to-below, group-as-unit, adjustment
scoping (within-group vs global), transforms, and the checkerboard — all on the CPU reference. The
integrated **demo scene** is guarded by a golden image, `tests/golden/compositor_demo.ppm`. The CPU
path is deterministic so its comparison is byte-exact; the GPU path is checked against the CPU
reference for every blend mode and against the golden within 1/255. Regenerate the golden from the
**CPU** path when the compositor intentionally changes:

```bash
./build/linux-debug/bin/mosaic --composite-demo --cpu --export tests/golden/compositor_demo.ppm
```
