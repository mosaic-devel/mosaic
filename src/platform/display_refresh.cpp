#include "platform/display_refresh.hpp"

#include "common/log.hpp"

// Linux speaks both display servers, chosen at runtime exactly as native_window.cpp does: the
// wl_display probe is the guard, and the X11 half stays live behind FLTK_BACKEND=x11. There is no
// token clash between <X11/Xlib.h> (dragged in by FL/platform.H on the hybrid build) and
// wayland-client's headers, so the two backends can share this TU (the wayland_subsurface.cpp
// finding).
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H> // fl_x11_display(), fl_x11_xid()
#include <FL/wayland.H>  // fl_wl_display()

#include <X11/extensions/Xrandr.h>
#include <wayland-client.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace mosaic::platform {
namespace {

spdlog::logger& plog() {
    static const auto logger = common::log::category("platform");
    return *logger;
}

// ---- Wayland ---------------------------------------------------------------------------------
//
// FLTK binds every wl_output itself, but its `mode` handler keeps only width/height and THROWS THE
// REFRESH AWAY (Fl_Wayland_Screen_Driver.cxx: output_mode stores width, height and nothing else),
// so there is nothing to read back out of it. We therefore bind our own wl_output proxies for the
// same globals. Binding a global twice is ordinary Wayland -- wayland_subsurface.cpp already does
// it for wl_subcompositor -- and it is the only way to get a listener onto an interface FLTK is
// already listening to, because a wl_proxy has room for exactly one.

struct WlOutput {
    std::uint32_t name = 0;   // registry name, so global_remove can find it again
    wl_output* proxy = nullptr;
    int x = 0, y = 0;         // position in the global compositor space, as `geometry` reports it
    int modeW = 0, modeH = 0; // the CURRENT mode, in the panel's own pixels
    int scale = 1;            // wl_output.scale (the HiDPI factor FLTK calls wld_scale)
    int refreshMHz = 0;       // the CURRENT mode's vertical refresh, in MILLIhertz
};

// ⚠ The entries are heap-stable on purpose: each proxy's listener carries a raw WlOutput* as its
// user data, and a std::vector<WlOutput> would dangle every one of them on its first reallocation.
struct WlOutputs {
    wl_registry* registry = nullptr; // kept ALIVE: it is what delivers monitor hotplug
    std::vector<std::unique_ptr<WlOutput>> list;
};

WlOutputs& wlOutputs() {
    // Never torn down. The proxies must outlive every caller and would otherwise have to be
    // destroyed against a wl_display FLTK has already closed; a handful of protocol objects held to
    // process exit is the cheaper end of that trade (the wayland_foreign.cpp discipline).
    static WlOutputs state;
    return state;
}

void outputGeometry(void* data, wl_output*, std::int32_t x, std::int32_t y, std::int32_t,
                    std::int32_t, std::int32_t, const char*, const char*, std::int32_t) {
    auto* out = static_cast<WlOutput*>(data);
    out->x = static_cast<int>(x);
    out->y = static_cast<int>(y);
}

void outputMode(void* data, wl_output*, std::uint32_t flags, std::int32_t w, std::int32_t h,
                std::int32_t refresh) {
    // ⚠ The compositor sends EVERY mode the panel supports, not just the active one, and the 60 Hz
    // entry of a 200 Hz monitor is in that list. Only the one flagged CURRENT is the cadence we may
    // pace on -- without this test the answer depends on which mode happened to arrive last.
    if ((flags & WL_OUTPUT_MODE_CURRENT) == 0)
        return;
    auto* out = static_cast<WlOutput*>(data);
    out->modeW = static_cast<int>(w);
    out->modeH = static_cast<int>(h);
    out->refreshMHz = static_cast<int>(refresh);
}

void outputDone(void*, wl_output*) {
    // The end of one atomic batch of the four events above. Nothing to do: the fields are read
    // lazily by displayRefreshHz(), so a half-applied batch is at worst one stale answer, and there
    // is no derived state here to recompute (which is what FLTK's own output_done exists for).
}

void outputScale(void* data, wl_output*, std::int32_t factor) {
    if (factor > 0)
        static_cast<WlOutput*>(data)->scale = static_cast<int>(factor);
}

// Assembled member-by-member from a zero-initialized struct rather than written as an aggregate
// literal: wl_output has grown events (`name`, `description` at version 4) and will grow more, so a
// brace list naming four of six is a warning away from a hard error on the next libwayland. The
// zeroed tail is also the SAFE tail -- libwayland calls a listener slot unconditionally, so an
// event we have no handler for must be one we never bound a high enough version to receive.
const wl_output_listener& outputListener() {
    static const wl_output_listener listener = [] {
        wl_output_listener l{};
        l.geometry = outputGeometry;
        l.mode = outputMode;
        l.done = outputDone;
        l.scale = outputScale;
        return l;
    }();
    return listener;
}

void registryGlobal(void*, wl_registry* reg, std::uint32_t name, const char* iface,
                    std::uint32_t version) {
    if (std::strcmp(iface, wl_output_interface.name) != 0)
        return;
    // Version 2 is what we need and all we want: `mode` (which carries the refresh) is version 1,
    // `scale` is version 2, and asking for 4 would start the `name`/`description` events our
    // listener deliberately has no slot for (see outputListener).
    const std::uint32_t bind = version < 2u ? version : 2u;
    auto* proxy = static_cast<wl_output*>(wl_registry_bind(reg, name, &wl_output_interface, bind));
    if (proxy == nullptr)
        return;
    WlOutputs& state = wlOutputs();
    state.list.push_back(std::make_unique<WlOutput>());
    WlOutput* entry = state.list.back().get();
    entry->name = name;
    entry->proxy = proxy;
    wl_output_add_listener(proxy, &outputListener(), entry);
}

void registryGlobalRemove(void*, wl_registry*, std::uint32_t name) {
    WlOutputs& state = wlOutputs();
    for (auto it = state.list.begin(); it != state.list.end(); ++it) {
        if ((*it)->name != name)
            continue;
        wl_output_destroy((*it)->proxy); // v2 binding: _destroy, not the v3 _release
        state.list.erase(it);
        return;
    }
}

const wl_registry_listener kRegistryListener = {registryGlobal, registryGlobalRemove};

// Bind the output table once. After this the table maintains ITSELF: the proxies live on FLTK's
// default queue, so every later `mode`, `scale` and hotplug announcement is delivered by FLTK's own
// event loop into the handlers above -- which is what makes asking for the refresh rate four times
// a second cost no round-trip at all.
void ensureWaylandOutputs(wl_display* display) {
    WlOutputs& state = wlOutputs();
    if (state.registry != nullptr)
        return;
    // Bind on a PRIVATE event queue so the two round-trips below cannot dispatch (and swallow)
    // FLTK's own default-queue events -- the wayland_subsurface.cpp discipline.
    wl_event_queue* queue = wl_display_create_queue(display);
    if (queue == nullptr)
        return;
    wl_registry* registry = wl_display_get_registry(display);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(registry), queue);
    wl_registry_add_listener(registry, &kRegistryListener, nullptr);
    state.registry = registry;
    wl_display_roundtrip_queue(display, queue); // the global announcements -> registryGlobal
    // ⚠ A SECOND round-trip. The wl_output proxies were created during the first one, so their
    // geometry/mode/scale/done burst was still in flight when it returned; one round-trip only ever
    // guarantees the events of requests made BEFORE it.
    wl_display_roundtrip_queue(display, queue);

    // Hand everything to the default queue before the private one dies (a proxy on a destroyed
    // queue is a crash), which is also what puts the table under FLTK's event loop from here on.
    // Outputs bound LATER inherit the registry's queue and therefore land here automatically.
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(registry), nullptr);
    for (const std::unique_ptr<WlOutput>& out : state.list)
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(out->proxy), nullptr);
    wl_event_queue_destroy(queue);
    plog().debug("display refresh: bound {} wl_output(s)", state.list.size());
}

double waylandRefreshHz(Fl_Window* win, wl_display* display) {
    ensureWaylandOutputs(display);
    const WlOutputs& state = wlOutputs();
    if (state.list.empty())
        return 0.0;
    if (state.list.size() == 1)
        return state.list.front()->refreshMHz / 1000.0; // one panel: no association to work out

    // WHICH panel is the window on? wl_surface.enter answers exactly that, and FLTK already listens
    // for it -- Fl_Window::screen_num() is maintained from the surface's output list (its Wayland
    // driver's surface_enter -> change_scale). ⚠ Worth stating plainly, because the present-wait
    // pacing this replaces was built on the belief that Wayland cannot tell us: what Wayland
    // withholds is the window's POSITION, not which output is showing it.
    const int screen = win->screen_num();
    int sx = 0, sy = 0, sw = 0, sh = 0;
    Fl::screen_xywh(sx, sy, sw, sh, screen);
    const double gui = Fl::screen_scale(screen) > 0.0f ? Fl::screen_scale(screen) : 1.0f;

    // Match on geometry rather than on list position: FLTK's screen order is the order its own
    // registry happened to bind the outputs in (and it inserts at the head, so it is not even ours
    // reversed). Its arithmetic is invertible, though -- screen_xywh divides the output's origin by
    // the GUI scale and its mode size by the GUI scale times the buffer scale -- so multiplying
    // back lands in the same space the `geometry` and `mode` events speak. Nearest-wins rather than
    // exact-match: a rounding disagreement must not lose the association, and with two panels the
    // right one wins by hundreds of pixels.
    const int wantX = static_cast<int>(std::lround(sx * gui));
    const int wantY = static_cast<int>(std::lround(sy * gui));
    const WlOutput* best = nullptr;
    long bestScore = 0;
    for (const std::unique_ptr<WlOutput>& out : state.list) {
        const int wantW = static_cast<int>(std::lround(sw * gui * out->scale));
        const int wantH = static_cast<int>(std::lround(sh * gui * out->scale));
        const long score = std::labs(out->x - wantX) + std::labs(out->y - wantY) +
                           std::labs(out->modeW - wantW) + std::labs(out->modeH - wantH);
        // A tie is a mirrored pair (two outputs at one origin, one size, different rates). Take the
        // SLOWER: presenting at the faster panel's rate would be exactly the waste we are here to
        // stop, and the user sees both.
        if (best == nullptr || score < bestScore ||
            (score == bestScore && out->refreshMHz < best->refreshMHz)) {
            best = out.get();
            bestScore = score;
        }
    }
    return best != nullptr ? best->refreshMHz / 1000.0 : 0.0;
}

// ---- X11 (RandR) -----------------------------------------------------------------------------

// The vertical refresh a RandR mode line actually produces. The formula is xrandr's own: the pixel
// clock divided by the pixels in one whole frame, borders and blanking included.
double modeRefreshHz(const XRRModeInfo& mode) {
    if (mode.hTotal == 0 || mode.vTotal == 0 || mode.dotClock == 0)
        return 0.0;
    double vTotal = mode.vTotal;
    if ((mode.modeFlags & RR_DoubleScan) != 0)
        vTotal *= 2.0; // every line scanned twice: half the frames
    if ((mode.modeFlags & RR_Interlace) != 0)
        vTotal /= 2.0; // half the lines per field: twice the fields
    return static_cast<double>(mode.dotClock) / (static_cast<double>(mode.hTotal) * vTotal);
}

double x11RefreshHz(Fl_Window* win) {
    Display* display = fl_x11_display();
    const Window xid = fl_x11_xid(win);
    if (display == nullptr || xid == 0)
        return 0.0;
    int eventBase = 0, errorBase = 0;
    if (XRRQueryExtension(display, &eventBase, &errorBase) == 0)
        return 0.0; // an X server without RandR: no mode to read, so the caller assumes one

    // The window's rectangle in the SERVER's pixel space -- the space RandR's CRTC rectangles live
    // in. Deliberately not Fl_Window::x()/y()/w()/h(): those are logical coordinates divided by
    // Fl::screen_scale, and on a scaled X11 desktop they are not comparable with a CRTC rect.
    Window root = 0;
    int ignoredX = 0, ignoredY = 0;
    unsigned int winW = 0, winH = 0, border = 0, depth = 0;
    if (XGetGeometry(display, xid, &root, &ignoredX, &ignoredY, &winW, &winH, &border, &depth) == 0)
        return 0.0;
    int winX = 0, winY = 0;
    Window child = 0;
    if (XTranslateCoordinates(display, xid, root, 0, 0, &winX, &winY, &child) == 0)
        return 0.0;

    // Not XRRGetScreenResources: that one asks the DRIVER to re-probe every connector, which costs
    // milliseconds and can even flicker a link. The `Current` variant answers from the server's
    // cached configuration, which is what we want -- we are asking what the mode IS, not what the
    // hardware could do.
    XRRScreenResources* res = XRRGetScreenResourcesCurrent(display, root);
    if (res == nullptr)
        return 0.0;
    double bestHz = 0.0;
    long bestOverlap = -1;
    for (int i = 0; i < res->ncrtc; ++i) {
        XRRCrtcInfo* crtc = XRRGetCrtcInfo(display, res, res->crtcs[i]);
        if (crtc == nullptr)
            continue;
        if (crtc->mode != 0 && crtc->noutput > 0) { // 0 == RandR's None: a CRTC driving nothing
            const long ox = std::min<long>(winX + winW, crtc->x + crtc->width) -
                            std::max<long>(winX, crtc->x);
            const long oy = std::min<long>(winY + winH, crtc->y + crtc->height) -
                            std::max<long>(winY, crtc->y);
            // Largest overlap wins, so a window straddling two panels is paced by the one showing
            // most of it. A window entirely off every CRTC (mid-drag, or on a workspace nothing is
            // scanning out) yields overlap <= 0 everywhere and falls through to the first CRTC,
            // which is a rate rather than nothing.
            const long overlap = (ox > 0 && oy > 0) ? ox * oy : 0;
            if (overlap > bestOverlap) {
                for (int m = 0; m < res->nmode; ++m) {
                    if (res->modes[m].id == crtc->mode) {
                        const double hz = modeRefreshHz(res->modes[m]);
                        if (hz > 0.0) {
                            bestOverlap = overlap;
                            bestHz = hz;
                        }
                        break;
                    }
                }
            }
        }
        XRRFreeCrtcInfo(crtc);
    }
    XRRFreeScreenResources(res);
    return bestHz;
}

} // namespace

double displayRefreshHz(Fl_Window* win) {
    if (win == nullptr || win->shown() == 0)
        return 0.0;
    if (wl_display* display = fl_wl_display())
        return waylandRefreshHz(win, display);
    return x11RefreshHz(win);
}

} // namespace mosaic::platform
