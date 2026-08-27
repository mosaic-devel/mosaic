#include "render/compositor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <numbers>
#include <type_traits>
#include <vector>

#include "common/geometry.hpp"
#include "common/log.hpp"
#include "common/profiler.hpp"
#include "common/thread_pool.hpp"
#include "core/adjustments.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/layer_effects.hpp"
#include "core/vector/flatten.hpp"  // vec::contentBounds(Object) -- the merge-down vector route
#include "core/vector/raster.hpp"
#include "core/vector/to_path.hpp"  // vec::pathFromGeometry / transformedPath (same route)
#include "render/blend.hpp"
#include "render/blur.hpp"
#include "render/effect_primitives.hpp"  // fx::boxBlurApprox -- the S34 shadows/highlights mask
#include "render/gpu_compositor.hpp"
#include "render/layer_effects_render.hpp"
#include "render/resample.hpp"
#include "render/stylize.hpp"  // the S35 artistic/stylize family (one branch, three functions)

namespace mosaic::render {
namespace {

using common::ColorF;
using common::ImageF;

// How a source layer is blended onto the accumulator. Swappable so the tree walk is shared by
// the CPU reference and the GPU compute path (S7-b): only this one hot, per-pixel step differs.
// `bounds` (nullptr = the whole buffer) is the target-space rect the source can possibly be
// non-transparent in -- reported by whoever RENDERED it, never re-derived from geometry here. That
// distinction is the safety property: a re-derived rect that under-estimates silently drops
// pixels, while a producer reporting the window it actually wrote cannot.
using BlendFn = std::function<void(ImageF& acc, const ImageF& src, core::BlendMode mode,
                                   float opacity, const common::Rect* bounds)>;

// A transform that maps every point to itself (the common case: a layer drawn 1:1).
[[nodiscard]] bool isIdentity(const common::Affine2D& t) noexcept {
    return t.m00 == 1.0 && t.m01 == 0.0 && t.m02 == 0.0 && t.m10 == 0.0 && t.m11 == 1.0 &&
           t.m12 == 0.0;
}

// Split [0, count) into contiguous bands across hardware threads and run fn(begin, end) on
// each. Bands touch disjoint ranges, so results stay bit-identical to the serial loop (the
// golden test remains valid). `minPerBand` keeps small inputs serial — composites run per
// frame during gestures, and the full-document walks below were the bulk of that cost
// (S15.x perf pass: ~300 ms/frame single-threaded at 1080p even at -O2).
//
// S60-b moved the loop to the shared pool (common/thread_pool.hpp): the SPLIT is unchanged --
// same band count, same [i0, i1) -- but the bands now run on parked workers instead of a
// std::vector<std::thread> built and joined per call, which at 18 call sites times the layers
// in the document was a measurable slice of every gesture frame.
using common::parallelFor;

// ---- Resampling (Transform Anti-aliasing) --------------------------------------------------
//
// The kernel bank (kernelRadius / kernelWeight / cubicKernel / chooseAutoFilter), the convolve +
// supersample passes and the whole-image samplers live in render/resample.hpp since S53-a, where
// the document operations (Image Size / Canvas Size / Rotate) and the export resize can reach
// them. What stays here is the compositor's own fused LEAF pass below: an 8-bit (or float) source
// placed into the document buffer THROUGH the transform, with the layer's mask folded at the
// sampled position.

constexpr double kPi = 3.14159265358979323846;

[[nodiscard]] bool isInteger(double v) noexcept { return v == std::floor(v); }

// One fused pass replacing toFloat + foldMask + transformImageF for LEAF layers: convert the
// 8-bit source to float THROUGH the transform, folding the optional mask at the sampled
// position. The split version walked (and allocated) three full document-sized frames per
// layer per composite — the bulk of the per-frame cost during drags (S15.x perf pass).
// `dst` is reset to transparent at w x h, reusing its allocation when the size already
// matches — the drag replay (S15-b) re-rasterises one layer per frame, and a fresh ~32 MiB
// buffer per frame spends more time in page faults than in the loop.
// Templated over the source depth (S55-a): a common::Image source converts 8-bit -> float on the
// way through; a common::ImageF source (the texture sky cache, §4.4 float lane) passes through
// unquantised — same fused pass, no banding round-trip. The 8-bit instantiation is byte-identical
// to the pre-template code.
template <typename SrcImage>
void rasteriseLayerInto(ImageF& dst, const SrcImage& src, const core::RasterMask* mask,
                        const common::Affine2D& t, std::uint32_t w, std::uint32_t h,
                        ResampleFilter filter = ResampleFilter::Nearest,
                        common::Rect* written = nullptr) {
    // `written` is the target-space rect this pass actually touched. Every early return below
    // leaves it EMPTY, and every path that draws sets it to its own destination window -- so the
    // caller's blend can skip the rest of the buffer knowing it is still transparent. Reporting
    // what was written beats re-deriving it from the layer's geometry, which would have to guess
    // at stroke overhang, resample footprints and mask placement and would drop pixels when it
    // guessed low.
    if (written != nullptr)
        *written = common::Rect{};
    dst.width = w;
    dst.height = h;
    const std::size_t n = static_cast<std::size_t>(w) * h * 4;
    workCounters().clearedTexels.fetch_add(static_cast<std::uint64_t>(w) * h,
                                           std::memory_order_relaxed);
    {
        MOSAIC_PERF_SCOPE("Layer buffer clear", common::Lane::Cpu);
        if (dst.rgba.size() == n) {
            // REUSED buffer (the drag replay re-rasterises one layer per frame into the same
            // allocation): the previous layer's pixels are still in it, so it really must be
            // written back to transparent.
            parallelFor(n, std::size_t{1} << 18, [&](std::size_t i0, std::size_t i1) {
                std::fill(dst.rgba.begin() + static_cast<std::ptrdiff_t>(i0),
                          dst.rgba.begin() + static_cast<std::ptrdiff_t>(i1), 0.0f);
            });
        } else {
            // FRESH buffer -- the walk's case, once per leaf per composite. `assign(n, 0.0f)` was
            // a 637 MB memset at 39.8 MP (75 ms, single-threaded, ~26 times per composite of the
            // S60 fixture) writing zeros over memory the kernel had ALREADY zeroed. Constructing
            // instead hands back calloc'd pages untouched, and they fault in only where this pass
            // writes -- which is the destination window it computes three lines down, not the
            // canvas. Same guarantee (all-zero, see ZeroPageAllocator), none of the traffic.
            dst.rgba = common::Floats(n);
        }
    }
    if (src.empty()) return;
    if (mask != nullptr && (mask->empty() || !mask->enabled)) mask = nullptr;

    constexpr float kInv255 = 1.0f / 255.0f;  // the 8-bit mask's coverage scale
    // Source texel scale: 8-bit normalises to [0,1]; a float source is already there.
    constexpr float kSrcScale =
        std::is_same_v<SrcImage, common::Image> ? 1.0f / 255.0f : 1.0f;
    const auto maskCov = [mask, &src](std::uint32_t sx, std::uint32_t sy) {
        if (mask == nullptr) return 1.0f;
        const std::uint32_t mx =
            mask->width == src.width
                ? sx
                : static_cast<std::uint32_t>(static_cast<std::size_t>(sx) * mask->width / src.width);
        const std::uint32_t my =
            mask->height == src.height
                ? sy
                : static_cast<std::uint32_t>(static_cast<std::size_t>(sy) * mask->height /
                                             src.height);
        return mask->coverage[static_cast<std::size_t>(my) * mask->width + mx] * kInv255;
    };
    const auto emit = [&](std::size_t dp, std::uint32_t sx, std::uint32_t sy) {
        const std::size_t sp = (static_cast<std::size_t>(sy) * src.width + sx) * 4;
        dst.rgba[dp + 0] = src.rgba[sp + 0] * kSrcScale;
        dst.rgba[dp + 1] = src.rgba[sp + 1] * kSrcScale;
        dst.rgba[dp + 2] = src.rgba[sp + 2] * kSrcScale;
        dst.rgba[dp + 3] = src.rgba[sp + 3] * kSrcScale * maskCov(sx, sy);
    };

    const bool linearIdentity = t.m00 == 1.0 && t.m01 == 0.0 && t.m10 == 0.0 && t.m11 == 1.0;
    if (linearIdentity &&
        (filter == ResampleFilter::Nearest || (isInteger(t.m02) && isInteger(t.m12)))) {
        // Pure translation (identity included) is an integer shift — see transformImageF.
        const long shiftX = static_cast<long>(std::floor(0.5 - t.m02));
        const long shiftY = static_cast<long>(std::floor(0.5 - t.m12));
        const long x0 = std::max<long>(0, -shiftX);
        const long x1 = std::min<long>(w, static_cast<long>(src.width) - shiftX);
        const long y0 = std::max<long>(0, -shiftY);
        const long y1 = std::min<long>(h, static_cast<long>(src.height) - shiftY);
        if (x1 <= x0 || y1 <= y0) return;
        if (written != nullptr)
            *written = common::Rect{static_cast<double>(x0), static_cast<double>(y0),
                                    static_cast<double>(x1 - x0), static_cast<double>(y1 - y0)};
        parallelFor(static_cast<std::size_t>(y1 - y0), 64, [&](std::size_t b0, std::size_t b1) {
            for (long y = y0 + static_cast<long>(b0); y < y0 + static_cast<long>(b1); ++y) {
                const auto sy = static_cast<std::uint32_t>(y + shiftY);
                std::size_t dp = (static_cast<std::size_t>(y) * w + x0) * 4;
                for (long x = x0; x < x1; ++x, dp += 4)
                    emit(dp, static_cast<std::uint32_t>(x + shiftX), sy);
            }
        });
        return;
    }
    const std::optional<common::Affine2D> invOpt = t.inverse();
    if (!invOpt) return;  // singular transform shows nothing (compositor semantics)
    const common::Affine2D inv = *invOpt;
    if (filter != ResampleFilter::Nearest) {
        // The fused fetch: straight-alpha source with the mask folded into alpha, 0 outside.
        const auto fetch = [&](long sx, long sy, float out[4]) {
            if (sx < 0 || sy < 0 || sx >= static_cast<long>(src.width) ||
                sy >= static_cast<long>(src.height)) {
                out[0] = out[1] = out[2] = out[3] = 0.0f;
                return;
            }
            const std::size_t sp =
                (static_cast<std::size_t>(sy) * src.width + static_cast<std::size_t>(sx)) * 4;
            out[0] = src.rgba[sp + 0] * kSrcScale;
            out[1] = src.rgba[sp + 1] * kSrcScale;
            out[2] = src.rgba[sp + 2] * kSrcScale;
            out[3] = src.rgba[sp + 3] * kSrcScale *
                     maskCov(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy));
        };
        // The extent is safe to hand over: `fetch` above returns {0,0,0,0} outside the source,
        // which is exactly convolveInto's clip precondition. This is what stops a headline-sized
        // text layer from paying a canvas-sized convolution.
        if (written != nullptr) {
            // The same projection resampleInto clips to, dilated by the kernel's reach and clamped
            // -- conservative on purpose: a rect slightly too LARGE only costs a few blended
            // texels, one slightly too small drops them.
            const common::Rect reach{
                -kMaxFootprintRadius - 1.0, -kMaxFootprintRadius - 1.0,
                static_cast<double>(src.width) + 2.0 * kMaxFootprintRadius + 2.0,
                static_cast<double>(src.height) + 2.0 * kMaxFootprintRadius + 2.0};
            *written = t.mapBounds(reach);
        }
        resampleInto(dst, w, h, inv, filter, fetch, src.width, src.height);
        return;
    }
    // The destination window the source can actually reach. Without it this walks the WHOLE
    // buffer -- inverse-mapping and bounds-testing every one of a canvas's texels to emit the few
    // thousand a small layer covers. That is the same defect resample.hpp's convolveInto carried,
    // in its sibling branch: this is the NEAREST path, so the clip is exact rather than dilated by
    // a kernel radius (one texel of slack for the floor()).
    const common::Rect srcRect{-1.0, -1.0, static_cast<double>(src.width) + 2.0,
                               static_cast<double>(src.height) + 2.0};
    const common::Rect d = t.mapBounds(srcRect);
    const auto clampU = [](double v, std::uint32_t hi) {
        return v <= 0.0 ? std::uint32_t{0} : static_cast<std::uint32_t>(std::min<double>(v, hi));
    };
    const std::uint32_t dx0 = clampU(std::floor(d.x), w);
    const std::uint32_t dx1 = clampU(std::ceil(d.right()), w);
    const std::uint32_t dy0 = clampU(std::floor(d.y), h);
    const std::uint32_t dy1 = clampU(std::ceil(d.bottom()), h);
    if (dx1 <= dx0 || dy1 <= dy0)
        return; // the source projects entirely off the destination
    if (written != nullptr)
        *written = common::Rect{static_cast<double>(dx0), static_cast<double>(dy0),
                                static_cast<double>(dx1 - dx0), static_cast<double>(dy1 - dy0)};
    parallelFor(dy1 - dy0, 64, [&](std::size_t band0, std::size_t band1) {
        for (std::uint32_t y = dy0 + static_cast<std::uint32_t>(band0),
                           yEnd = dy0 + static_cast<std::uint32_t>(band1);
             y < yEnd; ++y) {
            common::Vec2 p = inv.apply({dx0 + 0.5, y + 0.5});
            std::size_t dp = (static_cast<std::size_t>(y) * w + dx0) * 4;
            for (std::uint32_t x = dx0; x < dx1; ++x, p.x += inv.m00, p.y += inv.m10, dp += 4) {
                const long sx = static_cast<long>(std::floor(p.x));
                const long sy = static_cast<long>(std::floor(p.y));
                if (sx >= 0 && sy >= 0 && sx < static_cast<long>(src.width) &&
                    sy < static_cast<long>(src.height))
                    emit(dp, static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy));
            }
        }
    });
}

template <typename SrcImage>
[[nodiscard]] ImageF rasteriseLayer(const SrcImage& src, const core::RasterMask* mask,
                                    const common::Affine2D& t, std::uint32_t w, std::uint32_t h,
                                    ResampleFilter filter = ResampleFilter::Nearest,
                                    common::Rect* written = nullptr) {
    ImageF dst;
    rasteriseLayerInto(dst, src, mask, t, w, h, filter, written);
    return dst;
}

// Multiply each texel's alpha by the mask sampled THROUGH a transform: `imgToMask` maps a buffer
// pixel (its centre) to a point on the mask's grid, nearest-sampled; points OUTSIDE the grid read
// zero coverage -- the mask sheet defines everything it reveals, beyond it nothing shows. Serves
// every fold the window-aligned foldMask below cannot (S31): a group mask folded onto an OFFSET
// local buffer (buffer -> group-local is a translation), an UNLINKED mask fixed in the layer's
// parent space (buffer -> parent via pre^-1), and a vector layer's mask at target resolution
// (buffer -> layer-local via place^-1).
void foldMaskThrough(ImageF& img, const core::RasterMask& mask,
                     const common::Affine2D& imgToMask) {
    if (mask.empty() || img.empty()) return;
    constexpr float kInv255 = 1.0f / 255.0f;
    parallelFor(img.height, 64, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            common::Vec2 p = imgToMask.apply({0.5, y + 0.5});
            std::size_t ap = (static_cast<std::size_t>(y) * img.width) * 4 + 3;
            for (std::uint32_t x = 0; x < img.width;
                 ++x, p.x += imgToMask.m00, p.y += imgToMask.m10, ap += 4) {
                const long mx = static_cast<long>(std::floor(p.x));
                const long my = static_cast<long>(std::floor(p.y));
                float cov = 0.0f;
                if (mx >= 0 && my >= 0 && mx < static_cast<long>(mask.width) &&
                    my < static_cast<long>(mask.height))
                    cov = mask.coverage[static_cast<std::size_t>(my) * mask.width + mx] * kInv255;
                img.rgba[ap] *= cov;
            }
        }
    });
}

// The mask rasteriseLayerInto may fold at the SOURCE pixel: only a LINKED one. An unlinked mask
// is folded after placement, in the layer's parent space (foldUnlinkedMask below).
[[nodiscard]] const core::RasterMask* linkedMask(const core::Layer& layer) {
    const core::RasterMask* m = layer.mask();
    return (m != nullptr && !m->linked) ? nullptr : m;
}

// Fold an enabled UNLINKED mask over a layer's placed buffer (S31 link/unlink): the mask sits
// still in the layer's PARENT space -- its sheet placed there by core::maskPlacement, which for an
// unlinked mask is RasterMask::toLocal -- so moving the layer slides the pixels under a stationary
// mask. `pre` maps parent space to the buffer, so the fold samples through the inverse of the
// sheet's placement carried into it. No-op for null/disabled/linked/empty masks.
void foldUnlinkedMask(ImageF& img, const core::Layer& layer, const common::Affine2D& pre) {
    const core::RasterMask* m = layer.mask();
    if (m == nullptr || m->linked || !m->enabled || m->empty()) return;
    const std::optional<common::Affine2D> inv = (pre * core::maskPlacement(layer, *m)).inverse();
    if (!inv) return;  // a singular placement shows nothing anyway
    foldMaskThrough(img, *m, *inv);
}

// Multiply each texel's alpha by the layer's mask coverage. The mask shares the layer's space
// (it is linked, PLAN §3.7); when its resolution differs it is sampled proportionally.
void foldMask(ImageF& img, const core::RasterMask& mask) {
    if (mask.empty() || img.empty()) return;
    for (std::uint32_t y = 0; y < img.height; ++y) {
        const std::uint32_t my = mask.height == img.height
                                     ? y
                                     : static_cast<std::uint32_t>(static_cast<std::size_t>(y) *
                                                                  mask.height / img.height);
        for (std::uint32_t x = 0; x < img.width; ++x) {
            const std::uint32_t mx = mask.width == img.width
                                         ? x
                                         : static_cast<std::uint32_t>(static_cast<std::size_t>(x) *
                                                                      mask.width / img.width);
            const float cov = mask.coverage[static_cast<std::size_t>(my) * mask.width + mx] / 255.0f;
            const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
            img.rgba[p + 3] *= cov;
        }
    }
}

// Multiply each texel's alpha by a document-space coverage buffer (used for clip-to-below).
void multiplyAlpha(ImageF& img, const std::vector<float>& coverage) {
    const std::size_t n = std::min(img.pixelCount(), coverage.size());
    parallelFor(n, std::size_t{1} << 16, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) img.rgba[i * 4 + 3] *= coverage[i];
    });
}

// Blend `src` over `acc` in place, per texel, with the layer's blend mode and opacity.
void compositeBufferOver(ImageF& acc, const ImageF& src, core::BlendMode mode, float opacity,
                         const common::Rect* bounds = nullptr) {
    // ⚠ Skipping outside `bounds` is EXACT, not an approximation, and it is worth stating why:
    // outside its written window the source buffer is transparent, and a fully transparent source
    // is the identity for EVERY separable blend mode (W3C Compositing L1: the result is
    // (1 - as)*Cb + as*B(Cb, Cs), which is Cb at as == 0). So the bounded walk and the full walk
    // produce byte-identical accumulators; the bound only decides how much memory gets read.
    //
    // Without it, a 300 px layer still costs one full pass over a canvas-sized alpha channel --
    // 637 MB at 39.8 MP -- which is the whole of this row's cost on a large document.
    if (bounds != nullptr && acc.width == src.width && acc.height == src.height && acc.width != 0) {
        const auto lo = [](double v, std::uint32_t hi) {
            return v <= 0.0 ? std::uint32_t{0}
                            : static_cast<std::uint32_t>(std::min<double>(std::floor(v), hi));
        };
        const auto up = [](double v, std::uint32_t hi) {
            return v <= 0.0 ? std::uint32_t{0}
                            : static_cast<std::uint32_t>(std::min<double>(std::ceil(v), hi));
        };
        const std::uint32_t x0 = lo(bounds->x, acc.width), x1 = up(bounds->right(), acc.width);
        const std::uint32_t y0 = lo(bounds->y, acc.height), y1 = up(bounds->bottom(), acc.height);
        if (x1 <= x0 || y1 <= y0)
            return; // nothing of the source lands on the target
        const std::uint32_t stride = acc.width;
        parallelFor(y1 - y0, 16, [&](std::size_t b0, std::size_t b1) {
            for (std::uint32_t y = y0 + static_cast<std::uint32_t>(b0),
                               yEnd = y0 + static_cast<std::uint32_t>(b1);
                 y < yEnd; ++y) {
                for (std::uint32_t x = x0; x < x1; ++x) {
                    const std::size_t p = (static_cast<std::size_t>(y) * stride + x) * 4;
                    if (src.rgba[p + 3] * opacity <= 0.0f)
                        continue;
                    const ColorF backdrop{acc.rgba[p], acc.rgba[p + 1], acc.rgba[p + 2],
                                          acc.rgba[p + 3]};
                    const ColorF source{src.rgba[p], src.rgba[p + 1], src.rgba[p + 2],
                                        src.rgba[p + 3]};
                    const ColorF out = compositeOver(mode, backdrop, source, opacity);
                    acc.rgba[p] = out.r;
                    acc.rgba[p + 1] = out.g;
                    acc.rgba[p + 2] = out.b;
                    acc.rgba[p + 3] = out.a;
                }
            }
        });
        return;
    }
    const std::size_t n = std::min(acc.pixelCount(), src.pixelCount());
    parallelFor(n, std::size_t{1} << 15, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) {
            const std::size_t p = i * 4;
            // compositeOver returns the backdrop untouched for a fully transparent source;
            // skip its read-modify-write here so layers that cover little of the canvas (the
            // common case) cost ~one read per texel instead of three memory passes.
            if (src.rgba[p + 3] * opacity <= 0.0f) continue;
            const ColorF backdrop{acc.rgba[p], acc.rgba[p + 1], acc.rgba[p + 2], acc.rgba[p + 3]};
            const ColorF source{src.rgba[p], src.rgba[p + 1], src.rgba[p + 2], src.rgba[p + 3]};
            const ColorF out = compositeOver(mode, backdrop, source, opacity);
            acc.rgba[p] = out.r;
            acc.rgba[p + 1] = out.g;
            acc.rgba[p + 2] = out.b;
            acc.rgba[p + 3] = out.a;
        }
    });
}

[[nodiscard]] double param(const core::AdjustmentLayer& adj, const char* key, double fallback) {
    const auto& p = adj.params();
    const auto it = p.find(key);
    return it == p.end() ? fallback : it->second;
}

// sRGB <-> linear LUT pair for the PhotometricMatch grade (its math is physical, so it works in
// linear light while the accumulator stays in the encoded working space). 2048 entries with
// linear interpolation over [0,1]; out-of-range values (the HDR headroom a sky texture's cache
// can carry) fall back to the analytic IEC 61966-2-1 curves.
constexpr int kSrgbLutSize = 2048;

[[nodiscard]] float srgbDecodeAnalytic(float e) noexcept {
    if (e <= 0.04045f) return std::max(0.0f, e) / 12.92f;
    return std::pow((e + 0.055f) / 1.055f, 2.4f);
}
[[nodiscard]] float srgbEncodeAnalytic(float l) noexcept {
    if (l <= 0.0031308f) return std::max(0.0f, l) * 12.92f;
    return 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

[[nodiscard]] const std::array<float, kSrgbLutSize + 1>& srgbDecodeLut() {
    static const auto lut = [] {
        std::array<float, kSrgbLutSize + 1> t{};
        for (int i = 0; i <= kSrgbLutSize; ++i)
            t[i] = srgbDecodeAnalytic(static_cast<float>(i) / kSrgbLutSize);
        return t;
    }();
    return lut;
}
[[nodiscard]] const std::array<float, kSrgbLutSize + 1>& srgbEncodeLut() {
    static const auto lut = [] {
        std::array<float, kSrgbLutSize + 1> t{};
        for (int i = 0; i <= kSrgbLutSize; ++i)
            t[i] = srgbEncodeAnalytic(static_cast<float>(i) / kSrgbLutSize);
        return t;
    }();
    return lut;
}

[[nodiscard]] float lutSample(const std::array<float, kSrgbLutSize + 1>& lut, float v) noexcept {
    const float f = v * kSrgbLutSize;
    const int i = static_cast<int>(f);
    if (i < 0) return lut[0];
    if (i >= kSrgbLutSize) return lut[kSrgbLutSize];
    const float t = f - static_cast<float>(i);
    return lut[i] + (lut[i + 1] - lut[i]) * t;
}

[[nodiscard]] float srgbToLinear(float e) noexcept {
    return e <= 1.0f ? lutSample(srgbDecodeLut(), e) : srgbDecodeAnalytic(e);
}
[[nodiscard]] float linearToSrgb(float l) noexcept {
    return l <= 1.0f ? lutSample(srgbEncodeLut(), l) : srgbEncodeAnalytic(l);
}

// The PhotometricMatch constants, hoisted out of the pixel loop: every knob is a scalar in the
// params bag (written by texture::photometricMatchParams; absent keys default to the identity so
// an empty bag is a no-op). See docs/research-sky-estimate-from-layer.md §6.2 for the transfer.
struct PhotometricMatchConsts {
    float gainR = 1.0f, gainG = 1.0f, gainB = 1.0f;  // von Kries diagonal WB gains
    float mu = 0.0f;                                 // foreground mean log-luminance (ln)
    float lnScale = 0.0f;                            // ln(2^delta_ev)
    float sigmaRatio = 1.0f;                         // log-luminance contrast ratio
    float gradient = 0.0f;                           // vertical luminance gradient amplitude
    float rod = 0.0f;                                // scotopic rod fraction (0 by day)
    float nightR = 0.42f, nightG = 0.55f, nightB = 1.0f;  // rod-signal tint
    float nightGain = 1.0f;
    float saturation = 1.0f;  // post-mix saturation scale
};

[[nodiscard]] PhotometricMatchConsts photometricMatchConsts(const core::AdjustmentLayer& adj) {
    PhotometricMatchConsts c;
    c.gainR = static_cast<float>(param(adj, "gain_r", 1.0));
    c.gainG = static_cast<float>(param(adj, "gain_g", 1.0));
    c.gainB = static_cast<float>(param(adj, "gain_b", 1.0));
    c.mu = static_cast<float>(param(adj, "mu_log", std::log(0.18)));
    c.lnScale = static_cast<float>(param(adj, "delta_ev", 0.0) * std::numbers::ln2);
    c.sigmaRatio = static_cast<float>(std::clamp(param(adj, "sigma_ratio", 1.0), 0.5, 2.0));
    c.gradient = static_cast<float>(std::clamp(param(adj, "gradient", 0.0), -0.25, 0.25));
    c.rod = static_cast<float>(std::clamp(param(adj, "rod", 0.0), 0.0, 1.0));
    c.nightR = static_cast<float>(param(adj, "night_r", 0.42));
    c.nightG = static_cast<float>(param(adj, "night_g", 0.55));
    c.nightB = static_cast<float>(param(adj, "night_b", 1.0));
    c.nightGain = static_cast<float>(std::max(0.0, param(adj, "night_gain", 1.0)));
    c.saturation = static_cast<float>(std::clamp(param(adj, "saturation", 1.0), 0.0, 2.0));
    return c;
}

// The S32 scalar adjustments (Levels / Exposure / HueSaturation / ColorBalance / Threshold /
// Posterize) read through the core::adjustments schema -- absent keys fall back to the declared
// default and present ones clamp to the declared range, so the compositor and the editor can
// never disagree about a parameter's meaning and a hostile .mosaic file can never feed the math
// garbage. (BrightnessContrast and PhotometricMatch keep their original raw param() reads:
// their behavior predates the schema and stays byte-identical.) `identity` marks a bag whose
// values equal every default for a kind whose defaults are a no-op -- applyAdjustment returns
// before touching a pixel, so inserting a fresh layer composites byte-identically to no layer.
struct ScalarAdjustConsts {
    // Levels
    float inB = 0.0f, outB = 0.0f, outW = 1.0f, invRange = 1.0f, invGamma = 1.0f;
    // Exposure
    float expScale = 1.0f, offset = 0.0f, expInvGamma = 1.0f;
    // Hue/Saturation (hueShift in turns; satScale multiplicative; light in [-1,1])
    float hueShift = 0.0f, satScale = 1.0f, light = 0.0f;
    // Color balance: per band x channel deltas at full slider, pre-scaled to color units
    // ([shadows|midtones|highlights] x [r|g|b]); preserveLum restores the source luminosity.
    std::array<float, 9> cb{};
    bool preserveLum = true;
    // Threshold / Posterize
    float level = 0.5f;
    float postScale = 3.0f;  // levels - 1: quantizer denominator
    // Grayscale (S32 follow-up): the projection method + original<->gray mix + palette size
    int grayMethod = static_cast<int>(core::GrayscaleMethod::Luma);
    float grayMix = 1.0f;
    float grayLattice = 0.0f; // (grays - 1) when quantizing; 0 = continuous (grays == 256)
    // Matte Removal (S34): the chosen mode; the transfer is pure algebra on the pixel's own alpha.
    int matteMode = static_cast<int>(core::MatteMode::RemoveWhite);
    // Haze Removal (S34): the airlight colour and 1/transmission (a CONSTANT, never estimated).
    float airR = 1.0f, airG = 1.0f, airB = 1.0f, invT = 1.0f, hazeSat = 1.0f;
    // Vibrance (S34-a): the chroma-weighted saturation gain, in [-1, 1].
    float vibrance = 0.0f;
    // Photo Filter (S34-a): the filter colour in LINEAR light, the density mix, and whether the
    // backdrop's own luminance is restored after the filter darkens it.
    float pfR = 1.0f, pfG = 1.0f, pfB = 1.0f, pfDensity = 0.0f;
    bool pfPreserveLum = true;
    bool identity = false;
};

[[nodiscard]] ScalarAdjustConsts scalarAdjustConsts(const core::AdjustmentLayer& adj) {
    using enum core::AdjustmentKind;
    const core::AdjustmentKind kind = adj.adjustmentKind();
    const auto v = [&](const char* key) {
        const core::AdjustmentParamDesc* d = core::adjustmentParamDesc(kind, key);
        return d != nullptr ? core::adjustmentParamValue(adj, *d) : 0.0;
    };
    ScalarAdjustConsts c;
    switch (kind) {
        case Levels: {
            const double inB = v("in_black"), inW = v("in_white"), gamma = v("gamma");
            const double outB = v("out_black"), outW = v("out_white");
            c.inB = static_cast<float>(inB);
            c.invRange = static_cast<float>(1.0 / std::max(inW - inB, 1e-4));
            c.invGamma = static_cast<float>(1.0 / gamma);
            c.outB = static_cast<float>(outB);
            c.outW = static_cast<float>(outW);
            c.identity =
                inB == 0.0 && inW == 1.0 && gamma == 1.0 && outB == 0.0 && outW == 1.0;
            break;
        }
        case Exposure: {
            const double ev = v("exposure"), offset = v("offset"), gamma = v("gamma");
            c.expScale = static_cast<float>(std::exp2(ev));
            c.offset = static_cast<float>(offset);
            c.expInvGamma = static_cast<float>(1.0 / gamma);
            c.identity = ev == 0.0 && offset == 0.0 && gamma == 1.0;
            break;
        }
        case HueSaturation: {
            const double hue = v("hue"), sat = v("saturation"), light = v("lightness");
            c.hueShift = static_cast<float>(hue / 360.0);
            c.satScale = static_cast<float>(1.0 + sat / 100.0);
            c.light = static_cast<float>(light / 100.0);
            c.identity = hue == 0.0 && sat == 0.0 && light == 0.0;
            break;
        }
        case ColorBalance: {
            // Full slider (+-100) shifts its band's channel by up to kStrength color units,
            // faded by the band weight -- strong enough to grade with, soft enough to stack.
            constexpr double kStrength = 0.4;
            static constexpr const char* kKeys[9] = {
                "shadows_cr",    "shadows_mg",    "shadows_yb",
                "midtones_cr",   "midtones_mg",   "midtones_yb",
                "highlights_cr", "highlights_mg", "highlights_yb",
            };
            bool allZero = true;
            for (int i = 0; i < 9; ++i) {
                const double s = v(kKeys[i]);
                allZero = allZero && s == 0.0;
                c.cb[static_cast<std::size_t>(i)] =
                    static_cast<float>(s / 100.0 * kStrength);
            }
            c.preserveLum = v("preserve_luminosity") >= 0.5;
            c.identity = allZero;
            break;
        }
        case Threshold: c.level = static_cast<float>(v("level")); break;
        case Posterize:
            c.postScale = static_cast<float>(std::round(v("levels")) - 1.0);
            break;
        case Grayscale: {
            // Absent keys read the schema defaults (Luma at full strength, continuous) --
            // exactly the pre-S32 formula, so an old document's bag stays byte-identical.
            c.grayMethod = static_cast<int>(std::lround(v("method")));
            c.grayMix = static_cast<float>(v("strength") / 100.0);
            const double grays = std::round(v("grays"));
            c.grayLattice = grays <= 255.0 ? static_cast<float>(grays - 1.0) : 0.0f;
            c.identity = c.grayMix == 0.0f; // zero strength changes nothing
            break;
        }
        case MatteRemoval:
            // No identity early-out: every mode does real work wherever the backdrop is partly
            // transparent (an "inherently visible" kind, the Threshold/Posterize class). Over a
            // fully opaque backdrop the transfer is the identity anyway -- there is no matte.
            c.matteMode = static_cast<int>(std::lround(v("mode")));
            break;
        case HazeRemoval: {
            // Koschmieder's 1924 scattering model inverted at a CONSTANT transmission: an
            // Amount of 100% leaves t = 0.1, a strong stretch away from the airlight colour
            // without ever dividing by zero. `tint` swings the airlight warm (+) or blue (-).
            const double amount = v("amount"), air = v("airlight") / 100.0;
            const double tint = v("tint") / 100.0, sat = v("saturation") / 100.0;
            c.invT = static_cast<float>(1.0 / (1.0 - 0.9 * amount / 100.0));
            c.airR = static_cast<float>(std::clamp(air * (1.0 + 0.10 * tint), 0.0, 1.0));
            c.airG = static_cast<float>(air);
            c.airB = static_cast<float>(std::clamp(air * (1.0 - 0.10 * tint), 0.0, 1.0));
            c.hazeSat = static_cast<float>(sat);
            c.identity = amount == 0.0 && sat == 1.0;
            break;
        }
        case Vibrance: {
            // s' = s * (1 + k*(1 - s)): the gain fades to nothing as the pixel approaches full
            // saturation, so vivid colour (and skin, which sits low in chroma but high in the
            // eye's attention) is protected while muted colour moves. Nothing is measured from
            // the image -- k is the user's slider, and that is deliberate.
            const double amount = v("vibrance");
            c.vibrance = static_cast<float>(amount / 100.0);
            c.identity = amount == 0.0;
            break;
        }
        case PhotoFilter: {
            // A coloured filter is an ABSORPTION: it multiplies the light, so the transfer runs
            // in linear light through the compositor's LUT pair (the Exposure/Vignette
            // precedent). `density` mixes the filtered light back over the original.
            const double density = v("density") / 100.0;
            const auto preset = static_cast<core::PhotoFilterPreset>(
                std::lround(v("filter")));
            const common::Color8 col =
                preset == core::PhotoFilterPreset::Custom
                    ? common::Color8{static_cast<std::uint8_t>(std::lround(v("color_r"))),
                                     static_cast<std::uint8_t>(std::lround(v("color_g"))),
                                     static_cast<std::uint8_t>(std::lround(v("color_b"))), 255}
                    : core::photoFilterPresetColor(preset);
            c.pfR = srgbToLinear(static_cast<float>(col.r) / 255.0f);
            c.pfG = srgbToLinear(static_cast<float>(col.g) / 255.0f);
            c.pfB = srgbToLinear(static_cast<float>(col.b) / 255.0f);
            c.pfDensity = static_cast<float>(density);
            c.pfPreserveLum = v("preserve_luminosity") >= 0.5;
            c.identity = density == 0.0; // the EFFECTIVE no-op, not default-equality (§1)
            break;
        }
        default: break;
    }
    return c;
}

// The S34-a Gradient Map constants: ONE 256-entry RGBA lookup built per composite, sampled from
// the stored ramp through core::vec::sampleAt -- the same evaluation the vector rasteriser and
// the layer-effects overlays use, so a ramp looks identical wherever it is drawn (midpoints and
// all). `reverse` is baked into the table rather than flipped per pixel.
struct GradientMapConsts {
    std::array<std::array<float, 4>, 256> lut{};
};

[[nodiscard]] GradientMapConsts gradientMapConsts(const core::AdjustmentLayer& adj) {
    GradientMapConsts c;
    const core::vec::Gradient ramp = core::adjustmentGradientMap(adj);
    // The schema read (the file-wide schemaParam helper is declared further down, with the blur
    // branch it was written for) -- absent falls back to the declared default, present clamps.
    const core::AdjustmentParamDesc* rev =
        core::adjustmentParamDesc(core::AdjustmentKind::GradientMap, "reverse");
    const bool reverse = rev != nullptr && core::adjustmentParamValue(adj, *rev) >= 0.5;
    for (int i = 0; i < 256; ++i) {
        const double t = static_cast<double>(i) / 255.0;
        const common::ColorF s =
            core::vec::sampleAt(ramp, {reverse ? 1.0 - t : t, 0.0}); // Linear + identity: x == t
        c.lut[static_cast<std::size_t>(i)] = {s.r, s.g, s.b, s.a};
    }
    return c;
}

// Sample a gradient-map table at an encoded luma, linearly between the 8-bit lattice points --
// curveSample's twin, four channels wide (the stop ALPHA rides along as the per-tone strength).
[[nodiscard]] std::array<float, 4> gradientSample(const GradientMapConsts& c, float v) noexcept {
    const float t = std::clamp(v, 0.0f, 1.0f) * 255.0f;
    const int i = static_cast<int>(t);
    if (i >= 255) return c.lut[255];
    const auto lo = static_cast<std::size_t>(i);
    const float f = t - static_cast<float>(i);
    std::array<float, 4> out{};
    for (std::size_t k = 0; k < 4; ++k)
        out[k] = c.lut[lo][k] + (c.lut[lo + 1][k] - c.lut[lo][k]) * f;
    return out;
}

// The S34 Curves constants: ONE composed 256-entry lookup per output channel, built once per
// composite instead of evaluating a spline per pixel. The composition order is per-channel curve
// FIRST, then the composite curve on its result (the order every editor with both has used since
// Photoshop 4). `active[k] == false` means channel k's pair is the identity, and the pixel loop
// then passes that channel through VERBATIM -- not through a nominally-identity lookup, because
// a lerp between two float lattice points is not bit-exact and the §1 rule wants byte-identity
// on the channels the user did not touch.
struct CurvesConsts {
    std::array<std::array<float, 256>, 3> lut{};
    std::array<bool, 3> active{false, false, false};
    bool identity = true;
};

[[nodiscard]] CurvesConsts curvesConsts(const core::AdjustmentLayer& adj) {
    CurvesConsts c;
    const core::brush::Curve composite =
        core::adjustmentCurve(adj, core::CurveChannel::Composite);
    const bool compositeId = composite.isIdentity();
    constexpr int kFirst = static_cast<int>(core::CurveChannel::Red);  // Red/Green/Blue are 1..3
    for (int k = 0; k < 3; ++k) {
        const auto ch = static_cast<core::CurveChannel>(kFirst + k);
        const core::brush::Curve per = core::adjustmentCurve(adj, ch);
        if (compositeId && per.isIdentity()) continue;  // active[k] stays false: pass through
        c.active[static_cast<std::size_t>(k)] = true;
        c.identity = false;
        for (int i = 0; i < 256; ++i) {
            const double t = static_cast<double>(i) / 255.0;
            c.lut[static_cast<std::size_t>(k)][static_cast<std::size_t>(i)] =
                static_cast<float>(composite.eval(per.eval(t)));
        }
    }
    return c;
}

// Sample a composed curve LUT at an encoded value, linearly between the 8-bit lattice points.
// Values outside [0,1] clamp in: like Levels, Curves works on the display range by definition
// (its domain IS the unit square the user drew in), so HDR headroom is folded to the endpoints.
[[nodiscard]] float curveSample(const std::array<float, 256>& lut, float v) noexcept {
    const float t = std::clamp(v, 0.0f, 1.0f) * 255.0f;
    const int i = static_cast<int>(t);
    if (i >= 255) return lut[255];
    return lut[static_cast<std::size_t>(i)] +
           (lut[static_cast<std::size_t>(i) + 1] - lut[static_cast<std::size_t>(i)]) *
               (t - static_cast<float>(i));
}

[[nodiscard]] float smoothstepf(float e0, float e1, float x) noexcept {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

[[nodiscard]] float clamp01(float v) noexcept { return std::clamp(v, 0.0f, 1.0f); }

// RGB <-> HSL on [0,1] encoded values (the CSS / Foley-van Dam hexcone model) for the
// HueSaturation adjustment. Round-trips exactly at s=0 (gray stays gray).
struct Hsl {
    float h, s, l;  // h in turns [0,1)
};

[[nodiscard]] Hsl rgbToHsl(detail::Rgb c) noexcept {
    const float mx = std::max({c.r, c.g, c.b});
    const float mn = std::min({c.r, c.g, c.b});
    const float l = 0.5f * (mx + mn);
    if (mx == mn) return {0.0f, 0.0f, l};
    const float d = mx - mn;
    const float s = l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    float h;
    if (mx == c.r)
        h = (c.g - c.b) / d + (c.g < c.b ? 6.0f : 0.0f);
    else if (mx == c.g)
        h = (c.b - c.r) / d + 2.0f;
    else
        h = (c.r - c.g) / d + 4.0f;
    return {h / 6.0f, s, l};
}

[[nodiscard]] float hueToRgb(float p, float q, float t) noexcept {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

[[nodiscard]] detail::Rgb hslToRgb(Hsl v) noexcept {
    if (v.s <= 0.0f) return {v.l, v.l, v.l};
    const float q = v.l < 0.5f ? v.l * (1.0f + v.s) : v.l + v.s - v.l * v.s;
    const float p = 2.0f * v.l - q;
    return {hueToRgb(p, q, v.h + 1.0f / 3.0f), hueToRgb(p, q, v.h),
            hueToRgb(p, q, v.h - 1.0f / 3.0f)};
}

// Rec 709 luminance on LINEAR values -- the S7 side (texture::photometricMatchParams) computes
// mu_log with these weights in linear light, so the transfer must read the same quantity
// (detail::lum is the W3C encoded-space blend luminance, a different animal).
[[nodiscard]] float linearLum(detail::Rgb c) noexcept {
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

// The fused §6.2 transfer on one linear-RGB texel: WB gains -> log-luminance exposure/contrast
// (Reinhard 2001 statistics, luminance only) -> soft highlight shoulder -> vertical gradient ->
// scotopic night mix (Ward Larson 1997 rod luminance, Thompson 2002 tint) -> saturation.
[[nodiscard]] detail::Rgb applyPhotometricMatch(detail::Rgb lin, const PhotometricMatchConsts& c,
                                                float rowFactor) noexcept {
    lin = {lin.r * c.gainR, lin.g * c.gainG, lin.b * c.gainB};
    const float L = std::max(1e-6f, linearLum(lin));
    const float logL = std::log(L);
    const float logL2 = c.mu + c.lnScale + (logL - c.mu) * c.sigmaRatio;
    float f = std::exp(logL2 - logL);
    // Soft highlight shoulder: a Reinhard-style rolloff on the RAISE above max(original, knee),
    // so a brightening grade keeps highlight detail instead of clipping -- and a pixel the grade
    // does not raise passes through EXACTLY (an empty params bag must be a byte-level no-op).
    constexpr float kKnee = 0.7f;
    const float Lh = L * f;
    const float pivot = std::max(L, kKnee);
    if (Lh > pivot) {
        const float head = std::max(1e-4f, 1.0f - pivot);
        const float over = Lh - pivot;
        const float Lc = pivot + head * over / (head + over);
        f *= Lc / Lh;
    }
    const float g = f * rowFactor;
    lin = {lin.r * g, lin.g * g, lin.b * g};
    if (c.rod > 0.0f) {
        // Linear sRGB -> XYZ (IEC 61966-2-1 / Rec 709 primaries, D65).
        const float X = 0.4124f * lin.r + 0.3576f * lin.g + 0.1805f * lin.b;
        const float Y = 0.2126f * lin.r + 0.7152f * lin.g + 0.0722f * lin.b;
        const float Z = 0.0193f * lin.r + 0.1192f * lin.g + 0.9505f * lin.b;
        const float V =
            std::max(0.0f, Y * (1.33f * (1.0f + (Y + Z) / std::max(1e-6f, X)) - 1.68f));
        const float n = V * c.nightGain;
        lin = {std::lerp(lin.r, n * c.nightR, c.rod), std::lerp(lin.g, n * c.nightG, c.rod),
               std::lerp(lin.b, n * c.nightB, c.rod)};
    }
    if (c.saturation != 1.0f) {
        const float Ls = linearLum(lin);
        lin = {Ls + (lin.r - Ls) * c.saturation, Ls + (lin.g - Ls) * c.saturation,
               Ls + (lin.b - Ls) * c.saturation};
    }
    return lin;
}

// ---------------------------------------------------------------------------------------------
// The S33 blur branch (docs/blur-filters.md). Spatial kinds read the backdrop's NEIGHBORHOOD,
// so they cannot ride the per-pixel color loop: the branch blurs a copy of the accumulator with
// the kind's kernel, then blends it back under the same opacity/mask/clip modulation -- in
// premultiplied space, alpha included (a blur moves coverage, it does not recolor it; §2).
// ---------------------------------------------------------------------------------------------

// Defined with the resample helpers below; the blur math needs it early.
[[nodiscard]] double maxAxisScale(const common::Affine2D& t);

// Schema-clamped parameter read (the S32 rule: the blur kinds are schema-honest; only
// BrightnessContrast + PhotometricMatch keep raw reads, for byte-compat).
[[nodiscard]] double schemaParam(const core::AdjustmentLayer& adj, const char* key) {
    const core::AdjustmentParamDesc* d = core::adjustmentParamDesc(adj.adjustmentKind(), key);
    return d != nullptr ? core::adjustmentParamValue(adj, *d) : 0.0;
}

// A layer mask on an adjustment, sampled at PARENT-space point `p` against the parent-space
// rect `domain` the mask grid spans (nearest sample, matching the old buffer-scaled read
// byte-for-byte on every full-composite path). Sampling in parent space instead of buffer
// space is what makes a masked adjustment correct under a REGION composite: the old read
// stretched the whole mask onto the cropped buffer (the S60-a dirty-rect path patched wrong
// pixels under any masked adjustment); now the walk's placement carries the region offset in.
[[nodiscard]] float adjustmentMaskAt(const core::RasterMask& mk, common::Vec2 p,
                                     const common::Rect& domain) {
    if (mk.width == 0 || mk.height == 0 || domain.w <= 0.0 || domain.h <= 0.0) return 1.0f;
    const auto mx = std::clamp<long>(
        static_cast<long>(std::floor((p.x - domain.x) * mk.width / domain.w)), 0,
        static_cast<long>(mk.width) - 1);
    const auto my = std::clamp<long>(
        static_cast<long>(std::floor((p.y - domain.y) * mk.height / domain.h)), 0,
        static_cast<long>(mk.height) - 1);
    return static_cast<float>(mk.coverage[static_cast<std::size_t>(my) * mk.width +
                                          static_cast<std::size_t>(mx)]) /
           255.0f;
}

// The parent-space rect an adjustment layer's mask spans, honoring the layer's OWN transform (S31
// move/crop fix). The mask is a document-window SHEET placed in the adjustment's LOCAL space by
// core::maskPlacement, so its footprint in the walk's (parent) space is the sheet mapped through
// that placement and then the adjustment's transform. Threading it here is what makes a masked
// adjustment's effect RIDE a Move-tool drag or a crop: crop rebases every layer by a
// SetTransformCommand, so without this the mask stayed pinned to the parent origin while the
// backdrop slid under it -- paint landed offset and a crop looked like "the mask moved". A sheet
// captured at the layer's current transform maps back to the document window exactly (placement ==
// transform^-1), so a masked adjustment composites over the pixels it was built from whatever the
// transform, and an untransformed one is byte-for-byte what it always was. Paint (ui/vulkan_canvas
// beginMaskStroke) and selectionFromLayerMask map through core::maskToDocument; this is the same
// map expressed as the axis-aligned domain the per-pixel read stretches the sheet over.
[[nodiscard]] common::Rect adjustmentMaskDomain(const core::AdjustmentLayer& adj,
                                                const core::RasterMask& mk,
                                                const common::Rect& maskDomain) {
    // An UNLINKED sheet is already in parent space, so only a linked one takes the transform --
    // the same split foldUnlinkedMask makes for every other kind.
    const common::Affine2D place = mk.linked
                                       ? adj.transform() * core::maskPlacement(adj, mk)
                                       : core::maskPlacement(adj, mk);
    if (isIdentity(place)) return maskDomain;
    return place.mapBounds(
        common::Rect{0.0, 0.0, static_cast<double>(mk.width), static_cast<double>(mk.height)});
}

// The farthest any corner of `r` sits from `c`: the |pixel - center| bound the spin/zoom reach
// math needs (their tap displacement grows with distance from the center).
[[nodiscard]] double maxCornerDist(const common::Vec2& c, const common::Rect& r) {
    double d = 0.0;
    for (const common::Vec2 p : {common::Vec2{r.x, r.y}, common::Vec2{r.right(), r.y},
                                 common::Vec2{r.x, r.bottom()},
                                 common::Vec2{r.right(), r.bottom()}})
        d = std::max(d, std::hypot(p.x - c.x, p.y - c.y));
    return d;
}

// The per-pixel modulation EVERY adjustment shares: layer opacity x layer mask (sampled in
// parent space, so a cropped region buffer reads the right mask texels) x clip-to-below
// coverage. Hoisted for the S34 whole-buffer passes so they gate exactly the way the per-pixel
// loop and the blur branch do -- a masked-out pixel must stay byte-identical to the backdrop.
struct AdjustmentGate {
    float opacity = 1.0f;
    const core::RasterMask* mask = nullptr;
    common::Rect maskDom{};
    std::optional<common::Affine2D> preInv;
    const std::vector<float>* coverage = nullptr;

    [[nodiscard]] float at(std::uint32_t x, std::uint32_t y, std::size_t idx) const {
        float amt = opacity;
        if (mask != nullptr && preInv)
            amt *= adjustmentMaskAt(
                *mask, preInv->apply({static_cast<double>(x), static_cast<double>(y)}), maskDom);
        if (coverage != nullptr) amt *= (*coverage)[idx];
        return amt;
    }
};

[[nodiscard]] AdjustmentGate adjustmentGate(const core::AdjustmentLayer& adj,
                                            const std::vector<float>* coverage,
                                            const common::Affine2D& pre,
                                            const common::Rect& maskDomain) {
    AdjustmentGate g;
    g.opacity = adj.opacity();
    g.mask = (adj.hasMask() && adj.mask()->enabled) ? adj.mask() : nullptr;
    g.maskDom = g.mask != nullptr ? adjustmentMaskDomain(adj, *g.mask, maskDomain) : maskDomain;
    g.preInv = pre.inverse();
    g.coverage = coverage;
    return g;
}

// The spatial Grayscale methods' fixed defaults (no schema knobs -- keeping them off the Grayscale
// table leaves the editor clean; they apply only to Adaptive threshold). kAdaptiveRadius is the
// local-mean window half-width in application-space px (scaled by the placement at apply time);
// kAdaptiveBias is subtracted from that mean before the compare (~8/255, the OpenCV MEAN_C sense).
constexpr double kAdaptiveRadius = 12.0;
constexpr float kAdaptiveBias = 0.03f;

// The kernel support radius (application-space px) a spatial adjustment reads or spreads: how
// far a correct output pixel's inputs can lie (docs/blur-filters.md §5). `domain` is the rect
// whose output must come out correct, pre-padded by the caller for stacking.
[[nodiscard]] double blurAdjustmentReach(const core::AdjustmentLayer& adj,
                                         const common::Rect& domain) {
    using enum core::AdjustmentKind;
    // S35 (docs/filters-stylize.md §5): the stylize family keeps its own reach table. Forwarding
    // here rather than adding a second walk is what plugs it into compositeRegion and
    // groupLocalExtent for free -- both already ask this function for every spatial kind.
    if (isStylizeKind(adj.adjustmentKind())) return stylizeAdjustmentReach(adj, domain);
    switch (adj.adjustmentKind()) {
        case GaussianBlur: return 1.5 * schemaParam(adj, "radius");  // 3 * (radius/2) sigma
        case BoxBlur: return schemaParam(adj, "radius");
        case MotionBlur: return 0.5 * schemaParam(adj, "distance");
        case RadialBlur: {
            const common::Vec2 c{schemaParam(adj, "center_x"), schemaParam(adj, "center_y")};
            const double md = maxCornerDist(c, domain);
            const double amount = schemaParam(adj, "amount");
            if (static_cast<int>(schemaParam(adj, "mode")) ==
                static_cast<int>(core::RadialBlurMode::Zoom))
                return md * amount / 100.0;
            // Spin taps sit on the pixel's own center circle within +-arc/2: chord bound.
            const double arc = std::min(amount, 100.0) * kPi / 180.0;
            return 2.0 * md * std::sin(arc / 4.0);
        }
        case SurfaceBlur: return 1.5 * schemaParam(adj, "radius");
        case LensBlur: return schemaParam(adj, "radius") + 1.0;
        case DofBlur: return 1.5 * schemaParam(adj, "radius");
        case Grayscale:
            // Adaptive threshold reads a local box window (reach = its half-width); Floyd-
            // Steinberg diffuses across the WHOLE raster and cannot be bounded by a finite reach
            // (its dependency runs up/left to the raster origin -- see applyGrayscaleSpatial), so
            // it reports none: its region caveat is documented, not forbidden.
            return static_cast<int>(std::lround(schemaParam(adj, "method"))) ==
                           static_cast<int>(core::GrayscaleMethod::AdaptiveThreshold)
                       ? kAdaptiveRadius
                       : 0.0;
        case ShadowsHighlights:
            // The local-background mask is a 3-pass box almost-Gaussian of sigma = radius/3
            // (Kovesi 2010), so its support is ~radius; the pad covers the box-width rounding.
            // With both amounts at zero the pass returns before reading anything.
            if (schemaParam(adj, "shadows") <= 0.0 && schemaParam(adj, "highlights") <= 0.0)
                return 0.0;
            return schemaParam(adj, "radius") + 8.0;
        case Defringe: {
            // Only the lateral-CA rescale reads off-pixel, and (like spin/zoom) its displacement
            // grows with distance from the center, so bound it by the domain's farthest corner.
            // |s|/(1-|s|) <= 1.02*|s| over the schema's +-1% range, hence the small headroom.
            const double m = std::max(std::abs(schemaParam(adj, "ca_red")),
                                      std::abs(schemaParam(adj, "ca_blue"))) /
                             100.0;
            if (m <= 0.0) return 0.0;
            const common::Vec2 c{schemaParam(adj, "center_x"), schemaParam(adj, "center_y")};
            return maxCornerDist(c, domain) * m * 1.05 + 1.0;
        }
        default: return 0.0;
    }
}

// Total support radius of every visible spatial adjustment under `group`, in the group's own
// application space. Stacked blurs COMPOUND (each reads the previous one's output), so reaches
// SUM -- a max would under-grow the buffers. Subgroup contributions scale down their transform
// like descendantEffectsReach, with `domain` mapped into each subgroup's local space so the
// center-dependent (spin/zoom) bounds stay meaningful. The one-time diagonal pad conservatively
// covers the growth stacked stages add to each other's output regions (docs/blur-filters.md §5).
[[nodiscard]] double descendantAdjustmentReach(const core::GroupLayer& group,
                                               const common::Rect& domain) {
    const double diag = std::hypot(domain.w, domain.h);
    const common::Rect padded{domain.x - diag, domain.y - diag, domain.w + 2.0 * diag,
                              domain.h + 2.0 * diag};
    double reach = 0.0;
    for (std::size_t i = 0; i < group.childCount(); ++i) {
        const core::Layer& child = group.child(i);
        if (!child.visible() || child.opacity() <= 0.0f) continue;
        if (const auto* adj = child.as<core::AdjustmentLayer>()) {
            if (core::adjustmentIsSpatial(*adj))
                reach += blurAdjustmentReach(*adj, padded);
        } else if (const auto* sub = child.as<core::GroupLayer>()) {
            const std::optional<common::Affine2D> inv = sub->transform().inverse();
            if (!inv) continue;  // singular: the subgroup projects to nothing
            reach += maxAxisScale(sub->transform()) *
                     descendantAdjustmentReach(*sub, inv->mapBounds(padded));
        }
    }
    return reach;
}

// Resolve a spatial adjustment's schema params into a BlurOp with every geometric value mapped
// into BUFFER px through the walk's placement (§4): px sizes scale by the placement's axis
// scale (a 96px scope preview blurs proportionally), centers/lines map through it (a cropped
// region buffer sees the same geometry the full composite does). nullopt = identity params (a
// byte-level no-op, the §1 rule).
[[nodiscard]] std::optional<BlurOp> resolveBlurOp(const core::AdjustmentLayer& adj,
                                                  const common::Affine2D& pre, bool liveDrag) {
    using enum core::AdjustmentKind;
    const double scale = maxAxisScale(pre);
    if (scale <= 0.0) return std::nullopt;
    BlurOp op;
    op.kind = adj.adjustmentKind();
    op.draft = liveDrag;
    switch (op.kind) {
        case GaussianBlur: {
            op.size = static_cast<float>(schemaParam(adj, "radius") * 0.5 * scale);
            if (op.size <= 0.0f) return std::nullopt;
            break;
        }
        case BoxBlur: {
            op.size = static_cast<float>(std::lround(schemaParam(adj, "radius") * scale));
            if (op.size < 1.0f) return std::nullopt;
            break;
        }
        case MotionBlur: {
            const double dist = schemaParam(adj, "distance") * scale;
            if (dist < 1.0) return std::nullopt;
            op.size = static_cast<float>(dist);
            op.angleRad = static_cast<float>(schemaParam(adj, "angle") * kPi / 180.0);
            break;
        }
        case RadialBlur: {
            const double amount = schemaParam(adj, "amount");
            if (amount <= 0.0) return std::nullopt;
            const common::Vec2 c =
                pre.apply({schemaParam(adj, "center_x"), schemaParam(adj, "center_y")});
            op.cx = static_cast<float>(c.x);
            op.cy = static_cast<float>(c.y);
            op.mode = static_cast<int>(schemaParam(adj, "mode"));
            op.amount = op.mode == static_cast<int>(core::RadialBlurMode::Zoom)
                            ? static_cast<float>(amount / 100.0)
                            : static_cast<float>(amount);
            break;
        }
        case SurfaceBlur: {
            op.size = static_cast<float>(schemaParam(adj, "radius") * scale);
            if (op.size < 0.5f) return std::nullopt;
            op.threshold = static_cast<float>(schemaParam(adj, "threshold") / 100.0);
            break;
        }
        case LensBlur: {
            op.size = static_cast<float>(schemaParam(adj, "radius") * scale);
            if (op.size < 0.5f) return std::nullopt;
            op.blades = static_cast<int>(schemaParam(adj, "blades"));
            op.curvature = static_cast<float>(schemaParam(adj, "curvature") / 100.0);
            op.rotationRad = static_cast<float>(schemaParam(adj, "rotation") * kPi / 180.0);
            op.boost = static_cast<float>(schemaParam(adj, "boost") / 100.0);
            op.boostThreshold =
                static_cast<float>(schemaParam(adj, "boost_threshold") / 100.0);
            break;
        }
        case DofBlur: {
            const double r = schemaParam(adj, "radius") * scale;
            if (r < 0.5) return std::nullopt;
            // The ONE focus band (deliberately only one). Center/angle live in parent space; the
            // placement maps them into buffer px (an anisotropic preview scale tilts the
            // mapped direction, so the band normal derives from the MAPPED direction, not the
            // raw angle).
            op.size = static_cast<float>(r);
            const common::Vec2 c =
                pre.apply({schemaParam(adj, "center_x"), schemaParam(adj, "center_y")});
            op.cx = static_cast<float>(c.x);
            op.cy = static_cast<float>(c.y);
            const double a = schemaParam(adj, "angle") * kPi / 180.0;
            common::Vec2 dir = pre.applyVector({std::cos(a), std::sin(a)});
            const double len = std::hypot(dir.x, dir.y);
            dir = len > 1e-12 ? common::Vec2{dir.x / len, dir.y / len}
                              : common::Vec2{1.0, 0.0};
            op.dirX = static_cast<float>(dir.x);
            op.dirY = static_cast<float>(dir.y);
            op.band = static_cast<float>(schemaParam(adj, "band") * scale);
            op.feather = static_cast<float>(std::max(1.0, schemaParam(adj, "feather") * scale));
            op.mode = static_cast<int>(schemaParam(adj, "bokeh"));
            break;
        }
        default: return std::nullopt;
    }
    return op;
}

// The CPU reference dispatch for a resolved BlurOp -- the permanent source of truth the GPU
// override is parity-tested against (compositor.hpp seam contract).
void runBlurCpu(ImageF& img, const BlurOp& op) {
    using enum core::AdjustmentKind;
    switch (op.kind) {
        case GaussianBlur: fx::gaussianBlurImage(img, op.size); break;
        case BoxBlur: fx::boxBlurImage(img, static_cast<int>(op.size)); break;
        case MotionBlur: fx::motionBlurImage(img, op.angleRad, op.size, op.draft); break;
        case RadialBlur:
            if (op.mode == static_cast<int>(core::RadialBlurMode::Zoom))
                fx::zoomBlurImage(img, op.cx, op.cy, op.amount, op.draft);
            else
                fx::spinBlurImage(img, op.cx, op.cy, op.amount, op.draft);
            break;
        case SurfaceBlur: fx::surfaceBlurImage(img, op.size, op.threshold); break;
        case LensBlur: {
            const fx::ApertureKernel k = fx::makeApertureKernel(op.size, op.blades,
                                                                op.curvature, op.rotationRad,
                                                                op.draft);
            fx::lensBlurImage(img, k, op.boost, op.boostThreshold);
            break;
        }
        case DofBlur: {
            // Per-pixel radius = the feather ramp of |signed distance from the focus line|
            // past the band half-width, times the max radius. The FIELD is plain geometry;
            // the render stays level interpolation inside dofBlurImage (never a per-pixel gather).
            const float nx = -op.dirY;
            const float ny = op.dirX;
            std::vector<float> field(img.pixelCount());
            parallelFor(img.height, 64, [&](std::size_t row0, std::size_t row1) {
                for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
                    std::size_t idx = static_cast<std::size_t>(y) * img.width;
                    const float py = static_cast<float>(y) + 0.5f - op.cy;
                    for (std::uint32_t x = 0; x < img.width; ++x, ++idx) {
                        const float px = static_cast<float>(x) + 0.5f - op.cx;
                        const float d = std::abs(px * nx + py * ny);
                        field[idx] =
                            std::clamp((d - op.band) / op.feather, 0.0f, 1.0f) * op.size;
                    }
                }
            });
            fx::dofBlurImage(img, field, op.size,
                             op.mode == static_cast<int>(core::DofBokeh::Iris), op.draft);
            break;
        }
        default: break;
    }
}

// The registered GPU lane, if any (compositor.hpp). File-scope so the walk stays signature-
// stable; the app installs it once at startup, tests never do.
BlurRenderOverride g_blurOverride;
LayerEffectsRenderOverride g_layerEffectsOverride;

// The spatial half of applyAdjustment. `pre` is the walk's placement (parent space -> buffer),
// `maskDomain` the parent-space rect the layer mask spans; `liveDrag` lets the heavy gathers
// draft-subsample taps mid-gesture.
void applyBlurAdjustment(ImageF& acc, const core::AdjustmentLayer& adj,
                         const std::vector<float>* coverage, const common::Affine2D& pre,
                         const common::Rect& maskDomain, bool liveDrag) {
    if (acc.empty()) return;
    const std::optional<BlurOp> blurOp = resolveBlurOp(adj, pre, liveDrag);
    if (!blurOp) return;  // zero-amount params: a byte-level no-op (the §1 identity rule)
    ImageF blurred = acc;  // straight copy; the kernels transform it in place
    if (!(g_blurOverride && g_blurOverride(blurred, *blurOp)))
        runBlurCpu(blurred, *blurOp);

    // Blend the blurred backdrop over the original under opacity * mask * clip coverage, in
    // premultiplied space -- lerping straight RGB across differing alphas is not physical. The
    // amt == 1 fast path keeps the unmodulated case byte-equal to the raw kernel output (the
    // plateau/full-blur test pins rely on it).
    const float op = adj.opacity();
    const core::RasterMask* mk = (adj.hasMask() && adj.mask()->enabled) ? adj.mask() : nullptr;
    const common::Rect maskDom = mk ? adjustmentMaskDomain(adj, *mk, maskDomain) : maskDomain;
    const std::optional<common::Affine2D> preInv = pre.inverse();
    parallelFor(acc.height, 64, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            std::size_t idx = static_cast<std::size_t>(y) * acc.width;
            for (std::uint32_t x = 0; x < acc.width; ++x, ++idx) {
                float amt = op;
                if (mk && preInv) {
                    amt *= adjustmentMaskAt(
                        *mk,
                        preInv->apply({static_cast<double>(x), static_cast<double>(y)}),
                        maskDom);
                }
                if (coverage) amt *= (*coverage)[idx];
                if (amt <= 0.0f) continue;  // masked out: byte-identical backdrop (§2)
                const std::size_t p = idx * 4;
                if (amt >= 1.0f) {
                    acc.rgba[p] = blurred.rgba[p];
                    acc.rgba[p + 1] = blurred.rgba[p + 1];
                    acc.rgba[p + 2] = blurred.rgba[p + 2];
                    acc.rgba[p + 3] = blurred.rgba[p + 3];
                    continue;
                }
                const float ao = acc.rgba[p + 3];
                const float ab = blurred.rgba[p + 3];
                const float aOut = std::lerp(ao, ab, amt);
                if (aOut > 1e-6f) {
                    const float inv = 1.0f / aOut;
                    acc.rgba[p] =
                        std::lerp(acc.rgba[p] * ao, blurred.rgba[p] * ab, amt) * inv;
                    acc.rgba[p + 1] =
                        std::lerp(acc.rgba[p + 1] * ao, blurred.rgba[p + 1] * ab, amt) * inv;
                    acc.rgba[p + 2] =
                        std::lerp(acc.rgba[p + 2] * ao, blurred.rgba[p + 2] * ab, amt) * inv;
                } else {  // invisible either way: keep a plain lerp so the RGB stays finite
                    acc.rgba[p] = std::lerp(acc.rgba[p], blurred.rgba[p], amt);
                    acc.rgba[p + 1] = std::lerp(acc.rgba[p + 1], blurred.rgba[p + 1], amt);
                    acc.rgba[p + 2] = std::lerp(acc.rgba[p + 2], blurred.rgba[p + 2], amt);
                }
                acc.rgba[p + 3] = aOut;
            }
        }
    });
}

// Separable box MEAN of a single-channel plane, window half-width `r`, clamp-to-edge (replicate)
// at the borders -- the blur family's edge policy, so a region buffer's mean matches the full
// composite's at the shared physical edge and region == crop(full) holds byte-exactly (§5). The
// running-sum add/remove uses the same clamped indices, so the O(1)/px sum stays exact under
// replicate (multiple out-of-range taps collapse onto the same edge pixel consistently).
[[nodiscard]] std::vector<float> localBoxMean(const std::vector<float>& src, std::uint32_t W,
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

// The spatial Grayscale methods (Dithered / Adaptive threshold): both decide each output pixel
// from more than the pixel itself, so they run here as a whole-buffer pass instead of the per-
// pixel color loop below. The gray SOURCE is the classic Luma projection (detail::lum), clamped
// to [0,1] -- both methods are 1-bit decisions, so HDR headroom would only saturate. The
// binarized gray is mixed into the original by strength and gated by opacity/mask/clip coverage,
// exactly like the per-pixel Grayscale blend (alpha untouched).
//
// Region behaviour (docs/adjustment-layers.md §2): Adaptive threshold reports a finite window
// reach (blurAdjustmentReach) and reads with clamp-to-edge, so a dirty-rect recomposite stays
// byte-identical to the full composite -- region == crop(full), like the blur family. Floyd-
// Steinberg does NOT: its error path runs from the raster's top-left across the whole image, an
// unbounded up/left dependency the finite-reach machinery cannot express, so it reports zero
// reach and diffuses over whatever contiguous buffer it is handed. The FULL composite (the
// saved/exported image and every ordinary redraw) is always the exact FS result; only an
// incremental sub-region repaint under a dither layer re-seeds its diffusion within that rect.
void applyGrayscaleSpatial(ImageF& acc, const core::AdjustmentLayer& adj,
                           const std::vector<float>* coverage, const common::Affine2D& pre,
                           const common::Rect& maskDomain) {
    if (acc.empty()) return;
    const ScalarAdjustConsts sc = scalarAdjustConsts(adj);
    if (sc.grayMix <= 0.0f) return;  // zero strength: a byte-level no-op (the §1 identity rule)
    const std::uint32_t W = acc.width;
    const std::uint32_t H = acc.height;
    const std::size_t n = acc.pixelCount();

    // The gray plane: the classic Luma projection, clamped to the display range.
    std::vector<float> g(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t p = i * 4;
        g[i] = clamp01(detail::lum({acc.rgba[p], acc.rgba[p + 1], acc.rgba[p + 2]}));
    }

    if (static_cast<core::GrayscaleMethod>(sc.grayMethod) == core::GrayscaleMethod::Dithered) {
        // 1-bit Floyd-Steinberg error diffusion, standard kernel (7/16 right, 3/16 down-left,
        // 5/16 down, 1/16 down-right), left-to-right / top-to-bottom. SERIAL by construction --
        // the error at (x,y) feeds its right/below neighbours, so this cannot be row-parallel.
        // Error pushed past the right/bottom edge is dropped (the textbook FS boundary).
        for (std::uint32_t y = 0; y < H; ++y) {
            const std::size_t row = static_cast<std::size_t>(y) * W;
            const std::size_t below = row + W;
            for (std::uint32_t x = 0; x < W; ++x) {
                const std::size_t i = row + x;
                const float oldv = g[i];
                const float newv = oldv >= 0.5f ? 1.0f : 0.0f;
                const float err = oldv - newv;
                g[i] = newv;
                if (x + 1 < W) g[i + 1] += err * (7.0f / 16.0f);
                if (y + 1 < H) {
                    if (x > 0) g[below + x - 1] += err * (3.0f / 16.0f);
                    g[below + x] += err * (5.0f / 16.0f);
                    if (x + 1 < W) g[below + x + 1] += err * (1.0f / 16.0f);
                }
            }
        }
    } else {  // AdaptiveThreshold: binarize against the local-window mean minus a small bias.
        const int r = std::max(
            1, static_cast<int>(std::lround(kAdaptiveRadius * maxAxisScale(pre))));
        const std::vector<float> mean = localBoxMean(g, W, H, r);
        for (std::size_t i = 0; i < n; ++i)
            g[i] = g[i] >= mean[i] - kAdaptiveBias ? 1.0f : 0.0f;
    }

    // Blend the binarized gray into the original under strength * opacity * mask * clip coverage.
    const float op = adj.opacity();
    const core::RasterMask* mk = (adj.hasMask() && adj.mask()->enabled) ? adj.mask() : nullptr;
    const common::Rect maskDom = mk ? adjustmentMaskDomain(adj, *mk, maskDomain) : maskDomain;
    const std::optional<common::Affine2D> preInv = pre.inverse();
    parallelFor(H, 64, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            std::size_t idx = static_cast<std::size_t>(y) * W;
            for (std::uint32_t x = 0; x < W; ++x, ++idx) {
                float amt = op;
                if (mk && preInv) {
                    amt *= adjustmentMaskAt(
                        *mk,
                        preInv->apply({static_cast<double>(x), static_cast<double>(y)}),
                        maskDom);
                }
                if (coverage) amt *= (*coverage)[idx];
                if (amt <= 0.0f) continue;  // masked out: byte-identical backdrop (§2)
                const std::size_t p = idx * 4;
                const float gv = g[idx];
                const detail::Rgb c{acc.rgba[p], acc.rgba[p + 1], acc.rgba[p + 2]};
                const detail::Rgb adjusted{std::lerp(c.r, gv, sc.grayMix),
                                           std::lerp(c.g, gv, sc.grayMix),
                                           std::lerp(c.b, gv, sc.grayMix)};
                acc.rgba[p] = std::lerp(c.r, adjusted.r, amt);
                acc.rgba[p + 1] = std::lerp(c.g, adjusted.g, amt);
                acc.rgba[p + 2] = std::lerp(c.b, adjusted.b, amt);
                // Alpha untouched: an adjustment recolors the backdrop, it adds no coverage.
            }
        }
    });
}

// ---------------------------------------------------------------------------------------------
// The S34 spatial colour repairs (docs/adjustment-layers.md §2.2-§2.3).
// ---------------------------------------------------------------------------------------------

// Shadows/Highlights: LOCAL tone recovery. The classical non-linear masking form -- the mask is
// an inverted, low-pass, monochrome copy of the image and it drives a per-pixel EXPONENT
// (N. Moroney, "Local Color Correction Using Non-Linear Masking", IS&T/SID CIC 8, 2000). Reading
// the BACKGROUND luminance is exactly what separates this from a global gamma: the same mid-dark
// pixel is lifted inside a shadow and left alone against a bright sky.
//
// Region behaviour: the mask blur is a 3-pass box almost-Gaussian with clamp-to-edge reads and
// blurAdjustmentReach reports its support, so a dirty-rect recomposite stays byte-identical to
// the full composite (region == crop(full), the blur family's invariant).
void applyShadowsHighlights(ImageF& acc, const core::AdjustmentLayer& adj,
                            const std::vector<float>* coverage, const common::Affine2D& pre,
                            const common::Rect& maskDomain) {
    if (acc.empty()) return;
    const auto aS = static_cast<float>(schemaParam(adj, "shadows") / 100.0);
    const auto aH = static_cast<float>(schemaParam(adj, "highlights") / 100.0);
    if (aS <= 0.0f && aH <= 0.0f) return;  // a defaults bag is a byte-level no-op (the §1 rule)
    const auto tS = static_cast<float>(schemaParam(adj, "shadows_tone") / 100.0);
    const auto tH = static_cast<float>(schemaParam(adj, "highlights_tone") / 100.0);
    const std::uint32_t W = acc.width;
    const std::uint32_t H = acc.height;
    const std::size_t n = acc.pixelCount();

    // The mask plane: the backdrop's encoded luma, low-passed. Built at the buffer's own
    // resolution with the placement's axis scale applied to the radius, so a 96px scope preview
    // masks proportionally instead of at full-canvas strength (the S33 §4 rule).
    std::vector<float> mask(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t p = i * 4;
        mask[i] = clamp01(detail::lum({acc.rgba[p], acc.rgba[p + 1], acc.rgba[p + 2]}));
    }
    fx::boxBlurApprox(mask, static_cast<int>(W), static_cast<int>(H),
                      static_cast<float>(schemaParam(adj, "radius") * maxAxisScale(pre) / 3.0));

    // At a full slider with the mask fully in the band, the exponent reaches 1/3 (a strong lift)
    // or 3 (a strong recovery) -- enough authority to rescue a backlit frame, gentle in the
    // middle of the slider's travel.
    constexpr float kAuthority = 2.0f;
    const AdjustmentGate gate = adjustmentGate(adj, coverage, pre, maskDomain);
    parallelFor(H, 64, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            std::size_t idx = static_cast<std::size_t>(y) * W;
            for (std::uint32_t x = 0; x < W; ++x, ++idx) {
                const float amt = gate.at(x, y, idx);
                if (amt <= 0.0f) continue;  // masked out: byte-identical backdrop
                const float m = mask[idx];
                const float ws = aS > 0.0f ? 1.0f - smoothstepf(0.0f, tS, m) : 0.0f;
                const float wh = aH > 0.0f ? smoothstepf(1.0f - tH, 1.0f, m) : 0.0f;
                const float e = (1.0f + kAuthority * aH * wh) / (1.0f + kAuthority * aS * ws);
                if (e == 1.0f) continue;  // the mask says "not here": byte-identical, no pow()
                const std::size_t p = idx * 4;
                for (std::size_t k = 0; k < 3; ++k) {
                    const float v = acc.rgba[p + k];
                    acc.rgba[p + k] = std::lerp(v, std::pow(clamp01(v), e), amt);
                }
                // Alpha untouched: an adjustment recolors the backdrop, it adds no coverage.
            }
        }
    });
}

// Defringe: two independent repairs for the same complaint, neither of which looks at edges.
//
//   1. LATERAL chromatic aberration -- a radial rescale of the red and blue channels about the
//      optical center (Boult & Wolberg, "Correcting chromatic aberrations using image warping",
//      CVPR 1992; Panorama Tools shipped the same manual per-channel radial scale in 1998). The
//      factors are the USER's: nothing here estimates them from the image, ever.
//   2. AXIAL fringing -- the purple/green halo is a HUE, so it is suppressed as one: pixels whose
//      hue falls in the purple or green band and whose chroma clears a threshold are desaturated
//      toward their own lightness. There is deliberately NO edge term of any kind.
//
// Both read/write the encoded working space and never touch alpha.
void applyDefringe(ImageF& acc, const core::AdjustmentLayer& adj,
                   const std::vector<float>* coverage, const common::Affine2D& pre,
                   const common::Rect& maskDomain) {
    if (acc.empty()) return;
    const auto purple = static_cast<float>(schemaParam(adj, "purple") / 100.0);
    const auto green = static_cast<float>(schemaParam(adj, "green") / 100.0);
    const double sR = schemaParam(adj, "ca_red") / 100.0;
    const double sB = schemaParam(adj, "ca_blue") / 100.0;
    if (purple <= 0.0f && green <= 0.0f && sR == 0.0 && sB == 0.0) return;  // no-op (the §1 rule)
    // Clamped so the chroma gate's ramp is never degenerate (its upper edge is thr + 0.15).
    const float thr =
        std::clamp(static_cast<float>(schemaParam(adj, "threshold") / 100.0), 0.0f, 0.85f);
    const std::uint32_t W = acc.width;
    const std::uint32_t H = acc.height;
    const std::size_t n = acc.pixelCount();

    // ---- 1. The lateral-CA rescale, into two side planes (green is the reference and never
    // moves). Bilinear taps read PREMULTIPLIED colour and divide by the interpolated alpha, so a
    // transparent neighbour cannot bleed its undefined RGB in; edges clamp (replicate), which is
    // what keeps region == crop(full) exact.
    std::vector<float> rPlane;
    std::vector<float> bPlane;
    if (sR != 0.0 || sB != 0.0) {
        const common::Vec2 ctr =
            pre.apply({schemaParam(adj, "center_x"), schemaParam(adj, "center_y")});
        const auto sample = [&](std::size_t ch, double px, double py, float fallback) {
            const double fx = std::clamp(px, 0.0, static_cast<double>(W) - 1.0);
            const double fy = std::clamp(py, 0.0, static_cast<double>(H) - 1.0);
            const auto x0 = static_cast<std::uint32_t>(std::floor(fx));
            const auto y0 = static_cast<std::uint32_t>(std::floor(fy));
            const std::uint32_t x1 = std::min(x0 + 1, W - 1);
            const std::uint32_t y1 = std::min(y0 + 1, H - 1);
            const auto tx = static_cast<float>(fx - static_cast<double>(x0));
            const auto ty = static_cast<float>(fy - static_cast<double>(y0));
            const std::uint32_t xs[2] = {x0, x1};
            const std::uint32_t ys[2] = {y0, y1};
            const float wx[2] = {1.0f - tx, tx};
            const float wy[2] = {1.0f - ty, ty};
            float cAcc = 0.0f;
            float aAcc = 0.0f;
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    const std::size_t p =
                        (static_cast<std::size_t>(ys[j]) * W + xs[i]) * 4;
                    const float w = wx[i] * wy[j];
                    const float a = acc.rgba[p + 3];
                    cAcc += w * acc.rgba[p + ch] * a;
                    aAcc += w * a;
                }
            }
            return aAcc > 1e-6f ? cAcc / aAcc : fallback;
        };
        rPlane.resize(n);
        bPlane.resize(n);
        const double invR = 1.0 / (1.0 + sR);
        const double invB = 1.0 / (1.0 + sB);
        parallelFor(H, 64, [&](std::size_t row0, std::size_t row1) {
            for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
                std::size_t idx = static_cast<std::size_t>(y) * W;
                for (std::uint32_t x = 0; x < W; ++x, ++idx) {
                    const std::size_t p = idx * 4;
                    const double dx = static_cast<double>(x) - ctr.x;
                    const double dy = static_cast<double>(y) - ctr.y;
                    rPlane[idx] = sR != 0.0
                                      ? sample(0, ctr.x + dx * invR, ctr.y + dy * invR, acc.rgba[p])
                                      : acc.rgba[p];
                    bPlane[idx] =
                        sB != 0.0 ? sample(2, ctr.x + dx * invB, ctr.y + dy * invB, acc.rgba[p + 2])
                                  : acc.rgba[p + 2];
                }
            }
        });
    }

    // ---- 2. Hue-band chroma suppression + the blend.
    constexpr float kPurpleHue = 0.79f;          // ~285 deg: the violet-magenta axial fringe
    constexpr float kGreenHue = 1.0f / 3.0f;     // 120 deg: its complementary partner
    constexpr float kBandHalf = 0.11f;           // ~40 deg either side of a band center
    const bool suppress = purple > 0.0f || green > 0.0f;
    const bool haveCa = !rPlane.empty();
    const AdjustmentGate gate = adjustmentGate(adj, coverage, pre, maskDomain);
    parallelFor(H, 64, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            std::size_t idx = static_cast<std::size_t>(y) * W;
            for (std::uint32_t x = 0; x < W; ++x, ++idx) {
                const float amt = gate.at(x, y, idx);
                if (amt <= 0.0f) continue;  // masked out: byte-identical backdrop
                const std::size_t p = idx * 4;
                const detail::Rgb c{acc.rgba[p], acc.rgba[p + 1], acc.rgba[p + 2]};
                detail::Rgb o = haveCa ? detail::Rgb{rPlane[idx], c.g, bPlane[idx]} : c;
                if (suppress) {
                    Hsl v = rgbToHsl({clamp01(o.r), clamp01(o.g), clamp01(o.b)});
                    const auto band = [&](float center) {
                        float d = std::abs(v.h - center);
                        if (d > 0.5f) d = 1.0f - d;  // hue is circular
                        return 1.0f - smoothstepf(0.0f, kBandHalf, d);
                    };
                    const float chroma = smoothstepf(thr, thr + 0.15f, v.s);
                    const float w =
                        std::min(1.0f, purple * band(kPurpleHue) + green * band(kGreenHue)) *
                        chroma;
                    if (w > 0.0f) {  // w == 0 skips the HSL round trip, so it stays byte-exact
                        v.s = clamp01(v.s * (1.0f - w));
                        o = hslToRgb(v);
                    }
                }
                acc.rgba[p] = std::lerp(c.r, o.r, amt);
                acc.rgba[p + 1] = std::lerp(c.g, o.g, amt);
                acc.rgba[p + 2] = std::lerp(c.b, o.b, amt);
                // Alpha untouched: an adjustment recolors the backdrop, it adds no coverage.
            }
        }
    });
}

// Apply an adjustment layer to the accumulated backdrop (the layers already composited below
// it). `coverage`, when non-null, restricts the effect to the clip base's alpha (clip-to-below).
// `pre` (the walk's placement) + `maskDomain` place the layer mask in parent space (region-
// correct) and serve the S33 spatial branch above; `liveDrag` drafts the heavy kernels. EVERY
// kind has real math since S34 closed Curves.
void applyAdjustment(ImageF& acc, const core::AdjustmentLayer& adj,
                     const std::vector<float>* coverage, const common::Affine2D& pre,
                     const common::Rect& maskDomain, bool liveDrag) {
    using enum core::AdjustmentKind;
    const core::AdjustmentKind kind = adj.adjustmentKind();
    // S35: the stylize family owns its kernels AND its own opacity/mask/clip blend
    // (render/stylize.hpp), so it branches ahead of the scalar/spatial split -- including the two
    // per-pixel kinds (Add Noise, Vignette), which are position-dependent and so cannot ride the
    // geometry-free colour loop below.
    if (isStylizeKind(kind)) {
        applyStylizeAdjustment(acc, adj, coverage, pre, maskDomain, liveDrag);
        return;
    }
    if (core::adjustmentIsSpatial(adj)) {
        // The blur family is a whole-buffer kernel; Grayscale's Dithered/Adaptive methods are a
        // whole-buffer pass over the grayscaled backdrop; S34's Shadows/Highlights reads a
        // blurred mask and Defringe resamples two channels -- all read past the single pixel.
        if (kind == Grayscale)
            applyGrayscaleSpatial(acc, adj, coverage, pre, maskDomain);
        else if (kind == ShadowsHighlights)
            applyShadowsHighlights(acc, adj, coverage, pre, maskDomain);
        else if (kind == Defringe)
            applyDefringe(acc, adj, coverage, pre, maskDomain);
        else
            applyBlurAdjustment(acc, adj, coverage, pre, maskDomain, liveDrag);
        return;
    }
    const float op = adj.opacity();
    const core::RasterMask* mk =
        (adj.hasMask() && adj.mask()->enabled) ? adj.mask() : nullptr;
    const common::Rect maskDom = mk ? adjustmentMaskDomain(adj, *mk, maskDomain) : maskDomain;
    const std::optional<common::Affine2D> preInv = pre.inverse();
    const float brightness = static_cast<float>(param(adj, "brightness", 0.0));
    const float contrast = static_cast<float>(param(adj, "contrast", 0.0));
    const PhotometricMatchConsts pm =
        kind == PhotometricMatch ? photometricMatchConsts(adj) : PhotometricMatchConsts{};
    const ScalarAdjustConsts sc = scalarAdjustConsts(adj);
    // Curves builds its own consts (four splines composed into three 256-entry LUTs) only when
    // it is the kind in hand; a bag with no knots at all is the identity and returns here, so a
    // freshly inserted Curves layer composites byte-identically to no layer (the §1 rule).
    const CurvesConsts cv = kind == Curves ? curvesConsts(adj) : CurvesConsts{};
    if (kind == Curves && cv.identity) return;
    // Gradient Map builds its own 256-entry ramp table, again only when it is the kind in hand.
    // There is deliberately NO identity early-out for it: a gradient map has no identity ramp
    // (docs/adjustment-layers.md §2.6), the Threshold/Posterize/Matte Removal class.
    const GradientMapConsts gm = kind == GradientMap ? gradientMapConsts(adj) : GradientMapConsts{};
    if (sc.identity) return;     // a defaults bag is a byte-level no-op
    const float invH = acc.height > 0 ? 1.0f / static_cast<float>(acc.height) : 0.0f;

    parallelFor(acc.height, 64, [&](std::size_t row0, std::size_t row1) {
    for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
        std::size_t idx = static_cast<std::size_t>(y) * acc.width;
        for (std::uint32_t x = 0; x < acc.width; ++x, ++idx) {
            const std::size_t p = idx * 4;
            detail::Rgb c{acc.rgba[p], acc.rgba[p + 1], acc.rgba[p + 2]};
            detail::Rgb adjusted = c;
            switch (kind) {
                case Invert:
                    adjusted = {1.0f - c.r, 1.0f - c.g, 1.0f - c.b};
                    break;
                case Grayscale: {
                    // The chosen projection, optionally quantized onto the gray palette, mixed
                    // into the original by strength. Luma is the pre-S32 formula on raw
                    // (possibly HDR) encoded values -- kept unclamped so the default bag stays
                    // byte-identical to the old behavior.
                    using enum core::GrayscaleMethod;
                    float g = 0.0f;
                    switch (static_cast<core::GrayscaleMethod>(sc.grayMethod)) {
                        case NoChrominance: { // zero the chroma, keep the photometric luminance
                            const float Y = 0.2126f * srgbToLinear(c.r) +
                                            0.7152f * srgbToLinear(c.g) +
                                            0.0722f * srgbToLinear(c.b);
                            g = linearToSrgb(Y);
                            break;
                        }
                        case Luma: g = detail::lum(c); break;
                        case Red: g = c.r; break;
                        case Green: g = c.g; break;
                        case Blue: g = c.b; break;
                        case MaxChannel: g = std::max({c.r, c.g, c.b}); break;
                        case MinChannel: g = std::min({c.r, c.g, c.b}); break;
                        case Dithered:
                        case AdaptiveThreshold:
                            // Spatial: routed to applyGrayscaleSpatial before this loop, so these
                            // are unreachable here. Fall back to Luma to keep the switch total.
                            g = detail::lum(c);
                            break;
                    }
                    if (sc.grayLattice > 0.0f) // "how do I show this with N grays?"
                        g = std::round(clamp01(g) * sc.grayLattice) / sc.grayLattice;
                    adjusted = {std::lerp(c.r, g, sc.grayMix), std::lerp(c.g, g, sc.grayMix),
                                std::lerp(c.b, g, sc.grayMix)};
                    break;
                }
                case BrightnessContrast: {
                    const auto f = [&](float v) {
                        return std::clamp((v - 0.5f) * (1.0f + contrast) + 0.5f + brightness, 0.0f,
                                          1.0f);
                    };
                    adjusted = {f(c.r), f(c.g), f(c.b)};
                    break;
                }
                case PhotometricMatch: {
                    // The S55 harmonization grade: physical math, so decode to linear through
                    // the LUT pair, run the fused transfer, encode back into the working space.
                    const detail::Rgb lin = applyPhotometricMatch(
                        {srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b)}, pm,
                        1.0f + pm.gradient * (1.0f - 2.0f * static_cast<float>(y) * invH));
                    adjusted = {std::clamp(linearToSrgb(lin.r), 0.0f, 1.0f),
                                std::clamp(linearToSrgb(lin.g), 0.0f, 1.0f),
                                std::clamp(linearToSrgb(lin.b), 0.0f, 1.0f)};
                    break;
                }
                case Levels: {
                    // Classic input-range remap + gamma + output-range remap, per channel in
                    // the encoded working space. Levels inherently clamps its input window.
                    const auto f = [&](float v) {
                        const float t = clamp01((v - sc.inB) * sc.invRange);
                        return sc.outB + (sc.outW - sc.outB) * std::pow(t, sc.invGamma);
                    };
                    adjusted = {f(c.r), f(c.g), f(c.b)};
                    break;
                }
                case Exposure: {
                    // Photographic exposure works in linear light (the LUT pair): scale by
                    // 2^EV, add the offset, gamma-correct, encode back.
                    const auto f = [&](float v) {
                        float lin = srgbToLinear(v) * sc.expScale + sc.offset;
                        lin = std::max(lin, 0.0f);
                        if (sc.expInvGamma != 1.0f) lin = std::pow(lin, sc.expInvGamma);
                        return clamp01(linearToSrgb(lin));
                    };
                    adjusted = {f(c.r), f(c.g), f(c.b)};
                    break;
                }
                case HueSaturation: {
                    // Hexcone HSL: rotate hue, scale saturation, then Photoshop-style
                    // lightness (positive fades toward white, negative toward black).
                    Hsl v = rgbToHsl({clamp01(c.r), clamp01(c.g), clamp01(c.b)});
                    v.h += sc.hueShift;
                    v.h -= std::floor(v.h);
                    v.s = clamp01(v.s * sc.satScale);
                    detail::Rgb o = hslToRgb(v);
                    if (sc.light > 0.0f) {
                        o = {o.r + (1.0f - o.r) * sc.light, o.g + (1.0f - o.g) * sc.light,
                             o.b + (1.0f - o.b) * sc.light};
                    } else if (sc.light < 0.0f) {
                        const float m = 1.0f + sc.light;
                        o = {o.r * m, o.g * m, o.b * m};
                    }
                    adjusted = o;
                    break;
                }
                case ColorBalance: {
                    // Smooth partition of unity over the tonal axis (shadows fade out by
                    // mid-gray, highlights fade in from it, midtones take the rest); each
                    // band shifts its channels by the pre-scaled slider deltas.
                    const detail::Rgb in{clamp01(c.r), clamp01(c.g), clamp01(c.b)};
                    const float l = detail::lum(in);
                    const float ws = 1.0f - smoothstepf(0.0f, 0.5f, l);
                    const float wh = smoothstepf(0.5f, 1.0f, l);
                    const float wm = 1.0f - ws - wh;
                    detail::Rgb o{
                        clamp01(in.r + sc.cb[0] * ws + sc.cb[3] * wm + sc.cb[6] * wh),
                        clamp01(in.g + sc.cb[1] * ws + sc.cb[4] * wm + sc.cb[7] * wh),
                        clamp01(in.b + sc.cb[2] * ws + sc.cb[5] * wm + sc.cb[8] * wh)};
                    if (sc.preserveLum) o = detail::setLum(o, l);
                    adjusted = o;
                    break;
                }
                case Threshold: {
                    const float l = detail::lum({clamp01(c.r), clamp01(c.g), clamp01(c.b)});
                    const float v = l >= sc.level ? 1.0f : 0.0f;
                    adjusted = {v, v, v};
                    break;
                }
                case Posterize: {
                    const auto f = [&](float v) {
                        return std::round(clamp01(v) * sc.postScale) / sc.postScale;
                    };
                    adjusted = {f(c.r), f(c.g), f(c.b)};
                    break;
                }
                case Curves: {
                    // Three composed lookups, built once per composite. An untouched channel is
                    // passed through VERBATIM rather than through a nominally-identity lookup,
                    // so editing only the red curve leaves green and blue byte-identical.
                    adjusted = {cv.active[0] ? curveSample(cv.lut[0], c.r) : c.r,
                                cv.active[1] ? curveSample(cv.lut[1], c.g) : c.g,
                                cv.active[2] ? curveSample(cv.lut[2], c.b) : c.b};
                    break;
                }
                case MatteRemoval: {
                    // Pure compositing algebra (Porter & Duff 1984; Smith & Blinn 1996): recover
                    // the unmatted colour of a composite over a KNOWN matte, or move between
                    // straight and premultiplied storage. Alpha is never touched -- an
                    // adjustment recolors coverage, it does not change it -- so "premultiply"
                    // here means "bake the coverage into the colour", which is exactly the fix
                    // for a layer that was stored the other way round.
                    const float a = acc.rgba[p + 3];
                    const float inv = a > 0.0f ? 1.0f / a : 0.0f;
                    switch (static_cast<core::MatteMode>(sc.matteMode)) {
                        case core::MatteMode::RemoveWhite:
                            if (a <= 1e-4f) break;  // no colour survives to be recovered
                            adjusted = {clamp01((c.r - (1.0f - a)) * inv),
                                        clamp01((c.g - (1.0f - a)) * inv),
                                        clamp01((c.b - (1.0f - a)) * inv)};
                            break;
                        case core::MatteMode::RemoveBlack:
                        case core::MatteMode::Unpremultiply:  // the same algebra, both names
                            if (a <= 1e-4f) break;
                            adjusted = {clamp01(c.r * inv), clamp01(c.g * inv),
                                        clamp01(c.b * inv)};
                            break;
                        case core::MatteMode::Premultiply:
                            adjusted = {c.r * a, c.g * a, c.b * a};
                            break;
                    }
                    break;
                }
                case HazeRemoval: {
                    // Koschmieder's 1924 atmospheric-scattering model inverted about the
                    // airlight colour, at a CONSTANT transmission: J = A + (I - A)/t. There is
                    // deliberately no per-pixel transmission estimate of any kind.
                    detail::Rgb o{clamp01(sc.airR + (c.r - sc.airR) * sc.invT),
                                  clamp01(sc.airG + (c.g - sc.airG) * sc.invT),
                                  clamp01(sc.airB + (c.b - sc.airB) * sc.invT)};
                    if (sc.hazeSat != 1.0f) {  // restore the chroma the atmosphere washed out
                        const float l = detail::lum(o);
                        o = {clamp01(l + (o.r - l) * sc.hazeSat),
                             clamp01(l + (o.g - l) * sc.hazeSat),
                             clamp01(l + (o.b - l) * sc.hazeSat)};
                    }
                    adjusted = o;
                    break;
                }
                case GradientMap: {
                    // The backdrop's LUMA indexes the user's ramp and the sampled colour replaces
                    // the pixel (Photoshop 4, 1996). The stop ALPHA is the per-tone STRENGTH: a
                    // stop at a = 0 leaves its tone exactly as it found it, so one ramp can grade
                    // the shadows and let the highlights through. The pixel's OWN alpha is never
                    // written -- an adjustment recolours the backdrop, it adds no coverage.
                    const float l = clamp01(detail::lum({clamp01(c.r), clamp01(c.g),
                                                         clamp01(c.b)}));
                    const std::array<float, 4> s = gradientSample(gm, l);
                    adjusted = {std::lerp(c.r, s[0], s[3]), std::lerp(c.g, s[1], s[3]),
                                std::lerp(c.b, s[2], s[3])};
                    break;
                }
                case Vibrance: {
                    // Saturation weighted by how saturated the pixel ALREADY is: the gain fades
                    // to nothing at full chroma, so muted colour moves and vivid colour is left
                    // alone. A neutral pixel has s == 0 and so is byte-identical without needing
                    // a special case -- s' == s makes the whole HSL round trip skippable.
                    Hsl v = rgbToHsl({clamp01(c.r), clamp01(c.g), clamp01(c.b)});
                    const float s2 = clamp01(v.s * (1.0f + sc.vibrance * (1.0f - v.s)));
                    if (s2 == v.s) break; // nothing to move: keep the backdrop byte-for-byte
                    v.s = s2;
                    adjusted = hslToRgb(v);
                    break;
                }
                case PhotoFilter: {
                    // A gel over the lens ABSORBS: multiply the linear-light signal by the filter
                    // colour, mix by density, and (optionally) put the backdrop's own luminance
                    // back -- a dense filter otherwise just darkens the frame. Nothing here is
                    // estimated from the image.
                    const detail::Rgb lin{srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b)};
                    detail::Rgb o{std::lerp(lin.r, lin.r * sc.pfR, sc.pfDensity),
                                  std::lerp(lin.g, lin.g * sc.pfG, sc.pfDensity),
                                  std::lerp(lin.b, lin.b * sc.pfB, sc.pfDensity)};
                    if (sc.pfPreserveLum) {
                        const float srcY = linearLum(lin);
                        const float outY = linearLum(o);
                        // A black pixel has no luminance to restore, and scaling by 1/0 is not a
                        // colour: leave it exactly where the filter put it.
                        if (outY > 1e-6f && srcY > 0.0f) {
                            const float k = srcY / outY;
                            o = {o.r * k, o.g * k, o.b * k};
                        }
                    }
                    adjusted = {clamp01(linearToSrgb(o.r)), clamp01(linearToSrgb(o.g)),
                                clamp01(linearToSrgb(o.b))};
                    break;
                }
                default:
                    // The spatial kinds (the S33 blur family, Grayscale's two spatial methods,
                    // S34's Shadows/Highlights and Defringe, S34-a's High Pass) are unreachable
                    // here -- they return through the spatial branch above before this loop.
                    break;
            }

            float amt = op;
            if (mk && preInv) {
                amt *= adjustmentMaskAt(
                    *mk, preInv->apply({static_cast<double>(x), static_cast<double>(y)}),
                    maskDom);
            }
            if (coverage) amt *= (*coverage)[idx];

            acc.rgba[p] = std::lerp(c.r, adjusted.r, amt);
            acc.rgba[p + 1] = std::lerp(c.g, adjusted.g, amt);
            acc.rgba[p + 2] = std::lerp(c.b, adjusted.b, amt);
            // Alpha is untouched: an adjustment recolors the backdrop, it does not add coverage.
        }
    }
    });
}

// The resample settings threaded through the walk: the user's filter choice (Auto resolves per
// layer transform) and whether a transform gesture is in flight (Auto -> cheap Bilinear).
struct ResampleCtx {
    ResampleFilter filter = ResampleFilter::Nearest;
    bool liveDrag = false;
    // Reconstruct coverage partitions (core::CoveragePartition) while walking a STACK: the lower
    // half of a live partition gets its alpha rewritten so the fragment composited above it
    // reassembles the original surface instead of leaving the `over` seam. Off for isolated
    // renders -- a thumbnail, Rasterize or Merge Down of the hole alone must show the true soft
    // hole the layer actually stores, not the shape it takes on while its fragment is overhead.
    bool reconstructPartitions = false;
    // CompositeOptions::skipLayer, threaded down the walk: the one layer this composite leaves out,
    // at whatever depth it sits. Read-only -- the document is never touched to express it.
    core::LayerId skip = core::kInvalidLayerId;
};

// Forward declaration: rendering a layer may recurse into a group's children. `pre` maps the
// layer's PARENT-local coordinates to the W x H target buffer (identity at the document root); a
// nested group threads its own offset through it so its content is not clipped to a canvas-window.
// renderLayer() is the thin public entry: it renders the layer's isolated RGBA (renderLayerRaw)
// then applies its layer effects (LE-a) in the render seam, so EVERY caller -- the full walk,
// compositeGroup, and the drag cache's below/above rasters -- gets effects identically.
// `written` (optional) reports the target-space rect the render actually touched, so the caller's
// blend can skip the rest of the buffer -- see compositeBufferOver. Every path leaves it at the
// full buffer unless it can name something smaller.
[[nodiscard]] ImageF renderLayerRaw(const core::Layer& layer, const common::Affine2D& pre,
                                    std::uint32_t w, std::uint32_t h, const BlendFn& blend,
                                    const ResampleCtx& rs, common::Rect* written = nullptr);
[[nodiscard]] ImageF renderLayer(const core::Layer& layer, const common::Affine2D& pre,
                                 std::uint32_t w, std::uint32_t h, const BlendFn& blend,
                                 const ResampleCtx& rs, common::Rect* written = nullptr);

// A group's local-buffer rect, in the group's own coordinate space.
struct LocalExtent {
    long ox = 0;
    long oy = 0;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
};

// A safety valve against a pathologically scaled group demanding a vast local buffer (the canvas
// cap itself is 16384, §S9): beyond this the buffer clamps rather than allocating without bound.
constexpr std::uint32_t kMaxGroupBuffer = 16384;

// The larger of a transform's two column lengths (its per-axis scale): how many buffer px one
// layer-space px spans, so an effect's reach maps from layer space into the parent buffer.
[[nodiscard]] double maxAxisScale(const common::Affine2D& t) {
    return std::max(std::hypot(t.m00, t.m10), std::hypot(t.m01, t.m11));
}

// The farthest any descendant's effects paint OUTSIDE its content, expressed in `group`'s local
// space (each layer's reach scaled by the transform chain down to it). A group sizes its isolated
// buffer to its content PLUS this, so an outside stroke / drop shadow / outer glow on a grouped
// layer is not clipped at the group boundary (docs/layer-effects.md §4). `scale` accumulates the
// transforms above the recursion point (1.0 at the group itself).
[[nodiscard]] double descendantEffectsReach(const core::GroupLayer& group, double scale) {
    double reach = 0.0;
    for (std::size_t i = 0; i < group.childCount(); ++i) {
        const core::Layer& child = group.child(i);
        const double childScale = scale * maxAxisScale(child.transform());
        if (child.hasEffects() && !child.effects().empty())
            reach = std::max(reach, core::effectsOutwardReach(child.effects()) * childScale);
        if (const auto* sub = child.as<core::GroupLayer>())
            reach = std::max(reach, descendantEffectsReach(*sub, childScale));
    }
    return reach;
}

// The integer local-space rect an UNMASKED group composites its children into, given
// `localToTarget` (group-local -> the W x H target buffer): the group's content intersected with
// the part of the target window that pulls back into group-local space. Sizing the buffer to THIS
// (with an origin offset) instead of the fixed [0,W]x[0,H] window is what stops a transformed
// group from clipping children that fall outside the canvas-aligned window yet still project onto
// the visible canvas -- the "drill into a group and move a child" vanish bug. Returns false (no
// buffer) for a singular transform or empty / fully off-screen content.
[[nodiscard]] bool groupLocalExtent(const core::GroupLayer& group,
                                    const common::Affine2D& localToTarget, std::uint32_t W,
                                    std::uint32_t H, LocalExtent& out) {
    const std::optional<common::Rect> content = group.contentBounds(); // group-local space
    if (!content || content->empty()) return false;
    const std::optional<common::Affine2D> inv = localToTarget.inverse();
    if (!inv) return false; // singular: nothing shows through
    // Grow by any descendant effect reach so a grouped outside stroke / shadow is not clipped.
    common::Rect grown = *content;
    if (const double m = descendantEffectsReach(group, 1.0); m > 0.0)
        grown = {grown.x - m, grown.y - m, grown.w + 2.0 * m, grown.h + 2.0 * m};
    common::Rect visible =
        inv->mapBounds(common::Rect{0.0, 0.0, static_cast<double>(W), static_cast<double>(H)});
    // A spatial adjustment (S33 blur) inside the group both SPREADS content outward and READS
    // beyond the visible window's pullback, so its reach grows BOTH rects -- effects reach only
    // ever grew the content side (docs/blur-filters.md §5).
    if (const double m = descendantAdjustmentReach(group, grown.united(visible)); m > 0.0) {
        grown = {grown.x - m, grown.y - m, grown.w + 2.0 * m, grown.h + 2.0 * m};
        visible = {visible.x - m, visible.y - m, visible.w + 2.0 * m, visible.h + 2.0 * m};
    }
    const common::Rect r = grown.intersected(visible);
    if (r.empty()) return false;
    out.ox = static_cast<long>(std::floor(r.x));
    out.oy = static_cast<long>(std::floor(r.y));
    out.w = static_cast<std::uint32_t>(
        std::min<long>(static_cast<long>(std::ceil(r.right())) - out.ox, kMaxGroupBuffer));
    out.h = static_cast<std::uint32_t>(
        std::min<long>(static_cast<long>(std::ceil(r.bottom())) - out.oy, kMaxGroupBuffer));
    return out.w > 0 && out.h > 0;
}

// The mutable state of one group's bottom(0)->top walk. Threaded through walkStep so the full
// composite and the drag-cache replay (S15-b) share the exact per-layer semantics — any drift
// between them would show as the drag preview disagreeing with the committed composite.
struct GroupWalk {
    ImageF acc;
    std::vector<float> clipBase;  // alpha of the current clip base (the last non-clip layer)
    bool haveClipBase = false;
    // Maintaining the clip base is a full-frame alpha extraction per layer; skip the whole
    // mechanism when nothing in this group clips (the overwhelmingly common case).
    bool anyClips = false;
    // The walk's placement (this group's local space -> the buffer): the S33 blur branch maps
    // parent-space geometry through it and scales px parameters by its axis scale (§4 of
    // docs/blur-filters.md). Identity for canvas-space walks (the drag cache).
    common::Affine2D pre = common::Affine2D::identity();
    // The parent-space rect an adjustment's layer mask spans (the doc rect at root, the local
    // extent inside a group). Every walk sets it -- the default empty rect means "ignore
    // masks", which is never what a real walk wants.
    common::Rect maskDomain{};
    bool liveDrag = false;  // gesture in flight: heavy blur kernels may draft-subsample taps
};

[[nodiscard]] bool groupHasClips(const core::GroupLayer& group) {
    for (std::size_t i = 0; i < group.childCount(); ++i)
        if (group.child(i).clipToBelow()) return true;
    return false;
}

// Adjustments carry no raster of their own; walkStep's adjustment branch never reads src.
const ImageF kNoSrc;

// "Adjustment: <Kind>" for the profiler, from a table built once on first use -- so naming a row
// per kind costs no per-call allocation. Indexed by the enum, which is APPEND-ONLY (layer.hpp), so
// a new kind lands at the end and simply gets its own row.
[[nodiscard]] std::string_view adjustmentRowName(core::AdjustmentKind kind) {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (int i = 0; i <= static_cast<int>(core::AdjustmentKind::HighPass); ++i)
            v.emplace_back("Adjustment: " + std::string(core::adjustmentKindName(
                                                static_cast<core::AdjustmentKind>(i))));
        return v;
    }();
    const auto i = static_cast<std::size_t>(kind);
    return i < names.size() ? std::string_view(names[i]) : std::string_view("Adjustment: ?");
}

// One step of the walk: apply `layer` onto the accumulator with its blend mode, opacity and
// clip-to-below, maintaining the clip-base state. `src` is the layer's doc-space raster
// (renderLayer output) — const so the drag cache can replay from cached buffers; the (rare)
// clip multiply works on a copy. The caller filters invisible / zero-opacity layers.
void walkStep(GroupWalk& st, const core::Layer& layer, const ImageF& src, const BlendFn& blend,
              const common::Rect* srcBounds = nullptr) {
    const core::BlendMode mode = layer.blendMode();
    const float opacity = layer.opacity();
    if (const auto* adj = layer.as<core::AdjustmentLayer>()) {
        // docs/s60-performance-plan.md: adjustment cost had no row at all, because it is paid
        // inside this tree walk rather than at a call site anyone could scope. Instrumented HERE,
        // INSIDE the branch -- after the dynamic_cast has already succeeded -- so the row counts
        // real adjustment work instead of one near-zero sample per layer per frame (the
        // `updateReflectionEnv` lesson). Split spatial/scalar: a blur-family or Adaptive-Grayscale
        // adjustment is a whole-buffer kernel and costs orders of magnitude more than a per-pixel
        // curve, and averaging the two together would hide exactly the expensive one. Timing only
        // -- the CPU reference's arithmetic below is untouched and every golden holds.
        MOSAIC_PERF_SCOPE(core::adjustmentIsSpatial(*adj) ? "Adjustment layer (spatial)"
                                                          : "Adjustment layer (scalar)",
                          common::Lane::Cpu);
        // ...and the same sample again, keyed by KIND. The aggregate row above is the one the plan
        // documents and it stays, but it cannot answer "which kind owns this": it reported 26 calls
        // averaging 1.4 s with a 17.1 s worst case, and the honest guesses about which adjustment
        // that was were wrong twice. Per-kind, the answer was immediate -- Gaussian Blur, 30.3 s of
        // it, while Shadows/Highlights (suspect #1) was reporting 0.00 ms. The name comes from a
        // table built once, so a profiled run costs no allocation per adjustment and an unprofiled
        // one costs the same relaxed load it always did.
        MOSAIC_PERF_SCOPE(adjustmentRowName(adj->adjustmentKind()), common::Lane::Cpu);
        const std::vector<float>* cov =
            (adj->clipToBelow() && st.haveClipBase) ? &st.clipBase : nullptr;
        applyAdjustment(st.acc, *adj, cov, st.pre, st.maskDomain, st.liveDrag);
        return;
    }

    if (layer.clipToBelow() && st.haveClipBase) {
        ImageF clipped = src;
        multiplyAlpha(clipped, st.clipBase);
        {
            MOSAIC_PERF_SCOPE("Layer blend", common::Lane::Cpu);
            // multiplyAlpha only ever REDUCES alpha, so the clipped copy cannot be non-transparent
            // anywhere the original was not: the reported window still bounds it.
            blend(st.acc, clipped, mode, opacity, srcBounds);
        }
        return;  // a clipped layer never becomes the clip base itself
    }
    {
        // The one leaf cost with no row. Every layer -- a 300 px shape included -- blends over the
        // WHOLE accumulator, because renderLayer hands back a canvas-sized buffer and blend() has
        // no notion of the layer's extent.
        MOSAIC_PERF_SCOPE("Layer blend", common::Lane::Cpu);
        blend(st.acc, src, mode, opacity, srcBounds);
    }

    // A non-clipped layer becomes the clip base for any clipped layers stacked above it.
    if (st.anyClips) {
        st.clipBase.resize(src.pixelCount());
        parallelFor(st.clipBase.size(), std::size_t{1} << 16,
                    [&](std::size_t p0, std::size_t p1) {
                        for (std::size_t p = p0; p < p1; ++p)
                            st.clipBase[p] = src.rgba[p * 4 + 3];
                    });
        st.haveClipBase = true;
    }
}

// Composite a group's children bottom(0)->top into a fresh W x H buffer. `pre` maps the group's
// local space to that buffer (identity at the document root; a translation when a parent group
// offset its own local buffer): all children share the group's local space, so they all take it.
[[nodiscard]] ImageF compositeChildren(const core::GroupLayer& group, const common::Affine2D& pre,
                                       std::uint32_t w, std::uint32_t h, const BlendFn& blend,
                                       const ResampleCtx& rs, const common::Rect& maskDomain) {
    GroupWalk st;
    st.acc = ImageF(w, h);
    st.anyClips = groupHasClips(group);
    st.pre = pre;
    st.maskDomain = maskDomain;
    st.liveDrag = rs.liveDrag;
    for (std::size_t i = 0; i < group.childCount(); ++i) {
        const core::Layer& layer = group.child(i);
        if (!layer.visible() || layer.opacity() <= 0.0f) continue;
        // The read-only exclusion (CompositeOptions::skipLayer), applied at exactly the point an
        // invisible layer drops out -- so "composite without this layer" costs a comparison instead
        // of a document mutation.
        if (rs.skip != core::kInvalidLayerId && layer.id() == rs.skip) continue;
        if (layer.as<core::AdjustmentLayer>() != nullptr)
            walkStep(st, layer, kNoSrc, blend);
        else {
            // LEAF renders only: a group's cost is its own row (groupBuffers/groupBufferTexels),
            // and counting it here too would make "renders == leaf count" untrue for any nested
            // document -- an assertion that cannot hold is worse than no assertion.
            if (layer.as<core::GroupLayer>() == nullptr)
                workCounters().layerRenders.fetch_add(1, std::memory_order_relaxed);
            // The window renderLayer actually wrote, handed straight to the blend. Defaults to the
            // whole buffer, so a path that cannot report one stays exactly as correct as before.
            common::Rect written{0.0, 0.0, static_cast<double>(w), static_cast<double>(h)};
            const ImageF placed = renderLayer(layer, pre, w, h, blend, rs, &written);
            walkStep(st, layer, placed, blend, &written);
        }
    }
    return std::move(st.acc);
}

ImageF renderLayerRaw(const core::Layer& layer, const common::Affine2D& pre, std::uint32_t w,
                      std::uint32_t h, const BlendFn& blend, const ResampleCtx& rs,
                      common::Rect* written) {
    // Start at the whole buffer. Only the arms that KNOW their window narrow it; a kind that
    // cannot report one is left conservative and costs exactly what it always did.
    if (written != nullptr)
        *written = common::Rect{0.0, 0.0, static_cast<double>(w), static_cast<double>(h)};
    if (const auto* group = layer.as<core::GroupLayer>()) {
        // Composite the group as a unit in its own space, then place it through its transform;
        // its mask, opacity and blend are applied by the caller (the mask right here). All groups
        // take the content-extent local buffer (S31 closed the old masked-group exception, which
        // pinned the window to the target buffer and mis-folded under region composites): a
        // LINKED mask folds onto the local buffer in group-local space (1 mask px per local unit,
        // the buffer offset threaded through the fold); an UNLINKED one folds after placement, in
        // parent space, like every other kind.
        const common::Affine2D localToTarget = pre * group->transform();
        const core::RasterMask* mk = group->mask();
        if (mk != nullptr && (mk->empty() || !mk->enabled)) mk = nullptr;
        LocalExtent ext;
        if (!groupLocalExtent(*group, localToTarget, w, h, ext))
            return ImageF(w, h); // singular / nothing visible
        // ⚠ THE RESOLUTION OF THE ISOLATED BUFFER. `ext` is the group's content extent in
        // GROUP-LOCAL units, and one texel per local unit is the right buffer only when the group
        // lands on the target at roughly 1:1. Under a reduction it is catastrophic, because the
        // extent does not shrink with the target:
        //
        //     layer-panel thumbnail   target 34x34    -> local buffer 6956x5271   (31,717:1)
        //     3D reflect-env snapshot target 468x702  -> local buffer 5055x5271   (81:1)
        //
        // Both then discarded ~99.99% of that buffer in the placement resample below. The reflect
        // snapshot is the sharper indictment: it composites at 468x702 PRECISELY to be cheap, and
        // every group inside it quietly went back to full resolution anyway.
        //
        // Size the buffer to what the target can actually RESOLVE. `s` is the larger per-axis
        // scale of localToTarget, clamped to 1 so magnification never inflates the buffer (the
        // placement resample interpolates, as it always did).
        //
        // ⚠ s == 1 for any composite at or above document scale, which makes bw/bh exactly ext.w/
        // ext.h and every transform below reduce to its previous form -- so the full-canvas walk
        // is byte-identical and the goldens do not move. Only reductions change, and they change
        // toward a properly pre-filtered result instead of a point-sampled one.
        const double sAxisX = std::hypot(localToTarget.m00, localToTarget.m10);
        const double sAxisY = std::hypot(localToTarget.m01, localToTarget.m11);
        const double s = std::min(1.0, std::max(sAxisX, sAxisY));
        const std::uint32_t bw =
            s < 1.0 ? std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(ext.w * s)))
                    : ext.w;
        const std::uint32_t bh =
            s < 1.0 ? std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(ext.h * s)))
                    : ext.h;
        // buffer px -> group-local: undo the reduction, then the extent offset.
        const common::Affine2D bufToLocal =
            common::Affine2D::translation(static_cast<double>(ext.ox),
                                          static_cast<double>(ext.oy)) *
            common::Affine2D::scaling(static_cast<double>(ext.w) / bw,
                                      static_cast<double>(ext.h) / bh);
        workCounters().groupBuffers.fetch_add(1, std::memory_order_relaxed);
        workCounters().groupBufferTexels.fetch_add(static_cast<std::uint64_t>(bw) * bh,
                                                   std::memory_order_relaxed);
        ImageF local = compositeChildren(
            *group,
            common::Affine2D::scaling(static_cast<double>(bw) / ext.w,
                                      static_cast<double>(bh) / ext.h) *
                common::Affine2D::translation(-static_cast<double>(ext.ox),
                                              -static_cast<double>(ext.oy)),
            bw, bh, blend, rs,
            // The mask domain stays in LOCAL units: it is the rect an adjustment's mask spans in
            // the space the children are addressed in, which the buffer's resolution does not move.
            common::Rect{static_cast<double>(ext.ox), static_cast<double>(ext.oy),
                         static_cast<double>(ext.w), static_cast<double>(ext.h)});
        if (mk != nullptr && mk->linked) {
            // buffer -> group-local (the reduction + the extent offset) -> mask px (the sheet's
            // placement).
            if (const std::optional<common::Affine2D> mInv =
                    core::maskPlacement(*group, *mk).inverse())
                foldMaskThrough(local, *mk, *mInv * bufToLocal);
        }
        const common::Affine2D place = localToTarget * bufToLocal;
        ImageF out;
        {
            MOSAIC_PERF_SCOPE("Group place (resample)", common::Lane::Cpu);
            out = transformImageF(local, place, w, h, resolveFilter(rs.filter, place, rs.liveDrag));
        }
        if (written != nullptr) {
            const common::Rect reach{-kMaxFootprintRadius - 1.0, -kMaxFootprintRadius - 1.0,
                                     static_cast<double>(bw) + 2.0 * kMaxFootprintRadius + 2.0,
                                     static_cast<double>(bh) + 2.0 * kMaxFootprintRadius + 2.0};
            *written = place.mapBounds(reach);
        }
        foldUnlinkedMask(out, layer, pre);
        return out;
    }

    // Leaf layers: one fused pass converts + folds the mask + samples through the transform.
    const common::Image* src8 = nullptr;
    if (const auto* raster = layer.as<core::RasterLayer>()) {
        src8 = &raster->image();
    } else if (const auto* magic = layer.as<core::MagicLayer>()) {
        src8 = &magic->source();
    } else if (const auto* vlayer = layer.as<core::VectorLayer>()) {
        // Vector (S25): rasterize fill + stroke directly at TARGET resolution through `place`, so
        // the shape stays crisp at any zoom (no fixed-res bitmap to resample). Float-native output
        // -- no 8-bit quantization round-trip, so gradients composite band-free. The rasterizer
        // bounds its work to the object's pixel bbox. The mask folds at target resolution (S31):
        // a linked one through its sheet's placement in layer-local space (core::maskPlacement,
        // then place^-1, so it rides the transform), an unlinked one in parent space like every
        // other kind. The placement is what puts the sheet over the SHAPE: shape geometry is
        // centred on the local origin, so a sheet pinned to local (0,0) covered one quadrant of it
        // and zeroed the rest (the grid contract at RasterMask).
        if (!vlayer->hasObject()) return ImageF(w, h);
        const common::Affine2D vplace = pre * layer.transform();
        // The AA combo governs vector edges too (S26): an explicit Nearest filter hardens coverage
        // to crisp/aliased edges; Auto + every other kernel keep the analytic smooth AA.
        const bool antialias = rs.filter != ResampleFilter::Nearest;
        // Scoped because this is re-run at TARGET resolution on EVERY composite -- there is no
        // rasterised cache for a vector layer, and a live BooleanCompound re-flattens and
        // re-resolves its operands each time. Cheap for a rounded rect, not obviously cheap for a
        // 41-lobe rosette or a three-operand Exclude on a large canvas, and nothing measured it.
        ImageF vout;
        {
            MOSAIC_PERF_SCOPE("Vector rasterise", common::Lane::Cpu);
            vout = core::vec::rasterizeObjectF(*vlayer->object(), w, h, vplace, 0.25, antialias);
        }
        if (const core::RasterMask* m = layer.mask();
            m != nullptr && m->enabled && !m->empty() && m->linked) {
            if (const std::optional<common::Affine2D> inv =
                    (vplace * core::maskPlacement(layer, *m)).inverse())
                foldMaskThrough(vout, *m, *inv);
        }
        foldUnlinkedMask(vout, layer, pre);
        return vout;
    } else if (const auto* tlayer = layer.as<core::TextLayer>()) {
        // Text (S29-b): composite the app-populated pixel cache like a raster source -- the cache's
        // pixel->layer-local map folds in front of the layer transform (docs/type-tool.md §5.4). The
        // compositor never touches the font stack; an empty/unrendered block is a transparent layer.
        const common::Image* timg = tlayer->cachedImage();
        if (timg == nullptr) return ImageF(w, h);
        const common::Affine2D place = pre * layer.transform() * tlayer->cacheImageToLayer();
        ImageF tout;
        {
            MOSAIC_PERF_SCOPE("Layer resample (text)", common::Lane::Cpu);
            tout = rasteriseLayer(*timg, linkedMask(layer), place, w, h,
                                  resolveFilter(rs.filter, place, rs.liveDrag), written);
        }
        foldUnlinkedMask(tout, layer, pre);
        return tout;
    } else if (const auto* xlayer = layer.as<core::TextureLayer>()) {
        // Texture (S55-a): the app-populated generator cache composites exactly like the text arm
        // above; the compositor never sees a generator. The sky lane's cache is FLOAT and rides
        // the same fused pass unquantised (banding-free gradients, docs/texture-generator.md
        // §4.4); paper/grass are 8-bit. An unrendered cache is a transparent layer.
        const common::Affine2D place = pre * layer.transform() * xlayer->cacheImageToLayer();
        ImageF xout;
        if (const common::ImageF* fimg = xlayer->cachedImageF()) {
            xout = rasteriseLayer(*fimg, linkedMask(layer), place, w, h,
                                  resolveFilter(rs.filter, place, rs.liveDrag), written);
        } else {
            const common::Image* ximg = xlayer->cachedImage();
            if (ximg == nullptr) return ImageF(w, h);
            xout = rasteriseLayer(*ximg, linkedMask(layer), place, w, h,
                                  resolveFilter(rs.filter, place, rs.liveDrag), written);
        }
        foldUnlinkedMask(xout, layer, pre);
        return xout;
    } else {
        return ImageF(w, h);  // Adjustment/Magic handled elsewhere
    }
    const common::Affine2D place = pre * layer.transform();
    ImageF out;
    {
        MOSAIC_PERF_SCOPE("Layer resample (raster)", common::Lane::Cpu);
        out = rasteriseLayer(*src8, linkedMask(layer), place, w, h,
                             resolveFilter(rs.filter, place, rs.liveDrag), written);
    }
    foldUnlinkedMask(out, layer, pre);
    return out;
}

// The LAYER-LOCAL rect an effects layer's shading must see whole: every rendered pixel (for a 3D
// text solid that is the pixel CACHE's extent -- the rotated solid legitimately overhangs the flat
// contentBounds) dilated by the effects' outward reach. nullopt = unknown (fall back to the plain
// cropped path). The context matters because SDF / blur / gradient-normalisation all read the
// WHOLE silhouette: computing them from a window-cropped alpha invents an edge at the window
// border, which is exactly the "bevel rendered in blocks that do not connect" a dirty-REGION
// composite showed (user 2026-07-16; brush strokes + S60-b text edits composite regions).
std::optional<common::Rect> effectsContextLocal(const core::Layer& layer,
                                                const core::LayerEffects& fx) {
    std::optional<common::Rect> local;
    if (const auto* tl = layer.as<core::TextLayer>()) {
        const common::Image* img = tl->cachedImage();
        if (img != nullptr && !img->rgba.empty())
            local = tl->cacheImageToLayer().mapBounds(
                {0.0, 0.0, static_cast<double>(img->width), static_cast<double>(img->height)});
    }
    if (!local) local = layer.contentBounds();
    if (!local || local->empty()) return std::nullopt;
    const double m = core::effectsOutwardReach(fx);
    return common::Rect{local->x - m, local->y - m, local->w + 2.0 * m, local->h + 2.0 * m};
}

// The render seam (docs/layer-effects.md §4): render the layer's isolated RGBA, then apply its
// non-destructive effects in place before walkStep() blends it. No effects (the overwhelmingly
// common case) short-circuits to renderLayerRaw's exact output -- byte-identical, no cost. For a
// group this runs on the placed group composite (its children already carry their own effects,
// applied when this same wrapper rendered them), so group effects style the group as a unit.
// Rewrite `img`'s alpha so that compositing `upper` over it with plain `over` reconstructs the
// surface the two were cut from -- the coverage-partition fix (core::CoveragePartition).
//
// The halves store f = A*m (fragment) and r = A*(1-m) (this layer, the residual). `over` would
// give f + r(1-f) = A - r*f, short by r*f: up to 25% of the alpha at the half-coverage contour,
// which is the translucent rim along a feathered or anti-aliased cut. Solving f + b(1-f) = A for
// the residual's alpha gives
//
//     b = (A - f) / (1 - f) = r / (1 - f)          [A = f + r]
//
// so the rewrite needs only the fragment's own alpha, read straight off `lp.upper`. Three
// properties make this hold far more widely than a special case for "pasted directly on top":
//
//   * `over` is ASSOCIATIVE, so it does not matter how far up the stack the fragment sits or how
//     many pass-through groups wrap it -- layers in between simply occlude the reconstructed
//     surface exactly as they would have occluded the uncut original.
//   * alpha compositing is BLEND-INDEPENDENT (ao = as + ab(1-as)), so the fragment may carry any
//     blend mode; it then blends against a whole surface rather than a holey one.
//   * an ATTENUATED fragment (its own opacity, an enclosing group's, a linked mask) has target
//     alpha r + k*f, which is the same rewrite with the effective alpha substituted -- so nudging
//     the opacity slides continuously from "fully reassembled" to "the bare soft hole" instead of
//     snapping back to a rim.
//
// Both halves live on a common integer grid (integerGridPlacement is part of the liveness test),
// so the sample below is an integer index, never a resample.
void reconstructPartition(ImageF& img, const core::LivePartition& lp, const common::Affine2D& pre) {
    const auto* raster = lp.upper->as<const core::RasterLayer>();
    if (raster == nullptr || raster->image().empty() || img.empty()) return;
    // buffer px -> document px -> the fragment's own pixel grid.
    const std::optional<common::Affine2D> unPre = pre.inverse();
    const std::optional<common::Affine2D> unWorld = core::worldTransform(*lp.upper).inverse();
    if (!unPre || !unWorld) return;
    const common::Affine2D toFragment = *unWorld * *unPre;
    const common::Image& frag = raster->image();
    // A linked mask shares the fragment's own pixel grid (S31), sampled proportionally when the
    // resolutions differ -- the same fold the renderer applies.
    const core::RasterMask* mask = lp.upper->mask();
    if (mask != nullptr && (!mask->enabled || mask->empty())) mask = nullptr;

    parallelFor(img.height, 32, [&](std::uint32_t y0, std::uint32_t y1) {
        for (std::uint32_t y = y0; y < y1; ++y) {
            for (std::uint32_t x = 0; x < img.width; ++x) {
                const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
                const float r = img.rgba[p + 3];
                if (r <= 0.0f) continue;  // nothing there to hold open
                const common::Vec2 s = toFragment.apply({x + 0.5, y + 0.5});
                const long fx = static_cast<long>(std::floor(s.x));
                const long fy = static_cast<long>(std::floor(s.y));
                if (fx < 0 || fy < 0 || fx >= static_cast<long>(frag.width) ||
                    fy >= static_cast<long>(frag.height))
                    continue;  // outside the lifted piece: the residual is already whole here
                const auto ux = static_cast<std::uint32_t>(fx);
                const auto uy = static_cast<std::uint32_t>(fy);
                float f = frag.rgba[(static_cast<std::size_t>(uy) * frag.width + ux) * 4 + 3] /
                          255.0f * lp.scale;
                if (mask != nullptr) {
                    const std::uint32_t mx =
                        mask->width == frag.width
                            ? ux
                            : static_cast<std::uint32_t>(static_cast<std::size_t>(ux) *
                                                         mask->width / frag.width);
                    const std::uint32_t my =
                        mask->height == frag.height
                            ? uy
                            : static_cast<std::uint32_t>(static_cast<std::size_t>(uy) *
                                                         mask->height / frag.height);
                    f *= mask->coverage[static_cast<std::size_t>(my) * mask->width + mx] / 255.0f;
                }
                img.rgba[p + 3] = f >= 1.0f ? 0.0f : std::min(1.0f, r / (1.0f - f));
            }
        }
    });
}

ImageF renderLayer(const core::Layer& layer, const common::Affine2D& pre, std::uint32_t w,
                   std::uint32_t h, const BlendFn& blend, const ResampleCtx& rs,
                   common::Rect* written) {
    if (written != nullptr)
        *written = common::Rect{0.0, 0.0, static_cast<double>(w), static_cast<double>(h)};
    // The partition rewrite rides on top of whatever the layer otherwise renders as. A partition
    // requires an un-effected raster, so this can never race the effects path below.
    if (rs.reconstructPartitions) {
        if (const core::LivePartition lp = core::livePartitionFor(layer)) {
            ImageF img = renderLayerRaw(layer, pre, w, h, blend, rs);
            reconstructPartition(img, lp, pre);
            return img;
        }
    }
    if (!(layer.hasEffects() && !layer.effects().empty()))
        return renderLayerRaw(layer, pre, w, h, blend, rs, written);

    // 3D text consumes its overlays UPSTREAM (S30-e, docs/type-tool.md §12): the extrude
    // lanes evaluate colour/gradient/pattern overlays in glyph design space and bake them
    // onto the solid's faces, so applying them again here would smear the design over the
    // whole projected 3D rectangle -- exactly the outcome §12 exists to prevent. Strip them
    // and let everything else (shadows, glows, strokes, bevel, satin, fill-opacity) operate
    // on the composited 2D result, which is where those belong for a solid too.
    core::LayerEffects fx = layer.effects();
    if (const auto* tl = layer.as<core::TextLayer>(); tl != nullptr && tl->block().extrude) {
        fx.colorOverlay.enabled = false;
        fx.gradientOverlay.enabled = false;
        fx.patternOverlay.enabled = false;
        if (fx.empty()) return renderLayerRaw(layer, pre, w, h, blend, rs);
    }

    // Effects must be computed over the layer's WHOLE silhouette (see effectsContextLocal). When
    // the target buffer is a WINDOW that cuts through it (a dirty-region composite; also a shape
    // overhanging the canvas), render the full footprint in its own buffer, apply the effects
    // there, then crop the window back -- a region patch is then byte-identical to the same
    // window of a full composite, seam-free. A footprint inside the buffer keeps today's exact
    // in-place path (byte-identical, no copy).
    common::Affine2D fxPre = pre;
    long cropX = 0, cropY = 0;  // where the buffer window sits inside the footprint buffer
    std::uint32_t fw = w, fh = h;
    bool windowed = false;
    if (const auto local = effectsContextLocal(layer, fx)) {
        const common::Rect fb = (pre * layer.transform()).mapBounds(*local);
        // Pad for the resample filter's tap footprint, then snap outward to whole pixels.
        const long fx0 = static_cast<long>(std::floor(fb.x)) - 2;
        const long fy0 = static_cast<long>(std::floor(fb.y)) - 2;
        const long fx1 = static_cast<long>(std::ceil(fb.right())) + 2;
        const long fy1 = static_cast<long>(std::ceil(fb.bottom())) + 2;
        constexpr long kMaxFootprintPx = 16L * 1024 * 1024;  // ~256MB float RGBA: past it, crop
        if ((fx0 < 0 || fy0 < 0 || fx1 > static_cast<long>(w) || fy1 > static_cast<long>(h)) &&
            fx1 > fx0 && fy1 > fy0 && (fx1 - fx0) * (fy1 - fy0) <= kMaxFootprintPx) {
            // Grow to cover BOTH the footprint and the requested window (the window part not
            // covered by effects must still carry the raw render).
            const long ux0 = std::min(fx0, 0L), uy0 = std::min(fy0, 0L);
            const long ux1 = std::max(fx1, static_cast<long>(w));
            const long uy1 = std::max(fy1, static_cast<long>(h));
            if ((ux1 - ux0) * (uy1 - uy0) <= kMaxFootprintPx) {
                fxPre = common::Affine2D::translation(static_cast<double>(-ux0),
                                                      static_cast<double>(-uy0)) *
                        pre;
                fw = static_cast<std::uint32_t>(ux1 - ux0);
                fh = static_cast<std::uint32_t>(uy1 - uy0);
                cropX = -ux0;
                cropY = -uy0;
                windowed = true;
            }
        }
    }

    ImageF out = renderLayerRaw(layer, fxPre, fw, fh, blend, rs);
    // Pattern effect edges follow the document-wide AA combobox, exactly like vector edges above:
    // an explicit Nearest filter hardens them; Auto + every other kernel keep the smooth ramp.
    // Pass the buffer->layer-local map so a layer-anchored pattern is glued to the layer (rotates
    // and scales with it) rather than staying axis-aligned in buffer space.
    const common::Affine2D place = fxPre * layer.transform();  // layer-local -> buffer
    std::optional<common::Affine2D> bufferToLayer = place.inverse();
    // Anchor a layer-glued pattern's tile origin AND its angle-rotation pivot at the layer
    // content's TOP-LEFT corner, expressed in LAYER-LOCAL space (via contentBounds, so it stays
    // put under the layer's own rotation/scale). The pattern angle rotates about u==0; without
    // this shift u==0 sits at the layer's local origin, which can lie far from the content (e.g.
    // an identity-transform layer whose paint is in the canvas middle) -- so even a small angle
    // sweeps the whole pattern along a big arc instead of spinning in place. Baking translate(-cbTL)
    // after the inverse makes samplePattern see content-corner-relative coords, matching the
    // headless content-box fallback in paintAtNorm.
    if (bufferToLayer)
        if (const auto cb = layer.contentBounds())
            bufferToLayer = common::Affine2D::translation(-cb->x, -cb->y) * *bufferToLayer;
    // The S60-e GPU lane, offered the seam before the CPU reference runs (compositor.hpp).
    const bool fxAntialias = rs.filter != ResampleFilter::Nearest;
    // The effect stack had NO profiler row at all, which made a nine-effect headline invisible in
    // a table that was otherwise complete -- the same blind spot the adjustment branch had before
    // S60-a. Scoped HERE, past the fx.empty() short-circuit above, so an un-effected layer (the
    // overwhelming majority) still contributes no sample and cannot bury the one layer that costs.
    // The GPU override is inside the scope on purpose: it is the same seam doing the same job, and
    // reading Gpu against Cpu for one name is how the lane earns its keep.
    {
        MOSAIC_PERF_SCOPE("Layer effects",
                          g_layerEffectsOverride ? common::Lane::Gpu : common::Lane::Cpu);
        if (!(g_layerEffectsOverride &&
              g_layerEffectsOverride(out, fx, fxAntialias, bufferToLayer)))
            applyEffects(out, fx, fxAntialias, bufferToLayer);
    }
    if (!windowed) return out;

    ImageF window(w, h);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::size_t src = ((static_cast<std::size_t>(y) + cropY) * fw + cropX) * 4;
        std::copy_n(out.rgba.begin() + static_cast<std::ptrdiff_t>(src),
                    static_cast<std::size_t>(w) * 4,
                    window.rgba.begin() + static_cast<std::size_t>(y) * w * 4);
    }
    return window;
}

// Flatten the working buffer over the transparency checkerboard, yielding an opaque image.
void flattenOverCheckerboard(ImageF& acc, const CompositeOptions& opts) {
    const std::uint32_t size = opts.checkerSize == 0 ? 1 : opts.checkerSize;
    const auto chan = [](std::uint8_t v) { return static_cast<float>(v) / 255.0f; };
    const ColorF light{chan(opts.checkerLight.r), chan(opts.checkerLight.g),
                       chan(opts.checkerLight.b), 1.0f};
    const ColorF dark{chan(opts.checkerDark.r), chan(opts.checkerDark.g),
                      chan(opts.checkerDark.b), 1.0f};
    parallelFor(acc.height, 64, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            std::size_t p = static_cast<std::size_t>(y) * acc.width * 4;
            for (std::uint32_t x = 0; x < acc.width; ++x, p += 4) {
                const ColorF& bg = ((x / size) + (y / size)) % 2 != 0 ? dark : light;
                const float a = acc.rgba[p + 3];
                acc.rgba[p + 0] = acc.rgba[p + 0] * a + bg.r * (1.0f - a);
                acc.rgba[p + 1] = acc.rgba[p + 1] * a + bg.g * (1.0f - a);
                acc.rgba[p + 2] = acc.rgba[p + 2] * a + bg.b * (1.0f - a);
                acc.rgba[p + 3] = 1.0f;
            }
        }
    });
}

// Parallel float -> 8-bit conversion (same rounding as common::toImage8): the serial
// conversion alone was ~10% of a 1080p frame.
[[nodiscard]] common::Image toImage8Parallel(const ImageF& acc) {
    common::Image out(acc.width, acc.height);
    parallelFor(out.rgba.size(), std::size_t{1} << 18, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) {
            const float v = std::clamp(acc.rgba[i], 0.0f, 1.0f);
            out.rgba[i] = static_cast<std::uint8_t>(v * 255.0f + 0.5f);
        }
    });
    return out;
}

// Fill a rectangle of an 8-bit image (clipped to its bounds) with a solid color.
void fillRect(common::Image& img, std::uint32_t x0, std::uint32_t y0, std::uint32_t rw,
              std::uint32_t rh, common::Color8 c) {
    for (std::uint32_t y = y0; y < y0 + rh && y < img.height; ++y) {
        for (std::uint32_t x = x0; x < x0 + rw && x < img.width; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
            img.rgba[p] = c.r;
            img.rgba[p + 1] = c.g;
            img.rgba[p + 2] = c.b;
            img.rgba[p + 3] = c.a;
        }
    }
}

// Shared core for composite() and compositeRegion(): pick the blend backend, walk the WHOLE layer
// tree into a `w`x`h` buffer placed by `pre` (the document-root -> buffer map; identity for a full
// composite, a translation for a region), then flatten + convert to 8-bit. The blend step runs on
// the GPU when requested and available; the rest of the walk (masks, clip, adjustments,
// checkerboard) is the shared CPU code. Auto prefers the GPU and falls back to CPU; an explicit
// Gpu/GpuCompute request errors out if the GPU is unavailable.
[[nodiscard]] CompositeResult compositeBuffer(const core::Document& doc, const common::Affine2D& pre,
                                              std::uint32_t w, std::uint32_t h,
                                              const CompositeOptions& opts, Backend backend) {
    CompositeResult res;
    res.usedBackend = Backend::Cpu;
    static const auto log = common::log::category("render");

    BlendFn blend = compositeBufferOver;
    std::unique_ptr<GpuCompositor> gpu;
    const bool wantGpu =
        backend == Backend::Auto || backend == Backend::Gpu || backend == Backend::GpuCompute;
    if (wantGpu) {
        std::string gpuErr;
        gpu = GpuCompositor::create(gpuErr);
        if (gpu) {
            res.usedBackend = Backend::GpuCompute;
            GpuCompositor* g = gpu.get();
            // The GPU kernel blends whole buffers; it ignores the bound rather than growing a
            // sub-rect dispatch for a demo-only path, and its CPU fallback is handed the bound so
            // a refusal costs what the CPU lane would have cost anyway.
            blend = [g](ImageF& acc, const ImageF& src, core::BlendMode mode, float opacity,
                        const common::Rect* bounds) {
                std::string e;
                if (!g->blendOver(acc, src, mode, opacity, e)) {
                    compositeBufferOver(acc, src, mode, opacity, bounds); // per-op fallback
                }
            };
        } else if (backend != Backend::Auto) {
            res.error = "GPU compositor unavailable: " + gpuErr;
            return res;
        } else {
            log->debug("GPU compositor unavailable ({}); using CPU", gpuErr);
        }
    }

    // The mask domain is the DOC rect, not the buffer: a region composite's buffer is a crop,
    // and adjustment masks are authored against the document (the S33 masked-adjustment fix).
    ImageF acc = compositeChildren(doc.root(), pre, w, h, blend,
                                   ResampleCtx{opts.resampleFilter, opts.liveDrag,
                                               /*reconstructPartitions=*/true, opts.skipLayer},
                                   common::Rect{0.0, 0.0, static_cast<double>(doc.width()),
                                                static_cast<double>(doc.height())});
    if (opts.checkerboard) flattenOverCheckerboard(acc, opts);
    res.image = toImage8Parallel(acc);
    if (gpu) res.validationErrors = gpu->validationErrors();
    res.ok = true;
    return res;
}
}  // namespace

void setBlurRenderOverride(BlurRenderOverride fn) { g_blurOverride = std::move(fn); }

void setLayerEffectsRenderOverride(LayerEffectsRenderOverride fn) {
    g_layerEffectsOverride = std::move(fn);
}

CompositeResult composite(const core::Document& doc, const CompositeOptions& opts, Backend backend) {
    const std::uint32_t w = doc.width();
    const std::uint32_t h = doc.height();
    if (w == 0 || h == 0) {
        CompositeResult res;
        res.error = "composite: zero-sized canvas";
        return res;
    }
    static const auto log = common::log::category("render");
    workCounters().composites.fetch_add(1, std::memory_order_relaxed);
    log->debug("compositing {}x{} ({} layers)", w, h, doc.layerCount());
    return compositeBuffer(doc, common::Affine2D::identity(), w, h, opts, backend);
}

CompositeResult compositeScaled(const core::Document& doc, std::uint32_t outW, std::uint32_t outH,
                                const CompositeOptions& opts, Backend backend) {
    const std::uint32_t w = doc.width();
    const std::uint32_t h = doc.height();
    CompositeResult res;
    if (w == 0 || h == 0) {
        res.error = "compositeScaled: zero-sized canvas";
        return res;
    }
    if (outW == 0 || outH == 0) {
        res.error = "compositeScaled: zero-sized output";
        return res;
    }
    // At the document's own size this IS composite(): take the identity `pre` so every fast path
    // that keys on it (the leaf rasteriser's whole-row copy, transformImageF's exact 1:1 arm) still
    // fires and the result is byte-identical to the full walk rather than merely close to it.
    if (outW == w && outH == h)
        return compositeBuffer(doc, common::Affine2D::identity(), w, h, opts, backend);
    static const auto log = common::log::category("render");
    workCounters().composites.fetch_add(1, std::memory_order_relaxed);
    log->debug("compositing {}x{} into {}x{} ({} layers)", w, h, outW, outH, doc.layerCount());
    // Document -> buffer. Per axis, so a caller that rounded each dimension independently still
    // maps the document's own corners onto the buffer's.
    return compositeBuffer(doc,
                           common::Affine2D::scaling(static_cast<double>(outW) / w,
                                                     static_cast<double>(outH) / h),
                           outW, outH, opts, backend);
}

CompositeResult compositeRegion(const core::Document& doc, const common::Rect& roi,
                                const CompositeOptions& opts, Backend backend, bool clampToCanvas) {
    CompositeResult res;
    const long docW = static_cast<long>(doc.width());
    const long docH = static_cast<long>(doc.height());
    if (docW == 0 || docH == 0) {
        res.error = "compositeRegion: zero-sized canvas";
        return res;
    }
    // Floor/ceil to whole pixels. When clamped, also clip to the canvas: the result's top-left then
    // sits at (x0, y0); interactive callers pass an already-in-canvas integer rect, so clamping is a
    // no-op. Unclamped, the rect may straddle the canvas edge -- the buffer then spans off-canvas
    // pixels too (transparent, no layer projects there), placed by the same translation `pre`.
    long x0 = static_cast<long>(std::floor(roi.x));
    long y0 = static_cast<long>(std::floor(roi.y));
    long x1 = static_cast<long>(std::ceil(roi.right()));
    long y1 = static_cast<long>(std::ceil(roi.bottom()));
    if (clampToCanvas) {
        x0 = std::max(0L, x0);
        y0 = std::max(0L, y0);
        x1 = std::min(docW, x1);
        y1 = std::min(docH, y1);
    }
    if (x1 <= x0 || y1 <= y0) {
        res.error = "compositeRegion: empty or out-of-canvas roi";
        return res;
    }
    // A root-level spatial adjustment (S33 blur) reads beyond the requested rect, so the buffer
    // that must come out CORRECT is larger than the rect asked for: expand by the stack's total
    // reach, clamp back to the canvas (the full composite's own domain -- the kernels' edge
    // policy has to fire at the same physical pixel for region == crop(full) to hold byte-
    // exactly), composite, then crop. Group-nested blurs need no expansion here: their groups'
    // local buffers grow in groupLocalExtent instead (docs/blur-filters.md §5).
    const double reach = descendantAdjustmentReach(
        doc.root(), common::Rect{static_cast<double>(x0), static_cast<double>(y0),
                                 static_cast<double>(x1 - x0), static_cast<double>(y1 - y0)});
    if (reach >= 0.5) {
        const long e = static_cast<long>(std::ceil(reach));
        long ex0 = x0 - e;
        long ey0 = y0 - e;
        long ex1 = x1 + e;
        long ey1 = y1 + e;
        if (clampToCanvas) {
            ex0 = std::max(0L, ex0);
            ey0 = std::max(0L, ey0);
            ex1 = std::min(docW, ex1);
            ey1 = std::min(docH, ey1);
        }
        CompositeResult expanded = compositeBuffer(
            doc,
            common::Affine2D::translation(-static_cast<double>(ex0), -static_cast<double>(ey0)),
            static_cast<std::uint32_t>(ex1 - ex0), static_cast<std::uint32_t>(ey1 - ey0), opts,
            backend);
        if (!expanded.ok) return expanded;
        // Crop back to the requested rect. (Checker flattening, when on, stays anchored to the
        // expanded buffer; the interactive region caller flattens in screen space instead.)
        const auto w = static_cast<std::uint32_t>(x1 - x0);
        const auto h = static_cast<std::uint32_t>(y1 - y0);
        const auto dx = static_cast<std::size_t>(x0 - ex0);
        const auto dy = static_cast<std::size_t>(y0 - ey0);
        common::Image out(w, h);
        for (std::uint32_t row = 0; row < h; ++row) {
            const std::size_t src = ((row + dy) * expanded.image.width + dx) * 4;
            std::copy_n(expanded.image.rgba.begin() + static_cast<std::ptrdiff_t>(src),
                        static_cast<std::size_t>(w) * 4,
                        out.rgba.begin() + static_cast<std::size_t>(row) * w * 4);
        }
        expanded.image = std::move(out);
        return expanded;
    }
    return compositeBuffer(
        doc,
        common::Affine2D::translation(-static_cast<double>(x0), -static_cast<double>(y0)),
        static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0), opts, backend);
}

namespace {

// Does any layer in `doc` currently carry a live coverage partition (so some residual's alpha is
// being rewritten at render time)? The two drag fast paths below both cache or bypass that rewrite
// and so must stand down while one is in effect.
[[nodiscard]] bool groupHasLivePartition(const core::GroupLayer& group) {
    for (std::size_t i = 0; i < group.childCount(); ++i) {
        const core::Layer& c = group.child(i);
        if (c.partition().has_value() && core::livePartitionFor(c)) return true;
        if (const auto* g = c.as<const core::GroupLayer>())
            if (groupHasLivePartition(*g)) return true;
    }
    return false;
}

[[nodiscard]] bool documentHasLivePartition(const core::Document& doc) {
    return groupHasLivePartition(doc.root());
}

}  // namespace

bool canUseGpuDrag(const core::Document& doc, core::LayerId target) {
    const core::GroupLayer& root = doc.root();
    const std::size_t n = root.childCount();
    if (n == 0)
        return false;
    // Must be the TOPMOST direct child of the root -- nothing composites above it, so the GPU only
    // needs `below` + the dragged layer (no above texture / backdrop-dependent blends to bake).
    const core::Layer& top = root.child(n - 1);
    if (top.id() != target)
        return false;
    const auto* raster = top.as<core::RasterLayer>();
    if (raster == nullptr)
        return false; // need concrete source pixels for the GPU texture
    if (!top.visible())
        return false;
    if (const core::RasterMask* m = top.mask(); m != nullptr && m->enabled)
        return false; // a mask would need folding -- CPU path
    if (top.clipToBelow())
        return false; // clip-to-below is not a plain source-over
    if (static_cast<int>(top.blendMode()) > 18)
        return false; // non-separable HSL mode -- handled only on the CPU
    if (top.hasEffects() && !top.effects().empty())
        return false; // layer effects are CPU-only until S60 -- keep the drag on the CPU path
    // A live coverage partition anywhere below needs the residual's alpha rewritten before the
    // blend (the GPU pass composites raw layer textures), and the rewrite stops applying the
    // instant the drag breaks the partition. Both are CPU-path concerns.
    if (documentHasLivePartition(doc))
        return false;
    return true;
}

common::Image rasterizeLayer(const core::Layer& layer, std::uint32_t docW, std::uint32_t docH,
                             ResampleFilter filter) {
    if (docW == 0 || docH == 0)
        return {};
    // `pre` = identity means "render in the layer's PARENT-local space", which is exactly where the
    // replacement raster will sit with an identity transform of its own. renderLayer folds the mask
    // and applies the effects; it never applies the layer's opacity/blend, so those ride across to
    // the raster and the composite is unchanged. liveDrag is false: a commit deserves the full
    // kernel even if a gesture happens to be in flight.
    return common::toImage8(renderLayer(layer, common::Affine2D::identity(), docW, docH,
                                        compositeBufferOver, ResampleCtx{filter, false}));
}

WorkCounters& workCounters() noexcept {
    static WorkCounters c;
    return c;
}

common::Image compositeGroupInto(const core::GroupLayer& group, const common::Affine2D& docToOut,
                                 std::uint32_t outW, std::uint32_t outH) {
    if (outW == 0 || outH == 0)
        return {};
    // Identical to compositeGroup below, except that the group's world placement is composed with
    // the caller's document->output map, so the whole walk happens at output resolution.
    return common::toImage8(renderLayer(group, docToOut * core::parentWorldTransform(group), outW,
                                        outH, compositeBufferOver,
                                        ResampleCtx{ResampleFilter::Auto, false}));
}

common::Image compositeGroup(const core::GroupLayer& group, std::uint32_t docW,
                             std::uint32_t docH) {
    if (docW == 0 || docH == 0)
        return {};
    // Render the group placed by its WORLD transform (ancestors included). renderLayer owns the
    // local-extent sizing + mask folding; the group's own opacity/blend are intentionally ignored
    // (thumbnail / "select the group's pixels" want its content as drawn, not as composited up).
    // Auto quality (no gesture) so a rotated/scaled group thumbnail is anti-aliased.
    return common::toImage8(
        renderLayer(group, core::parentWorldTransform(group), docW, docH, compositeBufferOver,
                    ResampleCtx{ResampleFilter::Auto, false}));
}

namespace {
// Shared core of adjustmentPreview / adjustmentBackdrop: the compositeChildren walk over the
// adjustment's parent, truncated AT the adjustment. Including its own step gives "the backdrop
// with the adjustment applied" (the dock preview); excluding it gives the raw backdrop (the
// panel's histogram source).
[[nodiscard]] common::Image adjustmentScopeComposite(const core::AdjustmentLayer& adj,
                                                     std::uint32_t docW, std::uint32_t docH,
                                                     std::uint32_t outW, std::uint32_t outH,
                                                     bool includeSelf) {
    if (docW == 0 || docH == 0 || outW == 0 || outH == 0)
        return {};
    const core::GroupLayer* parent = adj.parent();
    if (parent == nullptr)
        return {}; // detached layer (mid-undo): nothing to preview
    // parent-local -> preview buffer: the ancestors' world placement, then the document window
    // scaled into the (small) output — the walk renders every sibling at preview resolution
    // directly, so the cost is bounded by outW x outH, not the canvas.
    const common::Affine2D pre =
        common::Affine2D::scaling(static_cast<double>(outW) / docW,
                                  static_cast<double>(outH) / docH) *
        core::parentWorldTransform(adj);
    GroupWalk st;
    st.acc = ImageF(outW, outH);
    st.anyClips = groupHasClips(*parent);
    st.pre = pre;  // preview scale: blur px parameters ride maxAxisScale(pre) (§4)
    // Mask domain = the parent-space rect the preview window covers (the doc rect for a
    // root-level adjustment) -- the same span the old buffer-scaled mask read implied.
    if (const std::optional<common::Affine2D> inv = pre.inverse())
        st.maskDomain = inv->mapBounds(common::Rect{0.0, 0.0, static_cast<double>(outW),
                                                    static_cast<double>(outH)});
    // The dock preview and the histogram grade the SAME backdrop the canvas shows, partition
    // reconstruction included.
    const ResampleCtx rs{ResampleFilter::Auto, false, /*reconstructPartitions=*/true};
    for (std::size_t i = 0; i < parent->childCount(); ++i) {
        const core::Layer& layer = parent->child(i);
        const bool self = layer.id() == adj.id();
        if (self && !includeSelf)
            break;
        if (layer.visible() && layer.opacity() > 0.0f) {
            if (layer.as<core::AdjustmentLayer>() != nullptr)
                walkStep(st, layer, kNoSrc, compositeBufferOver);
            else
                walkStep(st, layer,
                         renderLayer(layer, pre, outW, outH, compositeBufferOver, rs),
                         compositeBufferOver);
        }
        if (self)
            break;
    }
    return common::toImage8(st.acc);
}
} // namespace

common::Image adjustmentPreview(const core::AdjustmentLayer& adj, std::uint32_t docW,
                                std::uint32_t docH, std::uint32_t outW, std::uint32_t outH) {
    return adjustmentScopeComposite(adj, docW, docH, outW, outH, /*includeSelf=*/true);
}

common::Image adjustmentBackdrop(const core::AdjustmentLayer& adj, std::uint32_t docW,
                                 std::uint32_t docH, std::uint32_t outW, std::uint32_t outH) {
    return adjustmentScopeComposite(adj, docW, docH, outW, outH, /*includeSelf=*/false);
}

std::optional<common::Image> mergeDown(const core::Layer& upper, const core::RasterLayer& lower) {
    ImageF src;
    if (const auto* raster = upper.as<core::RasterLayer>())
        src = common::toFloat(raster->image());
    else if (const auto* magic = upper.as<core::MagicLayer>())
        src = common::toFloat(magic->source());
    else
        return std::nullopt; // group/vector/text/texture/adjustment: nothing to bake (yet)
    if (src.empty() || lower.image().empty())
        return std::nullopt;
    const core::RasterMask* mk = upper.mask();
    if (mk != nullptr && (mk->empty() || !mk->enabled)) mk = nullptr;
    if (mk != nullptr && mk->linked) foldMask(src, *mk);

    const std::optional<common::Affine2D> lowerInv = lower.transform().inverse();
    if (!lowerInv)
        return std::nullopt; // a singular lower transform has no pixel space to bake into
    // upper-local -> document -> lower-local, sampled at lower's resolution (the compositor's
    // nearest-neighbour semantics).
    ImageF placed = transformImageF(src, *lowerInv * upper.transform(), lower.image().width,
                                    lower.image().height);
    // An unlinked mask sits in the siblings' shared parent space: fold it AFTER placement,
    // sampling through lower's transform (lower-local -> parent) and then the sheet's own
    // placement there, exactly like the composite. `upper` is always a raster/magic source here,
    // whose sheet is its source grid, so that placement is the identity in practice.
    if (mk != nullptr && !mk->linked) {
        if (const std::optional<common::Affine2D> mInv = core::maskPlacement(upper, *mk).inverse())
            foldMaskThrough(placed, *mk, *mInv * lower.transform());
    }
    ImageF acc = common::toFloat(lower.image());
    if (upper.clipToBelow()) { // a clipped layer only shows where its base has coverage
        std::vector<float> baseAlpha(acc.pixelCount());
        for (std::size_t i = 0; i < baseAlpha.size(); ++i) baseAlpha[i] = acc.rgba[i * 4 + 3];
        multiplyAlpha(placed, baseAlpha);
    }
    // Merging the two halves of a live coverage partition is the one case where `over` is the wrong
    // operator outright: the halves TILE each pixel rather than overlapping, so their coverages add
    // instead of compounding. Baking them with `over` would freeze the rim into the pixels
    // permanently -- the one outcome no later edit can undo -- so the disjoint operator runs here
    // even though the stack composite reaches the same picture by rewriting alpha instead.
    if (core::partitionPairLive(lower, upper)) {
        const std::size_t n = std::min(acc.pixelCount(), placed.pixelCount());
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t p = i * 4;
            const float ab = acc.rgba[p + 3];
            const float as = placed.rgba[p + 3] * upper.opacity();
            const float ao = std::min(1.0f, ab + as);
            if (ao <= 0.0f) continue;
            const float inv = 1.0f / ao;
            acc.rgba[p] = (ab * acc.rgba[p] + as * placed.rgba[p]) * inv;
            acc.rgba[p + 1] = (ab * acc.rgba[p + 1] + as * placed.rgba[p + 1]) * inv;
            acc.rgba[p + 2] = (ab * acc.rgba[p + 2] + as * placed.rgba[p + 2]) * inv;
            acc.rgba[p + 3] = ao;
        }
        return common::toImage8(acc);
    }
    compositeBufferOver(acc, placed, upper.blendMode(), upper.opacity());
    return common::toImage8(acc);
}

MergeDownBake mergeDownBaked(const core::Layer& upper, const core::Layer& lower,
                             std::uint32_t docW, std::uint32_t docH, ResampleFilter filter,
                             bool emptyBackdrop) {
    MergeDownBake out;
    if (docW == 0 || docH == 0)
        return out;
    // `pre` = identity renders each layer in its PARENT's coordinate space, which is where the
    // replacement raster will sit with an identity transform of its own -- rasterizeLayer's exact
    // framing, so the mask/effects folding is the one implementation. liveDrag is false (a commit
    // deserves the full kernel) and partitions are NOT reconstructed here: the disjoint recombine
    // belongs to mergeDown()'s raster path, and this route only ever sees pairs that are not one.
    const ResampleCtx rs{filter, false};
    const ImageF lowerRas =
        renderLayer(lower, common::Affine2D::identity(), docW, docH, compositeBufferOver, rs);
    ImageF upperRas =
        renderLayer(upper, common::Affine2D::identity(), docW, docH, compositeBufferOver, rs);

    // The clip base a clipped UPPER layer sees is the lower layer's RAW rendered alpha -- before
    // the lower's own opacity and blend -- exactly as walkStep records it.
    std::vector<float> clipBase(lowerRas.pixelCount());
    for (std::size_t i = 0; i < clipBase.size(); ++i) clipBase[i] = lowerRas.rgba[i * 4 + 3];

    // Two walk steps onto an empty accumulator: this IS compositeChildren's arithmetic for the
    // pair, so a merge of the bottom two layers is byte-identical to what the canvas showed.
    ImageF acc(docW, docH);
    compositeBufferOver(acc, lowerRas, lower.blendMode(), lower.opacity());
    if (upper.blendMode() != core::BlendMode::Normal && !emptyBackdrop) {
        // A blend mode has to see the backdrop it saw live. Baked, it sees only `acc` -- which is
        // honest exactly where `acc` is opaque, so anything less is a refusal, not a wobble.
        const std::size_t n = std::min(acc.pixelCount(), upperRas.pixelCount());
        for (std::size_t i = 0; i < n; ++i) {
            if (upperRas.rgba[i * 4 + 3] > 0.0f && acc.rgba[i * 4 + 3] < 0.999f) {
                out.status = MergeDownBake::Status::UpperBlendUnbaked;
                return out;
            }
        }
    }
    if (upper.clipToBelow()) multiplyAlpha(upperRas, clipBase);
    compositeBufferOver(acc, upperRas, upper.blendMode(), upper.opacity());
    out.image = common::toImage8(acc);
    out.status = MergeDownBake::Status::Ok;
    return out;
}

common::Image applyAdjustmentToImage(const core::AdjustmentLayer& adj, const common::Image& img,
                                     const common::Affine2D& imageToParent, std::uint32_t docW,
                                     std::uint32_t docH) {
    if (img.rgba.empty())
        return img;
    ImageF acc = common::toFloat(img);
    // clip-to-below on an adjustment means "only where the base below has coverage"; merged down,
    // that base is this very image, so its alpha IS the coverage.
    std::vector<float> cov;
    const std::vector<float>* covp = nullptr;
    if (adj.clipToBelow()) {
        cov.resize(acc.pixelCount());
        for (std::size_t i = 0; i < cov.size(); ++i) cov[i] = acc.rgba[i * 4 + 3];
        covp = &cov;
    }
    // applyAdjustment's `pre` maps the shared PARENT space ONTO the buffer, so it is the inverse of
    // the caller's placement. A singular placement projects to nothing; the identity keeps the
    // scalar kinds working and leaves the geometric ones at their unscaled parameters.
    const std::optional<common::Affine2D> pre = imageToParent.inverse();
    applyAdjustment(acc, adj, covp, pre ? *pre : common::Affine2D::identity(),
                    common::Rect{0.0, 0.0, static_cast<double>(docW), static_cast<double>(docH)},
                    /*liveDrag=*/false);
    return common::toImage8(acc);
}

std::optional<core::vec::Object> mergeDownVector(const core::VectorLayer& upper,
                                                 const core::VectorLayer& lower) {
    const core::vec::Object* uo = upper.object();
    const core::vec::Object* lo = lower.object();
    if (uo == nullptr || lo == nullptr)
        return std::nullopt;
    // One object = one fill + one stroke + one paint order; one layer = one opacity, one blend
    // mode, one clip flag. Anything that disagrees cannot survive the combine, so it is a raster
    // job -- and with those four equal the combine is exact for ANY of them, because the disjointness
    // required below means every pixel is reached by exactly one of the two shapes.
    if (!(uo->fill == lo->fill) || !(uo->stroke == lo->stroke) || uo->paintOrder != lo->paintOrder)
        return std::nullopt;
    if (upper.opacity() != lower.opacity() || upper.blendMode() != lower.blendMode() ||
        upper.clipToBelow() != lower.clipToBelow())
        return std::nullopt;
    // A mask sheet and an effect stack are per LAYER: two of them cannot ride one layer, and one
    // drop shadow around the union is not the two the pair drew.
    const auto masked = [](const core::Layer& l) {
        const core::RasterMask* m = l.mask();
        return m != nullptr && m->enabled && !m->empty();
    };
    if (masked(upper) || masked(lower))
        return std::nullopt;
    if ((upper.hasEffects() && !upper.effects().empty()) ||
        (lower.hasEffects() && !lower.effects().empty()))
        return std::nullopt;

    const std::optional<common::Affine2D> lowerInv = lower.transform().inverse();
    if (!lowerInv)
        return std::nullopt;
    const common::Affine2D rel = *lowerInv * upper.transform();  // upper-local -> lower-local
    const bool linearIdentity =
        rel.m00 == 1.0 && rel.m01 == 0.0 && rel.m10 == 0.0 && rel.m11 == 1.0;
    const bool relIdentity = linearIdentity && rel.m02 == 0.0 && rel.m12 == 0.0;
    const auto anchored = [](const core::vec::Paint& p) {
        return std::holds_alternative<core::vec::Gradient>(p) ||
               std::holds_alternative<core::vec::Pattern>(p);
    };
    // A gradient / pattern is sampled in the OBJECT's local space, so rebasing one shape into the
    // other's space would recolour it; a stroke's width, dashes and miter are measured in the same
    // units, so a scale/rotation/shear would re-weight it (a translation leaves it alone).
    if ((anchored(lo->fill) || anchored(lo->stroke.paint)) && !relIdentity)
        return std::nullopt;
    if (lo->stroke.enabled && !linearIdentity)
        return std::nullopt;

    // No boolean op: concatenated contours reproduce two layers only where nothing is reached by
    // both. vec::contentBounds(Object) already includes the stroke's outward reach; one extra unit
    // of slack keeps two anti-aliased edges from meeting in the same pixel.
    const std::optional<common::Rect> lb = core::vec::contentBounds(*lo);
    const std::optional<common::Rect> ub = core::vec::contentBounds(*uo);
    if (!lb || !ub)
        return std::nullopt;
    const common::Rect grown{lb->x - 1.0, lb->y - 1.0, lb->w + 2.0, lb->h + 2.0};
    if (grown.intersects(rel.mapBounds(*ub)))
        return std::nullopt;

    core::vec::Path lp = core::vec::pathFromGeometry(lo->geometry);
    core::vec::Path up =
        core::vec::transformedPath(core::vec::pathFromGeometry(uo->geometry), rel);
    // Two fill rules cannot ride one path. (pathFromGeometry gives a parametric shape NonZero --
    // what its rasteriser assumes -- so a mixed pair only ever fails on hand-edited paths.)
    if (lp.fillRule != up.fillRule)
        return std::nullopt;
    if (lp.subpaths.empty() || up.subpaths.empty())
        return std::nullopt;  // nothing to combine: let the raster route say so
    for (core::vec::SubPath& sp : up.subpaths) lp.subpaths.push_back(std::move(sp));
    core::vec::Object out = *lo;  // fill, stroke and paint order all come from the lower object
    out.geometry = std::move(lp);
    return out;
}

namespace {

// buildDocumentRemapCommand's tree walk (the Crop tool's rebase, generalised in S53-a from "the
// crop shift" to any invertible `worldToNew`). `localShift` is that remap expressed in `group`'s
// local space. It is pushed INTO unmasked groups (conjugated by their transform) instead of
// onto them, so leaf world transforms come out identical either way and the rebase stays an
// affine-only edit (no per-child resampling). (Historically this also dodged a compositor bug:
// an UNMASKED group's local buffer now follows its content's visible extent, but it used to be a
// canvas-sized window that a translated group would slide off the kept content. That bug is
// fixed, so a direct group rebase would now composite correctly too; the push-into-children
// routing is kept because it is simpler geometry and still needed for the cases below.) Masked
// groups and singular ones rebase as a unit instead — a group's LINKED mask lives in group-local
// space (S31), so pushing the shift into the children would slide them out from under it, while
// rebasing the group carries mask and children together. (An UNLINKED mask deliberately stays in
// parent space through any transform, crop rebases included -- that is what unlinked means.)
// `ancestorsIdentity` tracks an all-identity group chain: only there can the delete-mode bake
// land the new-canvas image at identity without changing what the walk samples; `bakeFilter` is
// the kernel that bake resamples with (Nearest for the Crop tool, whose remap is a whole-pixel
// window; the caller's quality pick for Image Size / arbitrary rotation, which genuinely resample).
// `newToOld` maps a NEW-canvas point back onto the old document plane and `canvasLocked` says the
// remap carries the pixel grid onto itself at unit scale (an integer translation, a flip, a
// 90-degree.k turn): both serve the canvas-locked texture case below. A TEXTURE-GENERATOR layer is
// a procedural fill that texture::refreshTextureCache always regenerates to cover the WHOLE new
// canvas at identity cacheImageToLayer, so the rebase must NOT slide it by the remap -- that would
// displace the regenerated fill and leave an empty stripe -- and must remap its document-window
// mask to the kept window so the mask stays aligned with the fill (S31: paint lands right, no
// masked-out band survives the crop). Only an untransformed such layer with identity ancestors
// under a grid-preserving remap qualifies; anything else (a scale, an arbitrary rotation) falls
// through to the generic transform rebase, which keeps the fill's placement honest.
void addRemapRebase(core::CompositeCommand& cmd, const core::GroupLayer& group,
                    const common::Affine2D& localShift, std::uint32_t w, std::uint32_t h,
                    bool deletePixels, bool ancestorsIdentity, const common::Affine2D& newToOld,
                    bool canvasLocked, ResampleFilter bakeFilter,
                    const core::Layer* skip = nullptr) {
    for (const std::unique_ptr<core::Layer>& child : group.children()) {
        if (child.get() == skip) {
            continue; // handled by the caller (the S16-f extend-in-place fill bake)
        }
        if (const auto* sub = child->as<core::GroupLayer>();
            sub != nullptr && !child->hasMask()) {
            if (const std::optional<common::Affine2D> inv = sub->transform().inverse()) {
                addRemapRebase(cmd, *sub, *inv * localShift * sub->transform(), w, h,
                               deletePixels, ancestorsIdentity && isIdentity(sub->transform()),
                               newToOld, canvasLocked, bakeFilter);
                continue;
            } // singular: nothing shows through it; fall through to rebase the group itself
        }
        if (const auto* tex = child->as<core::TextureLayer>();
            tex != nullptr && canvasLocked && ancestorsIdentity && isIdentity(tex->transform())) {
            // Canvas-locked fill (see the function note): leave the transform alone so the
            // regenerated cache re-anchors to the new canvas, and remap its document-window mask to
            // the kept window (edge-clamped, so a shrink is an exact window and an expand extends
            // the border -- matching the compositor's clamped mask read). `canvasLocked` guarantees
            // the mapped centre is an exact cell centre, so a flip / quarter-turn is as lossless
            // here as the integer translation this loop was written for.
            if (const core::RasterMask* m = tex->mask(); m != nullptr && !m->empty()) {
                core::RasterMask cropped(w, h, 0);
                cropped.enabled = m->enabled;
                cropped.linked = m->linked;
                // The sheet's placement carries over unchanged: this branch only fires on a
                // canvas-locked layer at the identity transform, whose local space rebases WITH
                // the canvas -- the crop moves the cells, not the map they sit under.
                cropped.toLocal = m->toLocal;
                for (std::uint32_t ny = 0; ny < h; ++ny) {
                    for (std::uint32_t nx = 0; nx < w; ++nx) {
                        const common::Vec2 o = newToOld.apply(
                            {static_cast<double>(nx) + 0.5, static_cast<double>(ny) + 0.5});
                        const long sx =
                            std::clamp<long>(static_cast<long>(std::floor(o.x)), 0,
                                             static_cast<long>(m->width) - 1);
                        const long sy =
                            std::clamp<long>(static_cast<long>(std::floor(o.y)), 0,
                                             static_cast<long>(m->height) - 1);
                        cropped.coverage[static_cast<std::size_t>(ny) * w + nx] =
                            m->coverage[static_cast<std::size_t>(sy) * m->width + sx];
                    }
                }
                cmd.add(std::make_unique<core::SetLayerMaskCommand>(child->id(),
                                                                    std::move(cropped), "Crop"));
            }
            continue;
        }
        const auto* raster = child->as<core::RasterLayer>();
        if (deletePixels && ancestorsIdentity && raster != nullptr && !child->hasMask()) {
            // Bake through the compositor's own sampler so the composite is byte-exact: the
            // baked image at identity is, pixel for pixel, what the walk would have sampled
            // from the old image through the shifted transform.
            cmd.add(std::make_unique<core::SetTransformCommand>(child->id(),
                                                                common::Affine2D::identity()));
            cmd.add(std::make_unique<core::SetLayerPixelsCommand>(
                child->id(),
                common::toImage8(rasteriseLayer(raster->image(), nullptr,
                                                localShift * child->transform(), w, h,
                                                bakeFilter))));
        } else {
            cmd.add(std::make_unique<core::SetTransformCommand>(
                child->id(), localShift * child->transform()));
        }
    }
}

// S16-f: write the fill over every pixel of `img` that lies inside the NEW canvas [0,w)x[0,h)
// but outside the OLD canvas footprint — the area the expansion (or a rotated crop's corner
// wedges) added. `newToOld` maps new-canvas coordinates back onto the old document plane; a
// pixel whose mapped CENTRE, floored (nearest-pixel semantics), lands in [0,oldW)x[0,oldH) is
// old footprint and never touched. Axis-aligned crops pass the integer translation (x,y), for
// which the floored test is exactly the old integer window test. The fill is the constant `c`,
// or, when `src` is given (a new-canvas-sized image, the Inpaint mode's healed result), that
// image's pixel at the same canvas position. `originX/originY` = img's top-left in new-canvas
// coordinates (non-zero for the union-sized extend-in-place buffer).
void fillExpansion(common::Image& img, long originX, long originY,
                   const common::Affine2D& newToOld, long oldW, long oldH,
                   std::uint32_t canvasW, std::uint32_t canvasH, common::Color8 c,
                   const common::Image* src = nullptr) {
    if (src != nullptr && (src->width != canvasW || src->height != canvasH))
        src = nullptr; // contract violation: fall back to the constant, never misindex
    for (std::uint32_t py = 0; py < img.height; ++py) {
        const long cy = originY + static_cast<long>(py);
        if (cy < 0 || cy >= static_cast<long>(canvasH))
            continue;
        for (std::uint32_t px = 0; px < img.width; ++px) {
            const long cx = originX + static_cast<long>(px);
            if (cx < 0 || cx >= static_cast<long>(canvasW))
                continue;
            const common::Vec2 oldPt = newToOld.apply(
                {static_cast<double>(cx) + 0.5, static_cast<double>(cy) + 0.5});
            const long ox = static_cast<long>(std::floor(oldPt.x));
            const long oy = static_cast<long>(std::floor(oldPt.y));
            if (ox >= 0 && ox < oldW && oy >= 0 && oy < oldH)
                continue; // old canvas footprint — never touched
            const std::size_t o = (static_cast<std::size_t>(py) * img.width + px) * 4;
            if (src != nullptr) {
                const std::size_t so =
                    (static_cast<std::size_t>(cy) * src->width + static_cast<std::size_t>(cx)) *
                    4;
                img.rgba[o] = src->rgba[so];
                img.rgba[o + 1] = src->rgba[so + 1];
                img.rgba[o + 2] = src->rgba[so + 2];
                img.rgba[o + 3] = src->rgba[so + 3];
            } else {
                img.rgba[o] = c.r;
                img.rgba[o + 1] = c.g;
                img.rgba[o + 2] = c.b;
                img.rgba[o + 3] = c.a;
            }
        }
    }
}

// The standard extendable Background: root's BOTTOM child, an unmasked raster at identity whose
// image is exactly the old canvas. Anything else (transformed, masked, group, content-sized
// buffer) gets the fallback fill layer instead — extending in place is only done where it is
// provably a lossless integer-offset blit.
[[nodiscard]] const core::RasterLayer* extendableBackground(const core::Document& doc) {
    const auto& kids = doc.root().children();
    if (kids.empty() || kids.front()->hasMask())
        return nullptr;
    const auto* raster = kids.front()->as<core::RasterLayer>();
    if (raster == nullptr || !isIdentity(raster->transform()))
        return nullptr;
    if (raster->image().width != doc.width() || raster->image().height != doc.height())
        return nullptr;
    return raster;
}

// A remap that is a whole-pixel translation of the document plane -- the axis-aligned crop /
// Canvas Size case, where the new canvas is an integer window of the old one. The extend-in-place
// Background blit and Selection::cropped are exact only here.
[[nodiscard]] bool isIntegerTranslation(const common::Affine2D& t) noexcept {
    return t.m00 == 1.0 && t.m01 == 0.0 && t.m10 == 0.0 && t.m11 == 1.0 && isInteger(t.m02) &&
           isInteger(t.m12);
}

// A remap that carries the integer pixel grid onto itself at UNIT scale: an integer translation, a
// flip, a 90-degree.k turn, or any composition of them. isLosslessGrid alone also admits integer
// SCALES (2x, 3x, ...), which move pixels to different cells; |det| == 1 is what pins "same cells,
// permuted", i.e. the remaps under which a mask / selection sheet is an exact index remap.
[[nodiscard]] bool isGridWindow(const common::Affine2D& t) noexcept {
    if (!isLosslessGrid(t)) return false;
    const double det = t.determinant();  // exact: every entry is an integer here
    return det == 1.0 || det == -1.0;
}

// A remap that is a pure positive scale about the document origin -- Image Size. The selection
// scales with it (Selection::scaled); anything else about the plane is left alone.
[[nodiscard]] bool isOriginScale(const common::Affine2D& t) noexcept {
    return t.m01 == 0.0 && t.m10 == 0.0 && t.m02 == 0.0 && t.m12 == 0.0 && t.m00 > 0.0 &&
           t.m11 > 0.0;
}

// The document's guides carried through `worldToNew` (S53-a). A guide is a full-length line of
// constant x or y, so it survives exactly the remaps that map axes onto axes: an axis-PRESERVING
// one (translate / scale / flip) keeps each guide's orientation, an axis-SWAPPING one (a 90-degree
// turn) exchanges horizontal and vertical. An arbitrary rotation turns the line slanted, which the
// model cannot represent, so those guides are dropped rather than silently left behind. Guides
// pushed off the new canvas are dropped too; ids are preserved so undo restores them exactly.
[[nodiscard]] std::vector<core::Guide> remapGuides(const std::vector<core::Guide>& guides,
                                                   const common::Affine2D& worldToNew,
                                                   std::uint32_t newW, std::uint32_t newH) {
    std::vector<core::Guide> out;
    if (guides.empty())
        return out;
    const bool axisPreserving = worldToNew.m01 == 0.0 && worldToNew.m10 == 0.0;
    const bool axisSwapping = worldToNew.m00 == 0.0 && worldToNew.m11 == 0.0;
    if (!axisPreserving && !axisSwapping)
        return out;
    out.reserve(guides.size());
    for (const core::Guide& g : guides) {
        core::Guide n = g;
        if (axisPreserving) {
            n.position = g.horizontal() ? worldToNew.m11 * g.position + worldToNew.m12
                                        : worldToNew.m00 * g.position + worldToNew.m02;
        } else {
            n.orientation = g.horizontal() ? core::Guide::Orientation::Vertical
                                           : core::Guide::Orientation::Horizontal;
            n.position = g.horizontal() ? worldToNew.m01 * g.position + worldToNew.m02
                                        : worldToNew.m10 * g.position + worldToNew.m12;
        }
        const double extent =
            n.horizontal() ? static_cast<double>(newH) : static_cast<double>(newW);
        if (n.position < 0.0 || n.position > extent)
            continue; // the remap pushed it off the new canvas
        out.push_back(n);
    }
    return out;
}

// The selection carried through `worldToNew`. The mask is a document-sized coverage sheet, so it
// follows exactly as far as the remap is honest about pixels: an integer window is a copy, a
// flip / quarter-turn is an exact index permutation, a pure scale box-resamples the coverage, and
// an arbitrary rotation is CLEARED -- its pixel geometry does not survive the resample (the S16-f
// rotated-crop rule, kept).
[[nodiscard]] core::Selection remapSelection(const core::Selection& sel,
                                             const common::Affine2D& worldToNew,
                                             std::uint32_t newW, std::uint32_t newH) {
    if (isIntegerTranslation(worldToNew)) {
        return sel.cropped(-static_cast<long>(worldToNew.m02), -static_cast<long>(worldToNew.m12),
                           newW, newH);
    }
    if (isGridWindow(worldToNew))
        return sel.remapped(worldToNew, newW, newH);
    if (isOriginScale(worldToNew))
        return sel.scaled(newW, newH);
    return core::Selection{};
}

}  // namespace

std::unique_ptr<core::CompositeCommand>
buildDocumentRemapCommand(core::Document& doc, std::uint32_t w, std::uint32_t h,
                          const common::Affine2D& shift, bool deletePixels,
                          const std::optional<CropFill>& fill, std::string_view label,
                          ResampleFilter bakeFilter) {
    if (w == 0 || h == 0)
        return nullptr;
    const std::optional<common::Affine2D> newToOldOpt = shift.inverse();
    if (!newToOldOpt)
        return nullptr; // a singular remap collapses the document to nothing
    auto cmd = std::make_unique<core::CompositeCommand>(std::string(label));
    cmd->add(std::make_unique<core::ResizeCanvasCommand>(w, h));

    // S16-f expansion fill (see the header contract): only the area the old canvas did not
    // cover ever receives colour, so the visible result over the old footprint is untouched by
    // construction. A ROTATED crop always has fill portions (the wedges), so it counts as
    // expanding. newToOld maps new-canvas coords back onto the old document plane; dx,dy stay
    // the axis-aligned fast path's blit offset.
    const long oldW = static_cast<long>(doc.width());
    const long oldH = static_cast<long>(doc.height());
    const bool translationOnly = isIntegerTranslation(shift);
    const long dx = translationOnly ? static_cast<long>(shift.m02) : 0;
    const long dy = translationOnly ? static_cast<long>(shift.m12) : 0;
    const long x = -dx;  // the equivalent crop window's origin in OLD document coordinates
    const long y = -dy;
    const bool expands = !translationOnly || x < 0 || y < 0 ||
                         x + static_cast<long>(w) > oldW || y + static_cast<long>(h) > oldH;
    const common::Affine2D newToOld = *newToOldOpt;
    const core::Layer* extendSkip = nullptr;
    if (fill && expands && translationOnly) { // extend-in-place is a lossless blit: no rotation
        if (const core::RasterLayer* bg = extendableBackground(doc)) {
            extendSkip = bg;
            common::Image grown;
            common::Affine2D grownXform = common::Affine2D::identity();
            if (deletePixels) {
                // Delete mode clips every layer to the new canvas — the background too.
                grown = common::Image(w, h);
                common::blitRegion(grown, bg->image(), dx, dy);
                fillExpansion(grown, 0, 0, newToOld, oldW, oldH, w, h, fill->color,
                              fill->pixels ? &*fill->pixels : nullptr);
            } else {
                // Non-delete keeps every pixel: size the buffer to the UNION of the old
                // content and the new canvas, so a combined crop+expand (e.g. crop left,
                // expand right) destroys nothing.
                const long minX = std::min(0L, dx);
                const long minY = std::min(0L, dy);
                const long maxX = std::max(static_cast<long>(w), dx + oldW);
                const long maxY = std::max(static_cast<long>(h), dy + oldH);
                grown = common::Image(static_cast<std::uint32_t>(maxX - minX),
                                      static_cast<std::uint32_t>(maxY - minY));
                common::blitRegion(grown, bg->image(), dx - minX, dy - minY);
                fillExpansion(grown, minX, minY, newToOld, oldW, oldH, w, h, fill->color,
                              fill->pixels ? &*fill->pixels : nullptr);
                if (minX != 0 || minY != 0)
                    grownXform = common::Affine2D::translation(static_cast<double>(minX),
                                                               static_cast<double>(minY));
            }
            if (!isIdentity(grownXform))
                cmd->add(std::make_unique<core::SetTransformCommand>(bg->id(), grownXform));
            cmd->add(std::make_unique<core::SetLayerPixelsCommand>(bg->id(), std::move(grown)));
        }
    }

    addRemapRebase(*cmd, doc.root(), shift, w, h, deletePixels, /*ancestorsIdentity=*/true,
                   newToOld, isGridWindow(shift), bakeFilter, extendSkip);

    if (fill && expands && extendSkip == nullptr) {
        // Fallback: a new bottom raster covering ONLY the expansion (transparent over the old
        // footprint), so exotic stacks keep their exact appearance where the old canvas was.
        // Rotated crops always land here (extend-in-place is angle==0 only): the layer holds
        // the wedges + any outset, computed through newToOld.
        std::unique_ptr<core::RasterLayer> fillLayer = doc.makeRaster(fill->layerName, w, h);
        fillExpansion(fillLayer->image(), 0, 0, newToOld, oldW, oldH, w, h, fill->color,
                      fill->pixels ? &*fill->pixels : nullptr);
        cmd->add(std::make_unique<core::AddLayerCommand>(doc.root().id(), /*index=*/0,
                                                         std::move(fillLayer)));
    }

    // GUIDES ride the canvas (S53-a). Until now EVERY crop stranded them at their old document
    // coordinates -- a plain bug, fixed here in the shared engine so the Crop tool inherits it.
    if (std::vector<core::Guide> guides = remapGuides(doc.guides(), shift, w, h);
        guides != doc.guides()) {
        cmd->add(std::make_unique<core::SetGuidesCommand>(std::move(guides)));
    }

    if (!doc.selection().isEmpty())
        cmd->add(std::make_unique<core::SetSelectionCommand>(
            remapSelection(doc.selection(), shift, w, h)));
    return cmd;
}

std::unique_ptr<core::CompositeCommand> buildCropCommand(core::Document& doc, long x, long y,
                                                         std::uint32_t w, std::uint32_t h,
                                                         bool deletePixels,
                                                         const std::optional<CropFill>& fill,
                                                         double angle, common::Vec2 pivot) {
    // World -> new canvas: un-rotate the document about the pivot (the crop frame), then shift
    // the box origin to (0,0). angle == 0 keeps the historical pure translation, which is what
    // the engine's translationOnly fast paths key on.
    const common::Affine2D unrotate =
        angle == 0.0 ? common::Affine2D::identity()
                     : common::Affine2D::translation(pivot.x, pivot.y) *
                           common::Affine2D::rotation(-angle) *
                           common::Affine2D::translation(-pivot.x, -pivot.y);
    const common::Affine2D shift =
        common::Affine2D::translation(-static_cast<double>(x), -static_cast<double>(y)) *
        unrotate;
    return buildDocumentRemapCommand(doc, w, h, shift, deletePixels, fill, "Crop");
}

// ---- DragCompositeCache (S15-b) -------------------------------------------------------------
//
// During a Move drag only the dragged layer's transform changes per frame, yet the full walk
// re-rasterises and re-blends every layer. The cache freezes everything that cannot change
// mid-drag: the accumulator of the root children below the target, the doc-space raster of
// each child above it, and the clip-base state entering the target's index. The per-frame
// replay then re-rasterises ONLY the moved layer and re-runs walkStep over the cached buffers
// in stack order — bit-identical to the full walk because blends, clip multiplies and
// adjustments all run live against the accumulator; only the (expensive, transform-free)
// rasterisations are reused.

// Drag-cache admission (count + byte budget) lives in the header: see dragCacheFits.

bool DragCompositeCache::matches(const core::Document& doc, core::LayerId target,
                                 const CompositeOptions& opts) const noexcept {
    return m_valid && m_target == target && m_filter == opts.resampleFilter &&
           m_liveDrag == opts.liveDrag && m_width == doc.width() && m_height == doc.height() &&
           m_childCount == doc.root().childCount() && m_targetIndex < m_childCount &&
           doc.root().child(m_targetIndex).id() == target;
}

void DragCompositeCache::invalidate() noexcept {
    m_valid = false;
    m_belowAcc = common::ImageF{};
    m_groupLocal = common::ImageF{};
    m_above.clear();
    m_clipBase.clear();
}

bool DragCompositeCache::rebuild(const core::Document& doc, core::LayerId target,
                                 const CompositeOptions& opts) {
    invalidate();
    const std::uint32_t w = doc.width();
    const std::uint32_t h = doc.height();
    if (w == 0 || h == 0) return false;
    // The static below/above layers must be rendered exactly as the full composite would for the
    // SAME options (so the replay stays byte-identical to composite()); only the moved layer is
    // re-rasterised per frame in composite() below.
    const ResampleCtx rs{opts.resampleFilter, opts.liveDrag, /*reconstructPartitions=*/true};
    const core::GroupLayer& root = doc.root();
    const std::size_t idx = root.indexOf(target);
    if (idx == core::GroupLayer::npos)
        return false;  // not a top-level child: the general case belongs to S60-a's tiles
    const core::Layer& moved = root.child(idx);
    m_targetIsGroup = moved.kind() == core::LayerKind::Group;
    if (!m_targetIsGroup && moved.as<core::RasterLayer>() == nullptr &&
        moved.as<core::MagicLayer>() == nullptr && moved.as<core::TextLayer>() == nullptr &&
        moved.as<core::TextureLayer>() == nullptr)
        return false;  // no cached pixels to re-place per frame (and no Move handles either)
    // The moved layer is re-rasterised per frame BELOW without going through renderLayer, so its
    // own effects would be skipped; a group target's cached local buffer is also sized to plain
    // content bounds (no effect margin). Either way, fall back to the full composite (which
    // applies effects in the renderLayer seam) rather than drift from it. (Effects on the static
    // below/above layers ARE cached correctly -- those go through renderLayer.)
    if (moved.hasEffects() && !moved.effects().empty()) return false;
    if (m_targetIsGroup && descendantEffectsReach(*moved.as<core::GroupLayer>(), 1.0) > 0.0)
        return false;
    // A dragged group carrying a spatial adjustment (S33 blur) needs its local buffer grown by
    // the blur's reach, but the cache sizes it to plain content bounds -- same clipping hazard
    // as effects above, same answer: leave it to the full composite.
    if (m_targetIsGroup) {
        const auto& g = *moved.as<core::GroupLayer>();
        const std::optional<common::Rect> cb = g.contentBounds();
        const common::Rect dom = cb ? *cb
                                    : common::Rect{0.0, 0.0, static_cast<double>(doc.width()),
                                                   static_cast<double>(doc.height())};
        if (descendantAdjustmentReach(g, dom) > 0.0) return false;
    }
    // An UNLINKED mask folds in parent space through the LIVE transform (it stays put while the
    // pixels slide under it), so no per-gesture buffer can capture it: full composite per frame.
    if (const core::RasterMask* m = moved.mask();
        m != nullptr && m->enabled && !m->linked && !m->empty())
        return false;

    // Budget check before any allocation: count AND bytes (see kMaxCachedDragBytes).
    std::size_t buffers = 1 + (m_targetIsGroup ? 1 : 0);  // belowAcc (+ the group local)
    for (std::size_t j = idx + 1; j < root.childCount(); ++j) {
        const core::Layer& layer = root.child(j);
        if (layer.visible() && layer.opacity() > 0.0f &&
            layer.as<core::AdjustmentLayer>() == nullptr)
            ++buffers;
    }
    if (!dragCacheFits(buffers, w, h)) return false;

    // The walk below the target — exactly compositeChildren's loop over [0, idx), CPU blend
    // (the cache backs the UI's deterministic Backend::Cpu recomposite).
    GroupWalk st;
    st.acc = ImageF(w, h);
    st.anyClips = groupHasClips(root);
    st.maskDomain = {0.0, 0.0, static_cast<double>(w), static_cast<double>(h)};  // canvas space
    for (std::size_t j = 0; j < idx; ++j) {
        const core::Layer& layer = root.child(j);
        if (!layer.visible() || layer.opacity() <= 0.0f) continue;
        if (layer.as<core::AdjustmentLayer>() != nullptr)
            walkStep(st, layer, kNoSrc, compositeBufferOver);
        else
            walkStep(st, layer,
                     renderLayer(layer, common::Affine2D::identity(), w, h, compositeBufferOver, rs),
                     compositeBufferOver);
    }
    m_belowAcc = std::move(st.acc);
    m_clipBase = std::move(st.clipBase);
    m_haveClipBase = st.haveClipBase;
    m_anyClips = st.anyClips;

    // A group target's children are frozen during the drag — only its own transform moves.
    // Cache renderLayer's group path up to (not including) the final transformImageF.
    if (m_targetIsGroup) {
        const auto& group = *moved.as<core::GroupLayer>();
        // Cache the children over the group's FULL content bounds. Unlike renderLayer's per-frame
        // visible-region extent (which shifts as the group moves), the content bounds are
        // transform-independent, so the cached buffer stays valid for every frame; the replay
        // re-places it through the live transform plus this offset. A LINKED mask folds onto the
        // cached buffer in group-local space (the S31 group fold: 1 mask px per local unit through
        // the buffer offset) -- it rides the transform, so the folded buffer stays valid too.
        // (An unlinked mask was refused above.)
        const std::optional<common::Rect> content = group.contentBounds();
        if (!content || content->empty()) {
            m_groupLocalOX = 0;
            m_groupLocalOY = 0;
            m_groupLocal = ImageF{};
        } else {
            const long ox = static_cast<long>(std::floor(content->x));
            const long oy = static_cast<long>(std::floor(content->y));
            const long gw = static_cast<long>(std::ceil(content->right())) - ox;
            const long gh = static_cast<long>(std::ceil(content->bottom())) - oy;
            if (gw <= 0 || gh <= 0 || gw > static_cast<long>(kMaxGroupBuffer) ||
                gh > static_cast<long>(kMaxGroupBuffer))
                return false; // pathological extent: leave it to the full composite
            m_groupLocalOX = ox;
            m_groupLocalOY = oy;
            m_groupLocal = compositeChildren(
                group,
                common::Affine2D::translation(-static_cast<double>(ox),
                                              -static_cast<double>(oy)),
                static_cast<std::uint32_t>(gw), static_cast<std::uint32_t>(gh),
                compositeBufferOver, rs,
                common::Rect{static_cast<double>(ox), static_cast<double>(oy),
                             static_cast<double>(gw), static_cast<double>(gh)});
            if (const core::RasterMask* mk = group.mask();
                mk != nullptr && mk->enabled && !mk->empty() && mk->linked) {
                // buffer -> group-local -> mask px, exactly as renderLayerRaw folds it.
                if (const std::optional<common::Affine2D> mInv =
                        core::maskPlacement(group, *mk).inverse())
                    foldMaskThrough(m_groupLocal, *mk,
                                    *mInv * common::Affine2D::translation(
                                                static_cast<double>(ox), static_cast<double>(oy)));
            }
        }
    }

    // The doc-space raster of each child above the target. nullopt = skipped (invisible) or
    // an adjustment, both replayed live from the layer itself.
    for (std::size_t j = idx + 1; j < root.childCount(); ++j) {
        const core::Layer& layer = root.child(j);
        if (!layer.visible() || layer.opacity() <= 0.0f ||
            layer.as<core::AdjustmentLayer>() != nullptr)
            m_above.emplace_back(std::nullopt);
        else
            m_above.emplace_back(
                renderLayer(layer, common::Affine2D::identity(), w, h, compositeBufferOver, rs));
    }

    m_target = target;
    m_targetIndex = idx;
    m_childCount = root.childCount();
    m_width = w;
    m_height = h;
    m_filter = opts.resampleFilter;
    m_liveDrag = opts.liveDrag;
    m_valid = true;
    return true;
}

std::optional<common::Image> DragCompositeCache::composite(const core::Document& doc,
                                                           core::LayerId target,
                                                           const CompositeOptions& opts) {
    // The cache freezes the below/above rasters for the whole gesture, but a live partition's
    // residual renders differently depending on whether the partition still holds -- and dragging
    // either half breaks it on the first moved frame, mid-gesture. Rather than teach the cache to
    // notice, decline the fast path per frame while any partition is live: they exist only in the
    // moments right after a cut + paste in place, and the first drag frame retires them.
    if (documentHasLivePartition(doc)) return std::nullopt;
    // The cache's below/above buffers are built for the WHOLE stack, so it has no way to express
    // "leave one layer out" without a second set of them. Decline; the caller's fallback is the
    // full walk, which honours skipLayer.
    if (opts.skipLayer != core::kInvalidLayerId) return std::nullopt;
    if (!matches(doc, target, opts) && !rebuild(doc, target, opts)) return std::nullopt;
    const core::GroupLayer& root = doc.root();

    GroupWalk st;
    st.acc = std::move(m_replayAcc);  // reuse last frame's allocation (moved back below)
    st.acc = m_belowAcc;              // copy: the replay blends on top
    st.clipBase = m_clipBase;
    st.haveClipBase = m_haveClipBase;
    st.anyClips = m_anyClips;
    st.maskDomain = {0.0, 0.0, static_cast<double>(m_width), static_cast<double>(m_height)};
    st.liveDrag = true;  // drag-frame replay: heavy blur kernels draft-subsample (§3)

    // The moved layer: the only fresh rasterisation of the frame (renderLayer's leaf/group
    // semantics, with the live transform).
    const core::Layer& moved = root.child(m_targetIndex);
    if (moved.visible() && moved.opacity() > 0.0f) {
        if (m_targetIsGroup) {
            // Place the cached local buffer through the live group transform, accounting for the
            // buffer's origin offset in group-local space (zero for the masked window-aligned case).
            const common::Affine2D place =
                moved.transform() * common::Affine2D::translation(
                                        static_cast<double>(m_groupLocalOX),
                                        static_cast<double>(m_groupLocalOY));
            m_replaySrc = transformImageF(m_groupLocal, place, m_width, m_height,
                                          resolveFilter(m_filter, place, m_liveDrag));
        } else if (const auto* raster = moved.as<core::RasterLayer>()) {
            rasteriseLayerInto(m_replaySrc, raster->image(), moved.mask(), moved.transform(),
                               m_width, m_height,
                               resolveFilter(m_filter, moved.transform(), m_liveDrag));
        } else if (const auto* magic = moved.as<core::MagicLayer>()) {
            rasteriseLayerInto(m_replaySrc, magic->source(), moved.mask(), moved.transform(),
                               m_width, m_height,
                               resolveFilter(m_filter, moved.transform(), m_liveDrag));
        } else if (const auto* tlayer = moved.as<core::TextLayer>()) {
            // Text: place the (frozen, item-8) cached pixels through the live transform, folding the
            // cache's image->layer map in front -- byte-identical to renderLayer's leaf text path so
            // the drag preview matches the committed composite. An empty block has no cache (timg
            // null) -> an empty src, which rasteriseLayerInto clears to transparent.
            const common::Image* timg = tlayer->cachedImage();
            const common::Image empty;
            const common::Affine2D place = moved.transform() * tlayer->cacheImageToLayer();
            rasteriseLayerInto(m_replaySrc, timg != nullptr ? *timg : empty, moved.mask(), place,
                               m_width, m_height, resolveFilter(m_filter, place, m_liveDrag));
        } else if (const auto* xlayer = moved.as<core::TextureLayer>()) {
            // Texture: same replay as text -- place the generator cache (either lane) through the
            // live transform, byte-identical to renderLayerRaw's texture arm.
            const common::Affine2D place = moved.transform() * xlayer->cacheImageToLayer();
            if (const common::ImageF* fimg = xlayer->cachedImageF()) {
                rasteriseLayerInto(m_replaySrc, *fimg, moved.mask(), place, m_width, m_height,
                                   resolveFilter(m_filter, place, m_liveDrag));
            } else {
                const common::Image* ximg = xlayer->cachedImage();
                const common::Image empty;
                rasteriseLayerInto(m_replaySrc, ximg != nullptr ? *ximg : empty, moved.mask(),
                                   place, m_width, m_height,
                                   resolveFilter(m_filter, place, m_liveDrag));
            }
        } else {
            invalidate();  // the target's kind changed under us (rebuild filtered these)
            return std::nullopt;
        }
        walkStep(st, moved, m_replaySrc, compositeBufferOver);
    }

    // The layers above, replayed in stack order: blend modes, clip-to-below and adjustments
    // run live against the accumulator (exact for every mode — the moved layer below them may
    // have changed the backdrop AND the clip base); only the rasters come from the cache.
    for (std::size_t j = m_targetIndex + 1; j < root.childCount(); ++j) {
        const core::Layer& layer = root.child(j);
        if (!layer.visible() || layer.opacity() <= 0.0f) continue;
        if (layer.as<core::AdjustmentLayer>() != nullptr) {
            walkStep(st, layer, kNoSrc, compositeBufferOver);
            continue;
        }
        const std::optional<common::ImageF>& cached = m_above[j - m_targetIndex - 1];
        if (!cached) {  // a skipped child became visible without an invalidation: stale
            invalidate();
            return std::nullopt;
        }
        walkStep(st, layer, *cached, compositeBufferOver);
    }

    if (opts.checkerboard) flattenOverCheckerboard(st.acc, opts);
    common::Image out = toImage8Parallel(st.acc);
    m_replayAcc = std::move(st.acc);  // keep the allocation for the next frame
    return out;
}

std::unique_ptr<core::Document> makeCompositorDemo() {
    auto doc = std::make_unique<core::Document>(64, 64);
    core::GroupLayer& root = doc->root();

    // Bottom: an opaque blue square with a 4px transparent border (so the checkerboard shows).
    auto base = doc->makeRaster("Base");
    fillRect(base->image(), 4, 4, 56, 56, {40, 80, 200, 255});
    root.addOnTop(std::move(base));

    // A red square that multiplies into the blue.
    auto red = doc->makeRaster("Red x Multiply");
    fillRect(red->image(), 16, 16, 32, 32, {220, 60, 60, 255});
    red->setBlendMode(core::BlendMode::Multiply);
    root.addOnTop(std::move(red));

    // A green square that screens in at half opacity.
    auto green = doc->makeRaster("Green Screen 50%");
    fillRect(green->image(), 28, 28, 28, 28, {60, 200, 80, 255});
    green->setBlendMode(core::BlendMode::Screen);
    green->setOpacity(0.5f);
    root.addOnTop(std::move(green));

    // A global Invert adjustment on top: inverts the whole composite below it.
    root.addOnTop(doc->makeAdjustment("Invert", core::AdjustmentKind::Invert));
    return doc;
}

}  // namespace mosaic::render
