#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

#include "common/geometry.hpp"
#include "core/text/shaping.hpp"     // ShapedBlock (geometry/hit-test input)
#include "core/text/text_model.hpp"  // TextBlock

// On-canvas text editing core (docs/type-tool.md §6): the caret/selection model, byte<->geometry
// mapping, hit-testing, and caret navigation -- the pure, FLTK-free machinery the TextEditSession
// (vulkan_canvas) drives. Coordinates are LAYER-LOCAL em space, exactly like the ShapedBlock the
// shaper produces; the canvas folds in the layer transform + zoom itself. Kept free of FreeType/
// HarfBuzz and FLTK so it is unit-tested headlessly (per the verification division).
//
// Caret positions are BYTE offsets into TextBlock::utf8 (the same space as StyleRun ranges and a
// glyph's `cluster`). Stepping is by Unicode codepoint; grapheme clustering (combining marks, emoji
// ZWJ) and full bidi/RTL caret movement are later refinements -- this lane is correct for LTR text
// and degrades gracefully (it never indexes mid-UTF-8) for the rest, the "ASCII-first is fine" build
// the design sanctions (§6).
namespace mosaic::core::text {

using common::Vec2;

// A caret or selection over TextBlock::utf8, in byte offsets. `anchor` is the fixed end of a shift/
// drag selection; `focus` is the moving end (where the caret blinks). empty() => a plain caret.
struct TextSelection {
    std::size_t anchor = 0;
    std::size_t focus = 0;

    [[nodiscard]] bool empty() const noexcept { return anchor == focus; }
    [[nodiscard]] std::size_t lo() const noexcept { return anchor < focus ? anchor : focus; }
    [[nodiscard]] std::size_t hi() const noexcept { return anchor < focus ? focus : anchor; }
    void collapseTo(std::size_t p) noexcept { anchor = focus = p; }

    bool operator==(const TextSelection&) const = default;
};

// The drawing geometry of one caret in layer-local space: the bar runs from `top` to `bottom` --
// its two ENDS, named for the horizontal case; in a vertical block the bar lies ACROSS the column
// (top = the ascent-side end). `angleRad` is the bar's tilt off upright (0 for horizontal text,
// pi/2 when it lies across a vertical column; rotated layers / on-path baselines refine it later,
// §6.1). `height()` is the bar length (a convenience for the blink overlay).
struct CaretGeometry {
    Vec2 top{0, 0};
    Vec2 bottom{0, 0};
    double angleRad = 0.0;

    [[nodiscard]] double height() const noexcept { return (bottom - top).length(); }
};

// ---------------------------------------------------------------------------------------------
// UTF-8 / byte stepping (no font needed -- deterministic, fully unit-tested)
// ---------------------------------------------------------------------------------------------
// The next / previous codepoint boundary from byte offset `pos` (clamped to [0, size]). Never lands
// inside a multi-byte sequence; a malformed byte advances by one.
[[nodiscard]] std::size_t nextCharBoundary(std::string_view utf8, std::size_t pos);
[[nodiscard]] std::size_t prevCharBoundary(std::string_view utf8, std::size_t pos);

// Word-boundary stepping (UAX#29-lite: a word is a maximal run of letters/digits; moving forward
// skips trailing spaces, moving back skips leading spaces -- the editor "Ctrl+Arrow" feel).
[[nodiscard]] std::size_t nextWordBoundary(std::string_view utf8, std::size_t pos);
[[nodiscard]] std::size_t prevWordBoundary(std::string_view utf8, std::size_t pos);

// The byte range of the word under `pos` (double-click select-word): the letters/digits run, or the
// run of identical whitespace/punctuation, around `pos`. Returns {pos,pos} only for an empty string.
[[nodiscard]] TextSelection wordAt(std::string_view utf8, std::size_t pos);

// Start / end byte offset of the PARAGRAPH (the text between '\n's) containing `pos`. Logical-line
// Home/End; visual (wrapped) line edges use visualLineStart/End below.
[[nodiscard]] std::size_t paragraphStart(std::string_view utf8, std::size_t pos);
[[nodiscard]] std::size_t paragraphEnd(std::string_view utf8, std::size_t pos);

// ---------------------------------------------------------------------------------------------
// Geometry & hit-testing (need the laid-out ShapedBlock)
// ---------------------------------------------------------------------------------------------
// All of these read the block's writing mode off the ShapedBlock: for vertical text a "visual line"
// is a COLUMN, the inline axis runs down it (layer y), and results are projected into layer space
// accordingly. Horizontal output is unchanged (bit-identical to the pre-vertical code).
//
// The byte offset whose caret is nearest the layer-local point `local` (where a click lands the
// caret). Picks the nearest line on the block axis (row for horizontal, column for vertical), then
// the nearest inter-glyph gap along it; clamps to the block's start/end. Returns 0 for an empty
// block.
[[nodiscard]] std::size_t hitTest(const ShapedBlock& shaped, const TextBlock& block, Vec2 local);

// The caret bar for byte offset `pos`, in layer-local space -- always perpendicular to the flow
// (upright for horizontal text, lying across the column for vertical). Handles end-of-line, the
// trailing caret after the last glyph, empty lines, and an empty block (a default-height bar at the
// flow's start corner, sized from the block's first run). At a soft-wrap boundary the caret shows at
// the START of the next visual line (click-consistent; the End-key affinity refinement is a later
// round).
[[nodiscard]] CaretGeometry caretGeometry(const ShapedBlock& shaped, const TextBlock& block,
                                          std::size_t pos);

// The highlight rectangles (layer-local) covering byte range [lo, hi) -- one per visual line (per
// COLUMN in vertical modes) the range spans; an empty selected line gets a thin sliver so it reads
// as selected. Empty when lo>=hi.
[[nodiscard]] std::vector<common::Rect> selectionRects(const ShapedBlock& shaped,
                                                       const TextBlock& block, std::size_t lo,
                                                       std::size_t hi);

// The selection highlight as ORIENTED quads (each {TL,TR,BR,BL} in layer-local space) covering byte
// range [lo, hi). For flat/vertical text this is selectionRects turned into quads (axis-aligned, one
// per visual line -- identical coverage). For a BENT baseline (§9) an axis-aligned Rect can't describe
// a rotated run, so this returns one quad PER GLYPH, each turned by the glyph's baselineAngle, so the
// highlight (and the squiggle drawn along each quad's bottom edge) rides the arch. Empty when lo>=hi.
[[nodiscard]] std::vector<std::array<common::Vec2, 4>> selectionQuads(const ShapedBlock& shaped,
                                                                      const TextBlock& block,
                                                                      std::size_t lo, std::size_t hi);

// Move the caret one VISUAL line over (`dir` = -1 toward the first line, +1 toward the last; a line
// is a column in vertical modes, where +1 = the next column in column order). Aims for the INLINE
// coordinate `desiredInline` -- layer x for horizontal, layer y for vertical -- the caret "goal
// column/row", so repeated steps keep it. Returns `pos` unchanged at the first/last line. Pass
// `desiredInline < 0` to use the caret's current inline position.
[[nodiscard]] std::size_t moveCaretVertical(const ShapedBlock& shaped, const TextBlock& block,
                                            std::size_t pos, int dir, double desiredInline);

// Start / end byte offset of the VISUAL (wrapped) line containing `pos` -- Home/End. For Point text
// (one line per paragraph) these equal paragraphStart/End.
[[nodiscard]] std::size_t visualLineStart(const ShapedBlock& shaped, const TextBlock& block,
                                          std::size_t pos);
[[nodiscard]] std::size_t visualLineEnd(const ShapedBlock& shaped, const TextBlock& block,
                                        std::size_t pos);

}  // namespace mosaic::core::text
