#include "io/mosaic/journal.hpp"

#include "io/mosaic/naming.hpp"
#include "io/mosaic/records.hpp"
#include "io/mosaic/wire.hpp"

#include "common/fs_path.hpp" // pathFromUtf8 / utf8FromPath -- the journal lives under the account

#include <algorithm>
#include <nlohmann/json.hpp>

#define XXH_INLINE_ALL // vendored single-header usage (third_party/xxhash), like chunk.cpp
#include <xxhash.h>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#else
#include <io.h> // _commit -- the fsync under flushToDevice
// See save.cpp: the toolchain defines NOMINMAX, and redefining it is a -Werror diagnostic.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h> // SHGetKnownFolderPath -- the state directory, without going through getenv
#endif

namespace mosaic::io::native {
namespace {

using nlohmann::json;

// The journal's four path-taking primitives. On POSIX they ARE the plain CRT calls, byte for byte.
// On Windows every one routes through the WIDE entry point, because Mosaic carries UTF-8
// std::string paths and the narrow CRT decodes those bytes in the ACTIVE CODE PAGE: for a user
// whose account is "Zoë" -- and the journal always lives under the account, §2.6 -- fopen() on a
// perfectly correct UTF-8 path fails, and the whole autosave story silently becomes "could not
// create the recovery journal" with no crash protection behind it.

[[nodiscard]] std::FILE* openTruncating(const std::string& path) {
#if defined(_WIN32)
    return ::_wfopen(common::pathFromUtf8(path).c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

[[nodiscard]] std::FILE* reopenTruncating(const std::string& path, std::FILE* old) {
#if defined(_WIN32)
    return ::_wfreopen(common::pathFromUtf8(path).c_str(), L"wb", old);
#else
    return std::freopen(path.c_str(), "wb", old);
#endif
}

[[nodiscard]] bool removeFile(const std::string& path) {
#if defined(_WIN32)
    return ::_wremove(common::pathFromUtf8(path).c_str()) == 0;
#else
    return std::remove(path.c_str()) == 0;
#endif
}

#if defined(_WIN32)
// Reopen an EXISTING journal positioned at its end -- the state replaceWith has to reconstruct on
// Windows (see there). "r+b" plus an explicit seek rather than "a": append mode forces every write
// to EOF, which is nearly right, but C leaves the INITIAL position unspecified and sizeBytes()
// reads ftell. Seeking explicitly makes the reopened handle indistinguishable from the mid-stream
// one the POSIX path adopts.
[[nodiscard]] std::FILE* openAtEnd(const std::string& path) {
    std::FILE* f = ::_wfopen(common::pathFromUtf8(path).c_str(), L"r+b");
    if (f == nullptr)
        return nullptr;
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return nullptr;
    }
    return f;
}
#endif

[[nodiscard]] std::span<const std::uint8_t> stringBytes(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

[[nodiscard]] std::string boundChecksumHex(const JournalBinding& b) {
    return detail::bytesToHex({b.checksum.data(), b.checksumSize});
}

// Flush C-library buffers, then the OS's, down to the device. The journal's durability point:
// append() only buffers; this is what an autosave cadence actually pays for.
[[nodiscard]] bool flushToDevice(std::FILE* f) {
    if (std::fflush(f) != 0)
        return false;
#if defined(_WIN32)
    return _commit(_fileno(f)) == 0;
#elif defined(__linux__)
    return ::fdatasync(::fileno(f)) == 0;
#else
    return ::fsync(::fileno(f)) == 0;
#endif
}

} // namespace

std::string recoveryJournalPath(const std::string& documentUuid,
                                const std::string& canonicalDocumentPath) {
    namespace fs = std::filesystem;
    fs::path base;
#ifndef _WIN32
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && *xdg != '\0') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = fs::path(home != nullptr && *home != '\0' ? home : ".") / ".local" / "state";
    }
#else
    // _wgetenv, not std::getenv: the CRT keeps a NARROW copy of the environment transcoded through
    // the active code page, so "C:\Users\Zoë\AppData\Local" arrives as CP-1252 bytes that are not
    // UTF-8 and cannot be made into one. The wide environment is the same variable with no
    // transcode in it at all. Keeping the variable (rather than always asking the shell) is what
    // lets a portable install and the test suite redirect the state directory, exactly as
    // XDG_STATE_HOME does above.
    if (const wchar_t* local = ::_wgetenv(L"LOCALAPPDATA"); local != nullptr && *local != L'\0') {
        base = fs::path(local);
    } else {
        // No variable (a service, a scheduled task, a stripped environment): ask the shell, which
        // is authoritative and also follows a redirected AppData. KF_FLAG_CREATE because this
        // directory is app-owned state we are about to write into -- not user space, so creating it
        // is ours to do (§0). CoTaskMemFree is required even on failure, and is nullptr-safe.
        PWSTR folder = nullptr;
        const bool got =
            SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr,
                                             &folder)) &&
            folder != nullptr;
        base = got ? fs::path(folder) : fs::path(L".");
        ::CoTaskMemFree(folder);
    }
#endif
    std::array<std::uint8_t, 8> hash{};
    detail::storeLe64(hash.data(), XXH3_64bits(canonicalDocumentPath.data(),
                                               canonicalDocumentPath.size()));
    // utf8FromPath, not .string(): the return value is a UTF-8 path by Mosaic's convention and
    // every caller feeds it back through pathFromUtf8. Identity on POSIX; the one lossless
    // direction on Windows, where .string() would narrow through the active code page.
    // The uuid goes through pathFromUtf8 too. A minted UUID is ASCII, but this one can also have
    // come out of a file's root JSON (readCheckpoint), and a foreign writer's non-ASCII string
    // must produce a well-defined file name rather than an active-code-page guess at one.
    return common::utf8FromPath(
        base / "mosaic" / "recovery" /
        common::pathFromUtf8(documentUuid + "-" + detail::bytesToHex(hash)));
}

std::optional<JournalWriter> JournalWriter::create(const std::string& path,
                                                   const JournalBinding& binding,
                                                   std::string* error) {
    namespace fs = std::filesystem;
    const fs::path p = common::pathFromUtf8(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec); // best-effort; fopen reports the truth
    }
    JournalWriter w;
    w.path_ = path;
    w.file_ = openTruncating(path);
    if (w.file_ == nullptr) {
        if (error)
            *error = "could not create the recovery journal";
        return std::nullopt;
    }
    if (!w.writeHeader(binding)) {
        if (error)
            *error = "could not write the journal binding header";
        return std::nullopt; // ~JournalWriter closes; the unusable file is truncated, harmless
    }
    return w;
}

JournalWriter::JournalWriter(JournalWriter&& other) noexcept
    : file_(std::exchange(other.file_, nullptr)), path_(std::move(other.path_)),
      tip_(other.tip_) {}

JournalWriter& JournalWriter::operator=(JournalWriter&& other) noexcept {
    if (this != &other) {
        if (file_ != nullptr)
            std::fclose(file_);
        file_ = std::exchange(other.file_, nullptr);
        path_ = std::move(other.path_);
        tip_ = other.tip_;
    }
    return *this;
}

JournalWriter::~JournalWriter() {
    if (file_ != nullptr)
        std::fclose(file_);
}

bool JournalWriter::writeHeader(const JournalBinding& binding) {
    const std::string payload = json{{"uuid", binding.documentUuid},
                                     {"bound_commit", binding.commitId},
                                     {"bound_checksum", boundChecksumHex(binding)},
                                     {"path", binding.documentPath}}
                                    .dump();
    std::vector<std::uint8_t> frame;
    const auto seed = binding.linkSeed();
    const AppendedChunk a = appendChunk(frame, kTypeJournalHeader, zeroKey(), binding.commitId,
                                        stringBytes(payload), Profile::Store, kFlagCritical,
                                        &seed);
    if (std::fwrite(frame.data(), 1, frame.size(), file_) != frame.size())
        return false;
    tip_ = a.linkValue();
    return true;
}

bool JournalWriter::append(ChunkTag type, const ChunkKey& key, std::uint64_t generation,
                           std::span<const std::uint8_t> payload, Profile profile,
                           std::uint8_t flags) {
    if (file_ == nullptr)
        return false;
    std::vector<std::uint8_t> frame;
    // tip_ is copied into the frame before it advances -- the threaded-link discipline the
    // many-sequential-appends stress test exists to protect (spec 2.6).
    const AppendedChunk a = appendChunk(frame, type, key, generation, payload, profile, flags,
                                        &tip_);
    if (std::fwrite(frame.data(), 1, frame.size(), file_) != frame.size())
        return false;
    tip_ = a.linkValue();
    return true;
}

bool JournalWriter::sync() {
    return file_ != nullptr && flushToDevice(file_);
}

bool JournalWriter::reset(const JournalBinding& binding) {
    if (file_ == nullptr)
        return false;
    // freopen truncates in place -- the pre-Save frames vanish only here, which the caller must
    // sequence AFTER the Save's bytes are durable (Round 12 A2: the crash window keeps both).
    std::FILE* reopened = reopenTruncating(path_, file_);
    if (reopened == nullptr) {
        file_ = nullptr;
        return false;
    }
    file_ = reopened;
    return writeHeader(binding) && sync();
}

bool JournalWriter::discard() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (path_.empty())
        return true;
    return removeFile(path_);
}

bool JournalWriter::replaceWith(JournalWriter&& fresh) {
    const std::string freshPath = fresh.path_;
    if (fresh.file_ == nullptr) {
        fresh.discard(); // nothing to install; this journal is untouched
        return false;
    }
#ifdef _WIN32
    // Windows cannot do the POSIX trick below, and not for want of an API -- two rules forbid it
    // outright. (1) A file that ANY handle holds open without FILE_SHARE_DELETE can be neither
    // renamed nor replaced, and the CRT's fopen grants read/write sharing, never delete -- so both
    // the live journal and the fresh one must be closed before the swap. (2) There is no
    // "rename unlinks the old name and my handle keeps the old file alive": a Win32 handle follows
    // the file, so after the swap nothing is left to adopt. The order therefore inverts -- close
    // both, replace by name, REOPEN the destination at its end -- and what makes that a mechanical
    // difference rather than a semantic one is that the thing actually being adopted is
    // `fresh.tip_`, the in-memory running link, which no filesystem operation touches.
    //
    // The window this opens (both handles closed, the swap not yet done) costs nothing the POSIX
    // path does not also accept: the journal is crash INSURANCE, both files on disk are
    // individually complete and synced -- maybeCompact syncs the fresh one first -- so a crash
    // anywhere in here leaves one replayable journal for the next open. The one outcome that must
    // not happen is losing the LIVE journal without installing the fresh one, which is why the
    // failure path below reopens the live path before it reports.
    std::fclose(std::exchange(fresh.file_, nullptr));
    if (file_ != nullptr)
        std::fclose(std::exchange(file_, nullptr));
    const std::filesystem::path from = common::pathFromUtf8(freshPath);
    const std::filesystem::path to = common::pathFromUtf8(path_);
    // MOVEFILE_REPLACE_EXISTING is not optional: unlike POSIX rename(), a bare Win32 move REFUSES
    // an existing destination -- and here that is always the live journal. MOVEFILE_WRITE_THROUGH
    // is the parity for the parent-directory fsync the POSIX branch does below.
    if (::MoveFileExW(from.c_str(), to.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        std::error_code ec;
        std::filesystem::remove(from, ec); // drop the temp; the live journal is what we reopen
        file_ = openAtEnd(path_);          // may fail -- then appends report false and the session
        fresh.path_.clear();               // marks itself dead, which is the correct degradation
        return false;
    }
    file_ = openAtEnd(path_);
    tip_ = fresh.tip_;
    fresh.path_.clear(); // consumed: its destructor must not close or remove anything
    return file_ != nullptr;
#else
    if (std::rename(freshPath.c_str(), path_.c_str()) != 0) {
        fresh.discard(); // closes and removes the temp; this journal is untouched
        return false;
    }
    if (file_ != nullptr)
        std::fclose(file_); // the old inode: unlinked by the rename, closed here
    file_ = std::exchange(fresh.file_, nullptr);
    tip_ = fresh.tip_;
    fresh.path_.clear(); // consumed: its destructor must not close or remove anything
    // Parent-directory durability, like writeFileAtomic (spec 2.6): POSIX permits losing the
    // rename itself on power failure without it. Best-effort -- the journal is crash INSURANCE,
    // and the worst case of a lost rename is the pre-compaction journal, which replays the same.
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(path_).parent_path();
    if (!dir.empty()) {
        const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) {
            ::fsync(dfd);
            ::close(dfd);
        }
    }
#endif
    return true;
}

std::uint64_t JournalWriter::sizeBytes() const noexcept {
    if (file_ == nullptr)
        return 0;
#if defined(_WIN32)
    // _ftelli64, not ftell: MinGW's long is 32 bits, so ftell caps at 2GB and returns -1 past it.
    // This value is the growth-compaction trigger's only input (spec 2.6 "Growth" -- the journal
    // grows with the unsaved session), so capping it would report 0 for exactly the runaway journal
    // compaction exists to bound, and the fold would never fire. On POSIX long is 64 bits under
    // _FILE_OFFSET_BITS=64, which is why that branch stays as it is.
    const std::int64_t at = ::_ftelli64(file_);
#else
    const long at = std::ftell(file_);
#endif
    return at < 0 ? 0 : static_cast<std::uint64_t>(at);
}

std::optional<JournalHeaderInfo> readJournalHeader(std::span<const std::uint8_t> buf) {
    const auto rec = parseChunkAt(buf, 0);
    if (!rec.has_value() || !rec->valid || rec->type != kTypeJournalHeader || !rec->linked())
        return std::nullopt;
    const auto payload = decodeChunkPayload(*rec, buf);
    if (!payload.has_value())
        return std::nullopt;
    const json j = json::parse(payload->begin(), payload->end(), nullptr,
                               /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object() || !j.contains("uuid") || !j["uuid"].is_string())
        return std::nullopt;
    JournalHeaderInfo info;
    info.documentUuid = j["uuid"].get<std::string>();
    info.documentPath = j.value("path", std::string{});
    info.commitId = j.value("bound_commit", std::uint64_t{0});
    const std::string hex = j.value("bound_checksum", std::string{});
    if (hex.size() % 2 != 0 || hex.size() > kStrongChecksumSize * 2)
        return std::nullopt;
    info.checksumSize = static_cast<std::uint8_t>(hex.size() / 2);
    for (std::size_t i = 0; i < info.checksumSize; ++i) {
        const int hi = detail::hexNibble(hex[i * 2]);
        const int lo = detail::hexNibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return std::nullopt;
        info.checksum[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return info;
}

JournalBindingStatus verifyJournalHeader(std::span<const std::uint8_t> buf,
                                         const JournalBinding& expected, std::size_t* headerEnd,
                                         std::array<std::uint8_t, kLinkSize>* headerLink) {
    const auto rec = parseChunkAt(buf, 0);
    if (!rec.has_value() || !rec->valid || rec->type != kTypeJournalHeader || !rec->linked())
        return JournalBindingStatus::NoHeader;
    // Layer 1, structural: the chain seed is the bound commit's checksum, so a journal bound to
    // a different save of the file fails HERE, by the math, before any policy field is read.
    if (rec->link != expected.linkSeed())
        return JournalBindingStatus::WrongSeed;
    // Layer 2, policy: the JHDR fields reject independently (belt and suspenders, Round 11 A3).
    const auto payload = decodeChunkPayload(*rec, buf);
    if (!payload.has_value())
        return JournalBindingStatus::NoHeader;
    const json j = json::parse(payload->begin(), payload->end(), nullptr,
                               /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object() ||
        j.value("uuid", std::string{}) != expected.documentUuid ||
        j.value("bound_commit", std::uint64_t{0}) != expected.commitId ||
        j.value("bound_checksum", std::string{}) != boundChecksumHex(expected))
        return JournalBindingStatus::WrongBinding;
    if (headerEnd != nullptr)
        *headerEnd = rec->consumed;
    if (headerLink != nullptr)
        *headerLink = rec->linkValue();
    return JournalBindingStatus::Ok;
}

JournalReplay replayJournal(std::span<const std::uint8_t> buf, const JournalBinding& expected) {
    JournalReplay out;
    std::size_t offset = 0;
    std::array<std::uint8_t, kLinkSize> expectedLink{};
    out.binding = verifyJournalHeader(buf, expected, &offset, &expectedLink);
    if (out.binding != JournalBindingStatus::Ok)
        return out;

    const auto stop = [&](std::size_t at) {
        out.anomaly = true;
        out.anomalyOffset = at;
    };

    // A state's frames buffer in `pending` and commit only at its HIST frame -- transactional
    // per state (spec 3.2): a torn autosave loses at most the one half-written state.
    std::map<std::pair<ChunkTag, std::array<std::uint8_t, 16>>, RecoveredChunk> pending;
    while (offset < buf.size()) {
        const auto rec = parseChunkAt(buf, offset);
        if (!rec.has_value() || !rec->valid || !rec->complete || !rec->linked() ||
            rec->link != expectedLink) {
            stop(offset); // invalid, incomplete (torn tail), or link-mismatched: conservative stop
            break;
        }
        auto payload = decodeChunkPayload(*rec, buf);
        if (!payload.has_value()) {
            stop(offset);
            break;
        }
        expectedLink = rec->linkValue();
        const std::size_t at = offset;
        offset += rec->consumed;

        if (rec->type == kTypeHist) {
            const auto hist = parseHistRecord(*payload);
            if (!hist.has_value()) {
                stop(at);
                break;
            }
            bool complete = true;
            for (const DirtyKey& d : hist->dirty) {
                const auto it = pending.find({d.type, d.key.bytes});
                complete = complete && it != pending.end() &&
                           it->second.generation == hist->state;
            }
            if (!complete) {
                stop(at); // a HIST whose dirty content is not all present: not a valid state
                break;
            }
            for (const DirtyKey& d : hist->dirty) {
                const auto it = pending.find({d.type, d.key.bytes});
                out.chunks.push_back(std::move(it->second));
                pending.erase(it);
            }
            out.chunks.push_back(RecoveredChunk{rec->type, rec->key, rec->generation, rec->flags,
                                                std::move(*payload)});
            out.states.push_back(hist->state);
        } else {
            pending[{rec->type, rec->key.bytes}] = RecoveredChunk{
                rec->type, rec->key, rec->generation, rec->flags, std::move(*payload)};
        }
    }
    // Leftover pending frames belong to a state whose HIST never landed: dropped, exactly like
    // the torn tail they are.
    return out;
}

} // namespace mosaic::io::native
