// On-canvas text-editing core tests (docs/type-tool.md §6). The byte-level half (replaceText, UTF-8
// and word stepping, paragraph edges) is deterministic and font-free; the geometry half (hit-test,
// caret bar, selection rects, vertical motion) needs a laid-out block, so it is gated on a usable
// FontDB and asserts STRUCTURE (ordering, round-trips, line stacking), not golden pixels.
#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include "core/text/text_edit.hpp"
#include "core/text/text_model.hpp"
#include "platform/font_db.hpp"

using namespace mosaic::core::text;
using mosaic::common::ColorF;

namespace {

bool fontsAvailable(const mosaic::platform::FontDB& db) {
    if (db.families().empty()) return false;
    FontRef r;
    r.family = db.defaultFamily();
    return db.resolve(r).has_value();
}

CharStyle styled(ColorF c, float size = 24.0f) {
    CharStyle s;
    s.setSolidFill(c);
    s.sizePx = size;
    return s;
}

constexpr ColorF kRed{1, 0, 0, 1};
constexpr ColorF kGreen{0, 1, 0, 1};

}  // namespace

// ---------------------------------------------------------------------------------------------
// replaceText -- the editing primitive (deterministic)
// ---------------------------------------------------------------------------------------------
TEST_CASE("replaceText inserts and keeps the invariant") {
    TextBlock b = makeBlock("hello", styled(kRed));
    const std::size_t caret = replaceText(b, 5, 5, " world");
    CHECK(b.utf8 == "hello world");
    CHECK(caret == 11);
    CHECK(isValid(b));
    REQUIRE(b.runs.size() == 1);  // inserted text inherits the surrounding run -> one run
    CHECK(b.runs[0].style.solidFill() == kRed);
}

TEST_CASE("replaceText deletes a range") {
    TextBlock b = makeBlock("hello world");
    const std::size_t caret = replaceText(b, 0, 6, "");  // drop "hello "
    CHECK(b.utf8 == "world");
    CHECK(caret == 0);
    CHECK(isValid(b));
}

TEST_CASE("replaceText replaces a selection with new text") {
    TextBlock b = makeBlock("the cat sat");
    const std::size_t caret = replaceText(b, 4, 7, "dog");  // "cat" -> "dog"
    CHECK(b.utf8 == "the dog sat");
    CHECK(caret == 7);
    CHECK(isValid(b));
}

TEST_CASE("replaceText steps cleanly over multibyte text") {
    TextBlock b = makeBlock("aXb");
    // Replace 'X' (1 byte) with 'é' (2 bytes, 0xC3 0xA9): boundaries shift by +1.
    const std::size_t caret = replaceText(b, 1, 2, "\xC3\xA9");
    CHECK(b.utf8 == "a\xC3\xA9" "b");
    CHECK(caret == 3);  // a(1) + é(2)
    CHECK(isValid(b));
}

TEST_CASE("replaceText inserting a newline splits the paragraph, inheriting its style") {
    TextBlock b = makeBlock("ab");
    b.paragraphs[0].align = Paragraph::Align::Center;
    replaceText(b, 1, 1, "\n");
    CHECK(b.utf8 == "a\nb");
    REQUIRE(b.paragraphs.size() == 2);
    CHECK(b.paragraphs[0].align == Paragraph::Align::Center);
    CHECK(b.paragraphs[1].align == Paragraph::Align::Center);  // the new paragraph inherits
    CHECK(isValid(b));
}

TEST_CASE("replaceText deleting a newline merges paragraphs, keeping the first's style") {
    TextBlock b = makeBlock("a\nb");
    b.paragraphs[0].align = Paragraph::Align::Left;
    b.paragraphs[1].align = Paragraph::Align::Right;
    replaceText(b, 1, 2, "");  // delete the '\n'
    CHECK(b.utf8 == "ab");
    REQUIRE(b.paragraphs.size() == 1);
    CHECK(b.paragraphs[0].align == Paragraph::Align::Left);  // surviving paragraph = the first
    CHECK(isValid(b));
}

TEST_CASE("replaceText preserves distinct run styles around the edit") {
    TextBlock b = makeBlock("hello");
    setStyleRange(b, 0, 2, styled(kRed));    // "he" red
    setStyleRange(b, 2, 5, styled(kGreen));  // "llo" green
    REQUIRE(b.runs.size() == 2);
    // Insert at the boundary; the inserted span inherits the style at the caret (the run to its right).
    replaceText(b, 2, 2, "XY");
    CHECK(b.utf8 == "heXYllo");
    CHECK(isValid(b));
    CHECK(styleAt(b, 0).solidFill() == kRed);
    CHECK(styleAt(b, 3).solidFill() == kGreen);  // inside "XY"
    CHECK(styleAt(b, 6).solidFill() == kGreen);
}

TEST_CASE("replaceText with an explicit style overrides inheritance") {
    TextBlock b = makeBlock("hi", styled(kRed));
    replaceText(b, 2, 2, "!", styled(kGreen));
    CHECK(b.utf8 == "hi!");
    CHECK(styleAt(b, 0).solidFill() == kRed);
    CHECK(styleAt(b, 2).solidFill() == kGreen);
    CHECK(isValid(b));
}

TEST_CASE("replaceText clears to empty") {
    TextBlock b = makeBlock("gone");
    replaceText(b, 0, 4, "");
    CHECK(b.utf8.empty());
    CHECK(b.runs.empty());
    CHECK(b.paragraphs.size() == 1);
    CHECK(isValid(b));
}

// ---------------------------------------------------------------------------------------------
// UTF-8 / word / paragraph stepping (deterministic)
// ---------------------------------------------------------------------------------------------
TEST_CASE("char boundaries step whole codepoints") {
    const std::string s = "a\xC3\xA9z";  // a, é (2 bytes), z
    CHECK(nextCharBoundary(s, 0) == 1);
    CHECK(nextCharBoundary(s, 1) == 3);  // skips the é continuation byte
    CHECK(nextCharBoundary(s, 3) == 4);
    CHECK(nextCharBoundary(s, 4) == 4);  // clamped at end
    CHECK(prevCharBoundary(s, 4) == 3);
    CHECK(prevCharBoundary(s, 3) == 1);  // back over the whole é
    CHECK(prevCharBoundary(s, 1) == 0);
    CHECK(prevCharBoundary(s, 0) == 0);
}

TEST_CASE("word boundaries jump over words and spaces") {
    const std::string s = "foo bar baz";
    CHECK(nextWordBoundary(s, 0) == 4);   // start of "bar"
    CHECK(nextWordBoundary(s, 4) == 8);   // start of "baz"
    CHECK(nextWordBoundary(s, 8) == 11);  // end of text
    CHECK(prevWordBoundary(s, 11) == 8);  // start of "baz"
    CHECK(prevWordBoundary(s, 7) == 4);   // from inside "bar" back to its start
    CHECK(prevWordBoundary(s, 3) == 0);
}

TEST_CASE("wordAt selects the word under the caret") {
    const std::string s = "foo bar";
    TextSelection w = wordAt(s, 5);  // inside "bar"
    CHECK(w.lo() == 4);
    CHECK(w.hi() == 7);
    TextSelection w0 = wordAt(s, 0);
    CHECK(w0.lo() == 0);
    CHECK(w0.hi() == 3);
}

TEST_CASE("paragraph edges find the surrounding line") {
    const std::string s = "one\ntwo\nthree";
    CHECK(paragraphStart(s, 5) == 4);  // within "two"
    CHECK(paragraphEnd(s, 5) == 7);
    CHECK(paragraphStart(s, 0) == 0);
    CHECK(paragraphEnd(s, 0) == 3);
    CHECK(paragraphStart(s, 13) == 8);  // within "three"
    CHECK(paragraphEnd(s, 13) == 13);
}

// ---------------------------------------------------------------------------------------------
// Geometry & hit-testing (gated on a real FontDB)
// ---------------------------------------------------------------------------------------------
TEST_CASE("caretGeometry on an empty block is a non-degenerate bar at the origin") {
    TextShaper shaper;
    mosaic::platform::FontDB db;  // unused by an empty block, but keeps the shape of the API call
    TextBlock b = makeBlock("");
    ShapedBlock sh = shaper.layout(b, db);
    CaretGeometry c = caretGeometry(sh, b, 0);
    CHECK(c.height() > 0.0);
    CHECK(c.top.x == doctest::Approx(c.bottom.x));  // a vertical bar
}

TEST_CASE("an empty block's caret height scales with its pending style size") {
    // fixlist #3: the empty caret used to be a constant 24px regardless of the chosen size.
    TextShaper shaper;
    mosaic::platform::FontDB db;
    const auto caretH = [&](float size) {
        TextBlock b = makeBlock("", styled({0, 0, 0, 1}, size));
        ShapedBlock sh = shaper.layout(b, db);
        return caretGeometry(sh, b, 0).height();
    };
    CHECK(caretH(80.0f) > caretH(20.0f) + 1.0);  // bigger pending size => taller caret
}

TEST_CASE("an empty block's caret matches the first glyph's caret height (no jump on first type)") {
    // The empty caret must span the pending style's REAL font ascent+descent -- the same metric the
    // first typed glyph's caret uses -- so the caret doesn't visibly grow the moment you type.
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    CharStyle st = styled({0, 0, 0, 1}, 48.0f);
    st.font.family = db.defaultFamily();

    TextBlock empty = makeBlock("", st);
    const double emptyH = caretGeometry(shaper.layout(empty, db), empty, 0).height();

    TextBlock one = makeBlock("X", st);
    const ShapedBlock sh = shaper.layout(one, db);
    const double firstH = caretGeometry(sh, one, 1).height();  // caret right after the 'X'

    CHECK(emptyH == doctest::Approx(firstH));  // identical -> no jump when the first glyph appears
    CHECK(emptyH > 48.0);  // it's the font metric (ascent+descent > em), not the raw em size
}

TEST_CASE("caret advances left-to-right and hit-test round-trips") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    CharStyle st = styled({0, 0, 0, 1}, 32.0f);
    st.font.family = db.defaultFamily();
    TextBlock b = makeBlock("Hello", st);
    ShapedBlock sh = shaper.layout(b, db);

    const double x0 = caretGeometry(sh, b, 0).top.x;
    const double x3 = caretGeometry(sh, b, 3).top.x;
    const double x5 = caretGeometry(sh, b, 5).top.x;
    CHECK(x0 < x3);
    CHECK(x3 < x5);

    // A click left of the start lands at 0; far to the right lands at the end.
    CHECK(hitTest(sh, b, {-100.0, caretGeometry(sh, b, 0).top.y + 1.0}) == 0);
    CHECK(hitTest(sh, b, {1e6, caretGeometry(sh, b, 0).top.y + 1.0}) == 5);

    // Round-trip: the caret x for offset 3, hit back, returns 3.
    const CaretGeometry c3 = caretGeometry(sh, b, 3);
    const double midY = (c3.top.y + c3.bottom.y) * 0.5;
    CHECK(hitTest(sh, b, {c3.top.x, midY}) == 3);
}

TEST_CASE("selectionRects yields one rect per spanned line") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    CharStyle st = styled({0, 0, 0, 1}, 24.0f);
    st.font.family = db.defaultFamily();

    TextBlock one = makeBlock("Hello world", st);
    ShapedBlock sh1 = shaper.layout(one, db);
    auto rects = selectionRects(sh1, one, 0, 5);
    REQUIRE(rects.size() == 1);
    CHECK(rects[0].w > 0.0);
    CHECK(selectionRects(sh1, one, 3, 3).empty());  // empty range -> no rects

    TextBlock two = makeBlock("a\nb", st);  // two paragraphs -> two lines
    ShapedBlock sh2 = shaper.layout(two, db);
    auto rects2 = selectionRects(sh2, two, 0, 3);  // whole thing
    CHECK(rects2.size() == 2);
}

// ---------------------------------------------------------------------------------------------
// Bent baseline (§9) editing geometry -- the caret/hit-test/selection ride the arch, not the flat
// line box (they read the warped glyph pens + per-glyph baselineAngle applyBend leaves on the block).
// ---------------------------------------------------------------------------------------------
TEST_CASE("bent text: the caret rides the arch and tilts at the ends") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    CharStyle st = styled({0, 0, 0, 1}, 40.0f);
    st.font.family = db.defaultFamily();
    TextBlock b = makeBlock("MOSAIC", st);
    b.bend = 0.9f;
    const ShapedBlock sh = shaper.layout(b, db);
    REQUIRE(sh.glyphs.size() >= 5);
    const std::size_t n = b.utf8.size();

    const CaretGeometry c0 = caretGeometry(sh, b, 0);
    const CaretGeometry cEnd = caretGeometry(sh, b, n);
    const CaretGeometry cMid = caretGeometry(sh, b, n / 2);
    // Tilted at the ends, flattest at the apex.
    CHECK(std::abs(c0.angleRad) > 0.1);
    CHECK(std::abs(cEnd.angleRad) > 0.1);
    CHECK(std::abs(cMid.angleRad) < std::abs(c0.angleRad));
    // Rising into the apex on the left, descending out of it on the right -> opposite tilt.
    CHECK((c0.angleRad > 0.0) != (cEnd.angleRad > 0.0));
    // The apex lifts: the middle caret sits above (smaller y) the two end carets.
    CHECK(cMid.bottom.y < c0.bottom.y - 1.0);
    CHECK(cMid.bottom.y < cEnd.bottom.y - 1.0);
}

TEST_CASE("bent text: clicking lands on the warped glyphs (hit-test round-trips)") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    CharStyle st = styled({0, 0, 0, 1}, 40.0f);
    st.font.family = db.defaultFamily();
    TextBlock b = makeBlock("MOSAIC", st);  // single-byte ASCII, glyph i has cluster i
    b.bend = 0.8f;
    const ShapedBlock sh = shaper.layout(b, db);
    for (std::size_t pos : {std::size_t(0), std::size_t(1), std::size_t(3), std::size_t(5),
                            b.utf8.size()}) {
        const CaretGeometry c = caretGeometry(sh, b, pos);
        const double mx = (c.top.x + c.bottom.x) * 0.5, my = (c.top.y + c.bottom.y) * 0.5;
        CHECK(hitTest(sh, b, {mx, my}) == pos);
    }
}

TEST_CASE("bent text: the selection highlight is a gap-free oriented ribbon") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    CharStyle st = styled({0, 0, 0, 1}, 40.0f);
    st.font.family = db.defaultFamily();

    TextBlock b = makeBlock("MOSAIC", st);
    b.bend = 0.9f;
    const ShapedBlock sh = shaper.layout(b, db);
    const auto q = selectionQuads(sh, b, 0, b.utf8.size());
    CHECK(q.size() > sh.glyphs.size());  // a densely-sampled ribbon, not one facet per glyph
    // The first segment is rotated: its top edge (TL->TR) is clearly not horizontal.
    const auto& f = q.front();
    CHECK(std::abs(f[1].y - f[0].y) > 0.1 * std::abs(f[1].x - f[0].x));
    // Continuous ribbon: consecutive quads meet/overlap (their shared-rib corners are ~1.6px apart,
    // the dilation that paints over the AA seam) -- never a gap (which would be a full rib step apart).
    for (std::size_t k = 0; k + 1 < q.size(); ++k) {
        CHECK((q[k][1] - q[k + 1][0]).length() < 2.5);
        CHECK((q[k][2] - q[k + 1][3]).length() < 2.5);
    }

    // A flat block's selection stays axis-aligned: one quad per line, top edge horizontal.
    TextBlock flat = makeBlock("MOSAIC", st);
    const ShapedBlock shf = shaper.layout(flat, db);
    const auto qf = selectionQuads(shf, flat, 0, flat.utf8.size());
    REQUIRE(qf.size() == 1);
    CHECK(std::abs(qf[0][1].y - qf[0][0].y) < 0.01);
}

TEST_CASE("bent AREA text: the caret hugs the warped letters on EVERY line") {
    // Regression for the frame-driven Area arc (user 2026-07-14). The placement (applyBend) and the
    // editing chrome (caretGeometry / selectionQuads) must measure their perpendicular offsets from
    // the SAME reference. For an Area block that reference is now the FRAME TOP (bentArc.baseY == 0)
    // -- an edit chrome still measuring from the first line's baseline floats a whole first-baseline
    // (~an ascent + inset) off the letters, on every line at once.
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    CharStyle st = styled({0, 0, 0, 1}, 18.0f);
    st.font.family = db.defaultFamily();
    TextBlock b = makeBlock("The quick brown fox jumps over the lazy dog again and again", st);
    b.frame = TextFrame::Area;
    b.areaSize = {260.0f, 220.0f};
    // 0.4, deliberately: theta = 0.96 puts the arc centre at R = W/theta = 271 px -- deeper than the
    // 220 px box, so no line's offsets are centre-clamped and the caret bar keeps its full span (the
    // clamp is its own case below).
    b.bend = 0.4f;
    const ShapedBlock sh = shaper.layout(b, db);
    REQUIRE(sh.lines.size() >= 2); // the premise: multi-line, or "every line" pins nothing
    REQUIRE(sh.bentArc.active);

    // The caret AT a glyph's cluster must sit ON that glyph: its bottom end is the glyph's pen
    // plus the line's descent along the local normal, and glyph pens are the placement's own truth.
    for (const std::size_t li : {std::size_t{0}, sh.lines.size() - 1}) {
        const ShapedLine& ln = sh.lines[li];
        REQUIRE(ln.begin < ln.end);
        const ShapedGlyph& g = sh.glyphs[ln.begin];
        const CaretGeometry c = caretGeometry(sh, b, g.cluster);
        const double sa = std::sin(g.baselineAngle), ca = std::cos(g.baselineAngle);
        const mosaic::common::Vec2 expect{g.pen.x - sa * ln.descent, g.pen.y + ca * ln.descent};
        CHECK((c.bottom - expect).length() < 1.5);
        // ... and the caret's bar spans the line's ascent+descent along the normal.
        CHECK((c.top - c.bottom).length() ==
              doctest::Approx(ln.ascent + ln.descent).epsilon(0.05));
    }

    // The selection ribbon rides the same reference: a selection of the LAST visual line stays on
    // that line's warped glyphs (a rib corner lands within the line's own descent of its first
    // pen). The old first-baseline reference floated the whole ribbon ~a first-baseline (~17 px
    // here) up the normal, so its nearest corner could not come this close.
    const ShapedLine& last = sh.lines.back();
    const ShapedGlyph& lg = sh.glyphs[last.begin];
    const auto quads = selectionQuads(sh, b, visualLineStart(sh, b, lg.cluster),
                                      visualLineEnd(sh, b, lg.cluster));
    REQUIRE_FALSE(quads.empty());
    double nearest = 1e9;
    for (const auto& q : quads)
        for (const auto& p : q)
            nearest = std::min(nearest, (p - lg.pen).length());
    CHECK(nearest < 8.0);
}

TEST_CASE("bent text: a strong DOWNWARD bend never folds the ribbon past the arc centre") {
    // Regression (user 2026-07-07): with ascent > |R| (a big glyph on a tight downward arc) the
    // ribbon's ascent side used to offset THROUGH the arc's centre -- each quad self-crossed into a
    // bowtie the present shader's convex fill culls, so the highlight pinched into a fan and then
    // vanished entirely. The offsets are now centre-clamped (clampBendOffset): every quad must stay
    // convex with the canonical positive winding, and the caret must not cross the centre either.
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    CharStyle st = styled({0, 0, 0, 1}, 120.0f);
    st.font.family = db.defaultFamily();
    TextBlock b = makeBlock("M", st);  // one big glyph: W ~ an em, so |R| = W/2.4 << ascent
    b.bend = -1.0f;
    const ShapedBlock sh = shaper.layout(b, db);
    REQUIRE(sh.bentArc.active);
    const double R = sh.bentArc.W / sh.bentArc.theta;  // signed; negative for a downward bend
    REQUIRE(R < 0.0);
    REQUIRE(sh.lines.front().ascent > std::abs(R));  // the clamp is actually exercised

    const auto q = selectionQuads(sh, b, 0, b.utf8.size());
    REQUIRE(q.size() >= 2);
    for (const auto& quad : q) {
        // Convex + positive shoelace in y-down space: every consecutive-edge cross >= ~0 (slivers
        // pinched at the centre may be degenerate, never negative/self-crossing).
        for (int i = 0; i < 4; ++i) {
            const auto a = quad[i], bb = quad[(i + 1) % 4], c = quad[(i + 2) % 4];
            const double cross = (bb.x - a.x) * (c.y - bb.y) - (bb.y - a.y) * (c.x - bb.x);
            CHECK(cross >= -1e-6);
        }
    }
    // The caret's two ends stay on ONE side of the centre (no fold-through), and coincide with the
    // ribbon's clamped rib rather than poking past the apex.
    const double xc = sh.bentArc.x0 + 0.5 * sh.bentArc.W;
    const double cy = sh.bentArc.baseY + R * std::cos(sh.bentArc.theta * 0.5);
    const mosaic::common::Vec2 centre{xc, cy};
    for (std::size_t pos : {std::size_t{0}, b.utf8.size()}) {
        const CaretGeometry cg = caretGeometry(sh, b, pos);
        const auto dTop = cg.top - centre;
        const auto dBot = cg.bottom - centre;
        CHECK(dTop.x * dBot.x + dTop.y * dBot.y > 0.0);  // same side of the centre
    }
}

TEST_CASE("bent 3D text: the editing geometry rides the arch too (bend composes with extrude)") {
    // 3D + bend compose (S30 2026-07-07): isBent no longer excludes extruded blocks, so the caret /
    // selection / hit-test ride the warped pens exactly as for a flat bent block. (The canvas then
    // projects this bent geometry through the ExtrudePlaneMap -- not exercised here.)
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    CharStyle st = styled({0, 0, 0, 1}, 40.0f);
    st.font.family = db.defaultFamily();
    TextBlock b = makeBlock("MOSAIC", st);
    b.bend = 0.9f;
    b.extrude = Extrude{};
    const ShapedBlock sh = shaper.layout(b, db);
    REQUIRE(sh.bentArc.active);  // the layout bends even though the block is extruded
    const CaretGeometry c0 = caretGeometry(sh, b, 0);
    const CaretGeometry cEnd = caretGeometry(sh, b, b.utf8.size());
    CHECK(std::abs(c0.angleRad) > 0.1);  // tilted ends, like the flat bent block
    CHECK((c0.angleRad > 0.0) != (cEnd.angleRad > 0.0));
    // Clicking the middle caret's midpoint lands back on the same byte (warped hit-test active).
    const CaretGeometry cMid = caretGeometry(sh, b, 3);
    const double mx = (cMid.top.x + cMid.bottom.x) * 0.5, my = (cMid.top.y + cMid.bottom.y) * 0.5;
    CHECK(hitTest(sh, b, {mx, my}) == 3);
}

// ---------------------------------------------------------------------------------------------
// Fit-to-path (§9) -- placement + editing geometry ride the block's baked path (applyPath /
// CurveSampler), sharing the bend machinery. Deterministic baked polylines; fonts-gated.
// ---------------------------------------------------------------------------------------------
namespace {

PathFit straightFit(double y, double s0, double s1, bool flip = false) {
    PathFit f;
    f.layer = 1;
    f.s0 = s0;
    f.s1 = s1;
    f.flip = flip;
    mosaic::core::vec::Contour c;
    c.points = {{0.0, y}, {4000.0, y}};
    f.baked = {c};
    return f;
}

PathFit circleFit(mosaic::common::Vec2 centre, double radius, double s0) {
    PathFit f;
    f.layer = 1;
    f.s0 = s0;
    mosaic::core::vec::Contour c;
    c.closed = true;
    for (int i = 0; i < 128; ++i) {
        const double a = 2.0 * 3.14159265358979323846 * i / 128;
        c.points.push_back({centre.x + radius * std::cos(a), centre.y + radius * std::sin(a)});
    }
    f.baked = {c};
    f.s1 = 2.0 * 3.14159265358979323846 * radius;
    return f;
}

}  // namespace

TEST_CASE("path-fitted text rides a straight path at the bracket offset; hit-test round-trips") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    CharStyle st = styled({0, 0, 0, 1}, 40.0f);
    st.font.family = db.defaultFamily();
    TextBlock b = makeBlock("MOSAIC", st);
    b.pathFit = straightFit(500.0, 150.0, 2000.0);
    const ShapedBlock sh = shaper.layout(b, db);
    REQUIRE(sh.pathRide.active);
    REQUIRE_FALSE(sh.bentArc.active);

    // The first caret sits at arc-distance s0 on the path (x = 150, baseline y = 500), unrotated.
    const CaretGeometry c0 = caretGeometry(sh, b, 0);
    CHECK(c0.top.x == doctest::Approx(150.0).epsilon(0.01));
    CHECK(std::abs(c0.angleRad) < 1e-3);
    CHECK(c0.top.y < 500.0);     // ascent above the path
    CHECK(c0.bottom.y > 500.0);  // descent below it
    // Carets advance along the path and hit-testing lands back on the same byte.
    const CaretGeometry cEnd = caretGeometry(sh, b, b.utf8.size());
    CHECK(cEnd.top.x > c0.top.x + 50.0);
    for (std::size_t pos : {std::size_t(0), std::size_t(2), b.utf8.size()}) {
        const CaretGeometry c = caretGeometry(sh, b, pos);
        CHECK(hitTest(sh, b, {(c.top.x + c.bottom.x) * 0.5, (c.top.y + c.bottom.y) * 0.5}) == pos);
    }
    // The selection is a ribbon of valid (convex, positively wound) quads hugging the path.
    const auto q = selectionQuads(sh, b, 0, b.utf8.size());
    REQUIRE(q.size() >= 2);
    for (const auto& quad : q)
        for (int i = 0; i < 4; ++i) {
            const auto a = quad[i], bb = quad[(i + 1) % 4], c = quad[(i + 2) % 4];
            CHECK((bb.x - a.x) * (c.y - bb.y) - (bb.y - a.y) * (c.x - bb.x) >= -1e-6);
        }
}

TEST_CASE("path-fitted text: flip mirrors across the path and reverses the run direction") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    CharStyle st = styled({0, 0, 0, 1}, 40.0f);
    st.font.family = db.defaultFamily();
    TextBlock b = makeBlock("MOSAIC", st);
    b.pathFit = straightFit(500.0, 150.0, 900.0, /*flip=*/true);
    const ShapedBlock sh = shaper.layout(b, db);
    REQUIRE(sh.pathRide.active);
    const CaretGeometry c0 = caretGeometry(sh, b, 0);
    const CaretGeometry cEnd = caretGeometry(sh, b, b.utf8.size());
    // Runs from s1 backward...
    CHECK(c0.top.x == doctest::Approx(900.0).epsilon(0.01));
    CHECK(cEnd.top.x < c0.top.x - 50.0);
    // ...turned half a circle: the ascent side now points BELOW the path (mirrored).
    CHECK(std::abs(std::abs(c0.angleRad) - 3.14159265) < 1e-3);
    CHECK(c0.top.y > 500.0);
    CHECK(c0.bottom.y < 500.0);
}

TEST_CASE("path-fitted text on a closed circle wraps and hugs the radius") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    CharStyle st = styled({0, 0, 0, 1}, 40.0f);
    st.font.family = db.defaultFamily();
    const mosaic::common::Vec2 centre{600.0, 600.0};
    const double R = 220.0;
    TextBlock b = makeBlock("MOSAIC MOSAIC", st);
    // Start near the END of the circumference so the text wraps past s = total.
    auto fit = circleFit(centre, R, 2.0 * 3.14159265358979323846 * R - 40.0);
    b.pathFit = fit;
    const ShapedBlock sh = shaper.layout(b, db);
    REQUIRE(sh.pathRide.active);
    // Every glyph pen sits ON the circle (radius +- a couple of px of polyline flattening slack).
    for (const ShapedGlyph& g : sh.glyphs) {
        if (g.whitespace) continue;
        const double r = (g.pen - centre).length();
        CHECK(r > R - 4.0);
        CHECK(r < R + 4.0);
    }
    // And the carets ride the same circle -- including positions that wrapped past the seam.
    for (std::size_t pos : {std::size_t(0), b.utf8.size() / 2, b.utf8.size()}) {
        const CaretGeometry c = caretGeometry(sh, b, pos);
        const mosaic::common::Vec2 mid{(c.top.x + c.bottom.x) * 0.5, (c.top.y + c.bottom.y) * 0.5};
        const double r = (mid - centre).length();
        CHECK(r > R - 30.0);
        CHECK(r < R + 30.0);
        CHECK(hitTest(sh, b, mid) == pos);
    }
}

// ---------------------------------------------------------------------------------------------
// Vertical writing-mode editing geometry (commit C) -- Latin in the default Mixed orientation
// (rotated 90 deg) so no CJK face is needed; the geometry is orientation-independent.
// ---------------------------------------------------------------------------------------------
TEST_CASE("vertical caret lies across the column and advances down it; hit-test round-trips") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    CharStyle st = styled({0, 0, 0, 1}, 32.0f);
    st.font.family = db.defaultFamily();
    TextBlock b = makeBlock("Hello", st);
    b.writingMode = WritingMode::VerticalRL;
    ShapedBlock sh = shaper.layout(b, db);
    REQUIRE(sh.lines.size() == 1);

    // The bar is horizontal (both ends share y) and reports the quarter-turn angle.
    const CaretGeometry c0 = caretGeometry(sh, b, 0);
    CHECK(c0.top.y == doctest::Approx(c0.bottom.y));
    CHECK(c0.height() > 0.0);
    CHECK(c0.angleRad == doctest::Approx(1.5707963267948966));

    // Caret inline position (layer y) advances DOWN the column with the byte offset.
    const double y0 = caretGeometry(sh, b, 0).top.y;
    const double y3 = caretGeometry(sh, b, 3).top.y;
    const double y5 = caretGeometry(sh, b, 5).top.y;
    CHECK(y0 < y3);
    CHECK(y3 < y5);

    // A click above the column start lands at 0; far below lands at the end.
    const double colX = (c0.top.x + c0.bottom.x) * 0.5; // the column's cross-axis centre
    CHECK(hitTest(sh, b, {colX, -1e6}) == 0);
    CHECK(hitTest(sh, b, {colX, 1e6}) == 5);

    // Round-trip: the caret position for offset 3, hit back, returns 3.
    const CaretGeometry c3 = caretGeometry(sh, b, 3);
    CHECK(hitTest(sh, b, {(c3.top.x + c3.bottom.x) * 0.5, c3.top.y}) == 3);
}

TEST_CASE("vertical hit-test picks the nearest column, stepping the writing direction") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    CharStyle st = styled({0, 0, 0, 1}, 24.0f);
    st.font.family = db.defaultFamily();
    TextBlock b = makeBlock("abc\ndef", st); // two paragraphs -> two columns
    b.writingMode = WritingMode::VerticalRL;
    ShapedBlock sh = shaper.layout(b, db);
    REQUIRE(sh.lines.size() == 2);

    // Vertical-rl: the FIRST column is rightmost. A click far right lands in "abc", far left in "def".
    CHECK(hitTest(sh, b, {1e6, 0.0}) <= 3);
    CHECK(hitTest(sh, b, {-1e6, 0.0}) >= 4);

    b.writingMode = WritingMode::VerticalLR; // mirrored: first column leftmost
    sh = shaper.layout(b, db);
    CHECK(hitTest(sh, b, {-1e6, 0.0}) <= 3);
    CHECK(hitTest(sh, b, {1e6, 0.0}) >= 4);
}

TEST_CASE("vertical selectionRects are per-column rects stepping the writing direction") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    CharStyle st = styled({0, 0, 0, 1}, 24.0f);
    st.font.family = db.defaultFamily();
    TextBlock b = makeBlock("abc\ndef", st);
    b.writingMode = WritingMode::VerticalRL;
    ShapedBlock shRl = shaper.layout(b, db);
    auto rectsRl = selectionRects(shRl, b, 0, 7); // the whole block
    REQUIRE(rectsRl.size() == 2);
    CHECK(rectsRl[0].h > rectsRl[0].w);       // a column run: taller than wide
    CHECK(rectsRl[1].x < rectsRl[0].x);       // vertical-rl: the second column is LEFT of the first

    b.writingMode = WritingMode::VerticalLR;
    ShapedBlock shLr = shaper.layout(b, db);
    auto rectsLr = selectionRects(shLr, b, 0, 7);
    REQUIRE(rectsLr.size() == 2);
    CHECK(rectsLr[1].x > rectsLr[0].x);       // vertical-lr: columns advance rightward

    // A mid-column range spans only its own column.
    auto one = selectionRects(shRl, b, 1, 3); // inside "abc"
    REQUIRE(one.size() == 1);
    CHECK(one[0].h > 0.0);
}

TEST_CASE("vertical caret motion crosses columns and clamps at the block edges") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    CharStyle st = styled({0, 0, 0, 1}, 24.0f);
    st.font.family = db.defaultFamily();
    TextBlock b = makeBlock("abc\ndef", st);
    b.writingMode = WritingMode::VerticalRL;
    ShapedBlock sh = shaper.layout(b, db);

    // One column over from offset 1 (column 0) lands somewhere in "def" (bytes 4..7).
    const std::size_t next = moveCaretVertical(sh, b, 1, +1, -1.0);
    CHECK(next >= 4);
    CHECK(next <= 7);
    // Stepping past the first/last column is a no-op.
    CHECK(moveCaretVertical(sh, b, 1, -1, -1.0) == 1);
    CHECK(moveCaretVertical(sh, b, 5, +1, -1.0) == 5);

    // The goal INLINE coordinate (layer y) is honoured: aiming at the caret depth of offset 2
    // lands at the matching depth in the next column, not its start.
    const CaretGeometry c2 = caretGeometry(sh, b, 2);
    const std::size_t deep = moveCaretVertical(sh, b, 2, +1, c2.top.y);
    CHECK(deep > 4); // deeper than the column start

    // Home/End map to the column's start/end bytes.
    CHECK(visualLineStart(sh, b, 6) == 4);
    CHECK(visualLineEnd(sh, b, 6) == 7);
}

TEST_CASE("an empty vertical block's caret sits at the flow's start corner, lying across the column") {
    TextShaper shaper;
    mosaic::platform::FontDB db;
    CharStyle st = styled({0, 0, 0, 1}, 30.0f);

    // Vertical-rl Area: the first column hangs from the box's top-RIGHT inset corner.
    TextBlock rl = makeBlock("", st);
    rl.frame = TextFrame::Area;
    rl.areaSize = {100.0, 50.0};
    rl.writingMode = WritingMode::VerticalRL;
    const CaretGeometry cRl = caretGeometry(shaper.layout(rl, db), rl, 0);
    CHECK(cRl.top.y == doctest::Approx(cRl.bottom.y)); // a horizontal bar
    CHECK(cRl.top.x == doctest::Approx(100.0 - kAreaInset));
    CHECK(cRl.bottom.x < cRl.top.x); // extends leftward into the box
    CHECK(cRl.height() > 0.0);

    // Vertical-lr Area: mirrored to the top-LEFT inset corner, extending rightward.
    TextBlock lr = rl;
    lr.writingMode = WritingMode::VerticalLR;
    const CaretGeometry cLr = caretGeometry(shaper.layout(lr, db), lr, 0);
    CHECK(cLr.top.x == doctest::Approx(static_cast<double>(kAreaInset)));
    CHECK(cLr.bottom.x > cLr.top.x);

    // Vertical-rl POINT text grows leftward from the layer origin.
    TextBlock pt = makeBlock("", st);
    pt.writingMode = WritingMode::VerticalRL;
    const CaretGeometry cPt = caretGeometry(shaper.layout(pt, db), pt, 0);
    CHECK(cPt.top.x == doctest::Approx(0.0));
    CHECK(cPt.bottom.x < 0.0);
}

TEST_CASE("vertical motion crosses lines and clamps at the edges") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    TextShaper shaper;
    CharStyle st = styled({0, 0, 0, 1}, 24.0f);
    st.font.family = db.defaultFamily();
    TextBlock b = makeBlock("abc\ndef", st);
    ShapedBlock sh = shaper.layout(b, db);

    // From offset 1 (line 0) down one line lands somewhere on line 1 ("def", bytes 4..7).
    const std::size_t down = moveCaretVertical(sh, b, 1, +1, -1.0);
    CHECK(down >= 4);
    CHECK(down <= 7);
    // Up from the top line is a no-op.
    CHECK(moveCaretVertical(sh, b, 1, -1, -1.0) == 1);
    // Down from the bottom line is a no-op.
    CHECK(moveCaretVertical(sh, b, 5, +1, -1.0) == 5);

    CHECK(visualLineStart(sh, b, 6) == 4);  // within "def"
    CHECK(visualLineEnd(sh, b, 6) == 7);
}
