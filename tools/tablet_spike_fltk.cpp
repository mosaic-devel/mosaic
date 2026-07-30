// S19 Arc C — stage 2 of the Wayland tablet spike (docs/tablet.md §4): the same questions,
// but with FLTK OWNING the display connection and event loop, and the REAL shipped
// WaylandSubsurface as the child. Proves what the standalone client cannot:
//
//   Q1' zwp_tablet_* proxies bound on fl_wl_display() and left on the DEFAULT queue are
//       dispatched from inside Fl::wait() — the integration model the real backend will use.
//   Q2' proximity_in's surface IS fl_wl_surface(fl_wl_xid(win)) (pointer identity), and pen
//       input over the real WaylandSubsurface (empty input region, buffer attached to stand
//       in for Vulkan content) falls through to the FLTK surface.
//   Q4  Does the compositor STILL emulate wl_pointer events for the pen once this client is
//       tablet-aware? docs/tablet.md §3.1 drives the stroke lifecycle from FL_PUSH/FL_DRAG;
//       if binding the tablet manager suppresses pointer emulation (KWin emulates only for
//       non-tablet-aware clients), the Wayland backend must own the lifecycle itself from
//       tool down/up. This is the load-bearing answer for the backend design.
//
// Positioning: unlike stage 1 there is no fullscreen — a Wayland client cannot know its
// window's global position, so the pen hover-sweeps rows until proximity lands on the FLTK
// surface, then steers with a proportional controller using the surface-local coordinates
// the compositor reports back. Tip-down only happens well inside our own window.
//
// Exit codes: 0 = questions answered; 3 = tablet never announced; 4 = no uinput;
// 5 = pen never found the window / timeout; 6 = environment (not Wayland / no manager).

#include "virtual_pen.hpp"

#include "platform/wayland_subsurface.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/platform.H>
#include <FL/wayland.H>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "tablet-unstable-v2-client.h"

namespace {

using mosaic::spike::VirtualPen;

constexpr int kWinW = 800, kWinH = 600;
constexpr int kSubW = 400, kSubH = 300; // the stand-in "Vulkan canvas", at (0,0) like the real one
constexpr double kTick = 0.01;          // Fl::add_timeout cadence, 100 Hz

enum class St { WaitTablet, Settle, RowStart, Sweep, SteerCentre, Stroke, Wrap, Done };

struct Spike {
    Fl_Double_Window* win = nullptr;
    wl_surface* parentSurface = nullptr; // fl_wl_surface(fl_wl_xid(win))
    wl_surface* subSurface = nullptr;    // WaylandSubsurface::surface()

    wl_seat* seat = nullptr;
    wl_shm* shm = nullptr;
    zwp_tablet_manager_v2* manager = nullptr;
    zwp_tablet_seat_v2* tabletSeat = nullptr;

    VirtualPen pen;
    St st = St::WaitTablet;
    long tick = 0, stTick = 0;
    int row = 0;
    double steerX = 0, steerY = 0; // steer target, parent-surface-local
    St afterSteer = St::Done;
    int steerSettled = 0;

    // Tablet evidence
    bool tabletAdded = false, toolDone = false;
    wl_surface* proxSurface = nullptr;
    int proxParent = 0, proxSub = 0, proxOther = 0;
    int motions = 0, pressures = 0;
    std::uint32_t maxPressure = 0;
    bool downSeen = false, upSeen = false;
    double lastSx = -1, lastSy = -1;
    long lastMotionTick = -1;
    unsigned subDwellMask = 0; // 1=parent, 2=subsurface, 4=other

    // Q4 evidence: what FLTK's widget path sees while the pen is active
    int flEnter = 0, flMove = 0, flPush = 0, flDrag = 0, flRelease = 0, flLeave = 0;

    bool running = true;
    int exitCode = 0;
};

Spike g;

void say(const char* fmt, ...) {
    std::printf("%8.0fms | ", g.tick * 1000.0 * kTick);
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

const char* label(wl_surface* s) {
    if (s == g.parentSurface)
        return "FLTK-PARENT";
    if (s == g.subSurface)
        return "VULKAN-SUBSURFACE";
    return s == nullptr ? "(none)" : "(other)";
}

// A window that tallies what FLTK's event path delivers (the Q4 evidence).
class CountingWindow : public Fl_Double_Window {
public:
    CountingWindow() : Fl_Double_Window(kWinW, kWinH, "Mosaic tablet spike (FLTK)") {
        color(FL_DARK3);
    }
    int handle(int ev) override {
        switch (ev) {
        case FL_ENTER: ++g.flEnter; return 1;
        case FL_LEAVE: ++g.flLeave; return 1;
        case FL_MOVE:
            if (++g.flMove <= 3)
                say("FLTK saw FL_MOVE (%d, %d)", Fl::event_x(), Fl::event_y());
            return 1;
        case FL_PUSH:
            ++g.flPush;
            say("FLTK saw FL_PUSH (%d, %d)", Fl::event_x(), Fl::event_y());
            return 1;
        case FL_DRAG: ++g.flDrag; return 1;
        case FL_RELEASE:
            ++g.flRelease;
            say("FLTK saw FL_RELEASE");
            return 1;
        default: return Fl_Double_Window::handle(ev);
        }
    }
};

// ---------------------------------------------------------------- tablet listeners

void toolType(void*, zwp_tablet_tool_v2*, std::uint32_t t) { say("tool: type=%u", t); }
void toolHwSerial(void*, zwp_tablet_tool_v2*, std::uint32_t, std::uint32_t) {}
void toolHwWacom(void*, zwp_tablet_tool_v2*, std::uint32_t, std::uint32_t) {}
void toolCapability(void*, zwp_tablet_tool_v2*, std::uint32_t cap) {
    say("tool: capability=%u", cap);
}
void toolDoneEv(void*, zwp_tablet_tool_v2*) { g.toolDone = true; }
void toolRemoved(void*, zwp_tablet_tool_v2* t) {
    zwp_tablet_tool_v2_destroy(t);
}
void toolProximityIn(void*, zwp_tablet_tool_v2*, std::uint32_t, zwp_tablet_v2*,
                     wl_surface* surface) {
    g.proxSurface = surface;
    if (surface == g.parentSurface)
        ++g.proxParent;
    else if (surface == g.subSurface)
        ++g.proxSub;
    else
        ++g.proxOther;
    say("proximity_in surface=%s (%p)", label(surface), static_cast<void*>(surface));
}
void toolProximityOut(void*, zwp_tablet_tool_v2*) { g.proxSurface = nullptr; }
void toolDown(void*, zwp_tablet_tool_v2*, std::uint32_t) {
    g.downSeen = true;
    say("tablet down on %s", label(g.proxSurface));
}
void toolUp(void*, zwp_tablet_tool_v2*) {
    g.upSeen = true;
    say("tablet up on %s", label(g.proxSurface));
}
void toolMotion(void*, zwp_tablet_tool_v2*, wl_fixed_t fx, wl_fixed_t fy) {
    ++g.motions;
    if (g.proxSurface == g.parentSurface) {
        g.lastSx = wl_fixed_to_double(fx);
        g.lastSy = wl_fixed_to_double(fy);
        g.lastMotionTick = g.tick;
    }
    if (g.motions <= 3)
        say("motion (%.4f, %.4f) on %s", wl_fixed_to_double(fx), wl_fixed_to_double(fy),
            label(g.proxSurface));
}
void toolPressure(void*, zwp_tablet_tool_v2*, std::uint32_t p) {
    ++g.pressures;
    g.maxPressure = std::max(g.maxPressure, p);
}
void toolDistance(void*, zwp_tablet_tool_v2*, std::uint32_t) {}
void toolTilt(void*, zwp_tablet_tool_v2*, wl_fixed_t, wl_fixed_t) {}
void toolRotation(void*, zwp_tablet_tool_v2*, wl_fixed_t) {}
void toolSlider(void*, zwp_tablet_tool_v2*, std::int32_t) {}
void toolWheel(void*, zwp_tablet_tool_v2*, wl_fixed_t, std::int32_t) {}
void toolButton(void*, zwp_tablet_tool_v2*, std::uint32_t, std::uint32_t, std::uint32_t) {}
void toolFrame(void*, zwp_tablet_tool_v2*, std::uint32_t) {}
const zwp_tablet_tool_v2_listener kToolListener = {
    toolType,     toolHwSerial, toolHwWacom, toolCapability, toolDoneEv,   toolRemoved,
    toolProximityIn, toolProximityOut, toolDown, toolUp,     toolMotion,   toolPressure,
    toolDistance, toolTilt,     toolRotation, toolSlider,    toolWheel,    toolButton,
    toolFrame,
};

void tabName(void*, zwp_tablet_v2*, const char* name) { say("tablet: name='%s'", name); }
void tabId(void*, zwp_tablet_v2*, std::uint32_t, std::uint32_t) {}
void tabPath(void*, zwp_tablet_v2*, const char*) {}
void tabDone(void*, zwp_tablet_v2*) { g.tabletAdded = true; }
void tabRemoved(void*, zwp_tablet_v2* t) { zwp_tablet_v2_destroy(t); }
const zwp_tablet_v2_listener kTabletListener = {tabName, tabId, tabPath, tabDone, tabRemoved};

void seatTabletAdded(void*, zwp_tablet_seat_v2*, zwp_tablet_v2* tablet) {
    say("tablet_added (dispatched inside Fl::wait)");
    zwp_tablet_v2_add_listener(tablet, &kTabletListener, nullptr);
}
void seatToolAdded(void*, zwp_tablet_seat_v2*, zwp_tablet_tool_v2* tool) {
    zwp_tablet_tool_v2_add_listener(tool, &kToolListener, nullptr);
}
void seatPadAdded(void*, zwp_tablet_seat_v2*, zwp_tablet_pad_v2* pad) {
    zwp_tablet_pad_v2_destroy(pad);
}
const zwp_tablet_seat_v2_listener kTabletSeatListener = {seatTabletAdded, seatToolAdded,
                                                         seatPadAdded};

// ---------------------------------------------------------------- registry (FLTK's display)

void regGlobal(void* /*data*/, wl_registry* reg, std::uint32_t name, const char* iface,
               std::uint32_t) {
    if (std::strcmp(iface, wl_seat_interface.name) == 0)
        g.seat = static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, 1));
    else if (std::strcmp(iface, zwp_tablet_manager_v2_interface.name) == 0)
        g.manager = static_cast<zwp_tablet_manager_v2*>(
            wl_registry_bind(reg, name, &zwp_tablet_manager_v2_interface, 1));
    else if (std::strcmp(iface, wl_shm_interface.name) == 0)
        g.shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
}
void regGlobalRemove(void*, wl_registry*, std::uint32_t) {}
const wl_registry_listener kRegistryListener = {regGlobal, regGlobalRemove};

// Same pattern as WaylandSubsurface's bindSubcompositor: bind on a private queue so the
// roundtrip cannot swallow FLTK's pending events, then hand the proxies to the default
// queue — from then on Fl::wait() dispatches them (that IS Q1').
bool bindTabletGlobals(wl_display* display) {
    wl_event_queue* queue = wl_display_create_queue(display);
    if (queue == nullptr)
        return false;
    wl_registry* registry = wl_display_get_registry(display);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(registry), queue);
    wl_registry_add_listener(registry, &kRegistryListener, nullptr);
    wl_display_roundtrip_queue(display, queue);
    for (void* p : {static_cast<void*>(g.seat), static_cast<void*>(g.manager),
                    static_cast<void*>(g.shm)})
        if (p != nullptr)
            wl_proxy_set_queue(static_cast<wl_proxy*>(p), nullptr);
    wl_registry_destroy(registry);
    wl_event_queue_destroy(queue);
    return g.seat != nullptr && g.manager != nullptr && g.shm != nullptr;
}

// Give the (real) WaylandSubsurface a buffer so it is mapped, as it is when Vulkan presents.
bool attachSubsurfaceBuffer(wl_surface* surf, int scale) {
    const int w = kSubW * scale, h = kSubH * scale, stride = w * 4;
    const std::size_t size = static_cast<std::size_t>(stride) * static_cast<std::size_t>(h);
    int fd = memfd_create("spike-sub", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0)
        return false;
    void* mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        close(fd);
        return false;
    }
    std::fill_n(static_cast<std::uint32_t*>(mem), size / 4, 0xff2d6cdf);
    munmap(mem, size);
    wl_shm_pool* pool = wl_shm_create_pool(g.shm, fd, static_cast<std::int32_t>(size));
    wl_buffer* buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_surface_attach(surf, buf, 0, 0);
    wl_surface_commit(surf); // desync child: presents immediately
    return true;
}

// ---------------------------------------------------------------- pen driver

void steerTo(double sx, double sy, St then) {
    g.steerX = sx;
    g.steerY = sy;
    g.afterSteer = then;
    g.steerSettled = 0;
    g.st = St::SteerCentre; // reused for every steer; afterSteer differentiates
    g.stTick = 0;
}

void summaryAndQuit(int code);

void tickMachine() {
    ++g.tick;
    ++g.stTick;
    const double X = VirtualPen::kMaxX, Y = VirtualPen::kMaxY;
    // Tablet-units-per-pixel; both axes iterate to convergence, the constant only seeds it.
    // (Stage 1 measured ~23 units/px on a 1920-wide output.)
    constexpr double kGain = 23.0;

    switch (g.st) {
    case St::WaitTablet:
        if (g.tabletAdded) {
            say("tablet announced; hover rows begin");
            g.st = St::Settle;
            g.stTick = 0;
        } else if (g.stTick > 8 * 100) {
            say("FAIL: tablet never announced");
            summaryAndQuit(3);
        }
        break;
    case St::Settle:
        if (g.stTick >= 30) {
            g.row = 0;
            g.st = St::RowStart;
            g.stTick = 0;
        }
        break;
    case St::RowStart: {
        if (g.row >= 9) {
            say("FAIL: swept 9 rows without hitting the FLTK window");
            summaryAndQuit(5);
            break;
        }
        const double fy = 0.10 + 0.10 * g.row;
        g.pen.proximityIn(static_cast<std::int32_t>(0.02 * X),
                          static_cast<std::int32_t>(fy * Y));
        g.pen.sync();
        g.st = St::Sweep;
        g.stTick = 0;
        break;
    }
    case St::Sweep: {
        const double f = std::min(1.0, static_cast<double>(g.stTick) / 120.0);
        g.pen.move(static_cast<std::int32_t>((0.02 + 0.96 * f) * X), g.pen.y());
        g.pen.sync();
        // Hit the FLTK surface with usable feedback? Go steer to the window centre.
        if (g.proxSurface == g.parentSurface && g.lastMotionTick >= g.tick - 2) {
            say("hit FLTK surface at local (%.1f, %.1f); steering to the lower-right quadrant",
                g.lastSx, g.lastSy);
            steerTo(kWinW * 0.75, kWinH * 0.75, St::Stroke); // clear of the subsurface at (0,0)
        } else if (g.stTick >= 130) {
            g.pen.proximityOut();
            g.pen.sync();
            ++g.row;
            g.st = St::RowStart;
            g.stTick = 0;
        }
        break;
    }
    case St::SteerCentre: {
        // Evidence first: while heading for the subsurface, capture is conclusive the moment
        // proximity moves off the parent (parent-local feedback then freezes and the
        // controller stalls -- that stall IS the answer, not a failure).
        if (g.afterSteer == St::Wrap) {
            if (g.proxSurface == g.subSurface)
                g.subDwellMask |= 2u;
            else if (g.proxSurface != nullptr && g.proxSurface != g.parentSurface)
                g.subDwellMask |= 4u;
        }
        if (g.stTick > 400) {
            const bool answered = g.afterSteer == St::Wrap && g.subDwellMask != 0;
            say("steer timed out at local (%.1f, %.1f)%s", g.lastSx, g.lastSy,
                answered ? " -- subsurface CAPTURED the pen (that is the answer)" : "");
            summaryAndQuit(answered ? 0 : 5);
            break;
        }
        const double ex = g.steerX - g.lastSx, ey = g.steerY - g.lastSy;
        if (std::abs(ex) < 8 && std::abs(ey) < 8) {
            if (g.afterSteer == St::Wrap)
                g.subDwellMask |= g.proxSurface == g.parentSurface ? 1u : 0u;
            if (++g.steerSettled >= 10) {
                g.st = g.afterSteer;
                g.stTick = 0;
                if (g.st == St::Stroke) {
                    say("stroke: tip down at local (%.1f, %.1f)", g.lastSx, g.lastSy);
                    g.pen.tipDown();
                }
            }
        } else {
            g.steerSettled = 0;
            // Damped proportional step, clamped so an overshoot cannot leave the window far.
            auto step = [](double e) {
                return std::clamp(0.6 * kGain * e, -1200.0, 1200.0);
            };
            g.pen.move(g.pen.x() + static_cast<std::int32_t>(step(ex)),
                       g.pen.y() + static_cast<std::int32_t>(step(ey)));
            g.pen.sync();
        }
        break;
    }
    case St::Stroke: {
        const int n = 40;
        const double f = static_cast<double>(g.stTick) / n;
        g.pen.move(g.pen.x() + 8, g.pen.y());
        g.pen.pressure(static_cast<std::int32_t>(65535.0 * std::sin(f * 3.14159265)));
        g.pen.sync();
        if (g.stTick >= n) {
            g.pen.pressure(0);
            g.pen.tipUp();
            g.pen.sync();
            say("stroke: tip up; steering over the real WaylandSubsurface (top-left)");
            steerTo(kSubW / 2.0, kSubH / 2.0, St::Wrap);
        }
        break;
    }
    case St::Wrap:
        if (g.stTick == 1) {
            g.pen.proximityOut();
            g.pen.sync();
        } else if (g.stTick >= 30) {
            summaryAndQuit(0);
        }
        break;
    case St::Done:
        break;
    }
    if (g.tick > 90 * 100) {
        say("FAIL: watchdog");
        summaryAndQuit(5);
    }
}

void timerCb(void*) {
    if (!g.running)
        return;
    tickMachine();
    if (g.running)
        Fl::repeat_timeout(kTick, timerCb);
}

void summaryAndQuit(int code) {
    g.exitCode = code;
    g.running = false;
    std::printf("\n================ FLTK SPIKE SUMMARY ================\n");
    std::printf("Q1' tablet events dispatched from Fl::wait():  %s\n",
                g.tabletAdded && g.motions > 0 ? "YES" : "NO");
    std::printf("    motions=%d pressures=%d max=%u down/up=%d/%d\n", g.motions, g.pressures,
                g.maxPressure, g.downSeen, g.upSeen);
    std::printf("Q2' proximity_in counts: FLTK-parent=%d subsurface=%d other=%d\n", g.proxParent,
                g.proxSub, g.proxOther);
    std::printf("    dwell near subsurface centre -> owner mask=%u (1=parent fall-through, "
                "2=subsurface CAPTURED, 4=other)\n",
                g.subDwellMask);
    std::printf("Q4  FLTK events during pen activity: ENTER=%d MOVE=%d PUSH=%d DRAG=%d "
                "RELEASE=%d LEAVE=%d\n",
                g.flEnter, g.flMove, g.flPush, g.flDrag, g.flRelease, g.flLeave);
    std::printf("    pointer emulation for the pen: %s\n",
                g.flPush > 0        ? "ACTIVE (FLTK lifecycle usable)"
                : g.flMove > 0      ? "PARTIAL (motion only, no button)"
                                    : "SUPPRESSED (backend must own the stroke lifecycle)");
    std::printf("====================================================\n");
    std::fflush(stdout);
}

} // namespace

int main() {
    // The hybrid FLTK picks Wayland when WAYLAND_DISPLAY is set; make it explicit anyway.
    setenv("FLTK_BACKEND", "wayland", 1);

    CountingWindow win;
    g.win = &win;
    win.end();
    win.show();
    // Let the window map before touching fl_wl_* accessors.
    for (int i = 0; i < 50 && fl_wl_xid(&win) == nullptr; ++i)
        Fl::wait(0.05);
    wl_display* display = fl_wl_display();
    struct wld_window* xid = fl_wl_xid(&win);
    if (display == nullptr || xid == nullptr) {
        std::fprintf(stderr, "FLTK did not come up on the Wayland backend\n");
        return 6;
    }
    g.parentSurface = fl_wl_surface(xid);
    std::printf("FLTK parent wl_surface = %p\n", static_cast<void*>(g.parentSurface));

    std::string err;
    auto sub = mosaic::platform::WaylandSubsurface::create(&win, err); // the REAL shipped path
    if (sub == nullptr) {
        std::fprintf(stderr, "WaylandSubsurface: %s\n", err.c_str());
        return 6;
    }
    g.subSurface = static_cast<wl_surface*>(sub->surface());
    std::printf("Vulkan-stand-in subsurface = %p\n", static_cast<void*>(g.subSurface));

    if (!bindTabletGlobals(display)) {
        std::fprintf(stderr, "wl_seat / zwp_tablet_manager_v2 / wl_shm unavailable\n");
        return 6;
    }
    if (!attachSubsurfaceBuffer(g.subSurface, std::max(1, fl_wl_buffer_scale(&win)))) {
        std::fprintf(stderr, "could not map the subsurface\n");
        return 6;
    }
    g.tabletSeat = zwp_tablet_manager_v2_get_tablet_seat(g.manager, g.seat);
    zwp_tablet_seat_v2_add_listener(g.tabletSeat, &kTabletSeatListener, nullptr);
    wl_display_flush(display);

    if (std::string penErr; !g.pen.create(penErr)) {
        std::fprintf(stderr, "virtual pen: %s\n", penErr.c_str());
        return 4;
    }
    say("virtual pen created");

    Fl::add_timeout(kTick, timerCb);
    while (g.running && win.shown())
        Fl::wait(0.1); // <- the only dispatch loop; tablet listeners fire from in here
    g.pen.destroy();
    return g.exitCode;
}
