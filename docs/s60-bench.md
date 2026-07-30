# `--bench` — what the harness measures, and what it does not

The S60 measurement harness lives in `src/app/bench.cpp`. It is headless, deterministic and
display-free: every document is built in code from a fixed seed, nothing is read from disk, no
window is opened, and the same command on the same box produces the same table.

This file is the reference for **what a row means**. The verdicts read off these numbers live in
`docs/s60-performance-plan.md` §7; the methodology rules at the bottom of this file are the ones
this project earned the hard way and they are not optional.

---

## 1. Running it

```
build/linux-release/bin/mosaic --bench                       # list the scenarios and exit
build/linux-release/bin/mosaic --bench <scenario>            # per-scenario default iterations
build/linux-release/bin/mosaic --bench <scenario> --bench-iterations 20
```

Always a **release** build. The debug compositor is 5–10× slower and nobody runs it; a number
measured there describes a configuration no user experiences.

Two environment variables change what a run can say:

| variable | effect |
|---|---|
| `MOSAIC_PROFILE=1` (or `--profile`) | turns profiler collection on, which is what fills the two per-case tables below the main one |
| `MOSAIC_GPU_PROFILE=floor` | forces `GpuCaps` to the Vulkan 1.0 guaranteed minimums. The GPU rows must then **refuse themselves by name** and the CPU rows must still print — that is the floor sanity check, not a failure |
| `MOSAIC_TILE_DISPATCH=per-tile\|list\|indexed` | pins the resident lane's dispatch shape instead of `Auto`, for measuring the three against each other |

⚠ **Read the main table from a run WITHOUT `--profile`.** A profiled run is honest but it is not
the same run: every `MOSAIC_PERF_SCOPE` inside the measured region then costs a clock read and a
mutex-guarded map insert. With collection off a scope is one relaxed atomic load and a predictable
branch, which is why the scopes can sit inside the timed loops at all. Take the medians from the
plain run and the *split* from the profiled one.

---

## 2. The three tables

**The main table** — one row per case: `n`, `min`, `median`, `mean`, `max`, in milliseconds.
**Read the median.** One descheduled sample drags a mean and leaves the median where it was; both
are printed so a run whose two disagree is visible as such.

A row can print `(skipped: …)` and that is frequently a **result**, not a harness failure: "this
device will not take a 5000×8000 document", "this canvas exceeds `maxImageDimension2D`", "the lane
refuses `LayerEffects`". A named refusal in the table beats a silent absence in a log.

**The per-case DEV table** (`--profile` only) — `Lane::GpuDevice` rows attributed to one case.
This exists because `common::Profiler` is a **process-wide** collector keyed by (name, lane): one
`--bench` run used to pile every case's samples into the same `Tile upload` row, which is why the
item-13 gate's condition 4 was "not cleanly measurable" on 2026-07-28. Each case now clears the
collector before its timed loop and snapshots after it.

**The per-case HOST table** (`--profile` only) — the `Lane::Cpu` and `Lane::Gpu` rows of the same
snapshots. `Lane::Gpu` is wall-clock **at the call site**: it includes the host half (staging
memcpys, descriptor writes), the submit and any fence wait, so it is what the caller *pays*.
`Lane::GpuDevice` is what the device *spent*. The gap between a row on the two lanes is the
diagnostic — `Gpu` ≫ `GpuDevice` means the cost is host-side or fence-bound and no shader work will
touch it.

⚠ Consequence of the per-case scoping, so nobody reads the exit dump as a total: under
`--profile` the stderr dump at process exit shows only the **last** scoped case's rows. The
per-case tables are the ones to read.

---

## 3. The scenarios

| scenario | one sample is | what it deliberately excludes |
|---|---|---|
| `composite-full` | one `render::composite()` of the whole document, CPU backend | the present half (§4) |
| `composite-region` | one `render::compositeRegion()` of the roi, CPU backend | the present half |
| `paint-stroke` | one stroke FRAME: `extendTo` + dab composite + region recomposite | the present half; input latency before the sample reaches the engine |
| `move-fullcanvas` | one drag frame, or the gesture-start seed, or the gesture-end commit | the present half; the GPU drag pass in `WindowRenderer` |
| `type-keystroke` | one keystroke: `setBlock` + `refreshTextCaches` + recomposite | the present half; keyboard/IME latency |
| `blur-live` | one scrub frame: rewrite the radius, then full composite | the present half |
| `adjustment-live` | one drag frame: rewrite the parameter, then full composite | the present half |
| `tile-composite` | one frame: CPU composite (± its upload), or GPU composite + `resolve()` | the present *pass* (§4) |
| `present-upload` | one present: stage + copy the pixels into the canvas texture, or one `resolve()` | the composite that produced them |

### `tile-composite` — the item-13 gate

Both lanes, same documents, same process, back to back. Rows per canvas size:

| row | what it is |
|---|---|
| `cpu full` | `render::composite(…, Backend::Cpu)` — **the composite half only** |
| `cpu region 256` | `render::compositeRegion(…)` over one 256 px macrotile — **composite half only** |
| `cpu full +upload` | the same full composite **plus the app's present chain** |
| `cpu region 256 +upload` | the same region composite **plus the app's present chain** |
| `gpu full` | `markAllDirty()` + `composite()` + `resolve()` into the present texture |
| `gpu region 256` | `markDirty(rect)` + `composite()` + `resolve()`, **no content change** — the pure dirty-set path |
| `gpu edit 256` | a real 256² pixel edit + `invalidateContentBounds()` + `markLayerDirty(layer, rect)` + composite + resolve |
| `gpu edit 256 whole` | the same edit marked `markLayerDirty(id)` — the whole-layer-upload baseline, kept standing so the incremental win stays attributable rather than lucky |

**Compare `+upload` rows against `gpu` rows.** The `gpu` rows have always included `resolve()`, so
`cpu full` against `gpu full` compares four fifths of one frame with all of another. The two
composite-only rows are kept unchanged because three sessions of the plan's tables are stated in
them, but they are not the comparison.

The two halves stay separately attributable: under `--profile` a `+upload` case shows
`Bench composite (full|region)` and `Bench canvas upload (full|region)` as two HOST rows, so a
number that moved says *which half* moved.

### `present-upload` — the present half alone

Four rows per canvas size, two pairs doing the same job:

| pair | CPU row | resident row |
|---|---|---|
| put a whole canvas into the canvas texture | `cpu upload full` | `gpu resolve full` |
| put one 256 px macrotile into it | `cpu upload 256` | `gpu resolve 256` |

The CPU rows move the pixels across the bus; the resident rows move **no host bytes at all** —
`resolve()` reads the resident accumulator and writes the same texture on the device. That
difference is the whole thesis of S60-a item 11.

These rows are published separately so they can be **added to any CPU-only row in any other
scenario**: `composite-full` at 1920×1080 plus `1920x1080 cpu upload full` is that frame's honest
cost. The composite that feeds each `resolve()` is deliberately outside the timed region here —
`tile-composite`'s `gpu` rows are the ones that include it.

---

## 4. What the present half is, and what is still missing from it

In the app a CPU composite is followed by a **host upload into the canvas texture**
(`Canvas upload (full)` / `Canvas upload (region)` in `app_window.cpp`), while the resident lane
writes that same texture **on the device with no host bytes**. The bench reproduces the app's chain
step for step (`docs/s60-readback-consumers.md` §5 calls it "three CPU-side copies before the
staging write"):

```
full     m_lastComposite <- the composite            (a MOVE: free, so it is not modelled)
         -> VulkanCanvas::m_documentImage                       host copy
         -> WindowRenderer::m_pendingCanvas                     host copy
         -> memcpy into the mapped staging buffer               host copy
         -> barrier, vkCmdCopyBufferToImage, barrier            device
region   patchComposite(sub) into the doc-sized mirror          host copy (per row)
         -> VulkanCanvas::m_documentRegion                      host copy
         -> WindowRenderer::m_pendingCanvasRegion               host copy
         -> memcpy into staging, then a SUB-RECT copy           host copy + device
```

The destination is an ordinary device-local `R8G8B8A8_UNORM` image created with **the same usage
set as the real canvas texture** (`TRANSFER_DST | SAMPLED | STORAGE`), on
`render::VulkanContext::shared()`. No surface is involved, because an upload never needed one.

⚠ `TileCompositor::createResolveTarget` looks like the destination for this and **cannot serve**:
its image carries `STORAGE | SAMPLED | TRANSFER_SRC` and no `TRANSFER_DST`, so nothing may be copied
into it. Owning the image in the bench has a better reason anyway — the CPU present half has to be
measurable on a device that *refuses* the resident lane, which is exactly the floor profile.

**Still excluded, named so nobody has to rediscover it:**

- **The present pass itself** (`canvas_present.comp`) and the swapchain blit. Both lanes pay it
  identically — it samples the same texture whichever half wrote it — so excluding it changes no
  comparison, only the absolute frame cost. Measuring it needs a swapchain, and `--bench` is the
  no-display harness by design.
- **Submit granularity.** The app batches its copy into the frame's one command buffer and waits
  that fence at the *start of the next frame*; the bench gives the upload its own submit and its own
  fence wait. That overstates the CPU present half by roughly one submit plus one stall — and
  `TileCompositor::resolve()` submits and waits exactly the same way, so **both lanes are overstated
  symmetrically**. The alternative measures "queued" on one lane and "arrived" on the other.
- **VMA.** The app stages through a VMA allocation with `HOST_ACCESS_SEQUENTIAL_WRITE`; the bench
  uses a plain `HOST_VISIBLE|HOST_COHERENT` allocation. On a device where VMA picks host-visible
  device-local (resizable BAR) memory, the app's memcpy crosses PCIe and the bench's does not.
- **Everything the app hangs off the lane.** A benchmark that composites in a loop has no cursor, no
  pointer, no open Channels panel and therefore no pinned readback mirror. The 2026-07-29
  interactive defect — a fence per frame to keep a status-bar colour current — was invisible to every
  row here and no amount of re-running would have found it. `--bench` measures the *lane*; it cannot
  measure the *consumers*.

**Memory.** The present half holds a device image plus a full-canvas staging buffer plus the host
copies — about four document-images, ~640 MB at 5000×8000. Both scenarios build and tear it down
around the resident lane rather than beside it, so the two never hold their buffers at once.

---

## 5. ⚠ The resolve-layout correction (2026-07-29)

`TileCompositor::resolve()` takes its destination **by const reference** and therefore cannot write
the new layout back; the caller owns that fact (`tile_compositor.hpp`, `ResolveTarget::layout`).
`--bench` never carried it forward, so `dst.layout` stayed `VK_IMAGE_LAYOUT_UNDEFINED` for the life
of the run — and `UNDEFINED` is one of the three things that force resolve down its **full-canvas**
branch.

Every GPU row was therefore paying a whole-canvas resolve on every iteration while claiming to
measure the incremental path, and the app never behaved that way (`WindowRenderer` tracks the layout
in `residentCanvasLayout()`). The bench now routes every resolve through a helper that carries the
layout forward, gated on resolve's own `wrote` flag — resolve leaves the image *exactly* as it found
it when it had nothing to do, and claiming otherwise desynchronises the layout from the image.

**Consequence for the tables: the GPU rows get faster, so numbers from this build are not
comparable with the 2026-07-29 gate table.** The direction matters — this fix can only turn a gate
failure into a pass, never the reverse — so the gate must be re-read, not assumed.

---

## 6. Methodology — the two rules this project paid for

### (a) The **loaded** system is the PRIMARY figure. Quiet is the footnote.

Nobody runs an image editor on an idle box. A number measured with nothing else running describes a
machine the user does not have. Quote the loaded median first and note the load average beside it;
the quiet run belongs in a parenthesis.

### (b) ⚠ NEVER measure on a box loaded by the measurement's own toolchain.

"Ordinarily loaded" is the point. A box loaded by the session's own subagents is not a user's
machine, it is an artefact of how the work was done.

On 2026-07-29 a gate run at load **9.35** — the session's own agents — produced a `Tile resolve` row
reading **17.283 ms** against a true **0.437 ms**, which looked exactly like a scalability defect in
the region path and sent a whole session chasing one that did not exist. In the same run the
condition-1 margin at 3840×2160 collapsed from comfortable to 0.38 ms, with the GPU row *losing* on
the mean.

**GPU rows are far more load-sensitive than CPU rows.** A GPU row's host half is staging copies, a
submit and a fence wait — all of which queue behind other processes' work and the scheduler; a CPU
row is pure compute and degrades gracefully. So contention does not scale the table, it **reorders**
it, which is the failure mode that produces wrong conclusions rather than noisy ones.

Corollaries:

- Record the load average next to every table. A table without one is not reproducible.
- Both lanes run **in one process, back to back**, precisely so they see the same load. Never
  compare a GPU row from one run with a CPU row from another.
- A session that measured while building is a session that measured its build.

---

## 7. When adding a scenario

- **A row's name is its definition.** If what a row measures changes, the row gets a new name —
  a row whose definition moved while its name stayed is how a regression hides in a chart.
- **Instrumentation must not become the thing measured.** Keep `MOSAIC_PERF_SCOPE` out of the hot
  loop body, or prove it is free when collection is off (it is: one relaxed atomic load).
- **Warm up outside the timed loop**, and make every sample genuinely different — walk the ROI, step
  the parameter, cycle the character — so no cache anywhere can answer "unchanged" and turn a row
  into a no-op the day one is added.
- **Vulkan 1.0 floor.** No new extensions, no new features. `MOSAIC_GPU_PROFILE=floor` must still
  produce a runnable bench that refuses the GPU rows cleanly and prints the CPU ones.
- **No display.** This is the headless harness; a scenario that needs a window belongs somewhere
  else.
- **Numbers are DATA.** `i18n::init()` has already moved `LC_NUMERIC` to the user's locale, so a
  bare `%f` would emit their decimal separator and silently break any harness parsing the table.
  `CNumericLocale` pins the C numeric locale for the report; keep new output inside it.
