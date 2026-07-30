#include "common/image.hpp"
#include "core/brush/brush_engine.hpp"
#include "core/command.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/inpaint/inpaint_engine.hpp"
#include "core/layer.hpp"
#include "core/selection.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <memory>

// The CPU brush stamping engine (core::brush, S19-a): dab coverage falloff, spacing-driven dab
// placement, flow build-up within a stroke, the per-stroke opacity cap, and the dirty bounds. Pure
// CPU pixel math -- the FLTK event plumbing in VulkanCanvas is exercised by the --gui-frames run.
namespace {

using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::common::Rect;
using mosaic::common::Vec2;
using mosaic::core::brush::BrushDynamics;
using mosaic::core::brush::BrushEngine;
using mosaic::core::brush::BrushParams;
using mosaic::core::brush::dabCoverage;
using mosaic::core::brush::StrokeInput;

// Alpha (0..255) of pixel (x,y) in a document-sized image.
std::uint8_t alphaAt(const Image& img, int x, int y) {
    return img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4 + 3];
}
Color8 colorAt(const Image& img, int x, int y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

// Paint one solid dab and return the composited result over a transparent target.
Image paintOne(BrushParams params, std::uint32_t w, std::uint32_t h, Vec2 at) {
    Image img(w, h); // transparent target the engine paints onto in place
    BrushEngine eng;
    eng.begin(w, h, img, params, BrushDynamics{}, StrokeInput{at, 1.0});
    eng.composite();
    eng.end();
    return img;
}

} // namespace

TEST_CASE("dabCoverage: solid core, smooth shoulder, zero beyond the rim") {
    // Hard-ish tip: full coverage at the centre, none past the radius, monotonic non-increasing.
    CHECK(dabCoverage(0.0, 10.0, 0.8) == doctest::Approx(1.0));
    CHECK(dabCoverage(10.0, 10.0, 0.8) == doctest::Approx(0.0));
    CHECK(dabCoverage(20.0, 10.0, 0.8) == doctest::Approx(0.0));
    double prev = 1.1;
    for (double d = 0.0; d <= 10.0; d += 0.5) {
        const double c = dabCoverage(d, 10.0, 0.8);
        CHECK(c <= prev + 1e-9); // never increases outward
        CHECK(c >= -1e-9);
        CHECK(c <= 1.0 + 1e-9);
        prev = c;
    }
    // A soft tip (hardness 0) already falls below 1 partway out where a hard tip is still solid.
    CHECK(dabCoverage(4.0, 10.0, 0.0) < dabCoverage(4.0, 10.0, 0.95));
    // Degenerate radius deposits nothing.
    CHECK(dabCoverage(0.0, 0.0, 1.0) == doctest::Approx(0.0));
}

TEST_CASE("single dab: opaque centre, colour matches, framed by the diameter") {
    BrushParams p;
    p.diameter = 20.0;
    p.hardness = 1.0;
    p.flow = 1.0;
    p.opacity = 1.0;
    p.color = Color8{200, 50, 25, 255};
    const Image img = paintOne(p, 64, 64, {32.0, 32.0});

    CHECK(alphaAt(img, 32, 32) == 255);          // solid at the centre
    CHECK(colorAt(img, 32, 32) == p.color);      // ... and the active colour
    CHECK(alphaAt(img, 32, 60) == 0);            // well outside the 20 px tip: untouched
    // A dab ~10 px radius reaches roughly to x=42 and not far past it.
    CHECK(alphaAt(img, 40, 32) > 0);
    CHECK(alphaAt(img, 48, 32) == 0);
}

TEST_CASE("opacity caps a self-overlapping stroke") {
    // Two dabs stamped on top of each other at flow 1, opacity 0.5 -> the result is capped at ~50%
    // alpha, NOT building to opaque (the per-stroke opacity cap, Photoshop's model).
    BrushParams p;
    p.diameter = 16.0;
    p.hardness = 1.0;
    p.flow = 1.0;
    p.opacity = 0.5;
    p.color = Color8{0, 0, 0, 255};

    Image img(48, 48);
    BrushEngine eng;
    eng.begin(48, 48, img, p, BrushDynamics{}, StrokeInput{{24.0, 24.0}, 1.0});
    eng.extendTo(StrokeInput{{24.0, 24.0}, 1.0}); // same spot again (no extra travel)
    eng.flush(); // the walk lags one sample: lay the tail span before reading the pixels
    eng.composite();
    eng.end();
    const int a = alphaAt(img, 24, 24);
    CHECK(a > 110);
    CHECK(a < 140); // ~128, the 0.5 cap -- emphatically not 255
}

TEST_CASE("flow builds up across overlapping dabs but stays under opacity") {
    // Low flow, a stroke whose densely-spaced dabs overlap a point many times: alpha climbs well
    // above a single isolated dab yet never exceeds the opacity cap. (The engine is distance-based,
    // not an airbrush -- holding still deposits nothing; overlap comes from travel.)
    BrushParams p;
    p.diameter = 14.0;
    p.hardness = 1.0;
    p.flow = 0.25;
    p.opacity = 0.8;
    p.color = Color8{0, 0, 0, 255};

    const int oneDab = [&] {
        const Image o = paintOne(p, 48, 48, {24.0, 24.0});
        return static_cast<int>(alphaAt(o, 24, 24));
    }();

    Image img(48, 48);
    BrushEngine eng;
    eng.begin(48, 48, img, p, BrushDynamics{}, StrokeInput{{8.0, 24.0}, 1.0});
    eng.extendTo(StrokeInput{{40.0, 24.0}, 1.0}); // a pass straight through (24,24)
    eng.flush(); // the walk lags one sample: lay the tail span before reading the pixels
    eng.composite();
    eng.end();
    const int many = alphaAt(img, 24, 24);
    CHECK(many > oneDab);              // flow accumulates over the overlapping dabs
    CHECK(many <= 0.8 * 255 + 2);      // ... but never past the opacity cap
}

TEST_CASE("spacing places dabs along a stroke; dirty bounds frame it") {
    BrushParams p;
    p.diameter = 10.0;
    p.hardness = 0.9;
    p.spacing = 0.25; // a dab every ~2.5 px
    p.color = Color8{255, 255, 255, 255};

    Image img(80, 32);
    BrushEngine eng;
    eng.begin(80, 32, img, p, BrushDynamics{}, StrokeInput{{8.0, 16.0}, 1.0});
    eng.extendTo(StrokeInput{{72.0, 16.0}, 1.0}); // a horizontal stroke across the canvas
    eng.flush(); // the walk lags one sample: lay the tail span before reading the pixels
    const Rect dirty = eng.composite();
    eng.end();

    // The whole stroke line is painted (midpoints + the release point the press dab never reached).
    CHECK(alphaAt(img, 40, 16) > 0);
    CHECK(alphaAt(img, 64, 16) > 0);
    CHECK(alphaAt(img, 72, 16) > 0); // the release point is covered by the last dab's radius
    // Dirty rect spans the stroke horizontally and is a few px tall around the centre line.
    CHECK(dirty.x <= 4.0);
    CHECK(dirty.right() >= 73.0); // last dab quantises just shy of 72; its 5 px radius reaches past
    CHECK(dirty.y > 8.0);
    CHECK(dirty.bottom() < 24.0);
}

TEST_CASE("stroke -> one region SetLayerPixelsCommand round-trips through undo/redo") {
    // Mirrors VulkanCanvas's commit strategy (S60-c): the engine paints directly onto the live layer
    // image and keeps its own bounded pristine snapshot; the canvas reads out just the stroke's
    // bounding box, restore()s the layer, and pushes ONE region SetLayerPixelsCommand -- so the
    // whole stroke is a single undo step that captures the correct pre-stroke pixels.
    using mosaic::core::AddLayerCommand;
    using mosaic::core::Document;
    using mosaic::core::LayerId;
    using mosaic::core::RasterLayer;
    using mosaic::core::SetLayerPixelsCommand;

    Document doc(40, 40);
    auto raster = doc.makeRaster("Paint");
    const LayerId id = raster->id();
    doc.commands().push(std::make_unique<AddLayerCommand>(doc.root().id(), 0, std::move(raster)));
    auto* layer = doc.find(id)->as<RasterLayer>();
    REQUIRE(layer != nullptr);
    CHECK(alphaAt(layer->image(), 20, 20) == 0); // starts transparent

    BrushParams p;
    p.diameter = 12.0;
    p.color = Color8{255, 0, 0, 255};

    // --- the canvas's stroke flow ---
    BrushEngine eng;
    eng.begin(40, 40, layer->image(), p, BrushDynamics{}, StrokeInput{{12.0, 20.0}, 1.0});
    eng.composite();
    eng.extendTo(StrokeInput{{28.0, 20.0}, 1.0});
    eng.flush(); // the walk lags one sample: lay the tail span before reading the pixels
    eng.composite();
    eng.end();
    CHECK(alphaAt(layer->image(), 20, 20) == 255); // the live preview is on the layer

    const Rect db = eng.dirtyBounds();
    Image region = mosaic::common::copyRegion(layer->image(), static_cast<long>(db.x),
                                              static_cast<long>(db.y),
                                              static_cast<std::uint32_t>(db.w),
                                              static_cast<std::uint32_t>(db.h));
    eng.restore(); // revert so the command captures the right "old" region
    CHECK(alphaAt(layer->image(), 20, 20) == 0); // restored to pristine
    layer->invalidateContentBounds();
    doc.commands().push(std::make_unique<SetLayerPixelsCommand>(
        id, std::move(region), static_cast<long>(db.x), static_cast<long>(db.y)));

    // Re-applied by the command: the stroke line is opaque red across the canvas.
    CHECK(alphaAt(layer->image(), 20, 20) == 255);
    CHECK(colorAt(layer->image(), 20, 20) == Color8{255, 0, 0, 255});

    doc.commands().undo();
    CHECK(alphaAt(doc.find(id)->as<RasterLayer>()->image(), 20, 20) == 0); // back to transparent

    doc.commands().redo();
    CHECK(alphaAt(doc.find(id)->as<RasterLayer>()->image(), 20, 20) == 255); // and back to painted
}

TEST_CASE("coverage() records the brushed region (the Inpaint brush's hole mask, S39)") {
    BrushParams p;
    p.diameter = 10.0;
    p.color = Color8{220, 40, 40, 255}; // the inpaint overlay colour; coverage is colour-independent
    Image img(40, 40);
    BrushEngine eng;
    eng.begin(40, 40, img, p, BrushDynamics{}, StrokeInput{{20.0, 20.0}, 1.0});
    eng.end();

    CHECK(eng.width() == 40);
    CHECK(eng.height() == 40);
    // Coverage is the stroke's bounded footprint placed at (originX, originY); index relative to it.
    const auto& cov = eng.coverage();
    const std::uint32_t cw = eng.coverageWidth();
    const std::int32_t ox = eng.coverageOriginX();
    const std::int32_t oy = eng.coverageOriginY();
    REQUIRE(cov.size() == static_cast<std::size_t>(cw) * eng.coverageHeight());
    CHECK(cov[static_cast<std::size_t>(20 - oy) * cw + (20 - ox)] > 0.5f); // brushed at the centre
    // A corner of the footprint well outside the dab is untouched.
    CHECK(cov[0] == 0.0f);
}

TEST_CASE("the working rect stays bounded on a large document (S60-c)") {
    BrushParams p;
    p.diameter = 12.0;
    Image img(5000, 8000); // a big layer; begin() must NOT allocate it whole
    BrushEngine eng;
    eng.begin(img.width, img.height, img, p, BrushDynamics{}, StrokeInput{{2000.0, 3000.0}, 1.0});
    eng.composite();
    eng.extendTo(StrokeInput{{2100.0, 3050.0}, 1.0}); // a short stroke
    eng.flush(); // the walk lags one sample: lay the tail span before reading the pixels
    eng.composite();
    eng.end();

    // The coverage footprint covers the stroke + a tile of padding -- a tiny fraction of 40 MP.
    CHECK(eng.coverageWidth() <= 512u);
    CHECK(eng.coverageHeight() <= 512u);
    CHECK(eng.coverage().size() < static_cast<std::size_t>(img.width) * img.height / 50);
    // The footprint contains the stroke.
    CHECK(eng.coverageOriginX() <= 2000);
    CHECK(eng.coverageOriginY() <= 3000);
    // Only the stroke region was written; a far-away pixel is still pristine.
    CHECK(alphaAt(img, 0, 0) == 0);
    CHECK(alphaAt(img, 2050, 3025) > 0); // somewhere along the stroke is painted
}

TEST_CASE("Inpaint glue: brushed coverage -> hole Selection -> engine fills the hole") {
    using mosaic::core::Selection;
    namespace inpaint = mosaic::core::inpaint;

    // A flat field with a small contrasting blemish in the middle.
    const std::uint32_t W = 48, H = 48;
    Image img(W, H);
    img.fill(Color8{100, 150, 200, 255});
    for (std::uint32_t y = 21; y < 27; ++y)
        for (std::uint32_t x = 21; x < 27; ++x)
            img.rgba[(static_cast<std::size_t>(y) * W + x) * 4 + 0] = 255, // make it red
                img.rgba[(static_cast<std::size_t>(y) * W + x) * 4 + 1] = 0,
                img.rgba[(static_cast<std::size_t>(y) * W + x) * 4 + 2] = 0;

    // Brush over the blemish and turn the coverage into a hole mask (the canvas's brushHoleMask).
    BrushParams p;
    p.diameter = 12.0;
    BrushEngine eng;
    eng.begin(W, H, img, p, BrushDynamics{}, StrokeInput{{24.0, 24.0}, 1.0});
    eng.end(); // no composite(): img keeps the blemish for the inpaint input below
    // Scatter the bounded coverage footprint into a document-sized hole mask (brushHoleMask).
    Selection hole(W, H);
    const std::uint32_t cw = eng.coverageWidth(), ch = eng.coverageHeight();
    const std::int32_t ox = eng.coverageOriginX(), oy = eng.coverageOriginY();
    bool any = false;
    for (std::uint32_t ly = 0; ly < ch; ++ly)
        for (std::uint32_t lx = 0; lx < cw; ++lx)
            if (eng.coverage()[static_cast<std::size_t>(ly) * cw + lx] > 0.1f) {
                hole.data()[static_cast<std::size_t>(oy + ly) * W + (ox + lx)] = 255;
                any = true;
            }
    REQUIRE(any);

    // Diffusion backend (deterministic): a constant-boundary Laplace solve fills the hole with the
    // surrounding colour. (The app defaults to offset-stats; this asserts the wiring, not the kernel.)
    inpaint::InpaintEngine engine = inpaint::makeDefaultEngine();
    REQUIRE(engine.setActiveBackend("pde"));
    const mosaic::common::ImageF in = mosaic::common::toFloat(img);
    const inpaint::InpaintResult res = engine.run(inpaint::InpaintRequest{in, hole, {}});
    REQUIRE(res.ok);
    const Image out = mosaic::common::toImage8(res.image);

    // The blemish centre is filled back toward the flat field, no longer pure red.
    const std::size_t c = (static_cast<std::size_t>(24) * W + 24) * 4;
    CHECK(out.rgba[c + 0] < 160); // red pulled down from 255
    CHECK(out.rgba[c + 2] > 150); // blue pulled up toward 200
    // A pixel well outside the hole is untouched.
    const std::size_t corner = 0;
    CHECK(out.rgba[corner + 0] == 100);
    CHECK(out.rgba[corner + 2] == 200);
}

TEST_CASE("a dab off the document edge clips without writing out of bounds") {
    BrushParams p;
    p.diameter = 20.0;
    p.color = Color8{10, 20, 30, 255};
    // Centre at the top-left corner: most of the tip is off-canvas; the in-bounds quarter paints.
    const Image img = paintOne(p, 32, 32, {0.0, 0.0});
    CHECK(alphaAt(img, 0, 0) > 0);
    CHECK(img.rgba.size() == static_cast<std::size_t>(32) * 32 * 4); // unchanged buffer size
}

// ---- The S31 mask-paint lane's proxy contract ----------------------------------------------------
// VulkanCanvas paints a layer MASK by handing the engine the coverage as an opaque-gray RGBA proxy
// and reading coverage back as luma(rgb) * alpha (Rec.709 integer weights -- the exact readout in
// vulkan_canvas.cpp's proxyCoverage). This pins the two directions of that contract: the Brush
// writes its color's gray (white reveals, black hides), and the Eraser's destination-out carves
// alpha, decaying coverage toward 0 (hides). No canvas/FLTK involved -- pure engine + readout.
namespace {
std::uint8_t maskLaneCoverage(const Image& proxy, int x, int y) {
    const std::size_t p = (static_cast<std::size_t>(y) * proxy.width + x) * 4;
    const unsigned luma =
        (54u * proxy.rgba[p] + 183u * proxy.rgba[p + 1] + 19u * proxy.rgba[p + 2]) >> 8;
    return static_cast<std::uint8_t>((luma * proxy.rgba[p + 3] + 127u) / 255u);
}
Image grayProxy(std::uint32_t w, std::uint32_t h, std::uint8_t v) {
    Image img(w, h);
    for (std::size_t i = 0, n = static_cast<std::size_t>(w) * h; i < n; ++i) {
        img.rgba[i * 4 + 0] = v;
        img.rgba[i * 4 + 1] = v;
        img.rgba[i * 4 + 2] = v;
        img.rgba[i * 4 + 3] = 255;
    }
    return img;
}
} // namespace

TEST_CASE("mask-lane proxy: a black dab hides, a white dab reveals (luma readout)") {
    // A revealed (white) mask sheet: paint black -> coverage drops to 0 under the dab core.
    Image reveal = grayProxy(32, 32, 255);
    BrushParams black;
    black.diameter = 12.0;
    black.hardness = 1.0;
    black.color = Color8{0, 0, 0, 255};
    BrushEngine eng;
    eng.begin(32, 32, reveal, black, BrushDynamics{}, StrokeInput{{16.0, 16.0}, 1.0});
    eng.composite();
    eng.end();
    CHECK(maskLaneCoverage(reveal, 16, 16) == 0);   // hidden under the dab
    CHECK(maskLaneCoverage(reveal, 2, 2) == 255);   // untouched sheet stays revealed

    // A hidden (black) sheet: paint white -> coverage climbs back to 255.
    Image hide = grayProxy(32, 32, 0);
    BrushParams white = black;
    white.color = Color8{255, 255, 255, 255};
    BrushEngine eng2;
    eng2.begin(32, 32, hide, white, BrushDynamics{}, StrokeInput{{16.0, 16.0}, 1.0});
    eng2.composite();
    eng2.end();
    CHECK(maskLaneCoverage(hide, 16, 16) == 255);
    CHECK(maskLaneCoverage(hide, 2, 2) == 0);
}

TEST_CASE("mask-lane proxy: the eraser carves coverage toward 0 (destination-out on alpha)") {
    Image reveal = grayProxy(32, 32, 255);
    BrushParams erase;
    erase.diameter = 12.0;
    erase.hardness = 1.0;
    erase.strokeMode = mosaic::core::brush::StrokeMode::Erase;
    BrushEngine eng;
    eng.begin(32, 32, reveal, erase, BrushDynamics{}, StrokeInput{{16.0, 16.0}, 1.0});
    eng.composite();
    eng.end();
    CHECK(maskLaneCoverage(reveal, 16, 16) == 0);  // erased = alpha 0 = hidden
    CHECK(maskLaneCoverage(reveal, 2, 2) == 255);  // beyond the dab: revealed
}
