#pragma once

#include "io/mosaic/chunk.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// mosaic/file -- the checkpoint container (spec 2.3, 2.6 full write, 2.8 recovery ladder):
// preamble, the 128KB reserved root slot (with the RPTR overflow path), content chunks, the
// DIR directory chunk, and the replicated root -- once in the slot, twice at the end of the
// checkpoint region. Reading walks the recovery ladder: slot root (following RPTR when
// present) -> tail-window scan for the end replicas -> full linear scan, where the directory
// is an accelerator and the self-describing chunks are ground truth.
//
// This layer is deliberately document-agnostic: it moves typed (TYPE, KEY, generation, payload)
// chunks. What a TILE means -- dimensions, filtering, colour -- is the document layer's
// contract, arriving with the Save-path slice.
namespace mosaic::io::native {

inline constexpr std::size_t kRootSlotSize = 128 * 1024; // spec 2.3, Round 10-measured

// History-encoding mode recorded in the root (spec 2.3, 3.9): "journal" = H2, each retained
// state's after-images kept as their own (TYPE, KEY, generation) frames; "cas" = H4, unique
// content kept once as hash-keyed BLOB chunks with HIST records referencing hashes. The mode
// dispatches the ENCODER at checkpoint-build time; either mode round-trips every checkpoint --
// a wrong mode choice can only ever cost file size, never correctness (spec 3.9).
inline constexpr const char* kModeJournal = "journal";
inline constexpr const char* kModeCas = "cas";

struct FileChunk {
    ChunkTag type{};
    ChunkKey key{};
    std::uint64_t generation = 0;
    Profile profile = Profile::Balanced;
    std::uint8_t flags = kFlagCritical;
    // Reed-Solomon coverage (spec 2.7: current-content chunks). Set FALSE for retained-history
    // chunks -- history-region parity is deliberately out of the build plan (spec 3.8).
    bool parity = true;
    // Retained history (spec 3.3): a superseded content frame, or a HIST record, folded into this
    // checkpoint by compaction rather than describing the current document. The directory marks
    // these ("h": true) so a reader can tell current content from history WITHOUT parsing every
    // frame -- which is what lets a damaged history frame be reported as a lost undo state rather
    // than as damage to the document itself.
    bool history = false;
    std::vector<std::uint8_t> payload; // uncompressed; buildCheckpoint compresses per profile
    // Copy-through (spec 3.3): when non-empty, this already-framed chunk is spliced into the
    // checkpoint byte-verbatim -- history is never re-encoded at a full write -- and payload/
    // profile above are ignored. Build one only through makeVerbatimChunk, which verifies first.
    std::vector<std::uint8_t> verbatim;
};

// Verify-then-copy (spec 3.3, load-bearing): parse + checksum + decompress the frame, and only
// then wrap it for verbatim splicing. nullopt means the frame is damaged -- the caller's Save
// must route into recovery/parity instead of propagating silent corruption into a fresh file.
// `parity` and `history` are asked for rather than defaulted: a retained-history frame that
// forgets to declare itself reads back as damaged CONTENT if it ever rots, which is the wrong
// thing to tell a user. (They are never both true -- history carries no parity, spec 3.8.)
[[nodiscard]] std::optional<FileChunk> makeVerbatimChunk(std::span<const std::uint8_t> frame,
                                                         bool parity, bool history);

struct CheckpointInput {
    std::uint8_t documentType = kDocTypeRasterVector;
    std::string documentUuid;     // minted at document creation; mirrored root<->manifest
    std::uint64_t generation = 0; // the newest retained state id (only states consume ids)
    // The container version this file is stamped with, in the preamble AND in the root. Real
    // writers leave it alone; a test forges a file from the future with it.
    std::uint8_t formatVersion = kFormatVersion;
    // Which history encoding this checkpoint's retained chunks use (spec 2.3): the caller that
    // encoded them says so, and the root records it. buildCheckpoint itself is mode-agnostic --
    // both encodings arrive as FileChunks; compaction.cpp is where the encoders live.
    std::string mode = kModeJournal;
    std::vector<FileChunk> chunks;
};

// Full-write build progress (S48 Build 2 async save): called on the BUILDER'S thread with a
// fraction in [0, 1], roughly per chunk and parity stripe; never after the build returns. The
// UI relays it to the status bar from its own thread.
using BuildProgressFn = std::function<void(double)>;

// Build the complete checkpoint image in memory. The root goes into the slot when it fits (the
// common case); otherwise the slot gets a tiny RPTR pointer to the first end-of-checkpoint
// replica and readers need no new logic (the ladder below finds it either way).
[[nodiscard]] std::vector<std::uint8_t> buildCheckpoint(const CheckpointInput& in,
                                                        const BuildProgressFn& progress = {});

struct RecoveredChunk {
    ChunkTag type{};
    ChunkKey key{};
    std::uint64_t generation = 0;
    std::uint8_t flags = 0;
    std::vector<std::uint8_t> payload; // decompressed
    // Where this chunk's framed bytes live in the file image it was read from -- what compaction
    // needs to copy it through byte-verbatim (spec 3.3, makeVerbatimChunk). A frameLen of 0 means
    // there is no such extent: the frame was rebuilt in memory by parity, or it came from a
    // journal/salvage reader rather than the file image. Such a chunk must be RE-ENCODED from its
    // payload, never copied -- which is also how a compaction heals a parity-repaired file.
    std::size_t frameOffset = 0; // offset 0 is the preamble, never a frame: a safe sentinel
    std::size_t frameLen = 0;
};

struct ReadReport {
    bool rootFound = false;   // some replica (or RPTR target) verified
    bool usedFullScan = false;
    // This file was written by a NEWER container version than this build understands, and the
    // reader has declined to interpret it. Distinct from damage in every direction: nothing here
    // is broken, and guessing at bytes whose framing may have changed is how a reader ends up
    // confidently wrong. Callers must check this before trusting `chunks` (documentFromReport does).
    bool unsupportedVersion = false;
    std::uint8_t formatVersion = kFormatVersion; // as claimed by the root, else by the preamble
    std::uint8_t documentType = kDocTypeRasterVector;
    std::string documentUuid;
    std::uint64_t generation = 0;
    std::uint64_t walStartOffset = 0; // where the committed append region begins (spec 2.3)
    // The root's history-encoding mode (spec 2.3, 3.9). What the next fold's hysteresis starts
    // from; reference RESOLUTION stays self-describing (a HIST record either carries refs or it
    // does not), so a damaged or absent mode string can never flip content interpretation.
    std::string mode = kModeJournal;
    // The winning root's stored BLAKE3 (replicas are byte-identical, so any that verified).
    // This is the committed-region chain's seed and the journal's binding target (spec 2.6).
    std::array<std::uint8_t, kStrongChecksumSize> rootChecksum{};
    std::uint8_t rootChecksumSize = 0;
    std::string rsParamsJson;
    std::size_t rsReconstructed = 0; // lost entries brought back by parity, then re-verified
    std::size_t lostEntries = 0;     // CURRENT-CONTENT entries lost beyond what parity could carry
    // Retained-history frames lost. Counted apart from lostEntries because they are two different
    // things to tell a user about: history frames carry no parity by design (spec 3.8), so one
    // rotted undo state must not raise the "this file is damaged" face over a document whose
    // content is byte-perfect.
    std::size_t lostHistoryEntries = 0;
    std::vector<RecoveredChunk> chunks;   // current content: unique per (TYPE, KEY), highest gen
    // Retained history (spec 3.3), present only once a compaction has folded it in: every
    // superseded content frame plus every HIST record. Empty on a full-scan open -- that path
    // collapses by highest generation and cannot separate history from content, which is exactly
    // what flow 3e already tells the user ("the file's index and its save history are gone").
    std::vector<RecoveredChunk> retained;

    [[nodiscard]] const RecoveredChunk* find(const ChunkTag& t, const ChunkKey& k) const noexcept;
};

// The recovery ladder. Never throws on hostile input; returns whatever survives.
[[nodiscard]] ReadReport readCheckpoint(std::span<const std::uint8_t> file);

// The atomic full write (spec 2.6): temp file in the target's directory, write, fsync, rename
// over the target, then parent-directory fsync (POSIX permits losing the rename itself on power
// failure without it; best-effort where the filesystem cannot). A failure never leaves a
// half-written file at `path` -- the previous contents survive untouched.
[[nodiscard]] bool writeFileAtomic(const std::string& path, std::span<const std::uint8_t> bytes,
                                   std::string* error = nullptr);

} // namespace mosaic::io::native
