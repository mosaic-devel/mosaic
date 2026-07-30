#include "core/inpaint/backends/he_sun/offset_stats_backend.hpp"

#include "common/log.hpp"
#include "core/inpaint/backends/he_sun/graph_completion.hpp"
#include "core/inpaint/backends/he_sun/offset_statistics.hpp"
#include "core/inpaint/backends/he_sun/working_region.hpp"
#include "core/inpaint/outpaint.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace mosaic::core::inpaint {

InpaintResult OffsetStatisticsBackend::run(const InpaintRequest& request,
                                           const ProgressFn& progress) {
    InpaintResult res;
    res.image = request.image;
    if (request.image.empty()) {
        res.ok = false;
        res.detail = "empty image";
        return res;
    }
    ProgressReporter prog(progress, request.cancel);
    if (!prog.report(0.02f, "Analyzing")) {
        res.ok = false;
        res.detail = "cancelled";
        return res;
    }

    // Gather dominant offsets on the cropped (+downsampled) working region around the hole, so the
    // O(n^2) brute-force NNF cost scales with the hole's neighbourhood, not the whole image. (The
    // graph-completion fill itself is already hole-bounded.) Offsets come back in region pixels and
    // are rescaled to full resolution by the downsample factor.
    std::vector<Offset> offsets;
    {
        ScopedStage offsetStage(&res.timings, "offset-stats");
        const WorkingRegion wr =
            extractWorkingRegion(request.image, request.holeMask, request.params);
        if (!wr.image.empty()) {
            Selection regionMask(wr.image.width, wr.image.height);
            std::vector<std::uint8_t>& d = regionMask.data();
            const long w = static_cast<long>(request.image.width);
            const long h = static_cast<long>(request.image.height);
            for (std::uint32_t ry = 0; ry < wr.image.height; ++ry) {
                for (std::uint32_t rx = 0; rx < wr.image.width; ++rx) {
                    bool anyHole = false;
                    for (int dy = 0; dy < wr.scale && !anyHole; ++dy) {
                        for (int dx = 0; dx < wr.scale && !anyHole; ++dx) {
                            const long sx = wr.originX + static_cast<long>(rx) * wr.scale + dx;
                            const long sy = wr.originY + static_cast<long>(ry) * wr.scale + dy;
                            if (sx < w && sy < h && !request.holeMask.isEmpty() &&
                                sx < static_cast<long>(request.holeMask.width()) &&
                                sy < static_cast<long>(request.holeMask.height()) &&
                                request.holeMask.at(static_cast<std::uint32_t>(sx),
                                                    static_cast<std::uint32_t>(sy)) > 0) {
                                anyHole = true;
                            }
                        }
                    }
                    if (anyHole) {
                        d[static_cast<std::size_t>(ry) * wr.image.width + rx] = 255;
                    }
                }
            }
            // The NNF is the bulk of "Analyzing"; tick it across 0.05..0.43 so the multi-second
            // search animates instead of snapping from 2% straight to 45%.
            std::vector<Offset> regionOffsets = computeDominantOffsets(
                wr.image, regionMask, request.params, request.cancel,
                [&prog](float f) { return prog.report(0.05f + 0.38f * f, "Analyzing"); });
            // Boundary-driven candidates (§3.7.7): what the ring needs to continue, which the
            // global frequency vote may have missed. Appended (deduped) so the cut gains the
            // option; the seam energy still decides where — if anywhere — they win.
            for (const Offset& b :
                 computeBoundaryOffsets(wr.image, regionMask, request.params, request.cancel)) {
                if (std::find(regionOffsets.begin(), regionOffsets.end(), b) ==
                    regionOffsets.end()) {
                    regionOffsets.push_back(b);
                }
            }
            // §3.7.8 outpaint shift candidates: an expansion strip's far pixels need a valid
            // label whose source is BENIGN adjacent content, and the frequency vote rarely
            // supplies one (it finds self-similar structure, e.g. tower↔tower symmetry — the
            // labels that duplicate objects). Per mostly-hole side, a ladder of axis-aligned
            // INWARD shifts spanning the strip's depth, plus diagonal rungs where two strips
            // meet in a corner (whose axis escapes each land in the other strip); the energy
            // (with the structure penalty) still decides. Region-pixel units, appended BEFORE
            // the full-resolution rescale below — the first wiring appended them after it, so
            // the whole ladder was silently dead and the Broadway results came from the
            // structure penalty alone (found 2026-07-11; the strip wiring test pins this).
            if (isOutpaintHole(request.holeMask, request.image.width, request.image.height)) {
                for (const Offset& s : outpaintShiftCandidates(
                         regionMask, std::max(2, request.params.patchSize))) {
                    if (std::find(regionOffsets.begin(), regionOffsets.end(), s) ==
                        regionOffsets.end()) {
                        regionOffsets.push_back(s);
                    }
                }
            }
            offsets.reserve(regionOffsets.size());
            for (const Offset& o : regionOffsets) {
                offsets.push_back({o.u * wr.scale, o.v * wr.scale});
            }

            // A downsampled region quantizes offsets to multiples of wr.scale (±scale/2 per
            // axis) — enough to visibly misalign a treeline or horizon carried across the hole.
            // Re-anchor each candidate at FULL resolution by exhaustive local block matching
            // (docs §3.7.5); scale 1 needs none.
            if (wr.scale > 1) {
                const int radius = (wr.scale + 1) / 2 + 1;
                offsets = refineOffsetsFullRes(request.image, request.holeMask, offsets, radius,
                                               request.params.patchSize, request.cancel);
            }
        }
    } // offset-stats stage

    if (prog.cancelled()) {
        res.ok = false;
        res.detail = "cancelled";
        return res;
    }
    if (offsets.empty()) {
        res.ok = false;
        res.detail = "no dominant offsets (region smaller than a patch or no usable known region)";
        return res;
    }
    if (!prog.report(0.45f, "Analyzing")) {
        res.ok = false;
        res.detail = "cancelled";
        return res;
    }

    // Fill at full resolution with the (rescaled) offsets; the graph cut is bounded by the hole.
    // graphComplete drives the 0.45..1.0 span of the bar ("Solving" then "Blending") and streams a
    // live preview (the composite the instant the graph cut finishes, then the seam blend
    // refining).
    res.image = graphComplete(request.image, request.holeMask, offsets, request.params,
                              &res.timings, &prog);
    res.detail = "He & Sun offset-statistics graph completion";
    if (prog.cancelled()) {
        res.ok = false;
        res.detail = "cancelled";
        return res;
    }

    // Per-stage ms breakdown to the "core" log when MOSAIC_INPAINT_TIMING is set (the editor
    // freezes on the UI thread during a synchronous inpaint, so stderr is the only live signal of
    // progress).
    if (std::getenv("MOSAIC_INPAINT_TIMING") != nullptr) {
        double total = 0.0;
        std::string line = "inpaint offset-stats timings:";
        for (const StageTiming& t : res.timings) {
            line += ' ' + t.name + '=';
            line += std::to_string(static_cast<long>(t.ms)) + "ms";
            total += t.ms;
        }
        line += " total=" + std::to_string(static_cast<long>(total)) + "ms";
        common::log::category("core")->info("{}", line);
    }

    prog.report(1.0f, "Blending", &res.image);
    return res;
}

BackendInfo OffsetStatisticsBackend::info() const {
    BackendInfo bi;
    bi.displayName = "Offset statistics";
    bi.method = "Patch-offset graph completion";
    bi.authors = "Kaiming He & Jian Sun, 2014";
    bi.paper = "Image Completion Approaches Using the Statistics of Similar Patches (PAMI 2014)";
    bi.summary =
        "Finds the dominant ways the image repeats itself (offsets between similar patches), then "
        "rebuilds the hole by stitching shifted copies together with a graph cut and a Poisson "
        "seam blend. Best for removing objects from textured or structured scenes — it reuses real "
        "content instead of smearing.";
    bi.deviations = {
        "Exact KD-tree nearest-neighbour matching (clean-room), not PatchMatch.",
        "Offsets gathered on a cropped, downsampled working region so cost scales with the hole, "
        "then re-anchored at full resolution by exhaustive local block matching (no quantized "
        "misalignment).",
    };
    bi.augmentations = {
        "Boundary-driven candidate offsets: patches at the hole's edge vote for the offsets "
        "that continue THEIR content, joining the frequency-voted dominant set (Criminisi-"
        "style matching, expired 2023).",
        "Two-scale graph cut with a node budget, then banded full-resolution re-cuts along the "
        "seams (seam fronts land on true features, not coarse-block boundaries), plus one "
        "wide-corridor re-solve of the worst residual seam.",
        "Colours-&-gradients seam objective (Kwatra 2003 / Agarwala 2004) with the paper's E1 "
        "boundary anchoring.",
        "Copy-chain resolution: deep-interior pixels compose their copy chains to known content, "
        "so removed content never echoes back into the fill.",
        "Multigrid-accelerated Poisson seam blend with a projected red-black SOR polish "
        "(converges the low frequencies fixed sweep counts cannot).",
        "Outpaint mode for canvas expansions: inward shift-ladder and corner-diagonal "
        "candidates reach past foreground objects, and donors carrying strong structure pay a "
        "data cost so objects aren't duplicated into the new margin. Two further experimental "
        "levers (crisp boundary blending, atypical-donor tax) are off-by-default settings. "
        "Interior heals are untouched by all of it.",
    };
    bi.cost = "Large removals ~10-15 s at full resolution; CPU, multithreaded; memory scales "
              "with the working region";
    return bi;
}

BackendSettingsSchema OffsetStatisticsBackend::settingsSchema() const {
    BackendSettingsSchema s;
    // Curated controls; the remaining Params internals stay at their defaults. Keys map onto Params
    // in applyParam() below. Defaults mirror the "balanced" preset (the Params defaults).
    s.controls = {
        {"adaptiveSmallRegion", "Low-effort on small selection",
         "Sample only the area immediately around a small selection — much faster for small "
         "touch-ups. Large selections still use the full surrounding context.",
         ParamControl::Kind::Bool, 0, 1, 1, {}, 1 /*on*/, /*advanced*/ false},
        {"showSampleArea", "Show sampled area while working",
         "Tint the area the engine is analysing (a faint green wash) during an inpaint, so you can "
         "see what it's sampling.",
         ParamControl::Kind::Bool, 0, 1, 1, {}, 0 /*off*/, /*advanced*/ false},
        {"K", "Dominant offsets (K)",
         "How many repeating shift directions to consider. More can find better matches but is "
         "slower.",
         ParamControl::Kind::Int, 8, 120, 1, {}, 60, /*advanced*/ false},
        {"patchSize", "Patch size",
         "Side of the square patches compared when measuring how the image repeats.",
         ParamControl::Kind::Int, 4, 16, 1, {}, 8, /*advanced*/ false},
        {"maxRegion", "Working-region cap (px)",
         "The hole's neighbourhood is downsampled to fit this before analysis — smaller is faster, "
         "larger keeps more detail.",
         ParamControl::Kind::Int, 200, 1600, 100, {}, 800, /*advanced*/ true},
        {"graphCutMaxNodes", "Graph-cut node budget",
         "Caps the graph-cut size for big holes (they are solved coarse, then upscaled). Higher is "
         "sharper but slower.",
         ParamControl::Kind::Int, 2000, 20000, 1000, {}, 6000, /*advanced*/ true},
        {"poissonIterations", "Blend iterations",
         "Seam-blend relaxation sweeps. Small holes use exactly this many; large holes converge "
         "through a multigrid pre-solve first and cap the polish, so raising this mainly "
         "affects small touch-ups.",
         ParamControl::Kind::Int, 50, 400, 10, {}, 200, /*advanced*/ true},
        // The two §3.7.8 second-addendum levers, OFF by default: user testing found regressions
        // on real photos (sky-only expansion fills, sky favored near the ground), so they are
        // opt-in experiments until run to ground. They only ever affect canvas-expansion fills.
        {"outpaintBoundaryCrisp", "Expansion: crisp edge blending (experimental)",
         "When filling a canvas expansion, keep mismatched content at the old frame edge as a "
         "crisp edge instead of blending it smooth. Has no effect on ordinary heals.",
         ParamControl::Kind::Bool, 0, 1, 1, {}, 0 /*off*/, /*advanced*/ true},
        {"outpaintDeviationTax", "Expansion: avoid atypical donors (experimental)",
         "When filling a canvas expansion, avoid copying content that stands out from its "
         "surroundings (such as a stray cloud). Can bias fills toward flat areas. Has no effect "
         "on ordinary heals.",
         ParamControl::Kind::Bool, 0, 1, 1, {}, 0 /*off*/, /*advanced*/ true},
    };
    s.presets = {
        {"fast",
         "Fast",
         {{"K", 24},
          {"patchSize", 8},
          {"maxRegion", 400},
          {"graphCutMaxNodes", 3000},
          {"poissonIterations", 100}}},
        {"balanced",
         "Balanced",
         {{"K", 60},
          {"patchSize", 8},
          {"maxRegion", 800},
          {"graphCutMaxNodes", 6000},
          {"poissonIterations", 200}}},
        {"best",
         "Best",
         {{"K", 100},
          {"patchSize", 8},
          {"maxRegion", 1200},
          {"graphCutMaxNodes", 12000},
          {"poissonIterations", 300}}},
    };
    s.defaultPreset = "balanced";
    return s;
}

void OffsetStatisticsBackend::applyParam(Params& params, const std::string& key,
                                         double value) const {
    const auto asInt = [&] { return static_cast<int>(std::lround(value)); };
    if (key == "adaptiveSmallRegion") {
        params.adaptiveSmallRegion = value != 0.0;
    } else if (key == "showSampleArea") {
        params.showSampleArea = value != 0.0;
    } else if (key == "K") { // (showSampleArea is UI-only; the run ignores it)
        params.K = asInt();
    } else if (key == "patchSize") {
        params.patchSize = asInt();
    } else if (key == "maxRegion") {
        // One user knob caps both region axes (the downsample uses them as targets).
        params.maxRegionW = asInt();
        params.maxRegionH = asInt();
    } else if (key == "graphCutMaxNodes") {
        params.graphCutMaxNodes = asInt();
    } else if (key == "poissonIterations") {
        params.poissonIterations = asInt();
    } else if (key == "outpaintBoundaryCrisp") {
        params.outpaintBoundaryCrisp = value != 0.0;
    } else if (key == "outpaintDeviationTax") {
        params.outpaintDeviationTax = value != 0.0;
    }
}

std::optional<common::Rect>
OffsetStatisticsBackend::analysedRegion(std::uint32_t imageW, std::uint32_t imageH,
                                        const std::optional<common::Rect>& holeBounds,
                                        const Params& params) const {
    if (imageW == 0 || imageH == 0)
        return std::nullopt;
    return workingRegionRect(imageW, imageH, holeBounds, params);
}

} // namespace mosaic::core::inpaint
