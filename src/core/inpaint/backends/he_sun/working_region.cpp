#include "core/inpaint/backends/he_sun/working_region.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace mosaic::core::inpaint {

namespace {
[[nodiscard]] long ceilDiv(long a, long b) { return (a + b - 1) / b; }
}  // namespace

common::Rect workingRegionRect(std::uint32_t imageW, std::uint32_t imageH,
                               const std::optional<common::Rect>& holeBounds, const Params& p) {
    const long W = static_cast<long>(imageW);
    const long H = static_cast<long>(imageH);
    if (W == 0 || H == 0)
        return {};
    // ⚠⚠ READ THIS BEFORE TOUCHING THIS FUNCTION.
    // The window below is sized from the SELECTION (mult x the hole's bbox, centred on it, then
    // scaled to fit maxRegionW/H), and the offset search runs ONLY inside it. Whether the search
    // domain is derived from the selection or is invariant to it is a LOAD-BEARING property of this
    // engine, and `globalSearchRegion` exists solely to make it invariant. That flag is not an
    // optimisation toggle and must not be removed as an unused one.
    //
    // ⚠ Note the project record once asserted the opposite of what this code does — that the donor
    // search was "the entire known region, invariant to hole size". That was true of SYNTHESIS,
    // which copies from pixel+offset anywhere, and false of the offset DISCOVERY performed here.
    // Two different stages. Corrected 2026-07-12; do not re-derive the old claim from this comment.
    //
    // A tighter search window is the single most obvious speedup for a slow matcher, so narrowing
    // it WILL be proposed again. It buys nothing: the search cost is capped by nnfMaxPatches, NOT
    // by the region's size.
    if (p.globalSearchRegion) {
        return {0.0, 0.0, static_cast<double>(W), static_cast<double>(H)};
    }
    // Hole bounding box (whole image when the mask has no coverage).
    long bx = 0, by = 0, bw = W, bh = H;
    if (holeBounds) {
        bx = static_cast<long>(holeBounds->x);
        by = static_cast<long>(holeBounds->y);
        bw = static_cast<long>(holeBounds->w);
        bh = static_cast<long>(holeBounds->h);
    }
    // Margin multiplier around the hole bbox (centre-grown, clamped to the image). The default 3x
    // gives ~200% surrounding context. With "low-effort on small selection" a SMALL hole uses a much
    // tighter margin (down to ~1.4x = a ~20% surround) so it samples only its immediate
    // neighbourhood -- far less to analyse -- growing back to 3x as the hole approaches a third of
    // the image, where the full context matters. Bounds are handled by the clamp below (an
    // edge-touching hole shifts its window inward, keeping the requested size).
    double mult = 3.0;
    if (p.adaptiveSmallRegion) {
        const double holeFrac = std::max(static_cast<double>(bw) / W, static_cast<double>(bh) / H);
        mult = std::clamp(1.4 + (3.0 - 1.4) * (holeFrac / 0.33), 1.4, 3.0);
    }
    const long rw = std::min(W, std::max(bw, std::lround(static_cast<double>(bw) * mult)));
    const long rh = std::min(H, std::max(bh, std::lround(static_cast<double>(bh) * mult)));
    const long ox = std::clamp(bx + bw / 2 - rw / 2, 0L, W - rw);
    const long oy = std::clamp(by + bh / 2 - rh / 2, 0L, H - rh);
    return {static_cast<double>(ox), static_cast<double>(oy), static_cast<double>(rw),
            static_cast<double>(rh)};
}

WorkingRegion extractWorkingRegion(const common::ImageF& image, const Selection& holeMask,
                                   const Params& p) {
    WorkingRegion wr;
    const long W = static_cast<long>(image.width);
    const long H = static_cast<long>(image.height);
    if (W == 0 || H == 0) {
        return wr;  // empty image -> empty region
    }

    const common::Rect rr = workingRegionRect(image.width, image.height, holeMask.bounds(), p);
    const long ox = static_cast<long>(rr.x);
    const long oy = static_cast<long>(rr.y);
    const long rw = static_cast<long>(rr.w);
    const long rh = static_cast<long>(rr.h);

    // Downsample factor so the region fits the budget.
    const long sx = ceilDiv(rw, std::max(1, p.maxRegionW));
    const long sy = ceilDiv(rh, std::max(1, p.maxRegionH));
    const int scale = static_cast<int>(std::max(1L, std::max(sx, sy)));

    const std::uint32_t outW = static_cast<std::uint32_t>(ceilDiv(rw, scale));
    const std::uint32_t outH = static_cast<std::uint32_t>(ceilDiv(rh, scale));
    common::ImageF out(outW, outH);
    for (std::uint32_t oyi = 0; oyi < outH; ++oyi) {
        for (std::uint32_t oxi = 0; oxi < outW; ++oxi) {
            double r = 0, g = 0, b = 0, a = 0;
            int n = 0;
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    const long sxp = ox + static_cast<long>(oxi) * scale + dx;
                    const long syp = oy + static_cast<long>(oyi) * scale + dy;
                    if (sxp < ox + rw && syp < oy + rh && sxp < W && syp < H) {
                        const common::ColorF c =
                            image.at(static_cast<std::uint32_t>(sxp), static_cast<std::uint32_t>(syp));
                        r += c.r;
                        g += c.g;
                        b += c.b;
                        a += c.a;
                        ++n;
                    }
                }
            }
            if (n > 0) {
                const double inv = 1.0 / n;
                out.set(oxi, oyi,
                        {static_cast<float>(r * inv), static_cast<float>(g * inv),
                         static_cast<float>(b * inv), static_cast<float>(a * inv)});
            }
        }
    }

    wr.image = std::move(out);
    wr.scale = scale;
    wr.originX = static_cast<int>(ox);
    wr.originY = static_cast<int>(oy);
    wr.regionW = static_cast<int>(rw);
    wr.regionH = static_cast<int>(rh);
    return wr;
}

}  // namespace mosaic::core::inpaint
