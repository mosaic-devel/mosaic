#include "io/mosaic/compaction.hpp"

#include "io/mosaic/records.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// History-preserving compaction (S48 Build 1; spec 2.6 full write, 3.3 copy-through). The
// deliberate scope cut of the commit-append slice: a plain full write keeps only the newest state
// and would drop every retained undo state the appended saves accumulated. What is pinned here is
// that folding the file preserves the history AS HISTORY -- not merely as bytes on disk, but as
// states that reconstruct to exactly what they reconstructed to before the fold -- while the
// generation contract that makes index-free recovery work stays intact.
namespace {

using namespace mosaic::io::native;

constexpr std::uint64_t kLayer = 3;
constexpr std::uint64_t kBaseGeneration = 10; // states consume ids ABOVE this (spec 2.2)
constexpr std::uint32_t kKeys = 6;
constexpr const char* kUuid = "compaction-test-uuid";

ChunkKey keyOf(std::uint32_t i) {
    return tileKey(kLayer, i, 0);
}

std::vector<std::uint8_t> tileContent(std::uint64_t stateId, std::uint32_t keyIdx) {
    std::vector<std::uint8_t> p(1200 + keyIdx * 37);
    for (std::size_t i = 0; i < p.size(); ++i)
        p[i] = static_cast<std::uint8_t>(stateId * 31 + keyIdx * 11 + i * 5);
    return p;
}

CheckpointInput baseInput() {
    CheckpointInput in;
    in.documentUuid = kUuid;
    in.generation = kBaseGeneration;
    for (std::uint32_t i = 0; i < kKeys; ++i)
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

// (state id, key touched) for every save in file order.
using Plan = std::vector<std::pair<std::uint64_t, std::uint32_t>>;

// Every frame an opened file offers, wherever it lives: current content, retained history, or the
// committed append region. This is the whole point of the exercise -- after compaction the frames
// move between those lists, and nothing about the DOCUMENT's history may change.
std::vector<const RecoveredChunk*> allFrames(const OpenReport& r) {
    std::vector<const RecoveredChunk*> out;
    for (const auto* list : {&r.base.chunks, &r.base.retained, &r.committed})
        for (const RecoveredChunk& c : *list)
            out.push_back(&c);
    return out;
}

// The document as of state `at`: for each key, the newest frame no newer than `at` (spec 2.2,
// "highest generation wins"). This is exactly how a reader resolves any point in the history.
std::map<std::uint32_t, std::vector<std::uint8_t>> reconstructAt(const OpenReport& r,
                                                                 std::uint64_t at) {
    std::map<std::uint32_t, std::vector<std::uint8_t>> doc;
    for (std::uint32_t k = 0; k < kKeys; ++k) {
        const RecoveredChunk* best = nullptr;
        for (const RecoveredChunk* c : allFrames(r))
            if (c->type == kTypeTile && c->key == keyOf(k) && c->generation <= at &&
                (best == nullptr || c->generation > best->generation))
                best = c;
        if (best != nullptr)
            doc[k] = best->payload;
    }
    return doc;
}

// The analytic truth: what the plan says the document held after state `at`.
std::map<std::uint32_t, std::vector<std::uint8_t>> truthAt(const Plan& plan, std::uint64_t at) {
    std::map<std::uint32_t, std::vector<std::uint8_t>> t;
    for (std::uint32_t i = 0; i < kKeys; ++i)
        t[i] = tileContent(kBaseGeneration, i);
    for (const auto& [id, k] : plan)
        if (id <= at)
            t[k] = tileContent(id, k);
    return t;
}

// The state ids the file's HIST records describe, and what each says it dirtied.
std::map<std::uint64_t, std::vector<DirtyKey>> historyRecords(const OpenReport& r) {
    std::map<std::uint64_t, std::vector<DirtyKey>> out;
    for (const RecoveredChunk* c : allFrames(r))
        if (c->type == kTypeHist)
            if (const auto rec = parseHistRecord(c->payload))
                out[rec->state] = rec->dirty;
    return out;
}

// Build a file with `saves` appended commits, round-robin over the keys.
struct Fixture {
    std::vector<std::uint8_t> file;
    Plan plan;
    std::uint64_t lastState = kBaseGeneration;
};

Fixture buildWithSaves(int saves) {
    Fixture f;
    f.file = buildCheckpoint(baseInput());
    CommitTip tip = openDocument(f.file).tip;
    for (int i = 1; i <= saves; ++i) {
        const std::uint64_t id = kBaseGeneration + static_cast<std::uint64_t>(i);
        const auto k = static_cast<std::uint32_t>(i % kKeys);
        f.plan.emplace_back(id, k);
        appendSaveBatch(f.file, tip, std::vector<SaveState>{makeState(id, k)});
        f.lastState = id;
    }
    return f;
}

} // namespace

TEST_CASE("mosaic compaction: the folded file reconstructs EVERY state exactly as before") {
    // The decisive test. Ten appended saves, then a fold with an eleventh edit riding along. Every
    // point in the history -- the base state, each committed state, and the state the compaction
    // itself commits -- must resolve to the same document from the folded file as from the file it
    // replaced. If copy-through loses a superseded frame, or the generation rule slips by one,
    // exactly one of these positions goes wrong.
    Fixture f = buildWithSaves(10);
    const OpenReport before = openDocument(f.file);
    REQUIRE(before.commits.size() == 10);
    REQUIRE(before.base.retained.empty()); // an un-compacted file keeps its history in the region

    // The compaction's own edit: repaint key 2 at the next generation.
    const std::uint64_t newId = before.tip.commitId + 1;
    std::vector<StateChunk> edit{{kTypeTile, keyOf(2), tileContent(newId, 2), kFlagCritical}};

    std::string err;
    const auto compacted = buildCompactedCheckpoint(f.file, before, edit, &err);
    REQUIRE_MESSAGE(compacted.has_value(), err);
    CHECK(compacted->generation == newId);
    CHECK(compacted->reEncodedChunks == 0); // nothing was parity-rebuilt: everything copied through

    const OpenReport after = openDocument(compacted->bytes);
    REQUIRE(after.base.rootFound);
    CHECK(after.committed.empty()); // the append region was folded away...
    CHECK(after.commits.empty());
    CHECK(!after.base.retained.empty()); // ...and its history now lives in the checkpoint

    Plan fullPlan = f.plan;
    fullPlan.emplace_back(newId, 2);

    // Every state, both directions: the folded file agrees with the original file AND with the
    // analytic truth. The base state (below every save) is included -- it is the undo target of
    // save 1, and it is exactly the frame a naive full write would have thrown away.
    for (std::uint64_t s = kBaseGeneration; s <= newId; ++s) {
        CAPTURE(s);
        const auto want = truthAt(fullPlan, s);
        if (s < newId)
            CHECK(reconstructAt(before, s) == want); // the original file, as a control
        CHECK(reconstructAt(after, s) == want);
    }

    // The HIST records survive too -- a state's content is useless without its dirty list, which
    // is what tells the undo model which keys a step touched.
    const auto records = historyRecords(after);
    CHECK(records.size() == 11); // 10 committed states + the one this compaction committed
    for (const auto& [id, k] : fullPlan) {
        CAPTURE(id);
        REQUIRE(records.count(id) == 1);
        REQUIRE(records.at(id).size() == 1);
        CHECK(records.at(id)[0].type == kTypeTile);
        CHECK(records.at(id)[0].key == keyOf(k));
    }
}

TEST_CASE("mosaic compaction: the generation contract holds across the fold") {
    // "Only states consume ids; the checkpoint generation is the newest retained state id"
    // (spec 2.2). Round 12's A5 proved this load-bearing: a state that reuses the checkpoint's
    // generation makes highest-generation-wins TIE, and recovery silently returns stale content.
    Fixture f = buildWithSaves(4);
    const OpenReport before = openDocument(f.file);
    REQUIRE(before.tip.commitId == f.lastState);

    // A compaction that commits an edit takes the next id...
    const std::vector<StateChunk> edit{
        {kTypeTile, keyOf(1), tileContent(f.lastState + 1, 1), kFlagCritical}};
    const auto withEdit = buildCompactedCheckpoint(f.file, before, edit);
    REQUIRE(withEdit.has_value());
    CHECK(withEdit->generation == f.lastState + 1);

    // ...and a pure parity refresh, with nothing to commit, advances nothing.
    const auto noEdit = buildCompactedCheckpoint(f.file, before, {});
    REQUIRE(noEdit.has_value());
    CHECK(noEdit->generation == f.lastState);
    CHECK(openDocument(noEdit->bytes).tip.commitId == f.lastState);
    // Nothing to commit means no new state, so no new HIST record either.
    CHECK(historyRecords(openDocument(noEdit->bytes)).size() == 4);

    // The reopened tip binds to the new root, so the NEXT Save takes the id after it -- never the
    // checkpoint's own. Append one and confirm the newest content is the appended state's, not the
    // checkpoint's frame of the same key.
    std::vector<std::uint8_t> file = withEdit->bytes;
    OpenReport reopened = openDocument(file);
    CommitTip tip = reopened.tip;
    CHECK(tip.commitId == withEdit->generation);
    const std::uint64_t next = tip.commitId + 1;
    appendSaveBatch(file, tip, std::vector<SaveState>{makeState(next, 1)});

    const OpenReport final = openDocument(file);
    CHECK(final.commits == std::vector<std::uint64_t>{next});
    const RecoveredChunk* got = final.find(kTypeTile, keyOf(1));
    REQUIRE(got != nullptr);
    CHECK(got->generation == next);
    CHECK(got->payload == tileContent(next, 1)); // the appended state, never the folded one
}

TEST_CASE("mosaic compaction: copy-through is byte-verbatim, and parity covers current content") {
    Fixture f = buildWithSaves(6);
    const OpenReport before = openDocument(f.file);

    // A superseded frame's exact bytes, from the file about to be folded.
    std::vector<std::uint8_t> sampleHistory;
    for (const ChunkRecord& r : scanChunks(f.file))
        if (r.valid && r.type == kTypeTile && r.generation == kBaseGeneration + 1) {
            sampleHistory.assign(f.file.begin() + static_cast<std::ptrdiff_t>(r.offset),
                                 f.file.begin() +
                                     static_cast<std::ptrdiff_t>(r.offset + r.consumed));
            break;
        }
    REQUIRE(!sampleHistory.empty());

    const auto compacted = buildCompactedCheckpoint(f.file, before, {});
    REQUIRE(compacted.has_value());
    CHECK(compacted->currentChunks == kKeys);
    CHECK(compacted->retainedChunks == 12); // 6 superseded tiles + 6 HIST records

    // Spec 3.3: a state's content is compressed exactly once, at the Save that committed it. The
    // frame rides into the new file untouched.
    CHECK(std::search(compacted->bytes.begin(), compacted->bytes.end(), sampleHistory.begin(),
                      sampleHistory.end()) != compacted->bytes.end());

    // And through a SECOND fold, still untouched -- history does not decay by being carried.
    const OpenReport once = openDocument(compacted->bytes);
    const auto twice = buildCompactedCheckpoint(compacted->bytes, once, {});
    REQUIRE(twice.has_value());
    CHECK(twice->reEncodedChunks == 0);
    CHECK(twice->retainedChunks == compacted->retainedChunks);
    CHECK(std::search(twice->bytes.begin(), twice->bytes.end(), sampleHistory.begin(),
                      sampleHistory.end()) != twice->bytes.end());

    // The compaction's real deliverable: parity covers current content again (the append region
    // never had any). Damage a current tile -- it comes back, exactly.
    auto damaged = compacted->bytes;
    bool hit = false;
    for (const ChunkRecord& r : scanChunks(damaged))
        if (!hit && r.valid && r.type == kTypeTile && r.generation == kBaseGeneration + 6) {
            damaged[r.payloadOffset + 3] ^= 0xFF; // the newest frame of key 0
            hit = true;
        }
    REQUIRE(hit);
    const ReadReport repaired = readCheckpoint(damaged);
    CHECK(repaired.rsReconstructed == 1);
    CHECK(repaired.lostEntries == 0);

    // History carries no parity by design (spec 3.8): damaging a retained frame is an honest loss,
    // counted apart from content so it never raises the "this file is damaged" face.
    auto histDamaged = compacted->bytes;
    hit = false;
    for (const ChunkRecord& r : scanChunks(histDamaged))
        if (!hit && r.valid && r.type == kTypeHist) {
            histDamaged[r.payloadOffset + 2] ^= 0xFF;
            hit = true;
        }
    REQUIRE(hit);
    const ReadReport histLost = readCheckpoint(histDamaged);
    CHECK(histLost.rsReconstructed == 0);
    CHECK(histLost.lostEntries == 0);
    CHECK(histLost.lostHistoryEntries == 1);
}

TEST_CASE("mosaic compaction: a parity-rebuilt frame is re-encoded, not copied back damaged") {
    Fixture f = buildWithSaves(3);

    // Damage a CHECKPOINT tile that parity covers. The reader rebuilds it in memory; its bytes on
    // disk are still wrong, so copy-through must decline them and re-encode from the payload --
    // which is how the repair becomes permanent instead of riding the damage into the new file.
    bool hit = false;
    for (const ChunkRecord& r : scanChunks(f.file))
        if (!hit && r.valid && r.type == kTypeTile && r.generation == kBaseGeneration &&
            r.key == keyOf(4)) {
            f.file[r.payloadOffset + 7] ^= 0xFF;
            hit = true;
        }
    REQUIRE(hit);

    const OpenReport before = openDocument(f.file);
    REQUIRE(before.base.rsReconstructed == 1);
    REQUIRE(before.base.lostEntries == 0);

    const auto compacted = buildCompactedCheckpoint(f.file, before, {});
    REQUIRE(compacted.has_value());
    CHECK(compacted->reEncodedChunks == 1); // exactly the rebuilt frame, nothing else

    // The folded file is clean: no repair needed on the way in, and the content is right.
    const OpenReport after = openDocument(compacted->bytes);
    CHECK(after.base.rsReconstructed == 0);
    CHECK(after.base.lostEntries == 0);
    CHECK(after.base.lostHistoryEntries == 0);
    const RecoveredChunk* fixed = after.find(kTypeTile, keyOf(4));
    REQUIRE(fixed != nullptr);
    CHECK(fixed->payload == tileContent(kBaseGeneration, 4));
}

TEST_CASE("mosaic compaction: the threshold trips, the fold clears the debt") {
    // needsCompaction is parity-debt driven (spec 2.6): the append region carries no Reed-Solomon
    // coverage until a full write folds it in. The ratio is squeezed here for the tiny synthetic
    // batches -- the 128KB root slot dominates this checkpoint -- since what is under test is that
    // the fold actually clears what the trigger measures.
    constexpr double kRatio = 0.02;
    Fixture f;
    f.file = buildCheckpoint(baseInput());
    OpenReport open = openDocument(f.file);
    const std::uint64_t walStart = open.base.walStartOffset;
    CommitTip tip = open.tip;
    CHECK(!needsCompaction(f.file.size(), walStart, kRatio)); // a fresh checkpoint owes nothing

    std::uint64_t id = kBaseGeneration;
    int saves = 0;
    while (!needsCompaction(f.file.size(), walStart, kRatio) && saves < 200) {
        ++id;
        ++saves;
        appendSaveBatch(f.file, tip, std::vector<SaveState>{makeState(id, id % kKeys)});
    }
    REQUIRE(needsCompaction(f.file.size(), walStart, kRatio));
    REQUIRE(saves > 1); // the trigger must not fire on the very first Save

    const auto compacted = buildCompactedCheckpoint(f.file, openDocument(f.file), {});
    REQUIRE(compacted.has_value());
    const OpenReport after = openDocument(compacted->bytes);
    CHECK(!needsCompaction(compacted->bytes.size(), after.base.walStartOffset, kRatio));
    // The debt is cleared by folding, not by discarding: every state is still there.
    CHECK(historyRecords(after).size() == static_cast<std::size_t>(saves));
}

TEST_CASE("mosaic compaction: a file with no verified root is declined, never invented") {
    Fixture f = buildWithSaves(2);
    for (const ChunkRecord& r : scanChunks(f.file))
        if (r.valid && r.type == kTypeRoot)
            f.file[r.payloadOffset + 2] ^= 0xFF;

    const OpenReport open = openDocument(f.file);
    REQUIRE(!open.base.rootFound);
    REQUIRE(open.base.usedFullScan);

    std::string err;
    CHECK(!buildCompactedCheckpoint(f.file, open, {}, &err).has_value());
    CHECK(!err.empty());
}
