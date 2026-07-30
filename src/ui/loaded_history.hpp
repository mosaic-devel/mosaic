#pragma once

#include "core/command.hpp"
#include "io/mosaic/save.hpp" // OpenReport

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// mosaic/ui/loaded_history -- turn a freshly opened .mosaic's save history into the pre-applied
// undo branch the History panel walks (spec 3.5, the LiveUndoModel). One command per save, each
// moving the document between adjacent saved states:
//
//   - a content-only save (raster/vector edits) becomes a per-key ui::LoadedDeltaCommand: its
//     dirty chunks paired with the value each key held in the state below, so a step patches only
//     those keys (O(changed)) and the model holds only each delta;
//   - a save that changed document STRUCTURE (a re-emitted manifest) or a MASK keeps the whole-tree
//     core::LoadedStateCommand (applyChunksToDocument patches content surfaces only).
//
// A save's frames live in the committed append region until a compaction folds them into the
// checkpoint's retained history (spec 3.3), after which report.commits is empty and the same
// states are reached through report.base.retained. Both regions can hold states at once -- a
// compacted file saved three more times since -- so nothing here may assume a single source.
//
// Two history ENCODINGS exist on disk (spec 3.9, S48 Build 2), and each HIST record says which
// spelling it uses: a journal-mode (H2) record's dirty keys resolve to their own per-(KEY,
// generation) frames; a cas-mode (H4) record carries a content-hash reference per dirty entry,
// resolved against the file's BLOB chunks and its current content alike. Resolution is
// self-describing per record -- a file may legitimately hold both (a cas checkpoint with
// journal-shaped commit-append saves after it).
//
// Lives in the UI layer, not core, because it reaches into io (the layer rule: io depends on core,
// never the reverse). The document the caller holds must already be the file's NEWEST saved state
// (documentFromReport over `report`); adopt the result with CommandStack::adoptHistory.
namespace mosaic::ui {

// One resolved dirty entry of one saved state: the logical (TYPE, KEY) the state wrote, and the
// content it wrote there. `payload` points into `report`-owned bytes (a frame's payload, or the
// content region of a BLOB's) and stays valid as long as the report does.
struct LoadedChunk {
    io::native::ChunkTag type{};
    io::native::ChunkKey key{};
    std::uint8_t flags = 0; // how to interpret payload (kFlagFiltered), never transport bits
    std::span<const std::uint8_t> payload;
};

// One saved state of an opened file: the generation it consumed and the content it wrote.
struct LoadedState {
    std::uint64_t generation = 0;
    std::vector<LoadedChunk> chunks; // this state's dirty set; never HIST, never PRVW
};

// The file's saved states, oldest first, resolved from its HIST records wherever they live.
//
//   a value  -- the states, possibly none (an unsaved checkpoint, or a full-scan open, whose
//               collapsed chunks cannot separate history from content: flow 3e already tells the
//               user the save history is gone)
//   nullopt  -- the file HAS history, and it cannot be read: some state's dirty frame did not
//               survive (or a cas reference resolves to no surviving content), so every step past
//               it would move the document to content that state never held. A silently wrong
//               undo is worse than no undo, and the two cases must be told apart because they say
//               different things to the user.
[[nodiscard]] std::optional<std::vector<LoadedState>>
loadedStates(const io::native::OpenReport& report);

// Build the pre-applied history commands, oldest save first. `label(generation)` names each step
// for the panel. Returns empty when there is no readable history, or when a whole-tree
// reconstruction fails (history is a bonus -- the caller then simply leaves the panel empty).
[[nodiscard]] std::vector<std::unique_ptr<core::Command>>
buildLoadedHistory(const io::native::OpenReport& report,
                   const std::function<std::string(std::uint64_t)>& label);

} // namespace mosaic::ui
