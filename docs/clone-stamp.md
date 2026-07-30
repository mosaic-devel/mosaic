# The Stamp / Clone tool (S38)

*Built 2026-07-29. `ToolId::CloneStamp`, shortcut **S**, its own toolbar slot in the `PaintFill`
group.*

---

## 0. The ruling this tool was built under

PLAN §9 originally called this slot **"Heal tool (+ spot/blemish mode)"** and described an
anchor that *follows the cursor during the drag and snaps back on mouse-up*. Two user rulings
replaced that, and both are load-bearing:

1. **2026-06-19 — it is a STAMP/CLONE tool, not "heal."** *"Select a source region (Ctrl) and stamp
   it over the target, for when you want to heal a brushed selection without inpainting (which is
   S39's job)."*
2. **2026-07-29 — there is NO spot/blemish mode.** The PLAN's "a single click auto-heals a blemish
   from surrounding pixels" was a mistake in the PLAN. It is not built, and PLAN §9/§10 were
   corrected to say so.

The old heal choreography went with the old name. An anchor that walks with the cursor and snaps
back is how a *healing* brush picks a moving donor region; a clone stamp's source is a **point the
user picked**, and it moves with the brush by a **fixed offset**, which is a different mechanism
with a different affordance.

---

## 1. The model, in one paragraph

**A clone stamp copies pixels.** Ctrl-click (⌘-click on macOS) picks a **source point** in document
space. Painting then stamps the source region onto the target through the brush tip: for a target
pixel `p`, the tool reads the source at `p − offset` and composites it over the destination with
the stroke's own alpha. Nothing else happens. There is no seam blending, no gradient-domain step,
no texture synthesis, and no statistic of the destination anywhere in the path — see §7.

---

## 2. Where the code lives

| Piece | Lives in |
| --- | --- |
| The offset model, the sampler, the deposit | `src/core/clone_stamp.{hpp,cpp}` (`mosaic::core`) |
| Options→values, the source marker's geometry | `src/ui/clone_stamp_gesture.{hpp,cpp}` (FLTK-free) |
| Tool registration + the option set | `src/ui/tool.{hpp,cpp}` |
| Icon key | `src/ui/icon_pack.cpp` (`iconKeyFor`) |
| The gesture: anchor, stroke, marker, cursor | `src/ui/vulkan_canvas.{hpp,cpp}` |
| The sample-source snapshot (host side) | `src/ui/app_window.cpp` (`cloneSampleSnapshot`) |
| Tests | `tests/test_clone_stamp.cpp` |

The split is the one every tool since S26 uses: pure maths in `core`, pure gesture maths in a
`*_gesture` unit, pointer handling in the canvas, and everything that needs the document or the
compositor in the host.

---

## 3. The offset, and what "aligned" means

`core::CloneAnchorState` holds the picked source and, in aligned mode, the latched offset. The
whole model is `core::cloneStrokeOffset`:

* **Ctrl-click sets the anchor** (`setCloneAnchor`), in **document** coordinates — so it survives a
  zoom, a pan and a canvas rotation, and it means the same thing whatever layer is active. Picking
  a source **drops any latched offset**: re-picking is precisely the gesture that means "clone from
  here instead", and keeping the old offset would make the click do nothing.
* **The offset is `first stroke point − anchor`**, in document px. A target point `p` therefore
  reads the source at `p − offset`.
* **Aligned (default ON):** the offset is latched by the **first** stroke after the anchor is set,
  and **every later stroke keeps it**. This is what lets you paint a subject out over five strokes
  and have the five agree — the source travels with the brush *across* strokes as well as within
  one.
* **Non-aligned:** every stroke re-derives `strokeStart − anchor`, so **each stroke starts stamping
  at the anchor itself**. Repeated stamps of the same thing, deliberately.

A stroke begun with **no anchor picked** is **refused outright** — the engine never starts — and the
status bar says how to pick one. Inventing a source (the nearest edge, the pixel under the cursor)
would be a detector, and this tool has none.

---

## 4. Sampling modes

The `Sample` choice picks which pixels count as the source. All three are snapshotted **once, at
the press**, so the source is a still picture for the stroke's whole life.

| Mode | What it reads |
| --- | --- |
| **Current Layer** (default) | The active raster layer's own pre-stroke pixels, in its own grid. No compositing at all. |
| **Current and Below** | The document composited with everything that draws **above** the active layer temporarily hidden. |
| **All Layers** | The whole document's composite — the picture you are looking at. |

### "Current and Below" for a layer inside a transformed group

This is the case the phrase is ambiguous in, so the answer is stated rather than left to the reader.

**"Below" is read in the finished document, after every ancestor transform has been applied.**
`cloneSampleSnapshot` hides every sibling above the active layer *and* every sibling above each of
its ancestor groups, then composites normally. The active layer's own ancestors stay visible, so the
group's transform, opacity, blend mode and mask are all still in force. Concretely, for a layer
inside a group rotated 30°:

* the snapshot is a **document-space** image of the group's contribution (rotated), plus everything
  below the group, plus everything below the layer inside the group;
* nothing above the layer inside the group, and nothing above the group, contributes;
* the stamp maps target-layer pixels into that document image through the layer's **world**
  transform, so a stroke on the rotated layer clones from what the user can actually see rather
  than from the layer's own skewed grid.

The alternative reading — "the group's own local composite, in the group's space" — was rejected: it
would clone pixels that appear nowhere on screen, and the offset the user chose was measured on
screen.

⚠ The visibility flips are made and undone inside one function, with nothing between them but the
composite call. `core::Layer::setVisible` has no observers, so this is invisible to the panel, to
History and to the document's dirty flag. "All Layers" does not flip anything at all — it copies the
composite the canvas is already holding, which is why the common case costs a memcpy rather than the
full CPU walk (that walk is the named gesture-start stall, `docs/s60-gesture-start-stall.md`).

---

## 5. How the stroke is laid — the brush lane, with a different deposit

**There is no second dab walk, and there must never be one.** The clone stamp *is* the S19-a brush
stroke: the same press/drag dispatch, the same tablet drain, the same smoother, the same
`core::brush::BrushEngine`, the same reticle, and the same single `SetLayerPixelsCommand` on
release. What differs is only what lands.

Per composited batch:

1. `BrushEngine::composite()` runs as it always does and returns the rect it rewrote.
2. `VulkanCanvas::stampCloneRegion` **rewrites exactly that rect** through
   `core::applyCloneStamp`:

   ```
   alpha = coverage(p) · opacity · confine(p)
   out(p) = over( source(targetToSource · p), base(p), alpha · sourceAlpha )
   ```

That the two agree pixel for pixel about *where* the stroke is — and disagree only about *what* it
deposits — is the design, and it is why the clone stroke is pinned to the plain
`Uniform × Wash × Normal` path with an **opaque** placeholder colour. That is the one combination
whose finished alpha is exactly `coverage × opacity`, which is the number `applyCloneStamp`
recomputes. A preset tip, Buildup, a blend mode or the masking walk would make the engine's alpha
and the clone's disagree, and the mark would land somewhere the preview never showed. So the clone
stamp **never takes a brush preset** — the dock's preset section already hides for it, and
`currentBrushParams` excludes it explicitly.

Consequences that fall out of riding the brush lane, all of them free:

* **Size / hardness / opacity / flow / spacing** are the engine's own, off the options bar.
* **Pressure** drives size and flow, exactly as for the Brush (identity at pressure 1, so a mouse
  stroke is unchanged).
* **Selection confinement** is the engine's `StrokeConfinement`, applied by `applyCloneStamp` with
  the same coverage-multiply semantics — a feathered selection takes its proportion of the stamp,
  never an all-or-nothing clip.
* **One undo step per stroke.** The pixels are already in the layer when the pointer comes up;
  `finishBrushStroke` reads the stroke's bounding box out, `BrushEngine::restore()` puts the
  pre-stroke pixels back, and one region-scoped `SetLayerPixelsCommand` lands — byte for byte the
  paint brush's own commit, and the pattern the inpaint brush and the red-eye tool already use.
* **Cancel is exact.** `restore()` restores every pixel with coverage > 0, which is precisely the
  set `applyCloneStamp` writes.

### Auto-grow is OFF

The clone stamp opts out of the press-time layer auto-grow, for the inpaint brush's reason and one
of its own: it repairs **existing** content, and its pre-stroke snapshots (and the target→source map
built from the layer's world transform) are taken in the layer's own grid at the press — a growth
would re-home that grid under them and slide every stamped pixel by the growth's origin.

---

## 6. ⚠⚠ The pre-stroke-snapshot rule, and why it is not negotiable

**The deposit reads a PRE-STROKE snapshot of the destination and of the source. It never reads the
live target.**

`docs/brushes.md` §6.6b states the rule and §6.6c states its one documented exception. Reading live
pixels inside a stroke makes the mark depend on **composite cadence** — how often `composite()`
happened to run, which is a frame-rate question — and that breaks three things at once:

* **goldens**, which assume the mark is a pure function of `(params, samples)`;
* **undo replay**, which re-runs a recorded stroke and must reproduce it;
* **the incremental-refresh contract**, which lets the canvas re-composite a rectangle it has
  already composited and expect the same answer.

The exception in §6.6c is the **smudge walk**, whose smear *chain* is the mechanism — and even there
the engine does not read `m_target`; it keeps a stroke-local state buffer seeded from the pristine
base. **That exception does not apply here.** A clone stamp genuinely samples pre-stroke data
(§6.6b lists `duplicate` among the paintops that do), and a clone that read live pixels would smear
its own output every time the source and target regions overlap — the classic "the clone stamp
turned my texture to mush" defect.

In code this is two buffers, both taken at the press:

* `m_cloneBase` — the target layer's pristine pixels, the **destination** of the source-over.
  `applyCloneStamp` reads this, never `*in.target`, which makes the pass **idempotent per pixel**;
  that is what lets the canvas re-run it over a rectangle it has already written, and it is pinned
  by a test (`the deposit reads the PRE-STROKE snapshot, so running it twice changes nothing`).
* `m_cloneBackdrop` — the composited source, for the two non-default sampling modes only. In
  **Current Layer** mode there is no second buffer: the source *is* `m_cloneBase`.

The engine's own bounded `m_base` snapshot stays pristine throughout, because it is filled on
**first touch** (`coverage == 0`) during `deposit()`, which runs before `composite()` — and
`stampCloneRegion` only ever writes pixels whose coverage is already > 0.

### Known cost, and the way out if it ever matters

`m_cloneBase` is a **full copy of the layer image**, held for the stroke's duration (plus a
document-sized composite in the two backdrop modes). At 12 MP that is ~48 MB of transient working
buffer per stroke. It is deliberate: a lazily-grown snapshot would have to capture source pixels
*before* the stroke could reach them, and the source and target regions overlap precisely in the
case a clone stamp is most often used for. The bounded version is a real optimisation and a real
piece of work; it is not a correctness question, and it is not owed for this session.

---

## 7. Lineage, and the hard boundary on this tool

What ships is a positional copy and nothing more, and keeping it that way is the point.

**What is implemented is a positional copy.** Given a user-picked source point and a user-painted
stroke, the deposit is:

```
out = source(p − offset) · a  +  destination(p) · (1 − a)
```

with `a` the brush tip's own coverage. That is **alpha compositing of a translated copy** — the
same source-over expression the paint brush has run since S19-a, with the source colour read from
another place in the image instead of from a swatch. Positional pixel copying with a user-specified
offset, and alpha compositing through a soft brush mask, are both decades-old published practice:
Porter & Duff, *Compositing Digital Images*, SIGGRAPH 1984 (the `over` operator); Smith, *Paint*,
NYIT Technical Memo No. 7, 1978 (brush-mask compositing). Nothing here is novel.

**What is deliberately NOT implemented, and must not be added to this tool:**

* **healing** — any step that adjusts the stamped patch toward the destination's colour, luminance,
  texture or gradient field;
* **gradient-domain / Poisson seam blending** of the stamped patch;
* **texture synthesis** or patch-optimisation search of any kind;
* **any "make it match where it lands" step whatsoever**, however cheap.

That family belongs to S39; it is not this tool's job, and a reviewer who finds such a step here
should treat it as a defect. The option bar has no control that could motivate one: `Aligned` and
`Sample` are about *where the source is*, and everything else is the tip.

**Source-selection UX** — a modifier-click that sets a clone origin, and an "aligned" toggle — is
interface convention rather than mechanism, and it is described in general-purpose graphics texts
and in shipping free software (GIMP's Clone tool, Ctrl-click origin + Aligned/Registered/Fixed
modes, GPL, shipping since the 1990s) long enough to be unremarkable. We name published, dated work
only; nothing in this note asserts anything about what any other project practises.

---

## 8. UI

**Toolbar.** Its own slot (`ToolSlot::CloneStamp`) in the `PaintFill` group, between the Inpaint
brush and the eye tool. Shortcut **S** — free in `tool.cpp`'s descriptor table (the taken letters
are V M L W A C B E J Y K G I U P T Z), and free of the canvas's own bare-key gestures, which are
**R** (rotate) and Space (pan).

**Icon.** The default pack has carried `clone_stamp` art since S52 as a **reserved key**; the tool
it was reserved for now exists, so it simply claims it. No new SVG, no embed-list entry, no census
bump — and `heal` stays reserved, because healing is S39's and it is a different glyph on purpose.
Only one icon pack ships (the built-in default), so no other pack is owed anything.

**Options bar.** Size · Opacity · **Aligned** · **Sample**, with Hardness, Flow, Spacing and
Smoothing in the model for the tool's own panel. Smoothing is the shared brush-family preference and
the clone stamp joins the Brush / Eraser / Inpaint trio in `setBrushSmoothingEnabled`, so the four
cannot drift apart.

**Cursor and reticle.** Ordinarily the brush's size ring (the OS pointer is hidden, as for every
stroke tool). While the source-pick modifier is held the ring stands down and the cursor becomes the
**crosshair** — the next click picks a *point*, not a stroke, and the cursor says so.

⚠ **The clone stamp is the one stroke-family tool with no temporary eyedropper.** Ctrl already means
"pick the source" here, per the user's specification, and a tool cannot give one modifier two
meanings. `temporaryEyedropperActive()` excludes it explicitly.

**The source marker.** A circle with an inscribed diamond, at a fixed 9 logical px — distinct at a
glance from the brush's own size ring, and it never retraces a segment (a marker that draws a line
twice reads as a heavier weight where it doubles). It rides the shared overlay-line channel, exactly
as the shape wireframe, the pen spine and the Type area frame do; a lasso/shape/pen gesture can
never be live while the clone stamp is the active tool, so the channel is uncontended. It shows:

* the **picked anchor**, when no stroke is running;
* the **live source point** (`cursor − offset`) while a stroke runs — because the whole tool is
  "this lands over there", and a marker frozen at the anchor would stop saying that halfway through
  the first stroke.

**The source anchor survives a tool switch** (it is the tool's memory; losing it every time you
reach for the eyedropper would make the tool unusable) and is **cleared on a document swap** (a
point in a document that no longer exists).

### The ghost preview: deliberately not built

A translucent preview of the source *under* the reticle was scoped and **skipped**. It would need a
new renderer channel that samples the canvas texture at an offset — i.e. a shader and a present-pass
change, in `src/render/**` — for an affordance the live source marker already provides. The brief
said "skip it rather than risk the main path", and that is the call taken. If it is ever wanted, the
right shape is a second sampled quad on the present pass keyed off the same `cloneMarkerDocPoint()`
this file already computes.

---

## 9. Tests

`tests/test_clone_stamp.cpp`, all synthetic and all headless. Every expected number is hand-derived
in a comment beside it, so a change that moves a pixel has to argue with the arithmetic:

* the offset model — refusal with no anchor, aligned latching, non-aligned re-anchoring, and that
  re-picking the source drops the latched offset;
* `isWholePixelShift`, including a shift that reaches the sampler through a chain of affines;
* the sampler — nearest reads, out-of-bounds transparency, bilinear reproducing a pixel exactly at
  its own centre, a two-tap average, and the alpha-weighted colour rule that stops a transparent
  neighbour dragging its stale RGB in;
* the deposit — exact byte copy at full coverage, source-over at partial coverage, opacity as the
  ceiling, **idempotence** (the pre-stroke-snapshot rule), uncovered pixels left pristine,
  confinement as a coverage multiply with a feathered column, an off-source read depositing nothing,
  rect clamping, and malformed input;
* the UI half — the Sample index map and the marker's geometry (closed circle, on-circle vertices,
  no retraced segment, minimum visible radius).

---

## 10. Interactive checks only a human can run

The build is verified by construction and by the tests above; these are the things a person has to
look at.

1. **The basic gesture.** Ctrl-click a clean patch of a photograph, then paint over a blemish. The
   marker should appear where you clicked, the ring should track the pointer, and the stamped
   pixels should be an exact copy — zoom to 1:1 or further **(`docs/thumbnails-lie.md`: verify image
   work at 1:1 or on magnified crops, never on a downscaled strip)** and confirm grain and edges
   arrive intact, not softened.
2. **Aligned on.** Paint three separate strokes across a fence rail. The three must line up as one
   continuous copy of the source; the source marker must keep the same distance and bearing from the
   cursor throughout all three.
3. **Aligned off.** Same three strokes: each must restart at the anchor, stamping the same content
   three times.
4. **Re-picking mid-job.** With Aligned on and an offset established, Ctrl-click somewhere else and
   paint. The new stroke must clone from the new place, not from the old offset.
5. **Overlap.** Set a source only ~10 px from where you paint, then scrub back and forth over the
   same spot for several seconds. The result must stay crisp; any progressive smearing or mush means
   something is reading live pixels and the §6 rule has been broken.
6. **The three Sample modes.** On a document with an adjustment layer and a group above the active
   layer: *Current Layer* must ignore both, *Current and Below* must include the adjustment if it sits
   below and exclude the group above, and *All Layers* must clone exactly what you see. Then rotate
   a group containing the active layer and repeat — §4's ruling is what should be visible.
7. **Selection confinement.** Make a feathered elliptical marquee and clone across its edge. The
   stamp must fade with the feather, not stop at a hard line.
8. **Undo.** One stroke = one History entry named for a pixel edit; undo must restore the
   pre-stroke pixels exactly, and redo must put the clone back.
9. **Locked / vector layers.** Painting either must show the existing hint, not a silent no-op.
10. **No source picked.** Paint before Ctrl-clicking anything: the status bar must name the way in
    and nothing must land in History.
11. **The cursor swap.** Hold and release Ctrl with the pointer motionless over the canvas. The
    crosshair and the size ring must swap immediately, with no mouse movement needed.
12. **Tablet.** A pressure stroke should thin the stamp at the ends like a brush stroke; a tap
    should deposit one dab.
13. **macOS.** ⌘-click, not Ctrl-click, must pick the source (`FL_COMMAND`).
14. **Visual pass.** The toolbar glyph, the flyout-free slot, the options-bar layout at a narrow
    window, and the marker's weight against both themes.
