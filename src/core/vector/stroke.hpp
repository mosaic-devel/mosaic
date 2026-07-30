#pragma once

#include "common/geometry.hpp"
#include "core/vector/geometry.hpp"
#include "core/vector/paint.hpp"

// Stroke-to-outline (S25). Converts flattened Contours into the set of FILLED closed pieces that,
// rasterized NonZero, form the stroke: a quad per segment, a cap piece per open end, and a join
// piece per interior vertex; dashes split the path by arc length first. Every emitted piece is
// normalized to a consistent winding so the NonZero fill UNIONS overlaps cleanly (no cancellation)
// -- which sidesteps the fragile single-offset-outline self-intersection problems on tight turns.
// Classic public-domain geometry.
namespace mosaic::core::vec {

// Build the stroke outline for `contours` under `stroke`, in the SAME (layer-local) space as the
// input. `tolerancePx`/`toDevice` tune the tessellation of round caps/joins to device resolution.
// Caps (Butt/Round/Square), joins (Miter+limit / Round / Bevel) and dashes are honored. This builds
// a CENTRED outline at `stroke.width`; Inside/Outside ALIGNMENT is applied by the caller (raster.cpp)
// as a coverage-clip of a double-width centred stroke against the fill -- so this stays alignment-
// agnostic. Width is in local units, so it scales with the layer transform when mapped to pixels.
[[nodiscard]] Contours strokeOutline(const Contours& contours, const Stroke& stroke,
                                     double tolerancePx = 0.25,
                                     const common::Affine2D& toDevice = common::Affine2D::identity());

}  // namespace mosaic::core::vec
