#include "io/mosaic/file.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// The .mosaic checkpoint container (S48 Build 1, root/directory slice; spec 2.3, 2.6, 2.8):
// build/read round-trips, the 128KB slot + RPTR overflow, the replicated-root recovery ladder
// (slot -> tail window -> full scan), directory-as-accelerator behavior under damage, the
// truncation battery, and the atomic write's failure contract.
namespace {

using namespace mosaic::io::native;

std::vector<std::uint8_t> patternPayload(std::size_t n, std::uint8_t seed) {
    std::vector<std::uint8_t> p(n);
    for (std::size_t i = 0; i < n; ++i)
        p[i] = static_cast<std::uint8_t>(seed + i * 7);
    return p;
}

CheckpointInput sampleInput() {
    // Parity coverage mirrors the spec (2.7 current-content tile/vector; 3.8 history opted
    // out); manifest/preview ride uncovered here -- the manifest also lives in HIST snapshots.
    CheckpointInput in;
    in.documentUuid = "test-uuid-0001";
    in.generation = 40;
    in.chunks.push_back({kTypeManifest, zeroKey(), 40, Profile::Balanced, kFlagCritical,
                         /*parity=*/false, /*history=*/false, patternPayload(600, 1)});
    in.chunks.push_back({kTypePreview, zeroKey(), 40, Profile::Balanced, 0 /* ancillary */,
                         /*parity=*/false, /*history=*/false, patternPayload(256, 2)});
    for (std::uint32_t i = 0; i < 6; ++i)
        in.chunks.push_back({kTypeTile, tileKey(7, i, 0), 40, Profile::Balanced, kFlagCritical,
                             /*parity=*/true, /*history=*/false,
                             patternPayload(3000 + i * 13, static_cast<std::uint8_t>(3 + i))});
    in.chunks.push_back({kTypeVector, vectorKey(9), 40, Profile::Balanced, kFlagCritical,
                         /*parity=*/true, /*history=*/false, patternPayload(900, 11)});
    // Retained history: a superseded version of tile (7,0,0) at an older generation.
    in.chunks.push_back({kTypeTile, tileKey(7, 0, 0), 12, Profile::Balanced, kFlagCritical,
                         /*parity=*/false, /*history=*/true, patternPayload(3000, 99)});
    in.chunks.push_back({kTypeHist, histKey(40), 40, Profile::Store, kFlagCritical,
                         /*parity=*/false, /*history=*/true, patternPayload(120, 12)});
    return in;
}

// sampleInput() with the retained-history chunks dropped: what the CURRENT document is.
CheckpointInput sampleCurrentOnly() {
    CheckpointInput in = sampleInput();
    std::erase_if(in.chunks, [](const FileChunk& c) { return c.history; });
    return in;
}

// Every chunk written came back somewhere: current content in `chunks`, retained history in
// `retained`. The split itself is asserted separately (readCheckpoint splits by the directory's
// "h" flag, with a highest-generation collapse behind it).
bool contentMatches(const ReadReport& report, const CheckpointInput& in) {
    const auto present = [&](const FileChunk& c, const std::vector<RecoveredChunk>& where) {
        for (const RecoveredChunk& r : where)
            if (r.type == c.type && r.key == c.key && r.generation == c.generation &&
                r.payload == c.payload)
                return true;
        return false;
    };
    for (const FileChunk& c : in.chunks)
        if (!present(c, report.chunks) && !present(c, report.retained))
            return false;
    return true;
}

} // namespace

TEST_CASE("mosaic file: checkpoint round-trip via the fast path") {
    const CheckpointInput in = sampleInput();
    const auto file = buildCheckpoint(in);

    // The slot holds the real root in the common case (a few-KB root, spec 2.3).
    const auto slot = parseChunkAt(file, kPreambleSize);
    REQUIRE(slot.has_value());
    CHECK(slot->valid);
    CHECK(slot->type == kTypeRoot);

    const ReadReport report = readCheckpoint(file);
    CHECK(report.rootFound);
    CHECK(!report.usedFullScan);
    CHECK(report.lostEntries == 0);
    CHECK(report.generation == 40);
    CHECK(report.documentUuid == in.documentUuid);
    CHECK(report.walStartOffset > 0);
    CHECK(report.chunks.size() + report.retained.size() == in.chunks.size());
    CHECK(contentMatches(report, in));
    CHECK(report.lostHistoryEntries == 0);

    // The split (spec 3.3): `chunks` is CURRENT content, unique per (TYPE, KEY) -- the invariant
    // documentFromReport and the history walk both rely on. The superseded tile and the HIST
    // record land in `retained` instead, so a compacted file's history is reachable without ever
    // presenting two versions of one key as current.
    int currentVersions = 0;
    for (const RecoveredChunk& r : report.chunks) {
        CHECK(r.type != kTypeHist);
        if (r.type == kTypeTile && r.key == tileKey(7, 0, 0)) {
            ++currentVersions;
            CHECK(r.generation == 40); // the newest, never the retained generation-12 frame
        }
    }
    CHECK(currentVersions == 1);
    CHECK(report.retained.size() == 2); // the generation-12 tile + the HIST record
    CHECK(std::count_if(report.retained.begin(), report.retained.end(), [](const auto& r) {
              return r.type == kTypeTile && r.generation == 12;
          }) == 1);
    CHECK(std::count_if(report.retained.begin(), report.retained.end(), [](const auto& r) {
              return r.type == kTypeHist;
          }) == 1);

    // Frame extents: what compaction copies through byte-verbatim. Every frame the directory
    // resolved carries the offset/length of its own bytes in this image.
    for (const RecoveredChunk& r : report.chunks) {
        REQUIRE(r.frameLen > 0);
        const auto rec = parseChunkAt(file, r.frameOffset);
        REQUIRE(rec.has_value());
        CHECK(rec->valid);
        CHECK(rec->type == r.type);
        CHECK(rec->key == r.key);
        CHECK(rec->consumed == r.frameLen);
    }
}

TEST_CASE("mosaic file: a container from the future is refused, not mistaken for damage") {
    // Without a version gate, a v2 file whose chunk framing changed would present to this reader as
    // "no root verifies" -- and it would fall down the recovery ladder and tell the user their
    // perfectly good document was badly damaged. That is the worst sentence a format that prides
    // itself on never being silently wrong could produce, and it cannot be retrofitted into readers
    // already in the wild.
    CheckpointInput in = sampleInput();

    SUBCASE("a newer version claimed by the ROOT is refused on sight") {
        in.formatVersion = kFormatVersion + 1;
        const auto file = buildCheckpoint(in);
        const ReadReport r = readCheckpoint(file);
        CHECK(r.unsupportedVersion);
        CHECK(r.formatVersion == kFormatVersion + 1);
        CHECK(r.rootFound);       // we DID verify a root: the framing happens to still be ours...
        CHECK(!r.usedFullScan);   // ...but we decline to interpret what it indexes
        CHECK(r.chunks.empty());
        CHECK(r.lostEntries == 0); // nothing is lost: we simply did not look
    }

    SUBCASE("a newer version + unparseable framing is refused, NOT reported as destroyed") {
        // The real shape of a future file: this build cannot verify a single chunk in it.
        in.formatVersion = kFormatVersion + 7;
        auto file = buildCheckpoint(in);
        for (const ChunkRecord& rec : scanChunks(file))
            if (rec.valid)
                file[rec.payloadOffset] ^= 0xFF; // nothing verifies, as if the framing had changed
        const ReadReport r = readCheckpoint(file);
        CHECK(r.unsupportedVersion);
        CHECK(r.formatVersion == kFormatVersion + 7);
        CHECK(!r.rootFound);
        CHECK(!r.usedFullScan); // the honest refusal, never the "badly damaged" face
    }

    SUBCASE("a ROTTED preamble version byte is a non-event -- the checksummed root wins") {
        // The preamble carries no checksum of its own. A gate that trusted it alone would let one
        // flipped byte lock a user out of an intact document -- exactly the single-point-of-failure
        // class the manifest replica exists to kill. Two facts must agree before refusing, and here
        // they do not: three checksummed root replicas all say version 1.
        auto file = buildCheckpoint(in);
        file[8] = kFormatVersion + 9; // byte 8 of the preamble: the version, corrupted

        const ReadReport r = readCheckpoint(file);
        CHECK(!r.unsupportedVersion);
        CHECK(r.formatVersion == kFormatVersion);
        CHECK(r.rootFound);
        CHECK(r.lostEntries == 0);
        CHECK(contentMatches(r, in)); // opens exactly as if nothing had happened
    }

    SUBCASE("a v1 file with every root destroyed still degrades to the full scan") {
        // The gate must not swallow the genuine 3e case: intact preamble, no roots => badly damaged.
        auto file = buildCheckpoint(in);
        for (const ChunkRecord& rec : scanChunks(file))
            if (rec.valid && rec.type == kTypeRoot)
                file[rec.payloadOffset + 2] ^= 0xFF;
        const ReadReport r = readCheckpoint(file);
        CHECK(!r.unsupportedVersion);
        CHECK(!r.rootFound);
        CHECK(r.usedFullScan);
        CHECK(r.find(kTypeManifest, zeroKey()) != nullptr);
    }
}

TEST_CASE("mosaic file: the manifest is replicated, and a replica is not history") {
    // Two frames sharing (TYPE, KEY, GENERATION) are the SAME logical chunk -- generation is what
    // versions a key. buildCheckpoint uses that to give the manifest a second copy, far from the
    // first. The reader must collapse them as replicas, never file the loser under retained
    // history: a "history" frame that rots declines the whole undo walk, and a manifest replica
    // rotting must do nothing at all.
    CheckpointInput in;
    in.documentUuid = "test-uuid-replica";
    in.generation = 3;
    in.chunks.push_back({kTypeManifest, zeroKey(), 3, Profile::Balanced, kFlagCritical,
                         /*parity=*/false, /*history=*/false, patternPayload(700, 5)});
    for (std::uint32_t i = 0; i < 4; ++i)
        in.chunks.push_back({kTypeTile, tileKey(1, i, 0), 3, Profile::Balanced, kFlagCritical,
                             /*parity=*/true, /*history=*/false, patternPayload(800 + i, 7)});
    const auto file = buildCheckpoint(in);

    std::vector<std::size_t> copies;
    for (const ChunkRecord& r : scanChunks(file))
        if (r.valid && r.type == kTypeManifest)
            copies.push_back(r.offset);
    REQUIRE(copies.size() == 2);
    CHECK(copies[1] > copies[0]);

    const ReadReport clean = readCheckpoint(file);
    CHECK(clean.lostEntries == 0);
    CHECK(clean.retained.empty()); // the replica is NOT retained history
    CHECK(std::count_if(clean.chunks.begin(), clean.chunks.end(),
                        [](const auto& c) { return c.type == kTypeManifest; }) == 1);

    // Either copy may die: the other answers, and nothing is reported lost.
    for (const std::size_t off : copies) {
        auto damaged = file;
        const auto rec = parseChunkAt(damaged, off);
        REQUIRE(rec.has_value());
        damaged[rec->payloadOffset] ^= 0xFF;
        const ReadReport r = readCheckpoint(damaged);
        CHECK(r.lostEntries == 0);
        CHECK(r.lostHistoryEntries == 0);
        CHECK(r.retained.empty());
        const RecoveredChunk* m = r.find(kTypeManifest, zeroKey());
        REQUIRE(m != nullptr);
        CHECK(m->payload == patternPayload(700, 5)); // the survivor, byte-exact
    }

    // Both dead: honestly lost, and counted ONCE -- one chunk, not two areas.
    auto both = file;
    for (const std::size_t off : copies) {
        const auto rec = parseChunkAt(both, off);
        both[rec->payloadOffset] ^= 0xFF;
    }
    const ReadReport gone = readCheckpoint(both);
    CHECK(gone.lostEntries == 1);
    CHECK(gone.find(kTypeManifest, zeroKey()) == nullptr);
}

TEST_CASE("mosaic file: the recovery ladder survives losing any (or every) root replica") {
    const CheckpointInput in = sampleInput();
    const auto file = buildCheckpoint(in);

    // Rung 2: destroy the slot root -> the end-of-checkpoint replicas answer.
    std::vector<std::uint8_t> noSlot = file;
    REQUIRE(noSlot.size() >= kPreambleSize + kRootSlotSize); // the whole slot is in range
    std::fill(noSlot.begin() + kPreambleSize,
              noSlot.begin() + static_cast<std::ptrdiff_t>(kPreambleSize + kRootSlotSize), 0);
    ReadReport r2 = readCheckpoint(noSlot);
    CHECK(r2.rootFound);
    CHECK(!r2.usedFullScan);
    CHECK(contentMatches(r2, in));

    // Destroy the slot AND one end replica -> the second replica answers.
    std::vector<std::uint8_t> oneLeft = noSlot;
    const std::size_t walStart = readCheckpoint(file).walStartOffset;
    oneLeft[walStart + kHeaderSize + 4] ^= 0xFF; // first replica's payload
    ReadReport r3 = readCheckpoint(oneLeft);
    CHECK(r3.rootFound);
    CHECK(contentMatches(r3, in));

    // Rung 3: destroy ALL THREE replicas -> full scan reconstructs the content with no index,
    // collapsing to highest-generation-wins per (TYPE, KEY).
    std::vector<std::uint8_t> noRoots = noSlot;
    for (const ChunkRecord& rec : scanChunks(noRoots))
        if (rec.valid && rec.type == kTypeRoot)
            noRoots[rec.payloadOffset + 2] ^= 0xFF;
    ReadReport r4 = readCheckpoint(noRoots);
    CHECK(!r4.rootFound);
    CHECK(r4.usedFullScan);
    const RecoveredChunk* tile00 = r4.find(kTypeTile, tileKey(7, 0, 0));
    REQUIRE(tile00 != nullptr);
    CHECK(tile00->generation == 40); // the current version, not the retained older one
    CHECK(r4.find(kTypeManifest, zeroKey()) != nullptr);
    CHECK(r4.find(kTypeVector, vectorKey(9)) != nullptr);
}

TEST_CASE("mosaic file: root-slot overflow -- RPTR in the slot, real root at the end") {
    // The overflow driver is real now, not injected: a document with enough parity-covered
    // tiles grows an rs_params stripe map past the 128KB slot (Round 10's forcing case).
    CheckpointInput in;
    in.documentUuid = "test-uuid-overflow";
    in.generation = 5;
    for (std::uint32_t i = 0; i < 16000; ++i)
        in.chunks.push_back({kTypeTile, tileKey(1, i % 1000, i / 1000), 5, Profile::Store,
                             kFlagCritical, /*parity=*/true, /*history=*/false,
                             patternPayload(24, static_cast<std::uint8_t>(i))});
    const auto file = buildCheckpoint(in);

    const auto slot = parseChunkAt(file, kPreambleSize);
    REQUIRE(slot.has_value());
    CHECK(slot->valid);
    CHECK(slot->type == kTypeRootPtr);

    // Reached via the pointer...
    ReadReport viaPtr = readCheckpoint(file);
    CHECK(viaPtr.rootFound);
    CHECK(!viaPtr.usedFullScan);
    CHECK(viaPtr.rsParamsJson.size() > kRootSlotSize); // genuinely what overflowed the slot
    CHECK(contentMatches(viaPtr, in));

    // ...via the end replicas after the pointer is destroyed...
    std::vector<std::uint8_t> noPtr = file;
    REQUIRE(noPtr.size() >= kPreambleSize + kRootSlotSize); // the whole slot is in range
    std::fill(noPtr.begin() + kPreambleSize,
              noPtr.begin() + static_cast<std::ptrdiff_t>(kPreambleSize + kRootSlotSize), 0);
    ReadReport viaTail = readCheckpoint(noPtr);
    CHECK(viaTail.rootFound);
    CHECK(contentMatches(viaTail, in));

    // ...and via full scan after slot AND both replicas are gone.
    std::vector<std::uint8_t> nothing = noPtr;
    for (const ChunkRecord& rec : scanChunks(nothing))
        if (rec.valid && rec.type == kTypeRoot)
            nothing[rec.payloadOffset + 2] ^= 0xFF;
    ReadReport viaScan = readCheckpoint(nothing);
    CHECK(viaScan.usedFullScan);
    CHECK(viaScan.find(kTypeTile, tileKey(1, 0, 0)) != nullptr);
    CHECK(viaScan.chunks.size() == in.chunks.size());
}

TEST_CASE("mosaic file: parity reconstructs damaged entries -- exactly, or honestly not at all") {
    const CheckpointInput in = sampleInput();
    const auto clean = buildCheckpoint(in);
    const auto damage = [&](std::vector<std::uint8_t>& file, std::uint32_t tx) {
        for (const ChunkRecord& rec : scanChunks(file))
            if (rec.valid && rec.type == kTypeTile && rec.key == tileKey(7, tx, 0) &&
                rec.generation == 40) {
                file[rec.payloadOffset + 5] ^= 0xFF;
                return;
            }
    };

    // One damaged tile: reconstructed, re-verified, byte-exact -- nothing lost.
    auto one = clean;
    damage(one, 3);
    const ReadReport r1 = readCheckpoint(one);
    CHECK(r1.rsReconstructed == 1);
    CHECK(r1.lostEntries == 0);
    CHECK(r1.chunks.size() + r1.retained.size() == in.chunks.size());
    CHECK(contentMatches(r1, in));
    // A parity-rebuilt frame exists nowhere in the file: no extent, so compaction re-encodes it
    // from the payload rather than copying the damaged bytes through.
    const RecoveredChunk* rebuilt = r1.find(kTypeTile, tileKey(7, 3, 0));
    REQUIRE(rebuilt != nullptr);
    CHECK(rebuilt->frameLen == 0);

    // Two damaged tiles in the stripe: still within the m=2 budget.
    auto two = clean;
    damage(two, 1);
    damage(two, 4);
    const ReadReport r2 = readCheckpoint(two);
    CHECK(r2.rsReconstructed == 2);
    CHECK(r2.lostEntries == 0);
    CHECK(contentMatches(r2, in));

    // Three damaged tiles: beyond the budget -- ALL three honestly lost, never approximated,
    // and the undamaged entries still come back intact.
    auto three = clean;
    damage(three, 1);
    damage(three, 2);
    damage(three, 4);
    const ReadReport r3 = readCheckpoint(three);
    CHECK(r3.rsReconstructed == 0);
    CHECK(r3.lostEntries == 3);
    CHECK(r3.find(kTypeTile, tileKey(7, 0, 0)) != nullptr);
    CHECK(r3.find(kTypeVector, vectorKey(9)) != nullptr);

    // Damaged parity is an erasure like any other: both PRTY chunks + one tile = survivors
    // below k -- declined; two dead parity shards alone cost nothing.
    auto parityDead = clean;
    for (const ChunkRecord& rec : scanChunks(parityDead))
        if (rec.valid && rec.type == kTypeParity)
            parityDead[rec.payloadOffset + 1] ^= 0xFF;
    const ReadReport r4 = readCheckpoint(parityDead);
    CHECK(r4.lostEntries == 0); // parity chunks are not directory entries
    CHECK(contentMatches(r4, in));
    damage(parityDead, 2);
    const ReadReport r5 = readCheckpoint(parityDead);
    CHECK(r5.rsReconstructed == 0);
    CHECK(r5.lostEntries == 1);

    // A history chunk (parity=false, spec 3.8) is an honest loss when damaged -- but it is counted
    // APART from content loss. The document itself is byte-perfect here; only an old undo state
    // died, and raising the "this file is damaged" face over that would be crying wolf.
    auto hist = clean;
    for (const ChunkRecord& rec : scanChunks(hist))
        if (rec.valid && rec.type == kTypeTile && rec.key == tileKey(7, 0, 0) &&
            rec.generation == 12) {
            hist[rec.payloadOffset + 5] ^= 0xFF;
            break;
        }
    const ReadReport r6 = readCheckpoint(hist);
    CHECK(r6.rsReconstructed == 0);
    CHECK(r6.lostEntries == 0);
    CHECK(r6.lostHistoryEntries == 1);
    CHECK(contentMatches(r6, sampleCurrentOnly())); // current content untouched

    // ...and symmetrically, a damaged HIST record is a lost undo state, not lost content.
    auto histRec = clean;
    for (const ChunkRecord& rec : scanChunks(histRec))
        if (rec.valid && rec.type == kTypeHist) {
            histRec[rec.payloadOffset + 1] ^= 0xFF;
            break;
        }
    const ReadReport r7 = readCheckpoint(histRec);
    CHECK(r7.lostEntries == 0);
    CHECK(r7.lostHistoryEntries == 1);
}

TEST_CASE("mosaic file: truncation battery -- degrade, never crash, never lie") {
    const CheckpointInput in = sampleInput();
    const auto file = buildCheckpoint(in);
    for (int pct = 95; pct >= 5; pct -= 10) {
        const std::size_t len = file.size() * static_cast<std::size_t>(pct) / 100;
        const std::vector<std::uint8_t> cut(file.begin(),
                                            file.begin() + static_cast<std::ptrdiff_t>(len));
        const ReadReport report = readCheckpoint(cut); // must not crash at ANY length
        for (const RecoveredChunk& r : report.chunks) {
            // Whatever comes back must be byte-correct -- partial recovery is fine, silently
            // wrong data never is.
            bool matches = false;
            for (const FileChunk& c : in.chunks)
                if (c.type == r.type && c.key == r.key && c.generation == r.generation)
                    matches = (c.payload == r.payload);
            CHECK(matches);
        }
    }
}

TEST_CASE("mosaic file: atomic write contract") {
    namespace fs = std::filesystem;
    const char* env = std::getenv("TMPDIR");
    const fs::path dir = fs::path(env ? env : "/tmp") / "mosaic_file_test";
    fs::create_directories(dir);
    const std::string target = (dir / "doc.mosaic").string();

    const auto v1 = patternPayload(50000, 5);
    std::string error;
    REQUIRE(writeFileAtomic(target, v1, &error));
    {
        std::ifstream f(target, std::ios::binary);
        std::vector<std::uint8_t> back((std::istreambuf_iterator<char>(f)),
                                       std::istreambuf_iterator<char>());
        CHECK(back == v1);
    }

    // Overwrite is atomic-replace, and no temp litter survives success.
    const auto v2 = patternPayload(30000, 6);
    REQUIRE(writeFileAtomic(target, v2, &error));
    {
        std::ifstream f(target, std::ios::binary);
        std::vector<std::uint8_t> back((std::istreambuf_iterator<char>(f)),
                                       std::istreambuf_iterator<char>());
        CHECK(back == v2);
    }
    std::size_t entries = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        (void)e;
        ++entries;
    }
    CHECK(entries == 1);

    // Failure leaves the previous contents untouched (a nonexistent directory can't be saved
    // into; the check is that the OLD file is still exactly v2 afterwards).
    const std::string bad = (dir / "no-such-subdir" / "doc.mosaic").string();
    CHECK(!writeFileAtomic(bad, v1, &error));
    CHECK(!error.empty());
    {
        std::ifstream f(target, std::ios::binary);
        std::vector<std::uint8_t> back((std::istreambuf_iterator<char>(f)),
                                       std::istreambuf_iterator<char>());
        CHECK(back == v2);
    }
    fs::remove_all(dir);
}

TEST_CASE("mosaic file: checkpoint end-to-end through the real filesystem") {
    namespace fs = std::filesystem;
    const char* env = std::getenv("TMPDIR");
    const fs::path dir = fs::path(env ? env : "/tmp") / "mosaic_file_rt";
    fs::create_directories(dir);
    const std::string target = (dir / "roundtrip.mosaic").string();

    const CheckpointInput in = sampleInput();
    std::string error;
    REQUIRE(writeFileAtomic(target, buildCheckpoint(in), &error));

    std::ifstream f(target, std::ios::binary);
    std::vector<std::uint8_t> back((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
    const ReadReport report = readCheckpoint(back);
    CHECK(report.rootFound);
    CHECK(report.lostEntries == 0);
    CHECK(contentMatches(report, in));
    fs::remove_all(dir);
}
