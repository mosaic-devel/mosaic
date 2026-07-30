#pragma once

#include "core/blend_mode.hpp"
#include "core/brush/brush_engine.hpp"
#include "core/brush/curve_option.hpp"
#include "io/brush/preset_xml.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The imported brush preset -- what the mapper (io/brush/mapper.cpp) produces from a parsed
// preset document, and what the preset library hands the tools. It is deliberately ENGINE-SHAPED:
// core::brush types throughout, so Arc D's wiring consumes it without another translation layer.
//
// Two things are staged rather than final, because both need the tip FILE's pixels, which only
// the library (with the bundle in hand) can load:
//   * A predefined tip is a reference (filename + md5) until resolved to a BitmapTip.
//   * The accumulator choice and a legacy tip's application both hinge on a pixel-content test
//     (§3.5): `resolveApplication()` + `chooseAccumulator()` run post-load. The mapper's stored
//     verdict assumes AlphaMask, which is correct for every preset whose application is explicit.
//
// PresetProvenance is the §6.4 honesty contract: where the preset came from, how faithful the
// import is, and exactly which of its active options were dropped. It reports fidelity to the
// user and NOTHING ELSE -- never inspecting, restricting or phoning home about imported content.
namespace mosaic::io::brush {

enum class PresetFidelity : std::uint8_t {
    Exact,        // the pixel-brush family: everything active is honoured
    Approximated, // close but measurably different (a dropped option, an approximated paintop)
    Substituted,  // a paintop Mosaic has no engine for, imported as its nearest pixel-brush kin
};

inline constexpr int kMaxDroppedOptions = 64;

struct PresetProvenance {
    std::string sourceFormat;  // "kpp", "bundle", "mbp", ...
    std::string sourcePaintop; // the raw paintopid, e.g. "colorsmudge" -- even when substituted
    PresetFidelity fidelity = PresetFidelity::Exact;
    // Human-readable notes on what did not survive the import, one per active option: "Texture",
    // "Sharpness", "Airbrush", "Size: unknown sensor 'foo'". Capped at kMaxDroppedOptions.
    std::vector<std::string> droppedOptions;
};

// The TEXTURE option as imported (docs/brushes.md §6.6h). The pattern itself is EMBEDDED in the
// preset (`Texture/Pattern/Pattern`, a base64 payload the XML layer leaves encoded), and decoding
// + baking it into the 8-bit mask a dab reads is the LIBRARY's job -- exactly as a predefined tip's
// pixels are: the mapper is a pure function of the parsed document and does not decode images.
//
// So this carries the reference, and `LibraryPreset::texture` carries the resolved
// `core::brush::TextureParams`.
struct TextureImport {
    bool enabled = false;
    core::brush::TexturingMode mode = core::brush::TexturingMode::Multiply;
    core::brush::TextureBake bake; // scale + the five mask adjustments, baked once
    int offsetX = 0;
    int offsetY = 0;
    bool randomOffsetX = false;
    bool randomOffsetY = false;
    bool softTexturing = false;
    // The embedded pattern file, still base64 (a PNG in every shipped case).
    std::string patternBase64;
    std::string patternName; // `Texture/Pattern/Name`, for provenance
};

// The airbrush timing knobs (§3.2/§6.6h): the stroke's second, TIME-driven dab cadence. Honoured
// end-to-end since 2026-07-28 -- `core::brush::AirbrushParams` is what the engine reads, and the
// per-dab `Rate` option rides `options` below like every other curve option.
struct AirbrushImport {
    bool enabled = false;
    double rate = 20.0; // dabs per second
    bool ignoreSpacing = false;
};

// The masking brush as imported (§6.2). The final core::brush::MaskingParams needs the PRIMARY
// tip's absolute diameter (UseMasterSize multiplies it), so resolution is a separate, pure step.
struct MaskingImport {
    bool enabled = false;
    core::brush::MaskingOp op = core::brush::MaskingOp::Multiply;
    std::string opId;        // the raw MaskingCompositeOp id, for provenance
    bool unknownOp = false;  // an id beyond the three implemented: imported as multiply, noted
    bool useMasterSize = true;
    double masterSizeCoeff = 1.0;
    TipXml tip;              // the nested brush_definition (its OWN spacing/auto-spacing pair)
    double flow = 1.0;       // the nested FlowValue
    bool sizeFromPressure = false; // the nested PressureSize / PressureFlow gates
    bool flowFromPressure = false;
};

// The upstream cap on a resolved masking-tip size: min(15000, 3 x the configurable max brush
// size, whose default is 1000). Mosaic has no such setting, so the default's product is a
// constant.
inline constexpr double kMaxMaskingDiameter = 3000.0;

// `masterDiameter` is the primary tip's absolute size (known immediately for auto tips; after
// tip-file load for predefined ones). Pure -- the one place UseMasterSize/MasterSizeCoeff turn
// into document pixels.
[[nodiscard]] core::brush::MaskingParams resolveMasking(const MaskingImport& masking,
                                                        double masterDiameter);

struct BrushPreset {
    std::string name;
    PresetProvenance provenance;

    // The tip, as referenced by the preset. Auto tips are complete here; predefined ones carry
    // filename + md5 until the library resolves them against the bundle's brushes/.
    TipXml tip;

    core::brush::PaintMode paintMode = core::brush::PaintMode::Wash;
    bool eraserMode = false; // EraserMode, or a CompositeOp of "erase"
    // `Spacing/Isotropic` (§3.2): the cadence keys off the tip's larger extent and is the same in
    // every direction, instead of following the dab's spacing ellipse. A static flag, not the
    // per-dab `Spacing` option -- the engine honours it, so it costs no fidelity.
    bool isotropicSpacing = false;
    core::BlendMode blendMode = core::BlendMode::Normal;
    std::string compositeOpId; // raw, for provenance
    // The mapper's verdict under its stated assumption (see file comment). Colour dynamics are
    // already folded in; only a LegacyContentTest application can change it post-load.
    core::brush::StrokeAccumulator accumulator = core::brush::StrokeAccumulator::Uniform;
    // Whether an h/s/v/Mix/Darken option is on -- chooseAccumulator's second input, persisted
    // so the library can re-run the choice after the tip file resolves the application.
    bool colorDynamicsActive = false;

    // Every §3.2 option family the paintop carries, parsed through readCurveOption -- including
    // the two always-on ones (Opacity, Flow read as checked regardless of their bit). Order
    // follows the mapper's base-name table.
    std::vector<core::brush::CurveOptionData> options;

    // The positional options' axis gates (§6.6d): plain static properties that live BESIDE their
    // curve options in the file (`Scattering/AxisX`/`AxisY` default true;
    // `{Horizontal,Vertical}MirrorEnabled` default false). Meaningful only when the matching
    // option in `options` exists; carried unconditionally because the defaults are the file
    // format's own.
    bool scatterAxisX = true;
    bool scatterAxisY = true;
    bool mirrorHorizontal = false;
    bool mirrorVertical = false;
    // The Sharpness option's two static properties (§6.6e), beside its curve option like the
    // positional gates. `alignOutline` (`Sharpness/alignoutline`, default false) gates the pixel-grid
    // coordinate snap; `softness` (`Sharpness/softness`, int [0,100], default 0) is applyThreshold's
    // soft band. Krita 6 reads `Sharpness/softness` -- the Krita-4 `Sharpness/threshold` key is dead
    // there, so a preset that only carries the old key loads at softness 0, exactly as the reference.
    bool sharpnessAlignOutline = false;
    int sharpnessSoftness = 0;

    MaskingImport masking;
    AirbrushImport airbrush;
    // The texture option's reference (§6.6h); the library bakes it into LibraryPreset::texture.
    TextureImport texture;
    // The smudge engine's stroke params (§6.6c), already engine-shaped -- set by the mapper for
    // the colorsmudge family (mode, smear-alpha, the colour-rate ceiling's static term), disabled
    // for everything else. The per-dab rates live in `options` above (SmudgeRate / ColorRate /
    // SmudgeRadius) like every other curve option.
    core::brush::SmudgeParams smudge;
    // The SECOND ENGINE KIND (docs/brushes.md §6.6g), already engine-shaped -- set by the mapper for
    // the paintops `core::brush::painterKindForPaintop` names, `kind == None` for everything else.
    // Its per-span options live in `options` above (Density / Line width / Offset scale for the
    // sketch engine; Size / Rotation for both) like every other curve option; what is here is the
    // paintop's own static property block.
    core::brush::StrokePainterParams painter;
    // The hatching engine's static block (§6.6g). NOT a painter: hatching is a dab engine whose dab
    // content is a procedural lattice stencilled by the tip mask, so this rides `BrushParams` beside
    // `smudge` and its four per-dab options live in `options` above like every other curve option.
    core::brush::HatchingParams hatching;

    [[nodiscard]] const core::brush::CurveOptionData* option(std::string_view base) const;
    // True when the named option exists and is on (not-checkable counts as on -- §3.2).
    [[nodiscard]] bool optionActive(std::string_view base) const;
};

// The §3.5 four-branch application rule, in producer priority: preserveLightness >
// brushApplication > ColorAsMask > the bare content test. The XML layer folded the first three
// into `applicationRule` + `colorAsMask`; the last input -- does the tip IMAGE have colour and
// transparency -- only the loaded tip file can answer (TipFile::hasColorAndTransparency). The
// mapper calls this with `false` (right for every explicit application); the library re-runs it
// with the truth once the tip resolves, then re-runs chooseAccumulator.
[[nodiscard]] core::brush::TipApplication resolveTipApplication(const PredefinedTipXml& tip,
                                                                bool hasColorAndTransparency)
    noexcept;

// Provenance edits, shared by the mapper and the library (the library adds tip-resolution
// notes): append to droppedOptions (capped at kMaxDroppedOptions) / lower fidelity, never raise.
void addDroppedOption(BrushPreset& preset, std::string note);
void degradeFidelity(BrushPreset& preset, PresetFidelity floor);

} // namespace mosaic::io::brush
