#include "core/layer_grow.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace mosaic::core {
namespace {

// Saturate a double into +-kMaxLayerCoord and THEN make it an integer. The order is the whole point:
// `static_cast<long>(x)` for an x outside the type's range is undefined behaviour (on x86-64 it
// happens to yield LONG_MIN, which is worse than a wrap because it is a valid-looking corner), so
// the comparison has to happen while the value is still a double.
long saturate(double v) noexcept {
    const double lo = -static_cast<double>(kMaxLayerCoord);
    const double hi = static_cast<double>(kMaxLayerCoord);
    if (v < lo)
        return -kMaxLayerCoord;
    if (v > hi)
        return kMaxLayerCoord;
    return static_cast<long>(v);
}

} // namespace

PixelBox pixelBoxCovering(const common::Rect& r) noexcept {
    // A near-singular placement maps the canvas to +-inf, and inf - inf is NaN; neither compares
    // usefully, and neither has an integer image. An empty box is the honest answer, and it is the
    // one the callers already handle ("no growth").
    if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.w) || !std::isfinite(r.h))
        return {};
    const long x0 = saturate(std::floor(r.x));
    const long y0 = saturate(std::floor(r.y));
    const long x1 = saturate(std::ceil(r.right()));
    const long y1 = saturate(std::ceil(r.bottom()));
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {x0, y0, x1, y1};
}

PixelBox canvasBoxInLayer(const common::Affine2D& layerToDoc, std::uint32_t docW,
                          std::uint32_t docH) {
    if (docW == 0 || docH == 0)
        return {};
    const std::optional<common::Affine2D> inv = layerToDoc.inverse();
    if (!inv)
        return {}; // singular placement: the canvas has no image in this layer's grid
    return pixelBoxCovering(
        inv->mapBounds(common::Rect{0.0, 0.0, static_cast<double>(docW),
                                    static_cast<double>(docH)}));
}

PixelBox brushGrowthBox(std::uint32_t layerW, std::uint32_t layerH, const PixelBox& requested,
                        const PixelBox& canvasBox) {
    const PixelBox layerBox = layerPixelBox(layerW, layerH);
    if (layerBox.empty() || canvasBox.empty())
        return layerBox; // nothing to grow, or nowhere bounded to grow to

    // THE CEILING: the layer as it stands, united with the canvas and not one pixel more. The union
    // (rather than the canvas alone) is what keeps growth from ever SHRINKING a layer that already
    // reaches past the canvas edge -- a pasted layer hanging off the side keeps every pixel it has.
    const PixelBox ceiling{std::min(layerBox.x0, canvasBox.x0), std::min(layerBox.y0, canvasBox.y0),
                           std::max(layerBox.x1, canvasBox.x1),
                           std::max(layerBox.y1, canvasBox.y1)};

    // ⚠ THE INTERSECTION IS THE GUARD, AND IT RUNS BEFORE ANY SIZE IS COMPUTED FROM `requested`.
    // Whatever came in -- empty, inside-out, 1e13 wide -- what comes out is a sub-box of the
    // ceiling, so every width, height and cell count below is bounded by the canvas by
    // construction. Clamping AFTER sizing would mean allocating from the absurd number first, which
    // is the exact failure this is here to prevent.
    const PixelBox want{std::max(requested.x0, ceiling.x0), std::max(requested.y0, ceiling.y0),
                        std::min(requested.x1, ceiling.x1), std::min(requested.y1, ceiling.y1)};
    if (want.empty())
        return layerBox; // the stroke asked for nothing inside the ceiling: no growth

    const PixelBox grown{std::min(layerBox.x0, want.x0), std::min(layerBox.y0, want.y0),
                         std::max(layerBox.x1, want.x1), std::max(layerBox.y1, want.y1)};

    // The last line of defence, and it is NOT redundant with the ceiling: the canvas's image in this
    // layer's grid can itself be enormous (a layer scaled down by 1e-4 turns a 4k canvas into 4e7 px
    // a side, and that box is perfectly "inside the canvas"). Refusing is the safe answer -- the
    // stroke simply clips at the layer edge, which is what it always did.
    if (grown.width() > kMaxLayerSide || grown.height() > kMaxLayerSide ||
        grown.cells() > kMaxLayerCells)
        return layerBox;
    return grown;
}

common::Vec2 clampStrokePos(common::Vec2 p) noexcept {
    const auto sat = [](double v) noexcept -> double {
        if (!std::isfinite(v))
            return 0.0; // a NaN position has no place on the path; the origin is at least defined
        const double lim = static_cast<double>(kMaxLayerCoord);
        return v < -lim ? -lim : (v > lim ? lim : v);
    };
    return {sat(p.x), sat(p.y)};
}

} // namespace mosaic::core
