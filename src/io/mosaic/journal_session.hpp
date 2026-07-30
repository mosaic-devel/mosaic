#pragma once

#include "core/document.hpp"
#include "io/mosaic/docio.hpp"
#include "io/mosaic/journal.hpp"
#include "io/mosaic/save.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// mosaic/journal_session -- the APP-facing half of the recovery journal (spec 2.6): the io
// engine (journal.hpp) frames and replays; this drives it during an editing session. It owns a
// JournalWriter, remembers the last durable serialization as a baseline, and on each
// idle-triggered autosave appends the per-state DIFF (the WAL frame set) and fdatasyncs. It NEVER
// touches the user's document -- only $XDG_STATE_HOME/mosaic/recovery (the section-0 hard rule);
// a clean discard() removes the journal, a crash leaves it for the next open to find.
//
// The unit of work is save.hpp's SaveState -- the SAME state/HIST spelling the File->Save path
// uses -- so a restored journal composes with the committed region through documentFromReport
// exactly like retained history does.
namespace mosaic::io::native {

// Build a journal binding from an opened document's commit tip. The seed is the frame the tip
// binds to (the newest CMIT's xxh3 after an appended Save, the checkpoint root's BLAKE3 after a
// full write) -- precisely the chain seed journal.hpp requires. `path` rides in the JHDR as scan
// metadata (not a binding field). Use only when the tip is real (checksumSize > 0); a
// commit-less open (full-scan fallback) has nothing to bind to -- start a self-contained journal.
[[nodiscard]] JournalBinding bindingForTip(const std::string& uuid, const std::string& path,
                                           const CommitTip& tip);

// The seed a commit-less document's journal binds to: commitId 0, an all-zero 8-byte checksum.
// Self-consistent (the JHDR's own checksum seeds the content chain), so a restore rebuilds from
// the journal alone -- there is no file to compose onto. `path` empty marks it untitled for the
// app-start scan; a non-empty path (e.g. a PNG-backed doc) still keys the journal by that path.
[[nodiscard]] JournalBinding selfContainedBinding(const std::string& uuid, const std::string& path);

// The dirty StateChunks between two document serializations, for ONE journal autosave state
// (spec 2.6). A (type,key) present in both with identical bytes is skipped; one changed or newly
// present carries curr's payload/flags; a TILE present in `prev` but gone from `curr` becomes an
// explicit transparent TOMBSTONE -- composition is highest-generation-wins, so an erase needs a
// higher-generation blank tile to override the committed painted one. Vanished MFST/VECT need no
// tombstone: a structural edit re-emits the manifest, which wins wholesale and drops removed
// layers' surfaces from the read. An empty result means nothing changed since `prev`.
[[nodiscard]] std::vector<StateChunk> diffDocumentStates(const CheckpointInput& prev,
                                                         const CheckpointInput& curr);

// Growth compaction (spec 2.6 "Growth", S48 Build 2): a save-averse session's journal grows
// with every autosave, unbounded until a Save resets it. Past the size floor, once the file
// exceeds the gain factor times the session's live working set (the newest value of every key
// dirtied since the binding -- what a compacted journal would hold), the session rewrites the
// journal ATOMICALLY as one cumulative state: fresh file at a temp name, fully durable, renamed
// over. A crash at any point leaves a journal that replays to the same document; the temp name
// carries kJournalCompactSuffix so recovery scans never mistake a torn temp for a journal.
inline constexpr std::uint64_t kJournalCompactMinBytes = 8ull * 1024 * 1024;
inline constexpr double kJournalCompactGain = 2.0;
inline constexpr const char* kJournalCompactSuffix = ".compact";

class JournalSession {
public:
    // Create the journal at `path` (recoveryJournalPath(uuid, docPath)) bound to `binding`.
    // `baseline` is the content already durable elsewhere -- the opened .mosaic's serialization
    // for a file-backed doc, or nullopt for a self-contained journal whose first autosave carries
    // the whole document. `firstState` is the id the first autosaved state takes (tip.commitId + 1
    // for a file-backed doc, 1 for a self-contained one). `compactMinBytes` is the growth floor
    // (a test shrinks it; the app takes the default).
    [[nodiscard]] static std::optional<JournalSession>
    begin(const std::string& path, const JournalBinding& binding,
          std::optional<CheckpointInput> baseline, std::uint64_t firstState,
          std::string* error = nullptr, std::uint64_t compactMinBytes = kJournalCompactMinBytes);

    JournalSession(JournalSession&&) noexcept = default;
    JournalSession& operator=(JournalSession&&) noexcept = default;
    JournalSession(const JournalSession&) = delete;
    JournalSession& operator=(const JournalSession&) = delete;

    // Serialize `doc`, diff against the baseline, and -- if anything changed -- append the state
    // (content frames + HIST) and fdatasync. The baseline advances only after a durable sync. A
    // no-change tick returns true and writes nothing. On the FIRST I/O failure the session is
    // disabled (dead()): a torn autosave frame must not be followed by more, or conservative
    // replay would stop at it and lose everything after.
    bool autosave(const core::Document& doc, std::string* error = nullptr);

    // Clean close (the document was saved, replaced, or the app is exiting normally): delete the
    // journal so a clean exit leaves nothing to restore.
    void discard();

    [[nodiscard]] bool alive() const noexcept { return writer_.has_value() && !dead_; }
    [[nodiscard]] std::string path() const {
        return writer_.has_value() ? writer_->path() : std::string{};
    }

private:
    JournalSession() = default;
    bool maybeCompact(std::string* error);

    std::optional<JournalWriter> writer_;
    JournalBinding binding_; // growth compaction re-emits the same header
    CheckpointInput baseline_;
    std::uint64_t nextState_ = 1;
    std::uint64_t firstState_ = 1; // the cumulative state's id (never re-minted: it was consumed)
    std::uint64_t compactMinBytes_ = kJournalCompactMinBytes;
    std::uint64_t compactBackoffSize_ = 0; // after a failed rewrite: wait for real growth first
    // The newest autosaved value of every key dirtied since the binding -- exactly what a
    // compacted journal holds. Bounded by the session's dirty working set (a document's worth at
    // most), never by session length.
    std::map<std::pair<ChunkTag, std::array<std::uint8_t, 16>>, StateChunk> lastWritten_;
    bool dead_ = false;
};

} // namespace mosaic::io::native
