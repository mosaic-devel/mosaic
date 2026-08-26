#pragma once

#include <memory>

class Fl_Window; // forward-declared: this header stays free of FLTK/Wayland includes.

namespace mosaic::platform {

// Tells the compositor that a shown top-level window is a DIALOG, and whether it is modal to its
// parent -- xdg-dialog-v1.
//
// This is a hint, not a mechanism. Mosaic's dialogs are already genuinely modal: FLTK's own
// Fl_Window::set_modal() holds input, and fileDialogInputGuard keeps the chrome inert behind a
// native picker. What the compositor cannot know without this protocol is that the window IS a
// dialog, so it declines to do the things a desktop does with that knowledge -- dimming the parent,
// refusing to raise the parent above it, grouping the two in a task switcher. KWin acts on it;
// compositors that do not implement it ignore the request entirely and lose nothing.
//
// Construct one after show() and let it die with the dialog. Inert -- no Wayland objects, active()
// false -- on X11, on a stock FLTK with no xdg_toplevel accessor (platform/wayland_toplevel.hpp),
// or where the compositor does not offer the protocol.
class WaylandDialogHint {
public:
    // `modal` maps to xdg_dialog_v1.set_modal / unset_modal. Pass what FLTK was told, so the two
    // notions of modality cannot drift apart.
    WaylandDialogHint(Fl_Window* win, bool modal);
    ~WaylandDialogHint();

    WaylandDialogHint(const WaylandDialogHint&) = delete;
    WaylandDialogHint& operator=(const WaylandDialogHint&) = delete;

    // Whether the compositor actually took the hint.
    [[nodiscard]] bool active() const noexcept;

private:
    struct Impl; // the live xdg_wm_dialog_v1 + xdg_dialog_v1 proxies
    std::unique_ptr<Impl> m_impl;
};

} // namespace mosaic::platform
