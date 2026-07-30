#pragma once

#include "common/i18n.hpp" // N_(): mark for extraction; the History panel translates

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/geometry.hpp"
#include "core/blend_mode.hpp"
#include "core/command.hpp"
#include "core/guides.hpp"
#include "core/layer.hpp"
#include "core/selection.hpp"

// Concrete edits on the document model. They address layers by LayerId, call the model's
// mutators, and store what they need to reverse themselves. Scalar-property commands capture
// the old value on first apply() (so callers only supply the new value). SetOpacity and
// SetTransform coalesce within a gesture (same target + same non-zero coalesce id).
//
// Structural commands move ownership of layers in and out of the tree (held in a unique_ptr
// while detached), so a removed/added layer keeps its identity across undo/redo.
namespace mosaic::core {

// Bundles several sub-commands into one undo step (applied in order, undone in reverse). Build
// it with add() before pushing; the sub-commands must NOT be pre-applied.
class CompositeCommand : public Command {
public:
    explicit CompositeCommand(std::string name) : m_name(std::move(name)) {}
    void add(std::unique_ptr<Command> cmd) { m_children.push_back(std::move(cmd)); }
    [[nodiscard]] bool empty() const noexcept { return m_children.empty(); }

    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return m_name; }

private:
    std::string m_name;
    std::vector<std::unique_ptr<Command>> m_children;
};

// One step of a history LOADED from a saved .mosaic (S48): it owns a snapshot of the document's
// whole layer tree at the state BELOW it, and apply()/undo() are the same operation -- swap that
// snapshot with the document's current layer tree. Adjacent LoadedStateCommands on the stack thus
// walk the document between saved states as the History panel jumps. Loaded pre-applied via
// CommandStack::adoptHistory (never push()ed, which would re-apply). This is the first cut of the
// LiveUndoModel (spec 3.5): whole-tree swap now; per-key dirty-set replay is the later refinement.
class LoadedStateCommand : public Command {
public:
    LoadedStateCommand(std::string name, std::vector<std::unique_ptr<Layer>> below)
        : m_name(std::move(name)), m_layers(std::move(below)) {}
    void apply(Document& doc) override { swapTree(doc); }
    void undo(Document& doc) override { swapTree(doc); }
    [[nodiscard]] std::string_view name() const override { return m_name; }

private:
    void swapTree(Document& doc); // exchange m_layers with the document root's children
    std::string m_name;
    std::vector<std::unique_ptr<Layer>> m_layers;
};

// Insert a (pre-built, un-inserted) layer into `parentId` at `index`.
class AddLayerCommand : public Command {
public:
    AddLayerCommand(LayerId parentId, std::size_t index, std::unique_ptr<Layer> layer);
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Add Layer"); }
    [[nodiscard]] LayerId layerId() const noexcept { return m_layerId; }

private:
    LayerId m_parentId;
    std::size_t m_index;
    LayerId m_layerId;
    std::unique_ptr<Layer> m_held;  // owns the layer while it is outside the tree
};

// Remove a layer, remembering its location so undo restores it exactly.
class RemoveLayerCommand : public Command {
public:
    explicit RemoveLayerCommand(LayerId layerId) : m_layerId(layerId) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Delete Layer"); }

private:
    LayerId m_layerId;
    LayerId m_parentId = kInvalidLayerId;
    std::size_t m_index = 0;
    std::unique_ptr<Layer> m_held;
};

// Swap `targetId` for a pre-built `replacement`, in place: same parent, same index. Undo puts the
// original back exactly where it was, so the replacement never leaks and the tree order is stable.
//
// Both destructive layer conversions ride this: Layer -> Rasterize (a Text/Vector/Magic/Group layer
// becomes a RasterLayer of its baked pixels) and Layer -> Convert to Path (a Text or Vector layer
// becomes a VectorLayer holding an editable cubic Path). `label` names the step in History; the
// replacement carries its own id, so anything holding the old LayerId must re-resolve after this.
class ReplaceLayerCommand : public Command {
public:
    ReplaceLayerCommand(LayerId targetId, std::unique_ptr<Layer> replacement, std::string label);
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return m_label; }
    [[nodiscard]] LayerId replacementId() const noexcept { return m_replacementId; }

private:
    LayerId m_targetId;
    LayerId m_replacementId;
    LayerId m_parentId = kInvalidLayerId;
    std::size_t m_index = 0;
    std::string m_label;
    std::unique_ptr<Layer> m_incoming; // owns the replacement while it is outside the tree
    std::unique_ptr<Layer> m_outgoing; // ... and the original once it has been swapped out
};

// Move a layer to `newParentId` at `newIndex`. `newIndex` is the target position among the
// destination's children AFTER the layer has been removed from its old location (see
// docs/document-model.md). Supports both reorder and reparent.
class MoveLayerCommand : public Command {
public:
    MoveLayerCommand(LayerId layerId, LayerId newParentId, std::size_t newIndex);
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Move Layer"); }

private:
    LayerId m_layerId;
    LayerId m_newParentId;
    std::size_t m_newIndex;
    LayerId m_oldParentId = kInvalidLayerId;
    std::size_t m_oldIndex = 0;
    bool m_captured = false;
};

class SetNameCommand : public Command {
public:
    SetNameCommand(LayerId id, std::string newName) : m_id(id), m_new(std::move(newName)) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Rename Layer"); }

private:
    LayerId m_id;
    std::string m_new;
    std::string m_old;
    bool m_oldMarker = false; // naming a pasted layer adopts it (clears the badge); undo restores
    // A Text layer's row caption follows its content until someone names it by hand. Renaming is
    // exactly that moment, so apply() drops the auto-name flag -- otherwise the panel would keep
    // showing the first line of text and the rename would look like it did nothing. Undo restores.
    bool m_oldAutoName = false;
    bool m_captured = false;
};

// Rename the DOCUMENT itself (File -> Rename Document): the title is the document's identity in
// the titlebar and the New-Document dialog's Recent cards (the tab strip shows the file name),
// so renaming rides the command stack like any edit -- undoable, and it marks the document
// dirty through the saved marker so the new name reaches the next save's manifest.
class RenameDocumentCommand : public Command {
public:
    explicit RenameDocumentCommand(std::string newTitle) : m_new(std::move(newTitle)) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Rename Document"); }

private:
    std::string m_new;
    std::string m_old;
    bool m_captured = false;
};

class SetVisibleCommand : public Command {
public:
    SetVisibleCommand(LayerId id, bool visible) : m_id(id), m_new(visible) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Toggle Visibility"); }

private:
    LayerId m_id;
    bool m_new;
    bool m_old = false;
    bool m_captured = false;
};

// Lock / unlock a layer. `Layer::locked` long predates this command (Merge Down and the paint
// guard already refuse a locked layer); this is the undoable way to flip it. A locked layer still
// selects, still hides, and still takes opacity/blend edits -- what it refuses is anything that
// changes its PIXELS, its TRANSFORM, or its place in the stack (see LayerPanel / the Layer menu).
class SetLockedCommand : public Command {
public:
    SetLockedCommand(LayerId id, bool locked) : m_id(id), m_new(locked) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return m_new ? "Lock Layer" : "Unlock Layer"; }

private:
    LayerId m_id;
    bool m_new;
    bool m_old = false;
    bool m_captured = false;
};

class SetBlendModeCommand : public Command {
public:
    SetBlendModeCommand(LayerId id, BlendMode mode) : m_id(id), m_new(mode) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Set Blend Mode"); }

private:
    LayerId m_id;
    BlendMode m_new;
    BlendMode m_old = BlendMode::Normal;
    bool m_captured = false;
};

class SetClipToBelowCommand : public Command {
public:
    SetClipToBelowCommand(LayerId id, bool clip) : m_id(id), m_new(clip) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Clip to Layer Below"); }

private:
    LayerId m_id;
    bool m_new;
    bool m_old = false;
    bool m_captured = false;
};

// Coalescing scalar edit: consecutive pushes with the same target and the same non-zero
// `coalesceId` (one gesture) collapse into a single undo step.
class SetOpacityCommand : public Command {
public:
    SetOpacityCommand(LayerId id, float opacity, std::uint64_t coalesceId = 0)
        : m_id(id), m_new(opacity), m_coalesce(coalesceId) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Set Opacity"); }
    bool tryMergeWith(const Command& next) override;

private:
    LayerId m_id;
    float m_new;
    float m_old = 1.0f;
    bool m_captured = false;
    std::uint64_t m_coalesce;
};

// Coalescing layer-effects edit (LE-b): replace a layer's whole std::optional<LayerEffects> in one
// step (std::nullopt = no effects). Consecutive pushes with the same target and the same non-zero
// `coalesceId` (one slider drag / flyout gesture) collapse into a single undo step, like
// SetOpacityCommand. The value is small (a handful of structs), so storing old + new is cheap.
class SetLayerEffectsCommand : public Command {
public:
    SetLayerEffectsCommand(LayerId id, std::optional<LayerEffects> effects,
                           std::uint64_t coalesceId = 0)
        : m_id(id), m_new(std::move(effects)), m_coalesce(coalesceId) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Layer Effects"); }
    bool tryMergeWith(const Command& next) override;

private:
    LayerId m_id;
    std::optional<LayerEffects> m_new;
    std::optional<LayerEffects> m_old;
    bool m_captured = false;
    std::uint64_t m_coalesce;
};

class SetTransformCommand : public Command {
public:
    SetTransformCommand(LayerId id, const common::Affine2D& transform, std::uint64_t coalesceId = 0)
        : m_id(id), m_new(transform), m_coalesce(coalesceId) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Transform Layer"); }
    bool tryMergeWith(const Command& next) override;

private:
    LayerId m_id;
    common::Affine2D m_new;
    common::Affine2D m_old;
    bool m_captured = false;
    std::uint64_t m_coalesce;
};

// Transform several layers in ONE undo step -- the Move tool's multi-selection drag (S15-c), where
// the user shift-clicks to gather layers and drags them as a set without grouping. Each entry is an
// (id, new-transform) pair; the old transforms are captured on first apply. Coalesces like its
// single-layer sibling, but only when the next command targets the SAME ids in the SAME order with
// the SAME non-zero coalesce id (a stable selection through a drag), so a drag stays one step.
class SetTransformsCommand : public Command {
public:
    struct Entry {
        LayerId id;
        common::Affine2D transform;
    };
    SetTransformsCommand(std::vector<Entry> entries, std::uint64_t coalesceId = 0);
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Transform Layers"); }
    bool tryMergeWith(const Command& next) override;

private:
    struct Item {
        LayerId id;
        common::Affine2D neu;
        common::Affine2D old;
    };
    std::vector<Item> m_items;
    bool m_captured = false;
    std::uint64_t m_coalesce;
};

// Set several layers' opacities in ONE undo step -- S15-e's "All selected layers" multi-selection
// edit (the opacity slider applied across the Move-tool selection). Mirrors SetTransformsCommand:
// each entry is an (id, opacity) pair, old values captured on first apply, and a drag coalesces while
// the next command targets the SAME ids in the SAME order with the SAME non-zero coalesce id.
class SetOpacitiesCommand : public Command {
public:
    struct Entry {
        LayerId id;
        float opacity;
    };
    SetOpacitiesCommand(std::vector<Entry> entries, std::uint64_t coalesceId = 0);
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Set Opacity"); }
    bool tryMergeWith(const Command& next) override;

private:
    struct Item {
        LayerId id;
        float neu;
        float old;
    };
    std::vector<Item> m_items;
    bool m_captured = false;
    std::uint64_t m_coalesce;
};

// Replace a raster layer's pixels wholesale (S14-b: Edit/Cut's destructive half; later pixel
// edits may reuse it until tiled dirty-region commands arrive with S60-c). Stores the previous
// image on first apply -- two full copies per command, the same trade SetSelectionCommand makes.
class SetLayerPixelsCommand : public Command {
public:
    // Whole-layer replace: stores the entire old + new image (heavy on big layers; prefer the
    // region constructor when only a sub-rect changed). The region is the whole image.
    SetLayerPixelsCommand(LayerId id, common::Image next);
    // Region-scoped replace (S60-a): `regionPixels` are JUST the layer-local rect at integer origin
    // (originX, originY) with the image's own width/height as the extent. Only that sub-rectangle's
    // old + new pixels are stored and patched on apply/undo, so a brush stroke or inpaint fill costs
    // its bounding box, not the whole layer -- both in memory and in the apply/undo copy.
    SetLayerPixelsCommand(LayerId id, common::Image regionPixels, long originX, long originY);
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Edit Pixels"); }
    [[nodiscard]] std::optional<common::Rect> dirtyRegion(const Document& doc) const override;
    // The SAME rect this command stores, in the layer's own pixel space -- not the document AABB
    // dirtyRegion reports (S60-a item 13). The whole-layer form names no rect on purpose: it may
    // resize the image, and every tile index would then mean different pixels.
    [[nodiscard]] std::optional<LayerPixelEdit> dirtyLayerPixels(const Document& doc) const override;

private:
    LayerId m_id;
    bool m_wholeLayer = true; // true: replace the whole image (may resize); false: patch m_region
    long m_x = 0;             // layer-local origin of the stored region (region mode)
    long m_y = 0;
    common::Image m_new; // new pixels (whole image, or region-sized)
    common::Image m_old; // old pixels, captured on first apply
    bool m_captured = false;
};

// A brush stroke that also GREW its layer -- the bounded auto-grow (core/layer_grow.hpp): the
// stroke wandered off the layer's own pixel grid, so the grid is enlarged to take it, never past the
// canvas. Growth and paint are ONE undo step, because to the user they are one action.
//
// On apply the layer's existing image is re-homed into a `newW` x `newH` grid with its old top-left
// landing at (offsetX, offsetY) -- byte-exact, `copyRegion` reading outside the source as
// transparent, so the new band arrives empty -- the layer transform is post-translated by
// (-offsetX, -offsetY) so not one existing pixel MOVES in document space, and the stroke's region is
// then patched in. A raster layer's mask rides its image grid 1:1, so a mask is grown with it and
// the new band REVEALS (255): the band was outside the layer a moment ago, so revealing it cannot
// change a single composited pixel, while hiding it would make the new paint invisible.
//
// Undo restores the old image, the old mask and the old transform verbatim -- the growth is not
// "shrunk back" by arithmetic, it is replaced by what was captured, so a round trip is byte-exact.
// The History label is SetLayerPixelsCommand's, deliberately: to the user this is the same edit.
class GrowAndPaintLayerCommand : public Command {
public:
    // `regionPixels` at (regionX, regionY) are in the coordinates of the NEW (grown) grid.
    GrowAndPaintLayerCommand(LayerId id, std::uint32_t newW, std::uint32_t newH, long offsetX,
                             long offsetY, common::Image regionPixels, long regionX, long regionY);
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Edit Pixels"); }
    // Deliberately nullopt: growth changes the layer's EXTENT and its placement, so the region that
    // needs recompositing is not the stroke's box -- it is wherever the layer used to be plus
    // wherever it is now. The caller's fallback (a full recomposite) is the correct answer, and this
    // command fires at most once per layer per session.
    [[nodiscard]] std::optional<common::Rect> dirtyRegion(const Document& doc) const override;

private:
    LayerId m_id;
    std::uint32_t m_w; // the grown grid
    std::uint32_t m_h;
    long m_offX; // where the OLD image's (0,0) lands in the new grid
    long m_offY;
    common::Image m_new; // the stroke's region, in NEW-grid coordinates
    long m_rx;
    long m_ry;
    common::Image m_old;                 // the whole pre-growth image, captured on first apply
    std::optional<RasterMask> m_oldMask; // ... and its mask, when it had one
    common::Affine2D m_oldTransform;
    bool m_captured = false;
};

// One applied warp -- Mesh Warp or Perspective Warp (S35-b; docs/warp-tools.md) -- as ONE undo step.
//
// A warp is three things at once, and undo has to reverse all three together or the layer comes back
// wrong: the PIXELS (deformed, and at a new extent -- render::warpImage's output is tight to the
// deformed content), the PLACEMENT (post-translated by the reported offset so not one pixel already
// on the canvas moves), and the GRID stored on the layer (what makes re-entering the tool restore the
// handles). All six values -- old and new -- are carried, so undo restores the layer verbatim rather
// than trying to invert a deformation, which in general has no inverse at all.
//
// Deliberately NOT coalescing. A warp is a deliberate, framed, Apply-ed act, not a continuous
// scrub: one Apply is one history entry, and merging two of them would mean the intermediate state
// -- which the user looked at and accepted -- could never be returned to.
//
// Both pixel-bearing kinds ride it: RasterLayer::image() and MagicLayer::source(). The kinds whose
// pixels are a CACHE (Text, Texture) are refused by the tool, because their next cache refresh would
// discard the bake; Group / Adjustment have no pixel grid; a Vector layer is told to Rasterize first.
class SetLayerWarpCommand : public Command {
public:
    // `nextPixels` replaces the layer's whole image; `nextTransform` is its placement with the warp's
    // offset already folded in; `nextGrid` is the grid to store (std::nullopt clears it).
    SetLayerWarpCommand(LayerId id, common::Image nextPixels, common::Affine2D nextTransform,
                        std::optional<WarpGrid> nextGrid)
        : m_id(id), m_new(std::move(nextPixels)), m_newTransform(nextTransform),
          m_newGrid(std::move(nextGrid)) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Warp"); }
    // Deliberately nullopt (GrowAndPaintLayerCommand's reasoning): a warp changes the layer's EXTENT
    // and its placement, so the region needing a recomposite is wherever the layer used to be plus
    // wherever it is now -- the caller's whole-document fallback is the correct answer.
    [[nodiscard]] std::optional<common::Rect> dirtyRegion(const Document& doc) const override;

private:
    LayerId m_id;
    common::Image m_new;
    common::Affine2D m_newTransform;
    std::optional<WarpGrid> m_newGrid;
    common::Image m_old;                 // captured on first apply
    common::Affine2D m_oldTransform;
    std::optional<WarpGrid> m_oldGrid;
    bool m_captured = false;
};

// A named pixel-region fill (S39 Edit→Fill…, shared with S21 bucket/pattern fill). Mechanically a
// region-scoped SetLayerPixelsCommand: the caller precomputes the filled pixels (render::computeFill,
// which needs the blend modes that live in render/ — off-limits to core) and hands the region here,
// so undo/redo is the same byte-exact region patch. The only difference is the History label, "Fill".
class FillCommand : public SetLayerPixelsCommand {
public:
    FillCommand(LayerId id, common::Image regionPixels, long originX, long originY)
        : SetLayerPixelsCommand(id, std::move(regionPixels), originX, originY) {}
    [[nodiscard]] std::string_view name() const override { return N_("Fill"); }
};

// Set (or clear, with std::nullopt) a layer's whole raster mask in one step -- Add Mask / Delete
// Mask / Mask from Selection (S31). Stores the full old + new masks, the same trade
// SetSelectionCommand makes (a mask is one byte per pixel; acceptable until S60-c tiling). The
// History label is caller-supplied so the panel reads "Add Mask", "Delete Mask" or "Mask from
// Selection" as appropriate.
class SetLayerMaskCommand : public Command {
public:
    SetLayerMaskCommand(LayerId id, std::optional<RasterMask> next, std::string label = "Add Mask")
        : m_id(id), m_new(std::move(next)), m_label(std::move(label)) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return m_label; }

private:
    LayerId m_id;
    std::optional<RasterMask> m_new;
    std::optional<RasterMask> m_old;
    std::string m_label;
    bool m_captured = false;
};

// Enable / disable a layer's mask (the RasterMask::enabled flag): a disabled mask is ignored by
// the compositor but keeps its pixels -- the non-destructive "see the layer without its mask"
// toggle. A no-op when the layer has no mask.
class SetMaskEnabledCommand : public Command {
public:
    SetMaskEnabledCommand(LayerId id, bool enabled) : m_id(id), m_new(enabled) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override {
        return m_new ? "Enable Mask" : "Disable Mask";
    }

private:
    LayerId m_id;
    bool m_new;
    bool m_old = true;
    bool m_captured = false;
};

// Link / unlink a layer's mask (the RasterMask::linked flag): linked rides the layer's transform,
// unlinked stays put in the parent's space while the pixels move under it (the compositor owns
// the semantics -- render/compositor.cpp foldUnlinkedMask). A no-op when the layer has no mask.
//
// The flag also picks which space RasterMask::toLocal maps into, so the flip REBASES the sheet
// through the layer's own transform: clicking the chain must leave the mask exactly where it is
// and only change what happens on the NEXT move. Without that the mask jumps by the layer
// transform the moment it is clicked -- clear off the canvas on a shape layer, whose transform
// carries its whole position.
class SetMaskLinkedCommand : public Command {
public:
    SetMaskLinkedCommand(LayerId id, bool linked) : m_id(id), m_new(linked) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override {
        return m_new ? "Link Mask" : "Unlink Mask";
    }

private:
    LayerId m_id;
    bool m_new;
    bool m_old = true;
    common::Affine2D m_oldToLocal;  // the sheet's placement before the flip rebased it
    bool m_captured = false;
};

// Slide a layer's MASK SHEET without touching one byte of its coverage: a new RasterMask::toLocal
// per layer -- the sheet's placement, mask px -> the space the mask lives in (layer-local when
// linked, the layer's PARENT's when not; the grid contract in core/layer.hpp).
//
// The Arrange menu is what needs it. Aligning a layer writes its transform, and a LINKED mask rides
// that for free; an UNLINKED one deliberately does not (it stays put in parent space while the
// pixels move under it), so a masked adjustment / filter / generator would align its content out
// from under a sheet that never moved and the visible blob would not budge at all. Sliding the
// sheet by the same document-space delta (core::translatedMaskPlacement) is the other half of ONE
// user action, so it rides inside Arrange's CompositeCommand and never names an undo step of its own.
//
// Deliberately NOT SetLayerMaskCommand: that stores two whole coverage planes -- a byte per pixel,
// each way -- to describe what is a 2x3 change of placement, which for a translation would be a
// heavyweight lie. Shaped instead like SetTransformsCommand, its exact sibling: one (id, value) pair
// per layer, old values captured on first apply, coalescing only when the next command targets the
// SAME ids in the SAME order under the same non-zero id.
//
// No dirtyRegion() override, matching both neighbours it travels with: moving a sheet changes what
// shows BOTH where it was and where it now is, which is not one rect this command can name honestly
// (GrowAndPaintLayerCommand's reasoning), and the SetTransformsCommand beside it forces a full
// recomposite regardless.
class SetMaskPlacementCommand : public Command {
public:
    struct Entry {
        LayerId id;
        common::Affine2D toLocal;
    };
    explicit SetMaskPlacementCommand(std::vector<Entry> entries, std::uint64_t coalesceId = 0);
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Move Mask"); }
    bool tryMergeWith(const Command& next) override;

private:
    struct Item {
        LayerId id;
        common::Affine2D neu;
        common::Affine2D old;
    };
    std::vector<Item> m_items;
    bool m_captured = false;
    std::uint64_t m_coalesce;
};

// Patch a rectangular region of a layer's mask coverage (the S31 mask-paint stroke's commit) --
// SetLayerPixelsCommand's region pattern on the 8-bit coverage plane: only the touched rect's old
// + new bytes are stored, so a stroke costs its bounding box. The region is clipped to the mask on
// apply; a no-op when the layer (or its mask) is gone.
class SetMaskPixelsCommand : public Command {
public:
    // `region` is w x h coverage bytes at mask-local integer origin (originX, originY).
    SetMaskPixelsCommand(LayerId id, std::vector<std::uint8_t> region, std::uint32_t w,
                         std::uint32_t h, long originX, long originY)
        : m_id(id), m_new(std::move(region)), m_w(w), m_h(h), m_x(originX), m_y(originY) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Paint Mask"); }
    [[nodiscard]] std::optional<common::Rect> dirtyRegion(const Document& doc) const override;

private:
    void blit(Document& doc, const std::vector<std::uint8_t>& bytes) const;
    LayerId m_id;
    std::vector<std::uint8_t> m_new; // w*h coverage bytes
    std::vector<std::uint8_t> m_old; // captured on first apply
    std::uint32_t m_w;
    std::uint32_t m_h;
    long m_x;
    long m_y;
    bool m_captured = false;
};

// Set (or clear, with std::nullopt) the single vec::Object a vector layer carries -- the S25
// undoable authoring/editing edit the Shape/Pen tools (S26-S28) push. Captures the old object on
// first apply; a no-op when the target id is not a VectorLayer. The whole object (geometry + paint
// + stroke) is value data, so this stores a copy each way -- cheap next to raster pixels. The
// History label is caller-supplied so tools can read "Add Rectangle", "Edit Path", etc.
class SetVectorObjectCommand : public Command {
public:
    // `coalesceId` (non-zero) collapses consecutive edits of the same layer in one gesture (a live
    // options-bar / shape-designer drag) into a single undo step, like SetTransformCommand.
    // `nextTransform` (optional) sets the layer transform in the SAME step -- the parametric resize
    // (S26-b §7.1) scales the size params AND shifts the placement to pin the opposite handle, and
    // both must undo together. Left unset (the common case), the layer transform is untouched.
    SetVectorObjectCommand(LayerId id, std::optional<vec::Object> next,
                           std::string label = "Edit Vector", std::uint64_t coalesceId = 0,
                           std::optional<common::Affine2D> nextTransform = std::nullopt)
        : m_id(id), m_new(std::move(next)), m_label(std::move(label)), m_coalesce(coalesceId),
          m_newXform(nextTransform) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    bool tryMergeWith(const Command& next) override;
    [[nodiscard]] std::string_view name() const override { return m_label; }

private:
    void set(Document& doc, const std::optional<vec::Object>& obj) const;
    LayerId m_id;
    std::optional<vec::Object> m_new;
    std::optional<vec::Object> m_old;
    bool m_captured = false;
    std::string m_label;
    std::uint64_t m_coalesce;
    std::optional<common::Affine2D> m_newXform;  // also set the layer transform (resize re-anchor)
    std::optional<common::Affine2D> m_oldXform;  // captured on first apply when m_newXform is set
};

// Replace a TextLayer's whole TextBlock (S29-b on-canvas editing). The block is small and self-
// contained (text + runs + paragraphs + frame/AA), so -- like SetVectorObjectCommand for a vector
// object -- the command stores the whole old/new block. `coalesceId` (non-zero) collapses a typing
// burst / a dragged style edit into one undo step (a fresh id per word-or-pause boundary). The
// new block is captured up front; the old one on first apply, so callers only supply the result.
class SetTextCommand : public Command {
public:
    SetTextCommand(LayerId id, text::TextBlock next, std::string label = "Edit Text",
                   std::uint64_t coalesceId = 0)
        : m_id(id), m_new(std::move(next)), m_label(std::move(label)), m_coalesce(coalesceId) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    bool tryMergeWith(const Command& next) override;
    [[nodiscard]] std::string_view name() const override { return m_label; }

private:
    LayerId m_id;
    text::TextBlock m_new;
    text::TextBlock m_old;
    bool m_captured = false;
    std::string m_label;
    std::uint64_t m_coalesce;
};

// Replace a TextureLayer's whole TextureParams (S55; docs/texture-generator.md §3.1). The params
// are a small self-contained value (generator + seed + scale + spec variant), so -- exactly like
// SetTextCommand for a block -- the command stores the whole old/new value; the pixel cache is
// NOT captured (it regenerates deterministically from the params, §8.3). `coalesceId` (non-zero)
// collapses a slider/gizmo drag into one undo step (a fresh id per gesture).
class SetTextureCommand : public Command {
public:
    SetTextureCommand(LayerId id, texture::TextureParams next, std::string label = "Edit Texture",
                      std::uint64_t coalesceId = 0)
        : m_id(id), m_new(std::move(next)), m_label(std::move(label)), m_coalesce(coalesceId) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    bool tryMergeWith(const Command& next) override;
    [[nodiscard]] std::string_view name() const override { return m_label; }

private:
    LayerId m_id;
    texture::TextureParams m_new;
    texture::TextureParams m_old;
    bool m_captured = false;
    std::string m_label;
    std::uint64_t m_coalesce;
};

// Replace an AdjustmentLayer's whole params bag (S32; docs/adjustment-layers.md). The bag is a
// small name->double map, so -- exactly like SetTextureCommand for texture params -- the command
// stores the whole old/new value and captures the old bag on first apply. `coalesceId` (non-zero)
// collapses a live editor slider drag into one undo step (a fresh id per gesture). The History
// label carries the kind ("Edit Levels", "Edit Exposure", ...) -- callers supply it.
class SetAdjustmentParamsCommand : public Command {
public:
    SetAdjustmentParamsCommand(LayerId id, std::map<std::string, double> next,
                               std::string label = "Edit Adjustment",
                               std::uint64_t coalesceId = 0)
        : m_id(id), m_new(std::move(next)), m_label(std::move(label)), m_coalesce(coalesceId) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    bool tryMergeWith(const Command& next) override;
    [[nodiscard]] std::string_view name() const override { return m_label; }

private:
    LayerId m_id;
    std::map<std::string, double> m_new;
    std::map<std::string, double> m_old;
    bool m_captured = false;
    std::string m_label;
    std::uint64_t m_coalesce;
};

// Resize the document canvas (S16 crop; later Image→Canvas Size, S53). The canvas size alone:
// layers are untouched, so pairing it with per-layer rebases (and the selection crop) inside
// one CompositeCommand is the caller's job — render::buildCropCommand does exactly that.
class ResizeCanvasCommand : public Command {
public:
    ResizeCanvasCommand(std::uint32_t w, std::uint32_t h) : m_newW(w), m_newH(h) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Resize Canvas"); }

private:
    std::uint32_t m_newW;
    std::uint32_t m_newH;
    std::uint32_t m_oldW = 0;
    std::uint32_t m_oldH = 0;
    bool m_captured = false;
};

// Replace the document's selection mask (S13). UI/tools compute the result themselves
// (Selection::combine with the gesture's SelectOp) and push the final mask; clearing is pushing
// an empty Selection. Stores the full old + new masks -- heavy for huge documents, acceptable
// until tiled storage (S60-c). A drag gesture's intermediate updates should be UI-coalesced
// (S14) rather than pushed per mouse move.
//
// `label` names the step in the History panel ("Select" for the marquee/lasso commits and the
// Select menu, "Move Selection" for the S16-i move/nudge). Consecutive commands sharing one
// non-zero `coalesceId` collapse into a single undo step, like SetOpacityCommand: an arrow-key
// nudge burst is one entry, and the id changes whenever the canvas starts a fresh nudge session
// (so a burst can never merge across an undo or an unrelated selection edit).
class SetSelectionCommand : public Command {
public:
    explicit SetSelectionCommand(Selection next, std::uint64_t coalesceId = 0,
                                 std::string label = "Select")
        : m_new(std::move(next)), m_label(std::move(label)), m_coalesce(coalesceId) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return m_label; }
    bool tryMergeWith(const Command& next) override;

private:
    Selection m_new;
    Selection m_old;
    std::string m_label;
    std::uint64_t m_coalesce;
    bool m_captured = false;
};

// ---- Guide commands (View -> Guides) --------------------------------------------------------
// Add a guide the user pulled off a ruler. The guide carries its already-minted id, so redo
// restores the exact guide and undo removes it by id.
class AddGuideCommand : public Command {
public:
    explicit AddGuideCommand(Guide guide) : m_guide(guide) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Add Guide"); }

private:
    Guide m_guide;
};

// Remove a guide (dragged back onto its ruler, or deleted). Captures the guide on first apply so
// undo restores it exactly (position + id).
class RemoveGuideCommand : public Command {
public:
    explicit RemoveGuideCommand(std::uint64_t id) : m_id(id) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Remove Guide"); }

private:
    std::uint64_t m_id;
    Guide m_captured{};
    bool m_have = false;
};

// Move a guide to a new position (a drag). Both old + new positions are known from the gesture, so
// it is a plain set/restore. Coalesces within a drag (same id + matching non-zero coalesce id) so a
// whole guide drag is one undo step.
class MoveGuideCommand : public Command {
public:
    MoveGuideCommand(std::uint64_t id, double oldPos, double newPos, std::uint64_t coalesceId = 0)
        : m_id(id), m_old(oldPos), m_new(newPos), m_coalesce(coalesceId) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Move Guide"); }
    bool tryMergeWith(const Command& next) override;

private:
    std::uint64_t m_id;
    double m_old;
    double m_new;
    std::uint64_t m_coalesce;
};

// Clear all guides (View -> Clear Guides). Captures the whole list on first apply; undo restores it.
class ClearGuidesCommand : public Command {
public:
    ClearGuidesCommand() = default;
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Clear Guides"); }

private:
    std::vector<Guide> m_captured;
    bool m_have = false;
};

// Replace the whole guide list (S53-a). ClearGuidesCommand with a value: the caller computes the
// new list and this stores it, capturing the old one on first apply exactly as Clear does. It
// exists for the whole-document operations -- Canvas Size, Image Size, the orientation ops, Crop --
// which must carry the guides with the canvas rather than strand them at their old coordinates;
// they fold it into their single CompositeCommand, so it rarely names an undo step of its own.
class SetGuidesCommand : public Command {
public:
    explicit SetGuidesCommand(std::vector<Guide> next) : m_new(std::move(next)) {}
    void apply(Document& doc) override;
    void undo(Document& doc) override;
    [[nodiscard]] std::string_view name() const override { return N_("Move Guides"); }

private:
    std::vector<Guide> m_new;
    std::vector<Guide> m_old;
    bool m_captured = false;
};

}  // namespace mosaic::core
