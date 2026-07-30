#pragma once

#include "core/brush/stroke_painter.hpp"

#include <cstddef>
#include <vector>

// THE PARTICLE ENGINE (docs/brushes.md §6.6g), transcribed from the reference's particle paintop --
// the engine behind the shipped `v)_Experimental_Webs` preset.
//
// The mark is a persistent SIMULATION, not geometry. A fixed set of particles is planted at the
// press point, each with its own acceleration; per stroke segment the simulation is stepped
// `iterations` times, and on every step each particle is pulled toward the cursor, damped, moved,
// and splatted. Nothing about the mark is a function of the path alone -- run the same path twice
// with a different history and the particles are somewhere else.
//
// ⚠ ITS EQUATION IS DELIBERATELY UNSTABLE at a negative scale or gravity, and the reference says so
// in its own comment ("the effect of instability might be quite interesting for the painters"). It
// guards the divergence by clipping to the document instead of stabilizing the equation, and so
// does this: same equation, same guard, same coordinates.
namespace mosaic::core::brush {

// The backstop on the particle count. Each particle costs `iterations` splats per segment.
inline constexpr int kMaxParticles = 20000;

class ParticlePainter final : public StrokePainter {
public:
    explicit ParticlePainter(ParticlePainterParams params) : m_params(params) {}

    void begin(const StrokePainterContext& ctx) override;
    void paintSpan(StrokeCanvas& canvas, const StrokeSnapshot& a, const StrokeSnapshot& b,
                   StrokeState& state) override;

    [[nodiscard]] std::size_t particleCount() const noexcept { return m_pos.size(); }

private:
    // The reference's Wu particle: a 2x2 bilinear splat weighted by `weight`, added into the
    // segment's temporary. ⚠ It FLOORS its position and takes a SIGNED fraction, where the bristle
    // engine's otherwise-identical-looking splat truncates and takes an absolute one. Two engines,
    // two conventions, both reproduced.
    void splat(common::Vec2 pos, int opacity);

    ParticlePainterParams m_params;
    common::Color8 m_color{0, 0, 0, 255};

    std::vector<common::Vec2> m_pos;     // where each particle IS
    std::vector<common::Vec2> m_nextPos; // its velocity accumulator (the reference's name, kept)
    std::vector<double> m_acceleration;
    SegmentScratch m_scratch;
    bool m_planted = false;
};

} // namespace mosaic::core::brush
