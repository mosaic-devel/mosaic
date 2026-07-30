#include "core/brush/curve_option.hpp"

#include "core/brush/parse_util.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core::brush {

namespace {

// The curve is sampled once per option into a LUT and interpolated linearly, rather than evaluating
// the spline per dab per sensor. 256 entries is what the format's own producer uses, so an imported
// preset resolves through the same quantization it was authored against.
constexpr std::size_t kLutSize = 256;

[[nodiscard]] bool boolProperty(const PropertyLookup& props, const std::string& key, bool fallback) {
    const std::optional<std::string> v = props(key);
    bool out = false;
    return (v && detail::parseBool(*v, out)) ? out : fallback;
}

[[nodiscard]] double doubleProperty(const PropertyLookup& props, const std::string& key,
                                    double fallback) {
    const std::optional<std::string> v = props(key);
    double out = 0.0;
    return (v && detail::parseDouble(*v, out)) ? out : fallback;
}

[[nodiscard]] int intProperty(const PropertyLookup& props, const std::string& key, int fallback) {
    const std::optional<std::string> v = props(key);
    long long out = 0;
    if (!v || !detail::parseLongLong(*v, out))
        return fallback;
    return static_cast<int>(std::clamp<long long>(out, -1'000'000, 1'000'000));
}

} // namespace

CombineMode combineModeFromInt(int mode) noexcept {
    switch (mode) {
    case 1:
        return CombineMode::Add;
    case 2:
        return CombineMode::Max;
    case 3:
        return CombineMode::Min;
    case 4:
        return CombineMode::Difference;
    default:
        // 0, and anything a newer version invents. An unknown fold must not paint nothing.
        return CombineMode::Multiply;
    }
}

bool optionIsDynamic(const CurveOptionData& d) noexcept {
    return d.useCurve && !d.sensors.sensors.empty();
}

double ValueComponents::sizeLikeValue() const noexcept {
    // Every absent component contributes 1, because here they all multiply. Note that an absent
    // additive component is 1 and NOT additiveToScaling(0) == 0.5: "no additive sensor" means the
    // option is untouched, whereas "an additive sensor reading 0" means it is halved.
    const double offset = hasAbsoluteOffset ? absoluteOffset : 1.0;
    const double scalingPart = hasScaling ? scaling : 1.0;
    const double additivePart = hasAdditive ? additiveToScaling(additive) : 1.0;

    const double v = constant * offset * scalingPart * additivePart;
    if (!std::isfinite(v))
        return minSizeLike;
    return std::clamp(v, minSizeLike, maxSizeLike);
}

double ValueComponents::rotationLikeValue(double normalizedBaseAngle, bool absoluteAxesFlipped,
                                          double scalingPartCoeff,
                                          bool disableScalingPart) const noexcept {
    // With no absolute sensor the dab inherits the canvas's own rotation; with one, that sensor IS
    // the angle. A single-axis mirror reverses the sense of an absolute bearing, which is why the
    // flip is applied here and not to the sensor's reading.
    const double offset = !hasAbsoluteOffset ? normalizedBaseAngle
                          : absoluteAxesFlipped ? 0.5 - absoluteOffset
                                                : absoluteOffset;

    // A scaling sensor contributes to an ANGLE only after being re-centred on zero: a pressure of
    // 0.5 must mean "no rotation", not "half a turn".
    const double scalingPart =
        (hasScaling && !disableScalingPart) ? scalingToAdditive(scaling) : 0.0;
    const double additivePart = hasAdditive ? additive : 0.0;

    // Check the sum, not the wrap: wrapValue maps a non-finite input to its low bound, so guarding
    // its OUTPUT would be dead code that silently reported a half turn as if it were no rotation.
    const double raw = 2.0 * offset + constant * (scalingPartCoeff * scalingPart + additivePart);
    if (!std::isfinite(raw))
        return 0.0;
    return wrapValue(raw, -1.0, 1.0);
}

CurveOption::CurveOption(CurveOptionData data) : m_data(std::move(data)) {
    m_entries.reserve(m_data.sensors.sensors.size());
    for (const Sensor& s : m_data.sensors.sensors) {
        Entry e;
        e.sensor = s;
        e.klass = sensorClass(s.id);

        // `useSameCurve` overrides every sensor's own curve with the option's shared one. It is the
        // default, so a preset's per-sensor curves are usually decoration until the user unticks it.
        const Curve& curve = m_data.useSameCurve ? m_data.commonCurve : s.curve;
        if (!curve.isIdentity())
            e.lut = curve.toLut(kLutSize);

        m_entries.push_back(std::move(e));
    }
}

double CurveOption::evaluate(const Entry& e, StrokeState& state) const {
    const double raw = sensorValue(e.sensor, state, m_data.name);
    if (e.lut.empty())
        return raw; // identity curve; the class round trip below would be a no-op anyway

    // A curve's domain is [0,1]. The additive and absolute-rotation classes do not live there, so
    // each is carried into scaling space, mapped, and carried back. For absolute rotation the
    // carrier is a HALF TURN, so that the curve's ends meet where the angle wraps rather than at the
    // heading a stroke most often has.
    switch (e.klass) {
    case SensorClass::Additive: {
        const double mapped = evalLut(e.lut, additiveToScaling(raw));
        return scalingToAdditive(mapped);
    }
    case SensorClass::AbsoluteRotation: {
        const double mapped = evalLut(e.lut, wrapValue(raw + 0.5, 0.0, 1.0));
        return wrapValue(mapped + 0.5, 0.0, 1.0);
    }
    case SensorClass::Scaling:
        break;
    }
    return evalLut(e.lut, raw);
}

ValueComponents CurveOption::compute(StrokeState& state, bool useStrength) const {
    ValueComponents c;
    c.minSizeLike = m_data.strengthMin;
    c.maxSizeLike = m_data.strengthMax;

    // `useCurve` gates the SENSORS, not just their curves. With it off the option is a constant.
    if (m_data.useCurve) {
        std::vector<double> scalingValues;
        scalingValues.reserve(m_entries.size());

        for (const Entry& e : m_entries) {
            const double v = evaluate(e, state);
            switch (e.klass) {
            case SensorClass::Additive:
                c.additive += v; // several additive sensors SUM
                c.hasAdditive = true;
                break;
            case SensorClass::AbsoluteRotation:
                c.absoluteOffset = v; // ...whereas an absolute angle OVERWRITES: the last one wins
                c.hasAbsoluteOffset = true;
                break;
            case SensorClass::Scaling:
                scalingValues.push_back(v);
                c.hasScaling = true;
                break;
            }
        }

        // A lone scaling sensor is taken verbatim -- combineMode needs two things to combine, which
        // is why it is inert on almost every shipped preset.
        if (scalingValues.size() == 1) {
            c.scaling = scalingValues.front();
        } else if (scalingValues.size() > 1) {
            switch (m_data.combineMode) {
            case CombineMode::Add:
                c.scaling = 0.0;
                for (const double v : scalingValues)
                    c.scaling += v;
                break;
            case CombineMode::Max:
                c.scaling = *std::max_element(scalingValues.begin(), scalingValues.end());
                break;
            case CombineMode::Min:
                c.scaling = *std::min_element(scalingValues.begin(), scalingValues.end());
                break;
            case CombineMode::Difference: {
                const auto [lo, hi] = std::minmax_element(scalingValues.begin(), scalingValues.end());
                c.scaling = *hi - *lo;
                break;
            }
            case CombineMode::Multiply:
                c.scaling = 1.0;
                for (const double v : scalingValues)
                    c.scaling *= v;
                break;
            }
        }
    }

    if (useStrength)
        c.constant = m_data.strength;

    return c;
}

double CurveOption::sizeLikeValue(StrokeState& state, bool useStrength) const {
    return compute(state, useStrength).sizeLikeValue();
}

double CurveOption::rotationLikeValue(StrokeState& state, double normalizedBaseAngle,
                                      bool absoluteAxesFlipped, double scalingPartCoeff,
                                      bool disableScalingPart) const {
    return compute(state, true).rotationLikeValue(normalizedBaseAngle, absoluteAxesFlipped,
                                                  scalingPartCoeff, disableScalingPart);
}

bool CurveOption::isChecked() const noexcept { return !m_data.checkable || m_data.checked; }

bool CurveOption::isRandom() const noexcept {
    return std::any_of(m_entries.begin(), m_entries.end(), [](const Entry& e) {
        return e.sensor.id == SensorId::Fuzzy || e.sensor.id == SensorId::FuzzyStroke;
    });
}

CurveOptionKeys CurveOptionKeys::forBase(std::string_view base) {
    const std::string b(base);
    CurveOptionKeys k;
    k.enabled = "Pressure" + b; // prefix, not suffix -- the name is historical (docs/brushes.md §3.2)
    k.sensor = b + "Sensor";
    k.value = b + "Value";
    k.useCurve = b + "UseCurve";
    k.useSameCurve = b + "UseSameCurve";
    k.curveMode = b + "curveMode";
    k.commonCurve = b + "commonCurve";
    k.legacyCustom = "Custom" + b;
    k.legacyCurve = "Curve" + b;
    return k;
}

CurveOptionData readCurveOption(std::string_view base, const PropertyLookup& props, bool checkable,
                                double strengthMin, double strengthMax) {
    const CurveOptionKeys keys = CurveOptionKeys::forBase(base);

    CurveOptionData data;
    data.name = std::string(base);
    data.checkable = checkable;
    data.checked = boolProperty(props, keys.enabled, false);
    data.strengthMin = strengthMin;
    data.strengthMax = strengthMax;

    // An option's default strength is the TOP of its range, not 1 and not the middle: an option you
    // enable without touching should do the thing it is named after, at full effect.
    data.strength = doubleProperty(props, keys.value, strengthMax);
    data.useCurve = boolProperty(props, keys.useCurve, true);
    data.useSameCurve = boolProperty(props, keys.useSameCurve, true);
    data.combineMode = combineModeFromInt(intProperty(props, keys.curveMode, 0));

    const std::string sensorXml = props(keys.sensor).value_or(std::string{});
    data.sensors = SensorList::fromXml(sensorXml);

    // The shared curve seeds from the LAST sensor the XML defined -- a legacy of the days when an
    // option had exactly one curve, and the reason a multi-sensor preset's common curve looks
    // arbitrary until the user edits it. `sensors` is never empty: fromXml() floors to pressure.
    Curve seeded = data.sensors.sensors.back().curve;

    // Pre-2.x files put the single shared curve in `Curve{X}`, gated by `Custom{X}`, and wrote no
    // `<curve>` into the sensor XML at all. The presence of that element in the serialized form is
    // the discriminator the reference reader uses, so it is the one used here -- a parsed identity
    // curve is indistinguishable from an absent one, and shipped files write both.
    if (sensorXml.find("curve") == std::string::npos) {
        if (boolProperty(props, keys.legacyCustom, false)) {
            seeded = Curve::fromString(props(keys.legacyCurve).value_or(std::string(kIdentityCurve)));
            for (Sensor& s : data.sensors.sensors)
                s.curve = seeded;
        } else {
            seeded = Curve();
        }
    }

    if (!data.useSameCurve) {
        data.commonCurve = std::move(seeded);
    } else if (const std::optional<std::string> stored = props(keys.commonCurve)) {
        // Present-but-empty is a corrupt curve, and Curve::fromString degrades it to the identity --
        // which is what the reference reader also lands on.
        data.commonCurve = Curve::fromString(*stored);
    } else {
        // Absent: keep the seed as it is. Serializing it and re-parsing would be a spline rebuild
        // per option per preset for a byte-exact round trip we already know closes.
        data.commonCurve = std::move(seeded);
    }

    return data;
}

} // namespace mosaic::core::brush
