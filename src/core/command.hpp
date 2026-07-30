#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "common/geometry.hpp"

// Undo/redo (PLAN §3.7). Every edit to a Document is a Command with apply()/undo(); the
// CommandStack records them so any change can be reversed and re-applied. Continuous gestures
// (a drag, an opacity scrub) coalesce into a single undo step. The same command objects are the
// interface the headless op-runner (§3.15) drives, so every edit is scriptable and testable.
namespace mosaic::core {

class Document;

using LayerId = std::uint64_t;  // core/layer.hpp's own alias; repeated so this header stays leaf

// WHERE a command's pixels changed, in the LAYER'S OWN pixel space (S60-a item 13).
//
// `dirtyRegion` above answers "which part of the CANVAS must be re-composited" and is therefore in
// document space, mapped through the layer's placement. The device-resident compositor needs the
// other space: `render::TileCompositor::markLayerDirty(layer, rect)` copies out of
// `RasterLayer::image()`, so its rect is indexed the way that image is. The two are NOT
// interconvertible without loss -- a rotated layer's document AABB inverse-maps to a bigger box
// than the edit -- so the claim is reported from where the edit actually happened rather than
// derived from the canvas rect.
//
// ⚠ `rect` EMPTY means "the whole layer", which is the always-correct claim. `layer ==
// kInvalidLayerId` means "this command is not a single-layer pixel edit" and the caller must fall
// back to whatever it does for a structural change.
struct LayerPixelEdit {
    LayerId layer = 0;   // kInvalidLayerId
    common::Rect rect;   // layer-local px; empty == the whole image
};

// An undoable edit. The command stores whatever it needs to do and reverse itself; apply() and
// undo() must be exact inverses, and apply() must be idempotent across an undo/redo cycle (it
// is called again on redo).
class Command {
public:
    virtual ~Command() = default;

    virtual void apply(Document& doc) = 0;
    virtual void undo(Document& doc) = 0;

    // Short label for the undo-history UI, e.g. "Set Opacity".
    [[nodiscard]] virtual std::string_view name() const = 0;

    // Coalescing hook. `next` has already been apply()'d to the document. Return true if this
    // command absorbed it (so the stack discards `next` and the pair is one undo step). Default:
    // commands do not coalesce.
    virtual bool tryMergeWith(const Command& next) {
        (void)next;
        return false;
    }

    // Document-space bounding box of the pixels this command changes, used to scope the canvas
    // recomposite after an undo/redo instead of re-compositing the whole document (S60-a). The
    // default -- nullopt -- means "unknown / whole document": the safe fallback that recomposites
    // everything, used by structural edits, transforms, opacity, canvas resize, etc. Only
    // pixel-region edits (a brush stroke, an inpaint fill) override it with their tight rect.
    [[nodiscard]] virtual std::optional<common::Rect> dirtyRegion(const Document& doc) const {
        (void)doc;
        return std::nullopt;
    }

    // The same edit in the LAYER's own pixel space, for the device-resident compositor's
    // incremental upload (S60-a item 13; see LayerPixelEdit above). The default -- nullopt -- means
    // "not a single-layer pixel edit", which is right for every structural, transform, opacity and
    // selection command; those change no layer's pixels at all, and the compositor's plan diff sees
    // them without being told. Overridden only by the commands that patch a raster layer's image.
    //
    // ⚠ This is a PERFORMANCE claim, never a correctness one: `TileCompositor` notices a layer
    // whose `contentRevision` moved with no rect attached and re-sends it whole. A missing override
    // costs a transfer. A WRONG rect costs pixels -- so every override must report the rect it
    // actually stored, not one mapped back from document space.
    [[nodiscard]] virtual std::optional<LayerPixelEdit> dirtyLayerPixels(const Document& doc) const {
        (void)doc;
        return std::nullopt;
    }

    // When this command was recorded (wall clock), for the History panel's relative-time column.
    // Set by CommandStack::push; an entry keeps its time across undo/redo (it does not "happen
    // again" on redo from the user's point of view).
    using Clock = std::chrono::system_clock;
    [[nodiscard]] Clock::time_point timestamp() const noexcept { return m_timestamp; }
    void setTimestamp(Clock::time_point t) noexcept { m_timestamp = t; }

private:
    Clock::time_point m_timestamp{};
};

// A per-document undo/redo stack. push() applies and records a command (clearing the redo
// branch); undo()/redo() walk the history. Owned by the Document.
class CommandStack {
public:
    explicit CommandStack(Document& doc) : m_doc(doc) {}

    // Apply `cmd`, mark the document dirty, and record it. If the current top can coalesce with
    // `cmd` (tryMergeWith), they merge into one undo step instead of growing the stack.
    void push(std::unique_ptr<Command> cmd);

    void undo();
    void redo();
    void clear() noexcept;

    [[nodiscard]] bool canUndo() const noexcept { return !m_undo.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !m_redo.empty(); }
    [[nodiscard]] std::size_t undoCount() const noexcept { return m_undo.size(); }
    [[nodiscard]] std::size_t redoCount() const noexcept { return m_redo.size(); }

    // ---- Saved-position marker (S18-d, GIMP-style dirty tracking) ----
    // The document is "clean" exactly when the stack sits at the position it was last saved from --
    // no separate dirty boolean to desync. markSaved() moves the marker to the current position (a
    // real .mosaic Save; the marker starts at 0, so a fresh/opened document is clean); undoing back
    // to the marker goes clean again, and a new edit past it goes dirty. When a push destroys the
    // branch the marker lived on (it was in the redo tail), the marker becomes unreachable and the
    // document can never return to clean by undo — the honest GIMP/Photoshop behaviour.
    void markSaved() noexcept { m_savedPosition = position(); }
    // Force the document permanently dirty: no reachable saved position (S48 crash restore -- a
    // document rebuilt from the recovery journal holds unsaved work that was never written to disk,
    // so it must read dirty and cannot return to clean by undo, only by a real Save).
    void markNeverSaved() noexcept { m_savedPosition = kNoSavedPosition; }
    [[nodiscard]] bool isSaved() const noexcept {
        return m_savedPosition != kNoSavedPosition && m_savedPosition == position();
    }

    // Load an already-applied history into the undo branch WITHOUT re-applying it (opening a saved
    // .mosaic: the document already reflects its newest state, and these commands reconstruct the
    // earlier ones on undo). Replaces any existing history and marks the loaded top as the saved
    // position, so a freshly opened file is clean at its newest state. Each command's apply()/undo()
    // must be pure inverses that move the document between adjacent saved states (LoadedStateCommand
    // swaps the whole layer tree). Fires the change observer once so the History panel fills in.
    void adoptHistory(std::vector<std::unique_ptr<Command>> applied) {
        m_undo = std::move(applied);
        m_redo.clear();
        m_savedPosition = position();
        notifyChanged();
    }

    // Label of the next undo/redo target, or "" when there is none (for menu items).
    [[nodiscard]] std::string_view undoName() const;
    [[nodiscard]] std::string_view redoName() const;

    // ---- History view (S16-b) ----
    // The stack as one CHRONOLOGICAL list, oldest first: entries [0, position()) are applied
    // (the undo branch) and [position(), size()) are the not-redone tail the panel mutes.
    [[nodiscard]] std::size_t size() const noexcept { return m_undo.size() + m_redo.size(); }
    [[nodiscard]] std::size_t position() const noexcept { return m_undo.size(); }
    // Label of chronological entry `i`, or "" out of range.
    [[nodiscard]] std::string_view nameAt(std::size_t i) const;
    // Wall-clock time chronological entry `i` was recorded (epoch default if out of range) — the
    // History panel's relative-time column.
    [[nodiscard]] Command::Clock::time_point timeAt(std::size_t i) const;
    // Walk to `position` (clamped to [0, size()]) through plain undo()/redo() — the History
    // panel's click-to-jump. Observers are notified ONCE at the end, not per step.
    void jumpTo(std::size_t position);

    // Document-space region affected by the most recent undo()/redo()/jumpTo() (S60-a): the union
    // of the stepped commands' dirtyRegion()s, or nullopt if any of them was whole-document (so the
    // caller recomposites everything). Lets undo/redo of a brush stroke patch just the stroke's
    // bounding box instead of the whole canvas. Undefined before the first undo/redo.
    [[nodiscard]] std::optional<common::Rect> lastAffectedRegion() const { return m_lastRegion; }

    // The LAYER-LOCAL pixel claims of the same step (S60-a item 13), one per stepped command that
    // made one -- a jumpTo across several strokes reports several. Empty means nothing stepped
    // touched a raster layer's pixels in a way it could name, which the resident compositor handles
    // by re-sending whatever its own revision diff says moved. Undefined before the first
    // undo/redo, exactly like lastAffectedRegion().
    [[nodiscard]] const std::vector<LayerPixelEdit>& lastAffectedLayerEdits() const {
        return m_lastLayerEdits;
    }

    // Observer fired after the stack's history changes (push/undo/redo/clear/jumpTo) — the
    // History panel refreshes through this, because edits push from many places (menu, layer
    // panel, canvas tools) and no single UI choke point sees them all. A push absorbed by
    // coalescing does NOT fire (no entry changed; gesture pushes arrive per input event).
    void setOnChange(std::function<void()> cb) { m_onChange = std::move(cb); }

private:
    void notifyChanged() const {
        if (m_onChange && !m_suppressNotify)
            m_onChange();
    }

    // Sentinel: the saved position is unreachable (the branch it lived on was destroyed by a push).
    static constexpr std::size_t kNoSavedPosition = static_cast<std::size_t>(-1);

    Document& m_doc;
    std::vector<std::unique_ptr<Command>> m_undo;
    std::vector<std::unique_ptr<Command>> m_redo;
    std::function<void()> m_onChange;
    bool m_suppressNotify = false; // jumpTo() batches its steps into one notification
    std::optional<common::Rect> m_lastRegion; // dirty region of the last undo/redo (S60-a)
    std::vector<LayerPixelEdit> m_lastLayerEdits; // ... and its layer-local twin (S60-a item 13)
    std::size_t m_savedPosition = 0; // command-stack position last saved from (S18-d); 0 = fresh doc
};

}  // namespace mosaic::core
