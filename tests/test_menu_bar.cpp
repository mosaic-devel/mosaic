#include "ui/menu_bar.hpp"

#include <doctest/doctest.h>

#include <FL/Enumerations.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Menu_Item.H>

#include <cstddef>

// The pure menu-bar motivational-ticker layout math (mosaic::ui::tickerRegion,
// docs/motivational-ticker.md): a right-anchored strip over ~3/4 of the menu bar's empty right
// region, hidden when that region falls below a minimum width. Headless -- no FLTK graphics.
namespace {
using mosaic::ui::tickerRegion;
using mosaic::ui::TickerRegion;

constexpr int kGap = 28;   // clear space after the titles
constexpr int kInset = 12; // clear space at the bar's right edge
constexpr int kMin = 180;  // hide the ticker below this leftover width
} // namespace

TEST_CASE("tickerRegion: wide bar -> a right-anchored 3/4 strip") {
    TickerRegion r;
    // bar [0,1000), titles use 300 -> leftover = 1000 - 300 - 28 - 12 = 660; 3/4 = 495.
    REQUIRE(tickerRegion(0, 1000, 300, kGap, kInset, kMin, r));
    CHECK(r.w == 660 * 3 / 4);
    CHECK(r.x == 1000 - kInset - r.w); // right-anchored to the bar's right inset
    CHECK(r.x + r.w == 1000 - kInset); // ... so the far edge sits one inset in from the bar's right
    // The strip never reaches the titles: a clear gap remains past usedWidth (>= kGap here).
    CHECK(r.x - 300 >= kGap);
}

TEST_CASE("tickerRegion: barX offset shifts the strip by the same amount") {
    TickerRegion a;
    TickerRegion b;
    REQUIRE(tickerRegion(0, 1000, 300, kGap, kInset, kMin, a));
    REQUIRE(tickerRegion(50, 1000, 300, kGap, kInset, kMin, b));
    CHECK(b.w == a.w);
    CHECK(b.x == a.x + 50);
}

TEST_CASE("tickerRegion: leftover below the minimum -> hidden") {
    TickerRegion r;
    // titles eat almost everything: leftover = 1000 - 820 - 28 - 12 = 140 < 180 -> hide.
    CHECK_FALSE(tickerRegion(0, 1000, 820, kGap, kInset, kMin, r));
}

TEST_CASE("tickerRegion: exactly the minimum shows; one px narrower hides") {
    TickerRegion r;
    // used=780 -> leftover = 1000 - 780 - 40 = 180 == kMin -> shown.
    CHECK(tickerRegion(0, 1000, 780, kGap, kInset, kMin, r));
    // one px narrower bar -> leftover 179 < 180 -> hidden.
    CHECK_FALSE(tickerRegion(0, 999, 780, kGap, kInset, kMin, r));
}

TEST_CASE("tickerRegion: degenerate (titles exceed the bar) -> hidden") {
    TickerRegion r;
    CHECK_FALSE(tickerRegion(0, 400, 500, kGap, kInset, kMin, r));
}

// ---- Item badges (the pop-up's right gutter) ----------------------------------------------------
// The per-item badge-kind predicate (MenuBar::setItemBadge): the pop-up asks itemBadgeFor() per
// item each time it renders, both to reserve gutter width and to pick the glyph ("fx" / the mini
// checkerboard). Pin the mapping mechanism headlessly: unset = None everywhere, and an installed
// predicate resolves each item to its kind (the app keys off the callback pointer, as here).

namespace {
void cbBadgeFx(Fl_Widget*, void*) {}
void cbBadgeTexture(Fl_Widget*, void*) {}
void cbBadgePlain(Fl_Widget*, void*) {}
} // namespace

TEST_CASE("itemBadgeFor: unset predicate -> None; installed -> the per-item kind") {
    using mosaic::ui::MenuBar;
    // A parent window adopts the bar's pop-up sub-windows (never shown -- headless).
    Fl_Double_Window win(0, 0, 640, 480);
    MenuBar bar(0, 0, 640, 26);
    win.end();
    bar.add("&Layer/Layer &Effects...", 0, cbBadgeFx);
    bar.add("&Layer/Text&ure Generator...", 0, cbBadgeTexture);
    bar.add("&Layer/&New Layer", 0, cbBadgePlain);

    // The leaves, found by callback -- the same identification rule the app's predicate uses.
    const Fl_Menu_Item* fx = bar.find_item(cbBadgeFx);
    const Fl_Menu_Item* tex = bar.find_item(cbBadgeTexture);
    const Fl_Menu_Item* plain = bar.find_item(cbBadgePlain);
    REQUIRE(fx != nullptr);
    REQUIRE(tex != nullptr);
    REQUIRE(plain != nullptr);

    // No predicate installed: every item (and null) resolves to None -- no badges, no gutter.
    CHECK(bar.itemBadgeFor(fx) == MenuBar::ItemBadge::None);
    CHECK(bar.itemBadgeFor(nullptr) == MenuBar::ItemBadge::None);

    bar.setItemBadge([](const Fl_Menu_Item* it) {
        if (it != nullptr && it->callback() == cbBadgeFx)
            return MenuBar::ItemBadge::Fx;
        if (it != nullptr && it->callback() == cbBadgeTexture)
            return MenuBar::ItemBadge::Texture;
        return MenuBar::ItemBadge::None;
    });
    CHECK(bar.itemBadgeFor(fx) == MenuBar::ItemBadge::Fx);
    CHECK(bar.itemBadgeFor(tex) == MenuBar::ItemBadge::Texture);
    CHECK(bar.itemBadgeFor(plain) == MenuBar::ItemBadge::None);
    CHECK(bar.itemBadgeFor(nullptr) == MenuBar::ItemBadge::None); // null never reaches the predicate
}

// ---- Badge pictograms (mosaic::ui::badgeShape) --------------------------------------------------
// badgeShape() is the ONE table both badge renderers draw from -- the pop-up's fl_rectf pass,
// and on macOS the template NSImages the system menu carries (ui/sys_menu_macos.mm) -- so a kind
// with no mark, or one spilling out of its design box, is a defect on both platforms at once. The
// switch there carries no `default:`, which is what makes it total; these sweep the whole enum for
// the rest: whole-pixel rects inside the box, and no two kinds wearing the same mark.

namespace {
using Badge = mosaic::ui::MenuBar::ItemBadge;
using mosaic::ui::BadgeRect;
using mosaic::ui::BadgeShape;
using mosaic::ui::badgeShape;
using mosaic::ui::kItemBadgeCount;

static_assert(kItemBadgeCount == static_cast<int>(Badge::TypeHorizontal) + 1,
              "kItemBadgeCount must track the last ItemBadge value or the sweeps below miss kinds");

constexpr int kBadgeGutter = 34; // menu_bar.cpp's kBadgeW: what a badged row reserves on the right
constexpr int kBadgeBoxH = 12;   // the design-box height every pictogram is drawn in (26px rows)

// Every kind but None (nothing to draw) and Fx (set as bold-italic type) is a rectangle pictogram.
bool isPictogram(Badge k) { return k != Badge::None && k != Badge::Fx; }

bool sameShape(const BadgeShape& a, const BadgeShape& b) {
    if (a.w != b.w || a.h != b.h || a.rects.size() != b.rects.size())
        return false;
    for (std::size_t i = 0; i < a.rects.size(); ++i)
        if (a.rects[i].x != b.rects[i].x || a.rects[i].y != b.rects[i].y ||
            a.rects[i].w != b.rects[i].w || a.rects[i].h != b.rects[i].h)
            return false;
    return true;
}
} // namespace

TEST_CASE("badgeShape: None and Fx carry no rectangles, every other kind carries some") {
    CHECK(badgeShape(Badge::None).rects.empty());
    CHECK(badgeShape(Badge::Fx).rects.empty()); // set type, not a pictogram
    for (int i = 0; i < kItemBadgeCount; ++i) {
        const auto kind = static_cast<Badge>(i);
        if (!isPictogram(kind))
            continue;
        CAPTURE(i);
        CHECK_FALSE(badgeShape(kind).rects.empty()); // a blank mark would render as an empty gutter
    }
}

TEST_CASE("badgeShape: every pictogram is whole-pixel rectangles inside its own design box") {
    for (int i = 0; i < kItemBadgeCount; ++i) {
        const auto kind = static_cast<Badge>(i);
        if (!isPictogram(kind))
            continue;
        CAPTURE(i);
        const BadgeShape shape = badgeShape(kind);
        CHECK(shape.w > 0);
        CHECK(shape.h > 0);
        CHECK(shape.w <= kBadgeGutter); // the row only reserves kBadgeW for the badge
        CHECK(shape.h <= kBadgeBoxH);   // ... and centres it in the row
        for (const BadgeRect& r : shape.rects) {
            CHECK(r.w > 0); // a zero-extent rect draws nothing: a typo, not a mark
            CHECK(r.h > 0);
            CHECK(r.x >= 0);
            CHECK(r.y >= 0);
            CHECK(r.x + r.w <= shape.w); // never spills the box: the crisp-at-any-scale rule
            CHECK(r.y + r.h <= shape.h);
        }
    }
}

TEST_CASE("badgeShape: the S53 rotate / flip / size / bool / type kinds each get their own mark") {
    const Badge added[] = {Badge::Rotate90CW,   Badge::Rotate90CCW,  Badge::Rotate180,
                           Badge::FlipH,        Badge::FlipV,        Badge::CanvasSize,
                           Badge::ImageSize,    Badge::TrimContent,  Badge::BoolUnion,
                           Badge::BoolSubtract, Badge::BoolIntersect, Badge::BoolExclude,
                           Badge::TypeVertical, Badge::TypeHorizontal};
    for (const Badge a : added) {
        CAPTURE(static_cast<int>(a));
        const BadgeShape sa = badgeShape(a);
        CHECK_FALSE(sa.rects.empty());
        for (int i = 0; i < kItemBadgeCount; ++i) { // no two menu rows may wear the same pictogram
            const auto b = static_cast<Badge>(i);
            if (b == a || !isPictogram(b))
                continue;
            CAPTURE(i);
            CHECK_FALSE(sameShape(sa, badgeShape(b)));
        }
    }
}

// ---- Toggle / radio on-marks (mosaic::ui::itemMark) ---------------------------------------------
// The pure seam the pop-up's right gutter draws through: item flags + on/off in, mark geometry and
// the gutter the row reserves out. Pin the three states the S53 radio groups (Type ▸ Orientation,
// Anti-Alias, Kerning, Direction) depend on -- selected radio, unselected radio, checked toggle --
// headlessly, with no graphics.

namespace {
using mosaic::ui::ItemMark;
using mosaic::ui::itemMark;

void cbRadioH(Fl_Widget*, void*) {}
void cbRadioV(Fl_Widget*, void*) {}
void cbToggleSnap(Fl_Widget*, void*) {}
} // namespace

TEST_CASE("itemMark: a plain row reserves no gutter and draws no mark") {
    const ItemMark m = itemMark(0, /*on=*/false);
    CHECK(m.gutter == 0);
    CHECK_FALSE(m.dot);
    CHECK_FALSE(m.ring);
}

TEST_CASE("itemMark: a checked toggle is one filled dot with no ring around it") {
    const ItemMark on = itemMark(FL_MENU_TOGGLE, /*on=*/true);
    CHECK(on.dot);
    CHECK(on.dotR > 0.0);
    CHECK_FALSE(on.ring);
    CHECK(on.gutter > 0);
    const ItemMark off = itemMark(FL_MENU_TOGGLE, /*on=*/false);
    CHECK_FALSE(off.dot);           // unchecked draws nothing -- the standard menu convention ...
    CHECK(off.gutter == on.gutter); // ... but the row still holds the gutter
}

TEST_CASE("itemMark: a selected radio is a dot inside a ring, so it never reads as a check mark") {
    const ItemMark radio = itemMark(FL_MENU_RADIO, /*on=*/true);
    const ItemMark toggle = itemMark(FL_MENU_TOGGLE, /*on=*/true);
    CHECK(radio.dot);
    CHECK(radio.ring);
    CHECK(radio.ringStroke > 0.0);
    CHECK(radio.ringR > radio.dotR);                          // the dot sits inside the ring ...
    CHECK(radio.ringR - radio.ringStroke / 2.0 > radio.dotR); // ... with clear air between them
    CHECK(radio.dotR < toggle.dotR); // and its dot is the smaller of the two, at a glance
    CHECK_FALSE(toggle.ring);
}

TEST_CASE("itemMark: an unselected radio draws nothing yet keeps the group's labels aligned") {
    const ItemMark on = itemMark(FL_MENU_RADIO, /*on=*/true);
    const ItemMark off = itemMark(FL_MENU_RADIO, /*on=*/false);
    CHECK_FALSE(off.dot); // no half-lit mark: a dimmed one reads as "disabled", not "not chosen"
    CHECK_FALSE(off.ring);
    CHECK(off.gutter > 0);
    CHECK(off.gutter == on.gutter); // identical reserved gutter -> every label on one x
}

TEST_CASE("itemMark: a radio reserves the toggle's gutter, so a mixed menu stays on one grid") {
    CHECK(itemMark(FL_MENU_RADIO, false).gutter == itemMark(FL_MENU_TOGGLE, false).gutter);
    CHECK(itemMark(FL_MENU_RADIO, true).gutter == itemMark(FL_MENU_RADIO, false).gutter);
}

TEST_CASE("itemMark: FL_MENU_RADIO wins when an item carries both mark flags") {
    const ItemMark m = itemMark(FL_MENU_RADIO | FL_MENU_TOGGLE, /*on=*/true);
    CHECK(m.ring); // FLTK's own item drawing resolves the pair the same way
}

TEST_CASE("itemMark: a real radio group marks only its selected member, on one shared gutter") {
    using mosaic::ui::MenuBar;
    Fl_Double_Window win(0, 0, 640, 480); // a parent for the bar's pop-ups -- never shown
    MenuBar bar(0, 0, 640, 26);
    win.end();
    bar.add("&Type/&Orientation/&Horizontal", 0, cbRadioH, nullptr, FL_MENU_RADIO | FL_MENU_VALUE);
    bar.add("&Type/&Orientation/&Vertical", 0, cbRadioV, nullptr, FL_MENU_RADIO);
    bar.add("&View/&Snap", 0, cbToggleSnap, nullptr, FL_MENU_TOGGLE | FL_MENU_VALUE);

    const Fl_Menu_Item* h = bar.find_item(cbRadioH);
    const Fl_Menu_Item* v = bar.find_item(cbRadioV);
    const Fl_Menu_Item* snap = bar.find_item(cbToggleSnap);
    REQUIRE(h != nullptr);
    REQUIRE(v != nullptr);
    REQUIRE(snap != nullptr);
    REQUIRE(h->value() != 0); // Horizontal is the chosen member of the group
    REQUIRE(v->value() == 0);

    const ItemMark chosen = itemMark(h->flags, h->value() != 0);
    const ItemMark other = itemMark(v->flags, v->value() != 0);
    const ItemMark checked = itemMark(snap->flags, snap->value() != 0);
    CHECK(chosen.dot);
    CHECK(chosen.ring);         // the selected member wears the ringed dot ...
    CHECK_FALSE(checked.ring);  // ... which a checked toggle never does
    CHECK(checked.dot);
    CHECK_FALSE(other.dot);     // and the unselected member wears nothing at all
    CHECK_FALSE(other.ring);
    CHECK(chosen.gutter == other.gutter);   // both members reserve the same gutter
    CHECK(chosen.gutter == checked.gutter); // as does the toggle row elsewhere in the bar
}

// ---- Accelerators a live text editor keeps (the S53 shortcut audit) ------------------------------
// FLTK dispatches menu item accelerators globally: the focus widget gets FL_KEYBOARD first, but the
// moment it DECLINES a chord the same keystroke returns as FL_SHORTCUT and the menu fires it behind
// the caret. ui::menuShortcutYieldsToTextEditor is the seam that stops the document- and
// selection-mutating ones -- pinned here, because "which chords are fenced" is a product decision
// that must not drift silently, in either direction.

TEST_CASE("guarded shortcuts: every S53 accelerator yields to an active editor") {
    using mosaic::ui::menuShortcutYieldsToTextEditor;
    // The eight chords the S53 menus introduced. Each one restructures the document or the
    // selection, which is never what a keystroke means while a caret is on screen.
    const int added[] = {
        FL_COMMAND + FL_ALT + 'i',   // Image -> Image Size...
        FL_COMMAND + FL_ALT + 'c',   // Image -> Canvas Size...
        FL_COMMAND + FL_SHIFT + 'v', // Edit -> Paste in Place
        FL_COMMAND + FL_SHIFT + 'd', // Select -> Reselect
        FL_COMMAND + FL_ALT + 'a',   // Select -> Select All Layers
        FL_COMMAND + 'f',            // Filter -> Last Filter
        FL_COMMAND + '[',            // Layer -> Send Backward
        FL_COMMAND + ']',            // Layer -> Bring Forward
    };
    for (const int s : added) {
        CAPTURE(s);
        CHECK(menuShortcutYieldsToTextEditor(s));
    }
    // The pre-existing layer-structure chords are the same class of edit, so they are fenced too.
    CHECK(menuShortcutYieldsToTextEditor(FL_COMMAND + FL_SHIFT + 'n')); // New Layer
    CHECK(menuShortcutYieldsToTextEditor(FL_COMMAND + 'j'));            // Duplicate Layer
    CHECK(menuShortcutYieldsToTextEditor(FL_COMMAND + 'g'));            // Group Layers
    CHECK(menuShortcutYieldsToTextEditor(FL_COMMAND + 'e'));            // Merge Down
    CHECK(menuShortcutYieldsToTextEditor(FL_SHIFT + (FL_F + 5)));       // Edit -> Fill...
}

TEST_CASE("guarded shortcuts: the app-level accelerators stay global with a caret on screen") {
    using mosaic::ui::menuShortcutYieldsToTextEditor;
    // Fencing everything would be its own bug: these are meant to work while you are typing, and
    // the editor itself already claims the ones it wants (Ctrl+C/X/V/A never reach the menu at
    // all). Ctrl+Shift+C is deliberately here: the Type session explicitly passes Copy Merged on.
    const int global[] = {
        FL_COMMAND + 'n',            FL_COMMAND + 'o', FL_COMMAND + 's',
        FL_COMMAND + FL_SHIFT + 's', FL_COMMAND + 'w', FL_COMMAND + 'q',
        FL_COMMAND + ',',            FL_COMMAND + 'z', FL_COMMAND + FL_SHIFT + 'z',
        FL_COMMAND + FL_SHIFT + 'c', FL_COMMAND + 'c', FL_COMMAND + 'v',
        FL_COMMAND + 'x',            FL_COMMAND + 'a', FL_COMMAND + 'd',
        FL_COMMAND + 'r',            FL_COMMAND + '=', FL_COMMAND + '-',
        FL_COMMAND + '0',            FL_COMMAND + ';',
    };
    for (const int s : global) {
        CAPTURE(s);
        CHECK_FALSE(menuShortcutYieldsToTextEditor(s));
    }
    CHECK_FALSE(menuShortcutYieldsToTextEditor(0)); // an item with no accelerator matches nothing
}

TEST_CASE("guarded shortcuts: the table is non-empty and carries no duplicate chord") {
    const std::span<const int> table = mosaic::ui::textEditorGuardedShortcuts();
    REQUIRE_FALSE(table.empty());
    for (std::size_t i = 0; i < table.size(); ++i) {
        CAPTURE(i);
        CHECK(table[i] != 0); // a zero entry would fence "no shortcut", i.e. every plain item
        for (std::size_t j = i + 1; j < table.size(); ++j)
            CHECK(table[i] != table[j]);
        CHECK(mosaic::ui::menuShortcutYieldsToTextEditor(table[i])); // the predicate reads it
    }
}
