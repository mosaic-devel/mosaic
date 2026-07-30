#pragma once

#include "core/brush/curve.hpp"
#include "core/brush/stroke_state.hpp"

#include <vector>

// The global tablet input policy (docs/tablet.md §7): the user-level corrections applied to every
// raw sample at ingest, BEFORE the engine's per-preset dynamics read it. The engine sees a device
// that already behaves the way the user wants -- a worn nib re-spanned to [0,1], a personal feel
// curve, a pen held rotated reading as if held straight -- so presets stay portable across users
// and hardware.
//
// Platform-free on purpose: it operates on `StrokeInput` (the core mirror of
// `platform::TabletSample`), so the whole layer is exercised headlessly from canned streams. The
// remaining §7 pieces live elsewhere or later: SpeedSmoother is in stroke_state.hpp (StrokeState
// embeds one; the Settings->Tablet test area runs its own), and the rope/pulled-string stabilizer is
// deliberately DEFERRED -- it is not built, and nothing in this layer may start implementing it.
namespace mosaic::core::brush {

class TabletPolicy {
public:
    // The raw pressure pipeline, in order:
    //   1. Range remap -- [rawMin, rawMax] stretched to the full [0,1]. First, because it exists
    //      to re-normalize the DEVICE (worn nibs and cheap digitizers never reach 0 or 1); the
    //      response curve is the user's feel on top of a device that now spans its range.
    //   2. Response curve -- the baked 256-entry LUT. An identity curve short-circuits: the
    //      remapped value is returned untouched, not resampled through the LUT.
    // Input outside [0,1] is clamped before the pipeline; the result is always in [0,1].
    [[nodiscard]] double applyPressure(double raw) const noexcept;

    // Rotate the (xTilt, yTilt) vector by the tilt-direction offset, so the derived `ascension`
    // bearing shifts by exactly +offset and the lean MAGNITUDE is untouched -- sensors.cpp
    // expects the offset to be applied here, at ingest, never in the sensor. The `declination`
    // reading may move a fraction of a percent under an offset: the reference elevation formula
    // normalizes by whichever tilt axis dominates, so it is mildly direction-dependent by design
    // and no x/y rewrite can both shift the bearing and hold it exactly still. A zero offset is
    // a byte-exact pass-through.
    void applyTilt(double& xTilt, double& yTilt) const noexcept;

    // The whole policy over one sample. Every field other than pressure and the tilt pair passes
    // through verbatim -- position, rotation, tangential pressure and the timestamp are not the
    // policy's to touch.
    [[nodiscard]] StrokeInput apply(StrokeInput in) const noexcept;

    void setPressureCurve(const Curve& curve);
    // Both bounds are clamped to [0,1] and swapped if handed inverted, so the policy is
    // well-defined for anything a settings file can contain. An equal (degenerate) pair acts as a
    // threshold at that value rather than dividing by its zero span.
    void setPressureRange(double rawMin, double rawMax) noexcept;
    void setTiltOffsetDegrees(double degrees) noexcept;

    [[nodiscard]] const Curve& pressureCurve() const noexcept { return m_curve; }
    [[nodiscard]] double pressureMin() const noexcept { return m_rawMin; }
    [[nodiscard]] double pressureMax() const noexcept { return m_rawMax; }
    [[nodiscard]] double tiltOffsetDegrees() const noexcept { return m_tiltOffsetDegrees; }

    // True when the whole policy is a no-op (identity curve, full range, zero offset) -- the
    // ingest path skips apply() entirely for the default configuration.
    [[nodiscard]] bool isIdentity() const noexcept;

private:
    Curve m_curve;                 // kept for round-tripping to the settings UI
    std::vector<float> m_lut;      // empty while the curve is identity
    double m_rawMin = 0.0;
    double m_rawMax = 1.0;
    double m_tiltOffsetDegrees = 0.0;
    // The rotation baked once at set time. Stored rather than recomputed per sample: sin/cos of
    // the same angle must be the SAME bits on every sample or a still pen would jitter.
    double m_tiltCos = 1.0;
    double m_tiltSin = 0.0;
};

} // namespace mosaic::core::brush
