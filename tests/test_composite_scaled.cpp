#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>

#include "common/image.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "render/compositor.hpp"
#include "render/render.hpp"

using namespace mosaic;

namespace {

// A top-level raster layer filled with one opaque colour. Deliberately top-level: a NESTED group
// composites its subtree into a buffer sized in the group's own local pixels, so the bounded walk
// shrinks a nested document's final buffer but not its intermediate ones (documented on
// compositeScaled). These cases pin the bound, not that limit.
core::LayerId addFlat(core::Document& doc, const char* name, common::Color8 c, std::uint32_t w,
                      std::uint32_t h) {
    auto layer = doc.makeRaster(name, w, h);
    layer->image().fill(c);
    return doc.root().addOnTop(std::move(layer)).id();
}

common::Color8 px(const common::Image& img, std::uint32_t x, std::uint32_t y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// compositeScaled -- the bounded composite (S60, docs/s60-gesture-start-stall.md §3.2).
// ---------------------------------------------------------------------------------------------

TEST_CASE("compositeScaled at the document's own size is the full composite, byte for byte") {
    core::Document doc(37, 23);  // deliberately not a round number
    addFlat(doc, "bg", {20, 40, 60, 255}, 37, 23);
    addFlat(doc, "fg", {200, 100, 50, 128}, 20, 12);

    render::CompositeOptions opts;
    opts.checkerboard = false;
    const render::CompositeResult full = render::composite(doc, opts, render::Backend::Cpu);
    const render::CompositeResult same =
        render::compositeScaled(doc, 37, 23, opts, render::Backend::Cpu);
    REQUIRE(full.ok);
    REQUIRE(same.ok);
    // Not "close": the out-size-equals-doc-size arm takes the identity placement, so every fast
    // path that keys on it still fires and the result is the SAME picture, not a resampled one.
    CHECK(same.image == full.image);
}

TEST_CASE("compositeScaled returns exactly the requested size") {
    core::Document doc(100, 40);
    addFlat(doc, "bg", {255, 255, 255, 255}, 100, 40);

    render::CompositeOptions opts;
    opts.checkerboard = false;
    for (const auto [w, h] : {std::pair<std::uint32_t, std::uint32_t>{50, 20},
                              {25, 10},
                              {7, 3},
                              {1, 1}}) {
        const render::CompositeResult r =
            render::compositeScaled(doc, w, h, opts, render::Backend::Cpu);
        REQUIRE(r.ok);
        CHECK(r.image.width == w);
        CHECK(r.image.height == h);
    }
}

TEST_CASE("compositeScaled bounds a document far larger than any texture limit") {
    // The case the stall was measured on. The bound is what makes this cheap: the walk's cost is a
    // function of OUTPUT pixels, so a 40 Mpx document composites into 0.6 Mpx here.
    core::Document doc(5000, 8000);
    addFlat(doc, "bg", {10, 20, 30, 255}, 5000, 8000);

    render::CompositeOptions opts;
    opts.checkerboard = false;
    const render::CompositeResult r =
        render::compositeScaled(doc, 625, 1000, opts, render::Backend::Cpu);
    REQUIRE(r.ok);
    CHECK(r.image.width == 625);
    CHECK(r.image.height == 1000);
    // Vulkan 1.0 guarantees maxImageDimension2D 4096; the whole point of the bound is that what
    // comes back can be a texture on a floor device, which the document itself cannot.
    CHECK(r.image.width <= 4096);
    CHECK(r.image.height <= 4096);
    CHECK(px(r.image, 300, 500).r == 10);
}

TEST_CASE("compositeScaled minifies through a real filter, not a point sample") {
    // A 1px checkerboard reduced 8x. Point-sampling picks ONE phase and returns pure black or pure
    // white; a box filter returns the average. This is the property that lets the backdrop be
    // built at the bound instead of downsampled afterwards.
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("checks", 64, 64);
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 64; ++x) {
            const std::uint8_t v = ((x + y) % 2 == 0) ? 0 : 255;
            const std::size_t p = (static_cast<std::size_t>(y) * 64 + x) * 4;
            layer->image().rgba[p] = layer->image().rgba[p + 1] = layer->image().rgba[p + 2] = v;
            layer->image().rgba[p + 3] = 255;
        }
    doc.root().addOnTop(std::move(layer));

    render::CompositeOptions opts;
    opts.checkerboard = false;
    opts.resampleFilter = render::ResampleFilter::Auto;  // minification -> Area
    const render::CompositeResult r =
        render::compositeScaled(doc, 8, 8, opts, render::Backend::Cpu);
    REQUIRE(r.ok);
    const common::Color8 c = px(r.image, 4, 4);
    CHECK(c.r > 0);
    CHECK(c.r < 255);
    CHECK(c.a == 255);

    // Nearest is the control: it lands on one phase, so it is exactly one of the two extremes.
    opts.resampleFilter = render::ResampleFilter::Nearest;
    const render::CompositeResult point =
        render::compositeScaled(doc, 8, 8, opts, render::Backend::Cpu);
    REQUIRE(point.ok);
    const common::Color8 pc = px(point.image, 4, 4);
    CHECK((pc.r == 0 || pc.r == 255));
}

TEST_CASE("compositeScaled refuses a degenerate size instead of guessing") {
    core::Document doc(16, 16);
    addFlat(doc, "bg", {1, 2, 3, 255}, 16, 16);
    render::CompositeOptions opts;
    opts.checkerboard = false;

    const render::CompositeResult zeroW =
        render::compositeScaled(doc, 0, 8, opts, render::Backend::Cpu);
    CHECK_FALSE(zeroW.ok);
    CHECK_FALSE(zeroW.error.empty());
    const render::CompositeResult zeroH =
        render::compositeScaled(doc, 8, 0, opts, render::Backend::Cpu);
    CHECK_FALSE(zeroH.ok);
}

// ---------------------------------------------------------------------------------------------
// CompositeOptions::skipLayer -- the read-only exclusion (finding G6).
// ---------------------------------------------------------------------------------------------

TEST_CASE("skipLayer composites exactly as hiding the layer would") {
    const auto build = [](core::Document& doc) {
        addFlat(doc, "bg", {30, 60, 90, 255}, 24, 24);
        return addFlat(doc, "top", {240, 30, 30, 255}, 24, 24);
    };
    core::Document a(24, 24);
    const core::LayerId topA = build(a);
    core::Document b(24, 24);
    const core::LayerId topB = build(b);

    render::CompositeOptions skipOpts;
    skipOpts.checkerboard = false;
    skipOpts.skipLayer = topA;
    const render::CompositeResult skipped = render::composite(a, skipOpts, render::Backend::Cpu);

    render::CompositeOptions hideOpts;
    hideOpts.checkerboard = false;
    b.find(topB)->setVisible(false);
    const render::CompositeResult hidden = render::composite(b, hideOpts, render::Backend::Cpu);

    REQUIRE(skipped.ok);
    REQUIRE(hidden.ok);
    CHECK(skipped.image == hidden.image);
    // and it really did leave the top layer out -- the backdrop shows through.
    CHECK(px(skipped.image, 12, 12).r == 30);
}

TEST_CASE("skipLayer never touches the document") {
    core::Document doc(16, 16);
    addFlat(doc, "bg", {10, 10, 10, 255}, 16, 16);
    const core::LayerId top = addFlat(doc, "top", {250, 250, 250, 255}, 16, 16);

    const std::uint64_t revBefore = doc.find(top)->contentRevision();
    render::CompositeOptions opts;
    opts.checkerboard = false;
    opts.skipLayer = top;
    const render::CompositeResult r = render::composite(doc, opts, render::Backend::Cpu);
    REQUIRE(r.ok);

    // The whole point of G6: the old path expressed this by flipping the layer's own visibility and
    // flipping it back, an uncommanded mutation of the user's document.
    CHECK(doc.find(top)->visible());
    CHECK(doc.find(top)->contentRevision() == revBefore);
}

TEST_CASE("an unset or unknown skipLayer leaves the composite alone") {
    core::Document doc(16, 16);
    addFlat(doc, "bg", {10, 20, 30, 255}, 16, 16);
    addFlat(doc, "top", {200, 200, 200, 128}, 16, 16);

    render::CompositeOptions plain;
    plain.checkerboard = false;
    const render::CompositeResult a = render::composite(doc, plain, render::Backend::Cpu);

    render::CompositeOptions unknown = plain;
    unknown.skipLayer = core::LayerId{999999};  // no such layer
    const render::CompositeResult b = render::composite(doc, unknown, render::Backend::Cpu);

    REQUIRE(a.ok);
    REQUIRE(b.ok);
    CHECK(a.image == b.image);
}

TEST_CASE("skipLayer and the bound compose") {
    core::Document doc(64, 64);
    addFlat(doc, "bg", {0, 0, 255, 255}, 64, 64);
    const core::LayerId top = addFlat(doc, "top", {255, 0, 0, 255}, 64, 64);

    render::CompositeOptions opts;
    opts.checkerboard = false;
    opts.skipLayer = top;
    const render::CompositeResult r =
        render::compositeScaled(doc, 16, 16, opts, render::Backend::Cpu);
    REQUIRE(r.ok);
    CHECK(r.image.width == 16);
    CHECK(px(r.image, 8, 8).b == 255);
    CHECK(px(r.image, 8, 8).r == 0);
    CHECK(doc.find(top)->visible());
}
