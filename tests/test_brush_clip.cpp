#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/brush/brush_engine.hpp"
#include "core/selection.hpp"
#include "core/stroke_confinement.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <memory>
#include <vector>

// SELECTION CONFINEMENT (core/stroke_confinement.hpp): a stroke may not deposit outside the active
// selection, and it deposits a PROPORTIONAL share inside a feathered one.
//
// The load-bearing property here is the negative one -- a stroke with no selection is byte-for-byte
// the stroke the engine laid before confinement existed -- because every brush golden in the suite
// rests on it. It is pinned twice: "no selection" produces no confinement object at all (so not one
// instruction of this runs), and a FULLY-selected field is exactly the identity (255/255 == 1.0 to
// the bit, which is why StrokeConfinement::at divides instead of multiplying by a reciprocal).
namespace {

using mosaic::common::Affine2D;
using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::common::Rect;
using mosaic::core::makeStrokeConfinement;
using mosaic::core::Selection;
using mosaic::core::StrokeConfinement;
using mosaic::core::brush::BrushDynamics;
using mosaic::core::brush::BrushEngine;
using mosaic::core::brush::BrushParams;
using mosaic::core::brush::StrokeInput;

constexpr std::uint32_t kW = 64;
constexpr std::uint32_t kH = 64;

std::uint8_t alphaAt(const Image& img, int x, int y) {
    return img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4 + 3];
}

// A confinement field covering the whole target at one coverage value.
std::shared_ptr<const StrokeConfinement> uniformField(std::uint8_t v) {
    auto c = std::make_shared<StrokeConfinement>();
    c->w = kW;
    c->h = kH;
    c->v.assign(static_cast<std::size_t>(kW) * kH, v);
    return c;
}

// The same stroke every time: a horizontal drag across the middle with a hard, opaque tip, so the
// core of the mark is coverage 1 and the arithmetic under test is the only thing that moves it.
Image paintStroke(std::shared_ptr<const StrokeConfinement> confine) {
    BrushParams p;
    p.diameter = 14.0;
    p.hardness = 1.0;
    p.flow = 1.0;
    p.opacity = 1.0;
    p.color = Color8{200, 30, 40, 255};
    p.confine = std::move(confine);

    Image img(kW, kH);
    BrushEngine eng;
    const std::vector<StrokeInput> path{StrokeInput{{12.0, 32.0}, 1.0},
                                        StrokeInput{{32.0, 32.0}, 1.0},
                                        StrokeInput{{52.0, 32.0}, 1.0}};
    eng.begin(kW, kH, img, p, BrushDynamics{}, path.front());
    for (std::size_t i = 1; i < path.size(); ++i)
        eng.extendTo(path[i]);
    eng.flush(); // the walk lags one sample; lay the tail span before reading the pixels
    eng.composite();
    eng.end();
    return img;
}

} // namespace

TEST_CASE("no selection means no confinement object at all") {
    // "No selection" is an EMPTY Selection and means everything is editable. The factory returns
    // null for it, which is what keeps an unconfined stroke free of every branch, lookup and
    // multiply confinement would otherwise add.
    CHECK(makeStrokeConfinement(Selection{}, Affine2D::identity(), kW, kH) == nullptr);

    // An ACTIVE selection covering nothing is a different thing: it selects nothing, so the field
    // exists and reads 0 everywhere.
    const Selection nothing(kW, kH); // all-zero mask
    const auto empty = makeStrokeConfinement(nothing, Affine2D::identity(), kW, kH);
    REQUIRE(empty != nullptr);
    CHECK(empty->at(32, 32) == doctest::Approx(0.0));
}

TEST_CASE("a fully selected field is the exact identity -- the unconfined stroke, byte for byte") {
    const Image plain = paintStroke(nullptr);
    const Image confined = paintStroke(uniformField(255));
    REQUIRE(plain.rgba.size() == confined.rgba.size());
    // Byte-for-byte, not "close": 255/255 is exactly 1.0 in IEEE, so the confined composite runs
    // the identical expression. A reciprocal multiply here would land an ulp under 1.0 and move
    // whichever pixels sit on a rounding boundary -- which is the whole reason at() divides.
    CHECK(plain.rgba == confined.rgba);
    CHECK(alphaAt(plain, 32, 32) == 255); // ... and the stroke actually painted something
}

TEST_CASE("confinement is a coverage MULTIPLY, not a clip: a half-selected pixel takes half") {
    const Image plain = paintStroke(nullptr);
    const Image half = paintStroke(uniformField(128));

    const std::uint8_t full = alphaAt(plain, 32, 32);
    const std::uint8_t got = alphaAt(half, 32, 32);
    REQUIRE(full == 255); // the tip's core is solid, so the pin below is on a known quantity

    // NOT binary: a partially selected pixel is neither skipped nor painted whole.
    CHECK(got > 0);
    CHECK(got < full);
    // 128/255 of the stroke's alpha, to the rounding: sa = 1.0 * (128/255) -> 128.
    CHECK(static_cast<int>(got) == 128);

    // ... and it scales, rather than thresholding somewhere.
    CHECK(static_cast<int>(alphaAt(paintStroke(uniformField(64)), 32, 32)) == 64);
    CHECK(static_cast<int>(alphaAt(paintStroke(uniformField(192)), 32, 32)) == 192);
}

TEST_CASE("a stroke deposits nothing outside the selection, and leaves those bytes pristine") {
    // A field that selects only the left half of the target.
    auto left = std::make_shared<StrokeConfinement>();
    left->w = kW;
    left->h = kH;
    left->v.assign(static_cast<std::size_t>(kW) * kH, 0);
    for (std::uint32_t y = 0; y < kH; ++y)
        for (std::uint32_t x = 0; x < 32; ++x)
            left->v[static_cast<std::size_t>(y) * kW + x] = 255;

    const Image plain = paintStroke(nullptr);
    const Image clipped = paintStroke(left);

    // Inside: identical to the unconfined stroke (255 is the identity).
    CHECK(alphaAt(clipped, 20, 32) == alphaAt(plain, 20, 32));
    CHECK(alphaAt(clipped, 20, 32) > 0);
    // Outside: the stroke painted there unconfined, and here it did not touch a byte. The target
    // began transparent, so "untouched" is checkable directly.
    CHECK(alphaAt(plain, 45, 32) > 0);
    for (int x = 32; x < 64; ++x)
        CHECK(alphaAt(clipped, x, 32) == 0);
}

TEST_CASE("the coverage the Inpaint brush reads is confined too") {
    // The Inpaint brush reads the engine's coverage buffer as its hole mask, so confinement has to
    // reach it as well or the two would disagree about where the stroke went.
    auto left = std::make_shared<StrokeConfinement>();
    left->w = kW;
    left->h = kH;
    left->v.assign(static_cast<std::size_t>(kW) * kH, 0);
    for (std::uint32_t y = 0; y < kH; ++y)
        for (std::uint32_t x = 0; x < 32; ++x)
            left->v[static_cast<std::size_t>(y) * kW + x] = 255;

    BrushParams p;
    p.diameter = 14.0;
    p.hardness = 1.0;
    p.flow = 1.0;
    p.opacity = 1.0;
    p.confine = left;

    Image img(kW, kH);
    BrushEngine eng;
    eng.begin(kW, kH, img, p, BrushDynamics{}, StrokeInput{{12.0, 32.0}, 1.0});
    eng.extendTo(StrokeInput{{32.0, 32.0}, 1.0});
    eng.extendTo(StrokeInput{{52.0, 32.0}, 1.0});
    eng.flush();
    eng.composite();
    eng.end();

    const std::vector<float>& cov = eng.coverage();
    const std::uint32_t cw = eng.coverageWidth();
    const std::uint32_t ch = eng.coverageHeight();
    const std::int32_t ox = eng.coverageOriginX();
    // No `oy`: this field is a half-plane in X, so the row index never participates in the check.
    REQUIRE(cov.size() == static_cast<std::size_t>(cw) * ch);
    bool anyInside = false;
    for (std::uint32_t cy = 0; cy < ch; ++cy) {
        for (std::uint32_t cx = 0; cx < cw; ++cx) {
            const int x = ox + static_cast<int>(cx);
            const float c = cov[static_cast<std::size_t>(cy) * cw + cx];
            if (x >= 32)
                CHECK(c == 0.0f); // unselected: no coverage recorded at all
            else if (c > 0.0f)
                anyInside = true;
        }
    }
    CHECK(anyInside);
}

TEST_CASE("makeStrokeConfinement resamples the document selection onto the target grid") {
    // A 16x16 marquee in the top-left of a 64x64 document.
    const Selection sel = Selection::rectangle(kW, kH, Rect{8.0, 8.0, 16.0, 16.0});
    REQUIRE(!sel.isEmpty());

    SUBCASE("1:1 -- an untransformed layer on the document's own grid") {
        const auto f = makeStrokeConfinement(sel, Affine2D::identity(), kW, kH);
        REQUIRE(f != nullptr);
        CHECK(f->at(12, 12) == doctest::Approx(1.0));
        CHECK(f->at(2, 2) == doctest::Approx(0.0));
        CHECK(f->at(40, 40) == doctest::Approx(0.0));
        // The window is the selection's own bounds (plus the sampling dilation), never the layer.
        CHECK(f->w < kW);
        CHECK(f->h < kH);
    }

    SUBCASE("a translated layer -- the selection is a DOCUMENT-space field") {
        // The layer sits at document (8,8), so its own pixel (0,0) is the marquee's corner.
        const auto f = makeStrokeConfinement(sel, Affine2D::translation(8.0, 8.0), kW, kH);
        REQUIRE(f != nullptr);
        CHECK(f->at(4, 4) == doctest::Approx(1.0));  // layer (4,4) -> doc (12,12): inside
        CHECK(f->at(20, 20) == doctest::Approx(0.0)); // layer (20,20) -> doc (28,28): outside
    }

    SUBCASE("a singular placement selects nothing rather than dividing by zero") {
        const auto f = makeStrokeConfinement(sel, Affine2D::scaling(0.0, 0.0), kW, kH);
        REQUIRE(f != nullptr);
        CHECK(f->at(12, 12) == doctest::Approx(0.0));
    }
}

TEST_CASE("a feathered selection reaches the stroke as a genuine ramp, not a threshold") {
    // Feather is the case a hard clip would silently destroy: the mask is fractional, so the paint
    // has to be too.
    const Selection sel =
        Selection::rectangle(kW, kH, Rect{16.0, 8.0, 32.0, 48.0}).feathered(4.0);
    REQUIRE(!sel.isEmpty());
    const auto f = makeStrokeConfinement(sel, Affine2D::identity(), kW, kH);
    REQUIRE(f != nullptr);

    // Somewhere across the feathered edge there must be a value that is neither 0 nor 1 -- the
    // whole point, and the thing a kAntsCoverageThreshold-style hard test would erase.
    int fractional = 0;
    for (int x = 8; x < 26; ++x) {
        const double k = f->at(x, 32);
        if (k > 0.02 && k < 0.98)
            ++fractional;
    }
    CHECK(fractional > 0);

    // ... and the painted alpha tracks it: monotone across the ramp, strictly between the outside
    // and the inside.
    const Image img = paintStroke(f);
    const std::uint8_t inside = alphaAt(img, 32, 32);
    CHECK(inside > 0);
    bool sawPartial = false;
    for (int x = 8; x < 26; ++x) {
        const double k = f->at(x, 32);
        const std::uint8_t a = alphaAt(img, x, 32);
        if (k > 0.05 && k < 0.95 && a > 0 && a < inside)
            sawPartial = true;
    }
    CHECK(sawPartial);
}
