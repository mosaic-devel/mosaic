#include "common/settings.hpp"
#include "core/inpaint/inpaint_engine.hpp"
#include "render/gpu_policy.hpp"
#include "ui/curve_editor.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/theme.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Box.H>
#include <FL/fl_draw.H>
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <functional>
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

using namespace mosaic;
namespace fs = std::filesystem;

namespace {
fs::path scratchDir(const char* name) {
    fs::path dir = fs::temp_directory_path() / (std::string("mosaic_test_") + name);
    fs::remove_all(dir);
    return dir;
}
}  // namespace

TEST_CASE("settings survive a save/load round-trip") {
    const fs::path file = scratchDir("settings_roundtrip") / "settings.json";

    common::Settings out;
    out.theme = "dark";
    out.logLevel = "warn";
    out.language = "de";
    out.units = "imperial";
    out.dockWidth = 341;                 // the dragged dock width survives (S16-g); a non-default int
    out.recentFiles = {"/tmp/b.mosaic", "/tmp/a.png"}; // Open Recent survives, order intact (S55)
    out.cropSwitchToolAfterApply = true; // a non-default bool must survive the round-trip (S16-p)
    out.cropInitialFraming = "draw";     // a non-default enum string must survive too (S16-q)
    out.cropClearSelectionOnLeave = true; // clear-staged-crop-on-leave bool survives
    out.multiSelectionEdits = "all";     // S15-e multi-selection mode survives too
    out.overlayLineStyle = "adaptive";   // Settings → Appearance overlay-line style survives
    out.iconPack = "candy";              // Settings → Appearance → Icons: the pack id survives (S52)
    out.spellCheck = false;              // Settings → Text (deferred §2): flipped from the default true
    out.spellCheckAllCaps = true;        // the all-caps toggle survives (default false)
    out.textLanguage = "de-DE";          // the default text language survives
    out.emojiFont = "Noto Color Emoji";  // the preferred emoji fallback family survives (R5)
    out.inpaintBackend = "pde";          // Settings → Inpainting: active backend survives
    out.inpaintPreset = "custom";        // hand-tuned sentinel survives
    out.inpaintParams = {{"K", 72.0}, {"poissonIterations", 150.0}}; // the override map survives
    // Settings → Tablet (docs/tablet.md §7): the whole policy survives, curve string included. The
    // curve is the SAME "x,y;" interchange form the presets use, so a round-trip that mangled it
    // would also mangle every imported preset's curve.
    out.tabletPressureCurve = "0,0;0.4,0.15;1,1;";
    out.tabletPressureMin = 0.05;
    out.tabletPressureMax = 0.92;
    out.tabletTiltOffsetDegrees = -22.5;
    out.tabletSpeedMax = 5.5;
    out.tabletSpeedWindowMs = 45.0;
    out.brushSmoothing = false; // the non-default, so a round-trip that dropped it would show
    out.selectBrushAddByDefault = false; // S18 select-brush default op survives (default true)
    out.antsCirculate = true;            // S18 hidden marching-ants experiment survives (default false)
    out.renderingMode = "cpu-only";      // Settings → Rendering (S60-b item 14) survives

    std::string err;
    REQUIRE_MESSAGE(common::saveSettings(out, file, &err), err);
    CHECK(fs::exists(file));

    bool existed = false;
    const common::Settings loaded = common::loadSettings(file, &err, &existed);
    CHECK(existed);
    CHECK(err.empty());
    CHECK(loaded == out);

    fs::remove_all(file.parent_path());
}

// Re-selecting a backend must scroll the Backend Settings pane back to the top: Fl_Scroll re-derives
// its position from the scrollbar widget on draw, so a stale offset would re-apply to the freshly
// rebuilt controls (they'd render shifted). Needs a display (it shows the dialog + pumps draws), so
// it self-skips when headless. Guards a bug hit repeatedly during S51 Inpainting development.
TEST_CASE("inpaint Backend Settings scroll resets to the top on backend re-select") {
#if defined(__SANITIZE_ADDRESS__) ||                                                                \
    (defined(__has_feature) && __has_feature(address_sanitizer)) // NOLINT
    return; // showing an FLTK window leaks on teardown under LeakSanitizer (FLTK/X11 internals);
            // this layout test isn't a memory test, so skip it in the ASan build.
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return; // headless CI: the scroll behaviour only manifests once the pane is drawn

    ui::applyTheme(ui::darkPalette());
    auto eng = core::inpaint::makeDefaultEngine();
    ui::SettingsHost host;
    for (const auto& id : eng.backendIds()) {
        const auto* b = eng.backend(id);
        if (b == nullptr || !b->available())
            continue;
        host.inpaintBackends.push_back({id, b->info(), b->settingsSchema()});
    }
    host.setInpaintBackend = [](const std::string&) {};
    host.setInpaintPreset = [](const std::string&) {};
    host.setInpaintParam = [](const std::string&, double) {};

    auto* dlg = new ui::SettingsDialog(host);
    common::Settings s;
    s.inpaintBackend = "offset-stats"; // the backend with several (scrollable) controls
    s.inpaintPreset = "balanced";
    dlg->seed(s);
    dlg->show();
    for (int i = 0; i < 5; ++i)
        Fl::check();

    // Locate the Backend Settings scroll (the one holding real sliders, not just its own scrollbars)
    // and the backend selector (the Fl_Choice listing the backend names).
    Fl_Scroll* scroll = nullptr;
    Fl_Choice* backendChoice = nullptr;
    const auto realSlider = [](Fl_Scroll* sc, int i) -> Fl_Slider* {
        auto* sl = dynamic_cast<Fl_Slider*>(sc->child(i));
        if (sl == nullptr || sl == &sc->scrollbar || sl == &sc->hscrollbar)
            return nullptr;
        return sl;
    };
    std::function<void(Fl_Widget*)> find = [&](Fl_Widget* w) {
        if (auto* sc = dynamic_cast<Fl_Scroll*>(w))
            for (int i = 0; i < sc->children(); ++i)
                if (realSlider(sc, i) != nullptr)
                    scroll = sc;
        if (auto* ch = dynamic_cast<Fl_Choice*>(w)) {
            const Fl_Menu_Item* m = ch->menu();
            for (int i = 0; m != nullptr && m[i].text != nullptr; ++i)
                if (std::string(m[i].text) == "Offset statistics")
                    backendChoice = ch;
        }
        if (auto* g = dynamic_cast<Fl_Group*>(w))
            for (int i = 0; i < g->children(); ++i)
                find(g->child(i));
    };
    find(dlg);
    REQUIRE(scroll != nullptr);
    REQUIRE(backendChoice != nullptr);

    const auto firstSliderY = [&]() -> int {
        for (int i = 0; i < scroll->children(); ++i)
            if (auto* sl = realSlider(scroll, i))
                return sl->y();
        return -1;
    };
    const int seededY = firstSliderY();
    CHECK(seededY > 0);

    // Scroll down, then re-select the same backend — the controls are rebuilt and must land at top.
    scroll->scroll_to(0, 120);
    for (int i = 0; i < 3; ++i)
        Fl::check();
    backendChoice->do_callback();
    for (int i = 0; i < 5; ++i)
        Fl::check();

    CHECK(scroll->yposition() == 0);
    CHECK(firstSliderY() == seededY); // first control back at its un-scrolled position

    dlg->hide();
    delete dlg;
}

// The Icons detail panel (Appearance → Icons): pack name, artist and link moved INSIDE the
// description's scroll (user call 2026-07-10) -- every one of the four fields is arbitrary-length
// third-party manifest text, so every one word-wraps and long content scrolls instead of pushing
// the rest out of the panel. The link line is CLICKABLE only when its text is actually a web
// link: "link" is a manifest claim, and an e-mail or a social handle must render as plain text.
// Display-gated like the scroll-reset case above.
TEST_CASE("icon pack detail: identity wraps inside the scroll; the link gates on http(s)") {
#if defined(__SANITIZE_ADDRESS__) ||                                                                \
    (defined(__has_feature) && __has_feature(address_sanitizer)) // NOLINT
    return; // showing an FLTK window leaks on teardown under LeakSanitizer (FLTK/X11 internals)
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return; // layout + hover behaviour only manifest with a display to draw on

    ui::applyTheme(ui::darkPalette());
    const std::string longName = "The Grand Compendium of Exhaustively Descriptive and Frankly "
                                 "Unreasonably Long Icon Pack Names, Volume II";
    const std::string url = "https://example.test/packs/grand-compendium";
    const std::string handle = "@pixelsmith, somewhere on the fediverse (not a URL)";
    ui::SettingsHost host;
    host.iconPacks.push_back({"webby", longName, "A. Artist", url, "Short description."});
    host.iconPacks.push_back({"handle", "Handle Pack", "B. Artist", handle, "Another one."});

    auto* dlg = new ui::SettingsDialog(host);
    common::Settings s;
    s.iconPack = "webby";
    dlg->seed(s);
    dlg->show();
    for (int i = 0; i < 5; ++i)
        Fl::check();

    std::function<Fl_Widget*(Fl_Widget*, const std::string&)> findLabeled =
        [&](Fl_Widget* w, const std::string& text) -> Fl_Widget* {
        if (w->label() != nullptr && text == w->label())
            return w;
        if (auto* g = dynamic_cast<Fl_Group*>(w))
            for (int i = 0; i < g->children(); ++i)
                if (Fl_Widget* hit = findLabeled(g->child(i), text))
                    return hit;
        return nullptr;
    };

    Fl_Widget* nameW = findLabeled(dlg, longName);
    Fl_Widget* linkW = findLabeled(dlg, url);
    REQUIRE(nameW != nullptr);
    REQUIRE(linkW != nullptr);
    // The identity lines live INSIDE the scroll (not drawn above it), in the same one.
    CHECK(dynamic_cast<Fl_Scroll*>(nameW->parent()) != nullptr);
    CHECK(nameW->parent() == linkW->parent());
    // The long name wrapped: taller than any single 15pt line could be.
    CHECK(nameW->h() >= 30);
    // A real URL advertises clickability (tooltip + hand cursor on hover; opening happens on
    // FL_PUSH, which a test must not fire -- it would launch a browser).
    CHECK(linkW->tooltip() != nullptr);
    CHECK(linkW->handle(FL_ENTER) == 1);
    linkW->handle(FL_LEAVE); // restore the cursor

    // Switch to the pack whose "link" is a social handle: the SAME line, re-labelled, refuses
    // link behaviour -- the gate is per-text, not per-pack. (FL_PUSH == 0 is the refusal; a
    // clickable link claims the push. FL_ENTER can't distinguish: Fl_Box claims enter/leave
    // unconditionally for FLTK's tooltip machinery.)
    s.iconPack = "handle";
    dlg->seed(s);
    for (int i = 0; i < 3; ++i)
        Fl::check();
    Fl_Widget* handleW = findLabeled(dlg, handle);
    REQUIRE(handleW != nullptr);
    CHECK(handleW == linkW);
    CHECK(handleW->tooltip() == nullptr);
    CHECK(handleW->handle(FL_PUSH) == 0);

    dlg->hide();
    delete dlg;
}

TEST_CASE("resolveUnits: explicit values pass through; auto follows the locale") {
    CHECK(common::resolveUnits("metric") == "metric");
    CHECK(common::resolveUnits("imperial") == "imperial");
    // "auto", "" and anything unrecognised resolve to a concrete system via the locale.
    const std::string detected = common::detectMeasurementSystem();
    CHECK((detected == "metric" || detected == "imperial"));
    CHECK(common::resolveUnits("auto") == detected);
    CHECK(common::resolveUnits("") == detected);
    CHECK(common::resolveUnits("nonsense") == detected);
    // The default preference is "auto", so a fresh Settings follows the locale.
    CHECK(common::resolveUnits(common::Settings{}.units) == detected);
}

TEST_CASE("a missing settings file yields defaults, not an error") {
    const fs::path file = scratchDir("settings_absent") / "settings.json";  // parent removed

    std::string err;
    bool existed = true;
    const common::Settings loaded = common::loadSettings(file, &err, &existed);
    CHECK_FALSE(existed);
    CHECK(err.empty());
    CHECK(loaded == common::Settings{});  // built-in defaults
}

TEST_CASE("a malformed settings file reports an error and falls back to defaults") {
    const fs::path dir = scratchDir("settings_malformed");
    fs::create_directories(dir);
    const fs::path file = dir / "settings.json";
    { std::ofstream(file) << "{ not valid : json "; }

    std::string err;
    const common::Settings loaded = common::loadSettings(file, &err);
    CHECK_FALSE(err.empty());
    CHECK(loaded == common::Settings{});

    fs::remove_all(dir);
}

TEST_CASE("unknown keys are ignored; a wrong-typed key is rejected safely") {
    const fs::path dir = scratchDir("settings_unknown");
    fs::create_directories(dir);
    const fs::path file = dir / "settings.json";
    // "theme" valid, an unknown key present, "logLevel" the wrong type.
    { std::ofstream(file) << R"({"theme":"light","future":42,"logLevel":123})"; }

    std::string err;
    const common::Settings loaded = common::loadSettings(file, &err);
    // The wrong-typed field makes the whole load fall back to clean defaults (with an error).
    CHECK_FALSE(err.empty());
    CHECK(loaded == common::Settings{});

    fs::remove_all(dir);
}

TEST_CASE("save replaces atomically and leaves no temp file behind") {
    const fs::path dir = scratchDir("settings_replace");
    const fs::path file = dir / "settings.json";

    common::Settings first;
    first.theme = "light";
    common::Settings second;
    second.theme = "dark";

    std::string err;
    REQUIRE(common::saveSettings(first, file, &err));
    REQUIRE(common::saveSettings(second, file, &err));  // overwrite

    bool existed = false;
    CHECK(common::loadSettings(file, &err, &existed).theme == "dark");
    fs::path tmp = file;
    tmp += ".tmp";
    CHECK_FALSE(fs::exists(tmp));

    fs::remove_all(dir);
}

TEST_CASE("the default settings path sits under the config dir") {
    const fs::path dir = common::configDir();
    if (!dir.empty()) {  // HOME/XDG set in any normal environment
        CHECK(dir.filename() == "mosaic");
        CHECK(common::defaultSettingsPath() == dir / "settings.json");
    }
}

namespace {
// Save/restore one environment variable across a test body (POSIX; the suite is Linux-only).
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : m_name(name) {
        if (const char* old = std::getenv(name))
            m_old = old;
        if (value != nullptr)
            setenv(name, value, 1);
        else
            unsetenv(name);
    }
    ~ScopedEnv() {
        if (m_old.has_value())
            setenv(m_name, m_old->c_str(), 1);
        else
            unsetenv(m_name);
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* m_name;
    std::optional<std::string> m_old;
};
}  // namespace

TEST_CASE("dataDir follows XDG_DATA_HOME, then HOME, then gives up") {
    SUBCASE("XDG_DATA_HOME wins") {
        ScopedEnv xdg("XDG_DATA_HOME", "/tmp/xdg-data");
        CHECK(common::dataDir() == fs::path("/tmp/xdg-data/mosaic"));
    }
    SUBCASE("an empty XDG_DATA_HOME does not shadow HOME") {
        ScopedEnv xdg("XDG_DATA_HOME", "");
        ScopedEnv home("HOME", "/home/someone");
        CHECK(common::dataDir() == fs::path("/home/someone/.local/share/mosaic"));
    }
    SUBCASE("neither set: empty, not a guess") {
        ScopedEnv xdg("XDG_DATA_HOME", nullptr);
        ScopedEnv home("HOME", nullptr);
        CHECK(common::dataDir().empty());
    }
}

TEST_CASE("installedDataDir: the environment override wins and is taken verbatim") {
    ScopedEnv env("MOSAIC_DATA_DIR", "/nonexistent/override");
    CHECK(common::installedDataDir() == fs::path("/nonexistent/override"));
}

TEST_CASE("installedDataDir: without the override it resolves to an existing directory or none") {
    ScopedEnv env("MOSAIC_DATA_DIR", nullptr);
    const fs::path dir = common::installedDataDir();
    if (!dir.empty())
        CHECK(fs::is_directory(dir));
}

// ⚠ Settings → Tablet lives at a POSITIONAL rail index (docs/tablet.md §8: names[] plus kNavCount,
// with the panes pushed in parallel and no enum to keep them honest). Inserting a category anywhere
// above it silently shifts every pane below it by one, and the failure looks like "the Tablet page
// shows Color Management" -- which nobody notices in a diff. So pin it: select the Tablet section by
// index and require that what became visible is the pane holding the pressure-curve editor.
//
// The same walk proves the pane actually builds its controls, which is the other thing a headless
// suite cannot otherwise see.
TEST_CASE("Settings → Tablet sits at the rail index its readout timer assumes") {
#if defined(__SANITIZE_ADDRESS__) ||                                                                \
    (defined(__has_feature) && __has_feature(address_sanitizer)) // NOLINT
    return; // showing an FLTK window leaks on teardown under LeakSanitizer (FLTK/X11 internals)
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return; // headless CI: a pane has no visibility until it is shown

    ui::applyTheme(ui::darkPalette());
    ui::SettingsHost host;
    host.tabletBackend = "x11/xi2";
    host.tabletDevices.push_back({"Wacom Intuos Pro S Pen", "Pen", "pressure, tilt"});
    host.tabletReading = [] {
        ui::SettingsHost::TabletReading r;
        r.valid = true;
        r.pressure = 0.5;
        r.rateHz = 200.0;
        return r;
    };

    auto* dlg = new ui::SettingsDialog(host);
    dlg->seed(common::Settings{});
    dlg->show();
    for (int i = 0; i < 5; ++i)
        Fl::check();

    // Find the visible pane holding a CurveEditor -- the Tablet pane's unmistakable fingerprint.
    ui::CurveEditor* curve = nullptr;
    bool curveVisible = false;
    std::function<void(Fl_Widget*)> find = [&](Fl_Widget* w) {
        if (auto* ce = dynamic_cast<ui::CurveEditor*>(w)) {
            curve = ce;
            curveVisible = ce->visible_r() != 0;
        }
        if (auto* g = dynamic_cast<Fl_Group*>(w))
            for (int i = 0; i < g->children(); ++i)
                find(g->child(i));
    };

    find(dlg);
    REQUIRE_MESSAGE(curve != nullptr, "the Tablet pane did not build its pressure-curve editor");
    CHECK_FALSE(curveVisible); // the dialog opens on General, not on Tablet

    // Click the rail row the pane's timer believes is Tablet (kTabletSection == 3 in the .cpp).
    // If a category is ever inserted above it, THIS is what fails -- loudly, and in the right place.
    dlg->selectTabletSectionForTest();
    for (int i = 0; i < 5; ++i)
        Fl::check();
    find(dlg);
    CHECK_MESSAGE(curveVisible,
                  "selecting the Tablet rail index showed some OTHER pane -- the rail is positional "
                  "and a category was inserted above Tablet without moving its pane");

    dlg->hide();
    Fl::check();
    Fl::delete_widget(dlg);
    Fl::check();
}

// Settings → Tablet laid out its controls against kInnerW, which runs to within 6 px of a scrolling
// pane's vertical scrollbar -- so the pressure-curve Reset button was placed at kInnerX + kLabelW +
// 272 and ended two pixels PAST the window's right edge, with its caption crushed into the 68 px
// strip that was left. Nothing caught it because nothing looked: a widget hanging off the pane is
// invisible to a suite that only asks whether the widget EXISTS.
//
// So ask the two questions a human asks of a layout, in integers, with no display needed: does
// every control fit inside the column it was given, and does any control sit on top of another?
// Both are properties of the geometry alone. This is the backstop for every future row on the page.
TEST_CASE("Settings → Tablet: no control escapes the pane or overlaps another") {
    ui::applyTheme(ui::darkPalette());
    ui::SettingsHost host;
    host.tabletBackend = "x11/xi2";
    // Two devices, one of them with a long name and no valuators: the row that actually wraps.
    host.tabletDevices.push_back({"Wacom Intuos Pro S Pen (0x357)", "Pen", "pressure, tilt, wheel"});
    host.tabletDevices.push_back({"Wacom Intuos Pro S Finger touch", "Puck", ""});

    auto* dlg = new ui::SettingsDialog(host);
    dlg->seed(common::Settings{});

    // The Tablet pane is the scroll that holds the CurveEditor -- the same fingerprint the rail test
    // uses, and one that survives the panes being reordered.
    Fl_Scroll* scroll = nullptr;
    std::function<void(Fl_Widget*, Fl_Scroll*)> find = [&](Fl_Widget* w, Fl_Scroll* enclosing) {
        if (dynamic_cast<ui::CurveEditor*>(w) != nullptr && enclosing != nullptr)
            scroll = enclosing;
        if (auto* g = dynamic_cast<Fl_Group*>(w)) {
            auto* self = dynamic_cast<Fl_Scroll*>(w);
            for (int i = 0; i < g->children(); ++i)
                find(g->child(i), self != nullptr ? self : enclosing);
        }
    };
    find(dlg, nullptr);
    REQUIRE_MESSAGE(scroll != nullptr, "the Tablet pane's scrolling body was not found");

    // The usable column: the scroll's rect less the width its vertical scrollbar claims. A control
    // that reaches into that strip is either clipped by it or sitting under it.
    const int usableL = scroll->x();
    const int usableR = scroll->x() + scroll->w() - Fl::scrollbar_size();

    std::vector<Fl_Widget*> laid;
    for (int i = 0; i < scroll->children(); ++i) {
        Fl_Widget* c = scroll->child(i);
        if (dynamic_cast<Fl_Scrollbar*>(c) != nullptr)
            continue; // the scroll's own bars are not content
        laid.push_back(c);
    }
    CHECK(laid.size() > 8); // the pane really did build its rows (a smoke check on the walk itself)

    for (Fl_Widget* c : laid) {
        const std::string what = c->label() != nullptr ? c->label() : "(unlabelled)";
        CHECK_MESSAGE(c->x() >= usableL, "control starts left of the pane: ", what);
        CHECK_MESSAGE(c->x() + c->w() <= usableR,
                      "control runs past the pane's usable width (under the scrollbar, or off the "
                      "window entirely): ",
                      what);
    }

    // Overlap. Every row on this page is a plain stacked rectangle, so ANY intersection is a bug --
    // a caption sized too short for its wrapped text collides with the row beneath it, which is the
    // other half of what shipped.
    for (std::size_t i = 0; i < laid.size(); ++i)
        for (std::size_t j = i + 1; j < laid.size(); ++j) {
            const Fl_Widget* a = laid[i];
            const Fl_Widget* b = laid[j];
            const int ox = std::min(a->x() + a->w(), b->x() + b->w()) - std::max(a->x(), b->x());
            const int oy = std::min(a->y() + a->h(), b->y() + b->h()) - std::max(a->y(), b->y());
            const bool overlap = ox > 0 && oy > 0;
            const std::string wa = a->label() != nullptr ? a->label() : "(unlabelled)";
            const std::string wb = b->label() != nullptr ? b->label() : "(unlabelled)";
            CHECK_MESSAGE(!overlap, "two controls overlap: '", wa, "' and '", wb, "'");
        }

    Fl::delete_widget(dlg);
    Fl::check();
}

// The other half of the shipped breakage, and the half the geometry check above CANNOT see: a
// wrapped Fl_Box does not grow to fit its label and does not clip it either -- it just draws the
// overflow outside its own rect, straight over whatever is beneath. The Devices list was sized at one
// row per device and ran through the Test area's title; several captions were a line short and ran
// into the row below. Every one of those widgets has a perfectly legal rect, so the only way to catch
// it is to ask FLTK how tall the text it is about to draw actually is.
//
// Needs a display for font metrics, so it self-skips headless -- but it runs everywhere a human would
// ever have seen the bug.
TEST_CASE("Settings → Tablet: every label fits the box it was given") {
#if defined(__SANITIZE_ADDRESS__) ||                                                                \
    (defined(__has_feature) && __has_feature(address_sanitizer)) // NOLINT
    return; // showing an FLTK window leaks on teardown under LeakSanitizer (FLTK/X11 internals)
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return; // headless: there are no font metrics to measure against

    ui::applyTheme(ui::darkPalette());
    ui::SettingsHost host;
    host.tabletBackend = "x11/xi2";
    host.tabletDevices.push_back({"Wacom Intuos Pro S Pen (0x357)", "Pen", "pressure, tilt, wheel"});
    host.tabletDevices.push_back({"Wacom Intuos Pro S Finger touch", "Puck", ""});

    auto* dlg = new ui::SettingsDialog(host);
    dlg->seed(common::Settings{});
    dlg->show(); // opens the display, so fl_measure has a font to measure with
    for (int i = 0; i < 5; ++i)
        Fl::check();
    dlg->selectTabletSectionForTest();
    for (int i = 0; i < 5; ++i)
        Fl::check();

    Fl_Scroll* scroll = nullptr;
    std::function<void(Fl_Widget*, Fl_Scroll*)> find = [&](Fl_Widget* w, Fl_Scroll* enclosing) {
        if (dynamic_cast<ui::CurveEditor*>(w) != nullptr && enclosing != nullptr)
            scroll = enclosing;
        if (auto* g = dynamic_cast<Fl_Group*>(w)) {
            auto* self = dynamic_cast<Fl_Scroll*>(w);
            for (int i = 0; i < g->children(); ++i)
                find(g->child(i), self != nullptr ? self : enclosing);
        }
    };
    find(dlg, nullptr);
    REQUIRE(scroll != nullptr);

    int measured = 0;
    for (int i = 0; i < scroll->children(); ++i) {
        auto* box = dynamic_cast<Fl_Box*>(scroll->child(i));
        if (box == nullptr || box->label() == nullptr || *box->label() == '\0')
            continue;
        fl_font(box->labelfont(), box->labelsize());
        int tw = (box->align() & FL_ALIGN_WRAP) != 0 ? box->w() : 0; // 0 = measure unwrapped
        int th = 0;
        fl_measure(box->label(), tw, th, 0);
        ++measured;
        const std::string text(box->label());
        CHECK_MESSAGE(tw <= box->w(), "label is wider than its box: '", text, "'");
        CHECK_MESSAGE(th <= box->h(),
                      "label is taller than its box -- it will be drawn over the row beneath it: '",
                      text, "' (needs ", th, "px, has ", box->h(), "px)");
    }
    CHECK(measured >= 8); // captions + the device list + the field labels really were walked

    dlg->hide();
    Fl::check();
    Fl::delete_widget(dlg);
    Fl::check();
}

TEST_CASE("brushSmoothing migrates from the STRENGTH it briefly was to the toggle it is") {
    // ⚠ It shipped for a few hours as a 0..1 strength before becoming a toggle (there is no useful
    // "a little bit of rattle"). A settings file written in that window holds a NUMBER here, and
    // get<bool>() on a number THROWS -- which would have taken the whole settings load down with it,
    // losing every other preference the user had. Any nonzero strength meant "smoothing on".
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "mosaic_smoothing_migrate";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path file = dir / "settings.json";

    const auto loadWith = [&](const std::string& json) {
        std::ofstream(file) << json;
        std::string err;
        bool existed = false;
        const common::Settings s = common::loadSettings(file, &err, &existed);
        CHECK(existed);
        CHECK_MESSAGE(err.empty(), err); // it must LOAD, not throw and lose everything else
        return s;
    };

    CHECK(loadWith(R"({"brushSmoothing": 0.35, "dockWidth": 321})").brushSmoothing);
    CHECK(loadWith(R"({"brushSmoothing": 1.0})").brushSmoothing);
    CHECK_FALSE(loadWith(R"({"brushSmoothing": 0.0})").brushSmoothing); // 0 meant off
    // ... and the other settings in the same file still survive the migration
    CHECK(loadWith(R"({"brushSmoothing": 0.35, "dockWidth": 321})").dockWidth == 321);

    // The modern form, both ways.
    CHECK(loadWith(R"({"brushSmoothing": true})").brushSmoothing);
    CHECK_FALSE(loadWith(R"({"brushSmoothing": false})").brushSmoothing);

    fs::remove_all(dir);
}

// ---- Settings → Rendering / the GPU-use policy (S60-b item 14) ---------------------------------
//
// The decision is a PURE function precisely so it can be pinned here, with no command line, no
// settings file and no device in the room. What is being protected is the PRECEDENCE: a flag is a
// one-run override and must beat a persisted preference, because a user whose saved setting has
// been made wrong by a driver update has no other way to get one usable run out of the app.

TEST_CASE("GPU-use policy: a flag beats the environment beats the saved setting") {
    using namespace mosaic::render;

    // Nobody said anything: Auto, which is the untouched default behaviour.
    CHECK(decideGpuPolicy(GpuUseOverride::None, GpuUseOverride::None, false).use == GpuUse::Auto);
    CHECK(decideGpuPolicy(GpuUseOverride::None, GpuUseOverride::None, false).allowsComputeLane());

    // Only the setting spoke.
    CHECK(decideGpuPolicy(GpuUseOverride::None, GpuUseOverride::None, true).use == GpuUse::CpuOnly);
    CHECK_FALSE(
        decideGpuPolicy(GpuUseOverride::None, GpuUseOverride::None, true).allowsComputeLane());

    // --cpu with the setting saying "auto": the flag wins.
    CHECK(decideGpuPolicy(GpuUseOverride::ForceCpuOnly, GpuUseOverride::None, false).use ==
          GpuUse::CpuOnly);
    // ...and --gpu with the setting saying "cpu-only": the flag wins THAT way too, which is the
    // half that matters -- an explicit GPU request must not be refused by a settings file.
    CHECK(decideGpuPolicy(GpuUseOverride::ForceAuto, GpuUseOverride::None, true).use ==
          GpuUse::Auto);

    // The environment beats the setting and loses to the flag, in both directions.
    CHECK(decideGpuPolicy(GpuUseOverride::None, GpuUseOverride::ForceCpuOnly, false).use ==
          GpuUse::CpuOnly);
    CHECK(decideGpuPolicy(GpuUseOverride::None, GpuUseOverride::ForceAuto, true).use ==
          GpuUse::Auto);
    CHECK(decideGpuPolicy(GpuUseOverride::ForceAuto, GpuUseOverride::ForceCpuOnly, true).use ==
          GpuUse::Auto);
    CHECK(decideGpuPolicy(GpuUseOverride::ForceCpuOnly, GpuUseOverride::ForceAuto, false).use ==
          GpuUse::CpuOnly);
}

TEST_CASE("MOSAIC_CPU_ONLY: unset means NO OPINION, not off") {
    using namespace mosaic::render;
    // Saved so a machine that actually runs the suite in CPU-only mode is put back exactly as it
    // was found -- this test is the one place in the binary that writes the variable.
    const char* saved = std::getenv("MOSAIC_CPU_ONLY");
    const std::string savedValue = saved != nullptr ? saved : std::string{};
    const bool wasSet = saved != nullptr;

    unsetenv("MOSAIC_CPU_ONLY");
    CHECK(gpuUseOverrideFromEnv() == GpuUseOverride::None); // absence is silence, not "off"
    setenv("MOSAIC_CPU_ONLY", "", 1);
    CHECK(gpuUseOverrideFromEnv() == GpuUseOverride::None); // and so is empty
    setenv("MOSAIC_CPU_ONLY", "1", 1);
    CHECK(gpuUseOverrideFromEnv() == GpuUseOverride::ForceCpuOnly);
    setenv("MOSAIC_CPU_ONLY", "yes", 1);
    CHECK(gpuUseOverrideFromEnv() == GpuUseOverride::ForceCpuOnly);
    // The escape hatch: an explicit off overrides a settings file that says cpu-only.
    setenv("MOSAIC_CPU_ONLY", "0", 1);
    CHECK(gpuUseOverrideFromEnv() == GpuUseOverride::ForceAuto);
    setenv("MOSAIC_CPU_ONLY", "false", 1);
    CHECK(gpuUseOverrideFromEnv() == GpuUseOverride::ForceAuto);

    if (wasSet)
        setenv("MOSAIC_CPU_ONLY", savedValue.c_str(), 1);
    else
        unsetenv("MOSAIC_CPU_ONLY");
}

TEST_CASE("a compute lane consulted with the policy off is untouched; on, it refuses BY NAME") {
    using namespace mosaic::render;
    // Restore whatever the process was running under, whatever this test does or throws -- the
    // whole suite shares one policy, and leaving it flipped would silently skip every GPU test
    // that runs after this one.
    struct Restore {
        GpuPolicy saved = gpuPolicy();
        ~Restore() { setGpuPolicy(saved); }
    };
    [[maybe_unused]] const Restore restore;

    setGpuPolicy(GpuPolicy{GpuUse::Auto});
    {
        std::string error = "untouched";
        CHECK(computeLaneAllowed("test-lane", error));
        CHECK(error == "untouched"); // the default path writes nothing and decides nothing
    }

    setGpuPolicy(GpuPolicy{GpuUse::CpuOnly});
    {
        std::string error;
        CHECK_FALSE(computeLaneAllowed("test-lane", error));
        // A refusal is a named, actionable sentence -- never an empty string and never a failure.
        CHECK_FALSE(error.empty());
        CHECK(error.find("test-lane") != std::string::npos);
        CHECK(error.find("CPU-only") != std::string::npos);
        // It must name a way BACK, or a user reading the log cannot undo what they turned on.
        CHECK(error.find("--cpu") != std::string::npos);
    }
}
