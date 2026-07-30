#pragma once

#include "core/brush/stroke_painter.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// THE HAIRY / SUMI-E ENGINE (docs/brushes.md §6.6g), transcribed from the reference's hairy paintop
// -- the engine behind the shipped `d)_Ink-8_Sumi-e` preset.
//
// WHAT THE ALGORITHM ACTUALLY IS, because the name misleads and the misreading cost this preset a
// year in the excluded list (docs/brushes.md §5, and the 2026-07-27 ruling that lifted it):
//
//   1. Rasterize the tip ONCE, at the preset's own size, and turn every OPAQUE PIXEL of that raster
//      into a "bristle": a fixed offset from the raster's centre, plus that pixel's alpha, which is
//      the only thing the bristle ever knows about itself. A density percentage keeps a random
//      subset, drawn from a FIXED stream -- the layout is a property of the brush, not of a stroke.
//   2. Per stroke segment, put every bristle through ONE affine transform -- a pressure-driven
//      shear, a random jitter, the size scale, the rotation -- and draw a straight line from where
//      that bristle was to where it now is.
//   3. Count the marks each bristle has laid and read a transfer curve at that count. That counter
//      is the "ink"; the curve is how the stroke dries out.
//
// There is no mass-spring model, no stiffness-height mapping, and no bristle physics state of any
// kind: a bristle carries a previous position and an integer counter. It is a scatter of correlated
// 1 px lines with a per-line ageing term.
//
// ⚠ IT KEEPS A PER-SEGMENT SCRATCH, AND THAT IS THE TRANSCRIPTION. Upstream composites a segment's
// bristle marks into a temporary device and blits the RESULT over the layer, so within one segment
// the marks combine ADDITIVELY (saturating at full) and only the segment's total goes "over" what
// came before. Depositing each splat straight into the stroke's coverage would combine them "over"
// each other instead, which is measurably lighter wherever a bristle doubles back. The scratch is
// the painter's own, exactly as the temporary is the paintop's -- the ENGINE's accumulation is
// still the one and only one (stroke_painter.hpp).
namespace mosaic::core::brush {

// The backstop on how many bristles one tip may become. Every bristle costs a line per segment, so
// a 1000 px tip's million opaque pixels is not a brush, it is a hang. Well past any real tip: the
// shipped Sumi-e resolves to a few hundred.
inline constexpr std::size_t kMaxBristles = 20000;

class HairyPainter final : public StrokePainter {
public:
    explicit HairyPainter(HairyPainterParams params) : m_params(std::move(params)) {}

    void begin(const StrokePainterContext& ctx) override;
    void paintSpan(StrokeCanvas& canvas, const StrokeSnapshot& a, const StrokeSnapshot& b,
                   StrokeState& state) override;

    [[nodiscard]] std::size_t bristleCount() const noexcept { return m_bristles.size(); }

private:
    // One bristle. `x`/`y` are its fixed offset from the tip raster's centre and `length` is the
    // tip's alpha there -- the reference stores that alpha as the bristle's length and uses it, with
    // nothing else, as the mark's opacity whenever ink depletion is off.
    struct Bristle {
        double x = 0.0;
        double y = 0.0;
        double length = 0.0;
        double prevX = 0.0; // where this bristle's last mark ENDED, in tip-frame offset
        double prevY = 0.0;
        double inkAmount = 0.0; // the reference's Bristle::m_inkAmount, which starts at 0
        int counter = 0;        // marks laid -- the index into the depletion curve
    };

    // One mark of one bristle, into the segment's scratch (stroke_painter.hpp). The reference's
    // temporary device carries colour too, but every bristle deposits the same colour here -- see
    // the two untranscribed per-bristle colour paths in the .cpp.
    void splat(common::Vec2 pos, double alpha);
    // Which of the temporary device's three combination laws this preset's flags select.
    [[nodiscard]] ScratchMode scratchMode() const noexcept;

    // The reference's mouse-pressure ramp: a one-sided filter over the segment's own length that
    // stands in for a stylus on a mouse stroke. Stateful, by design.
    [[nodiscard]] double computeMousePressure(double distance);

    HairyPainterParams m_params;
    const BrushOptions* m_options = nullptr;
    common::Color8 m_color{0, 0, 0, 255};
    double m_baseAngleRad = 0.0;

    std::vector<Bristle> m_bristles;
    std::vector<float> m_inkCurve;      // the depletion transfer, sampled `inkAmount` times
    std::vector<common::Vec2> m_path;   // one bristle's trajectory, reused

    SegmentScratch m_scratch;

    double m_halfX = 0.0;   // the bristle field's half-extents, for the scratch bound
    double m_halfY = 0.0;
    double m_lastReach = 0.0; // the previous segment's transformed reach (connected paths)
    double m_oldPressure = 1.0;
    std::size_t m_counter = 0; // segments painted; the reference's `firstStroke()` is counter == 1
};

} // namespace mosaic::core::brush
