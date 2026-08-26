#include "platform/wayland_dialog.hpp"

#include "common/log.hpp"
#include "platform/native_window.hpp"    // nativeSurfaceHandle -> the window's wl_display
#include "platform/wayland_toplevel.hpp" // waylandToplevel() -- the patched-FLTK accessor

#include <cstring>
#include <string>
#include <wayland-client.h>
#include <xdg-dialog-v1-client.h>

namespace mosaic::platform {
namespace {

spdlog::logger& plog() {
    static const auto logger = common::log::category("platform");
    return *logger;
}

void registryGlobal(void* data, wl_registry* reg, std::uint32_t name, const char* iface,
                    std::uint32_t /*version*/) {
    auto* out = static_cast<xdg_wm_dialog_v1**>(data);
    if (std::strcmp(iface, xdg_wm_dialog_v1_interface.name) == 0) {
        *out = static_cast<xdg_wm_dialog_v1*>(
            wl_registry_bind(reg, name, &xdg_wm_dialog_v1_interface, 1));
    }
}
void registryGlobalRemove(void*, wl_registry*, std::uint32_t) {}
const wl_registry_listener kRegistryListener = {registryGlobal, registryGlobalRemove};

} // namespace

struct WaylandDialogHint::Impl {
    xdg_wm_dialog_v1* manager = nullptr;
    xdg_dialog_v1* dialog = nullptr;
};

WaylandDialogHint::WaylandDialogHint(Fl_Window* win, bool modal) {
    xdg_toplevel* toplevel = waylandToplevel(win);
    if (toplevel == nullptr)
        return; // X11, not shown, not a toplevel, or a stock FLTK -- all simply "no hint"

    NativeSurfaceHandle nh;
    std::string err;
    if (!nativeSurfaceHandle(win, nh, err) || nh.system != WindowSystem::Wayland ||
        nh.display == nullptr) {
        return;
    }
    auto* display = static_cast<wl_display*>(nh.display);

    // Bind on a PRIVATE event queue so the round-trip does not dispatch FLTK's own default-queue
    // events -- the wayland_foreign.cpp / wayland_subsurface.cpp discipline.
    wl_event_queue* queue = wl_display_create_queue(display);
    if (queue == nullptr)
        return;
    wl_registry* registry = wl_display_get_registry(display);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(registry), queue);
    xdg_wm_dialog_v1* manager = nullptr;
    wl_registry_add_listener(registry, &kRegistryListener, &manager);
    wl_display_roundtrip_queue(display, queue);
    wl_registry_destroy(registry);
    if (manager == nullptr) {
        wl_event_queue_destroy(queue);
        plog().debug("xdg-dialog-v1 not offered by the compositor; dialog hint skipped");
        return;
    }

    xdg_dialog_v1* dialog = xdg_wm_dialog_v1_get_xdg_dialog(manager, toplevel);
    // set_modal / unset_modal is double-buffered like the rest of xdg-shell state, and applies on
    // the toplevel's next commit -- which FLTK issues as it draws the freshly shown dialog.
    if (modal)
        xdg_dialog_v1_set_modal(dialog);
    else
        xdg_dialog_v1_unset_modal(dialog);

    // Hand both back to the default queue before the private one dies: a proxy on a destroyed queue
    // is a crash. They stay alive until this object does -- destroying the xdg_dialog_v1 is exactly
    // how the hint is withdrawn, so it must not happen early.
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(manager), nullptr);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(dialog), nullptr);
    wl_event_queue_destroy(queue);
    wl_display_flush(display);

    m_impl = std::make_unique<Impl>();
    m_impl->manager = manager;
    m_impl->dialog = dialog;
}

WaylandDialogHint::~WaylandDialogHint() {
    if (m_impl == nullptr)
        return;
    // Order matters: the dialog object refers to the toplevel, the manager made the dialog.
    if (m_impl->dialog != nullptr)
        xdg_dialog_v1_destroy(m_impl->dialog);
    if (m_impl->manager != nullptr)
        xdg_wm_dialog_v1_destroy(m_impl->manager);
}

bool WaylandDialogHint::active() const noexcept {
    return m_impl != nullptr && m_impl->dialog != nullptr;
}

} // namespace mosaic::platform
