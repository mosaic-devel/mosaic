#include "ui/color_models.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mosaic::ui {
namespace {

constexpr float kInv255 = 1.0F / 255.0F;

std::uint8_t channel8(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
}

// Shared hue computation (identical for HSL and HSV): degrees in [0, 360), 0 for greys.
float hueOf(float r, float g, float b, float max, float chroma) {
    if (chroma == 0.0F)
        return 0.0F;
    float h = 0.0F;
    if (max == r)
        h = std::fmod((g - b) / chroma, 6.0F);
    else if (max == g)
        h = (b - r) / chroma + 2.0F;
    else
        h = (r - g) / chroma + 4.0F;
    h *= 60.0F;
    return h < 0.0F ? h + 360.0F : h;
}

// hue' in [0, 6): the sextant form both *ToRgb conversions share. C = chroma, X = intermediate.
common::Color8 fromChroma(float h, float chroma, float x, float m) {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    if (h < 1.0F) {
        r = chroma;
        g = x;
    } else if (h < 2.0F) {
        r = x;
        g = chroma;
    } else if (h < 3.0F) {
        g = chroma;
        b = x;
    } else if (h < 4.0F) {
        g = x;
        b = chroma;
    } else if (h < 5.0F) {
        r = x;
        b = chroma;
    } else {
        r = chroma;
        b = x;
    }
    return {channel8(r + m), channel8(g + m), channel8(b + m), 255};
}

float wrapHue(float h) {
    h = std::fmod(h, 360.0F);
    return h < 0.0F ? h + 360.0F : h;
}

} // namespace

Hsl rgbToHsl(common::Color8 c) {
    const float r = static_cast<float>(c.r) * kInv255;
    const float g = static_cast<float>(c.g) * kInv255;
    const float b = static_cast<float>(c.b) * kInv255;
    const float max = std::max({r, g, b});
    const float min = std::min({r, g, b});
    const float chroma = max - min;
    const float l = (max + min) * 0.5F;
    const float s = chroma == 0.0F ? 0.0F : chroma / (1.0F - std::abs(2.0F * l - 1.0F));
    return {hueOf(r, g, b, max, chroma), s, l};
}

Hsv rgbToHsv(common::Color8 c) {
    const float r = static_cast<float>(c.r) * kInv255;
    const float g = static_cast<float>(c.g) * kInv255;
    const float b = static_cast<float>(c.b) * kInv255;
    const float max = std::max({r, g, b});
    const float min = std::min({r, g, b});
    const float chroma = max - min;
    return {hueOf(r, g, b, max, chroma), max == 0.0F ? 0.0F : chroma / max, max};
}

common::Color8 hslToRgb(Hsl c) {
    const float h = wrapHue(c.h) / 60.0F;
    const float s = std::clamp(c.s, 0.0F, 1.0F);
    const float l = std::clamp(c.l, 0.0F, 1.0F);
    const float chroma = (1.0F - std::abs(2.0F * l - 1.0F)) * s;
    const float x = chroma * (1.0F - std::abs(std::fmod(h, 2.0F) - 1.0F));
    return fromChroma(h, chroma, x, l - chroma * 0.5F);
}

common::Color8 hsvToRgb(Hsv c) {
    const float h = wrapHue(c.h) / 60.0F;
    const float s = std::clamp(c.s, 0.0F, 1.0F);
    const float v = std::clamp(c.v, 0.0F, 1.0F);
    const float chroma = v * s;
    const float x = chroma * (1.0F - std::abs(std::fmod(h, 2.0F) - 1.0F));
    return fromChroma(h, chroma, x, v - chroma);
}

} // namespace mosaic::ui
