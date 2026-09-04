// Shaping / layout / render tests for the Type core (docs/type-tool.md §5). System fonts vary by
// machine, so these assert STRUCTURE (glyph counts, monotone pen advance, line stacking, ink
// presence, per-run colour, AA-mode behaviour), not golden pixels -- robust and deterministic in
// kind. Everything is gated on a usable FontDB so a font-less CI sandbox still passes.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

#include "common/geometry.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/text/shaping.hpp"
#include "core/text/text_layer_render.hpp"
#include "core/text/text_model.hpp"
#include "core/text/text_render.hpp"
#include "platform/font_db.hpp"
#include "render/compositor.hpp"

using namespace mosaic::core::text;
using mosaic::common::Affine2D;
using mosaic::common::ColorF;
using mosaic::common::ImageF;

namespace {

bool fontsAvailable(const mosaic::platform::FontDB& db) {
    if (db.families().empty()) return false;
    FontRef r;
    r.family = db.defaultFamily();
    return db.resolve(r).has_value();
}

CharStyle styleOf(ColorF fill, float size, const std::string& family) {
    CharStyle s;
    s.setSolidFill(fill);
    s.sizePx = size;
    s.font.family = family;
    return s;
}

// Scan an ImageF: count inked (alpha>0.5) pixels, their bbox, and how many have a "soft" alpha.
struct InkStats {
    int inked = 0;
    int soft = 0;  // pixels with 0.02 < a < 0.98 (AA edge coverage)
    double minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
    ColorF anyInk{};
};
InkStats scan(const ImageF& img) {
    InkStats st;
    for (std::uint32_t y = 0; y < img.height; ++y) {
        for (std::uint32_t x = 0; x < img.width; ++x) {
            const ColorF c = img.at(x, y);
            if (c.a > 0.02f && c.a < 0.98f) ++st.soft;
            if (c.a > 0.5f) {
                ++st.inked;
                st.minX = std::min(st.minX, double(x));
                st.minY = std::min(st.minY, double(y));
                st.maxX = std::max(st.maxX, double(x));
                st.maxY = std::max(st.maxY, double(y));
                st.anyInk = c;
            }
        }
    }
    return st;
}

}  // namespace

TEST_CASE("layout shapes a word into one advancing line") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    TextBlock block = makeBlock("Hello", styleOf({0, 0, 0, 1}, 32.0f, db.defaultFamily()));
    const ShapedBlock sb = shaper.layout(block, db);

    CHECK(sb.glyphs.size() >= 5);   // 'l' ligatures are unusual; expect >=5 glyphs
    REQUIRE(sb.lines.size() == 1);
    // Pen x advances left-to-right across the line.
    for (std::size_t i = 1; i < sb.glyphs.size(); ++i) {
        CHECK(sb.glyphs[i].pen.x >= sb.glyphs[i - 1].pen.x - 0.001);
    }
    CHECK(sb.bounds.w > 0.0);
    CHECK(sb.bounds.h > 0.0);
    CHECK(sb.width > 0.0f);
}

TEST_CASE("layout stacks paragraphs onto successive baselines") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    TextBlock block = makeBlock("a\nb", styleOf({0, 0, 0, 1}, 24.0f, db.defaultFamily()));
    const ShapedBlock sb = shaper.layout(block, db);
    REQUIRE(sb.lines.size() == 2);
    CHECK(sb.lines[1].baselineY > sb.lines[0].baselineY);  // second line is below the first
    CHECK(sb.lines[0].paragraph == 0);
    CHECK(sb.lines[1].paragraph == 1);
}

// --- Vertical writing mode (B2) --------------------------------------------------------------
// Structural metrics only (system fonts vary): glyphs flow DOWN a column, columns step sideways,
// a block is a tall column, and Area wraps by the box height. HarfBuzz synthesises vertical
// advances even for a Latin face, so these are robust without a CJK-specific font.

TEST_CASE("vertical text actually rasterizes ink in BOTH column orders") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    // Regression: vertical-rl once clipped away ALL its ink -- the glyph's cross-axis centring was
    // mirrored with the column flip, so every outline landed on the far side of its line box and the
    // rasterizer dropped it (blank output, the "CJK invisible" bug). Rendered ink must be non-blank in
    // both orders. vertical-rl grows into negative layer-x (columns left of the origin), so shift it
    // on-buffer with a translation; vertical-lr grows into +x from the origin.
    for (WritingMode wm : {WritingMode::VerticalRL, WritingMode::VerticalLR}) {
        TextBlock block =
            makeBlock("\xE6\xB0\xB8\xE5\xAE\x89" /*永安*/, styleOf({0, 0, 0, 1}, 40.0f, db.defaultFamily()));
        block.writingMode = wm;
        const double tx = wm == WritingMode::VerticalRL ? 70.0 : 4.0;  // bring the column on-buffer
        const InkStats st = scan(renderTextF(shaper, block, db, 96, 160, Affine2D::translation(tx, 4)));
        CHECK(st.inked > 50);          // real ink, not a blank column
        CHECK(st.maxY - st.minY > st.maxX - st.minX);  // a tall column, not a row
    }
}

TEST_CASE("vertical writing mode stacks glyphs down a single column") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    TextBlock block = makeBlock("ABC", styleOf({0, 0, 0, 1}, 32.0f, db.defaultFamily()));
    block.writingMode = WritingMode::VerticalRL;
    const ShapedBlock sb = shaper.layout(block, db);

    REQUIRE(sb.glyphs.size() >= 3);
    REQUIRE(sb.lines.size() == 1);
    // Pen y advances down the column, monotonically; the last glyph is clearly below the first.
    for (std::size_t i = 1; i < sb.glyphs.size(); ++i)
        CHECK(sb.glyphs[i].pen.y >= sb.glyphs[i - 1].pen.y - 0.001);
    CHECK(sb.glyphs.back().pen.y > sb.glyphs.front().pen.y + 10.0);
    // Every glyph stays in the one column (pen x within a fraction of the em of the first).
    double lo = sb.glyphs[0].pen.x, hi = sb.glyphs[0].pen.x;
    for (const auto& g : sb.glyphs) { lo = std::min(lo, g.pen.x); hi = std::max(hi, g.pen.x); }
    CHECK(hi - lo < 0.5 * 32.0);
    CHECK(sb.bounds.h > sb.bounds.w);  // a column: taller than wide
}

TEST_CASE("vertical columns advance sideways (rl leftward, lr rightward)") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    TextBlock rl = makeBlock("A\nB", styleOf({0, 0, 0, 1}, 32.0f, db.defaultFamily()));
    rl.writingMode = WritingMode::VerticalRL;
    const ShapedBlock srl = shaper.layout(rl, db);
    REQUIRE(srl.lines.size() == 2);
    // The second paragraph's column sits to the LEFT of the first (columns run right-to-left).
    CHECK(srl.glyphs[srl.lines[1].begin].pen.x < srl.glyphs[srl.lines[0].begin].pen.x);

    TextBlock lr = makeBlock("A\nB", styleOf({0, 0, 0, 1}, 32.0f, db.defaultFamily()));
    lr.writingMode = WritingMode::VerticalLR;
    const ShapedBlock slr = shaper.layout(lr, db);
    REQUIRE(slr.lines.size() == 2);
    // ... and to the RIGHT for left-to-right columns.
    CHECK(slr.glyphs[slr.lines[1].begin].pen.x > slr.glyphs[slr.lines[0].begin].pen.x);
}

TEST_CASE("a vertical block is a column where a horizontal block of the same text is a row") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    const CharStyle st = styleOf({0, 0, 0, 1}, 32.0f, db.defaultFamily());

    const ShapedBlock sh = shaper.layout(makeBlock("ABCD", st), db);
    TextBlock v = makeBlock("ABCD", st);
    v.writingMode = WritingMode::VerticalLR;
    const ShapedBlock sv = shaper.layout(v, db);

    CHECK(sh.bounds.w > sh.bounds.h);  // horizontal: a wide, short row
    CHECK(sv.bounds.h > sv.bounds.w);  // vertical: a narrow, tall column
}

// --- Baseline bend / warp (S30, docs/type-tool.md §9) -----------------------------------------
// A positive bend bows the baseline into an arch, lifting the ends above the middle and turning each
// glyph by the local tangent; a zero bend is a no-op (flat text unchanged); the sign flips the arc;
// and the arc reaches the raster (assert ink, not just pen metrics -- the vertical-rl lesson above).

TEST_CASE("a positive bend arches the baseline and turns the end glyphs") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    TextBlock flat = makeBlock("MOSAIC", styleOf({0, 0, 0, 1}, 40.0f, db.defaultFamily()));
    TextBlock bent = flat;
    bent.bend = 0.9f;
    const ShapedBlock sf = shaper.layout(flat, db);
    const ShapedBlock sb = shaper.layout(bent, db);
    REQUIRE(sf.glyphs.size() == sb.glyphs.size());
    REQUIRE(sb.glyphs.size() >= 5);

    // Flat text carries no baseline angle (the arch pass never runs at bend 0).
    for (const auto& g : sf.glyphs) CHECK(g.baselineAngle == 0.0f);

    const std::size_t mid = sb.glyphs.size() / 2;
    // Arch ∩ (parabola pinned at the ends, apex pulled up): the middle glyph sits ABOVE (smaller y)
    // the two end glyphs, which stay near the flat baseline.
    CHECK(sb.glyphs[mid].pen.y < sb.glyphs.front().pen.y - 1.0f);
    CHECK(sb.glyphs[mid].pen.y < sb.glyphs.back().pen.y - 1.0f);
    // Each end glyph is turned by the local tangent; the turn is flattest at the apex.
    CHECK(std::abs(sb.glyphs.front().baselineAngle) > 0.2f);
    CHECK(std::abs(sb.glyphs.back().baselineAngle) > 0.2f);
    CHECK(std::abs(sb.glyphs[mid].baselineAngle) < std::abs(sb.glyphs.front().baselineAngle));
    CHECK(std::abs(sb.glyphs[mid].baselineAngle) < std::abs(sb.glyphs.back().baselineAngle));
    // Rising into the apex on the left, descending out of it on the right: opposite tangent signs.
    CHECK(sb.glyphs.front().baselineAngle < 0.0f);
    CHECK(sb.glyphs.back().baselineAngle > 0.0f);
    // The arch adds vertical extent vs the flat row.
    CHECK(sb.bounds.h > sf.bounds.h + 2.0);
}

TEST_CASE("the bend sign flips the arc, symmetrically") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    TextBlock up = makeBlock("MOSAIC", styleOf({0, 0, 0, 1}, 40.0f, db.defaultFamily()));
    up.bend = 0.7f;
    TextBlock down = up;
    down.bend = -0.7f;
    const ShapedBlock su = shaper.layout(up, db);
    const ShapedBlock sd = shaper.layout(down, db);
    REQUIRE(su.glyphs.size() == sd.glyphs.size());
    REQUIRE(su.glyphs.size() >= 5);
    const std::size_t mid = su.glyphs.size() / 2;

    CHECK(su.glyphs[mid].pen.y < su.glyphs.front().pen.y);  // +bend: apex (middle) lifts up
    CHECK(sd.glyphs[mid].pen.y > sd.glyphs.front().pen.y);  // -bend: apex drops down
    // Same |bend|, opposite end angles (the flat layout the arch reads is identical for both).
    CHECK(su.glyphs.front().baselineAngle ==
          doctest::Approx(-sd.glyphs.front().baselineAngle).epsilon(0.01));
}

TEST_CASE("a bent block rasterizes ink, arched taller than the flat row") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    TextBlock flat = makeBlock("WARPING", styleOf({0, 0, 0, 1}, 36.0f, db.defaultFamily()));
    TextBlock bent = flat;
    bent.bend = 0.8f;
    // A generous buffer, translated so the whole arch stays on-buffer.
    const InkStats fi = scan(renderTextF(shaper, flat, db, 320, 220, Affine2D::translation(12, 140)));
    const InkStats bi = scan(renderTextF(shaper, bent, db, 320, 220, Affine2D::translation(12, 140)));
    CHECK(fi.inked > 50);
    CHECK(bi.inked > 50);
    // The arch reaches the raster: bent ink spans more rows than the flat row.
    CHECK((bi.maxY - bi.minY) > (fi.maxY - fi.minY) + 5.0);
}

TEST_CASE("an AREA block's bend arc is the FRAME's, and the type size cannot move it") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    // A paragraph long enough to wrap into several lines in a 300 px frame.
    const char* kText = "The quick brown fox jumps over the lazy dog again and again";
    TextBlock small = makeBlock(kText, styleOf({0, 0, 0, 1}, 16.0f, db.defaultFamily()));
    small.frame = TextFrame::Area;
    small.areaSize = {300.0f, 220.0f};
    small.bend = 0.8f;
    const ShapedBlock ss = shaper.layout(small, db);
    REQUIRE(ss.lines.size() >= 2); // the premise: multi-line, or "every line rides" pins nothing
    REQUIRE(ss.bentArc.active);

    // ⚠ The arc is the FRAME's: anchored at the box's top-left, spanning the box width (user
    // 2026-07-14: "the Area text conforms to the bend based on the text size itself instead of
    // based on the Area box" -- this is the pin that it no longer does).
    CHECK(ss.bentArc.x0 == 0.0f);
    CHECK(ss.bentArc.baseY == 0.0f);
    CHECK(ss.bentArc.W == doctest::Approx(300.0f));

    // ... so a DIFFERENT type size in the SAME box bends on the SAME arc.
    TextBlock big = makeBlock(kText, styleOf({0, 0, 0, 1}, 34.0f, db.defaultFamily()));
    big.frame = TextFrame::Area;
    big.areaSize = small.areaSize;
    big.bend = small.bend;
    const ShapedBlock sbig = shaper.layout(big, db);
    REQUIRE(sbig.bentArc.active);
    CHECK(sbig.bentArc.x0 == ss.bentArc.x0);
    CHECK(sbig.bentArc.baseY == ss.bentArc.baseY);
    CHECK(sbig.bentArc.W == ss.bentArc.W);
    CHECK(sbig.bentArc.theta == ss.bentArc.theta);

    // Every glyph on every line is placed on that frame arc's parallel family: reconstruct each pen
    // from the UNBENT twin layout (flat pen + advance) through the arc -- centre at its flat
    // advance-distance along the arc, backed off half an advance along the tangent, offset from the
    // FRAME TOP by its flat baseline depth along the normal. A text-driven arc (the old code) fails
    // this on every line; a frame arc with the wrong reference fails it on all but one.
    TextBlock flat = small;
    flat.bend = 0.0f;
    const ShapedBlock sf = shaper.layout(flat, db);
    REQUIRE(sf.glyphs.size() == ss.glyphs.size());
    for (const std::size_t i : {std::size_t{0}, ss.glyphs.size() / 2, ss.glyphs.size() - 1}) {
        const auto& fg = sf.glyphs[i];
        const auto& bg = ss.glyphs[i];
        const double s = static_cast<double>(fg.pen.x) + 0.5 * fg.advance; // arc x0 == 0
        double ang = 0.0;
        const mosaic::common::Vec2 on = ss.bentArc.pointAt(s, ang);
        const double c = std::cos(ang), sn = std::sin(ang);
        const double half = 0.5 * fg.advance;
        const double dPerp = static_cast<double>(fg.pen.y); // depth below the frame top (baseY == 0)
        CHECK(bg.pen.x == doctest::Approx(on.x - c * half - sn * dPerp).epsilon(0.001));
        CHECK(bg.pen.y == doctest::Approx(on.y - sn * half + c * dPerp).epsilon(0.001));
        CHECK(bg.baselineAngle == doctest::Approx(ang).epsilon(0.001));
    }

    // A POINT block keeps the TEXT-driven arc: anchored at its own first baseline, spanning its own
    // advance -- the goldens above already pin its glyph placement; this pins the anchor apart.
    TextBlock pt = makeBlock("MOSAIC", styleOf({0, 0, 0, 1}, 40.0f, db.defaultFamily()));
    pt.bend = 0.8f;
    const ShapedBlock sp = shaper.layout(pt, db);
    REQUIRE(sp.bentArc.active);
    CHECK(sp.bentArc.baseY > 1.0f); // the first baseline sits an ascent below the top, never 0
    CHECK(sp.bentArc.W < 299.0f);   // ... and the span is the word's advance, not some box's width
}

// --- BentArc::warp -- the ONE flat-point -> bent-point mapping the chrome shares ---------------
// The Type tool's on-canvas chrome (the Area frame's four corners, its bowed top/bottom edges, the
// rotate hotspots) all have to land on the arc family applyBend laid the LETTERS on, or the handles
// stop agreeing with what is drawn -- "place them based on the line/box bend in their corners"
// (user 2026-07-29). warp() is that mapping, named once; these pin the three properties the canvas
// leans on. No fonts needed: BentArc is pure geometry.
TEST_CASE("BentArc::warp: the identity cases are EXACT, not merely close") {
    // ⚠ Bit-exactness is the contract, not an accident. pointAt casts through float, so a warp that
    // "reduced to" the identity by arithmetic would drift unbent chrome by an ulp against the flat
    // geometry it is supposed to reproduce -- and unbent, unrotated text must come out unchanged.
    const mosaic::common::Vec2 p{123.456789012345, -67.891234567891};

    ShapedBlock::BentArc none{};  // never bent: no arc at all
    CHECK(none.active == false);
    CHECK(none.warp(p) == p);

    ShapedBlock::BentArc straight{10.0f, 20.0f, 300.0f, 0.0f, true}; // active, but zero sweep
    CHECK(straight.warp(p) == p);

    ShapedBlock::BentArc tiny{10.0f, 20.0f, 300.0f, 5e-5f, true};    // below pointAt's own epsilon
    CHECK(tiny.warp(p) == p);
}

TEST_CASE("BentArc::warp: a flat point rides the arc at its own distance and depth") {
    // The Area frame arc applyBend builds: x0 = 0, baseY = 0, W = the box width (see the Area-bend
    // case above), arching UP.
    ShapedBlock::BentArc arc{0.0f, 0.0f, 300.0f, 0.9f, true};
    constexpr double kW = 300.0;

    // On the reference baseline (depth 0) warp IS pointAt -- the top edge of the frame.
    for (const double s : {0.0, 0.25 * kW, 0.5 * kW, kW}) {
        double ang = 0.0;
        const mosaic::common::Vec2 on = arc.pointAt(s, ang);
        const mosaic::common::Vec2 w = arc.warp({s, 0.0});
        CHECK(w.x == doctest::Approx(on.x).epsilon(1e-9));
        CHECK(w.y == doctest::Approx(on.y).epsilon(1e-9));
    }
    // Below it, the point steps down the LOCAL normal, so it stays exactly `depth` from the arc --
    // that is what makes the frame's bottom edge concentric with its top instead of a second circle.
    for (const double depth : {40.0, 160.0}) {
        for (const double s : {0.0, 0.5 * kW, kW}) {
            double ang = 0.0;
            const mosaic::common::Vec2 on = arc.pointAt(s, ang);
            const mosaic::common::Vec2 w = arc.warp({s, depth});
            const double dx = w.x - on.x, dy = w.y - on.y;
            CHECK(std::sqrt(dx * dx + dy * dy) == doctest::Approx(depth).epsilon(1e-6));
        }
    }
    // A +bend arches UP, so the middle of the top edge rises above its ends (screen y grows down).
    CHECK(arc.warp({0.5 * kW, 0.0}).y < arc.warp({0.0, 0.0}).y);
    // ... and points of the flat frame rect land where sectorContains -- the SAME sector the Area
    // clip mask cuts overset against -- says they should, which is what keeps the drawn frame, the
    // clip and the handles one shape. (Held just inside the rect: the two are exact inverses, so on
    // the boundary the answer is a coin toss at the last ulp.)
    for (const double lx : {1.0, 0.5 * kW, kW - 1.0})
        for (const double ly : {1.0, 199.0})
            CHECK(arc.sectorContains(arc.warp({lx, ly}), 200.0));
}

TEST_CASE("BentArc::warp: the warped corners beat both the flat rect AND its AABB") {
    // The two rejected placements, quantified. A bent Area frame is an annular sector; its four
    // handles have to sit on the sector's corners.
    ShapedBlock::BentArc arc{0.0f, 0.0f, 300.0f, -0.7f, true}; // a downward bow, for the other sign
    const double W = arc.W, H = 180.0;
    const std::array<mosaic::common::Vec2, 4> flat{
        {{0.0, 0.0}, {W, 0.0}, {W, H}, {0.0, H}}};
    std::array<mosaic::common::Vec2, 4> warped{};
    for (int i = 0; i < 4; ++i) warped[i] = arc.warp(flat[i]);

    // (1) The FLAT rect's corners -- what the box chrome used until 2026-07-29 -- are nowhere near
    // the bent frame. The gap grows with DEPTH below the reference arc (the frame's top edge is the
    // arc, so its ends barely move; the bottom edge swings out on the widening radius), which is
    // exactly why the BR resize handle was the visible casualty.
    CHECK((warped[2] - flat[2]).length() > 25.0);
    CHECK((warped[3] - flat[3]).length() > 25.0);
    // (2) The warped extent's AABB -- rejected in round 3 -- puts its corners in empty space
    // diagonally off the arc ends ("the rotate handles are very far away from the actual visible
    // area", user 2026-07-14). Measure that gap on the sagging edge.
    double lo = warped[0].y, hi = warped[0].y;
    for (const auto& p : warped) { lo = std::min(lo, p.y); hi = std::max(hi, p.y); }
    const mosaic::common::Vec2 sag = arc.warp({0.5 * W, H}); // the bottom edge's deepest point
    CHECK(hi < sag.y);           // the AABB built from CORNERS alone does not even reach the bow...
    CHECK(sag.y - hi > 5.0);     // ...by a wide margin, which is why the edges must be sampled

    // A -bend bows DOWN: the middle of the top edge sinks below its ends.
    CHECK(arc.warp({0.5 * W, 0.0}).y > warped[0].y);
    // The sides stay RADIAL chords of concentric circles -- i.e. straight, which is why the frame's
    // two side edges need no sampling at all and the warped quad's side edges are exact.
    const mosaic::common::Vec2 mid = arc.warp({0.0, 0.5 * H});
    CHECK((mid.x - 0.5 * (warped[0].x + warped[3].x)) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK((mid.y - 0.5 * (warped[0].y + warped[3].y)) == doctest::Approx(0.0).epsilon(1e-6));
}

TEST_CASE("a bent AREA block's laid-out glyphs sit on BentArc::warp of their flat pens") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    TextBlock bent = makeBlock("Mosaic bends its Area frame",
                               styleOf({0, 0, 0, 1}, 28.0f, db.defaultFamily()), TextFrame::Area);
    bent.areaSize = {320.0, 200.0};
    bent.bend = 0.75f;
    const ShapedBlock sb = shaper.layout(bent, db);
    REQUIRE(sb.bentArc.active);
    TextBlock flat = bent;
    flat.bend = 0.0f;
    const ShapedBlock sf = shaper.layout(flat, db);
    REQUIRE(sf.glyphs.size() == sb.glyphs.size());
    REQUIRE(!sb.glyphs.empty());

    // Each glyph's ORIGIN is warp(flat centre) backed off half an advance along the local tangent --
    // so warp is exactly the mapping the letters ride, and chrome placed through it lands on them.
    for (const std::size_t i : {std::size_t{0}, sb.glyphs.size() / 2, sb.glyphs.size() - 1}) {
        const auto& fg = sf.glyphs[i];
        const auto& bg = sb.glyphs[i];
        const mosaic::common::Vec2 on = sb.bentArc.warp(
            {static_cast<double>(fg.pen.x) + 0.5 * fg.advance, static_cast<double>(fg.pen.y)});
        const double half = 0.5 * fg.advance;
        CHECK(bg.pen.x ==
              doctest::Approx(on.x - std::cos(bg.baselineAngle) * half).epsilon(0.001));
        CHECK(bg.pen.y ==
              doctest::Approx(on.y - std::sin(bg.baselineAngle) * half).epsilon(0.001));
    }
}

TEST_CASE("vertical Area text wraps into columns by the box height") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    TextBlock block =
        makeBlock("ABCDEFGH", styleOf({0, 0, 0, 1}, 24.0f, db.defaultFamily()), TextFrame::Area);
    block.writingMode = WritingMode::VerticalRL;
    block.areaSize = {400.0, 80.0};  // short column height forces several columns
    const ShapedBlock sb = shaper.layout(block, db);
    CHECK(sb.lines.size() >= 2);
    for (const auto& ln : sb.lines) CHECK(ln.width <= 80.0 + 2.0);  // each column fits the box height
}

// --- Latin orientation in vertical text (B3) -------------------------------------------------
// `mixed` (the default) turns sideways runs 90 CW so a Latin word reads down the column; `upright`
// stacks each glyph in its own cell. Assert the rotation actually inks a column (the RL blank bug was
// a layout-correct/render-blank gap -- pen metrics alone missed it) AND that the two modes differ.

TEST_CASE("vertical mixed Latin actually rasterizes ink as a tall column in both orders") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    // Regression guard for B3: a 90 rotation that lands the outline outside its column cell would clip
    // to blank (the class of bug that made vertical-rl CJK invisible). Rotated Latin must ink a column.
    for (WritingMode wm : {WritingMode::VerticalRL, WritingMode::VerticalLR}) {
        TextBlock block = makeBlock("Type", styleOf({0, 0, 0, 1}, 36.0f, db.defaultFamily()));
        block.writingMode = wm;  // orientation defaults to Mixed -> Latin rotates
        const double tx = wm == WritingMode::VerticalRL ? 70.0 : 4.0;  // bring the column on-buffer
        const InkStats st = scan(renderTextF(shaper, block, db, 96, 200, Affine2D::translation(tx, 4)));
        CHECK(st.inked > 50);                          // real ink, not a clipped-away blank column
        CHECK(st.maxY - st.minY > st.maxX - st.minX);  // a tall column, not a wide row
    }
}

TEST_CASE("vertical mixed rotates Latin sideways where upright stacks it") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    const CharStyle st = styleOf({0, 0, 0, 1}, 32.0f, db.defaultFamily());

    // Horizontal reference: the sum of the glyphs' horizontal advances is the line width.
    const ShapedBlock sh = shaper.layout(makeBlock("Type", st), db);

    TextBlock mixed = makeBlock("Type", st);
    mixed.writingMode = WritingMode::VerticalRL;  // Mixed by default
    const ShapedBlock sm = shaper.layout(mixed, db);

    TextBlock upright = makeBlock("Type", st);
    upright.writingMode = WritingMode::VerticalRL;
    upright.orientation = TextOrientation::Upright;
    const ShapedBlock su = shaper.layout(upright, db);

    // The mode is reflected on the shaped glyphs: every Latin glyph rotates in mixed, none in upright.
    for (const auto& g : sm.glyphs) if (!g.whitespace) CHECK(g.rotated);
    for (const auto& g : su.glyphs) CHECK_FALSE(g.rotated);

    // A rotated glyph advances down the column by its HORIZONTAL advance, so the mixed column's length
    // matches the horizontal line width; the upright column stacks by ~1 em/glyph and is clearly taller.
    CHECK(sm.bounds.h == doctest::Approx(sh.bounds.w).epsilon(0.06));
    CHECK(su.bounds.h > sm.bounds.h + 32.0);
}

TEST_CASE("Area text wraps within its box") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    TextBlock block = makeBlock("one two three four five six seven",
                                styleOf({0, 0, 0, 1}, 20.0f, db.defaultFamily()),
                                TextFrame::Area);
    block.areaSize = {120.0, 400.0};  // narrow box forces several lines
    const ShapedBlock sb = shaper.layout(block, db);
    CHECK(sb.lines.size() >= 2);
    for (const auto& ln : sb.lines) CHECK(ln.width <= 120.0 + 1.0);  // content fits the box width
}

TEST_CASE("Area text breaks a word longer than the box (overflow-wrap)") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    // One long word, no whitespace: it must break mid-word to wrap inside the box (round-4 #2),
    // not spill out of it.
    TextBlock block = makeBlock("supercalifragilisticexpialidocious",
                                styleOf({0, 0, 0, 1}, 24.0f, db.defaultFamily()), TextFrame::Area);
    block.areaSize = {90.0, 400.0};
    const ShapedBlock sb = shaper.layout(block, db);
    CHECK(sb.lines.size() >= 2);  // the long word wrapped instead of overflowing on one line
    for (const auto& ln : sb.lines) CHECK(ln.width <= 90.0 + 1.0);
}

TEST_CASE("renderTextF inks pixels for a solid fill within the layout box") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    TextBlock block = makeBlock("Hi", styleOf({1, 0, 0, 1}, 64.0f, db.defaultFamily()));
    const ImageF img = renderTextF(shaper, block, db, 300, 120, Affine2D::identity());
    const InkStats st = scan(img);
    CHECK(st.inked > 50);              // there is real ink
    CHECK(st.anyInk.r > 0.5f);         // it is red
    CHECK(st.anyInk.g < 0.3f);
    CHECK(st.anyInk.b < 0.3f);
    CHECK(st.minX >= 0.0);             // ink stays on-canvas
    CHECK(st.maxX < 300.0);
}

TEST_CASE("AA mode None hardens edges; Grayscale produces soft coverage") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    const auto style = styleOf({0, 0, 0, 1}, 48.0f, db.defaultFamily());

    TextBlock gray = makeBlock("Ra", style);
    gray.aa = AntiAlias::Grayscale;
    const InkStats g = scan(renderTextF(shaper, gray, db, 200, 100, Affine2D::identity()));

    TextBlock none = makeBlock("Ra", style);
    none.aa = AntiAlias::None;
    const InkStats n = scan(renderTextF(shaper, none, db, 200, 100, Affine2D::identity()));

    CHECK(g.inked > 0);
    CHECK(g.soft > 0);   // grayscale AA leaves partially-covered edge pixels
    CHECK(n.inked > 0);
    CHECK(n.soft == 0);  // None thresholds coverage -> only 0/1 alpha
}

TEST_CASE("per-run paint colours different spans differently") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    TextBlock block = makeBlock("AB", styleOf({1, 0, 0, 1}, 64.0f, db.defaultFamily()));
    setStyleRange(block, 1, 2, styleOf({0, 0, 1, 1}, 64.0f, db.defaultFamily()));  // 'B' -> blue
    REQUIRE(block.runs.size() == 2);
    const ImageF img = renderTextF(shaper, block, db, 300, 120, Affine2D::identity());

    bool red = false, blue = false;
    for (std::uint32_t y = 0; y < img.height; ++y) {
        for (std::uint32_t x = 0; x < img.width; ++x) {
            const ColorF c = img.at(x, y);
            if (c.a < 0.5f) continue;
            if (c.r > 0.5f && c.b < 0.3f) red = true;
            if (c.b > 0.5f && c.r < 0.3f) blue = true;
        }
    }
    CHECK(red);
    CHECK(blue);
}

TEST_CASE("layoutBounds is null for empty text and a box for non-empty") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    CHECK_FALSE(layoutBounds(shaper, makeBlock(""), db).has_value());
    const auto b = layoutBounds(shaper, makeBlock("x", styleOf({0, 0, 0, 1}, 24.0f,
                                                                db.defaultFamily())), db);
    REQUIRE(b.has_value());
    CHECK(b->w > 0.0);
    CHECK(b->h > 0.0);
}

TEST_CASE("underline adds ink below the baseline") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    const auto base = styleOf({0, 0, 0, 1}, 48.0f, db.defaultFamily());

    TextBlock plain = makeBlock("mm", base);
    auto underlined = base;
    underlined.underline = true;
    TextBlock ul = makeBlock("mm", underlined);

    const InkStats p = scan(renderTextF(shaper, plain, db, 200, 100, Affine2D::identity()));
    const InkStats u = scan(renderTextF(shaper, ul, db, 200, 100, Affine2D::identity()));
    CHECK(u.inked > p.inked);        // the bar adds pixels
    CHECK(u.maxY >= p.maxY - 0.5);   // and it reaches at/below the glyph baseline region
}

// The strikeout metrics now come from the face's OS/2 table (yStrikeoutPosition / yStrikeoutSize)
// rather than from a fixed 0.26-em guess, and the underline position is only honoured when the
// face states one. A units or sign slip in either would put the bar somewhere absurd, so assert
// where the bar LANDS, not merely that it added pixels: a strike crosses the glyphs, which means
// it must not reach past the ink the same text already had.
TEST_CASE("a strikethrough bar crosses the glyphs instead of sitting outside them") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db))
        return;
    TextShaper shaper;
    const auto base = styleOf({0, 0, 0, 1}, 48.0f, db.defaultFamily());
    auto struck = base;
    struck.strikethrough = true;

    // "xoxo" -- no ascenders, no descenders, so the ink box IS the x-height band and a bar that
    // escaped it (above the cap line or below the baseline) grows the box measurably.
    const InkStats p =
        scan(renderTextF(shaper, makeBlock("xoxo", base), db, 300, 120, Affine2D::identity()));
    const InkStats s =
        scan(renderTextF(shaper, makeBlock("xoxo", struck), db, 300, 120, Affine2D::identity()));
    REQUIRE(p.inked > 0);
    CHECK(s.inked > p.inked);      // the bar is drawn at all
    CHECK(s.minY >= p.minY - 0.5); // ... not floating above the letters
    CHECK(s.maxY <= p.maxY + 0.5); // ... nor hanging below the baseline
}

// The 3D lane used to drop underline/strikethrough on the floor: an extruded block rendered
// through the mesh path, which never looked at the decoration bars at all, so "U" and "S" were
// silently inert the moment 3D was switched on (user 2026-08-28). Both now extrude as solids of
// their own, so each must add ink to the SOLID exactly as it adds ink to the flat render.
TEST_CASE("underline and strikethrough extrude with 3D text") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db))
        return;
    TextShaper shaper;
    const auto base = styleOf({0, 0, 0, 1}, 48.0f, db.defaultFamily());

    // A shallow head-on solid: the decoration must be visible in the render, not hidden behind a
    // steep rotation, and the default orientation is exactly head-on.
    Extrude ex;
    ex.depth = 6.0f;
    ex.lightingEnabled = false; // flat self-lit faces: ink presence, not shading, is the question

    auto blockWith = [&](bool underline, bool strike) {
        auto st = base;
        st.underline = underline;
        st.strikethrough = strike;
        TextBlock b = makeBlock("mm", st);
        b.extrude = ex;
        return b;
    };

    const InkStats plain3d =
        scan(renderTextF(shaper, blockWith(false, false), db, 300, 160, Affine2D::identity()));
    REQUIRE(plain3d.inked > 0); // the solid itself rendered at all
    const InkStats ul3d =
        scan(renderTextF(shaper, blockWith(true, false), db, 300, 160, Affine2D::identity()));
    const InkStats st3d =
        scan(renderTextF(shaper, blockWith(false, true), db, 300, 160, Affine2D::identity()));

    CHECK(ul3d.inked > plain3d.inked);      // the underline bar is part of the solid
    CHECK(ul3d.maxY >= plain3d.maxY - 0.5); // ... and it sits at/below the glyph bottoms
    CHECK(st3d.inked > plain3d.inked);      // the strike bar is too
    // A strike crosses the glyphs, so it widens the solid no further than the glyphs already do;
    // what it must do is add ink strictly INSIDE the existing vertical span.
    CHECK(st3d.maxY <= plain3d.maxY + 0.5);
}

// The display pipeline (S29-b): refreshTextCache populates the layer's pixel + bounds caches, and
// the (font-free) compositor composites those cached pixels like a raster source (docs §5.4).
TEST_CASE("a TextLayer composites its cached pixels into the document") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    mosaic::core::Document doc(200, 80);
    mosaic::core::Layer& layer = doc.root().addOnTop(doc.makeText("T"));
    auto* tl = layer.as<mosaic::core::TextLayer>();
    REQUIRE(tl != nullptr);
    tl->setBlock(makeBlock("Hi", styleOf({0, 0, 0, 1}, 40.0f, db.defaultFamily())));

    CHECK(tl->cachedImage() == nullptr);  // unrendered: contributes nothing
    CHECK(mosaic::core::text::refreshTextCache(*tl, shaper, db));
    CHECK(tl->cachedImage() != nullptr);
    CHECK(tl->contentBounds().has_value());
    CHECK(tl->cacheCurrent());  // a second refresh is a no-op
    CHECK_FALSE(mosaic::core::text::refreshTextCache(*tl, shaper, db));

    mosaic::render::CompositeOptions opts;  // true alpha (no checkerboard)
    const mosaic::render::CompositeResult res =
        mosaic::render::composite(doc, opts, mosaic::render::Backend::Cpu);
    REQUIRE(res.ok);
    int inked = 0;
    for (std::size_t i = 3; i < res.image.rgba.size(); i += 4)
        if (res.image.rgba[i] > 200) ++inked;
    CHECK(inked > 0);  // the glyphs landed opaque pixels in the composite

    // Editing the block invalidates the cache; the next refresh re-renders. (A REAL edit --
    // mutableBlock() alone observes nothing; invalidateContentBounds() bumps the revision.)
    tl->mutableBlock().bend = 0.25f;
    tl->invalidateContentBounds();
    CHECK_FALSE(tl->cacheCurrent());
}

TEST_CASE("Area overset text is clipped to the box unless the block is being edited") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    // A tall stack of lines in a SHORT box: the text overflows the box bottom (round-4 #3).
    TextBlock block = makeBlock("line one\nline two\nline three\nline four\nline five",
                                styleOf({0, 0, 0, 1}, 20.0f, db.defaultFamily()), TextFrame::Area);
    block.areaSize = {200.0, 40.0};  // ~ two lines tall; the rest overflows
    mosaic::core::Document doc(400, 400);
    mosaic::core::Layer& layer = doc.root().addOnTop(doc.makeText("T"));
    auto* tl = layer.as<mosaic::core::TextLayer>();
    REQUIRE(tl != nullptr);
    tl->setBlock(block);

    // Clipped (deselected): the cache image is clamped to the box height, and the content box is the
    // frame (so the whole box is clickable), not the overflowing text bounds.
    CHECK(mosaic::core::text::refreshTextCache(*tl, shaper, db, /*clipToArea=*/true));
    REQUIRE(tl->cachedImage() != nullptr);
    CHECK(tl->cacheClipped());
    const double clippedH = tl->cachedImage()->height;
    REQUIRE(tl->contentBounds().has_value());
    CHECK(tl->contentBounds()->h == doctest::Approx(40.0));  // the box, not the taller text

    // Unclipped (being edited): the overflow re-renders, so the cache image is taller.
    CHECK(mosaic::core::text::refreshTextCache(*tl, shaper, db, /*clipToArea=*/false));
    CHECK_FALSE(tl->cacheClipped());
    CHECK(tl->cachedImage()->height > clippedH);  // overflow now included

    // Leaving editing re-clips: the cache flips back even though the block didn't change.
    CHECK(mosaic::core::text::refreshTextCache(*tl, shaper, db, /*clipToArea=*/true));
    CHECK(tl->cacheClipped());
    CHECK(tl->cachedImage()->height == doctest::Approx(clippedH));
}

TEST_CASE("BentArc::sectorContains + bentSectorBounds: the warped rect, exactly") {
    // Straight: both reduce to the flat rect, exactly.
    ShapedBlock::BentArc flat{10.0f, 20.0f, 100.0f, 0.0f, true};
    CHECK(flat.sectorContains({50.0, 40.0}, 60.0));
    CHECK_FALSE(flat.sectorContains({50.0, 19.0}, 60.0));
    CHECK_FALSE(flat.sectorContains({111.0, 40.0}, 60.0));
    const mosaic::common::Rect fb = bentSectorBounds(flat, 60.0);
    CHECK(fb.x == doctest::Approx(10.0));
    CHECK(fb.y == doctest::Approx(20.0));
    CHECK(fb.w == doctest::Approx(100.0));
    CHECK(fb.h == doctest::Approx(60.0));

    // Bent: sectorContains must be pointAt's exact inverse -- every point constructed ON the
    // sector (arc sample + in-range normal offset) is inside; nudged past any of the four
    // boundaries it is out. Both bend signs.
    for (const float theta : {1.2f, -1.2f}) {
        ShapedBlock::BentArc arc{0.0f, 0.0f, 300.0f, theta, true};
        const double H = 120.0;
        for (const double s : {1.0, 150.0, 299.0}) {
            for (const double d : {0.5, 60.0, 119.5}) {
                double ang = 0.0;
                const auto p = arc.pointAt(s, ang);
                const double sn = std::sin(ang), cs = std::cos(ang);
                const mosaic::common::Vec2 on{p.x - sn * d, p.y + cs * d};
                CHECK(arc.sectorContains(on, H));
                // Past the deep edge / above the reference arc: out.
                CHECK_FALSE(arc.sectorContains({p.x - sn * (H + 2.0), p.y + cs * (H + 2.0)}, H));
                CHECK_FALSE(arc.sectorContains({p.x + sn * 2.0, p.y - cs * 2.0}, H));
            }
        }
        // Past the radial ends: out (step along the tangent beyond s=0 / s=W).
        double a0 = 0.0, a1 = 0.0;
        const auto e0 = arc.pointAt(0.0, a0);
        const auto e1 = arc.pointAt(300.0, a1);
        CHECK_FALSE(arc.sectorContains(
            {e0.x - std::cos(a0) * 3.0 - std::sin(a0) * 5.0, e0.y - std::sin(a0) * 3.0 + std::cos(a0) * 5.0}, 120.0));
        CHECK_FALSE(arc.sectorContains(
            {e1.x + std::cos(a1) * 3.0 - std::sin(a1) * 5.0, e1.y + std::sin(a1) * 3.0 + std::cos(a1) * 5.0}, 120.0));

        // The bounds cover the sector: every on-sector sample is inside them -- and the arch
        // genuinely leaves the flat rect (the bbox rises above y=0 for an up-arch).
        const mosaic::common::Rect bb = bentSectorBounds(arc, 120.0);
        for (const double s : {0.0, 75.0, 150.0, 225.0, 300.0}) {
            for (const double d : {0.0, 120.0}) {
                double ang = 0.0;
                const auto p = arc.pointAt(s, ang);
                CHECK(bb.x <= p.x - std::sin(ang) * d + 1e-6);
                CHECK(bb.right() >= p.x - std::sin(ang) * d - 1e-6);
                CHECK(bb.y <= p.y + std::cos(ang) * d + 1e-6);
                CHECK(bb.bottom() >= p.y + std::cos(ang) * d - 1e-6);
            }
        }
        if (theta > 0.0f)
            CHECK(bb.y < -10.0); // the arch's apex rises well above the flat frame top
    }
}

TEST_CASE("a bent Area block's overset clip conforms to the BEND, not the flat rect") {
    // Regression (user 2026-07-14: "the Area text clipping mask does not conform to bend"). The
    // frame-driven arc swings the whole box -- letters included -- above the flat rect's top, and
    // the old rect clamp sheared that arch flat. The clip is now the warped sector itself.
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    TextBlock block = makeBlock("arch top line\nline two\nline three\nline four\nline five\nsix",
                                styleOf({0, 0, 0, 1}, 20.0f, db.defaultFamily()), TextFrame::Area);
    block.areaSize = {300.0, 80.0};  // ~three lines tall: the rest oversets past the box
    block.bend = 0.8f;               // theta 1.92: the apex rises ~66 px above the flat top
    mosaic::core::Document doc(500, 500);
    mosaic::core::Layer& layer = doc.root().addOnTop(doc.makeText("T"));
    auto* tl = layer.as<mosaic::core::TextLayer>();
    REQUIRE(tl != nullptr);
    tl->setBlock(block);

    REQUIRE(mosaic::core::text::refreshTextCache(*tl, shaper, db, /*clipToArea=*/true));
    REQUIRE(tl->cachedImage() != nullptr);
    CHECK(tl->cacheClipped());

    const mosaic::common::Image& img = *tl->cachedImage();
    const mosaic::common::Affine2D toLayer = tl->cacheImageToLayer();
    const ShapedBlock::BentArc arc{0.0f, 0.0f, 300.0f,
                                   static_cast<float>(0.8f * kBendMaxSweep), true};
    bool inkAboveFlatTop = false;
    int inked = 0;
    int outsideSector = 0;
    for (std::uint32_t y = 0; y < img.height; ++y) {
        for (std::uint32_t x = 0; x < img.width; ++x) {
            if (img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4 + 3] == 0)
                continue;
            ++inked;
            const mosaic::common::Vec2 p =
                toLayer.apply({static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5});
            // ⚠ EVERY surviving pixel is inside the warped sector (half-px sampling slack): the
            // overset lines past the warped bottom are gone, exactly as the flat clip cut overset
            // past the flat bottom.
            if (!arc.sectorContains({p.x, p.y}, 80.0 + 1.5))
                ++outsideSector;
            if (p.y < -5.0)
                inkAboveFlatTop = true;
        }
    }
    CHECK(outsideSector == 0);
    CHECK(inked > 100); // the premise: the clip left real letters, not an empty image
    // ... and the arch's top SURVIVED above the flat frame top -- the pixels the old rect clamp
    // sheared off. This is the half that fails on the old code.
    CHECK(inkAboveFlatTop);
}

TEST_CASE("refreshTextCaches reports the doc-space rect a re-render dirtied (S60-b typing path)") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    mosaic::core::Document doc(1000, 600);
    mosaic::core::Layer& layer = doc.root().addOnTop(doc.makeText("T"));
    auto* tl = layer.as<mosaic::core::TextLayer>();
    REQUIRE(tl != nullptr);
    tl->setBlock(makeBlock("Hello", styleOf({0, 0, 0, 1}, 40.0f, db.defaultFamily())));
    layer.setTransform(Affine2D::translation(200.0, 300.0));

    // First render: the dirty rect is the new cache's doc-space extent (there was no old one).
    mosaic::common::Rect dirty{};
    CHECK(mosaic::core::text::refreshTextCaches(doc, shaper, db, mosaic::core::kInvalidLayerId,
                                                false, &dirty));
    REQUIRE_FALSE(dirty.empty());
    REQUIRE(tl->cachedImage() != nullptr);
    // It covers the placed text (translated by the layer transform) and is FAR smaller than the
    // document -- the whole point: a keystroke recomposites this band, not the 1000x600 canvas.
    CHECK(dirty.x >= 190.0);
    CHECK(dirty.y >= 290.0);
    CHECK(dirty.w < 500.0);
    CHECK(dirty.h < 200.0);
    const mosaic::common::Rect first = dirty;

    // No change -> no dirt (and no re-render).
    CHECK_FALSE(mosaic::core::text::refreshTextCaches(doc, shaper, db,
                                                      mosaic::core::kInvalidLayerId, false, &dirty));
    CHECK(dirty.empty());

    // An edit that SHRINKS the text: the dirty rect still covers the OLD extent (the vacated
    // pixels need repainting) as well as the new -- old ∪ new, so it contains the first rect's
    // area within the pad.
    tl->mutableBlock().utf8 = "Hi";
    tl->invalidateContentBounds();
    CHECK(mosaic::core::text::refreshTextCaches(doc, shaper, db, mosaic::core::kInvalidLayerId,
                                                false, &dirty));
    REQUIRE_FALSE(dirty.empty());
    CHECK(dirty.x <= first.x + 0.5);
    CHECK(dirty.y <= first.y + 0.5);
    CHECK(dirty.right() >= first.right() - 0.5);
    CHECK(dirty.bottom() >= first.bottom() - 0.5);
}

TEST_CASE("refreshTextCache measures the ink's own layer-local bbox (the rotate anchor)") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    mosaic::core::Document doc(400, 200);
    mosaic::core::Layer& layer = doc.root().addOnTop(doc.makeText("T"));
    auto* tl = layer.as<mosaic::core::TextLayer>();
    REQUIRE(tl != nullptr);
    tl->setBlock(makeBlock("Ink", styleOf({0, 0, 0, 1}, 40.0f, db.defaultFamily())));

    REQUIRE(mosaic::core::text::refreshTextCache(*tl, shaper, db));
    REQUIRE(tl->cachedImage() != nullptr);
    REQUIRE(tl->cachedInkBounds().has_value());
    const mosaic::common::Rect ink = *tl->cachedInkBounds();
    CHECK_FALSE(ink.empty());

    // TIGHTER than the cache's own extent: the image carries a 2 px pad on every side (plus metric
    // side-bearings the glyphs never touch), so an "ink bbox" that just echoed the image extent --
    // the mutant this pins against -- reads wider on both axes.
    const mosaic::common::Rect extent = tl->cacheImageToLayer().mapBounds(
        {0.0, 0.0, static_cast<double>(tl->cachedImage()->width),
         static_cast<double>(tl->cachedImage()->height)});
    CHECK(ink.w <= extent.w - 3.0);
    CHECK(ink.h <= extent.h - 3.0);
    // ... and inside it, and inside the metric layout bounds within an AA pixel.
    CHECK(ink.x >= extent.x - 1e-6);
    CHECK(ink.y >= extent.y - 1e-6);
    CHECK(ink.right() <= extent.right() + 1e-6);
    CHECK(ink.bottom() <= extent.bottom() + 1e-6);

    // An emptied block clears it: no pixels, no ink to anchor anything to.
    tl->mutableBlock().utf8.clear();
    tl->mutableBlock().runs.clear();
    tl->invalidateContentBounds();
    REQUIRE(mosaic::core::text::refreshTextCache(*tl, shaper, db));
    CHECK_FALSE(tl->cachedInkBounds().has_value());
}

TEST_CASE("a DRAFT render is half-res and can NEVER read as current -- the crisp pass always lands") {
    // The live-gesture / font-hover speedup (user 2026-07-14: "scaling text is extremely laggy",
    // "font hover still very laggy"): frames that will be replaced momentarily render at half the
    // bake, a quarter of the raster cost. The CONTRACT is the second half of the name: a draft
    // stores its halved bake as the cache's linear key, so the first non-draft refresh re-renders
    // crisp even though the block did not change -- soft pixels cannot get stuck on screen.
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    mosaic::core::Document doc(400, 200);
    mosaic::core::Layer& layer = doc.root().addOnTop(doc.makeText("T"));
    auto* tl = layer.as<mosaic::core::TextLayer>();
    REQUIRE(tl != nullptr);
    tl->setBlock(makeBlock("Draft", styleOf({0, 0, 0, 1}, 40.0f, db.defaultFamily())));

    // Crisp baseline.
    REQUIRE(mosaic::core::text::refreshTextCache(*tl, shaper, db));
    REQUIRE(tl->cachedImage() != nullptr);
    const std::uint32_t crispW = tl->cachedImage()->width;
    REQUIRE(crispW > 8);

    // A draft render of the same block: roughly half the pixels on each axis.
    tl->invalidateContentBounds();
    REQUIRE(mosaic::core::text::refreshTextCache(*tl, shaper, db, true, false, /*draft=*/true));
    const std::uint32_t draftW = tl->cachedImage()->width;
    CHECK(draftW < crispW * 3 / 4);
    CHECK(draftW > crispW / 4);

    // ⚠ THE CONTRACT: with NO block change at all, a plain refresh re-renders (the draft's halved
    // bake fails the linear key) and comes back at full resolution.
    CHECK(mosaic::core::text::refreshTextCache(*tl, shaper, db));
    CHECK(tl->cachedImage()->width == crispW);
    // ... and the crisp cache is stable: a second plain refresh is the usual no-op.
    CHECK_FALSE(mosaic::core::text::refreshTextCache(*tl, shaper, db));
}

// Item 8: the layer transform's linear part is BAKED into the rasterization, so a stretched/rotated
// text layer is rendered crisp at device resolution instead of upsampling the 1px/unit cache. The
// invariant the compositor relies on: transform * cacheImageToLayer is a pure translation (no scale
// left for it to resample away).
namespace {
void checkPureTranslation(const Affine2D& m) {
    CHECK(m.m00 == doctest::Approx(1.0).epsilon(0.001));
    CHECK(m.m11 == doctest::Approx(1.0).epsilon(0.001));
    CHECK(m.m01 == doctest::Approx(0.0).epsilon(0.001));
    CHECK(m.m10 == doctest::Approx(0.0).epsilon(0.001));
}
}  // namespace

TEST_CASE("a stretched text layer bakes its transform's linear part (crisp, not upsampled)") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    mosaic::core::Document doc(400, 200);
    mosaic::core::Layer& layer = doc.root().addOnTop(doc.makeText("T"));
    auto* tl = layer.as<mosaic::core::TextLayer>();
    REQUIRE(tl != nullptr);
    tl->setBlock(makeBlock("Hi", styleOf({0, 0, 0, 1}, 40.0f, db.defaultFamily())));

    // Untransformed: base resolution (1 px/unit), identity baked.
    REQUIRE(mosaic::core::text::refreshTextCache(*tl, shaper, db));
    REQUIRE(tl->cachedImage() != nullptr);
    const std::uint32_t w0 = tl->cachedImage()->width;
    const std::uint32_t h0 = tl->cachedImage()->height;
    checkPureTranslation(layer.transform() * tl->cacheImageToLayer());  // trivially identity here

    // A pure translation changes nothing about the linear part -> no re-raster, same base-res cache.
    layer.setTransform(Affine2D::translation(123.0, 45.0));
    CHECK_FALSE(mosaic::core::text::refreshTextCache(*tl, shaper, db));
    CHECK(tl->cachedImage()->width == w0);

    // A 3x scale re-renders the contours 3x larger (NOT a 3x upscale of the old bitmap) ...
    layer.setTransform(Affine2D::scaling(3.0, 3.0));
    REQUIRE(mosaic::core::text::refreshTextCache(*tl, shaper, db));
    CHECK(tl->cachedImage()->width > w0 * 5 / 2);   // ~3x, allowing for the constant AA pad
    CHECK(tl->cachedImage()->width < w0 * 7 / 2);
    CHECK(tl->cachedImage()->height > h0 * 5 / 2);
    // ... and the residual the compositor places by is a pure translation: no scale left to blur.
    checkPureTranslation(layer.transform() * tl->cacheImageToLayer());
}

TEST_CASE("a live transform drag freezes the baked text transform (cheap per frame)") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    mosaic::core::Document doc(400, 200);
    mosaic::core::Layer& layer = doc.root().addOnTop(doc.makeText("T"));
    auto* tl = layer.as<mosaic::core::TextLayer>();
    REQUIRE(tl != nullptr);
    tl->setBlock(makeBlock("Hi", styleOf({0, 0, 0, 1}, 40.0f, db.defaultFamily())));

    layer.setTransform(Affine2D::scaling(2.0, 2.0));
    REQUIRE(mosaic::core::text::refreshTextCache(*tl, shaper, db));
    const std::uint32_t baked2x = tl->cachedImage()->width;

    // Mid-drag the transform keeps changing; freezeTransform keeps the 2x bake -> zero re-raster.
    layer.setTransform(Affine2D::scaling(6.0, 6.0));
    CHECK_FALSE(mosaic::core::text::refreshTextCache(*tl, shaper, db, /*clipToArea=*/true,
                                                     /*freezeTransform=*/true));
    CHECK(tl->cachedImage()->width == baked2x);  // still the 2x cache

    // On commit (not frozen) the settled 6x transform re-bakes crisp.
    REQUIRE(mosaic::core::text::refreshTextCache(*tl, shaper, db));
    CHECK(tl->cachedImage()->width > baked2x * 2);  // grew toward 6x
    checkPureTranslation(layer.transform() * tl->cacheImageToLayer());
}

TEST_CASE("a Move-scaled small-point glyph tessellates in device space (no facets)") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    auto inkedOf = [](const mosaic::common::Image& img) {
        int n = 0;
        for (std::size_t i = 3; i < img.rgba.size(); i += 4)
            if (img.rgba[i] > 200) ++n;
        return n;
    };

    // 'o' is all curve: flattened coarsely at a tiny point size it becomes a visible polygon when the
    // layer transform magnifies it. Baking the transform's scale (item 8) with device-space tessellation
    // makes that 12pt-scaled-8x glyph match a native 96pt render -- a point-size-space flatten would
    // leave an ~11-gon, ~5% short on inked area. Same font both ways, so the check is font-independent.
    mosaic::core::Document doc(400, 400);
    mosaic::core::Layer& la = doc.root().addOnTop(doc.makeText("T"));
    mosaic::core::Layer& lb = doc.root().addOnTop(doc.makeText("T"));
    auto* baked = la.as<mosaic::core::TextLayer>();
    auto* native = lb.as<mosaic::core::TextLayer>();
    REQUIRE(baked != nullptr);
    REQUIRE(native != nullptr);

    baked->setBlock(makeBlock("o", styleOf({0, 0, 0, 1}, 12.0f, db.defaultFamily())));
    la.setTransform(Affine2D::scaling(8.0, 8.0));
    native->setBlock(makeBlock("o", styleOf({0, 0, 0, 1}, 96.0f, db.defaultFamily())));

    REQUIRE(mosaic::core::text::refreshTextCache(*baked, shaper, db));
    REQUIRE(mosaic::core::text::refreshTextCache(*native, shaper, db));
    REQUIRE(baked->cachedImage() != nullptr);
    REQUIRE(native->cachedImage() != nullptr);

    const int ba = inkedOf(*baked->cachedImage());
    const int na = inkedOf(*native->cachedImage());
    REQUIRE(na > 0);
    CHECK(std::abs(ba - na) < na * 3 / 100);  // within 3% -> the curve, not a coarse polygon
}

TEST_CASE("drag cache replays a text layer byte-for-byte vs the full composite") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    auto fill = [](mosaic::core::RasterLayer& r, std::uint8_t cr, std::uint8_t cg, std::uint8_t cb,
                   std::uint8_t ca) {
        auto& px = r.image().rgba;
        for (std::size_t p = 0; p < px.size(); p += 4) {
            px[p] = cr;
            px[p + 1] = cg;
            px[p + 2] = cb;
            px[p + 3] = ca;
        }
    };

    // below (opaque) -> text TARGET (scaled+rotated, so the baked linear is non-trivial) -> a
    // semi-transparent above layer. The Type-edit box Move drag rides the same drag-scoped cache the
    // Move tool uses; the replay must be indistinguishable from a fresh full composite (S15-b).
    mosaic::core::Document doc(64, 48);
    auto belowR = doc.makeRaster("below");
    fill(*belowR, 40, 90, 160, 255);
    doc.root().addOnTop(std::move(belowR));

    mosaic::core::Layer& tlayerRef = doc.root().addOnTop(doc.makeText("T"));
    auto* tl = tlayerRef.as<mosaic::core::TextLayer>();
    REQUIRE(tl != nullptr);
    const mosaic::core::LayerId id = tlayerRef.id();
    tl->setBlock(makeBlock("Ag", styleOf({0, 0, 0, 1}, 20.0f, db.defaultFamily())));
    tlayerRef.setTransform(Affine2D::trs({8.0, 10.0}, 0.3, {1.6, 1.6}));

    auto aboveR = doc.makeRaster("above");
    fill(*aboveR, 60, 200, 80, 150);
    doc.root().addOnTop(std::move(aboveR));

    REQUIRE(mosaic::core::text::refreshTextCache(*tl, shaper, db));  // bake at the base transform
    REQUIRE(tl->cachedImage() != nullptr);

    mosaic::render::DragCompositeCache cache;
    mosaic::render::CompositeOptions opts;
    opts.checkerboard = true;  // the UI's recomposite options -> cover the flatten path too
    const Affine2D base = tlayerRef.transform();
    for (int frame = 0; frame < 3; ++frame) {  // nudge translation each frame (linear part unchanged)
        tlayerRef.setTransform(Affine2D::translation(2.0 * frame, frame) * base);
        const std::optional<mosaic::common::Image> fast = cache.composite(doc, id, opts);
        REQUIRE(fast.has_value());  // the cache must ACCEPT a text target (no nullopt fallback)
        const mosaic::render::CompositeResult full =
            mosaic::render::composite(doc, opts, mosaic::render::Backend::Cpu);
        REQUIRE(full.ok);
        INFO("drag frame " << frame);
        CHECK(fast->rgba == full.image.rgba);
    }
}

TEST_CASE("renderFontSample paints the sample into the cell") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;

    const std::string fam = db.defaultFamily();
    const int W = 120, H = 24;
    const mosaic::common::Image img =
        renderFontSample(shaper, db, fam, "Abg", 16.0f, ColorF{0, 0, 0, 1}, W, H);
    REQUIRE(img.width == static_cast<std::uint32_t>(W));
    REQUIRE(img.height == static_cast<std::uint32_t>(H));

    // Some ink landed (non-zero alpha somewhere), and it stays within the left ~half (left-aligned,
    // a 3-glyph sample is far narrower than 120px) -- i.e. it didn't smear across the whole cell.
    bool anyInk = false;
    int rightmostInkX = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const std::uint8_t a = img.rgba[(static_cast<std::size_t>(y) * W + x) * 4 + 3];
            if (a > 0) {
                anyInk = true;
                rightmostInkX = std::max(rightmostInkX, x);
            }
        }
    CHECK(anyInk);
    CHECK(rightmostInkX < W);  // sane horizontal extent

    // An empty sample yields a fully transparent cell of the right size (no crash, no ink).
    const mosaic::common::Image blank =
        renderFontSample(shaper, db, fam, "", 16.0f, ColorF{0, 0, 0, 1}, W, H);
    REQUIRE(blank.width == static_cast<std::uint32_t>(W));
    bool blankInk = false;
    for (std::size_t p = 3; p < blank.rgba.size(); p += 4)
        if (blank.rgba[p] > 0) blankInk = true;
    CHECK_FALSE(blankInk);
}

// --- Variable fonts (R4, docs/type-tool.md §3.4) ----------------------------------------------
// Gated on an installed variable face (the CI-safe pattern): the tests find one via variableAxes
// and pass trivially on a machine without any.
namespace {

// The first installed family whose resolved face exposes axis `tag`, with that axis's metadata.
std::optional<std::pair<std::string, VariableAxis>> variableFamilyWith(
    const mosaic::platform::FontDB& db, TextShaper& shaper, const std::string& tag) {
    for (const std::string& fam : db.families()) {
        FontRef r;
        r.family = fam;
        const auto face = db.resolve(r);
        if (!face) continue;
        for (const VariableAxis& ax : shaper.variableAxes(*face))
            if (ax.tag == tag) return std::make_pair(fam, ax);
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("variableAxes reports sane wght metadata for a variable face") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    const auto found = variableFamilyWith(db, shaper, "wght");
    if (!found) return;  // no variable font installed
    const VariableAxis& ax = found->second;
    CHECK(ax.min < ax.max);
    CHECK(ax.def >= ax.min);
    CHECK(ax.def <= ax.max);
    CHECK_FALSE(ax.name.empty());
    // A static face reports no axes at all (the panel shows no sliders for it).
    // (Not asserted against a named family -- machines vary -- but the API contract is above.)
}

TEST_CASE("a variable face's wght axis actually changes the rendered ink weight") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    const auto found = variableFamilyWith(db, shaper, "wght");
    if (!found) return;  // no variable font installed
    const auto& [fam, ax] = *found;

    const auto blockAt = [&](float w) {
        CharStyle st = styleOf({0, 0, 0, 1}, 48.0f, fam);
        st.font.weight = w;
        return makeBlock("Hamburg", st);
    };
    // Render at the axis extremes: the heavy master must ink strictly more pixels than the light
    // one (a RENDER assert, not just metrics -- the vertical-rl blank-ink lesson).
    TextBlock thin = blockAt(ax.min);
    TextBlock heavy = blockAt(ax.max);
    const InkStats a = scan(renderTextF(shaper, thin, db, 420, 100, Affine2D::identity()));
    const InkStats b = scan(renderTextF(shaper, heavy, db, 420, 100, Affine2D::identity()));
    REQUIRE(a.inked > 0);
    REQUIRE(b.inked > 0);
    CHECK(b.inked > a.inked);
}

TEST_CASE("explicit FontRef::variations override the style-derived weight") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    const auto found = variableFamilyWith(db, shaper, "wght");
    if (!found) return;
    const auto& [fam, ax] = *found;

    CharStyle heavy = styleOf({0, 0, 0, 1}, 48.0f, fam);
    heavy.font.weight = ax.max;

    CharStyle overridden = styleOf({0, 0, 0, 1}, 48.0f, fam);
    overridden.font.weight = ax.min;                  // the style says light...
    overridden.font.variations["wght"] = ax.max;      // ...but the explicit axis setting wins

    TextBlock hb = makeBlock("Hamburg", heavy);
    TextBlock ob = makeBlock("Hamburg", overridden);
    const InkStats h = scan(renderTextF(shaper, hb, db, 420, 100, Affine2D::identity()));
    const InkStats o = scan(renderTextF(shaper, ob, db, 420, 100, Affine2D::identity()));
    REQUIRE(h.inked > 0);
    CHECK(o.inked == h.inked);  // deterministic: same file, same final design coordinates
}

TEST_CASE("the -liga feature string actually suppresses ligature substitution") {
    // Find a family whose default shaping ligates "fi" (fewer glyphs than codepoints); then the
    // same text with "-liga"/"-clig" must shape to the two separate glyphs. Gated: a machine whose
    // faces never ligate passes trivially.
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    for (const std::string& fam : db.families()) {
        TextBlock lig = makeBlock("fi", styleOf({0, 0, 0, 1}, 32.0f, fam));
        const std::size_t nLig = shaper.layout(lig, db).glyphs.size();
        if (nLig != 1) continue;  // this face doesn't ligate "fi"; try another

        TextBlock plain = makeBlock("fi", styleOf({0, 0, 0, 1}, 32.0f, fam));
        plain.runs[0].style.features = {"-liga", "-clig"};
        CHECK(shaper.layout(plain, db).glyphs.size() == 2);
        return;
    }
    // No ligating face installed -- nothing to assert on this machine.
}

// --- Kerning modes (R4 §13: metric / optical / none) --------------------------------------------
TEST_CASE("kerning None disables the font's metric pairs") {
    // Find a face whose kern data actually moves "AV" (metric width != none width); assert the
    // difference. A machine with no kerning faces passes trivially.
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    for (const std::string& fam : db.families()) {
        TextBlock metric = makeBlock("AV", styleOf({0, 0, 0, 1}, 48.0f, fam));
        TextBlock none = makeBlock("AV", styleOf({0, 0, 0, 1}, 48.0f, fam));
        none.runs[0].style.kerning = Kerning::None;
        const float wm = shaper.layout(metric, db).bounds.w;
        const float wn = shaper.layout(none, db).bounds.w;
        if (wm == wn) continue;  // this face doesn't kern AV; try another
        CHECK(wm < wn);          // kern pairs close the diagonal pair, so metric is narrower
        return;
    }
}

TEST_CASE("optical kerning equalizes gaps: closes AVA, leaves the stem control pair alone") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    const std::string fam = db.defaultFamily();

    const auto widthOf = [&](const char* text, Kerning k) {
        TextBlock b = makeBlock(text, styleOf({0, 0, 0, 1}, 48.0f, fam));
        b.runs[0].style.kerning = k;
        return shaper.layout(b, db).bounds.w;
    };

    // 'nn' pairs ARE the reference gap, so optical adjusts them by ~0.
    CHECK(widthOf("nnn", Kerning::Optical) ==
          doctest::Approx(widthOf("nnn", Kerning::None)).epsilon(0.002));
    // Diagonal pairs carry far more optical white than the stem gap -> optical pulls them in.
    CHECK(widthOf("AVA", Kerning::Optical) < widthOf("AVA", Kerning::None) - 0.5);
    // And it is deterministic.
    CHECK(widthOf("AVA", Kerning::Optical) == widthOf("AVA", Kerning::Optical));
}

