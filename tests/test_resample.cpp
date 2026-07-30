#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <vector>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "render/resample.hpp"

using namespace mosaic;
using render::ResampleFilter;

// ---------------------------------------------------------------------------------------------
// render/resample.hpp -- the kernel bank and the whole-image samplers extracted from the
// compositor in S53-a. The kernels are pinned against their CLOSED FORMS rather than against a
// recorded output: there is no bless/update mechanism in this repo, and a formula that has a
// closed form should be checked against the formula, not against yesterday's numbers.
// shaders/composite_tile.comp mirrors the same math, so a golden that drifts here drifts there.
// ---------------------------------------------------------------------------------------------
namespace {

// A LINEAR ramp is the right round-trip probe: every kernel here is symmetric and weight-
// normalised, so it reproduces a linear signal exactly away from the edges. Any interior
// deviation past 8-bit rounding is therefore a real defect, not the kernel doing its job.
common::Image rampImage(std::uint32_t w, std::uint32_t h) {
    common::Image img(w, h);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
            const auto v = static_cast<std::uint8_t>(x * 8);
            img.rgba[p] = v;
            img.rgba[p + 1] = v;
            img.rgba[p + 2] = v;
            img.rgba[p + 3] = 255;
        }
    }
    return img;
}

common::Color8 px(const common::Image& img, std::uint32_t x, std::uint32_t y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

int diff(std::uint8_t a, int b) { return std::abs(static_cast<int>(a) - b); }

// Every implemented kernel (Auto resolves to one of these before any sampling happens).
constexpr ResampleFilter kAllFilters[] = {
    ResampleFilter::Nearest,   ResampleFilter::Bilinear, ResampleFilter::Bicubic,
    ResampleFilter::Mitchell,  ResampleFilter::Lanczos2, ResampleFilter::Lanczos3,
    ResampleFilter::Area,      ResampleFilter::Gaussian, ResampleFilter::Supersample};

// The windowed-sinc closed form, written out here so the test does not just re-run the
// implementation: L_a(x) = sinc(x) * sinc(x/a), sinc(x) = sin(pi x) / (pi x).
double lanczosClosedForm(double x, double a) {
    const auto s = [](double v) {
        if (v == 0.0) return 1.0;
        return std::sin(std::numbers::pi * v) / (std::numbers::pi * v);
    };
    x = std::abs(x);
    return x < a ? s(x) * s(x / a) : 0.0;
}

}  // namespace

TEST_CASE("kernelRadius reports each kernel's half-support in source texels") {
    CHECK(render::kernelRadius(ResampleFilter::Area) == 0.5);
    CHECK(render::kernelRadius(ResampleFilter::Bilinear) == 1.0);
    CHECK(render::kernelRadius(ResampleFilter::Bicubic) == 2.0);
    CHECK(render::kernelRadius(ResampleFilter::Mitchell) == 2.0);
    CHECK(render::kernelRadius(ResampleFilter::Lanczos2) == 2.0);
    CHECK(render::kernelRadius(ResampleFilter::Gaussian) == 2.0);
    CHECK(render::kernelRadius(ResampleFilter::Lanczos3) == 3.0);
}

TEST_CASE("kernelWeight matches each kernel's closed form at known offsets") {
    using render::kernelWeight;
    // Box: 1 strictly inside the half-texel, 0 from the boundary outward.
    CHECK(kernelWeight(ResampleFilter::Area, 0.0) == 1.0);
    CHECK(kernelWeight(ResampleFilter::Area, 0.4) == 1.0);
    CHECK(kernelWeight(ResampleFilter::Area, 0.5) == 0.0);
    CHECK(kernelWeight(ResampleFilter::Area, 0.6) == 0.0);
    // Triangle.
    CHECK(kernelWeight(ResampleFilter::Bilinear, 0.0) == doctest::Approx(1.0));
    CHECK(kernelWeight(ResampleFilter::Bilinear, 0.25) == doctest::Approx(0.75));
    CHECK(kernelWeight(ResampleFilter::Bilinear, -0.25) == doctest::Approx(0.75));  // even
    CHECK(kernelWeight(ResampleFilter::Bilinear, 1.0) == 0.0);
    CHECK(kernelWeight(ResampleFilter::Bilinear, 1.5) == 0.0);
    // Catmull-Rom, the BC-spline at (0, 1/2):
    //   |x| < 1: 1.5|x|^3 - 2.5|x|^2 + 1     |x| < 2: -0.5|x|^3 + 2.5|x|^2 - 4|x| + 2
    CHECK(kernelWeight(ResampleFilter::Bicubic, 0.0) == doctest::Approx(1.0));
    CHECK(kernelWeight(ResampleFilter::Bicubic, 0.5) == doctest::Approx(0.5625));
    CHECK(kernelWeight(ResampleFilter::Bicubic, 1.0) == doctest::Approx(0.0));  // interpolating
    CHECK(kernelWeight(ResampleFilter::Bicubic, 1.5) == doctest::Approx(-0.0625));  // ringing lobe
    CHECK(kernelWeight(ResampleFilter::Bicubic, 2.0) == doctest::Approx(0.0));
    // Mitchell, the BC-spline at (1/3, 1/3): 8/9 at the centre, 1/18 at the first integer tap
    // (approximating, so it does NOT vanish there the way Catmull-Rom does).
    CHECK(kernelWeight(ResampleFilter::Mitchell, 0.0) == doctest::Approx(8.0 / 9.0));
    CHECK(kernelWeight(ResampleFilter::Mitchell, 1.0) == doctest::Approx(1.0 / 18.0));
    CHECK(kernelWeight(ResampleFilter::Mitchell, 2.0) == doctest::Approx(0.0));
    // Windowed sinc, against the closed form (and interpolating at every integer tap).
    for (double x : {0.25, 0.5, 0.9, 1.3, 1.75}) {
        CAPTURE(x);
        CHECK(kernelWeight(ResampleFilter::Lanczos2, x) ==
              doctest::Approx(lanczosClosedForm(x, 2.0)));
        CHECK(kernelWeight(ResampleFilter::Lanczos3, x) ==
              doctest::Approx(lanczosClosedForm(x, 3.0)));
    }
    CHECK(kernelWeight(ResampleFilter::Lanczos3, 0.0) == doctest::Approx(1.0));
    for (double k : {1.0, 2.0}) {
        CAPTURE(k);
        CHECK(kernelWeight(ResampleFilter::Lanczos2, k) == doctest::Approx(0.0));
        CHECK(kernelWeight(ResampleFilter::Lanczos3, k) == doctest::Approx(0.0));
    }
    CHECK(kernelWeight(ResampleFilter::Lanczos2, 2.5) == 0.0);  // past support
    CHECK(kernelWeight(ResampleFilter::Lanczos3, 3.5) == 0.0);
    // Gaussian with sigma = 0.5 source texels: exp(-2 x^2).
    CHECK(kernelWeight(ResampleFilter::Gaussian, 0.0) == doctest::Approx(1.0));
    CHECK(kernelWeight(ResampleFilter::Gaussian, 0.5) == doctest::Approx(std::exp(-0.5)));
    CHECK(kernelWeight(ResampleFilter::Gaussian, 1.0) == doctest::Approx(std::exp(-2.0)));
}

TEST_CASE("the interpolating kernels are a partition of unity at a fractional offset") {
    // Weights at the integer taps around a sample point must sum to 1, or a flat field would
    // change brightness with sub-pixel position. (Lanczos deliberately does NOT satisfy this --
    // it is a windowed sinc -- which is exactly why convolveInto normalises by the weight sum.)
    for (double frac : {0.1, 0.3, 0.5, 0.75}) {
        CAPTURE(frac);
        double bilinear = 0.0, bicubic = 0.0, area = 0.0;
        for (int k = -3; k <= 3; ++k) {
            const double t = static_cast<double>(k) - frac;
            bilinear += render::kernelWeight(ResampleFilter::Bilinear, t);
            bicubic += render::kernelWeight(ResampleFilter::Bicubic, t);
            area += render::kernelWeight(ResampleFilter::Area, t);
        }
        CHECK(bilinear == doctest::Approx(1.0));
        CHECK(bicubic == doctest::Approx(1.0));
        if (frac != 0.5)  // the box's own boundary: both neighbours fall exactly on the edge
            CHECK(area == doctest::Approx(1.0));
    }
}

TEST_CASE("isLosslessGrid accepts exactly the grid-preserving placements") {
    using A = common::Affine2D;
    CHECK(render::isLosslessGrid(A::identity()));
    CHECK(render::isLosslessGrid(A::translation(7, -3)));
    CHECK(render::isLosslessGrid(A::scaling(3, 3)));
    CHECK(render::isLosslessGrid(A::scaling(-1, 1)));         // horizontal flip
    CHECK(render::isLosslessGrid(A{0, -1, 12, 1, 0, 0}));     // 90-degree turn
    CHECK_FALSE(render::isLosslessGrid(A::translation(0.5, 0)));
    CHECK_FALSE(render::isLosslessGrid(A::scaling(1.5, 1.5)));
    CHECK_FALSE(render::isLosslessGrid(A::rotation(0.3)));
}

TEST_CASE("resolveFilter passes an explicit kernel through and resolves Auto by intent") {
    using A = common::Affine2D;
    // An explicit pick is honoured whatever the placement -- the user's choice is not second-
    // guessed (which is also why an orientation op must never be handed a smoothing filter).
    CHECK(render::resolveFilter(ResampleFilter::Lanczos3, A::identity()) == ResampleFilter::Lanczos3);
    CHECK(render::resolveFilter(ResampleFilter::Nearest, A::rotation(0.3)) == ResampleFilter::Nearest);
    // Auto buckets by intent (chooseAutoFilter).
    CHECK(render::resolveFilter(ResampleFilter::Auto, A::translation(4, 4)) ==
          ResampleFilter::Nearest);
    CHECK(render::resolveFilter(ResampleFilter::Auto, A::scaling(0.4, 0.4)) == ResampleFilter::Area);
    CHECK(render::resolveFilter(ResampleFilter::Auto, A::rotation(0.5)) == ResampleFilter::Lanczos3);
    CHECK(render::resolveFilter(ResampleFilter::Auto, A::rotation(0.5), /*liveDrag=*/true) ==
          ResampleFilter::Bilinear);
}

TEST_CASE("resampleImage returns the source bit-exactly for an exact-size or degenerate request") {
    const common::Image src = rampImage(16, 4);
    for (ResampleFilter f : kAllFilters) {
        INFO("filter=" << render::resampleFilterName(f));
        CHECK(render::resampleImage(src, 16, 4, f).rgba == src.rgba);
    }
    CHECK(render::resampleImage(src, 0, 4, ResampleFilter::Lanczos3).empty());
    CHECK(render::resampleImage(common::Image{}, 8, 8, ResampleFilter::Lanczos3).empty());
}

TEST_CASE("resampleImage: a 2x up / 2x down round trip reproduces a linear ramp for every filter") {
    // Symmetric normalised kernels reproduce a linear signal exactly, so the interior of the round
    // trip must come back to the ramp within 8-bit rounding. The 4-px margin is the honest
    // exclusion: near an edge every kernel reads outside the source, where there is nothing.
    const common::Image src = rampImage(32, 8);
    for (ResampleFilter f : kAllFilters) {
        INFO("filter=" << render::resampleFilterName(f));
        const common::Image up = render::resampleImage(src, 64, 16, f);
        REQUIRE(up.width == 64);
        REQUIRE(up.height == 16);
        const common::Image back = render::resampleImage(up, 32, 8, f);
        REQUIRE(back.width == 32);
        for (std::uint32_t y = 0; y < 8; ++y) {
            for (std::uint32_t x = 4; x < 28; ++x) {
                CAPTURE(x);
                CAPTURE(y);
                CHECK(diff(px(back, x, y).r, static_cast<int>(x * 8)) <= 3);
                CHECK(px(back, x, y).a == 255);
            }
        }
    }
}

TEST_CASE("resampleImage is deterministic byte for byte across repeated runs") {
    // The passes run on a shared thread pool; the band split touches disjoint ranges, so the
    // result may not depend on how the bands happen to be scheduled.
    const common::Image src = rampImage(64, 40);
    for (ResampleFilter f : {ResampleFilter::Lanczos3, ResampleFilter::Supersample,
                             ResampleFilter::Area, ResampleFilter::Mitchell}) {
        INFO("filter=" << render::resampleFilterName(f));
        const common::Image a = render::resampleImage(src, 23, 17, f);
        const common::Image b = render::resampleImage(src, 23, 17, f);
        CHECK(a.rgba == b.rgba);
    }
}

TEST_CASE("the footprint cap keeps an extreme minification from reaching far-away source texels") {
    // kMaxFootprintRadius bounds the per-pixel tap count under a heavy shrink: at 40x, Lanczos3's
    // unclamped support would be 3 * 40 = 120 source texels, so a band 100 texels away would still
    // carry weight. Capped at 8, the first destination pixel (its centre at source x = 20) can only
    // see source columns 12..27 -- so a band OUTSIDE that window must contribute nothing, and one
    // INSIDE it must contribute everything. The trade is stated in the header: an extreme shrink
    // aliases a little rather than taking thousands of taps per pixel.
    REQUIRE(render::kMaxFootprintRadius == 8.0);
    const auto bandImage = [](std::uint32_t x0, std::uint32_t x1) {
        common::Image img(800, 4);
        for (std::uint32_t y = 0; y < 4; ++y)
            for (std::uint32_t x = x0; x < x1; ++x) {
                const std::size_t p = (static_cast<std::size_t>(y) * 800 + x) * 4;
                img.rgba[p] = 255;
                img.rgba[p + 3] = 255;
            }
        return img;
    };
    const common::Image distant = render::resampleImage(bandImage(100, 111), 20, 4,
                                                        ResampleFilter::Lanczos3);
    REQUIRE(distant.width == 20);
    CHECK(px(distant, 0, 0).a == 0);  // outside the capped footprint: no contribution at all
    const common::Image inside = render::resampleImage(bandImage(12, 28), 20, 4,
                                                       ResampleFilter::Lanczos3);
    REQUIRE(inside.width == 20);
    CHECK(px(inside, 0, 0).a > 200);  // inside it: the band is what that pixel is made of
}

TEST_CASE("transformImage: an exact placement is a byte-exact copy for every filter") {
    // The identity and whole-pixel translations take the copy fast path, so the chosen kernel
    // cannot change them -- the invariant the compositor's Move-tool hot path depends on.
    const common::Image src = rampImage(12, 6);
    for (ResampleFilter f : kAllFilters) {
        INFO("filter=" << render::resampleFilterName(f));
        CHECK(render::transformImage(src, common::Affine2D::identity(), 12, 6, f).rgba == src.rgba);
        const common::Image shifted =
            render::transformImage(src, common::Affine2D::translation(3, 2), 12, 6, f);
        REQUIRE(shifted.width == 12);
        for (std::uint32_t y = 2; y < 6; ++y)
            for (std::uint32_t x = 3; x < 12; ++x)
                CHECK(px(shifted, x, y) == px(src, x - 3, y - 2));
        CHECK(px(shifted, 0, 0).a == 0);  // the vacated band is transparent
    }
}

TEST_CASE("transformImageF places a float source through an affine and drops a singular one") {
    common::ImageF src(4, 4);
    src.set(1, 1, {1.0f, 0.0f, 0.0f, 1.0f});
    const common::ImageF moved = render::transformImageF(src, common::Affine2D::translation(2, 1),
                                                         8, 8, ResampleFilter::Nearest);
    REQUIRE(moved.width == 8);
    CHECK(moved.at(3, 2).r == 1.0f);
    CHECK(moved.at(3, 2).a == 1.0f);
    CHECK(moved.at(1, 1).a == 0.0f);
    // A singular placement collapses to nothing rather than dividing by zero.
    const common::ImageF none =
        render::transformImageF(src, common::Affine2D::scaling(0.0, 1.0), 8, 8,
                                ResampleFilter::Nearest);
    REQUIRE(none.width == 8);
    bool allZero = true;
    for (float v : none.rgba)
        allZero = allZero && v == 0.0f;
    CHECK(allZero);
}
