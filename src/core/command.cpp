#include "core/command.hpp"

#include <algorithm>

#include "core/document.hpp"

namespace mosaic::core {

namespace {

// Bounding box of the union of two rects.
[[nodiscard]] common::Rect boundingUnion(const common::Rect& a, const common::Rect& b) {
    const double x0 = std::min(a.x, b.x);
    const double y0 = std::min(a.y, b.y);
    const double x1 = std::max(a.right(), b.right());
    const double y1 = std::max(a.bottom(), b.bottom());
    return {x0, y0, x1 - x0, y1 - y0};
}

// Fold one command's region into the running union for jumpTo(). A whole-document (nullopt) step
// latches `full`, forcing a full recomposite for the whole jump.
void foldRegion(std::optional<common::Rect>& acc, bool& full, const std::optional<common::Rect>& r) {
    if (full)
        return;
    if (!r) {
        full = true;
        acc.reset();
        return;
    }
    acc = acc ? boundingUnion(*acc, *r) : *r;
}

// The layer-local twin of foldRegion (S60-a item 13). No union is taken: two claims on two layers
// are two claims, and two claims on ONE layer are still cheaper sent separately than as the box
// that bounds them (render::TileCompositor coalesces them into copy runs itself).
void foldLayerEdit(std::vector<LayerPixelEdit>& acc, const std::optional<LayerPixelEdit>& e) {
    if (e)
        acc.push_back(*e);
}

} // namespace

void CommandStack::push(std::unique_ptr<Command> cmd) {
    cmd->apply(m_doc);
    cmd->setTimestamp(Command::Clock::now());
    const bool hadRedo = !m_redo.empty();
    // Clearing the redo branch destroys the history the saved marker lived on if the marker was in
    // that tail (position() < m_savedPosition): it can never be reached again by undo, so the
    // document stays dirty for good (S18-d). position() == m_undo.size() here (redo not yet cleared,
    // undo not yet grown).
    if (m_savedPosition != kNoSavedPosition && m_savedPosition > m_undo.size())
        m_savedPosition = kNoSavedPosition;
    m_redo.clear();  // a new edit invalidates the redo branch

    // Coalesce continuous gestures: if the top step can absorb this one, keep the top (its undo
    // still reverts to the gesture's original state; the doc already holds the latest value).
    if (!m_undo.empty() && m_undo.back()->tryMergeWith(*cmd)) {
        m_undo.back()->setTimestamp(cmd->timestamp()); // a long gesture shows its latest time
        if (hadRedo)
            notifyChanged(); // the absorb changed no entry, but the dropped tail did
        return;
    }
    m_undo.push_back(std::move(cmd));
    notifyChanged();
}

void CommandStack::undo() {
    if (m_undo.empty()) return;
    std::unique_ptr<Command> cmd = std::move(m_undo.back());
    m_undo.pop_back();
    m_lastRegion = cmd->dirtyRegion(m_doc); // read while the layer is still in its applied state
    m_lastLayerEdits.clear();
    foldLayerEdit(m_lastLayerEdits, cmd->dirtyLayerPixels(m_doc));
    cmd->undo(m_doc);
    m_redo.push_back(std::move(cmd));
    notifyChanged();
}

void CommandStack::redo() {
    if (m_redo.empty()) return;
    std::unique_ptr<Command> cmd = std::move(m_redo.back());
    m_redo.pop_back();
    m_lastRegion = cmd->dirtyRegion(m_doc);
    m_lastLayerEdits.clear();
    foldLayerEdit(m_lastLayerEdits, cmd->dirtyLayerPixels(m_doc));
    cmd->apply(m_doc);
    m_undo.push_back(std::move(cmd));
    notifyChanged();
}

void CommandStack::clear() noexcept {
    m_undo.clear();
    m_redo.clear();
    m_savedPosition = 0; // an empty history is the clean baseline again
    notifyChanged();
}

std::string_view CommandStack::nameAt(std::size_t i) const {
    if (i < m_undo.size())
        return m_undo[i]->name();
    const std::size_t r = i - m_undo.size();
    // The redo vector is a stack: its BACK is the chronologically next entry.
    if (r < m_redo.size())
        return m_redo[m_redo.size() - 1 - r]->name();
    return {};
}

Command::Clock::time_point CommandStack::timeAt(std::size_t i) const {
    if (i < m_undo.size())
        return m_undo[i]->timestamp();
    const std::size_t r = i - m_undo.size();
    if (r < m_redo.size())
        return m_redo[m_redo.size() - 1 - r]->timestamp(); // redo vector: back is chronological next
    return {};
}

void CommandStack::jumpTo(std::size_t position) {
    const std::size_t target = position < size() ? position : size();
    if (target == this->position())
        return; // nothing to walk (and no notification)
    m_suppressNotify = true;
    // Accumulate the union of every stepped command's region; undo()/redo() each overwrite
    // m_lastRegion per step, so set the jump's combined region last (it wins).
    std::optional<common::Rect> acc;
    std::vector<LayerPixelEdit> accEdits;
    bool full = false;
    while (this->position() > target) {
        foldRegion(acc, full, m_undo.back()->dirtyRegion(m_doc));
        foldLayerEdit(accEdits, m_undo.back()->dirtyLayerPixels(m_doc));
        undo();
    }
    while (this->position() < target) {
        foldRegion(acc, full, m_redo.back()->dirtyRegion(m_doc));
        foldLayerEdit(accEdits, m_redo.back()->dirtyLayerPixels(m_doc));
        redo();
    }
    m_suppressNotify = false;
    m_lastRegion = full ? std::nullopt : acc;
    m_lastLayerEdits = std::move(accEdits); // undo()/redo() each overwrote it per step
    notifyChanged();
}

std::string_view CommandStack::undoName() const {
    return m_undo.empty() ? std::string_view{} : m_undo.back()->name();
}

std::string_view CommandStack::redoName() const {
    return m_redo.empty() ? std::string_view{} : m_redo.back()->name();
}

}  // namespace mosaic::core
