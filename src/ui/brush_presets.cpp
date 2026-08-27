#include "ui/brush_presets.hpp"

#include "common/fs_path.hpp"
#include "common/log.hpp"
#include "common/settings.hpp"
#include "core/brush/stroke_preview.hpp"
#include "io/brush/kpp.hpp"
#include "io/brush/preset_brush.hpp"
#include "io/brush/preset_json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <utility>

namespace mosaic::ui {

namespace {

// Read a whole file. Empty (and *error set) on anything that is not a readable regular file --
// which a preset directory can contain: a dangling symlink, a half-copied download, a directory
// someone named `x.mbp`.
[[nodiscard]] std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& path,
                                                      std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr)
            *error = "cannot open " + path.filename().string();
        return {};
    }
    std::vector<std::uint8_t> bytes;
    (void)common::readWholeFile(common::utf8FromPath(path), bytes);
    if (bytes.empty() && error != nullptr)
        *error = path.filename().string() + " is empty";
    return bytes;
}

// The extension, lower-cased, WITH its dot ("" for none). Extensions arrive from a filesystem and
// from a file chooser, and neither promises a case.
[[nodiscard]] std::string lowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

// The two resource-identity tests the adoption below turns on. Written out field by field rather
// than through a defaulted `operator==` on the core structs: these are LOCAL questions ("did the
// library build exactly this?"), and a defaulted comparison on a struct of doubles would quietly
// become part of two core headers' public contracts for the sake of one call site here.
[[nodiscard]] bool sameAdjustments(const core::brush::TipAdjustments& a,
                                   const core::brush::TipAdjustments& b) noexcept {
    return a.autoMidPoint == b.autoMidPoint && a.midPoint == b.midPoint &&
           a.brightness == b.brightness && a.contrast == b.contrast;
}
[[nodiscard]] bool sameBake(const core::brush::TextureBake& a,
                            const core::brush::TextureBake& b) noexcept {
    return a.scale == b.scale && a.brightness == b.brightness && a.contrast == b.contrast &&
           a.neutralPoint == b.neutralPoint && a.invert == b.invert &&
           a.cutoffPolicy == b.cutoffPolicy && a.cutoffLeft == b.cutoffLeft &&
           a.cutoffRight == b.cutoffRight;
}

// One line in `provenance.droppedOptions`, and the fidelity floor that goes with it. The library
// spells this the same way for a bundle reference it could not resolve; a loose file that lost the
// same thing must not look intact either (the docio honesty discipline).
void noteDropped(io::brush::BrushPreset& preset, std::string note) {
    if (preset.provenance.droppedOptions.size() <
        static_cast<std::size_t>(io::brush::kMaxDroppedOptions))
        preset.provenance.droppedOptions.push_back(std::move(note));
    if (preset.provenance.fidelity == io::brush::PresetFidelity::Exact)
        preset.provenance.fidelity = io::brush::PresetFidelity::Approximated;
}

} // namespace

std::string uniquePresetName(const std::vector<std::string>& taken, std::string_view wanted) {
    std::string base(wanted);
    // Trim, because a name that is only spaces is a name nobody can search for and "" is how the
    // settings spell NO PRESET -- `indexOfName` refuses to match it, so a preset saved under it
    // could never be restored at startup.
    while (!base.empty() && std::isspace(static_cast<unsigned char>(base.front())) != 0)
        base.erase(base.begin());
    while (!base.empty() && std::isspace(static_cast<unsigned char>(base.back())) != 0)
        base.pop_back();
    if (base.empty())
        base = "Brush";

    const auto held = [&taken](const std::string& n) {
        return std::find(taken.begin(), taken.end(), n) != taken.end();
    };
    if (!held(base))
        return base;
    std::string candidate = base + " copy";
    for (int n = 2; held(candidate); ++n)
        candidate = base + " copy " + std::to_string(n);
    return candidate;
}

std::filesystem::path withMbpExtension(std::filesystem::path path) {
    std::string ext = path.extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".mbp")
        return path;
    // += , not replace_extension: a preset named "Ink 2.0" reduces to a stem carrying a dot, and
    // replace_extension would eat the "0" rather than add anything.
    path += ".mbp";
    return path;
}

bool writePresetFile(const io::brush::LibraryPreset& lp, const common::Image& icon,
                     const std::filesystem::path& path, std::string* error) {
    const auto fail = [error](std::string why) {
        if (error != nullptr)
            *error = std::move(why);
        return false;
    };

    // ⚠ THE CONTAINER'S RASTER *IS* THE THUMBNAIL (§3.1), so it is not optional -- writeMbp refuses
    // an empty image, and a preset with no picture would show the dock's fallback glyph forever. A
    // caller with nothing to hand in (an export, or an import whose source carried no raster) gets
    // the brush's own STROKE, which is the picture the dock's cards answer "what mark does this
    // make?" with anyway.
    common::Image raster = icon;
    if (raster.empty()) {
        core::brush::StrokePreviewStyle style;
        style.maxDiameter = kUserPresetIconDiameter;
        raster = core::brush::renderStrokePreview(io::brush::presetBrushParams(lp),
                                                  kUserPresetIconPx, kUserPresetIconPx, style);
    }

    std::string why;
    const std::optional<std::vector<std::uint8_t>> bytes =
        io::brush::writeMbp(lp.preset, raster, &why);
    if (!bytes)
        return fail(why.empty() ? "could not serialize the preset" : why);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return fail("cannot write " + path.filename().string());
    out.write(reinterpret_cast<const char*>(bytes->data()),
              static_cast<std::streamsize>(bytes->size()));
    if (!out)
        return fail("cannot write " + path.filename().string());
    return true;
}

std::string presetFileStem(std::string_view name) {
    constexpr std::size_t kMaxStem = 64; // filesystems argue past ~255 bytes; a stem is not a name
    std::string stem;
    stem.reserve(std::min(name.size(), kMaxStem));
    bool lastWasFill = false;
    for (const char ch : name) {
        const auto u = static_cast<unsigned char>(ch);
        const bool keep = (u < 128) && (std::isalnum(u) != 0 || ch == '-');
        if (keep) {
            stem.push_back(ch);
            lastWasFill = false;
        } else if (!lastWasFill) {
            // Runs collapse to ONE '_': "a)_Eraser  Circle" is a stem, not a transcription.
            stem.push_back('_');
            lastWasFill = true;
        }
        if (stem.size() >= kMaxStem)
            break;
    }
    while (!stem.empty() && stem.back() == '_')
        stem.pop_back();
    while (!stem.empty() && stem.front() == '_')
        stem.erase(stem.begin());
    return stem.empty() ? std::string("brush") : stem;
}

int BrushPresetStore::scanDir(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return 0;

    // Sorted, so the order a user sees does not depend on the filesystem's iteration order.
    std::vector<std::filesystem::path> bundles;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec))
            continue;
        if (entry.path().extension() == ".bundle")
            bundles.push_back(entry.path());
    }
    std::sort(bundles.begin(), bundles.end());

    int added = 0;
    for (const std::filesystem::path& b : bundles) {
        std::string error;
        const int n = m_lib.addBundleFile(b, &error);
        if (n == 0 && !error.empty()) {
            // A bundle that will not open is not fatal: the others still load, and the Brush still
            // paints. It IS worth a line in the log -- a silently missing preset set looks like a
            // bug in the picker.
            common::log::category("brush")->warn("preset bundle {}: {}", b.filename().string(),
                                                 error);
            continue;
        }
        added += n;
    }
    return added + scanLooseDir(dir);
}

int BrushPresetStore::scanLooseDir(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return 0;

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec))
            continue;
        const std::string ext = lowerExtension(entry.path());
        if (ext == ".mbp" || ext == ".kpp")
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end()); // the same rule the bundles follow, for the same reason

    int added = 0;
    for (const std::filesystem::path& f : files) {
        std::string error;
        const std::vector<std::uint8_t> bytes = readFileBytes(f, &error);
        std::optional<io::brush::BrushPreset> preset;
        if (!bytes.empty()) {
            preset = lowerExtension(f) == ".mbp"
                         ? io::brush::readMbp(bytes.data(), bytes.size(), &error)
                         : io::brush::readKpp(bytes.data(), bytes.size(), &error);
        }
        if (!preset) {
            // A preset that will not open is not fatal -- exactly as a bundle that will not open is
            // not. It IS worth a line: a brush the user saved and cannot find looks like data loss.
            common::log::category("brush")->warn("preset file {}: {}", f.filename().string(),
                                                 error.empty() ? "unreadable" : error);
            continue;
        }
        m_loose.push_back(adoptLoosePreset(std::move(*preset), f));
        ++added;
    }
    if (added > 0)
        rebuildAll();
    return added;
}

int BrushPresetStore::scan() {
    int n = scanDir(common::installedDataDir() / "brushes");
    // ⚠ The boundary is RECORDED HERE, where it is drawn, rather than re-derived later by comparing
    // paths against installedDataDir(): everything loaded from here on is the user's own, and that
    // is what the dock's "User" tab is (isUserPreset). Set it BEFORE the second scan -- and set it
    // for BOTH runs, because a loose file can sit in either directory (isUserPreset).
    m_libUserSplit = static_cast<int>(m_lib.presets().size());
    m_looseUserSplit = static_cast<int>(m_loose.size());
    n += scanDir(common::dataDir() / "brushes");
    rebuildAll();
    return n;
}

int BrushPresetStore::shippedLibCount() const noexcept {
    const int n = static_cast<int>(m_lib.presets().size());
    // ⚠ -1 is NO BOUNDARY, and that means the whole run is SHIPPED -- never that the whole run is
    // the user's. This is isUserPreset's old guard, moved to the one place the split is read.
    return m_libUserSplit < 0 ? n : std::min(m_libUserSplit, n);
}

int BrushPresetStore::shippedLooseCount() const noexcept {
    const int n = static_cast<int>(m_loose.size());
    return m_looseUserSplit < 0 ? n : std::min(m_looseUserSplit, n);
}

int BrushPresetStore::mergedIndexOfLib(int libSlot) const noexcept {
    const int shipped = shippedLibCount();
    if (libSlot < 0 || libSlot >= static_cast<int>(m_lib.presets().size()))
        return -1;
    // A user bundle's presets lead the whole corpus; a shipped one sits after every user preset.
    return libSlot >= shipped ? libSlot - shipped : m_userCount + libSlot;
}

int BrushPresetStore::mergedIndexOfLoose(int looseSlot) const noexcept {
    const int shipped = shippedLooseCount();
    if (looseSlot < 0 || looseSlot >= static_cast<int>(m_loose.size()))
        return -1;
    const int userLib = static_cast<int>(m_lib.presets().size()) - shippedLibCount();
    return looseSlot >= shipped ? userLib + (looseSlot - shipped)
                                : m_userCount + shippedLibCount() + looseSlot;
}

int BrushPresetStore::looseSlotOf(int index) const noexcept {
    const int shippedLib = shippedLibCount();
    const int shippedLoose = shippedLooseCount();
    const int userLib = static_cast<int>(m_lib.presets().size()) - shippedLib;
    if (index >= userLib && index < m_userCount)
        return shippedLoose + (index - userLib); // the user's own loose run
    const int shippedLooseStart = m_userCount + shippedLib;
    if (index >= shippedLooseStart &&
        index < shippedLooseStart + shippedLoose)
        return index - shippedLooseStart;        // a loose file that shipped
    return -1;                                   // a library (bundle) preset, or out of range
}

void BrushPresetStore::rebuildAll() {
    const int libCount = static_cast<int>(m_lib.presets().size());
    const int looseCount = static_cast<int>(m_loose.size());
    const int shippedLib = shippedLibCount();
    const int shippedLoose = shippedLooseCount();
    m_userCount = (libCount - shippedLib) + (looseCount - shippedLoose);

    m_all.clear();
    if (m_userCount == 0 && looseCount == 0) {
        // Nothing to merge and nothing to reorder -- every fresh install. presets() then hands out
        // the library's own vector: no copy, no cost.
    } else {
        // ⚠ USER FIRST (the user's call, feedback round 1). Within each half the library's bundles
        // lead the loose files, and each keeps its own scan order, so the only thing that moved is
        // where the user's run sits -- not how either run is sorted internally.
        m_all.reserve(m_lib.presets().size() + m_loose.size());
        m_all.insert(m_all.end(), m_lib.presets().begin() + shippedLib, m_lib.presets().end());
        m_all.insert(m_all.end(), m_loose.begin() + shippedLoose, m_loose.end());
        m_all.insert(m_all.end(), m_lib.presets().begin(), m_lib.presets().begin() + shippedLib);
        m_all.insert(m_all.end(), m_loose.begin(), m_loose.begin() + shippedLoose);
    }

    // Re-point WITHOUT re-resolving: the params are already built and re-running presetBrushParams
    // would mint a fresh raster id for a tip nothing changed about -- a cold dab cache for nothing.
    // A name that has left the corpus lands on NO PRESET -- the plain round nib, the one selection
    // that is always valid.
    //
    // ⚠ AND THE PARAMS GO WITH IT. Until a preset could be DELETED a name never left the corpus, so
    // the index alone was enough; now it can, and a slot reading -1 while activeParams() still
    // handed out the deleted brush's tip would keep painting with a brush that no longer exists.
    for (Selection* slot : {&m_brush, &m_eraser}) {
        if (slot->name.empty())
            continue;
        const int found = indexOfName(slot->name);
        if (found < 0) {
            slot->index = -1;
            slot->name.clear();
            slot->params.reset();
        } else {
            slot->index = found;
        }
    }
}

std::vector<std::string> BrushPresetStore::names() const {
    std::vector<std::string> out;
    out.reserve(presets().size());
    for (const io::brush::LibraryPreset& p : presets())
        out.push_back(p.preset.name);
    return out;
}

int BrushPresetStore::indexOfName(std::string_view name) const {
    if (name.empty())
        return -1; // "" is how the settings spell NO preset; never let it match a nameless import
    const std::vector<io::brush::LibraryPreset>& all = presets();
    for (std::size_t i = 0; i < all.size(); ++i)
        if (all[i].preset.name == name)
            return static_cast<int>(i);
    return -1;
}

const io::brush::LibraryPreset* BrushPresetStore::activePreset() const noexcept {
    return presetAt(m_brush.index);
}

const io::brush::LibraryPreset* BrushPresetStore::activeEraserPreset() const noexcept {
    return presetAt(m_eraser.index);
}

const io::brush::LibraryPreset* BrushPresetStore::presetAt(int index) const noexcept {
    if (index < 0 || static_cast<std::size_t>(index) >= presets().size())
        return nullptr;
    return &presets()[static_cast<std::size_t>(index)];
}

bool BrushPresetStore::selectInto(Selection& slot, int index) {
    if (index < 0) {
        slot.index = -1;
        slot.name.clear();
        slot.params.reset(); // back to the engine's own round tip
        return true;
    }
    if (static_cast<std::size_t>(index) >= presets().size())
        return false;

    slot.index = index;
    slot.name = presets()[static_cast<std::size_t>(index)].preset.name;
    // ⚠ ONCE, HERE -- not per stroke. This mints the tip's raster id, and a fresh id every stroke
    // would be a permanently cold dab cache.
    core::brush::BrushParams params =
        io::brush::presetBrushParams(presets()[static_cast<std::size_t>(index)]);
    // ⚠⚠ AND BECAUSE IT IS ONCE, THE RANDOMNESS HAS TO COME FROM SOMEWHERE ELSE (docs/brushes.md
    // §6.6i, a USER-REPORTED bug). These params are shared by every stroke the user lays with this
    // preset, `seed` included -- so without this flag every stroke drew the identical random
    // sequence, and a single-dab TAP is exactly one draw: `t)_Shapes_Mecha` stamped cell after cell
    // of the same one of its 23 shapes, and every `fuzzy` rotation in the set repeated too. The flag
    // makes the engine fold the stroke's own first sample into the seed, which varies per stroke
    // without costing the replay contract a thing (brush_engine.hpp's strokeSeedFor). The dock's
    // preview cards clear it again, on purpose (core/brush/stroke_preview.cpp).
    params.seedFromFirstSample = true;
    slot.params = std::make_shared<const core::brush::BrushParams>(std::move(params));
    return true;
}

bool BrushPresetStore::select(int index) {
    return selectInto(m_brush, index);
}

bool BrushPresetStore::selectEraser(int index) {
    return selectInto(m_eraser, index);
}

// ---- Loose presets: adoption, not re-derivation ------------------------------------------------

io::brush::LibraryPreset
BrushPresetStore::adoptLoosePreset(io::brush::BrushPreset preset,
                                   const std::filesystem::path& path) const {
    io::brush::LibraryPreset lp;
    lp.preset = std::move(preset);
    lp.sourcePath = path.string();
    lp.entryName.clear(); // a LOOSE file: there is no archive entry, and that is how the panel
                          // knows to decode the file itself rather than open it as a zip

    if (lp.preset.tip.kind == io::brush::TipXml::Kind::Predefined) {
        // ⚠ ADOPT, DO NOT REBUILD. The tip's pixels live in a bundle this file is not part of. The
        // library has already decoded that file, run §3.5's content test on it and BUILT a
        // BitmapTip for the resulting application + adjustments; the only build that can be right
        // here is one of those. Match on the resolved tip FILE NAME, and require the application
        // and the three adjustments to agree -- a donor built for a different application is a
        // different tip wearing the same file name.
        const std::string& want = lp.preset.tip.predefined.filename;
        const io::brush::PredefinedTipXml& ours = lp.preset.tip.predefined;
        const io::brush::LibraryPreset* donor = nullptr;
        for (const io::brush::LibraryPreset& cand : m_lib.presets()) {
            if (cand.tip == nullptr || cand.tipFileName != want)
                continue;
            const io::brush::PredefinedTipXml& theirs = cand.preset.tip.predefined;
            // ⚠ THE TWO REFERENCES MUST AGREE ON EVERY INPUT THE BUILD CONSUMED, not merely on the
            // file name. §3.5's application is decided by a rule + the tip IMAGE's content, and the
            // three adjustments are baked into the raster. Two presets that agree on the rule, its
            // explicit application, `colorAsMask` and the adjustments resolve identically over the
            // same file's pixels BY CONSTRUCTION -- which is the only claim that lets a build be
            // adopted without re-running the content test we cannot run.
            if (theirs.applicationRule != ours.applicationRule ||
                theirs.application != ours.application || theirs.colorAsMask != ours.colorAsMask)
                continue;
            if (!sameAdjustments(theirs.adjustments, ours.adjustments))
                continue;
            donor = &cand;
            break;
        }
        if (donor != nullptr) {
            lp.tip = donor->tip;
            lp.tipFileName = donor->tipFileName;
            lp.tipResolution = io::brush::TipResolution::ByFilename;
            // The absolute size is OUR scale over the DONOR's raster -- the library's own rule
            // (scale x the first frame's base size), read off the build we adopted rather than
            // re-derived from a file we cannot see.
            lp.masterDiameter = ours.scale * donor->tip->baseSize(0);
            // ... and the accumulator verdict is re-run with the RESOLVED application, exactly as
            // the library re-runs it post tip-load. A `.mbp` round-trips the finalized verdict and
            // would not need this; a loose `.kpp` carries the MAPPER's staged one, which assumes
            // AlphaMask (io/brush/preset.hpp), and a colour-stamping tip would import on the wrong
            // accumulator without it.
            lp.preset.accumulator = core::brush::chooseAccumulator(
                donor->tip->application(), lp.preset.colorDynamicsActive,
                core::brush::painterVariesColor(lp.preset.painter));
        } else {
            // Exactly the library's own degradation for an unresolvable bundle reference: the
            // preset stays USABLE on the round tip, and says so.
            lp.tipResolution = io::brush::TipResolution::Fallback;
            noteDropped(lp.preset,
                        "Tip file '" + want + "' (not installed; default round tip)");
            lp.preset.tip.kind = io::brush::TipXml::Kind::Auto;
            lp.preset.tip.autoTip = io::brush::AutoTipXml{};
            lp.preset.tip.autoTip.generator.diameter = 24.0; // the library's own fallBackToRoundTip
            lp.masterDiameter = lp.preset.tip.autoTip.generator.diameter;
        }
    } else {
        lp.tipResolution = io::brush::TipResolution::AutoTip;
        lp.masterDiameter = lp.preset.tip.autoTip.generator.diameter;
    }

    // Masking resolves last, exactly as it does in the library: UseMasterSize needs the primary's
    // absolute diameter. This one IS a pure function (io/brush/preset.hpp), so it is called, not
    // adopted.
    lp.masking = io::brush::resolveMasking(lp.preset.masking, lp.masterDiameter);

    // The TEXTURE pattern is a base64 payload that the library DECODES AND BAKES. Adopt a bake made
    // from the identical payload under the identical bake parameters; anything else loses the
    // grain, badged. (Re-baking here would be a second copy of §6.6h's rule, and the two copies
    // would drift the first time the bake changed.)
    if (lp.preset.texture.enabled) {
        const io::brush::LibraryPreset* donor = nullptr;
        for (const io::brush::LibraryPreset& cand : m_lib.presets()) {
            if (!cand.texture.enabled || cand.preset.texture.patternBase64.empty())
                continue;
            if (cand.preset.texture.patternBase64 != lp.preset.texture.patternBase64)
                continue;
            if (!sameBake(cand.preset.texture.bake, lp.preset.texture.bake))
                continue;
            donor = &cand;
            break;
        }
        if (donor != nullptr) {
            lp.texture = donor->texture;
            // The placement knobs are the PRESET's, not the pattern's -- only the baked mask is
            // shared, and two presets over one paper may still offset it differently.
            lp.texture.mode = lp.preset.texture.mode;
            lp.texture.offsetX = lp.preset.texture.offsetX;
            lp.texture.offsetY = lp.preset.texture.offsetY;
            lp.texture.randomOffsetX = lp.preset.texture.randomOffsetX;
            lp.texture.randomOffsetY = lp.preset.texture.randomOffsetY;
            lp.texture.softTexturing = lp.preset.texture.softTexturing;
        } else {
            noteDropped(lp.preset, "Texture pattern '" + lp.preset.texture.patternName +
                                       "' (not installed; texturing off)");
        }
    }
    return lp;
}

std::string BrushPresetStore::uniqueName(std::string_view wanted) const {
    return uniquePresetName(names(), wanted);
}

int BrushPresetStore::writeUserPreset(const io::brush::LibraryPreset& lp, const common::Image& icon,
                                      int replaceIndex, std::string* error) {
    const auto fail = [error](std::string why) {
        if (error != nullptr)
            *error = std::move(why);
        return -1;
    };

    int looseSlot = -1;
    if (replaceIndex >= 0) {
        // ⚠ A shipped bundle is READ-ONLY, and so is any preset inside one -- there is no writer for
        // a `.bundle` and there must not be one that edits the set Mosaic ships. Refuse loudly
        // rather than quietly minting a second brush of the same name. The same refusal covers a
        // preset inside a USER-installed bundle: it has no file of its own to overwrite.
        looseSlot = looseSlotOf(replaceIndex);
        if (looseSlot < 0 || !isUserPreset(replaceIndex))
            return fail("that preset is read-only");
    }

    std::error_code ec;
    const std::filesystem::path dir = common::dataDir() / "brushes";
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return fail("cannot create " + dir.string() + ": " + ec.message());

    // Overwriting keeps the file it already has; a NEW preset gets a fresh stem, disambiguated by a
    // counter because presetFileStem is deliberately not injective (accents and punctuation all
    // collapse to '_').
    std::filesystem::path file;
    if (looseSlot >= 0) {
        file = m_loose[static_cast<std::size_t>(looseSlot)].sourcePath;
    } else {
        const std::string stem = presetFileStem(lp.preset.name);
        file = dir / (stem + ".mbp");
        for (int n = 2; std::filesystem::exists(file, ec); ++n)
            file = dir / (stem + "-" + std::to_string(n) + ".mbp");
    }

    // ONE writer, shared with Export (writePresetFile): two serializers would drift, and an export
    // that did not produce byte-identical output to what the app reads back is a bug waiting.
    std::string why;
    if (!writePresetFile(lp, icon, file, &why))
        return fail(std::move(why));

    // ⚠ THE STORE KEEPS THE RESOLVED COPY, not a re-read of what was just written. The editor
    // already holds a built tip and a baked texture for this brush; adopting them straight back is
    // both faster and MORE faithful than re-reading a container that carries references to
    // resources it does not ship (adoptLoosePreset). The next launch does the adoption.
    io::brush::LibraryPreset stored = lp;
    stored.sourcePath = file.string();
    stored.entryName.clear();
    if (looseSlot >= 0) {
        m_loose[static_cast<std::size_t>(looseSlot)] = std::move(stored);
    } else {
        // A preset saved NOW is the user's own even if no user scan ever drew a boundary -- which is
        // the case for a bare scanDir() (the tests, and a build with no data dir at all).
        if (m_looseUserSplit < 0)
            m_looseUserSplit = static_cast<int>(m_loose.size());
        m_loose.push_back(std::move(stored));
        looseSlot = static_cast<int>(m_loose.size()) - 1;
    }
    rebuildAll();
    // ⚠ NOT `libCount + looseSlot` any more. The corpus is USER-FIRST (presets()), so the index a
    // caller has to be handed is where the reorder PUT it -- and the one place that arithmetic is
    // spelled is mergedIndexOfLoose. rebuildAll() runs first: the mapping reads m_userCount.
    return mergedIndexOfLoose(looseSlot);
}

int BrushPresetStore::importPresetFile(const std::filesystem::path& path, std::string* error) {
    const auto fail = [error](std::string why) {
        if (error != nullptr)
            *error = std::move(why);
        return -1;
    };

    const std::string ext = lowerExtension(path);
    if (ext != ".mbp" && ext != ".kpp" && ext != ".bundle")
        return fail("unsupported preset file type");

    std::string why;
    const std::vector<std::uint8_t> bytes = readFileBytes(path, &why);
    if (bytes.empty())
        return fail(why.empty() ? "the file is empty" : why);

    std::error_code ec;
    const std::filesystem::path dir = common::dataDir() / "brushes";
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return fail("cannot create " + dir.string() + ": " + ec.message());

    // Copy INTO the user's brush directory first, so an import survives the session -- a preset that
    // vanished on restart would be worse than an import that refused.
    std::filesystem::path dest = dir / path.filename();
    for (int n = 2; std::filesystem::exists(dest, ec); ++n)
        dest = dir / (path.stem().string() + "-" + std::to_string(n) + ext);
    {
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out)
            return fail("cannot write " + dest.filename().string());
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out)
            return fail("cannot write " + dest.filename().string());
    }

    if (ext == ".bundle") {
        const int before = static_cast<int>(m_lib.presets().size());
        const int added = m_lib.addBundleFile(dest, &why);
        if (added <= 0) {
            std::filesystem::remove(dest, ec); // do not leave a bundle we could not read behind
            return fail(why.empty() ? "the bundle carries no presets" : why);
        }
        // A bundle imported at RUNTIME is the user's own, and the library's boundary was drawn at
        // startup -- so if no user bundle had been scanned then, it is drawn here instead.
        if (m_libUserSplit < 0)
            m_libUserSplit = before;
        rebuildAll(); // the loose run shifts by `added`; the selections re-point by name
        return mergedIndexOfLib(before); // ... and the corpus is USER-FIRST now
    }

    std::optional<io::brush::BrushPreset> preset =
        ext == ".mbp" ? io::brush::readMbp(bytes.data(), bytes.size(), &why)
                      : io::brush::readKpp(bytes.data(), bytes.size(), &why);
    if (!preset) {
        std::filesystem::remove(dest, ec);
        return fail(why.empty() ? "the file is not a readable preset" : why);
    }
    // Two brushes answering to one name is what makes the persisted selection a coin toss (§8.2),
    // so an import that collides is renamed on the way in -- and the file it was written from is
    // rewritten under the new name, or the next scan would import the collision all over again.
    const std::string unique = uniqueName(preset->name);
    if (unique != preset->name) {
        preset->name = unique;
        const std::optional<common::Image> icon =
            ext == ".mbp" ? io::brush::readMbpIcon(bytes.data(), bytes.size())
                          : io::brush::readKppIcon(bytes.data(), bytes.size());
        std::filesystem::remove(dest, ec);
        const int index =
            writeUserPreset(adoptLoosePreset(*preset, dest), icon.value_or(common::Image{}), -1,
                            error);
        return index;
    }

    if (m_looseUserSplit < 0)
        m_looseUserSplit = static_cast<int>(m_loose.size());
    m_loose.push_back(adoptLoosePreset(std::move(*preset), dest));
    rebuildAll();
    return mergedIndexOfLoose(static_cast<int>(m_loose.size()) - 1);
}

// ---- Delete (docs/brushes.md §8.3, feedback round 1) -------------------------------------------

bool BrushPresetStore::canDeletePreset(int index) const noexcept {
    // Two conditions, and they are not the same one. `isUserPreset` says the preset came from the
    // user's data directory; `looseSlotOf` says it has a FILE OF ITS OWN. A preset inside a
    // user-installed bundle satisfies the first and fails the second, and deleting it would mean
    // deleting the archive and everything else in it.
    if (!isUserPreset(index))
        return false;
    const int slot = looseSlotOf(index);
    if (slot < 0)
        return false;
    return !m_loose[static_cast<std::size_t>(slot)].sourcePath.empty();
}

std::string BrushPresetStore::deleteRefusal(int index) const {
    if (canDeletePreset(index))
        return {};
    if (presetAt(index) == nullptr)
        return "there is no preset there";
    if (!isUserPreset(index))
        return "that preset ships with Mosaic and cannot be deleted";
    return "that preset lives inside a brush bundle -- remove the bundle file to remove it";
}

bool BrushPresetStore::deleteUserPreset(int index, std::string* error, int* nextIndex) {
    const auto fail = [error](std::string why) {
        if (error != nullptr)
            *error = std::move(why);
        return false;
    };
    if (!canDeletePreset(index))
        return fail(deleteRefusal(index));

    const int slot = looseSlotOf(index);
    const std::filesystem::path file = m_loose[static_cast<std::size_t>(slot)].sourcePath;

    // ⚠ THE FILE GOES FIRST, and a failure here is the whole operation failing. Dropping the entry
    // and leaving the file behind would resurrect the preset on the next scan -- a delete that
    // un-deletes itself at restart is worse than a delete that refused out loud.
    std::error_code ec;
    if (std::filesystem::exists(file, ec) && !std::filesystem::remove(file, ec))
        return fail("cannot remove " + file.filename().string() +
                    (ec ? ": " + ec.message() : std::string()));

    m_loose.erase(m_loose.begin() + slot);
    // The shipped|user boundary of the loose run is a COUNT of shipped files, and a user file sits
    // past it -- so removing one never moves it. (It is still clamped by shippedLooseCount(), which
    // is what keeps a stale split from outrunning a shrunken vector.)
    rebuildAll();

    if (nextIndex != nullptr) {
        // The preset that took the deleted one's place, or the last one when it WAS the last, or
        // -1 (Default round) when nothing is left. A UI pointing at the deleted index would other-
        // wise be pointing one past the end -- or, worse, at whatever slid into that slot silently.
        const int total = static_cast<int>(presets().size());
        *nextIndex = total == 0 ? -1 : std::min(index, total - 1);
    }
    return true;
}

} // namespace mosaic::ui
