#include "core/brush/mask_stroke.hpp"
#include "core/selection.hpp"

#include <doctest/doctest.h>

#include <cmath>

// The coverage-only select-brush stroke (core::brush::MaskStroke, S18): the soft-round-tip spacing
// walk deposits coverage into a bounded buffer with no colour composite, and the buffer *is* the
// selection contribution. Pure math -- the FLTK event plumbing in VulkanCanvas is the --gui-frames
// run's job.
namespace {

using mosaic::common::Vec2;
using mosaic::core::Selection;
using mosaic::core::brush::MaskStroke;
using mosaic::core::brush::MaskStrokeParams;

// Read the raw (built-toward-1) coverage at a document pixel from the bounded buffer.
float covAt(const MaskStroke& s, int x, int y) {
    const int lx = x - s.coverageOriginX();
    const int ly = y - s.coverageOriginY();
    if (lx < 0 || ly < 0 || lx >= static_cast<int>(s.coverageWidth()) ||
        ly >= static_cast<int>(s.coverageHeight()))
        return 0.0f;
    return s.coverage()[static_cast<std::size_t>(ly) * s.coverageWidth() + lx];
}

} // namespace

TEST_CASE("MaskStroke: a single dab builds full coverage at its centre and an AA edge") {
    MaskStrokeParams p;
    p.diameter = 20.0;
    p.hardness = 0.9;
    p.flow = 1.0;
    p.opacity = 1.0;
    MaskStroke s;
    s.begin(80, 80, p, {40.0, 40.0});
    s.end();

    CHECK(covAt(s, 40, 40) == doctest::Approx(1.0)); // solid core under the tip
    CHECK(covAt(s, 60, 40) == doctest::Approx(0.0)); // well outside the 10 px radius
    // The rim carries fractional coverage (dabCoverage's guaranteed AA shoulder).
    bool sawFraction = false;
    for (int x = 44; x <= 52; ++x) {
        const float c = covAt(s, x, 40);
        if (c > 0.0f && c < 1.0f)
            sawFraction = true;
    }
    CHECK(sawFraction);

    // toSelection at full flow/opacity lands the core solid, and empties outside.
    const Selection sel = s.toSelection();
    REQUIRE_FALSE(sel.isEmpty());
    CHECK(sel.at(40, 40) == 255);
    CHECK(sel.at(60, 40) == 0);
}

TEST_CASE("MaskStroke: flow scales a single dab's deposit; overlapping passes build toward 1") {
    MaskStrokeParams p;
    p.diameter = 20.0;
    p.hardness = 1.0; // hard core so the centre is solid coverage before flow
    p.flow = 0.5;
    MaskStroke half;
    half.begin(80, 80, p, {40.0, 40.0});
    half.end();
    CHECK(covAt(half, 40, 40) == doctest::Approx(0.5)); // one dab at flow 0.5 deposits 0.5

    // Scrubbing overlapped passes at the same flow builds "over" the previous coverage, so the band
    // climbs above a single dab's 0.5 (toward 1) -- the flow-vs-opacity build-up within one stroke.
    MaskStroke many;
    many.begin(80, 80, p, {40.0, 40.0});
    for (int i = 0; i < 6; ++i) {
        many.extendTo({20.0, 40.0});
        many.extendTo({60.0, 40.0});
    }
    many.end();
    CHECK(covAt(many, 40, 40) > 0.5);
}

TEST_CASE("MaskStroke: the opacity cap bounds the contribution however the stroke crosses itself") {
    MaskStrokeParams p;
    p.diameter = 24.0;
    p.hardness = 1.0;
    p.flow = 1.0;
    p.opacity = 0.5; // the whole stroke tops out here
    MaskStroke s;
    s.begin(80, 80, p, {40.0, 40.0});
    // Scrub back and forth over the same band many times: raw coverage saturates toward 1...
    for (int i = 0; i < 12; ++i) {
        s.extendTo({20.0, 40.0});
        s.extendTo({60.0, 40.0});
    }
    s.end();
    CHECK(covAt(s, 40, 40) == doctest::Approx(1.0).epsilon(0.02)); // raw coverage is (near) full

    // ...but the SELECTION contribution never exceeds opacity (0.5 -> byte ~128).
    const Selection sel = s.toSelection();
    REQUIRE_FALSE(sel.isEmpty());
    CHECK(sel.at(40, 40) == 128);
}

TEST_CASE("MaskStroke: a harder tip fills a broader solid core than a soft one") {
    const auto coreCoverage = [](double hardness, int x) {
        MaskStrokeParams p;
        p.diameter = 40.0; // 20 px radius
        p.hardness = hardness;
        p.flow = 1.0;
        MaskStroke s;
        s.begin(80, 80, p, {40.0, 40.0});
        s.end();
        return covAt(s, x, 40);
    };
    // 8 px from the centre (within the 20 px radius): the hard tip is still solid, the soft cone has
    // already fallen well below full.
    CHECK(coreCoverage(1.0, 48) > coreCoverage(0.0, 48));
    CHECK(coreCoverage(1.0, 48) == doctest::Approx(1.0));
    CHECK(coreCoverage(0.0, 48) < 1.0);
}

TEST_CASE("MaskStroke: the dab pattern is independent of how finely the pointer is sampled") {
    MaskStrokeParams p;
    p.diameter = 24.0;
    p.hardness = 0.8;
    p.flow = 1.0;

    // Coarse: one big segment. Fine: the same path in many small steps.
    MaskStroke coarse;
    coarse.begin(120, 60, p, {10.0, 30.0});
    coarse.extendTo({100.0, 30.0});
    coarse.end();

    MaskStroke fine;
    fine.begin(120, 60, p, {10.0, 30.0});
    for (int x = 13; x <= 100; x += 3)
        fine.extendTo({static_cast<double>(x), 30.0});
    fine.extendTo({100.0, 30.0});
    fine.end();

    // The 8-bit contributions are identical: the spacing walk carries its remainder across calls.
    CHECK(coarse.toSelection() == fine.toSelection());
}

TEST_CASE("MaskStroke: a stroke that deposits nothing yields an empty (no-selection) result") {
    MaskStrokeParams p;
    p.opacity = 0.0; // caps everything to zero
    MaskStroke s;
    s.begin(40, 40, p, {20.0, 20.0});
    s.end();
    CHECK(s.toSelection().isEmpty());

    // A fresh, never-begun stroke also has no contribution.
    MaskStroke idle;
    CHECK(idle.toSelection().isEmpty());
}
