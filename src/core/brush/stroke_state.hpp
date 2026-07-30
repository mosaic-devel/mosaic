#pragma once

#include "common/geometry.hpp"
#include "core/brush/math_util.hpp"
#include "core/brush/sensors.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

// The stroke's running state, and the raw value every sensor reads out of it (docs/brushes.md §6.2).
//
// A sensor is a projection of *the stroke so far* onto [0,1] (or [-1,1] for the additive class).
// Some sensors read the current sample directly (pressure, tilt); the rest need history that only
// this class keeps: how far the stroke has travelled, how long it has run, how fast it is moving,
// which way it is heading, how many dabs have been laid, and two reproducible random streams.
//
// FLTK-, Vulkan- and PLATFORM-free. `StrokeInput` mirrors `platform::TabletSample` (docs/tablet.md
// §2) rather than including it, so the engine can be exercised from a canned sample stream with no
// tablet, no window and no clock -- which is how every test here runs.
namespace mosaic::core::brush {

// One input sample. A mouse stroke fills `pressure = 1` and leaves the valuators at rest; that is
// deliberately not the same as "no dynamics", because a preset may drive size from `fuzzy` or
// `drawingangle`, which need no tablet at all.
struct StrokeInput {
    common::Vec2 pos{};              // document px, sub-pixel
    double pressure = 1.0;           // [0,1]
    double xTilt = 0.0;              // degrees; +/-60 is full scale
    double yTilt = 0.0;              // degrees
    double rotation = 0.0;           // degrees, barrel/art-pen rotation, [-180,180]
    double tangentialPressure = 0.0; // [0,1], airbrush finger wheel
    std::uint64_t timeUs = 0;        // OUR monotonic clock, never the driver's (docs/tablet.md §5)
};

// Calibration for the `speed` sensor (docs/tablet.md §7 makes both a user setting).
struct SpeedParams {
    // The instantaneous speed that reads as 1.0, in DOCUMENT px per millisecond. 3 px/ms is about
    // 3000 px/s -- a brisk flick across a 1080p canvas in a third of a second -- so an ordinary
    // stroke lands in the middle of the sensor's range rather than pinned near zero.
    double maxSpeed = 3.0;
    // Time constant of the exponential moving average. The EMA is driven by elapsed time rather
    // than by sample count, so a 200 Hz tablet and a 60 Hz mouse smooth over the same 30 ms.
    double windowMs = 30.0;
};

// Tilt full-scale, in degrees. The sensors normalize against this, not against the device's own
// range: a device that reports +/-90 saturates early rather than rescaling everyone else's presets.
inline constexpr double kMaxTiltDegrees = 60.0;

// The `speed` sensor's smoother, named as a component because docs/tablet.md §7 makes it one of
// the tablet policy layer's pieces: StrokeState embeds one for the stroke, and the Settings->
// Tablet test area (§8) will run its own over a live sample stream with no stroke at all.
//
// A time-constant EMA over the instantaneous speed: alpha = 1 - exp(-dt/tau). Driven by elapsed
// time rather than sample count, so the sampling rate drops out -- a 200 Hz tablet and a 60 Hz
// mouse smooth over the same window.
class SpeedSmoother {
public:
    void reset() noexcept { m_ema = 0.0; }

    // Fold one movement in. A non-positive dt (replayed event, stepped clock) and a non-positive
    // window both contribute nothing -- the guard lives here, where the division happens.
    void extend(double stepPx, double dtMs) noexcept;

    [[nodiscard]] double pxPerMs() const noexcept { return m_ema; }

    // Put the EMA's VALUE back, for StrokeState::rewindTo. Deliberately not its params: a rewind
    // re-reads the stroke, it does not recalibrate the sensor.
    void setValue(double pxPerMs) noexcept { m_ema = pxPerMs; }

    // The smoothed speed against params().maxSpeed, clamped to [0,1] -- the sensor's reading.
    // A non-positive maxSpeed reads 0 rather than dividing by it.
    [[nodiscard]] double normalized() const noexcept;

    void setParams(const SpeedParams& p) noexcept { m_params = p; }
    [[nodiscard]] const SpeedParams& params() const noexcept { return m_params; }

private:
    SpeedParams m_params{};
    double m_ema = 0.0;
};

// The stroke's derived state AT ONE POINT ALONG IT, as a plain value: the sample, and everything a
// sensor reads that describes where the stroke has got to and how it got there.
//
// It exists because the dab walk does not run in step with the sample stream. The walk lags it by one
// sample (the curve through a sample needs to know where the path goes NEXT -- brush_engine.hpp), so
// by the time a span is stamped the live StrokeState has already advanced past it. A dab must be
// evaluated against the state that belonged to IT, which means that state has to be storable and
// re-installable: hence a value type, and StrokeState::snapshot()/rewindTo().
//
// ⚠ WHAT IS DELIBERATELY NOT IN HERE is everything stroke-scoped and one-way: the two random streams,
// the dab counter, and the latched drawing angle. Those must keep running ACROSS a rewind -- a dab
// that re-reads an earlier point of the stroke must not also re-draw an earlier random number, or
// `fuzzy` would repeat itself once per span. Adding a field here is therefore a decision about
// whether it is a property of a POINT on the stroke or of the STROKE.
struct StrokeSnapshot {
    StrokeInput sample{};
    double distance = 0.0;       // px travelled to this point
    double elapsedMs = 0.0;      // since the stroke began
    double speedPxPerMs = 0.0;   // the EMA's VALUE; its calibration is stroke-scoped and stays put
    double drawingAngle = 0.0;   // radians, direction of travel
    double maxPressure = 0.0;    // the stroke's high-water pressure up to this point
};

// The state at `t` of the way from `a` to `b` -- what a DAB laid BETWEEN two samples reads, since the
// walk lays dabs at a cadence of its own and almost never on a sample.
//
// Every channel blends linearly except three that cannot:
//   - the drawing angle and the barrel rotation are ANGLES, so they take the shorter arc (a pen
//     crossing +/-180 must not spin a whole turn backwards between two samples);
//   - `maxPressure` is a running MAX, and a max does not interpolate: it is `a`'s high-water mark
//     against the blended sample's own pressure, which is exactly what the stroke would have recorded
//     had it been sampled here.
// `t` outside [0,1] is clamped: a dab is never off its own span.
[[nodiscard]] StrokeSnapshot lerpSnapshot(const StrokeSnapshot& a, const StrokeSnapshot& b,
                                          double t) noexcept;

class StrokeState {
public:
    // `seed` fixes both random streams. It is a parameter and never read from a clock, because a
    // stroke has to be replayable: golden images, the editor's preview, and undo/redo of a
    // `fuzzy`-driven stroke all depend on the same seed giving the same dabs.
    void begin(const StrokeInput& first, std::uint64_t seed);

    // Fold one input sample in: distance, elapsed time, speed EMA, drawing angle. Call once per
    // sample, before any dab that sample produces.
    void extendTo(const StrokeInput& sample);

    // Call once per dab, before evaluating its options. Advances the dab counter that `fade` reads.
    void beginDab() noexcept { ++m_dabIndex; }

    [[nodiscard]] const StrokeInput& sample() const noexcept { return m_sample; }
    [[nodiscard]] double distance() const noexcept { return m_distance; }   // px, whole stroke
    [[nodiscard]] double elapsedMs() const noexcept { return m_elapsedMs; }
    [[nodiscard]] double speed() const noexcept;                            // normalized [0,1]
    [[nodiscard]] double drawingAngle() const noexcept { return m_angle; }  // radians, direction of travel
    [[nodiscard]] int dabIndex() const noexcept { return m_dabIndex; }
    [[nodiscard]] double maxPressure() const noexcept { return m_maxPressure; }
    [[nodiscard]] bool active() const noexcept { return m_active; }

    // The drawing angle frozen at the first call of the stroke -- what `lockedAngleMode` reads. It
    // latches lazily rather than at begin() because a stroke has no direction until it moves.
    [[nodiscard]] double lockedDrawingAngle();

    // `fuzzy`: a fresh draw on every call, so two options both driven by fuzzy scatter
    // independently within one dab. Advances the stroke's random stream.
    [[nodiscard]] double nextRandom() noexcept;

    // `fuzzystroke`: one draw held for the whole stroke, distinct per option. Keyed rather than
    // stored, so it costs nothing and does not depend on which options were evaluated or in what
    // order -- evaluating an option twice in a dab must not change its value.
    [[nodiscard]] double strokeRandom(std::string_view key) const noexcept;

    void setSpeedParams(const SpeedParams& p) noexcept { m_speed.setParams(p); }
    [[nodiscard]] const SpeedParams& speedParams() const noexcept { return m_speed.params(); }

    // The derived state, as a value (StrokeSnapshot).
    [[nodiscard]] StrokeSnapshot snapshot() const noexcept;

    // Put the derived state back to `s` -- an earlier point of the stroke, or an interpolated one
    // between two samples. The dab walk brackets each span with this: rewind to the dab's own state,
    // stamp, and put the live state back so that strokeState() keeps reporting the pointer rather
    // than the last dab.
    //
    // ⚠ It moves ONLY what StrokeSnapshot carries. The random streams, the dab counter and the
    // latched angle keep running (see StrokeSnapshot), and so does the stroke's time bookkeeping --
    // `m_lastTimeUs` is what the NEXT arriving sample is diffed against, and a rewind is not the
    // arrival of a sample. Rewinding is a read of the stroke, never an edit of it.
    void rewindTo(const StrokeSnapshot& s) noexcept;

private:
    StrokeInput m_sample{};
    SpeedSmoother m_speed{};

    std::uint64_t m_seed = 0;
    std::uint64_t m_rngState = 0;

    double m_distance = 0.0;
    double m_elapsedMs = 0.0;
    double m_angle = 0.0;
    double m_maxPressure = 0.0;
    std::optional<double> m_lockedAngle;
    std::uint64_t m_lastTimeUs = 0;
    int m_dabIndex = -1; // beginDab() makes the first dab index 0
    bool m_active = false;
};

// The sensor's raw reading, BEFORE its response curve: [0,1] for the scaling class, [-1,1] for the
// additive class, [0,1) for drawingangle. `state` is non-const because `fuzzy` draws from the random
// stream and `lockedAngleMode` latches its angle on first read -- both are reads that change the
// stroke, and hiding that behind a const reference would be a lie.
//
// `optionKey` keys the per-stroke `fuzzystroke` draw so that two options sharing the sensor get
// different constants, as the reference implementation does.
[[nodiscard]] double sensorValue(const Sensor& sensor, StrokeState& state, std::string_view optionKey);

// [-1,1] <-> [0,1]. The additive class is evaluated through its curve in scaling space, because a
// curve's domain is [0,1] and an additive sensor's is not.
[[nodiscard]] constexpr double additiveToScaling(double x) noexcept { return 0.5 * (1.0 + x); }
[[nodiscard]] constexpr double scalingToAdditive(double x) noexcept { return -1.0 + 2.0 * x; }

// Wrap `x` into [lo, hi) -- see math_util.hpp. Re-exported because the dab pipeline and the tests
// reach for it beside the sensors that need it.
using detail::wrapValue;

} // namespace mosaic::core::brush
