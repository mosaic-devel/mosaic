#include "ui/menu_bar.hpp"

#include <doctest/doctest.h>

#include <FL/Enumerations.H>

// ---- Menu pop-up right-gutter geometry -------------------------------------------------------
//
// A pop-up row hangs up to three things off its right edge: the toggle/radio ON-MARK (or a submenu
// triangle), an item BADGE, and the right-aligned SHORTCUT text. The width pass reserves room for
// all three; the draw pass has to place them in the same chain, and for a long time it did not --
// the shortcut right-aligned to the pop-up's own right inset, ignoring both gutters. Since the
// on-mark is painted AFTER the text as an opaque square, a row with an accelerator AND a toggle had
// the tail of its shortcut repainted away (reported on View ▸ Show Guides, "Ctrl+;").
//
// ui::menuRowGutters is that chain, extracted so it exists exactly once. It is pure integer
// geometry, so the collision is directly assertable with no display, no font metrics and no
// screenshot -- which is the whole reason the arithmetic was worth extracting.

using mosaic::ui::menuRowGutters;
using mosaic::ui::MenuRowGutters;

namespace {
constexpr int kPopupW = 240; // any width: every field is relative to the right edge
} // namespace

TEST_CASE("a plain row gives its shortcut the whole right inset") {
    const MenuRowGutters g = menuRowGutters(kPopupW, /*markW=*/0, /*badged=*/false);
    // Nothing else occupies the gutter, so text and badge share the same right edge.
    CHECK(g.textRight == g.badgeRight);
    CHECK(g.textRight < kPopupW); // ... but never runs into the pop-up's own frame
}

TEST_CASE("a toggle row's shortcut stops clear of the on-mark's opaque blit") {
    // View ▸ Show Guides: FL_MENU_TOGGLE (so the width pass reserved a mark gutter) plus "Ctrl+;".
    constexpr int kMarkW = 16; // kArrowW -- the gutter itemMark() reserves from the FLAG
    const MenuRowGutters g = menuRowGutters(kPopupW, kMarkW, /*badged=*/false);
    // THE invariant: the text ends before the blit begins. The blit is opaque and painted later,
    // so an overlap is not a cosmetic crowding -- the glyphs under it are erased.
    CHECK(g.textRight <= g.markLeft);
    // ... and the mark itself still sits where it always did, inside the reserved gutter.
    CHECK(g.markLeft < g.markCenterX);
    CHECK(g.markCenterX < kPopupW);
    // Reserving the gutter must actually cost the text something, or the rule is a no-op.
    CHECK(g.textRight < menuRowGutters(kPopupW, /*markW=*/0, false).textRight);
}

TEST_CASE("a badged row's shortcut steps left past the badge too") {
    // Image ▸ Image Size…: "Ctrl+Alt+I" and an ImageSize badge, no mark. The same collision through
    // the other gutter -- unreported only because nobody had looked.
    const MenuRowGutters plain = menuRowGutters(kPopupW, 0, /*badged=*/false);
    const MenuRowGutters badged = menuRowGutters(kPopupW, 0, /*badged=*/true);
    CHECK(badged.badgeRight == plain.badgeRight); // the badge owns the outermost free gutter
    CHECK(badged.textRight < badged.badgeRight);  // ... and the text starts inboard of it
}

TEST_CASE("a row wearing both a mark and a badge stacks the two gutters") {
    // Type ▸ Orientation is a badged radio group: mark outermost, badge inboard, text inboard of
    // that. Every step must be strictly monotone or two occupants share pixels.
    constexpr int kMarkW = 16;
    const MenuRowGutters g = menuRowGutters(kPopupW, kMarkW, /*badged=*/true);
    CHECK(g.textRight < g.badgeRight);
    CHECK(g.badgeRight <= g.markLeft);
    CHECK(g.textRight <= g.markLeft);
    // Both gutters are paid for, not just the wider one.
    const MenuRowGutters markOnly = menuRowGutters(kPopupW, kMarkW, false);
    CHECK(g.textRight < markOnly.textRight);
}

TEST_CASE("the chain scales with the pop-up width, never with the row's contents") {
    // Every field is anchored to the right EDGE, so widening the pop-up moves all of them by the
    // same amount -- the property that lets one expression serve three draw sites.
    constexpr int kMarkW = 16;
    const MenuRowGutters a = menuRowGutters(200, kMarkW, true);
    const MenuRowGutters b = menuRowGutters(260, kMarkW, true);
    CHECK(b.textRight - a.textRight == 60);
    CHECK(b.badgeRight - a.badgeRight == 60);
    CHECK(b.markLeft - a.markLeft == 60);
    CHECK(b.markCenterX - a.markCenterX == 60);
}
