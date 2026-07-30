// Layer-effects Vulkan-lane parity tests (S60-e; docs/layer-effects.md §8, which designed the
// seam for exactly this sibling). The compute lane against render::applyEffects -- the permanent
// CPU reference -- on the same effect stacks. Gated on a usable Vulkan device (the
// test_extrude_gpu.cpp CI-safe pattern: a machine without one WARNs and passes).
//
// TOLERANCE-BASED, deliberately: both lanes run float over shared formulas, the distance fields
// come from the SAME `fx::` builders and the blur tables are uploaded bit-identical, so the drift
// left is fused-multiply-add contraction -- "the same picture", never the same bits, and never a
// substitute for the CPU-pinned byte goldens in test_layer_effects.cpp.
//
// The bound is a MAX plus a COUNT, never a mean: a mean over a padded effect ROI is dominated by
// the untouched border and would hide a corrupted tile entirely. The LayerEffectsGpu is
// constructed DIRECTLY here; the test binary must never install a global override, because every
// byte-pinned golden depends on the CPU lane serving the compositor.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/image.hpp"
#include "core/layer_effects.hpp"
#include "render/gpu_caps.hpp"
#include "render/gpu_policy.hpp"
#include "render/layer_effects_gpu.hpp"
#include "render/layer_effects_render.hpp"

using mosaic::common::ColorF;
using mosaic::common::ImageF;
using mosaic::core::BlendMode;
using mosaic::core::LayerEffects;
using mosaic::render::LayerEffectRefusal;
using mosaic::render::LayerEffectsGpu;
namespace vec = mosaic::core::vec;

namespace {

// ---- fixtures ---------------------------------------------------------------------------------

// The structured shape every case styles: a hard-edged disc with an ANTI-ALIASED rim (the rim is
// what the stroke SDF and the shadow choke actually key off), a solid rectangle with a corner (a
// straight edge and a right angle, where a bevelled distance field is most fragile), an alpha ramp
// down one side (partial coverage that the `min(res.a, cov)` clips must respect), and a fully
// transparent block carrying JUNK RGB -- the straight-vs-premultiplied tripwire: any lane that
// treats the buffer as premultiplied drags that junk into visible neighbours and fails loudly.
ImageF shapeImage(std::uint32_t w = 96, std::uint32_t h = 72) {
    ImageF img(w, h);
    const float cx = 34.0f, cy = 30.0f, rad = 15.0f;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            ColorF c{0.0f, 0.0f, 0.0f, 0.0f};
            const float dx = static_cast<float>(x) + 0.5f - cx;
            const float dy = static_cast<float>(y) + 0.5f - cy;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < rad + 0.5f) {  // a 1px linear AA rim, like the vector rasteriser leaves
                c = {0.85f, 0.35f, 0.15f, std::clamp(rad + 0.5f - d, 0.0f, 1.0f)};
            }
            if (x >= 56 && x < 82 && y >= 20 && y < 52) c = {0.15f, 0.45f, 0.9f, 1.0f};
            // The ramp rides the rectangle's right half, so partial coverage overlaps real colour.
            if (x >= 70 && x < 82 && y >= 20 && y < 52)
                c.a = std::clamp(1.0f - static_cast<float>(x - 70) / 14.0f, 0.0f, 1.0f);
            if (x >= 6 && x < 16 && y >= 58 && y < 68) c = {7.5f, -2.0f, 42.0f, 0.0f};
            img.set(x, y, c);
        }
    }
    return img;
}

// Content pressed hard against the LEFT and TOP buffer edges. An outward effect on this shape has
// its ROI clipped by the buffer, so the blur kernel's edge policy is load-bearing rather than
// academic -- this is the fixture that would catch the lane reaching for blur_separable.comp's
// CLAMP taps instead of effect_primitives.cpp's REFLECT-101 ones.
ImageF edgeShapeImage(std::uint32_t w = 64, std::uint32_t h = 48) {
    ImageF img(w, h);
    for (std::uint32_t y = 0; y < 22; ++y)
        for (std::uint32_t x = 0; x < 26; ++x)
            img.set(x, y, {0.9f, 0.8f, 0.2f, 1.0f});
    return img;
}

ImageF transparentImage(std::uint32_t w = 48, std::uint32_t h = 40) {
    ImageF img(w, h);
    // Junk RGB at zero alpha everywhere: the "an effect on a fully transparent layer" case must
    // leave every one of these floats exactly where it found them.
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            img.set(x, y, {0.3f * static_cast<float>(x), -1.5f, 9.25f, 0.0f});
    return img;
}

vec::Paint solid(float r, float g, float b, float a = 1.0f) {
    return vec::SolidPaint{ColorF{r, g, b, a}};
}

vec::Paint gradientPaint() {
    vec::Gradient g;
    g.stops = {{0.0, ColorF{1.0f, 0.0f, 0.0f, 1.0f}, 0.5},
               {1.0, ColorF{0.0f, 0.0f, 1.0f, 1.0f}, 0.5}};
    return g;
}

// ---- the parity bound -------------------------------------------------------------------------
//
// kEps is ONE fp16 ulp at 1.0 -- 2^-10 == 9.77e-4 == 0.249 of an 8-bit LSB. It is derived from the
// fp16 mantissa rather than from measurement: the project's working accumulator is
// R16G16B16A16_SFLOAT (~11 bits), so a difference smaller than one ulp at the top of the range is
// not representable by the time these pixels reach it, and a difference larger than that survives
// into what the user sees. This lane carries fp32 storage buffers end to end -- the seam takes and
// returns a host ImageF, so no rgba16f image exists inside it -- which means its OWN arithmetic
// drift (FMA contraction over identical tap orders, against host-cooked tables) sits three orders
// of magnitude below this. The bound is therefore set by what the pipeline can carry, not by what
// the lane happens to achieve, and it does not move if a driver's FMA policy changes.
constexpr double kEps = 1.0 / 1024.0;

// The HARD max, in 8-bit LSB: the standing project bound ("the GPU lane must match the CPU
// reference within 1/255"). It is 4x kEps, which is exactly the headroom a threshold branch needs:
// every guard in the shaders (`f <= 0`, `cov <= 0`, `d < lo - kAa`) sits where the source alpha it
// gates is itself going to zero, so a flip across one of them moves a pixel by ~0 -- but a
// pathological long-row accumulation in the box lane could reach an LSB, and that must fail the
// COUNT, not the MAX.
constexpr double kHardMaxLsb = 1.0;

// Pixels allowed past kEps. A sliver, not a licence: a wrong formula or a lost dispatch moves a
// whole region and blows this by orders of magnitude, while genuine rounding concentrates in
// isolated pixels along an iso-contour.
constexpr double kOutlierFrac = 0.001;

struct Diff {
    double maxAbs = 0.0;     // worst channel delta, absolute
    double meanAbs = 0.0;    // reported only -- never asserted on (see the file header)
    std::size_t over = 0;    // pixels with ANY channel past kEps
    std::size_t pixels = 0;
};

Diff compare(const ImageF& cpu, const ImageF& gpu) {
    Diff d;
    REQUIRE(cpu.width == gpu.width);
    REQUIRE(cpu.height == gpu.height);
    REQUIRE(cpu.rgba.size() == gpu.rgba.size());
    d.pixels = cpu.pixelCount();
    double sum = 0.0;
    for (std::size_t i = 0; i < cpu.rgba.size(); i += 4) {
        double pixelMax = 0.0;
        for (std::size_t c = 0; c < 4; ++c) {
            const double delta =
                std::abs(static_cast<double>(cpu.rgba[i + c]) - static_cast<double>(gpu.rgba[i + c]));
            sum += delta;
            pixelMax = std::max(pixelMax, delta);
        }
        d.maxAbs = std::max(d.maxAbs, pixelMax);
        if (pixelMax > kEps) ++d.over;
    }
    d.meanAbs = sum / static_cast<double>(cpu.rgba.size());
    return d;
}

std::unique_ptr<LayerEffectsGpu> makeLane(const char* who) {
    std::string err;
    auto gpu = LayerEffectsGpu::create(/*enableValidation=*/true, err);
    if (!gpu) {
        const std::string why = std::string("no layer-effects GPU lane -- skipping ") + who +
                                " (" + err + ")";
        WARN_MESSAGE(true, why);
    }
    return gpu;
}

// The whole assertion, in one place: the lane must SERVE the stack, and its picture must sit
// inside the bound above. `source` is the untouched fixture; both lanes start from a copy of it.
void checkParity(LayerEffectsGpu& gpu, const LayerEffects& fx, const char* label,
                 const ImageF& source) {
    // The lane must agree, before anything runs, that it can take this stack at all -- a case that
    // silently fell back would otherwise "pass" while testing nothing.
    REQUIRE(mosaic::render::layerEffectsAdmission(fx) == LayerEffectRefusal::None);

    ImageF cpu = source;
    mosaic::render::applyEffects(cpu, fx);
    ImageF via = source;
    const bool served = gpu.apply(via, fx);
    // Named local, not a temporary: doctest's INFO holds its operands by reference until the scope
    // ends, so the string has to outlive the macro.
    const std::string why{mosaic::render::layerEffectRefusalName(gpu.lastRefusal())};
    INFO(std::string(label) << ": lane refusal " << why);
    REQUIRE(served);

    const Diff d = compare(cpu, via);
    INFO(std::string(label) << ": maxAbs " << d.maxAbs << " (" << d.maxAbs * 255.0 << " LSB)"
                            << ", over-eps " << d.over << "/" << d.pixels << ", meanAbs "
                            << d.meanAbs);
    CHECK(d.maxAbs * 255.0 <= kHardMaxLsb);
    CHECK(static_cast<double>(d.over) <= kOutlierFrac * static_cast<double>(d.pixels) + 8.0);
}

mosaic::core::ShadowEffect dropShadow(float size, float distance, float spread) {
    mosaic::core::ShadowEffect sh;
    sh.enabled = true;
    sh.size = size;
    sh.distance = distance;
    sh.spread = spread;
    sh.color = ColorF{0.05f, 0.02f, 0.10f, 0.9f};
    sh.opacity = 0.75f;
    sh.blend = BlendMode::Multiply;
    sh.angleDeg = 120.0f;
    return sh;
}

}  // namespace

// ---- the served kinds --------------------------------------------------------------------------

TEST_CASE("GPU drop shadows draw the CPU lane's picture") {
    auto gpu = makeLane("drop shadow parity");
    if (!gpu) return;
    const ImageF source = shapeImage();

    // Small size: sigma = size/3 stays under kLargeBlurSigma, so this is the EXACT separable
    // Gaussian lane with reflect-101 edges.
    {
        LayerEffects fx;
        fx.dropShadows.push_back(dropShadow(6.0f, 6.0f, 0.0f));
        checkParity(*gpu, fx, "drop shadow, gaussian lane", source);
    }
    // Spread fattens the solid core before the blur softens it; a negative spread erodes it.
    {
        LayerEffects fx;
        fx.dropShadows.push_back(dropShadow(9.0f, 4.0f, 3.0f));
        checkParity(*gpu, fx, "drop shadow, positive spread", source);
    }
    {
        LayerEffects fx;
        fx.dropShadows.push_back(dropShadow(9.0f, 4.0f, -2.5f));
        checkParity(*gpu, fx, "drop shadow, negative spread", source);
    }
    // Large size: sigma >= 8 crosses into the 3-pass Kovesi BOX lane, whose running sum the
    // shader reproduces per line rather than re-deriving by a parallel gather.
    {
        LayerEffects fx;
        fx.dropShadows.push_back(dropShadow(30.0f, 10.0f, 0.0f));
        checkParity(*gpu, fx, "drop shadow, box lane", source);
    }
    // Stacked, and each with its own blend -- including a NON-SEPARABLE (HSL) mode, which is the
    // only path through blendNonSep.
    {
        LayerEffects fx;
        mosaic::core::ShadowEffect a = dropShadow(5.0f, 7.0f, 1.0f);
        a.blend = BlendMode::Screen;
        mosaic::core::ShadowEffect b = dropShadow(11.0f, 3.0f, 0.0f);
        b.angleDeg = -40.0f;
        b.blend = BlendMode::Luminosity;
        mosaic::core::ShadowEffect c = dropShadow(24.0f, 14.0f, 2.0f);
        c.blend = BlendMode::ColorBurn;  // a division by the source channel
        fx.dropShadows = {a, b, c};
        checkParity(*gpu, fx, "three stacked shadows, mixed blends", source);
    }
}

TEST_CASE("GPU outer glow draws the CPU lane's picture") {
    auto gpu = makeLane("outer glow parity");
    if (!gpu) return;
    const ImageF source = shapeImage();

    LayerEffects fx;
    fx.outerGlow.enabled = true;
    fx.outerGlow.paint = solid(1.0f, 0.9f, 0.35f, 1.0f);
    fx.outerGlow.size = 9.0f;
    fx.outerGlow.choke = 0.0f;
    fx.outerGlow.opacity = 0.8f;
    checkParity(*gpu, fx, "outer glow, screen, gaussian lane", source);

    fx.outerGlow.choke = 4.0f;   // a fattened core before the blur
    fx.outerGlow.size = 28.0f;   // and over into the box lane
    fx.outerGlow.blend = BlendMode::LinearDodge;
    checkParity(*gpu, fx, "outer glow, choked, box lane", source);

    // A drop shadow UNDER the glow: the `below` scratch has to accumulate in vector order, glow
    // last, before the layer is placed over the result.
    fx.dropShadows.push_back(dropShadow(8.0f, 5.0f, 0.0f));
    checkParity(*gpu, fx, "outer glow over a drop shadow", source);
}

TEST_CASE("GPU inner shadow and inner glow draw the CPU lane's picture") {
    auto gpu = makeLane("inner effects parity");
    if (!gpu) return;
    const ImageF source = shapeImage();

    {
        LayerEffects fx;
        mosaic::core::ShadowEffect sh = dropShadow(7.0f, 5.0f, 0.0f);
        fx.innerShadows.push_back(sh);
        checkParity(*gpu, fx, "inner shadow", source);
    }
    {
        // Two inner shadows, the second big enough to take the box lane and eroded by spread.
        LayerEffects fx;
        mosaic::core::ShadowEffect a = dropShadow(4.0f, 3.0f, 1.0f);
        mosaic::core::ShadowEffect b = dropShadow(27.0f, 9.0f, 2.0f);
        b.angleDeg = 200.0f;
        b.blend = BlendMode::Overlay;
        fx.innerShadows = {a, b};
        checkParity(*gpu, fx, "two inner shadows, box lane", source);
    }
    {
        // Edge source: the blurred, choked complement -- a bright band just inside every edge.
        LayerEffects fx;
        fx.innerGlow.enabled = true;
        fx.innerGlow.paint = solid(0.4f, 1.0f, 0.8f, 1.0f);
        fx.innerGlow.size = 10.0f;
        fx.innerGlow.choke = 2.0f;
        fx.innerGlow.source = mosaic::core::GlowEffect::Source::Edge;
        checkParity(*gpu, fx, "inner glow, edge source", source);
    }
    {
        // Centre source: a pure distance ramp with NO blur at all -- the one served effect whose
        // chain is a single seed dispatch, so it also pins that the ping-pong leaves the field in
        // the buffer the composite step reads.
        LayerEffects fx;
        fx.innerGlow.enabled = true;
        fx.innerGlow.paint = solid(0.4f, 1.0f, 0.8f, 1.0f);
        fx.innerGlow.size = 12.0f;
        fx.innerGlow.choke = 3.0f;
        fx.innerGlow.source = mosaic::core::GlowEffect::Source::Center;
        fx.innerGlow.blend = BlendMode::HardLight;
        checkParity(*gpu, fx, "inner glow, centre source", source);
    }
}

TEST_CASE("GPU solid overlay draws the CPU lane's picture") {
    auto gpu = makeLane("overlay parity");
    if (!gpu) return;
    const ImageF source = shapeImage();

    LayerEffects fx;
    fx.colorOverlay.enabled = true;
    fx.colorOverlay.paint = solid(0.2f, 0.6f, 1.0f, 1.0f);
    fx.colorOverlay.opacity = 0.65f;
    checkParity(*gpu, fx, "colour overlay, normal", source);

    // The overlay composites at its OWN alpha and clamps the RESULT to the coverage; a translucent
    // paint over the fixture's alpha ramp is where that ordering is visible.
    fx.colorOverlay.paint = solid(0.95f, 0.15f, 0.4f, 0.55f);
    fx.colorOverlay.blend = BlendMode::SoftLight;  // the sqrt branch
    checkParity(*gpu, fx, "colour overlay, soft light, translucent", source);

    fx.colorOverlay.blend = BlendMode::Hue;  // non-separable
    checkParity(*gpu, fx, "colour overlay, hue", source);

    // A SOLID paint parked in the gradient/pattern overlay slots is served too -- the slot does not
    // decide admission, the PAINT KIND does -- and all three coexist in a fixed z-order.
    fx.gradientOverlay.enabled = true;
    fx.gradientOverlay.paint = solid(0.1f, 0.9f, 0.2f, 0.4f);
    fx.gradientOverlay.blend = BlendMode::Multiply;
    fx.patternOverlay.enabled = true;
    fx.patternOverlay.paint = solid(1.0f, 1.0f, 1.0f, 0.25f);
    fx.patternOverlay.blend = BlendMode::Screen;
    checkParity(*gpu, fx, "three solid overlays in z-order", source);
}

TEST_CASE("GPU satin draws the CPU lane's picture") {
    auto gpu = makeLane("satin parity");
    if (!gpu) return;
    const ImageF source = shapeImage();

    LayerEffects fx;
    fx.satin.enabled = true;
    fx.satin.color = ColorF{0.1f, 0.05f, 0.25f, 1.0f};
    fx.satin.size = 14.0f;  // the model default -- sigma 5.6, already the BOX lane
    fx.satin.distance = 11.0f;
    fx.satin.angleDeg = 19.0f;
    fx.satin.invert = true;
    checkParity(*gpu, fx, "satin, inverted, box lane", source);

    fx.satin.size = 6.0f;  // sigma 2.4 -- the exact Gaussian lane
    fx.satin.invert = false;
    fx.satin.blend = BlendMode::Difference;
    checkParity(*gpu, fx, "satin, summed, gaussian lane", source);
}

TEST_CASE("GPU strokes draw the CPU lane's picture") {
    auto gpu = makeLane("stroke parity");
    if (!gpu) return;
    const ImageF source = shapeImage();

    using Align = mosaic::core::StrokeEffect::Align;
    const auto stroke = [](float width, Align align, ColorF c) {
        mosaic::core::StrokeEffect st;
        st.enabled = true;
        st.width = width;
        st.align = align;
        st.paint = vec::SolidPaint{c};
        return st;
    };

    {
        LayerEffects fx;
        fx.strokes = {stroke(4.0f, Align::Outside, ColorF{0.0f, 0.0f, 0.0f, 1.0f})};
        checkParity(*gpu, fx, "one outside stroke", source);
    }
    {
        LayerEffects fx;
        fx.strokes = {stroke(3.0f, Align::Inside, ColorF{1.0f, 1.0f, 1.0f, 1.0f})};
        checkParity(*gpu, fx, "one inside stroke", source);
    }
    {
        LayerEffects fx;
        fx.strokes = {stroke(5.0f, Align::Center, ColorF{0.2f, 0.9f, 0.3f, 0.8f})};
        checkParity(*gpu, fx, "one centre stroke", source);
    }
    {
        // The concentric white/black/white ring look: the outside rings must be drawn OUTERMOST
        // FIRST into the scratch and the innermost backed off by kInnerBack, or the ring-to-ring
        // seam and the content-rim seam both reappear.
        LayerEffects fx;
        fx.strokes = {stroke(3.0f, Align::Outside, ColorF{1.0f, 1.0f, 1.0f, 1.0f}),
                      stroke(2.0f, Align::Outside, ColorF{0.0f, 0.0f, 0.0f, 1.0f}),
                      stroke(4.0f, Align::Outside, ColorF{1.0f, 0.4f, 0.0f, 1.0f})};
        checkParity(*gpu, fx, "three concentric outside strokes", source);
    }
    {
        // A mixed stack whose middle entry is a zero-width placeholder and whose next is NoPaint:
        // both must still advance the cumulative offset, or every ring beyond them shifts inward.
        LayerEffects fx;
        mosaic::core::StrokeEffect zero =
            stroke(0.0f, Align::Outside, ColorF{1.0f, 0.0f, 0.0f, 1.0f});
        mosaic::core::StrokeEffect none =
            stroke(3.0f, Align::Outside, ColorF{1.0f, 0.0f, 0.0f, 1.0f});
        none.paint = vec::NoPaint{};
        mosaic::core::StrokeEffect inner =
            stroke(2.5f, Align::Inside, ColorF{0.1f, 0.1f, 0.6f, 1.0f});
        inner.blend = BlendMode::Multiply;
        fx.strokes = {stroke(2.0f, Align::Outside, ColorF{0.9f, 0.9f, 0.1f, 1.0f}), zero, none,
                      stroke(3.0f, Align::Outside, ColorF{0.0f, 0.2f, 0.8f, 1.0f}), inner};
        checkParity(*gpu, fx, "mixed stroke stack with zero-width and NoPaint entries", source);
    }
}

TEST_CASE("GPU fill-opacity draws the CPU lane's picture") {
    auto gpu = makeLane("fill-opacity parity");
    if (!gpu) return;
    const ImageF source = shapeImage();

    // Fill-opacity dims the layer's OWN pixels only. The effects must still key off the FULL
    // coverage, so a dimmed fill under a stroke and a shadow is the case that proves the captured
    // alpha was taken before the dim, not after.
    LayerEffects fx;
    fx.fillOpacity = 0.25f;
    checkParity(*gpu, fx, "fill-opacity alone", source);

    fx.dropShadows.push_back(dropShadow(8.0f, 6.0f, 0.0f));
    mosaic::core::StrokeEffect st;
    st.enabled = true;
    st.width = 3.0f;
    st.align = mosaic::core::StrokeEffect::Align::Outside;
    st.paint = solid(0.0f, 0.0f, 0.0f, 1.0f);
    fx.strokes.push_back(st);
    checkParity(*gpu, fx, "fill-opacity under a shadow and a stroke", source);

    fx.fillOpacity = 0.0f;  // the fully knocked-out fill: only the effects remain
    checkParity(*gpu, fx, "zero fill-opacity", source);
}

TEST_CASE("GPU layer effects draw the CPU lane's picture for a full served stack") {
    auto gpu = makeLane("full stack parity");
    if (!gpu) return;
    const ImageF source = shapeImage();

    // Every served tier at once, in the canonical z-order -- the case that pins the ORDER as well
    // as each effect, since a mis-sequenced chain still produces plausible pixels.
    LayerEffects fx;
    fx.fillOpacity = 0.8f;
    fx.dropShadows.push_back(dropShadow(10.0f, 7.0f, 1.0f));
    fx.outerGlow.enabled = true;
    fx.outerGlow.paint = solid(1.0f, 0.85f, 0.3f, 1.0f);
    fx.outerGlow.size = 12.0f;
    fx.colorOverlay.enabled = true;
    fx.colorOverlay.paint = solid(0.3f, 0.5f, 0.95f, 0.6f);
    fx.satin.enabled = true;
    fx.satin.size = 9.0f;
    fx.satin.distance = 7.0f;
    fx.innerShadows.push_back(dropShadow(6.0f, 4.0f, 0.0f));
    fx.innerGlow.enabled = true;
    fx.innerGlow.paint = solid(0.9f, 1.0f, 1.0f, 0.7f);
    fx.innerGlow.size = 8.0f;
    mosaic::core::StrokeEffect st;
    st.enabled = true;
    st.width = 3.5f;
    st.align = mosaic::core::StrokeEffect::Align::Outside;
    st.paint = solid(0.05f, 0.05f, 0.05f, 1.0f);
    fx.strokes.push_back(st);
    checkParity(*gpu, fx, "every served tier at once", source);
}

// ---- degenerate and boundary shapes -----------------------------------------------------------

TEST_CASE("GPU layer effects hold parity on degenerate and zero-radius parameters") {
    auto gpu = makeLane("degenerate parity");
    if (!gpu) return;
    const ImageF source = shapeImage();

    {
        // Zero SIZE: sigma is 0, so the CPU's blurCoverage returns without touching the plane and
        // the lane must plan no blur pass at all. The shadow is then the hard dilated core.
        LayerEffects fx;
        fx.dropShadows.push_back(dropShadow(0.0f, 5.0f, 0.0f));
        checkParity(*gpu, fx, "zero-size drop shadow", source);
    }
    {
        // Zero size AND zero distance: the shadow lands exactly under the shape and is entirely
        // hidden once the layer is placed over the scratch. A lane that placed the scratch OVER
        // the layer instead of under it fails here and nowhere else.
        LayerEffects fx;
        fx.dropShadows.push_back(dropShadow(0.0f, 0.0f, 0.0f));
        checkParity(*gpu, fx, "zero-size zero-distance drop shadow", source);
    }
    {
        LayerEffects fx;
        fx.outerGlow.enabled = true;
        fx.outerGlow.paint = solid(1.0f, 1.0f, 1.0f, 1.0f);
        fx.outerGlow.size = 0.0f;
        checkParity(*gpu, fx, "zero-size outer glow", source);
    }
    {
        // A stroke stack that paints nothing: enabled, but every entry either zero-width or
        // NoPaint. The CPU's `anyStroke` is false, so no SDF is built and no pixel moves.
        LayerEffects fx;
        mosaic::core::StrokeEffect zero;
        zero.enabled = true;
        zero.width = 0.0f;
        zero.paint = solid(1.0f, 0.0f, 0.0f, 1.0f);
        fx.strokes.push_back(zero);
        checkParity(*gpu, fx, "zero-width stroke only", source);
    }
    {
        // A fully transparent effect colour: every guard that reads `src.a <= 0` fires.
        LayerEffects fx;
        fx.colorOverlay.enabled = true;
        fx.colorOverlay.paint = solid(0.5f, 0.5f, 0.5f, 0.0f);
        checkParity(*gpu, fx, "fully transparent overlay paint", source);
    }
    {
        // An enabled satin with zero opacity -- the CPU's own early-out.
        LayerEffects fx;
        fx.satin.enabled = true;
        fx.satin.opacity = 0.0f;
        checkParity(*gpu, fx, "zero-opacity satin", source);
    }
}

TEST_CASE("GPU layer effects hold parity on a fully transparent layer") {
    auto gpu = makeLane("transparent-layer parity");
    if (!gpu) return;
    const ImageF source = transparentImage();

    LayerEffects fx;
    fx.fillOpacity = 0.5f;
    fx.dropShadows.push_back(dropShadow(9.0f, 6.0f, 2.0f));
    fx.outerGlow.enabled = true;
    fx.outerGlow.paint = solid(1.0f, 1.0f, 1.0f, 1.0f);
    fx.outerGlow.size = 12.0f;
    mosaic::core::StrokeEffect st;
    st.enabled = true;
    st.width = 4.0f;
    st.paint = solid(0.0f, 0.0f, 0.0f, 1.0f);
    fx.strokes.push_back(st);

    // No coverage anywhere means no content box, so the reference returns having written nothing.
    // The lane must SERVE that -- returning false here would be honest but weak; the claim worth
    // pinning is that it produces the same answer, byte for byte, including the junk RGB the
    // transparent pixels carry.
    ImageF cpu = source;
    mosaic::render::applyEffects(cpu, fx);
    ImageF via = source;
    CHECK(gpu->apply(via, fx));
    CHECK(cpu.rgba == source.rgba);  // the reference's own claim, pinned first
    CHECK(via.rgba == source.rgba);  // ...and the lane's, BYTE-exact rather than within tolerance
}

TEST_CASE("GPU layer effects hold parity when the footprint runs past the layer bounds") {
    auto gpu = makeLane("clipped-footprint parity");
    if (!gpu) return;
    const ImageF source = edgeShapeImage();

    // The content touches the left and top edges, so the ROI is CLIPPED by the buffer on two sides
    // and the blur kernel reaches for taps that do not exist. This is the case that separates
    // reflect-101 from clamp: the two edge policies disagree by a visible amount here, and by
    // nothing at all on a shape that floats clear of the border.
    {
        LayerEffects fx;
        fx.dropShadows.push_back(dropShadow(14.0f, 10.0f, 2.0f));
        checkParity(*gpu, fx, "shadow off the clipped top-left corner", source);
    }
    {
        // Angled the other way, so the offset drives the shadow OUT of the buffer entirely on two
        // sides -- every sample the bilinear fetch takes off the ROI must read exactly zero.
        LayerEffects fx;
        mosaic::core::ShadowEffect sh = dropShadow(10.0f, 22.0f, 0.0f);
        sh.angleDeg = 300.0f;
        fx.dropShadows.push_back(sh);
        checkParity(*gpu, fx, "shadow offset off the buffer", source);
    }
    {
        // A large outer glow plus a wide outside stroke: both grow the ROI past the buffer, and
        // the stroke's anti-aliased SDF is built on the clipped window.
        LayerEffects fx;
        fx.outerGlow.enabled = true;
        fx.outerGlow.paint = solid(0.2f, 0.7f, 1.0f, 1.0f);
        fx.outerGlow.size = 26.0f;  // the box lane, against a clipped border
        mosaic::core::StrokeEffect st;
        st.enabled = true;
        st.width = 7.0f;
        st.align = mosaic::core::StrokeEffect::Align::Outside;
        st.paint = solid(0.0f, 0.0f, 0.0f, 1.0f);
        fx.strokes.push_back(st);
        checkParity(*gpu, fx, "glow and stroke past the clipped bounds", source);
    }
    {
        // Inner effects grow the READ window without growing the paint footprint; near a clipped
        // border their complement field is exactly where the padding ran out.
        LayerEffects fx;
        fx.innerShadows.push_back(dropShadow(12.0f, 9.0f, 1.0f));
        fx.innerGlow.enabled = true;
        fx.innerGlow.paint = solid(1.0f, 0.6f, 0.2f, 1.0f);
        fx.innerGlow.size = 14.0f;
        checkParity(*gpu, fx, "inner effects against a clipped border", source);
    }
}

// ---- the refusal paths --------------------------------------------------------------------------

TEST_CASE("layer-effects lane admission is per kind, and pure") {
    // No device needed: admission is a predicate over the stack, so the served/refused boundary is
    // testable on every machine, not only on one with a GPU.
    using mosaic::render::layerEffectsAdmission;

    {
        LayerEffects fx;
        fx.dropShadows.push_back(dropShadow(6.0f, 6.0f, 0.0f));
        fx.colorOverlay.enabled = true;
        fx.colorOverlay.paint = solid(1.0f, 0.0f, 0.0f, 1.0f);
        CHECK(layerEffectsAdmission(fx) == LayerEffectRefusal::None);
    }
    // A GRADIENT anywhere refuses the whole stack: the paint evaluator's banding dither is a
    // 64-bit hash finished in double, and Vulkan 1.0 guarantees neither type.
    {
        LayerEffects fx;
        fx.gradientOverlay.enabled = true;
        fx.gradientOverlay.paint = gradientPaint();
        CHECK(layerEffectsAdmission(fx) == LayerEffectRefusal::NonSolidPaint);
    }
    {
        LayerEffects fx;
        mosaic::core::StrokeEffect st;
        st.enabled = true;
        st.width = 3.0f;
        st.paint = gradientPaint();
        fx.strokes.push_back(st);
        CHECK(layerEffectsAdmission(fx) == LayerEffectRefusal::NonSolidPaint);
    }
    {
        LayerEffects fx;
        fx.outerGlow.enabled = true;
        fx.outerGlow.paint = gradientPaint();
        CHECK(layerEffectsAdmission(fx) == LayerEffectRefusal::NonSolidPaint);
    }
    // A gradient on a stroke that paints NOTHING is not a refusal -- the CPU lane never evaluates
    // it either, so there is no pixel to disagree about.
    {
        LayerEffects fx;
        mosaic::core::StrokeEffect st;
        st.enabled = true;
        st.width = 0.0f;
        st.paint = gradientPaint();
        fx.strokes.push_back(st);
        CHECK(layerEffectsAdmission(fx) == LayerEffectRefusal::None);
    }
    // A disabled gradient overlay is not a refusal either.
    {
        LayerEffects fx;
        fx.gradientOverlay.enabled = false;
        fx.gradientOverlay.paint = gradientPaint();
        CHECK(layerEffectsAdmission(fx) == LayerEffectRefusal::None);
    }
    // BEVEL: refused for its own dither, on the same grounds.
    {
        LayerEffects fx;
        fx.bevel.enabled = true;
        fx.bevel.size = 5.0f;
        CHECK(layerEffectsAdmission(fx) == LayerEffectRefusal::Bevel);
    }
    // ...but a bevel with no size is the CPU lane's own no-op, so it is served.
    {
        LayerEffects fx;
        fx.bevel.enabled = true;
        fx.bevel.size = 0.0f;
        fx.dropShadows.push_back(dropShadow(5.0f, 5.0f, 0.0f));
        CHECK(layerEffectsAdmission(fx) == LayerEffectRefusal::None);
    }
    // Every refusal names itself in a way a log line can carry.
    CHECK(mosaic::render::layerEffectRefusalName(LayerEffectRefusal::NonSolidPaint) ==
          "non-solid paint");
    CHECK(mosaic::render::layerEffectRefusalName(LayerEffectRefusal::Bevel) == "bevel & emboss");
    CHECK(mosaic::render::layerEffectRefusalName(LayerEffectRefusal::None) == "none");
}

TEST_CASE("a refused stack leaves the buffer byte-untouched for the CPU lane") {
    auto gpu = makeLane("refusal contract");
    if (!gpu) return;
    const ImageF source = shapeImage();

    // A refusal must cost speed and NOTHING else: the caller runs the CPU lane on the same buffer
    // afterwards, so a partially-written buffer would corrupt the fallback rather than merely slow
    // it down. Both refused kinds are checked, each against a stack that also carries served ones.
    {
        LayerEffects fx;
        fx.dropShadows.push_back(dropShadow(6.0f, 6.0f, 0.0f));
        fx.gradientOverlay.enabled = true;
        fx.gradientOverlay.paint = gradientPaint();
        ImageF via = source;
        CHECK_FALSE(gpu->apply(via, fx));
        CHECK(gpu->lastRefusal() == LayerEffectRefusal::NonSolidPaint);
        CHECK(via.rgba == source.rgba);
        // ...and the CPU lane picking it up from there is the whole point.
        ImageF cpu = source;
        mosaic::render::applyEffects(cpu, fx);
        mosaic::render::applyEffects(via, fx);
        CHECK(via.rgba == cpu.rgba);
    }
    {
        LayerEffects fx;
        fx.bevel.enabled = true;
        fx.bevel.size = 6.0f;
        fx.colorOverlay.enabled = true;
        fx.colorOverlay.paint = solid(0.2f, 0.4f, 0.8f, 1.0f);
        ImageF via = source;
        CHECK_FALSE(gpu->apply(via, fx));
        CHECK(gpu->lastRefusal() == LayerEffectRefusal::Bevel);
        CHECK(via.rgba == source.rgba);
    }
    {
        // An EMPTY stack is not a refusal, it is the compositor's short-circuit: the lane declines
        // so an untouched layer pays nothing, and `io` is byte-identical either way.
        LayerEffects fx;
        ImageF via = source;
        CHECK_FALSE(gpu->apply(via, fx));
        CHECK(gpu->lastRefusal() == LayerEffectRefusal::None);
        CHECK(via.rgba == source.rgba);
    }
}

TEST_CASE("the layer-effects lane refuses itself in CPU-only mode") {
    using namespace mosaic::render;
    // Restore whatever the process was running under, whatever this test does -- the whole suite
    // shares one policy and leaving it flipped would silently skip every GPU test after this one.
    struct Restore {
        GpuPolicy saved = gpuPolicy();
        ~Restore() { setGpuPolicy(saved); }
    };
    [[maybe_unused]] const Restore restore;

    setGpuPolicy(GpuPolicy{GpuUse::CpuOnly});
    std::string error;
    const auto gpu = LayerEffectsGpu::create(/*enableValidation=*/false, error);
    CHECK(gpu == nullptr);
    // The refusal is a named, actionable sentence naming the lane and a way back.
    CHECK_FALSE(error.empty());
    CHECK(error.find("layer effects") != std::string::npos);
    CHECK(error.find("--cpu") != std::string::npos);
}

TEST_CASE("the layer-effects lane fits the Vulkan 1.0 floor") {
    using namespace mosaic::render;
    // The floor profile is the interesting case for this lane in the OPPOSITE direction from most:
    // it was designed to SERVE there, so what has to be pinned is that its declared footprint
    // still fits a minimal conforming 1.0 device -- four storage buffers against a guaranteed four
    // and an 80-byte push range against a guaranteed 128. If a future binding is added, this fails
    // here rather than turning into a silent whole-lane refusal on the hardware we target.
    GpuProbe probe;
    applyFloorProfile(probe);
    const GpuCaps caps = decide(probe);

    CHECK(caps.fitsStorageBuffers(LayerEffectsGpu::kStorageBufferBindings));
    CHECK(caps.fitsPushConstants(LayerEffectsGpu::kPushConstantBytes));
    CHECK(LayerEffectsGpu::kWorkgroupInvocations <= caps.limits.maxComputeWorkGroupInvocations);
    // A fifth storage buffer would NOT fit, which is exactly why the two kernels share one
    // four-binding layout and reinterpret it per pass rather than each declaring what it wants.
    CHECK_FALSE(caps.fitsStorageBuffers(LayerEffectsGpu::kStorageBufferBindings + 1));
}
