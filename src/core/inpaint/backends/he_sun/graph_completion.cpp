#include "core/inpaint/backends/he_sun/graph_completion.hpp"

#include "core/inpaint/outpaint.hpp"

#include "core/inpaint/backends/he_sun/graph_cut.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include "common/thread_pool.hpp"

namespace mosaic::core::inpaint {

namespace {

// Data cost for a label whose source pixel is out of bounds or itself a hole pixel: effectively
// infinite, so α-expansion never assigns an invalid offset when a valid one exists.
constexpr double kInvalid = 1e9;
// Weight of the hole↔known boundary coherence term (relative to the unit seam SSD).
constexpr double kBoundaryWeight = 1.0;
// Weight of the GRADIENT half of the seam objective ("colors & gradients": the two labels must
// also agree on the EDGE they produce across a seam pair, not just on the colours at it). Plain
// colour SSD is blind to structure — two views of a horizon shifted by a pixel are both "sky
// here, sea there" at the seam, so the cut happily crossed misaligned edges and the Poisson
// blend then smeared the step instead of aligning it (user report 2026-07-02). Equal footing
// with the colour term, as published. The objective is Kwatra et al. 2003 / Agarwala et al. 2004
// (SIGGRAPH); the Poisson blend that consumes it stays plain red-black Gauss-Seidel and never a
// quadtree-accelerated gradient-domain solve (see poissonSeamBlend).
constexpr double kSeamGradientWeight = 1.0;
// Sentinel offset for a hole pixel that has no valid source (no offset reaches known content). The
// Poisson blend treats it as "no source view" and falls back to the composite's own gradient, so it
// never samples the removed content through an invalid offset.
constexpr int kNoSource = std::numeric_limits<int>::min();
// Cap on the projected-SOR polish sweeps AFTER a multigrid pre-solve (the V-cycles own the low
// frequencies; the polish enforces the exact offset-based guidance + the [0,1] projection).
// Params::poissonIterations still applies when it is smaller.
constexpr int kMgPolishIters = 48;

[[nodiscard]] bool pixelHole(const Selection& m, std::uint32_t x, std::uint32_t y) {
    return !m.isEmpty() && x < m.width() && y < m.height() && m.at(x, y) > 0;
}

[[nodiscard]] long clampL(long v, long hi) {
    return v < 0 ? 0L : (v >= hi ? hi - 1 : v);
}

[[nodiscard]] double colorSSD(const common::ImageF& im, long ax, long ay, long bx, long by) {
    const common::ColorF a = im.at(static_cast<std::uint32_t>(ax), static_cast<std::uint32_t>(ay));
    const common::ColorF b = im.at(static_cast<std::uint32_t>(bx), static_cast<std::uint32_t>(by));
    const double dr = static_cast<double>(a.r) - b.r;
    const double dg = static_cast<double>(a.g) - b.g;
    const double db = static_cast<double>(a.b) - b.b;
    return dr * dr + dg * dg + db * db;
}

// SSD between two already-sampled RGB colours (the seam pairwise term over precomputed shifted
// samples — see computeGraphCutLabels).
[[nodiscard]] double colorSSDc(const common::ColorF& a, const common::ColorF& b) {
    const double dr = static_cast<double>(a.r) - b.r;
    const double dg = static_cast<double>(a.g) - b.g;
    const double db = static_cast<double>(a.b) - b.b;
    return dr * dr + dg * dg + db * db;
}

// The gradient half of the seam objective for edge (a,b) under labels (la,lb): each label's VIEW
// of the edge is the difference of its two samples; the mismatch of the two views is what a cut
// through misaligned structure costs. ‖(aLa−bLa) − (aLb−bLb)‖².
[[nodiscard]] double gradientSSDc(const common::ColorF& aLa, const common::ColorF& bLa,
                                  const common::ColorF& aLb, const common::ColorF& bLb) {
    const double dr =
        (static_cast<double>(aLa.r) - bLa.r) - (static_cast<double>(aLb.r) - bLb.r);
    const double dg =
        (static_cast<double>(aLa.g) - bLa.g) - (static_cast<double>(aLb.g) - bLb.g);
    const double db =
        (static_cast<double>(aLa.b) - bLa.b) - (static_cast<double>(aLb.b) - bLb.b);
    return dr * dr + dg * dg + db * db;
}

// Split [0, count) into hardware-thread bands and run fn(begin, end) on each (bands touch disjoint
// ranges). Used to parallelize the per-(node,label) cost precompute below. The band arithmetic is
// unchanged since S39; S60-b only runs the bands on the shared pool (common/thread_pool.hpp)
// instead of building and joining a fresh std::vector<std::thread> per call.
template <class Fn> void parallelRanges(int count, Fn&& fn) {
    const unsigned hw = common::hardwareThreads();
    const int bands = std::max(1, std::min<int>(static_cast<int>(hw), std::max(1, count / 1024)));
    if (bands <= 1) {
        fn(0, count);
        return;
    }
    const int step = (count + bands - 1) / bands;
    common::parallelBands(static_cast<std::size_t>((count + step - 1) / step), [&](std::size_t b) {
        const int lo = static_cast<int>(b) * step;
        fn(lo, std::min(count, lo + step));
    });
}

// Box-average downsample by integer factor l (>= 1).
[[nodiscard]] common::ImageF downsampleImage(const common::ImageF& src, int l) {
    if (l <= 1 || src.empty()) {
        return src;
    }
    const long sw = static_cast<long>(src.width);
    const long sh = static_cast<long>(src.height);
    const std::uint32_t w = static_cast<std::uint32_t>((sw + l - 1) / l);
    const std::uint32_t h = static_cast<std::uint32_t>((sh + l - 1) / l);
    common::ImageF out(w, h);
    for (std::uint32_t oy = 0; oy < h; ++oy) {
        for (std::uint32_t ox = 0; ox < w; ++ox) {
            double r = 0, g = 0, b = 0, a = 0;
            int n = 0;
            for (int dy = 0; dy < l; ++dy) {
                for (int dx = 0; dx < l; ++dx) {
                    const long sx = static_cast<long>(ox) * l + dx;
                    const long sy = static_cast<long>(oy) * l + dy;
                    if (sx < sw && sy < sh) {
                        const common::ColorF c =
                            src.at(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy));
                        r += c.r;
                        g += c.g;
                        b += c.b;
                        a += c.a;
                        ++n;
                    }
                }
            }
            if (n > 0) {
                const float inv = 1.0f / static_cast<float>(n);
                out.set(ox, oy,
                        {static_cast<float>(r) * inv, static_cast<float>(g) * inv,
                         static_cast<float>(b) * inv, static_cast<float>(a) * inv});
            }
        }
    }
    return out;
}

// Coarse hole mask: a coarse pixel is a hole if any fine pixel in its l×l block is.
[[nodiscard]] Selection downsampleMask(const Selection& mask, int l, std::uint32_t cw,
                                       std::uint32_t ch, long fw, long fh) {
    Selection out(cw, ch);
    std::vector<std::uint8_t>& d = out.data();
    for (std::uint32_t cy = 0; cy < ch; ++cy) {
        for (std::uint32_t cx = 0; cx < cw; ++cx) {
            bool anyHole = false;
            for (int dy = 0; dy < l && !anyHole; ++dy) {
                for (int dx = 0; dx < l && !anyHole; ++dx) {
                    const long fx = static_cast<long>(cx) * l + dx;
                    const long fy = static_cast<long>(cy) * l + dy;
                    if (fx < fw && fy < fh &&
                        pixelHole(mask, static_cast<std::uint32_t>(fx),
                                  static_cast<std::uint32_t>(fy))) {
                        anyHole = true;
                    }
                }
            }
            if (anyHole) {
                d[static_cast<std::size_t>(cy) * cw + cx] = 255;
            }
        }
    }
    return out;
}

// The graph-cut labeling: assign each hole pixel one of `offsets` by α-expansion (validity +
// hole↔known boundary data terms, seam-coherence pairwise term). Fills `outNodeOf` (size W*H,
// row-major; -1 = known) and returns the per-node label vector (node order = row-major hole scan).
// ---- Outpaint structure penalty ---------------------------------------------------------------
//
// When the hole is a canvas-expansion ring (isOutpaintHole), labels pay a per-pixel data cost
// for sourcing from strongly STRUCTURED content. Without it, wholesale duplication of a salient
// object into the ring is nearly free: a verbatim copy has no interior seams, and the ring's
// boundary terms cannot see the copy's interior (the Broadway-tower duplication). The measure is
// structure-tensor ANISOTROPY (λ1−λ2 of the blurred tensor): grass/sand (isotropic texture) and
// sky/cloud (smooth) score low, coherent long edges (buildings, people, horizons) score high — a
// thin horizon continuation costs a line, an imported object costs its area. Interior heals
// never build one (the gate), so their energy is untouched, byte for byte.
// (StructurePenalty + buildStructurePenalty live in the header, exposed for tests.)

} // namespace

// `dampFrac` (addendum, 2026-07-11): damp LOW-ENERGY anisotropy (applied after the
// normalisation — see the note at the damping loop for why that order).
// λ1−λ2 is energy-weighted, but the 98th-percentile normalisation is frame-relative, so faint-
// but-coherent structure — the soft edge of a cloud bank — still lands a nonzero cost that the
// max-dilation then spreads across the sky. On the Broadway repro exactly those damped-out
// responses taxed the clean far-sky donor columns, keeping a corner fragment the least-bad
// option. Multiplying by trace/(trace + dampFrac·T98) — trace = λ1+λ2 = the tensor's total
// gradient energy, T98 its robust frame scale — leaves hard edges (trace ≈ T98) essentially
// untouched and squashes coherent-but-faint responses toward zero. Textbook structure-tensor
// coherence weighting (Förstner 1986 / Weickert 1990s lineage); still the same
// structure-weighted data term, no new mechanism. <= 0 disables (the pre-addendum map).
//
// `devFrac` (second addendum, 2026-07-11): the LOCAL-DEVIATION term. The tensor is blind
// to smooth non-uniform content — a faint cloud's interior has λ1−λ2 ≈ 0 everywhere, so on the
// Broadway repro cloud-carrying donor sheets stayed cheap and their half-blended remains were
// the recorded horizon smudge and top-right wisp. The term taxes donors by the BAND-PASS energy
// of luma — |box_r(L) − box_R(L)|, a difference of two box means (unsharp-mask / difference-of-
// Gaussians form; textbook signal processing) — which is ~zero both for flat sky AND for fine
// texture like grass (killed by the first blur), but lights up cloud-scale blobs whole. Robustly
// normalised like the anisotropy, boundary-guard applied, then MAX-combined into the map AFTER
// the anisotropy dilation: everywhere the tensor already taxes (horizon halo included) the map
// is unchanged, so the tuned w=0.15 horizon behaviour cannot regress; the term only raises the
// floor where anisotropy is blind. No dilation of its own — a band-pass response covers a blob's
// interior by construction. Same structure-weighted data-term family; no new mechanism.
// <= 0 disables (bit-for-bit the previous map).
[[nodiscard]] StructurePenalty buildStructurePenalty(const common::ImageF& img,
                                                     const Selection& holeMask, double weight,
                                                     double dampFrac, double devFrac) {
    StructurePenalty sp;
    sp.w = static_cast<long>(img.width);
    sp.h = static_cast<long>(img.height);
    sp.weight = weight;
    const long W = sp.w;
    const long H = sp.h;
    if (W < 3 || H < 3) {
        return sp;
    }
    const std::size_t n = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
    std::vector<float> luma(n);
    for (long y = 0; y < H; ++y) {
        for (long x = 0; x < W; ++x) {
            const common::ColorF c =
                img.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            luma[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) +
                 static_cast<std::size_t>(x)] = 0.299F * c.r + 0.587F * c.g + 0.114F * c.b;
        }
    }
    // Sobel -> structure tensor components.
    std::vector<float> jxx(n, 0.0F);
    std::vector<float> jyy(n, 0.0F);
    std::vector<float> jxy(n, 0.0F);
    const auto L = [&](long x, long y) {
        return luma[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) +
                    static_cast<std::size_t>(x)];
    };
    for (long y = 1; y + 1 < H; ++y) {
        for (long x = 1; x + 1 < W; ++x) {
            const float gx = (L(x + 1, y - 1) + 2.0F * L(x + 1, y) + L(x + 1, y + 1)) -
                             (L(x - 1, y - 1) + 2.0F * L(x - 1, y) + L(x - 1, y + 1));
            const float gy = (L(x - 1, y + 1) + 2.0F * L(x, y + 1) + L(x + 1, y + 1)) -
                             (L(x - 1, y - 1) + 2.0F * L(x, y - 1) + L(x + 1, y - 1));
            const std::size_t o = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) +
                                  static_cast<std::size_t>(x);
            jxx[o] = gx * gx;
            jyy[o] = gy * gy;
            jxy[o] = gx * gy;
        }
    }
    // Separable box mean, radius r: smoothing the TENSOR (not the gradient) is what separates
    // coherent edges (components reinforce -> anisotropy survives) from isotropic texture
    // (random orientations cancel -> jxy averages out, jxx ~ jyy). The radius is a parameter
    // because the deviation term below reuses the same blur at two other radii.
    constexpr int kTensorBlurR = 6;
    const auto boxBlur = [&](std::vector<float>& v, const long r) {
        std::vector<float> tmp(n);
        for (long y = 0; y < H; ++y) { // horizontal pass
            double acc = 0.0;
            const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(W);
            for (long x = -r; x <= r; ++x) {
                acc += v[row + static_cast<std::size_t>(std::clamp(x, 0L, W - 1))];
            }
            for (long x = 0; x < W; ++x) {
                tmp[row + static_cast<std::size_t>(x)] = static_cast<float>(acc / (2 * r + 1));
                acc += v[row + static_cast<std::size_t>(std::clamp(x + r + 1, 0L, W - 1))] -
                       v[row + static_cast<std::size_t>(std::clamp(x - r, 0L, W - 1))];
            }
        }
        for (long x = 0; x < W; ++x) { // vertical pass
            double acc = 0.0;
            for (long y = -r; y <= r; ++y) {
                acc += tmp[static_cast<std::size_t>(std::clamp(y, 0L, H - 1)) *
                               static_cast<std::size_t>(W) +
                           static_cast<std::size_t>(x)];
            }
            for (long y = 0; y < H; ++y) {
                v[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) +
                  static_cast<std::size_t>(x)] = static_cast<float>(acc / (2 * r + 1));
                acc += tmp[static_cast<std::size_t>(std::clamp(y + r + 1, 0L, H - 1)) *
                               static_cast<std::size_t>(W) +
                           static_cast<std::size_t>(x)] -
                       tmp[static_cast<std::size_t>(std::clamp(y - r, 0L, H - 1)) *
                               static_cast<std::size_t>(W) +
                           static_cast<std::size_t>(x)];
            }
        }
    };
    boxBlur(jxx, kTensorBlurR);
    boxBlur(jyy, kTensorBlurR);
    boxBlur(jxy, kTensorBlurR);
    // Robust scale of a nonnegative field: the 98th percentile via a histogram (a lone specular
    // edge must not flatten everything else). Shared by the trace damping and the anisotropy
    // normalisation below.
    const auto pct98 = [n](const std::vector<float>& v, float maxV) {
        constexpr int kBins = 1024;
        std::vector<std::uint32_t> hist(kBins, 0);
        for (const float a : v) {
            ++hist[static_cast<std::size_t>(
                std::min<int>(kBins - 1, static_cast<int>(a / maxV * (kBins - 1))))];
        }
        std::uint64_t seen = 0;
        const auto target = static_cast<std::uint64_t>(0.98 * static_cast<double>(n));
        for (int b = 0; b < kBins; ++b) {
            seen += hist[static_cast<std::size_t>(b)];
            if (seen >= target) {
                return maxV * static_cast<float>(b + 1) / kBins;
            }
        }
        return maxV;
    };
    sp.map.resize(n);
    float maxA = 0.0F;
    for (std::size_t i = 0; i < n; ++i) {
        const float d = jxx[i] - jyy[i];
        const float a = std::sqrt(d * d + 4.0F * jxy[i] * jxy[i]); // λ1 − λ2
        sp.map[i] = a;
        maxA = std::max(maxA, a);
    }
    if (maxA <= 0.0F) {
        sp.map.assign(n, 0.0F);
        return sp;
    }
    // Robust normalisation, then clamp to 0..1.
    const float norm = std::max(pct98(sp.map, maxA), 1e-12F);
    for (float& a : sp.map) {
        a = std::min(1.0F, a / norm);
    }
    // Low-energy damping (see the function comment), AFTER the normalisation so it is a strict
    // reduction of the pre-addendum map (damping first would lower the 98th percentile and
    // re-amplify what it just squashed; dampFrac <= 0 therefore reproduces the old map exactly).
    // jxx+jyy is the blurred tensor's trace (λ1+λ2), untouched above, so still available here.
    if (dampFrac > 0.0) {
        float maxT = 0.0F;
        std::vector<float> tr(n);
        for (std::size_t i = 0; i < n; ++i) {
            tr[i] = jxx[i] + jyy[i];
            maxT = std::max(maxT, tr[i]);
        }
        if (maxT > 0.0F) {
            const float t0 = static_cast<float>(dampFrac) * pct98(tr, maxT);
            if (t0 > 0.0F) {
                for (std::size_t i = 0; i < n; ++i) {
                    sp.map[i] *= tr[i] / (tr[i] + t0);
                }
            }
        }
    }
    // Local-deviation map (devFrac > 0; see the function comment): band-pass luma magnitude,
    // |box_r(L) − box_R(L)|, r = the tensor radius, R ≈ min(W,H)/60 (cloud-blob passband),
    // robustly normalised at ITS 98th percentile, capped at 1, scaled by devFrac. Kept separate
    // until after the anisotropy dilation so max() cannot raise any already-taxed pixel.
    std::vector<float> dev;
    if (devFrac > 0.0) {
        const long rDev = std::clamp(std::min(W, H) / 60, 12L, 36L);
        std::vector<float> lo = luma;
        std::vector<float> hi = luma;
        boxBlur(lo, kTensorBlurR);
        boxBlur(hi, rDev);
        dev.resize(n);
        float maxD = 0.0F;
        for (std::size_t i = 0; i < n; ++i) {
            dev[i] = std::fabs(lo[i] - hi[i]);
            maxD = std::max(maxD, dev[i]);
        }
        if (maxD > 0.0F) {
            const float dNorm = std::max(pct98(dev, maxD), 1e-12F);
            for (float& d : dev) {
                d = static_cast<float>(devFrac) * std::min(1.0F, d / dNorm);
            }
        } else {
            dev.clear();
        }
    }
    // Zero the maps near the HOLE boundary before dilating: the image-against-empty-ring step
    // edge is the strongest "structure" in the frame and its halo taxed exactly the content an
    // outpaint SHOULD copy — the columns hugging the old frame edge (probe: the legitimate
    // continuation labels paid 0.149/px vs the tower's 0.25, so the field fragmented and the
    // tower stayed the least-bad option). Content adjacent to the boundary is prime donor
    // material; it must be free. The deviation map gets the same guard (the ring step edge is
    // equally its strongest response).
    {
        constexpr int rGuard = kTensorBlurR + 2;      // the tensor blur radius + slack
        const long margin = rGuard + std::clamp(std::min(W, H) / 50, 4L, 40L); // + dilation rd
        std::vector<std::uint8_t> nearHole(n, 0);
        for (long y = 0; y < H; ++y) {
            for (long x = 0; x < W; ++x) {
                if (pixelHole(holeMask, static_cast<std::uint32_t>(x),
                              static_cast<std::uint32_t>(y))) {
                    nearHole[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) +
                             static_cast<std::size_t>(x)] = 1;
                }
            }
        }
        // Separable box-max spreads the hole flag by `margin` in each axis.
        std::vector<std::uint8_t> tmp8(n);
        for (long y = 0; y < H; ++y) {
            const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(W);
            for (long x = 0; x < W; ++x) {
                std::uint8_t m = 0;
                for (long dx = -margin; dx <= margin && m == 0; ++dx) {
                    m = nearHole[row + static_cast<std::size_t>(std::clamp(x + dx, 0L, W - 1))];
                }
                tmp8[row + static_cast<std::size_t>(x)] = m;
            }
        }
        for (long x = 0; x < W; ++x) {
            for (long y = 0; y < H; ++y) {
                std::uint8_t m = 0;
                for (long dy = -margin; dy <= margin && m == 0; ++dy) {
                    m = tmp8[static_cast<std::size_t>(std::clamp(y + dy, 0L, H - 1)) *
                                 static_cast<std::size_t>(W) +
                             static_cast<std::size_t>(x)];
                }
                if (m != 0) {
                    const std::size_t o = static_cast<std::size_t>(y) *
                                              static_cast<std::size_t>(W) +
                                          static_cast<std::size_t>(x);
                    sp.map[o] = 0.0F;
                    if (!dev.empty()) {
                        dev[o] = 0.0F;
                    }
                }
            }
        }
    }
    // Max-DILATE: anisotropy is edge-sparse — a flat object interior (stone between windows)
    // pays nothing and a wholesale copy stayed cheap (the first Broadway sweep). Spreading each
    // strong response over its neighbourhood makes an object's interior carry its edges' cost;
    // grass/sky have nothing strong to spread. Separable square max, radius ~ min(W,H)/50.
    {
        const long rd = std::clamp(std::min(W, H) / 50, 4L, 40L);
        std::vector<float> tmp(n);
        for (long y = 0; y < H; ++y) {
            const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(W);
            for (long x = 0; x < W; ++x) {
                float m = 0.0F;
                for (long dx = -rd; dx <= rd; ++dx) {
                    m = std::max(m, sp.map[row + static_cast<std::size_t>(
                                               std::clamp(x + dx, 0L, W - 1))]);
                }
                tmp[row + static_cast<std::size_t>(x)] = m;
            }
        }
        for (long x = 0; x < W; ++x) {
            for (long y = 0; y < H; ++y) {
                float m = 0.0F;
                for (long dy = -rd; dy <= rd; ++dy) {
                    m = std::max(m, tmp[static_cast<std::size_t>(std::clamp(y + dy, 0L, H - 1)) *
                                            static_cast<std::size_t>(W) +
                                        static_cast<std::size_t>(x)]);
                }
                sp.map[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) +
                       static_cast<std::size_t>(x)] = m;
            }
        }
    }
    // Fold in the deviation term AFTER the dilation: max() only raises pixels the dilated
    // anisotropy left cheap (cloud interiors), never those it already taxes (the horizon halo
    // keeps its exact pre-addendum cost, so the tuned w=0.15 behaviour cannot regress). No
    // dilation for the deviation map: a band-pass response covers a blob's interior already.
    for (std::size_t i = 0; i < dev.size(); ++i) {
        sp.map[i] = std::max(sp.map[i], dev[i]);
    }
    return sp;
}

namespace {

// The coarse pass's penalty: the FULL-RES map max-pooled per l×l block — never recomputed on
// the downsampled image, whose blur erases exactly the fine structure (windows, battlements,
// a small person) the penalty exists to see.
[[nodiscard]] StructurePenalty downsamplePenaltyMax(const StructurePenalty& fine, int l, long cw,
                                                    long ch) {
    StructurePenalty sp;
    sp.w = cw;
    sp.h = ch;
    sp.weight = fine.weight;
    if (fine.map.empty() || l < 1) {
        return sp;
    }
    sp.map.assign(static_cast<std::size_t>(cw) * static_cast<std::size_t>(ch), 0.0F);
    for (long cy = 0; cy < ch; ++cy) {
        for (long cx = 0; cx < cw; ++cx) {
            float m = 0.0F;
            for (long dy = 0; dy < l; ++dy) {
                for (long dx = 0; dx < l; ++dx) {
                    const long fx = std::min(cx * l + dx, fine.w - 1);
                    const long fy = std::min(cy * l + dy, fine.h - 1);
                    m = std::max(m, fine.map[static_cast<std::size_t>(fy) *
                                                 static_cast<std::size_t>(fine.w) +
                                             static_cast<std::size_t>(fx)]);
                }
            }
            sp.map[static_cast<std::size_t>(cy) * static_cast<std::size_t>(cw) +
                   static_cast<std::size_t>(cx)] = m;
        }
    }
    return sp;
}

[[nodiscard]] std::vector<int> computeGraphCutLabels(const common::ImageF& image,
                                                     const Selection& holeMask,
                                                     const std::vector<Offset>& offsets,
                                                     std::vector<int>& outNodeOf, int maxCycles,
                                                     const std::atomic<bool>* cancel,
                                                     const std::function<bool(float)>& progress = {},
                                                      const StructurePenalty* sp = nullptr) {
    const long W = static_cast<long>(image.width);
    const long H = static_cast<long>(image.height);
    outNodeOf.assign(static_cast<std::size_t>(W) * static_cast<std::size_t>(H), -1);
    std::vector<std::pair<long, long>> pos;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            if (pixelHole(holeMask, x, y)) {
                outNodeOf[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + x] =
                    static_cast<int>(pos.size());
                pos.emplace_back(static_cast<long>(x), static_cast<long>(y));
            }
        }
    }
    const int numNodes = static_cast<int>(pos.size());
    if (numNodes == 0 || offsets.empty()) {
        return {};
    }
    const int numLabels = static_cast<int>(offsets.size());
    const int neigh[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    const std::vector<int>& nodeOf = outNodeOf;

    // Precompute, once and in parallel, the two quantities the α-expansion move loop would
    // otherwise recompute for every node/edge on every one of its K≈60 label passes:
    //   dataCost[node*K + l]  — validity (+inf for an out-of-bounds or hole source) plus the
    //                           hole↔known boundary coherence term for offset l at this node;
    //   nodeColor[node*K + l] — the (clamped) source sample image(x+offset_l), which the seam
    //                           pairwise term compares between two labels.
    // The seam term for an edge (a,b) under labels (la,lb) is then just
    //   ssd(nodeColor[a][la], nodeColor[a][lb]) + ssd(nodeColor[b][la], nodeColor[b][lb]),
    // identical to the previous position-dependent colorSSD form but with no image sampling, no
    // clamping, and no std::function dispatch in the inner loop. Pure efficiency; same energy
    // model.
    const bool dbg = std::getenv("MOSAIC_INPAINT_TIMING") != nullptr;
    const auto tA = std::chrono::steady_clock::now();
    const std::size_t nk = static_cast<std::size_t>(numNodes) * static_cast<std::size_t>(numLabels);
    std::vector<double> dataCost(nk);
    std::vector<common::ColorF> nodeColor(nk);
    parallelRanges(numNodes, [&](int n0, int n1) {
        for (int node = n0; node < n1; ++node) {
            const long x = pos[static_cast<std::size_t>(node)].first;
            const long y = pos[static_cast<std::size_t>(node)].second;
            const std::size_t base =
                static_cast<std::size_t>(node) * static_cast<std::size_t>(numLabels);
            for (int l = 0; l < numLabels; ++l) {
                const long sx = x + offsets[static_cast<std::size_t>(l)].u;
                const long sy = y + offsets[static_cast<std::size_t>(l)].v;
                nodeColor[base + static_cast<std::size_t>(l)] =
                    image.at(static_cast<std::uint32_t>(clampL(sx, W)),
                             static_cast<std::uint32_t>(clampL(sy, H)));
                double cost;
                if (sx < 0 || sy < 0 || sx >= W || sy >= H ||
                    pixelHole(holeMask, static_cast<std::uint32_t>(sx),
                              static_cast<std::uint32_t>(sy))) {
                    cost = kInvalid;
                } else {
                    double boundary = 0.0;
                    for (const auto& d : neigh) {
                        const long nx = x + d[0];
                        const long ny = y + d[1];
                        if (nx < 0 || ny < 0 || nx >= W || ny >= H) {
                            continue;
                        }
                        if (nodeOf[static_cast<std::size_t>(ny) * static_cast<std::size_t>(W) +
                                   nx] >= 0) {
                            continue; // hole neighbour -> handled by the pairwise seam term
                        }
                        boundary += colorSSD(image, sx, sy, nx, ny);
                        // First-order anchoring (He & Sun's own E1 form): the label's view OF
                        // the known neighbour (n + offset) must reproduce the known ring, not
                        // just abut it — this is what pins a horizon to its true height at the
                        // hole boundary instead of letting it step by a pixel or two.
                        const long gx = nx + offsets[static_cast<std::size_t>(l)].u;
                        const long gy = ny + offsets[static_cast<std::size_t>(l)].v;
                        if (gx >= 0 && gy >= 0 && gx < W && gy < H &&
                            !pixelHole(holeMask, static_cast<std::uint32_t>(gx),
                                       static_cast<std::uint32_t>(gy))) {
                            boundary += kSeamGradientWeight * colorSSD(image, gx, gy, nx, ny);
                        }
                    }
                    cost = kBoundaryWeight * boundary;
                    if (sp != nullptr) {
                        cost += sp->at(sx, sy); // outpaint structure penalty
                    }
                }
                dataCost[base + static_cast<std::size_t>(l)] = cost;
            }
        }
    });

    std::vector<std::pair<int, int>> edges;
    auto tryEdge = [&](int na, long xb, long yb) {
        const int nb = nodeOf[static_cast<std::size_t>(yb) * static_cast<std::size_t>(W) + xb];
        if (nb >= 0) {
            edges.emplace_back(na, nb);
        }
    };
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const int na = nodeOf[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + x];
            if (na < 0) {
                continue;
            }
            if (static_cast<long>(x) + 1 < W) {
                tryEdge(na, static_cast<long>(x) + 1, y);
            }
            if (static_cast<long>(y) + 1 < H) {
                tryEdge(na, x, static_cast<long>(y) + 1);
            }
        }
    }

    const auto dc = [&](int node, int label) -> double {
        return dataCost[static_cast<std::size_t>(node) * static_cast<std::size_t>(numLabels) +
                        static_cast<std::size_t>(label)];
    };
    const auto pw = [&](int ei, int la, int lb) -> double {
        if (la == lb) {
            return 0.0;
        }
        const std::size_t a = static_cast<std::size_t>(edges[static_cast<std::size_t>(ei)].first) *
                              static_cast<std::size_t>(numLabels);
        const std::size_t b = static_cast<std::size_t>(edges[static_cast<std::size_t>(ei)].second) *
                              static_cast<std::size_t>(numLabels);
        const auto ula = static_cast<std::size_t>(la);
        const auto ulb = static_cast<std::size_t>(lb);
        // Colours & gradients: the two labels must agree on the colours AT the seam pair AND on
        // the edge ACROSS it (kSeamGradientWeight) — the cut routes along matching structure
        // instead of across misaligned edges. All operands are the precomputed samples.
        return colorSSDc(nodeColor[a + ula], nodeColor[a + ulb]) +
               colorSSDc(nodeColor[b + ula], nodeColor[b + ulb]) +
               kSeamGradientWeight * gradientSSDc(nodeColor[a + ula], nodeColor[b + ula],
                                                  nodeColor[a + ulb], nodeColor[b + ulb]);
    };
    const auto tB = std::chrono::steady_clock::now();
    int cycles = 0;
    std::vector<int> labels = alphaExpansionImpl(numNodes, numLabels, dc, pw, edges,
                                                 std::max(1, maxCycles), &cycles, cancel, progress);
    const auto tC = std::chrono::steady_clock::now();
    if (dbg) {
        const double preMs = std::chrono::duration<double, std::milli>(tB - tA).count();
        const double solveMs = std::chrono::duration<double, std::milli>(tC - tB).count();
        std::fprintf(stderr,
                     "  [graph-cut] nodes=%d labels=%d edges=%zu precompute=%.0fms solve=%.0fms "
                     "cycles=%d\n",
                     numNodes, numLabels, edges.size(), preMs, solveMs, cycles);
    }
    return labels;
}

// Plain Poisson (gradient-domain) seam blend, solved by RED-BLACK Gauss-Seidel over the hole.
//
// ⚠ INVARIANT — this is the BASIC Poisson reconstruction, and it deliberately does NOT use
// Agarwala's quadtree-along-seams acceleration: we solve the full region with simple relaxation.
// That costs time and is a hard constraint on this file, not an oversight — do not "optimise" it
// into an adaptively-meshed solve. Red-black (checkerboard) ordering is just the iteration ORDER
// of the same Gauss-Seidel relaxation — a hole pixel's 4-connected neighbours are all the opposite
// colour, so a whole colour updates with no cross-dependency and runs across threads. No algorithm
// change; only the sweep is parallelized.
//
// Guidance: each hole pixel's target gradient toward a neighbour is taken from that pixel's OWN
// source (offset s_p): d = I(p+s_p) - I(q+s_p). Where neighbours share an offset this reproduces
// the source gradient (seamless regions are preserved); across a seam it removes the spurious jump.
// An out-of-bounds source sample falls back to the composite's local gradient so a good composite
// is a fixed point. Known pixels are fixed Dirichlet boundary; initialized from the composite.
// `boundaryCrisp` (≤0 = off, the historical behaviour): an outpaint-only extension of the
// crisp-seam rule to hole↔KNOWN boundary edges. At such an edge the guidance is the
// sheet's own view of it, so the correction the solve must diffuse inward is exactly the sheet's
// ring residual (its view of the known neighbour minus the neighbour itself — the E1 anchoring
// residual the cut already minimized but texture never zeroes). Along a canvas-expansion ring the
// boundary is a long straight line and those residuals are large wherever the ring is textured
// (grass) or the sheet carries faint content the ring lacks (a pale cloud): their harmonic
// extension is the recorded "vertical tone smear at the old frame edge" and the half-erased
// cloud ghost. When the sheet's view of the boundary edge disagrees with the composite's own
// gradient there by more than this threshold, the guidance keeps the composite's gradient — the
// same crisp/blend selection, same two candidate gradients already in this function, applied at
// one more edge class. Small DC mismatches (smooth sky) stay below the threshold and keep
// bridging exactly as before.
[[nodiscard]] common::ImageF
poissonSeamBlend(const common::ImageF& image, const std::vector<int>& nodeOf, long w, long h,
                 const std::vector<Offset>& offsets, const std::vector<int>& labels,
                 const common::ImageF& composite, int iters, double eps, double omega,
                 ProgressReporter* prog, float baseFrac, float spanFrac, double boundaryCrisp) {
    common::ImageF out = composite;
    const auto offsetAt = [&](long x, long y) -> Offset {
        const int node = nodeOf[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x];
        if (node < 0 || static_cast<std::size_t>(node) >= labels.size()) {
            return {kNoSource, kNoSource};
        }
        const int l = labels[static_cast<std::size_t>(node)];
        if (l < 0 || static_cast<std::size_t>(l) >= offsets.size()) {
            return {kNoSource, kNoSource}; // neighbour-filled pixel: no source -> composite fallback
        }
        return offsets[static_cast<std::size_t>(l)];
    };

    // Hole pixels split by checkerboard colour (red = (x+y) even). Within a colour every update is
    // independent, so each colour's pass parallelizes; the two passes alternate as Gauss-Seidel.
    std::vector<std::pair<int, int>> red;
    std::vector<std::pair<int, int>> black;
    for (long y = 0; y < h; ++y) {
        for (long x = 0; x < w; ++x) {
            if (nodeOf[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x] < 0) {
                continue;
            }
            ((x + y) & 1L ? black : red).emplace_back(static_cast<int>(x), static_cast<int>(y));
        }
    }

    // The source-space gradient across edge (a,b) under a single offset o: S(a+o) - S(b+o), where
    // S is the VIRTUAL COMPLETED source — a known pixel reads the image, a hole pixel reads the
    // COMPOSITE (its chain-resolved fill). The original code read the raw image everywhere, so a
    // view whose sample fell back inside the hole read the REMOVED content; with the old
    // under-converged fixed-sweep solve that poison stayed low-frequency-invisible, but the
    // multigrid solves the system it is given exactly and the removed object came back as a
    // smooth dark ghost (found 2026-07-02 on the Skagen church photo). The composite is fixed
    // for the whole solve: guidance stays constant, deterministic, and race-free under the
    // parallel sweeps. Returns false (writes nothing) only for out-of-bounds samples.
    const auto srcAtV = [&](long sx, long sy) -> common::ColorF {
        const bool inHole =
            nodeOf[static_cast<std::size_t>(sy) * static_cast<std::size_t>(w) + sx] >= 0;
        return inHole ? composite.at(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy))
                      : image.at(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy));
    };
    const auto viewGrad = [&](long ax, long ay, long bx, long by, const Offset& o, double& gr,
                              double& gg, double& gb) -> bool {
        if (o.u == kNoSource) {
            return false; // no-source pixel: skip this view so the composite-gradient fallback runs
        }
        const long asx = ax + o.u, asy = ay + o.v, bsx = bx + o.u, bsy = by + o.v;
        if (asx < 0 || asy < 0 || asx >= w || asy >= h || bsx < 0 || bsy < 0 || bsx >= w ||
            bsy >= h) {
            return false;
        }
        const common::ColorF A = srcAtV(asx, asy);
        const common::ColorF B = srcAtV(bsx, bsy);
        gr = static_cast<double>(A.r) - B.r;
        gg = static_cast<double>(A.g) - B.g;
        gb = static_cast<double>(A.b) - B.b;
        return true;
    };

    const int neigh[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

    // ---- One-pass guidance precompute (the stencil refactor, 2026-07-02) -----------------------
    // The guidance depends only on the (fixed) image, composite and effective offsets — never on
    // the evolving solution — so the whole offset-sampling half of the update is hoisted out of
    // the sweeps into per-pixel constants over the hole bbox:
    //   Rv(p) = Σ_{q known} image(q) + Σ_q d_pq   (d = the ANTISYMMETRIC guidance: the average of
    //           the two endpoints' source-space views of the edge — equal to the source gradient
    //           within one offset sheet, so a seamless composite stays a fixed point; both-views-
    //           out-of-bounds falls back to the composite's own gradient),
    //   nn(p) = # in-bounds 4-neighbours.
    // Every sweep is then the plain 5-point stencil u(p) = (Σ_{q∈hole} u(q) + Rv(p))/nn(p) — the
    // exact same iteration, memory-bound instead of sampling-bound (~5× faster sweeps), and
    // precisely the shape a GPU compute pass would want if one is ever warranted.
    long gx0 = w, gy0 = h, gx1 = -1, gy1 = -1; // hole bbox (grown by 1, clamped)
    for (const auto& [x, y] : red) {
        gx0 = std::min(gx0, static_cast<long>(x));
        gy0 = std::min(gy0, static_cast<long>(y));
        gx1 = std::max(gx1, static_cast<long>(x));
        gy1 = std::max(gy1, static_cast<long>(y));
    }
    for (const auto& [x, y] : black) {
        gx0 = std::min(gx0, static_cast<long>(x));
        gy0 = std::min(gy0, static_cast<long>(y));
        gx1 = std::max(gx1, static_cast<long>(x));
        gy1 = std::max(gy1, static_cast<long>(y));
    }
    if (gx1 < gx0) {
        return out; // no hole pixels (callers guard, but stay safe)
    }
    gx0 = std::max(0L, gx0 - 1);
    gy0 = std::max(0L, gy0 - 1);
    gx1 = std::min(w - 1, gx1 + 1);
    gy1 = std::min(h - 1, gy1 + 1);
    const long gw = gx1 - gx0 + 1;
    const long gh = gy1 - gy0 + 1;
    const auto gidx = [&](long x, long y) {
        return static_cast<std::size_t>(y - gy0) * static_cast<std::size_t>(gw) +
               static_cast<std::size_t>(x - gx0);
    };
    std::vector<float> Rv(static_cast<std::size_t>(gw) * static_cast<std::size_t>(gh) * 3, 0.0f);
    std::vector<std::uint8_t> nn(static_cast<std::size_t>(gw) * static_cast<std::size_t>(gh), 0);
    {
        const auto fillR = [&](const std::vector<std::pair<int, int>>& pts, std::size_t lo,
                               std::size_t hi) {
            for (std::size_t k = lo; k < hi; ++k) {
                const long x = pts[k].first;
                const long y = pts[k].second;
                const Offset s = offsetAt(x, y);
                double R[3] = {0, 0, 0};
                int n = 0;
                for (const auto& dd : neigh) {
                    const long nx = x + dd[0];
                    const long ny = y + dd[1];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                        continue;
                    }
                    ++n;
                    const bool nHole =
                        nodeOf[static_cast<std::size_t>(ny) * static_cast<std::size_t>(w) + nx] >=
                        0;
                    double dr = 0, dg = 0, db = 0;
                    double d1[3], d2[3];
                    const bool v1 = viewGrad(x, y, nx, ny, s, d1[0], d1[1], d1[2]);
                    const bool v2 =
                        nHole ? viewGrad(x, y, nx, ny, offsetAt(nx, ny), d2[0], d2[1], d2[2])
                              : false;
                    if (v1 && v2) {
                        // CRISP-SEAM rule (addendum): when the two endpoints' source
                        // views STRUCTURALLY disagree about this edge (one sheet sees flat sky,
                        // the other a canopy edge), averaging them invents a gradient neither
                        // sheet contains and the blend diffuses the conflict into a smudge — the
                        // grey wedge at the Skagen treeline junction. Past a disagreement
                        // threshold the guidance keeps the COMPOSITE's own gradient: the
                        // transition stays a crisp content edge (a fixed point of the solve)
                        // instead of a blur. Below it, the usual antisymmetric average blends
                        // DC-level mismatches exactly as before (banded sky unaffected — its
                        // views agree; both branches stay antisymmetric and both endpoints take
                        // the same branch, so the field remains conservative).
                        const double dis =
                            std::max({std::fabs(d1[0] - d2[0]), std::fabs(d1[1] - d2[1]),
                                      std::fabs(d1[2] - d2[2])});
                        constexpr double kCrispViewDisagree = 0.25;
                        if (dis > kCrispViewDisagree) {
                            const common::ColorF cp = composite.at(static_cast<std::uint32_t>(x),
                                                                   static_cast<std::uint32_t>(y));
                            const common::ColorF cq = composite.at(static_cast<std::uint32_t>(nx),
                                                                   static_cast<std::uint32_t>(ny));
                            dr = static_cast<double>(cp.r) - cq.r;
                            dg = static_cast<double>(cp.g) - cq.g;
                            db = static_cast<double>(cp.b) - cq.b;
                        } else {
                            dr = 0.5 * (d1[0] + d2[0]);
                            dg = 0.5 * (d1[1] + d2[1]);
                            db = 0.5 * (d1[2] + d2[2]);
                        }
                    } else if (v1 || v2) {
                        const double* dv = v1 ? d1 : d2;
                        dr = dv[0];
                        dg = dv[1];
                        db = dv[2];
                        // BOUNDARY-CRISP rule (outpaint only, see the header comment): a known
                        // neighbour has no second view, so the sheet's view is taken unopposed —
                        // and whatever it gets wrong about the ring becomes a correction the
                        // solve diffuses into the fill. Compare it against the composite's own
                        // boundary gradient (their difference IS the ring residual); past the
                        // threshold keep the composite's gradient, so the boundary stays a
                        // content edge instead of sourcing a smear.
                        if (boundaryCrisp > 0.0 && !nHole && v1) {
                            const common::ColorF cp = composite.at(static_cast<std::uint32_t>(x),
                                                                   static_cast<std::uint32_t>(y));
                            const common::ColorF cq = composite.at(static_cast<std::uint32_t>(nx),
                                                                   static_cast<std::uint32_t>(ny));
                            const double gr = static_cast<double>(cp.r) - cq.r;
                            const double gg = static_cast<double>(cp.g) - cq.g;
                            const double gb = static_cast<double>(cp.b) - cq.b;
                            const double dis = std::max({std::fabs(d1[0] - gr),
                                                         std::fabs(d1[1] - gg),
                                                         std::fabs(d1[2] - gb)});
                            if (dis > boundaryCrisp) {
                                dr = gr;
                                dg = gg;
                                db = gb;
                            }
                        }
                    } else {
                        const common::ColorF cp = composite.at(static_cast<std::uint32_t>(x),
                                                               static_cast<std::uint32_t>(y));
                        const common::ColorF cq = composite.at(static_cast<std::uint32_t>(nx),
                                                               static_cast<std::uint32_t>(ny));
                        dr = static_cast<double>(cp.r) - cq.r;
                        dg = static_cast<double>(cp.g) - cq.g;
                        db = static_cast<double>(cp.b) - cq.b;
                    }
                    if (!nHole) {
                        const common::ColorF kv = image.at(static_cast<std::uint32_t>(nx),
                                                           static_cast<std::uint32_t>(ny));
                        R[0] += kv.r;
                        R[1] += kv.g;
                        R[2] += kv.b;
                    }
                    R[0] += dr;
                    R[1] += dg;
                    R[2] += db;
                }
                const std::size_t o = gidx(x, y);
                Rv[o * 3 + 0] = static_cast<float>(R[0]);
                Rv[o * 3 + 1] = static_cast<float>(R[1]);
                Rv[o * 3 + 2] = static_cast<float>(R[2]);
                nn[o] = static_cast<std::uint8_t>(n);
            }
        };
        const auto runPts = [&](const std::vector<std::pair<int, int>>& pts) {
            const std::size_t count = pts.size();
            const unsigned hw = common::hardwareThreads();
            const std::size_t bands = std::max<std::size_t>(
                1, std::min<std::size_t>(hw, std::max<std::size_t>(1, count / 4096)));
            if (bands <= 1) {
                fillR(pts, 0, count);
                return;
            }
            const std::size_t step = (count + bands - 1) / bands;
            common::parallelBands((count + step - 1) / step, [&](std::size_t b) {
                const std::size_t lo = b * step;
                fillR(pts, lo, std::min(count, lo + step));
            });
        };
        runPts(red);
        runPts(black);
    }

    const auto updateAt = [&](int xi, int yi) -> double {
        const long x = xi;
        const long y = yi;
        const std::size_t o = gidx(x, y);
        const int n = nn[o];
        if (n == 0) {
            return 0.0;
        }
        double sr = Rv[o * 3 + 0], sg = Rv[o * 3 + 1], sb = Rv[o * 3 + 2];
        for (const auto& dd : neigh) {
            const long nx = x + dd[0];
            const long ny = y + dd[1];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                continue;
            }
            if (nodeOf[static_cast<std::size_t>(ny) * static_cast<std::size_t>(w) + nx] < 0) {
                continue; // known neighbour: already folded into Rv
            }
            const common::ColorF fv =
                out.at(static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny));
            sr += fv.r;
            sg += fv.g;
            sb += fv.b;
        }
        const float inv = 1.0f / static_cast<float>(n);
        const common::ColorF old =
            out.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
        // SOR over-relaxation accelerates the slow low-frequency convergence (omega == 1 is plain
        // Gauss-Seidel). Each channel is then PROJECTED to [0,1]: the result is an image, so this
        // is a valid box constraint (projected Gauss-Seidel) and a hard guarantee against the
        // white/black/magenta blowout a gradient-domain solve can otherwise produce.
        const float wf = static_cast<float>(omega);
        const auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
        const common::ColorF nv{clamp01(old.r + wf * (static_cast<float>(sr) * inv - old.r)),
                                clamp01(old.g + wf * (static_cast<float>(sg) * inv - old.g)),
                                clamp01(old.b + wf * (static_cast<float>(sb) * inv - old.b)),
                                old.a};
        out.set(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), nv);
        return std::max({static_cast<double>(std::fabs(nv.r - old.r)),
                         static_cast<double>(std::fabs(nv.g - old.g)),
                         static_cast<double>(std::fabs(nv.b - old.b))});
    };

    // One colour pass, parallelized into bands; returns the largest per-pixel change.
    const auto sweep = [&](const std::vector<std::pair<int, int>>& pts) -> double {
        const int count = static_cast<int>(pts.size());
        if (count == 0) {
            return 0.0;
        }
        const unsigned hw = common::hardwareThreads();
        const int bands =
            std::max(1, std::min<int>(static_cast<int>(hw), std::max(1, count / 2048)));
        std::vector<double> bandMax(static_cast<std::size_t>(bands), 0.0);
        const auto work = [&](int band, int lo, int hi) {
            double m = 0.0;
            for (int k = lo; k < hi; ++k) {
                m = std::max(m, updateAt(pts[static_cast<std::size_t>(k)].first,
                                         pts[static_cast<std::size_t>(k)].second));
            }
            bandMax[static_cast<std::size_t>(band)] = m;
        };
        if (bands <= 1) {
            work(0, 0, count);
        } else {
            const int step = (count + bands - 1) / bands;
            common::parallelBands(static_cast<std::size_t>((count + step - 1) / step),
                                  [&](std::size_t b) {
                                      const int lo = static_cast<int>(b) * step;
                                      work(static_cast<int>(b), lo, std::min(count, lo + step));
                                  });
        }
        double m = 0.0;
        for (const double d : bandMax) {
            m = std::max(m, d);
        }
        return m;
    };

    // MULTIGRID pre-solve for large holes. Plain relaxation
    // kills high-frequency error fast but low-frequency error decays as ~O(1/N²) sweeps — on a
    // hole hundreds of pixels across, hundreds of sweeps leave visible low-frequency residue
    // (the "banded sky": each offset sheet keeps a slightly wrong DC level). The fix is the
    // textbook geometric multigrid (Fedorenko 1964; Brandt 1977 — decades-old classical
    // numerics; deliberately NOT a quadtree-meshed gradient-domain solver — every level here is
    // a full regular grid, no adaptive meshing, and that stays true), applied
    // to the CORRECTION field c = u − composite:
    //     n·c(p) − Σ_{q∈hole} c(q) = r(p),   c = 0 on known pixels,
    // whose right-hand side r (the residual of the composite under the guidance) is nonzero
    // essentially only along seams — and, crucially, needs NO offset sampling at coarse levels,
    // so restriction/prolongation are plain mask-aware grid ops. V-cycles leave c with the low
    // frequencies solved; the projected SOR polish below then enforces the exact offset-based
    // equation and the [0,1] box constraint in a few dozen sweeps instead of hundreds.
    const std::size_t holeCount = red.size() + black.size();
    constexpr std::size_t kMgMinHole = 20000; // small holes: plain SOR already converges fast
    if (holeCount >= kMgMinHole) {
        // Hole bbox, grown by 1 and clamped: the multigrid works on this window only.
        long bx0 = w, by0 = h, bx1 = -1, by1 = -1;
        for (const auto& [x, y] : red) {
            bx0 = std::min(bx0, static_cast<long>(x));
            by0 = std::min(by0, static_cast<long>(y));
            bx1 = std::max(bx1, static_cast<long>(x));
            by1 = std::max(by1, static_cast<long>(y));
        }
        for (const auto& [x, y] : black) {
            bx0 = std::min(bx0, static_cast<long>(x));
            by0 = std::min(by0, static_cast<long>(y));
            bx1 = std::max(bx1, static_cast<long>(x));
            by1 = std::max(by1, static_cast<long>(y));
        }
        bx0 = std::max(0L, bx0 - 1);
        by0 = std::max(0L, by0 - 1);
        bx1 = std::min(w - 1, bx1 + 1);
        by1 = std::min(h - 1, by1 + 1);

        struct MgLevel {
            long w = 0, h = 0;
            std::vector<std::uint8_t> hole; // 1 = unknown cell (solve), 0 = Dirichlet c=0
            std::vector<float> c, r;        // 3 channels per cell
        };
        const auto idx = [](const MgLevel& L, long x, long y) {
            return static_cast<std::size_t>(y) * static_cast<std::size_t>(L.w) +
                   static_cast<std::size_t>(x);
        };
        std::vector<MgLevel> levels;
        {
            MgLevel L0;
            L0.w = bx1 - bx0 + 1;
            L0.h = by1 - by0 + 1;
            L0.hole.assign(static_cast<std::size_t>(L0.w) * static_cast<std::size_t>(L0.h), 0);
            for (long y = by0; y <= by1; ++y) {
                for (long x = bx0; x <= bx1; ++x) {
                    if (nodeOf[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x] >=
                        0) {
                        L0.hole[idx(L0, x - bx0, y - by0)] = 1;
                    }
                }
            }
            L0.c.assign(L0.hole.size() * 3, 0.0f);
            L0.r.assign(L0.hole.size() * 3, 0.0f);
            levels.push_back(std::move(L0));
            while (levels.back().w > 24 && levels.back().h > 24) {
                const MgLevel& f = levels.back();
                MgLevel c;
                c.w = (f.w + 1) / 2;
                c.h = (f.h + 1) / 2;
                c.hole.assign(static_cast<std::size_t>(c.w) * static_cast<std::size_t>(c.h), 0);
                for (long y = 0; y < f.h; ++y) {
                    for (long x = 0; x < f.w; ++x) {
                        if (f.hole[idx(f, x, y)] != 0) {
                            c.hole[idx(c, x / 2, y / 2)] = 1;
                        }
                    }
                }
                c.c.assign(c.hole.size() * 3, 0.0f);
                c.r.assign(c.hole.size() * 3, 0.0f);
                levels.push_back(std::move(c));
            }
        }

        // Level-0 right-hand side, derived from the precomputed guidance constants:
        //   r0(p) = Rv(p) − n·u0(p) + Σ_{q∈hole} u0(q)
        // — the residual of the composite under exactly the system the polish iterates, so the
        // polish below is a fixed point of the same equation. (u0 = composite.)
        {
            MgLevel& L0 = levels.front();
            const auto fillRes = [&](const std::vector<std::pair<int, int>>& pts) {
                for (const auto& [xi, yi] : pts) {
                    const long x = xi;
                    const long y = yi;
                    const std::size_t g = gidx(x, y);
                    const int n = nn[g];
                    double R[3] = {Rv[g * 3 + 0], Rv[g * 3 + 1], Rv[g * 3 + 2]};
                    for (const auto& dd : neigh) {
                        const long nx = x + dd[0];
                        const long ny = y + dd[1];
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                            continue;
                        }
                        if (nodeOf[static_cast<std::size_t>(ny) * static_cast<std::size_t>(w) +
                                   nx] < 0) {
                            continue;
                        }
                        const common::ColorF u0q = composite.at(static_cast<std::uint32_t>(nx),
                                                                static_cast<std::uint32_t>(ny));
                        R[0] += u0q.r;
                        R[1] += u0q.g;
                        R[2] += u0q.b;
                    }
                    const common::ColorF u0 = composite.at(static_cast<std::uint32_t>(x),
                                                           static_cast<std::uint32_t>(y));
                    const std::size_t o = idx(L0, x - bx0, y - by0) * 3;
                    L0.r[o + 0] = static_cast<float>(R[0] - n * u0.r);
                    L0.r[o + 1] = static_cast<float>(R[1] - n * u0.g);
                    L0.r[o + 2] = static_cast<float>(R[2] - n * u0.b);
                }
            };
            fillRes(red);
            fillRes(black);
        }

        // Mask-aware red-black Gauss-Seidel smoother on one level (c = 0 outside the mask). The
        // neighbour count uses grid bounds — the grown bbox is clamped to the image, so a frame
        // edge is a grid edge here exactly as it is in the polish loop.
        const auto smooth = [&](MgLevel& L, int iters2) {
            for (int it2 = 0; it2 < iters2; ++it2) {
                for (int color = 0; color < 2; ++color) {
                    for (long y = 0; y < L.h; ++y) {
                        for (long x = (y + color) & 1L; x < L.w; x += 2) {
                            const std::size_t o = idx(L, x, y);
                            if (L.hole[o] == 0) {
                                continue;
                            }
                            float sum[3] = {L.r[o * 3], L.r[o * 3 + 1], L.r[o * 3 + 2]};
                            int n = 0;
                            const long nbs[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                            for (const auto& nb : nbs) {
                                if (nb[0] < 0 || nb[1] < 0 || nb[0] >= L.w || nb[1] >= L.h) {
                                    continue;
                                }
                                ++n;
                                const std::size_t q = idx(L, nb[0], nb[1]);
                                if (L.hole[q] != 0) {
                                    sum[0] += L.c[q * 3];
                                    sum[1] += L.c[q * 3 + 1];
                                    sum[2] += L.c[q * 3 + 2];
                                }
                            }
                            if (n > 0) {
                                const float inv = 1.0f / static_cast<float>(n);
                                L.c[o * 3] = sum[0] * inv;
                                L.c[o * 3 + 1] = sum[1] * inv;
                                L.c[o * 3 + 2] = sum[2] * inv;
                            }
                        }
                    }
                }
            }
        };
        // Residual of the level equation, restricted into the coarse rhs as HALF the 2×2 block
        // sum. The factor is set by the geometry of this problem's sources: the rhs is
        // concentrated along CURVES (seams and the hole's boundary ring), and a line source's
        // strength per coarse cell doubles under a plain 2×2 sum (two fine line-cells collapse
        // into one), which compounds to a 2^levels blow-up of the correction — observed as a
        // saturated dark ghost before this factor was fixed (the multigrid unit test pins the
        // ramp amplitude). Half the sum keeps a line source's per-cell strength constant across
        // levels; the V-cycle's fine smoothing absorbs the (second-order) misfit this leaves
        // for the rare area-distributed residue.
        const auto restrictTo = [&](const MgLevel& f, MgLevel& c) {
            std::fill(c.r.begin(), c.r.end(), 0.0f);
            std::fill(c.c.begin(), c.c.end(), 0.0f);
            for (long y = 0; y < f.h; ++y) {
                for (long x = 0; x < f.w; ++x) {
                    const std::size_t o = idx(f, x, y);
                    if (f.hole[o] == 0) {
                        continue;
                    }
                    float res[3] = {f.r[o * 3], f.r[o * 3 + 1], f.r[o * 3 + 2]};
                    int n = 0;
                    const long nbs[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                    for (const auto& nb : nbs) {
                        if (nb[0] < 0 || nb[1] < 0 || nb[0] >= f.w || nb[1] >= f.h) {
                            continue;
                        }
                        ++n;
                        const std::size_t q = idx(f, nb[0], nb[1]);
                        if (f.hole[q] != 0) {
                            res[0] += f.c[q * 3];
                            res[1] += f.c[q * 3 + 1];
                            res[2] += f.c[q * 3 + 2];
                        }
                    }
                    res[0] -= n * f.c[o * 3];
                    res[1] -= n * f.c[o * 3 + 1];
                    res[2] -= n * f.c[o * 3 + 2];
                    const std::size_t co = idx(c, x / 2, y / 2);
                    if (c.hole[co] != 0) {
                        c.r[co * 3] += 0.5f * res[0];
                        c.r[co * 3 + 1] += 0.5f * res[1];
                        c.r[co * 3 + 2] += 0.5f * res[2];
                    }
                }
            }
        };
        // Cell-centered bilinear prolongation with a per-channel LINE SEARCH: the prolongated
        // coarse correction p is added as c += α·p with α = <res, A·p> / <A·p, A·p> minimizing
        // the fine residual norm along p (textbook one-dimensional subspace correction /
        // steepest-descent step, deterministic). This self-calibrates the inter-level scaling —
        // the mask-aware transfers cannot be given one exact constant (the rhs mixes line-
        // concentrated seam sources with the coarse hole's receding Dirichlet wall, and a fixed
        // factor over- or under-shoots per level, which compounded into the saturated-ghost
        // failure this replaced) — and can never make the residual worse (worst case α → 0).
        std::vector<float> pbuf, qbuf;
        const auto prolongAdd = [&](const MgLevel& c, MgLevel& f) {
            pbuf.assign(f.c.size(), 0.0f);
            for (long y = 0; y < f.h; ++y) {
                for (long x = 0; x < f.w; ++x) {
                    const std::size_t o = idx(f, x, y);
                    if (f.hole[o] == 0) {
                        continue;
                    }
                    const long cx = x / 2, cy = y / 2;
                    const long dx = (x & 1L) ? 1 : -1, dy = (y & 1L) ? 1 : -1;
                    const struct {
                        long x, y;
                        float wgt;
                    } taps[4] = {{cx, cy, 0.5625f},
                                 {cx + dx, cy, 0.1875f},
                                 {cx, cy + dy, 0.1875f},
                                 {cx + dx, cy + dy, 0.0625f}};
                    float add[3] = {0, 0, 0};
                    for (const auto& t : taps) {
                        if (t.x < 0 || t.y < 0 || t.x >= c.w || t.y >= c.h) {
                            continue;
                        }
                        const std::size_t q = idx(c, t.x, t.y);
                        if (c.hole[q] == 0) {
                            continue;
                        }
                        add[0] += t.wgt * c.c[q * 3];
                        add[1] += t.wgt * c.c[q * 3 + 1];
                        add[2] += t.wgt * c.c[q * 3 + 2];
                    }
                    pbuf[o * 3] = add[0];
                    pbuf[o * 3 + 1] = add[1];
                    pbuf[o * 3 + 2] = add[2];
                }
            }
            // q = A·p and the two inner products per channel; then c += α·p.
            qbuf.assign(f.c.size(), 0.0f);
            double resDotQ[3] = {0, 0, 0};
            double qDotQ[3] = {0, 0, 0};
            for (long y = 0; y < f.h; ++y) {
                for (long x = 0; x < f.w; ++x) {
                    const std::size_t o = idx(f, x, y);
                    if (f.hole[o] == 0) {
                        continue;
                    }
                    int n = 0;
                    float sumC[3] = {0, 0, 0};
                    float sumP[3] = {0, 0, 0};
                    const long nbs[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                    for (const auto& nb : nbs) {
                        if (nb[0] < 0 || nb[1] < 0 || nb[0] >= f.w || nb[1] >= f.h) {
                            continue;
                        }
                        ++n;
                        const std::size_t q = idx(f, nb[0], nb[1]);
                        if (f.hole[q] != 0) {
                            for (int ch = 0; ch < 3; ++ch) {
                                sumC[ch] += f.c[q * 3 + static_cast<std::size_t>(ch)];
                                sumP[ch] += pbuf[q * 3 + static_cast<std::size_t>(ch)];
                            }
                        }
                    }
                    for (int ch = 0; ch < 3; ++ch) {
                        const std::size_t oc = o * 3 + static_cast<std::size_t>(ch);
                        const double res = f.r[oc] - (n * f.c[oc] - sumC[ch]);
                        const double qv = n * pbuf[oc] - sumP[ch];
                        qbuf[oc] = static_cast<float>(qv);
                        resDotQ[ch] += res * qv;
                        qDotQ[ch] += qv * qv;
                    }
                }
            }
            float alpha[3];
            for (int ch = 0; ch < 3; ++ch) {
                alpha[ch] =
                    qDotQ[ch] > 1e-12 ? static_cast<float>(resDotQ[ch] / qDotQ[ch]) : 0.0f;
            }
            for (std::size_t i = 0; i < f.c.size(); i += 3) {
                f.c[i] += alpha[0] * pbuf[i];
                f.c[i + 1] += alpha[1] * pbuf[i + 1];
                f.c[i + 2] += alpha[2] * pbuf[i + 2];
            }
        };
        std::function<void(std::size_t)> vcycle = [&](std::size_t li) {
            MgLevel& L = levels[li];
            if (li + 1 >= levels.size()) {
                smooth(L, 80); // coarsest: cheap, solve well
                return;
            }
            smooth(L, 3);
            restrictTo(L, levels[li + 1]);
            vcycle(li + 1);
            prolongAdd(levels[li + 1], L);
            smooth(L, 4);
        };
        constexpr int kVCycles = 3;
        for (int cyc = 0; cyc < kVCycles; ++cyc) {
            vcycle(0);
            if (prog != nullptr &&
                !prog->report(baseFrac + spanFrac * 0.15f * static_cast<float>(cyc + 1),
                              "Blending")) {
                return out; // cancelled mid-multigrid: return the composite-initialized blend
            }
        }
        // Fold the correction into the working image (projected — u must stay a valid image).
        const MgLevel& L0 = levels.front();
        const auto clamp01f = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
        for (long y = 0; y < L0.h; ++y) {
            for (long x = 0; x < L0.w; ++x) {
                const std::size_t o = idx(L0, x, y);
                if (L0.hole[o] == 0) {
                    continue;
                }
                const common::ColorF u0 = out.at(static_cast<std::uint32_t>(bx0 + x),
                                                 static_cast<std::uint32_t>(by0 + y));
                out.set(static_cast<std::uint32_t>(bx0 + x), static_cast<std::uint32_t>(by0 + y),
                        {clamp01f(u0.r + L0.c[o * 3]), clamp01f(u0.g + L0.c[o * 3 + 1]),
                         clamp01f(u0.b + L0.c[o * 3 + 2]), u0.a});
            }
        }
        if (prog != nullptr && !prog->report(baseFrac + spanFrac * 0.35f, "Blending", &out)) {
            return out;
        }
    }

    // Projected SOR polish: enforces the exact offset-based equation and the [0,1] box constraint
    // (and is the WHOLE solve for small holes). With the multigrid pre-solve the low frequencies
    // are already settled, so this needs a few dozen sweeps, not hundreds.
    const int polishIters =
        holeCount >= kMgMinHole ? std::min(iters, kMgPolishIters) : iters;
    int it = 0;
    for (; it < polishIters; ++it) {
        const double dr = sweep(red);
        const double db = sweep(black);
        // Stream the refining blend as a live preview every few sweeps (throttled), and honour
        // cancellation. `out` is the full image with the current hole estimate — a valid preview.
        if (prog != nullptr && (it % 6 == 0 || it + 1 == polishIters)) {
            const float frac = baseFrac + spanFrac * (0.35f + 0.65f * (static_cast<float>(it + 1) /
                                                     static_cast<float>(std::max(1, polishIters))));
            if (!prog->report(frac, "Blending", &out)) {
                break; // cancelled: return the partial blend; the host discards it
            }
        }
        if (std::max(dr, db) < eps) {
            break;
        }
    }
    if (std::getenv("MOSAIC_INPAINT_TIMING") != nullptr) {
        std::fprintf(stderr, "  [poisson] hole=%zu mg=%d iters=%d\n", red.size() + black.size(),
                     holeCount >= kMgMinHole ? 1 : 0, it);
    }
    return out;
}

// Banded full-resolution re-cut (He & Sun §4.2 fine refinement, the graph-cut half). The coarse cut
// nearest-upsamples labels, so a seam lands on a coarse-block boundary up to ~l px away from the
// feature it should follow (the stair-stepped horizon), and the ICM polish below cannot repair
// it: moving a seam FRONT means relabeling a whole run of pixels at once, and every single-pixel
// step raises the energy. This pass CAN move fronts: for each pair of labels (A, B) that meet
// along a seam, the hole pixels labeled A or B within a band of the seam are re-solved as an
// EXACT binary min-cut over the same energy (validity + boundary colour/E1 data, colours &
// gradients pairwise; neighbours outside the band contribute their fixed labels through the
// unary term). Binary + submodular (pairwise(A,A) == pairwise(B,B) == 0, costs nonnegative), so
// alphaExpansionImpl converges to the subproblem's global optimum in its first cycles — the seam
// re-routes onto the true feature at full resolution. Pairs are processed most-contested first,
// each subproblem bounded (band radius shrinks under a node cap); deterministic throughout.
//
// Lineage: the α-expansion/min-cut machinery is our own Dinic solver; banded full-res refinement
// after a coarse cut follows He & Sun ECCV 2012 §4.2 and Liu & Caselles TIP 2013. The energy is
// identical to the coarse cut's — no new mechanism enters here.
void refineSeamsBandedCuts(const common::ImageF& image, const Selection& holeMask,
                           const std::vector<int>& nodeOf, long W, long H,
                           const std::vector<Offset>& offsets, std::vector<int>& labels,
                           int coarseScale, const std::atomic<bool>* cancel,
                           const StructurePenalty* sp = nullptr) {
    const int K = static_cast<int>(offsets.size());
    if (K == 0 || labels.empty()) {
        return;
    }
    // Node -> pixel in the row-major hole-scan order shared with `labels`.
    std::vector<std::pair<int, int>> pos;
    pos.reserve(labels.size());
    for (std::uint32_t y = 0; y < static_cast<std::uint32_t>(H); ++y) {
        for (std::uint32_t x = 0; x < static_cast<std::uint32_t>(W); ++x) {
            if (pixelHole(holeMask, x, y)) {
                pos.emplace_back(static_cast<int>(x), static_cast<int>(y));
            }
        }
    }
    if (pos.size() != labels.size()) {
        return; // enumeration mismatch — refuse rather than mis-index
    }

    // Validity: can this pixel copy REAL content under label l? Deep-interior pixels (every
    // candidate source lands back in the hole) are EXCLUDED from the re-cut entirely: their
    // pairwise samples would be the REMOVED content (meaningless to compare), and
    // unlike the per-pixel ICM (which at worst adds pixel noise there), an exact min-cut acting
    // on garbage energy flips whole regions. Their labels stay as the coarse cut placed them;
    // the copy-chain resolution gives them real content afterwards.
    const auto validFor = [&](long x, long y, int l) {
        const long sx = x + offsets[static_cast<std::size_t>(l)].u;
        const long sy = y + offsets[static_cast<std::size_t>(l)].v;
        return sx >= 0 && sy >= 0 && sx < W && sy < H &&
               !pixelHole(holeMask, static_cast<std::uint32_t>(sx),
                          static_cast<std::uint32_t>(sy));
    };

    // 1. Seam edges (right/down 4-neighbour pairs with differing valid labels, at least one
    // endpoint anchored in real content), grouped by the unordered label pair. Hole bbox for
    // band stamping.
    std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> seams; // pair -> seam pixels
    long hx0 = W, hy0 = H, hx1 = -1, hy1 = -1;
    for (std::size_t n = 0; n < pos.size(); ++n) {
        const long x = pos[n].first;
        const long y = pos[n].second;
        hx0 = std::min(hx0, x);
        hy0 = std::min(hy0, y);
        hx1 = std::max(hx1, x);
        hy1 = std::max(hy1, y);
        const int la = labels[n];
        if (la < 0 || la >= K) {
            continue;
        }
        const auto consider = [&](long nx, long ny) {
            const int qn = nodeOf[static_cast<std::size_t>(ny) * static_cast<std::size_t>(W) + nx];
            if (qn < 0) {
                return;
            }
            const int lb = labels[static_cast<std::size_t>(qn)];
            if (lb < 0 || lb >= K || lb == la) {
                return;
            }
            if (!validFor(x, y, la) && !validFor(nx, ny, lb)) {
                return; // deep-interior seam: nothing real anchors it
            }
            const std::pair<int, int> key{std::min(la, lb), std::max(la, lb)};
            seams[key].emplace_back(static_cast<int>(x), static_cast<int>(y));
        };
        if (x + 1 < W) {
            consider(x + 1, y);
        }
        if (y + 1 < H) {
            consider(x, y + 1);
        }
    }
    if (seams.empty() || hx1 < hx0) {
        return;
    }

    // Most-contested pairs first (longest seams are the visible ones); deterministic ties.
    std::vector<std::pair<std::pair<int, int>, const std::vector<std::pair<int, int>>*>> order;
    order.reserve(seams.size());
    for (const auto& [key, px] : seams) {
        order.emplace_back(key, &px);
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
        if (a.second->size() != b.second->size()) {
            return a.second->size() > b.second->size();
        }
        return a.first < b.first;
    });

    constexpr std::size_t kMaxPairs = 16;      // the visible seams; the rest are short
    constexpr std::size_t kMinSeamPixels = 32; // below this the ICM polish handles it
    constexpr int kMaxBandNodes = 150000;      // one Dinic stays fast at this size
    const long bw = hx1 - hx0 + 1;
    const long bh = hy1 - hy0 + 1;
    std::vector<std::uint8_t> stamp(static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh));
    std::vector<int> bandIdx(pos.size()); // node -> band index this round, or -1
    const int neigh[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

    std::size_t processed = 0;
    for (const auto& [key, seamPx] : order) {
        if (processed >= kMaxPairs || seamPx->size() < kMinSeamPixels) {
            break; // order is by size, so the first small seam ends the eligible run
        }
        if (cancel != nullptr && cancel->load()) {
            return;
        }
        ++processed;
        const int A = key.first;
        const int B = key.second;

        // 2. Band: A/B-labeled hole pixels within Chebyshev radius r of this pair's seam — the
        // coarse scale, since that is how far the coarse cut can have misplaced it. (A 2×
        // radius was tried on the Skagen photo: identical output, ~3× the cost — wider play
        // does not help once the seam sits on the best feature within reach; a structural
        // content mismatch needs different content, not a longer leash.) Shrinks while the
        // band would blow the node cap.
        int r = std::clamp(coarseScale, 4, 16);
        std::vector<int> band; // node indices
        for (;;) {
            std::fill(stamp.begin(), stamp.end(), 0);
            for (const auto& [sx, sy] : *seamPx) {
                const long x0 = std::max<long>(hx0, sx - r), x1 = std::min<long>(hx1, sx + r);
                const long y0 = std::max<long>(hy0, sy - r), y1 = std::min<long>(hy1, sy + r);
                for (long yy = y0; yy <= y1; ++yy) {
                    std::uint8_t* row = &stamp[static_cast<std::size_t>(yy - hy0) *
                                               static_cast<std::size_t>(bw)];
                    std::fill(row + (x0 - hx0), row + (x1 - hx0) + 1, std::uint8_t{1});
                }
            }
            band.clear();
            for (std::size_t n = 0; n < pos.size(); ++n) {
                const int l = labels[n];
                if (l != A && l != B) {
                    continue;
                }
                if (stamp[static_cast<std::size_t>(pos[n].second - hy0) *
                              static_cast<std::size_t>(bw) +
                          static_cast<std::size_t>(pos[n].first - hx0)] != 0) {
                    band.push_back(static_cast<int>(n));
                }
            }
            if (static_cast<int>(band.size()) <= kMaxBandNodes || r <= 2) {
                break;
            }
            r = std::max(2, r / 2);
        }
        if (band.size() < 2 || static_cast<int>(band.size()) > kMaxBandNodes) {
            continue;
        }
        std::fill(bandIdx.begin(), bandIdx.end(), -1);
        for (std::size_t i = 0; i < band.size(); ++i) {
            bandIdx[static_cast<std::size_t>(band[i])] = static_cast<int>(i);
        }

        // 3. Per-band-pixel costs for the two candidates, at full resolution. sample[i*2+c] is
        // the candidate's source colour; data[i*2+c] folds validity, the known-boundary term
        // (colour + E1) and the pairwise cost against FIXED hole neighbours outside the band.
        const int cand[2] = {A, B};
        std::vector<common::ColorF> sample(band.size() * 2);
        std::vector<double> data(band.size() * 2);
        parallelRanges(static_cast<int>(band.size()), [&](int i0, int i1) {
            for (int i = i0; i < i1; ++i) {
                const std::size_t n = static_cast<std::size_t>(band[static_cast<std::size_t>(i)]);
                const long x = pos[n].first;
                const long y = pos[n].second;
                for (int c = 0; c < 2; ++c) {
                    const int l = cand[c];
                    const long sx = x + offsets[static_cast<std::size_t>(l)].u;
                    const long sy = y + offsets[static_cast<std::size_t>(l)].v;
                    sample[static_cast<std::size_t>(i) * 2 + static_cast<std::size_t>(c)] =
                        image.at(static_cast<std::uint32_t>(clampL(sx, W)),
                                 static_cast<std::uint32_t>(clampL(sy, H)));
                    double cost = 0.0;
                    if (sx < 0 || sy < 0 || sx >= W || sy >= H ||
                        pixelHole(holeMask, static_cast<std::uint32_t>(sx),
                                  static_cast<std::uint32_t>(sy))) {
                        cost = kInvalid;
                    } else if (sp != nullptr) {
                        cost += sp->at(sx, sy); // penalty: in lockstep with the coarse cut
                    }
                    for (const auto& d : neigh) {
                        const long nx = x + d[0];
                        const long ny = y + d[1];
                        if (nx < 0 || ny < 0 || nx >= W || ny >= H) {
                            continue;
                        }
                        const int qn =
                            nodeOf[static_cast<std::size_t>(ny) * static_cast<std::size_t>(W) +
                                   nx];
                        if (qn < 0) {
                            // Known neighbour: boundary colour + E1 anchoring (same as the cut).
                            cost += kBoundaryWeight * colorSSD(image, clampL(sx, W), clampL(sy, H),
                                                               nx, ny);
                            const long gx = nx + offsets[static_cast<std::size_t>(l)].u;
                            const long gy = ny + offsets[static_cast<std::size_t>(l)].v;
                            if (gx >= 0 && gy >= 0 && gx < W && gy < H &&
                                !pixelHole(holeMask, static_cast<std::uint32_t>(gx),
                                           static_cast<std::uint32_t>(gy))) {
                                cost += kSeamGradientWeight * colorSSD(image, gx, gy, nx, ny);
                            }
                        } else if (bandIdx[static_cast<std::size_t>(qn)] < 0) {
                            // Hole neighbour with a FIXED label this round: its pairwise cost
                            // against the candidate rides the unary term.
                            const int lq = labels[static_cast<std::size_t>(qn)];
                            if (lq >= 0 && lq < K && lq != l) {
                                const auto srcAt = [&](long px, long py, int lbl) {
                                    return image.at(
                                        static_cast<std::uint32_t>(clampL(
                                            px + offsets[static_cast<std::size_t>(lbl)].u, W)),
                                        static_cast<std::uint32_t>(clampL(
                                            py + offsets[static_cast<std::size_t>(lbl)].v, H)));
                                };
                                const common::ColorF pl = srcAt(x, y, l);
                                const common::ColorF pq = srcAt(x, y, lq);
                                const common::ColorF ql = srcAt(nx, ny, l);
                                const common::ColorF qq = srcAt(nx, ny, lq);
                                cost += colorSSDc(pl, pq) + colorSSDc(ql, qq) +
                                        kSeamGradientWeight * gradientSSDc(pl, ql, pq, qq);
                            }
                        }
                    }
                    data[static_cast<std::size_t>(i) * 2 + static_cast<std::size_t>(c)] = cost;
                }
            }
        });

        // Band-internal edges, with each endpoint's candidate source colours precomputed. The
        // pairwise form is the cut's own (colours & gradients).
        std::vector<std::pair<int, int>> edges;
        for (std::size_t i = 0; i < band.size(); ++i) {
            const std::size_t n = static_cast<std::size_t>(band[i]);
            const long x = pos[n].first;
            const long y = pos[n].second;
            const auto tryEdge = [&](long nx, long ny) {
                if (nx < 0 || ny < 0 || nx >= W || ny >= H) {
                    return;
                }
                const int qn =
                    nodeOf[static_cast<std::size_t>(ny) * static_cast<std::size_t>(W) + nx];
                if (qn < 0) {
                    return;
                }
                const int bi = bandIdx[static_cast<std::size_t>(qn)];
                if (bi >= 0) {
                    edges.emplace_back(static_cast<int>(i), bi);
                }
            };
            tryEdge(x + 1, y);
            tryEdge(x, y + 1);
        }

        const auto dc = [&](int node, int c) -> double {
            return data[static_cast<std::size_t>(node) * 2 + static_cast<std::size_t>(c)];
        };
        const auto pw = [&](int ei, int ca, int cb) -> double {
            if (ca == cb) {
                return 0.0;
            }
            const std::size_t a =
                static_cast<std::size_t>(edges[static_cast<std::size_t>(ei)].first) * 2;
            const std::size_t b =
                static_cast<std::size_t>(edges[static_cast<std::size_t>(ei)].second) * 2;
            const auto ua = static_cast<std::size_t>(ca);
            const auto ub = static_cast<std::size_t>(cb);
            return colorSSDc(sample[a + ua], sample[a + ub]) +
                   colorSSDc(sample[b + ua], sample[b + ub]) +
                   kSeamGradientWeight * gradientSSDc(sample[a + ua], sample[b + ua],
                                                      sample[a + ub], sample[b + ub]);
        };
        // Binary + submodular (pairwise(A,A) == pairwise(B,B) == 0, costs nonnegative), so ONE
        // min-cut yields the subproblem's exact global optimum — no expansion loop, no energy
        // re-evaluations. Same Kolmogorov–Zabih arithmetic alphaExpansionImpl uses, specialized
        // to f == all-A, alpha == B (e00 = e11 = 0). inSourceSet == stays A.
        MaxFlowGraph g(static_cast<int>(band.size()));
        for (std::size_t i = 0; i < band.size(); ++i) {
            g.addTermWeights(static_cast<int>(i), dc(static_cast<int>(i), 1),
                             dc(static_cast<int>(i), 0));
        }
        for (std::size_t ei = 0; ei < edges.size(); ++ei) {
            const double e01 = pw(static_cast<int>(ei), 0, 1);
            const double e10 = pw(static_cast<int>(ei), 1, 0);
            const int p = edges[ei].first;
            const int q = edges[ei].second;
            if (e10 >= 0.0) {
                g.addTermWeights(p, e10, 0.0);
                g.addTermWeights(q, 0.0, e10);
            }
            const double beta = e01 + e10;
            if (beta > 0.0) {
                g.addEdge(p, q, beta, 0.0);
            }
        }
        g.maxflow();
        std::size_t flipped = 0;
        for (std::size_t i = 0; i < band.size(); ++i) {
            const int nl = cand[g.inSourceSet(static_cast<int>(i)) ? 0 : 1];
            if (labels[static_cast<std::size_t>(band[i])] != nl) {
                ++flipped;
            }
            labels[static_cast<std::size_t>(band[i])] = nl;
        }
        if (std::getenv("MOSAIC_INPAINT_TIMING") != nullptr) {
            std::fprintf(stderr,
                         "  [band-cut] pair=(%d,%d) seam=%zu r=%d band=%zu flipped=%zu\n", A, B,
                         seamPx->size(), r, band.size(), flipped);
        }
    }

    // WORST-SEAM ESCALATION: after the pairwise pass, the seam with the highest residual cost
    // per pixel gets ONE wide-corridor re-solve with a SMALL multi-label set — the labels
    // present in the corridor (capped). This is where a locally-precious candidate
    // that the coarse cut missed can still enter at full resolution: the pairwise bands can
    // only trade the two labels already meeting there, and a structural mismatch (the treeline
    // junction) often needs a THIRD, transitional sheet routed in from further away. Same
    // machinery as above (banded α-expansion); the result is adopted only if it
    // lowers the subproblem energy of the current labeling.
    if (cancel != nullptr && cancel->load()) {
        return;
    }
    do {
        // Full-res per-edge seam cost under the CURRENT labels.
        const auto srcAt = [&](long x, long y, int l) {
            return image.at(static_cast<std::uint32_t>(
                                clampL(x + offsets[static_cast<std::size_t>(l)].u, W)),
                            static_cast<std::uint32_t>(
                                clampL(y + offsets[static_cast<std::size_t>(l)].v, H)));
        };
        const auto edgeCost = [&](long x, long y, long nx, long ny, int la, int lb) -> double {
            const common::ColorF pl = srcAt(x, y, la);
            const common::ColorF pq = srcAt(x, y, lb);
            const common::ColorF ql = srcAt(nx, ny, la);
            const common::ColorF qq = srcAt(nx, ny, lb);
            return colorSSDc(pl, pq) + colorSSDc(ql, qq) +
                   kSeamGradientWeight * gradientSSDc(pl, ql, pq, qq);
        };
        struct PairStat {
            std::vector<std::pair<int, int>> px;
            double cost = 0.0;
        };
        std::map<std::pair<int, int>, PairStat> stats;
        for (std::size_t n = 0; n < pos.size(); ++n) {
            const long x = pos[n].first;
            const long y = pos[n].second;
            const int la = labels[n];
            if (la < 0 || la >= K) {
                continue;
            }
            const auto consider = [&](long nx, long ny) {
                const int qn =
                    nodeOf[static_cast<std::size_t>(ny) * static_cast<std::size_t>(W) + nx];
                if (qn < 0) {
                    return;
                }
                const int lb = labels[static_cast<std::size_t>(qn)];
                if (lb < 0 || lb >= K || lb == la) {
                    return;
                }
                if (!validFor(x, y, la) && !validFor(nx, ny, lb)) {
                    return;
                }
                PairStat& st = stats[{std::min(la, lb), std::max(la, lb)}];
                st.px.emplace_back(static_cast<int>(x), static_cast<int>(y));
                st.cost += edgeCost(x, y, nx, ny, la, lb);
            };
            if (x + 1 < W) {
                consider(x + 1, y);
            }
            if (y + 1 < H) {
                consider(x, y + 1);
            }
        }
        std::pair<int, int> worst{-1, -1};
        double worstResidual = 0.0;
        for (const auto& [key, st] : stats) {
            if (st.px.size() < kMinSeamPixels) {
                continue;
            }
            const double res = st.cost / static_cast<double>(st.px.size());
            if (res > worstResidual) {
                worstResidual = res;
                worst = key;
            }
        }
        if (worst.first < 0) {
            break;
        }
        const PairStat& st = stats[worst];

        // Corridor: hole pixels within kEscRadius of the worst seam (node-capped by shrinking).
        constexpr int kEscRadius = 64;
        constexpr int kEscLabels = 6;
        int r = kEscRadius;
        std::vector<int> band;
        std::map<int, long> corridorLabels; // label -> pixel count within the corridor
        for (;;) {
            std::fill(stamp.begin(), stamp.end(), 0);
            for (const auto& [sx, sy] : st.px) {
                const long x0 = std::max<long>(hx0, sx - r), x1 = std::min<long>(hx1, sx + r);
                const long y0 = std::max<long>(hy0, sy - r), y1 = std::min<long>(hy1, sy + r);
                for (long yy = y0; yy <= y1; ++yy) {
                    std::uint8_t* row = &stamp[static_cast<std::size_t>(yy - hy0) *
                                               static_cast<std::size_t>(bw)];
                    std::fill(row + (x0 - hx0), row + (x1 - hx0) + 1, std::uint8_t{1});
                }
            }
            corridorLabels.clear();
            long corridorCount = 0;
            for (std::size_t n = 0; n < pos.size(); ++n) {
                if (stamp[static_cast<std::size_t>(pos[n].second - hy0) *
                              static_cast<std::size_t>(bw) +
                          static_cast<std::size_t>(pos[n].first - hx0)] != 0) {
                    ++corridorCount;
                    const int l = labels[n];
                    if (l >= 0 && l < K) {
                        ++corridorLabels[l];
                    }
                }
            }
            if (corridorCount <= kMaxBandNodes || r <= 8) {
                break;
            }
            r = std::max(8, r / 2);
        }
        // Candidate labels: the pair itself + the corridor's most-populous others, capped.
        std::vector<int> cands{worst.first, worst.second};
        {
            std::vector<std::pair<long, int>> ranked; // (count, label), sort desc/asc
            for (const auto& [l, c] : corridorLabels) {
                if (l != worst.first && l != worst.second) {
                    ranked.emplace_back(c, l);
                }
            }
            std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
                if (a.first != b.first) {
                    return a.first > b.first;
                }
                return a.second < b.second;
            });
            for (const auto& [c, l] : ranked) {
                if (static_cast<int>(cands.size()) >= kEscLabels) {
                    break;
                }
                cands.push_back(l);
            }
        }
        const int L = static_cast<int>(cands.size());
        // Band: corridor pixels whose label is in the candidate set (mappable for the energy
        // comparison) with at least one valid candidate (never optimize on removed content).
        band.clear();
        for (std::size_t n = 0; n < pos.size(); ++n) {
            if (stamp[static_cast<std::size_t>(pos[n].second - hy0) *
                          static_cast<std::size_t>(bw) +
                      static_cast<std::size_t>(pos[n].first - hx0)] == 0) {
                continue;
            }
            const int l = labels[n];
            if (std::find(cands.begin(), cands.end(), l) == cands.end()) {
                continue;
            }
            bool anyValid = false;
            for (const int c : cands) {
                if (validFor(pos[n].first, pos[n].second, c)) {
                    anyValid = true;
                    break;
                }
            }
            if (anyValid) {
                band.push_back(static_cast<int>(n));
            }
        }
        if (band.size() < 2 || static_cast<int>(band.size()) > kMaxBandNodes || L < 2) {
            break;
        }
        std::fill(bandIdx.begin(), bandIdx.end(), -1);
        for (std::size_t i = 0; i < band.size(); ++i) {
            bandIdx[static_cast<std::size_t>(band[i])] = static_cast<int>(i);
        }

        // Per-band-pixel costs for the L candidates — the same construction as the pairwise
        // bands (validity + known-boundary colour/E1, fixed-out-of-band neighbours folded in).
        std::vector<common::ColorF> sample(band.size() * static_cast<std::size_t>(L));
        std::vector<double> data(band.size() * static_cast<std::size_t>(L));
        parallelRanges(static_cast<int>(band.size()), [&](int i0, int i1) {
            for (int i = i0; i < i1; ++i) {
                const std::size_t n = static_cast<std::size_t>(band[static_cast<std::size_t>(i)]);
                const long x = pos[n].first;
                const long y = pos[n].second;
                for (int c = 0; c < L; ++c) {
                    const int l = cands[static_cast<std::size_t>(c)];
                    const long sx = x + offsets[static_cast<std::size_t>(l)].u;
                    const long sy = y + offsets[static_cast<std::size_t>(l)].v;
                    sample[static_cast<std::size_t>(i) * static_cast<std::size_t>(L) +
                           static_cast<std::size_t>(c)] =
                        image.at(static_cast<std::uint32_t>(clampL(sx, W)),
                                 static_cast<std::uint32_t>(clampL(sy, H)));
                    double cost = 0.0;
                    if (sx < 0 || sy < 0 || sx >= W || sy >= H ||
                        pixelHole(holeMask, static_cast<std::uint32_t>(sx),
                                  static_cast<std::uint32_t>(sy))) {
                        cost = kInvalid;
                    } else if (sp != nullptr) {
                        cost += sp->at(sx, sy); // penalty: in lockstep with the coarse cut
                    }
                    for (const auto& d : neigh) {
                        const long nx = x + d[0];
                        const long ny = y + d[1];
                        if (nx < 0 || ny < 0 || nx >= W || ny >= H) {
                            continue;
                        }
                        const int qn =
                            nodeOf[static_cast<std::size_t>(ny) * static_cast<std::size_t>(W) +
                                   nx];
                        if (qn < 0) {
                            cost += kBoundaryWeight * colorSSD(image, clampL(sx, W), clampL(sy, H),
                                                               nx, ny);
                            const long gx = nx + offsets[static_cast<std::size_t>(l)].u;
                            const long gy = ny + offsets[static_cast<std::size_t>(l)].v;
                            if (gx >= 0 && gy >= 0 && gx < W && gy < H &&
                                !pixelHole(holeMask, static_cast<std::uint32_t>(gx),
                                           static_cast<std::uint32_t>(gy))) {
                                cost += kSeamGradientWeight * colorSSD(image, gx, gy, nx, ny);
                            }
                        } else if (bandIdx[static_cast<std::size_t>(qn)] < 0) {
                            const int lq = labels[static_cast<std::size_t>(qn)];
                            if (lq >= 0 && lq < K && lq != l) {
                                cost += edgeCost(x, y, nx, ny, l, lq);
                            }
                        }
                    }
                    data[static_cast<std::size_t>(i) * static_cast<std::size_t>(L) +
                         static_cast<std::size_t>(c)] = cost;
                }
            }
        });
        std::vector<std::pair<int, int>> edges;
        for (std::size_t i = 0; i < band.size(); ++i) {
            const std::size_t n = static_cast<std::size_t>(band[i]);
            const long x = pos[n].first;
            const long y = pos[n].second;
            const auto tryEdge = [&](long nx, long ny) {
                if (nx < 0 || ny < 0 || nx >= W || ny >= H) {
                    return;
                }
                const int qn =
                    nodeOf[static_cast<std::size_t>(ny) * static_cast<std::size_t>(W) + nx];
                if (qn >= 0 && bandIdx[static_cast<std::size_t>(qn)] >= 0) {
                    edges.emplace_back(static_cast<int>(i),
                                       bandIdx[static_cast<std::size_t>(qn)]);
                }
            };
            tryEdge(x + 1, y);
            tryEdge(x, y + 1);
        }
        const auto dc = [&](int node, int c) -> double {
            return data[static_cast<std::size_t>(node) * static_cast<std::size_t>(L) +
                        static_cast<std::size_t>(c)];
        };
        const auto pw = [&](int ei, int ca, int cb) -> double {
            if (ca == cb) {
                return 0.0;
            }
            const std::size_t a =
                static_cast<std::size_t>(edges[static_cast<std::size_t>(ei)].first) *
                static_cast<std::size_t>(L);
            const std::size_t b =
                static_cast<std::size_t>(edges[static_cast<std::size_t>(ei)].second) *
                static_cast<std::size_t>(L);
            const auto ua = static_cast<std::size_t>(ca);
            const auto ub = static_cast<std::size_t>(cb);
            return colorSSDc(sample[a + ua], sample[a + ub]) +
                   colorSSDc(sample[b + ua], sample[b + ub]) +
                   kSeamGradientWeight * gradientSSDc(sample[a + ua], sample[b + ua],
                                                      sample[a + ub], sample[b + ub]);
        };
        const auto energyOf = [&](const std::vector<int>& f) -> double {
            double e = 0.0;
            for (std::size_t i = 0; i < band.size(); ++i) {
                e += dc(static_cast<int>(i), f[i]);
            }
            for (std::size_t ei = 0; ei < edges.size(); ++ei) {
                e += pw(static_cast<int>(ei), f[static_cast<std::size_t>(edges[ei].first)],
                        f[static_cast<std::size_t>(edges[ei].second)]);
            }
            return e;
        };
        std::vector<int> current(band.size());
        for (std::size_t i = 0; i < band.size(); ++i) {
            current[i] = static_cast<int>(
                std::find(cands.begin(), cands.end(),
                          labels[static_cast<std::size_t>(band[i])]) -
                cands.begin());
        }
        const std::vector<int> res =
            alphaExpansionImpl(static_cast<int>(band.size()), L, dc, pw, edges,
                               /*maxCycles=*/2, nullptr, cancel);
        const double curE = energyOf(current);
        const double newE = energyOf(res);
        std::size_t flipped = 0;
        if (newE < curE - 1e-9) { // expansion re-inits from argmin-data: adopt only on gain
            for (std::size_t i = 0; i < band.size(); ++i) {
                const int nl = cands[static_cast<std::size_t>(res[i])];
                if (labels[static_cast<std::size_t>(band[i])] != nl) {
                    ++flipped;
                }
                labels[static_cast<std::size_t>(band[i])] = nl;
            }
        }
        if (std::getenv("MOSAIC_INPAINT_TIMING") != nullptr) {
            std::fprintf(stderr,
                         "  [band-esc] pair=(%d,%d) residual=%.4f r=%d band=%zu labels=%d "
                         "flipped=%zu (%s)\n",
                         worst.first, worst.second, worstResidual, r, band.size(), L, flipped,
                         newE < curE - 1e-9 ? "adopted" : "kept");
        }
    } while (false);
}

// Fine seam refinement (He & Sun §4.2): the coarse two-scale cut decides seam placement on a
// downsampled (blurred) image and nearest-upsamples the labels, so seams land on coarse-block
// boundaries and cut straight through fine features (e.g. cloud edges), leaving blocky seams the
// Poisson blend can't hide. Here we refine the upsampled labels at FULL resolution by local energy
// minimization (Gauss-Seidel iterated conditional modes — Besag 1986), letting each hole pixel
// switch to one of its hole-neighbours' offsets when that lowers the SAME energy the graph cut
// minimizes: a validity term (a source that is out of bounds or in the hole costs +inf) + the
// hole↔known boundary coherence (colour + first-order E1 anchoring) + the two-sided He & Sun
// seam-coherence term against hole neighbours with the colours-&-gradients edge-agreement term. A pixel with only one candidate (no differing neighbour) is skipped, so the work
// concentrates on seams; each sweep lets a seam migrate one pixel, so ~l sweeps route it up to ~l/2
// off a feature. Deterministic (fixed scan order). No second graph cut — the energy is identical
// to the graph cut's and ICM is generic coordinate descent.
void refineSeamLabels(const common::ImageF& image, const Selection& holeMask,
                      const std::vector<int>& nodeOf, long W, long H,
                      const std::vector<Offset>& offsets, std::vector<int>& labels, int sweeps,
                      const std::atomic<bool>* cancel,
                      const StructurePenalty* sp = nullptr) {
    const int K = static_cast<int>(offsets.size());
    if (K == 0 || labels.empty() || sweeps <= 0) {
        return;
    }
    const auto valid = [&](long x, long y, int l) {
        if (l < 0 || l >= K) {
            return false;
        }
        const long sx = x + offsets[static_cast<std::size_t>(l)].u;
        const long sy = y + offsets[static_cast<std::size_t>(l)].v;
        return sx >= 0 && sy >= 0 && sx < W && sy < H &&
               !pixelHole(holeMask, static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy));
    };
    const auto src = [&](long x, long y, int l) -> common::ColorF {
        const long sx = clampL(x + offsets[static_cast<std::size_t>(l)].u, W);
        const long sy = clampL(y + offsets[static_cast<std::size_t>(l)].v, H);
        return image.at(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy));
    };
    // Pixel position per node, in the row-major hole-scan order that indexes `labels`/`nodeOf`.
    std::vector<std::pair<int, int>> pos;
    pos.reserve(labels.size());
    for (std::uint32_t y = 0; y < static_cast<std::uint32_t>(H); ++y) {
        for (std::uint32_t x = 0; x < static_cast<std::uint32_t>(W); ++x) {
            if (pixelHole(holeMask, x, y)) {
                pos.emplace_back(static_cast<int>(x), static_cast<int>(y));
            }
        }
    }
    if (pos.size() != labels.size()) {
        return; // node enumeration mismatch — refuse rather than mis-index
    }
    const int neigh[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int sweep = 0; sweep < sweeps; ++sweep) {
        if (cancel != nullptr && cancel->load()) {
            return;
        }
        bool changed = false;
        for (std::size_t node = 0; node < pos.size(); ++node) {
            const long x = pos[node].first;
            const long y = pos[node].second;
            // Candidate offsets: this pixel's own + its hole-neighbours' current labels (deduped).
            int cand[8];
            int nc = 0;
            const auto add = [&](int l) {
                if (l < 0) {
                    return;
                }
                for (int i = 0; i < nc; ++i) {
                    if (cand[i] == l) {
                        return;
                    }
                }
                if (nc < 8) {
                    cand[nc++] = l;
                }
            };
            add(labels[node]);
            for (const auto& d : neigh) {
                const long nx = x + d[0];
                const long ny = y + d[1];
                if (nx < 0 || ny < 0 || nx >= W || ny >= H) {
                    continue;
                }
                const int qn = nodeOf[static_cast<std::size_t>(ny) * static_cast<std::size_t>(W) +
                                      static_cast<std::size_t>(nx)];
                if (qn >= 0 && static_cast<std::size_t>(qn) < labels.size()) {
                    add(labels[static_cast<std::size_t>(qn)]);
                }
            }
            if (nc <= 1) {
                continue; // no differing neighbour: not on a seam, nothing to route
            }
            double bestE = std::numeric_limits<double>::infinity();
            int bestL = labels[node];
            for (int ci = 0; ci < nc; ++ci) {
                const int l = cand[ci];
                double e = valid(x, y, l)
                               ? (sp != nullptr
                                      ? sp->at(x + offsets[static_cast<std::size_t>(l)].u,
                                               y + offsets[static_cast<std::size_t>(l)].v)
                                      : 0.0) // structure penalty: in lockstep with the coarse cut
                               : kInvalid;
                const common::ColorF cpl = src(x, y, l);
                for (const auto& d : neigh) {
                    const long nx = x + d[0];
                    const long ny = y + d[1];
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H) {
                        continue;
                    }
                    const int qn =
                        nodeOf[static_cast<std::size_t>(ny) * static_cast<std::size_t>(W) +
                               static_cast<std::size_t>(nx)];
                    if (qn < 0) {
                        // Known neighbour: hole↔known boundary coherence, colour + the same
                        // first-order (E1) anchoring the graph cut uses — the label's view of
                        // the known neighbour must reproduce it.
                        const common::ColorF known = image.at(static_cast<std::uint32_t>(nx),
                                                              static_cast<std::uint32_t>(ny));
                        e += colorSSDc(cpl, known);
                        const long gx = nx + offsets[static_cast<std::size_t>(l)].u;
                        const long gy = ny + offsets[static_cast<std::size_t>(l)].v;
                        if (gx >= 0 && gy >= 0 && gx < W && gy < H &&
                            !pixelHole(holeMask, static_cast<std::uint32_t>(gx),
                                       static_cast<std::uint32_t>(gy))) {
                            e += kSeamGradientWeight *
                                 colorSSDc(image.at(static_cast<std::uint32_t>(gx),
                                                    static_cast<std::uint32_t>(gy)),
                                           known);
                        }
                    } else if (static_cast<std::size_t>(qn) < labels.size()) {
                        const int lq = labels[static_cast<std::size_t>(qn)];
                        if (lq < 0) {
                            continue;
                        }
                        // Two-sided He & Sun seam term between this pixel and the hole
                        // neighbour, plus the colours-&-gradients edge-agreement term — the
                        // SAME energy the graph cut minimizes (this pass must not undo it).
                        const common::ColorF cql = src(nx, ny, l);
                        const common::ColorF cpq = src(x, y, lq);
                        const common::ColorF cqq = src(nx, ny, lq);
                        e += colorSSDc(cpl, cpq) + colorSSDc(cql, cqq) +
                             kSeamGradientWeight * gradientSSDc(cpl, cql, cpq, cqq);
                    }
                }
                if (e < bestE - 1e-9) {
                    bestE = e;
                    bestL = l;
                }
            }
            if (bestL != labels[node]) {
                labels[node] = bestL;
                changed = true;
            }
        }
        if (!changed) {
            break; // converged
        }
    }
}

// Re-validate every hole pixel's label so its source is at least IN BOUNDS. An out-of-bounds source
// is the one genuinely-broken case: synthesis would otherwise leave the original (removed) pixel in
// place — the stray white/black specks on holes that touch an image border. Where the label's source
// is out of bounds, keep the first offset whose source is in bounds; if none is, set the label to -1
// so synthesis neighbour-fills it and the Poisson blend uses its no-source fallback.
//
// IN-HOLE sources are deliberately LEFT ALONE at the LABEL level. On a large hole the graph cut is
// forced to give deep-interior pixels an offset that lands back in the hole (no offset reaches
// known content that far in); its data term already minimises this, so the labeling is its best
// effort, and reassigning those pixels to the nearest known-reaching offset would collapse a whole
// interior to one offset and tile self-similar texture into a converging-stripe "X". The chains
// those in-hole sources form are resolved AFTER this pass by resolveEffectiveOffsets (each copies
// its chain's known endpoint), so synthesis never copies removed content — this function only
// guarantees no label points OUT OF BOUNDS. Node order is the row-major hole scan.
void correctLabelsForValidity(const Selection& holeMask, long W, long H,
                              const std::vector<Offset>& offsets, std::vector<int>& labels) {
    const auto inBounds = [&](long x, long y) { return x >= 0 && y >= 0 && x < W && y < H; };
    const int K = static_cast<int>(offsets.size());
    std::size_t node = 0;
    for (std::uint32_t y = 0; y < static_cast<std::uint32_t>(H); ++y) {
        for (std::uint32_t x = 0; x < static_cast<std::uint32_t>(W); ++x) {
            if (!pixelHole(holeMask, x, y)) {
                continue;
            }
            if (node >= labels.size()) {
                return;
            }
            int& lab = labels[node];
            ++node;
            const auto valid = [&](int l) {
                return l >= 0 && l < K &&
                       inBounds(static_cast<long>(x) + offsets[static_cast<std::size_t>(l)].u,
                                static_cast<long>(y) + offsets[static_cast<std::size_t>(l)].v);
            };
            if (valid(lab)) {
                continue;
            }
            int found = -1;
            for (int l = 0; l < K; ++l) {
                if (valid(l)) {
                    found = l;
                    break;
                }
            }
            lab = found;
        }
    }
}

// Resolve each hole pixel's COPY CHAIN to known content (the deep-interior fix, 2026-07-02).
// A deep pixel's best label often sources back inside the hole, and synthesis used to copy the
// ORIGINAL (removed) content there — on object removal that resurrects the object as a shifted
// echo (the user's "castle ghost": removing the tower re-built a slimmer tower from its own
// pixels). The chain p -> p+o(p) -> ... is the copy graph the MRF already decided; composing it
// until it exits the hole gives every interior pixel real, KNOWN content while preserving the
// labeling's coherent sheets (neighbours in one sheet compose the same total shift, so no new
// seams appear). A cycle, dead end, or runaway resolves to "no source" — neighbour-fill + the
// Poisson composite fallback, a soft patch instead of an echo of what the user deleted.
// Memoized walk: every node is finalized once, so the pass is O(hole). Deterministic.
//
// ⚠ INVARIANT — this is a purely mechanical closure of He & Sun's own copy graph. The label set
// and its optimization stay untouched (the K dominant offsets): a COMPOSED shift is never a
// candidate in any optimization, i.e. this pass must not be turned into a per-pixel free-shift
// energy minimization. That is a hard constraint, not an oversight. Copying from already-filled
// regions is the mechanism of Criminisi's exemplar inpainting.
//
// Non-static (declared in the header) so the chain/inheritance behaviour is unit-testable with
// hand-crafted label fields; production code reaches it only through graphComplete().
} // namespace

std::vector<Offset> resolveEffectiveOffsets(const Selection& holeMask,
                                            const std::vector<int>& nodeOf, long W, long H,
                                            const std::vector<Offset>& offsets,
                                            const std::vector<int>& labels) {
    std::vector<std::pair<long, long>> pos; // node -> pixel (row-major hole scan, labels' order)
    pos.reserve(labels.size());
    for (std::uint32_t y = 0; y < static_cast<std::uint32_t>(H); ++y)
        for (std::uint32_t x = 0; x < static_cast<std::uint32_t>(W); ++x)
            if (pixelHole(holeMask, x, y))
                pos.emplace_back(static_cast<long>(x), static_cast<long>(y));
    std::vector<Offset> eff(labels.size(), {kNoSource, kNoSource});
    if (pos.size() != labels.size())
        return eff; // enumeration mismatch — refuse rather than mis-index
    const int K = static_cast<int>(offsets.size());
    std::vector<std::uint8_t> state(labels.size(), 0); // 0 new, 1 on current path, 2 final
    std::vector<int> path;
    for (std::size_t n = 0; n < labels.size(); ++n) {
        if (state[n] != 0)
            continue;
        path.clear();
        int cur = static_cast<int>(n);
        bool known = false;
        long tx = 0, ty = 0; // the chain's known endpoint, once found
        while (true) {
            if (state[static_cast<std::size_t>(cur)] == 2) { // memoized tail
                const Offset& e = eff[static_cast<std::size_t>(cur)];
                if (e.u != kNoSource) {
                    known = true;
                    tx = pos[static_cast<std::size_t>(cur)].first + e.u;
                    ty = pos[static_cast<std::size_t>(cur)].second + e.v;
                }
                break;
            }
            if (state[static_cast<std::size_t>(cur)] == 1)
                break; // cycle: the whole path resolves to no-source
            state[static_cast<std::size_t>(cur)] = 1;
            path.push_back(cur);
            if (path.size() > 64)
                break; // runaway guard (offsets have a minimum length; real chains are short)
            const int l = labels[static_cast<std::size_t>(cur)];
            if (l < 0 || l >= K)
                break;
            const long sx = pos[static_cast<std::size_t>(cur)].first +
                            offsets[static_cast<std::size_t>(l)].u;
            const long sy = pos[static_cast<std::size_t>(cur)].second +
                            offsets[static_cast<std::size_t>(l)].v;
            if (sx < 0 || sy < 0 || sx >= W || sy >= H)
                break;
            const int sn = nodeOf[static_cast<std::size_t>(sy) * static_cast<std::size_t>(W) + sx];
            if (sn < 0) { // the chain exits the hole: known content
                known = true;
                tx = sx;
                ty = sy;
                break;
            }
            cur = sn;
        }
        for (const int i : path) {
            state[static_cast<std::size_t>(i)] = 2;
            eff[static_cast<std::size_t>(i)] =
                known ? Offset{static_cast<int>(tx - pos[static_cast<std::size_t>(i)].first),
                               static_cast<int>(ty - pos[static_cast<std::size_t>(i)].second)}
                      : Offset{kNoSource, kNoSource};
        }
    }

    // Failed chains (cycles / runaways) inherit a resolved 4-neighbour's effective offset when
    // that offset is valid HERE (endpoint in bounds and outside the hole) — the neighbour's
    // coherent sheet simply extends over the failed pixel, exactly what the copy graph would
    // have produced had the chain not looped. Without this, failure clusters (typically where
    // the hole meets the FRAME edge, where no known ring exists to anchor chains) fall to the
    // neighbour-fill diffusion, which reads as smeared strips along the edge (user report
    // 2026-07-02, "strips in the bottom right"). Sweeps commit after each pass (order-
    // independent within a sweep, fixed neighbour precedence) so the result is deterministic;
    // pixels no sweep can reach stay no-source (diffusion remains the last resort).
    std::vector<std::size_t> unresolved;
    for (std::size_t n = 0; n < eff.size(); ++n)
        if (eff[n].u == kNoSource)
            unresolved.push_back(n);
    if (!unresolved.empty()) {
        // node index at (x, y), or -1 — O(1) via nodeOf (node order == row-major hole scan).
        const auto nodeAt = [&](long x, long y) -> int {
            if (x < 0 || y < 0 || x >= W || y >= H)
                return -1;
            return nodeOf[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) +
                          static_cast<std::size_t>(x)];
        };
        const int neigh[4][2] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}}; // fixed precedence: N,W,E,S
        for (int sweep = 0; sweep < 64 && !unresolved.empty(); ++sweep) {
            std::vector<std::pair<std::size_t, Offset>> adopt;
            std::vector<std::size_t> still;
            for (const std::size_t n : unresolved) {
                const long x = pos[n].first;
                const long y = pos[n].second;
                Offset take{kNoSource, kNoSource};
                for (const auto& d : neigh) {
                    const int qn = nodeAt(x + d[0], y + d[1]);
                    if (qn < 0)
                        continue;
                    const Offset& e = eff[static_cast<std::size_t>(qn)];
                    if (e.u == kNoSource)
                        continue;
                    const long sx = x + e.u;
                    const long sy = y + e.v;
                    if (sx >= 0 && sy >= 0 && sx < W && sy < H &&
                        !pixelHole(holeMask, static_cast<std::uint32_t>(sx),
                                   static_cast<std::uint32_t>(sy))) {
                        take = e;
                        break;
                    }
                }
                if (take.u != kNoSource)
                    adopt.emplace_back(n, take);
                else
                    still.push_back(n);
            }
            if (adopt.empty())
                break; // nothing reachable — the rest stay no-source
            for (const auto& [n, e] : adopt)
                eff[n] = e;
            unresolved.swap(still);
        }
    }
    return eff;
}

common::ImageF graphComplete(const common::ImageF& image, const Selection& holeMask,
                             const std::vector<Offset>& offsets, const Params& p,
                             std::vector<StageTiming>* timings, ProgressReporter* prog) {
    common::ImageF out = image;
    if (image.empty() || offsets.empty()) {
        return out;
    }
    const long W = static_cast<long>(image.width);
    const long H = static_cast<long>(image.height);
    if (prog != nullptr && !prog->report(0.45f, "Solving")) {
        return out; // cancelled before the graph cut
    }
    const std::atomic<bool>* cancel = prog != nullptr ? prog->cancelToken() : nullptr;
    // The graph cut is the "Solving" stage and drives 0.45..0.72 of the bar; the α-expansion ticks
    // this per label so the (multi-second) solve animates instead of snapping at the end.
    const std::function<bool(float)> gcProgress =
        prog != nullptr ? std::function<bool(float)>(
                              [prog](float f) { return prog->report(0.45f + 0.27f * f, "Solving"); })
                        : std::function<bool(float)>{};

    // Outpaint mode: a canvas-expansion ring gets the structure penalty (see
    // buildStructurePenalty above); an interior heal never builds one, keeping its energy —
    // and so its output — byte-identical to the historical path. Weight and the low-energy
    // damping fraction tuned on the Broadway-tower repro; MOSAIC_INPAINT_OUTPAINT_W /
    // MOSAIC_INPAINT_OUTPAINT_DAMP override for sweeps (0 disables each). The weight was 0.25
    // while the shift ladder was dead (the penalty fought duplication alone); with real ladder
    // candidates on the ballot that tax OVER-punished legitimate donors — grass hugging the
    // horizon carries the horizon edge's dilated anisotropy halo, so the strip's below-horizon
    // band went to cheap sky and the hill fell off a cliff at the old frame edge. 0.15 holds
    // the horizon and still admits no duplication (2026-07-11 sweep: 0.10/0.15 both clean).
    double outpaintW = 0.15;
    if (const char* v = std::getenv("MOSAIC_INPAINT_OUTPAINT_W")) {
        outpaintW = std::atof(v);
    }
    double outpaintDamp = 0.05;
    if (const char* v = std::getenv("MOSAIC_INPAINT_OUTPAINT_DAMP")) {
        outpaintDamp = std::atof(v);
    }
    // Deviation term: OFF unless the user enables Params::outpaintDeviationTax — user testing
    // found regressions on real photos (Skagen outpaint slow + sky-only, Broadway favoring sky
    // near the ground), so the lever is opt-in and uninvestigated. When
    // enabled, weight 1.5: swept 0.5/1.0/1.5/2.0 on the Broadway repro (2026-07-11); the
    // response is NON-monotonic — 1.0 leaves verbatim sky blocks on the horizon line, 1.5
    // clears them, 2.0 over-taxes legitimate donors and the blocks return — pinned by sweep,
    // not semantics. The env var overrides either way (the sweep tool).
    double outpaintDev = p.outpaintDeviationTax ? 1.5 : 0.0;
    if (const char* v = std::getenv("MOSAIC_INPAINT_OUTPAINT_DEV")) {
        outpaintDev = std::atof(v);
    }
    StructurePenalty spFull;
    const StructurePenalty* sp = nullptr;
    const bool outpaint = isOutpaintHole(holeMask, image.width, image.height);
    if (outpaintW > 0.0 && outpaint) {
        ScopedStage stage(timings, "outpaint-structure");
        spFull = buildStructurePenalty(image, holeMask, outpaintW, outpaintDamp, outpaintDev);
        sp = &spFull;
        if (const char* prefix = std::getenv("MOSAIC_INPAINT_DEBUG_PPM")) { // debug: the map
            common::Image dump(static_cast<std::uint32_t>(spFull.w),
                               static_cast<std::uint32_t>(spFull.h));
            for (std::size_t i = 0; i < spFull.map.size(); ++i) {
                const auto v = static_cast<std::uint8_t>(
                    std::lround(std::clamp(spFull.map[i], 0.0F, 1.0F) * 255.0F));
                dump.rgba[i * 4] = dump.rgba[i * 4 + 1] = dump.rgba[i * 4 + 2] = v;
                dump.rgba[i * 4 + 3] = 255;
            }
            std::string err;
            common::writePpm(dump, std::string(prefix) + "structure.ppm", &err);
        }
    }

    // Choose the graph-cut scale. A fixed twoScaleFactor>1 forces that factor; otherwise pick the
    // smallest factor whose coarse node (hole-pixel) count fits graphCutMaxNodes, capped at
    // graphCutMaxScale. The α-expansion runs one max-flow per (label × cycle) and its cost grows
    // with the node count, so bounding the coarse node count makes the graph cut ~independent of
    // hole size; the labels are then nearest-upsampled and Poisson-blended (He & Sun §4.2).
    int l = std::max(1, p.twoScaleFactor);
    if (p.twoScaleFactor <= 1) {
        long holeCount = 0;
        for (std::uint32_t y = 0; y < image.height; ++y) {
            for (std::uint32_t x = 0; x < image.width; ++x) {
                if (pixelHole(holeMask, x, y)) {
                    ++holeCount;
                }
            }
        }
        const long cap = std::max(1L, static_cast<long>(p.graphCutMaxNodes));
        const int maxL = std::max(1, p.graphCutMaxScale);
        l = 1;
        while (l < maxL && holeCount > cap * static_cast<long>(l) * static_cast<long>(l)) {
            ++l;
        }
    }

    std::vector<int> nodeOf; // fine, W*H
    std::vector<int> labels; // fine, per fine node (row-major hole scan)
    {
        ScopedStage stage(timings, l > 1 ? "graph-cut(2-scale)" : "graph-cut");
        if (l > 1) {
            // Coarse graph cut for speed; the offsets are rescaled by l for the coarse pass, then
            // the full-resolution offsets[label] are used for synthesis + Poisson (He & Sun §4.2,
            // two-scale).
            const common::ImageF cimg = downsampleImage(image, l);
            const Selection cmask = downsampleMask(holeMask, l, cimg.width, cimg.height, W, H);
            std::vector<Offset> coffs;
            coffs.reserve(offsets.size());
            for (const Offset& o : offsets) {
                coffs.push_back({static_cast<int>(std::lround(static_cast<double>(o.u) / l)),
                                 static_cast<int>(std::lround(static_cast<double>(o.v) / l))});
            }
            std::vector<int> cnodeOf;
            // The coarse pass needs the penalty at ITS scale: max-pooled from the full-res
            // map (see downsamplePenaltyMax — recomputing on cimg lost the fine structure).
            StructurePenalty spCoarse;
            if (sp != nullptr) {
                spCoarse = downsamplePenaltyMax(spFull, l, static_cast<long>(cimg.width),
                                                static_cast<long>(cimg.height));
            }
            const std::vector<int> clabels =
                computeGraphCutLabels(cimg, cmask, coffs, cnodeOf, p.graphCutMaxCycles, cancel,
                                      gcProgress, sp != nullptr ? &spCoarse : nullptr);

            nodeOf.assign(static_cast<std::size_t>(W) * static_cast<std::size_t>(H), -1);
            const long cw = static_cast<long>(cimg.width);
            const long ch = static_cast<long>(cimg.height);
            int fnode = 0;
            for (std::uint32_t y = 0; y < image.height; ++y) {
                for (std::uint32_t x = 0; x < image.width; ++x) {
                    if (!pixelHole(holeMask, x, y)) {
                        continue;
                    }
                    nodeOf[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + x] = fnode++;
                    const long cx = static_cast<long>(x) / l;
                    const long cy = static_cast<long>(y) / l;
                    int label = 0;
                    if (cx < cw && cy < ch) {
                        const int cn =
                            cnodeOf[static_cast<std::size_t>(cy) * static_cast<std::size_t>(cw) +
                                    cx];
                        if (cn >= 0 && static_cast<std::size_t>(cn) < clabels.size()) {
                            label = clabels[static_cast<std::size_t>(cn)];
                        }
                    }
                    labels.push_back(label);
                }
            }
        } else {
            labels = computeGraphCutLabels(image, holeMask, offsets, nodeOf, p.graphCutMaxCycles,
                                           cancel, gcProgress, sp);
        }
    } // graph-cut stage

    // Fine seam refinement — only meaningful after a coarse two-scale cut (l>1), where seams were
    // placed on the blurred coarse image and nearest-upsampled. First the banded BINARY re-cuts
    // move whole seam FRONTS onto the true full-res features (the stair-stepped-horizon fix — a
    // front move is exactly what per-pixel ICM cannot do), then the ICM pass polishes the
    // leftovers (short seams below the pair pass's floor, three-label meeting points). The
    // single-scale path's labeling is already full-resolution.
    if (p.seamRefine && l > 1) {
        ScopedStage stage(timings, "seam-refine");
        if (std::getenv("MOSAIC_INPAINT_NO_BANDCUT") == nullptr) { // ablation gate (debug)
            refineSeamsBandedCuts(image, holeMask, nodeOf, W, H, offsets, labels, l, cancel, sp);
        }
        const int sweeps = p.seamRefineSweeps > 0 ? p.seamRefineSweeps : std::max(4, l);
        refineSeamLabels(image, holeMask, nodeOf, W, H, offsets, labels, sweeps, cancel, sp);
    }
    if (prog != nullptr && !prog->report(0.73f, "Solving")) {
        return out; // cancelled during refinement
    }

    // Re-validate labels so no hole pixel copies OUT-OF-BOUNDS content (the white/black-speck fix on
    // border-touching holes). In-hole sources are left as-is — see correctLabelsForValidity. Mutates
    // `labels`; the composite and the Poisson blend both read the corrected labels, staying
    // consistent.
    correctLabelsForValidity(holeMask, W, H, offsets, labels);

    // Deep-interior resolution: compose each in-hole copy chain to its KNOWN endpoint (see
    // resolveEffectiveOffsets — the "removing the tower rebuilt a slimmer tower" fix). The
    // per-node effective offsets ride the existing synthesis + blend machinery as a synthetic
    // label set (label i -> effective offset i; no-source pixels stay -1 for the neighbour-fill
    // and the Poisson composite fallback).
    std::vector<Offset> effOffsets;
    std::vector<int> effLabels;
    {
        const std::vector<Offset> eff =
            resolveEffectiveOffsets(holeMask, nodeOf, W, H, offsets, labels);
        effOffsets.reserve(eff.size());
        effLabels.reserve(eff.size());
        for (std::size_t i = 0; i < eff.size(); ++i) {
            effLabels.push_back(eff[i].u == kNoSource ? -1 : static_cast<int>(i));
            effOffsets.push_back(eff[i]);
        }
        if (std::getenv("MOSAIC_INPAINT_TIMING") != nullptr) {
            std::size_t noSrc = 0, noSrcNearEdge = 0, node = 0;
            for (std::uint32_t y = 0; y < static_cast<std::uint32_t>(H); ++y)
                for (std::uint32_t x = 0; x < static_cast<std::uint32_t>(W); ++x) {
                    if (!pixelHole(holeMask, x, y))
                        continue;
                    if (node < eff.size() && eff[node].u == kNoSource) {
                        ++noSrc;
                        if (x < 24 || y < 24 || x + 24 >= static_cast<std::uint32_t>(W) ||
                            y + 24 >= static_cast<std::uint32_t>(H))
                            ++noSrcNearEdge;
                    }
                    ++node;
                }
            std::fprintf(stderr, "  [chains] nodes=%zu no-source=%zu near-edge=%zu\n", eff.size(),
                         noSrc, noSrcNearEdge);
        }
        // Diagnostic label/offset-field dump (MOSAIC_INPAINT_DEBUG_PPM=<prefix>): <prefix>lab.ppm
        // colours each hole pixel by its LABEL, <prefix>eff.ppm by its resolved EFFECTIVE offset
        // (white = no source, black = known). Debug-only; no effect on the result.
        if (const char* dbgPath = std::getenv("MOSAIC_INPAINT_DEBUG_PPM")) {
            const auto hashRgb = [](int u, int v, std::uint8_t* px) {
                std::uint32_t hsh = static_cast<std::uint32_t>(u * 73856093) ^
                                    static_cast<std::uint32_t>(v * 19349663);
                hsh ^= hsh >> 13;
                hsh *= 0x85EBCA6Bu;
                hsh ^= hsh >> 16;
                px[0] = static_cast<std::uint8_t>(64 + (hsh & 0x7F));
                px[1] = static_cast<std::uint8_t>(64 + ((hsh >> 8) & 0x7F));
                px[2] = static_cast<std::uint8_t>(64 + ((hsh >> 16) & 0x7F));
            };
            const auto dump = [&](const std::string& path, bool effective) {
                FILE* f = std::fopen(path.c_str(), "wb");
                if (f == nullptr)
                    return;
                std::fprintf(f, "P6\n%ld %ld\n255\n", W, H);
                std::size_t node = 0;
                for (long y = 0; y < H; ++y)
                    for (long x = 0; x < W; ++x) {
                        std::uint8_t px[3] = {0, 0, 0};
                        if (nodeOf[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) +
                                   static_cast<std::size_t>(x)] >= 0) {
                            if (effective) {
                                const Offset& o = eff[node];
                                if (o.u == kNoSource)
                                    px[0] = px[1] = px[2] = 255;
                                else
                                    hashRgb(o.u, o.v, px);
                            } else {
                                const int l = labels[node];
                                if (l < 0)
                                    px[0] = px[1] = px[2] = 255;
                                else
                                    hashRgb(offsets[static_cast<std::size_t>(l)].u,
                                            offsets[static_cast<std::size_t>(l)].v, px);
                            }
                            ++node;
                        }
                        std::fwrite(px, 1, 3, f);
                    }
                std::fclose(f);
            };
            dump(std::string(dbgPath) + "lab.ppm", false);
            dump(std::string(dbgPath) + "eff.ppm", true);
        }
    }

    const common::ImageF composite = applyOffsetLabels(image, holeMask, effOffsets, effLabels);
    // Diagnostic pre-blend composite dump (same MOSAIC_INPAINT_DEBUG_PPM prefix as the label
    // dumps): diffing <prefix>composite.ppm against the final result isolates what the Poisson
    // blend added — the correction field, where guidance artifacts (tone smears) live.
    if (const char* dbgPath = std::getenv("MOSAIC_INPAINT_DEBUG_PPM")) {
        std::string err;
        common::writePpm(common::toImage8(composite), std::string(dbgPath) + "composite.ppm", &err);
    }
    // The composite is the instant rough fill — stream it as the first preview before the (slower)
    // seam blend, so the user sees the result take shape immediately.
    if (prog != nullptr && !prog->report(0.74f, "Blending", &composite)) {
        return composite;
    }
    if (!p.poissonBlend) {
        return composite;
    }
    ScopedStage stage(timings, "poisson-blend");
    // Boundary-crisp guidance rule (see poissonSeamBlend): outpaint-only and OFF unless
    // the user enables Params::outpaintBoundaryCrisp — same regression note as the deviation
    // term above. Interior heals always pass 0 and keep the historical guidance bit-for-bit.
    // MOSAIC_INPAINT_OUTPAINT_BCRISP overrides for sweeps (≤0 disables).
    double boundaryCrisp = (outpaint && p.outpaintBoundaryCrisp) ? 0.08 : 0.0;
    if (const char* v = std::getenv("MOSAIC_INPAINT_OUTPAINT_BCRISP")) {
        boundaryCrisp = outpaint ? std::atof(v) : 0.0;
    }
    return poissonSeamBlend(image, nodeOf, W, H, effOffsets, effLabels, composite,
                            std::max(1, p.poissonIterations), p.pdeEpsilon, p.poissonOmega, prog,
                            0.75f, 0.25f, boundaryCrisp);
}

} // namespace mosaic::core::inpaint
