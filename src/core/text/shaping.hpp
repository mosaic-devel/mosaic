#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/text/font_provider.hpp"
#include "core/text/text_model.hpp"
#include "core/vector/geometry.hpp"  // vec::Contours

// Shaping + layout: TextBlock -> positioned glyphs -> the shared Contours seam (docs/type-tool.md
// §5.1). HarfBuzz shapes each run (complex scripts, ligatures, mark positioning); FreeType opens
// the faces and hands out outlines; the layout breaks lines, applies leading/alignment, and runs
// per-codepoint font fallback so emoji/CJK in an otherwise-Latin run resolve. FreeType/HarfBuzz are
// kept PRIVATE to shaping.cpp behind a PImpl, exactly as lcms2 is private to color_management --
// nothing here drags those libraries into core's public surface.
//
// A ShapedBlock is the GPU-residency cache unit (§5.4): it is invalidated only by a text/run/area/
// font change, never by zoom/move/recolour, so a future GPU renderer uploads it once.
namespace mosaic::core::text {

using common::Vec2;

// Inside padding for Area (frame) text, in layer units: the text is laid out this far from every box
// edge so the caret/glyphs never touch the wrapping-box frame (round-4 #4). Point text (no box) is
// unaffected. Shared so the empty-block caret (text_edit) insets to match the laid-out text (shaping).
inline constexpr float kAreaInset = 3.0f;

// Total baseline-arc sweep in radians at |TextBlock::bend| == 1 (~137 degrees). Shared by applyBend
// (shaping.cpp) and the canvas box chrome, so a frame edge bowed for a given bend value carries
// exactly the text's curvature (the old approximating parabola overshot the sag by up to 2x).
inline constexpr double kBendMaxSweep = 2.4;

// One positioned glyph in layer-local em space (y-down, baseline-relative origin at `pen`). Carries
// the face actually used (base or a fallback) so the outline / colour render can re-load it.
struct ShapedGlyph {
    std::uint32_t glyphId = 0;   // FACE glyph index (not a Unicode codepoint)
    std::size_t runIndex = 0;    // index into TextBlock::runs -> the style/paint to fill with
    std::size_t cluster = 0;     // byte offset into TextBlock::utf8 (caret/selection mapping, S29-b)
    std::size_t line = 0;        // index into ShapedBlock::lines
    Vec2 pen{0, 0};              // glyph origin (on the baseline) in layer-local space
    float advance = 0.0f;        // pen advance after this glyph (caret stepping / decoration spans)
    FontFace face;               // the resolved face this glyph came from
    float sizePx = 0.0f;         // em size from the run style
    bool colorGlyph = false;     // a COLR/CPAL or embedded-bitmap glyph (painted by the render stage)
    bool whitespace = false;     // space/separator: laid out (advances the pen) but emits no ink
    bool rotated = false;        // vertical `mixed` Latin/digit: the outline is turned 90 CW about the
                                 // pen (glyphContours), so the row-set glyph reads down the column (B3)
    float baselineAngle = 0.0f;  // radians: rotate the outline about `pen` for a bent/on-path baseline
                                 // (S30, §9). 0 for flat text. Composes with `pen` already carrying the
                                 // arc position; glyphContours applies it. Horizontal-only (never with
                                 // `rotated`). Editing geometry reads it so the caret rides the curve.
};

// One laid-out line: a half-open range of ShapedBlock::glyphs plus its baseline and box metrics.
// The layout is written in an axis-abstract frame (see LayoutBasis in shaping.cpp): glyphs advance
// along the INLINE axis, lines stack along the BLOCK axis. In the only mode shipping today,
// HorizontalTB, inline = +x and block = +y, so the fields below carry their literal horizontal
// meaning. The vertical modes (B2) reuse the same fields with the axes rotated -- `baselineY` becomes
// the line's block-axis coordinate, `x`/`width` its inline-axis start/extent -- which is why editing
// geometry (text_edit) reads the block's writing mode rather than assuming these are screen x/y.
struct ShapedLine {
    std::size_t begin = 0, end = 0;  // [begin,end) into ShapedBlock::glyphs
    std::size_t paragraph = 0;       // index into TextBlock::paragraphs
    float baselineY = 0.0f;          // baseline coord on the block axis (horizontal: layer-local y)
    float ascent = 0.0f;             // max ascent from the baseline, ascent side (positive)
    float descent = 0.0f;            // max descent from the baseline, descent side (positive)
    float x = 0.0f;                  // inline-axis start after alignment (horizontal: left edge x)
    float width = 0.0f;              // content advance along the inline axis (trailing WS excluded)
};

struct ShapedBlock {
    std::vector<ShapedGlyph> glyphs;
    std::vector<ShapedLine> lines;
    // The writing mode this block was laid out in (copied from the source TextBlock). Downstream
    // geometry (caret/selection/hit-testing in text_edit, arrow-key nav) reads it to interpret the
    // axis-abstract ShapedLine fields. Always HorizontalTB until B2 wires the vertical layout.
    WritingMode writingMode = WritingMode::HorizontalTB;
    common::Rect bounds{};   // tight layer-local box of the laid-out text (metrics-based)
    float width = 0.0f;      // overall layout width (max line width, or areaSize.x for Area)
    float height = 0.0f;     // total height (top of first line to bottom of last)
    // The arched baseline (§9) applyBend lays the text along: a CIRCULAR arc, parameterised by distance
    // ALONG it (`s` = a glyph's cumulative advance from the line start, 0..W), so letters keep even
    // spacing on the curve instead of bunching where it flattens. `active` only when the block was bent.
    // Both the mesher/placement and the editing chrome (caret / selection ribbon / bar) sample the SAME
    // curve via pointAt, so they always agree. The arc has total length W and signed sweep `theta`
    // (radius R = W/theta, signed); theta>0 arches up (∩), <0 down (∪); its chord midpoint is (x0+W/2,
    // baseY).
    struct BentArc {
        float x0 = 0, baseY = 0, W = 0, theta = 0;
        bool active = false;

        // The baseline point at distance `s` along the arc (0..W), plus the tangent angle there.
        Vec2 pointAt(double s, double& angle) const {
            if (std::abs(theta) < 1e-4) {  // ~straight
                angle = 0.0;
                return {static_cast<float>(x0 + s), baseY};
            }
            const double R = W / theta;                 // signed radius
            const double xc = x0 + 0.5 * W;             // chord midpoint x
            const double beta = (s / W - 0.5) * theta;  // -theta/2 .. +theta/2
            angle = beta;                               // tangent angle
            const double cy = baseY + R * std::cos(theta * 0.5);  // circle centre y
            return {static_cast<float>(xc + R * std::sin(beta)),
                    static_cast<float>(cy - R * std::cos(beta))};
        }

        // Carry a FLAT layer-local point through the bend: the arc point at the same distance
        // along the arc as `flat`'s x sits from x0, stepped down the local NORMAL by `flat`'s own
        // depth below baseY. This IS the mapping applyBend places every glyph with, so everything
        // that goes through it -- the Area frame's corners and edges, the bowed bottom bar, the
        // rotate hotspots -- rides the same family of concentric arcs the letters do, and is the
        // exact inverse of sectorContains.
        // ⚠ An inactive or straight arc returns `flat` UNCHANGED, to the bit. The short-circuit is
        // load-bearing, not an optimisation: pointAt casts through float, so letting the identity
        // case fall through would drift unbent chrome by an ulp against the flat path it is
        // supposed to reproduce exactly.
        [[nodiscard]] Vec2 warp(Vec2 flat) const {
            if (!active || std::abs(theta) < 1e-4)
                return flat;
            double angle = 0.0;
            const Vec2 on = pointAt(static_cast<double>(flat.x) - x0, angle);
            const double d = static_cast<double>(flat.y) - baseY;
            return {on.x - std::sin(angle) * d, on.y + std::cos(angle) * d};
        }

        // Is `p` inside the annular SECTOR the arc sweeps down to `depth` -- the warped image of
        // the flat rect [x0, x0+W] x [baseY, baseY+depth] under the bend? This is pointAt's exact
        // inverse (p = pointAt(s) + N(s)·d has p - C = (R-d)(sin b, -cos b), so d and b read
        // straight off the polar form), which is what makes it the Area frame's CLIP TEST: a bent
        // box's overset is cut against the box the user actually sees, not the flat rect it no
        // longer is. Straight (|theta| ~ 0) reduces to the rect test exactly.
        [[nodiscard]] bool sectorContains(Vec2 p, double depth) const {
            if (std::abs(theta) < 1e-4)
                return p.x >= x0 && p.x <= x0 + W && p.y >= baseY && p.y <= baseY + depth;
            const double R = W / theta;  // signed radius
            const double cx = x0 + 0.5 * W;
            const double cy = baseY + R * std::cos(theta * 0.5);
            const double vx = p.x - cx, vy = p.y - cy;
            const double r = std::sqrt(vx * vx + vy * vy);
            if (r < 1e-9)
                return false;  // the arc centre: past every depth the sector can hold
            // p - C = (R - d)(sin b, -cos b): the radial coordinate gives the depth, the angular
            // one the arc distance. R - d keeps R's sign for any depth short of the fold.
            const double d = R > 0.0 ? R - r : R + r;
            if (d < 0.0 || d > depth)
                return false;
            const double beta = R > 0.0 ? std::atan2(vx, -vy) : std::atan2(-vx, vy);
            const double s = (beta / theta + 0.5) * W;
            return s >= 0.0 && s <= W;
        }
    };
    BentArc bentArc;
    // Fit-to-path (§9): set when applyPath laid the block along block.pathFit->baked. The chrome /
    // editing geometry map a glyph's FLAT advance x to a path arc-distance via originX (the flat
    // content's left edge): s = pathFit->s0 + (flatX - originX), reversed from s1 when flipped.
    // The path itself lives on the block (pathFit->baked), keeping ShapedBlock lean.
    struct PathRide {
        bool active = false;
        float originX = 0.0f;
        float flatW = 0.0f;  // the flat content's advance span (originX .. originX+flatW)
    };
    PathRide pathRide;
    // Caret height (ascent+descent) for an EMPTY block, measured from the pending style's actual face
    // -- so the blinking caret faithfully shows the size of the text that will be typed, instead of
    // jumping when the first glyph (whose caret spans the real font metrics) appears. Zero unless the
    // block is empty (a populated caret uses its line's ascent/descent). See caretGeometry.
    float emptyCaretHeight = 0.0f;
};

// Fit-to-path shared samplers (§9) -- the ONE mapping placement (applyPath) and every piece of
// editing chrome (caret / selection ribbon / bar / brackets) go through, so they always agree.
// pathArcDistance maps a glyph/caret's FLAT advance x (originX = the flat content's left edge,
// ShapedBlock::pathRide.originX) to an arc-distance along the baked path: from bracket s0 toward
// s1, reversed from s1 when flipped. samplePathBaseline samples the path there: a closed
// single-contour path WRAPS (text slides around a circle forever); an open path extends straight
// past its ends (overflow stays visible + editable). `angle` returns the local tangent angle,
// turned half a circle when flipped (the mirrored side reads along the reversed direction).
[[nodiscard]] double pathArcDistance(const PathFit& fit, double flatX, double originX);
[[nodiscard]] Vec2 samplePathBaseline(const PathFit& fit, double arcDistance, double& angle);

// The tight axis-aligned bounds of the annular sector `arc` sweeps down to `depth` -- the warped
// image of the flat rect [x0, x0+W] x [baseY, baseY+depth]. The bent Area frame's own extent:
// the cache clip clamps to it (a bent box's arch rises past the flat rect's top) and the rotate
// affordance wraps it. Straight (|theta| ~ 0) returns the flat rect exactly.
[[nodiscard]] common::Rect bentSectorBounds(const ShapedBlock::BentArc& arc, double depth);

// One variable-font axis a face exposes (docs/type-tool.md §3.4): the OpenType tag ("wght",
// "wdth", "opsz", customs), the font's display name for it, and the design range. The Type panel
// shows a slider per axis of the selected face; `wght`/`wdth` edits map to FontRef::weight/
// widthAxis (so they compose with bold matching), anything else to FontRef::variations[tag].
struct VariableAxis {
    std::string tag;   // 4-char OpenType axis tag
    std::string name;  // display name from the font's fvar/name table (falls back to the tag)
    float min = 0.0f;
    float def = 0.0f;  // the axis default (the value an unset FontRef renders at)
    float max = 0.0f;

    bool operator==(const VariableAxis&) const = default;
};

// Per-face decoration metrics (px at a given size), for underline / strikethrough rules.
struct DecorationMetrics {
    float underlineOffset = 0.0f;     // below baseline (positive down)
    float underlineThickness = 1.0f;
    float strikeoutOffset = 0.0f;     // above baseline (positive up)
    float strikeoutThickness = 1.0f;
};

// Owns the FreeType library + a face cache + the HarfBuzz plumbing. One per thread (FT_Library is
// not thread-safe across faces); the app keeps one on the UI thread, the render path borrows it.
class TextShaper {
public:
    TextShaper();
    ~TextShaper();
    TextShaper(TextShaper&&) noexcept;
    TextShaper& operator=(TextShaper&&) noexcept;
    TextShaper(const TextShaper&) = delete;
    TextShaper& operator=(const TextShaper&) = delete;

    // Shape + lay out `block`, resolving every run's font (and per-codepoint fallback) through
    // `fonts`. Honours alignment, leading, indents, tracking, baseline shift, and (Area) word wrap.
    [[nodiscard]] ShapedBlock layout(const TextBlock& block, const FontProvider& fonts);

    // The BCP-47 language used to hyphenate a paragraph that sets no language of its own (deferred
    // §1). Seeded from the OS locale; the app overrides it from the document/Settings default. Empty
    // disables the fallback (a paragraph must then name its own language to hyphenate).
    void setDefaultLanguage(std::string language);

    // The flattened, positioned outline of one shaped glyph in layer-local space (y-down), ready to
    // hand to the vector fill rasterizer. Empty for whitespace and colour glyphs.
    [[nodiscard]] vec::Contours glyphContours(const ShapedGlyph& g, double tolerancePx = 0.25);

    // The same outline BEFORE flattening: the glyph's own cubic Beziers, positioned in layer-local
    // space. This is what Layer -> Convert to Path emits, and why converted text opens in the Pen
    // tool (S28) as a handful of smooth nodes rather than as a thousand sampled corners.
    // FreeType hands us quadratics for TrueType outlines and cubics for CFF; a quadratic elevates to
    // a cubic EXACTLY, so nothing here is an approximation. Empty for whitespace and colour glyphs
    // (a COLR/CPAL or bitmap glyph has no outline to convert -- the caller reports the skip).
    [[nodiscard]] vec::Path glyphPath(const ShapedGlyph& g);

    // Underline/strikeout placement for a face at a size (drawn by the render stage as rectangles).
    [[nodiscard]] DecorationMetrics decorationMetrics(const FontFace& face, float sizePx);

    // The variable axes `face` exposes (fvar), hidden axes filtered out; empty for a static face.
    // Cached per (path, index) -- axes are face-static -- so the panel can ask on every reflect.
    [[nodiscard]] std::vector<VariableAxis> variableAxes(const FontFace& face);

    // (Render stage, S29-a stage 4) Rasterize a colour glyph (COLR/CPAL layers or an embedded
    // bitmap) into a straight-alpha RGBA tile + its top-left layer-local offset. nullopt if the
    // glyph has no colour data. Defined in text_render.cpp's companion so the colour path lives
    // with the rest of rasterization, but declared here because it needs the PImpl's faces.
    struct ColorGlyphTile {
        common::Image rgba;  // straight-alpha pixels
        Vec2 origin{0, 0};   // layer-local position of the tile's top-left
        float pixelScale = 1.0f;  // tile pixels per layer unit (bitmap glyphs may differ from sizePx)
    };
    [[nodiscard]] std::optional<ColorGlyphTile> colorGlyphTile(const ShapedGlyph& g);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace mosaic::core::text
