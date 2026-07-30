#pragma once

#include "ui/popover.hpp" // the child-sub-window host (built before the parent is shown)

#include "core/vector/object.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Fl_Widget;

namespace mosaic::ui {

// ---- the on-diagram handles (pure; unit-tested in tests/test_shape_library.cpp) ---------------
//
// The designer's diagram carries per-kind drag handles, and BOTH halves of that gesture are pure
// functions of the object so they can be pinned headlessly -- "the handle is where the outline is"
// is exactly the sort of claim a screenshot cannot prove and a test can.
//
// Every handle on a corner-rounded kind is derived from the shared corner engine
// (core/vector/corner.hpp), not from the raw parameter box: at a rounded corner the outline never
// reaches the sharp vertex, so a handle placed there floats off the shape. See the .cpp for the
// per-kind id table.
[[nodiscard]] std::vector<std::pair<int, common::Vec2>>
shapeHandlePoints(const core::vec::Object& obj);

// `obj` with the parameter behind handle `handle` moved to `local` (object-local coords).
// `linkCorners` ties a rect's four corner radii together. nullopt when the kind/handle pair means
// nothing, so a caller can skip the edit entirely. Exactly inverse to shapeHandlePoints() for the
// corner-radius handles: dragging to a point puts the handle back at that point.
[[nodiscard]] std::optional<core::vec::Object>
shapeAfterHandleDrag(const core::vec::Object& obj, int handle, common::Vec2 local,
                     bool linkCorners);

class IconPacks;
class ScrubRuler;

// The shape-designer popover (S26-b §7.4, reworked S26-c): the "everything" surface for the selected
// vector shape. Three fixed bands, so the surface reads the same whatever kind is loaded:
//
//   1. a large live DIAGRAM of the working object over a transparency checkerboard, carrying the
//      shape's on-diagram handles (a rect corner, the ellipse's arc ends, a speech bubble's tail --
//      the §7.4 dragging that the first cut deferred), drawn with the software-AA GizmoCanvas;
//   2. the KIND GALLERY -- one icon per ShapeKind; picking one re-shapes the selected object in
//      place (ui::convertedShape), which is how the S26-c library kinds (speech bubble, arrow,
//      ring, cross, heart, chevron) are reached while the toolbar still holds only five variants;
//   3. the CONTROLS, grouped under section headers on one rhythm and scrolled when they overflow,
//      with the shared outline/dash editor tucked behind a disclosure.
//
// A genuine child sub-window like the colour flyout, built before the host window is shown. It edits
// a working COPY of the shape's object and reports every change through setOnEdit; the host pushes
// the coalesced SetVectorObjectCommand (the same live, undoable path as select-to-edit). Opened from
// the "Edit shape…" button in the Shape options bar.
class ShapeDesigner : public Popover {
public:
    ShapeDesigner();
    ~ShapeDesigner() override;

    // Fired live as a control changes, with the edited object (geometry params only; colours and the
    // placement are untouched). The host lands it as a coalesced SetVectorObjectCommand.
    void setOnEdit(std::function<void(core::vec::Object)> cb) { m_onEdit = std::move(cb); }

    // The shared precision-ruler HUD every options-bar slider uses (owned by the main window).
    // Non-owning; unset simply means the precision drag works without the floating ruler.
    void setScrubRuler(ScrubRuler* r);

    // The icon pack the kind gallery draws from (non-owning; the host's, so a user pack applies).
    // Unset falls back to a private default-pack instance, so the gallery always has art.
    void setIconPacks(IconPacks* packs, std::string packId);

    // Populate from `obj` (the shape currently selected for editing) and show anchored to `anchor`.
    // A null `obj` shows a "select a shape" hint instead of controls.
    void openFor(const Fl_Widget* anchor, const core::vec::Object* obj);

    void reapplyTheme() override; // rebuild the controls so they bake the new palette

    // A control changed: update the working object and report. `role` is the file-local Role enum
    // (passed as int so the enum can stay in the .cpp); `idx` is its slot (e.g. a rect corner 0-3).
    // Public only so the C callback thunk can reach it; not part of the popover's API.
    void applyControl(Fl_Widget* w, int role, int idx);

    // The kind gallery picked `kind` (the file-local ShapeKind, passed as int for the same reason):
    // convert the working object and rebuild. Public only for the gallery cell's callback.
    void applyKind(int kind);

    // Flip the collapsible "Outline" section. Public only for the disclosure button's thunk.
    void toggleOutline();

private:
    // A drag on an on-diagram handle: `handle` id (per-kind -- see the .cpp's handlePoints) + the
    // point in object-local coords. Set by the preview's drag callback.
    void applyDiagramDrag(int handle, common::Vec2 local);
    void rebuild();         // (re)build all three bands (open / theme change / a new shape)
    // Refill ONLY the scrolled controls band. The kind gallery calls this rather than rebuild():
    // rebuilding the whole popover from a gallery cell's own event handler would delete that cell
    // while FLTK is still inside its handle() (Fl_Group::clear deletes immediately).
    void rebuildControls();
    void sizeToContent();   // popover height <- the controls' height, then re-pin to the anchor
    void syncControls();    // re-read every numeric control's value from m_obj (after a drag)
    void emitEdit();        // m_obj -> onEdit + refresh the live diagram

    core::vec::Object m_obj; // the working copy the controls mutate
    bool m_hasObj = false;
    std::function<void(core::vec::Object)> m_onEdit;
    ScrubRuler* m_scrubRuler = nullptr; // shared precision HUD (non-owning; null = no HUD)

    struct State; // per-control bindings + the diagram + the icon cache (defined in the .cpp)
    std::unique_ptr<State> m_state;
};

} // namespace mosaic::ui
