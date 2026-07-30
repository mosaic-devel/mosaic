#pragma once

#include "io/mosaic/file.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

// mosaic/journal -- the recovery journal (spec 2.6): incremental autosave in an APP-OWNED file
// under the OS state directory, never the user's document. Only an explicit Save touches the
// user's path (the section-0 hard rule); the journal is the crash-window insurance between
// Saves, bound to the exact commit it extends so a stale journal is structurally unreplayable.
//
// The chain is explicit-link (spec 2.2): every frame's checksum stands alone, LINK carries the
// previous frame's checksum, and the JHDR header frame's LINK is the SEED -- the bound commit's
// own checksum (the checkpoint root's BLAKE3 after a full write, the newest CMIT's xxh3 after an
// appended Save). Binding is two independent layers, tested separately (Round 11/12 A3): the
// seed fails frame 0 by the math; the JHDR policy fields reject by comparison.
namespace mosaic::io::native {

// What the journal extends: one commit of the user's file. Built from a CommitTip (save.hpp)
// or a ReadReport's root fields after a full write.
struct JournalBinding {
    std::string documentUuid;
    // The document's canonical path when it has one, empty for an untitled document. Written into
    // the JHDR as metadata (NOT part of the binding check -- the pathhash in the journal filename
    // already keys on it): it lets an app-start scan of the recovery directory tell an untitled
    // journal (restore into a new window) from a file-backed one (fires when its file is opened),
    // without opening every candidate file. Spec 2.6.
    std::string documentPath;
    std::uint64_t commitId = 0; // the bound commit's generation (newest saved state id)
    std::array<std::uint8_t, kStrongChecksumSize> checksum{}; // the commit frame's stored bytes
    std::uint8_t checksumSize = 0;

    [[nodiscard]] std::array<std::uint8_t, kLinkSize> linkSeed() const noexcept {
        std::array<std::uint8_t, kLinkSize> v{};
        for (std::size_t i = 0; i < kLinkSize; ++i)
            v[i] = checksum[i];
        return v;
    }
};

// The journal's home (spec 2.6): <state-dir>/mosaic/recovery/<uuid>-<pathhash>. The path-hash
// component gives a COPIED document (same UUID, different path) its own journal instead of a
// fight over one; an untitled document (no path yet) simply hashes the empty string -- crash
// protection exists before a path does. Linux: $XDG_STATE_HOME, else ~/.local/state.
[[nodiscard]] std::string recoveryJournalPath(const std::string& documentUuid,
                                              const std::string& canonicalDocumentPath);

// The autosave writer. Owns the journal file handle for the whole editing session and threads
// the running link IN MEMORY (spec 2.6, implementation-critical): re-deriving the chain per
// append is the research's chain-reset bug -- invisible to any single-append test, fatal to
// every write after the first. Appends buffer in the OS; sync() is the durability point, so
// cadence policy can coalesce fsyncs without losing frame granularity.
class JournalWriter {
public:
    // Creates (or truncates) the journal and writes the JHDR binding frame. Parent directories
    // are created as needed -- the recovery dir is app-owned state, not user space.
    [[nodiscard]] static std::optional<JournalWriter> create(const std::string& path,
                                                             const JournalBinding& binding,
                                                             std::string* error = nullptr);

    JournalWriter(JournalWriter&& other) noexcept;
    JournalWriter& operator=(JournalWriter&& other) noexcept;
    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;
    ~JournalWriter(); // closes; the file stays (a crash must leave it behind -- that IS the point)

    // Append one linked frame, continuing the chain. Returns false on I/O failure.
    bool append(ChunkTag type, const ChunkKey& key, std::uint64_t generation,
                std::span<const std::uint8_t> payload, Profile profile = Profile::Fast,
                std::uint8_t flags = kFlagCritical);

    // Flush to the device (fdatasync). One sync covers every append since the last.
    bool sync();

    // Truncate and rebind to a new commit (after a successful Save). ORDERING IS LOAD-BEARING
    // (spec 2.6, Round 12 A2): call only once the Save's bytes are durable, so a crash between
    // Save and reset leaves BOTH the committed batch and a replayable journal -- zero loss.
    bool reset(const JournalBinding& binding);

    // Clean close: delete the journal (saved or deliberately discarded -- either way, a clean
    // exit leaves no journal, so "close without saving" genuinely discards).
    bool discard();

    // The growth-compaction swap (spec 2.6 "Growth"): `fresh` was fully written AND synced at a
    // temporary path in this journal's directory; rename it over this journal's path, then adopt
    // its handle and running link -- the handle survives the rename (same inode), so the session
    // keeps appending without a reopen. On failure this journal is untouched and fresh's file is
    // removed; either way `fresh` is consumed.
    bool replaceWith(JournalWriter&& fresh);

    // Bytes written so far (the growth threshold's input). 0 when the writer is closed.
    [[nodiscard]] std::uint64_t sizeBytes() const noexcept;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] std::array<std::uint8_t, kLinkSize> tip() const noexcept { return tip_; }

private:
    JournalWriter() = default;
    bool writeHeader(const JournalBinding& binding);

    std::FILE* file_ = nullptr;
    std::string path_;
    std::array<std::uint8_t, kLinkSize> tip_{}; // the threaded link state
};

// Binding verdicts, ordered by which defense fired: structural (the seed math) is checked
// before policy (the JHDR fields) -- two layers, each tested on its own.
enum class JournalBindingStatus : std::uint8_t {
    Ok = 0,
    NoHeader,     // no valid JHDR frame at offset 0
    WrongSeed,    // JHDR's LINK is not the expected commit's checksum (structurally stale)
    WrongBinding, // seed matched but the JHDR policy fields disagree (uuid/commit/checksum)
};

// The JHDR frame's decoded fields, read WITHOUT a pre-known binding (the app-start scan of the
// recovery directory has only the file on disk, not the document's tip). `documentPath` empty
// marks an untitled journal (restore into a new window); a set path fires when its file is
// opened. Reconstruct a JournalBinding from these to replay a self-consistent untitled journal.
struct JournalHeaderInfo {
    std::string documentUuid;
    std::string documentPath;
    std::uint64_t commitId = 0;
    std::array<std::uint8_t, kStrongChecksumSize> checksum{};
    std::uint8_t checksumSize = 0;

    [[nodiscard]] JournalBinding toBinding() const {
        JournalBinding b;
        b.documentUuid = documentUuid;
        b.documentPath = documentPath;
        b.commitId = commitId;
        b.checksum = checksum;
        b.checksumSize = checksumSize;
        return b;
    }
};

// Parse frame 0 as a JHDR and decode its JSON. nullopt when there is no valid JHDR at offset 0
// (a truncated, garbage, or non-journal file). Does not verify a binding -- it is how the scan
// learns which document a journal belongs to in the first place.
[[nodiscard]] std::optional<JournalHeaderInfo> readJournalHeader(std::span<const std::uint8_t> buf);

// Verify frame 0 against an expected binding. On Ok, `headerEnd` receives the offset of the
// first content frame and `headerLink` the JHDR frame's own checksum -- the content chain's
// expected first link.
[[nodiscard]] JournalBindingStatus verifyJournalHeader(std::span<const std::uint8_t> buf,
                                                       const JournalBinding& expected,
                                                       std::size_t* headerEnd = nullptr,
                                                       std::array<std::uint8_t, kLinkSize>*
                                                           headerLink = nullptr);

// Conservative journal replay (spec 2.8 step 3): verify binding, then walk the chain strictly,
// applying states transactionally at their HIST frames and STOPPING at the first invalid,
// incomplete, or link-mismatched frame. A torn tail yields exactly the last complete state.
struct JournalReplay {
    JournalBindingStatus binding = JournalBindingStatus::NoHeader;
    std::vector<RecoveredChunk> chunks;  // applied states' content + HIST frames
    std::vector<std::uint64_t> states;   // applied state ids, in order
    bool anomaly = false;                // stopped before the journal's end
    std::size_t anomalyOffset = 0;
};

[[nodiscard]] JournalReplay replayJournal(std::span<const std::uint8_t> buf,
                                          const JournalBinding& expected);

} // namespace mosaic::io::native
