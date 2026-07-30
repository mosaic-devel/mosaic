#include "core/brush/dab.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mosaic::core::brush {

namespace {

// An option that is present but UNCHECKED contributes the identity, not its strength: the reference's
// curve options return early on an unchecked option, before the constant is ever applied. So a preset
// carrying a Size option with its `PressureSize` bit clear paints exactly the size it was authored
// at, rather than that size scaled by `SizeValue`.
//
// `CurveOption::compute` does not check this itself -- it cannot, since the two size-like readers are
// also used by the editor to preview an option that is switched off. The gate belongs to the consumer.
[[nodiscard]] double sizeLikeOr(const std::optional<CurveOption>& opt, StrokeState& state,
                                double identity) {
    if (!opt || !opt->isChecked())
        return identity;
    return opt->sizeLikeValue(state);
}

// The reference's `normalizeAngleDegrees`, in float: fold an angle into [0, 360). Both branches are
// the reference's, so the two fmod placements (and the exact-360 case the second check catches)
// match line for line.
[[nodiscard]] float normalizeAngleDeg(float a) noexcept {
    if (a < 0.0f)
        a = 360.0f + std::fmod(a, 360.0f);
    return a >= 360.0f ? std::fmod(a, 360.0f) : a;
}

// The reference's `HSVTransform<HSVPolicy>` (kis_hsv_adjustment.cpp), transcribed for a single
// straight [0,1] RGB triple. `dh`/`ds`/`dv` are `m_adj_h`/`m_adj_s`/`m_adj_v` in [-1,1]. HSVPolicy
// is the one the brush dynamics select (type=HSV): value = max(r,g,b), hasChroma is `v > EPSILON`,
// fixupChroma is `min(v, c)`, and writeRGB uses `m = v - chroma`. Every branch -- the achromatic
// (black) short-circuit, the hue reconstruction, the nonlinear saturation boost, the value/chroma
// "movement" step, the sextant write -- is the reference's, in `float` as the reference runs it.
void hsvTransformRgb(float& r, float& g, float& b, float dh, float ds, float dv) {
    constexpr float kEps = 1e-9f; // the reference's EPSILON

    const float M = std::max(r, std::max(g, b));
    const float m = std::min(r, std::min(g, b));
    float chroma = M - m;
    float v = M; // HSVPolicy::valueFromRGB
    float h = 0.0f;

    if (!(v > kEps)) { // !HSVPolicy::hasChroma(v): the pixel is (near) black
        chroma = 0.0f;
        h = 0.0f;
        if (dv < 0.0f)
            v *= dv + 1.0f;
        else
            v += dv * (1.0f - v);
    } else {
        if (chroma > kEps) {
            if (r == M)
                h = (g - b) / chroma;
            else if (g == M)
                h = 2.0f + (b - r) / chroma;
            else
                h = 4.0f + (r - g) / chroma;

            h *= 60.0f;
            h += dh * 180.0f;
            h = normalizeAngleDeg(h);

            if (ds > 0.0f) {
                // The reference's nonlinear saturation slider: ds 0 -> x1, 0.5 -> x2, 1 -> x4.
                chroma = std::min(1.0f, chroma * (1.0f + ds + 2.0f * ds * ds));
            } else {
                chroma *= ds + 1.0f;
            }
        } else {
            h = 0.0f;
        }

        {
            const float dstV = dv > 0.0f ? 1.0f : 0.0f;
            const float vCoeff = dstV - v;
            const float chromaCoeff = 0.0f - chroma;
            const float movement = std::abs(dv);
            v += movement * vCoeff;
            chroma += movement * chromaCoeff;
        }

        v = std::clamp(v, 0.0f, 1.0f);
        chroma = std::min(v, chroma); // HSVPolicy::fixupChroma(c, v)
    }

    if (!(v > kEps)) {
        r = g = b = 0.0f;
    } else {
        h /= 60.0f;
        const int sextant = static_cast<int>(h); // h is in [0,360) -> h/60 in [0,6) -> [0,5]
        const float fract = h - static_cast<float>(sextant);
        const float x = (sextant & 0x1) ? chroma - chroma * fract : chroma * fract;
        const float mm = v - chroma; // HSVPolicy::writeRGB: m = v - c, then writeRGBSimple(x, m, v)
        switch (sextant) {
        case 0: r = v;      g = x + mm; b = mm;     break;
        case 1: r = x + mm; g = v;      b = mm;     break;
        case 2: r = mm;     g = v;      b = x + mm; break;
        case 3: r = mm;     g = x + mm; b = v;      break;
        case 4: r = x + mm; g = mm;     b = v;      break;
        case 5: r = v;      g = mm;     b = x + mm; break;
        default: break; // unreachable: the switch is total over [0,5]
        }
    }
}

// The reference's KisHSVOption saturation/value remap (its `else` branch), returning `m_adj_s` /
// `m_adj_v` in [-strength, +strength]. `raw` is the size-like value WITH strength; `strength` is the
// option's static `strengthValue()`, read regardless of the sensor. The strength is applied TWICE
// on purpose -- the reference multiplies computeSizeLikeValue (already ·strength) by strengthValue()
// again -- so at strength < 1 the channel's span is strength², and the neutral point (0) sits at a
// size-like value of 0.5.
[[nodiscard]] double hsvSizeLikeAdjust(const CurveOption& opt, StrokeState& state) {
    const double raw = opt.sizeLikeValue(state, /*useStrength=*/true);
    const double strength = opt.data().strength;
    const double half = strength * 0.5;
    const double val = raw * strength + (0.5 - half);
    return val * 2.0 - 1.0;
}

} // namespace

const BrushOptionSpec* drivenOption(std::string_view base) noexcept {
    for (const BrushOptionSpec& spec : kDrivenOptions)
        if (spec.base == base)
            return &spec;
    return nullptr;
}

Dab evaluateDab(const BrushOptions& options, const DabBase& base, common::Vec2 center,
                StrokeState& state) {
    Dab d;
    d.center = center;

    // Each scaling option multiplies the preset's static geometry. With no option -- or an unchecked
    // one -- the factor is exactly 1.0, and `x * 1.0 == x` to the bit, which is what makes a stroke
    // with no options byte-identical to the stroke the engine laid before options existed.
    d.diameter = base.diameter * sizeLikeOr(options.size, state, 1.0);
    d.ratio = base.ratio * sizeLikeOr(options.ratio, state, 1.0);
    d.flow = base.flow * sizeLikeOr(options.flow, state, 1.0);
    d.softness = base.softness * sizeLikeOr(options.softness, state, 1.0);

    d.angleRad = dabAngle(options, base.angleRad, state);

    return d;
}

void applyScatter(const ScatterOption& sc, double extentW, double extentH, StrokeState& state,
                  Dab& dab) {
    // Inert before the first draw, exactly like the reference: unchecked or axis-less scatter must
    // not perturb the random stream, or a preset that authors it off would replay differently from
    // one that never mentions it.
    if (!sc.option.isChecked() || (!sc.axisX && !sc.axisY))
        return;

    // "just use the most significant dimension for calculations" -- the reference, verbatim. The
    // sensor value reads WITH strength: Scatter's [0,5] span is its strength's whole meaning.
    const double diameter = std::max(extentW, extentH);
    const double sensorValue = sc.option.sizeLikeValue(state);

    const double jitter = (2.0 * state.nextRandom() - 1.0) * diameter * sensorValue;
    if (sc.axisX && sc.axisY) {
        // Two independent draws, X first: a square cloud, not a diagonal line.
        const double jitterY = (2.0 * state.nextRandom() - 1.0) * diameter * sensorValue;
        dab.center.x += jitter;
        dab.center.y += jitterY;
        return;
    }

    // One axis: the jitter lies ALONG the stroke (X) or ACROSS it (Y) -- axes of the stroke's own
    // frame, not the document's. A due-east stroke scatters horizontally under X and vertically
    // under Y; before any motion the drawing angle is 0 and the two coincide with the document's.
    const double a = state.drawingAngle();
    if (sc.axisX) {
        dab.center.x += std::cos(a) * jitter;
        dab.center.y += std::sin(a) * jitter;
    } else {
        // The normal: (-sin, cos), the drawing direction turned a quarter left.
        dab.center.x += -std::sin(a) * jitter;
        dab.center.y += std::cos(a) * jitter;
    }
}

void applyMirror(const MirrorOption& mo, StrokeState& state, Dab& dab) {
    if (!mo.option.isChecked() || (!mo.horizontal && !mo.vertical))
        return; // inert before the draw, same contract as applyScatter

    // The reference folds the canvas's own mirror state in as an increment here; the engine has no
    // canvas mirror (dabAngle records the same fact), so the option's flip is the whole parity.
    const bool flip = mo.option.sizeLikeValue(state) >= 0.5;
    dab.mirrorH = flip && mo.horizontal;
    dab.mirrorV = flip && mo.vertical;
}

void applySharpnessSnap(double sharpness, double extentW, double extentH, Dab& dab) {
    // The reference snaps the mask's TOP-LEFT `pt = center - halfExtent`, not the centre: `snapped =
    // sharpness*round(pt) + (1 - sharpness)*pt`. The centre delta is `snapped - pt = sharpness*
    // (round(pt) - pt)`, so shifting the centre by it makes placeDab (which recomputes `topLeft =
    // center - 0.5*extent`) land on the snapped top-left. At sharpness 1 that is an exact integer ->
    // zero sub-pixel phase; at 0 the delta is 0 and nothing moves. `std::round` matches Qt's qRound
    // (both round halves away from zero) over the finite coordinates a dab ever has.
    const double tlx = dab.center.x - 0.5 * extentW;
    const double tly = dab.center.y - 0.5 * extentH;
    dab.center.x += sharpness * (std::round(tlx) - tlx);
    dab.center.y += sharpness * (std::round(tly) - tly);
}

common::Color8 hsvAdjust(common::Color8 base, double dh, double ds, double dv) {
    // Uint8 -> float is `a / 255` (the reference's Uint8ToFloat LUT); the transform runs in float,
    // narrowing dh/ds/dv exactly as the reference does when it hands its double `m_adj_*` to the
    // float-parameter HSVTransform.
    float r = static_cast<float>(base.r) / 255.0f;
    float g = static_cast<float>(base.g) / 255.0f;
    float b = static_cast<float>(base.b) / 255.0f;
    hsvTransformRgb(r, g, b, static_cast<float>(dh), static_cast<float>(ds),
                    static_cast<float>(dv));
    // float -> uint8 is round-to-nearest with a [0,255] clamp (KoColorSpaceMaths<float,quint8>::
    // scaleToA over the already-[0,1]-clamped channel). Alpha passes through untouched.
    const auto q = [](float value) noexcept -> std::uint8_t {
        value = std::clamp(value, 0.0f, 1.0f);
        return static_cast<std::uint8_t>(value * 255.0f + 0.5f);
    };
    return common::Color8{q(r), q(g), q(b), base.a};
}

common::Color8 applyColorDynamics(const BrushOptions& options, common::Color8 base,
                                  StrokeState& state) {
    const bool hueOn = options.hue && options.hue->isChecked();
    const bool satOn = options.saturation && options.saturation->isChecked();
    const bool valOn = options.value && options.value->isChecked();

    // Inert: with NO channel checked the reference never builds the colour transformation at all, so
    // the paint colour passes through untouched and NO draw happens. Returning `base` here rather
    // than hsvAdjust(base, 0, 0, 0) also keeps the identity BYTE-EXACT -- an all-identity HSV round
    // trip is only near-exact in 8-bit -- and "an inert option changes nothing" has to be exact, the
    // same contract Scatter and Mirror keep. (When at least one channel IS checked the reference
    // transforms the colour even where a channel's own adjustment computes to zero, and so does the
    // path below -- so a checked-but-neutral value still round-trips the colour, exactly as it does.)
    if (!hueOn && !satOn && !valOn)
        return base;

    // The three channels in the reference's evaluation order (hue, saturation, value). Each draws
    // from the stroke's random streams ONLY when checked -- the reference's apply() returns before
    // touching the sensors on an unchecked option -- so a preset that drives only `v` rotates no hue,
    // touches no saturation and draws exactly one value.
    double dh = 0.0;
    double ds = 0.0;
    double dv = 0.0;
    if (hueOn)
        dh = options.hue->rotationLikeValue(state, /*normalizedBaseAngle=*/0.0,
                                            /*absoluteAxesFlipped=*/false, /*scalingPartCoeff=*/1.0,
                                            /*disableScalingPart=*/false);
    if (satOn)
        ds = hsvSizeLikeAdjust(*options.saturation, state);
    if (valOn)
        dv = hsvSizeLikeAdjust(*options.value, state);
    return hsvAdjust(base, dh, ds, dv);
}

double dabAngle(const BrushOptions& options, double baseAngleRad, StrokeState& state) {
    // Rotation ADDS to the tip's authored angle rather than replacing it: the preset's `angle` is a
    // property of the tip, the option is a property of the stroke, and a preset that authors a slanted
    // nib and then drives rotation from pressure means both.
    //
    // The engine has no canvas rotation and no document mirror to hand it, so the base angle is 0 and
    // the axes are unflipped. When a rotated canvas exists, THIS is where it enters -- and a preset
    // with an absolute `drawingangle` sensor ignores it anyway, because that sensor IS the angle.
    if (!options.rotation || !options.rotation->isChecked())
        return baseAngleRad;
    const double turns = options.rotation->rotationLikeValue(state, /*normalizedBaseAngle=*/0.0,
                                                             /*absoluteAxesFlipped=*/false,
                                                             /*scalingPartCoeff=*/1.0,
                                                             /*disableScalingPart=*/false);
    return baseAngleRad + turns * kRotationTurnRad;
}

double reticleDabAngle(const BrushOptions& options, double baseAngleRad, const StrokeInput& pen,
                       double headingRad) {
    if (!options.rotation || !options.rotation->isChecked())
        return baseAngleRad;
    if (options.rotation->isRandom() || !std::isfinite(headingRad))
        return baseAngleRad; // noise is not a direction, and neither is "hasn't moved yet"

    // A private state, so that drawing the ring cannot perturb the stroke: `rotationLikeValue` latches
    // the locked drawing angle and would draw from the random stream (ruled out above, but the engine's
    // live StrokeState is const to us anyway, and rightly).
    //
    // A heading is not something a StrokeState can be TOLD -- it is derived by moving. So it is seeded
    // through the snapshot, which is the same door the dab walk uses to put a dab back on its own point
    // of the stroke.
    StrokeState state;
    state.begin(pen, /*seed=*/0); // the seed is dead: isRandom() already refused every RNG reader
    StrokeSnapshot at = state.snapshot();
    at.drawingAngle = headingRad;
    state.rewindTo(at);

    const double turns = options.rotation->rotationLikeValue(state, /*normalizedBaseAngle=*/0.0,
                                                             /*absoluteAxesFlipped=*/false,
                                                             /*scalingPartCoeff=*/1.0,
                                                             /*disableScalingPart=*/true);
    return baseAngleRad + turns * kRotationTurnRad;
}

} // namespace mosaic::core::brush
