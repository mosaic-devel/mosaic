#include "io/brush/kpp.hpp"
#include "io/brush/mapper.hpp"
#include "io/brush/preset_xml.hpp"

#include "kpp_builder.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

// The .kpp reader + paintop mapper (io/brush/kpp.hpp, mapper.hpp). Four REAL CC-0 presets from
// the shipped default bundle serve as fixtures, each chosen for a trap it carries: Basic-5 is the
// plain zTXt wash brush; Pencil-3 stores its preset chunk as tEXt AND paints BUILDUP; Eraser_
// Circle's CompositeOp is "erase" (StrokeMode, not a blend mode); Charcoal_Pencil_Medium enables
// the masking brush with linear_dodge and a master-size coupling. Synthetic documents cover the
// mapper's tier/fallback matrix.
namespace {

using namespace mosaic::io::brush;
namespace cb = mosaic::core::brush;

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE_MESSAGE(f.good(), path);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(f.tellg()));
    f.seekg(0);
    REQUIRE(f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()))
                .good());
    return buf;
}

std::vector<std::uint8_t> fixture(const char* name) {
    return readFile(std::string(MOSAIC_FIXTURE_DIR) + "/brush/" + name);
}

// A minimal but complete tip, so synthetic presets are not badged for a missing one.
constexpr const char* kDef =
    R"(<param name="brush_definition" type="string"><![CDATA[<Brush type="auto_brush" BrushVersion="2" spacing="0.1"><MaskGenerator diameter="10"/></Brush>]]></param>)";

BrushPreset mapXml(std::string xml, bool withTip = true) {
    // Splice the minimal tip in front of </Preset> unless the case is ABOUT a missing tip.
    if (withTip)
        xml.insert(xml.rfind("</Preset>"), kDef);
    const auto parsed = parsePresetXml(xml);
    REQUIRE(parsed.has_value());
    return mapPreset(*parsed, "kpp");
}

bool hasDrop(const BrushPreset& p, std::string_view needle) {
    for (const std::string& d : p.provenance.droppedOptions)
        if (d.find(needle) != std::string::npos)
            return true;
    return false;
}

// ⚠ EVERY FIXTURE HERE CARRIES ONE STANDING NOTE, AND IT IS NOT A BUG IN THE FIXTURE. None of them
// says anything about Opacity -- and an absent `{X}Sensor` still yields a PRESSURE sensor, with
// `{X}UseCurve` defaulting on (§3.2, the two defaults that surprise). So every one of them has a
// live, pressure-driven opacity, which the engine honours as the stroke's STATIC ceiling and no
// further: a per-dab opacity is a per-dab CEILING, and Wash freezes its cap once at begin().
//
// The cases below are about the paintop, the composite op, the airbrush, the sensors. `otherDrops`
// is the provenance with that standing note taken out -- i.e. what each of them is actually asserting.
[[nodiscard]] std::vector<std::string> otherDrops(const BrushPreset& p) {
    std::vector<std::string> out;
    for (const std::string& d : p.provenance.droppedOptions)
        if (d.rfind("Opacity (dynamic", 0) != 0)
            out.push_back(d);
    return out;
}

// ---- synthetic .kpp containers: tests/kpp_builder.hpp (shared with test_brush_library) ----

using kpptest::syntheticKpp;

constexpr const char* kMinimalPreset =
    R"(<Preset name="syn" paintopid="paintbrush"><param name="k" type="string"><![CDATA[v]]></param></Preset>)";

} // namespace

// ------------------------------------------------------------------------------------------------
// Real fixtures.

TEST_CASE("kpp: Basic-5 -- the plain wash pixel brush, at Exact fidelity") {
    const auto buf = fixture("b)_Basic-5_Size_default.kpp");
    std::string err;
    const auto preset = readKpp(buf.data(), buf.size(), &err);
    REQUIRE_MESSAGE(preset.has_value(), err);

    CHECK(preset->name == "b)_Basic-5_Size");
    CHECK(preset->provenance.sourceFormat == "kpp");
    CHECK(preset->provenance.sourcePaintop == "paintbrush");
    CHECK(preset->provenance.fidelity == PresetFidelity::Exact);
    CHECK(preset->provenance.droppedOptions.empty());

    CHECK(preset->paintMode == cb::PaintMode::Wash);
    CHECK(preset->blendMode == mosaic::core::BlendMode::Normal);
    CHECK(!preset->eraserMode);
    CHECK(preset->accumulator == cb::StrokeAccumulator::Uniform);
    CHECK(!preset->masking.enabled);
    CHECK(!preset->airbrush.enabled);

    // The tip, exactly as the file spells it: a hard 40 px default circle, auto-spacing 0.8.
    CHECK(preset->tip.kind == TipXml::Kind::Auto);
    CHECK(preset->tip.spacing == 0.1);
    CHECK(preset->tip.useAutoSpacing);
    CHECK(preset->tip.autoSpacingCoeff == 0.8);
    const cb::MaskGeneratorParams& g = preset->tip.autoTip.generator;
    CHECK(g.diameter == 40.0);
    CHECK(g.hFade == 1.0);
    CHECK(g.falloff == cb::MaskFalloff::Default);
    CHECK(g.antialiasEdges);

    // PressureSize on, driven by a pressure sensor; the always-on pair reads as active with its
    // dead bit ignored.
    CHECK(preset->optionActive("Size"));
    REQUIRE(preset->option("Size") != nullptr);
    CHECK(preset->option("Size")->sensors.has(cb::SensorId::Pressure));
    CHECK(preset->optionActive("Opacity"));
    CHECK(preset->optionActive("Flow"));
    CHECK(!preset->optionActive("Rotation"));
}

TEST_CASE("kpp: Pencil-3 -- a tEXt container painting BUILDUP, with its texture honoured") {
    const auto buf = fixture("c)_Pencil-3_Large_4B.kpp");
    std::string err;
    const auto preset = readKpp(buf.data(), buf.size(), &err);
    REQUIRE_MESSAGE(preset.has_value(), err);

    // The container is one of the 35 tEXt presets: reading it at all pins the keyword rule
    // end-to-end.
    CHECK(preset->paintMode == cb::PaintMode::Buildup);
    CHECK(preset->tip.autoTip.generator.falloff == cb::MaskFalloff::Gauss);
    CHECK(preset->tip.autoTip.generator.diameter == 10.0);
    CHECK(preset->tip.autoTip.generator.ratio == 0.9);
    CHECK(preset->tip.angle == doctest::Approx(1.5708));

    // Texture/Pattern/Enabled is true, and since §6.6h the whole block is read and honoured -- so
    // this preset stops being badged for it. ⚠ It is BUILDUP, and it is still Exact, because its
    // `OpacityUseCurve` is FALSE: the Buildup-Opacity caveat needs a DYNAMIC opacity, which this one
    // does not have. (Its sibling c)_Pencil-5_Tilted does, and keeps the caveat.)
    CHECK_FALSE(hasDrop(*preset, "Texture"));
    CHECK(preset->provenance.fidelity == PresetFidelity::Exact);
    CHECK(preset->provenance.droppedOptions.empty());

    // The texture block, read name for name off the raw XML rather than from the reader's defaults.
    // The pattern payload stays base64 here -- decoding and baking it is the LIBRARY's job, exactly
    // as a predefined tip's pixels are (test_brush_texture.cpp takes it from there).
    CHECK(preset->texture.enabled);
    CHECK(preset->texture.mode == cb::TexturingMode::Subtract); // TexturingMode = 1
    CHECK(preset->texture.bake.scale == doctest::Approx(0.1));
    CHECK(preset->texture.bake.brightness == doctest::Approx(0.3));
    CHECK(preset->texture.bake.contrast == doctest::Approx(1.0));
    CHECK(preset->texture.bake.neutralPoint == doctest::Approx(0.5)); // absent -> the default
    CHECK_FALSE(preset->texture.bake.invert);
    CHECK(preset->texture.bake.cutoffPolicy == 0);
    CHECK(preset->texture.bake.cutoffRight == 254);
    CHECK(preset->texture.randomOffsetX);
    CHECK(preset->texture.randomOffsetY);
    CHECK_FALSE(preset->texture.softTexturing); // absent -> false
    CHECK(preset->texture.patternName == "10_drawed_dotted.png");
    CHECK_FALSE(preset->texture.patternBase64.empty());
    // ⚠ AND `Texture/Pattern/Strength` IS NOT READ. The file carries it (19 of the 21 shipped
    // texture presets do) and Krita 6 does not: the strength comes from the `Texture/Strength/`
    // CURVE option, and reading the dead Krita-2 key as well would apply it twice.
    REQUIRE(preset->option("Texture/Strength/") != nullptr);
    CHECK(preset->option("Texture/Strength/")->checked);
}

TEST_CASE("kpp: Eraser_Circle -- CompositeOp 'erase' is a stroke mode, not a blend mode") {
    const auto buf = fixture("a)_Eraser_Circle.kpp");
    const auto preset = readKpp(buf.data(), buf.size());
    REQUIRE(preset.has_value());

    CHECK(preset->eraserMode);
    CHECK(preset->compositeOpId == "erase");
    CHECK(preset->blendMode == mosaic::core::BlendMode::Normal); // untouched by "erase"
    CHECK(preset->provenance.fidelity == PresetFidelity::Exact); // native, nothing dropped
    CHECK(preset->provenance.droppedOptions.empty());
}

TEST_CASE("kpp: Charcoal_Pencil_Medium -- the masking brush, resolved against the master size") {
    const auto buf = fixture("h)_Charcoal_Pencil_Medium.kpp");
    const auto preset = readKpp(buf.data(), buf.size());
    REQUIRE(preset.has_value());

    const MaskingImport& m = preset->masking;
    CHECK(m.enabled);
    CHECK(m.op == cb::MaskingOp::LinearDodge);
    CHECK(!m.unknownOp);
    CHECK(m.useMasterSize);
    CHECK(m.masterSizeCoeff == doctest::Approx(0.45454545));
    CHECK(m.tip.kind == TipXml::Kind::Auto);

    // The primary is a 12 px gauss tip; resolution turns the coupling into document px.
    const double master = preset->tip.autoTip.generator.diameter;
    CHECK(master == 12.0);
    const cb::MaskingParams params = resolveMasking(m, master);
    CHECK(params.enabled);
    CHECK(params.op == cb::MaskingOp::LinearDodge);
    CHECK(params.diameter == doctest::Approx(12.0 * 0.45454545));
    CHECK(params.hardness == doctest::Approx(0.5)); // the nested tip's symmetric 0.5 fades

    // An AUTO nested tip builds its REAL generator at map time -- the masking walk stamps it, not
    // the analytic disc the hardness above parameterizes (that is the null-tip fallback only).
    REQUIRE(params.tip != nullptr);
    CHECK(params.tip->isProcedural());
    REQUIRE(params.tip->generator() != nullptr);
    CHECK(params.tip->generator()->hFade == doctest::Approx(0.5));
    CHECK(params.ratio == 1.0);
    CHECK(params.angleRad == 0.0);
}

TEST_CASE("kpp: the icon decodes independently of the preset") {
    const auto buf = fixture("b)_Basic-5_Size_default.kpp");
    std::string err;
    const auto icon = readKppIcon(buf.data(), buf.size(), &err);
    REQUIRE_MESSAGE(icon.has_value(), err);
    CHECK(icon->width == 200);
    CHECK(icon->height == 200);
}

// ------------------------------------------------------------------------------------------------
// Container gates.

TEST_CASE("kpp: the version gate is strict -- 2.2 and 5.0 only, absent included") {
    std::string err;

    auto ok = syntheticKpp("2.2", kMinimalPreset);
    CHECK(readKpp(ok.data(), ok.size(), &err).has_value());
    auto v5 = syntheticKpp("5.0", kMinimalPreset);
    CHECK(readKpp(v5.data(), v5.size(), &err).has_value());

    auto v6 = syntheticKpp("6.0", kMinimalPreset);
    CHECK(!readKpp(v6.data(), v6.size(), &err).has_value());
    CHECK(err.find("6.0") != std::string::npos);

    auto missing = syntheticKpp("", kMinimalPreset, /*withVersion=*/false);
    CHECK(!readKpp(missing.data(), missing.size(), &err).has_value());

    auto noPreset = syntheticKpp("2.2", "", /*withVersion=*/true, /*withPreset=*/false);
    CHECK(!readKpp(noPreset.data(), noPreset.size(), &err).has_value());
    CHECK(err.find("preset") != std::string::npos);
}

// ------------------------------------------------------------------------------------------------
// The mapper's tier / fallback matrix, on synthetic documents.

TEST_CASE("mapper: the conformance tiers of §6.4") {
    // Legacy `eraser` is the pixel brush plus EraserMode, at Exact fidelity.
    auto eraser = mapXml(R"(<Preset name="e" paintopid="eraser"></Preset>)");
    CHECK(eraser.eraserMode);
    // The PAINTOP costs nothing -- which is the tier claim. (Its fidelity is Approximated all the
    // same, on the standing dynamic-Opacity note every fixture here carries; see otherDrops.)
    CHECK(otherDrops(eraser).empty());
    CHECK(eraser.provenance.sourcePaintop == "eraser");
    CHECK(eraser.paintMode == cb::PaintMode::Wash); // the absent-PaintOpAction default

    // colorsmudge maps to the REAL smudge engine (§6.6c): Exact until something actually drops.
    auto smudge = mapXml(R"(<Preset name="s" paintopid="colorsmudge"></Preset>)");
    CHECK(smudge.provenance.fidelity == PresetFidelity::Exact);
    CHECK(smudge.smudge.enabled);
    CHECK_FALSE(smudge.smudge.dulling); // absent SmudgeRateMode defaults to smearing
    CHECK(smudge.smudge.maxSmudgeRate == 1.0);
    CHECK(otherDrops(smudge).empty());

    // Dulling mode, and the SmudgeRadius PERCENT migration: version < 2 (absent) divides the RAW
    // strength by 100 and caps at the old engine's 3.0 -- clamping before dividing would turn an
    // authored 300 into 0.03 instead of 3.0, which is why the order is pinned here.
    auto dulling = mapXml(R"(<Preset name="d" paintopid="colorsmudge">)"
                          R"(<param name="SmudgeRateMode" type="internal">1</param>)"
                          R"(<param name="PressureSmudgeRadius" type="internal">true</param>)"
                          R"(<param name="SmudgeRadiusValue" type="internal">300</param>)"
                          R"(</Preset>)");
    CHECK(dulling.smudge.dulling);
    {
        const cb::CurveOptionData* rad = dulling.option("SmudgeRadius");
        REQUIRE(rad != nullptr);
        CHECK(rad->strength == doctest::Approx(3.0));
    }

    // The legacy transcription's caveats BADGE rather than silently absorb: overlay mode reads
    // the merged image, and the engine smudges this layer only.
    auto overlay = mapXml(R"(<Preset name="o" paintopid="colorsmudge">)"
                          R"(<param name="MergedPaint" type="internal">true</param>)"
                          R"(</Preset>)");
    CHECK(overlay.provenance.fidelity == PresetFidelity::Approximated);
    CHECK(hasDrop(overlay, "Overlay"));
    CHECK(overlay.smudge.enabled); // it still smears -- this layer only

    // ...and the smudge trio active on a PIXEL paintop is dropped BADGE-FREE (§6.6i), the shape
    // Mirror-on-colorsmudge already had: the reference's pixel paintop constructs no smudge option
    // either, so the key reaches nothing there and the stroke Mosaic paints IS the reference's.
    // Badging it would report a divergence that does not exist.
    auto pixelTrio = mapXml(R"(<Preset name="p" paintopid="paintbrush">)"
                            R"(<param name="PressureSmudgeRate" type="internal">true</param>)"
                            R"(</Preset>)");
    CHECK(pixelTrio.provenance.fidelity == PresetFidelity::Exact);
    CHECK_FALSE(hasDrop(pixelTrio, "SmudgeRate"));
    CHECK_FALSE(pixelTrio.smudge.enabled);

    // A paintop with no engine at all still lands as a usable pixel brush, honestly badged.
    // ⚠ This case needs a paintop that is GENUINELY unsupported: `particlebrush` stood here until
    // its painter landed (§6.6g), at which point it started importing Exact and the case was
    // asserting nothing. `deformbrush` is a §6.6b Tier-2 "really a tool" family with no engine yet.
    auto foreign = mapXml(R"(<Preset name="f" paintopid="deformbrush"></Preset>)", /*withTip=*/false);
    CHECK(foreign.provenance.fidelity == PresetFidelity::Substituted);
    CHECK(hasDrop(foreign, "deformbrush"));
    // ...and it still leaves with a usable tip.
    CHECK(foreign.tip.kind == TipXml::Kind::Auto);
    CHECK(foreign.tip.autoTip.generator.diameter > 0.0);
}

TEST_CASE("mapper: roundmarker synthesizes its hard round tip from flat properties") {
    auto marker = mapXml(R"(<Preset name="m" paintopid="roundmarker">
        <param name="diameter" type="internal">42</param>
        <param name="spacing" type="internal">0.04</param>
    </Preset>)", /*withTip=*/false);
    CHECK(otherDrops(marker).empty()); // the synthesized tip costs nothing
    CHECK(marker.tip.kind == TipXml::Kind::Auto);
    CHECK(marker.tip.autoTip.generator.diameter == 42.0);
    CHECK(marker.tip.autoTip.generator.hFade == 1.0);
    CHECK(marker.tip.spacing == 0.04);

    // The flat defaults when the file is silent: 30 px, spacing 0.02.
    auto bare = mapXml(R"(<Preset name="m" paintopid="roundmarker"></Preset>)", /*withTip=*/false);
    CHECK(bare.tip.autoTip.generator.diameter == 30.0);
    CHECK(bare.tip.spacing == 0.02);
}

TEST_CASE("mapper: an unknown CompositeOp paints Normal and says so") {
    auto p = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="CompositeOp" type="string"><![CDATA[quantum_blend]]></param>
    </Preset>)");
    CHECK(p.blendMode == mosaic::core::BlendMode::Normal);
    CHECK(p.compositeOpId == "quantum_blend");
    CHECK(hasDrop(p, "quantum_blend"));
    CHECK(p.provenance.fidelity == PresetFidelity::Approximated);

    // The one id of the family spelled with a space maps, not drops.
    auto ll = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="CompositeOp" type="string"><![CDATA[linear light]]></param>
    </Preset>)");
    CHECK(ll.blendMode == mosaic::core::BlendMode::LinearLight);
    CHECK(otherDrops(ll).empty());
}

TEST_CASE("mapper: a not-yet-oracled masking op imports as multiply with the note") {
    auto p = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="MaskingBrush/Enabled" type="internal">true</param>
        <param name="MaskingBrush/MaskingCompositeOp" type="string"><![CDATA[hard_mix_photoshop]]></param>
        <param name="MaskingBrush/Preset/brush_definition" type="string"><![CDATA[<Brush type="auto_brush" spacing="0.1"><MaskGenerator diameter="8" hfade="0.7" vfade="0.7"/></Brush>]]></param>
    </Preset>)");
    CHECK(p.masking.enabled);
    CHECK(p.masking.op == cb::MaskingOp::Multiply);
    CHECK(p.masking.unknownOp);
    CHECK(hasDrop(p, "hard_mix_photoshop"));

    // A masking brush with no usable tip disables rather than guesses.
    auto noTip = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="MaskingBrush/Enabled" type="internal">true</param>
    </Preset>)");
    // (the primary tip is spliced in; only the MASKING tip is missing here)
    CHECK(!noTip.masking.enabled);
    CHECK(hasDrop(noTip, "MaskingBrush"));
}

TEST_CASE("mapper: the nested masking table is a PREFIX on the lookup") {
    // MaskingBrush/Preset/PressureSize -- prefix outside, plain names inside (§3.2). The nested
    // Size gate and its pressure sensor arrive through the stripped lookup.
    auto p = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="MaskingBrush/Enabled" type="internal">true</param>
        <param name="MaskingBrush/Preset/brush_definition" type="string"><![CDATA[<Brush type="auto_brush" spacing="0.15" useAutoSpacing="1" autoSpacingCoeff="2"><MaskGenerator diameter="8"/></Brush>]]></param>
        <param name="MaskingBrush/Preset/PressureSize" type="internal">true</param>
        <param name="MaskingBrush/Preset/SizeSensor" type="string"><![CDATA[<params id="pressure"/>]]></param>
        <param name="MaskingBrush/Preset/FlowValue" type="internal">0.6</param>
    </Preset>)");
    REQUIRE(p.masking.enabled);
    CHECK(p.masking.sizeFromPressure);
    CHECK(p.masking.flow == 0.6);

    // The masking walk's own cadence rides its tip element.
    const cb::MaskingParams params = resolveMasking(p.masking, 100.0);
    CHECK(params.spacing == 0.15);
    CHECK(params.useAutoSpacing);
    CHECK(params.autoSpacingCoeff == 2.0);
    CHECK(params.flow == 0.6);
    CHECK(params.sizeFromPressure);
}

TEST_CASE("mapper: resolveMasking caps the resolved diameter") {
    MaskingImport m;
    m.enabled = true;
    m.useMasterSize = true;
    m.masterSizeCoeff = 10.0;
    m.tip.kind = TipXml::Kind::Auto;
    const cb::MaskingParams params = resolveMasking(m, 1000.0);
    CHECK(params.diameter == kMaxMaskingDiameter); // min(15000, 3 x 1000): the upstream cap

    // Without the master coupling, an auto tip's own size governs.
    m.useMasterSize = false;
    m.tip.autoTip.generator.diameter = 17.0;
    CHECK(resolveMasking(m, 1000.0).diameter == 17.0);

    // Disabled resolves to disabled, whatever else is set.
    m.enabled = false;
    CHECK(!resolveMasking(m, 1000.0).enabled);
}

TEST_CASE("mapper: colour dynamics pick the Colored accumulator; h/s/v honoured, Mix/Darken badged") {
    // h/s/v ride the per-dab colour end-to-end now (§6.6f applyColorDynamics), so on a paintbrush
    // they are honoured, fall out of the generic drop, and cost no fidelity.
    auto p = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="Pressureh" type="internal">true</param>
    </Preset>)");
    CHECK(p.accumulator == cb::StrokeAccumulator::Colored);
    CHECK(!hasDrop(p, "h"));
    CHECK(p.provenance.fidelity == PresetFidelity::Exact);

    // Mix / Darken are not transcribed yet, so an active one is still a dropped, badged option
    // (the accumulator is still Colored -- it is a colour dynamic).
    auto mix = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="PressureMix" type="internal">true</param>
    </Preset>)");
    CHECK(mix.accumulator == cb::StrokeAccumulator::Colored);
    CHECK(hasDrop(mix, "Mix"));
    CHECK(mix.provenance.fidelity == PresetFidelity::Approximated);

    auto plain = mapXml(R"(<Preset name="x" paintopid="paintbrush"></Preset>)");
    CHECK(plain.accumulator == cb::StrokeAccumulator::Uniform);
}

TEST_CASE("mapper: airbrush comes from the MODERN spelling only, and costs no fidelity") {
    auto modern = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="PaintOpSettings/isAirbrushing" type="internal">true</param>
        <param name="PaintOpSettings/ignoreSpacing" type="internal">true</param>
    </Preset>)");
    CHECK(modern.airbrush.enabled);
    CHECK(modern.airbrush.rate == 20.0); // the modern default
    CHECK(modern.airbrush.ignoreSpacing);
    // §6.6h: the timed cadence is transcribed, so an airbrushing preset is no longer badged.
    CHECK_FALSE(hasDrop(modern, "Airbrush"));
    CHECK(modern.provenance.fidelity == PresetFidelity::Exact);

    // ⚠⚠ THE KRITA-2-ERA SPELLING IS A DEAD KEY AND IS NOT READ (§6.6i). This case used to assert
    // the opposite -- that the legacy prefix enabled the airbrush at its own default rate of 100 --
    // and honouring it is what froze the program: nothing anywhere in the reference reads the
    // `AirbrushOption/` prefix, so six shipped presets that spell only it (b)_Airbrush_Soft first
    // among them, a 600 px soft nib authored at rate 1000) are plain distance-cadence brushes there
    // and ran a dab-per-millisecond cadence here.
    auto legacy = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="AirbrushOption/isAirbrushing" type="internal">true</param>
        <param name="AirbrushOption/rate" type="internal">1000</param>
    </Preset>)");
    CHECK_FALSE(legacy.airbrush.enabled);
    CHECK(legacy.airbrush.rate == 20.0); // the reader's default, not the file's number
    CHECK_FALSE(legacy.airbrush.ignoreSpacing);
    // ... and it costs no fidelity, because the reference ignores it too.
    CHECK(otherDrops(legacy).empty());
    CHECK(legacy.provenance.fidelity == PresetFidelity::Exact);

    // Disabled airbrush settings are not an approximation.
    auto off = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="PaintOpSettings/isAirbrushing" type="internal">false</param>
        <param name="PaintOpSettings/rate" type="internal">55</param>
    </Preset>)");
    CHECK(!off.airbrush.enabled);
    CHECK(otherDrops(off).empty());
}

TEST_CASE("mapper: the texture block, its two badged paths and its dead keys") {
    // The two modes Mosaic transcribes cost no fidelity.
    auto sub = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="Texture/Pattern/Enabled" type="internal">true</param>
        <param name="Texture/Pattern/Pattern" type="bytearray">aVZCT1J3PT0=</param>
        <param name="Texture/Pattern/TexturingMode" type="internal">1</param>
        <param name="Texture/Pattern/Scale" type="internal">0.5</param>
        <param name="Texture/Pattern/Invert" type="internal">true</param>
    </Preset>)");
    CHECK(sub.texture.enabled);
    CHECK(sub.texture.mode == cb::TexturingMode::Subtract);
    CHECK(sub.texture.bake.scale == doctest::Approx(0.5));
    CHECK(sub.texture.bake.invert);
    CHECK(otherDrops(sub).empty());

    // A mode beyond the two -- 2 is the lightness mode, which needs a per-pixel COLOUR path -- is
    // imported as multiply with a note, exactly as an unimplemented masking-brush op is.
    auto lightness = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="Texture/Pattern/Enabled" type="internal">true</param>
        <param name="Texture/Pattern/Pattern" type="bytearray">aVZCT1J3PT0=</param>
        <param name="Texture/Pattern/TexturingMode" type="internal">2</param>
    </Preset>)");
    CHECK(lightness.texture.mode == cb::TexturingMode::Multiply);
    CHECK(hasDrop(lightness, "Texture mode 2"));
    CHECK(lightness.provenance.fidelity == PresetFidelity::Approximated);

    // A preset that LINKS its pattern rather than embedding it has nothing for the library to
    // resolve, so the whole option is off and says so -- which is what the reference does too when
    // its pattern will not load.
    auto linked = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="Texture/Pattern/Enabled" type="internal">true</param>
        <param name="Texture/Pattern/PatternFileName" type="string"><![CDATA[paper.pat]]></param>
    </Preset>)");
    CHECK_FALSE(linked.texture.enabled);
    CHECK(hasDrop(linked, "Texture (pattern not embedded)"));

    // Texturing OFF reads nothing at all, whatever the block says.
    auto off = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="Texture/Pattern/Enabled" type="internal">false</param>
        <param name="Texture/Pattern/TexturingMode" type="internal">7</param>
    </Preset>)");
    CHECK_FALSE(off.texture.enabled);
    CHECK(otherDrops(off).empty());

    // ⚠ An active `Texture/Strength/` with texturing OFF is a FAITHFUL badge-free drop, not a
    // kindness: the reference reads its strength option only from inside the texture composite,
    // which returns before it when texturing is disabled. Same for `Rate` without an airbrush.
    auto inert = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="PressureTexture/Strength/" type="internal">true</param>
        <param name="PressureRate" type="internal">true</param>
    </Preset>)");
    CHECK(otherDrops(inert).empty());
    CHECK(inert.provenance.fidelity == PresetFidelity::Exact);
}

TEST_CASE("mapper: a non-plain colour source is badged") {
    auto p = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="ColorSource/Type" type="string"><![CDATA[gradient]]></param>
    </Preset>)");
    CHECK(hasDrop(p, "gradient"));

    auto plain = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="ColorSource/Type" type="string"><![CDATA[plain]]></param>
    </Preset>)");
    CHECK(otherDrops(plain).empty());
}

TEST_CASE("mapper: an active option with an unknown sensor is badged by name") {
    auto p = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="PressureSize" type="internal">true</param>
        <param name="SizeSensor" type="string"><![CDATA[<params id="gravity"/>]]></param>
    </Preset>)");
    CHECK(hasDrop(p, "gravity"));

    // The same foreign sensor on an INACTIVE option is not worth a badge.
    auto off = mapXml(R"(<Preset name="x" paintopid="paintbrush">
        <param name="PressureSize" type="internal">false</param>
        <param name="SizeSensor" type="string"><![CDATA[<params id="gravity"/>]]></param>
    </Preset>)");
    CHECK(otherDrops(off).empty());
}

TEST_CASE("mapper: Scatter imports EXACT -- axis gates and the legacy Amount fix-up") {
    // Driven since §6.6d: an active Scatter costs nothing. The axis gates are plain properties
    // beside the option (default true), and the pre-2.x `Scattering/Amount` is the strength when
    // -- and only when -- the modern `ScatterValue` is absent.
    BrushPreset p = mapXml(
        R"(<Preset name="s" paintopid="paintbrush">)"
        R"(<param name="PressureScatter" type="string"><![CDATA[true]]></param>)"
        R"(<param name="Scattering/AxisY" type="string"><![CDATA[false]]></param>)"
        R"(<param name="Scattering/Amount" type="string"><![CDATA[2.5]]></param>)"
        R"(</Preset>)");
    CHECK(p.provenance.fidelity == PresetFidelity::Exact);
    CHECK(p.provenance.droppedOptions.empty());
    CHECK(p.scatterAxisX);      // absent -> the format's own default, true
    CHECK(!p.scatterAxisY);
    REQUIRE(p.option("Scatter") != nullptr);
    CHECK(p.option("Scatter")->checked);
    CHECK(p.option("Scatter")->strength == 2.5); // the fix-up read Amount

    // Both keys present: the modern one wins, the stale Amount is ignored.
    BrushPreset q = mapXml(
        R"(<Preset name="s" paintopid="paintbrush">)"
        R"(<param name="PressureScatter" type="string"><![CDATA[true]]></param>)"
        R"(<param name="ScatterValue" type="string"><![CDATA[1.5]]></param>)"
        R"(<param name="Scattering/Amount" type="string"><![CDATA[2.5]]></param>)"
        R"(</Preset>)");
    CHECK(q.option("Scatter")->strength == 1.5);
}

TEST_CASE("mapper: Mirror imports EXACT -- the axis gates default OFF") {
    BrushPreset p = mapXml(
        R"(<Preset name="m" paintopid="paintbrush">)"
        R"(<param name="PressureMirror" type="string"><![CDATA[true]]></param>)"
        R"(<param name="HorizontalMirrorEnabled" type="string"><![CDATA[true]]></param>)"
        R"(</Preset>)");
    CHECK(p.provenance.fidelity == PresetFidelity::Exact);
    CHECK(p.provenance.droppedOptions.empty());
    CHECK(p.mirrorHorizontal);
    CHECK(!p.mirrorVertical); // absent -> false: an axis is opted INTO, unlike Scatter's
    REQUIRE(p.option("Mirror") != nullptr);
    CHECK(p.option("Mirror")->checked);
}
