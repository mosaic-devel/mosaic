#include "io/mosaic/save.hpp"

#include "common/fs_path.hpp" // pathFromUtf8: the ONE way a UTF-8 path reaches a -W entry point

#include <algorithm>
#include <cassert>
#include <map>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
// NOMINMAX comes from the toolchain file (-DNOMINMAX), and re-#defining it with an empty body is a
// macro redefinition GCC diagnoses -- fatal under -Werror. The guard keeps std::min/std::max
// working for any Windows build that does not go through cmake/toolchains/mingw-w64.cmake.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mosaic::io::native {
namespace {

[[nodiscard]] std::span<const std::uint8_t> stringBytes(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

void bindTipTo(CommitTip& tip, std::size_t frameOffset, std::size_t frameEnd,
               std::uint64_t generation,
               const std::array<std::uint8_t, kStrongChecksumSize>& checksum,
               std::uint8_t checksumSize) {
    tip.lastCommitOffset = frameOffset;
    tip.committedEnd = frameEnd;
    tip.commitId = generation;
    tip.checksum = checksum;
    tip.checksumSize = checksumSize;
}

// The three on-disk facts the tail check compares against the in-memory tip. Same struct shape on
// both platforms so the callers below are one implementation with two I/O backends.
struct FileIdentity {
    std::uint64_t size = 0, device = 0, inode = 0;
};

#ifndef _WIN32

[[nodiscard]] std::optional<FileIdentity> identityOf(int fd) {
    struct stat st{};
    if (::fstat(fd, &st) != 0)
        return std::nullopt;
    return FileIdentity{static_cast<std::uint64_t>(st.st_size),
                        static_cast<std::uint64_t>(st.st_dev),
                        static_cast<std::uint64_t>(st.st_ino)};
}

#else

// The Win32 answer to fstat's (st_size, st_dev, st_ino): the volume serial number plus the 64-bit
// file index. WHAT THAT ACTUALLY GUARANTEES, stated plainly, because the safety argument rests on
// it and it is WEAKER than the POSIX pair it stands in for:
//
//   * On NTFS the (serial, index) pair identifies a file's directory entry and is stable for its
//     lifetime -- so the case this check exists for (another instance's compaction replaced the
//     file with byte-identical content, §2.6 D4) is caught exactly as on POSIX.
//   * Win32 promises uniqueness only WHILE A HANDLE IS OPEN. An index CAN be reused after the file
//     it named is deleted, so delete-then-recreate can in principle land on the old index.
//   * ReFS answers with a 128-bit id that this 64-bit pair truncates, and SMB/network redirectors
//     may synthesise the index or return zeros outright.
//
// Which is why identity is the SECOND of two agreeing facts, never the only one: verifyTail also
// does a positioned read of the last commit's STORED checksum, and that read is the load-bearing
// member. A filesystem that returns zeros for all three identity fields degrades gracefully rather
// than lying -- tip.device/tip.inode both stay 0, and verifyTail's `(device != 0 || inode != 0)`
// guard (the same one that skips an unstamped tip) simply drops the identity comparison and leaves
// size + checksum doing the work.
[[nodiscard]] std::optional<FileIdentity> identityOf(HANDLE h) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (::GetFileInformationByHandle(h, &info) == 0)
        return std::nullopt;
    return FileIdentity{
        (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
        info.dwVolumeSerialNumber,
        (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow};
}

// Open the document for metadata + positioned reads. FILE_SHARE_READ | FILE_SHARE_WRITE because
// this is a read of someone else's file and must exclude nobody -- and because the §0 rule says
// Mosaic holds no claim on a user's document beyond the operation that needs it.
[[nodiscard]] HANDLE openForTailCheck(const std::string& path) {
    return ::CreateFileW(common::pathFromUtf8(path).c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
}

// A positioned read -- the pread(2) shape Win32 has no direct name for. The offset rides in the
// OVERLAPPED structure, which is the documented way to read at an offset on a SYNCHRONOUS handle
// (ReadFile then completes before it returns). SetFilePointerEx + ReadFile would also work, but
// this form is positioned BY CONSTRUCTION rather than by a preceding call -- which is the property
// the POSIX branch gets from pread, and the reason neither branch can be broken by inserting an
// unrelated read between the two below. Returns bytes read, or -1.
[[nodiscard]] std::int64_t readAtOffset(HANDLE h, void* dst, std::size_t count,
                                        std::uint64_t offset) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    if (::ReadFile(h, dst, static_cast<DWORD>(count), &got, &ov) == 0) {
        // An OVERLAPPED read that starts at or past EOF reports ERROR_HANDLE_EOF instead of
        // returning zero bytes. Both mean "there are not that many bytes there", which is what
        // every caller's short-read comparison already treats as a missing frame.
        return ::GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    }
    return static_cast<std::int64_t>(got);
}

#endif

} // namespace

std::string histPayloadFor(const SaveState& state) {
    HistRecord rec;
    rec.state = state.stateId;
    rec.parent = state.stateId == 0 ? 0 : state.stateId - 1; // linear undo (spec 3.2)
    for (const StateChunk& c : state.chunks)
        rec.dirty.push_back({c.type, c.key});
    return histRecordJson(rec, state.metaJson);
}

SaveBatch buildSaveBatch(const std::array<std::uint8_t, kLinkSize>& link,
                         std::span<const SaveState> states) {
    assert(!states.empty() && "a Save with nothing to commit is the caller's bug");
    SaveBatch batch;
    std::array<std::uint8_t, kLinkSize> running = link;
    const auto emit = [&](ChunkTag type, const ChunkKey& key, std::uint64_t generation,
                          std::span<const std::uint8_t> payload,
                          std::uint8_t flags) -> AppendedChunk {
        const AppendedChunk a = appendChunk(batch.bytes, type, key, generation, payload,
                                            Profile::Balanced, flags, &running);
        running = a.linkValue();
        return a;
    };

    CmitRecord cmit;
    for (const SaveState& s : states) {
        for (const StateChunk& c : s.chunks)
            emit(c.type, c.key, s.stateId, c.payload, c.flags);
        const std::string hist = histPayloadFor(s);
        emit(kTypeHist, histKey(s.stateId), s.stateId, stringBytes(hist), kFlagCritical);
        cmit.batchStates.push_back(s.stateId);
    }
    cmit.savedState = states.back().stateId;
    const std::string cmitJson = cmitRecordJson(cmit);
    const AppendedChunk closing = emit(kTypeCommit, zeroKey(), cmit.savedState,
                                       stringBytes(cmitJson), kFlagCritical);
    batch.commitId = cmit.savedState;
    batch.cmitOffset = closing.offset;
    batch.cmitChecksum = closing.checksum;
    batch.cmitChecksumSize = closing.checksumSize;
    return batch;
}

void appendSaveBatch(std::vector<std::uint8_t>& file, CommitTip& tip,
                     std::span<const SaveState> states) {
    const SaveBatch batch = buildSaveBatch(tip.link(), states);
    const std::size_t base = file.size();
    file.insert(file.end(), batch.bytes.begin(), batch.bytes.end());
    bindTipTo(tip, base + batch.cmitOffset, file.size(), batch.commitId, batch.cmitChecksum,
              batch.cmitChecksumSize);
    tip.fileSize = file.size();
}

const RecoveredChunk* OpenReport::find(const ChunkTag& t, const ChunkKey& k) const noexcept {
    const RecoveredChunk* best = nullptr;
    const auto offer = [&](const RecoveredChunk& c) {
        if (c.type == t && c.key == k && (best == nullptr || c.generation >= best->generation))
            best = &c;
    };
    for (const RecoveredChunk& c : base.chunks)
        offer(c);
    for (const RecoveredChunk& c : committed)
        offer(c);
    return best;
}

OpenReport openDocument(std::span<const std::uint8_t> file) {
    OpenReport out;
    out.base = readCheckpoint(file);
    out.tip.fileSize = file.size();
    if (out.base.unsupportedVersion) {
        // A newer container. Its committed region may not be a committed region at all -- replaying
        // frames whose framing we do not understand is how a reader ends up confidently wrong.
        out.tip.committedEnd = file.size();
        return out;
    }
    if (!out.base.rootFound || out.base.walStartOffset >= file.size()) {
        // No verified root: the full scan already collapsed every self-describing chunk --
        // including committed batches' content (Round 12 A5) -- and there is nothing to bind an
        // append to. Saving such a document means a full write.
        out.tip.committedEnd = file.size();
        return out;
    }

    // Before any batch, the tip binds to the checkpoint root itself. Prefer a verified frame in
    // the tail region (it is also where the append region physically begins); if both end
    // replicas are damaged but the slot answered, bind to the slot.
    std::array<std::uint8_t, kLinkSize> seed{};
    for (std::size_t i = 0; i < kLinkSize; ++i)
        seed[i] = out.base.rootChecksum[i];
    out.tip.committedEnd = out.base.walStartOffset;
    if (const auto slot = parseChunkAt(file, kPreambleSize);
        slot.has_value() && slot->valid && slot->type == kTypeRoot) {
        bindTipTo(out.tip, kPreambleSize, out.base.walStartOffset, out.base.generation,
                  slot->checksum, slot->checksumSize);
        out.tipValid = true;
    }

    // Conservative committed-region replay (spec 2.8 step 2). Batch state buffers here and
    // flushes only at a validating CMIT -- Save atomicity (Round 12 A1).
    std::array<std::uint8_t, kLinkSize> expectedLink = seed;
    bool started = false;
    bool stopped = false;
    std::map<std::pair<ChunkTag, std::array<std::uint8_t, 16>>, RecoveredChunk> pending;
    std::vector<RecoveredChunk> batchChunks;
    std::vector<std::uint64_t> batchStates;
    const auto stop = [&](std::size_t at) {
        out.committedAnomaly = true;
        out.anomalyOffset = at;
        stopped = true;
    };

    for (const ChunkRecord& rec : scanChunks(file, out.base.walStartOffset)) {
        if (stopped)
            break;
        if (!started) {
            if (rec.valid && rec.type == kTypeRoot) {
                // An end-of-checkpoint replica (spec 2.3): the tip's binding until a CMIT lands.
                bindTipTo(out.tip, rec.offset, rec.offset + rec.consumed, rec.generation,
                          rec.checksum, rec.checksumSize);
                out.tipValid = true;
                continue;
            }
            if (rec.valid && rec.linked() && rec.link == seed)
                started = true; // the chain's anchor; fall through and process this frame
            else
                continue; // pre-chain debris: unreachable by any replay, harmless to skip
        }
        if (!rec.valid || !rec.complete) {
            stop(rec.offset);
            break;
        }
        if (rec.type == kTypeRoot || !rec.linked() || rec.link != expectedLink) {
            stop(rec.offset);
            break;
        }
        auto payload = decodeChunkPayload(rec, file);
        if (!payload.has_value()) {
            stop(rec.offset);
            break;
        }
        expectedLink = rec.linkValue();

        if (rec.type == kTypeHist) {
            const auto hist = parseHistRecord(*payload);
            if (!hist.has_value()) {
                stop(rec.offset);
                break;
            }
            bool complete = true;
            for (const DirtyKey& d : hist->dirty) {
                const auto it = pending.find({d.type, d.key.bytes});
                complete = complete && it != pending.end() &&
                           it->second.generation == hist->state;
            }
            if (!complete) {
                stop(rec.offset);
                break;
            }
            for (const DirtyKey& d : hist->dirty) {
                const auto it = pending.find({d.type, d.key.bytes});
                batchChunks.push_back(std::move(it->second));
                pending.erase(it);
            }
            batchChunks.push_back(RecoveredChunk{rec.type, rec.key, rec.generation, rec.flags,
                                                 std::move(*payload), rec.offset, rec.consumed});
            batchStates.push_back(hist->state);
        } else if (rec.type == kTypeCommit) {
            const auto cmit = parseCmitRecord(*payload);
            if (!cmit.has_value() || cmit->batchStates != batchStates || !pending.empty()) {
                stop(rec.offset); // a CMIT that disagrees with its own batch: not a commit
                break;
            }
            for (RecoveredChunk& c : batchChunks)
                out.committed.push_back(std::move(c));
            batchChunks.clear();
            batchStates.clear();
            out.commits.push_back(cmit->savedState);
            bindTipTo(out.tip, rec.offset, rec.offset + rec.consumed, rec.generation,
                      rec.checksum, rec.checksumSize);
            out.tipValid = true;
        } else {
            // Frame extent recorded: compaction copies exactly the frames replay accepted, so a
            // foreign writer's dead or pre-chain bytes can never ride into a fresh checkpoint.
            pending[{rec.type, rec.key.bytes}] =
                RecoveredChunk{rec.type,  rec.key,    rec.generation,
                               rec.flags, std::move(*payload), rec.offset, rec.consumed};
        }
    }
    // A trailing torn batch (crash mid-Save, or a foreign writer's dead bytes) leaves
    // committedEnd short of the physical end; the next appendSaveToFile truncates it.
    if (out.tip.committedEnd != file.size())
        out.committedAnomaly = true;
    return out;
}

bool stampTipIdentity(const std::string& path, CommitTip& tip, std::string* error) {
#ifndef _WIN32
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (error)
            *error = "could not open the file to stamp its identity";
        return false;
    }
    const auto id = identityOf(fd);
    ::close(fd);
    if (!id.has_value()) {
        if (error)
            *error = "could not stat the file";
        return false;
    }
    tip.device = id->device;
    tip.inode = id->inode;
    return true;
#else
    // The same two steps as the POSIX branch -- open, ask the filesystem who this file is, record
    // it -- with GetFileInformationByHandle standing in for fstat (see identityOf above for exactly
    // how much weaker that answer is, and why the tail check is still sound).
    const HANDLE h = openForTailCheck(path);
    if (h == INVALID_HANDLE_VALUE) {
        if (error)
            *error = "could not open the file to stamp its identity";
        return false;
    }
    const auto id = identityOf(h);
    ::CloseHandle(h);
    if (!id.has_value()) {
        if (error)
            *error = "could not stat the file";
        return false;
    }
    tip.device = id->device;
    tip.inode = id->inode;
    return true;
#endif
}

SaveStatus verifyTail(const std::string& path, const CommitTip& tip, std::string* error) {
    const auto fail = [&](SaveStatus status, const char* what) {
        if (error)
            *error = what;
        return status;
    };
#ifndef _WIN32
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return fail(SaveStatus::IoError, "could not open the file for the tail check");
    const auto id = identityOf(fd);
    if (!id.has_value()) {
        ::close(fd);
        return fail(SaveStatus::IoError, "could not stat the file");
    }
    if (id->size != tip.fileSize) {
        ::close(fd);
        return fail(SaveStatus::TailSizeChanged,
                    "the file no longer ends where this writer last left it");
    }
    if ((tip.device != 0 || tip.inode != 0) &&
        (id->device != tip.device || id->inode != tip.inode)) {
        ::close(fd);
        return fail(SaveStatus::TailIdentityChanged,
                    "the file was replaced since it was opened");
    }
    // One O(1) read of the last commit's STORED checksum at the known offset: parse the header
    // for the frame extent, then read the trailing checksum bytes. This detects foreign
    // rewrites; it deliberately does not re-hash the frame -- bit rot is recovery's job.
    std::array<std::uint8_t, kHeaderSize> header{};
    if (::pread(fd, header.data(), header.size(), static_cast<off_t>(tip.lastCommitOffset)) !=
        static_cast<ssize_t>(header.size())) {
        ::close(fd);
        return fail(SaveStatus::TailCommitMismatch, "the last commit's frame is gone");
    }
    std::vector<std::uint8_t> headerBuf(header.begin(), header.end());
    const auto rec = parseChunkAt(headerBuf, 0);
    if (!rec.has_value() || rec->checksumSize != tip.checksumSize) {
        ::close(fd);
        return fail(SaveStatus::TailCommitMismatch, "the last commit's frame is unreadable");
    }
    const std::size_t linkBytes = rec->linked() ? kLinkSize : 0;
    const std::uint64_t checksumAt =
        tip.lastCommitOffset + kHeaderSize + linkBytes + rec->payloadLen;
    std::array<std::uint8_t, kStrongChecksumSize> stored{};
    if (checksumAt + tip.checksumSize > id->size ||
        ::pread(fd, stored.data(), tip.checksumSize, static_cast<off_t>(checksumAt)) !=
            static_cast<ssize_t>(tip.checksumSize)) {
        ::close(fd);
        return fail(SaveStatus::TailCommitMismatch, "the last commit's checksum is gone");
    }
    ::close(fd);
    if (!std::equal(stored.begin(), stored.begin() + tip.checksumSize, tip.checksum.begin()))
        return fail(SaveStatus::TailCommitMismatch,
                    "the last commit is not the one this writer saved");
    return SaveStatus::Ok;
#else
    // Statement for statement the POSIX branch above, with CreateFileW/GetFileInformationByHandle
    // for open+fstat and readAtOffset for pread. The verdicts and their messages are identical
    // BY CONSTRUCTION -- the comparisons, their order, and every SaveStatus they yield are the same
    // code with two I/O backends, because a Windows Save that refused for a different reason (or
    // failed to refuse at all) would be a different format, not a port.
    const HANDLE h = openForTailCheck(path);
    if (h == INVALID_HANDLE_VALUE)
        return fail(SaveStatus::IoError, "could not open the file for the tail check");
    const auto id = identityOf(h);
    if (!id.has_value()) {
        ::CloseHandle(h);
        return fail(SaveStatus::IoError, "could not stat the file");
    }
    if (id->size != tip.fileSize) {
        ::CloseHandle(h);
        return fail(SaveStatus::TailSizeChanged,
                    "the file no longer ends where this writer last left it");
    }
    if ((tip.device != 0 || tip.inode != 0) &&
        (id->device != tip.device || id->inode != tip.inode)) {
        ::CloseHandle(h);
        return fail(SaveStatus::TailIdentityChanged,
                    "the file was replaced since it was opened");
    }
    // One O(1) read of the last commit's STORED checksum at the known offset -- header first for
    // the frame extent, then the trailing checksum bytes. As on POSIX it deliberately does not
    // re-hash the frame: bit rot is recovery's job, a foreign rewrite is this check's job.
    std::array<std::uint8_t, kHeaderSize> header{};
    if (readAtOffset(h, header.data(), header.size(), tip.lastCommitOffset) !=
        static_cast<std::int64_t>(header.size())) {
        ::CloseHandle(h);
        return fail(SaveStatus::TailCommitMismatch, "the last commit's frame is gone");
    }
    std::vector<std::uint8_t> headerBuf(header.begin(), header.end());
    const auto rec = parseChunkAt(headerBuf, 0);
    if (!rec.has_value() || rec->checksumSize != tip.checksumSize) {
        ::CloseHandle(h);
        return fail(SaveStatus::TailCommitMismatch, "the last commit's frame is unreadable");
    }
    const std::size_t linkBytes = rec->linked() ? kLinkSize : 0;
    const std::uint64_t checksumAt =
        tip.lastCommitOffset + kHeaderSize + linkBytes + rec->payloadLen;
    std::array<std::uint8_t, kStrongChecksumSize> stored{};
    if (checksumAt + tip.checksumSize > id->size ||
        readAtOffset(h, stored.data(), tip.checksumSize, checksumAt) !=
            static_cast<std::int64_t>(tip.checksumSize)) {
        ::CloseHandle(h);
        return fail(SaveStatus::TailCommitMismatch, "the last commit's checksum is gone");
    }
    ::CloseHandle(h);
    if (!std::equal(stored.begin(), stored.begin() + tip.checksumSize, tip.checksum.begin()))
        return fail(SaveStatus::TailCommitMismatch,
                    "the last commit is not the one this writer saved");
    return SaveStatus::Ok;
#endif
}

SaveStatus appendSaveToFile(const std::string& path, CommitTip& tip,
                            std::span<const SaveState> states, std::string* error) {
    const SaveStatus check = verifyTail(path, tip, error);
    if (check != SaveStatus::Ok)
        return check; // refuse loudly; the file is untouched (Round 13 D4)
#ifndef _WIN32
    const auto fail = [&](const char* what) {
        if (error)
            *error = what;
        return SaveStatus::IoError;
    };
    const SaveBatch batch = buildSaveBatch(tip.link(), states);
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0)
        return fail("could not open the file for appending");
    // Discard a dead tail (a torn earlier Save): unreachable by construction, and leaving it
    // would bury the new batch behind an anomaly that conservative replay stops at.
    if (tip.fileSize > tip.committedEnd &&
        ::ftruncate(fd, static_cast<off_t>(tip.committedEnd)) != 0) {
        ::close(fd);
        return fail("could not truncate the dead tail");
    }
    std::size_t written = 0;
    while (written < batch.bytes.size()) {
        const ::ssize_t n = ::pwrite(fd, batch.bytes.data() + written,
                                     batch.bytes.size() - written,
                                     static_cast<off_t>(tip.committedEnd + written));
        if (n < 0) {
            ::close(fd);
            // A torn batch may now exist -- exactly the crash shape the format absorbs: replay
            // opens at the previous commit, and the journal still holds these states.
            return fail("could not append the Save batch");
        }
        written += static_cast<std::size_t>(n);
    }
    const bool synced =
#ifdef __linux__
        ::fdatasync(fd) == 0;
#else
        ::fsync(fd) == 0;
#endif
    ::close(fd);
    if (!synced)
        return fail("could not flush the Save batch to disk");
    bindTipTo(tip, tip.committedEnd + batch.cmitOffset, tip.committedEnd + batch.bytes.size(),
              batch.commitId, batch.cmitChecksum, batch.cmitChecksumSize);
    tip.fileSize = tip.committedEnd;
    return SaveStatus::Ok;
#else
    const auto fail = [&](const char* what) {
        if (error)
            *error = what;
        return SaveStatus::IoError;
    };
    const SaveBatch batch = buildSaveBatch(tip.link(), states);
    // GENERIC_READ | GENERIC_WRITE mirrors O_RDWR, and the share mode mirrors it too: O_RDWR
    // excludes nobody, so neither does this. Locking the document here would also contradict §2.10
    // -- the advisory lock lives in the app-owned recovery dir precisely so that Mosaic never holds
    // a claim on a user's file, and detection (the tail check above), not exclusion, is the
    // load-bearing defense. A share mode of 0 would additionally fail against any sync client or
    // scanner that happens to have the file open, turning a survivable race into a refused Save.
    const HANDLE h = ::CreateFileW(common::pathFromUtf8(path).c_str(),
                                   GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return fail("could not open the file for appending");
    // Discard a dead tail (a torn earlier Save): unreachable by construction, and leaving it
    // would bury the new batch behind an anomaly that conservative replay stops at. SetEndOfFile
    // truncates at the CURRENT file pointer, so the seek IS the truncation's argument -- and it
    // doubles as the position the WriteFile loop below writes from, which is why this branch needs
    // no per-write offset the way pwrite does.
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(tip.committedEnd);
    if (::SetFilePointerEx(h, at, nullptr, FILE_BEGIN) == 0) {
        ::CloseHandle(h);
        return fail("could not seek to the append point");
    }
    if (tip.fileSize > tip.committedEnd && ::SetEndOfFile(h) == 0) {
        ::CloseHandle(h);
        return fail("could not truncate the dead tail");
    }
    std::size_t written = 0;
    while (written < batch.bytes.size()) {
        // WriteFile counts in a DWORD and is permitted to write SHORT, exactly like write(2), so
        // this loops like the POSIX one. A partial write mistaken for a whole one would leave a
        // batch whose CMIT frame is missing or truncated -- a document silently short of the state
        // the user just saved. The 1GiB cap keeps the count inside a DWORD on any batch size.
        const DWORD want =
            static_cast<DWORD>(std::min<std::size_t>(batch.bytes.size() - written, 1u << 30));
        DWORD n = 0;
        if (::WriteFile(h, batch.bytes.data() + written, want, &n, nullptr) == 0 || n == 0) {
            ::CloseHandle(h);
            // A torn batch may now exist -- exactly the crash shape the format absorbs: replay
            // opens at the previous commit, and the journal still holds these states. (n == 0 on
            // success is not a documented outcome for a non-zero request; treating it as failure is
            // what keeps this loop from spinning forever if a driver ever produces it.)
            return fail("could not append the Save batch");
        }
        written += n;
    }
    // FlushFileBuffers is fsync, and it is not optional: it is the durability point the whole
    // recovery ladder is designed around, the line either side of which a crash is survivable.
    // CloseHandle is NOT a substitute -- closing flushes this process's buffers to the filesystem,
    // never the filesystem's to the device.
    const bool synced = ::FlushFileBuffers(h) != 0;
    ::CloseHandle(h);
    if (!synced)
        return fail("could not flush the Save batch to disk");
    // Identical tip arithmetic to the POSIX branch, and the order inside it matters: bindTipTo
    // advances committedEnd to the new frame's end FIRST, so the assignment after it lands the new
    // value in fileSize (the file now ends exactly at the commit -- no dead tail).
    bindTipTo(tip, tip.committedEnd + batch.cmitOffset, tip.committedEnd + batch.bytes.size(),
              batch.commitId, batch.cmitChecksum, batch.cmitChecksumSize);
    tip.fileSize = tip.committedEnd;
    return SaveStatus::Ok;
#endif
}

bool needsCompaction(std::uint64_t fileSize, std::uint64_t walStartOffset,
                     double ratio) noexcept {
    if (fileSize <= walStartOffset || walStartOffset == 0)
        return false;
    return static_cast<double>(fileSize - walStartOffset) >
           ratio * static_cast<double>(walStartOffset);
}

} // namespace mosaic::io::native
