#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/brush/brush_engine.hpp"
#include "core/command.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/layer_grow.hpp"

#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <limits>
#include <memory>

// BOUNDED LAYER GROWTH (core/layer_grow.hpp) -- the brush's "grow the layer as the stroke paints
// outside it, up to the canvas size", and the guards that make an absurd pointer coordinate cost
// nothing.
//
// The failure being prevented is stated plainly in the request: a stroke at coordinate ten trillion
// must not size an allocation. So the tests below are as much about what does NOT happen (no growth,
// no cells, no undefined cast) as about what does.
using namespace mosaic; // `common::copyRegion` reads as itself here, as it does in the sources

namespace {

using mosaic::common::Affine2D;
using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::common::Rect;
using mosaic::common::Vec2;
using mosaic::core::brushGrowthBox;
using mosaic::core::canvasBoxInLayer;
using mosaic::core::clampStrokePos;
using mosaic::core::Document;
using mosaic::core::GrowAndPaintLayerCommand;
using mosaic::core::kMaxLayerCells;
using mosaic::core::kMaxLayerCoord;
using mosaic::core::kMaxLayerSide;
using mosaic::core::layerPixelBox;
using mosaic::core::LayerId;
using mosaic::core::PixelBox;
using mosaic::core::pixelBoxCovering;
using mosaic::core::RasterLayer;
using mosaic::core::RasterMask;
using mosaic::core::worldTransform;
using mosaic::core::brush::BrushDynamics;
using mosaic::core::brush::BrushEngine;
using mosaic::core::brush::BrushParams;
using mosaic::core::brush::StrokeInput;

constexpr double kAbsurd = 1.0e13; // the user's own number: "coordinate 10 trillion"

Color8 pixel(const Image& img, std::uint32_t x, std::uint32_t y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

void setPixel(Image& img, std::uint32_t x, std::uint32_t y, Color8 c) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    img.rgba[p] = c.r;
    img.rgba[p + 1] = c.g;
    img.rgba[p + 2] = c.b;
    img.rgba[p + 3] = c.a;
}

} // namespace

TEST_CASE("pixelBoxCovering saturates its corners BEFORE they become integers") {
    // The ordinary case is exact.
    CHECK(pixelBoxCovering(Rect{2.25, 3.75, 4.0, 4.0}) == PixelBox{2, 3, 7, 8});

    // ⚠ 1e13 does not fit in an `int`, and `static_cast<int>` of it is undefined behaviour rather
    // than a wrap. The saturation collapses both corners onto the same limit, so the box comes back
    // EMPTY -- nothing to allocate, nothing to cast, nothing to trim.
    const PixelBox absurd = pixelBoxCovering(Rect{kAbsurd, kAbsurd, 10.0, 10.0});
    CHECK(absurd.empty());
    CHECK(absurd.cells() == 0);

    // A rect straddling the whole real line clamps to the limits and stays sane.
    const PixelBox huge = pixelBoxCovering(Rect{-kAbsurd, -kAbsurd, 2.0 * kAbsurd, 2.0 * kAbsurd});
    CHECK(huge.x0 == -kMaxLayerCoord);
    CHECK(huge.x1 == kMaxLayerCoord);

    // Non-finite corners (what a near-singular inverse produces) have no integer image at all.
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(pixelBoxCovering(Rect{0.0, 0.0, inf, inf}).empty());
    CHECK(pixelBoxCovering(Rect{nan, nan, 4.0, 4.0}).empty());
}

TEST_CASE("canvasBoxInLayer places the canvas in the layer's own grid") {
    CHECK(canvasBoxInLayer(Affine2D::identity(), 256, 256) == PixelBox{0, 0, 256, 256});
    // A layer whose own (0,0) sits at document (100,100): the canvas starts 100 px to its left.
    CHECK(canvasBoxInLayer(Affine2D::translation(100.0, 100.0), 256, 256) ==
          PixelBox{-100, -100, 156, 156});
    // A singular placement has no layer-space image of the canvas.
    CHECK(canvasBoxInLayer(Affine2D::scaling(0.0, 0.0), 256, 256).empty());
    CHECK(canvasBoxInLayer(Affine2D::identity(), 0, 0).empty());
}

TEST_CASE("brushGrowthBox: growth stops exactly at the canvas") {
    const PixelBox canvas = canvasBoxInLayer(Affine2D::translation(10.0, 20.0), 256, 256);
    REQUIRE(canvas == PixelBox{-10, -20, 246, 236});

    // Asking for the whole canvas grows to exactly the canvas -- 256 x 256, not one pixel more.
    const PixelBox grown = brushGrowthBox(64, 64, canvas, canvas);
    CHECK(grown == PixelBox{-10, -20, 246, 236});
    CHECK(grown.width() == 256);
    CHECK(grown.height() == 256);

    // Asking for MORE than the canvas gets the canvas: the ceiling is the ceiling.
    CHECK(brushGrowthBox(64, 64, PixelBox{-9999, -9999, 9999, 9999}, canvas) == grown);

    // Asking for a box inside the layer grows nothing at all.
    CHECK(brushGrowthBox(64, 64, PixelBox{4, 4, 20, 20}, canvas) == layerPixelBox(64, 64));

    // Asking for a strip off one edge grows to cover the strip and no further.
    CHECK(brushGrowthBox(64, 64, PixelBox{60, 10, 90, 30}, canvas) == PixelBox{0, 0, 90, 64});
}

TEST_CASE("brushGrowthBox never shrinks a layer that already reaches past the canvas") {
    const PixelBox canvas{0, 0, 256, 256};
    // A 512x512 layer keeps every pixel it has, whatever is requested.
    CHECK(brushGrowthBox(512, 512, canvas, canvas) == layerPixelBox(512, 512));
    CHECK(brushGrowthBox(512, 512, PixelBox{10, 10, 20, 20}, canvas) == layerPixelBox(512, 512));
    // ... and an empty canvas box (singular placement) is simply no growth.
    CHECK(brushGrowthBox(64, 64, PixelBox{-100, -100, 100, 100}, PixelBox{}) ==
          layerPixelBox(64, 64));
}

TEST_CASE("an absurd requested box allocates NOTHING -- the guard runs before the arithmetic") {
    const PixelBox canvas{0, 0, 256, 256};

    // THE CASE THE USER ASKED FOR. The stroke wants a box ten trillion pixels out. If the request
    // were unioned with the layer before being clamped, the result would be ~1e13 px a side and the
    // caller would allocate it (or wrap trying). Intersecting FIRST leaves nothing inside the
    // ceiling, so the layer keeps its own box and the cost is two comparisons.
    const long absurd = static_cast<long>(kAbsurd);
    const PixelBox far{absurd, absurd, absurd + 40, absurd + 40};
    const PixelBox grown = brushGrowthBox(64, 64, far, canvas);
    CHECK(grown == layerPixelBox(64, 64));
    CHECK(grown.cells() == 64 * 64);
    CHECK(grown.width() <= 256);
    CHECK(grown.height() <= 256);

    // The same the other way (a negative absurdity), and inside-out -- a box whose width is
    // NEGATIVE, which is the shape that wraps to ~4e9 when a caller makes it unsigned.
    CHECK(brushGrowthBox(64, 64, PixelBox{-absurd - 40, -absurd - 40, -absurd, -absurd}, canvas) ==
          layerPixelBox(64, 64));
    CHECK(brushGrowthBox(64, 64, PixelBox{200, 200, 10, 10}, canvas) == layerPixelBox(64, 64));

    // And a canvas whose layer-space image is itself past the ceiling (a layer scaled down so far
    // that the canvas covers millions of its pixels) is REFUSED rather than allocated.
    const PixelBox vast{0, 0, kMaxLayerSide + 1, kMaxLayerSide + 1};
    CHECK(brushGrowthBox(64, 64, vast, vast) == layerPixelBox(64, 64));
    const long side = 1L << 15; // 32768^2 == 2^30 cells, past kMaxLayerCells
    const PixelBox wide{0, 0, side, side};
    CHECK(wide.cells() > kMaxLayerCells);
    CHECK(brushGrowthBox(64, 64, wide, wide) == layerPixelBox(64, 64));
}

TEST_CASE("clampStrokePos saturates a stroke sample without moving a real one") {
    // Anything remotely on-canvas passes through BIT-identical -- no stroke's geometry moves.
    CHECK(clampStrokePos(Vec2{12.5, -3.25}).x == 12.5);
    CHECK(clampStrokePos(Vec2{12.5, -3.25}).y == -3.25);
    CHECK(clampStrokePos(Vec2{-40000.0, 90000.5}).y == 90000.5);

    // The absurd coordinate becomes something the engine's `static_cast<int>` is defined on.
    const Vec2 far = clampStrokePos(Vec2{kAbsurd, -kAbsurd});
    CHECK(std::isfinite(far.x));
    CHECK(std::isfinite(far.y));
    CHECK(far.x <= static_cast<double>(kMaxLayerCoord));
    CHECK(far.y >= -static_cast<double>(kMaxLayerCoord));
    CHECK(far.x < static_cast<double>(std::numeric_limits<int>::max()));

    // NaN/inf have no place on a stroke path.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    CHECK(clampStrokePos(Vec2{nan, inf}).x == 0.0);
    CHECK(clampStrokePos(Vec2{nan, inf}).y == 0.0);
}

TEST_CASE("a stroke at an absurd coordinate paints nothing and allocates nothing") {
    // End to end on the engine, through the same clamp the canvas applies at the seam: the stroke
    // is entirely off the layer, so no dab lands, no working rect is grown, no coverage exists, and
    // the target is untouched. Without the clamp, the engine's dab-box cast would be undefined --
    // which a sanitizer build catches here rather than in the field.
    Image img(64, 64);
    BrushParams p;
    p.diameter = 24.0;
    p.hardness = 1.0;
    p.color = Color8{255, 255, 255, 255};

    BrushEngine eng;
    eng.begin(64, 64, img, p, BrushDynamics{},
              StrokeInput{clampStrokePos(Vec2{kAbsurd, kAbsurd}), 1.0});
    eng.extendTo(StrokeInput{clampStrokePos(Vec2{kAbsurd + 50.0, kAbsurd}), 1.0});
    eng.extendTo(StrokeInput{clampStrokePos(Vec2{-kAbsurd, kAbsurd}), 1.0});
    eng.flush();
    eng.composite();
    eng.end();

    CHECK(eng.coverageWidth() == 0);
    CHECK(eng.coverageHeight() == 0);
    CHECK(eng.coverage().empty()); // not one working-rect cell was allocated
    CHECK(eng.dirtyBounds().empty());
    for (std::uint8_t b : img.rgba)
        CHECK(b == 0);
}

TEST_CASE("GrowAndPaintLayerCommand grows, keeps the old pixels byte-exact, and does not shift") {
    Document doc(256, 256);
    auto layerOwned = doc.makeRaster("small", 64, 64);
    layerOwned->setTransform(Affine2D::translation(10.0, 20.0));
    // A recognisable pattern the growth has to carry across verbatim.
    Image& src = layerOwned->image();
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 64; ++x)
            setPixel(src, x, y,
                     Color8{static_cast<std::uint8_t>(x * 4), static_cast<std::uint8_t>(y * 4),
                            17, 255});
    const Image before = src;
    const Affine2D beforeXform = layerOwned->transform();
    const LayerId id = layerOwned->id();
    doc.root().addOnTop(std::move(layerOwned));

    // The growth the canvas would plan: the layer united with the canvas in its own grid.
    const RasterLayer* layer = doc.find(id)->as<RasterLayer>();
    REQUIRE(layer != nullptr);
    const PixelBox canvas = canvasBoxInLayer(worldTransform(*layer), doc.width(), doc.height());
    const PixelBox keep = brushGrowthBox(64, 64, canvas, canvas);
    REQUIRE(keep == PixelBox{-10, -20, 246, 236});

    // One painted pixel, in the NEW grid's coordinates (the layer's old (0,0) lands at (10,20)).
    Image region(1, 1);
    setPixel(region, 0, 0, Color8{9, 9, 9, 255});
    const Vec2 docPointOfOldOrigin = worldTransform(*layer).apply({0.5, 0.5});

    doc.commands().push(std::make_unique<GrowAndPaintLayerCommand>(
        id, static_cast<std::uint32_t>(keep.width()), static_cast<std::uint32_t>(keep.height()),
        -keep.x0, -keep.y0, std::move(region), 200, 200));

    const RasterLayer* grown = doc.find(id)->as<RasterLayer>();
    REQUIRE(grown != nullptr);
    CHECK(grown->image().width == 256);
    CHECK(grown->image().height == 256);

    // The existing pixels survive BYTE-EXACT at the growth offset ...
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 64; ++x)
            CHECK(pixel(grown->image(), x + 10, y + 20) == pixel(before, x, y));
    // ... the new band is empty ...
    CHECK(pixel(grown->image(), 0, 0).a == 0);
    // ... the stroke's region landed where it was told ...
    CHECK(pixel(grown->image(), 200, 200) == Color8{9, 9, 9, 255});
    // ... and NOTHING MOVED: the old (0,0) pixel centre still maps to the same document point.
    const Vec2 after = worldTransform(*grown).apply({10.5, 20.5});
    CHECK(after.x == doctest::Approx(docPointOfOldOrigin.x));
    CHECK(after.y == doctest::Approx(docPointOfOldOrigin.y));

    // Undo restores the grid, the pixels and the placement verbatim.
    doc.commands().undo();
    const RasterLayer* back = doc.find(id)->as<RasterLayer>();
    REQUIRE(back != nullptr);
    CHECK(back->image().width == 64);
    CHECK(back->image().height == 64);
    CHECK(back->image().rgba == before.rgba);
    CHECK(back->transform() == beforeXform);

    // ... and redo puts the growth back.
    doc.commands().redo();
    CHECK(doc.find(id)->as<RasterLayer>()->image().width == 256);
}

TEST_CASE("GrowAndPaintLayerCommand carries a layer mask with the grid") {
    Document doc(128, 128);
    auto layerOwned = doc.makeRaster("masked", 32, 32);
    RasterMask mask(32, 32, 0);
    mask.coverage[static_cast<std::size_t>(5) * 32 + 6] = 200;
    mask.linked = false;
    layerOwned->setMask(mask);
    const LayerId id = layerOwned->id();
    doc.root().addOnTop(std::move(layerOwned));

    Image region(1, 1);
    setPixel(region, 0, 0, Color8{1, 2, 3, 255});
    // Grow to the whole canvas with the old grid landing at (8, 8).
    doc.commands().push(std::make_unique<GrowAndPaintLayerCommand>(id, 128, 128, 8, 8,
                                                                   std::move(region), 40, 40));

    const RasterLayer* grown = doc.find(id)->as<RasterLayer>();
    REQUIRE(grown != nullptr);
    const RasterMask* gm = grown->mask();
    REQUIRE(gm != nullptr);
    CHECK(gm->width == 128);
    CHECK(gm->height == 128);
    CHECK(gm->linked == false); // the flags ride along
    // The authored coverage moved with the image ...
    CHECK(gm->coverage[static_cast<std::size_t>(5 + 8) * 128 + (6 + 8)] == 200);
    CHECK(gm->coverage[static_cast<std::size_t>(4 + 8) * 128 + (6 + 8)] == 0);
    // ... and the new band REVEALS, so growing the layer changes not one composited pixel (the band
    // is transparent) while the paint that lands there is visible.
    CHECK(gm->coverage[0] == 255);

    doc.commands().undo();
    const RasterMask* backMask = doc.find(id)->as<RasterLayer>()->mask();
    REQUIRE(backMask != nullptr);
    CHECK(backMask->width == 32);
    CHECK(backMask->height == 32);
    CHECK(backMask->coverage[static_cast<std::size_t>(5) * 32 + 6] == 200);
}

TEST_CASE("a stroke crossing a small layer's edge grows it and keeps the old pixels byte-exact") {
    // The whole feature, end to end at the model level: paint across the right edge of a 64x64
    // layer on a 256x256 canvas, plan the growth the way the canvas does, and land it.
    Document doc(256, 256);
    auto layerOwned = doc.makeRaster("small", 64, 64);
    Image& src = layerOwned->image();
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 64; ++x)
            setPixel(src, x, y, Color8{40, 60, 80, 255});
    const Image before = src;
    const LayerId id = layerOwned->id();
    doc.root().addOnTop(std::move(layerOwned));

    RasterLayer* layer = doc.find(id)->as<RasterLayer>();
    REQUIRE(layer != nullptr);
    const PixelBox canvas = canvasBoxInLayer(worldTransform(*layer), doc.width(), doc.height());
    // The press-time working grid: the layer united with the canvas.
    const PixelBox work = brushGrowthBox(64, 64, canvas, canvas);
    REQUIRE(work == PixelBox{0, 0, 256, 256});
    Image working = common::copyRegion(before, work.x0, work.y0,
                                       static_cast<std::uint32_t>(work.width()),
                                       static_cast<std::uint32_t>(work.height()));

    // A stroke straddling the old right edge (x = 64).
    BrushParams p;
    p.diameter = 20.0;
    p.hardness = 1.0;
    p.flow = 1.0;
    p.opacity = 1.0;
    p.color = Color8{255, 0, 0, 255};
    BrushEngine eng;
    eng.begin(256, 256, working, p, BrushDynamics{}, StrokeInput{{54.0, 32.0}, 1.0});
    eng.extendTo(StrokeInput{{74.0, 32.0}, 1.0});
    eng.extendTo(StrokeInput{{94.0, 32.0}, 1.0});
    eng.flush();
    eng.composite();
    eng.end();
    const Rect db = eng.dirtyBounds();
    REQUIRE(!db.empty());
    CHECK(db.right() > 64.0); // the stroke really did leave the old grid

    const auto rw = static_cast<std::uint32_t>(db.w);
    const auto rh = static_cast<std::uint32_t>(db.h);
    Image painted = common::copyRegion(working, static_cast<long>(db.x), static_cast<long>(db.y),
                                       rw, rh);
    const PixelBox touched{static_cast<long>(db.x), static_cast<long>(db.y),
                           static_cast<long>(db.x) + rw, static_cast<long>(db.y) + rh};
    const PixelBox keep = brushGrowthBox(64, 64, touched, canvas);
    // Tight: only as far as the stroke actually reached, never the whole working grid.
    CHECK(keep.x1 == touched.x1);
    CHECK(keep.x1 < 256);
    CHECK(keep == PixelBox{0, 0, touched.x1, 64});

    doc.commands().push(std::make_unique<GrowAndPaintLayerCommand>(
        id, static_cast<std::uint32_t>(keep.width()), static_cast<std::uint32_t>(keep.height()),
        -keep.x0, -keep.y0, std::move(painted), static_cast<long>(db.x) - keep.x0,
        static_cast<long>(db.y) - keep.y0));

    const RasterLayer* grown = doc.find(id)->as<RasterLayer>();
    REQUIRE(grown != nullptr);
    CHECK(grown->image().width == static_cast<std::uint32_t>(keep.width()));
    CHECK(grown->image().height == 64);
    // Paint landed beyond the OLD edge -- the thing that used to be silently discarded.
    CHECK(pixel(grown->image(), 70, 32).r > 100);
    // Every pixel outside the stroke's box is the layer's original byte for byte.
    for (std::uint32_t y = 0; y < 64; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
            const bool inStroke = static_cast<double>(x) >= db.x &&
                                  static_cast<double>(x) < db.right() &&
                                  static_cast<double>(y) >= db.y &&
                                  static_cast<double>(y) < db.bottom();
            if (!inStroke)
                CHECK(pixel(grown->image(), x, y) == pixel(before, x, y));
        }
    }

    doc.commands().undo();
    CHECK(doc.find(id)->as<RasterLayer>()->image().width == 64);
    CHECK(doc.find(id)->as<RasterLayer>()->image().rgba == before.rgba);
}
