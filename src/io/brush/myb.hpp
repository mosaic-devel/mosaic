#pragma once

#include "io/brush/preset.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The MyPaint `.myb` brush reader (docs/brushes.md §6.7): the only other engine-carrying format,
// and a dab stamper -- its substance is a sensor→curve model over dab radius/opacity/hardness/
// offsets, i.e. our CurveOption machinery. Importing one is a sensor-name remapping table plus a
// handful of unit conversions, not a new engine. Facts verified against the producer's own
// definitions (libmypaint: brushsettings.json, mypaint-brush.c, mypaint-mapping.c,
// mypaint-tiled-surface.c) and the seven shipped `(mypaint).myb` presets:
//
//   * A setting's live value is `base_value + Σ_inputs piecewise_linear(input)`, endpoint-clamped,
//     0 or 2..64 points per input. All-corner control points make our Curve piecewise linear, so
//     the interpolation itself imports EXACTLY.
//   * The dab falloff over the normalized SQUARED radius is piecewise linear through
//     (0,1) → (hardness,hardness) → (1,0) -- precisely the domain and meaning of our Soft mask
//     generator's softness_curve. The tip profile imports EXACTLY (a corner at the knee).
//   * radius_logarithmic is ln(radius_px); the actual radius is clamped to [0.2, 1000] px.
//     Pressure/speed dynamics ADD in log space, i.e. they MULTIPLY the radius -- which decomposes
//     per-sensor into our Size option's multiplicative fold exactly; only the exp() of each
//     sensor's piecewise-linear curve needs sampling (subdivided, so it is the one conversion that
//     is Approximated rather than exact).
//   * dabs_per_actual_radius D means one dab per radius/D: spacing = 1/(2D). The MyPaint default
//     D=2 lands exactly on the 0.25 default spacing. dabs_per_basic_radius folds into the same sum
//     (exact while the radius is static); dabs_per_second imports as the airbrush rate.
//   * opaque × opaque_multiply is clamped to [0,1] at the dab; opaque_multiply is the
//     "pressure gates opacity" mapping (base 0 + a pressure curve in every shipped file). The
//     product of the two pressure mappings composes into ONE per-dab-opacity (our Flow) curve,
//     sampled at the union of their knots (+midpoints where the product is quadratic between
//     knots; knots are hugged so a duplicate-x step survives). When a file does not name an
//     opaque_multiply pressure input at all, the consumer's defaults-then-file load order leaves
//     libmypaint's default (0,0)→(1,1) pressure ramp live (from_string overwrites only the inputs
//     the JSON names), so absence imports as that ramp -- never as an invisible constant 0.
//   * opaque_linearize (default 0.9) pre-corrects dab-overlap accumulation; our engine has no such
//     correction, so a nonzero value is a provenance note, never silently ignored.
//   * MyPaint dabs composite per dab with no stroke-level opacity cap: PaintMode::Buildup.
//
// Input remap (MyPaint input id → our sensor, x normalization):
//   pressure → Pressure (exact) · random → Fuzzy (exact) · tilt_declination → Declination (deg/90,
//   same polarity) · tilt_ascension → Ascension (deg/360 + 0.5) · speed1/speed2 → Speed (x/4 --
//   MyPaint's calibrated log scale vs our normalized EMA: Approximated) · stroke → Fade
//   (Approximated) · direction → DrawingAngle (via the Rotation option; Approximated).
//   Anything else (custom, gridmap_*, viewzoom, brush_radius, barrel_rotation, attack_angle,
//   direction_angle, tilt_declinationx/y) is reported per use through PresetProvenance.
//
// Only version-3 JSON is supported -- the version-2 text format predates every file in the shipped
// corpus and the producer's own library reads only JSON; a text-format file is rejected with a
// reason that says so. The input is a third-party file: parsing is total, capped, and anything
// dropped is a provenance note, never silent (§4.1: fidelity honesty only -- imported content is
// never inspected beyond what loading requires, never reported anywhere but the preset's badge).
namespace mosaic::io::brush {

inline constexpr std::size_t kMaxMybBytes = 4u << 20; // shipped files are ~3-6 KB
inline constexpr int kMaxMybInputsPerSetting = 32;    // libmypaint defines 18 inputs
inline constexpr int kMaxMybPointsPerInput = 64;      // libmypaint's own hard cap

// Parse a `.myb` held in memory. `name` becomes the preset's name (the format carries none; the
// producer names presets by filename). Nullopt + reason when the JSON, the version or the
// structure is unusable; setting-level loss lands in the provenance instead.
[[nodiscard]] std::optional<BrushPreset> readMyb(const std::uint8_t* data, std::size_t size,
                                                 std::string_view name,
                                                 std::string* error = nullptr);

} // namespace mosaic::io::brush
