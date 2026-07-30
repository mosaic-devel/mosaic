#pragma once

#include "core/brush/stroke_painter.hpp"

#include <cstddef>
#include <deque>
#include <vector>

// THE CURVE ENGINE (docs/brushes.md §6.6g), transcribed from the reference's curve paintop -- the
// engine behind the shipped `v)_Sketching-3_Leaky` preset.
//
// The mark is a SLIDING WINDOW of the stroke's history. Each span appends its end point to a deque
// capped at `strokeHistorySize`; once the window is full, the painter strokes ONE curve through it --
// a quadratic through the window's midpoint when `smoothing` is on, a cubic through its thirds when
// it is not -- at its own opacity. Optionally the segment itself is stroked as well, at full opacity.
//
// So the ribbon that trails behind the cursor is not paint laid along the path: it is the SAME curve
// redrawn every span from a window that has slid one point along, and it is the overlap of those
// redraws that makes the mark. Shorten the window and the ribbon tightens onto the path; lengthen it
// and it lags into a long lazy arc that cuts corners the pointer never cut.
namespace mosaic::core::brush {

class CurvePainter final : public StrokePainter {
public:
    explicit CurvePainter(CurvePainterParams params) : m_params(params) {}

    void begin(const StrokePainterContext& ctx) override;
    void paintSpan(StrokeCanvas& canvas, const StrokeSnapshot& a, const StrokeSnapshot& b,
                   StrokeState& state) override;

    [[nodiscard]] std::size_t historySize() const noexcept { return m_points.size(); }

private:
    // Stroke one polyline into the scratch as a MASK -- Max, not Add: the reference hands Qt a
    // single path and a single pen, so a path that doubles back on itself is one mark of one
    // opacity, not two overlapping ones. Chaining the transcribed thick line per edge is how the
    // stroke is rasterized; the joins are the difference, and §6.6g records it.
    void strokePolyline(StrokeCanvas& canvas, const std::vector<common::Vec2>& poly,
                        double lineWidth, double opacity);

    CurvePainterParams m_params;
    const BrushOptions* m_options = nullptr;
    common::Color8 m_color{0, 0, 0, 255};

    std::deque<common::Vec2> m_points; // the sliding window -- the drawing input
    std::vector<common::Vec2> m_poly;  // one flattened path, reused
    std::vector<LinePixel> m_pixels;   // one edge's raster, reused
    SegmentScratch m_scratch;
};

} // namespace mosaic::core::brush
