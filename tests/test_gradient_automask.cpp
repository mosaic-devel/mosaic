// Gradient automask (S22 + the S32 "select, then apply" reflex): a gradient laid down with an
// active selection is born MASKED to it, exactly as Filter > Adjustments builds its layer
// (docs/adjustment-layers.md §5). These cases pin the two halves MainWindow::previewGradient /
// commitGradient own and that the FLTK layer above them cannot be exercised headlessly:
//
//   1. the COMPOSITE -- byte-identical to no gradient at all outside the selection, byte-identical
//      to the unmasked gradient inside it, and halfway between under half coverage (a feathered or
//      merely anti-aliased selection must not harden into a binary cut-out on its way to a mask);
//   2. the PLACEMENT -- a gradient layer is a full-bleed VectorLayer whose transform is a
//      half-document translation, so its mask sheet is the document window pinned by
//      RasterMask::toLocal = the inverse of that transform, captured AFTER it is set (the grid
//      contract in core/layer.hpp; getting this wrong is the bug that erased three quadrants of
//      every shape layer);
//   3. the UNDO STEP -- the mask rides INSIDE the layer the commit hands to AddLayerCommand, so
//      layer and mask are one History entry in both directions;
//   4. the GATE -- Selection::anySelected() alone, and what each of the two rejected states would
//      otherwise produce.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/command.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/selection.hpp"
#include "core/vector/paint.hpp"
#include "render/compositor.hpp"
#include "ui/gradient_gesture.hpp"

using namespace mosaic;

namespace {

constexpr std::uint32_t kW = 8;
constexpr std::uint32_t kH = 1;

common::Image flatten(const core::Document& doc) {
    const render::CompositeResult r = render::composite(doc, {}, render::Backend::Cpu);
    REQUIRE(r.ok);
    return r.image;
}

common::Color8 px(const common::Image& img, std::uint32_t x, std::uint32_t y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

// An opaque red base under the gradient, so every pixel of the composite differs from every pixel
// of the ramp -- "the gradient did nothing here" is then visible at every x, not just some.
void seedBase(core::Document& doc) {
    auto base = doc.makeRaster("base", kW, kH);
    base->image().fill({255, 0, 0, 255});
    doc.root().addOnTop(std::move(base));
}

std::vector<core::vec::GradientStop> blueToGreen() {
    return {{0.0, core::vec::ColorF{0.0f, 0.0f, 1.0f, 1.0f}},
            {1.0, core::vec::ColorF{0.0f, 1.0f, 0.0f, 1.0f}}};
}

// The draft a left-to-right drag across the whole document authors -- the same call
// VulkanCanvas::currentGradientDraft makes, so the object AND the half-document placement under
// test are the tool's real ones.
ui::GradientDraft fullWidthDraft() {
    const std::optional<ui::GradientDraft> d = ui::buildGradientDraft(
        ui::GradientShape::Linear, {0.5, 0.5}, {static_cast<double>(kW) - 0.5, 0.5},
        static_cast<double>(kW), static_cast<double>(kH), blueToGreen(),
        core::vec::SpreadMethod::Pad, /*shift=*/false);
    REQUIRE(d.has_value());
    return *d;
}

// MainWindow::previewGradient's layer half: insert the live layer, then author it. The mask is
// deliberately NOT set here -- each case decides, because WHEN it is set relative to setTransform
// is precisely what case 2 is about.
core::VectorLayer* addGradientLayer(core::Document& doc, const ui::GradientDraft& draft) {
    std::unique_ptr<core::VectorLayer> layer = doc.makeVector("Gradient");
    core::VectorLayer* raw = layer.get();
    doc.root().addOnTop(std::move(layer));
    raw->setObject(draft.object);
    raw->setTransform(draft.placement);
    return raw;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// 1 + 2. The composite, and the sheet's placement
// ---------------------------------------------------------------------------------------------

TEST_CASE("a gradient born masked to the selection paints only inside it") {
    const ui::GradientDraft draft = fullWidthDraft();

    core::Document plain(kW, kH);
    seedBase(plain);
    const common::Image untouched = flatten(plain); // no gradient at all

    core::Document full(kW, kH);
    seedBase(full);
    addGradientLayer(full, draft);
    const common::Image gradientFull = flatten(full); // the unmasked gradient

    core::Document doc(kW, kH);
    seedBase(doc);
    core::VectorLayer* vl = addGradientLayer(doc, draft);
    core::Selection sel(kW, kH);
    sel.data() = {0, 0, 255, 255, 128, 128, 0, 0}; // out, out, in, in, half, half, out, out
    vl->setMask(core::maskFromSelection(*vl, sel, kW, kH));

    // The grid contract, pinned before a single pixel is compared: built AFTER setTransform, the
    // document-window sheet lands 1:1 on the document, so maskFromSelection took its fast path and
    // the coverage IS the selection -- feather, AA edge and all.
    REQUIRE(vl->mask() != nullptr);
    CHECK(core::maskToDocument(*vl, *vl->mask()) == common::Affine2D::identity());
    CHECK(vl->mask()->width == kW);
    CHECK(vl->mask()->height == kH);
    CHECK(vl->mask()->coverage == sel.data());

    const common::Image out = flatten(doc);
    for (std::uint32_t x = 0; x < kW; ++x) {
        CAPTURE(x);
        const common::Color8 got = px(out, x, 0);
        const common::Color8 off = px(untouched, x, 0);
        const common::Color8 on = px(gradientFull, x, 0);
        if (x < 2 || x >= 6) { // unselected: byte-identical to never having dragged
            CHECK(got.r == off.r);
            CHECK(got.g == off.g);
            CHECK(got.b == off.b);
            CHECK(got.a == off.a);
        } else if (x < 4) { // fully selected: the unmasked gradient, byte for byte
            CHECK(got.r == on.r);
            CHECK(got.g == on.g);
            CHECK(got.b == on.b);
            CHECK(got.a == on.a);
        } else { // half covered: halfway between the two (the ramp survived the mask)
            CHECK(std::abs(int{got.r} - (int{off.r} + int{on.r}) / 2) <= 2);
            CHECK(std::abs(int{got.g} - (int{off.g} + int{on.g}) / 2) <= 2);
            CHECK(std::abs(int{got.b} - (int{off.b} + int{on.b}) / 2) <= 2);
        }
    }
    // The masked and unmasked gradients really are different pictures -- otherwise every check
    // above would pass on a mask that did nothing.
    CHECK(out.rgba != gradientFull.rgba);
    CHECK(out.rgba != untouched.rgba);
}

TEST_CASE("gradient automask captures its sheet AFTER the layer transform, not before") {
    // Why previewGradient builds the mask below setTransform: a gradient layer's transform is a
    // translation to the DOCUMENT CENTRE (the full-bleed rect is authored around the local origin),
    // so a sheet captured while the transform was still the identity is placed half a document away
    // from the pixels it was built from -- the shape-layer "three erased quadrants" bug.
    const ui::GradientDraft draft = fullWidthDraft();
    core::Selection sel(kW, kH);
    sel.data() = {0, 0, 255, 255, 255, 255, 0, 0};

    core::Document good(kW, kH);
    seedBase(good);
    core::VectorLayer* right = addGradientLayer(good, draft); // object + transform, then...
    right->setMask(core::maskFromSelection(*right, sel, kW, kH));
    REQUIRE(right->mask() != nullptr);
    CHECK(core::maskToDocument(*right, *right->mask()) == common::Affine2D::identity());

    core::Document bad(kW, kH);
    seedBase(bad);
    std::unique_ptr<core::VectorLayer> layer = bad.makeVector("Gradient");
    core::VectorLayer* wrong = layer.get();
    bad.root().addOnTop(std::move(layer));
    wrong->setObject(draft.object);
    wrong->setMask(core::maskFromSelection(*wrong, sel, kW, kH)); // ... captured too early
    wrong->setTransform(draft.placement);
    REQUIRE(wrong->mask() != nullptr);
    // The identity capture no longer agrees with the layer: the sheet now rides the placement.
    CHECK_FALSE(core::maskToDocument(*wrong, *wrong->mask()) == common::Affine2D::identity());
    CHECK(flatten(good).rgba != flatten(bad).rgba);
}

// ---------------------------------------------------------------------------------------------
// 3. One History step
// ---------------------------------------------------------------------------------------------

TEST_CASE("gradient automask: layer and mask are ONE undo step") {
    // MainWindow::commitGradient detaches the live preview layer and re-adds it THROUGH an
    // AddLayerCommand. The automask is already on that layer, so it rides inside the command:
    // undo takes both away, redo brings both back, and there is no second entry for the mask.
    const ui::GradientDraft draft = fullWidthDraft();
    core::Document doc(kW, kH);
    seedBase(doc);
    const common::Image baseOnly = flatten(doc);
    const std::size_t undoBefore = doc.commands().undoCount();

    core::VectorLayer* vl = addGradientLayer(doc, draft); // inserted OUTSIDE the command stack
    const core::LayerId id = vl->id();
    core::Selection sel(kW, kH);
    sel.data() = {0, 0, 255, 255, 255, 255, 0, 0};
    vl->setMask(core::maskFromSelection(*vl, sel, kW, kH));
    CHECK(doc.commands().undoCount() == undoBefore); // dragging never spams History

    const std::optional<core::Document::Location> loc = doc.locate(id);
    REQUIRE(loc.has_value());
    std::unique_ptr<core::Layer> detached = loc->parent->removeAt(loc->index);
    REQUIRE(detached != nullptr);
    CHECK(detached->mask() != nullptr); // the mask travels with the layer, not beside it
    auto cmd = std::make_unique<core::CompositeCommand>(std::string("Add Gradient"));
    cmd->add(std::make_unique<core::AddLayerCommand>(loc->parent->id(), loc->index,
                                                     std::move(detached)));
    doc.commands().push(std::move(cmd));

    CHECK(doc.commands().undoCount() == undoBefore + 1); // exactly one step for layer + mask
    REQUIRE(doc.find(id) != nullptr);
    CHECK(doc.find(id)->mask() != nullptr);
    const common::Image committed = flatten(doc);
    CHECK(committed.rgba != baseOnly.rgba);

    doc.commands().undo();
    CHECK(doc.find(id) == nullptr);
    CHECK(doc.commands().undoCount() == undoBefore);
    CHECK(flatten(doc).rgba == baseOnly.rgba); // one undo, and the mask left with the layer

    doc.commands().redo();
    REQUIRE(doc.find(id) != nullptr);
    REQUIRE(doc.find(id)->mask() != nullptr);
    CHECK(doc.find(id)->mask()->coverage == sel.data());
    CHECK(flatten(doc).rgba == committed.rgba);
}

// ---------------------------------------------------------------------------------------------
// 4. The gate
// ---------------------------------------------------------------------------------------------

TEST_CASE("gradient automask: no selection leaves the layer maskless (full-bleed, as before)") {
    // previewGradient gates on Selection::anySelected() alone -- the same gate
    // insertAdjustmentLayer uses, false in both of the states below.
    const core::Selection none;
    CHECK(none.isEmpty());
    CHECK_FALSE(none.anySelected());

    core::Selection empty(kW, kH); // active, but covering nothing
    CHECK_FALSE(empty.isEmpty());
    CHECK_FALSE(empty.anySelected());

    core::Selection some(kW, kH); // any coverage at all is admitted
    some.data()[3] = 1;
    CHECK(some.anySelected());

    const ui::GradientDraft draft = fullWidthDraft();
    core::Document doc(kW, kH);
    seedBase(doc);
    core::VectorLayer* vl = addGradientLayer(doc, draft);
    CHECK(vl->mask() == nullptr); // the gate said no: nothing is set, and the drag is unchanged

    core::Document reference(kW, kH);
    seedBase(reference);
    addGradientLayer(reference, draft);
    CHECK(flatten(doc).rgba == flatten(reference).rgba);

    // What the gate PREVENTS, spelled out. An EMPTY selection would give maskFromSelection's
    // reveal-all sheet: invisible in the composite, but a lie in the dock (a mask thumbnail for a
    // mask the user never asked for)...
    const core::RasterMask revealAll = core::maskFromSelection(*vl, none, kW, kH);
    CHECK(revealAll.coverage.size() == static_cast<std::size_t>(kW) * kH);
    CHECK(std::all_of(revealAll.coverage.begin(), revealAll.coverage.end(),
                      [](std::uint8_t c) { return c == 255; }));
    // ... and an active selection of NOTHING would give an all-zero sheet: a gradient that paints
    // no pixel, which reads as "the drag did nothing".
    const core::RasterMask blank = core::maskFromSelection(*vl, empty, kW, kH);
    CHECK(blank.coverage.size() == static_cast<std::size_t>(kW) * kH);
    CHECK(std::all_of(blank.coverage.begin(), blank.coverage.end(),
                      [](std::uint8_t c) { return c == 0; }));
}
