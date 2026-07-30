#include "io/mosaic/preview.hpp"

#include "io/mosaic/fileinfo.hpp"

#include "core/document.hpp"
#include "core/layer.hpp"
#include "io/mosaic/chunk.hpp"
#include "io/mosaic/compaction.hpp"
#include "io/mosaic/docio.hpp"
#include "io/mosaic/file.hpp"
#include "io/mosaic/journal_session.hpp" // diffDocumentStates
#include "io/mosaic/records.hpp"
#include "io/mosaic/save.hpp"
#include "ui/loaded_history.hpp"

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
#include <utility>
#include <vector>

// The PRVW chunk (S48-b): an embedded 256px preview of the composite, written as an ordinary
// chunk of buildDocumentCheckpoint so diffDocumentStates decides when a Save actually writes
// one. A preview is DERIVED, not document content, and three rules enforce it -- each pinned
// here because breaking any one of them silently poisons a different subsystem:
//   A. loadedStates/buildLoadedHistory SKIP PRVW dirty keys (or every compacted file's history
//      reads as unreadable over a frame that was dropped on purpose);
//   B. applyChunksToDocument never receives one (it counts an arrival as rejected);
//   C. compaction DROPS superseded previews instead of retaining them as undo states.
namespace {

using namespace mosaic;
using namespace mosaic::io::native;
namespace fs = std::filesystem;

fs::path testDir() {
    const char* env = std::getenv("TMPDIR");
    const fs::path dir = fs::path(env != nullptr ? env : "/tmp") / "mosaic_preview";
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

core::RasterLayer& rasterNamed(core::Document& doc, const std::string& name) {
    for (const auto& c : doc.root().children())
        if (c->name() == name)
            if (auto* r = c->as<core::RasterLayer>())
                return *r;
    FAIL("no raster named " << name);
    return *doc.root().child(0).as<core::RasterLayer>();
}

std::unique_ptr<core::Document> makeDoc() {
    auto doc = std::make_unique<core::Document>(200, 150);
    doc->setUuid("uuid-preview");
    doc->setTitle("Preview");
    auto raster = doc->makeRaster("Paint", 200, 150);
    paintTile(*raster, 0, 0, 10);
    doc->root().addOnTop(std::move(raster));
    return doc;
}

// A deterministic stand-in for the app's composite: the seed varies the pixels, the size stays
// larger than kPreviewEdge so the downscale path is what gets exercised.
common::Image fakeComposite(std::uint8_t seed, std::uint32_t w = 400, std::uint32_t h = 300) {
    common::Image img(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
            img.rgba[p + 0] = static_cast<std::uint8_t>(seed + x);
            img.rgba[p + 1] = static_cast<std::uint8_t>(seed * 3 + y);
            img.rgba[p + 2] = static_cast<std::uint8_t>(seed ^ (x + y));
            img.rgba[p + 3] = 255;
        }
    return img;
}

std::size_t countPreviews(const std::vector<FileChunk>& chunks) {
    std::size_t n = 0;
    for (const FileChunk& c : chunks)
        n += c.type == kTypePreview ? 1 : 0;
    return n;
}

// The commit-append Save recipe (as MainWindow::commitAppendSave), preview riding along.
void appendSave(const std::string& path, const core::Document& doc, CommitTip& tip,
                CheckpointInput& baseline, const common::Image* preview) {
    const CheckpointInput after = *buildDocumentCheckpoint(doc, nullptr, preview);
    std::vector<StateChunk> dirty = diffDocumentStates(baseline, after);
    REQUIRE_FALSE(dirty.empty());
    SaveState st;
    st.stateId = tip.commitId + 1;
    st.chunks = std::move(dirty);
    std::string err;
    REQUIRE_MESSAGE(appendSaveToFile(path, tip, {&st, 1}, &err) == SaveStatus::Ok, err);
    baseline = after;
}

// TILE+VECT content as a comparable map (manifest excluded), as test_loaded_delta.cpp compares.
using ChunkMap = std::map<std::pair<ChunkTag, std::array<std::uint8_t, 16>>,
                          std::pair<std::uint8_t, std::vector<std::uint8_t>>>;
ChunkMap tileVectMap(const core::Document& doc) {
    auto in = buildDocumentCheckpoint(doc);
    REQUIRE(in.has_value());
    ChunkMap m;
    for (const FileChunk& c : in->chunks)
        if (c.type == kTypeTile || c.type == kTypeVector)
            m[{c.type, c.key.bytes}] = {c.flags, c.payload};
    return m;
}

} // namespace

TEST_CASE("mosaic preview: payload round-trips; downscale fits 256 and never upscales") {
    // Non-square, alpha included: the codec must carry all four channels exactly.
    common::Image small(37, 23);
    for (std::size_t i = 0; i < small.rgba.size(); ++i)
        small.rgba[i] = static_cast<std::uint8_t>(i * 7 + 3);
    const auto decoded = decodePreviewPayload(encodePreviewPayload(small));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == small);

    // 400x300 -> longest edge 256, aspect kept.
    const common::Image scaled = downscalePreview(fakeComposite(1), kPreviewEdge);
    CHECK(scaled.width == 256);
    CHECK(scaled.height == 192);
    // Already small stays at its own size -- freedesktop requests downscale, never upscale.
    CHECK(downscalePreview(small, kPreviewEdge) == small);

    // Transparency is honoured, not averaged as if it had colour: a half-transparent-black,
    // half-opaque-white source box must not darken toward the transparent half's RGB.
    common::Image edge(2, 1);
    edge.rgba = {0, 0, 0, 0, 255, 255, 255, 255};
    const common::Image one = downscalePreview(edge, 1);
    REQUIRE(one.width == 1);
    CHECK(one.rgba[0] == 255); // colour comes only from covered pixels...
    CHECK(one.rgba[3] == 128); // ...alpha is the honest area mean

    // Hostile payloads: truncated header, size/dimension disagreement, zero dims.
    CHECK_FALSE(decodePreviewPayload(std::vector<std::uint8_t>(4, 0)).has_value());
    std::vector<std::uint8_t> lying = encodePreviewPayload(small);
    lying.pop_back();
    CHECK_FALSE(decodePreviewPayload(lying).has_value());
    std::vector<std::uint8_t> zero(kPreviewHeaderSize, 0);
    CHECK_FALSE(decodePreviewPayload(zero).has_value());
}

TEST_CASE("mosaic preview: buildDocumentCheckpoint emits one Max-profile PRVW when supplied") {
    auto doc = makeDoc();
    const common::Image composite = fakeComposite(5);

    // Without a composite: no preview chunk at all (the journal path builds this way).
    const auto plain = buildDocumentCheckpoint(*doc);
    REQUIRE(plain.has_value());
    CHECK(countPreviews(plain->chunks) == 0);

    const auto in = buildDocumentCheckpoint(*doc, nullptr, &composite);
    REQUIRE(in.has_value());
    REQUIRE(countPreviews(in->chunks) == 1);
    const FileChunk* prvw = nullptr;
    for (const FileChunk& c : in->chunks)
        if (c.type == kTypePreview)
            prvw = &c;
    REQUIRE(prvw != nullptr);
    CHECK(prvw->key == zeroKey());
    CHECK(prvw->profile == Profile::Max); // written once, read many (spec 2.4)
    CHECK((prvw->flags & kFlagFiltered) != 0);
    CHECK_FALSE(prvw->parity); // spec 2.7: parity stripes tile/vector content only
    const auto img = decodePreviewPayload(prvw->payload);
    REQUIRE(img.has_value());
    CHECK(*img == downscalePreview(composite, kPreviewEdge));

    // The full write carries it, and the reader utility finds it.
    const auto read = newestPreviewInFile(buildCheckpoint(*in));
    REQUIRE(read.has_value());
    CHECK(*read == downscalePreview(composite, kPreviewEdge));
}

TEST_CASE("mosaic preview: the differ decides -- an unchanged downscale writes nothing") {
    auto doc = makeDoc();
    const common::Image a = fakeComposite(5);
    const common::Image b = fakeComposite(90);

    const CheckpointInput withA = *buildDocumentCheckpoint(*doc, nullptr, &a);
    const CheckpointInput withA2 = *buildDocumentCheckpoint(*doc, nullptr, &a);
    const CheckpointInput withB = *buildDocumentCheckpoint(*doc, nullptr, &b);

    CHECK(diffDocumentStates(withA, withA2).empty()); // same content, same composite: no bytes
    const std::vector<StateChunk> dirty = diffDocumentStates(withA, withB);
    REQUIRE(dirty.size() == 1); // only the preview changed
    CHECK(dirty[0].type == kTypePreview);

    // A composite that stops arriving (composite failure) neither diffs nor tombstones: PRVW is
    // not a TILE, so nothing is emitted and the file keeps its stored preview.
    CHECK(diffDocumentStates(withA, *buildDocumentCheckpoint(*doc)).empty());
}

TEST_CASE("mosaic preview: seeding the baseline from the file skips unchanged previews") {
    auto doc = makeDoc();
    const common::Image composite = fakeComposite(7);
    const std::string path = (testDir() / "seed.mosaic").string();
    std::string err;
    REQUIRE(writeFileAtomic(
        path, buildCheckpoint(*buildDocumentCheckpoint(*doc, nullptr, &composite)), &err));

    // The checkpoint's copy: seed a preview-less baseline and diff against the same composite.
    {
        const OpenReport report = openDocument(readBytes(path));
        CheckpointInput seeded = *buildDocumentCheckpoint(*doc);
        seedPreviewFromReport(seeded, report);
        REQUIRE(countPreviews(seeded.chunks) == 1);
        const CheckpointInput after = *buildDocumentCheckpoint(*doc, nullptr, &composite);
        CHECK(diffDocumentStates(seeded, after).empty()); // unchanged across sessions: no bytes
    }

    // The committed-region copy: an appended frame carries kFlagLinked on the wire, which
    // seeding must normalize away or the flag compare calls every unchanged preview dirty.
    CommitTip tip = openDocument(readBytes(path)).tip;
    REQUIRE(stampTipIdentity(path, tip, &err));
    CheckpointInput baseline = *buildDocumentCheckpoint(*doc, nullptr, &composite);
    paintTile(rasterNamed(*doc, "Paint"), 1, 0, 40);
    const common::Image composite2 = fakeComposite(8);
    appendSave(path, *doc, tip, baseline, &composite2);
    {
        const OpenReport report = openDocument(readBytes(path));
        CheckpointInput seeded = *buildDocumentCheckpoint(*doc);
        seedPreviewFromReport(seeded, report);
        REQUIRE(countPreviews(seeded.chunks) == 1);

        // Same composite again -> the diff is EMPTY: nothing to save.
        const CheckpointInput after = *buildDocumentCheckpoint(*doc, nullptr, &composite2);
        CHECK(diffDocumentStates(seeded, after).empty());
        // A changed composite -> exactly the preview is dirty.
        const common::Image changed = fakeComposite(21);
        const CheckpointInput after2 = *buildDocumentCheckpoint(*doc, nullptr, &changed);
        const std::vector<StateChunk> dirty = diffDocumentStates(seeded, after2);
        REQUIRE(dirty.size() == 1);
        CHECK(dirty[0].type == kTypePreview);
    }

    // A preview-less file seeds nothing.
    auto bare = makeDoc();
    CheckpointInput noSeed = *buildDocumentCheckpoint(*bare);
    seedPreviewFromReport(noSeed, openDocument(buildCheckpoint(*buildDocumentCheckpoint(*bare))));
    CHECK(countPreviews(noSeed.chunks) == 0);
}

TEST_CASE("mosaic preview: the newest PRVW wins, and a pre-preview file reads as none") {
    auto doc = makeDoc();
    const common::Image a = fakeComposite(10);
    const common::Image b = fakeComposite(60);
    const std::string path = (testDir() / "newest.mosaic").string();
    std::string err;
    REQUIRE(writeFileAtomic(path, buildCheckpoint(*buildDocumentCheckpoint(*doc, nullptr, &a)),
                            &err));
    CommitTip tip = openDocument(readBytes(path)).tip;
    REQUIRE(stampTipIdentity(path, tip, &err));
    CheckpointInput baseline = *buildDocumentCheckpoint(*doc, nullptr, &a);
    paintTile(rasterNamed(*doc, "Paint"), 1, 0, 40);
    appendSave(path, *doc, tip, baseline, &b);

    const auto newest = readNewestPreview(path, &err);
    REQUIRE_MESSAGE(newest.has_value(), err);
    CHECK(*newest == downscalePreview(b, kPreviewEdge));

    // A file written without previews (every pre-S48-b .mosaic): nullopt, with words.
    const std::string old = (testDir() / "legacy.mosaic").string();
    REQUIRE(writeFileAtomic(old, buildCheckpoint(*buildDocumentCheckpoint(*makeDoc())), &err));
    std::string why;
    CHECK_FALSE(readNewestPreview(old, &why).has_value());
    CHECK_FALSE(why.empty());
    CHECK_FALSE(readNewestPreview((testDir() / "missing.mosaic").string()).has_value());
}

TEST_CASE("mosaic preview: RULE B -- applyChunksToDocument rejects a PRVW, never applies it") {
    auto doc = makeDoc();
    const ChunkMap before = tileVectMap(*doc);

    StateChunk prvw;
    prvw.type = kTypePreview;
    prvw.key = zeroKey();
    prvw.payload = encodePreviewPayload(downscalePreview(fakeComposite(3), kPreviewEdge));
    prvw.flags = kFlagCritical | kFlagFiltered;

    std::size_t rejected = 0;
    applyChunksToDocument(*doc, {&prvw, 1}, &rejected);
    CHECK(rejected == 1);              // counted -- a pipeline bug must be visible...
    CHECK(tileVectMap(*doc) == before); // ...and the document untouched
}

TEST_CASE("mosaic preview: RULE A + C -- history skips PRVW keys; compaction drops superseded "
          "previews") {
    auto doc = makeDoc();
    const common::Image a = fakeComposite(10);
    const common::Image b = fakeComposite(60);
    const common::Image c = fakeComposite(200);
    const std::string path = (testDir() / "rules.mosaic").string();

    std::string err;
    REQUIRE(writeFileAtomic(path, buildCheckpoint(*buildDocumentCheckpoint(*doc, nullptr, &a)),
                            &err));
    CommitTip tip = openDocument(readBytes(path)).tip;
    REQUIRE(stampTipIdentity(path, tip, &err));
    CheckpointInput baseline = *buildDocumentCheckpoint(*doc, nullptr, &a);

    paintTile(rasterNamed(*doc, "Paint"), 1, 0, 40); // save 1: content + a changed preview
    appendSave(path, *doc, tip, baseline, &b);

    // The save's HIST record honestly names the PRVW it wrote...
    const OpenReport open1 = openDocument(readBytes(path));
    bool histNamesPreview = false;
    for (const RecoveredChunk& rc : open1.committed)
        if (rc.type == kTypeHist)
            if (const auto rec = parseHistRecord(rc.payload); rec.has_value())
                for (const DirtyKey& d : rec->dirty)
                    histNamesPreview = histNamesPreview || d.type == kTypePreview;
    CHECK(histNamesPreview);

    // ...and RULE A: the resolved states carry NO preview frame -- so nothing downstream
    // (LoadedDeltaCommand -> applyChunksToDocument) can ever be handed one.
    const auto states1 = ui::loadedStates(open1);
    REQUIRE(states1.has_value());
    REQUIRE(states1->size() == 1);
    for (const ui::LoadedState& st : *states1)
        for (const ui::LoadedChunk& rc : st.chunks)
            CHECK(rc.type != kTypePreview);

    // RULE C: fold the file, a third preview riding the compaction's own edit. The superseded
    // previews (checkpoint A, committed B) must be DROPPED -- not retained as undo states.
    paintTile(rasterNamed(*doc, "Paint"), 2, 1, 80);
    const CheckpointInput after = *buildDocumentCheckpoint(*doc, nullptr, &c);
    const std::vector<StateChunk> dirty = diffDocumentStates(baseline, after);
    REQUIRE_FALSE(dirty.empty());
    const auto folded = buildCompactedCheckpoint(readBytes(path), open1, dirty, &err);
    REQUIRE_MESSAGE(folded.has_value(), err);
    REQUIRE(writeFileAtomic(path, folded->bytes, &err));

    // Exactly ONE preview frame survives in the whole file image: the newest.
    std::size_t prvwFrames = 0;
    for (const ChunkRecord& rec : scanChunks(folded->bytes))
        prvwFrames += (rec.valid && rec.type == kTypePreview) ? 1 : 0;
    CHECK(prvwFrames == 1);
    const auto newest = newestPreviewInFile(folded->bytes);
    REQUIRE(newest.has_value());
    CHECK(*newest == downscalePreview(c, kPreviewEdge));

    const OpenReport open2 = openDocument(readBytes(path));
    CHECK(open2.base.lostEntries == 0);
    CHECK(open2.base.lostHistoryEntries == 0);
    for (const RecoveredChunk& rc : open2.base.retained)
        CHECK(rc.type != kTypePreview); // dropped, not folded in as history

    // The A x C interplay, the decisive check: both saves' HIST records still name PRVW keys
    // whose frames no longer exist. Without the skip, loadedStates would declare this perfectly
    // healthy history unreadable (nullopt); with it, the walk stands.
    const auto states2 = ui::loadedStates(open2);
    REQUIRE(states2.has_value());
    REQUIRE(states2->size() == 2); // save 1 + the state the fold committed
    auto history = ui::buildLoadedHistory(open2, [](std::uint64_t g) {
        return "save " + std::to_string(g);
    });
    REQUIRE(history.size() == 2);

    // And the walk actually moves the document: down to the base, back to the tip.
    auto res = documentFromReport(open2, &err);
    REQUIRE_MESSAGE(res.has_value(), err);
    core::Document& live = *res->document;
    const ChunkMap tipContent = tileVectMap(live);
    live.commands().adoptHistory(std::move(history));
    live.commands().jumpTo(0);
    CHECK(tileVectMap(live) != tipContent);
    live.commands().jumpTo(2);
    CHECK(tileVectMap(live) == tipContent);
}

TEST_CASE("mosaic fileinfo: the light reader reports the newest manifest's canvas + colour") {
    auto doc = makeDoc(); // 200x150
    doc->setDpi(240.0);
    const std::string path = (testDir() / "fileinfo.mosaic").string();
    std::string err;
    REQUIRE(writeFileAtomic(path, buildCheckpoint(*buildDocumentCheckpoint(*doc)), &err));

    const auto info = readDocumentInfo(path);
    REQUIRE(info.has_value());
    CHECK(info->width == 200);
    CHECK(info->height == 150);
    CHECK(info->dpi == 240.0);
    CHECK(info->title == "Preview"); // the document's own name (the recents card's Name seed)
    REQUIRE(info->colorSpace.has_value());
    CHECK(*info->colorSpace == doc->colorSpace());
    REQUIRE(info->precision.has_value());
    CHECK(*info->precision == doc->precision());

    // Absent file / empty bytes: honest nullopt, never a throw.
    CHECK(!readDocumentInfo((testDir() / "absent.mosaic").string()).has_value());
    CHECK(!documentInfoInFile({}).has_value());

    // Newest generation wins: append a save with a changed dpi and the reader must report the
    // NEW manifest, not the first one it happens to meet in the scan.
    CommitTip tip = openDocument(readBytes(path)).tip;
    REQUIRE(stampTipIdentity(path, tip, &err));
    CheckpointInput baseline = *buildDocumentCheckpoint(*doc);
    doc->setDpi(300.0);
    appendSave(path, *doc, tip, baseline, nullptr);
    const auto info2 = readDocumentInfo(path);
    REQUIRE(info2.has_value());
    CHECK(info2->dpi == 300.0);
}
