# S60 — Readback consumers: every CPU reader of the composite

**Status:** AUDIT (2026-07-23), **re-verified against the tree 2026-07-28** and **implemented as
`render::CompositeReadback`** (S60-a item 12). §10 is the routing table — read that first if you are
wiring a consumer; §§1–9 are the analysis it came from. ⚠ **Every line number in §§1–8 is from
`7aef80c` and is now stale** (82 commits and +571/−187 lines in `app_window.cpp`); the re-verified
positions and the two consumers that changed shape are in §10.2.

**Why this exists.** `docs/s60-performance-plan.md` §3.3 makes the composite GPU-resident and
demotes `m_lastComposite` from "always-coherent CPU mirror" to "one of several things that can ask
the device for pixels". §9 names the failure mode:

> **Readback consumers.** One missed `m_lastComposite` reader silently reinstates a full per-frame
> readback.

and §3.3:

> Enumerating those consumers is a prerequisite commit, not a detail — a missed one silently forces
> a full readback per frame and eats the entire win.

This is that enumeration. It is a **design input for S60-a item 12** ("Explicit `requestReadback()`
for the five CPU consumers, with staleness opt-in") — and the first finding is that there are not
five. There are **nineteen**.

Measured from the worktree at `7aef80c`. Line numbers are from that tree.

---

## 1. Today's invariant, stated precisely

`MainWindow::m_lastComposite` (`src/ui/app_window.cpp:10042`) is a full, document-sized, 8-bit
straight-alpha RGBA copy of the composited document. It is kept coherent by exactly three writers:

| Writer | Line | What it does |
|---|---|---|
| `recompositeNow()` | `src/ui/app_window.cpp:9558` | full CPU composite → **replaces** the whole buffer, `++m_compositeRevision` |
| `recompositeRegionNow()` → `patchComposite()` | `src/ui/app_window.cpp:9643`, `9689` | region CPU composite → **memcpy-patches** the rect, `++m_compositeRevision` |
| document lifecycle | `src/ui/app_window.cpp:8609`, `8723` | cleared on `clearDocument()` / tab switch |

Two derived CPU copies ride behind it purely to feed the present pass:
`VulkanCanvas::m_documentImage` (`src/ui/vulkan_canvas.cpp:271`, drained at `4855`) and
`WindowRenderer::m_pendingCanvas` / `m_pendingCanvasRegion` (`src/render/window_renderer.cpp:551`,
drained at `1980`–`2013`).

There is **no GPU→CPU path for the canvas image today**. `WindowRenderer` uploads and never reads
back; the only `vkCmdCopyImageToBuffer` calls in the tree belong to `blur_gpu`, `texture_gpu`,
`extrude_gpu`, `compute_fill`, `render.cpp` and `gpu_compositor` — all one-shot lanes, none of them
the interactive canvas. So every consumer below is, today, a **free pointer dereference**. After
S60-a every one of them is a transfer.

**`m_compositeRevision`** (`src/ui/app_window.cpp:10047`, starts at 1) is the coherence token. It is
bumped by both writers and read by three consumers (Smart Resize, the Channels histogram, the
adjustment-panel fade). It is *not* a content hash — an identical recomposite still bumps it.

---

## 2. Method, and how confident I am

### 2.1 What I did

1. `grep -rn m_lastComposite src/` → **47 hits**, all in `src/ui/app_window.cpp` except two prose
   references in `src/ui/channels_panel.hpp`. Every one of the 47 is accounted for below or is a
   write/lifecycle site from §1.
2. `grep -rn compositeRevision src/` → 12 hits; all three readers followed to their UI cadence.
3. `grep -rn "render::composite\|compositeRegion\|Backend::" src/` → **13** `composite` /
   `compositeRegion` calls outside `src/render/`: 11 in `app_window.cpp` (`3007`, `4133`, `4163`,
   `5958`, `5995`, `6696`, `8290`, `8434`, `9236`, `9586`, `9666`) and 2 in `app/main.cpp`
   (`165`, `192`). All 13 classified below.
4. `grep -rn "compositeGroup\|adjustmentPreview\|adjustmentBackdrop\|rasterizeLayer\|mergeDown"` →
   the sibling-walk family (§5), explicitly ruled out of scope so nobody rediscovers it in a panic.
5. Followed **every** `set*Provider` / `set*Host` on `MainWindow` — the `std::function` seams are
   the only way pixels leave the class without a direct member read. `setSourceProvider`
   (`764`), `setUnderProvider` (`1485`), `EyedropperHost` (`868`), `CropToolHost` (`917`–`944`),
   `MoveToolHost` (`881`), `BrushToolHost` (`948`), the Fill/Layer-Effects dialog hosts.
6. Read `MainWindow::onFrame()` (`9856`–`9969`) and `VulkanCanvas::renderFrame()`
   (`src/ui/vulkan_canvas.cpp:4855`–`4899`) end to end, to catch anything that samples the composite
   on the frame timer rather than from an edit callback. This is where the three most dangerous
   consumers turned up (§4).
7. `grep -rn "readback\|vkCmdCopyImageToBuffer" src/render/` to confirm the "no canvas readback
   exists today" claim above.
8. Checked `tests/` and `src/app/main.cpp` so the harness callers are on the record.

### 2.2 Confidence

- **Direct `m_lastComposite` readers: high (essentially certain).** The member is private to one
  class in one translation unit; the grep is exhaustive and all 47 hits are classified. If the
  enumeration is wrong here it is wrong about *cadence*, not about *existence*.
- **Direct `render::composite` / `compositeRegion` callers: high.** 14 sites, all read.
- **Indirect consumers reached through a `std::function` provider: medium-high.** I enumerated and
  followed every provider installed by `MainWindow` today, but a provider is by construction
  invisible to a grep for the member it closes over — `chans->setSourceProvider([this]{ return
  &m_lastComposite; })` at `764` is exactly that shape, and so is the adjustment-panel fade at
  `1485`. A *future* provider would not show up in any of the greps above. **This is the strongest
  argument for guard (c) in §7: the enumeration cannot be kept correct by discipline, only by an
  API seam that forces every consumer to name itself.**
- **Things I deliberately did not chase:** the inpaint engine backends, `core::brush`, the vector
  and text rasterisers, and colour management. All of them read *layer* pixels, never the composite;
  spot-checked (`brush_engine.cpp:1511 composite()` is the dab accumulator, an unrelated name
  collision).

---

## 3. The inventory

19 consumers. **Group A** reads the resident composite (these are the ones that become readbacks).
**Group B** runs its own compositor walk today (these choose between "readback the resident
composite" and "keep walking on the CPU"). §5 lists what is *not* a consumer.

Disposition vocabulary is the plan's: `sync readback (rect)`, `async readback + accept staleness`,
`GPU-side instead`, `drop`.

### Group A — direct readers of `m_lastComposite`

#### A1. Status-bar cursor colour readout

- **Who:** `MainWindow::onCanvasCursor` — `src/ui/app_window.cpp:9036`, reads at `9052`–`9061`.
  Driven by `VulkanCanvas::emitCursor` (`src/ui/vulkan_canvas.cpp:350`) via `notifyCursor` (`324`).
- **What:** **one pixel**, at `floor(docX), floor(docY)`. Falls back to `m_recomposePreview` during a
  Smart-Recompose review (`9053`).
- **When:** **per pointer event** — FL_MOVE, FL_DRAG *and* the tablet sink (`emitCursor` is the one
  funnel both paths use). Several per frame during a fast drag; zero when the pointer is still.
- **Latency tolerance:** **none.** The readout is a live label under a moving cursor; one frame of
  lag is the difference between "the colour under the pointer" and "the colour the pointer just
  left". Staleness here is directly perceptible on a gradient.
- **Disposition:** `sync readback (rect)` — but served from a **pinned CPU mirror of the macrotile
  under the pointer**, not a fence. One pixel is the worst possible transfer to do synchronously;
  pinning the containing macrotile while a pointer is over the canvas turns every event into a
  memory read. See `peek()` in §6.

#### A2. Eyedropper sample + loupe readout ⚠ **dangerous**

- **Who:** `MainWindow::eyedropperSample` — `src/ui/app_window.cpp:8257`, source resolved at `8271`
  through `wandMergedSource` (`8282`). Installed as `EyedropperHost::sample` at `868`.
  Called from `VulkanCanvas::syncLoupe` — `src/ui/vulkan_canvas.cpp:690` — and, on commit, from
  `pushEyedropper` (`662`) and `dragEyedropper` (`669`).
- **What:** a **(2r+1)² window**, r ∈ {0,1,2,5} from the tool's *Sample* option
  (`src/core/color_sample.hpp:16`). With *Source = All Layers* the image is `m_lastComposite`;
  with *Active Layer* it is the layer's own pixels (not a composite consumer).
- **When:** **every frame** while the Eyedropper (or the temporary Alt-eyedropper) is active and the
  pointer is inside the canvas — `syncLoupe` runs unconditionally inside `renderFrame()`
  (`src/ui/vulkan_canvas.cpp:4885`). Plus once per press and per drag event on commit.
- **Latency tolerance:** **none, and it is the strictest case in the whole list.** The loupe paints
  the sampled swatch in its ring and the hex/RGB tile beside it; the user's entire mental model is
  "this is the pixel under the crosshair". A one-frame lag reads as the loupe lying.
- **Disposition:** `sync readback (rect)` off a **pinned mirror**, same mechanism as A1 — the pin
  covers the sample window, so the 11×11 case is still one macrotile. Never a fence.
- **Trap:** the *stale* branch of `wandMergedSource` (`8288`–`8294`) runs a **full CPU composite**.
  Because the caller is per-frame, a persistently stale `m_lastComposite` means a full composite
  *every frame*. See Finding F4.
- ⚠ **2026-07-29 — the latency claim above is wrong, and F4 is fixed.** "None, and it is the
  strictest case in the whole list" conflated two callers that are not the same consumer. The
  **live readout** (`syncLoupe`, per frame) tolerates a lag perfectly well: a hover is by
  definition a period in which nothing is changing, so a stale-by-one-edit answer and a current one
  are *the same pixels*, and §10's own standing rule — a consumer that fires per frame or per
  pointer event is not a candidate for a blocking readback — outranks a feeling about latency. It
  asks **`AnyRecent`** now. The **commit** (`pushEyedropper` / `dragEyedropper`, per press and per
  drag event) is the one with the strict claim, because it writes the picked colour into the fg/bg
  swatch — state that outlives the gesture — and it keeps **`Current`**. Splitting them is what
  `Freshness` is for. See §8 F4.

#### A3. Magic Wand, Source = All Layers

- **Who:** `MainWindow::magicWandClick` — `src/ui/app_window.cpp:8074`, source at `8095`.
- **What:** the **whole canvas** — `core::magicWandSelection` floods from the seed and, with
  `contiguous = false`, touches every pixel.
- **When:** **once per click.** Never continuous.
- **Latency tolerance:** must reflect the document as committed *before* the click; a stale
  composite would flood the wrong region and land an undoable wrong selection. But it is a discrete
  action and a few ms of latency is invisible.
- **Disposition:** `sync readback (rect)` with `roi = whole canvas`. Expensive (one full transfer)
  but it happens on a click, not a frame. Do **not** make it async: the selection command must land
  in the same event turn as the click, or undo history interleaves with later edits.

#### A4. Edge Select Brush grow, Source = All Layers

- **Who:** `MainWindow::edgeBrushGrow` — `src/ui/app_window.cpp:8120`, source at `8135`.
- **What:** the **whole canvas** (`core::edgeGrowSelection` is a geodesic solve over the image).
- **When:** **once per stroke, on release** — the edge brush's solve-on-release rule is a hard
  design constraint: it must never solve during the stroke, so this can never be per-frame.
- **Latency tolerance / disposition:** identical to A3. `sync readback (rect)`, whole canvas, once
  per gesture *end*. Note this one is architecturally safe *because of* that solve-on-release rule —
  worth recording, because relaxing it would turn this into a per-frame full-canvas consumer.

#### A5. Channels-tab histogram

- **Who:** provider installed at `src/ui/app_window.cpp:764`; consumed by
  `ChannelsPanel::recompute` — `src/ui/channels_panel.cpp:378` — reached from `notifyChanged`
  (`368`, called at `app_window.cpp:9600`) and `onTabShown` (`374`, called at
  `src/ui/layer_panel.cpp:1177`).
- **What:** the **whole canvas** — `computeHistogram` bins every pixel into 5×256 bins.
- **When:** on a **full** recomposite, *and only when the tab is visible* (`notifyChanged` gates on
  `visible_r()`); plus once when the tab is shown. **Never** on a region patch — see Finding F2.
- **Latency tolerance:** **high.** A histogram one frame behind is invisible; a histogram a whole
  gesture behind is arguably *correct* (nobody reads a histogram mid-brush-stroke). The panel
  already accepts far worse staleness today.
- **Disposition:** `can be computed GPU-side instead` — this is the textbook `subgroupArithmetic`
  case the plan calls out in §2.2 and puts in S60-e. Until that lands, `async readback + accept
  staleness`: request on a settle timer, drop superseded requests, and stamp the result with the
  revision it came from so the panel can show "current" vs "computing". **Never** synchronous.

#### A6. On-canvas channel isolation ⚠ **dangerous**

- **Who:** `MainWindow::presentComposite` / `presentCompositeRegion` —
  `src/ui/app_window.cpp:9717`, `9729`; re-push hook `refreshCanvasForIsolation` at `9744`.
  Mask logic: `ChannelsPanel::applyIsolation` → `applyChannelViewMask`
  (`src/ui/channels_panel.hpp:148`, `79`).
- **What:** the **whole canvas** (or the patched rect) — it makes a **full copy** of the composite
  and remaps every pixel, on the way to the canvas.
- **When:** **every frame that pushes a composite**, whenever any channel eye is off.
- **Latency tolerance:** n/a — this is not a readback at all under residency.
- **Disposition:** **`GPU-side instead`, unambiguously.** A channel remap is four multiplies in
  `canvas_present.comp`; doing it as a CPU copy of the whole document per frame is pure waste that
  only exists because the composite happened to be on the CPU. `channels_panel.hpp:27-29` already
  admits this ("isolating a channel in the CANVAS compositor is a deliberate follow-up — it would
  need the GPU composite path"). **S60-a should collect that debt while it is in the present pass.**
- **Side effect worth flagging:** channel isolation currently *disables* the GPU drag fast path
  (`app_window.cpp:9192`, and the comment at `9188`–`9191`). Moving the remap into the present pass
  deletes that exception too.

#### A7. Smart Resize importance map + keep-region chips ⚠ **dangerous**

- **Who:** `MainWindow::ensureSmartAnalysis` — `src/ui/app_window.cpp:8421`, reads at `8424`–`8440`.
  Reached through **two** host callbacks: `smartRect` (`932`) and `keepRegions` (`934`).
  `keepRegions` is called from `VulkanCanvas::refreshSmartChips`
  (`src/ui/vulkan_canvas.cpp:2137`), which is called from `syncSmartChips` (`2206`) — **inside
  `renderFrame()`** (`4881`) — and from `applySmartCropSuggestion` (`2118`).
- **What:** the **whole canvas** — `core::retarget::buildImportanceMap` is a full-image analysis.
- **When:** the *check* is **per frame** while the Crop tool is up with Smart Resize on and a rect
  staged (`vulkan_canvas.cpp:2201`); the *rebuild* fires whenever `m_compositeRevision` moves
  (`8427`). So: cheap per frame, full-canvas on every edit while the Crop tool is active.
- **Latency tolerance:** **high.** The chips are a suggestion overlay; being one settle behind is
  invisible and arguably better (they stop flickering mid-edit). This is the plan's canonical
  "should say so" staleness opt-in.
- **Disposition:** `async readback + accept staleness`, keyed on the revision it was built from
  (the memo already works exactly this way). Longer term the importance map is a reduction and
  belongs GPU-side alongside the histogram (plan §4, S60-e).
- **Trap:** the stale-composite fallback at `8431`–`8438` runs a **full CPU composite**, and
  `8445` deliberately does **not** memoize it. Per-frame caller + unmemoized full composite = a
  pathological loop. See Finding F3.

#### A8. Smart Recompose job seed

- **Who:** `MainWindow::startRecompose` — `src/ui/app_window.cpp:4717`; guard at `4723`, copy at
  `4727` (`job->src = m_lastComposite`, a genuine full-image **copy**, handed to a worker thread).
- **What:** the **whole canvas**.
- **When:** once, on the Recompose button.
- **Latency tolerance:** none-ish, but it is a discrete user action that already shows a progress
  bar and takes seconds. A blocking full readback is invisible inside that.
- **Disposition:** `sync readback (rect)`, whole canvas, once. The worker needs an owned CPU image
  anyway, so the readback *is* the copy it was already making — net cost ≈ zero.

#### A9. Crop-expansion Inpaint fill seed

- **Who:** `MainWindow::startCropExpandFill` — `src/ui/app_window.cpp:4581`; guard at `4584`,
  read at `4596` (`common::blitRegion(seed, m_lastComposite, dx, dy)`).
- **What:** the **whole canvas**, blitted into a larger (post-expansion) seed buffer.
- **When:** once, on Crop→Apply with Fill = Inpaint.
- **Latency tolerance / disposition:** identical to A8. `sync readback (rect)`, whole canvas, once,
  behind a progress bar.
- **Note:** both A8 and A9 already *refuse to run* when `m_lastComposite` is stale
  (`4586`, `4725`) rather than recompositing. That refusal is a ready-made template for "this
  consumer would rather wait than pay": under residency they become "await the readback future,
  then start the worker".

#### A10. Adjustment-panel fade under-image ⚠ **dangerous**

- **Who:** the `setUnderProvider` lambda — `src/ui/app_window.cpp:1485`, reads at `1495`–`1519`.
  Consumed by `AdjustmentPanel::refreshFadeBlend` (`src/ui/adjustment_panel.cpp:744`), driven from
  `MainWindow::updateAdjustmentPanelFade` (`src/ui/app_window.cpp:5644`) → called every frame from
  `updateAdjustmentPanel` (`5623`) → `onFrame` (`9923`).
- **What:** **not a rect** — a *scattered gather*. For each of the ~panel-sized (w×h) output pixels
  it maps a **screen** point through `view.toDoc()` and samples one composite pixel (`1502`–`1512`).
  Under rotation/zoom the touched doc-space set is an arbitrary rotated quad; its AABB can be much
  larger than the panel, and at high zoom-out it can be **most of the document**.
- **When:** per frame while the panel is faded, gated on a fingerprint that folds
  `m_compositeRevision` **and** zoom/rotation/pan (`5680`–`5692`). So a **pan while the panel is
  faded invalidates it every frame** — deliberately, per the round-3 "ignores panning" fix at
  `5676`–`5679`.
- **Latency tolerance:** medium. It is a decorative translucency; one frame behind during a pan
  would be unnoticeable. But it must not be *arbitrarily* stale — the whole point is that the
  graded pixels stay visible under the panel while you drag its sliders.
- **Disposition:** **`GPU-side instead`.** This lambda is re-implementing the present pass —
  view transform + the 8 px screen-space checkerboard with `canvas_present.comp`'s own greys
  (`1516` even says so). Under residency the honest answer is "ask the renderer for a
  screen-space capture of the panel rect", which is one small `vkCmdCopyImageToBuffer` from the
  already-composed present target — a *screen-space* rect, bounded by the panel, independent of
  zoom. Falling back to `async readback + accept staleness` of the doc-space AABB is the
  contingency, and it is strictly worse.

#### A11. Recompose-review restore

- **Who:** `MainWindow::cancelRecomposeReview` — `src/ui/app_window.cpp:4874`, reads at `4878`–`4879`.
- **What:** the whole canvas, re-pushed to the canvas texture.
- **When:** once, on Esc / Cancel out of a Recompose review.
- **Latency tolerance:** none (it is the frame the display returns to the document) but trivial.
- **Disposition:** **`can be dropped`.** Under residency the accumulator still *holds* the document
  composite; leaving review is "stop displaying the preview texture", not "re-upload the document".
  This consumer exists only because the display source is a CPU image today. Same for the empty
  paths at `8611` (`clearDocument`) — they push an empty image to tear the texture down.

### Group B — independent compositor walks

These do not read `m_lastComposite`; they run `render::composite` / `compositeRegion` themselves.
Under residency each one is a fork in the road: **readback** (cheap if the resident composite is
already what it wants) or **keep the CPU walk** (correct, deterministic, and already written).

#### B1. Quick Export PNG

- **Who:** `MainWindow::quickExportPng` — `src/ui/app_window.cpp:4093`, composite at `4132`–`4133`.
- **What:** full canvas, `checkerboard=false`, `resampleFilter = currentResampleFilter()`.
- **When:** once, on the menu item, after a file dialog.
- **Disposition:** **keep the CPU walk.** Rationale: export is the *archival* path and must be
  deterministic and bit-reproducible; the plan pins the CPU lane as the golden reference (§2.4,
  §9 "the CPU goldens are byte-exact and must stay CPU-produced"). A readback of an fp16 device
  accumulator is not byte-identical to the fp32 CPU reference — acceptable for the screen, not for
  a file the user keeps. Cost is one composite behind a file dialog: irrelevant.

#### B2. Quick Export JPEG / JXL / Export As…

- **Who:** `MainWindow::compositeForExport` — `src/ui/app_window.cpp:4156`, composite at `4163`.
  Callers: `quickExportJpeg` (`4210`), `quickExportJxl` (`4258`), `exportAs` (`4283`).
- **What / when / disposition:** exactly B1. Same reasoning, same verdict: **keep the CPU walk.**

#### B3. `.mosaic` PRVW preview thumbnail

- **Who:** `MainWindow::compositeForPreview` — `src/ui/app_window.cpp:3005`, composite at
  `3006`–`3007`. Callers: `saveOtherVersion` (`2637`), `foldedWriteTo` (`3173`), `commitAppendSave`
  (`3200`), `plainWriteTo` (`3469`).
- **What:** full canvas, default options.
- **When:** once per **save** (not per autosave — `JournalSession::autosave`,
  `src/io/mosaic/journal_session.cpp:110`, passes no preview, so the recovery journal costs nothing
  here. Good design; worth not breaking).
- **Disposition:** **keep the CPU walk.** Its own comment at `3001`–`3003` is the argument:
  *"deterministic for unchanged content, so the differ can skip an unchanged downscale
  byte-for-byte"*. A GPU readback that wobbles in the low bit would make every save re-emit a PRVW
  chunk and grow the file for nothing. This is a **correctness dependency on CPU determinism**, not
  a performance choice.

#### B4. Edit ▸ Copy Merged

- **Who:** `MainWindow::copySelection(merged=true)` — `src/ui/app_window.cpp:6688`, composite at
  `6695`–`6696`.
- **What:** full canvas, `checkerboard=false`; then `core::copyMerged` crops to the selection.
- **When:** once, on Ctrl-Shift-C.
- **Disposition:** `sync readback (rect)` — **the selection's bounding box**, not the canvas. Unlike
  B1–B3 the clipboard is not an archival artifact and does not need bit-exactness against the CPU
  reference; and a merged copy of a small selection on a 5k canvas currently pays a full composite,
  which is a latent win. (If the selection is empty, `copyMerged` takes the whole canvas — then it
  is a full readback, once, on a keypress. Fine.)

#### B5. GPU-drag `below` texture

- **Who:** `MainWindow::buildBelowComposite` — `src/ui/app_window.cpp:9218`, composite at
  `9234`–`9236`. Called from `driveTransformPreview` (`9199`).
- **What:** full canvas with the dragged layer hidden. Already optimised: a single-top-level-child
  document returns a 1×1 transparent image (`9229`–`9230`).
- **When:** **once per gesture**, at arm time.
- **Disposition:** **`can be dropped`** — and this is the happy one. `below` exists so the GPU has
  a static backdrop to composite the dragged layer over; a *resident tiled compositor already has
  that*, as the set of macrotiles not dirtied by the dragged layer. Under S60-a the whole
  `beginGpuDrag(below, dragged)` seam (`src/ui/vulkan_canvas.cpp:296`,
  `src/render/window_renderer.cpp:1716`) collapses into "mark the dragged layer's tiles dirty".
  **This is a consumer that residency deletes rather than converts, and it is the single biggest
  gesture-start cost on a multi-layer document.**

#### B6. 3D-text `reflectCanvas` environment snapshot ⚠ **dangerous**

- **Who:** `MainWindow::refreshLayerReflection` — `src/ui/app_window.cpp:9339`, composite via
  `buildBelowComposite` at `9364`, downsample at `9367`–`9390`. Driven by `updateReflectionEnv`
  (`9317`), called **every frame** from `onFrame` (`9921`).
- **What:** a full-canvas composite *with the text layer hidden*, box-downsampled to ≤768 px
  (`kReflectEnvMaxDim`, `9250`) into a float env texture stored on the `TextLayer`
  (`src/core/layer.hpp:436`–`452`).
- **When:** the *fingerprint check* (`reflectStackFingerprint`, `9252`) is **per frame, per text
  layer**; the *rebuild* fires when the fingerprint moves and a 0.30 s settle
  (`kReflectEnvSettleSec`, `9251`) has elapsed. So: cheap per frame, one full composite + one
  full-canvas downsample per settle. And the rebuild calls `recompositeNow(false)` (`9336`) — a
  **second** full composite in the same frame.
- **Latency tolerance:** **high, and already exploited.** The 0.30 s settle exists precisely because
  the mirror does not need to be current mid-gesture — the round-2/round-3 feedback recorded in the
  comment at `9241`–`9249` is the design rationale.
- **Disposition:** `async readback + accept staleness` — request the downsampled env at whatever
  resolution the layer wants; the settle already tolerates hundreds of milliseconds. Better still,
  `GPU-side instead`: the env is a mip of the resident accumulator with one layer masked out, which
  is a natural thing for a tiled compositor to produce and hand straight to `ExtrudeGpu` (which
  today uploads it back — `src/render/extrude_gpu.cpp:455`–`465` — i.e. we currently do
  GPU→CPU→GPU for these pixels).
- **This is the consumer most likely to be missed**, because it is not in the plan's list, it lives
  under a name that says nothing about compositing, and it is on the per-frame path.

#### B7. Fill-dialog live preview pane

- **Who:** `MainWindow::fillCompositePreview` — `src/ui/app_window.cpp:5924`, `compositeRegion` at
  `5958` with **`clampToCanvas=false`**.
- **What:** a **rect** — the fill's affected doc-space AABB, grown to the pane's aspect
  (`matchRegionToPaneAspect`, `6020`), *deliberately allowed off-canvas*.
- **When:** on demand while the modal Fill dialog recomputes (per control change, not per frame).
- **Disposition:** **keep the CPU walk.** Two reasons, both structural: (a) it mutates the document
  (`5954` blit, `5961` restore) around the composite — a temporary edit the resident compositor's
  dirty-tile bookkeeping would have to be told about and then untold; (b) it needs pixels
  **outside the canvas**, which by construction the resident accumulator does not have.
  `clampToCanvas=false` is not an implementation detail, it is a requirement the readback API
  cannot satisfy.

#### B8. Layer-Effects dialog live preview pane

- **Who:** `MainWindow::layerEffectsPreview` — `src/ui/app_window.cpp:5971`, `compositeRegion` at
  `5995`, also **`clampToCanvas=false`**.
- **What / when:** a rect around the layer's `effectsBounds` in doc space, pane-aspect-matched;
  on demand from the modal (`src/ui/layer_effects_dialog.cpp:1396`).
- **Disposition:** **keep the CPU walk**, same off-canvas argument as B7.

### The producers (for completeness, not consumers)

`recompositeNow` (`9558`, composite at `9585`–`9586`) and `recompositeRegionNow` (`9643`,
`compositeRegion` at `9665`–`9666`) are what S60-a replaces. `src/app/main.cpp:165` (`--texture`)
and `:192` (`--composite-demo`) are headless harness callers and are unaffected.

---

## 4. The dangerous ones

Ranked by "how badly does this reinstate a per-frame full transfer if we get it wrong".

| # | Consumer | Cadence | Extent | Sync? | Why it is dangerous |
|---|---|---|---|---|---|
| 1 | **A2** Eyedropper loupe | **per frame** | (2r+1)² | ~~yes~~ **no** (2026-07-29) | Per-frame *and* — until 2026-07-29 — strictly synchronous *and* its stale path was a full CPU composite (F4, now fixed: the readout asks `AnyRecent`, the fallback is memoised on `m_compositeRevision`). A naive `requestReadback(1px).get()` here is still a fence every frame. |
| 2 | **A7** Smart Resize chips | **per frame** (check) | **full canvas** | yes | Per-frame caller, full-canvas rebuild on every revision bump, unmemoized stale fallback (F3). |
| 3 | **A10** Adjustment-panel fade | **per frame** while faded | **scattered, view-dependent** | yes | Invalidated by *pan*, not just edits. Not expressible as a doc-space rect at all under rotation. |
| 4 | **B6** 3D reflect env | **per frame** (check) | **full canvas** | yes | Not in the plan's list. Rebuild costs two full composites in one frame. |
| 5 | **A6** Channel isolation | **per frame** while active | **full canvas** | n/a | Not a readback but a full-canvas CPU copy+remap per frame that residency should delete outright. |
| 6 | **A1** Cursor readout | **per event** | 1 px | **yes** | Cheapest possible payload, worst possible latency requirement. Must never fence. |

The shape of the answer for 1, 2, 4 and 6 is the same and it is not "readback faster": it is a
**CPU-side mirror of a small pinned set of macrotiles, refreshed as part of the composite pass**.
Consumers that need one pixel *now* read the mirror; consumers that need the canvas read it
asynchronously and say so.

### What surprised me

- **The plan says five consumers. There are nineteen** — and three of the six most dangerous ones
  (A10, B6, A6) are not on the plan's list at all.
- **Two of the worst are on the per-frame path for reasons that have nothing to do with the
  composite**: the adjustment-panel fade is a *decoration*, and the 3D reflect env is a *text
  feature*. Neither name suggests "reads the whole canvas every frame".
- **Export is not the dangerous one.** Every export/save consumer (B1–B3) is once-per-user-action
  and, more importantly, *should not become a readback at all* — it has a determinism requirement
  the GPU lane cannot meet. The plan's §3.3 list puts export first; it deserves to be near-last.
- **One consumer disappears entirely** (B5, the GPU-drag `below` texture) and one is pure debt the
  present pass should have absorbed already (A6). Residency *removes* work here rather than
  relocating it.
- **The off-canvas requirement (B7/B8)** is a hard boundary on the readback API: two live consumers
  need pixels the resident accumulator does not and will not have.

---

## 5. What is *not* a consumer (ruled out on purpose)

Recorded so a later reader does not rediscover these in alarm.

- **Layer/document thumbnails** — `layerThumbnail` (`src/ui/layer_panel.cpp:204`) reads *layer*
  pixels. Its two compositor calls, `render::compositeGroup` (`231`) and `render::adjustmentPreview`
  (`269`), are **sibling walks over a subtree**, not reads of the document composite. They stay CPU
  and are unaffected by residency. (They are still worth attention for their own reasons —
  `compositeGroup` flattens the whole document per group thumbnail, as the comment at `320`–`321`
  admits — but that is S60-d's problem, not this document's.)
- **Shift-click "select the group's pixels"** — `LayerPanel::shiftClickThumbnail`
  (`src/ui/layer_panel.cpp:1932`, `compositeGroup` at `1947`). Same: a subtree walk.
- **Adjustment editor histogram** — `render::adjustmentBackdrop` (`src/ui/app_window.cpp:1549`).
  Deliberately composites the *backdrop without the adjustment's own step* at ~160 px. Not the
  document composite; cannot be served by a readback.
- **Layer ▸ Rasterize** (`6226`) and **Layer ▸ Merge Down** (`6367`) — single-layer bakes.
- **Magic Wand / Edge Brush / Eyedropper / Texture "Estimate from layer" with Source = Active
  Layer** — `activeLayerDocImage` (`8301`), used at `5748`, `8096`, `8136`, `8271`. Layer pixels.
- **Bucket fill** (`8152`) — floods the active raster's own pixels.
- **Inpaint live preview** (`4471`) — blits into the layer, then asks for a *region recomposite*.
  It is a **producer** of dirty rects, not a reader.
- **Brush engine `composite()`** (`src/core/brush/brush_engine.cpp:1511`) — name collision. It
  composites dabs into the stroke target.
- **The recovery journal** (`src/io/mosaic/journal_session.cpp:110`) — passes no preview, so
  autosave never composites. Keep it that way.
- **`ExportDialog`** (`src/ui/export_dialog.cpp:381`) — consumes the *one* flatten it is handed and
  resamples it in-dialog. Downstream of B2, not a separate consumer.

### And three CPU copies residency deletes

`m_lastComposite` itself + `patchComposite` (`9689`); `VulkanCanvas::m_documentImage`
(`src/ui/vulkan_canvas.cpp:272`); `WindowRenderer::m_pendingCanvas` /
`m_pendingCanvasRegion` (`src/render/window_renderer.cpp:552`, `561`). Today a single 1920×1080
frame that changes one dab does: region composite → patch a doc-sized CPU buffer → copy the sub-rect
into `m_documentRegion` → copy it again into `m_pendingCanvasRegion` → memcpy into staging → upload.
**Three CPU-side copies before the staging write.** That is worth stating in the S60-a commit
message.

---

## 6. Proposed API sketch — the explicit readback seam

A sketch, not an implementation. The shapes that matter are: (a) every consumer **names itself**,
(b) every consumer **declares its freshness requirement** rather than getting synchrony by default,
(c) the one-pixel-right-now case does not go through the futures path at all.

```cpp
// src/render/composite_readback.hpp  —  FLTK-free; the policy decisions are pure and unit-testable.
namespace mosaic::render {

// How current the pixels have to be. This is the field the plan's "should say so" refers to;
// it is deliberately NOT a bool, because "async" and "stale is fine" are different claims.
enum class Freshness : std::uint8_t {
    Current,   // must include every edit committed before this call (cursor readout, eyedropper)
    Settled,   // may lag a gesture in flight; must be current once the gesture ends (reflect env)
    AnyRecent, // any composite from the last few frames will do (histogram, Smart Resize)
};

struct ReadbackRequest {
    common::Rect roi;                       // document px; empty = whole canvas
    Freshness   freshness = Freshness::AnyRecent;   // note the SAFE default, not the convenient one
    bool        blocking  = false;          // caller will wait on the future within this event turn
    std::string_view name;                  // REQUIRED: profiler row + the debug budget report
};

struct ReadbackResult {
    common::Image  image;        // roi-sized, straight alpha, document space (8-bit today)
    common::Rect   roi;          // what was actually served: >= the request, macrotile-aligned
    std::uint64_t  revision = 0; // the composite revision these pixels are from
    bool           stale = false;// true when served from a cache older than the current revision
};

class CompositeReadback {
public:
    // The general path. Never blocks inside request(); `blocking` only promises the future will be
    // satisfiable this turn (it schedules a fence rather than riding the next frame's copy queue).
    [[nodiscard]] std::future<ReadbackResult> request(const ReadbackRequest& req);

    // The hot path, and the reason A1/A2 do not need futures at all. Served from the CPU mirror of
    // the macrotile containing `docPt`; nullopt when that tile is not mirrored (caller falls back
    // to request(), or to nothing — a readout that blinks off for one frame beats a fence).
    [[nodiscard]] std::optional<common::Color8> peek(common::Vec2 docPt) const noexcept;
    [[nodiscard]] std::optional<common::Image>  peekRect(const common::Rect& roi) const;

    // Keep the macrotiles covering `roi` CPU-mirrored, refreshed as part of the composite pass
    // (not as a separate transfer). RAII; refcounted; the pointer-following consumers hold one
    // while their tool is active. This is what makes `peek` cheap enough to call per frame.
    [[nodiscard]] MirrorPin pinMirror(const common::Rect& roi, std::string_view name);

    // Screen-space capture of the already-composed present target. A10's real answer: bounded by
    // the requested widget rect regardless of zoom/rotation, and it already includes the
    // checkerboard and view transform the lambda at app_window.cpp:1485 re-implements by hand.
    [[nodiscard]] std::future<common::Image> requestScreenRect(common::Rect screenPx,
                                                               std::string_view name);

    [[nodiscard]] std::uint64_t revision() const noexcept;

    // Debug/profiling surface (see §7).
    struct FrameStats { std::size_t requests, bytes, fences; };
    [[nodiscard]] FrameStats lastFrameStats() const noexcept;
};

}  // namespace mosaic::render
```

Notes on the shape, each earned by a consumer above:

- **`Freshness` defaults to `AnyRecent`, and `blocking` defaults to false.** A consumer that
  needs synchrony has to type it. That is the whole point: today synchrony is what you get by
  writing `m_lastComposite.rgba[p]`.
- **`peek` / `pinMirror` exist because A1, A2 and A6-adjacent consumers are per-frame and
  synchronous and *tiny*.** Serving them through futures would be architecturally tidy and
  practically a fence per frame. Pinning one macrotile under the pointer costs 256×256×4 = 256 KiB
  of mirror and turns the eyedropper into a memory read.
- **`requestScreenRect` is separate from `request`** because A10 wants *screen* pixels, and
  expressing it as a doc-space rect is what makes it unbounded under rotation.
- **`ReadbackResult::revision` + `stale`** is how A5/A7 opt into staleness *usefully* — they already
  memoize on `m_compositeRevision`, so they can drop a superseded result rather than binning a
  histogram twice.
- **`name` is not optional.** It is the profiler row, the budget report line, and the key of the
  registry in §7 guard (d).
- **What this API deliberately cannot do:** serve off-canvas pixels (B7/B8) or byte-exact CPU-lane
  output (B1–B3). Those consumers keep `render::composite` / `compositeRegion`, and that is a
  *decision*, not an omission — it should be stated in the S60-a commit so nobody "finishes the
  job" by routing them through readback later.

---

## 7. How to not regress this

> **⚠ 2026-07-29 — IT REGRESSED, ON THE FIRST INTERACTIVE RUN, AND §7's honest evaluation below is
> why the guard that would have caught it did not exist.** The resident lane shipped behind its
> opt-in measuring 87× faster than the CPU walk on `--bench`, and was *slower than the CPU walk* in
> the app: 25–30 ms spikes on every resident row. Cause: `refreshMirror()` ran on the frame path,
> memoised on the accumulator revision — a memo that never hits during an edit, because that is
> when the revision moves every frame. One held cursor-readout pin (the pointer over the canvas)
> put a transfer **and a fence** on every frame of every stroke. `materialise()` had the same memo
> over a blocking full-canvas readback that the histogram asks for with `AnyRecent`.
>
> **The rule is now in the class, not in the callers:** `refreshMirror()` refuses while a gesture
> is active **or** while the revision moved since the previous frame. The second predicate is the
> one §7 could not have guessed at — **typing is not a pointer gesture**, so a gesture-only guard
> still fences once per keystroke. `materialise()` applies the same predicate to anything that did
> not ask for `Current`. A pin being *taken* still seeds itself: that is an explicit consumer act,
> not the frame path, and `peek` after `pinMirror` has to work.
>
> Point 3 below — "it has almost no coverage where it matters" — was exactly right and is the part
> to internalise: **`--bench` cannot see this class of defect at all.** A benchmark that composites
> in a loop has no cursor, no pointer and no open panel, therefore no pinned mirror and no
> consumer. The gate measured the lane, correctly, while the whole win leaked out through something
> the app hangs off the lane. `tests/test_composite_readback.cpp` now pins the invariant directly
> (zero bytes on a gesture frame, zero on an editing frame, stale-refuses-for-`peek`,
> stale-still-serves-for-a-lag-tolerant-`request`, recovery on the first quiet frame), which is the
> coverage §7 said an assert could never get.

The plan proposes: *"assert in debug that no readback happens during a gesture frame."*

**Honest evaluation: as literally stated, that assert is both unimplementable-as-intended and
wrong.** Three reasons:

1. **There is no "gesture" flag to assert on.** The nearest things are
   `VulkanCanvas::transformGestureActive()`, `activeDragLayer() != kInvalidLayerId`,
   `textBlockEditGestureActive()`, the brush-stroke state, and `Fl::pushed() != nullptr`. A
   `gestureActive()` aggregate would have to be written. That part is genuinely easy and worth
   doing anyway — several call sites already assemble their own subset by hand
   (`app_window.cpp:9575`, `9664`).
2. **It would fire on correct code.** The eyedropper *drags* (`VulkanCanvas::dragEyedropper`,
   `src/ui/vulkan_canvas.cpp:666`) and the status-bar readout updates on FL_DRAG. Both are
   synchronous per-event composite reads **during a gesture**, and both are right. An assert that
   forbids them forbids the feature.
3. **It has almost no coverage where it matters.** Asserts only fire on exercised paths, and the
   headless verification set (`ctest`, `--gui-frames N`) drives **no gestures at all**. A debug
   assert would sit unexecuted in CI and fire, at best, on the developer's own machine while they
   happen to be dragging the right tool.

So: keep a narrowed version of the assert, but do not let it be the guard. Four things, in
increasing order of how much they actually protect:

**(a) A per-frame readback budget, in the profiler, in release.** `CompositeReadback` records
`requests / bytes / fences` per frame (`FrameStats` above) and feeds them to `common::Profiler` as
first-class rows. S60-α already made collection release-available behind `--profile`
(plan §7 item 6), so this is free. A new consumer then shows up as *a number that moved*, in the
build the user runs. **This is the one that would actually have caught A10 and B6.**

**(b) `--bench` reports readback bytes per frame as a column.** Plan §8.2 already specifies
`paint-stroke`, `move-fullcanvas`, `type-keystroke`, `opacity-drag`. Give each scenario a
synthetic gesture and print `readback_bytes/frame` next to `ms/frame`, with a recorded baseline.
"A session that does not move its scenario is not done" becomes "a session that moves
`readback_bytes/frame` off zero has to explain itself in the commit message". This is a CI-visible
number, which the assert is not.

**(c) Delete the raw accessor.** `m_lastComposite` becomes private with **no** getter; the only
route to composited pixels is `CompositeReadback`, whose every entry point demands a `name` and a
`Freshness`. A careless new consumer then cannot be written *at all* without declaring itself. This
is the only guard that addresses the actual failure mode identified in §2.2 — that a provider
lambda is invisible to grep. **Do this in the same commit that lands the API.**

**(d) A pinned consumer registry + a unit test.** `CompositeReadback` records the set of `name`s it
has ever been asked for; a test asserts that set equals a literal list checked into
`tests/test_composite_readback.cpp`, with a comment pointing here. Adding a consumer without
updating the list fails `ctest`. Cheap, durable, and it keeps *this document* honest, which is the
thing most likely to rot. (Registration has to happen at request time rather than at startup, so
the test drives each consumer at least once — which is worth having regardless.)

**(e) The narrowed assert, for what it is worth.** In debug:

```cpp
assert(!(req.freshness == Freshness::Current && req.blocking &&
         gestureActive() && area(req.roi) > kOneMacrotile) &&
       "a blocking full-canvas readback during a gesture — pin a mirror instead");
```

That is implementable, correct (it permits A1/A2's small pinned reads and forbids the thing we
actually fear), and near-useless in CI. Ship it, rely on (a)–(d).

---

## 8. Findings

Read-only audit; **nothing below was fixed**. Reported per the task's constraint.

**F1 — `m_lastComposite` is not maintained during a GPU-resident Move drag.** When
`m_gpuDragActive` (`src/ui/app_window.cpp:9201`), `driveTransformPreview` takes the GPU branch and
never reaches `recompositeNow`, so `m_lastComposite` holds pre-gesture pixels for the whole drag
(`9593` is the only full-frame writer). For the duration: the status-bar colour readout (A1), the
eyedropper's All-Layers source (A2), the Channels histogram (A5) and the Smart-Resize map (A7) all
report the document as it was before the gesture started. `syncAfterEdit` on drag end (`905`)
restores coherence. Practically invisible — nobody eyedrops mid-drag — and I am recording it not as
a defect to fix but because **it is the existing precedent that these four consumers already
tolerate whole-gesture staleness**, which is direct evidence for the dispositions in §3.

**F2 — a region recomposite never re-bins the Channels histogram, even when the tab is visible.**
`patchComposite` (`9689`) bumps `m_compositeRevision` at `9693` but does not call
`ChannelsPanel::notifyChanged`; only `recompositeNow` does (`9598`–`9600`). So brush strokes,
typing, inpaint previews and undo-of-a-scoped-edit leave a *visible* histogram stale until the tab
is hidden and re-shown (`onTabShown`, `src/ui/channels_panel.cpp:374`). Probably deliberate — a
full-canvas re-bin per dab would be brutal — but nothing says so, and the comment at
`src/ui/channels_panel.hpp:126`–`127` is now inaccurate: it claims `onTabShown` catches "edits …
that did not bump the revision", whereas region patches *do* bump it. S60-a should make this an
explicit staleness policy rather than an accident of which writer calls which hook.

**F3 — `ensureSmartAnalysis`'s stale fallback is unmemoized and sits behind a per-frame caller.**
`src/ui/app_window.cpp:8421`. When `m_lastComposite` is stale (`8424`–`8426`) the function runs a
**full CPU composite** (`8434`) plus a **full-canvas importance-map build** (`8440`), and line `8445`
deliberately does not advance `m_smartMapRev` — so the next call repeats both. The caller
`VulkanCanvas::refreshSmartChips` runs **every frame** from `syncSmartChips`
(`src/ui/vulkan_canvas.cpp:2206`, inside `renderFrame`) while the Crop tool is up with Smart Resize
on. Normally the next frame's recomposite clears the staleness, so this is a one-frame event. But
`recompositeNow` early-returns on a composite failure (`9587`–`9590`) **without touching
`m_lastComposite`**, and the canvas-size guard is a size comparison — so any state that keeps the
composite stale across frames turns this into two full-document passes per frame with no guard and
no log line. Not fixed; flagged because S60-a will be re-plumbing exactly this call.

**F4 — `wandMergedSource` has the same shape, behind a strictly worse caller.** ✅ **FIXED
2026-07-29.**
`src/ui/app_window.cpp:8282`; the stale branch at `8288`–`8294` is a full CPU composite into a
caller-owned scratch. Its caller `eyedropperSample` (`8271`) is invoked from
`VulkanCanvas::syncLoupe` (`src/ui/vulkan_canvas.cpp:690`) **every frame** while the Eyedropper is
active with Source = All Layers. Same exposure as F3, on a hotter path, with no memo at all (the
scratch is a local, so even a *successful* stale composite is thrown away and redone next frame).

**How it was fixed** (and what the fix does *not* claim):

- The scratch is gone. The fallback composites into `MainWindow::m_mergedSource`, **memoised on
  `m_compositeRevision`** — the same token `ensureSmartAnalysis` uses, and the same one
  `bumpCompositeRevision` is the single funnel for. A hover with no edits pays for **one** composite
  and then nothing; a view change (pan/zoom/rotate) does not invalidate it, because the image is
  document space and a view change moves neither the revision nor the document.
- The memo is **refused while a recomposite is queued and not yet drained**
  (`m_recompositePending || m_pendingRegion.queued`). That is the window in which the document has
  already changed and the revision has not caught up. Invisible in a live readout; not invisible in
  the Magic Wand, which floods an *undoable* selection off these same pixels. A pass made in that
  window does not stamp the revision, mirroring `ensureSmartAnalysis`'s refusal to stamp its own
  transient fallback.
- `bumpCompositeRevision` **releases** the pixels as well as invalidating them: a document-sized
  RGBA8 image is 160 MB at 5000×8000, and it must not be resident between edits. It is only ever
  populated on the degraded path, so this is one empty-check per dab in the ordinary build.
- The readout dropped to `AnyRecent`; only the commit still asks `Current`. See A2.

**What this does NOT establish.** The fix removes a provable per-frame full-canvas composite. It has
**not** been shown to be the cause of the user's report that the loupe "temporarily disengages" when
Source is switched to All Layers — that needs a runtime probe, and note that in the **default build
the fallback never runs at all** (`m_tiles == nullptr`, so `hostComposite` returns the CPU-maintained
`m_lastComposite`, which is never stale while a document is open). The fallback is reachable only
under `MOSAIC_TILE_COMPOSITOR=1`, where `materialise()` refuses when the lane is not serving and
`TileCompositor::readback()` can fail outright — see F6.

**F5 — the 3D reflect-env rebuild costs two full composites in one frame.**
`refreshLayerReflection` (`9339`) calls `buildBelowComposite` (`9364`, a full composite), and its
caller `updateReflectionEnv` (`9317`) then calls `recompositeNow(false)` (`9336`) — a second full
composite — in the same `onFrame` turn (`9921`), immediately after `onFrame` may already have run
one at `9874`. Bounded by the 0.30 s settle, so at most ~3/s, but on a 5000×8000 document that is
three full-canvas CPU composites in one frame. Not fixed; listed because it strengthens B6's
disposition (the env should come off the resident accumulator, not off a fresh walk).

**F6 — a full-canvas `readback()` allocates the whole covering macrotile set at the WORKING format,
in one host-visible buffer, and can simply fail.** Read-only finding, 2026-07-29, **not fixed** (it
is in `src/render/tile_compositor.cpp`, outside this change's ownership).
`TileCompositor::readback` (`:3503`) sizes its staging buffer as
`macrotile² × workingFormatBytes() × tilesCovering(roi).count()` (`:3535`–`:3540`). For a whole-canvas
roi on a 5000×8000 document at a 256 px macrotile that is 20×32 = 640 tiles × 256×256 × **8 bytes**
(fp16 RGBA) = **~335 MB of host-visible memory in a single `makeHostBuffer`** — for a result image of
160 MB. On a system without resizable BAR the host-visible heap is commonly 256 MB, so this can fail
outright, and `makeHostBuffer` failing is the *only* thing between it and a caller.

Why it matters here rather than as a footnote: `ResidentComposite::materialise` (`src/ui/
resident_composite.hpp:520`) logs a warning on failure and leaves the mirror **empty**, and does not
advance `m_mirrorRevision` — so it retries on the very next call. Before the F4 fix, the consumer
downstream of that (`wandMergedSource`) then ran a **full CPU composite**, also every call, also per
frame. A failing readback and a per-frame consumer compose into: one failed 335 MB allocation + one
warning line + one full-document CPU walk, *per frame*, for as long as the pointer hovers with
Source = All Layers. The F4 memo removes the third term; the first two remain.

**This is the strongest reading-only candidate for the user's "the loupe temporarily disengages"
report**, and it is exactly the kind of thing `--bench` cannot see (§7's own lesson). It needs a
runtime probe to confirm: run with `MOSAIC_TILE_COMPOSITOR=1` on a large document, hover with the
Eyedropper on All Layers, and look for `resident compositor: readback for "eyedropper" failed` in the
log. The structural fix is for `readback()` to stage **per macrotile row** (or per tile) into a
bounded, reused buffer rather than one allocation proportional to the document.

---

## 9. Summary for S60-a

- **19 consumers**, not five: 11 direct readers of `m_lastComposite`, 8 independent compositor walks.
- **6 are dangerous** (per-frame and/or full-canvas and/or strictly synchronous): the eyedropper
  loupe, Smart Resize chips, the adjustment-panel fade, the 3D reflect env, channel isolation, the
  cursor readout. **Three of those six are not in the plan's list.**
- **2 disappear** under residency (the GPU-drag `below` texture; the recompose-review restore) and
  **2 more should move into the present pass** (channel isolation; the panel fade's under-image).
- **5 must keep their CPU walk** and must be recorded as such, not "finished later": the three
  export/save paths (byte-determinism) and the two modal preview panes (off-canvas pixels).
- The load-bearing mechanism is not `requestReadback` — it is **`pinMirror` + `peek`**. Four of the
  six dangerous consumers want a handful of pixels *right now, every frame*, and a futures API
  serves those with a fence.
- The load-bearing *guard* is not an assert — it is **deleting the raw accessor** so that every
  future consumer has to name itself and declare a freshness.

---

## 10. The routing table (S60-a item 12, 2026-07-28)

§§1–9 are the audit. This section is what was built from it and where every consumer goes.

### 10.1 The seam

`src/render/composite_readback.{hpp,cpp}` — FLTK-free, Vulkan-free at the interface, borrowing a
`render::TileCompositor`.

| Entry point | Serves | Cost |
|---|---|---|
| `pinMirror(roi, name) -> MirrorPin` | keep a rect's macrotiles CPU-mirrored, RAII, refcounted | one transfer per pinned macrotile per composite that changed something |
| `peek(docPt)` / `peekRect(roi)` | the per-event, per-frame consumers | a hash lookup and an index — **no device contact at all** |
| `refreshMirror()` | called once per composite | no-op when nothing is pinned or the revision has not moved |
| `request(ReadbackRequest) -> future<ReadbackResult>` | the whole-canvas, once-per-action consumers | a transfer, unless the roi is already mirrored |
| `requestScreenRect(screenPx, name)` | A10 only | via an installed provider; **named refusal** when there is none |
| `consumers()` | the §7(d) registry | — |
| `beginFrame()` / `lastFrameStats()` | the §7(a) per-frame budget: `requests / bytes / fences / mirrorRefreshes` | — |

Three shapes are deliberate and should not be "tidied":

- **`Freshness` defaults to `AnyRecent`, `blocking` to false.** Synchrony has to be typed. Today it
  is what you get for writing `m_lastComposite.rgba[p]`, which is how this became a per-frame
  full-canvas cost in two places nobody intended.
- **`peek` MISSES rather than fetching.** It has no "fetch it if absent" branch and must never grow
  one: that branch is a fence, on the pointer path. A readout that blinks off for one frame beats a
  readout that stalls the gesture.
- **A partial mirror is a miss.** `peekRect` returns nullopt unless *every* covering macrotile is
  mirrored — a half-served eyedropper window is a wrong colour, not a partial one.

⚠ **`request()`'s future is already satisfied when it returns.** The shape is the contract, not yet
the mechanism; S60-c (composite off the UI thread) is the first point at which a deferred fence has
anywhere to live, and nothing about a caller changes when it does.

### 10.2 Every consumer, and where it goes

Positions re-verified 2026-07-28 against `src/ui/app_window.cpp` unless noted. **All 19 still
exist; none was added.** Names in the `route` column are the checked-in constants in
`render::consumers` — a consumer cannot name itself off the books.

| # | Consumer | Now at | Route | Freshness |
|---|---|---|---|---|
| A1 | status-bar cursor colour (`onCanvasCursor`) | `:9318`, reads `:9334`–`:9344` | `pinMirror` + `peek`, `kCursorReadout` | Current, never a fence |
| A2 | eyedropper + loupe — **live readout** (`eyedropperSample` ← `syncLoupe`) | `:10930`, source `:10980`; host `:1367` | `hostComposite`, `kEyedropper`; the All-Layers fallback walk is memoised on `m_compositeRevision` | **AnyRecent** |
| A2c | eyedropper — **commit** (`eyedropperSample` ← `pushEyedropper`/`dragEyedropper`) | `:10930`; host `:1372` | same funnel, `kEyedropper` | **Current** |
| A3 | magic wand, all layers (`magicWandClick`) | `:8346`, source `:8368` | `request` whole canvas, blocking, `kMagicWand` | Current |
| A4 | edge select brush grow (`edgeBrushGrow`) | `:8392`, source `:8408` | `request` whole canvas, blocking, `kEdgeBrush` | Current |
| A5 | Channels histogram | provider `:824`; `channels_panel.cpp:424` | `request`, `kHistogram`; **GPU-side in S60-e** | AnyRecent |
| A6 | on-canvas channel isolation | `:10051`, `:10067`, `:10083` | **not a readback — move the remap into `canvas_present.comp`** | n/a |
| A7 | Smart Resize map + chips (`ensureSmartAnalysis`) | `:8693`, stale walk `:8708` | `request`, `kSmartResize`, memoised on `revision` | AnyRecent |
| A8 | Smart Recompose seed (`startRecompose`) | `:4815`, copy `:4825` | `request` whole canvas, blocking, `kSmartRecompose` | Current |
| A9 | crop-expansion inpaint seed (`startCropExpandFill`) | `:4679`, blit `:4694` | `request` whole canvas, blocking, `kCropExpandFill` | Current |
| A10 | adjustment-panel fade under-image | lambda `:1561`, fade `:5762` | `requestScreenRect`, `kPanelFade` | Settled |
| A11 | recompose-review restore | `:4972` | **DROPPED** — the accumulator still holds the document; leaving review is "stop showing the preview", not "re-upload" | n/a |
| B1 | Quick Export PNG (`quickExportPng`) | `:4231` | **keeps `render::composite`** | — |
| B2 | Export JPEG/JXL/As… (`compositeForExport`) | `:4261` | **keeps `render::composite`** | — |
| B3 | `.mosaic` PRVW thumbnail (`compositeForPreview`) | `:3105` | **keeps `render::composite`** | — |
| B4 | Edit ▸ Copy Merged (`copySelection`) | `:6827` | `request` the SELECTION's bbox, blocking, `kCopyMerged` | Current |
| B5 | GPU-drag `below` texture (`buildBelowComposite`) | `:9538` | **DELETED by residency** — the static backdrop is the set of macrotiles the dragged layer does not dirty | n/a |
| B6 | 3D-text reflect env (`refreshLayerReflection`) | `:9658`, via `buildBelowComposite` `:9683` | `request`, `kReflectEnv`; the 0.30 s settle already tolerates it | Settled |
| B7 | Fill-dialog preview (`fillCompositePreview`) | `:6076` | **keeps `render::compositeRegion`** — `clampToCanvas=false` | — |
| B8 | Layer-Effects preview (`layerEffectsPreview`) | `:6113` | **keeps `render::compositeRegion`** — `clampToCanvas=false` | — |

**The five that keep the CPU walk are a decision, not an omission.** B1–B3 need byte-determinism
against the fp32 reference that an fp16 device accumulator cannot promise — a PRVW thumbnail that
wobbles in the low bit re-emits its chunk on every save and grows the file for nothing. B7/B8 need
pixels *outside* the canvas, which the accumulator does not and will not have. The registry test
asserts those five names are **absent** from the vocabulary, so "finishing the job" later fails
`ctest` instead of quietly breaking export.

### 10.3 What the re-verification changed

- **F2 IS FIXED, and A5 got hotter.** A new `MainWindow::bumpCompositeRevision` (`:10012`) is now
  the single revision-bump funnel that *both* writers call, and it notifies the Channels panel — so
  a region patch re-bins the histogram, which §3 A5 says never happens. Its cadence is now
  "every revision bump while the tab is visible", i.e. potentially per brush dab. That makes the
  `AnyRecent`/never-synchronous disposition **more** load-bearing, not less. `channels_panel.hpp`'s
  comment about `onTabShown` is now doubly inaccurate.
- **B5's 1×1-transparent shortcut gained a guard** (`:9531`): it now also requires the layer's
  parent to be the root, fixing a mis-fire for a layer nested in a group under a single-child root.
- **F3 and F5 are still live and unfixed**, now with profiler rows that cite this document by
  name (`:8695` Smart Resize, `:9652` reflect env). Instrumented, not repaired. **F4 was repaired on
  2026-07-29** (memo + `AnyRecent` readout); **F6 is new and unrepaired**, and it is the reason F4
  mattered in practice rather than only on paper.
- **`src/app/bench.cpp` is a new harness caller** (11 `render::composite` sites), in the same
  category as `src/app/main.cpp`'s two. Not a UI consumer; listed so the count reconciles.
- **All 11 `app_window.cpp` composite sites still pass `render::Backend::Cpu`** — `3105, 4231,
  4261, 6076, 6113, 6827, 8562, 8708, 9538, 9911, 9986`. That is item 13, and it is withheld.
- **No new provider lambda reads the composite.** Two new hosts exist (`setShapeToolHost`,
  `setGradientToolHost`); neither closes over `m_lastComposite`.

### 10.4 What still has to happen in `src/ui` (item 13's other half)

1. `m_lastComposite` becomes private with **no getter** — guard §7(c). Every consumer above then
   goes through `CompositeReadback`, which demands a name and a freshness.
2. A1/A2 hold a `MirrorPin` for as long as their tool is active, and drop it on tool change. A pin
   held forever is a leak of exactly one macrotile's worth of mirror, which is survivable; a pin
   never taken is a per-frame fence, which is not.
3. `CompositeReadback::beginFrame()` once per `onFrame`, and `refreshMirror()` wherever the dirty
   set is cleared — the same place, so the two cannot diverge.
4. A6's channel remap moves into `canvas_present.comp`. That also deletes the exception at
   `app_window.cpp:9192` where channel isolation disables the GPU drag fast path.
5. `CompositeReadback::setScreenCapture(...)` is wired to a `WindowRenderer` screen-rect capture,
   which does not exist yet. Until it does, A10 gets a named refusal rather than a silent fallback
   to the unbounded document-space gather.

### 10.5 Guard §7(a) is now half-built (2026-07-28)

§7 ranked four guards and said **(a) a per-frame readback budget, in the profiler, in release** was
"the one that would actually have caught A10 and B6". The profiler half of it now exists:

- `TileCompositor::readback()` and `readTarget()` each carry a `MOSAIC_PERF_SCOPE` — rows
  **`Tile readback (accumulator)`** and **`Tile readback (8-bit target)`**, `Lane::Gpu`, present in
  every build behind `--profile` / `MOSAIC_PROFILE=1`. A readback that starts happening per frame
  is now **a row whose `count` tracks the frame count**, in the build the user runs. That is the
  shape §7(a) asked for, and it required no new mechanism because the collector moved to
  `src/common` in S60-a.
- The rest of the lane is instrumented too, which matters here for a second-order reason: the
  device-time rows (`Lane::GpuDevice`, from `render::GpuTimer`) make it possible to tell a slow
  frame caused by a readback apart from one caused by the kernel. Before, both showed up as a
  larger `Lane::Gpu` number and looked the same.

Still owed from §7(a): `CompositeReadback::lastFrameStats()`'s `requests / bytes / fences /
mirrorRefreshes` are collected but **not yet fed to `common::Profiler` as first-class rows**, and
§7(b)'s `readback_bytes/frame` column in `--bench` does not exist. Both want `beginFrame()` to be
called from `onFrame`, which is item 13's other half (§10.4 item 3) and is not done. Note that the
new `--bench tile-composite` scenario asserts nothing about readback bytes today; adding
`TileCompositeStats::readbackBytes` as a column there is the cheapest way to close §7(b), and it
can be done without touching `src/ui` at all.
