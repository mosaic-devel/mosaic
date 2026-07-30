#include <doctest/doctest.h>

#include "core/color_sample.hpp"

using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::core::sampleColor;
using mosaic::core::sampleRadius;
using mosaic::core::SampleSize;
using mosaic::core::sampleSizeFromIndex;

namespace {
// A small helper: write one pixel of `img`.
void put(Image& img, int x, int y, Color8 c) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    img.rgba[p + 0] = c.r;
    img.rgba[p + 1] = c.g;
    img.rgba[p + 2] = c.b;
    img.rgba[p + 3] = c.a;
}
} // namespace

TEST_CASE("sampleRadius / sampleSizeFromIndex map the option table") {
    CHECK(sampleRadius(SampleSize::Point) == 0);
    CHECK(sampleRadius(SampleSize::Avg3) == 1);
    CHECK(sampleRadius(SampleSize::Avg5) == 2);
    CHECK(sampleRadius(SampleSize::Avg11) == 5);

    CHECK(sampleSizeFromIndex(0) == SampleSize::Point);
    CHECK(sampleSizeFromIndex(1) == SampleSize::Avg3);
    CHECK(sampleSizeFromIndex(2) == SampleSize::Avg5);
    CHECK(sampleSizeFromIndex(3) == SampleSize::Avg11);
    CHECK(sampleSizeFromIndex(-1) == SampleSize::Point); // out of range clamps to Point
    CHECK(sampleSizeFromIndex(99) == SampleSize::Point);
}

TEST_CASE("Point sample returns the exact pixel under the cursor") {
    Image img(4, 4);
    put(img, 2, 1, {10, 20, 30, 200});
    const auto c = sampleColor(img, 2, 1, SampleSize::Point);
    REQUIRE(c.has_value());
    CHECK(*c == Color8{10, 20, 30, 200});
}

TEST_CASE("Out-of-bounds and empty images pick nothing") {
    Image img(4, 4);
    CHECK_FALSE(sampleColor(img, -1, 0, SampleSize::Point).has_value());
    CHECK_FALSE(sampleColor(img, 0, -1, SampleSize::Point).has_value());
    CHECK_FALSE(sampleColor(img, 4, 0, SampleSize::Point).has_value());
    CHECK_FALSE(sampleColor(img, 0, 4, SampleSize::Point).has_value());
    CHECK_FALSE(sampleColor(Image{}, 0, 0, SampleSize::Avg3).has_value());
}

TEST_CASE("3x3 average is the mean of the window, rounded to nearest") {
    // A uniform grey field with one white pixel dead centre: the 3x3 average lifts the centre
    // toward white by exactly one ninth of the difference.
    Image img(3, 3);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            put(img, x, y, {90, 90, 90, 255});
    put(img, 1, 1, {180, 180, 180, 255});
    // mean = (8*90 + 180) / 9 = 900/9 = 100 exactly.
    const auto c = sampleColor(img, 1, 1, SampleSize::Avg3);
    REQUIRE(c.has_value());
    CHECK(*c == Color8{100, 100, 100, 255});
}

TEST_CASE("Rounding is to nearest, not truncation") {
    // Two pixels: 0 and 1 -> mean 0.5 -> rounds to 1 (a truncating divide would give 0).
    Image img(2, 1);
    put(img, 0, 0, {0, 0, 0, 0});
    put(img, 1, 0, {1, 3, 5, 7});
    // A 3x3 window at (0,0) clips to the two-pixel row: mean of {0,1}={0,1} etc.
    const auto c = sampleColor(img, 0, 0, SampleSize::Avg3);
    REQUIRE(c.has_value());
    // means: 0.5->1, 1.5->2, 2.5->3, 3.5->4 (round-half-up, not truncation).
    CHECK(*c == Color8{1, 2, 3, 4});
}

TEST_CASE("The window clips at the image edge -- a corner averages only real pixels") {
    // A 2x2 image, all channels 40 except the picked corner which is 80. A 5x5 window at a corner
    // clips to the whole 2x2, so the mean is (3*40 + 80)/4 = 200/4 = 50.
    Image img(2, 2);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            put(img, x, y, {40, 40, 40, 40});
    put(img, 0, 0, {80, 80, 80, 80});
    const auto c = sampleColor(img, 0, 0, SampleSize::Avg5);
    REQUIRE(c.has_value());
    CHECK(*c == Color8{50, 50, 50, 50});
}

TEST_CASE("Alpha is averaged like the colour channels") {
    Image img(1, 2);
    put(img, 0, 0, {200, 100, 50, 0});
    put(img, 0, 1, {200, 100, 50, 254});
    const auto c = sampleColor(img, 0, 0, SampleSize::Avg3); // window clips to the 1x2 column
    REQUIRE(c.has_value());
    CHECK(*c == Color8{200, 100, 50, 127}); // (0+254)/2 = 127
}
