#include "core/texture/sky_estimate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <limits>
#include <numbers>
#include <optional>

#include "core/texture/parallel_rows.hpp"
#include "core/texture/sky_almanac.hpp"
#include "core/texture/sky_camera.hpp"
#include "core/texture/texture_render.hpp"

// The S0-S5 estimate pipeline (docs/research-sky-estimate-from-layer.md §1-§4). Everything here
// is classical, deterministic and pure; the only external work is renderTexture() probe calls
// (S4's analysis-by-synthesis against our own forward model). ⚠ That property is a STANDING
// CONSTRAINT on this pipeline, not an accident of how it was written: no learned model, no
// trained segmenter, no external image data. The per-stage notes say where each stage relies
// on it.
namespace mosaic::core::texture {
namespace {

constexpr double kDegToRad = std::numbers::pi / 180.0;
constexpr double kRadToDeg = 180.0 / std::numbers::pi;

[[nodiscard]] double clamp01(double v) noexcept { return std::clamp(v, 0.0, 1.0); }

[[nodiscard]] double smooth01(double t) noexcept {
    t = clamp01(t);
    return t * t * (3.0 - 2.0 * t);
}

// The standard sRGB electro-optical transfer (IEC 61966-2-1), decode direction; the photo and
// the probe renders pass through the SAME curve so their statistics compare apples-to-apples.
[[nodiscard]] double srgbDecode(double e) noexcept {
    if (e <= 0.04045) return std::max(0.0, e) / 12.92;
    return std::pow((e + 0.055) / 1.055, 2.4);
}

// 256-entry byte decode LUT (the photo side; probes decode their float channels analytically).
[[nodiscard]] const std::array<float, 256>& srgbByteLut() {
    static const std::array<float, 256> lut = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) t[i] = static_cast<float>(srgbDecode(i / 255.0));
        return t;
    }();
    return lut;
}

[[nodiscard]] double linLum(double r, double g, double b) noexcept {
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

// Deterministic PRNG for RANSAC (splitmix64): fixed-seeded, so the whole estimate is a pure
// function of its inputs.
struct SplitMix {
    std::uint64_t s;
    std::uint64_t next() noexcept {
        s += 0x9e3779b97f4a7c15ull;
        std::uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    std::uint32_t below(std::uint32_t n) noexcept {
        return n == 0 ? 0 : static_cast<std::uint32_t>(next() % n);
    }
};

// ---------------------------------------------------------------------------------------------
// S0: working proxy (long edge <= `longEdge`, box-filtered in linear light). The box decimation
// doubles as a cheap noise/JPEG-artifact filter, which the horizon and statistics stages want.
// Analysis is windowed to the layer's opaque content (alpha >= 128) -- windowing the ANALYSIS,
// never the layer; border rows convert back to full document coordinates via
// (offX, offY, scale).
// ---------------------------------------------------------------------------------------------

struct Proxy {
    int w = 0, h = 0;
    double scale = 1.0;  // full-res pixels per proxy pixel
    long offX = 0, offY = 0;
    int fullW = 0, fullH = 0;
    std::vector<float> rgb;             // linear RGB, 3 per pixel
    std::vector<float> lum;             // linear luminance
    std::vector<float> enc;             // encoded (gamma) luma -- the perceptual gradient space
    std::vector<std::uint8_t> valid;    // majority-opaque proxy pixel
    std::vector<float> grad7;           // 7x7 box mean of |grad(enc)| (the texture measure)

    [[nodiscard]] bool ok() const noexcept { return w > 4 && h > 4; }
    [[nodiscard]] std::size_t idx(int x, int y) const noexcept {
        return static_cast<std::size_t>(y) * w + x;
    }
};

Proxy buildProxy(const common::Image& img, int longEdge) {
    Proxy p;
    if (img.empty()) return p;
    p.fullW = static_cast<int>(img.width);
    p.fullH = static_cast<int>(img.height);

    // Opaque-content window: skip fully transparent margins (a layer smaller than the canvas).
    int x0 = p.fullW, y0 = p.fullH, x1 = -1, y1 = -1;
    for (int y = 0; y < p.fullH; ++y) {
        const std::uint8_t* row = img.rgba.data() + static_cast<std::size_t>(y) * p.fullW * 4;
        for (int x = 0; x < p.fullW; ++x) {
            if (row[x * 4 + 3] >= 128) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
        }
    }
    if (x1 < x0 || y1 < y0) return p;  // nothing opaque
    const int cw = x1 - x0 + 1;
    const int ch = y1 - y0 + 1;
    const int k = std::max(1, (std::max(cw, ch) + longEdge - 1) / longEdge);
    p.w = std::max(1, cw / k);
    p.h = std::max(1, ch / k);
    if (!p.ok()) return p;
    p.scale = k;
    p.offX = x0;
    p.offY = y0;
    const std::size_t n = static_cast<std::size_t>(p.w) * p.h;
    p.rgb.assign(n * 3, 0.0f);
    p.lum.assign(n, 0.0f);
    p.enc.assign(n, 0.0f);
    p.valid.assign(n, 0);
    const auto& lut = srgbByteLut();

    for (int py = 0; py < p.h; ++py) {
        for (int px = 0; px < p.w; ++px) {
            double r = 0.0, g = 0.0, b = 0.0, e = 0.0;
            int count = 0, opaque = 0;
            for (int dy = 0; dy < k; ++dy) {
                const int yy = y0 + py * k + dy;
                if (yy > y1) break;
                const std::uint8_t* row =
                    img.rgba.data() + (static_cast<std::size_t>(yy) * p.fullW + x0 + px * k) * 4;
                for (int dx = 0; dx < k; ++dx) {
                    const int xx = x0 + px * k + dx;
                    if (xx > x1) break;
                    const std::uint8_t* q = row + dx * 4;
                    ++count;
                    if (q[3] < 128) continue;
                    ++opaque;
                    r += lut[q[0]];
                    g += lut[q[1]];
                    b += lut[q[2]];
                    e += (0.2126 * q[0] + 0.7152 * q[1] + 0.0722 * q[2]) / 255.0;
                }
            }
            const std::size_t i = p.idx(px, py);
            if (opaque * 2 >= count && opaque > 0) {
                p.valid[i] = 1;
                p.rgb[i * 3 + 0] = static_cast<float>(r / opaque);
                p.rgb[i * 3 + 1] = static_cast<float>(g / opaque);
                p.rgb[i * 3 + 2] = static_cast<float>(b / opaque);
                p.lum[i] = static_cast<float>(
                    linLum(p.rgb[i * 3], p.rgb[i * 3 + 1], p.rgb[i * 3 + 2]));
                p.enc[i] = static_cast<float>(e / opaque);
            }
        }
    }

    // Texture measure: |grad| of encoded luma (L1, central differences), then a 7x7 box mean.
    std::vector<float> g1(n, 0.0f);
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) {
            const int xm = std::max(0, x - 1), xp = std::min(p.w - 1, x + 1);
            const int ym = std::max(0, y - 1), yp = std::min(p.h - 1, y + 1);
            g1[p.idx(x, y)] = 0.5f * (std::abs(p.enc[p.idx(xp, y)] - p.enc[p.idx(xm, y)]) +
                                      std::abs(p.enc[p.idx(x, yp)] - p.enc[p.idx(x, ym)]));
        }
    // Separable 7x7 box (edge-clamped).
    std::vector<float> tmp(n, 0.0f);
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) {
            float s = 0.0f;
            for (int d = -3; d <= 3; ++d) s += g1[p.idx(std::clamp(x + d, 0, p.w - 1), y)];
            tmp[p.idx(x, y)] = s / 7.0f;
        }
    p.grad7.assign(n, 0.0f);
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) {
            float s = 0.0f;
            for (int d = -3; d <= 3; ++d) s += tmp[p.idx(x, std::clamp(y + d, 0, p.h - 1))];
            p.grad7[p.idx(x, y)] = s / 7.0f;
        }
    return p;
}

// ---------------------------------------------------------------------------------------------
// S2: the pixel prior P (Luo & Etz 2002 color physics; Zafarifar & de With 2006 combination) and
// its one EM-lite lobe refit. NO histogram, NO intensity quantile anywhere here: the
// luminance term normalizes by the scene MEAN, and the refit selects pixels by a fixed fraction
// of the prior's own maximum, never by an intensity distribution.
// ---------------------------------------------------------------------------------------------

// Flat-top Gaussian: full score within one Mahalanobis unit, Gaussian falloff beyond. A sky
// dome's chromaticity VARIES smoothly (zenith blue -> horizon haze), so a plain Gaussian around
// the refit mean would score the dome's own gradient as "not sky"; the flat top keeps the whole
// in-lobe family at 1 while ground colors (many units out) still crater.
[[nodiscard]] double lobeScore(const ChromaLobe& l, double cr, double cb) noexcept {
    const double dr = (cr - l.meanR) / std::max(1e-3, l.sigmaR);
    const double db = (cb - l.meanB) / std::max(1e-3, l.sigmaB);
    const double r = std::sqrt(dr * dr + db * db);
    if (r <= 1.0) return 1.0;
    return std::exp(-(r - 1.0) * (r - 1.0));
}

// Chromaticity of a linear-RGB proxy pixel: (r, b) components of rgb / (r+g+b).
void chromaAt(const Proxy& p, std::size_t i, double& cr, double& cb) {
    const double r = p.rgb[i * 3], g = p.rgb[i * 3 + 1], b = p.rgb[i * 3 + 2];
    const double s = r + g + b + 1e-9;
    cr = r / s;
    cb = b / s;
}

// P without the position term (the first, horizon-free pass) or with it (line in proxy coords).
std::vector<float> priorMap(const Proxy& p, const SkyColorModel& m, const double* lineM,
                            const double* lineB) {
    const std::size_t n = static_cast<std::size_t>(p.w) * p.h;
    std::vector<float> P(n, 0.0f);
    constexpr double g0 = 4.0 / 255.0;  // "sky is smooth at proxy scale"
    for (int y = 0; y < p.h; ++y) {
        for (int x = 0; x < p.w; ++x) {
            const std::size_t i = p.idx(x, y);
            if (!p.valid[i]) continue;
            double cr, cb;
            chromaAt(p, i, cr, cb);
            // Bright neutral clouds need brightness as well as neutrality; gate the gray lobe
            // on the bright-region normaliser (no percentile, deliberately). Once the
            // refit relocated the lobe to a twilight/golden family, the gate stands down: the
            // lobe is sky chroma now, and a dusk sky is legitimately dim.
            const double grayGate =
                m.grayAdaptive
                    ? 1.0
                    : smooth01((p.lum[i] / std::max(1e-4, m.lumNorm) - 0.45) / 0.3);
            const double color =
                std::max(lobeScore(m.blue, cr, cb), lobeScore(m.gray, cr, cb) * grayGate);
            const double gr = p.grad7[i] / g0;
            const double texture = std::exp(-gr * gr);
            const double lumT =
                std::max(0.35, clamp01(p.lum[i] / std::max(1e-4, 0.9 * m.lumNorm)));
            double pos = 1.0;
            if (lineM != nullptr && lineB != nullptr) {
                const double hz = *lineM * (x + 0.5) + *lineB;  // proxy coords
                pos = 1.0 / (1.0 + std::exp(-(hz - y) / (0.05 * p.h)));
            }
            const double v = std::pow(pos, 1.0) * std::pow(std::max(color, 1e-4), 1.2) *
                             std::pow(std::max(texture, 1e-4), 0.8) * std::pow(lumT, 0.5);
            P[i] = static_cast<float>(clamp01(v));
        }
    }
    return P;
}

SkyColorModel fitColorModel(const Proxy& p) {
    SkyColorModel m;
    // Scene mean luminance over valid pixels (the Lum normalizer).
    double lumSum = 0.0;
    std::size_t nValid = 0;
    const std::size_t n = static_cast<std::size_t>(p.w) * p.h;
    for (std::size_t i = 0; i < n; ++i)
        if (p.valid[i]) {
            lumSum += p.lum[i];
            ++nValid;
        }
    if (nValid == 0) return m;
    m.meanLum = std::max(1e-4, lumSum / static_cast<double>(nValid));
    // Bright-region normaliser: the mean of the pixels at/above the scene mean (two mean
    // passes; deliberately NOT a percentile). This is what "as bright as
    // the sky" means to the Lum term, so a dark ground band cannot drag the sky's own score.
    double hiSum = 0.0;
    std::size_t hiN = 0;
    for (std::size_t i = 0; i < n; ++i)
        if (p.valid[i] && p.lum[i] >= m.meanLum) {
            hiSum += p.lum[i];
            ++hiN;
        }
    m.lumNorm = hiN > 0 ? std::max(1e-4, hiSum / static_cast<double>(hiN)) : m.meanLum;

    // One EM-lite iteration (Zafarifar's adaptive-model idea, no offline training): score with
    // the canonical seeds, keep pixels above a fixed fraction of the map's own maximum, assign
    // each to its nearer lobe, refit means + sigmas (sigma ranges clamped so a lobe can never
    // collapse or eat the plane).
    const std::vector<float> P0 = priorMap(p, m, nullptr, nullptr);
    float maxP = 0.0f;
    for (const float v : P0) maxP = std::max(maxP, v);
    if (maxP <= 0.0f) return m;
    const float cut = 0.7f * maxP;
    double bs[5] = {0, 0, 0, 0, 0};  // blue lobe: n, sum cr, sum cb, sum cr^2, sum cb^2
    double gs[5] = {0, 0, 0, 0, 0};
    for (std::size_t i = 0; i < n; ++i) {
        if (!p.valid[i] || P0[i] < cut) continue;
        double cr, cb;
        chromaAt(p, i, cr, cb);
        const bool toBlue = lobeScore(m.blue, cr, cb) >= lobeScore(m.gray, cr, cb);
        double* s = toBlue ? bs : gs;
        s[0] += 1.0;
        s[1] += cr;
        s[2] += cb;
        s[3] += cr * cr;
        s[4] += cb * cb;
    }
    const auto refit = [](ChromaLobe& l, const double* s, double sigLo, double sigHi) {
        if (s[0] < 24.0) return;  // too few supporters: keep the seed
        const double mr = s[1] / s[0], mb = s[2] / s[0];
        const double vr = std::max(0.0, s[3] / s[0] - mr * mr);
        const double vb = std::max(0.0, s[4] / s[0] - mb * mb);
        l.meanR = mr;
        l.meanB = mb;
        // Padded: the supporters are the prior's own TOP slice, chromatically narrower than the
        // real sky family -- an unpadded refit over-tightens and rejects the dome's gradient.
        l.sigmaR = std::clamp(std::sqrt(vr) * 1.5 + 0.02, sigLo, sigHi);
        l.sigmaB = std::clamp(std::sqrt(vb) * 1.5 + 0.02, sigLo, sigHi);
    };
    refit(m.blue, bs, 0.05, 0.15);
    refit(m.gray, gs, 0.035, 0.08);
    m.grayAdaptive = std::hypot(m.gray.meanR - 1.0 / 3.0, m.gray.meanB - 1.0 / 3.0) > 0.05;
    return m;
}

// ---------------------------------------------------------------------------------------------
// S2: the DP sky-border polyline (Lie/Lin/Lin/Hung 2005 multi-stage graph with Zafarifar-style
// regional terms). One boundary row per column, smoothness-regularized: structurally leak-proof.
// The pairwise lambda*|db| term is minimized exactly with two sliding-window minima (monotonic
// deques), O(W * band). Rows [0, b) are sky, [b, H] ground; b == H means an all-sky column.
// ---------------------------------------------------------------------------------------------

struct DpResult {
    std::vector<int> border;      // per column, in [y0[x], y1[x]]
    std::vector<float> quality;   // per-column border quality for the §5.3 gate
};

DpResult dpBorder(const Proxy& p, const std::vector<float>& P, const std::vector<int>& y0,
                  const std::vector<int>& y1) {
    const int W = p.w, H = p.h;
    const int rows = H + 1;  // border value range is [0, H]
    constexpr double kInf = 1e15;
    constexpr double kAlpha = 3.0, kBeta = 3.0, kEdgeW = 6.0;
    // Smoothness: recalibrated against the per-pixel-normalized unary terms (the research doc
    // notes the change from the design draft's 2/px): scale-invariant in the proxy height.
    const double lambda = 14.0 / std::max(1, H);
    const int smax = std::max(2, static_cast<int>(0.15 * H));

    // Per-column prefix sums of sqrt(P) (invalid pixels score 0 -- they are "not sky" and also
    // "not ground evidence"; the mean divisors use the row count, which keeps columns
    // comparable). The sqrt compresses the prior toward 1 for the REGIONAL terms: a dim
    // twilight sky legitimately scores ~0.3 on the absolute prior, and the raw value would read
    // as "more ground than sky" to the symmetric alpha/beta costs -- the compressed value keeps
    // the ordering (sky >> ground) while letting the edge term decide the boundary row.
    std::vector<double> prefix(static_cast<std::size_t>(rows));
    std::vector<double> unary(static_cast<std::size_t>(rows));
    std::vector<double> dpPrev(static_cast<std::size_t>(rows), 0.0);
    std::vector<double> dpCur(static_cast<std::size_t>(rows), 0.0);
    std::vector<int> back(static_cast<std::size_t>(rows) * W, 0);
    std::vector<double> bestLeft(rows), bestRight(rows);
    std::vector<int> argLeft(rows), argRight(rows);

    DpResult out;
    out.border.assign(W, H);
    out.quality.assign(W, 0.0f);
    if (W <= 0 || H <= 1) return out;

    const auto edgeAt = [&](int x, int b) -> double {
        if (b <= 0 || b >= H) return 0.0;
        // Vertical transition strength at the candidate border, lightly smoothed across x.
        double e = 0.0;
        int c = 0;
        for (int dx = -1; dx <= 1; ++dx) {
            const int xx = std::clamp(x + dx, 0, W - 1);
            e += std::abs(p.enc[p.idx(xx, b)] - p.enc[p.idx(xx, b - 1)]);
            ++c;
        }
        return e / c;
    };

    std::vector<double> colUnary(static_cast<std::size_t>(rows) * W);
    for (int x = 0; x < W; ++x) {
        prefix[0] = 0.0;
        for (int y = 0; y < H; ++y)
            prefix[y + 1] = prefix[y] + std::sqrt(static_cast<double>(P[p.idx(x, y)]));
        for (int b = 0; b <= H; ++b) {
            double u;
            if (b < y0[x] || b > y1[x]) {
                u = kInf;
            } else {
                const double above = b > 0 ? (b - prefix[b]) / b : 0.0;         // 1-P deficit
                const double below = b < H ? (prefix[H] - prefix[b]) / (H - b) : 0.0;
                u = -kEdgeW * edgeAt(x, b) + kAlpha * above + kBeta * below;
            }
            colUnary[static_cast<std::size_t>(x) * rows + b] = u;
        }
    }

    for (int x = 0; x < W; ++x) {
        const double* u = &colUnary[static_cast<std::size_t>(x) * rows];
        if (x == 0) {
            for (int b = 0; b < rows; ++b) dpCur[b] = u[b];
        } else {
            // bestLeft[b] = min over b' in [b-smax, b] of dpPrev[b'] - lambda*b'.
            {
                std::deque<int> q;
                for (int b = 0; b < rows; ++b) {
                    const double v = dpPrev[b] - lambda * b;
                    while (!q.empty() && dpPrev[q.back()] - lambda * q.back() >= v) q.pop_back();
                    q.push_back(b);
                    while (q.front() < b - smax) q.pop_front();
                    bestLeft[b] = dpPrev[q.front()] - lambda * q.front();
                    argLeft[b] = q.front();
                }
            }
            // bestRight[b] = min over b' in [b, b+smax] of dpPrev[b'] + lambda*b'.
            {
                std::deque<int> q;
                for (int b = rows - 1; b >= 0; --b) {
                    const double v = dpPrev[b] + lambda * b;
                    while (!q.empty() && dpPrev[q.back()] + lambda * q.back() >= v) q.pop_back();
                    q.push_back(b);
                    while (q.front() > b + smax) q.pop_front();
                    bestRight[b] = dpPrev[q.front()] + lambda * q.front();
                    argRight[b] = q.front();
                }
            }
            for (int b = 0; b < rows; ++b) {
                const double l = bestLeft[b] + lambda * b;
                const double r = bestRight[b] - lambda * b;
                if (l <= r) {
                    dpCur[b] = u[b] + l;
                    back[static_cast<std::size_t>(x) * rows + b] = argLeft[b];
                } else {
                    dpCur[b] = u[b] + r;
                    back[static_cast<std::size_t>(x) * rows + b] = argRight[b];
                }
            }
        }
        std::swap(dpPrev, dpCur);
    }

    // Recover the path.
    int best = 0;
    for (int b = 1; b < rows; ++b)
        if (dpPrev[b] < dpPrev[best]) best = b;
    for (int x = W - 1; x >= 0; --x) {
        out.border[x] = best;
        if (x > 0) best = back[static_cast<std::size_t>(x) * rows + best];
    }

    // Per-column quality for the §5.3 gate: transition strength + prior separation across the
    // chosen border (a ragged, low-separation column reads as fine structure -- trees, hair).
    for (int x = 0; x < W; ++x) {
        const int b = out.border[x];
        prefix[0] = 0.0;
        for (int y = 0; y < H; ++y) prefix[y + 1] = prefix[y] + P[p.idx(x, y)];
        const double meanAbove = b > 0 ? prefix[b] / b : 0.0;
        const double meanBelow = b < H ? (prefix[H] - prefix[b]) / (H - b) : 0.0;
        out.quality[x] = static_cast<float>(2.0 * edgeAt(x, b) + (meanAbove - meanBelow));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// S1: horizon line. PRIMARY = robust RANSAC line fit (Fischler & Bolles 1981) to the DP border
// points, with the lower-quartile behavior when the border is ragged (an occluded skyline sits
// ABOVE the true horizon, so the lowest border points are the least-occluded evidence).
// CROSS-CHECK = Sobel + Hough (Duda & Hart 1972) + RANSAC on gradient maxima. Neither computes
// an intensity distribution, tests bimodality, or thresholds the image globally by intensity
//: the Sobel gate is a FIXED gradient threshold with one fixed relaxation step.
// ---------------------------------------------------------------------------------------------

struct LineFit {
    bool ok = false;
    double m = 0.0, b = 0.0;   // y = m*x + b, PROXY coordinates of the fitted grid
    double inlierFrac = 0.0;   // fraction of candidate points within the tight band
    double margin = 0.0;       // fraction of ALL border points near the line (peakedness)
    bool ragged = false;       // lower-quartile mode engaged (skyline suspected)
};

// Least-squares y = m x + b over a subset of points.
bool lsLine(const std::vector<double>& xs, const std::vector<double>& ys, double& m, double& b) {
    const std::size_t n = xs.size();
    if (n < 2) return false;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (std::size_t i = 0; i < n; ++i) {
        sx += xs[i];
        sy += ys[i];
        sxx += xs[i] * xs[i];
        sxy += xs[i] * ys[i];
    }
    const double d = n * sxx - sx * sx;
    if (std::abs(d) < 1e-9) return false;
    m = (n * sxy - sx * sy) / d;
    b = (sy - m * sx) / n;
    return true;
}

LineFit ransacLine(const std::vector<double>& xs, const std::vector<double>& ys, double band,
                   std::uint64_t seed) {
    LineFit f;
    const std::size_t n = xs.size();
    if (n < 8) return f;
    SplitMix rng{seed};
    double bm = 0.0, bb = 0.0;
    int bestIn = -1;
    for (int it = 0; it < 500; ++it) {
        const std::uint32_t i = rng.below(static_cast<std::uint32_t>(n));
        std::uint32_t j = rng.below(static_cast<std::uint32_t>(n));
        if (i == j || std::abs(xs[i] - xs[j]) < 1e-6) continue;
        const double m = (ys[j] - ys[i]) / (xs[j] - xs[i]);
        if (std::abs(m) > std::tan(40.0 * kDegToRad)) continue;  // tilt cap
        const double b = ys[i] - m * xs[i];
        int in = 0;
        for (std::size_t k = 0; k < n; ++k)
            if (std::abs(m * xs[k] + b - ys[k]) <= band) ++in;
        if (in > bestIn) {
            bestIn = in;
            bm = m;
            bb = b;
        }
    }
    if (bestIn < 8) return f;
    // Least-squares polish on the inliers.
    std::vector<double> ix, iy;
    for (std::size_t k = 0; k < n; ++k)
        if (std::abs(bm * xs[k] + bb - ys[k]) <= band) {
            ix.push_back(xs[k]);
            iy.push_back(ys[k]);
        }
    if (!lsLine(ix, iy, bm, bb)) return f;
    f.ok = true;
    f.m = bm;
    f.b = bb;
    f.inlierFrac = static_cast<double>(bestIn) / static_cast<double>(n);
    return f;
}

LineFit fitBorderLine(const Proxy& p, const std::vector<int>& border, std::uint64_t seed) {
    LineFit f;
    // Candidate points: columns whose border sits strictly inside the frame (an all-sky or
    // all-ground column carries no boundary evidence).
    std::vector<double> xs, ys;
    for (int x = 0; x < p.w; ++x)
        if (border[x] > 0 && border[x] < p.h) {
            xs.push_back(x + 0.5);
            ys.push_back(border[x]);
        }
    if (xs.size() < static_cast<std::size_t>(std::max(8, p.w / 4))) return f;

    // Border roughness: mean adjacent-column jump. A ragged border (trees, roofs) biases the
    // fit up, so the fit moves to the LOWER-QUARTILE points (largest y = least occluded).
    double jump = 0.0;
    int jumps = 0;
    for (std::size_t i = 1; i < ys.size(); ++i) {
        jump += std::abs(ys[i] - ys[i - 1]);
        ++jumps;
    }
    const bool ragged = jumps > 0 && (jump / jumps) > 0.02 * p.h;
    std::vector<double> fx = xs, fy = ys;
    if (ragged) {
        std::vector<double> sorted = ys;
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() * 3 / 4, sorted.end());
        const double q = sorted[sorted.size() * 3 / 4];  // 75th of border ROWS (geometry, not
                                                         // intensity)
        fx.clear();
        fy.clear();
        for (std::size_t i = 0; i < xs.size(); ++i)
            if (ys[i] >= q) {
                fx.push_back(xs[i]);
                fy.push_back(ys[i]);
            }
    }
    const double band = std::max(2.0, 0.01 * p.h);
    f = ransacLine(fx, fy, band, seed);
    f.ragged = ragged;
    if (!f.ok) return f;
    // Peakedness: how line-like the WHOLE border is (the confidence margin term).
    int near = 0;
    for (std::size_t i = 0; i < xs.size(); ++i)
        if (std::abs(f.m * xs[i] + f.b - ys[i]) <= 0.02 * p.h) ++near;
    f.margin = static_cast<double>(near) / static_cast<double>(xs.size());
    return f;
}

struct CrossCheck {
    bool ok = false;
    double m = 0.0, b = 0.0;  // proxy coordinates
    double inlierFrac = 0.0;
};

CrossCheck sobelHoughLine(const Proxy& p, std::uint64_t seed) {
    CrossCheck c;
    const int W = p.w, H = p.h;
    struct Pt {
        int x, y;
    };
    std::vector<Pt> edges;
    // FIXED gradient gate (one fixed relaxation step; never a distribution-derived threshold --
    // deliberately). Edge orientation within ~30 deg of horizontal: |gy| dominates.
    for (double thresh : {0.08, 0.04}) {
        edges.clear();
        for (int y = 1; y < H - 1; ++y)
            for (int x = 1; x < W - 1; ++x) {
                const auto e = [&](int xx, int yy) { return p.enc[p.idx(xx, yy)]; };
                const double gx = (e(x + 1, y - 1) + 2 * e(x + 1, y) + e(x + 1, y + 1)) -
                                  (e(x - 1, y - 1) + 2 * e(x - 1, y) + e(x - 1, y + 1));
                const double gy = (e(x - 1, y + 1) + 2 * e(x, y + 1) + e(x + 1, y + 1)) -
                                  (e(x - 1, y - 1) + 2 * e(x, y - 1) + e(x + 1, y - 1));
                if (std::hypot(gx, gy) >= thresh * 8.0 && std::abs(gy) >= 1.732 * std::abs(gx))
                    edges.push_back({x, y});
            }
        if (edges.size() >= 300) break;
    }
    if (edges.size() < 50) return c;
    // Deterministic decimation cap (never "strongest N" -- that would need a sort over
    // magnitudes; a stride keeps it distribution-free and cheap).
    if (edges.size() > 20000) {
        const std::size_t step = edges.size() / 20000 + 1;
        std::vector<Pt> dec;
        for (std::size_t i = 0; i < edges.size(); i += step) dec.push_back(edges[i]);
        edges.swap(dec);
    }

    // Hough over (tilt, intercept-at-centre): tilt +-40 deg step 1, intercept 2 px bins.
    constexpr int kTiltBins = 81;
    const int cBins = std::max(8, H);  // 2 px bins over [-H/2, 3H/2)
    std::vector<int> acc(static_cast<std::size_t>(kTiltBins) * cBins, 0);
    const double cx = W * 0.5;
    for (const Pt& e : edges) {
        for (int t = 0; t < kTiltBins; ++t) {
            const double tau = (t - 40) * kDegToRad;
            const double cc = e.y - std::tan(tau) * (e.x - cx);
            const int ci = static_cast<int>(std::floor((cc + 0.5 * H) / 2.0));
            if (ci >= 0 && ci < cBins) ++acc[static_cast<std::size_t>(t) * cBins + ci];
        }
    }
    int bt = 0, bc = 0, bv = 0;
    for (int t = 0; t < kTiltBins; ++t)
        for (int ci = 0; ci < cBins; ++ci) {
            const int v = acc[static_cast<std::size_t>(t) * cBins + ci];
            if (v > bv) {
                bv = v;
                bt = t;
                bc = ci;
            }
        }
    if (bv < 24) return c;
    const double tau = (bt - 40) * kDegToRad;
    const double cc = bc * 2.0 - 0.5 * H + 1.0;

    // Per-column TOPMOST edge point within +-5 px of the peak line, then RANSAC + polish.
    std::vector<double> xs, ys;
    std::vector<int> topRow(W, -1);
    for (const Pt& e : edges) {
        const double ly = std::tan(tau) * (e.x - cx) + cc;
        if (std::abs(e.y - ly) <= 5.0 && (topRow[e.x] < 0 || e.y < topRow[e.x])) topRow[e.x] = e.y;
    }
    for (int x = 0; x < W; ++x)
        if (topRow[x] >= 0) {
            xs.push_back(x + 0.5);
            ys.push_back(topRow[x]);
        }
    const LineFit rf = ransacLine(xs, ys, 2.0, seed ^ 0xC0FFEEull);
    if (!rf.ok) return c;
    c.ok = true;
    c.m = rf.m;
    c.b = rf.b;
    c.inlierFrac = rf.inlierFrac;
    return c;
}

// The cheap-physics sanity gates (the indoor/closeup rejector): the "sky" side must look like
// sky. CLASS MEANS over the split -- no distribution, no histogram.  
struct Gates {
    int passed = 0;     // of 3
    bool hardFail = false;
};

Gates skyGates(const Proxy& p, double m, double b) {
    Gates g;
    double lumA = 0, lumB = 0, blueA = 0, blueB = 0, gradA = 0, gradB = 0;
    std::size_t nA = 0, nB = 0;
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) {
            const std::size_t i = p.idx(x, y);
            if (!p.valid[i]) continue;
            double cr, cb;
            chromaAt(p, i, cr, cb);
            if (y < m * (x + 0.5) + b) {
                lumA += p.lum[i];
                blueA += cb;
                gradA += p.grad7[i];
                ++nA;
            } else {
                lumB += p.lum[i];
                blueB += cb;
                gradB += p.grad7[i];
                ++nB;
            }
        }
    if (nA < 16 || nB < 16) {
        g.hardFail = true;
        return g;
    }
    const bool lumOk = lumA / nA > lumB / nB;
    const bool blueOk = blueA / nA > blueB / nB;
    const bool gradOk = gradA / nA < gradB / nB;
    g.passed = (lumOk ? 1 : 0) + (blueOk ? 1 : 0) + (gradOk ? 1 : 0);
    g.hardFail = !lumOk && !blueOk && !gradOk;
    return g;
}

// ---------------------------------------------------------------------------------------------
// S1 -> camera: closed form + the fixed 2-parameter Gauss-Newton polish (pitch and roll
// ONLY, FOV held, no intrinsics, no vanishing points, no reprojection of the layer). The model
// horizon under candidate params comes from SkyCamera::project() of level world directions --
// the projection of the zero-elevation circle is exactly a line for a pinhole camera.
// ---------------------------------------------------------------------------------------------

struct HorizonEndpoints {
    double yAt0 = 0.0, yAtW = 0.0;  // document pixels
};

std::optional<HorizonEndpoints> modelHorizonLine(const SkyParams& s, std::uint32_t W,
                                                 std::uint32_t H) {
    const SkyCamera cam = SkyCamera::fromParams(s, W, H);
    double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
    if (!cam.project(directionFromAzEl(168.0, 0.0), x1, y1)) return std::nullopt;
    if (!cam.project(directionFromAzEl(192.0, 0.0), x2, y2)) return std::nullopt;
    if (std::abs(x2 - x1) < 1e-6) return std::nullopt;
    const double m = (y2 - y1) / (x2 - x1);
    const double b = y1 - m * x1;
    return HorizonEndpoints{b, m * W + b};
}

void refinePitchRoll(SkyParams& s, std::uint32_t W, std::uint32_t H, double yFit0, double yFitW) {
    for (int it = 0; it < 5; ++it) {
        const auto base = modelHorizonLine(s, W, H);
        if (!base) return;
        const double r0 = base->yAt0 - yFit0;
        const double r1 = base->yAtW - yFitW;
        if (std::abs(r0) < 1e-3 && std::abs(r1) < 1e-3) return;
        constexpr double d = 0.05;  // degrees
        SkyParams sp = s, sr = s;
        sp.pitchDeg += d;
        sr.rollDeg += d;
        const auto jp = modelHorizonLine(sp, W, H);
        const auto jr = modelHorizonLine(sr, W, H);
        if (!jp || !jr) return;
        const double j00 = (jp->yAt0 - base->yAt0) / d, j01 = (jr->yAt0 - base->yAt0) / d;
        const double j10 = (jp->yAtW - base->yAtW) / d, j11 = (jr->yAtW - base->yAtW) / d;
        const double det = j00 * j11 - j01 * j10;
        if (std::abs(det) < 1e-9) return;
        // Solve J * delta = -r  (2x2 Cramer).
        const double dp = ((-r0) * j11 - j01 * (-r1)) / det;
        const double dr = (j00 * (-r1) - (-r0) * j10) / det;
        s.pitchDeg = std::clamp(s.pitchDeg + dp, -45.0, 85.0);
        s.rollDeg = std::clamp(s.rollDeg + dr, -45.0, 45.0);
    }
}

// ---------------------------------------------------------------------------------------------
// S3: sun disc detection (Cozman & Krotkov 1995 brightest-region centroiding + radial-glow
// validation). Fixed saturation-crater level -- clipped means clipped, no adaptive threshold --
// and everything is gated to the S2 sky region, so streetlights and specular water stay out.
// ---------------------------------------------------------------------------------------------

struct SunFind {
    bool found = false;
    double confidence = 0.0;
    double px = 0.0, py = 0.0;  // PROXY coordinates of the crater/glow centroid
    bool behindCloud = false;
    bool treatedAsMoon = false;
    bool overexposed = false;   // > 40% of the sky is clipped: position meaningless
    std::string note;
};

SunFind detectSun(const Proxy& p, const std::vector<int>& border, double rDiscProxy,
                  double skyMedianLum) {
    SunFind out;
    const int W = p.w, H = p.h;
    const std::size_t n = static_cast<std::size_t>(W) * H;
    // Sky mask (above border), dilated by 5 px so a sun kissing the skyline still counts.
    std::vector<std::uint8_t> sky(n, 0);
    for (int x = 0; x < W; ++x)
        for (int y = 0; y < border[x]; ++y) sky[p.idx(x, y)] = 1;
    std::vector<std::uint8_t> dil = sky;
    for (int pass = 0; pass < 5; ++pass) {
        std::vector<std::uint8_t> next = dil;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (dil[p.idx(x, y)]) continue;
                if ((x > 0 && dil[p.idx(x - 1, y)]) || (x + 1 < W && dil[p.idx(x + 1, y)]) ||
                    (y > 0 && dil[p.idx(x, y - 1)]) || (y + 1 < H && dil[p.idx(x, y + 1)]))
                    next[p.idx(x, y)] = 1;
            }
        dil.swap(next);
    }

    // Saturation crater: min(R,G,B) at/above the 250/255 clip level, in linear terms.
    const float clipLin = static_cast<float>(srgbDecode(250.0 / 255.0));
    std::vector<std::uint8_t> crater(n, 0);
    std::size_t skyPx = 0, clippedSky = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const std::size_t i = p.idx(x, y);
            if (!p.valid[i]) continue;
            const float mn = std::min({p.rgb[i * 3], p.rgb[i * 3 + 1], p.rgb[i * 3 + 2]});
            if (sky[i]) {
                ++skyPx;
                if (mn >= clipLin) ++clippedSky;
            }
            if (dil[i] && mn >= clipLin) crater[i] = 1;
        }
    if (skyPx == 0) return out;
    if (static_cast<double>(clippedSky) / static_cast<double>(skyPx) > 0.40) {
        out.overexposed = true;
        out.note = "sky is largely blown out -- sun position not measurable";
        return out;
    }

    // Connected components of the crater mask (8-connected BFS).
    struct Comp {
        double cx = 0, cy = 0;
        int area = 0, perim = 0;
        double glowA = 0, glowB = 0, fitR2 = 0;
        bool valid = false;
    };
    std::vector<int> label(n, -1);
    std::vector<Comp> comps;
    std::vector<std::size_t> stack;
    for (std::size_t s0 = 0; s0 < n; ++s0) {
        if (!crater[s0] || label[s0] >= 0) continue;
        const int id = static_cast<int>(comps.size());
        comps.push_back({});
        Comp& c = comps.back();
        stack.assign(1, s0);
        label[s0] = id;
        while (!stack.empty()) {
            const std::size_t i = stack.back();
            stack.pop_back();
            const int x = static_cast<int>(i % W), y = static_cast<int>(i / W);
            c.cx += x + 0.5;
            c.cy += y + 0.5;
            ++c.area;
            bool edge = false;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H) {
                        edge = true;
                        continue;
                    }
                    const std::size_t j = p.idx(xx, yy);
                    if (!crater[j]) {
                        if (std::abs(dx) + std::abs(dy) == 1) edge = true;
                        continue;
                    }
                    if (label[j] < 0) {
                        label[j] = id;
                        stack.push_back(j);
                    }
                }
            if (edge) ++c.perim;
        }
        c.cx /= c.area;
        c.cy /= c.area;
    }

    // Validate each component: compactness, size, monotone radial glow with an exponential fit.
    for (Comp& c : comps) {
        if (c.area < 4) continue;
        const double equivR = std::sqrt(c.area / std::numbers::pi);
        if (equivR > 0.25 * W) continue;
        const double roundness =
            4.0 * std::numbers::pi * c.area / std::max(1.0, static_cast<double>(c.perim) * c.perim);
        if (roundness < 0.5) continue;
        // Radial profile over rings r in [r0, 4 r0].
        constexpr int kRings = 8;
        double ringLum[kRings] = {};
        int ringN[kRings] = {};
        const double r0 = std::max(1.0, equivR);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const std::size_t i = p.idx(x, y);
                if (!p.valid[i] || !dil[i]) continue;
                const double r = std::hypot(x + 0.5 - c.cx, y + 0.5 - c.cy);
                if (r < r0 || r >= 4.0 * r0) continue;
                const int ring = std::min(kRings - 1,
                                          static_cast<int>((r - r0) / (3.0 * r0) * kRings));
                ringLum[ring] += p.lum[i];
                ++ringN[ring];
            }
        bool haveAll = true, monotone = true;
        for (int k = 0; k < kRings; ++k) {
            if (ringN[k] < 2) {
                haveAll = false;
                break;
            }
            ringLum[k] /= ringN[k];
            if (k > 0 && ringLum[k] > ringLum[k - 1] * 1.06) monotone = false;
        }
        if (!haveAll || !monotone) continue;
        // Fit L(r) = a * exp(-r / b) + c: floor at the outermost ring, then log-linear LS.
        const double floor = ringLum[kRings - 1];
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int nfit = 0;
        for (int k = 0; k < kRings; ++k) {
            const double rMid = r0 + (k + 0.5) * 3.0 * r0 / kRings;
            const double v = ringLum[k] - floor;
            if (v <= 1e-6) continue;
            const double ly = std::log(v);
            sx += rMid;
            sy += ly;
            sxx += rMid * rMid;
            sxy += rMid * ly;
            ++nfit;
        }
        if (nfit < 4) continue;
        const double det = nfit * sxx - sx * sx;
        if (std::abs(det) < 1e-9) continue;
        const double slope = (nfit * sxy - sx * sy) / det;
        if (slope >= -1e-6) continue;  // must decay
        const double icept = (sy - slope * sx) / nfit;
        const double a = std::exp(icept);
        const double bDecay = -1.0 / slope;
        // R^2 of the fitted curve against the ring means (original values).
        double mean = 0.0;
        for (int k = 0; k < kRings; ++k) mean += ringLum[k];
        mean /= kRings;
        double ssTot = 0.0, ssRes = 0.0;
        for (int k = 0; k < kRings; ++k) {
            const double rMid = r0 + (k + 0.5) * 3.0 * r0 / kRings;
            const double fit = a * std::exp(-rMid / bDecay) + floor;
            ssTot += (ringLum[k] - mean) * (ringLum[k] - mean);
            ssRes += (ringLum[k] - fit) * (ringLum[k] - fit);
        }
        const double r2 = ssTot > 1e-12 ? 1.0 - ssRes / ssTot : 0.0;
        if (r2 < 0.8) continue;
        c.glowA = a;
        c.glowB = bDecay;
        c.fitR2 = r2;
        c.valid = true;
    }

    // Pick the brightest-widest glow; require a decisive margin over the runner-up.
    int best = -1, second = -1;
    for (std::size_t i = 0; i < comps.size(); ++i) {
        if (!comps[i].valid) continue;
        const double ab = comps[i].glowA * comps[i].glowB;
        if (best < 0 || ab > comps[best].glowA * comps[best].glowB) {
            second = best;
            best = static_cast<int>(i);
        } else if (second < 0 || ab > comps[second].glowA * comps[second].glowB) {
            second = static_cast<int>(i);
        }
    }
    if (best >= 0 && second >= 0 &&
        comps[best].glowA * comps[best].glowB <
            3.0 * comps[second].glowA * comps[second].glowB) {
        out.note = "several bright discs -- sun position ambiguous";
        return out;
    }
    if (best >= 0) {
        const Comp& c = comps[best];
        const double equivR = std::sqrt(c.area / std::numbers::pi);
        if (equivR < 0.5 * rDiscProxy || equivR > 20.0 * rDiscProxy) {
            out.note = "bright disc size implausible for the sun -- ignored";
            return out;
        }
        // Night + near-physical disc + tight glow: report the moon, leave the sun alone (v1).
        if (skyMedianLum < 0.01 && equivR <= 2.0 * rDiscProxy && c.glowB < 2.0 * rDiscProxy) {
            out.treatedAsMoon = true;
            out.note = "bright disc treated as moon -- sun left unchanged";
            return out;
        }
        out.found = true;
        out.px = c.cx;
        out.py = c.cy;
        out.confidence = std::clamp(0.55 + 0.35 * c.fitR2, 0.0, 0.9);
        return out;
    }

    // Fallback: sun behind thin cloud -- the brightest sky blob with radial monotony but no
    // clipping. Threshold at a fixed fraction of the sky's own MAXIMUM (not a percentile).
    float maxLum = 0.0f;
    std::size_t maxAt = 0;
    for (std::size_t i = 0; i < n; ++i)
        if (sky[i] && p.valid[i] && p.lum[i] > maxLum) {
            maxLum = p.lum[i];
            maxAt = i;
        }
    if (maxLum <= 0.0f || skyMedianLum <= 0.0) return out;
    if (maxLum < 4.0 * skyMedianLum) return out;  // no distinct bright smear
    const float cut = 0.85f * maxLum;
    // Blob = connected bright region containing the maximum.
    std::vector<std::uint8_t> blob(n, 0);
    stack.assign(1, maxAt);
    blob[maxAt] = 1;
    double bx = 0, by = 0;
    int bn = 0;
    while (!stack.empty()) {
        const std::size_t i = stack.back();
        stack.pop_back();
        const int x = static_cast<int>(i % W), y = static_cast<int>(i / W);
        bx += x + 0.5;
        by += y + 0.5;
        ++bn;
        const int dx4[4] = {-1, 1, 0, 0}, dy4[4] = {0, 0, -1, 1};
        for (int k = 0; k < 4; ++k) {
            const int xx = x + dx4[k], yy = y + dy4[k];
            if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
            const std::size_t j = p.idx(xx, yy);
            if (!blob[j] && sky[j] && p.valid[j] && p.lum[j] >= cut) {
                blob[j] = 1;
                stack.push_back(j);
            }
        }
    }
    if (bn < 4 || bn > static_cast<int>(0.2 * static_cast<double>(skyPx))) return out;
    out.found = true;
    out.behindCloud = true;
    out.px = bx / bn;
    out.py = by / bn;
    out.confidence = 0.35;
    out.note = "sun position approximate (behind cloud)";
    return out;
}

// ---------------------------------------------------------------------------------------------
// S4: sky signature + forward-model probe match (analysis-by-synthesis against renderTexture,
// the Lalonde/Narasimhan/Efros idea fitted to OUR dome instead of the Perez model). Ratio and
// chromaticity features on purpose: a global von Kries white-balance shift moves chromaH and
// chromaZ together, so the difference and the luminance ratio survive unknown camera WB.
// Medians here are of the SKY-SIGNATURE bands (parameter matching), not horizon detection or
// segmentation -- outside that constraint's scope by construction.
// ---------------------------------------------------------------------------------------------

struct SkySig {
    bool ok = false;
    double hR = 0, hB = 0;  // horizon-band chroma medians
    double zR = 0, zB = 0;  // zenith-band chroma medians
    double logGrad = 0;     // log(medLum horizon / medLum zenith)
    double medLum = 0;      // median linear luminance of all sky pixels
    std::array<double, 16> satRow{};  // mean saturation per relative sky row bin
    std::array<int, 16> satRowN{};
    std::vector<float> colGlow;  // horizon-band luminance per column (twilight azimuth cue)
};

[[nodiscard]] double medianOf(std::vector<float>& v) {
    if (v.empty()) return 0.0;
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
}

// Extract the signature from any linear-RGB grid: (w, h), per-pixel accessor returning false
// for invalid pixels, and the per-column sky border (rows [0, border(x)) are sky).
template <typename PixelFn, typename BorderFn>
SkySig extractSignature(int w, int h, PixelFn&& pixel, BorderFn&& borderAt) {
    SkySig sig;
    std::vector<float> hLum, zLum, allLum, hCr, hCb, zCr, zCb;
    sig.colGlow.assign(static_cast<std::size_t>(w), 0.0f);
    const double hBand = 0.08 * h;
    for (int x = 0; x < w; ++x) {
        const int b = std::clamp(static_cast<int>(borderAt(x)), 0, h);
        const int zTop = static_cast<int>(0.15 * b);
        const int hTop = std::max(0, b - static_cast<int>(hBand));
        float glow = 0.0f;
        int glowN = 0;
        for (int y = 0; y < b; ++y) {
            double r, g, bl;
            if (!pixel(x, y, r, g, bl)) continue;
            const double L = linLum(r, g, bl);
            const double s = r + g + bl + 1e-9;
            allLum.push_back(static_cast<float>(L));
            const double mx = std::max({r, g, bl});
            const double mn = std::min({r, g, bl});
            const double sat = mx > 1e-6 ? (mx - mn) / mx : 0.0;
            const int bin = std::min(15, static_cast<int>(16.0 * y / std::max(1, b)));
            sig.satRow[bin] += sat;
            ++sig.satRowN[bin];
            if (y < zTop) {
                zLum.push_back(static_cast<float>(L));
                zCr.push_back(static_cast<float>(r / s));
                zCb.push_back(static_cast<float>(bl / s));
            }
            if (y >= hTop) {
                hLum.push_back(static_cast<float>(L));
                hCr.push_back(static_cast<float>(r / s));
                hCb.push_back(static_cast<float>(bl / s));
                glow += static_cast<float>(L);
                ++glowN;
            }
        }
        sig.colGlow[x] = glowN > 0 ? glow / glowN : 0.0f;
    }
    if (allLum.size() < 64 || hLum.size() < 16 || zLum.size() < 16) return sig;
    for (int k = 0; k < 16; ++k)
        if (sig.satRowN[k] > 0) sig.satRow[k] /= sig.satRowN[k];
    sig.hR = medianOf(hCr);
    sig.hB = medianOf(hCb);
    sig.zR = medianOf(zCr);
    sig.zB = medianOf(zCb);
    const double mh = medianOf(hLum);
    const double mz = medianOf(zLum);
    sig.logGrad = std::log(std::max(1e-6, mh) / std::max(1e-6, mz));
    sig.medLum = medianOf(allLum);
    sig.ok = true;
    return sig;
}

// The weighted signature distance of the design's §4.2, plus one GENTLE brightness-plausibility
// term (a calibration addition over the design draft, noted in the research doc): the chroma
// features are deliberately exposure-invariant, but a golden-hour sky at high turbidity and a
// civil-twilight sky at low turbidity are near-twins chromatically -- 2 EV of median-luminance
// disagreement breaks the tie without overruling a genuinely under/over-exposed photo.
[[nodiscard]] double sigDistance(const SkySig& a, const SkySig& b) {
    const double dHZr = (a.hR - a.zR) - (b.hR - b.zR);
    const double dHZb = (a.hB - a.zB) - (b.hB - b.zB);
    const double dZr = a.zR - b.zR;
    const double dZb = a.zB - b.zB;
    const double dG = a.logGrad - b.logGrad;
    const double dEv = std::log2(std::max(1e-6, a.medLum) / std::max(1e-6, b.medLum));
    return 4.0 * (dHZr * dHZr + dHZb * dHZb) + 1.0 * (dZr * dZr + dZb * dZb) +
           2.0 * dG * dG + 0.02 * dEv * dEv;
}

// Render one dome+haze probe and extract its signature. The probe camera carries the estimate's
// pitch/roll and the CURRENT fov at the document's aspect, so its horizon geometry matches the
// photo's; its own border comes from its own camera (self-consistent analysis-by-synthesis).
// `exposureEv` matters more than it looks: the display mapping's night floors (airglow, the
// twilight-glow fill) are constant in DISPLAY space, so a twilight dome's apparent gradient and
// chroma CHANGE with exposure -- a probe must be synthesized at the photo's implied exposure or
// it describes a different-looking sky (the two-step render in the matcher below).
struct ProbeSet {
    int w = 0, h = 0;
    SkyParams base{};
};

std::optional<SkySig> renderProbe(const ProbeSet& ps, double el, double turb, double exposureEv) {
    SkyParams s = ps.base;
    s.enableDome = true;
    s.enableHaze = true;
    s.enableClouds = false;
    s.enableSun = false;
    s.enableMoon = false;
    s.starsAmount = 0.0;
    s.exposure = exposureEv;
    s.sunAzimuthDeg = 180.0;
    s.sunElevationDeg = el;
    s.turbidity = turb;
    TextureParams tp;
    tp.generator = Generator::Sky;
    tp.seed = 1;
    tp.scale = 1.0;
    tp.spec = s;
    const TextureRenderResult r =
        renderTexture(tp, static_cast<std::uint32_t>(ps.w), static_cast<std::uint32_t>(ps.h));
    if (!r.imageF || r.imageF->empty()) return std::nullopt;
    const common::ImageF& img = *r.imageF;
    const auto hz = modelHorizonLine(s, img.width, img.height);
    const double y0 = hz ? hz->yAt0 : ps.h;
    const double y1 = hz ? hz->yAtW : ps.h;
    const auto borderAt = [&](int x) {
        const double t = (x + 0.5) / ps.w;
        return std::clamp(y0 + (y1 - y0) * t, 0.0, static_cast<double>(ps.h));
    };
    const auto pixel = [&](int x, int y, double& rr, double& gg, double& bb) {
        const common::ColorF c = img.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
        rr = srgbDecode(std::max(0.0f, c.r));
        gg = srgbDecode(std::max(0.0f, c.g));
        bb = srgbDecode(std::max(0.0f, c.b));
        return c.a > 0.5f;
    };
    SkySig sig = extractSignature(ps.w, ps.h, pixel, borderAt);
    if (!sig.ok) return std::nullopt;
    return sig;
}

// ---------------------------------------------------------------------------------------------
// Small formatting helpers for the honesty notes (ASCII only).
// ---------------------------------------------------------------------------------------------

[[nodiscard]] std::string hhmm(double hourUtc) {
    double h = std::fmod(hourUtc, 24.0);
    if (h < 0.0) h += 24.0;
    int hi = static_cast<int>(h);
    int mi = static_cast<int>(std::lround((h - hi) * 60.0));
    if (mi == 60) {
        mi = 0;
        hi = (hi + 1) % 24;
    }
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", hi, mi);
    return buf;
}

void appendLine(std::string& s, const std::string& line) {
    if (line.empty()) return;
    if (!s.empty()) s += '\n';
    s += line;
}

bool cancelled(const SkyEstimateProgress* prog) {
    return prog != nullptr && prog->cancel.load(std::memory_order_relaxed);
}


void mark(SkyEstimateProgress* prog, std::uint32_t permille) {
    if (prog != nullptr) prog->permille.store(permille, std::memory_order_relaxed);
}

}  // namespace

// -------------------------------------------------------------------------------------------------
// S5: almanac inversion. The general engine is sunEventsAtAltitude (sky_almanac.hpp); this adds
// the design's picking policy: nearest valid solution to the current clock, ties to afternoon,
// unreachable elevations clamp to solar noon, polar days clamp with a note.
// -------------------------------------------------------------------------------------------------
SkyTimeInversion invertTimeFromElevation(const UtcTime& date, double latitudeDeg,
                                         double longitudeDeg, double elevationDeg,
                                         double currentHourUtc) {
    SkyTimeInversion out;
    const RiseSetTimes day = sunRiseSetTimes(date, latitudeDeg, longitudeDeg);
    const double elMax = day.transitAltitudeDeg;
    if (elevationDeg > elMax - 0.5) {
        out.valid = true;
        out.hourUtc = day.transit.hourUtc;
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "sun never reaches %.1f deg here on this date; set to solar noon (%.1f deg)",
                      elevationDeg, elMax);
        out.note = buf;
        return out;
    }
    const RiseSetTimes ev = sunEventsAtAltitude(date, latitudeDeg, longitudeDeg, elevationDeg);
    if (ev.rise.alwaysDown || ev.set.alwaysDown) {
        out.valid = true;
        out.hourUtc = day.transit.hourUtc;
        out.note = "polar night: the sun stays below that elevation all day; set to solar noon";
        return out;
    }
    if (ev.rise.alwaysUp || ev.set.alwaysUp) {
        out.valid = true;
        out.hourUtc = day.transit.hourUtc;
        out.note = "midnight sun: the sun stays above that elevation all day; set to solar noon";
        return out;
    }
    const bool riseOk = ev.rise.valid, setOk = ev.set.valid;
    if (!riseOk && !setOk) {
        out.valid = true;
        out.hourUtc = day.transit.hourUtc;
        out.note = "no crossing of that elevation this UTC day; set to solar noon";
        return out;
    }
    if (riseOk && setOk) {
        const auto dist = [&](double h) {
            double d = std::abs(h - currentHourUtc);
            return std::min(d, 24.0 - d);
        };
        const bool pickSet = dist(ev.set.hourUtc) <= dist(ev.rise.hourUtc);  // ties -> afternoon
        out.valid = true;
        out.hourUtc = pickSet ? ev.set.hourUtc : ev.rise.hourUtc;
        out.hasAlternative = true;
        out.alternativeHourUtc = pickSet ? ev.rise.hourUtc : ev.set.hourUtc;
        out.note = std::string(pickSet ? "afternoon assumed -- morning equivalent "
                                       : "morning assumed -- afternoon equivalent ") +
                   hhmm(out.alternativeHourUtc) + " UTC";
        return out;
    }
    out.valid = true;
    out.hourUtc = riseOk ? ev.rise.hourUtc : ev.set.hourUtc;
    out.note = riseOk ? "morning crossing (the afternoon one falls on a neighbouring UTC day)"
                      : "afternoon crossing (the morning one falls on a neighbouring UTC day)";
    return out;
}

// -------------------------------------------------------------------------------------------------
// The S0-S5 orchestrator.
// -------------------------------------------------------------------------------------------------
SkyEstimateResult estimateSkyFromLayer(const common::Image& photo,
                                       const SkyEstimateOptions& options,
                                       SkyEstimateProgress* progress) {
    SkyEstimateResult res;
    res.params = options.current;
    // Lens metadata is a measurement: the WHOLE pipeline (horizon inversion, probe cameras, sun
    // mapping) runs at the photo's real FOV, not the generator's current guess (design §3.2).
    if (options.fovDegFromExif.has_value()) {
        res.params.fovDeg = std::clamp(*options.fovDegFromExif, 10.0, 150.0);
        res.fov = {res.params.fovDeg, 1.0, true, "from lens metadata"};
    }

    // ---- S0: proxies ----
    const Proxy fine = buildProxy(photo, 1024);
    const Proxy coarse = buildProxy(photo, 256);
    if (!fine.ok() || !coarse.ok()) {
        res.aborted = true;
        res.summary = "The layer has no usable pixels. Settings unchanged.";
        return res;
    }
    mark(progress, 100);
    if (cancelled(progress)) {
        res.cancelled = true;
        return res;
    }

    const std::uint32_t docW = photo.width, docH = photo.height;

    // ---- S1 + S2a: horizon from the coarse whole-frame DP border (primary) + Sobel/Hough
    //      cross-check, fused per Gershikov's finding (position from the border, tilt from the
    //      edge method). ----
    const SkyColorModel coarseModel = fitColorModel(coarse);
    std::vector<float> coarseP = priorMap(coarse, coarseModel, nullptr, nullptr);
    // The HORIZON pass adds a lobe-free structural term: top-connected smoothness (the
    // gradient-domain border idea of Shen & Wang 2013). A twilight sky sweeps a chroma arc no
    // two-lobe model spans -- dim gray zenith through deep orange glow -- but it is SMOOTH all
    // the way down to the horizon, and the textured ground below is not. Per column, the
    // cumulative gradient maximum from the top row scores that property; purely gradient-based,
    // no intensity distribution, no threshold on intensity.   Blended into the coarse DP
    // input ONLY -- the segmentation prior (gates, S6) stays the color model's business.
    {
        constexpr double g1 = 0.010;
        for (int x = 0; x < coarse.w; ++x) {
            double cm = 0.0;
            for (int y = 0; y < coarse.h; ++y) {
                const std::size_t i = coarse.idx(x, y);
                cm = std::max(cm, static_cast<double>(coarse.grad7[i]));
                if (!coarse.valid[i]) continue;
                const double r = cm / g1;
                const float smooth = static_cast<float>(0.85 * std::exp(-r * r));
                coarseP[i] = std::max(coarseP[i], smooth);
            }
        }
    }
    std::vector<int> cy0(coarse.w, 0), cy1(coarse.w, coarse.h);
    const DpResult coarseDp = dpBorder(coarse, coarseP, cy0, cy1);
    double coarseSkyFrac = 0.0;
    for (int x = 0; x < coarse.w; ++x) coarseSkyFrac += coarseDp.border[x];
    coarseSkyFrac /= static_cast<double>(coarse.w) * coarse.h;
    bool skyOnly = coarseSkyFrac > 0.92;
    bool horizonApplied = false;
    double confHorizon = 0.0;
    double fitM = 0.0, fitB = 0.0;  // fused horizon line, DOC coordinates
    bool haveLine = false;
    std::string horizonNote;

    if (!skyOnly) {
        LineFit primary = fitBorderLine(coarse, coarseDp.border, 0x5EED0001ull);
        CrossCheck cross = sobelHoughLine(fine, 0x5EED0002ull);
        // Convert both to document coordinates (slope survives the uniform proxy scale).
        double pM = 0, pB = 0, cM = 0, cB = 0;
        if (primary.ok) {
            // y_doc = offY + (m * x_p + b) * k with x_p = (x_doc - offX) / k, so the slope
            // survives the uniform scale and the intercept re-anchors to document space.
            pM = primary.m;
            pB = coarse.offY + primary.b * coarse.scale - primary.m * coarse.offX;
        }
        if (cross.ok) {
            cM = cross.m;
            cB = fine.offY + cross.b * fine.scale - cross.m * fine.offX;
        }
        const double yc = 0.5 * docW;  // compare at the frame centre column
        double agreement = 0.0;
        if (primary.ok && cross.ok) {
            const double dv = std::abs((pM * yc + pB) - (cM * yc + cB));
            const double dt = std::abs(std::atan(pM) - std::atan(cM)) * kRadToDeg;
            agreement = std::exp(-(dv / (0.03 * docH)) * (dv / (0.03 * docH)) -
                                 (dt / 2.0) * (dt / 2.0));
            if (dv < 0.03 * docH && dt < 2.0) {
                fitM = cM;  // tilt from the edge method (angle-accurate)
                fitB = (pM * yc + pB) - cM * yc;  // position from the border (position-accurate)
                haveLine = true;
            } else {
                fitM = pM;
                fitB = pB;
                haveLine = true;
            }
        } else if (primary.ok) {
            fitM = pM;
            fitB = pB;
            haveLine = true;
        } else if (cross.ok) {
            fitM = cM;
            fitB = cB;
            haveLine = true;
        }

        if (haveLine) {
            // Sanity gates on the coarse proxy split (proxy-coord line).
            const double gm = fitM;
            const double gb = (fitB - coarse.offY + fitM * coarse.offX) / coarse.scale;
            const Gates gates = skyGates(coarse, gm, gb);
            if (gates.hardFail) {
                haveLine = false;
                horizonNote = "No horizon found -- camera left unchanged.";
            } else {
                const double margin = primary.ok ? primary.margin : 0.0;
                const double inlier = cross.ok ? cross.inlierFrac : 0.0;
                confHorizon = 0.35 * margin + 0.2 * inlier + 0.25 * agreement +
                              0.2 * (gates.passed / 3.0);
                if (primary.ok && cross.ok && agreement < 0.05)
                    confHorizon = std::min(confHorizon, 0.45);
                if (primary.ragged) {
                    confHorizon = std::min(confHorizon, 0.6);
                    horizonNote = "Skyline used as horizon (ragged border).";
                }
            }
        } else {
            horizonNote = "No horizon found -- camera left unchanged.";
        }
    } else {
        horizonNote = "All sky -- horizon left unchanged.";
    }

    // Camera mapping (closed form + Gauss-Newton polish against SkyCamera::project).
    if (haveLine && confHorizon >= 0.4) {
        const double vh = (fitM * (0.5 * docW) + fitB) / docH;
        const double halfTanX = std::tan(std::clamp(res.params.fovDeg, 10.0, 150.0) * 0.5 *
                                         kDegToRad);
        const double halfTanY = halfTanX * (docW > 0 ? static_cast<double>(docH) / docW : 1.0);
        SkyParams cam = res.params;  // == options.current, plus any lens-metadata FOV
        cam.shiftY = 0.0;  // one measured DOF: pitch is primary, the tilt-shift stays manual
        cam.pitchDeg = std::atan((2.0 * vh - 1.0) * halfTanY) * kRadToDeg;
        cam.rollDeg = std::atan(fitM) * kRadToDeg;  // clockwise-positive, sign pinned by test
        refinePitchRoll(cam, docW, docH, fitB, fitM * docW + fitB);
        res.pitch = {cam.pitchDeg, confHorizon, true,
                     confHorizon < 0.65 ? "low confidence" : ""};
        res.roll = {cam.rollDeg, confHorizon, true, res.pitch.note};
        res.params.pitchDeg = cam.pitchDeg;
        res.params.rollDeg = cam.rollDeg;
        res.params.shiftY = 0.0;
        horizonApplied = true;
        if (confHorizon < 0.65) appendLine(res.summary, "Horizon applied (low confidence).");
    } else {
        res.pitch = {options.current.pitchDeg, confHorizon, false, horizonNote};
        res.roll = {options.current.rollDeg, confHorizon, false, horizonNote};
        appendLine(res.summary, horizonNote);
    }
    mark(progress, 250);
    if (cancelled(progress)) {
        res.cancelled = true;
        return res;
    }

    // ---- S2b: segmentation-grade prior + banded DP border on the fine proxy. ----
    const SkyColorModel model = fitColorModel(fine);
    res.colorModel = model;
    std::optional<double> lineMf, lineBf;
    std::vector<int> fy0(fine.w, 0), fy1(fine.w, fine.h);
    const std::vector<float>* Pp = nullptr;
    std::vector<float> fineP;
    // Band the segmentation border around the horizon only when the line CLEARED its floor --
    // banding around a rejected line would force a phantom border through an image that has
    // none (the design's "whole frame if S1 failed").
    if (haveLine && confHorizon >= 0.4) {
        const double m = fitM;
        const double b = (fitB - fine.offY + fitM * fine.offX) / fine.scale;
        lineMf = m;
        lineBf = b;
        fineP = priorMap(fine, model, &*lineMf, &*lineBf);
        for (int x = 0; x < fine.w; ++x) {
            const double hz = m * (x + 0.5) + b;
            fy0[x] = std::clamp(static_cast<int>(hz - 0.35 * fine.h), 0, fine.h);
            fy1[x] = std::clamp(static_cast<int>(hz + 0.35 * fine.h), 0, fine.h);
        }
    } else {
        fineP = priorMap(fine, model, nullptr, nullptr);
    }
    Pp = &fineP;
    DpResult dp = dpBorder(fine, fineP, fy0, fy1);
    if (skyOnly) std::fill(dp.border.begin(), dp.border.end(), fine.h);

    res.proxyW = fine.w;
    res.proxyH = fine.h;
    res.proxyScale = fine.scale;
    res.proxyOffX = fine.offX;
    res.proxyOffY = fine.offY;
    res.borderRows.assign(fine.w, 0.0f);
    double skyFrac = 0.0;
    for (int x = 0; x < fine.w; ++x) {
        res.borderRows[x] = static_cast<float>(dp.border[x]);
        skyFrac += dp.border[x];
    }
    skyFrac /= static_cast<double>(fine.w) * fine.h;
    res.skyFraction = skyFrac;

    // §5.3 gates.
    double pIn = 0.0, pOut = 0.0;
    std::size_t nIn = 0, nOut = 0;
    for (int x = 0; x < fine.w; ++x)
        for (int y = 0; y < fine.h; ++y) {
            const std::size_t i = fine.idx(x, y);
            if (!fine.valid[i]) continue;
            if (y < dp.border[x]) {
                pIn += (*Pp)[i];
                ++nIn;
            } else {
                pOut += (*Pp)[i];
                ++nOut;
            }
        }
    pIn = nIn > 0 ? pIn / nIn : 0.0;
    pOut = nOut > 0 ? pOut / nOut : 0.0;
    int goodCols = 0;
    for (int x = 0; x < fine.w; ++x)
        if (dp.quality[x] > 0.35f) ++goodCols;
    const double fracGood = static_cast<double>(goodCols) / std::max(1, fine.w);
    res.segConfidence = clamp01(0.5 * clamp01((pIn - pOut) / 0.5) + 0.5 * fracGood);
    if (skyOnly) {
        res.segmentationUsable = true;
        res.segmentationNote = "all sky -- the mask covers everything";
    } else if (skyFrac < 0.02) {
        res.segmentationUsable = false;
        res.segmentationNote = "no sky found -- nothing to mask";
    } else if (skyFrac > 0.98) {
        res.segmentationUsable = true;
        res.segmentationNote = "all sky -- the mask covers everything";
    } else if (pIn < 0.6 || pOut > 0.35) {
        res.segmentationUsable = false;
        res.segmentationNote = "sky/foreground separation too weak for a reliable mask";
    } else {
        res.segmentationUsable = true;
        if (fracGood < 0.7)
            res.segmentationNote = "mask will be approximate around fine structures";
    }

    // The whole-estimate abort: no horizon AND essentially nothing sky-like. The fraction test
    // uses the UNBANDED coarse border (a rejected line must not manufacture a fraction), the
    // pIn test catches a banded border forced through non-sky content.
    if (!horizonApplied && !skyOnly && confHorizon < 0.4 &&
        (coarseSkyFrac < 0.02 || pIn < 0.15)) {
        res.aborted = true;
        res.summary = "No sky or horizon found in the layer. Settings unchanged.";
        return res;
    }
    mark(progress, 450);
    if (cancelled(progress)) {
        res.cancelled = true;
        return res;
    }

    // ---- S3: sun disc. ----
    // Median sky luminance (needed by both S3's night gate and S4's exposure match).
    {
        std::vector<float> skyLum;
        for (int x = 0; x < fine.w; ++x)
            for (int y = 0; y < dp.border[x]; ++y) {
                const std::size_t i = fine.idx(x, y);
                if (fine.valid[i]) skyLum.push_back(fine.lum[i]);
            }
        res.photoSkyMedianLum = medianOf(skyLum);
    }
    const double halfTanXDoc =
        std::tan(std::clamp(res.params.fovDeg, 10.0, 150.0) * 0.5 * kDegToRad);
    const double fPxProxy = 0.5 * fine.w / halfTanXDoc;
    const double rDiscProxy = 0.00445 * fPxProxy;
    SunFind sun = detectSun(fine, dp.border, rDiscProxy, res.photoSkyMedianLum);
    if (sun.found && !horizonApplied) {
        // The disc position maps through the CURRENT generator camera (no measured horizon to
        // anchor one): the direction is only as right as that guess, so say so.
        sun.confidence = std::min(sun.confidence, 0.45);
        sun.note = sun.note.empty() ? "camera unknown -- sun position approximate"
                                    : sun.note + "; camera unknown";
    }
    double sunElMeasured = 0.0, sunAzMeasured = 0.0;
    bool sunMeasured = false;
    if (sun.found) {
        const SkyCamera cam = SkyCamera::fromParams(res.params, docW, docH);
        const double dx = fine.offX + sun.px * fine.scale;
        const double dy = fine.offY + sun.py * fine.scale;
        const SkyVec3 d = cam.rayAt(dx, dy);
        sunElMeasured = std::asin(std::clamp(d.z, -1.0, 1.0)) * kRadToDeg;
        sunAzMeasured = std::atan2(d.x, d.y) * kRadToDeg;
        if (sunAzMeasured < 0.0) sunAzMeasured += 360.0;
        sunMeasured = true;
    }
    if (!sun.note.empty()) appendLine(res.summary, "Sun: " + sun.note + ".");
    mark(progress, 600);
    if (cancelled(progress)) {
        res.cancelled = true;
        return res;
    }

    // ---- S4: signature + probe match. ----
    const auto photoPixel = [&](int x, int y, double& r, double& g, double& b) {
        const std::size_t i = fine.idx(x, y);
        if (!fine.valid[i]) return false;
        r = fine.rgb[i * 3];
        g = fine.rgb[i * 3 + 1];
        b = fine.rgb[i * 3 + 2];
        return true;
    };
    const auto photoBorder = [&](int x) { return static_cast<double>(dp.border[x]); };
    const SkySig photoSig = extractSignature(fine.w, fine.h, photoPixel, photoBorder);

    double elEst = res.params.sunElevationDeg;
    double turbEst = res.params.turbidity;
    double confMatch = 0.0;
    double exposureEv = 0.0;
    bool haveMatch = false;
    bool middayBucket = false;
    double cloudyFrac = 0.0;
    bool overcast = false;

    if (photoSig.ok && !sun.overexposed) {
        ProbeSet ps;
        ps.w = 64;
        ps.h = std::clamp(static_cast<int>(std::lround(64.0 * docH / std::max(1u, docW))), 32, 96);
        ps.base = res.params;
        static constexpr double kEls[] = {-18, -15, -12, -9, -6, -4, -2, 0,
                                          2,   5,   10,  15, 20, 30, 45, 60};
        static constexpr double kTurbs[] = {1.5, 2.5, 4.0, 6.0, 9.0};
        const int nEl = static_cast<int>(std::size(kEls));
        const int nTb = static_cast<int>(std::size(kTurbs));

        std::vector<double> els;
        if (sunMeasured && !sun.behindCloud) {
            els.assign(1, sunElMeasured);
        } else {
            els.assign(kEls, kEls + nEl);
        }
        struct ProbeD {
            double el, turb, D, medLum, ev;
            SkySig sig;
        };
        std::vector<ProbeD> probes;
        probes.reserve(els.size() * nTb);
        const std::uint32_t p0 = 600, p1 = 900;
        int done = 0;
        const int total = static_cast<int>(els.size()) * nTb;
        bool bailed = false;
        for (const double el : els) {
            for (int ti = 0; ti < nTb && !bailed; ++ti) {
                if (cancelled(progress)) {
                    res.cancelled = true;
                    return res;
                }
                // Two-step exposure-consistent synthesis: a first render at 0 EV implies the
                // exposure that would make this candidate as bright as the photo; the probe is
                // then re-rendered AT that exposure, because the night floors make a twilight
                // dome's very structure exposure-dependent (see renderProbe). Day probes match
                // near 0 EV and skip the second render.
                auto sig = renderProbe(ps, el, kTurbs[ti], 0.0);
                double ev = 0.0;
                if (sig) {
                    ev = std::clamp(std::log2(std::max(1e-6, photoSig.medLum) /
                                              std::max(1e-6, sig->medLum)),
                                    -6.0, 6.0);
                    if (std::abs(ev) > 0.5) {
                        if (cancelled(progress)) {
                            res.cancelled = true;
                            return res;
                        }
                        sig = renderProbe(ps, el, kTurbs[ti], ev);
                        if (!sig) ev = 0.0;
                    } else {
                        ev = 0.0;
                    }
                }
                ++done;
                mark(progress, p0 + (p1 - p0) * done / std::max(1, total));
                if (!sig) continue;
                probes.push_back(
                    {el, kTurbs[ti], sigDistance(photoSig, *sig), sig->medLum, ev, *sig});
            }
        }
        if (probes.size() >= 3) {
            std::size_t best = 0;
            for (std::size_t i = 1; i < probes.size(); ++i)
                if (probes[i].D < probes[best].D) best = i;
            // Runner-up = the best probe of a DIFFERENT family along the applied axis: another
            // elevation on the full grid, another turbidity when the sun pinned the elevation.
            // (Turbidity neighbours at one elevation are near-twins by physics -- twilight
            // barely sees aerosols -- and must not read as ambiguity about the elevation.)
            const bool elAxis = els.size() > 1;
            double dRunner = std::numeric_limits<double>::max();
            for (std::size_t i = 0; i < probes.size(); ++i) {
                if (i == best) continue;
                const bool adjacent = elAxis
                                          ? std::abs(probes[i].el - probes[best].el) <= 3.01
                                          : std::abs(probes[i].turb - probes[best].turb) <= 1.51;
                if (!adjacent) dRunner = std::min(dRunner, probes[i].D);
            }
            elEst = probes[best].el;
            turbEst = probes[best].turb;
            // 3-point parabolic refine along el at the winning turbidity.
            if (!sunMeasured || sun.behindCloud) {
                const ProbeD* lo = nullptr;
                const ProbeD* hi = nullptr;
                for (const ProbeD& q : probes) {
                    if (std::abs(q.turb - probes[best].turb) > 1e-9) continue;
                    if (q.el < probes[best].el && (lo == nullptr || q.el > lo->el)) lo = &q;
                    if (q.el > probes[best].el && (hi == nullptr || q.el < hi->el)) hi = &q;
                }
                if (lo != nullptr && hi != nullptr) {
                    const double d0 = lo->D, d1 = probes[best].D, d2 = hi->D;
                    const double denom = d0 - 2.0 * d1 + d2;
                    if (denom > 1e-12) {
                        const double t = 0.5 * (d0 - d2) / denom;  // in (-1, 1) grid steps
                        const double stepLo = probes[best].el - lo->el;
                        const double stepHi = hi->el - probes[best].el;
                        elEst = probes[best].el + std::clamp(t, -1.0, 1.0) *
                                                       (t < 0.0 ? stepLo : stepHi);
                    }
                }
            }
            const double margin =
                dRunner < std::numeric_limits<double>::max()
                    ? clamp01(1.0 - probes[best].D / std::max(1e-9, dRunner))
                    : 0.5;
            // Margin (peakedness over the non-adjacent runner-up) carries the discrimination;
            // the absolute-D factor only guards against "best of a uniformly bad grid" (a photo
            // unlike ANY sky). A twilight match legitimately lands near D ~ 0.2 -- the photo's
            // 8-bit quantization floor -- so the guard's scale is deliberately soft.
            confMatch = margin * std::exp(-probes[best].D / 0.5);
            // The winning probe already carries most of the photo's exposure; the residual
            // median-luminance ratio tops it up.
            // Clamp widened from the design draft's +4: a night photo exposed up needs the
            // whole +-6 EV range once the probes synthesize exposure (research doc §4.2 note).
            exposureEv = std::clamp(
                probes[best].ev + std::log2(std::max(1e-6, photoSig.medLum) /
                                            std::max(1e-6, probes[best].medLum)),
                -6.0, 6.0);
            haveMatch = true;

            // Cloud coverage: sky pixels whose chromaticity sits nearer the neutral axis than
            // the matched clear-sky model predicts at their relative row.
            const SkySig& msig = probes[best].sig;
            std::size_t cloudy = 0, total2 = 0;
            for (int x = 0; x < fine.w; ++x) {
                const int b = dp.border[x];
                for (int y = 0; y < b; ++y) {
                    const std::size_t i = fine.idx(x, y);
                    if (!fine.valid[i]) continue;
                    const double r = fine.rgb[i * 3], g = fine.rgb[i * 3 + 1],
                                 bl = fine.rgb[i * 3 + 2];
                    const double mx = std::max({r, g, bl}), mn = std::min({r, g, bl});
                    const double sat = mx > 1e-6 ? (mx - mn) / mx : 0.0;
                    const int bin = std::min(15, static_cast<int>(16.0 * y / std::max(1, b)));
                    const double ref = msig.satRowN[bin] > 0 ? msig.satRow[bin] : 0.0;
                    ++total2;
                    if (ref > 0.02 && sat < 0.55 * ref) ++cloudy;
                }
            }
            cloudyFrac = total2 > 0 ? static_cast<double>(cloudy) / total2 : 0.0;
            overcast = cloudyFrac > 0.85 && std::abs(photoSig.logGrad) < 0.35;

            if (!sunMeasured && elEst > 30.0) {
                middayBucket = true;
                elEst = 35.0;
                confMatch = std::min(confMatch, 0.5);
            }
            if (overcast && !sunMeasured) {
                // An overcast signature is nearly elevation-invariant: degrade to a coarse
                // day / twilight / night luminance bucket and say so.
                if (res.photoSkyMedianLum >= 0.08)
                    elEst = 25.0;
                else if (res.photoSkyMedianLum >= 0.004)
                    elEst = -4.0;
                else
                    elEst = -15.0;
                confMatch = std::min(confMatch, 0.3);
                appendLine(res.summary,
                           "Overcast sky: time of day is a coarse brightness guess.");
            }
        }
    }

    // ---- Sun / time application. ----
    double appliedEl = res.params.sunElevationDeg;
    bool elApplied = false;
    if (sunMeasured && sun.confidence >= 0.4) {
        appliedEl = sunElMeasured;
        res.sunElevation = {sunElMeasured, sun.confidence, true, sun.note};
        res.sunAzimuth = {sunAzMeasured, sun.confidence, true, sun.note};
        elApplied = true;
    } else if (haveMatch && confMatch >= 0.35) {
        appliedEl = elEst;
        res.sunElevation = {elEst, confMatch, true,
                            middayBucket ? "midday bucket -- the clear-sky look is nearly "
                                           "elevation-invariant above 30 deg"
                                         : ""};
        elApplied = true;
        // Twilight azimuth cue: the horizon-band glow centroid points at the sub-horizon sun.
        if (!sunMeasured && elEst < 10.0 && photoSig.ok && !photoSig.colGlow.empty()) {
            float mx = 0.0f;
            double meanG = 0.0;
            int gxAt = -1;
            for (int x = 0; x < fine.w; ++x) {
                meanG += photoSig.colGlow[x];
                if (photoSig.colGlow[x] > mx) {
                    mx = photoSig.colGlow[x];
                    gxAt = x;
                }
            }
            meanG /= std::max(1, fine.w);
            if (gxAt >= 0 && meanG > 1e-6 && mx > 1.3 * meanG) {
                const SkyCamera cam = SkyCamera::fromParams(res.params, docW, docH);
                const double dx = fine.offX + (gxAt + 0.5) * fine.scale;
                const double dy =
                    fine.offY + std::clamp<double>(dp.border[gxAt] - 1, 0, fine.h) * fine.scale;
                const SkyVec3 d = cam.rayAt(dx, dy);
                double az = std::atan2(d.x, d.y) * kRadToDeg;
                if (az < 0.0) az += 360.0;
                res.sunAzimuth = {az, 0.4, true, "azimuth approximate (from horizon glow)"};
                sunAzMeasured = az;
                sunMeasured = true;  // frame-position knowledge for the notes below
            }
        }
    } else {
        res.sunElevation = {elEst, std::max(confMatch, sun.confidence), false,
                            "sun position not measurable -- left unchanged"};
        res.sunAzimuth = res.sunElevation;
        appendLine(res.summary, "Sun position left unchanged.");
    }

    if (elApplied) {
        if (options.dateAndPlaceMode) {
            // S5: elevation -> clock time via the almanac; the master clock then stamps a
            // coherent sun + moon. Framing note: the ephemeris sun follows the CLOCK, not the
            // photo's framing (the generator's camera faces due south by convention).
            const UtcTime date{res.params.obsYear, res.params.obsMonth, res.params.obsDay,
                               res.params.obsHourUtc};
            const SkyTimeInversion inv =
                invertTimeFromElevation(date, res.params.obsLatitudeDeg,
                                        res.params.obsLongitudeDeg, appliedEl,
                                        res.params.obsHourUtc);
            if (inv.valid) {
                applyMasterClock(res.params, {date.year, date.month, date.day, inv.hourUtc},
                                 res.params.obsLatitudeDeg, res.params.obsLongitudeDeg);
                res.timeUtc = {inv.hourUtc, res.sunElevation.confidence, true, inv.note};
                std::string line = "Time set to " + hhmm(inv.hourUtc) + " UTC";
                if (!inv.note.empty()) line += " (" + inv.note + ")";
                appendLine(res.summary, line + ".");
                if (sunMeasured) {
                    const double off = sunAzMeasured - 180.0;
                    if (std::abs(off) > 3.0) {
                        char buf[160];
                        std::snprintf(buf, sizeof(buf),
                                      "Sun placed by date & place; the photo's sun sat %.0f deg "
                                      "%s of centre -- switch to manual sun to match framing "
                                      "exactly.",
                                      std::abs(off), off < 0.0 ? "left" : "right");
                        appendLine(res.summary, buf);
                    }
                }
            }
        } else {
            res.params.sunElevationDeg = appliedEl;
            if (res.sunAzimuth.applied) res.params.sunAzimuthDeg = res.sunAzimuth.value;
        }
    }

    // ---- Turbidity / exposure / coverage. ----
    if (haveMatch && confMatch >= 0.35) {
        res.turbidity = {turbEst, confMatch, true, ""};
        res.params.turbidity = std::clamp(turbEst, 1.0, 10.0);
        res.exposure = {exposureEv, confMatch, true, ""};
        res.params.exposure = exposureEv;
        res.photoElevationForMatch = elApplied ? appliedEl : elEst;
        res.photoTurbidityForMatch = turbEst;
        const double coverage =
            overcast ? 0.95 : clamp01(cloudyFrac * 1.1);
        if (res.segmentationUsable) {
            res.cloudCoverage = {coverage, overcast ? 0.6 : 0.5, true, ""};
            res.params.cloudCoverage = coverage;
        } else {
            res.cloudCoverage = {coverage, 0.2, false, "cloud coverage needs a usable sky mask"};
        }
    } else {
        res.turbidity = {res.params.turbidity, confMatch, false, "sky match too weak"};
        res.exposure = {res.params.exposure, confMatch, false, "sky match too weak"};
        res.cloudCoverage = {res.params.cloudCoverage, confMatch, false, "sky match too weak"};
        if (photoSig.ok && !sun.overexposed)
            appendLine(res.summary, "Sky colour match too weak -- atmosphere left unchanged.");
        res.photoElevationForMatch = appliedEl;
        res.photoTurbidityForMatch = res.params.turbidity;
    }

    if (options.datePlaceFromExif)
        appendLine(res.summary, "Date & place read from the photo's metadata.");
    if (res.fov.applied) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Field of view %.0f deg (lens metadata).",
                      res.params.fovDeg);
        appendLine(res.summary, buf);
    } else {
        appendLine(res.summary, "Field of view unchanged (no lens metadata).");
    }
    if (!res.segmentationNote.empty()) appendLine(res.summary, "Mask: " + res.segmentationNote + ".");
    mark(progress, 1000);
    return res;
}

// -------------------------------------------------------------------------------------------------
// S6: full-resolution sky segmentation (design §5.2). Border upsample + seeded color floods (the
// magic wand's metric seam, wandColorDistance, with a lazy scanline flood so cost tracks the sky
// area) + prior reconcile + the holes policy, finished with Selection::smoothed / ::feathered --
// distance-transform + Gaussian only, no contour/convex-hull pipeline, no
// histogram or global intensity threshold.  
// -------------------------------------------------------------------------------------------------
Selection skySelectionFromEstimate(const common::Image& photo, const SkyEstimateResult& estimate,
                                   SkyEstimateProgress* progress, std::string* noteOut) {
    const auto fail = [&](const char* why) {
        if (noteOut != nullptr) *noteOut = why;
        return Selection{};
    };
    if (photo.empty()) return fail("no pixels");
    if (!estimate.segmentationUsable || estimate.borderRows.empty())
        return fail("Couldn't isolate the sky -- mask & harmonize skipped");
    const int W = static_cast<int>(photo.width);
    const int H = static_cast<int>(photo.height);
    const std::size_t N = static_cast<std::size_t>(W) * H;

    // The design's sky-only branch: an all-sky photo masks EVERYTHING (there is no boundary to
    // trace, and floods anchored to bright seeds would honestly under-cover a graded dome).
    if (estimate.skyFraction > 0.98) {
        Selection all(photo.width, photo.height);
        std::fill(all.data().begin(), all.data().end(), std::uint8_t{255});
        mark(progress, 1000);
        return all;
    }

    // The proxy border, upsampled linearly to full width in document coordinates.
    std::vector<float> borderDoc(static_cast<std::size_t>(W));
    {
        const int pw = static_cast<int>(estimate.borderRows.size());
        for (int x = 0; x < W; ++x) {
            const double xp = (x + 0.5 - estimate.proxyOffX) / estimate.proxyScale - 0.5;
            const int i0 = std::clamp(static_cast<int>(std::floor(xp)), 0, pw - 1);
            const int i1 = std::min(i0 + 1, pw - 1);
            const double t = clamp01(xp - i0);
            const double b =
                estimate.borderRows[i0] + (estimate.borderRows[i1] - estimate.borderRows[i0]) * t;
            borderDoc[x] =
                static_cast<float>(estimate.proxyOffY + b * estimate.proxyScale);
        }
    }
    mark(progress, 50);
    if (cancelled(progress)) return fail("cancelled");

    // ---- Full-resolution prior P (quantized to bytes; thresholds 0.8 -> 204, 0.9 -> 230). ----
    const auto& lut = srgbByteLut();
    std::vector<float> lumEnc(N), grad(N), tmp(N);
    parallelRows(static_cast<std::size_t>(H), [&](std::size_t r0, std::size_t r1) {
        for (std::size_t y = r0; y < r1; ++y) {
            const std::uint8_t* row = photo.rgba.data() + y * W * 4;
            for (int x = 0; x < W; ++x)
                lumEnc[y * W + x] = static_cast<float>(
                    (0.2126 * row[x * 4] + 0.7152 * row[x * 4 + 1] + 0.0722 * row[x * 4 + 2]) /
                    255.0);
        }
    });
    parallelRows(static_cast<std::size_t>(H), [&](std::size_t r0, std::size_t r1) {
        for (std::size_t y = r0; y < r1; ++y)
            for (int x = 0; x < W; ++x) {
                const int xm = std::max(0, x - 1), xp = std::min(W - 1, x + 1);
                const std::size_t ym = y > 0 ? y - 1 : 0;
                const std::size_t yp = y + 1 < static_cast<std::size_t>(H) ? y + 1 : y;
                grad[y * W + x] = 0.5f * (std::abs(lumEnc[y * W + xp] - lumEnc[y * W + xm]) +
                                          std::abs(lumEnc[yp * W + x] - lumEnc[ym * W + x]));
            }
    });
    if (cancelled(progress)) return fail("cancelled");
    // 7x7 box mean of |grad| (separable; edge-clamped), scaled to the proxy's spatial frequency:
    // the "smooth at proxy scale" texture measure keeps its meaning at full resolution by
    // widening with the proxy decimation factor (capped for cost).
    const int tr = std::clamp(static_cast<int>(std::lround(3.0 * estimate.proxyScale)), 3, 12);
    parallelRows(static_cast<std::size_t>(H), [&](std::size_t r0, std::size_t r1) {
        for (std::size_t y = r0; y < r1; ++y)
            for (int x = 0; x < W; ++x) {
                float s = 0.0f;
                for (int d = -tr; d <= tr; ++d) s += grad[y * W + std::clamp(x + d, 0, W - 1)];
                tmp[y * W + x] = s / (2 * tr + 1);
            }
    });
    parallelRows(static_cast<std::size_t>(H), [&](std::size_t r0, std::size_t r1) {
        for (std::size_t y = r0; y < r1; ++y)
            for (int x = 0; x < W; ++x) {
                float s = 0.0f;
                for (int d = -tr; d <= tr; ++d) {
                    const long yy = std::clamp<long>(static_cast<long>(y) + d, 0, H - 1);
                    s += tmp[static_cast<std::size_t>(yy) * W + x];
                }
                grad[y * W + x] = s / (2 * tr + 1);  // grad now holds the box-mean texture term
            }
    });
    lumEnc.clear();
    lumEnc.shrink_to_fit();
    if (cancelled(progress)) return fail("cancelled");

    const SkyColorModel& m = estimate.colorModel;
    std::vector<std::uint8_t> P(N, 0);
    constexpr double g0 = 4.0 / 255.0;
    parallelRows(static_cast<std::size_t>(H), [&](std::size_t r0, std::size_t r1) {
        for (std::size_t y = r0; y < r1; ++y) {
            const std::uint8_t* row = photo.rgba.data() + y * W * 4;
            for (int x = 0; x < W; ++x) {
                if (row[x * 4 + 3] < 128) continue;
                const double r = lut[row[x * 4]], g = lut[row[x * 4 + 1]],
                             b = lut[row[x * 4 + 2]];
                const double sum = r + g + b + 1e-9;
                const double cr = r / sum, cb = b / sum;
                const double L = linLum(r, g, b);
                const double grayGate =
                    m.grayAdaptive
                        ? 1.0
                        : smooth01((L / std::max(1e-4, m.lumNorm) - 0.45) / 0.3);
                const double color =
                    std::max(lobeScore(m.blue, cr, cb), lobeScore(m.gray, cr, cb) * grayGate);
                // The proxy's decimation is itself a low-pass, so the full-res gradient runs a
                // touch hotter; the widened box above compensates, the response curve matches.
                const double gr = grad[y * W + x] / g0;
                const double texture = std::exp(-gr * gr);
                const double lumT =
                    std::max(0.35, clamp01(L / std::max(1e-4, 0.9 * m.lumNorm)));
                const double pos =
                    1.0 / (1.0 + std::exp(-(borderDoc[x] - static_cast<double>(y)) / (0.05 * H)));
                const double v = pos * std::pow(std::max(color, 1e-4), 1.2) *
                                 std::pow(std::max(texture, 1e-4), 0.8) * std::pow(lumT, 0.5);
                P[y * W + x] = static_cast<std::uint8_t>(clamp01(v) * 255.0 + 0.5);
            }
        }
    });
    grad.clear();
    grad.shrink_to_fit();
    tmp.clear();
    tmp.shrink_to_fit();
    mark(progress, 300);
    if (cancelled(progress)) return fail("cancelled");

    // ---- Seeded floods (Adams & Bischof seeded growing on the wand's metric seam). ----
    std::vector<std::uint8_t> flood(N, 0);
    {
        std::vector<std::uint16_t> stamp(N, 0);
        std::uint16_t cur = 0;
        std::vector<std::uint32_t> stack, region;
        const auto pixelAt = [&](std::size_t i) {
            const std::uint8_t* q = photo.rgba.data() + i * 4;
            return common::Color8{q[0], q[1], q[2], q[3]};
        };
        // One lazy 4-connected flood from `seed` at tolerance `tol`; the flood is bounded a few
        // pixels past the border line (the combine step clips there anyway -- pure cost saving).
        const auto floodOnce = [&](std::size_t seed, double tol) {
            ++cur;
            region.clear();
            stack.assign(1, static_cast<std::uint32_t>(seed));
            stamp[seed] = cur;
            const common::Color8 sc = pixelAt(seed);
            while (!stack.empty()) {
                const std::uint32_t i = stack.back();
                stack.pop_back();
                region.push_back(i);
                const int x = static_cast<int>(i % W), y = static_cast<int>(i / W);
                const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                for (const auto& q : nb) {
                    if (q[0] < 0 || q[1] < 0 || q[0] >= W || q[1] >= H) continue;
                    if (static_cast<float>(q[1]) > borderDoc[q[0]] + 16.0f) continue;
                    const std::size_t j = static_cast<std::size_t>(q[1]) * W + q[0];
                    if (stamp[j] == cur) continue;
                    if (wandColorDistance(pixelAt(j), sc, /*useAlpha=*/true) <= tol) {
                        stamp[j] = cur;
                        stack.push_back(static_cast<std::uint32_t>(j));
                    }
                }
            }
        };
        for (int sx = 32; sx < W; sx += 64) {
            if (cancelled(progress)) return fail("cancelled");
            const int yMax = static_cast<int>(borderDoc[sx]) - 24;
            for (int sy = 32; sy < std::min(yMax, H); sy += 64) {
                const std::size_t i = static_cast<std::size_t>(sy) * W + sx;
                if (flood[i] || P[i] < 230) continue;
                floodOnce(i, 0.08);
                // Adaptive re-flood: spread of the wand metric over the first region.
                double s2 = 0.0;
                const common::Color8 sc = pixelAt(i);
                for (const std::uint32_t j : region) {
                    const double d = wandColorDistance(pixelAt(j), sc, true);
                    s2 += d * d;
                }
                const double sigma = region.empty() ? 0.0 : std::sqrt(s2 / region.size());
                const double tol2 = std::clamp(2.5 * sigma, 0.06, 0.20);
                if (std::abs(tol2 - 0.08) > 1e-3) floodOnce(i, tol2);
                for (const std::uint32_t j : region) flood[j] = 1;
            }
        }
    }
    mark(progress, 550);
    if (cancelled(progress)) return fail("cancelled");

    // ---- Combine: (above border) AND (flood OR P > 0.8). ----
    std::vector<std::uint8_t> sky(N, 0);
    parallelRows(static_cast<std::size_t>(H), [&](std::size_t r0, std::size_t r1) {
        for (std::size_t y = r0; y < r1; ++y)
            for (int x = 0; x < W; ++x) {
                const std::size_t i = y * W + x;
                if (static_cast<float>(y) < borderDoc[x] && (flood[i] || P[i] >= 204)) sky[i] = 1;
            }
    });

    // ---- Holes policy (one connected-components pass each way). ----
    {
        std::vector<std::int32_t> label(N, -1);
        std::vector<std::uint32_t> stack;
        const auto component = [&](std::size_t start, int id, std::uint8_t of,
                                   const std::vector<std::uint8_t>& maskv, bool& touchesEdge,
                                   std::vector<std::uint32_t>& pixels) {
            touchesEdge = false;
            pixels.clear();
            stack.assign(1, static_cast<std::uint32_t>(start));
            label[start] = id;
            while (!stack.empty()) {
                const std::uint32_t i = stack.back();
                stack.pop_back();
                pixels.push_back(i);
                const int x = static_cast<int>(i % W), y = static_cast<int>(i / W);
                if (x == 0 || y == 0 || x == W - 1 || y == H - 1) touchesEdge = true;
                const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                for (const auto& q : nb) {
                    if (q[0] < 0 || q[1] < 0 || q[0] >= W || q[1] >= H) continue;
                    const std::size_t j = static_cast<std::size_t>(q[1]) * W + q[0];
                    if (maskv[j] == of && label[j] < 0) {
                        label[j] = id;
                        stack.push_back(static_cast<std::uint32_t>(j));
                    }
                }
            }
        };
        // Fill enclosed non-sky islands smaller than 0.02% of the frame (dust, distant birds);
        // anything larger -- or anything reaching the frame edge / the ground mass -- stays
        // foreground (chimney, head against sky, power-line clusters).
        std::vector<std::uint32_t> pixels;
        int id = 0;
        const std::size_t fillCap = std::max<std::size_t>(4, N / 5000);
        for (std::size_t i = 0; i < N; ++i) {
            if (sky[i] || label[i] >= 0) continue;
            bool edge = false;
            component(i, id++, 0, sky, edge, pixels);
            if (!edge && pixels.size() < fillCap)
                for (const std::uint32_t j : pixels) sky[j] = 1;
        }
        // Drop sky specks not connected to the main sky body and smaller than 0.1% of the frame
        // (specular roofs, bright car glass).
        std::fill(label.begin(), label.end(), -1);
        std::vector<std::vector<std::uint32_t>> comps;
        for (std::size_t i = 0; i < N; ++i) {
            if (!sky[i] || label[i] >= 0) continue;
            bool edge = false;
            component(i, static_cast<int>(comps.size()), 1, sky, edge, pixels);
            comps.push_back(pixels);
        }
        std::size_t mainBody = 0, mainSize = 0;
        for (std::size_t c = 0; c < comps.size(); ++c)
            if (comps[c].size() > mainSize) {
                mainSize = comps[c].size();
                mainBody = c;
            }
        const std::size_t dropCap = std::max<std::size_t>(4, N / 1000);
        for (std::size_t c = 0; c < comps.size(); ++c)
            if (c != mainBody && comps[c].size() < dropCap)
                for (const std::uint32_t j : comps[c]) sky[j] = 0;
    }
    mark(progress, 700);
    if (cancelled(progress)) return fail("cancelled");

    // Final honesty gate on the hard mask.
    std::size_t covered = 0;
    for (std::size_t i = 0; i < N; ++i) covered += sky[i];
    const double frac = static_cast<double>(covered) / static_cast<double>(N);
    if (frac < 0.02) return fail("Couldn't isolate the sky -- mask & harmonize skipped");

    // ---- Edge finish: de-staircase the DP border, then feather (FH-EDT + Gaussian only). ----
    Selection sel(photo.width, photo.height);
    auto& data = sel.data();
    for (std::size_t i = 0; i < N; ++i) data[i] = sky[i] ? 255 : 0;
    sel = sel.smoothed(2.0);
    const double diag = std::hypot(static_cast<double>(W), static_cast<double>(H));
    sel = sel.feathered(std::clamp(diag / 1500.0, 1.0, 3.0));
    mark(progress, 1000);
    if (sel.isEmpty()) return fail("Couldn't isolate the sky -- mask & harmonize skipped");
    return sel;
}

// -------------------------------------------------------------------------------------------------
// S7: photometric harmonization parameters (design §6). The TARGET illuminant is parametric --
// generator state only: sun transmittance through our own airmass model blended with the mean
// dome radiance of a small dome-only probe render (constraint:  never statistics of a
// style/reference image). The SOURCE illuminant is classical color constancy on the photo's
// foreground: gray-edge (van de Weijer 2007) blended with white-patch Retinex (Land 1977),
// gray-world fallback (Buchsbaum 1980). Exposure/contrast ride Reinhard 2001 statistics on
// log-luminance only; the night response follows Thompson 2002 / Jensen 2000 with Ward Larson
// 1997 scotopic luminance. Every output is a scalar in the PhotometricMatch params bag.
// -------------------------------------------------------------------------------------------------
namespace {

// Kasten-Young (1989) relative air mass; public formula, shared shape with the renderer's.
[[nodiscard]] double airMassKY(double elevationDeg) noexcept {
    const double zenith = std::clamp(90.0 - elevationDeg, 0.0, 89.9);
    return 1.0 / (std::cos(zenith * kDegToRad) +
                  0.50572 * std::pow(96.07995 - zenith, -1.6364));
}

// Direct solar transmittance color at `elevationDeg` for `turbidity`: Beer-Lambert over
// Rayleigh (per-channel lambda^-4-ish zenith depths) + Angstrom aerosol depths scaled by the
// Preetham turbidity-to-beta mapping. Only the RATIO matters (the result is L1-normalized).
[[nodiscard]] std::array<double, 3> sunTransmittance(double elevationDeg, double turbidity) {
    const double m = airMassKY(std::max(0.0, elevationDeg));
    const double beta = std::max(0.0, 0.04608 * turbidity - 0.04586);
    // Zenith optical depths at ~(0.68, 0.55, 0.44) um: Rayleigh ~ lambda^-4.08, aerosol
    // Angstrom alpha = 1.3.
    const double tauR[3] = {0.0424, 0.0996, 0.2460};
    const double tauM[3] = {beta * std::pow(0.68, -1.3), beta * std::pow(0.55, -1.3),
                            beta * std::pow(0.44, -1.3)};
    std::array<double, 3> t{};
    double sum = 0.0;
    for (int c = 0; c < 3; ++c) {
        t[c] = std::exp(-m * (tauR[c] + tauM[c]));
        sum += t[c];
    }
    if (sum <= 1e-12) return {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
    for (double& v : t) v /= sum;
    return t;
}

// One dome-only probe: mean linear dome color, mean log2 luminance (the k(el) table entry) and
// the std of ln luminance (the model-side contrast). Sky pixels above the model horizon only.
struct DomeProbe {
    bool ok = false;
    std::array<double, 3> meanRgb{};
    double k = 0.0;      // mean log2 luminance
    double sigma = 0.0;  // std of ln luminance
};

DomeProbe domeProbe(const SkyParams& base, double elevationDeg, double turbidity) {
    DomeProbe out;
    SkyParams s = base;
    s.enableDome = true;
    s.enableHaze = true;
    s.enableClouds = false;
    s.enableSun = false;
    s.enableMoon = false;
    s.starsAmount = 0.0;
    s.exposure = 0.0;
    s.sunAzimuthDeg = 180.0;
    s.sunElevationDeg = elevationDeg;
    s.turbidity = turbidity;
    TextureParams tp;
    tp.generator = Generator::Sky;
    tp.seed = 1;
    tp.scale = 1.0;
    tp.spec = s;
    const TextureRenderResult r = renderTexture(tp, 32, 32);
    if (!r.imageF || r.imageF->empty()) return out;
    const common::ImageF& img = *r.imageF;
    const auto hz = modelHorizonLine(s, img.width, img.height);
    double sum[3] = {0, 0, 0};
    double kSum = 0.0, lnSum = 0.0, lnSq = 0.0;
    std::size_t n = 0;
    for (std::uint32_t y = 0; y < img.height; ++y)
        for (std::uint32_t x = 0; x < img.width; ++x) {
            if (hz) {
                const double t = (x + 0.5) / img.width;
                const double border = hz->yAt0 + (hz->yAtW - hz->yAt0) * t;
                if (y + 0.5 >= border) continue;
            }
            const common::ColorF c = img.at(x, y);
            const double r0 = srgbDecode(std::max(0.0f, c.r));
            const double g0v = srgbDecode(std::max(0.0f, c.g));
            const double b0 = srgbDecode(std::max(0.0f, c.b));
            sum[0] += r0;
            sum[1] += g0v;
            sum[2] += b0;
            const double L = std::max(1e-6, linLum(r0, g0v, b0));
            kSum += std::log2(L);
            const double ln = std::log(L);
            lnSum += ln;
            lnSq += ln * ln;
            ++n;
        }
    if (n < 16) return out;
    for (int c = 0; c < 3; ++c) out.meanRgb[c] = sum[c] / n;
    out.k = kSum / n;
    const double mean = lnSum / n;
    out.sigma = std::sqrt(std::max(0.0, lnSq / n - mean * mean));
    out.ok = true;
    return out;
}

}  // namespace

std::map<std::string, double> photometricMatchParams(const PhotometricMatchInput& input,
                                                     const common::Image& photo,
                                                     const Selection& skySelection) {
    std::map<std::string, double> bag;
    const auto& lut = srgbByteLut();
    const int W = static_cast<int>(photo.width);
    const int H = static_cast<int>(photo.height);

    // ---- Foreground statistics grid (long edge <= 512; box average of usable pixels). ----
    const int k = std::max(1, (std::max(W, H) + 511) / 512);
    const int gw = std::max(1, W / k), gh = std::max(1, H / k);
    std::vector<float> fg(static_cast<std::size_t>(gw) * gh * 3, 0.0f);
    std::vector<std::uint8_t> fgOk(static_cast<std::size_t>(gw) * gh, 0);
    double muSum = 0.0;
    std::size_t muN = 0;
    std::vector<float> chR, chG, chB;  // white-patch sample pool
    for (int gy = 0; gy < gh; ++gy)
        for (int gx = 0; gx < gw; ++gx) {
            double r = 0, g = 0, b = 0;
            int n = 0;
            for (int dy = 0; dy < k; ++dy)
                for (int dx = 0; dx < k; ++dx) {
                    const int x = gx * k + dx, y = gy * k + dy;
                    if (x >= W || y >= H) continue;
                    const std::uint8_t* q =
                        photo.rgba.data() + (static_cast<std::size_t>(y) * W + x) * 4;
                    if (q[3] < 128) continue;
                    if (skySelection.at(static_cast<std::uint32_t>(x),
                                        static_cast<std::uint32_t>(y)) >=
                        kAntsCoverageThreshold)
                        continue;  // sky side
                    const int mx = std::max({q[0], q[1], q[2]});
                    const int mn = std::min({q[0], q[1], q[2]});
                    if (mn >= 250 || mx <= 6) continue;  // near-clipped / near-black
                    r += lut[q[0]];
                    g += lut[q[1]];
                    b += lut[q[2]];
                    ++n;
                }
            if (n == 0) continue;
            const std::size_t i = static_cast<std::size_t>(gy) * gw + gx;
            fg[i * 3] = static_cast<float>(r / n);
            fg[i * 3 + 1] = static_cast<float>(g / n);
            fg[i * 3 + 2] = static_cast<float>(b / n);
            fgOk[i] = 1;
            muSum += std::log(std::max(1e-6, linLum(fg[i * 3], fg[i * 3 + 1], fg[i * 3 + 2])));
            ++muN;
            chR.push_back(fg[i * 3]);
            chG.push_back(fg[i * 3 + 1]);
            chB.push_back(fg[i * 3 + 2]);
        }

    const double strength = std::clamp(input.strength, 0.0, 1.0);
    const double muS = muN > 0 ? muSum / static_cast<double>(muN) : std::log(0.18);

    // ---- Source illuminant: gray-edge (sigma 2, Minkowski p 6) + white-patch (99th percentile
    //      of the FOREGROUND -- classical color constancy, not a segmentation threshold). ----
    std::array<double, 3> Ls{1.0, 1.0, 1.0};
    if (muN >= 16) {
        // Small separable Gaussian (sigma 2, radius 5) over the grid, per channel.
        std::array<double, 11> kern{};
        double ks = 0.0;
        for (int i = -5; i <= 5; ++i) {
            kern[i + 5] = std::exp(-0.5 * (i / 2.0) * (i / 2.0));
            ks += kern[i + 5];
        }
        for (double& v : kern) v /= ks;
        std::vector<float> blur(fg.size(), 0.0f), pass(fg.size(), 0.0f);
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x)
                for (int c = 0; c < 3; ++c) {
                    double s = 0.0;
                    for (int d = -5; d <= 5; ++d)
                        s += kern[d + 5] *
                             fg[(static_cast<std::size_t>(y) * gw + std::clamp(x + d, 0, gw - 1)) *
                                    3 +
                                c];
                    pass[(static_cast<std::size_t>(y) * gw + x) * 3 + c] = static_cast<float>(s);
                }
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x)
                for (int c = 0; c < 3; ++c) {
                    double s = 0.0;
                    for (int d = -5; d <= 5; ++d)
                        s += kern[d + 5] *
                             pass[(static_cast<std::size_t>(std::clamp(y + d, 0, gh - 1)) * gw +
                                   x) *
                                      3 +
                                  c];
                    blur[(static_cast<std::size_t>(y) * gw + x) * 3 + c] = static_cast<float>(s);
                }
        std::array<double, 3> ge{};
        std::size_t geN = 0;
        for (int y = 1; y < gh - 1; ++y)
            for (int x = 1; x < gw - 1; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * gw + x;
                if (!fgOk[i] || !fgOk[i - 1] || !fgOk[i + 1] || !fgOk[i - gw] || !fgOk[i + gw])
                    continue;
                bool any = false;
                for (int c = 0; c < 3; ++c) {
                    const double dx = 0.5 * (blur[(i + 1) * 3 + c] - blur[(i - 1) * 3 + c]);
                    const double dy = 0.5 * (blur[(i + gw) * 3 + c] - blur[(i - gw) * 3 + c]);
                    const double gmag = std::hypot(dx, dy);
                    ge[c] += std::pow(gmag, 6.0);
                    if (gmag > 1e-4) any = true;
                }
                if (any) ++geN;
            }
        std::array<double, 3> wp{};
        {
            const auto pct99 = [](std::vector<float>& v) {
                if (v.empty()) return 1.0;
                const std::size_t i = std::min(v.size() - 1,
                                               static_cast<std::size_t>(v.size() * 0.99));
                std::nth_element(v.begin(), v.begin() + i, v.end());
                return static_cast<double>(v[i]);
            };
            wp = {pct99(chR), pct99(chG), pct99(chB)};
        }
        const auto l1norm = [](std::array<double, 3> v) {
            const double s = v[0] + v[1] + v[2];
            if (s <= 1e-12) return std::array<double, 3>{1 / 3.0, 1 / 3.0, 1 / 3.0};
            for (double& c : v) c /= s;
            return v;
        };
        if (geN >= muN / 100 && geN >= 8) {
            std::array<double, 3> geC{};
            for (int c = 0; c < 3; ++c) geC[c] = std::pow(ge[c] / std::max<std::size_t>(1, geN),
                                                          1.0 / 6.0);
            const auto a = l1norm(geC);
            const auto b = l1norm(wp);
            Ls = l1norm({0.5 * a[0] + 0.5 * b[0], 0.5 * a[1] + 0.5 * b[1],
                         0.5 * a[2] + 0.5 * b[2]});
        } else {
            // Gray-world fallback: too few gradients to trust gray-edge.
            std::array<double, 3> gwMean{};
            for (std::size_t i = 0; i < fgOk.size(); ++i)
                if (fgOk[i])
                    for (int c = 0; c < 3; ++c) gwMean[c] += fg[i * 3 + c];
            Ls = l1norm(gwMean);
        }
    }

    // ---- Target illuminant + the k(el) pair (all from OUR forward model). ----
    const double elT = input.sky.sunElevationDeg;
    const DomeProbe target = domeProbe(input.sky, elT, input.sky.turbidity);
    const DomeProbe source = domeProbe(input.sky, input.photoElevationDeg, input.photoTurbidity);
    std::array<double, 3> Lt{1.0, 1.0, 1.0};
    {
        const double wDir = elT <= 0.0 ? 0.0
                                       : smooth01(elT / 2.0) *
                                             (0.5 + 0.3 * clamp01(elT / 15.0));
        const std::array<double, 3> sun = sunTransmittance(elT, input.sky.turbidity);
        std::array<double, 3> dome{1 / 3.0, 1 / 3.0, 1 / 3.0};
        if (target.ok) {
            const double s = target.meanRgb[0] + target.meanRgb[1] + target.meanRgb[2];
            if (s > 1e-12)
                dome = {target.meanRgb[0] / s, target.meanRgb[1] / s, target.meanRgb[2] / s};
        }
        double sum = 0.0;
        for (int c = 0; c < 3; ++c) {
            Lt[c] = wDir * sun[c] + (1.0 - wDir) * dome[c];
            sum += Lt[c];
        }
        for (double& v : Lt) v /= std::max(1e-12, sum);
    }

    double gains[3];
    for (int c = 0; c < 3; ++c)
        gains[c] = std::pow(std::clamp(Lt[c] / std::max(1e-6, Ls[c]), 0.6, 1.6), strength);

    double deltaEv = 0.0, sigmaRatio = 1.0;
    if (target.ok && source.ok) {
        deltaEv = std::clamp((target.k + input.sky.exposure) -
                                 (source.k + input.photoSkyExposureEv),
                             -6.0, 2.0);
        sigmaRatio = std::clamp(source.sigma > 1e-6 ? target.sigma / source.sigma : 1.0, 0.7, 1.3);
    }

    const double rodT = clamp01((-2.0 - elT) / 12.0);
    const double rod = rodT * rodT * (3.0 - 2.0 * rodT);
    const double gradient =
        input.confidence >= 0.5 ? std::clamp(0.4 * deltaEv, -0.12, 0.12) : 0.0;

    bag["gain_r"] = gains[0];
    bag["gain_g"] = gains[1];
    bag["gain_b"] = gains[2];
    bag["mu_log"] = muS;
    bag["delta_ev"] = deltaEv * strength;
    bag["sigma_ratio"] = 1.0 + (sigmaRatio - 1.0) * strength;
    bag["gradient"] = gradient;
    bag["rod"] = rod * strength;
    bag["night_r"] = 0.42;
    bag["night_g"] = 0.55;
    bag["night_b"] = 1.00;
    bag["night_gain"] = 1.0;
    bag["saturation"] = 1.0 - 0.5 * rod * strength;
    bag["strength"] = strength;
    return bag;
}

}  // namespace mosaic::core::texture
