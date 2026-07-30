#include "platform/wayland_subsurface.hpp"

#include "common/log.hpp"

// FLTK exposes the live wl_display / wl_compositor and a window's wl_surface; wayland-client
// gives us the registry + subcompositor + subsurface requests FLTK does not surface. There is
// no token clash between <X11/Xlib.h> (dragged in by FL/platform.H on the hybrid build) and
// wayland-client's headers, so the two can share this TU.
#include <FL/Fl_Window.H>
#include <FL/platform.H>
#include <FL/wayland.H> // fl_wl_display/_xid/_surface/_compositor/_buffer_scale
#include <cstdint>
#include <cstring>
#include <wayland-client.h>

namespace mosaic::platform {
namespace {

spdlog::logger& plog() {
    static const auto logger = common::log::category("platform");
    return *logger;
}

// Registry listener: capture the wl_subcompositor global. FLTK binds its own copy internally
// but does not expose it; binding a global more than once is allowed, so we bind a fresh handle.
void registryGlobal(void* data, wl_registry* reg, std::uint32_t name, const char* iface,
                    std::uint32_t /*version*/) {
    auto* out = static_cast<wl_subcompositor**>(data);
    if (std::strcmp(iface, wl_subcompositor_interface.name) == 0) {
        *out = static_cast<wl_subcompositor*>(
            wl_registry_bind(reg, name, &wl_subcompositor_interface, 1));
    }
}
void registryGlobalRemove(void*, wl_registry*, std::uint32_t) {}
const wl_registry_listener kRegistryListener = {registryGlobal, registryGlobalRemove};

// Bind wl_subcompositor on a *private* event queue so the roundtrip doesn't dispatch (and
// swallow) FLTK's own pending events. The bound proxy is handed back to the default queue.
wl_subcompositor* bindSubcompositor(wl_display* display) {
    wl_event_queue* queue = wl_display_create_queue(display);
    if (queue == nullptr)
        return nullptr;
    wl_registry* registry = wl_display_get_registry(display);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(registry), queue);
    wl_subcompositor* subcompositor = nullptr;
    wl_registry_add_listener(registry, &kRegistryListener, &subcompositor);
    wl_display_roundtrip_queue(display, queue); // process the global announcements
    if (subcompositor != nullptr) {
        // The private queue is about to die; move the proxy back to the default queue.
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(subcompositor), nullptr);
    }
    wl_registry_destroy(registry);
    wl_event_queue_destroy(queue);
    return subcompositor;
}

} // namespace

std::unique_ptr<WaylandSubsurface> WaylandSubsurface::create(Fl_Window* win, std::string& error) {
    wl_display* display = fl_wl_display();
    if (display == nullptr) {
        error = "not a Wayland session";
        return nullptr;
    }
    if (win == nullptr || win->shown() == 0) {
        error = "subsurface requested for a window that is not shown";
        return nullptr;
    }
    struct wld_window* xid = fl_wl_xid(win);
    wl_surface* parent = (xid != nullptr) ? fl_wl_surface(xid) : nullptr;
    wl_compositor* compositor = fl_wl_compositor();
    if (parent == nullptr || compositor == nullptr) {
        error = "no parent wl_surface / wl_compositor";
        return nullptr;
    }
    wl_subcompositor* subcompositor = bindSubcompositor(display);
    if (subcompositor == nullptr) {
        error = "wl_subcompositor unavailable";
        return nullptr;
    }

    auto self = std::unique_ptr<WaylandSubsurface>(new WaylandSubsurface());
    self->m_display = display;
    self->m_parent = parent;
    self->m_subcompositor = subcompositor; // owned now: the destructor frees it on any return

    self->m_surface = wl_compositor_create_surface(compositor);
    if (self->m_surface == nullptr) {
        error = "wl_compositor_create_surface failed";
        return nullptr;
    }
    self->m_subsurface = wl_subcompositor_get_subsurface(subcompositor, self->m_surface, parent);
    if (self->m_subsurface == nullptr) {
        error = "wl_subcompositor_get_subsurface failed";
        return nullptr;
    }

    int scale = fl_wl_buffer_scale(win);
    if (scale < 1)
        scale = 1;
    self->m_scale = scale;

    // The child fills the canvas from its top-left. Desync so the child presents on its own
    // (Vulkan) commits instead of waiting for a parent commit each frame.
    wl_subsurface_set_position(self->m_subsurface, 0, 0);
    wl_subsurface_set_desync(self->m_subsurface);
    wl_surface_set_buffer_scale(self->m_surface, scale);

    // Make the subsurface input-transparent: it is a pure presentation surface (Vulkan output), so
    // pointer/touch input must fall THROUGH to the parent FLTK surface, which routes it to the canvas
    // widget's handle(). Without an empty input region the subsurface's default (infinite) region
    // swallows every event over the canvas on native Wayland -- no zoom/pan/rotate -- while input
    // outside the canvas (on the FLTK surface) still works. Commit the child so it takes effect even
    // before the first Vulkan present (no buffer attached yet -> the surface simply stays unmapped).
    if (wl_region* empty = wl_compositor_create_region(compositor)) {
        wl_surface_set_input_region(self->m_surface, empty);
        wl_region_destroy(empty);
        wl_surface_commit(self->m_surface);
    }

    // A new subsurface's placement is parent-cached state: it only maps once the parent surface
    // is committed. FLTK isn't drawing this surface (Vulkan owns it), so commit it ourselves.
    // No buffer is attached here, so the parent keeps whatever buffer FLTK last gave it.
    wl_surface_commit(parent);
    wl_display_flush(display);

    plog().info("native Wayland: Vulkan subsurface created (buffer scale {})", scale);
    return self;
}

WaylandSubsurface::~WaylandSubsurface() {
    // Destroy our objects only; we never free the parent surface (FLTK owns it). We deliberately
    // do not commit the parent here -- by contract we are torn down while the parent is still
    // alive, and FLTK's next parent commit (or its window teardown) drops the removed subsurface.
    if (m_subsurface != nullptr)
        wl_subsurface_destroy(m_subsurface);
    if (m_surface != nullptr)
        wl_surface_destroy(m_surface);
    if (m_subcompositor != nullptr)
        wl_subcompositor_destroy(m_subcompositor);
}

void* WaylandSubsurface::surface() const noexcept {
    return m_surface;
}

void WaylandSubsurface::setBufferScale(int scale) noexcept {
    if (scale < 1)
        scale = 1;
    if (scale == m_scale || m_surface == nullptr)
        return;
    m_scale = scale;
    wl_surface_set_buffer_scale(m_surface, scale);
}

} // namespace mosaic::platform
