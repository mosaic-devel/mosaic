#include "platform/wayland_foreign.hpp"

#include "common/log.hpp"
#include "platform/native_window.hpp" // nativeSurfaceHandle -> the window's wl_display + wl_surface

// FLTK exposes the live wl_display and a window's wl_surface (via native_window); wayland-client
// gives us the registry + zxdg_exporter_v2 requests FLTK does not surface. The generated
// xdg-foreign-unstable-v2 client stubs come from wayland-scanner (see platform/CMakeLists.txt). As
// in wayland_subsurface.cpp, X11 headers (dragged in by the hybrid FLTK build elsewhere) and
// wayland-client's headers do not clash, so this TU is Wayland-only and clean.
#include <wayland-client.h>
#include <xdg-foreign-unstable-v2-client.h>

#include <cstring>
#include <string>

namespace mosaic::platform {
namespace {

spdlog::logger& plog() {
    static const auto logger = common::log::category("platform");
    return *logger;
}

// Registry listener: capture the zxdg_exporter_v2 global. Binding it ourselves (FLTK does not) on a
// private queue keeps the round-trip from dispatching FLTK's own pending events.
void registryGlobal(void* data, wl_registry* reg, std::uint32_t name, const char* iface,
                    std::uint32_t /*version*/) {
    auto* out = static_cast<zxdg_exporter_v2**>(data);
    if (std::strcmp(iface, zxdg_exporter_v2_interface.name) == 0) {
        *out = static_cast<zxdg_exporter_v2*>(
            wl_registry_bind(reg, name, &zxdg_exporter_v2_interface, 1));
    }
}
void registryGlobalRemove(void*, wl_registry*, std::uint32_t) {}
const wl_registry_listener kRegistryListener = {registryGlobal, registryGlobalRemove};

// The exported object's one event: the string handle the compositor minted for our surface.
void exportedHandle(void* data, zxdg_exported_v2*, const char* handle) {
    if (handle != nullptr)
        *static_cast<std::string*>(data) = handle;
}
const zxdg_exported_v2_listener kExportedListener = {exportedHandle};

} // namespace

// The live proxies the handle stays valid against -- destroyed in the dtor, after the dialog closes.
struct WaylandForeignExport::Impl {
    zxdg_exporter_v2* exporter = nullptr;
    zxdg_exported_v2* exported = nullptr;
};

WaylandForeignExport::WaylandForeignExport(Fl_Window* win) {
    NativeSurfaceHandle nh;
    std::string err;
    if (!nativeSurfaceHandle(win, nh, err) || nh.system != WindowSystem::Wayland ||
        nh.display == nullptr || nh.window == nullptr) {
        return; // X11 (the caller uses x11:<xid>) or the window is not shown yet -> empty handle
    }
    auto* display = static_cast<wl_display*>(nh.display);
    auto* surface = static_cast<wl_surface*>(nh.window);

    // Bind zxdg_exporter_v2 on a PRIVATE event queue so neither round-trip below dispatches (and
    // swallows) FLTK's own default-queue events -- the wayland_subsurface.cpp discipline.
    wl_event_queue* queue = wl_display_create_queue(display);
    if (queue == nullptr)
        return;
    wl_registry* registry = wl_display_get_registry(display);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(registry), queue);
    zxdg_exporter_v2* exporter = nullptr;
    wl_registry_add_listener(registry, &kRegistryListener, &exporter);
    wl_display_roundtrip_queue(display, queue); // process the global announcements
    wl_registry_destroy(registry);
    if (exporter == nullptr) {
        wl_event_queue_destroy(queue); // compositor has no xdg_foreign -> fall back to no parenting
        plog().debug("xdg_foreign (zxdg_exporter_v2) not offered by the compositor");
        return;
    }

    // Export the toplevel surface and wait one round-trip for the compositor's `handle` event. The
    // exported proxy inherits the private queue (via the exporter), so the event dispatches here.
    zxdg_exported_v2* exported = zxdg_exporter_v2_export_toplevel(exporter, surface);
    std::string handle;
    zxdg_exported_v2_add_listener(exported, &kExportedListener, &handle);
    wl_display_roundtrip_queue(display, queue);

    // The survivors must outlive this private queue: hand them back to the default queue before it
    // dies (a proxy on a destroyed queue is a crash). They receive no further events, but must stay
    // ALIVE -- destroying `exported` would revoke the handle mid-dialog.
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(exporter), nullptr);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(exported), nullptr);
    wl_event_queue_destroy(queue);

    if (handle.empty()) {
        zxdg_exported_v2_destroy(exported);
        zxdg_exporter_v2_destroy(exporter);
        return; // no handle came back -> empty (unparented, but no worse than before)
    }
    m_impl = std::make_unique<Impl>();
    m_impl->exporter = exporter;
    m_impl->exported = exported;
    m_handle = "wayland:" + handle;
}

WaylandForeignExport::~WaylandForeignExport() {
    if (m_impl == nullptr)
        return;
    if (m_impl->exported != nullptr)
        zxdg_exported_v2_destroy(m_impl->exported);
    if (m_impl->exporter != nullptr)
        zxdg_exporter_v2_destroy(m_impl->exporter);
}

} // namespace mosaic::platform
