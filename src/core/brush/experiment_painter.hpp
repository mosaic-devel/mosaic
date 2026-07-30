#pragma once

#include "core/brush/stroke_painter.hpp"

#include <cstddef>
#include <vector>

// THE EXPERIMENT / SHAPE-FILL ENGINE (docs/brushes.md §6.6g), transcribed from the reference's
// experiment paintop -- the engine behind the shipped `t)_Shapes_Fill` preset.
//
// The mark is the WHOLE STROKE, as one closed polygon, filled once. Nothing is laid while the
// pointer moves: each span appends its end point to a path, and the path is filled on release. It is
// the only engine of either kind whose output is not a function of any prefix of the stroke -- a
// stroke that ends somewhere else is a different shape everywhere, not merely at its end.
//
// ⚠ THAT IS WHY `finish()` EXISTS on `StrokePainter`. §6.6b flagged this engine as the one that
// fights `composite()`'s incremental pending-region model, and it does: the reference repaints its
// growing shape progressively (diffing painter paths, or fanning triangles from the stroke's first
// point) at up to 25 fps. Mosaic fills once, at `end()`. The shape is identical; what is lost is the
// live preview of it, and §6.6g records that as the deviation it is.
namespace mosaic::core::brush {

// The backstop on the accumulated path. A polygon fill is O(points) per SUB-SAMPLE, so an unbounded
// scribble would be quadratic in the stroke's length; well past any real stroke.
inline constexpr std::size_t kMaxExperimentPoints = 20000;

class ExperimentPainter final : public StrokePainter {
public:
    explicit ExperimentPainter(ExperimentPainterParams params) : m_params(params) {}

    void begin(const StrokePainterContext& ctx) override;
    void paintSpan(StrokeCanvas& canvas, const StrokeSnapshot& a, const StrokeSnapshot& b,
                   StrokeState& state) override;
    void finish(StrokeCanvas& canvas) override;

    [[nodiscard]] std::size_t pointCount() const noexcept { return m_path.size(); }

private:
    ExperimentPainterParams m_params;
    common::Color8 m_color{0, 0, 0, 255};
    std::vector<common::Vec2> m_path;
    SegmentScratch m_scratch;
    bool m_firstRun = true;
};

} // namespace mosaic::core::brush
