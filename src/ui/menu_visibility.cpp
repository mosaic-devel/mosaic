#include "ui/menu_visibility.hpp"

namespace mosaic::ui {
namespace {

bool isSubmenu(const Fl_Menu_Item* it) {
    return (it->flags & FL_SUBMENU) != 0;
}

// Does this title's subtree contain an item whose callback is `cb`? A null `cb` never matches, so
// an absent marker (no Debug menu in a release build) simply never claims a title.
bool titleContains(const Fl_Menu_Item* title, Fl_Callback* cb) {
    if (cb == nullptr)
        return false;
    int depth = 0;
    for (const Fl_Menu_Item* c = title + 1; c->label() != nullptr || depth > 0; ++c) {
        if (c->label() == nullptr) { // a null label closes the current submenu level
            --depth;
            continue;
        }
        if (c->callback() == cb)
            return true;
        if (isSubmenu(c))
            ++depth;
    }
    return false;
}

} // namespace

void setDocumentMenusVisible(Fl_Menu_Item* items, const DocumentMenuMarkers& markers,
                             bool visible) {
    if (items == nullptr)
        return;

    // Within File, the only items that make sense with NO document. Identified by callback so the
    // set survives label edits and translation.
    const auto alwaysAvailableFileItem = [&markers](const Fl_Menu_Item* it) {
        Fl_Callback* cb = it->callback();
        return cb == markers.fileNew || cb == markers.fileOpen || cb == markers.quit;
    };

    // Walk the flat array by RAW pointer -- NOT Fl_Menu_Item::next(), which SKIPS invisible items
    // (see FL/Fl_Menu_Item.H). The show-pass runs while document-only items are still hidden, so
    // next() would step straight over the very items that must be re-shown, leaving the bar stuck
    // in its no-document face. Advance one slot at a time and track submenu nesting: an FL_SUBMENU
    // item opens a level, a null-label item closes it, and depth returns to 0 at the array's end.
    bool firstTitle = true;
    int depth = 0;
    for (Fl_Menu_Item* t = items; t->label() != nullptr || depth > 0; ++t) {
        if (t->label() == nullptr) {
            --depth;
            continue;
        }
        if (depth == 0 && isSubmenu(t)) { // a bar top-level title
            if (firstTitle) {             // File: toggle only its document-only children
                firstTitle = false;
                int d = 0;
                for (Fl_Menu_Item* c = t + 1; c->label() != nullptr || d > 0; ++c) {
                    if (c->label() == nullptr) {
                        --d;
                        continue;
                    }
                    // New / Open / Quit stay; so does any submenu TITLE within File (Open Recent) --
                    // like Open, the recent list must work with no document, and its rows (nested
                    // at d>0) are never touched by this walk anyway.
                    if (d == 0 && !alwaysAvailableFileItem(c) && !isSubmenu(c)) {
                        if (visible)
                            c->show();
                        else
                            c->hide();
                    }
                    if (isSubmenu(c))
                        ++d;
                }
            } else if (titleContains(t, markers.about) || titleContains(t, markers.debugTool)) {
                // Help and Debug need no document: About is always meaningful, and the debug tools
                // (demo canvas, profiler, control-state exercisers) are exactly what you want on an
                // empty window.
                t->show();
            } else if (markers.settingsLivesInEditMenu && titleContains(t, markers.settings)) {
                // Edit with no document: the title stays, but only Settings inside it. Everything
                // else in Edit needs a document, and hiding those items takes their
                // FL_MENU_DIVIDERs with them -- so Settings is left alone with no stray separator.
                t->show();
                int d = 0;
                for (Fl_Menu_Item* c = t + 1; c->label() != nullptr || d > 0; ++c) {
                    if (c->label() == nullptr) {
                        --d;
                        continue;
                    }
                    if (d == 0 && c->callback() != markers.settings) {
                        if (visible)
                            c->show();
                        else
                            c->hide();
                    }
                    if (isSubmenu(c))
                        ++d;
                }
            } else { // a plain document menu (Image..View): the whole title comes and goes
                if (visible)
                    t->show();
                else
                    t->hide();
            }
        }
        if (isSubmenu(t))
            ++depth;
    }
}

} // namespace mosaic::ui
