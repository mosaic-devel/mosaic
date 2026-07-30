#pragma once

#include "core/vector/geometry.hpp"
#include "core/vector/paint.hpp"

// One vector object = geometry + fill + stroke. A VectorLayer holds exactly one of these
// (docs/vector-model.md §1: one object per layer; composition is the layer stack + groups).
// "Shape layer", "path layer", "gradient layer" are all just an Object with different
// geometry/paint -- never separate layer kinds.
namespace mosaic::core::vec {

struct Object {
    Geometry geometry;
    Paint fill = NoPaint{};
    Stroke stroke;

    // SVG paint-order: whether the stroke sits over the fill (default) or under it.
    enum class PaintOrder { FillThenStroke, StrokeThenFill } paintOrder = PaintOrder::FillThenStroke;

    bool operator==(const Object&) const = default;
};

}  // namespace mosaic::core::vec
