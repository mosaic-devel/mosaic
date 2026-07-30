#include "core/brush/brush_engine.hpp"

#include "core/brush/stroke_path.hpp"

#include "core/blend_math.hpp"
#include "core/brush/bitmap_tip.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace mosaic::core::brush {

namespace {

[[nodiscard]] double clamp01(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

// The bounded working rect grows in tile-aligned chunks so reallocations are rare (amortized) even
// on a long stroke -- a small enough tile to keep a tiny stroke cheap, large enough that a sweeping
// stroke doesn't realloc per dab.
constexpr int kTile = 256;

// The dab interval along ONE of a dab's axes, in document px, from that axis's extent. Auto-spacing
// (docs/brushes.md §3.5) is an ABSOLUTE step -- `coeff * sqrt(extent)` for extents >= 1 px, linear
// below -- so a bigger brush lays relatively denser dabs; otherwise the interval is the usual
// fraction of the extent. Floored so a tiny tip can never spin the dab loop forever.
//
// ⚠ THE FLOOR IS PER AXIS, not on the step the walk finally takes -- which is where the format puts
// it, and the two are NOT the same thing. On either axis they agree exactly; OFF-axis they do not. A
// 64 x 0.5 nib at spacing 1/8 wants sy = 0.0625: floor the axis and its 45-degree step is 0.71 px;
// floor the step and it is 0.50, so the nib is stamped half again as densely as it should be. Only a
// DIAGONAL probe can see the difference, and a test that drags along the two axes passes on both.
constexpr double kMinSpacingPx = 0.5;

// The non-termination backstop on the AIRBRUSH's timed cadence (§6.6h), DERIVED rather than picked:
// a span may spend at most `kMaxSpanBudgetMs` of elapsed time on timed dabs and each one costs at
// least `kMinTimedIntervalMs`, so this many is all the budget can ever pay for. The `+ 2` covers the
// carried remainder that arms the first dab and the arming that follows the last one.
//
// ⚠ IT IS A BACKSTOP AND NOT A LIMIT, and the difference is the whole point of the fix that put the
// budget here. It was 100000 and that was not a safety net -- it WAS the freeze, because the loop
// had nothing else to stop it and 100000 dab blits per mouse event is a hang. Reaching this number
// now means the budget arithmetic below is wrong, never that a stroke was long.
constexpr int kMaxTimedDabsPerSpan =
    static_cast<int>(kMaxSpanBudgetMs / kMinTimedIntervalMs) + 2;

[[nodiscard]] double spacingInterval(double extent, double spacing, bool autoSpacing, double coeff,
                                     double scale) {
    // The Spacing OPTION's per-dab `scale` multiplies the raw interval, both axes, exactly as the
    // reference's `spacing *= extraScale` (§6.6e) -- BEFORE the half-pixel floor, so a scale toward
    // zero cannot spin the dab loop forever. `scale == 1.0` (no option / unchecked) leaves `px * 1.0
    // == px` to the bit, which is what keeps every spacing golden byte-identical.
    const double px = (autoSpacing ? coeff * (extent < 1.0 ? extent : std::sqrt(extent))
                                    : spacing * extent) *
                      scale;
    return std::max(px, kMinSpacingPx);
}

} // namespace

// On a straight stroke this lands the next dab exactly where the reference's own walk does -- it
// accumulates displacement in the ellipse's frame and solves for the crossing of the unit ellipse,
// which is the same equation read from the other end. (Ours re-reads the heading at every dab, so it
// also tracks a CURVE, which an accumulator in a fixed frame does not.)
double spacingStepAlong(const SpacingEllipse& e, double headingRad) noexcept {
    if (e.sx == e.sy)
        return e.sx; // already floored per axis; see the header for why this branch must exist
    const double phi = headingRad - e.rot; // the heading, in the tip's own frame
    const double c = std::cos(phi) / e.sx;
    const double s = std::sin(phi) / e.sy;
    const double r = std::sqrt(c * c + s * s);
    // Both semi-axes are >= kMinSpacingPx, so the radius is too, in every direction; the guard is
    // against a non-finite heading, not a small one.
    return r > 0.0 ? 1.0 / r : kMinSpacingPx;
}

void BrushEngine::Box::add(int x, int y) {
    if (!valid) {
        x0 = x;
        y0 = y;
        x1 = x + 1;
        y1 = y + 1;
        valid = true;
        return;
    }
    x0 = std::min(x0, x);
    y0 = std::min(y0, y);
    x1 = std::max(x1, x + 1);
    y1 = std::max(y1, y + 1);
}

common::Rect BrushEngine::Box::rect() const {
    if (!valid)
        return {};
    return {static_cast<double>(x0), static_cast<double>(y0), static_cast<double>(x1 - x0),
            static_cast<double>(y1 - y0)};
}

double dabCoverage(double d, double R, double hardness) {
    if (R <= 0.0 || d >= R)
        return 0.0; // outside the tip, or a degenerate radius
    const double h = clamp01(hardness);
    const double edge1 = R;   // coverage reaches 0 here
    double edge0 = R * h;     // ... and is solid (1) inside here
    if (edge0 > edge1 - 0.75) // guarantee a ~0.75 px AA rim even for a hard dab
        edge0 = edge1 - 0.75;
    const double denom = std::max(edge1 - edge0, 1e-4);
    const double t = (edge1 - d) / denom;
    if (t <= 0.0)
        return 0.0;
    if (t >= 1.0)
        return 1.0;
    return t * t * (3.0 - 2.0 * t); // smoothstep shoulder
}

double maskingOp(MaskingOp op, double mask, double alpha) noexcept {
    // Transcribed from the reference's masking composite functions (docs/brushes.md §6.2), in real
    // arithmetic over [0,1]. The reference's linear_dodge zero-guards a zero destination so the
    // mask cannot paint alone; composite() gates on the paint stroke's own accumulation before
    // calling this, which subsumes that guard for every op.
    switch (op) {
    case MaskingOp::Subtract:
        return std::max(0.0, alpha - mask);
    case MaskingOp::LinearDodge:
        return std::min(1.0, alpha + mask);
    case MaskingOp::Multiply:
        break;
    }
    return mask * alpha;
}

double blendAverageOpacity(double opacity, double average) noexcept {
    // Transcribed from the reference's painter: rise instantly, decay at a fixed 0.1 exponent per
    // dab. The asymmetry is the point -- a stroke that presses harder mid-way raises its ceiling at
    // once, while one that eases off keeps striving near the ceiling it already earned instead of
    // carving a staircase down through its own paint.
    const double exponent = 0.1;
    return average < opacity ? opacity : exponent * opacity + (1.0 - exponent) * average;
}

double washAlphaDarkenAlpha(double dst, double cov, double flow, double opacity,
                            double averageOpacity) noexcept {
    // Transcribed from the reference's indirect-painting composite (default parameterization),
    // specialized to the alpha channel over a single-colour source -- docs/brushes.md §6.2 carries
    // the derivation. `lerp(a, b, t) = a + (b - a) * t` throughout.
    const double src = cov * opacity;
    double full;
    if (averageOpacity > opacity) {
        // The stroke has been louder than this dab: strive toward the running average, scaled by
        // how far this pixel already got (dst / average), so consecutive quiet dabs aim the alpha
        // at the ceiling the stroke earned rather than at their own lower one.
        full = averageOpacity > dst ? src + (averageOpacity - src) * (dst / averageOpacity) : dst;
    } else {
        // The plain case: this dab pulls the alpha toward its own ceiling, as hard as its mask
        // covers the pixel -- and NEVER down. A dab whose ceiling is below what the paint already
        // reached leaves it standing.
        full = opacity > dst ? dst + (opacity - dst) * cov : dst;
    }
    // The reference short-circuits full flow on exact equality; below it, flow interpolates the
    // whole step from "no change" (its default zero-flow alpha is the destination).
    return flow == 1.0 ? full : dst + (full - dst) * flow;
}

std::uint8_t sharpnessThreshold(std::uint8_t v, double threshold, int softness) noexcept {
    // Integer 8-bit arithmetic, verbatim from KisSharpnessOption::applyThreshold (OPACITY_OPAQUE_U8
    // = 255). The double->uint32 cast truncates toward zero, and the soft-band division is integer,
    // both exactly as the reference. softness is [0,100] (clamped at import), so `100 - softness` is
    // never negative before it becomes unsigned.
    const std::uint32_t tolerance = static_cast<std::uint32_t>(255.0 - threshold * 255.0);
    if (static_cast<std::uint32_t>(v) > tolerance)
        return 255;
    if (static_cast<std::uint32_t>(v) <=
        (static_cast<std::uint32_t>(100 - softness) * tolerance) / 100u)
        return 0;
    return v; // in the soft range: keep the original value
}

StrokeAccumulator chooseAccumulator(TipApplication application, bool colorDynamicsActive,
                                    bool painterVariesColor) {
    // §6.1: anything a single coverage channel cannot express -- a tip that stamps colour, an option
    // that varies the colour per dab, or a StrokePainter that picks a colour per mark -- needs
    // `Colored`; everything else takes the fast path. The shipped default set is 100 % AlphaMask, so
    // it all lands on `Uniform` bar the colour-dynamics preset and the one sketch preset that
    // randomizes its connection colour; third-party packs (ABR colour dynamics, RGBA hose cells) are
    // what land here otherwise.
    if (application != TipApplication::AlphaMask || colorDynamicsActive || painterVariesColor)
        return StrokeAccumulator::Colored;
    return StrokeAccumulator::Uniform;
}

double smudgeColorRateOpacity(double colorRate, double opacity, double maxSmudgeRate) noexcept {
    // Transcribed from the legacy strategy: lerp(0, maxColorRate, colorRate * opacity), where the
    // ceiling keeps a 0.2 floor however hard the brush smears. lerp from 0 is a product; the
    // reference clamps the result to [0,1] and so does this.
    const double maxColorRate = std::max(1.0 - maxSmudgeRate, 0.2);
    return clamp01(maxColorRate * (colorRate * opacity));
}

std::uint64_t strokeSeedFor(std::uint64_t base, const StrokeInput& first) noexcept {
    // Every channel of the first sample goes in, by its BIT PATTERN: two presses a hundredth of a
    // pixel apart must not collide, so the doubles are not rounded or quantized on the way in.
    const auto bits = [](double v) noexcept {
        std::uint64_t u = 0;
        std::memcpy(&u, &v, sizeof(u));
        return u;
    };
    const std::uint64_t parts[] = {bits(first.pos.x),   bits(first.pos.y),
                                   bits(first.pressure), bits(first.xTilt),
                                   bits(first.yTilt),    first.timeUs};
    std::uint64_t h = base;
    for (const std::uint64_t p : parts) {
        // Fold, then ONE splitmix64 finalizer round. The fold alone is not enough: a press at
        // integer coordinates with a zero tilt feeds long runs of identical low bits, and the
        // finalizer is what makes the first draw of the stroke's stream move for a one-microsecond
        // difference in `timeUs` (which is the only channel that separates two taps at one point).
        h ^= p + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        h += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = h;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        h = z ^ (z >> 31);
    }
    return h;
}

double airbrushIntervalMs(double rate, double rateScale) noexcept {
    // Transcribed from the reference's effective-timing step: the airbrush option supplies
    // `1000 / rate` ms and the Rate option's value DIVIDES it, so a value of 1 leaves the authored
    // rate alone and a value of 0 means "never". `kLongTime` stands in for the reference's own
    // ten-year sentinel: a stroke that reaches it lays no timed dab, which is the point.
    constexpr double kLongTime = 320000000000.0;
    if (!(rate > 0.0) || !std::isfinite(rate))
        return kLongTime;
    const double interval = 1000.0 / rate;
    if (!(rateScale > 0.0) || !std::isfinite(rateScale))
        return kLongTime;
    const double scaled = interval / rateScale;
    if (!std::isfinite(scaled))
        return kLongTime;
    // The reference bounds the interval into [MIN_TIMED_INTERVAL, LONG_TIME] where it is consumed.
    return std::clamp(scaled, kMinTimedIntervalMs, kLongTime);
}

int HaltonSequence::generate(int maxRange) noexcept {
    // The reference's generationStep, verbatim: walk the radical-inverse sequence n/d in `base`.
    const int x = m_d - m_n;
    if (x == 1) {
        m_n = 1;
        m_d *= m_base;
    } else {
        int y = m_d / m_base;
        while (x <= y)
            y /= m_base;
        m_n = (m_base + 1) * y - x;
    }
    // ...then read the current point scaled onto [0, maxRange], integer-rounded.
    return (m_n * maxRange + m_d / 2) / m_d;
}

SmudgeRect smudgeSampleRect(const SmudgeRect& src, double radius) noexcept {
    // The reference's minimalRect: 1x1 at the CLOSED rect's integer midpoint -- (x1 + x2) / 2 with
    // C++'s truncation toward zero, which for a negative sum is NOT `x + (w-1)/2`. Dabs hang off
    // the top-left of a document routinely, so the negative case is not theoretical.
    const SmudgeRect minimal{static_cast<int>((2LL * src.x + src.w - 1) / 2),
                             static_cast<int>((2LL * src.y + src.h - 1) / 2), 1, 1};
    if (!(radius > 0.0))
        return minimal;
    // blowRect: adjusted by extent * coeff per axis, TRUNCATED toward zero (the reference computes
    // the offsets in the rect's own integer type). radius < 1 shrinks -- but never to nothing: a
    // positive radius has coeff > -0.5, so `w - 2 * floor(|coeff| * w) >= 1` and the blown rect
    // always keeps at least the pixel column the midpoint sits in. The union below only irons out
    // the truncation corners of a negative-coordinate midpoint, exactly as the reference's rect
    // union does.
    const double coeff = 0.5 * (radius - 1.0);
    const int dw = static_cast<int>(src.w * coeff);
    const int dh = static_cast<int>(src.h * coeff);
    const SmudgeRect blown{src.x - dw, src.y - dh, src.w + 2 * dw, src.h + 2 * dh};
    const int x0 = std::min(blown.x, minimal.x);
    const int y0 = std::min(blown.y, minimal.y);
    const int x1 = std::max(blown.x + blown.w, minimal.x + 1);
    const int y1 = std::max(blown.y + blown.h, minimal.y + 1);
    return {x0, y0, x1 - x0, y1 - y0};
}

Dab BrushEngine::resolveDab(common::Vec2 center, double pressure) {
    // The dab counter advances FIRST: `fade` ramps over dabs, and it must read this dab's index, not
    // the last one's. It advances for clipped and zero-flow dabs too, so a stroke running off the
    // document edge keeps the dab sequence it would have had in the middle of the canvas.
    m_stroke.beginDab();

    DabBase base;
    base.diameter = m_params.diameter;
    base.ratio = m_params.ratio;
    base.angleRad = m_params.angleRad;
    base.flow = m_params.flow;

    Dab d;
    if (m_params.options) {
        // The caller has already put m_stroke at this dab's own point on the stroke.
        d = evaluateDab(*m_params.options, base, center, m_stroke);
    } else {
        d.center = center;
        d.diameter = base.diameter;
        d.ratio = base.ratio;
        d.angleRad = base.angleRad;
        d.flow = base.flow;
    }

    // BrushDynamics' two pressure booleans are the S19-a hooks the canvas still drives, and they are
    // exactly a Size and a Flow option with a pressure sensor and an identity curve. They apply ON TOP
    // of the option pipeline rather than beside it, so a preset that carries both is scaled once by
    // each -- and with no options at all, `base.diameter * clamp01(pressure)` is the identical
    // expression the engine evaluated before any of this existed. (Arc D's preset work supersedes
    // them: when a preset supplies a Size option, the canvas stops setting the bool.)
    if (m_dyn.sizeFromPressure)
        d.diameter *= clamp01(pressure);
    if (m_dyn.flowFromPressure)
        d.flow *= clamp01(pressure);

    // The per-dab opacity, evaluated beside the dab -- it is a property of the stroke's
    // ACCUMULATION, not of the dab's shape, which is why it is not a Dab field (dab.hpp). Once per
    // dab, in order, BEFORE the frame selection below and after evaluateDab above -- it can draw
    // from the same random streams the other options do, so its place in the draw sequence is part
    // of the stroke's replay contract. WITHOUT the strength: that is the whole stroke's ceiling
    // (m_cap), and folding it in here as well would square it. Advances for clipped and zero-flow
    // dabs exactly like the dab counter -- the average is a property of the stroke's geometry.
    if (m_dynOpacity) {
        m_dabOpacity =
            m_params.options->opacity->sizeLikeValue(m_stroke, /*useStrength=*/false);
        m_avgOpacity = blendAverageOpacity(m_dabOpacity, m_avgOpacity);
    } else if (m_buildOpacity) {
        // BUILDUP (§6.6i): the same option, read at the same point in the draw sequence, and the
        // ONE difference is what it drives. Direct painting has no stroke temp to strive toward, so
        // there is no running average to advance either -- the reference keeps updating one, but
        // nothing reads it outside its indirect composite. The value is still the sensors WITHOUT
        // the strength, because `m_cap` is where the strength lives in Mosaic (and where the
        // context bar's Opacity lands): `sensor * m_cap` IS the reference's own
        // `computeSizeLikeValue(info, /*useStrengthValue=*/true)` with the bar's ceiling standing in
        // for the preset's strength -- the same split the Wash path makes, applied to the deposit
        // instead of to the ceiling. Folding the strength in HERE as well would square it.
        m_dabOpacity = m_params.options->opacity->sizeLikeValue(m_stroke, /*useStrength=*/false);
    }

    // The smudge per-dab values (§6.6c), in the fixed order rate -> colour rate -> radius ->
    // opacity, after the wash pair above -- any of them can draw from the stroke's random streams,
    // so the order is part of the replay contract, and they advance for clipped dabs exactly like
    // the dab counter. The unchecked fallbacks are the reference's own hard-coded ones (dab.hpp's
    // kSmudge* specs), NOT the option's static strength: rate 1 (smear fully), colour rate 0
    // (deposit nothing -- what makes a blender a blender), radius 0 (sample one pixel). All WITH
    // strength, opacity included: colorsmudge is direct painting, so there is no stroke-level cap
    // for the strength to become.
    if (m_smudgeActive) {
        const BrushOptions* o = m_params.options.get();
        const auto checkedSizeLike = [&](const std::optional<CurveOption>& opt, double fallback) {
            return opt && opt->isChecked() ? opt->sizeLikeValue(m_stroke) : fallback;
        };
        m_dabSmudgeRate = o != nullptr ? checkedSizeLike(o->smudgeRate, 1.0) : 1.0;
        m_dabColorRate = o != nullptr ? checkedSizeLike(o->colorRate, 0.0) : 0.0;
        m_dabSmudgeRadius = o != nullptr ? checkedSizeLike(o->smudgeRadius, 0.0) : 0.0;
        m_dabSmudgeOpacity =
            o != nullptr && o->opacity ? o->opacity->sizeLikeValue(m_stroke) : 1.0;
    }

    // Which cell of an animated tip this dab stamps -- the one part of a Dab that `evaluateDab` does
    // not own, because choosing it needs the tip (dab.hpp). It happens HERE, in the once-per-dab step,
    // rather than in stamp(): a `Random` hose dimension draws from the same per-dab stream the `fuzzy`
    // sensors do, so selecting a frame for a dab that stamp() then CLIPS away would still have to
    // happen -- or a stroke running off the document edge would come back with a different cell
    // sequence than the same stroke in the middle of the canvas. A tip with no parasite (`dim == 0`)
    // draws nothing at all and answers 0, so this is free for the 24 non-hose bitmap tips.
    if (m_params.tip) {
        if (const BitmapTip* bmp = m_params.tip->bitmap(); bmp != nullptr)
            d.frame = m_hose.selectFrame(bmp->hose(), bmp->frameCount(), m_stroke,
                                         m_stroke.snapshot().sample);
    }

    // The two positional options (§6.6d), LAST among the per-dab draws and in this order --
    // appending to the stream keeps every pre-scatter golden byte-identical, and Scatter comes
    // before Mirror because its amplitude must not depend on a flip that hasn't been decided.
    // After the frame selection on purpose: the jitter's amplitude is the extents of the frame
    // this dab actually stamps (the reference reads its pipe brush's current mask dims -- the
    // PREVIOUS dab's frame; this dab's own is the same fact without the off-by-one). Scatter
    // rides the smudge walk too -- the reference's colorsmudge scatters its dab position -- and
    // the smear anchor tracks the scattered rect centres downstream, exactly as its reference
    // records the scattered dstDabRect's centre. Mirror never gets here on a smudge preset: its
    // slot stays empty (preset_brush.cpp).
    if (m_params.options && m_params.options->scatter) {
        // The rotated tip's axis-aligned extents -- what a mask raster measures. Without a tip
        // the analytic ellipse's own envelope stands in, as it does for the spacing cadence
        // (no preset walks tipless: this is the same belt-and-braces as dabSpacingEllipse).
        double w = d.diameter;
        double h = d.diameter * d.ratio;
        if (m_params.tip) {
            const DabExtent ext = dabExtent(tipDabShape(*m_params.tip, d.frame, d.diameter,
                                                        d.ratio, d.angleRad, false, false));
            w = ext.width;
            h = ext.height;
        }
        applyScatter(*m_params.options->scatter, w, h, m_stroke, d);
    }
    if (m_params.options && m_params.options->mirror)
        applyMirror(*m_params.options->mirror, m_stroke, d);

    // Sharpness (§6.6e), the LAST of the positional options -- after Scatter has jittered the centre,
    // because the reference snaps the SCATTERED position to the grid. Its per-dab value drives BOTH
    // the mask alpha threshold (stampTipDab, whenever the option is checked) and -- when alignOutline
    // is set and the static strength is > 0 (m_sharpnessSnap) -- the pixel-grid snap of the centre,
    // which measures the rotated tip's extents exactly as Scatter does. ONE draw, shared by both;
    // inert (unchecked, or dropped under smudge) it draws nothing and the centre holds.
    if (m_sharpnessActive) {
        m_dabSharpness = m_params.options->sharpness->option.sizeLikeValue(m_stroke);
        if (m_sharpnessSnap) {
            double w = d.diameter;
            double h = d.diameter * d.ratio;
            if (m_params.tip) {
                const DabExtent ext = dabExtent(tipDabShape(*m_params.tip, d.frame, d.diameter,
                                                            d.ratio, d.angleRad, false, false));
                w = ext.width;
                h = ext.height;
            }
            applySharpnessSnap(m_dabSharpness, w, h, d);
        }
    }

    // The Spacing cadence scale (§6.6e), evaluated LAST among the per-dab draws -- the reference
    // computes its KisSpacingOption AFTER laying the dab, to size the step to the NEXT one, and the
    // value can draw from the random streams. dabSpacingEllipse reads this member and multiplies the
    // interval by it (WITH strength, over [0,1]); absent or unchecked it stays exactly 1.0, so the
    // spacing goldens hold. It rides the smudge walk too, exactly as the reference's colorsmudge
    // spaces its dabs through the same option.
    m_dabSpacingScale = 1.0;
    if (m_params.options && m_params.options->spacing && m_params.options->spacing->isChecked())
        m_dabSpacingScale = m_params.options->spacing->sizeLikeValue(m_stroke);

    // Colour dynamics (§6.6f), the LAST per-dab draws -- APPENDED after the spacing scale so every
    // prior golden's random stream is byte-identical, exactly as Scatter/Mirror/Sharpness/Spacing
    // appended before it. The HSV options adjust the paint colour into m_dabDynColor (beginDeposit
    // reads it); the three channels draw in the reference's order (hue, saturation, value), each
    // only when checked -- an inert set draws nothing and leaves the flat colour.
    if (m_colorDynamicsActive)
        m_dabDynColor = applyColorDynamics(*m_params.options, m_params.color, m_stroke);

    // The hatching quartet (§6.6g), the LAST per-dab draws -- appended after the colour dynamics so
    // every prior golden's random stream is byte-identical, exactly as each family before it
    // appended. The order is the reference's own assignment order in its `paintAt`: angle,
    // crosshatching, separation, thickness. All four read WITH strength; an UNCHECKED one still
    // reads (the reference's `KisStandardOption::apply` returns 1.0 there without touching the
    // sensors, which is what `standardOptionValue` does), so an unchecked Thickness is 1.0 and an
    // unchecked Separation leaves the static separation alone via its own `checked` gate.
    if (m_hatchActive && m_params.options) {
        const BrushOptions& o = *m_params.options;
        m_dabHatch.angle = standardOptionValue(o.hatchAngle, m_stroke);
        m_dabHatch.crosshatching = standardOptionValue(o.crosshatching, m_stroke);
        m_dabHatch.separation = standardOptionValue(o.separation, m_stroke);
        m_dabHatch.thickness = standardOptionValue(o.thickness, m_stroke);
    }

    // The TEXTURE strength and the AIRBRUSH rate (§6.6h), the LAST per-dab draws -- appended after
    // the hatching quartet so every prior golden's random stream is byte-identical, exactly as each
    // family before them appended. Both read through the reference's `KisStandardOption::apply`
    // (checked -> size-like value WITH strength, unchecked -> exactly 1.0, without touching the
    // sensors), which is what `standardOptionValue` is.
    //
    // The rate is drawn LAST OF ALL for the same reason the spacing scale is drawn late: the
    // reference re-computes its timing AFTER laying a dab, to size the interval to the NEXT one.
    // Both are gated on their frozen flags, so an inert texture and an inert airbrush draw nothing.
    if (m_textureActive && m_params.options)
        m_dabTextureStrength = standardOptionValue(m_params.options->textureStrength, m_stroke);
    if (m_airbrushActive && m_params.options)
        m_dabRateScale = standardOptionValue(m_params.options->rate, m_stroke);

    // The floors the engine has always applied, moved here so that everything downstream -- the
    // stamp, the bbox and the spacing cadence -- reads ONE resolved diameter and cannot disagree
    // about it.
    d.diameter = std::max(d.diameter, 0.1);
    d.flow = clamp01(d.flow);
    return d;
}

SpacingEllipse BrushEngine::dabSpacingEllipse(const Dab& dab) const {
    // The dab's two extents, as the TIP paints them -- the same geometry stampTipDab() asks for, so
    // the cadence and the stamp cannot disagree about how big a dab is. Without a tip this is the
    // analytic circle's own envelope, which is what `(diameter, diameter * ratio)` already means.
    double w = dab.diameter;
    double h = dab.diameter * dab.ratio;
    double rot = dab.angleRad;
    if (m_params.tip != nullptr) {
        const DabShape s = tipDabShape(*m_params.tip, dab.frame, dab.diameter, dab.ratio,
                                       dab.angleRad, dab.mirrorH, dab.mirrorV);
        w = s.width;
        h = s.height;
        rot = s.angleRad;
    }

    SpacingEllipse e;
    if (m_params.isotropicSpacing) {
        // The author's opt-out: one interval, from the LARGER extent, the same in every direction.
        // The angle goes with it -- a circle has no orientation to carry.
        const double d = std::max(w, h);
        e.sx = spacingInterval(d, m_params.spacing, m_params.useAutoSpacing,
                               m_params.autoSpacingCoeff, m_dabSpacingScale);
        e.sy = e.sx;
        e.rot = 0.0;
        return e;
    }
    e.sx = spacingInterval(w, m_params.spacing, m_params.useAutoSpacing, m_params.autoSpacingCoeff,
                           m_dabSpacingScale);
    e.sy = spacingInterval(h, m_params.spacing, m_params.useAutoSpacing, m_params.autoSpacingCoeff,
                           m_dabSpacingScale);
    e.rot = rot;
    return e;
}

double BrushEngine::maskingDiameter(double pressure) const {
    const MaskingParams& mp = m_params.masking;
    double dia = mp.diameter;
    if (mp.sizeFromPressure)
        dia *= clamp01(pressure);
    return std::max(dia, 0.1);
}

SpacingEllipse BrushEngine::maskingSpacingEllipse(double pressure) const {
    const MaskingParams& mp = m_params.masking;
    const double dia = maskingDiameter(pressure);
    // Without a tip, both extents are the one diameter the analytic disc has: sx == sy, so
    // spacingStepAlong takes its scalar branch and the step is bit-for-bit `spacingInterval(dia)`
    // -- the step this walk always took. A round procedural tip lands on the same branch through
    // `dia * 1.0 == dia`.
    double w = dia;
    double h = dia;
    double rot = 0.0;
    if (mp.tip != nullptr) {
        const DabShape s = tipDabShape(*mp.tip, 0, dia, mp.ratio, mp.angleRad, false, false);
        w = s.width;
        h = s.height;
        rot = s.angleRad;
    }
    // The masking walk drives no options, so its cadence never carries a Spacing scale: 1.0 keeps
    // `px * 1.0 == px` and every masking-spacing golden byte-identical.
    SpacingEllipse e;
    e.sx = spacingInterval(w, mp.spacing, mp.useAutoSpacing, mp.autoSpacingCoeff, 1.0);
    e.sy = spacingInterval(h, mp.spacing, mp.useAutoSpacing, mp.autoSpacingCoeff, 1.0);
    e.rot = rot;
    return e;
}

double BrushEngine::strokeAlphaCap() const {
    const double o = clamp01(m_params.opacity);
    // An eraser has no colour to be transparent: only its opacity limits how much it carves.
    if (m_params.strokeMode == StrokeMode::Erase)
        return o;
    return o * (m_params.color.a / 255.0);
}

void BrushEngine::begin(std::uint32_t width, std::uint32_t height, common::Image& target,
                        const BrushParams& params, const BrushDynamics& dynamics,
                        StrokeInput first) {
    m_w = width;
    m_h = height;
    m_target = &target;
    m_params = params;
    m_dyn = dynamics;
    // No document-sized allocation: the coverage + base snapshot start empty and grow with the
    // stroke (ensureCovers), so begin() costs ~one dab even on a 5k x 8k layer (S60-c).
    m_ox = m_oy = 0;
    m_cw = m_ch = 0;
    m_coverage.clear();
    m_build.clear();
    m_colored.clear();
    m_mask.clear();
    m_base = common::Image{};
    m_smudgeState.clear();
    m_baseFilled.clear();
    // The smudge gate, frozen with everything else (§6.6c): SmudgeParams::enabled, a REAL tip (the
    // smudge walk stamps through the tip raster), a Paint stroke. When it holds, the smudge state
    // buffer owns the whole accumulation, so the axes the params COPY carries are normalized away
    // rather than half-honoured -- the reference's colorsmudge has no wash/buildup, no colour
    // accumulator, no masking brush and composites its colour rate as plain OVER (the mapper
    // badges any preset that authors otherwise).
    m_smudgeActive = m_params.smudge.enabled && m_params.tip != nullptr &&
                     m_params.strokeMode == StrokeMode::Paint;
    if (m_smudgeActive) {
        m_params.paintMode = PaintMode::Wash;              // buildup() false: no m_build
        m_params.accumulator = StrokeAccumulator::Uniform; // colored() false: no m_colored
        m_params.masking.enabled = false;                  // no masking walk beside the smudge walk
        m_params.blendMode = BlendMode::Normal;            // composite() takes the smudge branch
    }
    // The SECOND ENGINE KIND (§6.6g), frozen beside the smudge gate and losing to it: a preset is a
    // sketch brush or a smudge brush, never both, and half-running the two would be worse than
    // either. A painter that fails to build leaves the dab walk in place rather than a stroke that
    // paints nothing.
    m_painter.reset();
    m_painterActive = !m_smudgeActive && m_params.painter.kind != StrokePainterKind::None;
    if (m_painterActive) {
        m_painter = makeStrokePainter(m_params.painter);
        m_painterActive = m_painter != nullptr;
    }
    // The hatching gate (§6.6g), frozen with the other two. It is a DAB engine, so it needs the dab
    // walk to be the thing running -- no painter, no smudge -- and a REAL tip, because the lattice
    // is stencilled by the tip's own mask and a tipless brush has none.
    m_hatchActive = m_params.hatching.enabled && !m_smudgeActive && !m_painterActive &&
                    m_params.tip != nullptr;
    m_dabHatch = HatchingDabValues{};
    if (m_hatchActive && m_params.options) {
        const BrushOptions& o = *m_params.options;
        m_dabHatch.angleChecked = o.hatchAngle && o.hatchAngle->isChecked();
        m_dabHatch.crosshatchingChecked = o.crosshatching && o.crosshatching->isChecked();
        m_dabHatch.separationChecked = o.separation && o.separation->isChecked();
    }
    m_dabSmudgeRate = 1.0;
    m_dabColorRate = 0.0;
    m_dabSmudgeRadius = 0.0;
    m_dabSmudgeOpacity = 1.0;
    m_dabSpacingScale = 1.0; // resolveDab re-sets it per dab; 1.0 is the no-option identity
    m_haveSmudgeAnchor = false;
    m_cap = strokeAlphaCap(); // frozen with the rest of the params, like everything else at begin()
    m_confine = m_params.confine.get(); // the selection, frozen with the rest of the params
    // The dynamic-opacity gate, frozen with the cap: Wash only (the per-dab ceiling is an
    // indirect-painting mechanism -- Buildup's own composite is untranscribed and keeps the static
    // ceiling, badged at import), and only for an Opacity that genuinely MOVES. optionIsDynamic is
    // the importer's own predicate, so a preset cannot import as honoured and paint static. NEVER
    // under smudge, whose normalization just forced Wash: the smudge walk evaluates Opacity its
    // own way (WITH strength, per dab) and deposit() -- the only reader of this gate -- never runs.
    m_dynOpacity = !m_smudgeActive && m_params.paintMode == PaintMode::Wash && m_params.options &&
                   m_params.options->opacity && optionIsDynamic(m_params.options->opacity->data());
    // ... and BUILDUP's own half of it (§6.6i), gated the same way on the same predicate and
    // mutually exclusive with the line above by the mode test alone. Buildup is the reference's
    // DIRECT painting: there is no stroke temp to strive toward, so the option rides each dab's own
    // composite -- which in this accumulation is the per-dab share `opacity * cap` (deposit()).
    // NEVER under smudge for the same reason as above: colorsmudge normalizes to Wash and evaluates
    // Opacity its own way, and deposit() never runs on the smudge walk.
    m_buildOpacity = !m_smudgeActive && m_params.paintMode == PaintMode::Buildup &&
                     m_params.options && m_params.options->opacity &&
                     optionIsDynamic(m_params.options->opacity->data());
    m_dabOpacity = 1.0;
    m_avgOpacity = 0.0; // no dab yet: the first dab's own opacity becomes the average
    // Sharpness (§6.6e), frozen with the rest. The threshold runs whenever the option is CHECKED; the
    // coordinate snap needs alignOutline AND a positive static strength (the reference's own gate).
    // NEVER under smudge -- the reference's colorsmudge installs no sharpness option at all, so
    // ignoring it there is the faithful stroke (like Mirror; preset_brush.cpp never wires it).
    const bool sharpOn = !m_smudgeActive && m_params.options && m_params.options->sharpness &&
                         m_params.options->sharpness->option.isChecked();
    m_sharpnessActive = sharpOn;
    m_sharpnessSnap = sharpOn && m_params.options->sharpness->alignOutline &&
                      m_params.options->sharpness->option.data().strength > 0.0;
    m_sharpnessSoftness = sharpOn ? m_params.options->sharpness->softness : 0;
    m_dabSharpness = 1.0;
    // Colour dynamics (§6.6f), frozen with the rest. Active only on the Colored accumulator (a
    // per-dab colour needs the colour buffer -- Erase and Uniform have none, and colored() reads
    // both) and only when an h/s/v option is actually CHECKED. NEVER under smudge: the smudge
    // normalization above forced Uniform, so colored() is already false there, and the smudge walk
    // paints the stroke's own colour with no per-dab colour source. `colorDynamicsActive` (the
    // preset's flag) is the same predicate the importer's Colored verdict rests on -- requiring it
    // AND a checked h/s/v means a Colored-because-of-a-colour-TIP preset does not trip this path.
    const bool hasCheckedHsv =
        m_params.options &&
        ((m_params.options->hue && m_params.options->hue->isChecked()) ||
         (m_params.options->saturation && m_params.options->saturation->isChecked()) ||
         (m_params.options->value && m_params.options->value->isChecked()));
    m_colorDynamicsActive = m_params.colorDynamicsActive && colored() && hasCheckedHsv;
    m_dabDynColor = m_params.color; // resolveDab re-sets it per dab (safe default)
    // The TEXTURE gate (§6.6h), frozen with the rest. It needs a baked pattern and a REAL tip: the
    // grain composites into the tip's own 8-bit mask, and the analytic circle a tipless brush lays
    // is not a preset's dab. It DOES ride the smudge walk -- the reference installs the texture
    // option on the brush-based paintop base colorsmudge derives from, and the smudge dab's mask
    // goes through the same post-processing -- so, unlike Sharpness, m_smudgeActive is not a bar.
    m_textureActive = m_params.texture.enabled && m_params.texture.pattern != nullptr &&
                      !m_params.texture.pattern->empty() && m_params.tip != nullptr;
    m_dabTextureStrength = 1.0;
    m_textureOffX = 0;
    m_textureOffY = 0;
    // (The two effective offsets are drawn just below, once m_stroke carries this stroke's seed.)
    // The AIRBRUSH gate (§6.6h), frozen with the rest: a second, TIME-driven cadence on the dab
    // walk. Never beside a painter -- a painter's mark is a span, not a dab placed on a cadence,
    // and the reference's own painters never reach the timed walk either (they override the line
    // painter the cadence lives in). It DOES ride the smudge walk, which is the same dab walk.
    m_airbrushActive =
        m_params.airbrush.enabled && !m_painterActive && m_params.airbrush.rate > 0.0;
    m_dabRateScale = 1.0;
    m_timeCarry = 0.0;
    m_carry = 0.0;
    m_maskCarry = 0.0;
    m_lastMaskPressure = first.pressure;
    m_dabIndex = 0;
    m_total = Box{};
    m_pending = Box{};
    m_active = true;
    // An animated tip restarts on its first cell, because the dab counter restarts too. The mask
    // CACHE deliberately does not reset: its keys carry the tip's raster id, so what a previous
    // stroke rendered is exactly what this one would render.
    m_hose.beginStroke();
    // The derived state starts at the press: zero distance, zero elapsed, a still pen. The seed is a
    // param, never a clock (BrushParams::seed), so the stroke replays -- and when the caller asks
    // for it (`seedFromFirstSample`, §6.6i) the stroke's own first sample is folded in, which keeps
    // that replay contract to the letter while stopping every stroke of one preset from drawing the
    // identical random numbers. Off, this is the bare seed it always was, to the bit.
    m_stroke.begin(first, m_params.seedFromFirstSample ? strokeSeedFor(m_params.seed, first)
                                                       : m_params.seed);
    // The texture's effective offsets, ONCE PER STROKE and only now that m_stroke carries the seed.
    // The reference reads its per-stroke random source under the keys "texture_offset_x"/
    // "texture_offset_y", whose values are fixed for a stroke however many dabs read them;
    // strokeRandom() is that same object, keyed the same way -- so this draws from the stroke's SEED
    // rather than from the per-dab stream, and no pinned draw order moves. A non-random offset is
    // the preset's own integer, passed through.
    if (m_textureActive) {
        const TexturePattern& pat = *m_params.texture.pattern;
        const auto pw = static_cast<int>(pat.width);
        const auto ph = static_cast<int>(pat.height);
        m_textureOffX =
            m_params.texture.randomOffsetX
                ? std::clamp(static_cast<int>(m_stroke.strokeRandom("texture_offset_x") *
                                              static_cast<double>(pw)),
                             0, pw - 1)
                : m_params.texture.offsetX;
        m_textureOffY =
            m_params.texture.randomOffsetY
                ? std::clamp(static_cast<int>(m_stroke.strokeRandom("texture_offset_y") *
                                              static_cast<double>(ph)),
                             0, ph - 1)
                : m_params.texture.offsetY;
    }
    // Seed the spline's window with the first sample TWICE: a stroke has no sample before its first,
    // so the first span's incoming tangent is the chord itself (stroke_path.hpp's knot floor is what
    // turns that duplicate into a tangent instead of a division by zero). Both copies carry the state
    // as it stands at the press, which IS the state of a stroke that has not moved.
    const StrokeSnapshot head = m_stroke.snapshot();
    m_path.clear();
    m_path.push_back(head);
    m_path.push_back(head);

    if (m_painterActive) {
        // A painter's mark is made per SPAN, and there is no span yet -- so the press lays nothing
        // and only hands the painter the facts it is built against. (Upstream's sketch engine does
        // reach the press point through its `paintAt`, which appends it to the history and draws its
        // own zero-length self-connection; the painter is seeded with that point here instead, and
        // §6.6g records the at-most-one-pixel difference.)
        StrokePainterContext ctx;
        ctx.options = m_params.options.get();
        ctx.tip = m_params.tip.get();
        ctx.diameter = m_params.diameter;
        ctx.ratio = m_params.ratio;
        ctx.angleRad = m_params.angleRad;
        ctx.color = m_params.color;
        ctx.first = first;
        m_painter->begin(ctx);
    } else {
        // The first dab lands at the press point. It is a dab like any other: it advances the dab
        // counter and runs the option pipeline, against a stroke that has travelled nothing and is
        // standing still.
        const Dab d = resolveDab(first.pos, first.pressure);
        // The cadence to the first walked dab starts from THIS one's geometry. Its ellipse and not
        // its diameter: the press dab has no heading (the stroke is standing still), and the first
        // span will read the ellipse along the bearing it actually sets off on rather than along a
        // made-up one.
        m_lastSpacing = dabSpacingEllipse(d);
        stamp(d, first.pressure);
    }
    if (maskingActive())
        stampMask(first.pos, first.pressure); // ... and so does the masking stroke's
}

void BrushEngine::ensureCovers(int bx0, int by0, int bx1, int by1) {
    // `bx0..by1` is a half-open integer box, already clamped to the layer and non-empty. Fast path:
    // it already fits inside the current working rect.
    if (m_cw != 0 && bx0 >= m_ox && by0 >= m_oy && bx1 <= m_ox + static_cast<int>(m_cw) &&
        by1 <= m_oy + static_cast<int>(m_ch))
        return;

    // New rect = the box (unioned with the current rect), expanded outward to the tile grid and
    // clamped to the layer. Tile alignment keeps growth chunky so reallocation is amortized.
    int nx0 = bx0, ny0 = by0, nx1 = bx1, ny1 = by1;
    if (m_cw != 0) {
        nx0 = std::min(nx0, m_ox);
        ny0 = std::min(ny0, m_oy);
        nx1 = std::max(nx1, m_ox + static_cast<int>(m_cw));
        ny1 = std::max(ny1, m_oy + static_cast<int>(m_ch));
    }
    nx0 = std::max(0, (nx0 / kTile) * kTile);
    ny0 = std::max(0, (ny0 / kTile) * kTile);
    nx1 = std::min(static_cast<int>(m_w), ((nx1 + kTile - 1) / kTile) * kTile);
    ny1 = std::min(static_cast<int>(m_h), ((ny1 + kTile - 1) / kTile) * kTile);

    const auto ncw = static_cast<std::uint32_t>(nx1 - nx0);
    const auto nch = static_cast<std::uint32_t>(ny1 - ny0);
    const auto ncells = static_cast<std::size_t>(ncw) * nch;
    std::vector<float> ncov(ncells, 0.0f);
    std::vector<float> nbuild;
    if (buildup())
        nbuild.assign(ncells, 0.0f);
    std::vector<float> ncolored;
    if (colored())
        ncolored.assign(ncells * 4, 0.0f);
    std::vector<float> nmask;
    if (maskingActive())
        nmask.assign(ncells, 0.0f);
    std::vector<float> nsmudge;
    std::vector<std::uint8_t> nfilled;
    if (smudgeActive()) {
        nsmudge.assign(ncells * 4, 0.0f);
        nfilled.assign(ncells, 0);
    }
    common::Image nbase(ncw, nch); // base pixels are filled lazily on first touch in stamp()

    // Re-home the existing buffers into the larger ones at their shifted offset. EVERY per-pixel
    // buffer has to move: a coverage that survives while the base snapshot does not turns a later
    // restore() into garbage, silently (see tests/test_brush_wash_golden.cpp).
    if (m_cw != 0) {
        const auto dx = static_cast<std::uint32_t>(m_ox - nx0);
        const auto dy = static_cast<std::uint32_t>(m_oy - ny0);
        for (std::uint32_t row = 0; row < m_ch; ++row) {
            const std::size_t src = static_cast<std::size_t>(row) * m_cw;
            const std::size_t dst = static_cast<std::size_t>(row + dy) * ncw + dx;
            std::copy_n(&m_coverage[src], m_cw, &ncov[dst]);
            if (buildup())
                std::copy_n(&m_build[src], m_cw, &nbuild[dst]);
            if (colored())
                std::copy_n(&m_colored[src * 4], static_cast<std::size_t>(m_cw) * 4,
                            &ncolored[dst * 4]);
            if (maskingActive())
                std::copy_n(&m_mask[src], m_cw, &nmask[dst]);
            if (smudgeActive()) {
                std::copy_n(&m_smudgeState[src * 4], static_cast<std::size_t>(m_cw) * 4,
                            &nsmudge[dst * 4]);
                std::copy_n(&m_baseFilled[src], m_cw, &nfilled[dst]);
            }
            std::copy_n(&m_base.rgba[src * 4], static_cast<std::size_t>(m_cw) * 4,
                        &nbase.rgba[dst * 4]);
        }
    }
    m_ox = nx0;
    m_oy = ny0;
    m_cw = ncw;
    m_ch = nch;
    m_coverage = std::move(ncov);
    m_build = std::move(nbuild);
    m_colored = std::move(ncolored);
    m_mask = std::move(nmask);
    m_smudgeState = std::move(nsmudge);
    m_baseFilled = std::move(nfilled);
    m_base = std::move(nbase);
}

void BrushEngine::extendTo(StrokeInput sample) {
    if (!m_active)
        return;
    // Fold the sample into the derived state as it ARRIVES: it runs even when nothing is stamped --
    // a slowing pen still moves the speed EMA and the clock. (⚠ The dab walk now lags this by one
    // sample; see the m_stroke declaration for what Arc D owes because of it.)
    m_stroke.extendTo(sample);

    // A sample that did not move is not a point on the path -- it is the same point again, with new
    // pressure. Feeding it to the spline would put a zero-length knot span in the middle of a curve.
    // Absorb it instead, exactly as the old walk did when it found a zero-length segment: the newer
    // sample (and the state it just folded into) REPLACES the older one, so its pressure and its
    // elapsed time are the ones the next span interpolates from.
    //
    // ⚠ EXCEPT UNDER THE AIRBRUSH (§6.6h), where a sample that did not move is exactly the sample
    // that matters: a held pointer is what the timed cadence exists to paint through, and absorbing
    // it would fold the elapsed time into a span the walk lays somewhere else entirely. So the
    // duplicate is PUSHED, giving a zero-travel span that walkSpan pumps at that one point -- the
    // same shape the reference reaches, whose tool feeds its synthesized still-pointer samples
    // through the ordinary paint path and whose walk then runs its timed test on a degenerate
    // segment. (The spline copes: the knot floor in stroke_path.hpp is what makes a zero-length
    // knot span a tangent instead of a division by zero, and begin() already seeds the path with
    // one duplicate of its own.)
    if (!m_airbrushActive && !m_path.empty() &&
        (sample.pos - m_path.back().sample.pos).length() < 1e-9) {
        m_path.back() = m_stroke.snapshot();
        return;
    }
    m_path.push_back(m_stroke.snapshot());

    // Four points = one span with both its tangents known: stamp the span between the middle two.
    // This is the one-sample lag, and it is the price of a curve that passes THROUGH the samples
    // rather than merely near them.
    if (m_path.size() >= 4) {
        stampSpan(m_path[0], m_path[1], m_path[2], m_path[3]);
        m_path.erase(m_path.begin());
    }
}

void BrushEngine::flush() {
    if (!m_active)
        return;
    // The tail span has no successor to fit against, because the stroke is over. The last sample
    // stands in for its own -- the same duplication begin() uses at the head. After this the window
    // holds no unstamped span, which is what makes a second flush() a no-op.
    if (m_path.size() >= 3) {
        stampSpan(m_path[0], m_path[1], m_path[2], m_path[2]);
        m_path.erase(m_path.begin());
    }
}

void BrushEngine::end() {
    flush(); // a stroke whose last span was never laid is a stroke with a missing end
    if (m_painterActive && m_painter) {
        // The whole-stroke seam (§6.6b's `experimentbrush` fills its accumulated path on release).
        // After flush(), so a painter that draws here sees the last span too.
        PainterCanvas canvas(*this);
        m_painter->finish(canvas);
        // ⚠ AND THE SEAM MUST BE COMPOSITED, OR IT IS INVISIBLE. Every other composite() in the
        // system happens DURING the stroke (the live cadence); end() is the last thing a caller
        // runs, so paint laid here would never reach the target -- `experimentbrush`, which lays
        // NOTHING until release, would commit a blank stroke. Guarded on a painter having run, so a
        // dab stroke's end() is byte-identical to before and no golden can move.
        composite();
    }
    m_active = false;
}

void BrushEngine::stampSpan(const StrokeSnapshot& s0, const StrokeSnapshot& s1,
                            const StrokeSnapshot& s2, const StrokeSnapshot& s3) {
    // The path from s1 to s2, as a polyline. The flattener emits NOTHING when the curve is within
    // tolerance of the straight chord -- so a straight stroke walks a single edge, through the very
    // same arithmetic the old chord walk used, and lays bit-identical dabs.
    flattenCatmullRom(s0.sample.pos, s1.sample.pos, s2.sample.pos, s3.sample.pos,
                      kFlattenTolerancePx, m_flat);
    m_poly.clear();
    m_poly.reserve(m_flat.size() + 2);
    m_poly.push_back(s1.sample.pos);
    m_poly.insert(m_poly.end(), m_flat.begin(), m_flat.end());
    m_poly.push_back(s2.sample.pos);

    // The walk rewinds the stroke state to each dab's own point (walkSpan). Between spans the state
    // must read LIVE -- it is what strokeState() hands out, and the Settings->Tablet test area reads
    // the pointer's speed from it, not the last dab's. So stash it and put it back.
    const StrokeSnapshot live = m_stroke.snapshot();

    if (m_painterActive)
        paintSpanEdges(s1, s2); // the second engine kind: no dab walk at all (§6.6g)
    else
        walkSpan(false, s1, s2);
    if (maskingActive()) {
        // The masking stroke walks the same path on its OWN cadence (its own spacing mode, its own
        // pressure-scaled size, its own carry) -- a second stroke that happens to share the path, not
        // extra work per primary dab (docs/brushes.md §6.2).
        walkSpan(true, s1, s2);
    }

    m_stroke.rewindTo(live);
}

void BrushEngine::pumpStationarySpan(const StrokeSnapshot& a, const StrokeSnapshot& b) {
    // The airbrush's stationary half (§6.6h): a span whose two ends are the SAME POINT still has a
    // clock running across it, so the timed cadence still places dabs -- all of them at that point.
    // Everything else is walkSpan's own machinery, minus the distance cadence it has nothing to
    // measure: the state is interpolated by the same lerpSnapshot, the dab is resolved by the same
    // resolveDab (one evaluation, in order, drawing from the same streams), and the timed remainder
    // is the same m_timeCarry the moving walk carries.
    // The same TIME BUDGET the moving walk runs on (brush_engine.hpp's kMaxSpanBudgetMs), and the
    // same invariant: a dab costs one interval, the budget pays for it, and when it cannot the span
    // is over. Both inputs are clamped first -- the elapsed time because it is a delta of the
    // caller's clock, the carried remainder because earlier spans may not have spent it.
    const double dtMs = std::min(std::max(0.0, b.elapsedMs - a.elapsedMs), kMaxSpanBudgetMs);
    if (!(dtMs > 0.0)) {
        return; // no travel and no clock: there is nothing for either cadence to answer
    }
    const double carryIn = std::min(m_timeCarry, kMaxSpanBudgetMs);
    const double budgetMs = std::min(carryIn + dtMs, kMaxSpanBudgetMs);
    double spent = 0.0; // ms of the budget already paid out to dabs
    double interval = airbrushIntervalMs(m_params.airbrush.rate, m_dabRateScale);
    int laid = 0;
    while (budgetMs - spent >= interval && laid++ < kMaxTimedDabsPerSpan) {
        spent += interval;
        // Both remainders reset at a dab, whichever cadence placed it (the reference's own
        // `resetAccumulators`). There was no travel to accumulate, so zeroing the distance carry
        // here is that reset spelled out rather than a second rule.
        m_carry = 0.0;
        // Where in THIS span the dab falls: the budget it consumed, less whatever of it was already
        // elapsed before the span began.
        const double t = std::clamp((spent - carryIn) / dtMs, 0.0, 1.0);
        StrokeSnapshot dabState = lerpSnapshot(a, b, t);
        m_stroke.rewindTo(dabState);
        const double pr = dabState.sample.pressure;
        const Dab d = resolveDab(dabState.sample.pos, pr);
        stamp(d, pr);
        m_lastSpacing = dabSpacingEllipse(d);
        interval = airbrushIntervalMs(m_params.airbrush.rate, m_dabRateScale);
    }
    m_timeCarry = budgetMs - spent;
}

void BrushEngine::walkSpan(bool mask, const StrokeSnapshot& a, const StrokeSnapshot& b) {
    if (m_poly.size() < 2)
        return;
    m_edge.clear();
    m_edge.reserve(m_poly.size() - 1);
    double total = 0.0;
    for (std::size_t i = 0; i + 1 < m_poly.size(); ++i) {
        const double e = (m_poly[i + 1] - m_poly[i]).length();
        m_edge.push_back(e);
        total += e;
    }
    if (total < 1e-9) {
        // No travel. The DISTANCE cadence has nothing to measure and the walk has always stopped
        // here -- but the TIMED one still does, and dabs continuing to lay where the pointer rests
        // is the whole of what an airbrush IS. The reference reaches the same case the same way:
        // its walk still runs its timed test on a segment whose two ends coincide.
        if (m_airbrushActive && !mask)
            pumpStationarySpan(a, b);
        return;
    }

    double& carry = mask ? m_maskCarry : m_carry;

    // The bearing this span sets off on: the first edge with any length in it. (A zero-length lead
    // edge is possible -- the flattener can emit a point on top of s1 -- and atan2(0,0) is 0, which
    // would silently claim "due east".)
    double headIn = 0.0;
    for (std::size_t i = 0; i < m_edge.size(); ++i) {
        if (m_edge[i] > 0.0) {
            const common::Vec2 dir = m_poly[i + 1] - m_poly[i];
            headIn = std::atan2(dir.y, dir.x);
            break;
        }
    }

    // Spacing keys off the RESOLVED (option- and pressure-scaled) geometry, re-resolved after every
    // dab from that dab's own ellipse -- a fading stroke tightens its cadence with its tip. A
    // zero-flow dab still resets the cadence: it is a dab the walk laid, it just deposited nothing.
    //
    // The step is the last dab's ellipse read along the direction we are ABOUT TO TRAVEL, which is
    // this span's first edge -- not the direction that dab was laid travelling. On a shaped tip the
    // two differ, and the one that matters is where the brush is going: a knife that turns a corner
    // must tighten its cadence as it comes round, not a step later. (For a round ellipse the heading
    // is ignored entirely, so a round tip's arithmetic is untouched.)
    double spacingPx = mask ? spacingStepAlong(maskingSpacingEllipse(m_lastMaskPressure), headIn)
                            : spacingStepAlong(m_lastSpacing, headIn);
    double pos = spacingPx - carry; // arc distance from s1 to the first dab on this span

    // THE SECOND CADENCE (§6.6h). The airbrush asks a question beside "has the brush travelled a
    // spacing?": "has a timed interval elapsed?" -- and the walk lays a dab at whichever comes
    // first, which is the reference's `min(distanceFactor, timeFactor)` in arc-length form.
    //
    // The clock is the SPAN'S OWN, interpolated exactly as every other channel is: `elapsedMs` runs
    // linearly from `a` to `b` in the ARC fraction (the same fraction lerpSnapshot reads), so a
    // millisecond is 1/`msPerArc` document pixels of travel. No wall clock is read here or anywhere
    // below -- the mark stays a pure function of the sample stream, and that is the whole of the
    // replay argument.
    //
    // ⚠ The masking walk is not airbrushed: it carries no options and no airbrush block, and the
    // reference's masking stroke runs its own distance information with timing disabled.
    const bool timed = m_airbrushActive && !mask;
    // ⚠⚠ THE SPAN'S TIME BUDGET (brush_engine.hpp's kMaxSpanBudgetMs) -- the fix for a USER-REPORTED
    // HANG, and the invariant the whole cadence now rests on:
    //
    //     timed dabs on one span  <=  floor(budget / interval) + 1
    //
    // It is enforced by CONSUMING the budget as dabs are laid, below, rather than by counting them:
    // a dab is authorised by elapsed time, so the stop has to be derived from the same quantity that
    // authorised it. A counter bolted on beside the arc walk would be a second opinion about the
    // same question, and the two could disagree.
    //
    // Both inputs are clamped BEFORE the budget is formed, because both are unbounded upstream: the
    // elapsed time is a delta of the caller's clock (a stall makes it seconds) and the carried
    // remainder is whatever earlier spans could not spend.
    const double dtMs =
        timed ? std::min(std::max(0.0, b.elapsedMs - a.elapsedMs), kMaxSpanBudgetMs) : 0.0;
    const double msPerArc = (timed && total > 0.0) ? dtMs / total : 0.0;
    // ⚠ `ignoreSpacing` switches the DISTANCE cadence off entirely, exactly as the reference's
    // `distanceSpacingEnabled = !ignoreSpacing` does -- and only while the airbrush is on.
    const bool distanceOn = !(timed && m_params.airbrush.ignoreSpacing);
    double timeAcc = timed ? std::min(m_timeCarry, kMaxSpanBudgetMs)
                           : m_timeCarry; // ms since the last dab, as of `arcBase` on this span
    double arcBase = 0.0;
    // The milliseconds this span may still spend. NON-INCREASING: every dab charges it for the time
    // that dab consumed, and a TIMED dab consumes exactly one interval by construction (it is placed
    // at the arc where `timeAcc + msPerArc*(pos - arcBase)` reaches it). So each timed dab strictly
    // removes at least kMinTimedIntervalMs, and the loop's own accounting is what ends it -- however
    // the arc conversion behaves, and whatever `msPerArc` does at near-zero travel.
    double budgetMs = timed ? std::min(timeAcc + dtMs, kMaxSpanBudgetMs) : 0.0;
    double lastDabArc = pos - spacingPx; // the arc the previous dab sat at, in this span's frame
    if (timed) {
        const double interval = airbrushIntervalMs(m_params.airbrush.rate, m_dabRateScale);
        if (!distanceOn)
            pos = std::numeric_limits<double>::infinity();
        const double posT = (budgetMs >= interval && msPerArc > 0.0)
                                ? arcBase + std::max(0.0, (interval - timeAcc) / msPerArc)
                                : std::numeric_limits<double>::infinity();
        if (posT < pos)
            pos = posT;
    }
    std::size_t ei = 0;             // the edge `pos` currently falls in ...
    double base = 0.0;              // ... and the arc length at that edge's start
    int laid = 0;                   // only read under `timed`: the non-termination backstop
    while (pos <= total) {
        while (ei + 1 < m_edge.size() && pos > base + m_edge[ei]) {
            base += m_edge[ei];
            ++ei;
        }
        const double el = m_edge[ei];
        const common::Vec2 e0 = m_poly[ei];
        const common::Vec2 c = e0 + (m_poly[ei + 1] - e0) * (el > 0.0 ? (pos - base) / el : 0.0);
        const double t = pos / total; // state by ARC fraction; on one edge this IS the old chord t

        if (mask) {
            // The masking walk drives no options, so it needs the dab's pressure and nothing else --
            // which is lerpSnapshot's own expression for it, spelled out. (Its clamp is a no-op here:
            // the loop condition puts `t` in (0,1].)
            const double pr = a.sample.pressure + (b.sample.pressure - a.sample.pressure) * t;
            stampMask(c, pr);
            m_lastMaskPressure = pr;
            // The step onward is the masking tip's ellipse read along the curve's local tangent at
            // this dab -- the same rule as the primary walk below. A null masking tip's ellipse is
            // round and a round ellipse ignores the heading entirely (the scalar branch), so the
            // analytic masking walk steps bit-for-bit as it always did.
            double heading = headIn;
            if (el > 0.0) {
                const common::Vec2 dir = m_poly[ei + 1] - e0;
                heading = std::atan2(dir.y, dir.x);
            }
            spacingPx = spacingStepAlong(maskingSpacingEllipse(pr), heading);
            pos += spacingPx;
            continue;
        }

        // The dab's own state: everything the sensors read, interpolated between the span's two
        // samples -- NOT the live state, which the walk's one-sample lag has already carried past b.
        StrokeSnapshot dabState = lerpSnapshot(a, b, t);
        // The dab is on the CURVE, not on the chord between the two samples that lerpSnapshot blends.
        // No sensor reads a position today, so this is consistency rather than behaviour -- but a
        // state that lies about where its own dab is would be a trap laid for the very next option to
        // land (Scatter jitters a position; a pattern colour source samples one).
        dabState.sample.pos = c;
        // ... and its heading is the curve's local tangent there. On a single-edge (straight) span
        // that is the chord's own direction, which is bit-for-bit the angle the state already carried
        // -- so a straight stroke's `drawingangle` is exactly what it was before the curve existed.
        if (el > 0.0) {
            const common::Vec2 dir = m_poly[ei + 1] - e0;
            dabState.drawingAngle = std::atan2(dir.y, dir.x);
        }
        m_stroke.rewindTo(dabState);

        const double pr = dabState.sample.pressure;
        const Dab d = resolveDab(c, pr); // ONE evaluation per dab: it draws from the random stream
        stamp(d, pr);
        m_lastSpacing = dabSpacingEllipse(d);
        lastDabArc = pos;
        // ... and the step onward is that ellipse read along the heading AT THIS DAB, which the
        // walk has already computed as the curve's local tangent.
        spacingPx = spacingStepAlong(m_lastSpacing, dabState.drawingAngle);
        pos += spacingPx;
        if (timed) {
            // CHARGE THE BUDGET for the time this dab consumed -- the same quantity that authorised
            // it. A timed dab sits exactly where `timeAcc + msPerArc*(arc - arcBase)` reaches the
            // interval, so it costs exactly one interval; a dab the DISTANCE cadence placed first
            // costs less, and correctly so, because the reference discards that time too. The
            // `max(0, ...)` is what makes the budget non-increasing -- `lastDabArc` can sit behind
            // `arcBase` when an overdue distance carry put the first dab before the span's start,
            // and a negative charge would hand the loop back the time it just spent.
            budgetMs -= std::max(0.0, timeAcc + msPerArc * (lastDabArc - arcBase));
            // Both accumulators reset at a dab, whichever cadence placed it -- the reference's own
            // `resetAccumulators()`. The timed interval is re-read from THIS dab's Rate value, which
            // resolveDab has just drawn: the reference re-computes its timing after laying a dab,
            // for exactly the same reason the spacing option is read there.
            timeAcc = 0.0;
            arcBase = lastDabArc;
            const double interval = airbrushIntervalMs(m_params.airbrush.rate, m_dabRateScale);
            if (!distanceOn)
                pos = std::numeric_limits<double>::infinity();
            // ⚠ The budget gate is what ends the timed cadence, not the counter below: with nothing
            // left to pay with, the timed candidate stops arming and only the distance cadence can
            // place another dab.
            const double posT = (budgetMs >= interval && msPerArc > 0.0)
                                    ? arcBase + interval / msPerArc
                                    : std::numeric_limits<double>::infinity();
            if (posT < pos)
                pos = posT;
            if (++laid >= kMaxTimedDabsPerSpan)
                break; // the backstop; the budget above must have ended this loop long before
        }
    }
    // The remainder carried toward the next dab.
    //
    // ⚠ THE INERT EXPRESSION IS PRESERVED TO THE CHARACTER -- `(pos - spacingPx)` and not the arc
    // the last dab actually sat at -- because `(a + b) - b` is not `a` in IEEE doubles and EVERY
    // spacing golden in the suite was laid by this line. With the airbrush live the timed cadence
    // can place a dab at an arc `pos - spacingPx` does not name at all, so that case reads the arc
    // the walk really stamped (which is the same value when no dab was laid: `lastDabArc` is
    // seeded from the very same expression).
    carry = timed ? total - lastDabArc : total - (pos - spacingPx);
    // The timed remainder, carried to the next span -- and CLAMPED to the same budget, so a long
    // idle cannot bank a backlog that the next span dumps in one go. (Unclamped it grew without
    // limit: ten seconds of a pointer held under a slow rate banked ten seconds of credit.)
    if (timed)
        m_timeCarry = std::min(timeAcc + msPerArc * (total - arcBase), kMaxSpanBudgetMs);
}

int BrushEngine::PainterCanvas::width() const noexcept {
    return static_cast<int>(m_engine.m_w);
}

int BrushEngine::PainterCanvas::height() const noexcept {
    return static_cast<int>(m_engine.m_h);
}

void BrushEngine::PainterCanvas::plot(int x, int y, double alpha, common::Color8 color) {
    BrushEngine& e = m_engine;
    // Clipped to the document exactly as a dab's blit is, and an alpha of zero lays nothing rather
    // than costing a working-rect growth and a first-touch snapshot for a pixel it will not move.
    if (x < 0 || y < 0 || x >= static_cast<int>(e.m_w) || y >= static_cast<int>(e.m_h))
        return;
    if (!(alpha > 0.0))
        return;
    e.ensureCovers(x, y, x + 1, y + 1);

    // The same DabDeposit a dab builds, with the painter's own alpha standing in for the mask value.
    // `flow` is 1: neither engine of this kind has a flow concept -- the reference's paintops
    // composite their marks at the mark's own opacity -- so the deposit's alpha IS the plotted one,
    // and the dynamic-opacity pair (resolved once per span, exactly as it is once per dab) still
    // takes the transcribed wash-ceiling step under Wash.
    DabDeposit dep;
    dep.flow = 1.0;
    dep.dynOpacity = e.m_dynOpacity;
    dep.buildOpacity = e.m_buildOpacity;
    dep.opacity = e.m_dabOpacity;
    dep.avgOpacity = e.m_avgOpacity;
    dep.color = e.colored();
    if (dep.color) {
        dep.r = color.r / 255.0;
        dep.g = color.g / 255.0;
        dep.b = color.b / 255.0;
    }
    e.deposit(x, y, alpha > 1.0 ? 1.0 : alpha, dep);
}

void BrushEngine::beginPainterSegment() {
    // A painter's span is its dab: the counter `fade` reads advances once per span, and so does the
    // dab index the stroke's geometry is measured in. The dynamic-opacity pair advances here for the
    // same reason resolveDab advances it -- the reference evaluates its opacity option once per
    // `paintLine`, and both values are properties of the stroke, running across a rewind.
    m_stroke.beginDab();
    ++m_dabIndex;
    if (m_dynOpacity) {
        m_dabOpacity = m_params.options->opacity->sizeLikeValue(m_stroke, /*useStrength=*/false);
        m_avgOpacity = blendAverageOpacity(m_dabOpacity, m_avgOpacity);
    } else if (m_buildOpacity) {
        // The painters' half of §6.6i, and the reason the three BUILDUP painter presets
        // (v)_Sketching-1/-2/-3) were badged: a painter's mark is a span, its opacity is resolved
        // once per span exactly as a dab's is once per dab, and it lands in the very same
        // `deposit()`.
        m_dabOpacity = m_params.options->opacity->sizeLikeValue(m_stroke, /*useStrength=*/false);
    }
}

void BrushEngine::paintSpanEdges(const StrokeSnapshot& a, const StrokeSnapshot& b) {
    if (m_poly.size() < 2 || !m_painter)
        return;
    m_edge.clear();
    m_edge.reserve(m_poly.size() - 1);
    double total = 0.0;
    for (std::size_t i = 0; i + 1 < m_poly.size(); ++i) {
        const double e = (m_poly[i + 1] - m_poly[i]).length();
        m_edge.push_back(e);
        total += e;
    }
    if (total < 1e-9)
        return; // no travel: the span is a point, and a painter's mark is a segment

    PainterCanvas canvas(*this);
    double base = 0.0;
    for (std::size_t i = 0; i + 1 < m_poly.size(); ++i) {
        const double t0 = base / total;
        base += m_edge[i];
        const double t1 = base / total;

        // The state at each end of THIS edge, by arc fraction along the whole span -- the same
        // interpolation the dab walk uses, and on a single-edge (straight) span it is exactly the
        // span's own two snapshots.
        StrokeSnapshot sa = lerpSnapshot(a, b, t0);
        StrokeSnapshot sb = lerpSnapshot(a, b, t1);
        sa.sample.pos = m_poly[i];
        sb.sample.pos = m_poly[i + 1];
        if (m_edge[i] > 0.0) {
            const common::Vec2 dir = m_poly[i + 1] - m_poly[i];
            const double heading = std::atan2(dir.y, dir.x);
            sa.drawingAngle = heading;
            sb.drawingAngle = heading;
        }

        // ⚠ THE STROKE IS PUT AT THE EDGE'S END, not at its start: the reference evaluates every
        // one of its painters' options against the segment's SECOND paint information. A painter
        // that read the state itself would read the same point.
        m_stroke.rewindTo(sb);
        beginPainterSegment();
        m_painter->paintSpan(canvas, sa, sb, m_stroke);
    }
}

BrushEngine::DabDeposit BrushEngine::beginDeposit(const Dab& dab, std::size_t dabNo,
                                                 double pressure) const {
    DabDeposit d;
    d.flow = dab.flow;
    d.dynOpacity = m_dynOpacity;
    d.buildOpacity = m_buildOpacity;
    d.opacity = m_dabOpacity;
    d.avgOpacity = m_avgOpacity;
    // Resolve this dab's colour once, not per pixel. Only the Colored accumulator can honour a
    // per-dab colour; the returned alpha is ignored (see BrushDynamics::dabColor).
    d.color = colored();
    if (d.color) {
        // The per-dab colour source, in priority: the transcribed HSV colour dynamics (§6.6f,
        // resolved in resolveDab into m_dabDynColor) win on a colour-dynamics preset; else the host's
        // dabColor seam (third-party colour packs / RGBA hose); else the stroke's flat paint colour.
        // The returned alpha is ignored either way -- the ceiling froze at begin() (see dabColor).
        common::Color8 c;
        if (m_colorDynamicsActive)
            c = m_dabDynColor;
        else if (m_dyn.dabColor)
            c = m_dyn.dabColor(dabNo, pressure);
        else
            c = m_params.color;
        d.r = c.r / 255.0;
        d.g = c.g / 255.0;
        d.b = c.b / 255.0;
    }
    return d;
}

double BrushEngine::buildCap(const DabDeposit& dep) const noexcept {
    // BUILDUP's per-dab share of the stroke's ceiling (§6.6i). Without a dynamic Opacity option this
    // is `m_cap` verbatim -- the expression the accumulation has always used, to the bit, which is
    // what keeps every Buildup golden in the suite where it is. With one, the reference's direct
    // painting composites each dab at its OWN opacity, and `m_dabOpacity * m_cap` is exactly that
    // value in Mosaic's split (see resolveDab: sensors here, strength/ceiling in m_cap).
    //
    // ⚠ ONE function, read by both accumulators below. The Buildup alpha and the Colored buffer's
    // weight MUST use the same per-dab share or a two-colour Buildup stroke normalizes to the wrong
    // mix -- §6.1's own rule, and it was already true of `m_cap`; two copies of it would drift.
    return dep.buildOpacity ? clamp01(dep.opacity) * m_cap : m_cap;
}

void BrushEngine::deposit(int x, int y, double cov, const DabDeposit& dep) {
    // SELECTION CONFINEMENT: a pixel the selection does not reach takes no paint -- and records no
    // COVERAGE either, so the mask the Inpaint brush reads agrees with where the paint landed. A
    // PARTIALLY selected pixel deposits normally here and is scaled once, at composite(): the
    // coverage must still build toward 1 as it always did, or an overlapping stroke would climb
    // past the selection's own coverage instead of being capped by it.
    if (m_confine != nullptr && m_confine->at(x, y) <= 0.0)
        return;
    const double a = dep.flow * cov;
    const std::size_t ci =
        static_cast<std::size_t>(y - m_oy) * m_cw + static_cast<std::size_t>(x - m_ox);
    float& dst = m_coverage[ci];
    if (dst == 0.0f && m_target != nullptr) {
        // First touch: snapshot the still-pristine target pixel as this pixel's composite base
        // (composite() will overwrite the target here). Only stamped pixels are snapshotted, so the
        // base buffer never needs to span the whole layer.
        const std::size_t tp = (static_cast<std::size_t>(y) * m_w + x) * 4;
        const std::size_t bp = ci * 4;
        m_base.rgba[bp] = m_target->rgba[tp];
        m_base.rgba[bp + 1] = m_target->rgba[tp + 1];
        m_base.rgba[bp + 2] = m_target->rgba[tp + 2];
        m_base.rgba[bp + 3] = m_target->rgba[tp + 3];
    }
    // What this dab deposited at this pixel, as the Colored accumulation weighs it. On the static
    // wash path it is `a`, exactly as it always was; on the dynamic-opacity path it is the step the
    // accumulation actually took (which can be zero -- a dab whose ceiling is already met deposits
    // nothing, and its colour must weigh nothing).
    double washStep = a;
    if (!dep.dynOpacity) {
        dst = static_cast<float>(dst + a * (1.0 - dst)); // "over" build-up toward 1
    } else {
        // The per-dab CEILING (docs/brushes.md §6.2): the accumulation strives toward this dab's
        // own opacity -- or the stroke's running average where that is higher -- rather than
        // toward 1. A separate branch, not a parameterization of the line above: at opacity 1 the
        // two are identical in real arithmetic but differ in float grouping, and the static path's
        // bytes are pinned by every wash golden in the suite.
        const double before = dst;
        const double after = washAlphaDarkenAlpha(before, cov, dep.flow, dep.opacity,
                                                  dep.avgOpacity);
        dst = static_cast<float>(after);
        washStep = std::max(0.0, after - before);
    }
    if (buildup()) {
        // Each dab carries its own share of the stroke's ceiling, and the shares accumulate "over"
        // each other -- so overlapping dabs climb past `opacity` toward 1, which is exactly what
        // Wash's single end-of-stroke cap forbids. Compositing the dab straight into the target
        // would give the same result for Normal; accumulating instead keeps the region-refresh and
        // restore() machinery, which both read from a pristine base.
        //
        // Note this stores the CAPPED value where the coverage above stores the uncapped one. It has
        // to: `1 - prod(1 - a_i*cap)` is not a function of the coverage. So a lone dab -- `a * cap`
        // either way in real arithmetic -- can land one 8-bit level apart between the two modes (6026
        // of 4.0e9 sampled (a, cap) pairs disagree, all at very low alpha). Rounding coverage the same
        // way would move Wash's pinned bytes.
        float& acc = m_build[ci];
        const double da = a * buildCap(dep);
        acc = static_cast<float>(acc + da * (1.0 - acc));
    }
    if (dep.color) {
        // Premultiplied source-over: this dab's colour stacks OVER what earlier dabs left, weighted
        // by the same per-dab alpha the mode's own accumulator uses -- the wash step beside the
        // Wash coverage (`a` on the static path; the ceiling-limited step on the dynamic one),
        // `a*cap` beside the Buildup accumulation -- so the buffer's own alpha channel tracks that
        // accumulator and normalizing by it recovers exactly the colour the mode's compositing
        // implies. (The alpha channel's `da + acc*(1-da)` is the same float expression as the RGB
        // rows with C = 1, deliberately: a pure-white dab stream normalizes to exactly 1.0, not
        // 1-epsilon.)
        const double da = buildup() ? a * buildCap(dep) : washStep;
        float* acc = &m_colored[ci * 4];
        acc[0] = static_cast<float>(da * dep.r + acc[0] * (1.0 - da));
        acc[1] = static_cast<float>(da * dep.g + acc[1] * (1.0 - da));
        acc[2] = static_cast<float>(da * dep.b + acc[2] * (1.0 - da));
        acc[3] = static_cast<float>(da + acc[3] * (1.0 - da));
    }
    m_total.add(x, y);   // whole-stroke extent (dirtyBounds)
    m_pending.add(x, y); // not yet composited
}

void BrushEngine::stamp(const Dab& dab, double pressure) {
    // The dab's index is a property of the stroke's GEOMETRY -- it advances for clipped and
    // zero-flow dabs too, so a stroke running off the document edge keeps the same per-dab colour
    // sequence it would have had in the middle of the canvas.
    const std::size_t dabNo = m_dabIndex++;

    // A small floor on the coverage radius so a 1 px tip still deposits onto its centre pixel. `R` is
    // the tip's semi-axis along its OWN x; its y semi-axis is `R * ratio`.
    const double R = std::max(0.5 * dab.diameter, 0.6);
    // The smudge walk routes BEFORE the flow gate: the reference's colorsmudge never consults
    // flow (a pressure-0 dab still smears at its own rate), and a smudge dab must still plant its
    // anchor. The ratio guard stays -- the reference returns on a degenerate size before its mask
    // is rendered, anchor untouched, and so does this.
    if (smudgeActive()) {
        if (!(dab.ratio > 0.0))
            return;
        stampSmudgeDab(dab, R, pressure);
        return;
    }
    const double flow = dab.flow;
    if (flow <= 0.0)
        return;
    // A zero (or negative) ratio is a tip with no height. It is legal -- the mask generators define
    // it -- and it paints nothing rather than dividing by it.
    if (!(dab.ratio > 0.0))
        return;

    // A REAL tip -- one of the six procedural generators, or a decoded bitmap -- rasterizes through
    // the dab cache and blits. Everything after the coverage is identical to the analytic path's:
    // the two differ in where a pixel's coverage COMES FROM and in nothing else, which is why they
    // share `deposit()` rather than each keeping their own copy of the accumulation.
    if (m_params.tip) {
        stampTipDab(dab, R, dabNo, pressure);
        return;
    }

    const common::Vec2 center = dab.center;

    // The map from the document into the TIP's own frame: undo the dab's rotation, then its aspect.
    //
    // ⚠ AT ratio == 1 AND angleRad == 0 THIS IS THE IDENTITY IN IEEE ARITHMETIC, NOT MERELY TO WITHIN
    // A ROUNDING ERROR: cos(0) is exactly 1.0, sin(0) is exactly 0.0, x * 1.0 == x, and x + 0.0 == x
    // (0.0 is the additive identity for every finite double, signed zeros included, since the sum is
    // only ever squared). So a circular unrotated dab reduces to `sqrt(dx*dx + dy*dy)` -- the very
    // expression this loop evaluated before the tip had a shape -- and lays BIT-IDENTICAL bytes.
    // That is what lets `Uniform x Wash`, the mouse goldens and every straight-path golden survive a
    // change that gives the tip a shape. It is a test, not a hope.
    //
    // There is deliberately no `if (circular)` fast path guarding it. A branch would be provably
    // equivalent to the arithmetic below, which makes it dead weight to reason about and a mutant no
    // test could ever kill; if a profile ever asks for one, it can be added with a test that pins the
    // two paths byte-for-byte.
    const double cosT = std::cos(dab.angleRad);
    const double sinT = std::sin(dab.angleRad);
    const double invRatio = 1.0 / dab.ratio;

    // The dab's axis-aligned footprint: the rotated bounding BOX of the tip's two semi-axes. It is
    // conservative for an ellipse (it is the box of the rotated rectangle that contains it), which
    // costs a few edge pixels that evaluate to zero coverage and buys agreement with dab_mask.hpp's
    // `dabExtent` -- so the analytic tip and the rasterized one will not disagree about where a dab
    // lands. At ratio 1 / angle 0 it is exactly `R` on both axes, as it always was.
    const double halfH = R * dab.ratio;
    const double ax = std::fabs(R * cosT) + std::fabs(halfH * sinT);
    const double ay = std::fabs(R * sinT) + std::fabs(halfH * cosT);

    const int x0 = std::max(0, static_cast<int>(std::floor(center.x - ax - 1.0)));
    const int y0 = std::max(0, static_cast<int>(std::floor(center.y - ay - 1.0)));
    const int x1 = std::min(static_cast<int>(m_w), static_cast<int>(std::ceil(center.x + ax + 1.0)));
    const int y1 = std::min(static_cast<int>(m_h), static_cast<int>(std::ceil(center.y + ay + 1.0)));
    if (x0 >= x1 || y0 >= y1)
        return; // wholly off the document

    ensureCovers(x0, y0, x1, y1); // grow the bounded working rect to hold this dab

    // AFTER the off-document return, not before: `dabColor` is consulted once per dab that actually
    // LANDS. Its `dab` argument already carries the index of every dab the stroke laid, clipped ones
    // included, so a stroke at the edge still gets the colours it would have got in the middle -- and
    // a callback that is a pure function of that index (the contract) needs no call it cannot use.
    const DabDeposit dep = beginDeposit(dab, dabNo, pressure);

    for (int y = y0; y < y1; ++y) {
        const double dy = (static_cast<double>(y) + 0.5) - center.y;
        for (int x = x0; x < x1; ++x) {
            const double dx = (static_cast<double>(x) + 0.5) - center.x;
            // Into the tip's frame, then un-squash its aspect -- which turns the ellipse back into
            // the circle of radius R that the falloff is written for, so one `dabCoverage` serves
            // every shape.
            const double tx = dx * cosT + dy * sinT;
            const double ty = (-dx * sinT + dy * cosT) * invRatio;
            const double cov = dabCoverage(std::sqrt(tx * tx + ty * ty), R, m_params.hardness);
            if (cov <= 0.0)
                continue;
            deposit(x, y, cov, dep);
        }
    }
}

void BrushEngine::stampTipDab(const Dab& dab, double R, std::size_t dabNo, double pressure) {
    const BrushTip& tip = *m_params.tip;

    // The dab's geometry, as the TIP paints it. `2*R` rather than `dab.diameter`, so the floor that
    // keeps a sub-pixel dab depositing onto its centre pixel applies to a real tip too and the two
    // paths cannot disagree about how small a dab may get. A procedural tip fills that envelope
    // exactly; a bitmap tip preserves its frame's own aspect inside it.
    const DabShape want =
        tipDabShape(tip, dab.frame, 2.0 * R, dab.ratio, dab.angleRad, dab.mirrorH, dab.mirrorV);

    DabRequest req;
    req.tipId = tip.id;
    req.centerX = dab.center.x;
    req.centerY = dab.center.y;
    req.width = want.width;
    req.height = want.height;
    req.angleRad = want.angleRad;
    req.softness = dab.softness;
    req.frame = dab.frame;
    req.mirrorH = want.mirrorH;
    req.mirrorV = want.mirrorV;

    // Quantize FIRST, then render from the QUANTIZED values: the cache is exactly transparent only
    // because nothing downstream ever sees the raw request again (dab_cache.hpp). So the renderer
    // below is handed `q.shape` and `q.placement.subX/subY` -- both decoded from the key -- and a hit
    // and a miss return the same bytes.
    const QuantizedDab q = m_dabCache.quantize(req);
    if (q.empty())
        return; // degenerate, or larger than kMaxDabExtent: no mask, and so no dab

    const std::shared_ptr<const DabMask> mask = m_dabCache.get(q.key, [&] {
        return renderTipMask(tip, q.key.frame, q.shape,
                             dabSoftnessFromKey(q.key, m_dabCache.quantization()), q.placement.subX,
                             q.placement.subY);
    });
    if (mask->empty())
        return;

    // The blit's box is the MASK's own, never the placement's: dab_mask.hpp is explicit that a mask
    // one pixel narrower than its placement is a buffer overrun, and the mask's dims are the truth.
    // Clipped to the document, exactly as the analytic path clips its footprint.
    const int px = q.placement.x;
    const int py = q.placement.y;
    const int x0 = std::max(0, px);
    const int y0 = std::max(0, py);
    const int x1 = std::min(static_cast<int>(m_w), px + static_cast<int>(mask->width));
    const int y1 = std::min(static_cast<int>(m_h), py + static_cast<int>(mask->height));
    if (x0 >= x1 || y0 >= y1)
        return; // wholly off the document

    ensureCovers(x0, y0, x1, y1);

    const DabDeposit dep = beginDeposit(dab, dabNo, pressure); // once the dab is known to land

    // The hatching lattice for THIS dab (§6.6g), over the mask's own rect and in the mask's own
    // frame -- its phase comes from the document origin, so two overlapping dabs continue one
    // another's lines instead of each starting its own. Built once per dab and then read as a
    // second mask: the dab's content is the lattice, CLIPPED BY the tip, which is exactly §6.6(c)'s
    // "procedural pattern clipped by the tip mask".
    if (m_hatchActive) {
        hatchStencil(m_params.hatching, m_dabHatch, static_cast<double>(px),
                     static_cast<double>(py), static_cast<int>(mask->width),
                     static_cast<int>(mask->height), m_hatchStencil);
    }

    for (int y = y0; y < y1; ++y) {
        const auto my = static_cast<std::uint32_t>(y - py);
        for (int x = x0; x < x1; ++x) {
            std::uint8_t v = mask->at(static_cast<std::uint32_t>(x - px), my);
            if (v == 0)
                continue; // a zero mask thresholds to zero anyway
            if (m_hatchActive) {
                // The stencil multiplies the tip's own coverage, in 8-bit with round-to-nearest.
                // Gated on the frozen flag, so with hatching off this loop is byte-identical to
                // what it was before the engine had a lattice at all.
                const std::uint8_t h =
                    m_hatchStencil[static_cast<std::size_t>(my) *
                                       static_cast<std::size_t>(mask->width) +
                                   static_cast<std::size_t>(x - px)];
                if (h == 0)
                    continue;
                v = static_cast<std::uint8_t>((static_cast<int>(v) * h + 127) / 255);
                if (v == 0)
                    continue;
            }
            // The Sharpness threshold (§6.6e): harden the 8-bit mask before it deposits. Gated on the
            // frozen m_sharpnessActive, so with the option off this loop is byte-identical to before.
            if (m_sharpnessActive) {
                v = sharpnessThreshold(v, m_dabSharpness, m_sharpnessSoftness);
                if (v == 0)
                    continue;
            }
            // The TEXTURE composite (§6.6h), AFTER the sharpness threshold -- the reference's
            // post-processing step runs the two in exactly that order, and it matters: a texture
            // applied first would be thresholded back to 1-bit and vanish. The pattern is read at
            // the DOCUMENT pixel, so two overlapping dabs agree about the grain. Gated on the
            // frozen flag: with texturing off this loop is byte-identical to what it was.
            if (m_textureActive) {
                const std::uint8_t t = textureValueAt(*m_params.texture.pattern, x, y,
                                                      m_textureOffX, m_textureOffY);
                v = textureComposite(m_params.texture.mode, t, v,
                                     textureStrength8(m_dabTextureStrength),
                                     m_params.texture.softTexturing);
                if (v == 0)
                    continue;
            }
            deposit(x, y, static_cast<double>(v) / 255.0, dep);
        }
    }
}

void BrushEngine::seedSmudge(int bx0, int by0, int bx1, int by1) {
    // The §6.6b DabSource snapshot, bulk form. The box is already clamped to the layer and covered
    // by the working rect (the caller ran ensureCovers). An UNFILLED pixel's target bytes are
    // pristine -- composite() never writes a pixel whose coverage is 0, and every pixel the smudge
    // walk deposits into was seeded here first -- so reading m_target here is reading the
    // pre-stroke canvas, never the stroke.
    for (int y = by0; y < by1; ++y) {
        for (int x = bx0; x < bx1; ++x) {
            const std::size_t ci =
                static_cast<std::size_t>(y - m_oy) * m_cw + static_cast<std::size_t>(x - m_ox);
            if (m_baseFilled[ci])
                continue;
            m_baseFilled[ci] = 1;
            const std::size_t tp = (static_cast<std::size_t>(y) * m_w + x) * 4;
            const std::size_t bp = ci * 4;
            m_base.rgba[bp] = m_target->rgba[tp];
            m_base.rgba[bp + 1] = m_target->rgba[tp + 1];
            m_base.rgba[bp + 2] = m_target->rgba[tp + 2];
            m_base.rgba[bp + 3] = m_target->rgba[tp + 3];
            // The state buffer seeds PREMULTIPLIED (brush_engine.hpp: the reference's COPY
            // composite is a componentwise lerp in this form).
            const double a = m_base.rgba[bp + 3] / 255.0;
            m_smudgeState[bp] = static_cast<float>(m_base.rgba[bp] / 255.0 * a);
            m_smudgeState[bp + 1] = static_cast<float>(m_base.rgba[bp + 1] / 255.0 * a);
            m_smudgeState[bp + 2] = static_cast<float>(m_base.rgba[bp + 2] / 255.0 * a);
            m_smudgeState[bp + 3] = static_cast<float>(a);
        }
    }
}

void BrushEngine::stampSmudgeDab(const Dab& dab, double R, double pressure) {
    (void)pressure; // no colour dynamics on the smudge walk: the paint colour is the stroke's own
    const BrushTip& tip = *m_params.tip;

    // The dab's mask: the same shape/quantize/cache steps as stampTipDab, with ONE deliberate
    // difference -- in SMEARING mode the sub-pixel phase is disabled (the reference turns it off
    // for the whole stroke, its bug 327235: the smear must copy ALIGNED areas, or the patch
    // resamples against its source and the chain blurs). Quantizing against subPixelSteps = 1
    // forces phase bin 0, which decodes to the same mask bytes under the cache's own quantization
    // -- a phase-0 dab of the normal walk and a smear dab share cache entries, correctly.
    const DabShape want =
        tipDabShape(tip, dab.frame, 2.0 * R, dab.ratio, dab.angleRad, dab.mirrorH, dab.mirrorV);

    DabRequest req;
    req.tipId = tip.id;
    req.centerX = dab.center.x;
    req.centerY = dab.center.y;
    req.width = want.width;
    req.height = want.height;
    req.angleRad = want.angleRad;
    req.softness = dab.softness;
    req.frame = dab.frame;
    req.mirrorH = want.mirrorH;
    req.mirrorV = want.mirrorV;

    DabQuantization quant = m_dabCache.quantization();
    if (!m_params.smudge.dulling)
        quant.subPixelSteps = 1;
    const QuantizedDab q = quantizeDab(req, quant);
    if (q.empty())
        return; // degenerate: the reference bails before its mask too, anchor untouched

    const std::shared_ptr<const DabMask> mask = m_dabCache.get(q.key, [&] {
        return renderTipMask(tip, q.key.frame, q.shape,
                             dabSoftnessFromKey(q.key, m_dabCache.quantization()), q.placement.subX,
                             q.placement.subY);
    });
    if (mask->empty())
        return;

    // The anchor advances on every rendered dab -- clipped ones included, a property of the
    // stroke's geometry -- and it is the UNCLIPPED rect's centre, exactly the reference's
    // QRectF(dstDabRect).center() recorded before its first-run gate and before any clipping.
    const common::Vec2 rectCenter{q.placement.x + 0.5 * mask->width,
                                  q.placement.y + 0.5 * mask->height};
    const bool firstDab = !m_haveSmudgeAnchor;
    const common::Vec2 anchor = m_smudgeAnchor;
    m_smudgeAnchor = rectCenter;
    m_haveSmudgeAnchor = true;
    // THE FIRST DAB PAINTS NOTHING (the reference's m_firstRun): it only plants the anchor the
    // second dab's source patch is read at. A one-dab smudge stroke leaves the canvas untouched.
    if (firstDab)
        return;

    const int px = q.placement.x;
    const int py = q.placement.y;
    const int x0 = std::max(0, px);
    const int y0 = std::max(0, py);
    const int x1 = std::min(static_cast<int>(m_w), px + static_cast<int>(mask->width));
    const int y1 = std::min(static_cast<int>(m_h), py + static_cast<int>(mask->height));
    if (x0 >= x1 || y0 >= y1)
        return; // wholly off the document (the anchor above already advanced)

    // The per-dab factors, resolved once (resolveDab evaluated the sensors already).
    const double fScale = clamp01(m_dabSmudgeRate * m_dabSmudgeOpacity);
    const double oc = smudgeColorRateOpacity(m_dabColorRate, m_dabSmudgeOpacity,
                                             m_params.smudge.maxSmudgeRate);
    // ⚠ fScale gates the WHOLE blt (it multiplies every pixel's factor), so with fScale == 0 the
    // colour rate cannot land either -- exactly the reference, where the final painter's opacity
    // is smudgeRate * opacity and a zero makes the blt a no-op whatever the blend device holds.
    if (fScale <= 0.0)
        return;

    // The smear offset: INTEGER, the previous dab's rect centre minus this one's, rounded --
    // the reference's (lastPaintPos - newCenterPos).toPoint().
    const int offX = static_cast<int>(std::lround(anchor.x - rectCenter.x));
    const int offY = static_cast<int>(std::lround(anchor.y - rectCenter.y));

    // Grow the working rect over everything this dab READS as well as writes, then seed the state
    // buffer there (the DabSource step). Reads outside the document stay transparent -- the
    // reference's unbounded device answers its default pixel there -- so only the readable parts
    // join the box.
    int ux0 = x0;
    int uy0 = y0;
    int ux1 = x1;
    int uy1 = y1;
    SmudgeRect sample{};
    if (m_params.smudge.dulling) {
        // The source rect is the UNCLIPPED dst rect translated by the offset (the reference's
        // srcDabRect), and the sample rect blows it by the radius.
        const SmudgeRect srcRect{px + offX, py + offY, static_cast<int>(mask->width),
                                 static_cast<int>(mask->height)};
        sample = smudgeSampleRect(srcRect, m_dabSmudgeRadius);
        if (sample.x < static_cast<int>(m_w) && sample.x + sample.w > 0 &&
            sample.y < static_cast<int>(m_h) && sample.y + sample.h > 0) {
            ux0 = std::min(ux0, std::max(0, sample.x));
            uy0 = std::min(uy0, std::max(0, sample.y));
            ux1 = std::max(ux1, std::min(static_cast<int>(m_w), sample.x + sample.w));
            uy1 = std::max(uy1, std::min(static_cast<int>(m_h), sample.y + sample.h));
        }
    } else {
        const int sx0 = x0 + offX;
        const int sy0 = y0 + offY;
        const int sx1 = x1 + offX;
        const int sy1 = y1 + offY;
        if (sx0 < static_cast<int>(m_w) && sx1 > 0 && sy0 < static_cast<int>(m_h) && sy1 > 0) {
            ux0 = std::min(ux0, std::max(0, sx0));
            uy0 = std::min(uy0, std::max(0, sy0));
            ux1 = std::max(ux1, std::min(static_cast<int>(m_w), sx1));
            uy1 = std::max(uy1, std::min(static_cast<int>(m_h), sy1));
        }
    }
    ensureCovers(ux0, uy0, ux1, uy1);
    seedSmudge(ux0, uy0, ux1, uy1);

    // A read of the state buffer at document (sx, sy): transparent outside the document, seeded
    // inside it (the union box above covered every in-document read this dab makes).
    const auto readState = [&](int sx, int sy, double out[4]) {
        if (sx < 0 || sy < 0 || sx >= static_cast<int>(m_w) || sy >= static_cast<int>(m_h)) {
            out[0] = out[1] = out[2] = out[3] = 0.0;
            return;
        }
        const std::size_t ci =
            static_cast<std::size_t>(sy - m_oy) * m_cw + static_cast<std::size_t>(sx - m_ox);
        const float* s = &m_smudgeState[ci * 4];
        out[0] = s[0];
        out[1] = s[1];
        out[2] = s[2];
        out[3] = s[3];
    };

    // The paint colour, premultiplied by its own alpha. The reference's paint is opaque and its
    // colour rate composites OVER; with a translucent foreground the true premultiplied
    // over-at-opacity is `paint * oc + blend * (1 - paintAlpha * oc)`, which collapses to the
    // reference's plain lerp at alpha 1.
    const double pa = m_params.color.a / 255.0;
    const double paint[4] = {m_params.color.r / 255.0 * pa, m_params.color.g / 255.0 * pa,
                             m_params.color.b / 255.0 * pa, pa};
    const auto foldColorRate = [&](double v[4]) {
        if (oc <= 0.0)
            return;
        const double keep = 1.0 - pa * oc;
        v[0] = paint[0] * oc + v[0] * keep;
        v[1] = paint[1] * oc + v[1] * keep;
        v[2] = paint[2] * oc + v[2] * keep;
        v[3] = paint[3] * oc + v[3] * keep;
    };

    const int w = x1 - x0;
    const int h = y1 - y0;

    double fill[4] = {0.0, 0.0, 0.0, 0.0};
    if (m_params.smudge.dulling) {
        // DULLING: flood the dab with the sampled average -- the reference's AveragedSampleWrapper
        // (a plain mean of premultiplied pixels; its alpha-weighted straight mean is exactly that)
        // over Halton-sequence points of the sample rect, converging in batches. Out-of-document
        // points sample transparent and COUNT, as the reference's unbounded read does.
        const long long numPixels = static_cast<long long>(sample.w) * sample.h;
        HaltonSequence hx(2);
        HaltonSequence hy(3);
        double sum[4] = {0.0, 0.0, 0.0, 0.0};
        long long n = 0;
        const auto sampleOne = [&] {
            const int sx = sample.x + hx.generate(sample.w - 1);
            const int sy = sample.y + hy.generate(sample.h - 1);
            double v[4];
            readState(sx, sy, v);
            sum[0] += v[0];
            sum[1] += v[1];
            sum[2] += v[2];
            sum[3] += v[3];
            ++n;
        };
        // The straight-space 8-bit-scale mean the reference's convergence metric compares
        // (differenceA: the max channel difference, alpha included).
        const auto straightMean = [&](double out[4]) {
            const double a = sum[3] / static_cast<double>(n);
            out[3] = a * 255.0;
            const double inv = a > 0.0 ? 255.0 / (a * static_cast<double>(n)) : 0.0;
            out[0] = sum[0] * inv;
            out[1] = sum[1] * inv;
            out[2] = sum[2] * inv;
        };
        const long long minSamples =
            std::min(numPixels,
                     std::max<long long>(64, std::llround(0.02 * static_cast<double>(numPixels))));
        for (long long i = 0; i < minSamples; ++i)
            sampleOne();
        double last[4];
        straightMean(last);
        long long left = numPixels - minSamples;
        while (left > 0) {
            const long long batch = std::min<long long>(left, 16);
            for (long long i = 0; i < batch; ++i)
                sampleOne();
            double cur[4];
            straightMean(cur);
            const double diff =
                std::max(std::max(std::fabs(cur[0] - last[0]), std::fabs(cur[1] - last[1])),
                         std::max(std::fabs(cur[2] - last[2]), std::fabs(cur[3] - last[3])));
            if (diff <= 2.0)
                break;
            last[0] = cur[0];
            last[1] = cur[1];
            last[2] = cur[2];
            last[3] = cur[3];
            left -= batch;
        }
        fill[0] = sum[0] / static_cast<double>(n);
        fill[1] = sum[1] / static_cast<double>(n);
        fill[2] = sum[2] / static_cast<double>(n);
        fill[3] = sum[3] / static_cast<double>(n);
        foldColorRate(fill);
    } else {
        // SMEARING: copy the source patch FIRST (the reference reads its whole blend device before
        // the blt) -- the source and destination rects overlap whenever the spacing is tighter
        // than the dab, and an in-place walk would read pixels this same dab already wrote.
        m_smudgeScratch.resize(static_cast<std::size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                double v[4];
                readState(x0 + x + offX, y0 + y + offY, v);
                foldColorRate(v);
                float* d = &m_smudgeScratch[(static_cast<std::size_t>(y) * w + x) * 4];
                d[0] = static_cast<float>(v[0]);
                d[1] = static_cast<float>(v[1]);
                d[2] = static_cast<float>(v[2]);
                d[3] = static_cast<float>(v[3]);
            }
        }
    }

    // The blt: the state lerps toward the blend through the dab's mask -- the reference's final
    // COMPOSITE_COPY at `mask * fScale`, which is a componentwise premultiplied lerp, ALPHA
    // INCLUDED (a smudge can lower alpha; that is what lets it eat a stroke's trailing edge).
    // Coverage accumulates the geometric mask beside it (the Inpaint contract), and only pixels
    // the blt actually moves (f > 0) are marked at all.
    for (int y = y0; y < y1; ++y) {
        const auto my = static_cast<std::uint32_t>(y - py);
        for (int x = x0; x < x1; ++x) {
            std::uint8_t mv = mask->at(static_cast<std::uint32_t>(x - px), my);
            if (mv == 0)
                continue;
            // The TEXTURE composite (§6.6h) rides the smudge walk too: the reference installs its
            // texture option on the brush-based paintop base colorsmudge derives from, and the
            // smudge dab's mask goes through the very same post-processing step the pixel brush's
            // does. (Sharpness does NOT -- colorsmudge installs no sharpness option at all -- which
            // is why only one of the two appears here.) Gated on the frozen flag.
            if (m_textureActive) {
                const std::uint8_t t = textureValueAt(*m_params.texture.pattern, x, y,
                                                      m_textureOffX, m_textureOffY);
                mv = textureComposite(m_params.texture.mode, t, mv,
                                      textureStrength8(m_dabTextureStrength),
                                      m_params.texture.softTexturing);
                if (mv == 0)
                    continue;
            }
            const double m = static_cast<double>(mv) / 255.0;
            double f = m * fScale;
            // composite() copies the smudge STATE verbatim, so confinement has to land in the state
            // itself: scaling the blt's weight makes a half-selected pixel take half the smear and
            // an unselected one none. `f * 1.0 == f` exactly, and the null check keeps an
            // unconfined smudge on the identical expression it always ran.
            if (m_confine != nullptr) {
                const double k = m_confine->at(x, y);
                if (k <= 0.0)
                    continue;
                f *= k;
            }
            const std::size_t ci =
                static_cast<std::size_t>(y - m_oy) * m_cw + static_cast<std::size_t>(x - m_ox);
            float* s = &m_smudgeState[ci * 4];
            const float* b = m_params.smudge.dulling
                                 ? nullptr
                                 : &m_smudgeScratch[(static_cast<std::size_t>(y - y0) * w +
                                                     (x - x0)) *
                                                    4];
            const double b0 = b != nullptr ? b[0] : fill[0];
            const double b1 = b != nullptr ? b[1] : fill[1];
            const double b2 = b != nullptr ? b[2] : fill[2];
            const double b3 = b != nullptr ? b[3] : fill[3];
            s[0] = static_cast<float>(s[0] + (b0 - s[0]) * f);
            s[1] = static_cast<float>(s[1] + (b1 - s[1]) * f);
            s[2] = static_cast<float>(s[2] + (b2 - s[2]) * f);
            s[3] = static_cast<float>(s[3] + (b3 - s[3]) * f);
            float& cov = m_coverage[ci];
            cov = static_cast<float>(cov + m * (1.0 - cov));
            m_total.add(x, y);
            m_pending.add(x, y);
        }
    }
}

void BrushEngine::stampMask(common::Vec2 center, double pressure) {
    const MaskingParams& mp = m_params.masking;
    // The same sub-pixel floor as the primary's stamp(), so the two paths cannot disagree about
    // how small a dab may get.
    const double R = std::max(0.5 * maskingDiameter(pressure), 0.6);
    double flow = mp.flow;
    if (mp.flowFromPressure)
        flow *= clamp01(pressure);
    flow = clamp01(flow);
    if (flow <= 0.0)
        return;

    // A REAL masking tip -- the nested brush_definition's own generator or bitmap -- rasterizes
    // through the dab cache and blits, exactly as the primary's stampTipDab does. The analytic
    // disc below is the null-tip path only: a mask authored as an eroded texture that stamps as a
    // solid disc subtracts the whole nib and deletes the very stroke it was meant to texture.
    if (mp.tip) {
        stampMaskTipDab(center, 2.0 * R, flow);
        return;
    }

    const int x0 = std::max(0, static_cast<int>(std::floor(center.x - R - 1.0)));
    const int y0 = std::max(0, static_cast<int>(std::floor(center.y - R - 1.0)));
    const int x1 = std::min(static_cast<int>(m_w), static_cast<int>(std::ceil(center.x + R + 1.0)));
    const int y1 = std::min(static_cast<int>(m_h), static_cast<int>(std::ceil(center.y + R + 1.0)));
    if (x0 >= x1 || y0 >= y1)
        return; // wholly off the document

    ensureCovers(x0, y0, x1, y1);

    for (int y = y0; y < y1; ++y) {
        const double dy = (static_cast<double>(y) + 0.5) - center.y;
        for (int x = x0; x < x1; ++x) {
            const double dx = (static_cast<double>(x) + 0.5) - center.x;
            const double cov = dabCoverage(std::sqrt(dx * dx + dy * dy), R, mp.hardness);
            if (cov <= 0.0)
                continue;
            depositMask(x, y, flow * cov);
        }
    }
}

void BrushEngine::stampMaskTipDab(common::Vec2 center, double diameter, double flow) {
    const MaskingParams& mp = m_params.masking;
    const BrushTip& tip = *mp.tip;

    // Frame 0, no mirror, softness 1.0 (as authored): the masking walk drives no options, so an
    // animated masking tip stamps its first cell and nothing jitters. (No shipped masking tip is
    // a hose.)
    const DabShape want = tipDabShape(tip, 0, diameter, mp.ratio, mp.angleRad, false, false);

    DabRequest req;
    req.tipId = tip.id;
    req.centerX = center.x;
    req.centerY = center.y;
    req.width = want.width;
    req.height = want.height;
    req.angleRad = want.angleRad;
    req.softness = 1.0;
    req.frame = 0;
    req.mirrorH = want.mirrorH;
    req.mirrorV = want.mirrorV;

    // Quantize FIRST, then render from the QUANTIZED values -- the same transparency contract as
    // stampTipDab, against the same cache (the masking tip's own raster id keeps the two brushes'
    // masks apart).
    const QuantizedDab q = m_dabCache.quantize(req);
    if (q.empty())
        return; // degenerate, or larger than kMaxDabExtent: no mask, and so no dab

    const std::shared_ptr<const DabMask> mask = m_dabCache.get(q.key, [&] {
        return renderTipMask(tip, q.key.frame, q.shape,
                             dabSoftnessFromKey(q.key, m_dabCache.quantization()), q.placement.subX,
                             q.placement.subY);
    });
    if (mask->empty())
        return;

    // The blit's box is the MASK's own, clipped to the document -- as stampTipDab.
    const int px = q.placement.x;
    const int py = q.placement.y;
    const int x0 = std::max(0, px);
    const int y0 = std::max(0, py);
    const int x1 = std::min(static_cast<int>(m_w), px + static_cast<int>(mask->width));
    const int y1 = std::min(static_cast<int>(m_h), py + static_cast<int>(mask->height));
    if (x0 >= x1 || y0 >= y1)
        return; // wholly off the document

    ensureCovers(x0, y0, x1, y1);

    for (int y = y0; y < y1; ++y) {
        const auto my = static_cast<std::uint32_t>(y - py);
        for (int x = x0; x < x1; ++x) {
            const std::uint8_t v = mask->at(static_cast<std::uint32_t>(x - px), my);
            if (v == 0)
                continue;
            depositMask(x, y, flow * (static_cast<double>(v) / 255.0));
        }
    }
}

void BrushEngine::depositMask(int x, int y, double a) {
    const std::size_t ci =
        static_cast<std::size_t>(y - m_oy) * m_cw + static_cast<std::size_t>(x - m_ox);
    // The same wash-style accumulation as the coverage. No base snapshot (the mask is not a paint
    // touch; the snapshot happens when the PAINT stroke first reaches a pixel), no coverage write
    // (the Inpaint mask records paint, not masking).
    float& dst = m_mask[ci];
    dst = static_cast<float>(dst + a * (1.0 - dst));
    // A masking dab over an already-composited pixel changes its alpha, so the pixel must be
    // recomposited -- pending/total must grow exactly like a paint dab's. Where the paint stroke
    // never lands, composite() skips these pixels and the marks are inert.
    m_total.add(x, y);
    m_pending.add(x, y);
}

common::Rect BrushEngine::dirtyBounds() const {
    return m_total.rect();
}

common::Rect BrushEngine::composite() {
    if (m_target == nullptr || !m_pending.valid)
        return {};
    if (m_target->width != m_w || m_target->height != m_h)
        return {}; // the layer was resized under us: composite nothing

    common::Image& out = *m_target;

    // The smudge branch (§6.6c): the state buffer IS the answer -- what the layer looks like under
    // this stroke, evolved dab by dab -- so compositing is unpremultiplying it into the target
    // wherever the stroke actually moved a pixel (coverage > 0; seeded-but-untouched pixels stay
    // pristine, never re-encoded through the float round trip). No cap, no blend, no mask: begin()
    // normalized those axes away, and the reference's colorsmudge writes its device directly.
    if (smudgeActive()) {
        for (int y = m_pending.y0; y < m_pending.y1; ++y) {
            for (int x = m_pending.x0; x < m_pending.x1; ++x) {
                const std::size_t ci =
                    static_cast<std::size_t>(y - m_oy) * m_cw + static_cast<std::size_t>(x - m_ox);
                if (m_coverage[ci] <= 0.0f)
                    continue;
                const float* s = &m_smudgeState[ci * 4];
                const double a = s[3];
                const std::size_t p = (static_cast<std::size_t>(y) * m_w + x) * 4;
                const auto to8 = [](double v) {
                    return static_cast<std::uint8_t>(std::lround(clamp01(v) * 255.0));
                };
                if (a <= 0.0) {
                    // Premultiplication lost the RGB at alpha 0: keep the base colour under a
                    // fully-eaten pixel, the engine's erase convention (the masked sa == 0 branch
                    // below does the same).
                    const std::size_t bp = ci * 4;
                    out.rgba[p] = m_base.rgba[bp];
                    out.rgba[p + 1] = m_base.rgba[bp + 1];
                    out.rgba[p + 2] = m_base.rgba[bp + 2];
                    out.rgba[p + 3] = 0;
                    continue;
                }
                // Per-channel division, NOT multiplication by 1/a: IEEE x/x == 1.0 exactly, the
                // same normalization rule the Colored path pins.
                out.rgba[p] = to8(s[0] / a);
                out.rgba[p + 1] = to8(s[1] / a);
                out.rgba[p + 2] = to8(s[2] / a);
                out.rgba[p + 3] = to8(a);
            }
        }
        const common::Rect done = m_pending.rect();
        m_pending = Box{};
        return done;
    }

    const bool build = buildup();
    const bool erase = m_params.strokeMode == StrokeMode::Erase;
    const bool normal = m_params.blendMode == BlendMode::Normal;
    const bool perDab = colored() && !m_colored.empty();
    // Any pending pixel implies ensureCovers ran, which allocates every active buffer -- so when
    // masking is active, m_mask is never empty here.
    const bool masked = maskingActive();
    const MaskingOp mop = m_params.masking.op;
    const double fr = m_params.color.r / 255.0;
    const double fg = m_params.color.g / 255.0;
    const double fb = m_params.color.b / 255.0;

    for (int y = m_pending.y0; y < m_pending.y1; ++y) {
        for (int x = m_pending.x0; x < m_pending.x1; ++x) {
            const std::size_t ci =
                static_cast<std::size_t>(y - m_oy) * m_cw + static_cast<std::size_t>(x - m_ox);
            // Wash caps the accumulated coverage at the stroke's ceiling exactly once, here.
            // Buildup already folded that ceiling into each dab, so its accumulation IS the alpha.
            double sa = build ? static_cast<double>(m_build[ci]) : m_coverage[ci] * m_cap;
            if (!masked) {
                if (sa <= 0.0)
                    continue; // never stamped (gap in the bbox / zero cap): target stays pristine
                if (m_confine != nullptr) {
                    // The selection's coverage multiplies the stroke's finished alpha, so a
                    // feathered edge takes a PROPORTION of the paint rather than an all-or-nothing
                    // clip. At 255 the factor is exactly 1.0 (at() divides), so a fully selected
                    // pixel is byte-identical to an unconfined one.
                    sa *= m_confine->at(x, y);
                    if (sa <= 0.0)
                        continue; // outside the selection: deposit() never touched this pixel
                                  // either, so the target here is still pristine
                }
            } else {
                // The mask never paints alone (docs/brushes.md §6.2): a pixel the PAINT stroke
                // never touched stays pristine whatever the mask accumulated there. Gating on the
                // paint accumulation subsumes the reference's linear_dodge zero-guard and closes
                // hard_mix's mask-alone corner for every op.
                const double raw =
                    build ? static_cast<double>(m_build[ci]) : static_cast<double>(m_coverage[ci]);
                if (raw <= 0.0)
                    continue;
                // The op applies to the ACCUMULATED stroke alpha, before Wash's ceiling -- the
                // reference composites the mask onto the pre-opacity stroke device. Buildup folded
                // its ceiling into each dab, so there the op necessarily lands post-cap (a
                // combination the reference cannot express at all; §6.2).
                sa = maskingOp(mop, m_mask[ci], raw);
                if (!build)
                    sa *= m_cap;
                if (m_confine != nullptr)
                    sa *= m_confine->at(x, y); // the selection bounds what the masking op shaped
                // sa == 0 does NOT skip here: an earlier composite may have written this pixel
                // before a later masking dab carved it back to nothing, and only writing the base
                // bytes back undoes that. VERBATIM base bytes, not the general arithmetic with
                // sa = 0 -- that detour lands in the oa <= 1e-6 branch when the base is fully
                // transparent and would stomp its stashed RGB (which erase keeps un-premultiplied)
                // with zeros.
                if (sa <= 0.0) {
                    const std::size_t bz = ci * 4;
                    const std::size_t tz = (static_cast<std::size_t>(y) * m_w + x) * 4;
                    out.rgba[tz] = m_base.rgba[bz];
                    out.rgba[tz + 1] = m_base.rgba[bz + 1];
                    out.rgba[tz + 2] = m_base.rgba[bz + 2];
                    out.rgba[tz + 3] = m_base.rgba[bz + 3];
                    continue;
                }
            }
            const std::size_t bp = ci * 4; // base snapshot index (working rect)
            const std::size_t p = (static_cast<std::size_t>(y) * m_w + x) * 4; // target index
            const double br = m_base.rgba[bp] / 255.0;
            const double bg = m_base.rgba[bp + 1] / 255.0;
            const double bb = m_base.rgba[bp + 2] / 255.0;
            const double ba = m_base.rgba[bp + 3] / 255.0;
            const auto to8 = [](double v) {
                return static_cast<std::uint8_t>(std::lround(clamp01(v) * 255.0));
            };

            if (erase) {
                // Destination-out against the pristine base: keep the colour, carve the alpha. The
                // colour must survive un-premultiplied so that erasing to alpha 0 and painting back
                // over it does not drag a black fringe in from nowhere.
                out.rgba[p] = m_base.rgba[bp];
                out.rgba[p + 1] = m_base.rgba[bp + 1];
                out.rgba[p + 2] = m_base.rgba[bp + 2];
                out.rgba[p + 3] = to8(ba * (1.0 - sa));
                continue;
            }

            // The paint colour at this pixel: the stroke's one colour on the Uniform path, or the
            // dab accumulation normalized back from premultiplied form on the Colored one. For
            // Uniform these assignments are exact copies, so the arithmetic below is bit-for-bit
            // the pinned expression.
            double pr = fr;
            double pg = fg;
            double pb = fb;
            if (perDab) {
                const float* acc = &m_colored[ci * 4];
                const double ca = acc[3];
                // sa > 0 means at least one dab deposited here, so ca > 0 whenever it matters; the
                // guard only keeps a zero-alpha cell from dividing (leaving the stroke colour).
                //
                // Per-channel division, NOT multiplication by 1/ca: IEEE guarantees x/x == 1.0
                // exactly, while x*(1/x) can land one ulp under it. A channel deposited at 0 or at
                // full therefore normalizes to exactly 0.0 or 1.0, which is what lets a
                // constant-pure-colour Colored stroke reproduce the Uniform path byte-for-byte
                // (tests/test_brush_colored.cpp pins that).
                if (ca > 0.0) {
                    pr = clamp01(acc[0] / ca);
                    pg = clamp01(acc[1] / ca);
                    pb = clamp01(acc[2] / ca);
                }
            }

            // The source colour, after the blend mode has mixed it with the backdrop. Normal is
            // short-circuited to the paint colour itself rather than routed through the identity
            // blend: the arithmetic below is the expression `Uniform x Wash` is pinned to
            // byte-for-byte (tests/test_brush_wash_golden.cpp), and re-deriving `pr` as
            // `(1 - ba) * pr + ba * blendChannel(Normal, br, pr)` would round differently.
            double sr = pr;
            double sg = pg;
            double sb = pb;
            if (!normal) {
                // `core::detail`, spelled out: brush/bitmap_tip.hpp has a `brush::detail` of its
                // own, which an unqualified `detail::` would find first.
                const auto cb = core::detail::Rgb{static_cast<float>(br), static_cast<float>(bg),
                                                  static_cast<float>(bb)};
                const auto cs = core::detail::Rgb{static_cast<float>(pr), static_cast<float>(pg),
                                                  static_cast<float>(pb)};
                const core::detail::Rgb bl =
                    isSeparable(m_params.blendMode)
                        ? core::detail::Rgb{blendChannel(m_params.blendMode, cb.r, cs.r),
                                            blendChannel(m_params.blendMode, cb.g, cs.g),
                                            blendChannel(m_params.blendMode, cb.b, cs.b)}
                        : core::detail::blendNonSeparable(m_params.blendMode, cb, cs);
                // W3C: the blended colour is weighted into the source by the backdrop's alpha, so a
                // blend mode fades out over transparent pixels rather than blending against black.
                sr = (1.0 - ba) * pr + ba * static_cast<double>(bl.r);
                sg = (1.0 - ba) * pg + ba * static_cast<double>(bl.g);
                sb = (1.0 - ba) * pb + ba * static_cast<double>(bl.b);
            }

            const double oa = sa + ba * (1.0 - sa); // straight-alpha source-over
            double orr = 0.0;
            double og = 0.0;
            double ob = 0.0;
            if (oa > 1e-6) {
                const double inv = 1.0 / oa;
                orr = (sr * sa + br * ba * (1.0 - sa)) * inv;
                og = (sg * sa + bg * ba * (1.0 - sa)) * inv;
                ob = (sb * sa + bb * ba * (1.0 - sa)) * inv;
            }
            out.rgba[p] = to8(orr);
            out.rgba[p + 1] = to8(og);
            out.rgba[p + 2] = to8(ob);
            out.rgba[p + 3] = to8(oa);
        }
    }
    const common::Rect done = m_pending.rect();
    m_pending = Box{}; // composited: the next composite() handles only newly-stamped dabs
    return done;
}

void BrushEngine::restore() {
    if (m_target == nullptr || !m_total.valid)
        return;
    if (m_target->width != m_w || m_target->height != m_h)
        return;
    common::Image& out = *m_target;
    // Only stamped pixels (coverage > 0) were written by composite() and have a base snapshot;
    // unstamped pixels inside the m_total bbox were never touched, so they are already pristine.
    for (int y = m_total.y0; y < m_total.y1; ++y) {
        for (int x = m_total.x0; x < m_total.x1; ++x) {
            const std::size_t ci =
                static_cast<std::size_t>(y - m_oy) * m_cw + static_cast<std::size_t>(x - m_ox);
            if (m_coverage[ci] <= 0.0f)
                continue;
            const std::size_t bp = ci * 4;
            const std::size_t p = (static_cast<std::size_t>(y) * m_w + x) * 4;
            out.rgba[p] = m_base.rgba[bp];
            out.rgba[p + 1] = m_base.rgba[bp + 1];
            out.rgba[p + 2] = m_base.rgba[bp + 2];
            out.rgba[p + 3] = m_base.rgba[bp + 3];
        }
    }
}

} // namespace mosaic::core::brush
