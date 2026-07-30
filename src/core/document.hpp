#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/guides.hpp"
#include "core/layer.hpp"
#include "core/selection.hpp"

// Document -- a canvas plus the root of the layer tree, the working color state, and the id
// allocator. It is pure data/logic (no GPU; the compositor is S7), so it is fully exercisable
// headlessly. Edits should go through the command stack (S6-b) for undo/redo; the direct
// setters and tree mutators here are what those commands (and tests) build on.
namespace mosaic::core {

class CommandStack;

// Working color space of the document (PLAN §3.6). The full lcms2-backed set arrives with the
// color-management work (S12); this is the model-level enum.
enum class ColorSpace { SRGB, LinearSRGB, DisplayP3, AdobeRGB, Rec2020 };
[[nodiscard]] std::string_view colorSpaceName(ColorSpace cs);

// Working precision per channel (PLAN §3.6); 16-bit float is the default.
enum class Precision { U8, U16, F16, F32 };
[[nodiscard]] std::string_view precisionName(Precision p);

class Document {
public:
    Document(std::uint32_t width, std::uint32_t height, ColorSpace colorSpace = ColorSpace::SRGB,
             Precision precision = Precision::F16);
    ~Document();  // out-of-line: m_commands holds an incomplete CommandStack here

    // A document owns its layer tree and undo history; it is referenced by its CommandStack and
    // is not copyable or movable (that would dangle the back-reference).
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&&) = delete;
    Document& operator=(Document&&) = delete;

    // ---- Canvas / color state ----
    [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
    void setCanvasSize(std::uint32_t w, std::uint32_t h) noexcept {
        m_width = w;
        m_height = h;
    }
    [[nodiscard]] ColorSpace colorSpace() const noexcept { return m_colorSpace; }
    void setColorSpace(ColorSpace cs) noexcept { m_colorSpace = cs; }
    [[nodiscard]] Precision precision() const noexcept { return m_precision; }
    void setPrecision(Precision p) noexcept { m_precision = p; }
    [[nodiscard]] double dpi() const noexcept { return m_dpi; }
    void setDpi(double dpi) noexcept { m_dpi = dpi; }

    // Optional per-document ICC working profile (File -> New "Custom..." colour entry): an RGB
    // .icc path plus its embedded description for display. When set it outranks colorSpace()
    // for colour management; the enum stays the fallback wherever the file is unavailable.
    // (.mosaic serialization arrives with the next manifest-schema touch -- see PLAN S48-b.)
    [[nodiscard]] const std::string& iccProfilePath() const noexcept { return m_iccProfilePath; }
    [[nodiscard]] const std::string& iccProfileName() const noexcept { return m_iccProfileName; }
    void setIccProfile(std::string path, std::string name) {
        m_iccProfilePath = std::move(path);
        m_iccProfileName = std::move(name);
    }

    [[nodiscard]] const std::string& title() const noexcept { return m_title; }
    void setTitle(std::string title) { m_title = std::move(title); }
    // The file this document was loaded from / last saved to (empty if never saved).
    [[nodiscard]] const std::string& filePath() const noexcept { return m_filePath; }
    void setFilePath(std::string path) { m_filePath = std::move(path); }

    // The document's stable identity across saves and machines (spec: docs/mosaic-native-format.md
    // 2.6 -- the recovery journal and advisory lock key on it, existing BEFORE a file path does).
    // Empty until minted (io::native::mintDocumentUuid at first save / document creation); a
    // loaded .mosaic adopts the file's. Copy-a-file keeps the uuid by design -- the journal's
    // path-hash component is what disambiguates copies.
    [[nodiscard]] const std::string& uuid() const noexcept { return m_uuid; }
    void setUuid(std::string uuid) { m_uuid = std::move(uuid); }

    // Unsaved-changes flag (the window title's "• unsaved", S18-d; the tab close / save prompt,
    // S49). Derived from the command stack's saved-position marker (no separate boolean to desync):
    // the document is dirty whenever the stack is not at its last-saved position. Out-of-line in
    // document.cpp because it reaches into CommandStack (only forward-declared here).
    [[nodiscard]] bool dirty() const noexcept;

    // ---- Layer tree ----
    [[nodiscard]] GroupLayer& root() noexcept { return *m_root; }
    [[nodiscard]] const GroupLayer& root() const noexcept { return *m_root; }

    // Allocate the next unique layer id for this document (monotonic, never reused).
    [[nodiscard]] LayerId mintLayerId() noexcept { return m_nextId++; }
    [[nodiscard]] LayerId nextLayerId() const noexcept { return m_nextId; }
    // Deserialization ONLY (.mosaic open): restore the id allocator past every persisted id.
    // Ids are monotonic and never reused -- the .mosaic format's tile keys and the undo model
    // both depend on that -- so this must never move the allocator backwards over live ids.
    void setNextLayerId(LayerId next) noexcept { m_nextId = next > m_nextId ? next : m_nextId; }

    // Find a layer anywhere in the tree by id (the root is excluded), or nullptr.
    [[nodiscard]] Layer* find(LayerId id) noexcept;
    [[nodiscard]] const Layer* find(LayerId id) const noexcept;

    // Where a layer sits: its parent group and index. nullopt for an unknown id or the root.
    struct Location {
        GroupLayer* parent = nullptr;
        std::size_t index = 0;
    };
    [[nodiscard]] std::optional<Location> locate(LayerId id) noexcept;

    // Resolve a group by id, including the root (which find() excludes). nullptr if the id is
    // unknown or names a non-group layer. Commands use this to address a parent group.
    [[nodiscard]] GroupLayer* groupById(LayerId id) noexcept;

    // Total layers in the tree, excluding the root group.
    [[nodiscard]] std::size_t layerCount() const noexcept;

    // The undo/redo stack for this document. Edits should go through it.
    [[nodiscard]] CommandStack& commands() noexcept { return *m_commands; }
    [[nodiscard]] const CommandStack& commands() const noexcept { return *m_commands; }

    // ---- Factories (mint an id; the returned layer is not yet inserted) ----
    [[nodiscard]] std::unique_ptr<GroupLayer> makeGroup(std::string name = "Group");
    [[nodiscard]] std::unique_ptr<RasterLayer> makeRaster(std::string name, std::uint32_t w,
                                                          std::uint32_t h);
    // Raster layer sized to the canvas.
    [[nodiscard]] std::unique_ptr<RasterLayer> makeRaster(std::string name = "Layer");
    [[nodiscard]] std::unique_ptr<VectorLayer> makeVector(std::string name = "Vector");
    [[nodiscard]] std::unique_ptr<TextLayer> makeText(std::string name, std::string text = {});
    [[nodiscard]] std::unique_ptr<AdjustmentLayer> makeAdjustment(std::string name,
                                                                  AdjustmentKind kind);
    [[nodiscard]] std::unique_ptr<MagicLayer> makeMagic(std::string name, common::Image source);
    // A texture-generator layer (S55); auto-name from the generator via texture::generatorName.
    [[nodiscard]] std::unique_ptr<TextureLayer> makeTexture(std::string name,
                                                            texture::TextureParams params);

    // Deep-copy a layer (recursing into groups), minting fresh ids for the copy and every
    // descendant. All shared chrome (name/visibility/opacity/blend/clip/lock/transform/mask) and
    // the kind-specific payload (raster pixels, text, adjustment kind+params, magic source) are
    // copied; the returned layer is detached (not yet inserted). Powers layer duplicate +
    // drag-to-clone (S10). Layer's own copy is deleted (it would slice + duplicate ids), so this
    // is the supported way to clone.
    [[nodiscard]] std::unique_ptr<Layer> duplicateLayer(const Layer& src);

    // The active selection (S13): empty = no selection = everything editable. Mutate ONLY via
    // SetSelectionCommand so selection changes are undoable; UI/tools compute the new mask
    // (Selection::combine) and push that command. setSelection is the raw setter commands use.
    [[nodiscard]] const Selection& selection() const noexcept { return m_selection; }
    void setSelection(Selection s) noexcept {
        m_selection = std::move(s);
        ++m_selectionRevision;
    }
    // Bumped by every setSelection: per-frame consumers (the canvas mask upload, the status
    // bar's bounds scan) use it to skip re-syncing an unchanged selection -- both are
    // document-sized walks, too heavy to repeat per composite during a drag.
    [[nodiscard]] std::uint64_t selectionRevision() const noexcept { return m_selectionRevision; }

    // ---- Guides (View -> Rulers/Guides) -------------------------------------------------------
    // Draggable reference lines stored per document, in document coordinates. Mutate through the
    // guide commands (AddGuide/RemoveGuide/MoveGuide/ClearGuides) so edits are undoable; the raw
    // setters below are what those commands (and tests) build on. `showGuides`/`lockGuides` are
    // per-document view flags (not undoable, like a View toggle). Guides are session/view state and
    // are NOT yet persisted to the native format (owed -- see docs; the manifest is the place).
    [[nodiscard]] const std::vector<Guide>& guides() const noexcept { return m_guides; }
    // Allocate the next unique guide id for this document (monotonic; never reused).
    [[nodiscard]] std::uint64_t mintGuideId() noexcept { return m_nextGuideId++; }
    // Append a guide (its id already minted). Keeps m_nextGuideId ahead of any restored id so a
    // later mint never collides with an undone-then-redone guide.
    void addGuide(const Guide& g) {
        m_guides.push_back(g);
        if (g.id >= m_nextGuideId)
            m_nextGuideId = g.id + 1;
    }
    void removeGuide(std::uint64_t id) {
        for (auto it = m_guides.begin(); it != m_guides.end(); ++it)
            if (it->id == id) {
                m_guides.erase(it);
                return;
            }
    }
    void setGuidePosition(std::uint64_t id, double position) {
        if (Guide* g = findGuide(id))
            g->position = position;
    }
    [[nodiscard]] Guide* findGuide(std::uint64_t id) noexcept {
        for (Guide& g : m_guides)
            if (g.id == id)
                return &g;
        return nullptr;
    }
    [[nodiscard]] const Guide* findGuide(std::uint64_t id) const noexcept {
        for (const Guide& g : m_guides)
            if (g.id == id)
                return &g;
        return nullptr;
    }
    // Replace the whole guide list (Clear Guides + its undo). Advances the id allocator past every
    // restored id so a subsequent mint stays unique.
    void setGuides(std::vector<Guide> guides) {
        m_guides = std::move(guides);
        for (const Guide& g : m_guides)
            if (g.id >= m_nextGuideId)
                m_nextGuideId = g.id + 1;
    }
    [[nodiscard]] bool showGuides() const noexcept { return m_showGuides; }
    void setShowGuides(bool on) noexcept { m_showGuides = on; }
    [[nodiscard]] bool lockGuides() const noexcept { return m_lockGuides; }
    void setLockGuides(bool on) noexcept { m_lockGuides = on; }

private:
    std::uint32_t m_width;
    std::uint32_t m_height;
    ColorSpace m_colorSpace;
    Precision m_precision;
    double m_dpi = 72.0;
    std::string m_iccProfilePath; // "" = the colorSpace enum governs (see iccProfilePath())
    std::string m_iccProfileName;
    std::string m_title = "Untitled";
    std::string m_filePath;
    std::string m_uuid; // stable document identity (empty until minted; see uuid())
    LayerId m_nextId = 1;
    std::unique_ptr<GroupLayer> m_root;  // the tree root (holds top-level layers)
    std::unique_ptr<CommandStack> m_commands;
    Selection m_selection; // empty = no active selection (S13)
    std::uint64_t m_selectionRevision = 0;
    std::vector<Guide> m_guides;         // per-document reference lines (View -> Guides)
    std::uint64_t m_nextGuideId = 1;     // monotonic guide id allocator (0 = invalid)
    bool m_showGuides = true;            // per-document view flag (Show Guides)
    bool m_lockGuides = false;           // per-document view flag (Lock Guides)
};

// The topmost visible layer whose content covers `docPt` -- the Move tool's click-select (S15).
// Raster/magic layers are hit where their pixels are opaque (alpha > 0, sampled through the layer's
// transform); a vector layer is hit by its geometry (vec::hitTest) so shapes are click-selectable
// and movable too. Walks top-of-stack first and descends into groups (an invisible group hides its
// whole subtree); group/text/adjustment layers are never hits themselves.
[[nodiscard]] Layer* topmostLayerAt(GroupLayer& root, common::Vec2 docPt);

// The topmost visible VectorLayer whose object's geometry is hit at `docPt` (a geometry-aware pick,
// unlike topmostLayerAt which samples raster alpha and skips vectors) -- the Shape tool's
// select-to-edit (S26-b §7.1). `pickRadiusDoc` widens the pick band around thin outlines, in
// document px. Walks top-of-stack first, descends into groups; returns null when nothing is hit.
[[nodiscard]] VectorLayer* topmostVectorLayerAt(GroupLayer& root, common::Vec2 docPt,
                                                double pickRadiusDoc = 0.0);

// The topmost visible TextLayer whose laid-out content box contains `docPt` -- the Type tool's
// select-to-edit (S29-b §6): clicking an existing text block with the Type tool re-enters editing.
// Box test against the layer's (renderer-populated) contentBounds; `padDoc` widens it (in document
// px) so a click just outside the glyphs still catches. Null when the box is unmeasured or no hit.
[[nodiscard]] TextLayer* topmostTextLayerAt(GroupLayer& root, common::Vec2 docPt, double padDoc = 0.0);

// The topmost visible VectorLayer whose flattened path OUTLINE (the spine, not the fill body)
// passes within `padDoc` document px of `docPt` -- the Type tool's text-on-path entry (S30 §9):
// clicking near a path's spine starts text riding it, where topmostVectorLayerAt's fill-aware
// hit would also catch clicks anywhere inside a filled shape.
[[nodiscard]] VectorLayer* topmostVectorSpineAt(GroupLayer& root, common::Vec2 docPt,
                                                double padDoc);

// S30 fit-to-path: re-bake `tl`'s PathFit.baked from its source layer -- the source geometry
// flattened, then mapped path-local -> document -> text-local (so both layers' transforms are
// honoured). Returns true when the baked contours changed (the caller invalidates the layer's
// content bounds / recomposites). No-op when the block has no fit, either transform is singular,
// or the source layer is missing -- the LAST baked path is kept so the text holds its shape.
bool rebakeTextPathFit(Document& doc, TextLayer& tl);

// Affinity's group click model, click-to-drill (user-confirmed 2026-06-11): which layer a Move
// click on `hit` should target, given the currently targeted layer (null = none).
//   - Default: the OUTERMOST group containing the hit (grouped content moves together); the
//     hit itself when ungrouped.
//   - Drill: when the current target IS the hit or one of its ancestor groups, go ONE level
//     deeper toward the hit (click a group again to enter it).
//   - Scope: when the hit lives inside a group the current target also lives in, select the
//     hit's node at that depth (clicking siblings inside an entered group); clicking outside
//     any shared group exits back to the outermost rule.
[[nodiscard]] Layer* moveClickTarget(Layer* hit, Layer* current);

// Where a placed image lands on a `docW` x `docH` document (S50: a canvas file-drop, File->Open as
// Layer). It is scaled DOWN uniformly to fit inside the canvas when it is bigger, never magnified
// when it is smaller, and centred either way -- the placed-image convention. The result is a layer
// transform, so a MagicLayer's source stays untouched at full resolution and the compositor keeps
// resampling from it; nothing about this placement is destructive. A degenerate source or document
// yields the identity.
[[nodiscard]] common::Affine2D placedImageTransform(std::uint32_t srcW, std::uint32_t srcH,
                                                    std::uint32_t docW, std::uint32_t docH);

}  // namespace mosaic::core
