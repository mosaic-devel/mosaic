#include "io/mosaic/journal_session.hpp"

#include "io/mosaic/docio.hpp"
#include "io/mosaic/file.hpp"
#include "io/mosaic/journal.hpp"
#include "io/mosaic/save.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// The app-facing recovery journal (spec 2.6): the tombstone-aware per-state diff, the autosave
// loop, and end-to-end restore -- the journal, replayed, composes with the file's committed
// region through documentFromReport exactly like retained history does. Covers file-backed and
// self-contained (untitled) journals, incremental multi-state, and the erase tombstone.
namespace {

using namespace mosaic;
using namespace mosaic::io::native;
namespace fs = std::filesystem;

fs::path testDir() {
    const char* env = std::getenv("TMPDIR");
    const fs::path dir = fs::path(env != nullptr ? env : "/tmp") / "mosaic_journal_session";
    fs::create_directories(dir);
    return dir;
}

std::vector<std::uint8_t> readBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Paint one 64px-aligned tile of a raster layer a solid colour (a mostly-transparent layer, so a
// single tile is the whole dirty set -- the case the journal is built to keep O(edit)).
void paintTile(core::RasterLayer& layer, std::uint32_t tx, std::uint32_t ty, std::uint8_t v) {
    common::Image& img = layer.image();
    for (std::uint32_t y = 0; y < 64 && ty * 64 + y < img.height; ++y)
        for (std::uint32_t x = 0; x < 64 && tx * 64 + x < img.width; ++x) {
            const std::size_t p = ((ty * 64 + y) * img.width + (tx * 64 + x)) * 4;
            img.rgba[p + 0] = v;
            img.rgba[p + 1] = static_cast<std::uint8_t>(v + 40);
            img.rgba[p + 2] = static_cast<std::uint8_t>(v + 80);
            img.rgba[p + 3] = 255;
        }
}

void eraseTile(core::RasterLayer& layer, std::uint32_t tx, std::uint32_t ty) {
    common::Image& img = layer.image();
    for (std::uint32_t y = 0; y < 64 && ty * 64 + y < img.height; ++y)
        for (std::uint32_t x = 0; x < 64 && tx * 64 + x < img.width; ++x) {
            const std::size_t p = ((ty * 64 + y) * img.width + (tx * 64 + x)) * 4;
            img.rgba[p + 0] = img.rgba[p + 1] = img.rgba[p + 2] = img.rgba[p + 3] = 0;
        }
}

std::unique_ptr<core::Document> makeDoc(const char* uuid) {
    auto doc = std::make_unique<core::Document>(200, 150);
    doc->setUuid(uuid);
    doc->setTitle("Journal Test");
    auto raster = doc->makeRaster("Paint", 200, 150);
    paintTile(*raster, 0, 0, 10); // one painted tile so the file has committed content
    doc->root().addOnTop(std::move(raster));
    return doc;
}

core::RasterLayer& firstRaster(core::Document& doc) {
    return *doc.root().child(0).as<core::RasterLayer>();
}

// Content equality via re-serialization: two documents match iff their manifest + tile + vector
// chunk sets are byte-identical (uuid, title, layer ids and the id allocator all survive restore,
// so equal content yields equal chunks).
using ChunkMap = std::map<std::pair<ChunkTag, std::array<std::uint8_t, 16>>,
                          std::pair<std::uint8_t, std::vector<std::uint8_t>>>;
ChunkMap contentMap(const core::Document& doc) {
    auto in = buildDocumentCheckpoint(doc);
    REQUIRE(in.has_value());
    ChunkMap m;
    for (const FileChunk& c : in->chunks)
        m[{c.type, c.key.bytes}] = {c.flags, c.payload};
    return m;
}

// Compose a restored document: the file's checkpoint + committed region + the journal's replayed
// chunks, highest-generation-wins -- the exact path openMosaicAtPath's restore takes.
std::unique_ptr<core::Document> composeRestore(const OpenReport& report,
                                               const std::vector<RecoveredChunk>& journalChunks) {
    OpenReport synth = report;
    for (const RecoveredChunk& c : journalChunks)
        synth.committed.push_back(c);
    std::string err;
    auto res = documentFromReport(synth, &err);
    REQUIRE_MESSAGE(res.has_value(), err);
    return std::move(res->document);
}

} // namespace

TEST_CASE("diff: only changed tiles, unchanged skipped") {
    auto doc = makeDoc("uuid-diff");
    const CheckpointInput prev = *buildDocumentCheckpoint(*doc);
    paintTile(firstRaster(*doc), 2, 1, 90); // a new tile elsewhere
    const CheckpointInput curr = *buildDocumentCheckpoint(*doc);

    const std::vector<StateChunk> dirty = diffDocumentStates(prev, curr);
    // Exactly one new TILE, nothing else -- the manifest is unchanged by a paint.
    CHECK(dirty.size() == 1);
    CHECK(dirty[0].type == kTypeTile);
    CHECK(diffDocumentStates(curr, curr).empty()); // idempotent: no change -> nothing
}

TEST_CASE("diff: an erased tile becomes a transparent tombstone") {
    auto doc = makeDoc("uuid-tomb");
    paintTile(firstRaster(*doc), 1, 0, 70);
    const CheckpointInput prev = *buildDocumentCheckpoint(*doc);
    eraseTile(firstRaster(*doc), 1, 0);
    const CheckpointInput curr = *buildDocumentCheckpoint(*doc);

    const std::vector<StateChunk> dirty = diffDocumentStates(prev, curr);
    CHECK(dirty.size() == 1);
    CHECK(dirty[0].type == kTypeTile);
    CHECK((dirty[0].flags & kFlagFiltered) == 0); // a blank tile is stored raw, not filtered
    for (std::uint8_t b : dirty[0].payload)
        CHECK(b == 0); // fully transparent
}

TEST_CASE("restore: file-backed journal composes a painted edit onto the commit") {
    auto doc = makeDoc("uuid-fileback");
    const std::vector<std::uint8_t> onDisk = buildCheckpoint(*buildDocumentCheckpoint(*doc));
    const OpenReport report = openDocument(onDisk);
    REQUIRE(report.tipValid);

    const std::string jpath = (testDir() / "fileback.journal").string();
    const JournalBinding binding =
        bindingForTip(doc->uuid(), "/docs/a.mosaic", report.tip);
    auto session = JournalSession::begin(jpath, binding, *buildDocumentCheckpoint(*doc),
                                         report.tip.commitId + 1);
    REQUIRE(session.has_value());

    paintTile(firstRaster(*doc), 2, 2, 200); // unsaved edit
    REQUIRE(session->autosave(*doc));
    const ChunkMap want = contentMap(*doc);

    const std::vector<std::uint8_t> jbytes = readBytes(jpath);
    const JournalReplay replay = replayJournal(jbytes, binding);
    CHECK(replay.binding == JournalBindingStatus::Ok);
    CHECK(replay.states.size() == 1);
    CHECK_FALSE(replay.anomaly);

    auto restored = composeRestore(report, replay.chunks);
    CHECK(contentMap(*restored) == want);
}

TEST_CASE("restore: an erase in the session survives as a tombstone") {
    auto doc = makeDoc("uuid-erase"); // tile (0,0) is painted and committed
    const std::vector<std::uint8_t> onDisk = buildCheckpoint(*buildDocumentCheckpoint(*doc));
    const OpenReport report = openDocument(onDisk);

    const std::string jpath = (testDir() / "erase.journal").string();
    const JournalBinding binding = bindingForTip(doc->uuid(), "/docs/e.mosaic", report.tip);
    auto session = JournalSession::begin(jpath, binding, *buildDocumentCheckpoint(*doc),
                                         report.tip.commitId + 1);
    REQUIRE(session.has_value());

    eraseTile(firstRaster(*doc), 0, 0); // erase the committed tile
    REQUIRE(session->autosave(*doc));
    const ChunkMap want = contentMap(*doc); // the tile is now absent (transparent)

    const JournalReplay replay = replayJournal(readBytes(jpath), binding);
    auto restored = composeRestore(report, replay.chunks);
    // Composition alone would keep the committed painted tile; the tombstone must override it.
    CHECK(contentMap(*restored) == want);
    const common::Image& img = firstRaster(*restored).image();
    CHECK(img.rgba[3] == 0); // top-left pixel is transparent again
}

TEST_CASE("restore: multiple autosaves accumulate incrementally") {
    auto doc = makeDoc("uuid-multi");
    const std::vector<std::uint8_t> onDisk = buildCheckpoint(*buildDocumentCheckpoint(*doc));
    const OpenReport report = openDocument(onDisk);

    const std::string jpath = (testDir() / "multi.journal").string();
    const JournalBinding binding = bindingForTip(doc->uuid(), "/docs/m.mosaic", report.tip);
    auto session = JournalSession::begin(jpath, binding, *buildDocumentCheckpoint(*doc),
                                         report.tip.commitId + 1);
    REQUIRE(session.has_value());

    paintTile(firstRaster(*doc), 1, 0, 50);
    REQUIRE(session->autosave(*doc));
    CHECK(session->autosave(*doc)); // no change since last: a no-op tick, still true
    paintTile(firstRaster(*doc), 2, 1, 120);
    REQUIRE(session->autosave(*doc));
    const ChunkMap want = contentMap(*doc);

    const JournalReplay replay = replayJournal(readBytes(jpath), binding);
    CHECK(replay.states.size() == 2); // two real states, the no-op wrote nothing
    auto restored = composeRestore(report, replay.chunks);
    CHECK(contentMap(*restored) == want);
}

TEST_CASE("restore: a self-contained (untitled) journal rebuilds from itself alone") {
    auto doc = makeDoc("uuid-untitled");
    const std::string jpath = (testDir() / "untitled.journal").string();
    const JournalBinding binding = selfContainedBinding(doc->uuid(), ""); // no path, no baseline
    auto session = JournalSession::begin(jpath, binding, std::nullopt, /*firstState=*/1);
    REQUIRE(session.has_value());

    paintTile(firstRaster(*doc), 1, 1, 33); // the doc has never been saved
    REQUIRE(session->autosave(*doc));
    const ChunkMap want = contentMap(*doc);

    const JournalReplay replay = replayJournal(readBytes(jpath), binding);
    CHECK(replay.binding == JournalBindingStatus::Ok);
    REQUIRE_FALSE(replay.states.empty());
    // No file to compose onto: an empty base, the journal is everything.
    OpenReport empty;
    auto restored = composeRestore(empty, replay.chunks);
    CHECK(contentMap(*restored) == want);
}

TEST_CASE("readJournalHeader recovers uuid + path; discard removes the file") {
    auto doc = makeDoc("uuid-hdr");
    const std::string jpath = (testDir() / "hdr.journal").string();
    const JournalBinding binding = selfContainedBinding(doc->uuid(), "/docs/hdr.mosaic");
    auto session = JournalSession::begin(jpath, binding, std::nullopt, 1);
    REQUIRE(session.has_value());
    paintTile(firstRaster(*doc), 0, 1, 12);
    REQUIRE(session->autosave(*doc));

    const auto info = readJournalHeader(readBytes(jpath));
    REQUIRE(info.has_value());
    CHECK(info->documentUuid == "uuid-hdr");
    CHECK(info->documentPath == "/docs/hdr.mosaic");
    // The reconstructed binding replays the journal without knowing the original binding object.
    const JournalReplay replay = replayJournal(readBytes(jpath), info->toBinding());
    CHECK(replay.binding == JournalBindingStatus::Ok);
    CHECK(replay.states.size() == 1);

    session->discard();
    CHECK_FALSE(fs::exists(jpath));
}

// Incompressible pixels, so each autosave genuinely grows the journal by a tile's worth -- the
// LZ4 tier cannot fold a noise tile, which is what makes the growth threshold measurable with a
// tiny floor.
namespace {
void paintNoise(core::RasterLayer& layer, std::uint32_t tx, std::uint32_t ty, std::uint32_t seed) {
    common::Image& img = layer.image();
    std::uint32_t s = seed * 747796405u + 2891336453u;
    for (std::uint32_t y = 0; y < 64 && ty * 64 + y < img.height; ++y)
        for (std::uint32_t x = 0; x < 64 && tx * 64 + x < img.width; ++x) {
            const std::size_t p = ((ty * 64 + y) * img.width + (tx * 64 + x)) * 4;
            s = s * 1664525u + 1013904223u;
            img.rgba[p + 0] = static_cast<std::uint8_t>(s);
            img.rgba[p + 1] = static_cast<std::uint8_t>(s >> 8);
            img.rgba[p + 2] = static_cast<std::uint8_t>(s >> 16);
            img.rgba[p + 3] = 255;
        }
}
} // namespace

TEST_CASE("growth compaction: the journal folds atomically and keeps replaying (spec 2.6)") {
    auto doc = makeDoc("uuid-growth");
    const std::vector<std::uint8_t> onDisk = buildCheckpoint(*buildDocumentCheckpoint(*doc));
    const OpenReport report = openDocument(onDisk);
    REQUIRE(report.tipValid);
    const std::string jpath = (testDir() / "growth.journal").string();
    // A stale temp from a crashed compaction must be cleaned up by begin(), never mistaken for
    // anything.
    {
        std::ofstream stale(jpath + kJournalCompactSuffix, std::ios::binary);
        stale << "torn";
    }
    const JournalBinding binding = bindingForTip(doc->uuid(), "/docs/g.mosaic", report.tip);
    auto session = JournalSession::begin(jpath, binding, *buildDocumentCheckpoint(*doc),
                                         report.tip.commitId + 1, nullptr,
                                         /*compactMinBytes=*/2048);
    REQUIRE(session.has_value());
    CHECK_FALSE(fs::exists(jpath + kJournalCompactSuffix));

    // Erase the committed tile FIRST: the compacted cumulative state must carry the tombstone
    // forward, or composition would resurrect the committed pixels.
    eraseTile(firstRaster(*doc), 0, 0);
    REQUIRE(session->autosave(*doc));

    // Repeatedly repaint the SAME tile: the journal grows by a whole noise tile per autosave
    // while the live working set stays two entries -- the save-averse growth shape.
    std::uintmax_t peak = 0;
    bool shrank = false;
    for (std::uint32_t i = 0; i < 12; ++i) {
        paintNoise(firstRaster(*doc), 2, 1, 1000 + i);
        REQUIRE(session->autosave(*doc));
        const std::uintmax_t size = fs::file_size(jpath);
        if (size < peak)
            shrank = true;
        peak = std::max(peak, size);
    }
    REQUIRE(shrank);                                    // the rewrite actually happened...
    CHECK_FALSE(fs::exists(jpath + kJournalCompactSuffix)); // ...and left no temp behind

    // The compacted journal replays under the SAME binding to exactly the newest document --
    // including the erase.
    const ChunkMap want = contentMap(*doc);
    const JournalReplay replay = replayJournal(readBytes(jpath), binding);
    CHECK(replay.binding == JournalBindingStatus::Ok);
    CHECK_FALSE(replay.anomaly);
    auto restored = composeRestore(report, replay.chunks);
    CHECK(contentMap(*restored) == want);
    CHECK(firstRaster(*restored).image().rgba[3] == 0); // the erased tile stayed erased
    // The id contract, pinned: the cumulative state KEEPS the first autosaved id (consumed,
    // never re-minted) and no two replayed states share one. A duplicate id is a generation
    // TIE at compose time -- the silently-stale-content class of Round 12 A5 -- and the final
    // content comparison alone cannot see it (a later autosave usually papers over the tie).
    REQUIRE(!replay.states.empty());
    CHECK(replay.states.front() == report.tip.commitId + 1);
    const std::set<std::uint64_t> distinctIds(replay.states.begin(), replay.states.end());
    CHECK(distinctIds.size() == replay.states.size());

    // ...and the session keeps appending on the swapped handle: the explicit-link chain
    // continues across the rename, which is the part a re-derived link would silently break.
    paintNoise(firstRaster(*doc), 0, 1, 77);
    REQUIRE(session->autosave(*doc));
    const JournalReplay more = replayJournal(readBytes(jpath), binding);
    CHECK(more.binding == JournalBindingStatus::Ok);
    CHECK_FALSE(more.anomaly);
    auto restored2 = composeRestore(report, more.chunks);
    CHECK(contentMap(*restored2) == contentMap(*doc));
}
