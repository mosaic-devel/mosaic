#include "ui/color_models.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>

// RGB <-> HSL / HSV conversions for the picker's model combo (PLAN S12-a).
namespace {

using mosaic::common::Color8;
using mosaic::ui::Hsl;
using mosaic::ui::Hsv;

bool near(float a, float b, float eps = 0.005F) {
    return std::abs(a - b) <= eps;
}

bool channelsNear(Color8 a, Color8 b, int eps = 1) {
    return std::abs(int{a.r} - int{b.r}) <= eps && std::abs(int{a.g} - int{b.g}) <= eps &&
           std::abs(int{a.b} - int{b.b}) <= eps && a.a == b.a;
}

} // namespace

TEST_CASE("rgbToHsv: primaries, greys, and known mixes") {
    const Hsv red = mosaic::ui::rgbToHsv({255, 0, 0, 255});
    CHECK(near(red.h, 0.0F));
    CHECK(near(red.s, 1.0F));
    CHECK(near(red.v, 1.0F));

    const Hsv green = mosaic::ui::rgbToHsv({0, 255, 0, 255});
    CHECK(near(green.h, 120.0F, 0.1F));
    const Hsv blue = mosaic::ui::rgbToHsv({0, 0, 255, 255});
    CHECK(near(blue.h, 240.0F, 0.1F));

    // Greys: saturation 0, hue conventionally 0, value = level.
    const Hsv grey = mosaic::ui::rgbToHsv({128, 128, 128, 255});
    CHECK(near(grey.h, 0.0F));
    CHECK(near(grey.s, 0.0F));
    CHECK(near(grey.v, 128.0F / 255.0F));

    // Photoshop-style spot check: a half-bright orange.
    const Hsv orange = mosaic::ui::rgbToHsv({128, 64, 0, 255});
    CHECK(near(orange.h, 30.0F, 0.5F));
    CHECK(near(orange.s, 1.0F));
    CHECK(near(orange.v, 128.0F / 255.0F));
}

TEST_CASE("rgbToHsl: primaries, greys, and known mixes") {
    const Hsl red = mosaic::ui::rgbToHsl({255, 0, 0, 255});
    CHECK(near(red.h, 0.0F));
    CHECK(near(red.s, 1.0F));
    CHECK(near(red.l, 0.5F));

    const Hsl white = mosaic::ui::rgbToHsl({255, 255, 255, 255});
    CHECK(near(white.s, 0.0F));
    CHECK(near(white.l, 1.0F));
    const Hsl black = mosaic::ui::rgbToHsl({0, 0, 0, 255});
    CHECK(near(black.s, 0.0F));
    CHECK(near(black.l, 0.0F));

    // A pastel: light desaturated cyan-ish blue.
    const Hsl pastel = mosaic::ui::rgbToHsl({128, 192, 192, 255});
    CHECK(near(pastel.h, 180.0F, 0.5F));
    CHECK(near(pastel.l, 160.0F / 255.0F, 0.01F));
}

TEST_CASE("hsvToRgb / hslToRgb: inverse spot values + hue wrap + clamping") {
    CHECK(channelsNear(mosaic::ui::hsvToRgb({0.0F, 1.0F, 1.0F}), {255, 0, 0, 255}));
    CHECK(channelsNear(mosaic::ui::hsvToRgb({120.0F, 1.0F, 1.0F}), {0, 255, 0, 255}));
    CHECK(channelsNear(mosaic::ui::hslToRgb({240.0F, 1.0F, 0.5F}), {0, 0, 255, 255}));
    CHECK(channelsNear(mosaic::ui::hslToRgb({0.0F, 0.0F, 0.5F}), {128, 128, 128, 255}));

    // Out-of-range inputs are tolerated: hue wraps (h = 360 == h = 0, negatives wrap up),
    // saturation/lightness/value clamp -- slider/drag code can hand us raw values.
    CHECK(channelsNear(mosaic::ui::hsvToRgb({360.0F, 1.0F, 1.0F}), {255, 0, 0, 255}));
    CHECK(channelsNear(mosaic::ui::hsvToRgb({-120.0F, 1.0F, 1.0F}), {0, 0, 255, 255}));
    CHECK(channelsNear(mosaic::ui::hslToRgb({0.0F, 2.0F, -0.5F}), {0, 0, 0, 255}));
}

TEST_CASE("round-trips: RGB -> model -> RGB is identity (within rounding) on a channel grid") {
    // Sweep a coarse-but-representative grid of the cube (incl. 0 and 255 endpoints).
    for (int r = 0; r <= 255; r += 51) {
        for (int g = 0; g <= 255; g += 51) {
            for (int b = 0; b <= 255; b += 51) {
                const Color8 in{static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                                static_cast<std::uint8_t>(b), 255};
                CHECK(channelsNear(mosaic::ui::hsvToRgb(mosaic::ui::rgbToHsv(in)), in));
                CHECK(channelsNear(mosaic::ui::hslToRgb(mosaic::ui::rgbToHsl(in)), in));
            }
        }
    }
}
