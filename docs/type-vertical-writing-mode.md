# Vertical writing-mode — implementation plan (FULLY BUILT as of 2026-07-02)

> Companion to `docs/type-deferred-features.md` **§3** (the DESIGN + rationale) and `docs/type-tool.md`
> (the Type tool overall). This is the concrete, codebase-grounded build plan: the integration points
> with file anchors, the commit sequence, the settled decisions, and the test posture. **Status: every
> commit below has landed** — A (model), B1 (axis abstraction), B2 (vertical layout), B3 (Latin
> orientation), D/UI (Type-panel Writing mode + Orientation controls — note the placement was REVISED
> off the context bar into the panel), and C (editing geometry: caret/selection/hit-test/goal-column,
> the visual arrow-key remap, vertical spell squiggles along the column's under side, and the Point
> side-baseline decoration). Kept as the design record; file anchors below describe the pre-vertical
> code and may have drifted.

## What ships (user-visible)
A text object can flow **vertically** — glyphs stack top-to-bottom down a column, columns advance across
the box — the way Japanese/Chinese/Korean is traditionally set. A compact **writing-mode control on the
Type context bar** (right after the Size slider, before "Style…") flips a text object between Horizontal
and Vertical; embedded Latin/numbers rotate 90° by default (with an upright option). The caret, selection,
hit-testing, spell squiggles and arrow-key navigation all follow the vertical flow. It is a property of
the **whole text object**, never a separate tool.

## Settled decisions (user, 2026-07-01)
- **D1 Control placement → Type CONTEXT BAR**, inline right after the Size slider and before "Style…"
  (bar reads Font · Size · [Writing mode] · Style… · 3D…). No separate "vertical text" tool.
- **D2 Granularity → the WHOLE TEXT OBJECT.** Writing mode is a `TextBlock` property (next to
  `frame`/`areaSize`/`aa`), not per-paragraph. One text layer is entirely horizontal or entirely
  vertical — simplest model + UX, matches how vertical CJK is actually used.
- **D3 Scope → EVERYTHING for v1:** `vertical-rl` (right-to-left columns, the CJK standard) AND
  `vertical-lr` (left-to-right columns, e.g. Mongolian), AND the Latin **text-orientation** choice
  (`mixed` = rotate non-CJK 90°, the default; `upright` = stack Latin upright).
- **Sub-decision (mine, revisit if wanted): the bar control is a compact WRITING-MODE dropdown**
  (Horizontal / Vertical (RTL) / Vertical (LTR)); the rarer Latin **orientation** (mixed/upright)
  refinement lives in the **Type panel** Paragraph/Block area, so the bar stays a single control while
  still covering everything.

## Model (commit A)
Add to `core/text/text_model.hpp` `TextBlock` (block-level, like `frame`/`aa`):
```cpp
enum class WritingMode { HorizontalTB, VerticalRL, VerticalLR };   // default HorizontalTB
enum class TextOrientation { Mixed, Upright };                     // default Mixed (Latin rotated 90°)
WritingMode writingMode = WritingMode::HorizontalTB;
TextOrientation orientation = TextOrientation::Mixed;              // only consulted when vertical
```
`operator==` is defaulted (extends automatically). Whatever serializes a `TextBlock` (the `.mosaic`
manifest, later) gains two enum fields; the S29 model has no on-disk format yet, so this is just the
in-memory struct + its unit tests (`test_text_model.cpp`). **No behavior change** — defaults keep every
existing block horizontal. This commit is small and safe; it unblocks the rest.

## Shaping / layout — the core rework (`core/text/shaping.cpp`)
The layout is fully horizontal today (`shaping.cpp:302` `TextShaper::layout`):
- **Run shaping** (`~200–230`): `hb_buffer_set_direction(HB_DIRECTION_LTR/RTL)`, per-glyph
  `g.xAdvance` from `pos[i].x_advance`. Vertical needs `HB_DIRECTION_TTB`, the **`vert`/`vrt2`**
  feature enabled, and `y_advance`/`x_offset`/`y_offset` read from the vertical metrics (HarfBuzz
  supplies vertical advances + origins). A run's advance becomes a **Vec2**, not a scalar.
- **Line breaking** (`~348–438`): breaks when `advanceOf(...) > rightBudget` (a horizontal x budget).
  For vertical the budget is the column's **height** (`areaSize.y` for Area; unbounded for Point,
  which grows the column downward). The break logic is otherwise axis-agnostic once "advance" is the
  inline-axis component.
- **Position pass** (`~442–576`): computes `baselineY` per line (stacking down by leading) and `penX`
  per glyph (advancing right). This is the pass to **transpose**:
  - Horizontal-tb: inline = +x, block = +y (unchanged).
  - Vertical-rl: inline = +y (glyphs down the column), block = **−x** (columns right→left).
  - Vertical-lr: inline = +y, block = **+x** (columns left→right).
  Leading becomes the **column advance** (cross-axis gap); `ascent`/`descent` become the glyph's
  extent on either side of the column centerline (the vertical "baseline" is the column's centre).

### The de-risking split
`ShapedLine` (`shaping.hpp:48`) hard-codes horizontal fields (`baselineY`, `x`, `width`, `ascent`,
`descent`). Rather than rewrite it and the new behavior at once:
- **Commit B1 — engine generalization, OUTPUT-IDENTICAL.** Refactor `ShapedLine` + the position pass
  to an **axis abstraction** (inline/block axes, a `writingMode`-driven basis) that reproduces
  byte-identical horizontal output. `ShapedGlyph.advance` becomes the inline-axis advance; `ShapedGlyph.pen`
  stays the final layer-local position (already a `Vec2`, so downstream render is untouched here). Golden
  text-render tests (`test_text_render.cpp`) and caret tests (`test_text_edit.cpp`) must stay green
  unchanged — that is the proof the refactor is behavior-preserving.
- **Commit B2 — vertical-rl + vertical-lr.** Feed the vertical modes through the generalized engine:
  HarfBuzz `HB_DIRECTION_TTB` + `vert`, column stacking (−x / +x), height-budget line breaks, CJK
  glyphs upright. Verifiable headlessly: assert glyph pen positions for a known CJK string flow down
  then columns step sideways (a metrics golden, no font-file dependency beyond a bundled CJK test face
  — or gate on face availability like the hyphenation dict tests).
- **Commit B3 — Latin orientation.** In a vertical block, non-ideographic runs (Latin, digits) get a
  **90° glyph rotation** (`orientation == Mixed`) applied to the outline in `glyphContours`
  (`shaping.cpp:~99`) + the colour-glyph tile transform (`text_render.cpp`), advancing along the
  column by the glyph's *horizontal* advance. `orientation == Upright` stacks them upright (each Latin
  glyph its own column cell, advancing by its vertical extent). Reuse the tokenizer's `cjk` flag
  (`core/text/tokenize.*`) to classify runs.

## Editing geometry + navigation (commit C — `core/text/text_edit.*`, `ui/vulkan_canvas.cpp`)
Every geometry helper assumes horizontal and must become writing-mode aware (it already takes the
`ShapedBlock`, so it can read the mode once threaded through — pass `WritingMode` or read it off the
block):
- `caretGeometry` (`text_edit.hpp:85`): a caret is a bar **perpendicular to the flow** — horizontal in
  vertical text, between the glyph above and below.
- `selectionRects` (`text_edit.hpp:90`): per-**column** run rectangles instead of per-row. Spell
  squiggles (which map ranges through `selectionRects`, `vulkan_canvas.cpp`) fall out for free.
- `hitTest` (`text_edit.hpp:79`): nearest column, then nearest inter-glyph gap down it.
- `moveCaretVertical` / `visualLineStart` / `visualLineEnd` (`text_edit.hpp:97–105`): the "goal column"
  and Home/End rotate to the vertical axis.
- **Arrow-key remap** (`vulkan_canvas.cpp` key handler, `moveTextCaret`): in vertical-rl, **Down** =
  next glyph in column, **Up** = previous, **Left** = next column, **Right** = previous column
  (mirrored for vertical-lr). The visual arrow always moves the caret the way it points.
All of this is pure + unit-testable (`test_text_edit.cpp`) on a hand-built vertical `ShapedBlock`.

## UI (commit D — `ui/tool.cpp`, `ui/app_window.cpp`)
The `aa` control is the exact template — a **block-level** bar option reflected + applied without a new
funnel:
- `tool.cpp` `defaultOptionsFor(ToolId::Text)`: add a `choice`/`toggle` `ToolOption` **before**
  `typePanelButton()`, `inlineFlow = true` (flows in the left cluster, like Style…). Id `"writingMode"`.
- `app_window.cpp` `reflectTextOptions()` (`~1906`): read `textEditBlockForUi()->writingMode` onto the
  option (mirrors the `aa` reflect at `~1919`).
- `app_window.cpp` option-apply (`~1982`, the `id == "aa"` branch): `applyTextBlockEdit([wm](TextBlock&
  b){ b.writingMode = wm; }, coalesce)` — one undo step, the existing block-level funnel.
- The Latin **orientation** (Mixed/Upright) control lands in the Type panel (`ui/type_panel.cpp`)
  Paragraph/Block section via `applyTextBlockEdit`, shown only when the block is vertical.
- **Area box interaction:** a vertical Area box still resizes via the Type-edit box; the wrap budget is
  now its height. The "Type sizes / Move stretches" split is unchanged (`docs/type-tool.md §7`).

## Commit sequence (one logical change each)
1. **A — model:** `WritingMode` + `TextOrientation` on `TextBlock`, defaults horizontal, `operator==`,
   `test_text_model` cases. No behaviour change.
2. **B1 — engine generalization (output-identical):** axis-abstracted `ShapedLine` + position pass;
   all existing golden/caret tests stay green unchanged. No new behaviour.
3. **B2 — vertical-rl + vertical-lr layout:** HarfBuzz vertical shaping + column stacking + height
   budget; CJK upright. Metrics tests.
4. **B3 — Latin orientation:** rotate non-CJK runs 90° (Mixed) + the Upright option; outline + colour
   tile transforms.
5. **C — editing geometry + key nav:** caret/hit/selection/goal-column + arrow remap; squiggles &
   selection follow. `test_text_edit` vertical cases.
6. **D — UI:** the context-bar writing-mode control (reflect/apply like `aa`) + the Type-panel
   orientation control.

## Testing posture
Model, shaping metrics, and editing geometry are **headless-testable** and deterministic (doctest):
assert glyph pen positions and caret/selection rects for known strings in each mode; gate any real
CJK-font assertions on face availability (the CI-safe pattern used for the hyphenation dictionaries).
The **rendered** result — real vertical CJK, rotated vs upright Latin, the caret/selection *feel*, the
bar control — is the user's visual pass (per the verification division). B1 is validated specifically by
the pre-existing horizontal golden tests staying byte-identical.

## Licences
Vertical writing-mode is standard, decades-old CJK typesetting, codified in the open **CSS Writing
Modes** spec. HarfBuzz (MIT) vertical shaping and the FreeType outline rotation are already-used,
permissive/GPL-compatible dependencies — no new library.
