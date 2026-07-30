#include "io/brush/preset_json.hpp"

#include "core/blend_mode.hpp"
#include "io/brush/png_text.hpp"
#include "io/detail.hpp"
#include "io/io.hpp"

#include <cstring>
#include <nlohmann/json.hpp>
#include <utility>
#include <zlib.h>

namespace mosaic::io::brush {
namespace {

namespace cb = mosaic::core::brush;
using json = nlohmann::json;

// ------------------------------------------------------------------------------------------------
// Enum spellings. Each pair round-trips; parse returns nullopt for anything foreign, which the
// strict reader turns into a load failure (this is OUR format -- see the header).

const char* str(PresetFidelity f) {
    switch (f) {
    case PresetFidelity::Exact: return "exact";
    case PresetFidelity::Approximated: return "approximated";
    case PresetFidelity::Substituted: return "substituted";
    }
    return "exact";
}
std::optional<PresetFidelity> fidelityFrom(std::string_view s) {
    if (s == "exact")
        return PresetFidelity::Exact;
    if (s == "approximated")
        return PresetFidelity::Approximated;
    if (s == "substituted")
        return PresetFidelity::Substituted;
    return std::nullopt;
}

const char* str(TipXml::Kind k) {
    switch (k) {
    case TipXml::Kind::Auto: return "auto";
    case TipXml::Kind::Predefined: return "predefined";
    case TipXml::Kind::Unknown: return "unknown";
    }
    return "unknown";
}
std::optional<TipXml::Kind> kindFrom(std::string_view s) {
    if (s == "auto")
        return TipXml::Kind::Auto;
    if (s == "predefined")
        return TipXml::Kind::Predefined;
    if (s == "unknown")
        return TipXml::Kind::Unknown;
    return std::nullopt;
}

const char* str(cb::MaskShape s) {
    return s == cb::MaskShape::Rect ? "rect" : "circle";
}
std::optional<cb::MaskShape> shapeFrom(std::string_view s) {
    if (s == "circle")
        return cb::MaskShape::Circle;
    if (s == "rect")
        return cb::MaskShape::Rect;
    return std::nullopt;
}

const char* str(cb::MaskFalloff f) {
    switch (f) {
    case cb::MaskFalloff::Default: return "default";
    case cb::MaskFalloff::Soft: return "soft";
    case cb::MaskFalloff::Gauss: return "gauss";
    }
    return "default";
}
std::optional<cb::MaskFalloff> falloffFrom(std::string_view s) {
    if (s == "default")
        return cb::MaskFalloff::Default;
    if (s == "soft")
        return cb::MaskFalloff::Soft;
    if (s == "gauss")
        return cb::MaskFalloff::Gauss;
    return std::nullopt;
}

const char* str(TipApplicationRule r) {
    switch (r) {
    case TipApplicationRule::ForceLightness: return "forceLightness";
    case TipApplicationRule::Explicit: return "explicit";
    case TipApplicationRule::LegacyContentTest: return "legacyContentTest";
    }
    return "legacyContentTest";
}
std::optional<TipApplicationRule> ruleFrom(std::string_view s) {
    if (s == "forceLightness")
        return TipApplicationRule::ForceLightness;
    if (s == "explicit")
        return TipApplicationRule::Explicit;
    if (s == "legacyContentTest")
        return TipApplicationRule::LegacyContentTest;
    return std::nullopt;
}

const char* str(cb::TipApplication a) {
    switch (a) {
    case cb::TipApplication::AlphaMask: return "alphaMask";
    case cb::TipApplication::ImageStamp: return "imageStamp";
    case cb::TipApplication::LightnessMap: return "lightnessMap";
    case cb::TipApplication::GradientMap: return "gradientMap";
    }
    return "alphaMask";
}
std::optional<cb::TipApplication> applicationFrom(std::string_view s) {
    if (s == "alphaMask")
        return cb::TipApplication::AlphaMask;
    if (s == "imageStamp")
        return cb::TipApplication::ImageStamp;
    if (s == "lightnessMap")
        return cb::TipApplication::LightnessMap;
    if (s == "gradientMap")
        return cb::TipApplication::GradientMap;
    return std::nullopt;
}

const char* str(cb::PaintMode m) {
    return m == cb::PaintMode::Buildup ? "buildup" : "wash";
}
std::optional<cb::PaintMode> paintModeFrom(std::string_view s) {
    if (s == "wash")
        return cb::PaintMode::Wash;
    if (s == "buildup")
        return cb::PaintMode::Buildup;
    return std::nullopt;
}

const char* str(cb::StrokeAccumulator a) {
    return a == cb::StrokeAccumulator::Colored ? "colored" : "uniform";
}
std::optional<cb::StrokeAccumulator> accumulatorFrom(std::string_view s) {
    if (s == "uniform")
        return cb::StrokeAccumulator::Uniform;
    if (s == "colored")
        return cb::StrokeAccumulator::Colored;
    return std::nullopt;
}

const char* str(cb::MaskingOp op) {
    switch (op) {
    case cb::MaskingOp::Multiply: return "multiply";
    case cb::MaskingOp::Subtract: return "subtract";
    case cb::MaskingOp::LinearDodge: return "linearDodge";
    }
    return "multiply";
}
std::optional<cb::MaskingOp> maskingOpFrom(std::string_view s) {
    if (s == "multiply")
        return cb::MaskingOp::Multiply;
    if (s == "subtract")
        return cb::MaskingOp::Subtract;
    if (s == "linearDodge")
        return cb::MaskingOp::LinearDodge;
    return std::nullopt;
}

std::optional<core::BlendMode> blendModeFrom(std::string_view s) {
    for (int i = 0; i < core::kBlendModeCount; ++i)
        if (core::blendModeName(static_cast<core::BlendMode>(i)) == s)
            return static_cast<core::BlendMode>(i);
    return std::nullopt;
}

// ------------------------------------------------------------------------------------------------
// Serialization. Every field is always written, so the reader can always require it.

json toJson(const cb::Sensor& s) {
    return {
        {"id", std::string(cb::sensorName(s.id))},
        {"curve", s.curve.toString()},
        {"periodic", s.range.periodic},
        {"length", s.range.length},
        {"fanCornersEnabled", s.fan.fanCornersEnabled},
        {"fanCornersStep", s.fan.fanCornersStep},
        {"angleOffset", s.fan.angleOffset},
        {"lockedAngleMode", s.fan.lockedAngleMode},
    };
}

json toJson(const cb::SensorList& list) {
    json sensors = json::array();
    for (const cb::Sensor& s : list.sensors)
        sensors.push_back(toJson(s));
    return {{"sensors", std::move(sensors)}, {"unknownIds", list.unknownIds}};
}

json toJson(const cb::CurveOptionData& o) {
    return {
        {"name", o.name},
        {"checkable", o.checkable},
        {"checked", o.checked},
        {"useCurve", o.useCurve},
        {"combineMode", static_cast<int>(o.combineMode)},
        {"strength", o.strength},
        {"strengthMin", o.strengthMin},
        {"strengthMax", o.strengthMax},
        {"useSameCurve", o.useSameCurve},
        {"commonCurve", o.commonCurve.toString()},
        {"sensorList", toJson(o.sensors)},
    };
}

json toJson(const TipXml& tip) {
    json j{
        {"kind", str(tip.kind)},
        {"type", tip.type},
        {"angle", tip.angle},
        {"spacing", tip.spacing},
        {"useAutoSpacing", tip.useAutoSpacing},
        {"autoSpacingCoeff", tip.autoSpacingCoeff},
    };
    if (tip.kind == TipXml::Kind::Auto) {
        const cb::MaskGeneratorParams& g = tip.autoTip.generator;
        j["auto"] = {
            {"shape", str(g.shape)},
            {"falloff", str(g.falloff)},
            {"diameter", g.diameter},
            {"ratio", g.ratio},
            {"hFade", g.hFade},
            {"vFade", g.vFade},
            {"spikes", g.spikes},
            {"antialiasEdges", g.antialiasEdges},
            {"softness", g.softness},
            {"softnessCurve", g.softnessCurve.toString()},
            {"randomness", tip.autoTip.randomness},
            {"density", tip.autoTip.density},
            {"unknownFalloffId", tip.autoTip.unknownFalloffId},
        };
    } else if (tip.kind == TipXml::Kind::Predefined) {
        const PredefinedTipXml& p = tip.predefined;
        j["predefined"] = {
            {"filename", p.filename},
            {"md5sum", p.md5sum},
            {"scale", p.scale},
            {"applicationRule", str(p.applicationRule)},
            {"application", str(p.application)},
            {"colorAsMask", p.colorAsMask},
            {"colorfulCapable", p.colorfulCapable},
            {"adjustments",
             {{"autoMidPoint", p.adjustments.autoMidPoint},
              {"midPoint", p.adjustments.midPoint},
              {"brightness", p.adjustments.brightness},
              {"contrast", p.adjustments.contrast}}},
        };
    }
    return j;
}

json toJson(const MaskingImport& m) {
    return {
        {"enabled", m.enabled},
        {"op", str(m.op)},
        {"opId", m.opId},
        {"unknownOp", m.unknownOp},
        {"useMasterSize", m.useMasterSize},
        {"masterSizeCoeff", m.masterSizeCoeff},
        {"flow", m.flow},
        {"sizeFromPressure", m.sizeFromPressure},
        {"flowFromPressure", m.flowFromPressure},
        {"tip", toJson(m.tip)},
    };
}

// ------------------------------------------------------------------------------------------------
// Strict deserialization: the first missing/foreign field wins and names itself.

struct Ctx {
    std::string err;
    bool fail(std::string e) {
        if (err.empty())
            err = std::move(e);
        return false;
    }
};

bool getStr(Ctx& c, const json& j, const char* key, std::string& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return c.fail(std::string("missing or non-string '") + key + "'");
    out = it->get<std::string>();
    return true;
}
bool getBool(Ctx& c, const json& j, const char* key, bool& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_boolean())
        return c.fail(std::string("missing or non-bool '") + key + "'");
    out = it->get<bool>();
    return true;
}
bool getNum(Ctx& c, const json& j, const char* key, double& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number())
        return c.fail(std::string("missing or non-number '") + key + "'");
    out = it->get<double>();
    return true;
}
bool getInt(Ctx& c, const json& j, const char* key, int& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer())
        return c.fail(std::string("missing or non-integer '") + key + "'");
    out = it->get<int>();
    return true;
}
const json* getObj(Ctx& c, const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_object()) {
        c.fail(std::string("missing or non-object '") + key + "'");
        return nullptr;
    }
    return &*it;
}
const json* getArr(Ctx& c, const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) {
        c.fail(std::string("missing or non-array '") + key + "'");
        return nullptr;
    }
    return &*it;
}

// Curve strings degrade to the identity rather than failing -- Curve::fromString's own contract
// (it has no error channel); every string this module writes parses back verbatim.
bool getCurve(Ctx& c, const json& j, const char* key, cb::Curve& out) {
    std::string s;
    if (!getStr(c, j, key, s))
        return false;
    out = cb::Curve::fromString(s);
    return true;
}

template <typename T, typename F>
bool getEnum(Ctx& c, const json& j, const char* key, F&& parse, T& out) {
    std::string s;
    if (!getStr(c, j, key, s))
        return false;
    const auto v = parse(s);
    if (!v)
        return c.fail(std::string("foreign value for '") + key + "': " + s);
    out = *v;
    return true;
}

bool fromJson(Ctx& c, const json& j, cb::Sensor& out) {
    std::string id;
    if (!getStr(c, j, "id", id))
        return false;
    const auto sid = cb::sensorFromName(id);
    if (!sid)
        return c.fail("foreign sensor id: " + id);
    out = cb::Sensor::withDefaults(*sid);
    return getCurve(c, j, "curve", out.curve) && getBool(c, j, "periodic", out.range.periodic)
        && getInt(c, j, "length", out.range.length)
        && getBool(c, j, "fanCornersEnabled", out.fan.fanCornersEnabled)
        && getInt(c, j, "fanCornersStep", out.fan.fanCornersStep)
        && getNum(c, j, "angleOffset", out.fan.angleOffset)
        && getBool(c, j, "lockedAngleMode", out.fan.lockedAngleMode);
}

bool fromJson(Ctx& c, const json& j, cb::SensorList& out) {
    const json* sensors = getArr(c, j, "sensors");
    const json* unknown = getArr(c, j, "unknownIds");
    if (sensors == nullptr || unknown == nullptr)
        return false;
    if (sensors->size() > kMaxMbpSensorsPerOption || unknown->size() > kMaxMbpSensorsPerOption)
        return c.fail("sensor list over the cap");
    out.sensors.clear();
    out.unknownIds.clear();
    for (const json& s : *sensors) {
        cb::Sensor sensor;
        if (!s.is_object() || !fromJson(c, s, sensor))
            return c.err.empty() ? c.fail("non-object sensor") : false;
        out.sensors.push_back(std::move(sensor));
    }
    for (const json& u : *unknown) {
        if (!u.is_string())
            return c.fail("non-string unknown sensor id");
        out.unknownIds.push_back(u.get<std::string>());
    }
    return true;
}

bool fromJson(Ctx& c, const json& j, cb::CurveOptionData& out) {
    int combine = 0;
    const json* sensors = getObj(c, j, "sensorList");
    if (sensors == nullptr)
        return false;
    const bool ok = getStr(c, j, "name", out.name) && getBool(c, j, "checkable", out.checkable)
        && getBool(c, j, "checked", out.checked) && getBool(c, j, "useCurve", out.useCurve)
        && getInt(c, j, "combineMode", combine) && getNum(c, j, "strength", out.strength)
        && getNum(c, j, "strengthMin", out.strengthMin)
        && getNum(c, j, "strengthMax", out.strengthMax)
        && getBool(c, j, "useSameCurve", out.useSameCurve)
        && getCurve(c, j, "commonCurve", out.commonCurve) && fromJson(c, *sensors, out.sensors);
    out.combineMode = cb::combineModeFromInt(combine); // anything foreign is the default, as ever
    return ok;
}

bool fromJson(Ctx& c, const json& j, TipXml& out) {
    if (!getEnum(c, j, "kind", kindFrom, out.kind) || !getStr(c, j, "type", out.type)
        || !getNum(c, j, "angle", out.angle) || !getNum(c, j, "spacing", out.spacing)
        || !getBool(c, j, "useAutoSpacing", out.useAutoSpacing)
        || !getNum(c, j, "autoSpacingCoeff", out.autoSpacingCoeff))
        return false;
    if (out.kind == TipXml::Kind::Auto) {
        const json* a = getObj(c, j, "auto");
        if (a == nullptr)
            return false;
        cb::MaskGeneratorParams& g = out.autoTip.generator;
        return getEnum(c, *a, "shape", shapeFrom, g.shape)
            && getEnum(c, *a, "falloff", falloffFrom, g.falloff)
            && getNum(c, *a, "diameter", g.diameter) && getNum(c, *a, "ratio", g.ratio)
            && getNum(c, *a, "hFade", g.hFade) && getNum(c, *a, "vFade", g.vFade)
            && getInt(c, *a, "spikes", g.spikes)
            && getBool(c, *a, "antialiasEdges", g.antialiasEdges)
            && getNum(c, *a, "softness", g.softness)
            && getCurve(c, *a, "softnessCurve", g.softnessCurve)
            && getNum(c, *a, "randomness", out.autoTip.randomness)
            && getNum(c, *a, "density", out.autoTip.density)
            && getBool(c, *a, "unknownFalloffId", out.autoTip.unknownFalloffId);
    }
    if (out.kind == TipXml::Kind::Predefined) {
        const json* p = getObj(c, j, "predefined");
        if (p == nullptr)
            return false;
        PredefinedTipXml& t = out.predefined;
        const json* adj = getObj(c, *p, "adjustments");
        return getStr(c, *p, "filename", t.filename) && getStr(c, *p, "md5sum", t.md5sum)
            && getNum(c, *p, "scale", t.scale)
            && getEnum(c, *p, "applicationRule", ruleFrom, t.applicationRule)
            && getEnum(c, *p, "application", applicationFrom, t.application)
            && getBool(c, *p, "colorAsMask", t.colorAsMask)
            && getBool(c, *p, "colorfulCapable", t.colorfulCapable) && adj != nullptr
            && getBool(c, *adj, "autoMidPoint", t.adjustments.autoMidPoint)
            && getNum(c, *adj, "midPoint", t.adjustments.midPoint)
            && getNum(c, *adj, "brightness", t.adjustments.brightness)
            && getNum(c, *adj, "contrast", t.adjustments.contrast);
    }
    return true; // Kind::Unknown carries only the common attributes
}

bool fromJson(Ctx& c, const json& j, MaskingImport& out) {
    const json* tip = getObj(c, j, "tip");
    return getBool(c, j, "enabled", out.enabled) && getEnum(c, j, "op", maskingOpFrom, out.op)
        && getStr(c, j, "opId", out.opId) && getBool(c, j, "unknownOp", out.unknownOp)
        && getBool(c, j, "useMasterSize", out.useMasterSize)
        && getNum(c, j, "masterSizeCoeff", out.masterSizeCoeff) && getNum(c, j, "flow", out.flow)
        && getBool(c, j, "sizeFromPressure", out.sizeFromPressure)
        && getBool(c, j, "flowFromPressure", out.flowFromPressure) && tip != nullptr
        && fromJson(c, *tip, out.tip);
}

} // namespace

std::string presetToJson(const BrushPreset& preset) {
    json options = json::array();
    for (const cb::CurveOptionData& o : preset.options)
        options.push_back(toJson(o));
    const json j{
        {"schema", kMbpSchema},
        {"name", preset.name},
        {"provenance",
         {{"sourceFormat", preset.provenance.sourceFormat},
          {"sourcePaintop", preset.provenance.sourcePaintop},
          {"fidelity", str(preset.provenance.fidelity)},
          {"droppedOptions", preset.provenance.droppedOptions}}},
        {"tip", toJson(preset.tip)},
        {"paintMode", str(preset.paintMode)},
        {"eraserMode", preset.eraserMode},
        {"blendMode", std::string(core::blendModeName(preset.blendMode))},
        {"compositeOpId", preset.compositeOpId},
        {"accumulator", str(preset.accumulator)},
        {"colorDynamicsActive", preset.colorDynamicsActive},
        {"options", std::move(options)},
        {"masking", toJson(preset.masking)},
        {"airbrush",
         {{"enabled", preset.airbrush.enabled},
          {"rate", preset.airbrush.rate},
          {"ignoreSpacing", preset.airbrush.ignoreSpacing}}},
    };
    return j.dump();
}

std::optional<BrushPreset> presetFromJson(std::string_view text, std::string* error) {
    Ctx c;
    const auto fail = [&](std::string reason) -> std::optional<BrushPreset> {
        if (error != nullptr)
            *error = std::move(reason);
        return std::nullopt;
    };
    const json j = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
        return fail("not a JSON object");
    int schema = 0;
    if (!getInt(c, j, "schema", schema))
        return fail(c.err);
    if (schema > kMbpSchema)
        return fail("written by a newer Mosaic (schema " + std::to_string(schema) + ")");
    if (schema < 1)
        return fail("invalid schema");

    BrushPreset p;
    const json* prov = getObj(c, j, "provenance");
    const json* tip = getObj(c, j, "tip");
    const json* options = getArr(c, j, "options");
    const json* masking = getObj(c, j, "masking");
    const json* airbrush = getObj(c, j, "airbrush");
    if (prov == nullptr || tip == nullptr || options == nullptr || masking == nullptr
        || airbrush == nullptr)
        return fail(c.err);

    const json* dropped = getArr(c, *prov, "droppedOptions");
    bool ok = getStr(c, j, "name", p.name)
        && getStr(c, *prov, "sourceFormat", p.provenance.sourceFormat)
        && getStr(c, *prov, "sourcePaintop", p.provenance.sourcePaintop)
        && getEnum(c, *prov, "fidelity", fidelityFrom, p.provenance.fidelity)
        && dropped != nullptr && fromJson(c, *tip, p.tip)
        && getEnum(c, j, "paintMode", paintModeFrom, p.paintMode)
        && getBool(c, j, "eraserMode", p.eraserMode)
        && getEnum(c, j, "blendMode", blendModeFrom, p.blendMode)
        && getStr(c, j, "compositeOpId", p.compositeOpId)
        && getEnum(c, j, "accumulator", accumulatorFrom, p.accumulator)
        && getBool(c, j, "colorDynamicsActive", p.colorDynamicsActive)
        && fromJson(c, *masking, p.masking)
        && getBool(c, *airbrush, "enabled", p.airbrush.enabled)
        && getNum(c, *airbrush, "rate", p.airbrush.rate)
        && getBool(c, *airbrush, "ignoreSpacing", p.airbrush.ignoreSpacing);
    if (!ok)
        return fail(c.err);

    if (dropped->size() > static_cast<std::size_t>(kMaxDroppedOptions))
        return fail("droppedOptions over the cap");
    for (const json& d : *dropped) {
        if (!d.is_string())
            return fail("non-string droppedOptions entry");
        p.provenance.droppedOptions.push_back(d.get<std::string>());
    }
    if (options->size() > static_cast<std::size_t>(kMaxMbpOptions))
        return fail("options over the cap");
    for (const json& o : *options) {
        cb::CurveOptionData data;
        if (!o.is_object() || !fromJson(c, o, data))
            return fail(c.err.empty() ? "non-object option" : c.err);
        p.options.push_back(std::move(data));
    }
    return p;
}

std::optional<std::vector<std::uint8_t>> writeMbp(const BrushPreset& preset,
                                                  const common::Image& icon,
                                                  std::string* error) {
    auto png = encodePng(icon, {}, error);
    if (!png)
        return std::nullopt;
    // libpng closes with the fixed 12-byte IEND chunk; the iTXt goes right before it.
    static constexpr std::uint8_t kIend[12] = {0,    0,    0,    0,   'I', 'E',
                                               'N',  'D',  0xAE, 0x42, 0x60, 0x82};
    if (png->size() < 12 || std::memcmp(png->data() + png->size() - 12, kIend, 12) != 0) {
        if (error != nullptr)
            *error = "encoder did not close with IEND";
        return std::nullopt;
    }

    const std::string text = presetToJson(preset);
    // iTXt payload: keyword \0, compression flag 0, compression method 0, empty language tag \0,
    // empty translated keyword \0, then the (uncompressed) text.
    std::vector<std::uint8_t> payload;
    payload.reserve(kMbpKeyword.size() + 5 + text.size());
    payload.insert(payload.end(), kMbpKeyword.begin(), kMbpKeyword.end());
    payload.push_back(0); // keyword terminator
    payload.push_back(0); // compression flag: uncompressed
    payload.push_back(0); // compression method
    payload.push_back(0); // language tag (empty)
    payload.push_back(0); // translated keyword (empty)
    payload.insert(payload.end(), text.begin(), text.end());

    std::vector<std::uint8_t> chunk;
    chunk.reserve(12 + payload.size());
    const auto be32 = [&](std::uint32_t v) {
        chunk.push_back(static_cast<std::uint8_t>(v >> 24));
        chunk.push_back(static_cast<std::uint8_t>(v >> 16));
        chunk.push_back(static_cast<std::uint8_t>(v >> 8));
        chunk.push_back(static_cast<std::uint8_t>(v));
    };
    be32(static_cast<std::uint32_t>(payload.size()));
    const std::uint8_t type[4] = {'i', 'T', 'X', 't'};
    chunk.insert(chunk.end(), type, type + 4);
    chunk.insert(chunk.end(), payload.begin(), payload.end());
    uLong crc = crc32(0L, nullptr, 0);
    crc = crc32(crc, type, 4);
    crc = crc32(crc, payload.data(), static_cast<uInt>(payload.size()));
    be32(static_cast<std::uint32_t>(crc));

    png->insert(png->end() - 12, chunk.begin(), chunk.end());
    return png;
}

std::optional<BrushPreset> readMbp(const std::uint8_t* data, std::size_t size,
                                   std::string* error) {
    const auto scan = scanPngText(data, size, error);
    if (!scan)
        return std::nullopt;
    // By KEYWORD, never chunk type -- the .kpp lesson holds for our own container too.
    const PngText* text = scan->find(kMbpKeyword);
    if (text == nullptr) {
        if (error != nullptr)
            *error = scan->undecodable > 0
                ? "no readable 'mosaic-preset' chunk (some text chunks were undecodable)"
                : "no 'mosaic-preset' chunk";
        return std::nullopt;
    }
    return presetFromJson(text->text, error);
}

std::optional<common::Image> readMbpIcon(const std::uint8_t* data, std::size_t size,
                                         std::string* error) {
    std::vector<std::uint8_t> buf(data, data + size);
    return detail::decodePng(buf, error);
}

} // namespace mosaic::io::brush
