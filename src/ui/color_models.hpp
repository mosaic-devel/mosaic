#pragma once

#include "common/image.hpp" // Color8

// Colour-model conversions for the picker's model combo (PLAN S12-a): RGB <-> HSL and RGB <-> HSV,
// pure float math, FLTK-free and unit-tested. Lab is deliberately absent -- it needs a working-space
// white point, so it arrives with lcms2 in S12-b.
//
// Conventions: h in [0, 360) degrees; s/l/v in [0, 1]. Conversions to Color8 round per channel and
// leave alpha at 255 (the picker is opaque-only until full alpha lands). Note the classic
// degeneracies: at s == 0 (greys) hue is meaningless, and rgbToHs*() returns h = 0 -- the picker
// therefore keeps its own working HSL/HSV while the user edits, instead of round-tripping through
// RGB on every change (see ColorPicker).
namespace mosaic::ui {

struct Hsl {
    float h = 0.0F;
    float s = 0.0F;
    float l = 0.0F;
};

struct Hsv {
    float h = 0.0F;
    float s = 0.0F;
    float v = 0.0F;
};

[[nodiscard]] Hsl rgbToHsl(common::Color8 c);
[[nodiscard]] Hsv rgbToHsv(common::Color8 c);
[[nodiscard]] common::Color8 hslToRgb(Hsl c);
[[nodiscard]] common::Color8 hsvToRgb(Hsv c);

} // namespace mosaic::ui
