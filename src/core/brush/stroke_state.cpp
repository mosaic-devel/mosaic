#include "core/brush/stroke_state.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core::brush {

namespace {

// SplitMix64 (Steele/Lea/Flood 2014, public domain). Two jobs here: it advances the per-dab stream,
// and -- being a bijection with good avalanche -- it is also the mixer for the keyed per-stroke
// draw. Small, dependency-free, and above all REPRODUCIBLE across platforms and optimization
// levels, which `std::mt19937` seeded from a distribution is not.
[[nodiscard]] std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// The top 53 bits are the mantissa of a double, so this is exactly uniform on [0,1) with no
// modulo bias and no dependence on <random>'s implementation-defined distributions.
[[nodiscard]] double toUnitInterval(std::uint64_t bits) noexcept {
    return static_cast<double>(bits >> 11) * 0x1.0p-53;
}

// FNV-1a over the option name. Only needs to separate a handful of short ASCII keys.
[[nodiscard]] std::uint64_t hashKey(std::string_view key) noexcept {
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (const char c : key) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001B3ULL;
    }
    return h;
}

using detail::clamp01;

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

// The tilt direction (`ascension`): the bearing the stylus leans towards, in [0,1].
//
// A perfectly upright stylus leans nowhere, and atan2(0,0) is 0 -- which would read as a real
// bearing. Pin it to the 3-o'clock neutral instead: -pi/2, because sign(+0) is +1 for both axes.
// The map to [0,1] is an OFFSET, not a wrap: -pi and +pi are the same bearing but must land on
// opposite ends of the sensor's domain, so that a curve can distinguish them.
[[nodiscard]] double tiltDirection01(const StrokeInput& in) noexcept {
    const auto signPZ = [](double v) { return v >= 0.0 ? 1.0 : -1.0; };
    const double radians = (in.xTilt == 0.0 && in.yTilt == 0.0)
                               ? -signPZ(in.xTilt) * signPZ(in.yTilt) * (kPi * 0.5)
                               : std::atan2(-in.xTilt, in.yTilt);
    return clamp01(radians / kTwoPi + 0.5);
}

// The tilt elevation (`declination`): 1 when the stylus stands perpendicular to the tablet, 0 when
// it lies flat. Derived from the two tilt axes rather than reported directly.
[[nodiscard]] double tiltElevation01(const StrokeInput& in) noexcept {
    const double x = std::clamp(in.xTilt / kMaxTiltDegrees, -1.0, 1.0);
    const double y = std::clamp(in.yTilt / kMaxTiltDegrees, -1.0, 1.0);

    const double e = (std::fabs(x) > std::fabs(y)) ? std::sqrt(1.0 + y * y) : std::sqrt(1.0 + x * x);
    const double cosAlpha = std::sqrt(x * x + y * y) / e;
    const double alpha = std::acos(std::clamp(cosAlpha, -1.0, 1.0));

    return clamp01(alpha / (kPi * 0.5));
}

// A ramp that either saturates at 1 on reaching `length` or sawtooths back to 0.
//
// The parser clamps `length` to >= 1, but `Sensor` is an aggregate anyone may fill in by hand, and a
// zero here is a division by zero that returns NaN through fmod and infinity through the divide --
// straight into a curve LUT and then into a dab. Floor it where the division happens, not only where
// the file is read.
[[nodiscard]] double rampValue(double progress, const SensorRange& range) noexcept {
    const double length = std::max(1.0, static_cast<double>(range.length));
    const double v = range.periodic ? std::fmod(progress, length) : std::min(progress, length);
    return clamp01(v / length);
}

// Blend two angles along the SHORTER arc, in whatever unit `turn` is a full turn of. A pen crossing
// the +/-half-turn seam between two samples has turned a few degrees, not most of the way round, and
// a naive lerp would spin the dab backwards through every heading in between.
//
// The two endpoints are returned VERBATIM rather than derived: a dab that lands exactly on a sample
// has to read that sample's angle to the bit, and `a + d` is neither bit-exact nor -- at the seam
// itself, where +half and -half are the same bearing but opposite readings of the `rotation` sensor
// -- even the same number. Everything strictly between is canonicalized into [-half, +half).
[[nodiscard]] double lerpAngle(double a, double b, double t, double turn) noexcept {
    if (!(t > 0.0))
        return a;
    if (t >= 1.0)
        return b;
    const double half = 0.5 * turn;
    double d = std::fmod(b - a, turn);
    if (d > half)
        d -= turn;
    else if (d < -half)
        d += turn;
    return detail::wrapValue(a + d * t, -half, half);
}

} // namespace

StrokeSnapshot lerpSnapshot(const StrokeSnapshot& a, const StrokeSnapshot& b, double t) noexcept {
    const double u = std::clamp(t, 0.0, 1.0); // a dab is never off its own span
    const auto mix = [u](double x, double y) { return x + (y - x) * u; };

    StrokeSnapshot s;
    s.sample.pos = a.sample.pos + (b.sample.pos - a.sample.pos) * u;
    s.sample.pressure = mix(a.sample.pressure, b.sample.pressure);
    s.sample.xTilt = mix(a.sample.xTilt, b.sample.xTilt);
    s.sample.yTilt = mix(a.sample.yTilt, b.sample.yTilt);
    s.sample.rotation = lerpAngle(a.sample.rotation, b.sample.rotation, u, 360.0);
    s.sample.tangentialPressure = mix(a.sample.tangentialPressure, b.sample.tangentialPressure);
    // The clock only runs forward, and a stroke's samples are not guaranteed to (stroke_state's
    // extendTo holds the clock on a replayed event). Blending a backwards pair would hand the `time`
    // sensor a dab that happened before the span began, so a non-advancing pair simply holds.
    s.sample.timeUs = b.sample.timeUs > a.sample.timeUs
                          ? a.sample.timeUs + static_cast<std::uint64_t>(
                                                  static_cast<double>(b.sample.timeUs -
                                                                      a.sample.timeUs) *
                                                  u)
                          : a.sample.timeUs;

    s.distance = mix(a.distance, b.distance);
    s.elapsedMs = mix(a.elapsedMs, b.elapsedMs);
    s.speedPxPerMs = mix(a.speedPxPerMs, b.speedPxPerMs);
    s.drawingAngle = lerpAngle(a.drawingAngle, b.drawingAngle, u, kTwoPi);
    // A running max does not interpolate. `b.maxPressure` already folded in b's own pressure, which a
    // dab short of b has not reached yet -- so the high-water mark HERE is a's, raised by this dab's
    // own pressure, which is what the stroke would have recorded had it been sampled here. (At u == 1
    // that lands back on b.maxPressure.)
    s.maxPressure = std::max(a.maxPressure, s.sample.pressure);
    return s;
}

StrokeSnapshot StrokeState::snapshot() const noexcept {
    StrokeSnapshot s;
    s.sample = m_sample;
    s.distance = m_distance;
    s.elapsedMs = m_elapsedMs;
    s.speedPxPerMs = m_speed.pxPerMs();
    s.drawingAngle = m_angle;
    s.maxPressure = m_maxPressure;
    return s;
}

void StrokeState::rewindTo(const StrokeSnapshot& s) noexcept {
    m_sample = s.sample;
    m_distance = s.distance;
    m_elapsedMs = s.elapsedMs;
    m_speed.setValue(s.speedPxPerMs);
    m_angle = s.drawingAngle;
    m_maxPressure = s.maxPressure;
    // m_lastTimeUs, m_rngState, m_dabIndex, m_lockedAngle and m_seed are STROKE-scoped and stay put
    // -- see StrokeSnapshot. A rewind re-reads the stroke; it does not rewrite it.
}

void SpeedSmoother::extend(double stepPx, double dtMs) noexcept {
    // Time-constant EMA: alpha = 1 - exp(-dt/tau). Sampling rate drops out, so the smoothing is
    // the same 30 ms whether the samples arrive at 60 Hz or at the 200 Hz the XI2 ring delivers.
    if (dtMs > 0.0 && m_params.windowMs > 0.0) {
        const double instant = stepPx / dtMs; // px per ms
        const double alpha = -std::expm1(-dtMs / m_params.windowMs);
        m_ema += alpha * (instant - m_ema);
    }
}

double SpeedSmoother::normalized() const noexcept {
    if (!(m_params.maxSpeed > 0.0))
        return 0.0;
    return clamp01(m_ema / m_params.maxSpeed);
}

void StrokeState::begin(const StrokeInput& first, std::uint64_t seed) {
    m_sample = first;
    m_seed = seed;
    // Offset the stream from the seed so that strokeRandom() (which mixes the raw seed) and
    // nextRandom() cannot hand out the same number for the first dab of a stroke.
    m_rngState = seed ^ 0xA5A5A5A5DEADBEEFULL;

    m_distance = 0.0;
    m_elapsedMs = 0.0;
    m_speed.reset();
    m_angle = 0.0;
    m_maxPressure = first.pressure;
    m_lockedAngle.reset();
    m_lastTimeUs = first.timeUs;
    m_dabIndex = -1;
    m_active = true;
}

void StrokeState::extendTo(const StrokeInput& s) {
    if (!m_active) {
        begin(s, m_seed);
        return;
    }

    const common::Vec2 delta = s.pos - m_sample.pos;
    const double step = delta.length();

    // The stroke clock only ever runs forward. A sample stamped before the last one -- a replayed
    // event, a driver whose clock stepped -- contributes zero elapsed rather than a negative dt, and
    // does not move the clock back. Otherwise the `time` ramp would reverse mid-stroke and the speed
    // EMA would take a negative step. Holding `m_lastTimeUs` (rather than accumulating from a moved
    // clock) is what keeps the elapsed total right once the samples resume.
    const bool forward = s.timeUs > m_lastTimeUs;
    const double dtMs = forward ? static_cast<double>(s.timeUs - m_lastTimeUs) / 1000.0 : 0.0;

    m_distance += step;
    m_elapsedMs += dtMs;
    m_maxPressure = std::max(m_maxPressure, s.pressure);

    // The angle is only defined while the stroke moves. A repeated position -- which tablets and
    // Android emit constantly -- keeps the previous heading rather than snapping the dab to zero.
    if (step > 0.0)
        m_angle = std::atan2(delta.y, delta.x);

    m_speed.extend(step, dtMs);

    if (forward)
        m_lastTimeUs = s.timeUs;
    m_sample = s;
}

double StrokeState::speed() const noexcept { return m_speed.normalized(); }

double StrokeState::lockedDrawingAngle() {
    if (!m_lockedAngle)
        m_lockedAngle = m_angle;
    return *m_lockedAngle;
}

double StrokeState::nextRandom() noexcept { return toUnitInterval(splitmix64(m_rngState)); }

double StrokeState::strokeRandom(std::string_view key) const noexcept {
    std::uint64_t state = m_seed ^ hashKey(key);
    return toUnitInterval(splitmix64(state));
}

double sensorValue(const Sensor& sensor, StrokeState& state, std::string_view optionKey) {
    const StrokeInput& in = state.sample();

    switch (sensor.id) {
    case SensorId::Pressure:
        return clamp01(in.pressure);

    // The stroke's high-water pressure, not the current one: a "press once to commit" sensor.
    case SensorId::PressureIn:
        return clamp01(state.maxPressure());

    case SensorId::TangentialPressure:
        return clamp01(in.tangentialPressure);

    // The one absolute-rotation sensor. The half turn puts a stroke heading east at 0.5 rather than
    // at a discontinuity, and the offset is applied before the wrap so it cannot escape [0,1).
    case SensorId::DrawingAngle: {
        const double angle = sensor.fan.lockedAngleMode ? state.lockedDrawingAngle()
                                                        : state.drawingAngle();
        return wrapValue(0.5 + angle / kTwoPi + sensor.fan.angleOffset / 360.0, 0.0, 1.0);
    }

    // Tilt reads 1 upright and falls towards 0 as the stylus lies down, so a preset that maps tilt
    // to size does not invert when the pen is held straight.
    case SensorId::XTilt:
        return clamp01(1.0 - std::fabs(in.xTilt) / kMaxTiltDegrees);
    case SensorId::YTilt:
        return clamp01(1.0 - std::fabs(in.yTilt) / kMaxTiltDegrees);

    // Additive: a bearing is signed, so it spans [-1,1] and sums rather than scales. The user's
    // tilt-direction offset (docs/tablet.md §7) is applied to xTilt/yTilt at ingest by the policy
    // layer, not here -- the engine sees a stylus that is already oriented the way they hold it.
    case SensorId::Ascension:
        return scalingToAdditive(tiltDirection01(in));

    case SensorId::Declination:
        return tiltElevation01(in);

    // Additive. Barrel rotation is already signed about zero.
    case SensorId::Rotation:
        return std::clamp(in.rotation / 180.0, -1.0, 1.0);

    // Additive, and the only two sensors that are not a function of the stroke's geometry.
    case SensorId::Fuzzy:
        return scalingToAdditive(state.nextRandom());
    case SensorId::FuzzyStroke:
        return scalingToAdditive(state.strokeRandom(optionKey));

    case SensorId::Speed:
        return state.speed();

    // Three ramps over three different clocks: dabs, document px, milliseconds (docs/brushes.md §3.3).
    case SensorId::Fade:
        return rampValue(static_cast<double>(std::max(state.dabIndex(), 0)), sensor.range);
    case SensorId::Distance:
        return rampValue(state.distance(), sensor.range);
    case SensorId::Time:
        return rampValue(state.elapsedMs(), sensor.range);

    // Mosaic has no perspective grid, and adding one to satisfy a sensor no shipped preset uses
    // would be the tail wagging the dog. A preset that drives an option from `perspective` imports
    // and paints; that option simply reads a constant. The importer reports it (§6.4).
    case SensorId::Perspective:
        return 1.0;
    }

    return 1.0;
}

} // namespace mosaic::core::brush
