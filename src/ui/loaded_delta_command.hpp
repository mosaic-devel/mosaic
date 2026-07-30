#pragma once

#include "core/command.hpp"
#include "core/document.hpp"
#include "io/mosaic/docio.hpp" // applyChunksToDocument + StateChunk

#include <string>
#include <string_view>
#include <utility>
#include <vector>

// One step of a LOADED save history as a per-key delta (spec 3.5 -- the LiveUndoModel refinement of
// core::LoadedStateCommand's whole-tree swap). It holds the dirty CONTENT chunks of ONE saved state
// -- the AFTER bytes (this state's values) and the BEFORE bytes (the state below it) for each key
// the save touched -- and apply()/undo() patch just those keys onto the live document via
// io::native::applyChunksToDocument, instead of swapping the whole layer tree. So an undo/redo
// across loaded saves is O(changed), and the model holds only each save's delta, not N full
// document snapshots. Lives in the UI layer, not core, because it reaches into io (the layer rule:
// io depends on core, never the reverse); a save whose delta touches document STRUCTURE or a mask
// keeps the whole-tree core::LoadedStateCommand instead (loadCommittedHistory decides). Adopted
// pre-applied via CommandStack::adoptHistory: the document already sits at the newest state, so
// apply()/undo() only ever move it between adjacent saved states.
namespace mosaic::ui {

class LoadedDeltaCommand : public core::Command {
public:
    LoadedDeltaCommand(std::string name, std::vector<io::native::StateChunk> before,
                       std::vector<io::native::StateChunk> after)
        : m_name(std::move(name)), m_before(std::move(before)), m_after(std::move(after)) {}

    // apply() = step UP to this save (redo): write its own content. undo() = step DOWN to the save
    // below it: write the values those keys held there.
    void apply(core::Document& doc) override { io::native::applyChunksToDocument(doc, m_after); }
    void undo(core::Document& doc) override { io::native::applyChunksToDocument(doc, m_before); }
    [[nodiscard]] std::string_view name() const override { return m_name; }

private:
    std::string m_name;
    std::vector<io::native::StateChunk> m_before; // the state below this save (the undo target)
    std::vector<io::native::StateChunk> m_after;  // this save's own content (the redo target)
};

} // namespace mosaic::ui
