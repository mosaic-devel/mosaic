#pragma once

#include <memory>
#include <string>

class Fl_Window; // forward-declared: this header stays free of FLTK/Wayland includes.

namespace mosaic::platform {

// A live xdg_foreign (zxdg_exporter_v2) export of a top-level window's surface, for the XDG Desktop
// Portal's "parent window" hint. On native Wayland the portal cannot parent + modal a dialog to our
// window from an X11 xid -- it needs a Wayland foreign handle, which FLTK does not expose -- so the
// file-chooser dialog floated as its own un-parented, non-modal taskbar window (user report). This
// exports the window's toplevel surface via zxdg_exporter_v2 and KEEPS THE EXPORT OBJECT ALIVE for
// its own lifetime (destroying it invalidates the handle), yielding the "wayland:<handle>" string
// the portal's SetParentWindow/OpenFile parent argument wants.
//
// Construct one around a portal call and let it die when the dialog closes. It is a no-op (empty
// handle, no Wayland objects) when the session is X11 -- the caller uses the "x11:<xid>" path there
// -- or when the compositor does not implement xdg_foreign. Cheap to construct (one registry
// round-trip on a private event queue, mirroring wayland_subsurface.cpp).
class WaylandForeignExport {
public:
    explicit WaylandForeignExport(Fl_Window* win);
    ~WaylandForeignExport();

    WaylandForeignExport(const WaylandForeignExport&) = delete;
    WaylandForeignExport& operator=(const WaylandForeignExport&) = delete;

    // "wayland:<handle>" once exported, or empty (X11 / no xdg_foreign / not shown). Feed a non-empty
    // value straight to the portal as the parent-window string.
    [[nodiscard]] const std::string& handle() const noexcept { return m_handle; }

private:
    struct Impl;                 // holds the live wl_display + exporter + exported proxies
    std::unique_ptr<Impl> m_impl; // null on X11 / when export failed
    std::string m_handle;
};

} // namespace mosaic::platform
