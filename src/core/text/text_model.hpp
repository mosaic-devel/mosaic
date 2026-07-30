#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/geometry.hpp"
#include "common/image.hpp"        // ColorF
#include "core/text/extrude.hpp"     // Extrude (3D, S30-c; optional on the block)
#include "core/vector/geometry.hpp"  // vec::Contours -- PathFit's baked baseline (S30 §9)
#include "core/vector/paint.hpp"     // vec::Paint -- a run's colour is the vector paint model

// The rich-text data model behind Mosaic's Type tool (S29-a; design in docs/type-tool.md §3).
// All coordinates are LAYER-LOCAL (em / text units); the owning TextLayer's transform() places
// the block into the document, exactly as RasterLayer pixels and VectorLayer geometry are
// layer-local. This header is pure data + the run-list invariant helpers -- it depends only on
// `common` and the vector paint model, never on FreeType/HarfBuzz, so the model is testable with
// zero font machinery and a GPU renderer is a later backend swap, not a remodel (§5.4).
//
// Text is a flat string of Unicode plus a list of STYLE RUNS over byte ranges (not a span tree):
// enough for everything Mosaic needs, trivial to round-trip to SVG <tspan>/our native format, and
// it keeps split/merge-on-edit simple. The runs covering-and-non-overlapping invariant is the
// model's single most error-prone bit, so it gets its own helpers (below) and its own unit tests.
namespace mosaic::core::text {

using common::ColorF;
using common::Vec2;

// ---------------------------------------------------------------------------------------------
// Per-run style
// ---------------------------------------------------------------------------------------------
// A font request, resolved against the FontProvider/FontDB at shape time (§4.2): never a face
// handle, always a description, so the same block re-resolves correctly on another machine.
struct FontRef {
    std::string family = "Sans";             // e.g. "Inter", "Noto Serif"; resolved cross-platform
    float weight = 400.0f;                    // CSS-style 1..1000 (variable-font 'wght' aware §3.4)
    bool italic = false;                      // synthesised oblique if the face lacks a real italic
    float widthAxis = 100.0f;                 // OpenType 'wdth' (condensed..expanded), variable fonts
    std::map<std::string, float> variations;  // any other OpenType variable axis (tag -> value)

    bool operator==(const FontRef&) const = default;
};

// How adjacent-glyph spacing is derived (R4; docs/type-tool.md §13). Metric = the font's own
// kern/GPOS pairs (the default). Optical = pair spacing computed from the glyph SHAPES -- each
// pair's optical white is pulled toward the face's own stem gap -- for faces with poor kern data
// or mixed runs. None = no pair adjustment at all. Optical follows the long-published lineage:
// URW/Karow's "uniform optical white", plus Neville's ink-profile sampling.
enum class Kerning : std::uint8_t { Metric, Optical, None };

// The style of one run of characters. The run's paint is a vec::Paint (§3.3) -- solid OR gradient,
// the same model a shape fill uses -- so "convert to path" preserves it exactly and a future
// per-run stroke is just the vector Stroke. (The design doc's `ColorF fill` is the common-case
// shorthand; the model stores the full paint and `solidFill()`/`setSolidFill()` wrap the easy case.)
struct CharStyle {
    FontRef font;
    float sizePx = 24.0f;                     // em size in layer units (px at the layer's base scale)
    vec::Paint paint = vec::SolidPaint{ColorF{0.0f, 0.0f, 0.0f, 1.0f}};  // black, opaque
    bool underline = false;
    bool strikethrough = false;
    float tracking = 0.0f;                    // extra letter-spacing, 1/1000 em
    float baselineShift = 0.0f;               // super/subscript, px (positive = up)
    std::vector<std::string> features;        // enabled OpenType features ("liga","smcp","ss01"…) §3.4
    Kerning kerning = Kerning::Metric;        // pair spacing source (metric / optical / none) -- R4

    bool operator==(const CharStyle&) const = default;

    // Common-case colour accessors over `paint` (transparent when the paint is not a flat colour).
    [[nodiscard]] ColorF solidFill() const {
        if (const auto* s = std::get_if<vec::SolidPaint>(&paint)) return s->color;
        return ColorF{0, 0, 0, 0};
    }
    void setSolidFill(ColorF c) { paint = vec::SolidPaint{c}; }
};

// A half-open BYTE range [begin, end) into TextBlock::utf8, carrying its style. The run list
// covers [0, utf8.size()) with no gaps or overlaps after every edit (see the invariant helpers).
struct StyleRun {
    std::size_t begin = 0;
    std::size_t end = 0;
    CharStyle style;

    [[nodiscard]] std::size_t length() const noexcept { return end - begin; }
    bool operator==(const StyleRun&) const = default;
};

// ---------------------------------------------------------------------------------------------
// Per-paragraph style (a paragraph is the text between '\n's)
// ---------------------------------------------------------------------------------------------
struct Paragraph {
    enum class Align { Left, Center, Right, Justify } align = Align::Left;
    float leading = 1.2f;            // line height as a multiple of em (or absolute px if below)
    bool leadingAbsolute = false;
    float spaceBefore = 0.0f;        // px above the paragraph
    float spaceAfter = 0.0f;         // px below the paragraph
    float indentFirst = 0.0f;        // first-line indent, px
    float indentLeft = 0.0f;
    float indentRight = 0.0f;
    enum class Direction { Auto, LTR, RTL } direction = Direction::Auto;  // bidi base direction
    // BCP-47 language tag (e.g. "en-US", "de", "ja"); empty ⇒ inherit the document/app default.
    // Drives hyphenation-pattern and spell-dictionary selection (docs/type-deferred-features.md §0);
    // has no visible effect until those features consume it. See core/text/language.hpp.
    std::string language;
    // Automatic hyphenation of wrapped (Area) lines, using `language` to pick the pattern dictionary
    // (deferred §1). Off by default. Point text does not wrap, so it is ignored there.
    bool hyphenate = false;

    bool operator==(const Paragraph&) const = default;
};

// ---------------------------------------------------------------------------------------------
// The block
// ---------------------------------------------------------------------------------------------
enum class TextFrame {
    Point,  // click-created: an insertion caret; the box grows with the text, no wrapping (§7)
    Area,   // drag-created: a wrapping frame of `areaSize`; text reflows inside it
};

// Anti-aliasing mode for the rasterized text (§4.3). Subpixel only holds on an opaque, axis-
// aligned, unrotated, non-3D background, so the renderer silently degrades it to Grayscale when
// those preconditions are violated -- it is offered, never the default.
enum class AntiAlias { None, Grayscale, Subpixel };

// Writing mode: which way the text flows (docs/type-vertical-writing-mode.md). A whole-block property
// (like `frame`/`aa`), not per-paragraph. HorizontalTB is the default LTR/RTL horizontal flow with
// lines stacked top-to-bottom. The Vertical modes stack glyphs top-to-bottom down a column, with
// columns advancing right-to-left (VerticalRL -- the CJK standard) or left-to-right (VerticalLR).
// Consumed by shaping/layout + editing geometry only once the vertical engine lands; inert until then.
enum class WritingMode { HorizontalTB, VerticalRL, VerticalLR };

// In a vertical writing mode, how non-ideographic runs (Latin, digits) sit (CSS `text-orientation`).
// Mixed rotates them 90 degrees so a Latin word reads sideways (the default); Upright stacks each
// glyph upright in its own column cell. Ignored in HorizontalTB.
enum class TextOrientation { Mixed, Upright };

// Fit-to-path (S30, docs/type-tool.md §9): flow the block along a vector layer's path. The block
// references the source layer by id -- NOT by copying its geometry -- so editing/moving the path
// re-flows the text non-destructively. Glyphs are placed by arc-distance along the flattened path
// (the same position+tangent machinery as `bend`; bend is ignored while a PathFit is active).
struct PathFit {
    std::uint64_t layer = 0;  // the source VectorLayer's LayerId (core::LayerId; 0 = none)
    // The two range BRACKETS, as arc-distances along the flattened path: the text lays out from s0
    // toward s1 and the on-canvas brackets slide these. A closed single-contour path wraps (text
    // slides around a circle); an open path extends straight past its ends on overflow.
    double s0 = 0.0;
    double s1 = 0.0;
    // Ride the path's other side: the text mirrors across the path and reads along the reversed
    // direction (the centre bracket drag-across-the-path toggle).
    bool flip = false;
    // The source geometry flattened into THIS block's layer-local space. DERIVED state -- the app
    // re-bakes it whenever the source layer's content or either layer's transform changes (the
    // `layer` reference stays authoritative); cached on the block so layout()/editing geometry
    // remain pure functions of (block, fonts).
    vec::Contours baked;

    bool operator==(const PathFit&) const = default;
};

struct TextBlock {
    std::string utf8;                  // the text; one logical string, '\n' splits paragraphs
    std::vector<StyleRun> runs;        // cover [0, utf8.size()); split/merged on edit (invariant)
    std::vector<Paragraph> paragraphs; // one per '\n'-delimited paragraph (parallel to splits)
    TextFrame frame = TextFrame::Point;
    Vec2 areaSize{0, 0};               // Area only: the wrapping box in layer units (Point ignores)
    AntiAlias aa = AntiAlias::Grayscale;
    // Vertical writing-mode (docs/type-vertical-writing-mode.md). Block-level; default = the existing
    // horizontal flow, so every current block is unchanged. `orientation` only applies when vertical.
    WritingMode writingMode = WritingMode::HorizontalTB;
    TextOrientation orientation = TextOrientation::Mixed;
    // Baseline BEND/warp (S30, docs/type-tool.md §9): a signed amount in [-1,1] that bows the block's
    // baseline into a circular arc, 0 = flat (the default, so every existing block is unchanged).
    // Positive arches up (∩), negative down (∪). Each glyph is carried rigidly to its arc position and
    // turned by the local tangent (placement by position + tangent), so bend composes with per-run
    // styling. Horizontal text only; ignored for the vertical modes. Composes with 3D: the warped
    // outlines feed the extrude mesher, so the solid follows the curved baseline (§9).
    float bend = 0.0f;
    // Fit-to-path (§9): when set (and the baked path is non-empty), the block flows along the
    // referenced vector layer's path instead of a straight/bent baseline. Horizontal text only;
    // takes precedence over `bend`; composes with 3D exactly as bend does.
    std::optional<PathFit> pathFit;
    // The style a freshly-created/typed run inherits WHILE the block is EMPTY (no runs to read from).
    // The caret height and the first typed character both come from here, so an empty Point/Area
    // block's caret already shows the chosen size/font instead of a hardcoded 24px default (§7,
    // fixlist #3). makeBlock() seeds it from its style argument; styleAt()/blockEm() fall back to it
    // when `runs` is empty. Ignored once the block has any text (the runs carry the real styles).
    CharStyle emptyStyle;
    // 3D extrusion (S30-c, docs/type-tool.md §10): nullopt = the flat 2D default (every block
    // until the user enables 3D). One Extrude for the whole block -- one watertight solid.
    std::optional<Extrude> extrude;

    bool operator==(const TextBlock&) const = default;

    [[nodiscard]] bool empty() const noexcept { return utf8.empty(); }
};

// ---------------------------------------------------------------------------------------------
// Run-list & paragraph invariant helpers (the error-prone bit -- unit-tested in test_text_model)
// ---------------------------------------------------------------------------------------------
// The number of '\n'-delimited paragraphs in `utf8` (always >= 1; the empty string is one empty
// paragraph). `paragraphs` must parallel this count.
[[nodiscard]] std::size_t paragraphCount(const std::string& utf8);

// True when `runs` exactly tile [0, utf8.size()): sorted, contiguous, non-empty, first.begin==0,
// last.end==utf8.size() (or runs empty iff utf8 empty), AND paragraphs.size()==paragraphCount.
[[nodiscard]] bool isValid(const TextBlock& block);

// Force the invariant: drop empty/zero-length runs, sort, fill any gap with the neighbouring
// style (or a default CharStyle), clamp the last run to utf8.size(), merge adjacent equal-style
// runs, and resize `paragraphs` to paragraphCount (preserving existing entries, padding with a
// copy of the last / a default). Idempotent; isValid() holds afterwards.
void normalize(TextBlock& block);

// The style in effect at byte offset `pos` (clamped into the last run at end-of-text). Returns a
// default CharStyle when the block has no runs. This is what the caret/panel read for "current
// style", and what a Point-text insertion inherits.
[[nodiscard]] CharStyle styleAt(const TextBlock& block, std::size_t pos);

// Replace the style over [begin, end) with `style`, splitting runs at the boundaries and merging
// equal neighbours afterwards. begin/end are clamped to [0, utf8.size()); an empty/inverted range
// is a no-op. Keeps the invariant (callers need not normalize after).
void setStyleRange(TextBlock& block, std::size_t begin, std::size_t end, const CharStyle& style);

// Apply `mutate` to a copy of the existing style over [begin, end) (so callers change ONE
// property -- font, size, a feature -- without rebuilding the whole CharStyle). Splits/merges as
// setStyleRange does; equal-style segments after the edit collapse.
void mutateStyleRange(TextBlock& block, std::size_t begin, std::size_t end,
                      const std::function<void(CharStyle&)>& mutate);

// ---------------------------------------------------------------------------------------------
// Selection-wide queries (S29-c: what the Type context bar / panel read & write -- §8.2)
// ---------------------------------------------------------------------------------------------
// Which CharStyle fields are UNIFORM across a queried byte range. Each flag is true when every run
// touching the range shares that field's value; a false marks a "mixed" field the surfaces show
// blank/—. Setting a mixed field makes it uniform again. A caret (empty range) agrees on every field.
struct StyleAgreement {
    bool family = true, weight = true, italic = true, widthAxis = true, variations = true;
    bool sizePx = true, paint = true, underline = true, strikethrough = true;
    bool tracking = true, baselineShift = true, features = true, kerning = true;
};

// The common style across byte range [lo, hi): `style` carries the FIRST touched run's value for
// every field, and `agree` flags which fields are uniform across the range (so a control reads its
// value from `style` only where `agree` is set). lo==hi (a caret) returns styleAt(lo) all-agreeing;
// an empty block returns its emptyStyle all-agreeing. Range clamped/ordered into [0, utf8.size()).
struct CommonStyle {
    CharStyle style;
    StyleAgreement agree;
};
[[nodiscard]] CommonStyle commonStyle(const TextBlock& block, std::size_t lo, std::size_t hi);

// ---------------------------------------------------------------------------------------------
// OpenType feature toggles over CharStyle::features (R4, docs/type-tool.md §3.4)
// ---------------------------------------------------------------------------------------------
// `features` carries HarfBuzz feature strings: "smcp" enables a feature, "-liga" disables one the
// shaper applies by default (liga/clig/calt/kern). A UI toggle therefore reads/writes PRESENCE
// against the feature's default: a default-ON feature is on unless "-tag" is listed; a default-OFF
// one is on iff "tag" is. setFeatureEnabled normalizes (drops both forms, then records only a
// deviation from the default), so an untouched style stays an empty list.
[[nodiscard]] bool featureEnabled(const std::vector<std::string>& features, const std::string& tag,
                                  bool defaultOn);
void setFeatureEnabled(std::vector<std::string>& features, const std::string& tag, bool defaultOn,
                       bool on);

// The 0-based paragraph index containing byte offset `pos` -- the count of '\n' before pos (clamped).
[[nodiscard]] std::size_t paragraphIndexAt(const std::string& utf8, std::size_t pos);

// Per-field uniformity across the paragraphs a byte range touches (the paragraph twin of the above).
struct ParagraphAgreement {
    bool align = true, leading = true, leadingAbsolute = true, spaceBefore = true, spaceAfter = true;
    bool indentFirst = true, indentLeft = true, indentRight = true, direction = true, language = true;
    bool hyphenate = true;
};
struct CommonParagraph {
    Paragraph para;
    ParagraphAgreement agree;
};
// The common paragraph style across the paragraphs touched by byte range [lo, hi) -- those from
// paragraphIndexAt(lo) through paragraphIndexAt(hi-1) (a caret touches its one paragraph).
[[nodiscard]] CommonParagraph commonParagraph(const TextBlock& block, std::size_t lo, std::size_t hi);

// Apply `mutate` to EVERY paragraph the byte range [lo, hi) touches (so changing one property --
// alignment, leading -- across a multi-paragraph selection is one call). A caret edits its single
// paragraph. Paragraph count is unchanged, so the invariant holds (no normalize needed).
void mutateParagraphRange(TextBlock& block, std::size_t lo, std::size_t hi,
                          const std::function<void(Paragraph&)>& mutate);

// Build a block whose whole text shares one style and whose paragraphs default. The simplest
// constructor -- what authoring (S29-b) uses to create a fresh Point/Area block.
[[nodiscard]] TextBlock makeBlock(std::string utf8, CharStyle style = {},
                                  TextFrame frame = TextFrame::Point);

// Scale every run's font size (and the pending emptyStyle, and px-absolute baseline shift) by
// `factor`, clamping each size to [minSizePx, maxSizePx]. This is how Point text resizes on the
// Type-edit box's corner handle: the type re-shapes crisply at the new size, rather than the layer
// transform stretching the cached pixels (docs/type-tool.md §7 -- "Type sizes"). Tracking (1/1000
// em) and ratio leading scale with the size automatically, so they are untouched. factor<=0 is a
// no-op. Keeps the invariant.
void scaleTextSizes(TextBlock& block, float factor, float minSizePx = 4.0f,
                    float maxSizePx = 4000.0f);

// Replace the bytes [begin, end) of block.utf8 with `insert` (the editing primitive behind typing,
// paste, backspace and delete -- §6). Existing run boundaries shift by the edit; the inserted span
// takes `style` (default: the style in effect at `begin`, so typing inherits the run to its left);
// the paragraph list splices so that newlines added/removed split/merge paragraphs (the surviving
// paragraph keeps the FIRST affected paragraph's style). begin/end are clamped to [0, utf8.size())
// and ordered. Keeps the invariant (callers need not normalize after). Returns the byte offset just
// past the inserted text -- the natural new caret position (callers may ignore it).
std::size_t replaceText(TextBlock& block, std::size_t begin, std::size_t end,
                        std::string_view insert, std::optional<CharStyle> style = std::nullopt);

}  // namespace mosaic::core::text
