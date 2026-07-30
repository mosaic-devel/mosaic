#pragma once

#include "common/geometry.hpp"
#include "core/vector/geometry.hpp"
#include "core/vector/object.hpp"

// Vector hit-testing (S25). Point classification and outline distance over the flattened
// Contours -- the picking primitives the Move tool and the Shape/Pen tools (S26-S28) use to
// select a vector object and grab its outline. Classic point-in-polygon math (winding number /
// even-odd parity, Sunday); nothing here touches the GPU.
namespace mosaic::core::vec {

// Is layer-local point p inside the filled region of `cs` under `rule`? Open contours are treated
// as implicitly closed for fill (matching SVG).
[[nodiscard]] bool contains(const Contours& cs, common::Vec2 p, FillRule rule);

// Shortest distance from p to the DRAWN outline of `cs` (closed contours include the closing edge;
// open ones do not). Infinity when `cs` has no segments.
[[nodiscard]] double distanceToOutline(const Contours& cs, common::Vec2 p);

// The effective fill rule for a geometry (a Path carries its own; primitives are NonZero).
[[nodiscard]] FillRule fillRuleOf(const Geometry& g);

// Pick test for one object at a layer-local point: true if p is inside the fill (when the object
// has a fill) or within the pick band of the outline. `pickRadiusLocal` and `flattenTol` are in
// LAYER-LOCAL units -- a tool divides its device-space pick radius by the zoom before calling.
// An enabled stroke widens the band by half its width.
[[nodiscard]] bool hitTest(const Object& obj, common::Vec2 pLocal, double pickRadiusLocal = 0.0,
                           double flattenTol = 0.25);

}  // namespace mosaic::core::vec
