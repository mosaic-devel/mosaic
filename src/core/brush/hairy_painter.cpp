#include "core/brush/hairy_painter.hpp"

#include "core/brush/brush_tip.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core::brush {

namespace {

[[nodiscard]] double clamp01(double v) noexcept {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

// alpha in [0,1] -> the 8-bit opacity the reference's temporary device carries. Round to nearest,
// which is what its float->quint8 conversion does.
[[nodiscard]] int toByte(double a) noexcept {
    return static_cast<int>(std::lround(clamp01(a) * 255.0));
}

} // namespace

void HairyPainter::begin(const StrokePainterContext& ctx) {
    m_options = ctx.options;
    m_color = ctx.color;
    m_baseAngleRad = ctx.angleRad;
    m_bristles.clear();
    m_path.clear();
    m_lastReach = 0.0;
    m_oldPressure = 1.0;
    m_counter = 0;

    // The depletion transfer, sampled exactly as the reference samples it: `inkAmount` points of the
    // curve over [0,1] inclusive, indexed by the bristle's own mark counter. `Curve::toLut` IS that
    // sampling (`value(i / (size - 1))`, clamped to [0,1]).
    const int lutSize = std::max(2, m_params.inkAmount);
    m_inkCurve = m_params.inkDepletionCurve.toLut(static_cast<std::size_t>(lutSize));

    // ⚠ THE BRISTLES ARE THE TIP'S OWN PIXELS. Rasterize once, at the preset's authored master size
    // -- the same raster a stamped dab of this preset would use, so a bristle lands exactly where
    // that tip pixel would land -- and read the alpha out of it. The reference reads its brush's
    // mask at the brush's natural size for the same reason; the per-segment `Size` value scales the
    // whole field afterwards, it does not re-rasterize.
    DabMask mask;
    if (ctx.tip != nullptr) {
        const DabShape shape =
            tipDabShape(*ctx.tip, 0, ctx.diameter, ctx.ratio, 0.0, false, false);
        mask = renderTipMask(*ctx.tip, 0, shape, 1.0, 0.0, 0.0);
    }

    if (mask.empty()) {
        // No tip (or a raster the tip machinery refused): one hair at the centre, so a tipless hairy
        // preset degenerates to a single-hair pen rather than to a stroke that paints nothing.
        m_bristles.push_back(Bristle{0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0});
        m_halfX = 0.0;
        m_halfY = 0.0;
        return;
    }

    // ⚠ INTEGER centre, truncated -- the reference assigns `width * 0.5` to an int. On an odd raster
    // the bristle field therefore sits half a pixel off centre, and reproducing that is the
    // difference between a bristle landing on the tip pixel it came from and half a pixel beside it.
    const int centerX = static_cast<int>(static_cast<double>(mask.width) * 0.5);
    const int centerY = static_cast<int>(static_cast<double>(mask.height) * 0.5);

    // The density subset is drawn from a FIXED stream, exactly as the reference's is (its random
    // source is constructed with seed 0): which pixels become bristles is a property of the BRUSH,
    // identical on every stroke and every replay, and it must never consume the stroke's own stream.
    // ⚠ At density 1 the reference short-circuits BEFORE the draw, so a full-density brush pulls no
    // random numbers at all -- the same inert-option discipline the dab pipeline keeps.
    StrokeState brushRng;
    brushRng.begin(StrokeInput{}, /*seed=*/0);
    const double density = clamp01(m_params.densityFactor * 0.01);

    m_halfX = 0.0;
    m_halfY = 0.0;
    for (std::uint32_t y = 0; y < mask.height && m_bristles.size() < kMaxBristles; ++y) {
        for (std::uint32_t x = 0; x < mask.width && m_bristles.size() < kMaxBristles; ++x) {
            const double alpha = static_cast<double>(mask.at(x, y)) / 255.0;
            if (alpha == 0.0)
                continue;
            if (!(density == 1.0 || brushRng.nextRandom() <= density))
                continue;
            const double bx = static_cast<double>(static_cast<int>(x) - centerX);
            const double by = static_cast<double>(static_cast<int>(y) - centerY);
            m_bristles.push_back(Bristle{bx, by, alpha, 0.0, 0.0, 0.0, 0});
            m_halfX = std::max(m_halfX, std::abs(bx));
            m_halfY = std::max(m_halfY, std::abs(by));
        }
    }

    if (m_bristles.empty())
        m_bristles.push_back(Bristle{0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0});
}

double HairyPainter::computeMousePressure(double distance) {
    // The reference's constants, verbatim: a 20 px reference travel and a 0.02 floor, folded into a
    // 4:1 one-pole filter over the previous value. It is state, and it starts at 1.
    constexpr double kScale = 20.0;
    constexpr double kMinPressure = 0.02;
    const double oldPressure = m_oldPressure;
    double factor = 1.0 - distance / kScale;
    if (factor < 0.0)
        factor = 0.0;
    const double result = ((4.0 * oldPressure) + kMinPressure + factor) / 5.0;
    m_oldPressure = result;
    return result;
}

ScratchMode HairyPainter::scratchMode() const noexcept {
    // The reference's three branches, selected by the same two flags it selects them with:
    //  * compositing on  -> `plotPixel`: a plain source-over onto what the segment has laid so far.
    //  * antialias on    -> its Wu particle copy: ADD, saturating. This is the one that makes a
    //                       bristle doubling back on itself go solid rather than asymptotically dark.
    //  * neither         -> `darkenPixel`: keep whichever is more opaque -- a hard mark that cannot
    //                       build at all.
    if (m_params.useCompositing)
        return ScratchMode::Over;
    return m_params.antialias ? ScratchMode::Add : ScratchMode::Max;
}

void HairyPainter::splat(common::Vec2 pos, double alpha) {
    const int opacity = toByte(alpha);
    const ScratchMode mode = scratchMode();
    if (!m_params.antialias) {
        m_scratch.plot(static_cast<int>(std::lround(pos.x)), static_cast<int>(std::lround(pos.y)),
                       opacity, mode);
        return;
    }
    // The reference's Wu-style particle: a 2x2 bilinear split of the mark's opacity.
    // ⚠ TRUNCATION, then an ABSOLUTE fraction -- both the reference's. On the negative side of the
    // origin the pair puts the heavier weight on the far cell, which is a real (and reproduced)
    // asymmetry rather than a symmetric bilinear splat.
    const int ipx = static_cast<int>(pos.x);
    const int ipy = static_cast<int>(pos.y);
    const double fx = std::abs(pos.x - ipx);
    const double fy = std::abs(pos.y - ipy);
    const auto w = [opacity](double a, double b) {
        return static_cast<int>(std::lround(a * b * opacity));
    };
    m_scratch.plot(ipx, ipy, w(1.0 - fx, 1.0 - fy), mode);
    m_scratch.plot(ipx + 1, ipy, w(fx, 1.0 - fy), mode);
    m_scratch.plot(ipx, ipy + 1, w(1.0 - fx, fy), mode);
    m_scratch.plot(ipx + 1, ipy + 1, w(fx, fy), mode);
}

void HairyPainter::paintSpan(StrokeCanvas& canvas, const StrokeSnapshot& a, const StrokeSnapshot& b,
                             StrokeState& state) {
    ++m_counter;
    const bool firstSegment = m_counter == 1; // the reference's firstStroke()

    const double x1 = a.sample.pos.x;
    const double y1 = a.sample.pos.y;
    const double x2 = b.sample.pos.x;
    const double y2 = b.sample.pos.y;
    const double dx = x2 - x1;
    const double dy = y2 - y1;

    // The two per-segment option values, against the segment's END sample (the engine has already
    // rewound the stroke there). ⚠ The dab pipeline's own `dabAngle` supplies the rotation, so the
    // bristle field turns exactly as a stamped dab of the same preset would -- the reference reaches
    // the same geometry through its own opposite-signed angle convention and an inverse rotation in
    // the transform, and reproducing THAT rather than the resulting geometry would rotate the field
    // the wrong way against every other mark Mosaic makes.
    double scale = 1.0;
    double angle = m_baseAngleRad;
    if (m_options != nullptr) {
        scale = standardOptionValue(m_options->size, state);
        angle = dabAngle(*m_options, m_baseAngleRad, state);
    }
    scale *= m_params.scaleFactor;

    double mousePressure = 1.0;
    if (m_params.useMousePressure) {
        mousePressure = 1.0 - computeMousePressure(std::sqrt(dx * dx + dy * dy));
        scale *= mousePressure;
    }
    // ⚠ TWICE the sample's pressure. This drives the shear and the ink weights, and its range is
    // [0,2], not [0,1] -- the reference doubles it deliberately.
    const double pressure = mousePressure * (b.sample.pressure * 2.0);
    const double shear = pressure * m_params.shearFactor;
    const double threshold = 1.0 - b.sample.pressure;

    const double cosT = std::cos(angle);
    const double sinT = std::sin(angle);

    // The scratch's box: the segment, grown by the furthest a transformed bristle can reach. The
    // bound is geometric (shear, then jitter, then scale; the rotation preserves length), so it is
    // conservative rather than measured -- measuring would need a second pass over the bristles, and
    // the random draws can only happen once.
    const double shearAbs = std::abs(shear);
    const double jitter = std::abs(m_params.randomFactor);
    const double rx = (m_halfX + shearAbs * m_halfY + jitter) * std::abs(scale);
    const double ry = (shearAbs * m_halfX + m_halfY + jitter) * std::abs(scale);
    double reach = std::sqrt(rx * rx + ry * ry) + 4.0;
    if (!std::isfinite(reach))
        reach = 0.0;
    reach = std::min(reach, 65536.0);
    // A connected path starts at the PREVIOUS segment's transformed position, so the box must cover
    // that segment's reach as well as this one's.
    const double span = std::max(reach, m_lastReach);
    m_lastReach = reach;
    const double lo = std::min(x1, x2) - span;
    const double hi = std::max(x1, x2) + span;
    const double top = std::min(y1, y2) - span;
    const double bot = std::max(y1, y2) + span;
    const auto floorInt = [](double v) {
        return static_cast<int>(std::floor(std::clamp(v, -1.0e9, 1.0e9)));
    };
    // False (wholly off the document) does NOT skip the bristle loop below -- see its comment.
    (void)m_scratch.reset(floorInt(lo), floorInt(top), floorInt(hi) + 2, floorInt(bot) + 2, canvas);

    // ⚠ THE BRISTLE LOOP RUNS WHETHER OR NOT ANY OF IT LANDS ON THE DOCUMENT. Every bristle draws
    // two random numbers, advances its previous position and ages its ink counter -- all properties
    // of the STROKE, exactly like the dab walk's per-dab draws, which advance for clipped dabs too.
    // Skipping an off-canvas segment would give a stroke near an edge a different mark from the same
    // stroke in the middle of the canvas.
    const int lutLast = static_cast<int>(m_inkCurve.size()) - 1;
    for (Bristle& br : m_bristles) {
        const double randomX = (state.nextRandom() * 2.0 - 1.0) * m_params.randomFactor;
        const double randomY = (state.nextRandom() * 2.0 - 1.0) * m_params.randomFactor;

        // ONE affine transform, in the reference's composition order: shear, then the jitter
        // translation, then the size scale, then the rotation.
        double tx = br.x + shear * br.y;
        double ty = shear * br.x + br.y;
        tx += randomX;
        ty += randomY;
        tx *= scale;
        ty *= scale;
        const double ex = tx * cosT - ty * sinT;
        const double ey = tx * sinT + ty * cosT;

        double sx = ex;
        double sy = ey;
        if (!firstSegment && m_params.connectedPath) {
            sx = br.prevX;
            sy = br.prevY;
        }
        // Remembered BEFORE the segment's endpoints are added: a bristle's memory is its offset,
        // not its document position, which is what lets a connected path follow a moving stroke.
        br.prevX = ex;
        br.prevY = ey;

        // ⚠ AFTER the draws and after the memory: a bristle too short for the current pressure lays
        // no ink this segment, but it has still moved and still drew its two randoms.
        if (m_params.threshold && br.length < threshold)
            continue;

        linearTrajectory(common::Vec2{sx + x1, sy + y1}, common::Vec2{ex + x2, ey + y2}, m_path);
        std::size_t marks = m_path.size();
        // "avoid overlapping bristle caps with antialias on" -- the reference, verbatim: the last
        // trajectory point is dropped, so a bristle's end cap is laid by the NEXT segment's start.
        if (m_params.antialias && marks > 0)
            --marks;

        // With depletion off the mark's opacity is the bristle's length -- the tip's own alpha at
        // that pixel -- for every point of the trajectory, which is the reference's `else` branch.
        double alpha = br.length;
        for (std::size_t k = 0; k < marks; ++k) {
            double inkDepletion = 0.0;
            if (m_params.inkDepletionEnabled) {
                // The reference's fetch: the bristle's own mark counter, clamped to the LUT's last
                // entry -- a bristle that has laid more marks than the curve has samples stays at
                // its end value rather than running off it.
                const int idx = std::clamp(br.counter, 0, lutLast);
                inkDepletion = static_cast<double>(m_inkCurve[static_cast<std::size_t>(idx)]);
                // ⚠ SATURATION depletion is NOT transcribed: the reference runs it through an HSL
                // (not HSV) colour transformation, which the engine has no branch for, and no
                // shipped preset enables it. The importer badges a preset that does.
                if (m_params.useOpacity) {
                    double opacity = 0.0;
                    if (m_params.useWeights) {
                        opacity = pressure * m_params.pressureWeight +
                                  br.length * m_params.bristleLengthWeight +
                                  br.inkAmount * m_params.bristleInkAmountWeight +
                                  (1.0 - inkDepletion) * m_params.inkDepletionWeight;
                    } else {
                        opacity = br.length * br.inkAmount;
                    }
                    alpha = clamp01(opacity);
                }
            }
            splat(m_path[k], alpha);
            // The reference clamps the stored ink to [-1,1]; with depletion off this is a constant 1.
            br.inkAmount = std::clamp(1.0 - inkDepletion, -1.0, 1.0);
            ++br.counter;
        }
    }

    m_scratch.flush(canvas, m_color);
}

} // namespace mosaic::core::brush
