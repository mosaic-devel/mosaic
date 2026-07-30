#pragma once

// Which menu entries survive while NO document is open.
//
// Lives apart from app_window.cpp because it is pure logic over an Fl_Menu_Item array -- no
// window, no document, no graphics -- so it can be exercised headlessly. That matters here: the
// failure mode is a menu bar that looks subtly wrong, which nothing catches until a human opens
// the menu. The previous version identified Help as "the last top-level title", which held right
// up until a localized build put an untranslated debug title after it, and then silently hid the
// real Help menu instead. Titles are now identified by the callbacks they contain.

#include <FL/Fl_Menu_Item.H>

namespace mosaic::ui {

// The callbacks that identify each special menu. A title is "the Help menu" because it contains
// About, not because of where it sits; anything not matched is a plain document-only menu.
struct DocumentMenuMarkers {
    // Items inside File that are meaningful with no document open.
    Fl_Callback* fileNew = nullptr;
    Fl_Callback* fileOpen = nullptr;
    Fl_Callback* quit = nullptr;
    // Title markers.
    Fl_Callback* about = nullptr;    // -> the Help menu; always visible
    Fl_Callback* debugTool = nullptr; // -> the Debug menu (if built); always visible
    Fl_Callback* settings = nullptr;  // -> the Edit menu; see settingsLivesInEditMenu
    // On macOS Preferences belongs to the application menu, so Edit is not kept alive for it and
    // behaves like any other document menu.
    bool settingsLivesInEditMenu = true;
};

// Show (visible=true) or hide (visible=false) every document-dependent entry in `items`, which is
// the flat array behind an Fl_Menu_Bar. With visible=false the bar keeps: File's New / Open /
// Open Recent / Quit, the whole Help and Debug titles, and Edit reduced to Settings alone.
//
// Only FL_MENU_INVISIBLE is flipped, so each item's own FL_MENU_DIVIDER survives the round trip
// and group separators return to their original places.
void setDocumentMenusVisible(Fl_Menu_Item* items, const DocumentMenuMarkers& markers, bool visible);

} // namespace mosaic::ui
