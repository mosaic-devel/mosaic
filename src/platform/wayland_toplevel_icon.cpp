#include "platform/wayland_toplevel_icon.hpp"

#include "common/log.hpp"
#include "platform/native_window.hpp" // nativeSurfaceHandle -> the window's wl_display + wl_surface
#include "platform/wayland_toplevel.hpp" // waylandToplevel() -- the patched-FLTK accessor

// wayland-client gives us the registry, wl_shm and the generated xdg-toplevel-icon-v1 stubs
// (wayland-scanner; see platform/CMakeLists.txt). Same shape as wayland_foreign.cpp.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>
#include <xdg-toplevel-icon-v1-client.h>

namespace mosaic::platform {
namespace {

spdlog::logger& plog() {
    static const auto logger = common::log::category("platform");
    return *logger;
}

// The sizes always published, on top of whatever the compositor asks for. The protocol invites
// either behaviour -- "all icon sizes and scales that it can provide, OR which are explicitly
// requested" -- and supplying the union is the more robust half of that choice: KWin, measured,
// advertises exactly ONE size (96), so honouring only its request would leave a HiDPI task switcher
// or an overview grid with nothing else to pick from. Rasterizing an extra size from the SVG is
// cheap and happens once per process. 256 is the ceiling; beyond it we ship shm for nothing.
constexpr int kFallbackSizes[] = {16, 24, 32, 48, 64, 128, 256};

// A compositor is free to ask for anything, and every accepted size costs edge^2 * 4 bytes of shm
// that lives for the whole process -- so requests are clamped rather than trusted. The ceiling is
// the same 256 the baseline set stops at, and for the same reason: a larger icon buys nothing a
// scaled 256 does not. Eight is well above what a real compositor advertises (KWin sends one).
constexpr int kMinEdge = 8;
constexpr int kMaxEdge = 256;
constexpr std::size_t kMaxSizes = 8;

// ---- registry -----------------------------------------------------------------------------------

struct Globals {
    xdg_toplevel_icon_manager_v1* manager = nullptr;
    wl_shm* shm = nullptr;
};

void registryGlobal(void* data, wl_registry* reg, std::uint32_t name, const char* iface,
                    std::uint32_t /*version*/) {
    auto* out = static_cast<Globals*>(data);
    if (std::strcmp(iface, xdg_toplevel_icon_manager_v1_interface.name) == 0) {
        out->manager = static_cast<xdg_toplevel_icon_manager_v1*>(
            wl_registry_bind(reg, name, &xdg_toplevel_icon_manager_v1_interface, 1));
    } else if (std::strcmp(iface, wl_shm_interface.name) == 0) {
        out->shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
    }
}
void registryGlobalRemove(void*, wl_registry*, std::uint32_t) {}
const wl_registry_listener kRegistryListener = {registryGlobal, registryGlobalRemove};

// ---- the manager's size advertisement
// ------------------------------------------------------------

// The compositor may send a run of `icon_size` events after we bind, terminated by `done`. It may
// also send only `done` (no preference at all), which the protocol requires even then -- so one
// round-trip after binding is enough to know which case we are in.
struct SizePrefs {
    std::vector<int> sizes;
    bool done = false;
};

void managerIconSize(void* data, xdg_toplevel_icon_manager_v1*, std::int32_t size) {
    auto* prefs = static_cast<SizePrefs*>(data);
    if (size >= kMinEdge && size <= kMaxEdge && prefs->sizes.size() < kMaxSizes)
        prefs->sizes.push_back(static_cast<int>(size));
}
void managerDone(void* data, xdg_toplevel_icon_manager_v1*) {
    static_cast<SizePrefs*>(data)->done = true;
}
const xdg_toplevel_icon_manager_v1_listener kManagerListener = {managerIconSize, managerDone};

// ---- shm
// ------------------------------------------------------------------------------------------

// One anonymous, sealed memfd backs EVERY size: the pool is carved into per-size regions rather
// than opening a file descriptor per icon. F_SEAL_SHRINK is the seal that matters to a compositor
// -- it is what guarantees the mapping it makes cannot be pulled out from under it.
int createSealedMemfd(std::size_t bytes) {
    const int fd = memfd_create("mosaic-toplevel-icon", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        close(fd);
        return -1;
    }
    // Best-effort: a kernel that refuses the seal still gives a perfectly usable pool.
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK);
    return fd;
}

// wl_shm's ARGB8888 is 32-bit NATIVE-endian 0xAARRGGBB with PREMULTIPLIED alpha; our rasterizer
// hands back straight-alpha RGBA8 byte order. Both conversions happen here, in the one place that
// touches pixels, because getting either wrong shows up as a subtly dark or channel-swapped icon
// rather than as a failure.
void writeArgb8888(const common::Image& src, std::uint8_t* dst) {
    const std::size_t px = src.pixelCount();
    for (std::size_t i = 0; i < px; ++i) {
        const std::uint8_t r = src.rgba[i * 4 + 0];
        const std::uint8_t g = src.rgba[i * 4 + 1];
        const std::uint8_t b = src.rgba[i * 4 + 2];
        const std::uint8_t a = src.rgba[i * 4 + 3];
        // +127 rounds to nearest rather than truncating; over a 16px icon the difference is visible
        // as a dirty fringe on antialiased edges.
        const auto pm = [a](std::uint8_t c) -> std::uint32_t {
            return static_cast<std::uint32_t>((c * a + 127) / 255);
        };
        const std::uint32_t argb =
            (static_cast<std::uint32_t>(a) << 24) | (pm(r) << 16) | (pm(g) << 8) | pm(b);
        std::memcpy(dst + i * 4, &argb, sizeof argb);
    }
}

// ---- the shared icon --------------------------------------------------------------------------

// Built once and never torn down; see the header for why the buffers are deliberately immortal.
struct SharedIcon {
    bool attempted = false;
    xdg_toplevel_icon_manager_v1* manager = nullptr;
    xdg_toplevel_icon_v1* icon = nullptr;
};

SharedIcon& sharedIcon() {
    static SharedIcon instance;
    return instance;
}

// Bind the globals, ask the compositor which sizes it wants, rasterize them into one shm pool and
// assemble the icon object. Returns false (having logged once) if any step is unavailable.
bool buildSharedIcon(wl_display* display, const WaylandIconSpec& spec) {
    SharedIcon& shared = sharedIcon();

    // A PRIVATE event queue, so neither round-trip below dispatches -- and swallows -- FLTK's own
    // default-queue events. Same discipline as wayland_foreign.cpp / wayland_subsurface.cpp.
    wl_event_queue* queue = wl_display_create_queue(display);
    if (queue == nullptr)
        return false;
    wl_registry* registry = wl_display_get_registry(display);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(registry), queue);
    Globals globals;
    wl_registry_add_listener(registry, &kRegistryListener, &globals);
    wl_display_roundtrip_queue(display, queue); // process the global announcements
    wl_registry_destroy(registry);

    const auto giveUp = [&](const char* why) {
        if (globals.manager != nullptr)
            xdg_toplevel_icon_manager_v1_destroy(globals.manager);
        if (globals.shm != nullptr)
            wl_shm_destroy(globals.shm);
        wl_event_queue_destroy(queue);
        plog().debug("xdg-toplevel-icon: {}", why);
        return false;
    };

    if (globals.manager == nullptr) {
        // The common case on GNOME today (mutter#4100). Not a defect -- those sessions fall back to
        // the app_id -> .desktop -> Icon= chain, which works as soon as Mosaic is installed.
        return giveUp("not offered by this compositor; leaving the icon to the .desktop entry");
    }
    if (globals.shm == nullptr)
        return giveUp("compositor offers no wl_shm; cannot supply icon pixels");

    // Which sizes does it want? One round-trip carries the whole icon_size/done sequence.
    SizePrefs prefs;
    xdg_toplevel_icon_manager_v1_add_listener(globals.manager, &kManagerListener, &prefs);
    wl_display_roundtrip_queue(display, queue);
    // Union, not a fallback: the compositor's preferences are added to the baseline set rather than
    // replacing it. sort+unique makes a compositor that names a size we already ship a no-op.
    prefs.sizes.insert(prefs.sizes.end(), std::begin(kFallbackSizes), std::end(kFallbackSizes));
    std::sort(prefs.sizes.begin(), prefs.sizes.end());
    prefs.sizes.erase(std::unique(prefs.sizes.begin(), prefs.sizes.end()), prefs.sizes.end());

    // Rasterize first, so the pool can be sized exactly and a failed render simply drops that size.
    struct Rendered {
        int edge = 0;
        common::Image img;
    };
    std::vector<Rendered> rendered;
    std::size_t total = 0;
    for (const int edge : prefs.sizes) {
        common::Image img = spec.raster ? spec.raster(edge) : common::Image{};
        if (img.empty() || img.width != img.height ||
            img.width != static_cast<std::uint32_t>(edge)) {
            continue; // the protocol requires a SQUARE buffer of exactly this edge; skip otherwise
        }
        total += img.rgba.size();
        rendered.push_back({edge, std::move(img)});
    }
    if (rendered.empty())
        return giveUp("no icon size could be rasterized");

    const int fd = createSealedMemfd(total);
    if (fd < 0)
        return giveUp("could not create the shm backing file");
    auto* base =
        static_cast<std::uint8_t*>(mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (base == MAP_FAILED) {
        close(fd);
        return giveUp("could not map the shm backing file");
    }
    std::size_t offset = 0;
    for (const auto& r : rendered) {
        writeArgb8888(r.img, base + offset);
        offset += r.img.rgba.size();
    }

    wl_shm_pool* pool = wl_shm_create_pool(globals.shm, fd, static_cast<std::int32_t>(total));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(pool), queue);
    xdg_toplevel_icon_v1* icon = xdg_toplevel_icon_manager_v1_create_icon(globals.manager);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(icon), queue);

    // The icon-theme name costs nothing and is the path a compositor prefers when it CAN resolve it
    // (it then picks the size itself, from the installed theme). The buffers below are the fallback
    // the protocol mandates for exactly our case: an uninstalled binary, whose name resolves to
    // nothing. Both are set; the compositor chooses.
    if (!spec.name.empty())
        xdg_toplevel_icon_v1_set_name(icon, spec.name.c_str());

    offset = 0;
    std::vector<wl_buffer*> buffers; // kept only to move them off the private queue below
    for (const auto& r : rendered) {
        const auto stride = static_cast<std::int32_t>(r.edge * 4);
        wl_buffer* buffer =
            wl_shm_pool_create_buffer(pool, static_cast<std::int32_t>(offset), r.edge, r.edge,
                                      stride, WL_SHM_FORMAT_ARGB8888);
        offset += r.img.rgba.size();
        if (buffer == nullptr)
            continue;
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(buffer), queue);
        // Scale 1: these are pixel sizes we chose to match the compositor's request in
        // SURFACE-local units. Publishing the same image again at scale 2 would claim a HiDPI
        // variant we do not have; a compositor that wants 64@2 asked for an icon_size of 128
        // instead.
        xdg_toplevel_icon_v1_add_buffer(icon, buffer, 1);
        buffers.push_back(buffer);
        // Deliberately never DESTROYED: the protocol requires every wl_buffer to outlive the icon
        // referencing it (a 'no_buffer' protocol error otherwise, which kills the connection), and
        // this icon lives until the process exits. They are still tracked, because a proxy that
        // outlives the queue it sits on has to be moved off it first -- see the cleanup below.
    }

    wl_shm_pool_destroy(pool); // the buffers carved from it stay valid; the pool itself need not
    munmap(base, total);       // the compositor holds its own mapping of the sealed fd
    close(fd);

    if (buffers.empty()) {
        xdg_toplevel_icon_v1_destroy(icon);
        return giveUp("no icon buffer could be created");
    }

    // ---- cleanup, and the ORDER is the whole point
    // ------------------------------------------------ Everything bound off the registry inherited
    // the private queue (a proxy is created on its parent's queue), and that queue is about to be
    // destroyed. So every proxy that OUTLIVES it must first be moved back to the default queue, and
    // every proxy that does not must be destroyed BEFORE it. A proxy left pointing at a freed queue
    // is a use-after-free the next time libwayland touches it -- and for the buffers that is not
    // hypothetical: they live for the whole process.
    //
    // Survivors: the manager (set_icon is issued on it), the icon, and every wl_buffer.
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(globals.manager), nullptr);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(icon), nullptr);
    for (wl_buffer* buffer : buffers)
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(buffer), nullptr);
    // Not a survivor: wl_shm is finished with once the pool exists, so it goes first -- while its
    // queue is still alive.
    if (globals.shm != nullptr)
        wl_shm_destroy(globals.shm);
    wl_event_queue_destroy(queue);

    shared.manager = globals.manager;
    shared.icon = icon;
    plog().info("xdg-toplevel-icon: published {} icon size(s) to the compositor", buffers.size());
    return true;
}

} // namespace

bool applyWaylandToplevelIcon(Fl_Window* win, const WaylandIconSpec& spec) {
    xdg_toplevel* toplevel = waylandToplevel(win);
    if (toplevel == nullptr) {
        // Distinguish "your build cannot do this" from "your window is not eligible", once -- the
        // two look identical from the outside and lead to completely different bug reports.
        static bool warned = false;
        if (!warned && !waylandToplevelAccessorPresent() &&
            activeBackend() == WindowSystem::Wayland) {
            warned = true;
            plog().info("xdg-toplevel-icon: this FLTK exposes no xdg_toplevel accessor; window "
                        "icons come from the installed .desktop entry only (docs/wayland.md §4)");
        }
        return false;
    }

    NativeSurfaceHandle nh;
    std::string err;
    if (!nativeSurfaceHandle(win, nh, err) || nh.system != WindowSystem::Wayland ||
        nh.display == nullptr || nh.window == nullptr) {
        return false;
    }
    auto* display = static_cast<wl_display*>(nh.display);

    SharedIcon& shared = sharedIcon();
    if (!shared.attempted) {
        shared.attempted = true; // one attempt per process, success or not: nothing changes later
        buildSharedIcon(display, spec);
    }
    if (shared.icon == nullptr || shared.manager == nullptr)
        return false;

    // Immutable from here on, and reusable: the protocol lets one icon object be set on any number
    // of toplevels, it only forbids CHANGING it afterwards. That is why every window shares this
    // one.
    xdg_toplevel_icon_manager_v1_set_icon(shared.manager, toplevel, shared.icon);

    // set_icon is double-buffered and takes effect on the toplevel's next wl_surface.commit. FLTK
    // will commit on its next draw, but a window that opens and then sits idle would wear a generic
    // icon until something happened to repaint it -- so commit now and push it out. A bare commit
    // re-applies the surface's current state, which is a no-op beyond the icon itself.
    wl_surface_commit(static_cast<wl_surface*>(nh.window));
    wl_display_flush(display);
    return true;
}

} // namespace mosaic::platform
