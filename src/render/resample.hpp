#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "common/thread_pool.hpp"  // common::parallelFor -- the band split the passes below run on
#include "render/render.hpp"

// The resampler: the kernel bank plus the sampling passes that place one pixel grid into another
// through an affine (the "Transform Anti-aliasing" feature, S7). It lived in compositor.cpp's
// anonymous namespace until S53-a, when Image Size / Canvas Size / Rotate / Export all needed the
// same kernels; extracting it changed NOTHING about the math -- every formula, constant,
// premultiplication step, weight-sum normalisation, `double` accumulation and parallelFor split
// below is the compositor's, moved verbatim.
//
// Nothing here knows about layers, masks or documents: pixels in, pixels out. The compositor keeps
// its own fused leaf pass (rasteriseLayerInto -- 8-bit -> float WITH the layer's mask folded at the
// sampled position) and calls the templates here for the kernel work; render/document_ops.hpp's
// whole-document operations call the whole-image entry points at the bottom.
//
// The GPU lane mirrors this file: shaders/composite_tile.comp reproduces kernelRadius,
// kernelWeight, cubicKernel, the footprint widening and the kMaxFootprintRadius cap line for line,
// and tests/test_composite_tile_parity.cpp holds the two lanes to each other. A change to any of
// them must land in both.
namespace mosaic::render {

// One cubic-family kernel parameterised by (B, C) -- the BC-spline of Mitchell & Netravali.
// Catmull-Rom = (0, 1/2) (sharp interpolating), Mitchell = (1/3, 1/3) (balanced), B-spline =
// (1, 0) (smooth approximating). Writing the family once means each named cubic is a parameter
// pair. Exposed for unit testing the kernel shape.
[[nodiscard]] double cubicKernel(double x, double B, double C) noexcept;

// Resolve ResampleFilter::Auto to a concrete kernel for a layer placed by `t` (its local space ->
// the document), given whether a transform gesture is in flight. Pure (no FLTK / no GPU), so it is
// unit-tested directly. Intent buckets, each mapped to the best CURRENTLY-implemented kernel:
//   * lossless (identity / integer translate / 90deg.k rotation + integer scale) -> Nearest
//     (also keeps pixel art crisp);
//   * a live drag (non-lossless, gesture in flight)                              -> Bilinear (cheap);
//   * committed minification (max-axis scale < 1)                                -> Area (no ringing);
//   * committed enlarge / rotate                                                 -> Lanczos3 (sharp).
// A non-Auto value is returned unchanged (the user's explicit pick is honoured).
[[nodiscard]] ResampleFilter chooseAutoFilter(const common::Affine2D& t, bool liveDrag) noexcept;

// A transform that maps the integer pixel grid onto itself: identity, an integer translation, an
// integer scale (incl. flips) and/or a 90-degree.k rotation -- the lossless cases where Nearest is
// exact (and keeps pixel art crisp). The linear part must be a signed integer scale on either the
// axis-aligned (m01=m10=0) or the 90-degree-rotated (m00=m11=0) axes, with an integer translation.
[[nodiscard]] bool isLosslessGrid(const common::Affine2D& t) noexcept;

// ---- Resampling kernels (Transform Anti-aliasing) -----------------------------------------
//
// A transformed layer is placed by mapping each destination texel's centre back through the
// inverse and reconstructing the source there. Nearest point-samples (and aliases on any
// non-90deg rotation or non-integer scale); the higher-quality filters convolve a separable
// reconstruction kernel over the covered source texels. Two refinements make it correct:
//   * PREMULTIPLIED alpha -- filtering straight RGBA bleeds a transparent texel's (arbitrary)
//     RGB into its covered neighbours, so we weight premultiplied colour and un-premultiply once;
//   * FOOTPRINT widening -- on minification one destination texel covers many source texels, so
//     the kernel's source-space support scales by the reduction factor (a box of that width for
//     Area, a widened cubic/Lanczos otherwise) to low-pass instead of point-sampling + aliasing.

// The kernel's half-support in source texels (before footprint widening). Nearest (point) and
// Supersample have dedicated paths and never reach here.
[[nodiscard]] double kernelRadius(ResampleFilter f) noexcept;

// The 1-D kernel weight at offset `t` source texels from the sample centre (after the caller has
// divided out any footprint widening). cubicKernel is the public BC-spline (Bicubic = Catmull-Rom,
// Mitchell = (1/3,1/3)); the rest are local.
[[nodiscard]] double kernelWeight(ResampleFilter f, double t) noexcept;

// Resolve ResampleFilter::Auto to the concrete kernel for a layer placed by `t`.
[[nodiscard]] ResampleFilter resolveFilter(ResampleFilter user, const common::Affine2D& t,
                                           bool liveDrag = false) noexcept;

// Safety cap on the per-pixel footprint: under a heavy minification the footprint grows with the
// reduction factor (radius x scale), so cap each radius -- an extreme shrink then aliases
// slightly instead of taking thousands of taps per pixel (a multi-second/minute commit). The
// proper fix for heavy minification is mip-style pre-downsampling (a follow-up).
inline constexpr double kMaxFootprintRadius = 8.0;

// Bilinear sample at source point (px,py), returned PREMULTIPLIED. `fetch(sx,sy,out)` gives the
// straight-alpha source texel in [0,1] (out = {0,0,0,0} outside the source). Texel centres sit at
// integer+0.5. Used by the Supersample path's per-subsample fetch.
template <typename Fetch>
void bilinearPremul(Fetch&& fetch, double px, double py, double out[4]) {
    const double fx = px - 0.5, fy = py - 0.5;
    const long x0 = static_cast<long>(std::floor(fx));
    const long y0 = static_cast<long>(std::floor(fy));
    const double tx = fx - static_cast<double>(x0), ty = fy - static_cast<double>(y0);
    const double ws[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
    float c[4][4];
    fetch(x0, y0, c[0]);
    fetch(x0 + 1, y0, c[1]);
    fetch(x0, y0 + 1, c[2]);
    fetch(x0 + 1, y0 + 1, c[3]);
    out[0] = out[1] = out[2] = out[3] = 0.0;
    for (int k = 0; k < 4; ++k) {
        const double aw = static_cast<double>(c[k][3]) * ws[k];
        out[0] += static_cast<double>(c[k][0]) * aw;
        out[1] += static_cast<double>(c[k][1]) * aw;
        out[2] += static_cast<double>(c[k][2]) * aw;
        out[3] += aw;
    }
}

// Convolve `filter`'s separable kernel over the source described by `fetch` into `dst` (assumed
// pre-zeroed, straight-alpha), mapping each of the w x h destination texels through `inv`
// (document -> source). Premultiplication + footprint widening + normalisation happen here.
template <typename Fetch>
void convolveInto(common::ImageF& dst, std::uint32_t w, std::uint32_t h,
                  const common::Affine2D& inv, ResampleFilter filter, Fetch&& fetch) {
    // Source texels per destination texel along each source axis = the inverse-map column lengths.
    const double sclX = std::max(1.0, std::hypot(inv.m00, inv.m10));
    const double sclY = std::max(1.0, std::hypot(inv.m01, inv.m11));
    // The per-pixel tap footprint, capped at kMaxFootprintRadius (see the constant above).
    const double rx = std::min(kernelRadius(filter) * sclX, kMaxFootprintRadius);
    const double ry = std::min(kernelRadius(filter) * sclY, kMaxFootprintRadius);
    const double invSclX = 1.0 / sclX, invSclY = 1.0 / sclY;
    common::parallelFor(h, 32, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            common::Vec2 p = inv.apply({0.5, y + 0.5});
            std::size_t dp = static_cast<std::size_t>(y) * w * 4;
            for (std::uint32_t x = 0; x < w; ++x, p.x += inv.m00, p.y += inv.m10, dp += 4) {
                const long sx0 = static_cast<long>(std::ceil(p.x - 0.5 - rx));
                const long sx1 = static_cast<long>(std::floor(p.x - 0.5 + rx));
                const long sy0 = static_cast<long>(std::ceil(p.y - 0.5 - ry));
                const long sy1 = static_cast<long>(std::floor(p.y - 0.5 + ry));
                double pr = 0, pg = 0, pb = 0, pa = 0, wsum = 0;
                for (long sy = sy0; sy <= sy1; ++sy) {
                    const double wy = kernelWeight(filter, ((sy + 0.5) - p.y) * invSclY);
                    if (wy == 0.0) continue;
                    for (long sx = sx0; sx <= sx1; ++sx) {
                        const double wgt = wy * kernelWeight(filter, ((sx + 0.5) - p.x) * invSclX);
                        if (wgt == 0.0) continue;
                        float c[4];
                        fetch(sx, sy, c);
                        const double aw = static_cast<double>(c[3]) * wgt;
                        pr += static_cast<double>(c[0]) * aw;  // premultiplied accumulation
                        pg += static_cast<double>(c[1]) * aw;
                        pb += static_cast<double>(c[2]) * aw;
                        pa += aw;
                        wsum += wgt;
                    }
                }
                if (wsum <= 0.0) continue;  // nothing covered -> leave transparent
                dst.rgba[dp + 3] = static_cast<float>(std::clamp(pa / wsum, 0.0, 1.0));
                if (pa > 1e-8) {  // un-premultiply (rgb is meaningless at ~zero coverage)
                    dst.rgba[dp + 0] = static_cast<float>(pr / pa);
                    dst.rgba[dp + 1] = static_cast<float>(pg / pa);
                    dst.rgba[dp + 2] = static_cast<float>(pb / pa);
                }
            }
        }
    });
}

// Brute-force N x N supersampling: average N x N bilinear sub-samples across each destination
// texel. N scales with the minification factor (capped) so a heavy reduction still anti-aliases.
template <typename Fetch>
void supersampleInto(common::ImageF& dst, std::uint32_t w, std::uint32_t h,
                     const common::Affine2D& inv, Fetch&& fetch) {
    const double foot =
        std::max({1.0, std::hypot(inv.m00, inv.m10), std::hypot(inv.m01, inv.m11)});
    const int n = std::clamp(static_cast<int>(std::ceil(foot)) + 1, 2, 8);
    const double step = 1.0 / n;
    const double norm = 1.0 / (static_cast<double>(n) * n);
    common::parallelFor(h, 32, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            std::size_t dp = static_cast<std::size_t>(y) * w * 4;
            for (std::uint32_t x = 0; x < w; ++x, dp += 4) {
                double acc[4] = {0, 0, 0, 0};
                for (int j = 0; j < n; ++j) {
                    for (int i = 0; i < n; ++i) {
                        const common::Vec2 s =
                            inv.apply({static_cast<double>(x) + (i + 0.5) * step,
                                       static_cast<double>(y) + (j + 0.5) * step});
                        double sub[4];
                        bilinearPremul(fetch, s.x, s.y, sub);
                        acc[0] += sub[0];
                        acc[1] += sub[1];
                        acc[2] += sub[2];
                        acc[3] += sub[3];
                    }
                }
                dst.rgba[dp + 3] = static_cast<float>(std::clamp(acc[3] * norm, 0.0, 1.0));
                if (acc[3] > 1e-8) {  // un-premultiply
                    dst.rgba[dp + 0] = static_cast<float>(acc[0] / acc[3]);
                    dst.rgba[dp + 1] = static_cast<float>(acc[1] / acc[3]);
                    dst.rgba[dp + 2] = static_cast<float>(acc[2] / acc[3]);
                }
            }
        }
    });
}

// Dispatch a non-Nearest, non-fast-path resample of `fetch` into the pre-zeroed `dst` through `inv`.
template <typename Fetch>
void resampleInto(common::ImageF& dst, std::uint32_t w, std::uint32_t h,
                  const common::Affine2D& inv, ResampleFilter filter, Fetch&& fetch) {
    if (filter == ResampleFilter::Supersample)
        supersampleInto(dst, w, h, inv, std::forward<Fetch>(fetch));
    else
        convolveInto(dst, w, h, inv, filter, std::forward<Fetch>(fetch));
}

// ---- Whole-image entry points ----------------------------------------------------------------
//
// The four below are the compositor's sampler offered to callers that have an image rather than a
// layer: the export pipeline's resize stage and the S53-a document operations. They carry the same
// guarantees the walk relies on -- premultiplied sampling, a widened footprint on minification, an
// exact copy whenever the placement is a whole-pixel one -- and are deterministic (no GPU, no
// floating-point reduction order that depends on the thread count).

// What a kernel tap reads when its footprint reaches past the source.
//
// The two policies are not a preference — they answer different questions. Placing a LAYER, the
// area outside it genuinely is nothing, so a tap there must contribute transparent or the layer
// grows a halo of invented colour: that is Transparent, and it is what the compositor has always
// done. Resizing a whole IMAGE, there is no outside: the picture's border pixel is the last thing
// known, so a tap must clamp to it. Sampling transparent there instead drags the edge's alpha down
// (a 32x8 opaque ramp resampled 2x up and back came home at alpha 212 on every row within a kernel
// radius of an edge), which on Image Size would have handed the user a translucent border on an
// opaque photo. Transparent stays the default so every compositor call site is byte-identical.
enum class EdgeMode { Transparent, Clamp };

// Place `src` (in its own pixel space) into a `dstW` x `dstH` buffer through `srcToDst`, resampling
// with `filter`. Nearest (the default) is the lossless, dependency-free baseline the compositor's
// legacy callers use; ResampleFilter::Auto resolves through chooseAutoFilter. An identity placement
// at matching size, and any whole-pixel translation, are exact copies. A singular `srcToDst`
// collapses to an empty (transparent) result.
[[nodiscard]] common::ImageF transformImageF(const common::ImageF& src,
                                             const common::Affine2D& srcToDst, std::uint32_t dstW,
                                             std::uint32_t dstH,
                                             ResampleFilter filter = ResampleFilter::Nearest,
                                             EdgeMode edge = EdgeMode::Transparent);

// The 8-bit twin of transformImageF: straight-alpha RGBA in and out, sampled in float through the
// same kernels (so a round trip through an exact placement is byte-identical).
[[nodiscard]] common::Image transformImage(const common::Image& src,
                                           const common::Affine2D& srcToDst, std::uint32_t dstW,
                                           std::uint32_t dstH,
                                           ResampleFilter filter = ResampleFilter::Nearest,
                                           EdgeMode edge = EdgeMode::Transparent);

// Scale the whole of `src` to `dstW` x `dstH` (the Image Size / export resize stage). Equivalent to
// transformImageF through Affine2D::scaling(dstW/srcW, dstH/srcH) at EdgeMode::Clamp, with Auto
// resolved against that same scale. An empty source or a zero target gives an empty image; an
// exact-size request returns a bit-exact copy whatever the filter.
[[nodiscard]] common::ImageF resampleImageF(const common::ImageF& src, std::uint32_t dstW,
                                            std::uint32_t dstH, ResampleFilter filter);
[[nodiscard]] common::Image resampleImage(const common::Image& src, std::uint32_t dstW,
                                          std::uint32_t dstH, ResampleFilter filter);

}  // namespace mosaic::render
