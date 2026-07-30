#pragma once

// Smart Resize / Smart Recompose — keep-regions: the "what matters in this picture" model
// (PLAN S16-f; docs/smart-recompose-plan.md §1-§2, commit 2).
//
// A KeepRegion is one must-keep area of the document. Regions are detected AUTOMATICALLY from
// the fused importance map's salient blobs (the same blob machinery the crop search uses — moved
// here so both tiers share one implementation and, per the standing guardrails, one map). The UI
// shows them as editable chips (settled with user 2026-07-01: automatic by default, manual
// marking is refinement, not a hidden mode); the crop search consumes them as protect rects; the
// recompose pipeline cuts and rigidly re-places them.
//
// Face detection (F1, Viola-Jones) was scoped and DROPPED (user call 2026-07-02: shipping
// cascade data / training a clean model isn't worth the marginal chip quality — the importance
// map alone is good enough). The faceRects parameter and the Source::Face slot stay as the hook
// a future detector would use; callers pass none.
//
// Deterministic (fixed scan order, stable ordering, no RNG); FLTK-free; tested headless in
// tests/test_retarget.cpp.

#include "common/geometry.hpp" // mosaic::common::Rect
#include "core/retarget/importance_map.hpp"

#include <cstdint>
#include <vector>

namespace mosaic::core::retarget {

// One connected high-importance component of the map, in MAP space (half-open bbox). Shared by
// the crop search (blob-wholeness penalty) and keep-region extraction.
struct ImportanceBlob {
    std::uint32_t x0, y0, x1, y1; // half-open bounding box, map space
    double massFrac = 0.0;        // component mass / total map mass
    std::uint32_t cells = 0;      // component size in map cells (speckle guard)
};

// 4-connected components of {W >= absThreshold} by iterative flood fill in scan order; blobs
// carrying less than `minMassFrac` of `totalMass` are dropped. `totalMass` <= 0 yields none.
[[nodiscard]] std::vector<ImportanceBlob> findImportanceBlobs(const ImportanceMap& map,
                                                              double absThreshold,
                                                              double minMassFrac,
                                                              double totalMass);

// One must-keep region, in DOCUMENT space.
struct KeepRegion {
    enum class Source : std::uint8_t {
        Auto, // detected from the importance map's blobs
        Face, // reserved for a face detector (F1 was dropped 2026-07-02; hook kept)
        User, // hand-marked (chip add / selection tools)
    };
    common::Rect rect;     // snug doc-space bounding box (cut margins are the pipeline's job)
    double massFrac = 0.0; // share of total importance mass (0 for Face/User regions)
    Source source = Source::Auto;
};

// Tunables for extractKeepRegions. Hand-set constants (guardrail 3); thresholds are
// deliberately stricter than the crop search's internal blob pass — chips must stay few and
// meaningful, not one per pebble.
struct KeepRegionOptions {
    double blobThreshold = 0.3; // blob mask: W >= blobThreshold * max(W)
    // Mass floor: low enough that a small-but-genuine subject (the founding scenario's person —
    // 73 map cells at 0.0029 of total mass on the seam-carving beach photo) still earns a chip;
    // the cell floor below is what keeps single-cell texture speckle out.
    double blobMinMassFrac = 0.002; // ignore blobs below this share of the total mass
    std::uint32_t blobMinCells = 4; // ...and components smaller than this many map cells
    int maxRegions = 8;             // keep the top-N by mass (legibility cap; faces exempt)
    std::uint32_t mergeGapCells = 1; // merge blobs whose map bboxes touch within this many cells
    // Support expansion (the "castle spire" fix, 2026-07-02): each kept blob grows by a BOUNDED
    // GEODESIC DILATION of its strict cells into the support level — the same ONE map re-read at
    // supportThresholdFrac × the detection threshold — so a faint extremity of a detected object
    // (a turret cap against sky) joins its chip instead of surviving outside it. The dilation
    // walks cell-by-cell and stops after a reach proportional to the blob's size (hand-set
    // constants in the .cpp), which is what separates a protrusion (near the strict mass) from a
    // low-threshold flood like a cloud bank (connected, but far) — a bbox-union of support blobs
    // cannot make that distinction (verified on the beach photo: the turret caps connect to the
    // cloud field at support level). Outside (0,1) disables the pass.
    double supportThresholdFrac = 0.5;
};

// Detect the automatic keep-regions of `map`: salient blobs, near-touching ones merged, ordered
// by descending mass (stable), capped at maxRegions, converted to snug doc-space rects.
// `faceRects` (doc space; the dormant face hook) are appended as Source::Face regions, clamped
// to the document, never merged or capped — a face is always its own region. Empty map => none.
[[nodiscard]] std::vector<KeepRegion>
extractKeepRegions(const ImportanceMap& map, const KeepRegionOptions& opts = {},
                   const std::vector<common::Rect>& faceRects = {});

} // namespace mosaic::core::retarget
