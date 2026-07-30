#pragma once

#include <memory>
#include <string>

class Fl_Window; // forward-declared: this header stays free of FLTK/Wayland includes.

// Wayland client types, forward-declared so consumers need no <wayland-client.h>.
struct wl_display;
struct wl_surface;
struct wl_subsurface;
struct wl_subcompositor;

namespace mosaic::platform {

// A dedicated Wayland child surface for accelerated (Vulkan) content.
//
// Attaching a Vulkan swapchain straight onto an FLTK window's own wl_surface aborts the
// Wayland connection: FLTK already drives that surface (libdecor/Cairo) and Mesa's WSI then
// tries to create a second wp_linux_drm_syncobj_surface_v1 for it, which the protocol forbids
// ("Fatal error ... wp_linux_drm_syncobj_surface_v1"). The fix -- the same trick Fl_Gl_Window
// uses internally -- is to give the GPU its *own* wl_surface: a wl_subsurface stacked over the
// FLTK surface that nothing else touches. The Vulkan swapchain is built on surface().
//
// Teardown order matters: the owner must destroy the VkSurfaceKHR built on surface() *before*
// destroying this object (which destroys the child wl_surface), and both must be gone before
// FLTK destroys the parent window. See docs/vulkan.md.
class WaylandSubsurface {
public:
    // Create a child surface stacked over `win`'s wl_surface. `win` must be shown on the
    // Wayland backend. Returns nullptr / sets `error` on failure.
    static std::unique_ptr<WaylandSubsurface> create(Fl_Window* win, std::string& error);
    ~WaylandSubsurface();

    WaylandSubsurface(const WaylandSubsurface&) = delete;
    WaylandSubsurface& operator=(const WaylandSubsurface&) = delete;
    WaylandSubsurface(WaylandSubsurface&&) = delete;
    WaylandSubsurface& operator=(WaylandSubsurface&&) = delete;

    // The dedicated child wl_surface, as an opaque pointer (cast back to wl_surface*) so callers
    // stay free of Wayland headers. Feed it to vkCreateWaylandSurfaceKHR.
    [[nodiscard]] void* surface() const noexcept;

    // Re-apply the integer HiDPI buffer scale (call when it may have changed, e.g. on resize).
    void setBufferScale(int scale) noexcept;

private:
    WaylandSubsurface() = default;

    wl_display* m_display = nullptr;             // owned by FLTK
    wl_surface* m_parent = nullptr;              // FLTK's surface; not owned
    wl_subcompositor* m_subcompositor = nullptr; // owned (bound from the registry)
    wl_surface* m_surface = nullptr;             // our child surface; owned
    wl_subsurface* m_subsurface = nullptr;       // owned
    int m_scale = 1;
};

} // namespace mosaic::platform
