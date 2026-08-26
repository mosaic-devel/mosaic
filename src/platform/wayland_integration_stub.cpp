// The macOS and Windows side of the three Wayland xdg-shell integrations.
//
// Same reasoning as session_end_stub.cpp and preferWaylandBackendIfUnset(): whether the host has a
// Wayland compositor at all is a fact about the host, and it belongs on THIS side of the boundary.
// Defining these emptily here means src/ui calls them with no platform conditional at the call
// site -- and on both of these platforms FLTK's own Fl_Window::icon() already drives a real window
// icon, so there is nothing for the icon path to fall back to in the first place.

#include "platform/wayland_dialog.hpp"
#include "platform/wayland_toplevel.hpp"
#include "platform/wayland_toplevel_icon.hpp"

namespace mosaic::platform {

xdg_toplevel* waylandToplevel(Fl_Window*) {
    return nullptr;
}

bool waylandToplevelAccessorPresent() noexcept {
    return false;
}

bool applyWaylandToplevelIcon(Fl_Window*, const WaylandIconSpec&) {
    return false;
}

// Impl must be COMPLETE here even though nothing ever allocates one: ~unique_ptr<Impl> instantiates
// the deleter, and that needs the definition. An empty one is the whole of it off Wayland.
struct WaylandDialogHint::Impl {};

// m_impl stays null, so active() is false and the destructor has nothing to do.
WaylandDialogHint::WaylandDialogHint(Fl_Window*, bool) {}
WaylandDialogHint::~WaylandDialogHint() = default;

bool WaylandDialogHint::active() const noexcept {
    return false;
}

} // namespace mosaic::platform
