#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/layer_marquee.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// S15-f: the Move tool's empty-space drag marquee. Everything the band actually decides -- the
// rect-vs-rotated-box overlap test and which top-level units a document-space rect gathers -- lives
// in core::layer_marquee, FLTK-free, so it is pinned here headlessly; the canvas keeps only the
// pointer plumbing and the overlay lane (exercised by the --gui-frames smoke run instead).
namespace {

using mosaic::common::Affine2D;
using mosaic::common::Rect;
using mosaic::common::Vec2;
using mosaic::core::Document;
using mosaic::core::layerContentQuad;
using mosaic::core::layersInMarquee;
using mosaic::core::LayerId;
using mosaic::core::rectIntersectsQuad;

// An axis-aligned quad in TL, TR, BR, BL order (what layerContentQuad emits for an untransformed
// layer), so the SAT cases below read as plain rectangles.
std::array<Vec2, 4> boxQuad(double x, double y, double w, double h) {
    return {Vec2{x, y}, Vec2{x + w, y}, Vec2{x + w, y + h}, Vec2{x, y + h}};
}

// A `size` x `size` raster at document origin `at`, fully opaque, added on top of the stack.
LayerId addOpaqueRaster(Document& doc, const char* name, Vec2 at, std::uint32_t size) {
    auto layer = doc.makeRaster(name, size, size);
    for (std::size_t p = 3; p < layer->image().rgba.size(); p += 4)
        layer->image().rgba[p] = 255;
    layer->setTransform(Affine2D::translation(at.x, at.y));
    const LayerId id = layer->id();
    doc.root().addOnTop(std::move(layer));
    return id;
}

bool contains(const std::vector<LayerId>& ids, LayerId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

} // namespace

TEST_CASE("rectIntersectsQuad: overlap, containment, and contact-is-a-miss") {
    const Rect band{10.0, 10.0, 20.0, 20.0};

    CHECK(rectIntersectsQuad(band, boxQuad(20.0, 20.0, 30.0, 30.0)));  // partial overlap
    CHECK(rectIntersectsQuad(band, boxQuad(12.0, 12.0, 4.0, 4.0)));    // quad inside the band
    CHECK(rectIntersectsQuad(band, boxQuad(0.0, 0.0, 100.0, 100.0)));  // band inside the quad
    CHECK_FALSE(rectIntersectsQuad(band, boxQuad(40.0, 40.0, 5.0, 5.0)));  // disjoint
    // Touching edges are not "touched": a band dragged up to a layer's edge must not gather it,
    // and the same rule makes a zero-area band gather nothing at all.
    CHECK_FALSE(rectIntersectsQuad(band, boxQuad(30.0, 10.0, 5.0, 5.0)));
    CHECK_FALSE(rectIntersectsQuad(Rect{10.0, 10.0, 0.0, 0.0}, boxQuad(0.0, 0.0, 100.0, 100.0)));
    CHECK_FALSE(rectIntersectsQuad(Rect{}, boxQuad(0.0, 0.0, 100.0, 100.0)));
}

TEST_CASE("rectIntersectsQuad: a rotated box whose AABB overlaps but whose body does not") {
    // A diamond centred at (50,50) with half-diagonal 20: its AABB is (30,30)-(70,70), but the
    // body keeps clear of the AABB's corners. A band tucked into one corner overlaps the AABB and
    // must still be a MISS -- the whole reason the test is SAT and not box-vs-box.
    const std::array<Vec2, 4> diamond{Vec2{50.0, 30.0}, Vec2{70.0, 50.0}, Vec2{50.0, 70.0},
                                      Vec2{30.0, 50.0}};
    CHECK_FALSE(rectIntersectsQuad(Rect{30.0, 30.0, 5.0, 5.0}, diamond)); // AABB corner only
    CHECK(rectIntersectsQuad(Rect{45.0, 45.0, 10.0, 10.0}, diamond));     // through the middle
    CHECK(rectIntersectsQuad(Rect{30.0, 30.0, 12.0, 12.0}, diamond)); // far enough in to cut it
}

TEST_CASE("layerContentQuad: the content box through the layer's world transform") {
    Document doc(64, 64);
    const LayerId id = addOpaqueRaster(doc, "a", {8.0, 4.0}, 10);

    const auto quad = layerContentQuad(*doc.find(id));
    REQUIRE(quad.has_value());
    CHECK((*quad)[0] == Vec2{8.0, 4.0});   // TL
    CHECK((*quad)[2] == Vec2{18.0, 14.0}); // BR

    SUBCASE("a fully transparent raster has no content extent") {
        auto empty = doc.makeRaster("empty", 8, 8); // alpha 0 everywhere
        CHECK_FALSE(layerContentQuad(*empty).has_value());
    }
    SUBCASE("an adjustment layer has no content extent (a click-pick never lands on one either)") {
        auto adj = doc.makeAdjustment("adj", mosaic::core::AdjustmentKind::Levels);
        CHECK_FALSE(layerContentQuad(*adj).has_value());
    }
    SUBCASE("a nested layer maps through its ancestor group's transform") {
        auto group = doc.makeGroup("g");
        auto child = doc.makeRaster("child", 4, 4);
        for (std::size_t p = 3; p < child->image().rgba.size(); p += 4)
            child->image().rgba[p] = 255;
        child->setTransform(Affine2D::translation(2.0, 2.0));
        group->setTransform(Affine2D::translation(20.0, 20.0));
        group->addOnTop(std::move(child));
        // The GROUP maps as one unit: its content box is the union of its children in group space.
        const auto gq = layerContentQuad(*group);
        REQUIRE(gq.has_value());
        CHECK((*gq)[0] == Vec2{22.0, 22.0});
        CHECK((*gq)[2] == Vec2{26.0, 26.0});
    }
}

TEST_CASE("layersInMarquee: gathers the top-level units the band touches") {
    Document doc(128, 128);
    const LayerId a = addOpaqueRaster(doc, "a", {0.0, 0.0}, 10);    // (0,0)-(10,10)
    const LayerId b = addOpaqueRaster(doc, "b", {50.0, 50.0}, 10);  // (50,50)-(60,60)
    const LayerId c = addOpaqueRaster(doc, "c", {100.0, 100.0}, 10); // (100,100)-(110,110)

    SUBCASE("a band over two of the three takes exactly those two, bottom-first") {
        const std::vector<LayerId> hit = layersInMarquee(doc.root(), Rect{5.0, 5.0, 50.0, 50.0});
        REQUIRE(hit.size() == 2);
        CHECK(hit[0] == a); // stack order: bottom child first
        CHECK(hit[1] == b);
        CHECK_FALSE(contains(hit, c));
    }
    SUBCASE("a band that touches nothing gathers nothing") {
        CHECK(layersInMarquee(doc.root(), Rect{20.0, 20.0, 10.0, 10.0}).empty());
    }
    SUBCASE("a degenerate band (a click) gathers nothing") {
        CHECK(layersInMarquee(doc.root(), Rect{0.0, 0.0, 0.0, 0.0}).empty());
    }
    SUBCASE("invisible units are skipped, exactly as the click-pick skips them") {
        doc.find(b)->setVisible(false);
        const std::vector<LayerId> hit = layersInMarquee(doc.root(), Rect{5.0, 5.0, 50.0, 50.0});
        REQUIRE(hit.size() == 1);
        CHECK(hit[0] == a);
    }
    SUBCASE("locked units ARE gathered (a lock refuses transforms, not selection)") {
        doc.find(b)->setLocked(true);
        CHECK(contains(layersInMarquee(doc.root(), Rect{45.0, 45.0, 20.0, 20.0}), b));
    }
    SUBCASE("content outside the canvas is still swept") {
        doc.find(c)->setTransform(Affine2D::translation(-40.0, -40.0)); // fully off-canvas
        CHECK(contains(layersInMarquee(doc.root(), Rect{-45.0, -45.0, 10.0, 10.0}), c));
    }
}

TEST_CASE("layersInMarquee: a group is one unit, and rotation is honoured") {
    Document doc(128, 128);
    auto group = doc.makeGroup("g");
    auto inner = doc.makeRaster("inner", 8, 8);
    for (std::size_t p = 3; p < inner->image().rgba.size(); p += 4)
        inner->image().rgba[p] = 255;
    inner->setTransform(Affine2D::translation(40.0, 40.0));
    const LayerId innerId = inner->id();
    group->addOnTop(std::move(inner));
    const LayerId groupId = group->id();
    doc.root().addOnTop(std::move(group));

    // Grouped content is one object (the moveClickTarget model): the band gathers the GROUP.
    const std::vector<LayerId> hit = layersInMarquee(doc.root(), Rect{35.0, 35.0, 20.0, 20.0});
    REQUIRE(hit.size() == 1);
    CHECK(hit[0] == groupId);
    CHECK_FALSE(contains(hit, innerId));

    SUBCASE("rotating the group turns its box, and the band follows the turned body") {
        // 45 degrees about the document origin sends the (40,40)-(48,48) box off to roughly
        // (0, 56)-(0, 68) — nowhere near where it was, so the OLD place stops matching.
        constexpr double kQuarterPi = 0.78539816339744830961;
        doc.find(groupId)->setTransform(Affine2D::rotation(kQuarterPi));
        CHECK(layersInMarquee(doc.root(), Rect{35.0, 35.0, 20.0, 20.0}).empty());
        const auto quad = layerContentQuad(*doc.find(groupId));
        REQUIRE(quad.has_value());
        // Band a small rect straight onto the rotated box's own centre: it must match again.
        const Vec2 mid = ((*quad)[0] + (*quad)[2]) * 0.5;
        CHECK(contains(layersInMarquee(doc.root(), Rect{mid.x - 1.0, mid.y - 1.0, 2.0, 2.0}),
                       groupId));
    }
}
