// Is the X server's event_x/event_y the DEVICE's position, or its own screen-mapped guess?
//
// Measured (S19, 2026-07-11): a pen driven along a dead-straight line reaches the brush engine with
// a 0.49 px RMS sawtooth on X11 and 0.01 px on native Wayland -- 43x worse, sign-flipping on 4 of
// every 5 samples. Nothing is dropped and nothing is synthesized; the POSITIONS are wrong.
//
// tablet_x11.cpp takes position from `ev.event_x/event_y` and calls them "sub-pixel doubles straight
// from the wire". They do carry a fraction -- so the claim is not obviously false -- but a fraction
// is not the same thing as the DEVICE's position. This probe prints, for the same event, both:
//
//    event_x/event_y   what the backend uses now: the SCREEN-mapped pointer position
//    Abs X / Abs Y     the raw VALUATORS: the tablet's own coordinates, at its own resolution
//                      (0..44704 here -- ~23 device units per screen pixel)
//
// and fits a straight line to each, so the residual says which one is faithful. If the valuators are
// clean, the fix is to take position from THEM, and the sawtooth is a server artefact we have been
// faithfully reproducing.
//
// ⚠ It maps a window of its own and selects on THAT, not on the root. Two reasons, both measured:
// under KWin the desktop is native Wayland, so XWayland only synthesizes X tablet events while the
// pen is over an X window at all; and XI2 delivery stops at the deepest window with a selection, so
// a root selection sees nothing once a real client (Mosaic) has selected on its own window.
//   ./bin/xi2_valuator_probe & ./bin/pen_driver 700 500 500 120 200 1.5 1920 1080

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

// The probe's own window: the pen must stroke inside it.
constexpr int kWinX = 400, kWinY = 300, kWinW = 1000, kWinH = 500;

struct Row {
    double ex, ey; // event_x / event_y  -- the server's screen-mapped position (window-relative)
    double vx, vy; // Abs X / Abs Y      -- the device's own valuators
    double rx, ry; // root_x / root_y    -- the same server position, on the SCREEN
};

// Perpendicular RMS deviation from the best-fit straight line through the samples. The pen drives a
// straight line, so every bit of this is error.
double lineRms(const std::vector<double>& xs, const std::vector<double>& ys) {
    const auto n = static_cast<double>(xs.size());
    if (n < 3)
        return 0.0;
    double mx = 0, my = 0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        mx += xs[i] / n;
        my += ys[i] / n;
    }
    double sxx = 0, sxy = 0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        sxx += (xs[i] - mx) * (xs[i] - mx);
        sxy += (xs[i] - mx) * (ys[i] - my);
    }
    if (!(sxx > 0.0))
        return 0.0;
    const double m = sxy / sxx;
    const double b = my - m * mx;
    double acc = 0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        const double r = (ys[i] - (m * xs[i] + b)) / std::sqrt(1 + m * m);
        acc += r * r;
    }
    return std::sqrt(acc / n);
}

} // namespace

int main() {
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        std::fprintf(stderr, "no display\n");
        return 6;
    }
    int opcode = 0, ev = 0, err = 0;
    if (!XQueryExtension(dpy, "XInputExtension", &opcode, &ev, &err)) {
        std::fprintf(stderr, "no XInput2\n");
        return 6;
    }
    int major = 2, minor = 2;
    XIQueryVersion(dpy, &major, &minor);
    const int scrW = DisplayWidth(dpy, DefaultScreen(dpy));

    const Atom aAbsX = XInternAtom(dpy, "Abs X", True);
    const Atom aAbsY = XInternAtom(dpy, "Abs Y", True);
    const Atom aPressure = XInternAtom(dpy, "Abs Pressure", True);

    // Every stylus, by device id -> which valuator NUMBER carries Abs X and Abs Y. (The values
    // array is packed by SET-BIT order, not by axis number, so the number is what we must match.)
    //
    // Re-enumerated on XI_HierarchyChanged, and that is not optional: the virtual pen is plugged in
    // AFTER this probe starts, and XWayland mints a NEW X device for it. Enumerating once at startup
    // watched a device the pen never used, and the probe saw zero events.
    struct Axes {
        int numX, numY;
        double minX, maxX, minY, maxY; // the DECLARED range: the honest device->pixel scale
    };
    std::vector<std::pair<int, Axes>> styli;
    const auto enumerate = [&] {
        styli.clear();
        int cnt = 0;
        XIDeviceInfo* devs = XIQueryDevice(dpy, XIAllDevices, &cnt);
        for (int i = 0; i < cnt; ++i) {
            if (devs[i].use != XISlavePointer && devs[i].use != XIFloatingSlave)
                continue;
            // The STYLUS only. The eraser and the puck are separate X devices that also carry
            // Abs X/Y, and folding their events into one line fit corrupts both residuals.
            const std::string name = devs[i].name != nullptr ? devs[i].name : "";
            if (name.find("stylus") == std::string::npos && name.find("Pen") == std::string::npos)
                continue;
            bool hasPressure = false;
            int gotX = -1, gotY = -1;
            Axes ax{-1, -1, 0, 0, 0, 0};
            for (int c = 0; c < devs[i].num_classes; ++c) {
                if (devs[i].classes[c]->type != XIValuatorClass)
                    continue;
                const auto* v = reinterpret_cast<XIValuatorClassInfo*>(devs[i].classes[c]);
                if (v->label == aPressure)
                    hasPressure = true;
                else if (v->label == aAbsX) {
                    gotX = v->number;
                    ax.minX = v->min;
                    ax.maxX = v->max;
                } else if (v->label == aAbsY) {
                    gotY = v->number;
                    ax.minY = v->min;
                    ax.maxY = v->max;
                }
            }
            if (hasPressure && gotX >= 0 && gotY >= 0) {
                ax.numX = gotX;
                ax.numY = gotY;
                styli.push_back({devs[i].deviceid, ax});
                std::printf("stylus: '%s' (id %d), Abs X = val %d [%.0f..%.0f], Abs Y = val %d "
                            "[%.0f..%.0f]\n",
                            devs[i].name, devs[i].deviceid, gotX, ax.minX, ax.maxX, gotY, ax.minY,
                            ax.maxY);
            }
        }
        XIFreeDeviceInfo(devs);
        std::fflush(stdout);
    };
    enumerate();

    // Our own window, under where the pen will stroke (see the header: root selection sees nothing).
    const Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), kWinX, kWinY, kWinW, kWinH,
                                           0, 0, 0x202020);
    XStoreName(dpy, win, "xi2 valuator probe");
    XMapRaised(dpy, win);
    XFlush(dpy);

    unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {};
    XISetMask(bits, XI_Motion);
    XISetMask(bits, XI_HierarchyChanged);
    XIEventMask mask{XIAllDevices, sizeof(bits), bits};
    XISelectEvents(dpy, win, &mask, 1);
    XFlush(dpy);
    std::printf("listening for stylus motion...\n");
    std::fflush(stdout);

    std::vector<Row> rows;
    double rangeX = 0, rangeY = 0;
    long bothAxes = 0, xOnly = 0, yOnly = 0, neither = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    while (std::chrono::steady_clock::now() < deadline && rows.size() < 4000) {
        if (XPending(dpy) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        XEvent e;
        XNextEvent(dpy, &e);
        if (e.type != GenericEvent || e.xcookie.extension != opcode)
            continue;
        if (!XGetEventData(dpy, &e.xcookie))
            continue;
        if (e.xcookie.evtype == XI_HierarchyChanged) {
            XFreeEventData(dpy, &e.xcookie);
            enumerate(); // the pen was plugged in after we started; XWayland minted a new device
            continue;
        }
        if (e.xcookie.evtype == XI_Motion) {
            const auto* de = static_cast<XIDeviceEvent*>(e.xcookie.data);
            const Axes* ax = nullptr;
            for (const auto& [id, a] : styli)
                if (id == de->deviceid)
                    ax = &a;
            if (ax != nullptr) {
                rangeX = (ax->maxX - ax->minX);
                rangeY = (ax->maxY - ax->minY);
                Row r{de->event_x, de->event_y, 0, 0, de->root_x, de->root_y};
                bool gx = false, gy = false;
                int k = 0;
                for (int i = 0; i < de->valuators.mask_len * 8; ++i) {
                    if (!XIMaskIsSet(de->valuators.mask, i))
                        continue;
                    const double v = de->valuators.values[k++];
                    if (i == ax->numX) {
                        r.vx = v;
                        gx = true;
                    } else if (i == ax->numY) {
                        r.vy = v;
                        gy = true;
                    }
                }
                // ⚠ Which axes did this event actually CARRY? XI2 sends only the valuators that
                // CHANGED. If Abs X and Abs Y arrive in SEPARATE events, a client that remembers the
                // last value of each (as it must) pairs a fresh X with a STALE Y -- and the point it
                // reconstructs sits off the true path by roughly one sample of travel. That error is
                // proportional to SPEED, which is exactly the fingerprint we are chasing.
                if (gx && gy) ++bothAxes;
                else if (gx) ++xOnly;
                else if (gy) ++yOnly;
                else ++neither;
                if (gx && gy)
                    rows.push_back(r);
            }
        }
        XFreeEventData(dpy, &e.xcookie);
    }
    XCloseDisplay(dpy);

    if (rows.size() < 20) {
        std::fprintf(stderr, "only %zu samples; run pen_driver alongside\n", rows.size());
        return 5;
    }

    std::vector<double> ex, ey, vx, vy;
    for (const Row& r : rows) {
        ex.push_back(r.ex);
        ey.push_back(r.ey);
        vx.push_back(r.vx);
        vy.push_back(r.vy);
    }
    // Valuators -> pixels. The scale is REGRESSED out of the same events (px per device unit, from
    // event_x against Abs X) rather than assumed from the declared range and an output size: which
    // output the server maps the tablet onto is exactly the thing we do not know, and a scale that is
    // wrong by 2x would SHRINK the valuator residual by 2x and flatter the very claim under test.
    // A pure rescale cannot manufacture smoothness -- it moves the fitted line with the data -- so
    // once both series are in the same pixel units the residuals are directly comparable.
    const auto slope = [](const std::vector<double>& a, const std::vector<double>& b) {
        const auto n = static_cast<double>(a.size());
        double ma = 0, mb = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            ma += a[i] / n;
            mb += b[i] / n;
        }
        double saa = 0, sab = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            saa += (a[i] - ma) * (a[i] - ma);
            sab += (a[i] - ma) * (b[i] - mb);
        }
        return saa > 0 ? sab / saa : 1.0;
    };
    const double pxPerUnitX = slope(vx, ex); // screen px per device unit, measured
    const double pxPerUnitY = slope(vy, ey);
    std::vector<double> vxp, vyp;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        vxp.push_back(vx[i] * pxPerUnitX);
        vyp.push_back(vy[i] * pxPerUnitY);
    }
    const double unitsPerPx = pxPerUnitX != 0 ? 1.0 / pxPerUnitX : 0.0;
    std::printf("declared Abs X range %.0f; measured %.1f device units per screen pixel\n"
                "  -> the range spans %.0f px (X screen is %d px wide)\n"
                "declared Abs Y range %.0f -> spans %.0f px\n",
                rangeX, unitsPerPx, rangeX * pxPerUnitX, scrW, rangeY, rangeY * pxPerUnitY);

    const double rmsEvent = lineRms(ex, ey);
    const double rmsVal = lineRms(vxp, vyp);
    std::printf("\n=============== XI2 AXIS DELIVERY ===============\n");
    std::printf("  events carrying BOTH Abs X and Abs Y : %ld\n", bothAxes);
    std::printf("  events carrying only Abs X           : %ld\n", xOnly);
    std::printf("  events carrying only Abs Y           : %ld\n", yOnly);
    std::printf("  events carrying NEITHER              : %ld\n", neither);
    std::printf("  verdict: %s\n",
                (xOnly + yOnly) > 0
                    ? "STAGGERED -- a fresh X gets paired with a STALE Y (error grows with speed)"
                    : "both axes always arrive together");
    std::printf("\n=============== XI2 POSITION SOURCE ===============\n");
    std::printf("samples: %zu   (%.1f device units per screen pixel)\n", rows.size(), unitsPerPx);
    std::printf("  event_x/event_y (USED TODAY) : %.4f px RMS off the straight line\n", rmsEvent);
    std::printf("  Abs X / Abs Y   (VALUATORS)  : %.4f px RMS off the straight line\n", rmsVal);
    std::printf("  --> the valuators are %.0fx more faithful\n",
                rmsVal > 0 ? rmsEvent / rmsVal : 0.0);
    std::printf("first 6 samples (event vs valuator-in-px):\n");
    for (std::size_t i = 0; i < 6 && i < rows.size(); ++i)
        std::printf("   event(%8.3f, %8.3f)   valuator(%8.3f, %8.3f)\n", ex[i], ey[i], vxp[i],
                    vyp[i]);
    std::printf("===================================================\n");
    // Raw dump: the mapping must be fitted OFF THE MOVING SAMPLES ONLY. The pen dwells (jittering
    // in place) before it strokes, and that stationary cluster is high-leverage -- fitting through
    // it is what produced a bogus "the range spans 3776 px of a 3840 px screen".
    if (std::FILE* f = std::fopen("/tmp/xi2rows.txt", "w")) {
        for (const Row& r : rows)
            std::fprintf(f, "%.6f %.6f %.6f %.6f %.6f %.6f\n", r.vx, r.vy, r.ex, r.ey, r.rx, r.ry);
        std::fclose(f);
        std::printf("raw rows -> /tmp/xi2rows.txt (valX valY eventX eventY rootX rootY)\n");
    }
    return 0;
}
