#include "common/geometry.hpp"
#include "common/image.hpp"
#include "render/resample.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <doctest/doctest.h>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

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

// ---------------------------------------------------------------------------------------------
// convolveInto's AXIS-ALIGNED weight tables.
//
// When a placement has no rotation and no shear, the x-weights depend only on the destination
// column and the y-weights only on the row, so convolveInto hoists both out of the pixel loop into
// tables. That is a pure motion of work -- but it is a SECOND implementation of where the taps
// land, and the two are only ever compared here. The reference below re-derives the footprint and
// the weight sum straight from the definition (per texel, per tap, no tables), so a table that
// drifts by a column, caps a span early, or loses the all-zero-weight skip fails against the math
// rather than against a recorded picture.
//
// This is the shape the type stack composites in: text_layer_render bakes the layer transform's
// linear part into the glyph cache, so what reaches the compositor is a bitmap at 1:1 under a
// FRACTIONAL translation -- axis-aligned, sub-pixel, and resolved to Lanczos3 by Auto.
// ---------------------------------------------------------------------------------------------
namespace {

// One destination texel, straight from the definition in resample.hpp's header comment:
// premultiplied accumulation over the kernel footprint, normalised by the weight sum.
common::ColorF referenceTexel(const common::ImageF& src, const common::Affine2D& inv,
                              ResampleFilter f, std::uint32_t x, std::uint32_t y) {
    const double sclX = std::max(1.0, std::hypot(inv.m00, inv.m10));
    const double sclY = std::max(1.0, std::hypot(inv.m01, inv.m11));
    const double rx = std::min(render::kernelRadius(f) * sclX, render::kMaxFootprintRadius);
    const double ry = std::min(render::kernelRadius(f) * sclY, render::kMaxFootprintRadius);
    const common::Vec2 p = inv.apply({x + 0.5, y + 0.5});
    double pr = 0, pg = 0, pb = 0, pa = 0, wsum = 0;
    for (long sy = static_cast<long>(std::ceil(p.y - 0.5 - ry));
         sy <= static_cast<long>(std::floor(p.y - 0.5 + ry)); ++sy) {
        const double wy = render::kernelWeight(f, ((sy + 0.5) - p.y) / sclY);
        for (long sx = static_cast<long>(std::ceil(p.x - 0.5 - rx));
             sx <= static_cast<long>(std::floor(p.x - 0.5 + rx)); ++sx) {
            const double wgt = wy * render::kernelWeight(f, ((sx + 0.5) - p.x) / sclX);
            if (wgt == 0.0)
                continue;
            const bool inside = sx >= 0 && sy >= 0 && sx < static_cast<long>(src.width) &&
                                sy < static_cast<long>(src.height);
            const common::ColorF c =
                inside ? src.at(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy))
                       : common::ColorF{0.0f, 0.0f, 0.0f, 0.0f};
            const double aw = static_cast<double>(c.a) * wgt;
            pr += static_cast<double>(c.r) * aw;
            pg += static_cast<double>(c.g) * aw;
            pb += static_cast<double>(c.b) * aw;
            pa += aw;
            wsum += wgt;
        }
    }
    if (wsum <= 0.0)
        return {0.0f, 0.0f, 0.0f, 0.0f};
    common::ColorF out{0.0f, 0.0f, 0.0f, static_cast<float>(std::clamp(pa / wsum, 0.0, 1.0))};
    if (pa > 1e-8) {
        out.r = static_cast<float>(pr / pa);
        out.g = static_cast<float>(pg / pa);
        out.b = static_cast<float>(pb / pa);
    }
    return out;
}

common::ImageF noiseImageF(std::uint32_t w, std::uint32_t h, std::uint32_t seed) {
    common::ImageF img(w, h);
    std::uint32_t s = seed | 1u;
    const auto next = [&s] { // xorshift32: deterministic, and not a ramp any kernel reproduces
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(s & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
    };
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            img.set(x, y, {next(), next(), next(), next()});
    return img;
}

} // namespace

TEST_CASE("convolveInto's axis-aligned weight tables agree with the per-texel definition") {
    const common::ImageF src = noiseImageF(23, 17, 0xC0FFEEu);
    const auto fetch = [&src](long sx, long sy, float out[4]) {
        if (sx < 0 || sy < 0 || sx >= static_cast<long>(src.width) ||
            sy >= static_cast<long>(src.height)) {
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            return;
        }
        const std::size_t sp =
            (static_cast<std::size_t>(sy) * src.width + static_cast<std::size_t>(sx)) * 4;
        out[0] = src.rgba[sp + 0];
        out[1] = src.rgba[sp + 1];
        out[2] = src.rgba[sp + 2];
        out[3] = src.rgba[sp + 3];
    };
    // Every axis-aligned family the tables must cover: the sub-pixel translation a placed text
    // cache arrives as, an enlargement, a minification (which widens the footprint), and a flip
    // (a negative column, so the tap order runs backwards).
    struct Placement {
        const char* what;
        common::Affine2D t;
    };
    const Placement placements[] = {
        {"sub-pixel translate", common::Affine2D::translation(4.37, 2.81)},
        {"whole translate", common::Affine2D::translation(5.0, 3.0)},
        {"enlarge + translate",
         common::Affine2D::translation(3.25, 1.5) * common::Affine2D::scaling(2.4, 1.7)},
        {"minify + translate",
         common::Affine2D::translation(2.1, 0.9) * common::Affine2D::scaling(0.45, 0.6)},
        {"flip x", common::Affine2D::translation(30.0, 2.0) * common::Affine2D::scaling(-1.3, 1.1)},
    };
    constexpr ResampleFilter kConvolving[] = {ResampleFilter::Bilinear, ResampleFilter::Bicubic,
                                              ResampleFilter::Mitchell, ResampleFilter::Lanczos2,
                                              ResampleFilter::Lanczos3, ResampleFilter::Area,
                                              ResampleFilter::Gaussian};
    constexpr std::uint32_t kW = 48, kH = 36;
    std::size_t compared = 0;
    for (const Placement& pl : placements) {
        const std::optional<common::Affine2D> inv = pl.t.inverse();
        REQUIRE(inv.has_value());
        for (ResampleFilter f : kConvolving) {
            INFO("placement=" << std::string(pl.what)
                              << " filter=" << render::resampleFilterName(f));
            // Both with and without the source-extent clip: the clip moves the walk's bounds, and
            // the tables are indexed off those bounds.
            for (int clip = 0; clip < 2; ++clip) {
                common::ImageF dst(kW, kH);
                render::convolveInto(dst, kW, kH, *inv, f, fetch, clip ? src.width : 0,
                                     clip ? src.height : 0);
                for (std::uint32_t y = 0; y < kH; ++y) {
                    for (std::uint32_t x = 0; x < kW; ++x) {
                        const common::ColorF want = referenceTexel(src, *inv, f, x, y);
                        const common::ColorF got = dst.at(x, y);
                        // Bit-exact: the tables hold the same doubles the pixel loop computed,
                        // accumulated in the same order. Any drift here is a real defect.
                        CHECK(got.r == want.r);
                        CHECK(got.g == want.g);
                        CHECK(got.b == want.b);
                        CHECK(got.a == want.a);
                        ++compared;
                    }
                }
            }
        }
    }
    CHECK(compared == 5u * 7u * 2u * kW * kH);
}

TEST_CASE("a rotated placement still agrees with the per-texel definition") {
    // The general (non-axis-aligned) arm keeps evaluating the kernel per texel; the table arm must
    // not be reachable for it. Same reference, one sheared placement.
    const common::ImageF src = noiseImageF(19, 21, 0xBADCAFEu);
    const auto fetch = [&src](long sx, long sy, float out[4]) {
        if (sx < 0 || sy < 0 || sx >= static_cast<long>(src.width) ||
            sy >= static_cast<long>(src.height)) {
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            return;
        }
        const std::size_t sp =
            (static_cast<std::size_t>(sy) * src.width + static_cast<std::size_t>(sx)) * 4;
        out[0] = src.rgba[sp + 0];
        out[1] = src.rgba[sp + 1];
        out[2] = src.rgba[sp + 2];
        out[3] = src.rgba[sp + 3];
    };
    const common::Affine2D t = common::Affine2D::trs({6.4, 3.7}, 0.37, {1.4, 1.1}) *
                               common::Affine2D{1.0, 0.2, 0.0, 0.0, 1.0, 0.0};
    const std::optional<common::Affine2D> inv = t.inverse();
    REQUIRE(inv.has_value());
    constexpr std::uint32_t kW = 40, kH = 40;
    for (ResampleFilter f : {ResampleFilter::Lanczos3, ResampleFilter::Mitchell}) {
        INFO("filter=" << render::resampleFilterName(f));
        common::ImageF dst(kW, kH);
        render::convolveInto(dst, kW, kH, *inv, f, fetch, src.width, src.height);
        for (std::uint32_t y = 0; y < kH; ++y) {
            for (std::uint32_t x = 0; x < kW; ++x) {
                const common::ColorF want = referenceTexel(src, *inv, f, x, y);
                const common::ColorF got = dst.at(x, y);
                CHECK(got.r == want.r);
                CHECK(got.g == want.g);
                CHECK(got.b == want.b);
                CHECK(got.a == want.a);
            }
        }
    }
}
