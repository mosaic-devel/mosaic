#include "ui/loaded_history.hpp"

#include "core/command.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "io/mosaic/compaction.hpp"
#include "io/mosaic/docio.hpp"
#include "io/mosaic/file.hpp"
#include "io/mosaic/journal_session.hpp" // diffDocumentStates
#include "io/mosaic/save.hpp"
#include "ui/loaded_delta_command.hpp"

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

// The per-key LiveUndoModel (spec 3.5): a loaded save history walks the document between saved
// states by patching only each save's dirty keys (LoadedDeltaCommand + applyChunksToDocument),
// falling back to the whole-tree LoadedStateCommand for a structural/mask save. The decisive test
// is the WALK: building a real multi-save .mosaic and checking that jumping to every position
// reproduces the exact document a full reconstruction at that generation would give -- both
// directions -- and that a full round trip returns byte-identical.
namespace {

using namespace mosaic;
using namespace mosaic::io::native;
namespace fs = std::filesystem;

fs::path testDir() {
    const char* env = std::getenv("TMPDIR");
    const fs::path dir = fs::path(env != nullptr ? env : "/tmp") / "mosaic_loaded_delta";
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
    doc->setUuid("uuid-walk");
    doc->setTitle("Loaded Delta");
    auto raster = doc->makeRaster("Paint", 200, 150);
    paintTile(*raster, 0, 0, 10);
    doc->root().addOnTop(std::move(raster));
    return doc;
}

// The pixel/vector content of a document as a comparable chunk set (TILE + VECT). Deliberately
// EXCLUDES the manifest: a whole-tree undo does not roll back Document::nextLayerId (keeping the
// newest, collision-safe value is correct), so the manifest can differ benignly between a walked
// state and a fresh reconstruction; structure is compared via treeNames instead.
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

// The FULL chunk set (manifest included), for the round-trip integrity check at the newest state.
ChunkMap fullMap(const core::Document& doc) {
    auto in = buildDocumentCheckpoint(doc);
    REQUIRE(in.has_value());
    ChunkMap m;
    for (const FileChunk& c : in->chunks)
        m[{c.type, c.key.bytes}] = {c.flags, c.payload};
    return m;
}

std::vector<std::string> treeNames(const core::Document& doc) {
    std::vector<std::string> out;
    for (const auto& c : doc.root().children())
        out.push_back(c->name());
    return out;
}

// The commit-append Save recipe (as MainWindow::commitAppendSave / test_mosaic_commit_append): diff
// the live doc against the last-durable baseline and append that one state, advancing the tip.
void appendSave(const std::string& path, const core::Document& doc, CommitTip& tip,
                CheckpointInput& baseline) {
    const CheckpointInput after = *buildDocumentCheckpoint(doc);
    std::vector<StateChunk> dirty = diffDocumentStates(baseline, after);
    REQUIRE_FALSE(dirty.empty());
    SaveState st;
    st.stateId = tip.commitId + 1;
    st.chunks = std::move(dirty);
    std::string err;
    REQUIRE_MESSAGE(appendSaveToFile(path, tip, {&st, 1}, &err) == SaveStatus::Ok, err);
    baseline = after;
}

std::unique_ptr<core::Document> reopen(const std::string& path) {
    auto res = documentFromReport(openDocument(readBytes(path)));
    REQUIRE(res.has_value());
    return std::move(res->document);
}

std::unique_ptr<core::Document> fromCheckpoint(const CheckpointInput& in) {
    auto res = documentFromReport(openDocument(buildCheckpoint(in)));
    REQUIRE(res.has_value());
    return std::move(res->document);
}

// A full reconstruction of the state at (and below) generation `maxGen` -- the ground truth a
// walked state must match. maxGen == 0 -> the bare checkpoint (below the first save). Frames are
// gathered from wherever they live: current content, the checkpoint's retained history (after a
// compaction folded the append region in), and the committed region itself.
std::unique_ptr<core::Document> reconstructAt(const OpenReport& report, std::uint64_t maxGen) {
    OpenReport synth = report;
    synth.base.chunks.clear();
    synth.base.retained.clear();
    synth.committed.clear();
    synth.commits.clear();
    for (const std::vector<RecoveredChunk>* list :
         {&report.base.chunks, &report.base.retained, &report.committed})
        for (const RecoveredChunk& c : *list)
            if (c.type != kTypeHist && c.generation <= maxGen)
                synth.base.chunks.push_back(c); // documentFromReport resolves highest-gen-wins
    auto res = documentFromReport(synth);
    REQUIRE(res.has_value());
    return std::move(res->document);
}

// The document's content + structure at each position of an adopted history, position 0 (below the
// oldest save) through position size() (the newest saved state).
using WalkPositions = std::vector<std::pair<ChunkMap, std::vector<std::string>>>;
WalkPositions walkAllPositions(core::Document& doc) {
    WalkPositions out;
    for (std::size_t p = 0; p <= doc.commands().size(); ++p) {
        doc.commands().jumpTo(p);
        out.emplace_back(tileVectMap(doc), treeNames(doc));
    }
    return out;
}

} // namespace

TEST_CASE("applyChunksToDocument patches a tile in place and clears on an empty payload") {
    auto doc = makeDoc();
    const CheckpointInput base = *buildDocumentCheckpoint(*doc);
    paintTile(rasterNamed(*doc, "Paint"), 1, 0, 90); // a new tile
    const CheckpointInput after = *buildDocumentCheckpoint(*doc);

    // Apply the forward diff onto a fresh reopen of the base -> reaches `after`.
    auto live = fromCheckpoint(base);
    const std::vector<StateChunk> fwd = diffDocumentStates(base, after);
    applyChunksToDocument(*live, fwd);
    CHECK(tileVectMap(*live) == tileVectMap(*doc));

    // An empty-payload TILE clears that cell back to transparent -> reaches the base again.
    const core::LayerId lid = rasterNamed(*doc, "Paint").id();
    ChunkKey k = tileKey(lid, 1, 0);
    std::vector<StateChunk> clear{StateChunk{kTypeTile, k, {}, 0}};
    applyChunksToDocument(*live, clear);
    CHECK(tileVectMap(*live) == tileVectMap(*fromCheckpoint(base)));
}

TEST_CASE("LoadedDeltaCommand apply/undo moves the document between two states") {
    auto doc = makeDoc();
    const CheckpointInput s0 = *buildDocumentCheckpoint(*doc);
    paintTile(rasterNamed(*doc, "Paint"), 2, 2, 150);
    const CheckpointInput s1 = *buildDocumentCheckpoint(*doc);

    std::vector<StateChunk> after = diffDocumentStates(s0, s1); // s0 -> s1
    std::vector<StateChunk> before = diffDocumentStates(s1, s0); // s1 -> s0
    ui::LoadedDeltaCommand cmd("Edited Paint", std::move(before), std::move(after));

    // The live doc sits at s1; undo() -> s0, apply() (redo) -> s1.
    auto live = fromCheckpoint(s1);
    cmd.undo(*live);
    CHECK(tileVectMap(*live) == tileVectMap(*fromCheckpoint(s0)));
    cmd.apply(*live);
    CHECK(tileVectMap(*live) == tileVectMap(*fromCheckpoint(s1)));
}

TEST_CASE("loaded history: the per-key walk reproduces every saved state, both directions") {
    auto doc = makeDoc();
    const std::string path = (testDir() / "walk.mosaic").string();

    // Full write, then a mix of content saves (per-key delta) and a structural save (whole-tree).
    std::string err;
    REQUIRE(writeFileAtomic(path, buildCheckpoint(*buildDocumentCheckpoint(*doc)), &err));
    CommitTip tip = openDocument(readBytes(path)).tip;
    REQUIRE(stampTipIdentity(path, tip, &err));
    CheckpointInput baseline = *buildDocumentCheckpoint(*doc);

    paintTile(rasterNamed(*doc, "Paint"), 1, 0, 40); // save 1: content
    appendSave(path, *doc, tip, baseline);
    paintTile(rasterNamed(*doc, "Paint"), 2, 1, 80); // save 2: content
    appendSave(path, *doc, tip, baseline);
    auto extra = doc->makeRaster("Layer2", 200, 150); // save 3: STRUCTURAL (add a layer)
    paintTile(*extra, 0, 0, 120);
    doc->root().addOnTop(std::move(extra));
    appendSave(path, *doc, tip, baseline);
    paintTile(rasterNamed(*doc, "Layer2"), 1, 1, 200); // save 4: content on the new layer
    appendSave(path, *doc, tip, baseline);

    // Open at the newest state and adopt the loaded history.
    const OpenReport report = openDocument(readBytes(path));
    REQUIRE(report.commits.size() == 4);
    auto live = reopen(path);
    auto history = ui::buildLoadedHistory(
        report, [](std::uint64_t g) { return "save " + std::to_string(g); });
    REQUIRE(history.size() == 4);
    const ChunkMap newestContent = fullMap(*live); // full set incl. manifest, for the round trip
    live->commands().adoptHistory(std::move(history));
    REQUIRE(live->commands().isSaved()); // freshly opened at the newest state = clean
    const std::size_t n = live->commands().size();
    REQUIRE(n == 4);

    const std::vector<std::uint64_t> gens = report.commits; // ascending, one per save

    // Walk DOWN newest->oldest, checking each position against a full reconstruction.
    for (std::size_t p = n; p-- > 0;) {
        live->commands().jumpTo(p);
        const std::uint64_t maxGen = p == 0 ? 0 : gens[p - 1];
        auto truth = reconstructAt(report, maxGen);
        CAPTURE(p);
        CHECK(tileVectMap(*live) == tileVectMap(*truth));
        CHECK(treeNames(*live) == treeNames(*truth));
        CHECK(live->commands().isSaved() == (p == n)); // clean only at the saved (newest) position
    }
    // ...and back UP oldest->newest.
    for (std::size_t p = 1; p <= n; ++p) {
        live->commands().jumpTo(p);
        auto truth = reconstructAt(report, gens[p - 1]);
        CAPTURE(p);
        CHECK(tileVectMap(*live) == tileVectMap(*truth));
        CHECK(treeNames(*live) == treeNames(*truth));
    }

    // Back at the newest state, the FULL serialization (manifest included) is byte-identical -- a
    // whole round trip through the mixed per-key/whole-tree history corrupts nothing.
    CHECK(fullMap(*live) == newestContent);
    CHECK(live->commands().isSaved());
}

namespace {
// A minimal closed-triangle vector object whose second node's x varies the geometry -- enough to
// make a VECT chunk change between saves without needing a fill/stroke variant.
core::vec::Object triObject(double x) {
    core::vec::Object o;
    core::vec::Path path;
    core::vec::SubPath sp;
    sp.closed = true;
    sp.nodes.push_back({{0, 0}, {0, 0}, {0, 0}, core::vec::Node::Type::Corner});
    sp.nodes.push_back({{x, 0}, {x, 0}, {x, 0}, core::vec::Node::Type::Corner});
    sp.nodes.push_back({{15, 25}, {15, 25}, {15, 25}, core::vec::Node::Type::Corner});
    path.subpaths.push_back(sp);
    o.geometry = path;
    return o;
}
core::VectorLayer& vectorNamed(core::Document& doc, const std::string& name) {
    for (const auto& c : doc.root().children())
        if (c->name() == name)
            if (auto* v = c->as<core::VectorLayer>())
                return *v;
    FAIL("no vector named " << name);
    return *doc.root().child(0).as<core::VectorLayer>();
}
} // namespace

TEST_CASE("loaded history: a vector-geometry edit is walked per-key (VECT delta)") {
    auto doc = makeDoc(); // raster "Paint"
    auto vec = doc->makeVector("Shape");
    vec->setObject(triObject(30));
    doc->root().addOnTop(std::move(vec));

    const std::string path = (testDir() / "vect.mosaic").string();
    std::string err;
    REQUIRE(writeFileAtomic(path, buildCheckpoint(*buildDocumentCheckpoint(*doc)), &err));
    CommitTip tip = openDocument(readBytes(path)).tip;
    REQUIRE(stampTipIdentity(path, tip, &err));
    CheckpointInput baseline = *buildDocumentCheckpoint(*doc);

    // A save that edits ONLY the vector's geometry: the VECT chunk changes, has_object stays true,
    // so the manifest is unchanged -> a content (per-key) step, not structural.
    vectorNamed(*doc, "Shape").setObject(triObject(45));
    appendSave(path, *doc, tip, baseline);

    const OpenReport report = openDocument(readBytes(path));
    auto live = reopen(path);
    auto history = ui::buildLoadedHistory(report, [](std::uint64_t g) { return std::to_string(g); });
    REQUIRE(history.size() == 1);
    live->commands().adoptHistory(std::move(history));

    // Newest = the edited geometry; undo restores the original; redo re-applies. The VECT chunk
    // rides in tileVectMap, so this compares the actual serialized geometry at each state.
    CHECK(tileVectMap(*live) == tileVectMap(*reconstructAt(report, report.commits.back())));
    live->commands().jumpTo(0);
    CHECK(tileVectMap(*live) == tileVectMap(*reconstructAt(report, 0)));
    live->commands().jumpTo(1);
    CHECK(tileVectMap(*live) == tileVectMap(*reconstructAt(report, report.commits.back())));
}

TEST_CASE("loaded history: a save that adds a layer is walked whole-tree, content per-key") {
    // Same shape as above but assert the classification: 3 content saves are LoadedDeltaCommands,
    // the structural one is a LoadedStateCommand -- verified by behaviour (the layer appears/
    // disappears across that one step while the raster content rolls per-key elsewhere).
    auto doc = makeDoc();
    const std::string path = (testDir() / "classify.mosaic").string();
    std::string err;
    REQUIRE(writeFileAtomic(path, buildCheckpoint(*buildDocumentCheckpoint(*doc)), &err));
    CommitTip tip = openDocument(readBytes(path)).tip;
    REQUIRE(stampTipIdentity(path, tip, &err));
    CheckpointInput baseline = *buildDocumentCheckpoint(*doc);

    paintTile(rasterNamed(*doc, "Paint"), 1, 1, 60); // save 1: content
    appendSave(path, *doc, tip, baseline);
    auto extra = doc->makeRaster("Added", 200, 150); // save 2: structural
    paintTile(*extra, 2, 2, 130);
    doc->root().addOnTop(std::move(extra));
    appendSave(path, *doc, tip, baseline);

    const OpenReport report = openDocument(readBytes(path));
    auto live = reopen(path);
    auto history = ui::buildLoadedHistory(report, [](std::uint64_t g) { return std::to_string(g); });
    REQUIRE(history.size() == 2);
    live->commands().adoptHistory(std::move(history));

    CHECK(treeNames(*live) == std::vector<std::string>{"Paint", "Added"}); // newest: both layers
    live->commands().jumpTo(1); // undo the structural save -> the added layer is gone
    CHECK(treeNames(*live) == std::vector<std::string>{"Paint"});
    live->commands().jumpTo(0); // undo the content save -> still one layer, older content
    CHECK(treeNames(*live) == std::vector<std::string>{"Paint"});
    live->commands().jumpTo(2); // redo everything
    CHECK(treeNames(*live) == std::vector<std::string>{"Paint", "Added"});
}

TEST_CASE("loaded history: a COMPACTED file walks its history exactly as before the fold") {
    // The point of history-preserving compaction. Once the parity debt trips, a Save folds the
    // committed region into the checkpoint (spec 2.6/3.3) -- report.commits goes empty, and every
    // saved state is reached through the checkpoint's retained history instead. The user must not
    // be able to tell: the History panel walks the same steps to the same documents. This is what a
    // plain full write, which serializes only the newest state, would have destroyed.
    auto doc = makeDoc();
    const std::string path = (testDir() / "walk_compacted.mosaic").string();

    std::string err;
    REQUIRE(writeFileAtomic(path, buildCheckpoint(*buildDocumentCheckpoint(*doc)), &err));
    CommitTip tip = openDocument(readBytes(path)).tip;
    REQUIRE(stampTipIdentity(path, tip, &err));
    CheckpointInput baseline = *buildDocumentCheckpoint(*doc);

    paintTile(rasterNamed(*doc, "Paint"), 1, 0, 40); // save 1: content
    appendSave(path, *doc, tip, baseline);
    paintTile(rasterNamed(*doc, "Paint"), 2, 1, 80); // save 2: content
    appendSave(path, *doc, tip, baseline);
    auto extra = doc->makeRaster("Layer2", 200, 150); // save 3: STRUCTURAL (add a layer)
    paintTile(*extra, 0, 0, 120);
    doc->root().addOnTop(std::move(extra));
    appendSave(path, *doc, tip, baseline);
    paintTile(rasterNamed(*doc, "Layer2"), 1, 1, 200); // save 4: content on the new layer
    appendSave(path, *doc, tip, baseline);

    // The walk over the un-compacted file: the control this must reproduce.
    const OpenReport before = openDocument(readBytes(path));
    REQUIRE(before.commits.size() == 4);
    REQUIRE(before.base.retained.empty());
    auto control = reopen(path);
    auto controlHistory =
        ui::buildLoadedHistory(before, [](std::uint64_t g) { return "save " + std::to_string(g); });
    REQUIRE(controlHistory.size() == 4);
    control->commands().adoptHistory(std::move(controlHistory));
    const WalkPositions expected = walkAllPositions(*control);
    REQUIRE(expected.size() == 5); // below-save-1, then one per save

    // The threshold Save: fold the file, committing a fifth state on the way, exactly as
    // MainWindow::compactionSave does.
    paintTile(rasterNamed(*doc, "Paint"), 3, 1, 160);
    const CheckpointInput after = *buildDocumentCheckpoint(*doc);
    const std::vector<StateChunk> dirty = diffDocumentStates(baseline, after);
    REQUIRE_FALSE(dirty.empty());
    const auto folded = buildCompactedCheckpoint(readBytes(path), before, dirty, &err);
    REQUIRE_MESSAGE(folded.has_value(), err);
    REQUIRE(writeFileAtomic(path, folded->bytes, &err));

    const OpenReport post = openDocument(readBytes(path));
    CHECK(post.commits.empty());        // the append region was folded away...
    CHECK(!post.base.retained.empty()); // ...and its states now live in the checkpoint
    CHECK(post.base.lostHistoryEntries == 0);
    CHECK(post.base.lostEntries == 0);

    auto live = reopen(path);
    auto history =
        ui::buildLoadedHistory(post, [](std::uint64_t g) { return "save " + std::to_string(g); });
    REQUIRE(history.size() == 5); // the four folded saves + the one the fold itself committed
    live->commands().adoptHistory(std::move(history));
    REQUIRE(live->commands().isSaved()); // freshly opened at the newest state = clean

    // Every position the un-compacted file could reach, reached identically -- including position 3,
    // the structural save, whose whole-tree snapshot is now rebuilt from retained frames.
    for (std::size_t p = 0; p < expected.size(); ++p) {
        CAPTURE(p);
        live->commands().jumpTo(p);
        CHECK(tileVectMap(*live) == expected[p].first);
        CHECK(treeNames(*live) == expected[p].second);
        CHECK_FALSE(live->commands().isSaved()); // clean only at the newest, which is position 5
    }

    // ...and the newest position is the state the fold committed.
    live->commands().jumpTo(5);
    CHECK(tileVectMap(*live) == tileVectMap(*doc));
    CHECK(treeNames(*live) == treeNames(*doc));
    CHECK(live->commands().isSaved());

    // Each state also reconstructs from the folded file alone, with no walk involved.
    const auto postStates = ui::loadedStates(post);
    REQUIRE(postStates.has_value());
    for (const ui::LoadedState& st : *postStates) {
        CAPTURE(st.generation);
        auto truth = reconstructAt(post, st.generation);
        live->commands().jumpTo(static_cast<std::size_t>(st.generation));
        CHECK(tileVectMap(*live) == tileVectMap(*truth));
    }
}

TEST_CASE("loaded history: states in the checkpoint and the append region compose into one walk") {
    // A compacted file that has been saved again since: its oldest states live in the checkpoint's
    // retained history, its newest in the committed append region. Neither source may be assumed.
    auto doc = makeDoc();
    const std::string path = (testDir() / "walk_mixed.mosaic").string();

    std::string err;
    REQUIRE(writeFileAtomic(path, buildCheckpoint(*buildDocumentCheckpoint(*doc)), &err));
    CommitTip tip = openDocument(readBytes(path)).tip;
    REQUIRE(stampTipIdentity(path, tip, &err));
    CheckpointInput baseline = *buildDocumentCheckpoint(*doc);

    paintTile(rasterNamed(*doc, "Paint"), 1, 0, 40);
    appendSave(path, *doc, tip, baseline);
    paintTile(rasterNamed(*doc, "Paint"), 2, 1, 80);
    appendSave(path, *doc, tip, baseline);

    // Fold with nothing to commit (a pure parity refresh): states 1 and 2 move into the checkpoint.
    const auto folded = buildCompactedCheckpoint(readBytes(path), openDocument(readBytes(path)), {},
                                                 &err);
    REQUIRE_MESSAGE(folded.has_value(), err);
    REQUIRE(writeFileAtomic(path, folded->bytes, &err));

    // Re-arm and keep saving: states 3 and 4 land in the fresh append region.
    tip = openDocument(readBytes(path)).tip;
    REQUIRE(stampTipIdentity(path, tip, &err));
    baseline = *buildDocumentCheckpoint(*doc);
    auto extra = doc->makeRaster("Layer2", 200, 150); // STRUCTURAL, after the fold
    paintTile(*extra, 0, 0, 120);
    doc->root().addOnTop(std::move(extra));
    appendSave(path, *doc, tip, baseline);
    paintTile(rasterNamed(*doc, "Layer2"), 1, 1, 200);
    appendSave(path, *doc, tip, baseline);

    const OpenReport report = openDocument(readBytes(path));
    CHECK(report.commits.size() == 2);        // the two saves since the fold...
    CHECK(!report.base.retained.empty());     // ...and the two folded into the checkpoint
    const auto resolved = ui::loadedStates(report);
    REQUIRE(resolved.has_value());
    const std::vector<ui::LoadedState>& states = *resolved;
    REQUIRE(states.size() == 4);
    CHECK(states[0].generation == 1);
    CHECK(states[3].generation == 4);

    auto live = reopen(path);
    auto history = ui::buildLoadedHistory(
        report, [](std::uint64_t g) { return "save " + std::to_string(g); });
    REQUIRE(history.size() == 4);
    live->commands().adoptHistory(std::move(history));

    // Walk down and back up, checking every position against a full reconstruction. A history built
    // from only one region would have 2 steps here, not 4, and the walk would land on the wrong
    // document at every position below the fold.
    for (std::size_t p = live->commands().size() + 1; p-- > 0;) {
        live->commands().jumpTo(p);
        CAPTURE(p);
        auto truth = reconstructAt(report, static_cast<std::uint64_t>(p));
        CHECK(tileVectMap(*live) == tileVectMap(*truth));
        CHECK(treeNames(*live) == treeNames(*truth));
    }
    for (std::size_t p = 1; p <= live->commands().size(); ++p) {
        live->commands().jumpTo(p);
        CAPTURE(p);
        auto truth = reconstructAt(report, static_cast<std::uint64_t>(p));
        CHECK(tileVectMap(*live) == tileVectMap(*truth));
        CHECK(treeNames(*live) == treeNames(*truth));
    }
    CHECK(live->commands().isSaved());
}

TEST_CASE("loaded history: a full-scan open offers no history rather than a wrong one") {
    // Without a directory, the reader collapses by highest generation and cannot separate history
    // from content -- its HIST frames survive but their states' content does not. loadedStates must
    // decline: a walk built from them would report every state as today's pixels. Flow 3e already
    // tells the user the save history is gone.
    auto doc = makeDoc();
    const std::string path = (testDir() / "walk_fullscan.mosaic").string();

    std::string err;
    REQUIRE(writeFileAtomic(path, buildCheckpoint(*buildDocumentCheckpoint(*doc)), &err));
    CommitTip tip = openDocument(readBytes(path)).tip;
    REQUIRE(stampTipIdentity(path, tip, &err));
    CheckpointInput baseline = *buildDocumentCheckpoint(*doc);
    paintTile(rasterNamed(*doc, "Paint"), 1, 0, 40);
    appendSave(path, *doc, tip, baseline);

    std::vector<std::uint8_t> bytes = readBytes(path);
    for (const ChunkRecord& r : scanChunks(bytes))
        if (r.valid && r.type == kTypeRoot)
            bytes[r.payloadOffset + 2] ^= 0xFF; // destroy every root replica

    const OpenReport report = openDocument(bytes);
    REQUIRE(report.base.usedFullScan);
    // A value, holding no states: "this file has no history" -- NOT "its history is unreadable".
    const auto states = ui::loadedStates(report);
    REQUIRE(states.has_value());
    CHECK(states->empty());
    CHECK(ui::buildLoadedHistory(report, [](std::uint64_t) { return std::string("x"); }).empty());
}

TEST_CASE("loaded history: a rotted retained frame declines the whole walk, and says so") {
    // History carries no parity (spec 3.8), so a rotted retained frame is permanent. It cannot be
    // walked AROUND either: a later step's undo target IS that frame, so a partial history would
    // move the document to content the state never held. loadedStates must decline with nullopt --
    // distinguishable from "no history", because the two say different things to the user.
    auto doc = makeDoc();
    const std::string path = (testDir() / "walk_rotted.mosaic").string();

    std::string err;
    REQUIRE(writeFileAtomic(path, buildCheckpoint(*buildDocumentCheckpoint(*doc)), &err));
    CommitTip tip = openDocument(readBytes(path)).tip;
    REQUIRE(stampTipIdentity(path, tip, &err));
    CheckpointInput baseline = *buildDocumentCheckpoint(*doc);
    // Repaint the tile makeDoc() already painted, twice. That leaves BOTH kinds of retained frame:
    // the generation-0 base value (a seed, named by no HIST dirty list) and the generation-1 value
    // (save 1's own frame, superseded by save 2, and named by HIST 1).
    paintTile(rasterNamed(*doc, "Paint"), 0, 0, 40);
    appendSave(path, *doc, tip, baseline);
    paintTile(rasterNamed(*doc, "Paint"), 0, 0, 90);
    appendSave(path, *doc, tip, baseline);

    const auto folded =
        buildCompactedCheckpoint(readBytes(path), openDocument(readBytes(path)), {}, &err);
    REQUIRE_MESSAGE(folded.has_value(), err);
    std::vector<std::uint8_t> bytes = folded->bytes;

    // Rot the superseded (generation-1) tile: retained history, uncovered by parity.
    const OpenReport clean = openDocument(bytes);
    REQUIRE(ui::loadedStates(clean).has_value());
    REQUIRE(ui::loadedStates(clean)->size() == 2);
    bool hit = false;
    for (const RecoveredChunk& c : clean.base.retained)
        if (!hit && c.type == kTypeTile && c.generation == 1) {
            REQUIRE(c.frameLen > 0);
            const auto rec = parseChunkAt(bytes, c.frameOffset);
            REQUIRE(rec.has_value());
            bytes[rec->payloadOffset] ^= 0xFF;
            hit = true;
        }
    REQUIRE(hit);

    SUBCASE("a rotted SEED frame -- named by no HIST dirty list -- also declines") {
        // The insidious one. A generation-0 base tile that a later save overwrote is retained
        // history, but no state's dirty list names it: it is the value the FIRST save that touched
        // that key undoes TO. A per-key check over the dirty lists cannot see it go missing, and
        // its absence reads as "this key was blank below", so undoing would CLEAR the tile instead
        // of restoring it. Caught by corpus fixture 13, which rots exactly this frame.
        std::vector<std::uint8_t> seedRot = folded->bytes;
        bool seedHit = false;
        for (const RecoveredChunk& c : clean.base.retained)
            if (!seedHit && c.type == kTypeTile && c.generation == 0) {
                const auto rec = parseChunkAt(seedRot, c.frameOffset);
                REQUIRE(rec.has_value());
                seedRot[rec->payloadOffset] ^= 0xFF;
                seedHit = true;
            }
        REQUIRE(seedHit); // the base value of the tile saves 1 and 2 repainted

        const OpenReport seedRotted = openDocument(seedRot);
        CHECK(seedRotted.base.lostEntries == 0);
        CHECK(seedRotted.base.lostHistoryEntries == 1);
        CHECK_FALSE(ui::loadedStates(seedRotted).has_value());
        auto whole = documentFromReport(seedRotted); // the document is still perfect
        REQUIRE(whole.has_value());
        CHECK(tileVectMap(*whole->document) == tileVectMap(*doc));
    }

    const OpenReport rotted = openDocument(bytes);
    CHECK(rotted.base.lostEntries == 0);          // the DOCUMENT is byte-perfect...
    CHECK(rotted.base.lostHistoryEntries == 1);   // ...only an old undo state died
    CHECK(rotted.base.rsReconstructed == 0);      // and parity cannot bring it back (spec 3.8)
    CHECK_FALSE(ui::loadedStates(rotted).has_value()); // history present, unreadable
    CHECK(ui::buildLoadedHistory(rotted, [](std::uint64_t) { return std::string("x"); }).empty());

    // The document itself still opens, whole: the newest content never depended on that frame.
    auto live = documentFromReport(rotted);
    REQUIRE(live.has_value());
    CHECK(live->rejectedChunks == 0);
    CHECK(tileVectMap(*live->document) == tileVectMap(*doc));
}
