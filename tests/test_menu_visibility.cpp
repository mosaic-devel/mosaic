#include "ui/menu_visibility.hpp"

#include <doctest/doctest.h>

#include <FL/Fl_Menu_Item.H>

#include <cstring>
#include <string>
#include <vector>

// The no-document menu face (ui/menu_visibility.hpp). Pure logic over an Fl_Menu_Item array, so it
// runs headless -- which is the point: a wrong answer here is a subtly wrong menu bar, and nothing
// catches that until a human opens the menu. That is exactly how the original bug shipped.
namespace {
using mosaic::ui::DocumentMenuMarkers;
using mosaic::ui::setDocumentMenusVisible;

// Distinct callback identities; only their addresses matter.
void cbNew(Fl_Widget*, void*) {}
void cbOpen(Fl_Widget*, void*) {}
void cbQuit(Fl_Widget*, void*) {}
void cbSave(Fl_Widget*, void*) {}
void cbUndo(Fl_Widget*, void*) {}
void cbSettings(Fl_Widget*, void*) {}
void cbAbout(Fl_Widget*, void*) {}
void cbProfiler(Fl_Widget*, void*) {}
void cbResize(Fl_Widget*, void*) {}
void cbRecentRow(Fl_Widget*, void*) {}
// S53-b rows. The Image and Layer menus grew nested submenus (Rotate / Flip / Combine Paths) and
// the Type menu grew RADIO groups and rows that are born greyed -- three shapes the walk had never
// met, and each is a way to get the "hide everything, then put it all back" round trip wrong.
void cbRotateCW(Fl_Widget*, void*) {}
void cbFlipH(Fl_Widget*, void*) {}
void cbTrim(Fl_Widget*, void*) {}
void cbTypeRasterize(Fl_Widget*, void*) {}
void cbTypeHorizontal(Fl_Widget*, void*) {}
void cbTypeVerticalRL(Fl_Widget*, void*) {}
void cbCombineAdd(Fl_Widget*, void*) {}
void cbToggleMask(Fl_Widget*, void*) {}

// A menu shaped like the real bar. `translatedHelp` renames the Help title the way a catalog
// would, to prove nothing keys off the English word; `debugMenu` appends the untranslated Debug
// title AFTER Help, which is the arrangement that broke the old "Help is the last title" rule.
std::vector<Fl_Menu_Item> makeMenu(bool debugMenu, bool translatedHelp) {
    std::vector<Fl_Menu_Item> m;
    const auto item = [&m](const char* label, Fl_Callback* cb, int flags) {
        Fl_Menu_Item it{};
        it.text = label;
        it.callback_ = cb;
        it.flags = flags;
        m.push_back(it);
    };
    const auto close = [&m]() { m.push_back(Fl_Menu_Item{}); };

    item("&File", nullptr, FL_SUBMENU);
    item("&New...", cbNew, 0);
    item("&Open...", cbOpen, 0);
    item("Open &Recent", nullptr, FL_SUBMENU); // a submenu inside File: must never be hidden
    item("recent-1", cbRecentRow, 0);
    close();
    item("&Save", cbSave, FL_MENU_DIVIDER); // document-only
    item("&Quit", cbQuit, 0);
    close();

    item("&Edit", nullptr, FL_SUBMENU);
    item("&Undo", cbUndo, FL_MENU_DIVIDER); // document-only
    item("&Settings...", cbSettings, 0);
    close();

    item("&Image", nullptr, FL_SUBMENU); // a plain document menu
    item("Image Si&ze...", cbResize, FL_MENU_DIVIDER);
    item("&Rotate", nullptr, FL_SUBMENU); // S53: a submenu inside a HIDDEN top-level menu
    item("&90° Clockwise", cbRotateCW, 0);
    close();
    item("&Flip", nullptr, FL_SUBMENU);
    item("&Horizontal", cbFlipH, 0);
    close();
    item("&Trim to Content", cbTrim, 0);
    close();

    // S53-b Type: radio groups (the mode pickers) and rows that are born greyed because their
    // target does not exist yet. Both flags have to survive a hide/show round trip untouched.
    item("&Type", nullptr, FL_SUBMENU);
    item("&Rasterize Type", cbTypeRasterize, FL_MENU_INACTIVE | FL_MENU_DIVIDER);
    item("&Orientation", nullptr, FL_SUBMENU);
    // Labels distinct from Image ▸ Flip ▸ Horizontal: the helpers below find items BY LABEL, and
    // two rows sharing one would silently test the wrong menu.
    item("&Horizontal Text", cbTypeHorizontal, FL_MENU_RADIO | FL_MENU_VALUE);
    item("&Vertical Text", cbTypeVerticalRL, FL_MENU_RADIO);
    close();
    close();

    // S53-b Layer: the Combine Paths submenu plus a row whose LABEL is rewritten at runtime.
    item("&Layer", nullptr, FL_SUBMENU);
    item("Co&mbine Paths", nullptr, FL_SUBMENU);
    item("&Add", cbCombineAdd, 0);
    close();
    item("Disable Mask", cbToggleMask, 0);
    close();

    item(translatedHelp ? "&Hilfe" : "&Help", nullptr, FL_SUBMENU);
    item(translatedHelp ? "Ü&ber Mosaic" : "&About Mosaic", cbAbout, 0);
    close();

    if (debugMenu) {
        item("&Debug", nullptr, FL_SUBMENU);
        item("Timing Profiler...", cbProfiler, 0);
        close();
    }
    m.push_back(Fl_Menu_Item{}); // terminates the array
    return m;
}

DocumentMenuMarkers markers(bool withDebug, bool settingsInEdit = true) {
    DocumentMenuMarkers k;
    k.fileNew = cbNew;
    k.fileOpen = cbOpen;
    k.quit = cbQuit;
    k.about = cbAbout;
    k.settings = cbSettings;
    k.debugTool = withDebug ? cbProfiler : nullptr;
    k.settingsLivesInEditMenu = settingsInEdit;
    return k;
}

bool shown(const std::vector<Fl_Menu_Item>& m, const char* label) {
    for (const Fl_Menu_Item& it : m)
        if (it.text != nullptr && std::strcmp(it.text, label) == 0)
            return (it.flags & FL_MENU_INVISIBLE) == 0;
    FAIL("no such menu label: ", label);
    return false;
}
} // namespace

TEST_CASE("with no document: File, Edit, Help and Debug survive; document menus go") {
    auto m = makeMenu(/*debugMenu=*/true, /*translatedHelp=*/false);
    setDocumentMenusVisible(m.data(), markers(true), false);

    CHECK(shown(m, "&File"));
    CHECK(shown(m, "&New..."));
    CHECK(shown(m, "&Open..."));
    CHECK(shown(m, "Open &Recent")); // the recent list works with no document
    CHECK(shown(m, "&Quit"));
    CHECK_FALSE(shown(m, "&Save")); // needs a document

    CHECK(shown(m, "&Help")); // the reported bug: Help vanished on the empty window
    CHECK(shown(m, "&About Mosaic"));
    CHECK(shown(m, "&Debug"));
    CHECK(shown(m, "Timing Profiler..."));

    CHECK(shown(m, "&Edit"));          // kept, but...
    CHECK(shown(m, "&Settings..."));   // ... only Settings inside it
    CHECK_FALSE(shown(m, "&Undo"));

    CHECK_FALSE(shown(m, "&Image")); // a plain document menu, and so are the two S53-b ones
    CHECK_FALSE(shown(m, "&Type"));
    CHECK_FALSE(shown(m, "&Layer"));
    // Only the TITLE is flipped. A hidden title takes its whole subtree off the bar with it, so
    // the walk deliberately never touches nested rows -- which is exactly what leaves the
    // Rotate/Flip/Combine-Paths submenus, the radio dots and the greying intact for when the
    // document comes back. Asserted so a future "helpful" recursive hide is caught here.
    CHECK(shown(m, "&Rotate"));
    CHECK(shown(m, "&90° Clockwise"));
    CHECK(shown(m, "&Orientation"));
    CHECK(shown(m, "Co&mbine Paths"));
    CHECK(shown(m, "&Add"));
}

// The actual regression. The old code called the LAST top-level title "Help" and left it alone.
// Once a localized build put an untranslated "Debug" title after the translated Help, that rule
// protected Debug and hid the real Help instead.
TEST_CASE("a Debug title after a TRANSLATED Help does not steal Help's exemption") {
    auto m = makeMenu(/*debugMenu=*/true, /*translatedHelp=*/true);
    setDocumentMenusVisible(m.data(), markers(true), false);
    CHECK(shown(m, "&Hilfe"));      // identified by cbAbout, not by position or by the word "Help"
    CHECK(shown(m, "Ü&ber Mosaic"));
    CHECK(shown(m, "&Debug"));
    CHECK_FALSE(shown(m, "&Image"));
}

TEST_CASE("Help survives as the last title too (no Debug menu built)") {
    auto m = makeMenu(/*debugMenu=*/false, /*translatedHelp=*/true);
    setDocumentMenusVisible(m.data(), markers(false), false);
    CHECK(shown(m, "&Hilfe"));
    CHECK(shown(m, "Ü&ber Mosaic"));
    CHECK_FALSE(shown(m, "&Image"));
}

// macOS convention: Preferences lives in the application menu, so a one-item Edit menu on an empty
// window would be the wrong shape -- Edit stays hidden like any other document menu.
TEST_CASE("settingsLivesInEditMenu=false hides Edit entirely with no document") {
    auto m = makeMenu(/*debugMenu=*/false, /*translatedHelp=*/false);
    setDocumentMenusVisible(m.data(), markers(false, /*settingsInEdit=*/false), false);
    CHECK_FALSE(shown(m, "&Edit"));
    CHECK(shown(m, "&Help")); // Help is unaffected by the Edit convention
}

TEST_CASE("opening a document restores everything that was hidden") {
    auto m = makeMenu(/*debugMenu=*/true, /*translatedHelp=*/true);
    setDocumentMenusVisible(m.data(), markers(true), false);
    setDocumentMenusVisible(m.data(), markers(true), true);

    for (const char* label : {"&File", "&New...", "&Open...", "Open &Recent", "&Save", "&Quit",
                              "&Edit", "&Undo", "&Settings...", "&Image", "Image Si&ze...",
                              "&Rotate", "&90° Clockwise", "&Flip", "&Horizontal",
                              "&Trim to Content", "&Type", "&Rasterize Type", "&Orientation",
                              "&Horizontal Text", "&Vertical Text", "&Layer", "Co&mbine Paths",
                              "&Add", "Disable Mask", "&Hilfe", "Ü&ber Mosaic", "&Debug",
                              "Timing Profiler..."})
        CHECK(shown(m, label));
}

// hide()/show() flip only FL_MENU_INVISIBLE, so every OTHER flag has to come back exactly as it
// was. The S53-b menus lean on three of them: FL_MENU_RADIO + FL_MENU_VALUE carry the mode
// pickers' dots, and FL_MENU_INACTIVE carries the "no target for this yet" greying. A round trip
// that dropped any of them would leave a menu that looks right and behaves wrongly.
TEST_CASE("FL_MENU_RADIO, FL_MENU_VALUE and FL_MENU_INACTIVE survive a hide/show round trip") {
    auto m = makeMenu(/*debugMenu=*/true, /*translatedHelp=*/false);
    const auto flagsOf = [&m](const char* label) {
        for (const Fl_Menu_Item& it : m)
            if (it.text != nullptr && std::strcmp(it.text, label) == 0)
                return it.flags;
        FAIL("no such menu label: ", label);
        return 0;
    };
    REQUIRE((flagsOf("&Rasterize Type") & FL_MENU_INACTIVE) != 0);
    REQUIRE((flagsOf("&Horizontal Text") & FL_MENU_RADIO) != 0);
    REQUIRE((flagsOf("&Horizontal Text") & FL_MENU_VALUE) != 0);
    REQUIRE((flagsOf("&Vertical Text") & FL_MENU_VALUE) == 0);

    setDocumentMenusVisible(m.data(), markers(true), false);
    setDocumentMenusVisible(m.data(), markers(true), true);

    CHECK((flagsOf("&Rasterize Type") & FL_MENU_INACTIVE) != 0);
    CHECK((flagsOf("&Horizontal Text") & FL_MENU_RADIO) != 0);
    CHECK((flagsOf("&Horizontal Text") & FL_MENU_VALUE) != 0);
    CHECK((flagsOf("&Vertical Text") & FL_MENU_RADIO) != 0);
    CHECK((flagsOf("&Vertical Text") & FL_MENU_VALUE) == 0);
    // The submenu titles are back too, not just their rows.
    CHECK(shown(m, "&Orientation"));
    CHECK(shown(m, "Co&mbine Paths"));
}

// hide()/show() flip only FL_MENU_INVISIBLE, so group separators must come back where they were.
TEST_CASE("FL_MENU_DIVIDER survives a hide/show round trip") {
    auto m = makeMenu(/*debugMenu=*/true, /*translatedHelp=*/false);
    const auto dividerOn = [&m](const char* label) {
        for (const Fl_Menu_Item& it : m)
            if (it.text != nullptr && std::strcmp(it.text, label) == 0)
                return (it.flags & FL_MENU_DIVIDER) != 0;
        return false;
    };
    REQUIRE(dividerOn("&Save"));
    REQUIRE(dividerOn("&Undo"));
    setDocumentMenusVisible(m.data(), markers(true), false);
    setDocumentMenusVisible(m.data(), markers(true), true);
    CHECK(dividerOn("&Save"));
    CHECK(dividerOn("&Undo"));
}
