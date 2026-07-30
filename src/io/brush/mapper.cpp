#include "io/brush/mapper.hpp"

#include "core/brush/brush_tip.hpp" // makeTip -- the auto masking tip builds at map time
#include "core/brush/parse_util.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace mosaic::io::brush {
namespace {

namespace cb = mosaic::core::brush;

// ------------------------------------------------------------------------------------------------
// Property lookups over the parsed table.

[[nodiscard]] cb::PropertyLookup lookupOf(const PresetXml& xml) {
    return [&xml](std::string_view key) { return xml.property(key); };
}

// The MaskingBrush/Preset/ nested table (§3.2): a PREFIX on the lookup, not on the base names --
// the enable key is MaskingBrush/Preset/PressureSize, prefix outside. (Contrast Texture/Strength/,
// which is a base name CONTAINING slashes: enable key PressureTexture/Strength/.)
[[nodiscard]] cb::PropertyLookup maskingLookupOf(const PresetXml& xml) {
    return [&xml](std::string_view key) {
        return xml.property("MaskingBrush/Preset/" + std::string(key));
    };
}

[[nodiscard]] bool boolProperty(const PresetXml& xml, std::string_view key, bool fallback) {
    const std::optional<std::string> v = xml.property(key);
    if (!v)
        return fallback;
    bool out = false;
    return cb::detail::parseBool(*v, out) ? out : fallback;
}

[[nodiscard]] double doubleProperty(const PresetXml& xml, std::string_view key, double fallback) {
    const std::optional<std::string> v = xml.property(key);
    if (!v)
        return fallback;
    double out = 0.0;
    return cb::detail::parseDouble(*v, out) ? out : fallback;
}

[[nodiscard]] long long intProperty(const PresetXml& xml, std::string_view key,
                                    long long fallback) {
    const std::optional<std::string> v = xml.property(key);
    if (!v)
        return fallback;
    long long out = 0;
    return cb::detail::parseLongLong(*v, out) ? out : fallback;
}

// ------------------------------------------------------------------------------------------------
// CompositeOp -> BlendMode. The ids are the producer's registry strings, spelled exactly --
// including "linear light", the one id of the family written with a SPACE. "erase" is not a blend
// mode here: it becomes eraserMode (destination-out) at the call site.
[[nodiscard]] std::optional<core::BlendMode> blendModeFromCompositeOp(std::string_view id) {
    using core::BlendMode;
    static constexpr std::pair<std::string_view, BlendMode> kMap[] = {
        {"normal", BlendMode::Normal},
        {"darken", BlendMode::Darken},
        {"multiply", BlendMode::Multiply},
        {"burn", BlendMode::ColorBurn}, // "also known as 'color burn'"
        {"linear_burn", BlendMode::LinearBurn},
        {"lighten", BlendMode::Lighten},
        {"screen", BlendMode::Screen},
        {"dodge", BlendMode::ColorDodge},
        {"linear_dodge", BlendMode::LinearDodge},
        {"add", BlendMode::LinearDodge}, // the a.k.a., a distinct id upstream
        {"overlay", BlendMode::Overlay},
        {"soft_light", BlendMode::SoftLight},     // the Photoshop variant owns the plain id
        {"soft_light_svg", BlendMode::SoftLight}, // approximated: Mosaic has one SoftLight
        {"hard_light", BlendMode::HardLight},
        {"vivid_light", BlendMode::VividLight},
        {"linear light", BlendMode::LinearLight}, // sic: the space is the format
        {"pin_light", BlendMode::PinLight},
        {"diff", BlendMode::Difference},
        {"exclusion", BlendMode::Exclusion},
        {"subtract", BlendMode::Subtract},
        {"divide", BlendMode::Divide},
        {"hue", BlendMode::Hue},
        {"saturation", BlendMode::Saturation},
        {"color", BlendMode::Color},
        {"luminize", BlendMode::Luminosity},
    };
    for (const auto& [key, mode] : kMap)
        if (key == id)
            return mode;
    return std::nullopt;
}

// The masking op ids the ENGINE implements (§6.2): the format's default plus everything the
// shipped set uses. The other seven known ids and any foreign one import as multiply with a
// fidelity note, until their transcribed-oracle pass.
[[nodiscard]] std::optional<cb::MaskingOp> maskingOpFromId(std::string_view id) {
    if (id == "multiply")
        return cb::MaskingOp::Multiply;
    if (id == "subtract")
        return cb::MaskingOp::Subtract;
    if (id == "linear_dodge")
        return cb::MaskingOp::LinearDodge;
    return std::nullopt;
}

// ------------------------------------------------------------------------------------------------
// The option-family table (declared in mapper.hpp; read off the consumers: everything is
// Checkable over [0,1] except the two always-on options, Softness's [0.1,1] floor and Scatter's
// [0,5] span).
constexpr std::array<OptionSpec, 29> kPixelBrushOptions = {{
    {"Opacity", /*checkable=*/false, 0.0, 1.0},
    {"Flow", /*checkable=*/false, 0.0, 1.0},
    {"Size", true, 0.0, 1.0},
    {"Ratio", true, 0.0, 1.0},
    {"Softness", true, 0.1, 1.0},
    {"Rotation", true, 0.0, 1.0},
    {"Sharpness", true, 0.0, 1.0},
    {"LightnessStrength", true, 0.0, 1.0},
    {"Scatter", true, 0.0, 5.0},
    {"Darken", true, 0.0, 1.0},
    {"Mix", true, 0.0, 1.0},
    {"Rate", true, 0.0, 1.0},
    {"Mirror", true, 0.0, 1.0},
    {"Spacing", true, 0.0, 1.0},
    {"h", true, 0.0, 1.0},
    {"s", true, 0.0, 1.0},
    {"v", true, 0.0, 1.0},
    {"Texture/Strength/", true, 0.0, 1.0},
    // The smudge trio (§6.6c). Only colorsmudge presets author them, but the read is generic --
    // an absent option parses to unchecked-with-default, contributing nothing. SmudgeRadius'
    // [0,3] range is the OLD-engine consumer's (every shipped preset), applied AFTER the /100
    // version migration below.
    {"SmudgeRate", true, 0.0, 1.0},
    {"ColorRate", true, 0.0, 1.0},
    {"SmudgeRadius", true, 0.0, 3.0},
    // The sketch engine's trio (§6.6g). Same shape as the smudge trio: read generically for every
    // preset (an absent option parses to unchecked-with-default and contributes nothing), honoured
    // by ONE consumer, badged everywhere else. ⚠ Two of the names carry a SPACE -- the file's keys
    // really are `PressureLine width` / `Line widthValue`.
    {"Density", true, 0.0, 1.0},
    {"Line width", true, 0.0, 1.0},
    {"Offset scale", true, 0.0, 1.0},
    // The curve engine's own opacity (§6.6g). ⚠ `Line width` is SHARED with the sketch engine and
    // the two consumers impose different ranges upstream ([0,1] there, [0.1,1] here); one base, one
    // spec, and the wider range wins -- recorded in §6.6g, inert on the shipped set.
    {"Curves opacity", true, 0.0, 1.0},
    // The hatching engine's four (§6.6g). Read generically like every other family; honoured only
    // when HatchingParams::enabled, badged anywhere else.
    {"Angle", true, 0.0, 1.0},
    {"Crosshatching", true, 0.0, 1.0},
    {"Separation", true, 0.0, 1.0},
    {"Thickness", true, 0.0, 1.0},
}};

// Option families the dab pipeline evaluates END-TO-END (core/brush/dab.hpp's BrushOptions). An
// ACTIVE option outside this set is imported -- the data is all there, and the editor will show it --
// but it is listed in droppedOptions and the fidelity drops: the badge tells the user the stroke will
// not fully match, which beats silently painting something else.
//
// ⚠ THIS LIST IS A CLAIM ABOUT THE ENGINE AND IT MUST NOT RUN AHEAD OF ONE. It named `Scatter`,
// `Mirror` and `Spacing` from the day it was written until 2026-07-12 -- three options no dab has
// ever read -- so presets carrying them imported as EXACT and then painted without them, which is the
// one thing §6.4's honesty contract exists to prevent. Add a base here when a dab reads it, not when
// a plan says it will.
[[nodiscard]] bool optionSupported(std::string_view base) {
    return core::brush::drivenOption(base) != nullptr; // core/brush/dab.hpp: kDrivenOptions
}

// (`optionIsDynamic` -- does an option's value MOVE with the stroke? -- lives in
// core/brush/curve_option.hpp now: the engine's dynamic-opacity gate reads the very same predicate,
// so "honoured" and "driven" cannot drift apart.)

[[nodiscard]] bool isColorDynamicsBase(std::string_view base) {
    return base == "h" || base == "s" || base == "v" || base == "Mix" || base == "Darken";
}

// Local spellings of the shared provenance edits (declared in preset.hpp, defined below): the
// mapper's call sites read better short.
void addDrop(BrushPreset& preset, std::string note) {
    addDroppedOption(preset, std::move(note));
}

void degradeToAtLeast(BrushPreset& preset, PresetFidelity floor) {
    degradeFidelity(preset, floor);
}

// ------------------------------------------------------------------------------------------------

void readAirbrush(const PresetXml& xml, BrushPreset& preset) {
    // ⚠⚠ `AirbrushOption/*` IS A DEAD KEY, AND HONOURING IT WAS A USER-REPORTED HANG (§6.6h).
    // The reference's airbrush reader reads three keys and nothing else -- `isAirbrushing` (default
    // false), `rate` (default 20) and `ignoreSpacing` (default false), all under `PaintOpSettings/`
    // -- and there is no reader anywhere in it for the Krita-2-era `AirbrushOption/` prefix. So a
    // file carrying only those does NOT airbrush upstream, and six presets in the shipped set carry
    // only those: `b)_Airbrush_Soft`, `e)_Marker_Details` and four `l)_Adjust_*`.
    //
    // Reading them turned `b)_Airbrush_Soft` -- a **600 px** soft nib authored at rate **1000**,
    // the fastest the reference's own slider allows -- into a dab every millisecond, i.e. ~360
    // million pixel deposits a second on the UI thread, laying a stroke as solid as a marker's.
    // That is what "airbrushing freezes the program and the strokes come out absurdly thick" was.
    // The engine's per-span time budget bounds the loop; nothing can bound a cadence the preset was
    // never meant to run at all.
    preset.airbrush.enabled = boolProperty(xml, "PaintOpSettings/isAirbrushing", false);
    preset.airbrush.rate = doubleProperty(xml, "PaintOpSettings/rate", 20.0);
    preset.airbrush.ignoreSpacing = boolProperty(xml, "PaintOpSettings/ignoreSpacing", false);
    // ⚠ NOTHING HERE COSTS FIDELITY, in either direction. The timed cadence is transcribed
    // end-to-end (brush_engine.cpp's walkSpan) and the per-dab `Rate` option rides the option table
    // like any other, so a preset that DOES airbrush is honoured; and ignoring a key the reference
    // also ignores means a preset that carries only the dead spelling paints exactly the stroke it
    // paints there -- the badge-free shape Mirror-on-colorsmudge has (dab.hpp), not a kindness. The
    // one piece that is NOT the engine's -- synthesizing a sample while the pointer is held still --
    // is the reference's TOOL half too, and it makes no difference to what a given stream paints.
}

// The TEXTURE option (§6.6h), read name for name with the reference reader's own defaults. The
// pattern payload stays base64 here: decoding + baking it is the library's job, exactly as a
// predefined tip's pixels are.
void readTexture(const PresetXml& xml, BrushPreset& preset) {
    if (!boolProperty(xml, "Texture/Pattern/Enabled", false))
        return;

    TextureImport& t = preset.texture;
    // The base64 payload the XML layer leaves encoded. ⚠ A `bytearray` param is base64 in the FILE
    // and its decoded content is the producer's OWN base64 string, so the pattern's bytes are two
    // decodes down; the library does both.
    t.patternBase64 = xml.property("Texture/Pattern/Pattern").value_or(std::string{});
    t.patternName = xml.property("Texture/Pattern/Name").value_or(std::string{});
    if (t.patternBase64.empty()) {
        // The reference disables the whole option when its pattern will not load; a preset that
        // links a pattern rather than embedding it has nothing here for us to resolve.
        addDrop(preset, "Texture (pattern not embedded)");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
        return;
    }

    t.bake.scale = doubleProperty(xml, "Texture/Pattern/Scale", 1.0);
    t.bake.brightness = doubleProperty(xml, "Texture/Pattern/Brightness", 0.0);
    t.bake.contrast = doubleProperty(xml, "Texture/Pattern/Contrast", 1.0);
    t.bake.neutralPoint = doubleProperty(xml, "Texture/Pattern/NeutralPoint", 0.5);
    t.bake.invert = boolProperty(xml, "Texture/Pattern/Invert", false);
    t.bake.cutoffPolicy = static_cast<int>(intProperty(xml, "Texture/Pattern/CutoffPolicy", 0));
    t.bake.cutoffLeft = static_cast<int>(intProperty(xml, "Texture/Pattern/CutoffLeft", 0));
    t.bake.cutoffRight = static_cast<int>(intProperty(xml, "Texture/Pattern/CutoffRight", 255));
    t.offsetX = static_cast<int>(intProperty(xml, "Texture/Pattern/OffsetX", 0));
    t.offsetY = static_cast<int>(intProperty(xml, "Texture/Pattern/OffsetY", 0));
    t.randomOffsetX = boolProperty(xml, "Texture/Pattern/isRandomOffsetX", false);
    t.randomOffsetY = boolProperty(xml, "Texture/Pattern/isRandomOffsetY", false);
    t.softTexturing = boolProperty(xml, "Texture/Pattern/UseSoftTexturing", false);
    t.enabled = true;

    // ⚠ `Texture/Pattern/MaximumOffsetX`/`Y` are DELIBERATELY NOT READ -- the reference's own
    // reader skips them with a note that its widget recomputes them from the loaded pattern, so a
    // file's stored pair is stale data, not a setting. The random offset spans the pattern's real
    // dimensions instead.
    //
    // ⚠ `Texture/Pattern/Strength` is a DEAD Krita-2 key. Krita 6 reads the strength from the
    // `Texture/Strength/` curve option (the one with slashes in its base name), and 19 of the 21
    // shipped texture presets still carry the old key. Reading it would apply the strength twice.

    // The texturing mode. Mosaic implements the format's default (multiply) plus the only other
    // one the shipped set uses (subtract); the other fourteen import as multiply with a note --
    // the same treatment, for the same reason, that the masking brush's seven unimplemented ops
    // get. Four of them (lightness, gradient and the two height families) modify the dab's COLOUR
    // rather than its alpha and are a different mechanism, not a different constant.
    const int mode = static_cast<int>(intProperty(xml, "Texture/Pattern/TexturingMode", 0));
    if (mode == 0) {
        t.mode = cb::TexturingMode::Multiply;
    } else if (mode == 1) {
        t.mode = cb::TexturingMode::Subtract;
    } else {
        t.mode = cb::TexturingMode::Multiply;
        addDrop(preset, "Texture mode " + std::to_string(mode) + " (textured as multiply)");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }
}

void readMasking(const PresetXml& xml, BrushPreset& preset) {
    if (!boolProperty(xml, "MaskingBrush/Enabled", false))
        return;

    MaskingImport& m = preset.masking;
    m.opId = xml.property("MaskingBrush/MaskingCompositeOp").value_or("multiply");
    if (const std::optional<cb::MaskingOp> op = maskingOpFromId(m.opId)) {
        m.op = *op;
    } else {
        // Both the seven not-yet-oracled ids and a foreign one: multiply, with the note. The
        // reference's own unknown-id fallback is multiply too, so this is faithful for the
        // foreign case and honest for the known-but-unimplemented one.
        m.op = cb::MaskingOp::Multiply;
        m.unknownOp = true;
        addDrop(preset, "MaskingBrush op '" + m.opId + "'");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }
    m.useMasterSize = boolProperty(xml, "MaskingBrush/UseMasterSize", true);
    if (m.useMasterSize)
        m.masterSizeCoeff = doubleProperty(xml, "MaskingBrush/MasterSizeCoeff", 1.0);

    const std::optional<std::string> def = xml.property("MaskingBrush/Preset/brush_definition");
    std::optional<TipXml> tip = def ? parseTipXml(*def) : std::nullopt;
    if (!tip) {
        // A masking brush with no tip cannot walk. Disable rather than guess a tip.
        m = MaskingImport{};
        addDrop(preset, "MaskingBrush (no usable tip)");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
        return;
    }
    m.tip = std::move(*tip);
    m.enabled = true;

    // The nested table's own option gates, through the prefix-stripped lookup. The gates, the flow
    // strength and the TIP survive into MaskingParams (an Auto tip builds in resolveMasking; a
    // Predefined one resolves in the library, against the bundle's tip resources) -- the nested
    // table's other per-dab options (Rotation, Scatter, Opacity curves) are still not driven,
    // which §6.2 records.
    // The engine's gates mean "this channel responds to pressure": the option must be on, its
    // sensors live ({X}UseCurve gates the SENSORS, §3.2), and pressure among them. Flow's checked
    // bit is dead (always-on), so it contributes only the latter two.
    const cb::PropertyLookup nested = maskingLookupOf(xml);
    const cb::CurveOptionData size = cb::readCurveOption("Size", nested);
    const cb::CurveOptionData flow = cb::readCurveOption("Flow", nested, /*checkable=*/false);
    m.sizeFromPressure = size.checked && size.useCurve && size.sensors.has(cb::SensorId::Pressure);
    m.flowFromPressure = flow.useCurve && flow.sensors.has(cb::SensorId::Pressure);
    m.flow = flow.strength;

    if (m.tip.kind == TipXml::Kind::Unknown) {
        // An unknown nested type has nothing the library could resolve: the masking walk keeps its
        // analytic round disc.
        addDrop(preset, "MaskingBrush tip '" + m.tip.type + "' (substituted with a round tip)");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }
}

// The smudge engine's stroke params (§6.6c), for the colorsmudge family. Called AFTER the option
// loop on purpose: the SmudgeRadius version migration edits the parsed option in place, and the
// colour-rate ceiling's static term comes off the parsed SmudgeRate. Everything the engine's
// legacy transcription does not implement is badged here, never silently absorbed.
void readSmudge(const PresetXml& xml, BrushPreset& preset) {
    cb::SmudgeParams& s = preset.smudge;
    s.enabled = true;
    // SmudgeRateMode: 0 = smearing (the default), 1 = dulling.
    s.dulling = intProperty(xml, "SmudgeRateMode", 0) == 1;
    // The engine implements the reference's default COPY blt (alpha lerps down too). No shipped
    // preset authors smearAlpha off; one that does keeps the default and says so.
    if (!boolProperty(xml, "SmudgeRateSmearAlpha", true)) {
        addDrop(preset, "SmudgeRate smear-alpha off (smeared with alpha)");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }
    // The transcription is the LEGACY strategy -- the one every grayscale-tip preset runs. A
    // preset authored for the new engine paints through the legacy one and says so.
    if (boolProperty(xml, "SmudgeRateUseNewEngine", false)) {
        addDrop(preset, "SmudgeRate new engine (painted with the legacy smudge)");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }
    // Overlay mode reads the merged image under the stroke; the engine reads this layer.
    if (boolProperty(xml, "MergedPaint", false)) {
        addDrop(preset, "Overlay mode (smudges this layer only)");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }
    // §5's standing gate: the paint-load channel is DROPPED, never mapped (docs/brushes.md §6.6b;
    // upstream only its lightness strategy reads it anyway).
    if (boolProperty(xml, "PressurePaintThickness", false)) {
        addDrop(preset, "PaintThickness");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }
    // The engine's smudge walk is Paint-only; an authored eraser smudge paints as a plain eraser.
    if (preset.eraserMode) {
        addDrop(preset, "EraserMode (an eraser cannot smear; painted as a plain eraser)");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
        s.enabled = false;
    }
    // The reference composites the colour rate through the preset's CompositeOp; the engine's
    // transcription is the OVER every shipped preset authors.
    if (preset.compositeOpId != "normal") {
        addDrop(preset,
                "CompositeOp '" + preset.compositeOpId + "' (colour rate painted as Normal)");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }
    // No shipped smudge preset carries a masking brush; the engine runs the smudge walk alone.
    if (preset.masking.enabled) {
        addDrop(preset, "MaskingBrush (the smudge walk paints unmasked)");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }
    // The colour-rate ceiling's static term: the SmudgeRate option's strength, read regardless of
    // its checkbox -- the reference reads strengthValue() the same way.
    if (const cb::CurveOptionData* rate = preset.option("SmudgeRate"))
        s.maxSmudgeRate = std::clamp(rate->strength, 0.0, 1.0);
    // SmudgeRadiusVersion < 2 stores PERCENT (every Krita-4-era preset; the key itself is absent
    // there): divide RAW, then apply the old-engine cap -- the reference's own read order. The
    // spec's [0,3] range then clamps at evaluation.
    if (intProperty(xml, "SmudgeRadiusVersion", 1) < 2) {
        for (cb::CurveOptionData& o : preset.options) {
            if (o.name == "SmudgeRadius") {
                o.strength = std::min(o.strength / 100.0, 3.0);
                break;
            }
        }
    }
}

// The SECOND ENGINE KIND's static property blocks (docs/brushes.md §6.6g), for the paintops
// `painterKindForPaintop` names. Read name for name with the reference reader's own defaults; what
// the transcription does not implement is badged here, never silently absorbed.
void readSketchPainter(const PresetXml& xml, BrushPreset& preset) {
    cb::SketchPainterParams& s = preset.painter.sketch;
    s.probability = doubleProperty(xml, "Sketch/probability", 0.50);
    s.offset = doubleProperty(xml, "Sketch/offset", 30.0);
    s.lineWidth = static_cast<int>(intProperty(xml, "Sketch/lineWidth", 1));
    s.simpleMode = boolProperty(xml, "Sketch/simpleMode", false);
    s.makeConnection = boolProperty(xml, "Sketch/makeConnection", true);
    s.magnetify = boolProperty(xml, "Sketch/magnetify", true);
    s.randomRgb = boolProperty(xml, "Sketch/randomRGB", false);
    s.randomOpacity = boolProperty(xml, "Sketch/randomOpacity", false);
    s.distanceOpacity = boolProperty(xml, "Sketch/distanceOpacity", false);
    s.distanceDensity = boolProperty(xml, "Sketch/distanceDensity", true);
    s.antiAliasing = boolProperty(xml, "Sketch/antiAliasing", false);

    // The MASK connection test (a candidate point must land on an opaque pixel of the tip rasterized
    // at the new point) is not transcribed: the painter uses the radius test of `simpleMode`, whose
    // radius is the same measurement. Both shipped sketch presets author simple mode.
    if (!s.simpleMode) {
        addDrop(preset, "Sketch mask mode (connected within the tip's radius)");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }
    // Anti-aliasing reaches the thick-line rasterizer, but the reference draws an antialiased ONE
    // pixel connection with a Wu line, which is not transcribed -- the painter draws the hard DDA
    // line there. Badged whenever the flag is on, because the per-span line width is an option value
    // and can reach 1 px at any pressure. Neither shipped preset sets it.
    if (s.antiAliasing) {
        addDrop(preset, "Sketch anti-aliasing (1 px connections drawn hard-edged)");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }
}

void readHairyPainter(const PresetXml& xml, BrushPreset& preset) {
    cb::HairyPainterParams& h = preset.painter.hairy;
    h.useMousePressure = boolProperty(xml, "HairyBristle/useMousePressure", false);
    h.shearFactor = doubleProperty(xml, "HairyBristle/shear", 0.0);
    h.randomFactor = doubleProperty(xml, "HairyBristle/random", 2.0);
    h.scaleFactor = doubleProperty(xml, "HairyBristle/scale", 2.0);
    h.densityFactor = doubleProperty(xml, "HairyBristle/density", 100.0);
    h.threshold = boolProperty(xml, "HairyBristle/threshold", false);
    h.antialias = boolProperty(xml, "HairyBristle/antialias", false);
    h.useCompositing = boolProperty(xml, "HairyBristle/useCompositing", false);
    h.connectedPath = boolProperty(xml, "HairyBristle/isConnected", false);

    h.inkDepletionEnabled = boolProperty(xml, "HairyInk/enabled", false);
    h.inkAmount = static_cast<int>(intProperty(xml, "HairyInk/inkAmount", 1024));
    h.inkDepletionCurve =
        cb::Curve::fromString(xml.property("HairyInk/inkDepletionCurve")
                                  .value_or(std::string(cb::kIdentityCurve)));
    h.useOpacity = boolProperty(xml, "HairyInk/useOpacity", true);
    h.useWeights = boolProperty(xml, "HairyInk/useWeights", false);
    // ⚠ THREE OF THE FOUR WEIGHTS ARE PERCENTS AND THE FOURTH IS NOT. The reference divides
    // pressure / bristle-length / bristle-ink by 100 at load and passes the depletion weight
    // through raw; reproducing that asymmetry is the transcription, and tidying it would rescale
    // every weighted-opacity brush by fifty.
    h.pressureWeight = static_cast<double>(intProperty(xml, "HairyInk/pressureWeights", 50)) / 100.0;
    h.bristleLengthWeight =
        static_cast<double>(intProperty(xml, "HairyInk/bristleLengthWeights", 50)) / 100.0;
    h.bristleInkAmountWeight =
        static_cast<double>(intProperty(xml, "HairyInk/bristleInkAmountWeight", 50)) / 100.0;
    h.inkDepletionWeight =
        static_cast<double>(intProperty(xml, "HairyInk/inkDepletionWeight", 50));

    // Saturation depletion runs the bristle colour through an HSL (not HSV) transformation, which
    // the engine has no branch for; soaked ink samples the layer under the press to colour each
    // bristle, which is a pre-stroke canvas read this painter does not take. Both would give a
    // bristle a colour of its OWN, which is also what keeps this engine on the Uniform accumulator.
    if (boolProperty(xml, "HairyInk/useSaturation", false)) {
        addDrop(preset, "HairyInk saturation depletion");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }
    if (boolProperty(xml, "HairyInk/soak", false)) {
        addDrop(preset, "HairyInk soak (bristles keep the paint colour)");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }
}

void readCurvePainter(const PresetXml& xml, BrushPreset& preset) {
    cb::CurvePainterParams& c = preset.painter.curve;
    // ⚠ THE REFERENCE READER PASSES NO DEFAULTS AT ALL here -- an absent key reads false / 0 / 0.0,
    // and a zero history size then makes its own path build from an empty list. Mosaic reads the
    // same keys with the reader's STRUCT defaults (what its own UI always writes) and the painter
    // floors the history at 1, because a zero window is not a shape.
    c.makeConnection = boolProperty(xml, "Curve/makeConnection", false);
    c.smoothing = boolProperty(xml, "Curve/smoothing", false);
    c.strokeHistorySize = static_cast<int>(intProperty(xml, "Curve/strokeHistorySize", 30));
    c.lineWidth = static_cast<int>(intProperty(xml, "Curve/lineWidth", 1));
    c.curvesOpacity = doubleProperty(xml, "Curve/curvesOpacity", 1.0);
}

void readParticlePainter(const PresetXml& xml, BrushPreset& preset) {
    cb::ParticlePainterParams& p = preset.painter.particle;
    p.count = static_cast<int>(intProperty(xml, "Particle/count", 50));
    p.iterations = static_cast<int>(intProperty(xml, "Particle/iterations", 10));
    p.gravity = doubleProperty(xml, "Particle/gravity", 0.989);
    p.weight = doubleProperty(xml, "Particle/weight", 0.2);
    p.scaleX = doubleProperty(xml, "Particle/scaleX", 0.3);
    p.scaleY = doubleProperty(xml, "Particle/scaleY", 0.3);
}

void readExperimentPainter(const PresetXml& xml, BrushPreset& preset) {
    cb::ExperimentPainterParams& e = preset.painter.experiment;
    e.windingFill = boolProperty(xml, "Experiment/windingFill", false);
    e.hardEdge = boolProperty(xml, "Experiment/hardEdge", false);

    // Three of the engine's features are NOT transcribed, and each is a distinct mechanism rather
    // than a parameter of the fill: the outward DISPLACEMENT of the accumulated path (a per-point
    // push with a path-simplification pass behind it), the SPEED correction (a filtered stand-in
    // position that outruns the pointer), and the path SMOOTHING (quadratic segments through a
    // running midpoint). The shipped preset authors none of them.
    if (boolProperty(xml, "Experiment/displacementEnabled", false)) {
        addDrop(preset, "Experiment displacement");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }
    if (boolProperty(xml, "Experiment/speedEnabled", false)) {
        addDrop(preset, "Experiment speed");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }
    if (boolProperty(xml, "Experiment/smoothing", false)) {
        addDrop(preset, "Experiment smoothing");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }
}

// The HATCHING engine's static block (§6.6g). Not a painter -- a dab engine whose dab content is a
// procedural lattice -- so it reads here beside readSmudge and lands on `BrushPreset::hatching`.
void readHatching(const PresetXml& xml, BrushPreset& preset) {
    cb::HatchingParams& h = preset.hatching;
    h.enabled = true;
    h.angle = doubleProperty(xml, "Hatching/angle", -60.0);
    h.separation = doubleProperty(xml, "Hatching/separation", 6.0);
    h.thickness = doubleProperty(xml, "Hatching/thickness", 1.0);
    h.originX = doubleProperty(xml, "Hatching/origin_x", 50.0);
    h.originY = doubleProperty(xml, "Hatching/origin_y", 50.0);
    h.separationIntervals = static_cast<int>(intProperty(xml, "Hatching/separationintervals", 2));
    h.antialias = boolProperty(xml, "Hatching/bool_antialias", false);
    h.opaqueBackground = boolProperty(xml, "Hatching/bool_opaquebackground", false);
    h.subpixelPrecision = boolProperty(xml, "Hatching/bool_subpixelprecision", false);

    // ⚠ THE STYLE IS A PRIORITY CHAIN, NOT AN ENUM, and "no crosshatching" defaults TRUE -- so a
    // file that mentions none of the five hatches once. The reference's own order, verbatim.
    if (boolProperty(xml, "Hatching/bool_nocrosshatching", true)) {
        h.style = cb::CrosshatchingStyle::None;
    } else if (boolProperty(xml, "Hatching/bool_perpendicular", false)) {
        h.style = cb::CrosshatchingStyle::Perpendicular;
    } else if (boolProperty(xml, "Hatching/bool_minusthenplus", false)) {
        h.style = cb::CrosshatchingStyle::MinusThenPlus;
    } else if (boolProperty(xml, "Hatching/bool_plusthenminus", false)) {
        h.style = cb::CrosshatchingStyle::PlusThenMinus;
    } else if (boolProperty(xml, "Hatching/bool_moirepattern", false)) {
        h.style = cb::CrosshatchingStyle::Moire;
    }

    // The antialiased hatch line upstream is a dedicated varying-width Wu line, which is not
    // transcribed -- Mosaic draws the transcribed distance-field thick line with antialiasing on,
    // and the difference is confined to the edge ramp. The NON-antialiased branch is exact.
    if (h.antialias) {
        addDrop(preset, "Hatching anti-aliasing (lines drawn with the thick-line rasterizer)");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }
    // An opaque background fills the whole dab with the background colour before hatching, which
    // needs a background colour the engine's dab pipeline does not carry.
    if (h.opaqueBackground) {
        addDrop(preset, "Hatching opaque background");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }
}

// roundmarker stores its geometry as FLAT properties (diameter default 30, spacing 0.02, the
// auto-spacing pair), not as a brush_definition. It is a hard round tip by construction, which is
// why §6.4 calls its tier Native-trivial: synthesize exactly that.
void mapRoundMarker(const PresetXml& xml, BrushPreset& preset) {
    preset.tip.kind = TipXml::Kind::Auto;
    preset.tip.type = "auto_brush";
    preset.tip.spacing = doubleProperty(xml, "spacing", 0.02);
    preset.tip.useAutoSpacing = boolProperty(xml, "useAutoSpacing", false);
    preset.tip.autoSpacingCoeff = doubleProperty(xml, "autoSpacingCoeff", 1.0);
    cb::MaskGeneratorParams& g = preset.tip.autoTip.generator;
    g.shape = cb::MaskShape::Circle;
    g.falloff = cb::MaskFalloff::Default;
    g.diameter = doubleProperty(xml, "diameter", 30.0);
    g.ratio = 1.0;
    g.hFade = 1.0; // hard to the rim
    g.vFade = 1.0;
    g.antialiasEdges = true;
}

} // namespace

// ------------------------------------------------------------------------------------------------

std::span<const OptionSpec> pixelBrushOptionSpecs() noexcept {
    return kPixelBrushOptions;
}

void addDroppedOption(BrushPreset& preset, std::string note) {
    if (static_cast<int>(preset.provenance.droppedOptions.size()) >= kMaxDroppedOptions)
        return;
    preset.provenance.droppedOptions.push_back(std::move(note));
}

void degradeFidelity(BrushPreset& preset, PresetFidelity floor) {
    if (static_cast<int>(preset.provenance.fidelity) < static_cast<int>(floor))
        preset.provenance.fidelity = floor;
}

core::brush::TipApplication resolveTipApplication(const PredefinedTipXml& tip,
                                                  bool hasColorAndTransparency) noexcept {
    switch (tip.applicationRule) {
    case TipApplicationRule::ForceLightness:
        return cb::TipApplication::LightnessMap;
    case TipApplicationRule::Explicit:
        return tip.application;
    case TipApplicationRule::LegacyContentTest:
        break;
    }
    // The legacy branch: ImageStamp only for a colour-capable format whose image content has
    // colour AND transparency, with ColorAsMask not asserting mask-ness (§3.5).
    return tip.colorfulCapable && hasColorAndTransparency && !tip.colorAsMask
               ? cb::TipApplication::ImageStamp
               : cb::TipApplication::AlphaMask;
}

const cb::CurveOptionData* BrushPreset::option(std::string_view base) const {
    for (const cb::CurveOptionData& o : options)
        if (o.name == base)
            return &o;
    return nullptr;
}

bool BrushPreset::optionActive(std::string_view base) const {
    const cb::CurveOptionData* o = option(base);
    return o != nullptr && (!o->checkable || o->checked);
}

core::brush::MaskingParams resolveMasking(const MaskingImport& masking, double masterDiameter) {
    cb::MaskingParams params;
    params.enabled = masking.enabled;
    if (!masking.enabled)
        return params;

    params.op = masking.op;
    params.flow = std::clamp(masking.flow, 0.0, 1.0);
    params.spacing = masking.tip.spacing;
    params.useAutoSpacing = masking.tip.useAutoSpacing;
    params.autoSpacingCoeff = masking.tip.autoSpacingCoeff;
    params.sizeFromPressure = masking.sizeFromPressure;
    params.flowFromPressure = masking.flowFromPressure;
    params.angleRad = masking.tip.angle;

    // The tip's own absolute size: an auto tip declares it; a bitmap tip's approximation follows
    // the master (its authored coupling in all shipped cases anyway).
    double diameter = masking.tip.kind == TipXml::Kind::Auto
                          ? masking.tip.autoTip.generator.diameter
                          : masterDiameter;
    if (masking.useMasterSize)
        diameter = masking.masterSizeCoeff * masterDiameter;
    params.diameter = std::clamp(diameter, 0.0, kMaxMaskingDiameter);

    if (masking.tip.kind == TipXml::Kind::Auto) {
        // The REAL nested tip: the masking walk stamps the generator's own raster, exactly as the
        // primary stamps its tip. `hardness` is still derived (the fades' mean, exact for a plain
        // circle) because it is what the null-tip analytic disc falls back to -- and what the
        // stroke PREVIEW's capped masking brush stays honest with if the tip ever fails to build.
        const cb::MaskGeneratorParams& g = masking.tip.autoTip.generator;
        params.tip = cb::makeTip(g);
        params.ratio = g.ratio;
        params.hardness = std::clamp((g.hFade + g.vFade) * 0.5, 0.0, 1.0);
    }
    // A Predefined masking tip resolves in the LIBRARY -- it needs the bundle's tip resources,
    // which this pure mapping cannot see. Until it does (and whenever it cannot), the null tip
    // stamps the analytic disc at `hardness`, which is what the masking walk always stamped.
    return params;
}

BrushPreset mapPreset(const PresetXml& xml, std::string_view sourceFormat) {
    BrushPreset preset;
    preset.name = xml.name;
    preset.provenance.sourceFormat = std::string(sourceFormat);
    preset.provenance.sourcePaintop = xml.paintopId;

    // The conformance tier (§6.4). Legacy ids ship in defaults: `eraser` is the pixel brush plus
    // EraserMode; `smudge` is colorsmudge's older spelling. `colorsmudge` maps to a REAL engine
    // now (§6.6c) -- it starts Exact like the pixel family and pays only for what it actually
    // drops (readSmudge below and the option loop).
    const std::string& op = xml.paintopId;
    const bool pixelFamily = (op == "paintbrush" || op == "eraser" || op == "roundmarker");
    const bool smudgeFamily = (op == "colorsmudge" || op == "smudge");
    // ⚠ ONE LIST, read here and in preset_brush.cpp (core/brush/stroke_painter.hpp): a paintop with
    // a real StrokePainter starts Exact like the pixel and smudge families and pays only for what
    // it actually drops. Reading a different list on either side would promise an engine the stroke
    // does not get -- the exact failure kDrivenOptions exists to prevent, one level up.
    const cb::StrokePainterKind painterKind = cb::painterKindForPaintop(op);
    const bool painterFamily = painterKind != cb::StrokePainterKind::None;
    preset.painter.kind = painterKind;
    // Hatching is the one non-pixel family that is a DAB engine (§6.6g): it starts Exact like the
    // rest, and its own gate lives on `BrushPreset::hatching` rather than on the painter kind.
    const bool hatchingFamily = op == "hatchingbrush";
    if (pixelFamily || smudgeFamily || painterFamily || hatchingFamily) {
        preset.provenance.fidelity = PresetFidelity::Exact;
    } else if (op == "spraybrush" || op == "filter") {
        preset.provenance.fidelity = PresetFidelity::Approximated;
        addDrop(preset, "Paintop '" + op + "' (approximated as a pixel brush)");
    } else {
        preset.provenance.fidelity = PresetFidelity::Substituted;
        addDrop(preset, "Paintop '" + op + "' (substituted with a pixel brush)");
    }
    if (op == "eraser")
        preset.eraserMode = true;

    // The tip.
    if (op == "roundmarker") {
        mapRoundMarker(xml, preset);
    } else if (const std::optional<std::string> def = xml.property("brush_definition")) {
        if (std::optional<TipXml> tip = parseTipXml(*def)) {
            preset.tip = std::move(*tip);
            if (preset.tip.kind == TipXml::Kind::Unknown) {
                addDrop(preset, "Tip '" + preset.tip.type + "' (substituted with a round tip)");
                degradeToAtLeast(preset, PresetFidelity::Substituted);
            }
            if (preset.tip.kind == TipXml::Kind::Auto && preset.tip.autoTip.unknownFalloffId)
                addDrop(preset, "MaskGenerator id (loaded as gauss)");
        } else {
            addDrop(preset, "Tip (unparseable brush_definition; default round tip)");
            degradeToAtLeast(preset, PresetFidelity::Substituted);
        }
    } else if (pixelFamily) {
        addDrop(preset, "Tip (no brush_definition; default round tip)");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }

    // Whatever happened above, a preset leaves the mapper with a USABLE tip: an unknown or
    // missing definition falls back to the default round auto tip (the provenance already says
    // so). The common attributes that DID parse -- spacing, angle, the auto-spacing pair -- and
    // the raw type string are kept.
    if (preset.tip.kind == TipXml::Kind::Unknown) {
        preset.tip.kind = TipXml::Kind::Auto;
        preset.tip.autoTip = AutoTipXml{};
        preset.tip.autoTip.generator.diameter = 24.0;
    }

    // Flat stroke-level properties.
    preset.eraserMode = preset.eraserMode || boolProperty(xml, "EraserMode", false);
    preset.compositeOpId = xml.property("CompositeOp").value_or("normal");
    if (preset.compositeOpId == "erase") {
        // Not a blend mode: the erase presets carve alpha, exactly StrokeMode::Erase.
        preset.eraserMode = true;
    } else if (const std::optional<core::BlendMode> mode =
                   blendModeFromCompositeOp(preset.compositeOpId)) {
        preset.blendMode = *mode;
    } else {
        addDrop(preset, "CompositeOp '" + preset.compositeOpId + "' (painted as Normal)");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }

    // BUILDUP=1, WASH=2 (§3.8); the absent default is WASH, and so is anything unrecognized.
    preset.paintMode = intProperty(xml, "PaintOpAction", 2) == 1 ? cb::PaintMode::Buildup
                                                                 : cb::PaintMode::Wash;

    // `Spacing/Isotropic` (§3.2, default false): the author's opt-out of the spacing ellipse -- the
    // cadence keys off the tip's LARGER extent and is the same in every direction. It is NOT the
    // `Spacing` OPTION (a per-dab curve, still unsupported); this is a plain static flag and the
    // engine honours it, so it costs no fidelity. Two shipped presets set it, both bitmap chalks.
    preset.isotropicSpacing = boolProperty(xml, "Spacing/Isotropic", false);

    if (const std::optional<std::string> src = xml.property("ColorSource/Type");
        src && *src != "plain") {
        addDrop(preset, "ColorSource '" + *src + "'");
        degradeToAtLeast(preset, PresetFidelity::Approximated);
    }

    // The option families. Everything is imported; the unsupported-but-active ones are listed.
    const cb::PropertyLookup lookup = lookupOf(xml);
    bool colorDynamics = false;
    preset.options.reserve(kPixelBrushOptions.size());
    for (const OptionSpec& spec : kPixelBrushOptions) {
        cb::CurveOptionData data =
            cb::readCurveOption(spec.base, lookup, spec.checkable, spec.min, spec.max);
        // The reference's Scatter value fix-up, verbatim: pre-2.x files stored the strength as
        // `Scattering/Amount`, and it wins only when the modern `ScatterValue` is ABSENT (a file
        // carrying both is a modern file whose Amount is stale). Unclamped here like every other
        // strength -- sizeLikeValue applies the [0,5] span at evaluation.
        if (spec.base == "Scatter" && !xml.property("ScatterValue")) {
            if (xml.property("Scattering/Amount"))
                data.strength = doubleProperty(xml, "Scattering/Amount", data.strength);
        }
        const bool active = !data.checkable || data.checked;
        if (active) {
            for (const std::string& id : data.sensors.unknownIds)
                addDrop(preset, std::string(spec.base) + ": unknown sensor '" + id + "'");
            if (isColorDynamicsBase(spec.base))
                colorDynamics = true;
            // Opacity splits exactly where the reference's indirect painting splits it: the STATIC
            // strength is the whole stroke's ceiling (BrushParams::opacity), and live sensors ride
            // every dab. Under WASH that is a per-dab CEILING through the transcribed
            // washAlphaDarkenAlpha (§6.2); under BUILDUP -- the direct path -- it is the per-dab
            // SHARE of the accumulation instead (§6.6i, brush_engine's buildCap). Both halves are
            // transcribed now, so Opacity is honoured end-to-end in either mode and costs no note.
            if (spec.base == "SmudgeRate" || spec.base == "ColorRate" ||
                spec.base == "SmudgeRadius") {
                // ⚠ NOT A DROP OUTSIDE THE SMUDGE FAMILY, AND NOT A KINDNESS EITHER: the
                // reference's own pixel brush constructs no smudge option at all (its option list
                // is size/ratio/rate/softness/lightness/spacing/scatter/sharpness/rotation/opacity
                // + mirror + precision), so `SmudgeRate` on a `paintbrush` is read by NOTHING
                // there. Mosaic's engine reads the trio only when the smudge walk is live, so the
                // stroke MATCHES -- which is what the badge measures. Badge-free, exactly like
                // Mirror on a colorsmudge preset (dab.hpp), and it is what five Krita-2-era presets
                // still carrying stale keys were being punished for (§6.6i).
                //
                // The sketch trio below keeps its badge on purpose and NOT by oversight: no shipped
                // preset trips it, so the same reading has no corpus evidence behind it there yet.
            } else if (spec.base == "Density" || spec.base == "Offset scale") {
                // The sketch trio's mirror of the same caveat: only the SKETCH painter reads them
                // (§6.6g), so an active one anywhere else is a dropped option.
                if (painterKind != cb::StrokePainterKind::Sketch) {
                    addDrop(preset, std::string(spec.base));
                    degradeToAtLeast(preset, PresetFidelity::Approximated);
                }
            } else if (spec.base == "Line width") {
                // ⚠ SHARED between two engines: the sketch painter scales its connection width with
                // it and the curve painter its pen width. Honoured on either, badged on anything
                // else.
                if (painterKind != cb::StrokePainterKind::Sketch &&
                    painterKind != cb::StrokePainterKind::Curve) {
                    addDrop(preset, std::string(spec.base));
                    degradeToAtLeast(preset, PresetFidelity::Approximated);
                }
            } else if (spec.base == "Curves opacity") {
                if (painterKind != cb::StrokePainterKind::Curve) {
                    addDrop(preset, std::string(spec.base));
                    degradeToAtLeast(preset, PresetFidelity::Approximated);
                }
            } else if (spec.base == "Angle" || spec.base == "Crosshatching" ||
                       spec.base == "Separation" || spec.base == "Thickness") {
                // The hatching quartet, honoured by the hatching STENCIL alone -- the same caveat
                // the smudge trio carries, with a different consumer.
                if (!hatchingFamily) {
                    addDrop(preset, std::string(spec.base));
                    degradeToAtLeast(preset, PresetFidelity::Approximated);
                }
            } else if (isColorDynamicsBase(spec.base)) {
                // Colour dynamics (§6.6f). h/s/v ride the Colored accumulator's per-dab colour
                // end-to-end (applyColorDynamics), so OFF smudge they are honoured and cost no
                // fidelity -- they fall out of the generic drop below because optionSupported() now
                // returns true for them. Mix and Darken are not transcribed yet, so an active one is
                // still a dropped option. And on a SMUDGE preset the engine paints the stroke's own
                // colour (no per-dab colour source), so ANY colour-dynamics option is dropped there
                // and badged -- the reference's colorsmudge DOES adjust the colour, so this is an
                // honest fidelity note, not a faithful badge-free drop like Mirror's.
                if (smudgeFamily) {
                    addDrop(preset,
                            std::string(spec.base) + " (colour dynamics on a smudge stroke)");
                    degradeToAtLeast(preset, PresetFidelity::Approximated);
                } else if (!optionSupported(spec.base)) { // Mix / Darken -- h/s/v are supported now
                    addDrop(preset, std::string(spec.base));
                    degradeToAtLeast(preset, PresetFidelity::Approximated);
                }
            } else if (spec.base == "Texture/Strength/" || spec.base == "Rate") {
                // ⚠ The two single-consumer CADENCE/COMPOSITE options (§6.6h), and both are
                // faithful badge-FREE drops when their consumer is off -- like Mirror on a smudge
                // preset, not like the smudge trio on a pixel brush. The reference reads its
                // texture strength only from inside the texture composite (which returns before it
                // when texturing is disabled) and its rate only from inside the timing step (whose
                // `timingEnabled` is false without an airbrush): an option nothing consumes is an
                // option the reference does not apply either. 8 shipped presets carry a checked
                // `Texture/Strength/` with texturing enabled and are honoured end-to-end.
            } else if (!optionSupported(spec.base)) {
                // Every other active-but-unsupported option lists itself.
                addDrop(preset, std::string(spec.base));
                degradeToAtLeast(preset, PresetFidelity::Approximated);
            }
        }
        preset.options.push_back(std::move(data));
    }

    // The positional options' axis gates (§6.6d) -- static properties beside their curve options.
    // Read whole-file rather than per-option: the defaults are the format's own, and an absent key
    // is the default, not "off".
    preset.scatterAxisX = boolProperty(xml, "Scattering/AxisX", true);
    preset.scatterAxisY = boolProperty(xml, "Scattering/AxisY", true);
    preset.mirrorHorizontal = boolProperty(xml, "HorizontalMirrorEnabled", false);
    preset.mirrorVertical = boolProperty(xml, "VerticalMirrorEnabled", false);
    // Sharpness's two static properties (§6.6e): alignOutline gates the pixel-grid snap, softness is
    // the applyThreshold soft band. Read the reference's live key (`Sharpness/softness`, default 0);
    // clamp to the format's [0,100] range so a malformed file cannot make (100 - softness) negative.
    preset.sharpnessAlignOutline = boolProperty(xml, "Sharpness/alignoutline", false);
    preset.sharpnessSoftness =
        std::clamp(static_cast<int>(intProperty(xml, "Sharpness/softness", 0)), 0, 100);

    readTexture(xml, preset);
    readAirbrush(xml, preset);
    readMasking(xml, preset);
    if (smudgeFamily)
        readSmudge(xml, preset); // after the option loop and readMasking: see its comment
    // The painter's own property block, before the accumulator choice below reads it (§6.6g: a
    // sketch preset that randomizes its connection colour needs the Colored accumulator).
    switch (painterKind) {
    case cb::StrokePainterKind::Sketch: readSketchPainter(xml, preset); break;
    case cb::StrokePainterKind::Hairy: readHairyPainter(xml, preset); break;
    case cb::StrokePainterKind::Curve: readCurvePainter(xml, preset); break;
    case cb::StrokePainterKind::Particle: readParticlePainter(xml, preset); break;
    case cb::StrokePainterKind::Experiment: readExperimentPainter(xml, preset); break;
    case cb::StrokePainterKind::None: break;
    }
    if (hatchingFamily)
        readHatching(xml, preset);

    // §6.1's accumulator seam. A LegacyContentTest application is resolved after the tip file
    // loads; `false` is the assumption that is right for every explicit shipped preset, and
    // the library re-runs both calls with the real content verdict then.
    cb::TipApplication application = cb::TipApplication::AlphaMask;
    if (preset.tip.kind == TipXml::Kind::Predefined)
        application = resolveTipApplication(preset.tip.predefined,
                                            /*hasColorAndTransparency=*/false);
    preset.colorDynamicsActive = colorDynamics;
    preset.accumulator =
        cb::chooseAccumulator(application, colorDynamics, cb::painterVariesColor(preset.painter));

    return preset;
}

} // namespace mosaic::io::brush
