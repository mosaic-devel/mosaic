#include "common/image.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/layer_effects.hpp"
#include "core/text/shaping.hpp"
#include "core/text/text_layer_render.hpp"
#include "core/text/text_model.hpp"
#include "platform/font_db.hpp"
#include "render/compositor.hpp"
#include "render/effect_primitives.hpp"
#include "render/layer_effects_render.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <numeric>
#include <vector>

using namespace mosaic;

namespace {

// CPU reference composite, no checkerboard (real alpha preserved), so the effect cases can assert
// exact pixel + alpha values.
common::Image flatten(const core::Document& doc) {
    const render::CompositeResult r = render::composite(doc, {}, render::Backend::Cpu);
    REQUIRE(r.ok);
    return r.image;
}

common::Color8 px(const common::Image& img, std::uint32_t x, std::uint32_t y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

// Fill an axis-aligned square [x0,x0+s) x [y0,y0+s) of a raster's 8-bit image with `c`.
void fillSquare(core::RasterLayer& r, int x0, int y0, int s, common::Color8 c) {
    common::Image& img = r.image();
    for (int y = y0; y < y0 + s && y < static_cast<int>(img.height); ++y)
        for (int x = x0; x < x0 + s && x < static_cast<int>(img.width); ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
            img.rgba[p] = c.r;
            img.rgba[p + 1] = c.g;
            img.rgba[p + 2] = c.b;
            img.rgba[p + 3] = c.a;
        }
    r.invalidateContentBounds();
}

core::StrokeEffect solidStroke(float width, core::StrokeEffect::Align align, common::ColorF color) {
    core::StrokeEffect s;
    s.width = width;
    s.align = align;
    s.paint = core::vec::SolidPaint{color};
    s.enabled = true;
    return s;
}

// A two-stop linear gradient a(0)->b(1), identity transform (runs across the normalised content box
// left->right), Pad spread -- the layer-effects renderer's default gradient orientation.
core::vec::Gradient linearGradient(common::ColorF a, common::ColorF b) {
    core::vec::Gradient g;
    g.type = core::vec::GradientType::Linear;
    g.stops = {{0.0, a}, {1.0, b}};
    g.spread = core::vec::SpreadMethod::Pad;
    return g;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Model (core/layer_effects.hpp)
// ---------------------------------------------------------------------------------------------
TEST_CASE("LayerEffects::empty short-circuits an untouched stack") {
    core::LayerEffects fx;
    CHECK(fx.empty());  // default: fillOpacity 1, all effects disabled/empty

    fx.strokes.push_back(solidStroke(3.0f, core::StrokeEffect::Align::Outside, {1, 0, 0, 1}));
    CHECK_FALSE(fx.empty());
    fx.strokes.back().enabled = false;
    CHECK(fx.empty());  // a disabled stroke does not count

    fx.fillOpacity = 0.5f;
    CHECK_FALSE(fx.empty());
    fx.fillOpacity = 1.0f;
    CHECK(fx.empty());

    fx.dropShadows.emplace_back();  // default-disabled
    CHECK(fx.empty());
    fx.dropShadows.back().enabled = true;
    CHECK_FALSE(fx.empty());
}

TEST_CASE("effectsOutwardReach sums concentric outward strokes, ignores inside") {
    core::LayerEffects fx;
    CHECK(core::effectsOutwardReach(fx) == doctest::Approx(0.0f));

    fx.strokes.push_back(solidStroke(3.0f, core::StrokeEffect::Align::Outside, {0, 0, 0, 1}));
    fx.strokes.push_back(solidStroke(5.0f, core::StrokeEffect::Align::Outside, {0, 0, 0, 1}));
    CHECK(core::effectsOutwardReach(fx) == doctest::Approx(8.0f));  // concentric: 3 + 5

    fx.strokes.push_back(solidStroke(4.0f, core::StrokeEffect::Align::Inside, {0, 0, 0, 1}));
    CHECK(core::effectsOutwardReach(fx) == doctest::Approx(8.0f));  // inside adds no outward reach

    core::LayerEffects glow;
    glow.outerGlow.enabled = true;
    glow.outerGlow.size = 10.0f;
    glow.outerGlow.choke = 2.0f;
    CHECK(core::effectsOutwardReach(glow) == doctest::Approx(12.0f));
}

TEST_CASE("Layer::effectsBounds dilates content by the outward reach") {
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {0, 0, 255, 255});
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    const auto content = r.contentBounds();
    REQUIRE(content.has_value());
    CHECK(r.effectsBounds() == content);  // no effects -> identical to contentBounds

    core::LayerEffects fx;
    fx.strokes.push_back(solidStroke(5.0f, core::StrokeEffect::Align::Outside, {1, 0, 0, 1}));
    r.setEffects(fx);
    const auto eb = r.effectsBounds();
    REQUIRE(eb.has_value());
    CHECK(eb->x == doctest::Approx(content->x - 5.0));
    CHECK(eb->y == doctest::Approx(content->y - 5.0));
    CHECK(eb->w == doctest::Approx(content->w + 10.0));
    CHECK(eb->h == doctest::Approx(content->h + 10.0));

    // An inside-only stroke grows nothing.
    core::LayerEffects inside;
    inside.strokes.push_back(solidStroke(5.0f, core::StrokeEffect::Align::Inside, {1, 0, 0, 1}));
    r.setEffects(inside);
    CHECK(r.effectsBounds() == content);
}

// ---------------------------------------------------------------------------------------------
// Primitives (render/effect_primitives.hpp)
// ---------------------------------------------------------------------------------------------
TEST_CASE("signedDistanceField is negative inside, positive outside, ~0 at the edge") {
    const int w = 20, h = 20;
    std::vector<float> alpha(static_cast<std::size_t>(w) * h, 0.0f);
    for (int y = 6; y <= 13; ++y)  // an 8x8 opaque square
        for (int x = 6; x <= 13; ++x) alpha[static_cast<std::size_t>(y) * w + x] = 1.0f;

    const std::vector<float> sd = render::fx::signedDistanceField(alpha, w, h);
    const auto at = [&](int x, int y) { return sd[static_cast<std::size_t>(y) * w + x]; };

    CHECK(at(9, 9) < 0.0f);                          // interior
    CHECK(at(9, 9) == doctest::Approx(-4.0f));       // nearest edge is 4 px away
    CHECK(at(9, 3) == doctest::Approx(3.0f));        // 3 px above the top edge (y=6)
    CHECK(at(9, 0) == doctest::Approx(6.0f));        // 6 px above
    CHECK(at(3, 9) == doctest::Approx(3.0f));        // 3 px left of the left edge (x=6)
    CHECK(at(9, 3) > 0.0f);                          // exterior sign
}

TEST_CASE("gaussianBlur preserves a constant field and conserves impulse energy") {
    const int w = 11, h = 11;
    std::vector<float> flat(static_cast<std::size_t>(w) * h, 1.0f);
    render::fx::gaussianBlur(flat, w, h, 1.5f);
    for (float v : flat) CHECK(v == doctest::Approx(1.0f).epsilon(1e-4));

    std::vector<float> impulse(static_cast<std::size_t>(w) * h, 0.0f);
    impulse[static_cast<std::size_t>(5) * w + 5] = 1.0f;
    render::fx::gaussianBlur(impulse, w, h, 1.2f);
    const float sum = std::accumulate(impulse.begin(), impulse.end(), 0.0f);
    CHECK(sum == doctest::Approx(1.0f).epsilon(1e-3));           // normalised
    CHECK(impulse[static_cast<std::size_t>(5) * w + 5] < 1.0f);  // spread out
    CHECK(impulse[static_cast<std::size_t>(5) * w + 5] > 0.0f);
    // Symmetric about the centre.
    CHECK(impulse[static_cast<std::size_t>(5) * w + 4] ==
          doctest::Approx(impulse[static_cast<std::size_t>(5) * w + 6]));
    CHECK(impulse[static_cast<std::size_t>(4) * w + 5] ==
          doctest::Approx(impulse[static_cast<std::size_t>(6) * w + 5]));
}

TEST_CASE("boxBlurApprox preserves a constant field and smooths an impulse") {
    const int w = 21, h = 21;
    std::vector<float> flat(static_cast<std::size_t>(w) * h, 0.5f);
    render::fx::boxBlurApprox(flat, w, h, 2.0f);
    for (float v : flat) CHECK(v == doctest::Approx(0.5f).epsilon(1e-4));

    std::vector<float> impulse(static_cast<std::size_t>(w) * h, 0.0f);
    impulse[static_cast<std::size_t>(10) * w + 10] = 1.0f;
    render::fx::boxBlurApprox(impulse, w, h, 2.0f);
    const float sum = std::accumulate(impulse.begin(), impulse.end(), 0.0f);
    CHECK(sum == doctest::Approx(1.0f).epsilon(1e-2));            // ~energy-conserving
    CHECK(impulse[static_cast<std::size_t>(10) * w + 10] < 1.0f);  // spread
    CHECK(impulse[static_cast<std::size_t>(10) * w + 10] > 0.0f);
}

// ---------------------------------------------------------------------------------------------
// Compositor seam (render/layer_effects_render.cpp via the renderLayer wrapper)
// ---------------------------------------------------------------------------------------------
TEST_CASE("an empty / cleared effect stack composites byte-identically") {
    core::Document doc(48, 48);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 12, 12, 24, {30, 120, 220, 255});
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    const common::Image baseline = flatten(doc);

    r.setEffects(core::LayerEffects{});  // present but empty()
    CHECK(flatten(doc) == baseline);

    r.clearEffects();
    CHECK(flatten(doc) == baseline);
}

TEST_CASE("an outside stroke paints a ring beyond the layer edge, interior intact") {
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {0, 0, 255, 255});  // square x,y in [20,44)
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    core::LayerEffects fx;
    fx.strokes.push_back(solidStroke(4.0f, core::StrokeEffect::Align::Outside, {1, 0, 0, 1}));
    r.setEffects(fx);

    const common::Image out = flatten(doc);
    CHECK(px(out, 32, 32) == common::Color8{0, 0, 255, 255});  // interior unchanged
    CHECK(px(out, 18, 32) == common::Color8{255, 0, 0, 255});  // 2 px outside the left edge -> red
    CHECK(px(out, 15, 32).a == 0);                             // 5 px out -> beyond the band
}

TEST_CASE("bevel and stroke share ONE 3x field, and both still draw their own tier") {
    // The bevel and the stroke pass each used to build `signedDistanceFieldAA(alpha, rw, rh, 3)`
    // for themselves. It is a pure function of arguments neither of them changes, and it is the
    // most expensive single thing in the stack -- 3x supersampling is 9x the texels and
    // signedDistanceField runs TWO exact Euclidean transforms over them -- so a layer carrying
    // both, which is what a styled headline is, paid for it twice. It is now built once, lazily.
    //
    // What that could break is one tier being handed a field the other has finished with, so the
    // test asserts BOTH tiers still land: the stroke ring outside the square, the bevel's
    // light/dark shading inside it, and the untouched interior between them. A share that handed
    // the strokes an empty field would leave the ring transparent; one that handed the bevel a
    // stale field would leave the shading flat.
    //
    // ⚠ Mutation is a COMPILE error, not a test: applyBevel takes the field by const reference.
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {0, 0, 255, 255}); // square x,y in [20,44)
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    // Stroke only -- the reference for the ring.
    core::LayerEffects strokeOnly;
    strokeOnly.strokes.push_back(
        solidStroke(4.0f, core::StrokeEffect::Align::Outside, {1, 0, 0, 1}));
    r.setEffects(strokeOnly);
    const common::Image ring = flatten(doc);
    REQUIRE(px(ring, 18, 32) == common::Color8{255, 0, 0, 255});

    // Bevel only -- the reference for the shading. A raked light must make the two opposite inner
    // edges differ from each other; if it did not, the check below could not tell the tier ran.
    core::LayerEffects bevelOnly;
    bevelOnly.bevel.enabled = true;
    bevelOnly.bevel.size = 6.0f;
    bevelOnly.bevel.depth = 2.0f;
    r.setEffects(bevelOnly);
    const common::Image shaded = flatten(doc);
    const common::Color8 litEdge = px(shaded, 32, 23);  // near the top inner edge
    const common::Color8 darkEdge = px(shaded, 32, 41); // near the bottom inner edge
    REQUIRE(litEdge != darkEdge);                       // the bevel really does shade

    // Both together: each tier must still produce what it produced alone. The stroke is outside
    // the silhouette and the bevel is clipped to it, so neither can overwrite the other's probe.
    core::LayerEffects both = bevelOnly;
    both.strokes = strokeOnly.strokes;
    r.setEffects(both);
    const common::Image out = flatten(doc);
    CHECK(px(out, 18, 32) == px(ring, 18, 32));   // the ring is still drawn
    CHECK(px(out, 15, 32).a == 0);                // and still ends where it did
    CHECK(px(out, 32, 23) == litEdge);            // the bevel's lit edge is still shaded
    CHECK(px(out, 32, 41) == darkEdge);           // ... and its dark edge too
    CHECK(px(out, 32, 32) == px(shaded, 32, 32)); // the deep interior matches the bevel-only run
}

TEST_CASE("the 3x supersampled distance field is a pure function of its input") {
    // The sharing above is only sound because two calls with the same arguments are the same
    // field. Pinned directly, because "it is pure" is the whole argument for building it once.
    std::vector<float> alpha(48 * 40, 0.0f);
    for (int y = 8; y < 32; ++y)
        for (int x = 10; x < 38; ++x) // a rectangle with one bitten-out corner, so the field
            if (!(x > 30 && y > 24))  // varies in both axes and along a diagonal
                alpha[static_cast<std::size_t>(y) * 48 + x] = 1.0f;
    const std::vector<float> a = render::fx::signedDistanceFieldAA(alpha, 48, 40, 3);
    const std::vector<float> b = render::fx::signedDistanceFieldAA(alpha, 48, 40, 3);
    REQUIRE(a.size() == static_cast<std::size_t>(48) * 40);
    CHECK(a == b);
    // ... and it is not trivially constant, or the equality above would prove nothing.
    CHECK(*std::min_element(a.begin(), a.end()) < -1.0f);
    CHECK(*std::max_element(a.begin(), a.end()) > 1.0f);
}

TEST_CASE("an inside stroke stays within the layer, leaving the far interior") {
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {0, 0, 255, 255});
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    core::LayerEffects fx;
    fx.strokes.push_back(solidStroke(4.0f, core::StrokeEffect::Align::Inside, {1, 0, 0, 1}));
    r.setEffects(fx);

    const common::Image out = flatten(doc);
    CHECK(px(out, 22, 32) == common::Color8{255, 0, 0, 255});  // 2 px inside the left edge -> red
    CHECK(px(out, 32, 32) == common::Color8{0, 0, 255, 255});  // deep interior stays blue
    CHECK(px(out, 17, 32).a == 0);                             // nothing painted outside
}

TEST_CASE("concentric outside strokes stack outward in order") {
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {255, 255, 255, 255});
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    core::LayerEffects fx;
    fx.strokes.push_back(solidStroke(3.0f, core::StrokeEffect::Align::Outside, {1, 0, 0, 1}));  // inner
    fx.strokes.push_back(solidStroke(3.0f, core::StrokeEffect::Align::Outside, {0, 1, 0, 1}));  // outer
    r.setEffects(fx);

    const common::Image out = flatten(doc);
    CHECK(px(out, 18, 32) == common::Color8{255, 0, 0, 255});  // ~2 px out: inner (red) ring
    CHECK(px(out, 16, 32) == common::Color8{0, 255, 0, 255});  // ~4 px out: outer (green) ring
    CHECK(px(out, 13, 32).a == 0);                             // ~7 px out: past both
}

TEST_CASE("an outside stroke backs the content's anti-aliased rim (no seam)") {
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {0, 0, 255, 255});
    // Simulate an anti-aliased inner edge: the left content column is semi-transparent but still
    // inside the 0.5 contour. A naive over-composited outside stroke leaves it at ~59% coverage (the
    // visible seam); the below-composite must back it to full.
    common::Image& img = layer->image();
    for (int y = 20; y < 44; ++y) img.rgba[(static_cast<std::size_t>(y) * img.width + 20) * 4 + 3] = 150;
    layer->invalidateContentBounds();
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    core::LayerEffects fx;
    fx.strokes.push_back(solidStroke(4.0f, core::StrokeEffect::Align::Outside, {1, 0, 0, 1}));
    r.setEffects(fx);

    const common::Image out = flatten(doc);
    CHECK(px(out, 20, 32).a == 255);  // the semi-transparent rim is backed -> full coverage, no seam
    CHECK(px(out, 32, 32) == common::Color8{0, 0, 255, 255});  // interior: no stroke bleed-through
}

TEST_CASE("fill-opacity dims the layer's own pixels but not its stroke") {
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {0, 0, 255, 255});
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    core::LayerEffects fx;
    fx.fillOpacity = 0.5f;
    fx.strokes.push_back(solidStroke(4.0f, core::StrokeEffect::Align::Outside, {1, 0, 0, 1}));
    r.setEffects(fx);

    const common::Image out = flatten(doc);
    const common::Color8 interior = px(out, 32, 32);
    CHECK(interior.b == 255);            // colour intact
    CHECK(interior.a == doctest::Approx(128).epsilon(0.02));  // coverage halved
    CHECK(px(out, 18, 32) == common::Color8{255, 0, 0, 255});  // stroke stays full strength
}

// ---------------------------------------------------------------------------------------------
// Overlays (LE-c): colour + gradient, clipped to the shape, composited with blend + opacity.
// ---------------------------------------------------------------------------------------------
TEST_CASE("a solid colour overlay recolours the shape, clipped to its coverage") {
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {0, 0, 255, 255});  // blue square in [20,44)
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    core::LayerEffects fx;
    fx.colorOverlay.paint = core::vec::SolidPaint{{1, 0, 0, 1}};  // opaque red
    fx.colorOverlay.enabled = true;
    r.setEffects(fx);

    const common::Image out = flatten(doc);
    CHECK(px(out, 32, 32) == common::Color8{255, 0, 0, 255});  // interior recoloured red
    CHECK(px(out, 10, 10).a == 0);                             // outside the shape: untouched
}

TEST_CASE("a colour overlay's opacity blends toward the layer colour") {
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {0, 0, 255, 255});
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    core::LayerEffects fx;
    fx.colorOverlay.paint = core::vec::SolidPaint{{1, 0, 0, 1}};
    fx.colorOverlay.opacity = 0.5f;  // half red over blue -> purple
    fx.colorOverlay.enabled = true;
    r.setEffects(fx);

    const common::Color8 c = px(flatten(doc), 32, 32);
    CHECK(c.r == doctest::Approx(128).epsilon(0.03));
    CHECK(c.b == doctest::Approx(128).epsilon(0.03));
    CHECK(c.a == 255);
}

TEST_CASE("a gradient overlay varies across the shape (left stop -> right stop)") {
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {0, 0, 255, 255});  // [20,44), 24 px wide
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    core::LayerEffects fx;
    fx.gradientOverlay.paint = linearGradient({1, 0, 0, 1}, {0, 1, 0, 1});  // red -> green
    fx.gradientOverlay.enabled = true;
    r.setEffects(fx);

    const common::Image out = flatten(doc);
    const common::Color8 left = px(out, 22, 32);   // near the left edge -> red-dominant
    const common::Color8 right = px(out, 41, 32);  // near the right edge -> green-dominant
    CHECK(left.r > left.g);
    CHECK(right.g > right.r);
    CHECK(left.a == 255);
    CHECK(right.a == 255);
}

TEST_CASE("a gradient stroke paints (LE-c) where LE-a left it transparent") {
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {0, 0, 255, 255});
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    core::StrokeEffect s;
    s.width = 4.0f;
    s.align = core::StrokeEffect::Align::Outside;
    s.paint = linearGradient({1, 0, 0, 1}, {0, 1, 0, 1});  // red (left) -> green (right)
    s.enabled = true;
    core::LayerEffects fx;
    fx.strokes.push_back(s);
    r.setEffects(fx);

    const common::Image out = flatten(doc);
    const common::Color8 leftRing = px(out, 18, 32);   // 2 px past the left edge (Pad -> red stop)
    const common::Color8 rightRing = px(out, 45, 32);  // 2 px past the right edge (Pad -> green stop)
    CHECK(leftRing.a > 0);  // a gradient stroke now draws (LE-a drew nothing here)
    CHECK(leftRing.r > leftRing.g);
    CHECK(rightRing.a > 0);
    CHECK(rightRing.g > rightRing.r);
}

// ---------------------------------------------------------------------------------------------
// Color8 <-> ColorF bridge (common/image.hpp) + SetLayerEffectsCommand (core/commands.hpp)
// ---------------------------------------------------------------------------------------------
TEST_CASE("Color8 <-> ColorF bridge round-trips") {
    for (common::Color8 c : {common::Color8{0, 0, 0, 255}, common::Color8{255, 255, 255, 255},
                             common::Color8{255, 0, 128, 200}, common::Color8{17, 200, 99, 255}}) {
        CHECK(common::toColor8(common::toColorF(c)) == c);
    }
    const common::ColorF f = common::toColorF({255, 0, 128, 255});
    CHECK(f.r == doctest::Approx(1.0f));
    CHECK(f.g == doctest::Approx(0.0f));
    CHECK(f.b == doctest::Approx(128.0f / 255.0f));
    CHECK(common::toColor8({2.0f, -1.0f, 0.5f, 1.0f}) == common::Color8{255, 0, 128, 255});  // clamps
}

TEST_CASE("SetLayerEffectsCommand applies, undoes, and coalesces") {
    core::Document doc(32, 32);
    auto layer = doc.makeRaster("r");
    const core::LayerId id = layer->id();
    doc.root().addOnTop(std::move(layer));

    core::LayerEffects a;
    a.strokes.push_back(solidStroke(3.0f, core::StrokeEffect::Align::Outside, {1, 0, 0, 1}));
    doc.commands().push(std::make_unique<core::SetLayerEffectsCommand>(id, a));
    REQUIRE(doc.find(id)->hasEffects());
    CHECK(doc.find(id)->effects().strokes.at(0).width == doctest::Approx(3.0f));

    doc.commands().undo();
    CHECK_FALSE(doc.find(id)->hasEffects());  // back to none (was nullopt)
    doc.commands().redo();
    CHECK(doc.find(id)->hasEffects());

    // Two same-id / same-coalesce pushes collapse to one undo step (a slider drag).
    core::LayerEffects b = a;
    b.strokes.at(0).width = 7.0f;
    core::LayerEffects c = a;
    c.strokes.at(0).width = 11.0f;
    doc.commands().push(std::make_unique<core::SetLayerEffectsCommand>(id, b, /*coalesce=*/42));
    doc.commands().push(std::make_unique<core::SetLayerEffectsCommand>(id, c, /*coalesce=*/42));
    CHECK(doc.find(id)->effects().strokes.at(0).width == doctest::Approx(11.0f));  // latest value
    doc.commands().undo();  // ONE undo unwinds the whole coalesced drag back to width 3
    CHECK(doc.find(id)->effects().strokes.at(0).width == doctest::Approx(3.0f));
}

TEST_CASE("pattern overlay is glued to the content corner, independent of canvas position") {
    // A layer-anchored pattern's tile origin AND its angle pivot sit at the CONTENT's top-left corner
    // (in layer-local space), so the same content shows the same pattern wherever it sits on the
    // canvas -- and the angle spins the pattern IN PLACE there, not along a big arc about the layer's
    // local origin. Two identical 16px squares placed at different offsets, same pattern + angle, must
    // render byte-identically over the content. (Regression for the "angle sweeps a big arc" report.)
    const auto renderAt = [](int ox, int oy) {
        core::Document doc(96, 96);
        auto layer = doc.makeRaster("r");
        fillSquare(*layer, ox, oy, 16, {0, 200, 0, 255});
        core::vec::ProceduralPattern pp;
        pp.kind = core::vec::ProceduralPattern::Kind::Lines;
        pp.scale = 6.0f;
        pp.weight = 0.5f;
        pp.angleDeg = 30.0f;
        pp.fg = {1.0f, 0.0f, 0.0f, 1.0f};
        pp.bg = {0.0f, 0.0f, 1.0f, 1.0f};
        core::LayerEffects fx;
        fx.patternOverlay.enabled = true;
        fx.patternOverlay.paint = core::vec::Pattern{pp};
        layer->setEffects(fx);
        doc.root().addOnTop(std::move(layer));
        return flatten(doc);
    };
    const common::Image a = renderAt(8, 8);
    const common::Image b = renderAt(60, 52);
    int mismatches = 0;
    for (int dy = 0; dy < 16; ++dy)
        for (int dx = 0; dx < 16; ++dx)
            if (!(px(a, 8 + dx, 8 + dy) == px(b, 60 + dx, 52 + dy))) ++mismatches;
    CHECK(mismatches == 0);
}

TEST_CASE("checker overlay starts on a full corner square with no top/left seam") {
    // The checker begins with a FULL square at the content's top-left corner (no half-cell offset that
    // reads as "starting mid-square"), and its cell-boundary AA does not leave a 1px fg/bg-blended seam
    // along the top/left edge. Regression for the "checker starts at an offset" report.
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 12, 12, 32, {0, 200, 0, 255});  // hard-edged square, content corner at (12,12)
    core::vec::ProceduralPattern cp;
    cp.kind = core::vec::ProceduralPattern::Kind::Checker;
    cp.scale = 8.0f;  // 8px cells
    cp.fg = {1.0f, 0.0f, 0.0f, 1.0f};  // red
    cp.bg = {0.0f, 0.0f, 1.0f, 1.0f};  // blue
    core::LayerEffects fx;
    fx.patternOverlay.enabled = true;
    fx.patternOverlay.paint = core::vec::Pattern{cp};
    layer->setEffects(fx);
    doc.root().addOnTop(std::move(layer));

    const common::Image out = flatten(doc);
    // The corner cell is a full solid-red square (a half-cell offset would show only a quarter of it).
    for (int dy = 0; dy < 6; ++dy)
        for (int dx = 0; dx < 6; ++dx)
            CHECK(px(out, 12 + dx, 12 + dy) == common::Color8{255, 0, 0, 255});
    // No seam: the first row/column inside the shape are solid red, not a purple fg/bg blend.
    for (int d = 0; d < 6; ++d) {
        CHECK(px(out, 12 + d, 12).b < 16);  // top edge stays pure red (no blue bleed)
        CHECK(px(out, 12, 12 + d).b < 16);  // left edge
    }
}

// ---------------------------------------------------------------------------------------------
// Shadows & glows (LE-e): the blur tier -- drop/inner shadow, outer/inner glow (doc §5.2, §5.4).
// ---------------------------------------------------------------------------------------------
TEST_CASE("a disabled drop shadow keeps the stack empty() -> byte-identical") {
    // The short-circuit guard for the shadow/glow tier: a present-but-disabled shadow must not cost
    // anything nor change a pixel (mirrors the LE-a empty()-stack case for the new fields).
    core::Document doc(48, 48);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 12, 12, 24, {30, 120, 220, 255});
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    const common::Image baseline = flatten(doc);

    core::LayerEffects fx;
    fx.dropShadows.emplace_back();  // default-disabled
    fx.innerShadows.emplace_back(); // default-disabled
    // outerGlow / innerGlow default-disabled with NoPaint
    REQUIRE(fx.empty());
    r.setEffects(fx);
    CHECK(flatten(doc) == baseline);
}

TEST_CASE("a drop shadow inks below-and-right of the shape, interior intact") {
    // Default angle 120 (light upper-left) drops the shadow to the lower-RIGHT. Pin the direction:
    // the shadow inks below the bottom edge, and NOT above the top edge; the shape itself is untouched.
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {0, 0, 255, 255});  // blue square, x,y in [20,44)
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    core::LayerEffects fx;
    core::ShadowEffect sh;  // defaults: black, 0.75, Multiply, angle 120, distance 6, spread 0, size 6
    sh.enabled = true;
    fx.dropShadows.push_back(sh);
    r.setEffects(fx);

    const common::Image out = flatten(doc);
    CHECK(px(out, 32, 32) == common::Color8{0, 0, 255, 255});  // interior unchanged (shadow is UNDER it)
    const common::Color8 belowPx = px(out, 30, 47);  // ~3 px below the bottom edge -> in the shadow
    CHECK(belowPx.a > 100);                           // a solid shadow lands here
    CHECK(belowPx.r < 40);                            // and it is dark (black shadow)
    CHECK(belowPx.g < 40);
    CHECK(belowPx.b < 40);
    CHECK(px(out, 30, 16).a == 0);  // 4 px ABOVE the top edge (opposite the offset) -> no shadow
    // Directionality: the bottom edge is more shadowed than the (light-facing) top edge.
    CHECK(px(out, 30, 46).a > px(out, 30, 18).a);
}

TEST_CASE("a drop shadow's spread fattens the solid core") {
    // Higher spread dilates the core before the blur, so at a fixed point just outside the shape the
    // shadow reads more solid (higher alpha) than with no spread.
    const auto shadowAlphaAt = [](float spread, std::uint32_t x, std::uint32_t y) {
        core::Document doc(64, 64);
        auto layer = doc.makeRaster("r");
        fillSquare(*layer, 20, 20, 24, {0, 0, 255, 255});
        core::LayerEffects fx;
        core::ShadowEffect sh;
        sh.enabled = true;
        sh.distance = 0.0f;  // no offset, so both cases probe the same geometry
        sh.size = 6.0f;
        sh.spread = spread;
        fx.dropShadows.push_back(sh);
        layer->setEffects(fx);
        doc.root().addOnTop(std::move(layer));
        return px(flatten(doc), x, y).a;
    };
    // 3 px left of the left edge (x=20): the fattened core reaches further out.
    CHECK(shadowAlphaAt(4.0f, 17, 32) > shadowAlphaAt(0.0f, 17, 32));
}

TEST_CASE("an inner shadow hugs the light-facing interior edge, clipped to the shape") {
    // Inner shadow: dark band on the side TOWARD the light (top-left with the default angle), fading
    // to nothing on the far (bottom-right) interior; never inks outside the silhouette.
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {0, 0, 255, 255});  // blue square [20,44)
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    core::LayerEffects fx;
    core::ShadowEffect sh;  // black, Multiply, angle 120, distance 6, size 6
    sh.enabled = true;
    fx.innerShadows.push_back(sh);
    r.setEffects(fx);

    const common::Image out = flatten(doc);
    const common::Color8 topLeft = px(out, 22, 22);   // just inside the top-left corner
    const common::Color8 botRight = px(out, 41, 41);  // just inside the bottom-right corner
    const common::Color8 center = px(out, 32, 32);
    CHECK(topLeft.b < center.b);                       // top-left is shadowed (less blue than centre)
    CHECK(botRight.b > topLeft.b);                     // bottom-right stays bright (far from the shadow)
    CHECK(center.a == 255);                            // still fully covered
    CHECK(px(out, 14, 32).a == 0);                     // OUTSIDE the shape -> untouched (clipped)
}

TEST_CASE("an outer glow extends a coloured halo outward, interior intact") {
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {0, 0, 255, 255});  // blue square [20,44)
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    core::LayerEffects fx;
    fx.outerGlow.enabled = true;
    fx.outerGlow.paint = core::vec::SolidPaint{{1, 0, 0, 1}};  // red glow
    fx.outerGlow.size = 10.0f;
    fx.outerGlow.choke = 0.0f;
    r.setEffects(fx);

    const common::Image out = flatten(doc);
    CHECK(px(out, 32, 32) == common::Color8{0, 0, 255, 255});  // interior: glow is UNDER the content
    const common::Color8 near = px(out, 18, 32);  // 2 px outside the left edge -> in the halo
    CHECK(near.a > 0);
    CHECK(near.r > near.g);  // reddish
    CHECK(near.r > near.b);
    // The halo decays outward: closer to the edge is stronger than further away.
    CHECK(px(out, 18, 32).a > px(out, 14, 32).a);
}

TEST_CASE("an inner glow brightens inside the edge (Edge source), clipped to the shape") {
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {0, 0, 60, 255});  // dark square so a bright glow reads
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    core::LayerEffects fx;
    fx.innerGlow.enabled = true;
    fx.innerGlow.paint = core::vec::SolidPaint{{1, 1, 0, 1}};  // yellow glow
    fx.innerGlow.source = core::GlowEffect::Source::Edge;
    fx.innerGlow.size = 8.0f;
    r.setEffects(fx);

    const common::Image out = flatten(doc);
    const common::Color8 edge = px(out, 22, 22);    // just inside the top-left -> brightened
    const common::Color8 center = px(out, 32, 32);  // deep interior -> little glow (Edge source)
    CHECK(edge.r > center.r + 20);   // the edge is markedly more yellow than the centre
    CHECK(edge.a == 255);
    CHECK(px(out, 14, 32).a == 0);   // OUTSIDE the shape -> untouched (clipped)
}

TEST_CASE("an inner glow (Center source) brightens the interior, fading to the edges") {
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {0, 0, 60, 255});
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    core::LayerEffects fx;
    fx.innerGlow.enabled = true;
    fx.innerGlow.paint = core::vec::SolidPaint{{1, 1, 0, 1}};
    fx.innerGlow.source = core::GlowEffect::Source::Center;
    fx.innerGlow.size = 8.0f;
    r.setEffects(fx);

    const common::Image out = flatten(doc);
    const common::Color8 center = px(out, 32, 32);  // Center source -> brightest here
    const common::Color8 edge = px(out, 21, 21);    // right at the edge -> little glow
    CHECK(center.r > edge.r + 20);
    CHECK(px(out, 14, 32).a == 0);  // still clipped to the shape
}

TEST_CASE("a grouped layer's outside stroke is not clipped at the group buffer") {
    // Regression guard for effectsBounds growing a group's isolated buffer (docs §4): an unmasked
    // group sizes its buffer to the child content, which would clip an outside stroke without the
    // descendant-reach margin in groupLocalExtent.
    core::Document doc(64, 64);
    auto raster = doc.makeRaster("r");
    fillSquare(*raster, 20, 20, 24, {0, 0, 255, 255});
    core::RasterLayer& r = *raster;

    core::LayerEffects fx;
    fx.strokes.push_back(solidStroke(4.0f, core::StrokeEffect::Align::Outside, {1, 0, 0, 1}));
    r.setEffects(fx);

    auto group = doc.makeGroup("G");
    group->addOnTop(std::move(raster));
    doc.root().addOnTop(std::move(group));

    const common::Image out = flatten(doc);
    CHECK(px(out, 32, 32) == common::Color8{0, 0, 255, 255});  // interior
    CHECK(px(out, 17, 32) == common::Color8{255, 0, 0, 255});  // stroke survives the group boundary
}

// ---------------------------------------------------------------------------------------------
// ---- LE-f: Bevel & Emboss + Satin -- the shading tier (docs/layer-effects.md §5.5, §5.6).
// ---------------------------------------------------------------------------------------------
TEST_CASE("bevel highlights the light-facing inner edge and shadows the opposite, clipped to alpha") {
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {128, 128, 128, 255});  // mid-grey: shows both lighten AND darken
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    core::LayerEffects fx;
    fx.bevel.enabled = true;
    fx.bevel.style = core::BevelEffect::Style::InnerBevel;
    fx.bevel.size = 6.0f;
    fx.bevel.depth = 1.0f;
    fx.bevel.angleDeg = 120.0f;  // raked light from the upper-left (top + left edges face it)
    fx.bevel.altitudeDeg = 30.0f;
    fx.bevel.highlight = {1, 1, 1, 1};
    fx.bevel.highlightOpacity = 1.0f;
    fx.bevel.shadow = {0, 0, 0, 1};
    fx.bevel.shadowOpacity = 1.0f;
    r.setEffects(fx);

    const common::Image out = flatten(doc);
    const common::Color8 top = px(out, 32, 22);     // 2 px inside the TOP edge -> faces the light
    const common::Color8 bottom = px(out, 32, 42);  // 2 px inside the BOTTOM edge -> faces away
    const common::Color8 center = px(out, 32, 32);  // plateau (deeper than `size`) -> unshaded
    CHECK(top.r > 150);        // lit toward white (highlight)
    CHECK(bottom.r < 100);     // darkened toward black (shadow)
    CHECK(center == common::Color8{128, 128, 128, 255});  // flat interior byte-unchanged
    CHECK(px(out, 8, 8).a == 0);    // nothing painted outside the silhouette (top-left corner)
    CHECK(px(out, 55, 55).a == 0);  // ... nor bottom-right
}

TEST_CASE("bevel + satin present but disabled short-circuit to a byte-identical composite") {
    core::Document doc(48, 48);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 12, 12, 24, {40, 160, 90, 255});
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));
    const common::Image baseline = flatten(doc);

    core::LayerEffects fx;  // fields set, but enabled==false on both -> empty() -> today's exact path
    fx.bevel.style = core::BevelEffect::Style::Emboss;
    fx.bevel.size = 8.0f;
    fx.satin.color = {1, 0, 0, 1};
    fx.satin.distance = 20.0f;
    CHECK(fx.empty());
    r.setEffects(fx);
    CHECK(flatten(doc) == baseline);
}

TEST_CASE("satin lays a sheen inside the shape, clipped to the silhouette") {
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {0, 0, 255, 255});  // blue square [20,44)
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    core::LayerEffects fx;
    fx.satin.enabled = true;
    fx.satin.color = {1, 0, 0, 1};  // red sheen
    fx.satin.opacity = 1.0f;
    fx.satin.blend = core::BlendMode::Normal;
    fx.satin.invert = true;      // abs-difference of the two offset copies
    fx.satin.angleDeg = 90.0f;   // a purely vertical offset (0, -distance)
    fx.satin.distance = 6.0f;
    fx.satin.size = 2.0f;        // a light blur, so the copies stay crisp
    r.setEffects(fx);

    const common::Image out = flatten(doc);
    // 2 px inside the top edge: one offset copy samples OUTSIDE the shape, so the copies differ ->
    // full sheen.
    CHECK(px(out, 32, 22).r > 200);
    CHECK(px(out, 32, 22).b < 80);
    // Deep interior: both offset copies land inside -> abs-difference ~0 -> no sheen (stays blue).
    CHECK(px(out, 32, 32) == common::Color8{0, 0, 255, 255});
    // Clipped to the silhouette -- nothing painted outside the alpha.
    CHECK(px(out, 10, 10).a == 0);
    CHECK(px(out, 52, 52).a == 0);
}

TEST_CASE("satin with invert off (clamped sum) sheens the interior, not just the interference band") {
    core::Document doc(64, 64);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 20, 20, 24, {0, 0, 255, 255});
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    core::LayerEffects fx;
    fx.satin.enabled = true;
    fx.satin.color = {1, 0, 0, 1};
    fx.satin.opacity = 1.0f;
    fx.satin.blend = core::BlendMode::Normal;
    fx.satin.invert = false;     // clamped SUM of the two copies -> saturated across the interior
    fx.satin.angleDeg = 90.0f;
    fx.satin.distance = 6.0f;
    fx.satin.size = 2.0f;
    r.setEffects(fx);

    const common::Image out = flatten(doc);
    // Deep interior: both copies ~1 -> sum clamps to 1 -> full sheen (the invert==true case left it
    // blue; this is the discriminating difference).
    CHECK(px(out, 32, 32).r > 200);
    CHECK(px(out, 10, 10).a == 0);  // still clipped to the silhouette
}

// ---------------------------------------------------------------------------------------------
// S30-e (docs/type-tool.md §12): 3D text consumes its overlays in the extrude lanes -- the
// compositor must NOT apply them again (a second application over the projected rectangle is
// exactly the smear §12 forbids), while shadows/strokes still run on the composited result.
// ---------------------------------------------------------------------------------------------
TEST_CASE("3D text overlays apply once, per face; shadows still composite; flat text unchanged") {
    mosaic::platform::FontDB db;
    if (db.families().empty()) return;
    core::text::FontRef probe;
    probe.family = db.defaultFamily();
    if (!db.resolve(probe)) return;
    core::text::TextShaper shaper;

    core::Document doc(240, 120);
    auto* tl = doc.root().addOnTop(doc.makeText("T")).as<core::TextLayer>();
    REQUIRE(tl != nullptr);
    core::text::CharStyle st;
    st.font.family = db.defaultFamily();
    st.sizePx = 48.0f;
    // RED is the run's own paint: a 3D solid shades with the layer's colour (§10.4), so the
    // "red material" this case is about is set the same way a flat block's colour would be.
    st.setSolidFill({1, 0, 0, 1});
    core::text::TextBlock solid = core::text::makeBlock("HH", st);
    solid.extrude = core::text::Extrude{};
    solid.extrude->lightingEnabled = false; // flat self-lit: exact colour asserts
    tl->setBlock(solid);

    // A HALF-opacity green overlay over the red material: one application = (128, 128, 0); a
    // buggy second application in the compositor would pull it to (64, 191, 0).
    core::LayerEffects fx;
    fx.colorOverlay.enabled = true;
    fx.colorOverlay.paint = core::vec::SolidPaint{common::ColorF{0, 1, 0, 1}};
    fx.colorOverlay.opacity = 0.5f;
    tl->setEffects(fx);

    REQUIRE(core::text::refreshTextCache(*tl, shaper, db));
    const common::Image once = flatten(doc);
    // The densest ink pixel: scan for opaque coverage.
    int cx = -1, cy = -1;
    for (std::uint32_t y = 0; y < once.height && cx < 0; ++y)
        for (std::uint32_t x = 0; x < once.width; ++x)
            if (px(once, x, y).a == 255 && px(once, x, y).r > 64) {
                cx = static_cast<int>(x);
                cy = static_cast<int>(y);
                break;
            }
    REQUIRE(cx >= 0);
    const common::Color8 c = px(once, static_cast<std::uint32_t>(cx),
                                static_cast<std::uint32_t>(cy));
    CHECK(c.r > 115);  // ~128: the single, per-face application
    CHECK(c.r < 141);
    CHECK(c.g > 115);
    CHECK(c.g < 141);

    // A drop shadow on the same layer still runs in the compositor (only overlays are stripped):
    // the composite gains pixels outside the solid's own silhouette.
    const auto coverage = [](const common::Image& img) {
        int n = 0;
        for (std::size_t i = 3; i < img.rgba.size(); i += 4)
            if (img.rgba[i] > 16) ++n;
        return n;
    };
    const int bare = coverage(once);
    fx.dropShadows.emplace_back();
    fx.dropShadows.back().enabled = true;
    fx.dropShadows.back().distance = 8.0f;
    fx.dropShadows.back().size = 4.0f;
    fx.dropShadows.back().opacity = 1.0f;
    tl->setEffects(fx);
    core::text::refreshTextCache(*tl, shaper, db);  // overlay key unchanged; harmless either way
    CHECK(coverage(flatten(doc)) > bare + 50);

    // FLAT text keeps the 2D behaviour: the compositor's effect pass applies the overlay.
    core::text::TextBlock flat = core::text::makeBlock("HH", st);
    tl->setBlock(flat);
    core::LayerEffects flatFx;
    flatFx.colorOverlay.enabled = true;
    flatFx.colorOverlay.paint = core::vec::SolidPaint{common::ColorF{0, 1, 0, 1}};
    tl->setEffects(flatFx);
    REQUIRE(core::text::refreshTextCache(*tl, shaper, db));
    const common::Image flatOut = flatten(doc);
    int greens = 0;
    for (std::uint32_t y = 0; y < flatOut.height; ++y)
        for (std::uint32_t x = 0; x < flatOut.width; ++x) {
            const common::Color8 p = px(flatOut, x, y);
            if (p.a == 255 && p.g > 200 && p.r < 64) ++greens;
        }
    CHECK(greens > 0);  // the black glyphs read green through the 2D overlay pass
}

// S30-e feedback round: "Bevel/Emboss ... rendered in blocks that do not connect" on angled
// edges. The height field must come from the ANTI-ALIASED distance transform: the plain binary
// EDT facets along the Voronoi creases of the discrete boundary samples, and the Sobel turns the
// facets into patchy highlight/shadow blocks. Pin the fix: along a straight 45-degree edge the
// bevel surface is one uniform planar ramp, so the shaded colour at a constant inward depth must
// be (near) constant while marching along the edge.
TEST_CASE("bevel shading is continuous along an angled anti-aliased edge") {
    constexpr int kW = 64, kH = 64;
    common::ImageF io(kW, kH);
    constexpr float kSqrt2 = 1.4142135f;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            // Inside where x + y >= 64; coverage = the (sub-pixel) distance to the line, clamped.
            const float d = (static_cast<float>(x) + static_cast<float>(y) - 64.0f) / kSqrt2;
            const float cov = std::clamp(d + 0.5f, 0.0f, 1.0f);
            if (cov > 0.0f) io.set(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                                   {0.0f, 0.0f, 1.0f, cov});
        }

    core::LayerEffects fx;
    fx.bevel.enabled = true;
    fx.bevel.style = core::BevelEffect::Style::InnerBevel;
    fx.bevel.size = 8.0f;
    fx.bevel.depth = 1.0f;
    fx.bevel.soften = 0.0f;  // no smoothing help: the field itself must be continuous
    fx.bevel.angleDeg = 120.0f;
    fx.bevel.altitudeDeg = 30.0f;
    fx.bevel.highlightOpacity = 1.0f;
    fx.bevel.shadowOpacity = 1.0f;
    render::applyEffects(io, fx);

    // March along the edge at ~2.8px inward depth (x + y == 68), well inside the ramp band.
    float lo = 1e9f, hi = -1e9f;
    int distinct8bit = 0;
    int prevQ = -1;
    for (int x = 22; x <= 42; ++x) {
        const int y = 68 - x;
        const common::ColorF c = io.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
        const float shade = c.r + c.g;  // the blue base has none; highlight/shadow move it
        lo = std::min(lo, shade);
        hi = std::max(hi, shade);
        // The 8-bit view of the same march: the TPDF dither must vary it pixel to pixel, or the
        // uniform float shade quantises into one flat band -- banding reads as "lines running
        // through the beveled surface" (user 2026-07-16).
        const int q = static_cast<int>(std::lround(std::clamp(c.r, 0.0f, 1.0f) * 255.0f));
        if (prevQ >= 0 && q != prevQ) ++distinct8bit;
        prevQ = q;
    }
    CHECK(hi - lo < 0.06f);  // one planar ramp = one shade (binary-EDT facets blow well past this)
    CHECK(distinct8bit >= 2);  // ...but never one FLAT 8-bit band (the dither breathes in it)

    // Deterministic: the dither is a pure function of shape-anchored coordinates.
    common::ImageF again(kW, kH);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const float d = (static_cast<float>(x) + static_cast<float>(y) - 64.0f) / kSqrt2;
            const float cov = std::clamp(d + 0.5f, 0.0f, 1.0f);
            if (cov > 0.0f) again.set(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                                      {0.0f, 0.0f, 1.0f, cov});
        }
    render::applyEffects(again, fx);
    CHECK(again.rgba == io.rgba);
}

// The other half of the "blocks that do not connect" report: a dirty-REGION composite (brush
// strokes, S60-b text edits) hands renderLayer a WINDOW that cuts through the shape, and effects
// computed from the window-cropped alpha see a false edge at the border -- every patch shades
// differently from its neighbours. renderLayer now renders an effects layer's full footprint and
// crops after, so a region composite is byte-identical to the same window of a full composite.
TEST_CASE("a region composite matches the full composite through an effects layer") {
    core::Document doc(96, 96);
    auto layer = doc.makeRaster("r");
    fillSquare(*layer, 24, 24, 48, {200, 40, 40, 255});
    core::RasterLayer& r = *layer;
    doc.root().addOnTop(std::move(layer));

    core::LayerEffects fx;
    fx.bevel.enabled = true;  // SDF-driven: the false window edge would shade a phantom bevel
    fx.bevel.size = 7.0f;
    fx.bevel.altitudeDeg = 30.0f;
    fx.gradientOverlay.enabled = true;  // normalised to the shape box: a crop would re-span it
    fx.gradientOverlay.paint = linearGradient({0, 1, 0, 1}, {0, 0, 1, 1});
    fx.dropShadows.emplace_back();
    fx.dropShadows.back().enabled = true;  // blur reads context beyond the window too
    fx.dropShadows.back().distance = 5.0f;
    fx.dropShadows.back().size = 4.0f;
    r.setEffects(fx);

    const common::Image full = flatten(doc);
    // A window that cuts straight through the square (and its shadow).
    const common::Rect roi{40.0, 32.0, 33.0, 41.0};
    const render::CompositeResult reg =
        render::compositeRegion(doc, roi, {}, render::Backend::Cpu);
    REQUIRE(reg.ok);
    REQUIRE(reg.image.width == 33);
    int mismatches = 0;
    for (std::uint32_t y = 0; y < reg.image.height; ++y)
        for (std::uint32_t x = 0; x < reg.image.width; ++x)
            if (!(px(reg.image, x, y) == px(full, x + 40, y + 32))) ++mismatches;
    CHECK(mismatches == 0);
}
