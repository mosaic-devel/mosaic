# S60 — Performance, GPU residency, and portable Vulkan

**Status:** **S60-α COMPLETE 2026-07-23** — the foundations are built, merged and pushed; §1 is
now a record of what was wrong rather than what is. §7 carries the per-item state. This document
is the authoritative plan for the S60 arc; `PLAN.md` §9 S60-a…-d is superseded by the session
split in §7 below.

⚠ **§3.3's `requestReadback(rect) -> future` sketch is SUPERSEDED** by
`docs/s60-readback-consumers.md`, which audited the real consumers and found the shape wrong —
see §7's "landed alongside α".

**The brief, in the user's words:** target **Vulkan 1.0** as the floor and tack optimisations on
per detected version/extension; **back-fill from supported extensions first, CPU path only as the
absolute last resort**; every GPU lane owes a CPU lane and vice versa; a real **CPU-only mode**;
and the **dirty-tile grid is the file format's tile grid**. The dev rig (RX 6600 XT / RADV,
Vulkan 1.4) is explicitly *not* the target — it is the happy path we must not design around.

---

## 1. Ground truth: where the pixels actually go today

Measured from the tree at `7aef80c`, not from memory. This section exists because most of the S60
plan follows mechanically from it.

### 1.1 The interactive compositor is 100% CPU

Every live composite in the app is hard-pinned to the CPU backend. `src/ui/app_window.cpp` calls
`render::composite(...)` / `render::compositeRegion(...)` with `render::Backend::Cpu` at **every
one** of its ~10 call sites (`app_window.cpp:2995, 4121, 4151, 5945, 5982, 6685, 8279, 8423, 9225,
9575, 9655`). `render::GpuCompositor` — the S7-b compute blend kernel — is reached only by
`--composite-demo` and the unit tests. It has never run in the interactive app.

So the live frame is: CPU tree walk → CPU transform/resample → CPU blend (`blend.hpp`) → CPU
`toImage8` → staging upload of an 8-bit document-sized image → present compute pass. The user's
2026-07-03 verdict ("everything is so laggy only due to compositing almost everything on CPU…
frankly unbearable") is literally accurate.

**Even if we flipped the flag, `GpuCompositor` would be slower.** `blendOver` uploads *both*
operands and reads the accumulator back **per layer, per composite** (`gpu_compositor.hpp:38-42`,
and the header says so). At 1920×1080 rgba32f that is 8.3 MB up + 8.3 MB up + 8.3 MB back **per
layer**. This is the exact failure mode already diagnosed and fixed once, in the 3D-text lane:
the GPU lane was 108 ms/frame vs the CPU's 44 ms until device-local working buffers landed
(→ 10.4 ms). The compositor has not had that pass. **Flipping `Backend::Cpu` → `Auto` before
residency lands would be a regression, not a fix.** Residency is the whole point.

### 1.2 We create up to five separate Vulkan devices

Each GPU lane calls `VulkanContext::create()` for itself:

| Lane | Device created at | Lifetime |
|---|---|---|
| `WindowRenderer` (present/swapchain) | `window_renderer.cpp` (own instance+device) | window |
| `ExtrudeGpu` (3D text) | `extrude_gpu.cpp:172` | lazy, process |
| `TextureGpu` (sky/paper) | `texture_gpu.cpp:954` | lazy, process |
| `BlurGpu` (blur filters) | `blur_gpu.cpp:223` | lazy, process |
| `GpuCompositor` | `gpu_compositor.cpp:52` | per composite session |
| `computeFill` / headless render | `compute_fill.cpp:24`, `render.cpp:33` | per call |

Consequences: N× VkInstance + VkDevice + VMA allocator + command pool; **no shared memory
budget** (nothing can know the GPU is full); **every hand-off between lanes is a round trip
through host memory** (blur a texture layer = TextureGpu → host → BlurGpu → host → composite);
N× validation-layer cost in debug; and on a low-VRAM device, N× the baseline allocation
overhead. On the target hardware this is the single most expensive structural decision in the
renderer.

**This must be fixed first.** Everything else in S60 assumes one device.

### 1.3 We are accidentally already Vulkan-1.0-clean, and pinned to 1.2 anyway

The good news is large:

- **No device features are enabled anywhere.** `VkDeviceCreateInfo::pEnabledFeatures` is never
  set (`vulkan_context.cpp:155-159`, `window_renderer.cpp:296-302`). We run on the all-features-off
  configuration already.
- **No shader uses a post-1.0 feature.** All 12 shaders are plain `#version 450` with **zero**
  `#extension` directives, no subgroup ops, no 16/64-bit types, no descriptor indexing, no buffer
  references.
- **Every workgroup is 8×8×1 = 64 invocations**, under the 1.0 guaranteed `maxComputeWorkGroup-
  Invocations` of 128. `extrude_raster.comp` is 64×1×1. Nothing to change.
- **`canvas_present.comp`'s push block is exactly 128 bytes** — the 1.0 guaranteed
  `maxPushConstantsSize`, and the shader's own comment already says it is budgeting against that.
  Someone was already thinking about this.
- Only one device extension is requested anywhere: `VK_KHR_swapchain`.

The only things pinning us above 1.0 are **two constants and a compile flag**:

- `appInfo.apiVersion = VK_API_VERSION_1_2` (`vulkan_context.cpp:85`, `window_renderer.cpp:199`)
- `VmaAllocatorCreateInfo::vulkanApiVersion = VK_API_VERSION_1_2` (`window_renderer.cpp:315`)
- `glslc --target-env=vulkan1.2` (`cmake/EmbedShaders.cmake:58,65`) — which emits **SPIR-V 1.5**.
  A Vulkan 1.0 driver accepts **only SPIR-V 1.0**, so today's binary cannot even load its shaders
  on a 1.0 device. This is the one hard blocker, and it is a one-line fix.

**Conclusion: the "target 1.0, tier up" plan costs almost nothing to adopt and should be done at
the very start of the arc, before any new GPU code is written.** Writing the tiering harness after
the tiled compositor would mean retrofitting it into new code; writing it first means all new code
is born capability-gated.

### 1.4 Two Vulkan 1.0 limits are correctness problems, not perf problems

- **`maxImageDimension2D` is guaranteed to be only 4096.** We allocate document-sized images:
  the canvas texture (`window_renderer.cpp:692`), the `GpuCompositor` rgba32f accumulator
  (`gpu_compositor.cpp:17`), the `TextureGpu` float buffers. A 5000×8000 document — the exact
  case in PLAN §2's open Move-lag bug — **cannot be a single image** on a minimum-spec device.
  Tiling is therefore a *correctness* requirement for the low end, not only a speed one. That
  substantially strengthens the case for doing §3 properly.
- **`VK_FORMAT_R32G32B32A32_SFLOAT` is guaranteed for `STORAGE_IMAGE` but NOT for
  `SAMPLED_IMAGE_FILTER_LINEAR`.** Any bilinear sampling of a float working buffer (transformed
  layers, proxy downsampling, the loupe) is not guaranteed at rgba32f. `R16G16B16A16_SFLOAT`
  *is* guaranteed for both. See §2.4.
- **`maxPerStageDescriptorStorageBuffers` is guaranteed to be only 4.** `extrude_raster.comp`
  binds **7 SSBOs** (bindings 0–6). This exceeds the guaranteed floor. Real desktop drivers report
  far more, but "real drivers report more" is exactly the assumption this arc exists to stop
  making. The lane must query the limit and refuse itself (→ CPU lane) when it does not fit.

### 1.5 The CPU compositor spawns threads per call

`render/compositor.cpp:50-70` defines a `parallelFor(count, minPerBand, fn)` used at **18 sites**,
and it constructs and joins a fresh `std::vector<std::thread>` **every time**. During a gesture
this happens many times per frame. Thread creation is ~20-60 µs on Linux; 18 sites × several
layers × 60 Hz is real, and it is pure overhead that a persistent pool removes. Same pattern in
`core/texture/parallel_rows.hpp:16` and four inpaint backends. This is the cheapest large CPU win
available and it is independent of everything GPU.

### 1.6 There is no CPU presentation path at all

If Vulkan init fails, `VulkanCanvas` sets `m_initFailed` and the canvas draws **nothing**
(`vulkan_canvas.cpp:228-263`). There is no software fallback, not even a degraded one. This is the
honest scope of "CPU-only mode" — see §6, and note that it is much bigger than "don't use the GPU
compositor".

### 1.7 The profiler is compiled out of release

`src/ui/profiler.hpp` is `#ifdef MOSAIC_DEBUG`; `MOSAIC_PERF_SCOPE` expands to nothing in release
"deliberately". But the user runs the **release** build (the debug compositor is 5-10× slower), so
today **we cannot measure the build the user actually experiences**. An optimisation arc that
cannot measure its target configuration is guesswork. See §8.

> ✅ **Fixed in S60-α item 6** (collection is runtime-gated and ships in every build) and finished
> in S60-a: the collector now lives at **`src/common/profiler.{hpp,cpp}`** (`mosaic::common`), so
> `render` can instrument itself without `ui` having to be above it. The Timing Profiler *window*
> stays in `src/ui/timing_graph_window.{hpp,cpp}` and stays debug-only.

---

## 2. The capability model: Vulkan 1.0 floor, tiered opt-ins

### 2.1 The shape of it

One struct, built once at device creation, consulted everywhere:

```cpp
// src/render/gpu_caps.hpp   (FLTK-free, unit-testable — the decision function is pure)
struct GpuCaps {
    std::uint32_t apiVersion;          // min(instance, device) — the version we may actually use
    VkPhysicalDeviceLimits limits;     // verbatim, for direct interrogation
    // Tier flags: each is "this optimisation is available", never "this Vulkan version".
    bool subgroupArithmetic;   // 1.1 core, subgroupSupportedOperations & ARITHMETIC
    bool storage16bit;         // 1.1 VK_KHR_16bit_storage + storageBuffer16BitAccess
    bool timelineSemaphore;    // 1.2 core / VK_KHR_timeline_semaphore
    bool descriptorIndexing;   // 1.2 core / VK_EXT_descriptor_indexing (+ the specific sub-features)
    bool bufferDeviceAddress;  // 1.2 core
    bool shaderFloat16;        // 1.2 VkPhysicalDeviceShaderFloat16Int8Features
    bool synchronization2;     // 1.3 core / VK_KHR_synchronization2
    bool maintenance4;         // 1.3 core (maxBufferSize)
    bool hostImageCopy;        // 1.4 core / VK_EXT_host_image_copy
    bool memoryBudget;         // VK_EXT_memory_budget (any version)
    bool dedicatedAllocation;  // 1.1 core / VK_KHR_dedicated_allocation (VMA feeds on it)
    bool timestampQueries;     // limits.timestampComputeAndGraphics && timestampPeriod > 0
    bool portabilitySubset;    // VK_KHR_portability_subset present (MoltenVK) — MUST be enabled
    std::uint32_t transferQueueFamily; // UINT32_MAX if none distinct from the main queue
    std::uint32_t computeQueueFamily;  // async-compute family, or UINT32_MAX
};
```

**The load-bearing rule: nothing in the renderer branches on `apiVersion`.** Code branches on
capability flags only. A driver that reports 1.3 but lacks `descriptorIndexing` sub-features (real:
several mobile and older Mesa drivers advertise a version but gate individual features) then Just
Works, because we never asked "are you 1.3?", we asked "can you index descriptors?". This is
exactly the user's "incomplete 1.3/1.4 support" case, and capability-flag branching is what makes
it a non-event instead of a crash.

`gpu_caps.cpp` also owns the **enable lists**: given the probed device, produce the
`ppEnabledExtensionNames` + the pNext chain of feature structs to request. Requesting a feature we
did not verify is a `vkCreateDevice` failure, so the probe and the request must be one function.

### 2.2 What each tier actually buys *this* application

This is the part worth arguing about, because most "enable more extensions" lists are cargo cult.
For a tiled compute compositor, only a handful move the needle:

**Vulkan 1.0 — the floor. Everything works here.**
Explicit `vkCmdPipelineBarrier`, one descriptor set per dispatch, staging-buffer uploads, fences,
`vkCmdCopyBufferToImage`. Correct, portable, and roughly 80% of the win, because the win is
"stop round-tripping to host memory", not "use fancy extensions".

**1.1 — `subgroupArithmetic`.** Real gains in the *reduction* shaders, not the compositor:
histogram (Levels/Curves), the Smart-Resize importance map, blur box-sum prefixes, and
`sky_estimate`. A subgroup-reduced histogram is typically 3-8× a naive `atomicAdd` version.
Also `dedicatedAllocation` (hand VMA `VK_KHR_dedicated_allocation` + `get_memory_requirements2`
and it stops over-suballocating large images — matters most on small-VRAM parts).

**1.1 — `storage16bit`.** Halve upload bandwidth for layer tiles by staging `rgba16f` instead of
`rgba32f`. On a PCIe 2.0 x8 part (the target's likely bus) upload bandwidth *is* the bottleneck.

**1.2 — `descriptorIndexing`.** The big one for the tiled compositor. Without it, a composite of
L layers × T tiles is L×T dispatches, each with its own descriptor set write. With a runtime-sized
descriptor array of tile images plus `shaderSampledImageArrayNonUniformIndexing`, the whole thing
collapses to **one dispatch per layer** (or even one for the whole stack) that indexes tiles from a
buffer. This is the difference between "GPU-resident" and "GPU-resident and actually fast on a
document with 4000 tiles". Fall-back is straightforward: loop and dispatch per tile.

**1.2 — `timelineSemaphore`.** Lets uploads on a transfer queue and composites on the compute
queue interleave without a fence round-trip per submit. Pairs with §2.3.

**1.2 — `shaderFloat16`.** Half-rate → full-rate arithmetic on many mobile/integrated parts for
the blend kernel. Worth a specialization constant, not a separate shader.

**1.3 — `synchronization2`.** Purely an ergonomics/precision win: `VK_ACCESS_2_*` lets us express
"this tile is read by the next dispatch" without over-broad `ALL_COMMANDS` barriers. Measurable on
tile-heavy submissions where the 1.0 barriers are necessarily coarser. Nice-to-have.

**1.4 — `hostImageCopy`.** Removes the staging buffer *entirely* for CPU→GPU tile uploads: the
driver writes host memory straight into an optimally-tiled image. On the happy path this is the
single biggest upload win available. On the floor path we keep staging. Perfect tier candidate.

**Any version — `VK_EXT_memory_budget`.** Not an optimisation, an **enabler**: it is the only
portable way to know actual VRAM headroom, which is what S60-c's eviction policy must key on.
Without it we guess from `VkPhysicalDeviceMemoryProperties` heap sizes and hope. Enable it
wherever offered.

**Any version — `VK_KHR_portability_subset`.** Not optional: if a device advertises it (MoltenVK
on macOS, per `docs/build-macos.md`), `vkCreateDevice` **fails** unless we enable it. Must be in
the enable list unconditionally-if-present. Cheap, and it is a latent macOS bug today.

**Explicitly *not* worth it here:** dynamic rendering (we are compute-only, there is no render
pass to make dynamic), mesh shaders, ray tracing, `VK_EXT_shader_object`, push descriptors (our
descriptor churn goes away via descriptor indexing, not via cheaper writes).

### 2.3 Queues

Today: one graphics queue, one queue, everywhere. Worth probing for (a) a **dedicated transfer
family** so tile uploads overlap compositing, and (b) an **async-compute family**. Both are core
1.0 (`vkGetPhysicalDeviceQueueFamilyProperties`) — this is a *hardware* tier, not a version tier,
and it belongs in `GpuCaps` alongside the version tiers. Falls back to the single queue trivially.

### 2.4 Working-buffer format

Decide once, at probe time, from `vkGetPhysicalDeviceFormatProperties`:

1. `R32G32B32A32_SFLOAT` if it reports `STORAGE_IMAGE` **and** `SAMPLED_IMAGE_FILTER_LINEAR`
   (the dev rig will; it is not guaranteed).
2. else `R16G16B16A16_SFLOAT` — guaranteed for both, half the memory and half the bandwidth, and
   ~11 bits of mantissa is comfortably enough for 8-bit-source compositing. **This is probably the
   right default even where rgba32f is available**, and the tier probe should be able to say so.
3. `R8G8B8A8_UNORM` is the guaranteed floor but re-introduces the banding `ImageF` exists to avoid;
   only for a genuinely desperate device, and it must be visible in the log.

The CPU reference stays `ImageF` (fp32) regardless — golden tests compare against it with the
existing 1/255 tolerance, which fp16 clears comfortably.

### 2.5 Shader build

`cmake/EmbedShaders.cmake` moves to `--target-env=vulkan1.0` (SPIR-V 1.0) for the baseline set.
Tier-specific shader *variants* (subgroup histogram, fp16 blend, descriptor-indexed composite)
compile as separate `.spv` blobs at their own target env, selected at pipeline-creation time from
`GpuCaps`. Specialization constants handle the small knobs (workgroup size, fp16 on/off) without
a combinatorial blob explosion; separate blobs only where the SPIR-V genuinely differs.

**Cost check:** verified above that the current 12 shaders need *no* source changes to compile at
`vulkan1.0`. The retarget is a flag flip plus a build.

### 2.6 Honesty about "2010 shitboxes"

Worth stating plainly so the target is real rather than rhetorical: **no 2010 GPU supports Vulkan
at all.** The Vulkan 1.0 floor in hardware is GCN 1.0 (2011-12), Kepler (2012), and Intel Haswell
(2013, via ANV). So the true floor is ~2012-2013 hardware, plus **lavapipe** (software), plus
whatever mobile/embedded parts show up. That does not weaken the brief — those parts are exactly
where the guaranteed-minimum limits and missing extensions bite, and where a 4096 texture limit or
4 storage buffers is a live constraint rather than a theoretical one. It just means the floor is
"oldest Vulkan-capable hardware + software rasterisation", which is a target we can actually test
(lavapipe is installable and CI-able; see §8.3).

---

## 3. Tiles

### 3.1 The user's constraint, and a push-back

The brief: **dirty-tile region size is tied to the file format's tile size**. The format's tile is
**64 px** (`src/io/mosaic/docio.hpp:33`, `kTileSize = 64`), chosen by measurement in the S48
research (a 32×32 brush dab costs 13.8× more to autosave at 256 px than at 64 px, because the
format has no sub-tile diffing).

**I agree with the principle and want to split it in two.** 64 px is right for *tracking* and
wrong for *dispatching*, and they do not have to be the same number:

- **Dirty-tracking grid = 64 px, exactly the format's grid.** One `TileKey{layerId, tx, ty}`
  vocabulary shared by the compositor, the autosave journal, and the undo/region commands. One
  dirty set feeds both "recomposite this" and "write this to the journal". This is a genuinely
  good idea and it is the main reason to tie them: it removes an entire class of
  two-dirty-sets-disagree bugs, and the `.mosaic` journal already thinks in exactly these keys
  (`docio.cpp:284`, `TILE = layer_id(u64) + tx(u32) + ty(u32)`).

- **GPU dispatch/residency granularity = a 64 px multiple, probed per device** (a *macrotile*,
  e.g. 4×4 = 256 px, or 8×8 = 512 px on a device with a big `maxImageDimension2D`). Rationale: a
  1920×1080 document is 510 tiles at 64 px but only 40 macrotiles at 256 px. Per-dispatch cost
  (descriptor set, barrier, command-buffer bytes) is roughly constant, so at 64 px the fixed
  overhead dominates the actual work — a 64×64 rgba16f tile is 32 KB, which a mid-range GPU
  blends in microseconds. On the *target* hardware this is worse, not better, because low-end
  parts have proportionally higher per-dispatch overhead and fewer compute units to hide it.
  512 px macrotiles also collide with `maxImageDimension2D = 4096` far less often than a
  full-document image does.

So: **dirty in 64s, composite in macrotiles, where macrotile = 64 × 2^k and k is chosen from
`GpuCaps`.** A dirty 64-px tile marks its containing macrotile dirty. Worst case we recomposite
some clean 64-px tiles inside a dirty macrotile — bounded, measurable, and tunable by moving k. If
measurement says k=0 (64 px) wins on some device, k=0 is reachable without a redesign.

If you'd rather keep it strictly 1:1 at 64 px, that's a one-constant change and everything else in
this plan is unaffected — but I'd want the macrotile knob to exist so we can measure rather than
assume.

### 3.2 In-memory tiling is not yet started

`kTileSize` appears **only** in `src/io/mosaic/docio.cpp` — tiling exists in the *file format* and
nowhere else. In memory, a layer is still one flat `common::Image` (8-bit RGBA) and the composite
is one flat `ImageF`. So S60-c's "tiled pixel storage" is greenfield, and it should be built
against the same 64-px grid so that in-memory tile ↔ stored tile is 1:1 with no re-tiling on save.
That alignment is the strongest argument for the user's constraint and I want it recorded as a
decision, not an accident.

### 3.3 The residency model

```
Document (CPU, authoritative)
  └─ Layer
       ├─ CPU tiles  : 64px, 8-bit today / float after S43-a — the undo/save/export source
       └─ GPU tiles  : macrotile images in a device-local atlas, uploaded on demand,
                       evicted under budget pressure (VK_EXT_memory_budget)

Composite (GPU, derived)
  └─ macrotile accumulators, blended bottom→top per dirty macrotile,
     kept device-resident frame to frame; only dirty macrotiles are recomputed;
     the present pass samples the accumulator directly — no readback

Readback happens ONLY when the CPU genuinely needs pixels:
  export, the cursor colour readout, the histogram, Smart Resize analysis, thumbnails.
  Each of those becomes an explicit, named, asynchronous request — not a per-frame side effect.
```

The last line is the real architectural change. Today `m_lastComposite` is a full CPU copy of the
composite kept coherent on every patch (`app_window.cpp:patchComposite`), because five different
consumers want to read pixels. Under residency those consumers need an explicit API
(`requestReadback(rect) → future`), and the ones that can tolerate staleness (histogram, Smart
Resize) should say so. **Enumerating those consumers is a prerequisite commit**, not a detail —
a missed one silently forces a full readback per frame and eats the entire win.

---

## 4. GPU-path inventory

What has a GPU lane, what doesn't, and what S60 owes it.

| Operation | GPU lane today | Verdict |
|---|---|---|
| Blend / source-over | `composite_blend.comp` — **exists, unused by the app** | **S60-a**: make it the resident tiled path |
| Present / view transform | `canvas_present.comp` | ✓ already GPU-only |
| Empty-state idle field | `canvas_idle.comp` | ✓ |
| Move/Resize/Rotate drag | `canvas_drag_composite.comp` | ✓ (single unmasked raster layer only) |
| Blur (7 kinds) | `blur_separable/lens/dof/convert.comp` | ✓ Box/Motion/Radial still CPU-only → **S60-e** |
| Texture generator (sky/paper) | `texture_sky/paper.comp` | ✓ grass still CPU-only → **S60-e** |
| 3D text raster | `extrude_raster.comp` | ✓ but **7 SSBOs > 1.0 floor** → needs a caps gate |
| Solid fill | `fill.comp` | ✓ |
| **Layer transform / resample** | ✗ CPU only (`compositor.cpp` kernels) | **S60-a** — folds into the tile composite |
| **Adjustment layers** (S32, 8 kinds) | ✗ CPU only | **S60-d** — per-pixel, trivially portable |
| **Layer effects** (S-LE a…d) | ✗ CPU only | **S60-d** |
| **Brush dab stamping** (S19) | ✗ CPU only | **S60-d** — the highest-value one for felt latency |
| **Masks / clip-to-below fold** | ✗ CPU only | **S60-a** — must be in the tile kernel or the walk still round-trips |
| **Vector rasterisation** (S25) | ✗ CPU only | **S60-e** (PLAN already folds S25-c here) |
| **Text glyph rasterisation** | ✗ CPU only | **S60-e** |
| **Selection ops** (magic wand, select brush, morphology) | ✗ CPU only | **S60-e**, low priority — they are per-gesture, not per-frame |
| **Inpaint** (S37/S39) | ✗ CPU only, threaded | **out of scope** — patch-match is a poor compute fit and it is already async |
| **Colour management** (lcms2) | ✗ CPU only | **out of scope** — lcms2 is the correctness reference |
| **Histogram / Smart-Resize importance** | ✗ CPU only | **S60-e**, wants `subgroupArithmetic` |

**Ordering principle:** an operation earns a GPU lane when it is (a) on the per-frame path or (b)
per-pixel over the whole document. Brush stamping and adjustments are (a); vector/text raster are
(b)-ish but cached, so they matter less. Inpaint and colour management are neither.

**Critical structural point:** as long as *masks, clip, adjustments and transforms* stay on the
CPU, a "GPU-resident compositor" still round-trips per layer, because the walk has to come back to
host memory to apply them. The S7-b design (GPU does `blendOver`, CPU does everything else) is
precisely why the GPU compositor is slower than the CPU one. **S60-a's kernel must absorb the
whole per-layer step — transform + mask fold + clip + blend — or it will not pay.** That is the
single most important design constraint in this document.

---

## 5. CPU-path inventory

The reverse audit: what runs on the GPU with no CPU equivalent.

| GPU-only today | CPU path? |
|---|---|
| `canvas_present.comp` — view transform, checkerboard, pixel grid | ✗ **none** |
| Marching ants (S13) | ✗ none |
| Move/crop handles, rotate dial, guides, line gizmo | ✗ none |
| Loupe (S24) | ✗ none |
| Spell squiggles (overlay SSBO) | ✗ none |
| Text caret / selection quads | ✗ none |
| Empty-state ripple field + invitation atlas | ✗ none |
| Feather indicator, overlay line styles | ✗ none |

Everything else has a CPU reference by construction — the project's discipline of "CPU lane is the
golden reference, GPU lane must match within 1/255" has held for blur, texture, extrude, fill and
composite. **The entire CPU-path gap is the present pass and its overlays.** That is a coherent,
single-scope deliverable, and it is the same deliverable as CPU-only mode (§6).

---

## 6. CPU-only mode

### 6.1 Two different things wear this name

1. **"Don't use the GPU for compute"** — composite, blur, textures, 3D text all take their CPU
   lanes; presentation still uses Vulkan. **Cheap**: the lanes all already exist and already have
   silent per-call fallbacks. This is a settings toggle plus a `--cpu` flag that makes every
   `create()` return null. Should be built early, because it is also the **test harness** that
   proves every CPU lane still works — a CI run in this mode is the regression net for the whole
   "every GPU lane owes a CPU lane" rule.

2. **"Run with no Vulkan at all"** — no instance, no device, no swapchain. **Expensive**, because
   of §5: the present pass *is* the UI. Marching ants, handles, the loupe, the caret, the
   empty-state field are all shader code with no CPU twin.

### 6.2 Recommendation: three levels, in this order

- **Level 1 — `--cpu` compute-only (S60-b).** As above. Small, immediately useful, and it is the
  gate that keeps the CPU lanes honest.

- **Level 2 — software Vulkan (S60-f).** Detect a software device (`VK_PHYSICAL_DEVICE_TYPE_CPU`,
  i.e. lavapipe/SwiftShader), accept it, and surface a one-time non-modal notice
  ("software rendering — performance will be limited"). Zero new render code: same shaders, same
  present pass, same overlays, just slow. `GpuCaps` should also apply a *conservative profile* on
  a software device (smaller macrotiles, no speculative residency, aggressive proxy). This is
  already what PLAN §9 S60-d intends and it is by far the best effort/coverage ratio.

- **Level 3 — genuine no-Vulkan presentation (S60-f, stretch).** Only worth building if we decide
  Mosaic must run where *no* Vulkan ICD exists at all. The design that makes it tractable: extract
  the present pass's overlay work into a **pure `render::PresentModel`** — an FLTK-free, unit-
  tested description of what to draw (view transform, ants segments, handle quads, dial, loupe,
  squiggles, grid) — with two consumers: `canvas_present.comp` (existing) and a new CPU rasteriser
  feeding `fl_draw_image`. That refactor is worth doing *on its own merits* even if Level 3 never
  ships, because it makes the overlay logic testable, which today it is not.

  **Estimate honestly: Level 3 is a multi-session project.** It should not block anything else and
  it should be the last thing in the arc.

### 6.3 Recommended default policy

`Auto` (present behaviour) stays the default. Settings → Performance gains an explicit
**Rendering** choice: *Automatic / GPU required / CPU only*, plus a read-only **capability
readout** showing the detected device, Vulkan version, which tier flags fired, and which lane is
serving each operation. That readout is the diagnostic that makes every future "why is this slow
on my machine" report answerable in one screenshot — the `--gui-frames`/headless-verification
philosophy applied to the GPU. Per [[mosaic-no-toggle-for-strictly-better]] there is no toggle for
individual tiers: if a capability is present and faster, we use it.

---

## 7. Session plan

Replaces PLAN §9's S60-a…-d split. Each session is independently shippable and independently
measurable.

### S60-α — Foundations (do this first, it is small and everything depends on it)

**STATUS 2026-07-23: S60-α COMPLETE — all six items built, merged and pushed.**

1. ✅ **`render::GpuCaps`** (`066a7be`) — `GpuProbe` / pure `decide()` / `probePhysicalDevice`,
   tier flags, enable lists, `GpuFeatureChain`, format selection, macrotile policy, lane-admission
   helpers, `applyFloorProfile` + `MOSAIC_GPU_PROFILE=floor`. 20 cases / 132 assertions.
2. ✅ **Vulkan 1.0 floor** (`6082e3e`) — `MOSAIC_SHADER_TARGET_ENV=vulkan1.0` (all 12 shaders now
   emit SPIR-V 1.0, verified by header word; they were 1.5 and therefore unloadable on a 1.0
   driver), `requestedApiVersion()` in both contexts, VMA given the negotiated version, caps
   probed and stored at device creation in both paths.
3. ✅ **One shared device** (`7bbdb0f`) — `VulkanContext::shared()`, borrowed by all six compute
   lanes and held by `shared_ptr` so the device outlives every lane whatever the static
   destruction order. Sharing exposed two hazards per-lane devices hid, both handled at the
   context so no lane can forget them: `createCommandPool()` (pools are externally synchronized)
   and `submit()`/`waitIdle()` under a queue mutex (there are now zero direct `vkQueueSubmit` /
   `vkDeviceWaitIdle` calls outside the context). **Five devices are now two** — `WindowRenderer`
   deliberately keeps its own, because it selects a device that can *present*, and folding it in
   belongs to S60-a where the resident composite must live on the presenting device by
   construction rather than by guess.
4. ✅ **Caps gates on the existing lanes** (`f88f0a5`) — storage-buffer counts (extrude 9,
   blur 7, texture 6, all against a guaranteed 4), buffer ranges (lane caps were 256–512 MiB
   against a guaranteed 128 MiB — every lane would have sailed past its own guard), and image
   dimensions (`GpuCompositor::ensureSize`, `TextureGpu::render` vs a guaranteed 4096).
5. ✅ **`VK_KHR_portability_subset`** enabled wherever advertised (`6082e3e`) — was a latent
   `vkCreateDevice` failure on MoltenVK.
6. ✅ **Release-build profiling** — `Profiler` compiles in every build behind a runtime flag
   (`--profile` / `MOSAIC_PROFILE=1`); debug still defaults on, so the Help-menu FPS readout and
   Timing Profiler window are unchanged and stay debug-only. Composite-cost recording is now
   release-available, and a `--profile` run dumps the table to stderr on exit under a pinned
   C numeric locale. **GPU timestamp queries deliberately deferred to S60-a** — they want one
   device and one submit path to instrument, so doing them across today's four independent
   devices would be work thrown away.

*Exit criterion: the app runs identically on the dev rig, and `--gui-frames` is clean under a
Vulkan-1.0-only device profile.* **MET.** Full `ctest` green; the whole unit suite green under
`MOSAIC_GPU_PROFILE=floor`; the GPU-lane tests run 5335 assertions on real caps and **0** under
the floor profile — all three lanes correctly refuse themselves and the CPU lanes serve, on
hardware that is nothing like the floor. Composite output byte-identical throughout.

**Landed alongside α (parallel sessions, §7's later items pulled forward because they were
genuinely disjoint):**
- **§7 S60-b item 15 — the persistent thread pool** (`common/thread_pool`): replaces
  spawn-per-call at `compositor.cpp`'s 18 sites, `blur.cpp`, `parallel_rows.hpp` and six inpaint
  sites. Band arithmetic copied verbatim, so the partition is unchanged and every golden is
  byte-exact. Nested calls run inline on the calling thread (a band body never enqueues, so no
  wait-for cycle can form); an instrumented build measured **zero** nested calls across the whole
  suite, so the rule costs no parallelism today.
- **§8.2 — the `--bench` harness**, seven scenarios.

**Thread-pool A/B, measured in-tree** (same loaded box, avg 7.8, the two binaries run back to
back — not the sub-agent's off-tree static-lib comparison, which reported 2-3x larger wins than
this and should not be quoted):

| scenario | before | after | |
|---|---|---|---|
| composite-region 32² | 0.118 ms | 0.098 ms | −17% |
| composite-region 64² | 0.342 ms | 0.297 ms | −13% |
| composite-region 128² | 1.719 ms | 1.320 ms | **−23%** |
| composite-region 256² | 6.032 ms | 5.357 ms | −11% |
| composite-region 512² | 18.985 ms | 18.135 ms | −4% |
| composite-full 512² | 9.227 ms | 8.213 ms | −11% |
| composite-full 1920×1080 | 141.5 ms | 133.8 ms | −5% |
| composite-full 3840×2160 | 557.9 ms | 535.5 ms | −4% |
| composite-full 5000×8000 | 2657.6 ms | 2556.3 ms | −4% |

All nine move the right way, and the shape is exactly what the mechanism predicts: the small ROIs
gain most (dispatch overhead dominates small work) and the win thins as pixel work takes over.
This is a real but modest win — it removes an overhead floor, it does not touch the inner loop.
The inner loop is S60-a's problem.
- **§3.3 — the readback-consumer audit** → `docs/s60-readback-consumers.md`. It found **19**
  consumers, not the five §3.3 guessed, and three of the six dangerous ones were not on the list
  at all. It also corrects this document's proposed API: four of the six want a few pixels *now,
  every frame*, which a future serves badly — `pinMirror()` / `peek()` is the load-bearing shape,
  not `requestReadback(rect) -> future`. §3.3's sketch is superseded by that document.

**One tradeoff introduced deliberately, recorded so it is not rediscovered as a bug:** one shared
pool means bounded total threads instead of oversubscription. If a long background job (inpaint,
texture render) is saturating the pool, a UI-thread composite now largely runs its own bands
rather than spawning a competing set. Better throughput overall; that specific "compositing during
a long inpaint" case can be slower than the old oversubscribed behaviour.

### S60-a — The resident tiled compositor (the main event)

**SCOPE ADDITION (user, 2026-07-23): the gesture-END stall is in.** `docs/s60-gesture-start-stall.md`
measured what the `--bench` baseline missed — ending a transform gesture costs MORE than starting
one (1712 ms quiet / 4672 ms loaded at 5000×8000, against 919/3068 for the start), and it is named
in no plan document and no bench case. The cause is structural rather than incidental: because the
whole drag ran on the GPU, `recompositeNow` was never called during it, so when `gestureEnded` →
`syncAfterEdit` invalidates the drag cache and asks for a full recomposite there is no cache to
reuse — the full CPU walk plus a 160 MiB upload, every time. Fixing only the start turns
*freeze → move → freeze* into *move → freeze*, which is not a fix. Item 11 (the resident
accumulator) is what makes the committed pixels already-correct and already-resident at gesture
end; the exit criterion below is extended accordingly.


7. ✅ **`TileGrid` / `TileKey` + dirty-set plumbing** — `core/tile_grid.{hpp,cpp}` (`140238f`) is
   the shared 64 px vocabulary; `TileSet::macrotiles(k)` is the projection to the dispatch grid.
   The *plumbing* landed with item 8/9's host half: `TileCompositor` owns a `TileSet` on the 64 px
   grid, `markDirty(rect)` / `markLayerDirty(id)` / `markAllDirty()` feed it, and a **plan diff**
   (per-layer fingerprint over transform/opacity/blend/filter/mask/clip + `contentRevision` +
   `maskRevision`) dirties the union of where a changed layer WAS and where it IS, so a nudge
   recomposites a handful of macrotiles rather than the canvas. ⚠ The app-side callers do not
   exist yet — nothing in `src/ui` calls `markDirty`, because item 13 is withheld.
8. ✅ **GPU tile atlas + residency + upload path** — `render/tile_compositor.{hpp,cpp}`. The
   accumulator is a macrotile-slot atlas in `R16G16B16A16_SFLOAT` (one image where the document
   fits `maxImageDimension2D`, several where it does not); the source cache is per-layer device
   images budgeted through `TileResidency` (`2c5d9d2`) + `atlasBudgetBytes`; uploads are staging
   copies, once per layer per content change — and since 2026-07-29 only the **dirty macrotiles**
   of that layer when the caller names the region (see "Incremental (dirty-macrotile) layer
   upload" below; a caller that names nothing still re-sends the layer whole).
   ⚠ Two deliberate gaps: `hostImageCopy` is NOT taken
   (a tier win on a path that is already off the per-frame loop), and a layer larger than
   `maxImageDimension2D` is a clean **refusal** rather than a windowed bind — the kernel already
   accepts a source window (`b877fe1`), the host wiring for it belongs with item 10's per-tile
   descriptor loop.
9. ✅ **The fused per-layer kernel** — `shaders/composite_tile.comp` (`7f5ce63`, `b877fe1`) does
   transform + resample + mask fold + clip + blend in one dispatch per macrotile per layer, and
   now also **publishes the clip base** (binding 4, `uClipWrite`) so a clip run never returns to
   host memory. Push block 120 → 124 bytes, still inside the 128-byte floor; 2 storage images
   against a guaranteed 4. Parity: `tests/test_composite_tile_parity.cpp` (kernel, per blend mode
   × per resample filter × mask/clip/window shapes) and `tests/test_tile_compositor.cpp` (the
   whole lane end to end, plus the residency and dirty-set properties a benchmark cannot see).
   CPU reference untouched.
10. ✅ **BUILT + MEASURED (2026-07-29) — three dispatch shapes, byte-identical pixels, and the
    measurement says the floor shape wins.** The dirty-macrotile list moved into a storage buffer
    (`composite_tile.comp` binding 5, two `ivec4` per macrotile); an invocation maps *itself* to
    `(macrotile, pixel)` through `gl_WorkGroupID.z`, collapsing `layers × dirty macrotiles`
    dispatches to `layers × atlas images` — i.e. `layers` for any document fitting
    `maxImageDimension2D`. It is available **at the 1.0 floor** exactly as this entry predicted:
    one storage buffer against a guaranteed four, plain `#version 450`, and a specialization
    constant (core 1.0) selecting the shape at pipeline creation. Push block unchanged at 124
    bytes. The two shapes carry **the same six integers** — the host writes into the list record
    precisely what the per-tile loop would have pushed — so the tests assert **byte-identity**,
    not a 1/255 tolerance: a dispatch reshape has no licence to move a pixel, and a tolerance is
    how that defect would survive to become a visible seam at one zoom level.
    The descriptor-indexed variant is the *same source file* compiled a second time at its own
    target env, replacing the per-layer source/mask bindings with one runtime-sized, partially-bound
    descriptor array. Its gate asks **two** questions, and the second one is easy to miss:
    `GpuCaps.descriptorIndexing` says the device can index descriptors, and the new
    `GpuCaps::spirvVersion` / `fitsSpirvVersion()` says it can *load* a blob compiled to use them.
    They come apart on a 1.0/1.1 device carrying `VK_EXT_descriptor_indexing` — real hardware, not
    a hypothetical — and without the second gate that is a `vkCreateShaderModule` failure in the
    field. Neither is a version test at a call site; the version arithmetic happens once inside
    `decide()`. Every refusal is named (`DispatchRefusal::NoDescriptorIndexing / SpirvUnsupported /
    DescriptorBudget / PipelineFailed / TooManyLayers`) and none is an error.
    **`TileDispatch::Auto` resolves to the LIST shape on every device, including devices that can
    serve the tier, and that is a measurement rather than a preference.** RX 6600 XT, quiet box, 20
    iterations, `gpu full` medians: per-tile 6.462 / list **5.683** / indexed 5.713 ms at
    3840×2160, and 27.805 / **23.313** / 23.331 at 5000×8000. The list shape takes the whole 12–16%
    win; indexing the *sources* on top of it removes descriptor writes the driver was not charging
    much for. On single-macrotile region rows all three are identical, as they must be — there is
    nothing to collapse. Equal speed at strictly greater risk (a second SPIR-V blob, a runtime-sized
    descriptor array, a device-dependent path) is not a default. `MOSAIC_TILE_DISPATCH=per-tile|
    list|indexed` forces a shape for a measurement run and keeps the tier exercised; a unit
    assertion pins the Auto→TileList choice so nobody changes it without a measurement to point at.
    The per-tile loop stays as the reference the parity tests hold the other two to, and as the live
    answer when a run exceeds `maxComputeWorkGroupCount[2]`. See "Item 10, and the oversized-layer
    gap" below: that gap is untouched and does **not** close here.
11. ✅ **Resident accumulator → the present texture, on the device; the per-frame readback is
    dead.** `shaders/tile_resolve.comp` + `TileCompositor::resolve(ResolveTarget&)` convert the
    macrotiles recomposited since the last resolve from the rgba16f accumulator straight into the
    8-bit document texture `canvas_present.comp` already samples. Zero host bytes in either
    direction: the chain *region composite → patch a doc-sized CPU mirror → copy the sub-rect →
    copy it again → memcpy into staging → `vkCmdCopyBufferToImage`* (three CPU copies before the
    staging write, per the readback audit §5) becomes one dispatch per dirty macrotile.
    `WindowRenderer::prepareResidentCanvas()` / `residentCanvasImage()` / `residentCanvasLayout()`
    / `noteResidentCanvasWritten()` are the four-line seam on the presenting side.
    ⚠ The present pass was deliberately NOT reworked to sample the atlas directly: that needs
    `canvas_present.comp` to grow atlas-slot arithmetic and a runtime-sized image array, which is
    item 10's machinery, and it would put the whole overlay stack (the loupe, the drag pass, the
    channel-isolation debt) through a rewrite for a saving of one full-screen pass. The resolve
    keeps every one of those working unchanged and still deletes 100% of the bus traffic, which is
    what the exit criterion measures.
12. ✅ **The explicit readback seam** — `render::CompositeReadback`
    (`src/render/composite_readback.{hpp,cpp}`), in the `pinMirror()` / `peek()` shape
    `docs/s60-readback-consumers.md` establishes rather than §3.3's `requestReadback(rect) →
    future` sketch. `Freshness` defaults to `AnyRecent` and `blocking` to false, so synchrony has
    to be typed; every entry point demands a `name`; the consumer vocabulary is a checked-in set
    (`render::consumers::*`) that `tests/test_composite_readback.cpp` pins, so a consumer cannot be
    added without declaring itself. Full routing table in the readback document's new §10.
13. **WITHHELD, and now MEASURABLE.** Flip `app_window`'s call sites off `Backend::Cpu`.
    The flip was withheld pending a measurement that no harness could take — `--bench` never
    touched the GPU lane and `Lane::Gpu` profiler rows were reporting submit wall-clock. Both gaps
    are closed below (the `tile-composite` scenario and GPU timestamp queries). **The flip itself
    is still not made, and must not be made on the strength of an argument.** The exact command
    and the criterion are in "Item 13: the gate, stated so it can be run" below.
14. **Gesture end pays nothing extra.** The committed transform must land from the resident
    accumulator rather than a fresh CPU walk. Add a `--bench` case for it (there is none) and a
    profiler scope, because an unnamed stall is how this one survived a whole benchmark pass.
    - ✅ The bench case landed: `--bench move-fullcanvas` now prints a `gesture-end` row per canvas
      size, beside the existing `gesture-start` one. It composites at `liveDrag=false` with a
      SUB-PIXEL committed placement, because that is what a real commit is and it is what stops
      `Auto` dropping to the cheap kernel — measuring it as a live-drag integer translate (which is
      what the existing drag rows do) measures a different, cheaper operation.
    - ✅ The **profiler scopes landed**, and the module-layering blocker was resolved the first of
      the two ways this entry offered: the collector moved from `src/ui/profiler.hpp` down to
      `src/common/profiler.{hpp,cpp}` (`mosaic::common`), FLTK-free and Vulkan-free like the rest of
      `common`, so `render` instruments itself without `ui` sitting under it. The Timing Profiler
      *window* stayed in `src/ui/timing_graph_window.{hpp,cpp}` — the visualiser is UI. Gating is
      unchanged: collection ships in every build behind `--profile` / `MOSAIC_PROFILE=1` (debug
      defaults on), the window and the FPS readout stay debug-only, and `frameTick` / `Present`
      stay runtime-gated only. New rows:
      - **`Move gesture end (composite+upload)`** — the G3 stall, recorded on the `onFrame` drain
        that a Move release queued, beside the `Composite (full)` row it used to hide inside.
        Once per gesture: read `max`/`last`, not `avg`.
      - **`Drag texture upload`**, and inside it **`Sampled texture upload`** split into
        **`(host copy)`** and **`(fence wait)`** — G5. The two halves are separated because their
        fixes differ: a smaller backdrop / a mapped staging ring versus an asynchronous submit.
      - **`Adjustment layer (spatial)`** / **`(scalar)`** — adjustment cost had no row at all,
        because it is paid inside `compositor.cpp`'s tree walk. Scoped inside `walkStep`'s
        adjustment branch, *after* the `dynamic_cast` has succeeded, so the row counts real work
        rather than one near-zero sample per layer per frame. Composited output is byte-identical.

#### GPU timestamp queries — §8.1's deferred item, BUILT 2026-07-28

S60-α item 6 deferred these "to S60-a, because they want one device and one submit path to
instrument, and doing them across today's four independent devices would be work thrown away".
Both preconditions now hold (`VulkanContext::shared()` / `adopt()`; `VulkanContext::submit()` is
the only queue submission in `src/render` outside the swapchain), so they are built.

- **`render::GpuTimer`** (`src/render/gpu_timer.{hpp,cpp}`) — a `VK_QUERY_TYPE_TIMESTAMP` pool and
  the bookkeeping to turn tick pairs into profiler rows. Vulkan **1.0 core throughout**
  (`vkCreateQueryPool` / `vkCmdResetQueryPool` / `vkCmdWriteTimestamp` / `vkGetQueryPoolResults`),
  so it needs no tier — but it is still caps-gated, and under `MOSAIC_GPU_PROFILE=floor` the gate
  refuses it and the lane runs exactly as before with no device rows. A null timer is an ordinary
  outcome, never an error.
- **A third lane: `common::Lane::GpuDevice`.** Rows are keyed by `(name, lane)`, so
  `"Tile composite"` now appears **twice** — once on `Lane::Gpu` (wall-clock at the call site:
  host planning + descriptor writes + staging memcpys + submit + fence) and once on
  `Lane::GpuDevice` (what the device actually spent). **The gap between the two is the diagnostic**,
  and it is the thing every `Lane::Gpu` row in the tree was hiding: a lane that is fence-bound and
  a lane that is shader-bound look identical from the call site, and only the second is worth
  writing a shader for. `common::laneName()` is the one definition of the CPU/GPU/DEV tag, so the
  Timing window's pill and the `--profile` dump cannot disagree.
- **`GpuCaps.timestampValidBits`**, probed from `VkQueueFamilyProperties` (it is a *queue-family*
  fact, not a limit) as the minimum over the graphics/compute families. Vulkan says the high
  `64 - validBits` bits of a query result are **undefined, not zero**, so an unmasked subtraction
  on a 36-bit counter returns garbage; and the counter *wraps*, roughly every 69 s at 1 ns/tick, so
  a straddling frame must still read correctly. Both are pure functions
  (`maskTimestamp` / `timestampDeltaMs`) with synthetic-tick unit tests, because neither case can
  be provoked on the dev rig without waiting for a turnover.
- **`TileCompositor` is instrumented**, and it had **no profiler row at all** before this — the
  main event of S60-a was entirely invisible to the build the user runs. Device rows: `Tile
  composite` (the whole submission), `Tile upload`, `Tile clear`, `Tile blend` (the fused kernel —
  *the only row a shader change may move*), `Tile resolve`. Host rows: `Tile composite (plan)`,
  `(diff)`, `(fence wait)`, and `Tile readback (accumulator)` / `(8-bit target)` — readback gets
  rows precisely because it is the thing that must **not** happen per frame, per the readback
  document's §7(a) "a new consumer shows up as a number that moved".
- `Tile upload` is the residency assertion a wall clock cannot make: on a steady frame it must be
  ~0 ms with nothing transferred. A device-time upload row that is consistently non-zero means
  residency is not working, whatever the total says.

**Not adopted yet, and deliberately:** `BlurGpu`, `ExtrudeGpu`, `TextureGpu` and `WindowRenderer`
still report submit wall-clock on their `Lane::Gpu` rows. Adopting `GpuTimer` in each is three
lines (`beginSubmission` / `beginScope` + `endScope` / `resolveAndRecord` after the existing
fence wait), but `WindowRenderer` in particular is a large, concurrently-edited file and the value
is much lower than instrumenting the lane the arc is about. Left as a follow-up, not as a gap.

#### The presenting-device question, settled (2026-07-28)

S60-α item 3 left `WindowRenderer` on its own device and deferred the fold to here, "where the
resident composite must live on the presenting device by construction rather than by guess". The
call: **`TileCompositor` is constructed on the presenting device**, not the other way round.

- **It is forced, not preferred.** A `VkImage` does not cross a `VkDevice`, and external memory is
  not in the Vulkan 1.0 core this arc floors on. Item 11 needs the accumulator and the present
  target on one device; there is no version of that where they are on two.
- **The other direction cannot be made safe.** `VulkanContext::shared()` is created lazily by
  whichever compute lane runs first, has no surface, and cannot select a present-capable queue
  family without one. Making it surface-aware means either requiring a window before any headless
  lane runs — false for `--bench`, `--composite-demo`, `--texture` and every unit test — or
  destroying and recreating the shared device once a window appears, while lanes hold `shared_ptr`s
  to it and have live pipelines on it. Both are worse than what they buy.
- **§1.2's "one device" argument does not apply to this lane.** That rule exists because a hand-off
  between two compute lanes round-trips through host memory. `TileCompositor`'s *inputs* are CPU
  `core::RasterLayer` pixels, and its one *output* consumer is the present pass. Joining the
  presenting device removes a host round trip; it does not add one. The device count is unchanged
  at two.
- **Mechanism: `VulkanContext::adopt()`** wraps an externally-owned instance/physical
  device/device/queue in the same class every lane already borrows, so `TileCompositor` needed no
  new abstraction and the headless path is the identical code. An adopted context destroys only its
  own command pool. ⚠ Its queue mutex cannot see the adopter's own `vkQueueSubmit`, so adopter and
  borrower must submit from one thread — the UI thread, where both already run. That constraint is
  written on `adopt()` and is the one thing to re-read when S60-c moves compositing off the UI
  thread.
- **The way out later:** once this seam exists, seeding `VulkanContext::shared()` *from* the
  window's device is a small change, and it would give §1.2's ideal (one device for everything)
  without the ordering problem — because by then the window's device is already wrapped in the
  right type. Not done here; it would change which device every existing lane runs on, which is a
  measurement question, not a refactor.

#### Item 13: the gate, stated so it can be run

`--bench tile-composite` (new, 2026-07-28) is the instrument. It runs **both lanes on the same
document in the same process**, back to back, and prints four rows per canvas size:

| row | what it is |
|---|---|
| `cpu full` | `render::composite(..., Backend::Cpu)` — today's full walk |
| `cpu region 256` | `render::compositeRegion(...)` over one 256 px macrotile — today's brush/typing path |
| `gpu full` | `markAllDirty()` + `composite()` + `resolve()` into the present texture |
| `gpu region 256` | `markDirty(rect)` + `composite()` + `resolve()`, **no content change** — the pure dirty-set path |
| `gpu edit 256` | a real 256² pixel edit + `invalidateContentBounds()` + `markLayerDirty()` + composite + resolve |
| `cpu full +upload` | the same full composite **plus the app's present chain** (mirror, 2 host copies, staging write, copy-to-image, submit, fence) |
| `cpu region 256 +upload` | the same region composite **plus the app's present chain** — the row condition 1 should really be read on |

Two things about the comparison, both of which must be repeated by anyone who quotes it:

1. **Compare the `+upload` rows, not the bare `cpu` ones.** `cpu full` and `cpu region 256` are the
   composite half ONLY; the `gpu` rows have always included `resolve()`, so comparing them is
   comparing four fifths of one frame with all of another. `cpu full +upload` and `cpu region 256
   +upload` (2026-07-29) carry the app's present chain — the three host copies plus the staging
   write of the readback audit §5 — staged headlessly into a canvas texture with the real usage
   flags. The old caveat "a tie in the table is a GPU win in the app" is retired FOR THOSE ROWS and
   still applies to the two composite-only ones. Two symmetric residuals remain: both lanes pay one
   extra submit + fence the app batches into its frame, and neither pays the present pass.
2. **`gpu edit 256` is the row to read hardest.** A pixel edit bumps `contentRevision`, and this
   cut uploads a changed layer **whole**. If that row is bad on a large canvas, the next item is
   per-tile source upload — *not* the flip. A flip made on the `gpu region` row alone would ship a
   compositor that is excellent at everything except painting.

**The command** (loaded-system primary, per [[mosaic-working-with-claude]]; quiet-system as a
footnote), run against a **release** build:

```
# primary — loaded box, the two lanes are inside one process so they see the same load
build/linux-release/mosaic --bench tile-composite --bench-iterations 20
build/linux-release/mosaic --bench present-upload --bench-iterations 20   # the present half alone, both lanes
# footnote — quiet box, same command, and note the load state beside the numbers
# floor sanity — the lane must refuse itself and the CPU rows must still print
MOSAIC_GPU_PROFILE=floor build/linux-release/mosaic --bench tile-composite
# where the GPU time actually goes, once the table says something interesting
MOSAIC_PROFILE=1 build/linux-release/mosaic --bench tile-composite   # DEV rows on stderr at exit
```

**Pass/fail for item 13** — flip only if **all** of these hold on the loaded box, median:

- `gpu region 256` **<** `cpu region 256` at 1920×1080 **and** at 3840×2160. This is the
  interactive frame and it is the whole point; a loss here ends the discussion.
- `gpu full` **<** `cpu full` at 1920×1080 **and** at 5000×8000 (or `gpu full` is *skipped by a
  named refusal* at 5000×8000, in which case the flip must keep the CPU fallback on that path and
  say so).
- `gpu edit 256` **≤** `cpu region 256` × 1.25 at 3840×2160. A modest regression on the edit path
  is tolerable *only* because the CPU rows are optimistic; anything worse means the whole-layer
  upload has to be fixed first.
- Under `MOSAIC_PROFILE=1`, `Tile upload` on `Lane::GpuDevice` is ~0 for the `gpu region` rows.
  If it is not, residency is not working and the totals are measuring something else.
- `ctest` green, and green again under `MOSAIC_GPU_PROFILE=floor`.

If any of those fails, **item 13 stays withheld** and the failing row names the next item. That is
what keeping the flip last is for.

#### The gate was RUN — 2026-07-28. Verdict: **item 13 stays withheld.**

Release build, loaded box (load average 2.34), 20 iterations, medians in ms:

| canvas | cpu full | gpu full | cpu region 256 | gpu region 256 | gpu edit 256 |
|---|---|---|---|---|---|
| 512×512     |    8.911 |   0.770 | 3.321 | 0.718 |  1.286 |
| 1920×1080   |  131.684 |   2.095 | 3.266 | 1.070 |  5.382 |
| 3840×2160   |  523.954 |   6.540 | 3.114 | 1.682 | 18.804 |
| 5000×8000   | 2409.680 |  28.375 | 3.133 | 4.475 | 90.487 |

- ✅ **Condition 1** — `gpu region` beats `cpu region` at both sizes (1.070 vs 3.266; 1.682 vs 3.114).
- ✅ **Condition 2** — `gpu full` beats `cpu full` everywhere, by **63×** at 1920×1080 and **85×** at
  5000×8000. The resident lane is not marginally better at a full composite, it is a different
  order of magnitude.
- ❌ **Condition 3 — FAILS, and it is not close.** `gpu edit 256` at 3840×2160 is **18.804 ms**
  against a budget of `3.114 × 1.25 = 3.893 ms` — **4.8× over**. A 256×256 edit costs 18.8 ms on the
  GPU lane and 3.1 ms on the CPU walk, so flipping today would make **the brush and typing path —
  the most latency-sensitive thing in the app — roughly 6× worse** while making everything else
  vastly faster. That is precisely the trade this condition exists to refuse.
- ⚠ **Condition 4 — not cleanly measurable.** The profiler aggregates per PROCESS, not per bench
  case, so a single `--bench` run mixes the region rows with the edit rows that re-upload by design;
  the `Tile upload` DEV row cannot be attributed to the `gpu region` rows as the condition is
  worded. What the run does show: `Tile upload` is **30.56 ms of 46.33 ms of total device time
  (66%)**, and a 4K region composite completes in 1.682 ms total — far too little to contain a
  whole-layer upload. So residency *is* working on the region path, and the upload cost is
  concentrated in the edit path, exactly where condition 3 fails. **Fixing the instrument: the bench
  needs per-case profiler attribution before this condition can be checked as written.**
- ✅ **Condition 5** — `ctest` green (2501 cases / 1,561,248 assertions) and green again under
  `MOSAIC_GPU_PROFILE=floor`, where the 5000×8000 GPU rows correctly refuse by name (`resolve target
  exceeds maxImageDimension2D`, the 4096 floor).

**So the failing row names the next item, as intended: incremental upload.** An edit dirties one
256×256 macrotile and the lane re-uploads the whole layer. Upload only the dirty macrotiles of the
edited layer and condition 3 should collapse; then re-run this gate unchanged.

#### Incremental (dirty-macrotile) layer upload — BUILT 2026-07-29, ⚠ THE GATE RE-RUN IS OWED

Condition 3's fix, built. **Nothing below has been compiled, run or measured** — the session that
wrote it was not permitted to build — so every number in the table above still stands as the last
measurement, and **the verdict on item 13 is unchanged: withheld until the gate is re-run.**

**What the failing row actually was.** `gpu edit 256` paid twice for one 256 px edit, and only one
of the two was named in the verdict:

1. the whole layer was **re-uploaded** (`Tile upload` was 66% of device time), and
2. the whole layer's **footprint was recomposited** — because the per-layer plan fingerprint
   hashed `contentRevision` in with the transform, so "the layer moved" and "one block of the layer
   was repainted" were the same event, and the only answer to that event is to dirty the union of
   where the layer was and where it is. On a **full-canvas** layer — which is what the bench stack
   and most real documents have at the bottom — that union is the canvas.

Fixing only (1) would have left a full-canvas recomposite behind every brush dab, so both are
fixed, and the second one is why the plan diff changed shape.

**The API.** One new call, an overload of the existing one, and the existing one is untouched:

```cpp
// EVERYTHING about this layer's pixels changed (unchanged semantics: drops the device copy).
void markLayerDirty(core::LayerId id) noexcept;

// ... and WHERE. `layerRect` is in LAYER-LOCAL pixels -- the space RasterLayer::image() is
// indexed in, not document space -- because that is the space the upload copies out of.
void markLayerDirty(const core::RasterLayer& layer, const common::Rect& layerRect) noexcept;
```

- **The dirty vocabulary is the shared one.** Each resident layer carries a `core::TileSet` on the
  64 px grid *sized to its own image*, projected through the same `TileSet::macrotiles(k)` the
  dispatch uses. An upload therefore cannot refresh less than a recomposite is about to read, and
  two disjoint dabs cost two macrotiles rather than the box that bounds them.
- **Coalescing and the cap.** Dirty macrotiles are coalesced into runs along each macrotile row —
  one `VkBufferImageCopy` per run, packed tightly into one staging buffer. Past
  `kMaxUploadRegions` (64) runs, or once the runs add up to the whole image, the lane takes the
  full upload instead: one copy is cheaper than N and always correct.
- **It degrades safely, and the class enforces that rather than trusting the caller.** A full
  upload happens whenever: no region was given; the rect is empty; the layer has no device copy
  yet; the image was resized (every tile index in the ledger means different pixels then); the
  mask revision moved or a mask was added/removed; **or a `contentRevision` step went by that no
  rect described.** That last one is the dangerous case — an unnamed edit followed by a named one
  — and it is caught by requiring the ledger to advance one revision step at a time.
- **⚠ The layout, the one place this path invites a silent defect.** A partial upload transitions
  the layer image `SHADER_READ_ONLY_OPTIMAL → TRANSFER_DST_OPTIMAL`, **never from `UNDEFINED`**. A
  transition out of `UNDEFINED` may discard the image's contents, which would throw away every
  pixel the copies do not rewrite — and it would look perfect on the frame right after a full
  upload and wrong on the next one. `tests/test_tile_compositor.cpp` pins it by comparing a
  partially-uploaded composite with a full re-upload **byte for byte**.
- **Pixels do not move.** This is a pure transfer optimisation; the parity tests are unchanged and
  the new cases assert byte-identity against the full path as well as 1/255 against the CPU walk.
- **No app-side caller exists, by design.** Item 13 is still withheld and `src/ui` calls nothing
  here. When the flip is made, the caller is `SetLayerPixelsCommand` / the brush's
  `syncAfterEdit` seam — wherever the autosave journal is already told which tiles moved, which is
  the point of sharing the `TileKey` vocabulary.

**New counters** on `TileCompositeStatus` / `TileCompositeStats`: `uploadRegions`,
`partialUploads`, `fullUploads`, beside the existing `uploadBytes`. They are **event** counts on
purpose — a cache SIZE cannot witness a re-upload, because re-sending a layer's whole image leaves
`residentSourceBytes()` exactly where it was.

**`--bench tile-composite` changes, and how to re-run the gate.** The command and the pass/fail
criterion are **unchanged**; two things about the table are not:

- `gpu edit 256` now marks the edit **by region**, which is what an app-side caller would do. The
  pre-fix behaviour is kept beside it as a standing baseline row, **`gpu edit 256 whole`**
  (`markLayerDirty(id)`), so one run shows both and the difference between them is this slice.
  Condition 3 is read on `gpu edit 256`, as written.
- **Per-case profiler attribution**, which is condition 4's fix. `common::Profiler` aggregates per
  PROCESS, so one `--bench` run used to pile every case's samples into the same `Tile upload` row
  and the condition could not be checked as worded. Each case now clears the collector before its
  timed loop and snapshots after it, and `--bench` prints a second table of the **`Lane::GpuDevice`
  rows attributed to each case** (n / min / avg / max / total, where total = avg × n). No profiler
  API changed — `clear()` and `snapshot()` were already public.
  ⚠ Consequence: under `--profile` / `MOSAIC_PROFILE=1` the stderr dump at process exit now shows
  only the **last** case's rows. The per-case table is the one to read.

So the gate re-run is exactly:

```
build/linux-release/mosaic --bench tile-composite --bench-iterations 20
MOSAIC_PROFILE=1 build/linux-release/mosaic --bench tile-composite   # per-case DEV table on stdout
MOSAIC_GPU_PROFILE=floor build/linux-release/mosaic --bench tile-composite
```

with the same five conditions, plus the two readings this cut makes checkable:

- condition 3 on `gpu edit 256` (`≤ cpu region 256 × 1.25` at 3840×2160), and
- condition 4 read off the per-case table: `Tile upload` on `Lane::GpuDevice` ~0 for
  `gpu region 256`, and small — a macrotile, not a layer — for `gpu edit 256`.

**⚠ Owed, and not to be written up as done until it is run:** the gate itself. The expectation is
that `gpu edit 256` collapses toward `gpu region 256` plus one macrotile of transfer; the
expectation is not a measurement, and the table above must not be edited until one exists.

⚠ **A harness bug found by running it, fixed in the same pass:** the ROI walk was
`64 + (i*137) % 1024`, borrowed from `composite-region` whose document is 4096 square. On the
512-square canvas the rect left the document from the fifth sample, `compositeRegion` correctly
refused, and the row was **skipped** — silently removing the CPU baseline that the 512² GPU row is
measured against. The walk is now bounded by the document's smaller dimension. The first run of this
gate therefore had no `512×512 cpu region 256` row at all; the table above is from the fixed build.

#### Item 10, and the oversized-layer gap

**Item 10 is DONE (see its entry above); the oversized-layer gap is NOT, and did not close as a
side effect.** The scoping notes below stood up, including the one that mattered — "worth measuring
before reaching for the extension" turned out to be the whole answer, and the extension is now
built but not the default. Kept for the record:

- The dispatch count *was* `layers × dirty macrotiles`, one descriptor-set bind per
  `(layer, atlas image)`. The descriptor-indexed variant collapses that to one dispatch per layer.
- What it needs: a second SPIR-V blob at its own target env (`GL_EXT_nonuniform_qualifier`), a
  runtime-sized descriptor array of source images, the dirty-tile list in an SSBO so an invocation
  can map itself to `(macrotile, pixel)`, and a `GpuCaps.descriptorIndexing` gate with the per-tile
  loop kept beside it. Note the SSBO half is available **at the 1.0 floor** (1 storage buffer
  against a guaranteed 4) and is most of the win on a single-atlas document — worth measuring
  before reaching for the extension.
- **The oversized-layer gap does NOT close as a side effect, and the plan's item 8 note is
  optimistic about this.** The kernel accepts a source *window*, but it binds ONE source per
  dispatch, so a macrotile whose inverse-mapped footprint straddles two windows cannot be served by
  one dispatch — and under rotation or strong minification that is the common case, not the corner.
  Closing it honestly needs either (a) per-macrotile window selection with a refusal when the
  footprint straddles, which has a correctness cliff exactly where the pictures get interesting, or
  (b) the kernel accumulating over several windows in one dispatch, which is a kernel change.
  `TileRefusal::LayerTooLarge` remains a clean refusal until one of those is designed.

#### The leaf kinds the lane serves — 2026-07-29

The lane originally planned RASTER leaves only, so a single text layer refused the whole document
and the entire Type tool (2D and 3D alike) composited 100% on the CPU with none of S60-a's win
available. It now serves every leaf that reads a **fixed-resolution source image** — raster, magic,
text and texture — because `compositor.cpp`'s `renderLayerRaw` treats all four identically: one
fused pass, a linked mask folded at the source pixel, an unlinked one after placement. The two
cache-backed kinds differ in exactly one factor, `place = pre * layer.transform() *
cacheImageToLayer()`, and everything the plan derives (inverse, resolved filter, sub-sample count,
per-axis scale, filter-footprint pad, dirty bounds, fingerprint) comes off that full product.

Three facts worth not rediscovering:

- **VECTOR stays refused, and not for want of plumbing.** `core::vec::rasterizeObjectF` evaluates
  the object at TARGET resolution through the placement, so there is nothing of fixed size to make
  resident; a bitmap stand-in would draw a *different* picture at every zoom.
- **A text/texture cache goes stale WITHOUT `contentRevision` moving**, so the device copy is keyed
  on a new `cacheGeneration()` counter bumped inside `setCachedImage` (core/layer.hpp). The
  re-renders that leave the content revision standing still are the DRAFT (half-res) bake taken
  during a live block edit or a font hover and the crisp pass that replaces it, an Area block's
  clip flip, a 3D overlay re-bake, and a texture layer's canvas-resize re-render — every one of
  them replaces the pixels while `cacheCurrent()` goes back to reading true. A `contentRevision`
  key passes every other test in the suite and shows the draft bake forever; the regression net is
  `tests/test_tile_compositor.cpp`'s "a cache re-render the content revision cannot see still
  changes the picture".
- **The float (sky) texture cache is uploaded as `R16G16B16A16_SFLOAT`, not quantised to 8 bits.**
  No shader change: `composite_tile.comp` reads `uSrc` with `texelFetch` only, so any float or
  UNORM format serves. ~11 bits of mantissa is three bits finer than the 8-bit round trip the
  float lane exists to avoid, and it is the precision the accumulator already carries. The arm
  restates its own precondition (`GpuCaps::workingFormat == rgba16f`, refusing `DeviceTooSmall`)
  even though `create()` already refuses the whole lane on it — that redundancy becomes load-
  bearing the day the accumulator gains a second working format.

#### Adjustment layers: a SECOND kernel, admitted per kind — 2026-07-29

`planDocument` used to answer `TileRefusal::Adjustment` for any `core::AdjustmentLayer`, so **one
adjustment layer sent the whole document to the CPU walk** — and an adjustment stack is what a real
edit *is*, which made this the largest remaining hole in the lane after the leaf kinds closed.

An adjustment is **not a source**: it is a function of the accumulated backdrop, which on this lane
is the resident accumulator. So it takes a second kernel, `shaders/adjust_tile.comp`, over the same
dirty macrotiles — load the accumulator, apply a per-pixel transfer, store it back — under
`walkStep`'s modulation (opacity × layer mask × clip-to-below). It shares the composite kernel's
**descriptor set layout and pipeline layout**, so the descriptor pool, the per-(layer, atlas image)
set allocation and all three dispatch shapes are unchanged; the pipeline bind is the whole switch.
Nothing round-trips to the host, so `stats().readbacks` stays at zero and `uploadBytes` does not
move for an adjustment that carries neither a mask nor a lookup table.

**Admission is PER KIND, and that is the design decision, not an implementation shortcut.**
`core::AdjustmentKind` has ~30 members; porting all of them well is several passes, and a partially
ported lane that guessed at an unported one would draw the wrong picture — which is strictly worse
than being slow. So the served set is the **per-pixel, finite-slope** transfers and nothing else:

| served | refused, and why |
|---|---|
| Invert, Brightness/Contrast, Levels, Exposure, Hue/Saturation, Color Balance, Grayscale (its per-pixel projections, continuous palette), Curves, Gradient Map, Vibrance, Photo Filter, Haze Removal | **Threshold, Posterize, quantised Grayscale** — a lattice; see the bound below. **Matte Removal** — conditioned on 1/alpha. **PhotometricMatch** — 13 scalars against 12 push lanes, plus a row-dependent term. **Every spatial kind** (the S33 blur family, Shadows/Highlights, Defringe, High Pass, Grayscale's Dithered/Adaptive methods) — reads a neighbourhood the dirty set does not cover. **Every S35 stylize kind** — owns its own mask/clip blend, so it is not even the same shape of step. |

**The bound that draws the line**, and it is a property of the accumulator rather than of any kind:
the CPU reference works in fp32 and the accumulator is `rgba16f`, so the backdrop the kernel reads
differs from the reference's by ~2⁻¹¹ relative (~2.4e-4 near 0.5), and a transfer amplifies that by
its own |f′|. A finite slope stays inside the existing 1/255 parity bound. A **lattice** has an
infinite slope at every step and flips a *whole level* for any pixel that lands near one — on a
smooth gradient that is thousands of pixels, not a risk. Those kinds refuse rather than widening
the tolerance. Grayscale therefore refuses **per instance**: served at `grays == 256` (continuous,
the default), refused the moment the user asks for N greys.

Four facts worth not rediscovering:

- **An adjustment's BLEND MODE reaches neither lane.** `walkStep` takes the adjustment branch before
  any `blend()` call, so the mode never touches a pixel in the golden lane. The kernel mirrors the
  omission, and `planDocument` deliberately hashes `BlendMode::Normal` into the fingerprint instead
  of the layer's real mode — otherwise changing it would dirty the whole canvas for a recomposite
  that produces identical bytes. `tests/test_tile_compositor.cpp` pins the claim **in the CPU
  reference first**, so a future change fails there rather than blaming the shader.
- **A masked adjustment's footprint is still the WHOLE CANVAS.** `adjustmentMaskAt` *clamps* its
  sample into the sheet's parent-space domain instead of reading zero outside it, so the edge
  texel's coverage carries on to the canvas border. There is no rectangle outside which a masked
  adjustment is guaranteed to be a no-op, so there is none to name in the dirty set.
- **A parameter edit is visible ONLY through the plan-diff fingerprint.**
  `SetAdjustmentParamsCommand` replaces the params bag and touches nothing else — no
  `contentRevision`, no `maskRevision`, no transform. So `planDocument` hashes the *resolved*
  scalars into `fingerprint`, and for the two lookup kinds hashes the *whole bag* into
  `sourceRevision` as the transfer table's staleness key. This is the `cacheGeneration()` trap in a
  second dress, and it is pinned the same way.
- **Curves and Gradient Map ride a 256×1 transfer TABLE**, built host-side and uploaded through the
  existing float-leaf path as `R16G16B16A16_SFLOAT` — so they cost no upload code of their own, and
  ~11 bits of mantissa on a [0,1] table is ~0.06/255. It occupies the composite kernel's `uSrc`
  binding, which is free here for the same reason the per-tile push lanes are free in the list
  shapes: an adjustment has no source. Every other served kind binds the 1×1 stand-in and reads
  nothing.

**Future work, in the order it is worth doing.** (1) The lattice kinds need the *reference* to be
reachable at fp16, not a wider tolerance — the honest route is a kernel that quantises against the
same value the CPU sees, which means resolving what the accumulator's precision contract actually
is. (2) The spatial kinds need the dirty set grown by the kernel's reach
(`render::blurAdjustmentReach` already computes it) plus a second accumulator to gather from; that
is a real design, not a port. (3) The stylize family needs its own blend path mirrored. (4)
PhotometricMatch is a straight transcription once its scalars have somewhere to ride.

#### The gate was RE-RUN — 2026-07-29. Verdict: **all five conditions PASS.**

⚠ **The GPU rows in this table are stale as of the bench's resolve-layout fix.** `--bench` never
carried `ResolveTarget::layout` forward, so `resolve()` took its FULL-CANVAS branch on every call
and the `gpu region` / `gpu edit` rows were paying a whole-canvas resolve while claiming the
incremental path. The app (`resident_composite.hpp`) and the tests always carried it; only the
bench did not. The rows get FASTER, so this can only turn a failure into a pass — re-read the gate,
do not assume it. See docs/s60-bench.md §5.

Release build, quiet box (load average 2.62, comparable to the 2026-07-28 run's 2.34), 20
iterations, medians in ms. ⚠ The command in this section is `build/linux-release/**bin**/mosaic`;
the path written above was missing `bin/`.

| canvas | cpu full | gpu full | cpu region 256 | gpu region 256 | gpu edit 256 | gpu edit 256 whole |
|---|---|---|---|---|---|---|
| 512×512     |   10.135 |  0.741 | 4.258 | 0.703 | 1.355 |  1.290 |
| 1920×1080   |  146.598 |  1.692 | 3.722 | 0.910 | 1.479 |  4.974 |
| 3840×2160   |  566.470 |  5.840 | 3.183 | 1.685 | 2.219 | 19.046 |
| 5000×8000   | 2581.425 | 24.577 | 3.466 | 4.715 | 5.277 | 89.911 |

- ✅ **Condition 1** — 0.910 vs 3.722 at 1920×1080, 1.685 vs 3.183 at 3840×2160.
- ✅ **Condition 2** — 87× at 1920×1080, **105×** at 5000×8000.
- ✅ **Condition 3 — the row that withheld the flip for three sessions.** 2.219 ms against a budget
  of `3.183 × 1.25 = 3.979` — **56% of budget, from 4.8× over it.** The standing `gpu edit 256
  whole` baseline beside it (19.046 ms) is what makes this attributable rather than lucky: same
  edit, same document, whole-layer upload, **8.6× slower**. At 5000×8000 the same comparison is
  5.277 vs 89.911, i.e. **17×**.
- ✅ **Condition 4**, now checkable as written thanks to per-case attribution. `Tile upload` on
  `Lane::GpuDevice`, avg ms: **0.000–0.001 on every `gpu region` row** (residency is real), and
  **0.161 / 0.171 on the `gpu edit` rows at 1920 and 3840** — a macrotile, not a layer — against
  1.279 for `gpu edit 256 whole`.
- ✅ **Condition 5** — `ctest` green, and green again under `MOSAIC_GPU_PROFILE=floor`, where the
  5000×8000 GPU rows still refuse by name (`resolve target exceeds maxImageDimension2D`).

⚠ **A first attempt at this gate was run on a box loaded to 9.35 by the session's own subagents,
and it should be read as a caution rather than a result.** Every condition still passed, but the
condition-1 margin at 3840×2160 collapsed to 0.38 ms (3.280 vs 3.661) with the GPU row *losing* on
the mean, and `5000×8000 gpu region`'s `Tile resolve` read 17.283 ms against 0.437 at 4K, which
looked like a scalability defect in the region path and was not one — the quiet-box row is 4.715 ms
total. **The GPU rows are far more load-sensitive than the CPU rows**, because their host half is
staging copies, submit and fence wait; the CPU rows are pure compute. A "loaded box primary" means
*ordinarily* loaded, not loaded by the measurement's own toolchain.

**STATUS 2026-07-29 (second session): the gate PASSES, and the app-side wiring for item 13 exists
behind `MOSAIC_TILE_COMPOSITOR=1`, default OFF.** Incremental (dirty-macrotile) layer upload,
item 10, and the item 13 wiring are all compiled, tested and measured. **The flip itself is still
not made** — see "Item 13: what remains" below for the one thing the gate cannot answer.

#### Item 13: the app-side wiring — BUILT 2026-07-29, behind an opt-in, flip NOT made

`ui::ResidentComposite` (`src/ui/resident_composite.hpp`) is the only thing in `src/ui` that
touches `TileCompositor` or `CompositeReadback`. It is built on the first frame that has a renderer
to adopt, on the **presenting device**, and `MOSAIC_TILE_COMPOSITOR=1` is the only way to get one.
With the variable unset, all twelve `Backend::Cpu` call sites are untouched and every seam is a
null test.

- **The dirty set is fed from the EDIT seams, not the recomposite seams** — the brush's per-dab
  preview, `pushScopedPixelEdit`, the inpaint preview blit, and undo/redo through a new
  `core::Command::dirtyLayerPixels` overridden only by `SetLayerPixelsCommand`. That is deliberate:
  the rect at the edit is layer-local and exact, whereas a doc-space bbox inverse-maps to a
  *different* region under rotation. A unit test pins a 45° layer where the document AABB is
  **narrower** than the layer-local rect in one axis and taller in the other — proof that a
  document rect fed to a layer-local API does not merely over-copy, it copies the wrong texels.
- **Refusals latch for the gesture.** Skips the compositor cannot see (channel isolation, a
  Recompose review, a live Move drag) join its own named refusals; a refusal taken mid-stroke keeps
  the CPU lane for the rest of that stroke rather than alternating; reasons log once per change,
  not per frame. Every CPU↔resident boundary forces `markResolveDirty()`, because a CPU upload
  writes the canvas texture behind the compositor's back — without it the first resident frame
  after a CPU one is right and the next one is wrong.
- **⚠ The device does not die in a destructor.** `VulkanCanvas::hide()` destroys the renderer (FLTK
  tears it down before freeing the native window) and runs long before `~MainWindow`. Releasing the
  lane in the host's destructor therefore releases it *after* its `VkDevice` is gone — observed as
  30 leaked objects from the validation layer and a `vkDeviceWaitIdle` on an invalid handle. The
  lane is released from `VulkanCanvas::setOnRendererShutdown`, fired on both teardown paths with
  the device alive and idle, and it clears the creation latch so a re-shown canvas rebuilds on its
  **new** device instead of keeping a dangling one.
- Readback consumers are funnelled through a lazily-materialised mirror with named consumers
  (`docs/s60-readback-consumers.md` §10). One is unrouted: the clone-stamp source snapshot
  postdates the audit's 19 and has no name in `render::consumers`, so under the opt-in it pays an
  honest full CPU walk — correct, slower.

#### ⚠ The first interactive pass, and the defect it found — FIXED 2026-07-29

The user ran the opt-in and reported the lane was **extremely laggy**, with 25–30 ms spikes on
every resident row (`Composite (full)`, `Composite (resident)`, `Resident composite (serve)`,
`(mirror refresh)`, `Tile readback (accumulator)`) and no lag at all with the opt-in off. **The
resident lane was slower than the CPU walk it replaces**, on a build whose bench said it was 87×
faster. Both facts were true, and the gap between them is the lesson.

`Tile readback (accumulator)` firing at all was the diagnosis — that row exists precisely because
it must never happen per frame. `CompositeReadback::refreshMirror()` was called from `serve()` on
every frame and short-circuits when the accumulator's revision has not moved, **which is exactly
the wrong memo**: during an edit the revision moves every frame, so the memo never hits. One held
cursor-readout pin — i.e. the pointer anywhere over the canvas, which during a brush stroke is
always — therefore put a device→host transfer **and a fence** on every frame of every stroke, to
keep a status-bar colour current. The cost is the fence, not the 256 KB: it serialises the frame
against the device. `materialise()` had the same broken memo over a **blocking full-canvas**
readback (~33 MB at 3840×2160), which the histogram provider asks for with `AnyRecent`, so with
the Channels panel open the same stroke would drag the whole canvas back across the bus per frame.

**The rule, now enforced by the class rather than remembered by callers:** *nothing transfers while
the composite is moving.* `refreshMirror()` refuses when a gesture is active **or** when the
revision moved since the previous frame — the second predicate exists because **typing is not a
pointer gesture**, and without it a keystroke fences once per character on the path this roadmap
calls the most latency-sensitive in the app. `materialise()` refuses on the same predicate for any
consumer that did not ask for `Current`, which is what `Freshness` was for.

Three consequences, each deliberate:
- **A stale mirror is a MISS for `peek`** (the cursor readout, `Current` by contract) — the readout
  blinks off mid-stroke instead of reporting the colour the canvas had a moment ago. A glitch
  rather than a bug report, the same principle as the pre-existing "a partial mirror is a miss".
- **A stale mirror is still SERVED to `request`** when the consumer declared it tolerates a lag,
  with `stale` set. Refusing there would have converted a rule meant to *save* fences into one that
  causes them — the eyedropper loupe reads every frame while its tool is up.
- **A pin taken mid-gesture still seeds itself.** A pin is an explicit, one-off consumer act, the
  same class as `request()`; only the per-frame path is forbidden to pay. Otherwise `peek` after
  `pinMirror` would never work.

**⚠ That last consequence hid the second half of the defect, and a second interactive pass found
it:** the frame-path fix took the other rows from 25–30 ms to ~5 ms and left `Tile readback
(accumulator)` spiking at **23 ms**. The shape of that report is what identified it — a nested
profiler scope cannot exceed its parent, so a 23 ms readback sitting *outside* 5 ms resident rows
was not being called from `serve()` at all. It was `ResidentComposite::peekPixel`: the cursor
readout re-pins whenever the pointer crosses a macrotile boundary, and a pin seeds itself, so
"an explicit, one-off consumer act" was in fact **one transfer plus one fence per pointer event**
during a stroke. `peekPixel` now declines to take a new pin while the composite is moving and the
readout goes dark until the first quiet frame. The class-level rule was right; the exemption I
carved for explicit acts was too wide, because one consumer's "explicit act" fires per event.

**Net: a brush stroke and a keystroke now perform ZERO readbacks** — `serve` moves no host bytes,
`refreshMirror` is refused, `peekPixel` takes no pin, and `materialise` is refused for every
consumer that did not demand `Current`. What remains are one-off explicit costs on discrete user
actions (the eyedropper's first "All Layers" sample, a Magic Wand click), which are memoised per
revision and are what a blocking transfer is *for*.

`tests/test_composite_readback.cpp` pins all of it: zero bytes on a gesture frame (**zero**, not
"bounded"), zero on an editing frame with no gesture, the stale-refuses-rather-than-lies rule, the
recovery on the first quiet frame, and the mid-gesture pin seeding. The profiler call stays
unconditional so a `(mirror refresh)` row reading ~0 through a whole stroke is the standing
evidence this has not regressed.

**The wider lesson, which is why this is written up at length:** the gate measures the *lane* —
composite and resolve — and it was right. What it cannot see is a consumer the app hangs off the
lane, and that is where the whole win went. A benchmark that composites in a loop has no cursor,
no pointer, no open panel, and therefore no pinned mirror. **`--bench` cannot catch this class,
and no amount of re-running it would have.**

**What remains before the flip, and the gate cannot answer it:** `residentRecompositeNow` →
`adoptResidentDocument` → present has **never executed**. Reaching it requires a real edit, and
`--gui-frames` cannot make one, so headless verification tops out at "the lane initialises and
tears down cleanly". `TileCompositor` itself is covered from three directions (bench, kernel
parity, end-to-end lane tests); what is unproven is the app wiring around it. **The flip is one
line** — `residentCompositorRequested()` returning true — plus one test assertion, deliberately
test-visible. It should be made after an interactive pass with `MOSAIC_TILE_COMPOSITOR=1`:
paint, type, undo/redo, switch tabs, toggle channel isolation, drag a layer, and watch for a stale
or torn canvas rather than for a crash.

**STATUS 2026-07-28 (second session): items 7, 8, 9, 11, 12 are BUILT, plus §8.1's deferred
**GPU timestamp queries** and the `--bench tile-composite` scenario. None of the 2026-07-28 work
has been compiled or measured — neither session was permitted to build.** Item 10 is open; item 13
is **still withheld**, but it is no longer *unmeasurable*: the gate above is now a command with a
criterion rather than an argument. Item 14's bench case landed earlier.

**STATUS 2026-07-27: items 7, 8 and 9 are BUILT (not yet compiled or measured — the session that
wrote them was not permitted to build).** What exists is a complete, caps-gated, parity-tested
resident lane — `render::TileCompositor` — that composites a document of top-level raster layers
(transforms, all nine resample filters, linked/unlinked masks, clip runs, all 23 blend modes,
opacity) into a device-resident macrotile atlas, recomposites only dirty macrotiles, uploads a
layer only when its content or mask revision moves, and refuses by name everything it cannot do
exactly. Items 10–14 are open; **item 13 is untouched by design**, and no `src/ui` file calls the
new lane yet. See §7's per-item notes for the two deliberate gaps in item 8.

*Exit criterion for S60-a:* `--bench move-fullcanvas` improves on BOTH the gesture-start and the
new gesture-end case, and `composite-full` at 1920×1080 and 5000×8000 improves, all measured
loaded and back-to-back against the pre-flip build. If they do not move, commit 13 is withheld —
that is what keeping the flip last is for.

### S60-b — CPU-only compute mode + CPU-lane hardening

14. `--cpu` / Settings → Rendering; every `create()` short-circuits.
15. **Persistent thread pool** replacing the 18 per-call `parallelFor` spawn sites and
    `parallel_rows.hpp` (§1.5). Bit-identical band decomposition — the goldens must not move.
16. CI job running the full suite in CPU-only mode.

### S60-c — Present-paced loop + off-thread composite

FIFO/`wl_surface.frame` pacing, render-on-demand, composite moved off the UI thread. Unchanged
from PLAN §9 S60-b, but now much easier: a resident compositor has far less UI-thread state to
hand over.

### S60-d — Huge-document scalability

Tiled *in-memory* pixel storage on the 64 px grid (1:1 with `.mosaic`), the proxy/low-res live
composite (the open full-canvas Move-lag bug, PLAN §2), GPU memory budget + eviction driven by
`VK_EXT_memory_budget`.

### S60-e — Filling in the missing lanes

GPU lanes for brush stamping, adjustments, layer effects (per §4's ordering), then vector/text
raster and histogram. Each is: shader + caps gate + parity test + CPU lane untouched.

### S60-f — Hardening & fallback

`--device` selection, software-device acceptance + notice (Level 2), the capability readout UI,
sanitizer/validation passes, `PresentModel` extraction, and Level 3 only if we still want it.

---

## 8. Measurement discipline

An optimisation arc without a measurement harness is a rewrite with extra steps.

### 8.1 Profile the build the user runs

Split `MOSAIC_DEBUG`-gated profiling into two: keep the **Timing Profiler window** debug-only
(that was a deliberate product decision, 2026-07-18), but make the **collection** available in
release behind `--profile` / an env var. `MOSAIC_PERF_SCOPE` becomes a runtime-checked branch on
an atomic flag — a predictable-branch cost when off, which is nothing next to what it measures.
The collector lives in `src/common/profiler.{hpp,cpp}` and the window in
`src/ui/timing_graph_window.{hpp,cpp}`: a profiler is infrastructure, and putting it in `ui` is
what kept `render` — where the two biggest uninstrumented costs are — from being measurable at all.
Add **GPU timestamp queries** (core 1.0, gated on `timestampComputeAndGraphics`) so `Lane::Gpu`
rows report actual device time instead of CPU submit wall-clock — the current 3D-text lane
comment admits it is measuring the wrong thing.

> ✅ **BUILT 2026-07-28** (`render::GpuTimer`), with one correction to the sentence above: device
> time does **not** replace the `Lane::Gpu` row, it joins it. A third lane `Lane::GpuDevice`
> carries the device measurement under the *same name*, because both numbers are wanted and their
> difference is the whole diagnostic — submit wall-clock is what the caller pays and is not a lie,
> it is just a different quantity. Also corrected: the gate needs **three** facts, not one —
> `timestampComputeAndGraphics`, a non-zero `timestampPeriod`, and a non-zero
> `VkQueueFamilyProperties::timestampValidBits` on the family we submit on. The third is not a
> limit and was not in this plan's `GpuCaps` sketch; a transfer-only queue reports 0 and cannot
> execute `vkCmdWriteTimestamp` at all. See §7's "GPU timestamp queries" subsection.

### 8.2 A benchmark harness, not a stopwatch

`--bench <scenario>`: headless, deterministic, prints a table. Scenarios that map to the real
complaints — `paint-stroke` (S19 dab flood), `move-fullcanvas` (the 5k×8k open bug),
`type-keystroke` (the 64 ms regression already fixed once), `opacity-drag`, `blur-live`,
`adjustment-live`, `open-large`. Baseline numbers recorded in this document before S60-α starts,
re-run at every session exit. **A session that does not move its scenario is not done.**

**`tile-composite` (new 2026-07-28)** is the eighth scenario and the only one that runs **both**
lanes. Every other scenario is CPU-only, which is why item 13 sat withheld with nothing to measure
it: the harness could describe the thing being replaced but not the replacement. It is also the
first scenario whose rows can legitimately print `(skipped: refused: ...)` — a named refusal from
the resident lane is a *result* ("this device will not take a 5000×8000 document"), not a harness
failure, and it belongs in the table rather than in a log nobody reads. Details and the pass/fail
criterion: §7's "Item 13: the gate, stated so it can be run".

### 8.3 Test the floor, not the ceiling

`lavapipe` is installable on the dev box and is a Vulkan 1.3 software ICD. Two CI/manual profiles:

- `MOSAIC_GPU_PROFILE=floor` — forces `GpuCaps` to report the 1.0 guaranteed minimums regardless
  of what the device says (a *synthetic* floor: 4096 textures, 4 storage buffers, 128 B push
  constants, no tier flags). Runs on the dev rig. **This is how we actually test the shitbox
  without owning one**, and it is worth more than any amount of careful reading of the spec.
- `VK_ICD_FILENAMES=…lvp_icd… --gui-frames N` — real software rasterisation, catches anything the
  synthetic floor misses.

Both belong in the headless verification set alongside `ctest` and `--gui-frames`.

---

## 9. Risks

- **The fused kernel is the whole plan.** If transform/mask/clip stay on the CPU, S60-a produces a
  slower compositor and a lot of new code. Guard: parity + benchmark gate before flipping
  `Backend::Cpu`, and keep the flip as the *last* commit of S60-a so it can be withheld.
- **Readback consumers.** One missed `m_lastComposite` reader silently reinstates a full per-frame
  readback. Guard: enumerate them in a commit of their own, and assert in debug that no readback
  happens during a gesture frame.
- **Golden-image churn.** fp16 working buffers and reordered blends will move bytes. The existing
  1/255 GPU tolerance covers this; the **CPU** goldens are byte-exact and must stay CPU-produced.
  The thread-pool change in S60-b must be bit-identical by construction (same band split).
- **Scope gravity.** §4's "missing GPU lanes" list is long and every item is tempting. S60-a is
  the only session that changes the felt experience; e/f can slip indefinitely without harm.
- **`--device` and multi-GPU.** Hybrid laptops will pick wrong. `GpuCaps` should log the full
  enumerated device list so a bad pick is diagnosable before `--device` exists.
- **S43-a interaction.** The float layer-storage migration and tiled storage touch the same code.
  Tiling should land on whatever `Layer` stores at the time and not wait for float; the tile grid
  is orthogonal to the element type.
- **⚠ S60 completion is the repo's public-flip tripwire.** The repository is private *until after
  S60*. Two things in the tree are gated on that staying true: the edge-brush L1 variant (a) patch,
  and the env-gated inpaint colour-expectation prior. **Before S60 closes, audit for gated code and
  strip or re-gate it.** This is a checklist item for S60-f, not an afterthought.

---

## 10. Decisions (settled with the user, 2026-07-23)

All four forks resolved at scoping time. These are decisions, not defaults — reopen only with a
measurement that contradicts them.

1. **Tile granularity — SPLIT.** Dirty-tracking is the `.mosaic` 64 px grid, shared verbatim with
   the autosave journal's `TILE` keys; GPU dispatch coalesces into **macrotiles of 64 × 2^k**,
   with k chosen from `GpuCaps` (default 256 px, k=2). k=0 stays reachable without a redesign if
   measurement favours it on some device. (§3.1)
2. **CPU-only mode — Levels 1 + 2.** L1 `--cpu` compute-only (every lane `create()` returns null;
   doubles as the CI harness that keeps every CPU lane honest) and L2 software-device acceptance
   with a one-time notice + a conservative `GpuCaps` profile. **Level 3 — a genuine no-Vulkan
   presentation path — is NOT in scope for S60.** The `PresentModel` extraction may still be done
   on its own merits (it makes the overlay logic unit-testable, which it is not today), but it
   carries no obligation to grow a CPU rasteriser. (§6.2)
3. **Sequencing — S60-α, then straight into S60-a.** Attack the felt complaint first. S60-a is the
   only session in the arc that changes the user's experience; S60-b…-f can slip without harm.
   The thread-pool work is independent and loses nothing by waiting. (§7)
4. **Working-buffer format — `R16G16B16A16_SFLOAT` by default, everywhere.** Guaranteed for both
   `STORAGE_IMAGE` and `SAMPLED_IMAGE_FILTER_LINEAR` (rgba32f is guaranteed only for the former);
   half the memory and half the upload bandwidth on exactly the parts we are targeting; ~11-bit
   mantissa clears the existing 1/255 parity tolerance comfortably. The CPU reference stays fp32
   `ImageF`. rgba32f is not used even where offered; `R8G8B8A8_UNORM` remains the desperate floor,
   and taking it must be visible in the log. (§2.4)

**Also settled by PLAN §2 + the 2026-07-03 user directive:** S60 precedes Save/Export (S18-b).
