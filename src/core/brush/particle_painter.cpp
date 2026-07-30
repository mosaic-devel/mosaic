#include "core/brush/particle_painter.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core::brush {

namespace {

// The reference's integration step, verbatim. It is not a physical time: it is the constant the
// equation was tuned against, and the whole feel of the brush is in it.
constexpr double kTime = 0.000030;

// The reference guards its own divergence at this magnitude rather than at the document's edge --
// a coordinate past it is not clipped, it is dropped, because the value is about to stop being a
// number at all.
constexpr double kNearInfinity = 2147483600.0;

} // namespace

void ParticlePainter::begin(const StrokePainterContext& ctx) {
    m_color = ctx.color;
    m_planted = false;

    const int count = std::clamp(m_params.count, 0, kMaxParticles);
    m_pos.assign(static_cast<std::size_t>(count), common::Vec2{});
    m_nextPos.assign(static_cast<std::size_t>(count), common::Vec2{});
    m_acceleration.assign(static_cast<std::size_t>(count), 0.0);

    // ⚠ THE INITIAL POSITION IS SET ONCE, AT THE PRESS, and every particle starts on top of it --
    // the spread is made entirely by their DIFFERING accelerations, which are a plain ramp
    // `(i + iterations) * 0.5` over the particle index. There is no randomness in this engine at
    // all: it draws nothing from the stroke's streams, and two strokes over the same path are
    // identical whatever the seed.
    for (std::size_t i = 0; i < m_pos.size(); ++i) {
        m_pos[i] = ctx.first.pos;
        m_nextPos[i] = ctx.first.pos;
        m_acceleration[i] =
            (static_cast<double>(i) + static_cast<double>(m_params.iterations)) * 0.5;
    }
    m_planted = true;
}

void ParticlePainter::splat(common::Vec2 pos, int opacity) {
    // FLOOR and a SIGNED fraction (contrast the bristle engine's truncate-and-abs), then a 2x2
    // bilinear split of `opacity * weight`, added saturating into the segment's temporary.
    const int ipx = static_cast<int>(std::floor(pos.x));
    const int ipy = static_cast<int>(std::floor(pos.y));
    const double fx = pos.x - ipx;
    const double fy = pos.y - ipy;
    const double w = m_params.weight;
    const auto q = [opacity, w](double a, double b) {
        return static_cast<int>(std::lround(a * b * opacity * w));
    };
    m_scratch.plot(ipx, ipy, q(1.0 - fx, 1.0 - fy), ScratchMode::Add);
    m_scratch.plot(ipx + 1, ipy, q(fx, 1.0 - fy), ScratchMode::Add);
    m_scratch.plot(ipx, ipy + 1, q(1.0 - fx, fy), ScratchMode::Add);
    m_scratch.plot(ipx + 1, ipy + 1, q(fx, fy), ScratchMode::Add);
}

void ParticlePainter::paintSpan(StrokeCanvas& canvas, const StrokeSnapshot& a,
                                const StrokeSnapshot& b, StrokeState& state) {
    (void)a;
    (void)state; // this engine draws nothing random: it is a simulation, not a scatter
    if (m_pos.empty() || !m_planted)
        return;

    // The whole document is the scratch: a particle cloud is unbounded by construction (that is
    // what the gravity term is for) and there is no useful box short of the canvas. `reset` clamps
    // and clears; a stroke on a big canvas pays one clear per segment, which is the same order as
    // the simulation itself.
    if (!m_scratch.reset(0, 0, canvas.width(), canvas.height(), canvas))
        return;

    const common::Vec2 target = b.sample.pos;
    // The reference bounds its particles only when the equation is in its unstable regime; with
    // every coefficient positive it lets them fly and relies on the device being unbounded. Mosaic's
    // canvas IS the bound, so `plot` clips either way -- what this flag decides is whether a
    // diverged particle is dropped BEFORE it is splatted, which is the reference's own guard.
    const bool guarded =
        m_params.scaleX < 0.0 || m_params.scaleY < 0.0 || m_params.gravity < 0.0;

    // The paint colour's own alpha is the particle's opacity ceiling (`respectOpacity` is true at
    // this call site); the stroke's ceiling is applied by the engine at composite as always.
    const int opacity = m_color.a;

    for (int it = 0; it < m_params.iterations; ++it) {
        for (std::size_t j = 0; j < m_pos.size(); ++j) {
            // dist = (cursor - position) scaled per axis, then multiplied by this particle's own
            // acceleration; the accumulator takes it, is damped by gravity, and moves the particle
            // by a constant sliver of itself. Every line of this is the reference's.
            common::Vec2 dist = target - m_pos[j];
            dist.x *= m_params.scaleX;
            dist.y *= m_params.scaleY;
            dist = dist * m_acceleration[j];
            m_nextPos[j] = m_nextPos[j] + dist;
            m_nextPos[j] = m_nextPos[j] * m_params.gravity;
            m_pos[j] = m_pos[j] + (m_nextPos[j] * kTime);

            const common::Vec2 p = m_pos[j];
            if (!std::isfinite(p.x) || !std::isfinite(p.y))
                continue;
            if (guarded) {
                const bool nearInfinity = p.x < -kNearInfinity || p.x > kNearInfinity ||
                                          p.y < -kNearInfinity || p.y > kNearInfinity;
                if (nearInfinity)
                    continue;
            }
            splat(p, opacity);
        }
    }

    m_scratch.flush(canvas, m_color);
}

} // namespace mosaic::core::brush
