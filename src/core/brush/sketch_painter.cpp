#include "core/brush/sketch_painter.hpp"

#include "core/brush/brush_tip.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core::brush {

namespace {

// The reference scales each channel of the paint colour by its OWN random draw and rounds the
// result back to 8-bit through a QColor, which is `qRound(channelF * random * 255)` -- i.e. the
// channel byte times the draw. Alpha is not touched: the line's opacity is a separate step.
//
// ⚠ The three draws are pulled into named locals in the reference with a comment explaining why
// (argument evaluation order is unspecified in C++); the same reason applies here, and the ORDER
// r, g, b is part of the replay contract.
[[nodiscard]] common::Color8 randomRgbColor(common::Color8 base, StrokeState& state) {
    const double r1 = state.nextRandom();
    const double r2 = state.nextRandom();
    const double r3 = state.nextRandom();
    const auto q = [](double channel, double f) noexcept -> std::uint8_t {
        const long v = std::lround(channel * f);
        return static_cast<std::uint8_t>(std::clamp<long>(v, 0, 255));
    };
    return common::Color8{q(base.r, r1), q(base.g, r2), q(base.b, r3), base.a};
}

} // namespace

void SketchPainter::begin(const StrokePainterContext& ctx) {
    m_options = ctx.options;
    m_paintColor = ctx.color;
    m_lineColor = ctx.color;
    m_lineOpacity = 1.0;
    m_count = 0;
    m_radius = 0.0;
    m_pixels.clear();

    // The tip's own extents at the preset's authored size -- the same geometry a stamped dab of this
    // preset would fill, which is exactly what the reference reads off its brush (`m_brush->width()`
    // / `height()`, the mask's natural dimensions). A tipless brush falls back to the analytic
    // circle's envelope, as every other extents reader in the engine does.
    m_tipWidth = ctx.diameter;
    m_tipHeight = ctx.diameter * ctx.ratio;
    if (ctx.tip != nullptr) {
        const DabShape s = tipDabShape(*ctx.tip, 0, ctx.diameter, ctx.ratio, 0.0, false, false);
        m_tipWidth = s.width;
        m_tipHeight = s.height;
    }

    // ⚠ THE PRESS POINT IS PART OF THE HISTORY. Upstream reaches it through `paintAt()`, which
    // forwards the press to the same routine with both ends of the segment at the same place: the
    // point is appended and its own zero-length self-connection is drawn (a single pixel at most,
    // since a thick line whose ends share a pixel draws nothing). Mosaic seeds the history here and
    // draws nothing at the press -- the one-pixel difference is recorded in §6.6g rather than
    // reproduced through a degenerate span.
    m_points.clear();
    m_points.push_back(ctx.first.pos);
}

void SketchPainter::drawConnection(StrokeCanvas& canvas, common::Vec2 start, common::Vec2 end,
                                   double lineWidth) {
    const LineClip clip{0, 0, canvas.width(), canvas.height()};
    // The reference picks the rasterizer off the width and the anti-aliasing flag: a 1 px line goes
    // through a dedicated 1 px walk (hard DDA, or Wu when antialiased), anything wider through the
    // distance-field thick line.
    //
    // ⚠ THE ANTIALIASED 1 px CASE IS NOT TRANSCRIBED. Upstream draws it with a Wu line; Mosaic draws
    // the hard DDA line there, and the importer BADGES any preset that authors `Sketch/antiAliasing`
    // for exactly this branch. Neither shipped preset does.
    if (lineWidth == 1.0)
        rasterizeDdaLine(start, end, clip, m_pixels);
    else
        rasterizeThickLine(start, end, lineWidth, m_params.antiAliasing, clip, m_pixels);

    for (const LinePixel& p : m_pixels)
        canvas.plot(p.x, p.y, m_lineOpacity * p.weight, m_lineColor);
}

void SketchPainter::paintSpan(StrokeCanvas& canvas, const StrokeSnapshot& a, const StrokeSnapshot& b,
                              StrokeState& state) {
    const common::Vec2 prevPos = a.sample.pos;
    const common::Vec2 pos = b.sample.pos;

    // ⚠ APPENDED BEFORE THE EARLY RETURN BELOW, and only the span's END point is appended -- the
    // history is one point per span, not two, or every point would be in it twice.
    m_points.push_back(pos);

    // An absent option table is "no options at all": every one of the four reads exactly 1.0, the
    // same identity an unchecked option contributes (`standardOptionValue`).
    const auto value = [&](const std::optional<CurveOption>* opt) {
        return opt != nullptr ? standardOptionValue(*opt, state) : 1.0;
    };

    // The four per-span option values, in the reference's evaluation order -- each may draw from the
    // stroke's random streams, so the order is part of the replay contract.
    const double scale = value(m_options != nullptr ? &m_options->size : nullptr);
    // A dab too small to rasterize paints nothing AND does not advance the span counter: the
    // reference returns before its `m_count++`, so the next span still latches the radius.
    if (scale * m_tipWidth <= 0.01 || scale * m_tipHeight <= 0.01)
        return;

    const double lineWidth = std::max(
        0.9, value(m_options != nullptr ? &m_options->lineWidth : nullptr) * m_params.lineWidth);
    const double offsetScale =
        value(m_options != nullptr ? &m_options->offsetScale : nullptr) * m_params.offset * 0.01;
    const double currentProbability =
        value(m_options != nullptr ? &m_options->density : nullptr) * m_params.probability;

    // The segment itself, at whatever opacity and colour the PREVIOUS span's last connection left
    // behind (see the header). "Shaded" style turns this off and draws only the web.
    if (m_params.makeConnection)
        drawConnection(canvas, prevPos, pos, lineWidth);

    // ⚠ THE RADIUS IS LATCHED ON THE FIRST PAINTED SPAN and never re-read: the reference measures it
    // once, at `m_count == 0`, from the brush's natural size, and every later span scales THAT by
    // its own `Size` value. A stroke whose size option fades therefore shrinks its reach without
    // re-measuring the tip.
    if (m_count == 0)
        m_radius = 0.5 * std::max(m_tipWidth, m_tipHeight);

    // The connection test is a squared distance against a squared radius -- no square roots in the
    // loop, which is why every distance below is squared too.
    const double thresholdDistance = (m_radius * scale) * (m_radius * scale);
    const double density = thresholdDistance * currentProbability;
    double probability = 1.0 - currentProbability;

    const std::size_t size = m_points.size();
    for (std::size_t i = 0; i < size; ++i) {
        const common::Vec2 diff = m_points[i] - pos;
        const double distance = diff.x * diff.x + diff.y * diff.y;

        // `simpleMode`: a circle test against the latched radius. ⚠ The MASK test -- the other
        // branch, in which a candidate point must land on an opaque pixel of the rasterized tip
        // centred on the new point -- is NOT transcribed; the importer badges a preset that asks
        // for it and it falls back to this circle, whose radius is the same measurement.
        if (!(distance < thresholdDistance))
            continue;

        // The density falls off with distance when asked to: a far point is drawn rarely, a near
        // one almost always. (`density` is 0 when the probability is -- then this is inf or NaN,
        // the comparison below fails, and nothing is drawn, which is the reference's behaviour for
        // a zero-probability sketch.)
        if (m_params.distanceDensity)
            probability = distance / density;

        if (!(state.nextRandom() >= probability))
            continue;

        const common::Vec2 offsetPt = diff * offsetScale;

        // ⚠ BOTH OF THESE OUTLIVE THE LOOP ITERATION, and the reference never puts them back --
        // they are the painter's state from here on, including for the NEXT span's segment line.
        if (m_params.randomRgb)
            m_lineColor = randomRgbColor(m_paintColor, state);

        double opacity = 1.0;
        if (m_params.distanceOpacity) {
            // ⚠ ROUNDED, so this is a hard cut and not a fade: the argument is in (0,1], so the
            // connection is either fully opaque (within half the threshold) or invisible. The
            // reference's own comment questions the rounding; the behaviour is what ships.
            opacity *= std::round(1.0 - distance / thresholdDistance);
        }
        if (m_params.randomOpacity)
            opacity *= state.nextRandom();
        m_lineOpacity = opacity;

        // `magnetify` draws the connection between the two POINTS, pulled toward each other by the
        // offset; without it the connection is a short stroke centred on the NEW point, along the
        // direction of the candidate -- a tuft rather than a web.
        if (m_params.magnetify)
            drawConnection(canvas, pos + offsetPt, m_points[i] - offsetPt, lineWidth);
        else
            drawConnection(canvas, pos + offsetPt, pos - offsetPt, lineWidth);
    }

    ++m_count;
}

} // namespace mosaic::core::brush
