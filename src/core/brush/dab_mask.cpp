#include "core/brush/dab_mask.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core::brush {

namespace {

// A dab corner beyond this is off any conceivable document. Clamping to it as a *double*, before the
// integer conversion, is the point: `static_cast<int64_t>(floor(1e300))` is undefined behaviour
// rather than a big number, and a preset is an untrusted file.
constexpr double kMaxCorner = 1 << 24;

// A shape whose extents or angle are not finite paints nothing. Every caller path already clamps,
// but a preset is an untrusted file and `scale` has no bound in any of the formats.
[[nodiscard]] bool shapeIsSane(const DabShape& shape) noexcept {
    return std::isfinite(shape.width) && std::isfinite(shape.height) &&
           std::isfinite(shape.angleRad) && shape.width > 0.0 && shape.height > 0.0;
}

// `cos(pi/2)` is 6.1e-17, not 0. So a 24 x 12 tip turned a quarter turn has a computed extent of
// 12.0000000000000015, and `ceil` hands it a thirteenth column of very nearly nothing -- an
// off-by-one mask for the commonest angle there is, and one that would then disagree with a mask
// rendered at an angle of exactly 0 on a tip that had been authored rotated. Snap the dust away
// before rounding. Nothing meaningful in a dab lives within a nanopixel of an integer.
constexpr double kExtentSnap = 1e-9;

[[nodiscard]] double snapToInteger(double v) noexcept {
    const double r = std::round(v);
    return std::abs(v - r) < kExtentSnap ? r : v;
}

} // namespace

namespace detail {

std::uint32_t maskSpan(double extent, double sub) noexcept {
    if (!(extent > 0.0) || !std::isfinite(extent))
        return 0;
    const double span = std::ceil(extent + sub);
    if (!(span >= 1.0))
        return 0;
    // `extent` is already bounded by kMaxDabExtent, and a sub-pixel phase can only add one pixel to
    // it -- so this admits that pixel rather than rejecting a legal dab that sits at a phase.
    if (span > kMaxDabExtent + 1.0)
        return 0;
    return static_cast<std::uint32_t>(span);
}

TipFrameMap tipFrameMap(const DabShape& shape, const DabExtent& ext, double subX,
                        double subY) noexcept {
    TipFrameMap m;
    m.halfW = 0.5 * ext.width;
    m.halfH = 0.5 * ext.height;
    m.subX = subX;
    m.subY = subY;
    m.cosT = std::cos(shape.angleRad);
    m.sinT = std::sin(shape.angleRad);
    m.mirrorH = shape.mirrorH;
    m.mirrorV = shape.mirrorV;
    return m;
}

} // namespace detail

DabExtent dabExtent(const DabShape& shape) noexcept {
    if (!shapeIsSane(shape))
        return {};
    const double c = std::abs(std::cos(shape.angleRad));
    const double s = std::abs(std::sin(shape.angleRad));
    DabExtent e;
    e.width = snapToInteger(shape.width * c + shape.height * s);
    e.height = snapToInteger(shape.width * s + shape.height * c);
    if (e.width > kMaxDabExtent || e.height > kMaxDabExtent)
        return {};
    return e;
}

DabPlacement placeDab(const DabShape& shape, double centerX, double centerY,
                      int subPixelSteps) noexcept {
    DabPlacement p;
    const DabExtent ext = dabExtent(shape);
    if (ext.empty() || !std::isfinite(centerX) || !std::isfinite(centerY))
        return p;

    const int steps = std::max(1, subPixelSteps);
    const double topLeftX = centerX - 0.5 * ext.width;
    const double topLeftY = centerY - 0.5 * ext.height;

    // Split into an integer corner and a fractional phase, then snap the phase to the nearest bin.
    // A phase that rounds up to a whole pixel is carried into the corner rather than left as 1.0 --
    // that keeps the phase half-open (a `subX == 1.0` would render a mask one pixel too wide) and
    // keeps the worst-case placement error at half a bin instead of a whole one.
    const auto split = [steps](double v, std::int32_t& whole, double& sub) noexcept {
        const double f = std::floor(v);
        long bin = std::lround((v - f) * steps);
        double w = f;
        if (bin >= steps) {
            bin = 0;
            w += 1.0;
        }
        whole = static_cast<std::int32_t>(std::clamp(w, -kMaxCorner, kMaxCorner));
        sub = static_cast<double>(bin) / steps;
    };
    split(topLeftX, p.x, p.subX);
    split(topLeftY, p.y, p.subY);

    p.width = detail::maskSpan(ext.width, p.subX);
    p.height = detail::maskSpan(ext.height, p.subY);
    if (p.width == 0 || p.height == 0) {
        p = DabPlacement{};
    }
    return p;
}

DabShape shapeOf(const MaskGenerator& gen, double angleRad, bool mirrorH, bool mirrorV) noexcept {
    DabShape s;
    s.width = gen.width();
    s.height = gen.height();
    s.angleRad = angleRad;
    s.mirrorH = mirrorH;
    s.mirrorV = mirrorV;
    return s;
}

DabMask renderDabMask(const MaskGenerator& gen, double angleRad, bool mirrorH, bool mirrorV,
                      double subX, double subY) {
    DabMask mask;
    if (gen.isEmpty())
        return mask;

    subX = detail::normalizePhase(subX);
    subY = detail::normalizePhase(subY);

    const DabShape shape = shapeOf(gen, angleRad, mirrorH, mirrorV);
    const DabExtent ext = dabExtent(shape);
    if (ext.empty())
        return mask;

    mask.width = detail::maskSpan(ext.width, subX);
    mask.height = detail::maskSpan(ext.height, subY);
    if (mask.empty()) {
        mask = DabMask{};
        return mask;
    }
    mask.coverage.resize(static_cast<std::size_t>(mask.width) * mask.height, 0);

    // The generator's frame is the unrotated tip, centred on the origin, so undoing the dab's
    // rotation is the whole of the work. Mirroring a procedural tip is very nearly a no-op -- the six
    // generators are symmetric about both axes unless `spikes > 2` -- but it costs two negations and
    // keeping it here means the bitmap and procedural paths agree on what a mirrored dab means.
    const detail::TipFrameMap toTip = detail::tipFrameMap(shape, ext, subX, subY);
    std::size_t i = 0;
    for (std::uint32_t py = 0; py < mask.height; ++py) {
        for (std::uint32_t px = 0; px < mask.width; ++px, ++i) {
            double tx = 0.0;
            double ty = 0.0;
            toTip(px, py, tx, ty);
            mask.coverage[i] = detail::quantizeCoverage(gen.coverageAt(tx, ty));
        }
    }
    return mask;
}

} // namespace mosaic::core::brush
