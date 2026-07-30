#pragma once

// Smart Resize (content-aware cropping) for Mosaic — the crop-window search
// (PLAN S16-f; docs/smart-resize-research.md §4.3, §9 step 3).
//
// Given the fused importance map W and a target aspect, find the single axis-aligned window
// maximizing
//     score(R) = retained importance mass
//              − λ_cut  · importance mass in a thin band along ∂R   (don't slice structure)
//              − λ_blob · salient blobs partially clipped by R      (no half-objects)
//              − λ_prot · protect rects clipped by R                (never cut a face)
//              + λ_comp · composition(R)                            (thirds / centred placement)
// evaluated in O(1) per window via summed-area tables, swept coarse-to-fine over positions and
// a few uniform scales. Deterministic: fixed scan order, first-found wins ties, no RNG.
//
// ⚠ Standing design guardrails — load-bearing, deliberate, do not "improve" away:
//   1. the search consumes ONLY the single fused ImportanceMap — never separate per-signal
//      saliency maps (the moment tables below are integrals of that same W, not extra maps);
//   2. it returns exactly ONE window — no ranked alternatives, no clustering of candidates,
//      no next-best-non-overlapping suggestion logic;
//   3. every weight is a hand-set constant — nothing is learned from image collections.
//
// Lineage: the saliency-retaining crop-window search follows Suh, Ling, Bederson & Jacobs
// (UIST 2003), Chen et al. 2003, and Liu & Gleicher 2006 ("optimal pan-and-scan"); cropping as
// the artifact-free retargeting operator follows Rubinstein, Gutierrez, Sorkine & Shamir
// (SIGGRAPH Asia 2010). FLTK-free; unit-tested headless in tests/test_retarget.cpp.

#include "common/geometry.hpp" // mosaic::common::Rect
#include "core/retarget/importance_map.hpp"

#include <vector>

namespace mosaic::core::retarget {

// Tunables for chooseCropWindow. Hand-set constants (guardrail 3); the defaults are the
// "balanced" behaviour and every λ trades one benchmark failure mode against another.
struct SmartCropOptions {
    double lambdaCut = 0.5;      // penalty weight: importance mass under the window boundary
    double lambdaComp = 0.08;    // reward weight: thirds/centre placement of the retained mass
    double lambdaBlob = 1.0;     // penalty weight: partially clipped salient blobs
    double lambdaProtect = 8.0;  // near-hard penalty: partially clipped protect rects
    double minScale = 0.7;       // smallest window considered, as a fraction of the max fit
    int scaleSteps = 4;          // uniform scales swept between 1.0 and minScale (inclusive)
    int coarseSteps = 24;        // coarse grid positions per free axis
    double blobThreshold = 0.3;  // blob mask: W >= blobThreshold * max(W)
    double blobMinMassFrac = 0.01; // ignore blobs carrying less than this fraction of total mass
    // Must-keep regions in DOCUMENT space (the enabled keep-region chips feed these — auto
    // blobs and Ctrl-drag user chips alike). The window is steered to contain or cleanly
    // exclude each — never to slice one — via lambdaProtect.
    std::vector<common::Rect> protectRects;
    // Actively-ignored regions in DOCUMENT space (a keep-region chip the user toggled OFF):
    // their importance mass is zeroed before the search, so "off" content stops attracting the
    // window instead of merely losing its protection.
    std::vector<common::Rect> excludeRects;
    // Free-aspect trim mode (targetAspect <= 0): an edge strip is only trimmed while its mass
    // density stays below trimCheapDensity x the map's mean density (so a busy image stays
    // untrimmed) and while at least trimKeepMass of the total mass is retained.
    double trimCheapDensity = 0.35;
    double trimKeepMass = 0.90;
};

// The best crop window for `map` at `targetAspect` (w/h), as a DOCUMENT-space rect. A
// non-positive aspect = Free = SMART TRIM (Suh 2003's thumbnail formulation): shrink the frame
// edge-by-edge over the cheapest (least-important) strips — never into a protect rect — until
// the margins stop being cheap or trimKeepMass would be violated; a busy image stays full. An
// empty map yields the full source rect. The result always lies inside the source bounds; the
// caller snaps it to pixels (ui::snapCropRect) exactly like a hand-drawn rect.
[[nodiscard]] common::Rect chooseCropWindow(const ImportanceMap& map, double targetAspect,
                                            const SmartCropOptions& opts = {});

// Convenience: build the importance map for `src`, then search it (§5.1's pure entry point).
[[nodiscard]] common::Rect chooseCropWindow(const common::Image& src, double targetAspect,
                                            const SmartCropOptions& opts = {},
                                            const ImportanceOptions& mapOpts = {});

} // namespace mosaic::core::retarget
