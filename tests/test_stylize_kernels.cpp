// S35 stylize-kernel engine (docs/filters-stylize.md §3): analytic kernel signatures instead of
// golden pixels. A sharpen must raise a bright pixel and undershoot its neighbours by the exact
// amount the 3x3 kernel says; unsharp's threshold must be a real gate; the noise must be a pure
// function of the PARENT coordinate (translate the placement and the same grain follows the
// document); a mosaic cell must be anchored to the parent lattice; Kuwahara must hold a step edge;
// and every "no amount" case must return the input untouched, byte for byte.
#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "render/stylize_kernels.hpp"

using namespace mosaic;
namespace fx = mosaic::render::fx;

namespace {

common::ImageF flatField(std::uint32_t w, std::uint32_t h, common::ColorF c) {
    common::ImageF img(w, h);
    img.fill(c);
    return img;
}

// Opaque vertical step: columns < split are `lo`, the rest `hi`. The edge-preservation fixtures
// all start here.
common::ImageF stepField(std::uint32_t w, std::uint32_t h, std::uint32_t split, float lo,
                         float hi) {
    common::ImageF img(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const float v = x < split ? lo : hi;
            img.set(x, y, {v, v, v, 1.0f});
        }
    return img;
}

bool allFinite(const common::ImageF& img) {
    for (const float v : img.rgba)
        if (!std::isfinite(v)) return false;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Sharpen / unsharp
// ---------------------------------------------------------------------------------------------

TEST_CASE("sharpen: the 3x3 kernel's exact overshoot and undershoot") {
    // Mid-gray field with one bright pixel. Every value here is a binary fraction and the field
    // is opaque, so the premultiply/un-premultiply round trip is exact and the arithmetic below
    // is the kernel's definition restated: out = v + amount * (v - mean4).
    common::ImageF img = flatField(9, 9, {0.5f, 0.5f, 0.5f, 1.0f});
    img.set(4, 4, {1.0f, 1.0f, 1.0f, 1.0f});
    fx::sharpenImage(img, 1.0f);

    // Centre: mean4 == 0.5, so out == 1 + (1 - 0.5) == 1.5.
    CHECK(img.at(4, 4).r == 1.5f);
    // A four-neighbour: mean4 == (0.5 + 0.5 + 0.5 + 1.0)/4 == 0.625, so out == 0.375.
    CHECK(img.at(3, 4).r == 0.375f);
    CHECK(img.at(5, 4).r == 0.375f);
    CHECK(img.at(4, 3).r == 0.375f);
    CHECK(img.at(4, 5).r == 0.375f);
    // A diagonal neighbour sees the bright pixel in no four-neighbour slot: untouched.
    CHECK(img.at(3, 3).r == 0.5f);
    // Alpha is never sharpened -- sharpening coverage carves halos into the edge.
    CHECK(img.at(4, 4).a == 1.0f);
    CHECK(img.at(3, 4).a == 1.0f);
    CHECK(allFinite(img));
}

TEST_CASE("sharpen: zero amount returns the input byte for byte") {
    const common::ImageF before = stepField(8, 8, 4, 0.2f, 0.8f);
    common::ImageF img = before;
    fx::sharpenImage(img, 0.0f);
    CHECK(img.rgba == before.rgba);
}

TEST_CASE("unsharp mask: the threshold is a real gate") {
    const common::ImageF before = stepField(16, 16, 8, 0.25f, 0.75f);

    // Threshold 1.0 exceeds every possible |luma difference| in a [0,1] image, so no pixel is
    // touched. The image is opaque, so premultiply -> un-premultiply is the identity and the
    // result must be byte-identical.
    common::ImageF gated = before;
    fx::unsharpMaskImage(gated, 2.0f, 1.0f, 1.0f, /*draft=*/false);
    CHECK(gated.rgba == before.rgba);

    // With the gate open the step's two sides must move APART: the dark side of the edge
    // undershoots, the bright side overshoots (the halo unsharp masking exists to create).
    common::ImageF sharp = before;
    fx::unsharpMaskImage(sharp, 2.0f, 1.0f, 0.0f, /*draft=*/false);
    CHECK(sharp.at(7, 8).r < before.at(7, 8).r);
    CHECK(sharp.at(8, 8).r > before.at(8, 8).r);
    CHECK(sharp.at(8, 8).a == 1.0f);  // alpha untouched
    CHECK(allFinite(sharp));
}

// ---------------------------------------------------------------------------------------------
// Add noise
// ---------------------------------------------------------------------------------------------

TEST_CASE("add noise: zero amount is a no-op, and the grain is a function of PARENT position") {
    const common::ImageF base = flatField(8, 8, {0.5f, 0.5f, 0.5f, 1.0f});

    common::ImageF none = base;
    fx::addNoiseImage(none, common::Affine2D::identity(), 0.0f, false, false, 1u);
    CHECK(none.rgba == base.rgba);

    // Two runs of the same placement must agree exactly: the sample is a hash, never a stream.
    common::ImageF a = base;
    common::ImageF b = base;
    fx::addNoiseImage(a, common::Affine2D::identity(), 0.1f, false, false, 3u);
    fx::addNoiseImage(b, common::Affine2D::identity(), 0.1f, false, false, 3u);
    CHECK(a.rgba == b.rgba);
    CHECK(a.rgba != base.rgba);

    // Shift the placement by a whole parent pixel: the grain must SLIDE WITH THE DOCUMENT, so
    // the offset buffer's (x, y) carries what the unshifted buffer had at (x + 3, y + 2). This is
    // the invariant that makes a dirty-rect recomposite reproduce the full composite exactly.
    common::ImageF shifted = base;
    fx::addNoiseImage(shifted, common::Affine2D::translation(3.0, 2.0), 0.1f, false, false, 3u);
    for (std::uint32_t y = 0; y + 2 < 8; ++y)
        for (std::uint32_t x = 0; x + 3 < 8; ++x) {
            CHECK(shifted.at(x, y).r == a.at(x + 3, y + 2).r);
            CHECK(shifted.at(x, y).g == a.at(x + 3, y + 2).g);
        }

    // A different seed is a different field.
    common::ImageF other = base;
    fx::addNoiseImage(other, common::Affine2D::identity(), 0.1f, false, false, 4u);
    CHECK(other.rgba != a.rgba);

    // Monochromatic: one sample per pixel, so a neutral base stays neutral; alpha never moves.
    common::ImageF mono = base;
    fx::addNoiseImage(mono, common::Affine2D::identity(), 0.1f, false, true, 3u);
    for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 8; ++x) {
            const common::ColorF c = mono.at(x, y);
            CHECK(c.r == c.g);
            CHECK(c.g == c.b);
            CHECK(c.a == 1.0f);
        }
    CHECK(allFinite(mono));
}

TEST_CASE("add noise: uniform and gaussian carry the same variance") {
    // Both distributions are scaled to std-dev `sigma`, so the Amount slider means one thing in
    // either mode. Measured over 64x64 samples the two sample std-devs must land in the same
    // ballpark (a loose window -- this pins the SCALING, not the RNG's tail behaviour).
    const auto spread = [](bool uniform) {
        common::ImageF img = flatField(64, 64, {0.5f, 0.5f, 0.5f, 1.0f});
        fx::addNoiseImage(img, common::Affine2D::identity(), 0.1f, uniform, true, 9u);
        double sum = 0.0;
        double sum2 = 0.0;
        for (std::size_t i = 0; i < img.pixelCount(); ++i) {
            const double d = static_cast<double>(img.rgba[i * 4]) - 0.5;
            sum += d;
            sum2 += d * d;
        }
        const double n = static_cast<double>(img.pixelCount());
        return std::sqrt(sum2 / n - (sum / n) * (sum / n));
    };
    const double g = spread(false);
    const double u = spread(true);
    CHECK(g > 0.05);
    CHECK(g < 0.16);
    CHECK(u > 0.05);
    CHECK(u < 0.16);
}

// ---------------------------------------------------------------------------------------------
// Denoise
// ---------------------------------------------------------------------------------------------

TEST_CASE("denoise: k -> 0 flattens, k -> 1 passes through") {
    const common::ImageF before = stepField(16, 16, 8, 0.0f, 1.0f);

    // A noise level far above the local variance drives k to 0 everywhere, so the output is the
    // local box mean: the pixel next to the edge must lift off zero, while one far from the edge
    // (its whole window dark) must stay at zero.
    common::ImageF flat = before;
    fx::denoiseImage(flat, 2, 1.0f);
    CHECK(flat.at(7, 8).r > 0.1f);
    CHECK(flat.at(1, 8).r == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(flat.at(7, 8).a == 1.0f);  // alpha untouched

    // A noise level far BELOW the local variance drives k to 1, so the edge survives.
    common::ImageF kept = before;
    fx::denoiseImage(kept, 2, 0.001f);
    CHECK(kept.at(7, 8).r == doctest::Approx(0.0f).epsilon(0.02));
    CHECK(kept.at(8, 8).r == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(allFinite(kept));

    // Zero noise level is a declared no-op.
    common::ImageF none = before;
    fx::denoiseImage(none, 2, 0.0f);
    CHECK(none.rgba == before.rgba);
}

// ---------------------------------------------------------------------------------------------
// Pixelate
// ---------------------------------------------------------------------------------------------

TEST_CASE("pixelate: cells are uniform and anchored to the PARENT lattice") {
    common::ImageF img(4, 4);
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 4; ++x)
            img.set(x, y, {static_cast<float>(x) * 0.25f, 0.5f, 0.75f, 1.0f});

    common::ImageF blocks = img;
    fx::pixelateImage(blocks, common::Affine2D::identity(), 2.0);
    // cell = floor(parent / 2): columns {0,1} and {2,3} share a mean, exactly.
    CHECK(blocks.at(0, 0).r == blocks.at(1, 0).r);
    CHECK(blocks.at(2, 0).r == blocks.at(3, 0).r);
    CHECK(blocks.at(0, 0).r != blocks.at(2, 0).r);
    CHECK(blocks.at(0, 0).r == doctest::Approx(0.125f));  // mean of 0.0 and 0.25
    CHECK(blocks.at(0, 0).a == 1.0f);

    // Shift the placement by one parent px: the lattice must follow the DOCUMENT, so the cell
    // boundaries move and column 0 ends up alone in its cell while 1 and 2 pair up.
    common::ImageF shifted = img;
    fx::pixelateImage(shifted, common::Affine2D::translation(1.0, 0.0), 2.0);
    CHECK(shifted.at(1, 0).r == shifted.at(2, 0).r);
    CHECK(shifted.at(0, 0).r != shifted.at(1, 0).r);
    CHECK(shifted.at(0, 0).r == doctest::Approx(0.0f));  // its own cell: its own value back
    CHECK(allFinite(shifted));
}

// ---------------------------------------------------------------------------------------------
// Emboss
// ---------------------------------------------------------------------------------------------

TEST_CASE("emboss: a flat field is exactly mid-gray, an edge lights one side") {
    common::ImageF flat = flatField(8, 8, {0.4f, 0.6f, 0.9f, 1.0f});
    fx::embossImage(flat, 2.0f, 0.0f, 1.0f);  // whole-pixel taps: the bilinear read is exact
    for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 8; ++x) {
            const common::ColorF c = flat.at(x, y);
            CHECK(c.r == 0.5f);
            CHECK(c.g == 0.5f);
            CHECK(c.b == 0.5f);
            CHECK(c.a == 1.0f);  // relief replaces colour, it never carves coverage
        }

    // A bright bar lit from +x: the RISING edge reads above mid-gray (the tap ahead is brighter
    // than the tap behind) and the FALLING edge below it, which is exactly the lit/shadowed pair
    // that makes relief read as relief. Flat ground stays at mid-gray.
    common::ImageF bar(16, 16);
    for (std::uint32_t y = 0; y < 16; ++y)
        for (std::uint32_t x = 0; x < 16; ++x) {
            const float v = (x >= 8 && x < 12) ? 1.0f : 0.0f;
            bar.set(x, y, {v, v, v, 1.0f});
        }
    fx::embossImage(bar, 2.0f, 0.0f, 1.0f);
    CHECK(bar.at(7, 8).r > 0.5f);   // rising edge: lit
    CHECK(bar.at(12, 8).r < 0.5f);  // falling edge: shadowed
    CHECK(bar.at(2, 8).r == 0.5f);  // away from both edges: flat
    CHECK(allFinite(bar));
}

// ---------------------------------------------------------------------------------------------
// Oil paint (Kuwahara)
// ---------------------------------------------------------------------------------------------

TEST_CASE("oil paint: a step edge survives -- no intermediate values are minted") {
    common::ImageF img = stepField(24, 24, 12, 0.0f, 1.0f);
    fx::oilPaintImage(img, 2);
    // Every pixel has at least one quadrant lying entirely on one side of the edge, and that
    // quadrant's variance is zero, so it always wins: the output is still two-valued. (This is
    // the whole reason Kuwahara reads as "paint" rather than "blur".)
    for (std::uint32_t y = 0; y < 24; ++y)
        for (std::uint32_t x = 0; x < 24; ++x) {
            const float v = img.at(x, y).r;
            CHECK((v < 0.01f || v > 0.99f));
            CHECK(img.at(x, y).a == 1.0f);
        }
    CHECK(allFinite(img));
}

TEST_CASE("oil paint: a flat field comes back flat") {
    common::ImageF img = flatField(16, 16, {0.3f, 0.6f, 0.2f, 1.0f});
    fx::oilPaintImage(img, 3);
    for (std::uint32_t y = 0; y < 16; ++y)
        for (std::uint32_t x = 0; x < 16; ++x) {
            CHECK(img.at(x, y).r == doctest::Approx(0.3f).epsilon(0.001));
            CHECK(img.at(x, y).g == doctest::Approx(0.6f).epsilon(0.001));
            CHECK(img.at(x, y).a == doctest::Approx(1.0f).epsilon(0.001));
        }
}

// ---------------------------------------------------------------------------------------------
// Wave / Ripple
// ---------------------------------------------------------------------------------------------

TEST_CASE("wave: zero amplitude is a no-op; a flat field survives any displacement") {
    const common::ImageF before = stepField(16, 16, 8, 0.2f, 0.9f);
    common::ImageF none = before;
    fx::WaveOp op;
    op.amplitude = 0.0;
    op.wavelength = 8.0;
    fx::waveImage(none, common::Affine2D::identity(), common::Affine2D::identity(), op);
    CHECK(none.rgba == before.rgba);

    // Clamp-to-edge sampling means a displacement can never pull in transparency or off-image
    // garbage: a flat opaque field stays exactly that, everywhere, in both modes.
    for (const bool ripple : {false, true}) {
        common::ImageF flat = flatField(16, 16, {0.4f, 0.4f, 0.4f, 1.0f});
        fx::WaveOp w;
        w.ripple = ripple;
        w.amplitude = 5.0;
        w.wavelength = 7.0;
        w.dirX = 1.0;
        w.dirY = 0.0;
        w.center = {8.0, 8.0};
        fx::waveImage(flat, common::Affine2D::identity(), common::Affine2D::identity(), w);
        for (std::uint32_t y = 0; y < 16; ++y)
            for (std::uint32_t x = 0; x < 16; ++x) {
                CHECK(flat.at(x, y).r == doctest::Approx(0.4f).epsilon(0.001));
                CHECK(flat.at(x, y).a == doctest::Approx(1.0f).epsilon(0.001));
            }
        CHECK(allFinite(flat));
    }
}

TEST_CASE("wave: a whole-pixel displacement is an exact shift") {
    // Phase 90 degrees puts sin() at 1 at the origin of the phase axis, so with amplitude 2 and a
    // wavelength that keeps the whole row on the same crest, the row slides by exactly 2 px and
    // the bilinear read lands on pixel centres -- an exact copy, not a resample.
    common::ImageF img(16, 4);
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 16; ++x)
            img.set(x, y, {static_cast<float>(x) / 16.0f, 0.0f, 0.0f, 1.0f});
    const common::ImageF before = img;

    fx::WaveOp op;
    op.ripple = false;
    op.amplitude = 2.0;
    op.wavelength = 1.0e9;   // effectively constant phase across this tiny image
    op.phase = 3.14159265358979323846 / 2.0;  // sin -> 1
    op.dirX = 1.0;
    op.dirY = 0.0;
    fx::waveImage(img, common::Affine2D::identity(), common::Affine2D::identity(), op);
    // Pull semantics: the output at x takes what sat at x - 2.
    for (std::uint32_t x = 2; x < 16; ++x)
        CHECK(img.at(x, 1).r == doctest::Approx(before.at(x - 2, 1).r).epsilon(0.002));
    CHECK(img.at(0, 1).r == doctest::Approx(before.at(0, 1).r).epsilon(0.002));  // clamped edge
}

// ---------------------------------------------------------------------------------------------
// Vignette
// ---------------------------------------------------------------------------------------------

TEST_CASE("vignette: the core is untouched byte for byte, the rim darkens") {
    const common::ImageF before = flatField(32, 32, {0.6f, 0.6f, 0.6f, 1.0f});

    // Zero exposure is a declared no-op.
    common::ImageF none = before;
    fx::VignetteOp z;
    z.center = {16.0, 16.0};
    z.radius = 8.0;
    z.outer = 1.5;
    z.exposure = 0.0f;
    fx::vignetteImage(none, common::Affine2D::identity(), z);
    CHECK(none.rgba == before.rgba);

    // A radius that swallows the whole image leaves every pixel inside q <= 1: also byte-exact,
    // and this is the case that proves there is no decode/encode round-trip error in the core.
    common::ImageF wide = before;
    fx::VignetteOp big;
    big.center = {16.0, 16.0};
    big.radius = 1000.0;
    big.outer = 2.0;
    big.exposure = -2.0f;
    fx::vignetteImage(wide, common::Affine2D::identity(), big);
    CHECK(wide.rgba == before.rgba);

    // A tight radius: centre untouched, corner takes the full -2 EV (a factor of 4 in linear
    // light, so the encoded value must fall well below half).
    common::ImageF vig = before;
    fx::VignetteOp op;
    op.center = {16.0, 16.0};
    op.radius = 4.0;
    op.outer = 1.5;
    op.exponent = 2.0;
    op.exposure = -2.0f;
    fx::vignetteImage(vig, common::Affine2D::identity(), op);
    CHECK(vig.at(16, 16).r == before.at(16, 16).r);
    CHECK(vig.at(0, 0).r < 0.4f);
    CHECK(vig.at(0, 0).a == 1.0f);  // it dims the backdrop, it does not erase it
    CHECK(allFinite(vig));
}
