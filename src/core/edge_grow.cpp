#include "core/edge_grow.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

// See edge_grow.hpp for the L1 design, the lineage, and the load-bearing design invariants
// (I1/I2/I3) this file is built to. The pipeline: weighted multi-channel Sobel
// gradient -> per-pixel step cost 1 + lambda*g -> Toivanen-style iterated raster-scan geodesic
// distance from the stroke seeds -> linear ramp across the `reach` level into 8-bit coverage.
namespace mosaic::core {
namespace {

// The same BT.601 luma weights + colour-vs-alpha split as the wand metric (selection.cpp): the
// gradient is measured in the SAME weighted colour space the wand floods, so the two tools agree
// about which boundaries are strong. (I2: the weight is the image's own edge strength -- the
// gradient never references a seed or "center" colour.)
constexpr double kWr = 0.30, kWg = 0.59, kWb = 0.11;
constexpr double kColorW = 0.80, kAlphaW = 0.20;

// Edge-stop strength: edgeStop in [0,1] maps linearly onto lambda in [0, kEdgeLambdaMax]; the
// per-pixel step cost is 1 + lambda*g with g the normalised gradient in [0,1]. At the default
// (0.5 -> 500) a crisp ~10%-of-range edge (g ~ 0.1 across its ~2 px width) adds ~100 to the path
// cost -- beyond the default reach of 64, so it stops the grow -- while flat colour costs 1/px, so
// `reach` reads as pixels there. A tunable constant, not a magic threshold: the ramp/cost math is
// unit-tested; the *feel* is owed a visual pass (exactly like the wand's kWandAaBand).
constexpr double kEdgeLambdaMax = 1000.0;

// Half-width of the AA band, in cost units (the wand's kWandAaBand analogue). Over
// flat colour (cost 1/px) this is a ~1.5-px soft edge, matching the standing plain-isotropic-ramp
// AA convention (an invariant, not a default); where an edge arrested the grow the same band
// compresses to a sub-pixel spatial width, so edge-stopped boundaries stay crisp while flat-field
// boundaries anti-alias.
constexpr double kRampBand = 0.75;

// Raster-scan sweep cap. Each forward+backward double-sweep settles every path that only bends
// with the sweep direction; convex fronts converge in one, real strokes in a handful. Only spiral
// topologies need more -- the cap is a backstop against pathological images, not a tuning knob
// (the sweep loop exits as soon as a double-sweep changes nothing).
constexpr int kMaxDoubleSweeps = 64;

inline std::uint8_t coverageToByte(double c) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(c, 0.0, 1.0) * 255.0));
}

// The soft coverage a pixel at geodesic distance `d` earns for reach `T`: 1 solidly inside, a
// linear ramp through 0.5 at d==T, 0 solidly outside. Monotone in d, so the >=128 set the ants
// draw is exactly {d <= T} -- connected by construction (every relaxed pixel chains to a seed
// through neighbours of strictly smaller d).
inline double rampCoverage(double d, double T) noexcept {
    if (d <= T - kRampBand)
        return 1.0;
    if (d >= T + kRampBand)
        return 0.0;
    return (T + kRampBand - d) / (2.0 * kRampBand);
}

// Weighted multi-channel Sobel gradient magnitude over `src`, normalised to [0,1], computed for
// the ROI window [x0,y0)..(x1,y1). Sampling clamps to the IMAGE edge (not the ROI edge), so ROI-
// boundary pixels see their true neighbours. Per channel, |gx|,|gy| <= 1 after the /(4*255)
// normalisation, so (gx^2+gy^2)/2 is in [0,1] per channel and the weighted sum stays in [0,1].
std::vector<float> gradientMap(const common::Image& src, int x0, int y0, int x1, int y1) {
    const int w = static_cast<int>(src.width);
    const int h = static_cast<int>(src.height);
    const int rw = x1 - x0;
    std::vector<float> g(static_cast<std::size_t>(rw) * (y1 - y0));
    const auto sample = [&](int x, int y) {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        return src.rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
    };
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            // 3x3 neighbourhood, channel pointers.
            const std::uint8_t* p[3][3];
            for (int j = 0; j < 3; ++j)
                for (int i = 0; i < 3; ++i)
                    p[j][i] = sample(x + i - 1, y + j - 1);
            double energy = 0.0; // sum of w_c * (gx^2 + gy^2) / 2 over channels
            for (int c = 0; c < 4; ++c) {
                const auto v = [&](int i, int j) { return static_cast<double>(p[j][i][c]); };
                const double gx = (v(2, 0) + 2.0 * v(2, 1) + v(2, 2)) //
                                  - (v(0, 0) + 2.0 * v(0, 1) + v(0, 2));
                const double gy = (v(0, 2) + 2.0 * v(1, 2) + v(2, 2)) //
                                  - (v(0, 0) + 2.0 * v(1, 0) + v(2, 0));
                const double nx = gx / (4.0 * 255.0), ny = gy / (4.0 * 255.0);
                const double wc = c == 0   ? kColorW * kWr
                                  : c == 1 ? kColorW * kWg
                                  : c == 2 ? kColorW * kWb
                                           : kAlphaW;
                energy += wc * 0.5 * (nx * nx + ny * ny);
            }
            g[static_cast<std::size_t>(y - y0) * rw + (x - x0)] =
                static_cast<float>(std::sqrt(energy));
        }
    }
    return g;
}

} // namespace

Selection edgeGrowSelection(const common::Image& src, const Selection& seeds,
                            const EdgeGrowParams& params) {
    if (src.empty() || seeds.isEmpty() || seeds.width() != src.width ||
        seeds.height() != src.height)
        return {}; // no valid seed stroke -> "no selection"

    const double reach = std::max(0.0, params.reach);
    const double cutoff = reach + kRampBand;

    // ROI: the step cost is >= 1/px, so the geodesic distance dominates the chamfer distance,
    // which dominates the Euclidean one -- nothing beyond `cutoff` px of the seed bounds can earn
    // coverage. Solving inside the inflated seed box only is exact, not an approximation.
    const auto sb = seeds.bounds();
    if (!sb)
        return {};
    const int w = static_cast<int>(src.width);
    const int h = static_cast<int>(src.height);
    const int pad = static_cast<int>(std::ceil(cutoff)) + 1;
    const int x0 = std::max(0, static_cast<int>(sb->x) - pad);
    const int y0 = std::max(0, static_cast<int>(sb->y) - pad);
    const int x1 = std::min(w, static_cast<int>(sb->x + sb->w) + pad);
    const int y1 = std::min(h, static_cast<int>(sb->y + sb->h) + pad);
    const int rw = x1 - x0, rh = y1 - y0;

    // Per-pixel step cost 1 + lambda*g (I1: a boundary term only -- no foreground cost, no
    // background cost, no bias cost, no model of the stroke's colours anywhere in this file).
    const double lambda = std::clamp(params.edgeStop, 0.0, 1.0) * kEdgeLambdaMax;
    const std::vector<float> grad = gradientMap(src, x0, y0, x1, y1);
    std::vector<float> cost(grad.size());
    for (std::size_t i = 0; i < grad.size(); ++i)
        cost[i] = static_cast<float>(1.0 + lambda * grad[i]);

    // Seed the distance field: 0 on the stroke's full-coverage core, +inf elsewhere. The stroke's
    // own sub-threshold AA fringe is not a seed, but it lies within a pixel of one, so it lands
    // solidly inside the result for any practical reach.
    constexpr float kInf = std::numeric_limits<float>::infinity();
    std::vector<float> dist(static_cast<std::size_t>(rw) * rh, kInf);
    const auto& seedData = seeds.data();
    bool anySeed = false;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if (seedData[static_cast<std::size_t>(y) * w + x] >= kAntsCoverageThreshold) {
                dist[static_cast<std::size_t>(y - y0) * rw + (x - x0)] = 0.0f;
                anySeed = true;
            }
    if (!anySeed)
        return {};

    // Toivanen-style iterated raster-scan geodesic distance (8-neighbour chamfer): forward then
    // backward sweeps relax d(p) = min(d(p), d(q) + len * (cost(p)+cost(q))/2) until a double-
    // sweep changes nothing. Sources already at/past `cutoff` are skipped -- they cannot improve
    // any pixel that could still earn coverage, so the pruning is exact.
    constexpr float kSqrt2 = 1.41421356f;
    const float cutoffF = static_cast<float>(cutoff);
    const auto at = [&](int x, int y) -> float& {
        return dist[static_cast<std::size_t>(y) * rw + x];
    };
    const auto costAt = [&](int x, int y) {
        return cost[static_cast<std::size_t>(y) * rw + x];
    };
    bool changed = true;
    for (int sweep = 0; changed && sweep < kMaxDoubleSweeps; ++sweep) {
        changed = false;
        const auto relax = [&](int x, int y, int qx, int qy, float len) {
            if (qx < 0 || qy < 0 || qx >= rw || qy >= rh)
                return;
            const float dq = at(qx, qy);
            if (dq >= cutoffF)
                return;
            const float nd = dq + len * 0.5f * (costAt(x, y) + costAt(qx, qy));
            if (nd < at(x, y)) {
                at(x, y) = nd;
                changed = true;
            }
        };
        for (int y = 0; y < rh; ++y) // forward: the four causal neighbours
            for (int x = 0; x < rw; ++x) {
                relax(x, y, x - 1, y, 1.0f);
                relax(x, y, x - 1, y - 1, kSqrt2);
                relax(x, y, x, y - 1, 1.0f);
                relax(x, y, x + 1, y - 1, kSqrt2);
            }
        for (int y = rh - 1; y >= 0; --y) // backward: the mirrored four
            for (int x = rw - 1; x >= 0; --x) {
                relax(x, y, x + 1, y, 1.0f);
                relax(x, y, x + 1, y + 1, kSqrt2);
                relax(x, y, x, y + 1, 1.0f);
                relax(x, y, x - 1, y + 1, kSqrt2);
            }
    }

    // Ramp the distance field into coverage (a plain isotropic ramp).
    Selection out(src.width, src.height);
    auto& mask = out.data();
    for (int y = 0; y < rh; ++y)
        for (int x = 0; x < rw; ++x) {
            const float d = at(x, y);
            if (d >= cutoffF)
                continue;
            const double c = rampCoverage(d, reach);
            if (c > 0.0)
                mask[static_cast<std::size_t>(y + y0) * w + (x + x0)] = coverageToByte(c);
        }
    if (!out.anySelected())
        return {}; // a coverage-free result collapses to "no selection"
    return out;
}

} // namespace mosaic::core
