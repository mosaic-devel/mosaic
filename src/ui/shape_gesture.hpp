#pragma once

#include <optional>
#include <vector>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/vector/object.hpp"
#include "ui/tool.hpp"

// Shape-tool authoring math (S26) -- the pure, FLTK-free core that turns a press->current drag (plus
// the Shift/Alt modifiers and the active option snapshot) into the vec::Object a new VectorLayer
// will carry, together with the layer-local -> document placement transform. Kept free of FLTK and
// the canvas so it can be unit-tested headlessly (per the verification division); the VulkanCanvas
// gesture and app_window wiring consume it.
//
// Model note (docs/vector-model.md §1): the authored shape keeps its real size in its PARAMETRIC
// parameters (RectShape::size, EllipseShape::radii, ...), centred on the local origin, and the
// placement transform is a RIGID translation to the drag-box centre. So the Move/Resize gizmo moves
// and rotates the layer without distorting the shape or its stroke, and a later parametric resize
// (S26-b) edits the params in place.
//
// OUTLINES ARE NOT AUTHORED HERE (S26-c). The tool used to write a core::vec stroke straight onto
// the object from a Fill/Outline/Both paint picker; it no longer does. A shape is a FILL, and its
// outline is a Layer Effects STROKE (core::StrokeEffect -- stackable, concentric, Inside/Center/
// Outside), added from the options bar's "Outline…" button. core::vec::Object still CARRIES and
// renders a stroke, so every document and .mosaic file authored before this loads and looks exactly
// as it did; only the tool stopped writing one. The single exception is the LINE -- see below.
namespace mosaic::ui {

// Which parametric primitive the active shape tool authors. The first five are the S26-a set that
// owns a toolbar slot; the rest are the S26-c widening (speech bubble / arrow / ring / cross /
// heart / chevron-banner) -- authored the same way, reached through the shape-designer's kind
// gallery until they are given toolbar variants of their own (see shapeKindCatalog below).
enum class ShapeKind { Rect, Ellipse, Polygon, Star, Line, Callout, Arrow, Ring, Cross, Heart,
                       Banner };

// A flat snapshot of the active shape tool's options (read off the options bar by the canvas), so
// the math takes plain values and never touches the ToolManager.
//
// THE LINE EXCEPTION: a line has no interior, so a fill cannot express it -- a stroke is the only
// way it can exist at all. The Line kind therefore keeps a minimal tool-authored stroke (`lineWidth`
// + `cap`, painted in the foreground colour), and nothing else does. An outline AROUND that line is
// still a Stroke layer effect, exactly as for the closed kinds.
struct ShapeOptions {
    common::ColorF foreground{0.0f, 0.0f, 0.0f, 1.0f};          // the fill (a line's stroke) colour
    double lineWidth = 3.0;                                     // LINE ONLY: centreline weight
    core::vec::LineCap cap = core::vec::LineCap::Butt;          // LINE ONLY: end caps
    double cornerRadius = 0.0;                                  // rect (+ the rounded library kinds)
    int sides = 5;                                              // polygon
    int points = 5;                                             // star
    double innerRatio = 0.5;                                    // star inner/outer in (0,1)
    // Snap the drag's extent to whole document pixels so axis-aligned edges land on pixel
    // boundaries (crisp rectangle outlines / fills, no AA fringe). Default on; a tool toggle can
    // expose it later. Snapping the size+placement keeps the shape parametric (we round the box
    // corners, not the geometry), so a later parametric resize still snaps.
    bool snapToPixel = true;
};

// The result of a drag: the object to put on a fresh VectorLayer + where to place it. Built every
// drag frame to draw the WIREFRAME preview (shapeOutlinePolyline below), and once more on release --
// that last one is the object the new layer actually carries.
struct ShapeDraft {
    core::vec::Object object;        // geometry centred on the origin; filled (a line: stroked)
    common::Affine2D placement;      // layer-local -> document (a translation to the box centre)
};

// Map a shape tool id to its ShapeKind (nullopt for non-shape tools). Only the five kinds that own
// a toolbar variant map; the S26-c library kinds are picked in the shape designer's kind gallery,
// and gain a case here the day they are given ToolIds of their own.
[[nodiscard]] std::optional<ShapeKind> shapeKindFor(ToolId id);

// ---- The shape-kind catalogue (S26-c) ---------------------------------------------------------
// One list every surface that has to NAME or PICTURE a kind reads: the shape designer's kind
// gallery today, a toolbar flyout / icon-pack key tomorrow. Kept here, beside the kinds themselves,
// so a new primitive is declared in exactly one place. `name` is N_()-marked (extraction only) --
// the presenting surface calls _() on it.
struct ShapeKindInfo {
    ShapeKind kind;
    const char* iconKey;  // icon-pack file key: "shape_callout" -> assets/default_tools/<key>.svg
    const char* name;     // untranslated display name (wrap in _() at the point of display)
};

// Every ShapeKind, in the order a picker should present them (the five toolbar kinds first).
[[nodiscard]] const std::vector<ShapeKindInfo>& shapeKindCatalog();

// The catalogue entry for `kind` (never null: the catalogue covers the whole enum).
[[nodiscard]] const ShapeKindInfo& shapeKindInfo(ShapeKind kind);

// Convert `base` to `kind`, keeping its overall SIZE (its contentBounds box) and its paint, so the
// designer's kind gallery can re-shape a selected object in place without it jumping or resizing.
// Parameters that have no counterpart in the new kind take that kind's defaults. Returns `base`
// unchanged for a Path / an object already of that kind.
[[nodiscard]] core::vec::Object convertedShape(const core::vec::Object& base, ShapeKind kind);

// Build the shape a `pressDoc`->`currentDoc` drag describes (document-space points). `shift`
// constrains (square / circle / equal-radius, or a 45 deg-snapped line); `alt` anchors the drag at
// its CENTRE (press point) instead of a corner. Returns nullopt for a degenerate (sub-pixel) drag.
[[nodiscard]] std::optional<ShapeDraft> buildShapeDraft(ShapeKind kind, common::Vec2 pressDoc,
                                                        common::Vec2 currentDoc, bool shift, bool alt,
                                                        const ShapeOptions& opts);

// ---- Wireframe drag preview (S26-c): a shape is an OUTLINE until release -----------------------

// `draft`'s silhouette as a polyline in DOCUMENT space -- what the canvas draws while the drag is in
// flight. Nothing is added to the document until the pointer is released; the very same draft is
// then spawned as the real, FILLED vector layer, so what the wireframe traced is what lands.
//
// `docToDevice` maps document px to physical screen px and is used ONLY to pick the flattening
// tolerance, so curve smoothness tracks the zoom exactly like the rasteriser's does. A closed
// contour repeats its first point, so the caller can draw the result as one open polyline.
//
// ONE polyline, because the overlay lane it rides carries exactly one: a shape that flattens to
// several contours previews its LARGEST (by bounding-box area) -- its silhouette -- rather than
// stitching the contours together with segments that are not part of the shape.
[[nodiscard]] std::vector<common::Vec2> shapeOutlinePolyline(
    const ShapeDraft& draft,
    const common::Affine2D& docToDevice = common::Affine2D::identity());

// ---- Select-to-edit bridge (S26-b §7.1): an existing shape <-> the options bar ----------------

// Which parametric ShapeKind an object's geometry is (nullopt for a Path, a non-parametric/empty
// object, or a primitive this build has no ShapeKind for) -- used to switch the Shape tool to the
// kind of a clicked shape.
[[nodiscard]] std::optional<ShapeKind> shapeKindOf(const core::vec::Object& obj);

// Read `obj`'s options-bar-editable parameters into `io`: the per-shape hot parameter (corner radius
// / sides / points / inner %) and, for a LINE, its weight + cap. Nothing paint-related is read for a
// closed shape -- the bar no longer carries any. Colours are NOT read either: they stay on `io` (the
// colour swatch owns them), so reflecting a shape leaves the active colours alone.
void readShapeOptions(const core::vec::Object& obj, ShapeOptions& io);

// Apply the options-bar-editable parameters of `opts` onto a COPY of `base`, returning the edited
// object. The geometry's SIZE/placement and the object's paint (fill, and any stroke a pre-S26-c
// document carries) are preserved; only the hot parameter changes -- plus, for a LINE, its weight
// and cap, the one stroke the tool still owns.
[[nodiscard]] core::vec::Object editedObject(const core::vec::Object& base, const ShapeOptions& opts);

// Recolour `base`'s active paints from the colour swatch (a foreground/background change while a
// shape is selected): the fill (and a line's / a lone outline) take `fg`; when an object has BOTH a
// fill and a stroke -- which only a pre-S26-c document does now -- the outline takes `bg`. Geometry
// and stroke width are untouched; only the colours change.
[[nodiscard]] core::vec::Object recoloredObject(const core::vec::Object& base, common::ColorF fg,
                                                common::ColorF bg);

// ---- Resize-vs-transform: parametric resize by a bbox handle (S26-b §7.1) ----------------------

// The result of dragging a selection-box handle in RESIZE mode: the shape's SIZE parameters scaled
// (a line's stroke width is left uniform -- the Figma/Affinity default), together with the new layer
// placement that keeps the opposite handle pinned in document space (the parametric shapes are
// centred on the local origin, so a one-sided resize must shift the layer transform to re-anchor).
struct ShapeResize {
    core::vec::Object object;     // params scaled; paint + stroke width untouched
    common::Affine2D placement;   // new layer transform (rotation kept; anchor handle fixed)
};

// Resize `base` (placed by `placement`, its world transform) by dragging bbox handle `handle`
// (0-3 = corners TL,TR,BR,BL; 4-7 = edge mids T,R,B,L -- the transformHandleCenters order) to the
// document-space point `docPt`. The handles frame the object's contentBounds; the drag scales the
// shape's size PARAMETER(s) by the box ratio along each active axis (Rect/Ellipse/Line anisotropic;
// Polygon/Star uniform -- a single radius). `keepAspect` (Shift) locks the aspect; `fromCenter`
// (Alt) scales about the box centre instead of the opposite handle. nullopt for a degenerate box /
// a non-parametric object.
[[nodiscard]] std::optional<ShapeResize> resizeShape(const core::vec::Object& base,
                                                     const common::Affine2D& placement, int handle,
                                                     common::Vec2 docPt, bool keepAspect,
                                                     bool fromCenter);

}  // namespace mosaic::ui
