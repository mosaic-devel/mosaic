#include "render/resample.hpp"

#include <algorithm>
#include <optional>

namespace mosaic::render {

// The shared band split (common/thread_pool.hpp). Same spelling as in compositor.cpp, where these
// loops came from: the SPLIT is what the byte-pinned goldens were rendered through, so it must
// stay exactly the one those loops used.
using common::parallelFor;

namespace {

constexpr double kPi = 3.14159265358979323846;

// A transform that maps every point to itself (the common case: a layer drawn 1:1).
[[nodiscard]] bool isIdentity(const common::Affine2D& t) noexcept {
    return t.m00 == 1.0 && t.m01 == 0.0 && t.m02 == 0.0 && t.m10 == 0.0 && t.m11 == 1.0 &&
           t.m12 == 0.0;
}

[[nodiscard]] bool isInteger(double v) noexcept { return v == std::floor(v); }

[[nodiscard]] double sinc(double x) noexcept {
    if (x == 0.0) return 1.0;
    const double px = kPi * x;
    return std::sin(px) / px;
}

[[nodiscard]] double lanczosKernel(double x, double a) noexcept {
    x = std::abs(x);
    return x < a ? sinc(x) * sinc(x / a) : 0.0;
}

}  // namespace

double cubicKernel(double x, double B, double C) noexcept {
    x = std::abs(x);
    const double x2 = x * x, x3 = x2 * x;
    if (x < 1.0)
        return ((12.0 - 9.0 * B - 6.0 * C) * x3 + (-18.0 + 12.0 * B + 6.0 * C) * x2 +
                (6.0 - 2.0 * B)) /
               6.0;
    if (x < 2.0)
        return ((-B - 6.0 * C) * x3 + (6.0 * B + 30.0 * C) * x2 + (-12.0 * B - 48.0 * C) * x +
                (8.0 * B + 24.0 * C)) /
               6.0;
    return 0.0;
}

bool isLosslessGrid(const common::Affine2D& t) noexcept {
    if (!isInteger(t.m02) || !isInteger(t.m12)) return false;
    const bool axisAligned = t.m01 == 0.0 && t.m10 == 0.0 && isInteger(t.m00) && isInteger(t.m11);
    const bool quarterTurn = t.m00 == 0.0 && t.m11 == 0.0 && isInteger(t.m01) && isInteger(t.m10);
    return axisAligned || quarterTurn;
}

ResampleFilter chooseAutoFilter(const common::Affine2D& t, bool liveDrag) noexcept {
    if (isLosslessGrid(t)) return ResampleFilter::Nearest;  // exact; keeps pixel art crisp
    if (liveDrag) return ResampleFilter::Bilinear;          // cheap per-frame preview
    // Committed, non-lossless: a reduction box-averages (no ringing); enlarge / rotate get the
    // sharp high-quality kernel. Scale per axis = the linear part's column lengths.
    const double sx = std::hypot(t.m00, t.m10);
    const double sy = std::hypot(t.m01, t.m11);
    // A reduction in EITHER axis box-averages (Area): a sharp wide kernel's footprint is
    // radius x minification along the shrunk axis, so an anisotropic shrink (e.g. 1/5 height, full
    // width) would give Lanczos3 a ~15px y-footprint -> billions of taps on a big canvas (a
    // multi-second commit). Only a pure enlarge / rotate (both axes >= 1) takes the sharp kernel.
    if (std::min(sx, sy) < 1.0 - 1e-6) return ResampleFilter::Area;
    return ResampleFilter::Lanczos3;
}

double kernelRadius(ResampleFilter f) noexcept {
    switch (f) {
        case ResampleFilter::Area: return 0.5;  // a box one footprint wide
        case ResampleFilter::Bilinear: return 1.0;
        case ResampleFilter::Bicubic:
        case ResampleFilter::Mitchell:
        case ResampleFilter::Lanczos2:
        case ResampleFilter::Gaussian: return 2.0;
        case ResampleFilter::Lanczos3: return 3.0;
        default: return 1.0;  // Auto/Nearest/Supersample unreached
    }
}

double kernelWeight(ResampleFilter f, double t) noexcept {
    switch (f) {
        case ResampleFilter::Area: return std::abs(t) < 0.5 ? 1.0 : 0.0;  // box
        case ResampleFilter::Bilinear: {
            const double a = std::abs(t);
            return a < 1.0 ? 1.0 - a : 0.0;
        }
        case ResampleFilter::Bicubic: return cubicKernel(t, 0.0, 0.5);  // Catmull-Rom
        case ResampleFilter::Mitchell: return cubicKernel(t, 1.0 / 3.0, 1.0 / 3.0);
        case ResampleFilter::Lanczos2: return lanczosKernel(t, 2.0);
        case ResampleFilter::Lanczos3: return lanczosKernel(t, 3.0);
        case ResampleFilter::Gaussian: {
            constexpr double s = 0.5;  // sigma in source texels at 1:1
            return std::exp(-(t * t) / (2.0 * s * s));
        }
        default: {  // Auto/Nearest/Supersample unreached -> behave as Bilinear
            const double a = std::abs(t);
            return a < 1.0 ? 1.0 - a : 0.0;
        }
    }
}

ResampleFilter resolveFilter(ResampleFilter user, const common::Affine2D& t,
                             bool liveDrag) noexcept {
    return user == ResampleFilter::Auto ? chooseAutoFilter(t, liveDrag) : user;
}

// Place `src` (in layer/group-local space) into a `w`x`h` document-space buffer through `t`
// (local -> document), resampling with `filter` (Nearest by default -- the legacy behaviour kept
// for callers like mergeDown / the crop bake; the composite walk passes the resolved kernel).
common::ImageF transformImageF(const common::ImageF& src, const common::Affine2D& t,
                               std::uint32_t w, std::uint32_t h, ResampleFilter filter,
                               EdgeMode edge) {
    common::ImageF dst(w, h);
    if (src.empty()) return dst;
    // Auto is resolved here so image-level callers need not pre-resolve; every compositor call
    // site already hands over a concrete kernel, for which this is the identity.
    filter = resolveFilter(filter, t, /*liveDrag=*/false);
    if (isIdentity(t) && src.width == w && src.height == h) {
        dst.rgba = src.rgba;  // exact 1:1 fast path
        return dst;
    }
    const bool linearIdentity = t.m00 == 1.0 && t.m01 == 0.0 && t.m10 == 0.0 && t.m11 == 1.0;
    // Pure translation: nearest-neighbour reduces to an integer shift (floor(x+0.5-tx) ==
    // x + floor(0.5-tx) for integer x), so copy the overlapping span of each row wholesale.
    // This is the Move tool's hot path while dragging -- per-texel inverse mapping there made
    // drags visibly lag (S15 bug). An integer shift is byte-exact for EVERY filter; a fractional
    // translate only takes this snap-to-nearest path when the filter is Nearest (else it falls
    // through to the kernel, which interpolates the sub-pixel offset).
    if (linearIdentity &&
        (filter == ResampleFilter::Nearest || (isInteger(t.m02) && isInteger(t.m12)))) {
        const long shiftX = static_cast<long>(std::floor(0.5 - t.m02));
        const long shiftY = static_cast<long>(std::floor(0.5 - t.m12));
        // dst(x, y) = src(x + shiftX, y + shiftY) wherever that lands inside src.
        const long x0 = std::max<long>(0, -shiftX);
        const long x1 = std::min<long>(w, static_cast<long>(src.width) - shiftX);
        const long y0 = std::max<long>(0, -shiftY);
        const long y1 = std::min<long>(h, static_cast<long>(src.height) - shiftY);
        if (x1 <= x0 || y1 <= y0) return dst;
        parallelFor(static_cast<std::size_t>(y1 - y0), 64, [&](std::size_t b0, std::size_t b1) {
            for (long y = y0 + static_cast<long>(b0); y < y0 + static_cast<long>(b1); ++y) {
                const std::size_t dp = (static_cast<std::size_t>(y) * w + x0) * 4;
                const std::size_t sp =
                    (static_cast<std::size_t>(y + shiftY) * src.width + (x0 + shiftX)) * 4;
                std::copy_n(src.rgba.begin() + static_cast<std::ptrdiff_t>(sp),
                            static_cast<std::size_t>(x1 - x0) * 4,
                            dst.rgba.begin() + static_cast<std::ptrdiff_t>(dp));
            }
        });
        return dst;
    }
    const std::optional<common::Affine2D> invOpt = t.inverse();
    if (!invOpt) return dst;  // singular transform collapses to nothing
    const common::Affine2D inv = *invOpt;  // hoisted: no optional deref per texel
    if (filter != ResampleFilter::Nearest) {
        const bool clamp = edge == EdgeMode::Clamp;
        const auto fetch = [&src, clamp](long sx, long sy, float out[4]) {
            if (sx < 0 || sy < 0 || sx >= static_cast<long>(src.width) ||
                sy >= static_cast<long>(src.height)) {
                if (!clamp) {
                    out[0] = out[1] = out[2] = out[3] = 0.0f;
                    return;
                }
                // Clamp to edge: the border texel is the last thing the picture knows, and it is
                // what a whole-image resize must extend. See EdgeMode in the header.
                sx = std::clamp<long>(sx, 0, static_cast<long>(src.width) - 1);
                sy = std::clamp<long>(sy, 0, static_cast<long>(src.height) - 1);
            }
            const std::size_t sp =
                (static_cast<std::size_t>(sy) * src.width + static_cast<std::size_t>(sx)) * 4;
            out[0] = src.rgba[sp];
            out[1] = src.rgba[sp + 1];
            out[2] = src.rgba[sp + 2];
            out[3] = src.rgba[sp + 3];
        };
        resampleInto(dst, w, h, inv, filter, fetch);
        return dst;
    }
    parallelFor(h, 64, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            // March the inverse incrementally along the row: one apply() per row, then each x
            // step adds the inverse's first column (two adds per texel, not a matrix apply).
            common::Vec2 p = inv.apply({0.5, y + 0.5});
            for (std::uint32_t x = 0; x < w; ++x, p.x += inv.m00, p.y += inv.m10) {
                long sx = static_cast<long>(std::floor(p.x));
                long sy = static_cast<long>(std::floor(p.y));
                if (edge == EdgeMode::Clamp) {
                    sx = std::clamp<long>(sx, 0, static_cast<long>(src.width) - 1);
                    sy = std::clamp<long>(sy, 0, static_cast<long>(src.height) - 1);
                }
                if (sx >= 0 && sy >= 0 && sx < static_cast<long>(src.width) &&
                    sy < static_cast<long>(src.height)) {
                    dst.set(x, y,
                            src.at(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy)));
                }
            }
        }
    });
    return dst;
}

common::Image transformImage(const common::Image& src, const common::Affine2D& srcToDst,
                             std::uint32_t dstW, std::uint32_t dstH, ResampleFilter filter,
                             EdgeMode edge) {
    if (dstW == 0 || dstH == 0) return {};
    // The 8-bit lane is the float lane with a conversion on each end -- exactly what the crop
    // bake has always done, so an exact placement round-trips byte for byte.
    return common::toImage8(
        transformImageF(common::toFloat(src), srcToDst, dstW, dstH, filter, edge));
}

common::ImageF resampleImageF(const common::ImageF& src, std::uint32_t dstW, std::uint32_t dstH,
                              ResampleFilter filter) {
    if (src.empty() || dstW == 0 || dstH == 0) return {};
    if (dstW == src.width && dstH == src.height) return src;  // bit-exact, whatever the filter
    return transformImageF(src,
                           common::Affine2D::scaling(
                               static_cast<double>(dstW) / static_cast<double>(src.width),
                               static_cast<double>(dstH) / static_cast<double>(src.height)),
                           dstW, dstH, filter, EdgeMode::Clamp);
}

common::Image resampleImage(const common::Image& src, std::uint32_t dstW, std::uint32_t dstH,
                            ResampleFilter filter) {
    if (src.empty() || dstW == 0 || dstH == 0) return {};
    if (dstW == src.width && dstH == src.height) return src;  // bit-exact, whatever the filter
    return transformImage(src,
                          common::Affine2D::scaling(
                              static_cast<double>(dstW) / static_cast<double>(src.width),
                              static_cast<double>(dstH) / static_cast<double>(src.height)),
                          dstW, dstH, filter, EdgeMode::Clamp);
}

}  // namespace mosaic::render
