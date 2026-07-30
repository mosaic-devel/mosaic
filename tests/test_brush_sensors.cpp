#include <doctest/doctest.h>

#include "core/brush/sensors.hpp"

#include <algorithm>
#include <clocale>
#include <cmath>
#include <string>

using mosaic::core::brush::defaultRangeLength;
using mosaic::core::brush::kSensorCount;
using mosaic::core::brush::Sensor;
using mosaic::core::brush::SensorClass;
using mosaic::core::brush::sensorClass;
using mosaic::core::brush::sensorFromName;
using mosaic::core::brush::sensorHasRange;
using mosaic::core::brush::SensorId;
using mosaic::core::brush::SensorList;
using mosaic::core::brush::sensorName;
using mosaic::core::brush::sensorRangeAttribute;

namespace {

// Every fragment in a shipped preset opens with this. A parser that skips it silently would still
// pass the hand-written cases below, so the real ones carry it.
constexpr const char* kDoctype = "<!DOCTYPE params> ";

[[nodiscard]] std::string curveOf(const SensorList& l, SensorId id) {
    const Sensor* s = l.find(id);
    return s != nullptr ? s->curve.toString() : std::string("<absent>");
}

} // namespace

TEST_CASE("brush sensors: the sixteen wire names round-trip and are distinct") {
    for (std::size_t i = 0; i < kSensorCount; ++i) {
        const auto id = static_cast<SensorId>(i);
        CAPTURE(i);
        const std::string_view name = sensorName(id);
        CHECK_FALSE(name.empty());
        REQUIRE(sensorFromName(name).has_value());
        CHECK(*sensorFromName(name) == id);
    }
    // Distinctness: a duplicated table entry would make sensorFromName() lose a sensor silently.
    for (std::size_t i = 0; i < kSensorCount; ++i) {
        for (std::size_t j = i + 1; j < kSensorCount; ++j)
            CHECK(sensorName(static_cast<SensorId>(i)) != sensorName(static_cast<SensorId>(j)));
    }

    CHECK_FALSE(sensorFromName("tiltdirection").has_value());
    CHECK_FALSE(sensorFromName("").has_value());
    CHECK_FALSE(sensorFromName("Pressure").has_value()); // the wire is lowercase
}

TEST_CASE("brush sensors: the tilt sensors serialize under their legacy names") {
    // The whole reason SensorId's enumerators are named for the wire and not for the UI. Writing
    // "tiltdirection" into a preset would make the option silently inert on reload.
    CHECK(sensorName(SensorId::Ascension) == "ascension");   // UI: "tilt direction"
    CHECK(sensorName(SensorId::Declination) == "declination"); // UI: "tilt elevation"
}

TEST_CASE("brush sensors: the three combination classes are exhaustive and correctly assigned") {
    // docs/brushes.md §3.3, corrected 2026-07-09: four additive sensors, and drawingangle is not
    // one of them. fuzzy (23 presets) and drawingangle (14) are the 2nd/3rd most-used sensors in
    // the default set, so a wrong class here mis-renders a big slice of it.
    CHECK(sensorClass(SensorId::Rotation) == SensorClass::Additive);
    CHECK(sensorClass(SensorId::Ascension) == SensorClass::Additive);
    CHECK(sensorClass(SensorId::Fuzzy) == SensorClass::Additive);
    CHECK(sensorClass(SensorId::FuzzyStroke) == SensorClass::Additive);

    CHECK(sensorClass(SensorId::DrawingAngle) == SensorClass::AbsoluteRotation);

    // Declination is NOT additive even though Ascension is: an elevation is a magnitude, a
    // direction is a signed angle.
    CHECK(sensorClass(SensorId::Declination) == SensorClass::Scaling);
    for (const SensorId id : {SensorId::Pressure, SensorId::PressureIn, SensorId::TangentialPressure,
                              SensorId::XTilt, SensorId::YTilt, SensorId::Speed, SensorId::Fade,
                              SensorId::Distance, SensorId::Time, SensorId::Perspective})
        CHECK(sensorClass(id) == SensorClass::Scaling);

    // Counts, so that adding a sensor without classifying it fails here rather than in a dab.
    int additive = 0;
    int absolute = 0;
    for (std::size_t i = 0; i < kSensorCount; ++i) {
        const SensorClass c = sensorClass(static_cast<SensorId>(i));
        additive += (c == SensorClass::Additive);
        absolute += (c == SensorClass::AbsoluteRotation);
    }
    CHECK(additive == 4);
    CHECK(absolute == 1);
}

TEST_CASE("brush sensors: only three sensors carry a range, and time spells it `duration`") {
    for (std::size_t i = 0; i < kSensorCount; ++i) {
        const auto id = static_cast<SensorId>(i);
        const bool expected =
            id == SensorId::Fade || id == SensorId::Distance || id == SensorId::Time;
        CAPTURE(sensorName(id));
        CHECK(sensorHasRange(id) == expected);
        CHECK(sensorRangeAttribute(id).empty() == !expected);
    }

    CHECK(sensorRangeAttribute(SensorId::Fade) == "length");
    CHECK(sensorRangeAttribute(SensorId::Distance) == "length");
    CHECK(sensorRangeAttribute(SensorId::Time) == "duration"); // the trap

    // Fade is the odd one out; Distance and Time share 30.
    CHECK(defaultRangeLength(SensorId::Fade) == 1000);
    CHECK(defaultRangeLength(SensorId::Distance) == 30);
    CHECK(defaultRangeLength(SensorId::Time) == 30);
    CHECK(Sensor::withDefaults(SensorId::Fade).range.length == 1000);
    CHECK(Sensor::withDefaults(SensorId::Fade).curve.isIdentity());
}

TEST_CASE("brush sensors: a bare <params id=...> means an identity curve, not an absent one") {
    // THE trap. `<curve>` is omitted when it is the identity, so this fragment is a fully-specified
    // pressure sensor -- not a sensor with no response.
    const SensorList bare = SensorList::fromXml(R"(<!DOCTYPE params> <params id="pressure"/>)");
    REQUIRE(bare.sensors.size() == 1);
    CHECK(bare.sensors[0].id == SensorId::Pressure);
    CHECK(bare.sensors[0].curve.isIdentity());
    CHECK(bare.unknownIds.empty());

    // The explicit form shipped by older writers must mean exactly the same thing.
    const SensorList spelled = SensorList::fromXml(
        std::string(kDoctype) + R"(<params id="pressure"> <curve>0,0;1,1;</curve> </params> )");
    REQUIRE(spelled.sensors.size() == 1);
    CHECK(spelled.sensors[0].curve.isIdentity());
    CHECK(spelled.toXml() == bare.toXml());
}

TEST_CASE("brush sensors: shipped single-sensor fragments") {
    // Verbatim from a)_Eraser_Soft.kpp in the CC-0 default bundle.
    const SensorList fuzzy = SensorList::fromXml(
        R"(<!DOCTYPE params> <params id="fuzzy"> <curve>0,0;1,1;</curve> </params> )");
    REQUIRE(fuzzy.sensors.size() == 1);
    CHECK(fuzzy.sensors[0].id == SensorId::Fuzzy);

    // Verbatim from b)_Airbrush_Soft.kpp -- `id` is the SECOND attribute, and rotationModeEnabled
    // is a vestigial attribute with no reader in the current upstream source.
    const SensorList reordered = SensorList::fromXml(
        R"(<!DOCTYPE params> <params rotationModeEnabled="0" id="fuzzy"> <curve>0,0;1,1;</curve> </params> )");
    REQUIRE(reordered.sensors.size() == 1);
    CHECK(reordered.sensors[0].id == SensorId::Fuzzy);
    CHECK(reordered.unknownIds.empty()); // an unknown ATTRIBUTE is not an unknown SENSOR

    // Verbatim from c)_Pencil-1_Hard.kpp -- drawingangle's four attributes, in the order it ships
    // them, which is not the order §3.3 lists them in.
    const SensorList angle = SensorList::fromXml(
        R"(<!DOCTYPE params> <params id="drawingangle" lockedAngleMode="0" angleOffset="0" fanCornersEnabled="1" fanCornersStep="90"> <curve>0,0;1,1;</curve> </params> )");
    REQUIRE(angle.sensors.size() == 1);
    const Sensor& a = angle.sensors[0];
    CHECK(a.id == SensorId::DrawingAngle);
    CHECK(a.fan.fanCornersEnabled);
    CHECK(a.fan.fanCornersStep == 90);
    CHECK(a.fan.angleOffset == doctest::Approx(0.0));
    CHECK_FALSE(a.fan.lockedAngleMode);
}

TEST_CASE("brush sensors: the sensorslist shape") {
    // Verbatim from f)_Bristles-4_Glaze.kpp.
    const SensorList l = SensorList::fromXml(
        R"(<!DOCTYPE params> <params id="sensorslist"> <ChildSensor id="xtilt"> <curve>0,0;1,1;</curve> </ChildSensor> <ChildSensor id="ytilt"> <curve>0,0;1,1;</curve> </ChildSensor> </params> )");
    REQUIRE(l.sensors.size() == 2);
    CHECK(l.sensors[0].id == SensorId::XTilt); // XML order is preserved
    CHECK(l.sensors[1].id == SensorId::YTilt);
    CHECK(l.has(SensorId::XTilt));
    CHECK_FALSE(l.has(SensorId::Pressure));

    // Distinct per-sensor curves survive; the list is not collapsed onto one shared curve here.
    const SensorList mixed = SensorList::fromXml(
        R"(<params id="sensorslist"><ChildSensor id="speed"><curve>0,1;1,0;</curve></ChildSensor>)"
        R"(<ChildSensor id="pressure"><curve>0,0.753769;1,1;</curve></ChildSensor></params>)");
    REQUIRE(mixed.sensors.size() == 2);
    CHECK(curveOf(mixed, SensorId::Speed) == "0,1;1,0;");
    CHECK(curveOf(mixed, SensorId::Pressure) == "0,0.753769;1,1;");

    // "sensorslist" is decided by the root's id, not by the substring appearing anywhere. A curve
    // that happened to contain the word must not flip the shape.
    const SensorList single = SensorList::fromXml(R"(<params id="pressure"/>)");
    CHECK(single.sensors.size() == 1);
}

TEST_CASE("brush sensors: range attributes parse under their own spellings") {
    const SensorList fade =
        SensorList::fromXml(R"(<params id="fade" periodic="1" length="250"/>)");
    REQUIRE(fade.sensors.size() == 1);
    CHECK(fade.sensors[0].range.periodic);
    CHECK(fade.sensors[0].range.length == 250);

    // Time reads `duration`. A `length` on Time is somebody else's attribute: ignored, and the
    // default stands.
    const SensorList time = SensorList::fromXml(R"(<params id="time" duration="7"/>)");
    CHECK(time.sensors[0].range.length == 7);
    const SensorList timeWrong = SensorList::fromXml(R"(<params id="time" length="7"/>)");
    CHECK(timeWrong.sensors[0].range.length == 30);

    // ...and symmetrically, `duration` on Fade is ignored and Fade keeps its own default of 1000.
    const SensorList fadeWrong = SensorList::fromXml(R"(<params id="fade" duration="7"/>)");
    CHECK(fadeWrong.sensors[0].range.length == 1000);

    // Absent `periodic` is false, and absent length falls to the per-sensor default.
    const SensorList dist = SensorList::fromXml(R"(<params id="distance"/>)");
    CHECK_FALSE(dist.sensors[0].range.periodic);
    CHECK(dist.sensors[0].range.length == 30);
}

TEST_CASE("brush sensors: booleans accept 0/1 and the spelled forms") {
    CHECK(SensorList::fromXml(R"(<params id="fade" periodic="1"/>)").sensors[0].range.periodic);
    CHECK_FALSE(SensorList::fromXml(R"(<params id="fade" periodic="0"/>)").sensors[0].range.periodic);
    // The producers we read write 0/1; a foreign one might not.
    CHECK(SensorList::fromXml(R"(<params id="fade" periodic="true"/>)").sensors[0].range.periodic);
    CHECK_FALSE(SensorList::fromXml(R"(<params id="fade" periodic="false"/>)").sensors[0].range.periodic);
    // Any nonzero integer is true, as an int-valued attribute would be.
    CHECK(SensorList::fromXml(R"(<params id="fade" periodic="2"/>)").sensors[0].range.periodic);
}

TEST_CASE("brush sensors: everything unresolvable floors to pressure with an identity curve") {
    // Not "no sensors" -- an option always has one. The reference reader forces pressure active.
    for (const char* xml : {
             "",                                             // absent property
             "   ",                                          // whitespace only
             "<params",                                      // truncated
             "<params id=\"pressure\"",                      // unterminated tag
             "not xml at all",                               // garbage
             R"(<params id="sensorslist"/>)",                // an empty list
             R"(<params/>)",                                 // a root with no id
             R"(<params id="newfangled"/>)",                 // a sensor from a future version
             R"(<params id="sensorslist"><ChildSensor id="nope"/></params>)", // all children unknown
             R"(<params id="sensorslist"><ChildSensor/></params>)",           // a child with no id
         }) {
        CAPTURE(xml);
        const SensorList l = SensorList::fromXml(xml);
        REQUIRE(l.sensors.size() == 1);
        CHECK(l.sensors[0].id == SensorId::Pressure);
        CHECK(l.sensors[0].curve.isIdentity());
    }
}

TEST_CASE("brush sensors: unknown ids are recorded for provenance, deduplicated and capped") {
    const SensorList one = SensorList::fromXml(R"(<params id="newfangled"/>)");
    REQUIRE(one.unknownIds.size() == 1);
    CHECK(one.unknownIds[0] == "newfangled");
    CHECK(one.sensors[0].id == SensorId::Pressure); // the floor still applies

    // A known sensor beside an unknown one keeps the known one and reports the loss.
    const SensorList mixed = SensorList::fromXml(
        R"(<params id="sensorslist"><ChildSensor id="pressure"/><ChildSensor id="ghost"/></params>)");
    REQUIRE(mixed.sensors.size() == 1);
    CHECK(mixed.sensors[0].id == SensorId::Pressure);
    REQUIRE(mixed.unknownIds.size() == 1);
    CHECK(mixed.unknownIds[0] == "ghost");

    // The same unknown id twice is one report, not two.
    const SensorList dup = SensorList::fromXml(
        R"(<params id="sensorslist"><ChildSensor id="ghost"/><ChildSensor id="ghost"/></params>)");
    CHECK(dup.unknownIds.size() == 1);

    // Hostile input cannot make us grow the vector without bound.
    std::string many = R"(<params id="sensorslist">)";
    for (int i = 0; i < 200; ++i)
        many += "<ChildSensor id=\"ghost" + std::to_string(i) + "\"/>";
    many += "</params>";
    const SensorList capped = SensorList::fromXml(many);
    CHECK(capped.unknownIds.size() == 16);
    CHECK(capped.sensors.size() == 1);
}

TEST_CASE("brush sensors: an oversized fragment is refused before it becomes a DOM") {
    // The child/unknown caps run on an already-materialized document, so only a size check can bound
    // what a wall of <ChildSensor/> costs to parse. The largest fragment in either shipped bundle is
    // 192 bytes across all 425 of them, so a real preset is nowhere near this.
    std::string huge = R"(<params id="sensorslist">)";
    while (huge.size() < 64 * 1024)
        huge += R"(<ChildSensor id="pressure"/>)";
    huge += "</params>";
    REQUIRE(huge.size() > 64 * 1024);

    const SensorList refused = SensorList::fromXml(huge);
    REQUIRE(refused.sensors.size() == 1); // the floor, as for any unparsable input
    CHECK(refused.sensors[0].id == SensorId::Pressure);
    CHECK(refused.sensors[0].curve.isIdentity());

    // A fragment just under the cap is still parsed normally -- the guard is a size limit, not a
    // child-count limit wearing a disguise.
    const SensorList ok = SensorList::fromXml(R"(<params id="sensorslist"><ChildSensor id="xtilt"/></params>)");
    CHECK(ok.sensors[0].id == SensorId::XTilt);
}

TEST_CASE("brush sensors: a repeated id keeps the last definition in the first one's position") {
    const SensorList l = SensorList::fromXml(
        R"(<params id="sensorslist">)"
        R"(<ChildSensor id="pressure"><curve>0,0;1,1;</curve></ChildSensor>)"
        R"(<ChildSensor id="speed"><curve>0,1;1,0;</curve></ChildSensor>)"
        R"(<ChildSensor id="pressure"><curve>0,0.5;1,1;</curve></ChildSensor>)"
        R"(</params>)");
    REQUIRE(l.sensors.size() == 2);
    CHECK(l.sensors[0].id == SensorId::Pressure); // position of the first
    CHECK(l.sensors[0].curve.toString() == "0,0.5;1,1;"); // definition of the last
    CHECK(l.sensors[1].id == SensorId::Speed);
}

TEST_CASE("brush sensors: hostile attribute values are clamped rather than trusted") {
    // A zero or negative length is a division by zero in the sensor that consumes it.
    CHECK(SensorList::fromXml(R"(<params id="fade" length="0"/>)").sensors[0].range.length == 1);
    CHECK(SensorList::fromXml(R"(<params id="fade" length="-5"/>)").sensors[0].range.length == 1);
    CHECK(SensorList::fromXml(R"(<params id="fade" length="99999999999"/>)").sensors[0].range.length ==
          1'000'000);

    // fanCornersStep is a degree step; 0 would spin forever.
    const SensorList step = SensorList::fromXml(R"(<params id="drawingangle" fanCornersStep="0"/>)");
    CHECK(step.sensors[0].fan.fanCornersStep == 1);

    // A non-finite angle would poison every dab it touched.
    for (const char* v : {"nan", "inf", "-inf"}) {
        CAPTURE(v);
        const std::string xml = std::string(R"(<params id="drawingangle" angleOffset=")") + v + R"("/>)";
        CHECK(SensorList::fromXml(xml).sensors[0].fan.angleOffset == doctest::Approx(0.0));
    }

    // A finite but absurd bearing passes the finiteness check, then loses every low bit in the /360
    // the sensor performs. Reduce it into (-360, 360) instead: fmod is the identity on legitimate
    // values, and the consumer wraps the sum into one turn regardless.
    const auto offsetOf = [](const char* v) {
        return SensorList::fromXml(std::string(R"(<params id="drawingangle" angleOffset=")") + v +
                                   R"("/>)")
            .sensors[0]
            .fan.angleOffset;
    };
    CHECK(offsetOf("22.5") == doctest::Approx(22.5));   // untouched
    CHECK(offsetOf("-179") == doctest::Approx(-179.0)); // untouched, sign preserved
    CHECK(offsetOf("360") == doctest::Approx(0.0));
    CHECK(offsetOf("450") == doctest::Approx(90.0));
    CHECK(std::abs(offsetOf("1e300")) < 360.0);

    // A malformed number is not a parse failure; the default stands and the sensor survives.
    const SensorList junk = SensorList::fromXml(R"(<params id="fade" length="abc" periodic="xyz"/>)");
    CHECK(junk.sensors[0].range.length == 1000);
    CHECK_FALSE(junk.sensors[0].range.periodic);
}

TEST_CASE("brush sensors: fromXml(toXml(x)) preserves the model") {
    // Not byte-exact -- Mosaic's own presets are JSON and nothing re-exports a .kpp -- but the model
    // must survive a lap, which is what makes the parser's coverage real.
    const char* cases[] = {
        R"(<params id="pressure"/>)",
        R"(<params id="pressure"><curve>0,0.753769;1,1;</curve></params>)",
        R"(<params id="fade" periodic="1" length="250"/>)",
        R"(<params id="time" periodic="0" duration="7"/>)",
        R"(<params id="distance" periodic="1" length="30"><curve>0,1;0.5,0,is_corner;1,1;</curve></params>)",
        R"(<params id="drawingangle" lockedAngleMode="1" angleOffset="22.5" fanCornersEnabled="1" fanCornersStep="90"/>)",
        R"(<params id="sensorslist"><ChildSensor id="xtilt"/><ChildSensor id="ytilt"/></params>)",
        R"(<params id="sensorslist"><ChildSensor id="speed"><curve>0,1;1,0;</curve></ChildSensor><ChildSensor id="ascension"><curve>0,0.25;1,0.75;</curve></ChildSensor></params>)",
    };
    for (const char* xml : cases) {
        CAPTURE(xml);
        const SensorList a = SensorList::fromXml(xml);
        const SensorList b = SensorList::fromXml(a.toXml());
        REQUIRE(b.sensors.size() == a.sensors.size());
        for (std::size_t i = 0; i < a.sensors.size(); ++i) {
            CAPTURE(i);
            CHECK(b.sensors[i].id == a.sensors[i].id);
            CHECK(b.sensors[i].curve.toString() == a.sensors[i].curve.toString());
            CHECK(b.sensors[i].range.periodic == a.sensors[i].range.periodic);
            CHECK(b.sensors[i].range.length == a.sensors[i].range.length);
            CHECK(b.sensors[i].fan.fanCornersEnabled == a.sensors[i].fan.fanCornersEnabled);
            CHECK(b.sensors[i].fan.fanCornersStep == a.sensors[i].fan.fanCornersStep);
            CHECK(b.sensors[i].fan.angleOffset == doctest::Approx(a.sensors[i].fan.angleOffset));
            CHECK(b.sensors[i].fan.lockedAngleMode == a.sensors[i].fan.lockedAngleMode);
        }
        // Serializing twice is stable -- toXml() has no dependence on parse order or on `unknownIds`.
        CHECK(b.toXml() == a.toXml());
    }
}

TEST_CASE("brush sensors: toXml picks the shape from the sensor count and omits identity curves") {
    SensorList one;
    one.sensors.push_back(Sensor::withDefaults(SensorId::Pressure));
    CHECK(one.toXml() == R"(<params id="pressure"/>)");

    SensorList two;
    two.sensors.push_back(Sensor::withDefaults(SensorId::XTilt));
    two.sensors.push_back(Sensor::withDefaults(SensorId::YTilt));
    CHECK(two.toXml() ==
          R"(<params id="sensorslist"><ChildSensor id="xtilt"/><ChildSensor id="ytilt"/></params>)");

    // A non-identity curve is written; the range attributes ride along.
    SensorList fade;
    fade.sensors.push_back(Sensor::withDefaults(SensorId::Fade));
    fade.sensors[0].range.periodic = true;
    fade.sensors[0].curve = mosaic::core::brush::Curve::fromString("0,1;1,0;");
    CHECK(fade.toXml() ==
          R"(<params id="fade" periodic="1" length="1000"><curve>0,1;1,0;</curve></params>)");

    // An angleOffset is written %g, not with a fixed decimal tail.
    SensorList angle;
    angle.sensors.push_back(Sensor::withDefaults(SensorId::DrawingAngle));
    angle.sensors[0].fan.angleOffset = 22.5;
    CHECK(angle.toXml() == R"(<params id="drawingangle" fanCornersEnabled="0" fanCornersStep="30")"
                           R"( angleOffset="22.5" lockedAngleMode="0"/>)");

    // An empty list serializes to an empty sensorslist, which reads back as the pressure floor --
    // the same answer, not a lost sensor.
    const SensorList empty;
    CHECK(empty.toXml() == R"(<params id="sensorslist"/>)");
    CHECK(SensorList::fromXml(empty.toXml()).sensors[0].id == SensorId::Pressure);
}

TEST_CASE("brush sensors: attribute numbers ignore LC_NUMERIC") {
    // Same trap as the curve strings (tests/test_brush_curve.cpp): the app runs under the user's
    // locale, strtod and %g follow LC_NUMERIC, and the format always writes '.'. angleOffset is the
    // only fractional attribute a sensor carries, and under a comma-decimal locale the unguarded
    // reader silently substitutes its default -- a wrong dab rotation on every stamp, from a file
    // that is perfectly well-formed.
    const char* saved = std::setlocale(LC_ALL, nullptr);
    const std::string restore = saved != nullptr ? saved : "C";
    bool applied = false;
    for (const char* n : {"pl_PL.utf8", "pl_PL.UTF-8", "de_DE.utf8", "de_DE.UTF-8", "fr_FR.UTF-8",
                          "es_ES.UTF-8", "ru_RU.UTF-8", "nl_NL.UTF-8", "it_IT.UTF-8"}) {
        if (std::setlocale(LC_ALL, n) != nullptr) {
            applied = true;
            break;
        }
    }
    const bool comma = applied && std::string(std::localeconv()->decimal_point) == ",";
    if (!comma) {
        std::setlocale(LC_ALL, restore.c_str());
        MESSAGE("no comma-decimal locale installed; skipping the LC_NUMERIC checks");
        return;
    }

    const SensorList read = SensorList::fromXml(R"(<params id="drawingangle" angleOffset="22.5"/>)");
    CHECK(read.sensors[0].fan.angleOffset == doctest::Approx(22.5));

    // ...and the writer emits '.', so the fragment stays readable by a C-locale reader.
    SensorList write;
    write.sensors.push_back(Sensor::withDefaults(SensorId::DrawingAngle));
    write.sensors[0].fan.angleOffset = 22.5;
    CHECK(write.toXml().find(R"(angleOffset="22.5")") != std::string::npos);

    // Curves nested in a sensor go through the same helpers.
    const SensorList curve =
        SensorList::fromXml(R"(<params id="pressure"><curve>0,0.753769;1,1;</curve></params>)");
    CHECK(curve.sensors[0].curve.toString() == "0,0.753769;1,1;");
    CHECK(curve.sensors[0].curve.eval(0.0) == doctest::Approx(0.753769));

    std::setlocale(LC_ALL, restore.c_str());
}
