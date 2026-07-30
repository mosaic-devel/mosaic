# Tablet & Stylus Input — Research Note

> **Scope.** The required research note for **S19-b** (`PLAN.md` §0: research-first sessions begin by
> writing a `docs/` note). It settles how pressure, tilt and rotation reach Mosaic's tool pipeline on
> each platform, the event model they are normalized into, and the smoothing/stabilizer policy layer
> that consumes them. Companion to **`docs/brushes.md`** — the brush dynamics designed there are inert
> until this lands.
>
> **Decision (2026-07-09):** build **Linux** now — XInput2 *and* `zwp_tablet_v2`. Windows and macOS
> are fully designed here and built with their ports at **S57** / **S58**. There is no Windows build
> to test against today, and a driver-workaround layer can only be validated against real hardware.

---

## 1. Why this is platform work

**FLTK 1.4.5 has no tablet API.** Verified: no pressure/tilt/stylus symbols in any `FL/` header, and
no `zwp_tablet` strings in `libfltk`. `Fl::event_x()` / `event_y()` return integers; there is no
valuator channel of any kind.

Mosaic's brush engine has carried `StrokeSample::pressure` and a `BrushDynamics` struct since S19-a,
but **both call sites pass `1.0` as a literal** (`vulkan_canvas.cpp`, `pushBrushTool` /
`dragBrushTool`). Nothing downstream has ever seen a real valuator.

So the device layer is ours to write, per platform. Toolkits that "support tablets" generally do so
inside their platform plugin; there is no portable library to lean on that does not also drag in a
whole toolkit.

Two escape hatches make this tractable:
- `Fl::add_system_handler(Fl_System_Handler h, void *data)` — on X11 the handler receives the raw
  `XEvent*` before FLTK dispatches it. **The same hatch works on Windows** (verified S57): FLTK's
  event loop hands the handler each raw `MSG` after `PeekMessageW` and before
  `TranslateMessage`/`DispatchMessageW`, which is how §5a reaches `WT_PACKET` and `WM_POINTER*`
  without subclassing anything.
- `fl_wl_display()` / `fl_wl_compositor()` expose the Wayland connection, and Mosaic already binds
  registry globals directly in `src/platform/wayland_subsurface.cpp`.

---

## 2. The unified event model

`src/platform/tablet.{hpp,cpp}` normalizes every backend into:

```cpp
struct TabletSample {
    common::Vec2 pos;                 // document-independent surface coords, SUB-PIXEL
    double pressure;                  // [0,1], normalized by the device's reported axis maximum
    double xTilt, yTilt;              // degrees
    double rotation;                  // degrees; barrel/art-pen rotation
    double tangentialPressure;        // [0,1]; airbrush finger wheel
    enum class Tool { Pen, Eraser, Airbrush, Puck, Mouse } tool;
    std::uint64_t toolSerial;         // per-stylus unique id, 0 if unavailable
    bool inProximity;
    std::uint32_t buttons;
    std::uint64_t timeUs;             // OUR clock, not the driver's (see §5)
};
```

Derived sensors (`speed`, `drawingangle`, `distance`, `fade`, `time`, `fuzzy`, `fuzzystroke`) are
computed in `core/brush/` from the sample stream, not by the platform layer. `ascension` /
`declination` are derived from `xTilt`/`yTilt`.

The platform layer is FLTK-free and headless-testable: a backend is a source of `TabletSample`s, and
the tests feed a canned stream.

---

## 3. X11 / XInput2 — the primary path

XWayland is Mosaic's default session (`PLAN.md` §3), so this is the path most users will hit.

- `XIQueryVersion` ≥ 2.2; `XIQueryDevice` to enumerate slave devices and their valuator classes
  (`Abs Pressure`, `Abs Tilt X`, `Abs Tilt Y`, `Abs Wheel`), recording each valuator's `min`/`max` for
  normalization.
- `XISelectEvents` on our window for `XI_Motion`, `XI_ButtonPress`, `XI_ButtonRelease`, `XI_Enter`,
  `XI_Leave`.
- Hook through `Fl::add_system_handler`. XI2 events arrive as `GenericEvent` cookies, so
  `XGetEventData` / `XFreeEventData` are ours to manage. Match `xi_opcode` from
  `XQueryExtension("XInputExtension", …)`.
- Eraser detection: the stylus's inverted end is a **separate slave device** whose name conventionally
  ends in `eraser`. Prefer the device's `KIS`-style tool type where exposed; fall back to the name.

### 3.1 The duplication trap, and the design that avoids it
FLTK still receives core pointer events for the same motion. Selecting XI2 events on the master device
would double every sample; suppressing FLTK's core events would mean reimplementing hit-testing,
focus, click counting and drag semantics.

**Design.** XI2 fills a lock-free sample ring. FLTK's `FL_PUSH` / `FL_DRAG` / `FL_RELEASE` continue to
drive the *stroke lifecycle* exactly as they do today. On each `FL_DRAG`, the canvas **drains the
ring** and feeds every buffered sample to `BrushEngine::extendTo()` in order. With no stylus in
proximity, synthesize a single sample from `Fl::event_x/y()` at pressure 1.

This buys three things:

1. **Pressure, tilt and rotation**, obviously.
2. **~200 Hz sampling** instead of FLTK's coalesced motion rate.

   > ⚠ **This bullet used to end "…and it is why `docs/brushes.md` §6.2 can defer curve
   > interpolation." That was wrong, and the deferral was the bug.** A high sample rate does not
   > stop a stroke going polygonal; it only makes the polygon have more sides. It hid the defect on a
   > *tablet* while leaving it in plain sight on a **mouse**, which delivers one position per motion
   > event — so a 60 Hz mouse stroke was a literal 60-gon, and a 160 Hz display made it "much less"
   > bad rather than fixing it. Interpolation was never optional; the tablet was simply papering over
   > its absence. The dab walk now lays dabs along a **curve through the samples**
   > (`core/brush/stroke_path.hpp`, §6.2), and the sample rate buys what it actually buys: fidelity,
   > not smoothness.
3. **Sub-pixel dab placement.** Tablets report device coordinates far finer than `Fl::event_x()`'s
   integers. Bitmap tips need this (`docs/brushes.md` §6.2).

The ring is single-producer/single-consumer: the X11 system handler runs on the FLTK thread, so a
plain ring buffer with a head/tail index suffices — no atomics contention, no locking in the paint
path.

> **X11-only conclusion (2026-07-10).** This lifecycle design relies on the server's core-pointer
> emulation, which X11 always provides. On Wayland, binding the tablet manager suppresses pointer
> emulation for the pen entirely — see the §4 built-note, finding 4. The Wayland backend owns the
> stroke lifecycle itself.

### 3.2 Failure modes
- **No XInput2** (ancient X server / remote display) → backend reports unavailable, canvas falls back
  to synthesized pressure-1 samples. A stroke still paints.
- **A device with no pressure valuator** (some screen digitizers, most mice) → `pressure` is reported
  as 1.0 rather than 0, so size/opacity dynamics do not silently collapse the stroke to nothing.

> **Built-note (2026-07-10) — §10 steps 2–3.** `platform/tablet.{hpp,cpp}` (TabletSample, the
> overwrite-oldest SampleRing with an honest loss counter, the backend interface, the ingest
> clock), the §7 policy layer minus the still-unbuilt stabilizer (`core/brush/tablet_policy.*`;
> `SpeedSmoother` extracted from StrokeState bit-identically, a `==` parity test pins the seam),
> and the XI2 backend (`platform/tablet_x11.*`) are BUILT, headless-tested per §9 (hand-built
> `XIDeviceEvent`s; 44 cases; 17/17 mutants killed), and proven live by `tools/tablet_spike_x11`
> (MOSAIC_BUILD_TABLET_SPIKE, same uinput pen). The **canvas drain is deliberately not wired
> yet** — it lands with the dynamics wiring (§10 step 5), when `TabletSample` actually reaches
> `StrokeInput`. Facts the probe earned on KWin 6.7.0 / XWayland:
>
> 1. **Tool matching is CONTAINS, not ends-with.** XWayland names its emulated devices with a
>    client-ordinal suffix — observed verbatim: `xwayland-tablet stylus:11` / `eraser:11` /
>    `cursor:11`. The "conventionally ends in eraser" phrasing above is true of the bare Wacom
>    driver only; an ends-with match would misclassify every XWayland eraser as a Pen.
> 2. **XWayland pre-creates its tablet slaves EAGERLY**, before any tablet hardware exists, and
>    routes every Wayland tool through them: hotplug grows nothing and fires no
>    `XI_HierarchyChanged`. The hierarchy re-enumeration path is for native X servers; on
>    XWayland, init-time enumeration is already total (and Settings→Tablet's device list will
>    show the three xwayland-tablet entries even with no tablet attached — expected, not a bug).
> 3. Valuators carry the standard label atoms (pressure `[0..65535]` on axis 2, tilt `[-64..63]`
>    on 3/4); evdev tilt signs pass through unflipped, and the peak-magnitude tilt scaling parsed
>    a half-range lean to exactly ±30.00°.
>
>    > ⚠ **This finding also said "sub-pixel `event_x` is real through XWayland (`752.4805`
>    > observed), so §3.1's payoff #3 holds on this path too." That conclusion was WRONG, and it
>    > cost a release.** A fraction was observed and taken as proof of fidelity. It is not:
>    > `event_x` is the X server's screen-mapped **pointer**, and its fractional part is an artefact
>    > of that mapping, not the pen's position. Measured 2026-07-11 (`tools/xi2_valuator_probe`,
>    > `tools/tablet_diag_x11`): a pen driven along a dead-straight line reaches the brush engine
>    > with a **0.49 px RMS sawtooth** that flips direction on 4 of every 5 samples and sometimes
>    > steps *backwards* along the stroke. That was the staircased X11 stroke users reported while
>    > the same pen on native Wayland was smooth (0.011 px RMS). The pen's real position is in the
>    > **`Abs X`/`Abs Y` valuators**, which are 43× more faithful — see the §3.5 built-note.
>    >
>    > The lesson generalizes: *a number having decimals is not evidence that the decimals mean
>    > anything.* Payoff #3 needed a fidelity measurement (deviation from a known path), not an
>    > existence check.
> 4. **Normalization decisions settled in code:** tilt maps the device range onto ±60° by PEAK
>    MAGNITUDE (hardware zero stays exactly 0° over Wacom's asymmetric −64..63 declaration);
>    `Abs Wheel` is tangential pressure on an Airbrush and barrel rotation (±180°) otherwise;
>    a pressure axis with a degenerate declared range reports 1.0 like a missing one; the
>    event's button mask (state BEFORE the event) is combined with the event's own transition.
> 5. **Policy-layer fact:** the tilt-direction offset rotates the (xTilt, yTilt) pair, shifting
>    `ascension` by exactly the offset and preserving the lean magnitude — but the `declination`
>    READING may drift a fraction of a percent, because the reference elevation formula
>    normalizes by whichever axis dominates and is therefore mildly direction-dependent. No x/y
>    rewrite can both shift the bearing and hold declination exactly still.

---

### 3.5 Position comes from the valuators — **fixed 2026-07-11**

The staircased X11 stroke. Users saw it on the shipped (XWayland) path while the *same pen* on
native Wayland was smooth; a mouse was jagged everywhere (that is a separate bug — straight chords
between samples).

**Three suspects were named up front, and all three were wrong.** Measured against the real
`ui::TabletInput`, driven by the uinput virtual stylus, capturing the exact `StrokeInput` stream the
brush engine receives (`tools/tablet_diag_x11`, `tools/pen_driver`):

| suspect | verdict |
|---|---|
| `drain()` synthesizing a pressure-1.0 sample when the ring is empty | **never fires** — 0 of 301 samples |
| the X server compressing XI2 motion to the core pointer's rate | **not happening** — 202 samples/s arrive |
| sub-pixel loss (positions quantised to whole pixels) | **false** — positions do carry a fraction |

What is actually wrong is the **fraction itself**. `ev.event_x/event_y` is the server's screen-mapped
*pointer*, and its sub-pixel part is an artefact of that mapping rather than the pen's location. Driven
along a dead-straight line:

| position source | deviation from the line | direction changes |
|---|---|---|
| `event_x/event_y` (what shipped) | **0.488 px RMS**, ±0.76 px | 240 of 300 samples |
| `Abs X`/`Abs Y` valuators | **0.011 px RMS**, ±0.02 px | — |
| the same pen on native Wayland | 0.011 px RMS | — |

So the fix is to take position from the device's own valuators — and
the X11 stroke becomes *numerically identical* to the Wayland one (verified end-to-end in the app:
0.0112 px RMS on both, landing in the same place).

**The mapping is the dangerous part, and it is guarded.** The valuators are device units; turning
them into pixels assumes the device's declared range spans the screen. That is true under an identity
Coordinate Transformation Matrix (confirmed: XWayland/KWin sets exactly that) but *false* for a
tablet mapped to one output of several — and a wrong mapping paints hundreds of pixels from the pen,
which is far worse than a wobble. So every event cross-checks its valuator-derived screen position
against the server's own `root_x/root_y`, and falls back to `event_x/event_y` — today's behaviour —
whenever they disagree. The window's origin comes from the event itself (`root_x - event_x`), which is
exact: both endpoints carry the same server wobble and it cancels in the difference.

**The verdict is sticky, per device, never per event.** ⚠ The first cut checked the mapping on every
event with a tolerance sized to the server's noise (2 px) — and it *flapped*, switching position
source **inside a single stroke**. Mixing two sources sample-by-sample was **worse than either alone**:
it put 100 backward steps into a stroke that only ever moved forward. The tolerance is now deliberately
generous (8 px: the server's noise is ~1 px, a mis-mapping is off by hundreds), and a device must agree
for `kXi2MapTrustSamples` events before the valuator path engages at all. A hovering pen streams
continuously, so this is long settled before the tip can touch down.

## 4. Wayland / `zwp_tablet_v2` — the risky one

Bind `zwp_tablet_manager_v2` from the registry; Mosaic already walks the registry in
`WaylandSubsurface::create`. Then `zwp_tablet_manager_v2.get_tablet_seat(seat)` →
`zwp_tablet_seat_v2`, whose `tool_added` event yields a `zwp_tablet_tool_v2` carrying `type`
(pen/eraser/airbrush/…), `hardware_serial`, and a set of `capability` events (pressure, tilt,
rotation, distance, slider, wheel). Motion arrives as `proximity_in` / `down` / `motion` / `pressure`
/ `tilt` / `rotation` / `up` / `proximity_out`, batched by `frame`.

⚠ **A `frame` can batch `up` + `proximity_out` together** — that is what the compositor sends when
the pen lifts clear of the pad *at a stroke's end* (a hover-then-lift arrives as two frames). The
sink dispatch delivers **both**, End first, then the leave *(2026-07-14)*: the first cut dispatched
one callback per frame, and the swallowed leave meant no `FL_LEAVE` — the canvas kept the pointer
"inside", and the pen's tool cursor came back over the next surface still wearing the canvas's
*Hidden*. That was the sometimes-invisible cursor on native Wayland, and "sometimes" was exactly
this batching.

Position arrives as `wl_fixed` — **sub-pixel by construction**, which is the payoff.

> ⚠ **Two unknowns. Spike before designing around them.**
>
> 1. **FLTK does not expose its `wl_seat`.** `fl_wl_display()` and `fl_wl_compositor()` exist;
>    there is no `fl_wl_seat()`. We must bind our *own* `wl_seat` from the registry. A client may bind
>    a global more than once, so this should work — but the tablet seat we get must be the same
>    physical seat FLTK is using, and nothing in the protocol guarantees we pick the same one on a
>    multi-seat system.
> 2. **Tablet events are delivered per `wl_surface`.** Our Vulkan content lives on a *child
>    subsurface* stacked over FLTK's surface (`wayland_subsurface.cpp`). Whether `proximity_in` targets
>    the subsurface or the parent depends on the subsurface's input region — which, since the
>    subsurface landed, Mosaic sets EMPTY (`wayland_subsurface.cpp`; the "fallback" below is the
>    shipped configuration). Whether tablet-v2 honours it like `wl_pointer` does must be established
>    empirically, not from the spec.
>
> The spike is a ~150-line standalone client: bind seat + tablet manager, print `proximity_in`'s
> surface pointer, compare against `fl_wl_surface(fl_wl_xid(win))` and our subsurface. Run it before
> the Wayland backend is written.

Fallback if the surface routing proves unworkable: set an empty input region on the Vulkan subsurface
so all input lands on FLTK's surface, and take tablet events there. Mosaic's canvas already receives
mouse events on native Wayland today, so this is the configuration that is known to work for pointers.

> **Built-note (2026-07-10) — the spike ran; the risk is retired.** Two spike binaries, built
> with `-DMOSAIC_BUILD_TABLET_SPIKE=ON`: `tools/tablet_spike` (standalone client, fullscreen
> parent + two subsurface controls) and `tools/tablet_spike_fltk` (FLTK owns the connection and
> event loop, the REAL `WaylandSubsurface` as the child). Both are driven by a uinput virtual
> stylus (`tools/virtual_pen.hpp`) so the compositor ingests events through udev/libinput
> exactly like hardware, and both interlock: hover-only until `proximity_in` proves the pen is
> over our own surface, tip-down only at a position measured to be well inside it. Observed on
> KWin 6.7.0 / wayland 1.25, single seat. Findings:
>
> 1. **Unknown #1 is a non-issue.** A self-bound `wl_seat` (v1) handed to
>    `zwp_tablet_manager_v2.get_tablet_seat` delivers everything: `tablet_added` ~170 ms after
>    hotplug, tool type + capabilities, proximity/motion/pressure/tilt/down/up with correct
>    serials. Proxies left on the DEFAULT queue dispatch from inside `Fl::wait()` — the
>    private-queue bind + hand-back pattern from `wayland_subsurface.cpp` extends to the seat
>    and manager unchanged. The multi-seat caveat stays unproven (single-seat machine) but it
>    affects seat *choice*, not the mechanism.
> 2. **Unknown #2 resolved — input regions govern tablet routing exactly as they govern
>    pointers.** Over a mapped subsurface with an EMPTY input region (the shipped Vulkan-canvas
>    configuration), `proximity_in` targets the PARENT — pointer-identical to
>    `fl_wl_surface(fl_wl_xid(win))` — with parent-local coordinates; a control subsurface with
>    the default infinite region CAPTURES the pen (`proximity_out(parent)` →
>    `proximity_in(child)` on crossing). The backend therefore listens on the FLTK surface and
>    needs no input-region work at all.
> 3. **Sub-pixel confirmed.** `wl_fixed` motion arrives with live fractional bits (e.g.
>    `95.9375`) — §6.2's payoff is real.
> 4. **⚠ NEW, design-changing: pointer emulation is SUPPRESSED for tablet-aware clients.** The
>    moment the client binds the tablet manager, KWin stops synthesizing `wl_pointer` events
>    for the pen: FLTK received ZERO events (no FL_ENTER/MOVE/PUSH/DRAG/RELEASE) through an
>    entire pen stroke, tip-down included. §3.1's ring-drain-inside-FL_DRAG design is therefore
>    **X11/XI2-only**. The Wayland backend must own the full stroke lifecycle from tool
>    `down`/`up`/`motion` itself and drive the canvas directly. Ordinary mouse input is
>    unaffected (a mouse is native `wl_pointer`, not emulation).
> 5. **Synthetic-harness note:** an in-proximity tool that goes event-silent is forced
>    proximity-out by the input stack after ~50 ms. evdev filters unchanged ABS values, so a
>    perfectly still uinput pen goes silent and hits this; real pens jitter and never do. Test
>    streams must keep moving — or expect, and assert on, the prox-out.

> **Built-note (2026-07-10) — §10 step 4, the backend.** `platform/tablet_wayland.{hpp,cpp}` is
> BUILT and headless-tested (`test_tablet_wayland.cpp`, 22 cases, 9/9 mutants killed); the live
> half is what `tools/tablet_spike_fltk` already proved on KWin 6.7.0. Split like the XI2 backend
> (§9): the bug-prone logic — per-`frame` accumulation, axis normalization, the lifecycle
> transitions — is `WaylandTool` plus the `waylandToolType`/`waylandButtonBit` free functions, PURE
> over plain values (the tests hand-feed them; no `wl_display`), and only `TabletWayland` (registry
> bind + seat/tool proxies) touches the compositor. Decisions settled in code:
>
> 1. **The backend OWNS the lifecycle, so it delivers through a `TabletStrokeSink`, not the ring**
>    (finding 4). `frame()` fires ONE callback — `strokeBegin`/`strokeMotion`/`strokeEnd`/`hover`/
>    `proximityOut`, priority down > up > motion-while-down > proximity-out > hover — carrying a
>    normalized `TabletSample`. `ring()` STAYS EMPTY on Wayland (nothing pushes, so `overwritten()`
>    cannot lie about a drain that does not exist). The samples reach the sink NOT yet run through
>    `TabletPolicy`: the wiring (§10 step 5) applies it at ingest, the same seam the X11 drain uses.
> 2. **Normalization is simpler than X11's** because the compositor pre-normalizes: pressure is
>    `raw/65535` (device-independent — no min/max), missing → 1.0 (§3.2); tilt and rotation already
>    arrive in degrees and pass through unscaled (`tablet.hpp`); the `slider` (`[-65535,65535]`, the
>    airbrush finger wheel) maps two-sidedly to `tangentialPressure` with neutral → 0.5, matching
>    the X11 wheel at its range midpoint, and a missing slider → 0.0 (rest). `distance` and the
>    relative `wheel` have no `TabletSample` field and are dropped.
> 3. **Tool type** comes from the protocol enum (pen/brush/pencil → Pen, eraser → Eraser, airbrush
>    → Airbrush, lens → Puck, mouse/finger → Mouse); the values are duplicated into `tablet_wayland
>    .hpp` (so the classifier is testable without the generated header) and `static_assert`-pinned
>    to `ZWP_TABLET_TOOL_V2_TYPE_*` in the .cpp so the copy cannot drift.
> 4. **The tip rides `buttons` bit 0 off the down/up contact state** — identical to the X11 tip
>    button — and the barrel buttons `BTN_STYLUS`/`_STYLUS2`/`_STYLUS3` map to the X11 button-2/3/4
>    bits, so "the lower barrel button" means the same thing on either backend. Axis values PERSIST
>    across frames (a frame that omits an axis keeps its last value); only the lifecycle transitions
>    are per-frame. `timeUs` is `ingestClockUs()` stamped at `frame`, never the wire time (§5).
> 5. **Build:** `wayland-scanner` generates the client stubs into the module — the private-code
>    `.c` is compiled in, the client header sits on a PRIVATE include path, and the .hpp stays
>    protocol-header-free (forward-declared `wl_display`/`wl_surface` + a pimpl), so the headless
>    test and the FLTK-free wiring TU include it without the generated dir. `tablet_spike_fltk` now
>    links the module's protocol symbols and takes only the client header (no duplicate `.c`).
>
> ⚠ The app wiring is deliberately NOT here (§10 step 5): `Fl::add_system_handler`, the canvas's
> `TabletStrokeSink`, the `{pt, 1.0}` literals in `vulkan_canvas.cpp`, and Settings → Tablet.

---

## 4a. The wiring — **built 2026-07-11** (§10 step 5)

> **Built-note.** `ui/tablet_input.{hpp,cpp}` is the ONE place FLTK meets the platform tablet layer:
> `Fl::add_system_handler` on X11, the FLTK surface the Wayland backend binds to, and the single
> ingest seam where a `TabletSample` becomes a `core::brush::StrokeInput` and `TabletPolicy` applies.
> Above it, the canvas and the engine have never heard of XI2 or `wl_fixed`; below it, the platform
> layer stays FLTK-free. The `{pt, 1.0}` literals in `vulkan_canvas.cpp` are gone — real pressure,
> tilt, rotation and sub-pixel position reach the dab walk, and `BrushEngine` now takes the whole
> `StrokeInput` and drives a `StrokeState` (docs/brushes.md §6.2). Verified live on XWayland /
> KWin 6.7.0 with the uinput virtual stylus; 1274 cases green, 12/12 mutants killed.
>
> 1. **⚠ THE CONTACT SAMPLE ARRIVES AFTER `FL_PUSH`.** Measured, and it changes the design: the X
>    server's **core `ButtonPress` reaches the client ahead of the XI2 events carrying that same
>    contact**, so at `FL_PUSH` the ring is still EMPTY. A press that resolved its sample there gets
>    the synthesized pressure-1.0 fallback — a full-size, full-flow blob at the head of *every*
>    tablet stroke, before it settles to the pressure the nib actually made. Observed verbatim:
>    press → `p=1.0000`, the real contact 20 ms later → `p=0.3137`. So the canvas **defers the first
>    dab** to the first real sample (`m_brushPressPending`); the `FL_DRAG` drain begins the stroke,
>    and a tap that never drags is closed out by `finishBrushStroke`, which drains before it ends. A
>    MOUSE begins on the spot, unchanged — it has no contact sample to wait for, and its first dab
>    must land on the press and not on the drag. After the fix the first dab lands at `p=0.3137`.
>    (This is the X11 analogue of what finding 4 forces on Wayland: the device stream, not the
>    toolkit, knows when the stroke really began.)
> 2. **The discriminator is "is a stylus in proximity", not "is a tablet present".** A mouse and a
>    stylus both run under a live XI2 backend, so deferral keys off a real device sample having
>    landed within the last 150 ms — a hovering pen streams continuously (the stack forces a
>    proximity-out after ~50 ms of silence anyway, §4 finding 5), and a stale reading means the user
>    has reached for the mouse.
> 3. **FLTK reports GUI-SCALED coordinates; both backends report the surface's own.** XI2's
>    `event_x/y` are device pixels and `zwp_tablet_v2`'s motion is surface-local, while FLTK divides
>    by the screen's scale factor before it hands you `Fl::event_x()`. The ingest divides by
>    `Fl::screen_scale` — a no-op at the overwhelmingly common scale of 1, and verified live at
>    `FLTK_SCALING_FACTOR=2`, where the first dab lands at canvas-local `(249.67, 125.45)` against a
>    `(249, 125)` target; an unscaled ingest would have put it at ~`(499, 251)`.
> 4. **The ring is drained or dropped on EVERY canvas event.** XI2 fills it at ~200 Hz whenever the
>    pen is over the canvas — hovering, dragging a lasso, anything — and only a brush press and a
>    brush drag consume it. Everything else calls `discardBuffered()`, which *notes* the newest
>    sample (the device's live state, which the §8 test area and the proximity check both read) and
>    drops the rest. Without that, `SampleRing::overwritten()` would count samples nobody ever wanted
>    instead of the stalls it exists to catch.
> 5. **`ui::TabletStrokeGate`** is the Wayland sink's lifecycle guard, pulled out pure and tested. A
>    `down` only strokes **when the pen is in proximity**: the compositor still delivers down/motion/
>    up for a pen over any *other* surface of this client — the toolbar, a panel, a dialog — and the
>    backend dispatches those to the sink all the same, so without the gate, pressing a toolbar
>    button with the pen would begin a brush stroke on the canvas at the toolbar's coordinates. An
>    `up`, conversely, is gated on the **stroke** and not on proximity: a pen flicked off the tablet
>    delivers `up` and `proximity_out` in ONE frame, so the sample carrying that up already reads out
>    of proximity, and that stroke still has to end.
> 6. **Pressure drives size and flow** for the Brush and the Eraser (`currentBrushDynamics`). Both
>    channels are exact identities at pressure 1, so every mouse stroke stays byte-for-byte what it
>    was — a test pins that, because the goldens and the `Uniform × Wash` rule depend on it. The
>    **Inpaint brush opts out**: its dabs are the hole MASK the solver fills, and a pressure-shrunk
>    dab would mark a smaller region than the reticle promises. Arc D's per-preset dynamics supersede
>    this pair.
> 7. **Synthesized samples skip the policy.** With no stylus in proximity the drain synthesizes one
>    sample at pressure 1 (§3.2) — deliberately NOT through `TabletPolicy`, because a mouse has no
>    device pressure for a pressure curve to reshape, and a curve mapping 1.0 → 0.8 would otherwise
>    quietly weaken every mouse stroke in the program.
>
> ⚠ **Was a known limitation; now FIXED — `ui::TabletPointerSynth`, 2026-07-11.** With the tablet
> manager bound, KWin stops emulating pointer events for the pen (finding 4). The first cut answered
> that by letting the *backend* own the stroke lifecycle and drive the canvas directly — which worked,
> and meant that on `FLTK_BACKEND=wayland` **the pen drove the canvas stroke and nothing else**: it
> could not press a toolbar button, open a menu, reach a dialog, or even move the reticle on hover
> (nothing was generating `FL_MOVE`). The fix is the obvious one and it is now built: **synthesize the
> pointer events.** Each tool frame pushes its sample into the ring and then makes the `Fl::handle()`
> call the compositor declined to make (`Fl::e_*` and `Fl::handle(int, Fl_Window*)` are public, and
> FLTK's own platform drivers do precisely this). The pen is an ordinary pointer that happens to carry
> pressure; every tool works under it for the same reason every tool works under a mouse — **it is the
> same code**. `StrokeHost` and the canvas's five sink entry points are gone, and both platforms now
> run ONE lifecycle: the ring is the sample path, FLTK routes, the tool drains.
>
> Proven live (KWin 6.7.0, uinput virtual stylus, 2026-07-11): a hover raster produced `FL_MOVE`
> landing on a widget, and a tap produced `FL_PUSH` → 3× `FL_DRAG` → `FL_RELEASE`. ⚠ The canvas is not
> shown in the empty state — **open a document before driving a live pen at it**, or the pen has
> nothing of ours to be over.
>
> **The tool's CURSOR is ours too, and for the same reason.** A client that binds the tablet manager
> owns its tool cursor; one that never sets it shows the compositor's default — on KWin a **crosshair,
> over every pixel of the app**. FLTK cannot help: its cursor calls go through `wl_pointer.set_cursor`,
> and a tablet tool *has* no `wl_pointer`. So the backend binds `wp_cursor_shape_manager_v1`
> (wayland-protocols staging) and names a shape per tool, re-applying on every `proximity_in` (the
> serial is only valid against that event, and the compositor forgets the cursor on the way out).
> `zwp_tablet_tool_v2.set_cursor` with a **null surface hides it**, which is what the canvas asks for
> under a brush — the GPU reticle ring IS the cursor — and is the honest fallback when the compositor
> has no cursor-shape protocol at all: no cursor beats the wrong cursor.

---

## 5. Windows — designed here, **built 2026-07-30** (S57; see §5a)

This is the mess. It is not solvable, only managed, and the reason is structural: Windows has no
vendor-neutral tablet stack in the kernel. Two APIs, both partial:

**WinTab** — the ~1991 Wacom-authored de-facto standard. Not shipped by Windows; provided by the
*vendor driver* as `wintab32.dll`. Load it dynamically (never link) and resolve `WTInfoA`, `WTOpenA`,
`WTClose`, `WTPacketsGet`, `WTPacket`, `WTEnable`, `WTOverlap`. Build a `LOGCONTEXT` via
`WTInfoA(WTI_DEFSYSCTX, 0, &lc)`, set `lcPktData` / `lcOptions |= CXO_MESSAGES`, map `lcInOrgX/Y` and
`lcInExtX/Y` to the virtual desktop, and normalize `pkNormalPressure` by the axis maximum read from
`WTInfoA(WTI_DEVICES + i, DVC_NPRESSURE, &axis)`. Rich data (full pressure resolution, tilt, rotation,
serials). Absent or broken on many non-Wacom devices.

**Windows Ink / Pointer API** — `WM_POINTER` + `GetPointerPenInfo`. Shipped by the OS, works
everywhere, and is the only option on many modern devices. But: coarser pressure on some drivers, and
**barrel buttons are frequently not reported through it at all**.

**Design:**
- Both backends implemented; a user-visible switch selects the API (Settings → Tablet, §8), Windows
  only.
- **Auto-fallback:** if the user asks for WinTab and the context never opens (no `wintab32.dll`, or
  `WTOpenA` fails), fall back to Ink, log it, and *automatically enable* the barrel-button workaround.
  Do not silently paint with a dead tablet.
- **Barrel-button-as-mouse workaround:** read right/middle clicks from the ordinary mouse stream when
  the tablet API does not surface them.
- **Driver ExpressKey suppression:** many drivers emit `F13`–`F35` for their hardware buttons. Swallow
  that range by default.
- **Desktop-rect override:** some drivers map the stylus to the wrong monitor or an incorrect
  rectangle. Offer an explicit "custom tablet resolution" escape hatch (§8) that overrides the
  driver's `LOGCONTEXT` mapping with a user-supplied rect, or ignores it in favour of the virtual
  screen.
- **Event ordering:** in WinTab mode, tablet packets arrive asynchronously with respect to the mouse
  message queue. A tablet-down can precede the window's focus/enter events. Do not assume a stroke
  begins only after focus; and do not re-enable mouse events while a stroke is in flight.
- **Timestamps:** driver clocks are unreliable (some report milliseconds since system boot). Stamp
  `timeUs` from our own monotonic clock at ingest, on every platform, for exactly this reason.

We do **not** ship a driver. A universal Windows tablet driver — even one wrapping the Linux stack —
is far outside this project's scope, and the two-backend + workaround design is the achievable answer.

---

## 5a. The Windows backend — **built 2026-07-30** (S57)

`platform/tablet_win32.{hpp,cpp}` is the device layer and the `_WIN32` branch of
`ui/tablet_input.cpp` is the FLTK half — the S57 siblings of `tablet_x11.*` + the §4a wiring. Both
device paths from §5 are implemented behind one `TabletBackend`, and the selection happens once, at
`init()`.

> ⚠ **UNTESTED ON REAL HARDWARE, and read that literally.** There is no Windows machine in this
> project and no pen to hold. What has been verified is *compilation and linkage* under
> `x86_64-w64-mingw32-g++` and llvm-mingw clang at `-Wall -Wextra -Wpedantic -Werror`, plus the fact
> that the linked binary imports **nothing** from `Wintab32.dll` (so it starts on a machine with no
> tablet driver). Everything below marked ⚠ is a decision taken from the published API contract and
> from how other implementations of the same ABI behave; the user's interactive pass is what turns
> each into a fact. The specific things most likely to be wrong are collected at the end.

### 5a.1 The two paths, and which one wins

| | WinTab (`Wintab32.dll`) | Windows Ink (Pointer Input Stack) |
|---|---|---|
| shipped by | the **vendor driver** — absent with no driver | the **OS** — always present on our floor |
| pressure | the device's declared axis, full resolution | `POINTER_PEN_INFO::pressure`, 0…1024 |
| tilt | `ORIENTATION` azimuth + altitude | `tiltX` / `tiltY`, already degrees |
| barrel rotation | `ORIENTATION::orTwist` (the art pen) | `rotation`, 0…359 |
| airbrush wheel | `DVC_TPRESSURE` | **nothing** — the API has no such axis |
| barrel buttons | the physical mask, `pkButtons` | ⚠ frequently not reported at all |
| tool identity | per-cursor, plus a per-packet "inverted" bit | inverted / eraser flags only |

**Policy: WinTab first, Windows Ink as the fallback.** WinTab is tried when its DLL loads, the
driver answers "a tablet is attached", and a context opens; anything short of that falls through to
Ink, including an explicit `MOSAIC_TABLET_API=wintab` that did not come up — which is *logged*,
because silently painting with a dead tablet is the one outcome worth ruling out (§5).

Three things decided it, and none of them is a preference:

1. **The two axes that only WinTab has** are the two this program cares about beyond pressure:
   barrel rotation and the airbrush finger wheel. `docs/brushes.md`'s dynamics read both, and the
   Pointer Input Stack has no field for the wheel at all — a value we could only invent.
2. **The absence of `Wintab32.dll` is itself the correct signal.** A machine whose pen is a Surface /
   Elan / Synaptics digitizer has no WinTab driver, and that is exactly the machine where Ink is the
   only path. So "try WinTab, fall back" is not a preference ordering; it is a detection mechanism
   that happens to also be the preference ordering.
3. **The same ordering is what Krita ships**, defaulting to WinTab with an automatic switch to Ink
   when WinTab fails to activate — and, on that switch, automatically turning on its
   read-right-and-middle-clicks-from-the-mouse workaround. That is a decade of real-driver exposure
   pointing the same way, which is the strongest evidence available without hardware.

A machine with neither degrades to plain mouse input at `pressure = 1.0`, exactly as the Linux
backends do with no tablet present (§3.2). Note that the Ink path reports itself *available* even
when no pen exists — same as a live XI2 backend with zero tablets: it is listening, the ring stays
empty, and the canvas synthesizes.

### 5a.2 It is the X11 design, not the Wayland one

Windows keeps promoting pen input into the legacy mouse stream for compatibility, so FLTK's
`FL_PUSH`/`FL_DRAG`/`FL_MOVE`/`FL_RELEASE` arrives by itself and the backend only has to supply the
valuators: **fill the ring, let the canvas drain it** (§3.1). There is therefore no
`TabletPointerSynth` here and no tool-cursor work — the pen *is* the system pointer, and the cursor
FLTK sets through the HWND is the one it shows. That is the same shape macOS ended up with (§6), and
the opposite of Wayland, where binding the tablet manager takes the pointer stream away (§4,
finding 4).

⚠ If the interactive pass finds that FLTK receives *no* mouse events during a pen stroke, that
conclusion is wrong for this machine, and the fix is not a new design: reuse `ui::TabletPointerSynth`
exactly as the Wayland sink does. The ring is already the single sample path on all three platforms,
so it is a change to who calls `Fl::handle`, nothing more.

### 5a.3 The FLTK seam — `Fl::add_system_handler` is enough

**No HWND subclassing, and none needed.** FLTK's Windows event loop calls `PeekMessageW` with a null
window (so it retrieves messages for *every* window of the thread), hands each raw `MSG` to the
system handlers, and only then runs `TranslateMessage`/`DispatchMessageW` (`Fl_win32.cxx`). That is
the exact counterpart of the X11 `XEvent` tap, and it reaches both `WT_PACKET` and `WM_POINTER*`.
A nonzero return swallows the message.

Two consequences worth writing down:

- **WinTab's context hangs off a message-only window of our own** (`HWND_MESSAGE`), not off an app
  window. A context belongs to a *window*, and hanging ours off the main window would tie the packet
  stream to that window's activation — while Settings → Tablet's test area has to read the pen with
  a **dialog in front** (§8). A window that is never activated has no such state to get wrong, and
  the context is a **system** context (`WTI_DEFSYSCTX`), which tracks the pen across the desktop
  rather than following one window's focus. Its messages still reach us, because `PeekMessageW(NULL,
  …)` is thread-wide.
- **Neither path needs per-window registration.** Both report the pen across the whole desktop, and
  every sample records which window of ours it landed on — so `TabletInput::watch()` is not what
  makes the test area work here (it works already). What `watch()` does on Windows is switch off
  Windows' own pen gesture **visuals** on that window.

### 5a.4 Where each value is normalized

Everything below happens in `platform/tablet_win32.cpp`, and every pure function is separable from
the driver so a headless test can hand-build a packet (§9).

**WinTab:**

- **pressure** — `(pkNormalPressure − axMin) / (axMax − axMin)` from `DVC_NPRESSURE`, clamped.
  A missing axis, or a degenerate declared range, reports **1.0 and never 0**, the same §3.2 rule
  the XI2 backend applies with the same `remap01` shape: two backends normalizing pressure
  differently would make one nib feel like two.
- **tangential pressure** — the same two-sided remap of `DVC_TPRESSURE`; absent → `0.0` (rest).
  Unlike X11, this is a *separate axis* rather than a wheel multiplexed with rotation, so it needs
  no tool-type disambiguation.
- **tilt — passed through UNSCALED, in real degrees.** This is the decision `tablet.hpp` asks for,
  and WinTab lands on the "already speaks degrees" side: the `ORIENTATION` axes are **angular**, and
  each declares `axUnits = TU_CIRCLE` with `axResolution` giving **units per full circle** (Wacom
  declares 3600, i.e. tenths of a degree). So the units are known and convertible — the backend
  reads degrees-per-unit off that declaration, converts, and does **not** multiply by
  `kTiltFullScaleDegrees`. A resolution of zero is how a driver says "no tilt"; a driver that claims
  TU_CIRCLE with an unusable resolution falls back to the tenth-of-a-degree convention.
  Azimuth+altitude then become the x/y pair the model carries:
  `xTilt = atan(sin azimuth / tan|altitude|)`, `yTilt = −atan(cos azimuth / tan|altitude|)`. An
  upright pen (altitude 90°) comes out exactly `(0, 0)`; a pen flat on the pad comes out at ±90°,
  which the sensor layer saturates against its own full scale rather than rescaling. `|altitude|`
  because a **negative** altitude means the stylus is inverted, which says nothing about lean.
- **rotation** — `360 − twist`, wrapped into `[−180, 180]`: the twist axis counts the opposite way
  round from the model's field.
- **buttons** — `pkButtons` **is** the state mask, because the context is opened in absolute mode
  (`lcPktMode = 0`). Bit 0 is the tip, so "bit 0 = button 1 = tip" now holds on all four backends
  and a brush preset can name a barrel button once.
- **tool** — the cursor's name first, lowercased and by *contains* ("eraser" / "airbrush" /
  "puck"|"cursor"|"lens"), which is deliberately the identical classifier the XI2 backend runs over
  device names: "the eraser end" has to mean the same thing on both platforms. `CSR_TYPE`'s vendor
  bitfield is the fallback. ⚠ And `TPS_INVERT` **overrides both** — it is the driver saying, per
  packet, that the stylus is upside down, which beats any inference from a name.
- **position — sub-pixel, and this is the one non-obvious bit.** Left to its defaults the driver maps
  the tablet onto whole **screen pixels** and `pkX`/`pkY` arrive as integers: the fidelity is gone
  before we ever see it, and the stroke becomes the staircase §3.5 spent a release fixing on X11. So
  the context is opened with an **identity output mapping** (`lcOutOrg/lcOutExt = lcInOrg/lcInExt`),
  packets arrive in the context's own device units — tens of times finer than a pixel — and the map
  onto the desktop is done here in `double`.
- **`lcMsgBase` is read back from the opened context**, never assumed to be `WT_DEFBASE`: the message
  base is the driver's to choose, and a moved one would deliver packets we silently ignored.

**Windows Ink:**

- **pressure** — `pressure / 1024`, clamped; `PEN_MASK_PRESSURE` clear (or a zero reading) → `1.0`
  (§3.2).
- **tilt** — `tiltX`/`tiltY` are documented as **degrees** in `[−90, 90]` and pass through unscaled,
  the same side of `tablet.hpp`'s rule as `zwp_tablet_v2` and `NSEvent`.
- **rotation** — `rotation` is degrees in `[0, 359]`, wrapped into `[−180, 180]`.
- **tangential pressure** — `0.0`. The API has no airbrush wheel; inventing one would be a lie the
  airbrush reads as a real finger position.
- **`toolSerial`** — `0`. There is no per-stylus serial here. `sourceDevice` identifies the
  *digitizer*, and reporting it as a tool serial would make two different pens look like one.
- **position — sub-pixel from HIMETRIC.** `ptPixelLocation` is rounded to whole pixels;
  `ptHimetricLocationRaw` is the digitizer's own 0.01 mm grid, and `GetPointerDeviceRects` gives the
  two rectangles that turn one into the other. Cached per digitizer; if the call fails we fall back
  to the integer pixel location — a stroke still paints, merely quantized.
- **the coalesced history IS the fidelity.** Windows folds a fast pen into **one**
  `WM_POINTERUPDATE` carrying a `historyCount` of the samples it stands for, and taking only the
  newest turns a 200 Hz stroke back into a 60 Hz polygon — the same defect §3.1 payoff 2 exists to
  avoid. `GetPointerPenInfoHistory` returns them **newest first**, so they are pushed in reverse to
  keep the ring chronological. A count beyond a sane ceiling is rejected rather than truncated,
  because that call refuses a buffer smaller than the count it reported.

**Both paths — desktop coordinates become window-local:**

A WinTab packet says where the pen is *on the desktop*, with no notion of which window that is. The
target is resolved as: the pointer message's own `hwndTarget` if it is ours → **`GetCapture()`** if
it is ours → `WindowFromPoint`. Then `ClientToScreen` gives that window's client origin, and the
subtraction stays in `double` so the sub-pixel part survives. `TabletSample::surface` is the HWND.

⚠ **`GetCapture()` is not a nicety, it is the fix for dragging off the canvas.** A brush stroke
dragged past the canvas edge is ordinary use, and while it is in flight FLTK holds the mouse capture
(`Fl_win32.cxx` calls `SetCapture` on the push, on the *mouse window* — which for a click on the
canvas is the canvas subwindow's own HWND). Asking `WindowFromPoint` at that moment answers with
whatever is physically under the pen — another app, the desktop — the sample would be dropped as
"not ours", and the drain would synthesize a pressure-1.0 fallback in the middle of a stroke. That
would have been invisible in a screenshot and obvious to a hand.

A sample over **another application** is dropped outright rather than ringed: `pos` would be
meaningless, and a readout claiming a live stylus while the user is in another program is worse than
no readout.

### 5a.5 The driver quirks that were carried over

- **ExpressKey suppression** (§5). Many drivers emit F13–F35 for their hardware buttons. `WM_KEYDOWN`
  / `WM_KEYUP` / their `SYS` forms are swallowed for `VK_F13`…`VK_F24` (Windows has no virtual-key
  code above F24; the F35 in §5 is the X11 keysym range). This costs **nothing**: `ui/keymap.cpp`
  only ever produces F1–F12, so no binding can exist in that range to lose. ⚠ Keys only — nothing
  about a pen gesture is ever made to depend on the key stream, which is the standing Wayland lesson
  and holds on every platform.
- **Barrel-button-as-mouse** (§5) is **free on this architecture**, which is worth stating rather
  than leaving as an absence. Nothing is ever swallowed except the ExpressKey range, so the driver's
  own right/middle mouse emulation reaches FLTK as ordinary `FL_PUSH` with `Fl::event_button()`
  already correct — the same code path a mouse takes. There is no workaround to enable because there
  was never anything to break.
- **Pen gesture visuals are switched off** per window (`SetWindowFeedbackSetting`): the
  press-and-hold ring, the tap and right-tap ripples, the barrel-button splash. In a paint program
  every one of those draws itself over the artwork under the nib. Applied to the canvas window at
  `init()` and to each `watch()`ed window, and applied **even when no tablet backend comes up**,
  because a Surface pen driving the plain mouse path still triggers them.
- **The packet queue is deepened** (`WTQueueSizeSet`, best-effort). The default is a handful of
  packets; a 200 Hz pen overruns it between two frames and the driver drops the middle of the
  stroke. `WTPacketsGet` then lifts the whole queue per `WT_PACKET`, looping until it is dry.
- **`WT_INFOCHANGE` re-enumerates** — the WinTab analogue of XI2's `XI_HierarchyChanged`. Axis ranges
  and the cursor cache are rebuilt; the context survives, so the packet stream is not interrupted.
- **Desktop-rect override** (§5), as `MOSAIC_TABLET_MAPPING` = `driver` (the default: honour what the
  user configured in the tablet control panel) | `screen` (ignore it, use the virtual desktop) |
  `x,y,w,h`. A driver that declares a degenerate screen rect is silently promoted to `screen`,
  because pinning every sample to one point is not a thing to respect.
- **API override**, as `MOSAIC_TABLET_API` = `wintab` | `ink` | `auto`.

  Environment variables and not settings fields, deliberately: the situation both exist for is a
  machine whose driver is *present but wrong*, where the answer has to change before the program is
  usable enough to reach a dialog — the same reason `FLTK_BACKEND=x11` is an environment variable.
  Settings → Tablet's API selector (§8) supersedes the first of them when it lands, and the rect
  parse is `strtol` rather than `strtod` on purpose: a decimal separator means different things in
  different locales and this string is read after the locale is installed (`docs/i18n.md`).
- **Event ordering** (§5) needs nothing. Nothing here waits on focus, and nothing re-enables an
  input stream mid-stroke; the ring is filled from whatever arrives, whenever it arrives.
- **Timestamps** are ours (§5). `PK_TIME` is not even requested from the driver.
- **Proximity is NOT latched from the driver**, and this is a decision, not an omission. Every WinTab
  packet already carries `TPS_PROXIMITY` and every pointer sample carries `POINTER_FLAG_INRANGE`, so
  a sample stream cannot get stuck. A *bool* can: a missed proximity-out would leave it saying a
  stylus is on the tablet forever — and that flag is what a press branches on to defer its first dab
  (§4a finding 1), so a **mouse** press would stop painting until it was dragged. Windows therefore
  answers `stylusInProximity()` from sample age alone, exactly as Linux does.

### 5a.6 What was deliberately not done

- **`EnableMouseInPointer` is never called.** It would convert the *mouse* into pointer messages too,
  taking FLTK's `WM_MOUSEMOVE` stream away and breaking every non-pen interaction in the program.
  Pen input already generates `WM_POINTER*` without it.
- **Pen press-and-hold (the gesture, not its visual) is not disabled**, and it cannot be from where
  we stand: that needs `WM_TABLET_QUERYSYSTEMGESTURESTATUS` answered with
  `TABLET_DISABLE_PRESSANDHOLD`, and that message is **sent, not posted** — it never reaches a
  `PeekMessage`-based system handler at all, let alone one that can only swallow a message rather
  than return a value for it. ⚠ Doing it would take a `WndProc` of our own (HWND subclassing), which
  is invasive enough to be a separate, deliberate decision. If the interactive pass shows a lag
  before the first dab that the deferred-first-dab machinery does not explain, this is the suspect.
- **No settings persistence and no Settings → Tablet API selector.** §8's row stays unbuilt; the two
  environment overrides above are the interim.
- **The `WTI_DEFCONTEXT` digitizing context, `WTOverlap` bookkeeping on activation, and the
  `WTEnable` dance** are not used. A system context sidesteps the whole activation question, which is
  the reason it was chosen.

### 5a.7 ⚠ What is unverified, in the order it is likely to bite

1. **Tilt sign conventions.** Whether `xTilt`/`yTilt` come out with the same sign as the XI2 and
   `zwp_tablet_v2` backends for the same physical lean. Both the azimuth→x/y conversion and the
   negation on `yTilt` are taken from the published relationship, not measured. A stroke whose
   tilt-driven dab leans the wrong way is the symptom.
2. **The WinTab Y flip.** Input y grows away from the user, screen y grows down, so
   `winTabScreenPos` flips unconditionally. If it is wrong, the pen paints mirrored vertically —
   unmissable, and a one-line fix.
3. **Rotation's zero point and direction.** `360 − twist` places the art pen's neutral somewhere; the
   only way to know it is the *same* somewhere as the Linux backends is to turn a real art pen.
4. **Pressure curve shape.** WinTab pressure is normalized against the driver's declared range, which
   is not the same thing as the range a nib actually reaches — that is exactly what §7's pressure
   range clamp is for, and it may need a different default here than on Linux.
5. **Whether the mouse promotion happens at all** (§5a.2), and whether the first promoted
   `WM_LBUTTONDOWN` lags the first `WM_POINTERDOWN` enough to be felt.
6. **The message-only window.** Whether every driver delivers packets to a `HWND_MESSAGE` context
   window. If one does not, the fallback is a hidden `WS_POPUP` top-level, which is a two-line change
   in `createPacketWindow`.
7. **HiDPI.** `guiScale()` divides by `Fl::screen_scale`, which on Windows is a *real* fractional
   factor at the ubiquitous 150% setting rather than the 1 it almost always is on Linux. This is the
   one platform where that divide is load-bearing at default settings, and it has never been
   exercised there.
8. **The packet struct layout.** A `static_assert` pins it at 10 four-byte fields with no padding,
   which is why `PK_CONTEXT` (the one pointer-sized field) is not requested — but "the driver writes
   exactly the fields in `lcPktData`, in bit order" is an ABI contract, not something a cross-build
   can check.
9. **Whether a merely HOVERING pen produces samples at all**, on either path. That is what makes the
   §8 test area able to answer "is my tablet working" without painting, and what
   `stylusInProximity()` reads to decide how a press begins. WinTab reports a hovering cursor by
   design; `WM_POINTERUPDATE` is documented to cover hover as well as contact. If one of them turns
   out not to, the test area degrades to "works while the nib is down", which is a much weaker
   promise than the Linux one.

---

## 6. macOS — **S58**, designed here

`NSEvent` carries `pressure`, `tilt` (an `NSPoint` of normalized ±1), `rotation`,
`tangentialPressure`, `deviceID`, `pointingDeviceType` (pen/cursor/eraser) and `uniqueID`, delivered
through `NSEventTypeTabletPoint` and the tablet-proximity events. Read them from the `NSView`'s event
stream and normalize into `TabletSample`.

One known quirk to design for: on macOS, tablet *move* events are not generated until after a tablet
*press*, so hovering must continue to be tracked through ordinary mouse-moved events while the stylus
is in proximity but not down.

---

## 7. Policy layer (shared, `core/brush/`)

Platform-independent, FLTK-free, headless-tested with canned sample streams.

- **Pressure response curve** — a global `Curve` (the same type the brush dynamics use), baked to a
  256-entry LUT and applied to raw pressure at ingest. Serialized as `"x,y;x,y;"`, so it shares the
  editor widget from `docs/brushes.md` §8.3.
- **Pressure range clamp** — `min`/`max` raw pressure mapped to the full [0,1] output. Worn nibs and
  cheap digitizers never reach 0 or 1; without this they are unusable.
- **Tilt-direction offset** — a signed angle added to the derived `ascension`, for users who hold the
  pen rotated.
- **`SpeedSmoother`** — exponential moving average over a configurable window, producing the `speed`
  sensor. Two knobs: the speed that maps to 1.0, and the window length.
- **Stabilizer** — `None | Weighted | Stabilizer`:
  - *Weighted*: Gaussian-weighted average over the recent point history, σ derived from a distance
    parameter.
  - *Stabilizer*: a lagging sample deque; the painted point trails the cursor, with an optional
    catch-up drain on pen-up so the stroke ends where the pen did.

> ⚠ **WEIGHTED smoothing is what ships**: a **fixed-N** weighted average of the recent points, drawn
> through the averaged points. ⚠⚠ **THE WINDOW MUST NEVER ADAPT TO SPEED.** "Shrink the window when
> the pen moves fast so it feels responsive" is the obvious improvement, and it is exactly the thing
> this design refuses. The window is fixed. Do not make it velocity-adaptive.
>
> ⚠ **The rope / pulled-string STABILIZER is NOT BUILT**, and it is sequenced as its own slice rather
> than folded in here. The plain weighted-average and lagging-deque forms are long-standing practice;
> what is held back is the specific rope/anchor formulation, which is its own design surface.
>
> ⚠ **Do not confuse that with path INTERPOLATION**, which is a different mechanic entirely: a curve
> fitted *through the user's own samples* has no anchor, no lag and no filter — it decides where dabs
> land *between* the user's points, never *what those points should have been*. Catmull & Rom 1974 /
> Schneider 1990. The rule that keeps them apart: **interpolate; do not filter.**

---

## 8. Settings → Tablet

A new left-rail category. Note the rail is positional — `names[]` plus `kNavCount` in
`settings_dialog.cpp`, with panes pushed in parallel order and no enum — so the pane must be inserted
at the matching index.

| Control | Notes |
|---|---|
| Detected devices | name, bus, and which valuators were found. Diagnostic, not configurable. |
| Pressure curve | the `Curve` editor widget; identity by default |
| Pressure range | min/max raw clamp |
| Tilt direction offset | signed angle |
| Stabilizer mode + parameters | shared with the brush editor's per-preset override |
| Speed smoothing | max speed, window length |
| **Test area** | a scratch surface showing live raw pressure/tilt/rotation and the resolved sample rate. The single most useful control on the page: it answers "is my tablet working" without the user having to paint. |
| Event logging | writes raw packets to the log for bug reports |
| **API selector** | **Windows only** — WinTab / Windows Ink, plus the custom-resolution escape hatch (§5). Hidden elsewhere. |

Persistence follows the `common::Settings` pattern: plain fields, JSON round-trip in `settings.cpp`,
one `SettingsHost` callback each.

> **Built-note (2026-07-11).** The pane ships with the device list, the **test area**, the pressure
> curve, the pressure range, the tilt offset and speed smoothing. Two rows above are deliberately
> **not** there: the **stabilizer** (§7 — it is not built, and a control that does nothing is worse
> than no control) and the **API selector** (Windows only, S57). Event logging
> folds into the existing `--log-level debug` stream rather than a toggle of its own.
>
> **Still true after S57.** The Windows backend is built (§5a) and the API selector row is *still*
> not: the two overrides it would drive are environment variables (`MOSAIC_TABLET_API`,
> `MOSAIC_TABLET_MAPPING`, §5a.5) for a reason that is not laziness — the machine they exist for is
> one whose tablet driver is present but wrong, where the answer has to change *before* the program is
> usable enough to open a dialog. The pane's row is worth adding once the backend has been exercised
> on real hardware and we know which of the two overrides anyone actually reaches for. The **device
> list and the test area work on Windows today**, both device paths, with no per-window registration
> needed (§5a.3).
>
> - **The rail is positional and now has a test.** `names[]` + `kNavCount` in `settings_dialog.cpp`,
>   panes pushed in parallel, no enum — so Tablet sits at index 3 (after Tools) and the readout timer
>   keys off the same `kTabletSection`. Inserting a category above it silently shifts every pane below
>   by one, and the failure reads as "the Tablet page shows Color Management", which nobody catches in
>   a diff. `test_settings.cpp` selects the section by that index and requires the pane holding the
>   curve editor to become visible; a forged one-off drift of the constant kills it.
> - **The test area reads POST-policy values** — what it shows is what the brush engine is about to
>   get, so a curve that flattens the top of the stroke, or a range the nib never reaches, is visible
>   right there. It polls at 20 Hz, and *only* while it is the page on screen. It works while the pen
>   merely HOVERS, which is what lets it answer "is my tablet working" without painting anything: the
>   ring drop path notes the newest sample rather than discarding it silently (§4a finding 4).
> - **The pressure curve is the §8.3 widget, built here** (`ui/curve_editor.{hpp,cpp}`), not a
>   stopgap — the user chose to build it now, and Arc D's brush editor takes it as-is. It edits a
>   `core::brush::Curve` directly, so an imported preset's curve survives a visit to the editor
>   byte-exactly. Drag a point; click to add; right-click to remove (never an endpoint — the domain
>   must stay [0,1]); double-click to toggle a corner. Endpoints are pinned in x. A dragged point is
>   clamped strictly between its neighbours, because `Curve` DROPS a point that shares an x with an
>   earlier one and silently deleting the point under the cursor is nobody's idea of direct
>   manipulation. Tested headlessly by driving `handle()` through `Fl_Widget` with `Fl::e_x`/`e_y`
>   set — an `Fl_Widget` needs no display to exist or to take an event, only to draw.
> - **Every value is handed to the policy as the user set it**, inverted pressure ranges and all:
>   `TabletPolicy` clamps and swaps, an equal pair is a threshold rather than a division by a zero
>   span, an unparseable curve string yields the identity, and `SpeedSmoother` guards a non-positive
>   window. The ingest path is the validator; the dialog does not get a second opinion, and neither
>   does the JSON loader.

---

## 9. Test posture

The platform layer is a source of `TabletSample`s, so:

- **Backend-free tests** feed canned streams into the policy layer: pressure-curve LUT correctness,
  clamp behaviour at the extremes, `SpeedSmoother` convergence, stabilizer lag and catch-up.
- **Sensor derivation** (`drawingangle`, `distance`, `fade`, `speed`) is pure and golden-tested against
  a fixed stroke.
- **The X11 backend** is exercised by a headless test that constructs `XIDeviceEvent` structures
  directly and pushes them through the parse/normalize path — no X server needed.
- **The ring drain** is tested for ordering and for the no-proximity fallback.

Per house rules, Claude verifies headlessly (build preset + `ctest` + `mosaic --gui-frames N`); the
actual feel of a pen on glass is the user's pass.

---

## 10. Sequence (Arc C of `docs/brushes.md` §10)

1. This note.
2. `platform/tablet.{hpp,cpp}` — the event model, backend interface, and the policy layer with
   tests. **DONE 2026-07-10** (§3.2 built-note; the stabilizer stays unbuilt, §7).
3. X11 / XInput2 backend + the sample ring + canvas drain. **Backend + ring DONE 2026-07-10**,
   headless-tested and proven live by `tools/tablet_spike_x11` (§3.2 built-note); the canvas
   drain deliberately waits for step 5, when samples actually reach `StrokeInput`.
4. **Wayland spike** (§4) — **DONE 2026-07-10**, all unknowns retired (§4 built-note); the
   `zwp_tablet_v2` **backend is BUILT 2026-07-10** (§4 backend built-note): `platform/
   tablet_wayland.{hpp,cpp}`, owning the stroke lifecycle via `TabletStrokeSink` (finding 4),
   headless-tested + proven live by `tablet_spike_fltk`.
5. Wire `TabletSample` into `BrushDynamics` / the sensor evaluation from `docs/brushes.md` §6.2 —
   including the FL_DRAG ring drain on X11 and the policy layer at ingest. **DONE 2026-07-11**
   (§4a built-note): `ui/tablet_input.{hpp,cpp}`, the deferred first dab (the contact sample
   arrives after `FL_PUSH`), `BrushEngine` taking a whole `StrokeInput` and driving a
   `StrokeState`, and pressure finally driving size + flow. Proven live with the uinput stylus.
6. Smoothing and stabilizer (§7). **Speed smoothing is wired** (the §7 `SpeedSmoother`, calibrated
   from Settings → Tablet, driven by the engine's `StrokeState`); the **rope/pulled-string
   stabilizer is still unbuilt** and is its own slice.
7. Settings → Tablet. **DONE 2026-07-11** (§8 built-note), including the §8.3 curve editor
   (`ui/curve_editor.{hpp,cpp}`), which Arc D's brush editor now inherits rather than builds.

**Arc C is complete but for the stabilizer**, and the pen paints end to end on the
shipped (XWayland) session. What is owed is a human: the user's pen-feel pass on real hardware,
batched with the eraser visual pass (`docs/brushes.md` §8.4).

8. **Windows (§5) — DONE 2026-07-30** (§5a built-note): `platform/tablet_win32.{hpp,cpp}` and the
   `_WIN32` branch of `ui/tablet_input.cpp`, carrying both WinTab and the Pointer Input Stack behind
   the same `TabletBackend`, the same sample ring and the same policy seam. Compile- and
   link-verified only; ⚠ **nothing about it has touched a pen** (§5a.7).
9. **macOS (§6) — DONE** at S58: `ui/tablet_input_macos.mm`, the NSEvent local monitor.

All four backends now implement the same interface, and the canvas has never heard of any of them.
