# Mosaic — Project Plan

**Mosaic** is a professional, GPU-accelerated, cross-platform raster + vector image
editor in the spirit of Photoshop, written in C++ and licensed under **GPLv3**. It is
built for artists and power users who are tired of the gaps in existing free tools:
non-destructive editing, deep file-format support (PSD/PSB, RAW, HDR, an open native
`.mosaic` format, and roughly GIMP-level format coverage), a Vulkan-accelerated canvas
and effect pipeline, and a UI that feels at home on Linux, Windows, and (eventually)
macOS.

This document is the single source of truth for the project. It is written so that a
**fresh Claude session can resume work in seconds** — read §0 and §2, pick the next
unchecked item in §10, and go.

---

## 0. How to resume in a new session (READ THIS FIRST)

A new session should **not** re-derive context by scanning the whole tree. Instead:

1. **Read this file's §2 (Current Status)** — it names the exact next session.
2. **Read §10 (Progress Tracker)** — the open `- [ ]` items are the candidate tasks. ⚠ **"The first
   unchecked box is next" has not been true since about 2026-07** (see §2's phase note): the roadmap has
   been deliberately non-linear, driven by user directives that pull items forward, so several boxes are
   open across four different phases at once. Confirm sequencing from §2 rather than by ordinal.
3. **Read that session's full spec in §9** (and any `docs/` file it references).
4. Implement it, following the conventions in **§8** and the architecture in **§4**.
   - Add/extend unit tests and a golden-image test where it makes sense (§3.15).
   - Verify headlessly via the debug harness (`mosaic --headless …`, see §3.15 / S2).
5. **Tick the box** in §10, and **update §2** to point at the next session.
6. **Commit** the work as a single feature commit (Conventional Commits + trailer, §8.6).
7. If you changed an architecture-level fact, update the project memory entry.

> A persistent memory entry (`mosaic-project` in this project's memory dir) points
> here and restates this protocol. If it disagrees with this file, **this file wins** —
> fix the memory.

**Scope discipline:** each session in §9 is sized for roughly one Claude session and one
commit. If a session is too big (e.g. the Type tool or the Vulkan compositor), split it
into `Sxx-a` / `Sxx-b`, commit each, and note the split in §10. **Research-heavy sessions
(S17, S18, S19-b, S20, S37-a, S46) must begin by writing/refreshing their `docs/` research note
before any code.** *(Of that list only **S46** is still outstanding; the other five discharged their
notes before their code, and the gate earned its keep — the edge-aware brush note is the one that
turned an outright decline into a shippable variant, and `docs/brushes.md` §3 caught format facts
that a reimplementer would otherwise have guessed at.)*

---

## 1. Vision & Principles

**Product goals**
- A Photoshop-class editor: layers, groups, masks, non-destructive adjustment/filter
  layers, vector + text + raster, selections with live marching ants, a rich tool set.
- GPU-accelerated everywhere it reasonably helps (canvas, compositor, filters, export,
  loupe), via Vulkan.
- Deep, honest file support with explicit **loss warnings** on export.
- Offline-first and privacy-respecting: **no built-in ML/cloud features and no telemetry.** ML
  inpainting is intentionally **not** bundled (deps/licensing/weights/memory not worth it);
  it is left to **user Lua scripts** (S40) — local, the user's own choice, never cloud.

**Engineering principles**
- **Maintainability first.** Clear module boundaries (§4), small composable units, heavy
  documentation in `docs/`, consistent style (§8), tests + headless verification.
- **License-clean.** GPLv3-compatible deps only; document every dep's license (§6/§7).
  We implement no PatchMatch-style patch-optimization loop and bundle no HEVC codec —
  see §3.11 and §7.
- **Prefer system-installed dependencies.** We do not silently download/build heavy
  deps. Anything the user must install manually is called out in the README and §6.
  Only tiny permissive **header-only** libs are vendored in `third_party/`.
- **Native-feeling, not native-faking** (see §3.5). The app adapts to the host's
  light/dark mode, accent color, fonts, and DPI, but — like every serious creative tool
  (Photoshop, Krita, Blender, DaVinci Resolve) — it draws its own polished, consistent
  widgets rather than literal OS controls.
- **Characterful, not corporate.** Mosaic has a deliberate visual identity with warmth. Icons are
  **colorful and illustrative — they show what they do** — and we **reject the flat, monochrome,
  "soulless corporate" aesthetic** that homogenizes modern apps. The icon stance is detailed in §3.13
  (touchstones: Affinity Photo; GIMP's legacy color icons — that class, with our own identity).
- **Non-destructive by default.** Source data is preserved; edits are a re-playable graph
  (this also gives us undo/redo and a headless op-runner for testing — §3.7, §3.15).

**Non-goals (for now):** animation/video timeline, cloud sync, plugin marketplace,
mobile/touch-first UI. Some appear in the backlog (§12).

---

## 2. Current Status

> **Update this section at the end of every session.**

- **PARALLEL SESSION 2026-07-30 — S57 + the Windows half of S59: Mosaic now cross-compiles from
  Linux to Windows for x86_64 AND aarch64, and packages as a portable zip + a per-user MSI.**
  Seven implementation agents, one verifier. Full detail in §S57; `docs/build-windows.md` is the
  design surface and `packaging/windows/README.md` the pipeline.
  - **Both arches build clean.** x86_64 via the system mingw-w64 GCC, aarch64 via llvm-mingw (the
    GNU toolchain has no aarch64 target). 24 third-party libraries cross-built per arch as **DLLs**;
    Mosaic's own modules stay static in `mosaic.exe` (user decision).
  - **The `.mosaic` write path is implemented** — the one feature that was genuinely absent rather
    than merely un-compiled. Plus a Win32 Vulkan surface, registry appearance detection + a change
    watch, `MessageBeep` alerts, the shell `IFileDialog` picker, **WinTab + Windows Ink** tablet
    input, `ISpellChecker`, bundled hyphenation dictionaries, an Explorer `IThumbnailProvider`
    handler (the macOS Quick Look counterpart), and `common::pathFromUtf8`/`utf8FromPath`/`fopenUtf8`.
  - **A real user-facing bug fell out of that last one:** every raster export
    (`png/jpeg/webp/avif/tiff/gif/jxl`, plus `format_registry`) called `std::fopen` on a UTF-8 path,
    which Windows decodes in the **active code page** — so exporting into any folder with an accented
    or CJK name failed. Nine `fopen` sites and four `ifstream` readers converted.
  - **Verified headlessly:** `mosaic.exe` under Wine runs `--version`/`--help` and `--composite-demo`,
    the latter enumerating the real GPU, driving the **GPU-compute compositor** with **0 Vulkan
    validation errors**, and producing output **byte-identical to the Linux build**; the portable zip
    runs from a fresh extraction; both MSIs carry 163 files with the right arch template. **Linux
    build + full suite stay green (100%).**
  - **Three guards were keyed on the wrong thing and are now keyed on the compiler**, because
    llvm-mingw reproduced macOS-only failures exactly: the clang `-Wall` exemption set and
    `SPDLOG_NO_EXCEPTIONS` moved from `if(APPLE)` to `if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")`.
    ⚠ And `UNICODE`/`_UNICODE`/`NOMINMAX` moved OUT of the toolchain file into Mosaic's own project
    scope — a toolchain file is also read when cross-building the dependencies, and forcing `UNICODE`
    on the Vulkan-Loader (written against the ANSI API) stopped it compiling.
  - ⚠ **Runtime is entirely the user's check** — there is no Windows machine here, and Wine's Vulkan,
    GDI and DPI are not Windows'. Owed: fractional HiDPI, the frame-loop-blocking picker, and the
    whole interactive pass.

- **PARALLEL SESSION 2026-07-29 (sixth batch) — five S60 slices: the bounded drag backdrop, adapter
  selection, presenting-device texture admission, and GPU lanes for layer effects and the
  histogram.** Commits `0ba7e0d` / `f1e5190` / `e978a2d`. Verified: **2890 test cases / 1,600,206
  assertions green in release**, green under `MOSAIC_CPU_ONLY=1` (**1,592,829**) and
  `MOSAIC_GPU_PROFILE=floor` (**1,588,902**) — the counts DROP in both, which is the proof those
  modes refuse rather than silently doing nothing — and `--gui-frames` validation-clean with the
  resident lane off and on. ⚠ **asan was NOT run this session** (skipped by the user) — it is the
  one standing lane not covered here, and both new GPU lanes allocate device memory, so an asan
  pass is owed before either is installed.
  - **The gesture-start stall is bounded** (`docs/s60-gesture-start-stall.md` §3.2). New
    `render::compositeScaled` + `CompositeOptions::skipLayer`; the drag backdrop and the 3D reflect
    env now composite at the size they are consumed at instead of building the full canvas and
    throwing it away. The bound is the view's own zoom, so it is lossless: at 1:1 and above the
    divisor is 1 and nothing changes. Finding **G6** (the document mutated via `setVisible` to
    express exclusion) is closed; **G2** is reconciled in a comment; **G1** is left exactly as
    unfixed as it was. ⚠ **G3 — the gesture-END stall, the LARGER of the two — is untouched.**
  - **`--device` / MOSAIC_DEVICE** selects an adapter, every enumerated device is logged, and an
    unmatched selector warns and falls back. ⚠ The presenting device is the one that matters and it
    had its own ad-hoc pick; both contexts now share `pickPhysicalDevice`.
  - **Presenting-device textures are admitted, budgeted in bytes, and profiled** (finding **G5**):
    the "can this device host these textures" question is answerable BEFORE the CPU below-composite
    is paid, so a floor device stops paying seconds of walk for a lane it then refuses.
  - **Two S60-e lanes land but are NOT installed** — per-kind admission, parity-tested, one line
    each to wire, wanting an interactive pass. The histogram's real win needs the resident
    accumulator to bin from; against a host composite it is a wash, and under the tile compositor it
    would read back and re-upload, which is worse. Wiring it did expose and fix a real standing
    cost: the Channels panel re-binned the whole canvas synchronously on every revision bump.
  - ⚠ **Process note: the subagent API failed repeatedly mid-edit through this session** (~10
    terminations across all five agents). Partial work on disk plus re-reading before continuing is
    what made it recoverable; two slices were finished by the orchestrator directly. A build caught
    what no agent could: `TextureRefusal::None` collided with X11's `#define None 0L`.

- **PARALLEL SESSION 2026-07-29 (third batch) — S60-a item 10, the item 13 app-side wiring, and
  THE GATE RE-RUN: it PASSES.** Verified: **2777 test cases / 1,598,088 assertions green in release
  and asan**, green again under `MOSAIC_GPU_PROFILE=floor`, `--gui-frames` clean with the resident
  lane both off and on (asan leak profile = the standing fontconfig/pango/wayland set, **zero**
  Mosaic frames).
  - **The gate PASSES on all five conditions** (`docs/s60-performance-plan.md` §7). Condition 3 —
    the row that withheld item 13 for three sessions — went from **4.8× over budget to 56% of it**
    (`gpu edit 256` at 3840×2160: 18.804 → **2.219 ms** against 3.979). The standing `gpu edit 256
    whole` baseline beside it (19.046 ms) is what makes that attributable to the incremental upload
    rather than to luck. `gpu full` now beats `cpu full` by **87×** at 1920×1080 and **105×** at
    5000×8000. ⚠ **The flip is STILL not made** — see below.
  - **⚠ A benchmark methodology trap, worth more than the numbers.** The first re-run was taken on a
    box loaded to 9.35 **by this session's own subagents**. Every condition still passed, but
    condition 1's margin at 4K collapsed to 0.38 ms with the GPU row losing on the mean, and a
    `Tile resolve` row read 17.283 ms against a true 0.437 — which I reported as a possible
    scalability defect and it was not one. **GPU rows are far more load-sensitive than CPU rows**
    (their host half is staging copies, submit and fence wait; the CPU rows are pure compute). The
    quiet-box re-run is the recorded one. "Loaded-system primary" means *ordinarily* loaded, never
    loaded by the measurement's own toolchain.
  - **Item 10** — three dispatch shapes, byte-identical pixels. The SSBO dirty-tile list collapses
    `layers × macrotiles` dispatches to `layers`, needs **one storage buffer against a guaranteed
    four**, and is pure Vulkan 1.0. **The measurement then killed the tier as a default:** indexed
    and list are the same lane to within noise (`gpu full` 5.683 vs 5.713 ms at 4K; 23.313 vs
    23.331 at 5000×8000), so `Auto` resolves to the **list** shape everywhere and a unit assertion
    pins that. Equal speed at strictly greater risk is not a default. `MOSAIC_TILE_DISPATCH`
    forces a shape and keeps the tier exercised. Also caught: `descriptorIndexing` alone is not a
    sufficient gate — a 1.0/1.1 device carrying the extension **cannot load** a blob built at a
    newer target env, so `GpuCaps::spirvVersion` is now probed as the second question.
  - **Item 13's app-side wiring** exists behind `MOSAIC_TILE_COMPOSITOR=1`, **default OFF**; all
    twelve `Backend::Cpu` sites are untouched without it. The dirty set is fed from the **edit**
    seams (per-dab preview, `pushScopedPixelEdit`, inpaint blit, and undo/redo via a new
    `Command::dirtyLayerPixels`) because the rect there is layer-local and exact, whereas a
    doc-space bbox inverse-maps to a *different* region under rotation. ⚠ **The device does not die
    in a destructor** — `VulkanCanvas::hide()` destroys it well before `~MainWindow`, so releasing
    the lane in the host destructor leaked 30 Vulkan objects and called `vkDeviceWaitIdle` on a
    dead handle; the release moved to a `setOnRendererShutdown` hook on the canvas's own teardown
    path. **Owed before the flip:** `residentRecompositeNow` → present has **never executed** —
    reaching it needs a real edit and `--gui-frames` cannot make one, so it wants an interactive
    pass (paint, type, undo, tabs, channel isolation, layer drag) and not a headless one. The flip
    is one line, named in the plan doc.

- **PARALLEL SESSION 2026-07-29 (second batch) — the user's bug reports, their brush-editor
  feedback round, and S34-a.** Verified together: **2764 test cases / 1,582,882 assertions green in
  release and asan**, `--gui-frames` clean on native Wayland *and* under `FLTK_BACKEND=x11` (the
  asan smoke's leak profile is byte-identical to the previous batch's — the standing
  fontconfig/pango/wayland set, no Mosaic frame in it), `check_menus.py` clean across all 74
  catalogs.
  - **Three Wayland input defects, one class** (`0d2fbb8`). Space-pan dying mid-drag and R-rotate
    resetting under a held key were both a POINTER gesture made hostage to the key stream, on a
    backend where FLTK synthesises auto-repeat from a timer and `wl_keyboard_leave` clears the held
    set with no KEYUP. The rotate half is fully explained (a KEYUP for a held key walks through the
    `// ignore auto-repeat` guard, and two such pairs inside the double-tap window ARE a double
    tap); **the pan half could not be pinned to a specific event by reading alone**, so the fix
    removes the dependency instead — drags start on FL_PUSH and end on FL_RELEASE, and the key flags
    became a cache of `Fl::event_key()`. The pen-cursor disappearance was a **third** path after
    `4b2953e`/`2372e04`: an app-wide value dedup in `TabletWayland::setCursor` against a send that
    silently drops out of proximity. ⚠ Recorded not fixed: a **write-after-return in FLTK 1.4.5**
    (`Fl::e_text` into a returned stack buffer, written through by the case-swap retry) —
    `docs/wayland.md` §5.
  - **The menu row's right gutter** (same commit). A user-reported *visual* defect: the shortcut
    right-aligned into the gutter while the active-toggle dot is an OPAQUE blit drawn after it, so
    the dot repainted the shortcut's glyphs. Both badge branches already stepped left past that
    gutter — the rule existed twice for badges and zero times for the shortcut. One
    `menuRowGutters()` chain now. `Image ▸ Image Size…` was a second, unreported instance.
  - **S34-a** (`6d4a4e9`) — see §10.
  - **Brush editor feedback round 1** (`f89408a`), all seven items: the scratchpad reads the pen
    (per-window tablet watch, dynamics from the device but position from FLTK); it paints in the
    **preview's ink, not the foreground** (the blind-pair fallback swaps both paper and ink, so the
    surface must paint in what the picture came back in); rail previews through the dock's one
    renderer; a user badge distinct on all three axes from the fidelity mark; delete (refusing
    shipped *and* in-bundle presets) and export; **the import filter's brace list was not the bug**
    — the kdialog driver `initNativeFileDialog` turns on process-wide hands kdialog a syntax it
    cannot read, so both dialogs moved to `platform::showOpenDialog`/`showSaveDialog`; user presets
    first, with `isUserPreset` now a prefix range.
  - **i18n** (twice this session): 14 then 4 new Filter paths, 62 languages each time.
  - **Owed:** every slice's visual pass; the live-session items in `docs/wayland.md` §6 (10–13) and
    the tablet ones — a long slow space-pan past the 0.5 s repeat point, a long R-rotate that must
    not snap to 0° while a plain double-tap still resets, the pen across repeated proximity cycles,
    and pressure actually arriving over the *editor's* window; the `.icc` pickers in
    `settings_dialog.cpp` / `new_document_dialog.cpp` still carry the kdialog filter bug.

- **PARALLEL SESSION 2026-07-29 — five slices: S60-a's incremental upload, the modal brush editor,
  S34, S35, and S38.** Verified together: **2734 test cases / 1,581,498 assertions green in release
  and asan**, `--gui-frames` clean on native Wayland *and* under `FLTK_BACKEND=x11` (the asan smoke's
  leak summary is the standing fontconfig/pango/wayland-client one — no Mosaic frame appears in it),
  `check_menus.py` clean across all 74 catalogs.
  - **S60-a, incremental upload** (`9584276`). The 2026-07-28 gate failed condition 3 by 4.8× because
    an edit that dirties one macrotile re-uploaded the WHOLE layer. `markLayerDirty(layer, rect)` is
    a region-carrying overload with a staleness ledger per source; every doubt falls back to the
    whole-layer upload, so a missed region costs time and never pixels. A second defect sat behind
    the first: the plan-diff fingerprint hashed `contentRevision` IN, making "the layer moved" and
    "one block was repainted" the same event — a perfect upload would still have re-dispatched the
    canvas. ⚠ **Item 13 is still withheld and the gate has NOT been re-run** (see
    `docs/s60-performance-plan.md`); that re-run is the next S60 job.
  - **Brush Arc D — the modal editor** (`26f5f30`), `docs/brushes.md` §8.3: preset browse + checkable
    option list + option pages + a preview surface that is also a scratchpad. **Mosaic does not write
    `.kpp`**; user presets are the native `.mbp`, which already round-tripped losslessly and had no
    consumer. Owed: §8.1's chip, a dirty-close confirm, and the layout's visual pass (a window cannot
    be shot headlessly).
  - **S34 + S35** (`86c7f5f`) — fourteen kinds; they share every registration surface so they landed
    together. Curves closes the kind the code blocked on (knots as indexed doubles in the existing
    bag; absent IS the identity). **Defringe was deliberately narrowed and that changed the
    design** — no edge/locality/detection term at all — and Haze Removal ships constant-transmission
    Koschmieder, not the dark-channel formulation. Frequency Separation deferred (it creates layers).
    S35's noise is a hash of the parent-space pixel, so region == crop(full) and the grain cannot
    crawl; Denoise is Lee 1980, a median method cut for cost.
  - **S38 — the Stamp/Clone tool** (`2409ef1`). **No spot/blemish mode** — that text and the
    anchor-snap-back choreography were a PLAN mistake describing the heal tool this slot stopped
    being, and are removed. It reuses `BrushEngine` for the stroke's alpha and rewrites the reported
    rect with source pixels, so undo/dirty-rect/commit paths are untouched; the deposit reads a
    pre-stroke snapshot, never the live target. No ghost preview (needs a present-pass change).
  - **i18n** (`51265f8`): the 14 new Filter paths would have split the menu bar in **62** languages;
    repaired the b8bd4ca way (own parent segments, English leaf), audited lost=0 / changed=0.
  - **Owed:** every slice's visual pass; the S60 gate re-run; the `duplicate` preset can now map onto
    the Clone Stamp.

- **PARALLEL SESSION 2026-07-28 (third batch) — the user's feedback pass on the second batch, plus
  S38-b.** Verified together: 2653 test cases green in release, asan green, `--gui-frames` clean on
  native Wayland *and* under `FLTK_BACKEND=x11`, `check_menus.py` clean across all 74 catalogs.
  - **⚠ The boolean kernel was WRONG on every generic crossing** (`3cdb090`) — found by running the
    feature, not by review. `pairSplits()` computed each edge-edge intersection, rounded it to the
    lattice, then fed it to a guard that demands exact collinearity with the edge; a rounded point
    is off both lines by construction, so **neither edge was ever split**. Two overlapping discs
    came back as two rings totalling 76.9 where the answer is one ring of 126.37. The escapes are
    what named it: two radius-5 n-gons with centres 5 apart are correct for n = 6/12/24/48 (a
    multiple-of-6 n-gon has vertices *exactly* on the two true crossings, so nothing is computed)
    and wrong for n = 8/16/32/64/128. Axis-aligned squares worked because their crossings are
    lattice-exact; disjoint operands worked because there is nothing to compute. All four ops read
    the same fragment set, so Subtract and Exclude were equally broken. Pinned now by an n-gon sweep
    with per-n exact expected areas.
  - **Combine Paths commits a baked path, not a live compound** — the user's report was that the
    result "is neither selectable by the shapes tool or the path tool. It's a path layer." Correct:
    a compound satisfied no tool's predicate. The compound model stays for a future
    non-destructive mode.
  - **S53 feedback** (`2e2b7e2`): the preview's handles are draggable and the active tool is gated
    while a preview is staged; the 3×3 anchor control moved off whole-pixel rect tables onto
    `GizmoCanvas`'s SDF-coverage AA; width/height gained scrub sliders and the rotation a dial;
    Canvas Size's fill list is the Fill dialog's family (Gray/Color/Gradient/Pattern/**Inpaint**) —
    its absence was a miss inherited from the Crop tool's five, never a gate; Image Size stages at
    the new size against a ghost of the old frame so growth is visible at all.
  - **Two cursor/keyboard defects with the same shape — the app was right and the environment
    disagreed.** `FL_CURSOR_MOVE` renders four-way arrows on X11 but `breeze_cursors` symlinks
    `move` → `dnd-move`, a closed grabbing hand, so Move-tool *hover* announced dragging; fixed
    with real art, Wayland-only. And `onTextKey` ends in `if (word) return 0`, so every unclaimed
    Ctrl chord fell through to the menu accelerators **during a live Type session** — Ctrl+] was
    reordering the layer being typed into. The bar now claims and drops the edit-class chords while
    a text editor owns the keyboard (claims, not declines: FLTK retries `FL_SHORTCUT` case-toggled).
  - **Layers dock** (`0ceece5`): Ctrl/Cmd-toggle + Shift-extend row selection, and Shift-click on a
    thumbnail now loads a pixel selection for **every** layer kind (group → composite, adjustment →
    the backdrop it grades, text/vector/texture → `rasterizeLayer`), not just raster/magic.
  - **S38-b eye retouch, Tiers 1 + 2** (`72a0286`) — see `docs/red-eye-tool.md` §9. Tier 3 stays
    deferred research. Looking at the real corpus rather than synthetic swatches changed four
    constants, including the redness axis itself (the doc's `R - max(G,B)` reads 0.16 on a *magenta*
    retinal reflection — below any usable threshold).
  - **S38-b feedback round 1** — see `docs/red-eye-tool.md` §9.8. Both reported defects were real
    and both were only findable against real photographs. **The red rim**: a glow *fades* into the
    iris, and two steep multiplied ramps score the fade ~0 while the disc inside scores 1 — measured
    residual +0.16 one pixel outside a corrected pupil, which is what draws the ring. Fixed with
    hysteresis (Canny 1986) on the score field inside the scope, discriminating on **purity**, not
    excess. **De-redden barely working**: three causes — `scleraReference` ranked by redness alone,
    so it elected the *lashes* (luminance 0.51 for an 0.87 sclera) and the luminance lift had
    nothing to lift toward; only the chroma half of the detail was attenuated, so a vessel kept its
    darkness and became a grey streak (exactly "the veins just go dull"); and harmonization aimed at
    the region's own least-red tone, which on an injected eye is itself pink, so passes converged to
    pink. Now a **local white field** (normalized convolution) replaces the vessel, colour *and*
    darkness, plus a saturation ceiling and hue nudge — all scaled by a **whitening licence** so a
    subconjunctival hemorrhage, which shows no white to harmonize toward, is left alone instead of
    turned a confident grey. The iris gate is a **red-channel** ratio, not luminance: blood reflects
    red while an iris is dark in every channel, which separates them 4× better and was what removed
    the last artifact (dark vessel cores surviving as red speckle — invisible in a downscaled
    preview, obvious at 1:1).
  - **S38-b feedback round 2 — the rim was THREE defects** (`docs/red-eye-tool.md` §9.8). Round 1
    fixed the gate's share and the user reported it still there; the other two were invisible to
    round-1 verification because it fed the module the dataset MASKS as the scope. In the app the
    brush ring IS the scope, so its own edge lands on the pupil — the harness has to paint a real
    `MaskStroke` click. Then: (1) coverage was **scaling** the correction, so the ring's shoulder
    drew a half-corrected annulus across the glow — coverage is now a gate; (2) at tip hardness
    0.92 a 70 px ring's scope was gone by r = 34.5, i.e. **the ring promised more than it
    delivered** — the flash tip is hard now (MaskStroke still AAs at 1); (3) correcting to NEUTRAL
    is not enough because an iris is not neutral — a neutral band on a blue-green iris (excess
    −0.105) is still a drawn circle, so Tier 1 gained Tier 2's estimator, a **local iris tone**, and
    the target's chroma slides from it to neutral as the pixel becomes pupil. The permissive
    hysteresis ramp is measured against that tone too, which is what makes it safe on a brown iris
    (a brown iris and a glow's tail measure the same absolute excess; neither departs from its own
    surroundings). Corpus: 0 of 19 eyes leave a rim above their own iris, 0 non-red pixels moved.
  - **i18n** (`b8bd4ca`): S53's 56 new menu paths would have split the menu bar in **64** languages.
    Repaired; two traps recorded in that commit (msgmerge here **must** use `--no-fuzzy-matching`,
    and renaming a title's last translated child silently orphans that title's translation in a way
    `check_menus.py` cannot see).
  - **Owed:** the sclera variant has no icon of its own (both variants resolve to `red_eye`); the
    Pen still cannot author a *second* contour into an existing path (it edits every contour of one
    fine).

- **PARALLEL SESSION 2026-07-28 (second batch) — native Wayland by default, the S53 document
  operations, live path booleans, the Pen's own chrome lane, and five layer-thumbnail staleness
  fixes.** Version stays **0.2.17**. Headlines, and the parts that are deliberately *not* done:
  - **S59-a — the backend pin flipped.** `platform::preferX11BackendIfUnset()` became
    **`preferWaylandBackendIfUnset()`**: on a session with `WAYLAND_DISPLAY`, and with no user
    choice, Mosaic now pins `FLTK_BACKEND=wayland`. **A user-set `FLTK_BACKEND` still wins**
    (`FLTK_BACKEND=x11` is the supported escape hatch, and every X11 path in the tree stays live
    behind it) and a **pure-Xorg** session is not written to at all. It landed here rather than with
    S43-c because the reasons had already accumulated: the native canvas has been validation-clean
    since S11-c, the X11 resize black flash does not happen there, the file picker gets a real
    `zxdg_exporter_v2` parent instead of an XWayland `x11:<xid>` token KWin will not honour, and HDR
    output is impossible over X11. **New `docs/wayland.md`** is the reference for everything the
    flip changes — file dialogs, tablet, window icon, sub-window popovers, clipboard, cursors — and
    §5 of it is the list only a human on a real session can check.
    - **Two real cursor defects came out with it**, both behind the user's "hovering around chrome
      seems offset in some cases" report and both *wrong images*, never wrong coordinates.
      (1) FLTK's Wayland driver treats an `Fl_RGB_Image` cursor's size **and** hotspot as LOGICAL
      units and re-applies the buffer scale itself, so handing it Mosaic's device-resolution art
      drew the cursor at `scale ×` its size and pushed an off-centre hotspot `scale ×` too far —
      ~10–15 px for the pan and fit-to-path hands on a 2× output. `ui::CursorImage` now carries
      `logicalW/H` + `logicalHot{X,Y}` beside the device bitmap, and the canvas uses FLTK's own
      HiDPI idiom (`Fl_RGB_Image::scale()` + the logical hotspot). At scale 1 — X11, macOS — every
      step is an identity. The cursor caches were keyed on nothing, so they are now dropped on a
      content-scale change (that is the "*sometimes*" in the report: a window moved between a 1×
      and a 2× output).
      (2) FLTK resolves stock cursors on Wayland by their legacy Xcursor names and falls back to a
      **15×15 XPM with a dead-centre hotspot** when the theme lacks one. `FL_CURSOR_NWSE`/`NESW`
      ask for `fd_double_arrow`/`bd_double_arrow`, and **`breeze_cursors` — the KDE default —
      ships neither**, which is exactly what Mosaic requests for the Move/Crop/Shape/Type corner
      handles. New `ui::nwseCursor()`/`neswCursor()` are the rotate cursor's own double-arrow baked
      to ±45°, swapped in **on the Wayland backend only** (X11's `XCreateFontCursor` cannot miss,
      and making it unconditional would change the resize cursors for every X11 user).
    - **NOT done, and on purpose.** `xdg_dialog_v1` (the Settings dialog's modal dim) is still
      blocked exactly where it was: `get_xdg_dialog()` needs the dialog's `xdg_toplevel`, and FLTK
      1.4.5's public `FL/wayland.H` exposes only display/xid/surface/compositor/buffer_scale — the
      toplevel lives in FLTK's private `struct wld_window` and, the dialog being decorated, is
      actually owned by libdecor. **Parked until FLTK exposes it.** And
      `native_window.cpp:68`'s `out.scale = 1` on the **X11** branch was **left alone deliberately**
      (comment added): X11 has no per-window buffer scale to read, and every consumer of that
      number — overlay line widths, the brush reticle, RGBA cursor rasterization — has only ever run
      at the Wayland value, so deriving it would switch all of them on for X11 users in one untested
      step. The practical consequence is that HiDPI is exercised on the native-Wayland path only.
    - The **kdialog `--attach` fork's** documented retirement condition is now **met** (a default
      Plasma session takes the portal path with a real `wayland:<handle>` parent), but the fork is
      **kept** until that has been seen working on a real session — and it is still the only modal
      picker a pure-Xorg / `FLTK_BACKEND=x11` KDE user gets. Noted at the gate in `file_dialog.cpp`
      and in `docs/export-system-plan.md` §8.
  - **S53-a — whole-document image operations, on one engine.** `render::buildCropCommand` already
    *was* a canvas-resize-with-anchor-and-rotate engine, so it was **generalised** into
    `render::buildDocumentRemapCommand(doc, newW, newH, worldToNew, …)` — "the crop shift" replaced
    by any invertible affine — and Crop is now a four-line wrapper over it. Canvas Size (9-point
    anchor), Image Size, lossless Rotate 90/180 + Flip H/V, arbitrary Rotate and Trim to Content all
    inherit the group push-down, the masked/singular-group rule, the canvas-locked `TextureLayer`
    rule, the delete-mode bake, the expansion fill and the single-undo-step guarantee identically,
    instead of six copies of five special cases drifting apart. New `src/render/document_ops.*`;
    **new `docs/image-operations.md`**.
    - **The resampler is now public**: `src/render/resample.{hpp,cpp}`, lifted verbatim out of
      `compositor.cpp`'s anonymous namespace — same formulas, same constants, zero behaviour change
      — so the document ops, the export resize stage and the tests can reach it. `compositor.hpp`
      includes it, so every existing caller compiles unchanged. `shaders/composite_tile.comp` still
      mirrors these kernels line for line: a formula edit lands in both.
    - **The guides rebase — a plain pre-existing bug, now fixed.** Guides are stored in document
      pixels and *nothing* rebased them: every crop resized the canvas, moved every layer, and left
      the guides at their old coordinates. New `core::SetGuidesCommand` (capture-old-on-first-apply,
      mirroring `ClearGuidesCommand`) replaces the list inside the same `CompositeCommand`, so undo
      restores every dropped guide. Axis-preserving remaps map the coordinate, a quarter turn swaps
      horizontal↔vertical, and an arbitrary rotation **drops** the guides rather than leave them
      stale — the model cannot represent a slanted guide.
    - `core::Selection` grew **`remapped`** (an exact index permutation for the lossless grid ops —
      flip and flip back is byte-identical) and **`scaled`** (Image Size). Neither takes a
      `ResampleFilter`: that enum lives in `render`, which depends on `core` and not the reverse.
    - `buildOrientCommand` takes **no filter parameter at all** — the type system, not a comment,
      keeps the UI's "Lanczos 3" from silently convolving a 90° turn.
    - ⚠ **Known limit, stated out loud:** `convolveInto` caps its widened footprint at 8 source
      texels, so an *extreme* minification aliases slightly. The proper fix is mip-style
      pre-downsampling; until then, reductions past ~8× are better done in two passes.
      (`docs/image-operations.md` §8, pinned behaviourally by `tests/test_resample.cpp`.)
    - **S53-b — the UI half** rides the same batch: the new `src/ui/image_ops_panel.{hpp,cpp}` corner
      panel (live on-canvas preview, reusing the crop shaders) plus the
      File/Edit/Image/Layer/Type/Select/Filter/View completion and **Help ▸ About Mosaic** in
      `app_window.cpp`/`menu_bar.*` — which is also where `Layer ▸ Combine Paths ▸ Add/Subtract/
      Intersect/Exclude` exposes the boolean kernel. **`cbTodo` no longer backs a single menu item.**
      That is the source of the new msgids noted at the bottom of this entry.
  - **Path booleans (S28's deferred half) — BUILT.** `src/core/vector/boolean.{hpp,cpp}` plus a
    third `vec::Geometry` alternative, `BooleanCompound { BoolOp op; std::vector<Object> children; }`
    — which makes `Geometry` **recursive**, so `(A ∪ B) − C` is one live object on one layer.
    Taught to `flatten` (recurse at the same tolerance and device transform, then resolve the op on
    the polylines — so **no Bézier–Bézier clipper exists anywhere in the tree and none is needed**),
    to `to_path`, `hit` and the `.mosaic` writer/reader. The kernel snap-rounds onto a fixed integer
    lattice, which is what makes `orient2d` an exact `int64` determinant and makes "touching" an
    arithmetic fact rather than a tolerance; coincident edges, vertex-on-edge, self-intersection and
    mixed winding each have a named handler. Lineage is Vatti 1992 and Bentley–Ottmann 1979, both
    published well before 1995 — recorded in `boolean.hpp` and `docs/vector-model.md` §9, which the
    boolean slice wrote and which is the reference.
    - **Serialization does not move `kFormatVersion`**: a compound is written as a `"path"` carrying
      the **baked** outline plus a `"boolean"` side-car. An older build then reads a shape it fully
      understands (it merely stops being live) instead of rejecting the payload and — via docio's
      tolerance rule — inserting an empty vector layer that a re-save would make permanent.
    - **NOT done:** **Divide** (the stained-glass op) is still out — it is the only boolean that
      produces *multiple* objects and wants the group-of-layers treatment. Complexity is O(E²) in
      the flattened edge count, and the lattice is fixed at 1/1024 of a local unit (past roughly
      25600% zoom it, not the flattening tolerance, becomes the coarse step).
  - **The Pen chrome moved to its own lane (binding 6).** It used to ride the 64-entry guide line
    lane as a bag of plain segments capped at 40 of them — so a path over ~17 nodes silently lost
    its marks *and* starved the document's own guides, and it read as weak because that lane draws a
    flat coloured line with no casing and no knob idiom. Now: 512 knobs + 512 stems, drawn by
    `canvas_present.comp`'s `penChrome()` in the crop/transform language (square knob = cusp, round
    = smooth, smaller round = handle tip; hollow = unselected, filled = selected, blue border =
    hovered; `chromeShadow` on everything; every size hung off the transform handles' own `H`, so it
    is DPI-correct by construction). Consequences: **every** node's handles are drawn *and*
    grabbable (`penHitTest` gained a third tier below anchors, so what you can grab is exactly what
    you can see); the **path spine is drawn in edit mode**, which it never was; a **closing-loop
    ring** shows the click radius that shuts an open path, an affordance that had been a pure hit
    test; and `penPathPolyline` emits **all** contours separated by a `kPolylineBreak` sentinel the
    shader skips over — the mechanism a live `BooleanCompound` preview needs.
    - Two follow-ups the boolean work exposed, recorded in `docs/vector-model.md` §8: the text-on-
      path wrap heuristic (`baked.size() == 1 && front().closed`) silently loses wrap-around on a
      multi-contour result; and `render::mergeDownVector`'s stroked-bounds and fill-rule refusals
      exist precisely because there was no boolean kernel, so relaxing them is now a real option.
  - **Layer panel — five thumbnail-staleness fixes + a path badge.** Thumbnails froze whenever the
    thing that changed moved no revision the cache key read: a layer's **effects** (new
    `Layer::effectsRevision()`, the `maskRevision()` story one field down — a group composites its
    children *through* `applyEffects`); a **nested adjustment's** params (it has no
    `contentRevision()` of its own, so a group holding a Levels layer froze the instant its sliders
    moved, and never drifted again); the **document size** (a crop moves no layer revision and no
    world transform, yet every doc-resolution thumbnail re-frames); a **descendant** text/texture
    layer's renderer-filled **pixel cache** (the leaf key carried it, the subtree key did not); and
    panel-local edits now re-derive the badge rather than keep a stale one. `typeBadgeFor()` /
    `typeBadgeWidth()` are now pure and unit-tested — the width oracle *must* agree with `draw()`'s
    branch chain or the layer name runs under the badge. New **VectorPath badge**: a pen path is a
    `VectorLayer` exactly as a rectangle or a star is, so it wore the generic shapes mark; it now
    gets a bézier segment threaded through two anchor squares, one hollow and one filled.
  - **Owed across the batch:** `po/mosaic.pot` is now **further** out of date — the menu-completion
    work adds many msgids on top of the ~40 the previous batch already left unextracted; re-run the
    `pot` target before the next translation pass. `docs/icons-needed.md` gained the path badge and
    the texture chip rows (that file is S52's inventory, not a promise for this session).

- **PARALLEL SESSION 2026-07-28 — eight slices, `bc63366`…`58ac581`, all on one verification pass**
  (2501 test cases / 1,561,248 assertions green in release; `--gui-frames` clean on X11 *and*
  Wayland; suite green again under `MOSAIC_GPU_PROFILE=floor`). Each slice has its own commit; the
  detail lives there and in the per-area docs. Headlines, and the parts that are *not* done:
  - **The file picker froze the whole app on X11/KDE** (`bc63366`, user-reported). It fork()ed and
    then blocked the main thread on `read()`+`waitpid()` for the dialog's whole life — no repaint,
    no present, no timers, input piling up to replay in a burst. Six further hangs closed alongside
    (`setenv` between fork and exec in a multithreaded process; a `POLLNVAL` busy-spin on portal
    disconnect; an unread Request object path; `FL_CLOSE` slipping past the input guard; the guard
    also firing over FLTK's own in-process chooser; `EINTR` truncating the path).
  - **M4 common raster formats** (`b0cdd9c`): WebP/AVIF/TIFF/GIF encode+decode, all optional deps,
    plus the owed EXIF **write** half. AVIF refuses to appear at all unless libavif offers libaom or
    SVT-AV1 — the never-rav1e decision enforced at runtime, which packaging cannot do.
  - **Export As rebuilt** (`0d98ca6`) with an interactive preview (cursor-anchored wheel zoom, drag
    pan, drawn 1:1/fit glyphs). ⚠ **Still hard-stripping metadata** and the **ICC row is unwired**:
    `ExportRequest` carries no `ExifData` and `RenderInput.iccProfilePath` is unwired app-wide, so
    both controls would have been lies. That is the next export slice.
  - **Gradient automask** (`1982615`), **S36 cross-type Merge Down** (`c06e70b`), **S28 pen/path
    tool** (`c03a0b5`) — see the §10 tracker entries.
  - **Brush** (`fcb6eff`): the airbrush freeze was a **dead preset key the reference has no reader
    for**, turning six ordinary presets into one-dab-per-millisecond airbrushes; and *every stroke
    the app ever laid ran seed 0*, so single dabs never advanced a multi-frame tip and fuzzy
    rotation/scatter/mirror repeated across 23 presets. Census 94/17/6 → **104 Exact / 7 / 6**.
  - **S60 GPU timestamps** (`58ac581`): `TileCompositor` had **no instrumentation at all** and every
    `Lane::Gpu` row reported CPU submit wall-clock, which is why item 13 was undecidable rather than
    merely unmeasured. ⚠ **The backend flip is NOT included** — every composite site still passes
    `Backend::Cpu`. `docs/s60-performance-plan.md` §7 now states the gate as five runnable
    conditions; running them is the next S60 step.
  - **Owed across the batch:** `po/mosaic.pot` is stale (this session added ~40 msgids and removed
    one); the individual commits were **not** built in isolation, only the final tree was.

- **Stuck pressed-state across theme switches — FIXED 2026-07-24 (user-reported).** Every button in
  the app kept its pressed / toggled-down highlight from the *previous* palette across a dark↔light
  switch, both directions; most visible on toggles that SIT down (B/I/U/S, an open Style…/3D…
  button, a tool's option toggles), which wore a dark slab in light mode and vice versa while
  everything around them changed. `Fl_Button::draw()` paints the down box with `selection_color()`,
  and `FlatButton`'s constructor baked a concrete `controlActive` RGB into it — and unlike the label
  and the rest fill, `controlActive` has **no semantic FLTK slot to ride** (`FL_SELECTION_COLOR` is
  the accent), so it could not follow `applyTheme()`'s colour-map swap for free, and `reapplyTheme()`
  only ever un-froze the rest fill. The **base** now re-reads it, so one edit reaches the whole
  button family; the three subclasses that override `reapplyTheme()` to restore a fill of their own
  (`FilledButton`, tool_options' `TintedButton`, the AskOrTell `StageButton`) now **chain to
  `FlatButton::reapplyTheme()` first**, and that contract is written on the base declaration because
  an override that forgets to chain silently re-opens the bug for its subtree. `TintedButton` needed
  the chain anyway — its fixed tool-accent fill was likewise reset to the semantic controlBg by the
  base and only came back on the next hover. Regression test switches dark→light→dark over a live
  `FlatButton`/`FilledButton`/`GlyphButton`; **verified to FAIL with only the reapplyTheme refresh
  removed** and pass with it.

- **AskOrTell alert sounds — BUILT 2026-07-24 (user-reported gap).** The dialog now makes the noise
  a native one does. New `platform::playSystemSound` (`src/platform/system_sound.{hpp,cpp}`), keyed
  off the Stage **icon** (the icon already *is* the face's severity); `Restore` rides the *question*
  sound for the same reason it has its own icon — a healthy file. Backends: **libcanberra** on Linux
  (new **optional** dep, LGPL-2.1+, `pkg_check_modules(... QUIET)` — absent ⇒ silent dialogs, no code
  change), `NSBeep()` on macOS (plain C in AppKit, so no `.mm` sibling was needed), `MessageBeep()`
  on Windows. **No in-app toggle, by design** (user's call): every backend routes through a
  preference the OS already owns, so a second switch would be the no-toggle-for-strictly-better case.
  - ⚠ **The event-id fallback chains are load-bearing.** The Sound Naming Spec listing an id does not
    mean a theme ships it: the reference `freedesktop` theme has `dialog-information`/`-warning`/
    `-error` but **no `dialog-question`**, which fails `CA_ERROR_NOTFOUND` *in silence* — that would
    have muted exactly the face Mosaic shows most (Save your changes? / Restore? / Flatten History?).
    Each meaning now walks a chain to `bell`, and the winner is remembered per meaning. Found by
    probing the real module, not by reading the spec.
  - The subsystem is **OFF until `main()` arms it** — `ctest` presents thirteen faces, and library
    code must never be able to beep a developer's machine. `systemSoundsEnabled()` exists so the
    test can *assert* that silence. Sound fires on the hidden→shown transition only, so a staged
    flow (confirm → progress → summary) is one interruption, not a burst.
  - FLTK's `fl_beep` is deliberately **not** a fallback: its X11 path is `XBell` (routed nowhere on
    a modern desktop) and its Wayland path `fprintf(stderr, "\007")`, which spams the launching
    terminal instead of making a sound. Windows is the only platform where FLTK's own beep is right,
    and we call `MessageBeep` directly there anyway.
  - macOS **cross-compile-verified** (both slices, `-Werror -Wpedantic`, `_NSBeep` resolving through
    the already-linked AppKit); the Windows branch is unbuilt, like the rest of the Windows port.

- **Adjustment automask + the last two stock FLTK alerts retired — BUILT 2026-07-24.** Two small,
  unrelated user asks.
  - **Automask (S32, docs/adjustment-layers.md §5).** `Filter ▸ Adjustments ▸ …` (and `Filter ▸
    Blur ▸ …`, which rides the same entry point) now inserts the new layer **already masked to the
    active selection** — the "select, then adjust" reflex every comparable editor has. The mask
    comes from `core::maskFromSelection`, the same resampling `Select ▸ Mask from Selection` uses,
    and is set on the layer **before** the `AddLayerCommand` so it rides *into* that command:
    **one History step**, undo removes layer and mask together. A brand-new adjustment carries the
    identity transform, so `maskFromSelection` takes its 1:1 fast path and the mask is the
    selection's coverage verbatim — feather and AA edge intact, which two new
    `tests/test_adjustments.cpp` cases pin (byte-identical outside, byte-identical to the unmasked
    result inside, *halfway* under half coverage). The gate is `Selection::anySelected()` alone: it
    is false both for "no selection" (a reveal-all mask would be a lie in the dock) and for an
    active selection of nothing (a layer grading zero pixels reads as "the menu item did nothing").
    The selection is deliberately **left active** — the ants then read as "this is what got masked"
    — and the status bar says `Masked to the selection`.
  - **`fl_alert` is gone from `src/`.** The two surviving stock FLTK message boxes — the New-Document
    "Custom…" RGB-profile rejection and Settings' CMYK-profile rejection — are now themed
    `ui::AskOrTellDialog` Warning "tells" hosted on their own dialog. Both keep their existing msgid
    as the body (so 74 catalogs' translations survive) and gain a short headline. `docs/askortell-
    dialog.md` now states the rule outright: **AskOrTell is the app's only message box**, the
    `fl_alert`/`fl_message`/`fl_choice` family is banned from `src/`, and the pattern for code that
    cannot reach `MainWindow::tellError` is a stack `AskOrTellDialog` + `ask()` with its own window
    as host. `fl_beep()` is explicitly NOT covered — it is a sound, not a window, and the three
    remaining calls are the file-picker-in-flight quit guards, where opening a second modal is
    exactly the hang the guard exists to prevent. Template regenerated (3 new msgids, none removed).

- **Empty-state idle pass — BUILT 2026-07-23 (this session; awaiting user visual pass).** The
  no-document state moved from the opaque `EmptyStateView` FLTK sub-window into the canvas
  itself: a new `canvas_idle.comp` compute pass (the drag pass's structural sibling — own
  pipeline, descriptor set and 112-byte push block) renders an ambient "ripple basin" dot field
  behind the open-an-image invitation, replacing the bare background clear while documentless
  and blending the settling field OVER the present pass's output while a document arrives — the
  fade the old sub-window could never do (it could only sit opaquely over the Vulkan surface).
  Design settled interactively the same day (three-round artifact; ripple direction "C", copy
  option A: "Open an image" / "click anywhere, or drop a file into this window" / "File > New
  starts a blank canvas", drag-over headline swap "Drop it anywhere"). The invitation is a
  three-row atlas (idle / hover / drag-hot) baked at device resolution — text via
  `Fl_Image_Surface` white-on-black coverage tinted per line (the `rasterizeOverlayTile`
  conventions), the frame + icon composited in pure unit-tested pixel code
  (`ui/idle_invitation.*`) — re-baked on theme/DPI change, sampled 1:1 with `texelFetch`. Fade
  choreography is pure math in `ui/idle_fade.hpp` (settle 180 ms cubic-in; 80 ms beat + 420 ms
  cubic-out bloom; phase-continuous global clock, wrapped in double like the ants crawl). The
  canvas takes the invitation's input in a `handle()` intercept (click-anywhere opens, hand
  cursor + hover row, full DND conversation with the direct-`FL_PASTE` contract); the
  window-level chrome-drag mirror drives the hot bloom via `setIdleDropHot`. `EmptyStateView`
  retired (files deleted; tests ported to `test_idle_state.cpp` + new fade/bake/input pins).
  All three presets green, `--gui-frames` validation-clean documentless AND with a document
  opening (the blend-over path), self-screenshot verified (field + invitation + animation).
  **User feedback round 1 (same day, folded in):** field alpha tuned down (0.16/0.60 →
  0.11/0.42 — "reads a little too busy"); light mode pulls the resting frame toward textMuted
  and hover toward text (the palette border nearly vanished on the light canvasBg); and the
  frame's dash pattern now flows around the corner arcs on a single perimeter-length parameter
  with the period snapped to a whole dash count (solid corners next to dashed runs read as a
  mistake). Both themes re-verified by self-screenshot (light forced via `--config`).

- **New-Document round 6 — BUILT 2026-07-22 (awaiting user visual pass).** The
  round-6 feedback, all ten items: TitleDrum holds "New" still and rotates only the ambition
  (word list grown: Document/Creation/Artwork/Adventure/Story/Vision/Wonder/Masterpiece);
  GalleryCard title/subtitle lines now CLIP + scroll via the shared Marquee — this was also the
  gallery's "infinite boldening" (unclipped `fl_draw` centring long paths past the card into
  gutters nothing ever erases); the two summary readouts are left-aligned ScrollingLabels for
  the same reason; rail order is Recent, Templates, Print, Screen, Texture with line-art badges
  on the personal shelves (clock = Recent, birthday cake = Templates, ahead of the birthday-card
  template); the DPI quick-pick zone widened to chevron + unit tag with the link-hand cursor
  over it; clicking bare chrome now unfocuses text fields APP-WIDE (unconsumed FL_PUSH in the
  global `Fl::event_dispatch` guard → `Fl::focus(nullptr)` → NumberField commits); a selected
  Recent seeds the greyed Name box with the manifest's own title (`DocumentFileInfo.title`, new,
  test-pinned) falling back to the file stem, and the user's editable name is restored on
  leaving; recents drop the `.mosaic` suffix from card titles (foreign images keep theirs); and
  the orientation switch's "doesn't visually change" bug was the SQUARE deadlock — swapOrientation
  early-returned before updateSummary, so on a square canvas the pref turned the sheets while
  the switch stayed neutral; a live switch now shows the explicit pick on squares (disabled
  file-backed squares stay neutral). All three presets green + `--gui-frames` smoke clean.
  **Round-7 follow-ups same day:** the hand cursor is re-asserted per enter/move (Fl_Input
  re-sets the I-beam every FL_MOVE, so the transition-only set flickered); and the **titlebar
  now announces a .mosaic document by its manifest title** (tabs deliberately keep the file
  name; foreign-backed documents keep the filename in the titlebar too). Open design note: Save
  As does not adopt the typed stem as the document title, so an auto-named "Untitled N" stays in
  the titlebar until a rename affordance exists (File → Rename / tab double-click) — raised with
  the user.

- **S48-b + New-Document round 5 + thumbnail-cache retirement — BUILT 2026-07-22 (earlier
  session; awaiting user visual pass).** S48-b landed whole (PRVW chunk + MIME/.desktop/install
  rules + mosaic-thumbnailer + KIO plugin — see §S48-b, ticked in §10); the New-Document dialog
  took its round-5 feedback (shared-base expression evaluator w/ unfocus evaluation, SVGA/VGA
  size subtitles, TitleDrum rail title, untitled auto-naming, custom-size recents +
  Recent-gallery section captions, DPI quick-pick, Custom-ICC colour entry incl. document model
  + `.mosaic` grown fields + picker wiring); and the app-owned `stateDir()/thumbnails` cache is
  RETIRED — .mosaic cards read their embedded PRVW + the new light `io/mosaic/fileinfo` manifest
  reader, plain-image cards read the desktop's shared freedesktop thumbnail cache (read-only;
  existing io/brush MD5 keys it) + a header-only `io::probeImageDimensions`. All three presets
  green; next per the S48 arc: Build 2 (H4 + adaptive switching).

- **S32 Non-destructive adjustment/filter framework — BUILT 2026-07-17 (this session; awaiting
  user visual pass).** The typed parameter system (`core/adjustments.{hpp,cpp}`: per-kind
  `AdjustmentParamDesc` tables — key/label/range/default/step/type — over the unchanged params-bag
  storage; compositor + menu-insert + editor all read through it, hostile bags clamp, absent keys
  default); **real math for every scalar kind** in `applyAdjustment` (Levels, Exposure in linear
  light via the LUT pair, Hue/Saturation hexcone HSL, Color Balance w/ smooth tonal bands +
  W3C-setLum preserve-luminosity, Threshold, Posterize — Curves landed in S34; a defaults/empty bag EARLY-OUTS byte-identically, test-pinned); `SetAdjustmentParamsCommand`
  (whole old/new bag, per-control coalescing); **Filter ▸ Adjustments ▸ …** inserts above the
  active layer inside its group + selects it (one AddLayerCommand; only implemented kinds offered,
  escaped `\/` labels); the **editor is a pinned corner popover** (`ui/adjustment_panel.*`, the
  Type/"3D…" placement — user redirected mid-session away from a first-cut modal) **triggered by
  the adjustment layer being the ACTIVE layer** (`updateAdjustmentPanel` onFrame transition watch;
  Esc-dismissed stays closed until re-selection), schema-generated rows streaming through
  `applyAdjustmentField` (the applyTextBlockField twin: same-control edits coalesce into one undo
  step), Reset = one undoable defaults re-seed, per-frame drift re-sync so undo moves the sliders;
  **dock**: half-filled-circle chip badge (texture-chip sibling, clickable → reopen) + context-menu
  "Edit Adjustment…" (absent when no params). PhotometricMatch got a schema (the S55 grade is now
  hand-tunable from the dock) but stays off the menu; its compositor reads stay RAW (estimator
  values may exceed editor ranges — byte-compat preserved, sky goldens untouched). The math is
  decades-old textbook territory. Docs: **docs/adjustment-layers.md** (new) +
  document-model/compositor cross-refs updated. Tests: `test_adjustments.cpp` (schema sanity,
  seed/clamp/fallback, command coalesce boundaries, analytic math invariants incl. +1 EV = ×2
  linear + hue-rotation primaries + luminosity preservation + posterize lattice) +
  `test_adjustment_panel.cpp` (headless census across kind switch, funnel field ids, no-edit drift
  sync, Reset) — 1815 cases green release+asan, gui-frames clean. **Same-day follow-up (user):
  the row THUMBNAIL is a live scope preview** — `render::adjustmentPreview` (the compositeChildren
  walk truncated at the adjustment; agrees with group thumbs by construction) at ≤96px, cached
  behind `adjustmentScopeRevision` (own knobs + subtreeRevision of every sibling below — and
  subtreeRevision now folds maskRevision/clipToBelow, also fixing stale group thumbs on child-mask
  edits), refreshed on the text-thumb settle timer during panel drags; invisible adjustment / empty
  scope preview the honest plain/transparent answer. +4 preview tests. **Follow-up round 2 (user,
  same day): PRO CONTROLS + polish** — (1) the dock badge is a passive half-circle type mark now
  (the chip language means "opens an editor"; the panel opens by the layer being active); (2)
  **Color Balance = three tone-band `ui::ToneWheel`s** (AA hue disc + puck, double-click recentres;
  puck ↔ cr/mg/yb via `core::colorBalanceToPlane/FromPlane`, 120°-apart axes, achromatic mean
  preserved — ⚠ named ToneWheel because **ColorWheel already exists** in color_surfaces.hpp; the
  first cut collided (same namespace, ODR) and segfaulted every ColorFlyout teardown); (3)
  **Levels = histogram + black/gamma/white handles** (gamma handle at t=0.5^gamma) **+ output
  ramp**; **Threshold = histogram + cut handle** — fed by `render::adjustmentBackdrop` (the scope
  walk stopped BEFORE self); (4) **Hue/Sat slider tracks carry value ramps**
  (`ScrubSlider::setTrackFill`, default rendering untouched); (5) **occlusion fade**: the panel
  drops to 45% while overlapping the visible document with the pointer elsewhere — hand-composited
  translucency (host reconstructs the canvas beneath from m_lastComposite + view + 8px screen
  checker; panel renders children into an Fl_Image_Surface — NEVER the window itself, it snapshots
  BLACK — and blends); (6) **Grayscale grew method (Choice: Luminance/Luma/Average/Lightness/
  Value/R/G/B; default Luma = the pre-S32 formula byte-compat) + strength %** — the schema gained
  the Choice param type (Dropdown row; the bag stores the option index). **Round 3 (user testing,
  same day):** context "Edit Adjustment…" REMOVED (right-click selects → panel opens anyway); badge
  ink back to the full-contrast one-ink (passive glyph stays); **panel yields while Style…/3D… are
  shown** (they share the corner and stack BELOW it — the "Style can't open" bug — seen-latch reset
  so it self-reopens when they close); **fade rebuilt**: blend composed OUTSIDE draw() (an
  Fl_Image_Surface inside draw() corrupts the GC — the crash), invalidated by a composite+view
  (zoom/pan/rotation)+panel-rect fingerprint (pan/zoom never recomposite — the stale-ghost bug) +
  content-dirty flag, all blits depth-3 RGB (depth-4 fl_draw_image channel-misreads = the magenta
  artifact; ToneWheel disc converted too); **Grayscale methods rewritten** (No chrominance / Luma /
  Red / Green / Blue filters — Average/Lightness/Value dropped as near-indistinguishable) **+ a
  Grays palette-size param** (2..256; "show this image with 3 grays"; 256 = continuous byte-compat
  default); the Hue/Sat ramp strips follow the bar's rounded corners. **Round 4 (user, same day):**
  the faded panel's controls piled at the top-left — `Fl_Widget_Surface::draw(child)` SUBTRACTS a
  non-window widget's x/y from the origin, so the deltas must be passed
  (`surf.draw(child, x, y)`); and the Style/3D fight fix WIDENED — the panel now yields for the
  ENTIRE text session (`textEditTarget` gate, not just panels-shown) and auto-opens restore
  `Fl::focus` (a selection-opened panel must never steal typing focus). ⚠ round-4's "can't open
  Style/3D" was never reproduced headlessly — if it recurs, collect the exact repro before
  theorizing. **Round 5 (user directive): the UNIFIED CORNER-PANEL MODEL** — "I'm tired of the
  mountain of issues stemming from this one panel model" → `ui::PanelArbiter` (pure logic, no
  FLTK): every corner panel registers as EXPLICIT (button-toggled; `valid()` evaporates a request
  when its session/anchor dies; toggle-over-toggle = the Style/3D exclusivity for free) or
  CONDITIONAL (`wants()` → context token, e.g. the active adjustment layer id; Esc suppresses
  exactly that token until it changes = the seen-latch as a rule). Explicit outranks conditional;
  the QUEUE = re-resolution (close Style… → the adjustment panel returns). `syncCornerPanels()`
  (onFrame + each button toggle) reconciles widget reality vs the arbiter — external hides
  (theme/bar-rebuild/Esc) self-heal. openTypePanel/openType3dPanel/updateAdjustmentPanel shrank to
  guards + toggle + sync; m_adjPanelSeen and the per-panel yield logic DELETED. 5 headless arbiter
  cases pin the fight classes (`tests/test_panel_arbiter.cpp`). 1823 cases green release+asan,
  gui-frames clean.
- **S33 Blur filters (the blur gallery + Depth of Field) — BUILT 2026-07-17 (awaiting user
  visual pass).** Design + build in one arc (docs/blur-filters.md). **Seven
  spatial AdjustmentKinds** (Gaussian/Box/Motion/Radial spin+zoom/Surface/Lens/DoF) end-to-end:
  schema tables, docio tokens, **Filter ▸ Blur ▸ …** (visible defaults — the sanctioned
  identity-at-defaults deviation; centers seeded to the document center on insert), S32 popover
  picks them up untouched. **CPU kernel engine** `render/blur.{hpp,cpp}`: premultiplied,
  clamp-edge, alpha diffuses; lens gathers in linear light behind a SINGLE lower boost
  threshold; DoF = signed-distance band field interpolating a 5-level pyramid, each level
  blurred FROM THE SOURCE. **Compositor**: the spatial branch ahead of the color loop (existing
  docs byte-identical), `blurAdjustmentReach` SUMS stacked reaches, `groupLocalExtent` grows
  content AND window pullback, `compositeRegion` expand+crop keeps **region == crop(full)
  byte-exact** (the money test); preview-scale threading so 96px scopes blur proportionally; a
  PRE-EXISTING masked-adjustment bug fixed (mask sampled stretched onto region buffers —
  placement-aware now, test-pinned). **DoF canvas gizmo**: focus line + band/feather edges +
  move/rotate knobs in the crop-chrome language (binding 11 SSBO, hairlines + square knobs;
  amounts stay in the popover, deliberately), tool-independent, PUSH/DRAG/RELEASE claimed as a
  pair, drag latches press-time inverse (the bend-plane lesson), one undo step per gesture
  through `applyAdjustmentFieldOn` sharing the panel's coalesce stream; drags + panel scrubs
  composite DRAFT with a full-quality settle (`blurScrubInFlight`). **Vulkan compute lane**
  `render/blur_gpu.{hpp,cpp}` + 4 `shaders/blur_*.comp` behind `setBlurRenderOverride`
  (the S55-h seam pattern; lazy create, per-call CPU fallback; Box/Motion/Radial refused —
  CPU is cheaper than readback): gaussian/surface/lens/DoF parity-pinned on RADV (meanAbs
  ≤4e-8, 0 outliers; DoF focus band byte-exact through the GPU), CPU stays the golden lane.
  Tests: `test_blur_kernels.cpp` (analytic kernel signatures,
  band/plateau byte-exactness, alpha diffusion, mask gating, region==crop across six subcases,
  two-resolution equivalence, 7 golden pins) + `test_blur_gpu.cpp` + S32 suite extensions —
  **1848 cases green release+asan, gui-frames clean X11+Wayland.** ⚠ ASan note: LSan now shows
  a system-libfontconfig-only leak trailer (~650KB, zero Mosaic frames) — verified PRE-EXISTING
  at the session's base commit via a stashed-tree baseline; environmental (rolling-release
  fontconfig), not ours.
- **S30-e 3D-text Layer-Effects integration — BUILT 2026-07-16 (awaiting user visual
  pass).** The §12 contract as specced: colour/gradient/pattern overlays are evaluated in the
  glyph's 2D design space, baked into per-material **overlay-albedo maps**
  (`core/text/extrude_overlay.{hpp,cpp}` — blend mode + opacity composited over the albedo once, on
  the CPU), and sampled by BOTH render lanes through the mesh's per-vertex UVs — the **front cap**
  always, walls/bevels only with the new `Extrude::overlayWrapSides` ("Wrap effects onto sides" in
  the 3D panel), the back cap never. The design shades WITH the surface (albedo + metal F0), and the
  compositor **strips the overlays from `applyEffects` for extruded text** (they'd otherwise smear
  over the projected 3D rectangle — the §12 outcome this exists to prevent) while shadows/glows/
  strokes/bevel/satin still run on the composited result. The overlays joined the text-cache
  validity key (`TextLayer::cachedOverlays`) so an effects edit re-renders the solid; the 3D popup
  viewport + LE-modal preview both carry the effects. GPU lane: UVs in the vertex packing, overlay
  maps on binding 8, parity green on the RX 6600 XT (incl. wrap mode). Flat text byte-identical
  throughout. **FEEDBACK ROUND same day (5 items, all fixed):** (1) wrap mode now paints the WHOLE
  solid — walls/bevels sample a second per-material map in the **unrolled side domain** (outline
  arc-length × depth, `ExtrudeVertex::side` + `SideStation` return map) so patterns tile
  undistorted instead of stretching into lines, gradients keep the design continuation, and the
  back cap takes the design (mirrored); (2) the 3D popup viewport refreshes on Layer-Effects edits
  (`refreshType3dPanel` from the LE dialog's applyLive/commit); (3) the Bevel "blocks that do not
  connect" was the dirty-REGION composite cropping the effect context — `renderLayer` now renders
  an effects layer's full footprint and crops after (region == full-composite window, test-pinned),
  plus AA-SDF + a smoothing floor for edge quality — *and the residual "lines through the beveled
  surface" was 8-bit BANDING*: the sky's TPDF dither is now shared (`common/dither.hpp`, sky
  goldens byte-identical) and applied to the bevel shade ramp (silhouette-anchored keys keep
  region==full byte-exact) and to the gradient-stop editor's ramp strip (user-requested); (4) the
  Style/3D panel layout corruption was
  build() running inside a group-stretched hidden popover — both panels normalize to their base
  footprint before layout (test-pinned), and (5) the 3D panel got the Type panel's ScrollView +
  region-clamped height (it scrolls now).
  Core flood engine `core::bucketFillCoverage` (`src/core/fill.{hpp,cpp}`, pure/FLTK-free/unit-tested):
  a click → tolerance flood (shares the S17 `wandColorDistance` metric) → per-pixel FILL coverage
  (solid interior + optional 1px outer feather; contiguous 4-conn flood or global "match all").
  The interactive tool (`ToolId::BucketFill`, "K") now has a canvas path: `VulkanCanvas::pushBucketFill`
  → `MainWindow::bucketFillClick` floods the active raster layer, intersects with the active selection
  (whole layer if none), fills the active foreground via the shared S39 `render::computeFill`, and
  lands ONE `core::FillCommand` ("Fill" in History). Options: Tolerance / Contiguous / **Anti-alias**
  (new) / Opacity. **Pattern/gradient fill already ships in Edit ▸ Fill (S39, fully working — the old
  "(with S21)" greying was already gone); the bucket tool paints the foreground.** See
  `docs/bucket-fill.md` (plain 1980s flood + tolerance + pattern tiling; nothing edge-aware). 1658
  tests green.
- **S22 Gradient tool — BUILT 2026-07-15 in a worktree (pending merge + a user visual pass).** The
  interactive tool over the already-shipped S25 vector gradient stack: drag on the canvas to author a
  **full-bleed `RectShape` + `vec::Gradient`** `VectorLayer` (an editable, maskable "gradient layer"),
  with an on-canvas axis/handle gizmo, four shapes (Linear/Radial/**Elliptical**/Conic — Elliptical =
  a Radial with an anisotropic transform), per-segment **blend curves** (`GradientStop::midpoint`,
  default 0.5 = byte-identical to pre-S22), and the reusable `GradientFlyout` for stops/spread. Re-
  selecting the tool on a gradient layer re-drags *the same* gradient; one undo step per gesture.
  Scope in `docs/gradient-tool.md` (gradient MESH deliberately NOT built). See §10 S22 for detail.
- **Phase (reconciled 2026-07-03):** the roadmap has been **non-linear** — lots of hopping. Phases 0–2
  core are largely complete. **Phase 4 vector + text is now substantially done** out of order: S25 (CPU
  vector infra), **S26 Shape tool**, **S27 Line gizmo**, and the **entire S29 Type tool** (2D core +
  on-canvas edit + Type panel + hyphenation/spell-check/vertical/variable-fonts/kerning/emoji), and
  **S30-c/-d 3D type** was built *ahead of* the S30 advanced-2D work (which is still open). **Phase 6
  inpainting** is deep (S37 engine + a big quality/perf pass + a second Resynthesizer backend; S39 brush +
  Edit→Fill), and **Smart Resize / Smart Recompose** content-aware retargeting (S16-r) shipped. Slices of
  **S60-a/-c** (interactive perf) were pulled forward. **Implemented-but-not-fully-polished:** S16-f crop
  expansion and S30-c/-d 3D type both await a final user visual pass. **Still open / the real gaps:**
  **S17** (magic wand), **S18** (select brush), **S30** (advanced-2D type), **S38** (clone stamp), and the
  big **S60 GPU-residency** arc. Phase 3 brush still has only **S19-a base** + **S19-c**. *(S16-g
  layer-panel/History polish landed 2026-07-09. **S16-i, S49 and S50 all landed 2026-07-09**, together
  with layer-name truncation, the magic-layer badge, and **Rasterize + Convert to Path** on the layer
  context menu — the two items the menu had been withholding because a greyed item is a promise.
  **S17 and S18 now have their research notes**, `docs/research-selection.md` and
  `docs/research-select-brush.md`: the wand ships as scoped; edge-aware "quick
  selection" is DECLINED.)*
  ⚠ **This is a 2026-07-03 snapshot; its "still open / the real gaps" list is spent** — S17, S18, S30,
  S38 and Phase 3's brush arc all shipped between 07-09 and 07-29, and the whole of Phase 5's filter
  gallery with them. §10 is the authority. Two things in it did not age: the roadmap is still
  deliberately **non-linear**, and the edge-aware decline was later *narrowed* rather than reversed —
  the constraint turned out to bind the UI, not the algorithm, which is what let the L1 edge brush
  ship as solve-on-release (see S18).
- **S24 Eyedropper + loupe BUILT (2026-07-15, own worktree — not yet merged).** The colour-picker
  tool now samples into the fg swatch (Alt/right → bg; drag samples live), reusing the Magic Wand's
  source resolvers + a pure `core::sampleColor` box-average; and a **GPU loupe** (present-pass SSBO
  binding 10) follows the cursor — nearest-neighbour magnifying the composite with its own pixel grid,
  a centre cell, a swatch band + a hex/RGB readout. See `docs/eyedropper-loupe.md`. 1659
  tests green. **⭐ User visual/interactive pass owed.**
- **~~Next up — CONFIRM SEQUENCING AT SESSION START (two user-directed candidates)~~ — this bullet is
  SPENT (reconciled 2026-07-29); every item it named has shipped.** Kept only for the two rulings inside
  it that still bind. **(1) S60 GPU residency** — *pulled forward 2026-07-03 by user directive* ("CPU
  compositing… frankly unbearable"); that directive is still the reason S60 outranks feature work, and it
  is not discharged: the resident lane is built and its gate passes, but the default is still the CPU
  walk (see §10 S60). **(2) Save/Export (S18-b)** — closed; real `.mosaic` Save landed 2026-07-08 and
  export landed on the M1…M4 milestone track. The Phase-2/3 tool gaps this bullet listed (**S17** wand,
  **S18** select brush, **S38** clone stamp), **S16-i** marquee polish, and the user's **Layer Effects**
  itch have all since shipped — the tracker is the current authority on what is open, not this bullet.
  The S25 vector fork is fully resolved: (b) deferred polish DONE (bbox-limited raster + stroke
  Inside/Outside via coverage-clip); (c) GPU-resident renderer DEFERRED → folded into S60.
- **⚠ OPEN (user-reported, NOT fixed):** Move-tool lag on a **full-canvas 5k×8k layer** — the
  per-drag-frame recomposite does a full blend + `toImage8` + full GPU upload, and dirty-region can't
  help (moving a full-canvas layer changes everything). Fix = the **S60-d proxy/low-res live-drag
  composite** (design fork: proxy ratio vs GPU-resident transform — consult the user); the item moved
  from the old `-c` slot when `docs/s60-performance-plan.md` §7 re-cut the split. Smaller in-canvas
  Move/Resize/Rotate drags are already on the GPU-resident fast path (done + user-verified). **Still
  open on 2026-07-29**, but the ground under it has moved twice: the resident tile lane now serves a
  live Move drag (`32dcbcb`) and the gesture-*start* stall is bounded by compositing the drag backdrop at
  the size it is consumed at (`f1e5190`) — while the gesture-**END** stall, measured as the larger of the
  two, is untouched (`docs/s60-gesture-start-stall.md` finding G3).
- **Recently completed (2026-06-24 → 07-03, all committed + pushed; this PLAN reconcile = 2026-07-03):**
  - *S52 tool icon packs + the default set "Tesserae" (2026-07-10).* The §3.13 icon-system
    finalization, as a PACK system: a pack = a folder with `mosaic_icon_pack.json` (identity +
    credits; the file IS the pack marker) + one SVG per stable tool key; user packs under
    `dataDir()/icon_packs/`; per-icon fallback to the embedded default (a one-icon pack is
    legitimate); **packs are TOOLS ONLY** (the §3.13 scope note holds — chrome/dialog icons are out
    of reach). Shipped the default pack `assets/default_tools/` — 32 bespoke colour SVGs (every
    implemented tool, every PLAN-named future tool, + a Magnetic Lasso), designed by Claude Fable 5
    (CC0-1.0, credited in docs/credits.md); the S11 inline placeholders are DELETED (tools register
    with `ui::defaultIconSvg`). Settings → Appearance split into General | Icons sub-tabs; Icons =
    selected-pack card (preview strip + bold-name/artist/link/description credit block) over a
    scrollable pack-card browser; `Settings::iconPack` persists, toolbar re-rasterizes live. Census
    test pins 32/32 keys rasterizing colourful at 20 px.
  - *S16-g layer-dock polish + professionalisation (2026-07-09).* Closed the whole S16-g backlog
    (History age re-tick that paces itself and never wakes an idle tab; the tab-entry gutter flicker
    root-caused to `Fl_Scroll::scrollbar.visible()` being a frame stale; a genuinely translucent
    offscreen drag chip with the start knob restored and the drop cues drawn over it; cursor-anchored
    ghost) and, in the same pass, gave the dock a real chrome-icon set (one-ink tinted SVGs), inline
    rename, layer locking (`SetLockedCommand`, enforced on structural edits + Move transforms), a row
    context menu, and a **width-resizable dock** persisted in `Settings::dockWidth`. ~~Rasterize /
    Convert-to-Path deliberately NOT offered — neither exists yet.~~ **Both landed later the same day**
    (`5d1efa0`), and the context menu shows each only on the kinds it can act on. Also fixed: disabled `GlyphButton`
    /word-toggle glyphs kept full contrast where a disabled `Dropdown` muted to `textMuted`; and a
    debug-only Help → Enable/Disable All Controls pair to eyeball exactly that. 894 tests. **User
    visual pass owed.**
  - *S26 Shape tool + S27 Line gizmo (2026-06-24).* Select-to-edit vector shapes, resize-vs-transform
    ("Scale stroke") handles, the shape-designer popover (on-diagram handles for rect/polygon/star +
    ellipse arcs), Solid/Hollow/Outlined line modes + a dedicated Line gizmo, and a Krita-style ScrubSlider.
  - *S29 Type tool — FULL (2026-06-25 → 07-03), user-verified, rounds CLOSED.* Text model + HarfBuzz
    shaping + emoji + CPU render (-a); on-canvas editing w/ GPU caret/selection (-b); Type panel +
    selection-style + font picker w/ in-face previews (-c); then hyphenation, spell-check (squiggles +
    suggestions), vertical writing-mode, variable-font axes, OpenType features, metric/optical kerning,
    and a Settings→Text emoji-font picker (R4–R5). 571 tests. `docs/type-tool.md`.
  - *S30-c/-d 3D type (2026-07-03).* Extrude engine (math kit, earcut, watertight mesh, 4 bevel profiles),
    a CPU z-buffer/Blinn-Phong lane AND a GPU-parity Vulkan compute-raster lane rendering into the text
    cache, and the "3D…" popup (live viewport, trackball, XYZ rings, depth/bevel handles, light sphere).
    3 feedback rounds: chrome + canvas reflections ("Sides only"), GPU readback perf fix, mutually-exclusive
    popups, presets + intensity, 3D-faithful editing chrome, back-face selection. ~604 tests. **Final user
    eyeball ("round 4") still owed** — the gizmo itself is user-praised.
  - *S16-f canvas expansion + crop-rotate (2026-07-02).* Crop beyond the canvas (unclamped rect + snap band
    + green expansion visuals), a **working** Inpaint fill for the ring (async), and
    crop-rotate (the Inpaint fill entry greys out while rotated). User visual pass owed.
  - *Smart Resize / Smart Recompose — S16-r (2026-07-01 → 02).* Content-aware retargeting: keep-region
    chips (Ctrl-drag), rigid placement solver, recompose pipeline + inpaint background heal, async job w/
    review/nudge/apply, band blend, credits sheet. `docs/smart-resize-research.md`.
  - *Inpaint quality + perf pass + Resynthesizer backend (2026-07-02).* Frame-edge/ghost/treeline fixes +
    multigrid blend + de-quantized offsets (36MP fill 21→~13s), plus a second clean-room Resynthesizer
    (Harrison) engine, wave-parallel (102→19.7s). GPU declined by user. User-verified.
  - *Annoyances settings category (2026-06-30).* Shipped with "Cheesy motivational one-liners"; only the
    S18-d unsaved-title toggle still waits on Save. UI strings standardized on US "color".
  - *Layer Effects + Texture Generator SCOPED (2026-07-03, docs only).* Design forks locked;
    **NOT built** — see the `mosaic-layer-effects` memory.
  - *S48 `.mosaic` native format SCOPED (2026-07-07, docs only).* Full spec `docs/mosaic-native-format.md`
    (supersedes the old ZIP-container sketch — §3.16) + narrative research write-up
    `docs/mosaic-native-format-research.md`, both synthesizing a standalone empirical research project
    (container corruption-resilience/speed/compression + undo-history persistence design, ~500 adversarial
    checks). **Round 10 follow-up same day**: 5 remaining
    decisions (tile size, fast-tier codec, root-slot sizing, retention-budget default, HIST `parent`
    field) had been answered by reasoning instead of testing — caught, then actually tested: **64px
    tiles** (not 256px — 13.8x cheaper small-edit autosave, negligible ratio cost), **LZ4 confirmed
    over zstd** for the fast tier (LZ4 measurably faster both directions), **128KB root slot +
    overflow-to-chunk** (designed/built/tested), retention budget **unlimited + a measured, cheap
    safety net**, `HIST.parent` **kept** (proven safe to drop, kept anyway for a free consistency
    check). **Review round same day (Round 11, harness-tested first)**: autosave moved OUT of the
    user's file into a **recovery-journal sidecar** (user hard rule: only explicit Save writes the
    user's path) w/ structural stale-journal rejection + explicit-link frames + honest salvage
    rules; structured 16-byte chunk keys (64-bit `LayerId`-safe); checkpoint copy-through;
    advisory per-document lock. **Round 12 same day**: File→Save = **commit-append** (atomic
    batch; full write only for Save As / first save / threshold compaction), autosave cadence
    grounded in a measured 64px write-volume number (SSD wear a non-issue). **NOT built** — see
    the `native-format-research` memory.
- **Recently completed (2026-06-20 → 23, all committed + pushed; this PLAN refresh = 2026-06-23):**
  - *S25 vector layer infrastructure — CPU path (2026-06-23).* Design in
    **`docs/vector-model.md`** (lineage: Loop–Blinn, Slug, generic stencil-then-cover from the OpenGL
    Red Book; NVIDIA's NV_path_rendering specifics deliberately avoided; recommended CPU "floor" =
    libtess2/earcut + analytic AA).
    Built `src/core/vector/` (geometry/paint/object value model; `flatten()→Contours` seam +
    `samplePathAt`; hit-test; scanline rasterizer w/ AA, solid + gradient fill, full stroke
    caps/joins/dashes; **float-native** `rasterizeObjectF`), the `VectorLayer` one-object payload,
    `SetVectorObjectCommand`, and compositor rendering of vector layers at target resolution.
    22 vector test cases; full suite 326/326 green. GPU-resident renderer + stroke Inside/Outside
    alignment deferred (S25 fork — see Next session).
  - *Fill dialog (S39) follow-ups A–F + Wayland/picker fixes (2026-06-22).* **(A)** the `FillDialog`
    now hosts its own `DropdownPopup`/`ContextMenu`, so its Contents/Mode combos are themed (with the
    family dividers); **(B)** shared `DropdownPopup` is fixed-size (resize guard) + scrolls internally
    (mouse-wheel + a themed pill grab) when taller than the window; **(C)** the opacity-slider preview
    is frame-coalesced (no per-tick `compositeRegion`); **(D)** a new **"Color…"** content opens a
    compact **`ColorFlyout`** speech-bubble (a child sub-window that reuses the picker's three surfaces
    — Field / HSL-wheel / SV-wheel, extracted to `color_surfaces.hpp` — plus the shared `ui::HexField`
    and a live swatch); **(E)** Protect-alpha tooltip; **(F)** the in-dialog inpaint Preview drives the
    status-bar progress bar + a pane note (in-dialog Esc cancels). **Wayland round:** the flyout/picker
    sub-windows must use the `(x,y,w,h)` ctor + be built before the parent is shown (else Wayland makes
    them stray top-levels); the comic-book triangle is an AA coverage patch (`drawBubbleTriangleLeft`
    in theme.cpp). The **original colour picker** gained the same bubble pointing at the swatch with
    balanced left/bottom margins — via `Fl_Window::shape()` (true transparency over the canvas) on
    **X11/Xwayland only**; **native Wayland drops the triangle** (shape can't cut it there) and shows a
    plain panel with the same balanced margins. (User-verified iteratively 2026-06-22. The native-Wayland
    half of that was **our bug, fixed in S58-i** — every platform cuts the triangle now.)
  - *Inpaint engine perf + quality + S39-b async (2026-06-20).* Hours → ~16s full-res (all three
    stages bounded + multithreaded, deterministic/no-RNG); blob/seam/speckle fixes; backends reorg
    `src/core/inpaint/backends/{he_sun,pde,script}/`; engine runs **off the UI thread** with a
    status-bar progress bar + cancel + throttled live preview, canvas stays navigable.
  - *Settings → Inpainting category (`fdba4b7`) + full-chrome disabled state during a run (`219a020`);
    engine ~2× speed pass (`c730865` — exact-NNF early-out + graph-cut convergence).*
  - *S60-a interactive-perf pass — 6 commits `e2ed52d`→`f3b0d8f` (2026-06-21).* `compositeRegion()`
    ROI primitive + dirty-region recomposite for live brush/inpaint + partial GPU upload; brush-size
    slider no longer full-recomposites + region patches frame-coalesced; region-scoped pixel commands +
    scoped undo/redo; brush engine bounded to the stroke working-rect (no doc-sized buffers; tagged
    S60-c). Pushed + user-verified visually fine.
  - *GPU-resident Move/Resize/Rotate drag (`85e3b94`+`6869206`+`59b280e`).* Per-frame GPU composite of
    below + dragged(transformed), CPU `DragCompositeCache` fallback; `59b280e` fixed the activeDrag
    ordering bug (fast path never engaged → during-drag lag), the 30s anisotropic-shrink freeze
    (chooseAutoFilter bucketed by max-axis), and the ~520 ms→~55 ms start hitch. User-verified + pushed.
  - *S39 inpaint asks (`43c21d7`+`8715cc5`).* Adaptive small-selection working region
    (`Params.adaptiveSmallRegion`, "Low-effort on small selection" toggle) + backend-agnostic
    sample-area preview (`IInpaintBackend::analysedRegion()`; "Show sampled area" toggle, off by
    default). ⚠ Small-selection speed gain negligible — the graph-cut K dominates (see the optional win).
- **Earlier — transient bug/feature pass (2026-06-15; all done, kept for reference).** DONE: the **3 settings/textbox
  fixes** — themed text-field right-click menu (`ui::ContextMenu`) + whole-value Ctrl+C, and the
  user-picked CMYK profile's embedded name in Settings (see the two §12 "DONE 2026-06-15" entries; a
  follow-up pass fixed the Ctrl+C focus root-cause, the right-drag-selects bug, and folded in the
  colour-picker fields — only the new-doc dialog menu stays deferred). **Settings categories built out:**
  the **Tools** category now uses **horizontal sub-tabs** (`SubTabBar` + per-tool sub-panes, cohesive with
  the app's Layers|History tabs — the user's IA call, so a long scroll never buries settings). DONE in Tools:
  **S16-p** (post-apply switch), **S16-q** (crop initial-framing — three `OptionCard` diagram cards incl. the
  **Inset** 15% option), and **S15-e** (multi-selection edits — diagram cards + Disabled/All/Active behaviour;
  "All" works while the Move tool holds the selection, persistent panel-owned selection is the flagged
  follow-up). Also DONE: a later **2026-06-16** sweep unified the multi-selection visuals (the right-side
  layer-row **dot** is the sole indicator — accent=active/edited, grey=others, all-accent in "All" mode —
  matching the S15-e cards), made **"All selected layers" editable** (mixed blend label + average opacity
  in the strip, with the blend flyout **dotting every mode present** in the selection), and added the
  **"Smooth freehand lasso"** toggle (Catmull-Rom of both preview + commit) under a new Tools/**Lasso**
  sub-tab. The transient pass is thus **cleared** — **S16-n and S16-o are now DONE** (overflow for the
  options bar + the left toolbar). **S19-c is now DONE** (2026-06-16, two commits): crisp
  nearest-neighbour pixels above 100 % zoom (hardcoded on the zoom ratio, not a toggle — the
  industry rule: nearest when magnifying, bilinear when minifying), a View > Show Pixel Grid toggle
  (default on, hairlines auto-fading in only at high zoom), and the transparency checkerboard moved
  from the doc-space CPU bake into a screen-space present-shader effect (constant size under
  zoom/rotation; the cursor readout now reports real pixel values). A **2026-06-17 polish pass** then
  landed a run of transform/selection refinements (all on `main`): **marquee/lasso ants are now
  pixel-aligned** (the present pass NEAREST-samples the mask, so rectangle corners are sharp and AA
  ellipse/lasso ants ride the crisp pixel staircase — Photoshop convention; the parked poly-lasso
  high-zoom note is thus resolved), the **pixel-grid onset raised to ~1000 %**, a **checked-menu-toggle
  accent dot** (the themed pop-up never drew FL_MENU_TOGGLE state), **whole-pixel Move translation**
  (a raster move is lossless — content stays crisp + the box rides the grid; sub-pixel placement is
  reserved for a future **Free Transform** tool), the **crop box pixel-aligned** (staged rect →
  `snapCropRect`), a **custom rotate cursor** for the Move tool's corner rotate band, and a
  **Move-tool transform HUD** (bottom-right: top-left position + W×H while moving/scaling, degrees
  while rotating; reuses the crop HUD tile). A **follow-up cursor/HUD pass (commit `3840c2d`, all
  visual-confirmed by the user)** then reworked the rotate cursor to apple's `left_side` double-arrow
  art — **recoloured** (blue/green source → two-tone, theme-aware) and **vector-rotated** (the turn
  angle is baked into the SVG so nanosvg renders it crisp in one AA pass, fixing the earlier
  blurry-bitmap-spin; angle is formatted locale-independently so a comma-locale can't stall the
  rotation), hotspot-centred, `kRotateSize = 24`, rasterised at the true `m_contentScale` (fractional
  HiDPI); the arrow tracks the box-centre tangent while dragging and sweeps the corner's 90° wedge on
  hover. **Double-clicking the rotate band now resets the selection's rotation to exactly 0.00°** (one
  undo step; flashes a "0.00°" HUD) — this **supersedes** the dropped "Shift-snaps-to-absolute-0" idea.
  Snaps: **Move-rotate Shift-snap stays 5°**; **poly-lasso Shift-snap is now 15°** (was 5°). HUD polish:
  a `·` separator between position and size, a **2-decimal degree** readout, and a **flash fix** (a
  click-to-select no longer flashes the HUD, gated on an actual Move gesture). *(S16-i marquee mask
  move + arrow-nudge remains open — see §10; S16-f canvas expansion shipped 2026-07-02, S16-g
  panel/History polish 2026-07-09.)*
  **Annoyances stays BLOCKED:** S18-d (unsaved-title) needs a Save to clear the command-stack dirty
  marker, and **Save (S18-b) isn't implemented** — so it waits until S18-b lands.
- **Transform Anti-aliasing — DONE 2026-06-17 (headless-verified; user visual pass PASSED 2026-06-17).** The
  greenlit feature + ALL its queued "next-session" kernels in one pass. **Root cause confirmed:**
  rotation is already non-destructive (the compositor samples each layer's SOURCE through its
  transform every composite); the jaggies were just NEAREST resampling at any non-90° angle — so the
  fix is **AA resampling**, not a 0° cache or angle-snapping. **Shipped:** a `render::ResampleFilter`
  enum + a pure, unit-tested `chooseAutoFilter(transform, liveDrag)` + a shared **premultiplied-alpha,
  footprint-widening** kernel sampler wired into `sampleTransformed` + `rasteriseLayerInto` (the
  integer-translate/identity fast path kept — whole-pixel Move stays lossless for every filter).
  **Kernels:** Nearest, Bilinear, **one `cubicKernel(x,B,C)`** → Bicubic (Catmull-Rom) + Mitchell +
  B-spline, **Lanczos 2/3**, **Area** (box, footprint-scaled → proper minify low-pass), Gaussian, and
  brute-force **Supersample** (NxN, adaptive). **Auto buckets:** lossless→Nearest, live-drag→Bilinear
  (cheap per-frame), committed enlarge/rotate→**Lanczos3**, committed minify→Area. Lives in the **Move
  tool options bar** ("Anti-aliasing" `Choice`, default Auto) → `CompositeOptions{resampleFilter,
  liveDrag}` (`liveDrag` from `VulkanCanvas::transformGestureActive()`); the **drag cache** threads the
  same options so its replay stays byte-identical to the full composite. CPU-only (no shader work).
  **Tests:** analytic invariants instead of brittle binary goldens — chooseAutoFilter buckets,
  cubicKernel family, **DC-preservation** under rotation for every kernel, **premultiplied no-bleed**,
  **Area box-average**, and **byte-identity of a whole-pixel translate across all filters**; build +
  ctest (243 cases) + ASan + `--gui-frames` Vulkan-clean. The next-up filter curation in §"NEXT-SESSION
  FILTERS" is thus implemented (remaining ideas: windowed-Lanczos minify, expose Area/SSAA promotions).
- **Roadmap change — ML inpainting DROPPED; S40 is now Lua scripting infrastructure (user 2026-06-17).**
  A built-in opt-in local ML inpainting model (the old S40: ONNX Runtime + LaMa weights) is **cut** —
  the inference dependency, model licensing, packaging, pulling arbitrary weights, and ML memory
  footprint are not worth it for the project. **Instead S40 = Scripting infrastructure (Lua via sol2)**:
  a sane, documented API over the command system, **with an example script that hooks the inpainting
  engine**, so the (inevitable) users who want ML inpainting can wire their own backend. **Regular
  *classical/algorithmic* inpainting STAYS** (S37-a/-b/-c engine: He & Sun default + Telea/NS; S38 Heal; S39
  Inpaint brush + Edit→Fill→Inpaint). Scripting was already a §11 "recommended" item; it's now scheduled.
  Done across §1, §3.11, §3.15, §6, §7, §9 (S40), §10, §11, §13.
- **Earlier:** *Inpaint engine perf + quality + S39-b async (2026-06-20).* The He & Sun engine
  was unusably slow (hours, UI-frozen) and blobbed on large selections. Now: **all three stages bounded
  & multithreaded** (offset-stats deterministic-decimation `nnfMaxPatches`, graph-cut node-cap two-scale +
  efficient Dinic, red-black SOR Poisson) → **full-res ~16s**; **blob fix** (antisymmetric Poisson
  guidance + project-to-[0,1]);
  `src/core/inpaint/backends/{he_sun,pde,script}/` reorg + README; **S39-b async**: engine on a worker
  thread, status-bar progress bar + cancel X + throttled live preview, canvas stays navigable (pan/zoom/
  rotate) + padlock reticle via `setInpaintBusy`, cancel token aborts long stages promptly. Suite 280
  green. **The three follow-ups flagged here all landed since:** Settings → **Inpainting category**
  (`fdba4b7` — Engine combobox + per-backend self-description + Backend Settings/Defaults), the **full
  per-control disabled-state pass** during a run (`219a020`), and a **quality/speed pass** (`2faa04c`
  speck/seam fixes + `c730865` ~2× engine). *(Future idea, not scheduled: the candidate paper
  `~/Desktop/2403.14292v1.pdf` would be its own session — treat skeptically.)*
- *S39-a **Inpaint brush** (2026-06-19).* New `ToolId::InpaintBrush` (shortcut J,
  placeholder bandaid icon) reusing the S19-a stroke machinery: the stroke paints a translucent **red
  ~35 % overlay** (the brush engine's coverage = the mask); on release the brushed region becomes a
  hole `Selection` fed to `InpaintEngine::run` (offset-stats default) and landed as one
  `SetLayerPixelsCommand`. Engine owned by `MainWindow`; synchronous on release. **S39-b
  (Edit→Fill→Inpaint) + async/progress deferred.** Tests in `test_brush_engine.cpp`; suite 279 green;
  `--gui-frames` + ASan clean. Earlier same day: *S19-a brush engine **base** + follow-up fixes (reticle
  → lasso luminance-key, locked-layer padlock + punch-out, default Background unlocked) + S18-b **Open**.*
  A headless CPU stamping engine `src/core/brush/` (`BrushEngine`): spacing-walked dabs, flow + a
  **per-stroke opacity cap** (Photoshop model), smoothstep hardness, active-foreground colour, a
  **pressure/tilt-ready dynamics API** (mouse pressure = 1; the S19-b hook). Wired into `VulkanCanvas`
  (press/drag/release → live recomposited preview → ONE `SetLayerPixelsCommand` per stroke). The
  **GPU reticle** rides the lasso overlay SSBO (`canvas_present.comp`); **reworked to the lasso's
  luminance-keyed monochrome** + a **padlock glyph/status hint on locked layers** (the Brush respects
  `locked()`). **File→Open** decodes PNG/JPEG (`io::loadImage`, libpng+turbojpeg) into a single
  unlocked raster layer. Big-doc lag is the S60 dirty-tile job (measured, deferred). `tests`:
  `test_brush_engine` + `test_io`; suite 277 green; `--gui-frames` clean. *Deferred:* brush presets +
  Settings panel + GPU stamping; Save/Export; the layer lock-glyph/unlock UI (PLAN §12) + the
  default-Background-lock decision. Earlier: *Transform Anti-aliasing (above). S19-c + the 2026-06-17
  transform/selection polish pass (crisp pixels, pixel
  grid, screen-space checker; pixel-aligned marquee ants + crop box; whole-pixel Move; custom rotate
  cursor; rotate/poly-lasso Shift snaps; Move transform HUD; menu-toggle dot), then the cursor/HUD
  follow-up pass (commit `3840c2d`: recoloured + vector-rotated `left_side` rotate cursor, double-click
  rotate→0.00° reset, poly-lasso 15°, Move-HUD separator/flash/2dp — all visual-confirmed).* See the §2
  status narrative above for the detail. Earlier: *S15-d — multi-select blend/opacity gating (2026-06-14,
  ad-hoc user request, S15-c follow-up).* The Layers panel learns the Move-tool multi-selection
  (highlights all selected rows) and **disables blend/opacity while >1 is selected**, the blend combo showing the
  mixed state ("Normal, Multiply") — the safe default; "apply to all" / "active only" become a future
  setting (S15-e, naming = behaviour not brand). Also queued: **S16-q** (crop initial-framing as a
  setting). Headless-verified (build + `ctest` 100% + `--gui-frames` Vulkan-clean); **user's visual
  pass PASSED 2026-06-15.** *(Prior: S15-c — shift-click multi-select move.)*
  **(1) Crop-box staircase — the REAL fix.** Round 2 had claimed it resolved; the user's 2026-06-14
  screenshots proved otherwise. Diagnosed from a perpendicular luminance profile across the rotated
  edge: the outline's `smoothstep(1.1, 0.4)` core held a **full-white plateau** under ~0.4px, so on a
  shallow-angle edge the white core snapped between 1 and 2px and the dark halo went near-hard — a
  classic single-sample staircase. Replaced (`transformHandles`, shared by crop + Move) with a **linear
  1px coverage ramp** (`clamp(1.1 - edge, 0, 1)` core + a softer `clamp((1.6-edge)/1.4+0.5,0,1)*0.8`
  halo); a compute shader has no `fwidth()`, but `segDist` is already in screen px so a fixed-px ramp is
  correct. Verified smooth offline at 0°/9.6°/33° (numpy repro) with no axis-aligned blur regression;
  the rule-of-thirds guides got the same treatment. **(2) Dial ticks.** The 15° ticks were
  constant-angular-width wedges in a radial annulus → diagonal ticks elongated into tapered "needles"
  while axis ticks stayed short stubs (user). Now real **`segDist` radial segments** (uniform
  length/width/AA at every angle); `segDist` moved above `overlay`. The floated text-outline idea was
  dropped (user: just an experiment, the dial needs no redesign). **(3) Apply/Cancel restyle**
  (user-chosen via consult): the cheap 1px green/red outlines are gone — **Apply = a solid accent-blue
  filled "primary"** (`FilledButton`, luminance-picked label, hover-brighten), **Cancel = a quiet
  neutral `FlatButton`** with red dropped (Cancel isn't destructive — red is reserved for genuinely
  destructive actions; the crop Cancel option flipped `Destructive → None`). `ToolAccent::Destructive`
  is kept as a solid danger-red fill for future real destructive buttons.
  **Follow-up tunes (same day):** *(4th pass)* widening the core to dodge the barber pole made the box
  read **blurry when straight** — single-sample coverage of a sub-pixel-thin line forces a trade between
  crisp-straight and pole-when-rotated, and the dark halo only made the wobble visible (the "is colour
  the culprit?" question). *(5th pass — the real fix)* **supersample the outline** (3×3 sub-samples →
  true area coverage, only near the line; `outlineSample` helper), so a **thin crisp ~1px core** (back
  to `clamp(1.0 - edge)`, gentle 0.55 halo) is also clean at **any** angle — offline-verified crisp at 0°
  + pole-free at 9.6°/33°. Guides returned to a thin ~1px core. Dial **ticks** thickened to ~1.3px at
  full ink (`clamp(1.5 - tickD, 0, 1)`). The **dial-readout text outline** (the earlier "experiment")
  was implemented for the user to evaluate: a dark ~1px ring (CPU-dilate the text coverage, composite
  the ring then white text on top) on the dial's no-pill tile; the HUD keeps its pill. **(6th pass —
  SETTLED, the real real fix)** the user (and the literature) were right: the white-on-black **two-tone
  itself** was the culprit — even perfectly AA'd it's a fussy frozen barber pole. Dropped it. **The
  crop/transform box is now a single thin ACCENT-blue line** (`kAccent`, matching the dial needle; a hue
  reads on light AND dark content where white/black each vanish on half — confirmed by an offline split
  light/dark render of white vs accent vs black), still supersampled for cleanliness. **Guides = a single
  thin grey line** (no two-tone). Dial **text outline: kept** (user approved). How the pros do it,
  recorded: crop tools lean on the shield + a single-colour line; selections use animated black/white
  *dashes* (marching ants — the only "right" two-tone); vector/UI tools use an accent line. **(7th pass —
  SETTLED)** decoupled the box colour from the theme accent (user: a garish user accent would wreck it
  long-term) → a **FIXED conventional "selection blue" `kBoxColor` #2F80ED** (the colour everyone uses
  for selection/bounding boxes), independent of the dial needle's `kAccent`. Box **widened to ~2px**
  (1px read thin; `clamp(1.5 - edge)`). **Guides now supersampled too** (3×3 via a `guideDist` helper, an
  early-out near a guide) so the thin line has no along-the-line ripple at an angle. **(8th pass — box
  SETTLED + APPROVED)** the user called the box **PERFECT**. **Guides (SETTLED after a long detour):**
  tried flat grey (too faint on light), a darker box-blue (muddy, competed with the box), then white + a
  soft dark shadow — but the user spotted that the shadow is itself a dark tone, so white-core-over-shadow
  is the **same two-tone → it poles faintly at shallow angles** (5°, 85°, 95°). **Key insight (recorded):
  any thin TWO-tone line poles a little at shallow angles — it's inherent to true area coverage, not a
  tuning miss; the box is pole-free ONLY because it's single-colour (its ripple reads as a faint intensity
  wobble, not a black/white alternation). Supersampling can't remove it.** A content-adaptive/inverted
  guide colour was considered and rejected (breaks on white↔black transitions, looks weird, AA infighting).
  **Final: a thin SINGLE-colour WHITE guide, no shadow** — the Photoshop/Lightroom convention (they omit
  the shadow for exactly this reason). Pole-free at every angle like the box; the accepted trade is that
  white is subtle on light content (the grid is a quiet compositional aid; the box is the boundary).
  Supersampled (3×3) for clean AA. **The crop overlay is DONE.** *(Possible later nicety: thread the live theme accent into the dial needle only; the box
  stays the fixed selection blue by design.)*
- **Earlier:** *S16-m rounds 1–2 (2026-06-12/13, 6 commits).* Canvas-edge AA, crop-shield AA (1st +
  2nd pass, ~3px feather), custom-ratio colon nudge, **overlay-text rework** (real UI font via
  `Fl_Image_Surface` into a present-pass RGBA tile; the 5×7 bitmap font deleted), HUD park-at-bottom,
  bigger dial font, double-tap-R "Reset" flash. (Round 2's "staircase resolved" claim was premature —
  see round 3 above. Full detail in §10 S16-m.)
- **Earlier:** *S16-h/j/l/k — Crop feedback round (S16-e follow-ups) (2026-06-13, four
  commits).* The user picked "crop bug-fixes first" over S16-f/S16-i; build (incl. glslc) + `ctest` +
  `--gui-frames` green per commit (interactive/visual verification is the user's — see the
  verification-division note). **S16-h (move jitter):** `snapCropRect` rounded the two edges
  independently (`lround(x)` vs `lround(x+w)`), so a pure translate oscillated the snapped W/H ±1px;
  now rounds **origin and size separately** (`w=lround(r.w)`) → translation-invariant size. One fix
  covers the crop HUD, the status-bar readout AND apply; audited the Move tool (continuous float, no
  integer readout) + status bar (already direct `lround(r.w/r.h)`) — clean. New translation-sweep
  test; snap goldens unchanged. **S16-j (rotation):** `hc01/hc23` are screen px, so a rotated view
  spins the quad — the rule-of-thirds guides now draw **parametric between opposing edges**
  (`segDist`, riding + rotating with the rect, per the recorded decision) and the size HUD anchors
  **below the quad's screen-lowest corner, centred on the centroid X** (was the doc-space bottom
  edge: left edge at 90°, overlap at 180°). **S16-l (custom-ratio bar):** new `ToolOption::joinPrev`
  binds a control tightly to its predecessor and renders its label as a **centred enlarged
  SEPARATOR** (`W : H` is one tight group; empty labels take no caption space, evening the margins);
  a `NumberInput` (`Fl_Float_Input` subclass) accepts a typed `,` and parse/format are now
  **locale-independent** (`from_chars`/`to_chars`) — fixing comma-locale decimal entry under
  `setlocale(LC_ALL,"")`; applies to every Number control. **S16-k (HUD glyphs):** the HUD lowercase
  set (p,x,i,n,c,m) redrawn to share one **x-height (rows 1-5) + baseline (row 5)** with 'p's
  descender extending to row 6; punctuation given a **tighter per-glyph advance** (`hudAdv`, dots in
  cols 1-2) so "7.0" nestles the dot; the HUD **hides** when the on-screen crop box's smaller
  dimension < ~56 logical px (zoomed-out dwarfing). No new translatable strings.
- **Earlier:** *S16-e — Crop Custom aspect ratio + size HUD (2026-06-13, two commits).* Build
  (incl. glslc) + `ctest` + `--gui-frames` green throughout (interactive/visual verification is the
  user's — see the verification-division note). **(1) Custom ratio:** a **"Custom"** entry
  (`kCropRatioCustom`) in the Ratio combo reveals two `[N] : [N]` **Number** fields
  (`ratioW`/`ratioH`; floats, min 0.01, no practical max); aspect = `customCropRatio(w,h,swap)`
  (pure + unit-tested) through the existing `conformCropRect` path. The fields live in the option
  set but start non-primary (hidden); `MainWindow::refreshCropCustomFields` flips `primary` on the
  Free/preset↔Custom transition and triggers a **deferred** `Fl::add_timeout(0,…)` bar rebuild —
  never synchronous inside the changed control's own callback (that `clear()`s and deletes the live
  widget: use-after-free). `ToolOptionsBar::syncValues()` now skips the **focused** control so a
  per-keystroke `FL_WHEN_CHANGED` re-sync can't eat a half-typed decimal (`"1." → "1"`).
  **(2) Size HUD** (user's pick: a centred pill below the rect, one line): `W × H px · w × h in/cm`,
  composited in the **present compute shader** (FLTK can't paint over the Vulkan surface — the
  dial's path). The 5×7 bitmap font gained `× . p x i n c m ·` + a blank glyph; `drawHud` builds the
  glyph string per pixel inside a cheap reject band under the rect. Unit from
  `common::resolveUnits(Settings::units)` (resolved in `main`, carried `RunOptions::units` →
  `MainWindow::m_metric` → `CropToolHost::metricUnits`) + the doc DPI. **Stays at the 128 B push
  budget** by riding the rotation-dial lanes (`overlayCenter`=W,H px, `overlay.y`=DPI,
  `overlay.z`=metric, `overlay.w`=HUD-active), free whenever the dial is off — so the HUD is simply
  suppressed while the view rotates (dial wins if both set; `overlay.w`=0 when neither is active, so
  stale degrees can't ghost a HUD). *(Open → S16-g style: the HUD font is fixed `px=2` like the dial,
  so it reads small on HiDPI.)* **Verified:** build + `ctest` (new `customCropRatio` cases) +
  `--gui-frames` clean per commit; pot regenerated.
- **Earlier (condensed — full detail in §10 / git):** **S16-c** (tooltips at construction + every
  options-bar control; crop labels "Swap orientation"/"Delete Cropped Pixels", ratio-conform on re-entry
  + staged rect persists across tool switches, "Guides" toggle; **unmasked-group local-buffer extent
  fix** — drill-in + move no longer clips a child [`renderLayer`/`compositeChildren` thread a `pre`
  transform, content-sized buffer + integer offset, byte-exact for identity groups/root]; History
  relative-time column; `common::Settings.units` auto|metric|imperial);
  **S16-b** (History panel — `CommandStack` history view [size/position/nameAt/jumpTo, one batched
  notification, `setOnChange` observer] + `ui::HistoryPanel` right-dock tab: a real Layers|History strip,
  "Original" row + chronological entries, position highlight + muted redo tail, click-to-jump,
  deletion-free same-count refresh);
  **S16** (Crop tool — `ui::CropGesture` draw/move/resize + ratio presets/Swap + doc clamp; present-pass
  shield/thirds/handles via the `ants.z` mode lane; ONE "Crop" CompositeCommand [`render::buildCropCommand`:
  ResizeCanvas + group-aware rebase push-down + optional Delete-Cropped bake + `Selection::cropped`];
  Esc/Enter/dbl-click);
  **S15** + **S15-b** (Move/transform — Affinity-style Move [V]: click-select [`core::topmostLayerAt`] +
  `ui::TransformGesture` [move/scale/rotate, layer-local scaling, Shift/Alt, Esc] + present-pass handles +
  ONE coalesced `SetTransformCommand`; three same-day fix passes [world transforms, live-test fixes, MAILBOX
  present]; **S15-b** drag latency — event-kicked frames + `render::DragCompositeCache` byte-exact replay,
  bench 72→29 ms/frame on a loaded box);
  **S14-b** (Clipboard — `core/clipboard.*` + `SetLayerPixelsCommand` + Edit menu [Ctrl+X/C/V,
  Ctrl+Shift+C] + Fl_Copy_Surface out / Fl::paste in + thumbnail boolean ops with the +/−/× chip);
  **S14** (Marquee select rect/ellipse/lasso — `Selection::ellipse`/`::polygon` AA + `ui::SelectionGesture`
  [press-mods=op, drag-mods=shape, poly close, Esc/Enter] + frame-coalesced live preview; one
  `SetSelectionCommand`/gesture);
  **S13-b** (Status bar — doc size/depth + physical size at ppi, cursor pos + colour-under-cursor chip,
  zoom/rotation, colour-space indicator, selection bounds; event-driven, pure formatting unit-tested);
  **S13** (Selection model + marching ants — core `Selection` mask + boolean ops + `SetSelectionCommand`;
  present-pass ants [R8 mask texture + screen-space edge + dash phase]; Shift-click-thumbnail → selection;
  Select All/Deselect/Inverse);
  **S12** (Colour picker + management — picker UI [model combo + SV field/hue strip + hex redesign; review
  rework: editable readouts, square field/wheel/triangle surfaces, settings-persisted default]; **lcms2** +
  Lab + CMYK + colour-space indicator + gamut warning/snap + swatches/recents; ICC `.icc` loading + vendored
  FOGRA39 CMYK default);
  **S11-e** (toolbar **flyout variant groups** — `ui::ToolSlot` + per-slot shown-variant in `ToolManager`;
  one button/slot, corner triangle for multi-variant, right-click/triangle opens `ui::ToolFlyout`; 16 tools
  / 12 slots);
  **S11-d** (active-colour **swatch** + flat-colour **picker** popover — `ui::ColorState` fg/bg holder; the
  two-diagonal-chip `ui::ColorSwatch` at the toolbar bottom [X swaps, D resets]; a reusable anchored
  `ui::Popover` **child sub-window** host + a flat `ui::ColorPicker` stub. **Popover hosting corrected**: a
  genuine child sub-window [four-arg ctor + built before `show()`] — no taskbar entry / centring /
  orphan-on-close).
- **Earlier:** **S11-c** (options-bar consolidation + UI hardening: the bar became the *primary* per-tool
  surface — generic **Properties-tab mirror cut**; `ToolOption::primary` hot-subset [Brush bar = Size +
  Opacity] + overflow drop + window `size_range()` floor + `ui::ToolGroup` toolbar dividers; fixed
  options-bar control-stretch-on-widen via a `ToolOptionsBar::resize()` override + native-Wayland canvas
  input via an empty subsurface input region; the **X11 resize black-flash** was deemed unfixable
  app-side → §12 backlog);
  **S11-b** (per-tool **option model** `ui::ToolOption` [Slider/Choice/Toggle] + full-width
  **options bar** under the menu rendering the active tool's options, each edit written back +
  `notifyOptionsChanged`; lockstep `syncValues()` infra);
  **S11-a** (tool framework `ui::ToolManager` + 12 tools with colorful runtime-rasterized
  SVG icons; always-square `ui::LeftToolbar` + tooltips + accent active state; plain-letter shortcuts
  via `MainWindow::handle`; **Move is an arrow cursor**, Affinity-style select/transform per S15);
  **S10-d** (layer **drag ghost** — a translucent chip [faded thumbnail + name, accent
  edge, drop shadow] rides the cursor while the dragged row shows a dashed "lifted" slot; press-time
  grab offset; no new commands, drop-back restores);
  **S10-c** (Layers list **recurses the tree** — nested indented rows + a group
  disclosure triangle / collapse via view-only `GroupLayer::expanded()`; per-layer **properties strip**
  = blend-mode `Dropdown` → `SetBlendModeCommand` + opacity `Slider` → coalesced `SetOpacityCommand`;
  **reparent drag** via `planDrop()`/`moveIndexFor()`/`isSelfOrDescendant()`; **Layer→Group Layers**
  = one `CompositeCommand`; shift-click thumbnail stubbed → S13);
  **S10-b** (deep `core::Document::duplicateLayer()` + drag-to-reorder via
  `MoveLayerCommand` + drag-onto-plus clone [green "+" target] + Layer→Duplicate);
  **S10-a** (Layers dock: the main window became menu + canvas (resizable) + a
  fixed-width right dock; `ui::LayerPanel`/`LayerRow` — a "Layers" header tab over a scrollable list,
  top of stack at top, each row a visibility eye + thumbnail [`layerThumbnail()`, pure + tested] +
  name + accent active highlight; bottom New/Delete toolbar; Layer→New/Delete + Edit→Undo/Redo wired;
  every edit via the command stack → `recomposite()`; rows read live state so only add/delete/undo
  rebuild the list — `src/ui/layer_panel.*`);
  **S9** (New-document dialog — File→New → themed modal: A-series / US-paper / pixel
  presets + a custom **px/mm/cm/in/pt** size at a chosen **ppi** + colour space + bit depth +
  background; preset↔fields sync, unit changes re-express the size keeping pixels fixed; the pure
  size + `buildDocument` logic is unit-tested; `setDocumentImage` gained a `fitView` arg —
  `src/ui/new_document_dialog.*`); S0–S4 (planning → theming); **S5** (settings + logging + i18n; nlohmann/json
  vendored); **S6** (document & layer model + undo/redo, split a/b — `docs/document-model.md`);
  **S7-a** (CPU reference compositor: `common::ImageF`, `render/blend.hpp`, `render::composite` tree
  walk with masks/clip/groups/adjustment-scoping + checkerboard, `--composite-demo` + golden);
  **S7-b** (GPU compute blend kernel `shaders/composite_blend.comp` mirroring `blend.hpp` via a
  pluggable `BlendFn`; **VMA** vendored v3.3.0; `render::GpuCompositor`; GPU==CPU per mode —
  `docs/compositor.md`); **S7-c** (canvas wiring — the composite shown on the live canvas);
  **S8** (interactive canvas viewport: `ui::CanvasView` pan/zoom/rotate via `Affine2D`; renderer
  presents through the `canvas_present.comp` compute pass that inverse-samples the document, so
  rotation works; wheel/Space/R input + View menu; S8-b rotation dial overlay — `docs/vulkan.md`).
- **Verified toolchain (host):** GCC 16.1.1, CMake 4.3.3, Ninja 1.13.2; FLTK **1.4.5**
  (hybrid X11+Wayland); `VK_LAYER_khronos_validation`, `glslc` present; GPU = AMD RX 6600 XT
  (RADV, Vulkan 1.4). Session is Wayland with XWayland available.
- **Standing decisions / gotchas:**
  - **Document model (S6):** the layer tree is the source of truth; UI/tools must edit **only
    via `doc.commands().push(...)`** so everything is undoable + scriptable. Group children are
    ordered **bottom(0)→top**. Layers are referenced by `LayerId`, never raw pointer, in commands/
    serialization. `MoveLayerCommand`'s `newIndex` is post-removal. `Document` is non-copyable/
    movable (the `CommandStack` holds a back-reference). Raster pixels are RGBA8 for now
    (→ float migration **S43-a**; tiled storage **S60-c**). See `docs/document-model.md`.
  - **Logging (S5):** log via `common::log::category("<module>")` (cache it in a TU-local
    `static`); never log on hot/pixel paths (use `std::expected`). CLI **result** output stays
    on stdout/stderr via iostreams — it is not logging.
  - **Settings (S5):** `common::Settings` holds **UI-free** serializable fields only (so
    `common` can own it); `ui` maps `theme`↔`ThemeMode`, `app` maps `logLevel`↔`log::Level`.
    nlohmann/json is a PRIVATE impl detail of `settings.cpp` — keep it out of headers.
  - **i18n (S5):** wrap user strings in `_()`; menu **paths** are wrapped whole for now
    (translators keep the `/`), per-segment refinement is S53/S54 — see `docs/i18n.md`. Re-run
    `cmake --build … --target pot` after adding strings.
  - **Theming (S4):** FLTK 1.4.5 has **no** subclassable `Fl_Scheme` (PLAN §3.5 corrected) —
    token palette + custom boxtypes, not a scheme subclass. Theme **mode** now persists in
    settings (S5) and resolves at startup; the in-app **picker UI** is still S51.
  - **Windowing (S3/S59-a, `docs/vulkan.md` + `docs/wayland.md`):** the Vulkan canvas is validated
    over **both** backends, and since **S59-a native Wayland is the DEFAULT**
    (`platform::preferWaylandBackendIfUnset()` pins `FLTK_BACKEND=wayland` when a Wayland session is
    present and the user has not chosen; `FLTK_BACKEND=x11` is the escape hatch, and a pure-Xorg
    session is untouched). The swapchain presents to a dedicated `wl_subsurface`
    (`platform::WaylandSubsurface`) instead of FLTK's own surface, sidestepping the
    `wp_linux_drm_syncobj_surface_v1` abort. **Gotcha (fixed post-S11-c):** that subsurface must be
    given an **empty `wl_surface` input region**, else its default *infinite* region swallows all
    pointer input over the canvas and FLTK never sees it (zoom/pan/rotate dead on native Wayland) — any
    future presentation-only overlay surface needs the same. Native Wayland is a **hard prerequisite for
    S43 HDR output** (XWayland/Xorg are SDR-only — §3.6); the flip did **not** wait for S43-c, it
    landed early in S59-a because the file picker, the resize black flash and the cursor work all
    argued for it independently.
  - **Compositor (S7, `docs/compositor.md`):** the composite is **derived** from the tree via
    `render::composite(doc, opts, backend)`. Groups composite **as a unit** (bottom→top), so group
    opacity/blend/mask and **adjustment scoping** (within-group vs global-downward at root) fall out
    of tree position. Blend math has **one definition** in `render/blend.hpp` (W3C spec), mirrored
    line-for-line by `shaders/composite_blend.comp` — **edit both together** (tests assert
    GPU==CPU per mode). Only the per-pixel blend runs on GPU (pluggable `BlendFn`); the rest is
    shared CPU code. **Compositing is in the document's encoded (gamma) space for now** —
    S12-b brings **picker-level** colour management only (lcms2 in the picker + the gamut warning);
    the compositor's **linear-light + ICC re-plumb is S43-b**, directly after the float-pixel
    migration (**S43-a**) — §3.6. Float buffer is `common::ImageF`; model pixels stay RGBA8
    (float migration → **S43-a**; tiled storage → **S60-c**). Leaf transforms use
    **nearest** sampling (quality/bilinear later; magic-layer resampling S50). **VMA** (vendored,
    MIT) backs device memory (`render/vma_impl.cpp`, warnings off). **Canvas:** `WindowRenderer`
    uploads the composite to a texture and `vkCmdBlitImage`-es it centered+aspect-fit
    (`fitCentered`) over the themed bg; the app shows a **placeholder document**
    (`ui::buildPlaceholderDocument`) until real open/new (S9 / S18-b / S50). **Deferred to S60-a (perf):** GPU
    residency (blendOver round-trips per layer) + tiled dirty regions.
- **Notes / gotchas for the next session (S17 — Magic wand, research-first):**
  - **Research note FIRST** (§0 rule): write/refresh `docs/research-selection.md` before any
    code — colour-distance metrics (which colour space: the doc's encoded space now, Lab via
    the S12-b lcms2 engine later?), tolerance semantics, contiguous (flood fill) vs global
    (whole-image match), anti-aliased edge coverage, feather. S18's select brush shares the
    note — scope it for both.
  - **Where it plugs in:** the wand is a SELECTION tool — register in the marquee/lasso slot
    family (S11-e flyout or its own slot), produce ONE `SetSelectionCommand` per click
    through the existing canvas → host commit path (`pushSelectionGesture`'s precedent), and
    respect the S14 modifier conventions (press-time mods = boolean op; the op-badge cursors
    are reusable via `ui::selectionCursor`). Tool options (tolerance slider,
    contiguous toggle, sample-layers choice) ride the S11-b options bar.
  - The flood fill reads the COMPOSITE ("all layers") or the active layer's pixels — the
    "Source" option precedent is the eyedropper stub's (Active Layer / All Layers). The
    composite is available as MainWindow's `m_lastComposite` (the cursor-readout source); a
    selection from it must use the raw (checkerboard-free) composite, not the displayed one
    — compute via `render::composite` with `checkerboard=false` like Copy Merged does.
  - **Stack-depth cap decision** may surface with the History panel now visible (an unbounded
    stack on huge pixel commands is memory-heavy — SetLayerPixels/SetSelection store full
    copies). If capping, evict from the bottom (oldest) and say so in the panel; document
    the choice in §3.
  - **History panel leftovers (S16-b done, 2026-06-12):** `CommandStack::setOnChange` holds
    ONE observer (MainWindow re-wires per document in `presentDocument`); a same-count
    `HistoryPanel::refresh` must stay deletion-free (a click-jump refreshes from inside the
    clicked row's `handle()` — see the comment there before "simplifying" it).
  - **Crop tool leftovers for later sessions (S16 done, 2026-06-12):** the compositor's
    canvas-sized group-LOCAL window clips content a transformed group shifts in from outside
    that window — pre-existing, now documented in `buildCropCommand` (push-down sidesteps it
    for unmasked groups; masked/singular groups still rebase as a unit). S60-a owns the real
    fix (offset/extent-aware group buffers). The present-pass push block is at EXACTLY 128
    bytes — any new overlay must multiplex an existing lane (ants.z is now a mode enum), not
    grow the block. View compensation after crop (keeping the kept content stationary on
    screen) was considered and skipped — the view keeps its pan/zoom, so the content shifts
    on screen by the crop origin once; revisit only if the user reports it as jarring.
  - **Crop default = full canvas — RESOLVED, keep it (user, 2026-06-14).** Selecting the Crop
    tool frames the whole canvas (handles on the edges, no dim shield). The user questioned this;
    confirmed it's the right default: it *is* the convention for our class (Photoshop / Affinity /
    Lightroom; GIMP's draw-first is the outlier) and the common crop-inward is then one gesture
    (grab a handle, pull) instead of two (draw, then adjust). An inset default was rejected
    (arbitrary margin; reads as "already cropped"). No code change. **Follow-up (same day):** making
    this default a **setting** (full canvas / inset / draw-to-begin) is on the roadmap as **S16-q** — the
    default stays full-canvas, the others become opt-in for power users.
  - **Popover hosting rules (learned the hard way; don't regress):** the popover MUST be a genuine child
    **sub-window**, not a top-level — else KDE/Wayland gives it a **taskbar entry**, **centres** it, and
    **orphans the app** on close. Two things make it a sub-window: (1) the **four-arg** `Fl_Double_Window
    (0,0,W,H)` base ctor (the two-arg `(W,H)` form forces a top-level); (2) **build it before the parent's
    `show()`** (a window added to an already-realized parent is promoted back to a top-level). The main
    window builds + owns it; the opener holds a non-owning pointer. No `xdg_popup` needed — works on X11 +
    XWayland + native Wayland identically.
  - **Fixed-layout FLTK gotchas (S11-a/c/d):** an `Fl_Group`'s default `resizable()` is *itself*, so it
    scales children — call `resizable(nullptr)` on fixed strips/columns. `Fl_Group::clear()` resets
    `resizable()` to the group (the options bar re-asserts via its `resize()` override). A **bottom-pinned**
    child (the swatch) needs its column's `resize()` to re-place it (see `LeftToolbar::resize`).
  - **Border-edge ownership (user feedback, 2026-06):** docked chrome must never draw a full
    rect border — adjacent elements double the shared hairline to 2 px. `ui::Panel` draws only
    its **owned** edges (`borderEdges()` bitmask); exactly one element owns each junction (the
    options bar owns top+bottom, the toolbar its right, the dock its left, the status bar its
    top; window-edge sides draw nothing). Floating surfaces (popovers) keep all four. See
    `docs/theming.md` § Border-edge ownership — apply the rule to every new docked surface.
  - Build/run: `cmake --build --preset linux-debug && ctest --preset linux-debug`; smoke
    `mosaic --gui-frames 8` (add `FLTK_BACKEND=wayland` for the native path).
    **The user verifies interactive UI quickly** — prefer a short build + a screenshot from them over
    elaborate offscreen harnesses. Offscreen `Fl_Image_Surface` works for a *non-window* widget, but a
    **double-buffered window must be shown first** (else it renders black) and showing a borderless
    popover with no realized main window segfaults — i.e. it only mirrors reality with a host window up.

---

## 3. Key Technical Decisions

Each decision records the choice, the reasoning, and (where it mattered) the research that
informed it. Sources are collected in **Appendix C**.

### 3.1 Language & compiler
- **C++20** baseline (concepts, ranges, `<bit>`, designated initializers, `consteval`).
  Opportunistically use C++23 (`std::expected`, `std::print`) guarded by feature tests.
- **GCC ≥ 13** is the primary, fully-supported compiler. **Clang ≥ 16** is a
  best-effort secondary (kept compiling, not release-gated).
- Exceptions: allowed, but hot paths (render loop, pixel kernels) use `std::expected` /
  error codes and never throw. RTTI: on (FLTK and a few helpers use it).

### 3.2 Build system — **CMake + Ninja + presets**
Plain Makefiles do not scale to this project's cross-compilation, per-platform dependency
sets, shader compilation, asset generation, and test matrix. We use:
- **CMake ≥ 3.24** with **`CMakePresets.json`** (configure/build/test/package presets):
  `linux-debug`, `linux-release`, `linux-asan`, `windows-x86_64`, `windows-arm64`,
  `macos-arm64`, `macos-x86_64`.
- **Ninja** generator. Out-of-source builds in `build/<preset>/`.
- **Toolchain files** in `cmake/toolchains/`: `mingw-w64.cmake` (Windows, arch selected with
  `-DMOSAIC_WIN_ARCH=x86_64|aarch64` — GCC for x86_64, llvm-mingw for aarch64) and
  `osxcross.cmake` (macOS, `-DMOSAIC_OSX_ARCH=arm64|x86_64`).
- Dependency discovery via `find_package` + `pkg-config` (system-first, §6). A
  `cmake/Dependencies.cmake` centralizes this and prints a clear summary of what was
  found, what is optional and disabled, and what the user must install.
- **Shaders** compiled GLSL→SPIR-V at build time (shaderc/`glslangValidator`) and embedded
  as byte arrays via a small generator (`cmake/EmbedShaders.cmake`).
- **App icon** rasterized from `assets/app_icon.svg` at build time (nanosvg, §3.13) into
  the platform icon formats; the SVG is the single source — swap the file to rebrand.

### 3.3 GUI toolkit — **FLTK 1.4** (with a documented fallback)
**Chosen: FLTK 1.4.** Rationale, informed by research:
- FLTK is lightweight, has a permissive **FLTK License** (LGPL-2 + static-linking
  exception → GPLv3-compatible), and builds cleanly cross-platform incl. MinGW.
- Custom widgets are first-class: you subclass `Fl_Widget`/`Fl_Group` and override
  `draw()`/`handle()`. Since we are drawing a custom pro UI anyway, this is a feature.
- Theming is fully in our own hands. (The "subclassable `Fl_Scheme` OOP system with
  Aqua/Fluent/Sweet host-mimicking schemes" this choice originally cited **does not exist**
  in FLTK 1.4.5 — corrected in S4.) We theme via a token palette + custom boxtypes on the
  flat `gtk+` base scheme (§3.5, `docs/theming.md`), which suits a custom pro UI anyway.
- **Vulkan integration** is feasible: FLTK windows are real native surfaces. We derive a
  `VulkanCanvas : public Fl_Window`, take the native handle after `show()` (`fl_xid` on
  X11, `fl_wl_surface`/Wayland, `HWND` on Windows, `NSView`/CAMetalLayer on macOS), and
  create a `VkSurfaceKHR` via the platform WSI extension. We suppress FLTK's own drawing
  on that window.

**Fallback (documented, not chosen):** if FLTK proves limiting for the volume of custom
widgets or for crisp HiDPI/IME/text, switch the UI layer to **Qt 6 Widgets** under
LGPLv3/GPLv3. The architecture (§4) isolates the toolkit behind the `ui/` and `platform/`
modules specifically so this swap stays contained. Qt is heavier and is the explicit
second choice per the brief.

### 3.4 Rendering — **Vulkan 1.2 baseline, 1.3 opportunistic**
- **Target Vulkan 1.2** as the portable baseline (covers old drivers and MoltenVK widely).
  Detect and opportunistically use 1.3 features (dynamic rendering, synchronization2,
  timeline semaphores, `maintenance4`) behind capability checks, with 1.2 fallbacks.
- **macOS via MoltenVK:** MoltenVK 1.3 (May 2025) provides Vulkan 1.3, 1.4 (Aug 2025)
  provides Vulkan 1.4 — so the macOS port is not blocked by API level, only by build
  tooling (osxcross) and testing access (backlog, §12).
- **Vulkan Memory Allocator (VMA)** (MIT, header-only, vendored) for device memory.
- **shaderc**/`glslangValidator` to compile shaders at build; SPIR-V embedded (§3.2).
- Validation layers enabled in debug builds; `--gpu-validation` flag; RenderDoc-friendly.
- A CPU fallback path for headless CI/golden tests where no GPU is available (§3.15).

### 3.5 Theming & the "native look" question
The brief wants Mosaic to "look like the system it's running on." Interpreted honestly:
**literal native widgets are neither practical across toolkits nor desirable for a pro
creative app** — Photoshop, Krita, Blender, and DaVinci all ship their own custom UI.
Mosaic therefore implements a **system-adaptive custom theme**:
- Detect and follow **light/dark preference, accent color, UI font, and DPI/scaling** from
  the host (KDE/GNOME/Cinnamon via XDG portals/`gsettings`/Qt platform theme hints;
  Windows via `UISettings`/registry incl. Mica/immersive dark mode; macOS appearance).
- Provide a small set of built-in themes (System-adaptive default, plus explicit Light/Dark
  and a neutral "Mosaic" theme) selectable in settings.
- **Theming mechanism (corrected in S4):** FLTK 1.4.5's `Fl_Scheme` is **not** a subclassable
  OOP scheme system (its constructor is "not yet implemented"; the Aqua/Fluent/Sweet schemes
  don't exist — only `base/gtk+/gleam/plastic/none`). So instead of a `MosaicScheme` subclass,
  the engine drives a **token-based palette** onto FLTK's global color map + **custom
  `FL_FREE_BOXTYPE` boxtypes** + tooltip styling, on a flat base scheme (`gtk+`). All custom
  widgets read the tokens. Full detail in `docs/theming.md`.

The result feels at home (matches dark/light + accent + DPI) without pretending to be the
OS toolkit. This is the documented stance; revisit only if the fallback to Qt is taken.

### 3.6 Color management & HDR
- **Little CMS (lcms2)**, MIT — ICC profiles, transforms, soft-proofing, gamut warnings.
- Internal compositing in **linear light** (scheduled: **S43-b**), working precision selectable:
  8-bit int, 16-bit int, **16-bit float (default)**, 32-bit float (storage lands in **S43-a**).
  Document color space + bit depth per document; show a **color-space indicator** in the status
  bar (**S13-b**).
- **HDR:** float working buffers; output transfer functions PQ/HLG/scRGB; Vulkan swapchain
  HDR color spaces (`VK_EXT_swapchain_colorspace`, `VK_EXT_hdr_metadata`). Show an **HDR
  indicator**, and **warn** when the display/compositor lacks HDR or has it disabled.
  - **Presentation-backend dependency (Linux):** true HDR *output* requires the **native
    Wayland** present path (compositor color management + the Vulkan HDR color spaces above).
    **XWayland and Xorg are SDR-only**, so the S3 default (X11/XWayland — §3.3,
    `docs/vulkan.md`) can never present HDR. The native-Wayland canvas (§12 backlog) is thus a
    **hard prerequisite for the HDR-output half of S43**. HDR *editing* (float pipeline,
    EXR/JXL/AVIF I/O, tone-mapping to an SDR window, the indicator/warning logic) is
    backend-independent and works in the meantime.
- **OpenColorIO** (BSD) is an optional later integration for film-style config-driven
  pipelines; lcms2 is the baseline.

### 3.7 Document & rendering model
The core is a **layer tree** rendered by a **Vulkan compositor**:
- Layer kinds: **Raster**, **Vector** (paths/shapes), **Text**, **Group**, **Adjustment/
  Filter** (non-destructive), and **Magic** (a.k.a. linked/smart source — keeps the
  original full-res data and a transform, resampling from source to minimize loss).
- Each layer has: transform, opacity, blend mode, visibility, optional **raster mask** and
  optional **vector mask**, clip-to-below flag.
- **Adjustment/Filter layers** affect layers **below them within their group**, or
  globally downward if not in a group (matches the brief). Implemented as the compositor
  applying the effect to the accumulated buffer at that tree position.
- **Non-destructive** = the tree is the source of truth; the composite is derived. Editing
  re-renders affected subtrees only (dirty-region tracking + tiled caches for big images).
- **Document *type* (Raster today; a Vector type is deferred to S30-b).** The current document is a
  Raster document: vector layers composite into its fixed pixel grid, so viewport zoom magnifies
  pixels. A future **Vector document type** keeps the canvas resolution-independent (re-render at
  view resolution) and exposes vector-only tooling. The geometry model (§2 of `docs/vector-model.md`)
  is already type-neutral; the doc-type is a canvas/UI policy resolved at S30-b (and persisted by the
  `.mosaic` format, §3.16).
- **Undo/redo** via a command stack; continuous ops (a brush stroke, a drag) coalesce into
  one command. The **same command objects power the headless op-runner** (§3.15), so every
  edit is scriptable and testable.
- Detailed spec: `docs/document-model.md` (written in S6).

### 3.8 Image I/O & formats
A pluggable I/O registry (`io/`) with one module per format, each declaring capabilities
(layers? alpha? HDR? bit depth? vector? metadata?). On export, a **capability diff** drives
the **loss-warning dialog**: "PNG has no layers → image will be flattened", "JPEG has no
alpha → transparency will be lost", "format is SDR → HDR will be tone-mapped", "vector text
will be rasterized", etc. Target ≈ GIMP-level coverage (§6 lists the libs):
PNG, JPEG, TIFF, BMP, GIF, TGA, PNM/PPM/PGM, ICO, WebP, **AVIF** (royalty-free, first
class), **OpenEXR**, **JPEG XL**, optional **HEIF** (system codec only, §7), DDS, plus
**OpenRaster (.ora)** for interop, **PSD/PSB** (§3.9), RAW (§3.10), and **`.mosaic`**.

### 3.9 PSD / PSB — **Molecular Matters `psd_sdk` (BSD-2)** + spec fill-in
- **`psd_sdk`** is BSD-2-Clause (GPL-compatible), C++, reads **and writes** PSD, and builds
  on Linux/Windows/macOS → vendored in `third_party/` as the primary engine.
- Gaps (PSB large-document specifics, exotic layer/effect data) are filled per Adobe's
  published spec; our own reader/writer code lives in `io/psd/`. `libpsd` exists but is
  **LGPL/copyleft** — only consulted as reference, not linked, to keep the dep set clean.
- Reference: Adobe Photoshop File Format spec (Appendix C).

### 3.10 RAW — **LibRaw (LGPL-2.1 option)**
- LibRaw is tri-licensed (LGPL-2.1 / CDDL-1.0 / commercial). We use the **LGPL-2.1**
  option, which is GPLv3-compatible. 1000+ cameras; extracts **EXIF/makernotes/lens** for
  the **camera-info panel** (exposure, aperture, ISO, focal length, etc.).
- Note: LibRaw dropped Foveon/Sigma (GPL dcraw origin) — documented limitation.

### 3.11 Inpainting — pluggable backends (ML left to user scripts)
**Engine lineage (settled by the S37-a research note — `docs/inpainting-research.md`, the source of
truth; this section's earlier "classical-only" assumptions are superseded):** the literature was
surveyed in depth — **Criminisi** exemplar-based inpainting, **Boykov/Veksler** graph cuts, **Poisson**
editing, the **He & Sun** method, **Telea (2004)** and basic **Bertalmío Navier–Stokes**. ⚠ **Standing
implementation constraints:** we never implement PatchMatch's propagation+random-search loop, and the
PDE backend stays on Telea / basic Navier–Stokes and takes no other scheme. Both are deliberate and
binding, not gaps waiting to be filled.
- **Pluggable backend engine.** One `InpaintEngine` + an `IInpaintBackend` interface (namespace
  `mosaic::core::inpaint`, over `common::ImageF` + `core::Selection`). The **default built-in backend is
  the He & Sun offset-statistics graph solver** (`OffsetStatisticsBackend`: translation-only; own KD-tree
  NNF — *not* PatchMatch; own α-expansion + max-flow; own Poisson blend), clean-room from the
  published papers — the higher-quality object-removal path. An **optional `PdeBackend`** ships
  **Telea + basic Navier–Stokes** as a fast small-hole/preview fallback.
- **No built-in ML model (decision, user 2026-06-17).** A bundled local ML inpainter (the
  old plan: ONNX Runtime + LaMa weights, settings toggle) is **cut** — the inference
  dependency, model licensing, packaging, downloading arbitrary weights, and the ML memory
  footprint are not worth it for Mosaic. **ML inpainting is instead a user-scripting
  concern:** the **Lua scripting API (S40)** exposes the inpainting engine as a **pluggable
  backend**, and S40 ships an **example script that registers a custom inpaint provider** so
  users who want a local model can wire their own (their dep, their weights, their choice).
- All inpaint entry points (Heal tool, Inpaint brush, Edit→Fill→Inpaint) call the same engine API;
  backends register at startup (built-ins) or script-load (the **`ScriptBackend`** shim, S40). The S40
  example registers a custom inpaint provider against this hook. **Research note written first
  (S37-a, ✅ done); engine + backends follow in S37-b/-c.**

### 3.12 Brush engine & imports
- Custom brush engine; **CPU stamping core** (GPU stamping stays behind the engine API — decision
  2026-06-19). Pressure/tilt via system tablet APIs (`docs/tablet.md`). Ships preset brushes.
- **Brush UI direction.** The options bar carries only the *hot* controls — a **brush preset chip**
  (thumbnail + name + dirty dot) + **Size** + **Flow/Opacity** + **Blend mode**. Clicking the chip
  opens the **brush editor**: a **modal** with a checkable option rail, a curve editor, and a
  **paintable live preview** that never commits to the document. Brush **presets live in a right-dock
  section shown only for the Brush tool** — the other brush-family tools (Inpaint, Heal, Clone,
  Smudge, Select brush) keep a plain circular tip. Controls worth mirroring: **Stabilizer**,
  **Symmetry / Mirror**, **Protect Alpha**. ⚠ **"Wet Edges" needs a rethink** — see `docs/brushes.md`
  §5 (fluid/wet-paint simulation is a technique family we do not build).
- **The reticle always traces the actual tip shape**, not a circle — no outline-style setting.
- **The Eraser is built with this rework** (today it is registered but inert). Its *size* follows the
  brush by default; its *preset* does not.
- **Imports are a first-class goal:** GIMP `.gbr`/`.gih`, Photoshop `.abr`, MyPaint `.myb`, Krita
  `.kpp` (a PNG whose `zTXt` chunk holds the preset XML) and `.bundle`. We map foreign paintop params
  to ours best-effort, and every imported preset carries a **fidelity badge**
  (Exact / Approximated / Substituted) plus the list of options we dropped.
- **We bundle a cleared default preset set** (`Krita_4_Default_Resources`, **CC-0**, attributed in
  `docs/credits.md`). *This reverses an earlier stance in this file that spoke of "Krita's GPL brush
  content": the content is CC-0/CC-BY, not GPL — and GPL content would have been compatible with
  GPLv3 Mosaic regardless.* `RGBA_brushes` is **not** shipped (its licence field is under-specified).
- **We never inspect, report or restrict what a user imports.** Our only liability surface is what we
  *redistribute*. Importing is neutral; publishing is not — a future "export a brush pack" or curated
  in-app browser would need the same LICENCE clearance the default set got.
- Full design + verified format reference in `docs/brushes.md`; input in `docs/tablet.md`.

### 3.13 Icons & SVG rendering — **nanosvg** + a colorful, illustrative icon identity
- **nanosvg** (zlib, header-only, vendored) rasterizes the **app icon at build** and tool/UI
  icons **at runtime** (full color) for crisp DPI scaling (no pre-baked PNG zoo).
- **Icon identity (a deliberate stance — see §1).** Mosaic's icons are **colorful, illustrative,
  and self-describing**: each icon *depicts what its tool/command does*, with real color and a bit
  of depth. We **explicitly reject the flat, monochrome, single-weight "corporate" line-icon
  aesthetic** (the Lucide / Tabler / Material-Symbols look that homogenizes modern UIs) — that is a
  **non-goal, not a fallback**. **Touchstones:** Affinity Photo's tool icons (colorful, legible,
  clearly functional) and GIMP's **legacy "Wilber" color icon theme** (pre-2.10, before the move to
  flat symbolic icons) as a friendlier reference. Mosaic aims for **that class** with its **own
  cohesive identity** — not a reuse of either set, and **not** a claim to out-design a pro team; the
  goal is a polished, self-describing colour set that *fits Mosaic*. Icons are **bespoke,
  GPL-compatible color SVGs** (credited in §7), **authored (or user-/community-provided) at S52** —
  there is no third-party set to "find," and a human-authored set is welcome and expected. Until S52,
  every tool ships a *coloured placeholder* (§3.13 last bullet).
- **`docs/icons-needed.md` is the running icon inventory** (user request, 2026-06): every icon,
  placeholder, or unicode/code-drawn stand-in is listed there with where it appears and its raster
  size(s) — update it in any session that adds one; it is the S52 design brief.
- **Placeholders are fine; monochrome is not.** Early tool sessions ship *placeholder* icons so the
  functionality is testable, but even placeholders are **colored** (not white/grey line art). The
  full, polished, consistent icon set is produced in **S52**.
- **⚠ Scope of the colour rule (user decision, 2026-07-09).** It governs the icons the eye picks
  **one of** from a set: the **tool icons** (19 in a column — colour is what makes them findable)
  and the **dialog stage icons** (one large face carrying the emotional register). It does **not**
  govern **panel chrome** — the layer dock's eye/lock cells and its add/group/delete buttons. Those
  are deliberately **one-ink** (`ui::drawIcon`: white-on-transparent SVG, RGB replaced by a palette
  colour at draw time, coverage kept in the alpha). Reasons: three colourful chips in a 32px dock
  strip read as clutter, and a baked-in colour cannot mute itself when the control is disabled or
  follow a light/dark re-theme. They are **final, not placeholders** — S52 leaves them alone. This
  is a narrowing of the rule's scope, **not** a licence to ship flat monochrome tool icons.
- For high-fidelity SVG *layer import* later, consider `resvg` (adds a Rust dep) — deferred.

### 3.14 Internationalization (i18n)
- **GNU gettext** (`libintl`): wrap user-facing strings in `_()` / `gettext`, extract a
  `.pot`, ship English first. Adding a language = drop in a `.po`/`.mo`. Runtime language
  switch. RTL and CJK handled via HarfBuzz/FreeType in the text stack. See `docs/i18n.md`.

### 3.15 Testing & debuggability
This is also **how Claude debugs features without a display**:
- **Unit tests:** **doctest** (MIT, header-only, vendored, fast to compile). `ctest`
  integration. (Catch2 is an acceptable system-package alternative.)
- **Headless op-runner:** `mosaic --headless` drives the document model + command system
  with **no GUI**, rendering through the Vulkan offscreen path **or a CPU fallback** when
  no GPU is present. Scriptable: `mosaic --headless --run ops.json` (a list of commands) and
  `--export out.png` so any pipeline can be exercised and diffed in CI.
- **Golden-image tests:** render a scene → compare to a reference PNG within a tolerance;
  store references in `tests/golden/`. This is the backbone for verifying filters,
  compositor, blend modes, inpainting, etc.
- **Vulkan validation layers** + best-practices layer in debug; RenderDoc capture markers.
- **Logging:** spdlog (MIT) with levels and categories; `--log-level`, `--log=file`.
- **Sanitizers:** ASan/UBSan presets; clang-tidy + warnings-as-errors in CI.
- **Lua scripting** layer (sol2/MIT) over the same command API, for power users and richer
  automated tests — **scheduled: S40** (it also carries the user-pluggable inpainting backend; §3.11).

### 3.16 The `.mosaic` native format (high level)
Open, documented, versioned, forward-compatible. **SUPERSEDES the ZIP-container sketch below** —
scoped 2026-07-07 against a standalone, empirically-validated 11-round research project (six
container designs benchmarked for corruption resilience/save-speed/compression; four-plus
undo-history designs benchmarked the same way; ~500 adversarial test checks). Full spec:
**`docs/mosaic-native-format.md`**; the research's own
narrative (not a spec — the story) is **`docs/mosaic-native-format-research.md`**.
- **HARD RULE (user, 2026-07-07): the user's file is written only by an explicit Save.**
  Incremental autosave goes to an app-owned **recovery journal** in the OS state directory
  (explicit-link WAL-style frames, O(changed data), bound to the exact commit it extends —
  a stale journal fails structurally), reset on Save, deleted on clean close; crash recovery
  restores it as *unsaved in-memory* state. Untitled documents get crash protection too.
- **File→Save COMMITS, it does not rewrite** (user, 2026-07-07; Round 12-tested): Save appends
  the session's new states as one atomic committed batch (O(changed data), ms not seconds — a
  torn Save opens at the previous commit); Save As / first-save / a threshold-tripped Save do
  the full write (compaction = parity refresh + eviction, folded into Save, never background —
  measured: appending costs the same bytes as the rewrite it replaces, −0.9%). Autosave cadence
  measured at 64px: heavy hour ≈ 60MB journal; SSD wear a non-issue at any cadence
  (~0.19%/day of a 600TBW drive worst-case) — coalesce for latency/battery, not flash.
- **Bespoke self-describing chunked binary** (PNG/EBML lineage), **not ZIP** — ZIP's central
  directory is written last and is the first thing lost on truncation, with zero built-in
  redundancy (measured against a chunked design that recovers ~85-88% of data across a corruption
  battery vs. ~17% for a ZIP baseline). Fixed 16-byte preamble (magic + format-version +
  `documentType`) for instant sniffing, never load-bearing; replicated, strong-checksummed
  root/directory (an accelerator, never the sole path in — full linear scan is ground truth);
  header-covering per-chunk checksums (xxh3-64 fast / BLAKE3 strong) over a structured
  16-byte chunk KEY (64-bit `LayerId` + tile coords — no arithmetic id encoding);
  checkpoint-only Reed-Solomon parity; Paeth spatial filtering.
- `manifest.json`/directory/history payloads **stay JSON** (nlohmann/json already vendored) —
  only the outer envelope changes from a ZIP wrapper to the chunk stream above.
- Layer pixel data as tiled chunks (per-tile, enabling partial load/save); vector/text/effects as
  structured JSON.
- **Unknown chunks are preserved** on load/save (forward compat, via a PNG-style critical/ancillary
  flag bit — no registry lookup needed). **Undo/redo history is retained inside the same
  container — LINEAR, not tree-structured** (a product decision made after the research concluded;
  every mainstream raster editor does linear undo). Default encoding: after-image-per-dirty-tile
  (the same content the recovery journal already writes at autosave; carried into the checkpoint
  by verify-then-copy, with journal states re-encoded once — Save cost never grows with session
  length); on by default, no toggle. An interactive
  undo/redo hot path (touches only the one crossed state's own dirtied keys, never a whole-document
  resolve) is part of the MVP, not an afterthought.
- **Document type** is a manifest field, so the one format round-trips both a **Raster** document
  (with its flattened pixel canvas + raster layers) and a future **Vector** document (geometry only,
  no fixed pixel canvas — see S30-b). Decide the schema for both at S48.

---

## 4. Architecture Overview

Layered modules with one-directional dependencies (UI → Core → Platform; nothing depends
on UI):

```
            +-----------------------------------------------------+
            |  ui/        FLTK shell, custom widgets, tools,       |
            |             panels, dialogs, menus, theming          |
            +------------------------+----------------------------+
                                     | (no reverse deps)
   +----------------+   +-----------------------+   +------------------+
   |  io/           |   |  core/                |   |  render/         |
   |  format mods,  |<->|  document model,      |<->|  Vulkan backend, |
   |  loss warnings |   |  layers/masks/effects,|   |  compositor,     |
   |                |   |  commands/undo, color |   |  shaders, VMA    |
   +-------+--------+   +-----------+-----------+   +--------+---------+
           |                        |                        |
           +------------------------+------------------------+
                                    |
                         +----------v-----------+
                         |  platform/           |
                         |  windowing, DPI,     |
                         |  native handles,     |
                         |  drag&drop, theme    |
                         |  detection, tablet   |
                         +----------------------+
            common/ : logging, math, color types, geometry, image buffers, i18n
```

- **Threading:** UI thread (FLTK event loop) · a render/submit thread (Vulkan) · a worker
  pool (I/O decode, CPU filter fallbacks, inpainting, thumbnailing). Document edits happen
  on the UI thread through commands; long ops run on workers with progress + cancel.
- **Toolkit isolation:** all FLTK usage lives in `ui/` + `platform/` so the Qt fallback
  (§3.3) stays contained.
- Details per module are authored into `docs/architecture.md` (S1) and refined over time.

---

## 5. Repository Layout

```
Mosaic/
├─ PLAN.md                 # this file (root, per brief)
├─ README.md               # starts "# An image editor, written by Claude" (S1)
├─ LICENSE                 # GPLv3 verbatim (S1)
├─ .gitignore
├─ CMakeLists.txt          # top-level (S1)
├─ CMakePresets.json       # (S1)
├─ assets/
│  └─ app_icon.svg         # provided; single source for all app icons
├─ cmake/                  # toolchains/, Dependencies.cmake, EmbedShaders.cmake, …
├─ src/
│  ├─ common/  core/  render/  io/  ui/  platform/
│  └─ app/                 # main(), CLI/headless entry, argument parsing
├─ shaders/                # .vert/.frag/.comp → compiled to SPIR-V at build
├─ third_party/            # vendored header-only/permissive: VMA, nanosvg, doctest,
│                          #   nlohmann/json, psd_sdk, (sol2 later)
├─ tests/                  # unit tests + golden/ reference images + ops scripts
├─ docs/                   # all docs except README/LICENSE/PLAN (per brief)
└─ tools/                  # dev scripts (icon gen check, i18n extract, etc.)
```

---

## 6. Dependencies (per platform, with licenses)

**Policy:** system packages first. **Header-only, permissive** libs are vendored in
`third_party/` (no user action). Anything that must be **built from source/installed
manually is flagged ⚠ and listed in the README** before the user hits it. MoltenVK is
**macOS-only** (never built for Linux/Windows). On the host (**CachyOS/Arch**), Windows
cross-libs come from the **AUR** (`mingw-w64-*`); several are not packaged and need a
manual cross-build (⚠) — those are enumerated in `docs/build-windows.md` (S57).

### 6.1 Build toolchain
| Tool | Arch (pacman) | Debian/Ubuntu (apt) | Fedora (dnf) |
|---|---|---|---|
| GCC ≥13 / build basics | `base-devel` | `build-essential` | `gcc-c++` |
| CMake ≥3.24 | `cmake` | `cmake` | `cmake` |
| Ninja | `ninja` | `ninja-build` | `ninja-build` |
| Git | `git` | `git` | `git` |
| Shader compiler | `shaderc` `glslang` | `glslang-tools` `spirv-tools` | `glslang` `glslc` |
| MinGW cross (Win) | `mingw-w64-gcc` + AUR libs ⚠ | `g++-mingw-w64-x86-64` ⚠ | `mingw64-gcc-c++` (good lib coverage) |

### 6.2 Required runtime/build libraries
| Library | Purpose | License | Arch | Debian | Fedora |
|---|---|---|---|---|---|
| Vulkan headers+loader | GPU API | Apache-2.0 | `vulkan-headers` `vulkan-icd-loader` | `libvulkan-dev` | `vulkan-loader-devel` `vulkan-headers` |
| Vulkan validation (dev) | debugging | Apache-2.0 | `vulkan-validation-layers` | `vulkan-validationlayers-dev` | `vulkan-validation-layers` |
| FLTK **1.4** | GUI toolkit | FLTK (LGPL+exc.) | `fltk` | `libfltk1.3-dev` ⚠ (1.4 may need build) | `fltk-devel` ⚠ (verify 1.4) |
| lcms2 | color management | MIT | `lcms2` | `liblcms2-dev` | `lcms2-devel` |
| FreeType | font rasterization | FTL/GPL | `freetype2` | `libfreetype-dev` | `freetype-devel` |
| HarfBuzz | text shaping | MIT | `harfbuzz` | `libharfbuzz-dev` | `harfbuzz-devel` |
| Fontconfig (Linux) | font discovery | MIT-like | `fontconfig` | `libfontconfig-dev` | `fontconfig-devel` |
| libpng | PNG | libpng | `libpng` | `libpng-dev` | `libpng-devel` |
| libjpeg-turbo | JPEG | BSD-like | `libjpeg-turbo` | `libjpeg-dev` | `libjpeg-turbo-devel` |
| libtiff | TIFF | libtiff (BSD) | `libtiff` | `libtiff-dev` | `libtiff-devel` |
| libwebp | WebP | BSD | `libwebp` | `libwebp-dev` | `libwebp-devel` |
| giflib | GIF | MIT | `giflib` | `libgif-dev` | `giflib-devel` |
| OpenEXR + Imath | EXR/HDR | BSD-3 | `openexr` `imath` | `libopenexr-dev` `libimath-dev` | `openexr-devel` `imath-devel` |
| libjxl | JPEG XL | BSD-3 | `libjxl` | `libjxl-dev` | `libjxl-devel` |
| libavif (+aom/dav1d) | AVIF (royalty-free) | BSD | `libavif` `libaom` `dav1d` | `libavif-dev` | `libavif-devel` |
| LibRaw | RAW + EXIF | LGPL-2.1 (used) | `libraw` | `libraw-dev` | `LibRaw-devel` |
| gettext / libintl | i18n | LGPL | `gettext` | `gettext` | `gettext` |
| spdlog (+fmt) | logging | MIT | `spdlog` `fmt` | `libspdlog-dev` | `spdlog-devel` |
| pugixml | XML (kpp/ora/svg) | MIT | `pugixml` | `libpugixml-dev` | `pugixml-devel` |
| libzip | `.ora`/`.kpp` (NOT `.mosaic` -- bespoke container, docs/mosaic-native-format.md) | BSD-3 | `libzip` | `libzip-dev` | `libzip-devel` |
| lz4 | `.mosaic` fast-tier compression (autosave journal) | BSD-2 | `lz4` | `liblz4-dev` | `lz4-devel` |
| zstd | `.mosaic` balanced/max compression | BSD-3 | `zstd` | `libzstd-dev` | `libzstd-devel` |

### 6.3 Optional libraries (feature-gated; off if missing)
| Library | Enables | License | Notes |
|---|---|---|---|
| libheif (+libde265/x265/kvazaar) | HEIF/HEIC | LGPL | ⚠ **never bundled** (§7); rely on system pkg; AVIF preferred |
| OpenCV | classical inpainting / CV helpers | Apache-2.0 | heavy; we may implement Telea/NS ourselves to avoid the dep |
| OpenColorIO | film color pipelines | BSD-3 | later enhancement |
| Lua + sol2 | scripting/automation (S40) + user-pluggable inpaint backend | MIT | **scheduled S40**; sol2 vendored. (No built-in ML inpainter — ONNX/LaMa dropped 2026-06-17; users script their own) |
| MoltenVK | Vulkan on macOS | Apache-2.0 | **macOS only**; via Vulkan SDK / `homebrew` in osxcross env |
| libcanberra | the AskOrTell dialog's alert sounds | LGPL-2.1+ | **Linux only** (`libcanberra` / `libcanberra-dev` / `libcanberra-devel`); plays freedesktop event ids from the user's XDG sound theme. Absent ⇒ silent dialogs, nothing else changes. macOS uses `NSBeep`, Windows `MessageBeep` — neither needs a dep |

### 6.4 Vendored (header-only, no user install)
VMA (MIT), nanosvg (zlib), doctest (MIT), nlohmann/json (MIT), **psd_sdk (BSD-2)**,
(later) sol2 (MIT). Tracked as pinned copies/submodules with their LICENSE files.

---

## 7. Licensing

- **Mosaic is GPLv3.** `LICENSE` is the verbatim GPLv3 text (added in S1).
- **All linked deps are GPLv3-compatible:** FLTK license, Apache-2.0, MIT, BSD-2/3, zlib,
  libpng, FTL, and **LGPL** libs (lcms2 is MIT; LibRaw/libheif/libintl LGPL paths are
  compatible). **LibRaw is used under its LGPL-2.1 option** (not CDDL, which is *not*
  GPL-compatible).
- **HEVC/HEIF is never bundled.** HEIF is **optional**, decode/encode only via the **system**
  `libheif`+codecs; we ship no HEVC codec. **AVIF (royalty-free) is the recommended first-class
  HDR/modern format.** This is a hard packaging constraint, not a to-do.
- **Bundled content licenses** (icons, preset brushes, sample images) are GPL-compatible
  and recorded in `docs/credits.md` (S52). The **default brush presets + tips are CC-0**
  (`Krita_4_Default_Resources`), attributed anyway as a courtesy; `RGBA_brushes` is not shipped
  because its licence field is under-specified. See `docs/brushes.md` §4. *(An earlier revision of
  this line said "we never ship Krita's GPL brush content" — that content is CC-0/CC-BY, not GPL.)*
  Third-party brushes a **user** imports are the user's business: we never inspect, report or
  restrict them, and our liability surface is only what we redistribute.
- **Bundled ICC profiles (S12-b/-c research, finalized 2026-06-10):** the default CMYK profile
  **`ISOcoated_v2_300_eci.icc`** (FOGRA39-based; vendored in `third_party/icc-profiles/`, extracted
  unmodified from Debian `icc-profiles_2.1`) is under the **HEIDELBERG ICC profile licence**:
  redistribution **is explicitly permitted** — including bundled with other software — provided the
  licence text ships alongside (it does), no fee is charged for the profile itself (Mosaic is
  gratis), and the file is unmodified. **Not DFSG/FSF-free** (no-modify + no-fee), so distros may
  strip it — it is a separate data file, not GPLv3 program code, and the app must degrade
  gracefully without it (CMYK model hides / S12-c user profiles take over). The zlib-relicensed
  basICColor variants are unobtainable with provenance (colormanagement.org is gone); **ECI's
  website downloads remain non-redistributable** — never vendor from eci.org directly.
- Each vendored lib keeps its upstream LICENSE in `third_party/<lib>/`. A generated
  `docs/third-party-licenses.md` aggregates them (S1 sets this up).

---

## 8. Coding Standards & Conventions

1. **Style:** `.clang-format` (LLVM-based, 4-space, 100 cols — finalized in S1). Format on
   commit. `clang-tidy` config in repo; warnings-as-errors in CI (GCC + Clang; **CI = GitHub
   Actions, stood up in S56-b**).
2. **Naming:** `PascalCase` types, `camelCase` functions/vars, `m_member`, `SCREAMING` for
   constants/macros, `snake_case` files. Namespaces mirror modules: `mosaic::core`,
   `mosaic::render`, `mosaic::io`, `mosaic::ui`, `mosaic::platform`, `mosaic::common`.
3. **Headers/units:** one primary type per file where reasonable; `#pragma once`; minimize
   includes (forward-declare; PIMPL for heavy/ABI-sensitive types).
4. **Error handling:** `std::expected`/error codes on hot/pixel paths; exceptions only at
   coarse boundaries (file I/O, startup). Every `VkResult` checked via a helper.
5. **Docs:** Doxygen-style comments on public APIs; each non-trivial module gets a
   `docs/<module>.md`. Comment *why*, not *what*.
6. **Git workflow:** linear history on `main`, **one feature/session per commit**
   (Conventional Commits: `feat:`/`fix:`/`build:`/`docs:`/`test:`/`refactor:`/`chore:`,
   optional scope e.g. `feat(render): …`). Every commit message ends with a co-author
   trailer naming the **model that actually wrote the session** — always the *current*
   model's name, never a name copied from an older commit:
   ```
   Co-Authored-By: Claude <current model name> <noreply@anthropic.com>
   ```
   (e.g. `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` as of 2026-06.)
   (A session may split into multiple commits if it was split into sub-sessions; keep each
   commit self-contained and buildable.) **Versioning + release tagging: see item 9.**
7. **Tests:** new logic ships with doctest unit tests; rendering/filters ship a golden
   image; keep `ctest` green. Don't merge a red build.
8. **Every commit must build** under `linux-debug` and pass `ctest`.
9. **Versioning (pre-1.0) — `0.3.0`, and it moves on RELEASES (decision 2026-08-24; supersedes the
   2026-06-22 "perma-alpha" freeze).** From S1 to S60 the version sat frozen at **`0.2.17`** and the
   reasoning was sound at the time: the pre-1.0 number was a placeholder of the form
   `0.<phase>.<session>`, the **roadmap went non-linear** (S37/S39/S51-a/S60-a landed out of order
   while S17/S18 stayed open), so no `0.<phase>.<session>` could honestly describe a build — and
   since nothing was ever *published*, the **short git commit** appended as build metadata was the
   only identifier that meant anything to anyone.
   **Publishing is what changed it.** A user who downloads an AppImage or an MSI has no git tree to
   interrogate, and a bug report has to be able to name a build. So the rule is now: **the version
   moves when, and only when, something is released** — which is the one thing SemVer actually asks
   of a pre-1.0 number. `0.3.0` is the first tagged, published release (S59, 2026-08-24). The commit
   is still appended and is still the exact identifier — `mosaic --version` prints e.g.
   `Mosaic 0.3.0+g4148fc3` (`-dirty` with uncommitted changes) — but the number in front of it is
   now the human-facing one, and it is what a release tag (`vX.Y.Z`) and every packaged artifact
   name are stamped with.
   **`1.0.0` is unchanged in meaning and still lands only after** the roadmap and immediate backlog
   are essentially complete — never mid-roadmap. (Long-tail backlog items may stay open and must not
   block 1.0.) At 1.0.0 MAJOR/MINOR/PATCH flip to breaking/feature/fix. Mechanism: `MOSAIC_GIT_REV`
   is computed in the top-level `CMakeLists.txt` and baked into the generated version header;
   configure re-runs when a commit lands (the git reflog moves).

---

## 9. Roadmap — Phases & Sessions

Each session = one coherent, commit-able feature. Format: **goal**, then *Deliverables*,
*Prereqs*, *Verify*, and *Covers* (which brief requirements it satisfies). Tick them off in
§10. Sessions may be split (`-a`/`-b`) if too large for one context window.

### Phase 0 — Foundations

**S1 — Repository scaffolding & build system.** Stand up the buildable skeleton.
- *Deliverables:* `CMakeLists.txt` + `CMakePresets.json`; `cmake/Dependencies.cmake`
  (system-first detection + clear missing-dep report) and `EmbedShaders.cmake`; toolchain
  file stubs; `src/` module dirs each compiling an empty lib; `app/main.cpp` printing
  version; **README.md** (first line exactly `# An image editor, written by Claude`, with
  per-platform build instructions: Arch/pacman primary, Debian, Fedora, Windows-cross note,
  macOS backlog note); **LICENSE** (verbatim GPLv3); `.clang-format`, `.clang-tidy`;
  `docs/` skeleton (`architecture.md`, `build-windows.md`,
  `build-macos.md`, `third-party-licenses.md`); doctest wired with one trivial test;
  `third_party/` license files.
- *Prereqs:* none. *Verify:* `cmake --preset linux-debug && cmake --build … && ctest` all
  succeed; `mosaic --version` runs.
- *Covers:* build system, README, LICENSE, GPLv3, docs/ layout, .gitignore, GCC-first.

**S2 — Vulkan bootstrap + headless/offscreen harness.** Instance/device/queues, VMA,
validation layers, shader compile→embed pipeline, an offscreen render target, and a CPU
fallback; `mosaic --headless --clear-color … --export out.png`.
- *Verify:* golden image of a cleared offscreen target; validation-clean. *Covers:* Vulkan
  foundation, **debuggability harness** for Claude.

**S3 — FLTK app shell + Vulkan canvas window.** Main window, menu-bar skeleton
(File/Edit/Image/Layer/Type/Select/Filter/View/Help), `VulkanCanvas : Fl_Window` showing a
cleared swapchain; DPI/scaling; app icon rasterized from SVG at build.
- *Covers:* window, menu bar skeleton, app icon from SVG, Vulkan-on-canvas.

**S4 — Theming engine + custom-widget base + tooltips.** `MosaicScheme` (`Fl_Scheme`),
palette tokens, system light/dark + accent + DPI detection, base widget classes, global
tooltip support.
- *Covers:* theming/native-feel (§3.5), tooltips, custom-widget groundwork.

**S5 — Settings, logging, i18n scaffolding.** Config store (load/save), spdlog setup,
gettext wrapping + English `.pot` extraction in build, error-reporting dialog.
- *Covers:* i18n (English first, easy to extend), logging, settings foundation.

### Phase 1 — Document model & canvas

**S6 — Document & layer model + undo/redo.** Layer tree (Raster/Vector/Text/Group/
Adjustment/Magic), masks, blend-mode enum, transforms; command stack with coalescing.
`docs/document-model.md`.
- *Covers:* layers, **layer groups**, masks data model, **magic layer** model, undo basis.

**S7 — Vulkan compositor.** Render the layer tree to the canvas: blend modes as shaders,
transparency checkerboard, adjustment-layer application within group/global, tiled dirty
regions. Golden tests per blend mode.
- *Covers:* "Vulkan acceleration of the canvas/preview", non-destructive composite basis.

**S8 — Canvas viewport interaction.** Zoom; **Space-drag pan**; **R-rotate** with a
degree-readout circle overlay, **Shift snaps to 5°**, **double-tap R resets**; fit/100%.
- **No canvas tooltip.** The Vulkan canvas deliberately has **no** hover tooltip — a tooltip
  popping up over the work area would be a constant irritation while drawing/editing. Tooltips
  stay on the toolbar, options bar, and panels (S4 onward); the canvas surface itself never
  shows one.
- *Covers:* space-pan, R-rotate overlay/snap/double-tap reset, no-tooltip canvas.

**S9 — New-document dialog.** Presets **A1–A4** (and A0/A5, US Letter/Legal, common px),
custom size/units/DPI, color space, bit depth, background.
- *Covers:* preset sizes (A1/A2/A3/A4…) on new project, color space at creation.
- **REDESIGNED 2026-07-22 (absorbing S55's welcome-screen role):** a category rail (Recent / Print /
  Screen / Templates) over a `GalleryCard` gallery, the settings form with a live Document Summary
  (resolved px + per-layer memory estimate), themed `ui::Dropdown`s throughout, **New from
  Clipboard**, template instantiation from `data/presets/*.mosaic`, and recent-file cards with
  cached previews. Returns a `NewDocumentChoice` (blank/template/clipboard/recent); the app
  dispatches. See the S9-follow-ups block in §12 for what landed vs remains.
- *Future redesign (deferred):* a **document-type chooser** (Raster vs Vector) up front — a Vector
  document drops the bit-depth/background-raster knobs and is resolution-independent. That redesign
  is where S30-b's Vector document type surfaces in this dialog.

**S10 — Layer panel (right tabbed dock).** Layer list with thumbnails, add/delete/
reorder/drag, **drag-onto-plus clones** (plus turns green w/ rectangular outline), group
create/collapse, visibility/opacity/blend mode, **Shift-click thumbnail selects layer
pixels**.
- *Covers:* right tabbed panel (Layers default), add/delete/drag/clone-on-plus, groups UI,
  shift-click-preview-to-select.
- **S10-d — Layer drag preview (drag ghost).** Make a layer drag *visually obvious*. Today (post
  S10-b) a drag only shows the accent **drop-line** (and the green "+" clone target); the row being
  moved gives no feedback beyond its active highlight. S10-d adds a floating, **semi-transparent
  "ghost"** of the dragged layer (its `layerThumbnail()` + name) that **follows the cursor**, and
  renders the **source row in a muted/"lifted" state** while the drag is live, so the stack reads as
  "this layer is in motion." On an invalid drop or a drop back at the origin, the row simply settles
  back where it was. Purely additive visual feedback layered on the S10-b mechanics
  (`rowPressed/rowDragged/rowReleased`, the drop-line, the green clone target) — the reorder/clone
  **outcomes and command-stack edits are unchanged**.
  - *Deliverables:* a ghost overlay drawn at the cursor (in `LayerPanel::draw()` over the list, or a
    lightweight top-level follow-widget if it must escape the dock bounds), compositing the dragged
    row's thumbnail + name at reduced opacity; a muted/lifted draw state for the source `LayerRow`
    while `m_dragging`; a press-time pointer offset so the ghost doesn't jump; ghost position tracks
    the `Fl::event_x/y` already read during the drag; the existing drop-line and green "+" target
    remain (the ghost is *in addition* to them). No new commands. *(Considered and rejected as
    fiddlier: actually removing the row from the list mid-drag and re-inserting on cancel — the
    ghost + lifted-row approach is the standard, robust pattern in Photoshop/Krita/Finder.)*
  - *Prereqs:* S10-b (done). Independent of S10-c — can land before or after it.
  - *Verify:* drive `rowPressed/rowDragged` with `Fl::e_x/e_y` set and screenshot the ghost mid-drag
    (the S10-b throwaway pattern); confirm a drop-back pushes **no** command; build + ASan clean;
    `--gui-frames` validation-clean.
  - *Covers:* polish of the S10 right-dock layer drag — a clear, direct-manipulation drag affordance.

### Phase 2 — Core tools & selection

**S11 — Tool framework + left toolbar + tool options bar + active-color swatch.** Tool
registry/active-tool, **left toolbar** with icons + tooltips + **grouping**, the **tool options
bar** (below) as the **primary per-tool surface**, and the **two-diagonal-square color swatch**
(active overlays inactive; click active→picker, click inactive→make active).
- **Tool options bar — the primary per-tool surface.** A slim horizontal strip pinned directly
  under the menu bar (S3) and **above** the document tab strip (S49). It surfaces the handful of
  **most-frequently-changed** settings for the active tool (each `ToolOption` flagged `primary`) —
  brush size/opacity, selection feather + boolean mode, type face/size — always one click away.
  The full option set stays in the tool model; the *deep* settings are reached via a **"More…"**
  affordance that opens the tool's **own dedicated panel** when one exists (Brush Settings @S19,
  Character/Paragraph @S29, gradient editor @S22) — Affinity's context-toolbar model, which we
  prefer to a generic dump. With no active tool/document the bar is empty.
- **No generic "Properties tab".** Earlier drafts paired the bar with a right-dock tab that
  rendered the *same* options vertically. That is **cut**: an auto-listed mirror is redundant for
  simple tools and inadequate for rich ones. "Properties" returns later as a **contextual** dock —
  the *selected object's* geometry/attributes and *adjustment-layer* params (different content,
  populated by S15/S32) — not a tool-option mirror. The dock stays **Layers-only** until then.
- **Toolbar grouping (single column + flyouts + subtle dividers).** A flat column does not scale
  to ~30 tools (taller than the screen). Tools declare a **`ToolGroup`**; the column draws **thin
  dividers** between groups (S11-c) and nests tool **variants behind flyout popups** (long-press /
  corner triangle; Photoshop/Affinity-style — S11-e), keeping one neat column that still scales.
- *Covers:* left toolbar (grouped, flyouts), tool icons, **tool options bar** (primary surface +
  "More…" bridge), the diagonal color swatch behavior, tools-use-active-color plumbing.

**S12 — Color picker (Affinity-style popover) + color management.** Grow the S11-d flat-colour
stub into the real picker on the **same `ui::Popover` host** — a compact, **anchored popover** (the
shape settled with the user: *not* the Photoshop/Krita always-docked giant selector). Add a colour-
**model combo** (HSL / RGB / HSV / Lab / Hex) over sliders + a hex field + a **colour field/wheel**;
**lcms2** integration with the document working space, a **color-space indicator**, an **out-of-gamut
warning** when the picked colour falls outside the working space, and a **pick-within-gamut / snap to
nearest in-gamut** action. Plus swatches/palette + recent colours. **Redesign the hex input** (S11-d
ships a bare `#RRGGBB` text box — the user finds the leading `#` ugly *and* a plain text box unsatisfying):
e.g. a styled field with the `#` as a fixed prefix glyph / inset label, fixed-width hex glyphs, paste-friendly.
- **Split (2026-06):** **S12-a — picker UI:** the model combo with **HSL / RGB / HSV** (pure-math
  models; a "Hex" combo entry was cut in review — the hex field is permanent), the **colour
  field/wheel**, and the **hex-input redesign**. **S12-b — colour management:** **lcms2** + the
  document working space, the **Lab + CMYK** models (CMYK lcms2-managed, 4 slider rows),
  the **colour-space indicator** (interim home inside the picker until the **S13-b** status bar
  lands, one session later), the **out-of-gamut warning + snap** (semantics below), and
  **swatches/palette + recent colours** (working-space-aware). One commit per sub-session (§0).
- **S12-c — ICC profile support (added 2026-06, user request).** Load **user-supplied `.icc`
  files**: `core::ColorEngine` gains a from-file constructor; a settings-level choice of CMYK
  profile (defaulting to the **vendored `ISOcoated_v2_300_eci.icc`** — FOGRA39-based, HEIDELBERG
  licence, redistribution-clean; provenance + terms in `third_party/icc-profiles/README.md` and
  §7) and of a custom working space. Document-level **Assign/Convert profile** arrives with the
  image ops (S53-a); honouring **embedded profiles** on open/export arrives with S41–S42.
- **Gamut-warning semantics (S12-b; settled with the user, 2026-06):** "out of gamut" means **outside the
  document's working space**. RGB / HSV / HSL / hex edits are expressed *in* the working space, so they
  are in-gamut by construction and never warn. **Lab mode is the only S12 route to an impossible
  colour** (CIELAB spans colours no RGB working space reaches). While editing in Lab, keep the
  unclamped Lab value live so the warning — a Photoshop-style **⚠ triangle** beside the preview, with a
  small swatch of the **nearest in-gamut colour** — can appear/disappear as the user drags; clicking the
  triangle/swatch snaps to that colour (the "pick-within-gamut" action). What lands in `ColorState` is
  **always the clamped working-space colour** (it is a `common::Color8`, so commit-time clamping is
  forced by the type, not a policy choice). Detection: convert Lab → working space as unbounded float
  (lcms2) and test for channels outside [0, 1]; snap = clamp / relative-colorimetric re-conversion.
  (Photoshop's yellow triangle actually warns against the **CMYK proof space** — that is
  *soft-proofing*, deferred with the §11 soft-proofing item, **not** S12 scope.)
- *Covers:* color picker (popover), colorspace support + indicator, gamut warning, active-color source.
- *Builds on S11-d:* `ui::ColorState` (active fg/bg), `ui::Popover` (anchored **child sub-window** host —
  works identically on X11 / XWayland / native Wayland; no positioning caveat left), `ui::ColorPicker`
  (RGB + hex), `parseHexColor`/`hexString`. Keep growing the picker as ordinary FLTK widgets in the
  sub-window (the combo's own dropdown, the wheel, etc. all just work) — don't make it a top-level.

**S13 — Selection model + animated marching ants.** Selection mask + boolean ops; the
**animated black/white dashed marquee** scrolling along edges (GPU shader).
- *Covers:* animated marching-ants selection.

**S13-b — Status bar.** *(Promoted from §12 backlog, 2026-06.)* A slim bottom strip on the main
window: **document size** (px + physical size at the document ppi) + **bit depth**; the live
**cursor position** in document coordinates and the **colour under the cursor** (swatch + RGBA/hex)
while the pointer is over the canvas; **zoom %** + **rotation°**; the **colour-space indicator**
(moves here from its S12-b interim home in the picker; the **HDR indicator/warning** joins at
S43-c); the active **selection bounds** (S13 just landed); and a **status/progress** area for long
worker operations (wired up as workers arrive, §4). Shares its pixel-readout source with the future
Info panel (§11) so the two never disagree.
- *Prereqs:* S13 (selection bounds; everything else is S8/S9-era state). *Verify:* readout
  formatting (coords/colour/physical size) unit-tested pure; screenshot from the user;
  `--gui-frames` validation-clean.
- *Covers:* the §12 status-bar item; the permanent home §3.6's indicators were missing.

**S14 — Marquee select tools.** Rectangle, ellipse, **lasso**; Shift/Ctrl/Alt = add/
subtract/intersect & constraints.
- *Covers:* select tool rectangle/ellipse/lasso, modifier support.

**S14-b — Clipboard (cut/copy/paste).** *(Added 2026-06 — a core editor feature the roadmap had
skipped entirely.)* Edit→Cut/Copy/Paste + **Copy Merged** + **Paste as new layer**, Photoshop-standard
shortcuts (Ctrl+X/C/V, Shift+Ctrl+C/V). Copy takes the active layer's pixels under the current
selection (whole layer when none); cut also clears them; paste creates a new raster layer (at source
position in-document, centred otherwise). Everything through commands (undoable). **External
interop:** copy places a flattened RGBA image on the OS clipboard and paste accepts images from other
apps, via FLTK 1.4's image clipboard (`Fl::copy_image` / `Fl::paste` — X11 + Wayland + Windows).
- *Also (rider, user-requested 2026-06):* **boolean ops for Shift-click-thumbnail**. The S13
  gesture is Replace-only because Shift is spent as the trigger; compose the *other* modifiers on
  top: **Shift+Ctrl+click = Add**, **Shift+Alt = Subtract**, **Shift+Ctrl+Alt = Intersect** (Add
  and Intersect are literally Photoshop's combos, whose trigger is Ctrl). Do NOT move the trigger
  to Ctrl: Ctrl+click on rows is the future multi-select-layers gesture. Indicator: a tiny
  **+/−/× chip drawn on the thumbnail's ants frame** (the panel owns that draw code — same badge
  language as the canvas cursor, zero new custom cursors; the stock hand stays, per user).
- *Prereqs:* S13/S14 (selection model + tools). *Verify:* unit tests on the mask-apply/flatten logic;
  headless command round-trips; cross-app copy/paste confirmed interactively by the user.
- *Covers:* clipboard in-app + external, paste-as-layer; thumbnail-selection boolean ops.

**S15 — Move/transform tool (the default **Move** tool, V — an arrow-**cursor** icon).** Modelled on
**Affinity Photo's Move tool**, *not* Photoshop's pure layer-nudge: with the plain cursor, **click an
object on the canvas to select it** (also selecting its layer in the stack), then **transform handles**
appear around it — drag a handle to resize/rotate/skew (with **Shift/Ctrl/Alt** constraints), drag the
body to move. Click empty space to deselect. (It is *not* a pan-the-canvas tool — that is Space-drag /
the Hand, S8.)
- *Covers:* "select tool with plain cursor → selects in stack + transform box", Shift/Ctrl/Alt
  resizing, Affinity-style click-select-then-handles.

**S15-b — Interactive drag latency (mini-session).** *(Added 2026-06-12, user request, after
the S15.x/y/z fix passes made the costs measurable.)* Two scoped changes that cut 60 Hz
input-to-photon latency during Move drags / opacity gestures from ~50–80 ms toward ~25–35 ms.
Context a fresh session needs: the per-frame composite was already taken from 305 → 67 ms
(1080p×3 layers, debug, 8 cores — S15.y: -O2 pixel modules, fused `rasteriseLayer`,
`parallelFor`, mallopt) and presentation already prefers MAILBOX (S15.z rider). The two
remaining software costs, in order:

1. **Event-driven frame kicks.** `MainWindow::onFrame` (src/ui/app_window.cpp, `frameTimer`)
   runs off a free-running `Fl::repeat_timeout(1/60)`; drag input recorded by
   `VulkanCanvas::dragMoveTool` waits in the pending slot up to 16.7 ms (avg ~8) for the next
   tick, and the unsynced timer beats against vblank (felt as lag wobble). Fix: a
   `MainWindow::requestFrame()` — when canvas-affecting input lands (move-drag pending,
   selection-gesture preview dirty, pan/zoom/rotate) and no frame is queued, fire the frame
   timer immediately (`Fl::remove_timeout` + `Fl::add_timeout(0.0, …)`); `onFrame` re-arms
   the 1/60 heartbeat (ants crawl + rotation dial still need it when idle). Guard with a
   frame-queued flag so event storms cannot busy-loop: at most one immediate kick per frame
   interval. The canvas reaches the host via a callback, following the existing
   `MoveToolHost` / selection-host pattern (src/ui/vulkan_canvas.hpp).

2. **Drag-scoped composite cache.** During a Move drag only the dragged layer's transform
   changes, but `recompositeNow` re-rasterises and re-blends EVERY layer each frame. At drag
   start cache: (a) `belowAcc` — the composite accumulator of root children `0..i-1` (`i` =
   the dragged top-level child's index), and (b) the rasterised doc-space `ImageF` of each
   child above `i` (per-layer `rasteriseLayer` output is the expensive part; the blends are
   cheap). Per frame: copy `belowAcc`, rasterise ONLY the moved layer (world transform),
   blend it, then blend/apply the cached above buffers in stack order. This is **exact** for
   all blend modes, clip-to-below, and adjustment layers (adjustments above apply to the live
   accumulator exactly as in the full walk). Constraints: enable only when the drag target is
   a TOP-LEVEL child of the root (nested targets → full-composite fallback; the general case
   belongs to S60-a's tiles); cap cache memory (≈8 doc-sized float buffers; fall back past
   it); invalidate on `MoveToolHost::gestureEnded`, any non-SetTransform command, undo/redo,
   panel edits, and document switch — simplest correct rule: drop the cache whenever
   `syncAfterEdit`/`LayerPanel::refresh` runs, rebuild lazily on the next drag frame. The
   pieces it needs (`rasteriseLayer`, `compositeBufferOver`, `applyAdjustment`) are file-local
   in src/render/compositor.cpp — build the cache inside the render module (e.g.
   `render::DragCompositeCache`) rather than exporting them.
- *Verify:* unit tests comparing hot-path output **byte-for-byte** against the full composite
  for: a Normal stack; a non-Normal blend above the target; an adjustment layer above;
  clip-to-below above; and the not-top-level fallback. The existing golden test must stay
  green. Bench target: ≤ ~15 ms/frame at 1080p×3 (debug; write a throwaway bench like the
  S15.y one). Manual: Move drags + the opacity slider on a 60 Hz display — the user verifies.
- *Covers:* the interactive half of §12's edit-latency item. S60-a/b still own GPU residency,
  dirty tiles, present pacing, and moving the composite off the UI thread — do NOT start
  those here; this mini-session is deliberately two changes and done.

**S16 — Crop tool.** Interactive crop with aspect presets; non-destructive option.
- *Covers:* crop tool.

**S16-b — History panel.** *(Added 2026-06, user request.)* A **History tab** sharing the right
dock with Layers (the S10 header grows into a real tab strip): the command stack as a list
(commands already carry `name()`), oldest at the top, the current stack position highlighted;
**click an entry to jump** there (multi-undo/redo through the existing stack — no new command
types). Entries past the position render muted, exactly like the stack's redo tail behaves.
Keep it deliberately dumb: a *view* over `core::CommandStack` plus an observer hook — no
snapshots, no branching, no persistence. (**Selective deletion** of entries is S36-b — it needs
the command-footprint API; this panel is its UI landing zone.)
- *Prereqs:* S10 (stack + dock). *Verify:* list/jump model pure + unit-tested (FLTK-free);
  interactive check by the user.
- *Covers:* the history/undo panel every Photoshop-class editor carries; the ground S36-b
  stands on.

**S17 — Magic wand (research-first).** `docs/research-selection.md`: color-distance flood
fill in a chosen color space, tolerance, contiguous/global, anti-aliased edges, feather.
- *Covers:* magic-wand select (researched).
- **LANDED 2026-07-15** (§10). The research note went first, as §0 requires, and the wand went ahead
  unchanged — the tolerance flood is 1990s-era technique, and it is the *edge-aware* "quick selection"
  neighbour that carries the constraints (see the edge-brush note under S18). The colour-distance metric
  became `core::wandColorDistance` and is now shared by S21's bucket fill and S24's eyedropper, so the
  three tools cannot drift on what "similar colour" means.

**S18 — Select brush (research-first) + Select menu.** Paint-to-select with soft edges;
Select menu (all/none/inverse, grow/shrink/feather/smooth, **Mask from selection** entry
wired in S31).
- *Also:* the **marching-ants direction experiment** (user discussion, 2026-06). The present
  pass dashes along screen-space x+y, so the ants drift uniformly toward one corner instead of
  circulating around the boundary like Photoshop's (which animates dash offset along the stroked
  contour). Cheap shader-level try: the pass already samples the 4 neighbours, so the mask
  gradient is nearly free — rotate it 90° for a local tangent and dash along `dot(p, tangent)`,
  which circulates clockwise on any boundary (corners cheat a little). Land it behind a
  **documented hidden setting** (`antsCirculate`; no UI entry — list it in
  `docs/settings-and-logging.md`, the key doubles as the experiment flag). The **default stays
  the diagonal crawl** — it is calm, uniform, and liked; if the tangent variant shimmers on AA
  edges it simply stays hidden.
- *Covers:* select brush (researched), Select menu.
- **LANDED 2026-07-15** (§10), in five slices, **including the ants-direction experiment** — the
  `antsCirculate` tangent variant went in behind the hidden setting exactly as scoped, and the diagonal
  crawl remains the default. The paint-to-select stroke reuses the S19 brush walk for **coverage only**
  (`MaskStroke` in `core::brush`), so soft edges come from the same falloff the paint tools use rather
  than a second implementation. The **edge-aware variant** followed 2026-07-16 as `ToolId::EdgeBrush`,
  narrowed to what its research pass allowed: variant (b), solve-on-release, shipped;
  ⚠ **variant (a) — live-during-stroke — is deliberately NOT enabled** and its patch stays parked.
  That is a standing constraint on this tool, not an unfinished slice.

**S18-b — Dev-grade image open & save (PNG + JPEG).** *(Pull-forward, added 2026-06.)* A deliberate
thin slice of S41/S42 so Phases 3–6 are dogfooded on **real images** instead of placeholder/new docs:
File→Open decodes PNG/JPEG (libpng / libjpeg-turbo, both already required deps) into a
single-raster-layer document; File→Export writes the flattened composite (PNG always; JPEG + quality).
Lives in `io/` so S41/S42 grow it into the real registry — **no** format registry, loss-warning
dialog, metadata, or colour management yet (S41/S42/S12-b absorb this). Also unlocks golden tests and
user screenshots against real photographs for every tool session that follows.
- *Verify:* PNG/JPEG round-trip goldens; open→edit→export exercised headlessly via the op-runner.
- *Covers:* early dogfooding; a usable open/export path long before Phase 7 (de-risks S41/S42).
- **OPEN portion DONE 2026-06-19** (Save/Export still pending): `io::loadImage` decodes PNG (libpng
  simplified `png_image` API) + JPEG (libjpeg-turbo `tj3` API) into 8-bit straight-alpha RGBA, format
  **sniffed from magic bytes** (not the extension); `io` now links libpng + libturbojpeg. File→Open
  (native chooser, ⌘O) builds a **single UNLOCKED raster layer** named after the file and presents it
  (unlocked because you opened it to edit — unlike New's locked Background). `tests/test_io.cpp` +
  `tests/fixtures/sample.{png,jpg}`. Save/Export (PNG always; JPEG+quality) is the next S18-b slice,
  and unblocks **S18-d** (unsaved-title).

**S18-d — Unsaved-state window title.** *(Added 2026-06, user request.)* The title becomes
**`<doc>.<ext> • unsaved[ for N min] — Mosaic`** — document first, because taskbars and alt-tab
truncate from the right. Dirty tracking = a **saved-position marker on the command stack** (no
separate boolean to desync; Save moves the marker, undoing back to it goes clean again), which
is why this rides after **S18-b** — there is no Save to clear the flag before that. The bare
`• unsaved` appears with the first unsaved edit; the duration joins past a threshold (default
**5 min**), at minute granularity. Settings live under a personality-bearing section header —
**"Annoyances"** — while the labels themselves stay translation-plain: **"Show how long the
document has been unsaved in the title"** (on by default) and **"…include seconds"** (off by
default: a title re-rendering every second is motion in the eye-line and makes some WMs and
screen readers chatty; minutes mean one quiet update per minute off the existing frame timer).
- *Prereqs:* S18-b (Save exists). *Verify:* title formatting + dirty-marker transitions pure +
  unit-tested (incl. undo-back-to-clean); interactive title check on X11 + Wayland.
- *Covers:* the unsaved-changes affordance (user request, 2026-06).

### Phase 3 — Painting & raster tools

**S19 — Brush engine + preset brushes + tablet input.** Split (2026-06):
- **S19-a — Engine core + presets.** GPU stamping, spacing/flow/opacity/hardness; ships preset
  brushes; uses the active colour; a **pressure/tilt-ready dynamics API** (driven by the mouse until
  -b). NB: GPU stamping onto CPU-resident layers would round-trip per dab — expect to pull a slice of
  **S60-a GPU residency** forward for the *active layer* (acknowledged in §13).
  - **BASE DONE 2026-06-19** (no presets — user-directed): a **CPU** stamping engine
    `src/core/brush/BrushEngine` (spacing-walked dabs, flow + per-stroke opacity cap, smoothstep
    hardness, active colour, pressure/tilt-ready dynamics with mouse pressure = 1), wired into
    `VulkanCanvas` (live recomposited preview → one `SetLayerPixelsCommand` per stroke). The **brush
    reticle is a GPU shader** in `canvas_present.comp` riding the **lasso overlay SSBO** — a size ring
    at the cursor drawn with the **same luminance-keyed monochrome as the lasso line** (white on dark
    content, black on light; reworked 2026-06-19 from the first two-tone version per user), with a
    **padlock glyph + status hint when the active layer is locked** (the Brush respects `locked()`;
    the ring is **punched out** within the lock's clear zone so they never collide — small brush shows
    the lock alone, larger fades the ring in around it; OS pointer hidden over the canvas). The New-doc
    Background is now **unlocked by default** (user 2026-06-19) so a fresh canvas paints immediately.
    The reticle + engine are the reuse base for Eraser/Clone(S38)/
    Inpaint(S39). `tests/test_brush_engine.cpp`. **Decision (user 2026-06-19): CPU stamping core is
    the keeper** (a future CPU-only mode wants it); GPU stamping (the S60-a pull-forward) stays
    **deferred behind the same engine API**. **Big-doc brush lag (≈53 ms/frame at 1920×1080 vs 6.6 ms
    at 500²) is the S60 dirty-tile recomposite** — measured, NOT fixed here (full-doc CPU `ImageF`
    recomposite per frame; user: "tiling is for S60"). *Still to do for full S19-a:* preset brushes,
    the Brush Settings panel (spacing/dynamics/scatter/texture UI), and the GPU path.
  - **ENGINE REWORK SCOPED 2026-07-09 → `docs/brushes.md`** (the note §3.12 promised; every format
    fact in it verified against a shipped file). The S19-a remainder is now a four-arc rework:
    **A** engine (curves/sensors → 6 mask generators → bitmap tips + LRU cache → `StrokeAccumulator
    {Uniform,Colored}` × `PaintMode {Wash,Buildup}` + erase + blend modes + masking brush → stroke
    state), **B** formats (PNG chunk walker → pugixml → `.kpp`/`.gbr`/`.gih`/`.abr`/`.bundle` +
    fidelity mapper → `common::dataDir()` + preset library → CC-0 default set), **C** input
    (`docs/tablet.md`), **D** UI (curve editor → preset chip → Brush-only dock section → modal editor
    with a paintable preview → tip-shaped reticle → eraser ties). A and B are headless-testable and
    land before any UI. **The Eraser is built in this rework** — `ToolId::Eraser` is registered but
    has no canvas path today, and the engine has no destination-out. **Root cause of the rework:**
    `m_coverage` is single-channel float with one colour applied at `composite()`, so per-dab colour,
    smudge, blend modes and erase are *inexpressible*, not merely unimplemented. `Uniform × Wash` is
    pinned byte-identical to today by test.
- **S19-b — Tablet/stylus input (pressure + tilt; research-first).** Wire real tablet events into the
  tool pipeline: **XInput2** valuators on X11 and **`zwp_tablet_v2`** on native Wayland (we already
  speak raw Wayland for the canvas subsurface — same plumbing precedent), normalised into `platform/`
  events the brush dynamics consume; Windows Ink arrives with S57. FLTK has no tablet API, so this is
  platform-layer work — short `docs/tablet.md` research note first. *(Added 2026-06 — previously the
  engine was "pressure-ready" but no session ever delivered pressure.)*
  - **RESEARCH NOTE WRITTEN 2026-07-09 (`docs/tablet.md`); the §0 gate is satisfied.** Confirmed by
    inspection: FLTK 1.4.5 exposes no tablet API and `libfltk` carries no `zwp_tablet` symbols.
    Design: a `platform::TabletSample` normalized from each backend; XI2 fills a lock-free sample
    ring while FLTK's `FL_PUSH/DRAG/RELEASE` keep driving the stroke *lifecycle*, and each `FL_DRAG`
    **drains the ring** — buying ~200 Hz sampling (no polygonal fast strokes) and **sub-pixel dab
    placement** without restructuring the event pipeline. ⚠ **Wayland spike required first:** FLTK
    exposes `fl_wl_display()`/`fl_wl_compositor()` but **not its `wl_seat`**, and tablet events are
    delivered per-`wl_surface` while our Vulkan content lives on a child subsurface. **Scope: Linux
    now (X11 + Wayland); Windows (WinTab + Windows Ink + a driver-workaround layer) and macOS are
    designed in the note and built at S57/S58.** Stabilizer's rope/"pulled-string" variant is
    deliberately deferred and is not built here.
- **S19-c — Crisp pixels + pixel grid at high zoom.** *(Promoted from §12 backlog, 2026-06.)* The
  present pass switches to **nearest-neighbour** sampling above a zoom threshold (≈ ≥ 600 %) so
  zoomed-in pixels are discrete instead of bilinear blur, plus an optional **pixel grid** overlay
  (hairlines between texels, Photoshop/Krita-style) — both **View**-menu toggles. Small +
  self-contained (a sampler/threshold switch in `canvas_present.comp` + a grid pass); scheduled here
  because pixel-level editing starts with the painting tools. Distinct from the S24 loupe (own grid).
  **Also: the screen-space transparency checkerboard** *(user-requested 2026-06)*. Today the CPU
  compositor *bakes* a document-space checker behind the layers (`CompositeOptions::checkerboard`),
  so it scales and rotates with the view. Move it into `canvas_present.comp`: composite with **true
  alpha**, and where alpha < 1 blend a checker computed in *pre-rotation screen* coordinates
  (constant ~8–12 px under any zoom/rotation — Photoshop behaviour), greys from the theme palette.
  Bonuses: the status-bar/Info readout starts reporting *real* pixel values (today it reads the
  checker-contaminated composite); Copy Merged already wants a checker-free flatten. Mind: the
  present shader gains an alpha path, checker-baking golden images regenerate, and the §12
  rotated-canvas-edge AA note touches the same boundary math — batch them here.
  **DONE 2026-06-16** (commits `39d7f4d` crisp+grid, `4d03b9b` checker): after checking the
  industry, the nearest-neighbour switch is **hardcoded on the zoom ratio, NOT a 600 % threshold
  and NOT a toggle** (user decision) — nearest whenever *magnifying* (> 100 %, < 1 doc texel/screen
  px), bilinear when minifying (Photoshop/Krita rule: blockiness when zoomed in is *wanted*,
  shimmer when zoomed out is not). The **pixel grid** is the only View toggle (default on,
  auto-fading in by texel size ~400–800 %). Crisp sampling needed **no Vulkan change** — sampling
  at texel centres through the existing bilinear sampler == nearest. The grid on/off bit rides
  `pc.bgColor.a` (push block already at the 128-byte budget). Checker greys are fixed 255/205 in
  screen px (8 px) — theme-palette-derived greys deferred as polish. **No goldens regenerated**: the
  only display path flipped to true-alpha; the compositor keeps `checkerboard` for `--composite-demo`
  + Copy Merged. The rotated-edge AA note was left untouched. The §12 backlog entry is cleared.
- *Covers:* brush tool + preset brushes, active-color use, real pressure/tilt (§3.12, §11),
  crisp-pixel zoom + pixel grid.
- **LANDED — all four arcs, 2026-07-09 → 07-29 (§10 S19).** The Brush Settings panel this section asked
  for arrived as two surfaces, not one: a preset **card** list in the right dock plus the modal
  `ui::BrushEditorDialog`. Per-option transcription then continued for another nine sessions past the
  arcs (`docs/brushes.md` §6.6b–6.6i) and is tracked there by a **conformance census**, not by a
  checkbox — currently **104 Exact / 7 Approximate / 6 unsupported** over the shipped corpus. ⚠ The
  standing ruling that governs all of it (2026-07-27): the **per-mechanism brush gates are WITHDRAWN** —
  a brush mechanism transcribed from Krita's published GPL source needs no gate of its own, which
  supersedes the individual gates this section and `docs/brushes.md` §5 record. The one exception kept
  shut is the **stabilizer's** rope / pulled-string variant. **GPU stamping remains the one deliberate hole** (§6.3): it stays CPU
  behind the engine API, with the mask generators written as pure functions so a GLSL mirror plus a
  parity lane is a mechanical follow-on at S60-e.

**S20 — Brush import.** `.gbr`/`.gih`/`.abr`/`.myb` then **Krita `.kpp`** (PNG text-chunk XML).
- **The research is discharged: `docs/brushes.md` §3 is the verified format reference** (every fact
  checked against a shipped file — chunks decompressed, XML parsed, bundles enumerated), and §7 is the
  build order. This session is now **Arc B of the S19 engine rework**, not a research session: PNG
  chunk walker → vendored pugixml → `.kpp` reader + paintop mapper + `PresetProvenance` fidelity →
  `.gbr`/`.gih`/`.abr`/`.myb` → `.bundle` → `common::dataDir()` + the preset library → the CC-0
  default set + `docs/credits.md`. Depends on Arc A's engine model; both are headless-testable.
  **Importing third-party brushes is first-class and stays uninspected** (`docs/brushes.md` §4.1):
  we clear only what *we redistribute*, and `PresetProvenance` reports fidelity, never copyright.
- *Covers:* importing Krita brushes.
- **LANDED 2026-07-10** (§10 S20) — every slice above, in that order, plus two the list did not name:
  `md5` as the tip-resolution digest (case-insensitive, as the corpus turned out to require) and the
  native **`.mbp`** container, so a user's own preset has a home that is ours. ⚠ **User presets are
  `.mbp`, never `.kpp`** — we read Krita's format and never write it.

**S21 — Bucket fill.** Tolerance flood fill + pattern fill; active color.
- *Covers:* bucket fill tool.

**S22 — Gradient tool.** Linear/radial/elliptical/conical, editable stops + **blend
curves**; creates an **editable, maskable gradient layer** (effectively vector).
- *Covers:* gradient tool (all types + curves) as editable maskable layer.

**S23 — Eraser + Blur/Dodge/Burn + Smudge brushes.** Brush-family tools sharing the engine.
- **The Eraser half moves into the S19 engine rework** (`docs/brushes.md` §2.3, §6.1, §8.4).
  `ToolId::Eraser` is registered with options today but has **no canvas path and the engine has no
  destination-out**, so selecting it is inert. It is built there as `StrokeMode::Erase` plus the two
  eraser-tie preferences, because it is an accumulator axis rather than a tool.
- **The siblings then reduce to tool wiring.** Once Arc A lands `StrokeAccumulator{Uniform,Colored}`
  the dab can read its destination, so Blur/Dodge/Burn are per-dab kernels over the base snapshot.
  **Smudge is plain destination-sample-and-blend** (the Painter-era technique) — a per-pixel "paint
  load" channel is deliberately NOT built (`docs/brushes.md` §5), and this is the route taken
  instead. `colorsmudge` preset import maps here at Approximate fidelity (§6.4).
- *Covers:* blur/dodge brush tool (+ burn, smudge, eraser as natural siblings).
- **PARTLY LANDED (§10 S23).** ✅ The **Eraser** shipped 2026-07-10 exactly as scoped —
  `StrokeMode::Erase` plus the eraser-tie preference. ✅ **Smudge** shipped 2026-07-14, but as a full
  transcribed `colorsmudge` **engine** (`docs/brushes.md` §6.6c) reachable through presets — not at
  Approximate fidelity, and not as its own `ToolId`. The route this section chose to avoid the paint-load
  channel is the route taken. ❌ **Blur / Dodge / Burn are still unwired** — the prediction holds: with
  the accumulator landed each is a per-dab kernel over the pre-stroke snapshot, which is the shape the
  S38 clone stamp already proves, so what is left is a `kToolDefs` row and an operator apiece.

**S24 — Color-picker (eyedropper) tool with loupe.** Sample color; **circular popup
magnifier following the cursor** showing nearest pixels with a grid (GPU-magnified).
- *Covers:* color picker tool with magnifier loupe + pixel grid.
- *Status (2026-07-15) — **BUILT** (`docs/eyedropper-loupe.md`).* The tool's options (Sample
  Point/3×3/5×5/11×11 + Source Active/All Layers) pre-existed but were **inert**; S24 wired the
  sampling + the loupe. **Sampling:** pure `core::sampleColor` (per-channel straight-8-bit box average,
  edge-clipped) fed by the Magic Wand's own source resolvers (`activeLayerDocImage`/`wandMergedSource`,
  so *Source* can't drift between the two tools); a press picks into the **foreground**, **Alt/right →
  background**, and a **drag samples live**. No undo step (a swatch change is app state). **The loupe:**
  a circular **GPU** magnifier drawn by the present pass — its own SSBO channel (**binding 10**,
  `WindowRenderer::setLoupe` + a `loupe()` in `canvas_present.comp`), centred on the cursor (OS pointer
  hidden, reusing the brush's `want==20`), nearest-neighbour magnifying `uDoc` at the texel-centre snap
  with its **own** pixel grid (distinct from the S19-c View grid), a box-blue **centre cell**, a swatch
  band + a hex/RGB readout tile (shared overlay tile). Colour-pick + loupe + box-average are all
  decades-old technique. +7 tests (`test_color_sample`, 1652→1659 green).
  **⭐ User visual/interactive pass OWED** (magnification/radius/grid feel; the doc §5 lists the checks).

### Phase 4 — Vector & text

**S25 — Vector layer infrastructure.** Path/shape model, fills/strokes, GPU tessellation +
rendering, hit-testing. **Design in `docs/vector-model.md`.**
- *Covers:* adding vector elements to the layer stack.
- *Status (2026-06-23) — CPU path DONE:* `src/core/vector/` value model (geometry/paint/object;
  one `LayerKind::Vector`, one object per layer; gradient = a paint), the `flatten()→Contours`
  **seam** + `samplePathAt` arc-length walk, hit-testing, and a CPU scanline rasterizer (analytic
  AA; solid + linear/radial/conic gradient fill; full stroke = Butt/Round/Square caps,
  Miter/Round/Bevel joins, dashes) feeding a **float-native** `rasterizeObjectF`. `VectorLayer`
  carries one `vec::Object`; `SetVectorObjectCommand` is the undoable authoring edit; the compositor
  renders vector layers at **target resolution** (crisp at any zoom). **Polish DONE (2026-06-23):**
  stroke Inside/Outside alignment (coverage-clip of a double-width centred stroke) + bbox-limited
  rasterization (`CoverageBuffer` sub-rect + `pixelBoundsOf`). **The GPU-resident renderer**
  (stencil-then-cover / Loop–Blinn — the doc's "ceiling") is **folded into S60**: its payoff is
  no-readback residency, a property of the compositor (not yet GPU-resident).

**S26 — Shape tool.** Rectangle/ellipse/polygon/star/line presets; **Shift/Ctrl/Alt**
(square↔rect, circle↔ellipse, from-center, etc.).
- *Covers:* shape tool with modifier constraints.
- *Status (2026-06-23) — S26-a (authoring) DONE:* the Shape slot now carries 5 variants
  (rect/ellipse/polygon/star/line, each with real option sets). A drag authors a parametric
  `vec::Object` via the pure `ui::buildShapeDraft` (size in the params, rigid placement transform;
  Shift = square/circle/45°-line, Alt = from-centre), shown live as an outline overlay, committed
  as a new `VectorLayer` on release. **S26-b (next):** pick an existing shape + the parametric
  resize-vs-transform gizmo toggle.

**S27 — Line tool.** Vector line with **custom stroke** (width/caps/joins/dash).
- *Covers:* line tool with custom stroke.

**S28 — Pen / custom path tool.** Bézier authoring, edit handles, **custom stroke**;
convert/boolean ops.
- *Covers:* custom path tool with custom stroke.
- **LANDED 2026-07-28, in two passes the same day** (§10). Pass 1 (`c03a0b5`): authoring, editing,
  the custom stroke — booleans held back. Pass 2, same day: the **boolean kernel**
  (`core/vector/boolean.*` + the `BooleanCompound` `Geometry` alternative, `Layer ▸ Combine Paths`)
  and the **chrome rebuild** onto its own overlay lane. Both are written up in
  `docs/vector-model.md` — §8 for the tool and its chrome, **§9 for the booleans** (model, the
  flatten seam, the snap-rounded kernel, the version-stable serialization, and the known limits).
  **Divide** — the one boolean that yields multiple objects — is still out.

**S29 — Type tool (core).** Rich text layer: system fonts (FreeType+HarfBuzz), **AA modes
incl. off**, per-run styling (bold/italic/strikethrough/color/font/size), alignment. **Design
in `docs/type-tool.md`** (settled 2026-06-25; the tool is split across as many
sessions as it needs — this is make-or-break).
- *Covers:* typeface tool (AA options, per-part styles), Type-layer in stack.
- *Split:* **S29-a** model + shaping (HarfBuzz) + render (FreeType outline → `flatten`→`Contours`
  → the S25 vector rasterizer), the **cross-platform `FontDB`** (OS-default family + OS fallback
  cascade — *no hardcoded names*) and **colour-glyph/emoji** render, headless; **S29-b** on-canvas
  live editing (caret/selection, a `TextEditSession` that owns focus + IME so the chrome doesn't
  eat keys; bidi-aware caret) + the **rotating I-beam cursor** (reuses the Move rotate-cursor +
  `xterm.svg`) + authoring (drag = **Area** / click = **Point** at the slider size — the
  Affinity-annoyance fix — with a Settings toggle); **S29-c** the bottom-right transient **Type
  panel** popup (no comic bubble; = the PLAN's Character/Paragraph panel, relocated) + the
  context-bar split (5 hot controls) + variable-axis/OpenType-feature/optical-kerning/AA controls +
  the **emoji-font picker** in Settings.

**S30 — Type tool (advanced 2D).** **Bend/warp via a handle**, **fit-to-path with on-canvas
start/end/flip range handles** when dragged onto a path layer (both reuse `samplePathAt`),
right-click **Rasterize / Convert to path**.
- *Covers:* text bending handle, fit-to-path + path-range handles, rasterize/convert-to-path.
- **LANDED 2026-07-05 → 07-07, hardened through 07-29** (§10 S30; `docs/type-tool.md` §9/§9.1). Both
  halves did reuse `samplePathAt` as designed, and the path reference being a `LayerId` rather than a
  copy is what makes editing the path re-flow the text. Two corrections are load-bearing and were not
  foreseen here: **an Area block's bend is the FRAME's arc, not the text span's** (so two type sizes in
  one box bend identically, and the overset clip is the warped sector rather than a flat rect), and
  **every piece of bent chrome now goes through one named mapping** (`BentArc::warp`, bitwise identity
  when unbent) after handles were found to be expressed against the flat box while the glyphs were not.

**S30-c / S30-d — 3D type (its own subsystem).** Settled 2026-06-25 (`docs/type-tool.md` §10):
a **real extruded mesh** (Contours → cap + side walls + bevel, one watertight solid → no
z-fighting) rendered through Vulkan with a **perspective camera, quaternion any-axis rotation,
and a real (toggle-gated) light model**, to an offscreen target the compositor samples as a
normal layer. **Per-run material** (not per-run depth) is the z-fight-free middle ground. Adds
`Vec3/Vec4/Mat4/Quat` to `common`. **S30-c** = engine (geometry + render); **S30-d** = the mini
3D-scene UI (handles) in the Type panel + the on-canvas 3D transform gizmo + light controls.
Designed **GPU-residency-aware** (mesh resident; rotate/light = uniform updates) and carries
per-vertex design-space UVs so the **per-face Layer Effects** (S30-e, below) drop in.
- *Covers:* 3D typeface (custom orientation, lighting), the 3D scene/transform UI.

**S30-e — 3D-text Layer-Effects integration.** Per-face effect mapping (effects evaluated in the
glyph's 2D design space, applied to the **front face**; optional wrap-to-sides) + wiring 3D text
into the Layer-Effects pipeline (gradient overlay / stroke / drop shadow / glow). **Scheduled, not
"future"** (user 2026-06-25); **paired with the general Layer-Effects feature** (its prerequisite —
that broader panel still needs its own slot when its phase arrives). The 3D mesh already carries the
design-space UVs this needs (`docs/type-tool.md` §12).
- *Covers:* the per-face Layer-Effects behaviour for 3D text.

**S30-b — Vector document type (deferred; user-requested 2026-06-23).** A second **document
*type*** alongside today's **Raster** document: a *Vector* document whose canvas is
resolution-independent and which exposes **only vector operations/tools** (no raster grid, no
brush/raster layers). The motivation: in a Raster document a vector layer is composited into the
document's fixed pixel grid, so **zooming the viewport magnifies pixels** — correct for a raster
document, but exactly what a Vector document must avoid. A Vector document re-renders its geometry
at *view* resolution, so it stays crisp at any zoom (the "why use Inkscape when I can do this 10×
faster here" pillar — `docs/vector-model.md` §1). The §2 data model is already document-type-neutral
(geometry is resolution-independent; `flatten()` serves any target), so this is a *document/canvas
policy + UI* feature, not a model rewrite.
- *Deliberately sequenced LATE despite the number — it is gated on two later stages:*
  - **`.mosaic` format (S48):** the one native format must round-trip **both** Raster and Vector
    document types (a `documentType` field in the manifest; a Vector document stores no flattened
    pixel canvas). Resolve the schema there. See §3.16.
  - **New-Document redesign:** the New-document dialog (S9) gains a **document-type chooser**
    (Raster vs Vector) up front, which reshapes its options (a Vector document has no bit-depth /
    background-fill raster knobs). That redesign is the natural slot to introduce the chooser; this
    session owns the canvas/tool-gating behind it.
- *Covers:* a resolution-independent Vector document type (crisp-at-any-zoom canvas; vector-only
  tooling), distinct from the Raster document type. Cross-refs: §3.7, §3.16, S9, S48.

### Phase 5 — Layer effects, filters, masks

**S31 — Layer masks + Mask from selection.** Raster mask edit/paint, link/unlink,
**Mask from selection** (wires the Select-menu entry).
- *Covers:* layer masks, mask from selection.

**S32 — Non-destructive adjustment/filter-layer framework.** Filter menu inserts a filter
layer **above the selected layer**, applying **downward within the group** (or globally if
ungrouped); reorder/toggle/edit params live.
- *Covers:* non-destructive layer filters via Filter menu with the specified scoping.

**S33 — Filters: blurs.** Gaussian, box, motion, radial, lens — Vulkan compute. Golden
tests.
- *Covers:* "several different kinds of blur filters".
- **LANDED 2026-07-17 — `docs/blur-filters.md`, seven kinds not five** (§10 S33). Gaussian / Box /
  Motion / Radial (Spin + Zoom) / Surface / Lens / **Depth of Field**, as S32 filter layers rather than
  destructive filters, with the golden tests this line asked for plus the one that actually pins the
  design — `region == crop(full)` byte-exact, which is what makes a *non-local* kernel safe under a
  scoped recomposite. The Vulkan compute lane landed with them and **refuses Box/Motion/Radial on
  purpose** (readback costs more than the kernel), so "Vulkan compute" here means four kernels, not
  seven.

**S34 — Filters: color.** Levels, curves, brightness/contrast, hue/sat, color balance,
**grayscale**, invert, threshold; **shadows/highlights**, **defringe / chromatic-aberration fix**,
**matte removal** (remove white/black matte, divide/multiply by alpha — compositing fixes poached
from Affinity), **haze removal**, and **Frequency Separation** (split low/high frequency for
non-destructive retouching).
- *Covers:* grayscale + color adjustments, plus the Affinity-poached photo/compositing fixes.
- *Deferred:* **Frequency Separation** — it creates layers rather than grading a backdrop, so it
  needs its own command + group/blend convention (docs/adjustment-layers.md §8).

**S34-a — the cheap, high-value filter remainder (user-picked 2026-07-29).** The Curves editor's
**backdrop histogram** (`HistoStrip` already exists in `adjustment_panel.cpp` and already draws the
adjustment's own backdrop for Levels/Threshold — this is a reuse, not a new widget), **Gradient
Map** (the gradient model + editor already exist; storage follows Curves' indexed-doubles
precedent), **Vibrance**, **Photo Filter**, **High Pass**. Plus **canvas gizmos for the Vignette
and Wave/Ripple centres**, which S35 left settable only by number sliders while Radial Blur and
Depth of Field get on-canvas handles from the same pipeline (that half of S34-a is canvas work and
rides with the input session).
- *Covers:* the two half-finished things S34/S35 left behind, plus the four filters with the best
  value-per-unit-work in the gap.

**S34-b — the rest of the filter gap (scoped 2026-07-29, not started).** What the S34-a bundle
deliberately left, in the order recommended:
- **Color Lookup (3D LUT).** The highest-leverage single addition left: a `.cube` / `.3dl` reader
  plus tetrahedral interpolation makes every existing film-look pack usable in Mosaic, and it ties
  into the lcms2 colour-management story. Needs a parameter that references a FILE, which Curves'
  indexed-double storage does not cover — the design question is whether the LUT is embedded in the
  document (portable, big) or referenced (small, breakable); decide before building. Read-only
  access to a user path, so the never-write-user-files rule is untouched.
- **Channel Mixer** and **Selective Color.** Pro colour tools; per-pixel, no new machinery, wide
  schemas.
- **The distort family — Twirl, Spherize/Pinch, Polar Coordinates, Displace.** S35's Wave built the
  displacement-and-resample frame, so each is close to incremental on top of it.
- **Lens correction (barrel/pincushion).** The natural sibling of Defringe. ⚠ **INVARIANT: user-set
  only, never estimated from the image** — a hard constraint on this family, not an oversight.
- **Frequency Separation** (above) and S35's own deferrals: Crystallize, a median/dust-and-scratches
  method (cut for cost, not law — an honest median at radius 16 is O(r²·log r)/px/channel), and a
  **GPU lane** for the heavy stylize kernels (Oil Paint and Denoise at large radii on a 36 MP photo
  is where the CPU lane hurts; `setBlurRenderOverride` is the established seam — this may instead
  land as part of S60-e).
- ⛔ **NOT in S34-b, and the list is recorded so nobody adds them as "just another row":** Auto Tone /
  Auto Contrast / Auto Color, or any "Auto" button on Curves or Levels (nothing may *automatically
  derive* a curve from image statistics); deconvolution-style smart-sharpen / shake reduction;
  dark-channel dehaze; anisotropic Kuwahara; any estimated CA or lens correction. These are standing
  constraints on this area, deliberate and costly — not an unfinished backlog.

**S35 — Filters: artistic/stylize.** Sharpen/unsharp, noise/denoise, posterize, pixelate,
emboss, oil/wave, vignette.
- *Covers:* "artistic, etc." filters.

**S35-b — Mesh Warp + Perspective Warp (user-requested).** A live **deformation grid** (drag mesh
nodes; add/remove rows/cols) and a 4-corner **perspective warp**, applied **non-destructively** where
possible (re-render from source, magic-layer-style — §3.7). Modelled on Affinity's Mesh Warp /
Perspective tools. (Full *Liquify* — push/bloat/pucker brushes — is a separate backlog item, §12.)
- *Covers:* mesh warp + perspective warp (the warp functionality flagged from the Affinity study).

**S36 — Rasterize / Rasterize-down.** Right-click rasterize on layers; **rasterize-down on
an effect applies it to the layer below only**.
- *Covers:* rasterize on right-click; rasterize-down for effects.

**S36-b — Selective undo (research-first).** *(Added 2026-06, user request — the "I should have
done this differently 40 steps ago" rescue.)* Begin with `docs/selective-undo.md`. Remove a
*past* command from the history together with everything that depends on it, keeping the
independent work that came after. Mechanics: every command declares a **footprint** (layer id +
document region; structural commands name the layers they create/move/delete) — dependency =
footprint overlap or structural reference, conservative by design. Deleting an entry always
takes its **transitive dependent closure**, so the surviving suffix is independent of the
deleted set *by construction* — its baked old/new pixel state stays valid and replay is safe
(undo to the deletion point, drop the closure, re-apply the survivors; stable `LayerId`s keep
structural commands honest). The deletion itself is **one undoable meta-command** — this must
never be the app's only unrecoverable action. **UI** (in the S16-b History panel): a context
entry reading **"Remove edit…"** when nothing depends on it and **"Remove edit and N dependent
edits…"** otherwise (the closure is computed at menu-open, so N is always real); the
confirmation highlights exactly the rows that would go (a graph walk — row highlighting only,
no live canvas preview: replaying into a scratch composite per hover is the expensive
non-goal). **"Selective undo"** is the docs/marketing name; menus stay in plain words. Cost
honesty: footprints are metadata and the undo stack already stores the heavy state, so memory
is ~free; commit cost = one suffix replay, bounded by history depth (cheaper after S60-c
tiling).
- *Prereqs:* S16-b (the panel) + the footprint API on `core::Command`; deliberately sequenced
  **after** the non-destructive stack (S31 masks / S32 adjustments) — non-destructive editing
  absorbs most "fix the past" cases, leaving destructive raster work: a smaller, better-defined
  problem.
- *Verify:* unit tests on footprint overlap, closure computation, and **replay equivalence**
  (remove + replay == the document that never did it); the meta-undo restores byte-identically.
- *Covers:* selective undo / history surgery (user request, 2026-06) — a genuine differentiator
  (neither Photoshop nor Krita ships this).

### Phase 6 — Healing & inpainting (research-heavy) + scripting

**S37-a — Inpainting research note (research-first). ✅ DONE (2026-06-18).** `docs/inpainting-research.md`:
papers, the pluggable-backend architecture, and the He & Sun engine design. Outcome: the default is the
He & Sun offset-statistics solver; the PDE backend is Telea / basic Navier–Stokes and takes no other
scheme; PatchMatch's propagation+random-search loop is deliberately never implemented (§3.11).
- *Covers:* inpainting research note + the §3.11 engine constraints.

**S37-b — Inpainting engine + backend interface (+ PDE backend). ✅ DONE (2026-06-18, commit `c336757`).**
`src/core/inpaint/`, namespace `mosaic::core::inpaint`: `InpaintEngine` (backend registry + dispatch +
`makeDefaultEngine()`) + `IInpaintBackend` / `InpaintRequest` / `InpaintResult` / `Params`, over
`common::ImageF` + `core::Selection` (coverage>0 == hole). First concrete backend `PdeBackend` — the
classical filler, currently **harmonic/Laplace diffusion** (Gauss-Seidel); the
sharper **Telea Fast-Marching** kernel is a planned drop-in *in this same backend*, **not yet written**.
Inert `ScriptBackend` shim landed for the S40 Lua hook. Tests use analytic invariants (`test_inpaint.cpp`);
golden-image diffs arrive with S37-c. **Not wired to any UI/tool yet (S38/S39); not user-visual-passed.**
- *Covers:* inpainting engine API + pluggable backends + the classical filler.

**S37-c — Default backend: He & Sun offset-statistics graph solver. 🔶 STAGE 1 DONE (2026-06-18, commit `da7ee70`); graph solver REMAINING.**
The research-derived quality default (`OffsetStatisticsBackend`), built clean-room from He & Sun (ECCV
2012 / TPAMI 2014), translation-only, isolated to one backend.
- ✅ **Stage 1 — offset statistics:** `offset_statistics.cpp` `computeDominantOffsets()` — per-known-patch
  nearest-neighbour (brute-force, **no PatchMatch**) under `|s|>tau` → dominant offsets by frequency.
  Tested via a periodic-image invariant.
- ✅ **Stage 2 — working-region extraction:** `working_region.cpp` `extractWorkingRegion()` — hole-bbox×3
  crop + box-average downsample to the `maxRegion` budget (offsets rescale by `scale`). Tested. (2026-06-18)
- ✅ **Stage 3 — graph cut:** `graph_cut.cpp` — `MaxFlowGraph` (from-scratch Dinic's max-flow/min-cut)
  + `alphaExpansion()` multi-label solver (Kolmogorov–Zabih pairwise terms, submodular-truncated, swept
  to convergence). Our own code. Tested analytically (known energy optima). (2026-06-18)
- ✅ **Stage 4 — copy-by-labels:** `applyOffsetLabels()` builds the completed image from a labeling
  (out(x) = image(x + offset[label])); tested (period-offset fill reconstructs a periodic image). (2026-06-18)
- ✅ **Stage 5 — solver assembly + backend:** added a **per-edge pairwise-cost overload** of `alphaExpansion`
  (position-dependent seam costs via the KZ construction); `graph_completion.cpp` `graphComplete()` builds
  the He & Sun MRF (validity data t-links + seam-coherence n-links over the K offset labels) → α-expansion →
  copy-by-labels; `OffsetStatisticsBackend` wraps it and is **registered** in `makeDefaultEngine`. End-to-end
  tested: a periodic image's hole is reconstructed exactly. (2026-06-18)
- ✅ **Stage 6 — working-region wiring:** `OffsetStatisticsBackend` gathers offsets on the cropped
  (+downsampled) working region around the hole (offset NNF cost now scales with the hole neighbourhood,
  not the whole image; offsets rescaled by the downsample factor); the fill stays full-resolution and
  hole-bounded. Tested on a hole embedded in a larger image. (2026-06-18)
- ✅ **Stage 7 — KD-tree NNF + default flip:** a hand-rolled exact KD-tree (Bentley 1975, public domain;
  our own code, no nanoflann) replaces the brute-force NNF in `computeDominantOffsets`, so offset-gathering
  no longer scans all patch pairs; `offset-stats` is now the **engine default** (`PdeBackend` kept as the
  fast fallback for tiny scratches). ASan/UBSan clean. (2026-06-18)
- **S37-c is COMPLETE, including all quality polish** — the He & Sun offset-statistics backend is the
  engine default and works end-to-end: offsets via hand-rolled KD-tree NNF on the cropped/downsampled
  working region → α-expansion graph completion (validity + hole↔known **boundary seam terms** + seam
  coherence) → copy-by-labels → **plain Poisson seam blend** → with a **two-scale** coarse graph cut for
  large holes. The graph-cut + gradient-domain-blend **combination** follows Interactive Digital
  Photomontage (2004) and plain Poisson blending; ⚠ **deliberately excluded and to stay excluded:**
  the quadtree Poisson variant, object-symmetry completion, any video extension, and all
  PatchMatch/patch-optimization.
  Entry-point wiring (Heal / brush / Edit→Fill→Inpaint) is S38/S39.
- *Covers:* the high-quality object-removal backend (the research note's recommended default).

**S38 — Stamp / Clone tool. BUILT 2026-07-29 — see `docs/clone-stamp.md`.**
- **NAMING (user 2026-06-19): this is a STAMP/CLONE tool, not "heal"** — select a source region
  (Ctrl) and stamp it over the target, for when you want to heal a brushed selection *without*
  inpainting (which is S39's job). "Stamp Tool" is a generic descriptive name (Photoshop's is "Clone
  Stamp"), not a trademark blocker.
- ⚠ **THERE IS NO SPOT/BLEMISH MODE (user 2026-07-29).** This slot used to read *"Heal tool
  (+ spot/blemish mode) … a single click auto-heals a blemish from surrounding pixels"*, with an
  anchor that **followed the cursor during the drag and snapped back on mouse-up**. **All of that was
  a mistake in this PLAN and is deleted.** The snap-back choreography described the *heal* tool this
  slot stopped being in 2026-06; one-click blemish healing is a healing operation, which is S39's
  job and sits behind S39's constraints. Neither is built and neither is owed.
- **As built:** `ToolId::CloneStamp`, shortcut **S**, its own toolbar slot in `PaintFill`, the
  pack's reserved `clone_stamp` glyph. Ctrl (⌘ on macOS) **+ click sets the SOURCE anchor** —
  crosshair cursor while held, a circle-and-diamond marker at the anchor (and at the live source
  point during a stroke). Painting stamps source→target through the brush tip at
  `offset = first stroke point − anchor`. **Aligned** (default on) latches that offset for every
  later stroke; **non-aligned** re-anchors each stroke to the same source point. **Sample** =
  Current layer / Current & below / All layers. Bar: Size · Opacity · Aligned · Sample (Hardness /
  Flow / Spacing / Smoothing secondary). One `SetLayerPixelsCommand` per stroke.
- **It reuses the S19-a stroke machinery outright** (`core::brush::BrushEngine`) — no second dab
  walk: the engine lays the stroke's ALPHA and `core::applyCloneStamp` replaces its deposit with
  source pixels over exactly the rect `composite()` reported. It reads the **PRE-STROKE snapshot**,
  never the live target (docs/brushes.md §6.6b), which is what makes the deposit idempotent and
  keeps undo replay and the incremental-refresh contract intact.
- ⚠ **INVARIANT — a clone stamp COPIES PIXELS.** No healing, no gradient-domain/Poisson seam
  blending, no texture synthesis, no "make it match the destination" step of any kind, ever. That
  family belongs to S39 and is constrained there. See `docs/clone-stamp.md`.
- *Covers:* the requested clone/stamp tool. (The heal tool the slot was originally worded for is
  superseded by the 2026-06-19 ruling; healing proper is S39.)

**S38-b — Red Eye tool.** A small targeted tool: click/drag over an eye to mask the red pupil and
desaturate + darken it (a **localized correction**, sitting in the retouch family — not a global
filter, since it needs per-eye targeting). Cheap; reuses the selection/mask plumbing.
- *Covers:* red-eye removal (Affinity-study item; implemented as a tool, not a filter).
- **LANDED 2026-07-28 — as a two-variant eye-retouch slot, not one red-eye tool** (§10 S38-b;
  `docs/red-eye-tool.md`). `ToolId::RedEye` is Tier 1 (flash red-eye) and `ToolId::RedEyeSclera` is
  Tier 2 (sclera de-redding / vein suppression); they share one toolbar slot and one flyout, which is
  what "universal eye retouch" cost over the cheap version scoped here. Two feedback rounds followed
  the same day — and round 2 established that **the flash rim was three separate defects**, only one of
  which round 1 had fixed (§9.8). Shortcut is **Y**, not the R this line's doc predecessor assumed:
  bare `r` is claimed by the canvas rotate.

**S39 — Inpaint brush + Edit→Fill→Inpaint.** Brush marks a region → engine fills; menu
fill uses current selection.
- *Covers:* inpaint brush tool, Edit→Fill…→Inpaint.
- **S39-a (Inpaint brush) DONE 2026-06-19.** New `ToolId::InpaintBrush` (shortcut **J**, own slot,
  PaintFill; placeholder bandaid icon). It **reuses the S19-a stroke machinery**: the stroke paints a
  translucent **red ~35 % overlay** marking the region (the brush engine's coverage buffer doubles as
  the mask — `BrushEngine::coverage()`); on **mouse-up** the overlay is dropped and the brushed region
  becomes a hole `Selection` (coverage > 0.1) fed to **`InpaintEngine::run`** (offset-stats default),
  landed as one `SetLayerPixelsCommand`. Engine owned by `MainWindow` (`makeDefaultEngine`), run
  **synchronously** on release (brushed regions are small + the backend crops to a working region).
  `VulkanCanvas::commitInpaint` host hook; tests in `test_brush_engine.cpp` (coverage accessor + the
  coverage→mask→engine glue).
- **S39-b (async run + live preview + cancel) DONE 2026-06-20.** The engine runs off the UI thread with
  a status-bar progress bar + cancel + throttled live preview; the canvas stays navigable; Settings →
  Inpainting category landed alongside. (Adaptive small-selection region + sample-area preview followed,
  2026-06-22.)
- **`Edit→Fill…` (incl. Inpaint) — SHIPPED 2026-06-22; follow-ups A–F all DONE 2026-06-22.** `Edit→Fill…`
  (**Shift+F5**) opens a **Fill dialog** (`ui::fill_dialog.*` + `core::FillCommand` + `render::computeFill`).
  It was deferred (user 2026-06-19/22) until the dialog/modal design language settled; now built.
  **SHIPPED (2026-06-22, two passes):** the transactional modal + two-column layout; Contents =
  Foreground / Background / White / Black / 50% Gray / **Color…** / **Inpaint** (a divider separates the
  solids from Inpaint); Blending (Mode + Opacity) + **Protect alpha**; one undoable `core::FillCommand` via
  `pushScopedPixelEdit`. **Pass-2 polish (user feedback 2026-06-22):** the preview pane is now
  **document-context-aware** — `host.compositePreview` temporarily applies the candidate pixels and
  `render::compositeRegion`s a padded ROI, so it reflects ALL layers + the active layer's own blend
  mode/opacity (was a useless isolated-layer preview); the **Mode + Contents dropdowns use the themed
  `Dropdown` with FL_MENU_DIVIDER family dividers** (divider support added to `ui::DropdownPopup` +
  the shared `addBlendModeItems`, also applied to the **layers-panel blend dropdown**); the **Fill
  button is the rounded `FlatButton`-derived `FilledButton`** (matches Cancel); and **Inpaint now has
  an in-dialog `Preview` button** that runs the engine once, **caches** the result in the pane, and
  **Fill commits the cache without re-inpainting** (no cache → falls back to the async path).
  **Follow-ups A–F DONE (2026-06-22; see the `fill-dialog-followups` memory):** **(A)** the dialog hosts
  its own `DropdownPopup`/`ContextMenu` so its combos are themed (with the dividers); **(B)**
  `DropdownPopup` is fixed-size + scrolls internally (wheel + themed grab) for long lists; **(C)** the
  opacity-slider preview is frame-coalesced; **(D)** the **Color…** content opens a compact **`ColorFlyout`**
  speech-bubble child sub-window reusing the picker's three surfaces (`color_surfaces.hpp`) + shared
  `ui::HexField` + live swatch; **(E)** Protect-alpha tooltip; **(F)** the in-dialog inpaint Preview drives
  the status-bar progress + a pane note (in-dialog Esc cancels; full async-into-pane still optional). The
  **original colour picker** also gained the comic-book bubble (triangle at the swatch, balanced margins;
  `Fl_Window::shape()` for true transparency on X11/Xwayland, plain panel on native Wayland where shape
  can't cut it). **Pattern** waits for **S21** (no patterns yet — user 2026-06-22). **Settled design
  (2026-06-22):**
  - **Transactional modal** (OK/Cancel; NOT the Settings-style modeless instant-apply): set up → **Fill**
    applies once / **Cancel** discards. Enter=Fill, Esc=Cancel. Targets the current **selection** (whole
    active layer if none). Reuses the themed `Dropdown`/`Slider`/`ColorSwatch` + the crop accent-filled
    primary button + the Settings panel framing.
  - **Two-column layout (user 2026-06-22):** **left** = Contents + its contextual block + Blending +
    Options; **right** = a **preview pane** (top) with the **`Preview` button directly under it** (shown
    only for Inpaint). Footer stays a clean `[ Cancel ] [ Fill ]`. The pane shows the **affected region
    zoomed to fit with padding** — the selection bbox **plus a margin of surrounding pixels** for context
    (not the bare affected area), aspect-fit. **Single result view** — no before/after split (too busy;
    user 2026-06-22; Cancel reveals the original).
  - **Contents** (`ui::Dropdown`): Foreground / Background / Colour… / **Inpaint** / Pattern… / White /
    Black / 50% Gray, with a small **contextual block** below that swaps per choice (swatch preview;
    Colour… → a `ColorSwatch` chip opening the picker popover; **Inpaint** → a one-line note + the
    `Low-effort on small selection` checkbox + an "Inpaint settings…" link to Settings→Inpainting —
    NOT the full backend params; Pattern… → **greyed "(with S21)"** for now). **Label is just "Inpaint"
    — never "Content-Aware"** (Adobe trademark; user 2026-06-22).
  - **Blending:** Mode (`Dropdown`, default Normal) + Opacity (`Slider`, 0–100%). **Options:** a single
    **Protect alpha** checkbox (a.k.a. "preserve transparency / lock transparent pixels" — restrict the
    fill to pixels the layer already has, alpha>0; independent of the Opacity slider; same concept as the
    brush engine's planned Protect Alpha and the layer alpha-lock — **use one consistent term app-wide**).
  - **Preview lives in the dialog pane, split by cost (user 2026-06-22).** The preview is the **in-dialog
    pane**, NOT (only) on-canvas — opening the modal may dim/grey the parent and obscure the canvas, so the
    pane is the canonical preview. **Cheap Contents** (solid colour / Foreground / Background / White /
    Black / 50% Gray / Pattern) update the pane **live** as Colour / Mode / Opacity / Protect-alpha change
    (cheap, so auto-running is free; no button). **Inpaint** is too expensive to auto-run (memory per
    selection + blend-stage hiccups + a tiny preview = small gains), so a **`Preview` button under the
    pane** (shown only when Content=Inpaint) runs the engine **once** via the S39-b async path (status-bar
    progress + cancel), shows it in the pane, and **caches** it; **Fill then commits the cached result
    without re-inpainting.** Cache keyed on (selection, layer pixels, inpaint params); any change (toggling
    Low-effort, re-selecting, editing the layer) invalidates it, so the next Preview — or a Fill with no
    valid cache — re-runs the engine exactly once. **Inpaint greys out Mode / Opacity / Protect-alpha** (a
    reconstruction, not an overlay).
  - **One undoable command:** a small **`core::FillCommand`** (region + content descriptor + mode/opacity/
    protect-alpha) shared with **S21** bucket/pattern fill so History reads "Fill" once; the Inpaint
    content keeps its `SetLayerPixelsCommand` engine-result path. Build the dialog once; S21 reaches it.

**S40 — Scripting infrastructure (Lua via sol2).** *(Replaces the dropped "local inpainting
model" session — user 2026-06-17; see §3.11.)* Embed **Lua (sol2/MIT, vendored)** over the
existing command system: a **sane, documented, stable API** (document/layers/selection/transform/
filters/colour + the headless op-runner's command set), script load/run from the UI and
`mosaic --headless --script foo.lua`, sandboxed (no ambient filesystem/network beyond explicit,
opt-in host calls). **Ships an example script that registers a custom inpaint provider** against
§3.11's pluggable engine hook — the reference path for users who want ML inpainting (their dep,
their weights). Docs in `docs/scripting.md`.
- *Covers:* scripting/automation API (§11), and the user-extensible inpainting backend that
  replaces the cut built-in ML model.

### Phase 7 — File formats & color/HDR

> ⚠️ **The export half of this phase — S41, S42, and S56's dialog — is sequenced as MILESTONES M1…M7 in
> `docs/export-system-plan.md` §10, which is the ledger of record.** The scope pass (2026-07-04)
> deliberately re-cut the work by *shippable artefact* rather than by session number, because the
> registry, the loss system and the dialog are not separable into "framework then formats": each format
> needs the registry, and the registry is only provable through a format. Read §10 there before
> planning any file-format work; the entries below say what each session was *for* and where it landed.
> **M1–M4 are built** (Quick Export → PNG; the framework; the generated Export As modal; WebP / AVIF /
> TIFF / GIF + the EXIF write half). ⚠ **The `src/io` → `src/core/io` relocation that plan originally
> called for was made and then reverted** (`4b692c5` → `97cca43`): pure I/O under `core/` inverts the
> dependency, and `src/io/` is where the code lives and stays. Anything still naming a `src/core/io/…`
> path is describing a directory that does not exist.

**S41 — I/O framework + loss-warning system.** Format registry with capability flags; the
**export loss-warning dialog** (transparency/HDR/layers-flatten/vector-rasterize/…).
- *Covers:* the output-format loss warning (transparency/HDR/flatten/rasterize/etc.).
- **LANDED 2026-07-28 as milestone M2** (`c9a8a85`; §10 S41 and `docs/export-system-plan.md` §2/§4).
  Two shapes here turned out to matter more than the checklist: the loss system is a **pure `diff()`
  over capability rows**, so it is unit-testable against a table and a `diff()` that always warns
  *fails its own test* — and it is not a dialog at all but a live banner in the export modal, because a
  warning you must dismiss before you can act on it teaches nothing. And the warning text stays
  **untranslated English plus a stable `LossCode`** inside `core/io`, with the UI doing the translating,
  because the I/O layer is deliberately gettext-free.

**S42 — Common raster formats.** PNG, JPEG, TIFF, BMP, GIF, TGA, PNM, ICO, WebP (read+write
where sensible).
- *Covers:* core of "~GIMP-level format support".
- **MOSTLY LANDED across M1/M3/M4** (§10 S42). Built: PNG, JPEG, JPEG XL, WebP, AVIF, TIFF, GIF — each
  with its own `FormatCaps`, `OptionsSchema` and registry entry, every optional dependency gated at
  **runtime** rather than at packaging time (AVIF simply does not appear unless libavif offers libaom or
  SVT-AV1, which is how the never-rav1e decision gets enforced somewhere it cannot be undone
  downstream). Landing with them because those formats require them: EXIF write-back, ICC embedding on
  export, and a dependency-free quantizer. **BMP / TGA / PNM / ICO from this line are M5's**, together
  with PAM, QOI and Radiance HDR, in the curated `libmosaicformats` tier.

**S43 — HDR: float pipeline → linear-light compositing → formats + output.** Pre-split (2026-06) —
the two prerequisites flagged in §13 now live here, in dependency order:
- **S43-a — Float pixel pipeline.** Raster layers store the document's chosen precision (U8 / U16 /
  F16 / F32 — the S9 dialog's bit-depth choice finally becomes real storage, not just metadata); the
  compositor consumes native precision end-to-end (`common::ImageF` already exists); precision
  conversion on open/new. **Tiled storage deliberately stays S60-c** — float-ness and tiling are
  separable migrations.
- **S43-b — Linear-light + ICC compositing.** The compositor re-plumb §3.6 always promised: decode
  the working space → **linear light** (lcms2, building on S12-b), composite linear, re-encode for
  display. `render/blend.hpp` and `shaders/composite_blend.comp` change **together** (the §2 rule;
  GPU==CPU tests re-run), goldens re-baselined deliberately and documented in `docs/compositor.md`.
- **S43-c — HDR & high-bit formats + indicators + output.** OpenEXR, JPEG XL, AVIF I/O; the **HDR
  indicator** + **warn when the display/compositor lacks/has-disabled HDR**; the Vulkan HDR swapchain
  path (`VK_EXT_swapchain_colorspace` / `VK_EXT_hdr_metadata`). *Prereq:* the **native-Wayland
  canvas** for actual HDR *output* — XWayland/Xorg are SDR-only (§3.6); the file I/O, tone-mapping,
  and indicator/warning halves are backend-independent. ~~**The default presentation backend flips to
  native Wayland here.**~~ **It already did — in S59-a, 2026-07-28**, so this prerequisite arrives
  satisfied; what S43-c must still handle is that a user on the `FLTK_BACKEND=x11` escape hatch (or a
  pure-Xorg session) gets no HDR output, which is a case to *detect and say*, not to fix
  (§2 windowing note, §12, `docs/wayland.md`).
- *Covers:* HDR support + indicator + display-HDR warning, plus the float + linear-light foundations.

**S44 — Extended formats.** Optional HEIF (system codec only, §7), DDS, **OpenRaster `.ora`** import/
export, others to approach GIMP parity.
- *Covers:* remainder of "~GIMP-level format support".

**S45 — RAW import + camera-info panel.** LibRaw decode + demosaic options; panel showing
**exposure/aperture/ISO/focal length/lens/etc.**
- *Covers:* RAW support + camera info.

**S46 — PSD/PSB read (research-first).** psd_sdk-based reader → layer tree/masks/blend
modes/text/smart objects best-effort; `docs/psd-notes.md`.
- *Covers:* PSD/PSB read.

**S47 — PSD/PSB write.** Best-effort writer (layers/masks/metadata) with loss warnings for
Mosaic-only features.
- *Covers:* PSD/PSB write.

**S48 — `.mosaic` native format.** **BUILD 1 BUILT (2026-07-07/08); BUILD 2 BUILT + MERGED 2026-07-23
(`9e110d1`) — see the Build 2 subsection below. Both builds are done; the whole session is ticked in
§10.** Build 1: container layers (framing/codec/root+dir/RS parity `97cca43`), journal + commit-append Save + lineage salvage (`92a1ee3`), recovery-dialog copy settled + Restore icon (`05ccd38`/`5d8d8da`), document<->container bridge — full-fidelity all-kinds round-trip (`ca66c95`), File→Save/Save As/Open wired, corrupt-corpus generator (damage flows 3a-3e/4) + recovery dialogs live + save-history-in-panel (`2a717f5`/`a5029cb`/`b47d575`/`9781729`), **crash restore + journal autosave (flows 1/2) wired (`33d03ca`)**: app-owned idle-coalesced autosave under `$XDG_STATE_HOME/mosaic/recovery` (tombstone-aware per-state diff; reset-on-Save, discard-on-clean-close), open-time Restore/orphan faces composed after any damage face, untitled restore at app start; `corrupt_corpus --plant` writes the flow-1/2 fixtures + their journals (planted, not shipped); **§2.10 advisory lock (flow 6) wired**: an OS advisory lock (flock/exclusive-handle) on a recovery-dir file — never the user's document — so a second window opens read-only ([Cancel]/[Open read-only]; read-only skips the lock+journal and routes Save to Save As), and a dead holder's stale lock auto-releases into ordinary journal recovery; **commit-append File→Save wired (`0cb3e79`)**: an armed CommitAnchor {tip, baseline, walStart} (set on a clean open + after every full write, never for recovered/read-only/full-scan opens) lets Save append just the tombstone-aware diff since the last save (diffDocumentStates → one SaveState → appendSaveToFile, O(changed)); the full write stays for Save As / first save / a recovered-or-full-scan open, and a tail-check failure refuses loudly ([Cancel]/[Save a copy…]); and **efficient per-key LiveUndoModel wired (`b270b0e`)**: loaded save history no longer holds N full layer-tree snapshots — each content save becomes a per-key ui::LoadedDeltaCommand (its committed chunks paired with the value each key held below it, applied in place via io::native::applyChunksToDocument — O(changed)); a save that changes structure (a re-emitted manifest) or a mask keeps the proven whole-tree LoadedStateCommand. and **history-preserving auto-compaction wired**: once the append region's parity debt trips `needsCompaction`, THAT Save folds the file instead of appending — `buildCompactedCheckpoint` (io/mosaic/compaction) keeps the newest frame per (TYPE, KEY) as parity-covered current content and carries every frame it superseded, plus every `HIST` record, into the checkpoint as retained history, spliced byte-verbatim (spec 3.3 encode-once) while the Save's own edit takes a fresh generation; a parity-rebuilt frame has no on-disk extent and is re-encoded instead, making the repair permanent. The reader grew the surface this needs: `ReadReport.retained` + `lostHistoryEntries` (history carries no parity by design, so a rotted undo state is a status line, never the "damaged file" face) and frame extents on `RecoveredChunk`; `loadedStates`/`buildLoadedHistory` now source saved states from the checkpoint's retained history and the committed region alike, so a fold is invisible to the History panel. The compaction Save tail-checks before its full write — it replaces the very region a foreign writer's commits would sit in. and **the manifest is now REPLICATED** (`buildCheckpoint` writes it twice, far apart, after the parity chunks): a user feeding random corruption at a real file found that one flipped byte in the ~500-byte `MFST` frame — 0.07% of a 744KB document — made the file unopenable while all 1068 other frames stayed checksum-valid. Parity is the wrong tool for it (a stripe pads every shard to its longest member, so a large manifest would inflate its whole stripe, and it would still survive only `m` losses); a second copy costs the manifest's own size and survives a burst that takes a whole stripe. Readers treat two frames sharing (TYPE, KEY, GENERATION) as replicas of one chunk — the format's own rule that generation versions a key — so whichever verifies answers, losing one replica is not damage, and losing every copy is counted once by identity. Measured: a 4KB burst now opens 12/12 (was 11/12), a 64KB burst 12/12 (was 10/12), 0.1% scattered damage 8/8 (was 6/8). **Mosaic also takes a POSITIONAL file argument** (`mosaic file.mosaic`, no flag — how a desktop hands a program a file: dropped on the icon, "Open with", an xdg-mime association; `--` escapes a leading dash), which also makes the real app's open path drivable headlessly. and **the container-version gate** (§2.1/§2.3): `format_version` now rides in the root under its BLAKE3 checksum as well as in the unprotected preamble byte, and refusing takes TWO agreeing facts — a verified root naming a newer version, or *no chunk verifying at all* while the preamble claims one. So a rotted preamble byte is a non-event (the checksummed root wins), while a genuine future container whose framing changed is refused with "this file needs a newer Mosaic" instead of degrading into the recovery ladder and telling the user their intact document was destroyed — the one remaining item that could not have been retrofitted into readers already in the wild. A refused file is never replayed and never folded. FORMAT DESIGN NOW CLOSED. **What this paragraph once listed as remaining has since landed** (recorded here rather than left standing beside it): **S48-b** previews + Linux desktop integration BUILT 2026-07-22 (see its own subsection); **Build 2** — H4 + adaptive switching — BUILT + MERGED 2026-07-23; **Flatten History** (§3.7) and **journal growth compaction** (§2.6) both shipped inside Build 2; **macOS Quick Look** shipped as S58-e (thumbnail *and* space-bar preview). Still genuinely open: the **Windows write path** (**S57** — unimplemented, not merely un-compile-verified; see there), the §9 state-count retention cap (user ruling: unlimited, so nothing to build), and `Profile::Max` (implemented in the codec, selected by no writer). Spec:
`docs/mosaic-native-format.md` (supersedes the old ZIP-container sketch — see §3.16), narrative
research write-up `docs/mosaic-native-format-research.md`. Reader/writer: bespoke chunked
container (§3.16), **64px tiles** (empirically tested vs. 128/256px, not inherited from the
research prototype's untested 256px default), manifest + tiled layer data + effect graph,
**forward-compatible unknown-chunk preservation**, linear undo/redo history retained in-container,
round-trip golden tests.
- *Covers:* the open/extendable/maintainable/documented `.mosaic` format.
- *Also:* a `documentType` manifest field so the one format carries **both Raster and Vector
  documents** (§3.16) — settle the Vector-document schema here even though the type ships at S30-b.
- *Review round 2026-07-07 (Round 11, tested in the research harness before entering the spec):*
  autosave = **recovery-journal sidecar** (user hard rule — only explicit Save writes the user's
  file), explicit-link journal frames + honest salvage rules, structured chunk keys, checkpoint
  copy-through, advisory per-document lock. No format-level open questions remain.
- *Save-semantics round 2026-07-07 (Round 12, harness-tested):* **File→Save = commit-append**
  (atomic batch + `CMIT` frame; encode-once at `balanced`), full write reserved for Save As /
  first save / threshold compaction; journal binds at commit granularity; salvage + full-scan
  proven on appended files (incl. the generation-rule tie demonstration); autosave cadence
  policy grounded in a measured 64px write-volume number.
- *Build 1:* container + recovery journal/lock/salvage + linear H2 history + Paeth (real SIMD
  decoder) + the interactive undo/redo hot path. *Build 2:* H4 content-addressed history +
  adaptive H2↔H4 switching — needs NO telemetry
  (churn is measured locally per-document at checkpoint time), sequenced second purely to keep
  Build 1 small. *Not in the build plan* (real, tested, available if ever needed): xor-delta history
  compaction (H3), history-region Reed-Solomon parity — see the spec doc §3.8/§3.9/§6.

### Phase 8 — UX polish & infrastructure

**S49 — Tabbed document selector.** Open-file tabs with an **X to close** and a **save/
discard/cancel** dialog on unsaved changes.
- *Covers:* tabbed open-file selector with X + save/discard dialog.

**S50 — Drag & drop + magic layers.** **Drop on tab bar → open as document**; **drop on
canvas → add as a Magic layer**; `File → Open as layer…`; magic-layer resampling from
original on transform.
- *Covers:* drag-drop behaviors, "all files opened as layers are magic layers".

**S51 — Keybindings + settings UI.** Remappable shortcuts whose **defaults are the shortcuts Mosaic
already has** — harvested from the existing menu accelerators and the plain-letter tool keys in
`kToolDefs`, so enabling the feature moves nothing until the user rebinds it (**user ruling
2026-07-29**, superseding this section's original "Photoshop-like defaults": adopting another
product's map would silently reassign keys that already work here, and the accelerators the app
ships with are the ones its own documentation and muscle memory refer to);
settings UI (theme, performance, language). **Split (user 2026-06-14): S51-a =
settings UI, PULLED FORWARD to next** (the settings backlog — S15-e, S16-q, S16-p, S18-d, theme
picker, units — is stacking up; build the surface now and land settings as we go); **S51-b =
keybindings** (stays here). S51-a must design a **coherent IA** first (see `settings-dialog-coherence`):
a likely **Tools/Crop** group (S16-p + S16-q + S15-e), an **Appearance** group (theme mode + units),
and **Annoyances** (S18-d). S15-e/S16-q also want **small per-option diagrams** (like the in-chat
previews). The picker also needs the runtime re-theming pass below.
- *Also (user-reported 2026-06):* **runtime re-theming** — today a light/dark change (in-app or
  OS-side) needs a restart. `applyTheme()` is idempotent, but many widgets *bake* palette colours
  at construction (window/menu/scroll/button `color(...)` calls), so S51's theme picker needs a
  re-theme pass: a broadcast hook (or rebuild) that re-applies baked colours + redraws, plus the
  canvas clear colour. While mode = System, **follow the OS live** (poll
  `platform::detectSystemTheme`/accent every few seconds from the frame timer, or a KDE/portal
  signal if cheap) — re-theming on the fly is the same machinery the picker needs anyway.
- *Covers:* changing keybindings (defaults = the app's own existing shortcuts), settings surface,
  runtime/live theme switching.

**S52 — Icon system finalization + credits.** Replace the colored *placeholder* tool icons with the
final **colorful, illustrative, self-describing** icon set per the §3.13 identity (Affinity-class;
GIMP-legacy-friendly but more polished — **not** flat/monochrome). Complete tool/UI icon coverage
(bespoke color SVGs, DPI runtime render), `docs/credits.md`, license aggregation.
- *Covers:* icons for all tools/UI elements (colorful, descriptive — the §1/§3.13 identity).
- **LANDED 2026-07-10 — as a PACK SYSTEM, which this entry did not ask for** (§10 S52). A pack is a
  folder holding `mosaic_icon_pack.json` (identity + credits; the file *is* the marker) and one SVG or
  PNG per stable tool key, with per-icon fallback to the embedded default so a **one-icon pack is
  legitimate** — the design that makes "replace the placeholders" a user-facing capability instead of a
  one-time commit. Packs are **tools only**: the §3.13 scope note holds, and the chrome/dialog icons
  remain the one-ink tinted set from S16-g. The default ships as **"Smalti"** (GIMP's colour tool icons
  under CC-BY-SA-4.0 with Mosaic additions and reworks), which replaced the bespoke **"Tesserae"** set
  landed the same day; `docs/credits.md` carries the per-icon provenance and the licence aggregation.

**S53 — Image operations + menu completion.** Split (2026-06) — the "Image menu" was hiding real
engineering (resampling quality) inside a checklist session:
- **S53-a — Image & canvas operations.** **Image size** (resample with selectable quality: nearest /
  bilinear / bicubic / Lanczos — a shared `render/` resampler that S15 transforms and S50
  magic-layer resampling also use), **canvas size** (9-point anchor), rotate 90°/180°/arbitrary,
  flip H/V, **trim to content**. Whole-document commands (every layer/mask/vector transform updated;
  undoable); golden tests per resampler kernel.
- **S53-b — Menu completion.** Flesh out File (Save/Save as…/Export…/Open as layer…), Edit, Layer,
  Type, Select, Filter, View, **Help→About** — wiring existing functionality; no new engines hide here.
- *Covers:* full menu bar incl. File Save/Save as/Export, Help→About; the §11 Image-menu item.
- **LANDED 2026-07-28** (§2, §10). The shape of the answer was not "six new commands" but **one
  generalised engine**: `render::buildCropCommand` already was a canvas-resize-with-anchor-and-rotate
  command builder, so it became `render::buildDocumentRemapCommand(doc, newW, newH, worldToNew, …)`
  and Crop became a wrapper over it — every document op then inherits the group push-down, the
  masked/singular-group rule, the canvas-locked texture rule, the delete-mode bake, the expansion
  fill and the one-undo-step guarantee for free, instead of six copies of five special cases. The
  "shared `render/` resampler" the spec asked for is now literally shared: `render/resample.*`,
  lifted **verbatim** out of `compositor.cpp`'s anonymous namespace. Two things the spec did not
  anticipate: **guides were stranded by every crop** (a pre-existing bug, fixed here via
  `core::SetGuidesCommand`), and the **selection** had to learn to ride along
  (`Selection::remapped` for the lossless grid ops, `Selection::scaled` for Image Size).
  `docs/image-operations.md` is the reference, including the two decisions worth re-reading before
  changing anything: why `buildOrientCommand` takes **no** `ResampleFilter`, and what Image Size
  bakes versus what it carries by transform.

**S54 — i18n completion.** Catalog extraction in CI, runtime language switch, RTL/CJK
verification, contributor guide for translations.
- *Covers:* i18n made easy to extend.
- **LANDED 2026-07-24.** 74 language catalogs — Krita's set minus `tok` (a 137-word constructed
  language cannot carry "Levels" without coining vocabulary; left to someone who speaks it).
  - **Layout:** `po/<lang>/mosaic.po`, KDE/Krita style. Invariant: *files in `po/` are templates,
    directories are languages* — which is why `po/motivate/` became `po/motivate.pot` + `motivate.md`.
    `po/LINGUAS` + `Plural-Forms` generated from `tools/i18n/languages.py`.
  - **Coverage is deliberately partial:** the core UI (633 of 1020 msgids) — everything except the
    Texture Generator dialog and the Settings long-form help. Untranslated entries fall back to the
    English msgid, so a partial catalog is a first-class thing, not a broken one. Widening is
    `core_worklist.py --all` plus another pass; no machinery changes.
    39,883 strings landed: 57 languages near-complete (≥600/633), 9 partial (ia cy fy ga kk nds oc
    ug br), 7 high-confidence-subset-only (tg km hne mai wa se xh), plus en_GB at 28 by design.
    Where a language has no established graphics vocabulary, inventing one is worse than English —
    a wrong term must be *noticed* before it can be fixed.
  - ⚠ **Menu translation is all-or-nothing.** FLTK joins sibling menu items by their shared parent
    text, so a half-translated tree grows TWO File menus each holding half the items — and msgfmt
    sees nothing wrong, since both entries are individually valid. Hence `check_menus.py`, and
    hence the thin languages translate zero menu paths rather than some.
  - **Pipeline (`tools/i18n/`):** translators write `id<TAB>text` TSV; `assemble_po.py` owns all
    `.po` framing. Bulk-translating into raw `.po` is where catalogs get corrupted (a dropped
    continuation line, an unescaped quote), and this removes that failure mode entirely.
  - **The safety gate earns its keep.** Reordering `%zu … %s` into `%s … %zu` for natural word
    order is the commonest thing a translator does to a format string and the most dangerous —
    printf then pops a `const char*` where the caller pushed a `size_t`. A multiset check waves it
    through; `msgfmt --check` caught it in ja/ru/uk. `fix_format()` now rewrites such cases into
    gettext positional form (`%1$zu`, `%2$s`) rather than rejecting a good translation, and
    `msgfmt --check` runs on every catalog at build time as an independent second opinion.
  - **`$MOSAIC_LANG`** switches the UI language for one run (`MOSAIC_LANG=ca@valencia:ca`), messages
    only. It exists because `LANG=ja_JP.UTF-8` does nothing on a box that never generated that
    locale. ⚠ The trap: gettext ignores the selection when LC_MESSAGES is `C`, glibc decides that
    from the *applied* locale (not the environment), and `C.UTF-8` is excluded too — so the fix is
    a guarded `setlocale` retry that must never downgrade a locale that already worked. Regression
    test in `tests/test_i18n.cpp`.
  - **RTL is text-level only:** FLTK 1.4 links Pango/HarfBuzz/FriBidi so ar/fa/he/ug shape and
    reorder correctly *within a label*; widget layout is not mirrored. Separate work.
  - ⚠ **Machine-assisted first drafts.** Usable, not finished; each catalog header says so.
    Native-speaker review is the expected next step. Interactive visual pass owed.

**S55 — Welcome screen, recent files, autosave/crash recovery.** *(recommended addition)*
Start screen (new/open/templates/recent), autosave to temp `.mosaic`, crash recovery.
- *Covers:* recommended additions (§11) improving robustness/UX.
- **LANDED 2026-07-22 — but as the New-Document dialog redesign, not a separate welcome screen
  (user ruling).** Autosave + crash recovery had already shipped with S48 (journal autosave,
  restore flows); the remaining welcome-screen role (recents with previews, templates, new-from-
  clipboard) folded into File→New — see S9's redesign note + the §12 S9-follow-ups block. The
  template documents themselves (`data/presets/*.mosaic`) are still to be designed.

**S48 Build 2 — H4 + adaptive switching: design rulings RESOLVED 2026-07-22 (user consult;
agent dispatched).** The §3.9 parameters stand as ruled earlier (hysteresis `switch_up=0.35` /
`switch_down=0.15`; retention unlimited ⇒ the churn window is the WHOLE retained history — the
research's wrong-window bug must be test-pinned). New rulings this session: **(1) PROACTIVE
early fold** — a plain Save may choose a full-write fold to realize a mode switch early, gated
on projected benefit (a reuse fraction AND an absolute-bytes floor) **and a document-size/time
gate**, throttled so folds never run back-to-back without meaningful churn change; passive
compaction-time switching remains the backstop. **(2) The full write leaves the UI thread** —
snapshot (`buildDocumentCheckpoint`) stays synchronous so the document is quiescent, then
compress/parity/layout/atomic-write run on a worker with `StatusBar::setProgress` progress (the
inpaint async pattern); Save-during-save coalesces, close/quit waits with progress, tail-check +
adoptWrittenFile/journal-rebind complete on the UI thread. Commit-append saves stay synchronous
(O(changed)). **(3) Ride-alongs in scope:** journal growth compaction (§2.6) and Flatten
History (§3.7). Standing OUT list unchanged (H3, history-region parity, encryption, NFS,
telemetry — never re-add).

**BUILT 2026-07-23 (branch `s48-build2`: 8 slices by a worktree agent 2026-07-22 + 3 review-fix
commits in the main session; verified, mutation-tested, merged).** All three rulings plus both
ride-alongs:
- `cd21e4e` **H4 content-addressed history** — root `mode:"cas"` (no format-version bump; Build 1
  files read as `"journal"`), hash-keyed `BLOB` chunks (KEY = first 128 bits of content BLAKE3,
  full hash heads the payload; a lying blob resolves nothing), per-dirty-entry `"b"/"f"` refs
  INSIDE each HIST entry (hash and key cannot skew), dedup against CURRENT frames (the ref
  resolves to the parity-covered current chunk), seeds stay per-key frames in both modes. The fold
  measures whole-history churn (bytes-weighted hash reuse; the window IS the retention horizon —
  wrong-window bug pinned as a regression test) and switches by asymmetric hysteresis 0.35/0.15.
  Folding a file with `lostHistoryEntries > 0` REFUSES (damage is never laundered); an
  unresolvable history is preserved as-is, no re-spelling, no switch.
- `bef20fc` **passive switch points** — Save As + every anchored full write route through the
  history-preserving fold (`foldedWriteTo`), so Save As finally carries retained history into the
  new file (spec 3.3) and pending mode switches land at every full write. Quiet gates: armed
  anchor only; clean tail check against the SOURCE; fold-unbuildable falls back to APPEND (the
  compaction Save keeps its loud conflict face), never to a history-dropping plain write.
- `6493035` **proactive early fold** — pure policy `ui/save_policy.hpp`: fires only when
  journal-mode AND churn ≥ the fold's own switch-up AND projected duplicate bytes ≥ 1MB AND
  file ≤ 8MB AND the signal moved ≥ 0.05 since the last attempt. Live signal = io `ChurnTracker`
  seeded from the open (only under the size cap), advanced O(changed) per commit-append Save,
  carried on the CommitAnchor. cas→journal is never proactive.
- `d04035e`+`c41c298` **async full write** (design note committed first, as ruled) — snapshot
  synchronous; file-read/fold/build/parity/atomic-write on a worker (inpaint pattern, status-bar
  progress, controls disabled); authoritative late tail check on the worker just before the
  rename, conflict dialog on the UI thread; adopt/markSaved/lock/journal-rebind only after
  durability (Round 12 A2); saves coalesce newest-wins; close/quit waits with live progress;
  commit-append saves stay synchronous (~0.07ms/batch). The anchor rearm reuses the snapshot's
  serialization — one serialize per full write.
- `9d277d0` **journal growth compaction** (§2.6 "Growth") — past an 8MB floor, once the journal
  exceeds 2× the live working set (newest value per dirtied key, O(dirty) in memory), it is
  rewritten as ONE cumulative state at a `.compact` temp, synced, renamed over, parent-dir
  fsynced; the writer's handle survives the rename so the link chain continues unbroken; the
  cumulative state takes the FIRST autosaved id (consumed, never re-minted — a duplicate id
  would be a compose-time generation tie, Round 12 A5); tombstones ride through; failures back
  off until real growth; torn temps cleaned at begin() and skipped by the restore scan.
- `a81016f` **Flatten History** (§3.7) — File→Flatten History…, AskOrTell confirm (Cancel
  default); .mosaic-backed docs get a PLAIN full write (deliberately never the folding path),
  stack cleared + dirty-until-durable so a failed write cannot leave a clean title over a file
  still holding history; untitled/non-mosaic clears the session stack only.
- `407cea2` **corpus 16/17/18** — cas fold clean (walk resolves; the ladder is blind to the
  encoding), rotted history-only BLOB = document byte-perfect / no dialog / ONE loss by identity /
  whole walk declined (the 13-shape), parity repair of current-referenced content restores pixels
  AND walk. Tool grew `historyResolves()` (mirrors ui::loadedStates, links no FLTK) + a
  force-mode fold knob.
- **Review fixes (main session):** `37b6d2d` background-save quiescence holds against DND
  (drops could swap/edit the document mid-job and finalize would adopt into the wrong one) +
  Flatten History now tail-checks loudly BEFORE the stack clears; `8059015` an
  unreadable-but-verified HIST record declines the whole walk (the lostHistoryEntries verdict —
  skipping it walked structural steps over the gap), fold carries it verbatim, corpus mirror
  matches; `6fb7879` the cumulative-id contract test-pinned.

**Verification (2026-07-23):** 1,944 cases green debug/release/asan + corpus selfcheck +
gui-frames smoke (untitled AND real-document open). **Mutation battery: 11/11 DETECTED**
(wrong-window churn; both hysteresis directions; fold-launders-damage; all 5 proactive-policy
gates individually; blob-duplicates-current; cumulative-id re-mint — the last one initially
SURVIVED because a later autosave papers over the generation tie, closed by pinning the id
contract itself). **Measured churn sweep through the real fold** (60 states, 24 keys, ~16KB
tiles): H4 = 100.2% of H2 at churn 0 (costs nothing when it cannot win), 85.0% at 0.25, 76.9%
at 0.53, 67.8% at 0.67, 63.8% at 0.95 — the research's Round-8 curve reproduced in shipping
code. Fold rate on real corpus docs ~11–12 MB/s end to end (worst proactive stall at the 8MB
cap ≈ 0.7s — same order as the dispatch estimate; the gates stand). Real corpus docs measure
churn 0.172 → dead band, journal held. NOT built (unchanged OUT list): H3, history-region
parity, encryption, NFS/two-machine, telemetry.

**S48-b — `.mosaic` previews + Linux desktop integration. BUILT 2026-07-22** (all four items:
PRVW chunk w/ mutation-tested skip/reject/drop rules + save-path wiring + open-time baseline
seeding; `image/x-mosaic` MIME + `.desktop` + hicolor icons + the project's first `install()`
rules; `mosaic-thumbnailer` linking io+common only — legacy pre-PRVW files get no thumbnail by
design, render stays out; KF6-guarded KIO ThumbnailCreator for Dolphin. `readNewestPreview` +
the light `io/mosaic/fileinfo` manifest reader now feed the New-Document dialog's cards, and the
app-owned `stateDir()/thumbnails` cache is RETIRED — deleted at startup; plain-image recents read
the desktop's shared freedesktop cache read-only + a header-only `io::probeImageDimensions`.)
Original scope follows. The `PRVW` chunk finally gets a
consumer, and Mosaic finally introduces itself to the desktop. Prerequisite discovered in the S48
audit: **the project has NO install rules at all** — no `.desktop` entry, no MIME package, nothing
that installs the binary — so the desktop cannot know what a `.mosaic` is, no thumbnailer can fire,
"Open with" does not list Mosaic, and the positional CLI argument (`mosaic file.mosaic`, shipped
`7985a2e`) is unreachable from a file manager. Some of this groundwork is S59's (packaging) and is
deliberately laid here first.
- **MIME + desktop.** `shared-mime-info` XML declaring `image/x-mosaic` with a magic rule on the
  preamble bytes (`8C 4D 4F 53 0D 0A 1A 0A`) and a `*.mosaic` glob; a `.desktop` entry with
  `Exec=mosaic %f` + `MimeType=image/x-mosaic;image/png;image/jpeg;`; a hicolor mimetype icon
  (`assets/app_icon.svg` exists; a document-flavoured variant does not); CMake `install()` rules,
  which do not exist yet for anything.
- **`PRVW` chunk (measured, 2026-07-09).** 256px longest edge (freedesktop "large", so 128 and 256
  requests both downscale rather than upscale), RGBA, Paeth-filtered, **`Profile::Max`** — 10.6KB on
  the corpus poster vs 16.1KB at balanced; the first real consumer for a profile implemented and
  unused since the codec slice, and exactly what §2.4 earmarked it for (written once, read many).
  Written as an ordinary chunk of `buildDocumentCheckpoint` so `diffDocumentStates` decides: a Save
  whose edit does not alter the 256×144 downscale emits NO preview and costs nothing; a visible edit
  costs 10.6KB. Self-correcting — preview bytes in the append region raise the parity debt that
  trips the compaction which collapses them to one.
  Three rules, none optional: `buildLoadedHistory` skips `PRVW` (a thumbnail is derived, not
  document content); `applyChunksToDocument` never receives one; **compaction DROPS superseded
  previews instead of retaining them as undo states**. And `io` must not gain a dependency on
  `render` — the app owns the compositor and supplies the preview image downward (a full CPU
  composite of the corpus document is 84ms, in the noise beside the serialization Save already does).
- **`mosaic-thumbnailer`** (user call: a separate binary, as `ffmpegthumbnailer` and
  `gnome-raw-thumbnailer` do). Links `mosaic_io` + `mosaic_common` only — no FLTK, no Vulkan, no
  fontconfig, no display — because a file manager spawns it once per file in a directory. Reads the
  newest `PRVW`, downscales to the requested size, writes a PNG. Falls back to compositing only if
  the file predates previews.
- **Both desktops** (user call). `/usr/share/thumbnailers/mosaic.thumbnailer` covers Nautilus,
  Thunar, Nemo, Caja; Dolphin uses an entirely separate mechanism — a KIO `ThumbnailCreator` plugin
  in `/usr/lib/qt6/plugins/kf6/thumbcreator/` — and there is **no bridge between them** (verified on
  the dev machine). The KDE plugin target is guarded by `find_package(KF6KIO QUIET)` so the build
  never hard-depends on KF6.
- *Covers:* the last consumer the native format is missing, and the desktop integration every other
  slice has been quietly assuming.

**S56 — Export pipeline + presets.** Render-to-texture → encode; **Export…** dialog with
per-format options + loss warnings; export presets, batch export.
- *Covers:* "rendering to final output (texture→format)", export UX.
- **MOSTLY LANDED as milestones M1 + M3** (2026-07-04 / 07-28; §10 S56 and `docs/export-system-plan.md`
  §10). The dialog is the interesting part of the as-built: it no longer knows a single format name —
  the list comes from the registry, the **options panel is generated** from each backend's schema, the
  encode goes through `FormatBackend::encode()`, and the loss banner is `diff()` — so adding a format
  adds no dialog code. Its async pipeline is keyed on format + options + size + filter + matte, and the
  **encoded bytes are the source of both the exact file size and the preview** (decoded back), so a JPEG
  preview shows its own artefacts rather than a clean render. ❌ **Batch export is the remainder**;
  presets ship as fixed built-in intents with a "Custom" fallback, and whether users can *save* their
  own is an open product question.

**S56-b — CI on GitHub Actions.** *(Demoted here from S18-c, 2026-06-14, user call: CI is not a
priority during early development, and GitHub Actions bills/limits private repos — the project stays
private until the roadmap is essentially complete, so CI lands right before the cross-compile/release
phase.)* A workflow building + testing every push/PR: an **Arch Linux container** job for parity with the
dev host (pacman deps incl. FLTK 1.4 + shaderc/glslang; configure/build `linux-debug`; run `ctest` —
headless tests use the S2 CPU fallback, optionally `vulkan-swrast`/lavapipe for the GPU path), plus
**clang-format** and **clang-tidy** gate jobs (§8 warnings-as-errors). Failed golden-image diffs are
uploaded as artifacts; README gets a status badge. (Release/packaging jobs stay in S59.)
- *Verify:* the workflow is green on GitHub for the commit that adds it; a deliberate local
  format/tidy violation fails the right job.
- *Covers:* the CI that §3.15/§8 have referenced all along (golden diffs, tidy/warnings gates).

### Phase 9 — Cross-platform & release

**S57 — Windows cross-compile (MinGW-w64). BUILT 2026-07-30.** Mosaic cross-compiles from Linux to
Windows for **two** architectures, both compile-/link-clean, and packages as a portable zip + a
per-user MSI. Reference: `docs/build-windows.md` (design) + `packaging/windows/README.md` (pipeline).
- *Covers:* Windows support via Linux cross-compile (no Visual Studio, ever).
- **Toolchains — two, and not by preference.** `cmake/toolchains/mingw-w64.cmake`, arch via
  `-DMOSAIC_WIN_ARCH=x86_64|aarch64`; presets `windows-x86_64` / `windows-arm64`. x86_64 uses the
  **system mingw-w64 GCC** (same compiler family as the Linux build, so the `-Werror` bar needs no new
  suppressions); aarch64 uses **llvm-mingw**, because the GNU mingw-w64 toolchain has *no* aarch64
  target at all. Windows API floor **10 1809** (`_WIN32_WINNT=0x0A00`).
- **Dependency stack cross-built from source per arch** by `packaging/windows/build-deps.sh` (24
  libraries, all **shared**: Mosaic's modules link statically into `mosaic.exe`, the third-party stack
  ships as ~31 DLLs beside it — user decision). Vulkan uses the standard Windows loader and the
  vendor ICDs, so ⚠ **`vulkan-1.dll` is deliberately NOT shipped** (it is the piece that knows where
  the machine's ICDs are registered; the loader is cross-built for its import library only).
- **What the port needed beyond a rebuild:** a Win32 window + `VK_KHR_win32_surface`, registry-backed
  light/dark + accent detection with a `RegNotifyChangeKeyValue` watch, `MessageBeep` alerts, the
  shell's `IFileDialog` picker, **WinTab + Windows Ink** tablet input (`docs/tablet.md` §5a),
  `ISpellChecker` spell-check, libhyphen with bundled dictionaries, an Explorer `IThumbnailProvider`
  handler (the counterpart to macOS Quick Look), and `common::pathFromUtf8`/`utf8FromPath`/`fopenUtf8`
  — because `std::filesystem::path` is `wchar_t`-based on Windows and the implicit narrow conversion
  decodes in the **active code page**, so a UTF-8 path with an accented or CJK component was mangled.
- **✅ The `.mosaic` write path is now IMPLEMENTED** (it was the one genuinely missing feature, not a
  compile gap — S48 audit 2026-07-09). `stampTipIdentity` uses `GetFileInformationByHandle` (volume
  serial + file index), `verifyTail` does an O(1) positioned `ReadFile` through an `OVERLAPPED`, and
  `appendSaveToFile` does `SetEndOfFile` + a short-write-safe `WriteFile` loop + `FlushFileBuffers`.
  `AdvisoryLock`'s Windows branch now compiles and relies on the kernel closing a dead holder's
  handle for stale-lock release. ⚠ **Honest limit:** Win32 promises file-index uniqueness only while a
  handle is open (reuse after delete; ReFS truncation; SMB may synthesise), so that half is *weaker*
  than POSIX `(st_dev, st_ino)` — the load-bearing check is the positioned checksum read, and the
  existing `(device != 0 || inode != 0)` guard degrades gracefully rather than falsely passing.
- **Verified here (headless):** both arches build clean; `mosaic.exe` runs under Wine —
  `--version`/`--help` and `--composite-demo`, the latter enumerating the real GPU, running the
  **GPU-compute compositor** with **0 Vulkan validation errors** and producing output **byte-identical
  to the Linux build**; the portable zip runs from a fresh extraction; 163-file MSIs for both arches
  (`Template: x64;1033` / `Arm64;1033`). Linux build + full suite stay green.
- ⚠ **Known Windows v1 gaps:** **fractional HiDPI** — the canvas carries an *integer* buffer scale
  (pinned to 1, as X11 does) while Windows DPI is fractional, so at 125/150/200% the document renders
  into the top-left `w()×h()` physical pixels of a larger framebuffer; fixing it means teaching the
  canvas a non-integer content scale (the S58-c class of work). The **file picker blocks the frame
  loop** (`IFileDialog::Show` pumps its own message loop, so the window stays responsive to the OS but
  Mosaic's timers do not fire). **OpenEXR / LibRaw / libzip are not cross-built** (each probes and
  disables cleanly). Runtime is entirely the **user's** check: no Windows machine here, and Wine's
  Vulkan/GDI/DPI are not Windows'.

**S58 — macOS cross-compile (osxcross + MoltenVK). BUILT 2026-07-23.** Mosaic now cross-compiles
from Linux to a **universal (arm64 + x86_64)** macOS app, compile-/link-clean for both arches.
Toolchain: osxcross (real `cmake/toolchains/osxcross.cmake`, arch via `-DMOSAIC_OSX_ARCH`; presets
`macos-arm64`/`macos-x86_64`). Vulkan: cross-built Vulkan-Loader + headers per arch + the prebuilt
universal MoltenVK ICD (`packaging/macos/fetch-vulkan.sh`); the app points the loader at the bundled
ICD at startup. Surface: `VK_EXT_metal_surface` via a `CAMetalLayer` on the FLTK window's NSView
(`platform/native_window_macos.mm`). The third-party stack (FLTK-Cocoa, freetype, harfbuzz,
fontconfig, lcms2, png/jpeg/lz4/zstd, expat, libhyphen, spdlog) is cross-built statically
(`packaging/macos/build-deps.sh`). **Deployment floor macOS 13.3** (libc++ charconv). **Runtime is
the USER's Mac-side check** (no Mac here). Full pipeline + design in `packaging/macos/README.md` +
`docs/build-macos.md`. v1 gaps (Linux-only this pass): tablet pressure, hyphen dicts, native
dark-mode detection; spell-check uses native `NSSpellChecker`.
- *Covers:* macOS cross-compile, formerly a backlog eventuality.
- ~~**Quick Look `.mosaic` preview extension still owed**~~ — **BUILT 2026-07-24 as S58-e** (below): both
  the thumbnail extension and the space-bar preview ship as `.appex` bundles, ad-hoc signed with the
  host. Whether macOS loads an ad-hoc-signed extension from a non-notarized app is the one runtime
  unknown left, not the code.
- **S58-a — macOS i18n (2026-07-24).** The DMG had bundled all 74 `.mo` catalogs since S54, but
  nothing read them: `gettext()` is part of glibc, so `find_package(Intl)` succeeds for free on
  Linux and the macOS dep set never grew a libintl — `MOSAIC_HAVE_GETTEXT` stayed undefined and
  `tr()` passed every string through. `build-deps.sh` now cross-builds the **gettext runtime**
  sub-package, and `mosaic_common` names **iconv + CoreFoundation** beside `Intl::Intl` on Apple
  (FindIntl reports neither; GNU libintl needs both).
- **S58-b — macOS native menu bar (2026-07-24).** `ui::MenuBar` derives from `Fl_Sys_Menu_Bar`, so
  on macOS the menus are the **system menu bar** at the top of the screen (off macOS that class has
  no platform driver and *is* `Fl_Menu_Bar`, so Linux is byte-for-byte unchanged). The in-window
  28 px row is gone there (`kMenuBarHeight` = 0) and About / Settings / Quit move to the
  **application menu**, which also drops FLTK's Print items and NSWindow tabbing (Mosaic has its own
  document tabs). **Item badges** cannot ride an NSMenuItem's drawing, so the pictogram geometry
  moved into a shared table (`ui::badgeShape`) that both renderers read: the pop-up fills it with
  `fl_rectf`, `ui/sys_menu_macos.mm` fills it into an NSImage marked `setTemplate:YES` — a template
  image contributes only alpha, so AppKit tints each badge for light/dark, the highlight, and the
  disabled state on its own. The system menu is a snapshot FLTK rebuilds wholesale, so every
  item-array edit publishes through `MenuBar::update()`, which re-attaches the badges.
  The motivational-one-liner ticker is **dropped on macOS** (its home was the menu row's empty
  right end) and its Annoyances row is not built there — a toggle that cannot do anything is worse
  than an absent one. **Runtime is the user's Mac-side check**, as with all of S58.
- **S58-c — HiDPI overlay widths (2026-07-24).** Every length in `canvas_present.comp` was written
  as a PHYSICAL pixel count, so each one halved in angular size on a 2x display: marching ants,
  lasso, reticle, guides, crop chrome, squiggles, the loupe and the pixel grid all came out
  hairline-thin (surfaced on a Retina Mac; Linux HiDPI had the same bug, just less looked at). The
  file now follows one rule — a distance feeding a coverage **profile** is divided by `uiScale()`
  (so each carefully-tuned profile keeps its exact shape, magnified), while a cull margin, bbox pad,
  dash period, radius offset or glyph size is **multiplied** by it. Element geometry needs neither:
  the renderer already scales positions, radii and the handle half-size on their way into the
  buffers. The scale rides `pc.ants.w`, which the canvas writes as exactly `4 * contentScale` every
  frame regardless of state (the push block has been at its 128-byte budget since S15). At
  `contentScale == 1` every factor is 1.0, so the 1x render is unchanged by construction.
- **S58-d — JPEG XL on macOS (2026-07-24).** JXL is a first-class Mosaic format (File ▸ Quick
  Export as JPEG XL, plus the Export dialog), but `src/io/CMakeLists.txt` treats libjxl as OPTIONAL,
  so the macOS build — which only ever cross-built the REQUIRED deps — silently shipped without it.
  `build-deps.sh` now cross-builds **brotli 1.1.0 + highway 1.2.0 + libjxl 0.11.2** (version-matched
  to the Linux build), with the tools/plugins/benchmarks off and colour management taken from the
  lcms2 already in the prefix rather than libjxl's bundled skcms, so the .app has ONE CMS. brotli and
  highway are built as real installed libraries rather than left to libjxl's in-tree copies, which
  are never installed. Linking needed one gate change: a static libjxl must name its PRIVATE
  dependencies, and `libbrotlicommon` lives in `libbrotlienc`'s `Requires.private`, which the plain
  shared-oriented pkg-config resolution drops — so on Apple the io target uses the `JXL_STATIC_*`
  variables (pkg-config's `--static` answer). Still absent on macOS: libtiff, libwebp, OpenEXR,
  LibRaw.
- **S58-e — macOS thumbnails (2026-07-24).** A Quick Look THUMBNAIL extension
  (`Contents/PlugIns/MosaicQuickLook.appex`, `src/thumbnailer/quicklook_macos.mm`) so `.mosaic`
  files show their own picture in Finder, the Open panel and Spotlight. macOS has no
  spawn-a-helper-per-file protocol like freedesktop's, so this is the third host for the SAME PRVW
  read the thumbnailer binary and the KIO plugin already do — io + common only, no FLTK, no Vulkan.
  The host app now EXPORTS the `org.mosaic.document` UTI (conforming to `public.image`, matching
  the `image/x-mosaic` MIME type `data/desktop/mosaic.xml` declares) because the system matches a
  file to an extension by TYPE, never by suffix. rcodesign signs bundles recursively, so the nested
  .appex is covered by the existing call. **Whether macOS loads an ad-hoc-signed extension from a
  non-notarized app is the runtime unknown** — triage commands in `packaging/macos/README.md`.
  **Follow-up after the first Mac test:** the UTI conformed to `public.image`, which is a claim that
  ImageIO can decode the file — the system's own thumbnailer and previewer then CLAIM it, fail on a
  container they cannot parse, and never fall through to ours (no thumbnails, and an error from the
  space bar). Now `public.data` + `public.content`. The **space-bar preview** followed as a second
  extension point (`com.apple.quicklook.preview`, `QLPreviewProvider` from QuickLookUI, macOS 12+):
  a second `.appex` because `NSExtensionPointIdentifier` is single-valued, embedding the same
  executable and naming a different principal class. It draws the PRVW captioned with the canvas
  size from `readDocumentInfo` (manifest-only, so it costs no more than the preview read) — it does
  NOT composite, which is the same dependency rule the thumbnailer follows.
- **S58-f — feedback from the first real M1 test (2026-07-24).** FLTK's auto **Window menu** is
  gone (`no_window_menu`): it appended itself after Mosaic's last title and its contents are
  hard-coded English that never passes through the catalogs, and a single-window app whose
  documents are tabs earns neither. **History entry names** are now marked `N_()` in core and
  translated where the panel builds each row — extractable at last, though still awaiting a
  catalog pass. The canvas logs its resolved **content scale** once per change, so "is the HiDPI
  scale even reaching the renderer" stops being a guess. And `native_window_macos.mm` now derives
  the pixel size from FLTK's own `w()/h()` rather than `[contentView bounds]`: callers size the
  swapchain from this and the canvas viewport from `w()/h()`, and Cocoa reconciles a CHILD window's
  frame on its own schedule, so the two could disagree with nothing arriving later to correct it.
  **Known limitations recorded, not fixed** (`docs/build-macos.md`): the application-menu strings
  are new msgids with no catalog entries; generated document titles pair a fixed adjective with a
  rotating noun, which cannot be grammatical in languages where an adjective must agree with its
  noun (a catalog-shaped problem across the whole inflected set, deferred by budget); tablet
  pressure on macOS stays untested because the hardware needs an out-of-tree vendor driver.
- **Loupe rotation fix (2026-07-24, user-reported).** The eyedropper loupe mapped its screen offset
  straight into document axes (`loupeSampleDoc + d / loupeMag`), so it ignored the view rotation
  entirely: on a rotated canvas the magnified content slid off at the view angle while the cursor
  moved straight, reading as the loupe drifting in a direction of its own. The offset now goes
  through the inverse view transform's linear part for its DIRECTION (and handedness, so a mirrored
  view follows too) while the SCALE still comes from `loupeMag` — the loupe's magnification is its
  own, not the view's. The centre-cell marker moved to doc space for the same reason, so it turns
  with the texel it outlines. At zero rotation both reduce to exactly the old expressions.
- **S58-g — File ▸ Open crashed the Mac app (2026-07-24, user crash report).** SIGSEGV at a null
  read inside `fl_filename_match`, two frames deep from `-[FLopenDelegate panel:shouldShowFilename:]`,
  while the open panel was asking which items to enable. The cause is upstream, in FLTK 1.4.5: the
  Cocoa open panel runs **out of process** and asks the app about items in batches
  (`requestAppEnabledStateForItems:replyBlock:`), and not every item it asks about is a plain file
  URL — cloud, shared and smart-list rows answer **nil** to `-[NSURL path]`. `shouldEnableURL:`
  forwarded that nil to `shouldShowFilename:`, which sent `fileSystemRepresentation` to nil (NULL)
  and handed the NULL to `fl_filename_match`, whose very first act is to dereference it. The same
  line also indexed `filter_pattern[]` with an unchecked `indexOfSelectedItem`: the popup holds
  `_filt_total` patterns, then a separator, then "All Files", so only `0..numberOfItems-3` are
  valid — the separator row and the −1 "nothing selected" answer both read out of bounds, and only
  the "All Files" row was excluded. Fixed by patching the dependency:
  `packaging/macos/patches/fltk-1.4.5-open-panel-nil-path.patch` guards the index, the nil filename
  and a NULL path-or-pattern, each meaning "no pattern to test against" and so showing the item —
  which is what the popup's own All Files row already does. `build-deps.sh` grew an `apply_patch`
  helper that runs after `unpack` on every run (the tarball is re-extracted each time) and treats a
  patch that no longer applies as **fatal**, so a version bump cannot silently drop a fix. Verified
  in the shipped universal binary: both slices reach `fl_filename_match` only past the new guards.
  It needed no Mosaic code change — the filters the dialog is given were never the problem.
- **S58-i — the bubble was never an FLTK limitation: TWO Mosaic bugs, both fixed (2026-07-24).**
  S58-h gated the speech bubble off on macOS on the evidence that `Fl_Window::shape()` "doesn't work
  there", matching what native Wayland had done since S39. Investigating the Wayland half (which can
  be driven headlessly here — a repro under `sway` on `WLR_BACKENDS=headless`, captured with `grim`
  and read pixel-by-pixel) showed both platforms were failing on **our** code, and the gate is now
  reverted: the bubble is on everywhere.
  - **Cause 1 — we painted outside the platform bracket.** `Popover::draw()` overrode
    `Fl_Window::draw()` and called `draw_children()` itself, so `draw_begin()`/`draw_end()` never
    ran — and that bracket IS the shape implementation off X11: Wayland cuts the mask in `draw_end()`
    (a cairo `CLEAR`), macOS clips to it in `draw_begin()` (`CGContextClipToMask`). X11 alone cuts
    server-side inside `shape()` itself (`XShapeCombineMask`), which is why the one platform we
    develop on was also the only one that ever worked. Proven by variant: child vs top-level and
    single vs double-buffered ALL fail; routing through `Fl_Window::draw()` cuts correctly.
  - **Cause 2 — we claimed the shape too late.** FLTK's Wayland driver declares a fully-opaque
    region for any window whose `shape()` is null when its surface is created, and never clears it
    when a shape arrives later. A popover is a SUB-window, so its surface is created when the host
    window is shown — long before `showAnchored()` knows where the triangle points. The compositor
    had been promised an opaque surface and ignored the alpha, painting the cut region as raw cleared
    pixels: a **black hole through to the desktop**, measured as `(0,0,0)` against a red host.
    `seedOpaqueWindowShape()` claims a fully-opaque shape from the subclass constructor while the
    window is still unmapped; it cuts nothing and changes no pixel, it just gets there first.
  - **The fix's shape.** `Popover::draw()` and `BubbleFlyout::draw()` are now `final` and do nothing
    but bind a painter and call `Fl_Double_Window::draw()`; the painting moved to a virtual
    `drawContent()` that the new `MOSAIC_CHROME_BOX` boxtype dispatches to from inside the bracket
    (`theme.cpp`, the same `Fl::set_boxtype` mechanism as the other `MOSAIC_*` boxes). Because
    `Fl_Group::draw_children()` is not virtual and `Fl_Window::draw()` has no post-children hook —
    and three flyouts paint overlays after their children — `drawContent()` keeps calling
    `draw_children()` itself and the base's duplicate pass is suppressed by clearing the damage it
    keys off. That let all 8 overriding subclasses convert by pure rename, with no body edits.
    `Fl_Window_Driver.H` is not an installed header and `draw_backdrop()` is not virtual, so the
    boxtype is the only supported way back into the bracket.
  - ⚠ **This could not have been fixed in FLTK:** the Linux build statically links the distro's
    `libfltk.a`, so `packaging/macos/patches/` would only ever have fixed the Mac.
  - Verified on native Wayland by screenshotting the REAL `ui::Popover` (a harness links
    `libmosaic_ui.a`) — margins now read `(255,0,0)` through to the host. X11 via XWayland is
    byte-identical apart from its hard 1-bit XShape edge where Wayland blends one pixel. macOS
    inherits the same code path but stays **unverified until the user runs the DMG**.
- **S58-h — the speech-bubble popovers join the plain-panel side on macOS (2026-07-24, user-observed).**
  SUPERSEDED SAME DAY by S58-i, which found the real cause and reverted this gate. Kept as the record
  of a workaround shipped on a wrong diagnosis.
  `Fl_Window::shape()` does not cut the triangle's corner margins transparent on Cocoa either, so the
  bubble read as an opaque notch beside the body — the same symptom native Wayland has always had, and
  the reason the plain-panel fallback exists. `Popover::bubbleSupported()` is now false on `__APPLE__`,
  which is the single gate: the six call sites that reserve `kBubbleTri` of margin stop reserving it,
  `applyBubbleShape` stops calling `shape()`, and `drawBubbleChrome` draws the plain panel with the
  same balanced margins. No new code path — macOS takes the one Wayland has been using since S39.
  Worth recording because the first guess at the cause was WRONG: FLTK's Cocoa driver does build a
  `CGImageMask` from an RGBA mask (`shape_alpha_` sets `shape_data_->shape_`, so `w->shape()` is
  non-null and `makeWindow` does apply `setOpaque:NO` + `clearColor`). The mask is built and the window
  is transparent; something between there and the screen drops it — plausibly the `Fl_Double_Window`
  flush, which clips while drawing into the offscreen and then blits the full rectangle. Undiagnosed on
  purpose: the gate is the fix the user asked for, and chasing it further costs a DMG round-trip per
  guess. If it is ever worth the triangle, `packaging/macos/patches/` (S58-g) is now the place for it.

**S59 — Packaging & release engineering.** Linux AppImage and/or Flatpak, Windows portable
build, versioning, About box, desktop integration, `.mosaic` MIME/file association.
- *Covers:* distributable artifacts.
- **macOS `.dmg` slice DONE 2026-07-23 (with S58):** `packaging/macos/make-dmg.sh` builds an
  **unsigned** universal `Mosaic.app` + a drag-to-Applications `.dmg` entirely on Linux (no Mac,
  no root) — HFS+ via `newfs_hfs`/`hfsplus`/`dmg`, the angled low-opacity app-icon background, and
  a Finder `.DS_Store` (window/icon layout + background alias) authored via `ds_store`/`mac_alias`.
  No signing/notarization by user decision (Gatekeeper Open-anyway). Remaining S59: Linux
  AppImage/Flatpak, S59-a native-Wayland polish. (**The Windows portable build + MSI landed with
  S57 on 2026-07-30** — `packaging/windows/make-package.sh`.)
- **S59-a — native-Wayland desktop-integration polish:** `xdg_dialog_v1` modal dim / prevent-raise for
  the Settings dialog (blocked until FLTK exposes the dialog's `xdg_toplevel` — see §12) + the Wayland
  **app_id** pin and `.desktop`/hicolor **icon** (§12). All opportunistic: bind/skip cleanly where the
  compositor or FLTK doesn't support it.
  - **PARTLY LANDED 2026-07-28 — and it grew a headline the spec never had: the backend default
    itself flipped to native Wayland** (`platform::preferWaylandBackendIfUnset`; `FLTK_BACKEND=x11`
    is the escape hatch, a pure-Xorg session is untouched). That was scheduled to ride along with
    S43-c and did not need to: the file-picker parenting, the X11 resize black flash and the cursor
    work each argued for it on their own. Two genuine Wayland cursor defects came out with it (the
    device-vs-logical hotspot double-scale; `breeze_cursors` shipping neither `fd_double_arrow` nor
    `bd_double_arrow` for the corner handles), plus the `FL_ENTER` cursor-state reset that closes the
    §12 stuck-rotate-cursor item. The **app_id** pin landed. **`xdg_dialog_v1` did not** — FLTK
    1.4.5's public surface is unchanged, so the blocker in §12 is exactly where it was.
  - **`docs/wayland.md` is the reference** for the native path: what the flip changes (dialogs,
    tablet, icon, popovers, clipboard, resize, HDR, cursors), what is deliberately not done, and §5's
    nine checks that only a human on a real session can make.

**S60 — Performance & hardening.** Split (2026-06) — it had become a phase pretending to be a
session; the §12 items pegged to it now have explicit homes:

> ⚠️ **SCOPED IN FULL 2026-07-23 → `docs/s60-performance-plan.md`, which SUPERSEDES the -a…-d
> split below.** The scope pass measured the tree and found: the interactive compositor is pinned
> to `Backend::Cpu` at every app call site (the GPU compositor has never run in the app, and would
> be *slower* if flipped — it round-trips both operands per layer); we create **up to five separate
> VkDevices** (one per lane); and we are accidentally already Vulkan-1.0-feature-clean (no device
> features enabled anywhere, no shader uses a post-1.0 feature, every workgroup ≤ 64 invocations,
> `canvas_present.comp`'s push block is exactly the 128-byte 1.0 floor) but pinned to 1.2 by two
> constants and `--target-env=vulkan1.2`. **User directive: baseline Vulkan 1.0, tier optimisations
> on by detected capability (never by version number), back-fill from supported extensions before
> ever taking the CPU path; dirty-tile grid tied to the `.mosaic` 64 px tile grid.** New session
> split: **S60-α** foundations (`GpuCaps` + 1.0 retarget + ONE shared device) → **S60-a** the
> resident tiled compositor (the fused transform+mask+clip+blend kernel — the load-bearing
> constraint) → **S60-b** CPU-only mode + thread pool → **S60-c** present pacing → **S60-d** huge
> docs → **S60-e** missing GPU lanes → **S60-f** hardening/fallback. Five open user questions in
> §10 of the doc.
>
> **STATE 2026-07-29 (see §10 for the per-item ledger): S60-α is COMPLETE, S60-a's items 7–13 are
> BUILT, and the doc's five-condition performance gate PASSES.** ⚠ **The compositor is nonetheless
> still 100 % CPU by default** — the resident lane exists only behind `MOSAIC_TILE_COMPOSITOR=1`, and
> the flip is a one-line change blocked on an *interactive* pass rather than on any measurement, because
> the resident present path cannot be reached without a real edit and `--gui-frames` cannot make one.
> Slices of -b, -c, -e and -f have landed opportunistically alongside; -d is untouched apart from the
> GPU memory budget. The `-a`…`-d` bullets below therefore describe the **original intent**, not the
> current state.

- **S60-a — Interactive compositor performance.** **GPU residency** (layers stay device-resident;
  `GpuCompositor::blendOver` stops round-tripping per layer) + **tiled dirty-region recomposite**
  (a one-layer edit re-blends only affected tiles, with cached above/below subtree composites).
  Absorbs the §2 S7 deferral and the *scoping* half of the §12 edit-latency item.
  - **Coverage partitions — BUILT 2026-07-24** (sequenced here so the operator is written once,
    before residency moves the walk). Cut splits one surface into `A·m` / `A·(1−m)`, but `over`
    assumes independent coverage and loses up to **25 % of the alpha at `m = 0.5`** — the
    translucent rim a feathered *or merely anti-aliased* cut shows when pasted back in place. No
    `over`-based repair exists (`a + b(1−a) = A` forces one edge hard), so the halves record a
    `core::CoveragePartition` and the compositor rewrites the lower half's alpha to `b = r/(1−f)`.
    `over` is associative ⇒ this holds with the piece **anywhere above** the hole — nested in
    pass-through groups, several layers up, either half grouped — and `f` folds the piece's
    opacity, group opacities and a linked mask so fading slides continuously to the bare hole.
    Liveness keys on `RasterLayer::alphaFingerprint()`, **not** `contentRevision()`, so undo/redo
    back onto the split pixels revives it. **Needs no GLSL**: the GPU lane is a per-op `blendOver`,
    and this is a per-layer alpha rewrite in the CPU walk — but the two drag fast paths
    (`DragCompositeCache`, `canUseGpuDrag`) stand down while a partition is live. Also fixed: the
    two halves used to truncate independently and sum to 254/255; the residual is now derived as
    `src_a − fragment_a`, exact in 8 bits. **Documented limitation:** a blend mode on either half
    retires it — alpha is blend-independent but colour is not, and reconstructing would trade the
    alpha rim for a colour ring (Subtract makes it obvious). Merge Down recombines disjointly
    (baking `over` there is irreversible); isolated renders keep the true soft hole. Persisted in
    `.mosaic` (rel affine + token; the hash is re-derived on load). 12 tests in
    `tests/test_coverage_partition.cpp`. **User interactive pass owed.**
- **S60-b — Present-paced render loop.** FIFO/vsync-paced presentation (`wl_surface.frame` on
  Wayland), **render-on-demand** (idle ⇒ no frames; high-refresh panels get their full rate), and
  the composite moved **off the UI thread** onto the render/worker thread (§4). Absorbs the §12
  present-pacing item and the *offload* half of edit-latency.
- **S60-c — Huge-document scalability.** **Tiled pixel storage** (the half deliberately left out of
  S43-a), proxy/low-res preview, GPU memory budget + eviction.
- **S60-d — Hardening & final polish.** `--device` adapter selection + a clean software-rendering
  fallback (lavapipe) with a warning (§12 item); sanitizer- and validation-clean passes; profiling
  of remaining hot paths; accessibility checks; docs finalization. (Also the natural slot for the
  §12 crisp-pixels/pixel-grid item if it hasn't landed earlier.)
- *Covers:* "professional, intuitive, presentable" bar; maintainability.

> Cross-cutting (applied every session, not separate items): **unit tests** (S1 onward),
> **Claude debuggability** (S2 harness), **tooltips** (S4 onward; **never on the canvas
> itself** — §S8), **Vulkan acceleration**
> (S2/S7 onward), and **active-color usage** (S11 onward).

---

## 10. Progress Tracker

> Tick `- [x]` when committed; if a session was split, note it (e.g. `S29-a`, `S29-b`). ⚠ **The
> original "the first unchecked box is the next task" rule no longer describes this project** — the
> roadmap has been non-linear since roughly 2026-07 and boxes are open across four phases at once, so
> §2 is what names the next session. `[~]` marks a session that is genuinely part-built, and where it
> appears the entry says which half.

**Phase 0**
- [x] S0 — Planning (PLAN.md, .gitignore, git init)
- [x] S1 — Repo scaffolding & build system (README, LICENSE, CMake, docs/ skeleton)
- [x] S2 — Vulkan bootstrap + headless/offscreen harness — [x] S2-a (Vulkan + harness) · [x] S2-b (shader-embed + compute)
- [x] S3 — FLTK app shell + Vulkan canvas window
- [x] S4 — Theming engine + custom-widget base + tooltips
- [x] S5 — Settings, logging, i18n scaffolding (+ error dialog; nlohmann/json vendored)

**Phase 1**
- [x] S6 — Document & layer model + undo/redo — [x] S6-a (model + geometry) · [x] S6-b (commands/undo)
- [x] S7 — Vulkan compositor — [x] S7-a (CPU reference + blend modes) · [x] S7-b (GPU compute blend kernel + VMA) · [x] S7-c (canvas wiring; GPU residency + tiled dirty regions deferred to S60 perf)
- [x] S8 — Canvas viewport — [x] S8-a (view transform + pan/zoom/rotate input + fit/100% + View menu; compute presenter) · [x] S8-b (rotation degree-readout dial overlay)
- [x] S9 — New-document dialog (A-series presets + custom size/units/DPI + colour space/bit depth/background)
- [x] S10 — Layer panel (right dock) — [x] S10-a (right dock + Layers list: thumbnails, eye/visibility, active select, add/delete, Edit→Undo/Redo) · [x] S10-b (deep layer duplicate in core, drag-to-reorder, drag-onto-plus clone — "+" outlined green for the whole drag, filled green on hover — Layer→Duplicate) · [x] S10-c (groups create/collapse/nesting + reparent-drag, per-layer opacity slider + blend-mode dropdown, Shift-click-thumbnail→select [stub until S13]) · [x] S10-d (layer drag ghost: a floating semi-transparent chip follows the cursor + source row shows a dashed lifted/dimmed slot; drop-back restores — no new commands)

**Phase 2**
- [x] S11 — Tool framework + left toolbar + tool options bar + color swatch — [x] S11-a (tool registry/active-tool [`ui::ToolManager`] + 12 built-in tools with runtime-rasterized **colorful** SVG icons; always-square left toolbar with tooltips + accent active state; plain-letter tool shortcuts) · [x] S11-b (per-tool **option model** [Slider/Choice/Toggle] + **tool options bar** rendering the active tool's options, with the lockstep notification infra) · [x] S11-c (options-bar consolidation: `ToolOption::primary` hot-subset + overflow drop + min window size + brush placeholder trim + `ToolGroup` + subtle toolbar dividers + XWayland resize-flash fix; generic Properties-tab mirror **cut**) · [x] S11-d (two-diagonal-square active-colour swatch [fg-over-bg chips + swap/reset glyphs] + `ui::ColorState` fg/bg holder the tools will read + a reusable anchored `ui::Popover` host backing a flat-colour `ui::ColorPicker` stub — RGB sliders + hex; X swaps, D resets) · [x] S11-e (toolbar **flyout variant groups**: `ui::ToolSlot` + per-slot shown-variant tracking in `ToolManager`; one button per slot with a corner triangle for multi-variant slots; **right-click**/triangle opens a `ui::ToolFlyout` [`Popover` rows] to switch variant; seeded marquee rect/ellipse, lasso free/poly, shape rect/ellipse/line [16 tools / 12 slots]; `Popover` `setBaseSize` + top-aligned placement + `reanchorActivePopover`)
- [x] S12 — Colour picker + colour management — [x] S12-a part 1 (picker UI: model combo + SV field/hue strip + hex-input redesign) · [x] S12-a part 2 (review rework: drop Hex from combo, editable readouts, square field + wheel/triangle surfaces + surface combo, settings-persisted default, flyout-dismiss bug) · [x] S12-b (lcms2 + Lab + CMYK + colour-space indicator + gamut warning/snap + swatches/recents) · [x] S12-c (ICC profile support: user .icc loading; vendored HEIDELBERG-licensed FOGRA39 CMYK default)
- [x] S13 — Selection model + animated marching ants — [x] part 1 (core Selection mask + boolean ops + SetSelectionCommand) · [x] part 2 (marching-ants present pass [R8 mask texture + screen-space edge + dash-phase push constant] + Shift-click-thumbnail → `selectionFromLayerPixels` + Select All/Deselect/Inverse menu)
- [x] S13-b — Status bar (doc size/bit depth + physical size at ppi, cursor pos + colour-under-cursor chip, zoom/rotation, colour-space indicator [moved out of the picker], selection bounds, status/progress slot; event-driven, pure formatting unit-tested)
- [x] S14 — Marquee select (rect/ellipse/lasso) — `Selection::ellipse`/`::polygon` (AA, even-odd) + `ui::SelectionGesture` (press-mods=op, drag-mods=shape, poly click-click-close, Esc/Enter) + canvas tool dispatch with frame-coalesced live preview (combined mask for marquees, ~1-screen-px rubber band for lassos); one `SetSelectionCommand` per gesture
- [x] S14-b — Clipboard (cut/copy/paste + external interop) — `core/clipboard.*` (copy/copy-merged/cut-clear/paste-position/white-flatten, pure) + `SetLayerPixelsCommand` + Edit menu (Ctrl+X/C/V, Ctrl+Shift+C) + Fl_Copy_Surface out / Fl::paste in (own-copy recognition) + thumbnail boolean ops (Shift+Ctrl/Alt/both) with the +/−/× frame chip
- [x] S15 — Move/transform tool — Affinity-style Move (V): click-select (`core::topmostLayerAt`) + `ui::TransformGesture` (move/scale/rotate, layer-local scaling, Shift/Alt constraints, Esc restore) + present-pass handles overlay + ONE coalesced `SetTransformCommand` per drag (skew deferred → S35-b note)
- [x] S15-b — Interactive drag latency (mini: event-kicked frames [`MainWindow::requestFrame` + canvas callback] + `render::DragCompositeCache` [belowAcc + cached above rasters + clip-base state, byte-exact replay via the shared `walkStep`]; bonus exact wins: transparent-texel skip in `compositeBufferOver`, reusable replay buffers; bench 72→29 ms/frame representative @1080p×3 on a loaded box)
- [x] S15-c — **Move tool multi-selection (shift-click add-to-move)** (user 2026-06-14). Shift-click **toggles** a layer in/out of the move selection (add if absent, remove if present — the canvas-object norm of Photoshop/Illustrator/Figma; *shift-click again* subtracts, no Ctrl), and the gathered layers **move/scale/rotate together as a set WITHOUT being grouped**. The selection is canvas-only + ephemeral (`VulkanCanvas::m_moveTargets`, cleared on tool switch / new doc / click-away). Framing box: one layer = its own (rotated) content box (unchanged); several = the **axis-aligned union** of their content rects (`moveSelectionBox`). The gesture runs once in the box frame; its delta `boxWorld * baseInv` is applied to **each layer's press-time world transform** (captured at `beginMoveGesture`, since `transformFor` is absolute-from-press) and pushed as ONE coalescing **`core::SetTransformsCommand`** (new multi-layer command; coalesces only when the next step targets the same ids/order/coalesce — a stable drag = one undo step). Click disambiguation via the existing dead-zone latch: a no-drag body click drills (single) / collapses to the clicked unit (multi) / toggles (shift); add-on-press for a shift over an unselected layer so a follow-on drag moves it too. `activeDragLayer()` returns invalid for a multi-drag (drag-cache models one moving layer → full composite fallback). Host callback `setTransform`→`setTransforms(vector<(id,xf)>, coalesce)`.
- [x] S15-d — **Multi-select blend/opacity gating** (user 2026-06-14, S15-c follow-up — found that the
    panel's blend-mode + opacity strip silently acted on only the *last-clicked* layer of a Move
    multi-selection). The panel now learns the Move-tool selection set (`MoveToolHost::selectionChanged`
    → `LayerPanel::setMoveSelection`, ephemeral, distinct from the persistent single active layer): all
    selected rows get a subtler muted-bar highlight (the active one keeps the accent bar), and **while >1
    is selected the blend/opacity controls are disabled** — the safe default the user picked, since these
    are per-layer compositing props, not batch edits (industry is split: Photoshop leans active-only,
    Affinity/Figma apply-to-all). The blend dropdown shows the selection's **mixed state** ("Normal,
    Multiply" — distinct modes in stack order, via `Dropdown::setOverrideText` + `mixedBlendLabel`); the
    disabled opacity slider + readout show the selection's **average** opacity, marked approximate as
    "~NN%" (read-only eye candy, consistent with the blend label — an average opacity is admittedly
    useless; "All selected layers" mode in S15-e reuses the average but makes it editable). Single/empty
    selection restores the normal active-layer strip. **Polish (same day):** the disabled `Slider` no
    longer greys its cell (raw `fl_rectf`, not `draw_box`, so FLTK's `fl_inactive` doesn't dark-box it).
- [x] S15-e — **"Multi-selection edits" setting** — **DONE 2026-06-16** (landed in the S51-a **Tools**
    category, **Move** sub-tab). What blend/opacity do with several layers selected, since the industry is
    split. **Settled naming (user-approved):** title **"Multi-selection edits"**; behaviour-named options —
    **"Disabled (edit one at a time)"** (S15-d default), **"All selected layers"** (Affinity/Figma — one
    coalescing command across the set), **"Active layer only"** (Photoshop). `Settings::multiSelectionEdits`
    "disabled"|"all"|"active" → RunOptions → `LayerPanel::MultiSelectMode` via `setMultiSelectionMode`, live
    through `SettingsHost::setMultiSelectionEdits`. Shown as three `ui::OptionCard` **diagram cards** (a mini
    layer stack: thumbnail + name bar per row; accent ring = in the selection, accent dot = the edit lands
    there). **Behaviour (LayerPanel):** Disabled keeps the old inert strip (mixed-blend label + ~avg opacity,
    deactivated); Active-only keeps the strip live and edits `m_active`; **All** edits the whole move-selection
    in one undo step — a `CompositeCommand` of `SetBlendModeCommand`s for blend, and a NEW multi-layer
    coalescing **`core::SetOpacitiesCommand`** (mirrors `SetTransformsCommand`; sets all selected to the
    slider value, the drag coalescing into one step) for opacity. The draw() "~NN%" average + strip gating are
    all mode-aware. **FOLLOW-UP (flagged, architectural):** "All" works only while the **Move tool holds the
    multi-selection** (it's ephemeral via `m_moveSelection`); making the selection **persistent + panel-owned**
    so it survives tool switches is the follow-up (`multiSelectActive()` is the seam). Tests:
    `SetOpacitiesCommand` coalescing + settings round-trip extended; build + ctest + gui-frames clean (the only
    ASan leak is the pre-existing FLTK `putenv` in test_theme). USER does the visual pass. Pairs with
    [[S16-q]]/[[S16-p]].
- [x] S16 — Crop tool — staged-rect gesture (ui::CropGesture: draw/move/resize + ratio presets/Swap, Shift/Alt, doc clamp) + present-pass overlay (shield/thirds/handles via the ants.z mode lane) + ONE "Crop" CompositeCommand (`render::buildCropCommand`: ResizeCanvas + group-aware rebase push-down + optional Delete-Cropped bake + Selection::cropped); Esc/Enter/dbl-click; live size in the status bar
- [x] S16-b — History panel — CommandStack grows the history view (size/position/nameAt/jumpTo [one batched notification] + setOnChange observer [merged coalesced pushes don't fire]); `ui::HistoryPanel` right-dock tab (LayerPanel header → a real Layers|History tab strip): Original row + chronological entries, position highlight, muted redo tail, click-to-jump; in-place same-count refresh (a jump refreshes from inside the clicked row's handle())
- [x] S16-c — Crop/UX feedback fix pass (user 2026-06-13) — tooltips set at construction + every options-bar control covered; crop labels "Swap orientation"/"Delete Cropped Pixels", ratio-conform on re-entry + **staged rect persists across tool switches**, "Guides" toggle (rule-of-thirds, pixel-snapped + visible); **unmasked-group local-buffer extent fix** (drill-into-a-group + move no longer clips a child — `renderLayer`/`compositeChildren` thread a `pre` transform, sizing the buffer to visible content with an offset; byte-exact for identity-transform groups/root; drag-cache caches over content bounds + offset); History panel **relative-time column** (`Command` push timestamp + `CommandStack::timeAt`, "just now/2m/3h"→date past a day, full stamp on hover); `common::Settings.units` (auto|metric|imperial, locale default via glibc `_NL_MEASUREMENT`). The layer-drag drop-indicator work also started here but was superseded by S16-d (see there). No glyphs in UI labels (host-font risk — user rule)
- [x] S16-d — Feedback rounds 2–3 + crop Apply/Cancel (user 2026-06-13): drop-indicator rework (drop-line no longer hidden by the ghost — ghost narrowed; in/out-of-group shown by a 1px ring on the joined group + a 2px indented line; bottom-of-group AND just-below-group both reachable via cursor indent in `planDrop`); **caption/tab bolding fix** (the Blend/Opacity captions, % readout, and Layers|History tab labels gated on `damage() & ~FL_DAMAGE_CHILD` so a child-only redraw stops re-stamping them heavier); History panel **Up/Down step through states** (panel takes focus, consumes arrows before the Fl_Scroll) + relative-time column insets past the scrollbar + tip shows seconds + calmer tooltip delays (0.4→0.7 s, hover-delay); **options-bar Button + Number control kinds** (`ToolManager::onAction`; outlined `Fl_Float_Input`) and crop **Apply/Cancel** (worded green/red, NO glyphs, **right-anchored** so they never scroll off; Apply=commitCrop, Cancel=reset to ratio-conformed full canvas; Enter/Esc kept; `rebuild()` refactored to left-fill + right-anchor)
- [x] S16-e — Crop custom ratio + px/unit size readout (2026-06-13, two commits). **(1) Custom ratio** — a **"Custom"** entry (index `kCropRatioCustom`) in the Ratio combo reveals two `[N] : [N]` **Number** fields (`ratioW`/`ratioH`, floats, min 0.01, no practical max); aspect = `customCropRatio(w,h,swap)` (pure + unit-tested), fed through the existing `conformCropRect` path. The fields live in the option set permanently but start non-primary (hidden); the host (`MainWindow::refreshCropCustomFields`) flips `primary` on the Free/preset↔Custom transition and triggers a **deferred** `Fl::add_timeout(0,…)` bar rebuild — never synchronous inside the control's own callback (use-after-free). `ToolOptionsBar::syncValues()` now skips the focused control so per-keystroke `FL_WHEN_CHANGED` re-sync can't eat a half-typed decimal. **(2) Size HUD** (user pick: a centred pill below the rect, one line) — `W × H px · w × h in/cm`, rendered in the present compute shader (FLTK can't paint over the Vulkan surface). The 5×7 bitmap font gained `× . p x i n c m ·` + a blank; `drawHud` builds the glyph string per-pixel inside a cheap reject band below the rect. Unit = `common::resolveUnits(Settings::units)` (resolved in `main`, carried on `RunOptions::units` → `MainWindow::m_metric` → `CropToolHost::metricUnits`) + the doc DPI. **Stays at the 128 B push budget** by riding the rotation-dial lanes (`overlayCenter`=W,H px, `overlay.y`=DPI, `overlay.z`=metric, `overlay.w`=HUD-active) — free whenever the dial is off, so the HUD is simply suppressed while the view rotates (the `setCropSizeReadout` setter; dial wins if both set, and `overlay.w` is 0 when neither is active so a stale degrees can't ghost a HUD). **Verified:** build (incl. glslc) + `ctest` + `--gui-frames` clean; pot regenerated (Custom + tooltip strings).
- [x] S16-f — Canvas expansion (crop beyond the canvas) — **DONE 2026-07-02 (5 commits, `54fe06a`…`a9b589f`), BEYOND the original spec.** As built: staged rect UNCLAMPED (safety envelope = canvas outset 2x its larger dim; zoom-aware 8px canvas-edge snap band keeps plain crops effortless; conform keeps stay-inside behaviour for inside rects), buildCropCommand handles expansion + optional CropFill (standard Background extended in place byte-exact — non-delete uses a UNION-sized buffer so crop+expand destroys nothing; exotic stacks get a bottom "Canvas fill" layer covering ONLY the ring), **Fill combo** = Transparent/White/Black/Active color/Background color/**Inpaint — a WORKING inpaint fill** (async CropExpandJob heals the ring on the translated flatten via the S37 engine, lands through the same one-undo-step command via CropFill.pixels; NOT a disabled placeholder — the "Inpaint (soon)" guardrail rested on an over-broad reading, corrected in docs/smart-resize-research.md §3.10: what is actually ruled out is a post-crop preview-image CHOOSER, not the fill itself), expansion visuals (crop-box outline turns green per-pixel exactly where it leaves the canvas + thick 45° green hatch over the staged expansion — zero new GPU data, doc coords + quad SDF already in the present shader), **crop-ROTATE** (corner band outside the box, Move-tool conventions: cursor 15/Shift 15°/dbl-click reset + a 1° zero-magnet; frame rect + FIXED pivot representation keeps gestures cursor-following; apply = conjugated rebase through translate∘rotate; wedges fill like expansion; selection cleared; not offered while Smart Resize ON) with **§3.10 guardrail 3 live: the Inpaint entry greys out while the crop is rotated** (per-item Dropdown disable machinery added: ToolOption.disabledChoices → FL_MENU_INACTIVE → popup dims + refuses commit). ⚠ **Standing guardrails, §3.10 — deliberate and not to be "fixed":** no operation-preview chooser ever; fill only on explicit Apply of a persistent pre-chosen mode; inpaint × rotation mutually exclusive. 554 tests release+ASan, swan-photo inpaint-expand eyeballed headless (seamless), user visual pass owed. (Shares the "extent == canvas" theme with the S16-c group fix; masked-group extent is still S31/S60-a.) **Separate crop-*in* axis SCOPED (2026-07-01): Smart Resize (image retargeting) — a Crop-tool toggle that content-aware-crops to a new aspect ratio. Full research + build plan in `docs/smart-resize-research.md`. Verdict: content-aware cropping only (the best-ranked available operator per the RetargetMe benchmark); ⚠ **seam carving GUILLOTINED** (also ranked last) and **warp EXCLUDED** (deformation disliked) — both deliberate, neither is a gap; face-aware via Viola-Jones. BUILT 2026-07-02 — see S16-r.**
- [x] S16-r — Smart Resize + Smart Recompose (content-aware retargeting) — **DONE 2026-07-01 → 07-02 (research + ~11 commits, `0ce014b`…`dae0902`).** The crop-*in* axis: retarget to a new aspect ratio without squashing. **Smart Resize** = content-aware cropping (the best-ranked available RetargetMe operator; ⚠ seam carving GUILLOTINED and warp EXCLUDED — deliberate exclusions that stand). **Smart Recompose** = object-preserving retargeting: automatic keep-region extraction + **Ctrl-drag keep-region chips** ("mark what matters", fork F-d), a rigid placement solver, the recompose pipeline (prepare/assemble split + an inpaint FillFn adapter that heals vacated background via the S37 engine), an **async Recompose job** with review/nudge/apply, residual-seam band blend (window shuns healed content), and an **"About Smart Resize" credits sheet**. F1 (face-aware) resolved = no faces; chip-editing depth closed. Full build plan in `docs/smart-resize-research.md`. *(Supersedes the "NOT started" note that trailed S16-f.)*
- [x] S16-g — Layer-panel & History polish backlog (user 2026-06-13, round 3) — **DONE 2026-07-09**, together with a layer-dock professionalisation pass the user asked for in the same breath (icons, rename, lock, context menu, resizable dock). All five backlog bullets closed:
  - **History focus:** **DONE (2026-06-14)** — `applyTabVisibility` now calls `m_history->take_focus()` when the History tab is shown, so Up/Down step through states without a click-to-focus first. (Chose on-show over on-hover: hover-focus would steal focus while merely mousing over.)
  - **History time labels go stale — DONE.** `HistoryPanel::armAgeTick()` schedules an `Fl::add_timeout` whose delay is **the time until the soonest row's caption would actually change** (`ageTickDelay()`, mirroring `relativeTime()`'s bands: 1 s while any entry is in the seconds band, up to the next whole minute below an hour, up to the next whole hour below a day, and **never** once every entry shows an absolute date). It re-arms only while `visible_r()`, is disarmed by an overridden `hide()` and the dtor, and redraws only the rows whose caption moved (`HistoryRow::ageLabelStale()` compares against `m_paintedAge`). So an idle History tab never wakes the app, and a tab that isn't showing never ticks at all.
    - **Entry layout flicker — DONE, root-caused.** It was not a repaint race: `HistoryRow::draw()` read `sc->scrollbar.visible()`, and **`Fl_Scroll` only decides that inside its own `draw()`**, so the first paint after a tab switch used the *previous* answer for the gutter and the age column jumped one frame later. Both panels now compute the gutter themselves on Fl_Scroll's own rule (content taller than viewport → `Fl::scrollbar_size()`) and push it into the rows (`HistoryRow/LayerRow::setScrollGutter`, `updateScrollGutter()` on every row-count or viewport change; `HistoryPanel::onTabShown()` settles it before the first paint). Fixes the same latent bug in the Layers tab's active-row dot, which now matters because the dock is width-resizable.
  - **Drag ghost vs drop-line — DONE, both remedies taken.** The chip is now a **genuine offscreen RGBA composite** (`Fl_Image_Surface` → one `Fl_RGB_Image` blit at α≈0.83, with a soft offset shadow baked into the image's bottom-right margin), so the row beneath reads through it; and the drop cues are drawn **on top of** it instead of dodging it. The **start knob is back** — an AA'd accent disc at the line's left end (`drawAAPrims`), whose `under()` sampler returns the translucent card colour where it overlaps the chip and the row's own fill elsewhere, so its edge never haloes against the wrong ground. ⚠ **`Fl_Image_Surface::image()` returns the raster at DEVICE resolution**, not the logical size you asked for: on a scaled screen the pixel-extraction loop must walk the raster's own dimensions and map each pixel back to the logical card/shadow regions (reading the top-left `imgW × imgH` would blit a magnified corner of the card), then `Fl_Image::scale()` back to logical size for the draw.
  - **Ghost cursor anchoring — DONE.** The chip's top-left rides the cursor (`+10,+6`, clear of the arrow's hotspot); the press-time grab-point offset (`m_grabDX/DY`) is gone.
  - **Ghost confined to the Layers tab — RESOLVED as "design around it" (user's call, 2026-07-09).** A drag can only ever target a row position *inside* the dock, so leaving it is meaningless; a top-level overlay window was rejected because stray top-levels are exactly the FLTK/Wayland trap this project keeps hitting (see the sub-window ctor rule). The chip stays clamped to the dock, and the real complaints above are what got fixed.
  - **Shipped alongside (same session, user request):** panel-chrome icon set (`ui/icons.hpp` + `assets/icon_{plus,trash,group_layers,eye_open,eye_closed,lock_open,lock_closed}.svg`) — **one-ink** white SVGs tinted to a palette colour at draw time, so one source serves both themes, hover, and the disabled state (the colourful/illustrative *tool* icons remain S52's job); `IconButton` bottom strip (Add + Group on the left, Delete exiled to the right edge, both structural buttons greying on a locked layer); **inline rename** (double-click the name / context menu → a `TextInput` floated over the row, Enter/click-away commit, Escape reverts, no command on a blank or unchanged name) with `SetNameCommand` now clearing `TextLayer::autoNamed` (undo restores) so renaming a Text layer is not a silent no-op; **layer locking** — new `core::SetLockedCommand` over the long-existing `Layer::locked` bit, enforced against *structural* edits (delete/group/reorder-drag) in the panel and against *transforms* at `VulkanCanvas::beginMoveGesture` (any locked layer in a multi-selection vetoes the gesture, Photoshop's rule; the host names the refusal in the status bar), with visibility/opacity/blend deliberately left editable; **row context menu** (`ui::ContextMenu`) offering only functionality that exists — Rename / Duplicate / Delete / Group / Merge Down / Hide-Show / Lock-Unlock / Layer Effects (**Rasterize and Convert to Path were deliberately absent: neither command nor engine path existed yet — they landed with the features, 2026-07-09, and the menu shows each only on the kinds it can act on**); and a **width-resizable dock** — the splitter lives on the dock's *own left edge* (a sibling widget cannot paint over `VulkanCanvas`, which is an `Fl_Window`), `MainWindow::applyDockWidth()` places the body regions, `Settings::dockWidth` persists it on gesture end, and the wish is kept unclamped so the dock springs back when the window grows.
  - **FEEDBACK ROUND 1 (user, same day) — all fixed:** (1) **the rename field would not accept typing** and only drew right after a click. `clear_visible_focus()` looks like it merely suppresses the focus rectangle, but **`Fl_Widget::take_focus()` returns 0 when that flag is clear** (verified empirically against FLTK 1.4), so the field never got the keyboard. Removed; the app's global `Fl::visible_focus(0)` is the knob that actually suppresses focus rings. (2) **The editor was being painted over.** It is a sibling floating above one row, and FLTK repaints a damaged child alone — so `m_scroll->redraw()` or a row's hover redraw wiped it. Row-list repaints now funnel through `LayerPanel::redrawList()` / `LayerRow::requestRedraw()`, which damage the whole panel while an edit is live (children then draw in order, editor last). That also explains the "finicky hover-off dismiss": the editor was never dismissed, just overdrawn. Dismissal is now one rule — a press anywhere in the dock outside the editor lands the edit. (3) **Lock icon always drawn** (muted when unlocked); the hover-reveal cell read as a hole. (4) **History rows now follow dock-width changes** (`Fl_Scroll::resize` moves its children but never re-widths them → `HistoryPanel::layoutRows()`). (5) **Age captions no longer advance on hover** — `relativeTime()` was recomputed inside `draw()`; the row caches its caption and only the re-tick (or `setTime`) moves it. (6) **Icons were blurry**: a 24-unit viewBox rasterized at 16/18px put every straight edge on a half-pixel. Re-authored on a **16×16 grid, drawn at exactly 16px** (`ui::kIconPx`, no longer a parameter), 1px strokes on half-integers / 2px on integers; the curve-only eyes carry 1.5px so they have a solid core instead of a grey smear. Measured fully-inked fraction: 1.00/0.96/1.00/0.65/0.44/0.33 native vs 0.00–0.47 off-grid, pinned by `tests/test_icon.cpp`. (7) **Thumbnails now frame the layer's CONTENT**, not the document — a small object on a big canvas was an invisible speck. Content bounds through the world transform, squared + padded, magnification capped at 4× so a one-pixel layer reads as a dot; sampling only inside the frame *is* the culling. (8) **Text (and 3D-text) previews were blank on open**: a `TextLayer`'s pixels live in a renderer-filled cache, the panel builds its rows before the first composite, and filling the cache bumps no content revision — so the placeholder stuck for ever. `presentDocument` now calls `ensureTextCaches()` **before** `setDocument()`, and the thumbnail cache keys on the text cache pointer + size so a late arrival always invalidates.
- [x] S16-h — **Move-gesture 1px size jitter FIXED** (user 2026-06-13, S16-e feedback). `snapCropRect` rounded the two edges independently (`lround(x)` vs `lround(x+w)` step at different sub-pixel thresholds), so a pure *translate* oscillated the snapped width between `floor(w)`/`ceil(w)`. Now rounds **origin and size separately** (`x0=lround(r.x)`, `w=lround(r.w)`) → the snapped size is translation-invariant and a Move can't flicker W/H (trade: a left/top-edge resize may wobble its pinned far edge 1px, far less noticeable). One fix covers the crop HUD, the status-bar readout AND apply (all route through `snapCropRect`). Audited the S15 Move tool (continuous float transform, no integer endpoint readout) and the status bar (already `lround(r.w/r.h)` directly) — neither flickers. New translation-sweep test; snap goldens unchanged.
- [x] S16-i — **Marquee selection move + nudge** — **DONE 2026-07-09.** Modifier-free press inside the ants grabs the selection (four-way move cursor on hover); arrow keys nudge 1 doc px, 10 with Shift. Decisions settled: "inside" = coverage >= `core::kAntsCoverageThreshold` (128), the same iso-contour `canvas_present.comp` draws the ants along, so the grabbable region is exactly the one the user sees enclosed; press-time modifiers keep their S14 boolean-op meaning, which is how one draws a new marquee from inside an existing selection; a committed translation CLIPS at the document edge (the mask is document-sized) and pushing all of it off commits "no selection" — but `SelectionMoveGesture` always translates its PRESS-TIME copy of the mask, never the previous result, so a drag out over an edge and back is lossless; a move never combines, it replaces; nudge axes are DOCUMENT axes (integer doc px; a screen-up arrow under view rotation would mean resampling the coverage). A drag is one undo step on release; an arrow burst coalesces into one via a coalesce id on `SetSelectionCommand`, ended whenever `Document::selectionRevision` is not the one the canvas's own last commit produced. `SetSelectionCommand` gained a History label, so these read "Move Selection". **FEEDBACK ROUND 1 (user, same day): diagonal nudge did nothing diagonal.** The arrow handler read only `Fl::event_key()` — the key that fired the event — and the window system auto-repeats only the key pressed LAST, so holding Left and adding Down stopped Left's repeats and walked straight down. It now reads the whole arrow-key state per keydown (counting the firing key unconditionally, in case a backend's held-key query is unreliable); opposing arrows cancel to zero and consume the key rather than pushing a no-op undo step. (user 2026-06-13: "literally no way to move the marquee selection"). The core missing interaction: with a marquee/lasso selection active, **drag inside it** to reposition the selection *outline* (the mask, not the pixels — distinct from the Move tool), plus **arrow-key nudge** (1px; Shift = 10px). Lands as a `SetSelectionCommand` over the translated mask with a live frame-coalesced preview (the ants follow). Decide the grab affordance (inside-the-marquee cursor, modifier-free) and the edge cases (clamp vs allow off-canvas; combine-mode interplay). Really selection-tooling — could pair with **S18**'s Select menu, but pulled forward as it's fundamental.
- [x] S16-j — **Crop overlay correct under view rotation** (user 2026-06-13). `hc01`/`hc23` are screen px, so a rotated view spins the crop quad on screen; both overlay bits had assumed an axis-aligned box. (a) The **rule-of-thirds guides** (collapsed/duplicated at 90°, non-uniform at 45°) now draw **parametric between the opposing quad edges** (`segDist` of interpolated corners), so each line rides + rotates with the rect — **DECISION (recorded):** the guides **rotate with the crop rect** (Lightroom/PS — the grid aligns to the framed result). Dropped the axis-aligned pixel-snap for `segDist` AA, as the transform-handle edges already do. (b) The **size HUD** now anchors below the quad's **screen-lowest corner** (max corner Y) centred on the **centroid X** (was the doc-space "bottom" edge `hc23`: at 90° it sat on the left edge, at 180° it overlapped the box).
- [x] S16-k — **Crop size HUD rendering polish** (user 2026-06-13). (a) The HUD lowercase set (p,x,i,n,c,m) was redrawn to share one **x-height (rows 1-5) + baseline (row 5)**; the only descender, 'p', now extends **below** to row 6 instead of floating up. (b) Punctuation (`.`/`·`) got a **tighter per-glyph advance** (`hudAdv`, dots moved to cols 1-2) so "7.0" nestles the dot between the digits; `drawHud` now walks variable-width slots rather than a uniform grid. (c) **Zoom/box-size awareness:** the HUD **hides** when the crop box's smaller on-screen dimension drops below ~56 logical px (zoomed-out dwarfing); the rect/handles/guides stay and the font keeps its readable size when shown. *(Chose hide-below-floor over follow-the-cursor; the latter is still open if wanted.)*
- [x] S16-l — **Options-bar custom-ratio layout + decimal input** (user 2026-06-13). (a) New `ToolOption::joinPrev` binds a control tightly to its predecessor (no `kGroupGap` before it) and renders its label as a **centred, enlarged SEPARATOR** between the two controls — crop's `W : H` is now one tight group with a centred colon; an **empty label takes no caption space**, so the Ratio combo / custom fields / toggles are evenly spaced (`join()` helper mirrors `secondary()`). (b) A `NumberInput` (`Fl_Float_Input` subclass) accepts a typed `,` (inserts `.`), and parse/format are **locale-independent** (`std::from_chars`/`std::to_chars`, normalise `,`→`.`, drop a leading `+`) — fixing comma-locale decimal entry under `setlocale(LC_ALL,"")`. Applies to **every** Number control.
- [x] S16-m — **Crop/overlay visual feedback round 2** (user 2026-06-13, 5 commits). (1) **Canvas-edge AA** — the present pass's hard doc-boundary test staircased a rotated canvas; now coverage-AA'd (doc-rect SDF feathered ~1 screen px). (2) **Crop-shield AA** — the dim-outside shield used a hard `insideControlsQuad`; now `controlsQuadDist` SDF feathered ~1px (outline/handles already AA via `segDist`). (3) **Colon nudge** — custom-ratio `:` box raised 2px. (4) **Overlay-text rework (headline)** — the crop HUD + dial readouts used a hand-rolled 5×7 bitmap font (pixel-art); replaced with the **real UI font** rasterized by FLTK (`Fl_Image_Surface`) into a new RGBA overlay-text texture (present-pass binding 3, fixed-capacity upload mirroring the mask), cached by string; the shader deletes the bitmap-font apparatus and just composites the tile (`overlayCenter` = content size, `overlay` = {dial active, radius, angle, HUD active}, dial centre = viewport centre). Real `× · °` from the font. (5) **HUD clamp/park** — positioning moved into the shader: below the rotated quad's screen-lowest corner, centred on the centroid X, **parking near the BOTTOM** when the box is taller than the view (reverses S16-k(c)'s hide-when-small, per user; the top park felt like a jarring teleport on zoom-in, so 6f93a52 moved it to the bottom). *(S16-k(c)'s small-box hide is thus superseded.)* **Round-2 follow-up (6f93a52):** crop-shield feather widened to ~3px `smoothstep`; rotation dial gets a bigger font (~18px, explicit `fontPx`); double-tap-R reset flashes the dial ~0.55s with a "Reset" label. Crop-box staircase verified resolved by the 2026-06-14 user screenshots. **Round 3 (2026-06-14, design consult):** the round-2 "staircase resolved" was premature — the outline's `smoothstep(1.1,0.4)` core held a full-white plateau, snapping the white core 1↔2px on shallow rotated edges; replaced (`transformHandles`, shared by crop + Move) with a linear 1px **coverage** ramp (`clamp(1.1-edge,0,1)` + softer halo), guides too; offline-verified smooth at 0°/9.6°/33°. Dial **ticks** redrawn as uniform `segDist` radial segments (were angular wedges → diagonal "needles"). **Apply/Cancel** restyled per the user's pick — solid accent-blue primary Apply + quiet neutral Cancel (red dropped: Cancel isn't destructive). Text outline on the dial was dropped (experiment).
- [x] S16-n — **Tool-options-bar overflow (chevron + popover)** — DONE 2026-06-16 (visual pass pending). The bar no longer **drops** the controls it can't fit: a **drawn** double-chevron button (`OverflowChevron`, `fl_line`, host-font rule) pinned just left of the right-anchored action buttons toggles an `OptionsOverflowPopover` (a `Popover` subclass) that lists the overflowed controls as a vertical stack — each left-fill **group** (a base option + its `joinPrev` followers, e.g. crop's "W : H") one horizontal row, so a group never splits across the bar/popover boundary. Two-pass fit: measure without the chevron, and if anything overflows, reserve its width and re-fit. The popover is a **child sub-window built before the main window is shown** (created next to the tool flyout, injected via `ToolOptionsBar::setOverflowPopover`), **re-populated** every `rebuild()` (its stale rows + their dead callbacks cleared first); the overflow controls' `Binding`s join `m_state->bindings` so `syncValues()` keeps both surfaces in lockstep; the pinned action Buttons (Apply/Cancel) never overflow. `emit()` gained a `rowY` param so the same control-creation drives both the horizontal bar and the stacked popover rows. **S16-o** reuses this affordance for the left toolbar.
- [x] S16-o — **Left-toolbar vertical overflow** — DONE 2026-06-16 (visual pass PASSED, user). The tool
  column now relayouts on resize (`LeftToolbar::rebuild`/`relayout`): when the slots can't fit above the
  bottom-pinned swatch it reserves a downward double-chevron (`OverflowChevronV`, `fl_line`, sized 26×22
  to mirror S16-n's chevron padding, pinned `kChevGap` above the swatch) that toggles a
  `ToolbarOverflowPopover` (a `Popover` of stacked tool buttons, carrying the toolbar's group dividers).
  Drop order = least-used last (bottom of the natural slot order); the **active tool is always kept
  visible** — if it would overflow it takes the last visible slot and the displaced slot drops out (pure,
  unit-tested `splitToolbarSlots` in `toolbar_layout.hpp`). The popover is a child sub-window built before
  show + injected via `setOverflowPopover` (S16-n pattern); a tool-change relayout is **deferred**
  (`Fl::add_timeout`) to avoid deleting an overflow button mid-click. Relayout only rebuilds when the
  split actually changes (cheap chevron re-pin otherwise), so a drag-resize doesn't re-rasterize icons.
  Also fixed `Popover::place()` to anchor via `top_window_offset()` so a variant flyout opened from a
  button **inside** the overflow popover lands beside that row (was top-left-mispinned).
- [x] S16-p — **Auto-switch tool after applying a crop** — DONE 2026-06-15 (landed in the S51-a **Tools**
  settings category). `Settings::cropSwitchToolAfterApply` (default false) → `RunOptions` →
  `MainWindow::m_cropSwitchToolAfterApply` (live via the `SettingsHost::setCropSwitchToolAfterApply`
  callback); `ToolManager` now tracks `previous()` (recorded in `setActive`, ignoring same-tool
  re-selects); `MainWindow::applyCrop` switches to `previous()` (→ Move if that was Crop) after the crop
  command when on. Settings → Tools shows a themed `CheckBox`. Tests: settings round-trip +
  `ToolManager::previous` tracking. **User visual pass: PASSED 2026-06-15.** **RESOLVED (2026-06-14,
  design consult) — this is a SETTING, not a hardcoded behaviour; no longer an either/or:**
  - **Default = industry behaviour: STAY on the Crop tool after Apply** (Photoshop / GIMP / Affinity
    Photo all keep Crop active — the box re-frames the new canvas, ready to re-crop or switch manually).
    The "exit-after-done" model is Lightroom / Capture One (crop is a modal develop step). New users get
    the expected default.
  - **Opt-in toggle = SWITCH away from Crop after Apply** (the user's preferred behaviour). When on,
    switch to the **previous tool, falling back to Move** if Crop was the first/only tool selected
    (restores prior context, non-arbitrary) — preferred over always-Move.
  - **Settings home is OPEN — do NOT assume "Annoyances"** (that section is scoped to the
    window-title / unsaved-state toggles; this doesn't clearly fit). Record as **"home TBD"**; S51 owns
    laying out a coherent settings IA and placing the accumulated toggles. See project-memory notes
    `crop-post-apply-tool-setting` / `settings-dialog-coherence`.
  - **Impl:** add the bool to `common::Settings` (default = stay on Crop); the switch fires in
    `MainWindow::applyCrop` after the crop `CompositeCommand` is pushed; for the previous-tool fallback,
    `ToolManager` must track the previously-active tool (record on tool change, ignoring re-selection of
    the same tool).
- [x] S16-q — **Crop initial-framing setting** — DONE 2026-06-15 (landed in the S51-a **Tools** category,
    `Settings::cropInitialFraming` "whole-canvas"|"draw" → RunOptions → `VulkanCanvas::setCropFraming` /
    `CropFraming` enum, live via `SettingsHost::setCropInitialFraming`). A **combo** so power users can change
    the default. Shipped: **"Whole canvas"** (default — current/industry) + **"Draw to begin"** (GIMP-style:
    `ensureCropRect` stages nothing, `m_cropRect` stays `nullopt` until the first drag; the overlay/HUD/
    `cropOptionsChanged`/`pushCropTool` all gate on `m_cropRect.has_value()` so there's no resting box or
    phantom handles, and Esc mid-first-draw clears back to nothing via `m_cropDrawFromEmpty`).
    **Diagram cards + Inset landed 2026-06-16 (user notes):** the Dropdown was replaced with **three**
    `ui::OptionCard`s — **"Whole canvas"** (full rect, thirds + corner handles, no dim), **"Inset"** (a centred
    **15% margin**, hardcoded `kCropInsetFraction` in `ensureCropRect`; the rest dimmed), **"Draw to begin"**
    (a partial in-progress rect drawn with the SAME chrome the app shows mid-drag — thirds + handles, NOT a
    dashed sketch — over a dimmed canvas, plus a white-core/black-outline crosshair cursor). Handles = the 4
    corners only, subtle accent (the app's full 8 read too loud at card size — user's 2nd-pass call); the
    crosshair is white+black on both themes (drawn as four 1px-offset black copies so every tip is haloed);
    thirds clamp to the rect's w-1/h-1 extent (no 1px overhang); the "sun" disc lightens on dark / darkens on
    light so it shows on both. `OptionCard` generalizes the Appearance `ThemeCard` (mockup
    `PreviewFn` + label + accent ring); the Tools pane first shipped as an `Fl_Scroll` but was **superseded by horizontal sub-tabs
    in S15-e** (a `SubTabBar` + per-tool sub-panes, cohesive with the app's Layers|History tabs — the user's
    IA call, so a long scroll never buries settings). **Caption rhythm convention** (`kCaptionGap`/`kSettingGap`): a caption hugs the
    control above it and the next setting starts a clear gap below, so a paragraph never reads as belonging
    to the control below it. Tested (build + ctest + gui-frames); USER does the visual pass. Home settled =
    Tools/Crop group, pairs with [[S16-p]].
    **Card-scene v2 (2026-06-16, user notes):** the framing cards now draw a tiny landscape (its own scene
    colours, not palette chrome) — daytime sky + **yellow sun** on light, night sky + **crescent moon** on
    dark (co-located upper-left so neither collides with the corner handles), coloured ground. Replaced the
    palette-derived sun that vanished/blobbed on light mode. **Also landed: a Tools/Crop "Clear the crop
    selection when leaving the Crop tool" checkbox** — `Settings::cropClearSelectionOnLeave` (default off =
    keep the staged rect across tool switches per S16-c) → RunOptions → `MainWindow`; when on, `onToolChanged`
    calls `resetCropTool()` as you leave Crop (live via `setCropClearSelectionOnLeave`). **The Tools sub-pane
    is getting tight** — that second checkbox has no caption; the next per-tool setting needs a per-sub-pane
    themed **scroll** (queued — see the resume memory).
- [x] S17 — Magic wand (research-first) — **BUILT + MERGED 2026-07-15** (`72b3608`/`ad10aeb`/`72b201f`, merged `6da865e`; research note `docs/research-selection.md`). Three slices: the pure core colour-tolerance flood engine (`core::wandColorDistance` + contiguous/global match, AA edge coverage, feather — the metric S21's bucket fill and S24's eyedropper now share), tool registration + options bar (Tolerance / Contiguous / Anti-alias / Sample source Active-Layer vs All-Layers), and the canvas + app wiring landing one `SetSelectionCommand` per click. *User visual pass owed.*
- [x] S18 — Select brush (research-first) + Select menu — **BUILT + MERGED 2026-07-15** (`1c6f480`…`3aacbe0`, merged `6da865e`; research note `docs/research-select-brush.md`). Five slices: Grow / Shrink / Feather / Smooth morphology in core and on the **Select menu**, a coverage-only `MaskStroke` in `core::brush` (paint-to-select reusing the S19 engine's walk for coverage alone), tool registration + options + canvas wiring, and the **`antsCirculate` marching-ants-direction experiment** the entry parenthesised as a hidden setting. The **edge-aware variant** (`ToolId::EdgeBrush`, the L1 grow-to-edges/solve-on-release brush) followed 2026-07-16 (`98fc592`, merged `b6ba79c`), narrowed to solve-on-release by deliberate constraint (see S18 in §9). *User visual pass owed.*
- [x] S18-b — Dev-grade image open/save (PNG+JPEG pull-forward of S41/S42) — **CLOSED: Open DONE 2026-06-19** (File→Open decodes PNG/JPEG via libpng+turbojpeg → one unlocked raster layer; `io::loadImage` + `tests/test_io`). The **Save half was never built as a pull-forward and no longer needs to be** — real `.mosaic` File→Save / Save As landed with S48 Build 1 (2026-07-08, `1dde8aa`), and raster **export** landed on the S41/S42 milestone track instead (Quick Export → PNG `4b692c5`, the generated Export As modal `0d98ca6`). Everything this entry was blocking — S18-d's unsaved title, the Annoyances toggle — is therefore unblocked and shipped.
- [x] S18-d — Unsaved-state window title (command-stack dirty marker + duration; "Annoyances" settings) — **DONE 2026-07-04** (origin/main `fb98be4`): dirty = a saved-position marker on the command stack (`CommandStack::isSaved`/`markSaved`; `Document::dirty()` derives — no boolean); pure golden-tested `formatWindowTitle` → `<doc> • unsaved[ for N min] — Mosaic`; Settings→Annoyances show-duration (on) + include-seconds (off). `markSaved()` is now CALLED by the real `.mosaic` File→Save/Save As (S48 document slice, 2026-07-08) — the dirty title is literally true about the disk.

**Phase 3**
- [x] S19 — Brush engine + presets + tablet — **BUILT as the four-arc rework, 2026-07-09 → 07-29; `docs/brushes.md` is the surface and is maintained per-slice, the plan is only the index.** — [x] S19-a (engine core + presets: the 2026-06-19 base — CPU stamping engine `src/core/brush/BrushEngine` [spacing-walked dabs, flow + per-stroke opacity cap, smoothstep hardness, active colour, pressure/tilt-ready dynamics] + GPU reticle in `canvas_present.comp` + one `SetLayerPixelsCommand` per stroke — then **Arc A** [curves/sensors → 6 mask generators → bitmap tips + dab-mask LRU → `StrokeAccumulator{Uniform,Colored}` × `PaintMode{Wash,Buildup}` + erase + blend modes + masking brush → stroke state] and **Arc D** [`ui::CurveEditor`, the `BrushPresetChip`, the Brush section in the right dock with real preset **cards**, `Tools → Brush` settings, the tip-tracing elliptical reticle, and the modal **`ui::BrushEditorDialog`** with a paintable scratchpad — `docs/brushes.md` §8.3, built 2026-07-29 `26f5f30`, feedback round 1 `f89408a`]. Per-option transcription then continued across §6.6b–6.6i: the paintop arc's 15 exotics, the **smudge** engine, Scatter/Mirror, Spacing/Sharpness, HSV colour dynamics, `StrokePainter` (sketch / hairy Sumi-e / curve / particle / experiment / hatching), Texture + Airbrush, and the dead-key/per-stroke-seed/BUILDUP-Opacity pass — conformance census **104 Exact / 7 / 6**. ⚠ **The one named remainder is the GPU stamping path** (`docs/brushes.md` §6.3): stamping stays CPU behind the engine API by decision, with `MaskGenerator::coverageAt` factored as a pure function so a GLSL mirror + parity lane can follow at S60-e. *Visual passes owed; the mutation sweep on the three no-build chunks is owed.* · [x] S19-b (tablet pressure/tilt input — **Arc C, 2026-07-10 → 07-11**: `docs/tablet.md`; a normalised `platform::TabletSample` + policy layer, the **XI2** backend proven against a live probe on XWayland, the **`zwp_tablet_v2`** Wayland backend, samples driving the stroke through `StrokeInput`/`StrokeState`, Settings → Tablet with the curve editor. macOS Cocoa/NSEvent followed at S58 [`a86e9e6`], untested by choice; Windows Ink is still S57's) · [x] S19-c (crisp pixels + pixel grid + screen-space checker — 2026-06-16)
- [x] S20 — Brush import (incl. Krita .kpp) — **BUILT 2026-07-10 as Arc B of the S19 rework** (research discharged 2026-07-09; `docs/brushes.md` §3 is the verified format reference and §7 the as-built ledger). Read-only ZIP walker → `.bundle` reader → PNG text-chunk walker → pugixml preset XML → **`.kpp`** reader + paintop mapper + `PresetProvenance` fidelity badge → the bitmap tip readers **`.gbr`/`.gih`/`.abr`/`.png`** → md5 tip resolution → `.myb` (the §6.7 remap importer) and the native **`.mbp`** container → `common::dataDir()`/`installedDataDir()` + the preset library (bundle scan, tip resolution) → the shipped **CC-0 default set** + `docs/credits.md` + census test. Library census reproduces to the unit over the shipped bundle (117/117 presets, 47 bitmap tips). **Imported third-party brushes stay uninspected by policy** (§4.1) — we clear only what we redistribute, and provenance reports fidelity, never copyright.
- [x] S21 — Bucket fill — **DONE 2026-07-15 (worktree; user visual pass owed).** Core flood engine `core::bucketFillCoverage` (`src/core/fill.{hpp,cpp}`, pure/unit-tested — shares the S17 `wandColorDistance` metric; contiguous 4-conn scanline flood or global match; solid interior + optional 1px outer AA feather) + the interactive Bucket tool canvas path (`VulkanCanvas::pushBucketFill` → `MainWindow::bucketFillClick`): click → flood → intersect with the active selection (whole layer if none) → fill the active foreground via the shared S39 `render::computeFill` → ONE `core::FillCommand` ("Fill"). Options: Tolerance/Contiguous/**Anti-alias**/Opacity; respects layer lock. **Pattern/gradient fill already lands via Edit ▸ Fill (S39).** See `docs/bucket-fill.md`.
- [x] S22 — Gradient tool (editable gradient layer) — **BUILT 2026-07-15 (worktree, pending merge + user visual pass).** The interactive tool over the existing S25 vector gradient stack. `ui/gradient_gesture.*` (pure, FLTK-free, unit-tested) turns a doc-space drag into a **full-bleed `RectShape` + `vec::Gradient`** `VectorLayer` — the "gradient layer" `docs/vector-model.md` §1 prescribed: **editable** (re-selecting the tool on it reconstructs the handles from the gradient transform and re-drags *the same* gradient), **maskable** (the base `Layer`'s `RasterMask`, free), precision-independent (compositor renders vector at target res), and it serialises through the existing docjson vector path. Four shapes **Linear / Radial / Elliptical / Conic** → three model types (**Elliptical = a Radial with an anisotropic transform**; `gradientShapeOf` tells them apart by axis length on re-edit). On-canvas gizmo: square end handles + a round midpoint move-handle, riding the existing `setLineGizmo` overlay lane (Gradient never coexists with Shape/Move); hit-test + handle math are pure + tested. **Blend curves** = a per-stop `GradientStop::midpoint` (Photoshop diamond / CSS `<color-hint>`, `blend = f^(log0.5/logm)`); default 0.5 is an exact identity so every pre-S22 gradient renders + serialises byte-identically (docjson writes the 6th stop element only when off-default; reads 5-or-6). Context bar: Type + "Stops…" (opens the reusable `GradientFlyout`) + Opacity (the layer's). Authoring = the Affinity live-preview-layer pattern (one `AddLayerCommand`); handle re-drags coalesce to one `SetVectorObjectCommand` per gesture. Lineage in `docs/gradient-tool.md` (gradient math = 1985–2003 PostScript/SVG/CSS standards + Photoshop 1990 / CorelDRAW 1996 interactive fills; **gradient MESH deliberately NOT built**). +12 test cases (1652→1664). **Owed a visual pass:** elliptical minor-axis handle + ellipse outline overlay + flyout midpoint diamonds.
- [ ] S23 — Eraser + Blur/Dodge/Burn + Smudge — **partly done; what is left is genuinely only tool wiring.** ✅ The **Eraser** shipped with the S19 rework (`177c793`, 2026-07-10 — `StrokeMode::Erase` wired to the canvas, size tied to the brush, plus the `Tools → Eraser` preset-follows-brush setting); ✅ **Smudge** exists as a full brush **engine** (colorsmudge transcribed, `8a87bf4`, 2026-07-14 — `docs/brushes.md` §6.6c) reachable as a preset, but **not** as its own `ToolId`. ❌ **Blur / Dodge / Burn are not registered tools** — the Smalti icon pack already carries art for all three (they were drawn against `docs/icons-needed.md`, which is an inventory, not a claim), and each reduces to a `kToolDefs` row plus a per-dab operator over the pre-stroke snapshot on the S38 clone-stamp pattern. Decide at build time whether Smudge also gets a slot of its own or stays preset-only.
- [x] S24 — Color-picker tool + loupe — **DONE 2026-07-15 (worktree, merged; user visual pass owed).** Eyedropper sampling (`core::sampleColor` box-average over Point/3×3/5×5/11×11, reusing the Magic Wand's Active-Layer / All-Layers source resolvers so the two tools' Source can't drift) into the foreground — Alt/right → background, live-drag sampling — plus a **GPU loupe**: a circular present-pass magnifier (`shaders/canvas_present.comp`, own SSBO channel/binding 10) that nearest-neighbour magnifies the doc under the cursor with its own pixel grid, centre cell, swatch band + hex/RGB readout, OS cursor hidden. Reuses the S19-c crisp-pixel snap + the brush-reticle overlay machinery. See `docs/eyedropper-loupe.md`. +7 tests (→1659).

**Phase 4**
- [x] S25 — Vector layer infrastructure — **CPU path** (value model · `flatten` seam · hit-test ·
  scanline rasterizer w/ AA, gradient fill + full stroke · `SetVectorObjectCommand` · compositor
  render at target res; see `docs/vector-model.md`). *GPU-resident renderer + stroke Inside/Outside
  alignment deferred (the "ceiling").*
- [x] S26 — Shape tool — **DONE 2026-06-24 (`396206d`…`b8eeeeb`).** S26-a authoring (5 shape variants) + **S26-b** select-to-edit (click a shape → live edit; the Move tool selects/moves vector shapes; fill=fg/outline=bg), the parametric **resize-vs-transform** handles ("Scale stroke" toggle), a paint-preview swatch in the options bar, and the **shape-designer popover** (§7.4 — on-diagram drag handles for rect/polygon/star + ellipse arc handles, up-bubble, dashes/ranges). A Krita-style `ScrubSlider` was added for the options bar along the way.
- [x] S27 — Line tool — **DONE 2026-06-24** (landed under the S26 vector work): line paint modes (Solid/Hollow/Outlined, §7.5) + a **dedicated Line gizmo** — endpoints + a round bend handle (`b8eeeeb`).
- [x] S28 — Pen/custom path tool — **DONE 2026-07-28 (`c03a0b5`).** Bézier authoring (corner/smooth
  nodes, Alt-cusp, Shift-constrain, click-first-node to close), node + handle editing on a committed
  path, de Casteljau segment insertion, and the S27 custom stroke (width/cap/join/dash). Commits as
  one `AddLayerCommand` by reusing the Shape tool's draft path — no new document-side code. Pure core
  in `ui/pen_gesture.*` (zero FLTK references, so it cannot fetch a coordinate itself); all events
  enter through `VulkanCanvas::handle()` and every point through the existing `eventDocPoint()`.
  ~~**Boolean ops deliberately deferred**~~ — **BUILT the same day, second batch**: the new
  `Geometry` alternative (`BooleanCompound`, which makes `Geometry` recursive) plus
  `core/vector/boolean.*`, taught to `flatten`/`to_path`/`hit`/docjson, exposed as
  `Layer ▸ Combine Paths`. The kernel snap-rounds onto an integer lattice, so `orient2d` is an exact
  `int64` determinant and coincident edges are resolved by arithmetic rather than a tie-break —
  which is the objection the deferral was made on. **Divide** (the multi-object op) is still out.
  Rationale and the whole design now in `docs/vector-model.md` **§9**. Convert/rasterize needed no
  new code. The **node/handle chrome was rebuilt** in the same batch onto its own overlay lane
  (binding 6): every node's handles drawn *and* grabbable, transform-chrome knob language, a spine
  in edit mode, and a closing-loop ring (§8). Visual pass owed on it.
- [x] S29 — Type tool — **DONE 2026-06-25 → 07-03 (~25 commits, `2357666`…`4b5d11f`); user-verified, rounds CLOSED.** `docs/type-tool.md`. **-a** text model + fonts + HarfBuzz shaping + emoji + CPU render (headless); **-b** on-canvas editing — I-beam cursor, edit session, GPU caret/selection, Type-edit box, crisp vector-stretch, drag perf (rounds 1–5b); **-c** Type panel + context-bar split + selection-style core + custom font picker w/ in-face previews + live-edit perf (R1–R3). Then every deferred/advanced follow-up shipped: per-paragraph **language** attr, **hyphenation** (libhyphen), **spell-check** (enchant backend + red squiggles + right-click suggestions + Add-to-Dictionary/Ignore), **vertical writing-mode** + editing geometry (arrows follow the column, side baselines), **variable-font axes** (engine + per-face panel sliders), **OpenType feature toggles**, **metric/optical/none kerning**, and a Settings→Text **emoji-font picker** (R4–R5). 571 tests green release+ASan.
- [x] S30 — Type tool (advanced 2D: warp · fit-to-path + range handles · rasterize) — **BUILT 2026-07-05 → 07-07, hardened through 07-29; `docs/type-tool.md` §9 is the surface.** The 3D arc (S30-c/-d) leapfrogged it and the hyphenation/spell-check/vertical/variable-font work landed under S29, so this entry closed out of order. As built: **baseline bend/warp** — one on-canvas drop-tab handle warping the baseline along a smooth arc, glyphs placed by position + tangent (`bcc9343`); **fit-to-path** — a `PathFit` model holding a `LayerId` *reference* to the path so editing the path re-flows the text non-destructively, arc-distance placement, curved editing geometry, and the on-canvas click-a-path-to-type-on-it entry with sliding start/end/centre-flip **brackets** (`b10c05e`/`22c3841`); **Rasterize + Convert to Path** on the layer context menu (`5d1efa0`, 2026-07-09 — the two items the menu had been withholding because a greyed item is a promise). Warp composes *into* the extrude, so a bent or path-fitted baseline drives the 3D solid too (`c828779`). Two later correction rounds are part of the as-built and worth reading before touching it: **an Area block's bend is the FRAME's arc, not the text span's** (`269996e`/`a8e25f3`, 2026-07-14 — every line rides a parallel arc at its own depth, two type sizes in one box bend identically, and the overset clip became the warped sector), and **the bent block's chrome is ONE geometry** (2026-07-29, `bb65d19` — every piece of chrome goes through `BentArc::warp`, bitwise identity when unbent). *Visual pass owed.*
- [x] S30-c / S30-d — 3D type — **BUILT + PUSHED 2026-07-03 (~14 commits, `cc0bc3a`…`23e2f09`); gizmo user-praised, a final visual "round 4" eyeball is the only thing owed.** **S30-c** engine: 3D math kit (`common/geometry3d.hpp`), vendored earcut, the Extrude model (`optional<Extrude>` on TextBlock), a watertight extrude mesh (4 bevel profiles, design-space UVs, per-run material ranges), a **CPU** z-buffer/Blinn-Phong render lane INTO the text pixel cache (scale-true z=0 plane), and a **Vulkan compute-raster lane with GPU/CPU parity**. **S30-d** the un-greyed "3D…" popup: live viewport, free trackball, XYZ rotation rings, depth/bevel drag handles, light-direction sphere — every edit through the block funnel so the canvas IS the live preview and undo is one step per gesture. **3 feedback rounds** landed: chrome shading + canvas reflections (+ a "Sides only" mode), GPU-lane readback perf fix (108→10.4 ms/frame), mutually-exclusive Style/3D popups + shared colour paradigm (SwatchChip/ColorFlyout), per-vertex bevel-inset clamp, resize-crash + drag-accumulation fixes, AA gizmos + hover, 3D-faithful editing chrome (front-cap homography — caret/selection/box project onto the solid), back-face selection, and presets (Chrome/Gold/Copper/Steel/Plastic/Matte) + intensity. ~604 tests green release+ASan. Lineage: WordArt/Dimensions-era extrusion, Blinn 1977, classic ear clipping.
- [x] S30-e — 3D-text Layer-Effects integration — **BUILT 2026-07-16 (awaiting user visual pass).** The §12 per-face mapping: overlays evaluated in glyph design space → per-material overlay-albedo maps (`core/text/extrude_overlay.*`) sampled via the mesh UVs by both render lanes — front cap by default, walls/bevels behind the new `Extrude::overlayWrapSides` ("Wrap effects onto sides", 3D panel), never the back cap; the design is lit with the surface. The compositor strips the consumed overlays from `applyEffects` for extruded text (no smear over the projected rectangle) while shadows/glows/strokes still composite in 2D, per the doc. Overlays joined the text-cache validity key (`TextLayer::cachedOverlays`); 3D-popup viewport + LE-modal preview carry effects; GPU parity green incl. wrap mode. Flat text byte-identical.
- [ ] S30-b — Vector document type (deferred; gated on S48 `.mosaic` format + the New-Document redesign)

**Phase 5**
- [x] S31 — Layer masks + mask from selection — **BUILT 2026-07-16 (worktree, pending merge + user visual pass).** The full raster-mask story over the long-standing `RasterMask{coverage, enabled, linked}` model. **Core:** `revealAllMask` / `maskFromSelection` / `selectionFromLayerMask` (the mask grid = the layer's LOCAL grid — raster/magic: the source image; every other kind: the doc window — selection coverage resampled through `worldTransform`, so the mask reveals exactly the DOC pixels selected whatever the transform); commands `SetLayerMaskCommand` (add/replace/delete), `SetMaskEnabled/LinkedCommand`, `SetMaskPixelsCommand` (region-scoped coverage patch, `dirtyRegion` through the fold transform → rides the S60-a scoped recomposite); `Layer::maskRevision` (the dock's mask-thumb cache key). **Compositor:** `linked` honored everywhere at last — linked masks fold pre-transform byte-identically to before; UNLINKED masks fold after placement, fixed in the layer's parent space (`foldMaskThrough`/`foldUnlinkedMask`; leaf + group + vector + merge-down; the drag cache refuses them); **vector layers now fold masks** (the S25 follow-up — at target res through `place⁻¹`, so the S22 gradient layer is maskable for real); **masked groups moved to the content-extent buffer** (the fold threads the offset), fixing the window-aligned mis-fold under REGION composites (the acknowledged S31/S60-a debt). **UI:** Select ▸ **Mask from Selection** (the §4.5 entry, absent till now; status-bar refusals); the dock's **mask thumbnail** + link **chain** in the gap (click toggles) + disabled **X** + accent **target ring**; click the mask thumb to aim edits at the mask / pixel thumb to re-aim (Shift-click the mask thumb selects its coverage); context menu **Add / Delete / Disable–Enable / Unlink–Link Mask** (Add seeds from the active selection, PS button semantics); **mask painting** — Brush/Eraser redirect through an opaque-gray RGBA proxy so the whole S19 brush stack works on masks (white reveals / black hides / eraser carves to hidden; Rec.709 luma readout; one `SetMaskPixelsCommand` per stroke; Inpaint brush stays on pixels). Text layers refuse masks for now (their cache grid swims). +24 tests (1677→1701 cases, 1,072,541 asserts).
- [x] S32 — Non-destructive adjustment/filter framework — **BUILT 2026-07-17 (awaiting user visual pass).** Typed param schema (`core/adjustments.*`) over the params bag; full scalar math (Levels / Exposure-in-linear / Hue-Sat / Color Balance / Threshold / Posterize; Curves waits on its S34 editor; defaults = byte-level no-op); `SetAdjustmentParamsCommand` w/ per-control coalescing; Filter ▸ Adjustments insert-above-active; the editor = a pinned corner popover on the Type-panel pattern, shown while the adjustment layer is ACTIVE (schema-generated rows, live one-command-per-edit undo, Reset, drift re-sync); dock half-circle chip badge + "Edit Adjustment…" context item. docs/adjustment-layers.md. See §2.
- [x] S33 — Filters: blurs — **BUILT 2026-07-17 (`456cb74`, gizmo kinds `f32c463`/`8e17401`/`b9c14dc`, angle params on `ui::Dial` `1d12539`); `docs/blur-filters.md` is the surface.** **Seven** spatial `AdjustmentKind`s rather than the five this line asked for — Gaussian / Box / Motion / Radial (Spin + Zoom) / Surface / Lens / **Depth of Field** — end to end as S32 filter layers: schema tables, docio tokens, **Filter ▸ Blur ▸ …** with visible (non-identity) defaults, centres seeded to the document centre on insert. CPU kernel engine `render/blur.{hpp,cpp}` is the golden lane (premultiplied, clamp-edge, alpha diffuses; Lens gathers in linear light; DoF is a signed-distance band field interpolating a 5-level pyramid, each level blurred **from the source**); the compositor's spatial branch keeps `region == crop(full)` byte-exact, and `blurAdjustmentReach` sums stacked reaches. On-canvas **gizmos** in the crop-chrome language (DoF focus line + band/feather edges + move/rotate knobs; a Radial crosshair; the `Ring` kind S34-a later reused for Vignette/Ripple), draft-composite during a scrub with a full-quality settle. **Vulkan compute lane** `render/blur_gpu.*` + 4 `shaders/blur_*.comp` behind `setBlurRenderOverride`, parity-pinned on RADV — Box/Motion/Radial deliberately **refuse** the GPU because readback costs more than the CPU kernel. ⚠ Standing design constraints are pinned as code comments at the boost pre-pass, the DoF pyramid and the gizmo. *User visual pass owed.*
- [x] S34 — Filters: color (incl. grayscale) — **BUILT 2026-07-29 (awaiting user visual pass).** Curves end to end (indexed-knot storage in the existing double bag, composite + per-channel curves composed into one 256-entry LUT per channel, `ui::CurveEditor` plot + channel picker, one undo step per gesture); Shadows/Highlights (SPATIAL, non-linear masking, region==crop(full)); Defringe (hue-band chroma suppression + user-set lateral-CA radial rescale — ⚠ **no** edge/detection term, and never estimated from the image; both are standing constraints); Matte Removal (Porter–Duff algebra, 4 modes); Haze Removal (Koschmieder inversion at CONSTANT transmission; ⚠ the dark-channel prior is deliberately NOT shipped). `adjustmentImplemented` is now true for every kind. **Frequency Separation DEFERRED** (a layer-creating workflow, not an adjustment kind — docs/adjustment-layers.md §8). docs/adjustment-layers.md §2.1–2.5.
- [x] S35 — Filters: artistic/stylize — **BUILT 2026-07-29 (awaiting user visual pass).** Nine kinds in a self-contained `render/stylize.{hpp,cpp}` module behind one compositor branch (`isStylizeKind` / `applyStylizeAdjustment` / `stylizeAdjustmentReach`): Sharpen, Unsharp Mask, Add Noise (hash-seeded on the PARENT pixel, so region==crop(full) and the grain does not crawl under pan/zoom), Denoise (Lee 1980 MMSE — ⚠ NLM/BM3D deliberately declined), Pixelate (parent-anchored lattice, serial cell accumulation for region byte-equality), Emboss, Oil Paint (⚠ the *original* Kuwahara 1976, never the anisotropic variant), Wave/Ripple, Vignette (linear-light exposure falloff, user-set only). Filter ▸ Stylize. **Deferred:** Crystallize, a median denoise method (cost), a GPU lane, canvas gizmos for the Vignette/Ripple centres. docs/filters-stylize.md.
- [x] S34-a — Filter remainder — **BUILT 2026-07-29 (awaiting user visual pass).** The Curves editor's backdrop histogram (a REUSE of `HistoStrip`'s data path; all four channels built in one pass; ⚠ the invariant holds — it is drawn, never derived from); Gradient Map (stops as indexed doubles per the Curves precedent, absent = the default black→white ramp, dither fixed at None so `region == crop(full)` survives); Vibrance (identity at 0); Photo Filter (15 presets + custom, linear-light, optional luminosity preserve); High Pass (spatial, reach-reporting). ⚠ New standing invariant for all three: never derive the setting from the image. Vignette + Wave/Ripple centre gizmos landed with the input session (`BlurGizmoKind::Ring`).
- [ ] S34-b — Filter gap: 3D LUT, Channel Mixer, Selective Color, the distort family, lens correction, Frequency Separation, the stylize GPU lane (⛔ see §9 S34-b for what must NOT be added)
- [x] S35-b — Mesh Warp + Perspective Warp — **BUILT 2026-07-30 (`9dbf82a`; `docs/warp-tools.md`).**
    Two tools sharing one slot (`ToolSlot::Warp`, shortcut **Q**, right after Crop, flyout on the
    Marquee/Lasso/Shape precedent). `render/warp.{hpp,cpp}` is the kernel and it is **inverse-mapped
    throughout**: the mesh is a piecewise **Catmull–Rom** surface (Catmull & Rom 1974) evaluated ONCE for
    the whole lattice, so adjacent patches share their boundary vertices exactly and cannot seam, then
    rasterised as triangles with the source interpolated barycentrically — each triangle's dest→source
    map *is* an affine, so its inverse and its footprint come free. Perspective is a single 4-point
    **homography** (8×8 elimination with partial pivoting) inverse-mapped with the per-pixel `w` divide,
    plus a horizon-sign guard and a non-convex/degenerate refusal; it is **never triangulated**, because
    a triangulated projective map seams along the diagonal. Sampling reuses `render/resample.hpp`'s
    kernel bank in premultiplied alpha, so it inherits that path's 8-texel footprint cap and says so.
    `core::WarpGrid` lives on the layer beside the mask with its own `warpRevision()`, and `warpImage`'s
    **two-grid form applies only the DIFFERENCE** — which is what makes re-entering the tool an edit
    rather than a double-apply. Live pixel preview: frame-coalesced Draft bakes from a pristine base,
    Final on release, one non-coalescing `SetLayerWarpCommand` per Apply. The overlay rides the
    **existing** polyline + pen-chrome lanes (curved isolines from the same surface evaluator, the Pen's
    anchor squares, screen-space hit radii) — **no new descriptor binding**. The lattice persists as an
    additive optional per-layer `"warp"` manifest node, schema version unmoved, malformed ⇒ refuse.
    ⚠ **HONEST LIMITS:** warps **BAKE**. Text/texture/vector layers are told to Rasterize and a masked
    layer is refused outright; within one session there is no compounding (every bake reads a pristine
    base) but each new session costs one resample. The original brief's claim that magic layers would
    re-derive losslessly from `source()` was **wrong and was dropped rather than shipped** — a bake has
    nowhere else to keep the original. The fully non-destructive route is a **compositor warp stage**,
    named as the follow-up along with mask warping. ⭐ *Visual pass owed; live-preview cost is unmeasured
    on a large canvas; a `warp_perspective` glyph is owed (both variants wear the reserved `warp` art).*
- [ ] S36 — Rasterize / rasterize-down — **layer-type half DONE 2026-07-28 (`c06e70b`): Merge Down
  now works across kinds** (shape+shape → one editable path when lossless, else rasterize both;
  gradient/text/texture/magic/group onto anything → bake; adjustment merged down bakes its grade into
  the layer below and narrates the scope change). Four *specific* refusals replace the old generic
  one. ⚠ The **"rasterize-down on an effect applies it to the layer below only"** half is still
  unbuilt — that is what remains of S36.
- [ ] S36-b — Selective undo (research-first: footprint API + dependent-closure removal, via the History panel)

**Phase 6**
- [x] S37-a — Inpainting research note (research-first) — DONE 2026-06-18 (`docs/inpainting-research.md`)
- [x] S37-b — Inpainting engine + backend interface + diffusion PdeBackend + ScriptBackend shim — DONE 2026-06-18 (`c336757`)
- [x] S37-c — Default backend: He & Sun offset-statistics graph solver — DONE 2026-06-18 (KD-tree NNF on the working region → α-expansion graph completion w/ boundary seam terms → copy-by-labels → plain Poisson seam blend → two-scale for large holes; `offset-stats` is the engine default; graph-cut+blend combination as designed)
- [x] S37+ — Inpaint quality + perf pass + Resynthesizer backend — **2026-07-02 (~11 commits, `752c07b`…`6c4b0aa`).** Post-S37-c refinements repro'd headless on 36MP photos: frame-edge strip fix, de-quantized full-res offsets, banded binary re-cuts, multigrid Poisson + guidance hygiene (kills the ghost/"dark-grass" class), stencil-form blend + one min-cut per band (**36MP fill 21→~13s**), boundary-driven candidate offsets + worst-seam escalation (the treeline-junction fix), and outpaint tuning (structure penalty + shift ladder + banded donors, §3.7.8). **Second engine added: Resynthesizer** (`1c8d12e`) — clean-room Harrison texture synthesis as an optional backend choice; wave-parallel + plugin defaults (`6c4b0aa`, **36MP fill 102→19.7s**, parity w/ the default engine). GPU DECLINED by user ("no GPU for minor gains"). User-verified ("honestly better").
- [x] S38 — Stamp/Clone tool (the renamed "Heal" slot — user 2026-06-19) — **DONE 2026-07-29**
  (`docs/clone-stamp.md`): `ToolId::CloneStamp`, shortcut **S**, own slot; Ctrl/⌘-click picks the
  source, painting stamps it through the brush tip on the S19-a engine (pre-stroke snapshot, one
  `SetLayerPixelsCommand` per stroke); Aligned + Sample (Current layer / Current & below / All
  layers). ⚠ **NO spot/blemish mode** — the PLAN's old "(+ spot/blemish mode)" and its
  cursor-following/snap-back heal choreography were a mistake and were deleted from §9 (user
  2026-07-29). ⚠ A clone stamp COPIES PIXELS: no healing, no seam blending, no texture synthesis —
  that family is S39's. *User visual/interactive pass owed (`docs/clone-stamp.md` §10).*
- [x] S38-b — Red Eye tool — **BUILT 2026-07-28 (`72a0286`, feedback rounds 1 + 2 `e78b918`/`6cf8d2c`); `docs/red-eye-tool.md` is the surface.** It shipped as a **universal eye-retouch slot with two variants**, not one red-eye tool: `ToolId::RedEye` (Tier 1 — flash red-eye: click or drag over an eye, mask the red pupil, desaturate + darken it) and `ToolId::RedEyeSclera` (Tier 2 — sclera de-redding / vein suppression), sharing one toolbar slot and one flyout. Localised correction as the entry intended, on the selection/mask plumbing, one undo step per application. Two feedback rounds followed the same day and both are part of the as-built: round 1 fixed the rim + local-white handling and the licence note; **round 2 found the flash rim was THREE defects, only one of which round 1 had fixed** (`docs/red-eye-tool.md` §9.8). ⚠ Shortcut: §2.5 of the doc said **R** was free and it was not — `VulkanCanvas::onKeyDown` claims bare `r` for rotate, so the tool took **Y**. Tier 3 stays out of scope. Corpus for regression eyeballing lives outside the repo. *User visual pass owed.*
- [x] S39 — Inpaint brush + Edit→Fill→Inpaint (core complete; refinements flagged in §9) — [x] S39-a (inpaint brush: `ToolId::InpaintBrush` [J], red mask overlay → `InpaintEngine::run` fill on release, one `SetLayerPixelsCommand` — 2026-06-19) · [x] S39-b (async: engine off the UI thread + status-bar progress/cancel + throttled live preview, canvas stays navigable; Settings→Inpainting category; adaptive small-selection working region + backend-agnostic sample-area preview — 2026-06-20/22) · [x] **Edit→Fill…** (transactional Fill **dialog**, Shift+F5 — `ui::fill_dialog.*`; Contents FG/BG/White/Black/50%Gray/**Inpaint**, blend Mode+Opacity+Protect-alpha, live preview pane; `render::computeFill`→`core::FillCommand`; Inpaint→Fill runs the async path — **2026-06-22**. Follow-ups: in-dialog Inpaint preview/cache + Colour…/Pattern contents, see §9 S39)
- [ ] S40 — Scripting infrastructure (Lua via sol2) + sane API + example inpaint-hook script (replaces the dropped ML inpainting model)

**Phase 7**
- [x] S41 — I/O framework + loss-warning system — **BUILT 2026-07-28 as export milestone M2 (`c9a8a85`); `docs/export-system-plan.md` §2/§4 + §10 line 2 is the surface, and the milestone ledger there is the index of record for everything in Phase 7's export half.** `src/io/` gained `format_backend.hpp` + `format_registry.*` (the registry, its Common/extended tiering and its availability gating, so an unbuilt optional codec is simply *absent* rather than a broken row), `caps.*` (`FormatCaps` + the **pure `diff()`** that is the whole loss-warning system — it emits an ordered `LossCode` set with a severity, and the UI is what translates and colours it, because `core/io` is gettext-free), `document_profile.*` (`DocumentProfile` extraction — the one place that knows how to ask a `core::Document` what an encoder needs to be warned about), and `options_schema.*` (the typed per-format option vocabulary with defaults, clamping, step snapping, `visibleWhen` conditions and a `validateSchema()` that names each class of author mistake). Four headless test files pin it (`tests/test_export_{options,loss,profile,registry}.cpp`) — including that a `diff()` which always warns **fails**. **Two things this milestone deliberately did NOT build:** `render::resizeImage` (specced in the doc §5 item 1; `compositor.cpp` was mid-rewrite for S60-a) and the generic options *panel* — M2 built and unit-tested only the data model. ⚠ The `src/io` → `src/core/io` relocation this milestone's original plan called for **was made and then reverted** (`4b692c5` moved it, `97cca43` moved it back on 2026-07-07): pure I/O under `core/` inverts the dependency, so `src/io/` is where the code lives and stays.
    ✅ **The metadata + ICC slice CLOSED 2026-07-30 (`9dbf82a`; `docs/export-system-plan.md` §7d)** — the
    debt M4 left, where the Export dialog's Metadata toggle and ICC row described behaviour that did not
    exist (`ExportRequest` carried no EXIF, `RenderInput`'s profile seam was never read). Both are real
    now. The **provenance rule** (`io::documentExif`) takes the earliest-minted, *effectively* visible
    raster/magic layer that carries a record — deliberately not the bottom-most, because dragging a layer
    must not rewrite which camera the exported file claims — and **orientation is forced to 1**
    (`io::exifForExport`), since load bakes it upright and records 1, so writing the original back would
    double-apply it in someone else's viewer. JPEG gained APP1 Exif, a **multi-segment** APP2 profile
    (real profiles exceed one 65533-byte segment — the classic bug) and JFIF density; JXL gained an Exif
    box and an original-profile tag. `RenderInput` now carries the profile as **bytes**, because a working
    space has no file and the trial encode runs off the UI thread. `diff()` took a defaulted
    `MetadataRequest`, so the banner still reports what the **format** cannot carry and never what the
    user chose to omit — every M2 golden held unchanged. TIFF EXIF is declared **absent** on purpose: it
    is implementable but not testable here (`extractExif` reads only PNG/JPEG, libtiff is PRIVATE to
    `io`), and a control that lies is worse than one that says no. ⚠ Known smell: `caps.hpp` now pulls
    `document_profile.hpp` → `io.hpp`, so the pure loss-diff header transitively sees the codec API (no
    cycle, no FLTK). ⭐ *Visual pass owed on the new Colour-and-metadata section.*
- [ ] S42 — Common raster formats — **most of the way there; the remainder is one named milestone.** ✅ **M4 (`b0cdd9c`, 2026-07-28)** added **WebP, AVIF, TIFF and GIF** — codec, decoder, `FormatCaps`, `OptionsSchema` and registry entry each, every dependency optional and gated at runtime (AVIF refuses to appear unless libavif offers libaom or SVT-AV1 — the never-rav1e decision enforced where packaging cannot enforce it). **PNG** (`4b692c5`, M1) and **JPEG + JPEG XL** (`b795fb9`, adapted into the registry at M3) were already there. Landing with M4 because those formats need them: **EXIF write-back** (`src/io/exif_write.*` — the owed half of the read slice `493b6af`), **ICC embedding** on export, a dependency-free **quantizer** (`src/io/quantize.*`), and PNG's own eXIf/iCCP/pHYs chunks. ⚠ **Those last two are `src/io` capabilities, not yet working controls** — as of `b0cdd9c` the rebuilt Export As dialog still hard-strips metadata and its ICC row is unwired (`ExportRequest` carries no `ExifData`; `RenderInput.iccProfilePath` is unwired app-wide), because a control that cannot act would have been a lie. Wiring them is its own slice and is **not** recorded here as done. Deliberately not built: JPEG-in-TIFF, animation in any format, and EXIF *reading* from the four new containers. ✅ **M5 CLOSED 2026-07-30 (`9dbf82a`; `docs/formats-curated.md`)** — the curated `libmosaicformats` tier this line's BMP / TGA / PNM / ICO belonged to, plus PAM, QOI and Radiance HDR. Six dependency-free codecs in the new **`src/formats/`** (target `mosaic::formats`, namespace `mosaicfmt`) whose CMake `DEPS` list is **deliberately empty**, so §2.2's "depends on nothing but a pixel-buffer view" is enforced by the build rather than merely intended (§11 tests them standalone, and the first six test files include only `formats/*.hpp` — the fence is visible in their include lists). **BMP** V3/V4/V5 at 32/24/16/8-bit with RLE8 and V5 embedded ICC; **TGA** v2 with RLE and the extension area that actually records straight-vs-premultiplied; **PNM/PAM** P1–P7; **QOI** clean-room from the published spec (Szablewski 2021), bit-exact; **ICO** with the doubled-height DIB and the AND mask, BMP *and* PNG payloads (compact only because the DIB parser/writer is **shared with `bmp.cpp`** — one hostile-input surface, not two); **Radiance HDR** RGBE with adaptive and old RLE. Encode **and** decode, each behind one adapter (`backends/mosaicformats_backend.cpp`) with a real caps row and options schema, reachable from `File ▸ Open` too — export-only would be half a format. Every decoder is truncation-swept on exact-size allocations. ⚠ **HDR is honest about not being HDR yet:** no tone-mapper was invented, caps say 8-bit on a high-dynamic-range format, and real float output waits on the §5 `ImageF` tap. ⚠ **The library sits at `src/formats/`, not the `src/core/io/mosaicformats/` the plan had specified since 2026-07-04** — that path was written while `io` was briefly under `core/`, a move since reverted (see S41 above); a library that must depend on nothing cannot live inside the target it is meant to be independent of, and `src/io/mosaic/` is already the native format. Doc corrected in place. ⚠ **Named follow-up:** a **per-option** caps model — BMP's depth, PNM's variant, QOI's channels and TGA's attributes all change what a file can carry, but caps describe a backend, not backend-plus-options, so choosing a narrower depth currently under-warns and choosing PAM over-warns; one fix would also cover JPEG-in-TIFF and the animation gaps. CUR/ANI stay with the exotic tier (M7). ⭐ *Visual pass owed on the new backend-settings rows.*
- [ ] S43 — HDR — [ ] S43-a (float pixel pipeline) · [ ] S43-b (linear-light + ICC compositing) · [ ] S43-c (HDR formats + indicators + HDR output; **the backend default no longer waits here — it flipped to Wayland early, in S59-a**, so S43-c inherits a native-Wayland presentation path instead of having to flip one)
- [ ] S44 — Extended formats (HEIF optional, .ora, …)
- [ ] S45 — RAW import + camera-info panel
- [ ] S46 — PSD/PSB read (research-first)
- [ ] S47 — PSD/PSB write
- [x] S48 — `.mosaic` native format — **BUILT in two builds; `docs/mosaic-native-format.md` is the spec and §S48 above carries the design rounds.** **Build 1 (2026-07-07/08, `97cca43`…`b270b0e`)**: the container layers (chunk framing with the PNG-style transfer trap, xxh3-64 + BLAKE3 checksums, store/LZ4/zstd codec tiers with whole-tile Paeth and a real SSE2 decode lane, the 128 KB root slot + RPTR overflow + DIR directory + ×3 root replication + the slot→tail→full-scan recovery ladder, from-scratch GF(256) Reed–Solomon parity, `writeFileAtomic`), the document↔container bridge, the **recovery journal** (crash restore + autosave as a *sidecar* — only an explicit Save ever writes the user's file), the **advisory per-document lock** + read-only open, commit-append File→Save, retained linear **H2 history** loading into the History panel with a per-key `LiveUndoModel`, history-preserving compaction, and a self-verifying corrupt-corpus generator. **Build 2 (2026-07-22/23, merged `9e110d1`)**: **H4 content-addressed history** (cas mode, BLOB chunks) with **adaptive H2↔H4 switching** measured locally per document at checkpoint time (no telemetry), every full write folding history through so Save As keeps the walk and switches land passively, proactive early fold, **journal growth compaction**, the full write moved **off the UI thread** (`docs/async-save-design.md`), and **Flatten History** as the storage-reclaim escape hatch. 11/11 mutation battery; three review defects fixed pre-merge. ⚠ **The Windows write path is unimplemented, not merely un-compile-verified** — see S57 for exactly which three functions and what they cost. ⚠ **S48's own follow-on ideas stay unbuilt on purpose** (xor-delta history compaction H3, history-region RS parity — real, tested, available if ever needed; spec §3.8/§3.9/§6). *User's real-app interactive pass owed.*

**Phase 8**
- [x] S49 — Tabbed document selector — **DONE 2026-07-09.** `ui::TabStrip` under the options bar, over the canvas column. **Hidden while a single document is open** (user's call: a lone tab says nothing the title bar does not, and it would cost a canvas row for ever), so `tabStripHeight()` is part of the canvas's top, not a fixed offset. The ACTIVE document's state stays in the existing `m_document`/`m_journal`/`m_lock`/`m_commit`/`m_unsavedSince` register (≈250 call sites untouched); `m_sessions` backs the rest, with `unloadActiveSession()`/`loadSession()` spilling and filling it (zoom/pan/rotation included, so a tab returns where it was left). Every open document keeps its OWN journal + lock — a background tab is still crash-protected and still holds its file. A deliberate close discards the journal; a crash must not, which is why `closeSession()`, `~MainWindow` and the quit path all discard and nothing else does. `cbQuit` now goes through `do_callback()` (FLTK does not funnel `hide()` through it) so quitting prompts and tears the journals down. Opening an already-open path raises its tab instead of finding this window's own advisory lock and coming up read-only. Close via the tab X, middle-click, or File→Close (Ctrl+W); the last tab closes to the empty state. A dirty tab gets [Don't save] [Cancel] [Save] with Escape landing on Cancel; a dirty BACKGROUND tab is brought forward first, a clean one is torn down where it sits. The CLI now takes several positional paths (an "Open with…" multi-selection, a shell glob).
- [x] S50 — Drag & drop + magic layers — **DONE 2026-07-09.** The `MagicLayer` model was already whole (the compositor samples `source()` through the layer transform every frame, docio round-trips it, the clipboard and Shift-click-thumbnail know it); nothing CREATED one. Now: drop on the CANVAS → each file placed as a magic layer; drop on the TAB STRIP → each opens as a document; `File → Open as Layer…` wired (it was a `cbTodo` stub); a `.mosaic` dropped on the canvas opens as a document (a document is not a layer). Both surfaces are their own drop targets — the canvas is its own `Fl_Window`, so it takes FL_DND_* directly and the payload arrives as a DIRECT FL_PASTE that never bubbles — and the strip draws an accent frame while a file is over it, because the difference between the two meanings is otherwise invisible. Placement is `core::placedImageTransform`: shrink to fit, never magnify, centre. "Magic-layer resampling from original on transform" needed no new code, only a test proving the compositor already honours it (place a probe 1:1, scale to 1/16 — the composite loses the probe pixel — scale back, and the composite is byte-identical). **FEEDBACK ROUND 1 (user, same day): drops on the canvas were refused before the canvas ever saw them.** `MainWindow::handle` returned 0 for every `FL_DND_*` while a document was open — a leftover from when the drag genuinely had no target — which tells the drag source "not here" and ends it. Fixing it needed knowing how FLTK actually dispatches DND, and the answer was probed rather than assumed: **FLTK hands a DND event down to a child SUB-window (the canvas is one) but NOT to a plain child widget (the tab strip is one)**, so merely delegating to `Fl_Group::handle` would have fixed the canvas and left the strip silently broken, with its `FL_DND_RELEASE` misdelivered to the canvas underneath it. The routing is therefore explicit: pick the target by coordinates, and pin `Fl::belowmouse()` to it on `FL_DND_ENTER` — FLTK then delivers the later `FL_DND_RELEASE` and the `FL_PASTE` carrying the URI list straight to that widget (`fl_selection_requestor` is belowmouse), and emits the `FL_DND_LEAVE` on the previous target for free when the pointer crosses between them. The strip is skipped while hidden (its rect is stale, and that row belongs to the canvas). **Consequence, recorded deliberately:** with a single document open there is no tab strip, hence no drop-to-open-a-document target — a canvas drop places a magic layer, and File→Open is the way to a second tab. That follows from the user's own "no tab for a single document" rule.
- [x] S51-a — **Settings UI (pulled forward, user 2026-06-14) — DONE (build order ①②③ + categories all
    landed 2026-06-14→16; the last blocked row, Annoyances' unsaved-title toggle, cleared 2026-07-08
    when the real `.mosaic` Save gave `markSaved()` a caller — see S18-b/S18-d).** The settings surface + coherent IA;
    landed the accumulated toggles (S15-e, S16-q, S16-p) + theme mode picker (`394f61d`, with runtime
    re-theming) + units, plus the Tools sub-tabs, Color Management, and the Inpainting category
    (`fdba4b7`). The **Annoyances** category itself SHIPPED 2026-06-30 (`2d35ff0`, with the
    "Cheesy motivational one-liners" toggle); its **S18-d** unsaved-title row went live 2026-07-08 with
    the `.mosaic` Save. See §2 + §9 S51. *Build settings as we go, not endlessly backlog.*
    **Design settled (2026-06-14):** left category rail + right content pane; instant-apply + modeless
    (load-modify-write so the picker's sticky `pickerSurface`/recents are never clobbered); IA =
    General / Appearance / Tools / Color Management / Annoyances; diagram cards for Theme/Crop/Multi-
    select; build order ① surface → ② cards → ③ theme + re-theming.
  - **① surface — DONE (2026-06-14):** `ui::SettingsDialog` (`src/ui/settings_dialog.{hpp,cpp}`) — the
    rail+pane shell, `NavItem` rows, instant-apply plumbing via a `SettingsHost` (std::function setters
    → `MainWindow::persistSetting` load-modify-write + live apply), Done/Esc, opened from **Edit →
    Settings… (Ctrl+,)** as a lazily-built, reused modeless window. Wired the two already-backed
    settings: **General → Units** (live: updates `m_metric`, the continuous frame loop re-reads it) and
    **Color Management → CMYK profile** (Browse…/Use-default; re-applies via `applyProfileSettings`).
    Headless-verified (build + ctest 100% + `--gui-frames` Vulkan-clean); **user's visual pass PASSED
    2026-06-15.**
  - **② option-cards + ③ theme picker + runtime re-theming — DONE (2026-06-14→16):** `ui::OptionCard`
    diagram cards (Theme / Crop-framing / Multi-select), the Appearance theme picker with always-on
    live re-theming (`394f61d`; per-component `reapplyTheme()`, follows the OS while System), and the
    Tools category (horizontal `SubTabBar` sub-panes — Crop S16-p/-q, Move S15-e, Lasso) + Color
    Management + Inpainting (`fdba4b7`). **Annoyances** landed 2026-06-30 (`2d35ff0`, "Cheesy
    motivational one-liners"); its S18-d unsaved-title toggle came live 2026-07-08 with the `.mosaic` Save.
- [x] S51-b — Keybindings (remappable shortcuts) — **BUILT 2026-07-30 (`9dbf82a`; `docs/keybindings.md`).**
    ⚠ **The defaults are the shortcuts the application already had** (user ruling 2026-07-29, correcting
    this line's and §9 S51's earlier "Photoshop-like defaults"): they were harvested VERBATIM from
    `buildMenu`'s inline accelerators, `menu_bar`'s text-editor fence and `tool.cpp`'s `kToolDefs`
    letters (plus bare X/D for the colours). Nothing was moved to match another product and nothing was
    invented for a command that had none — `tests/test_keymap.cpp` pins all 38 menu/colour rows against
    the literal each was harvested from, so drift fails loudly instead of quietly moving a key someone
    already uses. The tool letters **seed from `kToolDefs`**, which is why S35-b's two warp tools (below)
    arrived remappable without anyone touching the table. New `ui::Keymap` = an FLTK-free model plus a
    three-function bridge (`FL_COMMAND` is a function call, so the platform question is asked in exactly
    one place); overrides persist **sparsely** in `Settings::keymap`, so changing a default still reaches
    anyone who never rebound it. `buildMenu` reads every accelerator from the keymap, and a remap
    re-points items by callback then re-mirrors through `MenuBar::update()` (the macOS system menu bar),
    with no restart. Conflict detection covers the FLTK-specific case that a plain-letter menu
    accelerator would swallow a tool key. Settings ▸ Keybindings (rail index 8) is searchable +
    instant-apply. **Scope limit, by design:** canvas-local keys — space-pan, R-rotate, arrow-nudge, the
    Type editor — are NOT remappable; they are gesture-coupled and `docs/wayland.md` records why that
    needs its own pass. ⭐ *Interactive pass owed, and the key-capture focus park is the first thing to
    try (`docs/keybindings.md`).*
- [x] S52 — Icon system finalization + credits — **BUILT 2026-07-10** (`3f648b1` the system + first
    default set, `10e41fb` raster packs + the scrolling description, `0a82f78` pack identity,
    `46cb731` the current default). The §3.13 identity landed as a **pack system**, which the entry did
    not anticipate: a pack is a folder with `mosaic_icon_pack.json` (identity + credits; the file *is*
    the pack marker) plus one SVG — or PNG — per stable tool key, user packs under
    `dataDir()/icon_packs/`, per-icon fallback to the embedded default so a one-icon pack is
    legitimate. **Packs are TOOLS ONLY** — the §3.13 scope note holds, chrome and dialog icons stay
    out of reach (they are the S16-g one-ink set). The shipped default is **"Smalti"** (39 icons,
    GIMP's colour tool icons CC-BY-SA-4.0 with Mosaic additions and reworks; it *replaced* the
    originally-shipped bespoke set "Tesserae", which `3f648b1` had landed the same day). `docs/credits.md`
    carries per-icon provenance and the licence aggregation; `docs/icons-needed.md` is the running
    inventory, not a promise. ⚠ **`EmbedAssets` stale-mtime trap** — preserved-mtime copies shipped
    stale bytes until `699068c`; verify an icon edit through nanosvg, not by eye. *Visual pass owed.*
- [x] S53 — Image ops + menus — **DONE 2026-07-28.** [x] **S53-a** (image & canvas operations + resampling quality) — one generalised engine, `render::buildDocumentRemapCommand`, with `buildCropCommand` reduced to a wrapper over it; Canvas Size (9-point anchor), Image Size, lossless Rotate 90/180 + Flip H/V, arbitrary Rotate, Trim to Content; the compositor's resampler extracted **verbatim** to the public `render/resample.*`; `core::SetGuidesCommand` + `Selection::{remapped,scaled}` so guides and the selection finally ride the canvas (guides were stranded by *every* crop until now — a plain pre-existing bug). `docs/image-operations.md`. ⚠ Known limit kept and documented: `convolveInto`'s footprint cap at 8 source texels aliases on an extreme minification (mip-style pre-downsampling is the follow-up). · [x] **S53-b** (menu completion + the Image-ops corner panel with a live on-canvas preview) — in `src/ui/app_window.cpp` + `src/ui/menu_bar.*`; also where `Layer ▸ Combine Paths` exposes the S28 boolean kernel. ⚠ It adds many msgids: `po/mosaic.pot` needs re-extracting.
- [x] S54 — i18n completion — landed 2026-07-24: 74 catalogs at core coverage (633/1020 msgids), `po/<lang>/` layout, `$MOSAIC_LANG` runtime switch, `tools/i18n/` pipeline; machine-assisted drafts awaiting native-speaker review
- [x] S55 — Welcome/recent/autosave/crash recovery — landed 2026-07-22 as the **New-Document dialog redesign** (recents + templates + from-clipboard; autosave/crash-recovery had landed with S48); template .mosaic files themselves still to be designed
- [ ] S56 — Export pipeline + presets — **the pipeline and the dialog shipped as export milestones M1/M3
    (`4b692c5`, `0d98ca6`, 2026-07-04 → 07-28); what is left of this entry is batch export.** ✅ The
    **Export As modal** no longer knows a single format name: the format list is
    `FormatRegistry::exportOrder()`, the options panel is **generated** from each backend's
    `optionsSchema()`, the encode goes through `FormatBackend::encode()`, the live loss banner is
    `diff(profile, caps, values)` coloured off `worstSeverity`, and the Matte row shows itself off
    `caps().alpha == None` rather than off "is this JPEG". Plus **Quick Export**, the XDG-portal file
    dialog, a **preset row** (fixed built-in intents — index 0 is always "Custom", so the row never
    claims a preset the settings no longer match), the resize-quality dropdown, the path policy, and an
    async pipeline whose key names format + options + size + filter + matte, so a quality-slider drag
    re-runs only the encode stage and the *same bytes* feed both the exact file-size readout and the
    decoded preview (a JPEG preview shows its own artefacts). ❌ **Not built: batch export** (and
    user-savable presets, if they are wanted — decide). ⚠ `render::resizeImage` is still owed
    (`docs/export-system-plan.md` §5 item 1). *Visual pass owed.*
- [x] **S56-b — CI on GitHub Actions — DONE 2026-08-24.** `.github/workflows/ci.yml`: an **Arch Linux container** builds `linux-debug` and runs `ctest` under `xvfb-run` with `vulkan-swrast` present, plus a **clang-format** gate and an advisory **clang-tidy** step. Arch rather than an LTS base is the point — it is the toolchain the project is developed on, so the mandatory `-Werror` wall is exercised against the *newest* GCC. ⚠ Two deliberate departures from the sketch above: the lint gates are scoped to a commit's own **changed lines** (via `git-clang-format`), not the tree — the tree predates any formatting gate and carries ~48k lines of drift, *inconsistently* (some files indent case labels, more do not), so a tree-wide gate could only be made green by a reformat commit that would bury real history. And clang-tidy is **advisory** (`continue-on-error`), because `.clang-tidy` sets `WarningsAsErrors: ''` and the checks have never been enforced, so a blocking gate would fail on pre-existing code rather than on the change under review. Tightening either one is a standalone decision, not this session's.

**Phase 9**
- [x] S48-b — .mosaic previews + Linux desktop integration (BUILT 2026-07-22)
- [x] S57 — Windows cross-compile (MinGW-w64) — **BUILT 2026-07-30** for **x86_64 (system mingw GCC)** and **aarch64 (llvm-mingw)**, both compile-/link-clean, plus a portable zip and a per-user MSI (`msitools`' `wixl`). 24 third-party libraries cross-built as DLLs per arch; Mosaic's own modules static. The `.mosaic` **write path is now implemented** (the one genuinely missing feature, S48 audit) along with a Win32 surface, registry appearance detection, `IFileDialog`, **WinTab + Windows Ink**, `ISpellChecker`, bundled hyphenation dictionaries, an Explorer `IThumbnailProvider` handler, and UTF-8↔UTF-16 path handling. `mosaic.exe` verified under Wine: the GPU-compute composite is **byte-identical to Linux** with 0 validation errors. ⚠ Fractional HiDPI, the frame-loop-blocking picker, and OpenEXR/LibRaw/libzip remain v1 gaps; runtime is the user's check. See §S57.
- [x] S58 — macOS cross-compile (osxcross + MoltenVK) — **BUILT 2026-07-23 (`e42dcde`), then eight
    follow-up slices 07-23/24; `docs/build-macos.md` + `packaging/macos/README.md` are the surface.** No
    longer "investigation (backlog)": Mosaic cross-compiles from Linux to a **universal (arm64 +
    x86_64)** `Mosaic.app`, Vulkan through a bundled universal MoltenVK ICD, surface via
    `VK_EXT_metal_surface` on a `CAMetalLayer`, the whole third-party stack cross-built statically,
    deployment floor macOS 13.3. Follow-ups, each in §9: **-a** libintl (the catalogs were bundled but
    unread), **-b** the native system menu bar with template-NSImage badges, **-c** the HiDPI overlay-width
    rule in `canvas_present.comp`, **-d** cross-built libjxl, **-e** Quick Look **thumbnail + space-bar
    preview** extensions, **-f** the first real M1 test's feedback, **-g** a File▸Open SIGSEGV fixed by
    patching FLTK 1.4.5 (which is what created `packaging/macos/patches/`), **-h/-i** the speech-bubble
    gate and its reversal — the popover shape bug was *ours*, on every non-X11 platform, and its fix is
    the `MOSAIC_CHROME_BOX` pattern. Also here: dark-mode/accent detection + live theme following, native
    hyphenation via CoreFoundation, and the Cocoa/NSEvent tablet backend. ⚠ **Runtime is the user's
    Mac-side check** (no Mac here); tablet pressure stays untested by choice; `MOSAIC_MAC_WORK` must be
    passed to rebuild a dep.
- [ ] S59 — Packaging & release engineering — [x] **the macOS `.dmg` slice DONE 2026-07-23** (with S58, `e42dcde`…`97f0014`): `packaging/macos/make-dmg.sh` builds an unsigned universal `Mosaic.app` and a drag-to-Applications `.dmg` entirely on Linux with no Mac and no root — HFS+ via `newfs_hfs`/`hfsplus`/`dmg`, a dark dithered background with the drag arrow, and a real Finder `.DS_Store` window/icon layout authored through an alias template. No signing or notarization, by user decision. — [~] **S59-a — HALF CLOSED 2026-07-28.** ✅ The **native-Wayland default** landed (`platform::preferWaylandBackendIfUnset`, `FLTK_BACKEND=x11` the escape hatch), together with the two Wayland cursor defects behind the user's "offset chrome" report — the device-vs-logical hotspot double-scale (`ui::CursorImage` now carries the logical box + hotspot) and the missing `fd_double_arrow`/`bd_double_arrow` in `breeze_cursors` (new `ui::nwseCursor`/`neswCursor`, substituted on the Wayland backend only) — plus the cursor-cache drop on a content-scale change and the `FL_ENTER` cursor-state reset that fixes the §12 stuck-rotate-cursor item. The **app_id** pin — `Fl_Window::default_xclass("mosaic")` next to the backend pin, before the first window exists, so `xclass` → app_id → `.desktop` → `Icon=` resolves deterministically (`docs/wayland.md` §3) — is the other half of the taskbar-icon item; `data/desktop/mosaic.desktop` and the scalable hicolor icons are already installed by CMake, so the chain completes as soon as the app is **installed**. Running from the build tree still shows a generic icon, expectedly. ⚠ **That "as soon as it is installed" caveat was the v0.3.1 AppImage bug** (user report, 2026-08-26): an AppImage is *never* installed, so it had no Wayland icon at all while looking correct on X11 — X11 carries icon pixels on the window (`_NET_WM_ICON`), Wayland has no equivalent and FLTK's Wayland driver implements no `icons()` override, making the `Fl_Window::icon()` the app already sets a silent no-op there. Fixed by speaking `xdg-toplevel-icon-v1` directly (KWin yes, Mutter not yet — GNOME/mutter#4100), plus `packaging/linux/desktop-entry/` for the launcher entry, MIME and grouping that no protocol can supply. ✅ **`xdg_dialog_v1` DONE 2026-08-26**, together with **`xdg-toplevel-icon-v1`** — both were blocked on the same missing thing, the window's `xdg_toplevel`, and both were unblocked by the same ten-line patch: `packaging/linux/patches/fltk-1.4.5-wayland-toplevel-accessor.patch` exposes what `Fl_Wayland_Window_Driver::xdg_toplevel()` **already computes** internally but keeps in a private header. Applied only by the release AppImage job, which has built FLTK from source since day one — so §2.5's "we cannot patch FLTK on Linux" constraint is untouched, and the accessor is reached through a **weak symbol** so a stock distro FLTK still builds, links and simply reports the feature absent (CI, on Arch's unpatched `fltk`, still compiles every line under `-Werror`). Reference: `docs/wayland.md` §3.1 and §4.1. — [x] **the Linux AppImage slice DONE 2026-08-24** (S59): `packaging/linux/make-appimage.sh` installs into an AppDir, bundles the `ldd` closure minus the set that must come from the host, writes an `AppRun` and packs with `appimagetool`, for **x86_64 and aarch64**. Three exclusions carry the reasoning: **glibc + the loader** (an AppImage runs under the host's `ld.so`, which is precisely why the BUILD HOST sets the compatibility floor — CI builds on Ubuntu 24.04, glibc 2.39, because an image built on a rolling distro runs only on rolling distros), the **GPU stack**, and **libstdc++/libgcc** — Mesa's radv and lavapipe link libLLVM against the *host's* libstdc++, and putting ours ahead of it on `LD_LIBRARY_PATH` is a known way to make the GPU disappear. `AppRun` must export `MOSAIC_DATA_DIR` and `MOSAIC_LOCALEDIR`: `installedDataDir()`/`resolveLocaleDir()` otherwise return the compile-time `/usr` paths, which inside a mounted image name the HOST's `/usr`, and the brush bundle, the CMYK profile and all 74 catalogs would silently be missing. Remaining S59: **Flatpak**, PNG raster icon sizes, AppStream `metainfo.xml`, a reverse-DNS app id. — [x] **the Windows slice DONE 2026-07-30** (with S57): `packaging/windows/make-package.sh` builds a portable zip and a **per-user MSI** (via `msitools`' `wixl`, not NSIS) for both arches from one staging tree, with the DLL payload derived as a transitive PE-import closure rather than hardcoded, `.mosaic` file association + Explorer thumbnail-handler registration, a multi-resolution `.ico` rendered per size from the SVG, and a `VERSIONINFO`/PerMonitorV2/UTF-8 manifest. Unsigned by decision (Windows needs no signature to run; SmartScreen warns once).
- [ ] S60 — Performance & hardening — ⚠ **the -a…-d split this line used to enumerate is SUPERSEDED by
  `docs/s60-performance-plan.md` §7** (scoped in full 2026-07-23), which is the ledger of record; the
  sub-items below are its split, and each carries an item-level status there rather than here.
  - [x] **S60-α — foundations (COMPLETE 2026-07-23, all six items).** `render::GpuCaps` (`066a7be`) —
    probe / pure `decide()` / tier flags / lane admission / `MOSAIC_GPU_PROFILE=floor`; the **Vulkan 1.0
    floor** (`6082e3e` — all 12 shaders now emit SPIR-V 1.0, verified by header word; they had been 1.5
    and therefore unloadable on a 1.0 driver); **one shared device** (`7bbdb0f` — five VkDevices became
    two, `WindowRenderer` deliberately keeping its own because it must be able to *present*); caps gates
    on the existing lanes (`f88f0a5` — every lane's own guard would have sailed past the guaranteed
    minima); `VK_KHR_portability_subset`; and **release-build profiling** behind `--profile` /
    `MOSAIC_PROFILE=1`. Landed alongside because they were genuinely disjoint: the process-wide
    **thread pool** (replacing spawn-per-call at 25+ sites, band arithmetic copied verbatim so every
    golden stayed byte-exact; measured −4 % to −23 %), the **`--bench` harness**, and the
    **readback-consumer audit** (`docs/s60-readback-consumers.md` — 19 consumers, not the five the plan
    guessed, and it corrected the proposed API to `pinMirror()`/`peek()`).
  - [~] **S60-a — the resident tiled compositor (items 7–13 BUILT; the flip is NOT made).** `TileGrid`/
    `TileSet` as the shared 64 px vocabulary (`140238f`), the GPU tile atlas + `TileResidency` LRU with
    pinning and honest refusal (`2c5d9d2`), the **fused per-layer kernel** `composite_tile.comp`
    (`7f5ce63`, source-window support `b877fe1`) doing transform + resample + mask fold + clip + blend in
    one dispatch, item 10's dispatch collapse (`d77df3b` — and its measurement **killed the indexed tier
    as a default**: equal speed at strictly greater risk is not a default), the resident accumulator →
    present texture so the per-frame readback is dead (`9f0aa04`), the explicit `CompositeReadback` seam,
    incremental dirty-macrotile upload (`9584276`), and item 13's app-side `ui::ResidentComposite`
    (`c17777d`) fed from the **edit** seams because a doc-space bbox inverse-maps to a *different* region
    under rotation. ⭐ **The five-condition gate in `docs/s60-performance-plan.md` §7 PASSES** (re-run
    `3627db1`): condition 3 went from 4.8× over budget to 56% of it, and `gpu full` beats `cpu full` by
    **87×** at 1920×1080 / **105×** at 5000×8000. ⚠ **The default is still `Backend::Cpu` at every app
    call site** — the lane exists only behind `MOSAIC_TILE_COMPOSITOR=1`. The flip is **one line** and is
    blocked on nothing but an **interactive** pass: `residentRecompositeNow` → present has never
    executed, because reaching it needs a real edit and `--gui-frames` cannot make one. Two lanes now
    serve leaves beyond raster (adjustment `27f9534`, text/texture/magic `0f91e9e`, a live Move drag
    `32dcbcb`; `9b47fcb` records which kinds). ⚠ The **gesture-END stall (G3)** — the larger of the two
    — is untouched; the start stall is bounded (`f1e5190`). Also here, sequenced before residency moved
    the walk: **coverage partitions** (`3137208`, 2026-07-24 — the `over`-impossibility proof and the
    alpha rewrite that makes a feathered cut leave no seam; `docs/compositor.md`, 12 tests, user
    interactive pass owed).
  - [~] **S60-b — CPU-only compute mode + CPU-lane hardening.** `MOSAIC_CPU_ONLY=1` and a suite that runs
    green in it (`8b59a1b`) — and the assertion count **drops**, which is the proof the mode refuses
    rather than silently doing nothing. The thread pool landed early, under α.
  - [~] **S60-c — present-paced loop + off-thread composite.** Frame pacing on the **display** rather
    than a 60 Hz clock (`655b5d6`), input driving the frame rate with focus deciding only how cheap idle
    gets (`d0550ce`), background culling and the readout no longer touching the device (`70c461c`), and
    "no readback while the composite is moving" as a rule enforced by the class (`243524e`). ❌ The
    composite itself is still on the UI thread.
  - [ ] **S60-d — huge-document scalability** (tiled pixel storage, proxy/low-res preview, eviction).
    The GPU memory budget landed early (`89a5d0f`, in bytes not buffers `e4e776b`); ⚠ the user-reported
    **full-canvas 5k×8k Move-drag** lever is what this session owes.
  - [~] **S60-e — filling in the missing GPU lanes.** Channel isolation moved into the present pass
    (`4032ec9`); an eyedropper that samples all layers without re-compositing per frame (`3ee46a6`).
    ⚠ **Two lanes land but are NOT installed** (`e978a2d`, layer effects + the Channels histogram) —
    per-kind admission, parity-tested, one line each to wire, wanting an interactive pass; the
    histogram's real win needs the resident accumulator to bin from.
  - [~] **S60-f — hardening & fallback.** `--device` / `MOSAIC_DEVICE` adapter selection with every
    enumerated device logged and an unmatched selector warning and falling back, plus one shared
    `pickPhysicalDevice` for the presenting context (`0ba7e0d`); presenting-device texture admission,
    budgeted in bytes and profiled, so a floor device refuses **before** paying the CPU below-composite.
    ❌ Still owed: the lavapipe software-rendering fallback path with its warning, accessibility, and
    docs finalization. ⚠ **Bench discipline is part of this item now:** never measure on a box loaded by
    your own agents — GPU rows are far more load-sensitive than CPU rows, and a loaded run reported a
    scalability defect that did not exist.

---

## 11. Recommended Additional Features (suggested, beyond the brief)

In-scope additions already folded into the roadmap or strongly recommended:
- **Rulers, guides, grid, snapping, smart guides; alignment & distribution.**
- **Navigator, Histogram, and Info (pixel readout) panels** as right-dock tabs.
- **Status bar** (bottom strip): document size, cursor position + colour under the cursor on the
  canvas, zoom/rotation, colour-space + HDR indicators, etc. — **scheduled: S13-b** (spec in §9).
- **Swatches/palette panel**, recent colors, color-harmony helper.
- **Clone stamp tool** (sibling of Heal); **Smudge** (S23).
- **Free transform / perspective / warp**; Image menu (image size, canvas size,
  rotate/flip, trim — **scheduled: S53-a**).
- **Selection refinement:** feather, grow/shrink, smooth, refine-edge.
- **Autosave + crash recovery** and **session/tab restore** (S55).
- **Welcome/start screen** with templates + recent (S55).
- **Command palette** (searchable actions) and a full **shortcut cheat-sheet**.
- **Soft-proofing + gamut warning**, ICC assign/convert, color-blindness simulation.
- **Metadata/EXIF/XMP viewer-editor**.
- **Proxy/low-res preview** for very large images; **tiling + GPU memory budget** (S60).
- **Tablet/stylus pressure & tilt** (**scheduled: S19-b**), touch/gesture pan-zoom-rotate.
- **Accessibility:** keyboard navigation, high-contrast theme, labels for assistive tech.
- **Scripting/automation API** (Lua via sol2) over the command system — power users + tests.
  **Scheduled: S40** (pulled in as the replacement for the dropped built-in ML inpainter; it also
  carries the user-pluggable inpainting backend + an example hook script — §3.11).
- **Plugin architecture** for filters and format modules (keeps the codebase modular and
  extensible, reinforcing the maintainability goal).

---

## 12. Backlog / Eventualities

- ⚠ **Windows/Wine UI defects — the Vulkan canvas has no child window of its own (open, S59).**
  Two user-reported symptoms with one likely root cause. **Submenus render BEHIND the Vulkan
  canvas** (on real Windows *and* Wine), and **FLTK chrome flickers** (Wine only; native Windows
  does not show it). The asymmetry across platforms is the evidence: Wayland gives the canvas a
  dedicated `wl_subsurface` (`platform/wayland_subsurface.cpp`), macOS a dedicated NSView subview
  carrying the `CAMetalLayer` (`native_window_macos.mm`), and X11 gets away with the main window
  because FLTK's menus there are override-redirect TOP-LEVEL X windows that always float above.
  Windows alone presents into **FLTK's own HWND** — `native_window_win32.cpp` says so in its
  header: "the HWND FLTK already made IS presentable … it never touches the window". A swapchain
  that owns an HWND's client area is composited by DWM as one layer, so GDI chrome and popups
  cannot reliably appear over it, and the chrome repaint races the present — a race DWM hides and
  Wine does not. *Likely fix:* mirror the other two platforms and create a dedicated child HWND
  (`WS_CHILD | WS_CLIPSIBLINGS`) for the surface. **Unverified — there is no Windows machine in
  this loop.**
  ⚠ Note for the record: the `VK_SUBOPTIMAL_KHR` swapchain-recreation fix in S59 was committed
  claiming to address this flicker. It does NOT — the user confirmed the symptoms are FLTK
  compositing, not swapchain churn. That change stands on its own merits (it strictly reduces
  swapchain teardown) but is not a fix for anything reported here.

- ~~**macOS port** (osxcross + MoltenVK): blocked mainly on build tooling + testing access~~ — **BUILT
  2026-07-23/24 and promoted out of the backlog into S58** (universal `.app` + `.dmg`, native menu bar,
  Quick Look, dark mode, tablet, JXL, three rounds of real-M1 feedback). The "testing access" half of the
  blocker never cleared and is now a *standing* condition rather than a blocker: there is no Mac here, so
  every macOS slice ships headless-verified and the user runs the DMG.
  (S58). Treat as eventuality per brief.
- **Native Wayland Vulkan canvas** — ✅ **DONE** (backlog item cleared, post-S7). The swapchain
  used to abort on FLTK's own `wl_surface` (`wp_linux_drm_syncobj_surface_v1`); the canvas now
  presents to a dedicated **`wl_subsurface`** (`platform::WaylandSubsurface`, bound via a private
  registry roundtrip and stacked over the FLTK surface), so `FLTK_BACKEND=wayland` runs
  validation-clean and displays correctly (verified on RX 6600 XT / RADV). ~~**X11/XWayland stays
  the default** for now; the default is expected to flip when S43-c lands.~~ **UPDATED 2026-07-28
  (S59-a): native Wayland IS the default** — `platform::preferWaylandBackendIfUnset()` pins
  `FLTK_BACKEND=wayland` on a Wayland session with no user choice; `FLTK_BACKEND=x11` is the
  escape hatch and a pure-Xorg session is left alone. It did not wait for S43-c: the file-picker
  parenting, the resize black flash and the cursor work each argued for it on their own, and HDR
  output (XWayland/Xorg are SDR-only — §3.6, §9) is now simply unblocked rather than the trigger.
  Details in `docs/vulkan.md`; everything the flip changes *around* the canvas is in
  **`docs/wayland.md`**.
- **X11 resize "black flash" — now ESCAPE-HATCH-ONLY (was: open bug, X11-only).** Unchanged as a
  bug, but no longer on the path anyone takes by default: since S59-a a Wayland session runs the
  **native** backend, where this does not happen, so only a pure-Xorg session or a
  `FLTK_BACKEND=x11` user can still see it. Its removal from the default path was one of the
  arguments *for* the flip. The diagnosis below stands if it is ever picked up again.
  When the window is resized on **X11** — both
  pure **Xorg** (tested on i3) and **XWayland** (tested on KDE) — the FLTK-drawn chrome
  (menu/toolbar/options bar/dock) flashes **black** for a frame or two; the Vulkan canvas is immune
  (separate, continuously-presenting surface). **Native Wayland is clean** (its buffer path doesn't
  show it). Tried + reverted (S11-c, didn't beat it): an X background pixel (`XSetWindowBackground`),
  a deferred per-frame chrome redraw, and a *synchronous* `Fl::flush()` in `MainWindow::resize`. The
  cause is almost certainly the **`Fl_Double_Window` back-buffer being reallocated during the X11
  resize** (and/or the child-window interaction) showing through before FLTK recommits — an
  FLTK/X11-level issue, not something a normal app-side redraw fixes. Next ideas to try when revisited:
  a single-buffered top-level (or a dedicated resize buffer), an `Fl::add_idle` repaint, a newer FLTK,
  or upstreaming. **Lower priority than ever:** purely cosmetic, transient, X11-only, and now off the
  default path entirely (native Wayland — the default since S59-a — is unaffected).
- **Status bar** — **scheduled (2026-06): S13-b**; the full spec moved to §9. (Permanent home of
  the colour-space indicator; the HDR indicator/warning joins it at S43-c.)
- **New-document dialog enhancements (S9 follow-ups) — REDESIGN LANDED 2026-07-22** (the dialog
  absorbed S55's welcome-screen role; user ruling: templates are plain `.mosaic` files under
  `data/presets/`, numbered `1-Birthday.mosaic`-style, never embedded in the binary, the installed
  path arriving with S59):
  - ~~**Stock comboboxes**~~ **DONE:** every `Fl_Choice` swapped for the themed `ui::Dropdown`
    (the dialog hosts its own `DropdownPopup` + `ContextMenu` children, created before show). The
    promotion also minted the **shared `ui::FilledButton`** (widgets.hpp; the five file-local
    copies collapsed — tool_options' arbitrary-fill variant renamed `TintedButton`) and the
    **shared `ui::GalleryCard`** (thumbnail/preview-fn + title + subtitle + hover + accent ring +
    double-click activate; the settings `OptionCard` can migrate onto it later).
  - ~~**Per-layer memory estimate**~~ **DONE:** the Document Summary shows resolved px + "≈ N MB
    per layer" (`bytesPerPixel`/`layerMemoryBytes`/`formatByteSize`, headless-tested).
  - ~~**Scrollable preset gallery**~~ **DONE:** category rail (Recent / Print / Screen /
    Templates) over a card grid; size presets draw the paper-proportioned placeholder, templates
    and recents render real previews (256px PNG cache + meta sidecar under
    `stateDir()/thumbnails`, refreshed on every open/save, mtime-invalidated for templates;
    superseded for `.mosaic` by S48-b's `PRVW` when that lands). Plus **Open Recent** in the File
    menu (ring buffer ≤10, `Settings::recentFiles`, Clear Recents) and **New from Clipboard**
    (async FL_PASTE sizing, `m_clipboardNewPending`).
  - **Feedback round (user, 2026-07-22 — same day):** New-from-Clipboard became a **card** leading
    the Recent gallery (real preview + exact px, via a bounded-pump synchronous pre-fetch;
    pre-selected on landing when present) instead of a bottom-bar button — which surfaced that
    **external image paste was broken app-wide**: FLTK's clipboard decodes an external image/png
    offer through `Fl_Shared_Image::get()`, which knows no formats until a handler registers
    (in-app copies round-trip as image/bmp, decoded by FLTK core — why only browser copies
    failed). Fixed by registering **our own handler backed by `io::loadImage`** (probe-verified
    on X11 + Wayland; d=4, alpha kept) — deliberately NOT `fltk::images`, whose bundled nanosvg
    collides at link with our vendored copy. Plus the design pass: equal-AREA preset sheets
    (ratios read as siblings), sheets **filled with the live Background choice**
    (white/black/checker), label font fit-to-sheet (US Tabloid overflowed), a **Width↔Height
    ratio-link chain toggle**, and faint accent **unit tags inside the value fields** (px/mm/…,
    ppi) replacing the external unit labels.
  - **Round 3 (user, 2026-07-22):** a **Portrait|Landscape orientation switch** (swaps W/H,
    preserves preset selection), sheet art re-sized **uniform-height per orientation** (equal-area
    made the Square read smaller than its 16:9 neighbours), recents cards show their
    **abbreviated location** (`~/…` parent dir; dims moved to the summary panel), and the **Name
    field deactivates for a recent selection** (opening keeps the file's own title).
  - **Round 4 (user, 2026-07-22):** disabled visual states for the orientation/link controls; the
    orientation pick now **survives preset clicks and turns the sheet art**; sheet sizing settled
    FINAL as **uniform per orientation class** (ratio-proportional depiction abandoned after three
    failed schemes — exact dims live in the subtitle; verified by offscreen self-render); the
    greyed form **seeds from the source document's real values** (thumbnail sidecar v2 carries
    dpi/colour/depth; clipboard defaults to 72 ppi); value fields evaluate **simple arithmetic**
    ("1024*2", + − × ÷ + parens — flagged follow-up: promote into the shared `parseFieldNumber`
    so every NumberField gets it); the unit-tag no-erase boldening fixed; and a **Texture**
    category (power-of-two squares 128–8192, `DocumentPreset.category` now explicit).
  - ~~**"Custom…" colour space entry (user request, 2026-06)**~~ **DONE 2026-07-22 (round 5):**
    the Color dropdown's trailing "Custom…" row opens the native chooser, validates by actually
    loading the profile (a non-RGB pick never sticks), shows the embedded description via
    `Dropdown::setOverrideText`, and re-offers last time's profile if it still loads. The
    document model stores enum-AND-path (`Document::iccProfilePath/Name`; the enum stays the
    fallback wherever the file is unavailable), `.mosaic` carries grown `color.icc_path/icc_name`
    manifest fields, and the picker's engine serves the document profile
    (`ColorPicker::setWorkingSpace(cs, iccPath)`; the S12-c settings-level override still wins).
    Embedding the profile BYTES in the file (fully portable documents) is the noted upgrade path.
  - **Remaining Affinity-study bits:** Layout/Color/Margins sub-tabs (not needed at the current
    knob count) and a **"Show on startup"** toggle (needs a startup-dialog notion first); the
    category sidebar + Document Summary landed above. Actual template documents are still to be
    designed by hand (user, when time allows).
- **Detachable / resizable docks (tear-off panels).** The right dock's tabbed Layers/Properties
  (S10/S11) should be **resizable** (drag the dock↔canvas splitter to rebalance width) and
  **detachable** — tear a tab off into its own floating, re-dockable window, and re-arrange/stack
  tabs. FLTK has **no built-in docking framework**, so this is **session-sized**, not a quick
  fix: it needs a small dock manager (draggable splitters; tabs as movable items; floating
  `Fl_Double_Window` hosts that can re-parent a panel; layout persisted in settings). Schedule it
  in **Phase 8 (UX polish)** alongside the workspace/panel work (≈ S51–S55); until then the dock
  stays a fixed-width right strip. (A *contextual* Properties dock — selected-object/adjustment
  state, not a tool-option mirror — is a later addition; the generic mirror once planned for S11 was
  **cut**, see §9 S11.)
- **Menu-bar redesign — DONE (2026-06-14, `5108fd4` + `d201b05`).** The top `Fl_Menu_Bar`'s stock
  Motif pop-ups (override-redirect, painted over other apps on focus loss) were replaced by **themed
  `MenuBar`/`MenuPopup`** sub-windows (the same child-sub-window pattern as `ui::Dropdown` — Wayland-
  clean, no taskbar, dismiss on outside-click), keeping `Fl_Menu_Bar`'s machinery (title layout/draw,
  hit-testing, accelerators, mnemonics). Renders flat items + `FL_MENU_DIVIDER` separators +
  right-aligned shortcut text + nested submenus; `d201b05` added native-Wayland hover-switch, the
  Alt+R Filter mnemonic, and keyboard-mode underlines. (The last stock pulldown — the new-document
  dialog's bare `Fl_Choice`s — fell with that dialog's 2026-07-22 redesign: **no stock Motif pop-up
  remains anywhere in the app**.)
- **`xdg_dialog_v1` for native-Wayland modal — DEFERRED 2026-06-15 (blocked on FLTK internals).** The
  modal Settings dialog is enforced at the FLTK event level on all backends (controls on the parent
  don't respond), but native Wayland doesn't **dim / prevent-raise** the parent the way XWayland's WM
  does (FLTK 1.4.5 lacks `xdg_dialog_v1`). Plan was: bind `xdg_wm_dialog_v1` iff advertised + `set_modal`
  on the dialog's toplevel. **Blocker found (2026-06-15):** `get_xdg_dialog()` needs the dialog's
  `xdg_toplevel`, but FLTK 1.4.5's public `fl_wl_*` exposes only `wl_display`/`wl_surface`/`compositor`/
  `xid` — **NOT** the toplevel (the original "reach via `fl_wl_*`" premise was wrong). The toplevel
  lives in FLTK's **private** `struct wld_window` (not installed on Arch); and since the dialog is
  decorated, the toplevel is owned by **libdecor** (`libdecor_frame_get_xdg_toplevel()`), whose `frame`
  ptr is also in that private struct. The only route is hand-declaring FLTK 1.4.5's private struct
  layout (fragile: version + build-config dependent offsets) — and it's untestable from the headless
  harness (native-Wayland verification is the user's). The protocol XML (`/usr/share/wayland-protocols/
  staging/xdg-dialog/xdg-dialog-v1.xml`) + `libdecor-0` are present, so only the toplevel access is
  missing. **Decision (user, 2026-06-15):** defer this cosmetic dim until FLTK exposes the toplevel
  (or we vendor its private header / upstream an accessor) — **scheduled under S59-a** / FLTK 1.5. A
  guarded private-struct hack (cross-check `wl_surface`==`fl_wl_surface(xid)` + `fl_win`==win, skip
  silently on mismatch) remains the fallback if it's ever wanted before then.
  **STILL BLOCKED after S59-a (2026-07-28), and re-verified there:** the backend flip made native
  Wayland the default but changed nothing about FLTK's public surface, so `FL/wayland.H` still
  exposes only display / xid / surface / compositor / buffer_scale. This is the half of S59-a that
  did **not** close. Cosmetic only — the dialog is genuinely modal already, it just does not dim its
  parent (`docs/wayland.md` §4).
- **Native-Wayland rotate cursor won't revert on canvas re-enter — ~~WON'T FIX~~ FIXED 2026-07-28
  (S59-a); awaiting the user's interactive check.** *Was* (user, 2026-06-17): on the native-Wayland
  backend, after the Move tool rotates a selection, moving the pointer out of the canvas and back
  leaves the **custom rotate cursor stuck** (it should revert to the hover/idle cursor); X11 /
  XWayland are fine. The stated justification for not fixing it was **"X11 is the default backend
  until S43"** — **that is no longer true**, S59-a made native Wayland the default, so the item had
  to be re-decided rather than inherited.
  - **The mechanism, once looked at, is not an FLTK cursor-serial bug at all.** On Wayland
    `seat->default_cursor` is **per-application**, not per-window: whatever any widget last set is
    what the pointer shows, and a drag-and-drop resets to it. Combined with `updateToolCursor`'s
    `if (want == m_cursorState) return;` early-out, the canvas believed it had already sent the
    cursor it wanted and sent nothing on re-entry. The canvas now **forgets its cursor state on
    `FL_ENTER`** (a `-2` sentinel) and re-sends unconditionally; on X11 that costs one redundant,
    idempotent `cursor()` call per canvas entry. `docs/wayland.md` §2.3.
  - Item 4 of `docs/wayland.md` §5 is the check: rotate a selection with the Move tool, leave the
    canvas, come back — the cursor must revert.
- **Move-tool rotate cursor orbits a point offset from the figure centre — PARKED, cosmetic (user,
  2026-06-18).** While *dragging* a Move-tool rotation, the rotate-cursor arrow reads as tangent to a
  point sitting a few px *beside* the figure's true rotation centre — an immovable offset point next to
  the centre. **Reproduces on both XWayland and native Wayland; not HiDPI-specific.** Two leading
  theories were chased and **both ruled out** (don't re-chase):
  1. *Cursor centre re-derived vs the gesture pivot.* `applyRotateCursor`'s drag branch points the arrow
     tangent to `c = average(corners)` (the on-screen box corners), while the gesture rotates about
     `TransformGesture::m_centerDoc`. These are **mathematically the same point** for a pure rotate:
     the result is `T(c)·R·T(-c)·base`, so the box centre is invariant and equals `m_centerDoc`, and the
     affine `CanvasView::toScreen` preserves centroids ⇒ `average(corners) == toScreen(m_centerDoc)`. A
     debug capture last session put **both at (439.9, 386.2)**. So "feed the cursor `m_centerDoc`
     directly" is a clean refactor (removes the two-sources-of-truth) but a **visual no-op** — it will
     not move the orbit centre. Not implemented; not worth a commit on its own.
  2. *Viewport-vs-window coordinate offset.* The canvas is a subwindow at `(kToolbarWidth, bodyTop)`
     (`app_window.cpp`), so two frames genuinely exist — but `Fl::event_x/y()` inside
     `VulkanCanvas::handle()` is **subwindow-local**, the same frame `toScreen` returns. Proof it can't be
     a frame offset: **handle hit-testing and rotate-band arming consume the identical two quantities**
     (`(Fl::event_x/y)` vs `toScreen(corners)` in `hitTransformControls`) and work correctly; a
     ~`kToolbarWidth` gap would mis-place every handle/band by the same amount, very visibly.
  What's left, and where a future pass should look: hit-testing (correct) and the cursor (offset) differ
  in exactly one way — the cursor is a **rendered bitmap pinned by a hotspot**, while the angle math draws
  from the same `m_gestureResult` as the box overlay (so math + box can't disagree). Checked the art:
  `left_side.svg`'s arrow bbox centres on ~(127.6, 129.1) vs the bake/hotspot point (128.5, 128.5) —
  essentially centred, so a glyph-offset cause is weak too. Remaining candidates are **cursor hotspot
  placement** (compositor honouring `(hotX, hotY)` = box centre) or that a straight tangent arrow simply
  implies no unique centre and is being read as offset. **Cosmetic at most; user is done chasing it.**
  - *Note (S59-a, 2026-07-28):* the device-vs-logical hotspot double-scale found this session **does**
    displace a centred hotspot on a HiDPI **Wayland** output, and is now fixed — but this item was
    reported as reproducing on **XWayland too**, where the whole conversion is an identity, so that
    cannot be the whole story. Worth one re-look on a fixed build before anyone theorises further.
- **Text-input internal margin — DONE 2026-06-15.** Text sat flush against the left/right edge of
  every input (CMYK field, new-doc fields, crop number inputs, colour readouts) because they shared
  `MOSAIC_PANEL_BOX` (dx=1). Added a dedicated **`MOSAIC_INPUT_BOX`** (theme.{hpp,cpp}) — same 1px
  frame (reuses `drawPanelBox`) but a wider horizontal interior inset (`set_boxtype(...,4,1,8,2)` →
  4px each side; FLTK insets input text by the box dx/dw). Routed the four input sites to it
  (tool_options crop `NumberInput`, color_picker numeric readouts, new_document `styleInput`, settings
  CMYK field). 4px clears the narrowest field (the 34px colour readouts) without clipping a 3-digit
  value; `MOSAIC_PANEL_BOX` left untouched so panels/popovers/label boxes don't shift. The picker hex
  field is `FL_FLAT_BOX` inside a composed row (unaffected). Headless-verified (build + ctest + smoke);
  **user's visual pass PASSED 2026-06-15.**
- **Themed text-field right-click menu + whole-value Ctrl+C — DONE 2026-06-15** (transient pass, fixes
  #1/#2). FLTK's stock Motif Cut/Copy/Paste/Select-All menu on text inputs is replaced by a flat themed
  **`ui::ContextMenu`** (`widgets.{hpp,cpp}`) — a child sub-window on the exact `DropdownPopup` rules
  (wl_subsurface, no taskbar entry, never paints over other apps; one per top-level, created before
  `show()`; global `g_contextMenus` registry + `contextMenuFor(top_window())` lookup + the
  `dismissActiveContextMenu{,OnOutsideClick}` helpers). Driven by a simple `ContextAction`
  {label, action, enabled, divider} list. Thin `TextField<Base, Editable>` subclasses
  (`TextInput`/`FloatInput`/`IntInput`/`TextOutput`) route through `handleTextFieldEvent`, which opens
  the menu on right-click (falls back to the stock menu if the field's top-level created no host) and
  makes **plain Ctrl+C copy the whole value when nothing is selected** (FLTK copies only the selection,
  so the read-only CMYK `Fl_Output` copied nothing — the user-reported #2; no-Shift guard so
  Ctrl+Shift+C "Copy Merged" still passes). Read-only fields get Copy + Select All only. **Wired into
  the two top-levels that already host themed pop-ups:** MainWindow (the crop options-bar number inputs)
  + the Settings dialog (CMYK output → `TextOutput`); `NumberInput` re-based onto `ui::FloatInput`.
  Build + ctest (100%) + `--gui-frames` Vulkan-clean; pot regenerated.
  **Follow-up pass (same day, user-reported):** *(a)* **Ctrl+C was still dead** on the CMYK output —
  root-caused with a synthetic-event harness to **`clear_visible_focus()`**: a field that can't take
  keyboard focus never receives `FL_KEYBOARD` (`take_focus()=0`, even a direct `Fl::focus()` doesn't
  stick), so the key never reached `handleTextFieldEvent`. Removed it (an `Fl_Output` shows focus only
  via selection highlight — no caret/ring — so no visual wart); confirmed the Ctrl+C branch then fires.
  *(b)* **Right-click-drag kept selecting text** under the open menu — the field stays the platform's
  "pushed" widget, so its base still got the drag; now `handleTextFieldEvent` swallows `FL_DRAG`/
  `FL_RELEASE` while `activeContextMenu()` is non-null. *(c)* **Colour-picker fields now included** (no
  longer deferred): the picker's `ui::Dropdown`s already prove the cross-sub-window pattern (their list
  is the *main* window's `DropdownPopup`, a sibling drawn over the picker, dismissed via
  `Popover::handle`'s top-level-coord forwarding), so the hex field + numeric readouts became
  `TextInput`/`IntInput` and resolve to the main window's `ContextMenu` via `top_window()`;
  `Popover::handle` now also forwards `dismissActiveContextMenuOnOutsideClick` + closes an open menu on
  Escape before itself. **Still DEFERRED:** the **new-document** dialog (a plain modal function with
  stock `Fl_Choice`) — folds in with its themed-Dropdown uplift when next touched (the S9-followup
  above).
  **Second follow-up (same day, user-reported):** *(d)* the now-focusable read-only `Fl_Output` drew a
  meaningless insertion **caret** ("^") when focused → hidden by matching `cursor_color` to the field
  ground (the useful selection highlight still shows; re-applied on re-theme). *(e)* **clicking a
  picker context-menu item that overhangs the canvas dismissed the picker:** in the route-through-parent
  model the parent `MainWindow::handle` sees the `FL_PUSH` *before* it reaches the on-top menu child, so
  `dismissActivePopoverOnOutsideClick` fired (click outside the picker) and *then* the menu committed —
  both. Fixed by sparing presses that land on the popover's own child pop-ups (the active `ContextMenu`
  OR `DropdownPopup`, which can extend beyond the popover) from popover dismissal — also closing the
  same latent dropdown-overhang bug. **User visual pass: PASSED 2026-06-15.**
- **User-picked CMYK profile name in Settings — DONE 2026-06-15** (transient pass, fix #3). The Color
  Management field showed only the *basename* of a user-chosen `.icc`, while the built-in default showed
  its embedded description. New `core::cmykProfileName(path)` reads any CMYK profile's `cmsInfoDescription`
  (the existing `defaultCmykProfileName()` now delegates to it); a `SettingsHost::cmykProfileName`
  callback (wired to it in `app_window`, keeping the dialog lcms2-free) lets `updateCmykDisplay()` show
  the **embedded description** as the field value, falling back to the basename if unreadable, with the
  full path still on the hover tooltip. Build + ctest green. **User visual pass: PASSED 2026-06-15.**
- **In-flight lasso/poly line — DONE 2026-06-15 (user-verified).** The FreeLasso/PolyLasso preview
  rasterized its path into a doc-pixel mask drawn with marching ants, so an angled drag staircased.
  Now a smooth **screen-space line** in the present pass: new SSBO (binding 4, `{count, pad, min, max,
  pts[]}`) carries the polyline + a bbox; the shader **bbox-culls** then draws a supersampled `segDist`
  line, **hard luminance-keyed** (pure white on dark / black on light — never grey, single colour/px so
  no barber-pole; flips B↔W where content crosses mid-grey, accepted). Canvas `syncLassoOverlay()` feeds
  it each frame (+ poly rubber-band); long freehand drags subsample by a **stable fixed-index stride**
  (no jiggle); committed-selection ants keep showing underneath. `kLassoMaxVerts = 4096`. Rect/Ellipse
  unchanged. **OPEN (queued, #3): freehand-lasso smoothing** — a Catmull-Rom smooth of the path; must
  smooth BOTH preview + commit (so the selection matches), so it lands as an opt-in **Tools setting**
  ("Smooth freehand lasso"), not always-on. Industry (Photoshop/Photopea) doesn't smooth; user wants it
  parked as "open for future discussion."
- **Settings → Inpainting → Backend Settings sliders go stale on a live theme change (user-flagged
  2026-06-22, NOT yet fixed).** Switching dark→light (or vice-versa) while the Settings dialog is open
  leaves the per-backend `ui::Slider`s painted in the old palette — they don't pick up the app-wide
  re-theme. Same root cause as the FlatButton baked-colour issue: the sliders cache palette colours
  (e.g. `setCellColor`) at construction and the Inpainting pane's `reapplyTheme()` path doesn't
  re-apply them. Fix: have the dynamic inpaint controls re-read the palette on `SettingsDialog::
  reapplyTheme()` (or rebuild them). Low priority; the modeless Fill dialog is immune (rebuilt per open).
- **Chrome polish trio (user-reported 2026-06; long-standing eyesores, batch into one cleanup
  commit alongside any UI session — at the latest S60-d):** (a) the stock FLTK **scrollbar** (layer
  panel) looks out of place next to the themed widgets — theme it like `ui::Slider`; (b) ~~jagged
  un-antialiased circles on slider grips + the colour-field marker ring~~ **done 2026-06-11**
  (`ui::renderAAPrims`/`drawAAPrims` in `theme.*` — coverage rasteriser over an under-sampler;
  reuse it for any future dot/ring chrome); (c) ~~the **menu/dropdown hover** state looks
  slightly off~~ **resolved for dropdowns 2026-06-14**: `ui::Dropdown` now opens a bespoke themed
  pop-up (`DropdownPopup` in `widgets.cpp` — a **child sub-window** of the top-level [like
  `ui::Popover`: Wayland-clean, no taskbar, won't paint over other windows], shown over the combo with
  the selected row aligned; hover = a neutral `controlHover` wash, selection = an accent **dot** on the
  right [AA disc, no glyph]) instead of Fl_Choice's stock Motif pulldown. **Still stock:** the top
  `Fl_Menu_Bar` menus (→ menu-bar redesign above; the last bare `Fl_Choice`s fell with the
  new-document redesign 2026-07-22); (d) the **rotated canvas' edges alias** (hard
  document boundary in the present pass — the 1-px coverage-blend used on the picker wheel/triangle
  edges is the same idea, applied in `canvas_present.comp`); (e) the **About dialog** is a stock
  `fl_message()` — massive "i" icon, unthemed button. Don't fight FLTK's message box: build a small
  custom About window (our widgets + the already-rasterized `appIconImage()`, version string, GPLv3
  line, repo link). *(Interim 5-min hack if it grates: `fl_message_icon()->image(...)`.)*
- **Wayland window/taskbar icon (user-reported 2026-06) — ADDRESSED 2026-07-28 (S59-a); the rest is
  packaging.** On native Wayland the Mosaic icon did not show — Wayland has no per-window pixel-icon
  protocol (X11's `_NET_WM_ICON` is why X11 works); compositors match the toplevel's **app_id** to an
  installed `.desktop` file's `Icon=`. **S59-a pins the app_id**: `Fl_Window::default_xclass("mosaic")`
  runs next to the backend pin, before the first window exists, so FLTK's `xclass` → app_id (and →
  `WM_CLASS` on X11, the same match there) is deterministic. `data/desktop/mosaic.desktop` already
  declares `Icon=mosaic` + `StartupWMClass=mosaic` and CMake already installs it plus the **scalable**
  hicolor icons, so the chain is complete **as soon as the app is installed**. Running from the build
  tree, with no `.desktop` in the search path, still shows a generic icon — expected, and a packaging
  (S59) matter rather than a bug. Verify with `cmake --install` to a prefix on `XDG_DATA_DIRS`
  (`docs/wayland.md` §3, §5 item 8). Still packaging-side: PNG raster icon sizes (some environments
  do not rasterize SVG), an AppStream `metainfo.xml`, and a reverse-DNS application id. (The newer
  `xdg-toplevel-icon` protocol may eventually let FLTK push the pixel icon directly; the `.desktop`
  file remains the portable fix.)
- **Flyout origin triangle (speech-bubble look).** The user suggests the tool flyout grow a little
  triangle pointing at its slot button. *Not* actually trivial: the popover is a rectangular child
  sub-window with **no transparency**, so a protruding notch needs a margin-strip + parent-matching
  paint trick or real shaping — design first, don't promise. (The flyout's dismiss-on-reclick *bug*
  is S12-a part 2 scope, §2.)
- **Layer rename + layer locking — ~~not yet in any session~~ DONE 2026-07-09, in S16-g** (`2f1a67a`,
  `bd98722`). Both halves landed as described below, with two refinements the sketch did not have:
  `SetNameCommand` also clears `TextLayer::autoNamed` (undo restores it), so renaming a Text layer is not
  a silent no-op; and the full lock is enforced against *structural* edits in the panel **and** against
  transforms at `beginMoveGesture`, where **any** locked layer in a multi-selection vetoes the whole
  gesture (Photoshop's rule) with the host naming the refusal in the status bar. Visibility, opacity and
  blend were deliberately left editable. The finer **pixel / position / alpha** locks are still open and
  are the only part of this entry that remains. Original sketch, kept for that remainder: **Rename** is small:
  double-click the `LayerRow` name → inline edit → an undoable `RenameLayerCommand` (the name already
  lives on the layer). **Locking** is Photoshop-style per-layer locks — full lock first (command-stack
  guards + a row lock glyph), finer pixel/position/alpha locks later (alpha overlaps the brush
  engine's Protect Alpha, §3.12). Together ≈ one small session; a good Phase-2/3 gap-filler (an
  S10-e if layer-panel work reopens, or wherever the user pins it).
  - **NOTE (2026-06-19, from S19-a brush dogfooding):** `Layer::locked()` is currently only enforced
    by Merge Down — no edit tool honoured it until the brush. The Brush now **respects the lock**
    (the reticle draws a padlock + a "layer is locked" status hint on a paint attempt; `VulkanCanvas`
    has the enforcement). ~~There is **no layer-panel lock glyph or lock/unlock UI yet** — that's this
    backlog item, so once a layer is locked there is no in-app way to unlock it.~~ **Closed 2026-07-09:**
    the row draws a lock icon at all times (muted when unlocked — a hover-reveal cell read as a hole),
    and Lock/Unlock is in the row context menu.
  - **RESOLVED (user 2026-06-19): the New-doc opaque Background is now created UNLOCKED**
    (`new_document_dialog.cpp`) — a fresh white/black canvas paints immediately (Krita/GIMP/Affinity
    behaviour; File→Open also opens unlocked). The full-lock that *blocks* painting stays the model
    for layers the user explicitly locks (with the reticle padlock feedback). Photoshop-style
    **partial** locks (position/alpha that still allow painting) remain a possible later refinement
    when this lock UI lands.
- **Liquify (push/bloat/pucker/twirl brushes).** Brush-driven local image warping, distinct from the
  grid-based **Mesh Warp + Perspective Warp** tool (S35-b). A natural sibling of the brush family and
  the warp tools; schedule after both exist (≈ Phase 5/6).
- **Crisp pixels + pixel grid at high zoom.** ~~Bilinear-only present pass blurs when zoomed in.~~
  **DONE in S19-c (2026-06-16)** — nearest when magnifying (hardcoded on the zoom ratio, not a
  threshold/toggle), View > Show Pixel Grid, and screen-space transparency checker. See the S19-c
  entry above. (The per-pixel **loupe** magnifier remains distinct — its own grid, ships with the
  eyedropper tool **S24**.)
- **Force software / CPU rendering (device selection).** The headless/golden path already has a
  **CPU compositor fallback** (§3.4/§3.15), and because Mosaic targets the Vulkan 1.2 baseline a
  software ICD like **Mesa lavapipe (LLVMpipe)** already runs the *full interactive path* unmodified
  when it is the selected/only Vulkan device — just slowly. What's missing is **explicit control**: a
  `--device` / settings option to **enumerate adapters and force the software/CPU one** (and a clean
  "no usable GPU → software, with a warning" fallback), rather than depending on `VK_ICD_FILENAMES`
  or driver ordering. ~~**Scheduled (2026-06): S60-d.**~~ **HALF DONE 2026-07-29 (`0ba7e0d`, S60-f):**
  `--device` / `MOSAIC_DEVICE` selects an adapter, every enumerated device is logged, an unmatched
  selector warns and falls back, and the presenting context stopped making its own ad-hoc pick — both
  contexts now share one `pickPhysicalDevice`. ❌ What is still missing is the *other* half of this entry:
  a clean "no usable GPU → software, with a warning" path, and the lavapipe run itself has not been
  exercised. Note the Vulkan baseline moved to **1.0** at S60-α, which only widens what a software ICD
  can serve. (`MOSAIC_GPU_PROFILE=floor` and `MOSAIC_CPU_ONLY=1` are the two related switches that do
  exist, and both are covered by the test suite.)
- **Present-paced rendering (refresh-rate adaptive + render-on-demand).** The canvas is driven by a
  fixed **60 Hz `Fl::repeat_timeout`** today — fine to bootstrap, but it under-draws high-refresh
  panels (120/144 Hz) and wastes power redrawing an idle editor at 60 fps. The fix is **not** to poll
  the display's Hz: pace by **swapchain present timing** instead — present in **FIFO** (vsync) so the
  GPU is paced to the panel's actual refresh automatically, and **only render when the document/view
  is dirty** (idle ⇒ no frames). On Wayland this maps to **`wl_surface.frame` callbacks** (the
  compositor calls us at the right cadence); and the refresh rate *is* exposed there via
  `wl_output.mode` if ever needed — so this is **not** blocked by Wayland's restrictions (it's
  arguably the better-supported path). Touches the `render::WindowRenderer` present loop + the
  `VulkanCanvas` frame driver; ~~**scheduled (2026-06): S60-b**~~ — re-slotted to **S60-c** by
  `docs/s60-performance-plan.md` §7 and **LANDED 2026-07-29** (`655b5d6` pacing on the display rather
  than a 60 Hz clock, `d0550ce` input driving the frame rate with focus deciding only how cheap idle
  gets, `70c461c` background culling + a readout that stops touching the device). The prediction held —
  pacing by present timing rather than by polling the display's Hz is what the fix turned out to be. ❌
  The **off-thread composite** half of that session is not done: the composite still runs on the UI
  thread. Pairs with the frame-coalesced recompositing already in place.
- **Interactive edit latency — scope & offload the live re-composite.** The opacity-slider *freeze* is
  fixed (recomposites are coalesced to one-per-frame via `requestRecomposite()` → `MainWindow::onFrame`),
  but a **continuous single-layer edit** (the opacity / blend-mode slider drag) still previews with a
  *little* lag. Two structural causes remain: (1) **a one-layer change re-composites the whole document** —
  `recompositeNow()` calls `render::composite(*doc, …, Backend::Cpu)`, which re-walks and re-blends the
  *entire* layer tree and re-uploads the full image to the canvas texture every frame, even though only one
  layer's opacity moved; and (2) it runs **synchronously on the FLTK UI thread** inside the frame timer, so
  a heavy composite caps the achievable frame rate and adds input→canvas latency. Fix directions: **scope
  the work to what changed** — cache the composite of the subtree below/above the edited layer and only
  re-blend the changed layer per drag-frame (the **dirty-region / tiled** half of the §2 "Deferred to S60:
  GPU residency + tiled dirty regions" note); keep layers **GPU-resident** so a re-blend is a GPU dispatch
  rather than a CPU whole-tree walk + full re-upload (the GPU-residency half of that note — today even
  `render::GpuCompositor::blendOver` round-trips per layer); and/or move the composite **off the UI thread**
  onto the render/worker thread (§4 threading model) so it never stalls input. Distinct from but pairs with
  the **present-paced rendering** item above — that governs *when* a frame is drawn, this governs *how
  cheaply* a frame's composite is produced. (A release build also masks much of this: the CPU compositor is
  unoptimized in debug — but the structural fix is the scoping + offload, not the build type.)
  **Scheduled (2026-06):** the scoping/GPU-residency half is **S60-a**; the off-UI-thread + pacing
  half is **S60-b**.
- **HEIF/HEVC**: optional, system-codec only, never bundled (§7).
- **Animation/frames, onion-skinning**: far future; the layer model leaves room.
- **Plugin architecture, advanced scripting maturity, OpenColorIO, resvg-based SVG import**: later
  enhancements. (Base Lua scripting itself is **scheduled: S40**, not deferred.)
- **Clang as a release-gated compiler** (currently best-effort).

---

## 13. Risks & Open Questions

- **FLTK custom-widget volume / HiDPI / IME.** Mitigation: §3.3 fallback to Qt 6 with the
  toolkit isolated in `ui/`+`platform/`. Decision checkpoint after S10/S29.
- **FLTK↔Vulkan windowing per platform/backend** (X11 vs **Wayland** vs Win32 vs Metal):
  validated in S3 for **X11/XWayland** (quirks in `docs/vulkan.md`); **native Wayland** needs a
  `wl_subsurface` (§12) and is a **prerequisite for HDR output** — XWayland/Xorg are SDR-only
  (§3.6, S43). Win32/Metal arrive with their ports (S57/S58).
- **PSD/PSB fidelity** (text engine, layer styles, smart objects): set expectations; ship
  best-effort with explicit loss warnings (S46/S47).
- **Inpainting quality:** the built-in default is the **He & Sun offset-statistics** graph
  solver — comparable-class quality to PatchMatch-based fills without implementing one (S37-a
  research note), with a **Telea/NS** PDE fast-path. Higher-quality **ML** inpainting is
  **deliberately not bundled** — it's left to **user Lua scripts** via the S40 pluggable backend
  (with an example hook), so Mosaic ships no inference dep and no weights, and weight licensing is
  the user's concern, not ours.
- **Cross-compiling the dependency set to Windows** (which MinGW libs are packaged vs need
  manual builds): enumerate in `docs/build-windows.md` (S57); flag ⚠ in README.
- **Performance on huge/HDR documents:** addressed by tiling, dirty regions, proxy preview,
  GPU memory budgeting (S7/S60).
- **Two un-homed S43 prerequisites — RESOLVED (2026-06, with the user):** S43 was pre-split in
  dependency order — **S43-a** (float pixel pipeline) → **S43-b** (compositor linear-light + ICC
  re-plumb) → **S43-c** (HDR formats/indicators/output) — and the overloaded S60 was split into
  **S60-a–d** (§9), which also rehomed tiled pixel storage (**S60-c**) and the §12 perf items.
- **S19-a will likely pull a slice of S60-a forward:** GPU brush stamping onto CPU-resident layers
  would round-trip per dab; expect the *active layer* to go GPU-resident in S19-a, with full
  residency + tiled dirty regions still S60-a.

---

## 14. Docs Index (`docs/` — created/grown across sessions)

> **Reconciled against the tree 2026-07-29.** This index had drifted to about half the directory. The
> rule it now follows: **every file in `docs/` is listed, and a name that is a plan rather than a file is
> marked `(planned)`** — an index that silently mixes the two is worse than no index, because it makes a
> missing note look like a note you failed to find. Several of these are the *detailed surface* for a
> tracker line that is deliberately only an index entry; that pattern is intended (see §10).

**Foundations & platform** — `architecture.md` · `document-model.md` · `compositor.md` (incl. the
coverage-partition operator and its `over`-impossibility proof) · `vulkan.md` · **`wayland.md`** (the
native-Wayland backend — default since S59-a: what the flip changes, the cursor contract, desktop
integration, the FLTK traps, and the checks only a human on a real session can make) · `theming.md` ·
`i18n.md` · `settings-and-logging.md` · `build-windows.md` · `build-macos.md` · `credits.md` ·
`third-party-licenses.md` · `icons-needed.md` (running icon inventory → S52).

**Performance (S60)** — **`s60-performance-plan.md`** (the ledger of record: ground truth, the capability
tiers, the tile/residency model, the α…-f session split, and §7's five-condition gate) ·
`s60-bench.md` · `s60-readback-consumers.md` (the audit that found 19 consumers, not five, and corrected
the proposed API) · `s60-gesture-start-stall.md` (the start/END stall measurements and findings G1–G6).

**Formats & I/O** — `mosaic-native-format.md` (S48 spec) · `mosaic-native-format-research.md` (S48
research narrative) · `async-save-design.md` (the off-thread full write) ·
**`export-system-plan.md`** (the export/I-O milestone ledger M1…M7 — S41/S42/S56 all land through it;
§8 is the file-picker modality story) · `heic-strategy.md` (HEIC delegation; parked) ·
`psd-notes.md` *(planned → S46)*.

**Tools, paint & selection** — `brushes.md` (the four-arc engine rework, the verified format reference,
and the running conformance census) · `brush-opacity-prior-art.md` ·
`tablet.md` · `bucket-fill.md` · `gradient-tool.md` · `eyedropper-loupe.md` · `clone-stamp.md` (S38) ·
`red-eye-tool.md` (S38-b, incl. §9.8's three-defect rim) · `research-selection.md` (S17/S18) ·
`research-select-brush.md` · `smart-resize-research.md` · `smart-recompose-plan.md` ·
`inpainting-research.md`.

**Layers, filters & effects** — `adjustment-layers.md` (S32/S34/S34-a, and the standing constraints on
future filter work) · `blur-filters.md` (S33) · `filters-stylize.md` (S35) · `layer-effects.md` ·
`le-d2-image-patterns.md` · `image-operations.md` (S53-a: the one document-remap engine, the anchor
model, the guides rebase, what Image Size bakes vs carries, and the resampler extraction) ·
`texture-generator.md` (S55) · `research-sky-estimate-from-layer.md`.

**Vector & type** — `vector-model.md` (vector stack + renderer; §8 the Pen tool + its chrome, §9 the
boolean kernel) · `type-tool.md` (Type tool design, S29/S30; §9 warp + fit-to-path) ·
`type-deferred-features.md` · `type-vertical-writing-mode.md` · `spell-check-plan.md`.

**UI odds and ends** — `askortell-dialog.md` · `motivational-ticker.md`.

**Planned, not yet written** — `scripting.md` (Lua API + inpaint-hook example → S40).

---

## Appendix A — Requested-feature → Session traceability

| Brief requirement | Session(s) |
|---|---|
| PSD/PSB support | S46, S47 |
| `.mosaic` open/extendable/documented format | S48 |
| ~GIMP-level format coverage | S42, S43, S44 |
| RAW + camera info | S45 |
| Output-format loss warnings (alpha/HDR/flatten/rasterize/…) | S41, S56 |
| HDR support + indicator + display-HDR warning | S43 |
| Colorspace support + indicator | S12, S13-b, S43 |
| New-project preset sizes A1–A4… | S9 |
| Left toolbar | S11 |
| Tool options bar (most-changed options per tool; the primary per-tool surface) | S11 |
| Stamp / Clone tool (Ctrl-pick a source, stamp it through the brush tip) | S38 |
| Inpaint brush tool | S39 |
| Edit→Fill…→Inpaint | S39 |
| Inpainting research note (papers + architecture) | S37-a ✅ |
| Inpainting engine + pluggable backends (+ diffusion PdeBackend) | S37-b ✅ |
| Inpainting default backend (He & Sun offset-statistics graph solver) | S37-c ✅ COMPLETE (engine default; KD-tree NNF + working-region + α-expansion + boundary seams + Poisson blend + two-scale; combination as designed) |
| Scripting infrastructure (Lua) + user-pluggable inpaint backend + example hook | S40 |
| Select rect/ellipse/lasso | S14 |
| Magic wand (researched) | S17 |
| Select brush (researched) | S18 |
| Non-destructive layer filters (Filter menu, scoped downward) | S32 |
| Layer groups | S6, S10 |
| Blur/grayscale/artistic filters | S33, S34, S35 |
| Unit tests | S1 + all |
| A way for Claude to debug | S2 (headless harness) + all |
| Select tool (cursor) → select in stack + transform box | S15 |
| Crop tool | S16 |
| Typeface tool (AA modes, per-part styles, bend, 3D, fit-to-path, rasterize/to-path) | S29, S30 |
| Blur/Dodge brush | S23 |
| Color-picker tool + loupe magnifier + grid | S24 |
| i18n (English first, easy to add) | S5, S54 |
| Line tool (vector, custom stroke) | S27 |
| Brush tool + presets + Krita brushes | S19, S20 |
| Vector elements in layer stack | S25 |
| Vector document type (resolution-independent canvas, vector-only) | S30-b (deferred; gated on S48 + New-Document redesign) |
| Shape tool (+ Shift/Ctrl/Alt) | S26 |
| Tools use active color | S11, S12 + all |
| Shift/Ctrl/Alt resizing & shape constraints | S15, S26 |
| Gradient tool (linear/elliptical/radial/conical + curves, editable layer) | S22 |
| Bucket fill | S21 |
| Custom path tool (custom stroke) | S28 |
| Space-pan, R-rotate overlay/snap/double-tap reset | S8 |
| Layer masks | S31 |
| Icons for tools/UI | S11 (initial), S52 |
| Mask from selection | S31 |
| Shift-click layer preview → select contents | S10 |
| Animated marching-ants selection | S13 |
| Rasterize (right-click; rasterize-down on effects) | S36 |
| Tooltips (everywhere **except the canvas**) | S4 + all; canvas exception S8 |
| Keybindings in settings (defaults = the shortcuts Mosaic already ships, per the 2026-07-29 ruling — **not** Photoshop's) | S51 |
| Tabbed open-file selector with X + save/discard | S49 |
| Drag-drop (tab→open, canvas→layer) | S50 |
| Files-as-layers are magic layers | S6, S50 |
| Right tabbed panel (Layers default; the generic Properties mirror was **cut** in S11-c — a *contextual* Properties dock returns with S15/S32) + add/del/drag/clone-on-plus(green) | S10, S11 (→ S15/S32) |
| Menu bar (File/Edit/Image/Layer/Type/Select/Filter/View/Help; Save/Save as/Export; About) | S3, S53 |
| Toolbar diagonal two-square color picker behavior | S11 |
| Full Vulkan acceleration throughout | S2, S7, S24, S33+ |

## Appendix B — Glossary

- **Magic layer** — a layer that keeps its original full-resolution source + a transform,
  resampling from the source so repeated transforms minimize information loss (the
  non-trademarked equivalent of a "smart object").
- **Adjustment/Filter layer** — a non-destructive effect node that modifies the composited
  result of layers below it within its group (or globally if ungrouped).
- **Marching ants** — the animated black/white dashed outline of a selection.
- **Loss warning** — the pre-export diff between document capabilities and target-format
  capabilities (alpha, HDR, layers, vector), surfaced to the user.
- **Headless op-runner** — GUI-less execution of the command system for scripting + tests.

## Appendix C — Sources / references

- FLTK 1.4 theming & schemes / subclassing: <https://www.fltk.org/doc-1.4/subclassing.html>,
  <https://www.fltk.org/doc-1.4/common.html>
- FLTK custom window surfaces / `Fl_Window`: <https://www.fltk.org/doc-1.3/classFl__Window.html>
- MoltenVK ⇒ Vulkan 1.3 (2025): <https://www.khronos.org/news/permalink/moltenvk-1.3-released-for-vulkan-1.3-support-on-apple-devices>,
  <https://github.com/KhronosGroup/MoltenVK>
- LaMa-style / hybrid inpainting context (reference for the S40 user-script inpaint hook, not a
  bundled dep): <https://research.adobe.com/publication/supercaf/>
- PSD: Molecular Matters `psd_sdk` (BSD-2, read+write): <https://github.com/MolecularMatters/psd_sdk>;
  Adobe Photoshop File Format spec: <https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/>
- RAW: LibRaw (LGPL-2.1 / CDDL): <https://www.libraw.org/docs>, <https://github.com/LibRaw/LibRaw/blob/master/LICENSE.LGPL>
- HEIF/HEVC packaging & the AVIF alternative: <https://github.com/strukturag/libheif/issues/591>
- Krita `.kpp` brush format: <https://docs.krita.org/en/user_manual/loading_saving_brushes.html>,
  <https://community.kde.org/Krita/PaintOp_Presets>

---

*End of PLAN.md. Keep §2 and §10 current — they are the resume contract for future sessions.*
