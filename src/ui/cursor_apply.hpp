#pragma once

#include "ui/cursors.hpp"

#include <memory>

class Fl_RGB_Image;
class Fl_Window;

// Handing a built `ui::CursorImage` to FLTK, plus the one stock-cursor SUBSTITUTION that more
// than one widget needs. `ui/cursors.hpp` stays FLTK-free (pure art, unit-tested headlessly);
// everything here touches Fl_Window and therefore cannot live beside it.
//
// Why a shared home at all: the Wayland move-cursor substitution was written three times -- once
// in the canvas, once in the Export preview, and not at all in the two gizmo panes that needed it
// just as much. A per-site copy is a per-site chance to be forgotten, and it was: the 2026-07-29
// chrome audit named the Export preview as "the one chrome site still on the stock request" while
// `Extrude3dViewport` (the 3D orbit gizmo) and `TexturePreviewPane` (the texture-generator pan)
// were both asking for it too. One implementation, so the next widget that wants a move cursor
// gets the substitution by construction rather than by remembering. See docs/wayland.md §2.5.
namespace mosaic::ui {

// Hand a device-pixel CursorImage to FLTK the way FLTK wants it. On the WAYLAND backend
// Fl_Wayland_Window_Driver::set_cursor_4args treats the Fl_RGB_Image's w()/h() AND the hotspot as
// LOGICAL units and multiplies both by the window's buffer scale -- so feeding it a 2x bitmap with
// its 2x hotspot draws the cursor at twice its intended size. Fl_RGB_Image::scale() is the answer:
// the DATA stays at device resolution (crisp) while w()/h() report the logical box, which is
// FLTK's own HiDPI image idiom. At scale 1 (X11, macOS) every line is an identity.
// Returns nullptr for an empty / malformed CursorImage; every caller falls back to a stock cursor.
[[nodiscard]] std::unique_ptr<Fl_RGB_Image> makeCursorImage(const CursorImage& c);

// The scale a CHROME widget should rasterize a custom cursor at: the window's own Wayland buffer
// scale, so a dialog on a 2x output gets the same crisp, correctly-sized pointer the canvas does.
// 1.0 on X11 and macOS -- neither has a per-window buffer scale to read, and macOS shows an
// Fl_RGB_Image cursor at pixel==point (VulkanCanvas::cursorBuildScale carries the same pin).
[[nodiscard]] double chromeCursorScale(Fl_Window* win);

// The four-way MOVE arrow -- "this is draggable in any direction" -- substituted for the stock
// FL_CURSOR_MOVE on the Wayland backend ONLY.
//
// The defect it exists for: FLTK resolves FL_CURSOR_MOVE on Wayland by the legacy Xcursor NAME
// `move`, and breeze_cursors -- the KDE default -- symlinks `move` to `dnd-move`, a CLOSED
// GRABBING HAND whose hotspot is dead centre (12,12 of 24) while its apparent point is its
// fingertips, ~10 px up and left. So the pointer looks like it is aiming somewhere the click does
// not land, and it says "you are dragging right now" during a mere hover. X11 asks for XC_fleur
// and gets a properly centred four-way arrow, which is what ui::moveCursor() rebuilds -- hence
// Wayland-only: substituting unconditionally would change the cursor for every X11 user, whose
// theme already answers correctly.
//
// One instance per widget that shows the cursor. Building it rasterizes an SVG, so the bitmap is
// cached on the only two things it varies with (the palette and the build scale); the widget owns
// the cache, which keeps its lifetime inside FLTK's rather than trailing a static Fl_RGB_Image
// past teardown.
class MoveCursor {
public:
    MoveCursor();
    ~MoveCursor();
    MoveCursor(const MoveCursor&) = delete;
    MoveCursor& operator=(const MoveCursor&) = delete;

    // Set the move cursor on `win` (the widget's own toplevel -- never fl_cursor(), which targets
    // Fl::first_window()). A null window, a non-Wayland backend or a rasterization failure all
    // land on the stock FL_CURSOR_MOVE.
    void apply(Fl_Window* win, bool darkMode, double buildScale);

    // The chrome overload: theme from the active palette, scale from chromeCursorScale(win).
    void apply(Fl_Window* win);

    // Drop the cached bitmap -- on a theme change, or when the window moves between outputs of
    // different scale and the art it was built for no longer matches.
    void reset() noexcept;

private:
    CursorImage m_pixels;
    std::unique_ptr<Fl_RGB_Image> m_image;
    bool m_dark = true;
    double m_scale = 0.0; // 0 = nothing cached (a real build scale is always >= 1)
};

} // namespace mosaic::ui
