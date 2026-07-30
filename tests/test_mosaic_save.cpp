#include "io/mosaic/save.hpp"

#include "io/mosaic/compaction.hpp"
#include "io/mosaic/journal.hpp"
#include "io/mosaic/salvage.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// Commit-append Save + the full write (S48 Build 1, Save-path slice; spec 2.6, 2.8, 3.3) --
// Round 12's A/B batteries and Round 13's dual-writer D-checks re-run against the real
// implementation: multi-Save round-trips, torn-Save atomicity, torn-Save+journal composing to
// zero loss, commit-granularity binding staleness, committed-region salvage honesty, the
// full-scan fallback with the load-bearing generation rule, threshold compaction with parity
// restoration and copy-through identity, the O(1) pre-Save tail check, and the D2
// lineage-blending hazard as a regression test.
namespace {

namespace fs = std::filesystem;
using namespace mosaic::io::native;

constexpr std::uint64_t kLayer = 7;
constexpr std::uint64_t kBaseGeneration = 10; // states consume ids ABOVE this (spec 2.2)
constexpr const char* kUuid = "save-test-uuid";

ChunkKey keyOf(std::uint32_t i) {
    return tileKey(kLayer, i, 0);
}

std::vector<std::uint8_t> tileContent(std::uint64_t stateId, std::uint32_t keyIdx) {
    std::vector<std::uint8_t> p(1500 + keyIdx * 31);
    for (std::size_t i = 0; i < p.size(); ++i)
        p[i] = static_cast<std::uint8_t>(stateId * 29 + keyIdx * 7 + i * 3);
    return p;
}

CheckpointInput baseInput(std::uint32_t keys = 6) {
    CheckpointInput in;
    in.documentUuid = kUuid;
    in.generation = kBaseGeneration;
    for (std::uint32_t i = 0; i < keys; ++i)
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

using Plan = std::vector<std::pair<std::uint64_t, std::uint32_t>>;

std::map<std::uint32_t, std::vector<std::uint8_t>> truthAfter(const Plan& plan,
                                                              std::uint32_t keys = 6) {
    std::map<std::uint32_t, std::vector<std::uint8_t>> t;
    for (std::uint32_t i = 0; i < keys; ++i)
        t[i] = tileContent(kBaseGeneration, i);
    for (const auto& [id, k] : plan)
        t[k] = tileContent(id, k);
    return t;
}

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

void writeFileBytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

const ChunkRecord* findFrame(const std::vector<ChunkRecord>& recs, const ChunkTag& t,
                             std::uint64_t generation) {
    for (const ChunkRecord& r : recs)
        if (r.valid && r.type == t && r.generation == generation)
            return &r;
    return nullptr;
}

// The standard three-Save fixture: batches [11,12] [13,14] [15,16]; state 13 touches key 5,
// which no other state re-touches (the honest-staleness probe, R12 A4).
struct Fixture {
    std::vector<std::uint8_t> base;
    OpenReport open0;
    Plan plan;
    std::vector<std::vector<SaveState>> batches;
    std::vector<std::vector<std::uint8_t>> afterSave; // file image after save i
    std::vector<CommitTip> tips;                      // tip after save i
};

Fixture makeFixture() {
    Fixture f;
    f.base = buildCheckpoint(baseInput());
    f.open0 = openDocument(f.base);
    REQUIRE(f.open0.tipValid);
    f.plan = {{11, 0}, {12, 1}, {13, 5}, {14, 2}, {15, 3}, {16, 4}};
    std::vector<SaveState> states;
    for (const auto& [id, k] : f.plan)
        states.push_back(makeState(id, k));
    f.batches = {{states[0], states[1]}, {states[2], states[3]}, {states[4], states[5]}};

    std::vector<std::uint8_t> file = f.base;
    CommitTip tip = f.open0.tip;
    for (const auto& batch : f.batches) {
        appendSaveBatch(file, tip, batch);
        f.afterSave.push_back(file);
        f.tips.push_back(tip);
    }
    return f;
}

} // namespace

TEST_CASE("mosaic save: commit-appended Saves round-trip, and the file tip survives many (A0)") {
    const Fixture f = makeFixture();
    const OpenReport open = openDocument(f.afterSave[2]);
    CHECK(open.base.rootFound);
    CHECK(open.commits == std::vector<std::uint64_t>{12, 14, 16});
    CHECK(!open.committedAnomaly);
    CHECK(open.tipValid);
    CHECK(open.tip.commitId == 16);
    CHECK(open.tip.committedEnd == f.afterSave[2].size());
    CHECK(mismatchedKeys({&open.base.chunks, &open.committed}, truthAfter(f.plan)).empty());

    // The chain-reset stress, file edition (spec 2.6): twenty more single-state Saves, each
    // continuing the IN-MEMORY tip. A tip re-derived (or re-seeded) per Save survives one
    // append and silently orphans the rest.
    std::vector<std::uint8_t> file = f.afterSave[2];
    CommitTip tip = open.tip;
    Plan longPlan = f.plan;
    for (std::uint64_t id = 17; id <= 36; ++id) {
        const auto k = static_cast<std::uint32_t>(id % 6);
        appendSaveBatch(file, tip, std::vector<SaveState>{makeState(id, k)});
        longPlan.emplace_back(id, k);
    }
    const OpenReport again = openDocument(file);
    CHECK(again.commits.size() == 23);
    CHECK(again.tip.commitId == 36);
    CHECK(mismatchedKeys({&again.base.chunks, &again.committed}, truthAfter(longPlan)).empty());
}

TEST_CASE("mosaic save: a torn Save opens at the previous commit -- batch atomicity (A1)") {
    const Fixture f = makeFixture();
    // The crash lands mid-batch-3: wherever the tear falls inside the batch, no CMIT means no
    // states -- never a half-saved hybrid.
    std::vector<std::uint8_t> torn(
        f.afterSave[2].begin(),
        f.afterSave[2].begin() + static_cast<std::ptrdiff_t>(f.afterSave[1].size() + 90));
    const OpenReport open = openDocument(torn);
    CHECK(open.commits == std::vector<std::uint64_t>{12, 14});
    CHECK(open.committedAnomaly);
    CHECK(open.tip.commitId == 14);
    CHECK(open.tip.committedEnd == f.afterSave[1].size()); // the dead tail is known, bounded
    const Plan committed(f.plan.begin(), f.plan.begin() + 4);
    CHECK(mismatchedKeys({&open.base.chunks, &open.committed}, truthAfter(committed)).empty());
}

TEST_CASE("mosaic save: torn Save + intact journal = zero loss; success = stale journal (A2/A3)") {
    const auto dir = testDir("mosaic_save_journal");
    const Fixture f = makeFixture();

    // The journal held states 15-16 when the third Save tore: reset happens only AFTER the
    // Save's bytes are durable (spec 2.6), so the crash window keeps both.
    const std::string jpath = (dir / "crash.journal").string();
    auto writer = JournalWriter::create(jpath, bindingFor(f.tips[1]));
    REQUIRE(writer.has_value());
    for (const SaveState& s : f.batches[2]) {
        for (const StateChunk& c : s.chunks)
            REQUIRE(writer->append(c.type, c.key, s.stateId, c.payload, Profile::Fast, c.flags));
        const std::string hist = histPayloadFor(s);
        REQUIRE(writer->append(kTypeHist, histKey(s.stateId), s.stateId,
                               {reinterpret_cast<const std::uint8_t*>(hist.data()), hist.size()},
                               Profile::Store));
    }
    REQUIRE(writer->sync());
    const auto journal = readFileBytes(jpath);

    std::vector<std::uint8_t> torn(
        f.afterSave[2].begin(),
        f.afterSave[2].begin() + static_cast<std::ptrdiff_t>(f.afterSave[1].size() + 90));
    const OpenReport open = openDocument(torn);
    const JournalReplay replay = replayJournal(journal, bindingFor(f.tips[1]));
    CHECK(replay.binding == JournalBindingStatus::Ok);
    CHECK(replay.states == std::vector<std::uint64_t>{15, 16});
    CHECK(mismatchedKeys({&open.base.chunks, &open.committed, &replay.chunks},
                         truthAfter(f.plan))
              .empty()); // nothing lost

    // A3: had the Save SUCCEEDED, that same journal is stale against the new commit --
    // structurally, at commit granularity.
    CHECK(replayJournal(journal, bindingFor(f.tips[2])).binding ==
          JournalBindingStatus::WrongSeed);
    fs::remove_all(dir);
}

TEST_CASE("mosaic save: damage in an old batch -- conservative stop, honest salvage (A4)") {
    const Fixture f = makeFixture();
    auto damaged = f.afterSave[2];
    const auto recs = scanChunks(damaged);
    const ChunkRecord* tile13 = findFrame(recs, kTypeTile, 13);
    REQUIRE(tile13 != nullptr);
    damaged[tile13->payloadOffset + 20] ^= 0xFF;

    // Conservative replay: everything from the damaged batch on is discarded (previous commit).
    const OpenReport open = openDocument(damaged);
    CHECK(open.commits == std::vector<std::uint64_t>{12});
    CHECK(open.committedAnomaly);
    const Plan firstBatch(f.plan.begin(), f.plan.begin() + 2);
    CHECK(mismatchedKeys({&open.base.chunks, &open.committed}, truthAfter(firstBatch)).empty());

    // Salvage recovers the later commits with the lost state's key flagged EXACTLY (R11
    // machinery in the file's committed region).
    const SalvageReport s =
        salvageLinkedRegion(damaged, open.base.walStartOffset, f.open0.tip.link());
    CHECK(s.gaps >= 1); // the damage is REPORTED, never silent
    const SalvageLineage* primary = s.primary();
    REQUIRE(primary != nullptr);
    CHECK(primary->states == std::vector<std::uint64_t>{11, 12, 13, 14, 15, 16});
    CHECK(primary->precise);
    CHECK(primary->bridgedGap);
    REQUIRE(primary->flagged.size() == 1);
    CHECK(primary->flagged[0].key == keyOf(5));
    CHECK(mismatchedKeys({&open.base.chunks, &primary->chunks}, truthAfter(f.plan)) ==
          std::vector<std::uint32_t>{5}); // flagged == mismatching, exactly
}

TEST_CASE("mosaic save: full-scan reconstructs appended files; the generation rule is load-bearing (A5)") {
    // A 7th key exists only in the checkpoint -- the tie probe.
    auto in = baseInput(7);
    auto file = buildCheckpoint(in);
    OpenReport open0 = openDocument(file);
    CommitTip tip = open0.tip;
    const Plan plan = {{11, 0}, {12, 1}, {13, 5}, {14, 2}, {15, 3}, {16, 4}};
    for (const auto& [id, k] : plan)
        appendSaveBatch(file, tip, std::vector<SaveState>{makeState(id, k)});

    // Destroy every ROOT replica: no index, no walStartOffset -- self-description only.
    for (const ChunkRecord& rec : scanChunks(file))
        if (rec.valid && rec.type == kTypeRoot)
            file[rec.payloadOffset + 2] ^= 0xFF;
    const ReadReport scan = readCheckpoint(file);
    CHECK(scan.usedFullScan);
    CHECK(mismatchedKeys({&scan.chunks}, truthAfter(plan, 7)).empty());

    // The tie: a frame reusing the CHECKPOINT'S generation for key 6 (violating spec 2.2's
    // "only states consume ids; checkpoint generation = newest retained state id"). Highest-
    // generation-wins cannot prefer it -- the foreign edit is silently shadowed, which is
    // exactly why the rule is load-bearing for index-free recovery (R12 A5's live failure).
    appendChunk(file, kTypeTile, keyOf(6), kBaseGeneration, tileContent(99, 6),
                Profile::Balanced);
    const ReadReport tied = readCheckpoint(file);
    const RecoveredChunk* key6 = newest({&tied.chunks}, kTypeTile, keyOf(6));
    REQUIRE(key6 != nullptr);
    CHECK(key6->payload == tileContent(kBaseGeneration, 6)); // the checkpoint's version, still

    // A writer that follows the rule (id above the checkpoint generation) wins cleanly.
    appendChunk(file, kTypeTile, keyOf(6), 17, tileContent(17, 6), Profile::Balanced);
    const ReadReport ruled = readCheckpoint(file);
    const RecoveredChunk* key6b = newest({&ruled.chunks}, kTypeTile, keyOf(6));
    REQUIRE(key6b != nullptr);
    CHECK(key6b->payload == tileContent(17, 6));
}

TEST_CASE("mosaic save: dual writers -- determinism, no blending, both sides recoverable (D)") {
    const auto base = buildCheckpoint(baseInput());
    const OpenReport open0 = openDocument(base);
    const auto seed = open0.tip.link();

    // Two instances open the same commit and diverge; both continue from the same last state id
    // (the realistic collision), touching different tiles.
    const std::vector<SaveState> statesA = {makeState(21, 0), makeState(22, 1)};
    const std::vector<SaveState> statesB = {makeState(21, 2), makeState(22, 3)};
    const Plan planA = {{21, 0}, {22, 1}};
    const auto truthA = truthAfter(planA);
    const auto truthB = truthAfter({{21, 2}, {22, 3}});

    std::vector<std::uint8_t> fileAB = base;
    CommitTip tipA = open0.tip;
    appendSaveBatch(fileAB, tipA, statesA);
    CommitTip tipB = open0.tip; // B's stale view -- it never saw A's Save
    appendSaveBatch(fileAB, tipB, statesB);

    SUBCASE("D1: conservative replay picks one deterministic lineage; the loser is inert") {
        const OpenReport open = openDocument(fileAB);
        CHECK(open.commits == std::vector<std::uint64_t>{22});
        CHECK(open.committedAnomaly); // the dead bytes are noticed, not misread
        CHECK(mismatchedKeys({&open.base.chunks, &open.committed}, truthA).empty());
        // B's keys hold base content -- never blended in.
        const RecoveredChunk* k2 = open.find(kTypeTile, keyOf(2));
        REQUIRE(k2 != nullptr);
        CHECK(k2->payload == tileContent(kBaseGeneration, 2));
    }

    SUBCASE("D2/D3: lineage salvage -- primary exact, foreign reported, NEVER merged") {
        const SalvageReport s =
            salvageLinkedRegion(fileAB, open0.base.walStartOffset, seed);
        REQUIRE(s.lineages.size() == 2);
        CHECK(s.rootConflict); // two chains claim the same seed: a true dual-writer conflict
        const SalvageLineage* primary = s.primary();
        REQUIRE(primary != nullptr);
        CHECK(primary->states == std::vector<std::uint64_t>{21, 22});
        CHECK(primary->flagged.empty());
        CHECK(mismatchedKeys({&open0.base.chunks, &primary->chunks}, truthA).empty());
        // THE D2 REGRESSION: a salvager that blends lineages puts B's tiles into the primary.
        for (const RecoveredChunk& c : primary->chunks) {
            CHECK(!(c.key == keyOf(2)));
            CHECK(!(c.key == keyOf(3)));
        }
        const SalvageLineage& foreign =
            &s.lineages[0] == primary ? s.lineages[1] : s.lineages[0];
        CHECK(foreign.seedRooted); // it forked from the same commit
        CHECK(foreign.states == std::vector<std::uint64_t>{21, 22});
        CHECK(newest({&foreign.chunks}, kTypeTile, keyOf(2)) != nullptr);
        CHECK(mismatchedKeys({&open0.base.chunks, &foreign.chunks}, truthB).empty());
    }

    SUBCASE("D5: the frame-interleaved race -- replay degrades safely; salvage recovers both") {
        const SaveBatch batchA = buildSaveBatch(seed, statesA);
        const SaveBatch batchB = buildSaveBatch(seed, statesB);
        const auto framesA = scanChunks(batchA.bytes);
        const auto framesB = scanChunks(batchB.bytes);
        std::vector<std::uint8_t> raced = base;
        const auto splice = [&](const std::vector<std::uint8_t>& src, const ChunkRecord& r) {
            raced.insert(raced.end(), src.begin() + static_cast<std::ptrdiff_t>(r.offset),
                         src.begin() + static_cast<std::ptrdiff_t>(r.offset + r.consumed));
        };
        for (std::size_t i = 0; i < std::max(framesA.size(), framesB.size()); ++i) {
            if (i < framesA.size())
                splice(batchA.bytes, framesA[i]);
            if (i < framesB.size())
                splice(batchB.bytes, framesB[i]);
        }

        const OpenReport open = openDocument(raced);
        CHECK(open.commits.empty()); // the last pre-race commit: base content, never wrong
        CHECK(open.committedAnomaly);
        CHECK(mismatchedKeys({&open.base.chunks, &open.committed}, truthAfter({})).empty());

        const SalvageReport s = salvageLinkedRegion(raced, open0.base.walStartOffset, seed);
        CHECK(s.rootConflict);
        std::size_t seedRooted = 0;
        bool sawA = false, sawB = false;
        for (const SalvageLineage& ln : s.lineages) {
            if (!ln.seedRooted)
                continue;
            ++seedRooted;
            CHECK(ln.states == std::vector<std::uint64_t>{21, 22});
            sawA = sawA ||
                   mismatchedKeys({&open0.base.chunks, &ln.chunks}, truthA).empty();
            sawB = sawB ||
                   mismatchedKeys({&open0.base.chunks, &ln.chunks}, truthB).empty();
        }
        CHECK(seedRooted == 2);
        CHECK(sawA); // both racers' batches, complete, separate -- link-following beats
        CHECK(sawB); // file order (D5b); surfaced as a root conflict, never auto-merged
    }
}

TEST_CASE("mosaic save: the pre-Save tail check refuses every foreign change loudly (D4)") {
    const auto dir = testDir("mosaic_save_tail");
    const std::string path = (dir / "doc.mosaic").string();
    const auto base = buildCheckpoint(baseInput());
    std::string error;
    REQUIRE(writeFileAtomic(path, base, &error));

    OpenReport open = openDocument(readFileBytes(path));
    CommitTip tip = open.tip;
    REQUIRE(open.tipValid);
    REQUIRE(stampTipIdentity(path, tip, &error));
    const CommitTip tipBefore = tip;

    const std::vector<SaveState> batch1 = {makeState(11, 0), makeState(12, 1)};
    REQUIRE(appendSaveToFile(path, tip, batch1, &error) == SaveStatus::Ok);
    CHECK(verifyTail(path, tip) == SaveStatus::Ok); // D4a: the file the writer left behind
    const auto snapshot = readFileBytes(path);

    // The saved file round-trips end-to-end through the real filesystem.
    const OpenReport reopened = openDocument(snapshot);
    CHECK(reopened.commits == std::vector<std::uint64_t>{12});
    CHECK(mismatchedKeys({&reopened.base.chunks, &reopened.committed},
                         truthAfter({{11, 0}, {12, 1}}))
              .empty());

    // D4d: a stale expectation (from before the Save) fails against the newer file.
    CHECK(verifyTail(path, tipBefore) == SaveStatus::TailSizeChanged);

    // D4b: a foreign append since open.
    {
        std::ofstream f(path, std::ios::binary | std::ios::app);
        const std::vector<std::uint8_t> junk(64, 0x77);
        f.write(reinterpret_cast<const char*>(junk.data()), 64);
    }
    CHECK(verifyTail(path, tip) == SaveStatus::TailSizeChanged);
    fs::resize_file(path, tip.fileSize);
    CHECK(verifyTail(path, tip) == SaveStatus::Ok);

    // D4c: truncation since open.
    fs::resize_file(path, tip.fileSize - 30);
    CHECK(verifyTail(path, tip) == SaveStatus::TailSizeChanged);
    // A refused Save leaves the file exactly as it found it.
    CommitTip tipCopy = tip;
    CHECK(appendSaveToFile(path, tipCopy, batch1, &error) == SaveStatus::TailSizeChanged);
    CHECK(readFileBytes(path).size() == tip.fileSize - 30);
    writeFileBytes(path, snapshot); // in-place restore keeps the inode

    // Inode replacement with byte-identical content (a foreign compaction under a stale
    // handle): size and checksum match; only the identity check can catch it.
    REQUIRE(writeFileAtomic(path, snapshot, &error)); // rename = new inode
    CHECK(verifyTail(path, tip) == SaveStatus::TailIdentityChanged);
    REQUIRE(stampTipIdentity(path, tip, &error)); // re-adopt, as an explicit user choice would
    CHECK(verifyTail(path, tip) == SaveStatus::Ok);

    // Same-size in-place corruption of the last commit's stored checksum.
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(-1, std::ios::end);
        f.put('\x00');
    }
    CHECK(verifyTail(path, tip) == SaveStatus::TailCommitMismatch);
    writeFileBytes(path, snapshot);
    CHECK(verifyTail(path, tip) == SaveStatus::Ok);
    fs::remove_all(dir);
}

TEST_CASE("mosaic save: the next Save truncates a torn batch's dead tail (file-level A2 shape)") {
    const auto dir = testDir("mosaic_save_torn_tail");
    const std::string path = (dir / "doc.mosaic").string();
    const auto base = buildCheckpoint(baseInput());
    std::string error;
    REQUIRE(writeFileAtomic(path, base, &error));

    OpenReport open = openDocument(readFileBytes(path));
    CommitTip tip = open.tip;
    REQUIRE(stampTipIdentity(path, tip, &error));
    const std::vector<SaveState> batch1 = {makeState(11, 0), makeState(12, 1)};
    const std::vector<SaveState> batch2 = {makeState(13, 2), makeState(14, 3)};
    REQUIRE(appendSaveToFile(path, tip, batch1, &error) == SaveStatus::Ok);

    // A crash tears the second Save halfway through its batch.
    {
        const SaveBatch torn = buildSaveBatch(tip.link(), batch2);
        std::ofstream f(path, std::ios::binary | std::ios::app);
        f.write(reinterpret_cast<const char*>(torn.bytes.data()),
                static_cast<std::streamsize>(torn.bytes.size() / 2));
    }

    // Reopen: the previous commit, a known dead tail, and a tip pointing before it.
    const auto bytes = readFileBytes(path);
    OpenReport crashed = openDocument(bytes);
    CHECK(crashed.commits == std::vector<std::uint64_t>{12});
    CHECK(crashed.committedAnomaly);
    CHECK(crashed.tip.committedEnd < crashed.tip.fileSize);
    CommitTip tip2 = crashed.tip;
    REQUIRE(stampTipIdentity(path, tip2, &error));

    // The retried Save truncates the dead bytes and lands cleanly.
    REQUIRE(appendSaveToFile(path, tip2, batch2, &error) == SaveStatus::Ok);
    const OpenReport final_ = openDocument(readFileBytes(path));
    CHECK(final_.commits == std::vector<std::uint64_t>{12, 14});
    CHECK(!final_.committedAnomaly);
    CHECK(mismatchedKeys({&final_.base.chunks, &final_.committed},
                         truthAfter({{11, 0}, {12, 1}, {13, 2}, {14, 3}}))
              .empty());
    fs::remove_all(dir);
}

TEST_CASE("mosaic save: threshold compaction -- the Save that folds the file, and the journal "
          "rebinding onto it (R12 B + spec 3.3)") {
    // The mechanics of the fold itself (state-by-state reconstruction, copy-through identity, the
    // generation contract, parity restoration) are pinned in test_mosaic_compaction.cpp against
    // buildCompactedCheckpoint. What matters HERE is the Save-path integration: the trigger folds
    // a full write into an ordinary Save, and the recovery journal must rebind onto the new root
    // -- its old binding is structurally stale the instant the checkpoint is replaced.
    const auto dir = testDir("mosaic_save_compaction");
    auto file = buildCheckpoint(baseInput());
    OpenReport open = openDocument(file);
    CommitTip tip = open.tip;
    const std::uint64_t walStart = open.base.walStartOffset;
    const CommitTip preCompactionTip = tip;

    // Single-state Saves until the parity-debt threshold trips (never a background job). The
    // ratio is squeezed for the test's tiny synthetic batches -- the 128KB root slot dominates
    // this checkpoint, and what is under test is the trigger folding a full write into a Save,
    // not the shipping constant.
    std::uint64_t stateId = kBaseGeneration;
    int compactedAtSave = -1;
    Plan plan;
    for (int saveN = 1; saveN <= 60; ++saveN) {
        if (needsCompaction(file.size(), walStart, 0.02)) {
            compactedAtSave = saveN;
            break;
        }
        ++stateId;
        const auto k = static_cast<std::uint32_t>(stateId % 6);
        plan.emplace_back(stateId, k);
        appendSaveBatch(file, tip, std::vector<SaveState>{makeState(stateId, k)});
    }
    REQUIRE(compactedAtSave > 1);

    // THIS Save performs the full write instead of appending -- and it carries an edit of its own,
    // exactly as a real Ctrl+S does. The edit takes the next state id; everything it supersedes
    // becomes retained history.
    const OpenReport atThreshold = openDocument(file);
    const std::uint64_t compactionState = atThreshold.tip.commitId + 1;
    const std::vector<StateChunk> edit{
        {kTypeTile, keyOf(5), tileContent(compactionState, 5), kFlagCritical}};
    plan.emplace_back(compactionState, 5u);

    std::string cerr;
    const auto compacted = buildCompactedCheckpoint(file, atThreshold, edit, &cerr);
    REQUIRE_MESSAGE(compacted.has_value(), cerr);
    CHECK(compacted->generation == compactionState);

    // The document is what every save (including this one) made it, and the append region is gone.
    const OpenReport reopened = openDocument(compacted->bytes);
    CHECK(reopened.base.rootFound);
    CHECK(reopened.committed.empty());
    CHECK(mismatchedKeys({&reopened.base.chunks}, truthAfter(plan)).empty());
    CHECK(!needsCompaction(compacted->bytes.size(), reopened.base.walStartOffset, 0.02));

    // Verify-then-copy (spec 3.3, load-bearing): a damaged frame is CAUGHT at Save time and never
    // propagated into a fresh file -- the corruption stops here rather than being blessed by a new
    // checksum over bad bytes.
    const ChunkRecord* anyHist = nullptr;
    const auto recs = scanChunks(compacted->bytes);
    for (const ChunkRecord& r : recs)
        if (r.valid && r.type == kTypeHist) {
            anyHist = &r;
            break;
        }
    REQUIRE(anyHist != nullptr);
    std::vector<std::uint8_t> corrupt(
        compacted->bytes.begin() + static_cast<std::ptrdiff_t>(anyHist->offset),
        compacted->bytes.begin() + static_cast<std::ptrdiff_t>(anyHist->offset + anyHist->consumed));
    REQUIRE(makeVerbatimChunk(corrupt, /*parity=*/false, /*history=*/true).has_value());
    corrupt[kHeaderSize + 2] ^= 0xFF;
    CHECK(!makeVerbatimChunk(corrupt, /*parity=*/false, /*history=*/true).has_value());

    // The journal rebinds to the new root; the pre-compaction binding is structurally stale.
    const std::string jpath = (dir / "rebind.journal").string();
    auto writer = JournalWriter::create(jpath, bindingFor(reopened.tip));
    REQUIRE(writer.has_value());
    const SaveState next = makeState(compactionState + 1, 0);
    for (const StateChunk& c : next.chunks)
        REQUIRE(writer->append(c.type, c.key, next.stateId, c.payload));
    const std::string hist = histPayloadFor(next);
    REQUIRE(writer->append(kTypeHist, histKey(next.stateId), next.stateId,
                           {reinterpret_cast<const std::uint8_t*>(hist.data()), hist.size()}));
    REQUIRE(writer->sync());
    const auto journal = readFileBytes(jpath);
    CHECK(replayJournal(journal, bindingFor(reopened.tip)).binding ==
          JournalBindingStatus::Ok);
    CHECK(replayJournal(journal, bindingFor(preCompactionTip)).binding ==
          JournalBindingStatus::WrongSeed);
    fs::remove_all(dir);
}

TEST_CASE("mosaic save: a container from the future is never replayed, and never folded") {
    // openDocument must not walk an append region whose framing it may not understand, and
    // compaction must not rewrite a file it cannot read as something it is not.
    CheckpointInput in = baseInput();
    in.formatVersion = kFormatVersion + 1;
    const auto file = buildCheckpoint(in);

    const OpenReport open = openDocument(file);
    CHECK(open.base.unsupportedVersion);
    CHECK(open.base.formatVersion == kFormatVersion + 1);
    CHECK(open.committed.empty());
    CHECK(open.commits.empty());
    CHECK(!open.committedAnomaly); // "the file ends where it ends" -- not an anomaly, just unread
    CHECK(!open.tipValid);         // nothing to append onto: a Save here would be a full write

    std::string err;
    CHECK(!buildCompactedCheckpoint(file, open, {}, &err).has_value());
    CHECK(err.find("newer Mosaic") != std::string::npos);
}

TEST_CASE("mosaic save: needsCompaction edges") {
    CHECK(!needsCompaction(0, 0));
    CHECK(!needsCompaction(1000, 1000));   // no appended region at all
    CHECK(!needsCompaction(1400, 1000));   // under the default 0.5 debt ratio
    CHECK(needsCompaction(1501, 1000));    // past it
    CHECK(!needsCompaction(500, 1000));    // shorter than the checkpoint: nothing to fold
    CHECK(needsCompaction(1200, 1000, 0.1));
}
