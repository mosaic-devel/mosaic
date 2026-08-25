#include "platform/native_window.hpp"

// FLTK's platform headers expose the native-handle accessors. FL/platform.H drags in
// <X11/Xlib.h>, whose macros (None, Status, Bool, ...) are harmless here: this TU uses no
// such identifiers and includes no Vulkan headers.
#include <FL/Fl_Window.H>
#include <FL/platform.H> // fl_x11_display(), fl_x11_xid()
#include <FL/wayland.H>  // fl_wl_display(), fl_wl_xid(), fl_wl_surface(), fl_wl_buffer_scale()
#include <cstdint>
#include <cstdlib>

namespace mosaic::platform {

void preferWaylandBackendIfUnset() {
    const char* backend = std::getenv("FLTK_BACKEND");
    const bool unset = (backend == nullptr) || (backend[0] == '\0');
    if (unset && std::getenv("WAYLAND_DISPLAY") != nullptr) {
        // Written rather than left to FLTK's own "Wayland if WAYLAND_DISPLAY" default so the
        // choice is OURS and inspectable: code that has to know the backend BEFORE any window
        // exists reads this variable (activeBackend() needs a shown window), and a future FLTK
        // is free to change what "unset" means. A user-set value reaches here non-empty and is
        // never touched -- FLTK_BACKEND=x11 remains the escape hatch.
        setenv("FLTK_BACKEND", "wayland", /*overwrite=*/1);
    }
    // No WAYLAND_DISPLAY: leave the variable alone. FLTK then picks X11 on a pure-Xorg session,
    // which is what those users want, and a Wayland-less session/build is never written to.
}

WindowSystem activeBackend() {
    // In the hybrid build exactly one display accessor is non-null once FLTK is up.
    return (fl_wl_display() != nullptr) ? WindowSystem::Wayland : WindowSystem::X11;
}

int windowBufferScale(Fl_Window* win) {
    // ⚠ fl_wl_buffer_scale() casts the window's driver to Fl_Wayland_Window_Driver unconditionally,
    // so it must never be called on the X11 half of the hybrid build -- the display probe is the
    // guard, exactly as in nativeSurfaceHandle() below. FLTK itself copes with an unmapped window
    // (Fl_Wayland_Window_Driver::wld_scale falls back to the largest scale among known outputs).
    if (win == nullptr || fl_wl_display() == nullptr)
        return 1;
    const int scale = fl_wl_buffer_scale(win);
    return scale > 0 ? scale : 1;
}

void raiseNativeWindowToTop(Fl_Window* /*win*/) {
    // Nothing to do on X11 or Wayland. A menu pop-up is not a sibling of the canvas here: on X11
    // FLTK maps it as an override-redirect TOP-LEVEL window, and on Wayland the canvas lives on its
    // own wl_subsurface with the chrome on the parent surface. Neither can be occluded by the
    // canvas, which is exactly why the bug this answers is Windows-only.
}

bool nativeSurfaceHandle(Fl_Window* win, NativeSurfaceHandle& out, std::string& error) {
    if (win == nullptr || win->shown() == 0) {
        error = "native handle requested for a window that is not shown";
        return false;
    }

    if (struct wl_display* wlDisplay = fl_wl_display()) {
        struct wld_window* xid = fl_wl_xid(win);
        struct wl_surface* surface = xid ? fl_wl_surface(xid) : nullptr;
        if (surface == nullptr) {
            error = "no wl_surface for window";
            return false;
        }
        out.system = WindowSystem::Wayland;
        out.display = wlDisplay;
        out.window = surface;
        out.scale = fl_wl_buffer_scale(win);
        if (out.scale < 1)
            out.scale = 1;
    } else {
        Display* display = fl_x11_display();
        const Window xid = fl_x11_xid(win);
        if (display == nullptr || xid == 0) {
            error = "no X11 handle for window";
            return false;
        }
        out.system = WindowSystem::X11;
        out.display = display;
        out.window = reinterpret_cast<void*>(static_cast<std::uintptr_t>(xid));
        // ⚠ HARD-CODED, deliberately (re-affirmed S59-a). X11 has no per-window buffer scale to
        // read, and every consumer of this number -- the canvas overlay widths, the reticle, the
        // RGBA cursor rasterization -- has only ever been exercised at the Wayland value. Deriving
        // it here (from Xft.dpi / RANDR / Fl::screen_scale) would switch all of those on for X11
        // users in one step, untested. Leave it until someone owns that HiDPI pass end to end.
        out.scale = 1;
    }

    out.pixelWidth = win->w() * out.scale;
    out.pixelHeight = win->h() * out.scale;
    return true;
}

} // namespace mosaic::platform
