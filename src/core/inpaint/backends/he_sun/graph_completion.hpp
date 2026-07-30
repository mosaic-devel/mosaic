#pragma once

// Algorithm: K. He and J. Sun, "Image Completion Approaches Using the Statistics of Similar
// Patches," IEEE TPAMI 36(12), 2014 (prelim. ECCV 2012). Clean-room from the publications.
//
// ⚠ INVARIANT — the preprocessing follows the papers ONLY, never the widely-productised recipe
// built on top of them: no reflection padding, no downscale to a fixed ~100x75 working image, no
// Laplacian edge mask. Those three steps are deliberately absent and must stay absent.
//
// He & Sun graph-based completion (PLAN S37-c): fill the hole by choosing, per hole pixel,
// one of the dominant offsets via α-expansion — a validity data term (0 if x+offset lands on a
// known pixel, else infinite) plus a position-dependent seam-coherence smoothness term — then copy
// each hole pixel from its chosen source. Built clean-room from He & Sun, over our own graph cut.
//
// First implementation runs at full resolution; the working-region crop + downsample
// (extractWorkingRegion) and a KD-tree NNF are the documented optimizations still to wire in.

#include "common/image.hpp"
#include "core/inpaint/backends/he_sun/offset_statistics.hpp" // Offset
#include "core/inpaint/inpaint_backend.hpp"                   // Params
#include "core/selection.hpp"

#include <algorithm>
#include <vector>

namespace mosaic::core::inpaint {

// Complete `image` over the hole (holeMask coverage > 0) using the candidate `offsets`. Returns the
// filled image with known pixels unchanged; returns the input unchanged if there are no offsets or
// no hole. If `timings` is non-null, per-stage wall-clock costs (graph-cut, poisson-blend, …) are
// appended for the diagnostic breakdown. If `prog` is non-null it drives the back portion of the
// progress bar ("Solving" during the graph cut, "Blending" during the seam blend), streams a live
// preview (the composite the instant the graph cut finishes, then each seam-blend refinement), and
// aborts early when the host requests cancellation (prog->cancelled() is then true).
[[nodiscard]] common::ImageF graphComplete(const common::ImageF& image, const Selection& holeMask,
                                           const std::vector<Offset>& offsets, const Params& p,
                                           std::vector<StageTiming>* timings = nullptr,
                                           ProgressReporter* prog = nullptr);

// Outpaint structure penalty: the per-pixel data-cost map labels pay for sourcing from
// strongly structured or locally deviant content during a canvas-expansion fill (rationale at the
// build function's definition). at() clamps and scales by `weight`.
// Exposed for tests; graphComplete() builds and consumes it internally, for outpaint holes only.
struct StructurePenalty {
    std::vector<float> map; // 0..1 per pixel, at the owning image's scale
    long w = 0;
    long h = 0;
    double weight = 0.0;
    [[nodiscard]] double at(long sx, long sy) const {
        if (map.empty()) {
            return 0.0;
        }
        sx = std::clamp(sx, 0L, w - 1);
        sy = std::clamp(sy, 0L, h - 1);
        return weight *
               static_cast<double>(map[static_cast<std::size_t>(sy) * static_cast<std::size_t>(w) +
                                       static_cast<std::size_t>(sx)]);
    }
};

// Build the map: robustly normalised structure-tensor anisotropy, low-energy-damped by dampFrac
// (≤ 0 disables), max-combined with the local-deviation band-pass term scaled by devFrac (≤ 0
// disables, restoring the anisotropy-only map bit-for-bit); both terms are zeroed within the
// near-boundary guard margin, and the anisotropy is max-dilated before the combine.
[[nodiscard]] StructurePenalty buildStructurePenalty(const common::ImageF& img,
                                                     const Selection& holeMask, double weight,
                                                     double dampFrac, double devFrac);

// Resolve each hole pixel's copy chain `p -> p+offset(label(p)) -> ...` to its KNOWN endpoint and
// return the per-node EFFECTIVE offset (node order = row-major hole scan, `labels`' order). A
// pixel whose chain cycles or dead-ends inherits a resolved 4-neighbour's effective offset when
// that offset is valid from its own position (sheet extension — the frame-edge strip fix); only
// pixels no sweep can reach come back as the no-source sentinel {INT_MIN, INT_MIN} (synthesis
// neighbour-fills those). `nodeOf` maps y*W+x -> node index or -1 for known. Deterministic.
// Exposed for tests; graphComplete() is the production caller.
[[nodiscard]] std::vector<Offset> resolveEffectiveOffsets(const Selection& holeMask,
                                                          const std::vector<int>& nodeOf, long W,
                                                          long H,
                                                          const std::vector<Offset>& offsets,
                                                          const std::vector<int>& labels);

} // namespace mosaic::core::inpaint
