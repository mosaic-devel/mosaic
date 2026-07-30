#pragma once

// The macOS system menu bar (S58-b). Compiled only on Apple -- see ui/CMakeLists.txt.
//
// FLTK does the structural half: ui::MenuBar derives from Fl_Sys_Menu_Bar, whose macOS driver
// mirrors the Fl_Menu_Item array into NSMenus at the top of the screen and builds the application
// menu. This header is the two things that mirroring cannot carry: Mosaic's item badges, and the
// application menu's Mosaic-specific contents.

#include <FL/Fl_Widget.H> // Fl_Callback

namespace mosaic::ui {

class MenuBar;

// The application-menu strings, already translated by the caller. They are passed in rather than
// looked up here because the i18n template is extracted from .cpp/.hpp only (po/CMakeLists.txt
// globs those two), so an _() inside an .mm would silently never reach a translator.
//
// `appName` is substituted by the caller, not by a format specifier: FLTK runs each of these
// through -[NSString stringWithFormat:], and a stray '%' arriving from a translation would read
// past the argument list. installMacApplicationMenu() doubles any '%' it finds for that reason.
struct MacAppMenuText {
    const char* about = "About Mosaic";
    const char* services = "Services";
    const char* hide = "Hide Mosaic";
    const char* hideOthers = "Hide Others";
    const char* showAll = "Show All";
    const char* quit = "Quit Mosaic";
};

// Hand FLTK the strings it builds the application menu from. MUST run before anything opens the
// display (FLTK builds that menu once, inside fl_open_display, and keeps the pointers), so this
// belongs at the top of runApp, not in the window constructor.
void setMacApplicationMenuText(const MacAppMenuText& text);

// Wire the application menu's Mosaic-specific items: "About Mosaic" and "Settings…" (⌘,), which on
// macOS belong under the app's own name rather than in Help and Edit. Call once, after the window
// that owns the callbacks exists.
void installMacApplicationMenu(const char* settingsLabel, Fl_Callback* about, Fl_Callback* settings,
                               void* userData);

// Re-attach Mosaic's item badges to the live system menu. The system menu is a SNAPSHOT of the
// item array which FLTK rebuilds from scratch on every Fl_Sys_Menu_Bar::update(), taking the
// badges with it -- so this runs from MenuBar::update(), after each rebuild.
void applyMacMenuBadges(const MenuBar& bar);

} // namespace mosaic::ui
