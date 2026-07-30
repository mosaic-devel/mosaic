#pragma once

#include "common/image.hpp"
#include "core/blend_mode.hpp"
#include "core/layer.hpp"  // LayerId (the cut's lift provenance)
#include "core/selection.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

// The clipboard model (PLAN S14-b): pure pixel logic for Edit→Cut/Copy/Copy Merged/Paste.
// Everything here is document-space and FLTK-free (unit-tested in tests/test_clipboard.cpp);
// the OS-clipboard interop (Fl_Copy_Surface out, Fl::paste in) lives with the main window.
namespace mosaic::core {

class Layer;

// What Cut/Copy carries: RGBA pixels (straight alpha) plus where in the document they came
// from, so an in-document paste lands back at the source position. The paste-semantics pass
// (2026-06-11) added provenance: the source layer's name (so a pixel paste can be named
// "Selection from <layer>") and, for WHOLE-layer copies, the layer's style — pasting one is
// Affinity's object clipboard, behaving like Layer→Duplicate rather than an anonymous paste.
struct ClipboardContent {
    common::Image image;
    int docX = 0;
    int docY = 0;
    // Which layer the pixels came from (UI sets "merged" for Copy Merged); empty means the
    // content arrived from outside (the OS clipboard).
    std::string sourceName;

    // The source layer's restorable properties, recorded only when the copy took the whole
    // layer (no selection active): paste re-applies them, name verbatim.
    struct LayerStyle {
        std::string name;
        float opacity = 1.0f;
        BlendMode blend = BlendMode::Normal;
    };
    std::optional<LayerStyle> style;

    // CUT ONLY: which layer these pixels were lifted OFF, so an in-place paste can re-link the two
    // halves as a coverage partition (core::CoveragePartition) and the compositor can recombine
    // them without the `over` seam. Recorded only when the split is a true partition -- a raster
    // source on an integer-translated grid, so the fragment's document-space pixels and the
    // residual's layer-space pixels line up one-to-one (see partitionEligibleSource).
    struct Lift {
        LayerId sourceLayer = kInvalidLayerId;
        std::uint64_t sourceRevision = 0;  // the residual's contentRevision() AFTER the erase
    };
    std::optional<Lift> lift;
};

// Is `layer` a cut source whose two halves would genuinely tile? Requires a raster layer whose
// WORLD transform is an integer translation: the compositor then places it with the lossless
// Nearest kernel and each residual pixel lands on exactly one document pixel, which is the pixel
// the fragment took its complementary alpha from. Under any other placement the two halves are
// resampled onto grids that disagree by up to half a pixel, and calling that a partition would
// trade a faint seam for a hard one.
[[nodiscard]] bool partitionEligibleSource(const Layer& layer);

// The layer's pixels under `sel` — the WHOLE layer when sel.isEmpty() (empty = "everything
// editable", not "nothing") — sampled in document space through the layer's transform
// (nearest, like the compositor's leaf walk) and cropped to the covered bounds; selection
// coverage multiplies alpha. nullopt when the layer kind has no pixels, or nothing visible is
// covered (Photoshop's "no pixels were selected" case).
[[nodiscard]] std::optional<ClipboardContent> copyFromLayer(const Layer& layer,
                                                            const Selection& sel,
                                                            std::uint32_t docW,
                                                            std::uint32_t docH);

// Copy Merged: the flattened composite under `sel` (the whole canvas when empty), alpha-masked
// by coverage and cropped to the selection bounds. The composite must be checkerboard-free.
[[nodiscard]] std::optional<ClipboardContent> copyMerged(const common::Image& composite,
                                                         const Selection& sel);

// Cut's destructive half: the raster layer's image with the selection's coverage erased from
// its alpha (evaluated per layer pixel through the transform; an empty selection clears the
// whole layer). nullopt when the layer isn't an editable raster, or nothing would change.
[[nodiscard]] std::optional<common::Image> imageWithSelectionCleared(const Layer& layer,
                                                                     const Selection& sel);

// Where a pasted layer's top-left lands: at `source` when the content came from this document,
// centred otherwise (may go negative for content larger than the canvas — intentional).
[[nodiscard]] std::pair<int, int> pastePosition(std::uint32_t contentW, std::uint32_t contentH,
                                                std::optional<std::pair<int, int>> source,
                                                std::uint32_t docW, std::uint32_t docH);

// The image flattened over white with alpha forced opaque — what the OS clipboard receives
// (cross-app clipboards are RGB), and the comparison key that recognises our own copy when it
// round-trips back in through Fl::paste.
[[nodiscard]] common::Image flattenedOverWhite(const common::Image& img);

} // namespace mosaic::core
