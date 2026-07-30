#include "io/brush/preset_json.hpp"

#include "core/brush/curve.hpp"
#include "core/brush/curve_option.hpp"
#include "core/brush/sensors.hpp"
#include "io/io.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// The native .mbp container (io/brush/preset_json.hpp): OUR format, so the reader is STRICT --
// every failure case here must fail loudly with a reason, and the round trip must be lossless
// down to the curve strings. Hostile payloads are built by surgical edits of the serializer's own
// output (the same trick test_zip plays with its own deflate): every mutant is one field away
// from valid, so a pass can only mean the check under test fired.
namespace {

using namespace mosaic::io::brush;
namespace cb = mosaic::core::brush;
namespace core = mosaic::core;
namespace common = mosaic::common;
using json = nlohmann::json;

// A preset exercising EVERY serialized field away from its default: predefined tip with
// adjustments, masking with its own auto tip, three sensors across two options, unknown ids,
// dropped options, airbrush.
BrushPreset maximalPreset() {
    BrushPreset p;
    p.name = "Ink \"maximal\" \xE5\xA2\xA8"; // quotes + UTF-8 survive the JSON round trip
    p.provenance.sourceFormat = "kpp";
    p.provenance.sourcePaintop = "paintbrush";
    p.provenance.fidelity = PresetFidelity::Approximated;
    p.provenance.droppedOptions = {"Texture", "Size: unknown sensor 'foo'"};

    p.tip.kind = TipXml::Kind::Predefined;
    p.tip.type = "gbr_brush";
    p.tip.angle = 0.7;
    p.tip.spacing = 0.18;
    p.tip.useAutoSpacing = true;
    p.tip.autoSpacingCoeff = 0.9;
    p.tip.predefined.filename = "vegetal.gbr";
    p.tip.predefined.md5sum = "0123456789abcdef0123456789abcdef";
    p.tip.predefined.scale = 1.75;
    p.tip.predefined.applicationRule = TipApplicationRule::Explicit;
    p.tip.predefined.application = cb::TipApplication::LightnessMap;
    p.tip.predefined.colorAsMask = true;
    p.tip.predefined.colorfulCapable = true;
    p.tip.predefined.adjustments.autoMidPoint = true;
    p.tip.predefined.adjustments.midPoint = 0.4;
    p.tip.predefined.adjustments.brightness = -0.25;
    p.tip.predefined.adjustments.contrast = 0.1;

    p.paintMode = cb::PaintMode::Buildup;
    p.eraserMode = true;
    p.blendMode = static_cast<core::BlendMode>(1); // any non-Normal mode; round-trips by name
    p.compositeOpId = "some_raw_id";
    p.accumulator = cb::StrokeAccumulator::Colored;
    p.colorDynamicsActive = true;

    cb::CurveOptionData size;
    size.name = "Size";
    size.checkable = true;
    size.checked = true;
    size.useCurve = true;
    size.strength = 0.8;
    size.strengthMin = 0.1;
    size.strengthMax = 2.0;
    size.useSameCurve = false;
    size.commonCurve = cb::Curve({{0.0, 0.0, true}, {1.0, 1.0, true}});
    cb::Sensor pressure = cb::Sensor::withDefaults(cb::SensorId::Pressure);
    pressure.curve = cb::Curve({{0.0, 0.1, true}, {0.4, 0.9, false}, {1.0, 1.0, true}});
    cb::Sensor fade = cb::Sensor::withDefaults(cb::SensorId::Fade);
    fade.range.periodic = true;
    fade.range.length = 123;
    size.sensors.sensors = {pressure, fade};
    size.sensors.unknownIds = {"tangentialpressure"};

    cb::CurveOptionData rot;
    rot.name = "Rotation";
    rot.checked = true;
    rot.useCurve = true;
    cb::Sensor angle = cb::Sensor::withDefaults(cb::SensorId::DrawingAngle);
    angle.fan.fanCornersEnabled = true;
    angle.fan.fanCornersStep = 45;
    angle.fan.angleOffset = 15.5;
    angle.fan.lockedAngleMode = true;
    rot.sensors.sensors = {angle};
    p.options = {size, rot};

    p.masking.enabled = true;
    p.masking.op = cb::MaskingOp::LinearDodge;
    p.masking.opId = "linear_dodge_raw";
    p.masking.unknownOp = true;
    p.masking.useMasterSize = false;
    p.masking.masterSizeCoeff = 2.5;
    p.masking.flow = 0.6;
    p.masking.sizeFromPressure = true;
    p.masking.flowFromPressure = false;
    p.masking.tip.kind = TipXml::Kind::Auto;
    p.masking.tip.type = "auto_brush";
    p.masking.tip.spacing = 0.33;
    p.masking.tip.autoTip.generator.shape = cb::MaskShape::Rect;
    p.masking.tip.autoTip.generator.falloff = cb::MaskFalloff::Gauss;
    p.masking.tip.autoTip.generator.diameter = 42.0;
    p.masking.tip.autoTip.generator.ratio = 0.5;
    p.masking.tip.autoTip.generator.hFade = 0.2;
    p.masking.tip.autoTip.generator.vFade = 0.3;
    p.masking.tip.autoTip.generator.spikes = 5;
    p.masking.tip.autoTip.generator.antialiasEdges = true;
    p.masking.tip.autoTip.generator.softness = 0.7;
    p.masking.tip.autoTip.generator.softnessCurve =
        cb::Curve({{0.0, 1.0, false}, {0.6, 0.6, true}, {1.0, 0.0, false}});
    p.masking.tip.autoTip.randomness = 0.15;
    p.masking.tip.autoTip.density = 0.85;
    p.masking.tip.autoTip.unknownFalloffId = true;

    p.airbrush.enabled = true;
    p.airbrush.rate = 33.5;
    p.airbrush.ignoreSpacing = true;
    return p;
}

common::Image testIcon() {
    common::Image icon(8, 5);
    for (std::uint32_t i = 0; i < icon.pixelCount(); ++i) {
        icon.rgba[i * 4 + 0] = static_cast<std::uint8_t>(i * 7);
        icon.rgba[i * 4 + 1] = static_cast<std::uint8_t>(255 - i * 5);
        icon.rgba[i * 4 + 2] = static_cast<std::uint8_t>(i * 13);
        icon.rgba[i * 4 + 3] = static_cast<std::uint8_t>(200 + i);
    }
    return icon;
}

// One-field surgery on the serializer's own output; returns the reader's verdict.
std::optional<BrushPreset> readEdited(const std::function<void(json&)>& edit,
                                      std::string* err = nullptr) {
    json j = json::parse(presetToJson(maximalPreset()));
    edit(j);
    return presetFromJson(j.dump(), err);
}

} // namespace

// ------------------------------------------------------------------------------------------------

TEST_CASE("mbp: preset JSON round-trips losslessly and deterministically") {
    const BrushPreset a = maximalPreset();
    const std::string ja = presetToJson(a);
    // Determinism: an equal preset built independently serializes to the identical string.
    CHECK(ja == presetToJson(maximalPreset()));

    std::string err;
    const auto b = presetFromJson(ja, &err);
    REQUIRE_MESSAGE(b.has_value(), err);
    CHECK(presetToJson(*b) == ja); // full-fidelity: the round trip is byte-stable

    // Spot checks through the typed model, not just the string.
    CHECK(b->name == a.name);
    CHECK(b->provenance.fidelity == PresetFidelity::Approximated);
    CHECK(b->provenance.droppedOptions == a.provenance.droppedOptions);
    CHECK(b->tip.kind == TipXml::Kind::Predefined);
    CHECK(b->tip.predefined.md5sum == a.tip.predefined.md5sum);
    CHECK(b->tip.predefined.application == cb::TipApplication::LightnessMap);
    CHECK(b->tip.predefined.adjustments.brightness == doctest::Approx(-0.25));
    CHECK(b->paintMode == cb::PaintMode::Buildup);
    CHECK(b->eraserMode);
    CHECK(b->blendMode == a.blendMode);
    CHECK(b->accumulator == cb::StrokeAccumulator::Colored);
    CHECK(b->colorDynamicsActive);
    REQUIRE(b->options.size() == 2);
    const cb::CurveOptionData& size = b->options[0];
    CHECK(size.strengthMax == doctest::Approx(2.0));
    REQUIRE(size.sensors.sensors.size() == 2);
    CHECK(size.sensors.sensors[0].curve.eval(0.4) == doctest::Approx(0.9).epsilon(1e-9));
    CHECK(size.sensors.sensors[1].range.periodic);
    CHECK(size.sensors.sensors[1].range.length == 123);
    CHECK(size.sensors.unknownIds == std::vector<std::string>{"tangentialpressure"});
    const cb::Sensor& angle = b->options[1].sensors.sensors.at(0);
    CHECK(angle.fan.fanCornersEnabled);
    CHECK(angle.fan.fanCornersStep == 45);
    CHECK(angle.fan.angleOffset == doctest::Approx(15.5));
    CHECK(angle.fan.lockedAngleMode);
    CHECK(b->masking.enabled);
    CHECK(b->masking.op == cb::MaskingOp::LinearDodge);
    CHECK(b->masking.unknownOp);
    CHECK(b->masking.masterSizeCoeff == doctest::Approx(2.5));
    CHECK(b->masking.tip.autoTip.generator.shape == cb::MaskShape::Rect);
    CHECK(b->masking.tip.autoTip.generator.softnessCurve.eval(0.6)
          == doctest::Approx(0.6).epsilon(1e-9));
    CHECK(b->airbrush.enabled);
    CHECK(b->airbrush.rate == doctest::Approx(33.5));
    CHECK(b->airbrush.ignoreSpacing);
}

TEST_CASE("mbp: container round-trips the preset and the icon") {
    const BrushPreset preset = maximalPreset();
    const common::Image icon = testIcon();
    std::string err;
    const auto bytes = writeMbp(preset, icon, &err);
    REQUIRE_MESSAGE(bytes.has_value(), err);

    const auto back = readMbp(bytes->data(), bytes->size(), &err);
    REQUIRE_MESSAGE(back.has_value(), err);
    CHECK(presetToJson(*back) == presetToJson(preset));

    // The raster IS the icon, losslessly (RGBA PNG).
    const auto image = readMbpIcon(bytes->data(), bytes->size(), &err);
    REQUIRE_MESSAGE(image.has_value(), err);
    CHECK(image->width == icon.width);
    CHECK(image->height == icon.height);
    CHECK(image->rgba == icon.rgba);

    // An empty icon cannot become a container.
    CHECK_FALSE(writeMbp(preset, common::Image{}, &err).has_value());
    CHECK_FALSE(err.empty());
}

TEST_CASE("mbp: encodePng matches savePng byte for byte") {
    const common::Image icon = testIcon();
    std::string err;
    const auto mem = mosaic::io::encodePng(icon, {}, &err);
    REQUIRE_MESSAGE(mem.has_value(), err);

    const auto path = std::filesystem::temp_directory_path() / "mosaic_test_encode_png.png";
    REQUIRE_MESSAGE(mosaic::io::savePng(icon, path.string(), {}, &err), err);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    std::vector<std::uint8_t> disk(static_cast<std::size_t>(f.tellg()));
    f.seekg(0);
    REQUIRE(f.read(reinterpret_cast<char*>(disk.data()),
                   static_cast<std::streamsize>(disk.size()))
                .good());
    std::filesystem::remove(path);
    CHECK(*mem == disk);

    CHECK_FALSE(mosaic::io::encodePng(common::Image{}, {}, &err).has_value());
    CHECK(err.find("empty") != std::string::npos);
}

TEST_CASE("mbp: reader is strict -- every defect names itself") {
    std::string err;

    SUBCASE("newer schema") {
        CHECK_FALSE(readEdited([](json& j) { j["schema"] = 2; }, &err).has_value());
        CHECK(err.find("newer Mosaic") != std::string::npos);
    }
    SUBCASE("invalid schema") {
        CHECK_FALSE(readEdited([](json& j) { j["schema"] = 0; }, &err).has_value());
        CHECK(err.find("invalid schema") != std::string::npos);
    }
    SUBCASE("missing top-level string") {
        CHECK_FALSE(readEdited([](json& j) { j.erase("name"); }, &err).has_value());
        CHECK(err.find("'name'") != std::string::npos);
    }
    SUBCASE("foreign enum values fail, not default") {
        CHECK_FALSE(
            readEdited([](json& j) { j["provenance"]["fidelity"] = "perfect"; }, &err)
                .has_value());
        CHECK(err.find("fidelity") != std::string::npos);
        CHECK_FALSE(readEdited([](json& j) { j["blendMode"] = "hyperburn"; }, &err).has_value());
        CHECK_FALSE(readEdited([](json& j) { j["paintMode"] = "wet"; }, &err).has_value());
        CHECK_FALSE(readEdited([](json& j) { j["accumulator"] = "spicy"; }, &err).has_value());
        CHECK_FALSE(
            readEdited([](json& j) { j["tip"]["predefined"]["application"] = "stamp?"; }, &err)
                .has_value());
        CHECK_FALSE(
            readEdited([](json& j) { j["options"][0]["sensorList"]["sensors"][0]["id"] = "psi"; },
                       &err)
                .has_value());
        CHECK(err.find("psi") != std::string::npos);
    }
    SUBCASE("wrong types fail") {
        CHECK_FALSE(readEdited([](json& j) { j["eraserMode"] = "yes"; }, &err).has_value());
        CHECK_FALSE(readEdited([](json& j) { j["airbrush"]["rate"] = "fast"; }, &err).has_value());
        CHECK_FALSE(readEdited([](json& j) { j["options"][1] = 7; }, &err).has_value());
        CHECK_FALSE(readEdited([](json& j) { j["masking"] = json::array(); }, &err).has_value());
    }
    SUBCASE("a predefined tip requires its block") {
        CHECK_FALSE(
            readEdited([](json& j) { j["tip"].erase("predefined"); }, &err).has_value());
        CHECK(err.find("'predefined'") != std::string::npos);
    }
    SUBCASE("caps") {
        CHECK_FALSE(readEdited(
                        [](json& j) {
                            for (int i = 0; i < kMaxMbpOptions + 1; ++i)
                                j["options"].push_back(j["options"][0]);
                        },
                        &err)
                        .has_value());
        CHECK(err.find("options over the cap") != std::string::npos);
        CHECK_FALSE(readEdited(
                        [](json& j) {
                            auto& sensors = j["options"][0]["sensorList"]["sensors"];
                            for (int i = 0; i < kMaxMbpSensorsPerOption + 1; ++i)
                                sensors.push_back(sensors[0]);
                        },
                        &err)
                        .has_value());
        CHECK(err.find("sensor list over the cap") != std::string::npos);
        CHECK_FALSE(readEdited(
                        [](json& j) {
                            for (int i = 0; i < kMaxDroppedOptions + 1; ++i)
                                j["provenance"]["droppedOptions"].push_back("x");
                        },
                        &err)
                        .has_value());
        CHECK(err.find("droppedOptions over the cap") != std::string::npos);
    }
    SUBCASE("not JSON at all") {
        CHECK_FALSE(presetFromJson("]", &err).has_value());
        CHECK(err == "not a JSON object");
        CHECK_FALSE(presetFromJson("[1]", &err).has_value());
    }
}

TEST_CASE("mbp: container failures are graceful") {
    std::string err;
    // Not a PNG.
    const std::uint8_t junk[] = {'j', 'u', 'n', 'k'};
    CHECK_FALSE(readMbp(junk, sizeof junk, &err).has_value());

    // A valid PNG with no preset chunk: the plain encoder output.
    const auto plain = mosaic::io::encodePng(testIcon(), {}, &err);
    REQUIRE(plain.has_value());
    CHECK_FALSE(readMbp(plain->data(), plain->size(), &err).has_value());
    CHECK(err.find("mosaic-preset") != std::string::npos);

    // Truncations of a real container: never a crash, never a false positive.
    const auto full = writeMbp(maximalPreset(), testIcon(), &err);
    REQUIRE(full.has_value());
    for (std::size_t cut : {std::size_t{0}, std::size_t{7}, std::size_t{33},
                            full->size() / 2, full->size() - 13, full->size() - 1})
        CHECK_FALSE(readMbp(full->data(), cut, &err).has_value());
}
