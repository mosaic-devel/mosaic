#pragma once

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/vector/object.hpp"

#include <optional>
#include <vector>

// Gradient-tool authoring math (S22) -- the pure, FLTK-free core that turns a press->current drag
// (document space, plus the Shift modifier) into the vec::Object a new gradient VectorLayer
// carries, and that reconstructs / re-edits the on-canvas handles of an existing gradient layer.
// Kept free of FLTK and the canvas so it can be unit-tested headlessly (like shape_gesture); the
// VulkanCanvas gesture and app_window wiring consume it.
//
// Model note (docs/vector-model.md §1, docs/gradient-tool.md): a "gradient layer" is a VectorObject
// whose geometry is a FULL-BLEED RectShape (the document size, centred on the local origin) and
// whose fill is a vec::Gradient. The layer transform places the rect (a rigid translation to the
// document centre), so object-local space is doc space shifted by -centre. The gradient's own
// `transform` (gradient unit-space -> object-local) carries the geometry the user drags:
//   * Linear     -- unit (0,0)->start, (1,0)->end.
//   * Radial     -- unit origin = centre, the unit circle = the radius r (an ISOTROPIC scale).
//   * Elliptical -- a Radial whose scale is ANISOTROPIC (rx != ry): the unit circle maps to an
//                   ellipse. The vector model has no separate "elliptical" type; it is a Radial
//                   with a non-uniform transform, distinguished on re-edit by comparing the axis
//                   lengths.
//   * Conic      -- unit origin = centre, angle 0 (the first stop) points down the +x axis.
namespace mosaic::ui {

// The four gradient shapes the tool authors. Elliptical collapses to vec::GradientType::Radial in
// the object (see the model note); the other three map one-to-one.
enum class GradientShape { Linear, Radial, Elliptical, Conic };

// The minor/major ratio (ry/rx) a fresh Elliptical drag authors, and the one a retype TO Elliptical
// falls back to: an ellipse whose ry equals its rx is a circle, and would read straight back as a
// circular Radial (gradientShapeOf compares the two axis lengths), so the squash has to be real.
inline constexpr double kGradientDefaultAspect = 0.5;

// The tool's "type" Choice index (0 Linear / 1 Radial / 2 Elliptical / 3 Conic) -> a GradientShape.
[[nodiscard]] GradientShape gradientShapeFromChoice(int choice);
// The inverse: a GradientShape -> its "type" Choice index (so re-selecting a layer syncs the bar).
[[nodiscard]] int gradientChoiceForShape(GradientShape shape);

// The bar's "Dithering" Choice index (0 None / 1 Ordered / 2 Blue noise / 3 Noise) <-> the model's
// core::vec::DitherKind, which is where the setting LIVES (it rides the gradient paint, so it
// persists, serialises and re-renders identically -- exactly like SpreadMethod). Same two-function
// bridge the Type choice uses, so the bar can be driven from a bound layer and vice versa.
[[nodiscard]] core::vec::DitherKind gradientDitherFromChoice(int choice);
[[nodiscard]] int gradientDitherChoice(core::vec::DitherKind kind);

// Which tool's select-to-edit binds a vector object (S22). A GRADIENT object -- one whose fill is a
// vec::Gradient -- belongs to the Gradient tool: the Shape bar carries no gradient control at all,
// and its swatch recolour (recoloredObject) would REPLACE the gradient with a flat fill, so binding
// one there is destructive rather than merely useless. The mirror holds too: a plain shape has no
// gradient for the Gradient bar's Type / Stops / Dithering to drive, so the Gradient tool leaves it
// alone. Pure, so the exclusion is unit-tested in both directions.
[[nodiscard]] bool gradientToolBinds(const core::vec::Object& obj);
[[nodiscard]] bool shapeToolBinds(const core::vec::Object& obj);

// The result of a drag: the object for a fresh VectorLayer + where to place it.
struct GradientDraft {
    core::vec::Object object;   // full-bleed rect + gradient fill, geometry centred at local origin
    common::Affine2D placement; // layer-local -> document (a translation to the document centre)
};

// Build the gradient a `pressLocal`->`currentLocal` drag describes (LAYER-LOCAL points), reusing
// the caller's working `stops` + `spread` + `dither`. `aspect` = ry/rx for Elliptical (ignored
// otherwise). `shift` snaps the axis angle to 45 deg steps. The axis length (and the minor radius)
// are clamped to a tiny positive floor so the transform stays invertible.
[[nodiscard]] core::vec::Gradient
buildGradient(GradientShape shape, common::Vec2 pressLocal, common::Vec2 currentLocal,
              double aspect, const std::vector<core::vec::GradientStop>& stops,
              core::vec::SpreadMethod spread, bool shift,
              core::vec::DitherKind dither = core::vec::DitherKind::None);

// Author a NEW full-bleed gradient layer object from a document-space drag over a `docW` x `docH`
// document. Returns nullopt for a degenerate (sub-pixel) drag. `stops`/`spread`/`dither` are the
// tool's working ramp; `shift` snaps the angle.
[[nodiscard]] std::optional<GradientDraft>
buildGradientDraft(GradientShape shape, common::Vec2 pressDoc, common::Vec2 currentDoc, double docW,
                   double docH, const std::vector<core::vec::GradientStop>& stops,
                   core::vec::SpreadMethod spread, bool shift,
                   core::vec::DitherKind dither = core::vec::DitherKind::None);

// The GradientShape of an object's fill (nullopt when the fill is not a gradient) -- used to
// re-open the Gradient tool on a clicked gradient layer and to sync the "type" choice.
[[nodiscard]] std::optional<GradientShape> gradientShapeOf(const core::vec::Object& obj);

// The on-canvas handle anchor points (DOCUMENT space) for a gradient `obj` placed by `layerXform`
// (its world transform). `valid` is false when the object has no gradient fill.
struct GradientHandles {
    GradientShape shape = GradientShape::Linear;
    common::Vec2 start; // linear start  / radial-elliptical-conic CENTRE
    common::Vec2 end;   // linear end    / primary-axis EDGE (radius + rotation)
    // The image of gradient-unit (0,1): the MINOR-axis edge of the radial family. Always filled in
    // (it is what makes the outline the exact affine image of the unit circle, whatever the layer
    // transform does), but only DRAGGABLE -- and only drawn as a handle -- when `hasMinor`.
    common::Vec2 minor;
    bool hasMinor = false; // a fourth minor-axis handle shows (Elliptical only; see retypeGradient)
    bool valid = false;
};
[[nodiscard]] GradientHandles gradientHandles(const core::vec::Object& obj,
                                              const common::Affine2D& layerXform);

// Which handle a document-space point hits, within `pickDoc` document units: 0 = start/centre,
// 1 = end/edge, 2 = elliptical minor edge, 3 = body -- the round midpoint handle the gizmo draws
// for EVERY shape, plus (Linear only) anywhere along the axis line, both of which move the whole
// gradient rigidly. -1 = none. End/minor win over start, and start over the midpoint, so a
// coincident pair (a tiny gradient) stays grabbable.
[[nodiscard]] int hitGradientHandle(const GradientHandles& h, common::Vec2 docPt, double pickDoc);

// Does this handle set draw a shape-outline ring at all? The radial family and Conic do; Linear
// does not (its axis line already shows its whole extent), nor does an invalid set.
[[nodiscard]] bool gradientHasRing(const GradientHandles& h);

// The gizmo's SHAPE OUTLINE -- the circle a Radial (or a Conic's sweep) covers, the ellipse an
// Elliptical does -- as a DISTANCE FIELD rather than a polyline. `centre` + the basis vectors
// `ux`/`uy` are the affine image of the gradient's unit circle (the handle anchors: ux = end -
// start, uy = minor - start), in whatever space the caller works in; the return is the signed
// distance from `p` to that ellipse, negative inside, in the SAME units.
//
// This is what replaced the chorded ring (S22 fix): a polyline needed ~pi*sqrt(2R) chords to keep
// its sag under a quarter pixel, so at any real zoom it either read as a visible polygon or ate the
// whole 64-entry overlay-line lane the DOCUMENT's guides share. As a distance field the ring costs
// two lane slots, is exactly smooth at every zoom, and starves nothing.
//
// The estimate is the first-order (Taubin) distance F/|grad F| of the implicit form
// |M^-1 (p - centre)|^2 = 1: zero exactly ON the ellipse and correct to first order beside it,
// which is all a 1 px hairline needs. Its residual error is O(d^2/rho) -- second order in the
// distance, and only material for a wildly eccentric ellipse right at a major-axis tip.
// shaders/canvas_present.comp's gradientRing() is this same expression, per screen pixel.
// Returns +infinity ("no ring here") for a degenerate basis and at the exact centre, where grad F
// vanishes -- the shader's two guards, which return the untouched pixel, are the same two cases.
[[nodiscard]] double gradientRingDistance(common::Vec2 centre, common::Vec2 ux, common::Vec2 uy,
                                          common::Vec2 p);

// Retype an existing gradient object's fill to `shape`, keeping its centre, its primary axis
// (length AND angle), its stops, its spread and its dither kind -- what the bar's "Type" choice
// does to the gradient layer bound for editing. Linear and the radial family reinterpret the SAME
// two points (start/centre, end/edge), so switching kind re-reads the geometry you already dragged
// instead of discarding it. A retype TO Elliptical from a circular transform adopts
// kGradientDefaultAspect; away from it restores an isotropic scale. Returns `base` unchanged when
// its fill is not a gradient.
[[nodiscard]] core::vec::Object retypeGradient(const core::vec::Object& base, GradientShape shape);

// Re-edit `base`'s gradient after dragging handle `handle` from `pressDoc` to `currentDoc`
// (document space); `layerXform` is the layer's world transform. Point handles move rigidly by the
// drag delta (no jump-to-cursor); the body handle translates the whole gradient. Stops, spread and
// dither are preserved. Returns `base` unchanged when it has no gradient fill.
[[nodiscard]] core::vec::Object dragGradientHandle(const core::vec::Object& base,
                                                   const common::Affine2D& layerXform, int handle,
                                                   common::Vec2 pressDoc, common::Vec2 currentDoc,
                                                   bool shift);

} // namespace mosaic::ui
