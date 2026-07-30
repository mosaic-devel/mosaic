#include "io/mosaic/save.hpp"

#include "io/export_path.hpp"
#include "io/mosaic/journal.hpp"
#include "io/mosaic/lock.hpp"

#include "common/fs_path.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The Windows port of the .mosaic write path (S57) -- pinned by the invariants that are PORTABLE,
// because the verification host is Linux and an assertion hidden behind #ifdef _WIN32 would never
// run here at all.
//
// What the port changed is not algorithmic. Every filesystem call now routes its UTF-8
// std::string path through common::pathFromUtf8 before it reaches a -W entry point, because on
// Windows std::filesystem::path is wchar_t-based and a narrow string is decoded in the ACTIVE CODE
// PAGE, not as UTF-8. Getting that wrong fails in the worst possible shape: invisible in ASCII,
// total outside it. A user whose account name carries an accent -- and the recovery journal ALWAYS
// lives under the account name (spec 2.6) -- gets "could not save the document" on every Ctrl+S
// and, silently, no crash protection whatsoever.
//
// So the load-bearing cases below drive the whole save ladder, and the whole journal lifecycle,
// through a directory whose name is not ASCII. On Linux the filesystem is byte-transparent and they
// pass without the helper doing anything interesting; on Windows they are the difference between a
// working editor and a broken one, and they fail loudly the moment a path stops going through the
// helper. The genuinely host-specific facts (which separator a joined path gets, whether a drive
// letter counts as absolute) are SPLIT by platform rather than skipped, so each host asserts its
// own truth instead of nothing.
namespace {

namespace fs = std::filesystem;
using namespace mosaic;
using namespace mosaic::io;
using namespace mosaic::io::native;

// Not ASCII, and deliberately not coverable by any single legacy code page either: a Latin-1
// character, Cyrillic and CJK in one name. A code-page decode cannot round-trip this even by luck,
// which is what makes the test diagnostic rather than lucky.
constexpr const char* kAwkwardDir = "mosaic_s57_Zoë_тест_日本";
constexpr const char* kAwkwardFile = "документ-Zoë.mosaic";

constexpr std::uint64_t kLayer = 3;
constexpr std::uint64_t kBaseGeneration = 10;
constexpr const char* kUuid = "s57-win-io-uuid";

// A fresh empty directory under TMPDIR whose PARENT component is non-ASCII, so both the directory
// creation and every open below carry the awkward bytes.
fs::path awkwardDir(const char* leaf) {
    const char* env = std::getenv("TMPDIR");
    const fs::path dir = common::pathFromUtf8(env != nullptr ? env : "/tmp") / kAwkwardDir / leaf;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    // A host that cannot make a UTF-8 directory name would make every case below meaningless, so
    // this is a hard requirement rather than a skip.
    REQUIRE(fs::is_directory(dir));
    return dir;
}

std::vector<std::uint8_t> tileContent(std::uint64_t stateId, std::uint32_t keyIdx) {
    std::vector<std::uint8_t> p(900 + keyIdx * 17);
    for (std::size_t i = 0; i < p.size(); ++i)
        p[i] = static_cast<std::uint8_t>(stateId * 31 + keyIdx * 5 + i * 7);
    return p;
}

CheckpointInput baseInput() {
    CheckpointInput in;
    in.documentUuid = kUuid;
    in.generation = kBaseGeneration;
    for (std::uint32_t i = 0; i < 4; ++i)
        in.chunks.push_back({kTypeTile, tileKey(kLayer, i, 0), kBaseGeneration, Profile::Balanced,
                             kFlagCritical, /*parity=*/true, /*history=*/false,
                             tileContent(kBaseGeneration, i)});
    return in;
}

SaveState makeState(std::uint64_t id, std::uint32_t keyIdx) {
    SaveState s;
    s.stateId = id;
    s.chunks.push_back({kTypeTile, tileKey(kLayer, keyIdx, 0), tileContent(id, keyIdx),
                        kFlagCritical});
    return s;
}

JournalBinding bindingFor(const CommitTip& tip, const std::string& docPath) {
    JournalBinding b;
    b.documentUuid = kUuid;
    b.documentPath = docPath;
    b.commitId = tip.commitId;
    b.checksum = tip.checksum;
    b.checksumSize = tip.checksumSize;
    return b;
}

std::vector<std::uint8_t> readFileBytes(const std::string& path) {
    std::ifstream f(common::pathFromUtf8(path), std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
}

// The newest payload for a tile key across a checkpoint and a committed region.
const std::vector<std::uint8_t>* newestTile(const OpenReport& open, std::uint32_t keyIdx) {
    const RecoveredChunk* found = open.find(kTypeTile, tileKey(kLayer, keyIdx, 0));
    return found == nullptr ? nullptr : &found->payload;
}

} // namespace

// The pathFromUtf8/utf8FromPath round trip itself is NOT re-asserted here: tests/test_fs_path.cpp
// already pins it in both directions, for ASCII, accented and CJK inputs, plus the POSIX
// byte-identity invariant. What is missing there -- and what every case below supplies -- is the
// round trip happening INSIDE the .mosaic write path, where a single un-converted call site is the
// whole bug.

TEST_CASE("S57: the whole Save ladder works under a non-ASCII directory and file name") {
    const fs::path dir = awkwardDir("save");
    const std::string path = common::utf8FromPath(dir / kAwkwardFile);
    // The path Mosaic carries and the path the filesystem sees must be the same file. On Windows
    // this is precisely what a narrow CreateFileA would get wrong -- and it would get it wrong
    // SILENTLY, by creating or failing to find a differently-named file.
    std::string error;
    REQUIRE_MESSAGE(writeFileAtomic(path, buildCheckpoint(baseInput()), &error), error);
    CHECK(fs::exists(dir / kAwkwardFile));
    // writeFileAtomic's temp is gone: nothing beside the document but the document. (On Windows the
    // temp now carries a unique suffix, so a leftover would show up here rather than being silently
    // reused by the next save.)
    CHECK(std::distance(fs::directory_iterator(dir), fs::directory_iterator{}) == 1);

    OpenReport open = openDocument(readFileBytes(path));
    REQUIRE(open.tipValid);
    CommitTip tip = open.tip;
    // stampTipIdentity has to OPEN the file to ask the filesystem who it is; on Windows that is
    // GetFileInformationByHandle behind CreateFileW, and a mangled path fails here first.
    REQUIRE_MESSAGE(stampTipIdentity(path, tip, &error), error);
    CHECK(tip.device != 0);
    CHECK(tip.inode != 0);

    // Two consecutive appended Saves -- the case the port's audit found broken on Windows, where a
    // new document could be saved exactly ONCE (the first save takes the full-write path, which
    // was implemented; every later Ctrl+S hit an unconditional IoError).
    REQUIRE(appendSaveToFile(path, tip, std::vector<SaveState>{makeState(11, 0)}, &error) ==
            SaveStatus::Ok);
    CHECK(verifyTail(path, tip) == SaveStatus::Ok);
    REQUIRE(appendSaveToFile(path, tip, std::vector<SaveState>{makeState(12, 1)}, &error) ==
            SaveStatus::Ok);
    CHECK(verifyTail(path, tip) == SaveStatus::Ok);
    CHECK(tip.fileSize == tip.committedEnd); // no dead tail after a clean Save

    const OpenReport reopened = openDocument(readFileBytes(path));
    CHECK(reopened.commits == std::vector<std::uint64_t>{11, 12});
    CHECK(!reopened.committedAnomaly);
    REQUIRE(newestTile(reopened, 0) != nullptr);
    REQUIRE(newestTile(reopened, 1) != nullptr);
    CHECK(*newestTile(reopened, 0) == tileContent(11, 0));
    CHECK(*newestTile(reopened, 1) == tileContent(12, 1));
    CHECK(*newestTile(reopened, 2) == tileContent(kBaseGeneration, 2)); // untouched by either Save

    // The tail check still REFUSES through the same plumbing: a foreign append moves the size, and
    // the Save must decline without writing rather than land a batch that is dead on arrival.
    const std::uint64_t before = tip.fileSize;
    {
        std::ofstream f(dir / kAwkwardFile, std::ios::binary | std::ios::app);
        const std::vector<std::uint8_t> junk(48, 0x5A);
        f.write(reinterpret_cast<const char*>(junk.data()),
                static_cast<std::streamsize>(junk.size()));
    }
    CHECK(verifyTail(path, tip) == SaveStatus::TailSizeChanged);
    CommitTip stale = tip;
    CHECK(appendSaveToFile(path, stale, std::vector<SaveState>{makeState(13, 2)}, &error) ==
          SaveStatus::TailSizeChanged);
    CHECK(readFileBytes(path).size() == before + 48); // refused means UNTOUCHED

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("S57: the recovery journal's whole lifecycle works under a non-ASCII directory") {
    const fs::path dir = awkwardDir("journal");
    const std::string docPath = common::utf8FromPath(dir / kAwkwardFile);
    std::string error;
    REQUIRE_MESSAGE(writeFileAtomic(docPath, buildCheckpoint(baseInput()), &error), error);
    OpenReport open = openDocument(readFileBytes(docPath));
    REQUIRE(open.tipValid);
    const JournalBinding binding = bindingFor(open.tip, docPath);

    // The journal path itself is non-ASCII, exactly as it is on a real Windows box: the recovery
    // directory sits under the account name, so an accented user has an accented journal path and
    // there is no ASCII fallback anywhere in the chain.
    const std::string jpath = common::utf8FromPath(dir / "Zoë-журнал");
    auto writer = JournalWriter::create(jpath, binding, &error);
    REQUIRE_MESSAGE(writer.has_value(), error);
    CHECK(fs::exists(dir / "Zoë-журнал"));

    const SaveState st = makeState(11, 0);
    for (const StateChunk& c : st.chunks)
        REQUIRE(writer->append(c.type, c.key, st.stateId, c.payload, Profile::Fast, c.flags));
    const std::string hist = histPayloadFor(st);
    REQUIRE(writer->append(kTypeHist, histKey(st.stateId), st.stateId,
                           std::span<const std::uint8_t>(
                               reinterpret_cast<const std::uint8_t*>(hist.data()), hist.size()),
                           Profile::Store));
    REQUIRE(writer->sync());
    // sizeBytes() is the growth-compaction trigger's only input, and it comes from ftell -- which
    // on Windows must be _ftelli64, since MinGW's long is 32 bits. After a sync the buffered writer
    // and the file agree, so the real file size is the assertion.
    CHECK(writer->sizeBytes() == fs::file_size(dir / "Zoë-журнал"));

    const JournalReplay replay = replayJournal(readFileBytes(jpath), binding);
    CHECK(replay.binding == JournalBindingStatus::Ok);
    CHECK(!replay.anomaly);
    CHECK(replay.states == std::vector<std::uint64_t>{11});

    // reset() rebinds in place (freopen on POSIX, _wfreopen on Windows) and must still be able to
    // find its own file afterwards.
    REQUIRE(writer->reset(binding));
    CHECK(writer->sizeBytes() > 0); // the fresh JHDR frame
    const JournalReplay afterReset = replayJournal(readFileBytes(jpath), binding);
    CHECK(afterReset.binding == JournalBindingStatus::Ok);
    CHECK(afterReset.states.empty());

    // discard() deletes by path -- _wremove on Windows.
    REQUIRE(writer->discard());
    CHECK(!fs::exists(dir / "Zoë-журнал"));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("S57: the journal growth swap installs the fresh file and keeps appending after it") {
    // replaceWith is the one place where the two platforms genuinely diverge: POSIX renames over
    // the live journal and ADOPTS the handle (which survives, pointing at the now-unlinked file),
    // while Windows must close both handles first -- a file held open without FILE_SHARE_DELETE
    // cannot be replaced -- and then reopen the destination positioned at its end. This case pins
    // the observable contract both must satisfy, so the POSIX branch is protected against the
    // refactor and the Windows branch has something concrete to be equivalent TO.
    const fs::path dir = awkwardDir("swap");
    const std::string docPath = common::utf8FromPath(dir / kAwkwardFile);
    std::string error;
    REQUIRE_MESSAGE(writeFileAtomic(docPath, buildCheckpoint(baseInput()), &error), error);
    const OpenReport open = openDocument(readFileBytes(docPath));
    REQUIRE(open.tipValid);
    const JournalBinding binding = bindingFor(open.tip, docPath);

    const std::string jpath = common::utf8FromPath(dir / "Zoë-журнал");
    auto live = JournalWriter::create(jpath, binding, &error);
    REQUIRE_MESSAGE(live.has_value(), error);
    const SaveState first = makeState(11, 0);
    for (const StateChunk& c : first.chunks)
        REQUIRE(live->append(c.type, c.key, first.stateId, c.payload, Profile::Fast, c.flags));
    REQUIRE(live->sync());

    // The compacted replacement, written and synced at a temp path in the same directory (exactly
    // what JournalSession::maybeCompact does), carrying one cumulative state.
    const std::string tmpPath = jpath + ".compact";
    auto fresh = JournalWriter::create(tmpPath, binding, &error);
    REQUIRE_MESSAGE(fresh.has_value(), error);
    const SaveState folded = makeState(11, 1);
    for (const StateChunk& c : folded.chunks)
        REQUIRE(fresh->append(c.type, c.key, folded.stateId, c.payload, Profile::Fast, c.flags));
    const std::string hist = histPayloadFor(folded);
    REQUIRE(fresh->append(kTypeHist, histKey(folded.stateId), folded.stateId,
                          std::span<const std::uint8_t>(
                              reinterpret_cast<const std::uint8_t*>(hist.data()), hist.size()),
                          Profile::Store));
    REQUIRE(fresh->sync());
    const std::uint64_t freshSize = fresh->sizeBytes();
    REQUIRE(freshSize > 0);

    REQUIRE(live->replaceWith(std::move(*fresh)));
    CHECK(!fs::exists(fs::path(common::pathFromUtf8(tmpPath)))); // the temp is consumed, not left
    CHECK(fs::exists(dir / "Zoë-журнал"));
    // The adopted handle must be positioned at the END of the installed file, whether it was
    // inherited (POSIX) or reopened (Windows). If it were at offset 0, the next append would
    // overwrite the JHDR frame and the journal would stop replaying at all.
    CHECK(live->sizeBytes() == freshSize);

    // And the session keeps going: one more append continues the ADOPTED running link, which is the
    // in-memory state the swap is really about (spec 2.6's chain-reset bug is what happens when it
    // is re-derived instead).
    const SaveState next = makeState(12, 2);
    for (const StateChunk& c : next.chunks)
        REQUIRE(live->append(c.type, c.key, next.stateId, c.payload, Profile::Fast, c.flags));
    const std::string hist2 = histPayloadFor(next);
    REQUIRE(live->append(kTypeHist, histKey(next.stateId), next.stateId,
                         std::span<const std::uint8_t>(
                             reinterpret_cast<const std::uint8_t*>(hist2.data()), hist2.size()),
                         Profile::Store));
    REQUIRE(live->sync());
    CHECK(live->sizeBytes() > freshSize);

    const JournalReplay replay = replayJournal(readFileBytes(jpath), binding);
    CHECK(replay.binding == JournalBindingStatus::Ok);
    CHECK(!replay.anomaly); // an unbroken chain across the swap
    CHECK(replay.states == std::vector<std::uint64_t>{11, 12});

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("S57: the advisory lock takes and releases a lock file under a non-ASCII directory") {
    const fs::path dir = awkwardDir("lock");
    // The lock lives beside the journal, so it inherits the account name's bytes too -- and on
    // Windows it is an exclusive CreateFileW whose dead-holder auto-release is the whole §2.10
    // mechanism. Nothing here can observe the Windows sharing violation from Linux; what it does
    // pin is that the path reaches the filesystem intact and that release() is idempotent.
    const std::string lpath = common::utf8FromPath(dir / "sub" / "Zoë-замок.lock");
    std::optional<AdvisoryLock> a;
    REQUIRE(AdvisoryLock::tryAcquire(lpath, a) == AdvisoryLock::Status::Acquired);
    REQUIRE(a.has_value());
    CHECK(a->path() == lpath);
    CHECK(fs::exists(dir / "sub" / "Zoë-замок.lock")); // the parent dir was created for it
    a->release();
    a->release(); // idempotent

    std::optional<AdvisoryLock> b;
    CHECK(AdvisoryLock::tryAcquire(lpath, b) == AdvisoryLock::Status::Acquired);

    std::error_code ec;
    b.reset();
    fs::remove_all(dir, ec);
}

#if !defined(_WIN32)
TEST_CASE("S57: the recovery journal path stays under the state dir, non-ASCII account and all") {
    // setenv is POSIX-only, so this case is guarded rather than made portable. On Windows the same
    // redirection happens through LOCALAPPDATA, read via _wgetenv (std::getenv would hand back the
    // active-code-page transcode of it, which is not UTF-8), with SHGetKnownFolderPath as the
    // authority when the variable is absent.
    const fs::path state = awkwardDir("state");
    const std::string stateUtf8 = common::utf8FromPath(state);
    REQUIRE(::setenv("XDG_STATE_HOME", stateUtf8.c_str(), 1) == 0);
    const std::string j = recoveryJournalPath(kUuid, "/home/Zoë/art/документ.mosaic");
    CHECK(j.rfind(stateUtf8 + "/mosaic/recovery/", 0) == 0);
    CHECK(j.find(kUuid) != std::string::npos);
    // Stable for the same document, distinct for a copy at another path (the path-hash component).
    CHECK(j == recoveryJournalPath(kUuid, "/home/Zoë/art/документ.mosaic"));
    CHECK(j != recoveryJournalPath(kUuid, "/home/Zoë/other/документ.mosaic"));
    // The lock shares the journal's key exactly.
    CHECK(recoveryLockPath(kUuid, "/home/Zoë/art/документ.mosaic") == j + ".lock");
    ::unsetenv("XDG_STATE_HOME");
    std::error_code ec;
    fs::remove_all(state, ec);
}
#endif

TEST_CASE("S57: isAbsolutePath answers the HOST's rule and refuses every half-rooted path") {
#if defined(_WIN32)
    CHECK(isAbsolutePath("C:\\Users\\Zoë\\shot.png"));
    CHECK(isAbsolutePath("C:/Users/Zoë/shot.png"));
    CHECK(isAbsolutePath("\\\\server\\share\\shot.png")); // UNC: root name + root directory
    CHECK(!isAbsolutePath("C:shot.png"));  // relative to the current directory ON drive C
    CHECK(!isAbsolutePath("\\shot.png"));  // the CURRENT drive's root -- process state, not a path
    CHECK(!isAbsolutePath("/shot.png"));   // the same thing, POSIX-spelled
    CHECK(!isAbsolutePath("/home/tester/shot.png"));
#else
    CHECK(isAbsolutePath("/home/tester/shot.png"));
    CHECK(isAbsolutePath("/home/Zoë/shot.png"));
    // A Windows-shaped path is NOT absolute here, and that is the answer the policy needs: it gets
    // DROPPED rather than resolved against the process working directory (bug 1, test_export_path).
    CHECK(!isAbsolutePath("C:\\Users\\Zoë\\shot.png"));
    CHECK(!isAbsolutePath("\\\\server\\share\\shot.png"));
#endif
    CHECK(!isAbsolutePath(""));
    CHECK(!isAbsolutePath("shot.png"));
    CHECK(!isAbsolutePath("sub/dir/shot.png"));
}

TEST_CASE("S57: a joined export path uses the host separator and never doubles one") {
#if defined(_WIN32)
    CHECK(resolveExportPath("shot.png", "C:\\srv\\out") == "C:\\srv\\out\\shot.png");
    CHECK(resolveExportPath("shot.png", "C:\\srv\\out\\") == "C:\\srv\\out\\shot.png");
    CHECK(resolveExportPath("shot.png", "C:/srv/out") == "C:/srv/out\\shot.png"); // input: either
#else
    CHECK(resolveExportPath("shot.png", "/srv/out") == "/srv/out/shot.png");
    CHECK(resolveExportPath("shot.png", "/srv/out/") == "/srv/out/shot.png");
#endif
    // An absolute typed path wins outright, and a relative one with nowhere honest to resolve
    // against yields NOTHING -- never the working directory. Identical on both hosts.
    CHECK(resolveExportPath("shot.png", "").empty());
    CHECK(resolveExportPath("", "/srv/out").empty());
}

TEST_CASE("S57: a drive-rooted document path keeps its separator, and never leaks into POSIX") {
    ExportPathInputs in;
    in.documentPath = "C:\\shot.mosaic";
    in.fallbackDir = "/home/tester/Pictures";
#if defined(_WIN32)
    // "C:" alone is the current directory ON drive C -- a different place, and one isAbsolutePath
    // rightly refuses -- so the drive root must keep its separator or the folder is silently lost.
    CHECK(exportStartFolder(in) == "C:\\");
#else
    // Here the same input is not absolute at all, so it is dropped and the next rule answers.
    CHECK(exportStartFolder(in) == "/home/tester/Pictures");
#endif
}
