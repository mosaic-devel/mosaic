#pragma once

#include "core/brush/stroke_painter.hpp"

#include <cstddef>
#include <vector>

// THE SKETCH ENGINE (docs/brushes.md §6.6g), transcribed from the reference's sketch paintop -- the
// engine behind the two shipped `v)_Sketching-*_Chrome_*` presets.
//
// The mark is the stroke's own HISTORY. Every span appends its END point to a growing list, and then
// the painter walks the WHOLE list and draws a line between the new point and every earlier point
// that is close enough to it. A stroke that doubles back on itself therefore webs itself together,
// and the web -- not the segment -- is the drawing. (The segment itself is drawn too, optionally:
// `makeConnection`.)
//
// Three knobs shape the web, and each is a per-span curve option riding the same pipeline a dab's
// options ride: how far a connection may reach (`Size`, through the tip's radius), how likely each
// candidate is to be drawn (`Density` x `Sketch/probability`), and how far the connection's two ends
// are pulled apart or together along the line joining them (`Offset scale` x `Sketch/offset`).
//
// ⚠ TWO PIECES OF STATE SURVIVE A SPAN ON PURPOSE, and they are the reference's, not an oversight:
// the line OPACITY and the line COLOUR are properties of the painter, set inside the connection loop
// and never restored -- so the segment line at the head of the next span is drawn in whatever the
// last connection of the previous span left behind. Tidying that away would change the mark.
namespace mosaic::core::brush {

class SketchPainter final : public StrokePainter {
public:
    explicit SketchPainter(SketchPainterParams params) : m_params(params) {}

    void begin(const StrokePainterContext& ctx) override;
    void paintSpan(StrokeCanvas& canvas, const StrokeSnapshot& a, const StrokeSnapshot& b,
                   StrokeState& state) override;

    // The stroke's point history, for the tests: one point per span the painter accepted, plus the
    // press point it was seeded with.
    [[nodiscard]] const std::vector<common::Vec2>& points() const noexcept { return m_points; }

private:
    // The reference's `drawConnection`: one line, through whichever rasterizer the line's width and
    // the anti-aliasing flag select.
    void drawConnection(StrokeCanvas& canvas, common::Vec2 start, common::Vec2 end,
                        double lineWidth);

    SketchPainterParams m_params;
    const BrushOptions* m_options = nullptr;

    std::vector<common::Vec2> m_points; // the stroke's history -- the drawing input
    std::vector<LinePixel> m_pixels;    // one line's raster, reused across spans

    // The tip's own extents at the preset's authored size. The sketch engine never STAMPS the tip:
    // it reads a radius off it (`simpleMode`) and a "is this dab too small to draw" test.
    double m_tipWidth = 24.0;
    double m_tipHeight = 24.0;
    double m_radius = 0.0; // latched on the first span, exactly as the reference latches it

    common::Color8 m_paintColor{0, 0, 0, 255}; // the stroke's colour
    common::Color8 m_lineColor{0, 0, 0, 255};  // ... and the one the NEXT line will use
    double m_lineOpacity = 1.0;                // ... at this opacity (both carry across spans)
    std::size_t m_count = 0;                   // spans actually painted (the reference's m_count)
};

} // namespace mosaic::core::brush
