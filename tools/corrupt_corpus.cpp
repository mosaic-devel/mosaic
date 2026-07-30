// corrupt_corpus -- emit a directory of damaged Build-1 .mosaic files, one per recovery
// scenario (docs/askortell-dialog.md flows 3a-3e + the dual-writer root conflict), for manual
// testing of the real reader through the real app. Replaces ~/Desktop/corrupted, which predates
// Round 10 and conforms to the Python prototype's wire format -- against the real reader it
// exercises only garbage-rejection.
//
// Every file is SELF-VERIFIED after writing: the tool re-reads it from disk and asserts the
// reader reaches the exact verdict the scenario exists to demonstrate (repaired counts, lost
// entries, conservative stop, salvage lineages, root conflict). A corpus that drifts from the
// reader fails loudly here instead of silently mis-teaching a manual test.
//
// Journal fixtures (flows 1/2) are PLANTED, never shipped: a journal keys on the absolute document
// path + $XDG_STATE_HOME, so it is only meaningful next to the exact file it was planted for. Pass
// --plant to also write 10-journal-restore.mosaic + 11-journal-orphan.mosaic AND their matching
// recovery journals under $XDG_STATE_HOME (default off, so the ctest self-check never touches app
// state). Move a planted file and its journal stops matching -- that is the orphan case by
// construction. The advisory-lock fixture (flow 6) arrives with that slice.
//
// Cases 12-14 cover history-preserving COMPACTION (spec 2.6/3.3): a folded file has no committed
// region at all, so its save history is reached through the checkpoint's retained frames instead.
// 13 rots one of those frames -- uncovered by parity by design (spec 3.8) -- to prove a lost undo
// state is reported as exactly that, and never as a damaged document.
//
// --plant also PRUNES the recovery journals and advisory locks left behind by earlier generations
// of this corpus (each run mints fresh uuids, so the previous run's <uuid>-<pathhash> entries are
// orphaned). Matching is by the path-hash of a file this tool writes, and nothing else.
//
// Usage: corrupt_corpus <output-dir> [--plant]

#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/text/text_model.hpp"
#include "core/vector/object.hpp"
#include "io/mosaic/blob.hpp"
#include "io/mosaic/chunk.hpp"
#include "io/mosaic/compaction.hpp"
#include "io/mosaic/docio.hpp"
#include "io/mosaic/file.hpp"
#include "io/mosaic/journal.hpp"
#include "io/mosaic/journal_session.hpp"
#include "io/mosaic/records.hpp"
#include "io/mosaic/reedsolomon.hpp"
#include "io/mosaic/salvage.hpp"
#include "io/mosaic/save.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace io = mosaic::io::native;
namespace core = mosaic::core;
namespace common = mosaic::common;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

std::vector<std::uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool writeFile(const fs::path& p, const std::vector<std::uint8_t>& bytes) {
    std::string err;
    if (!io::writeFileAtomic(p.string(), bytes, &err)) {
        std::printf("  FAIL  writing %s: %s\n", p.string().c_str(), err.c_str());
        ++g_failures;
        return false;
    }
    return true;
}

// ---- the base document --------------------------------------------------------------------
// A real 1920x1080 poster of five worked layers, so the corpus tests the reader against genuine
// document content -- not a toy. Bottom to top: a full-coverage "Sky" gradient backdrop (~510
// non-uniform tiles -- dozens of k=8 parity stripes), a painted "Hills" silhouette across the
// lower third, a sparse translucent "Grain" texture, a "Shapes" vector layer (a parametric star
// sun, solid-filled + stroked), and a 3D-extruded "Title" text layer. Every appended save (see
// applyEdit) then performs one more real, visible edit into a DIFFERENT layer, so the seeded
// history is genuine and a save destroyed mid-history removes specific content the eye can name
// while the saves past it come back intact.

constexpr int kCanvasW = 1920;
constexpr int kCanvasH = 1080;

struct Rgb {
    std::uint8_t r, g, b;
};

std::uint8_t* pixel(common::Image& img, int x, int y) {
    return img.rgba.data() + (static_cast<std::size_t>(y) * img.width + x) * 4;
}

// Fill the opaque disc of radius `radius` centred at (cx,cy) with `c` -- the round mark edits paint.
void paintDisc(core::RasterLayer& layer, int cx, int cy, int radius, Rgb c) {
    auto& img = layer.image();
    const int w = static_cast<int>(img.width);
    const int h = static_cast<int>(img.height);
    const long r2 = static_cast<long>(radius) * radius;
    for (int y = std::max(0, cy - radius); y < std::min(h, cy + radius); ++y)
        for (int x = std::max(0, cx - radius); x < std::min(w, cx + radius); ++x) {
            const long dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > r2)
                continue;
            std::uint8_t* px = pixel(img, x, y);
            px[0] = c.r;
            px[1] = c.g;
            px[2] = c.b;
            px[3] = 255;
        }
}

template <class L>
L* findLayer(core::Document& doc, const std::string& name) {
    for (const auto& child : doc.root().children())
        if (child->name() == name)
            return static_cast<L*>(child.get());
    return nullptr;
}

// The single object a VectorLayer holds: a star "sun", solid-filled and dark-stroked. The star is
// layer-local around the origin; the layer transform places it on the canvas.
core::vec::Object sunObject(int points, Rgb fill) {
    core::vec::Object o;
    core::vec::StarShape star;
    star.points = points;
    star.outerRadius = 150;
    star.innerRadius = 70;
    o.geometry = core::vec::ParametricShape{star};
    o.fill = core::vec::SolidPaint{{fill.r / 255.0f, fill.g / 255.0f, fill.b / 255.0f, 1.0f}};
    o.stroke.enabled = true;
    o.stroke.width = 6.0;
    o.stroke.paint = core::vec::SolidPaint{{0.15f, 0.10f, 0.05f, 1.0f}};
    return o;
}

// A bold 3D-extruded headline block.
core::text::TextBlock titleBlock(const std::string& words, float depth) {
    core::text::CharStyle base;
    base.font.family = "Inter";
    base.font.weight = 800.0f;
    base.sizePx = 180.0f;
    base.tracking = 6.0f;
    core::text::TextBlock b = core::text::makeBlock(words, base);
    core::text::Extrude ex;
    ex.depth = depth;
    ex.material.albedo = {0.85f, 0.70f, 0.20f, 1.0f};
    ex.material.metalness = 0.6f;
    ex.perspective = 18.0f;
    b.extrude = ex;
    return b;
}

std::unique_ptr<core::Document> buildBaseDocument() {
    auto doc = std::make_unique<core::Document>(kCanvasW, kCanvasH);
    doc->setTitle("Poster");
    doc->setUuid(io::mintDocumentUuid());

    // 1. Sky: full-coverage vertical gradient, a faint per-pixel dither so every tile is distinct.
    auto sky = doc->makeRaster("Sky", kCanvasW, kCanvasH);
    {
        auto& img = sky->image();
        for (int y = 0; y < kCanvasH; ++y) {
            const float t = static_cast<float>(y) / (kCanvasH - 1);
            for (int x = 0; x < kCanvasW; ++x) {
                std::uint8_t* px = pixel(img, x, y);
                const int d = (x * 13 + y * 7) & 7;
                px[0] = static_cast<std::uint8_t>(30 + t * t * 200.0f + d);
                px[1] = static_cast<std::uint8_t>(45 + t * 140.0f + d);
                px[2] = static_cast<std::uint8_t>(120 + (1.0f - t) * 110.0f);
                px[3] = 255;
            }
        }
    }
    doc->root().addOnTop(std::move(sky));

    // 2. Hills: an opaque rolling silhouette across the lower third.
    auto hills = doc->makeRaster("Hills", kCanvasW, kCanvasH);
    {
        auto& img = hills->image();
        for (int x = 0; x < kCanvasW; ++x) {
            const double hy = kCanvasH * 0.60 + 70.0 * std::sin(x * 0.006) +
                              35.0 * std::sin(x * 0.017 + 1.0);
            for (int y = static_cast<int>(hy); y < kCanvasH; ++y) {
                std::uint8_t* px = pixel(img, x, y);
                px[0] = 34;
                px[1] = static_cast<std::uint8_t>(70 + (y - hy) * 0.04);
                px[2] = 48;
                px[3] = 255;
            }
        }
    }
    doc->root().addOnTop(std::move(hills));

    // 3. Grain: a sparse translucent texture, confined to a top-right "light leak" region so it
    // touches only a corner of tiles rather than the whole canvas.
    auto grain = doc->makeRaster("Grain", kCanvasW, kCanvasH);
    grain->setOpacity(0.35f);
    grain->setBlendMode(core::BlendMode::Screen);
    {
        auto& img = grain->image();
        for (int y = 0; y < kCanvasH * 2 / 5; ++y)
            for (int x = kCanvasW * 3 / 5; x < kCanvasW; ++x) {
                if (((x + y) / 5) % 3 != 0) // diagonal stipple
                    continue;
                std::uint8_t* px = pixel(img, x, y);
                const std::uint8_t n = static_cast<std::uint8_t>((x * 7 + y * 131) & 0xFF);
                px[0] = n;
                px[1] = n;
                px[2] = n;
                px[3] = 90;
            }
    }
    doc->root().addOnTop(std::move(grain));

    // 4. Shapes: the star "sun", warm-filled, placed top-right.
    auto shapes = doc->makeVector("Shapes");
    shapes->setObject(sunObject(12, {255, 210, 90}));
    shapes->setTransform(common::Affine2D::trs({1500, 320}, 0.0, {1, 1}));
    doc->root().addOnTop(std::move(shapes));

    // 5. Title: 3D-extruded headline, lower-left.
    auto title = doc->makeText("Title");
    title->setBlock(titleBlock("MOSAIC", 22.0f));
    title->setAutoNamed(false);
    title->setTransform(common::Affine2D::trs({140, 760}, 0.0, {1, 1}));
    doc->root().addOnTop(std::move(title));

    return doc;
}

// The seeded edit history: one real, visible edit per save, each into a DIFFERENT layer, so a
// save lost mid-history removes specific content the eye can name. Save ids are 1-based; index =
// stateId - 1. Four edits cover every scenario's history depth.
void applyEdit(core::Document& doc, int index) {
    switch (index) {
    case 0: // save 1: a bright sun-glow painted into the Sky, behind the star
        paintDisc(*findLayer<core::RasterLayer>(doc, "Sky"), 1500, 320, 120, {255, 245, 200});
        break;
    case 1: // save 2: the Shapes star gains points and turns cool
        findLayer<core::VectorLayer>(doc, "Shapes")->setObject(sunObject(20, {150, 210, 255}));
        break;
    case 2: // save 3: a dark tree painted onto the Hills
        paintDisc(*findLayer<core::RasterLayer>(doc, "Hills"), 470, 880, 70, {30, 80, 40});
        break;
    default: // save 4+: the 3D Title is rewritten and extruded deeper
        findLayer<core::TextLayer>(doc, "Title")->setBlock(titleBlock("MOSAIC 26", 40.0f));
        break;
    }
}

std::vector<std::uint8_t> checkpointBytes(const core::Document& doc) {
    std::string err;
    const auto input = io::buildDocumentCheckpoint(doc, &err);
    if (!input.has_value()) {
        std::printf("  FAIL  buildDocumentCheckpoint: %s\n", err.c_str());
        ++g_failures;
        return {};
    }
    return io::buildCheckpoint(*input);
}

// One SPARSE SaveState: serialize the document before and after the edit and keep only the
// chunks whose bytes changed (or appeared) -- the realistic commit-append shape, and the one
// that makes a destroyed save's content GENUINELY stale in the final document (a full-content
// state after it would just rewrite every key and hide the loss). The app-side differ lands
// with the journal-autosave slice; this tool-local one only serves fixture generation.
io::SaveState diffState(const io::CheckpointInput& before, const io::CheckpointInput& after,
                        std::uint64_t stateId) {
    io::SaveState st;
    st.stateId = stateId;
    std::map<std::pair<io::ChunkTag, std::array<std::uint8_t, 16>>,
             const std::vector<std::uint8_t>*>
        prior;
    for (const auto& c : before.chunks)
        prior[{c.type, c.key.bytes}] = &c.payload;
    for (const auto& c : after.chunks) {
        const auto it = prior.find({c.type, c.key.bytes});
        if (it != prior.end() && *it->second == c.payload)
            continue; // unchanged
        st.chunks.push_back(io::StateChunk{c.type, c.key, c.payload, c.flags});
    }
    return st;
}

// A pristine in-memory reopen of the base checkpoint: every appended-save scenario starts from
// its own copy (same uuid, no bands), so each file's states hold exactly the bands its own
// saves painted -- the shared builder document would smuggle earlier scenarios' bands in.
std::unique_ptr<core::Document> freshDoc(const std::vector<std::uint8_t>& base) {
    std::string err;
    auto reread = io::documentFromReport(io::openDocument(base), &err);
    if (!reread.has_value() || reread->document == nullptr) {
        std::printf("  FAIL  reopening the base document: %s\n", err.c_str());
        ++g_failures;
        return nullptr;
    }
    return std::move(reread->document);
}

// Open a file and hand back a stamped tip, the way the app's Save path will.
bool tipFor(const fs::path& p, io::CommitTip& tip) {
    const auto bytes = readFile(p);
    const io::OpenReport rep = io::openDocument(bytes);
    if (!rep.tipValid)
        return false;
    tip = rep.tip;
    std::string err;
    return io::stampTipIdentity(p.string(), tip, &err);
}

// Append one save through the real Save API: serialize, run `edit`, serialize again, commit the
// sparse diff as the given state id; returns the file size afterwards. Taking the edit explicitly
// lets the dual-writer scenario give two writers DIFFERENT content under the same state id.
std::uintmax_t appendSaveWith(const fs::path& p, core::Document& doc, io::CommitTip& tip,
                              std::uint64_t stateId, const std::function<void()>& edit) {
    std::string err;
    const auto before = io::buildDocumentCheckpoint(doc, &err);
    edit();
    const auto after = io::buildDocumentCheckpoint(doc, &err);
    if (!before.has_value() || !after.has_value()) {
        std::printf("  FAIL  serializing for state %llu: %s\n",
                    static_cast<unsigned long long>(stateId), err.c_str());
        ++g_failures;
        return fs::file_size(p);
    }
    const io::SaveState st = diffState(*before, *after, stateId);
    const io::SaveStatus s = io::appendSaveToFile(p.string(), tip, {&st, 1}, &err);
    if (s != io::SaveStatus::Ok) {
        std::printf("  FAIL  appendSaveToFile(%s, state %llu): %s\n", p.string().c_str(),
                    static_cast<unsigned long long>(stateId), err.c_str());
        ++g_failures;
    }
    return fs::file_size(p);
}

// The ordinary save: the state's own seeded edit (state id N == applyEdit index N-1).
std::uintmax_t appendSave(const fs::path& p, core::Document& doc, io::CommitTip& tip,
                          std::uint64_t stateId) {
    return appendSaveWith(p, doc, tip, stateId,
                          [&] { applyEdit(doc, static_cast<int>(stateId) - 1); });
}

// The CHECKPOINT's parity-eligible frames in file order: the TILE and current-content VECT chunks
// the writer stripes together (docio.cpp marks both parity=true; file.cpp stripes them in this
// order), restricted to the checkpoint region (offset < wal_start). Two reasons to index HERE:
// tiles alone would break "these frames share one k=8 stripe" once vector layers interleave VECT
// frames among the tiles (frame i lives in stripe i / k); and once a file carries appended saves,
// their TILE/VECT frames live past wal_start and are NOT parity-covered (history opts out, §3.8),
// so they must be excluded or the stripe index would point at an uncovered frame.
std::vector<io::ChunkRecord> checkpointParityFrames(const std::vector<std::uint8_t>& bytes) {
    const std::size_t wal = static_cast<std::size_t>(io::openDocument(bytes).base.walStartOffset);
    std::vector<io::ChunkRecord> out;
    for (const auto& rec : io::scanChunks(bytes))
        if (rec.valid && rec.payloadOffset < wal &&
            (rec.type == io::kTypeTile || rec.type == io::kTypeVector))
            out.push_back(rec);
    return out;
}

// Corrupt one frame: flip a byte in its payload (breaks the checksum, leaves framing findable).
void corruptPayload(std::vector<std::uint8_t>& bytes, const io::ChunkRecord& rec) {
    bytes.at(rec.payloadOffset) ^= 0xFF;
}

// Write `base` to `p`, then append `nSaves` real saves through the Save API so the file carries a
// genuine committed history: EVERY scenario builds on this, because a corruption fixture with no
// history exercises nothing about history recovery (and the History panel opens empty). Returns the
// editing document + leaves `tip` ready to append more (04's torn tail, 05's post-gap save).
std::unique_ptr<core::Document> seedHistory(const fs::path& p, const std::vector<std::uint8_t>& base,
                                            io::CommitTip& tip, int nSaves) {
    writeFile(p, base);
    tipFor(p, tip);
    auto doc = freshDoc(base);
    for (int s = 1; s <= nSaves; ++s)
        appendSave(p, *doc, tip, static_cast<std::uint64_t>(s));
    return doc;
}

// The saved-state ids a file's HIST records describe, wherever those records live: the committed
// append region, or the checkpoint's retained history once a compaction folded the region in
// (spec 3.3). This mirrors ui::loadedStates, which the tool cannot call -- mosaic_ui links FLTK
// and this tool deliberately does not.
std::set<std::uint64_t> historyStateIds(const io::OpenReport& r) {
    std::set<std::uint64_t> ids;
    for (const auto* list : {&r.base.retained, &r.committed})
        for (const io::RecoveredChunk& c : *list)
            if (c.type == io::kTypeHist)
                if (const auto rec = io::parseHistRecord(c.payload))
                    ids.insert(rec->state);
    return ids;
}

// The threshold Save, exactly as MainWindow::compactionSave performs it: fold the committed region
// back into the checkpoint (carrying its retained history through byte-verbatim) while this Save's
// own edit takes the next generation. `editIndex` < 0 folds with nothing to commit.
bool compactFile(const fs::path& p, core::Document& doc, int editIndex,
                 const char* forceMode = nullptr) {
    const auto bytes = readFile(p);
    const io::OpenReport pre = io::openDocument(bytes);
    std::string err;
    std::vector<io::StateChunk> dirty;
    if (editIndex >= 0) {
        const auto before = io::buildDocumentCheckpoint(doc, &err);
        applyEdit(doc, editIndex);
        const auto after = io::buildDocumentCheckpoint(doc, &err);
        if (!before.has_value() || !after.has_value()) {
            std::printf("  FAIL  serializing for the fold: %s\n", err.c_str());
            ++g_failures;
            return false;
        }
        dirty = diffState(*before, *after, pre.tip.commitId + 1).chunks;
    }
    io::CompactionOptions opts;
    if (forceMode != nullptr)
        opts.forceMode = forceMode; // the cas fixtures force H4; everything else takes hysteresis
    const auto folded = io::buildCompactedCheckpoint(bytes, pre, dirty, &err, opts);
    if (!folded.has_value()) {
        std::printf("  FAIL  buildCompactedCheckpoint: %s\n", err.c_str());
        ++g_failures;
        return false;
    }
    return writeFile(p, folded->bytes);
}

// Mirrors ui::loadedStates' RESOLUTION verdict (the tool links no FLTK, historyStateIds only
// counts records): every state's dirty entry must reach its content -- a cas reference through a
// verified BLOB or any hashed surviving frame, a journal entry through its own (TYPE, KEY,
// generation) frame -- and any loss declines the WHOLE walk, exactly like the panel (a partial
// history moves the document to content no state ever held).
bool historyResolves(const io::OpenReport& r) {
    if (r.base.lostHistoryEntries > 0)
        return false;
    std::map<std::tuple<io::ChunkTag, std::array<std::uint8_t, 16>, std::uint64_t>, bool> byVersion;
    std::set<io::BlobHash> byHash;
    for (const auto* list : {&r.base.chunks, &r.base.retained, &r.committed})
        for (const io::RecoveredChunk& c : *list) {
            if (c.type == io::kTypeHist)
                continue;
            if (c.type == io::kTypeBlob) {
                if (io::blobContentOf(c.payload).has_value()) {
                    io::BlobHash h{};
                    std::copy(c.payload.begin(), c.payload.begin() + io::kBlobHashSize, h.begin());
                    byHash.insert(h);
                }
                continue;
            }
            byVersion[{c.type, c.key.bytes, c.generation}] = true;
            byHash.insert(io::blobHashOf(c.payload));
        }
    bool any = false;
    for (const auto* list : {&r.base.retained, &r.committed})
        for (const io::RecoveredChunk& c : *list) {
            if (c.type != io::kTypeHist)
                continue;
            const auto rec = io::parseHistRecord(c.payload);
            if (!rec.has_value())
                return false; // verified but unreadable: ui::loadedStates declines the walk
            any = true;
            for (std::size_t i = 0; i < rec->dirty.size(); ++i) {
                if (rec->dirty[i].type == io::kTypePreview)
                    continue; // derived, dropped by design: never part of the walk
                const bool hasRef = i < rec->refs.size() && rec->refs[i].present;
                if (hasRef ? byHash.count(rec->refs[i].hash) == 0
                           : byVersion.count({rec->dirty[i].type, rec->dirty[i].key.bytes,
                                              rec->state}) == 0)
                    return false;
            }
        }
    return any;
}

// Is save 1's {255,245,200} sun-glow present in the Sky? Full-scan doesn't count lost entries (no
// directory to measure against), so this content probe is how the shredded case (09, edits
// destroyed -> glow GONE) is told apart from the merely structure-destroyed case (07, whose save
// frames survive full-scan -> glow present).
bool skyGlowPresent(core::Document& doc) {
    auto* sky = findLayer<core::RasterLayer>(doc, "Sky");
    if (sky == nullptr)
        return false;
    const std::uint8_t* px = pixel(sky->image(), 1500, 320);
    return px[0] > 240 && px[1] > 230 && px[2] > 180;
}

// Every fixture filename this tool generates. Used to prune the recovery state keyed to these
// exact paths -- and ONLY these, so pointing the tool at a directory holding real documents can
// never take their crash-restore data with it.
constexpr const char* kFixtureNames[] = {
    "00-pristine.mosaic",         "01-appended-saves.mosaic",
    "02-repaired-by-parity.mosaic", "03-damaged-beyond-parity.mosaic",
    "04-torn-last-save.mosaic",   "05-damaged-mid-history.mosaic",
    "06-dual-writer.mosaic",      "07-structure-destroyed.mosaic",
    "08-content-cratered.mosaic", "09-shredded.mosaic",
    "10-journal-restore.mosaic",  "11-journal-orphan.mosaic",
    "12-compacted.mosaic",        "13-compacted-history-rot.mosaic",
    "14-compacted-then-appended.mosaic", "15-manifest-replica.mosaic",
    "16-cas-folded.mosaic",       "17-cas-blob-rot.mosaic",
    "18-cas-repaired-by-parity.mosaic",
};

// Recovery journals and advisory locks are named <uuid>-<pathhash> under $XDG_STATE_HOME. Every
// regeneration mints fresh document uuids, so the PREVIOUS run's entries for these very files are
// orphaned the moment the fixtures are rewritten: inert (nothing will look them up again, because
// no document carries those uuids any more) but one more set accumulates every time.
//
// Prune them, matching only on the path-hash of a file this tool is about to write. Runs under
// --plant, the flag that already opts into touching app state, so the ctest self-check stays
// hands-off. Called BEFORE anything is generated, so the fresh journals planted later are safe.
//
// Caveat worth naming: a lock file is deliberately left in place on release (removing it races a
// concurrent acquire), so this deletes locks a live Mosaic could still be holding. Regenerating the
// corpus already overwrites the very documents such a window has open, so that window is looking at
// bytes that no longer exist either way.
std::size_t pruneRecoveryState(const fs::path& out) {
    // recoveryJournalPath spells the key as <dir>/<uuid>-<pathhash>; an empty uuid leaves exactly
    // the "-<pathhash>" suffix every entry for that path must end with.
    const auto suffixFor = [](const fs::path& p) {
        return fs::path(io::recoveryJournalPath("", fs::weakly_canonical(p).string()))
            .filename()
            .string();
    };
    const fs::path dir = fs::path(io::recoveryJournalPath("probe", "probe")).parent_path();
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return 0;

    std::vector<std::string> suffixes;
    suffixes.reserve(std::size(kFixtureNames));
    for (const char* name : kFixtureNames)
        suffixes.push_back(suffixFor(out / name));

    std::size_t removed = 0;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        const bool isLock = name.ends_with(".lock");
        const std::string key = isLock ? name.substr(0, name.size() - 5) : name;
        for (const std::string& suffix : suffixes) {
            if (key.size() <= suffix.size() || !key.ends_with(suffix))
                continue;
            std::error_code rmEc;
            if (fs::remove(e.path(), rmEc))
                ++removed;
            break;
        }
    }
    return removed;
}

// Plant the journal fixtures (flows 1/2). Each writes a CLEAN .mosaic to `out` and a matching
// recovery journal under $XDG_STATE_HOME, keyed on the file's absolute path + uuid -- so opening
// the file in the real app fires the crash-restore face. Self-verified through the reader + replay
// exactly as app_window's offerJournalRestore classifies them. Print the journal paths so a manual
// tester (and cleanup) can find them.
void plantJournalFixtures(const fs::path& out, const std::vector<std::uint8_t>& base) {
    // -- 10 (flow 1): a clean file + a journal bound to its commit, holding two unsaved changes --
    std::printf("10-journal-restore.mosaic  (+ planted journal)\n");
    {
        const fs::path p = fs::weakly_canonical(out / "10-journal-restore.mosaic");
        io::CommitTip tip;
        auto doc = seedHistory(p, base, tip, 2); // two committed saves
        if (doc != nullptr) {
            const io::OpenReport rep = io::openDocument(readFile(p));
            const std::string uuid = doc->uuid();
            const io::JournalBinding binding = io::bindingForTip(uuid, p.string(), rep.tip);
            const std::string jpath = io::recoveryJournalPath(uuid, p.string());
            auto session = io::JournalSession::begin(jpath, binding,
                                                     io::buildDocumentCheckpoint(*doc),
                                                     rep.tip.commitId + 1);
            if (!session.has_value()) {
                std::printf("  FAIL  begin journal\n");
                ++g_failures;
            } else {
                applyEdit(*doc, 2); // an unsaved tree onto the Hills
                session->autosave(*doc);
                applyEdit(*doc, 3); // an unsaved title rewrite
                session->autosave(*doc);
                const io::JournalReplay replay = io::replayJournal(readFile(jpath), binding);
                check(replay.binding == io::JournalBindingStatus::Ok &&
                          replay.states.size() == 2 && !replay.anomaly,
                      "flow 1: clean file, journal bound to its commit, two unsaved changes");
                std::printf("  journal: %s\n", jpath.c_str());
            }
        }
    }

    // -- 11 (flow 2): a journal bound to an OLD commit; the file was saved again since ------------
    std::printf("11-journal-orphan.mosaic  (+ planted stale journal)\n");
    {
        const fs::path p = fs::weakly_canonical(out / "11-journal-orphan.mosaic");
        io::CommitTip tip;
        auto doc = seedHistory(p, base, tip, 2);
        if (doc != nullptr) {
            const io::OpenReport rep1 = io::openDocument(readFile(p));
            const std::string uuid = doc->uuid();
            const io::JournalBinding oldBinding = io::bindingForTip(uuid, p.string(), rep1.tip);
            const std::string jpath = io::recoveryJournalPath(uuid, p.string());
            auto session = io::JournalSession::begin(jpath, oldBinding,
                                                     io::buildDocumentCheckpoint(*doc),
                                                     rep1.tip.commitId + 1);
            if (!session.has_value()) {
                std::printf("  FAIL  begin journal\n");
                ++g_failures;
            } else {
                applyEdit(*doc, 2);
                session->autosave(*doc); // one unsaved change captured in the journal
                // The file moves on: a fresh save lands (a DIFFERENT edit), so the file's tip no
                // longer matches the journal's binding -- the orphan condition, by construction.
                appendSaveWith(p, *doc, tip, 3, [&] { applyEdit(*doc, 3); });
                const io::OpenReport rep2 = io::openDocument(readFile(p));
                const io::JournalBinding nowBinding = io::bindingForTip(uuid, p.string(), rep2.tip);
                const io::JournalReplay replay = io::replayJournal(readFile(jpath), nowBinding);
                check(replay.binding == io::JournalBindingStatus::WrongSeed,
                      "flow 2: journal bound to a superseded commit -> orphan");
                std::printf("  journal: %s\n", jpath.c_str());
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    bool plant = false;
    fs::path out;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--plant")
            plant = true;
        else if (out.empty())
            out = arg;
        else
            out.clear(); // a second positional: usage error
    }
    if (out.empty()) {
        std::printf("usage: corrupt_corpus <output-dir> [--plant]\n");
        return 2;
    }
    std::error_code ec;
    fs::create_directories(out, ec);
    if (ec) {
        std::printf("cannot create %s: %s\n", out.string().c_str(), ec.message().c_str());
        return 2;
    }

    if (plant) {
        // Clear the previous generation's orphaned journals/locks for these exact fixture paths
        // before minting new uuids, so repeated regeneration does not silt up the recovery dir.
        if (const std::size_t n = pruneRecoveryState(out); n > 0)
            std::printf("pruned %zu stale recovery file(s) from earlier corpus generations\n\n", n);
    }

    auto doc = buildBaseDocument();
    const std::vector<std::uint8_t> base = checkpointBytes(*doc);
    if (base.empty())
        return 1;

    // -- 00: clean file WITH history (control) -------------------------------------------------
    std::printf("00-pristine.mosaic\n");
    {
        const fs::path p = out / "00-pristine.mosaic";
        io::CommitTip tip;
        seedHistory(p, base, tip, 3); // three real saves -> a three-entry History panel
        const io::OpenReport r = io::openDocument(readFile(p));
        std::string err;
        const auto d = io::documentFromReport(r, &err);
        check(r.base.rootFound && r.base.rsReconstructed == 0 && r.base.lostEntries == 0 &&
                  !r.committedAnomaly && r.commits.size() == 3 && d.has_value() &&
                  d->rejectedChunks == 0,
              "opens clean, three saves of history, no repairs, no losses");
    }

    // -- 01: a deeper clean history (committed-region replay, no damage) ------------------------
    std::printf("01-appended-saves.mosaic\n");
    {
        const fs::path p = out / "01-appended-saves.mosaic";
        io::CommitTip tip;
        seedHistory(p, base, tip, 4); // four saves -> touches every layer (Sky/Shapes/Hills/Title)
        const io::OpenReport r = io::openDocument(readFile(p));
        std::string err;
        const auto d = io::documentFromReport(r, &err);
        check(r.commits.size() == 4 && !r.committedAnomaly && d.has_value() &&
                  d->rejectedChunks == 0,
              "replays 4 commits, newest content wins");
    }

    // -- 02 (flow 3a): parity-repairable checkpoint damage, history intact ---------------------
    std::printf("02-repaired-by-parity.mosaic\n");
    {
        const fs::path p = out / "02-repaired-by-parity.mosaic";
        io::CommitTip tip;
        seedHistory(p, base, tip, 3);
        std::vector<std::uint8_t> bytes = readFile(p);
        const auto elig = checkpointParityFrames(bytes);
        check(elig.size() >= 16, "checkpoint has enough parity-eligible frames for stripes");
        corruptPayload(bytes, elig[3]); // one erasure in stripe 0: within the m=2 budget
        writeFile(p, bytes);
        const io::OpenReport r = io::openDocument(readFile(p));
        std::string err;
        const auto d = io::documentFromReport(r, &err);
        check(r.base.rsReconstructed >= 1 && r.base.lostEntries == 0 && r.commits.size() == 3 &&
                  d.has_value() && d->rejectedChunks == 0,
              "checkpoint damage repaired by parity; history replays (app: status line only)");
    }

    // -- 03 (flow 3b): checkpoint damage beyond parity, history intact --------------------------
    std::printf("03-damaged-beyond-parity.mosaic\n");
    {
        const fs::path p = out / "03-damaged-beyond-parity.mosaic";
        io::CommitTip tip;
        seedHistory(p, base, tip, 3);
        std::vector<std::uint8_t> bytes = readFile(p);
        const auto elig = checkpointParityFrames(bytes);
        // Three erasures inside ONE k=8 stripe (stripe 1 = eligible frames [8..15]) -- one past
        // the m=2 budget, so this stripe cannot be reconstructed. These top-left checkpoint tiles
        // are in a region no save repaints, so their loss stands (a save would otherwise win).
        const std::size_t s0 = 1 * io::kRsDataShards;
        check(elig.size() >= s0 + io::kRsParityShards + 1,
              "checkpoint has a full second stripe to break");
        corruptPayload(bytes, elig[s0]);
        corruptPayload(bytes, elig[s0 + 1]);
        corruptPayload(bytes, elig[s0 + 2]);
        writeFile(p, bytes);
        const io::OpenReport r = io::openDocument(readFile(p));
        std::string err;
        const auto d = io::documentFromReport(r, &err);
        check(r.base.lostEntries >= 1 && r.commits.size() == 3 && d.has_value(),
              "checkpoint areas lost beyond parity; history replays (app: damage tell)");
    }

    // -- 04 (flow 3d): a torn save at the tail, three clean saves before it --------------------
    std::printf("04-torn-last-save.mosaic\n");
    {
        const fs::path p = out / "04-torn-last-save.mosaic";
        io::CommitTip tip;
        auto d1 = seedHistory(p, base, tip, 3); // saves 1-3 clean
        const std::uintmax_t afterSave3 = fs::file_size(p);
        const std::uintmax_t afterSave4 = appendSave(p, *d1, tip, 4);
        fs::resize_file(p, afterSave3 + (afterSave4 - afterSave3) / 2, ec); // tear save 4
        const io::OpenReport r = io::openDocument(readFile(p));
        check(r.commits.size() == 3 && r.commits.back() == 3 && r.committedAnomaly,
              "opens at the last complete save (3); the torn 4th is set aside");
    }

    // -- 05 (flow 3c): damage mid-history, a clean save before AND intact saves past it --------
    std::printf("05-damaged-mid-history.mosaic\n");
    {
        const fs::path p = out / "05-damaged-mid-history.mosaic";
        writeFile(p, base);
        io::CommitTip tip;
        tipFor(p, tip);
        auto d1 = freshDoc(base);
        const std::uintmax_t afterSave1 = appendSave(p, *d1, tip, 1); // clean
        const std::uintmax_t afterSave2 = appendSave(p, *d1, tip, 2); // to be damaged
        appendSave(p, *d1, tip, 3);                                   // clean, past the damage
        std::vector<std::uint8_t> bytes = readFile(p);
        // Destroy the MIDDLE of save 2's batch (a byte every 32 wrecks each >=46-byte frame),
        // leaving save 1 clean before it -- the canonical Round 11 B4 shape: conservative replay
        // stops at save 1 (so the panel still shows one save of history), and salvage's primary
        // lineage bridges the gap to recover save 3, flagging save 2's destroyed content.
        const std::uintmax_t batchLen = afterSave2 - afterSave1;
        for (std::uintmax_t off = afterSave1 + batchLen / 3; off < afterSave1 + 2 * batchLen / 3;
             off += 32)
            bytes[static_cast<std::size_t>(off)] ^= 0xFF;
        writeFile(p, bytes);
        const io::OpenReport r = io::openDocument(readFile(p));
        check(r.commits.size() == 1 && r.commits[0] == 1 && r.committedAnomaly,
              "conservative open stops at save 1 (before the mid-history damage)");
        const io::SalvageReport s = io::salvageLinkedRegion(
            bytes, static_cast<std::size_t>(r.base.walStartOffset),
            [&] {
                std::array<std::uint8_t, io::kLinkSize> seed{};
                for (std::size_t i = 0; i < io::kLinkSize; ++i)
                    seed[i] = r.base.rootChecksum[i];
                return seed;
            }());
        const io::SalvageLineage* prim = s.primary();
        const auto hasState = [&](std::uint64_t id) {
            return prim != nullptr &&
                   std::find(prim->states.begin(), prim->states.end(), id) != prim->states.end();
        };
        // Save 3 must come back past the gap; save 2's destroyed content must be HONESTLY flagged
        // (or the lineage declared imprecise when its HIST happened to land in the damage window).
        const bool salvages = prim != nullptr && prim->seedRooted && prim->bridgedGap &&
                              hasState(3) && (!prim->flagged.empty() || !prim->precise);
        check(salvages, "salvage bridges the gap: save 3 intact, save 2's loss flagged");
    }

    // -- 06 (flow 4): dual-writer root conflict ------------------------------------------------
    std::printf("06-dual-writer.mosaic\n");
    {
        const fs::path pa = out / "06-dual-writer.mosaic";
        const fs::path pb = out / ".dual-writer-b.tmp";
        writeFile(pa, base);
        writeFile(pb, base);
        io::CommitTip tipA, tipB;
        tipFor(pa, tipA);
        tipFor(pb, tipB);
        // Two writers, both reopened from the same base (same uuid, same tip) -- the real
        // double-open. Both mint state id 1, but paint the sky glow in DIFFERENT colours, so the
        // two batches carry genuinely different content (distinct checksums = two lineages).
        auto docA = freshDoc(base);
        auto docB = freshDoc(base);
        appendSave(pa, *docA, tipA, 1); // writer A: state 1 = the warm sun-glow (edit 0)
        appendSaveWith(pb, *docB, tipB, 1, [&] { // writer B: a cool-toned glow, same state id
            paintDisc(*findLayer<core::RasterLayer>(*docB, "Sky"), 1500, 320, 120, {120, 190, 255});
        });
        // Splice B's batch after A's: two seed-rooted lineages in one file.
        std::vector<std::uint8_t> bytesA = readFile(pa);
        const std::vector<std::uint8_t> bytesB = readFile(pb);
        bytesA.insert(bytesA.end(), bytesB.begin() + static_cast<std::ptrdiff_t>(base.size()),
                      bytesB.end());
        writeFile(pa, bytesA);
        fs::remove(pb, ec);
        const io::OpenReport r = io::openDocument(readFile(pa));
        check(r.commits.size() == 1, "conservative open follows exactly one lineage (D1)");
        const io::SalvageReport s = io::salvageLinkedRegion(
            bytesA, static_cast<std::size_t>(r.base.walStartOffset),
            [&] {
                std::array<std::uint8_t, io::kLinkSize> seed{};
                for (std::size_t i = 0; i < io::kLinkSize; ++i)
                    seed[i] = r.base.rootChecksum[i];
                return seed;
            }());
        check(s.rootConflict, "salvage reports the dual-writer root conflict (app: flow-4 ask)");
    }

    // -- 07 (flow 3e): structure destroyed -----------------------------------------------------
    std::printf("07-structure-destroyed.mosaic\n");
    {
        const fs::path p = out / "07-structure-destroyed.mosaic";
        io::CommitTip tip;
        seedHistory(p, base, tip, 3); // the save frames survive full-scan (highest generation wins)
        std::vector<std::uint8_t> bytes = readFile(p);
        for (const auto& rec : io::scanChunks(bytes))
            if (rec.valid && (rec.type == io::kTypeRoot || rec.type == io::kTypeDir ||
                              rec.type == io::kTypeRootPtr))
                corruptPayload(bytes, rec);
        writeFile(p, bytes);
        const io::OpenReport r = io::openDocument(readFile(p));
        std::string err;
        const auto d = io::documentFromReport(r, &err);
        // Full-scan reassembles the newest content per key (checkpoint + saves), but with no root
        // to seed the chain the discrete saves cannot be enumerated -- so this is the one file
        // whose History panel stays empty (the structure that indexes history is gone). The save
        // CONTENT still survives full-scan, though: the sun-glow is there (contrast case 09).
        check(!r.base.rootFound && r.base.usedFullScan && r.commits.empty() && d.has_value() &&
                  skyGlowPresent(*d->document),
              "no root survives; full-scan still yields the newest content incl. the edits");
    }

    // -- 08 (flow 3b, catastrophic): a crater of lost checkpoint content -----------------------
    // The reader is good enough that most fixtures open looking perfect; this one loses so much
    // that the damage is unmissable. An asteroid-sized hit destroys ~10 whole parity stripes of
    // checkpoint tiles (every frame in each -- far past the m=2 budget), so dozens of tiles read
    // transparent: a visible crater across the poster. Structure + history stay intact.
    std::printf("08-content-cratered.mosaic\n");
    {
        const fs::path p = out / "08-content-cratered.mosaic";
        io::CommitTip tip;
        seedHistory(p, base, tip, 3);
        std::vector<std::uint8_t> bytes = readFile(p);
        const auto elig = checkpointParityFrames(bytes);
        const std::size_t first = 3 * io::kRsDataShards; // leave the first stripes intact
        const std::size_t last = std::min(elig.size(), 13 * io::kRsDataShards);
        check(last > first + 40, "checkpoint has enough tiles to crater");
        for (std::size_t i = first; i < last; ++i)
            corruptPayload(bytes, elig[i]); // whole stripes wiped -> no parity can save them
        writeFile(p, bytes);
        const io::OpenReport r = io::openDocument(readFile(p));
        std::string err;
        const auto d = io::documentFromReport(r, &err);
        check(r.base.rootFound && r.base.lostEntries >= 40 && r.commits.size() == 3 &&
                  d.has_value(),
              "dozens of checkpoint tiles lost (visible crater); structure + history intact");
    }

    // -- 09 (flow 3e, catastrophic): shredded -- structure AND history destroyed ----------------
    // The worst case: the append region (every save) is wiped, the root/directory are destroyed,
    // and a stripe of the checkpoint is cratered too. Full-scan salvages what checkpoint tiles
    // survive, but the edits are gone (the document opens at a DEGRADED original, holes and all),
    // and with no root there is no history to enumerate. Recovery visibly fails here.
    std::printf("09-shredded.mosaic\n");
    {
        const fs::path p = out / "09-shredded.mosaic";
        io::CommitTip tip;
        seedHistory(p, base, tip, 3);
        std::vector<std::uint8_t> bytes = readFile(p);
        const std::size_t wal = static_cast<std::size_t>(io::openDocument(bytes).base.walStartOffset);
        // Wipe the entire committed region (all saves) + the structure + a checkpoint stripe.
        for (const auto& rec : io::scanChunks(bytes)) {
            if (!rec.valid)
                continue;
            const bool inHistory = rec.payloadOffset >= wal && rec.type != io::kTypeRoot;
            const bool structure = rec.type == io::kTypeRoot || rec.type == io::kTypeDir ||
                                   rec.type == io::kTypeRootPtr;
            if (inHistory || structure)
                corruptPayload(bytes, rec);
        }
        const auto elig = checkpointParityFrames(bytes);
        const std::size_t s0 = 2 * io::kRsDataShards;
        for (std::size_t i = s0; i < s0 + io::kRsDataShards && i < elig.size(); ++i)
            corruptPayload(bytes, elig[i]); // a cratered checkpoint stripe on top
        writeFile(p, bytes);
        const io::OpenReport r = io::openDocument(readFile(p));
        std::string err;
        const auto d = io::documentFromReport(r, &err);
        // Full-scan doesn't count lostEntries (no directory to measure against), so the proof of
        // real loss is by CONTENT: the committed edits are gone (sun-glow absent -- the document
        // opens at the degraded original), on top of the destroyed structure + unindexable history.
        check(!r.base.rootFound && r.base.usedFullScan && r.commits.empty() && d.has_value() &&
                  !skyGlowPresent(*d->document),
              "structure + history destroyed; edits gone (degraded original), no history");
    }

    // -- 12: a COMPACTED file (history folded into the checkpoint, no committed region) --------
    // The shape every long-lived .mosaic reaches: once the append region's parity debt trips
    // needsCompaction, that Save folds the file. report.commits goes EMPTY and every saved state is
    // reached through the checkpoint's retained history instead -- the History panel must not be
    // able to tell. The plain full write this replaces would have kept the pixels and dropped the
    // undo history entirely.
    std::printf("12-compacted.mosaic\n");
    {
        const fs::path p = out / "12-compacted.mosaic";
        io::CommitTip tip;
        auto doc = seedHistory(p, base, tip, 3);
        if (doc != nullptr && compactFile(p, *doc, /*editIndex=*/3)) { // fold + commit save 4
            const io::OpenReport r = io::openDocument(readFile(p));
            std::string err;
            const auto d = io::documentFromReport(r, &err);
            check(r.base.rootFound && !r.base.usedFullScan && r.commits.empty() &&
                      !r.base.retained.empty() && !r.committedAnomaly && r.base.lostEntries == 0 &&
                      r.base.lostHistoryEntries == 0 && r.base.rsReconstructed == 0 &&
                      historyStateIds(r).size() == 4 && r.tip.commitId == 4 && d.has_value() &&
                      d->rejectedChunks == 0 && skyGlowPresent(*d->document),
                  "folded: 4 saves of history in the checkpoint, no committed region, opens clean");
        }
    }

    // -- 13: a compacted file whose RETAINED history rotted ------------------------------------
    // History carries no parity by design (spec 3.8), so a rotted retained frame is permanent. The
    // document is byte-perfect; only its past died. This must NOT raise the "this file is damaged"
    // face -- and because a later step's undo target IS the missing frame, a partial history would
    // move the document to content that state never held, so the whole walk is declined.
    std::printf("13-compacted-history-rot.mosaic\n");
    {
        const fs::path p = out / "13-compacted-history-rot.mosaic";
        io::CommitTip tip;
        auto doc = seedHistory(p, base, tip, 3);
        if (doc != nullptr && compactFile(p, *doc, /*editIndex=*/-1)) { // fold, nothing to commit
            std::vector<std::uint8_t> bytes = readFile(p);
            const io::OpenReport clean = io::openDocument(bytes);
            // Rot a GENERATION-0 retained tile: the base value of a tile a later save repainted.
            // No HIST dirty list names it -- it is the value that save's undo restores TO -- so a
            // reader that only checks each state's dirty frames would miss its loss and silently
            // CLEAR the tile when the user steps below that save. The rule is drawn at "any
            // retained frame lost", which is what this fixture exists to hold in place.
            bool hit = false;
            for (const io::RecoveredChunk& c : clean.base.retained) {
                if (hit || c.type != io::kTypeTile || c.generation != 0 || c.frameLen == 0)
                    continue;
                if (const auto rec = io::parseChunkAt(bytes, c.frameOffset)) {
                    corruptPayload(bytes, *rec);
                    hit = true;
                }
            }
            check(hit, "found a generation-0 retained seed tile to rot");
            writeFile(p, bytes);
            const io::OpenReport r = io::openDocument(readFile(p));
            std::string err;
            const auto d = io::documentFromReport(r, &err);
            check(r.base.rootFound && r.base.lostEntries == 0 && r.base.lostHistoryEntries == 1 &&
                      r.base.rsReconstructed == 0 && !r.committedAnomaly && d.has_value() &&
                      d->rejectedChunks == 0 && skyGlowPresent(*d->document),
                  "document byte-perfect, one retained seed frame lost -> history declined");
        }
    }

    // -- 14: compacted, then saved again (history in BOTH regions) -----------------------------
    // The steady state of a working document: old saves folded into the checkpoint, newer ones
    // appended after it. A History panel that reads only one region shows half the walk, and lands
    // on the wrong document at every position below the fold.
    std::printf("14-compacted-then-appended.mosaic\n");
    {
        const fs::path p = out / "14-compacted-then-appended.mosaic";
        io::CommitTip tip;
        auto doc = seedHistory(p, base, tip, 2);
        if (doc != nullptr && compactFile(p, *doc, /*editIndex=*/-1) && tipFor(p, tip)) {
            appendSave(p, *doc, tip, 3); // the tree onto the Hills
            appendSave(p, *doc, tip, 4); // the 3D title rewritten
            const io::OpenReport r = io::openDocument(readFile(p));
            std::string err;
            const auto d = io::documentFromReport(r, &err);
            check(r.base.rootFound && r.commits.size() == 2 && !r.base.retained.empty() &&
                      !r.committedAnomaly && r.base.lostEntries == 0 &&
                      r.base.lostHistoryEntries == 0 && historyStateIds(r).size() == 4 &&
                      d.has_value() && d->rejectedChunks == 0 && skyGlowPresent(*d->document),
                  "2 folded saves + 2 appended = one 4-step history across both regions");
        }
    }

    // -- 15: the manifest's primary copy destroyed; its replica carries the document -----------
    // The manifest is the one chunk without which nothing opens -- and it is ~500 bytes of a 744KB
    // file. A user feeding random corruption found that flipping a single byte in it made the whole
    // document unopenable while every other frame stayed valid. It now ships twice, far apart. This
    // fixture must open perfectly, with NO dialog and no damage counted.
    std::printf("15-manifest-replica.mosaic\n");
    {
        const fs::path p = out / "15-manifest-replica.mosaic";
        io::CommitTip tip;
        seedHistory(p, base, tip, 3);
        std::vector<std::uint8_t> bytes = readFile(p);
        std::vector<io::ChunkRecord> copies;
        for (const auto& rec : io::scanChunks(bytes))
            if (rec.valid && rec.type == io::kTypeManifest)
                copies.push_back(rec);
        check(copies.size() == 2, "the checkpoint ships two manifest copies");
        check(!copies.empty() && copies.back().offset - copies.front().offset > 64 * 1024,
              "the copies are far enough apart that one burst cannot take both");
        if (copies.size() == 2) {
            corruptPayload(bytes, copies.front()); // destroy the PRIMARY
            writeFile(p, bytes);
            const io::OpenReport r = io::openDocument(readFile(p));
            std::string err;
            const auto d = io::documentFromReport(r, &err);
            check(r.base.rootFound && !r.base.usedFullScan && r.base.lostEntries == 0 &&
                      r.base.lostHistoryEntries == 0 && !r.committedAnomaly &&
                      r.commits.size() == 3 && d.has_value() && d->rejectedChunks == 0 &&
                      d->document->layerCount() == 5 && skyGlowPresent(*d->document),
                  "the replica answers: opens clean, nothing lost, history intact");
        }
    }

    // -- 16: a cas-mode (H4) fold -- content-addressed history, references to current content --
    // Build 2's second history encoding (spec 3.9): the fold re-spells every retained state's
    // dirty entries as content-hash references. With one edit per layer nothing repeats, so every
    // reference resolves to the parity-covered CURRENT frame (dedup against current content) --
    // the dominant real-world shape. The recovery ladder and the History panel must not be able
    // to tell the encodings apart on a clean file.
    std::printf("16-cas-folded.mosaic\n");
    {
        const fs::path p = out / "16-cas-folded.mosaic";
        io::CommitTip tip;
        auto doc = seedHistory(p, base, tip, 3);
        if (doc != nullptr && compactFile(p, *doc, /*editIndex=*/3, io::kModeCas)) {
            const io::OpenReport r = io::openDocument(readFile(p));
            std::string err;
            const auto d = io::documentFromReport(r, &err);
            bool refs = false;
            for (const io::RecoveredChunk& c : r.base.retained)
                if (c.type == io::kTypeHist)
                    if (const auto rec = io::parseHistRecord(c.payload))
                        refs = refs || !rec->refs.empty();
            check(r.base.rootFound && r.base.mode == io::kModeCas && refs && r.commits.empty() &&
                      !r.committedAnomaly && r.base.lostEntries == 0 &&
                      r.base.lostHistoryEntries == 0 && historyStateIds(r).size() == 4 &&
                      historyResolves(r) && d.has_value() && d->rejectedChunks == 0 &&
                      skyGlowPresent(*d->document),
                  "cas fold: 4 saves as hash references, opens clean, walk resolves");
        }
    }

    // -- 17: a cas file whose BLOB rotted --------------------------------------------------------
    // The title is rewritten (save 4) and then written BACK (save 5), so save 4's text lives only
    // as a hash-keyed BLOB after the fold -- history-only content, deliberately outside parity
    // (spec 3.8). Rotting it must read exactly like 13: the document is byte-perfect, NO dialog,
    // one lost undo state counted by identity, and the whole walk declined.
    std::printf("17-cas-blob-rot.mosaic\n");
    {
        const fs::path p = out / "17-cas-blob-rot.mosaic";
        io::CommitTip tip;
        auto doc = seedHistory(p, base, tip, 4); // saves 1-4; save 4 rewrites the Title
        if (doc != nullptr) {
            appendSaveWith(p, *doc, tip, 5, [&] { // save 5: the title written BACK
                findLayer<core::TextLayer>(*doc, "Title")->setBlock(titleBlock("MOSAIC", 22.0f));
            });
            if (compactFile(p, *doc, /*editIndex=*/-1, io::kModeCas)) {
                std::vector<std::uint8_t> bytes = readFile(p);
                bool hit = false;
                for (const auto& rec : io::scanChunks(bytes))
                    if (!hit && rec.valid && rec.type == io::kTypeBlob) {
                        corruptPayload(bytes, rec);
                        hit = true;
                    }
                check(hit, "the cas fold stored save 4's superseded title as a BLOB");
                writeFile(p, bytes);
                const io::OpenReport r = io::openDocument(readFile(p));
                std::string err;
                const auto d = io::documentFromReport(r, &err);
                check(r.base.rootFound && r.base.mode == io::kModeCas &&
                          r.base.lostEntries == 0 && r.base.lostHistoryEntries == 1 &&
                          r.base.rsReconstructed == 0 && !r.committedAnomaly &&
                          !historyResolves(r) && d.has_value() && d->rejectedChunks == 0 &&
                          skyGlowPresent(*d->document),
                      "document byte-perfect, one blob lost (counted once) -> history declined");
            }
        }
    }

    // -- 18 (flow 3a on cas): parity repairs current content that history references ------------
    // A cas reference into current content leans on the SAME Reed-Solomon coverage the document
    // does: damage the referenced current frame and the repair restores both the pixels and the
    // walk -- the parity-posture split (current covered, history-only blobs not) working as
    // designed.
    std::printf("18-cas-repaired-by-parity.mosaic\n");
    {
        const fs::path p = out / "18-cas-repaired-by-parity.mosaic";
        io::CommitTip tip;
        auto doc = seedHistory(p, base, tip, 3);
        if (doc != nullptr && compactFile(p, *doc, /*editIndex=*/3, io::kModeCas)) {
            std::vector<std::uint8_t> bytes = readFile(p);
            const auto elig = checkpointParityFrames(bytes);
            check(elig.size() >= 16, "cas checkpoint has enough parity-eligible frames");
            corruptPayload(bytes, elig[5]); // one erasure: within the m=2 budget
            writeFile(p, bytes);
            const io::OpenReport r = io::openDocument(readFile(p));
            std::string err;
            const auto d = io::documentFromReport(r, &err);
            check(r.base.rootFound && r.base.mode == io::kModeCas &&
                      r.base.rsReconstructed >= 1 && r.base.lostEntries == 0 &&
                      r.base.lostHistoryEntries == 0 && historyResolves(r) &&
                      historyStateIds(r).size() == 4 && d.has_value() && d->rejectedChunks == 0,
                  "cas current-content damage repaired by parity; the walk survives (status line)");
        }
    }

    // -- journal fixtures (flows 1/2): planted next to their files, opt-in ----------------------
    if (plant)
        plantJournalFixtures(out, base);

    // -- README ---------------------------------------------------------------------------------
    {
        std::ofstream md(out / "README.md");
        md << "# .mosaic corrupt corpus (Build 1)\n\n"
              "Generated by `corrupt_corpus` from a real 1920x1080 poster of five worked layers: a\n"
              "full-coverage `Sky` gradient, a painted `Hills` silhouette, a translucent `Grain`\n"
              "texture, a `Shapes` vector layer (a star sun), and a 3D-extruded `Title`. EVERY file\n"
              "then carries a real committed save history (a corruption fixture without one tests\n"
              "nothing about history recovery): each save makes one edit into a different layer --\n"
              "save 1 paints a sun-glow into the Sky, save 2 recolours the star and adds points,\n"
              "save 3 paints a tree onto the Hills, save 4 rewrites the 3D title. Saves are sparse\n"
              "commit-append batches (only the changed chunks), so a destroyed save's content is\n"
              "genuinely stale, not hidden by later rewrites. Opening a file loads this history into\n"
              "the History panel (jump between saves). Every file was self-verified against the real\n"
              "reader at generation time. Open each in Mosaic (File->Open):\n\n"
              "| file | scenario | history | expected |\n"
              "|---|---|---|---|\n"
              "| 00-pristine | control | 3 saves | opens clean, no dialog; History panel shows 3 saves |\n"
              "| 01-appended-saves | committed replay | 4 saves | opens clean at the newest save; History shows 4 |\n"
              "| 02-repaired-by-parity | flow 3a | 3 saves | no dialog; status line: repaired 1 block; pixels exact |\n"
              "| 03-damaged-beyond-parity | flow 3b | 3 saves | \"This file is damaged\" tell; a stripe of checkpoint Sky tiles reads transparent |\n"
              "| 04-torn-last-save | flow 3d | 3 saves | \"The last save didn't finish\" tell; opens at save 3, the torn 4th set aside |\n"
              "| 05-damaged-mid-history | flow 3c | 1 save (conservative) | the recover ASK: [Open recovered version] brings back save 3 past the gap, save 2 flagged; [Open last complete save] = save 1 |\n"
              "| 06-dual-writer | flow 4 | 1 save (writer A) | \"Two programs saved into this file\"; opens writer A's warm glow, offers writer B's cool glow via [Save other version as...] |\n"
              "| 07-structure-destroyed | flow 3e | none (unindexable) | \"This file is badly damaged\" tell (names the layers recovered + that the index/history are gone); full-scan yields the newest content, root gone so no History |\n"
              "| 08-content-cratered | flow 3b (catastrophic) | 3 saves | ~10 checkpoint stripes wiped: a visible transparent CRATER across the poster; \"This file is damaged\", dozens of areas lost |\n"
              "| 09-shredded | flow 3e (catastrophic) | none | the worst case: every save + the structure + a checkpoint stripe destroyed; opens at a DEGRADED original (edits gone, holes), no history -- recovery visibly fails |\n"
              "| 12-compacted | compaction (no damage) | 4 saves, folded | opens clean, no dialog; the append region is GONE (history lives in the checkpoint) yet the History panel still shows 4 saves and jumps between them |\n"
              "| 13-compacted-history-rot | retained-history loss | none readable | the document is byte-perfect and opens with NO dialog; status line: \"this file's save history could not be read\"; the History panel is empty |\n"
              "| 14-compacted-then-appended | compaction + later saves | 2 folded + 2 appended | opens clean; the History panel shows all 4, composed from the checkpoint AND the append region |\n"
              "| 15-manifest-replica | manifest redundancy | 3 saves | the manifest's primary copy is destroyed; the file opens PERFECTLY, no dialog, nothing counted lost -- the replica carried it |\n"
              "| 16-cas-folded | cas (H4) history, clean | 4 saves, folded | Build 2's content-addressed encoding: opens clean, no dialog; History shows 4 saves exactly like 12 |\n"
              "| 17-cas-blob-rot | cas retained-history loss | none readable | a hash-keyed BLOB rotted: document byte-perfect, NO dialog; status line says the save history could not be read (13's verdict, cas spelling) |\n"
              "| 18-cas-repaired-by-parity | flow 3a on cas | 4 saves | one referenced current frame damaged and repaired by parity: no dialog, status line only, History intact |\n\n"
              "Cases 08 and 09 exist because the reader is otherwise good enough that most fixtures\n"
              "open looking perfect -- these lose enough that the damage is unmissable.\n\n"
              "## Compaction (12-14)\n\n"
              "An ordinary File->Save appends a batch. Once the append region's parity debt trips the\n"
              "compaction threshold, THAT Save folds the file instead: the checkpoint is rewritten with\n"
              "the newest frame per key as current content, and every frame it superseded -- plus every\n"
              "`HIST` record -- carried through byte-verbatim behind it as retained history. Reading it\n"
              "back must be indistinguishable. 13 is the one case where it is not: retained history is\n"
              "deliberately uncovered by Reed-Solomon parity, so damage there is permanent. It is\n"
              "reported as lost UNDO STATES, never as a damaged document -- the pixels are exact.\n\n"
              "## Journal fixtures (flows 1/2) -- planted with `--plant`\n\n"
              "A recovery journal keys on the file's ABSOLUTE path + $XDG_STATE_HOME, so it cannot\n"
              "be shipped -- it must be planted next to the exact file it belongs to. Regenerate\n"
              "with `corrupt_corpus <dir> --plant` (run the app afterwards with the same\n"
              "$XDG_STATE_HOME). Two files + their journals under the state dir are written:\n\n"
              "| file | scenario | expected |\n"
              "|---|---|---|\n"
              "| 10-journal-restore | flow 1 | opens clean, then \"Unsaved changes found\" (Restore icon, forced choice): Restore brings back two unsaved edits (tree + title) as a dirty document; Discard drops them |\n"
              "| 11-journal-orphan | flow 2 | the file was saved again after the journal was written, so \"Old unsaved changes no longer match this file\": Keep sets the journal aside, Discard removes it |\n\n"
              "Move a planted file (or regenerate with a different $XDG_STATE_HOME) and flow 1\n"
              "degrades to flow 2 by construction.\n\n"
              "`--plant` also PRUNES the journals and advisory locks that earlier generations of this\n"
              "corpus left in the recovery dir. Every run mints fresh document uuids, so the previous\n"
              "run's `<uuid>-<pathhash>` entries are orphaned the moment the fixtures are rewritten --\n"
              "inert, but one more set each time. Only entries whose path-hash matches a file this tool\n"
              "writes are removed; recovery state for any other document is never touched.\n\n"
              "Flow 6 (already open in another window) has no static fixture -- it needs a LIVE\n"
              "second holder. Open any .mosaic in two Mosaic windows at once: the second offers\n"
              "\"already open in another window\" ([Cancel] [Open read-only]); read-only routes Save to\n"
              "Save As. Kill the first window (a crash) and reopening acquires the stale lock cleanly.\n";
    }

    std::printf("%s\n", g_failures == 0 ? "corpus OK" : "CORPUS BROKEN");
    return g_failures == 0 ? 0 : 1;
}
