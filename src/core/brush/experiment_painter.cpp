#include "core/brush/experiment_painter.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core::brush {

void ExperimentPainter::begin(const StrokePainterContext& ctx) {
    m_color = ctx.color;
    m_path.clear();
    m_firstRun = true;
    // ⚠ The press point is not seeded here. The reference's first `paintLine` moves to `pi1` and
    // lines to `pi2` -- so the path's first point is the FIRST SPAN's start, which is the press
    // point, and it is appended below rather than here. Seeding as well would double it, and a
    // doubled vertex is a zero-length edge that the winding rule counts twice.
}

void ExperimentPainter::paintSpan(StrokeCanvas& canvas, const StrokeSnapshot& a,
                                  const StrokeSnapshot& b, StrokeState& state) {
    (void)canvas; // nothing is laid until release
    (void)state;  // this engine draws nothing random

    if (m_path.size() >= kMaxExperimentPoints)
        return;
    if (m_firstRun) {
        m_firstRun = false;
        m_path.push_back(a.sample.pos);
    }
    m_path.push_back(b.sample.pos);
}

void ExperimentPainter::finish(StrokeCanvas& canvas) {
    if (m_path.size() < 3)
        return; // two points enclose no area; the reference's fill of a degenerate path lays nothing

    double lo = m_path[0].x;
    double hi = m_path[0].x;
    double top = m_path[0].y;
    double bot = m_path[0].y;
    for (const common::Vec2& p : m_path) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y))
            return;
        lo = std::min(lo, p.x);
        hi = std::max(hi, p.x);
        top = std::min(top, p.y);
        bot = std::max(bot, p.y);
    }
    const auto floorInt = [](double v) {
        return static_cast<int>(std::floor(std::clamp(v, -1.0e9, 1.0e9)));
    };
    if (!m_scratch.reset(floorInt(lo) - 1, floorInt(top) - 1, floorInt(hi) + 2, floorInt(bot) + 2,
                         canvas))
        return;

    // ⚠ `windingFill` is not decoration: the stroke IS a self-crossing scribble, and the two rules
    // disagree about every one of its crossings. Non-zero winding fills the whole enclosed blob;
    // the alternate rule punches the overlaps back out.
    //
    // ⚠ The edge is antialiased UNLESS `hardEdge`, which is the reference's own inversion
    // (`setAntiAliasPolygonFill(!m_hardEdge)`).
    fillPolygon(m_path, m_params.windingFill, !m_params.hardEdge, m_scratch);
    m_scratch.flush(canvas, m_color);
}

} // namespace mosaic::core::brush
