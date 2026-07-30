// S19 Arc C — the Wayland tablet spike (docs/tablet.md §4). A standalone client that answers,
// against the LIVE compositor, the unknowns the zwp_tablet_v2 backend design hangs on:
//
//   Q1  Binding our OWN wl_seat from the registry (FLTK does not expose its seat) yields a
//       zwp_tablet_seat_v2 that actually announces tablets and delivers tool events —
//       including pressure and tilt.
//   Q2  Tablet events respect wl_surface input regions the way wl_pointer does. Mosaic's
//       Vulkan subsurface ships with an EMPTY input region (wayland_subsurface.cpp), so pen
//       input over the canvas must fall THROUGH to the parent (FLTK's) surface. Proven both
//       ways: a subsurface with the default (infinite) region must CAPTURE proximity instead.
//   Q3  Motion arrives as wl_fixed with sub-pixel precision.
//
// No physical tablet is assumed: the spike creates a virtual stylus through uinput
// (virtual_pen.hpp), which the compositor ingests via udev/libinput exactly like hardware.
//
// Surface topology replicates Mosaic's: a fullscreen parent (stands in for the FLTK window)
// with two mapped 400x300 subsurfaces — CHILD-EMPTY at (60,60) with an empty input region
// (the shipped Vulkan-canvas configuration), CHILD-FULL at (520,60) with the default region.
//
// Safety interlock: the pen only HOVERS (harmless everywhere) until proximity_in proves it
// is over OUR fullscreen surface; the tip-down stroke happens only at a position computed —
// from a three-dwell affine fit of tablet coords onto parent-local coords — to land well
// inside our own window. If the tablet maps to a different output, the spike re-fullscreens
// there and retries.
//
// Exit codes: 0 = all three questions answered (see the summary block for the answers);
// 2 = no zwp_tablet_manager_v2; 3 = compositor never announced the virtual tablet;
// 4 = uinput unavailable; 5 = no proximity on any output / timeout; 6 = connection error.

#include "virtual_pen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>

#include "tablet-unstable-v2-client.h"
#include "xdg-shell-client.h"

namespace {

using mosaic::spike::VirtualPen;

constexpr int kChildW = 400, kChildH = 300;
constexpr int kChildEmptyX = 60, kChildEmptyY = 60;
constexpr int kChildFullX = 520, kChildFullY = 60;
constexpr int kTickHz = 100; // driver cadence; one uinput report per tick

// One dwell sample: where the pen was told to be vs. where the compositor said it is
// (parent-surface-local). Dwelling lets compositor latency drain before we read it.
struct Sample {
    double tx = 0, ty = 0; // tablet units (what we sent)
    double sx = 0, sy = 0; // parent-local (what came back)
    bool valid = false;
};

struct Fit {
    double ax = 0, bx = 0, ay = 0, by = 0;
    bool valid = false;
    double toTabletX(double sx) const { return (sx - bx) / ax; }
    double toTabletY(double sy) const { return (sy - by) / ay; }
};

enum class St {
    WaitTablet, // uinput device live; waiting for zwp_tablet_seat_v2.tablet_added
    Settle,     // grace after tablet_added
    DwellA,     // prox-in at (5%,55%), dwell        -> sample A
    SweepX,     // hover x: 5% -> 95% at y=55%
    DwellB,     // dwell                             -> sample B (x fit)
    MoveY,      // hover y: 55% -> 80% at x=95%
    DwellC,     // dwell                             -> sample C (y fit)
    Retry,      // no proximity anywhere: re-fullscreen on the next output
    StrokeHover,// glide to a fit-computed point well inside the parent
    Stroke,     // tip down, pressure ramp + tilt wiggle
    HoverEmpty, // glide to CHILD-EMPTY's centre, dwell: who gets the events?
    HoverFull,  // glide to CHILD-FULL's centre, dwell: who gets the events?
    Wrap,       // prox-out, drain trailing events
    Done
};

struct Spike {
    // Wayland globals
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr; // OUR bind — the Q1 subject
    zwp_tablet_manager_v2* tabletManager = nullptr;
    xdg_wm_base* wmBase = nullptr;
    std::vector<wl_output*> outputs;

    // Surfaces
    wl_surface* parent = nullptr;
    xdg_surface* xdgSurf = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_surface* childEmpty = nullptr;
    wl_surface* childFull = nullptr;
    wl_subsurface* subEmpty = nullptr;
    wl_subsurface* subFull = nullptr;
    int parentW = 0, parentH = 0;
    int pendingW = 0, pendingH = 0;
    bool configured = false;
    bool childrenMapped = false;

    // Tablet
    zwp_tablet_seat_v2* tabletSeat = nullptr;
    zwp_tablet_tool_v2* tool = nullptr;
    wl_surface* proxSurface = nullptr; // surface of the current proximity, null when out

    // Virtual pen + state machine
    VirtualPen pen;
    St st = St::WaitTablet;
    long tick = 0;      // global tick count
    long stTick = 0;    // ticks spent in the current state
    int outputIdx = 0;  // which output we are fullscreen on
    Sample a, b, c;
    Fit fit;
    double glideFromX = 0, glideFromY = 0, glideToX = 0, glideToY = 0;

    // Evidence
    bool verbose = false;
    bool tabletAdded = false, toolDone = false;
    std::string tabletName;
    std::uint32_t toolType = 0;
    std::vector<std::uint32_t> toolCaps;
    int proxInParent = 0, proxInChildEmpty = 0, proxInChildFull = 0;
    int motions = 0, frames = 0, pressures = 0, tilts = 0;
    bool subpixelSeen = false;
    double subpixelExampleX = 0, subpixelExampleY = 0;
    std::uint32_t maxPressure = 0;
    double lastMotionSx = 0, lastMotionSy = 0;
    long lastParentMotionTick = -1;
    bool downSeen = false, upSeen = false;
    // Who owned proximity during each child dwell (bitmask: 1=parent, 2=empty, 4=full)
    unsigned hoverEmptyMask = 0, hoverFullMask = 0;

    bool running = true;
    int exitCode = 0;
};

Spike g;

double elapsedMs() { return 1000.0 * static_cast<double>(g.tick) / kTickHz; }

void say(const char* fmt, ...) {
    std::printf("%8.0fms | ", elapsedMs());
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

const char* label(wl_surface* s) {
    if (s == nullptr)
        return "(none)";
    if (s == g.parent)
        return "PARENT";
    if (s == g.childEmpty)
        return "CHILD-EMPTY-REGION";
    if (s == g.childFull)
        return "CHILD-FULL-REGION";
    return "(foreign?)";
}

// ---------------------------------------------------------------- shm buffers

wl_buffer* makeBuffer(int w, int h, std::uint32_t argb) {
    const int stride = w * 4;
    const std::size_t size = static_cast<std::size_t>(stride) * static_cast<std::size_t>(h);
    int fd = memfd_create("spike-buffer", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0)
        return nullptr;
    void* mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    std::fill_n(static_cast<std::uint32_t*>(mem), size / 4, argb);
    munmap(mem, size);
    wl_shm_pool* pool = wl_shm_create_pool(g.shm, fd, static_cast<std::int32_t>(size));
    wl_buffer* buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

// ---------------------------------------------------------------- tablet listeners

void toolType(void*, zwp_tablet_tool_v2*, std::uint32_t t) {
    g.toolType = t;
    say("tool: type=%u (%s)", t, t == ZWP_TABLET_TOOL_V2_TYPE_PEN ? "PEN" : "other");
}
void toolHwSerial(void*, zwp_tablet_tool_v2*, std::uint32_t hi, std::uint32_t lo) {
    say("tool: hardware_serial=%08x%08x", hi, lo);
}
void toolHwWacom(void*, zwp_tablet_tool_v2*, std::uint32_t, std::uint32_t) {}
void toolCapability(void*, zwp_tablet_tool_v2*, std::uint32_t cap) {
    g.toolCaps.push_back(cap);
    static const char* names[] = {"tilt", "pressure", "distance", "rotation", "slider", "wheel"};
    say("tool: capability=%u (%s)", cap, cap <= 6 && cap >= 1 ? names[cap - 1] : "?");
}
void toolDoneEv(void*, zwp_tablet_tool_v2*) {
    g.toolDone = true;
    say("tool: done (%zu capabilities)", g.toolCaps.size());
}
void toolRemoved(void*, zwp_tablet_tool_v2* t) {
    say("tool: removed");
    zwp_tablet_tool_v2_destroy(t);
    if (g.tool == t)
        g.tool = nullptr;
}
void toolProximityIn(void*, zwp_tablet_tool_v2*, std::uint32_t serial, zwp_tablet_v2*,
                     wl_surface* surface) {
    g.proxSurface = surface;
    if (surface == g.parent)
        ++g.proxInParent;
    else if (surface == g.childEmpty)
        ++g.proxInChildEmpty;
    else if (surface == g.childFull)
        ++g.proxInChildFull;
    say("proximity_in  serial=%u surface=%s", serial, label(surface));
}
void toolProximityOut(void*, zwp_tablet_tool_v2*) {
    say("proximity_out (was %s)", label(g.proxSurface));
    g.proxSurface = nullptr;
}
void toolDown(void*, zwp_tablet_tool_v2*, std::uint32_t serial) {
    g.downSeen = true;
    say("down serial=%u on %s", serial, label(g.proxSurface));
}
void toolUp(void*, zwp_tablet_tool_v2*) {
    g.upSeen = true;
    say("up on %s", label(g.proxSurface));
}
void toolMotion(void*, zwp_tablet_tool_v2*, wl_fixed_t fx, wl_fixed_t fy) {
    ++g.motions;
    const double x = wl_fixed_to_double(fx), y = wl_fixed_to_double(fy);
    if (((fx | fy) & 0xff) != 0 && !g.subpixelSeen) {
        g.subpixelSeen = true;
        g.subpixelExampleX = x;
        g.subpixelExampleY = y;
    }
    if (g.proxSurface == g.parent) {
        g.lastMotionSx = x;
        g.lastMotionSy = y;
        g.lastParentMotionTick = g.tick;
    }
    if (g.verbose || g.motions <= 3)
        say("motion (%.4f, %.4f) on %s", x, y, label(g.proxSurface));
}
void toolPressure(void*, zwp_tablet_tool_v2*, std::uint32_t p) {
    ++g.pressures;
    g.maxPressure = std::max(g.maxPressure, p);
    if (g.verbose || g.pressures <= 3 || p == 65535)
        say("pressure %u on %s", p, label(g.proxSurface));
}
void toolDistance(void*, zwp_tablet_tool_v2*, std::uint32_t) {}
void toolTilt(void*, zwp_tablet_tool_v2*, wl_fixed_t tx, wl_fixed_t ty) {
    ++g.tilts;
    if (g.verbose || g.tilts <= 3)
        say("tilt (%.2f, %.2f)", wl_fixed_to_double(tx), wl_fixed_to_double(ty));
}
void toolRotation(void*, zwp_tablet_tool_v2*, wl_fixed_t) {}
void toolSlider(void*, zwp_tablet_tool_v2*, std::int32_t) {}
void toolWheel(void*, zwp_tablet_tool_v2*, wl_fixed_t, std::int32_t) {}
void toolButton(void*, zwp_tablet_tool_v2*, std::uint32_t, std::uint32_t b, std::uint32_t s) {
    say("button %u state=%u", b, s);
}
void toolFrame(void*, zwp_tablet_tool_v2*, std::uint32_t) { ++g.frames; }
const zwp_tablet_tool_v2_listener kToolListener = {
    toolType,     toolHwSerial, toolHwWacom, toolCapability, toolDoneEv,   toolRemoved,
    toolProximityIn, toolProximityOut, toolDown, toolUp,     toolMotion,   toolPressure,
    toolDistance, toolTilt,     toolRotation, toolSlider,    toolWheel,    toolButton,
    toolFrame,
};

void tabName(void*, zwp_tablet_v2*, const char* name) {
    g.tabletName = name;
    say("tablet: name='%s'", name);
}
void tabId(void*, zwp_tablet_v2*, std::uint32_t vid, std::uint32_t pid) {
    say("tablet: id=%04x:%04x", vid, pid);
}
void tabPath(void*, zwp_tablet_v2*, const char* path) { say("tablet: path=%s", path); }
void tabDone(void*, zwp_tablet_v2*) { g.tabletAdded = true; }
void tabRemoved(void*, zwp_tablet_v2* t) {
    say("tablet: removed");
    zwp_tablet_v2_destroy(t);
}
const zwp_tablet_v2_listener kTabletListener = {tabName, tabId, tabPath, tabDone, tabRemoved};

void seatTabletAdded(void*, zwp_tablet_seat_v2*, zwp_tablet_v2* tablet) {
    say("tablet_added (on OUR self-bound seat)");
    zwp_tablet_v2_add_listener(tablet, &kTabletListener, nullptr);
}
void seatToolAdded(void*, zwp_tablet_seat_v2*, zwp_tablet_tool_v2* tool) {
    say("tool_added");
    g.tool = tool;
    zwp_tablet_tool_v2_add_listener(tool, &kToolListener, nullptr);
}
void seatPadAdded(void*, zwp_tablet_seat_v2*, zwp_tablet_pad_v2* pad) {
    say("pad_added (unexpected for a pen-only device)");
    zwp_tablet_pad_v2_destroy(pad);
}
const zwp_tablet_seat_v2_listener kTabletSeatListener = {seatTabletAdded, seatToolAdded,
                                                         seatPadAdded};

// ---------------------------------------------------------------- xdg listeners

void wmPing(void*, xdg_wm_base* wm, std::uint32_t serial) { xdg_wm_base_pong(wm, serial); }
const xdg_wm_base_listener kWmBaseListener = {wmPing};

void topConfigure(void*, xdg_toplevel*, std::int32_t w, std::int32_t h, wl_array*) {
    g.pendingW = w;
    g.pendingH = h;
}
void topClose(void*, xdg_toplevel*) {
    say("toplevel closed by compositor");
    g.running = false;
    g.exitCode = 5;
}
const xdg_toplevel_listener kToplevelListener = {topConfigure, topClose};

void mapChildren() {
    if (g.childrenMapped)
        return;
    g.childrenMapped = true;
    auto makeChild = [&](int px, int py, std::uint32_t argb, bool emptyRegion, wl_surface** surf,
                         wl_subsurface** sub) {
        *surf = wl_compositor_create_surface(g.compositor);
        *sub = wl_subcompositor_get_subsurface(g.subcompositor, *surf, g.parent);
        wl_subsurface_set_position(*sub, px, py);
        wl_subsurface_set_desync(*sub);
        if (emptyRegion) {
            // The shipped Vulkan-canvas configuration (wayland_subsurface.cpp): a mapped,
            // input-transparent child.
            wl_region* empty = wl_compositor_create_region(g.compositor);
            wl_surface_set_input_region(*surf, empty);
            wl_region_destroy(empty);
        }
        wl_surface_attach(*surf, makeBuffer(kChildW, kChildH, argb), 0, 0);
        wl_surface_commit(*surf);
    };
    makeChild(kChildEmptyX, kChildEmptyY, 0xff2d6cdf, true, &g.childEmpty, &g.subEmpty);
    makeChild(kChildFullX, kChildFullY, 0xffcf5b3d, false, &g.childFull, &g.subFull);
    say("children mapped: CHILD-EMPTY-REGION blue @(%d,%d), CHILD-FULL-REGION orange @(%d,%d)",
        kChildEmptyX, kChildEmptyY, kChildFullX, kChildFullY);
}

void surfConfigure(void*, xdg_surface* xs, std::uint32_t serial) {
    xdg_surface_ack_configure(xs, serial);
    int w = g.pendingW > 0 ? g.pendingW : 1280;
    int h = g.pendingH > 0 ? g.pendingH : 800;
    if (w != g.parentW || h != g.parentH) {
        g.parentW = w;
        g.parentH = h;
        wl_surface_attach(g.parent, makeBuffer(w, h, 0xff202028), 0, 0);
        say("parent configured %dx%d (output %d)", w, h, g.outputIdx);
    }
    mapChildren();
    wl_surface_commit(g.parent); // maps/moves the children too (their placement is parent state)
    g.configured = true;
}
const xdg_surface_listener kXdgSurfaceListener = {surfConfigure};

// ---------------------------------------------------------------- registry

void regGlobal(void*, wl_registry* reg, std::uint32_t name, const char* iface, std::uint32_t) {
    auto is = [&](const wl_interface& i) { return std::strcmp(iface, i.name) == 0; };
    if (is(wl_compositor_interface))
        g.compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    else if (is(wl_subcompositor_interface))
        g.subcompositor = static_cast<wl_subcompositor*>(
            wl_registry_bind(reg, name, &wl_subcompositor_interface, 1));
    else if (is(wl_shm_interface))
        g.shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
    else if (is(wl_seat_interface))
        g.seat = static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, 1));
    else if (is(zwp_tablet_manager_v2_interface))
        g.tabletManager = static_cast<zwp_tablet_manager_v2*>(
            wl_registry_bind(reg, name, &zwp_tablet_manager_v2_interface, 1));
    else if (is(xdg_wm_base_interface))
        g.wmBase = static_cast<xdg_wm_base*>(
            wl_registry_bind(reg, name, &xdg_wm_base_interface, 1));
    else if (is(wl_output_interface))
        g.outputs.push_back(
            static_cast<wl_output*>(wl_registry_bind(reg, name, &wl_output_interface, 1)));
}
void regGlobalRemove(void*, wl_registry*, std::uint32_t) {}
const wl_registry_listener kRegistryListener = {regGlobal, regGlobalRemove};

// ---------------------------------------------------------------- driver state machine

void enter(St s) {
    g.st = s;
    g.stTick = 0;
}

Sample takeSample(const char* which) {
    Sample s;
    s.tx = g.pen.x();
    s.ty = g.pen.y();
    // Trust the last parent motion only if it arrived during this dwell or with the final
    // pre-dwell move. evdev filters unchanged ABS values, so a stationary dwell produces NO
    // new events -- the sample of record is the one that carried the final position.
    if (g.lastParentMotionTick >= g.tick - (g.stTick + 2)) {
        s.sx = g.lastMotionSx;
        s.sy = g.lastMotionSy;
        s.valid = true;
        say("sample %s: tablet(%.0f,%.0f) -> parent-local(%.3f,%.3f)", which, s.tx, s.ty, s.sx,
            s.sy);
    } else {
        say("sample %s: NO parent motion during dwell", which);
    }
    return s;
}

void computeFit() {
    g.fit.valid = false;
    if (g.a.valid && g.b.valid && g.c.valid && std::abs(g.b.tx - g.a.tx) > 1.0
        && std::abs(g.c.ty - g.b.ty) > 1.0) {
        g.fit.ax = (g.b.sx - g.a.sx) / (g.b.tx - g.a.tx);
        g.fit.ay = (g.c.sy - g.b.sy) / (g.c.ty - g.b.ty);
        if (std::abs(g.fit.ax) > 1e-6 && std::abs(g.fit.ay) > 1e-6) {
            g.fit.bx = g.a.sx - g.fit.ax * g.a.tx;
            g.fit.by = g.b.sy - g.fit.ay * g.b.ty;
            g.fit.valid = true;
            say("fit: sx = %.6f*tx %+.2f ; sy = %.6f*ty %+.2f", g.fit.ax, g.fit.bx, g.fit.ay,
                g.fit.by);
        }
    }
    if (!g.fit.valid)
        say("fit: FAILED (insufficient dwell samples)");
}

void beginGlide(double toParentX, double toParentY) {
    g.glideFromX = g.pen.x();
    g.glideFromY = g.pen.y();
    g.glideToX = g.fit.toTabletX(toParentX);
    g.glideToY = g.fit.toTabletY(toParentY);
}

void glideStep(long t, long dur) {
    const double f = std::min(1.0, static_cast<double>(t) / static_cast<double>(dur));
    g.pen.move(static_cast<std::int32_t>(std::lround(g.glideFromX + (g.glideToX - g.glideFromX) * f)),
               static_cast<std::int32_t>(std::lround(g.glideFromY + (g.glideToY - g.glideFromY) * f)));
    g.pen.sync();
}

unsigned proxBit() {
    return g.proxSurface == g.parent       ? 1u
           : g.proxSurface == g.childEmpty ? 2u
           : g.proxSurface == g.childFull  ? 4u
                                           : 0u;
}

void tickMachine() {
    ++g.tick;
    ++g.stTick;
    const double X = VirtualPen::kMaxX, Y = VirtualPen::kMaxY;
    switch (g.st) {
    case St::WaitTablet:
        if (g.tabletAdded && g.configured) {
            say("compositor announced the virtual tablet; settling");
            enter(St::Settle);
        } else if (g.stTick > 8 * kTickHz) {
            say("FAIL: compositor never announced the virtual tablet (is this seat0? "
                "check the compositor journal for libinput rejects)");
            g.exitCode = 3;
            g.running = false;
        }
        break;
    case St::Settle:
        if (g.stTick == 30) {
            say("hover pass: prox-in at (5%%, 55%%) of the tablet area");
            g.pen.proximityIn(static_cast<std::int32_t>(0.05 * X),
                              static_cast<std::int32_t>(0.55 * Y));
            g.pen.sync();
            enter(St::DwellA);
        }
        break;
    case St::DwellA:
        if (g.stTick >= 12) {
            g.a = takeSample("A");
            enter(St::SweepX);
        }
        break;
    case St::SweepX: {
        const double f = static_cast<double>(g.stTick) / 120.0;
        g.pen.move(static_cast<std::int32_t>((0.05 + 0.90 * std::min(1.0, f)) * X),
                   static_cast<std::int32_t>(0.55 * Y));
        g.pen.sync();
        if (g.stTick >= 120)
            enter(St::DwellB);
        break;
    }
    case St::DwellB:
        if (g.stTick >= 12) {
            g.b = takeSample("B");
            if (g.proxInParent + g.proxInChildEmpty + g.proxInChildFull == 0)
                enter(St::Retry);
            else
                enter(St::MoveY);
        }
        break;
    case St::MoveY: {
        const double f = static_cast<double>(g.stTick) / 30.0;
        g.pen.move(static_cast<std::int32_t>(0.95 * X),
                   static_cast<std::int32_t>((0.55 + 0.25 * std::min(1.0, f)) * Y));
        g.pen.sync();
        if (g.stTick >= 30)
            enter(St::DwellC);
        break;
    }
    case St::DwellC:
        if (g.stTick >= 12) {
            g.c = takeSample("C");
            computeFit();
            if (g.fit.valid) {
                beginGlide(g.parentW * 0.5, g.parentH * 0.65);
                say("stroke: gliding to parent-local (%.0f, %.0f)", g.parentW * 0.5,
                    g.parentH * 0.65);
                enter(St::StrokeHover);
            } else {
                say("no usable fit; skipping stroke + child hovers");
                enter(St::Wrap);
            }
        }
        break;
    case St::Retry:
        if (g.stTick == 1) {
            g.pen.proximityOut();
            g.pen.sync();
            ++g.outputIdx;
            if (g.outputIdx >= static_cast<int>(g.outputs.size())) {
                say("FAIL: swept every output without a single proximity_in");
                g.exitCode = 5;
                g.running = false;
                break;
            }
            say("no proximity on output %d; re-fullscreening on output %d", g.outputIdx - 1,
                g.outputIdx);
            xdg_toplevel_set_fullscreen(g.toplevel, g.outputs[static_cast<size_t>(g.outputIdx)]);
            wl_surface_commit(g.parent);
        } else if (g.stTick >= 80) {
            g.pen.proximityIn(static_cast<std::int32_t>(0.05 * X),
                              static_cast<std::int32_t>(0.55 * Y));
            g.pen.sync();
            enter(St::DwellA);
        }
        break;
    case St::StrokeHover:
        glideStep(g.stTick, 20);
        if (g.stTick >= 24) {
            // Interlock: tip-down only while proximity is on one of OUR surfaces.
            if (g.proxSurface != nullptr) {
                say("stroke: tip down (pressure ramp, tilt wiggle)");
                g.pen.tipDown();
                enter(St::Stroke);
            } else {
                say("stroke: SKIPPED (not over our surface after glide)");
                enter(St::HoverEmpty);
                beginGlide(kChildEmptyX + kChildW / 2.0, kChildEmptyY + kChildH / 2.0);
            }
        }
        break;
    case St::Stroke: {
        const int n = 40;
        const double f = static_cast<double>(g.stTick) / n;
        g.pen.move(g.pen.x() + 8, g.pen.y());
        g.pen.pressure(static_cast<std::int32_t>(65535.0 * std::sin(f * 3.14159265)));
        g.pen.tilt(static_cast<std::int32_t>(30 * std::sin(f * 6.28)), 15);
        g.pen.sync();
        if (g.stTick >= n) {
            g.pen.pressure(0);
            g.pen.tipUp();
            g.pen.tilt(0, 0);
            g.pen.sync();
            say("stroke: tip up");
            beginGlide(kChildEmptyX + kChildW / 2.0, kChildEmptyY + kChildH / 2.0);
            say("hover test: gliding to CHILD-EMPTY-REGION centre");
            enter(St::HoverEmpty);
        }
        break;
    }
    case St::HoverEmpty:
        glideStep(g.stTick, 20);
        if (g.stTick > 24) // glide done + latency drained: sample who owns proximity
            g.hoverEmptyMask |= proxBit();
        if (g.stTick >= 45) {
            beginGlide(kChildFullX + kChildW / 2.0, kChildFullY + kChildH / 2.0);
            say("hover test: gliding to CHILD-FULL-REGION centre");
            enter(St::HoverFull);
        }
        break;
    case St::HoverFull:
        glideStep(g.stTick, 20);
        if (g.stTick > 24)
            g.hoverFullMask |= proxBit();
        if (g.stTick >= 45)
            enter(St::Wrap);
        break;
    case St::Wrap:
        if (g.stTick == 1) {
            g.pen.proximityOut();
            g.pen.sync();
        } else if (g.stTick >= 30) {
            enter(St::Done);
            g.running = false;
        }
        break;
    case St::Done:
        break;
    }
    if (g.tick > 60 * kTickHz) {
        say("FAIL: global watchdog fired");
        g.exitCode = 5;
        g.running = false;
    }
}

// ---------------------------------------------------------------- summary

const char* maskStr(unsigned m) {
    switch (m) {
    case 0: return "nobody";
    case 1: return "PARENT";
    case 2: return "CHILD-EMPTY-REGION";
    case 4: return "CHILD-FULL-REGION";
    default: return "MIXED";
    }
}

void summary() {
    std::printf("\n==================== SPIKE SUMMARY ====================\n");
    const bool q1 = g.tabletAdded && g.toolDone && g.pressures > 0;
    std::printf("Q1 self-bound wl_seat -> zwp_tablet_seat_v2 delivers:  %s\n",
                q1 ? "YES" : "NO");
    std::printf("   tablet_added=%d tool_done=%d type=%u caps=%zu motions=%d frames=%d\n",
                g.tabletAdded, g.toolDone, g.toolType, g.toolCaps.size(), g.motions, g.frames);
    std::printf("   pressure events=%d max=%u (65535 sent)  tilt events=%d  down/up=%d/%d\n",
                g.pressures, g.maxPressure, g.tilts, g.downSeen, g.upSeen);
    std::printf("Q2 routing: proximity_in counts  parent=%d  child-empty=%d  child-full=%d\n",
                g.proxInParent, g.proxInChildEmpty, g.proxInChildFull);
    std::printf("   dwell over CHILD-EMPTY-REGION -> events went to: %s%s\n",
                maskStr(g.hoverEmptyMask),
                g.hoverEmptyMask == 1 ? "   (fall-through CONFIRMED)" : "");
    std::printf("   dwell over CHILD-FULL-REGION  -> events went to: %s%s\n",
                maskStr(g.hoverFullMask),
                g.hoverFullMask == 4 ? "   (region capture CONFIRMED)" : "");
    std::printf("Q3 sub-pixel motion: %s", g.subpixelSeen ? "YES" : "NO");
    if (g.subpixelSeen)
        std::printf("   e.g. (%.4f, %.4f)", g.subpixelExampleX, g.subpixelExampleY);
    std::printf("\n=======================================================\n");
    if (g.exitCode == 0 && !(q1 && g.subpixelSeen && g.proxInParent > 0))
        g.exitCode = 5;
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0)
            g.verbose = true;
        else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc)
            g.outputIdx = std::atoi(argv[++i]);
    }

    g.display = wl_display_connect(nullptr);
    if (g.display == nullptr) {
        std::fprintf(stderr, "not a Wayland session\n");
        return 6;
    }
    wl_registry* reg = wl_display_get_registry(g.display);
    wl_registry_add_listener(reg, &kRegistryListener, nullptr);
    wl_display_roundtrip(g.display);
    if (g.tabletManager == nullptr) {
        std::fprintf(stderr, "compositor does not advertise zwp_tablet_manager_v2\n");
        return 2;
    }
    if (g.compositor == nullptr || g.subcompositor == nullptr || g.shm == nullptr
        || g.seat == nullptr || g.wmBase == nullptr || g.outputs.empty()) {
        std::fprintf(stderr, "missing core globals\n");
        return 6;
    }
    if (g.outputIdx >= static_cast<int>(g.outputs.size()))
        g.outputIdx = 0;
    xdg_wm_base_add_listener(g.wmBase, &kWmBaseListener, nullptr);

    // Q1's subject: the tablet seat obtained from OUR OWN wl_seat bind.
    g.tabletSeat = zwp_tablet_manager_v2_get_tablet_seat(g.tabletManager, g.seat);
    zwp_tablet_seat_v2_add_listener(g.tabletSeat, &kTabletSeatListener, nullptr);

    // Parent surface, fullscreen so hover exploration cannot land on anyone else's window.
    g.parent = wl_compositor_create_surface(g.compositor);
    g.xdgSurf = xdg_wm_base_get_xdg_surface(g.wmBase, g.parent);
    xdg_surface_add_listener(g.xdgSurf, &kXdgSurfaceListener, nullptr);
    g.toplevel = xdg_surface_get_toplevel(g.xdgSurf);
    xdg_toplevel_add_listener(g.toplevel, &kToplevelListener, nullptr);
    xdg_toplevel_set_title(g.toplevel, "Mosaic tablet spike");
    xdg_toplevel_set_app_id(g.toplevel, "mosaic-tablet-spike");
    xdg_toplevel_set_fullscreen(g.toplevel, g.outputs[static_cast<size_t>(g.outputIdx)]);
    wl_surface_commit(g.parent);
    wl_display_roundtrip(g.display); // first configure lands here

    std::string err;
    if (!g.pen.create(err)) {
        std::fprintf(stderr, "virtual pen: %s\n", err.c_str());
        return 4;
    }
    say("virtual pen created; waiting for the compositor to announce it");

    const int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    itimerspec its{};
    its.it_interval.tv_nsec = 1000000000L / kTickHz;
    its.it_value.tv_nsec = its.it_interval.tv_nsec;
    timerfd_settime(tfd, 0, &its, nullptr);

    pollfd fds[2] = {{wl_display_get_fd(g.display), POLLIN, 0}, {tfd, POLLIN, 0}};
    while (g.running) {
        while (wl_display_prepare_read(g.display) != 0)
            wl_display_dispatch_pending(g.display);
        wl_display_flush(g.display);
        if (poll(fds, 2, 2000) < 0) {
            wl_display_cancel_read(g.display);
            break;
        }
        if ((fds[0].revents & POLLIN) != 0)
            wl_display_read_events(g.display);
        else
            wl_display_cancel_read(g.display);
        if (wl_display_dispatch_pending(g.display) < 0) {
            std::fprintf(stderr, "wayland connection error\n");
            g.exitCode = 6;
            break;
        }
        if ((fds[1].revents & POLLIN) != 0) {
            std::uint64_t expirations = 0;
            (void)!read(tfd, &expirations, sizeof(expirations));
            tickMachine();
        }
    }

    g.pen.destroy();
    summary();
    close(tfd);
    wl_display_disconnect(g.display);
    return g.exitCode;
}
