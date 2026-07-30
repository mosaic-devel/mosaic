// The S19 Arc C X11/XI2 live probe (docs/tablet.md §3): the headless tests prove the
// parse/normalize path over hand-built XI structs; this proves the LIVE half of TabletX11 --
// XIQueryVersion, atom interning, hotplug via XI_HierarchyChanged, per-slave XISelectEvents, and
// GenericEvent cookie handling -- against the real XWayland server, driven by the same uinput
// virtual stylus as the Wayland spike so the events cross udev/libinput/KWin/XWayland exactly
// like hardware.
//
// It is ALSO the fact-check for two claims the backend bakes in:
//   1. XWayland's emulated tablet devices carry a client-ordinal suffix ("xwayland-tablet
//      eraser:13"), which is why tool matching is CONTAINS and not ends-with. The probe prints
//      every device name verbatim.
//   2. XWayland labels its valuators with the standard "Abs Pressure"/"Abs Tilt *" atoms. If it
//      did not, init would intern None and classification would find no axes.
//
// Synthetic-stream rule (§4 built-note, finding 5): an event-silent in-proximity tool is forced
// out of proximity after ~50 ms, so the pen jitters through every dwell.
//
// Run inside the session (needs $DISPLAY or :0, and /dev/uinput access):
//   cmake --preset linux-debug -DMOSAIC_BUILD_TABLET_SPIKE=ON && cmake --build ... tablet_spike_x11
//   ./build/linux-debug/bin/tablet_spike_x11

#include "platform/tablet_x11.hpp"
#include "virtual_pen.hpp"

#include <X11/Xatom.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using mosaic::platform::TabletSample;
using mosaic::platform::TabletX11;
using mosaic::platform::Xi2Device;
using mosaic::spike::VirtualPen;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

[[nodiscard]] std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

[[nodiscard]] const char* toolName(TabletSample::Tool t) {
    switch (t) {
    case TabletSample::Tool::Pen: return "Pen";
    case TabletSample::Tool::Eraser: return "Eraser";
    case TabletSample::Tool::Airbrush: return "Airbrush";
    case TabletSample::Tool::Puck: return "Puck";
    case TabletSample::Tool::Mouse: return "Mouse";
    }
    return "?";
}

// Process pending X events for `ms`, feeding everything into the backend and draining the ring.
void pump(Display* dpy, TabletX11& backend, std::uint64_t ms, std::vector<TabletSample>& out) {
    const std::uint64_t deadline = nowMs() + ms;
    while (nowMs() < deadline) {
        while (XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            backend.handleEvent(ev);
        }
        TabletSample s;
        while (backend.ring().pop(s))
            out.push_back(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// One hover report with jitter so evdev never filters us into the forced prox-out.
void jitterHover(VirtualPen& pen, std::int32_t x, std::int32_t y, int phase) {
    pen.move(x + ((phase & 1) != 0 ? 24 : -24), y + ((phase & 2) != 0 ? 24 : -24));
    pen.sync();
}

void printRawDevices(Display* dpy) {
    int n = 0;
    XIDeviceInfo* infos = XIQueryDevice(dpy, XIAllDevices, &n);
    std::printf("-- raw XIQueryDevice (%d devices) --\n", n);
    if (infos == nullptr)
        return;
    for (int i = 0; i < n; ++i)
        std::printf("   id=%2d use=%d  \"%s\"\n", infos[i].deviceid, infos[i].use, infos[i].name);
    XIFreeDeviceInfo(infos);
}

} // namespace

int main() {
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr)
        dpy = XOpenDisplay(":0");
    if (dpy == nullptr) {
        std::printf("FAIL  no X display reachable\n");
        return 1;
    }
    const int screen = DefaultScreen(dpy);
    const int screenW = DisplayWidth(dpy, screen);
    const int screenH = DisplayHeight(dpy, screen);
    std::printf("-- X screen %dx%d --\n", screenW, screenH);

    // A fullscreen window, so wherever KWin maps the tablet, the pen is over US.
    XSetWindowAttributes attrs{};
    attrs.background_pixel = BlackPixel(dpy, screen);
    attrs.event_mask = StructureNotifyMask;
    Window win = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0,
                               static_cast<unsigned>(screenW), static_cast<unsigned>(screenH), 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWBackPixel | CWEventMask, &attrs);
    Atom wmState = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom fs = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    XChangeProperty(dpy, win, wmState, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&fs), 1);
    XStoreName(dpy, win, "Mosaic XI2 probe");
    XMapWindow(dpy, win);
    XFlush(dpy);

    // P1: the backend comes up on a live server.
    TabletX11 backend;
    check(backend.init(dpy, win), "P1 init: XInputExtension present and XI2 >= 2.2");
    const std::size_t devicesBefore = backend.devices().size();
    std::printf("      classified before hotplug: %zu\n", devicesBefore);

    // Let the WM actually fullscreen + map us before the pen appears.
    std::vector<TabletSample> samples;
    pump(dpy, backend, 400, samples);

    // P2: hotplug. Two legitimate server behaviours here (both observed):
    //  - a native server (xf86-input-wacom/libinput) creates a NEW slave on uinput hotplug and
    //    announces it with XI_HierarchyChanged -> the count grows;
    //  - XWayland pre-creates its emulated "xwayland-tablet ..." slaves EAGERLY, before any
    //    tablet hardware exists, and routes every wayland tool through them -> the count is
    //    already nonzero at init and does not move on hotplug.
    VirtualPen pen;
    std::string err;
    if (!pen.create(err)) {
        std::printf("FAIL  uinput: %s\n", err.c_str());
        return 1;
    }
    const std::uint64_t hotplugStart = nowMs();
    while (nowMs() - hotplugStart < 3000 && backend.devices().size() <= devicesBefore)
        pump(dpy, backend, 100, samples);
    const std::size_t classifiedAfter = backend.devices().size();
    check(classifiedAfter > devicesBefore || devicesBefore > 0,
          "P2 hotplug: a stylus is classified (hierarchy-grown, or XWayland's eager devices)");
    std::printf("INFO  P2 fact: %zu classified at init, %zu after hotplug (%s)\n", devicesBefore,
                classifiedAfter,
                classifiedAfter > devicesBefore ? "hierarchy event grew the set"
                                                : "eager pre-created devices, no hierarchy growth");
    printRawDevices(dpy);
    std::printf("-- classified --\n");
    const Xi2Device* stylusDev = nullptr;
    bool sawEraserDevice = false;
    for (const Xi2Device& d : backend.devices()) {
        std::printf("   id=%2d tool=%-8s pressure=[%g..%g]#%d tiltX=[%g..%g]#%d  \"%s\"\n",
                    d.deviceId, toolName(d.tool), d.pressure.min, d.pressure.max,
                    d.pressure.number, d.tiltX.min, d.tiltX.max, d.tiltX.number, d.name.c_str());
        if (d.tool == TabletSample::Tool::Pen && d.pressure.present() && stylusDev == nullptr)
            stylusDev = &d;
        if (d.tool == TabletSample::Tool::Eraser)
            sawEraserDevice = true;
    }
    check(stylusDev != nullptr, "P2 classify: a Pen with a pressure axis");
    std::printf("%s  P2 fact: eraser device %s (claim: XWayland creates suffixed eraser devices)\n",
                sawEraserDevice ? "INFO" : "WARN",
                sawEraserDevice ? "present and classified" : "NOT present");

    // P3: hover motion routes to our (fullscreen) window and parses in-bounds.
    samples.clear();
    pen.proximityIn(VirtualPen::kMaxX / 2, VirtualPen::kMaxY / 2);
    pen.sync();
    for (int i = 0; i < 60 && samples.size() < 8; ++i) {
        jitterHover(pen, VirtualPen::kMaxX / 2, VirtualPen::kMaxY / 2, i);
        pump(dpy, backend, 25, samples);
    }
    if (samples.size() < 8) {
        // Multi-monitor case: the desktop centre may be off our output; walk a coarse grid.
        std::printf("      centre hover silent; walking a 5x5 grid\n");
        for (int gy = 0; gy < 5 && samples.size() < 8; ++gy)
            for (int gx = 0; gx < 5 && samples.size() < 8; ++gx) {
                const std::int32_t px = VirtualPen::kMaxX * gx / 4;
                const std::int32_t py = VirtualPen::kMaxY * gy / 4;
                for (int i = 0; i < 12 && samples.size() < 8; ++i) {
                    jitterHover(pen, px, py, i);
                    pump(dpy, backend, 25, samples);
                }
            }
    }
    check(samples.size() >= 8, "P3 hover: XI_Motion reaches handleEvent through the ring");
    bool inBounds = !samples.empty();
    bool fractional = false;
    for (const TabletSample& s : samples) {
        if (s.pos.x < -1 || s.pos.x > screenW + 1 || s.pos.y < -1 || s.pos.y > screenH + 1)
            inBounds = false;
        if (s.pos.x != std::floor(s.pos.x) || s.pos.y != std::floor(s.pos.y))
            fractional = true;
    }
    check(inBounds, "P3 hover: positions are window-local and in-bounds");
    if (!samples.empty())
        std::printf("INFO  P3 fact: first sample pos=(%.4f, %.4f) pressure=%.3f; fractional bits %s\n",
                    samples[0].pos.x, samples[0].pos.y, samples[0].pressure,
                    fractional ? "OBSERVED" : "not observed (XWayland may quantize)");
    const double hoverX = samples.empty() ? VirtualPen::kMaxX / 2.0 : 0.0; // anchor for the stroke
    (void)hoverX;

    // P4: a pressure-ramp stroke. Tip down = X button 1; the parsed pressure must sweep the range.
    samples.clear();
    const std::int32_t cx = pen.x();
    const std::int32_t cy = pen.y();
    pen.pressure(2000);
    pen.tipDown();
    pen.sync();
    pump(dpy, backend, 60, samples);
    for (int i = 0; i <= 40; ++i) {
        pen.move(cx + i * 40, cy + ((i & 1) != 0 ? 30 : -30));
        pen.pressure(VirtualPen::kMaxPressure * i / 40);
        pen.sync();
        pump(dpy, backend, 15, samples);
    }
    double minP = 2.0;
    double maxP = -1.0;
    bool buttonSeen = false;
    for (const TabletSample& s : samples) {
        minP = std::min(minP, s.pressure);
        maxP = std::max(maxP, s.pressure);
        if ((s.buttons & 1u) != 0)
            buttonSeen = true;
    }
    std::printf("INFO  P4 fact: %zu stroke samples, parsed pressure spans [%.3f .. %.3f]\n",
                samples.size(), minP, maxP);
    check(maxP > 0.9, "P4 pressure: ramp reaches the top of the normalized range");
    check(minP < 0.1, "P4 pressure: ramp starts near the bottom");
    check(buttonSeen, "P4 buttons: the tip press reads as X button 1 in the sample mask");

    // P5: tilt magnitude. Half of the pen's +-64 range must parse near half of full scale (30
    // degrees). Signs are REPORTED, not asserted -- that is a fact for the built-note.
    samples.clear();
    pen.tilt(32, -16);
    for (int i = 0; i < 20; ++i) {
        jitterHover(pen, pen.x(), pen.y(), i);
        pump(dpy, backend, 20, samples);
    }
    double xt = 0.0;
    double yt = 0.0;
    if (!samples.empty()) {
        xt = samples.back().xTilt;
        yt = samples.back().yTilt;
    }
    std::printf("INFO  P5 fact: pen tilt (32,-16)/64 parsed as xTilt=%.2f yTilt=%.2f degrees\n", xt, yt);
    check(!samples.empty() && std::fabs(std::fabs(xt) - 30.0) < 5.0,
          "P5 tilt: half-range tilt parses near 30 degrees");

    // P6: release. The button-1 bit must drop in the release sample.
    samples.clear();
    pen.pressure(0);
    pen.tipUp();
    pen.sync();
    for (int i = 0; i < 10; ++i) {
        jitterHover(pen, pen.x(), pen.y(), i);
        pump(dpy, backend, 20, samples);
    }
    bool sawUnbuttoned = false;
    for (const TabletSample& s : samples)
        if ((s.buttons & 1u) == 0)
            sawUnbuttoned = true;
    check(sawUnbuttoned, "P6 release: the tip-up cleared button 1 in the stream");

    // P7: bookkeeping across the whole run.
    check(backend.ring().overwritten() == 0, "P7 ring: nothing was overwritten at this drain cadence");
    // The samples vector was cleared per phase; monotonicity is asserted on the last batch.
    bool monotone = true;
    for (std::size_t i = 1; i < samples.size(); ++i)
        monotone = monotone && samples[i - 1].timeUs <= samples[i].timeUs;
    check(monotone, "P7 clock: ingest timestamps are monotone");

    pen.proximityOut();
    pen.sync();
    pen.destroy();
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    std::printf(g_failures == 0 ? "VERDICT: ALL PASS\n" : "VERDICT: %d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
