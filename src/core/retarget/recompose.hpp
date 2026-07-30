#pragma once

// Smart Recompose — object-preserving retargeting core (PLAN S16-f;
// docs/smart-recompose-plan.md §2, commit 3).
//
// The pipeline that serves the case cropping provably cannot (important content at opposite
// edges, large aspect change): cut the keep-regions out with a feathered margin, heal the holes,
// crop the healed background to the target, re-place the regions RIGIDLY — preserving their
// ordering and approximate relative positions, never deforming a kept pixel — and composite.
//
// Lineage: Setlur, Takagi, Raskar, Gleicher & Gooch, "Automatic Image Retargeting" (MUM 2005).
// The standing guardrails apply: one fused importance map, one result, no learned weights, and
// ⚠ THE OPERATOR IS USER-INVOKED — this function is only ever called because the user pressed
// Recompose. Mosaic makes no automatic crop-vs-recompose decision anywhere, and that absence is
// deliberate and load-bearing: do not add one.
//
// The hole filler is injected (`FillFn`) so this stays pure and headless-testable; the
// production adapter wraps the S37 inpaint engine at the UI layer. Deterministic given a
// deterministic filler (the inpaint engine is; project requirement).

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/retarget/keep_regions.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace mosaic::core::retarget {

// Fill the given doc-space holes of `img` in place (true on success). Production = inpaint
// engine adapter; tests = deterministic mock.
using FillFn = std::function<bool(common::Image& img, const std::vector<common::Rect>& holes)>;

struct RecomposeOptions {
    double cutMarginFrac = 0.10; // pad each region's cut by this fraction of its own size (the
                                 // feather band; loose cuts per plan §2 — fork F-a)
    double minGapFrac = 0.02;    // min gap between placed regions, of the target's short side
    int solverMaxSweeps = 100;   // constraint-projection sweeps before declaring infeasible
};

// Where one input region landed in the output (regionIndex -> input order). `target` is the
// region's SNUG rect in output coordinates; the UI's preview/nudge works on these.
struct PlacedRegion {
    std::size_t regionIndex = 0;
    common::Rect target;
};

struct RecomposeResult {
    common::Image image; // the retargeted picture (empty unless ok)
    std::vector<PlacedRegion> placements;
    bool ok = false;
    std::string detail; // why not, when !ok ("regions do not fit", "fill failed", ...)
};

// One cut keep-region, ready to re-place: the padded pixels (never resampled) plus the achieved
// per-side pad — the feather band; document-edge clamping can shrink a side to 0 (stays opaque).
struct RecomposePiece {
    common::Image image;
    double padL = 0.0, padT = 0.0, padR = 0.0, padB = 0.0;
};

// Everything the final composite needs, kept apart so the UI's preview NUDGE can move a
// placement and re-assemble in milliseconds without re-running the hole fill (the expensive
// stage): the healed background already cropped to the target, the cut pieces, and the current
// placements (snug rects, output space — mutate these, then assembleRecompose again).
struct RecomposeStaged {
    common::Image background;           // healed + cropped to targetW x targetH (empty unless ok)
    std::vector<RecomposePiece> pieces; // parallel to the input regions
    std::vector<common::Rect> placed;   // snug placement per region, output space
    std::uint32_t targetW = 0, targetH = 0;
    bool ok = false;
    std::string detail; // why not, when !ok
};

// Run the pipeline through placement + cut + heal + background window — everything but the final
// composite. Same guarantees and failure modes as recompose() (which is prepare + assemble).
[[nodiscard]] RecomposeStaged prepareRecompose(const common::Image& src, double targetAspect,
                                               const std::vector<KeepRegion>& regions,
                                               const FillFn& fill,
                                               const RecomposeOptions& opts = {});

// Composite the staged pieces at their CURRENT placements over a copy of the staged background
// (feathered, never resampled). With `blendSeams` (the default) each piece's pad band is then
// re-solved in the gradient domain (plan §2 step 5) — the band's colours adapt to the
// destination background so nothing reads as a pasted square; the snug core is never touched
// (rigidity). Pass false for the drag-rate live nudge (feather only, ms-cheap; costs the band
// polish until the next blended assemble). Pure and deterministic; empty image when !staged.ok.
[[nodiscard]] common::Image assembleRecompose(const RecomposeStaged& staged,
                                              bool blendSeams = true);

// The pure placement solver (exposed for tests and the UI nudge). Given the regions' snug
// source rects, place them in a targetW x targetH frame: seeded at the proportional position,
// kept fully in-frame, order preserved along each pair's source separation axis with at least
// `minGap` between them, displacement from the seed minimized by symmetric constraint
// projection. Returns one rect per input, or EMPTY when rigid placement is infeasible (a region
// larger than the frame, or a constrained chain that cannot fit).
[[nodiscard]] std::vector<common::Rect>
solvePlacements(const std::vector<common::Rect>& rects, double srcW, double srcH, double targetW,
                double targetH, double minGap, int maxSweeps = 100);

// Run the full pipeline on the flattened composite `src`: target dims are the max-fit box for
// `targetAspect` (like the crop tier — never more pixels than the source). Fails gracefully
// (ok=false + detail) on: no regions, bad aspect, infeasible rigid placement, or filler failure.
[[nodiscard]] RecomposeResult recompose(const common::Image& src, double targetAspect,
                                        const std::vector<KeepRegion>& regions,
                                        const FillFn& fill, const RecomposeOptions& opts = {});

} // namespace mosaic::core::retarget
