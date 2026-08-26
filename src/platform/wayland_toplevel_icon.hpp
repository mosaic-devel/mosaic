#pragma once

#include "common/image.hpp"

#include <functional>
#include <string>

class Fl_Window; // forward-declared: this header stays free of FLTK/Wayland includes.

namespace mosaic::platform {

// What to show as the window icon. `name` is an XDG icon-theme name (ours is "mosaic", matching
// data/desktop/mosaic.desktop's Icon=); `raster` renders the same icon at an arbitrary edge length,
// which is what lets us answer the compositor's PREFERRED sizes exactly instead of guessing.
struct WaylandIconSpec {
    std::string name;
    // Returns a square, straight-alpha (NOT premultiplied) RGBA8 image `edge` px on a side, or an
    // empty Image if it cannot render that size. Called a handful of times, once per size, on the
    // first apply only.
    std::function<common::Image(int edge)> raster;
};

// Give `win`'s Wayland toplevel an application icon, via xdg-toplevel-icon-v1.
//
// WHY THIS EXISTS AT ALL. Wayland has no _NET_WM_ICON: a client cannot hand the compositor icon
// pixels the way it can on X11, and FLTK's Wayland driver implements no icons() override, so
// Fl_Window::icon() -- which Mosaic already sets -- is a silent no-op on this backend. The only
// other route to a window icon is app_id -> an INSTALLED .desktop -> Icon=, which is why an
// AppImage (never installed) shows a generic placeholder on Wayland while looking correct on X11.
// This protocol is the direct replacement, and the only one that works for an uninstalled binary.
//
// Returns true only if the icon was actually handed to the compositor. False covers, all of them
// ordinary and none of them an error worth bothering the user about:
//   * the session is X11, or `win` is not shown / is not a toplevel;
//   * FLTK lacks the toplevel accessor (see platform/wayland_toplevel.hpp) -- a stock distro FLTK;
//   * the compositor does not implement xdg-toplevel-icon-v1. KWin does; **Mutter does not yet**
//     (GNOME/mutter#4100), so GNOME sessions land here and keep the .desktop-derived icon.
//
// Safe and cheap to call for every top-level window: the icon object and its pixel buffers are
// built ONCE, on the first call that finds a usable toplevel, and reused for every later window.
// They then live for the rest of the process -- the protocol requires each wl_buffer to outlive the
// icon that references it, and an icon that is set once at startup and never changed has no reason
// to be freed. That is a few hundred KB of shm for the whole run, and it removes every use-after-
// free hazard the alternative would carry.
bool applyWaylandToplevelIcon(Fl_Window* win, const WaylandIconSpec& spec);

} // namespace mosaic::platform
