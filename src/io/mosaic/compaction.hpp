#pragma once

#include "io/mosaic/blob.hpp"
#include "io/mosaic/save.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

// mosaic/compaction -- the history-preserving full write (spec 2.6 compaction, 3.3 copy-through,
// 3.9 adaptive H2<->H4 switching).
//
// An ordinary File->Save appends a committed batch, so the append region grows and carries no
// Reed-Solomon parity (it is written after the checkpoint's stripes were computed). Once that
// debt trips needsCompaction (save.hpp), THAT Save performs a full write instead -- never a
// background job, every byte behind an explicit Ctrl+S.
//
// The full write cannot simply re-serialize the document: that would keep the newest content and
// silently drop every retained undo state the file had accumulated. Instead the file is folded:
//
//   current content  = the newest frame per (TYPE, KEY) across checkpoint + committed region,
//                      overridden by the Save's own edit, which takes a fresh generation
//   retained history = every state those frames superseded, encoded in the mode the fold picks:
//     "journal" (H2) -- each superseded frame kept as its own (TYPE, KEY, generation) frame,
//                       plus every HIST record;
//     "cas" (H4)     -- unique content kept ONCE as a hash-keyed BLOB chunk, with each HIST
//                       record's dirty entries referencing hashes; content equal to a current
//                       frame's is not stored at all (the reference resolves to the parity-
//                       covered current frame). BLOB chunks are history: no parity (spec 3.8).
//
// Within a mode everything still on disk is copied through BYTE-VERBATIM (spec 3.3 encode-once):
// a state's content is compressed exactly once, at the Save that first committed it, so
// compaction never costs O(session length) in CPU. A frame that parity rebuilt in memory has no
// on-disk extent and is re-encoded from its verified payload instead -- which is how a parity
// repair becomes permanent rather than being copied back as damaged bytes. The ONLY whole-
// history re-encode is an H2<->H4 mode switch (spec 3.3/3.9), which the hysteresis below keeps
// rare by construction.
//
// The generation contract (spec 2.2, load-bearing for index-free recovery): only STATES consume
// generation ids, and the checkpoint's own generation is the newest retained state id. A
// compaction with a pending edit advances to tip.commitId + 1; a compaction with nothing to
// commit (a pure parity refresh) advances nothing. Reusing an id would make "highest generation
// wins" tie and silently resolve to stale content (Round 12, A5).
namespace mosaic::io::native {

// The H2<->H4 hysteresis thresholds (spec 3.9, Round 9-validated): asymmetric so churn hovering
// near one cutoff cannot thrash the format back and forth, each switch being a full re-encode.
inline constexpr double kSwitchUp = 0.35;   // journal -> cas at churn >= this
inline constexpr double kSwitchDown = 0.15; // cas -> journal at churn < this

// Pick the encoding for the next checkpoint from the file's current mode plus its measured
// whole-history churn fraction. An unknown mode string reads as "journal" -- the safe default,
// and what every Build 1 file is.
[[nodiscard]] std::string chooseHistoryMode(const std::string& currentMode, double churnFraction);

// The churn signal (spec 3.9): how much of the retained history is duplicate content, measured
// by BYTES over content hashes -- 1 - unique/total, 0 when there is no history. The window is
// deliberately the WHOLE retained history, never a recent slice: retention is unlimited, so
// "everything retained" and "the window" are the same set by construction, and a short window
// forgets that an old high-churn stretch is still in the file and still deduplicating (the
// research's wrong-window bug: 17.2MB where 12.0MB was available).
//
// The tracker is incremental so the app can keep a live signal across commit-append Saves at
// O(changed) per Save -- seed it from the open report once, then add each Save's dirty chunks.
struct ChurnTracker {
    std::uint64_t totalBytes = 0;  // every retained after-image, counted every time it occurs
    std::uint64_t uniqueBytes = 0; // each distinct content counted once

    void add(const BlobHash& hash, std::uint64_t contentBytes);
    [[nodiscard]] double fraction() const noexcept {
        return totalBytes == 0
                   ? 0.0
                   : 1.0 - static_cast<double>(uniqueBytes) / static_cast<double>(totalBytes);
    }
    // What cas mode would save over journal mode, before compression: the duplicate bytes.
    [[nodiscard]] std::uint64_t projectedSavings() const noexcept {
        return totalBytes - uniqueBytes;
    }

private:
    struct HashKey {
        std::size_t operator()(const BlobHash& h) const noexcept;
    };
    std::unordered_set<BlobHash, HashKey> seen_;
};

// Seed a tracker with every retained state's instances from an opened file (checkpoint retained
// history + committed region alike -- the whole horizon). PRVW instances never count: previews
// are derived artifacts and compaction drops their superseded copies. nullopt when some state's
// content cannot be resolved (a damaged file): no signal is better than a wrong one.
[[nodiscard]] std::optional<ChurnTracker> churnFromOpen(const OpenReport& open);

// Add one commit-append Save's dirty chunks to the running signal (the state just appended).
void addStateToChurn(ChurnTracker& tracker, std::span<const StateChunk> chunks);

struct CompactionOptions {
    // Force the fold's encoding ("journal"/"cas") instead of deciding by churn + hysteresis.
    // A test knob: production callers leave it empty.
    std::string forceMode;
    // Forwarded to buildCheckpoint (async save): reported from the fold's own thread.
    BuildProgressFn progress;
};

struct CompactionResult {
    std::vector<std::uint8_t> bytes;
    std::uint64_t generation = 0;    // the new root's generation: the newest retained state id
    std::string mode;                // the encoding this fold chose (root "mode")
    double churnFraction = 0.0;      // the measured whole-history churn that chose it
    std::size_t currentChunks = 0;   // frames describing the document as it is now
    std::size_t retainedChunks = 0;  // history folded in behind them (frames + BLOBs + HIST)
    std::size_t blobChunks = 0;      // of those, hash-keyed BLOB chunks (cas mode only)
    std::size_t reEncodedChunks = 0; // frames with no on-disk extent (parity rebuilt them)
};

// Fold `file`'s committed region back into a fresh checkpoint image, preserving its history.
//
// `open` MUST be openDocument(file) over the same bytes -- the frame extents it recorded are what
// copy-through splices, so passing a different image silently re-encodes everything (correct, but
// pointlessly slow). `newState` is the Save's dirty chunks, from diffDocumentStates; pass an
// empty span to compact without committing an edit.
//
// nullopt when the file has no verified root (there is no history to preserve and no identity to
// carry -- the caller must write a plain checkpoint from the document instead), or when some
// retained state's content cannot be resolved (fold nothing rather than fold a history that
// would not walk).
[[nodiscard]] std::optional<CompactionResult>
buildCompactedCheckpoint(std::span<const std::uint8_t> file, const OpenReport& open,
                         std::span<const StateChunk> newState, std::string* error = nullptr,
                         const CompactionOptions& options = {});

} // namespace mosaic::io::native
