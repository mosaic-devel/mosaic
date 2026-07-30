#pragma once

#include "io/brush/preset.hpp"
#include "io/brush/preset_xml.hpp"

#include <span>
#include <string_view>

// The paintop mapper (docs/brushes.md §6.4, §7): a parsed preset document -> Mosaic's BrushPreset,
// with a PresetProvenance that says exactly how faithful the trip was.
//
// The conformance tiers, per §6.4: the pixel-brush family (`paintbrush`, the legacy `eraser`, and
// `roundmarker`) maps at full fidelity; `colorsmudge`/`smudge`, `spraybrush` and `filter` are
// Approximated; every other paintop is Substituted -- imported as its nearest pixel-brush
// equivalent and flagged, never dropped. An active option the engine cannot honour yet downgrades
// Exact to Approximated and lists itself in droppedOptions.
//
// Honesty counters, never a copyright cop: provenance reports fidelity to the user and nothing
// else. Imported content is never inspected beyond what loading it requires, never reported
// anywhere but the preset's own badge, and never restricted (§4.1).
namespace mosaic::io::brush {

// Map one parsed preset document. Total: any recognizable <Preset> yields a preset -- the
// question the provenance answers is only how much survived. `sourceFormat` lands in the
// provenance verbatim ("kpp", "bundle", ...).
[[nodiscard]] BrushPreset mapPreset(const PresetXml& xml, std::string_view sourceFormat);

// The §3.2 option families of the pixel-brush property surface, with each consumer's
// checkability and strength range. ONE table: the other importers (.myb) build the same
// 18-family option vector, so a preset's shape never depends on which format it came from.
struct OptionSpec {
    std::string_view base;
    bool checkable = true;
    double min = 0.0;
    double max = 1.0;
};
[[nodiscard]] std::span<const OptionSpec> pixelBrushOptionSpecs() noexcept;

} // namespace mosaic::io::brush
