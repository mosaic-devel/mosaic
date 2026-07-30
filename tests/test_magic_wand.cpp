#include "common/image.hpp"
#include "core/selection.hpp"

#include <doctest/doctest.h>

// The magic wand's core engine (S17, docs/research-selection.md §9 commit 1): the weighted-RGBA
// distance metric, the 4-connected scanline flood (contiguous) + global scan, the distance-ramp AA,
// and the "no coverage -> no selection" rule. Pure, GPU-free, synthetic images -- the same test
// posture Selection::polygon/ellipse have. The canvas click->commit shell is verified visually.
namespace {

using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::core::magicWandSelection;
using mosaic::core::Selection;
using mosaic::core::wandColorDistance;
using mosaic::core::WandParams;

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

void put(Image& img, int x, int y, Color8 c) {
    const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 4;
    img.rgba[i + 0] = c.r;
    img.rgba[i + 1] = c.g;
    img.rgba[i + 2] = c.b;
    img.rgba[i + 3] = c.a;
}

void fillRect(Image& img, int x0, int y0, int w, int h, Color8 c) {
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x)
            put(img, x, y, c);
}

std::size_t countSelected(const Selection& s) {
    std::size_t n = 0;
    for (std::uint8_t v : s.data())
        if (v > 0)
            ++n;
    return n;
}

constexpr Color8 kRed{255, 0, 0, 255};
constexpr Color8 kGreen{0, 255, 0, 255};
constexpr Color8 kBlue{0, 0, 255, 255};

} // namespace

TEST_CASE("wandColorDistance: normalised to [0,1], identical pixels are zero") {
    CHECK(wandColorDistance(kRed, kRed, true) == doctest::Approx(0.0));
    // Black vs white, RGB only: the luma weights sum to 1, so the max colour distance is exactly 1.
    CHECK(wandColorDistance({0, 0, 0, 255}, {255, 255, 255, 255}, false) == doctest::Approx(1.0));
    // Same colour, opaque vs fully transparent: only the alpha term contributes (weight 0.20).
    const double da = wandColorDistance({128, 128, 128, 255}, {128, 128, 128, 0}, true);
    CHECK(da == doctest::Approx(std::sqrt(0.20)));
    // Ignoring alpha, that same pair is a perfect match.
    CHECK(wandColorDistance({128, 128, 128, 255}, {128, 128, 128, 0}, false) == doctest::Approx(0.0));
}

TEST_CASE("contiguous flood: selects the clicked solid region, tight bounds, leaves the rest") {
    Image img = solid(16, 16, kGreen);
    fillRect(img, 4, 4, 8, 8, kRed); // a red block on a green field

    WandParams p;
    p.tolerance = 0.1;
    p.antialias = false;
    const Selection s = magicWandSelection(img, 6, 6, p); // click inside the red block

    CHECK(s.width() == 16);
    CHECK(s.height() == 16);
    CHECK(s.at(6, 6) == 255);
    CHECK(s.at(4, 4) == 255);
    CHECK(s.at(11, 11) == 255);
    CHECK(s.at(3, 4) == 0);   // just outside the block: green, unselected
    CHECK(s.at(12, 12) == 0);
    CHECK(countSelected(s) == 64); // exactly the 8x8 block
    const auto b = s.bounds();
    REQUIRE(b.has_value());
    CHECK(b->x == 4);
    CHECK(b->y == 4);
    CHECK(b->w == 8);
    CHECK(b->h == 8);
}

TEST_CASE("connectivity is 4-connected: a diagonal touch does not bridge regions") {
    // Two 2x2 red blocks meeting only at the corner (1,1)/(2,2), on a green field.
    Image img = solid(4, 4, kGreen);
    fillRect(img, 0, 0, 2, 2, kRed);
    fillRect(img, 2, 2, 2, 2, kRed);

    WandParams p;
    p.tolerance = 0.1;
    p.antialias = false;
    const Selection s = magicWandSelection(img, 0, 0, p); // click the top-left block

    CHECK(s.at(0, 0) == 255);
    CHECK(s.at(1, 1) == 255);
    CHECK(countSelected(s) == 4);   // only the top-left block...
    CHECK(s.at(2, 2) == 0);         // ...never leaks across the diagonal touch (would be 8-connected)
    CHECK(s.at(3, 3) == 0);
}

TEST_CASE("global match ignores connectivity: both same-colour regions are selected") {
    Image img = solid(4, 4, kGreen);
    fillRect(img, 0, 0, 2, 2, kRed);
    fillRect(img, 2, 2, 2, 2, kRed);

    WandParams p;
    p.tolerance = 0.1;
    p.antialias = false;
    p.contiguous = false; // "select by colour"
    const Selection s = magicWandSelection(img, 0, 0, p);

    CHECK(s.at(0, 0) == 255);
    CHECK(s.at(2, 2) == 255);       // the diagonally-separated block IS selected now
    CHECK(s.at(3, 3) == 255);
    CHECK(countSelected(s) == 8);   // both red blocks, no green
}

TEST_CASE("tolerance thresholds the metric: 0 selects only exact matches, 1 selects all") {
    // A 256x1 greyscale ramp: pixel x has grey value x, so distance(seed=x0) == x/255 (alpha off).
    Image img(256, 1);
    for (int x = 0; x < 256; ++x)
        put(img, x, 0, {static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(x),
                        static_cast<std::uint8_t>(x), 255});

    WandParams p;
    p.antialias = false;
    p.sampleAlpha = false; // keep the metric exactly x/255 for a crisp count

    p.tolerance = 0.0;
    CHECK(countSelected(magicWandSelection(img, 0, 0, p)) == 1); // only the seed (x/255 <= 0 => x==0)

    p.tolerance = 0.5; // x/255 <= 0.5 => x in [0,127]
    const Selection half = magicWandSelection(img, 0, 0, p);
    CHECK(half.at(127, 0) == 255);
    CHECK(half.at(128, 0) == 0);
    CHECK(countSelected(half) == 128);

    p.tolerance = 1.0; // everything (the run is connected from x=0)
    CHECK(countSelected(magicWandSelection(img, 0, 0, p)) == 256);
}

TEST_CASE("anti-alias: soft fringe below the ants threshold, hard interior at >=128") {
    Image img(256, 1);
    for (int x = 0; x < 256; ++x)
        put(img, x, 0, {static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(x),
                        static_cast<std::uint8_t>(x), 255});

    WandParams p;
    p.sampleAlpha = false;
    p.tolerance = 0.5;

    p.antialias = false;
    const Selection hard = magicWandSelection(img, 0, 0, p);
    p.antialias = true;
    const Selection aa = magicWandSelection(img, 0, 0, p);

    // The >=128 (marching-ants) set is byte-for-byte the same set the hard flood selected -- the AA
    // never grows the enclosed region, only softens its edge (research §5: ants track the 0.5 line).
    for (std::uint32_t x = 0; x < 256; ++x)
        CHECK((aa.at(x, 0) >= 128) == (hard.at(x, 0) == 255));

    CHECK(aa.at(0, 0) == 255); // solidly inside -> full coverage

    // A soft outer fringe exists: some pixel just past the tolerance carries partial (sub-128) coverage.
    bool sawPartial = false;
    for (std::uint32_t x = 0; x < 256; ++x)
        if (aa.at(x, 0) > 0 && aa.at(x, 0) < 128)
            sawPartial = true;
    CHECK(sawPartial);
    CHECK(aa.at(128, 0) > 0);   // first pixel past T=0.5 gets fringe...
    CHECK(aa.at(128, 0) < 128); // ...but never crosses into the selected set (no leak)
}

TEST_CASE("alpha in the distance: a transparent pixel walls off the flood (RGBA vs RGB)") {
    // Same grey RGB throughout; the middle pixel is transparent. With alpha on, it blocks the flood.
    Image img = solid(3, 1, {128, 128, 128, 255});
    put(img, 1, 0, {128, 128, 128, 0}); // transparent, identical RGB

    WandParams p;
    p.tolerance = 0.15;
    p.antialias = false;

    p.sampleAlpha = true; // the transparent pixel is far in alpha -> flood stops at the seed
    CHECK(countSelected(magicWandSelection(img, 0, 0, p)) == 1);

    p.sampleAlpha = false; // alpha ignored -> all three identical-RGB pixels select
    CHECK(countSelected(magicWandSelection(img, 0, 0, p)) == 3);
}

TEST_CASE("invalid seed or empty source -> no selection (not an all-zero mask)") {
    const Image empty;
    CHECK(magicWandSelection(empty, 0, 0, WandParams{}).isEmpty());

    const Image img = solid(4, 4, kBlue);
    CHECK(magicWandSelection(img, -1, 0, WandParams{}).isEmpty());
    CHECK(magicWandSelection(img, 4, 0, WandParams{}).isEmpty());
    CHECK(magicWandSelection(img, 0, 9, WandParams{}).isEmpty());

    // A valid click always selects at least the seed pixel (never an empty "selected nothing").
    const Selection s = magicWandSelection(img, 2, 2, WandParams{});
    CHECK_FALSE(s.isEmpty());
    CHECK(s.anySelected());
}
