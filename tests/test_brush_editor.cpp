// The modal brush editor's model (docs/brushes.md §8.3), and the user-preset store under it.
//
// ⚠ WHAT THIS FILE CAN AND CANNOT SEE. `BrushEditorDialog` is an `Fl_Window`, and an unshown
// Fl_Window renders BLACK to an `Fl_Image_Surface` -- so the dialog's LOOK is owed to the user's
// visual pass and cannot be screenshot here. What CAN be pinned is everything the dialog composes
// itself out of: the rail's grouping (a partition, exactly like §8.2's preset tabs), the labels (the
// file's base names are WIRE names, not captions), the unique-name and file-stem rules a save
// depends on, and the loose-preset round trip -- write an `.mbp`, scan it back, and check the store
// adopted the library's resolved resources rather than re-deriving them.
//
// ⚠ AND ONE THING MORE, SINCE FEEDBACK ROUND 1: the rail's preset list is an ordinary `Fl_Widget`,
// not a window -- so its draw() CAN be driven into an `Fl_Image_Surface`, which is what lets the
// "each strip is laid once, and a redraw lays none" contract be asserted at all.
#include "core/blend_mode.hpp"          // the blind pair: Lighten is the identity for black on white
#include "core/brush/dab.hpp" // kDrivenOptions: the ONE list of what a dab actually reads
#include "core/brush/sensors.hpp"
#include "core/brush/stroke_preview.hpp" // the scratchpad's ink is what the RENDER came back in
#include "io/brush/preset.hpp"
#include "io/brush/preset_json.hpp"
#include "ui/brush_editor.hpp"
#include "ui/brush_presets.hpp"

#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_Widget.H>
#include <FL/fl_draw.H>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib> // setenv/unsetenv: the dataDir() cases drive the store through the environment
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace cb = mosaic::core::brush;
namespace ib = mosaic::io::brush;

using mosaic::ui::BrushEditorRow;
using mosaic::ui::BrushOptionGroup;
using mosaic::ui::brushEditorRows;
using mosaic::ui::brushOptionGroupLabel;
using mosaic::ui::brushOptionGroupOf;
using mosaic::ui::brushOptionLabel;
using mosaic::ui::BrushPresetStore;
using mosaic::ui::brushSensorLabel;
using mosaic::ui::kBrushTipPage;
using mosaic::ui::presetFileStem;
using mosaic::ui::uniquePresetName;

namespace {

// One option, as the reader would have produced it.
cb::CurveOptionData opt(const cb::BrushOptionSpec& spec, bool checked) {
    cb::CurveOptionData d;
    d.name = std::string(spec.base);
    d.checkable = spec.checkable;
    d.checked = checked;
    d.strengthMin = spec.strengthMin;
    d.strengthMax = spec.strengthMax;
    d.strength = 1.0;
    d.sensors.sensors.push_back(cb::Sensor::withDefaults(cb::SensorId::Pressure));
    return d;
}

// The rows that are not group captions, in order.
std::vector<std::string> bodyBases(const std::vector<BrushEditorRow>& rows) {
    std::vector<std::string> out;
    for (const BrushEditorRow& r : rows)
        if (!r.header)
            out.push_back(r.base);
    return out;
}

} // namespace

// ---- The rail's grouping is a PARTITION ---------------------------------------------------------

TEST_CASE("editor groups: every driven option lands in exactly one group") {
    // The §8.2 tab rule, one level down: an option in two groups appears twice and answers to
    // neither; an option in none is an option the user cannot reach. There are only four groups and
    // brushOptionGroupOf is total, so "exactly one" is the interesting half -- and it is the half a
    // future option can break by being added to two of the `if` chains.
    for (const cb::BrushOptionSpec& spec : cb::kDrivenOptions) {
        int hits = 0;
        const BrushOptionGroup got = brushOptionGroupOf(spec.base);
        for (const BrushOptionGroup g : {BrushOptionGroup::General, BrushOptionGroup::Colour,
                                         BrushOptionGroup::Texture, BrushOptionGroup::Tip})
            if (g == got)
                ++hits;
        CHECK_MESSAGE(hits == 1, std::string(spec.base));
    }

    // The assignments that actually carry meaning, spelled out so a silent re-file is a failure.
    CHECK(brushOptionGroupOf("Size") == BrushOptionGroup::General);
    CHECK(brushOptionGroupOf("Opacity") == BrushOptionGroup::General);
    CHECK(brushOptionGroupOf("Flow") == BrushOptionGroup::General);
    CHECK(brushOptionGroupOf("Spacing") == BrushOptionGroup::General);
    CHECK(brushOptionGroupOf("h") == BrushOptionGroup::Colour);
    CHECK(brushOptionGroupOf("s") == BrushOptionGroup::Colour);
    CHECK(brushOptionGroupOf("v") == BrushOptionGroup::Colour);
    CHECK(brushOptionGroupOf("SmudgeRate") == BrushOptionGroup::Colour);
    // ⚠ The base name really does end in a slash (§3.2); the group lookup must match it verbatim.
    CHECK(brushOptionGroupOf("Texture/Strength/") == BrushOptionGroup::Texture);
    CHECK(brushOptionGroupOf("Rotation") == BrushOptionGroup::Tip);
    CHECK(brushOptionGroupOf("Scatter") == BrushOptionGroup::Tip);
    // A base from a newer preset files under Tip rather than vanishing: a row you can see and switch
    // off beats an option silently ignored.
    CHECK(brushOptionGroupOf("SomethingFromTheFuture") == BrushOptionGroup::Tip);
}

TEST_CASE("editor labels: every base and every sensor has one, and none is the wire name") {
    for (const cb::BrushOptionSpec& spec : cb::kDrivenOptions)
        CHECK_FALSE(brushOptionLabel(spec.base).empty());
    for (std::size_t i = 0; i < cb::kSensorCount; ++i)
        CHECK_FALSE(brushSensorLabel(static_cast<cb::SensorId>(i)).empty());

    // The three that a wire name would render as nonsense.
    CHECK(brushOptionLabel("h") != "h");
    CHECK(brushOptionLabel("Texture/Strength/") != "Texture/Strength/");
    CHECK(brushOptionLabel("SmudgeRate") != "SmudgeRate");
    // ⚠ `ascension`/`declination` are the SERIALIZED ids of what a person calls tilt direction and
    // tilt elevation (sensors.hpp). The UI must not print either wire word.
    const auto wire = [](cb::SensorId id) { return std::string(cb::sensorName(id)); };
    CHECK(brushSensorLabel(cb::SensorId::Ascension) != wire(cb::SensorId::Ascension));
    CHECK(brushSensorLabel(cb::SensorId::Declination) != wire(cb::SensorId::Declination));

    for (const BrushOptionGroup g : {BrushOptionGroup::General, BrushOptionGroup::Colour,
                                     BrushOptionGroup::Texture, BrushOptionGroup::Tip})
        CHECK_FALSE(brushOptionGroupLabel(g).empty());
}

// ---- The rail's rows ----------------------------------------------------------------------------

TEST_CASE("editor rows: the preset's OWN options, grouped, Tip first inside Tip") {
    ib::BrushPreset preset;
    preset.options.push_back(opt(cb::kSizeOptionSpec, true));
    preset.options.push_back(opt(cb::kOpacityOptionSpec, false));
    preset.options.push_back(opt(cb::kHueOptionSpec, true));
    preset.options.push_back(opt(cb::kRotationOptionSpec, false));

    const std::vector<BrushEditorRow> rows = brushEditorRows(preset);
    const std::vector<std::string> bases = bodyBases(rows);

    // Order is group order (General, Colour, Texture, Tip), and inside Tip the pseudo-row leads.
    CHECK(bases == std::vector<std::string>{"Size", "Opacity", "h", std::string(kBrushTipPage),
                                            "Rotation"});

    // A caption over every non-empty group and NONE over an empty one: this preset carries no
    // texture option, so there must be no Texture caption advertising an empty room.
    int headers = 0;
    for (const BrushEditorRow& r : rows) {
        if (!r.header)
            continue;
        ++headers;
        CHECK(r.group != BrushOptionGroup::Texture);
    }
    CHECK(headers == 3); // General, Colour, Tip

    // ⚠ Opacity and Flow are ALWAYS ON: their `Pressure{X}` bit is written to shipped files and the
    // reader forces both on regardless (§3.2), so the row must be UNcheckable and read CHECKED even
    // though the option above was built with checked=false.
    const auto find = [&rows](std::string_view base) -> const BrushEditorRow* {
        for (const BrushEditorRow& r : rows)
            if (!r.header && r.base == base)
                return &r;
        return nullptr;
    };
    REQUIRE(find("Opacity") != nullptr);
    CHECK_FALSE(find("Opacity")->checkable);
    CHECK(find("Opacity")->checked);
    REQUIRE(find("Size") != nullptr);
    CHECK(find("Size")->checkable);
    CHECK(find("Size")->checked);
    REQUIRE(find("Rotation") != nullptr);
    CHECK(find("Rotation")->checkable);
    CHECK_FALSE(find("Rotation")->checked); // an unchecked option still gets a row -- just unticked

    // The Tip pseudo-row is not an option and must never be offered a checkbox.
    REQUIRE(find(kBrushTipPage) != nullptr);
    CHECK_FALSE(find(kBrushTipPage)->checkable);
}

TEST_CASE("editor rows: an option the preset never mentions gets no row") {
    // ⚠ An ABSENT option is not a disabled one (§6.2) -- it contributes the identity, and offering a
    // control for it would invent an option the file does not carry (and a save would then write
    // one into a preset that never had it).
    ib::BrushPreset preset;
    preset.options.push_back(opt(cb::kSizeOptionSpec, true));
    const std::vector<std::string> bases = bodyBases(brushEditorRows(preset));
    CHECK(std::find(bases.begin(), bases.end(), "Scatter") == bases.end());
    CHECK(std::find(bases.begin(), bases.end(), "Flow") == bases.end());
    // ... but the Tip page is always there: the tip is not an option and never was.
    CHECK(std::find(bases.begin(), bases.end(), std::string(kBrushTipPage)) != bases.end());
}

TEST_CASE("editor rows: a preset with no options at all still shows the tip page") {
    const std::vector<std::string> bases = bodyBases(brushEditorRows(ib::BrushPreset{}));
    CHECK(bases == std::vector<std::string>{std::string(kBrushTipPage)});
}

// ---- The save rules -----------------------------------------------------------------------------

TEST_CASE("uniquePresetName: a saved brush never shadows one that already exists") {
    // Two presets answering to one name make the persisted selection a coin toss: the settings store
    // it BY NAME (§8.2) and indexOfName takes the first match, which is whichever the scan reached
    // first -- the user's edit or the file they edited it from.
    const std::vector<std::string> taken{"Ink", "Ink copy", "Chalk"};
    CHECK(uniquePresetName(taken, "Pencil") == "Pencil");
    CHECK(uniquePresetName(taken, "Chalk") == "Chalk copy");
    CHECK(uniquePresetName(taken, "Ink") == "Ink copy 2"); // "Ink copy" is taken too
    // ⚠ "" is how the settings spell NO PRESET, and indexOfName refuses to match it -- so a preset
    // saved under an empty (or whitespace) name could never be restored at startup.
    CHECK(uniquePresetName(taken, "") == "Brush");
    CHECK(uniquePresetName(taken, "   ") == "Brush");
    CHECK(uniquePresetName(taken, "  Pencil  ") == "Pencil"); // trimmed, not renamed
    CHECK(uniquePresetName({}, "Ink") == "Ink");
}

TEST_CASE("presetFileStem: a name becomes a file name, and never an empty one") {
    CHECK(presetFileStem("Pencil") == "Pencil");
    CHECK(presetFileStem("a)_Eraser_Circle") == "a_Eraser_Circle");
    // Runs of unusable bytes collapse to ONE fill character, and the edges are trimmed.
    CHECK(presetFileStem("  Wet   Knife  ") == "Wet_Knife");
    CHECK(presetFileStem("///") == "brush");
    CHECK(presetFileStem("") == "brush");
    // '-' survives; everything non-ASCII-alphanumeric does not.
    CHECK(presetFileStem("Ink-7") == "Ink-7");
    CHECK(presetFileStem("Größe") == "Gr_e");
    // Capped: a filesystem argues past a few hundred bytes, and a stem is not an identity.
    CHECK(presetFileStem(std::string(400, 'a')).size() <= 64);
}

// ---- The loose-preset round trip ----------------------------------------------------------------

namespace {

// An env var, saved and put back. doctest runs cases sequentially in one thread, so a scoped
// override cannot leak into a sibling case -- but it WOULD leak into the rest of the binary without
// this, and half the suite reads dataDir().
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : m_name(name) {
        if (const char* old = std::getenv(name); old != nullptr) {
            m_had = true;
            m_old = old;
        }
        ::setenv(name, value.c_str(), 1);
    }
    ~ScopedEnv() {
        if (m_had)
            ::setenv(m_name, m_old.c_str(), 1);
        else
            ::unsetenv(m_name);
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* m_name;
    bool m_had = false;
    std::string m_old;
};

// A minimal, fully-resolvable preset: a PROCEDURAL tip, so nothing has to be adopted from a bundle.
ib::LibraryPreset autoPreset(std::string name) {
    ib::LibraryPreset lp;
    lp.preset.name = std::move(name);
    lp.preset.provenance.sourceFormat = "kpp";
    lp.preset.provenance.sourcePaintop = "paintbrush";
    lp.preset.tip.kind = ib::TipXml::Kind::Auto;
    lp.preset.tip.type = "auto_brush";
    lp.preset.tip.spacing = 0.1;
    lp.preset.tip.autoTip.generator.diameter = 30.0;
    lp.preset.options.push_back(opt(cb::kSizeOptionSpec, true));
    lp.preset.options.push_back(opt(cb::kOpacityOptionSpec, false));
    lp.masterDiameter = 30.0;
    lp.tipResolution = ib::TipResolution::AutoTip;
    return lp;
}

fs::path freshDir(const char* leaf) {
    const fs::path dir = fs::temp_directory_path() / leaf;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

} // namespace

TEST_CASE("store: a saved preset lands in dataDir()/brushes and comes back on the next scan") {
    const fs::path home = freshDir("mosaic_test_brush_editor_home");
    const fs::path shipped = freshDir("mosaic_test_brush_editor_shipped"); // deliberately EMPTY
    const ScopedEnv xdg("XDG_DATA_HOME", home.string());
    const ScopedEnv installed("MOSAIC_DATA_DIR", shipped.string());

    ib::LibraryPreset lp = autoPreset("Editor Test Brush");
    lp.preset.tip.autoTip.generator.diameter = 41.0;
    lp.masterDiameter = 41.0;

    int savedIndex = -1;
    {
        BrushPresetStore store;
        std::string error;
        savedIndex = store.writeUserPreset(lp, {}, -1, &error);
        REQUIRE_MESSAGE(savedIndex >= 0, error);
        CHECK(store.presets().size() == 1);
        CHECK(store.presets()[0].preset.name == "Editor Test Brush");
        // ⚠ A preset saved NOW is the user's own even though no scan ever drew a boundary -- which
        // is exactly the case a bare scanDir() leaves behind (isUserPreset's -1 guard).
        CHECK(store.isUserPreset(savedIndex));
        CHECK(store.userPresetCount() == 1);
        // The store keeps the RESOLVED copy, so what was just saved paints what the editor previewed.
        CHECK(store.presets()[0].masterDiameter == 41.0);
    }

    // One file, and its name is a STEM of the preset's name -- not the name itself.
    std::vector<fs::path> written;
    for (const auto& e : fs::directory_iterator(home / "mosaic" / "brushes"))
        written.push_back(e.path());
    REQUIRE(written.size() == 1);
    CHECK(written[0].extension() == ".mbp");
    CHECK(written[0].stem().string() == "Editor_Test_Brush");

    // A fresh store rescans it. scan() reads the (empty) shipped dir first and DRAWS THE BOUNDARY
    // before the user's, so everything found below is the user's own.
    BrushPresetStore back;
    CHECK(back.scan() == 1);
    REQUIRE(back.presets().size() == 1);
    const ib::LibraryPreset& round = back.presets()[0];
    CHECK(round.preset.name == "Editor Test Brush");
    CHECK(round.preset.options.size() == 2);
    CHECK(round.preset.tip.kind == ib::TipXml::Kind::Auto);
    // ⚠ AN AUTO TIP CARRIES ITS OWN DIAMETER and the reload reads it from there -- which is why the
    // editor's Diameter control writes BOTH the resolved master size and the generator's.
    CHECK(round.masterDiameter == 41.0);
    CHECK(back.isUserPreset(0));
    CHECK(back.indexOfName("Editor Test Brush") == 0);
    // Selecting it resolves ONCE and hands out params built from the reloaded copy.
    REQUIRE(back.select(0));
    REQUIRE(back.activeParams() != nullptr);
    CHECK(back.activeParams()->diameter == 41.0);

    std::error_code ec;
    fs::remove_all(home, ec);
    fs::remove_all(shipped, ec);
}

TEST_CASE("store: a second save under the same name is renamed, never shadowed") {
    const fs::path home = freshDir("mosaic_test_brush_editor_home2");
    const fs::path shipped = freshDir("mosaic_test_brush_editor_shipped2");
    const ScopedEnv xdg("XDG_DATA_HOME", home.string());
    const ScopedEnv installed("MOSAIC_DATA_DIR", shipped.string());

    BrushPresetStore store;
    std::string error;
    REQUIRE(store.writeUserPreset(autoPreset("Twin"), {}, -1, &error) == 0);
    // uniqueName is what the editor runs the field's text through before it writes.
    CHECK(store.uniqueName("Twin") == "Twin copy");
    REQUIRE(store.writeUserPreset(autoPreset(store.uniqueName("Twin")), {}, -1, &error) == 1);
    CHECK(store.presets().size() == 2);
    CHECK(store.indexOfName("Twin") == 0);
    CHECK(store.indexOfName("Twin copy") == 1);

    // Overwriting one of the user's own replaces it IN PLACE -- no second file, no second row.
    ib::LibraryPreset edited = autoPreset("Twin");
    edited.preset.tip.autoTip.generator.diameter = 77.0;
    edited.masterDiameter = 77.0;
    REQUIRE(store.writeUserPreset(edited, {}, 0, &error) == 0);
    CHECK(store.presets().size() == 2);
    CHECK(store.presets()[0].masterDiameter == 77.0);

    int files = 0;
    for (const auto& e : fs::directory_iterator(home / "mosaic" / "brushes")) {
        (void)e;
        ++files;
    }
    CHECK(files == 2);

    std::error_code ec;
    fs::remove_all(home, ec);
    fs::remove_all(shipped, ec);
}

TEST_CASE("store: a loose preset whose tip file is not installed loads badged, never dropped") {
    // ⚠ THE HONESTY CONTRACT, and it is the reason `.mbp` is enough for this slice. A user preset
    // records its tip by REFERENCE; the pixels live in a bundle the file is not part of. With the
    // library empty there is nothing to adopt, so the preset must come back on the round tip AND say
    // so -- exactly as the library degrades an unresolvable bundle reference. A preset that vanished
    // instead would be data loss wearing a shrug.
    const fs::path dir = freshDir("mosaic_test_brush_editor_loose");

    ib::BrushPreset preset = autoPreset("Predefined Tip Brush").preset;
    preset.tip.kind = ib::TipXml::Kind::Predefined;
    preset.tip.type = "gbr_brush";
    preset.tip.predefined.filename = "not-installed-anywhere.gbr";
    preset.tip.predefined.scale = 1.0;

    mosaic::common::Image icon(4, 4);
    icon.fill({255, 255, 255, 255});
    std::string error;
    const auto bytes = ib::writeMbp(preset, icon, &error);
    REQUIRE_MESSAGE(bytes.has_value(), error);
    {
        std::ofstream out(dir / "loose.mbp", std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes->data()),
                  static_cast<std::streamsize>(bytes->size()));
    }

    BrushPresetStore store;
    CHECK(store.scanDir(dir) == 1);
    REQUIRE(store.presets().size() == 1);
    const ib::LibraryPreset& lp = store.presets()[0];
    CHECK(lp.preset.name == "Predefined Tip Brush");
    CHECK(lp.tipResolution == ib::TipResolution::Fallback);
    CHECK(lp.tip == nullptr);                                       // the analytic round tip
    CHECK_FALSE(lp.preset.provenance.droppedOptions.empty());       // ... and it says so
    CHECK(lp.preset.provenance.fidelity != ib::PresetFidelity::Exact);
    // ⚠ A bare scanDir draws NO shipped|user boundary, so nothing is the user's -- rather than
    // EVERYTHING being, which is what an unguarded `index >= 0` would have said (§8.2).
    CHECK_FALSE(store.isUserPreset(0));
    CHECK(store.userPresetCount() == 0);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("store: an unreadable loose file is skipped, and the rest still load") {
    const fs::path dir = freshDir("mosaic_test_brush_editor_junk");
    {
        std::ofstream out(dir / "broken.mbp", std::ios::binary);
        out << "this is not a PNG at all";
    }
    {
        std::ofstream out(dir / "empty.kpp", std::ios::binary);
    }
    std::string error;
    const auto bytes = ib::writeMbp(autoPreset("Survivor").preset,
                                    [] {
                                        mosaic::common::Image i(4, 4);
                                        i.fill({255, 255, 255, 255});
                                        return i;
                                    }(),
                                    &error);
    REQUIRE_MESSAGE(bytes.has_value(), error);
    {
        std::ofstream out(dir / "zzz-good.mbp", std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes->data()),
                  static_cast<std::streamsize>(bytes->size()));
    }

    BrushPresetStore store;
    CHECK(store.scanDir(dir) == 1); // one loaded; two refused, neither fatal
    REQUIRE(store.presets().size() == 1);
    CHECK(store.presets()[0].preset.name == "Survivor");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- ⭐ THE USER'S RUN LEADS (feedback round 1, item 7) ------------------------------------------

namespace {

// Write `name` as a loose `.mbp` into `dir`, through the SAME writer the app uses. `stem` decides
// the scan order within a directory, which is a plain path sort.
void writeLoose(const fs::path& dir, const std::string& stem, const std::string& name) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string error;
    REQUIRE_MESSAGE(mosaic::ui::writePresetFile(autoPreset(name), {}, dir / (stem + ".mbp"), &error),
                    error);
}

std::vector<std::string> nameOrder(const BrushPresetStore& store) {
    std::vector<std::string> out;
    for (const ib::LibraryPreset& p : store.presets())
        out.push_back(p.preset.name);
    return out;
}

} // namespace

TEST_CASE("store: the USER's presets lead the corpus, and each run keeps its own order") {
    // ⚠ THE USER'S CALL, and it moved an index range that three other things read. `presets()` used
    // to be "the shipped scan, then whatever the user installed"; it is now "the user's own, then
    // everything Mosaic ships". What must NOT change is the order WITHIN either half -- a reorder
    // that also reshuffled the shipped set would move 114 cells nobody asked to move.
    const fs::path home = freshDir("mosaic_test_brush_order_home");
    const fs::path shipped = freshDir("mosaic_test_brush_order_shipped");
    const ScopedEnv xdg("XDG_DATA_HOME", home.string());
    const ScopedEnv installed("MOSAIC_DATA_DIR", shipped.string());

    writeLoose(shipped / "brushes", "a_ship", "Shipped A");
    writeLoose(shipped / "brushes", "b_ship", "Shipped B");
    writeLoose(home / "mosaic" / "brushes", "a_mine", "Mine A");
    writeLoose(home / "mosaic" / "brushes", "b_mine", "Mine B");

    BrushPresetStore store;
    CHECK(store.scan() == 4);
    CHECK(nameOrder(store) ==
          std::vector<std::string>{"Mine A", "Mine B", "Shipped A", "Shipped B"});

    // ⚠ isUserPreset is now a PREFIX range, and the whole point is that it agrees with the order:
    // "the user's" and "the first m_userCount" have to be the same set or the dock's User tab and
    // the dock's order would disagree about the same preset.
    CHECK(store.userPresetCount() == 2);
    CHECK(store.isUserPreset(0));
    CHECK(store.isUserPreset(1));
    CHECK_FALSE(store.isUserPreset(2));
    CHECK_FALSE(store.isUserPreset(3));
    CHECK_FALSE(store.isUserPreset(-1)); // "Default round" is nobody's preset
    CHECK_FALSE(store.isUserPreset(4));  // past the end

    // The SELECTION is by name and survives the move -- which is exactly why it is stored by name.
    CHECK(store.indexOfName("Shipped A") == 2);
    REQUIRE(store.select(store.indexOfName("Shipped A")));
    CHECK(store.activePreset() != nullptr);
    CHECK(store.activePreset()->preset.name == "Shipped A");

    // ... and a NEW save lands in the user's run, i.e. at the FRONT half, pushing the shipped run
    // along by one. The index writeUserPreset reports is the one in the reordered corpus, not the
    // slot it happened to occupy in the loose vector.
    std::string error;
    const int saved = store.writeUserPreset(autoPreset("Mine C"), {}, -1, &error);
    REQUIRE_MESSAGE(saved >= 0, error);
    CHECK(saved == 2);
    CHECK(nameOrder(store) ==
          std::vector<std::string>{"Mine A", "Mine B", "Mine C", "Shipped A", "Shipped B"});
    CHECK(store.userPresetCount() == 3);
    CHECK(store.isUserPreset(2));
    CHECK_FALSE(store.isUserPreset(3));
    // The selection followed its NAME across the shift rather than staying on index 2.
    CHECK(store.activeIndex() == 3);
    CHECK(store.activePreset()->preset.name == "Shipped A");

    std::error_code ec;
    fs::remove_all(home, ec);
    fs::remove_all(shipped, ec);
}

TEST_CASE("store: a bare scanDir still owns NOTHING, and orders nothing differently") {
    // ⚠ THE -1 GUARD, which the reorder had to carry forward. A bare scanDir() draws NO boundary,
    // and "no boundary" means the whole run is SHIPPED -- never that the whole run is the user's.
    // Get that backwards and every preset in the library files itself under the user's own brushes
    // AND jumps to the front of the dock.
    const fs::path dir = freshDir("mosaic_test_brush_order_bare");
    writeLoose(dir, "a", "First");
    writeLoose(dir, "b", "Second");

    BrushPresetStore store;
    CHECK(store.scanDir(dir) == 2);
    CHECK(store.userPresetCount() == 0);
    CHECK_FALSE(store.isUserPreset(0));
    CHECK_FALSE(store.isUserPreset(1));
    CHECK(nameOrder(store) == std::vector<std::string>{"First", "Second"});

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- ⭐ DELETE (feedback round 1, item 5) --------------------------------------------------------

TEST_CASE("store: deleting a user preset takes the file, the entry and every pointer at it") {
    const fs::path home = freshDir("mosaic_test_brush_delete_home");
    const fs::path shipped = freshDir("mosaic_test_brush_delete_shipped");
    const ScopedEnv xdg("XDG_DATA_HOME", home.string());
    const ScopedEnv installed("MOSAIC_DATA_DIR", shipped.string());

    writeLoose(shipped / "brushes", "ship", "Shipped One");
    writeLoose(home / "mosaic" / "brushes", "a_mine", "Mine A");
    writeLoose(home / "mosaic" / "brushes", "b_mine", "Mine B");

    BrushPresetStore store;
    REQUIRE(store.scan() == 3);
    REQUIRE(nameOrder(store) == std::vector<std::string>{"Mine A", "Mine B", "Shipped One"});

    // ⚠ A SHIPPED PRESET IS NEVER DELETABLE, and the refusal is a sentence rather than a silence:
    // "the button is greyed out" is not an answer to "why can I not delete this".
    CHECK_FALSE(store.canDeletePreset(2));
    CHECK_FALSE(store.deleteRefusal(2).empty());
    std::string error;
    CHECK_FALSE(store.deleteUserPreset(2, &error));
    CHECK_FALSE(error.empty());
    CHECK(store.presets().size() == 3); // ... and nothing happened
    CHECK(fs::exists(shipped / "brushes" / "ship.mbp"));

    // The user's own goes -- entry AND file. A delete that left the file behind would resurrect the
    // preset on the next scan, which is a delete that un-deletes itself.
    REQUIRE(store.select(0));
    REQUIRE(store.activeParams() != nullptr);
    CHECK(store.activeIndex() == 0);

    int next = -99;
    error.clear();
    REQUIRE_MESSAGE(store.deleteUserPreset(0, &error, &next), error);
    CHECK(next == 0); // "Mine B" slid into the deleted slot: that is what the UI points at
    CHECK(nameOrder(store) == std::vector<std::string>{"Mine B", "Shipped One"});
    CHECK(store.userPresetCount() == 1);
    CHECK_FALSE(fs::exists(home / "mosaic" / "brushes" / "a_mine.mbp"));
    CHECK(fs::exists(home / "mosaic" / "brushes" / "b_mine.mbp"));

    // ⚠⚠ AND THE ACTIVE SELECTION DID NOT KEEP PAINTING WITH IT. rebuildAll re-points by name, and a
    // name that has left the corpus falls all the way back to NO preset -- index, name AND the
    // resolved params. An index reading -1 while activeParams() still handed out the deleted
    // brush's tip is a tool painting with a brush that no longer exists.
    CHECK(store.activeIndex() == -1);
    CHECK(store.activePreset() == nullptr);
    CHECK(store.activeParams() == nullptr);

    // The last one out leaves nothing to point at.
    error.clear();
    next = -99;
    REQUIRE_MESSAGE(store.deleteUserPreset(0, &error, &next), error);
    CHECK(nameOrder(store) == std::vector<std::string>{"Shipped One"});
    CHECK(store.userPresetCount() == 0);
    CHECK(next == 0); // one preset left, and it is at 0

    std::error_code ec;
    fs::remove_all(home, ec);
    fs::remove_all(shipped, ec);
}

// ---- ⭐ EXPORT (feedback round 1, item 6) --------------------------------------------------------

TEST_CASE("export writes the same .mbp the store writes, and the name always ends in one") {
    // ⚠ `.mbp`, and NOT `.kpp` (§8.3 ②) -- export does not quietly become a second serializer for a
    // format Mosaic deliberately does not write.
    CHECK(mosaic::ui::withMbpExtension(fs::path("brush")) == fs::path("brush.mbp"));
    CHECK(mosaic::ui::withMbpExtension(fs::path("brush.mbp")) == fs::path("brush.mbp"));
    CHECK(mosaic::ui::withMbpExtension(fs::path("brush.MBP")) == fs::path("brush.MBP"));
    // ⚠ APPENDED, never replace_extension: "Ink 2.0" reduces to a stem carrying a dot, and replacing
    // the extension would eat the "0" instead of adding anything.
    CHECK(mosaic::ui::withMbpExtension(fs::path("Ink 2.0")) == fs::path("Ink 2.0.mbp"));

    const fs::path dir = freshDir("mosaic_test_brush_export");
    ib::LibraryPreset lp = autoPreset("Exported Brush");
    lp.preset.tip.autoTip.generator.diameter = 63.0;
    lp.masterDiameter = 63.0;

    std::string error;
    const fs::path out = mosaic::ui::withMbpExtension(dir / "exported");
    REQUIRE_MESSAGE(mosaic::ui::writePresetFile(lp, {}, out, &error), error);
    REQUIRE(fs::exists(out));

    // It reads back through the ordinary reader -- an export nothing could open is not an export.
    std::ifstream in(out, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    REQUIRE_FALSE(bytes.empty());
    const std::optional<ib::BrushPreset> back = ib::readMbp(bytes.data(), bytes.size(), &error);
    REQUIRE_MESSAGE(back.has_value(), error);
    CHECK(back->name == "Exported Brush");
    CHECK(back->tip.autoTip.generator.diameter == 63.0);
    // The container carries a RASTER even though the caller handed in none: the raster IS the
    // preset's thumbnail (§3.1), so the writer renders the brush's own stroke rather than refusing.
    const std::optional<mosaic::common::Image> icon = ib::readMbpIcon(bytes.data(), bytes.size());
    REQUIRE(icon.has_value());
    CHECK_FALSE(icon->empty());

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- ⭐ THE SCRATCHPAD'S INK (feedback round 1, item 4) ------------------------------------------

TEST_CASE("a preview reports the pair it LANDED on, not the pair it was asked for") {
    // ⚠ THE RULING, in the one place it can be pinned. The editor's scratchpad and its auto stroke
    // share ONE surface, so they must share one ink -- and the ink the surface actually wears is not
    // always the one the style asked for: a brush that cannot mark the requested pair is re-laid on
    // the FALLBACK paper under the FALLBACK ink. A scratchpad that kept painting in the app's
    // foreground (or even in `style.ink`) would contradict the picture underneath it in precisely
    // the five shipped cases the fallback exists for.
    cb::StrokePreviewStyle style;
    style.paper = {255, 255, 255, 255};
    style.ink = {0, 0, 0, 255};

    cb::BrushParams plain;
    plain.diameter = 12.0;
    const cb::StrokePreviewRender ok = cb::renderStrokePreviewResolved(plain, 96, 40, style);
    CHECK_FALSE(ok.usedFallback);
    CHECK(ok.ink.r == style.ink.r);
    CHECK(ok.ink.g == style.ink.g);
    CHECK(ok.ink.b == style.ink.b);
    CHECK(ok.paper.r == style.paper.r);
    CHECK_FALSE(ok.image.empty());

    // Lighten is the IDENTITY for black over white: max(x, 0) == x in every channel. The brush is
    // not broken -- white under black is simply a pair it cannot move.
    cb::BrushParams blind = plain;
    blind.blendMode = mosaic::core::BlendMode::Lighten;
    const cb::StrokePreviewRender fell = cb::renderStrokePreviewResolved(blind, 96, 40, style);
    CHECK(fell.usedFallback);
    CHECK(fell.ink.r == style.fallbackInk.r);
    CHECK(fell.ink.g == style.fallbackInk.g);
    CHECK(fell.ink.b == style.fallbackInk.b);
    CHECK(fell.paper.r == style.fallbackPaper.r);
    // And the old entry point still hands back exactly the image the resolved one does.
    const mosaic::common::Image legacy = cb::renderStrokePreview(blind, 96, 40, style);
    CHECK(legacy.rgba == fell.image.rgba);
}

// ---- ⭐ THE RAIL PREVIEWS, AND IT PAYS THE DOCK'S PRICE (feedback round 1, items 2 + 3 + 5) ------

TEST_CASE("brush editor: the rail's presets lay their OWN stroke, once each") {
#if defined(__SANITIZE_ADDRESS__) ||                                                               \
    (defined(__has_feature) && __has_feature(address_sanitizer)) // NOLINT
    return; // FLTK/X11 internals leak on teardown under LeakSanitizer; this is not a memory test
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return;

    const fs::path home = freshDir("mosaic_test_brush_rail_home");
    const fs::path shipped = freshDir("mosaic_test_brush_rail_shipped"); // deliberately EMPTY
    const ScopedEnv xdg("XDG_DATA_HOME", home.string());
    const ScopedEnv installed("MOSAIC_DATA_DIR", shipped.string());

    BrushPresetStore store;
    std::string error;
    for (int i = 0; i < 6; ++i) {
        const int at =
            store.writeUserPreset(autoPreset("Rail " + std::to_string(i)), {}, -1, &error);
        REQUIRE_MESSAGE(at >= 0, error);
    }
    REQUIRE(store.presets().size() == 6);

    mosaic::ui::BrushEditorHost host; // no app behind it: every seam is optional by construction
    mosaic::ui::BrushEditorDialog dlg(&store, host);
    REQUIRE(dlg.seed(0));

    Fl_Widget* list = dlg.presetListForTest();
    REQUIRE(list != nullptr);
    // Nothing has been PAINTED yet, so nothing has been rendered: the strips are lazy, exactly like
    // the dock's thumbnails. A list that rendered all 114 shipped presets on open would stall the
    // editor for two tenths of a second before it ever appeared.
    CHECK(dlg.presetListStrokeRenders() == 0);

    // Draw the list into a viewport the size of the rail's own scroll window. ⚠ The clip is pushed
    // explicitly: `fl_not_clipped` is the lazy gate, and a surface with no clip would report every
    // one of the rows as visible -- which is precisely the cost the gate exists to avoid.
    constexpr int kViewW = 222;
    constexpr int kViewH = 190;
    const auto paint = [&] {
        auto* surf = new Fl_Image_Surface(kViewW, kViewH);
        Fl_Surface_Device::push_current(surf);
        fl_push_clip(0, 0, kViewW, kViewH);
        // (0, 0): Fl_Widget_Surface::draw subtracts a non-window widget's own x/y itself, so the
        // delta is where the widget's TOP-LEFT should land, not a translation of it.
        surf->draw(list, 0, 0);
        fl_pop_clip();
        Fl_Surface_Device::pop_current();
        delete surf;
    };

    paint();
    const std::size_t first = dlg.presetListStrokeRenders();
    CHECK(first > 0);
    // Only what FITS. The rows are 46 px in a 190 px window, so at most five can have been laid.
    CHECK(first <= static_cast<std::size_t>(kViewH / dlg.presetListRowHeight()) + 1);

    // ⚠⚠ AND A SECOND PAINT RENDERS NOTHING. This is the §8.2 contract, asserted the only honest way
    // -- on an EVENT count, because a cache SIZE cannot witness a re-render (it refills to exactly
    // the size it had). At ~1.0-1.7 ms a strip, a list that re-laid its strokes every draw would
    // make the whole dialog crawl.
    paint();
    CHECK(dlg.presetListStrokeRenders() == first);

    // ⭐ DELETE, end to end. Every preset here is one of the user's own, so the button is live.
    CHECK(dlg.canDeleteWorking());
    CHECK(dlg.presetIndex() == 0);
    const std::string doomed = dlg.working().preset.name;
    REQUIRE(dlg.deleteForTest());
    CHECK(store.presets().size() == 5);
    CHECK(store.indexOfName(doomed) == -1);
    // The editor re-seeded onto whatever took the slot rather than sitting on an index that is gone.
    CHECK(dlg.presetIndex() == 0);
    CHECK(dlg.working().preset.name != doomed);
    // ... and the strip cache went with it: every key in it named a preset that has since moved.
    const std::size_t afterDelete = dlg.presetListStrokeRenders();
    paint();
    CHECK(dlg.presetListStrokeRenders() > afterDelete);

    std::error_code ec;
    fs::remove_all(home, ec);
    fs::remove_all(shipped, ec);
}
