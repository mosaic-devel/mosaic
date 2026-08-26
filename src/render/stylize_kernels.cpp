#include "render/stylize_kernels.hpp"
#include "common/profiler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "common/thread_pool.hpp"

// The S35 stylize kernels (docs/filters-stylize.md §3). Every kernel obeys the contract stated in
// stylize_kernels.hpp; the shared machinery below (premultiplied planes, the clamp-to-edge
// separable box mean and Gaussian, the bilinear plane sampler, the sRGB LUT pair and the noise
// hash) is deliberately local to this translation unit -- these are small, and copying the two
// idioms the compositor already uses (localBoxMean's running sum, the PhotometricMatch LUT pair)
// keeps the module free-standing, which is what lets it be dropped into the compositor behind a
// single branch.
namespace mosaic::render::fx {
namespace {

using common::parallelFor;

constexpr double kPi = 3.14159265358979323846;

// The classic luma weights (core/blend_math.hpp's detail::lum, restated here so the kernels do
// not reach across module headers for three constants).
constexpr float kLumR = 0.30f;
constexpr float kLumG = 0.59f;
constexpr float kLumB = 0.11f;

// Below this coverage a pixel has no recoverable straight colour, so un-premultiplying is
// skipped and the incoming RGB is left alone (see writeBack).
constexpr float kMinAlpha = 1e-6f;

// ---- premultiplied working planes -------------------------------------------------------------

// c[0..2] = premultiplied R/G/B, c[3] = straight alpha. Planar rather than interleaved because
// every kernel here is separable or windowed per channel, and a plane is what the box-mean and
// Gaussian passes want.
struct Planes {
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    std::array<std::vector<float>, 4> c;
};

[[nodiscard]] Planes premultiply(const common::ImageF& img) {
    MOSAIC_PERF_SCOPE("FX planes (premultiply)", mosaic::common::Lane::Cpu);
    Planes pl;
    pl.w = img.width;
    pl.h = img.height;
    const std::size_t n = img.pixelCount();
    for (std::vector<float>& p : pl.c) p.resize(n);
    parallelFor(n, std::size_t{1} << 16, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) {
            const std::size_t p = i * 4;
            const float a = img.rgba[p + 3];
            pl.c[0][i] = img.rgba[p] * a;
            pl.c[1][i] = img.rgba[p + 1] * a;
            pl.c[2][i] = img.rgba[p + 2] * a;
            pl.c[3][i] = a;
        }
    });
    return pl;
}

// Straight-alpha write-back. A pixel that ends with (near) zero coverage KEEPS the RGB it came in
// with: un-premultiplying by zero has no answer, and leaving it alone keeps fully transparent
// pixels byte-identical to the backdrop instead of flushing their colour to black. RGB is floored
// at 0 -- a sharpening undershoot can go negative and a negative premultiplied channel is not a
// colour -- but deliberately NOT capped at 1: the working buffer carries HDR headroom (the S55 sky
// cache feeds the compositor unquantised floats) and the final 8-bit conversion clamps anyway.
void writeBack(common::ImageF& img, const Planes& pl) {
    parallelFor(pl.c[3].size(), std::size_t{1} << 16, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) {
            const std::size_t p = i * 4;
            const float a = pl.c[3][i];
            img.rgba[p + 3] = a;
            if (a <= kMinAlpha) continue;
            const float inv = 1.0f / a;
            img.rgba[p] = std::max(0.0f, pl.c[0][i] * inv);
            img.rgba[p + 1] = std::max(0.0f, pl.c[1][i] * inv);
            img.rgba[p + 2] = std::max(0.0f, pl.c[2][i] * inv);
        }
    });
}

// Free a plane's storage outright. The windowed kernels below build several full-resolution
// intermediates, and on a 4k canvas each one is 64 MiB -- dropping them the moment they are dead
// keeps the peak at a handful of planes instead of a dozen.
void release(std::vector<float>& v) {
    std::vector<float>().swap(v);
}

// ---- separable primitives (clamp-to-edge) ------------------------------------------------------

// Separable box MEAN, window half-width `r`, clamp-to-edge (replicate) -- the same running-sum
// idiom the compositor's localBoxMean uses, and for the same reason: O(1) per pixel, and the
// add/remove pair uses the same clamped indices so multiple out-of-range taps collapse onto the
// same edge pixel consistently (docs/blur-filters.md §5).
[[nodiscard]] std::vector<float> boxMean(const std::vector<float>& src, std::uint32_t W,
                                         std::uint32_t H, int r) {
    const std::size_t n = static_cast<std::size_t>(W) * H;
    const float inv = 1.0f / static_cast<float>(2 * r + 1);
    const int maxX = static_cast<int>(W) - 1;
    const int maxY = static_cast<int>(H) - 1;
    std::vector<float> tmp(n);
    for (std::uint32_t y = 0; y < H; ++y) {  // horizontal pass
        const std::size_t row = static_cast<std::size_t>(y) * W;
        float sum = 0.0f;
        for (int k = -r; k <= r; ++k)
            sum += src[row + static_cast<std::uint32_t>(std::clamp(k, 0, maxX))];
        for (std::uint32_t x = 0; x < W; ++x) {
            tmp[row + x] = sum * inv;
            const int add = std::clamp(static_cast<int>(x) + r + 1, 0, maxX);
            const int sub = std::clamp(static_cast<int>(x) - r, 0, maxX);
            sum += src[row + static_cast<std::uint32_t>(add)] -
                   src[row + static_cast<std::uint32_t>(sub)];
        }
    }
    std::vector<float> out(n);
    for (std::uint32_t x = 0; x < W; ++x) {  // vertical pass
        float sum = 0.0f;
        for (int k = -r; k <= r; ++k)
            sum += tmp[static_cast<std::size_t>(std::clamp(k, 0, maxY)) * W + x];
        for (std::uint32_t y = 0; y < H; ++y) {
            out[static_cast<std::size_t>(y) * W + x] = sum * inv;
            const int add = std::clamp(static_cast<int>(y) + r + 1, 0, maxY);
            const int sub = std::clamp(static_cast<int>(y) - r, 0, maxY);
            sum += tmp[static_cast<std::size_t>(add) * W + x] -
                   tmp[static_cast<std::size_t>(sub) * W + x];
        }
    }
    return out;
}

// In-place separable Gaussian of a single plane, std-dev `sigma`, support `half` taps each side,
// CLAMP-TO-EDGE. (effect_primitives' gaussianBlur is reflect-101 -- correct for the coverage
// planes layer effects blur, wrong for content here, exactly the divergence blur.hpp documents.)
void gaussianPlane(std::vector<float>& plane, std::uint32_t W, std::uint32_t H, float sigma,
                   int half) {
    MOSAIC_PERF_SCOPE("FX gaussian plane", mosaic::common::Lane::Cpu);
    if (sigma <= 0.0f || half < 1) return;
    std::vector<float> k(static_cast<std::size_t>(half) + 1);
    const float denom = 2.0f * sigma * sigma;
    float norm = 0.0f;
    for (int i = 0; i <= half; ++i) {
        k[static_cast<std::size_t>(i)] = std::exp(-static_cast<float>(i * i) / denom);
        norm += (i == 0 ? 1.0f : 2.0f) * k[static_cast<std::size_t>(i)];
    }
    const float invNorm = 1.0f / norm;
    for (float& v : k) v *= invNorm;

    const int maxX = static_cast<int>(W) - 1;
    const int maxY = static_cast<int>(H) - 1;
    const std::size_t n = static_cast<std::size_t>(W) * H;
    std::vector<float> tmp(n);
    parallelFor(H, 64, [&](std::size_t row0, std::size_t row1) {  // horizontal
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            const std::size_t row = static_cast<std::size_t>(y) * W;
            for (std::uint32_t x = 0; x < W; ++x) {
                float acc = plane[row + x] * k[0];
                for (int i = 1; i <= half; ++i) {
                    const int l = std::clamp(static_cast<int>(x) - i, 0, maxX);
                    const int r = std::clamp(static_cast<int>(x) + i, 0, maxX);
                    acc += (plane[row + static_cast<std::uint32_t>(l)] +
                            plane[row + static_cast<std::uint32_t>(r)]) *
                           k[static_cast<std::size_t>(i)];
                }
                tmp[row + x] = acc;
            }
        }
    });
    parallelFor(H, 64, [&](std::size_t row0, std::size_t row1) {  // vertical
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            const std::size_t row = static_cast<std::size_t>(y) * W;
            for (std::uint32_t x = 0; x < W; ++x) {
                float acc = tmp[row + x] * k[0];
                for (int i = 1; i <= half; ++i) {
                    const int u = std::clamp(static_cast<int>(y) - i, 0, maxY);
                    const int d = std::clamp(static_cast<int>(y) + i, 0, maxY);
                    acc += (tmp[static_cast<std::size_t>(u) * W + x] +
                            tmp[static_cast<std::size_t>(d) * W + x]) *
                           k[static_cast<std::size_t>(i)];
                }
                plane[row + x] = acc;
            }
        }
    });
}

// Bilinear plane read in INDEX space (integer coordinates land on pixel centres), clamp-to-edge.
// An integer sample position reproduces the stored value exactly (the far weight is 0), which is
// what makes the analytic kernel tests -- and an unrotated, whole-pixel displacement -- exact.
[[nodiscard]] float samplePlane(const std::vector<float>& p, std::uint32_t W, std::uint32_t H,
                                float x, float y) {
    const int maxX = static_cast<int>(W) - 1;
    const int maxY = static_cast<int>(H) - 1;
    const float cx = std::clamp(x, 0.0f, static_cast<float>(maxX));
    const float cy = std::clamp(y, 0.0f, static_cast<float>(maxY));
    const int x0 = static_cast<int>(std::floor(cx));
    const int y0 = static_cast<int>(std::floor(cy));
    const int x1 = std::min(x0 + 1, maxX);
    const int y1 = std::min(y0 + 1, maxY);
    const float fx = cx - static_cast<float>(x0);
    const float fy = cy - static_cast<float>(y0);
    const std::size_t r0 = static_cast<std::size_t>(y0) * W;
    const std::size_t r1 = static_cast<std::size_t>(y1) * W;
    const float top = p[r0 + static_cast<std::uint32_t>(x0)] * (1.0f - fx) +
                      p[r0 + static_cast<std::uint32_t>(x1)] * fx;
    const float bot = p[r1 + static_cast<std::uint32_t>(x0)] * (1.0f - fx) +
                      p[r1 + static_cast<std::uint32_t>(x1)] * fx;
    return top * (1.0f - fy) + bot * fy;
}

// ---- sRGB <-> linear (the vignette's exposure ramp is photometric) -----------------------------
//
// The same 2048-entry interpolated LUT pair the compositor uses for Exposure / PhotometricMatch,
// restated here so the kernels stay free-standing; out-of-range values (HDR headroom) fall back to
// the analytic IEC 61966-2-1 curves.
constexpr int kSrgbLutSize = 2048;

[[nodiscard]] float srgbDecodeAnalytic(float e) noexcept {
    if (e <= 0.04045f) return std::max(0.0f, e) / 12.92f;
    return std::pow((e + 0.055f) / 1.055f, 2.4f);
}
[[nodiscard]] float srgbEncodeAnalytic(float l) noexcept {
    if (l <= 0.0031308f) return std::max(0.0f, l) * 12.92f;
    return 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}
[[nodiscard]] const std::array<float, kSrgbLutSize + 1>& srgbLut(bool decode) {
    static const auto dec = [] {
        std::array<float, kSrgbLutSize + 1> t{};
        for (int i = 0; i <= kSrgbLutSize; ++i)
            t[static_cast<std::size_t>(i)] =
                srgbDecodeAnalytic(static_cast<float>(i) / kSrgbLutSize);
        return t;
    }();
    static const auto enc = [] {
        std::array<float, kSrgbLutSize + 1> t{};
        for (int i = 0; i <= kSrgbLutSize; ++i)
            t[static_cast<std::size_t>(i)] =
                srgbEncodeAnalytic(static_cast<float>(i) / kSrgbLutSize);
        return t;
    }();
    return decode ? dec : enc;
}
[[nodiscard]] float lutSample(const std::array<float, kSrgbLutSize + 1>& lut, float v) noexcept {
    const float f = v * kSrgbLutSize;
    const int i = static_cast<int>(f);
    if (i < 0) return lut[0];
    if (i >= kSrgbLutSize) return lut[kSrgbLutSize];
    const float t = f - static_cast<float>(i);
    return lut[static_cast<std::size_t>(i)] +
           (lut[static_cast<std::size_t>(i) + 1] - lut[static_cast<std::size_t>(i)]) * t;
}
[[nodiscard]] float toLinear(float e) noexcept {
    return e <= 1.0f ? lutSample(srgbLut(true), e) : srgbDecodeAnalytic(e);
}
[[nodiscard]] float toEncoded(float l) noexcept {
    return l <= 1.0f ? lutSample(srgbLut(false), l) : srgbEncodeAnalytic(l);
}

// ---- the noise hash ---------------------------------------------------------------------------

// A 32-bit integer finalizer (the "lowbias32" bit-mixer, C. Wellons 2018, public domain): three
// xorshift-multiply rounds, near-zero avalanche bias. Used as a PURE FUNCTION of position rather
// than as a stream, which is the whole point -- see addNoiseImage.
[[nodiscard]] std::uint32_t mix32(std::uint32_t x) noexcept {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// hash(seed, parent-space cell, channel). Signed coordinates convert to unsigned modulo 2^32
// (well-defined) so negative parent coordinates -- an off-canvas region buffer, a translated
// group -- hash as cleanly as positive ones.
[[nodiscard]] std::uint32_t noiseHash(std::uint32_t seed, std::int32_t ix, std::int32_t iy,
                                      std::uint32_t ch) noexcept {
    std::uint32_t h = mix32(seed * 0x9e3779b9u);
    h = mix32(h ^ (static_cast<std::uint32_t>(ix) * 0x85ebca6bu));
    h = mix32(h ^ (static_cast<std::uint32_t>(iy) * 0xc2b2ae35u));
    return mix32(h ^ (ch * 0x27d4eb2fu));
}

// The hash as a uniform in (0,1), STRICTLY open at both ends: the top 24 bits (all a float can
// hold exactly) offset by half a step, so the Box-Muller log below can never see 0 or 1 and the
// value is exactly representable -- no rounding can push it to an endpoint.
[[nodiscard]] float unit01(std::uint32_t h) noexcept {
    return (static_cast<float>(h >> 8) + 0.5f) * (1.0f / 16777216.0f);
}

}  // namespace

// ---- Sharpen ----------------------------------------------------------------------------------

void sharpenImage(common::ImageF& img, float amount) {
    if (img.empty() || amount <= 0.0f) return;
    Planes pl = premultiply(img);
    const std::uint32_t W = pl.w;
    const std::uint32_t H = pl.h;
    const int maxX = static_cast<int>(W) - 1;
    const int maxY = static_cast<int>(H) - 1;
    for (int ch = 0; ch < 3; ++ch) {
        // Read from a snapshot: the kernel is a neighbourhood read, so writing in place would
        // feed already-sharpened taps back into the pixels to the right and below.
        const std::vector<float> src = pl.c[static_cast<std::size_t>(ch)];
        std::vector<float>& dst = pl.c[static_cast<std::size_t>(ch)];
        parallelFor(H, 64, [&](std::size_t row0, std::size_t row1) {
            for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
                const std::size_t row = static_cast<std::size_t>(y) * W;
                const std::size_t up =
                    static_cast<std::size_t>(std::max(0, static_cast<int>(y) - 1)) * W;
                const std::size_t dn =
                    static_cast<std::size_t>(std::min(maxY, static_cast<int>(y) + 1)) * W;
                for (std::uint32_t x = 0; x < W; ++x) {
                    const auto xl =
                        static_cast<std::uint32_t>(std::max(0, static_cast<int>(x) - 1));
                    const auto xr =
                        static_cast<std::uint32_t>(std::min(maxX, static_cast<int>(x) + 1));
                    const float mean4 =
                        0.25f * (src[row + xl] + src[row + xr] + src[up + x] + src[dn + x]);
                    const float v = src[row + x];
                    dst[row + x] = v + amount * (v - mean4);
                }
            }
        });
    }
    writeBack(img, pl);
}

// ---- Unsharp mask -----------------------------------------------------------------------------

void unsharpMaskImage(common::ImageF& img, float sigma, float amount, float threshold01,
                      bool draft) {
    if (img.empty() || amount <= 0.0f || sigma <= 0.0f) return;
    Planes pl = premultiply(img);
    // 3 sigma of support is the blur family's convention; a live drag truncates to 2 sigma, which
    // is the only draft lane the S35 family needs (every other kernel here is O(1) per pixel).
    const int half = std::max(1, static_cast<int>(std::ceil((draft ? 2.0f : 3.0f) * sigma)));
    std::array<std::vector<float>, 3> blurred{pl.c[0], pl.c[1], pl.c[2]};
    for (std::vector<float>& b : blurred) gaussianPlane(b, pl.w, pl.h, sigma, half);

    parallelFor(pl.c[3].size(), std::size_t{1} << 16, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) {
            const float d0 = pl.c[0][i] - blurred[0][i];
            const float d1 = pl.c[1][i] - blurred[1][i];
            const float d2 = pl.c[2][i] - blurred[2][i];
            // Threshold gate (the third of the classic three knobs): only detail whose LUMA
            // difference clears the threshold is sharpened, so grain and skin texture stay put
            // while real edges get the boost. A zero threshold sharpens everything.
            if (std::abs(kLumR * d0 + kLumG * d1 + kLumB * d2) < threshold01) continue;
            pl.c[0][i] += amount * d0;
            pl.c[1][i] += amount * d1;
            pl.c[2][i] += amount * d2;
        }
    });
    writeBack(img, pl);
}

// ---- High pass (S34-a) --------------------------------------------------------------------------

void highPassImage(common::ImageF& img, float sigma, bool draft) {
    if (img.empty() || sigma <= 0.0f) return;
    Planes pl = premultiply(img);
    // The same 3-sigma support (2 under a live drag) unsharp uses -- the two kernels ARE the same
    // Gaussian difference, so a High Pass radius has to mean what an Unsharp radius means.
    const int half = std::max(1, static_cast<int>(std::ceil((draft ? 2.0f : 3.0f) * sigma)));
    std::array<std::vector<float>, 3> blurred{pl.c[0], pl.c[1], pl.c[2]};
    for (std::vector<float>& b : blurred) gaussianPlane(b, pl.w, pl.h, sigma, half);

    parallelFor(pl.c[3].size(), std::size_t{1} << 16, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) {
            const float a = pl.c[3][i];
            // No recoverable straight colour under (near) zero coverage: leave the pixel exactly
            // as it came in, the writeBack rule -- a transparent region has no detail to show.
            if (a <= kMinAlpha) continue;
            const float inv = 1.0f / a;
            const std::size_t p = i * 4;
            for (std::size_t k = 0; k < 3; ++k) {
                img.rgba[p + k] = std::clamp(
                    0.5f + (pl.c[k][i] - blurred[k][i]) * inv, 0.0f, 1.0f);
            }
            // Alpha untouched: a high pass rewrites colour, it moves no coverage.
        }
    });
}

// ---- Add noise --------------------------------------------------------------------------------

void addNoiseImage(common::ImageF& img, const common::Affine2D& bufToParent, float sigma,
                   bool uniform, bool monochrome, std::uint32_t seed) {
    if (img.empty() || sigma <= 0.0f) return;
    const std::uint32_t W = img.width;
    // Uniform noise is scaled to the SAME variance as the Gaussian: a uniform on [-s, s] has
    // variance s^2/3, so s = sigma*sqrt(3) makes the Amount slider mean one thing in both modes.
    const float uniformHalf = sigma * 1.7320508f;
    parallelFor(img.height, 64, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            std::size_t idx = static_cast<std::size_t>(y) * W;
            for (std::uint32_t x = 0; x < W; ++x, ++idx) {
                // The grain is keyed to the PARENT-space pixel the sample lands on, never to the
                // buffer index: that is what makes it identical between a full composite and a
                // dirty-rect region composite, and what pins it to the document under pan/zoom.
                const common::Vec2 p =
                    bufToParent.apply({static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5});
                // Clamped before the cast: a double past INT32 range converts as undefined
                // behaviour, and a placement that projects a pixel two billion document px away
                // is degenerate, not artistic.
                const auto ix =
                    static_cast<std::int32_t>(std::clamp(std::floor(p.x), -2.0e9, 2.0e9));
                const auto iy =
                    static_cast<std::int32_t>(std::clamp(std::floor(p.y), -2.0e9, 2.0e9));
                const std::size_t q = idx * 4;
                for (std::uint32_t ch = 0; ch < 3; ++ch) {
                    const std::uint32_t k = monochrome ? 0u : ch;
                    float d = 0.0f;
                    if (uniform) {
                        d = (unit01(noiseHash(seed, ix, iy, k)) - 0.5f) * 2.0f * uniformHalf;
                    } else {
                        // Box-Muller from two independent hashes of the same cell; both are in
                        // (0,1), so the log is always finite.
                        const float u1 = unit01(noiseHash(seed, ix, iy, k));
                        const float u2 = unit01(noiseHash(seed ^ 0x5bf03635u, ix, iy, k));
                        d = sigma * std::sqrt(-2.0f * std::log(u1)) *
                            std::cos(2.0f * static_cast<float>(kPi) * u2);
                    }
                    // Added in the ENCODED working space (film grain is a display-space texture,
                    // and that is where the Amount slider's numbers are legible). Floored at 0;
                    // the top is left to the final 8-bit clamp so HDR headroom survives.
                    img.rgba[q + ch] = std::max(0.0f, img.rgba[q + ch] + d);
                }
                // Alpha untouched: noise recolours the backdrop, it adds no coverage.
            }
        }
    });
}

// ---- Denoise (Lee 1980 local linear MMSE) ------------------------------------------------------

void denoiseImage(common::ImageF& img, int radius, float noiseSigma) {
    if (img.empty() || radius < 1 || noiseSigma <= 0.0f) return;
    Planes pl = premultiply(img);
    const std::size_t n = pl.c[3].size();
    const float noiseVar = noiseSigma * noiseSigma;
    std::vector<float> sq(n);
    for (int ch = 0; ch < 3; ++ch) {
        std::vector<float>& plane = pl.c[static_cast<std::size_t>(ch)];
        parallelFor(n, std::size_t{1} << 16, [&](std::size_t i0, std::size_t i1) {
            for (std::size_t i = i0; i < i1; ++i) sq[i] = plane[i] * plane[i];
        });
        const std::vector<float> m = boxMean(plane, pl.w, pl.h, radius);
        const std::vector<float> m2 = boxMean(sq, pl.w, pl.h, radius);
        parallelFor(n, std::size_t{1} << 16, [&](std::size_t i0, std::size_t i1) {
            for (std::size_t i = i0; i < i1; ++i) {
                // var = E[x^2] - E[x]^2, floored at 0 (the running sums can land a hair below).
                const float var = std::max(0.0f, m2[i] - m[i] * m[i]);
                // k -> 0 where the window is flat (all of it is noise) and -> 1 where it is
                // structured (all of it is signal). This is the whole filter.
                const float k = var > 0.0f ? std::max(0.0f, var - noiseVar) / var : 0.0f;
                plane[i] = m[i] + k * (plane[i] - m[i]);
            }
        });
    }
    writeBack(img, pl);  // alpha rides through untouched
}

// ---- Pixelate ---------------------------------------------------------------------------------

void pixelateImage(common::ImageF& img, const common::Affine2D& bufToParent, double cellParent) {
    if (img.empty() || cellParent <= 0.0) return;
    const std::uint32_t W = img.width;
    const std::uint32_t H = img.height;
    // The cell-index window the buffer can possibly touch: the parent-space bounding box of the
    // buffer rectangle (exact for an affine map -- the extremes are at the corners).
    const common::Rect pb = bufToParent.mapBounds(
        common::Rect{0.0, 0.0, static_cast<double>(W), static_cast<double>(H)});
    const double cx0 = std::floor(pb.x / cellParent);
    const double cy0 = std::floor(pb.y / cellParent);
    const double cx1 = std::floor(pb.right() / cellParent);
    const double cy1 = std::floor(pb.bottom() / cellParent);
    // A near-singular placement can map the buffer onto an astronomically large parent rect, and
    // casting a double past LONG_MAX is undefined -- so the magnitude is checked in DOUBLE first.
    // Nothing artistic lives out here; the honest answer for a degenerate placement is "no-op".
    constexpr double kCellLimit = 1.0e9;
    if (!(std::abs(cx0) <= kCellLimit && std::abs(cy0) <= kCellLimit &&
          std::abs(cx1) <= kCellLimit && std::abs(cy1) <= kCellLimit))
        return;  // the negated form also rejects NaN
    const auto minCx = static_cast<long>(cx0);
    const auto minCy = static_cast<long>(cy0);
    const auto maxCx = static_cast<long>(cx1);
    const auto maxCy = static_cast<long>(cy1);
    const long nx = maxCx - minCx + 1;
    const long ny = maxCy - minCy + 1;
    if (nx <= 0 || ny <= 0) return;
    const auto cells = static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
    // Sanity fence: a cell smaller than a pixel is a no-op the caller already screens for, so a
    // grid bigger than the buffer means the geometry is degenerate (a near-singular placement).
    // Bailing beats allocating an unbounded table.
    if (cells > img.pixelCount() + 1024) return;

    std::vector<float> sum(cells * 4, 0.0f);
    std::vector<std::uint32_t> count(cells, 0u);
    const auto cellOf = [&](std::uint32_t x, std::uint32_t y) -> long {
        const common::Vec2 p =
            bufToParent.apply({static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5});
        const long cx = static_cast<long>(std::floor(p.x / cellParent)) - minCx;
        const long cy = static_cast<long>(std::floor(p.y / cellParent)) - minCy;
        if (cx < 0 || cx >= nx || cy < 0 || cy >= ny) return -1;  // rounding at the bbox rim
        return cy * nx + cx;
    };

    // SERIAL on purpose. A banded parallel reduction would sum a cell's pixels in an order that
    // depends on the band split -- and so on the BUFFER's height -- while a region composite's
    // blocks have to be byte-identical to the full composite's (docs/blur-filters.md §5). Row-major
    // over the buffer visits a cell's pixels in the same relative order in both buffers, so the
    // float sum is the same bits. The pass is one streaming add per pixel.
    for (std::uint32_t y = 0; y < H; ++y) {
        for (std::uint32_t x = 0; x < W; ++x) {
            const long c = cellOf(x, y);
            if (c < 0) continue;
            const std::size_t q = (static_cast<std::size_t>(y) * W + x) * 4;
            const float a = img.rgba[q + 3];
            float* s = &sum[static_cast<std::size_t>(c) * 4];
            s[0] += img.rgba[q] * a;
            s[1] += img.rgba[q + 1] * a;
            s[2] += img.rgba[q + 2] * a;
            s[3] += a;
            ++count[static_cast<std::size_t>(c)];
        }
    }

    parallelFor(H, 64, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            for (std::uint32_t x = 0; x < W; ++x) {
                const long c = cellOf(x, y);
                if (c < 0) continue;
                const std::uint32_t k = count[static_cast<std::size_t>(c)];
                if (k == 0) continue;
                const float inv = 1.0f / static_cast<float>(k);
                const float* s = &sum[static_cast<std::size_t>(c) * 4];
                const std::size_t q = (static_cast<std::size_t>(y) * W + x) * 4;
                const float a = s[3] * inv;
                img.rgba[q + 3] = a;  // a mosaic resamples coverage, exactly like a blur
                if (a <= kMinAlpha) continue;
                const float ia = inv / a;
                img.rgba[q] = std::max(0.0f, s[0] * ia);
                img.rgba[q + 1] = std::max(0.0f, s[1] * ia);
                img.rgba[q + 2] = std::max(0.0f, s[2] * ia);
            }
        }
    });
}

// ---- Emboss -----------------------------------------------------------------------------------

void embossImage(common::ImageF& img, float offX, float offY, float amount) {
    if (img.empty()) return;
    const std::uint32_t W = img.width;
    const std::uint32_t H = img.height;
    const std::size_t n = img.pixelCount();
    // The relief is read off the PREMULTIPLIED luma, so a transparent neighbour contributes 0 and
    // the shape's own alpha edge embosses like any other edge (straight luma there is arbitrary).
    std::vector<float> lum(n);
    parallelFor(n, std::size_t{1} << 16, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) {
            const std::size_t p = i * 4;
            lum[i] = (kLumR * img.rgba[p] + kLumG * img.rgba[p + 1] + kLumB * img.rgba[p + 2]) *
                     img.rgba[p + 3];
        }
    });
    const float hx = 0.5f * offX;
    const float hy = 0.5f * offY;
    parallelFor(H, 64, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            for (std::uint32_t x = 0; x < W; ++x) {
                const auto fx = static_cast<float>(x);
                const auto fy = static_cast<float>(y);
                const float d = samplePlane(lum, W, H, fx + hx, fy + hy) -
                                samplePlane(lum, W, H, fx - hx, fy - hy);
                const float v = std::clamp(0.5f + amount * d, 0.0f, 1.0f);
                const std::size_t q = (static_cast<std::size_t>(y) * W + x) * 4;
                img.rgba[q] = v;
                img.rgba[q + 1] = v;
                img.rgba[q + 2] = v;
                // Alpha untouched: the relief replaces colour, it does not carve coverage.
            }
        }
    });
}

// ---- Oil paint (Kuwahara 1976) ------------------------------------------------------------------

void oilPaintImage(common::ImageF& img, int half) {
    if (img.empty() || half < 1) return;
    Planes pl = premultiply(img);
    const std::uint32_t W = pl.w;
    const std::uint32_t H = pl.h;
    const std::size_t n = pl.c[3].size();
    const int maxX = static_cast<int>(W) - 1;
    const int maxY = static_cast<int>(H) - 1;

    // Quadrant statistics. A box mean of half-width `half` read at the offset (+-half, +-half) IS
    // the mean of the corresponding quadrant of the (4*half+1)^2 window centred on the pixel --
    // which is what makes the classic O(window) Kuwahara O(1) per pixel here.
    {
        std::vector<float> lum(n);
        std::vector<float> lum2(n);
        parallelFor(n, std::size_t{1} << 16, [&](std::size_t i0, std::size_t i1) {
            for (std::size_t i = i0; i < i1; ++i) {
                const float l = kLumR * pl.c[0][i] + kLumG * pl.c[1][i] + kLumB * pl.c[2][i];
                lum[i] = l;
                lum2[i] = l * l;
            }
        });
        const std::vector<float> mL = boxMean(lum, W, H, half);
        release(lum);
        const std::vector<float> mL2 = boxMean(lum2, W, H, half);
        release(lum2);

        // The winning quadrant per pixel, kept as one byte so the four channel passes below can
        // reuse it without holding four more full-resolution mean planes alive at once.
        std::vector<std::uint8_t> pick(n, 0u);
        parallelFor(H, 64, [&](std::size_t row0, std::size_t row1) {
            for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
                for (std::uint32_t x = 0; x < W; ++x) {
                    float best = 0.0f;
                    int bestQ = 0;
                    for (int q = 0; q < 4; ++q) {
                        const int dx = (q & 1) ? half : -half;
                        const int dy = (q & 2) ? half : -half;
                        const auto sx =
                            static_cast<std::uint32_t>(std::clamp(static_cast<int>(x) + dx, 0, maxX));
                        const auto sy =
                            static_cast<std::uint32_t>(std::clamp(static_cast<int>(y) + dy, 0, maxY));
                        const std::size_t j = static_cast<std::size_t>(sy) * W + sx;
                        const float var = mL2[j] - mL[j] * mL[j];
                        if (q == 0 || var < best) {
                            best = var;
                            bestQ = q;
                        }
                    }
                    pick[static_cast<std::size_t>(y) * W + x] = static_cast<std::uint8_t>(bestQ);
                }
            }
        });

        for (std::size_t ch = 0; ch < 4; ++ch) {
            const std::vector<float> m = boxMean(pl.c[ch], W, H, half);
            parallelFor(H, 64, [&](std::size_t row0, std::size_t row1) {
                for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
                    for (std::uint32_t x = 0; x < W; ++x) {
                        const std::size_t i = static_cast<std::size_t>(y) * W + x;
                        const int q = pick[i];
                        const int dx = (q & 1) ? half : -half;
                        const int dy = (q & 2) ? half : -half;
                        const auto sx = static_cast<std::uint32_t>(
                            std::clamp(static_cast<int>(x) + dx, 0, maxX));
                        const auto sy = static_cast<std::uint32_t>(
                            std::clamp(static_cast<int>(y) + dy, 0, maxY));
                        pl.c[ch][i] = m[static_cast<std::size_t>(sy) * W + sx];
                    }
                }
            });
        }
    }
    writeBack(img, pl);  // alpha took its quadrant's mean too: this filter resamples content
}

// ---- Wave / Ripple ------------------------------------------------------------------------------

void waveImage(common::ImageF& img, const common::Affine2D& pre,
               const common::Affine2D& bufToParent, const WaveOp& op) {
    if (img.empty() || op.amplitude == 0.0 || op.wavelength <= 0.0) return;
    const Planes src = premultiply(img);  // the source snapshot; the result goes straight into img
    const std::uint32_t W = img.width;
    const std::uint32_t H = img.height;
    const double k = 2.0 * kPi / op.wavelength;
    // The wave TRAVELS along `dir`, so its phase is read along the perpendicular: at angle 0 that
    // makes each row slide horizontally by a sine of its y -- the shape everyone means by "wave".
    const double nx = -op.dirY;
    const double ny = op.dirX;

    parallelFor(H, 64, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            for (std::uint32_t x = 0; x < W; ++x) {
                const common::Vec2 p =
                    bufToParent.apply({static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5});
                double dx = 0.0;
                double dy = 0.0;
                if (op.ripple) {
                    const double ux = p.x - op.center.x;
                    const double uy = p.y - op.center.y;
                    const double r = std::sqrt(ux * ux + uy * uy);
                    if (r > 1e-9) {  // the exact centre has no radial direction: leave it put
                        const double s = op.amplitude * std::sin(k * r + op.phase);
                        dx = s * ux / r;
                        dy = s * uy / r;
                    }
                } else {
                    const double s =
                        op.amplitude * std::sin(k * (p.x * nx + p.y * ny) + op.phase);
                    dx = s * op.dirX;
                    dy = s * op.dirY;
                }
                // Pull semantics: the output pixel takes the colour that sat `d` away from it.
                const common::Vec2 sb = pre.apply({p.x - dx, p.y - dy});
                // pre maps into CONTINUOUS buffer coordinates (pixel i spans [i, i+1)), while the
                // sampler works in index space (integer = centre), hence the half-pixel shift.
                const auto sx = static_cast<float>(sb.x - 0.5);
                const auto sy = static_cast<float>(sb.y - 0.5);
                const float a = samplePlane(src.c[3], W, H, sx, sy);
                const std::size_t q = (static_cast<std::size_t>(y) * W + x) * 4;
                img.rgba[q + 3] = a;  // a displacement resamples coverage along with colour
                if (a <= kMinAlpha) continue;  // no recoverable colour: keep what was there
                const float inv = 1.0f / a;
                img.rgba[q] = std::max(0.0f, samplePlane(src.c[0], W, H, sx, sy) * inv);
                img.rgba[q + 1] = std::max(0.0f, samplePlane(src.c[1], W, H, sx, sy) * inv);
                img.rgba[q + 2] = std::max(0.0f, samplePlane(src.c[2], W, H, sx, sy) * inv);
            }
        }
    });
}

// ---- Vignette ------------------------------------------------------------------------------------

void vignetteImage(common::ImageF& img, const common::Affine2D& bufToParent,
                   const VignetteOp& op) {
    if (img.empty() || op.exposure == 0.0f || op.radius <= 0.0) return;
    const std::uint32_t W = img.width;
    const double invR = 1.0 / op.radius;
    const double band = op.outer > 1.0 ? 1.0 / (op.outer - 1.0) : 0.0;
    const bool ellipse = op.exponent == 2.0;  // the default: a plain hypot, no pow at all
    const double invN = 1.0 / op.exponent;

    parallelFor(img.height, 64, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            std::size_t idx = static_cast<std::size_t>(y) * W;
            for (std::uint32_t x = 0; x < W; ++x, ++idx) {
                const common::Vec2 p =
                    bufToParent.apply({static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5});
                const double ux = std::abs(p.x - op.center.x) * invR;
                const double uy = std::abs(p.y - op.center.y) * invR;
                const double q = ellipse ? std::sqrt(ux * ux + uy * uy)
                                         : std::pow(std::pow(ux, op.exponent) +
                                                        std::pow(uy, op.exponent), invN);
                if (q <= 1.0) continue;  // the un-vignetted core: byte-identical backdrop
                // smoothstep across the feather band; band == 0 means a hard edge at q == 1.
                double t = band > 0.0 ? std::min(1.0, (q - 1.0) * band) : 1.0;
                t = t * t * (3.0 - 2.0 * t);
                const float gain = std::exp2(op.exposure * static_cast<float>(t));
                const std::size_t pix = idx * 4;
                for (std::size_t ch = 0; ch < 3; ++ch) {
                    // Photographic: the falloff is a light-level scale, so it happens in LINEAR
                    // light and comes back through the encode (the Exposure-kind precedent).
                    img.rgba[pix + ch] = toEncoded(toLinear(img.rgba[pix + ch]) * gain);
                }
                // Alpha untouched: a vignette dims the backdrop, it does not erase it.
            }
        }
    });
}

}  // namespace mosaic::render::fx
