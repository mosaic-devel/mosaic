#include "core/fill.hpp"

#include "core/selection.hpp" // wandColorDistance -- the shared S17 colour metric

#include <algorithm>
#include <cmath>
#include <utility>

namespace mosaic::core {

namespace {

// Half-width of the outer anti-alias band, in the metric's normalised units (mirrors the wand's
// kWandAaBand, docs/research-selection.md §5). A pixel just outside the flood (metric distance d >
// T) earns a soft ramp fading from 0.5 at d==T to 0 at d==T+kAaBand -- a one-pixel feather on the
// region's edge, never bright enough to read as a solid extra pixel.
constexpr double kAaBand = 0.02;

inline common::Color8 pixelAt(const common::Image& img, int x, int y) noexcept {
    const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[i], img.rgba[i + 1], img.rgba[i + 2], img.rgba[i + 3]};
}

inline std::uint8_t coverageToByte(double c) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(c, 0.0, 1.0) * 255.0));
}

// 4-connected scanline (span) flood over the hard tolerance predicate `in`: fills whole row runs at
// once and only seeds the run-starts in the rows above/below, so the worklist tracks spans not
// pixels (the same shape as the wand's flood, docs/research-selection.md §4). `filled` is the
// interior set.
void scanlineFlood(int seedX, int seedY, int w, int h, const std::vector<char>& in,
                   std::vector<char>& filled) {
    const auto idx = [w](int x, int y) { return static_cast<std::size_t>(y) * w + x; };
    if (!in[idx(seedX, seedY)])
        return; // the seed itself is out of tolerance -> nothing floods
    std::vector<std::pair<int, int>> stack;
    stack.emplace_back(seedX, seedY);
    while (!stack.empty()) {
        const auto [px, py] = stack.back();
        stack.pop_back();
        if (filled[idx(px, py)] || !in[idx(px, py)])
            continue; // a redundant seed a prior span already swallowed
        int lx = px;
        while (lx > 0 && !filled[idx(lx - 1, py)] && in[idx(lx - 1, py)])
            --lx;
        int rx = px;
        while (rx < w - 1 && !filled[idx(rx + 1, py)] && in[idx(rx + 1, py)])
            ++rx;
        for (int x = lx; x <= rx; ++x)
            filled[idx(x, py)] = 1;
        for (const int ny : {py - 1, py + 1}) {
            if (ny < 0 || ny >= h)
                continue;
            int x = lx;
            while (x <= rx) {
                if (filled[idx(x, ny)] || !in[idx(x, ny)]) {
                    ++x;
                    continue;
                }
                stack.emplace_back(x, ny); // a new run start; skip to its end so we push it once
                ++x;
                while (x <= rx && !filled[idx(x, ny)] && in[idx(x, ny)])
                    ++x;
            }
        }
    }
}

} // namespace

std::vector<std::uint8_t> bucketFillCoverage(const common::Image& src, int seedX, int seedY,
                                             const FillParams& params) {
    const int w = static_cast<int>(src.width);
    const int h = static_cast<int>(src.height);
    if (src.empty() || seedX < 0 || seedY < 0 || seedX >= w || seedY >= h)
        return {}; // no valid seed -> nothing to fill

    const auto idx = [w](int x, int y) { return static_cast<std::size_t>(y) * w + x; };
    const common::Color8 seed = pixelAt(src, seedX, seedY);
    const double T = std::clamp(params.tolerance, 0.0, 1.0);
    const bool alpha = params.sampleAlpha;
    const auto dist = [&](int x, int y) {
        return wandColorDistance(pixelAt(src, x, y), seed, alpha);
    };

    // The hard tolerance predicate over the whole image (computed once, reused by both branches).
    std::vector<char> in(static_cast<std::size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            in[idx(x, y)] = dist(x, y) <= T ? 1 : 0;

    std::vector<char> filled;
    if (params.contiguous) {
        // Contiguous: 4-connected flood from the seed -- only the connected within-tolerance
        // region.
        filled.assign(static_cast<std::size_t>(w) * h, 0);
        scanlineFlood(seedX, seedY, w, h, in, filled);
    } else {
        // Global ("all matching pixels"): every within-tolerance pixel, connectivity ignored.
        filled = in;
    }

    std::vector<std::uint8_t> cov(static_cast<std::size_t>(w) * h, 0);
    bool any = false;
    for (std::size_t i = 0, n = filled.size(); i < n; ++i)
        if (filled[i]) {
            cov[i] = 255; // solid interior -- the fill lands fully opaque inside the region
            any = true;
        }

    if (params.antialias && any) {
        // Outer boundary ramp: an unfilled pixel 4-adjacent to the region earns a partial coverage
        // from the soft distance ramp, feathering the fill's edge without extending the solid set.
        // Gating on the HARD flood (like the wand, §5) keeps the soft band from bridging into a
        // disconnected same-colour region.
        const int dx4[4] = {-1, 1, 0, 0};
        const int dy4[4] = {0, 0, -1, 1};
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (filled[idx(x, y)])
                    continue;
                bool touches = false;
                for (int k = 0; k < 4; ++k) {
                    const int nx = x + dx4[k], ny = y + dy4[k];
                    if (nx >= 0 && ny >= 0 && nx < w && ny < h && filled[idx(nx, ny)]) {
                        touches = true;
                        break;
                    }
                }
                if (!touches)
                    continue;
                const double d = dist(x, y); // d > T for an unfilled pixel: a sub-0.5 feather
                const double c = d >= T + kAaBand ? 0.0 : (T + kAaBand - d) / (2.0 * kAaBand);
                if (c > 0.0)
                    cov[idx(x, y)] = coverageToByte(c);
            }
    }

    if (!any)
        return {}; // a coverage-free result collapses to "nothing to fill"
    return cov;
}

} // namespace mosaic::core
