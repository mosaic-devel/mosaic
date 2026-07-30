# S60 — The gesture-start stall: `buildBelowComposite`

**Status:** DESIGN (2026-07-23). Analysis only — **no code was changed by this document.** Line
numbers are from the worktree at `7e65eed` (S60-a's first commit, `core/tile_grid.hpp`); the
measurements were taken with the release binary built at `a10ab74+dirty`, which predates that
header and touches none of the code below.

**Why this exists.** `--bench move-fullcanvas` reports a `gesture-start` case — the single full CPU
composite that seeds the GPU-resident Move drag — at **0.92 s quiet / 1.34–3.07 s loaded** on a
5000×8000 document. PLAN §2's open Move-lag bug names only the *per-frame* cost and prescribes
S60-c's proxy live composite, which does not touch this. This document confirms the finding, says
exactly where the bench's framing is wrong, enumerates the ways out, and recommends one.

---

## 0. TL;DR

1. **The stall is real and reproducible.** 919 ms quiet, 1337 ms at load 6, ~3.1 s at the load the
   task's table was taken under. It blocks the UI thread inside an FLTK timeout: no redraw, no
   cursor, no event processing.
2. **It is not unconditional, and it is not on mouse-down.** It fires on the *first drag frame past
   the 3 px dead zone*, only when `render::canUseGpuDrag` says **yes**, and only when the root has
   ≥2 top-level children. When the fast lane says *no*, the stall does not happen at all — you pay
   the per-frame CPU cost instead. **The two costs in the bench table are alternatives, not a sum.**
3. **The bench measured the smaller of the gesture's two stalls.** Gesture *end* pays a full CPU
   walk plus a full-canvas upload — `full-walk int`, 1712 ms quiet / 2588 ms at load 6 / 4672 ms at
   the task's load — and nothing in PLAN §2, `docs/s60-performance-plan.md` or `bench.cpp` names it.
   A user who nudges a full-canvas layer ten times pays **twenty** multi-second freezes.
4. **Recommendation:** bound the backdrop by *what is on screen* — composite the visible document
   rect into a screen-sized buffer — on one new `render::compositeScaled` entry point. Ship the
   whole-document-at-a-bounded-scale variant first: **it needs no shader change at all**, because
   `canvas_drag_composite.comp` already samples `uBelow` normalised (line 90), which the existing
   1×1 shortcut proved. Estimated ~0.5 session for the scaled variant, ~1 for the viewport-bounded
   one. Expected: 919 ms → ~30–80 ms quiet, ~100–260 ms loaded.
5. **It does not disappear for free once residency lands** — it disappears at the *end* of S60-a
   (items 11–13), which `docs/s60-performance-plan.md` §9 explicitly reserves the right to withhold.
   That is precisely why a bounded backdrop is worth building now, and the primitive it needs
   (`compositeScaled`) is reusable by S60-d's proxy and by the 3D reflect env (readback-audit F5).

---

## 1. Confirm or refute

### 1.1 The trigger chain, exactly

```
FL_PUSH   VulkanCanvas::pushMoveTool          (vulkan_canvas.cpp:1367)   -> beginMoveGesture, NO host call
FL_DRAG   VulkanCanvas::dragMoveTool          (vulkan_canvas.cpp:1477)   -> dead zone (3 px, :152), then
                                                                            record m_moveDragPending only
frame     MainWindow::onFrame                 (app_window.cpp:9914)
          -> m_canvas->flushMoveDrag()        (app_window.cpp:9927)
          -> VulkanCanvas::flushMoveDrag      (vulkan_canvas.cpp:1500)
          -> pushSelectionTransform           (vulkan_canvas.cpp:1519)
          -> MoveToolHost::setTransforms      (app_window.cpp:887-898)
          -> MainWindow::driveTransformPreview(app_window.cpp:9204)
          -> buildBelowComposite              (app_window.cpp:9239)   <-- THE STALL
          -> VulkanCanvas::beginGpuDrag       (vulkan_canvas.cpp:297)
          -> WindowRenderer::beginGpuDrag     (window_renderer.cpp:1714) <-- and a second, unmeasured one
```

Three corrections to the framing fall straight out of this:

**(a) It is not on mouse-down.** A press alone costs nothing; `pushMoveTool` only arms the gesture.
The stall lands on the first *frame* after the pointer has travelled 3 px
(`kMoveDragDeadZonePx`, `vulkan_canvas.cpp:152`). Subjectively this is *worse* than a mouse-down
stall, not better: the user has already committed to a motion, the hand is moving, and the layer
does not follow for three seconds.

**(b) It is not unconditional.** The gate is `app_window.cpp:9213-9214`:

```cpp
if (!m_gpuDragTried && target != core::kInvalidLayerId && !channelIsolationActive() &&
    render::canUseGpuDrag(*m_document, target)) {
```

and `render::canUseGpuDrag` (`compositor.cpp:2151`) requires the target to be the **topmost direct
child of the root**, a **visible RasterLayer**, with **no enabled mask**, **no clip-to-below**, a
**separable blend mode (0..18)** and **no layer effects**. Miss any of those — or have a channel
eye off — and `buildBelowComposite` is never called; `driveTransformPreview` falls to
`requestRecomposite` and the gesture pays the *per-frame* CPU cost instead.

So the bench's four rows describe **four mutually exclusive worlds**:

| the layer you are dragging | what you pay |
|---|---|
| topmost root child, unmasked, separable, no effects, no channel isolation | `gesture-start` **once**, then ~0 per frame (GPU), then `full-walk` **once** at release |
| anything else, top-level, cache-eligible | `drag-cache` **per frame**, no gesture-start |
| nested / unlinked-masked / over budget | `full-walk` **per frame** |

Reading the table as "3.1 s of gesture start *plus* 1.8 s a frame" overstates it; reading
`gesture-start` as "what you pay unconditionally" is simply not what the code does. What *is* true,
and is the useful version of the claim: **the gesture-start stall is the price of the fast lane.**
It is paid exactly when the optimisation applies. That is a worse property than the bench's framing,
not a better one — the fast lane taxes its own admission.

**(c) It is not paid more than once per gesture** — `m_gpuDragTried` is set at `:9215` before the
attempt and cleared only in `MoveToolHost::gestureEnded` (`:904`) and on document switch (`:8511`).
Esc-cancel routes through `pushSelectionTransform` again but finds the flag set. Note the flag is
set **only inside the eligible branch**, so an ineligible layer re-runs `canUseGpuDrag` every frame
— that is a handful of pointer compares, correctly cheap.

But it *is* paid once per **gesture**, and a gesture is press → 3 px → release. Nudging a layer into
place is ten gestures. At the task's loaded numbers that is 10 × (3.07 + 4.67) ≈ **77 seconds** of
frozen UI to move something ten times.

### 1.2 What the 3 s is actually spent on

`buildBelowComposite` (`app_window.cpp:9239-9264`) does five things:

1. `MOSAIC_PERF_SCOPE("Drag below-composite", Lane::Cpu)` — release-available since S60-α.
2. The 1×1 shortcut (`:9250-9251`): if the root has ≤1 top-level child, return `common::Image(1,1)`.
   This is a real and good optimisation — it is why single-layer documents do not stall — and it is
   also load-bearing evidence for the fix (§3.2).
3. `l->setVisible(false)` … composite … `l->setVisible(wasVisible)` (`:9253-9260`). An unobserved,
   uncommanded mutation of the document to express "composite without this layer".
4. `render::composite(*m_document, opts, render::Backend::Cpu)` with **default**
   `CompositeOptions` — `resampleFilter = Nearest`, `liveDrag = false` — where every display
   composite uses `currentResampleFilter()` (default `Auto`). See Finding **G2**.
5. Return by value: a full document-sized `common::Image` (160 MiB at 5000×8000).

The cost is item 4 and it is not mysterious. Measured throughput of the CPU compositor on this box
(§2): **~61 ms per output megapixel with four layers, ~23 ms/Mpx with one.** A 5000×8000 document
is 40 Mpx. There is no pathology to find — the compositor is doing exactly the work it was asked
for, over 40 million pixels, on the UI thread, inside an FLTK timeout.

**What else is avoidable, beyond the 1×1 case:**

- **It composites the whole document when only a screenful is ever displayed.** At fit zoom on a
  5000×8000 document the canvas widget shows ~1.3 Mpx. 97% of the composited pixels are thrown at
  a texture that will be minified away by the present pass. This is the entire opportunity.
- **It composites at full document resolution regardless of zoom.** Same point, second axis.
- **It is not the only cost in the seam.** `WindowRenderer::beginGpuDrag`
  (`window_renderer.cpp:1714`) then calls `uploadSampledTexture` **twice** (`:1399`), each of which
  allocates a document-sized `R8G8B8A8` image *and* a document-sized host-visible staging buffer,
  `memcpy`s into it, submits a one-shot command and **blocks on `vkWaitForFences`** (`:1507`). At
  5000×8000 that is 160 MiB × 2 of memcpy, 320 MiB of staging, 320 MiB of device images, and two
  synchronous queue round-trips — all inside the same frozen frame, and **none of it under a
  profiler scope** (there is no `MOSAIC_PERF_SCOPE` anywhere in `window_renderer.cpp`). See
  Finding **G5**.
- **It is thrown away at release.** The below composite is not reused by the gesture-end
  recomposite, which walks the whole stack again from scratch (§1.3).

### 1.3 The part the bench did not measure: gesture END

`MoveToolHost::gestureEnded` (`app_window.cpp:899-905`) disarms the GPU pass and calls
`syncAfterEdit` (`:9862`), which does `m_dragCache.invalidate()` then
`requestRecomposite(false)` → next `onFrame` → `recompositeNow` (`:9602`). By then
`VulkanCanvas::endMoveGesture` has already run `m_transform.cancel()` (`vulkan_canvas.cpp:1558`),
so `activeDragLayer()` is invalid and the drag cache is not even attempted — and it was never built
in the first place, because the whole drag ran on the GPU and `recompositeNow` was never called.

So release pays **the full walk**, plus `presentComposite` → a full 160 MiB canvas upload:

| | quiet | load ≈ 6 | task's load |
|---|---|---|---|
| `gesture-start` (5000×8000) | 919 ms | 1337 ms | 3068 ms |
| `full-walk int` (5000×8000) ≈ gesture end | **1712 ms** | **2588 ms** | **4672 ms** |

**The gesture-end stall is roughly 1.5× the gesture-start stall**, it is unconditional given that
the fast lane armed, and it appears in no plan document. Any fix that addresses only gesture start
converts *freeze → move → freeze* into *move → freeze*. That is a real improvement and it is half
the problem.

(Gesture end is arguably worse than `full-walk int` suggests: that bench row runs with
`liveDrag=true` and an integer translation, so `Auto` resolves to the lossless Nearest kernel. A
committed rotate or sub-pixel placement resolves to **Lanczos3** at `liveDrag=false`.)

### 1.4 Verdict

**Confirmed, with the framing corrected.** The stall exists, is reproducible, and is the price of
admission to the GPU fast lane rather than an unconditional tax. The bench's *conclusion* — that
S60-c's prescribed proxy live composite does not cover what the user feels — is correct and
**understated**: the prescription covers neither of the two stalls that bracket the gesture.

---

## 2. Measurements

All from this box: 8 cores, 31 GiB, release build `0.2.17+ga10ab74-dirty` (GNU 16.1.1), CPU backend,
`--bench-iterations` as noted. Medians. **Loaded is the headline** per project discipline; my
"loaded" is eight spinners (load average ≈ 6), lighter than the load the task's table was taken
under (≈ 3.3× quiet, versus my ≈ 1.45×), so both are given.

### 2.1 `--bench move-fullcanvas`

| case | quiet (n=7) | load ≈ 6 (n=5) | task's table |
|---|---|---|---|
| 1920×1080 drag-cache int | 38.7 ms | 70.4 ms | — |
| 1920×1080 drag-cache subpx | 43.2 ms | 83.1 ms | — |
| 1920×1080 full-walk int | 74.5 ms | 116.2 ms | — |
| 1920×1080 gesture-start | 32.0 ms | 46.7 ms | — |
| 5000×8000 drag-cache int | 726.3 ms | 1082.3 ms | 1755 ms |
| 5000×8000 drag-cache subpx | 812.8 ms | 1082.9 ms | 2260 ms |
| **5000×8000 full-walk int** | **1712.2 ms** | **2587.8 ms** | **4672 ms** |
| **5000×8000 gesture-start** | **918.7 ms** | **1336.7 ms** | **3068 ms** |

The shapes agree; only the load multiplier differs. The finding reproduces.

### 2.2 The compositor's throughput — the number that decides every candidate

`--bench composite-full`, quiet, n=5, four layers:

| canvas | Mpx | median | ms / Mpx |
|---|---|---|---|
| 512×512 | 0.26 | 10.0 ms | 38.4 |
| 1920×1080 | 2.07 | 126.4 ms | 61.1 |
| 3840×2160 | 8.29 | 499.6 ms | 60.3 |
| 5000×8000 | 40.0 | 2444.6 ms | 61.1 |

**Linear in output pixels above ~2 Mpx, at ~61 ms/Mpx for four layers on eight cores** (≈ 15 ns per
pixel per layer). The `gesture-start` case composites one visible layer and lands at 23 ms/Mpx.

Two consequences that constrain everything in §3:

- **Cost is a function of *output* pixels, not document pixels.** Every "make the output smaller"
  candidate wins proportionally to area, i.e. quadratically in a linear scale factor.
- **A screenful is the floor.** A 1400×900 canvas widget is 1.26 Mpx → **29 ms quiet (1 layer
  below) to 77 ms quiet (4 layers below)**; ~100–260 ms at the task's load. No CPU-side fix can
  beat that, because the pixels have to be produced somewhere. Only residency (§3.6) gets to zero.

---

## 3. The candidates

### 3.1 Asynchronous below-composite, show something until it lands

Move `render::composite` to a worker; arm the GPU pass when it finishes.

- **Cost:** the composite reads the live document while the UI thread is pushing a
  `SetTransformsCommand` every frame. Worse, `buildBelowComposite` *mutates* the document
  (`setVisible(false)`, `:9253`) to express "without this layer". Off-thread, that is a data race
  against the present path, `reflectStackFingerprint` (which mixes `visible()`), and the layer
  panel. A prerequisite is a `CompositeOptions::skipLayer` (or `skipLayers`) so the exclusion is a
  read-only parameter of the walk. That is a small, clean, independently good change.
- **What the user sees meanwhile:** either the layer does not follow the cursor for 3 s (arm late),
  or the backdrop is missing — transparent checkerboard where the document used to be — for 3 s
  (arm now with a 1×1 backdrop). Both are bad for three seconds.
- **Verdict: not a fix on its own, a *finisher*.** Async converts a freeze into a wrong picture of
  the same duration. It is worth having *after* the work is bounded (§3.2), as the mechanism that
  refines a proxy backdrop to full resolution without a hitch.

### 3.2 A proxy / low-res below-composite, upscaled ⭐

Composite the whole document into a `docW/k × docH/k` buffer and hand that to `beginGpuDrag`.

**The shader already supports this with zero changes.** `canvas_drag_composite.comp:90`:

```glsl
vec4 below = texture(uBelow, (vec2(pos) + 0.5) / pc.docSize);
```

`uBelow` is a `sampler2D` sampled in **normalised** coordinates with a linear sampler. The comment
above that line says so explicitly, because the 1×1 shortcut already relies on it. A `below` texture
of any resolution therefore Just Works and is bilinearly upsampled by the hardware.

- **Cost to build:** one new public entry point. `compositor.cpp:2015` already has
  `compositeBuffer(doc, pre, w, h, opts, backend)` taking an arbitrary affine and output size —
  `compositeRegion` is a translation-only caller of it. A scaled composite is
  `compositeBuffer(doc, Affine2D::scale(1.0/k), docW/k, docH/k, opts, backend)`. Call it
  `render::compositeScaled(doc, outW, outH, opts, backend)`.
- **Anti-aliasing is free.** `renderLayerRaw` chooses the kernel from the *composed* placement
  (`resolveFilter(rs.filter, place, rs.liveDrag)` where `place = pre * layer.transform()`,
  `compositor.cpp:1805-1807`). A 1/k `pre` is a minification, so `Auto` resolves to **Area** — a
  proper box filter, not point sampling. This is the difference between a proxy that looks right
  and one that crawls with aliasing, and it costs nothing.
- **Numbers** (quiet, one layer below; multiply by ~1.45 for my load, ~3.3 for the task's):

  | k | proxy size | Mpx | est. | upload |
  |---|---|---|---|---|
  | 1 (today) | 5000×8000 | 40.0 | 919 ms | 160 MiB |
  | 2 | 2500×4000 | 10.0 | ~230 ms | 40 MiB |
  | 4 | 1250×2000 | 2.5 | ~58 ms | 10 MiB |
  | 8 | 625×1000 | 0.63 | ~14 ms | 2.5 MiB |

- **What the seam looks like.** The *dragged* layer stays sharp (it is uploaded at full resolution
  and sampled through its own transform); the *backdrop* softens. Choose `k` from the view so the
  proxy is never coarser than the screen: `k = clamp(floor(1 / zoom), 1, kMax)`. At fit zoom on a
  5000×8000 document (≈ 0.28×) that is k = 3, and the softening is **literally invisible** — the
  present pass was going to minify by 3.5× anyway. The visible failure mode is only reached by
  picking `k` blind: then the backdrop blurs at gesture start and snaps sharp at release, which is
  a very noticeable "the picture breathed".
- **The catch: it does nothing when zoomed in.** At zoom ≥ 1, k = 1 and you are back to 919 ms —
  even though only a screenful is visible. That is the whole argument for §3.3.
- **Bonus:** `k ≥ 2` brings the `below` texture under `maxImageDimension2D`'s Vulkan-1.0 guaranteed
  4096 for this document (2500×4000). It is the *only* one of the three drag textures we are free
  to shrink.
- **Verdict: yes, and it is the smallest possible diff.** Ship it first.

### 3.3 Bound it by the viewport as well ⭐⭐

Composite the **visible document rect** into a **screen-sized** buffer: cost becomes O(canvas widget
area), constant in both document size and zoom.

- **Cost to build:** the same `compositeBuffer` call with a `pre` that is the view transform
  restricted to the visible rect; plus **~4 lines of shader** and 16 bytes of push constant to carry
  the backdrop's doc-space rect (`vec4 belowRect`), turning line 90 into
  `texture(uBelow, (vec2(pos) + 0.5 - belowRect.xy) / belowRect.zw)`. The push block is currently
  **48 of the 128 guaranteed bytes** (`canvas_drag_composite.comp:24-35`), so there is room.
- **Plus a new invariant:** *the currently visible doc rect of the canvas texture is correct.*
  Outside `belowRect` the dispatch must skip its store, leaving the pre-drag composite (with the
  dragged layer's ghost at its old position) in the canvas texture. That is invisible until the view
  changes — and the view **can** change mid-drag: `FL_MOUSEWHEEL` is handled unconditionally at
  `vulkan_canvas.cpp:7175` and is not gated on `Fl::pushed()`. So a view change must rebuild the
  backdrop and re-dispatch. There is already a `notifyViewChanged` seam to hang that on, and a
  rebuild now costs ~30–80 ms, so rebuilding is affordable.
- **Numbers:** 1.26 Mpx regardless of document or zoom → **29 ms (1 layer) to 77 ms (4 layers)
  quiet**, ~100–260 ms at the task's load. Backdrop is **exact** at the displayed resolution, at
  every zoom. No blur, ever, so §3.2's one visual risk disappears.
- **Verdict: this is the right shape.** It is a strict superset of §3.2 and it is the same principle
  S60-a is built on ("only compute what is visible / dirty"), which is why it is not wasted work.

### 3.4 An incremental, persistent below-composite

Keep the "composite of everything below the topmost root child" alive across gestures and
invalidate it on edits.

- **It is well-defined**, because `canUseGpuDrag` already pins the target to the topmost root child:
  "below the dragged layer" is always "root children [0, n-1)". `DragCompositeCache::m_belowAcc`
  (`compositor.hpp:290`) is literally this buffer already, built per gesture and thrown away.
- **The invalidation key already exists.** `MainWindow::reflectStackFingerprint`
  (`app_window.cpp:9273`) hashes every layer's id, visibility, opacity, blend, transform, content
  revision and mask presence — it was written for the 3D reflect env, which is the *same* cache
  ("the document composited without this one layer"). Excluding the target's own transform gives
  exactly the key a below-cache wants.
- **Cost:** a retained document-sized buffer. As `common::Image` that is 160 MiB at 5000×8000; as
  the `ImageF` accumulator the walk actually produces, **640 MiB**. On the document in PLAN §2 this
  is not a cache, it is a resident allocation the user did not ask for.
- **It does not fix the first gesture** — the one after opening a file, and the one after every
  edit. A first-use stall of 3 s is still a 3 s stall.
- **Verdict: no, not on its own; yes as a cheap multiplier on top of §3.2/§3.3.** Caching a
  *proxy* backdrop (2.5 MiB at k=8) is all upside: the second and subsequent nudges of the same
  layer cost nothing and the memory is trivial. Build it only after the work is bounded, so that
  what is retained is small.

### 3.5 Derive the backdrop from `m_lastComposite` by subtracting the dragged layer

**Mathematically dead for the case that motivated this document.** The composite is source-over with
blend, straight alpha (`canvas_drag_composite.comp:70-81`, mirroring `blend.hpp`):

```
a_o = a_s + a_b (1 - a_s)
c_o = (a_s · mixed + a_b (1 - a_s) · c_b) / a_o ,   mixed = (1-a_b) c_s + a_b · blend(c_b, c_s)
```

To recover the backdrop `(c_b, a_b)` from the composite `(c_o, a_o)` and the source `(c_s, a_s)`:

```
a_b = (a_o - a_s) / (1 - a_s)            -- undefined at a_s = 1
c_b = (c_o · a_o - a_s · mixed) / (a_b (1 - a_s))
```

and for anything but **Normal**, `mixed` itself depends on `c_b`, so the second line is an implicit
equation — exact only for `BlendMode::Normal`.

Three fatal properties, in increasing order of decisiveness:

1. Exact only for Normal (mode 0). `canUseGpuDrag` admits modes 0..18.
2. **Undefined wherever the dragged layer is opaque.** `a_s = 1` ⇒ division by zero.
3. **Quantisation is amplified by `1/(1 - a_s)`.** `m_lastComposite` is 8-bit
   (`app_window.cpp:10097`). At `a_s = 0.9` a ±1/255 composite error becomes ±10/255 in the
   recovered backdrop; at 0.99, ±100/255.

Property 2 is the killer: **the reported bug is a full-canvas opaque layer.** Subtraction is exactly
undefined over the whole canvas in the one case we are trying to fix. It would work for a small
semi-transparent decal — which never stalls anyway.

**Verdict: no. Record the algebra so nobody re-derives it hopefully.**

### 3.6 Do it on the GPU, after S60-a's resident compositor

`docs/s60-readback-consumers.md` §B5 already reaches the right conclusion:

> `below` exists so the GPU has a static backdrop to composite the dragged layer over; a *resident
> tiled compositor already has that* … **This is a consumer that residency deletes rather than
> converts.**

That is correct, and I want to add two things the audit did not have to say:

- **Residency deletes it only if the accumulator keeps a checkpoint below the topmost layer.**
  §3.3 of the performance plan describes macrotile accumulators "blended bottom→top per dirty
  macrotile". A full-canvas layer moving dirties **every** macrotile, so the naive answer re-blends
  the whole stack every frame — fine on a GPU (40 Mpx × 2 layers ≈ single-digit ms) but it means
  "below" is *recomputed*, not *reused*. Keeping a partial-accumulation checkpoint at the
  second-from-top index is what actually makes the gesture free, and it costs one more resident
  accumulator. **This is a design requirement for S60-a item 9/11 that the plan does not currently
  name.**
- **It arrives at the *end* of S60-a.** The plan's own risk register (§9) says: *"keep the flip as
  the last commit of S60-a so it can be withheld."* Items 11 (resident accumulator → present),
  12 (`requestReadback`) and 13 (flip off `Backend::Cpu`) are the last three and the riskiest. The
  gesture-start stall survives everything before them, and S60-a is one header old today
  (`7e65eed`).

**Verdict: yes, eventually, and it is the real fix. It is not a reason to wait.**

### 3.7 Defer: start with no backdrop, fill it in on frame 2

Arm the drag with a 1×1 transparent `below` immediately, build the real one next frame.

- Synchronously on frame 2, this delays the stall by 16 ms and changes nothing.
- Asynchronously, it is §3.1 with the "arm now" choice: the whole document below the dragged layer
  **disappears** for the duration. On a photo edit that is the picture vanishing and a checkerboard
  appearing under a floating layer. It reads as a crash, not as a wait.
- **Verdict: no.** Worth recording because it is the obvious idea and it is worse than the freeze it
  replaces. Once the work is bounded to ~30 ms the question does not arise.

### 3.8 Summary table

| # | candidate | gesture-start on 5k×8k | build | risk | verdict |
|---|---|---|---|---|---|
| 3.1 | async | unchanged duration, non-blocking | medium (needs `skipLayer`) | data race, ugly interim | finisher only |
| 3.2 | proxy, whole doc at 1/k | ~58 ms quiet (k=4) | **small** | blur if `k` is blind; no help zoomed in | **ship first** |
| 3.3 | viewport + display resolution | ~30–80 ms quiet, constant | medium | canvas-texture staleness invariant | **the shape** |
| 3.4 | persistent below | unchanged first time, 0 after | small | 160–640 MiB retained | only over 3.2/3.3 |
| 3.5 | subtract from `m_lastComposite` | — | — | **undefined where `a_s`=1** | **no** |
| 3.6 | S60-a residency | ~0 | large (the arc) | lands last, can be withheld | the real fix |
| 3.7 | defer / no backdrop | unchanged duration | small | looks like a crash | no |

---

## 4. Recommendation

**Build §3.3 — a below-composite bounded by the visible rect at display resolution — on a new
`render::compositeScaled` primitive, and land §3.2 (the whole document at a view-derived scale) as
its first commit because it needs no shader change at all.**

### 4.1 The shape

1. **`render::compositeScaled(doc, outW, outH, opts, backend)`** — a thin public wrapper over the
   existing `compositeBuffer(doc, pre, w, h, opts, backend)` (`compositor.cpp:2015`) with a
   scaling `pre`. Unit-tested against `composite()` downsampled, at the existing tolerances.
2. **`CompositeOptions::skipLayer`** (a `core::LayerId`, `kInvalidLayerId` by default) so
   "composite without this layer" stops being a visibility mutation (`app_window.cpp:9253-9260`).
   Read-only walks are a precondition for §3.1 later and remove a latent hazard now.
3. **`buildBelowComposite` chooses its output size from the view**, capped by
   `WindowRenderer::caps().limits.maxImageDimension2D`, and passes `currentResampleFilter()` /
   `liveDrag` instead of the defaults (Finding **G2**).
4. **Then** the viewport bound: `vec4 belowRect` in the push block, skip-store outside it, rebuild
   on `notifyViewChanged`.
5. **Then, optionally**, cache the (now small) proxy across gestures on a
   `reflectStackFingerprint`-shaped key (§3.4), and refine to full resolution asynchronously
   (§3.1).

### 4.2 Why now, and why this is not throwaway work

- `compositeScaled` is **also** the primitive S60-d's proxy live composite needs (plan §7 S60-d).
- `compositeScaled` **also** fixes readback-audit finding **F5**: `refreshLayerReflection`
  (`app_window.cpp:9402-9425`) composites the whole 40 Mpx document and then hand-box-downsamples it
  to ≤768 px. Compositing *at* 768 px directly is ~2700× less work on that path, and the reflect env
  is a mirror — exactness is not a requirement there.
- `skipLayer` removes a document mutation that any off-thread composite would have to remove anyway.
- The **gesture-end** stall (Finding **G3**) is fixed only by S60-a items 11–13; a bounded backdrop
  is the thing that keeps the *start* fast if those are withheld.

### 4.3 What it costs and what it risks

- **Cost:** ~0.5 session for steps 1–3 (the whole-document scaled proxy, no shader change), ~1
  session for step 4. Steps 5 are opportunistic.
- **Risks, honestly:**
  - *The mask domain under a scaled `pre` is unverified.* `compositeBuffer` passes the mask domain
    as the **document** rect (`compositor.cpp:2048-2051`) while `pre` maps doc→buffer. Region
    composites only ever use a translation. A scaling `pre` must be proven against masked
    adjustments and unlinked masks (`foldUnlinkedMask(img, layer, pre)`, `compositor.cpp:510`)
    before it is trusted. **This is the one thing that could make the estimate wrong.**
  - *Group local extents.* `groupLocalExtent` (`compositor.cpp:1595`) sizes a group's buffer from
    the target window pulled back through `localToTarget`; under a 1/k `pre` that window shrinks
    proportionally, which should be correct but wants a test.
  - *A view-change rebuild during a drag* is new state in a path that currently has none.
  - *The ceiling is ~100–260 ms loaded* (§2.2). This is a stopgap that makes the app usable, not one
    that makes the stall disappear. Say so in the commit message.

---

## 5. Correctness traps

What any fix must preserve — beyond the two the task names (the drag cache refuses **unlinked
masks**, `compositor.cpp:2613-2617`, because their fold rides the live transform; and **channel
isolation** forces the CPU path, `app_window.cpp:9213`, because the GPU drag writes straight to the
canvas texture and bypasses the `presentComposite` remap).

1. **The backdrop must agree with the frame before it.** The frame before the drag came from
   `recompositeNow` with `currentResampleFilter()`; the backdrop comes from
   `buildBelowComposite`'s defaults. **Today those already disagree** (Finding **G2**) — any fix
   must fix it, not inherit it. *On screen:* the instant you start to drag, every rotated or scaled
   layer below jumps from anti-aliased to point-sampled, and jumps back on release.
2. **`below` must not contain the dragged layer.** Obvious, and the reason §3.5 is tempting. *On
   screen:* a ghost of the layer stays at its original position while the copy slides away.
3. **The dragged layer's alpha semantics.** The shader composites straight alpha; the backdrop must
   be straight alpha too, at whatever resolution. Downsampling straight-alpha RGBA without
   premultiplying **bleeds colour out of transparent pixels**. `compositeScaled` composites *into* a
   small buffer rather than downsampling a big one, so this is avoided by construction — which is
   another reason to prefer a scaled composite over a post-hoc resize of the full one. *On screen:*
   dark or white halos around every soft edge in the backdrop, only during a drag.
4. **The 1×1 shortcut's precondition.** It is only sound because `canUseGpuDrag` pins the target to
   the topmost root child. Its second caller has no such guarantee — Finding **G1**. Any refactor
   must not widen the shortcut.
5. **Off-canvas pixels do not exist here.** Unlike B7/B8 in the readback audit, the drag backdrop is
   strictly the canvas. A viewport-bounded rect must be clamped to the canvas, and the region
   outside must composite as transparent, not as "unwritten".
6. **`m_lastComposite` stays stale for the whole drag** (readback audit F1) and that is *relied
   upon*: nothing may start reading it as if the drag were live. A fix that "helpfully" patches
   `m_lastComposite` from the proxy would feed a low-res image to the eyedropper, the cursor
   readout, the Channels histogram and Smart Resize.
7. **Determinism belongs to the CPU lane.** The proxy is a display artifact. Export, save-preview
   and `.mosaic` PRVW must keep their full-resolution CPU walks (readback audit B1–B3).
8. **The document must not be mutated to express exclusion** once anything is off-thread (§3.1).

**What a wrong fix looks like on screen, in one list:** the backdrop softens and snaps back
(blind `k`); the background vanishes under a floating layer (§3.7); a ghost of the layer stays put
(§3.5); halos around soft edges (trap 3); aliasing appears the moment you press (trap 1); the
picture outside the viewport is stale when you zoom out mid-drag (§3.3's invariant); the 3D-text
mirror goes black (Finding G1).

---

## 6. How to measure it

### 6.1 The scope that already exists

`MOSAIC_PERF_SCOPE("Drag below-composite", Lane::Cpu)` (`app_window.cpp:9242`), release-reachable
with `--profile` / `MOSAIC_PROFILE=1` since S60-α. This is the primary number. It is **once per
gesture**, so read `max` and `last`, not `avg`.

**It does not cover the upload.** `WindowRenderer::beginGpuDrag` has no scope at all (Finding
**G5**). Add `MOSAIC_PERF_SCOPE("Drag texture upload", Lane::Gpu)` around `beginGpuDrag`'s two
`uploadSampledTexture` calls before claiming the fix works — otherwise a 919 ms → 58 ms composite
win could be hiding a few hundred ms of staging and fence that nobody is looking at.

Also worth a scope, for the other half of the problem: gesture end already reports as
`Composite (full)` (`app_window.cpp:9938`) and `Canvas upload (full)`; a `Move gesture end` row
would make the pair legible in one table.

### 6.2 The bench case to add

`src/app/bench.cpp:446-468` measures gesture-start as a full `render::composite` with the dragged
layer hidden. Two changes make it measure the fix:

- Parameterise it by backdrop scale (`k ∈ {1, 2, 4, 8}`) and print the row per `k`, so the
  ratio-vs-quality trade is a table rather than an argument.
- Add a **`gesture-end`** case — `moved.setTransform(translation(37.5, -12.5))` then a full
  `composite()` at `liveDrag=false` with `currentResampleFilter()`-equivalent `Auto`. Today the
  table implies the gesture is bracketed by one stall; it is bracketed by two, and the second is
  bigger.

Also fix the case's options: it uses `displayOptions()` (`Auto`) while the app uses the
`CompositeOptions` default (`Nearest`). Harmless on the synthetic all-identity document, wrong in
principle, and it hides Finding **G2** from the harness.

### 6.3 What "good enough" is

| threshold | number | meaning |
|---|---|---|
| ideal | ≤ 33 ms loaded | two frames; indistinguishable from instant |
| **target** | **≤ 100 ms loaded on 5000×8000 at fit zoom** | a hitch you notice and forgive |
| acceptable stopgap | ≤ 250 ms loaded | the app is clearly alive |
| today | 1337–3068 ms loaded | the app is gone |

§2.2 says the target is reachable when the backdrop is one screenful over a shallow stack, and that
the *acceptable* band is where a deep stack lands. **Set the exit criterion at ≤ 250 ms loaded and
report the fit-zoom number alongside it**, because "loaded, 5000×8000, 4 layers, fit zoom" is the
user's actual case.

### 6.4 The interactive check

`--gui-frames` drives no gestures, so this cannot be verified headlessly end to end. The honest
protocol: open a 5000×8000 document with a background under a full-canvas layer, run with
`--profile`, press-drag-release ten times at fit zoom and again at 1:1, quit, and read the
`Drag below-composite` / `Drag texture upload` / `Composite (full)` rows off the stderr dump
(`main.cpp:453-458`). Ten gestures give ten samples of a once-per-gesture cost.

---

## 7. Findings

Read-only analysis; **nothing below was fixed.** Reported per the task's constraint.

**G1 — the 1×1 backdrop shortcut is unsound for its second caller.**
`buildBelowComposite`'s shortcut (`app_window.cpp:9250-9251`) returns a 1×1 transparent image when
`m_document->root().childCount() <= 1`. That is correct **only** if `target` is a top-level child —
which `render::canUseGpuDrag` guarantees for the drag caller and **nothing guarantees** for the
other caller, `refreshLayerReflection` (`app_window.cpp:9402`). A document whose root holds a single
group containing `[photo, 3D text]` therefore hands the reflect env a 1×1 transparent image;
`below.empty()` is false for a 1×1 image, so the guard at `:9403` does not catch it, and the
downsample at `:9404-9412` yields a 1×1 environment. **The 3D text's canvas reflection silently
mirrors nothing** whenever the root has ≤1 child and the text is nested. Not fixed. (The correct
predicate is "target is the topmost root child", which is what the drag path already knows.)

**G2 — the drag backdrop is composited with different resample options than the frame it
replaces.** `buildBelowComposite` uses a default-constructed `CompositeOptions`
(`app_window.cpp:9256-9258`) → `resampleFilter = Nearest`, `liveDrag = false`. Every display
composite uses `opts.resampleFilter = currentResampleFilter()` (`recompositeNow`, `:9618`;
`recompositeRegionNow`, `:9702`), whose default is `Auto` (`:9467`). Any layer *below* the dragged
one with a non-lossless transform is therefore point-sampled in the drag backdrop and filtered
everywhere else: a visible aliasing pop at gesture start and back at release. Invisible on
all-identity stacks, which is why it has survived. Not fixed. (`bench.cpp` compounds it by measuring
the case with `displayOptions()`, i.e. `Auto` — the harness and the app do not agree.)

**G3 — the gesture-END stall is unnamed and larger than the gesture-start one.**
`gestureEnded` → `syncAfterEdit` (`app_window.cpp:9862`) → `m_dragCache.invalidate()` →
`requestRecomposite` → `recompositeNow` (`:9602`), with `activeDragLayer()` already invalid
(`vulkan_canvas.cpp:1558` cancels the gesture first) and no drag cache ever built (the drag ran on
the GPU, so `recompositeNow` was never called during it). Release therefore pays the **full CPU
walk** plus a full-canvas upload: 1712 ms quiet / 2588 ms at load 6 / 4672 ms at the task's load on
5000×8000 — versus 919/1337/3068 for gesture start. It appears in no plan document and no bench
case. Not fixed.

**G4 — `kMaxCachedDragBuffers` is a count calibrated for 1080p.** `compositor.cpp:2551-2555`:
*"Cache budget, in document-sized float buffers (~32 MiB each at 1080p) … Six cached."* At
5000×8000 one `ImageF` is **640 MiB**, so the same constant admits **~3.8 GiB** of drag cache on the
exact document PLAN §2 is about, before the per-frame working copies. The budget should be in bytes,
or scaled by `docW*docH`. Not fixed. (It also plausibly explains why `drag-cache int` is only 2.4×
faster than `full-walk int` at 5000×8000 but 1.9× at 1080p — at 40 Mpx the "fast" path is
memory-bandwidth-bound on gigabyte buffers.)

**G5 — `WindowRenderer::beginGpuDrag` is unprofiled and uncapped.** `window_renderer.cpp:1714`
uploads `below` and `dragged` through `uploadSampledTexture` (`:1399`), each allocating a
document-sized `R8G8B8A8_UNORM` image plus a document-sized host-visible staging buffer, `memcpy`ing
into it and blocking on `vkWaitForFences` (`:1507`). At 5000×8000 that is 320 MiB of memcpy, 320 MiB
of staging and 320 MiB of device images, synchronously, in the same frozen frame. There is **no
`MOSAIC_PERF_SCOPE` anywhere in `window_renderer.cpp`**, so none of it is visible to `--profile`.
And `5000 > 4096`, the Vulkan-1.0 guaranteed `maxImageDimension2D`: `WindowRenderer` holds a
`GpuCaps` (`window_renderer.hpp:444`, `:484`) and consults it for neither the drag textures nor
`ensureCanvasTexture` (`:712`), so on a floor device `vmaCreateImage` simply fails, the warning at
`:1722` fires, and the gesture falls back to the CPU path **after** having paid the full CPU below
composite. S60-α item 4 gated the compute lanes; the presenting device's own textures were not part
of that sweep. Not fixed.

**G6 — `buildBelowComposite` mutates the document to express exclusion.**
`l->setVisible(false); … l->setVisible(wasVisible);` (`app_window.cpp:9253-9260`), with no command
and no observer. Safe today only because the call is synchronous and non-reentrant on the UI thread.
It is a hard blocker for any off-thread variant, and it silently perturbs anything that reads
`visible()` reentrantly. A `CompositeOptions::skipLayer` is the read-only replacement. Not fixed.

---

## 8. Summary

- The stall is **real** (919 ms quiet / 1337 ms at load 6 / 3068 ms at the task's load on
  5000×8000), **once per gesture**, and blocks the UI thread inside `onFrame`.
- It is **not unconditional and not on mouse-down**: it fires on the first drag frame past a 3 px
  dead zone, only when `canUseGpuDrag` admits the layer, only with ≥2 top-level children. It is the
  **price of admission to the fast lane**, which is a worse property than "an unconditional tax".
- The bench measured **the smaller of the two stalls**. Gesture end costs a full CPU walk plus a
  160 MiB upload — 1.5× more — and is named nowhere (**G3**).
- **Recommendation:** bound the backdrop by what is on screen (§3.3), on a new
  `render::compositeScaled` primitive, shipping the no-shader-change scaled-proxy variant first
  (§3.2). ~0.5–1 session. Expected 919 ms → 30–80 ms quiet, 100–260 ms loaded.
- **It does not disappear for free once residency lands** — it disappears with S60-a items 11–13,
  the three the plan reserves the right to withhold, and only if the resident accumulator keeps a
  below-the-topmost-layer checkpoint (a design requirement §3.3 of the performance plan does not
  currently state).
- **Subtracting the dragged layer from `m_lastComposite` is mathematically undefined** exactly where
  it is needed: `a_s = 1` over a full-canvas opaque layer (§3.5).
- Six findings recorded, none fixed: **G1** the 1×1 shortcut breaks the 3D reflect env; **G2** the
  backdrop uses different resample options than the frame it replaces; **G3** the unnamed
  gesture-end stall; **G4** the drag cache's budget is a count, not bytes; **G5** the drag uploads
  are unprofiled and uncapped, and fail outright on a Vulkan-1.0 floor device; **G6** the composite
  excludes a layer by mutating the document.
