#pragma once

#include <map>
#include <memory>
#include <string>

#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/selection.hpp"
#include "core/texture/texture_params.hpp"
#include "core/texture/texture_render.hpp"

// The "mask & harmonize" commit shape (S55 estimate-from-layer, design §6.3): ONE
// CompositeCommand -- so one undo step -- assembling
//
//   [ AddLayer   (sky TextureLayer, baked cache pre-installed, BELOW the photo)
//   , SetLayerMask (photo, NOT-sky -- the feathered foreground mask)
//   , AddLayer   (PhotometricMatch AdjustmentLayer, clipped to the photo, ABOVE it) ]
//
// Sky below + photo masked to its foreground above: the feathered foreground edge composites
// over the new sky (correct fringing), the sky plate stays whole and editable, and each piece is
// independently disable-able afterwards. Assembly only -- the caller (the dialog's ACCEPT path,
// phase 2) pushes the command through the document's CommandStack. Compositing stays
// user-driven: ⚠ this helper is invoked by an explicit user toggle and must NEVER be chained
// automatically, and the sky it composites is PROCEDURAL — never a second photographic image.
// Both are hard constraints on this path, not defaults.
namespace mosaic::core::texture {

struct SkyConformPlan {
    TextureParams skyParams{};        // the accepted generator value (the new layer's content)
    TextureRenderResult baked{};      // the full-res bake to pre-install as the layer's cache
    Selection skySelection{};         // doc-space sky coverage (S6's product)
    std::map<std::string, double> matchParams{};  // the PhotometricMatch bag (S7's product)
    std::string label = "Sky from layer";         // the History entry's name
};

// Build the composite command against `photoLayerId` (the analyzed layer). Returns nullptr when
// the photo layer cannot be located, the selection is empty, or the plan carries no sky params
// -- the caller reports and skips, never half-commits.
[[nodiscard]] std::unique_ptr<CompositeCommand> buildSkyConformCommand(Document& doc,
                                                                       LayerId photoLayerId,
                                                                       SkyConformPlan plan);

}  // namespace mosaic::core::texture
