#pragma once

#include "common/image.hpp"    // Image (the grow's gradient source)
#include "core/selection.hpp"  // Selection (seeds in, coverage out)

// The edge-aware select brush's grow engine (L1): the
// stroke's pixels are geometric seeds; the selection grows outward by an edge-weighted (geodesic)
// distance transform and stops where the image gradient is high; the smooth distance field is
// thresholded through a ramp into Mosaic's 8-bit AA coverage mask (core::Selection).
//
// Lineage (the published art this engine descends from): Bai & Sapiro geodesic matting (2007);
// Criminisi & Sharp geodesic forests (2008); Toivanen's raster-scan geodesic distance transform
// (1996); Sobel gradient magnitude (textbook).
//
// Load-bearing design invariants, marked where they bind. These are deliberate, not oversights --
// do not relax them:
//   I1 -- a PURE edge-weighted distance grow: no foreground/background/bias cost values, no
//         cost-based fg/bg classification, no graph cut, no colour/appearance model built from
//         the stroke. The stroke supplies geometric seeds ONLY.
//   I2 -- the per-pixel weight is the image's own gradient magnitude (an edge/boundary term),
//         never similarity to a seed / "center pixel" colour.
//   I3 -- no machine learning: the geodesic field IS the answer, thresholded to a mask.
// A fourth invariant (solve on release, not during the stroke) binds in the tool, not here -- this
// function is a pure batch transform with no notion of an in-progress stroke.
namespace mosaic::core {

// The edge brush's knobs (options bar). Deliberately minimal -- the whole behaviour is "how far"
// and "how hard edges stop it"; everything else is the standing AA convention.
struct EdgeGrowParams {
    // How far the selection grows from the stroke, in flat-image pixels. This is the geodesic
    // threshold: over flat colour a step costs 1/px so `reach` reads directly as pixels; crossing
    // an edge consumes reach much faster (see kEdgeLambdaMax in the .cpp), which is what makes
    // the grow stop there.
    double reach = 64.0;
    // In [0,1]: how strongly image edges arrest the grow. 0 = edges are ignored entirely (the
    // result is a plain distance disc of `reach` around the stroke); 1 = the hardest stop.
    double edgeStop = 0.5;
};

// Grow `seeds` (a document-space coverage mask -- the brush stroke; pixels at or above
// kAntsCoverageThreshold seed the transform) outward over `src` (the same document-space RGBA
// source the magic wand floods) by an edge-weighted geodesic distance transform,
// and ramp-threshold the distance field into an anti-aliased `src`-sized Selection. An empty
// `src`, empty or dimension-mismatched `seeds`, a stroke with no full-coverage core, or a
// coverage-free result all yield an empty ("no selection") Selection -- never an active selection
// of nothing. Combining with the press-time op is the caller's job (`Selection::combine`), same
// as every other selection source.
[[nodiscard]] Selection edgeGrowSelection(const common::Image& src, const Selection& seeds,
                                          const EdgeGrowParams& params);

} // namespace mosaic::core
