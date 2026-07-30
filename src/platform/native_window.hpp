#pragma once

#include <string>

class Fl_Window; // forward-declared: this header stays free of FLTK/X11/Wayland includes.

namespace mosaic::platform {

// Which windowing system a shown FLTK window is backed by. FLTK 1.4 on Linux is a hybrid
// build that chooses one of these at runtime (see docs/vulkan.md).
enum class WindowSystem {
    X11,
    Wayland,
    Cocoa, // macOS: `window` is a CAMetalLayer* attached to the FLTK window's NSView (S58)
    Win32, // Windows: `display` is the process HINSTANCE, `window` the window's HWND (S57)
};

// The raw native handles a Vulkan WSI surface is built from. Pointers are opaque here so
// that render/ can consume them without pulling in X11/Wayland/Windows headers; render/ casts
// them back to the concrete types when calling vkCreate{Xlib,Wayland,Win32}SurfaceKHR.
struct NativeSurfaceHandle {
    WindowSystem system = WindowSystem::X11;
    void* display = nullptr; // Display* (X11), wl_display* (Wayland) or HINSTANCE (Win32)
    void* window = nullptr;  // X11 Window id (as void*), wl_surface* (Wayland) or HWND (Win32)
    int pixelWidth = 0;      // best-effort drawable size hint, in pixels
    int pixelHeight = 0;     // (render/ prefers the live VkSurface currentExtent)
    int scale = 1;           // integer buffer scale (Wayland HiDPI); 1 otherwise
};

// If the user has not chosen a backend (FLTK_BACKEND unset) but a Wayland session is
// present, pin FLTK to its NATIVE WAYLAND backend (S59-a; this used to pin x11). The Vulkan
// canvas presents to its own `wl_subsurface` there and has been validation-clean since S11-c;
// going native also retires the X11 resize black-flash, drops the XWayland hop in front of the
// file-picker portal, and is a hard prerequisite for HDR output (S43). What the flip changes,
// and how to get back, is in docs/wayland.md. A **pure-Xorg** session (no WAYLAND_DISPLAY) is
// left alone and still lands on X11. Must be called before the first FLTK window is shown.
// A user-set FLTK_BACKEND always wins -- `FLTK_BACKEND=x11` is the supported escape hatch, and
// every X11 path in this file stays live behind it.
//
// A no-op on macOS and Windows, where FLTK is built against one backend and there is nothing to
// choose. It is still declared (and defined, emptily) there so its one caller -- main(), a line
// before the first window -- needs no platform conditional: whether the host offers a choice of
// backends is a fact about the host, and it belongs on this side of the boundary.
void preferWaylandBackendIfUnset();

// The backend FLTK actually initialized. Only meaningful after a window has been shown.
[[nodiscard]] WindowSystem activeBackend();

// The integer Wayland buffer scale of `win` -- the HiDPI factor its surface is composited at.
// **1 on X11, macOS and Windows**, none of which has such a per-window value (the same deliberate
// pin as NativeSurfaceHandle::scale below -- Windows' HiDPI is a fractional per-monitor DPI, which
// the app reads elsewhere, not an integer surface scale), and 1 for a null window. Safe before the
// window maps: FLTK answers from the outputs it knows about. Unlike nativeSurfaceHandle() this
// needs no shown window and allocates nothing, so a widget can ask it per pointer-motion event --
// which is what custom-cursor rasterization does (ui::chromeCursorScale).
[[nodiscard]] int windowBufferScale(Fl_Window* win);

// Fill `out` with the native handles for a *shown* window. Returns false / sets `error`
// if the window is not yet shown or no usable handle is available.
[[nodiscard]] bool nativeSurfaceHandle(Fl_Window* win, NativeSurfaceHandle& out,
                                       std::string& error);

} // namespace mosaic::platform
