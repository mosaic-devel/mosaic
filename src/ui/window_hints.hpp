#pragma once

class Fl_Window;

namespace mosaic::ui {

// Hand a JUST-SHOWN top-level window the things a Wayland compositor cannot work out for itself:
// the application icon, and -- for a modal or non-modal dialog -- the fact that it is a dialog.
//
// Call it immediately after show(), on every window that gets its own titlebar and taskbar entry.
// It is cheap (the icon is built once per process and reused) and idempotent, and it does nothing
// at all on X11, macOS and Windows, where FLTK's own Fl_Window::icon() already drives the icon and
// there is no dialog protocol to speak. See platform/wayland_toplevel_icon.hpp for why Wayland
// needs an explicit protocol here and X11 does not.
//
// Whether the dialog hint is sent is decided from FLTK's own modal()/non_modal() flags, which is
// exactly the condition under which FLTK gives the toplevel a PARENT -- and xdg-dialog-v1 is
// explicitly inert on a toplevel with no parent, so the two must agree or the hint is wasted.
void applyToplevelHints(Fl_Window* win);

} // namespace mosaic::ui
