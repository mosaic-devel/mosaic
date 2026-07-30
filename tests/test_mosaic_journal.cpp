#include "io/mosaic/journal.hpp"

#include "io/mosaic/salvage.hpp"
#include "io/mosaic/save.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// The recovery journal (S48 Build 1, journal slice; spec 2.6, 2.8) -- Round 11's A/B batteries
// re-run against the real implementation, as the spec's section 7 demands: binding (both layers,
// separately), torn tails, the many-sequential-appends chain stress (the research's chain-reset
// bug is invisible to any single-append test BY CONSTRUCTION), lifecycle, and salvage honesty
// including the naive-scanner hazard as a regression test.
namespace {

namespace fs = std::filesystem;
using namespace mosaic::io::native;

constexpr std::uint64_t kLayer = 7;
constexpr std::uint64_t kBaseGeneration = 10; // states consume ids ABOVE this (spec 2.2)
constexpr const char* kUuid = "journal-test-uuid";

ChunkKey keyOf(std::uint32_t i) {
    return tileKey(kLayer, i, 0);
}

// Deterministic, state- and key-dependent content: any mixup lands as a byte mismatch.
std::vector<std::uint8_t> tileContent(std::uint64_t stateId, std::uint32_t keyIdx) {
    std::vector<std::uint8_t> p(1500 + keyIdx * 31);
    for (std::size_t i = 0; i < p.size(); ++i)
        p[i] = static_cast<std::uint8_t>(stateId * 29 + keyIdx * 7 + i * 3);
    return p;
}

CheckpointInput baseInput() {
    CheckpointInput in;
    in.documentUuid = kUuid;
    in.generation = kBaseGeneration;
    for (std::uint32_t i = 0; i < 6; ++i)
        in.chunks.push_back({kTypeTile, keyOf(i), kBaseGeneration, Profile::Balanced, kFlagCritical,
                             /*parity=*/true, /*history=*/false, tileContent(kBaseGeneration, i)});
    return in;
}

SaveState makeState(std::uint64_t id, std::uint32_t keyIdx) {
    SaveState s;
    s.stateId = id;
    s.chunks.push_back({kTypeTile, keyOf(keyIdx), tileContent(id, keyIdx), kFlagCritical});
    return s;
}

using Plan = std::vector<std::pair<std::uint64_t, std::uint32_t>>; // (state id, key index)

// Ground truth per key after the whole plan ran.
std::map<std::uint32_t, std::vector<std::uint8_t>> truthAfter(const Plan& plan) {
    std::map<std::uint32_t, std::vector<std::uint8_t>> t;
    for (std::uint32_t i = 0; i < 6; ++i)
        t[i] = tileContent(kBaseGeneration, i);
    for (const auto& [id, k] : plan)
        t[k] = tileContent(id, k);
    return t;
}

// Highest generation wins across any number of recovered-chunk lists -- the document merge.
const RecoveredChunk* newest(std::initializer_list<const std::vector<RecoveredChunk>*> lists,
                             const ChunkTag& t, const ChunkKey& k) {
    const RecoveredChunk* best = nullptr;
    for (const auto* list : lists)
        for (const RecoveredChunk& c : *list)
            if (c.type == t && c.key == k &&
                (best == nullptr || c.generation >= best->generation))
                best = &c;
    return best;
}

// How many of the 6 keys mismatch ground truth under a given merge -- the silent-wrong-data
// detector every honesty check hinges on.
std::vector<std::uint32_t> mismatchedKeys(
    std::initializer_list<const std::vector<RecoveredChunk>*> lists,
    const std::map<std::uint32_t, std::vector<std::uint8_t>>& truth) {
    std::vector<std::uint32_t> out;
    for (const auto& [k, want] : truth) {
        const RecoveredChunk* got = newest(lists, kTypeTile, keyOf(k));
        if (got == nullptr || got->payload != want)
            out.push_back(k);
    }
    return out;
}

JournalBinding bindingFor(const CommitTip& tip) {
    JournalBinding b;
    b.documentUuid = kUuid;
    b.commitId = tip.commitId;
    b.checksum = tip.checksum;
    b.checksumSize = tip.checksumSize;
    return b;
}

fs::path testDir(const char* name) {
    const char* env = std::getenv("TMPDIR");
    const fs::path dir = fs::path(env != nullptr ? env : "/tmp") / name;
    fs::create_directories(dir);
    return dir;
}

std::vector<std::uint8_t> readFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
}

bool journalAppendState(JournalWriter& w, const SaveState& s) {
    for (const StateChunk& c : s.chunks)
        if (!w.append(c.type, c.key, s.stateId, c.payload, Profile::Fast, c.flags))
            return false;
    const std::string hist = histPayloadFor(s);
    return w.append(kTypeHist, histKey(s.stateId), s.stateId,
                    {reinterpret_cast<const std::uint8_t*>(hist.data()), hist.size()},
                    Profile::Store);
}

// Locate a frame for targeted corruption.
const ChunkRecord* findFrame(const std::vector<ChunkRecord>& recs, const ChunkTag& t,
                             std::uint64_t generation) {
    for (const ChunkRecord& r : recs)
        if (r.valid && r.type == t && r.generation == generation)
            return &r;
    return nullptr;
}

// Destroy a frame's STRUCTURE (corrupt the length field and checksum suffix), the B3b/B4 shape:
// the frame no longer even parses, so its checksum bytes are unrecoverable.
void smashFrame(std::vector<std::uint8_t>& buf, const ChunkRecord& rec) {
    buf[rec.offset + kOffPayloadLen] = 0xFF;
    buf[rec.offset + kOffPayloadLen + 1] = 0xFF;
    buf[rec.offset + kOffPayloadLen + 2] = 0xFF;
    for (std::size_t i = 0; i < 8; ++i)
        buf[rec.payloadOffset + rec.payloadLen + i] ^= 0xA5;
}

} // namespace

TEST_CASE("mosaic journal: the chain survives many sequential appends (the reset-bug stress)") {
    // Re-seeding the link on every call instead of threading it is invisible to a single-append
    // test and silently discards every autosave after the first (spec 2.6). Thirty states, four
    // syncs interleaved, replayed to the last byte.
    const auto dir = testDir("mosaic_journal_stress");
    const auto file = buildCheckpoint(baseInput());
    const OpenReport open = openDocument(file);
    REQUIRE(open.tipValid);
    REQUIRE(open.tip.commitId == kBaseGeneration);

    const std::string jpath = (dir / "stress.journal").string();
    auto writer = JournalWriter::create(jpath, bindingFor(open.tip));
    REQUIRE(writer.has_value());

    Plan plan;
    for (std::uint64_t id = 11; id <= 40; ++id) {
        plan.emplace_back(id, static_cast<std::uint32_t>(id % 6));
        REQUIRE(journalAppendState(*writer, makeState(id, static_cast<std::uint32_t>(id % 6))));
        if (id % 8 == 0)
            REQUIRE(writer->sync()); // coalescing merges fsyncs, not bytes (spec 2.6 cadence)
    }
    REQUIRE(writer->sync());

    const auto journal = readFileBytes(jpath);
    const JournalReplay replay = replayJournal(journal, bindingFor(open.tip));
    CHECK(replay.binding == JournalBindingStatus::Ok);
    CHECK(!replay.anomaly);
    REQUIRE(replay.states.size() == 30);
    for (std::size_t i = 0; i < replay.states.size(); ++i)
        CHECK(replay.states[i] == 11 + i);
    CHECK(mismatchedKeys({&open.base.chunks, &replay.chunks}, truthAfter(plan)).empty());
    fs::remove_all(dir);
}

TEST_CASE("mosaic journal: a torn tail yields exactly the last complete state (R11 A2)") {
    const auto dir = testDir("mosaic_journal_torn");
    const auto file = buildCheckpoint(baseInput());
    const OpenReport open = openDocument(file);

    const std::string jpath = (dir / "torn.journal").string();
    auto writer = JournalWriter::create(jpath, bindingFor(open.tip));
    REQUIRE(writer.has_value());
    const Plan plan = {{11, 0}, {12, 1}, {13, 2}};
    for (const auto& [id, k] : plan)
        REQUIRE(journalAppendState(*writer, makeState(id, k)));
    REQUIRE(writer->sync());

    auto journal = readFileBytes(jpath);
    const auto recs = scanChunks(journal);
    const ChunkRecord* tile13 = findFrame(recs, kTypeTile, 13);
    REQUIRE(tile13 != nullptr);
    journal.resize(tile13->offset + 40); // the crash tears state 13's tile frame in half

    const JournalReplay replay = replayJournal(journal, bindingFor(open.tip));
    CHECK(replay.binding == JournalBindingStatus::Ok);
    CHECK(replay.anomaly);
    CHECK(replay.states == std::vector<std::uint64_t>{11, 12});
    const Plan applied = {{11, 0}, {12, 1}}; // state 13 gone entirely -- never half-applied
    CHECK(mismatchedKeys({&open.base.chunks, &replay.chunks}, truthAfter(applied)).empty());
    fs::remove_all(dir);
}

TEST_CASE("mosaic journal: binding rejects a stale journal -- each defense layer separately") {
    const auto dir = testDir("mosaic_journal_binding");

    // Two saves of "the same document": the re-save carries an edit, and -- the load-bearing
    // part -- a HIGHER generation, because states consume ids (spec 2.2). The generation in the
    // root JSON is what guarantees two different saves can never share a root checksum; content
    // alone could compress to identical sizes and leave every offset in the root unchanged.
    auto inputV2 = baseInput();
    inputV2.chunks[0].payload = tileContent(99, 0);
    inputV2.generation = 12;
    const auto fileV1 = buildCheckpoint(baseInput());
    const auto fileV2 = buildCheckpoint(inputV2);
    const OpenReport openV1 = openDocument(fileV1);
    const OpenReport openV2 = openDocument(fileV2);
    REQUIRE(openV1.tip.checksum != openV2.tip.checksum);

    const std::string jpath = (dir / "stale.journal").string();
    auto writer = JournalWriter::create(jpath, bindingFor(openV1.tip));
    REQUIRE(writer.has_value());
    REQUIRE(journalAppendState(*writer, makeState(11, 0)));
    REQUIRE(writer->sync());
    const auto journal = readFileBytes(jpath);

    // The genuine stale case (R11 A3a): bound to v1, the file is now v2. The chain seed is the
    // bound commit's checksum, so this fails STRUCTURALLY -- by the math, before any policy
    // field is consulted -- and salvage refuses identically (never salvaged into the wrong doc).
    const JournalReplay stale = replayJournal(journal, bindingFor(openV2.tip));
    CHECK(stale.binding == JournalBindingStatus::WrongSeed);
    CHECK(stale.states.empty());
    CHECK(stale.chunks.empty());
    CHECK(salvageJournal(journal, bindingFor(openV2.tip)).binding ==
          JournalBindingStatus::WrongSeed);

    // Layer 2 in isolation (R11 A3b): a JHDR whose SEED matches but whose policy fields lie --
    // craftable only by a buggy or malicious writer, which is exactly why the layer exists.
    // Hand-build a journal seeded from v2 whose JSON claims a different uuid.
    std::vector<std::uint8_t> forged;
    const auto seed = bindingFor(openV2.tip).linkSeed();
    const std::string lying = std::string("{\"uuid\":\"someone-else\",\"bound_commit\":") +
                              std::to_string(openV2.tip.commitId) + ",\"bound_checksum\":\"00\"}";
    appendChunk(forged, kTypeJournalHeader, zeroKey(), openV2.tip.commitId,
                {reinterpret_cast<const std::uint8_t*>(lying.data()), lying.size()},
                Profile::Store, kFlagCritical, &seed);
    const JournalReplay forgedReplay = replayJournal(forged, bindingFor(openV2.tip));
    CHECK(forgedReplay.binding == JournalBindingStatus::WrongBinding);

    // No header at all.
    const std::vector<std::uint8_t> garbage(256, 0x5A);
    CHECK(replayJournal(garbage, bindingFor(openV1.tip)).binding ==
          JournalBindingStatus::NoHeader);
    fs::remove_all(dir);
}

TEST_CASE("mosaic journal: parity-repaired checkpoint + journal replay compose (R11 A4)") {
    const auto dir = testDir("mosaic_journal_parity");
    const auto clean = buildCheckpoint(baseInput());
    const OpenReport openClean = openDocument(clean);

    const std::string jpath = (dir / "compose.journal").string();
    auto writer = JournalWriter::create(jpath, bindingFor(openClean.tip));
    REQUIRE(writer.has_value());
    const Plan plan = {{11, 0}, {12, 3}};
    for (const auto& [id, k] : plan)
        REQUIRE(journalAppendState(*writer, makeState(id, k)));
    REQUIRE(writer->sync());
    const auto journal = readFileBytes(jpath);

    // Damage a parity-covered tile the journal does NOT touch; both mechanisms must fire.
    auto corrupted = clean;
    const auto corruptedRecs = scanChunks(corrupted);
    const ChunkRecord* victim = nullptr;
    for (const ChunkRecord& r : corruptedRecs)
        if (r.valid && r.type == kTypeTile && r.key == keyOf(4))
            victim = &r;
    REQUIRE(victim != nullptr);
    corrupted[victim->payloadOffset + 5] ^= 0xFF;

    const ReadReport repaired = readCheckpoint(corrupted);
    CHECK(repaired.rsReconstructed >= 1);
    const JournalReplay replay = replayJournal(journal, bindingFor(openClean.tip));
    CHECK(replay.binding == JournalBindingStatus::Ok);
    CHECK(replay.states == std::vector<std::uint64_t>{11, 12});
    CHECK(mismatchedKeys({&repaired.chunks, &replay.chunks}, truthAfter(plan)).empty());
    fs::remove_all(dir);
}

TEST_CASE("mosaic journal: lifecycle -- reset rebinds after Save, discard removes on clean close") {
    const auto dir = testDir("mosaic_journal_lifecycle");
    auto file = buildCheckpoint(baseInput());
    OpenReport open = openDocument(file);
    CommitTip tip = open.tip;

    const std::string jpath = (dir / "lifecycle.journal").string();
    auto writer = JournalWriter::create(jpath, bindingFor(tip));
    REQUIRE(writer.has_value());
    REQUIRE(journalAppendState(*writer, makeState(11, 0)));
    REQUIRE(journalAppendState(*writer, makeState(12, 1)));
    REQUIRE(writer->sync());
    const JournalBinding preSave = bindingFor(tip);

    // File->Save commits states 11-12; ONLY THEN does the journal reset, rebound to the CMIT.
    const std::vector<SaveState> batch = {makeState(11, 0), makeState(12, 1)};
    appendSaveBatch(file, tip, batch);
    REQUIRE(writer->reset(bindingFor(tip)));

    REQUIRE(journalAppendState(*writer, makeState(13, 2)));
    REQUIRE(writer->sync());
    const auto journal = readFileBytes(jpath);

    // The reset journal replays against the new commit -- and only against it.
    const JournalReplay fresh = replayJournal(journal, bindingFor(tip));
    CHECK(fresh.binding == JournalBindingStatus::Ok);
    CHECK(fresh.states == std::vector<std::uint64_t>{13});
    CHECK(replayJournal(journal, preSave).binding == JournalBindingStatus::WrongSeed);

    // Clean close deletes the journal: "close without saving" genuinely discards (spec 2.6).
    REQUIRE(writer->discard());
    CHECK(!fs::exists(jpath));
    fs::remove_all(dir);
}

// POSIX-only: setenv/unsetenv do not exist in the MSVC-flavoured CRT mingw targets, and the case is
// about $XDG_STATE_HOME specifically -- the Windows path resolves through LOCALAPPDATA /
// FOLDERID_LocalAppData instead, which has no environment override worth pinning (S57).
#if !defined(_WIN32)
TEST_CASE("mosaic journal: recovery path shape -- uuid + path hash under the state dir") {
    const auto dir = testDir("mosaic_journal_xdg");
    // setenv (not putenv+strdup -- the LSan trap tests/lsan.supp documents).
    REQUIRE(setenv("XDG_STATE_HOME", dir.string().c_str(), 1) == 0);

    const std::string a = recoveryJournalPath("uuid-a", "/home/user/art/piece.mosaic");
    const std::string b = recoveryJournalPath("uuid-a", "/home/user/other/piece.mosaic");
    const std::string untitled = recoveryJournalPath("uuid-a", "");
    CHECK(a.find((dir / "mosaic" / "recovery").string()) == 0);
    CHECK(a != b);        // a COPIED document gets its own journal, not a fight over one
    CHECK(a != untitled); // crash protection exists before a path does
    CHECK(a == recoveryJournalPath("uuid-a", "/home/user/art/piece.mosaic")); // stable

    // An untitled document's journal round-trips like any other.
    const auto file = buildCheckpoint(baseInput());
    const OpenReport open = openDocument(file);
    auto writer = JournalWriter::create(untitled, bindingFor(open.tip));
    REQUIRE(writer.has_value());
    REQUIRE(journalAppendState(*writer, makeState(11, 5)));
    REQUIRE(writer->sync());
    const JournalReplay replay = replayJournal(readFileBytes(untitled), bindingFor(open.tip));
    CHECK(replay.binding == JournalBindingStatus::Ok);
    CHECK(replay.states == std::vector<std::uint64_t>{11});
    REQUIRE(writer->discard());
    unsetenv("XDG_STATE_HOME");
    fs::remove_all(dir);
}
#endif // !_WIN32

TEST_CASE("mosaic journal: salvage honesty battery (R11 B)") {
    const auto dir = testDir("mosaic_journal_salvage");
    const auto file = buildCheckpoint(baseInput());
    const OpenReport open = openDocument(file);
    const JournalBinding binding = bindingFor(open.tip);

    // States 11-15 touch keys 0-4; state 16 touches key 5, which NO other state touches: after
    // losing state 16, key 5 holds pre-edit content -- the exact stale-content hazard a
    // salvager has to be honest about. States 17-22 touch keys 0-4 again.
    Plan plan;
    for (std::uint64_t id = 11; id <= 15; ++id)
        plan.emplace_back(id, static_cast<std::uint32_t>(id - 11));
    plan.emplace_back(16, 5);
    for (std::uint64_t id = 17; id <= 22; ++id)
        plan.emplace_back(id, static_cast<std::uint32_t>((id - 17) % 5));
    const auto truth = truthAfter(plan);

    const std::string jpath = (dir / "salvage.journal").string();
    auto writer = JournalWriter::create(jpath, binding);
    REQUIRE(writer.has_value());
    for (const auto& [id, k] : plan)
        REQUIRE(journalAppendState(*writer, makeState(id, k)));
    REQUIRE(writer->sync());
    const auto journal = readFileBytes(jpath);
    const auto recs = scanChunks(journal);

    std::vector<std::uint64_t> allStates;
    for (std::uint64_t id = 11; id <= 22; ++id)
        allStates.push_back(id);

    SUBCASE("B0 gate: the undamaged journal salvages to exactly the replay result") {
        const JournalSalvage s = salvageJournal(journal, binding);
        REQUIRE(s.binding == JournalBindingStatus::Ok);
        REQUIRE(s.report.lineages.size() == 1);
        const SalvageLineage& ln = s.report.lineages[0];
        CHECK(ln.seedRooted);
        CHECK(ln.precise);
        CHECK(ln.flagged.empty());
        CHECK(s.report.gaps == 0);
        CHECK(!s.report.rootConflict);
        CHECK(ln.states == allStates);
        CHECK(mismatchedKeys({&open.base.chunks, &ln.chunks}, truth).empty());
    }

    SUBCASE("B1: conservative replay stops at the damage and discards 6 intact states") {
        auto flipped = journal;
        const ChunkRecord* tile16 = findFrame(recs, kTypeTile, 16);
        REQUIRE(tile16 != nullptr);
        flipped[tile16->payloadOffset + 3] ^= 0xFF;
        const JournalReplay replay = replayJournal(flipped, binding);
        CHECK(replay.anomaly);
        CHECK(replay.states == std::vector<std::uint64_t>{11, 12, 13, 14, 15});
    }

    SUBCASE("B2 regression: the naive apply-what-parses scanner commits silent wrong data") {
        // A suite that cannot detect silently-wrong salvage output cannot protect the honesty
        // rules. This IS the naive scanner -- best generation wins, damage skipped, no chain,
        // no flags -- and it must produce a "successful" document with wrong content.
        auto flipped = journal;
        const ChunkRecord* tile16 = findFrame(recs, kTypeTile, 16);
        REQUIRE(tile16 != nullptr);
        flipped[tile16->payloadOffset + 3] ^= 0xFF;
        std::vector<RecoveredChunk> naive;
        for (const ChunkRecord& r : scanChunks(flipped)) {
            if (!r.valid || r.type != kTypeTile)
                continue;
            auto payload = decodeChunkPayload(r, flipped);
            if (!payload.has_value())
                continue;
            naive.push_back(RecoveredChunk{r.type, r.key, r.generation, r.flags,
                                           std::move(*payload)});
        }
        const auto wrong = mismatchedKeys({&open.base.chunks, &naive}, truth);
        CHECK(wrong == std::vector<std::uint32_t>{5}); // key 5 is stale, and NOTHING said so
    }

    SUBCASE("B4: structural damage -- salvage recovers all states with the exact key flagged") {
        auto smashed = journal;
        const ChunkRecord* tile16 = findFrame(recs, kTypeTile, 16);
        REQUIRE(tile16 != nullptr);
        smashFrame(smashed, *tile16);
        const JournalSalvage s = salvageJournal(smashed, binding);
        REQUIRE(s.binding == JournalBindingStatus::Ok);
        REQUIRE(s.report.lineages.size() == 1); // the gap BRIDGES: same writer, not a fork
        const SalvageLineage& ln = s.report.lineages[0];
        CHECK(ln.states == allStates); // state 16 counts, applied-with-flag
        CHECK(ln.precise);             // its HIST survived: the dirty list is the authority
        CHECK(ln.bridgedGap);
        CHECK(s.report.gaps >= 1); // the gap is REPORTED, never silent
        REQUIRE(ln.flagged.size() == 1);
        CHECK(ln.flagged[0].type == kTypeTile);
        CHECK(ln.flagged[0].key == keyOf(5));
        // The honesty identity: flagged keys == mismatching keys, exactly (R11 B4).
        CHECK(mismatchedKeys({&open.base.chunks, &ln.chunks}, truth) ==
              std::vector<std::uint32_t>{5});
    }

    SUBCASE("B4b: the HIST itself destroyed -- declared-imprecise, no unflagged wrong data") {
        auto smashed = journal;
        const ChunkRecord* hist16 = findFrame(recs, kTypeHist, 16);
        REQUIRE(hist16 != nullptr);
        smashFrame(smashed, *hist16);
        const JournalSalvage s = salvageJournal(smashed, binding);
        REQUIRE(s.binding == JournalBindingStatus::Ok);
        REQUIRE(s.report.lineages.size() == 1);
        const SalvageLineage& ln = s.report.lineages[0];
        CHECK(!ln.precise); // the dirty list is unknown: the flags are only a lower bound
        for (const std::uint64_t id : ln.states)
            CHECK(id != 16);
        // Every mismatching key must be flagged (the lower bound covers the truth here).
        const auto wrong = mismatchedKeys({&open.base.chunks, &ln.chunks}, truth);
        for (const std::uint32_t k : wrong) {
            bool flagged = false;
            for (const DirtyKey& d : ln.flagged)
                flagged = flagged || (d.type == kTypeTile && d.key == keyOf(k));
            CHECK(flagged);
        }
    }

    SUBCASE("B5: a torn tail under strict replay stops at the last complete state (parity)") {
        const ChunkRecord* tile16 = findFrame(recs, kTypeTile, 16);
        REQUIRE(tile16 != nullptr);
        std::vector<std::uint8_t> torn(journal.begin(),
                                       journal.begin() +
                                           static_cast<std::ptrdiff_t>(tile16->offset + 40));
        const JournalReplay replay = replayJournal(torn, binding);
        CHECK(replay.anomaly);
        CHECK(replay.states == std::vector<std::uint64_t>{11, 12, 13, 14, 15});
        // Salvage of the same tear: nothing wrong, nothing half-applied.
        const JournalSalvage s = salvageJournal(torn, binding);
        REQUIRE(s.report.primary() != nullptr);
        const auto wrong = mismatchedKeys({&open.base.chunks, &s.report.primary()->chunks},
                                          truthAfter(Plan(plan.begin(), plan.begin() + 5)));
        CHECK(wrong.empty());
    }

    SUBCASE("B6: reordered frames fragment the chain -- detected, never silently accepted") {
        const ChunkRecord* tileA = findFrame(recs, kTypeTile, 13);
        const ChunkRecord* tileB = findFrame(recs, kTypeTile, 19);
        REQUIRE(tileA != nullptr);
        REQUIRE(tileB != nullptr);
        // Splice-swap the two complete frames (sizes differ -- content differs).
        std::vector<std::uint8_t> reordered;
        const auto append = [&](std::size_t from, std::size_t len) {
            reordered.insert(reordered.end(), journal.begin() + static_cast<std::ptrdiff_t>(from),
                             journal.begin() + static_cast<std::ptrdiff_t>(from + len));
        };
        append(0, tileA->offset);
        append(tileB->offset, tileB->consumed);
        append(tileA->offset + tileA->consumed,
               tileB->offset - (tileA->offset + tileA->consumed));
        append(tileA->offset, tileA->consumed);
        append(tileB->offset + tileB->consumed, journal.size() - tileB->offset - tileB->consumed);

        const JournalReplay replay = replayJournal(reordered, binding);
        CHECK(replay.anomaly); // strict replay: link mismatch, conservative stop
        CHECK(replay.states == std::vector<std::uint64_t>{11, 12});
        const JournalSalvage s = salvageJournal(reordered, binding);
        REQUIRE(s.binding == JournalBindingStatus::Ok);
        CHECK(s.report.lineages.size() > 1); // the chain fragments: reordering is visible
        REQUIRE(s.report.primary() != nullptr);
        CHECK(s.report.primary()->states == std::vector<std::uint64_t>{11, 12});
    }
    fs::remove_all(dir);
}
