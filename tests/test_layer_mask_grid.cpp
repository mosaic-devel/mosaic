#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <string>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/selection.hpp"
#include "core/vector/geometry.hpp"
#include "core/vector/object.hpp"
#include "core/vector/paint.hpp"
#include "render/compositor.hpp"

// The RasterMask GRID CONTRACT (core/layer.hpp), asserted in PIXELS through the compositor.
//
// The bug these pin: a mask's sheet used to be pinned to layer-local (0,0), and a SHAPE layer's
// local origin is the shape's own CENTRE (shape authoring keeps the geometry centred and puts the
// position in the transform, ui/shape_gesture.hpp). Everything at negative local coordinates --
// three quadrants of every shape -- therefore fell off the sheet, where the fold reads ZERO
// coverage, so merely ADDING a mask erased most of the shape and "Mask from Selection" revealed at
// most the quarter of it below and right of its centre. The sheet now carries its placement
// (RasterMask::toLocal), captured from the layer's world transform when it is built.
//
// Every case below is transform-sensitive on purpose: an axis-aligned, identity-placed layer
// passes against the bug, because at the document origin the broken map IS the right one.
using namespace mosaic;

namespace {

constexpr common::Color8 kBg{200, 40, 40, 255};
constexpr common::ColorF kShapeF{0.1f, 0.35f, 0.9f, 1.0f};

common::Image flatten(const core::Document& doc) {
    const render::CompositeResult r = render::composite(doc, {}, render::Backend::Cpu);
    REQUIRE(r.ok);
    return r.image;
}

common::Color8 px(const common::Image& img, std::uint32_t x, std::uint32_t y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

bool nearColor(common::Color8 a, common::Color8 b) {
    const auto d = [](std::uint8_t u, std::uint8_t v) {
        return u > v ? u - v : v - u;
    };
    return d(a.r, b.r) <= 1 && d(a.g, b.g) <= 1 && d(a.b, b.b) <= 1 && d(a.a, b.a) <= 1;
}

// An opaque background so a hidden pixel is unmistakably different from a revealed one.
void addBackground(core::Document& doc) {
    auto bg = doc.makeRaster("bg");
    bg->image().fill(kBg);
    doc.root().addOnTop(std::move(bg));
}

// A shape layer exactly as the shape tool authors one: parametric geometry CENTRED on the local
// origin, placed by the layer transform (ui/shape_gesture.hpp buildShapeDraft).
core::VectorLayer* addShape(core::Document& doc, common::Vec2 size, const common::Affine2D& place) {
    auto v = doc.makeVector("shape");
    core::vec::Object o;
    o.geometry = core::vec::ParametricShape{core::vec::RectShape::uniform(size, 0.0)};
    o.fill = core::vec::SolidPaint{kShapeF};
    v->setObject(std::move(o));
    v->setTransform(place);
    auto* raw = v.get();
    doc.root().addOnTop(std::move(v));
    return raw;
}

// The whole point, in one assertion: with the mask on, every document pixel the selection covered
// looks exactly as it did unmasked, and every pixel it did not covers nothing at all. Counts
// rather than per-pixel CHECKs so a failure reports one number and one sample coordinate.
void checkRevealedExactlyWhereSelected(const common::Image& unmasked, const common::Image& masked,
                                       const core::Selection& sel) {
    std::size_t wrongRevealed = 0, wrongHidden = 0;
    std::string firstBad;
    for (std::uint32_t y = 0; y < masked.height; ++y) {
        for (std::uint32_t x = 0; x < masked.width; ++x) {
            const bool selected = sel.at(x, y) == 255;
            const common::Color8 want = selected ? px(unmasked, x, y) : kBg;
            if (nearColor(px(masked, x, y), want)) continue;
            (selected ? wrongRevealed : wrongHidden)++;
            if (firstBad.empty())
                firstBad = "(" + std::to_string(x) + "," + std::to_string(y) + ")";
        }
    }
    INFO("first mismatching pixel: ", firstBad);
    CHECK(wrongRevealed == 0); // the selected pixels must survive -- the erased-quadrant bug
    CHECK(wrongHidden == 0);   // and nothing outside the selection may show
}

} // namespace

// ---- Adding a mask must never change what you see --------------------------------------------
// A reveal-all mask is the sharpest regression test there is: it covers everything, so the
// composite MUST be byte-identical. Under the old rule it deleted whatever sat at negative local
// coordinates, which on a shape layer is most of the shape.

TEST_CASE("a reveal-all mask on a shape layer changes nothing") {
    core::Document doc(64, 64);
    addBackground(doc);
    core::VectorLayer* shape = addShape(doc, {24, 16}, common::Affine2D::translation(40, 44));

    const common::Image before = flatten(doc);
    CHECK(px(before, 30, 40) != kBg); // the shape's top-left quadrant really is drawn

    shape->setMask(core::revealAllMask(*shape, 64, 64));
    const common::Image after = flatten(doc);
    CHECK(after.rgba == before.rgba);
}

TEST_CASE("a reveal-all mask on a rotated, scaled shape layer changes nothing") {
    core::Document doc(64, 64);
    addBackground(doc);
    core::VectorLayer* shape =
        addShape(doc, {24, 16},
                 common::Affine2D::trs({34, 30}, std::numbers::pi / 6.0, {1.5, 1.5}));

    const common::Image before = flatten(doc);
    shape->setMask(core::revealAllMask(*shape, 64, 64));
    CHECK(flatten(doc).rgba == before.rgba);
}

TEST_CASE("a reveal-all mask on a transformed group changes nothing") {
    // Same defect, one kind over: a group's sheet was pinned to GROUP-local (0,0) too, so a group
    // whose own transform offsets its local space lost everything above/left of that origin.
    core::Document doc(64, 64);
    addBackground(doc);
    auto group = doc.makeGroup("g");
    group->setTransform(common::Affine2D::translation(10, 6));
    auto member = doc.makeRaster("m", 64, 64);
    member->image().fill(common::Color8{250, 250, 250, 255});
    member->setTransform(common::Affine2D::translation(-10, -6)); // net identity: covers the doc
    group->addOnTop(std::move(member));
    auto* raw = group.get();
    doc.root().addOnTop(std::move(group));

    const common::Image before = flatten(doc);
    CHECK(px(before, 2, 2) == common::Color8{250, 250, 250, 255});

    raw->setMask(core::revealAllMask(*raw, 64, 64));
    CHECK(flatten(doc).rgba == before.rgba);
}

// ---- Mask from Selection ---------------------------------------------------------------------

TEST_CASE("Mask from Selection on a shape layer reveals exactly the selected document pixels") {
    core::Document doc(64, 64);
    addBackground(doc);
    // Shape spanning doc [28,52) x [36,52) -- its local space runs [-12,12) x [-8,8).
    core::VectorLayer* shape = addShape(doc, {24, 16}, common::Affine2D::translation(40, 44));
    const common::Image unmasked = flatten(doc);

    // Everything left of doc x = 40: the shape's LEFT half, which is exactly the part the old
    // sheet could not address at all (local x < 0).
    const core::Selection sel = core::Selection::rectangle(64, 64, {0, 0, 40, 64});
    shape->setMask(core::maskFromSelection(*shape, sel, 64, 64));
    const common::Image masked = flatten(doc);

    CHECK(px(masked, 30, 40) == px(unmasked, 30, 40)); // selected half survives
    CHECK(nearColor(px(masked, 48, 40), kBg));         // unselected half is gone
    checkRevealedExactlyWhereSelected(unmasked, masked, sel);
}

TEST_CASE("Mask from Selection holds under a rotation + scale") {
    core::Document doc(64, 64);
    addBackground(doc);
    core::VectorLayer* shape =
        addShape(doc, {24, 16},
                 common::Affine2D::trs({34, 30}, std::numbers::pi / 6.0, {1.5, 1.5}));
    const common::Image unmasked = flatten(doc);

    const core::Selection sel = core::Selection::rectangle(64, 64, {0, 0, 34, 64});
    shape->setMask(core::maskFromSelection(*shape, sel, 64, 64));
    // The sheet is captured in DOCUMENT pixels, so the coverage is the selection verbatim -- the
    // rotation rides in the placement instead of resampling (and quantising) the mask.
    CHECK(shape->mask()->coverage == sel.data());
    checkRevealedExactlyWhereSelected(unmasked, flatten(doc), sel);
}

TEST_CASE("Mask from Selection on a raster layer still masks on its SOURCE grid") {
    // The other half of the contract, unchanged: a raster sheet is the image's own pixels, so a
    // smaller-than-canvas raster gets a smaller mask and the selection is back-mapped into it.
    core::Document doc(16, 12);
    auto raster = doc.makeRaster("r", 6, 4);
    raster->image().fill(common::Color8{255, 255, 255, 255});
    raster->setTransform(common::Affine2D::translation(3, 1));
    auto* raw = raster.get();
    doc.root().addOnTop(std::move(raster));

    const core::Selection sel = core::Selection::rectangle(16, 12, {5, 1, 2, 4}); // doc cols 5..6
    const core::RasterMask m = core::maskFromSelection(*raw, sel, 16, 12);
    CHECK(m.width == 6);
    CHECK(m.height == 4);
    CHECK(m.toLocal == common::Affine2D::identity());
    CHECK(m.coverage[0] == 0);   // local (0,0) = doc (3,1): outside
    CHECK(m.coverage[2] == 255); // local (2,0) = doc (5,1): inside
    CHECK(m.coverage[3] == 255); // local (3,0) = doc (6,1): inside
    CHECK(m.coverage[4] == 0);   // local (4,0) = doc (7,1): outside
}

// ---- Linked / unlinked, after the layer moves -------------------------------------------------

TEST_CASE("a linked shape mask rides a later move") {
    core::Document doc(64, 64);
    addBackground(doc);
    core::VectorLayer* shape = addShape(doc, {24, 16}, common::Affine2D::translation(40, 44));
    const core::Selection sel = core::Selection::rectangle(64, 64, {0, 0, 40, 64});
    shape->setMask(core::maskFromSelection(*shape, sel, 64, 64));
    REQUIRE(shape->mask()->linked);

    // Slide the shape 8 px right: it now spans doc [36,60), and the half its mask reveals must
    // have travelled with it -- doc [36,48).
    shape->setTransform(common::Affine2D::translation(48, 44));
    const common::Image moved = flatten(doc);
    CHECK(px(moved, 38, 44) != kBg); // still revealed
    CHECK(px(moved, 44, 44) != kBg); // travelled past the ORIGINAL selection edge with the shape
    CHECK(nearColor(px(moved, 56, 44), kBg)); // the masked-out half is still masked out
}

TEST_CASE("an unlinked shape mask stays pinned in document space, and unlinking does not move it") {
    core::Document doc(64, 64);
    addBackground(doc);
    core::VectorLayer* shape = addShape(doc, {24, 16}, common::Affine2D::translation(40, 44));
    const core::Selection sel = core::Selection::rectangle(64, 64, {0, 0, 40, 64});
    doc.commands().push(std::make_unique<core::SetLayerMaskCommand>(
        shape->id(), core::maskFromSelection(*shape, sel, 64, 64), "Mask from Selection"));
    const common::Image linked = flatten(doc);

    // Clicking the chain must not move the mask -- only stop it following the layer from now on.
    doc.commands().push(std::make_unique<core::SetMaskLinkedCommand>(shape->id(), false));
    REQUIRE_FALSE(shape->mask()->linked);
    CHECK(flatten(doc).rgba == linked.rgba);

    // Now the same move: the mask stays in DOCUMENT space, so the shape slides out from under it.
    shape->setTransform(common::Affine2D::translation(48, 44));
    const common::Image moved = flatten(doc);
    CHECK(px(moved, 38, 44) != kBg);      // doc x < 40 is still what the mask reveals
    CHECK(nearColor(px(moved, 44, 44), kBg));  // ...and the shape moved out from under it here
    CHECK(nearColor(px(moved, 56, 44), kBg));

    doc.commands().undo(); // the unlink undoes cleanly, placement included
    CHECK(shape->mask()->linked);
    shape->setTransform(common::Affine2D::translation(40, 44));
    CHECK(flatten(doc).rgba == linked.rgba);
}

// ---- The neighbouring surfaces ----------------------------------------------------------------

TEST_CASE("selectionFromLayerMask round-trips a shape layer's mask") {
    core::Document doc(64, 64);
    addBackground(doc);
    core::VectorLayer* shape =
        addShape(doc, {24, 16},
                 common::Affine2D::trs({34, 30}, std::numbers::pi / 6.0, {1.5, 1.5}));
    const core::Selection sel = core::Selection::rectangle(64, 64, {8, 12, 20, 30});
    shape->setMask(core::maskFromSelection(*shape, sel, 64, 64));

    // Shift-clicking the mask thumbnail must give back the selection it was built from, in
    // document space -- the same map the compositor folds through.
    const auto back = core::selectionFromLayerMask(*shape, 64, 64);
    REQUIRE(back.has_value());
    CHECK(back->data() == sel.data());
}

TEST_CASE("Add to Mask combines on the sheet already there, not a freshly captured one") {
    // The Select menu's Add/Subtract combine the new coverage with the existing mask BYTE-WISE, so
    // both have to be the same sheet. Re-capturing after the layer had moved would silently mix
    // two sheets that no longer align.
    core::Document doc(64, 64);
    addBackground(doc);
    core::VectorLayer* shape = addShape(doc, {24, 16}, common::Affine2D::translation(40, 44));
    shape->setMask(core::maskFromSelection(
        *shape, core::Selection::rectangle(64, 64, {0, 0, 40, 64}), 64, 64));
    const common::Affine2D sheet = shape->mask()->toLocal;

    shape->setTransform(common::Affine2D::translation(48, 44)); // moved since
    const core::RasterMask again = core::maskFromSelection(
        *shape, core::Selection::rectangle(64, 64, {0, 0, 40, 64}), 64, 64);
    CHECK(again.toLocal == sheet);
    CHECK(again.width == shape->mask()->width);
    CHECK(again.height == shape->mask()->height);
}
