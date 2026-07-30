#include <doctest/doctest.h>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/clone_stamp.hpp"
#include "core/stroke_confinement.hpp"
#include "ui/clone_stamp_gesture.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

// The S38 Stamp / Clone tool (docs/clone-stamp.md), tested headlessly on synthetic images built
// right here. Every expectation below is derived from the arithmetic in core/clone_stamp.cpp rather
// than from a run of it -- the numbers are hand-computed in the comments so a change that moves a
// pixel has to argue with the maths, not with a recorded output.
namespace {

using mosaic::common::Affine2D;
using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::common::Vec2;
using mosaic::core::CloneAnchorState;
using mosaic::core::CloneStampInput;

// A w x h image whose every pixel is `c`.
[[nodiscard]] Image solid(std::uint32_t w, std::uint32_t h, Color8 c) {
    Image img(w, h);
    img.fill(c);
    return img;
}

void put(Image& img, std::uint32_t x, std::uint32_t y, Color8 c) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    img.rgba[p] = c.r;
    img.rgba[p + 1] = c.g;
    img.rgba[p + 2] = c.b;
    img.rgba[p + 3] = c.a;
}

[[nodiscard]] Color8 get(const Image& img, std::uint32_t x, std::uint32_t y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

} // namespace

// ---- The offset model (docs/clone-stamp.md §3) --------------------------------------------------

TEST_CASE("a clone stroke with no picked source is refused, not invented") {
    CloneAnchorState s;
    CHECK(!s.hasAnchor);
    CHECK(!mosaic::core::cloneStrokeOffset(s, {40.0, 40.0}, /*aligned=*/true).has_value());
    CHECK(!mosaic::core::cloneStrokeOffset(s, {40.0, 40.0}, /*aligned=*/false).has_value());
    // A refused stroke must not leave state behind that a LATER stroke could pick up.
    CHECK(!s.hasOffset);
}

TEST_CASE("aligned latches the first stroke's offset and every later stroke keeps it") {
    CloneAnchorState s;
    mosaic::core::setCloneAnchor(s, {10.0, 10.0});

    const auto first = mosaic::core::cloneStrokeOffset(s, {30.0, 25.0}, /*aligned=*/true);
    REQUIRE(first.has_value());
    CHECK(first->x == 20.0); // 30 - 10
    CHECK(first->y == 15.0); // 25 - 10
    CHECK(s.hasOffset);

    // A second stroke that begins somewhere else entirely still clones at the SAME offset -- that
    // is what keeps a subject painted out over several strokes in one piece.
    const auto second = mosaic::core::cloneStrokeOffset(s, {300.0, -7.0}, /*aligned=*/true);
    REQUIRE(second.has_value());
    CHECK(second->x == 20.0);
    CHECK(second->y == 15.0);
}

TEST_CASE("non-aligned re-anchors every stroke to the same source point") {
    CloneAnchorState s;
    mosaic::core::setCloneAnchor(s, {10.0, 10.0});

    const auto a = mosaic::core::cloneStrokeOffset(s, {30.0, 25.0}, /*aligned=*/false);
    REQUIRE(a.has_value());
    CHECK(a->x == 20.0);
    CHECK(a->y == 15.0);
    CHECK(!s.hasOffset); // nothing is latched, by construction

    const auto b = mosaic::core::cloneStrokeOffset(s, {50.0, 50.0}, /*aligned=*/false);
    REQUIRE(b.has_value());
    CHECK(b->x == 40.0); // 50 - 10: this stroke starts stamping AT the anchor again
    CHECK(b->y == 40.0);

    // ... and switching Aligned back on re-derives rather than resurrecting the first offset.
    const auto c = mosaic::core::cloneStrokeOffset(s, {11.0, 12.0}, /*aligned=*/true);
    REQUIRE(c.has_value());
    CHECK(c->x == 1.0);
    CHECK(c->y == 2.0);
}

TEST_CASE("re-picking the source drops the latched aligned offset") {
    CloneAnchorState s;
    mosaic::core::setCloneAnchor(s, {10.0, 10.0});
    (void)mosaic::core::cloneStrokeOffset(s, {30.0, 25.0}, /*aligned=*/true);
    REQUIRE(s.hasOffset);

    mosaic::core::setCloneAnchor(s, {100.0, 100.0});
    CHECK(!s.hasOffset); // otherwise the click would have done nothing at all
    const auto next = mosaic::core::cloneStrokeOffset(s, {110.0, 90.0}, /*aligned=*/true);
    REQUIRE(next.has_value());
    CHECK(next->x == 10.0);  // 110 - 100
    CHECK(next->y == -10.0); //  90 - 100
}

// ---- The exact-copy branch ----------------------------------------------------------------------

TEST_CASE("a whole-pixel shift is recognised so the stamp copies bytes instead of resampling") {
    CHECK(mosaic::core::isWholePixelShift(Affine2D::identity()));
    CHECK(mosaic::core::isWholePixelShift(Affine2D::translation(37.0, -12.0)));
    // Half a pixel is a resample, and so is any scale or rotation.
    CHECK(!mosaic::core::isWholePixelShift(Affine2D::translation(0.5, 0.0)));
    CHECK(!mosaic::core::isWholePixelShift(Affine2D::translation(0.0, -3.25)));
    CHECK(!mosaic::core::isWholePixelShift(Affine2D::scaling(2.0, 2.0)));
    CHECK(!mosaic::core::isWholePixelShift(Affine2D::rotation(0.1)));
    // The offset reaches the sampler through a few affine multiplies, so a shift that is a whole
    // number to within floating-point noise must still count as one.
    const Affine2D chained = Affine2D::translation(-4.0, 0.0) * Affine2D::identity() *
                             Affine2D::translation(0.0, 9.0);
    CHECK(mosaic::core::isWholePixelShift(chained));
}

// ---- The sampler --------------------------------------------------------------------------------

TEST_CASE("nearest sampling reads the pixel the coordinate falls inside") {
    Image src = solid(4, 4, Color8{0, 0, 0, 255});
    put(src, 2, 1, Color8{10, 20, 30, 255});

    // Pixel (2,1)'s centre is (2.5, 1.5); anywhere inside [2,3)x[1,2) reads it.
    CHECK(mosaic::core::sampleClone(src, 2.5, 1.5, false) == Color8{10, 20, 30, 255});
    CHECK(mosaic::core::sampleClone(src, 2.01, 1.99, false) == Color8{10, 20, 30, 255});
    // Outside the source there are no pixels to copy, and "transparent" is the honest answer.
    CHECK(mosaic::core::sampleClone(src, -0.5, 1.5, false) == Color8{0, 0, 0, 0});
    CHECK(mosaic::core::sampleClone(src, 4.5, 1.5, false) == Color8{0, 0, 0, 0});
    CHECK(mosaic::core::sampleClone(Image{}, 0.5, 0.5, false) == Color8{0, 0, 0, 0});
}

TEST_CASE("bilinear sampling reproduces a pixel exactly at its own centre") {
    Image src = solid(4, 4, Color8{7, 200, 41, 255});
    // At a pixel centre the two taps in each axis collapse to one (tx = ty = 0), so the answer is
    // that pixel's own bytes -- which is what makes a bilinear clone of an unshifted region lossless.
    CHECK(mosaic::core::sampleClone(src, 2.5, 1.5, true) == Color8{7, 200, 41, 255});
}

TEST_CASE("bilinear sampling averages two opaque neighbours") {
    // Columns 0..1 are black, columns 2..3 are 200-grey; every pixel opaque.
    Image src = solid(4, 4, Color8{0, 0, 0, 255});
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 2; x < 4; ++x)
            put(src, x, y, Color8{200, 200, 200, 255});

    // At sx = 2.0 the sample sits exactly between columns 1 and 2 (fx = 1.5 -> x0 = 1, tx = 0.5);
    // sy = 1.5 lands on row 1's centre (ty = 0), so the vertical pair collapses. The two live taps
    // weigh 0.5 each and both are opaque, so the answer is (0 + 200) / 2 = 100.
    const Color8 c = mosaic::core::sampleClone(src, 2.0, 1.5, true);
    CHECK(c.r == 100);
    CHECK(c.g == 100);
    CHECK(c.b == 100);
    CHECK(c.a == 255);
}

TEST_CASE("a transparent neighbour contributes transparency, never its stale colour") {
    Image src = solid(4, 4, Color8{0, 0, 0, 0});
    put(src, 1, 1, Color8{255, 0, 0, 255});   // opaque red
    put(src, 2, 1, Color8{0, 255, 0, 0});     // fully TRANSPARENT, with green bytes behind it
    put(src, 1, 2, Color8{255, 0, 0, 255});
    put(src, 2, 2, Color8{0, 255, 0, 0});

    // sx = 2.0, sy = 1.5 -> taps (1,1) w=0.5 and (2,1) w=0.5 (the y pair collapses).
    // alpha = 0.5*1 + 0.5*0 = 0.5 -> lround(127.5) = 128.
    // colour weight = 0.5*1 only, so the colour is the RED tap's, undiluted.
    const Color8 c = mosaic::core::sampleClone(src, 2.0, 1.5, true);
    CHECK(c.a == 128);
    CHECK(c.r == 255);
    CHECK(c.g == 0);
    CHECK(c.b == 0);
}

// ---- The deposit --------------------------------------------------------------------------------

namespace {

// A clone set-up on an 8x8 grid: the destination is opaque blue, the source is opaque red, and the
// stroke covers the 4x4 window at (2,2) at `cov`. `targetToSource` shifts the read 2 px left, so a
// target pixel at x reads the source at x - 2.
//
// ⚠ The shift has to keep the WHOLE stroke window on the source. The window is x in [2,6), so a
// 2 px shift reads [0,4) and a 4 px one would read [-2,2) -- putting two of the four columns off
// the source, where the deposit correctly leaves the destination pristine (the "cloning from off
// the source" case below pins that). A bench that half-misses looks like a broken deposit and is
// really a broken bench.
struct Bench {
    Image base = solid(8, 8, Color8{0, 0, 255, 255});
    Image source = solid(8, 8, Color8{255, 0, 0, 255});
    Image target = solid(8, 8, Color8{0, 0, 255, 255});
    std::vector<float> coverage = std::vector<float>(16, 0.0f);

    [[nodiscard]] CloneStampInput input(double opacity = 1.0) {
        CloneStampInput in;
        in.target = &target;
        in.base = &base;
        in.source = &source;
        in.targetToSource = Affine2D::translation(-2.0, 0.0);
        in.bilinear = false; // a whole-pixel shift: a byte copy
        in.coverage = coverage.data();
        in.covX = 2;
        in.covY = 2;
        in.covW = 4;
        in.covH = 4;
        in.opacity = opacity;
        return in;
    }
    void coverAll(float v) {
        for (float& f : coverage)
            f = v;
    }
};

} // namespace

TEST_CASE("a fully covered clone deposit is an exact byte copy of the source") {
    Bench b;
    b.coverAll(1.0f);
    const CloneStampInput in = b.input();
    CHECK(mosaic::core::applyCloneStamp(in, 2, 2, 6, 6) == std::size_t{16});

    for (std::uint32_t y = 2; y < 6; ++y)
        for (std::uint32_t x = 2; x < 6; ++x)
            CHECK(get(b.target, x, y) == Color8{255, 0, 0, 255});
    // Outside the stroke the destination is untouched, byte for byte.
    CHECK(get(b.target, 1, 2) == Color8{0, 0, 255, 255});
    CHECK(get(b.target, 6, 5) == Color8{0, 0, 255, 255});
}

TEST_CASE("the deposit is a plain source-over at partial coverage") {
    // src (255,0,0,255) over base (0,0,255,255) at alpha 0.5:
    //   oa = 0.5 + 1*(1 - 0.5) = 1
    //   r  = (1.0*0.5 + 0.0*1*0.5) / 1 = 0.5 -> lround(127.5) = 128
    //   b  = (0.0*0.5 + 1.0*1*0.5) / 1 = 0.5 -> 128
    Bench b;
    b.coverAll(0.5f);
    CHECK(mosaic::core::applyCloneStamp(b.input(), 2, 2, 6, 6) == std::size_t{16});
    CHECK(get(b.target, 3, 3) == Color8{128, 0, 128, 255});

    // Opacity is the stroke's ceiling and multiplies the coverage, so full coverage at 0.5 opacity
    // must land on exactly the same pixel.
    Bench c;
    c.coverAll(1.0f);
    CHECK(mosaic::core::applyCloneStamp(c.input(0.5), 2, 2, 6, 6) == std::size_t{16});
    CHECK(get(c.target, 3, 3) == Color8{128, 0, 128, 255});
}

TEST_CASE("the deposit reads the PRE-STROKE snapshot, so running it twice changes nothing") {
    // This is the §6.6b guard rail expressed as a test: if the composite read the live target, the
    // second pass would blend red over the already-half-red pixel and drift.
    Bench b;
    b.coverAll(0.5f);
    CHECK(mosaic::core::applyCloneStamp(b.input(), 2, 2, 6, 6) == std::size_t{16});
    const Image once = b.target;
    CHECK(mosaic::core::applyCloneStamp(b.input(), 2, 2, 6, 6) == std::size_t{16});
    CHECK(b.target == once);
}

TEST_CASE("an uncovered pixel inside the rect is left pristine") {
    Bench b;
    b.coverAll(1.0f);
    b.coverage[static_cast<std::size_t>(1) * 4 + 1] = 0.0f; // target pixel (3,3)
    CHECK(mosaic::core::applyCloneStamp(b.input(), 2, 2, 6, 6) == std::size_t{15});
    CHECK(get(b.target, 3, 3) == Color8{0, 0, 255, 255});
    CHECK(get(b.target, 4, 3) == Color8{255, 0, 0, 255});
}

TEST_CASE("the selection confines the stamp as a coverage multiply, never as a clip") {
    Bench b;
    b.coverAll(1.0f);
    // A 2x4 window of the selection over the stroke's left half: fully selected in column 2,
    // HALF selected in column 3, and absent (therefore 0) everywhere else.
    mosaic::core::StrokeConfinement confine;
    confine.x = 2;
    confine.y = 2;
    confine.w = 2;
    confine.h = 4;
    confine.v.assign(8, 0);
    for (int row = 0; row < 4; ++row) {
        confine.v[static_cast<std::size_t>(row) * 2 + 0] = 255;
        confine.v[static_cast<std::size_t>(row) * 2 + 1] = 128;
    }
    CloneStampInput in = b.input();
    in.confine = &confine;
    CHECK(mosaic::core::applyCloneStamp(in, 2, 2, 6, 6) == std::size_t{8}); // only the 2x4 selected window

    CHECK(get(b.target, 2, 3) == Color8{255, 0, 0, 255}); // 255/255 == 1 exactly: byte-identical
    // Column 3 takes 128/255 of the paint. sa = 0.50196..., oa = 1, and
    // r = sa -> lround(sa * 255) = lround(128.0) = 128; b = 1 - sa -> lround(127.0) = 127.
    CHECK(get(b.target, 3, 3) == Color8{128, 0, 127, 255});
    CHECK(get(b.target, 4, 3) == Color8{0, 0, 255, 255}); // outside the selection: pristine
}

TEST_CASE("cloning from off the source deposits nothing rather than a hole") {
    Bench b;
    b.coverAll(1.0f);
    CloneStampInput in = b.input();
    in.targetToSource = Affine2D::translation(-40.0, 0.0); // every read lands off the source
    CHECK(mosaic::core::applyCloneStamp(in, 2, 2, 6, 6) == std::size_t{16});
    for (std::uint32_t y = 2; y < 6; ++y)
        for (std::uint32_t x = 2; x < 6; ++x)
            CHECK(get(b.target, x, y) == Color8{0, 0, 255, 255}); // the destination survives
}

TEST_CASE("the deposit is clamped to the coverage window and to the target") {
    Bench b;
    b.coverAll(1.0f);
    // A rect covering the whole image still only writes the 4x4 the stroke actually covers.
    CHECK(mosaic::core::applyCloneStamp(b.input(), -100, -100, 1000, 1000) == std::size_t{16});
    CHECK(get(b.target, 0, 0) == Color8{0, 0, 255, 255});
    CHECK(get(b.target, 7, 7) == Color8{0, 0, 255, 255});
}

TEST_CASE("a malformed clone input deposits nothing") {
    Bench b;
    b.coverAll(1.0f);
    CloneStampInput none;
    CHECK(mosaic::core::applyCloneStamp(none, 0, 0, 8, 8) == std::size_t{0});
    CloneStampInput noSource = b.input();
    noSource.source = nullptr;
    CHECK(mosaic::core::applyCloneStamp(noSource, 2, 2, 6, 6) == std::size_t{0});
    // The layer was resized under the stroke: the snapshot no longer describes the target.
    Image small = solid(4, 4, Color8{0, 0, 0, 255});
    CloneStampInput mismatched = b.input();
    mismatched.base = &small;
    CHECK(mosaic::core::applyCloneStamp(mismatched, 2, 2, 6, 6) == std::size_t{0});
}

// ---- The UI half --------------------------------------------------------------------------------

TEST_CASE("the Sample choice's indices map onto the three sampling modes") {
    CHECK(mosaic::ui::cloneSampleForChoice(0) == mosaic::core::CloneSampleSource::CurrentLayer);
    CHECK(mosaic::ui::cloneSampleForChoice(1) == mosaic::core::CloneSampleSource::CurrentAndBelow);
    CHECK(mosaic::ui::cloneSampleForChoice(2) == mosaic::core::CloneSampleSource::AllLayers);
    // An index the bar could never produce falls back to the safest mode, not to a wild one.
    CHECK(mosaic::ui::cloneSampleForChoice(-1) == mosaic::core::CloneSampleSource::CurrentLayer);
    CHECK(mosaic::ui::cloneSampleForChoice(99) == mosaic::core::CloneSampleSource::CurrentLayer);
}

TEST_CASE("the source marker is a closed circle with an inscribed diamond, and never retraces") {
    const Vec2 c{100.0, 50.0};
    const double r = 9.0;
    const std::vector<Vec2> pts = mosaic::ui::cloneMarkerPolyline(c, r);
    REQUIRE(pts.size() == std::size_t{29}); // 24 circle segments (25 points, closed) + 4 diamond chords

    for (const Vec2& p : pts) {
        const double d = std::hypot(p.x - c.x, p.y - c.y);
        CHECK(std::abs(d - r) < 1e-9); // every vertex sits on the circle, diamond corners included
    }
    // The circle closes on itself...
    CHECK(std::abs(pts[24].x - pts[0].x) < 1e-9);
    CHECK(std::abs(pts[24].y - pts[0].y) < 1e-9);
    // ... and no segment is drawn twice, which would read as a heavier line where it doubled.
    for (std::size_t i = 1; i < pts.size(); ++i)
        CHECK(std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y) > 1e-9);
}

TEST_CASE("the source marker keeps a visible size however small a radius it is handed") {
    const std::vector<Vec2> pts = mosaic::ui::cloneMarkerPolyline({0.0, 0.0}, 0.25);
    REQUIRE(!pts.empty());
    CHECK(std::hypot(pts[0].x, pts[0].y) == 2.0); // the floor, exactly
}
