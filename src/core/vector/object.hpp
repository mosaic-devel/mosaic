#pragma once

#include "core/text/extrude.hpp" // text::Extrude -- the shared 3D solid model (see Object::extrude)
#include "core/vector/geometry.hpp"
#include "core/vector/paint.hpp"

#include <optional>

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

    // 3D extrusion (docs/vector-model.md §11): nullopt is the flat 2D default, so every existing
    // object is untouched. Set, the object renders as an extruded SOLID instead of a flat fill --
    // the same watertight-solid model, bevels, materials, lighting, orientation and camera the
    // Type tool's 3D gives a text block, through the same mesher and the same render lanes.
    //
    // ⚠ Why a `text::` type in a vector header. The extrusion model was authored for the Type tool
    // and lives under core/text, but nothing in it is typographic: it consumes vec::Contours and
    // emits a mesh (core/text/extrude_mesh.hpp takes `vec::Contours` as ITS input, which is the
    // same seam read from the other side). A shape is the simpler caller -- a text block is just
    // "a lot of contours with runs" -- so sharing it verbatim is what keeps 3D shapes and 3D text
    // one feature instead of two implementations that drift. Moving the type to a neutral home
    // would be a rename across both lanes and buys nothing today.
    //
    // The fill region extrudes as run 0 and the stroke outline, when the object is stroked, as
    // run 1 -- so Extrude::runMaterials can give an outline its own material, which is exactly
    // what per-run materials do for text.
    std::optional<text::Extrude> extrude;

    bool operator==(const Object&) const = default;
};

}  // namespace mosaic::core::vec
