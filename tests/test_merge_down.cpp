#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/blend_mode.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/vector/geometry.hpp"
#include "core/vector/object.hpp"
#include "core/vector/paint.hpp"
#include "render/compositor.hpp"

// Layer -> Merge Down across layer KINDS (S36). The load-bearing claim of every route is the same
// one: the merged document must composite to the pixels the canvas already showed. So each case
// takes the composite BEFORE the merge and asserts the merge's own output is byte-identical to it,
// then lands the real one-undo-step command shape and re-composites.
//
// Every comparison here is EXACT, on purpose. render::mergeDownBaked replays the compositor's own
// two walk steps (compositeBufferOver onto an empty accumulator, lower then upper), so for the
// bottom two layers of a document it is not "close to" the live composite -- it is the same
// arithmetic on the same buffers.

using namespace mosaic;
using core::BlendMode;

namespace {

// The filter render::composite's DEFAULT CompositeOptions uses. Baking with any other one would
// change vector edges (Nearest hardens them), so the merge and the composite must agree -- the
// same trap rasterizeLayer's `filter` argument exists to close.
constexpr render::ResampleFilter kFilter = render::ResampleFilter::Nearest;

common::Image flatten(const core::Document& doc) {
    const render::CompositeResult r = render::composite(doc, {}, render::Backend::Cpu);
    REQUIRE(r.ok);
    return r.image;
}

core::vec::Object solidRect(double w, double h, common::ColorF c) {
    core::vec::Object o;
    o.geometry = core::vec::RectShape{{w, h}};
    o.fill = core::vec::SolidPaint{c};
    return o;
}

// A "gradient layer": the same VectorLayer kind, distinguished only by the paint (vector-model.md
// §1). The gradient's unit space is mapped onto the rect's local x so the ramp really spans it.
core::vec::Object gradientRect(double w, double h) {
    core::vec::Object o;
    o.geometry = core::vec::RectShape{{w, h}};
    core::vec::Gradient g;
    g.type = core::vec::GradientType::Linear;
    g.stops.push_back(core::vec::GradientStop{0.0, common::ColorF{1.0f, 0.2f, 0.0f, 1.0f}});
    g.stops.push_back(core::vec::GradientStop{1.0, common::ColorF{0.0f, 0.3f, 1.0f, 1.0f}});
    g.transform = common::Affine2D::translation(-w * 0.5, 0.0) * common::Affine2D::scaling(w, 1.0);
    o.fill = g;
    return o;
}

// The ONE undo step MainWindow::mergeDownLayer pushes: the lower layer is rewritten and the upper
// one removed together, so a single undo restores both.
core::LayerId landMerge(core::Document& doc, core::LayerId upperId, core::LayerId lowerId,
                        std::unique_ptr<core::Layer> replacement) {
    const core::LayerId newId = replacement->id();
    auto cmd = std::make_unique<core::CompositeCommand>("Merge Down");
    cmd->add(
        std::make_unique<core::ReplaceLayerCommand>(lowerId, std::move(replacement), "Merge Down"));
    cmd->add(std::make_unique<core::RemoveLayerCommand>(upperId));
    doc.commands().push(std::move(cmd));
    return newId;
}

// The pixel route as the app runs it: bake both sides, land the result as a plain raster (identity
// transform, opacity 1, Normal blend -- mergeDownBaked has already consumed all of that).
std::unique_ptr<core::RasterLayer> bakedRaster(core::Document& doc, common::Image pixels,
                                               const std::string& name) {
    std::unique_ptr<core::RasterLayer> raster = doc.makeRaster(name, pixels.width, pixels.height);
    raster->image() = std::move(pixels);
    return raster;
}

// A grayscale mask sheet that hides the left `cut` columns of a `w` x `h` grid.
core::RasterMask leftCutMask(std::uint32_t w, std::uint32_t h, std::uint32_t cut) {
    core::RasterMask m(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            m.coverage[static_cast<std::size_t>(y) * w + x] = x < cut ? 0 : 255;
    return m;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// shape + shape -> ONE vector layer (the route that keeps the geometry editable)
// ---------------------------------------------------------------------------------------------
TEST_CASE("Merge Down: two disjoint shape layers combine into one vector layer") {
    core::Document doc(64, 64);
    auto lowerPtr = doc.makeVector("shape A");
    lowerPtr->setObject(solidRect(16.0, 16.0, common::ColorF{0.9f, 0.1f, 0.1f, 1.0f}));
    lowerPtr->setTransform(common::Affine2D::translation(16, 16));
    const core::LayerId lowerId = lowerPtr->id();
    const core::VectorLayer* lower = lowerPtr.get();
    doc.root().addOnTop(std::move(lowerPtr));

    auto upperPtr = doc.makeVector("shape B");
    upperPtr->setObject(solidRect(16.0, 16.0, common::ColorF{0.9f, 0.1f, 0.1f, 1.0f}));
    upperPtr->setTransform(common::Affine2D::translation(46, 46));
    const core::LayerId upperId = upperPtr->id();
    const core::VectorLayer* upper = upperPtr.get();
    doc.root().addOnTop(std::move(upperPtr));

    const common::Image before = flatten(doc);

    std::optional<core::vec::Object> combined = render::mergeDownVector(*upper, *lower);
    REQUIRE(combined.has_value());
    // Two contours in ONE object: subpaths merged, no boolean op, nothing rasterized.
    const auto* path = std::get_if<core::vec::Path>(&combined->geometry);
    REQUIRE(path != nullptr);
    CHECK(path->subpaths.size() == 2);

    auto merged = doc.makeVector(lower->name());
    merged->setTransform(lower->transform());  // the combine is in the lower layer's local space
    merged->setObject(std::move(*combined));
    const core::LayerId mergedId = landMerge(doc, upperId, lowerId, std::move(merged));

    CHECK(doc.root().childCount() == 1);
    CHECK(doc.root().child(0).kind() == core::LayerKind::Vector);  // still editable geometry
    CHECK(doc.root().child(0).id() == mergedId);
    CHECK(flatten(doc).rgba == before.rgba);

    // ONE undo puts BOTH layers back, in their original order.
    doc.commands().undo();
    REQUIRE(doc.root().childCount() == 2);
    CHECK(doc.root().child(0).id() == lowerId);
    CHECK(doc.root().child(1).id() == upperId);
    CHECK(flatten(doc).rgba == before.rgba);
    doc.commands().redo();
    REQUIRE(doc.root().childCount() == 1);
    CHECK(doc.root().child(0).id() == mergedId);
    CHECK(flatten(doc).rgba == before.rgba);
}

TEST_CASE("Merge Down declines the vector route whenever one object cannot carry both layers") {
    const auto shapeDoc = [](core::Document& doc) {
        auto a = doc.makeVector("A");
        a->setObject(solidRect(12.0, 12.0, common::ColorF{0.2f, 0.5f, 0.9f, 1.0f}));
        a->setTransform(common::Affine2D::translation(16, 16));
        doc.root().addOnTop(std::move(a));
        auto b = doc.makeVector("B");
        b->setObject(solidRect(12.0, 12.0, common::ColorF{0.2f, 0.5f, 0.9f, 1.0f}));
        b->setTransform(common::Affine2D::translation(48, 48));
        doc.root().addOnTop(std::move(b));
    };
    const auto upperOf = [](core::Document& doc) {
        return doc.root().child(1).as<core::VectorLayer>();
    };
    const auto lowerOf = [](core::Document& doc) {
        return doc.root().child(0).as<core::VectorLayer>();
    };

    SUBCASE("the baseline pair really does combine") {
        core::Document doc(64, 64);
        shapeDoc(doc);
        CHECK(render::mergeDownVector(*upperOf(doc), *lowerOf(doc)).has_value());
    }
    SUBCASE("two fills cannot ride one object") {
        core::Document doc(64, 64);
        shapeDoc(doc);
        core::vec::Object o = *upperOf(doc)->object();
        o.fill = core::vec::SolidPaint{common::ColorF{1.0f, 1.0f, 0.0f, 1.0f}};
        upperOf(doc)->setObject(std::move(o));
        CHECK_FALSE(render::mergeDownVector(*upperOf(doc), *lowerOf(doc)).has_value());
    }
    SUBCASE("two opacities cannot ride one layer") {
        core::Document doc(64, 64);
        shapeDoc(doc);
        upperOf(doc)->setOpacity(0.5f);
        CHECK_FALSE(render::mergeDownVector(*upperOf(doc), *lowerOf(doc)).has_value());
    }
    SUBCASE("a mask is per layer, so two of them cannot") {
        core::Document doc(64, 64);
        shapeDoc(doc);
        upperOf(doc)->setMask(leftCutMask(64, 64, 32));
        CHECK_FALSE(render::mergeDownVector(*upperOf(doc), *lowerOf(doc)).has_value());
    }
    SUBCASE("overlapping shapes: concatenated contours are not what `over` drew") {
        core::Document doc(64, 64);
        shapeDoc(doc);
        // Now sitting on top of the lower shape instead of well clear of it.
        upperOf(doc)->setTransform(common::Affine2D::translation(20, 20));
        CHECK_FALSE(render::mergeDownVector(*upperOf(doc), *lowerOf(doc)).has_value());
    }
    SUBCASE("a gradient is anchored to its own object space, so it cannot be rebased") {
        core::Document doc(64, 64);
        shapeDoc(doc);
        lowerOf(doc)->setObject(gradientRect(12.0, 12.0));
        upperOf(doc)->setObject(gradientRect(12.0, 12.0));  // same paint, but the shapes sit apart
        CHECK_FALSE(render::mergeDownVector(*upperOf(doc), *lowerOf(doc)).has_value());
    }
}

// ---------------------------------------------------------------------------------------------
// gradient + shape, gradient + raster, text + raster, group + raster -- the pixel route
// ---------------------------------------------------------------------------------------------
TEST_CASE("Merge Down: a gradient layer over a shape rasterizes both, picture unchanged") {
    core::Document doc(64, 64);
    auto lowerPtr = doc.makeVector("shape");
    lowerPtr->setObject(solidRect(30.0, 20.0, common::ColorF{0.1f, 0.7f, 0.2f, 1.0f}));
    lowerPtr->setTransform(common::Affine2D::translation(32, 44));
    const core::LayerId lowerId = lowerPtr->id();
    const core::VectorLayer* lower = lowerPtr.get();
    doc.root().addOnTop(std::move(lowerPtr));

    auto upperPtr = doc.makeVector("gradient");
    upperPtr->setObject(gradientRect(64.0, 24.0));
    upperPtr->setTransform(common::Affine2D::translation(32, 14));
    upperPtr->setOpacity(0.75f);
    const core::LayerId upperId = upperPtr->id();
    const core::VectorLayer* upper = upperPtr.get();
    doc.root().addOnTop(std::move(upperPtr));

    const common::Image before = flatten(doc);
    // Different paints AND different opacities: one object cannot hold the pair, so the vector
    // route declines and the pixel route takes over rather than dropping the styling silently.
    CHECK_FALSE(render::mergeDownVector(*upper, *lower).has_value());

    render::MergeDownBake bake = render::mergeDownBaked(*upper, *lower, 64, 64, kFilter, true);
    REQUIRE(bake.status == render::MergeDownBake::Status::Ok);
    CHECK(bake.image.rgba == before.rgba);  // the bake IS what the canvas showed

    const core::LayerId mergedId =
        landMerge(doc, upperId, lowerId, bakedRaster(doc, std::move(bake.image), "shape"));
    REQUIRE(doc.root().childCount() == 1);
    CHECK(doc.root().child(0).kind() == core::LayerKind::Raster);
    CHECK(doc.root().child(0).id() == mergedId);
    CHECK(flatten(doc).rgba == before.rgba);

    doc.commands().undo();
    REQUIRE(doc.root().childCount() == 2);
    CHECK(doc.root().child(0).kind() == core::LayerKind::Vector);
    CHECK(doc.root().child(0).id() == lowerId);
    CHECK(doc.root().child(1).id() == upperId);
    CHECK(flatten(doc).rgba == before.rgba);
    doc.commands().redo();
    CHECK(doc.root().childCount() == 1);
    CHECK(flatten(doc).rgba == before.rgba);
}

TEST_CASE("Merge Down: a gradient layer bakes into the raster below it") {
    core::Document doc(48, 48);
    auto lowerPtr = doc.makeRaster("bg", 48, 48);
    lowerPtr->image().fill(common::Color8{40, 40, 40, 255});
    const core::LayerId lowerId = lowerPtr->id();
    const core::RasterLayer* lower = lowerPtr.get();
    doc.root().addOnTop(std::move(lowerPtr));

    auto upperPtr = doc.makeVector("gradient");
    upperPtr->setObject(gradientRect(40.0, 40.0));
    upperPtr->setTransform(common::Affine2D::translation(24, 24));
    const core::LayerId upperId = upperPtr->id();
    const core::VectorLayer* upper = upperPtr.get();
    doc.root().addOnTop(std::move(upperPtr));

    const common::Image before = flatten(doc);
    // The pre-S36 entry point has no pixels for a vector layer -- that refusal is exactly what the
    // baked route replaces.
    CHECK_FALSE(render::mergeDown(*upper, *lower).has_value());

    render::MergeDownBake bake = render::mergeDownBaked(*upper, *lower, 48, 48, kFilter, true);
    REQUIRE(bake.status == render::MergeDownBake::Status::Ok);
    CHECK(bake.image.rgba == before.rgba);

    landMerge(doc, upperId, lowerId, bakedRaster(doc, std::move(bake.image), "bg"));
    REQUIRE(doc.root().childCount() == 1);
    CHECK(flatten(doc).rgba == before.rgba);
    doc.commands().undo();
    REQUIRE(doc.root().childCount() == 2);
    CHECK(doc.root().child(1).kind() == core::LayerKind::Vector);
    CHECK(flatten(doc).rgba == before.rgba);
}

TEST_CASE("Merge Down: a text layer bakes into the raster below it") {
    core::Document doc(32, 32);
    auto lowerPtr = doc.makeRaster("bg", 32, 32);
    lowerPtr->image().fill(common::Color8{20, 60, 120, 255});
    const core::LayerId lowerId = lowerPtr->id();
    const core::RasterLayer* lower = lowerPtr.get();
    doc.root().addOnTop(std::move(lowerPtr));

    // A text layer's pixels are a renderer-filled CACHE (docs/type-tool.md §5.4); the compositor
    // only ever reads that cache, so this test needs no font stack -- it fills the cache directly,
    // exactly as MainWindow::ensureTextCaches would before the merge.
    auto textPtr = doc.makeText("caption", "hi");
    common::Image glyphs(8, 8);
    glyphs.fill(common::Color8{255, 230, 0, 200});
    textPtr->setCachedImage(std::move(glyphs), common::Affine2D::identity());
    textPtr->setTransform(common::Affine2D::translation(10, 12));
    textPtr->setOpacity(0.8f);
    const core::LayerId upperId = textPtr->id();
    const core::TextLayer* upper = textPtr.get();
    doc.root().addOnTop(std::move(textPtr));

    const common::Image before = flatten(doc);
    CHECK_FALSE(render::mergeDown(*upper, *lower).has_value());  // no raster of its own

    render::MergeDownBake bake = render::mergeDownBaked(*upper, *lower, 32, 32, kFilter, true);
    REQUIRE(bake.status == render::MergeDownBake::Status::Ok);
    CHECK(bake.image.rgba == before.rgba);

    landMerge(doc, upperId, lowerId, bakedRaster(doc, std::move(bake.image), "bg"));
    REQUIRE(doc.root().childCount() == 1);
    CHECK(doc.root().child(0).kind() == core::LayerKind::Raster);
    CHECK(flatten(doc).rgba == before.rgba);
    doc.commands().undo();
    REQUIRE(doc.root().childCount() == 2);
    CHECK(doc.root().child(1).kind() == core::LayerKind::Text);
    CHECK(flatten(doc).rgba == before.rgba);
}

TEST_CASE("Merge Down: a group above a raster flattens into it") {
    core::Document doc(32, 32);
    auto bgPtr = doc.makeRaster("bg", 32, 32);
    bgPtr->image().fill(common::Color8{10, 10, 10, 255});
    const core::LayerId lowerId = bgPtr->id();
    const core::RasterLayer* lower = bgPtr.get();
    doc.root().addOnTop(std::move(bgPtr));

    auto groupPtr = doc.makeGroup("g");
    auto a = doc.makeRaster("a", 16, 16);
    a->image().fill(common::Color8{200, 0, 0, 255});
    auto b = doc.makeRaster("b", 16, 16);
    b->image().fill(common::Color8{0, 0, 200, 128});
    b->setTransform(common::Affine2D::translation(8, 8));
    groupPtr->addOnTop(std::move(a));
    groupPtr->addOnTop(std::move(b));
    groupPtr->setOpacity(0.6f);
    const core::LayerId upperId = groupPtr->id();
    const core::GroupLayer* upper = groupPtr.get();
    doc.root().addOnTop(std::move(groupPtr));

    const common::Image before = flatten(doc);
    CHECK_FALSE(render::mergeDown(*upper, *lower).has_value());  // a group has no raster

    render::MergeDownBake bake = render::mergeDownBaked(*upper, *lower, 32, 32, kFilter, true);
    REQUIRE(bake.status == render::MergeDownBake::Status::Ok);
    CHECK(bake.image.rgba == before.rgba);

    landMerge(doc, upperId, lowerId, bakedRaster(doc, std::move(bake.image), "bg"));
    REQUIRE(doc.root().childCount() == 1);
    CHECK(flatten(doc).rgba == before.rgba);
    doc.commands().undo();  // the whole subtree comes back with one undo
    REQUIRE(doc.root().childCount() == 2);
    const auto* back = doc.root().child(1).as<core::GroupLayer>();
    REQUIRE(back != nullptr);
    CHECK(back->childCount() == 2);
    CHECK(flatten(doc).rgba == before.rgba);
}

// ---------------------------------------------------------------------------------------------
// Masks and per-layer transforms must fold exactly as the compositor folds them
// ---------------------------------------------------------------------------------------------
TEST_CASE("Merge Down bakes both layers' masks and transforms, not just their pixels") {
    core::Document doc(32, 32);
    auto lowerPtr = doc.makeRaster("bg", 32, 32);
    lowerPtr->image().fill(common::Color8{200, 200, 200, 255});
    lowerPtr->setMask(leftCutMask(32, 32, 8));  // the sheet IS the source grid for a raster
    const core::LayerId lowerId = lowerPtr->id();
    const core::RasterLayer* lower = lowerPtr.get();
    doc.root().addOnTop(std::move(lowerPtr));

    common::Image srcPixels(16, 16);
    srcPixels.fill(common::Color8{0, 160, 0, 255});
    auto upperPtr = doc.makeMagic("placed", std::move(srcPixels));
    upperPtr->setMask(leftCutMask(16, 16, 4));
    upperPtr->setTransform(common::Affine2D::translation(6, 6) *
                           common::Affine2D::rotation(0.35));
    const core::LayerId upperId = upperPtr->id();
    const core::MagicLayer* upper = upperPtr.get();
    doc.root().addOnTop(std::move(upperPtr));

    const common::Image before = flatten(doc);
    // The masks really are doing something (otherwise this test proves nothing).
    CHECK(before.rgba[(4u * 32 + 2) * 4 + 3] == 0);

    render::MergeDownBake bake = render::mergeDownBaked(*upper, *lower, 32, 32, kFilter, true);
    REQUIRE(bake.status == render::MergeDownBake::Status::Ok);
    CHECK(bake.image.rgba == before.rgba);

    landMerge(doc, upperId, lowerId, bakedRaster(doc, std::move(bake.image), "bg"));
    REQUIRE(doc.root().childCount() == 1);
    // Both masks are IN the pixels now; carrying either across would apply it twice.
    CHECK(doc.root().child(0).mask() == nullptr);
    CHECK(flatten(doc).rgba == before.rgba);
}

// ---------------------------------------------------------------------------------------------
// An adjustment merged DOWN bakes its grade into the layer below
// ---------------------------------------------------------------------------------------------
TEST_CASE("Merge Down: an adjustment bakes its grade into the layer below") {
    core::Document doc(16, 16);
    auto lowerPtr = doc.makeRaster("bg", 16, 16);
    lowerPtr->image().fill(common::Color8{60, 120, 200, 255});
    const core::LayerId lowerId = lowerPtr->id();
    core::RasterLayer* lower = lowerPtr.get();
    doc.root().addOnTop(std::move(lowerPtr));

    auto adjPtr = doc.makeAdjustment("Invert", core::AdjustmentKind::Invert);
    const core::LayerId adjId = adjPtr->id();
    const core::AdjustmentLayer* adj = adjPtr.get();
    doc.root().addOnTop(std::move(adjPtr));

    const common::Image before = flatten(doc);
    CHECK(before.rgba[0] == 195);  // 255 - 60: the grade is live

    // With exactly one layer under it, the adjustment's live scope IS that layer, so baking it
    // down is not merely "what the user meant" -- it is byte-identical.
    common::Image graded =
        render::applyAdjustmentToImage(*adj, lower->image(), lower->transform(), 16, 16);
    CHECK(graded.rgba == before.rgba);

    auto cmd = std::make_unique<core::CompositeCommand>("Merge Down");
    cmd->add(std::make_unique<core::SetLayerPixelsCommand>(lowerId, std::move(graded)));
    cmd->add(std::make_unique<core::RemoveLayerCommand>(adjId));
    doc.commands().push(std::move(cmd));

    REQUIRE(doc.root().childCount() == 1);
    CHECK(flatten(doc).rgba == before.rgba);
    doc.commands().undo();
    REQUIRE(doc.root().childCount() == 2);
    CHECK(doc.root().child(1).kind() == core::LayerKind::Adjustment);
    CHECK(flatten(doc).rgba == before.rgba);
    doc.commands().redo();
    CHECK(doc.root().childCount() == 1);
    CHECK(flatten(doc).rgba == before.rgba);
}

TEST_CASE("Merge Down: a clipping adjustment grades only where the layer below has coverage") {
    core::Document doc(16, 16);
    auto lowerPtr = doc.makeRaster("half", 16, 16);
    for (std::uint32_t y = 0; y < 16; ++y)
        for (std::uint32_t x = 0; x < 16; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * 16 + x) * 4;
            lowerPtr->image().rgba[p] = 60;
            lowerPtr->image().rgba[p + 1] = 120;
            lowerPtr->image().rgba[p + 2] = 200;
            lowerPtr->image().rgba[p + 3] = x < 8 ? 255 : 0;
        }
    const core::RasterLayer* lower = lowerPtr.get();
    doc.root().addOnTop(std::move(lowerPtr));

    auto adjPtr = doc.makeAdjustment("Invert", core::AdjustmentKind::Invert);
    adjPtr->setClipToBelow(true);
    const core::LayerId adjId = adjPtr->id();
    const core::AdjustmentLayer* adj = adjPtr.get();
    doc.root().addOnTop(std::move(adjPtr));

    const common::Image before = flatten(doc);
    common::Image graded =
        render::applyAdjustmentToImage(*adj, lower->image(), lower->transform(), 16, 16);
    CHECK(graded.rgba[0] == 195);        // inside the clip base: graded
    CHECK(graded.rgba[8 * 4 + 0] == 60);  // outside it: the raw colour is left alone

    // The comparison is on the COMPOSITE, not the raster: colour under alpha 0 is invisible, and
    // grading a layer's own pixels deliberately leaves it where it was rather than clearing it.
    const core::LayerId lowerId = doc.root().child(0).id();
    auto cmd = std::make_unique<core::CompositeCommand>("Merge Down");
    cmd->add(std::make_unique<core::SetLayerPixelsCommand>(lowerId, std::move(graded)));
    cmd->add(std::make_unique<core::RemoveLayerCommand>(adjId));
    doc.commands().push(std::move(cmd));
    REQUIRE(doc.root().childCount() == 1);
    CHECK(flatten(doc).rgba == before.rgba);
    doc.commands().undo();
    REQUIRE(doc.root().childCount() == 2);
    CHECK(flatten(doc).rgba == before.rgba);
}

// ---------------------------------------------------------------------------------------------
// The refusals -- the ones that are a genuine "no", not a missing feature
// ---------------------------------------------------------------------------------------------
TEST_CASE("Merge Down refuses a blend mode it cannot bake honestly") {
    core::Document doc(16, 16);
    // A lower layer that is NOT opaque: the upper's Multiply blends against whatever is under the
    // pair, and a bake cannot see that.
    auto lowerPtr = doc.makeRaster("sparse", 16, 16);
    lowerPtr->image().fill(common::Color8{180, 180, 180, 0});
    const core::RasterLayer* lower = lowerPtr.get();
    doc.root().addOnTop(std::move(lowerPtr));

    auto upperPtr = doc.makeRaster("multiply", 16, 16);
    upperPtr->image().fill(common::Color8{200, 200, 200, 255});
    upperPtr->setBlendMode(BlendMode::Multiply);
    const core::RasterLayer* upper = upperPtr.get();
    doc.root().addOnTop(std::move(upperPtr));

    CHECK(render::mergeDownBaked(*upper, *lower, 16, 16, kFilter, /*emptyBackdrop=*/false).status ==
          render::MergeDownBake::Status::UpperBlendUnbaked);
    // ... but with nothing at all below the pair, an empty backdrop is no backdrop: the blend has
    // nothing it could have seen, so the same bake is exact.
    CHECK(render::mergeDownBaked(*upper, *lower, 16, 16, kFilter, /*emptyBackdrop=*/true).status ==
          render::MergeDownBake::Status::Ok);

    // An opaque lower layer carries the blend honestly whatever is beneath the pair.
    core::Document opaque(16, 16);
    auto solid = opaque.makeRaster("solid", 16, 16);
    solid->image().fill(common::Color8{180, 180, 180, 255});
    const core::RasterLayer* base = solid.get();
    opaque.root().addOnTop(std::move(solid));
    auto mul = opaque.makeRaster("multiply", 16, 16);
    mul->image().fill(common::Color8{200, 200, 200, 255});
    mul->setBlendMode(BlendMode::Multiply);
    const core::RasterLayer* mulp = mul.get();
    opaque.root().addOnTop(std::move(mul));
    const common::Image liveOpaque = flatten(opaque);
    render::MergeDownBake ok =
        render::mergeDownBaked(*mulp, *base, 16, 16, kFilter, /*emptyBackdrop=*/false);
    REQUIRE(ok.status == render::MergeDownBake::Status::Ok);
    CHECK(ok.image.rgba == liveOpaque.rgba);
}

TEST_CASE("Merge Down: the raster fast path still declines what has no raster of its own") {
    core::Document doc(8, 8);
    auto lowerPtr = doc.makeRaster("bg", 8, 8);
    lowerPtr->image().fill(common::Color8{10, 20, 30, 255});
    const core::RasterLayer* lower = lowerPtr.get();
    doc.root().addOnTop(std::move(lowerPtr));

    const auto group = doc.makeGroup("g");
    const auto vec = doc.makeVector("v");
    const auto adj = doc.makeAdjustment("Invert", core::AdjustmentKind::Invert);
    CHECK_FALSE(render::mergeDown(*group, *lower).has_value());
    CHECK_FALSE(render::mergeDown(*vec, *lower).has_value());
    CHECK_FALSE(render::mergeDown(*adj, *lower).has_value());
}

TEST_CASE("Merge Down: the baked route consumes both layers' opacity and blend, so nothing rides") {
    // The merged raster must be plain (opacity 1, Normal, unclipped): carrying the lower layer's
    // opacity across would scale the UPPER layer's contribution too, which is not what the canvas
    // showed. Pinned as arithmetic: bake == the live composite of a faded pair.
    core::Document doc(16, 16);
    auto lowerPtr = doc.makeRaster("faded", 16, 16);
    lowerPtr->image().fill(common::Color8{240, 40, 40, 255});
    lowerPtr->setOpacity(0.4f);
    const core::RasterLayer* lower = lowerPtr.get();
    doc.root().addOnTop(std::move(lowerPtr));

    auto upperPtr = doc.makeRaster("over", 8, 8);
    upperPtr->image().fill(common::Color8{40, 40, 240, 200});
    upperPtr->setTransform(common::Affine2D::translation(4, 4));
    upperPtr->setOpacity(0.7f);
    const core::RasterLayer* upper = upperPtr.get();
    doc.root().addOnTop(std::move(upperPtr));

    const common::Image before = flatten(doc);
    render::MergeDownBake bake = render::mergeDownBaked(*upper, *lower, 16, 16, kFilter, true);
    REQUIRE(bake.status == render::MergeDownBake::Status::Ok);
    CHECK(bake.image.rgba == before.rgba);
}
