#include "common/image.hpp"
#include "core/edge_grow.hpp"
#include "core/selection.hpp"

#include <doctest/doctest.h>

#include <cstdlib>

// The edge-aware select brush's grow engine (L1): the
// edge-weighted geodesic distance transform, its ramp AA, the edge-stop behaviour, and the "no
// coverage -> no selection" rule. Pure, GPU-free, synthetic images -- the same test posture
// test_magic_wand.cpp has. The stroke->commit shell is exercised in the tool tests; the *feel*
// (reach/edge-stop defaults) is owed a visual pass.
namespace {

using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::core::EdgeGrowParams;
using mosaic::core::edgeGrowSelection;
using mosaic::core::Selection;

// A w x h opaque image filled with `c`.
Image solid(std::uint32_t w, std::uint32_t h, Color8 c) {
    Image img(w, h);
    for (std::size_t i = 0; i < img.pixelCount(); ++i) {
        img.rgba[i * 4 + 0] = c.r;
        img.rgba[i * 4 + 1] = c.g;
        img.rgba[i * 4 + 2] = c.b;
        img.rgba[i * 4 + 3] = c.a;
    }
    return img;
}

void fillRect(Image& img, int x0, int y0, int w, int h, Color8 c) {
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 4;
            img.rgba[i + 0] = c.r;
            img.rgba[i + 1] = c.g;
            img.rgba[i + 2] = c.b;
            img.rgba[i + 3] = c.a;
        }
}

// A document-sized seed mask with a full-coverage rectangle (the stroke's core).
Selection seedRect(std::uint32_t docW, std::uint32_t docH, int x0, int y0, int w, int h) {
    Selection s(docW, docH);
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x)
            s.data()[static_cast<std::size_t>(y) * docW + x] = 255;
    return s;
}

std::size_t countInside(const Selection& s) { // the >=128 set the ants enclose
    std::size_t n = 0;
    for (std::uint8_t v : s.data())
        if (v >= 128)
            ++n;
    return n;
}

constexpr Color8 kGrey{128, 128, 128, 255};
constexpr Color8 kRed{255, 0, 0, 255};
constexpr Color8 kGreen{0, 255, 0, 255};

} // namespace

TEST_CASE("flat image: the grow is a plain distance disc of `reach` around the seed") {
    // No edges anywhere -> cost is exactly 1/px, so the geodesic field is the chamfer distance
    // and the >=128 set is {d <= reach}: a disc (chamfer octagon) around the seed pixel.
    const Image img = solid(64, 64, kGrey);
    const Selection seeds = seedRect(64, 64, 32, 32, 1, 1);

    EdgeGrowParams p;
    p.reach = 10.0;
    const Selection s = edgeGrowSelection(img, seeds, p);

    REQUIRE_FALSE(s.isEmpty());
    CHECK(s.at(32, 32) == 255);                    // the seed itself: solid
    CHECK(s.at(32 + 10, 32) >= 128);               // axis-aligned: exactly reach px away is on the ants line
    CHECK(s.at(32 + 12, 32) == 0);                 // solidly beyond reach (past the AA band)
    CHECK(s.at(32, 32 - 10) >= 128);               // symmetric in y
    CHECK(s.at(32 + 8, 32 + 8) == 0);              // diagonal: sqrt2-weighted, 8*sqrt2 > 10+band
    CHECK(s.at(32 + 7, 32 + 7) >= 128);            // 7*sqrt2 ~ 9.9 <= 10: inside
    // The disc never reaches the document edge (reach 10 from centre 32).
    CHECK(s.at(0, 0) == 0);
    CHECK(s.at(63, 63) == 0);
}

TEST_CASE("a strong colour edge stops the grow; edgeStop=0 ignores it") {
    // A green field with a red half starting at x=32; seed sits at x=24, 8 px left of the edge.
    Image img = solid(64, 64, kGreen);
    fillRect(img, 32, 0, 32, 64, kRed);
    const Selection seeds = seedRect(64, 64, 24, 32, 1, 1);

    EdgeGrowParams p;
    p.reach = 20.0;
    p.edgeStop = 0.5;
    const Selection stopped = edgeGrowSelection(img, seeds, p);
    REQUIRE_FALSE(stopped.isEmpty());
    CHECK(stopped.at(24, 32) == 255);  // the seed
    CHECK(stopped.at(30, 32) >= 128);  // flat green up to the edge: selected
    CHECK(stopped.at(40, 32) == 0);    // across the edge: the grow was arrested
    CHECK(stopped.at(50, 32) == 0);
    // ...but the same reach in the open directions is unimpeded (leftwards is flat green).
    CHECK(stopped.at(24 - 18, 32) >= 128);

    p.edgeStop = 0.0; // edges off -> a plain disc that sails straight across the boundary
    const Selection open = edgeGrowSelection(img, seeds, p);
    CHECK(open.at(40, 32) >= 128); // 16 px away, well inside reach 20
}

TEST_CASE("the edge wall is watertight: growth goes around, never through") {
    // A 2px-wide red wall with a gap is a classic geodesic case: the grow must reach the far side
    // only through the gap, i.e. pixels behind the wall far from the gap stay unselected while
    // pixels behind the gap select.
    Image img = solid(64, 64, kGreen);
    fillRect(img, 31, 0, 2, 28, kRed);  // wall above the gap
    fillRect(img, 31, 36, 2, 28, kRed); // wall below the gap (gap: y in [28,36))
    const Selection seeds = seedRect(64, 64, 20, 8, 1, 1); // left side, far from the gap

    EdgeGrowParams p;
    p.reach = 40.0;
    p.edgeStop = 1.0;
    const Selection s = edgeGrowSelection(img, seeds, p);
    REQUIRE_FALSE(s.isEmpty());
    CHECK(s.at(28, 8) >= 128);  // left of the wall at seed height: selected
    CHECK(s.at(40, 8) == 0);    // directly across the wall: NOT selected (the wall held)
    // Through the gap: (20,8) -> gap (~32,32) -> (40,32) is a ~38 px walk, within reach 40.
    CHECK(s.at(40, 32) >= 128);
}

TEST_CASE("ramp AA: a soft sub-128 fringe rings the >=128 interior, and interior is connected") {
    const Image img = solid(48, 48, kGrey);
    const Selection seeds = seedRect(48, 48, 24, 24, 1, 1);

    EdgeGrowParams p;
    p.reach = 8.0;
    const Selection s = edgeGrowSelection(img, seeds, p);
    REQUIRE_FALSE(s.isEmpty());

    // Fringe pixels (0 < v < 128) exist and each touches the >=128 interior within one step --
    // the AA band is a boundary ramp, not scattered speckle.
    bool sawFringe = false;
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 48; ++x) {
            const std::uint8_t v = s.at(x, y);
            if (v == 0 || v >= 128)
                continue;
            sawFringe = true;
            bool touches = false;
            for (int dy = -1; dy <= 1 && !touches; ++dy)
                for (int dx = -1; dx <= 1 && !touches; ++dx)
                    if (s.at(x + dx, y + dy) >= 128)
                        touches = true;
            CHECK(touches);
        }
    CHECK(sawFringe);
}

TEST_CASE("seed semantics: sub-threshold seed coverage seeds nothing; mismatches are empty") {
    const Image img = solid(16, 16, kGrey);

    // A seed mask whose coverage never reaches the ants threshold has no full-coverage core.
    Selection faint(16, 16);
    faint.data()[8 * 16 + 8] = 100; // < kAntsCoverageThreshold
    CHECK(edgeGrowSelection(img, faint, {}).isEmpty());

    CHECK(edgeGrowSelection(img, Selection{}, {}).isEmpty());          // empty seeds
    CHECK(edgeGrowSelection(Image{}, seedRect(16, 16, 8, 8, 1, 1), {}).isEmpty()); // empty src
    CHECK(edgeGrowSelection(img, seedRect(8, 8, 4, 4, 1, 1), {}).isEmpty());       // size mismatch
}

TEST_CASE("reach 0 still selects the stroke itself (solid), with only the AA band beyond") {
    const Image img = solid(16, 16, kGrey);
    const Selection seeds = seedRect(16, 16, 6, 6, 4, 4);

    EdgeGrowParams p;
    p.reach = 0.0;
    const Selection s = edgeGrowSelection(img, seeds, p);
    REQUIRE_FALSE(s.isEmpty());
    for (int y = 6; y < 10; ++y)
        for (int x = 6; x < 10; ++x)
            CHECK(s.at(x, y) >= 128); // every seed pixel stays enclosed by the ants
    CHECK(s.at(6 + 5, 8) == 0);       // nothing grows solidly outward
    CHECK(countInside(s) == 16);      // the >=128 set is exactly the stroke core (d<=0 == seeds)
}

TEST_CASE("determinism: the same inputs produce byte-identical masks") {
    Image img = solid(48, 48, kGreen);
    fillRect(img, 20, 0, 3, 48, kRed);
    // A deterministic pseudo-random speckle so the field is not trivially uniform.
    unsigned state = 12345u;
    for (std::size_t i = 0; i < img.pixelCount(); ++i) {
        state = state * 1664525u + 1013904223u;
        img.rgba[i * 4 + 1] = static_cast<std::uint8_t>(128 + (state >> 28));
    }
    const Selection seeds = seedRect(48, 48, 8, 20, 3, 3);

    EdgeGrowParams p;
    p.reach = 25.0;
    p.edgeStop = 0.7;
    const Selection a = edgeGrowSelection(img, seeds, p);
    const Selection b = edgeGrowSelection(img, seeds, p);
    CHECK(a == b);
}
