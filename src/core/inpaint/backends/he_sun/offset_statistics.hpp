#pragma once

// Stage 1 of the He & Sun offset-statistics inpainting backend (PLAN S37-c).
//
// For each KNOWN patch, find the translation offset to its most-similar *other* known patch under
// a non-locality constraint (|s| > tau), then return the dominant offsets by frequency — the few
// translations under which the image best repeats itself. The graph-completion solver (the rest of
// S37-c) stacks the image shifted by these offsets and picks, per hole pixel, which shift to copy.
//
// ⚠ INVARIANT — what this stage computes is pure statistics over translations, gathered with
// INDEPENDENT, exact per-patch nearest-neighbour lookups (a KD-tree). It is deliberately NOT a
// propagation + random-search loop: a patch's offset is NEVER seeded from a neighbouring pixel's,
// and a candidate offset is NEVER perturbed. That is a hard constraint on this file, not an
// accident of implementation — do not "optimise" the search into one, and do not describe the
// search as propagation- or perturbation-based, because it is neither. The statistics are
// insensitive to the NNF (per He & Sun), so the search is also bounded for speed without changing
// this property (see below). Clean-room from the published paper.
//
// Performance: the patch set is uniformly DECIMATED to Params::nnfMaxPatches by a fixed raster
// stride (deterministic — no randomness — so results are reproducible), which bounds the otherwise
// O(n^2) match cost so it no longer grows with image size; the independent queries then run across
// hardware threads. Region cropping + downsample to <=maxRegionW/H is done by the caller
// (extractWorkingRegion). Decimation is a pure data-reduction step: each retained patch is still
// matched by an exact, independent nearest-neighbour lookup.

#include "common/image.hpp"
#include "core/inpaint/inpaint_backend.hpp" // Params
#include "core/selection.hpp"

#include <atomic>
#include <functional>
#include <vector>

namespace mosaic::core::inpaint {

// An integer translation offset (in pixels) between two patches.
struct Offset {
    int u = 0;
    int v = 0;
    friend bool operator==(const Offset&, const Offset&) = default;
};

// Dominant translation offsets over the KNOWN region of `image` (a pixel is in the hole, and thus
// excluded, where `holeMask` coverage > 0; an empty mask means the whole image is known). Returns
// up to `p.K` offsets, most-frequent first; the non-locality threshold is `p.tauFraction *
// max(width, height)`. Empty when the image is smaller than one patch. If `cancel` is non-null the
// (parallel) NN search polls it and bails early — returning {} — so a long Analyzing stage aborts.
// `progress`, if set, is called with the NNF completion fraction (0..1) as batches of queries
// finish, so the multi-second Analyzing stage animates; returning false from it also requests
// cancellation.
[[nodiscard]] std::vector<Offset>
computeDominantOffsets(const common::ImageF& image, const Selection& holeMask, const Params& p,
                       const std::atomic<bool>* cancel = nullptr,
                       const std::function<bool(float)>& progress = {});

// Boundary-driven candidate offsets. The K dominant offsets are FREQUENCY-voted over the whole
// working region, so an offset that is locally precious — the one mapping the image's single
// "continuation" segment onto a structure that ends at the hole — usually loses the global
// ballot, and the fill then has no candidate able to continue that structure (the Skagen
// treeline junction). This pass matches the fully-known patches ADJACENT to the hole (the ring)
// against the known patch set with the same exact KD-tree k-NN (independent queries, no
// propagation — the invariant above holds here too), votes each query's best non-local match,
// and returns the top `p.boundaryOffsets` distinct offsets (count desc, then smaller magnitude,
// then lexicographic — the dominant-offsets ranking). The caller appends them to the dominant
// set (dedup) so the graph cut gains the OPTION; the energy still decides. Candidate generation
// only: the mechanism is Criminisi boundary matching feeding He & Sun's label framework.
// Deterministic; empty when p.boundaryOffsets <= 0 or the ring/patch sets are degenerate.
[[nodiscard]] std::vector<Offset>
computeBoundaryOffsets(const common::ImageF& image, const Selection& holeMask, const Params& p,
                       const std::atomic<bool>* cancel = nullptr);

// Outpaint shift-candidate ladder. A canvas-expansion strip's far pixels need a label whose
// source is BENIGN adjacent content, and the frequency vote rarely supplies one (it finds
// self-similar structure — the labels that duplicate
// objects). For each mostly-hole side of `regionHoleMask` (the working-region hole), returns a
// geometric ladder of axis-aligned INWARD shifts spanning that side's strip depth up to the
// region extent, then — where two adjacent sides are both mostly-hole (an expansion CORNER,
// whose pixels no axis-aligned shift can source: the horizontal escape lands in the vertical
// strip and vice versa) — a shorter ladder of DIAGONAL inward shifts pairing the two sides'
// depths, so the corner block has a valid benign source on the menu at all. The energy still
// decides; this is candidate generation only, in region-pixel units, deterministic, internally
// deduped (order: left/right/top/bottom axis rungs, then TL/TR/BL/BR corner rungs). `pad`
// spaces each first rung past the strip depth (callers pass max(2, patchSize)). Sides whose
// depth is 0 or the full region extent (degenerate) contribute nothing; corners require both
// adjacent sides. Restricted candidate generation atop the existing engine — no new mechanism.
[[nodiscard]] std::vector<Offset> outpaintShiftCandidates(const Selection& regionHoleMask,
                                                          int pad);

// Refine coarsely-quantized dominant offsets at FULL resolution. Offsets gathered on a
// downsampled working region come back as multiples of the region
// scale — quantized by up to ±scale/2 per axis — which visibly misaligns structure the fill
// carries across a hole (a treeline or horizon lands a few pixels off; the seam objective cannot
// repair what no candidate can express). Each offset is re-anchored by EXHAUSTIVE local search:
// every integer offset within ±radius is scored by the mean patch SSD between P(x) and
// P(x+offset') over a deterministic stride grid of sample positions near the hole where both
// patches are fully known, and the argmin wins (ties: smaller adjustment, then lexicographic).
// Duplicates after refinement are dropped (first wins). radius <= 0 returns the input unchanged.
//
// ⚠ INVARIANT — this is classic exhaustive block matching (full-search motion estimation, 1980s
// video coding): every candidate is evaluated INDEPENDENTLY and exactly. No propagation between
// pixels, and no per-pixel free-shift optimization — the label set stays the K dominant offsets.
// He & Sun's own offsets are full-resolution; this step only removes a deviation our
// working-region downsample introduced.
[[nodiscard]] std::vector<Offset>
refineOffsetsFullRes(const common::ImageF& image, const Selection& holeMask,
                     const std::vector<Offset>& offsets, int radius, int patchSize,
                     const std::atomic<bool>* cancel = nullptr);

// Apply an α-expansion labeling: produce the completed image by copying each hole pixel from its
// chosen source (x + offsets[label]). `labels` is indexed in hole-node order — the row-major scan
// of holeMask coverage>0 — exactly as the graph-completion solver enumerates its variable nodes.
// Known pixels are kept. A hole pixel is filled ONLY from a source that is in-bounds AND known; a
// source that is out of bounds or itself still in the hole would copy the content being removed
// (the origin of stray white/black specks on large holes), so such pixels — and any with a negative
// (no-source) label — are neighbour-filled by a small bounded diffusion instead. The fill is
// order-independent, hence deterministic. The caller corrects labels for validity first, so this is
// a hard guarantee rather than the normal path.
[[nodiscard]] common::ImageF applyOffsetLabels(const common::ImageF& image,
                                               const Selection& holeMask,
                                               const std::vector<Offset>& offsets,
                                               const std::vector<int>& labels);

} // namespace mosaic::core::inpaint
