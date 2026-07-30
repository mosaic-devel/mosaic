#include "core/arrange.hpp"
#include "core/arrange_target.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"

using namespace mosaic;
using mosaic::common::Affine2D;
using mosaic::common::Rect;
using mosaic::common::Vec2;
using core::AlignEdge;
using core::DistributeAxis;

// ---- alignTranslations (union reference: the align-to-selection convention) ---------------------

TEST_CASE("align to union: the extreme box stays put, the rest move to meet it") {
    const std::vector<Rect> boxes{{10, 10, 20, 20}, {50, 40, 10, 10}};

    SUBCASE("Left") {
        const auto d = core::alignTranslations(boxes, AlignEdge::Left);
        CHECK(d[0] == Vec2{0.0, 0.0});   // already on the union's left edge
        CHECK(d[1] == Vec2{-40.0, 0.0}); // 50 -> 10
    }
    SUBCASE("Right") {
        const auto d = core::alignTranslations(boxes, AlignEdge::Right);
        CHECK(d[0] == Vec2{30.0, 0.0}); // right 30 -> 60
        CHECK(d[1] == Vec2{0.0, 0.0});
    }
    SUBCASE("HCenter") {
        const auto d = core::alignTranslations(boxes, AlignEdge::HCenter);
        CHECK(d[0] == Vec2{15.0, 0.0});  // centre 20 -> the union's centre 35
        CHECK(d[1] == Vec2{-20.0, 0.0}); // centre 55 -> 35
    }
    SUBCASE("Top") {
        const auto d = core::alignTranslations(boxes, AlignEdge::Top);
        CHECK(d[0] == Vec2{0.0, 0.0});
        CHECK(d[1] == Vec2{0.0, -30.0}); // 40 -> 10
    }
    SUBCASE("Bottom") {
        const auto d = core::alignTranslations(boxes, AlignEdge::Bottom);
        CHECK(d[0] == Vec2{0.0, 20.0}); // bottom 30 -> 50
        CHECK(d[1] == Vec2{0.0, 0.0});
    }
    SUBCASE("VMiddle") {
        const auto d = core::alignTranslations(boxes, AlignEdge::VMiddle);
        CHECK(d[0] == Vec2{0.0, 10.0});  // centre 20 -> the union's centre 30
        CHECK(d[1] == Vec2{0.0, -15.0}); // centre 45 -> 30
    }
}

TEST_CASE("align to union: a single box is its own union -- every edge is a no-op") {
    // This is WHY the app routes a single-layer selection through the reference overload with the
    // canvas rect instead: against its own union, nothing would ever move.
    const std::vector<Rect> boxes{{10, 20, 30, 40}};
    for (const AlignEdge e : {AlignEdge::Left, AlignEdge::HCenter, AlignEdge::Right, AlignEdge::Top,
                              AlignEdge::VMiddle, AlignEdge::Bottom}) {
        const auto d = core::alignTranslations(boxes, e);
        REQUIRE(d.size() == 1);
        CHECK(d[0] == Vec2{0.0, 0.0});
    }
}

TEST_CASE("align: empty input -> empty output (both overloads)") {
    CHECK(core::alignTranslations({}, AlignEdge::Left).empty());
    CHECK(core::alignTranslations({}, AlignEdge::Left, Rect{0, 0, 100, 100}).empty());
}

// ---- alignTranslations (explicit reference: the single-layer align-to-canvas path) --------------

TEST_CASE("align to a reference rect: each box lines up to the fixed rect's edge/centre") {
    const Rect canvas{0, 0, 200, 100};
    const std::vector<Rect> boxes{{10, 20, 30, 40}}; // right 40, bottom 60, centre (25, 40)

    SUBCASE("Left") {
        CHECK(core::alignTranslations(boxes, AlignEdge::Left, canvas)[0] == Vec2{-10.0, 0.0});
    }
    SUBCASE("HCenter") {
        CHECK(core::alignTranslations(boxes, AlignEdge::HCenter, canvas)[0] == Vec2{75.0, 0.0});
    }
    SUBCASE("Right") {
        CHECK(core::alignTranslations(boxes, AlignEdge::Right, canvas)[0] == Vec2{160.0, 0.0});
    }
    SUBCASE("Top") {
        CHECK(core::alignTranslations(boxes, AlignEdge::Top, canvas)[0] == Vec2{0.0, -20.0});
    }
    SUBCASE("VMiddle") {
        CHECK(core::alignTranslations(boxes, AlignEdge::VMiddle, canvas)[0] == Vec2{0.0, 10.0});
    }
    SUBCASE("Bottom") {
        CHECK(core::alignTranslations(boxes, AlignEdge::Bottom, canvas)[0] == Vec2{0.0, 40.0});
    }
}

TEST_CASE("align to a reference rect: a box larger than the canvas overhangs symmetrically") {
    const Rect canvas{0, 0, 100, 100};
    const std::vector<Rect> boxes{{-30, 0, 200, 100}}; // wider than the canvas
    // Centre 70 -> 50: the overhang splits evenly left/right instead of snapping an edge.
    CHECK(core::alignTranslations(boxes, AlignEdge::HCenter, canvas)[0] == Vec2{-20.0, 0.0});
}

TEST_CASE("align to a reference rect: several boxes all move to the SAME edge (no union)") {
    const Rect canvas{0, 0, 100, 100};
    const std::vector<Rect> boxes{{10, 0, 10, 10}, {50, 0, 20, 10}};
    const auto d = core::alignTranslations(boxes, AlignEdge::Right, canvas);
    CHECK(d[0] == Vec2{80.0, 0.0}); // right 20 -> 100
    CHECK(d[1] == Vec2{30.0, 0.0}); // right 70 -> 100
}

// ---- distributeTranslations ---------------------------------------------------------------------

TEST_CASE("distribute: equal gaps between adjacent boxes, extremes fixed") {
    // Total span 0..100, sizes 10+20+10 = 40 -> two gaps of 30 each.
    const std::vector<Rect> boxes{{0, 0, 10, 10}, {15, 0, 20, 10}, {90, 0, 10, 10}};
    const auto d = core::distributeTranslations(boxes, DistributeAxis::Horizontal);
    CHECK(d[0] == Vec2{0.0, 0.0});  // extreme: stays
    CHECK(d[1] == Vec2{25.0, 0.0}); // 15 -> 40 (10 + gap 30)
    CHECK(d[2] == Vec2{0.0, 0.0});  // extreme: stays
}

TEST_CASE("distribute: order is by position, not input order") {
    const std::vector<Rect> boxes{{40, 0, 10, 10}, {0, 0, 10, 10}, {90, 0, 10, 10}};
    const auto d = core::distributeTranslations(boxes, DistributeAxis::Horizontal);
    // Span 0..100, sizes 30 -> gaps of 35; the middle box (input index 0) lands at 45.
    CHECK(d[0] == Vec2{5.0, 0.0});
    CHECK(d[1] == Vec2{0.0, 0.0});
    CHECK(d[2] == Vec2{0.0, 0.0});
}

TEST_CASE("distribute: fewer than three boxes -> all zero") {
    const std::vector<Rect> two{{0, 0, 10, 10}, {50, 0, 10, 10}};
    const auto d = core::distributeTranslations(two, DistributeAxis::Vertical);
    REQUIRE(d.size() == 2);
    CHECK(d[0] == Vec2{0.0, 0.0});
    CHECK(d[1] == Vec2{0.0, 0.0});
}

// =================================================================================================
// core/arrange_target.hpp -- WHICH layers Arrange acts on, and WHAT box it lines up for each.
//
// The two families the boxes above could never describe: a masked adjustment/filter layer (no
// content at all -- the mask is its whole position) and a canvas-filling generator/gradient (content
// the size of the canvas, so every align is a no-op unless the mask is taken into account).
// =================================================================================================

namespace {

// A `w` x `h` mask sheet, all cells hidden except the half-open CELL rect `reveal`.
core::RasterMask sheet(std::uint32_t w, std::uint32_t h, Rect reveal,
                       Affine2D toLocal = Affine2D::identity(), bool linked = true) {
    core::RasterMask m(w, h, 0);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            if (reveal.contains({static_cast<double>(x), static_cast<double>(y)}))
                m.coverage[static_cast<std::size_t>(y) * w + x] = 255;
        }
    }
    m.toLocal = toLocal;
    m.linked = linked;
    return m;
}

// An opaque `w` x `h` raster at `place` -- contentBounds() is the whole image.
core::RasterLayer* addRaster(core::Document& doc, std::uint32_t w, std::uint32_t h,
                             const Affine2D& place, core::GroupLayer* parent = nullptr) {
    auto r = doc.makeRaster("r", w, h);
    r->image().fill(common::Color8{255, 255, 255, 255});
    r->setTransform(place);
    auto* raw = r.get();
    (parent != nullptr ? *parent : doc.root()).addOnTop(std::move(r));
    return raw;
}

// An adjustment layer: the kind with NO contentBounds() override at all, which is exactly why
// Arrange used to skip it.
core::AdjustmentLayer* addAdjustment(core::Document& doc,
                                     const Affine2D& place = Affine2D::identity()) {
    auto a = doc.makeAdjustment("levels", core::AdjustmentKind::Levels);
    a->setTransform(place);
    auto* raw = a.get();
    doc.root().addOnTop(std::move(a));
    return raw;
}

} // namespace

// ---- arrangeBounds: the effective visible box ----------------------------------------------------

TEST_CASE("effective box: an UNMASKED layer is byte-identical to what Arrange always computed") {
    // The load-bearing regression: widening the menu must not move one existing layer by one ULP.
    // A rotated, non-uniformly scaled placement so any reordering of the maths would show.
    core::Document doc(64, 64);
    core::RasterLayer* l =
        addRaster(doc, 10, 8, Affine2D::trs({12, 7}, std::numbers::pi / 5.0, {1.5, 0.75}));

    const std::optional<Rect> cb = l->contentBounds();
    REQUIRE(cb.has_value());
    const Rect today = core::worldTransform(*l).mapBounds(*cb); // applyArrange's own expression
    const std::optional<Rect> box = core::arrangeBounds(*l);
    REQUIRE(box.has_value());
    CHECK(*box == today); // exact, not approximate: anything else IS a behaviour change
}

TEST_CASE("effective box: an adjustment layer has no content, so its MASK is its position") {
    core::Document doc(64, 64);
    core::AdjustmentLayer* adj = addAdjustment(doc);
    REQUIRE_FALSE(adj->contentBounds().has_value());
    CHECK_FALSE(core::arrangeBounds(*adj).has_value()); // no content and no mask: nothing to align

    // A document-window sheet (the grid contract's rule for adjustment kinds: toLocal is the
    // inverse of the world transform, which at the identity is the identity) revealing doc
    // [20,28) x [30,34).
    adj->setMask(sheet(64, 64, Rect{20, 30, 8, 4}));
    const std::optional<Rect> box = core::arrangeBounds(*adj);
    REQUIRE(box.has_value());
    CHECK(*box == Rect{20, 30, 8, 4});
}

TEST_CASE("effective box: content AND mask -> their intersection, which is what is visible") {
    // The masked-generator case in miniature: content covering far more than the mask reveals.
    core::Document doc(64, 64);
    core::RasterLayer* l = addRaster(doc, 10, 8, Affine2D::identity());
    l->setMask(sheet(10, 8, Rect{5, 2, 5, 4})); // a raster sheet IS the image grid (toLocal = id)

    const std::optional<Rect> box = core::arrangeBounds(*l);
    REQUIRE(box.has_value());
    CHECK(*box == Rect{5, 2, 5, 4}); // not {0,0,10,8}: the rest of the layer shows nothing
}

TEST_CASE("effective box: a mask that misses the content entirely -> nullopt") {
    core::Document doc(64, 64);
    core::RasterLayer* l = addRaster(doc, 10, 8, Affine2D::translation(40, 40));
    // UNLINKED: the sheet is pinned in the parent's (here document) space at the origin, while the
    // pixels sit at (40, 40). Nothing of this layer is visible, so there is no box to line up.
    l->setMask(sheet(10, 8, Rect{0, 0, 10, 8}, Affine2D::identity(), /*linked=*/false));
    CHECK_FALSE(core::arrangeBounds(*l).has_value());
}

TEST_CASE("effective box: a DISABLED mask is ignored -- the compositor ignores it too") {
    core::Document doc(64, 64);
    core::RasterLayer* l = addRaster(doc, 10, 8, Affine2D::identity());
    core::RasterMask m = sheet(10, 8, Rect{5, 2, 5, 4});
    m.enabled = false;
    l->setMask(std::move(m));

    const std::optional<Rect> box = core::arrangeBounds(*l);
    REQUIRE(box.has_value());
    CHECK(*box == Rect{0, 0, 10, 8}); // back to the plain content box
}

TEST_CASE("effective box: an EMPTY sheet is ignored, an all-ZERO sheet hides the layer") {
    // Two opposite degenerate masks that must not be confused. A 0 x 0 sheet was never given cells
    // and the compositor maps it to "no mask"; a sheet full of zeros is a mask that deliberately
    // hides everything, and a fully hidden layer has no visible box to align.
    core::Document doc(64, 64);
    core::RasterLayer* empty = addRaster(doc, 10, 8, Affine2D::identity());
    empty->setMask(core::RasterMask{}); // 0 x 0, enabled
    const std::optional<Rect> emptyBox = core::arrangeBounds(*empty);
    REQUIRE(emptyBox.has_value());
    CHECK(*emptyBox == Rect{0, 0, 10, 8});

    core::RasterLayer* hidden = addRaster(doc, 10, 8, Affine2D::identity());
    hidden->setMask(sheet(10, 8, Rect{})); // every cell 0
    CHECK_FALSE(core::arrangeBounds(*hidden).has_value());

    core::AdjustmentLayer* adj = addAdjustment(doc);
    adj->setMask(sheet(64, 64, Rect{})); // ... and with no content either
    CHECK_FALSE(core::arrangeBounds(*adj).has_value());
}

// ---- maskCoverageBounds + maskToDocument ---------------------------------------------------------

TEST_CASE("mask coverage bbox is tight to coverage > 0, and a cell spans [x, x+1)") {
    core::RasterMask m(8, 6, 0);
    m.coverage[static_cast<std::size_t>(1) * 8 + 2] = 1;   // (2,1): barely non-zero still counts
    m.coverage[static_cast<std::size_t>(4) * 8 + 5] = 255; // (5,4)
    const std::optional<Rect> cells = core::maskCoverageBounds(m);
    REQUIRE(cells.has_value());
    CHECK(*cells == Rect{2, 1, 4, 4}); // x 2..5 inclusive -> [2, 6); y 1..4 -> [1, 5)

    CHECK_FALSE(core::maskCoverageBounds(core::RasterMask(8, 6, 0)).has_value()); // all zero
    CHECK_FALSE(core::maskCoverageBounds(core::RasterMask{}).has_value());        // no cells
}

TEST_CASE("mask box: a non-identity sheet placement (toLocal) is honoured") {
    core::Document doc(64, 64);
    // 2x-scaled sheet offset by (5,3): cell (1,1)..(2,2) covers doc [7,11) x [5,9).
    core::AdjustmentLayer* adj = addAdjustment(doc);
    adj->setMask(sheet(8, 6, Rect{1, 1, 2, 2}, Affine2D::translation(5, 3) * Affine2D::scaling(2, 2)));

    const std::optional<Rect> box = core::arrangeBounds(*adj);
    REQUIRE(box.has_value());
    CHECK(*box == Rect{7, 5, 4, 4});
}

TEST_CASE("mask box: a scaled + translated LAYER transform carries a linked sheet with it") {
    core::Document doc(64, 64);
    core::AdjustmentLayer* adj =
        addAdjustment(doc, Affine2D::translation(10, 20) * Affine2D::scaling(3, 2));
    adj->setMask(sheet(8, 6, Rect{2, 1, 2, 2})); // cells [2,4) x [1,3), toLocal identity, linked

    const std::optional<Rect> box = core::arrangeBounds(*adj);
    REQUIRE(box.has_value());
    CHECK(*box == Rect{16, 22, 6, 4}); // (2,1)*(3,2) + (10,20) .. (4,3)*(3,2) + (10,20)
}

TEST_CASE("mask box: a ROTATED layer transform maps the sheet through maskToDocument") {
    core::Document doc(64, 64);
    // A quarter turn: local (x, y) -> (-y, x), then + (30, 10).
    core::AdjustmentLayer* adj =
        addAdjustment(doc, Affine2D::trs({30, 10}, std::numbers::pi / 2.0, {1, 1}));
    adj->setMask(sheet(8, 6, Rect{2, 1, 2, 2}));

    const std::optional<Rect> box = core::arrangeBounds(*adj);
    REQUIRE(box.has_value());
    CHECK(box->x == doctest::Approx(27.0));
    CHECK(box->y == doctest::Approx(12.0));
    CHECK(box->w == doctest::Approx(2.0));
    CHECK(box->h == doctest::Approx(2.0));
}

// ---- translatedMaskPlacement: the unlinked-mask half of the move ---------------------------------

TEST_CASE("translatedMaskPlacement declines everything that already rides the transform") {
    core::Document doc(64, 64);
    core::RasterLayer* bare = addRaster(doc, 10, 8, Affine2D::identity());
    CHECK_FALSE(core::translatedMaskPlacement(*bare, {5, 5}).has_value()); // no mask

    core::RasterLayer* linked = addRaster(doc, 10, 8, Affine2D::identity());
    linked->setMask(sheet(10, 8, Rect{0, 0, 4, 4}));
    // A LINKED mask rides the layer transform, so the align needs no second edit at all.
    CHECK_FALSE(core::translatedMaskPlacement(*linked, {5, 5}).has_value());

    core::RasterLayer* off = addRaster(doc, 10, 8, Affine2D::identity());
    core::RasterMask m = sheet(10, 8, Rect{0, 0, 4, 4}, Affine2D::identity(), /*linked=*/false);
    m.enabled = false;
    off->setMask(std::move(m));
    CHECK_FALSE(core::translatedMaskPlacement(*off, {5, 5}).has_value()); // disabled: inert
}

TEST_CASE("translatedMaskPlacement moves the visible box by the DOC delta under a scaled parent") {
    // The one case that tells a point-map from a vector-map. Inside a group scaled 2x and offset,
    // mapping the delta as a POINT leaks the group's translation into the answer; as a VECTOR it
    // does not. Under an identity parent both are identical, which is why this test has a parent.
    core::Document doc(64, 64);
    auto group = doc.makeGroup("g");
    group->setTransform(Affine2D::translation(7, 3) * Affine2D::scaling(2, 2));
    core::GroupLayer* g = group.get();
    doc.root().addOnTop(std::move(group));

    auto adjOwned = doc.makeAdjustment("levels", core::AdjustmentKind::Levels);
    core::AdjustmentLayer* adj = adjOwned.get();
    g->addOnTop(std::move(adjOwned));
    adj->setMask(sheet(16, 16, Rect{2, 3, 4, 2}, Affine2D::identity(), /*linked=*/false));

    const std::optional<Rect> before = core::arrangeBounds(*adj);
    REQUIRE(before.has_value());
    CHECK(*before == Rect{11, 9, 8, 4}); // (2,3)*2 + (7,3) .. (6,5)*2 + (7,3)

    const Vec2 delta{12.0, -5.0};
    const std::optional<Affine2D> next = core::translatedMaskPlacement(*adj, delta);
    REQUIRE(next.has_value());
    adj->mask()->toLocal = *next;

    const std::optional<Rect> after = core::arrangeBounds(*adj);
    REQUIRE(after.has_value());
    CHECK(after->x == doctest::Approx(before->x + delta.x));
    CHECK(after->y == doctest::Approx(before->y + delta.y));
    CHECK(after->w == doctest::Approx(before->w));
    CHECK(after->h == doctest::Approx(before->h));
}

// ---- The whole edit, as the Arrange menu lands it -------------------------------------------------

// applyArrange's recipe, verbatim: the doc-space translation becomes a parent-relative transform
// (invParent * delta * world) plus -- for an UNLINKED mask -- the matching sheet slide, both inside
// one CompositeCommand so they undo together.
namespace {

void pushArrangeMove(core::Document& doc, core::Layer& layer, Vec2 delta) {
    const std::optional<Affine2D> invParent = core::parentWorldTransform(layer).inverse();
    REQUIRE(invParent.has_value());
    auto composite = std::make_unique<core::CompositeCommand>("Transform Layers");
    composite->add(std::make_unique<core::SetTransformsCommand>(
        std::vector<core::SetTransformsCommand::Entry>{
            {layer.id(),
             *invParent * Affine2D::translation(delta.x, delta.y) * core::worldTransform(layer)}},
        /*coalesceId=*/0));
    if (const std::optional<Affine2D> placement = core::translatedMaskPlacement(layer, delta)) {
        composite->add(std::make_unique<core::SetMaskPlacementCommand>(
            std::vector<core::SetMaskPlacementCommand::Entry>{{layer.id(), *placement}}));
    }
    doc.commands().push(std::move(composite));
}

} // namespace

TEST_CASE("align: a mask-only adjustment layer moves by its transform, the linked sheet riding it") {
    core::Document doc(100, 100);
    core::AdjustmentLayer* adj = addAdjustment(doc);
    adj->setMask(sheet(100, 100, Rect{10, 40, 10, 4}));

    const std::optional<Rect> before = core::arrangeBounds(*adj);
    REQUIRE(before.has_value());
    REQUIRE(*before == Rect{10, 40, 10, 4});

    // A single target aligns to the CANVAS -- the app's own rule for a lone box (see the
    // align-to-reference cases above), which is the path this layer could never reach before.
    const Rect canvas{0, 0, 100, 100};
    const std::vector<Vec2> d =
        core::alignTranslations({*before}, AlignEdge::HCenter, canvas);
    REQUIRE(d.size() == 1);
    CHECK(d[0] == Vec2{35.0, 0.0}); // centre 15 -> 50

    pushArrangeMove(doc, *adj, d[0]);
    const std::optional<Rect> after = core::arrangeBounds(*adj);
    REQUIRE(after.has_value());
    CHECK(*after == Rect{45, 40, 10, 4});

    doc.commands().undo();
    CHECK(*core::arrangeBounds(*adj) == *before); // exact round trip: the old value is restored
}

TEST_CASE("align: an UNLINKED mask is slid with the layer, so the visible blob really moves") {
    // Without the SetMaskPlacementCommand half, the content slides out from under a stationary
    // sheet and the intersection -- what you see -- moves by LESS than asked, or not at all.
    core::Document doc(64, 64);
    auto group = doc.makeGroup("g");
    group->setTransform(Affine2D::translation(7, 3) * Affine2D::scaling(2, 2));
    core::GroupLayer* g = group.get();
    doc.root().addOnTop(std::move(group));

    core::RasterLayer* l = addRaster(doc, 10, 8, Affine2D::identity(), g);
    l->setMask(sheet(16, 16, Rect{3, 0, 7, 8}, Affine2D::identity(), /*linked=*/false));

    const std::optional<Rect> before = core::arrangeBounds(*l);
    REQUIRE(before.has_value());
    CHECK(*before == Rect{13, 3, 14, 16}); // content [7,27)x[3,19) ∩ sheet [13,27)x[3,19)

    const Vec2 delta{5.0, -2.0};
    pushArrangeMove(doc, *l, delta);
    const std::optional<Rect> after = core::arrangeBounds(*l);
    REQUIRE(after.has_value());
    CHECK(after->x == doctest::Approx(before->x + delta.x));
    CHECK(after->y == doctest::Approx(before->y + delta.y));
    CHECK(after->w == doctest::Approx(before->w)); // the box TRANSLATES; it must not deform
    CHECK(after->h == doctest::Approx(before->h));

    doc.commands().undo(); // one step undoes BOTH halves
    CHECK(*core::arrangeBounds(*l) == *before);
    doc.commands().redo();
    CHECK(core::arrangeBounds(*l)->x == doctest::Approx(before->x + delta.x));
}

// ---- arrangeTargets: which layers survive the ladder ---------------------------------------------

TEST_CASE("arrangeTargets drops the invalid, the vanished, the hidden and the locked") {
    core::Document doc(64, 64);
    core::RasterLayer* a = addRaster(doc, 4, 4, Affine2D::identity());
    core::RasterLayer* hidden = addRaster(doc, 4, 4, Affine2D::identity());
    core::RasterLayer* locked = addRaster(doc, 4, 4, Affine2D::identity());
    core::RasterLayer* b = addRaster(doc, 4, 4, Affine2D::identity());
    hidden->setVisible(false);
    locked->setLocked(true); // a lock refuses transform edits everywhere else; now here too

    const auto resolve = [&doc](core::LayerId id) -> const core::Layer* { return doc.find(id); };
    const std::vector<core::LayerId> got = core::arrangeTargets(
        {b->id(), core::kInvalidLayerId, hidden->id(), a->id(), locked->id(), 999999, b->id()},
        resolve);
    // Order is the candidates' own (b before a), duplicates collapse to the FIRST occurrence.
    REQUIRE(got.size() == 2);
    CHECK(got[0] == b->id());
    CHECK(got[1] == a->id());
}

TEST_CASE("arrangeTargets: an empty candidate list stays empty (the ladder falls through)") {
    core::Document doc(64, 64);
    const auto resolve = [&doc](core::LayerId id) -> const core::Layer* { return doc.find(id); };
    CHECK(core::arrangeTargets({}, resolve).empty());
}

TEST_CASE("distribute still needs three real boxes, mask-defined ones included") {
    // The >= 3 rule is the menu's, but it counts the boxes THIS header produces -- so two masked
    // adjustment layers must still distribute to nothing.
    core::Document doc(100, 100);
    core::AdjustmentLayer* one = addAdjustment(doc);
    one->setMask(sheet(100, 100, Rect{5, 5, 10, 10}));
    core::AdjustmentLayer* two = addAdjustment(doc);
    two->setMask(sheet(100, 100, Rect{60, 5, 10, 10}));

    std::vector<Rect> boxes;
    for (const core::Layer* l : {static_cast<const core::Layer*>(one),
                                 static_cast<const core::Layer*>(two)}) {
        if (const std::optional<Rect> box = core::arrangeBounds(*l))
            boxes.push_back(*box);
    }
    REQUIRE(boxes.size() == 2); // both DO have a box now -- they simply cannot be distributed
    const auto d = core::distributeTranslations(boxes, DistributeAxis::Horizontal);
    CHECK(d[0] == Vec2{0.0, 0.0});
    CHECK(d[1] == Vec2{0.0, 0.0});
}
