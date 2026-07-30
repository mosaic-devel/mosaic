// Smart Resize importance map — see importance_map.hpp for the design and the lineage credits.

#include "core/retarget/importance_map.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace mosaic::core::retarget {
namespace {

// Alpha-weighted Rec.709 luma of one 8-bit RGBA pixel, in [0,1]. Transparent pixels read as 0
// so fully transparent regions carry no structure (they are free to crop away).
inline double luma(const std::uint8_t* px) {
    const double a = px[3] / 255.0;
    return (0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2]) / 255.0 * a;
}

// The working-resolution luminance: each map cell is the exact box average of its source-pixel
// block (integer cell edges from the floor of the proportional split, so every source pixel
// lands in exactly one cell and the result is deterministic).
std::vector<double> downsampledLuma(const common::Image& src, std::uint32_t mapW,
                                    std::uint32_t mapH) {
    std::vector<double> lum(static_cast<std::size_t>(mapW) * mapH, 0.0);
    for (std::uint32_t my = 0; my < mapH; ++my) {
        const std::uint32_t y0 = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(my) * src.height) / mapH);
        std::uint32_t y1 = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(my + 1) * src.height) / mapH);
        y1 = std::max(y1, y0 + 1);
        for (std::uint32_t mx = 0; mx < mapW; ++mx) {
            const std::uint32_t x0 = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(mx) * src.width) / mapW);
            std::uint32_t x1 = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(mx + 1) * src.width) / mapW);
            x1 = std::max(x1, x0 + 1);
            double sum = 0.0;
            for (std::uint32_t y = y0; y < y1; ++y) {
                const std::uint8_t* row =
                    src.rgba.data() + (static_cast<std::size_t>(y) * src.width + x0) * 4;
                for (std::uint32_t x = x0; x < x1; ++x, row += 4)
                    sum += luma(row);
            }
            lum[static_cast<std::size_t>(my) * mapW + mx] =
                sum / (static_cast<double>(y1 - y0) * (x1 - x0));
        }
    }
    return lum;
}

// L1 gradient magnitude of `lum` by central differences (replicated border).
std::vector<double> gradientEnergy(const std::vector<double>& lum, std::uint32_t w,
                                   std::uint32_t h) {
    std::vector<double> e(lum.size(), 0.0);
    auto L = [&](std::uint32_t x, std::uint32_t y) {
        return lum[static_cast<std::size_t>(y) * w + x];
    };
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint32_t yl = y > 0 ? y - 1 : 0;
        const std::uint32_t yr = y + 1 < h ? y + 1 : h - 1;
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t xl = x > 0 ? x - 1 : 0;
            const std::uint32_t xr = x + 1 < w ? x + 1 : w - 1;
            e[static_cast<std::size_t>(y) * w + x] =
                std::abs(L(xr, y) - L(xl, y)) * 0.5 + std::abs(L(x, yr) - L(x, yl)) * 0.5;
        }
    }
    return e;
}

// Box mean of `src` over a (2r+1)-square window (clamped at the borders), via a summed-area
// table so the whole pass is O(w*h) regardless of radius.
std::vector<double> boxMean(const std::vector<double>& src, std::uint32_t w, std::uint32_t h,
                            std::uint32_t r) {
    // sat[(y+1)*(w+1) + (x+1)] = sum of src over [0..x] x [0..y]
    std::vector<double> sat(static_cast<std::size_t>(w + 1) * (h + 1), 0.0);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            sat[static_cast<std::size_t>(y + 1) * (w + 1) + (x + 1)] =
                src[static_cast<std::size_t>(y) * w + x] +
                sat[static_cast<std::size_t>(y) * (w + 1) + (x + 1)] +
                sat[static_cast<std::size_t>(y + 1) * (w + 1) + x] -
                sat[static_cast<std::size_t>(y) * (w + 1) + x];
    std::vector<double> out(src.size(), 0.0);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint32_t y0 = y > r ? y - r : 0;
        const std::uint32_t y1 = std::min(y + r + 1, h);
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t x0 = x > r ? x - r : 0;
            const std::uint32_t x1 = std::min(x + r + 1, w);
            const double sum = sat[static_cast<std::size_t>(y1) * (w + 1) + x1] -
                               sat[static_cast<std::size_t>(y0) * (w + 1) + x1] -
                               sat[static_cast<std::size_t>(y1) * (w + 1) + x0] +
                               sat[static_cast<std::size_t>(y0) * (w + 1) + x0];
            out[static_cast<std::size_t>(y) * w + x] =
                sum / (static_cast<double>(y1 - y0) * (x1 - x0));
        }
    }
    return out;
}

// Scale `v` in place so its maximum is 1 (a no-op for an all-zero field). W only ever compares
// candidate windows, so per-signal max normalization just puts the weights on a common footing.
void normalizeMax(std::vector<double>& v) {
    double m = 0.0;
    for (const double x : v)
        m = std::max(m, x);
    if (m <= 0.0)
        return;
    const double inv = 1.0 / m;
    for (double& x : v)
        x *= inv;
}

} // namespace

ImportanceMap buildImportanceMap(const common::Image& src, const ImportanceOptions& opts) {
    ImportanceMap map;
    if (src.empty() || opts.maxDim < 1)
        return map;
    map.sourceW = src.width;
    map.sourceH = src.height;
    // Working resolution: long side <= maxDim, aspect preserved, never upsampled, >= 1 px.
    const std::uint32_t longSide = std::max(src.width, src.height);
    const double scale = std::min(1.0, static_cast<double>(opts.maxDim) / longSide);
    map.width = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(std::lround(src.width * scale)));
    map.height = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(std::lround(src.height * scale)));

    const std::vector<double> lum = downsampledLuma(src, map.width, map.height);
    std::vector<double> grad = gradientEnergy(lum, map.width, map.height);
    // Edge-density window: ~1/12 of the map's short side — big enough to spread importance over
    // a textured object's interior, small enough to leave genuinely flat regions cheap to crop.
    const std::uint32_t r =
        std::max<std::uint32_t>(1, std::min(map.width, map.height) / 12);
    std::vector<double> density = boxMean(grad, map.width, map.height, r);
    normalizeMax(grad);
    normalizeMax(density);

    map.w.resize(lum.size());
    const double sigma = 0.35; // of the normalized half-diagonal: ~0.13 at the corners — mild
    const double invTwoSigmaSq = 1.0 / (2.0 * sigma * sigma);
    for (std::uint32_t y = 0; y < map.height; ++y) {
        const double py = (y + 0.5) / map.height - 0.5;
        for (std::uint32_t x = 0; x < map.width; ++x) {
            const double px = (x + 0.5) / map.width - 0.5;
            const double prior = std::exp(-(px * px + py * py) * invTwoSigmaSq);
            const std::size_t i = static_cast<std::size_t>(y) * map.width + x;
            map.w[i] = static_cast<float>(opts.gradientWeight * grad[i] +
                                          opts.edgeDensityWeight * density[i] +
                                          opts.centerPriorWeight * prior);
        }
    }
    return map;
}

} // namespace mosaic::core::retarget
