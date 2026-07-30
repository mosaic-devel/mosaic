#pragma once

#include "common/image.hpp"
#include "core/layer.hpp"     // core::LayerId, core::Layer
#include "core/selection.hpp" // core::SelectOp (thumbnail boolean ops, S14-b)
#include "ui/icons.hpp"       // IconButton, Icon (the panel-chrome icon set, S16-g)
#include "ui/theme.hpp"       // Palette (layerRowBackground's ramp)
#include "ui/widgets.hpp"     // Panel, FlatButton, TextInput

#include <FL/Fl_Widget.H>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Fl_RGB_Image;
class Fl_Scroll;

namespace mosaic::core {
class Document;
}

// The right-dock **Layers** panel (PLAN S10): a tabbed header, a per-layer properties strip
// (blend mode + opacity) for the active layer, a scrollable tree of the document's layers (top of
// the stack at the top; groups nest with indentation + a collapse triangle), and an add/delete
// toolbar. Every model edit goes through the document's command stack (so it is undoable +
// scriptable); after each, an onChange callback asks the host to re-composite the canvas. S10-a
// = dock + list + visibility/active + add/delete; S10-b = reorder/clone drag; S10-c = groups,
// per-layer opacity/blend, reparent-drag, and the (stubbed-until-S13) shift-click-to-select;
// S10-d = the drag ghost (a translucent chip of the dragged layer rides the cursor while its
// source row shows a muted, dashed "lifted" slot). S16-b grows the header into a real tab
// strip: the panel doubles as the right DOCK, hosting the History tab (ui::HistoryPanel) in
// the body below the shared header.
namespace mosaic::ui {

class ChannelsPanel;
class HistoryPanel;
class LayerPanel;

// Which tab the right dock shows (S16-b; Channels added later).
enum class DockTab : std::uint8_t { Layers, History, Channels };

// One row in the layer list: a visibility "eye" toggle, a lock cell, an optional group disclosure
// triangle, a thumbnail, and the layer name -- indented by tree depth, with an active-layer
// highlight. Rows are lightweight + rebuilt by the panel; live state (visibility, lock, active,
// group expansion) is read back from the document/panel each draw so a toggle needs only a redraw.
//
// Both the eye and the LOCK always paint (an open padlock, muted, when unlocked). The first cut
// revealed the lock only on hover or when locked, Photoshop-style; the user rejected it -- the
// empty cell reads as a hole in the row rather than as restraint (2026-07-09).
class LayerRow : public Fl_Widget {
public:
    LayerRow(int X, int Y, int W, int H, LayerPanel* panel, core::LayerId id);

    // A small per-layer-TYPE badge riding after the name (like the paste marker), so vector/text/etc.
    // layers read at a glance. The badge is a composable glyph: a TYPE mark plus, where a type has
    // sub-modes, a MODE mark -- a serif "T" with a filled dot for Point text ("a point") and with a
    // marquee rectangle for Area text ("a frame").
    // Gradient is the same split one level down: a GRADIENT layer is a VectorLayer too (a full-bleed
    // rect whose fill is a vec::Gradient, docs/vector-model.md §1), so it earns its own mark rather
    // than the generic shapes one -- otherwise the two are indistinguishable in the dock (S22).
    // VectorPath is that split a third time and for the same reason: a pen PATH is a VectorLayer as
    // well (vec::Geometry's Path alternative), so it wore the generic shapes mark and read exactly
    // like a rectangle or a star. typeBadgeFor() owns which one a layer earns.
    enum class TypeBadge {
        None,
        VectorShape,
        VectorPath,
        Gradient,
        TextPoint,
        TextArea,
        Magic,
        Texture,
        Adjustment
    };
    void setName(std::string name) { m_name = std::move(name); }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    void setThumbnail(common::Image thumb) { m_thumb = std::move(thumb); }
    void setDepth(int depth) { m_depth = depth; }
    void setPastedMarker(bool marked) { m_pastedMarker = marked; }
    void setTypeBadge(TypeBadge b) { m_typeBadge = b; }
    void setHasEffects(bool e) { m_hasEffects = e; } // draws the clickable fx badge (LE-b)
    // Mask affordances (S31): a second thumbnail right of the pixel one, the link chain in the
    // gap between them, and a disabled X. Presence + pixels are pushed on rebuild like the pixel
    // thumbnail; the live enabled/linked flags are read back from the document each draw (the
    // eye/lock pattern), so a flag command needs only a redraw.
    void setMaskState(bool hasMask) { m_hasMask = hasMask; }
    void setMaskThumbnail(common::Image thumb) { m_maskThumb = std::move(thumb); }
    [[nodiscard]] int depth() const noexcept { return m_depth; }
    [[nodiscard]] core::LayerId layerId() const noexcept { return m_id; }
    // Re-evaluate the hover affordance (ants + op glyph) after a modifier keydown/keyup, which
    // goes to the focus widget, not the hovered row; no-op unless the pointer is on this row.
    void modifiersChanged();

protected:
    void draw() override;
    int handle(int event) override;

private:
    [[nodiscard]] bool layerVisible() const;
    [[nodiscard]] bool layerLocked() const;
    [[nodiscard]] bool layerIsGroup() const;
    [[nodiscard]] bool layerExpanded() const;
    [[nodiscard]] bool layerHasPixels() const; // Raster/Magic: the Shift-click gesture works
    [[nodiscard]] bool layerMaskEnabled() const; // live RasterMask::enabled (true if no mask)
    [[nodiscard]] bool layerMaskLinked() const;  // live RasterMask::linked (true if no mask)
    [[nodiscard]] int eyeCellX() const;   // left of the visibility cell
    [[nodiscard]] int lockCellX() const;  // left of the lock cell (right of the eye)
    [[nodiscard]] int contentX() const; // left of the disclosure/thumbnail (after eye+lock+indent)
    [[nodiscard]] int maskThumbX() const; // left of the mask thumbnail (only when m_hasMask)
    [[nodiscard]] int nameX() const;    // left of the name text (right of the thumbnail(s))
    [[nodiscard]] int typeBadgeWidth() const; // px the type/paste badge needs (0 = none)
    void updateCursor(); // hand over the thumbnail while Shift is held (select-pixels affordance)
    // redraw(), except that the row being renamed damages the whole PANEL instead: the rename
    // editor floats over this row, and a lone update_child(row) would paint across it.
    void requestRedraw();

    LayerPanel* m_panel;
    core::LayerId m_id;
    std::string m_name;
    common::Image m_thumb; // opaque RGBA, pre-composited over a checkerboard
    int m_depth = 0;       // tree depth (0 = top level); drives indentation
    bool m_hover = false;
    bool m_handCursor = false; // the hand cursor is currently shown (avoid re-setting per move)
    bool m_eyeHover = false;   // pointer over the eye cell (inks it accent)
    bool m_lockHover = false;  // pointer over the lock cell (inks it accent)
    bool m_pastedMarker = false; // badge: unorganized pasted pixel data (clears on rename)
    TypeBadge m_typeBadge = TypeBadge::None; // badge: layer type (vector shape, ... )
    bool m_hasMask = false;        // the row shows a mask thumbnail (S31)
    common::Image m_maskThumb;     // grayscale coverage, aspect-fit (maskThumbnail)
    bool m_maskHover = false;      // pointer over the mask thumbnail (hand: click targets it)
    bool m_linkHover = false;      // pointer over the chain gap (hand: click toggles linkage)
    bool m_hasEffects = false;   // badge: the layer carries layer effects (LE-b)
    bool m_fxHover = false;      // pointer over the fx badge (hand cursor + brighter outline)
    int m_fxX = 0;               // the fx badge's hit rect (set in draw, read in handle)
    int m_fxW = 0;
    // The texture badge is the fx badge's sibling on a TextureLayer row: a framed, clickable chip
    // that (re)opens the Texture Generator modal (S55). Same hover/hit-rect machinery as fx.
    bool m_txHover = false;      // pointer over the texture badge (hand cursor + brighter outline)
    int m_txX = 0;               // the texture badge's hit rect (set in draw, read in handle)
    int m_txW = 0;
    // Right edge of the NAME text (set in draw, read in handle). A double-click renames only over
    // the name itself: the badges and the active-layer dot also sit right of nameX(), and starting
    // an inline rename from those -- with the name selected, so the next keystroke replaces it --
    // would be a silent, unasked-for edit.
    int m_nameRight = 0;
    core::SelectOp m_hoverOp = core::SelectOp::Replace; // chip on the ants frame (S14-b)
};

class LayerPanel : public Panel {
public:
    LayerPanel(int X, int Y, int W, int H);

    // Point the panel at the document to display/edit (non-owning; null clears). Resets the active
    // layer to the top of the stack and rebuilds the list.
    void setDocument(core::Document* doc);
    // Invoked after the panel mutates the document, so the host can re-composite + repaint.
    void setOnChange(std::function<void()> cb) { m_onChange = std::move(cb); }
    // Invoked when a layer's fx badge is clicked, to (re)open the Layer Effects modal for it (LE-b).
    void setOnOpenEffects(std::function<void(core::LayerId)> cb) { m_onOpenEffects = std::move(cb); }
    // Invoked when a texture layer's badge is clicked, to (re)open the Texture Generator modal (S55).
    void setOnOpenTexture(std::function<void(core::LayerId)> cb) { m_onOpenTexture = std::move(cb); }
    // Invoked by the row context menu's Merge Down. The command lives on the host (it needs the
    // renderer), and it already narrates each refusal through the status bar, so the menu item is
    // always offered rather than greyed on a guard this panel would have to duplicate.
    void setOnMergeDown(std::function<void()> cb) { m_onMergeDown = std::move(cb); }
    // The row context menu's Rasterize / Convert to Path. Like Merge Down, the commands live on the
    // host: rasterizing needs the compositor and converting text needs the shaper, neither of which
    // the panel may reach. The panel only decides WHICH items a layer's kind earns.
    void setOnRasterize(std::function<void(core::LayerId)> cb) { m_onRasterize = std::move(cb); }
    void setOnConvertToPath(std::function<void(core::LayerId)> cb) {
        m_onConvertToPath = std::move(cb);
    }
    // Invoked when the panel's OWN row grammar (see RowClick / selectRow below) changes the
    // multi-selection, so the host can mirror the set onto the canvas's move targets. The dock and
    // the canvas are one selection wearing two hats -- MainWindow::selectAllLayers already feeds
    // both -- and the panel may not reach VulkanCanvas itself. Deliberately NOT fired from
    // setMoveSelection(), which is the host pushing the CANVAS's set IN: echoing it straight back
    // would be a loop.
    void setOnSelectionChanged(std::function<void(const std::vector<core::LayerId>&)> cb) {
        m_onSelectionChanged = std::move(cb);
    }
    // How the panel narrates a refusal. The dock has no status bar of its own, and a gesture that
    // silently does nothing reads as a broken gesture -- the Shift-click-a-thumbnail path uses this
    // to say WHY a layer had nothing to select (the host binds it to transientStatus).
    void setOnStatus(std::function<void(std::string)> cb) { m_onStatus = std::move(cb); }

    // The dock's width splitter is a grab band on the dock's own LEFT EDGE (VulkanCanvas is an
    // Fl_Window, and no sibling widget may paint over a child sub-window). It belongs to ui::RightDock
    // now -- the panel is no longer the dock -- but the band still runs down THIS panel's left edge,
    // so the list must keep insetting past it or a row's eye cell would share pixels with the resize
    // band. Kept here as the inset the layout obeys; RightDock::splitterWidth() is its owner.
    [[nodiscard]] static constexpr int splitterWidth() { return 5; }
    // Make `id` active, then request the host open its Layer Effects modal (fx-badge click path).
    void openEffectsFor(core::LayerId id);
    // Make `id` active, then request the host open its Texture Generator modal (texture-badge click);
    // activating the texture layer first is what puts the generator dialog into EDIT mode (§3.3).
    void openTextureFor(core::LayerId id);
    // Rebuild the row list from the document (after an external edit / undo); re-validates active.
    void refresh();
    // Re-fetch row thumbnails in place (no row rebuild) -- light enough for live updates while typing.
    void refreshThumbnails();
    void reapplyTheme() override; // runtime theme change: re-fill panel + scroll, repaint History

    // Forwarded by the main window on modifier keydown/keyup so the hovered row's thumbnail
    // affordance follows the keys even while the pointer is motionless (trackballs).
    void modifiersChanged();

    // The dock's tab strip (S16-b). The History tab is a child panel sharing this dock's body;
    // the host wires its stack observer + jump callback through history().
    void setTab(DockTab tab);
    [[nodiscard]] DockTab tab() const noexcept { return m_tab; }
    [[nodiscard]] HistoryPanel* history() const noexcept { return m_history; }
    // The Channels tab's body (per-channel histogram); null before construction. The host wires its
    // composite source + change notifications through this.
    [[nodiscard]] ChannelsPanel* channels() const noexcept { return m_channels; }

    [[nodiscard]] core::Document* document() const noexcept { return m_doc; }
    [[nodiscard]] core::LayerId activeLayer() const noexcept { return m_active; }
    // Width the list's vertical scrollbar will occupy (see ScrollView::scrollbarGutter for why this
    // is pushed to the rows rather than read from `scrollbar.visible()` in their draw()).
    [[nodiscard]] int scrollGutter() const noexcept { return m_scrollGutter; }

    // The Move tool's multi-selection set (S15-c), mirrored here so the panel can highlight every
    // selected row and gate the blend/opacity strip while several layers are selected. Distinct
    // from the persistent single active layer; cleared when the move selection drops (e.g. a tool
    // switch). Empty / single sets leave the strip operating on the active layer as usual.
    void setMoveSelection(std::vector<core::LayerId> selection);
    [[nodiscard]] bool isInMoveSelection(core::LayerId id) const;
    // The set itself. back() is the PRIMARY, the convention every producer here obeys
    // (VulkanCanvas::moveTargets, core::layersInMarquee, and selectRow below).
    [[nodiscard]] const std::vector<core::LayerId>& moveSelection() const noexcept {
        return m_moveSelection;
    }
    // True while more than one layer is in the move-selection (rows then show selection dots).
    [[nodiscard]] bool multiSelectActive() const noexcept { return m_moveSelection.size() > 1; }
    // True when an edit applies across the whole move-selection (S15-e "All selected layers"). Public
    // so LayerRow can colour its selection dot: in All mode EVERY selected row reads accent (the edit
    // lands on all); otherwise the active row is accent and the rest grey.
    [[nodiscard]] bool editsAllSelected() const;

    // What a blend/opacity edit does while several layers are selected (S15-e):
    //  Disabled - the strip is inert on a multi-selection (edit one layer at a time; the default).
    //  All      - apply to every selected layer in one coalescing undo step (Affinity/Figma).
    //  Active   - apply to the active layer only (Photoshop); the strip stays live.
    enum class MultiSelectMode { Disabled, All, Active };
    void setMultiSelectionMode(MultiSelectMode mode);

    // ---- the row multi-selection grammar ----------------------------------------------------
    // The standard list vocabulary, which the dock's rows owed the canvas: a press resolves to one
    // of these (rowClickFor, from the live modifiers) and selectRow applies it to m_moveSelection.
    // The set the panel ends up with is mirrored to the host through m_onSelectionChanged, so the
    // canvas's move targets and the dock's highlighted rows never disagree.
    enum class RowClick : std::uint8_t {
        Replace, // plain press: the selection becomes exactly this row, which is the new anchor
        Toggle,  // Ctrl / Cmd: this row joins or leaves the set; it becomes the new anchor
        Extend,  // Shift: every DISPLAYED row from the anchor to this one; the anchor does not move
    };
    void selectRow(core::LayerId id, RowClick how);
    // The row a Shift-extend measures from: wherever the last plain / Ctrl press landed. Falls back
    // to the active row when it is no longer displayed (a collapsed group, an undone delete).
    [[nodiscard]] core::LayerId selectionAnchor() const noexcept { return m_selectAnchor; }

    // ---- "select the layer's pixels" (Shift-click a thumbnail) --------------------------------
    // The DOCUMENT-space coverage that gesture loads for `id`. Every layer KIND answers, each
    // through the picture the compositor already produces for its row rather than through a second
    // rasterizer of its own -- see the definition for the per-kind routing. nullopt only when there
    // is no document, no such layer, or no canvas; a kind with nothing in it answers with a
    // coverage-free Selection, which the caller reports rather than silently applying.
    [[nodiscard]] std::optional<core::Selection> layerPixelCoverage(core::LayerId id) const;
    // Cheap "does that gesture have anything to aim at?", driving the row's hand cursor and its
    // marching-ants thumbnail preview. Never composites (it runs per pointer-move); the honest
    // answer for the kinds that would need one is the CLICK's job.
    [[nodiscard]] bool layerHasSelectablePixels(core::LayerId id) const;

    // Row interactions.
    void setActive(core::LayerId id);
    void toggleVisible(core::LayerId id);
    void toggleLocked(core::LayerId id);         // undoable (SetLockedCommand)
    void toggleExpanded(core::LayerId id);      // collapse/expand a group (view state, not undoable)
    void shiftClickThumbnail(core::LayerId id);  // "select this layer's pixels" (S10-c / S13 / S14-b)

    // ---- Layer masks (S31) -----------------------------------------------------------------
    // Whether edits aim at the ACTIVE layer's MASK instead of its pixels: the dock's
    // click-the-mask-thumbnail target, read by the app's brush host at stroke begin. True only
    // while the active layer actually has a mask; switching rows re-aims at pixels.
    [[nodiscard]] bool maskEditTarget() const;
    void targetMask(core::LayerId id);   // activate the row + aim edits at its mask
    void targetPixels(core::LayerId id); // activate the row + aim edits back at its pixels
    // Context-menu mask ops -- each pushes ONE undoable command. Add seeds from the active
    // selection when there is one (the Photoshop button semantics), reveal-all otherwise; the
    // fresh mask becomes the edit target.
    void addMaskTo(core::LayerId id);
    void deleteMask(core::LayerId id);
    void toggleMaskEnabled(core::LayerId id);
    void toggleMaskLinked(core::LayerId id);
    // Shift-click the MASK thumbnail: select the mask's coverage (the gesture
    // selectionFromLayerPixels deliberately left to S31), same Ctrl/Alt boolean ops.
    void shiftClickMaskThumbnail(core::LayerId id);
    // The mask thumbnail, cache-keyed on Layer::maskRevision (cachedThumbnail's sibling). An
    // empty image for a maskless layer.
    [[nodiscard]] const common::Image& cachedMaskThumbnail(const core::Layer& layer,
                                                           bool* rebuilt = nullptr);
    // Right-click on a row: select it, then open the themed ContextMenu at the cursor.
    void showRowMenu(core::LayerId id);

    // Inline rename (double-click a row's name, or the context menu). A single-line editor is
    // floated over the row; Enter / focus-loss commit a SetNameCommand, Escape reverts. Committing
    // an unchanged (or blank) name pushes nothing, so renaming is never a spurious undo step.
    void beginRename(core::LayerId id);
    void commitRename();
    void cancelRename();
    [[nodiscard]] bool renaming() const noexcept { return m_renameId != core::kInvalidLayerId; }
    [[nodiscard]] core::LayerId renamingRow() const noexcept { return m_renameId; }

    // Repaint the row list. The rename editor is a SIBLING floating over one row, and FLTK repaints
    // a damaged child on its own -- so a row (or the whole scroll) redrawing by itself paints right
    // over the editor, which is never told to redraw. While an edit is live, damage the panel
    // instead: a full redraw walks the children in order and the editor, added last, lands on top.
    void redrawList();

    // Row drag (called by LayerRow during its event handling; they read Fl::event_* directly). A
    // press selects the row; dragging past a small threshold starts a reorder/reparent, or a clone
    // if released over the New Layer ("+") button (outlined green for the whole drag as a
    // clone-target affordance, then filled solid green while the pointer is actually over it).
    void rowPressed(core::LayerId id);
    void rowDragged();
    void rowReleased();

    // True while `id`'s row is the one being dragged: the row paints itself as a muted, dashed
    // "slot" (its thumbnail + name have been "lifted" into the floating ghost). Read by LayerRow.
    [[nodiscard]] bool isRowLifted(core::LayerId id) const noexcept {
        return m_dragging && id == m_dragId;
    }

    // Toolbar actions (also reachable from the Layer menu).
    void addRasterLayer();
    void deleteActive();
    void duplicateActive(); // clone the active layer in place (Duplicate / drag-onto-plus)
    void groupActive();      // wrap the active layer in a new group (Layer->Group Layers)

    // True when the active layer refuses structural edits (delete / reorder / reparent / group)
    // because it is locked. Pixel and transform edits are refused elsewhere (the brush guard and
    // VulkanCanvas::beginMoveGesture); visibility, opacity and blend stay editable by design.
    [[nodiscard]] bool activeLayerLocked() const;

    // The layer's thumbnail, re-rendered only when its content revision, transform, or (for text)
    // its renderer-populated pixel cache moved. Re-rendering EVERY thumbnail per rebuildRows made
    // adding the Nth layer O(N · pixels). `rebuilt`, when given, reports whether this call actually
    // re-rendered -- refreshThumbnails() uses it to redraw only the rows that changed.
    [[nodiscard]] const common::Image& cachedThumbnail(const core::Layer& layer,
                                                       bool* rebuilt = nullptr);

    // Properties-strip widget callbacks (invoked by the FLTK choice/slider; public so the
    // file-local callback thunks can reach them, as with the toolbar actions above).
    void onBlendChanged();   // blend-mode dropdown
    void onOpacityChanged(); // opacity slider (coalesces a drag into one undo step)

    // The dock is width-resizable (the splitter on its left edge), so the children are laid out
    // explicitly rather than left to Fl_Group's proportional resize -- the dropdown, slider, readout
    // column and button strip all have fixed or anchored geometry. Public: the main window places
    // the body regions itself.
    void resize(int X, int Y, int W, int H) override;

protected:
    void draw() override; // header chrome, the properties-strip captions, empty-state, drag feedback
    int handle(int event) override; // tab-strip clicks (the header has no child widgets)

private:
    void layoutChildren(); // (re)place every child from the panel's current x/y/w/h
    void updateScrollGutter(); // recompute scrollGutter() after a row-count or viewport change
    void rebuildRows();
    void applyTabVisibility(); // show/hide the Layers widgets vs the History child (S16-b)
    // The i-th header tab's horizontal span [first, second), or -1 width when out of range.
    [[nodiscard]] std::pair<int, int> tabSpan(int index) const;
    void notifyChanged();   // recomposite via m_onChange
    void selectTopLayer();
    void syncProperties();  // push the active layer's blend/opacity into the strip widgets
    void syncActionButtons(); // grey the bottom strip's Delete/Group on a locked (or absent) layer
    // The blend-mode label for a multi-selection: the distinct modes across the set, in stack
    // order, joined ("Normal, Multiply"). A single distinct mode returns just its name.
    [[nodiscard]] std::string mixedBlendLabel() const;
    // The distinct blend-mode indices across the move-selection (stack order), for the blend
    // dropdown's flyout dots in a mixed multi-selection (S15-e). Mirrors mixedBlendLabel().
    [[nodiscard]] std::vector<int> distinctBlendIndices() const;
    // True when the move-selection's layers don't all share one opacity (the readout shows "~avg").
    [[nodiscard]] bool selectionOpacitiesMixed() const;
    void captureDragGhost(); // snapshot the dragged layer's (faded) thumbnail + name at drag start
    void drawDragGhost();    // paint the floating ghost chip at the cursor (called from draw())
    // `id`'s index in the DISPLAYED row list, or -1 (collapsed inside a group, or gone).
    [[nodiscard]] int rowIndexOf(core::LayerId id) const;
    // The displayed rows from `fromId` to `toId` inclusive, walked FROM the anchor -- so the row
    // that was clicked always lands LAST, i.e. back() is the primary (setMoveTargets mirrors the
    // primary back into this panel's active row, and a range whose back() were the far end of the
    // sweep would yank the active row off the one you actually clicked).
    [[nodiscard]] std::vector<core::LayerId> rowsBetween(core::LayerId fromId,
                                                         core::LayerId toId) const;
    void status(std::string message); // narrate through m_onStatus (a no-op when unbound)

    // Where a drop would land, resolved from the cursor's Y against the displayed rows. Used by
    // both the drag-feedback draw and the release that issues the MoveLayerCommand.
    struct DropPlan {
        bool valid = false;
        core::LayerId parentId = core::kInvalidLayerId;
        std::size_t endIndex = 0; // desired final child index in parent (BEFORE removal)
        // The group the dragged layer would JOIN, or invalid for the top level. The draw rings it
        // so in-group vs out-of-group is unambiguous; a position line is always drawn alongside.
        core::LayerId groupRow = core::kInvalidLayerId;
        int lineY = 0;            // drop-line y
        int lineDepth = 0;        // drop-line indent depth (the target's child depth)
    };
    [[nodiscard]] DropPlan planDrop(int eventY) const;
    [[nodiscard]] bool overPlusButton() const; // is the pointer over the New Layer button?
    // Widget-space pixel rect (common::Rect is a doc-space double rect -- the wrong currency here).
    struct PixelRect {
        int x = 0, y = 0, w = 0, h = 0;
    };
    // Where the floating ghost chip's CARD (not its shadow margin) currently sits, in panel coords.
    [[nodiscard]] PixelRect ghostCardRect() const;
    // The colour a pixel of the drop-line knob composites against: the translucent ghost card where
    // the two overlap, otherwise the fill of the row underneath. Feeds drawAAPrims's `under`.
    [[nodiscard]] common::Color8 colorUnderKnob(int px, int py) const;
    // True if `node` is `ancestor` itself or sits inside its subtree (an invalid move destination).
    [[nodiscard]] bool isSelfOrDescendant(core::LayerId ancestor, core::LayerId node) const;

    core::Document* m_doc = nullptr;
    core::LayerId m_active = core::kInvalidLayerId;
    std::vector<core::LayerId> m_moveSelection; // S15-c Move-tool multi-selection (row highlight + gating)
    core::LayerId m_selectAnchor = core::kInvalidLayerId; // Shift-extend's anchor row
    MultiSelectMode m_multiMode = MultiSelectMode::Disabled; // S15-e: how multi-selection edits apply
    std::function<void()> m_onChange;
    std::function<void(core::LayerId)> m_onOpenEffects; // fx-badge click -> open the effects modal
    std::function<void(core::LayerId)> m_onOpenTexture; // texture-badge click -> the generator modal
    std::function<void()> m_onMergeDown;                // context menu -> the host's Merge Down
    std::function<void(core::LayerId)> m_onRasterize;   // ... -> the host's Rasterize
    std::function<void(core::LayerId)> m_onConvertToPath;
    std::function<void(const std::vector<core::LayerId>&)> m_onSelectionChanged;
    std::function<void(std::string)> m_onStatus;        // refusals -> the host's status bar
    DockTab m_tab = DockTab::Layers;       // active dock tab (S16-b)
    HistoryPanel* m_history = nullptr;     // the History tab's body (child; hidden on Layers)
    ChannelsPanel* m_channels = nullptr;   // the Channels tab's body (child; per-channel histogram)
    ScrollView* m_scroll = nullptr;
    Dropdown* m_blendChoice = nullptr;   // active-layer blend mode
    Slider* m_opacitySlider = nullptr;   // active-layer opacity (0..1)
    // The bottom strip: the two constructive actions sit together on the left, the destructive one
    // is exiled to the right edge so a mis-aimed click cannot delete a layer.
    IconButton* m_addButton = nullptr;
    IconButton* m_groupButton = nullptr;
    IconButton* m_deleteButton = nullptr;
    // The inline rename editor: ONE editor, floated over whichever row is being renamed. It is a
    // child of the panel (not of the scroll) so rebuildRows()'s m_scroll->clear() cannot delete the
    // widget whose handle() the commit is running inside.
    TextInput* m_renameEditor = nullptr;
    core::LayerId m_renameId = core::kInvalidLayerId;
    bool m_renameCommitting = false; // re-entrancy latch: hide() -> FL_UNFOCUS -> commitRename()
    int m_scrollGutter = 0;          // see scrollGutter()
    std::vector<LayerRow*> m_rows; // current rows, display order (top of stack first)

    // Drag state.
    core::LayerId m_dragId = core::kInvalidLayerId;
    int m_pressY = 0;
    int m_dragY = 0;
    int m_dragX = 0;  // cursor x during the drag (drives the ghost), updated alongside m_dragY
    bool m_dragging = false;
    bool m_dragOverPlus = false;

    // The floating drag chip, composited ONCE at drag start into a real RGBA image (card + border +
    // thumbnail + name, plus a soft shadow in the bottom-right margin) and blitted with alpha every
    // frame. It used to be an opaque card painted with fl_rectf, which meant the drop-line had to
    // hide under it or be slashed across it; a genuinely translucent chip lets the line and its
    // start knob be drawn ON TOP and still read (S16-g).
    common::Image m_ghostChip;                 // RGBA, card + shadow margin
    std::unique_ptr<Fl_RGB_Image> m_ghostImg;  // views m_ghostChip's pixels; rebuilt with it
    int m_ghostCardW = 0;                      // the card's size, excluding the shadow margin
    int m_ghostCardH = 0;
    std::uint64_t m_opacityCoalesce = 1; // gesture id, bumped each time an opacity drag ends

    // Thumbnail cache (see cachedThumbnail): keyed by layer id, validated against the inputs a
    // thumbnail depends on, pruned to the document's current layers on each rebuild.
    struct ThumbEntry {
        common::Image image;
        std::uint64_t contentRev = 0;
        common::Affine2D transform;
        // A TextLayer's pixels live in a cache the RENDERER fills, and filling it bumps no content
        // revision. On document open the panel builds its rows before the first composite, so a
        // text (and especially a 3D text) layer would keep its blank placeholder for ever. Key on
        // the cache itself: null -> non-null, or a re-render to a different size, invalidates.
        const void* textCache = nullptr;
        std::uint32_t textCacheW = 0;
        std::uint32_t textCacheH = 0;
        // The CANVAS size the thumbnail was rendered against. Everything derived at document
        // resolution re-frames when the canvas does -- a group's composite, an adjustment's scope
        // preview (whose aspect ratio comes straight off the doc rect), a vector layer's
        // rasterization, and any layer with no contentBounds, which frames the doc rect as its
        // fallback -- yet a crop / canvas resize moves NO layer revision and no world transform,
        // so without this the whole set survives one unchanged.
        std::uint32_t docW = 0;
        std::uint32_t docH = 0;
        // The mask thumbnail's slot (S31), keyed on Layer::maskRevision -- every mask mutation
        // (paint, flags, add/delete) bumps it, so a byte-level coverage hash is never needed.
        common::Image maskImage;
        std::uint64_t maskRev = static_cast<std::uint64_t>(-1);
    };
    std::unordered_map<core::LayerId, ThumbEntry> m_thumbCache;
    bool m_maskTarget = false; // edits aim at the active layer's mask (see maskEditTarget())
};

// Convert a desired FINAL child index in a parent (as if nothing were removed) to the
// MoveLayerCommand `newIndex`, which is measured AFTER the dragged layer has been removed: a
// same-parent move past the dragged layer's old slot shifts the target down by one. Pure;
// unit-tested.
[[nodiscard]] std::size_t moveIndexFor(std::size_t endIndex, bool sameParent, std::size_t oldIndex);

// What a press on a layer ROW does to the panel's multi-selection. Ctrl (Cmd on macOS -- the
// caller passes FL_COMMAND, which resolves per platform) toggles the row in or out; Shift extends
// the range from the anchor. Shift WINS over Ctrl when both are down: "extend the toggled set" is a
// gesture no editor agrees on, and extending is the half people actually reach for. Pure;
// unit-tested. (Shift over a THUMBNAIL never reaches here -- that hit region is spent on the
// select-the-layer's-pixels gesture, which is why the row grammar's toggle key is Ctrl rather than
// Photoshop's Shift.)
[[nodiscard]] LayerPanel::RowClick rowClickFor(bool shift, bool command);

// The boolean op a Shift-click on a layer thumbnail applies (S14-b): Shift is spent as the
// gesture trigger, so the *other* modifiers choose -- Ctrl = Add, Alt = Subtract, both =
// Intersect, none = Replace. (Add/Intersect land on Photoshop's exact key combos, whose
// trigger is Ctrl.) Pure; unit-tested.
[[nodiscard]] core::SelectOp thumbnailSelectOp(bool ctrl, bool alt);

// The background fill for one layer row. One ramp, strongest first -- precedence is
// **active > hover > selected**:
//   active   -> pal.controlActive  (the active layer; unchanged, and always the loudest row)
//   hover    -> pal.controlHover   (the row under the pointer, selected or not)
//   selected -> pal.controlSelected(in the S15-c multi-selection but not active: a SLIGHT tint,
//                                   about half a hover, so the set reads as a group at a glance)
//   else     -> pal.panelBg
// Hover outranking selection is what keeps the pointer's feedback alive on an already-selected row
// (the fill still steps up when you cross it); the selection DOT is what goes on saying "selected"
// for that one row, and every other selected row keeps its tint, so the group never disappears.
// `enabled` is the row's active_r(): a greyed panel (inpaint chrome lock) paints the rest state
// only, exactly as before. Pure (no FLTK), so it is unit-tested against both palettes.
[[nodiscard]] common::Color8 layerRowBackground(const Palette& pal, bool enabled, bool active,
                                                bool selected, bool hover);

// The badge a layer earns from its kind -- and, for the one vector kind, from its GEOMETRY first
// and its PAINT second (a pen path outranks a gradient fill; see the definition for why). A
// VectorLayer with no object earns none, exactly as it did before the split. Pure; unit-tested.
[[nodiscard]] LayerRow::TypeBadge typeBadgeFor(const core::Layer& layer);

// The px a row's type/paste badge occupies, 0 when it draws none. THE WIDTH ORACLE: it MUST agree
// with the branch chain at the bottom of LayerRow::draw(), because the layer name is ellipsized
// against this number and a badge that draws wider than it claims gets a name run under it. The
// paste marker REPLACES the type badge (they are one slot), which is why it is answered here rather
// than added. Pure, so the agreement is pinned by a test instead of by eye.
[[nodiscard]] int typeBadgeWidth(LayerRow::TypeBadge badge, bool pastedMarker);

// Build a `box`x`box` opaque thumbnail for a layer: the DOCUMENT rect aspect-fit over a
// checkerboard, with the layer's pixels sampled through its transform (a doc-space view, like
// Photoshop's canvas-clipped thumbs — transform edits show); a neutral placeholder for kinds
// without pixels. Pure (no FLTK), so it is unit-tested.
[[nodiscard]] common::Image layerThumbnail(const core::Layer& layer, int box, std::uint32_t docW,
                                           std::uint32_t docH);

// Build a `box`x`box` opaque thumbnail of a raster mask (S31): the coverage as grayscale
// (0 = hidden = black, 255 = revealed = white), aspect-fit over a dark neutral ground. Pure
// (no FLTK), so it is unit-tested like layerThumbnail.
[[nodiscard]] common::Image maskThumbnail(const core::RasterMask& mask, int box);

} // namespace mosaic::ui
