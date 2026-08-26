#include "render/effect_primitives.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "common/thread_pool.hpp"

// ⚠ PARALLELISM NOTE (S60). Every loop in this file walks INDEPENDENT rows, columns or pixels:
// each iteration writes one output slot and reads only inputs no other iteration writes. There is
// no reduction anywhere, so banding through common::parallelFor is BIT-IDENTICAL for any band
// split and any thread count -- which is the compositor's determinism rule, and what lets the
// layer-effects goldens stand unchanged across this change.
//
// It matters because this file is the whole of the effects lane's arithmetic (SDF, both blurs) and
// it ran ENTIRELY ON ONE THREAD while the rest of the compositor banded through the shared pool.
// On the S60 fixture that was ~69 s of an ~180 s open, spread evenly across eight stages with no
// hotspot -- the even spread WAS the symptom.
//
// The band minimums below are rows/columns, not pixels: a band has to be worth a task, and these
// planes are effect ROIs (thousands of px on a side), not thumbnails.

namespace mosaic::render::fx {

std::vector<float> extractAlpha(const common::ImageF& img) {
    std::vector<float> a(img.pixelCount());
    common::parallelFor(a.size(), std::size_t{1} << 16, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) a[i] = img.rgba[i * 4 + 3];
    });
    return a;
}

namespace {

// Reflect-101 index mirror (edges mirror without repeating the boundary sample).
[[nodiscard]] int reflect(int p, int n) noexcept {
    if (n == 1) return 0;
    while (p < 0 || p >= n) {
        if (p < 0) p = -p;
        if (p >= n) p = 2 * (n - 1) - p;
    }
    return p;
}

// One separable Gaussian pass over `len` samples at `stride` (a row or a column), reading `src`
// and writing `dst`, both indexed base + i*stride. `kernel` is the half-kernel [0..r] with the
// centre at index 0 (symmetric), already normalised so k[0] + 2*sum(k[1..r]) == 1.
void gaussPass(const float* src, float* dst, int len, int stride, const float* kernel, int r) {
    for (int i = 0; i < len; ++i) {
        float acc = src[i * stride] * kernel[0];
        for (int k = 1; k <= r; ++k) {
            acc += (src[reflect(i - k, len) * stride] + src[reflect(i + k, len) * stride]) * kernel[k];
        }
        dst[i * stride] = acc;
    }
}

}  // namespace

void gaussianBlur(std::vector<float>& plane, int w, int h, float sigma) {
    if (sigma <= 0.0f || w <= 0 || h <= 0) return;
    const int r = std::max(1, static_cast<int>(std::ceil(3.0f * sigma)));
    std::vector<float> kernel(r + 1);
    const float inv2s2 = 1.0f / (2.0f * sigma * sigma);
    float sum = 0.0f;
    for (int k = 0; k <= r; ++k) {
        kernel[k] = std::exp(-static_cast<float>(k) * static_cast<float>(k) * inv2s2);
        sum += (k == 0 ? kernel[k] : 2.0f * kernel[k]);
    }
    for (float& k : kernel) k /= sum;

    std::vector<float> tmp(plane.size());
    // Horizontal: each row (stride 1), plane -> tmp. Rows are disjoint in both operands.
    common::parallelFor(static_cast<std::size_t>(h), 16, [&](std::size_t y0, std::size_t y1) {
        for (std::size_t y = y0; y < y1; ++y)
            gaussPass(plane.data() + y * static_cast<std::size_t>(w),
                      tmp.data() + y * static_cast<std::size_t>(w), w, 1, kernel.data(), r);
    });
    // Vertical: each column (stride w), tmp -> plane. Columns are likewise disjoint.
    common::parallelFor(static_cast<std::size_t>(w), 16, [&](std::size_t x0, std::size_t x1) {
        for (std::size_t x = x0; x < x1; ++x)
            gaussPass(tmp.data() + x, plane.data() + x, h, w, kernel.data(), r);
    });
}

namespace {

[[nodiscard]] int clampi(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

// One normalised box pass of radius `r` over `len` samples at `stride`, edges extended (clamp).
// Running sum -> O(len). `src` and `dst` may not alias.
void boxPass(const float* src, float* dst, int len, int stride, int r) {
    if (len == 0) return;
    if (r <= 0) {
        for (int i = 0; i < len; ++i) dst[i * stride] = src[i * stride];
        return;
    }
    const float norm = 1.0f / static_cast<float>(2 * r + 1);
    float acc = 0.0f;
    for (int k = -r; k <= r; ++k) acc += src[clampi(k, 0, len - 1) * stride];
    dst[0] = acc * norm;
    for (int i = 1; i < len; ++i) {
        acc += src[clampi(i + r, 0, len - 1) * stride] - src[clampi(i - r - 1, 0, len - 1) * stride];
        dst[i * stride] = acc * norm;
    }
}

// Box radii for a 3-pass almost-Gaussian of std-dev `sigma` (Kovesi 2010): mix of two odd box
// widths so the passes' combined variance matches the Gaussian's.
[[nodiscard]] std::array<int, 3> boxRadiiForGauss(float sigma) {
    const double s = sigma;
    double wIdeal = std::sqrt(12.0 * s * s / 3.0 + 1.0);
    int wl = static_cast<int>(std::floor(wIdeal));
    if (wl % 2 == 0) --wl;
    const int wu = wl + 2;
    const double mIdeal =
        (12.0 * s * s - 3.0 * wl * wl - 4.0 * 3.0 * wl - 3.0 * 3.0) / (-4.0 * wl - 4.0);
    const int m = static_cast<int>(std::lround(mIdeal));
    std::array<int, 3> radii{};
    for (int i = 0; i < 3; ++i) {
        const int width = (i < m ? wl : wu);
        radii[i] = std::max(0, (width - 1) / 2);
    }
    return radii;
}

}  // namespace

void boxBlurApprox(std::vector<float>& plane, int w, int h, float sigma) {
    if (sigma <= 0.0f || w <= 0 || h <= 0) return;
    const std::array<int, 3> radii = boxRadiiForGauss(sigma);
    std::vector<float> tmp(plane.size());
    for (const int r : radii) {
        if (r <= 0) continue;
        // Horizontal (plane -> tmp) then vertical (tmp -> plane).
        common::parallelFor(static_cast<std::size_t>(h), 16, [&](std::size_t y0, std::size_t y1) {
            for (std::size_t y = y0; y < y1; ++y)
                boxPass(plane.data() + y * static_cast<std::size_t>(w),
                        tmp.data() + y * static_cast<std::size_t>(w), w, 1, r);
        });
        common::parallelFor(static_cast<std::size_t>(w), 16, [&](std::size_t x0, std::size_t x1) {
            for (std::size_t x = x0; x < x1; ++x)
                boxPass(tmp.data() + x, plane.data() + x, h, w, r);
        });
    }
}

namespace {

constexpr float kInf = 1e20f;

// Felzenszwalb-Huttenlocher 1D squared-distance transform of `f` (n samples) -> `d`. `v` (size n)
// and `z` (size n+1) are caller-provided scratch (parabola sites + their break points).
void edt1d(const float* f, float* d, int n, int* v, float* z) {
    int k = 0;
    v[0] = 0;
    z[0] = -kInf;
    z[1] = kInf;
    for (int q = 1; q < n; ++q) {
        float s = ((f[q] + static_cast<float>(q) * q) -
                   (f[v[k]] + static_cast<float>(v[k]) * v[k])) /
                  (2.0f * static_cast<float>(q) - 2.0f * static_cast<float>(v[k]));
        while (s <= z[k]) {
            --k;
            s = ((f[q] + static_cast<float>(q) * q) -
                 (f[v[k]] + static_cast<float>(v[k]) * v[k])) /
                (2.0f * static_cast<float>(q) - 2.0f * static_cast<float>(v[k]));
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = kInf;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < static_cast<float>(q)) ++k;
        const float dq = static_cast<float>(q) - static_cast<float>(v[k]);
        d[q] = dq * dq + f[v[k]];
    }
}

// 2D squared EDT of `grid` (seeded 0 at seed pixels / +inf elsewhere), in place: columns then rows.
void edt2d(std::vector<float>& grid, int w, int h) {
    const int maxdim = std::max(w, h);
    // ⚠ The four scratch buffers are PER BAND, not shared: edt1d writes all of them, so hoisting
    // them out (as the serial version could) would have every worker stomping one parabola-site
    // list. They are allocated inside the band body for that reason, and the allocation is
    // amortised over `maxdim` samples per line times however many lines the band owns.
    common::parallelFor(static_cast<std::size_t>(w), 16, [&](std::size_t x0, std::size_t x1) {
        std::vector<float> f(maxdim), d(maxdim), z(maxdim + 1);
        std::vector<int> v(maxdim);
        for (std::size_t x = x0; x < x1; ++x) {  // columns (vary y)
            for (int y = 0; y < h; ++y) f[y] = grid[static_cast<std::size_t>(y) * w + x];
            edt1d(f.data(), d.data(), h, v.data(), z.data());
            for (int y = 0; y < h; ++y) grid[static_cast<std::size_t>(y) * w + x] = d[y];
        }
    });
    common::parallelFor(static_cast<std::size_t>(h), 16, [&](std::size_t y0, std::size_t y1) {
        std::vector<float> f(maxdim), d(maxdim), z(maxdim + 1);
        std::vector<int> v(maxdim);
        for (std::size_t y = y0; y < y1; ++y) {  // rows (vary x)
            const std::size_t row = y * static_cast<std::size_t>(w);
            for (int x = 0; x < w; ++x) f[x] = grid[row + x];
            edt1d(f.data(), d.data(), w, v.data(), z.data());
            for (int x = 0; x < w; ++x) grid[row + x] = d[x];
        }
    });
}

}  // namespace

std::vector<float> signedDistanceField(const std::vector<float>& alpha, int w, int h) {
    const std::size_t n = static_cast<std::size_t>(w) * h;
    std::vector<float> distToOutside(n), distToInside(n);
    common::parallelFor(n, std::size_t{1} << 16, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) {
            const bool inside = alpha[i] >= 0.5f;
            distToOutside[i] = inside ? kInf : 0.0f;  // seeds = outside pixels
            distToInside[i] = inside ? 0.0f : kInf;   // seeds = inside pixels
        }
    });
    edt2d(distToOutside, w, h);
    edt2d(distToInside, w, h);
    std::vector<float> sd(n);
    common::parallelFor(n, std::size_t{1} << 16, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) {
            const bool inside = alpha[i] >= 0.5f;
            sd[i] = inside ? -std::sqrt(distToOutside[i]) : std::sqrt(distToInside[i]);
        }
    });
    return sd;
}

std::vector<float> signedDistanceFieldAA(const std::vector<float>& alpha, int w, int h, int ss) {
    if (ss <= 1 || w <= 0 || h <= 0) return signedDistanceField(alpha, w, h);
    const int W = w * ss, H = h * ss;
    std::vector<float> up(static_cast<std::size_t>(W) * H);
    common::parallelFor(static_cast<std::size_t>(H), 16, [&](std::size_t Y0, std::size_t Y1) {
    for (int Y = static_cast<int>(Y0); Y < static_cast<int>(Y1); ++Y) {
        const float fy = (static_cast<float>(Y) + 0.5f) / static_cast<float>(ss) - 0.5f;
        const int iy = static_cast<int>(std::floor(fy));
        const float ty = fy - static_cast<float>(iy);
        const int y0 = std::clamp(iy, 0, h - 1), y1 = std::clamp(iy + 1, 0, h - 1);
        for (int X = 0; X < W; ++X) {
            const float fx = (static_cast<float>(X) + 0.5f) / static_cast<float>(ss) - 0.5f;
            const int ix = static_cast<int>(std::floor(fx));
            const float tx = fx - static_cast<float>(ix);
            const int x0 = std::clamp(ix, 0, w - 1), x1 = std::clamp(ix + 1, 0, w - 1);
            const float a00 = alpha[static_cast<std::size_t>(y0) * w + x0];
            const float a10 = alpha[static_cast<std::size_t>(y0) * w + x1];
            const float a01 = alpha[static_cast<std::size_t>(y1) * w + x0];
            const float a11 = alpha[static_cast<std::size_t>(y1) * w + x1];
            up[static_cast<std::size_t>(Y) * W + X] =
                (a00 * (1.0f - tx) + a10 * tx) * (1.0f - ty) + (a01 * (1.0f - tx) + a11 * tx) * ty;
        }
    }
    });
    const std::vector<float> sdUp = signedDistanceField(up, W, H);  // distances in ss-subpixels
    std::vector<float> sd(static_cast<std::size_t>(w) * h);
    const float norm = 1.0f / static_cast<float>(ss * ss * ss);  // block average, ss-subpixels -> px
    common::parallelFor(static_cast<std::size_t>(h), 16, [&](std::size_t y0, std::size_t y1) {
        for (int y = static_cast<int>(y0); y < static_cast<int>(y1); ++y) {
            for (int x = 0; x < w; ++x) {
                float acc = 0.0f;
                for (int sy = 0; sy < ss; ++sy)
                    for (int sx = 0; sx < ss; ++sx)
                        acc += sdUp[static_cast<std::size_t>(y * ss + sy) * W + (x * ss + sx)];
                sd[static_cast<std::size_t>(y) * w + x] = acc * norm;
            }
        }
    });
    return sd;
}

}  // namespace mosaic::render::fx
