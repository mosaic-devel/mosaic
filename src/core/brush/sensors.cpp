#include "core/brush/sensors.hpp"

#include "core/brush/parse_util.hpp"
#include "core/brush/xml_util.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace mosaic::core::brush {

namespace {

using detail::formatNumber;
using detail::parseDouble;
using detail::parseLongLong;
using detail::trim;

// Bounds on what a third-party fragment may ask of us. The XML comes from a file the user chose to
// open, so every attribute that later divides is clamped here rather than at the point it would go
// wrong, and the input is refused outright before it can be turned into a DOM.
//
// kMaxFragmentBytes is the one that actually bounds memory: pugixml materializes the whole document
// before any of the other caps can run, so a multi-megabyte wall of <ChildSensor/> would already
// have been allocated (at several times its own size) by the time kMaxChildren stopped us reading
// it. The largest sensor fragment in either shipped CC-0 bundle is 192 bytes across all 425 of
// them, so 64 KiB is ~340x headroom over anything real.
constexpr std::size_t kMaxFragmentBytes = 64 * 1024;
constexpr std::size_t kMaxChildren = 256;    // <ChildSensor> elements we will look at
constexpr std::size_t kMaxUnknownIds = 16;   // ids recorded for PresetProvenance
constexpr int kMaxRangeLength = 1'000'000;   // fade dabs / distance px / time tenths-of-a-second

// Indexed by SensorId. The wire names -- the ONLY place `ascension`/`declination` are spelled.
constexpr std::array<std::string_view, kSensorCount> kSensorNames = {
    "pressure", "pressurein", "tangentialpressure", "drawingangle",
    "xtilt",    "ytilt",      "ascension",          "declination",
    "rotation", "fuzzy",      "fuzzystroke",        "speed",
    "fade",     "distance",   "time",               "perspective",
};

// The enum's order is the table's order; a reordered enumerator would silently rename two sensors.
static_assert(kSensorNames.size() == kSensorCount);
static_assert(static_cast<std::size_t>(SensorId::Perspective) == kSensorCount - 1);

[[nodiscard]] constexpr std::size_t index(SensorId id) noexcept {
    return static_cast<std::size_t>(id);
}

// The length attribute's spelling, as a C string so pugixml can take it directly.
[[nodiscard]] const char* rangeAttributeCStr(SensorId id) noexcept {
    switch (id) {
    case SensorId::Fade:
    case SensorId::Distance:
        return "length";
    case SensorId::Time:
        return "duration"; // the trap: Time does not spell it `length`
    default:
        return "";
    }
}

// The strict attribute readers moved to core/brush/xml_util.hpp when the preset XML layer
// (io/brush/preset_xml.cpp) grew a second consumer of the same garbage-to-documented-default rule.
using detail::boolAttribute;
using detail::doubleAttribute;
using detail::intAttribute;

// Read one sensor from the element that carries its `id` -- the root `<params>` in the single-sensor
// shape, a `<ChildSensor>` otherwise. The two shapes differ only in where the element sits.
[[nodiscard]] Sensor readSensor(pugi::xml_node node, SensorId id) {
    Sensor s = Sensor::withDefaults(id);

    // Omitted <curve> means the identity, not "no curve". withDefaults() already left it there.
    if (const pugi::xml_node curve = node.child("curve"))
        s.curve = Curve::fromString(curve.text().get());

    // Every "absent attribute" default is read back off withDefaults(), never restated as a literal
    // here: a default that lives in two places is a default that will disagree with itself. This is
    // also what keeps Fade's length of 1000 from being quietly overwritten with the struct's 30.
    const SensorRange rd = s.range;
    const SensorFan fd = s.fan;

    if (sensorHasRange(id)) {
        s.range.periodic = boolAttribute(node.attribute("periodic"), rd.periodic);
        s.range.length =
            intAttribute(node.attribute(rangeAttributeCStr(id)), rd.length, 1, kMaxRangeLength);
    }

    if (id == SensorId::DrawingAngle) {
        s.fan.fanCornersEnabled = boolAttribute(node.attribute("fanCornersEnabled"), fd.fanCornersEnabled);
        s.fan.fanCornersStep = intAttribute(node.attribute("fanCornersStep"), fd.fanCornersStep, 1, 360);
        s.fan.lockedAngleMode = boolAttribute(node.attribute("lockedAngleMode"), fd.lockedAngleMode);

        // The offset is a bearing, so reduce it into (-360, 360) rather than clamping: fmod is the
        // identity on every legitimate value and is exact, where `angleOffset="1e300"` would
        // otherwise survive its finiteness check and then lose all its low bits in the /360 that
        // the sensor performs -- a nonsense rotation on every dab, from an attribute that looked
        // well-formed. The consumer wraps the sum into one turn anyway, so this loses nothing.
        s.fan.angleOffset = std::fmod(doubleAttribute(node.attribute("angleOffset"), fd.angleOffset), 360.0);
    }

    return s;
}

// Resolve `node`'s id and fold the sensor into `out`. A repeated id keeps the last definition, in
// the position the first one claimed -- the reference reader maps by id, so later wins; holding the
// position keeps toXml() deterministic.
void appendSensor(SensorList& out, pugi::xml_node node) {
    const char* rawId = node.attribute("id").value();
    const std::string_view name(rawId != nullptr ? rawId : "");
    if (name.empty())
        return;

    const std::optional<SensorId> id = sensorFromName(name);
    if (!id) {
        if (out.unknownIds.size() < kMaxUnknownIds &&
            std::find(out.unknownIds.begin(), out.unknownIds.end(), name) == out.unknownIds.end())
            out.unknownIds.emplace_back(name);
        return;
    }

    Sensor s = readSensor(node, *id);
    const auto it = std::find_if(out.sensors.begin(), out.sensors.end(),
                                 [&](const Sensor& e) { return e.id == *id; });
    if (it != out.sensors.end())
        *it = std::move(s);
    else
        out.sensors.push_back(std::move(s));
}

void writeSensor(pugi::xml_node node, const Sensor& s) {
    node.append_attribute("id") = std::string(sensorName(s.id)).c_str();

    if (sensorHasRange(s.id)) {
        node.append_attribute("periodic") = s.range.periodic ? 1 : 0;
        node.append_attribute(rangeAttributeCStr(s.id)) = s.range.length;
    }
    if (s.id == SensorId::DrawingAngle) {
        node.append_attribute("fanCornersEnabled") = s.fan.fanCornersEnabled ? 1 : 0;
        node.append_attribute("fanCornersStep") = s.fan.fanCornersStep;
        node.append_attribute("angleOffset") = formatNumber(s.fan.angleOffset).c_str();
        node.append_attribute("lockedAngleMode") = s.fan.lockedAngleMode ? 1 : 0;
    }

    // Omitting the identity curve is the format's convention, not an optimization: a reader that
    // sees `<curve>` treats the option as having a custom curve (see the legacy Custom{X} path).
    if (!s.curve.isIdentity())
        node.append_child("curve").text().set(s.curve.toString().c_str());
}

struct StringWriter final : pugi::xml_writer {
    std::string out;
    void write(const void* data, std::size_t size) override {
        out.append(static_cast<const char*>(data), size);
    }
};

} // namespace

std::string_view sensorName(SensorId id) noexcept {
    const std::size_t i = index(id);
    return i < kSensorNames.size() ? kSensorNames[i] : std::string_view{};
}

std::optional<SensorId> sensorFromName(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kSensorNames.size(); ++i) {
        if (kSensorNames[i] == name)
            return static_cast<SensorId>(i);
    }
    return std::nullopt;
}

SensorClass sensorClass(SensorId id) noexcept {
    switch (id) {
    // Four additive sensors, not one. Their outputs sum into a component of their own and never see
    // curveMode; `ascension` is here because a tilt DIRECTION is a signed angle, not a magnitude.
    case SensorId::Rotation:
    case SensorId::Ascension:
    case SensorId::Fuzzy:
    case SensorId::FuzzyStroke:
        return SensorClass::Additive;
    // The only absolute-rotation sensor. It sets rather than sums -- an angle sensor that added
    // would spin the dab further on every extra sensor.
    case SensorId::DrawingAngle:
        return SensorClass::AbsoluteRotation;
    default:
        return SensorClass::Scaling;
    }
}

bool sensorHasRange(SensorId id) noexcept {
    return id == SensorId::Fade || id == SensorId::Distance || id == SensorId::Time;
}

std::string_view sensorRangeAttribute(SensorId id) noexcept { return rangeAttributeCStr(id); }

int defaultRangeLength(SensorId id) noexcept { return id == SensorId::Fade ? 1000 : 30; }

Sensor Sensor::withDefaults(SensorId id) {
    Sensor s;
    s.id = id;
    s.range.length = defaultRangeLength(id);
    return s;
}

SensorList SensorList::fromXml(std::string_view xml) {
    SensorList out;

    pugi::xml_document doc;
    // Size is checked before the DOM is built, not after: every other cap below runs on an
    // already-materialized document, so none of them can bound what a wall of <ChildSensor/> costs
    // to parse. Short-circuit order is the whole point of this condition.
    if (xml.size() <= kMaxFragmentBytes && doc.load_buffer(xml.data(), xml.size())) {
        const pugi::xml_node root = doc.document_element();
        // The reference reader decides the shape by searching the raw text for "sensorslist"; the
        // root's id says the same thing without matching a curve that happens to contain the word.
        const std::string_view rootId = root.attribute("id").value();
        if (rootId == "sensorslist") {
            std::size_t seen = 0;
            for (pugi::xml_node child : root.children("ChildSensor")) {
                if (++seen > kMaxChildren)
                    break;
                appendSensor(out, child);
            }
        } else if (!rootId.empty()) {
            appendSensor(out, root);
        }
    }

    // The floor. Malformed XML, an empty string, an unknown id, and a sensorslist whose children we
    // all rejected land here together: an option always has at least one sensor, and pressure with
    // an identity curve is the one the reference reader falls back to.
    if (out.sensors.empty())
        out.sensors.push_back(Sensor::withDefaults(SensorId::Pressure));

    return out;
}

std::string SensorList::toXml() const {
    pugi::xml_document doc;
    pugi::xml_node root = doc.append_child("params");

    if (sensors.size() == 1) {
        writeSensor(root, sensors.front());
    } else {
        // Zero sensors serializes to an empty list, which fromXml() reads back as the pressure
        // floor. That is the same answer, not a lost sensor.
        root.append_attribute("id") = "sensorslist";
        for (const Sensor& s : sensors)
            writeSensor(root.append_child("ChildSensor"), s);
    }

    StringWriter writer;
    doc.print(writer, "", pugi::format_raw | pugi::format_no_declaration);
    return std::move(writer.out);
}

const Sensor* SensorList::find(SensorId id) const noexcept {
    const auto it = std::find_if(sensors.begin(), sensors.end(),
                                 [&](const Sensor& s) { return s.id == id; });
    return it != sensors.end() ? &*it : nullptr;
}

} // namespace mosaic::core::brush
