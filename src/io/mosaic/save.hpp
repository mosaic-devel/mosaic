#pragma once

#include "io/mosaic/file.hpp"
#include "io/mosaic/records.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// mosaic/save -- the two write paths of spec 2.6 and the ordinary open path of spec 2.8:
//
//   File->Save = COMMIT-APPEND: one atomic batch (each state's chunks encoded ONCE at balanced,
//   its HIST frame, a closing CMIT) appended onto the committed region, chained by explicit
//   links from the checkpoint root / the previous CMIT. Cost: O(changed since last Save).
//
//   Save As / first save / threshold compaction = the FULL WRITE: buildCheckpoint + atomic
//   rename (file.hpp), with retained history VERIFIED THEN COPIED byte-verbatim (spec 3.3,
//   makeVerbatimChunk) -- never re-encoded, so Save cost never grows with session length.
//
//   Opening = readCheckpoint + committed-region replay: batch-transactional (a batch's states
//   apply only once its CMIT validates), stopping at the first anomaly -- a torn Save opens at
//   the previous commit, never a half-saved hybrid (Round 12 A1).
//
// This layer stays document-agnostic: states carry typed chunks; what a TILE means is the
// document layer's contract.
namespace mosaic::io::native {

// One dirty chunk of one undo state, uncompressed.
struct StateChunk {
    ChunkTag type{};
    ChunkKey key{};
    std::vector<std::uint8_t> payload;
    std::uint8_t flags = kFlagCritical; // + kFlagFiltered when the payload is Paeth-filtered
};

// One undo state bound for a Save batch (and, symmetrically, for journal autosave -- the same
// HIST record spelling serves both, records.hpp). metaJson optionally carries document-layer
// HIST fields (op/params/manifest_snapshot, spec 3.2), merged under the canonical fields.
struct SaveState {
    std::uint64_t stateId = 0;
    std::vector<StateChunk> chunks;
    std::string metaJson;
};

// The state's HIST payload: {"state", "parent" (= state-1, linear undo), "dirty" from `chunks`}.
[[nodiscard]] std::string histPayloadFor(const SaveState& state);

// The writer's in-memory knowledge of where the file's committed region ends (spec 2.6,
// implementation-critical): derived by scanning exactly ONCE (openDocument), advanced in memory
// by every Save -- never re-derived per call (the chain-reset bug).
struct CommitTip {
    std::uint64_t committedEnd = 0; // end of the last valid commit: where the next batch begins
    std::uint64_t fileSize = 0;     // observed size (> committedEnd iff a dead tail exists)
    std::uint64_t lastCommitOffset = 0; // the frame the tip binds to: newest CMIT, else a ROOT
    std::uint64_t commitId = 0;         // that frame's generation (newest saved state id)
    std::array<std::uint8_t, kStrongChecksumSize> checksum{}; // its stored checksum bytes
    std::uint8_t checksumSize = 0;
    std::uint64_t device = 0, inode = 0; // file identity; 0 = not stamped (in-memory only)

    [[nodiscard]] std::array<std::uint8_t, kLinkSize> link() const noexcept {
        std::array<std::uint8_t, kLinkSize> v{};
        for (std::size_t i = 0; i < kLinkSize; ++i)
            v[i] = checksum[i];
        return v;
    }
};

// One built batch, ready to append. cmitOffset/cmitChecksum are the next tip.
struct SaveBatch {
    std::vector<std::uint8_t> bytes;
    std::uint64_t commitId = 0;
    std::size_t cmitOffset = 0; // within `bytes`
    std::array<std::uint8_t, kStrongChecksumSize> cmitChecksum{};
    std::uint8_t cmitChecksumSize = 0;
};

// Build one committed batch continuing the explicit-link chain from `link`. Encode-once: this
// is where a state's content gets its archival balanced encoding (spec 3.3) -- the journal's
// fast-tier copy was crash insurance, not the keeper. `states` must be non-empty.
[[nodiscard]] SaveBatch buildSaveBatch(const std::array<std::uint8_t, kLinkSize>& link,
                                       std::span<const SaveState> states);

// Append a batch onto an in-memory file image and advance the tip. The tail check is the
// file-level API's job; this trusts the caller's buffer.
void appendSaveBatch(std::vector<std::uint8_t>& file, CommitTip& tip,
                     std::span<const SaveState> states);

// Opening a document (spec 2.8 steps 1-2): the checkpoint ladder, then conservative
// committed-region replay from wal_start_offset. ROOT replicas inside the region are skipped
// (spec 2.3); the chain anchors at the frame linking to the root's checksum, so pre-chain
// debris is inert. This is ordinary opening, not disaster recovery -- it is how the newest
// saved content is reached.
struct OpenReport {
    ReadReport base;                      // the checkpoint (or full-scan fallback)
    std::vector<RecoveredChunk> committed; // applied batches' content + HIST frames, in order
    std::vector<std::uint64_t> commits;    // CMIT saved-state ids, in order
    bool committedAnomaly = false; // the file does not end at the last valid commit (dead tail)
    std::size_t anomalyOffset = 0; // where conservative replay stopped, when it stopped early
    bool tipValid = false;         // a commit frame exists to append after (else: full write)
    CommitTip tip;

    // Highest generation wins across checkpoint + committed content -- the current document.
    [[nodiscard]] const RecoveredChunk* find(const ChunkTag& t, const ChunkKey& k) const noexcept;
};

[[nodiscard]] OpenReport openDocument(std::span<const std::uint8_t> file);

// Stamp the on-disk identity (size sanity + device/inode) into a tip derived from openDocument
// over the same bytes -- the half of the tail check only the filesystem can provide.
[[nodiscard]] bool stampTipIdentity(const std::string& path, CommitTip& tip,
                                    std::string* error = nullptr);

enum class SaveStatus : std::uint8_t {
    Ok = 0,
    TailSizeChanged,     // st_size differs from the tip: foreign append or truncation (D4b/c)
    TailIdentityChanged, // inode/device swapped: a foreign compaction under a stale handle (D4)
    TailCommitMismatch,  // the last commit's stored checksum is not the one the tip binds to
    IoError,
};

// The O(1) pre-Save tail check (spec 2.6, Round 13): st_size + inode identity + one read of the
// last commit's STORED checksum at the known offset. Any mismatch means the file is no longer
// what this writer's in-memory state says -- the Save must refuse loudly and surface a
// conflict, never write a batch that would be dead on arrival. Check-then-append is not atomic;
// the advisory lock (spec 2.10, a later slice) exists for the true race -- this catches every
// non-racing violation.
[[nodiscard]] SaveStatus verifyTail(const std::string& path, const CommitTip& tip,
                                    std::string* error = nullptr);

// File->Save: tail check, then append one durable batch at tip.committedEnd (truncating a dead
// tail left by a torn earlier Save first -- it is unreachable by construction and would bury
// the new batch behind an anomaly), fdatasync, advance the tip. On any non-Ok status the file
// is untouched. The caller resets the journal only AFTER this returns Ok (Round 12 A2).
[[nodiscard]] SaveStatus appendSaveToFile(const std::string& path, CommitTip& tip,
                                          std::span<const SaveState> states,
                                          std::string* error = nullptr);

// The compaction trigger (spec 2.6): parity-debt driven -- the appended region carries no
// Reed-Solomon coverage until a full write folds it in. When a Save finds the region past this
// ratio of the checkpoint, THAT Save performs the full write instead. Never a background job;
// no user-facing knob.
inline constexpr double kCompactionDebtRatio = 0.5;

[[nodiscard]] bool needsCompaction(std::uint64_t fileSize, std::uint64_t walStartOffset,
                                   double ratio = kCompactionDebtRatio) noexcept;

} // namespace mosaic::io::native
