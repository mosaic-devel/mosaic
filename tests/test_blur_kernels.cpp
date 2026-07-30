// S33 blur-kernel engine (docs/blur-filters.md §3/§9): analytic kernel signatures instead of
// golden pixels -- an impulse must land the kernel's own shape (Gaussian profile, flat box
// plateau, motion line, spin arc, zoom ray, aperture polygon), the bilateral must hold a step
// edge, the DoF field must keep its focus band byte-identical, and every kernel must diffuse
// alpha (a blur moves coverage, §2) without ever minting NaNs on transparent input.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

#include "common/image.hpp"
#include "render/blur.hpp"

using namespace mosaic;
namespace fx = mosaic::render::fx;

namespace {

common::ImageF flatField(std::uint32_t w, std::uint32_t h, common::ColorF c) {
    common::ImageF img(w, h);
    img.fill(c);
    return img;
}

// Sum of one straight channel over the whole image. On opaque fixtures straight == premul, so
// this is the "mass" the conservation checks track.
double channelSum(const common::ImageF& img, int ch) {
    double sum = 0.0;
    for (std::size_t i = 0; i < img.pixelCount(); ++i)
        sum += img.rgba[i * 4 + static_cast<std::size_t>(ch)];
    return sum;
}

bool allFinite(const common::ImageF& img) {
    for (float v : img.rgba)
        if (!std::isfinite(v))
            return false;
    return true;
}

float maxAbs(const common::ImageF& img) {
    float m = 0.0f;
    for (float v : img.rgba)
        m = std::max(m, std::abs(v));
    return m;
}

// The 5x5 opaque red square on a transparent field that every alpha-diffusion case starts
// from ([8..12]^2 in a 21x21 canvas).
common::ImageF squareOnTransparent() {
    common::ImageF img(21, 21);
    for (std::uint32_t y = 8; y <= 12; ++y)
        for (std::uint32_t x = 8; x <= 12; ++x)
            img.set(x, y, {1.0f, 0.0f, 0.0f, 1.0f});
    return img;
}

// Max alpha outside the square's original footprint -- nonzero once a kernel moved coverage.
float alphaBeyondSquare(const common::ImageF& img) {
    float m = 0.0f;
    for (std::uint32_t y = 0; y < img.height; ++y)
        for (std::uint32_t x = 0; x < img.width; ++x) {
            if (x >= 8 && x <= 12 && y >= 8 && y <= 12)
                continue;
            m = std::max(m, img.at(x, y).a);
        }
    return m;
}

// Analytic IEC 61966-2-1 decode (the reference test_adjustments.cpp pins its LUT pair
// against) -- the lens boost comparison integrates output energy in linear light with it.
double srgbDecodeRef(double e) {
    return e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Separable kernels
// ---------------------------------------------------------------------------------------------

TEST_CASE("blur: gaussian impulse -> symmetric monotone profile, conserved mass") {
    common::ImageF img = flatField(33, 33, {0.25f, 0.25f, 0.25f, 1.0f});
    img.set(16, 16, {1.0f, 1.0f, 1.0f, 1.0f});
    const common::ImageF before = img;
    fx::gaussianBlurImage(img, 0.0f);
    CHECK(img == before); // sigma <= 0: byte-exact no-op
    fx::gaussianBlurImage(img, -3.0f);
    CHECK(img == before);

    const double massBefore = channelSum(img, 0);
    fx::gaussianBlurImage(img, 2.0f);
    // The impulse sits 16 px from every edge and the support is 6 px, so clamp never bites
    // and the premultiplied mass must survive the round trip.
    CHECK(std::abs(channelSum(img, 0) - massBefore) < 1e-3);

    float prev = img.at(16, 16).r;
    CHECK(prev > 0.25f);
    for (int d = 1; d <= 6; ++d) {
        const std::uint32_t ud = static_cast<std::uint32_t>(d);
        const float right = img.at(16 + ud, 16).r;
        CHECK(std::abs(right - img.at(16 - ud, 16).r) < 1e-6f); // mirror symmetry
        CHECK(std::abs(right - img.at(16, 16 - ud).r) < 1e-6f); // separable => 4-fold
        CHECK(std::abs(right - img.at(16, 16 + ud).r) < 1e-6f);
        CHECK(right <= prev + 1e-7f); // monotone decay away from the impulse
        prev = right;
    }
    for (std::uint32_t x = 23; x < 33; ++x)
        CHECK(std::abs(img.at(x, 16).r - 0.25f) < 1e-6f); // flat beyond the 3-sigma support
    CHECK(std::abs(img.at(0, 0).a - 1.0f) < 1e-6f);       // opaque field stays opaque
}

TEST_CASE("blur: box impulse row -> flat plateau of width 2r+1") {
    common::ImageF img = flatField(33, 1, {0.0f, 0.0f, 0.0f, 1.0f});
    img.set(16, 0, {1.0f, 1.0f, 1.0f, 1.0f});
    const common::ImageF before = img;
    fx::boxBlurImage(img, 0);
    CHECK(img == before); // radius <= 0: byte-exact no-op
    fx::boxBlurImage(img, 3);
    const float share = 1.0f / 7.0f;
    for (std::uint32_t x = 0; x < 33; ++x) {
        const float expect = (x >= 13 && x <= 19) ? share : 0.0f;
        CHECK(std::abs(img.at(x, 0).r - expect) < 1e-6f); // equal weights, hard plateau edge
        CHECK(std::abs(img.at(x, 0).a - 1.0f) < 1e-6f);
    }
}

// ---------------------------------------------------------------------------------------------
// Directional gathers
// ---------------------------------------------------------------------------------------------

TEST_CASE("blur: motion smear stays on its axis") {
    const auto impulseField = [] {
        common::ImageF img = flatField(21, 21, {0.0f, 0.0f, 0.0f, 1.0f});
        img.set(10, 10, {1.0f, 1.0f, 1.0f, 1.0f});
        return img;
    };

    common::ImageF horiz = impulseField();
    fx::motionBlurImage(horiz, 0.0f, 6.0f, false);
    for (std::uint32_t y = 0; y < 21; ++y)
        for (std::uint32_t x = 0; x < 21; ++x)
            if (y != 10)
                CHECK(horiz.at(x, y).r < 1e-6f); // rows off the axis stay untouched
    CHECK(horiz.at(10, 10).r < 1.0f);            // the impulse spread out...
    CHECK(horiz.at(13, 10).r > 1e-3f);           // ...along +x
    CHECK(horiz.at(7, 10).r > 1e-3f);            // ...and -x (the segment is symmetric)

    common::ImageF vert = impulseField();
    fx::motionBlurImage(vert, static_cast<float>(std::numbers::pi / 2.0), 6.0f, false);
    for (std::uint32_t y = 0; y < 21; ++y)
        for (std::uint32_t x = 0; x < 21; ++x)
            if (x != 10)
                CHECK(vert.at(x, y).r < 1e-6f);
    CHECK(vert.at(10, 13).r > 1e-3f);
    CHECK(vert.at(10, 7).r > 1e-3f);
}

TEST_CASE("blur: spin spreads along the arc; centre pixel untouched") {
    common::ImageF img = flatField(41, 41, {0.0f, 0.0f, 0.0f, 1.0f});
    img.set(20, 20, {1.0f, 0.0f, 0.0f, 1.0f}); // the exact centre, pinned unchanged
    img.set(30, 20, {1.0f, 1.0f, 1.0f, 1.0f}); // impulse at radius 10, angle 0
    const common::ImageF before = img;
    fx::spinBlurImage(img, 20.0, 20.0, 0.0f, false);
    CHECK(img == before); // non-positive arc: byte-exact no-op
    fx::spinBlurImage(img, 20.0, 20.0, 60.0f, false);
    CHECK(img.at(20, 20) == common::ColorF{1.0f, 0.0f, 0.0f, 1.0f});

    // Ink lands on the impulse's own circle; every clearly different radius stays clean (the
    // 3 px guard band absorbs the bilinear reach of sqrt(2) around each tap).
    bool arcInk = false;
    for (std::uint32_t y = 0; y < 41; ++y)
        for (std::uint32_t x = 0; x < 41; ++x) {
            const double r = std::hypot(x - 20.0, y - 20.0);
            const bool isImpulse = (x == 30 && y == 20);
            if (!isImpulse && std::abs(r - 10.0) < 1.0 && img.at(x, y).g > 1e-3f)
                arcInk = true;
            if (r < 7.0 || r > 13.0)
                CHECK(img.at(x, y).g < 1e-6f);
        }
    CHECK(arcInk);
    CHECK(img.at(30, 20).g < 1.0f); // the impulse itself got smeared
}

TEST_CASE("blur: zoom streaks along the ray through the centre") {
    common::ImageF img = flatField(41, 41, {0.0f, 0.0f, 0.0f, 1.0f});
    img.set(32, 20, {1.0f, 1.0f, 1.0f, 1.0f}); // impulse 12 px right of the centre
    const common::ImageF before = img;
    fx::zoomBlurImage(img, 20.0, 20.0, 0.0f, false);
    CHECK(img == before); // non-positive fraction: byte-exact no-op
    fx::zoomBlurImage(img, 20.0, 20.0, 0.5f, false);
    // Rows two or more off the ray never see the impulse (their gather segments stay on
    // their own rays; only the immediate neighbour rows can touch it through bilinear reach).
    for (std::uint32_t y = 0; y < 41; ++y)
        for (std::uint32_t x = 0; x < 41; ++x)
            if (y <= 18 || y >= 22)
                CHECK(img.at(x, y).r < 1e-6f);
    // Pixels between the centre and the impulse sample only inward of themselves: clean.
    for (std::uint32_t x = 21; x <= 29; ++x)
        CHECK(img.at(x, 20).r < 1e-6f);
    // The outward stretch of the ray (segments crossing the impulse) catches the streak.
    for (std::uint32_t x = 33; x <= 38; ++x)
        CHECK(img.at(x, 20).r > 1e-4f);
    CHECK(img.at(32, 20).r < 1.0f);
}

// ---------------------------------------------------------------------------------------------
// Surface (bilateral)
// ---------------------------------------------------------------------------------------------

TEST_CASE("blur: surface preserves a hard step and flattens low-contrast noise") {
    common::ImageF step(32, 16);
    for (std::uint32_t y = 0; y < 16; ++y)
        for (std::uint32_t x = 0; x < 32; ++x) {
            const float v = x < 16 ? 0.2f : 0.8f;
            step.set(x, y, {v, v, v, 1.0f});
        }
    const common::ImageF ref = step;
    fx::surfaceBlurImage(step, 0.0f, 0.1f);
    CHECK(step == ref); // non-positive radius: byte-exact no-op
    fx::surfaceBlurImage(step, 4.0f, 0.1f);
    // The 0.6 luma jump is six range sigmas: cross-edge weights are ~exp(-18), so each side
    // must hold its flat value to well under a display quantum.
    for (std::uint32_t y = 0; y < 16; ++y)
        for (std::uint32_t x = 0; x < 32; ++x) {
            const float flat = x < 16 ? 0.2f : 0.8f;
            CHECK(std::abs(step.at(x, y).r - flat) < 2.0f / 255.0f);
        }

    // Low-contrast noise (well inside the threshold) must flatten: variance collapses.
    common::ImageF noise(16, 16);
    std::uint32_t lcg = 12345u;
    for (std::uint32_t y = 0; y < 16; ++y)
        for (std::uint32_t x = 0; x < 16; ++x) {
            lcg = lcg * 1664525u + 1013904223u;
            const float v =
                0.5f + 0.02f * (static_cast<float>((lcg >> 8) & 0xffu) / 127.5f - 1.0f);
            noise.set(x, y, {v, v, v, 1.0f});
        }
    const auto variance = [](const common::ImageF& im) {
        double mean = 0.0;
        for (std::size_t i = 0; i < im.pixelCount(); ++i)
            mean += im.rgba[i * 4];
        mean /= static_cast<double>(im.pixelCount());
        double var = 0.0;
        for (std::size_t i = 0; i < im.pixelCount(); ++i) {
            const double d = im.rgba[i * 4] - mean;
            var += d * d;
        }
        return var / static_cast<double>(im.pixelCount());
    };
    const double varBefore = variance(noise);
    fx::surfaceBlurImage(noise, 3.0f, 0.1f);
    CHECK(variance(noise) < varBefore * 0.5);
}

// ---------------------------------------------------------------------------------------------
// Aperture kernel + lens gather
// ---------------------------------------------------------------------------------------------

TEST_CASE("blur: aperture kernel is normalised, symmetric, and strides above the threshold") {
    for (const bool draft : {false, true})
        for (const float radius : {2.5f, 5.0f, 13.0f}) {
            const fx::ApertureKernel k = fx::makeApertureKernel(radius, 6, 0.35f, 0.4f, draft);
            double sum = 0.0;
            for (const float wgt : k.weight)
                sum += wgt;
            CHECK(std::abs(sum - 1.0) < 1e-5); // subsampled or not, mass renormalises to 1
            CHECK(k.radius == static_cast<int>(std::ceil(radius)));
        }

    // Curvature 1 is a disc: the build cannot depend on rotation.
    const fx::ApertureKernel disc0 = fx::makeApertureKernel(5.0f, 6, 1.0f, 0.0f, false);
    const fx::ApertureKernel disc1 = fx::makeApertureKernel(5.0f, 6, 1.0f, 0.7f, false);
    REQUIRE(disc0.weight.size() == disc1.weight.size());
    for (std::size_t i = 0; i < disc0.weight.size(); ++i) {
        CHECK(disc0.offX[i] == disc1.offX[i]);
        CHECK(disc0.offY[i] == disc1.offY[i]);
        CHECK(std::abs(disc0.weight[i] - disc1.weight[i]) < 1e-6f);
    }

    // Curvature 0 hexagon: rotating by one blade period reproduces the kernel (the 6-fold
    // symmetry of the aperture boundary), and 180 degrees = 3 periods makes the tap weights
    // point-symmetric on the integer lattice.
    const fx::ApertureKernel hex = fx::makeApertureKernel(5.0f, 6, 0.0f, 0.0f, false);
    const fx::ApertureKernel hexRot = fx::makeApertureKernel(
        5.0f, 6, 0.0f, static_cast<float>(std::numbers::pi / 3.0), false);
    REQUIRE(hex.weight.size() == hexRot.weight.size());
    for (std::size_t i = 0; i < hex.weight.size(); ++i)
        CHECK(std::abs(hex.weight[i] - hexRot.weight[i]) < 1e-6f);
    const auto weightAt = [&hex](float ox, float oy) -> float {
        for (std::size_t i = 0; i < hex.weight.size(); ++i)
            if (hex.offX[i] == ox && hex.offY[i] == oy)
                return hex.weight[i];
        return -1.0f;
    };
    for (std::size_t i = 0; i < hex.weight.size(); ++i) {
        const float mirrored = weightAt(-hex.offX[i], -hex.offY[i]);
        REQUIRE(mirrored >= 0.0f);
        CHECK(std::abs(mirrored - hex.weight[i]) < 1e-6f);
    }

    // The tap lattice stays exact up to the stride threshold and coarsens above it (full
    // quality at radius 12, draft at radius 6).
    CHECK(fx::makeApertureKernel(12.0f, 6, 0.5f, 0.0f, false).stride == 1);
    CHECK(fx::makeApertureKernel(13.0f, 6, 0.5f, 0.0f, false).stride == 2);
    CHECK(fx::makeApertureKernel(6.0f, 6, 0.5f, 0.0f, true).stride == 1);
    CHECK(fx::makeApertureKernel(7.0f, 6, 0.5f, 0.0f, true).stride == 2);
    const fx::ApertureKernel coarse = fx::makeApertureKernel(13.0f, 6, 0.5f, 0.0f, false);
    CHECK(coarse.stride == 2);
    for (std::size_t i = 0; i < coarse.offX.size(); ++i) {
        const int ox = static_cast<int>(coarse.offX[i]);
        CHECK((ox + coarse.radius) % coarse.stride == 0); // offsets ride the -R..R lattice
    }
}

TEST_CASE("blur: lens gather takes the aperture's shape; boost blooms highlights") {
    const auto impulseField = [] {
        common::ImageF img = flatField(31, 31, {0.0f, 0.0f, 0.0f, 1.0f});
        img.set(15, 15, {1.0f, 1.0f, 1.0f, 1.0f});
        return img;
    };
    const fx::ApertureKernel hex = fx::makeApertureKernel(5.0f, 6, 0.0f, 0.0f, false);

    common::ImageF img = impulseField();
    fx::lensBlurImage(img, hex, 0.0f, 0.0f);
    CHECK(allFinite(img));
    // Ink at p means the kernel holds a tap at (impulse - p). Offset (5,0) sits on a polygon
    // vertex (AA weight 0.5); offset (4,3) has d = 5 beyond the hexagon's mid-edge extent
    // (~4.36) so it is cut -- but the curvature-1 disc keeps it.
    CHECK(img.at(15, 15).r > 1e-4f);
    CHECK(img.at(10, 15).r > 1e-5f);
    CHECK(img.at(11, 12).r < 1e-6f);
    CHECK(img.at(22, 15).r < 1e-6f); // beyond the support radius entirely
    common::ImageF disc = impulseField();
    fx::lensBlurImage(disc, fx::makeApertureKernel(5.0f, 6, 1.0f, 0.0f, false), 0.0f, 0.0f);
    CHECK(disc.at(11, 12).r > 1e-6f);

    // Boost: the white impulse's linear luma 1.0 exceeds the 0.5 threshold, so it gathers
    // with gain 1 + 4*0.75 = 4 and is never un-boosted -- the output must carry visibly more
    // linear energy than the plain gather.
    common::ImageF plain = impulseField();
    common::ImageF boosted = impulseField();
    fx::lensBlurImage(plain, hex, 0.0f, 0.0f);
    fx::lensBlurImage(boosted, hex, 0.75f, 0.5f);
    double linPlain = 0.0;
    double linBoost = 0.0;
    for (std::size_t i = 0; i < plain.pixelCount(); ++i) {
        linPlain += srgbDecodeRef(plain.rgba[i * 4]);
        linBoost += srgbDecodeRef(boosted.rgba[i * 4]);
    }
    CHECK(linBoost > 2.0 * linPlain);
}

// ---------------------------------------------------------------------------------------------
// Depth of field
// ---------------------------------------------------------------------------------------------

TEST_CASE("blur: dof field semantics -- focus band byte-identical, plateau equals top level") {
    // Content with both colour structure and an alpha edge, so a premul slip would show.
    common::ImageF img(24, 24);
    for (std::uint32_t y = 0; y < 24; ++y)
        for (std::uint32_t x = 0; x < 12; ++x)
            img.set(x, y, {static_cast<float>(x) / 23.0f, 0.3f,
                           1.0f - static_cast<float>(x) / 23.0f, 1.0f});
    for (std::uint32_t y = 8; y <= 15; ++y)
        for (std::uint32_t x = 14; x <= 21; ++x)
            img.set(x, y, {1.0f, 0.0f, 0.0f, 1.0f});
    const common::ImageF original = img;
    const float maxRadius = 4.0f;

    const std::vector<float> zeroField(img.pixelCount(), 0.0f);
    fx::dofBlurImage(img, zeroField, maxRadius, false, false);
    CHECK(img == original); // an all-zero field returns the backdrop byte-identically

    // Guard rails: a wrong-sized field or a non-positive radius must not touch the image.
    const std::vector<float> shortField(img.pixelCount() - 1, 1.0f);
    fx::dofBlurImage(img, shortField, maxRadius, false, false);
    CHECK(img == original);
    const std::vector<float> fullField(img.pixelCount(), maxRadius);
    fx::dofBlurImage(img, fullField, 0.0f, false, false);
    CHECK(img == original);

    // A saturated field returns the top level exactly == a Gaussian at maxRadius/2.
    common::ImageF expected = original;
    fx::gaussianBlurImage(expected, maxRadius / 2.0f);
    fx::dofBlurImage(img, fullField, maxRadius, false, false);
    for (std::size_t i = 0; i < img.rgba.size(); ++i)
        CHECK(std::abs(img.rgba[i] - expected.rgba[i]) < 1e-5f);

    // Half in focus, half saturated: the focus half keeps its exact bytes even though the
    // other half blurs (the mask-the-subject-out workflow's honesty invariant).
    img = original;
    std::vector<float> halfField(img.pixelCount(), 0.0f);
    for (std::uint32_t y = 0; y < 24; ++y)
        for (std::uint32_t x = 12; x < 24; ++x)
            halfField[static_cast<std::size_t>(y) * 24 + x] = maxRadius;
    fx::dofBlurImage(img, halfField, maxRadius, false, false);
    for (std::uint32_t y = 0; y < 24; ++y)
        for (std::uint32_t x = 0; x < 24; ++x) {
            const common::ColorF want = x < 12 ? original.at(x, y) : expected.at(x, y);
            CHECK(img.at(x, y) == want);
        }

    // A mid-field value engages the premultiplied lerp between adjacent levels.
    img = original;
    const std::vector<float> midField(img.pixelCount(), maxRadius * 0.6f);
    fx::dofBlurImage(img, midField, maxRadius, false, false);
    CHECK(allFinite(img));
    CHECK(img != original);

    // Iris mode runs the aperture gather per level: finite and visibly different.
    img = original;
    fx::dofBlurImage(img, fullField, maxRadius, true, false);
    CHECK(allFinite(img));
    CHECK(img != original);
}

// ---------------------------------------------------------------------------------------------
// Cross-kernel invariants
// ---------------------------------------------------------------------------------------------

TEST_CASE("blur: every kernel diffuses alpha and survives fully transparent input") {
    const fx::ApertureKernel lensK = fx::makeApertureKernel(3.0f, 6, 0.5f, 0.0f, false);
    const std::vector<float> dofField(21u * 21u, 4.0f);
    const auto applyEach = [&](int which, common::ImageF& img) {
        switch (which) {
        case 0: fx::gaussianBlurImage(img, 2.0f); break;
        case 1: fx::boxBlurImage(img, 2); break;
        case 2: fx::motionBlurImage(img, 0.7f, 6.0f, false); break;
        case 3: fx::spinBlurImage(img, 4.0, 10.0, 80.0f, false); break;
        case 4: fx::zoomBlurImage(img, 4.0, 10.0, 0.5f, false); break;
        case 5: fx::surfaceBlurImage(img, 3.0f, 1.0f); break;
        case 6: fx::lensBlurImage(img, lensK, 0.0f, 0.0f); break;
        case 7: fx::dofBlurImage(img, dofField, 4.0f, false, false); break;
        case 8: fx::dofBlurImage(img, dofField, 4.0f, true, false); break;
        }
    };
    for (int which = 0; which < 9; ++which) {
        CAPTURE(which);
        // Coverage must MOVE: alpha appears beyond the square's original footprint.
        common::ImageF square = squareOnTransparent();
        applyEach(which, square);
        CHECK(allFinite(square));
        CHECK(alphaBeyondSquare(square) > 1e-4f);
        // A fully transparent canvas must stay empty -- and NaN-free (the unpremultiply's
        // divide-by-alpha is the classic source).
        common::ImageF clear(16, 16);
        applyEach(which, clear);
        CHECK(allFinite(clear));
        CHECK(maxAbs(clear) < 1e-7f);
        // An empty image is a structural no-op for every kernel.
        common::ImageF empty;
        applyEach(which, empty);
        CHECK(empty.rgba.empty());
    }
}

TEST_CASE("blur: non-positive amounts are byte-exact no-ops (no premul round trip)") {
    // Fractional alpha makes a premultiply/unpremultiply round trip observable at the ulp
    // level, so byte-equality proves the early-outs really skip it.
    common::ImageF img(9, 7);
    for (std::uint32_t y = 0; y < 7; ++y)
        for (std::uint32_t x = 0; x < 9; ++x)
            img.set(x, y, {0.10f + 0.08f * static_cast<float>(x),
                           0.05f + 0.10f * static_cast<float>(y),
                           0.90f - 0.07f * static_cast<float>(x),
                           0.30f + 0.05f * static_cast<float>(y)});
    const common::ImageF before = img;
    fx::gaussianBlurImage(img, 0.0f);
    CHECK(img == before);
    fx::boxBlurImage(img, 0);
    CHECK(img == before);
    fx::motionBlurImage(img, 1.0f, 0.0f, false);
    CHECK(img == before);
    fx::spinBlurImage(img, 4.0, 3.0, 0.0f, false);
    CHECK(img == before);
    fx::zoomBlurImage(img, 4.0, 3.0, 0.0f, false);
    CHECK(img == before);
    fx::surfaceBlurImage(img, 0.0f, 0.5f);
    CHECK(img == before);
    fx::lensBlurImage(img, fx::makeApertureKernel(0.0f, 6, 0.5f, 0.0f, false), 0.5f, 0.1f);
    CHECK(img == before); // a radius-0 aperture is the identity gather: skipped whole
    fx::dofBlurImage(img, std::vector<float>(img.pixelCount(), 1.0f), 0.0f, false, false);
    CHECK(img == before);
}
