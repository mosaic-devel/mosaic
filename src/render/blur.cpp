#include "render/blur.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "common/thread_pool.hpp"

namespace mosaic::render::fx {

namespace {

using common::Floats;
using common::ImageF;

constexpr double kPi = 3.14159265358979323846;

// Band sizing for the parallel loops below. Separable passes hand whole lines to a band and
// gathers hand whole rows, so no two bands ever share a line; tiny images stay serial because
// spawning threads would cost more than the work they'd carry.
constexpr std::size_t kMinLinesPerBand = 8;
constexpr std::size_t kMinGatherRows = 2;
constexpr std::size_t kMinPixelsPerBand = 4096;

// Split [0, count) into contiguous bands across hardware threads and run fn(begin, end) on
// each (the compositor's parallelFor shape -- literally the same helper since S60-b, bands on
// the shared pool rather than a thread spawned per call). Bands touch disjoint index ranges and
// every band computes exactly what the serial loop would there, so the result is bit-identical
// to serial -- the property the goldens and the region==full equivalence test lean on.
using common::parallelFor;

[[nodiscard]] int clampi(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

// ---- Straight <-> premultiplied -------------------------------------------------------------

// Straight -> premultiplied in place. Every kernel convolves premultiplied values, so the
// (arbitrary) RGB of fully transparent pixels carries zero weight into visible neighbours.
void premultiplyInPlace(ImageF& img) {
    parallelFor(img.pixelCount(), kMinPixelsPerBand, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) {
            float* p = &img.rgba[i * 4];
            p[0] *= p[3];
            p[1] *= p[3];
            p[2] *= p[3];
        }
    });
}

// Premultiplied -> straight in place. A zero-coverage result holds no colour information, so
// its RGB pins to 0 (rather than 0/0) and downstream math stays NaN-free.
void unpremultiplyInPlace(ImageF& img) {
    parallelFor(img.pixelCount(), kMinPixelsPerBand, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) {
            float* p = &img.rgba[i * 4];
            if (p[3] > 0.0f) {
                const float inv = 1.0f / p[3];
                p[0] *= inv;
                p[1] *= inv;
                p[2] *= inv;
            } else {
                p[0] = 0.0f;
                p[1] = 0.0f;
                p[2] = 0.0f;
            }
        }
    });
}

// ---- Separable pass framework ---------------------------------------------------------------

// Run `pass(src, dst, len, stride)` over every row (img -> tmp) then every column
// (tmp -> img); `stride` is in floats (4 along a row, width*4 along a column). Lines are
// independent of each other, so banding rows/columns across threads keeps bit-identity.
template <typename Pass>
void separableRgba(ImageF& img, const Pass& pass) {
    const int w = static_cast<int>(img.width);
    const int h = static_cast<int>(img.height);
    const std::ptrdiff_t rowStride = static_cast<std::ptrdiff_t>(w) * 4;
    std::vector<float> tmp(img.rgba.size());
    parallelFor(static_cast<std::size_t>(h), kMinLinesPerBand,
                [&](std::size_t y0, std::size_t y1) {
                    for (std::size_t y = y0; y < y1; ++y)
                        pass(img.rgba.data() + static_cast<std::ptrdiff_t>(y) * rowStride,
                             tmp.data() + static_cast<std::ptrdiff_t>(y) * rowStride, w,
                             std::ptrdiff_t{4});
                });
    parallelFor(static_cast<std::size_t>(w), kMinLinesPerBand,
                [&](std::size_t x0, std::size_t x1) {
                    for (std::size_t x = x0; x < x1; ++x)
                        pass(tmp.data() + static_cast<std::ptrdiff_t>(x) * 4,
                             img.rgba.data() + static_cast<std::ptrdiff_t>(x) * 4, h, rowStride);
                });
}

// Normalised Gaussian half-kernel [0..r] with r = ceil(3*sigma), the effect_primitives
// builder. The passes here pair it with CLAMP indexing, not reflect-101: these kernels blur
// content, and a composite must replicate its canvas edge instead of mirroring it (the
// divergence blur.hpp documents on purpose).
[[nodiscard]] std::vector<float> gaussianHalfKernel(float sigma) {
    const int r = std::max(1, static_cast<int>(std::ceil(3.0f * sigma)));
    std::vector<float> kernel(static_cast<std::size_t>(r) + 1);
    const float inv2s2 = 1.0f / (2.0f * sigma * sigma);
    float sum = 0.0f;
    for (int k = 0; k <= r; ++k) {
        kernel[k] = std::exp(-static_cast<float>(k) * static_cast<float>(k) * inv2s2);
        sum += (k == 0 ? kernel[k] : 2.0f * kernel[k]);
    }
    for (float& k : kernel) k /= sum;
    return kernel;
}

// One Gaussian pass over a line of `len` RGBA samples spaced `stride` floats apart, edges
// clamped. `kernel` is the normalised half-kernel with the centre weight at index 0.
void gaussPassRgba(const float* src, float* dst, int len, std::ptrdiff_t stride,
                   const float* kernel, int r) {
    // ⚠ BORDER / INTERIOR SPLIT -- the same one bf449d3 gave the stylize planes and 26dc2cd gave
    // the effects Gaussian. This pass never got it, and it is the one the Gaussian Blur ADJUSTMENT
    // runs: clamping is only ever needed within `r` samples of a line's end, but the single loop
    // paid two clampi calls -- four comparisons -- on every tap of every sample, on all four
    // channels' behalf. At sigma 12 that is 73 taps over 39.8 MP per pass.
    //
    // Byte-identical: in the interior both clamps are the identity by construction, so the same
    // taps are summed with the same weights in the same order. The ends keep the clamped form
    // verbatim, in the `edge` lambda below -- which is the original loop body, moved not rewritten.
    const auto edge = [&](int i) {
        const float* c = src + i * stride;
        float acc[4] = {c[0] * kernel[0], c[1] * kernel[0], c[2] * kernel[0], c[3] * kernel[0]};
        for (int k = 1; k <= r; ++k) {
            const float* lo = src + clampi(i - k, 0, len - 1) * stride;
            const float* hi = src + clampi(i + k, 0, len - 1) * stride;
            for (int ch = 0; ch < 4; ++ch)
                acc[ch] += (lo[ch] + hi[ch]) * kernel[k];
        }
        float* o = dst + i * stride;
        for (int ch = 0; ch < 4; ++ch)
            o[ch] = acc[ch];
    };
    // A line shorter than the kernel is all border; `hi >= lo` keeps the interior range empty
    // rather than negative in that case.
    const int lo = std::min(r, len);
    const int hi = std::max(lo, len - r);
    for (int i = 0; i < lo; ++i)
        edge(i);
    for (int i = lo; i < hi; ++i) {
        const float* c = src + i * stride;
        float acc[4] = {c[0] * kernel[0], c[1] * kernel[0], c[2] * kernel[0], c[3] * kernel[0]};
        for (int k = 1; k <= r; ++k) {
            const float* l = c - static_cast<std::ptrdiff_t>(k) * stride;
            const float* h = c + static_cast<std::ptrdiff_t>(k) * stride;
            for (int ch = 0; ch < 4; ++ch)
                acc[ch] += (l[ch] + h[ch]) * kernel[k];
        }
        float* o = dst + i * stride;
        for (int ch = 0; ch < 4; ++ch)
            o[ch] = acc[ch];
    }
    for (int i = hi; i < len; ++i)
        edge(i);
}

// One exact box pass of half-width `r` over a line of RGBA samples, edges clamped: the
// effect_primitives boxPass running-sum shape widened to four channels, O(1) per pixel.
void boxPassRgba(const float* src, float* dst, int len, std::ptrdiff_t stride, int r) {
    const float norm = 1.0f / static_cast<float>(2 * r + 1);
    float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int k = -r; k <= r; ++k) {
        const float* s = src + clampi(k, 0, len - 1) * stride;
        for (int ch = 0; ch < 4; ++ch)
            acc[ch] += s[ch];
    }
    for (int ch = 0; ch < 4; ++ch)
        dst[ch] = acc[ch] * norm;
    for (int i = 1; i < len; ++i) {
        const float* add = src + clampi(i + r, 0, len - 1) * stride;
        const float* sub = src + clampi(i - r - 1, 0, len - 1) * stride;
        float* o = dst + i * stride;
        for (int ch = 0; ch < 4; ++ch) {
            acc[ch] += add[ch] - sub[ch];
            o[ch] = acc[ch] * norm;
        }
    }
}

// ---- Gather machinery -----------------------------------------------------------------------

// Bilinear fetch from a premultiplied RGBA buffer at a fractional pixel position, clamped to
// the edge (replicate). Coordinates are pixel-index based: sampling at an integer position
// returns that pixel exactly, which the axis-aligned kernel tests depend on.
[[nodiscard]] std::array<float, 4> sampleBilinear(const float* rgba, int w, int h, double fx,
                                                  double fy) noexcept {
    const double flx = std::floor(fx);
    const double fly = std::floor(fy);
    const float tx = static_cast<float>(fx - flx);
    const float ty = static_cast<float>(fy - fly);
    const int ix = static_cast<int>(flx);
    const int iy = static_cast<int>(fly);
    const int x0 = clampi(ix, 0, w - 1);
    const int x1 = clampi(ix + 1, 0, w - 1);
    const int y0 = clampi(iy, 0, h - 1);
    const int y1 = clampi(iy + 1, 0, h - 1);
    const float* p00 = rgba + (static_cast<std::size_t>(y0) * w + x0) * 4;
    const float* p10 = rgba + (static_cast<std::size_t>(y0) * w + x1) * 4;
    const float* p01 = rgba + (static_cast<std::size_t>(y1) * w + x0) * 4;
    const float* p11 = rgba + (static_cast<std::size_t>(y1) * w + x1) * 4;
    const float w00 = (1.0f - tx) * (1.0f - ty);
    const float w10 = tx * (1.0f - ty);
    const float w01 = (1.0f - tx) * ty;
    const float w11 = tx * ty;
    std::array<float, 4> out;
    for (int ch = 0; ch < 4; ++ch)
        out[ch] = p00[ch] * w00 + p10[ch] * w10 + p01[ch] * w01 + p11[ch] * w11;
    return out;
}

// Shared driver for the per-pixel gather kernels (motion/spin/zoom): premultiply, snapshot
// the premultiplied pixels as the read-only source, run `gather(src, w, h, x, y, out)` for
// every pixel, then un-premultiply. Each pixel depends only on the snapshot, so row-banding
// stays bit-identical to serial. A gather that returns without writing leaves the pixel to
// ride the premul round trip like a one-tap gather at itself.
template <typename Gather>
void gatherKernel(ImageF& img, const Gather& gather) {
    premultiplyInPlace(img);
    const common::Floats src = img.rgba;
    const int w = static_cast<int>(img.width);
    const int h = static_cast<int>(img.height);
    parallelFor(static_cast<std::size_t>(h), kMinGatherRows,
                [&](std::size_t y0, std::size_t y1) {
                    for (std::size_t y = y0; y < y1; ++y)
                        for (int x = 0; x < w; ++x)
                            gather(src.data(), w, h, x, static_cast<int>(y),
                                   &img.rgba[(y * static_cast<std::size_t>(w) +
                                              static_cast<std::size_t>(x)) *
                                             4]);
                });
    unpremultiplyInPlace(img);
}

// Gather tap budget: one tap per pixel of travel plus one, floored at 3 so short spans still
// average, and capped so a pathological span cannot stall a composite. Draft (live-drag)
// composites cap at 33 instead of 129; both are deterministic, the settled composite always
// re-runs at full quality.
[[nodiscard]] int tapCount(double extentPx, bool draft) noexcept {
    const int cap = draft ? 33 : 129;
    const int n = static_cast<int>(std::ceil(extentPx)) + 1;
    return std::clamp(n, 3, cap);
}

// ---- Surface-blur helpers -------------------------------------------------------------------

// Premultiplied Rec.709 luma plane of an interleaved RGBA buffer. The bilateral's range term
// runs on premultiplied luma so fully transparent pixels read as dark instead of as their
// arbitrary straight RGB.
[[nodiscard]] std::vector<float> premulLumaPlane(const Floats& rgba) {
    std::vector<float> luma(rgba.size() / 4);
    parallelFor(luma.size(), kMinPixelsPerBand, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) {
            const float* p = &rgba[i * 4];
            luma[i] = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        }
    });
    return luma;
}

// One 1D bilateral line: `len` samples, `step` pixels apart, starting at pixel `start`.
// Each output is the spatial taper times a Gaussian on the luma difference to the centre,
// normalised per pixel because the range term varies per pixel (unlike a fixed convolution).
void bilateralPass(const Floats& src, const std::vector<float>& luma, Floats& dst,
                   std::size_t start, std::size_t step, int len,
                   const float* spatial, int r, float rangeCoeff) {
    for (int i = 0; i < len; ++i) {
        const std::size_t centre = start + static_cast<std::size_t>(i) * step;
        const float centreLuma = luma[centre];
        float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float wsum = 0.0f;
        for (int k = -r; k <= r; ++k) {
            const std::size_t px =
                start + static_cast<std::size_t>(clampi(i + k, 0, len - 1)) * step;
            const float d = luma[px] - centreLuma;
            const float wgt = spatial[k < 0 ? -k : k] * std::exp(d * d * rangeCoeff);
            const float* s = &src[px * 4];
            for (int ch = 0; ch < 4; ++ch)
                acc[ch] += s[ch] * wgt;
            wsum += wgt;
        }
        // The centre tap always contributes spatial[0] * exp(0) > 0, so wsum cannot vanish.
        const float inv = 1.0f / wsum;
        float* o = &dst[centre * 4];
        for (int ch = 0; ch < 4; ++ch)
            o[ch] = acc[ch] * inv;
    }
}

// ---- Linear-light helpers (lens blur) -------------------------------------------------------

// IEC 61966-2-1 sRGB decode via a shared 1024-entry table with linear interpolation between
// entries: a per-tap analytic pow() would dominate the aperture gather. The encode side stays
// the exact analytic curve -- it runs once per output pixel, and a second table would stack a
// second interpolation error onto the round trip.
[[nodiscard]] const std::array<float, 1024>& srgbDecodeTable() {
    static const std::array<float, 1024> table = [] {
        std::array<float, 1024> t{};
        for (int i = 0; i < 1024; ++i) {
            const double e = static_cast<double>(i) / 1023.0;
            t[i] = static_cast<float>(e <= 0.04045 ? e / 12.92
                                                   : std::pow((e + 0.055) / 1.055, 2.4));
        }
        return t;
    }();
    return table;
}

[[nodiscard]] float srgbToLinear(float encoded) noexcept {
    const std::array<float, 1024>& table = srgbDecodeTable();
    const float f = std::clamp(encoded, 0.0f, 1.0f) * 1023.0f;
    const int i = std::min(static_cast<int>(f), 1022);
    const float t = f - static_cast<float>(i);
    return table[i] + (table[i + 1] - table[i]) * t;
}

[[nodiscard]] float linearToSrgb(float linear) noexcept {
    const double l = std::clamp(static_cast<double>(linear), 0.0, 1.0);
    const double e = l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
    return static_cast<float>(std::clamp(e, 0.0, 1.0));
}

}  // namespace

// ---- Kernels --------------------------------------------------------------------------------

void gaussianBlurImage(common::ImageF& img, float sigma) {
    if (img.empty() || sigma <= 0.0f)
        return;
    premultiplyInPlace(img);
    const std::vector<float> kernel = gaussianHalfKernel(sigma);
    const int r = static_cast<int>(kernel.size()) - 1;
    separableRgba(img, [&kernel, r](const float* s, float* d, int len, std::ptrdiff_t stride) {
        gaussPassRgba(s, d, len, stride, kernel.data(), r);
    });
    unpremultiplyInPlace(img);
}

void boxBlurImage(common::ImageF& img, int radius) {
    if (img.empty() || radius <= 0)
        return;
    premultiplyInPlace(img);
    separableRgba(img, [radius](const float* s, float* d, int len, std::ptrdiff_t stride) {
        boxPassRgba(s, d, len, stride, radius);
    });
    unpremultiplyInPlace(img);
}

void motionBlurImage(common::ImageF& img, float angleRad, float distancePx, bool draft) {
    if (img.empty() || distancePx <= 0.0f)
        return;
    // The segment spans pixel +- distance/2 along the blur direction with equal tap weights:
    // the uniform line integral of docs/blur-filters.md §3, symmetric about the pixel.
    const int n = tapCount(distancePx, draft);
    const double halfX = std::cos(static_cast<double>(angleRad)) * 0.5 * distancePx;
    const double halfY = std::sin(static_cast<double>(angleRad)) * 0.5 * distancePx;
    const double stepT = 2.0 / (n - 1);
    const float invN = 1.0f / static_cast<float>(n);
    gatherKernel(img, [&](const float* src, int w, int h, int x, int y, float* out) {
        float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (int i = 0; i < n; ++i) {
            const double s = -1.0 + stepT * i;
            const std::array<float, 4> tap =
                sampleBilinear(src, w, h, x + s * halfX, y + s * halfY);
            for (int ch = 0; ch < 4; ++ch)
                acc[ch] += tap[ch];
        }
        for (int ch = 0; ch < 4; ++ch)
            out[ch] = acc[ch] * invN;
    });
}

void spinBlurImage(common::ImageF& img, double cx, double cy, float arcDeg, bool draft) {
    if (img.empty() || arcDeg <= 0.0f)
        return;
    // The header caps the arc at 100 degrees: beyond that the smear reads as a swirl defect
    // and the tap budget for far pixels explodes with no visual payoff.
    const double arcRad = static_cast<double>(std::min(arcDeg, 100.0f)) * (kPi / 180.0);
    gatherKernel(img, [&](const float* src, int w, int h, int x, int y, float* out) {
        const double dx = x - cx;
        const double dy = y - cy;
        const double radius = std::sqrt(dx * dx + dy * dy);
        if (radius == 0.0)
            return;  // a zero-length arc: the exact centre pixel stays as it is
        const double phi = std::atan2(dy, dx);
        const int n = tapCount(radius * arcRad, draft);
        const double t0 = phi - 0.5 * arcRad;
        const double dt = arcRad / (n - 1);
        float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (int i = 0; i < n; ++i) {
            const double t = t0 + dt * i;
            const std::array<float, 4> tap =
                sampleBilinear(src, w, h, cx + radius * std::cos(t), cy + radius * std::sin(t));
            for (int ch = 0; ch < 4; ++ch)
                acc[ch] += tap[ch];
        }
        const float invN = 1.0f / static_cast<float>(n);
        for (int ch = 0; ch < 4; ++ch)
            out[ch] = acc[ch] * invN;
    });
}

void zoomBlurImage(common::ImageF& img, double cx, double cy, float frac, bool draft) {
    if (img.empty() || frac <= 0.0f)
        return;
    // The header declares frac in (0..1]; anything larger would overshoot through the centre
    // and drag content in from the far side, so it saturates at a full pull to the centre.
    const double f = std::min(static_cast<double>(frac), 1.0);
    gatherKernel(img, [&](const float* src, int w, int h, int x, int y, float* out) {
        const double dx = x - cx;
        const double dy = y - cy;
        const double dist = std::sqrt(dx * dx + dy * dy);
        const int n = tapCount(dist * f, draft);
        const double dt = 1.0 / (n - 1);
        float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (int i = 0; i < n; ++i) {
            const double shrink = 1.0 - f * (dt * i);
            const std::array<float, 4> tap =
                sampleBilinear(src, w, h, cx + dx * shrink, cy + dy * shrink);
            for (int ch = 0; ch < 4; ++ch)
                acc[ch] += tap[ch];
        }
        const float invN = 1.0f / static_cast<float>(n);
        for (int ch = 0; ch < 4; ++ch)
            out[ch] = acc[ch] * invN;
    });
}

void surfaceBlurImage(common::ImageF& img, float radius, float threshold01) {
    if (img.empty() || radius <= 0.0f)
        return;
    premultiplyInPlace(img);
    const int w = static_cast<int>(img.width);
    const int h = static_cast<int>(img.height);
    const int r = std::max(1, static_cast<int>(std::ceil(radius)));
    // The spatial taper stays unnormalised: the per-pixel range normalisation absorbs any
    // constant factor anyway.
    const float sigmaS = 0.5f * radius;
    const float spatialCoeff = -1.0f / (2.0f * sigmaS * sigmaS);
    std::vector<float> spatial(static_cast<std::size_t>(r) + 1);
    for (int k = 0; k <= r; ++k)
        spatial[k] = std::exp(static_cast<float>(k) * static_cast<float>(k) * spatialCoeff);
    // The floor keeps a zero threshold from collapsing every off-centre weight to exactly 0
    // (0/0 after normalisation); at 1e-4 the filter simply preserves everything.
    const float sigmaR = std::max(threshold01, 1e-4f);
    const float rangeCoeff = -1.0f / (2.0f * sigmaR * sigmaR);

    Floats tmp(img.rgba.size());
    {
        const std::vector<float> luma = premulLumaPlane(img.rgba);
        parallelFor(static_cast<std::size_t>(h), kMinLinesPerBand,
                    [&](std::size_t y0, std::size_t y1) {
                        for (std::size_t y = y0; y < y1; ++y)
                            bilateralPass(img.rgba, luma, tmp, y * static_cast<std::size_t>(w),
                                          1, w, spatial.data(), r, rangeCoeff);
                    });
    }
    {
        const std::vector<float> luma = premulLumaPlane(tmp);
        parallelFor(static_cast<std::size_t>(w), kMinLinesPerBand,
                    [&](std::size_t x0, std::size_t x1) {
                        for (std::size_t x = x0; x < x1; ++x)
                            bilateralPass(tmp, luma, img.rgba, x, static_cast<std::size_t>(w),
                                          h, spatial.data(), r, rangeCoeff);
                    });
    }
    unpremultiplyInPlace(img);
}

ApertureKernel makeApertureKernel(float radius, int blades, float curvature01, float rotationRad,
                                  bool draft) {
    ApertureKernel kernel;
    if (radius <= 0.0f) {
        // Degenerate aperture: a single unit tap at the origin (the identity gather), which
        // lensBlurImage treats as its no-op signal.
        kernel.radius = 0;
        kernel.stride = 1;
        kernel.offX = {0.0f};
        kernel.offY = {0.0f};
        kernel.weight = {1.0f};
        return kernel;
    }
    const int n = std::clamp(blades, 3, 8);
    const float curve = std::clamp(curvature01, 0.0f, 1.0f);
    const int r = static_cast<int>(std::ceil(radius));
    // Above the threshold the tap lattice coarsens so cost stays ~O(threshold^2) per pixel;
    // renormalising the surviving weights keeps the subsampled kernel mass-conserving. Draft
    // halves the threshold for live drags.
    const int coarseAt = draft ? 6 : 12;
    const int stride = radius <= static_cast<float>(coarseAt)
                           ? 1
                           : static_cast<int>(std::ceil(radius / static_cast<float>(coarseAt)));
    kernel.radius = r;
    kernel.stride = stride;
    const double sector = 2.0 * kPi / n;
    const double cosHalfSector = std::cos(kPi / n);
    double sum = 0.0;
    for (int oy = -r; oy <= r; oy += stride) {
        for (int ox = -r; ox <= r; ox += stride) {
            const double d = std::sqrt(static_cast<double>(ox) * ox +
                                       static_cast<double>(oy) * oy);
            const double theta = std::atan2(static_cast<double>(oy), static_cast<double>(ox));
            // The N-gon boundary from its inradius: fold the angle into one blade sector and
            // measure the flat edge's distance at that angle, then morph toward the disc.
            double local = std::fmod(theta - static_cast<double>(rotationRad), sector);
            if (local < 0.0)
                local += sector;
            const double polygonRadius = radius * cosHalfSector / std::cos(local - 0.5 * sector);
            const double apertureRadius = polygonRadius + (radius - polygonRadius) * curve;
            // A one-pixel anti-aliasing ramp across the aperture edge, so small radii do not
            // alias into squares of whole taps.
            const double wgt = std::clamp(apertureRadius - d + 0.5, 0.0, 1.0);
            if (wgt <= 0.0)
                continue;
            kernel.offX.push_back(static_cast<float>(ox));
            kernel.offY.push_back(static_cast<float>(oy));
            kernel.weight.push_back(static_cast<float>(wgt));
            sum += wgt;
        }
    }
    if (kernel.weight.empty() || sum <= 0.0) {
        // Unreachable for radius > 0 (the near-origin taps always carry weight), but a delta
        // kernel is a safer fallback than a division by zero.
        kernel.offX = {0.0f};
        kernel.offY = {0.0f};
        kernel.weight = {1.0f};
        return kernel;
    }
    const float invSum = static_cast<float>(1.0 / sum);
    for (float& wgt : kernel.weight)
        wgt *= invSum;
    return kernel;
}

void lensBlurImage(common::ImageF& img, const ApertureKernel& k, float boost01,
                   float boostThreshold01) {
    // A degenerate kernel (radius 0 / no taps) is the identity gather; skip the whole linear
    // round trip so the no-op path stays byte-exact.
    if (img.empty() || k.radius <= 0 || k.weight.empty())
        return;
    const int w = static_cast<int>(img.width);
    const int h = static_cast<int>(img.height);
    const std::size_t pixels = img.pixelCount();
    // The baked offsets are integer-valued floats; gathering on ints needs no bilinear.
    const std::size_t taps = k.weight.size();
    std::vector<int> offX(taps);
    std::vector<int> offY(taps);
    for (std::size_t i = 0; i < taps; ++i) {
        offX[i] = static_cast<int>(k.offX[i]);
        offY[i] = static_cast<int>(k.offY[i]);
    }
    // Decode sRGB to linear, premultiply IN LINEAR, and gain-boost highlight pixels before
    // the gather. The boost is never undone afterwards: clipped SDR highlights blooming into
    // aperture shapes is the point of the lever (blur.hpp).
    // ⚠ TWO INVARIANTS: the boost stays a SINGLE lower threshold (never an upper+lower "light
    // range" pair) applied as this separate pre-pass on pixel VALUES; and the gather below uses
    // only uniform aperture weights -- never make a tap's weight depend on its highlight-ness.
    // Both are hard constraints on this function, not an oversight.
    std::vector<float> src(pixels * 4);
    const bool boost = boost01 > 0.0f;
    const float gain = 1.0f + 4.0f * boost01;
    parallelFor(pixels, kMinPixelsPerBand, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) {
            const float* p = &img.rgba[i * 4];
            const float a = p[3];
            float lr = srgbToLinear(p[0]) * a;
            float lg = srgbToLinear(p[1]) * a;
            float lb = srgbToLinear(p[2]) * a;
            if (boost && 0.2126f * lr + 0.7152f * lg + 0.0722f * lb > boostThreshold01) {
                lr *= gain;
                lg *= gain;
                lb *= gain;
            }
            float* s = &src[i * 4];
            s[0] = lr;
            s[1] = lg;
            s[2] = lb;
            s[3] = a;
        }
    });
    parallelFor(static_cast<std::size_t>(h), kMinGatherRows,
                [&](std::size_t y0, std::size_t y1) {
                    for (std::size_t y = y0; y < y1; ++y) {
                        for (int x = 0; x < w; ++x) {
                            float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                            for (std::size_t t = 0; t < taps; ++t) {
                                const int sx = clampi(x + offX[t], 0, w - 1);
                                const int sy = clampi(static_cast<int>(y) + offY[t], 0, h - 1);
                                const float* s =
                                    &src[(static_cast<std::size_t>(sy) * w + sx) * 4];
                                const float wgt = k.weight[t];
                                for (int ch = 0; ch < 4; ++ch)
                                    acc[ch] += s[ch] * wgt;
                            }
                            float* o = &img.rgba[(y * static_cast<std::size_t>(w) +
                                                  static_cast<std::size_t>(x)) *
                                                 4];
                            const float a = acc[3];
                            if (a > 0.0f) {
                                const float inv = 1.0f / a;
                                o[0] = linearToSrgb(acc[0] * inv);
                                o[1] = linearToSrgb(acc[1] * inv);
                                o[2] = linearToSrgb(acc[2] * inv);
                            } else {
                                o[0] = 0.0f;
                                o[1] = 0.0f;
                                o[2] = 0.0f;
                            }
                            o[3] = std::clamp(a, 0.0f, 1.0f);
                        }
                    }
                });
}

void dofBlurImage(common::ImageF& img, const std::vector<float>& radiusPlane, float maxRadius,
                  bool iris, bool draft) {
    if (img.empty() || maxRadius <= 0.0f || radiusPlane.size() != img.pixelCount())
        return;
    // The pre-blurred pyramid at {1/4, 1/2, 3/4, 1} * maxRadius. Level 0 is the untouched
    // input itself, so the focus band can stay byte-identical (the compositor suite pins it).
    // ⚠ TWO INVARIANTS ARE LOAD-BEARING HERE: every level is blurred INDEPENDENTLY FROM THE
    // SOURCE (never level k from level k-1's result -- never a step-dilated cascade), and the
    // render below stays strictly interpolation-between-levels (never "apply a kernel per pixel
    // by a per-pixel radius"). Do not "optimize" either property away: the cost is deliberate.
    std::array<common::ImageF, 4> levels;
    for (int lv = 1; lv <= 4; ++lv) {
        levels[lv - 1] = img;
        const float levelRadius = maxRadius * (static_cast<float>(lv) / 4.0f);
        if (iris) {
            // The fixed v1 iris: 6 blades, mild curvature, no rotation (docs §3). Boost stays
            // off -- DoF bloom is a future lever, not a default.
            lensBlurImage(levels[lv - 1],
                          makeApertureKernel(levelRadius, 6, 0.35f, 0.0f, draft), 0.0f, 0.0f);
        } else {
            gaussianBlurImage(levels[lv - 1], 0.5f * levelRadius);
        }
    }
    parallelFor(img.pixelCount(), kMinPixelsPerBand, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) {
            const float f = std::clamp(radiusPlane[i], 0.0f, maxRadius);
            // f/maxRadius is exactly 1 at saturation and exactly 0 in focus, so both ends hit
            // the integer fast path below and return level bytes untouched.
            const float t = f / maxRadius * 4.0f;
            const int lo = static_cast<int>(t);
            const float fr = t - static_cast<float>(lo);
            float* o = &img.rgba[i * 4];
            if (fr == 0.0f) {
                if (lo == 0)
                    continue;  // the focus band: the original pixel bytes, no round trip
                const float* p = &levels[lo - 1].rgba[i * 4];
                o[0] = p[0];
                o[1] = p[1];
                o[2] = p[2];
                o[3] = p[3];
                continue;
            }
            const float* pa = lo == 0 ? o : &levels[lo - 1].rgba[i * 4];
            const float* pb = &levels[lo].rgba[i * 4];
            // Lerp adjacent levels in premultiplied space: coverage and colour interpolate
            // together, so a soft alpha edge crossing the focus falloff cannot ring.
            const float aa = pa[3];
            const float ab = pb[3];
            const float al = aa + (ab - aa) * fr;
            if (al > 0.0f) {
                const float inv = 1.0f / al;
                for (int ch = 0; ch < 3; ++ch) {
                    const float pma = pa[ch] * aa;
                    const float pmb = pb[ch] * ab;
                    o[ch] = (pma + (pmb - pma) * fr) * inv;
                }
            } else {
                o[0] = 0.0f;
                o[1] = 0.0f;
                o[2] = 0.0f;
            }
            o[3] = al;
        }
    });
}

}  // namespace mosaic::render::fx
