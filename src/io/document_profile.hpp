#pragma once

#include "common/exif.hpp"
#include "common/image.hpp"
#include "core/document.hpp"
#include "io/io.hpp" // EmbeddedMetadata -- documentMetadata's return type

#include <cstdint>
#include <optional>

// io/document_profile -- WHAT THE DOCUMENT ACTUALLY USES, probed from the model (Export & I/O
// plan §4). One half of the loss system: the other half is FormatCaps (io/caps.hpp, what the
// target format can carry), and diff() is the pure function between them.
//
// The struct is deliberately a flat bag of facts with no pointers into the document: once
// profiled, the export pipeline can hold it, hash it, cache against it and diff it on every
// slider tick without touching the layer tree again.
namespace mosaic::io {

struct DocumentProfile {
    // ---- structure ----
    int layerCount = 0;              // every layer in the tree, hidden ones included: an export
                                     // drops them entirely, so they are a loss too
    bool hasMultipleLayers = false;  // layerCount > 1
    bool hasVector = false;          // a VectorLayer carrying geometry
    bool hasText = false;            // a TextLayer carrying text
    bool hasEffects = false;         // a layer with a non-empty LayerEffects (LE-a)
    bool hasExtrude3d = false;       // a TextLayer whose block has an Extrude (S30-c)
    bool hasAdjustments = false;     // an AdjustmentLayer (baked by the flatten)
    bool usesBlendModes = false;     // any layer off BlendMode::Normal, or opacity < 1
    bool usesSoftMask = false;       // a RasterMask with partial (not 0/255) coverage

    // ---- pixels ----
    bool hasAlpha = false;      // the flatten carries transparency (see the note on the
                                // structural probe below -- exact only with a flatten in hand)
    int distinctColors = -1;    // distinct RGBA values in the flatten, or -1 when uncounted /
                                // above the counting cap (i.e. "treat as truecolour")
    core::Precision precision = core::Precision::U8;  // the document's declared working
                                                      // precision -- INTENT (§5 high-bit note)

    // What the export pipeline can actually hand an encoder today. Kept separate from
    // `precision` on purpose: composite() still collapses to 8-bit (§5), so a default document
    // (Precision::F16!) must NOT raise a permanent "HDR will be clipped" banner. When the ImageF
    // tap lands (S43-a) the extractor raises these two and the depth warnings in diff() go live
    // with no change to diff() itself.
    int sourceBitDepth = 8;
    bool sourceIsFloat = false;

    // ---- colour & metadata ----
    bool hasICC = false;           // a per-document ICC working profile is set
    bool hasNonSrgbSpace = false;  // colorSpace() is not plain sRGB
    bool hasEXIF = false;          // some layer carries camera metadata (the S41 read slice)
    bool hasXMP = false;           // no XMP in the model yet -- always false; the field exists so
                                   // M4's metadata work has somewhere to put it
    double dpi = 72.0;

    // ---- vector detail (only meaningful against a vector target: SVG/PDF/EPS) ----
    bool usesConicGradient = false;  // vec::GradientType::Conic -- no SVG 1.1 equivalent
    bool usesStrokeAlign = false;    // vec::StrokeAlign::Inside/Outside -- SVG strokes are
                                     // centre-only; export outlines them to a fill
    bool usesDashes = false;         // a dashed stroke (survives SVG/PDF, lost by EPS flatten)

    bool operator==(const DocumentProfile&) const = default;
};

// Probe `doc` structurally. Every field but `hasAlpha` and `distinctColors` is exact.
//
// `hasAlpha` is CONSERVATIVE here: without the flatten the honest answer is "yes unless some
// visible layer provably seals the whole canvas opaquely" (documentIsProvablyOpaque below), so
// this over-reports rather than promising an opacity the export would not deliver. Use the
// overload below wherever the composite already exists -- which is everywhere in the export
// pipeline, because §5 composites first. `distinctColors` stays -1 (uncounted).
[[nodiscard]] DocumentProfile profileDocument(const core::Document& doc);

// Same probe, with the already-composited flatten in hand: `hasAlpha` becomes an exact scan and
// `distinctColors` an exact count (capped at `colorCountLimit`; above the cap it stays -1, which
// every consumer reads as "truecolour"). This is the overload the Export modal uses.
[[nodiscard]] DocumentProfile profileDocument(const core::Document& doc,
                                              const common::Image& flattened,
                                              int colorCountLimit = 256);

// ---------------------------------------------------------------------------------------------
// The document's EXIF provenance (M5)
// ---------------------------------------------------------------------------------------------

// The camera metadata an export of `doc` should write, or nullopt for none.
//
// EXIF lives PER LAYER in this model (core::Layer::exif(), stamped by the open path), and an
// export is a flatten of many layers -- so "the document's EXIF" is a question the model does not
// answer and somebody has to decide. The rule, and why it is this one:
//
//   Among the layers that could be where a photograph ENTERED this document -- a Raster or Magic
//   layer, the only two kinds io::loadImageWithMetadata ever stamps -- that are EFFECTIVELY
//   VISIBLE (their own flag and every enclosing group's) and carry a non-empty record, take the
//   one with the SMALLEST LAYER ID. Nothing qualifies => nullopt, and the export writes no EXIF.
//
// Smallest id means EARLIEST MINTED: ids are monotonic and never reused (Document::mintLayerId),
// so for the ordinary case -- File ▸ Open on a photo, which creates exactly one layer before any
// edit can happen -- it names precisely the layer the document was opened from, which is the
// provenance we actually want to record. It was chosen over "the bottom-most visible one" (the
// obvious alternative) because stack POSITION is something a user rearranges freely in the layer
// dock, and dragging a layer must not silently rewrite which camera the exported file claims to
// have come from. Visibility is required for the opposite reason: metadata describing a photo
// that contributes nothing to the exported pixels is worse than no metadata at all.
//
// The returned record has already been through io::exifForExport, so its orientation reads 1.
[[nodiscard]] std::optional<common::ExifData> documentExif(const core::Document& doc);

// Everything a side-car-capable encoder needs from `doc`, in ONE call: the provenance EXIF
// serialised into its wire payload (documentExif above), the profile bytes to embed
// (core::documentIccProfile -- a custom .icc, else the working space, else nothing for plain
// sRGB), and the print density. `keepMetadata == false` -- §6's Metadata toggle off -- yields an
// EMPTY record, which every writer reads as "write nothing at all".
//
// It exists so the export entry points cannot drift apart about what an export of this document
// carries. There are four of them (the modal, "Export to <file>", and the three Quick Exports),
// and four independent answers to "does a quick export keep the camera data?" is exactly the kind
// of inconsistency a user notices and nobody can explain.
[[nodiscard]] EmbeddedMetadata documentMetadata(const core::Document& doc,
                                                bool keepMetadata = true);

// True when any pixel is not fully opaque.
[[nodiscard]] bool imageHasTransparency(const common::Image& image) noexcept;

// The number of distinct RGBA values in `image`, or -1 as soon as more than `limit` are seen
// (so a photograph costs one early bail-out, not a set the size of the image). An empty image
// has 0 distinct colours. (Not noexcept: the bounded working set is a hash table.)
[[nodiscard]] int countDistinctColors(const common::Image& image, int limit);

// True when some layer provably makes the flatten opaque: a visible, unclipped, unmasked,
// effect-free RasterLayer at full opacity in Normal blend, sitting untransformed over the whole
// canvas with no transparent pixel in it. Alpha only ever grows as the stack composites (`over`
// never lowers the destination's alpha, and no blend mode in core::BlendMode erases), so one
// such layer anywhere in the stack settles the question. Narrow and conservative by design:
// a false "no" only costs an over-cautious warning, a false "yes" would be a lie.
[[nodiscard]] bool documentIsProvablyOpaque(const core::Document& doc) noexcept;

} // namespace mosaic::io
