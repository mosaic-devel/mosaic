#include "core/vector/raster.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

#include "common/dither.hpp"  // ditherTPDF: the white-noise kind (shared with the sky renderer)
#include "core/vector/flatten.hpp"
#include "core/vector/hit.hpp"
#include "core/vector/pattern.hpp"
#include "core/vector/stroke.hpp"

namespace mosaic::core::vec {
namespace {

using common::Affine2D;
using common::ColorF;
using common::Vec2;

ColorF lerp(const ColorF& a, const ColorF& b, double f) {
    const float t = static_cast<float>(f);
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
            a.a + (b.a - a.a) * t};
}

// Map a gradient parameter through its spread mode into the sampling range.
double applySpread(double t, SpreadMethod sp) {
    switch (sp) {
        case SpreadMethod::Pad: return std::clamp(t, 0.0, 1.0);
        case SpreadMethod::Repeat: return t - std::floor(t);
        case SpreadMethod::Reflect: {
            const double m = std::fmod(std::abs(t), 2.0);
            return m > 1.0 ? 2.0 - m : m;
        }
    }
    return std::clamp(t, 0.0, 1.0);
}

// Bias a segment fraction `f` in [0,1] by a colour midpoint `m` in (0,1) -- the blend curve (S22):
// the two endpoint colours mix 50/50 at f == m (Photoshop's diamond / CSS `<color-hint>`). m == 0.5
// leaves f untouched (a straight linear blend), so default gradients render byte-identically.
double applyMidpoint(double f, double m) {
    if (m <= 0.0) return f >= 1.0 ? 1.0 : (f <= 0.0 ? 0.0 : 1.0);  // degenerate: hard step at start
    if (m >= 1.0) return f <= 0.0 ? 0.0 : (f >= 1.0 ? 1.0 : 0.0);  // degenerate: hard step at end
    if (std::abs(m - 0.5) < 1e-9) return f;                        // linear: exact identity
    // blend = f ^ (log 0.5 / log m): 0 at f=0, 1 at f=1, and exactly 0.5 at f=m.
    return std::pow(std::clamp(f, 0.0, 1.0), std::log(0.5) / std::log(m));
}

ColorF sampleStops(const std::vector<GradientStop>& stops, double t) {
    if (stops.empty()) return {0, 0, 0, 0};
    if (t <= stops.front().offset) return stops.front().color;
    if (t >= stops.back().offset) return stops.back().color;
    for (std::size_t i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].offset) {
            const double span = stops[i].offset - stops[i - 1].offset;
            const double f = span > 1e-9 ? (t - stops[i - 1].offset) / span : 0.0;
            return lerp(stops[i - 1].color, stops[i].color, applyMidpoint(f, stops[i - 1].midpoint));
        }
    }
    return stops.back().color;
}

// ---- Gradient dithering (S22; docs/gradient-tool.md §7) -----------------------------------------
// Technique lineage, all published and decades old: Bayer 1973 (the ordered matrix); Holladay's
// screen-cell tiling, 1978; PostScript halftone screens, 1985; Ulichney, "Digital Halftoning"
// (MIT Press, 1987) and "The void-and-cluster method for dither array generation" (Proc. SPIE
// 1913, 1993). NO error diffusion here -- it is not a point function (paint.hpp / docs §7).

// Bayer's recursive ordered matrix at 8x8, M(2n) = [[4M, 4M+2], [4M+3, 4M+1]] from M(1) = [[0]].
// Written out as the published table so it can be read straight against the 1973 paper.
constexpr std::array<std::uint8_t, 64> kBayer8{{
    0,  32, 8,  40, 2,  34, 10, 42,  //
    48, 16, 56, 24, 50, 18, 58, 26,  //
    12, 44, 4,  36, 14, 46, 6,  38,  //
    60, 28, 52, 20, 62, 30, 54, 22,  //
    3,  35, 11, 43, 1,  33, 9,  41,  //
    51, 19, 59, 27, 49, 17, 57, 25,  //
    15, 47, 7,  39, 13, 45, 5,  37,  //
    63, 31, 55, 23, 61, 29, 53, 21,  //
}};

constexpr int kBlueN = 64;  // blue-noise tile edge; 4096 ranks, tiled toroidally over the canvas

// Ulichney's void-and-cluster (1993): a rank for every cell of a toroidal kBlueN^2 grid such that
// the binary pattern at ANY threshold is a well-spaced (blue-noise) point set. Built once, lazily,
// and cached -- it is a few tens of ms of pure arithmetic, and only the BlueNoise kind pays it.
// Thread-safe by C++11 magic statics.
const std::array<std::uint16_t, kBlueN * kBlueN>& blueNoiseRanks() {
    static const std::array<std::uint16_t, kBlueN * kBlueN> ranks = [] {
        constexpr int N = kBlueN;
        constexpr int kCells = N * N;
        constexpr int R = 4;      // kernel radius, ~2.7 sigma (the tail below 0.03 contributes none)
        constexpr int K = 2 * R + 1;
        std::array<double, K * K> kern{};
        for (int dy = -R; dy <= R; ++dy)
            for (int dx = -R; dx <= R; ++dx)
                kern[static_cast<std::size_t>((dy + R) * K + (dx + R))] =
                    std::exp(-static_cast<double>(dx * dx + dy * dy) / (2.0 * 1.5 * 1.5));

        std::vector<std::uint8_t> ones(kCells, 0);
        std::vector<double> filt(kCells, 0.0);  // the ones' Gaussian-filtered density
        const auto stamp = [&](int idx, double sign) {
            const int cx = idx % N, cy = idx / N;
            for (int dy = -R; dy <= R; ++dy) {
                const int y = ((cy + dy) % N + N) % N;
                for (int dx = -R; dx <= R; ++dx) {
                    const int x = ((cx + dx) % N + N) % N;
                    filt[static_cast<std::size_t>(y * N + x)] +=
                        sign * kern[static_cast<std::size_t>((dy + R) * K + (dx + R))];
                }
            }
        };
        const auto place = [&](int idx) {
            ones[static_cast<std::size_t>(idx)] = 1;
            stamp(idx, 1.0);
        };
        const auto clear = [&](int idx) {
            ones[static_cast<std::size_t>(idx)] = 0;
            stamp(idx, -1.0);
        };
        // Tightest CLUSTER = the densest occupied cell; largest VOID = the emptiest free one.
        const auto tightestCluster = [&] {
            int best = -1;
            double bv = 0.0;
            for (int i = 0; i < kCells; ++i) {
                const auto u = static_cast<std::size_t>(i);
                if (ones[u] != 0 && (best < 0 || filt[u] > bv)) {
                    best = i;
                    bv = filt[u];
                }
            }
            return best;
        };
        const auto largestVoid = [&] {
            int best = -1;
            double bv = 0.0;
            for (int i = 0; i < kCells; ++i) {
                const auto u = static_cast<std::size_t>(i);
                if (ones[u] == 0 && (best < 0 || filt[u] < bv)) {
                    best = i;
                    bv = filt[u];
                }
            }
            return best;
        };

        // The prototype binary pattern: a deterministic scatter of ~1/10 of the cells (splitmix64,
        // fixed seed -- the tile must be reproducible), relaxed by moving the tightest cluster into
        // the largest void until the two coincide.
        constexpr int kSeeded = kCells / 10;
        std::uint64_t seed = 0x9e3779b97f4a7c15ULL;
        const auto nextRandom = [&seed] {
            std::uint64_t v = (seed += 0x9e3779b97f4a7c15ULL);
            v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
            v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
            return v ^ (v >> 31);
        };
        for (int placed = 0; placed < kSeeded;) {
            const int idx = static_cast<int>(nextRandom() % static_cast<std::uint64_t>(kCells));
            if (ones[static_cast<std::size_t>(idx)] == 0) {
                place(idx);
                ++placed;
            }
        }
        for (int guard = 0; guard < 2 * kSeeded; ++guard) {
            const int c = tightestCluster();
            clear(c);
            const int v = largestVoid();
            place(v);
            if (v == c) break;  // stable: the tightest cluster IS the largest void
        }

        std::array<std::uint16_t, kCells> out{};
        const std::vector<std::uint8_t> protoOnes = ones;
        const std::vector<double> protoFilt = filt;
        // Phase 1 -- rank the prototype's own points, densest first, counting DOWN to 0.
        for (int rank = kSeeded - 1; rank >= 0; --rank) {
            const int c = tightestCluster();
            clear(c);
            out[static_cast<std::size_t>(c)] = static_cast<std::uint16_t>(rank);
        }
        // Phases 2+3 -- from the prototype again, fill the largest void, over and over. (The
        // paper's phase 3 looks for "the tightest cluster of the MINORITY", which past the halfway
        // point is the zeros; on a torus with a symmetric kernel that is the same cell as the
        // largest void of the ones, because the kernel sums to the same constant over every cell.
        // So the two phases are one loop here.)
        ones = protoOnes;
        filt = protoFilt;
        for (int rank = kSeeded; rank < kCells; ++rank) {
            const int v = largestVoid();
            place(v);
            out[static_cast<std::size_t>(v)] = static_cast<std::uint16_t>(rank);
        }
        return out;
    }();
    return ranks;
}

// Fold a signed pixel coordinate into [0, n) -- the dither tiles across the whole plane, including
// the negative half (a layer can sit at a negative offset).
int wrapCoord(std::int32_t v, int n) {
    const int m = static_cast<int>(v % n);
    return m < 0 ? m + n : m;
}

double gradientParam(const Gradient& g, Vec2 gs) {
    switch (g.type) {
        case GradientType::Linear: return gs.x;  // projection onto the gradient x-axis
        case GradientType::Radial: return std::sqrt(gs.x * gs.x + gs.y * gs.y);
        case GradientType::Conic: {
            double a = std::atan2(gs.y, gs.x) / (2.0 * M_PI);
            return a < 0.0 ? a + 1.0 : a;
        }
    }
    return 0.0;
}

// Colour of a paint at a layer-local point (constant for solid; evaluated for gradients/patterns).
ColorF paintColorAt(const Paint& paint, Vec2 localPt, bool antialias = true,
                    SamplePixel pixel = {}) {
    if (const auto* s = std::get_if<SolidPaint>(&paint)) return s->color;
    if (const auto* g = std::get_if<Gradient>(&paint)) {
        const std::optional<Affine2D> inv = g->transform.inverse();
        const Vec2 gs = inv ? inv->apply(localPt) : localPt;  // gradient unit-space coordinate
        ColorF c = sampleStops(g->stops, applySpread(gradientParam(*g, gs), g->spread));
        if (g->dither != DitherKind::None && pixel.valid) {
            // Perturb by a fraction of ONE 8-bit step, BEFORE anything downstream quantises: that
            // is what turns a band edge into a dithered boundary whatever the final bit depth is.
            // ALL FOUR channels move -- the tool's default ramp fades in ALPHA, so alpha bands too
            // -- and the clamp means a saturated (0 or 1) channel is left exactly alone.
            constexpr double kLsb = 1.0 / 255.0;
            const auto shift = [&](float v, int ch) {
                // ⚠ A SATURATED CHANNEL IS LEFT EXACTLY ALONE, and the clamp alone does NOT do
                // that: at v == 1 an outward offset clamps, but an inward one lands below 1 and
                // speckles the ramp's opaque end (stray translucent pixels against the backdrop --
                // most visible in ALPHA, which is why the ends of the tool's fading default ramp
                // showed it). An exactly-representable endpoint carries no quantization error for
                // dither to spread, so there is nothing to gain by moving it.
                if (!(v > 0.0f && v < 1.0f))
                    return v;
                return static_cast<float>(
                    std::clamp(static_cast<double>(v) +
                                   ditherOffsetLsb(g->dither, pixel.x, pixel.y, ch) * kLsb,
                               0.0, 1.0));
            };
            c = ColorF{shift(c.r, 0), shift(c.g, 1), shift(c.b, 2), shift(c.a, 3)};
        }
        return c;
    }
    if (const auto* pat = std::get_if<Pattern>(&paint))
        return samplePattern(*pat, localPt, antialias);  // tiles in the same units the point is given
    return {0, 0, 0, 0};  // NoPaint
}

// Composite a (coverage x paint) layer source-over onto a straight-alpha float buffer. Only the
// coverage buffer's sub-rect is visited (coverage is zero elsewhere) -- this is the bbox bound.
void paintCoverageOver(common::ImageF& dst, const CoverageBuffer& cov, const Paint& paint,
                       const std::optional<Affine2D>& invToPixel, bool antialias = true) {
    if (std::holds_alternative<NoPaint>(paint)) return;
    const bool solid = std::holds_alternative<SolidPaint>(paint);
    const ColorF solidColor = solid ? std::get<SolidPaint>(paint).color : ColorF{};
    for (std::uint32_t y = cov.oy; y < cov.oy + cov.height; ++y)
        for (std::uint32_t x = cov.ox; x < cov.ox + cov.width; ++x) {
            const float c = std::min(1.0f, cov.at(x, y));
            if (c <= 0.0f) continue;
            // (x,y) is the TARGET pixel, which is exactly the dither key a gradient wants: stable
            // under tiling / threading, and independent of the layer's own transform.
            const ColorF s =
                solid ? solidColor
                      : paintColorAt(paint,
                                     invToPixel ? invToPixel->apply({x + 0.5, y + 0.5})
                                                : Vec2{static_cast<double>(x),
                                                       static_cast<double>(y)},
                                     antialias,
                                     SamplePixel{static_cast<std::int32_t>(x),
                                                 static_cast<std::int32_t>(y), true});
            const float sa = s.a * c;
            if (sa <= 0.0f) continue;
            const ColorF d = dst.at(x, y);
            const float outA = sa + d.a * (1.0f - sa);
            if (outA <= 1e-8f) {
                dst.set(x, y, {0, 0, 0, 0});
                continue;
            }
            const float k = 1.0f / outA;  // straight-alpha source-over
            dst.set(x, y, {(s.r * sa + d.r * d.a * (1.0f - sa)) * k,
                           (s.g * sa + d.g * d.a * (1.0f - sa)) * k,
                           (s.b * sa + d.b * d.a * (1.0f - sa)) * k, outA});
        }
}

// Accumulate an inside span [xa,xb) into one sub-scanline's coverage row, with analytic partial
// coverage at the fractional endpoints, scaled by the per-subsample weight. `row` points at column
// x0 of the sub-rect; the span is clamped to [x0,x1) and indexed relative to x0.
void addSpan(float* row, std::uint32_t x0, std::uint32_t x1, double xa, double xb, float weight) {
    xa = std::clamp(xa, static_cast<double>(x0), static_cast<double>(x1));
    xb = std::clamp(xb, static_cast<double>(x0), static_cast<double>(x1));
    if (xb <= xa) return;
    const int ixa = static_cast<int>(std::floor(xa));
    const int ixb = static_cast<int>(std::floor(xb));
    const int hi = static_cast<int>(x1);
    const int base = static_cast<int>(x0);
    if (ixa == ixb) {
        if (ixa < hi) row[ixa - base] += static_cast<float>(xb - xa) * weight;
        return;
    }
    if (ixa < hi) row[ixa - base] += static_cast<float>((ixa + 1) - xa) * weight;
    for (int x = ixa + 1; x < ixb && x < hi; ++x) row[x - base] += weight;
    if (ixb < hi) row[ixb - base] += static_cast<float>(xb - ixb) * weight;
}

}  // namespace

// Public gradient/paint sampler (paint.hpp): forwards to the file-local paintColorAt so the vector
// rasteriser and the layer-effects renderer share one gradient evaluation -- dithering included.
ColorF sampleAt(const Paint& paint, common::Vec2 localPt, bool antialias, SamplePixel pixel) {
    return paintColorAt(paint, localPt, antialias, pixel);
}

// The dither threshold/offset for one destination pixel + channel (paint.hpp). Pure and tiling.
double ditherOffsetLsb(DitherKind kind, std::int32_t px, std::int32_t py, int ch) {
    switch (kind) {
        case DitherKind::None:
            return 0.0;
        case DitherKind::Ordered: {
            // The classic centred ordered threshold: (M + 0.5) / n^2 - 0.5, symmetric about zero,
            // so the ramp's mean is untouched and only its band edges move.
            const std::size_t i =
                static_cast<std::size_t>(wrapCoord(py, 8) * 8 + wrapCoord(px, 8));
            return (static_cast<double>(kBayer8[i]) + 0.5) / 64.0 - 0.5;
        }
        case DitherKind::BlueNoise: {
            const std::size_t i = static_cast<std::size_t>(wrapCoord(py, kBlueN) * kBlueN +
                                                           wrapCoord(px, kBlueN));
            return (static_cast<double>(blueNoiseRanks()[i]) + 0.5) /
                       static_cast<double>(kBlueN * kBlueN) -
                   0.5;
        }
        case DitherKind::Noise:
            return common::ditherTPDF(static_cast<std::uint32_t>(px),
                                      static_cast<std::uint32_t>(py), ch);
    }
    return 0.0;
}

std::optional<PixelBounds> pixelBoundsOf(const Contours& contours, std::uint32_t W, std::uint32_t H,
                                         std::uint32_t pad) {
    if (W == 0 || H == 0) return std::nullopt;
    bool any = false;
    double minx = 0, miny = 0, maxx = 0, maxy = 0;
    for (const auto& c : contours)
        for (const Vec2& p : c.points) {
            if (!any) {
                minx = maxx = p.x;
                miny = maxy = p.y;
                any = true;
            } else {
                minx = std::min(minx, p.x);
                miny = std::min(miny, p.y);
                maxx = std::max(maxx, p.x);
                maxy = std::max(maxy, p.y);
            }
        }
    if (!any) return std::nullopt;
    const double padd = static_cast<double>(pad);
    const double lox = std::floor(minx) - padd, loy = std::floor(miny) - padd;
    const double hix = std::ceil(maxx) + padd, hiy = std::ceil(maxy) + padd;
    PixelBounds b;
    b.x0 = static_cast<std::uint32_t>(std::clamp(lox, 0.0, static_cast<double>(W)));
    b.y0 = static_cast<std::uint32_t>(std::clamp(loy, 0.0, static_cast<double>(H)));
    b.x1 = static_cast<std::uint32_t>(std::clamp(hix, 0.0, static_cast<double>(W)));
    b.y1 = static_cast<std::uint32_t>(std::clamp(hiy, 0.0, static_cast<double>(H)));
    if (b.empty()) return std::nullopt;
    return b;
}

CoverageBuffer rasterizeCoverage(const Contours& contours, std::uint32_t W, std::uint32_t H,
                                 FillRule rule, int subsamples, std::optional<PixelBounds> clip) {
    const PixelBounds box = clip.value_or(PixelBounds{0, 0, W, H});
    CoverageBuffer cov;
    cov.ox = box.x0;
    cov.oy = box.y0;
    cov.width = box.width();
    cov.height = box.height();
    cov.a.assign(static_cast<std::size_t>(cov.width) * cov.height, 0.0f);
    if (box.empty()) return cov;

    // Non-horizontal edges; every contour closes for fill. {ax, ay, bx, by}.
    std::vector<std::array<double, 4>> edges;
    for (const auto& c : contours) {
        const auto& pts = c.points;
        const std::size_t n = pts.size();
        if (n < 2) continue;
        for (std::size_t i = 0; i < n; ++i) {
            const Vec2 a = pts[i];
            const Vec2 b = pts[(i + 1) % n];
            if (a.y != b.y) edges.push_back({a.x, a.y, b.x, b.y});
        }
    }
    if (edges.empty()) return cov;

    const int N = std::max(1, subsamples);
    const float weight = 1.0f / static_cast<float>(N);
    std::vector<std::pair<double, int>> xs;  // (x crossing, winding direction)
    for (std::uint32_t y = box.y0; y < box.y1; ++y) {
        float* row = &cov.a[static_cast<std::size_t>(y - box.y0) * cov.width];
        for (int s = 0; s < N; ++s) {
            const double sy = static_cast<double>(y) + (static_cast<double>(s) + 0.5) / N;
            xs.clear();
            for (const auto& e : edges) {
                const double ay = e[1], by = e[3];
                if (sy < std::min(ay, by) || sy >= std::max(ay, by)) continue;  // half-open
                const double t = (sy - ay) / (by - ay);
                xs.push_back({e[0] + t * (e[2] - e[0]), by > ay ? 1 : -1});
            }
            if (xs.size() < 2) continue;
            std::sort(xs.begin(), xs.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            int w = 0;
            for (std::size_t k = 0; k + 1 < xs.size(); ++k) {
                w += (rule == FillRule::NonZero) ? xs[k].second : 1;
                const bool inside = (rule == FillRule::NonZero) ? (w != 0) : (w & 1);
                if (inside) addSpan(row, box.x0, box.x1, xs[k].first, xs[k + 1].first, weight);
            }
        }
    }
    return cov;
}

common::ImageF rasterizeFillF(const Object& obj, std::uint32_t W, std::uint32_t H,
                              const Affine2D& toPixel, double tolerancePx) {
    common::ImageF img(W, H);  // transparent (zeros)
    if (std::holds_alternative<NoPaint>(obj.fill) || W == 0 || H == 0) return img;

    // Flatten in pixel-tolerance, then map the (layer-local) contour points into pixel space.
    Contours cs = flatten(obj.geometry, tolerancePx, toPixel);
    for (auto& c : cs)
        for (Vec2& p : c.points) p = toPixel.apply(p);

    const std::optional<PixelBounds> bbox = pixelBoundsOf(cs, W, H);
    if (!bbox) return img;  // shape lands entirely off-canvas
    const CoverageBuffer cov = rasterizeCoverage(cs, W, H, fillRuleOf(obj.geometry), 4, bbox);

    const bool solid = std::holds_alternative<SolidPaint>(obj.fill);
    const ColorF solidColor = solid ? std::get<SolidPaint>(obj.fill).color : ColorF{};
    const std::optional<Affine2D> invToPixel = toPixel.inverse();

    for (std::uint32_t y = cov.oy; y < cov.oy + cov.height; ++y)
        for (std::uint32_t x = cov.ox; x < cov.ox + cov.width; ++x) {
            const float c = std::min(1.0f, cov.at(x, y));
            if (c <= 0.0f) continue;
            ColorF col = solidColor;
            if (!solid) {
                const Vec2 localP = invToPixel ? invToPixel->apply({x + 0.5, y + 0.5})
                                               : Vec2{static_cast<double>(x), static_cast<double>(y)};
                col = paintColorAt(obj.fill, localP);
            }
            img.set(x, y, {col.r, col.g, col.b, col.a * c});  // straight alpha, coverage-scaled
        }
    return img;
}

common::Image rasterizeFill(const Object& obj, std::uint32_t W, std::uint32_t H,
                            const Affine2D& toPixel, double tolerancePx) {
    return common::toImage8(rasterizeFillF(obj, W, H, toPixel, tolerancePx));
}

common::ImageF rasterizeObjectF(const Object& obj, std::uint32_t W, std::uint32_t H,
                                const Affine2D& toPixel, double tolerancePx, bool antialias) {
    common::ImageF img(W, H);
    if (W == 0 || H == 0) return img;
    const std::optional<Affine2D> invToPixel = toPixel.inverse();

    // Snap coverage to 0/1 (crisp/aliased edges) when antialiasing is off (Nearest filter).
    const auto harden = [&](CoverageBuffer& cov) {
        if (antialias) return;
        for (float& a : cov.a) a = a >= 0.5f ? 1.0f : 0.0f;
    };

    // Line paint modes (S26-b §7.5): a LineShape painted Hollow / Outlined renders the weight-thick
    // line REGION (the centreline stroked at the Object's stroke width) as an area, with a thin
    // contrasting border. The border is a coverage RING -- the thick region at a larger width minus
    // it at a smaller one -- so it never shows the internal seams that stroking the region pieces
    // would (the quad/cap joins). Solid (and every other shape) falls through to fill+stroke below.
    if (const auto* ps = std::get_if<ParametricShape>(&obj.geometry)) {
        if (const auto* line = std::get_if<LineShape>(ps);
            line != nullptr && line->paint != LineShape::Paint::Solid) {
            const Contours centre = flatten(obj.geometry, tolerancePx, toPixel);
            const double weight = std::max(0.0, obj.stroke.width);
            const double bw = std::max(0.0, line->borderWidth);
            const bool outlined = line->paint == LineShape::Paint::Outlined;
            // Outlined: body fills the weight region, the border ring is added OUTSIDE it.
            // Hollow: a border ring straddling the weight edge, the inside left empty.
            const double outerW = outlined ? weight + 2.0 * bw : weight + bw;
            const double innerW = outlined ? weight : std::max(0.0, weight - bw);
            const auto regionCov = [&](double width,
                                       const std::optional<PixelBounds>& bbox) -> CoverageBuffer {
                Stroke s;
                s.width = width;
                s.cap = obj.stroke.cap;
                s.dashArray = obj.stroke.dashArray; // dashes/dots carry through to Hollow/Outlined
                s.dashOffset = obj.stroke.dashOffset;
                Contours o = strokeOutline(centre, s, tolerancePx, toPixel);
                for (auto& c : o)
                    for (Vec2& p : c.points) p = toPixel.apply(p);
                return bbox ? rasterizeCoverage(o, W, H, FillRule::NonZero, 4, bbox)
                            : CoverageBuffer{};
            };
            // The outer region first, for the shared sub-rect both coverages rasterize into.
            Stroke os;
            os.width = outerW;
            os.cap = obj.stroke.cap;
            os.dashArray = obj.stroke.dashArray;
            os.dashOffset = obj.stroke.dashOffset;
            Contours outer = strokeOutline(centre, os, tolerancePx, toPixel);
            for (auto& c : outer)
                for (Vec2& p : c.points) p = toPixel.apply(p);
            const std::optional<PixelBounds> bbox = pixelBoundsOf(outer, W, H);
            if (bbox) {
                CoverageBuffer outerCov = rasterizeCoverage(outer, W, H, FillRule::NonZero, 4, bbox);
                const CoverageBuffer innerCov =
                    innerW > 0.0 ? regionCov(innerW, bbox) : CoverageBuffer{};
                CoverageBuffer ring = outerCov; // outer - inner (clamped) == the contrasting border
                for (std::size_t i = 0; i < ring.a.size(); ++i)
                    ring.a[i] = std::max(
                        0.0f, outerCov.a[i] - (i < innerCov.a.size() ? innerCov.a[i] : 0.0f));
                harden(ring);
                if (outlined) { // body in the line colour (stroke), ring in the border colour (fill)
                    CoverageBuffer body = innerCov;
                    harden(body);
                    paintCoverageOver(img, body, obj.stroke.paint, invToPixel, antialias);
                    paintCoverageOver(img, ring, obj.fill, invToPixel, antialias);
                } else { // Hollow: only the ring, in the line colour
                    paintCoverageOver(img, ring, obj.stroke.paint, invToPixel, antialias);
                }
            }
            return img;
        }
    }

    const auto paintFill = [&] {
        if (std::holds_alternative<NoPaint>(obj.fill)) return;
        Contours cs = flatten(obj.geometry, tolerancePx, toPixel);
        for (auto& c : cs)
            for (Vec2& p : c.points) p = toPixel.apply(p);
        const std::optional<PixelBounds> bbox = pixelBoundsOf(cs, W, H);
        if (!bbox) return;
        CoverageBuffer cov = rasterizeCoverage(cs, W, H, fillRuleOf(obj.geometry), 4, bbox);
        harden(cov);
        paintCoverageOver(img, cov, obj.fill, invToPixel, antialias);
    };
    const auto paintStroke = [&] {
        if (!obj.stroke.enabled || std::holds_alternative<NoPaint>(obj.stroke.paint) ||
            obj.stroke.width <= 0.0)
            return;
        const Contours local = flatten(obj.geometry, tolerancePx, toPixel);

        // Inside/Outside alignment (docs/vector-model.md §2.4): only meaningful for a closed,
        // fillable contour. Realised by coverage-clipping a CENTRED, DOUBLE-width stroke against
        // the shape's fill coverage -- robust on any concave/self-intersecting path (it reuses the
        // union-of-pieces rasterizer instead of fragile outline-offsetting). Open paths -> Center.
        const bool anyClosed = std::any_of(local.begin(), local.end(),
                                           [](const Contour& c) { return c.closed; });
        StrokeAlign align = anyClosed ? obj.stroke.align : StrokeAlign::Center;

        Stroke s = obj.stroke;
        if (align != StrokeAlign::Center) s.width = obj.stroke.width * 2.0;
        Contours outline = strokeOutline(local, s, tolerancePx, toPixel);
        for (auto& c : outline)
            for (Vec2& p : c.points) p = toPixel.apply(p);
        const std::optional<PixelBounds> bbox = pixelBoundsOf(outline, W, H);
        if (!bbox) return;
        CoverageBuffer scov = rasterizeCoverage(outline, W, H, FillRule::NonZero, 4, bbox);

        if (align != StrokeAlign::Center) {
            Contours fillc = local;  // the shape's fill, over the SAME sub-rect, to clip against
            for (auto& c : fillc)
                for (Vec2& p : c.points) p = toPixel.apply(p);
            const CoverageBuffer fcov =
                rasterizeCoverage(fillc, W, H, fillRuleOf(obj.geometry), 4, bbox);
            for (std::size_t i = 0; i < scov.a.size(); ++i)  // same extent -> element-wise
                scov.a[i] = (align == StrokeAlign::Inside) ? std::min(scov.a[i], fcov.a[i])
                                                           : std::min(scov.a[i], 1.0f - fcov.a[i]);
        }
        harden(scov);
        paintCoverageOver(img, scov, obj.stroke.paint, invToPixel, antialias);
    };

    if (obj.paintOrder == Object::PaintOrder::StrokeThenFill) {
        paintStroke();
        paintFill();
    } else {
        paintFill();
        paintStroke();
    }
    return img;
}

}  // namespace mosaic::core::vec
