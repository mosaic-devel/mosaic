#include "io/brush/preset_xml.hpp"

#include <doctest/doctest.h>

#include <string>

// The preset XML layer (io/brush/preset_xml.hpp): the <Preset> property table and the <Brush> tip
// element. The traps here are the ones docs/brushes.md §3.2/§3.4/§3.5 pins against shipped files
// -- scrambled attribute order, the two spacing defaults, radius-as-synonym, BrushVersion's
// doubling default, the adjustment migration, and the four-branch application rule -- plus the
// hostile-input caps. Test inputs reproduce the real corpus shapes (type= before name=, CDATA
// values, the <MaskGenerator> child) rather than idealized ones.
namespace {

using namespace mosaic::io::brush;
namespace cb = mosaic::core::brush;

} // namespace

TEST_CASE("preset_xml: a corpus-shaped document parses -- scrambled attributes, CDATA, types") {
    // Attribute order is type-first in 23545 of the corpus's 28152 params; a name-first regex
    // mis-parses 64 of 82 presets (§3.2). CDATA and plain text must read identically.
    const auto preset = parsePresetXml(R"(<Preset paintopid="paintbrush" name="walker test">
        <param type="string" name="CompositeOp"><![CDATA[normal]]></param>
        <param name="OpacityValue" type="internal">0.85</param>
        <param type="internal" name="EraserMode">false</param>
        <param type="bytearray" name="Texture/Pattern/Pattern">aGVsbG8=</param>
        <param type="color" name="Color">#ff0000</param>
        <param type="string" name="Empty"><![CDATA[]]></param>
        <param type="hologram" name="FromTheFuture">shiny</param>
    </Preset>)");
    REQUIRE(preset.has_value());
    CHECK(preset->name == "walker test");
    CHECK(preset->paintopId == "paintbrush");
    CHECK(preset->params.size() == 7);
    CHECK(preset->skippedParams == 0);
    CHECK(preset->duplicateParams == 0);

    CHECK(preset->property("CompositeOp") == "normal");
    CHECK(preset->property("OpacityValue") == "0.85");
    CHECK(preset->params.at("OpacityValue").type == PresetParam::Type::Internal);
    CHECK(preset->params.at("Texture/Pattern/Pattern").type == PresetParam::Type::ByteArray);
    CHECK(preset->params.at("Color").type == PresetParam::Type::Color);

    // An unknown param TYPE is kept (ignored-not-rejected), and counted for provenance.
    CHECK(preset->unknownParamTypes == 1);
    CHECK(preset->property("FromTheFuture") == "shiny");

    // Absent is nullopt; PRESENT-BUT-EMPTY is an empty string. Option defaults differ on
    // exactly this distinction (§3.2).
    CHECK(preset->property("Empty") == std::string());
    CHECK(!preset->property("NotThere").has_value());
}

TEST_CASE("preset_xml: duplicate params keep the LAST value, counted") {
    const auto preset = parsePresetXml(R"(<Preset name="d" paintopid="paintbrush">
        <param name="K" type="string"><![CDATA[first]]></param>
        <param name="K" type="string"><![CDATA[second]]></param>
    </Preset>)");
    REQUIRE(preset.has_value());
    CHECK(preset->property("K") == "second");
    CHECK(preset->duplicateParams == 1);
}

TEST_CASE("preset_xml: nameless params are skipped and counted, not fatal") {
    const auto preset = parsePresetXml(R"(<Preset name="d" paintopid="p">
        <param type="string"><![CDATA[orphan]]></param>
        <param name="ok" type="string"><![CDATA[v]]></param>
    </Preset>)");
    REQUIRE(preset.has_value());
    CHECK(preset->skippedParams == 1);
    CHECK(preset->property("ok") == "v");
}

TEST_CASE("preset_xml: unusable documents are errors") {
    std::string err;
    CHECK(!parsePresetXml("", &err).has_value());
    CHECK(!parsePresetXml("<Preset name='x'", &err).has_value()); // malformed
    CHECK(!parsePresetXml("<NotAPreset/>", &err).has_value());
    CHECK(err.find("Preset") != std::string::npos);
}

TEST_CASE("preset_xml: v5 embedded resources parse, payload trimmed") {
    const auto preset = parsePresetXml(R"(<Preset name="v5" paintopid="paintbrush">
        <param name="k" type="string"><![CDATA[v]]></param>
        <resources>
            <resource type="brushes" md5sum="abc123" name="tip" filename="tip.gbr">
                aGVsbG8gd29ybGQ=
            </resource>
        </resources>
    </Preset>)");
    REQUIRE(preset.has_value());
    REQUIRE(preset->resources.size() == 1);
    const EmbeddedResource& r = preset->resources[0];
    CHECK(r.type == "brushes");
    CHECK(r.md5sum == "abc123");
    CHECK(r.name == "tip");
    CHECK(r.filename == "tip.gbr");
    CHECK(r.base64 == "aGVsbG8gd29ybGQ="); // whitespace trimmed, payload untouched
}

TEST_CASE("preset_xml: the caps hold -- param count and value size") {
    SUBCASE("param count") {
        std::string xml = R"(<Preset name="big" paintopid="p">)";
        for (int i = 0; i < kMaxPresetParams + 5; ++i)
            xml += "<param name=\"k" + std::to_string(i) + "\" type=\"internal\">1</param>";
        xml += "</Preset>";
        const auto preset = parsePresetXml(xml);
        REQUIRE(preset.has_value());
        CHECK(static_cast<int>(preset->params.size()) == kMaxPresetParams);
        CHECK(preset->skippedParams == 5);
    }
    SUBCASE("single value size") {
        std::string xml = R"(<Preset name="fat" paintopid="p"><param name="huge" type="string">)";
        xml += std::string(kMaxPresetValueBytes + 1, 'x');
        xml += "</param><param name=\"ok\" type=\"internal\">1</param></Preset>";
        const auto preset = parsePresetXml(xml);
        REQUIRE(preset.has_value());
        CHECK(preset->skippedParams == 1);
        CHECK(!preset->property("huge").has_value());
        CHECK(preset->property("ok") == "1");
    }
}

// ------------------------------------------------------------------------------------------------
// <Brush>

TEST_CASE("preset_xml: a real corpus auto_brush element, attribute order and all") {
    // Verbatim from b)_Basic-5_Size_default.kpp's masking-brush definition: type= arrives LAST on
    // the element, and the MaskGenerator scrambles its own attributes too.
    const auto tip = parseTipXml(
        R"(<Brush density="1" spacing="0.1" BrushVersion="2" autoSpacingCoeff="1" angle="0"
                  randomness="0" type="auto_brush" useAutoSpacing="0">
             <MaskGenerator diameter="5" hfade="0.5" antialiasEdges="0" vfade="0.5" id="default"
                  spikes="2" type="circle" ratio="1"/>
           </Brush>)");
    REQUIRE(tip.has_value());
    CHECK(tip->kind == TipXml::Kind::Auto);
    CHECK(tip->type == "auto_brush");
    CHECK(tip->spacing == 0.1);
    CHECK(tip->useAutoSpacing == false);
    CHECK(tip->autoSpacingCoeff == 1.0);
    CHECK(tip->angle == 0.0);
    CHECK(tip->autoTip.density == 1.0);
    CHECK(tip->autoTip.randomness == 0.0);
    const cb::MaskGeneratorParams& g = tip->autoTip.generator;
    CHECK(g.diameter == 5.0);
    CHECK(g.hFade == 0.5);
    CHECK(g.vFade == 0.5);
    CHECK(g.ratio == 1.0);
    CHECK(g.spikes == 2);
    CHECK(g.antialiasEdges == false);
    CHECK(g.shape == cb::MaskShape::Circle);
    CHECK(g.falloff == cb::MaskFalloff::Default);
}

TEST_CASE("preset_xml: the spacing default is 1.0 on auto elements and 0.25 on predefined ones") {
    // The two factories genuinely disagree; §3.5's "absent means 0.25" is the predefined half.
    const auto autoTip = parseTipXml(R"(<Brush type="auto_brush"><MaskGenerator/></Brush>)");
    REQUIRE(autoTip.has_value());
    CHECK(autoTip->spacing == 1.0);

    const auto gbr = parseTipXml(R"(<Brush type="gbr_brush" filename="t.gbr" BrushVersion="2"/>)");
    REQUIRE(gbr.has_value());
    CHECK(gbr->spacing == 0.25);
}

TEST_CASE("preset_xml: legacy radius is a SYNONYM for diameter, not a half of it") {
    // "mistakenly named radius for 2.2" -- same number, different name, and it wins when both
    // are present.
    const auto legacy = parseTipXml(
        R"(<Brush type="auto_brush"><MaskGenerator radius="8" id="default"/></Brush>)");
    REQUIRE(legacy.has_value());
    CHECK(legacy->autoTip.generator.diameter == 8.0);

    const auto both = parseTipXml(
        R"(<Brush type="auto_brush"><MaskGenerator radius="8" diameter="20"/></Brush>)");
    REQUIRE(both.has_value());
    CHECK(both->autoTip.generator.diameter == 8.0);
}

TEST_CASE("preset_xml: BrushVersion defaults to the doubling version") {
    // Absent => "1" => scale x2. Every reference in the default set writes "2", which is why a
    // reader with this backwards does not notice until a third-party preset arrives (§3.5).
    const auto absent = parseTipXml(R"(<Brush type="gbr_brush" filename="t.gbr" scale="1.5"/>)");
    REQUIRE(absent.has_value());
    CHECK(absent->predefined.scale == 3.0);

    const auto v1 = parseTipXml(
        R"(<Brush type="gbr_brush" filename="t.gbr" scale="1.5" BrushVersion="1"/>)");
    CHECK(v1->predefined.scale == 3.0);

    const auto v2 = parseTipXml(
        R"(<Brush type="gbr_brush" filename="t.gbr" scale="1.5" BrushVersion="2"/>)");
    CHECK(v2->predefined.scale == 1.5);
}

TEST_CASE("preset_xml: a soft generator's missing curve degrades to a descending profile") {
    // The identity would build an inside-out tip -- opaque rim, clear centre (§3.4). Present
    // curves are taken verbatim.
    const auto missing = parseTipXml(
        R"(<Brush type="auto_brush"><MaskGenerator id="soft" type="circle"/></Brush>)");
    REQUIRE(missing.has_value());
    const cb::Curve& fallback = missing->autoTip.generator.softnessCurve;
    CHECK(fallback.eval(0.0) == 1.0);
    CHECK(fallback.eval(1.0) == 0.0);

    const auto given = parseTipXml(
        R"(<Brush type="auto_brush">
             <MaskGenerator id="soft" softness_curve="0,1;0.25,0.9;1,0;"/></Brush>)");
    REQUIRE(given.has_value());
    CHECK(given->autoTip.generator.softnessCurve.points().size() == 3);
}

TEST_CASE("preset_xml: an unknown generator id loads as gauss, flagged for provenance") {
    const auto foreign = parseTipXml(
        R"(<Brush type="auto_brush"><MaskGenerator id="quantum" type="rect"/></Brush>)");
    REQUIRE(foreign.has_value());
    CHECK(foreign->autoTip.generator.falloff == cb::MaskFalloff::Gauss);
    CHECK(foreign->autoTip.generator.shape == cb::MaskShape::Rect);
    CHECK(foreign->autoTip.unknownFalloffId);

    const auto gauss = parseTipXml(
        R"(<Brush type="auto_brush"><MaskGenerator id="gauss"/></Brush>)");
    CHECK(gauss->autoTip.generator.falloff == cb::MaskFalloff::Gauss);
    CHECK(!gauss->autoTip.unknownFalloffId);
}

TEST_CASE("preset_xml: the four-branch application rule, in the producer's priority order") {
    using Rule = TipApplicationRule;
    const char* base = R"(<Brush type="png_brush" filename="t.png" BrushVersion="2" %s/>)";
    const auto parse = [&](const char* attrs) {
        char buf[512];
        std::snprintf(buf, sizeof buf, base, attrs);
        auto tip = parseTipXml(buf);
        REQUIRE(tip.has_value());
        return tip->predefined;
    };

    // preserveLightness outranks everything, including an explicit brushApplication.
    auto p = parse(R"(preserveLightness="1" brushApplication="1")");
    CHECK(p.applicationRule == Rule::ForceLightness);

    // preserveLightness=false falls back to the legacy test with ColorAsMask defaulting TRUE.
    p = parse(R"(preserveLightness="0")");
    CHECK(p.applicationRule == Rule::LegacyContentTest);
    CHECK(p.colorAsMask == true);

    p = parse(R"(brushApplication="2")");
    CHECK(p.applicationRule == Rule::Explicit);
    CHECK(p.application == cb::TipApplication::LightnessMap);

    // An out-of-range value is AlphaMask, NOT clamped into GradientMap.
    p = parse(R"(brushApplication="7")");
    CHECK(p.applicationRule == Rule::Explicit);
    CHECK(p.application == cb::TipApplication::AlphaMask);

    p = parse(R"(ColorAsMask="0")");
    CHECK(p.applicationRule == Rule::LegacyContentTest);
    CHECK(p.colorAsMask == false);

    // No attribute at all: the pre-4.4 heuristic -- content decides by itself.
    p = parse("");
    CHECK(p.applicationRule == Rule::LegacyContentTest);
    CHECK(p.colorAsMask == false);
}

TEST_CASE("preset_xml: the adjustment migration doubles version-1 values") {
    // AdjustmentVersion < 2 with no AutoAdjustMidPoint attribute: the old renderer applied the
    // curve twice, and the blunt undo doubles -- midpoint about 127 (clamped), brightness and
    // contrast outright, negative contrast through the changed formula (§3.5).
    const auto migrated = parseTipXml(
        R"(<Brush type="gbr_brush" filename="t.gbr" BrushVersion="2"
                  AdjustmentMidPoint="100" BrightnessAdjustment="0.5" ContrastAdjustment="-0.5"/>)");
    REQUIRE(migrated.has_value());
    const cb::TipAdjustments& adj = migrated->predefined.adjustments;
    CHECK(adj.midPoint == 73.0);   // 127 + (100-127)*2
    CHECK(adj.brightness == 1.0);  // 0.5 * 2, deliberately unclamped
    CHECK(adj.contrast == -0.5);   // -0.5*2 = -1, then 1/(1-(-1)) - 1
    CHECK(!adj.neutral());

    // The presence of AutoAdjustMidPoint -- even as "0" -- means Krita 5 wrote it: no migration.
    const auto modern = parseTipXml(
        R"(<Brush type="gbr_brush" filename="t.gbr" BrushVersion="2" AutoAdjustMidPoint="0"
                  AdjustmentMidPoint="100" BrightnessAdjustment="0.5" ContrastAdjustment="-0.5"/>)");
    CHECK(modern->predefined.adjustments.midPoint == 100.0);
    CHECK(modern->predefined.adjustments.brightness == 0.5);
    CHECK(modern->predefined.adjustments.contrast == -0.5);

    // So does AdjustmentVersion 2.
    const auto v2 = parseTipXml(
        R"(<Brush type="gbr_brush" filename="t.gbr" BrushVersion="2" AdjustmentVersion="2"
                  AdjustmentMidPoint="100"/>)");
    CHECK(v2->predefined.adjustments.midPoint == 100.0);

    // Neutral values migrate to neutral: the whole default set is untouched.
    const auto neutral = parseTipXml(R"(<Brush type="gbr_brush" filename="t.gbr"/>)");
    CHECK(neutral->predefined.adjustments.neutral());
}

TEST_CASE("preset_xml: svg tips are not colour-capable -- adjustments are not even read") {
    const auto svg = parseTipXml(
        R"(<Brush type="svg_brush" filename="t.svg" BrushVersion="2"
                  AdjustmentMidPoint="100" BrightnessAdjustment="0.9"/>)");
    REQUIRE(svg.has_value());
    CHECK(!svg->predefined.colorfulCapable);
    CHECK(svg->predefined.adjustments.neutral()); // the producer ignores them; so do we

    const auto png = parseTipXml(R"(<Brush type="png_brush" filename="t.png" BrushVersion="2"/>)");
    CHECK(png->predefined.colorfulCapable);
}

TEST_CASE("preset_xml: an unknown brush type still yields the common attributes") {
    // The mapper substitutes and reports (§6.4); the parser must not drop the element.
    const auto tip = parseTipXml(R"(<Brush type="quantum_brush" spacing="0.5" angle="1.5"/>)");
    REQUIRE(tip.has_value());
    CHECK(tip->kind == TipXml::Kind::Unknown);
    CHECK(tip->type == "quantum_brush");
    CHECK(tip->spacing == 0.5);
    CHECK(tip->angle == 1.5);
}

TEST_CASE("preset_xml: a missing MaskGenerator child leaves the generator at its defaults") {
    const auto tip = parseTipXml(R"(<Brush type="auto_brush"/>)");
    REQUIRE(tip.has_value());
    CHECK(tip->autoTip.generator.diameter == 1.0);
    CHECK(tip->autoTip.generator.ratio == 1.0);
    CHECK(tip->autoTip.generator.hFade == 0.0);
    CHECK(tip->autoTip.generator.antialiasEdges == false);
}

TEST_CASE("preset_xml: no <Brush> element is an error") {
    std::string err;
    CHECK(!parseTipXml("<NotABrush/>", &err).has_value());
    CHECK(err.find("Brush") != std::string::npos);
    CHECK(!parseTipXml("", &err).has_value());
}
