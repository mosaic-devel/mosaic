// macOS system menu bar: item badges + the application menu (S58-b).
//
// ⚠ Compile/link-verified only; there is no Mac in the loop. Runtime assumptions are flagged
// "Mac-side" below, as in the other .mm files here.
//
// WHY BADGES NEED THEIR OWN PATH. The in-window pop-ups draw the layer dock's glyphs in a row's
// right gutter -- the bold-italic "fx" on Layer Effects, the checkerboard on Texture Generator, an
// alignment pictogram on each Arrange ▸ Align -- as wayfinding. An NSMenuItem has no custom-draw
// hook; what it has is an image. So each badge is rasterized from the SHARED shape table
// (ui::badgeShape) into an NSImage marked `setTemplate:YES`.
//
// A template image contributes only its ALPHA: AppKit throws the colour away and tints the shape
// with whatever the menu is currently drawing text in. That single flag is what makes a badge
// black on a light menu, white in dark mode, white again while the row is highlighted blue, and
// grey when the row is disabled -- one image, every appearance, no theme observer.
//
// Mac-side unknowns: whether the 12x12 / 16x12 design boxes read at the size AppKit lays menu-item
// images out at, and how "fx" set at 12pt sits next to the menu font.

#include "ui/sys_menu_macos.hpp"

#include "ui/menu_bar.hpp"

#import <AppKit/AppKit.h>

#include <FL/Fl.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Sys_Menu_Bar.H>
#include <FL/platform.H> // Fl_Mac_App_Menu, fl_open_display

#include <cstddef>
#include <string>

namespace mosaic::ui {
namespace {

// FLTK feeds each application-menu string to -[NSString stringWithFormat:] (to substitute the
// bundle name for a "%@"). Mosaic passes its own name in the string instead, so any '%' that
// reaches here can only be an accident -- most likely from a translation -- and would make the
// formatter read an argument that was never pushed. Double it: "%%" formats back to a literal '%'.
std::string escapePercent(const char* s) {
    std::string out;
    for (const char* p = s != nullptr ? s : ""; *p != '\0'; ++p) {
        out.push_back(*p);
        if (*p == '%')
            out.push_back('%');
    }
    return out;
}

// The rasterized badges, one per ItemBadge kind, built on first use and kept for the process
// lifetime (a menu is rebuilt on every item change, so these would otherwise be re-rendered
// hundreds of times). Index by the enum's underlying value.
//
// The bound rides ui::kItemBadgeCount rather than a literal: it was a hand-kept 9 until S53 added
// the rotate/flip/size/boolean/type marks, and a stale literal here shows nothing on macOS while
// the pop-up renderer on every other platform draws them fine -- a silent, platform-only omission.
constexpr std::size_t kBadgeKinds = static_cast<std::size_t>(kItemBadgeCount);

NSImage* makeBadgeImage(MenuBar::ItemBadge kind) {
    if (kind == MenuBar::ItemBadge::None)
        return nil;

    NSImage* img = nil;
    if (kind == MenuBar::ItemBadge::Fx) {
        // Set type, not a pictogram: the same bold-italic "fx" the pop-up rows and the layer
        // chips wear. Black because the template flag below discards the colour anyway.
        NSFont* base = [NSFont boldSystemFontOfSize:12];
        NSFont* font = [[NSFontManager sharedFontManager] convertFont:base
                                                          toHaveTrait:NSItalicFontMask];
        NSDictionary* attrs = @{
            NSFontAttributeName : (font != nil ? font : base),
            NSForegroundColorAttributeName : [NSColor blackColor]
        };
        NSAttributedString* text = [[[NSAttributedString alloc] initWithString:@"fx"
                                                                   attributes:attrs] autorelease];
        const NSSize textSize = [text size];
        const NSSize box = NSMakeSize(ceil(textSize.width), 12.0);
        img = [NSImage imageWithSize:box
                             flipped:YES
                      drawingHandler:^BOOL(NSRect rect) {
                        [text drawAtPoint:NSMakePoint(0, (rect.size.height - textSize.height) / 2)];
                        return YES;
                      }];
    } else {
        // Whole-pixel rectangles from the shared table. Captured as a raw pointer + count: the
        // table has static storage duration, so the block outliving this call is fine.
        const BadgeShape shape = badgeShape(kind);
        const BadgeRect* rects = shape.rects.data();
        const int count = static_cast<int>(shape.rects.size());
        if (count == 0)
            return nil;
        // flipped:YES so the handler's y grows downward, matching the table's design box (which is
        // written for FLTK's top-left origin).
        img = [NSImage imageWithSize:NSMakeSize(shape.w, shape.h)
                             flipped:YES
                      drawingHandler:^BOOL(NSRect) {
                        [[NSColor blackColor] set];
                        for (int i = 0; i < count; ++i)
                            NSRectFill(NSMakeRect(rects[i].x, rects[i].y, rects[i].w, rects[i].h));
                        return YES;
                      }];
    }
    [img setTemplate:YES]; // tint with the menu's own text colour: light/dark/highlight/disabled
    return [img retain];   // cached for the process lifetime
}

NSImage* badgeImage(MenuBar::ItemBadge kind) {
    static NSImage* cache[kBadgeKinds] = {nil};
    static bool built[kBadgeKinds] = {false};
    const auto i = static_cast<std::size_t>(kind);
    if (i >= kBadgeKinds)
        return nil;
    if (!built[i]) {
        built[i] = true;
        cache[i] = makeBadgeImage(kind);
    }
    return cache[i];
}

// Walk one NSMenu and hang each item's badge on it.
//
// The Fl_Menu_Item behind an NSMenuItem is reachable through -getFlItem, which FLTK's own
// FLMenuItem subclass declares. We ask for the SELECTOR rather than the class: the class is
// private to Fl_MacOS_Sys_Menu_Bar.mm, and an item that does not answer to it is not one of ours
// (so it silently gets no badge) -- which is exactly the behaviour we want if FLTK ever renames it.
void decorateMenu(NSMenu* menu, const MenuBar& bar) {
    if (menu == nil)
        return;
    SEL flItemSel = @selector(getFlItem);
    for (NSMenuItem* item in [menu itemArray]) {
        if ([item hasSubmenu]) {
            decorateMenu([item submenu], bar); // one nested level today (Filter ▸ Blur), any depth here
            continue;
        }
        if (![item respondsToSelector:flItemSel])
            continue;
        using GetFlItemFn = const Fl_Menu_Item* (*)(id, SEL);
        auto getFlItem = reinterpret_cast<GetFlItemFn>([item methodForSelector:flItemSel]);
        const Fl_Menu_Item* mi = getFlItem(item, flItemSel);
        if (mi == nullptr)
            continue;
        const MenuBar::ItemBadge kind = bar.itemBadgeFor(mi);
        if (kind != MenuBar::ItemBadge::None)
            [item setImage:badgeImage(kind)];
    }
}

} // namespace

void setMacApplicationMenuText(const MacAppMenuText& text) {
    // FLTK keeps these pointers, so they must outlive the call.
    static std::string about, services, hide, hideOthers, showAll, quit;
    about = escapePercent(text.about);
    services = escapePercent(text.services);
    hide = escapePercent(text.hide);
    hideOthers = escapePercent(text.hideOthers);
    showAll = escapePercent(text.showAll);
    quit = escapePercent(text.quit);

    Fl_Mac_App_Menu::about = about.c_str();
    Fl_Mac_App_Menu::services = services.c_str();
    Fl_Mac_App_Menu::hide = hide.c_str();
    Fl_Mac_App_Menu::hide_others = hideOthers.c_str();
    Fl_Mac_App_Menu::show = showAll.c_str();
    Fl_Mac_App_Menu::quit = quit.c_str();
    // Mosaic cannot print. FLTK skips the whole "Print Front Window" block -- item, titlebar
    // toggle and separator -- when this title is empty, which is the only way to drop it.
    Fl_Mac_App_Menu::print = "";

    // NO Window menu at all. FLTK's default (tabbing_mode_automatic) also lets macOS merge windows
    // into native tabs, which would put a second, OS-level tab strip beside Mosaic's own document
    // tabs (S49) -- but tabbing_mode_none still leaves a Window menu, and that one is FLTK's: it is
    // appended after our last title, its contents are built from hard-coded English, and none of it
    // passes through the catalogs. Mosaic is a single-window app whose documents are tabs, so a menu
    // listing one window earns neither the width nor the untranslated corner. Must be set before the
    // first window is shown.
    Fl_Sys_Menu_Bar::window_menu_style(Fl_Sys_Menu_Bar::no_window_menu);
}

void installMacApplicationMenu(const char* settingsLabel, Fl_Callback* about, Fl_Callback* settings,
                               void* userData) {
    fl_open_display(); // builds the application menu if it does not exist yet

    // "About Mosaic" -- FLTK owns the item (it is item 0 of the application menu and carries the
    // localized title from setMacApplicationMenuText); this only points it at our dialog.
    if (about != nullptr)
        Fl_Sys_Menu_Bar::about(about, userData);

    if (settings == nullptr)
        return;
    // "Settings…" (⌘,) under the app's own name, the platform's home for it. FLTK stores the array
    // pointer, so it has to outlive this call.
    static std::string label;
    static Fl_Menu_Item items[2] = {};
    label = settingsLabel != nullptr ? settingsLabel : "Settings…";
    items[0].text = label.c_str();
    items[0].shortcut(FL_COMMAND + ',');
    items[0].callback(settings);
    items[0].user_data(userData);
    items[1].text = nullptr; // terminator

    // custom_application_menu_items() builds a hidden Fl_Menu_Bar to hold the array, and a widget
    // constructed while a group is open joins that group. This runs from the main window's
    // constructor, i.e. inside its begin()/end() -- so park the current group first, or the window
    // adopts a 0x0 menu bar it never asked for.
    Fl_Group* previous = Fl_Group::current();
    Fl_Group::current(nullptr);
    Fl_Mac_App_Menu::custom_application_menu_items(items);
    Fl_Group::current(previous);
}

void applyMacMenuBadges(const MenuBar& bar) {
    NSMenu* main = [NSApp mainMenu];
    if (main == nil)
        return;
    // Skip index 0, the application menu: FLTK owns those items, and its About item stores an
    // Fl_Menu_Item BY VALUE where the rest store a pointer -- so asking it for one would hand back
    // a callback address reinterpreted as an object. Everything from index 1 on is our bar.
    for (NSInteger i = 1; i < [main numberOfItems]; ++i)
        decorateMenu([[main itemAtIndex:i] submenu], bar);
}

} // namespace mosaic::ui
