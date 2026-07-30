#pragma once

#include <algorithm>
#include <cmath>

#include "common/image.hpp"
#include "core/blend_mode.hpp"

// Blend-mode math (S7). Header-only and pure so it has a single definition shared by the CPU
// reference compositor, the brush engine, the unit tests, and -- mirrored in GLSL -- the GPU
// compositor (S7-b). The formulas follow the W3C "Compositing and Blending Level 1" spec,
// which matches the Photoshop/Krita conventions the `core::BlendMode` enum is ordered by.
//
// It lives in core/ rather than render/ because `mosaic_render` links Vulkan and depends on
// `mosaic_core`, while the brush engine (core/brush, deliberately Vulkan- and platform-free) needs
// these formulas at composite time (docs/brushes.md §6.1). `render/blend.hpp` re-exports the names
// under `mosaic::render` so the compositor's call sites are unchanged. Nothing here is
// render-specific: it is arithmetic on colours.
//
// Everything operates on straight (non-premultiplied) colors with channels in [0,1]. The
// document is composited in its encoded (gamma) space for now -- the same space Photoshop
// blends in by default -- so results match user expectations; linear-light compositing and ICC
// transforms arrive with color management (S12, PLAN §3.6).
namespace mosaic::core {

namespace detail {

// A color-only (no alpha) triple, used by the non-separable (HSL) blend modes.
struct Rgb {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

// W3C luminance for the non-separable modes (the spec's 0.3/0.59/0.11 weights).
[[nodiscard]] inline float lum(Rgb c) noexcept { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; }

// Clip a color back into [0,1] while preserving its luminance (W3C ClipColor).
[[nodiscard]] inline Rgb clipColor(Rgb c) noexcept {
    const float l = lum(c);
    const float n = std::min({c.r, c.g, c.b});
    const float x = std::max({c.r, c.g, c.b});
    if (n < 0.0f && l - n > 0.0f) {
        const float k = l / (l - n);
        c = {l + (c.r - l) * k, l + (c.g - l) * k, l + (c.b - l) * k};
    }
    if (x > 1.0f && x - l > 0.0f) {
        const float k = (1.0f - l) / (x - l);
        c = {l + (c.r - l) * k, l + (c.g - l) * k, l + (c.b - l) * k};
    }
    return c;
}

// Shift `c` to have luminance `l` (W3C SetLum).
[[nodiscard]] inline Rgb setLum(Rgb c, float l) noexcept {
    const float d = l - lum(c);
    return clipColor({c.r + d, c.g + d, c.b + d});
}

[[nodiscard]] inline float sat(Rgb c) noexcept {
    return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b});
}

// Set the saturation of `c` to `s`, keeping the channels' relative order (W3C SetSat). The
// canonical vector form `(c - min) * s / (max - min)` maps min->0, max->s and scales the mid
// channel between -- identical per channel, and identical to the GLSL mirror in S7-b.
[[nodiscard]] inline Rgb setSat(Rgb c, float s) noexcept {
    const float mn = std::min({c.r, c.g, c.b});
    const float mx = std::max({c.r, c.g, c.b});
    if (mx <= mn) return {0.0f, 0.0f, 0.0f};
    const float k = s / (mx - mn);
    return {(c.r - mn) * k, (c.g - mn) * k, (c.b - mn) * k};
}

[[nodiscard]] inline Rgb blendNonSeparable(core::BlendMode m, Rgb b, Rgb s) noexcept {
    using enum core::BlendMode;
    switch (m) {
        case Hue: return setLum(setSat(s, sat(b)), lum(b));
        case Saturation: return setLum(setSat(b, sat(s)), lum(b));
        case Color: return setLum(s, lum(b));
        case Luminosity: return setLum(b, lum(s));
        default: return s;
    }
}

}  // namespace detail

// The four HSL modes act on the RGB triple as a whole; the rest act per channel.
[[nodiscard]] inline bool isSeparable(core::BlendMode m) noexcept {
    using enum core::BlendMode;
    return m != Hue && m != Saturation && m != Color && m != Luminosity;
}

// Separable blend of one channel: backdrop `b` against source `s`, both in [0,1].
[[nodiscard]] inline float blendChannel(core::BlendMode m, float b, float s) noexcept {
    using enum core::BlendMode;
    switch (m) {
        case Normal: return s;
        case Darken: return std::min(b, s);
        case Multiply: return b * s;
        case ColorBurn: return s <= 0.0f ? 0.0f : 1.0f - std::min(1.0f, (1.0f - b) / s);
        case LinearBurn: return std::max(0.0f, b + s - 1.0f);
        case Lighten: return std::max(b, s);
        case Screen: return b + s - b * s;
        case ColorDodge: return s >= 1.0f ? 1.0f : std::min(1.0f, b / (1.0f - s));
        case LinearDodge: return std::min(1.0f, b + s);
        case Overlay: return b <= 0.5f ? 2.0f * b * s : 1.0f - 2.0f * (1.0f - b) * (1.0f - s);
        case SoftLight: {
            if (s <= 0.5f) return b - (1.0f - 2.0f * s) * b * (1.0f - b);
            const float d = b <= 0.25f ? ((16.0f * b - 12.0f) * b + 4.0f) * b : std::sqrt(b);
            return b + (2.0f * s - 1.0f) * (d - b);
        }
        case HardLight: return s <= 0.5f ? 2.0f * b * s : 1.0f - 2.0f * (1.0f - b) * (1.0f - s);
        case VividLight: {
            if (s <= 0.5f) {
                const float d = 2.0f * s;
                return d <= 0.0f ? 0.0f : std::max(0.0f, 1.0f - (1.0f - b) / d);
            }
            const float d = 2.0f * (1.0f - s);
            return d <= 0.0f ? 1.0f : std::min(1.0f, b / d);
        }
        case LinearLight: return std::clamp(b + 2.0f * s - 1.0f, 0.0f, 1.0f);
        case PinLight: return s <= 0.5f ? std::min(b, 2.0f * s) : std::max(b, 2.0f * s - 1.0f);
        case Difference: return std::abs(b - s);
        case Exclusion: return b + s - 2.0f * b * s;
        case Subtract: return std::max(0.0f, b - s);
        case Divide: return s <= 0.0f ? 1.0f : std::min(1.0f, b / s);
        // Non-separable modes are handled by blendNonSeparable; fall back to source.
        case Hue:
        case Saturation:
        case Color:
        case Luminosity:
            return s;
    }
    return s;
}

// Composite a source color over a backdrop using a blend mode and a uniform layer opacity,
// per the W3C source-over-with-blending formula. Inputs and output are straight RGBA in [0,1];
// `opacity` (the layer's [0,1] opacity, already times any per-pixel mask/clip in `source.a`)
// scales the source alpha. The backdrop is returned unchanged when the (scaled) source is
// fully transparent.
[[nodiscard]] inline common::ColorF compositeOver(core::BlendMode mode, common::ColorF backdrop,
                                                  common::ColorF source, float opacity) noexcept {
    const float as = source.a * opacity;
    if (as <= 0.0f) return backdrop;
    const float ab = backdrop.a;
    const float ao = as + ab * (1.0f - as);
    if (ao <= 0.0f) return {0.0f, 0.0f, 0.0f, 0.0f};

    const detail::Rgb cb{backdrop.r, backdrop.g, backdrop.b};
    const detail::Rgb cs{source.r, source.g, source.b};
    detail::Rgb bl;
    if (isSeparable(mode)) {
        bl = {blendChannel(mode, cb.r, cs.r), blendChannel(mode, cb.g, cs.g),
              blendChannel(mode, cb.b, cs.b)};
    } else {
        bl = detail::blendNonSeparable(mode, cb, cs);
    }

    // Each channel: blend is weighted into the source by the backdrop alpha, then source-over.
    const auto mix = [&](float cbx, float csx, float blx) {
        const float src = (1.0f - ab) * csx + ab * blx;
        return (as * src + ab * (1.0f - as) * cbx) / ao;
    };
    return {mix(cb.r, cs.r, bl.r), mix(cb.g, cs.g, bl.g), mix(cb.b, cs.b, bl.b), ao};
}

}  // namespace mosaic::core
