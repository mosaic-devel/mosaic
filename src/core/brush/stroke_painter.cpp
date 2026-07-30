#include "core/brush/stroke_painter.hpp"

#include "core/brush/curve_painter.hpp"
#include "core/brush/experiment_painter.hpp"
#include "core/brush/hairy_painter.hpp"
#include "core/brush/particle_painter.hpp"
#include "core/brush/sketch_painter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace mosaic::core::brush {

namespace {

[[nodiscard]] bool finitePoint(common::Vec2 p) noexcept {
    return std::isfinite(p.x) && std::isfinite(p.y);
}

// The rasterizers' one emit point: the clip test and the cap live here so neither walk can forget
// either of them.
struct LineSink {
    const LineClip& clip;
    std::vector<LinePixel>& out;

    void emit(int x, int y, double weight) {
        if (out.size() >= kMaxLinePixels)
            return;
        if (x < clip.x0 || x >= clip.x1 || y < clip.y0 || y >= clip.y1)
            return;
        out.push_back(LinePixel{x, y, weight});
    }
};

} // namespace

double standardOptionValue(const std::optional<CurveOption>& opt, StrokeState& state) {
    if (!opt || !opt->isChecked())
        return 1.0;
    return opt->sizeLikeValue(state);
}

void rasterizeDdaLine(common::Vec2 start, common::Vec2 end, const LineClip& clip,
                      std::vector<LinePixel>& out) {
    out.clear();
    if (!finitePoint(start) || !finitePoint(end))
        return;

    LineSink sink{clip, out};

    int x = static_cast<int>(std::floor(start.x));
    int y = static_cast<int>(std::floor(start.y));
    const int x2 = static_cast<int>(std::floor(end.x));
    const int y2 = static_cast<int>(std::floor(end.y));
    const int xd = x2 - x;
    const int yd = y2 - y;

    // ⚠ `lockAxis` is load-bearing (see the header). A pure horizontal line leaves the gradient at
    // 0 with the flag SET; a pure vertical one sets it to 2 so the y branch is taken, and the flag
    // then zeroes the step so x never moves.
    // ⚠ FLOAT, not double, and that is the transcription: the reference accumulates this walk's
    // gradient in `float`, so a long line's minor coordinate rounds where its floats round.
    float m = 0.0f;
    bool lockAxis = true;
    if (xd == 0) {
        m = 2.0f;
    } else if (yd != 0) {
        lockAxis = false;
        m = static_cast<float>(yd) / static_cast<float>(xd);
    }

    // The float cursors start at the FLOORED corner, not at the sub-pixel endpoint: the reference
    // seeds them from its own integers, so a DDA line is a function of the two pixel corners alone.
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);

    sink.emit(x, y, 1.0);

    std::size_t steps = 0;
    if (std::fabs(m) > 1.0f) {
        const int inc = yd > 0 ? 1 : -1;
        m = lockAxis ? 0.0f : 1.0f / m;
        m *= static_cast<float>(inc);
        while (y != y2 && steps++ < kMaxLinePixels) {
            y += inc;
            fx += m;
            x = static_cast<int>(std::lround(fx));
            sink.emit(x, y, 1.0);
        }
    } else {
        const int inc = xd > 0 ? 1 : -1;
        m *= static_cast<float>(inc);
        while (x != x2 && steps++ < kMaxLinePixels) {
            x += inc;
            fy += m;
            y = static_cast<int>(std::lround(fy));
            sink.emit(x, y, 1.0);
        }
    }
}

void rasterizeThickLine(common::Vec2 start, common::Vec2 end, double width, bool antialias,
                        const LineClip& clip, std::vector<LinePixel>& out) {
    out.clear();
    if (!finitePoint(start) || !finitePoint(end) || !std::isfinite(width))
        return;

    int x1 = static_cast<int>(std::floor(start.x));
    int y1 = static_cast<int>(std::floor(start.y));
    int x2 = static_cast<int>(std::floor(end.x));
    int y2 = static_cast<int>(std::floor(end.y));
    // Two endpoints in the same pixel draw NOTHING. The reference returns here, and a sketch brush
    // connecting a point to itself hits this on every segment.
    if (x2 == x1 && y2 == y1)
        return;

    const int dstX = x2 - x1;
    const int dstY = y2 - y1;
    const double uniC = static_cast<double>(dstX) * y1 - static_cast<double>(dstY) * x1;
    const double projectionDenominator =
        1.0 / (static_cast<double>(dstX) * dstX + static_cast<double>(dstY) * dstY);

    // The sub-pixel term is read off the MAJOR axis only, and it widens the band rather than
    // shifting it -- the reference adds it straight into the half width.
    const double subPixel =
        std::abs(dstX) > std::abs(dstY) ? start.x - x1 : start.y - y1;
    const double halfWidth = width * 0.5 + subPixel;
    const int W = static_cast<int>(std::lround(halfWidth)) + 1;

    const int X1 = x1;
    const int Y1 = y1;
    const int X2 = x2;
    const int Y2 = y2;
    if (x2 < x1)
        std::swap(x1, x2);
    if (y2 < y1)
        std::swap(y1, y2);

    double denominator = std::sqrt(static_cast<double>(dstY) * dstY +
                                   static_cast<double>(dstX) * dstX);
    if (denominator == 0.0)
        denominator = 1.0;
    denominator = 1.0 / denominator;

    LineSink sink{clip, out};

    // The scan box is the reference's, INTERSECTED with the clip. That is not a deviation: the sink
    // drops everything outside the clip anyway, and an unclipped box over a long diagonal line is
    // quadratic in its length.
    const int sy0 = std::max(y1 - W, clip.y0);
    const int sy1 = std::min(y2 + W, clip.y1);
    const int sx0 = std::max(x1 - W, clip.x0);
    const int sx1 = std::min(x2 + W, clip.x1);

    for (int y = sy0; y < sy1; ++y) {
        for (int x = sx0; x < sx1; ++x) {
            const double projection =
                ((x - X1) * static_cast<double>(dstX) + (y - Y1) * static_cast<double>(dstY)) *
                projectionDenominator;
            const double scanX = X1 + projection * dstX;
            const double scanY = Y1 + projection * dstY;

            // Inside the segment's own span the distance is perpendicular; beyond either end it is
            // the distance to the nearer endpoint, which is what rounds the line's caps.
            double aa = 0.0;
            if (scanX < x1 || scanX > x2 || scanY < y1 || scanY > y2) {
                const double d1 = std::sqrt(static_cast<double>(x - X1) * (x - X1) +
                                            static_cast<double>(y - Y1) * (y - Y1));
                const double d2 = std::sqrt(static_cast<double>(x - X2) * (x - X2) +
                                            static_cast<double>(y - Y2) * (y - Y2));
                aa = std::min(d1, d2);
            } else {
                aa = std::abs(dstY * static_cast<double>(x) - dstX * static_cast<double>(y) + uniC) *
                     denominator;
            }

            if (aa > halfWidth)
                continue;

            double weight = 1.0;
            if (antialias && aa > halfWidth - 1.0) {
                // The reference multiplies the colour's alpha by a factor it TRUNCATES into a byte
                // and then applies as an 8-bit multiply, so the rim's ramp is quantized to 1/255
                // and its top step is 255/255 rather than 256/256. Transcribed, truncation included.
                const int f = static_cast<int>((1.0 - (aa - (halfWidth - 1.0))) * 256.0);
                weight = static_cast<double>(std::clamp(f, 0, 255)) / 255.0;
            }
            sink.emit(x, y, weight);
        }
    }
}

void linearTrajectory(common::Vec2 start, common::Vec2 end, std::vector<common::Vec2>& out) {
    out.clear();
    if (!finitePoint(start) || !finitePoint(end))
        return;

    const double xd = end.x - start.x;
    const double yd = end.y - start.y;

    // ⚠ TRUNCATION toward zero, not floor: the reference casts. It matters on the negative side of
    // the origin, where floor and cast differ by one.
    int x = static_cast<int>(start.x);
    int y = static_cast<int>(start.y);
    const int x2 = static_cast<int>(end.x);
    const int y2 = static_cast<int>(end.y);
    double fx = start.x;
    double fy = start.y;
    double m = yd / xd; // deliberately unguarded: the branches below handle inf and NaN

    out.push_back(start);

    std::size_t steps = 0;
    if (std::fabs(m) > 1.0) {
        int incr = 0;
        if (yd > 0.0) {
            m = 1.0 / m;
            incr = 1;
        } else {
            m = -1.0 / m;
            incr = -1;
        }
        while (y != y2 && steps++ < kMaxLinePixels) {
            fx += m;
            fy += incr;
            y += incr;
            out.push_back(common::Vec2{fx, fy});
        }
    } else {
        int incr = 0;
        if (xd > 0.0) {
            incr = 1;
        } else {
            incr = -1;
            m = -m;
        }
        while (x != x2 && steps++ < kMaxLinePixels) {
            fy += m;
            fx += incr;
            x += incr;
            out.push_back(common::Vec2{fx, fy});
        }
    }

    // The exact end point always closes the list -- so a bristle that did not move still lays two
    // coincident points, and the antialiased path's "drop the last one" leaves it exactly one.
    out.push_back(end);
}

void flattenQuadratic(common::Vec2 p0, common::Vec2 c, common::Vec2 p1,
                      std::vector<common::Vec2>& out) {
    const double hull = (c - p0).length() + (p1 - c).length();
    const int steps = std::clamp(static_cast<int>(std::ceil(hull * 0.5)), 1, 4096);
    for (int i = 1; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        const double u = 1.0 - t;
        out.push_back(p0 * (u * u) + c * (2.0 * u * t) + p1 * (t * t));
    }
}

void flattenCubic(common::Vec2 p0, common::Vec2 c1, common::Vec2 c2, common::Vec2 p1,
                  std::vector<common::Vec2>& out) {
    const double hull = (c1 - p0).length() + (c2 - c1).length() + (p1 - c2).length();
    const int steps = std::clamp(static_cast<int>(std::ceil(hull * 0.5)), 1, 4096);
    for (int i = 1; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        const double u = 1.0 - t;
        out.push_back(p0 * (u * u * u) + c1 * (3.0 * u * u * t) + c2 * (3.0 * u * t * t) +
                      p1 * (t * t * t));
    }
}

bool SegmentScratch::reset(int x0, int y0, int x1, int y1, const StrokeCanvas& canvas) {
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, canvas.width());
    y1 = std::min(y1, canvas.height());
    if (x0 >= x1 || y0 >= y1) {
        m_w = 0;
        m_h = 0;
        return false;
    }
    const auto cells = static_cast<std::size_t>(x1 - x0) * static_cast<std::size_t>(y1 - y0);
    if (cells > kMaxScratchCells) {
        // The backstop. Keep the top-left corner rather than nothing, so a pathological brush still
        // paints something recognizable instead of vanishing.
        y1 = y0 + static_cast<int>(kMaxScratchCells / static_cast<std::size_t>(x1 - x0));
        if (y1 <= y0)
            y1 = y0 + 1;
    }
    m_x = x0;
    m_y = y0;
    m_w = x1 - x0;
    m_h = y1 - y0;
    const auto n = static_cast<std::size_t>(m_w) * static_cast<std::size_t>(m_h);
    if (m_cells.size() < n)
        m_cells.resize(n);
    std::fill_n(m_cells.begin(), n, static_cast<std::uint8_t>(0));
    return true;
}

void SegmentScratch::plot(int x, int y, int value, ScratchMode mode) {
    if (value <= 0 || x < m_x || y < m_y || x >= m_x + m_w || y >= m_y + m_h)
        return;
    std::uint8_t& dst = m_cells[static_cast<std::size_t>(y - m_y) * static_cast<std::size_t>(m_w) +
                                static_cast<std::size_t>(x - m_x)];
    const int had = dst;
    int now = had;
    switch (mode) {
    case ScratchMode::Add:
        now = had + value;
        break;
    case ScratchMode::Over:
        now = static_cast<int>(std::lround(value + had * (255.0 - value) / 255.0));
        break;
    case ScratchMode::Max:
        now = std::max(had, value);
        break;
    }
    dst = static_cast<std::uint8_t>(std::clamp(now, 0, 255));
}

void SegmentScratch::flush(StrokeCanvas& canvas, common::Color8 color, double opacity) const {
    if (!(opacity > 0.0))
        return;
    for (int y = 0; y < m_h; ++y) {
        for (int x = 0; x < m_w; ++x) {
            const std::uint8_t v =
                m_cells[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_w) +
                        static_cast<std::size_t>(x)];
            if (v == 0)
                continue;
            canvas.plot(m_x + x, m_y + y, (static_cast<double>(v) / 255.0) * opacity, color);
        }
    }
}

void fillPolygon(const std::vector<common::Vec2>& points, bool windingFill, bool antialias,
                 SegmentScratch& out) {
    if (points.size() < 3 || out.width() <= 0 || out.height() <= 0)
        return;

    // The two fill rules, evaluated by casting a ray to the right and counting crossings: the
    // WINDING rule sums signed crossings and fills where the sum is non-zero; the alternate rule
    // fills where the count is odd. A self-crossing scribble -- which is what this engine's stroke
    // is -- reads very differently under the two, which is why the flag exists.
    const auto inside = [&](double px, double py) {
        int winding = 0;
        int crossings = 0;
        const std::size_t n = points.size();
        for (std::size_t i = 0; i < n; ++i) {
            const common::Vec2 a = points[i];
            const common::Vec2 b = points[(i + 1) % n];
            if ((a.y <= py) == (b.y <= py))
                continue;
            const double t = (py - a.y) / (b.y - a.y);
            if (a.x + t * (b.x - a.x) <= px)
                continue;
            ++crossings;
            winding += b.y > a.y ? 1 : -1;
        }
        return windingFill ? winding != 0 : (crossings & 1) != 0;
    };

    constexpr int kSub = 4; // 4x4 supersampling: 17 distinct coverage levels on an edge
    for (int y = 0; y < out.height(); ++y) {
        for (int x = 0; x < out.width(); ++x) {
            const double px = out.originX() + x;
            const double py = out.originY() + y;
            if (!antialias) {
                if (inside(px + 0.5, py + 0.5))
                    out.plot(out.originX() + x, out.originY() + y, 255, ScratchMode::Max);
                continue;
            }
            int hits = 0;
            for (int sy = 0; sy < kSub; ++sy)
                for (int sx = 0; sx < kSub; ++sx)
                    if (inside(px + (sx + 0.5) / kSub, py + (sy + 0.5) / kSub))
                        ++hits;
            if (hits == 0)
                continue;
            out.plot(out.originX() + x, out.originY() + y,
                     static_cast<int>(std::lround(255.0 * hits / (kSub * kSub))),
                     ScratchMode::Max);
        }
    }
}

StrokePainterKind painterKindForPaintop(std::string_view paintopId) noexcept {
    // ⚠ ONE LIST (see the header). Add a paintop here the day its painter paints, never the day a
    // plan says it will -- `mapPreset` reads this to decide the preset's fidelity FLOOR.
    //
    // ⚠ `hatchingbrush` is deliberately NOT here and that is not an omission: it derives from the
    // reference's BRUSH-BASED base class and overrides `paintAt`, so it is a DAB engine whose dab
    // content is a procedural pattern stencilled by the tip mask -- exactly what §6.6(c) predicted.
    // It lives in the dab pipeline (brush_engine.hpp's HatchingParams), not here.
    if (paintopId == "sketchbrush")
        return StrokePainterKind::Sketch;
    if (paintopId == "hairybrush")
        return StrokePainterKind::Hairy;
    if (paintopId == "curvebrush")
        return StrokePainterKind::Curve;
    if (paintopId == "particlebrush")
        return StrokePainterKind::Particle;
    if (paintopId == "experimentbrush")
        return StrokePainterKind::Experiment;
    return StrokePainterKind::None;
}

bool painterVariesColor(const StrokePainterParams& params) noexcept {
    // The sketch engine's `randomRGB` builds a fresh colour per connection (each channel of the
    // paint colour scaled by its own random draw), which one coverage channel cannot carry. Nothing
    // else does: the hairy engine's bristles all deposit the stroke's own colour, because the two
    // things that would vary it per bristle -- saturation depletion and soaked ink -- are not
    // transcribed (hairy_painter.hpp).
    return params.kind == StrokePainterKind::Sketch && params.sketch.randomRgb;
}

std::unique_ptr<StrokePainter> makeStrokePainter(const StrokePainterParams& params) {
    switch (params.kind) {
    case StrokePainterKind::Sketch:
        return std::make_unique<SketchPainter>(params.sketch);
    case StrokePainterKind::Hairy:
        return std::make_unique<HairyPainter>(params.hairy);
    case StrokePainterKind::Curve:
        return std::make_unique<CurvePainter>(params.curve);
    case StrokePainterKind::Particle:
        return std::make_unique<ParticlePainter>(params.particle);
    case StrokePainterKind::Experiment:
        return std::make_unique<ExperimentPainter>(params.experiment);
    case StrokePainterKind::None:
        break;
    }
    return nullptr;
}

} // namespace mosaic::core::brush
