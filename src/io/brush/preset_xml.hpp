#pragma once

#include "core/brush/bitmap_tip.hpp"
#include "core/brush/mask_generator.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The preset XML layer (docs/brushes.md §3.1-§3.2, §3.4-§3.5): the `<Preset>` document that rides
// in a `.kpp`'s text chunk, and the `<Brush>` tip element that rides -- as a CDATA string -- in its
// `brush_definition` property. Parsed with pugixml, never a scanner: attribute order is not fixed
// (the corpus writes `type=` before `name=` in 23545 of 28152 params), and a `name=`-first regex
// mis-parses 64 of the 82 pixel-brush presets (§3.2).
//
// This layer is STRUCTURE only. It produces a flat property table plus parsed tip parameters; what
// the properties mean -- the seven-key option families, the two prefixing rules, the always-on
// options -- is the mapper's business (§3.2, Arc B step 3). All number parsing goes through
// core/brush/parse_util.hpp: the format always spells the decimal point '.', whatever LC_NUMERIC
// says (the shipped-curve-parser-bug-on-pl_PL rule).
//
// Every default and every quirk here was read off the producer's own loader, not guessed:
//   * `spacing` defaults to **1.0 on an auto_brush element but 0.25 on a predefined one** -- the
//     two factories genuinely disagree, and §3.5's "absent means 0.25" is the predefined half.
//   * Legacy `radius=` is a SYNONYM for `diameter=` ("mistakenly named radius for 2.2") -- the
//     same value, not a half of it -- and it WINS when both are present.
//   * `hfade`/`vfade` default 0.0 (fully soft), `antialiasEdges` defaults FALSE, `spikes` 2.
//   * The generator `type` is matched as `"circle"`-or-else-rect; the `id` as default / soft /
//     else-GAUSS (an unknown id is a gauss tip upstream, recorded here as `unknownFalloffId`).
//   * `softness_curve` is used only when the attribute is PRESENT; absent keeps Mosaic's
//     descending default (mask_generator.hpp) -- the identity would build an inside-out tip.
//   * A predefined tip's `scale` (default 1.0) is DOUBLED unless `BrushVersion` says "2"; the
//     attribute's default is "1", so omitting it doubles (§3.5).
//   * The three tip adjustments carry a MIGRATION: `AdjustmentVersion` < 2 with no
//     `AutoAdjustMidPoint` attribute doubles the midpoint's offset from 127 (clamped 0..255),
//     doubles brightness and contrast, and remaps a negative contrast through 1/(1-c)-1 (§3.5).
//     Values are stored post-migration and deliberately UNCLAMPED beyond the midpoint, matching
//     the producer.
//   * `angle` is RADIANS -- it is summed straight into the dab shape's rotation upstream.
//
// The input is a third-party file: parsing is total (a scan or an error, never a crash), every
// cap below is enforced, and anything skipped is counted rather than silently dropped.
namespace mosaic::io::brush {

// One `<param>` of the preset document. `value` is the raw text -- CDATA unwrapped, but base64
// still encoded for `bytearray` (decoding is its consumer's business, and most are never read).
struct PresetParam {
    enum class Type : std::uint8_t { String, Internal, Color, ByteArray, Unknown };
    Type type = Type::String;
    std::string value;
};

// One `<resource>` of a version-5.0 preset's `<resources>` element. The payload stays base64.
struct EmbeddedResource {
    std::string type;
    std::string md5sum;
    std::string name;
    std::string filename;
    std::string base64;
};

inline constexpr int kMaxPresetParams = 4096;      // the stock pixel brush carries 94
inline constexpr int kMaxEmbeddedResources = 256;  // v5 presets embed a handful at most
inline constexpr std::size_t kMaxPresetValueBytes = 16u << 20; // one param or resource payload
inline constexpr std::size_t kMaxPresetTotalBytes = 64u << 20; // all of them together

// The parsed `<Preset>` document: attributes + the flat property table.
struct PresetXml {
    std::string name;      // the `name` attribute (may differ from the file's name)
    std::string paintopId; // `paintopid` -- which engine the preset drives (§3.9)
    std::map<std::string, PresetParam, std::less<>> params;
    std::vector<EmbeddedResource> resources;

    // Honesty counters (the docio discipline): a hostile or newer file loses content to the caps
    // or to malformed entries, and the loss must be visible, not silent.
    int skippedParams = 0;     // over the caps, or a param with no usable name
    int skippedResources = 0;  // resources over their count cap or the shared byte budget
    int duplicateParams = 0;   // same name twice -- the LAST wins, as the producer's map does
    int unknownParamTypes = 0; // type= values this build has no enum for (kept, as Unknown)

    // The mapper's lookup: the raw value of `key`, or nullopt when absent. Absent is NOT the same
    // as empty -- option defaults differ (§3.2) -- which is why this is optional<string>.
    [[nodiscard]] std::optional<std::string> property(std::string_view key) const;
};

// Parse a `<Preset>` document. Fails (nullopt + reason) only when the XML itself is unusable:
// malformed syntax, or a root element that is not <Preset>. Individually bad params are counted.
[[nodiscard]] std::optional<PresetXml> parsePresetXml(std::string_view xml,
                                                      std::string* error = nullptr);

// ------------------------------------------------------------------------------------------------
// The <Brush> tip element (the `brush_definition` property's CDATA payload).

// auto_brush: the six-generator procedural tip (§3.4).
struct AutoTipXml {
    core::brush::MaskGeneratorParams generator;
    double randomness = 0.0;
    double density = 1.0;
    bool unknownFalloffId = false; // an id this build has never heard of loads as gauss, flagged
};

// A predefined tip's application cannot be resolved from the XML alone: the legacy branches hinge
// on whether the tip IMAGE has colour and transparency -- a pixel-content test (§3.5) that only
// the tip loader can run. So the XML layer records which RULE applies and its inputs verbatim.
enum class TipApplicationRule : std::uint8_t {
    // `preserveLightness` was present and true: LightnessMap, unconditionally.
    ForceLightness,
    // `brushApplication` was present: `application` below holds it.
    Explicit,
    // Legacy: ImageStamp iff the tip is a colour-capable format AND its image has colour AND
    // transparency AND !colorAsMask. Three attribute shapes land here, with different colorAsMask:
    // `preserveLightness=false` reads ColorAsMask defaulting TRUE; a bare `ColorAsMask` supplies
    // itself; NO attribute at all means colorAsMask=false (the pre-4.4 automatic heuristic).
    LegacyContentTest,
};

// gbr/gih/png/svg/abr reference: filename + md5, plus everything §3.5 pins.
struct PredefinedTipXml {
    std::string filename;
    std::string md5sum;
    double scale = 1.0; // post-BrushVersion doubling: what the engine multiplies baseSize by

    TipApplicationRule applicationRule = TipApplicationRule::LegacyContentTest;
    core::brush::TipApplication application = core::brush::TipApplication::AlphaMask;
    bool colorAsMask = false; // meaningful under LegacyContentTest

    // True for the subtypes whose loader class can carry colour (gbr_brush -- which includes the
    // .gih hose -- and png_brush). svg/abr are not: their LegacyContentTest always resolves
    // AlphaMask, and the three adjustments are NOT EVEN READ for them upstream -- so this parser
    // leaves them neutral there too, rather than recording attributes the producer ignores.
    bool colorfulCapable = false;

    core::brush::TipAdjustments adjustments; // post-migration (§3.5); neutral unless colorfulCapable
};

struct TipXml {
    enum class Kind : std::uint8_t { Auto, Predefined, Unknown };
    Kind kind = Kind::Unknown;
    std::string type; // the raw `type` attribute, for provenance ("auto_brush", "gbr_brush", ...)

    // The common attributes every <Brush> carries. `angle` in radians.
    double angle = 0.0;
    double spacing = 0.25; // set from the attribute, or the KIND'S default -- 1.0 auto, 0.25 predefined
    bool useAutoSpacing = false;
    double autoSpacingCoeff = 1.0;

    AutoTipXml autoTip;           // meaningful when kind == Auto
    PredefinedTipXml predefined;  // meaningful when kind == Predefined
};

// Parse the `<Brush>` element (given the whole sub-document string). Fails only on unusable XML
// or a missing <Brush> element; an unknown `type` yields Kind::Unknown with the common attributes
// still read, so the mapper can substitute and report rather than drop the preset.
[[nodiscard]] std::optional<TipXml> parseTipXml(std::string_view xml, std::string* error = nullptr);

} // namespace mosaic::io::brush
