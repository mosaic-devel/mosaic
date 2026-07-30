#include "ui/crop_gesture.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <numbers>

// The S16 Crop tool's pure half: the staged-rect math (draw/move/resize with ratio, Shift and
// Alt constraints, document clamping), the ratio-option mapping, the option-change re-conform,
// and the integer snap the apply path uses. The FLTK/Vulkan plumbing in VulkanCanvas stays
// thin and is exercised by the --gui-frames smoke run.
namespace {

using mosaic::common::Rect;
using mosaic::common::Vec2;
using mosaic::ui::conformCropRect;
using mosaic::ui::CropGesture;
using mosaic::ui::CropMode;
using mosaic::ui::CropPixels;
using mosaic::ui::cropRatioForOptions;
using mosaic::ui::customCropRatio;
using mosaic::ui::kCropRatioCustom;
using mosaic::ui::snapCropRect;

constexpr double kDocW = 400.0;
constexpr double kDocH = 300.0;

bool near(const Rect& a, const Rect& b, double eps = 1e-9) {
    return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps &&
           std::abs(a.w - b.w) < eps && std::abs(a.h - b.h) < eps;
}

} // namespace

TEST_CASE("begin: rejects bad input, Draw accepts an empty base") {
    CropGesture g;
    CHECK_FALSE(g.begin(CropMode::None, -1, {0, 0}, {0, 0, 10, 10}));
    CHECK_FALSE(g.begin(CropMode::Resize, 8, {0, 0}, {0, 0, 10, 10})); // no such handle
    CHECK_FALSE(g.begin(CropMode::Move, -1, {0, 0}, {}));              // empty rect to move
    CHECK(g.begin(CropMode::Draw, -1, {5, 5}, {}));
    CHECK(g.active());
    g.cancel();
    CHECK_FALSE(g.active());
}

TEST_CASE("draw: a free drag spans press to cursor, either direction") {
    CropGesture g;
    REQUIRE(g.begin(CropMode::Draw, -1, {100, 100}, {0, 0, kDocW, kDocH}));
    CHECK(near(g.rectFor({160, 140}, 0, false, false, kDocW, kDocH), {100, 100, 60, 40}));
    // Dragging up-left of the anchor flips the rect.
    CHECK(near(g.rectFor({40, 60}, 0, false, false, kDocW, kDocH), {40, 60, 60, 40}));
    // Past the document is allowed (S16-f expansion) — only the safety envelope bounds it.
    CHECK(near(g.rectFor({1000, 1000}, 0, false, false, kDocW, kDocH),
               {100, 100, 900, 900}));
}

TEST_CASE("draw: the snap band pulls edges onto the canvas bounds; far past it stays free") {
    CropGesture g;
    REQUIRE(g.begin(CropMode::Draw, -1, {100, 100}, {0, 0, kDocW, kDocH}));
    // 2px and 3px outside the corner, tol 6: both axes snap exactly onto the document edge.
    CHECK(near(g.rectFor({kDocW + 2, kDocH - 3}, 0, false, false, kDocW, kDocH, 6.0),
               {100, 100, kDocW - 100, kDocH - 100}));
    // Near the zero edge the nearest bound wins.
    CHECK(near(g.rectFor({2, 150}, 0, false, false, kDocW, kDocH, 6.0), {0, 100, 100, 50}));
    // Beyond the band nothing snaps: this rect stages a genuine expansion.
    CHECK(near(g.rectFor({kDocW + 100, kDocH + 50}, 0, false, false, kDocW, kDocH, 6.0),
               {100, 100, kDocW, kDocH - 50}));
}

TEST_CASE("draw: Shift squares a free draw; an explicit ratio constrains it") {
    CropGesture g;
    REQUIRE(g.begin(CropMode::Draw, -1, {100, 100}, {0, 0, kDocW, kDocH}));
    // Shift: square from the dominant dimension (60 > 40).
    CHECK(near(g.rectFor({160, 140}, 0, true, false, kDocW, kDocH), {100, 100, 60, 60}));
    // 2:1 ratio: width 60 dominates (60/2 = 30 >= 40? no -> height 40 wins, w = 80).
    CHECK(near(g.rectFor({160, 140}, 2.0, false, false, kDocW, kDocH), {100, 100, 80, 40}));
    // The ratio survives the ENVELOPE clamp (a runaway drag): bounded, ratio held.
    const double out = mosaic::ui::kCropOutsetFactor * kDocW; // the larger dimension
    const Rect r = g.rectFor({5000, 5000}, 2.0, false, false, kDocW, kDocH);
    CHECK(r.w == doctest::Approx(2.0 * r.h));
    CHECK(r.right() <= kDocW + out + 1e-9);
    CHECK(r.bottom() <= kDocH + out + 1e-9);
}

TEST_CASE("draw: Alt grows symmetrically about the press point") {
    CropGesture g;
    REQUIRE(g.begin(CropMode::Draw, -1, {100, 100}, {0, 0, kDocW, kDocH}));
    CHECK(near(g.rectFor({130, 120}, 0, false, true, kDocW, kDocH), {70, 80, 60, 40}));
    // Symmetric growth may reach past the document (expansion); the envelope is the bound.
    const Rect r = g.rectFor({350, 120}, 0, false, true, kDocW, kDocH);
    CHECK(near(r, {-150, 80, 500, 40}));
}

TEST_CASE("move: translates freely to the envelope; Shift locks the axis") {
    CropGesture g;
    REQUIRE(g.begin(CropMode::Move, -1, {100, 100}, {50, 50, 100, 80}));
    CHECK(near(g.rectFor({130, 110}, 0, false, false, kDocW, kDocH), {80, 60, 100, 80}));
    // Far past the corner the ENVELOPE bounds the rect (kCropOutsetFactor x 400 = 800 out).
    CHECK(near(g.rectFor({-500, -500}, 0, false, false, kDocW, kDocH),
               {-550, -550, 100, 80}));
    CHECK(near(g.rectFor({5000, 5000}, 0, false, false, kDocW, kDocH),
               {kDocW + 800 - 100, kDocH + 800 - 80, 100, 80}));
    // Shift: the dominant axis only.
    CHECK(near(g.rectFor({140, 110}, 0, true, false, kDocW, kDocH), {90, 50, 100, 80}));
}

TEST_CASE("move: the snap band aligns the rect to canvas edges by translation") {
    CropGesture g;
    REQUIRE(g.begin(CropMode::Move, -1, {100, 100}, {50, 50, 100, 80}));
    // Origin lands at -4: within tol of 0 -> the leading edge sits exactly on the canvas.
    CHECK(near(g.rectFor({46, 100}, 0, false, false, kDocW, kDocH, 6.0), {0, 50, 100, 80}));
    // Origin lands at 297: within tol of W-w=300 -> the trailing edge kisses the right bound.
    CHECK(near(g.rectFor({347, 100}, 0, false, false, kDocW, kDocH, 6.0), {300, 50, 100, 80}));
    // Well outside the band: no snap, the offset is staged as-is.
    CHECK(near(g.rectFor({150, 100}, 0, false, false, kDocW, kDocH, 6.0), {100, 50, 100, 80}));
}

TEST_CASE("resize: corner handles anchor at the opposite corner") {
    CropGesture g;
    // Handle 2 = BR; anchor = TL (50, 50).
    REQUIRE(g.begin(CropMode::Resize, 2, {150, 130}, {50, 50, 100, 80}));
    CHECK(near(g.rectFor({170, 150}, 0, false, false, kDocW, kDocH), {50, 50, 120, 100}));
    // Crossing the anchor flips the rect.
    CHECK(near(g.rectFor({30, 20}, 0, false, false, kDocW, kDocH), {30, 20, 20, 30}));
    // Shift keeps the rect's own 100:80 aspect; the wider pull dominates.
    const Rect r = g.rectFor({250, 130}, 0, true, false, kDocW, kDocH);
    CHECK(r.w == doctest::Approx(200));
    CHECK(r.h == doctest::Approx(160));
    CHECK(near(r, {50, 50, 200, 160}, 1e-6));
}

TEST_CASE("resize: edge handles drive one axis; a ratio re-centres the other") {
    CropGesture g;
    // Handle 6 = bottom edge; anchor = the top edge (y = 50). x never moves, free ratio.
    REQUIRE(g.begin(CropMode::Resize, 6, {100, 130}, {50, 50, 100, 80}));
    CHECK(near(g.rectFor({777, 170}, 0, false, false, kDocW, kDocH), {50, 50, 100, 120}));
    // With a 1:1 ratio the width follows the height, centred on the rect's x-centre (100).
    const Rect r = g.rectFor({777, 170}, 1.0, false, false, kDocW, kDocH);
    CHECK(near(r, {40, 50, 120, 120}));
}

TEST_CASE("resize: Alt resizes around the rect's centre") {
    CropGesture g;
    // Handle 5 = right edge; base centre x = 100. Pulling to x=160 doubles the half-width.
    REQUIRE(g.begin(CropMode::Resize, 5, {150, 90}, {50, 50, 100, 80}));
    CHECK(near(g.rectFor({160, 90}, 0, false, true, kDocW, kDocH), {40, 50, 120, 80}));
}

TEST_CASE("resize: a tiny pull keeps at least a ~1px rect") {
    CropGesture g;
    REQUIRE(g.begin(CropMode::Resize, 2, {150, 130}, {50, 50, 100, 80}));
    const Rect r = g.rectFor({50.2, 50.2}, 0, false, false, kDocW, kDocH);
    CHECK(r.w >= 1.0);
    CHECK(r.h >= 1.0);
}

TEST_CASE("cropRatioForOptions: presets, Original, and Swap") {
    CHECK(cropRatioForOptions(0, false, kDocW, kDocH) == 0.0); // Free
    CHECK(cropRatioForOptions(1, false, kDocW, kDocH) == doctest::Approx(400.0 / 300.0));
    CHECK(cropRatioForOptions(2, false, kDocW, kDocH) == doctest::Approx(1.0));
    CHECK(cropRatioForOptions(3, false, kDocW, kDocH) == doctest::Approx(4.0 / 3.0));
    CHECK(cropRatioForOptions(4, true, kDocW, kDocH) == doctest::Approx(9.0 / 16.0));
    CHECK(cropRatioForOptions(5, false, kDocW, kDocH) == doctest::Approx(3.0 / 2.0));
    CHECK(cropRatioForOptions(0, true, kDocW, kDocH) == 0.0); // Swap can't flip Free
    // "Custom" has no preset ratio: cropRatioForOptions falls back to free (the caller reads the
    // ratioW/ratioH fields via customCropRatio instead).
    CHECK(cropRatioForOptions(kCropRatioCustom, false, kDocW, kDocH) == 0.0);
}

TEST_CASE("customCropRatio: the ratioW:ratioH fields, with Swap and the degenerate guard") {
    CHECK(customCropRatio(16.0, 9.0, false) == doctest::Approx(16.0 / 9.0));
    CHECK(customCropRatio(16.0, 9.0, true) == doctest::Approx(9.0 / 16.0)); // Swap flips orientation
    CHECK(customCropRatio(2.39, 1.0, false) == doctest::Approx(2.39));      // floats, no max
    CHECK(customCropRatio(0.0, 9.0, false) == 0.0); // a zero component -> free (no div-by-zero)
    CHECK(customCropRatio(16.0, 0.0, true) == 0.0); // ...and Swap can't flip a free ratio
}

TEST_CASE("conformCropRect: keeps the centre and area, clamps into the document") {
    // A 100x100 at centre (100,100) conformed to 4:1 keeps area 10000 -> 200x50.
    const Rect r = conformCropRect({50, 50, 100, 100}, 4.0, kDocW, kDocH);
    CHECK(r.w == doctest::Approx(200));
    CHECK(r.h == doctest::Approx(50));
    CHECK(r.center().x == doctest::Approx(100));
    CHECK(r.center().y == doctest::Approx(100));
    // Free ratio: untouched.
    CHECK(near(conformCropRect({50, 50, 100, 100}, 0.0, kDocW, kDocH), {50, 50, 100, 100}));
    // An INSIDE near-document rect conformed to a wide ratio can't exceed the document (entry
    // and ratio switches keep their historical stay-inside behaviour).
    const Rect wide = conformCropRect({0, 0, kDocW, kDocH}, 10.0, kDocW, kDocH);
    CHECK(wide.w <= kDocW + 1e-9);
    CHECK(wide.right() <= kDocW + 1e-9);
    CHECK(wide.w == doctest::Approx(10.0 * wide.h));
}

TEST_CASE("conformCropRect: a rect staging an expansion keeps its outset across a ratio switch") {
    // Partially outside: conform must NOT pull it back into the canvas, only hold the ratio.
    const Rect r = conformCropRect({-50, -50, 500, 400}, 2.0, kDocW, kDocH);
    CHECK(r.w == doctest::Approx(2.0 * r.h));
    CHECK(r.center().x == doctest::Approx(200)); // centre kept
    CHECK(r.center().y == doctest::Approx(150));
    CHECK(r.x < 0.0); // still an expansion
}

TEST_CASE("crop frame mapping: round-trips, identity at angle 0, exact 90-degree corners") {
    using mosaic::ui::cropBoxCorners;
    using mosaic::ui::cropFrameToDoc;
    using mosaic::ui::docToCropFrame;
    const Vec2 pivot{20, 15};
    const Vec2 p{7.5, 42.25};
    // Identity at 0.
    CHECK(cropFrameToDoc(p, 0.0, pivot).x == p.x);
    CHECK(cropFrameToDoc(p, 0.0, pivot).y == p.y);
    // Round-trip at an arbitrary angle.
    const Vec2 rt = docToCropFrame(cropFrameToDoc(p, 0.7, pivot), 0.7, pivot);
    CHECK(rt.x == doctest::Approx(p.x));
    CHECK(rt.y == doctest::Approx(p.y));
    // Exact 90 degrees: TL (10,10) about (20,15) -> (25,5).
    const auto c = cropBoxCorners({10, 10, 20, 10}, std::numbers::pi / 2.0, pivot);
    CHECK(c[0].x == doctest::Approx(25.0));
    CHECK(c[0].y == doctest::Approx(5.0));
    CHECK(c[2].x == doctest::Approx(15.0)); // BR (30,20) -> (15,25)
    CHECK(c[2].y == doctest::Approx(25.0));
}

TEST_CASE("snapCropRect: rounds edges, bounds by the envelope, and never collapses") {
    CHECK(snapCropRect({10.4, 19.6, 99.8, 50.2}, 400, 300) == CropPixels{10, 20, 100, 50});
    // Beyond-canvas rects survive (staged expansion; the 800px envelope is the only bound).
    CHECK(snapCropRect({-5, -5, 1000, 1000}, 400, 300) == CropPixels{-5, -5, 1000, 1000});
    CHECK(snapCropRect({-10000, 0, 50000, 50}, 400, 300) == CropPixels{-800, 0, 2000, 50});
    CHECK(snapCropRect({100, 100, 0.1, 0.1}, 400, 300) == CropPixels{100, 100, 1, 1});
    CHECK(snapCropRect({0, 0, 10, 10}, 0, 0) == CropPixels{});
}

TEST_CASE("snapCropRect: a pure translate never changes the snapped size (S16-h)") {
    // A Move only translates the staged rect (constant w/h); sweeping the origin across sub-pixel
    // positions must not flicker the reported W/H — the move-jitter bug was independent rounding of
    // the two edges (lround(x) vs lround(x+w) step at different thresholds).
    const Rect base{10.3, 20.7, 99.6, 50.4};
    const CropPixels ref = snapCropRect(base, 400, 300);
    CHECK(ref.w == 100); // lround(99.6)
    CHECK(ref.h == 50);  // lround(50.4)
    for (int i = 0; i <= 10; ++i) {
        const double off = i * 0.1; // 0.0 .. 1.0, crossing every rounding boundary
        const CropPixels moved =
            snapCropRect({base.x + off, base.y + off, base.w, base.h}, 400, 300);
        CHECK(moved.w == ref.w);
        CHECK(moved.h == ref.h);
    }
}
