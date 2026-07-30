#include "render/region_fill.hpp"

#include "render/blend.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::render {

namespace {
[[nodiscard]] std::uint8_t toByte(float v) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}
} // namespace

common::Image computeFill(const common::Image& region, const std::vector<std::uint8_t>& coverage,
                          common::Color8 fill, core::BlendMode mode, float opacity,
                          bool protectAlpha) {
    common::Image out = region;
    if (region.empty() || opacity <= 0.0f)
        return out;

    const std::size_t n = region.pixelCount();
    // An empty (or mismatched) coverage buffer means "fully covered"; otherwise it must be one
    // coverage byte per pixel. Guarding on the exact size keeps a wrong-sized buffer from reading
    // out of bounds.
    const bool haveCoverage = coverage.size() == n;
    const common::ColorF src{fill.r / 255.0f, fill.g / 255.0f, fill.b / 255.0f, 1.0f};
    const float op = std::clamp(opacity, 0.0f, 1.0f);

    for (std::size_t i = 0; i < n; ++i) {
        const float cov = haveCoverage ? coverage[i] / 255.0f : 1.0f;
        const float strength = cov * op;
        if (strength <= 0.0f)
            continue;

        const std::size_t p = i * 4;
        const common::ColorF bg{region.rgba[p] / 255.0f, region.rgba[p + 1] / 255.0f,
                                region.rgba[p + 2] / 255.0f, region.rgba[p + 3] / 255.0f};
        if (protectAlpha && bg.a <= 0.0f)
            continue; // preserve transparency: never paint into an already-transparent pixel

        common::ColorF res = compositeOver(mode, bg, src, strength);
        if (protectAlpha)
            res.a = bg.a; // keep the original alpha; only the colour changes

        out.rgba[p] = toByte(res.r);
        out.rgba[p + 1] = toByte(res.g);
        out.rgba[p + 2] = toByte(res.b);
        out.rgba[p + 3] = toByte(res.a);
    }
    return out;
}

common::Image computeFillPaint(const common::Image& region,
                               const std::vector<std::uint8_t>& coverage,
                               const core::vec::Paint& paint, long originX, long originY,
                               core::BlendMode mode, float opacity, bool protectAlpha,
                               bool antialias) {
    common::Image out = region;
    if (region.empty() || opacity <= 0.0f)
        return out;

    const std::size_t n = region.pixelCount();
    const bool haveCoverage = coverage.size() == n;
    const float op = std::clamp(opacity, 0.0f, 1.0f);
    // A pattern samples in real layer px (fixed feature size); everything else (gradient) is keyed
    // to the region normalised to [0,1]^2 so it spans the fill area.
    const bool pattern = std::holds_alternative<core::vec::Pattern>(paint);
    const double bw = std::max(1u, region.width);
    const double bh = std::max(1u, region.height);

    for (std::uint32_t y = 0; y < region.height; ++y) {
        for (std::uint32_t x = 0; x < region.width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * region.width + x;
            const float cov = haveCoverage ? coverage[i] / 255.0f : 1.0f;
            const float strength = cov * op;
            if (strength <= 0.0f)
                continue;

            const std::size_t p = i * 4;
            const common::ColorF bg{region.rgba[p] / 255.0f, region.rgba[p + 1] / 255.0f,
                                    region.rgba[p + 2] / 255.0f, region.rgba[p + 3] / 255.0f};
            if (protectAlpha && bg.a <= 0.0f)
                continue; // preserve transparency: never paint into an already-transparent pixel

            const common::Vec2 pt = pattern ? common::Vec2{static_cast<double>(originX) + x + 0.5,
                                                           static_cast<double>(originY) + y + 0.5}
                                            : common::Vec2{(x + 0.5) / bw, (y + 0.5) / bh};
            // Keyed on the DESTINATION pixel so an Edit -> Fill gradient dithers on the same
            // lattice the rasterizer uses (the normalized `pt` above cannot serve as the key).
            const common::ColorF src = core::vec::sampleAt(
                paint, pt, antialias,
                core::vec::SamplePixel{static_cast<std::int32_t>(originX + x),
                                       static_cast<std::int32_t>(originY + y), true});

            common::ColorF res = compositeOver(mode, bg, src, strength);
            if (protectAlpha)
                res.a = bg.a; // keep the original alpha; only the colour changes

            out.rgba[p] = toByte(res.r);
            out.rgba[p + 1] = toByte(res.g);
            out.rgba[p + 2] = toByte(res.b);
            out.rgba[p + 3] = toByte(res.a);
        }
    }
    return out;
}

} // namespace mosaic::render
