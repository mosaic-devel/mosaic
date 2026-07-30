#include <doctest/doctest.h>

#include "ui/color_state.hpp"
#include "ui/color_swatch.hpp" // swatchHitRegion + the swatch's pure geometry

using mosaic::common::Color8;
using mosaic::ui::ColorState;
using mosaic::ui::hexString;
using mosaic::ui::parseHexColor;
using mosaic::ui::SwatchRegion;
using mosaic::ui::swatchHitRegion;

TEST_CASE("ColorState starts black-on-white and mutates with observer notifications") {
    ColorState s;
    CHECK(s.foreground() == Color8{0, 0, 0, 255});   // black
    CHECK(s.background() == Color8{255, 255, 255, 255}); // white

    int fires = 0;
    s.addObserver([&] { ++fires; });

    s.setForeground({10, 20, 30, 255});
    CHECK(fires == 1);
    CHECK(s.foreground() == Color8{10, 20, 30, 255});

    s.setForeground({10, 20, 30, 255}); // unchanged -> no notification
    CHECK(fires == 1);

    s.swap();
    CHECK(fires == 2);
    CHECK(s.foreground() == Color8{255, 255, 255, 255});
    CHECK(s.background() == Color8{10, 20, 30, 255});

    s.reset();
    CHECK(fires == 3);
    CHECK(s.foreground() == Color8{0, 0, 0, 255});
    CHECK(s.background() == Color8{255, 255, 255, 255});
}

TEST_CASE("ColorState notifies every registered observer") {
    ColorState s;
    int a = 0;
    int b = 0;
    s.addObserver([&] { ++a; });
    s.addObserver([&] { ++b; });
    s.setBackground({1, 2, 3, 255});
    CHECK(a == 1);
    CHECK(b == 1);
}

TEST_CASE("parseHexColor accepts #RRGGBB and #RGB, with or without '#'/whitespace") {
    CHECK(parseHexColor("#FFFFFF") == Color8{255, 255, 255, 255});
    CHECK(parseHexColor("ffffff") == Color8{255, 255, 255, 255});
    CHECK(parseHexColor("#000000") == Color8{0, 0, 0, 255});
    CHECK(parseHexColor("#FF0000") == Color8{255, 0, 0, 255});
    CHECK(parseHexColor("#1A2B3C") == Color8{0x1A, 0x2B, 0x3C, 255});
    CHECK(parseHexColor("  #1a2b3c  ") == Color8{0x1A, 0x2B, 0x3C, 255}); // trimmed + lowercase

    // Three-digit shorthand doubles each nibble.
    CHECK(parseHexColor("#F00") == Color8{255, 0, 0, 255});
    CHECK(parseHexColor("0f0") == Color8{0, 255, 0, 255});
    CHECK(parseHexColor("#abc") == Color8{0xAA, 0xBB, 0xCC, 255});

    // Parsed colours are always opaque (alpha handling arrives with S12).
    CHECK(parseHexColor("#123456")->a == 255);
}

TEST_CASE("parseHexColor rejects malformed input") {
    CHECK_FALSE(parseHexColor("").has_value());
    CHECK_FALSE(parseHexColor("#").has_value());
    CHECK_FALSE(parseHexColor("#12").has_value());      // too short
    CHECK_FALSE(parseHexColor("#12345").has_value());   // 5 digits
    CHECK_FALSE(parseHexColor("#1234567").has_value()); // too long
    CHECK_FALSE(parseHexColor("#GGGGGG").has_value());  // non-hex
    CHECK_FALSE(parseHexColor("xyz").has_value());
}

TEST_CASE("hexString formats uppercase #RRGGBB and round-trips through parseHexColor") {
    CHECK(hexString({255, 255, 255, 255}) == "#FFFFFF");
    CHECK(hexString({0, 0, 0, 255}) == "#000000");
    CHECK(hexString({0x1A, 0x2B, 0x3C, 255}) == "#1A2B3C");

    const Color8 c{17, 200, 99, 255};
    CHECK(parseHexColor(hexString(c)) == c);
}

TEST_CASE("swatchHitRegion classifies the swatch's clickable regions") {
    const int w = mosaic::ui::kSwatchW;
    const int h = mosaic::ui::kSwatchH;

    CHECK(swatchHitRegion(6, 6, w, h) == SwatchRegion::Foreground);
    CHECK(swatchHitRegion(28, 4, w, h) == SwatchRegion::Swap);   // top-right glyph
    CHECK(swatchHitRegion(4, 35, w, h) == SwatchRegion::Reset);  // bottom-left glyph
    CHECK(swatchHitRegion(30, 30, w, h) == SwatchRegion::Background); // back chip, clear of the front
    CHECK(swatchHitRegion(15, 15, w, h) == SwatchRegion::Foreground); // overlap -> front wins

    CHECK(swatchHitRegion(33, 39, w, h) == SwatchRegion::None); // empty corner
    CHECK(swatchHitRegion(-1, 5, w, h) == SwatchRegion::None);  // outside bounds
    CHECK(swatchHitRegion(40, 5, w, h) == SwatchRegion::None);
}
