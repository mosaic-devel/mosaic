#include "core/brush/curve_painter.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core::brush {

void CurvePainter::begin(const StrokePainterContext& ctx) {
    m_options = ctx.options;
    m_color = ctx.color;
    m_points.clear();
    m_poly.clear();
    m_pixels.clear();
    // ⚠ The press point is NOT seeded. The reference's curve paintop paints nothing at `paintAt`
    // (it only reports a spacing), so its history starts empty and its first point is the first
    // segment's end -- unlike the sketch engine, whose `paintAt` does forward to its own routine.
}

void CurvePainter::strokePolyline(StrokeCanvas& canvas, const std::vector<common::Vec2>& poly,
                                  double lineWidth, double opacity) {
    if (poly.size() < 2 || !(opacity > 0.0))
        return;

    // The path's bounding box, grown by the pen's half width plus a margin for the rasterizer's
    // own one-pixel band.
    double lo = poly[0].x;
    double hi = poly[0].x;
    double top = poly[0].y;
    double bot = poly[0].y;
    for (const common::Vec2& p : poly) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y))
            return;
        lo = std::min(lo, p.x);
        hi = std::max(hi, p.x);
        top = std::min(top, p.y);
        bot = std::max(bot, p.y);
    }
    const double pad = 0.5 * std::abs(lineWidth) + 3.0;
    const auto floorInt = [](double v) {
        return static_cast<int>(std::floor(std::clamp(v, -1.0e9, 1.0e9)));
    };
    // A path wholly off the document still costs nothing but is not an error: reset() says so and
    // the plots below simply land nowhere.
    if (!m_scratch.reset(floorInt(lo - pad), floorInt(top - pad), floorInt(hi + pad) + 2,
                         floorInt(bot + pad) + 2, canvas))
        return;

    const LineClip clip{m_scratch.originX(), m_scratch.originY(),
                        m_scratch.originX() + m_scratch.width(),
                        m_scratch.originY() + m_scratch.height()};
    for (std::size_t i = 0; i + 1 < poly.size(); ++i) {
        // ⚠ A ONE PIXEL pen still goes through the thick line, not the DDA walk: the reference
        // hands Qt a pen of that width, and its 1 px path stroke is a filled band, not a Bresenham
        // walk. (The sketch engine's `lineWidth == 1` special case is the sketch paintop's own,
        // not a property of drawing lines.)
        rasterizeThickLine(poly[i], poly[i + 1], lineWidth, /*antialias=*/true, clip, m_pixels);
        for (const LinePixel& p : m_pixels)
            m_scratch.plot(p.x, p.y, static_cast<int>(std::lround(p.weight * 255.0)),
                           ScratchMode::Max);
    }

    // One mask, one opacity: the whole stroked path deposits at this path's opacity, exactly as the
    // reference blits its temporary once at the opacity it set on its painter.
    m_scratch.flush(canvas, m_color, opacity);
}

void CurvePainter::paintSpan(StrokeCanvas& canvas, const StrokeSnapshot& a, const StrokeSnapshot& b,
                             StrokeState& state) {
    // ⚠ The history size is floored at 1. The reference reads it with no default at all, so a
    // preset missing the key gives 0 -- and its own path build then reads the front of an empty
    // list. A zero window is not a shape.
    const auto maxPoints = static_cast<std::size_t>(std::max(1, m_params.strokeHistorySize));

    m_points.push_back(b.sample.pos);
    while (m_points.size() > maxPoints)
        m_points.pop_front();

    // An absent option table is "no options at all": each of the two reads exactly 1.0, the same
    // identity an unchecked option contributes.
    const auto value = [&](const std::optional<CurveOption>* opt) {
        return opt != nullptr ? standardOptionValue(*opt, state) : 1.0;
    };

    // The two per-span option values, in the reference's order. Both read through
    // `KisStandardOption::apply`, so an unchecked one contributes exactly 1.0. ⚠ Their consumer
    // range upstream is [0.1, 1] here and [0, 1] on the sketch paintop -- ONE base name, two
    // ranges. Mosaic's option table has one spec per base and keeps the wider [0,1]; neither
    // shipped preset checks either option, so the floor is inert. §6.6g records it.
    const double lineWidth =
        value(m_options != nullptr ? &m_options->lineWidth : nullptr) * m_params.lineWidth;

    // The segment itself, at FULL opacity -- the reference draws it before setting any opacity on
    // its painter, and restores the painter to unit opacity after the curve.
    if (m_params.makeConnection) {
        m_poly.clear();
        m_poly.push_back(a.sample.pos);
        m_poly.push_back(b.sample.pos);
        strokePolyline(canvas, m_poly, lineWidth, 1.0);
    }

    // ⚠ NOTHING IS DRAWN UNTIL THE WINDOW IS FULL. A short stroke that never fills the history
    // lays only its connection lines -- which is the reference's behaviour and is why a curve
    // preset with a long history feels like it "catches up" after the first few samples.
    if (m_points.size() < maxPoints)
        return;

    const double curvesOpacity =
        value(m_options != nullptr ? &m_options->curvesOpacity : nullptr) * m_params.curvesOpacity;

    m_poly.clear();
    m_poly.push_back(m_points.front());
    if (m_params.smoothing) {
        // A quadratic whose single control point is the MIDDLE of the window.
        flattenQuadratic(m_points.front(), m_points[maxPoints / 2], m_points.back(), m_poly);
    } else {
        // A cubic whose controls are at one third and two thirds of the window. ⚠ The reference
        // indexes `step` and `step + step` with `step = maxPoints / 3`, which for a window of 30 is
        // 10 and 20 -- integer division, and the endpoint is the window's LAST element rather than
        // index `3*step`, so an unevenly divisible window leans on its own tail.
        const std::size_t step = maxPoints / 3;
        flattenCubic(m_points.front(), m_points[step], m_points[std::min(step + step, maxPoints - 1)],
                     m_points.back(), m_poly);
    }
    strokePolyline(canvas, m_poly, lineWidth, curvesOpacity);
}

} // namespace mosaic::core::brush
