#pragma once

#include "core/brush/curve.hpp"
#include "core/brush/sensors.hpp"
#include "core/brush/stroke_state.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// One dynamic option of a brush preset -- Size, Opacity, Rotation, Scatter and the rest
// (docs/brushes.md §3.2) -- and the machinery that turns its active sensors into the single number
// a dab needs.
//
// The pipeline, per dab:
//     sensors -> raw values (stroke_state.hpp)
//             -> each through its response curve, in its CLASS's space
//             -> folded into three components: a multiplicative scaling, an additive sum, and an
//                absolute offset that overwrites
//             -> read out as a size-like or a rotation-like value
//
// The three classes are not interchangeable and only ONE of them is governed by `combineMode`; see
// docs/brushes.md §3.3 and SensorClass. Getting this wrong silently mis-renders the two most-used
// sensors in the default preset set.
//
// FLTK- and Vulkan-free, and free of any preset-file dependency: `readCurveOption` takes a lookup
// function, so Arc B's `.kpp` reader plugs in without this header knowing what XML is.
namespace mosaic::core::brush {

// How two or more active SCALING sensors fold together. `{X}curveMode`. It never touches the
// additive or absolute-rotation classes, and a lone scaling sensor is taken verbatim -- so on
// almost every real preset this is inert.
enum class CombineMode : std::uint8_t {
    Multiply = 0, // the default
    Add = 1,
    Max = 2,
    Min = 3,
    Difference = 4,
};

// Anything outside 0..4 is the default. A preset from a newer version must not paint nothing.
[[nodiscard]] CombineMode combineModeFromInt(int mode) noexcept;

struct CurveOptionData;

// An option whose value MOVES with the stroke: its sensors are live (`{X}UseCurve` gates the
// sensors themselves, not merely their curves -- §3.2) and it has sensors to gate. A static one
// collapses to its constant strength, which a consumer can always honour whatever the option
// means. ⚠ ONE COPY: the importer's honesty contract (io/brush/mapper.cpp -- does a live option
// cost fidelity?) and the engine's dynamic-opacity gate (brush_engine.cpp -- does the wash
// accumulation take the per-dab path?) BOTH read this; two spellings of "dynamic" that could
// disagree would let a preset import as honoured and paint static, or the reverse.
[[nodiscard]] bool optionIsDynamic(const CurveOptionData& d) noexcept;

// The parsed form of one option's seven properties, plus the strength range its consumer imposes.
struct CurveOptionData {
    // The option's base name, e.g. "Size". Also keys the per-stroke `fuzzystroke` draw, which is why
    // two options driven by the same sensor scatter differently.
    std::string name;

    // `Pressure{X}` gates the whole option -- the key name is historical and has nothing to do with
    // the pressure sensor. Options that are always on (Opacity, Flow) are not checkable and read as
    // checked regardless.
    bool checkable = true;
    bool checked = false;

    // `{X}UseCurve`. False disables the SENSORS, not merely their curves: the option collapses to
    // its constant strength. That is what the reference implementation does, and it is not what the
    // key name suggests.
    bool useCurve = true;

    CombineMode combineMode = CombineMode::Multiply;

    double strength = 1.0;    // `{X}Value`
    double strengthMin = 0.0; // the consumer's range, not a preset property
    double strengthMax = 1.0;

    bool useSameCurve = true; // `{X}UseSameCurve`: every sensor answers to `commonCurve`
    Curve commonCurve;        // `{X}commonCurve`
    SensorList sensors;       // `{X}Sensor`
};

// The three components a dab consumes. They combine differently because they mean different things:
// a scale multiplies, an offset adds, an absolute angle replaces.
struct ValueComponents {
    double constant = 1.0; // the option's strength, or 1 when the caller asked to ignore it
    double scaling = 1.0;
    double additive = 0.0;
    double absoluteOffset = 0.0;
    bool hasScaling = false;
    bool hasAdditive = false;
    bool hasAbsoluteOffset = false;
    double minSizeLike = 0.0;
    double maxSizeLike = 1.0;

    // For options that scale a magnitude: size, opacity, flow, softness, scatter.
    [[nodiscard]] double sizeLikeValue() const noexcept;

    // For options that are an angle: rotation, and the mirror/ratio family that reads as one.
    // `normalizedBaseAngle` is the canvas rotation in [0,1]; `absoluteAxesFlipped` is true when the
    // document's coordinate system is mirrored on exactly one axis, which reverses the sense of an
    // absolute angle. Returns a value in [-1,1).
    [[nodiscard]] double rotationLikeValue(double normalizedBaseAngle, bool absoluteAxesFlipped,
                                           double scalingPartCoeff,
                                           bool disableScalingPart) const noexcept;
};

class CurveOption {
public:
    explicit CurveOption(CurveOptionData data);

    // Evaluate against the stroke. Non-const `state` because `fuzzy` advances the random stream and
    // `lockedAngleMode` latches -- evaluating an option is not a pure read of the stroke.
    //
    // `useStrength` false drops the constant to 1, for the callers that apply strength themselves.
    [[nodiscard]] ValueComponents compute(StrokeState& state, bool useStrength = true) const;

    [[nodiscard]] double sizeLikeValue(StrokeState& state, bool useStrength = true) const;
    [[nodiscard]] double rotationLikeValue(StrokeState& state, double normalizedBaseAngle,
                                           bool absoluteAxesFlipped, double scalingPartCoeff,
                                           bool disableScalingPart) const;

    // True when the option is on: either it is not checkable, or its `Pressure{X}` gate is set.
    [[nodiscard]] bool isChecked() const noexcept;

    // True when a fuzzy sensor is active, so the caller knows the option is not a pure function of
    // the stroke's geometry (the reticle cannot preview it; a cache cannot key on position alone).
    [[nodiscard]] bool isRandom() const noexcept;

    [[nodiscard]] const CurveOptionData& data() const noexcept { return m_data; }

private:
    // One active sensor plus the LUT of the curve that actually applies to it, resolved once at
    // construction. An identity curve resolves to no LUT at all: the overwhelmingly common case,
    // and the class-space round trip through an identity curve is exactly the identity anyway.
    struct Entry {
        Sensor sensor;
        SensorClass klass = SensorClass::Scaling;
        std::vector<float> lut; // empty == identity
    };

    [[nodiscard]] double evaluate(const Entry& e, StrokeState& state) const;

    CurveOptionData m_data;
    std::vector<Entry> m_entries;
};

// ---------------------------------------------------------------------------------------------
// Reading an option out of a preset's flat property table.

// The seven properties one option occupies, for base name `X`. `Pressure{X}` is the odd one: the
// prefix goes in front, everything else is a suffix.
struct CurveOptionKeys {
    std::string enabled;      // "Pressure" + X
    std::string sensor;       // X + "Sensor"
    std::string value;        // X + "Value"
    std::string useCurve;     // X + "UseCurve"
    std::string useSameCurve; // X + "UseSameCurve"
    std::string curveMode;    // X + "curveMode"
    std::string commonCurve;  // X + "commonCurve"

    // The pre-2.x pair, still present in shipped files: a single curve shared by every sensor.
    std::string legacyCustom; // "Custom" + X
    std::string legacyCurve;  // "Curve" + X

    [[nodiscard]] static CurveOptionKeys forBase(std::string_view base);
};

// A preset's `<param name=...>` table, as a lookup. Nullopt for an absent key -- which is NOT the
// same as an empty value, since the defaults differ.
using PropertyLookup = std::function<std::optional<std::string>(std::string_view key)>;

// Parse one option out of `props`. Every property is optional and every default is the one the
// reference reader applies, including the two that surprise: an absent `{X}Sensor` still yields a
// pressure sensor (sensors.hpp), and `{X}UseCurve` gates the sensors rather than the curves.
//
// `checkable` and the strength range come from the option's consumer, not from the file: Opacity and
// Flow are always on, and Size's strength means something different from Rotation's.
[[nodiscard]] CurveOptionData readCurveOption(std::string_view base, const PropertyLookup& props,
                                              bool checkable = true, double strengthMin = 0.0,
                                              double strengthMax = 1.0);

} // namespace mosaic::core::brush
