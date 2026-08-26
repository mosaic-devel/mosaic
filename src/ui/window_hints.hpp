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

// Apply the above to EVERY top-level window, for the life of the process -- the one call the app
// actually makes. Install it once, before the first window is shown.
//
// ⚠ This is deliberately central rather than a line after each show(), because the hand-wired
// version of this shipped broken: seven dialogs were wired and four -- New Document, Export, About,
// Select > Modify -- were simply missed, so they showed a placeholder icon and no modal dim while
// Settings looked correct. A per-site call is a rule every future dialog has to remember, and the
// failure is silent. This needs nothing from a new dialog.
//
// Runs from an Fl::add_check callback, so it fires on every pass of the event loop INCLUDING the
// nested Fl::wait() loops the modal dialogs spin -- which is the only reason a dialog opened from
// inside one gets its hints at all. Each window is handled once, on the pass after it is shown and
// before the frame that follows, so the icon is in place for the toplevel's first commit. A window
// that is hidden and later re-shown is treated as new, because FLTK gives it a new xdg_toplevel.
void installToplevelHintWatcher();

} // namespace mosaic::ui
