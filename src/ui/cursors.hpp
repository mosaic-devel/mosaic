#pragma once

#include "common/image.hpp"
#include "core/selection.hpp" // core::SelectOp

// Procedurally drawn pointer cursors (S14 follow-up, user feedback). FLTK's stock
// FL_CURSOR_CROSS maps to the legacy server crosshair -- chunky, unthemed, and blind to the
// boolean-op modifiers -- so the selection tools carry their own RGBA cursor instead
// (Fl_Window::cursor(const Fl_RGB_Image*, hotx, hoty)). Pure builders (no FLTK), unit-tested
// in tests/test_cursors.cpp.
namespace mosaic::ui {

// A built cursor bitmap + its hotspot (the pixel that sits on the click point).
//
// TWO coordinate systems, and the difference is load-bearing (S59-a). `image`/`hotX`/`hotY` are
// DEVICE pixels: the builders rasterize at the HiDPI factor so the art stays crisp. But FLTK --
// and, under it, the Wayland compositor -- wants `Fl_Window::cursor(const Fl_RGB_Image*, hotx,
// hoty)` in LOGICAL units and re-applies the window's buffer scale ITSELF
// (Fl_Wayland_Window_Driver::set_cursor_4args multiplies both the image box and the hotspot by it).
// Handing it the device numbers therefore draws the cursor at scale x its intended size and pushes
// an OFF-CENTRE hotspot scale x too far from the point the art aims at -- ~10-15 px for the pan and
// fit-to-path hands on a 2x output, which is the "hovering around chrome seems offset" report.
// So every builder also reports the logical box it was asked for: the caller sets the device
// bitmap's drawing size with `Fl_RGB_Image::scale(logicalW, logicalH, 0, 1)` (FLTK's own HiDPI
// image idiom -- full-resolution data, logical extent) and passes logicalHot{X,Y} as the hotspot.
// On X11 and macOS the builders are driven at scale 1, where the two systems coincide and every
// one of those steps is an identity. See docs/wayland.md.
struct CursorImage {
    common::Image image; // the bitmap, in DEVICE pixels
    int hotX = 0;        // the hotspot as an index INTO `image` (device px)
    int hotY = 0;
    int logicalW = 0;    // the box the builder was asked for, before the HiDPI factor
    int logicalH = 0;
    int logicalHotX = 0; // the same hotspot in those logical units -- what FLTK must be given
    int logicalHotY = 0;
};

// The selection tools' pointer: a fine crosshair -- white core over a 1-px black halo, so it
// reads on any canvas content -- with the pending boolean op as a small glyph badge at the
// lower right: + (Add), - (Subtract), x (Intersect); Replace carries no badge. `scale` is a
// whole-pixel HiDPI factor (nearest-upscaled; the hotspot follows).
[[nodiscard]] CursorImage selectionCursor(core::SelectOp op, int scale = 1);

// The pan gestures' pointer (S16, native-Wayland follow-up): a hand -- open palm for "grab"
// (Space held, ready to pan) and a closed fist for "grabbing" (a pan drag / middle-mouse is under
// way). FLTK 1.4's Fl_Cursor enum has no open/closed grab pair (only the pointing FL_CURSOR_HAND),
// so we carry our own to stay identical across X11/Wayland/Windows/macOS. The art is the vendored
// GPLv3 apple_cursor SVGs (third_party/apple_cursor: hand1 = open, move = closed; white fill + black
// outline) rasterized through common::rasterizeSvg (nanosvg, which drops their drop-shadow filter --
// as wanted for a cursor); the hotspots are apple_cursor's own. `scale` is a whole-pixel HiDPI
// factor. Returns an empty image on failure (the caller falls back to a stock cursor).
[[nodiscard]] CursorImage panCursor(bool grabbing, int scale = 1);

// The Type tool's fit-to-path hover hand (S30 §9 follow-up, user 2026-07-14): shown while the
// pointer sits near a vector layer's path spine, where a CLICK flows text onto that path -- the
// affordance had no indicator at all. The art is the vendored apple_cursor hand2 (the flat open
// hand; the user's own pick), same white-fill/black-outline family as the pan hands, hotspot at
// the raised fingertip. `scale` is a whole-pixel HiDPI factor. Empty image on failure (the caller
// falls back to FL_CURSOR_HAND).
[[nodiscard]] CursorImage fitTextCursor(int scale = 1);

// The Move tool's rotate cursor (S15 follow-up; art swapped to the apple left_side double-arrow
// 2026-06-17): the vendored cursor is recoloured to a two-tone -- `darkMode` ? white outline / black
// inner : black outline / white inner -- so it reads on any content (nanosvg drops its drop-shadow
// filter). `angleRad` is the screen direction the straight arrow should point along; the rotation is
// baked into the SVG and rasterized in one AA pass (crisp). The CALLER decides that direction (the
// canvas anchors it to the nearest handle on hover, the box centre while dragging). The hotspot is
// the art's centre, the size-square's centre. `scale` is the device/content scale (>=1, may be
// fractional) -- the glyph is rasterized at that resolution so it stays crisp at any HiDPI factor.
[[nodiscard]] CursorImage rotateCursor(double angleRad, bool darkMode, double scale = 1.0);

// The Type tool's rotating I-beam (S29-b; docs/type-tool.md §6.1): the vendored apple_cursor xterm
// glyph, recoloured to the theme two-tone (`darkMode` ? white outline / black inner : the reverse)
// like the rotate cursor, and turned to the local baseline orientation -- `angleRad` is the screen
// angle of the baseline the caret will sit on (0 = a horizontal baseline → an upright I-beam; the
// caller passes the layer-rotation / on-path tangent / warp tangent). The clip-path is stripped so a
// turned I-beam is not cropped, and the rotation is baked into the SVG (one crisp AA pass), bucketed
// by the caller. The hotspot is the I-beam centre (the insertion point). `scale` is the device scale
// (>=1, may be fractional). Returns an empty image on failure (the caller falls back to a stock cursor).
[[nodiscard]] CursorImage textCursor(double angleRad, bool darkMode, double scale = 1.0);

// The two DIAGONAL RESIZE cursors (S59-a), for the Move / Crop / Shape / Type corner handles:
// `nwseCursor` points north-west <-> south-east, `neswCursor` north-east <-> south-west. Same art,
// theming and hotspot rule as `rotateCursor` (they ARE that straight double-arrow, baked to +-45
// deg), so a corner handle and the rotate band read as one family.
//
// Why they exist at all: FLTK's stock FL_CURSOR_NWSE / FL_CURSOR_NESW are resolved on the Wayland
// backend by their legacy X11 Xcursor names -- `fd_double_arrow` / `bd_double_arrow` -- which a
// theme is free not to ship. **breeze_cursors, the KDE default, ships neither.** When the lookup
// misses, FLTK falls back to a built-in **15x15 XPM with a dead-centre (7,7) hotspot**, inside a
// theme whose real arrows are 24 px and point from near their top-left corner: the handle cursor
// then sits several px from where it appears to point. X11 cannot miss this way
// (XCreateFontCursor always answers), so the CALLER substitutes these only where the stock lookup
// can fail -- swapping them in unconditionally would change the resize cursors for every X11 user.
// Returns an empty image on failure (the caller falls back to the stock cursor).
[[nodiscard]] CursorImage nwseCursor(bool darkMode, double scale = 1.0);
[[nodiscard]] CursorImage neswCursor(bool darkMode, double scale = 1.0);

// The FOUR-WAY MOVE arrow (2026-07-28), for every state that means "this is draggable in any
// direction": the Move tool's box body, the transform anchor, the selection-move hover, the DoF
// centre knob. The vendored apple_cursor `all-scroll`, theming and hotspot rule exactly as
// `rotateCursor` (blue outline / green inner recoloured to the theme two-tone, hotspot at the art
// centre, one crisp AA pass at the device scale) -- so the box's move, resize and rotate cursors
// read as one family.
//
// Why it exists, and it is the same story as nwseCursor/neswCursor with a worse ending: FLTK's
// stock FL_CURSOR_MOVE resolves on Wayland by the Xcursor NAME `move`, and **breeze_cursors, the
// KDE default, symlinks `move` -> `dnd-move` -- a CLOSED, GRABBING HAND**. So merely HOVERING a
// Move-tool-selected layer announced "you are dragging right now", and announced it with the wrong
// vocabulary entirely: a hand belongs to panning (panCursor's open/closed pair), the Move tool's
// own language is the move / resize / rotate arrows. On X11 the same request is XC_fleur -- the
// four-way arrow this rebuilds -- so the two backends showed different cursors for one state, and
// only one of them was right. Substituted by the CALLER on Wayland alone: X11 already answers
// correctly, and swapping unconditionally would change the cursor for every X11 user.
// Returns an empty image on failure (the caller falls back to the stock cursor).
[[nodiscard]] CursorImage moveCursor(bool darkMode, double scale = 1.0);

} // namespace mosaic::ui
