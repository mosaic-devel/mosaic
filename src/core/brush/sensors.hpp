#pragma once

#include "core/brush/curve.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The dynamics sensors (docs/brushes.md §3.3) -- the input side of every CurveOption. A sensor reads
// one channel of the stroke (pressure, speed, the drawing angle, a random draw...) and maps it
// through its response Curve; the option then combines the active sensors into the value that drives
// size, opacity, rotation and the rest.
//
// This header owns three things: the sixteen sensor identities and their WIRE names, the class that
// decides how a sensor's output enters the dab, and the parser/serializer for the `<params id=...>`
// XML fragment that a preset's `{X}Sensor` property holds.
//
// Traps this module exists to contain, each verified against a shipped preset:
//   - `<curve>` is OMITTED when the curve is the identity, so a bare `<params id="pressure"/>` means
//     "pressure, identity curve", not "no curve". (Older files write it out in full; both parse.)
//   - A sensor is active because it is PRESENT. There is no `enabled` attribute.
//   - Every shipped fragment begins `<!DOCTYPE params>`, not with its root element.
//   - Attribute order is not fixed -- `id` is the SECOND attribute in some shipped files -- so this
//     is parsed with a real XML parser (pugixml), not a scanner, and unknown attributes such as the
//     vestigial `rotationModeEnabled` are ignored rather than rejected.
//   - `time` spells its length attribute `duration`; `fade` and `distance` spell it `length`, and
//     `fade` alone defaults it to 1000 rather than 30.
//   - `ascension` and `declination` are the wire names of the sensors a UI calls "tilt direction" and
//     "tilt elevation". The enumerators below are named for the wire, so nothing can serialize the
//     UI name by accident; sensorName() is the only place either spelling is written down.
//   - An absent, empty or unresolvable `{X}Sensor` floors to pressure-with-identity-curve, NOT to an
//     option with no sensors.
//
// FLTK- and Vulkan-free, headless-tested. pugixml is an implementation detail of the .cpp and never
// appears in this header.
namespace mosaic::core::brush {

// The sixteen sensors, named for their serialized ids. `Ascension` is tilt direction (0 when the nib
// points at you, clockwise through ±180°); `Declination` is tilt elevation (90 when the stylus is
// perpendicular to the tablet, 0 when parallel).
enum class SensorId : std::uint8_t {
    Pressure,
    PressureIn,
    TangentialPressure,
    DrawingAngle,
    XTilt,
    YTilt,
    Ascension,
    Declination,
    Rotation,
    Fuzzy,       // a fresh random draw per dab
    FuzzyStroke, // one random draw held for the whole stroke
    Speed,
    Fade,
    Distance,
    Time,
    Perspective,
};

inline constexpr std::size_t kSensorCount = 16;

// How a sensor's curve output reaches the dab. The three classes are combined differently and are
// NOT interchangeable -- see docs/brushes.md §3.3, and the CurveOption that consumes them.
enum class SensorClass : std::uint8_t {
    // Output in [0,1]. Several active scaling sensors are folded together by `{X}curveMode`
    // (multiply/add/max/min/difference) into one multiplicative factor. A lone scaling sensor is
    // taken verbatim, which is why curveMode is inert on almost every real preset.
    Scaling,
    // Output in [-1,1]. Curve evaluation happens in scaling space (0.5*(1+x) in, -1+2y out).
    // Active additive sensors SUM into their own component and ignore curveMode.
    Additive,
    // Output in [0,1), an angle. Curve evaluation happens through a half-turn wrap on both sides.
    // The component is OVERWRITTEN rather than summed, and curveMode is ignored. Only DrawingAngle.
    AbsoluteRotation,
};

// The serialized id, e.g. "tangentialpressure". Never a UI label.
[[nodiscard]] std::string_view sensorName(SensorId id) noexcept;

// Inverse of sensorName(). Nullopt for anything else -- an id from a newer or foreign preset.
[[nodiscard]] std::optional<SensorId> sensorFromName(std::string_view name) noexcept;

[[nodiscard]] SensorClass sensorClass(SensorId id) noexcept;

// `periodic` + a length, carried by Fade, Distance and Time. When periodic, the sensor sawtooths
// back to 0 on reaching `length` instead of saturating at 1.
struct SensorRange {
    bool periodic = false;
    // The three sensors count in three different units, and each editor spells it out: Fade counts
    // DABS (unitless), Distance counts document PX, Time counts MILLISECONDS -- which is why Time's
    // default of 30 is a brief opening ramp rather than half a minute. Always >= 1: a zero length is
    // a division by zero in the sensor, and hostile input can supply one.
    int length = 30;
};

// Carried by DrawingAngle alone.
struct SensorFan {
    bool fanCornersEnabled = false;
    int fanCornersStep = 30;  // degrees. The editor offers 5..90; we accept 1..360 from a file.
    double angleOffset = 0.0; // degrees, finite, reduced into (-360, 360). The editor offers ±180.
    bool lockedAngleMode = false;
};

// True for Fade, Distance, Time -- the sensors that serialize `periodic` and a length.
[[nodiscard]] bool sensorHasRange(SensorId id) noexcept;

// The attribute a sensor spells its length with: "length" for Fade/Distance, "duration" for Time,
// "" for the thirteen that have none. The Time spelling is the trap.
[[nodiscard]] std::string_view sensorRangeAttribute(SensorId id) noexcept;

// The length used when the attribute is absent: 1000 for Fade, 30 for Distance and Time.
[[nodiscard]] int defaultRangeLength(SensorId id) noexcept;

struct Sensor {
    SensorId id = SensorId::Pressure;
    Curve curve;              // the identity when `<curve>` was omitted
    SensorRange range{};      // meaningful only when sensorHasRange(id)
    SensorFan fan{};          // meaningful only when id == DrawingAngle

    // `id` with its per-sensor attribute defaults applied (only `range.length` actually varies).
    [[nodiscard]] static Sensor withDefaults(SensorId id);
};

// The parsed value of one `{X}Sensor` preset property.
struct SensorList {
    // Active sensors, in the order the XML listed them, deduplicated by id (a repeated id keeps the
    // last, as the reference reader does). Never empty: see fromXml().
    std::vector<Sensor> sensors;

    // Sensor ids present in the XML that this build does not know. Recorded rather than silently
    // dropped so an importer can report the loss through PresetProvenance (docs/brushes.md §6.4).
    // Capped, because the input is a third-party file.
    std::vector<std::string> unknownIds;

    // Parses the `<params id=...>` fragment in either of its two shapes: a single sensor whose id
    // sits on the root, or `<params id="sensorslist">` wrapping `<ChildSensor id=...>` children.
    //
    // Total-function: malformed XML, an empty string, an unresolvable id and a list with no
    // resolvable child all yield the same floor as the reference reader -- a single Pressure sensor
    // with an identity curve -- because a preset with one bad option must still load and paint.
    [[nodiscard]] static SensorList fromXml(std::string_view xml);

    // The fragment this list would serialize to: the single-sensor shape when exactly one sensor is
    // active, the sensorslist shape otherwise, `<curve>` omitted where identity. Compact (no
    // indentation), so it round-trips fromXml() semantically but not byte-for-byte -- Mosaic's own
    // presets are JSON (§7) and nothing re-exports a .kpp today, so only the parse direction has a
    // fidelity requirement. Round-tripping is how the parser is tested.
    [[nodiscard]] std::string toXml() const;

    [[nodiscard]] const Sensor* find(SensorId id) const noexcept;
    [[nodiscard]] bool has(SensorId id) const noexcept { return find(id) != nullptr; }
};

} // namespace mosaic::core::brush
