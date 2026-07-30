#include "io/mosaic/compaction.hpp"

#include "io/mosaic/blob.hpp"
#include "io/mosaic/records.hpp"
#include "ui/loaded_history.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// H4 content-addressed history + adaptive H2<->H4 switching (S48 Build 2; spec 3.9). What is
// pinned here, in order of importance:
//
//   1. A wrong mode choice is NEVER a correctness issue -- every state reconstructs identically
//      from either encoding, through the reader's own resolution, including across H2->H4->H2
//      switch chains with commit-append saves in between.
//   2. Dedup is real and honest: shared content is stored once, content equal to a current frame
//      is not stored at all, and damage to a shared blob is counted ONCE, by identity, as a
//      history loss that declines the whole walk (never a silently partial history).
//   3. The churn signal measures the WHOLE retained history -- the window is the retention
//      horizon -- with the research's wrong-window bug pinned as a regression, and the
//      asymmetric hysteresis holds both directions.
//   4. Parity posture: current-referenced content stays parity-covered; history-only blobs
//      carry none (spec 3.8), and the recovery ladder behaves identically over cas files.
namespace {

using namespace mosaic::io::native;
namespace mui = mosaic::ui;

constexpr std::uint64_t kLayer = 3;
constexpr std::uint64_t kBaseGeneration = 10; // states consume ids ABOVE this (spec 2.2)
constexpr std::uint32_t kKeys = 6;
constexpr const char* kUuid = "cas-test-uuid";
constexpr std::uint8_t kTileFlags = kFlagCritical | kFlagFiltered;

ChunkKey keyOf(std::uint32_t i) {
    return tileKey(kLayer, i, 0);
}

std::uint32_t keyIdxOf(const ChunkKey& k) {
    return static_cast<std::uint32_t>(k.bytes[8]) | (static_cast<std::uint32_t>(k.bytes[9]) << 8);
}

// Content by PALETTE id, not by state: reuse -- the same bytes painted again later, possibly
// under a different key -- is exactly what content-addressing exists to deduplicate.
std::vector<std::uint8_t> palette(std::uint32_t pid) {
    std::vector<std::uint8_t> p(1200 + (pid % 7) * 37);
    for (std::size_t i = 0; i < p.size(); ++i)
        p[i] = static_cast<std::uint8_t>(pid * 31 + i * 5);
    return p;
}

constexpr std::uint32_t kBasePalette = 1000; // base key i holds palette kBasePalette + i

CheckpointInput baseInput() {
    CheckpointInput in;
    in.documentUuid = kUuid;
    in.generation = kBaseGeneration;
    for (std::uint32_t i = 0; i < kKeys; ++i)
        in.chunks.push_back({kTypeTile, keyOf(i), kBaseGeneration, Profile::Balanced, kTileFlags,
                             /*parity=*/true, /*history=*/false, palette(kBasePalette + i)});
    return in;
}

SaveState makeState(std::uint64_t id, std::uint32_t keyIdx, std::uint32_t pid) {
    SaveState s;
    s.stateId = id;
    s.chunks.push_back({kTypeTile, keyOf(keyIdx), palette(pid), kTileFlags});
    return s;
}

// (state id, key touched, palette painted) for every save in file order.
struct Step {
    std::uint64_t id;
    std::uint32_t key;
    std::uint32_t pid;
};
using Plan = std::vector<Step>;

struct Fixture {
    std::vector<std::uint8_t> file;
    Plan plan;
    std::uint64_t lastState = kBaseGeneration;
};

Fixture buildWithPlan(const Plan& plan) {
    Fixture f;
    f.plan = plan;
    f.file = buildCheckpoint(baseInput());
    CommitTip tip = openDocument(f.file).tip;
    for (const Step& s : plan) {
        appendSaveBatch(f.file, tip, std::vector<SaveState>{makeState(s.id, s.key, s.pid)});
        f.lastState = s.id;
    }
    return f;
}

// A plan of `n` saves cycling `distinct` palettes round-robin over the keys, starting at
// state id `from` + 1 and palette base `pbase`.
Plan cyclingPlan(std::uint64_t from, int n, std::uint32_t pbase, std::uint32_t distinct) {
    Plan p;
    for (int i = 1; i <= n; ++i)
        p.push_back({from + static_cast<std::uint64_t>(i),
                     static_cast<std::uint32_t>(i) % kKeys,
                     pbase + (static_cast<std::uint32_t>(i) % distinct)});
    return p;
}

// The analytic truth: what the plan says the document held after state `at`.
std::map<std::uint32_t, std::vector<std::uint8_t>> truthAt(const Plan& plan, std::uint64_t at) {
    std::map<std::uint32_t, std::vector<std::uint8_t>> t;
    for (std::uint32_t i = 0; i < kKeys; ++i)
        t[i] = palette(kBasePalette + i);
    for (const Step& s : plan)
        if (s.id <= at)
            t[s.key] = palette(s.pid);
    return t;
}

// The document at state `at`, composed THROUGH THE READER'S OWN RESOLUTION (ui::loadedStates) --
// per-(KEY, generation) frames and content references alike. This is the walk a reopened History
// panel performs, so it is the equivalence that matters.
std::map<std::uint32_t, std::vector<std::uint8_t>> walkAt(const OpenReport& r, std::uint64_t at) {
    std::map<std::uint32_t, std::vector<std::uint8_t>> doc;
    const auto states = mui::loadedStates(r);
    REQUIRE(states.has_value());
    const std::uint64_t first =
        states->empty() ? at + 1 : states->front().generation;
    for (const auto* list : {&r.base.chunks, &r.base.retained, &r.committed})
        for (const RecoveredChunk& c : *list) {
            if (c.type != kTypeTile || c.generation >= first)
                continue;
            const std::uint32_t k = keyIdxOf(c.key);
            // Newest frame below the oldest state seeds each key -- the walk's own seeding rule.
            const auto it = doc.find(k);
            if (it == doc.end())
                doc[k] = c.payload;
        }
    // Frames below `first` are unique per key in these fixtures (one base frame each).
    for (const mui::LoadedState& st : *states) {
        if (st.generation > at)
            break;
        for (const mui::LoadedChunk& c : st.chunks) {
            CHECK((c.flags & kFlagFiltered) != 0); // interpretation flags survive resolution
            doc[keyIdxOf(c.key)] =
                std::vector<std::uint8_t>(c.payload.begin(), c.payload.end());
        }
    }
    return doc;
}

// Frames of one type in a file image, by scan (what is PHYSICALLY there, not what resolves).
std::vector<ChunkRecord> framesOfType(std::span<const std::uint8_t> file, const ChunkTag& t) {
    std::vector<ChunkRecord> out;
    for (const ChunkRecord& r : scanChunks(file))
        if (r.valid && r.type == t)
            out.push_back(r);
    return out;
}

// Distinct BLOB identities in a file (replicas of one hash collapse), for dedup counting.
std::set<std::array<std::uint8_t, 16>> blobIdentities(std::span<const std::uint8_t> file) {
    std::set<std::array<std::uint8_t, 16>> ids;
    for (const ChunkRecord& r : framesOfType(file, kTypeBlob))
        ids.insert(r.key.bytes);
    return ids;
}

// Fold with a forced mode -- the deterministic-encoding knob the correctness tests use;
// hysteresis has its own tests below.
CompactionResult foldAs(std::span<const std::uint8_t> file, const OpenReport& open,
                        std::span<const StateChunk> edit, const char* mode) {
    std::string err;
    CompactionOptions opts;
    opts.forceMode = mode;
    const auto folded = buildCompactedCheckpoint(file, open, edit, &err, opts);
    REQUIRE_MESSAGE(folded.has_value(), err);
    return *folded;
}

} // namespace

TEST_CASE("cas: every state reconstructs exactly as before, and duplicates are stored once") {
    // Twelve saves cycling THREE palettes (heavy reuse, the shape H4 exists for), then six
    // fresh saves so the cycled contents end up history-only. The fold must preserve every walk
    // position while physically storing each distinct content once.
    Plan plan = cyclingPlan(kBaseGeneration, 12, 2000, 3);
    for (const Step& s : cyclingPlan(kBaseGeneration + 12, 6, 2100, 6))
        plan.push_back(s);
    Fixture f = buildWithPlan(plan);
    const OpenReport before = openDocument(f.file);
    REQUIRE(before.commits.size() == 18);

    const CompactionResult folded = foldAs(f.file, before, {}, kModeCas);
    CHECK(folded.mode == kModeCas);
    CHECK(folded.blobChunks > 0);

    const OpenReport after = openDocument(folded.bytes);
    REQUIRE(after.base.rootFound);
    CHECK(after.base.mode == kModeCas); // the root records the encoding (spec 2.3)
    CHECK(after.committed.empty());
    CHECK(!after.base.retained.empty());

    for (std::uint64_t s = kBaseGeneration; s <= f.lastState; ++s) {
        CAPTURE(s);
        const auto want = truthAt(f.plan, s);
        CHECK(walkAt(before, s) == want); // the original file, as a control
        CHECK(walkAt(after, s) == want);
    }

    // Dedup is physical: a distinct HISTORY-ONLY content is stored exactly once, as one BLOB;
    // content still current is stored as its (parity-covered) current frame and NEVER duplicated
    // as a blob; superseded base palettes are seeds -- kept as frames, not blobs. Compute the
    // expected identity set from the plan rather than hand-waving it: every painted content that
    // matches no current frame.
    std::set<std::vector<std::uint8_t>> currentContents;
    for (std::uint32_t k = 0; k < kKeys; ++k)
        currentContents.insert(truthAt(f.plan, f.lastState).at(k));
    std::set<std::vector<std::uint8_t>> expectBlobs;
    for (const Step& s : f.plan)
        if (currentContents.count(palette(s.pid)) == 0)
            expectBlobs.insert(palette(s.pid));
    CHECK(expectBlobs.size() == 3); // the three cycled palettes, each referenced by 4 states
    CHECK(blobIdentities(folded.bytes).size() == expectBlobs.size());
    CHECK(folded.blobChunks == expectBlobs.size());

    // Every instance of a shared content resolves to ONE stored copy: physically, each distinct
    // non-current content appears exactly once among the BLOB frames.
    std::map<std::array<std::uint8_t, 16>, int> copies;
    for (const ChunkRecord& r : framesOfType(folded.bytes, kTypeBlob))
        ++copies[r.key.bytes];
    for (const auto& [id, n] : copies)
        CHECK(n == 1);

    // Retained HIST records carry references now (the cas spelling)...
    bool sawRefs = false;
    for (const RecoveredChunk& c : after.base.retained)
        if (c.type == kTypeHist)
            if (const auto rec = parseHistRecord(c.payload); rec.has_value() && !rec->refs.empty())
                sawRefs = true;
    CHECK(sawRefs);
}

TEST_CASE("cas: wrong mode is never a correctness issue -- both encodings walk identically") {
    // The invariant the whole adaptive design rests on (spec 3.9): encode the SAME history both
    // ways and every checkpoint round-trips regardless. The penalty of a wrong choice is file
    // size, full stop.
    Fixture f = buildWithPlan(cyclingPlan(kBaseGeneration, 10, 3000, 2));
    const OpenReport open = openDocument(f.file);

    const CompactionResult asJournal = foldAs(f.file, open, {}, kModeJournal);
    const CompactionResult asCas = foldAs(f.file, open, {}, kModeCas);
    CHECK(asJournal.mode == kModeJournal);
    CHECK(asCas.mode == kModeCas);
    CHECK(blobIdentities(asJournal.bytes).empty());

    const OpenReport j = openDocument(asJournal.bytes);
    const OpenReport c = openDocument(asCas.bytes);
    for (std::uint64_t s = kBaseGeneration; s <= f.lastState; ++s) {
        CAPTURE(s);
        const auto want = truthAt(f.plan, s);
        CHECK(walkAt(j, s) == want);
        CHECK(walkAt(c, s) == want);
    }

    // And the size claim itself, directionally: under real reuse the cas encoding is smaller.
    CHECK(asCas.bytes.size() < asJournal.bytes.size());
}

TEST_CASE("cas: H2 -> H4 -> H2 switch chains preserve the history through appended saves") {
    Fixture f = buildWithPlan(cyclingPlan(kBaseGeneration, 8, 4000, 2));

    // H2 -> H4.
    const CompactionResult toCas = foldAs(f.file, openDocument(f.file), {}, kModeCas);

    // Append six FRESH saves onto the cas file -- commit-append batches are mode-neutral
    // per-(KEY, generation) frames, and the two spellings must compose into one walk. Fresh
    // content on every key also pushes the cycled palettes out of the current set, so the next
    // fold genuinely stores them as blobs.
    std::vector<std::uint8_t> file = toCas.bytes;
    Plan grown = f.plan;
    {
        OpenReport reopened = openDocument(file);
        CommitTip tip = reopened.tip;
        for (int i = 1; i <= 6; ++i) {
            const Step s{f.lastState + static_cast<std::uint64_t>(i),
                         static_cast<std::uint32_t>(i) % kKeys,
                         4100 + static_cast<std::uint32_t>(i)};
            appendSaveBatch(file, tip, std::vector<SaveState>{makeState(s.id, s.key, s.pid)});
            grown.push_back(s);
        }
    }
    const OpenReport mixed = openDocument(file);
    CHECK(mixed.base.mode == kModeCas);
    CHECK(mixed.commits.size() == 6);
    for (std::uint64_t s = kBaseGeneration; s <= grown.back().id; ++s) {
        CAPTURE(s);
        CHECK(walkAt(mixed, s) == truthAt(grown, s));
    }

    // H4 -> H4 folds: the first stores the now-history-only cycled palettes as blobs; the second
    // splices those blob frames byte-verbatim (spec 3.3 encode-once).
    const CompactionResult casAgain = foldAs(file, mixed, {}, kModeCas);
    CHECK(blobIdentities(casAgain.bytes).size() == 2); // the two cycled palettes
    std::vector<std::uint8_t> sampleBlob;
    {
        const auto blobs = framesOfType(casAgain.bytes, kTypeBlob);
        REQUIRE(!blobs.empty());
        sampleBlob.assign(
            casAgain.bytes.begin() + static_cast<std::ptrdiff_t>(blobs[0].offset),
            casAgain.bytes.begin() +
                static_cast<std::ptrdiff_t>(blobs[0].offset + blobs[0].consumed));
    }
    const CompactionResult casThird =
        foldAs(casAgain.bytes, openDocument(casAgain.bytes), {}, kModeCas);
    CHECK(std::search(casThird.bytes.begin(), casThird.bytes.end(), sampleBlob.begin(),
                      sampleBlob.end()) != casThird.bytes.end());
    for (std::uint64_t s = kBaseGeneration; s <= grown.back().id; ++s) {
        CAPTURE(s);
        CHECK(walkAt(openDocument(casThird.bytes), s) == truthAt(grown, s));
    }

    // H4 -> H2: the switch back re-materializes per-(KEY, generation) frames; nothing may drift.
    const CompactionResult toJournal =
        foldAs(casThird.bytes, openDocument(casThird.bytes), {}, kModeJournal);
    const OpenReport back = openDocument(toJournal.bytes);
    CHECK(back.base.mode == kModeJournal);
    CHECK(blobIdentities(toJournal.bytes).empty()); // no blobs survive a journal fold
    for (std::uint64_t s = kBaseGeneration; s <= grown.back().id; ++s) {
        CAPTURE(s);
        CHECK(walkAt(back, s) == truthAt(grown, s));
    }
}

TEST_CASE("cas: blob damage is counted once by identity and declines the whole walk") {
    // Two states painting the SAME palette on DIFFERENT keys, then both keys repainted so the
    // shared content is history-only: exactly one BLOB holds it.
    Plan plan{{kBaseGeneration + 1, 1, 7000},
              {kBaseGeneration + 2, 2, 7000},
              {kBaseGeneration + 3, 1, 7001},
              {kBaseGeneration + 4, 2, 7002}};
    Fixture f = buildWithPlan(plan);
    const CompactionResult folded = foldAs(f.file, openDocument(f.file), {}, kModeCas);
    REQUIRE(blobIdentities(folded.bytes).size() == 1);

    // Damage the shared blob's payload.
    auto damaged = folded.bytes;
    const auto blobs = framesOfType(damaged, kTypeBlob);
    REQUIRE(blobs.size() == 1);
    damaged[blobs[0].payloadOffset + 4] ^= 0xFF;

    const OpenReport hurt = openDocument(damaged);
    // One loss, by identity -- two states referenced it, but ONE chunk rotted (spec 2.3's
    // counting rule) -- and it is a HISTORY loss: the document's content is byte-perfect, so the
    // "damaged file" face must not rise (history carries no parity, spec 3.8).
    CHECK(hurt.base.lostHistoryEntries == 1);
    CHECK(hurt.base.lostEntries == 0);
    CHECK(hurt.base.rsReconstructed == 0);
    for (std::uint32_t k = 0; k < kKeys; ++k) {
        const RecoveredChunk* cur = hurt.find(kTypeTile, keyOf(k));
        REQUIRE(cur != nullptr);
        CHECK(cur->payload == truthAt(plan, f.lastState).at(k));
    }
    // The walk declines WHOLE: a partial history would step over the gap into content some state
    // never held (the corpus-13 rule, unchanged in cas mode).
    CHECK_FALSE(mui::loadedStates(hurt).has_value());

    // And the fold refuses to launder the damage into a clean-looking file.
    std::string err;
    CHECK(!buildCompactedCheckpoint(damaged, hurt, {}, &err).has_value());
    CHECK(!err.empty());
}

TEST_CASE("cas: parity covers current-referenced content, never history-only blobs") {
    // One state's content stays CURRENT for its key while an older state references the same
    // bytes -- dedup against current content (spec 3.9): no blob is stored, the reference
    // resolves to the parity-covered current frame.
    Plan plan{{kBaseGeneration + 1, 1, 8000},  // painted...
              {kBaseGeneration + 2, 1, 8001},  // ...superseded...
              {kBaseGeneration + 3, 1, 8000}}; // ...and painted BACK: state 11 == current content
    Fixture f = buildWithPlan(plan);
    const CompactionResult folded = foldAs(f.file, openDocument(f.file), {}, kModeCas);
    // palette 8000 is current (key 1) -> not a blob; palette 8001 is history-only -> one blob.
    REQUIRE(blobIdentities(folded.bytes).size() == 1);

    // Damage the CURRENT frame of key 1. Parity rebuilds it -- and with it, state 11's
    // referenced content: the walk survives current-frame damage because current content is
    // parity-covered no matter how many history states reference it.
    auto damaged = folded.bytes;
    bool hit = false;
    for (const ChunkRecord& r : scanChunks(damaged))
        if (!hit && r.valid && r.type == kTypeTile && r.key == keyOf(1) &&
            r.generation == kBaseGeneration + 3) {
            damaged[r.payloadOffset + 2] ^= 0xFF;
            hit = true;
        }
    REQUIRE(hit);
    const OpenReport repaired = openDocument(damaged);
    CHECK(repaired.base.rsReconstructed == 1);
    CHECK(repaired.base.lostEntries == 0);
    CHECK(repaired.base.lostHistoryEntries == 0);
    const auto states = mui::loadedStates(repaired);
    REQUIRE(states.has_value());
    REQUIRE(states->size() == 3);
    for (std::uint64_t s = kBaseGeneration; s <= f.lastState; ++s) {
        CAPTURE(s);
        CHECK(walkAt(repaired, s) == truthAt(plan, s));
    }

    // The blob, by contrast, is deliberately parity-uncovered (spec 3.8): destroying it is an
    // honest, unrepaired history loss -- pinned in the blob-damage case above.
    const auto blobs = framesOfType(folded.bytes, kTypeBlob);
    REQUIRE(blobs.size() == 1);
    CHECK((blobs[0].flags & kFlagCritical) != 0);
}

TEST_CASE("cas: the recovery ladder behaves identically -- full scan reconstructs a cas file") {
    Fixture f = buildWithPlan(cyclingPlan(kBaseGeneration, 6, 9000, 2));
    const CompactionResult folded = foldAs(f.file, openDocument(f.file), {}, kModeCas);

    // Destroy every ROOT replica: the ladder's last rung (spec 2.8 full scan) must still
    // reassemble the newest content from self-describing chunks alone -- BLOB chunks ride along
    // without colliding with anything.
    auto damaged = folded.bytes;
    for (const ChunkRecord& r : scanChunks(damaged))
        if (r.valid && r.type == kTypeRoot)
            damaged[r.payloadOffset + 2] ^= 0xFF;
    const OpenReport scanned = openDocument(damaged);
    CHECK(!scanned.base.rootFound);
    CHECK(scanned.base.usedFullScan);
    const auto want = truthAt(f.plan, f.lastState);
    for (std::uint32_t k = 0; k < kKeys; ++k) {
        const RecoveredChunk* cur = scanned.find(kTypeTile, keyOf(k));
        REQUIRE(cur != nullptr);
        CHECK(cur->payload == want.at(k));
    }
    // A full-scan open has no separable history -- flow 3e's face; same verdict as journal mode.
    const auto states = mui::loadedStates(scanned);
    REQUIRE(states.has_value());
    CHECK(states->empty());
}

TEST_CASE("cas: PRVW rules are unaffected -- previews never become blobs or references") {
    // A preview in the base checkpoint and one written by a save: after a cas fold exactly one
    // PRVW remains (the newest, as current content), no blob holds preview bytes, and the
    // state's HIST entry for it carries no reference.
    Fixture f;
    {
        CheckpointInput in = baseInput();
        std::vector<std::uint8_t> pv(400, 0x11);
        in.chunks.push_back({kTypePreview, zeroKey(), kBaseGeneration, Profile::Balanced,
                             kFlagCritical, /*parity=*/false, /*history=*/false, pv});
        f.file = buildCheckpoint(in);
    }
    CommitTip tip = openDocument(f.file).tip;
    SaveState s = makeState(kBaseGeneration + 1, 1, 9500);
    s.chunks.push_back({kTypePreview, zeroKey(), std::vector<std::uint8_t>(420, 0x22),
                        kFlagCritical});
    appendSaveBatch(f.file, tip, std::vector<SaveState>{s});
    f.plan = {{kBaseGeneration + 1, 1, 9500}};
    f.lastState = kBaseGeneration + 1;

    const CompactionResult folded = foldAs(f.file, openDocument(f.file), {}, kModeCas);
    const auto previews = framesOfType(folded.bytes, kTypePreview);
    CHECK(previews.size() == 1); // the superseded one is DROPPED, not retained (S48-b rule)
    CHECK(previews[0].generation == kBaseGeneration + 1);
    for (const ChunkRecord& r : framesOfType(folded.bytes, kTypeBlob))
        CHECK(r.payloadLen != 420 + kBlobHashSize); // no blob of the dropped preview bytes

    const OpenReport after = openDocument(folded.bytes);
    for (const RecoveredChunk& c : after.base.retained)
        if (c.type == kTypeHist)
            if (const auto rec = parseHistRecord(c.payload); rec.has_value())
                for (std::size_t i = 0; i < rec->dirty.size(); ++i)
                    if (rec->dirty[i].type == kTypePreview)
                        CHECK(!(i < rec->refs.size() && rec->refs[i].present));
    const auto states = mui::loadedStates(after);
    REQUIRE(states.has_value());
    REQUIRE(states->size() == 1);
    CHECK(walkAt(after, f.lastState) == truthAt(f.plan, f.lastState));
}

TEST_CASE("adaptive: hysteresis is asymmetric and holds in the dead band") {
    // The thresholds themselves (spec 3.9, Round 9): up at 0.35, down at 0.15, and BOTH modes
    // hold inside (0.15, 0.35) -- that band existing is what stops thrash, each switch being a
    // whole-history re-encode.
    CHECK(chooseHistoryMode(kModeJournal, 0.00) == kModeJournal);
    CHECK(chooseHistoryMode(kModeJournal, 0.34) == kModeJournal);
    CHECK(chooseHistoryMode(kModeJournal, 0.35) == kModeCas);
    CHECK(chooseHistoryMode(kModeJournal, 0.90) == kModeCas);
    CHECK(chooseHistoryMode(kModeCas, 0.90) == kModeCas);
    CHECK(chooseHistoryMode(kModeCas, 0.35) == kModeCas);
    CHECK(chooseHistoryMode(kModeCas, 0.20) == kModeCas); // dead band: hold
    CHECK(chooseHistoryMode(kModeCas, 0.15) == kModeCas);
    CHECK(chooseHistoryMode(kModeCas, 0.14) == kModeJournal);
    CHECK(chooseHistoryMode(kModeJournal, 0.20) == kModeJournal); // dead band: hold
    CHECK(chooseHistoryMode("", 0.90) == kModeCas); // an unknown mode reads as journal
}

TEST_CASE("adaptive: the fold measures churn and switches by hysteresis") {
    // All-fresh content: churn 0 -> journal stays.
    {
        Plan fresh;
        for (int i = 1; i <= 8; ++i)
            fresh.push_back({kBaseGeneration + static_cast<std::uint64_t>(i),
                             static_cast<std::uint32_t>(i) % kKeys,
                             5000 + static_cast<std::uint32_t>(i)});
        Fixture f = buildWithPlan(fresh);
        std::string err;
        const auto folded = buildCompactedCheckpoint(f.file, openDocument(f.file), {}, &err);
        REQUIRE_MESSAGE(folded.has_value(), err);
        CHECK(folded->churnFraction == doctest::Approx(0.0));
        CHECK(folded->mode == kModeJournal);
        CHECK(openDocument(folded->bytes).base.mode == kModeJournal);
    }
    // Heavy reuse: churn far past switch-up -> the fold switches to cas on its own.
    {
        Fixture f = buildWithPlan(cyclingPlan(kBaseGeneration, 16, 5100, 2));
        std::string err;
        const auto folded = buildCompactedCheckpoint(f.file, openDocument(f.file), {}, &err);
        REQUIRE_MESSAGE(folded.has_value(), err);
        CHECK(folded->churnFraction > kSwitchUp);
        CHECK(folded->mode == kModeCas);
        CHECK(openDocument(folded->bytes).base.mode == kModeCas);

        // ...and a cas file whose churn stays above switch-down HOLDS cas on the next fold.
        const auto again =
            buildCompactedCheckpoint(folded->bytes, openDocument(folded->bytes), {}, &err);
        REQUIRE_MESSAGE(again.has_value(), err);
        CHECK(again->mode == kModeCas);
    }
}

TEST_CASE("adaptive: the churn window is the WHOLE retained history (wrong-window regression)") {
    // The research's own bug, pinned (spec 3.9): fresh states, then a long high-churn stretch,
    // then fresh states again. A signal windowed to recent activity sees only the trailing fresh
    // stretch and flips back to journal -- even though the churny middle is STILL fully retained
    // and still deduplicating. The whole-history signal must keep cas.
    Plan plan;
    std::uint64_t id = kBaseGeneration;
    for (int i = 1; i <= 6; ++i) // fresh
        plan.push_back({++id, static_cast<std::uint32_t>(i) % kKeys,
                        6000 + static_cast<std::uint32_t>(i)});
    for (int i = 1; i <= 20; ++i) // churny: two palettes over and over
        plan.push_back({++id, static_cast<std::uint32_t>(i) % kKeys,
                        6100 + (static_cast<std::uint32_t>(i) % 2)});
    for (int i = 1; i <= 6; ++i) // fresh again -- what a recent window would see
        plan.push_back({++id, static_cast<std::uint32_t>(i) % kKeys,
                        6200 + static_cast<std::uint32_t>(i)});
    Fixture f = buildWithPlan(plan);

    // The counterfactual, computed by hand: the churn of ONLY the trailing window. Every one of
    // those contents is fresh, so a windowed signal reads ~0 -- far below switch-down, i.e. a
    // windowed policy would pick journal here. This inequality failing is the regression alarm.
    {
        ChurnTracker windowed;
        for (std::size_t i = plan.size() - 6; i < plan.size(); ++i)
            windowed.add(blobHashOf(palette(plan[i].pid)), palette(plan[i].pid).size());
        CHECK(windowed.fraction() < kSwitchDown);
    }

    std::string err;
    const auto folded = buildCompactedCheckpoint(f.file, openDocument(f.file), {}, &err);
    REQUIRE_MESSAGE(folded.has_value(), err);
    CHECK(folded->churnFraction >= kSwitchUp); // the retained middle dominates the signal
    CHECK(folded->mode == kModeCas);

    // And the live tracker agrees with the fold's own measurement, state for state: seeding from
    // the open report is the same walk.
    const auto tracker = churnFromOpen(openDocument(f.file));
    REQUIRE(tracker.has_value());
    CHECK(tracker->fraction() == doctest::Approx(folded->churnFraction).epsilon(0.02));
}

TEST_CASE("cas: HIST extras survive re-spelling across mode switches") {
    // A HIST record carries document-layer fields this layer does not own (op/params/
    // manifest_snapshot). Adding references (H2->H4) and dropping them (H4->H2) must not lose
    // those fields.
    Fixture f;
    f.file = buildCheckpoint(baseInput());
    CommitTip tip = openDocument(f.file).tip;
    SaveState s = makeState(kBaseGeneration + 1, 1, 9900);
    s.metaJson = R"({"op":"paint","brush":"round"})";
    appendSaveBatch(f.file, tip, std::vector<SaveState>{s});
    SaveState s2 = makeState(kBaseGeneration + 2, 1, 9901);
    appendSaveBatch(f.file, tip, std::vector<SaveState>{s2});

    const auto extrasOf = [](std::span<const std::uint8_t> file, std::uint64_t state) {
        for (const ChunkRecord& r : framesOfType(file, kTypeHist))
            if (r.generation == state) {
                std::vector<std::uint8_t> whole(file.begin(), file.end());
                const auto rec = parseChunkAt(whole, r.offset);
                REQUIRE(rec.has_value());
                const auto payload = decodeChunkPayload(*rec, whole);
                REQUIRE(payload.has_value());
                return histExtrasJson(*payload);
            }
        return std::string{};
    };
    REQUIRE(extrasOf(f.file, kBaseGeneration + 1).find("round") != std::string::npos);

    const CompactionResult toCas = foldAs(f.file, openDocument(f.file), {}, kModeCas);
    CHECK(extrasOf(toCas.bytes, kBaseGeneration + 1).find("round") != std::string::npos);
    const CompactionResult back = foldAs(toCas.bytes, openDocument(toCas.bytes), {},
                                         kModeJournal);
    CHECK(extrasOf(back.bytes, kBaseGeneration + 1).find("round") != std::string::npos);
}

TEST_CASE("blob helpers: payload spelling and the lying-hash defense") {
    const std::vector<std::uint8_t> content{1, 2, 3, 4, 5, 6, 7, 8, 9};
    const BlobHash h = blobHashOf(content);
    const ChunkKey k = blobKeyOf(h);
    CHECK(std::equal(k.bytes.begin(), k.bytes.end(), h.begin())); // KEY = first 128 bits

    const auto payload = makeBlobPayload(h, content);
    REQUIRE(payload.size() == kBlobHashSize + content.size());
    const auto got = blobContentOf(payload);
    REQUIRE(got.has_value());
    CHECK(std::equal(got->begin(), got->end(), content.begin(), content.end()));

    // A stored head hash the content does not produce resolves NOTHING: trusting a lying writer
    // here would hand some state's reference the wrong bytes.
    auto lying = payload;
    lying[3] ^= 0xFF; // damage the stored hash, not the content
    CHECK(!blobContentOf(lying).has_value());
    auto rotten = payload;
    rotten[kBlobHashSize + 2] ^= 0xFF; // damage the content, not the hash
    CHECK(!blobContentOf(rotten).has_value());
    CHECK(!blobContentOf(std::vector<std::uint8_t>(8, 0)).has_value()); // too short
}

TEST_CASE("hist records: the cas reference spelling round-trips and cannot skew") {
    HistRecord rec;
    rec.state = 42;
    rec.parent = 41;
    rec.dirty.push_back({kTypeTile, keyOf(1)});
    rec.dirty.push_back({kTypePreview, zeroKey()});
    rec.dirty.push_back({kTypeVector, vectorKey(9)});
    rec.refs.assign(3, BlobRef{});
    rec.refs[0] = {true, blobHashOf(palette(1)), kTileFlags};
    rec.refs[2] = {true, blobHashOf(palette(2)), kFlagCritical};

    const std::string json = histRecordJson(rec, R"({"op":"test"})");
    const auto parsed = parseHistRecord(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(json.data()),
                                      json.size()));
    REQUIRE(parsed.has_value());
    CHECK(parsed->state == 42);
    CHECK(parsed->dirty == rec.dirty);
    REQUIRE(parsed->refs.size() == 3);
    CHECK(parsed->refs[0] == rec.refs[0]);
    CHECK(!parsed->refs[1].present); // the PRVW entry carries no reference
    CHECK(parsed->refs[2] == rec.refs[2]);
    const std::string extras = histExtrasJson(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(json.data()),
                                      json.size()));
    CHECK(extras.find("test") != std::string::npos);

    // A record with no refs at all parses to the H2 spelling: refs empty, not a list of absences.
    HistRecord plain;
    plain.state = 7;
    plain.dirty.push_back({kTypeTile, keyOf(0)});
    const std::string pjson = histRecordJson(plain);
    const auto pparsed = parseHistRecord(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(pjson.data()),
                                      pjson.size()));
    REQUIRE(pparsed.has_value());
    CHECK(pparsed->refs.empty());
}

TEST_CASE("an unreadable HIST record declines the whole walk (never a partial history)") {
    // A HIST frame that VERIFIES but will not parse -- a foreign writer's spelling, not damage
    // (damage is lostHistoryEntries, declined earlier). Skipping it would walk the panel over
    // the gap: a structural step past the opaque state would reconstruct a tree missing its
    // edit. The verdict is the lost-history one: the whole walk declines, the document stands.
    CheckpointInput in = baseInput();
    const std::string junk = "not a history record";
    in.chunks.push_back({kTypeHist, histKey(kBaseGeneration), kBaseGeneration, Profile::Store,
                         kFlagCritical, /*parity=*/false, /*history=*/true,
                         std::vector<std::uint8_t>(junk.begin(), junk.end())});
    std::vector<std::uint8_t> file = buildCheckpoint(in);
    CommitTip tip = openDocument(file).tip;
    appendSaveBatch(file, tip, std::vector<SaveState>{makeState(kBaseGeneration + 1, 0, 9100)});

    const OpenReport r = openDocument(file);
    CHECK(r.base.lostHistoryEntries == 0); // the frame is intact -- this is not damage...
    CHECK(!mui::loadedStates(r).has_value()); // ...but the walk is declined wholesale

    // The fold carries the opaque record VERBATIM (it cannot re-spell what it cannot read), so
    // a folded file keeps declining instead of laundering the state away -- in either mode.
    for (const char* mode : {kModeJournal, kModeCas}) {
        const CompactionResult folded = foldAs(file, r, {}, mode);
        const OpenReport after = openDocument(folded.bytes);
        CHECK(after.base.lostHistoryEntries == 0);
        CHECK(!mui::loadedStates(after).has_value());
    }
}
