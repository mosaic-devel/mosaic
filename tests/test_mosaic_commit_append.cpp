#include "io/mosaic/docio.hpp"
#include "io/mosaic/file.hpp"
#include "io/mosaic/journal_session.hpp" // diffDocumentStates (the app-side tombstone-aware differ)
#include "io/mosaic/save.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

// The File->Save commit-append recipe (spec 2.6, Round 12), exactly as MainWindow::commitAppendSave
// composes it: a full write arms {baseline serialization, stamped tip}; each later Save diffs the
// live document against the baseline (diffDocumentStates) and appends ONE state onto the committed
// region (appendSaveToFile), advancing the tip. This pins that the composition of the docio bridge,
// the tombstone-aware differ and the container append round-trips -- appends (never rewrites), grows
// the history by one commit per Save, and carries erases through as tombstones -- so the app wiring
// rests on a verified io flow (the app modal blocks a headless end-to-end drive).
namespace {

using namespace mosaic;
using namespace mosaic::io::native;
namespace fs = std::filesystem;

fs::path testDir() {
    const char* env = std::getenv("TMPDIR");
    const fs::path dir = fs::path(env != nullptr ? env : "/tmp") / "mosaic_commit_append";
    fs::create_directories(dir);
    return dir;
}

std::vector<std::uint8_t> readBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

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
    doc->setTitle("Commit Append Test");
    auto raster = doc->makeRaster("Paint", 200, 150);
    paintTile(*raster, 0, 0, 10); // one painted tile so the checkpoint has committed content
    doc->root().addOnTop(std::move(raster));
    return doc;
}

core::RasterLayer& firstRaster(core::Document& doc) {
    return *doc.root().child(0).as<core::RasterLayer>();
}

// Content equality via re-serialization: two documents match iff their chunk sets (manifest + tiles
// + vectors) are byte-identical. uuid/title/layer ids survive the round-trip, so equal content ->
// equal chunks.
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

// The armed state a File->Save appends onto: the last-durable serialization + the stamped tip
// (MainWindow::CommitAnchor, minus walStart which only gates the compaction fallback).
struct Anchor {
    CheckpointInput baseline;
    CommitTip tip;
};

// The full write + arm that saveDocumentAs/writeMosaicTo does: write the checkpoint atomically, then
// open it back to derive + stamp the tip and take the baseline.
Anchor fullWriteAndArm(const core::Document& doc, const std::string& path) {
    const std::vector<std::uint8_t> bytes = buildCheckpoint(*buildDocumentCheckpoint(doc));
    std::string err;
    REQUIRE_MESSAGE(writeFileAtomic(path, bytes, &err), err);
    const OpenReport report = openDocument(readBytes(path));
    REQUIRE(report.tipValid);
    Anchor a;
    a.tip = report.tip;
    REQUIRE_MESSAGE(stampTipIdentity(path, a.tip, &err), err);
    a.baseline = *buildDocumentCheckpoint(doc);
    return a;
}

// One commit-append Save (MainWindow::commitAppendSave): diff live doc vs the baseline, append the
// dirty state, advance the tip + baseline. Returns the appended chunk count (0 => nothing to write).
std::size_t commitAppendSave(const core::Document& doc, const std::string& path, Anchor& anchor) {
    const CheckpointInput after = *buildDocumentCheckpoint(doc);
    std::vector<StateChunk> dirty = diffDocumentStates(anchor.baseline, after);
    if (dirty.empty())
        return 0; // the app leaves the file untouched and just clears the dirty marker
    SaveState st;
    st.stateId = anchor.tip.commitId + 1;
    const std::size_t n = dirty.size();
    st.chunks = std::move(dirty);
    std::string err;
    const SaveStatus s = appendSaveToFile(path, anchor.tip, {&st, 1}, &err);
    REQUIRE_MESSAGE(s == SaveStatus::Ok, err);
    anchor.baseline = after;
    return n;
}

std::unique_ptr<core::Document> reopen(const std::string& path) {
    std::string err;
    auto res = documentFromReport(openDocument(readBytes(path)), &err);
    REQUIRE_MESSAGE(res.has_value(), err);
    return std::move(res->document);
}

} // namespace

TEST_CASE("commit-append Save: a painted edit appends (never rewrites) and reopens intact") {
    auto doc = makeDoc("uuid-append");
    const std::string path = (testDir() / "append.mosaic").string();
    Anchor anchor = fullWriteAndArm(*doc, path);
    const std::vector<std::uint8_t> beforeAppend = readBytes(path);
    const std::uint64_t firstCommit = anchor.tip.commitId;

    paintTile(firstRaster(*doc), 2, 1, 130); // one new tile: the edit since the save
    const std::size_t chunks = commitAppendSave(*doc, path, anchor);
    CHECK(chunks == 1); // exactly the one changed tile (a paint leaves the manifest alone)

    const std::vector<std::uint8_t> afterAppend = readBytes(path);
    // Append-only: the file GREW and its first bytes are byte-identical to the pre-Save image -- a
    // commit, not a full rewrite (the whole point of spec 2.6).
    REQUIRE(afterAppend.size() > beforeAppend.size());
    CHECK(std::equal(beforeAppend.begin(), beforeAppend.end(), afterAppend.begin()));
    // The tip advanced by exactly one committed state.
    CHECK(anchor.tip.commitId == firstCommit + 1);

    const OpenReport report = openDocument(afterAppend);
    CHECK_FALSE(report.committedAnomaly);
    CHECK(report.commits.size() == 1); // one appended Save == one committed state
    CHECK(report.commits.back() == firstCommit + 1);
    // The reopened document carries the edit.
    CHECK(contentMap(*reopen(path)) == contentMap(*doc));
}

TEST_CASE("commit-append Save: several Saves each add one commit and the history grows") {
    auto doc = makeDoc("uuid-multi");
    const std::string path = (testDir() / "multi.mosaic").string();
    Anchor anchor = fullWriteAndArm(*doc, path);
    const std::uint64_t base = anchor.tip.commitId;

    paintTile(firstRaster(*doc), 1, 0, 50);
    CHECK(commitAppendSave(*doc, path, anchor) == 1);
    paintTile(firstRaster(*doc), 2, 1, 90);
    CHECK(commitAppendSave(*doc, path, anchor) == 1);
    paintTile(firstRaster(*doc), 0, 2, 200);
    CHECK(commitAppendSave(*doc, path, anchor) == 1);

    const OpenReport report = openDocument(readBytes(path));
    CHECK_FALSE(report.committedAnomaly);
    REQUIRE(report.commits.size() == 3);
    CHECK(report.commits[0] == base + 1);
    CHECK(report.commits[1] == base + 2);
    CHECK(report.commits[2] == base + 3);
    CHECK(report.tip.commitId == base + 3);
    CHECK(contentMap(*reopen(path)) == contentMap(*doc));
}

TEST_CASE("commit-append Save: an unchanged Save writes nothing (byte-identical file)") {
    auto doc = makeDoc("uuid-noop");
    const std::string path = (testDir() / "noop.mosaic").string();
    Anchor anchor = fullWriteAndArm(*doc, path);
    const std::vector<std::uint8_t> before = readBytes(path);

    // Nothing changed since the save: the differ is empty, so the app skips the write entirely.
    CHECK(commitAppendSave(*doc, path, anchor) == 0);
    CHECK(readBytes(path) == before); // the file is left exactly as it was
}

TEST_CASE("commit-append Save: an erase carries through as a tombstone on reopen") {
    auto doc = makeDoc("uuid-erase"); // tile (0,0) painted + committed by the full write
    const std::string path = (testDir() / "erase.mosaic").string();
    Anchor anchor = fullWriteAndArm(*doc, path);

    eraseTile(firstRaster(*doc), 0, 0); // erase the committed tile back to transparent
    const std::size_t chunks = commitAppendSave(*doc, path, anchor);
    CHECK(chunks == 1); // the tombstone tile

    // Highest-generation-wins composition would keep the committed painted tile; the appended
    // tombstone (a higher generation) must override it.
    auto restored = reopen(path);
    CHECK(contentMap(*restored) == contentMap(*doc));
    CHECK(firstRaster(*restored).image().rgba[3] == 0); // top-left is transparent again
}

TEST_CASE("commit-append Save: a structural edit re-emits the manifest and reopens with the layer") {
    auto doc = makeDoc("uuid-struct");
    const std::string path = (testDir() / "struct.mosaic").string();
    Anchor anchor = fullWriteAndArm(*doc, path);

    auto extra = doc->makeRaster("Second", 200, 150); // add a whole layer: a structural change
    paintTile(*extra, 1, 1, 77);
    doc->root().addOnTop(std::move(extra));

    const std::size_t chunks = commitAppendSave(*doc, path, anchor);
    CHECK(chunks >= 2); // the re-emitted manifest + the new layer's painted tile

    auto restored = reopen(path);
    CHECK(restored->layerCount() == 2);
    CHECK(contentMap(*restored) == contentMap(*doc));
}
