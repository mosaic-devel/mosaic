// A live pen driver for the REAL Mosaic binary (S19 jagged-stroke investigation).
//
// The diagnostic spike measures ui::TabletInput on a stub window and finds nothing wrong. This
// drives the SHIPPED app instead: it creates a uinput STYLUS, steers it to a point on the canvas,
// and lays one straight stroke at a constant rate and pressure. Pair it with $MOSAIC_TABLET_PROBE
// to capture the exact StrokeInput stream the engine received, on either backend.
//
// ⚠ IT CREATES A STYLUS AND NOTHING ELSE. An earlier version also made a uinput KEYBOARD to tap "B"
// for the Brush (ToolManager starts on Move) -- but a virtual keyboard types into whatever window
// holds the focus, which is NOT necessarily ours: on a run where the stroke missed the canvas the
// keystrokes went to the desktop and opened KRunner. Never inject keys into a live session. The app
// selects the Brush itself when $MOSAIC_TABLET_PROBE is set.
//
// The pen is absolute-mapped to the PRIMARY OUTPUT (KWin), so the mapping from screen pixels to
// tablet units is linear and open-loop -- no feedback from the app is needed or possible.
//
//   pen_driver <cx> <cy> <dx> <dy> <hz> <secs> [mapW mapH]
//     cx,cy   stroke start, in SCREEN pixels
//     dx,dy   total stroke displacement, in SCREEN pixels
//     hz      device report rate
//     secs    stroke duration
//     mapW/H  the output the tablet is mapped to. ⚠ KWin maps a tablet to the PRIMARY OUTPUT, not
//             to the whole desktop -- on a dual-head X screen (3840x1080 here) taking the mapping
//             from DisplayWidth halves every coordinate and the stroke lands somewhere else
//             entirely. Defaults to the X screen only when it is not given.

#include "virtual_pen.hpp"

#include <X11/Xlib.h>
#include <linux/uinput.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

using mosaic::spike::VirtualPen;

} // namespace

int main(int argc, char** argv) {
    if (argc < 7) {
        std::fprintf(stderr, "usage: pen_driver <cx> <cy> <dx> <dy> <hz> <secs>\n");
        return 2;
    }
    const double cx = std::atof(argv[1]);
    const double cy = std::atof(argv[2]);
    const double dx = std::atof(argv[3]);
    const double dy = std::atof(argv[4]);
    const int hz = std::atoi(argv[5]);
    const double secs = std::atof(argv[6]);

    // Screen geometry, for the screen-pixel -> tablet-unit map. XWayland reports the same output
    // size the compositor uses, and KWin maps the tablet to the primary output.
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        std::fprintf(stderr, "no X display (need $DISPLAY, XWayland is fine)\n");
        return 6;
    }
    double sw = DisplayWidth(dpy, DefaultScreen(dpy));
    double sh = DisplayHeight(dpy, DefaultScreen(dpy));
    XCloseDisplay(dpy);
    if (argc >= 9) { // the tablet's output, which on KWin is the PRIMARY one, not the desktop
        sw = std::atof(argv[7]);
        sh = std::atof(argv[8]);
    }
    std::printf("screen %.0fx%.0f; stroke (%.0f,%.0f) -> (%.0f,%.0f) at %d Hz over %.2f s\n", sw,
                sh, cx, cy, cx + dx, cy + dy, hz, secs);

    const auto toX = [sw](double px) {
        return static_cast<std::int32_t>(px / sw * VirtualPen::kMaxX);
    };
    const auto toY = [sh](double py) {
        return static_cast<std::int32_t>(py / sh * VirtualPen::kMaxY);
    };

    VirtualPen pen;
    std::string err;
    if (!pen.create(err)) {
        std::fprintf(stderr, "uinput: %s\n", err.c_str());
        return 4;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(800)); // udev + libinput + the compositor

    // Hover onto the canvas first: proximity_in, then dwell with a live jitter (an event-silent
    // in-proximity tool is forced out of proximity after ~50 ms).
    pen.proximityIn(toX(cx), toY(cy));
    pen.sync();
    for (int i = 0; i < 20; ++i) {
        pen.move(toX(cx) + (i % 2), toY(cy));
        pen.sync();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    pen.pressure(VirtualPen::kMaxPressure / 2);
    pen.tipDown();
    pen.sync();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    const int steps = static_cast<int>(secs * hz);
    const auto period = std::chrono::microseconds(1'000'000 / (hz > 0 ? hz : 200));
    auto next = std::chrono::steady_clock::now();
    for (int i = 1; i <= steps; ++i) {
        next += period;
        const double f = static_cast<double>(i) / steps;
        pen.move(toX(cx + dx * f), toY(cy + dy * f));
        pen.pressure(VirtualPen::kMaxPressure / 2); // constant: anything reading 1.0 is synthesized
        pen.sync();
        std::this_thread::sleep_until(next);
    }
    pen.pressure(0);
    pen.tipUp();
    pen.sync();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    pen.proximityOut();
    pen.sync();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::printf("stroke emitted: %d samples\n", steps);
    return 0;
}
