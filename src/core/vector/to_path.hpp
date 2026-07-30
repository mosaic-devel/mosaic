#pragma once

#include "core/vector/geometry.hpp"

// "Convert to Path": a parametric shape promoted to the editable node/handle model, in exact cubic
// Beziers (docs/vector-model.md §7.6; PLAN S26's "editing a node converts to path").
//
// This is deliberately NOT flatten(). flatten() answers "what polyline draws this at `tolerancePx`
// on THIS device", and its output is a rendering artefact: an ellipse becomes a hundred-odd sampled
// points. A converted path is the *model* the Pen tool (S28) will open, so an ellipse must come back
// as four cubic quarter-arcs with smooth handles, not as a hundred corner nodes.
//
// Fidelity. Every curve here is either exact or the standard circular-arc approximation:
//   * A quadratic (the line tool's bend) elevates to a cubic EXACTLY.
//   * A circular / elliptical arc is emitted at <= 90 degrees per cubic with control offsets
//     k = 4/3 * tan(dt/4) along the parametric tangent -- error < 3e-4 of the radius per segment.
//     An ellipse is the affine image of a circle and that approximation commutes with an affine
//     map, so the same formula is right for both.
// The corner geometry (tangent points, the inset clamp, which arc is the minor one) mirrors
// flatten.cpp's emitCorner exactly, and the test suite pins that: flattening a converted path must
// trace the same outline as flattening the shape it came from.
namespace mosaic::core::vec {

// The shape as an editable path. Closedness, winding and corner treatment all match what flatten()
// would have drawn. A degenerate shape (zero size / radius) yields a Path with no subpaths.
[[nodiscard]] Path pathFromShape(const ParametricShape& shape);

// The geometry as an editable path: a Path passes through untouched, a ParametricShape converts.
[[nodiscard]] Path pathFromGeometry(const Geometry& geometry);

// `path` with every anchor and both handles mapped through `t`. Cubic Beziers are affine-invariant
// -- the image of a cubic under an affine map is the cubic through the mapped control points -- so
// this is EXACT, not a re-approximation. Rebasing one layer's geometry into another's local space
// is what lets Layer->Merge Down combine two shape layers into one path (S36).
[[nodiscard]] Path transformedPath(const Path& path, const common::Affine2D& t);

} // namespace mosaic::core::vec
