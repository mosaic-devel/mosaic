#include <doctest/doctest.h>

#include <cmath>
#include <numbers>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "common/image.hpp"
#include "core/blend_mode.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/selection.hpp"
#include "render/blend.hpp"
#include "render/compositor.hpp"

using namespace mosaic;
using core::BlendMode;

namespace {

// Composite a document on the CPU reference path, no checkerboard (preserving real alpha). The
// integration cases below assert exact pixel values, so they pin the deterministic CPU backend;
// the GPU path is checked separately (against the CPU output) at the end of this file.
common::Image flatten(const core::Document& doc) {
    const render::CompositeResult r = render::composite(doc, {}, render::Backend::Cpu);
    REQUIRE(r.ok);
    REQUIRE(r.usedBackend == render::Backend::Cpu);
    return r.image;
}

common::Color8 px(const common::Image& img, std::uint32_t x, std::uint32_t y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

float lum(common::ColorF c) { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; }

}  // namespace

// ---------------------------------------------------------------------------------------------
// Blend-mode math (blend.hpp) -- the per-channel formulas, with known reference values.
// ---------------------------------------------------------------------------------------------
TEST_CASE("separable blend channels match their definitions") {
    using render::blendChannel;
    CHECK(blendChannel(BlendMode::Normal, 0.2f, 0.7f) == doctest::Approx(0.7f));
    CHECK(blendChannel(BlendMode::Multiply, 0.5f, 0.5f) == doctest::Approx(0.25f));
    CHECK(blendChannel(BlendMode::Screen, 0.5f, 0.5f) == doctest::Approx(0.75f));
    CHECK(blendChannel(BlendMode::Darken, 0.3f, 0.7f) == doctest::Approx(0.3f));
    CHECK(blendChannel(BlendMode::Lighten, 0.3f, 0.7f) == doctest::Approx(0.7f));
    CHECK(blendChannel(BlendMode::Difference, 0.7f, 0.2f) == doctest::Approx(0.5f));
    CHECK(blendChannel(BlendMode::Exclusion, 0.5f, 0.5f) == doctest::Approx(0.5f));
    CHECK(blendChannel(BlendMode::LinearDodge, 0.6f, 0.6f) == doctest::Approx(1.0f));  // clamped
    CHECK(blendChannel(BlendMode::Subtract, 0.3f, 0.5f) == doctest::Approx(0.0f));     // clamped
    CHECK(blendChannel(BlendMode::Overlay, 0.25f, 0.5f) == doctest::Approx(0.25f));
    CHECK(blendChannel(BlendMode::HardLight, 0.5f, 0.25f) == doctest::Approx(0.25f));
    CHECK(blendChannel(BlendMode::Divide, 0.25f, 0.5f) == doctest::Approx(0.5f));
    CHECK(blendChannel(BlendMode::LinearLight, 0.6f, 0.45f) == doctest::Approx(0.5f));  // b+2s-1
    CHECK(blendChannel(BlendMode::PinLight, 0.8f, 0.25f) == doctest::Approx(0.5f));
}

TEST_CASE("blend channel edge cases avoid division by zero") {
    using render::blendChannel;
    CHECK(blendChannel(BlendMode::ColorBurn, 0.5f, 0.0f) == doctest::Approx(0.0f));
    CHECK(blendChannel(BlendMode::ColorDodge, 0.5f, 1.0f) == doctest::Approx(1.0f));
    CHECK(blendChannel(BlendMode::Divide, 0.5f, 0.0f) == doctest::Approx(1.0f));
    CHECK(blendChannel(BlendMode::ColorBurn, 0.5f, 0.5f) == doctest::Approx(0.0f));
    CHECK(blendChannel(BlendMode::ColorDodge, 0.5f, 0.5f) == doctest::Approx(1.0f));
}

TEST_CASE("non-separable blend modes preserve the expected luminance") {
    using render::compositeOver;
    const common::ColorF backdrop{0.2f, 0.6f, 0.8f, 1.0f};
    const common::ColorF source{0.9f, 0.1f, 0.3f, 1.0f};
    // With both fully opaque, compositeOver returns the pure blend result.
    const common::ColorF color = compositeOver(BlendMode::Color, backdrop, source, 1.0f);
    const common::ColorF lumi = compositeOver(BlendMode::Luminosity, backdrop, source, 1.0f);
    CHECK(lum(color) == doctest::Approx(lum(backdrop)).epsilon(0.01));  // Color keeps backdrop L
    CHECK(lum(lumi) == doctest::Approx(lum(source)).epsilon(0.01));     // Luminosity takes source L
    CHECK_FALSE(render::isSeparable(BlendMode::Hue));
    CHECK(render::isSeparable(BlendMode::Multiply));
}

// ---------------------------------------------------------------------------------------------
// compositeOver -- the source-over-with-blend alpha math.
// ---------------------------------------------------------------------------------------------
TEST_CASE("compositeOver alpha and opacity behave like source-over") {
    using render::compositeOver;
    const common::ColorF b{0.2f, 0.2f, 0.2f, 1.0f};

    // A fully transparent source leaves the backdrop untouched.
    CHECK(compositeOver(BlendMode::Normal, b, {1, 1, 1, 0.0f}, 1.0f) == b);

    // An opaque Normal source fully replaces an opaque backdrop.
    const common::ColorF s{0.8f, 0.1f, 0.1f, 1.0f};
    CHECK(compositeOver(BlendMode::Normal, b, s, 1.0f) == s);

    // Half opacity blends halfway.
    const common::ColorF half = compositeOver(BlendMode::Normal, b, s, 0.5f);
    CHECK(half.r == doctest::Approx(0.5f));
    CHECK(half.a == doctest::Approx(1.0f));

    // Over a transparent backdrop, alpha composes and straight color is the source's.
    const common::ColorF over = compositeOver(BlendMode::Normal, {0, 0, 0, 0}, {0.4f, 0.5f, 0.6f, 0.5f}, 1.0f);
    CHECK(over.a == doctest::Approx(0.5f));
    CHECK(over.r == doctest::Approx(0.4f));

    // Two half-alpha layers: ao = as + ab(1-as) = 0.5 + 0.5*0.5 = 0.75.
    const common::ColorF acc = compositeOver(BlendMode::Normal, {0, 0, 0, 0.5f}, {1, 1, 1, 0.5f}, 1.0f);
    CHECK(acc.a == doctest::Approx(0.75f));
}

// ---------------------------------------------------------------------------------------------
// Compositor integration -- the layer-tree walk.
// ---------------------------------------------------------------------------------------------
TEST_CASE("a single opaque raster composites to its own pixels") {
    core::Document doc(2, 2);
    auto layer = doc.makeRaster("solid");
    layer->image().fill({100, 150, 200, 255});
    doc.root().addOnTop(std::move(layer));

    const common::Image out = flatten(doc);
    CHECK(out.width == 2);
    CHECK(out.height == 2);
    CHECK(px(out, 0, 0) == common::Color8{100, 150, 200, 255});
    CHECK(px(out, 1, 1) == common::Color8{100, 150, 200, 255});
}

TEST_CASE("an empty document composites to transparency") {
    core::Document doc(2, 2);
    const common::Image out = flatten(doc);
    CHECK(px(out, 0, 0) == common::Color8{0, 0, 0, 0});
}

TEST_CASE("Normal stacking: the top opaque layer wins") {
    core::Document doc(1, 1);
    auto bottom = doc.makeRaster("b");
    bottom->image().fill({0, 0, 255, 255});
    doc.root().addOnTop(std::move(bottom));
    auto top = doc.makeRaster("t");
    top->image().fill({255, 0, 0, 255});
    doc.root().addOnTop(std::move(top));

    CHECK(px(flatten(doc), 0, 0) == common::Color8{255, 0, 0, 255});
}

TEST_CASE("Multiply by white is identity; the product is exact") {
    core::Document doc(1, 1);
    auto bottom = doc.makeRaster("b");
    bottom->image().fill({200, 100, 50, 255});
    doc.root().addOnTop(std::move(bottom));
    auto top = doc.makeRaster("t");
    top->image().fill({255, 255, 255, 255});
    top->setBlendMode(BlendMode::Multiply);
    doc.root().addOnTop(std::move(top));

    CHECK(px(flatten(doc), 0, 0) == common::Color8{200, 100, 50, 255});  // x white == unchanged
}

TEST_CASE("layer opacity blends toward the backdrop") {
    core::Document doc(1, 1);
    auto bottom = doc.makeRaster("b");
    bottom->image().fill({0, 0, 0, 255});
    doc.root().addOnTop(std::move(bottom));
    auto top = doc.makeRaster("t");
    top->image().fill({255, 255, 255, 255});
    top->setOpacity(0.5f);
    doc.root().addOnTop(std::move(top));

    CHECK(px(flatten(doc), 0, 0) == common::Color8{128, 128, 128, 255});
}

TEST_CASE("an invisible layer is skipped") {
    core::Document doc(1, 1);
    auto bottom = doc.makeRaster("b");
    bottom->image().fill({0, 0, 0, 255});
    doc.root().addOnTop(std::move(bottom));
    auto top = doc.makeRaster("t");
    top->image().fill({255, 255, 255, 255});
    top->setVisible(false);
    doc.root().addOnTop(std::move(top));

    CHECK(px(flatten(doc), 0, 0) == common::Color8{0, 0, 0, 255});
}

TEST_CASE("a raster mask gates the layer's alpha") {
    core::Document doc(2, 1);
    auto bottom = doc.makeRaster("b");
    bottom->image().fill({0, 0, 0, 255});
    doc.root().addOnTop(std::move(bottom));

    auto top = doc.makeRaster("t", 2, 1);
    top->image().fill({255, 255, 255, 255});
    core::RasterMask mask(2, 1, 255);
    mask.coverage[1] = 0;  // hide the right pixel
    top->setMask(std::move(mask));
    doc.root().addOnTop(std::move(top));

    const common::Image out = flatten(doc);
    CHECK(px(out, 0, 0) == common::Color8{255, 255, 255, 255});  // masked in
    CHECK(px(out, 1, 0) == common::Color8{0, 0, 0, 255});        // masked out -> base shows
}

TEST_CASE("clip-to-below restricts a layer to the base's alpha") {
    core::Document doc(2, 1);
    // Base covers only the left pixel.
    auto base = doc.makeRaster("base", 2, 1);
    base->image().rgba = {0, 0, 255, 255, /*right*/ 0, 0, 0, 0};
    doc.root().addOnTop(std::move(base));
    // A full red layer clipped to the base.
    auto clipped = doc.makeRaster("clip", 2, 1);
    clipped->image().fill({255, 0, 0, 255});
    clipped->setClipToBelow(true);
    doc.root().addOnTop(std::move(clipped));

    const common::Image out = flatten(doc);
    CHECK(px(out, 0, 0) == common::Color8{255, 0, 0, 255});  // clipped onto the base
    CHECK(px(out, 1, 0) == common::Color8{0, 0, 0, 0});      // outside the base -> nothing
}

TEST_CASE("a group's opacity applies to the whole group") {
    core::Document doc(1, 1);
    auto base = doc.makeRaster("base");
    base->image().fill({0, 0, 0, 255});
    doc.root().addOnTop(std::move(base));

    auto group = doc.makeGroup("grp");
    group->setOpacity(0.5f);
    auto inner = doc.makeRaster("inner");
    inner->image().fill({255, 255, 255, 255});
    group->addOnTop(std::move(inner));
    doc.root().addOnTop(std::move(group));

    CHECK(px(flatten(doc), 0, 0) == common::Color8{128, 128, 128, 255});
}

TEST_CASE("a full-canvas group placed 1:1 composites the same as its children do bare") {
    // A group whose local extent already IS the target window, placed 1:1, is not a resample: the
    // placed buffer is the composited buffer. renderLayerRaw MOVES it rather than copying, which
    // at 39.8 MP saves reading and writing 637 MB to hand a buffer to itself -- and makes `local`
    // a moved-from object for the rest of the function.
    //
    // So this pins the two things a move can break that a copy cannot: that the placed result is
    // the children's composite (not an empty or garbage buffer), and that the mask fold applied to
    // the local buffer BEFORE the move still reaches the output.
    const auto build = [](bool wrapInGroup, bool masked) {
        core::Document doc(24, 16);
        auto base = doc.makeRaster("base", 24, 16);
        for (std::uint32_t y = 0; y < 16; ++y)
            for (std::uint32_t x = 0; x < 24; ++x) {
                const std::size_t p = (static_cast<std::size_t>(y) * 24 + x) * 4;
                base->image().rgba[p] = static_cast<std::uint8_t>(x * 10);
                base->image().rgba[p + 1] = static_cast<std::uint8_t>(y * 15);
                base->image().rgba[p + 2] = 200;
                base->image().rgba[p + 3] = 255;
            }
        core::RasterMask mask; // opaque left half, transparent right half
        mask.width = 24;
        mask.height = 16;
        mask.enabled = true;
        mask.linked = true;
        mask.coverage.assign(static_cast<std::size_t>(24) * 16, std::uint8_t{0});
        for (std::uint32_t y = 0; y < 16; ++y)
            for (std::uint32_t x = 0; x < 12; ++x)
                mask.coverage[static_cast<std::size_t>(y) * 24 + x] = 255;
        if (!wrapInGroup) {
            if (masked)
                base->setMask(mask);
            doc.root().addOnTop(std::move(base));
            return flatten(doc);
        }
        auto group = doc.makeGroup("grp");
        group->addOnTop(std::move(base));
        if (masked)
            group->setMask(mask);
        doc.root().addOnTop(std::move(group));
        return flatten(doc);
    };
    // Unmasked: the group is pure packaging, so wrapping must change nothing at all.
    CHECK(build(/*wrapInGroup=*/true, /*masked=*/false).rgba ==
          build(/*wrapInGroup=*/false, /*masked=*/false).rgba);
    // Masked: the fold happens on the local buffer, before the move. A move that lost it would
    // leave the right half painted.
    const common::Image maskedGroup = build(/*wrapInGroup=*/true, /*masked=*/true);
    CHECK(maskedGroup.rgba == build(/*wrapInGroup=*/false, /*masked=*/true).rgba);
    CHECK(px(maskedGroup, 3, 8).a == 255); // inside the mask
    CHECK(px(maskedGroup, 20, 8).a == 0);  // outside it

    // ⚠ IDENTITY DOES NOT IMPLY MATCHING SIZES, and the size check is what carries that. A group
    // whose content starts AT THE ORIGIN but covers only part of the canvas gets a local extent of
    // (0, 0, w', h'): the buffer->local map is a translation by zero, i.e. the identity, while the
    // buffer is smaller than the target window. Moving there would hand back a buffer of the wrong
    // size. This is the case that fails if the move is gated on the placement alone.
    core::Document quadrant(24, 16);
    auto small = quadrant.makeRaster("small", 12, 8);
    small->image().fill({255, 0, 0, 255});
    auto grp = quadrant.makeGroup("grp");
    grp->addOnTop(std::move(small));
    quadrant.root().addOnTop(std::move(grp));
    const common::Image out = flatten(quadrant);
    REQUIRE(out.width == 24);
    REQUIRE(out.height == 16);
    CHECK(px(out, 3, 3) == common::Color8{255, 0, 0, 255}); // the group's quadrant is painted
    CHECK(px(out, 20, 12).a == 0);                          // and the rest of the canvas is not
}

TEST_CASE("an adjustment layer scopes to its group, not the layers outside it") {
    // 2x1 canvas. Red base everywhere; a group adds green to the left pixel only and inverts
    // INSIDE the group -> the invert touches the green, never the red base.
    core::Document doc(2, 1);
    auto base = doc.makeRaster("base", 2, 1);
    base->image().fill({255, 0, 0, 255});
    doc.root().addOnTop(std::move(base));

    auto group = doc.makeGroup("grp");
    auto green = doc.makeRaster("green", 2, 1);
    green->image().rgba = {0, 255, 0, 255, /*right*/ 0, 0, 0, 0};
    group->addOnTop(std::move(green));
    group->addOnTop(doc.makeAdjustment("inv", core::AdjustmentKind::Invert));
    doc.root().addOnTop(std::move(group));

    const common::Image out = flatten(doc);
    CHECK(px(out, 0, 0) == common::Color8{255, 0, 255, 255});  // green inverted -> magenta
    CHECK(px(out, 1, 0) == common::Color8{255, 0, 0, 255});    // base red untouched
}

TEST_CASE("a root adjustment scopes globally to everything below it") {
    // Same scene but the invert is at the root, on top -> it inverts the whole composite,
    // so the exposed red base becomes cyan.
    core::Document doc(2, 1);
    auto base = doc.makeRaster("base", 2, 1);
    base->image().fill({255, 0, 0, 255});
    doc.root().addOnTop(std::move(base));
    auto green = doc.makeRaster("green", 2, 1);
    green->image().rgba = {0, 255, 0, 255, 0, 0, 0, 0};
    doc.root().addOnTop(std::move(green));
    doc.root().addOnTop(doc.makeAdjustment("inv", core::AdjustmentKind::Invert));

    const common::Image out = flatten(doc);
    CHECK(px(out, 0, 0) == common::Color8{255, 0, 255, 255});  // green inverted -> magenta
    CHECK(px(out, 1, 0) == common::Color8{0, 255, 255, 255});  // red base inverted -> cyan
}

TEST_CASE("a masked adjustment's effect rides the layer transform (S31 mask/transform sync)") {
    // 5x1 red canvas; a root Invert reveals ONLY document column 2 through its mask. Moving the
    // adjustment must carry the revealed column with it: the mask tracks the layer, so paint,
    // composite and crop agree instead of the mask staying pinned to the document origin. Before
    // the fix the fold ignored the adjustment's own transform (maskDomain was the parent placement),
    // so the invert stayed on column 2 however far the layer slid -- the "mask moved" bug.
    core::Document doc(5, 1);
    auto base = doc.makeRaster("base", 5, 1);
    base->image().fill({255, 0, 0, 255});
    doc.root().addOnTop(std::move(base));

    auto adj = doc.makeAdjustment("inv", core::AdjustmentKind::Invert);
    auto* al = adj.get();
    core::RasterMask m(5, 1, 0);
    m.coverage[2] = 255;  // reveal document column 2 only
    al->setMask(std::move(m));
    doc.root().addOnTop(std::move(adj));

    const common::Color8 cyan{0, 255, 255, 255};  // red inverted
    const common::Color8 red{255, 0, 0, 255};

    // Identity: the invert lands on column 2.
    common::Image out = flatten(doc);
    CHECK(px(out, 2, 0) == cyan);
    CHECK(px(out, 3, 0) == red);

    // Slide the adjustment right by one: the revealed column rides to 3 (was pinned to 2 before).
    al->setTransform(common::Affine2D::translation(1, 0));
    out = flatten(doc);
    CHECK(px(out, 3, 0) == cyan);
    CHECK(px(out, 2, 0) == red);
}

TEST_CASE("crop carries a masked adjustment's revealed region (the mask rides the rebase)") {
    // 6x1 red canvas; a root Invert reveals document column 3. A delete-mode crop keeping [2,6)
    // rebases the adjustment by translate(-2,0); its mask must ride so the inverted pixel that sat
    // at doc column 3 lands at new column 1 (the crop shifted everything left by two). Before the
    // fix the doc-sized mask stretched onto the new 4-wide canvas and the invert landed at the
    // wrong column -- a masked-out region surviving the crop in the wrong place.
    core::Document doc(6, 1);
    auto base = doc.makeRaster("base", 6, 1);
    base->image().fill({255, 0, 0, 255});
    doc.root().addOnTop(std::move(base));

    auto adj = doc.makeAdjustment("inv", core::AdjustmentKind::Invert);
    auto* al = adj.get();
    core::RasterMask m(6, 1, 0);
    m.coverage[3] = 255;  // reveal document column 3 only
    al->setMask(std::move(m));
    doc.root().addOnTop(std::move(adj));

    const common::Color8 cyan{0, 255, 255, 255};
    const common::Color8 red{255, 0, 0, 255};
    CHECK(px(flatten(doc), 3, 0) == cyan);  // pre-crop sanity

    doc.commands().push(render::buildCropCommand(doc, 2, 0, 4, 1, /*deletePixels=*/true));
    CHECK(doc.width() == 4);
    const common::Image out = flatten(doc);
    CHECK(px(out, 1, 0) == cyan);  // doc col 3 -> new col 1, carried by the mask
    CHECK(px(out, 0, 0) == red);
    CHECK(px(out, 2, 0) == red);

    // One undo restores the pre-crop composite (the mask unshifts with the rebase).
    doc.commands().undo();
    CHECK(doc.width() == 6);
    CHECK(px(flatten(doc), 3, 0) == cyan);
}

TEST_CASE("a layer transform places pixels by nearest sampling") {
    core::Document doc(4, 4);
    auto base = doc.makeRaster("base", 4, 4);
    base->image().fill({0, 0, 0, 255});
    doc.root().addOnTop(std::move(base));

    auto dot = doc.makeRaster("dot", 4, 4);
    dot->image().rgba[0] = 255;  // a single white pixel at (0,0)
    dot->image().rgba[1] = 255;
    dot->image().rgba[2] = 255;
    dot->image().rgba[3] = 255;
    dot->setTransform(common::Affine2D::translation(2, 0));  // shift right by 2
    doc.root().addOnTop(std::move(dot));

    const common::Image out = flatten(doc);
    CHECK(px(out, 2, 0) == common::Color8{255, 255, 255, 255});  // moved here
    CHECK(px(out, 0, 0) == common::Color8{0, 0, 0, 255});        // vacated
}

TEST_CASE("transform sampling: fractional translation rounds, scale walks the general path") {
    // Guards the S15 fast paths: a pure translation must reduce to the same integer shift
    // nearest sampling produced per-texel, and the general affine path must agree with the
    // pre-optimization output.
    core::Document doc(4, 4);
    auto dot = doc.makeRaster("dot", 4, 4);
    const std::size_t p = (1 * 4 + 1) * 4; // a single white pixel at (1,1)
    dot->image().rgba[p] = dot->image().rgba[p + 1] = 255;
    dot->image().rgba[p + 2] = dot->image().rgba[p + 3] = 255;
    auto* raster = dot->as<core::RasterLayer>();
    doc.root().addOnTop(std::move(dot));

    raster->setTransform(common::Affine2D::translation(0.6, 0)); // rounds to a 1 px shift
    common::Image out = flatten(doc);
    CHECK(px(out, 2, 1).a == 255);
    CHECK(px(out, 1, 1).a == 0);

    raster->setTransform(common::Affine2D::translation(0.4, 0)); // rounds to no shift
    out = flatten(doc);
    CHECK(px(out, 1, 1).a == 255);
    CHECK(px(out, 2, 1).a == 0);

    raster->setTransform(common::Affine2D::scaling(2, 2)); // general path: the dot covers 2x2
    out = flatten(doc);
    CHECK(px(out, 2, 2).a == 255);
    CHECK(px(out, 3, 3).a == 255);
    CHECK(px(out, 1, 1).a == 0);
}

// ---------------------------------------------------------------------------------------------
// Transform Anti-aliasing (render::ResampleFilter / chooseAutoFilter / cubicKernel).
// ---------------------------------------------------------------------------------------------
namespace {
using render::ResampleFilter;

// Composite on the CPU reference with an explicit resample filter (and live-drag flag).
common::Image flattenF(const core::Document& doc, ResampleFilter f, bool liveDrag = false) {
    render::CompositeOptions opts;
    opts.resampleFilter = f;
    opts.liveDrag = liveDrag;
    const render::CompositeResult r = render::composite(doc, opts, render::Backend::Cpu);
    REQUIRE(r.ok);
    return r.image;
}

// A rotation by `rad` about the point (cx, cy).
common::Affine2D rotateAbout(double rad, double cx, double cy) {
    return common::Affine2D::translation(cx, cy) * common::Affine2D::rotation(rad) *
           common::Affine2D::translation(-cx, -cy);
}

int diff(std::uint8_t a, int b) { return std::abs(static_cast<int>(a) - b); }

// Every implemented kernel (Auto is resolved separately by chooseAutoFilter).
constexpr ResampleFilter kAllFilters[] = {
    ResampleFilter::Nearest,   ResampleFilter::Bilinear, ResampleFilter::Bicubic,
    ResampleFilter::Mitchell,  ResampleFilter::Lanczos2, ResampleFilter::Lanczos3,
    ResampleFilter::Area,      ResampleFilter::Gaussian, ResampleFilter::Supersample};
}  // namespace

TEST_CASE("chooseAutoFilter: lossless grid transforms resolve to Nearest") {
    using A = common::Affine2D;
    // identity, integer translate, integer scale (incl. tripling + flip), exact 90deg rotation.
    CHECK(render::chooseAutoFilter(A::identity(), false) == ResampleFilter::Nearest);
    CHECK(render::chooseAutoFilter(A::translation(3, -5), false) == ResampleFilter::Nearest);
    CHECK(render::chooseAutoFilter(A::scaling(2, 2), false) == ResampleFilter::Nearest);
    CHECK(render::chooseAutoFilter(A::scaling(3, 3), false) == ResampleFilter::Nearest);
    CHECK(render::chooseAutoFilter(A::scaling(-1, 1), false) == ResampleFilter::Nearest); // h-flip
    CHECK(render::chooseAutoFilter(A{0, -1, 0, 1, 0, 0}, false) == ResampleFilter::Nearest); // 90deg
    // ...even mid-drag: lossless stays exact (and the integer-shift fast path handles it).
    CHECK(render::chooseAutoFilter(A::translation(4, 4), true) == ResampleFilter::Nearest);
}

TEST_CASE("chooseAutoFilter: non-lossless transforms bucket by intent") {
    using A = common::Affine2D;
    // A live drag is always the cheap Bilinear, whatever the transform.
    CHECK(render::chooseAutoFilter(A::translation(0.5, 0), true) == ResampleFilter::Bilinear);
    CHECK(render::chooseAutoFilter(A::rotation(0.5), true) == ResampleFilter::Bilinear);
    CHECK(render::chooseAutoFilter(A::scaling(0.3, 0.3), true) == ResampleFilter::Bilinear);
    // Committed: sub-pixel translate / rotate / non-integer enlarge -> sharp Lanczos3.
    CHECK(render::chooseAutoFilter(A::translation(0.5, 0), false) == ResampleFilter::Lanczos3);
    CHECK(render::chooseAutoFilter(A::rotation(0.5), false) == ResampleFilter::Lanczos3);
    CHECK(render::chooseAutoFilter(A::scaling(2.5, 2.5), false) == ResampleFilter::Lanczos3);
    // Committed reduction in EITHER axis -> box Area (no ringing, and a bounded footprint -- a
    // sharp wide kernel on a minified axis takes radius x reduction taps per pixel: a heavy-shrink
    // commit freeze on a big canvas, S60-a).
    CHECK(render::chooseAutoFilter(A::scaling(0.4, 0.4), false) == ResampleFilter::Area);
    CHECK(render::chooseAutoFilter(A::scaling(1.0, 0.2), false) == ResampleFilter::Area); // 1/5 height
    CHECK(render::chooseAutoFilter(A::scaling(0.5, 2.0), false) == ResampleFilter::Area); // anisotropic
}

TEST_CASE("cubicKernel: the BC-spline family reproduces its named members") {
    // Catmull-Rom (0, 1/2): interpolating -> 1 at 0, exactly 0 at the integer taps.
    CHECK(render::cubicKernel(0.0, 0.0, 0.5) == doctest::Approx(1.0));
    CHECK(render::cubicKernel(1.0, 0.0, 0.5) == doctest::Approx(0.0));
    CHECK(render::cubicKernel(2.0, 0.0, 0.5) == doctest::Approx(0.0));
    CHECK(render::cubicKernel(3.0, 0.0, 0.5) == doctest::Approx(0.0)); // past support
    // Even symmetry.
    CHECK(render::cubicKernel(-0.4, 0.0, 0.5) == doctest::Approx(render::cubicKernel(0.4, 0.0, 0.5)));
    // Approximating members: Mitchell (1/3,1/3) = 8/9 at 0; B-spline (1,0) = 2/3 at 0.
    CHECK(render::cubicKernel(0.0, 1.0 / 3.0, 1.0 / 3.0) == doctest::Approx(8.0 / 9.0));
    CHECK(render::cubicKernel(0.0, 1.0, 0.0) == doctest::Approx(2.0 / 3.0));
    // Partition of unity at a fractional offset (the four taps around p=0.3 sum to 1).
    double sum = 0;
    for (double off : {-1.3, -0.3, 0.7, 1.7}) sum += render::cubicKernel(off, 0.0, 0.5);
    CHECK(sum == doctest::Approx(1.0));
}

TEST_CASE("transform AA: every filter preserves a solid interior under rotation") {
    // A full-canvas opaque solid rotated about its centre: the centre stays fully covered with no
    // edge within any kernel's reach, so a correct (normalised, partition-of-unity) kernel returns
    // the exact source colour. This is the DC-preservation invariant for the whole filter set.
    for (ResampleFilter f : kAllFilters) {
        core::Document doc(24, 24);
        auto layer = doc.makeRaster("solid", 24, 24);
        layer->image().fill({200, 120, 40, 255});
        layer->setTransform(rotateAbout(0.65, 12.0, 12.0));
        doc.root().addOnTop(std::move(layer));
        const common::Image out = flattenF(doc, f);
        INFO("filter=" << render::resampleFilterName(f));
        const common::Color8 p = px(out, 12, 12);
        CHECK(p.a == 255);
        CHECK(diff(p.r, 200) <= 1);
        CHECK(diff(p.g, 120) <= 1);
        CHECK(diff(p.b, 40) <= 1);
    }
}

TEST_CASE("transform AA: premultiplied resampling does not bleed transparent colour") {
    // Left half opaque red, right half FULLY TRANSPARENT but coloured green. Upscaling 2x with a
    // smoothing filter interpolates alpha across the boundary; premultiplied filtering keeps the
    // colour pure red (straight-alpha filtering would average toward yellow at the seam).
    for (ResampleFilter f :
         {ResampleFilter::Bilinear, ResampleFilter::Bicubic, ResampleFilter::Lanczos3,
          ResampleFilter::Supersample}) {
        core::Document doc(8, 8);
        auto layer = doc.makeRaster("split", 4, 4);
        for (std::uint32_t y = 0; y < 4; ++y)
            for (std::uint32_t x = 0; x < 4; ++x) {
                const std::size_t p = (static_cast<std::size_t>(y) * 4 + x) * 4;
                if (x < 2) {  // opaque red
                    layer->image().rgba[p] = 255;
                    layer->image().rgba[p + 3] = 255;
                } else {  // transparent green (its RGB must never appear)
                    layer->image().rgba[p + 1] = 255;
                    layer->image().rgba[p + 3] = 0;
                }
            }
        layer->setTransform(common::Affine2D::scaling(2, 2));
        doc.root().addOnTop(std::move(layer));
        const common::Image out = flattenF(doc, f);
        INFO("filter=" << render::resampleFilterName(f));
        for (std::uint32_t y = 0; y < 8; ++y)
            for (std::uint32_t x = 0; x < 8; ++x) {
                const common::Color8 c = px(out, x, y);
                if (c.a > 0) {
                    CHECK(c.g <= 1);  // no green leaked from the transparent half
                    CHECK(c.b <= 1);
                }
            }
    }
}

TEST_CASE("transform AA: Area box-averages the source footprint on minification") {
    // A 4x4 opaque source reduced 0.5x: each output texel is the equal-weight average of its 2x2
    // source block. Block (0,0) = {0,255,255,0} -> ~128; the rest are solid 255.
    core::Document doc(2, 2);
    auto layer = doc.makeRaster("blocks", 4, 4);
    layer->image().fill({255, 255, 255, 255});
    const auto setGray = [&](std::uint32_t x, std::uint32_t y, std::uint8_t v) {
        const std::size_t p = (static_cast<std::size_t>(y) * 4 + x) * 4;
        layer->image().rgba[p] = layer->image().rgba[p + 1] = layer->image().rgba[p + 2] = v;
    };
    setGray(0, 0, 0);
    setGray(1, 1, 0);  // block (0,0) now {0,255,255,0}
    layer->setTransform(common::Affine2D::scaling(0.5, 0.5));
    doc.root().addOnTop(std::move(layer));
    const common::Image out = flattenF(doc, ResampleFilter::Area);
    CHECK(px(out, 0, 0).a == 255);
    CHECK(diff(px(out, 0, 0).r, 128) <= 1);  // box average, not a point sample
    CHECK(px(out, 1, 1) == common::Color8{255, 255, 255, 255});
}

TEST_CASE("transform AA: a whole-pixel translate is byte-identical for every filter") {
    // The integer-shift fast path is lossless, so the chosen filter cannot change a pure
    // whole-pixel Move -- the Move tool's common case stays crisp regardless of the AA setting.
    core::Document doc(8, 8);
    auto base = doc.makeRaster("base", 8, 8);
    base->image().fill({20, 40, 60, 255});
    doc.root().addOnTop(std::move(base));
    auto dot = doc.makeRaster("dot", 8, 8);
    dot->image().rgba[(3 * 8 + 3) * 4 + 0] = 255;
    dot->image().rgba[(3 * 8 + 3) * 4 + 3] = 255;
    dot->setTransform(common::Affine2D::translation(2, 1));  // whole-pixel shift
    doc.root().addOnTop(std::move(dot));
    const common::Image nearest = flattenF(doc, ResampleFilter::Nearest);
    for (ResampleFilter f : kAllFilters) {
        INFO("filter=" << render::resampleFilterName(f));
        CHECK(flattenF(doc, f).rgba == nearest.rgba);
    }
}

TEST_CASE("compositeGroup flattens one subtree, transforms applied") {
    core::Document doc(8, 8);
    auto* group = doc.root().addOnTop(doc.makeGroup("G")).as<core::GroupLayer>();
    auto child = doc.makeRaster("C", 8, 8);
    child->image().rgba[0] = 200; // red pixel at (0,0)
    child->image().rgba[3] = 255;
    child->setTransform(common::Affine2D::translation(2, 0));
    group->addOnTop(std::move(child));
    group->setTransform(common::Affine2D::translation(0, 3));

    const common::Image flat = render::compositeGroup(*group, 8, 8);
    REQUIRE(flat.width == 8);
    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        return flat.rgba[(static_cast<std::size_t>(y) * 8 + x) * 4 + 3];
    };
    CHECK(at(2, 3) == 255); // child transform (+2 x) then group transform (+3 y)
    CHECK(at(0, 0) == 0);
    CHECK(at(2, 0) == 0);
}

TEST_CASE("an unmasked group does not clip a child outside the canvas-aligned local window") {
    // Repro of the "drill into a group, move a child, part of it vanishes" bug. A group that
    // carries its own transform used to composite its children into a canvas-sized window in
    // GROUP-LOCAL space, so a child whose content fell outside [0,W]x[0,H] locally was clipped --
    // even where the group's transform brought it back onto the visible canvas. The local buffer
    // now follows the content's visible extent.
    core::Document doc(8, 8);
    auto* group = doc.root().addOnTop(doc.makeGroup("G")).as<core::GroupLayer>();
    group->setTransform(common::Affine2D::translation(0, 6)); // group-local (x,y) -> doc (x, y+6)

    auto child = doc.makeRaster("C", 8, 8);
    const std::size_t p = (0 * 8 + 2) * 4; // child-local (2,0)
    child->image().rgba[p] = 255;
    child->image().rgba[p + 3] = 255;
    child->setTransform(common::Affine2D::translation(0, -4)); // -> group-local (2,-4): off-window
    group->addOnTop(std::move(child));

    const common::Image out = flatten(doc);
    // group-local (2,-4) -> doc (2, -4+6) = (2,2). Previously clipped to nothing; now present.
    CHECK(px(out, 2, 2) == common::Color8{255, 0, 0, 255});
    CHECK(px(out, 0, 0).a == 0);
}

TEST_CASE("mergeDown bakes the upper layer into the lower's pixel space") {
    core::Document doc(4, 4);
    auto lowerPtr = doc.makeRaster("lower");
    lowerPtr->image().fill({100, 100, 100, 255});
    auto* lower = lowerPtr.get();
    doc.root().addOnTop(std::move(lowerPtr));

    auto upperPtr = doc.makeRaster("upper", 2, 2);
    upperPtr->image().fill({200, 0, 0, 255});
    upperPtr->setTransform(common::Affine2D::translation(2, 2)); // covers (2,2)..(3,3)
    auto* upper = upperPtr.get();
    doc.root().addOnTop(std::move(upperPtr));

    // The merged image must equal the two-layer composite (identity lower transform).
    const common::Image live = flatten(doc);
    const std::optional<common::Image> merged = render::mergeDown(*upper, *lower);
    REQUIRE(merged.has_value());
    CHECK(*merged == live);

    // Upper opacity participates (50% red over grey, straight-alpha lerp).
    upper->setOpacity(0.5f);
    const std::optional<common::Image> faded = render::mergeDown(*upper, *lower);
    REQUIRE(faded.has_value());
    CHECK(*faded == flatten(doc));

    // A group has no pixels to bake.
    const auto group = doc.makeGroup("g");
    CHECK_FALSE(render::mergeDown(*group, *lower).has_value());
}

TEST_CASE("checkerboard flattening fills transparent regions opaquely") {
    core::Document doc(2, 1);  // empty -> fully transparent
    render::CompositeOptions opts;
    opts.checkerboard = true;
    opts.checkerSize = 1;
    opts.checkerLight = {255, 255, 255, 255};
    opts.checkerDark = {0, 0, 0, 255};
    const render::CompositeResult r = render::composite(doc, opts, render::Backend::Cpu);
    REQUIRE(r.ok);
    CHECK(px(r.image, 0, 0) == common::Color8{255, 255, 255, 255});  // light square
    CHECK(px(r.image, 1, 0) == common::Color8{0, 0, 0, 255});        // dark square
}

// ---------------------------------------------------------------------------------------------
// compositeRegion unclamped (the modal live-preview panes' off-canvas emulation): a ROI that
// straddles the canvas edge yields a full-rect buffer whose off-canvas pixels stay transparent,
// while layer content that projects beyond the canvas still renders.
// ---------------------------------------------------------------------------------------------
TEST_CASE("compositeRegion unclamped: off-canvas is transparent and spilled content still renders") {
    core::Document doc(4, 4);
    auto base = doc.makeRaster("base", 4, 4);
    base->image().fill({40, 120, 200, 255});
    doc.root().addOnTop(std::move(base));

    // A 2x2 all-white layer shifted to (3,3): only doc pixel (3,3) is in-canvas; (4,3),(3,4),(4,4)
    // spill off the right/bottom edge.
    auto dot = doc.makeRaster("dot", 2, 2);
    dot->image().fill({255, 255, 255, 255});
    dot->setTransform(common::Affine2D::translation(3, 3));
    doc.root().addOnTop(std::move(dot));

    render::CompositeOptions opts; // no checkerboard: keep real alpha so we can read it back

    // Clamped (the default) refuses a fully off-canvas ROI and clips a straddling one to the canvas.
    const render::CompositeResult clamped =
        render::compositeRegion(doc, {-2.0, -2.0, 3.0, 3.0}, opts, render::Backend::Cpu,
                                /*clampToCanvas=*/true);
    REQUIRE(clamped.ok);
    CHECK(clamped.image.width == 1); // [-2,1) clipped to [0,1)
    CHECK(clamped.image.height == 1);

    // Unclamped over the top-left corner: a 6x6 buffer covering doc x,y in [-2,4). The off-canvas
    // quadrant (buffer < (2,2)) is transparent; the canvas content lands at buffer offset (2,2).
    const render::CompositeResult tl =
        render::compositeRegion(doc, {-2.0, -2.0, 6.0, 6.0}, opts, render::Backend::Cpu,
                                /*clampToCanvas=*/false);
    REQUIRE(tl.ok);
    CHECK(tl.image.width == 6);
    CHECK(tl.image.height == 6);
    CHECK(px(tl.image, 0, 0).a == 0); // off-canvas -> transparent
    CHECK(px(tl.image, 1, 1).a == 0);
    CHECK(px(tl.image, 2, 2) == common::Color8{40, 120, 200, 255}); // doc (0,0): base blue
    CHECK(px(tl.image, 4, 4) == common::Color8{40, 120, 200, 255}); // doc (2,2): base blue (pre-dot)

    // Unclamped over the bottom-right corner: a 4x4 buffer covering doc x,y in [2,6). The dot's
    // spilled pixels at doc (4,3),(3,4),(4,4) render white even though they are outside the canvas;
    // pixels past the layer's own footprint stay transparent.
    const render::CompositeResult br =
        render::compositeRegion(doc, {2.0, 2.0, 4.0, 4.0}, opts, render::Backend::Cpu,
                                /*clampToCanvas=*/false);
    REQUIRE(br.ok);
    CHECK(br.image.width == 4);
    CHECK(br.image.height == 4);
    CHECK(px(br.image, 1, 1) == common::Color8{255, 255, 255, 255}); // doc (3,3): dot over base
    CHECK(px(br.image, 2, 1) == common::Color8{255, 255, 255, 255}); // doc (4,3): spilled off-canvas
    CHECK(px(br.image, 1, 2) == common::Color8{255, 255, 255, 255}); // doc (3,4): spilled off-canvas
    CHECK(px(br.image, 2, 2) == common::Color8{255, 255, 255, 255}); // doc (4,4): spilled off-canvas
    CHECK(px(br.image, 3, 3).a == 0); // doc (5,5): no layer there -> transparent
}

// ---------------------------------------------------------------------------------------------
// buildCropCommand (S16) -- the crop's single undo step: in BOTH modes the cropped composite
// must be the old composite's window byte for byte, and one undo restores everything.
// ---------------------------------------------------------------------------------------------
namespace {

// Count where `after` disagrees with `before`'s (ox,oy) window (0 = byte-exact).
std::size_t windowMismatches(const common::Image& before, const common::Image& after, long ox,
                             long oy) {
    std::size_t mismatches = 0;
    for (std::uint32_t y = 0; y < after.height; ++y)
        for (std::uint32_t x = 0; x < after.width; ++x)
            if (!(px(after, x, y) == px(before, static_cast<std::uint32_t>(x + ox),
                                        static_cast<std::uint32_t>(y + oy))))
                ++mismatches;
    return mismatches;
}

}  // namespace

TEST_CASE("crop command: the cropped composite is the old one's window; undo restores all") {
    for (const bool deletePixels : {false, true}) {
        CAPTURE(deletePixels);
        auto doc = render::makeCompositorDemo();
        // Non-trivial transforms exercise both paths: the red square's translation and the
        // green square's rotation must survive a rebase (bounds-only mode) or bake to the
        // same samples (delete mode).
        doc->root().child(1).setTransform(common::Affine2D::translation(3, 2));
        doc->root().child(2).setTransform(common::Affine2D::translation(42, 42) *
                                          common::Affine2D::rotation(0.3) *
                                          common::Affine2D::translation(-42, -42));
        doc->setSelection(core::Selection::rectangle(64, 64, {6, 6, 20, 20}));

        const common::Image before = flatten(*doc);
        const core::Selection beforeSel = doc->selection();

        const long ox = 8;
        const long oy = 4;
        const std::uint32_t w = 40;
        const std::uint32_t h = 48;
        doc->commands().push(render::buildCropCommand(*doc, ox, oy, w, h, deletePixels));

        CHECK(doc->width() == w);
        CHECK(doc->height() == h);
        const common::Image after = flatten(*doc);
        REQUIRE(after.width == w);
        REQUIRE(after.height == h);
        CHECK(windowMismatches(before, after, ox, oy) == 0);

        // Delete mode flattens a top-level raster's live transform; bounds-only rebases it.
        const common::Affine2D redT = doc->root().child(1).transform();
        if (deletePixels)
            CHECK(redT == common::Affine2D::identity());
        else
            CHECK(redT == common::Affine2D::translation(-5, -2));

        // The selection followed the crop window.
        CHECK(doc->selection().width() == w);
        CHECK(doc->selection().at(0, 2) == 255); // old (8, 6): inside the selected block
        CHECK(doc->selection().at(20, 24) == 0); // old (28, 28): outside it

        // ONE undo restores canvas, pixels, transforms and selection bit-for-bit.
        doc->commands().undo();
        CHECK(doc->width() == 64);
        CHECK(doc->height() == 64);
        CHECK(flatten(*doc).rgba == before.rgba);
        CHECK(doc->selection() == beforeSel);

        doc->commands().redo();
        CHECK(doc->width() == w);
        CHECK(flatten(*doc).rgba == after.rgba);
    }
}

TEST_CASE("crop command: the shift is pushed INTO groups; masked rasters only rebase") {
    auto doc = std::make_unique<core::Document>(32, 32);
    // A masked raster is never baked, even in delete mode (the bake can't fold the mask yet).
    auto masked = doc->makeRaster("masked");
    masked->image().fill({200, 50, 50, 255});
    masked->setMask(core::RasterMask(32, 32, 255));
    const core::LayerId maskedId = doc->root().addOnTop(std::move(masked)).id();
    // An (identity-transform) group: the compositor renders its content through a canvas-sized
    // LOCAL window, so the rebase must land on the CHILD, never translate the group itself —
    // else the window slides off the kept content and the crop margin goes blank.
    auto group = doc->makeGroup("G");
    auto child = doc->makeRaster("C");
    child->image().fill({50, 200, 50, 128});
    child->setTransform(common::Affine2D::translation(2, 1));
    const core::LayerId childId = child->id();
    group->addOnTop(std::move(child));
    const core::LayerId groupId = doc->root().addOnTop(std::move(group)).id();

    const common::Image before = flatten(*doc);
    doc->commands().push(render::buildCropCommand(*doc, 6, 8, 16, 12, /*deletePixels=*/true));

    const common::Image after = flatten(*doc);
    CHECK(windowMismatches(before, after, 6, 8) == 0);

    const core::Layer* m = doc->find(maskedId);
    REQUIRE(m != nullptr);
    CHECK(m->hasMask());
    CHECK(m->transform() == common::Affine2D::translation(-6, -8));
    CHECK(m->as<core::RasterLayer>()->image().width == 32); // pixels untouched
    // The group keeps its transform; the nested child took the (delete-mode) bake — its
    // ancestors are all identity, so the bake is exact there too.
    CHECK(doc->find(groupId)->transform() == common::Affine2D::identity());
    CHECK(doc->find(childId)->transform() == common::Affine2D::identity());
    CHECK(doc->find(childId)->as<core::RasterLayer>()->image().width == 16);

    // One undo restores the whole tree.
    doc->commands().undo();
    CHECK(doc->find(groupId)->transform() == common::Affine2D::identity());
    CHECK(doc->find(childId)->transform() == common::Affine2D::translation(2, 1));
    CHECK(doc->find(childId)->as<core::RasterLayer>()->image().width == 32);
    CHECK(flatten(*doc).rgba == before.rgba);
}

TEST_CASE("crop command: a transformed group keeps its transform; the child takes the shift") {
    auto doc = std::make_unique<core::Document>(32, 32);
    auto group = doc->makeGroup("G");
    auto child = doc->makeRaster("C");
    child->image().fill({50, 200, 50, 255});
    child->setTransform(common::Affine2D::translation(2, 1));
    const core::LayerId childId = child->id();
    group->addOnTop(std::move(child));
    group->setTransform(common::Affine2D::translation(-4, 0));
    const core::LayerId groupId = doc->root().addOnTop(std::move(group)).id();

    // Delete mode must NOT bake under a transformed ancestor (the bake's identity placement
    // only matches the walk when the chain is identity) — the child rebases instead, by the
    // group-conjugated shift (== the plain shift for translations).
    doc->commands().push(render::buildCropCommand(*doc, 6, 8, 16, 12, /*deletePixels=*/true));
    CHECK(doc->find(groupId)->transform() == common::Affine2D::translation(-4, 0));
    CHECK(doc->find(childId)->transform() == common::Affine2D::translation(-4, -7));
    CHECK(doc->find(childId)->as<core::RasterLayer>()->image().width == 32); // not baked
    // (Pixel equality is NOT asserted here: a non-identity group window keeps the
    // compositor's pre-existing local-extent clipping — S60-a owns that fix.)
}

// ---------------------------------------------------------------------------------------------
// buildCropCommand expansion (S16-f) -- a rect beyond the canvas grows it; the old composite
// survives byte-exact at its new offset; the added area takes the fill colour (or stays
// transparent); one undo restores everything.
// ---------------------------------------------------------------------------------------------
namespace {

// after(x+ox, y+oy) must equal before(x, y) for all of `before` (the expansion counterpart of
// windowMismatches, which checks a shrinking window).
std::size_t offsetMismatches(const common::Image& before, const common::Image& after, long ox,
                             long oy) {
    std::size_t mismatches = 0;
    for (std::uint32_t y = 0; y < before.height; ++y)
        for (std::uint32_t x = 0; x < before.width; ++x)
            if (!(px(after, static_cast<std::uint32_t>(x + ox),
                     static_cast<std::uint32_t>(y + oy)) == px(before, x, y)))
                ++mismatches;
    return mismatches;
}

// A 64x64 doc with the standard extendable stack: a doc-sized identity Background (blue) under
// a small red square layer.
std::unique_ptr<core::Document> makeExpansionDemo() {
    auto doc = std::make_unique<core::Document>(64, 64);
    auto bg = doc->makeRaster("Background");
    bg->image().fill({20, 40, 200, 255});
    doc->root().addOnTop(std::move(bg));
    auto red = doc->makeRaster("red", 16, 16);
    red->image().fill({220, 30, 30, 255});
    red->setTransform(common::Affine2D::translation(24, 24));
    doc->root().addOnTop(std::move(red));
    return doc;
}

constexpr common::Color8 kFillOrange{255, 128, 0, 255};

}  // namespace

TEST_CASE("crop expansion: standard Background extends in place, ring takes the fill colour") {
    for (const bool deletePixels : {false, true}) {
        CAPTURE(deletePixels);
        auto doc = makeExpansionDemo();
        doc->setSelection(core::Selection::rectangle(64, 64, {6, 6, 20, 20}));
        const common::Image before = flatten(*doc);
        const std::size_t childCount = doc->root().childCount();

        // 10 left, 6 top, 10 right, 6 bottom added.
        doc->commands().push(render::buildCropCommand(
            *doc, -10, -6, 84, 76, deletePixels, render::CropFill{kFillOrange, "Canvas fill"}));

        CHECK(doc->width() == 84);
        CHECK(doc->height() == 76);
        CHECK(doc->root().childCount() == childCount); // extended in place, no new layer
        const common::Image after = flatten(*doc);
        CHECK(offsetMismatches(before, after, 10, 6) == 0);
        // The ring is the fill colour, to the corners and up to the old footprint's edge.
        CHECK(px(after, 0, 0) == kFillOrange);
        CHECK(px(after, 83, 75) == kFillOrange);
        CHECK(px(after, 9, 6) == kFillOrange);   // last column left of the footprint
        CHECK(px(after, 10, 5) == kFillOrange);  // last row above it
        CHECK(px(after, 74, 70) == kFillOrange); // first pixel past its bottom-right corner
        // The bottom layer grew to the new canvas at identity (pure expansion: union == canvas).
        const auto* bg = doc->root().child(0).as<core::RasterLayer>();
        REQUIRE(bg != nullptr);
        CHECK(bg->image().width == 84);
        CHECK(bg->image().height == 76);
        CHECK(bg->transform() == common::Affine2D::identity());
        // The selection re-anchored with the content.
        CHECK(doc->selection().at(16, 12) == 255); // old (6, 6)
        CHECK(doc->selection().at(4, 4) == 0);     // new ring: never selected

        doc->commands().undo();
        CHECK(doc->width() == 64);
        CHECK(doc->root().child(0).as<core::RasterLayer>()->image().width == 64);
        CHECK(flatten(*doc).rgba == before.rgba);
    }
}

TEST_CASE("crop expansion: no fill leaves the ring transparent and layers untouched in size") {
    auto doc = makeExpansionDemo();
    const common::Image before = flatten(*doc);
    doc->commands().push(
        render::buildCropCommand(*doc, -10, -6, 84, 76, /*deletePixels=*/false));
    const common::Image after = flatten(*doc);
    CHECK(offsetMismatches(before, after, 10, 6) == 0);
    CHECK(px(after, 0, 0).a == 0); // transparent ring
    const auto* bg = doc->root().child(0).as<core::RasterLayer>();
    CHECK(bg->image().width == 64); // rebased, not grown
    CHECK(bg->transform() == common::Affine2D::translation(10, 6));
}

TEST_CASE("crop expansion: non-extendable stack gets a bottom fill layer over the ring only") {
    auto doc = makeExpansionDemo();
    // A transformed bottom layer is NOT extendable in place.
    doc->root().child(0).setTransform(common::Affine2D::translation(1, 0));
    const common::Image before = flatten(*doc);
    const std::size_t childCount = doc->root().childCount();

    doc->commands().push(render::buildCropCommand(
        *doc, -10, -6, 84, 76, /*deletePixels=*/false, render::CropFill{kFillOrange, "Canvas fill"}));

    CHECK(doc->root().childCount() == childCount + 1);
    const auto* fillLayer = doc->root().child(0).as<core::RasterLayer>();
    REQUIRE(fillLayer != nullptr);
    CHECK(fillLayer->name() == "Canvas fill");
    CHECK(px(fillLayer->image(), 0, 0) == kFillOrange);
    CHECK(px(fillLayer->image(), 40, 40).a == 0); // transparent over the old footprint
    const common::Image after = flatten(*doc);
    CHECK(offsetMismatches(before, after, 10, 6) == 0);
    CHECK(px(after, 0, 0) == kFillOrange);

    doc->commands().undo();
    CHECK(doc->root().childCount() == childCount);
    CHECK(flatten(*doc).rgba == before.rgba);
}

TEST_CASE("crop expansion: fill.pixels lands the healed ring; a size mismatch falls back") {
    auto doc = makeExpansionDemo();
    const common::Image before = flatten(*doc);

    // A recognisable per-pixel pattern standing in for the Inpaint mode's healed result.
    common::Image healed(72, 64);
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 72; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * 72 + x) * 4;
            healed.rgba[o] = static_cast<std::uint8_t>(x);
            healed.rgba[o + 1] = static_cast<std::uint8_t>(y);
            healed.rgba[o + 2] = 7;
            healed.rgba[o + 3] = 255;
        }
    render::CropFill fill{{255, 0, 255, 255}, "Canvas fill"};
    fill.pixels = healed;
    doc->commands().push(
        render::buildCropCommand(*doc, 0, 0, 72, 64, /*deletePixels=*/false, fill));
    const common::Image after = flatten(*doc);
    CHECK(offsetMismatches(before, after, 0, 0) == 0); // old footprint untouched
    CHECK(px(after, 64, 10) == common::Color8{64, 10, 7, 255}); // ring = the healed pixels
    CHECK(px(after, 71, 63) == common::Color8{71, 63, 7, 255});
    doc->commands().undo();
    CHECK(flatten(*doc).rgba == before.rgba);

    // A wrongly-sized pixels image must fall back to the constant, never misindex.
    render::CropFill bad{{255, 0, 255, 255}, "Canvas fill"};
    bad.pixels = common::Image(10, 10);
    doc->commands().push(
        render::buildCropCommand(*doc, 0, 0, 72, 64, /*deletePixels=*/false, bad));
    CHECK(px(flatten(*doc), 70, 32) == common::Color8{255, 0, 255, 255});
}

TEST_CASE("crop expansion: combined crop-left + expand-right keeps every pixel in bounds mode") {
    auto doc = makeExpansionDemo();
    const common::Image before = flatten(*doc);

    // Crop 8 off the left, add 24 on the right: x=8, w=80 on a 64-wide canvas.
    doc->commands().push(render::buildCropCommand(
        *doc, 8, 0, 80, 64, /*deletePixels=*/false, render::CropFill{kFillOrange, "Canvas fill"}));

    CHECK(doc->width() == 80);
    const common::Image after = flatten(*doc);
    // Old content x in [8,64) survives at x-8; the added strip x in [56,80) is filled.
    std::size_t mismatches = 0;
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 8; x < 64; ++x)
            if (!(px(after, x - 8, y) == px(before, x, y)))
                ++mismatches;
    CHECK(mismatches == 0);
    CHECK(px(after, 56, 0) == kFillOrange);
    CHECK(px(after, 79, 63) == kFillOrange);
    // Bounds mode destroyed nothing: the union-sized background still holds the cropped-off
    // 8 columns (buffer wider than the canvas, shifted left).
    const auto* bg = doc->root().child(0).as<core::RasterLayer>();
    CHECK(bg->image().width == 88);
    CHECK(bg->transform() == common::Affine2D::translation(-8, 0));

    doc->commands().undo();
    CHECK(flatten(*doc).rgba == before.rgba);
}

TEST_CASE("rotated crop: 90 degrees maps content exactly; undo restores; both modes") {
    for (const bool deletePixels : {false, true}) {
        CAPTURE(deletePixels);
        auto doc = std::make_unique<core::Document>(64, 64);
        auto bg = doc->makeRaster("Background");
        bg->image().fill({20, 40, 200, 255});
        doc->root().addOnTop(std::move(bg));
        auto red = doc->makeRaster("red", 8, 8);
        red->image().fill({220, 30, 30, 255});
        red->setTransform(common::Affine2D::translation(8, 8));
        doc->root().addOnTop(std::move(red));
        const common::Image before = flatten(*doc);

        // The whole square canvas as a box rotated 90° about the canvas centre: content turns
        // in place, no wedges (the rotated canvas covers itself exactly).
        doc->commands().push(render::buildCropCommand(*doc, 0, 0, 64, 64, deletePixels,
                                                      std::nullopt, std::numbers::pi / 2.0,
                                                      {32.0, 32.0}));
        CHECK(doc->width() == 64);
        const common::Image after = flatten(*doc);
        // Red centre (12,12) -> R(-90° about 32,32) -> (12,52); its old home is now blue.
        CHECK(px(after, 12, 52) == common::Color8{220, 30, 30, 255});
        CHECK(px(after, 12, 12) == common::Color8{20, 40, 200, 255});

        doc->commands().undo();
        CHECK(flatten(*doc).rgba == before.rgba);
    }
}

TEST_CASE("rotated crop: 45 degrees fills the wedges and clears the selection") {
    auto doc = makeExpansionDemo();
    doc->setSelection(core::Selection::rectangle(64, 64, {6, 6, 20, 20}));
    const common::Image before = flatten(*doc);

    doc->commands().push(render::buildCropCommand(
        *doc, 0, 0, 64, 64, /*deletePixels=*/false, render::CropFill{{255, 255, 255, 255},
        "Canvas fill"}, std::numbers::pi / 4.0, {32.0, 32.0}));
    const common::Image after = flatten(*doc);
    // The new canvas's corners are OUTSIDE the rotated old canvas: wedge -> the fill colour.
    CHECK(px(after, 1, 1) == common::Color8{255, 255, 255, 255});
    CHECK(px(after, 62, 62) == common::Color8{255, 255, 255, 255});
    // The centre is still old content: the red square is centred on the pivot, so its middle
    // pixel survives the rotation in place.
    CHECK(px(after, 32, 32).r > 100);
    // A rotated crop cannot carry the selection's pixel geometry: cleared.
    CHECK(doc->selection().isEmpty());

    doc->commands().undo();
    CHECK(flatten(*doc).rgba == before.rgba);
    CHECK_FALSE(doc->selection().isEmpty());
}

// ---------------------------------------------------------------------------------------------
// DragCompositeCache (S15-b) -- the drag fast path must be indistinguishable from the full
// CPU walk, byte for byte, across the frames of a simulated Move drag.
// ---------------------------------------------------------------------------------------------
namespace {

// Drive a three-"frame" drag of `target`: each frame nudges its transform, composites through
// the cache, and pins the result against a fresh full composite. Checkerboard on (the UI's
// recomposite options) so the flatten path is covered too.
void checkDragCacheMatchesFull(core::Document& doc, core::LayerId target) {
    core::Layer* moved = doc.find(target);
    REQUIRE(moved != nullptr);
    render::DragCompositeCache cache;
    render::CompositeOptions opts;
    opts.checkerboard = true;
    const common::Affine2D base = moved->transform();
    for (int frame = 0; frame < 3; ++frame) {
        moved->setTransform(common::Affine2D::translation(1.5 * frame, frame) * base);
        const std::optional<common::Image> fast = cache.composite(doc, target, opts);
        REQUIRE(fast.has_value());
        const render::CompositeResult full = render::composite(doc, opts, render::Backend::Cpu);
        REQUIRE(full.ok);
        INFO("drag frame " << frame);
        CHECK(fast->rgba == full.image.rgba);
    }
    moved->setTransform(base);
}

// A doc-sized raster filled with `c` over [x0,y0)..(x1,y1), transparent elsewhere.
std::unique_ptr<core::RasterLayer> patchLayer(core::Document& doc, const char* name,
                                              std::uint32_t x0, std::uint32_t y0,
                                              std::uint32_t x1, std::uint32_t y1,
                                              common::Color8 c) {
    auto layer = doc.makeRaster(name);
    for (std::uint32_t y = y0; y < y1 && y < doc.height(); ++y) {
        for (std::uint32_t x = x0; x < x1 && x < doc.width(); ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * doc.width() + x) * 4;
            layer->image().rgba[p] = c.r;
            layer->image().rgba[p + 1] = c.g;
            layer->image().rgba[p + 2] = c.b;
            layer->image().rgba[p + 3] = c.a;
        }
    }
    return layer;
}

}  // namespace

TEST_CASE("drag cache: a Normal stack matches the full composite byte-for-byte") {
    core::Document doc(16, 12);
    doc.root().addOnTop(patchLayer(doc, "below", 0, 0, 16, 12, {40, 80, 200, 255}));
    auto target = patchLayer(doc, "target", 2, 2, 8, 8, {220, 60, 60, 255});
    const core::LayerId id = target->id();
    doc.root().addOnTop(std::move(target));
    doc.root().addOnTop(patchLayer(doc, "above", 6, 1, 14, 6, {60, 200, 80, 180}));

    checkDragCacheMatchesFull(doc, id);
}

TEST_CASE("drag cache: a non-Normal blend above the target stays exact") {
    core::Document doc(16, 12);
    doc.root().addOnTop(patchLayer(doc, "below", 0, 0, 16, 12, {90, 90, 90, 255}));
    auto target = patchLayer(doc, "target", 1, 1, 9, 9, {200, 160, 40, 255});
    const core::LayerId id = target->id();
    doc.root().addOnTop(std::move(target));
    auto mult = patchLayer(doc, "multiply", 4, 4, 16, 12, {120, 60, 220, 255});
    mult->setBlendMode(BlendMode::Multiply);
    mult->setOpacity(0.7f);
    doc.root().addOnTop(std::move(mult));
    auto screen = patchLayer(doc, "screen", 0, 6, 10, 12, {30, 220, 180, 200});
    screen->setBlendMode(BlendMode::Screen);
    doc.root().addOnTop(std::move(screen));

    checkDragCacheMatchesFull(doc, id);
}

TEST_CASE("drag cache: an adjustment layer above applies to the live accumulator") {
    core::Document doc(16, 12);
    doc.root().addOnTop(patchLayer(doc, "below", 0, 0, 16, 12, {200, 50, 50, 255}));
    auto target = patchLayer(doc, "target", 3, 3, 10, 10, {50, 200, 50, 255});
    const core::LayerId id = target->id();
    doc.root().addOnTop(std::move(target));
    doc.root().addOnTop(doc.makeAdjustment("invert", core::AdjustmentKind::Invert));

    checkDragCacheMatchesFull(doc, id);
}

TEST_CASE("drag cache: clip-to-below above the target follows the moving clip base") {
    core::Document doc(16, 12);
    doc.root().addOnTop(patchLayer(doc, "below", 0, 0, 16, 12, {40, 40, 40, 255}));
    // The target is the clip BASE: dragging it must move where the clipped layer shows.
    auto target = patchLayer(doc, "target", 2, 2, 9, 9, {220, 220, 220, 255});
    const core::LayerId id = target->id();
    doc.root().addOnTop(std::move(target));
    auto clipped = patchLayer(doc, "clipped", 0, 0, 16, 12, {255, 0, 0, 255});
    clipped->setClipToBelow(true);
    doc.root().addOnTop(std::move(clipped));

    checkDragCacheMatchesFull(doc, id);
}

TEST_CASE("drag cache: a masked group target re-places its cached local composite") {
    core::Document doc(16, 12);
    doc.root().addOnTop(patchLayer(doc, "below", 0, 0, 16, 12, {30, 60, 120, 255}));
    auto group = doc.makeGroup("G");
    const core::LayerId id = group->id();
    auto inner = patchLayer(doc, "inner", 1, 1, 7, 7, {250, 120, 30, 255});
    inner->setTransform(common::Affine2D::translation(2, 1));
    group->addOnTop(std::move(inner));
    auto innerTop = patchLayer(doc, "innerTop", 3, 3, 9, 9, {30, 250, 120, 160});
    innerTop->setBlendMode(BlendMode::Screen);
    group->addOnTop(std::move(innerTop));
    core::RasterMask mask(16, 12, 255);
    for (std::uint32_t x = 0; x < 16; ++x) mask.coverage[x] = 0; // hide the group's top row
    group->setMask(std::move(mask));
    group->setOpacity(0.8f);
    doc.root().addOnTop(std::move(group));

    checkDragCacheMatchesFull(doc, id);
}

TEST_CASE("drag cache: an unmasked transformed group target re-places its offset local buffer") {
    // The unmasked group fast path caches the children over the group's content bounds (with an
    // origin offset) and re-places that buffer through the live transform each frame. It must stay
    // byte-identical to the full composite (which sizes the buffer to the per-frame visible region)
    // as the group is dragged -- including the offset the content-bounds origin introduces.
    core::Document doc(16, 12);
    doc.root().addOnTop(patchLayer(doc, "below", 0, 0, 16, 12, {30, 60, 120, 255}));
    auto group = doc.makeGroup("G");
    const core::LayerId id = group->id();
    auto inner = patchLayer(doc, "inner", 1, 1, 7, 7, {250, 120, 30, 255});
    inner->setTransform(common::Affine2D::translation(3, 2));
    group->addOnTop(std::move(inner));
    auto innerTop = patchLayer(doc, "innerTop", 4, 4, 10, 10, {30, 250, 120, 160});
    innerTop->setBlendMode(BlendMode::Screen);
    group->addOnTop(std::move(innerTop));
    group->setTransform(common::Affine2D::translation(-2, 3)); // a transformed (unmasked) group
    group->setOpacity(0.8f);
    doc.root().addOnTop(std::move(group));

    checkDragCacheMatchesFull(doc, id);
}

TEST_CASE("drag cache: nested and unknown targets fall back to the full composite") {
    core::Document doc(8, 8);
    auto group = doc.makeGroup("G");
    auto nested = patchLayer(doc, "nested", 0, 0, 4, 4, {200, 0, 0, 255});
    const core::LayerId nestedId = nested->id();
    group->addOnTop(std::move(nested));
    doc.root().addOnTop(std::move(group));

    render::DragCompositeCache cache;
    CHECK_FALSE(cache.composite(doc, nestedId).has_value());          // not a top-level child
    CHECK_FALSE(cache.composite(doc, core::LayerId{9999}).has_value()); // unknown id
}

TEST_CASE("drag cache: invalidate() picks up edits made between drags") {
    core::Document doc(16, 12);
    auto below = patchLayer(doc, "below", 0, 0, 16, 12, {10, 120, 10, 255});
    auto* belowRaw = below.get();
    doc.root().addOnTop(std::move(below));
    auto target = patchLayer(doc, "target", 4, 4, 12, 10, {120, 10, 120, 255});
    const core::LayerId id = target->id();
    doc.root().addOnTop(std::move(target));

    render::DragCompositeCache cache;
    render::CompositeOptions opts;
    opts.checkerboard = true;
    REQUIRE(cache.composite(doc, id, opts).has_value()); // cache built against the green below

    belowRaw->image().fill({200, 200, 0, 255}); // the edit any non-drag path would make...
    belowRaw->invalidateContentBounds();
    cache.invalidate();                          // ...followed by the owner's invalidation

    const std::optional<common::Image> fast = cache.composite(doc, id, opts);
    REQUIRE(fast.has_value());
    const render::CompositeResult full = render::composite(doc, opts, render::Backend::Cpu);
    REQUIRE(full.ok);
    CHECK(fast->rgba == full.image.rgba);
}

TEST_CASE("drag cache: a structural change rebuilds instead of replaying stale buffers") {
    core::Document doc(16, 12);
    doc.root().addOnTop(patchLayer(doc, "below", 0, 0, 16, 12, {80, 80, 80, 255}));
    auto target = patchLayer(doc, "target", 2, 2, 10, 10, {200, 80, 80, 255});
    const core::LayerId id = target->id();
    doc.root().addOnTop(std::move(target));

    render::DragCompositeCache cache;
    REQUIRE(cache.composite(doc, id).has_value());

    // A new layer lands above (child count changes): the next composite must rebuild.
    doc.root().addOnTop(patchLayer(doc, "late", 5, 5, 16, 12, {0, 200, 200, 128}));
    const std::optional<common::Image> fast = cache.composite(doc, id);
    REQUIRE(fast.has_value());
    const render::CompositeResult full = render::composite(doc, {}, render::Backend::Cpu);
    REQUIRE(full.ok);
    CHECK(fast->rgba == full.image.rgba);
}

TEST_CASE("drag cache: the buffer budget bows out to the full composite") {
    core::Document doc(8, 8);
    auto target = patchLayer(doc, "target", 0, 0, 4, 4, {200, 0, 0, 255});
    const core::LayerId id = target->id();
    doc.root().addOnTop(std::move(target));
    for (int i = 0; i < 8; ++i) // 8 rasters above + belowAcc > the ~6-buffer cache budget
        doc.root().addOnTop(patchLayer(doc, "above", 1, 1, 6, 6, {0, 20, 200, 90}));

    render::DragCompositeCache cache;
    CHECK_FALSE(cache.composite(doc, id).has_value());
}

// ---------------------------------------------------------------------------------------------
// Golden image -- the integrated demo scene, guarded against regressions byte-for-byte.
// ---------------------------------------------------------------------------------------------
namespace {

// Load the RGB bytes of tests/golden/compositor_demo.ppm (P6), or empty on failure.
std::vector<unsigned char> loadGolden(int& w, int& h) {
    std::ifstream in(std::string(MOSAIC_GOLDEN_DIR) + "/compositor_demo.ppm", std::ios::binary);
    if (!in.good()) return {};
    std::string magic;
    int maxv = 0;
    in >> magic >> w >> h >> maxv;
    in.get();  // consume the single whitespace after the header
    if (magic != "P6") return {};
    std::vector<unsigned char> rgb(static_cast<std::size_t>(w) * h * 3);
    in.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    if (in.gcount() != static_cast<std::streamsize>(rgb.size())) return {};
    return rgb;
}

// Max absolute per-channel (RGB) difference between an image and the golden RGB bytes.
int maxDiffVsGolden(const common::Image& img, const std::vector<unsigned char>& golden) {
    int worst = 0;
    for (std::size_t i = 0; i < img.pixelCount(); ++i) {
        for (int c = 0; c < 3; ++c) {
            const int d = std::abs(static_cast<int>(img.rgba[i * 4 + c]) -
                                   static_cast<int>(golden[i * 3 + c]));
            worst = std::max(worst, d);
        }
    }
    return worst;
}

}  // namespace

TEST_CASE("the CPU demo composite matches its golden reference exactly") {
    const auto doc = render::makeCompositorDemo();
    render::CompositeOptions opts;
    opts.checkerboard = true;
    const render::CompositeResult r = render::composite(*doc, opts, render::Backend::Cpu);
    REQUIRE(r.ok);

    int gw = 0, gh = 0;
    const std::vector<unsigned char> golden = loadGolden(gw, gh);
    REQUIRE_MESSAGE(!golden.empty(), "missing golden: regenerate with --composite-demo --cpu --export");
    REQUIRE(gw == static_cast<int>(r.image.width));
    REQUIRE(gh == static_cast<int>(r.image.height));
    CHECK(maxDiffVsGolden(r.image, golden) == 0);  // CPU is deterministic -> byte-exact
}

// ---------------------------------------------------------------------------------------------
// GPU compute path (S7-b) -- verified to match the CPU reference. Skipped where no GPU exists.
// ---------------------------------------------------------------------------------------------
namespace {

// True per-channel closeness within `tol` (GPU float vs CPU float differ by <=1 in 8-bit).
bool imagesClose(const common::Image& a, const common::Image& b, int tol) {
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size()) return false;
    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
        if (std::abs(static_cast<int>(a.rgba[i]) - static_cast<int>(b.rgba[i])) > tol) return false;
    }
    return true;
}

}  // namespace

TEST_CASE("GPU compositor matches the CPU reference for every blend mode") {
    // Probe for a GPU; skip cleanly on machines (CI) without one.
    {
        core::Document probe(1, 1);
        if (!render::composite(probe, {}, render::Backend::GpuCompute).ok) {
            MESSAGE("GPU compositor unavailable, skipping GPU blend-mode checks");
            return;
        }
    }

    for (int m = 0; m < core::kBlendModeCount; ++m) {
        const auto mode = static_cast<BlendMode>(m);
        core::Document doc(4, 4);
        auto backdrop = doc.makeRaster("backdrop");
        backdrop->image().fill({64, 160, 200, 255});
        doc.root().addOnTop(std::move(backdrop));
        auto source = doc.makeRaster("source");
        source->image().fill({200, 80, 140, 180});  // partial alpha exercises source-over
        source->setBlendMode(mode);
        source->setOpacity(0.8f);
        doc.root().addOnTop(std::move(source));

        const common::Image cpu = render::composite(doc, {}, render::Backend::Cpu).image;
        const render::CompositeResult gpu = render::composite(doc, {}, render::Backend::GpuCompute);
        REQUIRE(gpu.ok);
        CHECK(gpu.usedBackend == render::Backend::GpuCompute);
        CHECK(gpu.validationErrors == 0);
        INFO("blend mode: " << core::blendModeName(mode));
        CHECK(imagesClose(cpu, gpu.image, 1));
    }
}

TEST_CASE("GPU demo composite matches the golden within tolerance") {
    const auto doc = render::makeCompositorDemo();
    render::CompositeOptions opts;
    opts.checkerboard = true;
    const render::CompositeResult r = render::composite(*doc, opts, render::Backend::GpuCompute);
    if (!r.ok) {
        MESSAGE("GPU compositor unavailable, skipping GPU golden check");
        return;
    }
    CHECK(r.validationErrors == 0);
    int gw = 0, gh = 0;
    const std::vector<unsigned char> golden = loadGolden(gw, gh);
    REQUIRE(!golden.empty());
    REQUIRE(gw == static_cast<int>(r.image.width));
    CHECK(maxDiffVsGolden(r.image, golden) <= 1);  // GPU float rounding within 1/255
}

// ---------------------------------------------------------------------------------------------
// Dirty-region recomposite (S60-a): compositeRegion() must equal the matching sub-rectangle of a
// full composite for leaf layers, at a fraction of the cost.
// ---------------------------------------------------------------------------------------------
namespace {

// Fill a raster with a deterministic, spatially varying pattern so a sub-rectangle comparison is
// sensitive to position -- a flat fill would pass even with a wrong origin offset.
void fillPattern(core::RasterLayer& layer, std::uint8_t seed) {
    common::Image& img = layer.image();
    for (std::uint32_t y = 0; y < img.height; ++y)
        for (std::uint32_t x = 0; x < img.width; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
            img.rgba[p + 0] = static_cast<std::uint8_t>((x * 7 + seed) & 0xFF);
            img.rgba[p + 1] = static_cast<std::uint8_t>((y * 5 + seed * 3) & 0xFF);
            img.rgba[p + 2] = static_cast<std::uint8_t>((x * 3 + y * 11 + seed) & 0xFF);
            img.rgba[p + 3] = static_cast<std::uint8_t>(160 + ((x + y + seed) % 96));
        }
}

// Assert compositeRegion(roi) is byte-identical to the matching window of `full`.
void checkRegionMatchesFull(const core::Document& doc, const common::Image& full, long rx, long ry,
                            std::uint32_t rw, std::uint32_t rh) {
    const render::CompositeResult r = render::compositeRegion(
        doc, {static_cast<double>(rx), static_cast<double>(ry), static_cast<double>(rw),
              static_cast<double>(rh)},
        {}, render::Backend::Cpu);
    REQUIRE(r.ok);
    REQUIRE(r.image.width == rw);
    REQUIRE(r.image.height == rh);
    for (std::uint32_t y = 0; y < rh; ++y)
        for (std::uint32_t x = 0; x < rw; ++x)
            REQUIRE(px(r.image, x, y) == px(full, static_cast<std::uint32_t>(rx) + x,
                                            static_cast<std::uint32_t>(ry) + y));
}

}  // namespace

TEST_CASE("compositeRegion equals the full composite sub-rect (blend + opacity + mask)") {
    core::Document doc(64, 48);
    auto bottom = doc.makeRaster("b");
    fillPattern(*bottom, 11);
    doc.root().addOnTop(std::move(bottom));

    auto mid = doc.makeRaster("m");
    fillPattern(*mid, 73);
    mid->setBlendMode(BlendMode::Multiply);
    mid->setOpacity(0.6f);
    doc.root().addOnTop(std::move(mid));

    auto top = doc.makeRaster("t");
    fillPattern(*top, 200);
    top->setBlendMode(BlendMode::Screen);
    core::RasterMask mask(64, 48, 255); // varies across the canvas -> exercises mask folding too
    for (std::size_t i = 0; i < mask.coverage.size(); ++i)
        mask.coverage[i] = static_cast<std::uint8_t>((i * 13) & 0xFF);
    top->setMask(std::move(mask));
    doc.root().addOnTop(std::move(top));

    const common::Image full = flatten(doc);

    checkRegionMatchesFull(doc, full, 0, 0, 64, 48);   // whole canvas
    checkRegionMatchesFull(doc, full, 10, 8, 20, 16);  // interior
    checkRegionMatchesFull(doc, full, 0, 0, 5, 5);     // top-left corner
    checkRegionMatchesFull(doc, full, 59, 43, 5, 5);   // bottom-right corner
    checkRegionMatchesFull(doc, full, 30, 0, 1, 48);   // 1px-wide column
}

TEST_CASE("compositeRegion is byte-identical with an integer-translated layer") {
    core::Document doc(40, 40);
    auto bg = doc.makeRaster("bg");
    fillPattern(*bg, 5);
    doc.root().addOnTop(std::move(bg));
    auto moved = doc.makeRaster("moved");
    fillPattern(*moved, 90);
    moved->setTransform(common::Affine2D::translation(7.0, -3.0)); // whole-pixel -> lossless Nearest
    doc.root().addOnTop(std::move(moved));

    const common::Image full = flatten(doc);
    checkRegionMatchesFull(doc, full, 12, 9, 16, 18);
}

TEST_CASE("compositeRegion clamps to the canvas and rejects empty/out-of-bounds rects") {
    core::Document doc(16, 16);
    auto l = doc.makeRaster("l");
    fillPattern(*l, 1);
    doc.root().addOnTop(std::move(l));
    const common::Image full = flatten(doc);

    // A rect poking past the bottom-right is clamped to the canvas.
    const render::CompositeResult clamped =
        render::compositeRegion(doc, {10.0, 10.0, 20.0, 20.0}, {}, render::Backend::Cpu);
    REQUIRE(clamped.ok);
    CHECK(clamped.image.width == 6);
    CHECK(clamped.image.height == 6);
    for (std::uint32_t y = 0; y < 6; ++y)
        for (std::uint32_t x = 0; x < 6; ++x)
            CHECK(px(clamped.image, x, y) == px(full, 10 + x, 10 + y));

    // Fully outside / zero-area -> not ok.
    CHECK_FALSE(render::compositeRegion(doc, {20.0, 20.0, 4.0, 4.0}, {}, render::Backend::Cpu).ok);
    CHECK_FALSE(render::compositeRegion(doc, {0.0, 0.0, 0.0, 0.0}, {}, render::Backend::Cpu).ok);
}

// ---------------------------------------------------------------------------------------------
// GPU-resident drag fast-path applicability (S60-a canUseGpuDrag).
// ---------------------------------------------------------------------------------------------
TEST_CASE("canUseGpuDrag: only a topmost, visible, unmasked, separable raster qualifies") {
    core::Document doc(32, 32);
    auto bottom = doc.makeRaster("bottom");
    const core::LayerId bottomId = bottom->id();
    doc.root().addOnTop(std::move(bottom));
    auto top = doc.makeRaster("top");
    const core::LayerId topId = top->id();
    doc.root().addOnTop(std::move(top));

    CHECK(render::canUseGpuDrag(doc, topId));        // topmost raster, Normal, visible, unmasked
    CHECK_FALSE(render::canUseGpuDrag(doc, bottomId)); // a layer composites above it
    CHECK_FALSE(render::canUseGpuDrag(doc, core::kInvalidLayerId));

    auto* topLayer = doc.find(topId);
    REQUIRE(topLayer != nullptr);

    topLayer->setBlendMode(BlendMode::Multiply); // separable -> still ok
    CHECK(render::canUseGpuDrag(doc, topId));
    topLayer->setBlendMode(BlendMode::Hue); // non-separable HSL -> CPU
    CHECK_FALSE(render::canUseGpuDrag(doc, topId));
    topLayer->setBlendMode(BlendMode::Normal);

    topLayer->setVisible(false);
    CHECK_FALSE(render::canUseGpuDrag(doc, topId));
    topLayer->setVisible(true);

    topLayer->setClipToBelow(true);
    CHECK_FALSE(render::canUseGpuDrag(doc, topId));
    topLayer->setClipToBelow(false);

    topLayer->setMask(core::RasterMask(32, 32, 255));
    CHECK_FALSE(render::canUseGpuDrag(doc, topId));
    CHECK(render::canUseGpuDrag(doc, topId) == false);
}

// ---- S50 magic layers: the source is never baked, so transforms never compound ---------------

namespace {

// A `n x n` image with a single opaque red pixel at (px,py); everything else transparent. The
// lone pixel is the fidelity probe: nearest-resampling a shrunken raster destroys it outright.
common::Image probeImage(std::uint32_t n, std::uint32_t px, std::uint32_t py) {
    common::Image img(n, n);
    img.fill(common::Color8{0, 0, 0, 0});
    const std::size_t p = (static_cast<std::size_t>(py) * n + px) * 4;
    img.rgba[p + 0] = 255;
    img.rgba[p + 3] = 255;
    return img;
}

std::size_t countOpaque(const common::Image& img) {
    std::size_t n = 0;
    for (std::size_t i = 3; i < img.rgba.size(); i += 4)
        if (img.rgba[i] > 128)
            ++n;
    return n;
}

} // namespace

TEST_CASE("placedImageTransform fits, never magnifies, and centres") {
    using core::placedImageTransform;
    using common::Vec2;

    // Bigger than the canvas: scaled down uniformly to touch the limiting edge, and centred.
    {
        const common::Affine2D t = placedImageTransform(400, 200, 100, 100); // scale 0.25
        CHECK(t.apply(Vec2{0, 0}).x == doctest::Approx(0.0));
        CHECK(t.apply(Vec2{400, 200}).x == doctest::Approx(100.0)); // spans the full width
        CHECK(t.apply(Vec2{0, 0}).y == doctest::Approx(25.0));      // centred vertically
        CHECK(t.apply(Vec2{400, 200}).y == doctest::Approx(75.0));
    }
    // Smaller than the canvas: NOT magnified, just centred.
    {
        const common::Affine2D t = placedImageTransform(40, 20, 100, 100);
        CHECK(t.apply(Vec2{0, 0}).x == doctest::Approx(30.0));
        CHECK(t.apply(Vec2{40, 20}).x == doctest::Approx(70.0)); // still 40 px wide
        CHECK(t.apply(Vec2{0, 0}).y == doctest::Approx(40.0));
    }
    // Exactly the canvas: the identity.
    CHECK(placedImageTransform(64, 64, 64, 64) == common::Affine2D::identity());
    // Degenerate inputs never divide by zero.
    CHECK(placedImageTransform(0, 10, 64, 64) == common::Affine2D::identity());
    CHECK(placedImageTransform(10, 10, 0, 64) == common::Affine2D::identity());
}

TEST_CASE("a magic layer composites from its full-resolution source, not from a baked raster") {
    core::Document doc(64, 64);
    // A 64x64 probe placed 1:1, then shrunk hard and grown back. A RasterLayer would have to bake
    // each step; a MagicLayer re-reads source() through whatever transform it currently carries.
    auto magic = doc.makeMagic("placed", probeImage(64, 32, 32));
    core::MagicLayer* m = magic.get();
    doc.root().addOnTop(std::move(magic));

    const common::Image before = render::composite(doc, {}, render::Backend::Cpu).image;
    const std::size_t opaqueBefore = countOpaque(before);
    CHECK(opaqueBefore >= 1); // the probe pixel survives a 1:1 placement

    // Shrink to 1/16 and back. The intermediate composite loses the probe to minification...
    m->setTransform(common::Affine2D::scaling(1.0 / 16.0, 1.0 / 16.0));
    const common::Image shrunk = render::composite(doc, {}, render::Backend::Cpu).image;
    CHECK(countOpaque(shrunk) < opaqueBefore);

    // ... but the SOURCE was never touched, so restoring the transform restores the pixels exactly.
    m->setTransform(common::Affine2D::identity());
    const common::Image after = render::composite(doc, {}, render::Backend::Cpu).image;
    CHECK(after.rgba == before.rgba);
    CHECK(m->source().width == 64); // still full resolution behind the shrunken view
}

TEST_CASE("a placed magic layer lands where placedImageTransform says, and keeps its source") {
    core::Document doc(100, 100);
    auto magic = doc.makeMagic("placed", probeImage(400, 200, 100)); // 400x400 source, probe centred
    core::MagicLayer* m = magic.get();
    m->setTransform(core::placedImageTransform(400, 400, doc.width(), doc.height()));
    doc.root().addOnTop(std::move(magic));

    const common::Image out = render::composite(doc, {}, render::Backend::Cpu).image;
    // The source is square and the canvas is square: it fills the canvas at 1/4 scale.
    const std::optional<common::Rect> bounds = m->contentBounds();
    REQUIRE(bounds.has_value());
    CHECK(m->source().width == 400); // untouched
    CHECK(out.width == 100);
    // The probe at source (200,100) maps to doc (50, 25): scale 0.25, no offset (square fit).
    CHECK(m->transform().apply(common::Vec2{200, 100}).x == doctest::Approx(50.0));
    CHECK(m->transform().apply(common::Vec2{200, 100}).y == doctest::Approx(25.0));
}

// ---- Layer -> Rasterize: the composite must not move ------------------------------------------
//
// rasterizeLayer bakes a layer's transform, mask and effects into pixels but deliberately leaves its
// visibility, opacity, blend mode and clip-to-below alone -- those style how it composites INTO its
// parent, so they ride across to the raster that replaces it. Carry exactly those four and the
// document composites to identical pixels. That invariant is what makes Rasterize safe, so pin it.

namespace {

// Replace `id` with a RasterLayer of its baked pixels, the way MainWindow::rasterizeLayerCommand
// does, and hand back the composite afterwards.
common::Image compositeAfterRasterizing(core::Document& doc, core::LayerId id,
                                       render::ResampleFilter filter = render::ResampleFilter::Nearest) {
    core::Layer* layer = doc.find(id);
    REQUIRE(layer != nullptr);
    // The filter must match the one the composite below uses -- it decides whether vector edges keep
    // their analytic AA, so baking with a different one would change the picture (it did, once).
    common::Image baked = render::rasterizeLayer(*layer, doc.width(), doc.height(), filter);
    REQUIRE_FALSE(baked.empty());
    auto raster = doc.makeRaster(layer->name(), baked.width, baked.height);
    raster->image() = std::move(baked);
    raster->setVisible(layer->visible());
    raster->setOpacity(layer->opacity());
    raster->setBlendMode(layer->blendMode());
    raster->setClipToBelow(layer->clipToBelow());
    doc.commands().push(
        std::make_unique<core::ReplaceLayerCommand>(id, std::move(raster), "Rasterize"));
    return render::composite(doc, {}, render::Backend::Cpu).image;
}

// Largest per-channel difference between two same-sized images.
int maxChannelDelta(const common::Image& a, const common::Image& b) {
    REQUIRE(a.rgba.size() == b.rgba.size());
    int worst = 0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i)
        worst = std::max(worst, std::abs(static_cast<int>(a.rgba[i]) - static_cast<int>(b.rgba[i])));
    return worst;
}

core::vec::Object filledEllipse(double rx, double ry, common::ColorF c) {
    core::vec::Object o;
    o.geometry = core::vec::EllipseShape{{rx, ry}};
    o.fill = core::vec::SolidPaint{c};
    return o;
}

} // namespace

TEST_CASE("rasterizing a vector layer leaves the composite untouched, opacity and blend included") {
    core::Document doc(64, 64);
    auto bg = doc.makeRaster("bg", 64, 64);
    bg->image().fill(common::Color8{200, 40, 40, 255});
    doc.root().addOnTop(std::move(bg));

    auto vec = doc.makeVector("shape");
    vec->setObject(filledEllipse(20.0, 14.0, common::ColorF{0.1f, 0.4f, 0.9f, 1.0f}));
    vec->setTransform(common::Affine2D::translation(32, 32));
    vec->setOpacity(0.55f);
    vec->setBlendMode(core::BlendMode::Multiply);
    const core::LayerId id = vec->id();
    doc.root().addOnTop(std::move(vec));

    const common::Image before = render::composite(doc, {}, render::Backend::Cpu).image;
    const common::Image after = compositeAfterRasterizing(doc, id);
    CHECK(after.rgba == before.rgba);

    // The layer really did change kind, and undo puts the vector back byte-for-byte.
    CHECK(doc.root().child(1).kind() == core::LayerKind::Raster);
    doc.commands().undo();
    CHECK(doc.root().child(1).kind() == core::LayerKind::Vector);
    CHECK(doc.root().child(1).id() == id);
    CHECK(render::composite(doc, {}, render::Backend::Cpu).image.rgba == before.rgba);
    doc.commands().redo();
    CHECK(doc.root().child(1).kind() == core::LayerKind::Raster);
    CHECK(render::composite(doc, {}, render::Backend::Cpu).image.rgba == before.rgba);
}

TEST_CASE("rasterizing a magic layer bakes its placement and leaves the composite untouched") {
    core::Document doc(64, 64);
    auto magic = doc.makeMagic("placed", probeImage(64, 20, 20));
    magic->setTransform(core::placedImageTransform(64, 64, 64, 64));
    magic->setOpacity(0.8f);
    const core::LayerId id = magic->id();
    doc.root().addOnTop(std::move(magic));

    const common::Image before = render::composite(doc, {}, render::Backend::Cpu).image;
    CHECK(compositeAfterRasterizing(doc, id).rgba == before.rgba);
    CHECK(doc.root().child(0).kind() == core::LayerKind::Raster);
    // ... and the magic layer's non-destructive source is gone: that is what "rasterize" means.
    CHECK(doc.root().child(0).as<core::MagicLayer>() == nullptr);
}

TEST_CASE("rasterizing a group flattens its subtree without moving the composite") {
    core::Document doc(64, 64);
    auto group = doc.makeGroup("g");
    auto a = doc.makeRaster("a", 32, 32);
    a->image().fill(common::Color8{0, 180, 0, 255});
    auto b = doc.makeRaster("b", 32, 32);
    b->image().fill(common::Color8{0, 0, 180, 200});
    b->setTransform(common::Affine2D::translation(16, 16));
    group->addOnTop(std::move(a));
    group->addOnTop(std::move(b));
    group->setOpacity(0.6f);
    const core::LayerId id = group->id();
    doc.root().addOnTop(std::move(group));

    const common::Image before = render::composite(doc, {}, render::Backend::Cpu).image;
    CHECK(compositeAfterRasterizing(doc, id).rgba == before.rgba);
    CHECK(doc.root().child(0).kind() == core::LayerKind::Raster); // the subtree is gone
    CHECK(doc.root().childCount() == 1);
}

TEST_CASE("rasterizing bakes a layer mask in, so the raster needs none") {
    core::Document doc(32, 32);
    // A MAGIC layer, not a vector one: the compositor folds a leaf raster/magic/text mask today,
    // but a VectorLayer's mask is still ignored (render/compositor.cpp: "mask folding is a
    // follow-up (S31)"). Testing the bake on a vector layer would prove nothing.
    common::Image src(32, 32);
    src.fill(common::Color8{255, 255, 255, 255});
    auto magic = doc.makeMagic("placed", std::move(src));
    core::RasterMask mask(32, 32);
    for (std::uint32_t y = 0; y < 32; ++y)
        for (std::uint32_t x = 0; x < 32; ++x)
            mask.coverage[static_cast<std::size_t>(y) * 32 + x] = x < 16 ? 0 : 255;
    magic->setMask(mask);
    const core::LayerId id = magic->id();
    doc.root().addOnTop(std::move(magic));

    // The mask really is doing something: the left half of the composite is empty.
    const common::Image before = render::composite(doc, {}, render::Backend::Cpu).image;
    CHECK(before.rgba[(16u * 32 + 4) * 4 + 3] == 0);    // left: masked away
    CHECK(before.rgba[(16u * 32 + 24) * 4 + 3] == 255); // right: opaque

    CHECK(compositeAfterRasterizing(doc, id).rgba == before.rgba);
    // The mask is IN the pixels now; carrying it across as well would apply it twice.
    const core::Layer& baked = doc.root().child(0);
    REQUIRE(baked.kind() == core::LayerKind::Raster);
    CHECK(baked.mask() == nullptr);
    const common::Image& px = baked.as<core::RasterLayer>()->image();
    CHECK(px.rgba[(16u * 32 + 4) * 4 + 3] == 0);    // the bake carries the hole
    CHECK(px.rgba[(16u * 32 + 24) * 4 + 3] == 255);
}

TEST_CASE("rasterizing honours the document's resample filter -- it must not re-AA the picture") {
    core::Document doc(64, 64);
    auto vec = doc.makeVector("shape");
    vec->setObject(filledEllipse(20.0, 14.0, common::ColorF{1.0f, 1.0f, 1.0f, 1.0f}));
    vec->setTransform(common::Affine2D::translation(32, 32));
    const core::Layer& layer = doc.root().addOnTop(std::move(vec));

    // Nearest hardens vector edges; Auto keeps the analytic AA. A bake that ignored the document's
    // choice would silently anti-alias (or alias) the layer the moment you rasterized it -- which is
    // exactly the bug this argument exists to prevent.
    const common::Image hard = render::rasterizeLayer(layer, 64, 64, render::ResampleFilter::Nearest);
    const common::Image soft = render::rasterizeLayer(layer, 64, 64, render::ResampleFilter::Auto);
    CHECK(hard.rgba != soft.rgba);
    CHECK(maxChannelDelta(hard, soft) > 32); // an edge, not a rounding wobble

    // Every alpha in the hard bake is 0 or 255; the soft one carries partial coverage.
    bool hardHasPartial = false;
    bool softHasPartial = false;
    for (std::size_t i = 3; i < hard.rgba.size(); i += 4) {
        hardHasPartial |= hard.rgba[i] != 0 && hard.rgba[i] != 255;
        softHasPartial |= soft.rgba[i] != 0 && soft.rgba[i] != 255;
    }
    CHECK_FALSE(hardHasPartial);
    CHECK(softHasPartial);
}

// ---------------------------------------------------------------------------------------------
// Layer masks (S31): link/unlink semantics, vector mask folding, masked groups under region
// composites, and merge-down parity.
// ---------------------------------------------------------------------------------------------

TEST_CASE("a LINKED mask rides the layer's transform; an UNLINKED one stays put in doc space") {
    core::Document doc(4, 1);
    auto bottom = doc.makeRaster("b");
    bottom->image().fill({0, 0, 0, 255});
    doc.root().addOnTop(std::move(bottom));

    auto top = doc.makeRaster("t", 4, 1);
    top->image().fill({255, 255, 255, 255});
    core::RasterMask mask(4, 1, 0);
    mask.coverage[0] = 255; // reveal only mask column 0
    top->setMask(std::move(mask));
    auto* topPtr = top.get();
    doc.root().addOnTop(std::move(top));
    topPtr->setTransform(common::Affine2D::translation(1, 0)); // slide the layer right by 1

    // Linked (default): the mask moves with the layer -> the revealed pixel lands at doc x=1.
    {
        const common::Image out = flatten(doc);
        CHECK(px(out, 0, 0) == common::Color8{0, 0, 0, 255});
        CHECK(px(out, 1, 0) == common::Color8{255, 255, 255, 255});
        CHECK(px(out, 2, 0) == common::Color8{0, 0, 0, 255});
    }

    // Unlinked: the mask sits still at doc x=0 -- where the moved layer has no pixel to show
    // (its column -1 doesn't exist), so NOTHING of the white layer survives at x=0 and the
    // revealed window shows only what slides under it.
    topPtr->mask()->linked = false;
    {
        const common::Image out = flatten(doc);
        CHECK(px(out, 0, 0) == common::Color8{0, 0, 0, 255});  // white column -1: nothing there
        CHECK(px(out, 1, 0) == common::Color8{0, 0, 0, 255});  // masked out (mask x=1 is 0)
        CHECK(px(out, 2, 0) == common::Color8{0, 0, 0, 255});
    }

    // Slide the layer back to identity: linked and unlinked agree again (the mask grids align).
    topPtr->setTransform(common::Affine2D::identity());
    {
        const common::Image out = flatten(doc);
        CHECK(px(out, 0, 0) == common::Color8{255, 255, 255, 255});
        CHECK(px(out, 1, 0) == common::Color8{0, 0, 0, 255});
    }
}

TEST_CASE("a disabled mask is ignored whatever its linkage") {
    core::Document doc(2, 1);
    auto bottom = doc.makeRaster("b");
    bottom->image().fill({0, 0, 0, 255});
    doc.root().addOnTop(std::move(bottom));
    auto top = doc.makeRaster("t", 2, 1);
    top->image().fill({255, 255, 255, 255});
    core::RasterMask mask(2, 1, 0); // hides everything...
    mask.enabled = false;           // ...but disabled: full white shows
    top->setMask(std::move(mask));
    auto* topPtr = top.get();
    doc.root().addOnTop(std::move(top));

    CHECK(px(flatten(doc), 0, 0) == common::Color8{255, 255, 255, 255});
    topPtr->mask()->linked = false;
    CHECK(px(flatten(doc), 1, 0) == common::Color8{255, 255, 255, 255});
}

TEST_CASE("a vector layer's mask folds at target resolution (S31 closes the S25 follow-up)") {
    core::Document doc(64, 64);
    auto bg = doc.makeRaster("bg", 64, 64);
    bg->image().fill(common::Color8{200, 40, 40, 255});
    doc.root().addOnTop(std::move(bg));

    auto vec = doc.makeVector("shape");
    vec->setObject(filledEllipse(20.0, 14.0, common::ColorF{0.1f, 0.4f, 0.9f, 1.0f}));
    vec->setTransform(common::Affine2D::translation(32, 32));
    auto* vecPtr = vec.get();
    doc.root().addOnTop(std::move(vec));

    const common::Image unmasked = render::composite(doc, {}, render::Backend::Cpu).image;
    CHECK(px(unmasked, 32, 32) != common::Color8{200, 40, 40, 255}); // the ellipse shows

    // A mask hiding the layer-local left half: local x < 0 -- the ellipse's left side. The mask
    // grid is the document window in LAYER-LOCAL units, so with the layer at translation(32,32)
    // local column 0..31 is the doc's 32..63... the mask below reveals local x in [0,32).
    core::RasterMask mask(64, 64, 0);
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 32; ++x) mask.coverage[y * 64 + x] = 255;
    vecPtr->setMask(std::move(mask));

    const common::Image masked = render::composite(doc, {}, render::Backend::Cpu).image;
    // Local x in [0,32) => doc x in [32,64): the ellipse's RIGHT half survives, the left is cut.
    CHECK(px(masked, 40, 32) == px(unmasked, 40, 32));               // revealed (doc x=40)
    CHECK(px(masked, 24, 32) == common::Color8{200, 40, 40, 255});   // hidden -> bg shows

    // Unlinked, the same mask reveals DOC columns [0,32) instead: the left half survives.
    vecPtr->mask()->linked = false;
    const common::Image unlinked = render::composite(doc, {}, render::Backend::Cpu).image;
    CHECK(px(unlinked, 24, 32) == px(unmasked, 24, 32));             // revealed (doc x=24)
    CHECK(px(unlinked, 40, 32) == common::Color8{200, 40, 40, 255}); // hidden -> bg shows
}

TEST_CASE("a masked group renders correctly under a REGION composite (the S31 extent fix)") {
    core::Document doc(16, 12);
    doc.root().addOnTop(patchLayer(doc, "below", 0, 0, 16, 12, {40, 80, 200, 255}));

    auto group = doc.makeGroup("g");
    auto member = patchLayer(doc, "m", 2, 2, 14, 10, {250, 250, 250, 255});
    group->addOnTop(std::move(member));
    core::RasterMask mask(16, 12, 255);
    for (std::uint32_t y = 0; y < 12; ++y)
        for (std::uint32_t x = 8; x < 16; ++x) mask.coverage[y * 16 + x] = 0; // hide the right half
    group->setMask(std::move(mask));
    doc.root().addOnTop(std::move(group));

    const render::CompositeResult full = render::composite(doc, {}, render::Backend::Cpu);
    REQUIRE(full.ok);
    // A region straddling the mask edge must be byte-identical to the full composite's sub-rect
    // (the old window-aligned fold squashed the mask onto the region buffer).
    const common::Rect roi{6, 3, 6, 6};
    const render::CompositeResult region =
        render::compositeRegion(doc, roi, {}, render::Backend::Cpu);
    REQUIRE(region.ok);
    for (std::uint32_t y = 0; y < region.image.height; ++y)
        for (std::uint32_t x = 0; x < region.image.width; ++x)
            CHECK(px(region.image, x, y) == px(full.image, x + 6, y + 3));
}

TEST_CASE("merge-down folds an unlinked mask in parent space, matching the composite") {
    core::Document doc(4, 1);
    auto lower = doc.makeRaster("lower", 4, 1);
    lower->image().fill({0, 0, 0, 255});
    doc.root().addOnTop(std::move(lower));

    auto upper = doc.makeRaster("upper", 4, 1);
    upper->image().fill({255, 255, 255, 255});
    core::RasterMask mask(4, 1, 0);
    mask.coverage[2] = 255; // reveal doc column 2 only (unlinked)
    mask.linked = false;
    upper->setMask(std::move(mask));
    auto* upperPtr = upper.get();
    doc.root().addOnTop(std::move(upper));
    upperPtr->setTransform(common::Affine2D::translation(1, 0));

    const common::Image before = flatten(doc);
    const auto merged = render::mergeDown(doc.root().child(1), // upper
                                          *doc.root().child(0).as<core::RasterLayer>());
    REQUIRE(merged.has_value());
    // Replace the pair with the merged raster and compare composites.
    core::Document doc2(4, 1);
    auto baked = doc2.makeRaster("baked", 4, 1);
    baked->image() = *merged;
    doc2.root().addOnTop(std::move(baked));
    CHECK(flatten(doc2).rgba == before.rgba);
}

// ---------------------------------------------------------------------------------------------
// Drag-cache admission (S15-b cache, budget corrected in S60-a). The count alone was never a
// memory bound; these pin the arithmetic the header's comment claims.
// ---------------------------------------------------------------------------------------------

TEST_CASE("drag cache: a buffer's cost is the document's, not a constant") {
    // The count was calibrated at 1080p and the calibration does not travel.
    CHECK(render::dragBufferBytes(1920, 1080) == 33177600ull);   //  31.6 MiB
    CHECK(render::dragBufferBytes(3840, 2160) == 132710400ull);  // 126.6 MiB
    CHECK(render::dragBufferBytes(5000, 8000) == 640000000ull);  // 610.4 MiB
}

TEST_CASE("drag cache: everything at or below 4K behaves exactly as before") {
    // The count binds at these sizes, so the byte budget changes nothing -- which is the point:
    // the fix must not alter the case it was calibrated on.
    for (auto [w, h] : {std::pair{1920u, 1080u}, std::pair{3840u, 2160u}}) {
        CAPTURE(w);
        CAPTURE(h);
        CHECK(render::dragCacheFits(render::kMaxCachedDragBuffers, w, h));
        CHECK_FALSE(render::dragCacheFits(render::kMaxCachedDragBuffers + 1, w, h));
    }
}

TEST_CASE("drag cache: a huge document caches belowAcc alone instead of 3.6 GiB") {
    // Six buffers at 5000x8000 is 3.6 GiB -- it swaps or dies rather than caching. One is 610 MiB
    // and still buys the ~2.7x the bench measured over the full walk, so the answer is "fewer",
    // not "none".
    CHECK(render::dragCacheFits(1, 5000, 8000));
    CHECK_FALSE(render::dragCacheFits(2, 5000, 8000));
    CHECK(1ull * render::dragBufferBytes(5000, 8000) <= render::kMaxCachedDragBytes);
    CHECK(2ull * render::dragBufferBytes(5000, 8000) > render::kMaxCachedDragBytes);
}

TEST_CASE("drag cache: an absurd canvas is refused outright, without overflowing") {
    // 65535^2 x 16 B is ~68 TiB. The arithmetic must stay in uint64 and simply say no.
    CHECK_FALSE(render::dragCacheFits(1, 65535, 65535));
    CHECK(render::dragBufferBytes(65535, 65535) > render::kMaxCachedDragBytes);
    CHECK(render::dragCacheFits(0, 65535, 65535)); // zero buffers is free, whatever the size
}
