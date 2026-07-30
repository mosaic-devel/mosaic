#pragma once

class Fl_Window; // forward-declared: this header stays free of FLTK/X11/Wayland/Win32 includes.

namespace mosaic::platform {

// The vertical refresh rate, in Hz, of the display the *shown* window `win` is on -- i.e. how often
// that panel can actually put a new image in front of the user. The number is about the WINDOW, not
// about the process: on a two-monitor desk it follows the window from the 200 Hz panel to the 60 Hz
// one, and the caller may (and should) re-ask rather than cache it for the session.
//
// 0.0 means "cannot be determined": the window is not shown, the display server does not expose the
// mode (a RandR-less X server, a compositor whose wl_output never reported a current mode), or the
// value that came back was not plausible. ⚠ A caller that paces frames on this must treat 0.0 as
// "assume a conservative rate", never as "no limit" -- an unbounded present rate is exactly the
// defect this function exists to fix.
//
// Cheap enough to call several times a second: each backend keeps whatever mode table it needs live
// (the Wayland one is refreshed by FLTK's own event loop) and re-resolves only which display the
// window is on. It must be called on the UI thread -- it reads FLTK and display-server state.
[[nodiscard]] double displayRefreshHz(Fl_Window* win);

} // namespace mosaic::platform
