#include "io/brush/myb.hpp"

#include "core/brush/curve.hpp"
#include "core/brush/curve_option.hpp"
#include "core/brush/mask_generator.hpp"
#include "core/brush/sensors.hpp"
#include "io/brush/mapper.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <nlohmann/json.hpp>
#include <utility>
#include <vector>

namespace mosaic::io::brush {
namespace {

namespace cb = mosaic::core::brush;
using json = nlohmann::json;

// ------------------------------------------------------------------------------------------------
// The producer's mapping model: value = base + Σ_inputs piecewise_linear(input), endpoint-clamped.

struct Pt {
    double x = 0.0;
    double y = 0.0;
};

struct InputMap {
    std::string id;
    std::vector<Pt> pts; // >= 2, sorted by x as serialized
};

struct Mapping {
    double base = 0.0;
    std::vector<InputMap> inputs;
};

// Endpoint-clamped piecewise-linear evaluation -- mypaint_mapping_calculate's semantics.
double evalPts(const std::vector<Pt>& p, double x) {
    if (p.empty())
        return 0.0;
    if (x <= p.front().x)
        return p.front().y;
    for (std::size_t i = 1; i < p.size(); ++i) {
        if (x <= p[i].x) {
            const double x0 = p[i - 1].x, x1 = p[i].x;
            if (x1 - x0 <= 0.0)
                return p[i].y;
            const double t = (x - x0) / (x1 - x0);
            return p[i - 1].y + t * (p[i].y - p[i - 1].y);
        }
    }
    return p.back().y;
}

// A piecewise-linear function's extrema sit on its knots.
double maxAtKnots(const std::vector<Pt>& p) {
    double m = p.empty() ? 0.0 : p.front().y;
    for (const Pt& q : p)
        m = std::max(m, q.y);
    return m;
}

// One setting's mapping out of the settings object; `def` is the producer's default base_value.
Mapping readMapping(const json& settings, const char* name, double def) {
    Mapping m;
    m.base = def;
    const auto it = settings.find(name);
    if (it == settings.end() || !it->is_object())
        return m;
    if (const auto b = it->find("base_value"); b != it->end() && b->is_number())
        m.base = b->get<double>();
    const auto in = it->find("inputs");
    if (in == it->end() || !in->is_object())
        return m;
    for (const auto& [key, val] : in->items()) {
        if (static_cast<int>(m.inputs.size()) >= kMaxMybInputsPerSetting)
            break;
        if (!val.is_array())
            continue;
        std::vector<Pt> pts;
        for (const json& p : val) {
            if (static_cast<int>(pts.size()) >= kMaxMybPointsPerInput)
                break;
            if (p.is_array() && p.size() >= 2 && p[0].is_number() && p[1].is_number())
                pts.push_back({p[0].get<double>(), p[1].get<double>()});
        }
        // The producer's own rule: 0 points = inactive, exactly 1 is not a mapping at all.
        if (pts.size() >= 2)
            m.inputs.push_back({key, std::move(pts)});
    }
    return m;
}

bool hasInputs(const Mapping& m) {
    return !m.inputs.empty();
}

// The ONE input mapping libmypaint's mypaint_brush_from_defaults installs: pressure (0,0)->(1,1)
// on opaque_multiply. The consumer loads a file over a defaults-initialized brush and
// update_brush_setting_from_json_object overwrites ONLY the inputs the JSON names -- so that ramp
// SURVIVES unless the file itself carries an opaque_multiply "pressure" key (an empty array is an
// explicit clear; readMapping already yields the constant for it). Absence therefore imports as
// the ramp, not as a constant 0 -- which would be an invisible brush the producer never draws.
bool declaresOpaqueMultiplyPressure(const json& settings) {
    const auto it = settings.find("opaque_multiply");
    if (it == settings.end() || !it->is_object())
        return false;
    const auto in = it->find("inputs");
    return in != it->end() && in->is_object() && in->contains("pressure");
}

// ------------------------------------------------------------------------------------------------
// The sensor-name remap table (the header's contract). xNorm = clamp01(x * scale + offset) maps
// the producer's input domain onto the [0,1] our sensor emits -- each verified against
// stroke_state.cpp's readout, not guessed.

struct InputRemap {
    std::string_view id;
    cb::SensorId sensor;
    double scale = 1.0;
    double offset = 0.0;
    bool exact = true; // false: the response scale differs -> provenance note + Approximated
};

constexpr InputRemap kInputRemaps[] = {
    {"pressure", cb::SensorId::Pressure, 1.0, 0.0, true},
    {"random", cb::SensorId::Fuzzy, 1.0, 0.0, true},
    // MyPaint speeds are a calibrated log scale with a soft range of 0..4; our Speed sensor is a
    // normalized EMA. x/4 preserves the curve's shape over the working range, not its calibration.
    {"speed1", cb::SensorId::Speed, 0.25, 0.0, false},
    {"speed2", cb::SensorId::Speed, 0.25, 0.0, false},
    // stroke: 0..1 over the stroke with optional periodic wrap; our Fade counts dabs. Shape only.
    {"stroke", cb::SensorId::Fade, 1.0, 0.0, false},
    // tilt_declination: 90 = perpendicular; ours: 1 = perpendicular. Same polarity.
    {"tilt_declination", cb::SensorId::Declination, 1.0 / 90.0, 0.0, true},
    // tilt_ascension: degrees in [-180,180]; ours: bearing/2pi + 0.5. The zero reference may
    // differ between tablets/drivers, so the remap is honest about being approximate.
    {"tilt_ascension", cb::SensorId::Ascension, 1.0 / 360.0, 0.5, false},
};

const InputRemap* findRemap(std::string_view id) {
    for (const InputRemap& r : kInputRemaps)
        if (r.id == id)
            return &r;
    return nullptr;
}

double clamp01(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

// ------------------------------------------------------------------------------------------------
// Curve builders. All-corner control points make our Curve piecewise linear -- the producer's
// interpolation exactly -- and duplicate-x knots (MyPaint step curves) are nudged apart because
// Curve drops exact duplicates.

cb::Curve curveFromSamples(std::vector<Pt> samples) {
    std::stable_sort(samples.begin(), samples.end(),
                     [](const Pt& a, const Pt& b) { return a.x < b.x; });
    std::vector<cb::CurvePoint> points;
    points.reserve(samples.size());
    double lastX = -1.0;
    for (const Pt& s : samples) {
        double x = clamp01(s.x);
        if (x <= lastX)
            x = lastX + 1e-6; // preserve a step as a near-vertical ramp
        if (x > 1.0)
            break;
        points.push_back({x, clamp01(s.y), /*corner=*/true});
        lastX = x;
        if (points.size() >= 64) // the producer's own per-mapping cap
            break;
    }
    return cb::Curve(std::move(points));
}

// The sensor whose curve is `pts` remapped through `remap`, with `transform` applied to each y.
template <typename F>
cb::Sensor remappedSensor(const InputRemap& remap, const std::vector<Pt>& pts, F&& transform) {
    std::vector<Pt> samples;
    samples.reserve(pts.size());
    for (const Pt& p : pts)
        samples.push_back({clamp01(p.x * remap.scale + remap.offset), transform(p.y)});
    cb::Sensor s = cb::Sensor::withDefaults(remap.sensor);
    s.curve = curveFromSamples(std::move(samples));
    return s;
}

// exp(f(x) - fmax) sampled fine enough that the chord error stays under ~0.15 % (f-space step
// <= 0.1: exp vs chord deviates ~0.13 % at the midpoint) -- below the dab pipeline's own
// quantization, which is why a pressure-driven radius still counts as Exact.
cb::Sensor expSensor(const InputRemap& remap, const std::vector<Pt>& pts, double fmax) {
    // Every knot is kept (the knots are exact); midpoint subdivision spends whatever of the
    // 64-point budget the knots leave, evenly, so a legal 64-knot mapping is never truncated.
    const int perSegment = pts.size() >= 2
        ? std::max(0, static_cast<int>((64 - pts.size()) / (pts.size() - 1)))
        : 0;
    std::vector<Pt> samples;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        samples.push_back({pts[i].x, pts[i].y});
        if (i + 1 < pts.size()) {
            const int wanted = std::min(
                16, static_cast<int>(std::ceil(std::fabs(pts[i + 1].y - pts[i].y) / 0.1)));
            const int steps = std::min(wanted, perSegment + 1);
            for (int s = 1; s < steps; ++s) {
                const double t = static_cast<double>(s) / steps;
                samples.push_back({pts[i].x + t * (pts[i + 1].x - pts[i].x),
                                   pts[i].y + t * (pts[i + 1].y - pts[i].y)});
            }
        }
    }
    for (Pt& s : samples) {
        s.x = clamp01(s.x * remap.scale + remap.offset);
        s.y = std::exp(s.y - fmax);
    }
    cb::Sensor sensor = cb::Sensor::withDefaults(remap.sensor);
    sensor.curve = curveFromSamples(std::move(samples));
    return sensor;
}

// ------------------------------------------------------------------------------------------------
// Assembly helpers.

cb::CurveOptionData* option(BrushPreset& preset, std::string_view base) {
    for (cb::CurveOptionData& o : preset.options)
        if (o.name == base)
            return &o;
    return nullptr;
}

// Route every input of `m` through the remap table: usable ones are handed to `use`, the rest
// become provenance notes. Returns the usable (remap, points) pairs.
std::vector<std::pair<const InputRemap*, const std::vector<Pt>*>>
routeInputs(BrushPreset& preset, const Mapping& m, std::string_view optionLabel) {
    std::vector<std::pair<const InputRemap*, const std::vector<Pt>*>> used;
    for (const InputMap& in : m.inputs) {
        const InputRemap* remap = findRemap(in.id);
        if (remap == nullptr) {
            addDroppedOption(preset, std::string(optionLabel) + ": input '" + in.id + "'");
            degradeFidelity(preset, PresetFidelity::Approximated);
            continue;
        }
        if (!remap->exact)
            degradeFidelity(preset, PresetFidelity::Approximated);
        used.push_back({remap, &in.pts});
    }
    return used;
}

} // namespace

std::optional<BrushPreset> readMyb(const std::uint8_t* data, std::size_t size,
                                   std::string_view name, std::string* error) {
    const auto fail = [&](std::string reason) -> std::optional<BrushPreset> {
        if (error != nullptr)
            *error = std::move(reason);
        return std::nullopt;
    };
    if (data == nullptr || size == 0)
        return fail("empty file");
    if (size > kMaxMybBytes)
        return fail("file exceeds the .myb size cap");

    // The version-2 text format opens with comment lines; say so instead of "bad JSON".
    std::size_t first = 0;
    while (first < size && (data[first] == ' ' || data[first] == '\t' || data[first] == '\r'
                            || data[first] == '\n'))
        ++first;
    if (first < size && data[first] == '#')
        return fail("MyPaint text-format (version 2) brush; only version 3 JSON is supported");

    const json doc = json::parse(data, data + size, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object())
        return fail("not a JSON object");
    const auto version = doc.find("version");
    if (version == doc.end() || !version->is_number() || version->get<double>() != 3.0)
        return fail("unsupported .myb version (only version 3 is supported)");
    const auto settingsIt = doc.find("settings");
    if (settingsIt == doc.end() || !settingsIt->is_object())
        return fail("no settings object");
    const json& settings = *settingsIt;

    BrushPreset preset;
    preset.name = std::string(name);
    preset.provenance.sourceFormat = "myb";
    preset.provenance.sourcePaintop = "mypaint";
    preset.provenance.fidelity = PresetFidelity::Exact;

    // The same 18-family option vector every importer produces, at the same defaults an absent
    // kpp property yields; the families .myb maps are overwritten below.
    const auto emptyLookup = [](std::string_view) { return std::optional<std::string>{}; };
    for (const OptionSpec& spec : pixelBrushOptionSpecs())
        preset.options.push_back(
            cb::readCurveOption(spec.base, emptyLookup, spec.checkable, spec.min, spec.max));

    // Defaults are the producer's (brushsettings.json).
    const Mapping radius = readMapping(settings, "radius_logarithmic", 2.0);
    const Mapping opaque = readMapping(settings, "opaque", 1.0);
    const Mapping opaqueMul = [&] {
        Mapping m = readMapping(settings, "opaque_multiply", 0.0);
        if (!declaresOpaqueMultiplyPressure(settings))
            m.inputs.push_back({"pressure", {{0.0, 0.0}, {1.0, 1.0}}});
        return m;
    }();
    const Mapping linearize = readMapping(settings, "opaque_linearize", 0.9);
    const Mapping hardness = readMapping(settings, "hardness", 0.8);
    const Mapping scatter = readMapping(settings, "offset_by_random", 0.0);
    const Mapping dabRatio = readMapping(settings, "elliptical_dab_ratio", 1.0);
    const Mapping dabAngle = readMapping(settings, "elliptical_dab_angle", 90.0);
    const Mapping dabsActual = readMapping(settings, "dabs_per_actual_radius", 2.0);
    const Mapping dabsBasic = readMapping(settings, "dabs_per_basic_radius", 0.0);
    const Mapping dabsPerSecond = readMapping(settings, "dabs_per_second", 0.0);
    const Mapping eraser = readMapping(settings, "eraser", 0.0);
    const Mapping antiAliasing = readMapping(settings, "anti_aliasing", 1.0);

    // ---- Size: dynamics ADD in log space = multiply the radius, which decomposes per-sensor
    // into the Size option's multiplicative fold. The static part (base + each input's maximum)
    // is folded into the tip diameter so every per-sensor curve lands in (0,1].
    double logDiameter = radius.base;
    const auto sizeInputs = routeInputs(preset, radius, "Size");
    for (const auto& [remap, pts] : sizeInputs)
        logDiameter += maxAtKnots(*pts);
    const double diameter =
        2.0 * std::clamp(std::exp(logDiameter), 0.2, 1000.0); // the producer's radius clamps
    if (!sizeInputs.empty()) {
        cb::CurveOptionData& size = *option(preset, "Size");
        size.checked = true;
        size.useCurve = true;
        size.useSameCurve = false;
        size.strength = 1.0;
        size.sensors.sensors.clear();
        for (const auto& [remap, pts] : sizeInputs)
            size.sensors.sensors.push_back(expSensor(*remap, *pts, maxAtKnots(*pts)));
    }

    // ---- Flow: MyPaint's opaque x opaque_multiply is the PER-DAB alpha (dabs accumulate with
    // no stroke-level cap), which is our Flow under Buildup -- not Opacity, the stroke cap.
    // Opacity therefore imports as a constant 1. Foreign inputs on either factor are dropped
    // per-input; the pressure parts compose into one curve, sampled at the union of their knots
    // plus midpoints (the product is quadratic between knots).
    {
        const std::vector<Pt>* fo = nullptr;
        const std::vector<Pt>* fm = nullptr;
        for (const Mapping* m : {&opaque, &opaqueMul}) {
            for (const InputMap& in : m->inputs) {
                if (in.id == "pressure") {
                    (m == &opaque ? fo : fm) = &in.pts;
                } else {
                    addDroppedOption(preset, "Flow: input '" + in.id + "' on "
                                                 + (m == &opaque ? "opaque" : "opaque_multiply"));
                    degradeFidelity(preset, PresetFidelity::Approximated);
                }
            }
        }
        cb::CurveOptionData& flow = *option(preset, "Flow");
        cb::CurveOptionData& opacity = *option(preset, "Opacity");
        opacity.useCurve = false;
        opacity.strength = 1.0;
        const auto dabAlpha = [&](double p) {
            const double o = std::max(0.0, opaque.base + (fo != nullptr ? evalPts(*fo, p) : 0.0));
            return clamp01(o * (opaqueMul.base + (fm != nullptr ? evalPts(*fm, p) : 0.0)));
        };
        if (fo == nullptr && fm == nullptr) {
            flow.useCurve = false;
            flow.strength = dabAlpha(0.0);
            if (flow.strength == 0.0)
                addDroppedOption(preset, "Flow: zero (opaque_multiply is 0 with no mapping)");
        } else {
            std::vector<double> xs{0.0, 1.0};
            for (const std::vector<Pt>* f : {fo, fm})
                if (f != nullptr)
                    for (const Pt& p : *f) {
                        // A duplicate-x knot pair is a STEP (Ink_pen's 0.015 pressure gate); the
                        // union would collapse it to one x. Hug every knot so a step in either
                        // factor survives the resampling as a near-vertical ramp.
                        const double x = clamp01(p.x);
                        xs.push_back(clamp01(x - 1e-6));
                        xs.push_back(x);
                        xs.push_back(clamp01(x + 1e-6));
                    }
            std::sort(xs.begin(), xs.end());
            xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
            std::vector<Pt> samples;
            for (std::size_t i = 0; i < xs.size(); ++i) {
                samples.push_back({xs[i], dabAlpha(xs[i])});
                if (i + 1 < xs.size())
                    samples.push_back(
                        {0.5 * (xs[i] + xs[i + 1]), dabAlpha(0.5 * (xs[i] + xs[i + 1]))});
            }
            flow.useCurve = true;
            flow.strength = 1.0;
            flow.useSameCurve = false;
            flow.sensors.sensors.clear();
            cb::Sensor s = cb::Sensor::withDefaults(cb::SensorId::Pressure);
            s.curve = curveFromSamples(std::move(samples));
            flow.sensors.sensors.push_back(std::move(s));
        }
    }
    if (linearize.base > 0.05 || hasInputs(linearize)) {
        addDroppedOption(preset, "Flow: dab-overlap linearization");
        degradeFidelity(preset, PresetFidelity::Approximated);
    }

    // ---- Scatter: gaussian offset in base-radius units vs the engine's scatter span. The shape
    // (linear in the mapped value) imports exactly; the unit semantics are approximate.
    {
        double top = scatter.base;
        for (const InputMap& in : scatter.inputs)
            top = std::max(top, scatter.base + maxAtKnots(in.pts));
        if (top > 0.0) {
            const auto used = routeInputs(preset, scatter, "Scatter");
            cb::CurveOptionData& sc = *option(preset, "Scatter");
            sc.checked = true;
            sc.strength = std::min(top, 5.0);
            addDroppedOption(preset, "Scatter: gaussian offset approximated");
            degradeFidelity(preset, PresetFidelity::Approximated);
            if (used.empty()) {
                sc.useCurve = false;
            } else {
                sc.useCurve = true;
                sc.useSameCurve = false;
                sc.sensors.sensors.clear();
                for (const auto& [remap, pts] : used)
                    sc.sensors.sensors.push_back(remappedSensor(
                        *remap, *pts, [&](double y) { return (scatter.base + y) / top; }));
            }
        }
    }

    // ---- The tip: a Soft-falloff circle whose softness_curve IS the producer's dab profile over
    // the normalized squared radius: (0,1) -> (h,h) -> (1,0), a corner at the knee. h is clamped
    // to [0.02, 0.98]: 0 draws nothing upstream (the preset must stay usable), and the 3-point
    // profile needs the knee strictly inside.
    double h = std::clamp(hardness.base, 0.0, 1.0);
    if (hasInputs(hardness)) {
        addDroppedOption(preset, "Hardness: dynamic");
        degradeFidelity(preset, PresetFidelity::Approximated);
    }
    if (h < 0.02) {
        addDroppedOption(preset, "Hardness: zero (draws nothing upstream); floored");
        degradeFidelity(preset, PresetFidelity::Approximated);
        h = 0.02;
    }
    h = std::min(h, 0.98);

    preset.tip.kind = TipXml::Kind::Auto;
    preset.tip.type = "mypaint";
    cb::MaskGeneratorParams& g = preset.tip.autoTip.generator;
    g.shape = cb::MaskShape::Circle;
    g.falloff = cb::MaskFalloff::Soft;
    g.diameter = diameter;
    g.ratio = 1.0 / std::clamp(dabRatio.base, 1.0, 10.0);
    g.antialiasEdges = antiAliasing.base > 0.5;
    g.softnessCurve = cb::Curve({{0.0, 1.0, false}, {h, h, true}, {1.0, 0.0, false}});
    if (hasInputs(dabRatio)) {
        addDroppedOption(preset, "Ratio: dynamic aspect");
        degradeFidelity(preset, PresetFidelity::Approximated);
    }
    if (hasInputs(antiAliasing)) {
        addDroppedOption(preset, "Anti-aliasing: dynamic");
        degradeFidelity(preset, PresetFidelity::Approximated);
    }

    // ---- Angle: static tilts the tip; direction-driven becomes the Rotation option on the
    // DrawingAngle sensor (the producer folds direction mod 180). Skipped when the dab is round
    // and stays round -- the angle then has no visible effect.
    const bool dabIsRound = g.ratio >= 0.999 && !hasInputs(dabRatio);
    const InputMap* direction = nullptr;
    if (!dabIsRound) { // angle inputs on a dab that stays round have no visible effect upstream
        for (const InputMap& in : dabAngle.inputs) {
            if (in.id == "direction")
                direction = &in;
            else {
                addDroppedOption(preset, "Rotation: input '" + in.id + "'");
                degradeFidelity(preset, PresetFidelity::Approximated);
            }
        }
    }
    if (direction != nullptr) {
        cb::CurveOptionData& rot = *option(preset, "Rotation");
        rot.checked = true;
        rot.useCurve = true;
        rot.useSameCurve = false;
        rot.strength = 1.0;
        // Angle as a fraction of the full turn, sampled at each knot on both half-turns (the
        // producer's direction input folds mod 180). A wrap through 0/360 between neighbouring
        // samples gets a hugging pair so the interpolation doesn't sweep the long way round.
        const auto angleAt = [&](double dirDeg) {
            const double deg = dabAngle.base + evalPts(direction->pts, dirDeg);
            return deg / 360.0 - std::floor(deg / 360.0);
        };
        std::vector<Pt> samples;
        for (double half : {0.0, 0.5})
            for (const Pt& p : direction->pts)
                samples.push_back({clamp01(p.x / 360.0 + half), angleAt(p.x)});
        std::sort(samples.begin(), samples.end(),
                  [](const Pt& a, const Pt& b) { return a.x < b.x; });
        std::vector<Pt> withWraps;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            if (i > 0 && std::fabs(samples[i].y - samples[i - 1].y) > 0.5) {
                const double xm = 0.5 * (samples[i - 1].x + samples[i].x);
                const bool up = samples[i].y > samples[i - 1].y;
                withWraps.push_back({xm, up ? 0.0 : 1.0});
                withWraps.push_back({xm, up ? 1.0 : 0.0});
            }
            withWraps.push_back(samples[i]);
        }
        cb::Sensor s = cb::Sensor::withDefaults(cb::SensorId::DrawingAngle);
        s.curve = curveFromSamples(std::move(withWraps));
        rot.sensors.sensors.clear();
        rot.sensors.sensors.push_back(std::move(s));
        addDroppedOption(preset, "Rotation: follows stroke direction (approximated)");
        degradeFidelity(preset, PresetFidelity::Approximated);
    } else if (!dabIsRound) {
        preset.tip.angle = dabAngle.base * (3.14159265358979323846 / 180.0);
    }

    // ---- Spacing: one dab per radius/D means spacing = 1/(2D). The two per-radius rates sum
    // exactly while the radius is static; with size dynamics the basic-radius part deviates.
    const double dabsPerRadius = std::max(0.0, dabsActual.base) + std::max(0.0, dabsBasic.base);
    if (hasInputs(dabsActual) || hasInputs(dabsBasic) || hasInputs(dabsPerSecond)) {
        addDroppedOption(preset, "Spacing: dynamic dab rate");
        degradeFidelity(preset, PresetFidelity::Approximated);
    }
    if (dabsPerRadius > 0.0) {
        preset.tip.spacing = std::clamp(1.0 / (2.0 * dabsPerRadius), 0.01, 10.0);
        if (dabsBasic.base > 0.0 && !sizeInputs.empty()) {
            addDroppedOption(preset, "Spacing: base-radius dab rate approximated");
            degradeFidelity(preset, PresetFidelity::Approximated);
        }
    } else {
        preset.tip.spacing = 0.25;
        if (dabsPerSecond.base <= 0.0)
            addDroppedOption(preset, "Spacing: no dab rate; default applied");
    }
    preset.tip.useAutoSpacing = false;

    if (dabsPerSecond.base > 0.0) {
        preset.airbrush.enabled = true;
        preset.airbrush.rate = dabsPerSecond.base;
        preset.airbrush.ignoreSpacing = false;
        addDroppedOption(preset, "Airbrush");
        degradeFidelity(preset, PresetFidelity::Approximated);
    }

    // ---- Mode: per-dab compositing with no stroke cap.
    preset.paintMode = cb::PaintMode::Buildup;
    preset.accumulator = cb::StrokeAccumulator::Uniform;
    preset.colorDynamicsActive = false;
    preset.blendMode = core::BlendMode::Normal;

    if (hasInputs(eraser)) {
        addDroppedOption(preset, "Eraser: dynamic");
        degradeFidelity(preset, PresetFidelity::Approximated);
    }
    if (eraser.base >= 0.5) {
        preset.eraserMode = true;
        if (eraser.base < 0.95) {
            addDroppedOption(preset, "Eraser: partial strength");
            degradeFidelity(preset, PresetFidelity::Approximated);
        }
    } else if (eraser.base > 0.05) {
        addDroppedOption(preset, "Eraser: partial strength");
        degradeFidelity(preset, PresetFidelity::Approximated);
    }

    // ---- Features the engine has no home for: reported when ACTIVE, silent when at the
    // producer's default. The list is the mark-affecting settings; tracking/smoothing settings
    // are input conditioning (the tablet layer's domain) and are not the brush's mark.
    struct Check {
        const char* setting;
        double def;
        const char* note;
    };
    static constexpr Check kChecks[] = {
        {"smudge", 0.0, "Smudge"},
        {"smudge_length", 0.5, "Smudge"},
        {"lock_alpha", 0.0, "Lock alpha"},
        {"colorize", 0.0, "Colorize"},
        {"snap_to_pixel", 0.0, "Snap to pixel"},
        {"radius_by_random", 0.0, "Size: random radius"},
        {"change_color_h", 0.0, "Color dynamics"},
        {"change_color_l", 0.0, "Color dynamics"},
        {"change_color_v", 0.0, "Color dynamics"},
        {"change_color_hsl_s", 0.0, "Color dynamics"},
        {"change_color_hsv_s", 0.0, "Color dynamics"},
        {"pressure_gain_log", 0.0, "Pressure gain"},
    };
    bool noted[std::size(kChecks)] = {};
    for (std::size_t i = 0; i < std::size(kChecks); ++i) {
        const Mapping m = readMapping(settings, kChecks[i].setting, kChecks[i].def);
        if (std::fabs(m.base - kChecks[i].def) > 1e-9 || hasInputs(m)) {
            // One note per label (the five change_color_* settings share one).
            bool dup = false;
            for (std::size_t j = 0; j < i; ++j)
                dup = dup || (noted[j] && std::string_view(kChecks[j].note) == kChecks[i].note);
            if (!dup)
                addDroppedOption(preset, kChecks[i].note);
            noted[i] = true;
            degradeFidelity(preset, PresetFidelity::Approximated);
        }
    }

    // Any OTHER setting carrying a dynamic mapping is behaviour we did not import; a static
    // unfamiliar base is engine feel, not a mark, and stays silent.
    static constexpr std::string_view kConsumed[] = {
        "radius_logarithmic", "opaque", "opaque_multiply", "opaque_linearize", "hardness",
        "offset_by_random", "elliptical_dab_ratio", "elliptical_dab_angle",
        "dabs_per_actual_radius", "dabs_per_basic_radius", "dabs_per_second", "eraser",
        "anti_aliasing",
    };
    int scanned = 0;
    for (const auto& [key, val] : settings.items()) {
        if (++scanned > 512)
            break;
        if (!val.is_object() || !val.contains("inputs") || !val["inputs"].is_object()
            || val["inputs"].empty())
            continue;
        bool consumed = std::any_of(std::begin(kConsumed), std::end(kConsumed),
                                    [&](std::string_view c) { return c == key; });
        for (const Check& c : kChecks)
            consumed = consumed || key == c.setting; // already noted above
        if (!consumed) {
            addDroppedOption(preset, "Dynamic setting '" + key + "'");
            degradeFidelity(preset, PresetFidelity::Approximated);
        }
    }

    return preset;
}

} // namespace mosaic::io::brush
