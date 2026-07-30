#include "core/selection.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>

// The Select-menu morphology ops (S18, docs/research-select-brush.md §4): Grow / Shrink via a signed
// EDT offset, Feather via a separable Gaussian, Smooth via blur + threshold + re-AA. All four must
// preserve the mask's AA-coverage semantics and collapse an empty input to "no selection".
namespace {

using mosaic::core::Selection;

// A hard-edged square of side `s` at (x0,y0) inside a docW x docH canvas.
Selection square(std::uint32_t docW, std::uint32_t docH, int x0, int y0, int s) {
    return Selection::rectangle(docW, docH,
                                {static_cast<double>(x0), static_cast<double>(y0),
                                 static_cast<double>(s), static_cast<double>(s)});
}

// Count pixels at full coverage (255) and pixels with a strictly-fractional value.
struct Census {
    int solid = 0;
    int fractional = 0;
    int any = 0;
};
Census census(const Selection& s) {
    Census c;
    for (std::uint8_t v : s.data()) {
        if (v == 255)
            ++c.solid;
        if (v > 0)
            ++c.any;
        if (v > 0 && v < 255)
            ++c.fractional;
    }
    return c;
}

} // namespace

TEST_CASE("morphology: empty input returns empty for every op") {
    const Selection none;
    CHECK(none.grown(5).isEmpty());
    CHECK(none.shrunk(5).isEmpty());
    CHECK(none.feathered(3.0).isEmpty());
    CHECK(none.smoothed(3.0).isEmpty());
}

TEST_CASE("morphology: a zero (non-null) radius / distance is the identity") {
    const Selection sq = square(40, 40, 12, 12, 16);
    CHECK(sq.grown(0) == sq);
    CHECK(sq.shrunk(0) == sq);
    CHECK(sq.feathered(0.0) == sq);
    CHECK(sq.smoothed(0.0) == sq);
}

TEST_CASE("Grow expands the selection outward by ~N px and keeps a clean AA edge") {
    const Selection sq = square(60, 60, 20, 20, 20); // [20,40) x [20,40)
    const Selection g = sq.grown(4);
    REQUIRE_FALSE(g.isEmpty());

    // The bounds expand by ~N on every side (SDF rounds corners, so allow the diagonal a pixel).
    const auto gb = g.bounds();
    REQUIRE(gb.has_value());
    CHECK(gb->x <= 16.5);            // left edge moved out from 20
    CHECK(gb->right() >= 43.5);      // right edge moved out from 40
    CHECK(gb->y <= 16.5);
    CHECK(gb->bottom() >= 43.5);

    // The interior stays fully selected and the edge carries fractional coverage (AA preserved).
    CHECK(g.at(30, 30) == 255);
    CHECK(census(g).fractional > 0);
    // A point 4 px outside the original right edge is now inside.
    CHECK(g.at(42, 30) >= 128);
}

TEST_CASE("Shrink contracts the selection inward and Shrink = Grow with a negated offset") {
    const Selection sq = square(60, 60, 20, 20, 20);
    const Selection s = sq.shrunk(4);
    REQUIRE_FALSE(s.isEmpty());

    const auto sb = s.bounds();
    REQUIRE(sb.has_value());
    CHECK(sb->x >= 22.5);       // left edge pulled in from 20
    CHECK(sb->right() <= 37.5); // right edge pulled in from 40
    CHECK(s.at(30, 30) == 255); // centre still solid
    CHECK(s.at(21, 21) == 0);   // a former-edge corner is now outside
}

TEST_CASE("morphology preserves AA on a curved edge (Grow/Shrink/Feather/Smooth)") {
    // An axis-aligned rectangle has no AA to preserve (its edge distances are integers, so the ramp
    // lands on binary); an ELLIPSE has a genuinely fractional edge, and every op must keep one.
    const Selection e = Selection::ellipse(80, 80, {16.0, 16.0, 48.0, 48.0});
    REQUIRE_FALSE(e.isEmpty());
    CHECK(census(e).fractional > 0); // the input is anti-aliased to begin with

    CHECK(census(e.grown(4)).fractional > 0);
    CHECK(census(e.shrunk(4)).fractional > 0);
    CHECK(census(e.feathered(2.0)).fractional > 0);
    CHECK(census(e.smoothed(2.0)).fractional > 0);

    // Grow really does enlarge and Shrink really does contract the disc.
    const auto eb = e.bounds();
    const auto gb = e.grown(4).bounds();
    const auto shb = e.shrunk(4).bounds();
    REQUIRE(eb.has_value());
    REQUIRE(gb.has_value());
    REQUIRE(shb.has_value());
    CHECK(gb->w > eb->w);
    CHECK(shb->w < eb->w);
}

TEST_CASE("Grow N then Shrink N recovers the original within AA tolerance") {
    const Selection sq = square(80, 80, 24, 24, 32);
    const Selection round = sq.grown(6).shrunk(6);
    REQUIRE_FALSE(round.isEmpty());

    // Deep interior and far exterior must be exactly restored; only the AA edge (and rounded
    // corners) may differ, so compare on the >=128 membership over the non-boundary pixels.
    int mismatches = 0;
    for (std::uint32_t y = 0; y < 80; ++y)
        for (std::uint32_t x = 0; x < 80; ++x) {
            const bool a = sq.at(x, y) >= 128;
            const bool b = round.at(x, y) >= 128;
            if (a != b)
                ++mismatches;
        }
    // A handful of corner pixels may round differently; the body of a 32x32 square is recovered.
    CHECK(mismatches <= 24);
    CHECK(round.at(40, 40) == 255);
    CHECK(round.at(2, 2) == 0);
}

TEST_CASE("Feather blurs the coverage into a fractional halo, preserving AA") {
    const Selection sq = square(60, 60, 20, 20, 20);
    const Census before = census(sq);
    const Selection f = sq.feathered(3.0);
    REQUIRE_FALSE(f.isEmpty());

    // The blur softens the hard edge: many more fractional pixels than the crisp square had, and
    // the coverage spreads beyond the original footprint (the halo).
    const Census after = census(f);
    CHECK(after.fractional > before.fractional);
    CHECK(after.any > before.any);          // coverage bled outward
    CHECK(after.solid < before.solid);      // the solid core shrank as the edge softened
    CHECK(f.at(30, 30) == 255);             // deep interior still solid
    CHECK(f.at(19, 30) > 0);                // one px outside the old edge now has coverage
    CHECK(f.at(19, 30) < 255);
}

TEST_CASE("Smooth removes speckle smaller than the radius and re-AAs the edge") {
    // A solid block plus a lone 1-px speck far from it: Smooth should wash the speck out and keep
    // the block (rounding its corners), leaving a clean fractional edge.
    Selection s(60, 60);
    for (std::uint32_t y = 20; y < 40; ++y)
        for (std::uint32_t x = 20; x < 40; ++x)
            s.data()[static_cast<std::size_t>(y) * 60 + x] = 255;
    s.data()[static_cast<std::size_t>(5) * 60 + 5] = 255; // an isolated speck

    CHECK(s.at(5, 5) == 255);
    const Selection sm = s.smoothed(3.0);
    REQUIRE_FALSE(sm.isEmpty());
    CHECK(sm.at(5, 5) == 0);     // the speck is gone
    CHECK(sm.at(30, 30) == 255); // the block survives
}

TEST_CASE("morphology: a large-radius feather/smooth terminates promptly (no O(n*radius) blowup)") {
    // Regression for the "Modify Selection amount slider hangs the program" report: the amount slider
    // ranges to 1000 px, and the exact truncated-kernel Gaussian was O(w*h*radius) -- on the order of
    // 1e10 multiply-adds on a selection this size at radius 1000, i.e. minutes of frozen UI. The
    // large-radius path is now a three-pass box approximation, O(w*h) regardless of radius. A generous
    // wall-clock bound catches a regression to the quadratic path (tens of seconds to minutes) without
    // flaking on the linear one (a few ms even on a slow debug/sanitizer build).
    const Selection s = square(1200, 1000, 400, 300, 400); // ~1.2 M-pixel plane, one solid block
    REQUIRE_FALSE(s.isEmpty());

    const auto timed = [](auto&& fn) {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };

    Selection f, sm;
    const double featherSec = timed([&] { f = s.feathered(1000.0); });
    const double smoothSec = timed([&] { sm = s.smoothed(1000.0); });
    CHECK(featherSec < 5.0); // was tens of seconds+ under the exact O(n*radius) kernel
    CHECK(smoothSec < 5.0);
    CHECK_FALSE(f.isEmpty());   // a wide feather still leaves coverage behind
    CHECK(f.at(600, 500) > 0);  // ... including the block's deep interior

    // Grow/Shrink at the slider's extreme are EDT-based (already linear): confirm they return promptly
    // and don't wedge either.
    Selection g, sh;
    const double growSec = timed([&] { g = s.grown(800); });
    const double shrinkSec = timed([&] { sh = s.shrunk(800); });
    CHECK(growSec < 5.0);
    CHECK(shrinkSec < 5.0);
    CHECK_FALSE(g.isEmpty()); // grew outward
}

TEST_CASE("Feather with a large radius still blurs as a low-pass (box approximation path)") {
    // Above the exact-kernel cutoff the blur switches to the box approximation; it must still behave
    // like a Gaussian low-pass: a hard block gains a fractional halo bleeding outside its footprint,
    // and coverage falls off monotonically from the centre outward.
    const Selection sq = square(400, 400, 150, 150, 100); // [150,250)^2, hard edges (fractional == 0)
    const Census before = census(sq);
    const Selection f = sq.feathered(40.0); // well past kExactGaussianSigmaMax -> the box path runs
    REQUIRE_FALSE(f.isEmpty());
    const Census after = census(f);
    CHECK(after.fractional > before.fractional); // a soft, anti-aliased edge appeared
    CHECK(after.any > before.any);               // coverage bled outward past the block
    CHECK(after.solid < before.solid);           // the solid core softened under the wide blur
    CHECK(f.at(200, 200) > f.at(110, 110));      // centre stays more covered than 40 px outside it
}
