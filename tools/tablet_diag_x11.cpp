// S19 -- the JAGGED STROKE diagnostic (X11/XI2, the shipped default path).
//
// The user reports that a tablet stroke on X11 comes out staircased, while the SAME pen on native
// Wayland is smooth. Three suspects were named; this probe measures all three at once, against the
// REAL ui::TabletInput (not a re-implementation of it), driven by the uinput virtual stylus so the
// events cross udev / libinput / KWin / XWayland exactly as hardware does.
//
//   S1  THE SYNTHESIZED FALLBACK. TabletInput::drain() feeds one synthesized sample at the INTEGER
//       FLTK position with PRESSURE 1.0 whenever the ring comes up empty. On X11 the core pointer
//       stream arrives AHEAD of the XI2 stream carrying the same motion, so the ring goes empty
//       mid-stroke -- and every such FL_DRAG injects a full-pressure, pixel-snapped dab into the
//       middle of a real stroke. The pen here strokes at a CONSTANT pressure well away from 1.0, so
//       any sample that arrives reading exactly 1.0 IS a synthesized one. That count is the verdict.
//
//   S2  MOTION COMPRESSION. If the X server compresses XI2 motion down to the core pointer's rate,
//       the stroke is a ~60-point polyline no matter what the client does. Measured as REAL samples
//       delivered per second of stroke, against the 200 Hz the pen actually emits.
//
//   S3  SUB-PIXEL LOSS. tablet_x11.cpp takes position from ev.event_x/event_y and claims "sub-pixel
//       doubles straight from the wire". If the server hands back the screen-mapped, integer-
//       quantised pointer position instead, every dab snaps to a pixel -- which is literally a
//       staircase. Measured as the fraction of real samples whose position has a zero fractional
//       part. (At GUI scale 1 the ingest divide is a no-op and cannot manufacture a fraction.)
//
// Run inside the session (needs $DISPLAY and /dev/uinput access):
//   cmake --preset linux-debug -DMOSAIC_BUILD_TABLET_SPIKE=ON
//   cmake --build build/linux-debug --target tablet_diag_x11
//   ./build/linux-debug/bin/tablet_diag_x11
//
// Exit codes: 0 = measured; 4 = no uinput; 5 = the pen never found the window; 6 = no X11 backend.

#include "core/brush/stroke_state.hpp"
#include "ui/tablet_input.hpp"
#include "virtual_pen.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

using mosaic::spike::VirtualPen;

constexpr int kWinW = 900, kWinH = 700;
constexpr double kStrokePressure = 0.5;  // constant, and NOT 1.0 -- see S1
constexpr int kPenHz = 200;              // what the device emits
double g_strokeSeconds = 1.5;   // argv[2]
double g_strokeSpan = 0.30;     // argv[3]: fraction of the tablet swept (line mode)
bool g_arc = true;              // argv[4]: 0 = straight line, 1 = ARC
constexpr double kArcRadiusPx = 160.0;

// Fit a circle to the received points (Kasa's algebraic fit) and return the RMS radial residual --
// how far the stream strays from the circle the pen actually drew. A straight-line stroke cannot
// show a wobble on a CURVE, and it is a curve the user's strokes wobble on.
struct CircleFit {
    double cx = 0, cy = 0, r = 0, rms = 0, worst = 0;
};
[[nodiscard]] CircleFit fitCircle(const std::vector<std::pair<double, double>>& pts) {
    CircleFit f;
    const auto n = static_cast<double>(pts.size());
    if (n < 4)
        return f;
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0, sxz = 0, syz = 0, sz = 0;
    for (const auto& [x, y] : pts) {
        const double z = x * x + y * y;
        sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y;
        sxz += x * z; syz += y * z; sz += z;
    }
    const double a11 = 2 * (sxx - sx * sx / n), a12 = 2 * (sxy - sx * sy / n);
    const double a22 = 2 * (syy - sy * sy / n);
    const double b1 = sxz - sx * sz / n, b2 = syz - sy * sz / n;
    const double det = a11 * a22 - a12 * a12;
    if (std::abs(det) < 1e-9)
        return f;
    f.cx = (b1 * a22 - b2 * a12) / det;
    f.cy = (a11 * b2 - a12 * b1) / det;
    double rsum = 0;
    for (const auto& [x, y] : pts)
        rsum += std::sqrt((x - f.cx) * (x - f.cx) + (y - f.cy) * (y - f.cy));
    f.r = rsum / n;
    double acc = 0;
    for (const auto& [x, y] : pts) {
        const double d = std::sqrt((x - f.cx) * (x - f.cx) + (y - f.cy) * (y - f.cy)) - f.r;
        acc += d * d;
        f.worst = std::max(f.worst, std::abs(d));
    }
    f.rms = std::sqrt(acc / n);
    return f;
}

[[nodiscard]] std::uint64_t nowUs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

struct Diag {
    mosaic::ui::TabletInput tablet;
    VirtualPen pen;

    // steering feedback (written by the FLTK thread, read by the pen thread)
    std::atomic<int> lastMoveX{-1}, lastMoveY{-1};
    std::atomic<bool> steered{false};
    std::atomic<bool> pushSeen{false};
    // ⚠ Only samples from the ARC ITSELF may enter the fit. The tip-down retry jitters the pen IN
    // PLACE, with the tip already down -- so those samples arrive as FL_DRAGs and pile up as a dense
    // cluster at one point. Fitting a circle through that cluster skews the fit, and skews it WORST
    // on a fast stroke, where the arc contributes fewest points. It made the harness accuse the
    // backend of a wobble that was the harness's own.
    std::atomic<bool> arcRunning{false};
    std::atomic<bool> strokeDone{false};
    std::atomic<bool> penFailed{false};

    // the measurements
    int flPush = 0, flDrag = 0, flMove = 0, flRelease = 0;
    int dragsWithZeroReal = 0;   // FL_DRAGs whose only sample was the synthesized one
    int realSamples = 0;         // pressure != 1.0 -- the device's own
    int fakeSamples = 0;         // pressure == 1.0 exactly -- synthesized by the fallback (S1)
    int integralPositions = 0;   // real samples landing on a whole pixel (S3)
    std::uint64_t strokeStartUs = 0, strokeEndUs = 0;
    std::uint64_t firstRealUs = 0, lastRealUs = 0; // the stroke's real span, not FLTK's
    double rateAtEnd = 0.0;
    std::vector<double> firstFracs;             // a few raw fractional parts, for the eyeball
    std::vector<std::pair<double, double>> path; // every real sample: the stroke's actual shape
};

Diag g;

// Simulated paint load, in milliseconds, burned inside the FL_DRAG handler (argv[1]). The idle
// window was the wrong experiment: the REAL canvas composites and presents on every drag, so the X
// queue backs up behind it -- and a backlog is the only condition under which the client and the
// server get to disagree about what to do with the events in it.
int g_loadMs = 0;

void burn(int ms) {
    if (ms <= 0)
        return;
    const std::uint64_t until = nowUs() + static_cast<std::uint64_t>(ms) * 1000;
    while (nowUs() < until) { /* spin: a sleep would yield the CPU and hide the queueing */
    }
}

// A synthesized sample is pressure EXACTLY 1.0 (the fallback writes the literal); the device is
// driven at kStrokePressure and cannot land there.
[[nodiscard]] bool isSynthesized(const mosaic::core::brush::StrokeInput& in) {
    return in.pressure >= 0.999999;
}

class DiagWindow : public Fl_Double_Window {
public:
    DiagWindow() : Fl_Double_Window(kWinW, kWinH, "Mosaic jagged-stroke diagnostic (X11)") {
        color(FL_DARK3);
    }

    int handle(int ev) override {
        switch (ev) {
        case FL_ENTER:
            return 1;
        case FL_MOVE:
            ++g.flMove;
            g.lastMoveX.store(Fl::event_x());
            g.lastMoveY.store(Fl::event_y());
            g.tablet.discardBuffered(); // exactly what the canvas does on a hover
            return 1;
        case FL_PUSH:
            ++g.flPush;
            g.strokeStartUs = nowUs();
            g.pushSeen.store(true);
            return 1;
        case FL_DRAG: {
            ++g.flDrag;
            int real = 0;
            g.tablet.drain(static_cast<double>(Fl::event_x()), static_cast<double>(Fl::event_y()),
                           [&real](const mosaic::core::brush::StrokeInput& in) {
                               if (isSynthesized(in)) {
                                   ++g.fakeSamples;
                                   return;
                               }
                               ++real;
                               ++g.realSamples;
                               if (g.firstRealUs == 0)
                                   g.firstRealUs = nowUs();
                               g.lastRealUs = nowUs();
                               const double fx = std::abs(in.pos.x - std::floor(in.pos.x));
                               const double fy = std::abs(in.pos.y - std::floor(in.pos.y));
                               if (fx == 0.0 && fy == 0.0)
                                   ++g.integralPositions;
                               if (g.firstFracs.size() < 12)
                                   g.firstFracs.push_back(fx);
                               if (g.arcRunning.load())
                                   g.path.emplace_back(in.pos.x, in.pos.y);
                           });
            if (real == 0)
                ++g.dragsWithZeroReal;
            burn(g_loadMs); // stand in for composite + present
            return 1;
        }
        case FL_RELEASE:
            ++g.flRelease;
            g.strokeEndUs = nowUs();
            g.rateAtEnd = g.tablet.sampleRateHz();
            return 1;
        default:
            return Fl_Double_Window::handle(ev);
        }
    }
};

// The pen thread. Steers to the window centre off the FL_MOVE feedback the FLTK thread publishes,
// then lays one long straight stroke at a constant pressure and a constant 200 Hz.
void penThread(int targetX, int targetY) {
    std::string err;
    if (!g.pen.create(err)) {
        std::fprintf(stderr, "virtual pen: %s\n", err.c_str());
        g.penFailed.store(true);
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(600)); // let udev/XWayland notice it

    // Tablet units per screen pixel: the tablet spans the primary output.
    int sx = 0, sy = 0, sw = 0, sh = 0;
    Fl::screen_xywh(sx, sy, sw, sh, 0);
    const double gainX = static_cast<double>(VirtualPen::kMaxX) / (sw > 0 ? sw : 1920);
    const double gainY = static_cast<double>(VirtualPen::kMaxY) / (sh > 0 ? sh : 1080);

    std::int32_t px = VirtualPen::kMaxX / 2, py = VirtualPen::kMaxY / 2;
    g.pen.proximityIn(px, py);
    g.pen.sync();

    // Closed-loop steer: nudge, read where FLTK says we landed, correct.
    for (int i = 0; i < 400; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const int mx = g.lastMoveX.load(), my = g.lastMoveY.load();
        if (mx < 0) { // not over our window yet -- sweep
            px = static_cast<std::int32_t>(px + 0.02 * VirtualPen::kMaxX);
            if (px > VirtualPen::kMaxX) {
                px = 0;
                py = static_cast<std::int32_t>(py + 0.05 * VirtualPen::kMaxY);
                if (py > VirtualPen::kMaxY)
                    py = 0;
            }
            g.pen.move(px, py);
            g.pen.sync();
            continue;
        }
        const double ex = targetX - mx, ey = targetY - my;
        if (std::abs(ex) < 6 && std::abs(ey) < 6) {
            g.steered.store(true);
            break;
        }
        px = static_cast<std::int32_t>(px + 0.6 * gainX * ex);
        py = static_cast<std::int32_t>(py + 0.6 * gainY * ey);
        g.pen.move(px, py);
        g.pen.sync();
    }
    if (!g.steered.load()) {
        std::fprintf(stderr, "pen never steered onto the window\n");
        g.strokeDone.store(true);
        return;
    }

    // THE STROKE: tip down, constant pressure, a straight sweep to the right at 200 Hz.
    //
    // Retried until FLTK actually reports the press. A single tip-down report is NOT reliable here:
    // an event-silent in-proximity tool is forced OUT of proximity after ~50 ms (§4 finding 5), so
    // the convergence dwell can quietly drop the tool before BTN_TOUCH lands -- and a run that
    // strokes without ever pressing measures nothing while looking like it measured something.
    const std::int32_t downPressure =
        static_cast<std::int32_t>(kStrokePressure * VirtualPen::kMaxPressure);
    for (int i = 0; i < 40 && !g.pushSeen.load(); ++i) {
        g.pen.move(px + (i % 2), py); // jitter: keep the tool alive through the dwell
        g.pen.pressure(downPressure);
        g.pen.tipDown();
        g.pen.sync();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!g.pushSeen.load()) {
        std::fprintf(stderr, "the tip never pressed\n");
        g.strokeDone.store(true);
        return;
    }

    const int steps = static_cast<int>(g_strokeSeconds * kPenHz);
    const double dx = (g_strokeSpan * VirtualPen::kMaxX) / steps;
    // The ARC: a half circle of kArcRadiusPx SCREEN pixels, centred so the pen starts where it
    // already is. gainX/gainY convert screen px to tablet units (they differ per axis, so a circle
    // in pixels is an ellipse in tablet units -- which is exactly the conversion the app must undo).
    const double ax0 = px - kArcRadiusPx * gainX; // centre
    const double ay0 = py;
    const auto periodUs = std::chrono::microseconds(1'000'000 / kPenHz);
    auto next = std::chrono::steady_clock::now();
    g.arcRunning.store(true); // from here on, and only from here, the samples are the arc's
    for (int i = 0; i < steps; ++i) {
        next += periodUs;
        if (g_arc) {
            const double th = std::acos(-1.0) * static_cast<double>(i) / steps; // 0 .. pi
            px = static_cast<std::int32_t>(ax0 + kArcRadiusPx * gainX * std::cos(th));
            py = static_cast<std::int32_t>(ay0 + kArcRadiusPx * gainY * std::sin(th));
        } else {
            px = static_cast<std::int32_t>(px + dx);
        }
        g.pen.move(px, py);
        g.pen.pressure(static_cast<std::int32_t>(kStrokePressure * VirtualPen::kMaxPressure));
        g.pen.sync();
        std::this_thread::sleep_until(next);
    }
    g.arcRunning.store(false);
    g.pen.pressure(0);
    g.pen.tipUp();
    g.pen.sync();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    g.pen.proximityOut();
    g.pen.sync();
    g.strokeDone.store(true);
}

void report() {
    // The span between the first and last REAL sample -- FL_RELEASE is not reliably delivered
    // before the process tears the pen down, and a zero-length span made every rate below read 0.
    const double secs = (g.lastRealUs > g.firstRealUs)
                            ? static_cast<double>(g.lastRealUs - g.firstRealUs) / 1e6
                            : 0.0;
    const double realHz = secs > 0.0 ? g.realSamples / secs : 0.0;
    const double dragHz = secs > 0.0 ? g.flDrag / secs : 0.0;
    const int emitted = static_cast<int>(g_strokeSeconds * kPenHz);

    std::printf("\n================ X11 JAGGED-STROKE DIAGNOSTIC ================\n");
    std::printf("backend            : %s\n", g.tablet.backendName().c_str());
    std::printf("GUI scale          : %.2f\n",
                static_cast<double>(Fl::screen_scale(0)));
    std::printf("simulated paint load per FL_DRAG: %d ms\n", g_loadMs);
    std::printf("stroke             : %.2f s, pen emitting %d Hz at pressure %.2f (%d samples)\n",
                secs, kPenHz, kStrokePressure, emitted);
    std::printf("FLTK events        : PUSH=%d DRAG=%d MOVE=%d RELEASE=%d\n", g.flPush, g.flDrag,
                g.flMove, g.flRelease);
    std::printf("\n");
    std::printf("S0 SAMPLE SURVIVAL (of the %d the pen emitted, how many reached the stroke?)\n",
                emitted);
    std::printf("   reached the stroke : %d\n", g.realSamples);
    std::printf("   LOST               : %d (%.0f%%)\n", emitted - g.realSamples,
                100.0 * (emitted - g.realSamples) / emitted);
    std::printf("   verdict: %s\n",
                g.realSamples < 0.75 * emitted
                    ? "CONFIRMED -- device samples are being DROPPED before the engine sees them"
                    : "not reproduced -- the samples survive");
    std::printf("\n");

    std::printf("S1 SYNTHESIZED FALLBACK (drain() with an empty ring)\n");
    std::printf("   real samples fed to the stroke : %d\n", g.realSamples);
    std::printf("   SYNTHESIZED pressure-1.0 dabs  : %d   <-- injected into a real stroke\n",
                g.fakeSamples);
    std::printf("   FL_DRAGs that drained ZERO real: %d of %d\n", g.dragsWithZeroReal, g.flDrag);
    std::printf("   verdict: %s\n",
                g.fakeSamples > 0
                    ? "CONFIRMED -- the fallback fires mid-stroke and stamps full-pressure, "
                      "pixel-snapped dabs"
                    : "not reproduced");
    std::printf("\n");

    std::printf("S2 MOTION COMPRESSION (is the server throttling XI2 to the core rate?)\n");
    std::printf("   real samples/s delivered : %.1f Hz   (pen emits %d Hz)\n", realHz, kPenHz);
    std::printf("   FL_DRAG rate             : %.1f Hz\n", dragHz);
    std::printf("   TabletInput::sampleRateHz(): %.1f Hz\n", g.rateAtEnd);
    std::printf("   verdict: %s\n",
                realHz < 0.6 * kPenHz
                    ? "CONFIRMED -- samples are being lost/compressed before we see them"
                    : "not reproduced -- the samples ARE arriving");
    std::printf("\n");

    // S4 is the one that found the bug. S1-S3 all came back NEGATIVE against the real TabletInput:
    // nothing is dropped, nothing is compressed, and the positions do carry a fraction. What is
    // wrong is the fraction itself -- event_x/event_y is the SERVER's screen-mapped pointer, and its
    // sub-pixel part is an artefact of that mapping rather than the pen's location. The pen here
    // strokes a DEAD STRAIGHT LINE, so every bit of deviation from one is the backend's.
    if (g_arc) {
        // ⚠ THE METRIC THAT MATTERS. A straight line proves nothing: the flattener emits no interior
        // points for one, so the curve path is not even exercised -- and it is on CURVES that the
        // user sees the wobble.
        const CircleFit f = fitCircle(g.path);
        std::printf("S4 ARC FIDELITY (the pen drew a half circle -- did we?)\n");
        std::printf("   fitted circle    : centre (%.1f, %.1f) r=%.2f px\n", f.cx, f.cy, f.r);
        std::printf("   radial deviation : %.4f px RMS, %.3f px worst\n", f.rms, f.worst);
        std::printf("   verdict: %s\n",
                    f.rms > 0.05 ? "WOBBLE -- the stream does not follow the circle the pen drew"
                                 : "CLEAN");
        if (const char* dump = std::getenv("MOSAIC_ARC_DUMP")) {
            if (std::FILE* fp = std::fopen(dump, "w")) {
                for (const auto& [x, y] : g.path)
                    std::fprintf(fp, "%.6f %.6f %.6f\n", x, y,
                                 std::sqrt((x - f.cx) * (x - f.cx) + (y - f.cy) * (y - f.cy)) - f.r);
                std::fclose(fp);
            }
        }
        std::printf("\n");
    }
    const auto pd = g.tablet.positionDiag();
    if (pd.fromValuators + pd.fromServer > 0) {
        std::printf("S5 POSITION SOURCE (X11: the device's valuators, or the server's pointer?)\n");
        std::printf("   from the DEVICE's valuators : %zu\n", pd.fromValuators);
        std::printf("   from the SERVER's pointer   : %zu   <-- these carry the wobble\n",
                    pd.fromServer);
        std::printf("   trusted device kicked out   : %d times (worst delta %.2f px)\n",
                    pd.untrusts, pd.worstUntrustDelta);
        std::printf("   verdict: %s\n",
                    pd.untrusts > 0
                        ? "MIXED MID-STROKE -- the guard is unseating a device whose mapping is fine"
                        : "consistent");
        std::printf("\n");
    }
    std::printf("S4b STRAIGHT-LINE FIDELITY (only meaningful in line mode)\n");
    if (g.path.size() >= 3) {
        const auto n = static_cast<double>(g.path.size());
        double mx = 0, my = 0;
        for (const auto& [x, y] : g.path) {
            mx += x / n;
            my += y / n;
        }
        double sxx = 0, sxy = 0;
        for (const auto& [x, y] : g.path) {
            sxx += (x - mx) * (x - mx);
            sxy += (x - mx) * (y - my);
        }
        const double m = sxx > 0 ? sxy / sxx : 0.0;
        const double b = my - m * mx;
        double acc = 0, worst = 0;
        int flips = 0;
        double prev = 0;
        for (const auto& [x, y] : g.path) {
            const double r = (y - (m * x + b)) / std::sqrt(1 + m * m);
            acc += r * r;
            worst = std::max(worst, std::abs(r));
            if (prev * r < 0)
                ++flips;
            prev = r;
        }
        const double rms = std::sqrt(acc / n);
        std::printf("   deviation from the straight line : %.4f px RMS, %.3f px worst\n", rms,
                    worst);
        std::printf("   direction changes (a sawtooth)   : %d of %zu samples\n", flips,
                    g.path.size() - 1);
        std::printf("   verdict: %s\n",
                    rms > 0.1 ? "JAGGED -- the position stream is not the pen's path"
                              : "CLEAN -- matches what the same pen produces on native Wayland "
                                "(0.011 px RMS)");
    }
    std::printf("\n");

    std::printf("S3 SUB-PIXEL LOSS (does event_x really carry a fraction?)\n");
    std::printf("   real samples on a WHOLE pixel : %d of %d\n", g.integralPositions,
                g.realSamples);
    std::printf("   first fractional parts        : ");
    for (const double f : g.firstFracs)
        std::printf("%.3f ", f);
    std::printf("\n   verdict: %s\n",
                (g.realSamples > 0 && g.integralPositions == g.realSamples)
                    ? "CONFIRMED -- every position is integer-quantised; the sub-pixel claim is FALSE"
                    : "not reproduced -- positions carry a fraction");
    std::printf("==============================================================\n");
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    // Default to X11 (the shipped path), but let the caller pick -- the whole point is comparing the
    // two backends with ONE harness.
    if (std::getenv("FLTK_BACKEND") == nullptr)
        setenv("FLTK_BACKEND", "x11", 1);
    if (argc > 1)
        g_loadMs = std::atoi(argv[1]);
    if (argc > 2)
        g_strokeSeconds = std::atof(argv[2]);
    if (argc > 3)
        g_strokeSpan = std::atof(argv[3]);
    if (argc > 4)
        g_arc = std::atoi(argv[4]) != 0;

    DiagWindow win;
    win.end();
    win.show();
    for (int i = 0; i < 50 && !win.shown(); ++i)
        Fl::wait(0.05);
    Fl::wait(0.1);

    g.tablet.init(&win);
    if (g.tablet.backendName().empty()) {
        std::fprintf(stderr, "no X11 tablet backend came up\n");
        return 6;
    }
    std::printf("backend up: %s, %zu device(s)\n", g.tablet.backendName().c_str(),
                g.tablet.devices().size());
    for (const auto& d : g.tablet.devices())
        std::printf("  device: '%s' tool=%s valuators=[%s]\n", d.name.c_str(), d.tool.c_str(),
                    d.valuators.c_str());

    std::thread pen(penThread, kWinW / 2, kWinH / 2);

    const std::uint64_t deadline = nowUs() + 40'000'000;
    while (!g.strokeDone.load() && !g.penFailed.load() && nowUs() < deadline)
        Fl::wait(0.01);
    Fl::wait(0.05); // let the tail of the event stream land
    pen.join();

    if (g.penFailed.load())
        return 4;
    if (g.flPush == 0) {
        std::fprintf(stderr, "the pen never pressed on our window (steered=%d, moves=%d)\n",
                     static_cast<int>(g.steered.load()), g.flMove);
        return 5;
    }
    report();
    return 0;
}
