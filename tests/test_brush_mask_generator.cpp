#include <doctest/doctest.h>

#include "core/brush/mask_generator.hpp"

#include <cmath>
#include <string>
#include <vector>

using mosaic::core::brush::Curve;
using mosaic::core::brush::MaskFalloff;
using mosaic::core::brush::maskFalloffFromName;
using mosaic::core::brush::maskFalloffName;
using mosaic::core::brush::MaskGenerator;
using mosaic::core::brush::MaskGeneratorParams;
using mosaic::core::brush::MaskShape;
using mosaic::core::brush::maskShapeFromName;
using mosaic::core::brush::maskShapeName;

namespace {

[[nodiscard]] MaskGeneratorParams tip(MaskShape shape, MaskFalloff falloff, double diameter = 24.0) {
    MaskGeneratorParams p;
    p.shape = shape;
    p.falloff = falloff;
    p.diameter = diameter;
    return p;
}

// Every (shape, falloff) pair, so a property can be asserted across the whole family at once.
[[nodiscard]] std::vector<MaskGeneratorParams> allSix(double diameter = 24.0) {
    std::vector<MaskGeneratorParams> out;
    for (const MaskShape s : {MaskShape::Circle, MaskShape::Rect})
        for (const MaskFalloff f : {MaskFalloff::Default, MaskFalloff::Soft, MaskFalloff::Gauss})
            out.push_back(tip(s, f, diameter));
    return out;
}

[[nodiscard]] std::string describe(const MaskGeneratorParams& p) {
    return std::string(maskFalloffName(p.falloff)) + "/" + std::string(maskShapeName(p.shape));
}

} // namespace

TEST_CASE("brush mask generator: the six serialized names round-trip") {
    for (const MaskFalloff f : {MaskFalloff::Default, MaskFalloff::Soft, MaskFalloff::Gauss}) {
        REQUIRE(maskFalloffFromName(maskFalloffName(f)).has_value());
        CHECK(*maskFalloffFromName(maskFalloffName(f)) == f);
    }
    for (const MaskShape s : {MaskShape::Circle, MaskShape::Rect}) {
        REQUIRE(maskShapeFromName(maskShapeName(s)).has_value());
        CHECK(*maskShapeFromName(maskShapeName(s)) == s);
    }
    CHECK(maskFalloffName(MaskFalloff::Default) == "default");
    CHECK(maskShapeName(MaskShape::Rect) == "rect");
    CHECK_FALSE(maskFalloffFromName("Gauss").has_value()); // the wire is lowercase
    CHECK_FALSE(maskShapeFromName("rectangle").has_value());
    CHECK_FALSE(maskFalloffFromName("").has_value());
}

TEST_CASE("brush mask generator: all six are opaque at the centre and empty far outside") {
    for (const MaskGeneratorParams& p : allSix()) {
        CAPTURE(describe(p));
        const MaskGenerator g(p);
        CHECK_FALSE(g.isEmpty());
        CHECK(g.coverageAt(0.0, 0.0) == doctest::Approx(1.0).epsilon(0.01));
        CHECK(g.coverageAt(1000.0, 0.0) == doctest::Approx(0.0));
        CHECK(g.coverageAt(0.0, -1000.0) == doctest::Approx(0.0));
        CHECK(g.coverageAt(12.01, 12.01) == doctest::Approx(0.0)); // outside a 24 px tip's corner
    }
}

TEST_CASE("brush mask generator: coverage never escapes [0,1], for any input at all") {
    // Coverage feeds an accumulator directly. A value outside [0,1] -- or a NaN from a degenerate
    // parameter set a hostile preset can ask for -- corrupts the dab rather than erroring.
    std::vector<MaskGeneratorParams> hostile;
    for (MaskGeneratorParams p : allSix()) {
        hostile.push_back(p);
        p.hFade = 0.0;
        p.vFade = 0.0;
        hostile.push_back(p);
        p.hFade = 1.0;
        p.vFade = 1.0;
        p.softness = 0.0;
        hostile.push_back(p);
        p.ratio = 0.001;
        p.spikes = 9;
        p.antialiasEdges = false;
        hostile.push_back(p);
        p.diameter = 0.5;
        hostile.push_back(p);
    }

    for (const MaskGeneratorParams& p : hostile) {
        CAPTURE(describe(p));
        CAPTURE(p.diameter);
        CAPTURE(p.ratio);
        CAPTURE(p.hFade);
        CAPTURE(p.softness);
        const MaskGenerator g(p);
        for (int iy = -40; iy <= 40; ++iy) {
            for (int ix = -40; ix <= 40; ++ix) {
                const double c = g.coverageAt(ix * 0.7, iy * 0.7);
                REQUIRE(std::isfinite(c));
                REQUIRE(c >= 0.0);
                REQUIRE(c <= 1.0);
            }
        }
        // Non-finite offsets come from a NaN dab centre upstream; they must not propagate.
        CHECK(g.coverageAt(std::nan(""), 0.0) == doctest::Approx(0.0));
        CHECK(g.coverageAt(0.0, std::numeric_limits<double>::infinity()) == doctest::Approx(0.0));
    }
}

TEST_CASE("brush mask generator: a zero diameter or ratio is empty, not a division by zero") {
    for (MaskGeneratorParams p : allSix()) {
        CAPTURE(describe(p));

        MaskGeneratorParams noDiameter = p;
        noDiameter.diameter = 0.0;
        const MaskGenerator a(noDiameter);
        CHECK(a.isEmpty());
        CHECK(a.coverageAt(0.0, 0.0) == doctest::Approx(0.0));

        MaskGeneratorParams noRatio = p;
        noRatio.ratio = 0.0;
        const MaskGenerator b(noRatio);
        CHECK(b.isEmpty());
        CHECK(b.coverageAt(0.0, 0.0) == doctest::Approx(0.0));

        // Negative geometry is clamped rather than reflected.
        MaskGeneratorParams negative = p;
        negative.diameter = -10.0;
        CHECK(MaskGenerator(negative).isEmpty());
    }
}

TEST_CASE("brush mask generator: coverage falls monotonically from the centre") {
    // The one property every falloff shares, and the one a stamping loop depends on. Walked along
    // the x axis, where the rect and the circle agree about where the tip ends.
    for (const MaskGeneratorParams& p : allSix(40.0)) {
        CAPTURE(describe(p));
        const MaskGenerator g(p);
        double prev = 1.01;
        for (int i = 0; i <= 100; ++i) {
            const double x = i * 0.2; // out to 20 px = the rim
            const double c = g.coverageAt(x, 0.0);
            CAPTURE(x);
            CHECK(c <= prev + 1e-9);
            prev = c;
        }
        CHECK(prev == doctest::Approx(0.0).epsilon(0.02)); // reaches transparent at the rim
    }
}

TEST_CASE("brush mask generator: a circle is radially symmetric, a rect is not") {
    const MaskGenerator circle(tip(MaskShape::Circle, MaskFalloff::Default, 40.0));
    // Along the diagonal at radius 15 vs along the axis at radius 15: a disc treats them alike.
    const double axis = circle.coverageAt(15.0, 0.0);
    const double diag = circle.coverageAt(15.0 / std::sqrt(2.0), 15.0 / std::sqrt(2.0));
    CHECK(axis == doctest::Approx(diag).epsilon(0.02));
    // The corner of the bounding box is outside the disc entirely.
    CHECK(circle.coverageAt(19.0, 19.0) == doctest::Approx(0.0));

    const MaskGenerator rect(tip(MaskShape::Rect, MaskFalloff::Default, 40.0));
    // ...but inside the square, where the rect still paints.
    CHECK(rect.coverageAt(15.0, 15.0) > 0.0);
}

TEST_CASE("brush mask generator: every falloff is symmetric in both axes") {
    for (const MaskGeneratorParams& p : allSix(30.0)) {
        CAPTURE(describe(p));
        const MaskGenerator g(p);
        for (const double x : {0.0, 3.3, 9.9, 14.0}) {
            for (const double y : {0.0, 2.2, 7.7, 13.0}) {
                CAPTURE(x);
                CAPTURE(y);
                const double q = g.coverageAt(x, y);
                CHECK(g.coverageAt(-x, y) == doctest::Approx(q));
                CHECK(g.coverageAt(x, -y) == doctest::Approx(q));
                CHECK(g.coverageAt(-x, -y) == doctest::Approx(q));
            }
        }
    }
}

TEST_CASE("brush mask generator: ratio squashes the tip along y only") {
    MaskGeneratorParams p = tip(MaskShape::Circle, MaskFalloff::Default, 40.0);
    p.ratio = 0.5;
    const MaskGenerator g(p);
    CHECK(g.width() == doctest::Approx(40.0));
    CHECK(g.height() == doctest::Approx(20.0));

    // The tip reaches 20 px along x but only 10 px along y.
    CHECK(g.coverageAt(19.0, 0.0) >= 0.0);
    CHECK(g.coverageAt(21.0, 0.0) == doctest::Approx(0.0));
    CHECK(g.coverageAt(0.0, 11.0) == doctest::Approx(0.0));
}

TEST_CASE("brush mask generator: hFade is HARDNESS, not softness") {
    // The attribute reads like "how much fade", but a larger value means a LARGER solid core: the
    // fade coefficient is 2/(fade * width), so fade = 1 puts the shoulder at the rim (a hard tip)
    // and small fade values pull it inward (a soft one). Getting this backwards inverts every
    // procedural tip in the default set.
    const auto coreRadius = [](double fade) {
        MaskGeneratorParams p = tip(MaskShape::Circle, MaskFalloff::Default, 40.0);
        p.hFade = fade;
        p.vFade = fade;
        p.antialiasEdges = false;
        const MaskGenerator g(p);
        double r = 0.0;
        for (int i = 0; i <= 200; ++i) {
            const double x = i * 0.1;
            if (g.coverageAt(x, 0.0) > 0.999)
                r = x;
        }
        return r;
    };

    const double hard = coreRadius(1.0);
    const double medium = coreRadius(0.5);
    const double soft = coreRadius(0.1);
    CHECK(hard > medium);
    CHECK(medium > soft);
    CHECK(hard == doctest::Approx(20.0).epsilon(0.02)); // solid all the way to the rim
    CHECK(medium == doctest::Approx(10.0).epsilon(0.05));
}

TEST_CASE("brush mask generator: antialiasing adds a rim to an otherwise hard tip") {
    MaskGeneratorParams hard = tip(MaskShape::Circle, MaskFalloff::Default, 40.0);
    hard.hFade = 1.0;
    hard.vFade = 1.0;

    MaskGeneratorParams aliased = hard;
    aliased.antialiasEdges = false;

    const MaskGenerator withRim(hard);
    const MaskGenerator withoutRim(aliased);

    // Without antialiasing the tip is a disc: solid right up to the rim, then nothing.
    CHECK(withoutRim.coverageAt(19.5, 0.0) == doctest::Approx(1.0));
    CHECK(withoutRim.coverageAt(20.5, 0.0) == doctest::Approx(0.0));

    // With it, the last pixel is a ramp -- partial, monotone, and gone by the rim.
    const double inner = withRim.coverageAt(19.0, 0.0);
    const double outer = withRim.coverageAt(19.9, 0.0);
    CHECK(inner < 1.0);
    CHECK(inner > 0.0);
    CHECK(outer < inner);
    CHECK(withRim.coverageAt(10.0, 0.0) == doctest::Approx(1.0)); // the core is untouched
}

TEST_CASE("brush mask generator: the soft falloff's curve IS the profile, and it descends") {
    MaskGeneratorParams p = tip(MaskShape::Circle, MaskFalloff::Soft, 40.0);
    p.antialiasEdges = false;

    // The default is a descending ramp, not the identity: the curve maps squared radius directly to
    // coverage, so an identity curve would build an inside-out tip. Every shipped softness_curve
    // descends, and the header's default matches them.
    const MaskGenerator linear(p);
    CHECK(linear.coverageAt(0.0, 0.0) == doctest::Approx(1.0));
    // Halfway in SQUARED radius, not in radius: r = sqrt(0.5) * 20.
    CHECK(linear.coverageAt(20.0 / std::sqrt(2.0), 0.0) == doctest::Approx(0.5).epsilon(0.02));
    CHECK(linear.coverageAt(20.0, 0.0) == doctest::Approx(0.0).epsilon(0.02));

    // Verbatim from a shipped `soft` tip: a wide flat core, then a fast fall. `Curve::eval` holds
    // the domain flat below the first knot, which is what makes the core solid at all.
    p.softnessCurve = Curve::fromString("0.742972,1;1,0;");
    const MaskGenerator plateau(p);
    CHECK(plateau.coverageAt(0.0, 0.0) == doctest::Approx(1.0));
    CHECK(plateau.coverageAt(0.8 * 20.0, 0.0) == doctest::Approx(1.0)); // r^2 = 0.64 < 0.743
    CHECK(plateau.coverageAt(20.0, 0.0) == doctest::Approx(0.0).epsilon(0.02));
    CHECK(plateau.coverageAt(0.95 * 20.0, 0.0) < 0.9); // past the knot, falling

    // Also verbatim: the most common shipped curve. Still opaque at the centre, clear at the rim.
    p.softnessCurve = Curve::fromString("0,0.39911;0.429719,0.118523;1,0;");
    const MaskGenerator shipped(p);
    CHECK(shipped.coverageAt(0.0, 0.0) == doctest::Approx(0.39911).epsilon(0.01)); // a translucent tip
    CHECK(shipped.coverageAt(20.0, 0.0) == doctest::Approx(0.0).epsilon(0.02));
}

TEST_CASE("brush mask generator: softness reshapes default and soft, and is inert on gauss") {
    // Faithful, and a real fidelity trap: the reference's gauss generators never override the
    // softness hook, so a preset that pairs an enabled Softness option with a gauss mask is
    // silently static. An importer has to report that rather than pretend it works.
    const auto profileAt = [](MaskFalloff f, double softness) {
        MaskGeneratorParams p = tip(MaskShape::Circle, f, 40.0);
        p.hFade = 0.5;
        p.vFade = 0.5;
        p.antialiasEdges = false;
        p.softness = softness;
        return MaskGenerator(p).coverageAt(12.0, 0.0);
    };

    CHECK(profileAt(MaskFalloff::Default, 1.0) != doctest::Approx(profileAt(MaskFalloff::Default, 0.3)));
    CHECK(profileAt(MaskFalloff::Soft, 1.0) != doctest::Approx(profileAt(MaskFalloff::Soft, 0.3)));
    CHECK(profileAt(MaskFalloff::Gauss, 1.0) == doctest::Approx(profileAt(MaskFalloff::Gauss, 0.3)));

    // Lower softness softens: the same radius is less covered.
    CHECK(profileAt(MaskFalloff::Default, 0.3) < profileAt(MaskFalloff::Default, 1.0));
}

TEST_CASE("brush mask generator: spikes fold the tip into a rotationally repeated wedge") {
    // 4 of the shipped presets use spikes = 9; all four are circles.
    MaskGeneratorParams p = tip(MaskShape::Circle, MaskFalloff::Default, 60.0);
    p.spikes = 9;
    p.ratio = 0.35; // an elongated tip, so the fold is visible as a star rather than a disc
    const MaskGenerator star(p);

    // The mask now repeats every 2*pi/9 radians about the origin.
    constexpr double kPi = 3.14159265358979323846;
    const double step = 2.0 * kPi / 9.0;
    for (const double r : {5.0, 12.0, 22.0}) {
        for (const double a : {0.1, 0.9, 2.0, 3.5}) {
            CAPTURE(r);
            CAPTURE(a);
            const double c0 = star.coverageAt(r * std::cos(a), r * std::sin(a));
            const double c1 = star.coverageAt(r * std::cos(a + step), r * std::sin(a + step));
            CHECK(c0 == doctest::Approx(c1).epsilon(0.02));
        }
    }

    // spikes = 2 is an ordinary tip: the fold is a no-op, and an out-of-range count is clamped
    // rather than looping forever.
    MaskGeneratorParams plain = p;
    plain.spikes = 2;
    MaskGeneratorParams absurd = p;
    absurd.spikes = 1'000'000;
    CHECK(MaskGenerator(absurd).params().spikes == 64);
    MaskGeneratorParams negative = p;
    negative.spikes = -5;
    CHECK(MaskGenerator(negative).params().spikes == 2);
    CHECK(MaskGenerator(negative).coverageAt(3.0, 1.0) ==
          doctest::Approx(MaskGenerator(plain).coverageAt(3.0, 1.0)));
}

TEST_CASE("brush mask generator: coverageAt is pure") {
    // The GLSL parity lane depends on this: same coefficients, same (x, y), same answer, no matter
    // how many times or in what order it is asked (docs/brushes.md §6.3).
    for (const MaskGeneratorParams& p : allSix(17.0)) {
        CAPTURE(describe(p));
        const MaskGenerator g(p);
        std::vector<double> first;
        for (int i = -20; i <= 20; ++i)
            first.push_back(g.coverageAt(i * 0.55, i * -0.31));
        for (int pass = 0; pass < 3; ++pass) {
            std::size_t k = 0;
            for (int i = -20; i <= 20; ++i)
                CHECK(g.coverageAt(i * 0.55, i * -0.31) == first[k++]); // bit-exact, not Approx
        }
    }
}

TEST_CASE("brush mask generator: hFade reads as HARDNESS on the gauss falloff too") {
    // gauss derives its Gaussian width from `1 - (hFade + vFade)/2`, so the same attribute points the
    // same way on both falloffs even though the formulas share nothing: hFade -> 1 is hard.
    const auto coverageAt8 = [](double fade) {
        MaskGeneratorParams p = tip(MaskShape::Circle, MaskFalloff::Gauss, 20.0);
        p.hFade = fade;
        p.vFade = fade;
        return MaskGenerator(p).coverageAt(8.0, 0.0); // 80% of the way to the rim
    };
    CHECK(coverageAt8(0.04) < 0.1); // a tiny fade is a very SOFT tip
    CHECK(coverageAt8(0.6) > coverageAt8(0.3));
    CHECK(coverageAt8(0.9) > 0.9); // a large fade is nearly solid out to the rim
}

TEST_CASE("brush mask generator: a shipped spiked gauss tip") {
    // Verbatim from d)_Ink-1_Precision.kpp: gauss/circle, spikes 9, hfade = vfade = 0.04.
    MaskGeneratorParams p;
    p.shape = MaskShape::Circle;
    p.falloff = MaskFalloff::Gauss;
    p.diameter = 20.0;
    p.ratio = 1.0;
    p.hFade = 0.04;
    p.vFade = 0.04;
    p.spikes = 9;
    p.antialiasEdges = true;

    const MaskGenerator g(p);
    CHECK(g.coverageAt(0.0, 0.0) == doctest::Approx(1.0).epsilon(0.02));
    CHECK(g.coverageAt(10.5, 0.0) == doctest::Approx(0.0));

    // A fade of 0.04 makes an extremely soft tip -- barely 5% coverage at 80% of the radius. The
    // preset is named "Precision" because it is small and spiked, not because it is crisp.
    CHECK(g.coverageAt(8.0, 0.0) < 0.1);
    CHECK(g.coverageAt(2.0, 0.0) > 0.5);

    double prev = 1.01;
    for (int i = 0; i <= 100; ++i) {
        const double c = g.coverageAt(i * 0.1, 0.0);
        CHECK(c <= prev + 1e-9);
        prev = c;
    }
}
