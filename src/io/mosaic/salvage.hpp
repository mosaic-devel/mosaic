#pragma once

#include "io/mosaic/file.hpp"
#include "io/mosaic/journal.hpp"
#include "io/mosaic/records.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// mosaic/salvage -- recovery past a gap (spec 2.8): a SEPARATE, EXPLICITLY-LABELED mode, never
// the default. Conservative replay's stop-at-first-anomaly provably over-discards (one damaged
// mid-journal frame threw away 12 intact states, Round 11 B1); the naive alternative -- apply
// whatever parses -- returns a "successful" document with silently wrong content (B2). Salvage
// reconciles the two under honesty rules:
//
//   - A state applies ONLY if its HIST and every chunk in its dirty set individually verify.
//   - Skipping a damaged state is permitted only with per-key staleness flags; when the HIST
//     itself is destroyed the dirty list is unknown and the result degrades to
//     DECLARED-IMPRECISE -- the flags are a lower bound, never fake precision (B4b).
//   - Salvage NEVER BLENDS LINEAGES (Round 13 D2, the regression this file exists to prevent):
//     frames partition into link-chains; the primary is the chain rooted at the expected seed
//     (identical to conservative replay's winner); every other chain is a foreign lineage --
//     fully recovered, separately reported, never merged. Two seed-rooted chains are a true
//     dual-writer race, surfaced as a root conflict for the user to resolve.
//
// One nuance the batteries pin down: a chain segment whose root link matches NOTHING (not the
// seed, no surviving frame) and which physically follows a damaged gap is the same writer's
// chain continuing past a destroyed frame -- it rejoins the preceding lineage WITH the gap
// recorded and flags raised (Round 11 B4). A segment whose root IS a known checksum is a
// genuine fork (a second writer) and stays separate no matter what.
namespace mosaic::io::native {

struct SalvageLineage {
    std::array<std::uint8_t, kLinkSize> root{}; // the chain's first LINK value
    bool seedRooted = false;  // root == the expected seed: the primary claim
    bool bridgedGap = false;  // continued past a destroyed-frame gap (same-writer rejoin)
    bool precise = true;      // false = declared-imprecise: `flagged` is only a lower bound
    std::vector<std::uint64_t> states;   // applied state ids, in order
    std::vector<RecoveredChunk> chunks;  // applied content + HIST frames
    std::vector<DirtyKey> flagged;       // keys whose content is stale/lost in this lineage
};

struct SalvageReport {
    std::vector<SalvageLineage> lineages; // in file order of first frame
    bool rootConflict = false;            // >1 seed-rooted lineage: a true dual-writer race
    std::size_t gaps = 0;                 // contiguous damaged/unchainable regions skipped

    // The lineage a reader may adopt (after user consent): the first seed-rooted chain --
    // by construction the same content conservative replay would have picked. nullptr when
    // even frame 0 of the chain is gone.
    [[nodiscard]] const SalvageLineage* primary() const noexcept;
};

// Salvage one explicit-link region: the file's committed append region (start =
// wal_start_offset, seed = the checkpoint root's checksum) or a journal's content frames.
// ROOT chunks inside the region are skipped (end-of-checkpoint replicas, spec 2.3).
[[nodiscard]] SalvageReport salvageLinkedRegion(std::span<const std::uint8_t> buf,
                                                std::size_t start,
                                                const std::array<std::uint8_t, kLinkSize>& seed);

// Journal salvage: the JHDR binding gates it exactly like replay (a journal bound elsewhere is
// rejected, never salvaged into the wrong document); past the header, same engine.
struct JournalSalvage {
    JournalBindingStatus binding = JournalBindingStatus::NoHeader;
    SalvageReport report;
};

[[nodiscard]] JournalSalvage salvageJournal(std::span<const std::uint8_t> buf,
                                            const JournalBinding& expected);

} // namespace mosaic::io::native
