#include <doctest/doctest.h>

#include "core/brush/dab_mask.hpp"

#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

using mosaic::core::brush::DabExtent;
using mosaic::core::brush::dabExtent;
using mosaic::core::brush::DabMask;
using mosaic::core::brush::DabPlacement;
using mosaic::core::brush::DabShape;
using mosaic::core::brush::kMaxDabExtent;
using mosaic::core::brush::MaskFalloff;
using mosaic::core::brush::MaskGenerator;
using mosaic::core::brush::MaskGeneratorParams;
using mosaic::core::brush::MaskShape;
using mosaic::core::brush::placeDab;
using mosaic::core::brush::renderDabMask;
using mosaic::core::brush::shapeOf;

namespace {

constexpr double kPi = 3.14159265358979323846;

[[nodiscard]] MaskGeneratorParams roundTip(double diameter = 24.0, double ratio = 1.0) {
    MaskGeneratorParams p;
    p.shape = MaskShape::Circle;
    p.falloff = MaskFalloff::Default;
    p.diameter = diameter;
    p.ratio = ratio;
    return p;
}

[[nodiscard]] double totalCoverage(const DabMask& m) {
    return std::accumulate(m.coverage.begin(), m.coverage.end(), 0.0);
}

// The coverage-weighted centre of mass, in mask pixel coords.
void centreOfMass(const DabMask& m, double& cx, double& cy) {
    double sum = 0.0;
    double sx = 0.0;
    double sy = 0.0;
    for (std::uint32_t y = 0; y < m.height; ++y)
        for (std::uint32_t x = 0; x < m.width; ++x) {
            const double w = m.at(x, y);
            sum += w;
            sx += w * (x + 0.5);
            sy += w * (y + 0.5);
        }
    cx = sum > 0.0 ? sx / sum : 0.0;
    cy = sum > 0.0 ? sy / sum : 0.0;
}

} // namespace

TEST_CASE("dabExtent is the bounding box of the rotated tip") {
    DabShape s{40.0, 10.0, 0.0, false, false};
    CHECK(dabExtent(s).width == doctest::Approx(40.0));
    CHECK(dabExtent(s).height == doctest::Approx(10.0));

    s.angleRad = kPi / 2.0; // a quarter turn swaps the axes
    CHECK(dabExtent(s).width == doctest::Approx(10.0));
    CHECK(dabExtent(s).height == doctest::Approx(40.0));

    s.angleRad = kPi / 4.0; // and at 45 degrees both are (w + h) / sqrt(2)
    const double diag = (40.0 + 10.0) / std::sqrt(2.0);
    CHECK(dabExtent(s).width == doctest::Approx(diag));
    CHECK(dabExtent(s).height == doctest::Approx(diag));

    // A half turn is the identity on the box, and the box is symmetric in the angle's sign.
    for (double a = -3.0; a < 3.0; a += 0.37) {
        DabShape p{31.0, 7.0, a, false, false};
        DabShape q{31.0, 7.0, -a, false, false};
        DabShape r{31.0, 7.0, a + kPi, false, false};
        CHECK(dabExtent(p).width == doctest::Approx(dabExtent(q).width));
        CHECK(dabExtent(p).width == doctest::Approx(dabExtent(r).width));
        CHECK(dabExtent(p).height == doctest::Approx(dabExtent(r).height));
    }
}

TEST_CASE("dabExtent refuses the degenerate and the hostile") {
    CHECK(dabExtent(DabShape{0.0, 10.0, 0.0, false, false}).empty());
    CHECK(dabExtent(DabShape{10.0, 0.0, 0.0, false, false}).empty());
    CHECK(dabExtent(DabShape{-5.0, 10.0, 0.0, false, false}).empty());
    CHECK(dabExtent(DabShape{std::nan(""), 10.0, 0.0, false, false}).empty());
    CHECK(dabExtent(DabShape{10.0, 10.0, std::nan(""), false, false}).empty());
    CHECK(dabExtent(DabShape{INFINITY, 10.0, 0.0, false, false}).empty());
    // Too large to allocate: refused, not clamped, so the caller sees "no dab" rather than a
    // silently truncated one.
    CHECK(dabExtent(DabShape{kMaxDabExtent + 1.0, 10.0, 0.0, false, false}).empty());
}

TEST_CASE("placeDab splits a centre into a corner and a quantized phase") {
    const DabShape s{24.0, 24.0, 0.0, false, false};

    SUBCASE("an integer centre with an even extent lands on the grid") {
        const DabPlacement p = placeDab(s, 100.0, 50.0, 4);
        CHECK(p.x == 88); // 100 - 12
        CHECK(p.y == 38);
        CHECK(p.subX == doctest::Approx(0.0));
        CHECK(p.subY == doctest::Approx(0.0));
        CHECK(p.width == 24);
        CHECK(p.height == 24);
    }

    SUBCASE("the phase snaps to the nearest bin") {
        CHECK(placeDab(s, 100.30, 50.0, 4).subX == doctest::Approx(0.25));
        CHECK(placeDab(s, 100.20, 50.0, 4).subX == doctest::Approx(0.25));
        CHECK(placeDab(s, 100.10, 50.0, 4).subX == doctest::Approx(0.00));
        CHECK(placeDab(s, 100.60, 50.0, 4).subX == doctest::Approx(0.50));
    }

    SUBCASE("a phase that rounds up to a whole pixel is carried into the corner") {
        // 0.9 -> bin 4 of 4, which is not a legal phase. It must become corner+1, phase 0 -- never a
        // phase of 1.0, which would make the mask a pixel wider than the render produces.
        const DabPlacement p = placeDab(s, 100.90, 50.0, 4);
        CHECK(p.x == 89);
        CHECK(p.subX == doctest::Approx(0.0));
        CHECK(p.width == 24);
    }

    SUBCASE("subPixelSteps = 1 disables sub-pixel placement") {
        for (double d = 0.0; d < 1.0; d += 0.1) {
            const DabPlacement p = placeDab(s, 100.0 + d, 50.0, 1);
            CHECK(p.subX == doctest::Approx(0.0));
        }
    }

    SUBCASE("a non-zero phase widens the mask by exactly one pixel") {
        CHECK(placeDab(s, 100.0, 50.0, 4).width == 24);
        CHECK(placeDab(s, 100.25, 50.0, 4).width == 25);
        CHECK(placeDab(s, 100.5, 50.0, 4).width == 25);
    }

    SUBCASE("a degenerate shape places nothing") {
        CHECK(placeDab(DabShape{0.0, 10.0, 0.0, false, false}, 5.0, 5.0, 4).empty());
        CHECK(placeDab(s, std::nan(""), 5.0, 4).empty());
    }
}

TEST_CASE("renderDabMask agrees with placeDab on the mask's dimensions") {
    // The blit reads `placement.width * placement.height` bytes out of the mask. If these two ever
    // disagree the result is a buffer overrun, so pin it across shapes, angles and phases.
    for (double dia : {3.0, 24.0, 61.5}) {
        for (double ratio : {1.0, 0.4}) {
            for (double angle : {0.0, 0.3, kPi / 2, 2.9, -1.1}) {
                for (int bin = 0; bin < 4; ++bin) {
                    const MaskGenerator gen{roundTip(dia, ratio)};
                    const double sub = bin / 4.0;
                    const DabShape s = shapeOf(gen, angle);
                    const DabPlacement p = placeDab(s, 10.0 + sub, 20.0 + sub, 4);
                    const DabMask m = renderDabMask(gen, angle, false, false, p.subX, p.subY);
                    CAPTURE(dia);
                    CAPTURE(ratio);
                    CAPTURE(angle);
                    CAPTURE(bin);
                    CHECK(m.width == p.width);
                    CHECK(m.height == p.height);
                    CHECK(m.coverage.size() == static_cast<std::size_t>(m.width) * m.height);
                }
            }
        }
    }
}

TEST_CASE("a round tip renders centred, symmetric and opaque in the middle") {
    const MaskGenerator gen{roundTip(24.0)};
    const DabMask m = renderDabMask(gen, 0.0, false, false, 0.0, 0.0);
    REQUIRE(m.width == 24);
    REQUIRE(m.height == 24);

    CHECK(m.at(12, 12) == 255);
    CHECK(m.at(0, 0) == 0); // the corners of the box are outside the disc
    CHECK(m.at(23, 23) == 0);

    // Symmetric under both reflections and under the diagonal.
    for (std::uint32_t y = 0; y < 24; ++y)
        for (std::uint32_t x = 0; x < 24; ++x) {
            CHECK(m.at(x, y) == m.at(23 - x, y));
            CHECK(m.at(x, y) == m.at(x, 23 - y));
            CHECK(m.at(x, y) == m.at(y, x));
        }

    double cx = 0.0;
    double cy = 0.0;
    centreOfMass(m, cx, cy);
    CHECK(cx == doctest::Approx(12.0).epsilon(0.001));
    CHECK(cy == doctest::Approx(12.0).epsilon(0.001));
}

TEST_CASE("the sub-pixel phase shifts the tip by exactly that phase") {
    const MaskGenerator gen{roundTip(24.0)};
    double cx0 = 0.0;
    double cy0 = 0.0;
    centreOfMass(renderDabMask(gen, 0.0, false, false, 0.0, 0.0), cx0, cy0);

    for (int bin = 1; bin < 4; ++bin) {
        const double sub = bin / 4.0;
        const DabMask m = renderDabMask(gen, 0.0, false, false, sub, 0.0);
        double cx = 0.0;
        double cy = 0.0;
        centreOfMass(m, cx, cy);
        CAPTURE(bin);
        CHECK(cx == doctest::Approx(cx0 + sub).epsilon(0.002));
        CHECK(cy == doctest::Approx(cy0).epsilon(0.002));
    }
}

TEST_CASE("a placed dab lands where it was asked to, to within half a phase bin") {
    // The whole point of the corner/phase split: blitting the mask at the integer corner must put the
    // tip's centre of mass back at the requested centre.
    const MaskGenerator gen{roundTip(24.0)};
    const DabShape s = shapeOf(gen, 0.0);
    for (double frac = 0.0; frac < 1.0; frac += 0.05) {
        const double centre = 100.0 + frac;
        const DabPlacement p = placeDab(s, centre, 60.0, 4);
        const DabMask m = renderDabMask(gen, 0.0, false, false, p.subX, p.subY);
        double cx = 0.0;
        double cy = 0.0;
        centreOfMass(m, cx, cy);
        CAPTURE(frac);
        // Half of a quarter-pixel bin, plus a hair for the mask's own 8-bit quantization. An
        // absolute bound, not doctest's relative `epsilon`, which at x = 100 would admit whole pixels.
        CHECK(std::abs((p.x + cx) - centre) <= 0.125 + 5e-3);
    }
}

TEST_CASE("placeDab survives a centre far outside any document") {
    // `floor(1e300)` does not fit in an integer, and converting it is undefined behaviour rather
    // than a large number. Such a dab is off-canvas whatever we do; it must not be UB on the way.
    const DabShape s{24.0, 24.0, 0.0, false, false};
    for (double c : {1e300, -1e300, 1e18, -1e18, 3e9}) {
        const DabPlacement p = placeDab(s, c, c, 4);
        CAPTURE(c);
        CHECK(p.subX >= 0.0);
        CHECK(p.subX < 1.0);
        CHECK(p.width == 24);
    }
}

TEST_CASE("rotating an elliptical tip by a quarter turn transposes its mask") {
    MaskGeneratorParams params = roundTip(24.0, 0.5); // 24 x 12
    const MaskGenerator gen{params};
    const DabMask flat = renderDabMask(gen, 0.0, false, false, 0.0, 0.0);
    const DabMask tall = renderDabMask(gen, kPi / 2.0, false, false, 0.0, 0.0);

    REQUIRE(flat.width == tall.height);
    REQUIRE(flat.height == tall.width);
    CHECK(totalCoverage(flat) == doctest::Approx(totalCoverage(tall)).epsilon(0.001));

    // `cos(pi/2)` is 6.1e-17 rather than 0, so the coordinate map is a hair off a true transpose and
    // a coverage sitting exactly on an 8-bit rounding boundary may land either side of it.
    for (std::uint32_t y = 0; y < flat.height; ++y)
        for (std::uint32_t x = 0; x < flat.width; ++x)
            CHECK(std::abs(flat.at(x, y) - tall.at(tall.width - 1 - y, x)) <= 1);
}

TEST_CASE("a near-axis angle does not grow the mask by a phantom pixel") {
    // Regression: `cos(pi/2) * 24 + sin(pi/2) * 12` is 12.0000000000000015, and ceil() turns that
    // into a thirteenth column. dabExtent snaps the float dust away first. Checked at all four
    // right angles and both diagonals, where the trig is at its least exact.
    for (int q = 0; q < 4; ++q) {
        const double angle = q * kPi / 2.0;
        const DabExtent e = dabExtent(DabShape{24.0, 12.0, angle, false, false});
        CAPTURE(q);
        const double along = (q % 2 == 0) ? 24.0 : 12.0;
        const double across = (q % 2 == 0) ? 12.0 : 24.0;
        CHECK(e.width == along); // exact equality: the snap must land on the integer
        CHECK(e.height == across);
        CHECK(placeDab(DabShape{24.0, 12.0, angle, false, false}, 50.0, 50.0, 4).width ==
              static_cast<std::uint32_t>(along));
    }
    // A genuinely fractional extent must survive untouched.
    const DabExtent frac = dabExtent(DabShape{24.3, 12.0, 0.0, false, false});
    CHECK(frac.width == doctest::Approx(24.3));
}

TEST_CASE("mirroring a symmetric procedural tip changes nothing") {
    // The six generators are symmetric about both axes at spikes = 2, so this is the mirror path's
    // correctness check: it must be an involution that does not shift or scale the dab.
    const MaskGenerator gen{roundTip(17.0, 0.6)};
    const DabMask plain = renderDabMask(gen, 0.7, false, false, 0.25, 0.0);
    const DabMask both = renderDabMask(gen, 0.7, true, true, 0.25, 0.0);
    REQUIRE(plain.width == both.width);
    REQUIRE(plain.height == both.height);
    // Mirroring in x and y together is a half-turn of the tip, which a two-fold symmetric tip is
    // invariant under -- but the dab is also rotated, so compare through the same half-turn.
    for (std::uint32_t y = 0; y < plain.height; ++y)
        for (std::uint32_t x = 0; x < plain.width; ++x)
            CHECK(plain.at(x, y) == both.at(x, y));
}

TEST_CASE("mirroring a spiked tip is a real reflection") {
    MaskGeneratorParams params = roundTip(32.0);
    params.spikes = 5; // no longer symmetric about the x axis
    const MaskGenerator gen{params};
    const DabMask plain = renderDabMask(gen, 0.0, false, false, 0.0, 0.0);
    const DabMask flipped = renderDabMask(gen, 0.0, true, false, 0.0, 0.0);

    REQUIRE(plain.width == flipped.width);
    REQUIRE(plain.height == flipped.height);
    CHECK(totalCoverage(plain) == doctest::Approx(totalCoverage(flipped)).epsilon(0.02));

    bool anyDifferent = false;
    for (std::uint32_t y = 0; y < plain.height && !anyDifferent; ++y)
        for (std::uint32_t x = 0; x < plain.width; ++x)
            if (plain.at(x, y) != flipped.at(x, y)) {
                anyDifferent = true;
                break;
            }
    CHECK(anyDifferent); // otherwise the mirror flag is being ignored

    // A horizontal mirror of the mask reproduces it exactly: the tip is centred, the extent is
    // symmetric, and the phase is zero.
    for (std::uint32_t y = 0; y < plain.height; ++y)
        for (std::uint32_t x = 0; x < plain.width; ++x)
            CHECK(plain.at(x, y) == flipped.at(plain.width - 1 - x, y));
}

TEST_CASE("an illegal sub-pixel phase cannot desynchronize a mask from its own dimensions") {
    // The blit trusts `mask.width * mask.height` bytes to exist. Nothing stops a caller -- or a dab
    // cache whose key quantizes the phase differently -- from passing a phase outside [0,1). Such a
    // phase is treated as zero, so the mask stays self-consistent instead of being sized for one
    // phase and drawn at another.
    const MaskGenerator gen{roundTip(24.0)};
    const DabMask zero = renderDabMask(gen, 0.0, false, false, 0.0, 0.0);
    constexpr double kInf = std::numeric_limits<double>::infinity();
    for (double bad : {1.0, 1.5, -0.25, std::nan(""), kInf, -kInf}) {
        const DabMask m = renderDabMask(gen, 0.0, false, false, bad, bad);
        CAPTURE(bad);
        CHECK(m.coverage.size() == static_cast<std::size_t>(m.width) * m.height);
        CHECK(m.width == zero.width);
        CHECK(m.height == zero.height);
        CHECK(m.coverage == zero.coverage);
    }
    // A legal phase is untouched.
    CHECK(renderDabMask(gen, 0.0, false, false, 0.75, 0.0).width == 25);
}

TEST_CASE("an empty generator renders an empty mask") {
    MaskGeneratorParams params = roundTip(24.0);
    params.ratio = 0.0;
    const MaskGenerator gen{params};
    REQUIRE(gen.isEmpty());
    const DabMask m = renderDabMask(gen, 0.0, false, false, 0.0, 0.0);
    CHECK(m.empty());
    CHECK(m.coverage.empty());
}

TEST_CASE("every rendered byte is a real coverage value") {
    // Nothing may produce a NaN-derived byte or read outside the buffer, for any angle or phase.
    for (double angle = -4.0; angle < 4.0; angle += 0.31) {
        for (int bin = 0; bin < 4; ++bin) {
            const MaskGenerator gen{roundTip(9.0, 0.31)};
            const DabMask m = renderDabMask(gen, angle, bin % 2 == 0, bin % 3 == 0, bin / 4.0, 0.5);
            CHECK(m.coverage.size() == static_cast<std::size_t>(m.width) * m.height);
            CHECK(totalCoverage(m) > 0.0); // a 9 x 2.8 tip still paints something
        }
    }
}
