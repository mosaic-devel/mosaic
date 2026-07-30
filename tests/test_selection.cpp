#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/selection.hpp"

#include <doctest/doctest.h>

#include <memory>

// The document-level selection mask + boolean ops + the undoable command (PLAN S13 part 1),
// and pixels-as-selection from a layer's alpha (part 2: the Shift-click-thumbnail gesture).
namespace {

using mosaic::common::Affine2D;
using mosaic::core::Document;
using mosaic::core::Selection;
using mosaic::core::selectionFromLayerPixels;
using mosaic::core::SelectOp;

// Set one RGBA pixel's alpha in a layer image (colour channels opaque grey for realism).
void setAlpha(mosaic::common::Image& img, std::uint32_t x, std::uint32_t y, std::uint8_t a) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    img.rgba[p] = img.rgba[p + 1] = img.rgba[p + 2] = 128;
    img.rgba[p + 3] = a;
}

} // namespace

TEST_CASE("empty vs all-zero: no-selection is not the same as selecting nothing") {
    const Selection none;
    CHECK(none.isEmpty());
    CHECK_FALSE(none.anySelected());

    const Selection zero(8, 8);
    CHECK_FALSE(zero.isEmpty()); // an active selection...
    CHECK_FALSE(zero.anySelected()); // ...that covers nothing
    CHECK_FALSE(zero.bounds().has_value());
}

TEST_CASE("rectangle: filled, clamped to the document, tight bounds") {
    const Selection s = Selection::rectangle(16, 16, {4, 4, 8, 8});
    CHECK(s.at(4, 4) == 255);
    CHECK(s.at(11, 11) == 255);
    CHECK(s.at(3, 4) == 0);
    CHECK(s.at(12, 11) == 0);
    const auto b = s.bounds();
    REQUIRE(b.has_value());
    CHECK(b->x == 4);
    CHECK(b->y == 4);
    CHECK(b->w == 8);
    CHECK(b->h == 8);

    // A rect hanging off the document clamps instead of writing out of bounds.
    const Selection edge = Selection::rectangle(16, 16, {12, -4, 100, 100});
    const auto eb = edge.bounds();
    REQUIRE(eb.has_value());
    CHECK(eb->x == 12);
    CHECK(eb->y == 0);
    CHECK(eb->w == 4);
    CHECK(eb->h == 16);
}

TEST_CASE("boolean ops: add is max, subtract removes overlap, intersect keeps it") {
    const Selection a = Selection::rectangle(16, 16, {0, 0, 8, 8});
    const Selection b = Selection::rectangle(16, 16, {4, 4, 8, 8});

    const Selection added = Selection::combine(a, b, SelectOp::Add);
    CHECK(added.at(0, 0) == 255);
    CHECK(added.at(11, 11) == 255);
    CHECK(added.at(12, 12) == 0);

    const Selection cut = Selection::combine(a, b, SelectOp::Subtract);
    CHECK(cut.at(0, 0) == 255);  // a-only survives
    CHECK(cut.at(5, 5) == 0);    // the overlap is removed
    CHECK(cut.at(11, 11) == 0);  // b-only was never in a

    const Selection both = Selection::combine(a, b, SelectOp::Intersect);
    CHECK(both.at(5, 5) == 255);
    CHECK(both.at(0, 0) == 0);
    CHECK(both.at(11, 11) == 0);
    const auto bb = both.bounds();
    REQUIRE(bb.has_value());
    CHECK(bb->x == 4);
    CHECK(bb->w == 4);

    // Replace adopts the new mask outright; an empty base acts as a zero mask.
    CHECK(Selection::combine(a, b, SelectOp::Replace) == b);
    CHECK(Selection::combine(Selection{}, b, SelectOp::Add) == b);
    CHECK_FALSE(Selection::combine(Selection{}, b, SelectOp::Intersect).anySelected());
}

TEST_CASE("SetSelectionCommand: undoable, redoable, clear via empty mask") {
    Document doc(16, 16);
    CHECK(doc.selection().isEmpty());

    doc.commands().push(std::make_unique<mosaic::core::SetSelectionCommand>(
        Selection::rectangle(16, 16, {2, 2, 4, 4})));
    CHECK(doc.selection().anySelected());
    CHECK(doc.selection().at(3, 3) == 255);

    doc.commands().push(std::make_unique<mosaic::core::SetSelectionCommand>(Selection{}));
    CHECK(doc.selection().isEmpty()); // Select -> None is just pushing the empty mask

    doc.commands().undo();
    CHECK(doc.selection().at(3, 3) == 255);
    doc.commands().undo();
    CHECK(doc.selection().isEmpty());
    doc.commands().redo();
    CHECK(doc.selection().at(3, 3) == 255);
}

TEST_CASE("inverted: complement per pixel; empty has no complement") {
    CHECK(Selection{}.inverted().isEmpty());

    const Selection s = Selection::rectangle(8, 8, {2, 2, 4, 4});
    const Selection inv = s.inverted();
    CHECK(inv.at(0, 0) == 255);
    CHECK(inv.at(3, 3) == 0);
    CHECK(inv.inverted() == s); // double inversion round-trips

    // Partial coverage flips around the midpoint.
    Selection partial(2, 1);
    partial.data()[0] = 100;
    CHECK(partial.inverted().at(0, 0) == 155);
    CHECK(partial.inverted().at(1, 0) == 255);
}

TEST_CASE("cropped: the selection carried into a cropped document (S16)") {
    CHECK(Selection{}.cropped(2, 2, 4, 4).isEmpty()); // nothing to carry

    // A 4x4 block at (4,4) in a 16x16 doc, cropped to the 8x8 window at (2,2).
    const Selection s = Selection::rectangle(16, 16, {4, 4, 4, 4});
    const Selection c = s.cropped(2, 2, 8, 8);
    REQUIRE_FALSE(c.isEmpty());
    CHECK(c.width() == 8);
    CHECK(c.height() == 8);
    CHECK(c.at(1, 1) == 0);   // old (3,3): unselected
    CHECK(c.at(2, 2) == 255); // old (4,4)
    CHECK(c.at(5, 5) == 255); // old (7,7)
    CHECK(c.at(6, 6) == 0);   // old (8,8): outside the block

    // A crop window partly hanging off the old mask copies only the overlap.
    const Selection off = s.cropped(-2, -2, 8, 8);
    CHECK(off.at(6, 6) == 255); // old (4,4)
    CHECK(off.at(3, 3) == 0);

    // Cropping all coverage away drops back to "no selection", not a mask of zeros.
    CHECK(s.cropped(10, 10, 4, 4).isEmpty());
}

TEST_CASE("selectionFromLayerPixels: identity raster layer copies its alpha") {
    Document doc(8, 8);
    auto layer = doc.makeRaster("a");
    setAlpha(layer->image(), 1, 1, 255);
    setAlpha(layer->image(), 2, 1, 100); // anti-aliased edges keep partial coverage
    const auto sel = selectionFromLayerPixels(*layer, doc.width(), doc.height());
    REQUIRE(sel.has_value());
    CHECK(sel->width() == 8);
    CHECK(sel->height() == 8);
    CHECK(sel->at(1, 1) == 255);
    CHECK(sel->at(2, 1) == 100);
    CHECK(sel->at(0, 0) == 0);
    CHECK(sel->anySelected());
}

TEST_CASE("selectionFromLayerPixels: the transform places the alpha in document space") {
    Document doc(8, 8);
    auto layer = doc.makeRaster("a");
    setAlpha(layer->image(), 0, 0, 255);
    layer->setTransform(Affine2D::translation(3, 2));
    const auto sel = selectionFromLayerPixels(*layer, doc.width(), doc.height());
    REQUIRE(sel.has_value());
    CHECK(sel->at(3, 2) == 255); // moved with the layer
    CHECK(sel->at(0, 0) == 0);

    // 2x scale: the source pixel covers a 2x2 document block.
    layer->setTransform(Affine2D::scaling(2, 2));
    const auto scaled = selectionFromLayerPixels(*layer, doc.width(), doc.height());
    REQUIRE(scaled.has_value());
    CHECK(scaled->at(0, 0) == 255);
    CHECK(scaled->at(1, 1) == 255);
    CHECK(scaled->at(2, 2) == 0);

    // A transform pushing the pixels off-canvas clips: nothing is selected.
    layer->setTransform(Affine2D::translation(100, 100));
    const auto off = selectionFromLayerPixels(*layer, doc.width(), doc.height());
    REQUIRE(off.has_value());
    CHECK_FALSE(off->anySelected());

    // A singular transform collapses to nothing, like the compositor's leaf walk.
    layer->setTransform(Affine2D::scaling(0, 0));
    const auto collapsed = selectionFromLayerPixels(*layer, doc.width(), doc.height());
    REQUIRE(collapsed.has_value());
    CHECK_FALSE(collapsed->anySelected());
}

TEST_CASE("selectionFromLayerPixels: only pixel-bearing layer kinds participate") {
    Document doc(4, 4);
    const auto group = doc.makeGroup("g");
    CHECK_FALSE(selectionFromLayerPixels(*group, 4, 4).has_value());

    mosaic::common::Image src(2, 2);
    setAlpha(src, 0, 0, 200);
    const mosaic::core::MagicLayer magic(99, "m", std::move(src));
    const auto sel = selectionFromLayerPixels(magic, 4, 4);
    REQUIRE(sel.has_value());
    CHECK(sel->at(0, 0) == 200); // magic layers select from their source
    CHECK(sel->at(1, 1) == 0);
}

// ---- the S14 marquee/lasso rasterisers ------------------------------------------------------

TEST_CASE("polygon: an axis-aligned square matches rectangle() exactly") {
    const std::vector<mosaic::common::Vec2> square{{2, 2}, {12, 2}, {12, 10}, {2, 10}};
    const Selection poly = Selection::polygon(16, 16, square);
    const Selection rect = Selection::rectangle(16, 16, {2, 2, 10, 8});
    CHECK(poly == rect); // pixel-aligned edges rasterise crisp, no stray AA
}

TEST_CASE("polygon: triangle coverage, area, and bounds") {
    const std::vector<mosaic::common::Vec2> tri{{1, 1}, {13, 1}, {1, 13}};
    const Selection s = Selection::polygon(16, 16, tri);
    CHECK(s.at(2, 2) == 255);  // interior
    CHECK(s.at(14, 14) == 0);  // outside
    CHECK(s.at(7, 6) > 0);     // straddles the hypotenuse (x+y=14): anti-aliased
    CHECK(s.at(7, 6) < 255);
    double area = 0.0;
    for (const std::uint8_t v : s.data())
        area += v / 255.0;
    CHECK(area == doctest::Approx(0.5 * 12 * 12).epsilon(0.02)); // ~analytic triangle area
    const auto b = s.bounds();
    REQUIRE(b.has_value());
    CHECK(b->x == 1.0);
    CHECK(b->y == 1.0);
}

TEST_CASE("polygon: degenerate inputs and document clamping") {
    CHECK_FALSE(Selection::polygon(8, 8, {}).anySelected());
    CHECK_FALSE(Selection::polygon(8, 8, {{1, 1}, {5, 5}}).anySelected());
    // A polygon hanging off the canvas clamps to the visible part.
    const std::vector<mosaic::common::Vec2> off{{-5, -5}, {8, -5}, {8, 8}, {-5, 8}};
    const Selection s = Selection::polygon(16, 16, off);
    CHECK(s == Selection::rectangle(16, 16, {0, 0, 8, 8}));
}

TEST_CASE("polygon: a self-crossing path follows the even-odd rule (lasso holes)") {
    // A bowtie: the two triangles fill, the pinch point between them stays empty.
    const std::vector<mosaic::common::Vec2> bowtie{{2, 2}, {14, 2}, {2, 14}, {14, 14}};
    const Selection s = Selection::polygon(16, 16, bowtie);
    CHECK(s.at(8, 3) == 255);  // top triangle
    CHECK(s.at(8, 13) == 255); // bottom triangle
    CHECK(s.at(4, 8) == 0);    // beside the pinch: even-odd leaves it out
}

TEST_CASE("ellipse: coverage, symmetry, area, and clamping") {
    // rx 15, ry 10, centred at (20, 15).
    const Selection e = Selection::ellipse(40, 30, {5, 5, 30, 20});
    CHECK(e.at(20, 15) == 255); // centre
    CHECK(e.at(6, 6) == 0);     // bounding-box corner lies outside the curve
    CHECK(e.at(30, 22) > 0);    // a 45-degree-diagonal pixel straddles the curve: anti-aliased
    CHECK(e.at(30, 22) < 255);
    // 4-way symmetry: pixel centres mirror across the ellipse centre (x -> 39-x, y -> 29-y).
    // The vertex ring is a multiple of 4 segments, so mirrored coverage matches within rounding.
    for (const auto [x, y] : {std::pair{30U, 22U}, {14U, 10U}, {8U, 15U}, {25U, 19U}}) {
        const int a = e.at(x, y);
        const int b = e.at(39 - x, 29 - y);
        CHECK(a - b >= -1);
        CHECK(a - b <= 1);
    }
    double area = 0.0;
    for (const std::uint8_t v : e.data())
        area += v / 255.0;
    CHECK(area == doctest::Approx(3.14159265 * 15 * 10).epsilon(0.02));

    CHECK_FALSE(Selection::ellipse(8, 8, {2, 2, 0, 5}).anySelected()); // empty rect = nothing
    // An ellipse centred off-canvas still rasterises its visible slice.
    const Selection part = Selection::ellipse(8, 8, {-6, -6, 12, 12});
    CHECK(part.at(0, 0) > 0);
    CHECK_FALSE(part.at(7, 7) > 0);
}

// ---- S16-i: translating the mask -------------------------------------------------------------

TEST_CASE("translated shifts coverage, clips at the document edge, and vacates to zero") {
    const Selection base = Selection::rectangle(16, 16, {2, 2, 4, 4}); // covers x,y in [2,6)

    const Selection right = base.translated(3, 0);
    CHECK(right.at(5, 3) == 255); // 2+3
    CHECK(right.at(8, 3) == 255); // 5+3, the last covered column
    CHECK(right.at(2, 3) == 0);   // vacated
    CHECK(right.at(9, 3) == 0);   // past the far edge of the moved rect

    const Selection diag = base.translated(-1, 2);
    CHECK(diag.at(1, 4) == 255);
    CHECK(diag.at(1, 3) == 0);

    // Coverage pushed past an edge is gone: the mask is document-sized, there is nowhere to keep it.
    const Selection clipped = base.translated(-4, 0); // the rect's left two columns fall off
    CHECK(clipped.at(0, 3) == 255);                   // was x=4
    CHECK(clipped.at(1, 3) == 255);                   // was x=5
    CHECK(clipped.at(2, 3) == 0);                     // nothing followed them
    CHECK(clipped.bounds()->w == 2);

    // Zero offset is identity; an empty selection has nothing to move.
    CHECK(base.translated(0, 0) == base);
    CHECK(Selection{}.translated(3, 3).isEmpty());
}

TEST_CASE("translating every covered pixel off the canvas is no-selection, not a zero mask") {
    const Selection base = Selection::rectangle(16, 16, {2, 2, 4, 4});
    const Selection gone = base.translated(-6, 0); // the whole rect leaves through the left edge
    CHECK(gone.isEmpty());                         // "no selection", never an all-zero active mask
    CHECK_FALSE(gone.anySelected());

    // A shift larger than the document short-circuits to the same answer.
    CHECK(base.translated(-40, 0).isEmpty());
    CHECK(base.translated(0, 999).isEmpty());
}

TEST_CASE("translating from the base is lossless across an edge; chaining results is not") {
    const Selection base = Selection::rectangle(16, 16, {0, 4, 4, 4}); // flush against the left edge

    // Out past the edge and back, always re-derived FROM THE BASE (what SelectionMoveGesture does).
    CHECK(base.translated(-2, 0).bounds()->w == 2); // half of it is off-canvas at the far point
    CHECK(base.translated(0, 0) == base);           // ... and it all comes back

    // Chaining the results instead erodes the mask permanently -- the reason the gesture keeps a
    // copy of the press-time base rather than re-translating what it last produced.
    const Selection chained = base.translated(-2, 0).translated(2, 0);
    CHECK(chained.bounds()->w == 2);
    CHECK(chained != base);
}

TEST_CASE("a nudge burst coalesces into one undo step; a fresh burst does not merge into it") {
    Document doc(16, 16);
    const Selection base = Selection::rectangle(16, 16, {2, 2, 4, 4});
    doc.commands().push(std::make_unique<mosaic::core::SetSelectionCommand>(base));
    const std::size_t afterSelect = doc.commands().undoCount();

    // Two nudges of one burst share a coalesce id: one step, holding the LATEST mask and the
    // ORIGINAL pre-burst selection as its undo target.
    doc.commands().push(std::make_unique<mosaic::core::SetSelectionCommand>(base.translated(1, 0),
                                                                            7, "Move Selection"));
    doc.commands().push(std::make_unique<mosaic::core::SetSelectionCommand>(base.translated(2, 0),
                                                                            7, "Move Selection"));
    CHECK(doc.commands().undoCount() == afterSelect + 1);
    CHECK(doc.selection() == base.translated(2, 0));
    doc.commands().undo();
    CHECK(doc.selection() == base); // straight back past the whole burst

    doc.commands().redo();
    // A different id (the canvas mints one per burst) never merges into the previous step.
    doc.commands().push(std::make_unique<mosaic::core::SetSelectionCommand>(base.translated(2, 1),
                                                                            8, "Move Selection"));
    CHECK(doc.commands().undoCount() == afterSelect + 2);

    // id 0 = never coalesce, so the ordinary marquee/Select-menu commits stay separate steps.
    doc.commands().push(std::make_unique<mosaic::core::SetSelectionCommand>(base));
    doc.commands().push(std::make_unique<mosaic::core::SetSelectionCommand>(base.translated(0, 1)));
    CHECK(doc.commands().undoCount() == afterSelect + 4);
}

// ---- Layer masks (S31): selection <-> mask conversion -------------------------------------------

TEST_CASE("revealAllMask sizes to the layer's grid: source image for raster, document otherwise") {
    Document doc(16, 12);
    auto raster = doc.makeRaster("r", 6, 4); // smaller than the canvas
    const auto* rl = raster.get();
    doc.root().addOnTop(std::move(raster));
    const mosaic::core::RasterMask rm = mosaic::core::revealAllMask(*rl, 16, 12);
    CHECK(rm.width == 6);
    CHECK(rm.height == 4);
    CHECK(rm.coverage == std::vector<std::uint8_t>(6 * 4, 255));
    CHECK(rm.enabled);
    CHECK(rm.linked);

    auto group = doc.makeGroup("g");
    const auto* gl = group.get();
    doc.root().addOnTop(std::move(group));
    const mosaic::core::RasterMask gm = mosaic::core::revealAllMask(*gl, 16, 12);
    CHECK(gm.width == 16);
    CHECK(gm.height == 12);
}

TEST_CASE("maskFromSelection copies coverage 1:1 on an untransformed document-sized layer") {
    Document doc(8, 8);
    auto raster = doc.makeRaster("r"); // canvas-sized
    const auto* rl = raster.get();
    doc.root().addOnTop(std::move(raster));
    const Selection sel = Selection::rectangle(8, 8, {2, 3, 4, 2});
    const mosaic::core::RasterMask m = mosaic::core::maskFromSelection(*rl, sel, 8, 8);
    CHECK(m.width == 8);
    CHECK(m.height == 8);
    CHECK(m.coverage == sel.data()); // byte-exact: both are 8-bit coverage
}

TEST_CASE("maskFromSelection back-maps through the layer transform (the mask reveals DOC pixels)") {
    Document doc(8, 8);
    auto raster = doc.makeRaster("r");
    auto* rl = raster.get();
    doc.root().addOnTop(std::move(raster));
    rl->setTransform(Affine2D::translation(3, 0)); // layer slid right by 3
    const Selection sel = Selection::rectangle(8, 8, {4, 0, 2, 8}); // doc columns 4..5
    const mosaic::core::RasterMask m = mosaic::core::maskFromSelection(*rl, sel, 8, 8);
    // Mask is on the LAYER grid: layer-local x maps to doc x+3, so local columns 1..2 are covered.
    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        return m.coverage[static_cast<std::size_t>(y) * m.width + x];
    };
    CHECK(at(0, 4) == 0);
    CHECK(at(1, 4) == 255);
    CHECK(at(2, 4) == 255);
    CHECK(at(3, 4) == 0);
}

TEST_CASE("maskFromSelection places a document-window sheet on the pixels it was built from") {
    // A doc-window sheet (every kind but raster/magic) is the selection VERBATIM, at document
    // resolution: the layer's transform rides in RasterMask::toLocal instead of skewing the
    // coverage, so the mask reveals the doc pixels the user selected and the dock's thumbnail
    // shows what they selected. Before the sheet carried a placement the coverage was back-mapped
    // into layer-local space instead, which pushed everything at negative local coordinates OFF
    // the sheet -- three quadrants of every shape layer (see test_layer_mask_grid.cpp).
    Document doc(8, 8);
    auto adj = doc.makeAdjustment("inv", mosaic::core::AdjustmentKind::Invert);
    auto* al = adj.get();
    doc.root().addOnTop(std::move(adj));
    al->setTransform(Affine2D::translation(3, 0)); // the adjustment slid right by 3
    const Selection sel = Selection::rectangle(8, 8, {4, 0, 2, 8}); // doc columns 4..5
    const mosaic::core::RasterMask m = mosaic::core::maskFromSelection(*al, sel, 8, 8);
    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        return m.coverage[static_cast<std::size_t>(y) * m.width + x];
    };
    CHECK(m.width == 8);
    CHECK(m.coverage == sel.data());
    CHECK(at(3, 4) == 0);
    CHECK(at(4, 4) == 255);
    CHECK(at(5, 4) == 255);
    // ...and the placement is what puts those cells back over doc columns 4..5: mask px -> doc is
    // the identity, whatever the layer's own transform.
    CHECK(mosaic::core::maskToDocument(*al, m) == Affine2D::identity());
}

TEST_CASE("maskFromSelection with no selection reveals everything (S13 empty semantics)") {
    Document doc(4, 4);
    auto raster = doc.makeRaster("r");
    const auto* rl = raster.get();
    doc.root().addOnTop(std::move(raster));
    const mosaic::core::RasterMask m = mosaic::core::maskFromSelection(*rl, Selection{}, 4, 4);
    CHECK(m.coverage == std::vector<std::uint8_t>(16, 255));
}

TEST_CASE("selectionFromLayerMask samples the mask where the compositor folds it") {
    Document doc(8, 8);
    auto raster = doc.makeRaster("r");
    auto* rl = raster.get();
    doc.root().addOnTop(std::move(raster));
    CHECK_FALSE(mosaic::core::selectionFromLayerMask(*rl, 8, 8).has_value()); // no mask yet

    mosaic::core::RasterMask m(8, 8, 0);
    for (std::uint32_t y = 0; y < 8; ++y) m.coverage[y * 8 + 2] = 255; // local column 2
    rl->setMask(m);
    rl->setTransform(Affine2D::translation(3, 0));

    // Linked: the mask rides the transform -> doc column 5.
    const auto linked = mosaic::core::selectionFromLayerMask(*rl, 8, 8);
    REQUIRE(linked.has_value());
    CHECK(linked->at(5, 4) == 255);
    CHECK(linked->at(2, 4) == 0);

    // Unlinked: the mask sits still in parent (document) space -> doc column 2.
    rl->mask()->linked = false;
    const auto unlinked = mosaic::core::selectionFromLayerMask(*rl, 8, 8);
    REQUIRE(unlinked.has_value());
    CHECK(unlinked->at(2, 4) == 255);
    CHECK(unlinked->at(5, 4) == 0);

    // A coverage-free mask collapses to "no selection", never an active selection of nothing.
    rl->setMask(mosaic::core::RasterMask(8, 8, 0));
    const auto none = mosaic::core::selectionFromLayerMask(*rl, 8, 8);
    REQUIRE(none.has_value());
    CHECK(none->isEmpty());
}
