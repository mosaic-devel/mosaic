#include "core/text/text_edit.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

// The pure caret/selection + geometry machinery behind on-canvas text editing (§6). All byte<->inline
// mapping assumes LTR logical order within a line (HarfBuzz clusters monotonically non-decreasing),
// which is exact for Latin/LTR text; RTL/bidi visual reordering of the caret is a later round (the
// model already carries Paragraph::direction). Geometry is writing-mode aware: the byte<->coordinate
// maps work on the INLINE axis (x for horizontal, y down the column for vertical) and the results
// are projected into layer space through EditBasis below. Nothing here touches FreeType/HarfBuzz or
// FLTK.
namespace mosaic::core::text {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kHalfPi = 1.5707963267948966;

bool isContinuation(unsigned char b) { return (b & 0xC0) == 0x80; }

// Decode the codepoint at byte `i` (assumed a boundary); returns the codepoint (U+FFFD on garbage).
char32_t decodeAt(std::string_view s, std::size_t i) {
    if (i >= s.size()) return 0;
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const std::size_t n = s.size();
    auto cont = [&](std::size_t k) { return k < n && isContinuation(static_cast<unsigned char>(s[k])); };
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && cont(i + 1))
        return (char32_t(c & 0x1F) << 6) | (s[i + 1] & 0x3F);
    if ((c & 0xF0) == 0xE0 && cont(i + 1) && cont(i + 2))
        return (char32_t(c & 0x0F) << 12) | (char32_t(s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
    if ((c & 0xF8) == 0xF0 && cont(i + 1) && cont(i + 2) && cont(i + 3))
        return (char32_t(c & 0x07) << 18) | (char32_t(s[i + 1] & 0x3F) << 12) |
               (char32_t(s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
    return 0xFFFD;
}

enum class CharClass { Space, Word, Punct };
CharClass classOf(char32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') return CharClass::Space;
    if ((cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
        cp == '_' || cp >= 0x80)
        return CharClass::Word;  // treat all non-ASCII as word-forming (CJK/accented letters)
    return CharClass::Punct;
}

// The codepoint class of the character ending at byte `i` (i.e. just left of offset `i`).
CharClass classBefore(std::string_view s, std::size_t i) {
    return classOf(decodeAt(s, prevCharBoundary(s, i)));
}

// ---- line byte ranges & x-mapping ----------------------------------------------------------------

struct LineInfo {
    std::size_t byteStart = 0;
    std::size_t byteEnd = 0;
};

std::vector<std::pair<std::size_t, std::size_t>> paragraphRanges(const std::string& s) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t nl = s.find('\n', start);
        const std::size_t end = nl == std::string::npos ? s.size() : nl;
        out.emplace_back(start, end);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

std::vector<LineInfo> lineByteRanges(const ShapedBlock& sh, const TextBlock& block) {
    const auto paras = paragraphRanges(block.utf8);
    std::vector<LineInfo> infos(sh.lines.size());
    auto paraOf = [&](std::size_t p) {
        return p < paras.size() ? paras[p] : std::make_pair(std::size_t{0}, block.utf8.size());
    };
    auto firstCluster = [&](std::size_t li) -> std::size_t {
        const ShapedLine& ln = sh.lines[li];
        if (ln.begin < ln.end) return sh.glyphs[ln.begin].cluster;  // LTR: lowest cluster
        return paraOf(ln.paragraph).first;
    };
    for (std::size_t li = 0; li < sh.lines.size(); ++li) {
        const ShapedLine& ln = sh.lines[li];
        const auto pr = paraOf(ln.paragraph);
        const bool firstInPara = li == 0 || sh.lines[li - 1].paragraph != ln.paragraph;
        const bool lastInPara =
            li + 1 >= sh.lines.size() || sh.lines[li + 1].paragraph != ln.paragraph;
        infos[li].byteStart = firstInPara ? pr.first : infos[li - 1].byteEnd;
        infos[li].byteEnd = lastInPara ? pr.second : firstCluster(li + 1);
    }
    return infos;
}

// The line index whose byte range contains `pos`, preferring the LATER line at a shared boundary
// (so a soft-wrap caret shows at the start of the next visual line -- click-consistent).
std::size_t lineOf(const std::vector<LineInfo>& infos, std::size_t pos) {
    std::size_t li = 0;
    bool found = false;
    for (std::size_t k = 0; k < infos.size(); ++k)
        if (pos >= infos[k].byteStart && pos <= infos[k].byteEnd) {
            li = k;
            found = true;
        }
    if (!found && !infos.empty()) li = infos.size() - 1;
    return li;
}

// Layer-local x of the caret for byte `pos` on line `li` (pos assumed within that line's range).
double caretXInLine(const ShapedBlock& sh, const std::vector<LineInfo>& infos, std::size_t li,
                    std::size_t pos) {
    const ShapedLine& ln = sh.lines[li];
    double adv = ln.x;
    for (std::size_t i = ln.begin; i < ln.end; ++i) {
        const ShapedGlyph& g = sh.glyphs[i];
        const std::size_t c = g.cluster;
        const std::size_t nextC = i + 1 < ln.end ? sh.glyphs[i + 1].cluster : infos[li].byteEnd;
        if (pos <= c) return adv;  // caret sits before this glyph
        if (pos < nextC) {         // inside a multi-byte / ligature cluster: interpolate
            const double frac = nextC > c ? double(pos - c) / double(nextC - c) : 0.0;
            return adv + frac * g.advance;
        }
        adv += g.advance;
    }
    return adv;  // at/after the line's last glyph
}

// The byte offset on line `li` nearest layer-local x `x` (click / vertical-move column landing).
std::size_t byteAtX(const ShapedBlock& sh, const std::vector<LineInfo>& infos, std::size_t li,
                    double x) {
    const ShapedLine& ln = sh.lines[li];
    if (ln.begin == ln.end || x <= ln.x) return infos[li].byteStart;
    double adv = ln.x;
    for (std::size_t i = ln.begin; i < ln.end; ++i) {
        const ShapedGlyph& g = sh.glyphs[i];
        if (x < adv + g.advance * 0.5) return g.cluster;  // left half -> before this glyph
        adv += g.advance;
    }
    return infos[li].byteEnd;
}

float blockEm(const TextBlock& block) {
    // An empty block has no runs, so the caret height comes from the pending style the block was
    // created with (the chosen size), not a hardcoded default (fixlist #3).
    return block.runs.empty() ? block.emptyStyle.sizePx : block.runs.front().style.sizePx;
}

// ---- bent baseline (§9) --------------------------------------------------------------------------
// A bent block's glyphs carry a WARPED pen + a per-glyph baselineAngle (shaping.applyBend), while the
// ShapedLine boxes stay flat. So the geometry helpers below ride the glyph pens/angles instead of the
// flat line frame, and the caret/hit/selection follow the arch. Bend is horizontal-only, matching
// applyBend's own gate; an extruded block bends too (the canvas projects this bent geometry through
// the ExtrudePlaneMap afterwards, so the chrome hugs the bent solid).
bool isBent(const TextBlock& block) {
    return block.bend != 0.0f && block.writingMode == WritingMode::HorizontalTB;
}

// Clamp a perpendicular offset from the bent baseline so it never reaches the arc's CENTRE. The
// centre sits at signed offset R = W/theta along the normal; an inward offset at/past it folds the
// offset curve back over itself -- the selection ribbon's quads then self-cross ("bowties") and the
// present shader's convex-fill test culls them, which is why the highlight used to pinch into a fan
// and then vanish entirely as a DOWNWARD bend grew (the ascent side points at the centre there; the
// much smaller descent side made upward bends immune). Offsets stop just short of the centre
// instead, so the chrome pinches to the centre point -- exactly where the warped letters themselves
// collide -- and stays a valid strip.
double clampBendOffset(const ShapedBlock::BentArc& arc, double u) {
    if (!arc.active || std::abs(arc.theta) < 1e-4) return u;
    const double lim = 0.98 * (arc.W / arc.theta);  // signed radius, with a small margin
    return arc.theta < 0.0f ? std::max(u, lim) : std::min(u, lim);
}

// ---- fit-to-path (§9) ----------------------------------------------------------------------------
// True when applyPath laid the block along its baked path (the path-mode twin of isBent).
bool isPathFit(const ShapedBlock& shaped, const TextBlock& block) {
    return shaped.pathRide.active && block.pathFit && !block.pathFit->baked.empty() &&
           block.writingMode == WritingMode::HorizontalTB;
}

// The ONE flat-x -> curved-baseline mapping the editing geometry rides: the bend arc or the fitted
// path, whichever laid the block out (inactive for flat/vertical text). Sampling the same curve the
// placement used keeps caret / ribbon / hit-test / squiggles coincident with the warped letters.
struct CurveSampler {
    const ShapedBlock::BentArc* arc = nullptr;
    const PathFit* fit = nullptr;
    double originX = 0.0;
    // The curve's perpendicular reference: a line's rib offset is baselineY - refY. It is the SAME
    // reference the placement measured dPerp against -- the arc's own baseY (the first baseline for
    // Point, the FRAME TOP for Area), the first baseline for a fitted path. Reading it from the
    // line list here while the placement read it from the arc is how the caret floated off an Area
    // block's warped letters.
    double refY = 0.0;

    [[nodiscard]] bool active() const { return arc != nullptr || fit != nullptr; }
    [[nodiscard]] Vec2 at(double flatX, double& angle) const {
        if (fit != nullptr)
            return samplePathBaseline(*fit, pathArcDistance(*fit, flatX, originX), angle);
        return arc->pointAt(flatX - originX, angle);
    }
    // Perpendicular offsets fold at the bend arc's centre (clampBendOffset); a path's local
    // curvature is segment-wise unknown, so offsets pass through (sharp corners may pinch -- v1).
    [[nodiscard]] double clampOffset(double u) const {
        return arc != nullptr ? clampBendOffset(*arc, u) : u;
    }
};

CurveSampler curveSamplerFor(const ShapedBlock& shaped, const TextBlock& block) {
    CurveSampler s;
    if (isPathFit(shaped, block)) {
        s.fit = &*block.pathFit;
        s.originX = shaped.pathRide.originX;
        // applyPath measures dPerp from the first line's baseline (shaping.cpp), so its chrome does.
        s.refY = shaped.lines.empty() ? 0.0 : shaped.lines.front().baselineY;
    } else if (isBent(block) && shaped.bentArc.active) {
        s.arc = &shaped.bentArc;
        s.originX = shaped.bentArc.x0;
        // ... while applyBend measures from the arc's OWN baseY: the first baseline for Point
        // (where this changes nothing) but the FRAME TOP for Area.
        s.refY = shaped.bentArc.baseY;
    }
    return s;
}

// ---- writing-mode projection (the editing twin of shaping.cpp's LayoutBasis) ---------------------
// ShapedLine fields are axis-abstract (inline start/extent + a block-axis baseline; shaping.hpp), so
// the geometry helpers compute in that frame and project through this basis into layer space -- and
// invert the projection for hit-testing. HorizontalTB is the identity (inline = +x, block = +y); the
// horizontal call sites below keep their exact pre-vertical expressions so that output stays
// bit-identical. `anchor` mirrors the layout's blockAnchor: vertical-rl Area columns hang from the
// box's RIGHT edge (block coord 0 maps to layer x = areaSize.x); everything else anchors the block
// axis at the layer origin (vertical-rl Point grows leftward into negative x).
struct EditBasis {
    WritingMode mode = WritingMode::HorizontalTB;
    double anchor = 0.0;

    [[nodiscard]] bool vertical() const { return mode != WritingMode::HorizontalTB; }

    [[nodiscard]] Vec2 place(double inl, double blk) const {
        switch (mode) {
            case WritingMode::VerticalRL: return {anchor - blk, inl};
            case WritingMode::VerticalLR: return {blk, inl};
            case WritingMode::HorizontalTB:
            default: return {inl, blk};
        }
    }
    // Inverse projection of a layer-local point onto the two axes.
    [[nodiscard]] double inlineCoord(Vec2 p) const { return vertical() ? p.y : p.x; }
    [[nodiscard]] double blockCoord(Vec2 p) const {
        switch (mode) {
            case WritingMode::VerticalRL: return anchor - p.x;
            case WritingMode::VerticalLR: return p.x;
            case WritingMode::HorizontalTB:
            default: return p.y;
        }
    }
};

EditBasis basisOf(const ShapedBlock& shaped, const TextBlock& block) {
    EditBasis b;
    b.mode = shaped.writingMode;
    // Mirrors layout(): only a vertical-rl AREA box (one with a real inline budget -- its height)
    // anchors block coord 0 away from the origin, at the box's right edge.
    if (b.mode == WritingMode::VerticalRL && block.frame == TextFrame::Area &&
        block.areaSize.y > 0.0)
        b.anchor = block.areaSize.x;
    return b;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// UTF-8 / byte stepping
// ---------------------------------------------------------------------------------------------
std::size_t nextCharBoundary(std::string_view utf8, std::size_t pos) {
    const std::size_t n = utf8.size();
    if (pos >= n) return n;
    std::size_t i = pos + 1;
    while (i < n && isContinuation(static_cast<unsigned char>(utf8[i]))) ++i;
    return i;
}

std::size_t prevCharBoundary(std::string_view utf8, std::size_t pos) {
    if (pos == 0) return 0;
    std::size_t i = std::min(pos, utf8.size()) - 1;
    while (i > 0 && isContinuation(static_cast<unsigned char>(utf8[i]))) --i;
    return i;
}

std::size_t nextWordBoundary(std::string_view utf8, std::size_t pos) {
    const std::size_t n = utf8.size();
    if (pos >= n) return n;
    std::size_t i = pos;
    const CharClass cls = classOf(decodeAt(utf8, i));
    while (i < n && classOf(decodeAt(utf8, i)) == cls) i = nextCharBoundary(utf8, i);
    while (i < n && classOf(decodeAt(utf8, i)) == CharClass::Space) i = nextCharBoundary(utf8, i);
    return i;
}

std::size_t prevWordBoundary(std::string_view utf8, std::size_t pos) {
    std::size_t i = std::min(pos, utf8.size());
    while (i > 0 && classBefore(utf8, i) == CharClass::Space) i = prevCharBoundary(utf8, i);
    if (i == 0) return 0;
    const CharClass cls = classBefore(utf8, i);
    while (i > 0 && classBefore(utf8, i) == cls) i = prevCharBoundary(utf8, i);
    return i;
}

TextSelection wordAt(std::string_view utf8, std::size_t pos) {
    const std::size_t n = utf8.size();
    if (n == 0) return {0, 0};
    const std::size_t base = pos < n ? pos : prevCharBoundary(utf8, n);
    const CharClass cls = classOf(decodeAt(utf8, base));
    std::size_t lo = base;
    while (lo > 0 && classBefore(utf8, lo) == cls) lo = prevCharBoundary(utf8, lo);
    std::size_t hi = nextCharBoundary(utf8, base);
    while (hi < n && classOf(decodeAt(utf8, hi)) == cls) hi = nextCharBoundary(utf8, hi);
    return {lo, hi};
}

std::size_t paragraphStart(std::string_view utf8, std::size_t pos) {
    pos = std::min(pos, utf8.size());
    const std::size_t nl = utf8.rfind('\n', pos == 0 ? 0 : pos - 1);
    return nl == std::string_view::npos ? 0 : nl + 1;
}

std::size_t paragraphEnd(std::string_view utf8, std::size_t pos) {
    pos = std::min(pos, utf8.size());
    const std::size_t nl = utf8.find('\n', pos);
    return nl == std::string_view::npos ? utf8.size() : nl;
}

// ---------------------------------------------------------------------------------------------
// Geometry & hit-testing
// ---------------------------------------------------------------------------------------------
std::size_t hitTest(const ShapedBlock& shaped, const TextBlock& block, Vec2 local) {
    if (shaped.lines.empty()) return 0;
    const auto infosAll = lineByteRanges(shaped, block);
    if ((isBent(block) || isPathFit(shaped, block)) && !shaped.glyphs.empty()) {
        // The flat line frame doesn't describe the arch/path, so land on the nearest warped caret
        // boundary: the left origin before each glyph, the trailing edge after each line's last glyph.
        double best = kInf;
        std::size_t bestByte = 0;
        for (std::size_t li = 0; li < shaped.lines.size(); ++li) {
            const ShapedLine& ln = shaped.lines[li];
            for (std::size_t i = ln.begin; i < ln.end; ++i) {
                const ShapedGlyph& g = shaped.glyphs[i];
                const double d = (local - g.pen).length();
                if (d < best) { best = d; bestByte = g.cluster; }
            }
            if (ln.begin < ln.end) {
                const ShapedGlyph& g = shaped.glyphs[ln.end - 1];
                const Vec2 tail{g.pen.x + std::cos(g.baselineAngle) * g.advance,
                                g.pen.y + std::sin(g.baselineAngle) * g.advance};
                const double d = (local - tail).length();
                if (d < best) { best = d; bestByte = infosAll[li].byteEnd; }
            }
        }
        return bestByte;
    }
    const EditBasis basis = basisOf(shaped, block);
    const auto infos = infosAll;
    // Nearest line on the BLOCK axis (row for horizontal, column for vertical), then the nearest
    // inter-glyph gap along the inline axis. blockCoord == local.y for horizontal (unchanged).
    const double blk = basis.blockCoord(local);
    std::size_t li = 0;
    double best = kInf;
    for (std::size_t k = 0; k < shaped.lines.size(); ++k) {
        const ShapedLine& ln = shaped.lines[k];
        const double top = ln.baselineY - ln.ascent;
        const double bot = ln.baselineY + ln.descent;
        const double d = blk < top ? top - blk : (blk > bot ? blk - bot : 0.0);
        if (d < best) {
            best = d;
            li = k;
        }
    }
    return byteAtX(shaped, infos, li, basis.inlineCoord(local));
}

CaretGeometry caretGeometry(const ShapedBlock& shaped, const TextBlock& block, std::size_t pos) {
    pos = std::min(pos, block.utf8.size());
    const EditBasis basis = basisOf(shaped, block);
    // The bar is always PERPENDICULAR to the flow: upright between glyphs for horizontal text,
    // lying across the column for vertical (place() rotates the same block-axis span).
    const double angle = basis.vertical() ? kHalfPi : 0.0;
    if (shaped.lines.empty()) {
        // Height = the pending style's real face ascent+descent (measured by the shaper) so the empty
        // caret matches the first glyph that will be typed; fall back to the raw em if unmeasured.
        const double h = shaped.emptyCaretHeight > 0.0f ? double(shaped.emptyCaretHeight)
                                                        : double(blockEm(block));
        // An EMPTY path-fitted block's caret already sits ON the path at the start bracket, tilted
        // to the local tangent -- where the first typed glyph will land -- instead of at the layer
        // origin (which for a click-created path block is nowhere near the path).
        if (block.pathFit && !block.pathFit->baked.empty() &&
            block.writingMode == WritingMode::HorizontalTB) {
            double a = 0.0;
            const Vec2 p = samplePathBaseline(*block.pathFit, block.pathFit->s0, a);
            const double ca = std::cos(a), sa = std::sin(a);
            const double uTop = -0.8 * h, uBot = 0.2 * h;  // ~ascent/descent split about the baseline
            return {{p.x - sa * uTop, p.y + ca * uTop}, {p.x - sa * uBot, p.y + ca * uBot}, a};
        }
        // An empty Area block's caret sits at the box's inset corner (matching where the first glyph
        // will land), so it doesn't touch the frame; Point text has no box, so it stays at the origin.
        // place() puts that corner top-left for horizontal, top-RIGHT for vertical-rl (where the
        // first column will hang), top-left again for vertical-lr.
        const double in = (block.frame == TextFrame::Area && block.areaSize.x > 0.0 &&
                           block.areaSize.y > 0.0)
                              ? static_cast<double>(kAreaInset)
                              : 0.0;
        return {basis.place(in, in), basis.place(in, in + h), angle};  // an inset caret bar
    }
    const auto infos = lineByteRanges(shaped, block);
    const std::size_t li = lineOf(infos, pos);
    const ShapedLine& ln = shaped.lines[li];
    const CurveSampler smp = curveSamplerFor(shaped, block);
    if (smp.active() && !shaped.glyphs.empty()) {
        // Ride the arch/path at the caret's advance-distance along it -- the SAME point the selection
        // ribbon samples there -- so the caret bar coincides exactly with the selection's end rib.
        double a = 0.0;
        const Vec2 p = smp.at(caretXInLine(shaped, infos, li, pos), a);
        const double ca = std::cos(a), sa = std::sin(a);
        const double dPerp = static_cast<double>(ln.baselineY) - smp.refY;
        // Offsets along the local normal N = (-sa, ca), centre-clamped like the selection ribbon's
        // ribs (clampOffset) so the caret bar ends exactly where the ribbon's end rib does.
        const double uTop = smp.clampOffset(dPerp - ln.ascent);
        const double uBot = smp.clampOffset(dPerp + ln.descent);
        return {{p.x - sa * uTop, p.y + ca * uTop}, {p.x - sa * uBot, p.y + ca * uBot}, a};
    }
    const double x = caretXInLine(shaped, infos, li, pos);
    return {basis.place(x, ln.baselineY - ln.ascent), basis.place(x, ln.baselineY + ln.descent),
            angle};
}

std::vector<common::Rect> selectionRects(const ShapedBlock& shaped, const TextBlock& block,
                                         std::size_t lo, std::size_t hi) {
    std::vector<common::Rect> out;
    if (lo >= hi || shaped.lines.empty()) return out;
    const EditBasis basis = basisOf(shaped, block);
    const auto infos = lineByteRanges(shaped, block);
    for (std::size_t li = 0; li < shaped.lines.size(); ++li) {
        const ShapedLine& ln = shaped.lines[li];
        const std::size_t a = std::max(lo, infos[li].byteStart);
        const std::size_t b = std::min(hi, infos[li].byteEnd);
        if (a > b) continue;
        const double top = ln.baselineY - ln.ascent;  // block-axis span of the line / column
        const double height = ln.ascent + ln.descent;
        if (a == b) {  // only an empty line wholly inside the selection earns a sliver
            if (ln.begin == ln.end && lo <= infos[li].byteStart && hi >= infos[li].byteEnd) {
                if (!basis.vertical())
                    out.push_back({ln.x, top, blockEm(block) * 0.25, height});
                else  // the sliver runs a quarter-em DOWN the empty column
                    out.push_back(common::Rect::fromCorners(
                        basis.place(ln.x, top),
                        basis.place(ln.x + blockEm(block) * 0.25, top + height)));
            }
            continue;
        }
        const double x0 = caretXInLine(shaped, infos, li, a);
        const double x1 = caretXInLine(shaped, infos, li, b);
        if (x1 > x0) {
            if (!basis.vertical())  // kept as the exact prior expression (bit-identical horizontal)
                out.push_back({x0, top, x1 - x0, height});
            else  // a column-run rect: inline span [x0,x1] projected across the column's cross extent
                out.push_back(common::Rect::fromCorners(basis.place(x0, top),
                                                        basis.place(x1, top + height)));
        }
    }
    return out;
}

std::vector<std::array<common::Vec2, 4>> selectionQuads(const ShapedBlock& shaped,
                                                        const TextBlock& block, std::size_t lo,
                                                        std::size_t hi) {
    std::vector<std::array<common::Vec2, 4>> out;
    if (lo >= hi || shaped.lines.empty()) return out;
    const CurveSampler smp = curveSamplerFor(shaped, block);
    if (!smp.active()) {  // flat / vertical: selectionRects turned into (axis-aligned) quads
        for (const common::Rect& r : selectionRects(shaped, block, lo, hi))
            out.push_back({common::Vec2{r.x, r.y}, common::Vec2{r.right(), r.y},
                           common::Vec2{r.right(), r.bottom()}, common::Vec2{r.x, r.bottom()}});
        return out;
    }
    // Curved (bend arc or fitted path): sample the SAME curve the placement laid the text along, by
    // flat advance-x, so the ribbon tracks the even-spaced letters and is a smooth strip (not one
    // facet per glyph). Per line: a "rib" (a perpendicular cross-bar ascent..descent) at each
    // sample, consecutive ribs joined into edge-sharing quads (no gaps). Lower lines ride the curve
    // offset perpendicular by their baseline delta. TL,TR,BR,BL, positive shoelace.
    const auto infos = lineByteRanges(shaped, block);
    for (std::size_t li = 0; li < shaped.lines.size(); ++li) {
        const ShapedLine& ln = shaped.lines[li];
        const std::size_t a = std::max(lo, infos[li].byteStart);
        const std::size_t bEnd = std::min(hi, infos[li].byteEnd);
        if (a >= bEnd) continue;
        // x0/x1 are the caret flat-x positions (the same measure the placement maps by), so the
        // ribbon covers the selected letters exactly and ends on the caret bar.
        const double x0 = caretXInLine(shaped, infos, li, a);
        const double x1 = caretXInLine(shaped, infos, li, bEnd);
        if (x1 <= x0) continue;
        const double dPerp = static_cast<double>(ln.baselineY) - smp.refY;
        // The rib's two ends as offsets along the local normal, centre-clamped (clampOffset) so no
        // rib ever folds past the bend arc's centre -- the fold made the quads self-cross and the
        // shader cull them (the highlight vanished at strong downward bends).
        const double uTop = smp.clampOffset(dPerp - ln.ascent);
        const double uBot = smp.clampOffset(dPerp + ln.descent);
        const int n = std::clamp(static_cast<int>((x1 - x0) / 4.0) + 1, 2, 96);  // ~4px per rib
        struct Samp { double px, py, ca, sa; };
        std::vector<Samp> s;
        s.reserve(static_cast<std::size_t>(n) + 1);
        for (int k = 0; k <= n; ++k) {
            const double xk = x0 + (x1 - x0) * (static_cast<double>(k) / n);
            double ang = 0.0;
            const Vec2 pt = smp.at(xk, ang);
            s.push_back({pt.x, pt.y, std::cos(ang), std::sin(ang)});
        }
        // Build each quad from a rib at sample k and k+1, each dilated OUTWARD along the tangent by
        // kOverlap, so consecutive quads overlap by 2·kOverlap and the AA seam at every shared rib
        // (which would otherwise sit at 50% coverage = a faint 1px line) is painted over.
        constexpr double kOverlap = 0.8;
        auto rib = [&](const Samp& p, double along) {
            const double ox = p.px + p.ca * along, oy = p.py + p.sa * along;
            return std::pair<Vec2, Vec2>{{ox - p.sa * uTop, oy + p.ca * uTop},
                                         {ox - p.sa * uBot, oy + p.ca * uBot}};
        };
        for (int k = 0; k < n; ++k) {
            // Dilate only the INTERNAL rib of each seam; the two OUTER ribs stay exactly on the caret
            // positions, so the selection doesn't poke past the caret at its ends (user 2026-07-04).
            const double aAlong = k == 0 ? 0.0 : -kOverlap;
            const double bAlong = k == n - 1 ? 0.0 : kOverlap;
            const auto A = rib(s[static_cast<std::size_t>(k)], aAlong);
            const auto B = rib(s[static_cast<std::size_t>(k) + 1], bAlong);
            out.push_back({A.first, B.first, B.second, A.second});
        }
    }
    return out;
}

std::size_t moveCaretVertical(const ShapedBlock& shaped, const TextBlock& block, std::size_t pos,
                              int dir, double desiredInline) {
    // Works wholly in (line index, inline coordinate) space, so it is writing-mode agnostic: for
    // vertical text "line" means COLUMN and the goal coordinate is the caret's layer-local y.
    if (shaped.lines.empty()) return pos;
    const auto infos = lineByteRanges(shaped, block);
    const std::size_t li = lineOf(infos, std::min(pos, block.utf8.size()));
    const long target = static_cast<long>(li) + dir;
    if (target < 0 || target >= static_cast<long>(shaped.lines.size())) return pos;
    const double x = desiredInline >= 0.0 ? desiredInline : caretXInLine(shaped, infos, li, pos);
    return byteAtX(shaped, infos, static_cast<std::size_t>(target), x);
}

std::size_t visualLineStart(const ShapedBlock& shaped, const TextBlock& block, std::size_t pos) {
    if (shaped.lines.empty()) return 0;
    const auto infos = lineByteRanges(shaped, block);
    return infos[lineOf(infos, std::min(pos, block.utf8.size()))].byteStart;
}

std::size_t visualLineEnd(const ShapedBlock& shaped, const TextBlock& block, std::size_t pos) {
    if (shaped.lines.empty()) return 0;
    const auto infos = lineByteRanges(shaped, block);
    return infos[lineOf(infos, std::min(pos, block.utf8.size()))].byteEnd;
}

}  // namespace mosaic::core::text
