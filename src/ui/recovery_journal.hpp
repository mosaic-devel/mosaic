#pragma once

#include "io/mosaic/journal.hpp"

#include <cstddef>

// Crash-restore classification (docs/askortell-dialog.md flows 1 + 2): the pure decision of which
// journal face an open triggers, split from the FLTK presentation so it is exercised headlessly
// (tests/test_recovery_journal.cpp), exactly like recovery_flow.hpp does for the damage faces.
// The journal engine (io/mosaic/journal.hpp) computes the facts; this maps them to the settled UX.
namespace mosaic::ui {

enum class JournalRecovery {
    None,    // no journal, an empty/corrupt one, or a bound one with nothing to restore
    Restore, // flow 1: the journal binds to this file's commit and holds unsaved states
    Orphan,  // flow 2: a journal exists for this document but binds to a different commit
};

struct JournalRecoveryDecision {
    JournalRecovery kind = JournalRecovery::None;
    std::size_t changeCount = 0; // flow 1: unsaved states replayed (the "%d changes" count)
    bool tornTail = false;       // flow 1: replay stopped early -- the last change was cut off
};

// Classify one journal against the file just opened. `status` is verifyJournalHeader's verdict
// over the file's true binding; `replay` is replayJournal's result under that binding.
//
//  - Ok + replayed states     -> Restore (the common crash case; tornTail rides replay.anomaly).
//  - Ok + no states           -> None (bound, but the crash beat the first autosave: nothing to
//                                offer; the caller discards it).
//  - WrongSeed / WrongBinding -> Orphan (a JHDR for this document that no longer matches the file
//                                on disk: it was saved or replaced outside Mosaic since the crash).
//  - NoHeader                 -> None (no valid JHDR: garbage; the caller discards it silently).
[[nodiscard]] inline JournalRecoveryDecision
classifyJournalRecovery(io::native::JournalBindingStatus status,
                        const io::native::JournalReplay& replay) {
    using io::native::JournalBindingStatus;
    JournalRecoveryDecision d;
    switch (status) {
    case JournalBindingStatus::Ok:
        if (!replay.states.empty()) {
            d.kind = JournalRecovery::Restore;
            d.changeCount = replay.states.size();
            d.tornTail = replay.anomaly;
        }
        return d; // else None: nothing worth restoring
    case JournalBindingStatus::WrongSeed:
    case JournalBindingStatus::WrongBinding:
        d.kind = JournalRecovery::Orphan;
        return d;
    case JournalBindingStatus::NoHeader:
        return d; // None
    }
    return d;
}

} // namespace mosaic::ui
