#pragma once

#include "core/brush/brush_engine.hpp"
#include "io/brush/library.hpp"

// A preset, as the ENGINE takes it (docs/brushes.md §8, Arc D): `LibraryPreset` -> `BrushParams`,
// the tip and the option pipeline included. Pure, and the last translation layer -- everything under
// it is already engine-shaped (io/brush/preset.hpp made sure of that), so this is a mapping and not
// a reader.
//
// ⚠ CALL IT ONCE PER PRESET, NOT ONCE PER STROKE. It mints a fresh raster id for the tip
// (brush_tip.hpp), and a fresh id is a COLD DAB CACHE: every dab of every stroke would re-rasterize
// its own mask. The BrushParams it returns is cheap to copy and its tip is shared, so the shape to
// build is "resolve when the user PICKS a preset, copy per stroke".
//
// There is no honesty list here, deliberately. What the engine cannot drive is already recorded, per
// preset, in `BrushPreset::provenance.droppedOptions` -- the importer computes it from the very same
// `core::brush::kDrivenOptions` this file maps (dab.hpp). One list, one badge: an option cannot be
// driven without being honoured, or honoured without being driven.
namespace mosaic::io::brush {

// What the CALLER still owns, and must set on top of the returned params:
//   * `color` -- the editor's foreground. Never a property of a preset.
//   * `seed` -- fixes the stroke's random streams. A parameter, never a clock.
//   * `strokeMode` -- the Eraser tool carves with whatever preset it is handed.
//   * `diameter` / `opacity`, when a context-bar slider is showing live values for them (§8.1). The
//     values returned here are the preset's own, which is what those sliders are seeded FROM.
[[nodiscard]] core::brush::BrushParams presetBrushParams(const LibraryPreset& preset);

} // namespace mosaic::io::brush
