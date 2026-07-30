// S19 Arc C, §10 step 5 -- the DYNAMICS WIRING. Two halves, both headless:
//
//  * ui::TabletStrokeGate -- the lifecycle guard the Wayland sink puts in front of the canvas. Pure
//    (no FLTK, no compositor), and it encodes two findings that are invisible when broken.
//  * BrushEngine driven by a real StrokeInput stream -- pressure finally reaching the dab walk
//    (the `{pt, 1.0}` literals in vulkan_canvas.cpp are gone), and the StrokeState the §6.2 option
//    pipeline will evaluate against.
//
// The device-to-sample half is already covered by test_tablet{,_x11,_wayland}.cpp; what is new here
// is what happens to a sample AFTER it is normalized.
#include "core/brush/brush_engine.hpp"
#include "core/brush/stroke_state.hpp"
#include "ui/tablet_input.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Widget.H>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

using mosaic::core::brush::BrushDynamics;
using mosaic::core::brush::BrushEngine;
using mosaic::core::brush::BrushParams;
using mosaic::core::brush::SpeedParams;
using mosaic::core::brush::StrokeInput;
using mosaic::ui::TabletPointerSynth;
using mosaic::ui::TabletStrokeGate;
namespace platform = mosaic::platform;

namespace {

// A white opaque layer to carve/paint into.
mosaic::common::Image layer(std::uint32_t w, std::uint32_t h) {
    mosaic::common::Image img;
    img.width = w;
    img.height = h;
    img.rgba.assign(static_cast<std::size_t>(w) * h * 4, std::uint8_t{255});
    return img;
}

// Count the pixels a stroke actually touched (coverage > 0), which is the raster's own answer to
// "how big were the dabs".
std::size_t covered(const BrushEngine& e) {
    std::size_t n = 0;
    for (float c : e.coverage())
        if (c > 0.0f)
            ++n;
    return n;
}

StrokeInput at(double x, double y, double pressure, std::uint64_t timeUs = 0) {
    StrokeInput in;
    in.pos = {x, y};
    in.pressure = pressure;
    in.timeUs = timeUs;
    return in;
}

} // namespace

// The canvas's own window id. Every gate call names the surface its sample came from, because the
// backend reads the pen over more than one of our windows (§8: the settings dialog's test area).
constexpr std::uint64_t kCanvas = 0x1001;
constexpr std::uint64_t kDialog = 0x2002;

TEST_CASE("TabletStrokeGate: a down outside our surface never becomes a stroke") {
    TabletStrokeGate g;
    g.setCanvasSurface(kCanvas);
    CHECK_FALSE(g.stroking());

    // The pen is over ANOTHER surface of this client -- the toolbar, a panel. zwp_tablet_v2 still
    // delivers down/motion/up to us, and the backend still dispatches them; only `inProximity` says
    // the proximity_in did not name our surface. Nothing may reach the canvas.
    CHECK_FALSE(g.begin(/*inProximity=*/false, kCanvas)); // <- without this, a toolbar press paints
    CHECK_FALSE(g.stroking());
    CHECK_FALSE(g.motion(kCanvas)); // ... and a down we refused cannot then move
    CHECK_FALSE(g.end());           // ... nor end
    CHECK_FALSE(g.stroking());
}

// The SECOND surface rule, and a different one: the pen IS in proximity, on a window that really is
// ours -- the Settings dialog, which the backend watches so the test area can read a hovering pen.
// Its samples are in the DIALOG's coordinates. A stroke fed from them would paint at wherever the
// dialog is sitting, in a stroke the user began by pressing the pen on a settings control.
TEST_CASE("TabletStrokeGate: an in-proximity down on ANOTHER of our windows never strokes") {
    TabletStrokeGate g;
    g.setCanvasSurface(kCanvas);

    CHECK_FALSE(g.begin(/*inProximity=*/true, kDialog));
    CHECK_FALSE(g.stroking());
    CHECK_FALSE(g.hover(kDialog)); // nor may it move the reticle, which lives on the canvas
    CHECK(g.hover(kCanvas));

    // And a stroke that IS live is not moved by a sample from elsewhere.
    CHECK(g.begin(true, kCanvas));
    CHECK_FALSE(g.motion(kDialog));
    CHECK(g.motion(kCanvas));
}

TEST_CASE("TabletStrokeGate: an in-proximity down strokes until it is ended") {
    TabletStrokeGate g;
    g.setCanvasSurface(kCanvas);
    CHECK(g.begin(/*inProximity=*/true, kCanvas));
    CHECK(g.stroking());
    CHECK(g.motion(kCanvas));
    CHECK(g.motion(kCanvas)); // motion does not consume the stroke
    CHECK(g.end());
    CHECK_FALSE(g.stroking());
    CHECK_FALSE(g.motion(kCanvas)); // ... and the stroke is spent
    CHECK_FALSE(g.end());           // a second up is not a second stroke end
}

TEST_CASE("TabletStrokeGate: an up out of proximity STILL ends the stroke") {
    // A pen flicked off the tablet delivers `up` and `proximity_out` in ONE frame, so the sample
    // carrying the up already reads out of proximity. Gating end() on proximity (rather than on the
    // stroke) would strand the stroke: the engine would never close, and the paint would never land
    // as a command.
    TabletStrokeGate g;
    g.setCanvasSurface(kCanvas);
    CHECK(g.begin(true, kCanvas));
    CHECK(g.end()); // end() takes neither proximity nor a surface, and that is the whole point
    CHECK_FALSE(g.stroking());
}

TEST_CASE("TabletStrokeGate: proximity-out mid-stroke drops the stroke") {
    TabletStrokeGate g;
    g.setCanvasSurface(kCanvas);
    CHECK(g.begin(true, kCanvas));
    g.proximityOut();
    CHECK_FALSE(g.stroking());
    CHECK_FALSE(g.motion(kCanvas));
    CHECK_FALSE(g.end()); // the canvas already closed it out on the proximity-out callback
}

TEST_CASE("BrushEngine: pressure drives size and flow -- the literals are gone") {
    BrushParams p;
    p.diameter = 40.0;
    p.flow = 1.0;
    p.opacity = 1.0;
    p.hardness = 1.0;

    BrushDynamics dyn; // what ui::VulkanCanvas::currentBrushDynamics now hands the Brush/Eraser
    dyn.sizeFromPressure = true;
    dyn.flowFromPressure = true;

    SUBCASE("a light touch lays a smaller dab than a heavy one") {
        mosaic::common::Image soft = layer(200, 200);
        BrushEngine a;
        a.begin(200, 200, soft, p, dyn, at(100, 100, 0.25));
        const std::size_t lightPixels = covered(a);

        mosaic::common::Image hard = layer(200, 200);
        BrushEngine b;
        b.begin(200, 200, hard, p, dyn, at(100, 100, 1.0));
        const std::size_t heavyPixels = covered(b);

        CHECK(lightPixels > 0);          // a light touch still paints -- it does not vanish
        CHECK(lightPixels < heavyPixels); // ... but it paints a smaller mark
    }

    SUBCASE("a light touch deposits less paint per dab (flow)") {
        mosaic::common::Image img = layer(200, 200);
        BrushEngine e;
        e.begin(200, 200, img, p, dyn, at(100, 100, 0.25));
        // Coverage at the dab centre IS the deposited flow for the first dab.
        const std::size_t cx = static_cast<std::size_t>(100 - e.coverageOriginX());
        const std::size_t cy = static_cast<std::size_t>(100 - e.coverageOriginY());
        const float c = e.coverage()[cy * e.coverageWidth() + cx];
        CHECK(c == doctest::Approx(0.25).epsilon(0.01)); // flow 1.0 * pressure 0.25
    }

    SUBCASE("pressure 1 is an EXACT identity -- every mouse stroke is byte-for-byte unchanged") {
        // The whole reason both flags can default ON for the Brush and the Eraser: a mouse always
        // passes pressure 1, and at pressure 1 both channels are multiplications by one. If this
        // ever drifts, every existing golden and the Uniform x Wash hard rule drift with it.
        mosaic::common::Image withDyn = layer(200, 200);
        BrushEngine a;
        a.begin(200, 200, withDyn, p, dyn, at(40, 40, 1.0));
        a.extendTo(at(160, 90, 1.0));
        a.extendTo(at(60, 150, 1.0));
        a.flush();
        a.composite();

        mosaic::common::Image withoutDyn = layer(200, 200);
        BrushEngine b;
        b.begin(200, 200, withoutDyn, p, BrushDynamics{}, at(40, 40, 1.0));
        b.extendTo(at(160, 90, 1.0));
        b.extendTo(at(60, 150, 1.0));
        b.flush();
        b.composite();

        REQUIRE(withDyn.rgba.size() == withoutDyn.rgba.size());
        CHECK(withDyn.rgba == withoutDyn.rgba); // byte-identical, not merely similar
    }
}

TEST_CASE("BrushEngine: the sample stream drives StrokeState (docs/brushes.md §6.2)") {
    BrushParams p;
    p.diameter = 10.0;
    mosaic::common::Image img = layer(400, 400);
    BrushEngine e;

    SpeedParams sp;
    sp.maxSpeed = 2.0;   // px/ms that reads as 1.0
    sp.windowMs = 10.0;  // a short window, so the EMA converges within the stroke
    e.setSpeedParams(sp);

    e.begin(400, 400, img, p, BrushDynamics{}, at(100.0, 100.0, 0.5, /*timeUs=*/0));

    SUBCASE("begin() seeds a still pen at the press") {
        CHECK(e.strokeState().distance() == doctest::Approx(0.0));
        CHECK(e.strokeState().elapsedMs() == doctest::Approx(0.0));
        CHECK(e.strokeState().speed() == doctest::Approx(0.0));
        CHECK(e.strokeState().maxPressure() == doctest::Approx(0.5));
        // setSpeedParams survives begin(): begin resets the EMA's VALUE, never its calibration.
        CHECK(e.strokeState().speedParams().maxSpeed == doctest::Approx(2.0));
        CHECK(e.strokeState().speedParams().windowMs == doctest::Approx(10.0));
    }

    SUBCASE("extendTo() accumulates distance, time, angle and the speed EMA") {
        // 100 px due east over 100 ms, in ten 10 px / 10 ms steps: exactly 1 px/ms.
        for (int i = 1; i <= 10; ++i)
            e.extendTo(at(100.0 + 10.0 * i, 100.0, 0.9, static_cast<std::uint64_t>(i) * 10'000));

        CHECK(e.strokeState().distance() == doctest::Approx(100.0));
        CHECK(e.strokeState().elapsedMs() == doctest::Approx(100.0));
        CHECK(e.strokeState().drawingAngle() == doctest::Approx(0.0)); // due east
        CHECK(e.strokeState().maxPressure() == doctest::Approx(0.9));  // the stroke's high-water mark
        // 1 px/ms against a 2 px/ms full scale -> 0.5, once the 10 ms EMA has caught up over 100 ms.
        CHECK(e.strokeState().speed() == doctest::Approx(0.5).epsilon(0.02));
    }

    SUBCASE("a sample that is too short to stamp still moves the clock") {
        // The state folds in BEFORE the dab walk's early-out, so a slowing pen keeps the speed EMA
        // and the stroke clock honest even when no dab is due.
        e.extendTo(at(100.0, 100.0, 0.5, 50'000)); // zero travel, 50 ms later
        CHECK(e.strokeState().elapsedMs() == doctest::Approx(50.0));
        CHECK(e.strokeState().distance() == doctest::Approx(0.0));
    }
}

// ---------------------------------------------------------------------------------------------
// TabletPointerSynth -- the pen, made into an FLTK pointer (docs/tablet.md §4 finding 4)
// ---------------------------------------------------------------------------------------------
//
// On native Wayland the compositor stops emulating pointer events for the pen the moment we bind the
// tablet manager, so FLTK hears NOTHING from it. The pen could paint the canvas and do nothing else
// in the program: no toolbar, no menus, no dialogs, and no hover -- with no FL_MOVE, the reticle
// never followed a hovering pen. The wiring now hands FLTK the events the compositor declined to
// send, and this is what proves they land where the pen actually is.
//
// Headless: an Fl_Window routes an event with no display behind it (the curve editor's finding).
namespace {

// A widget that records what it was sent and where.
class EventSpy : public Fl_Widget {
public:
    EventSpy(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}
    struct Hit {
        int event;
        int x;
        int y;
        int state;
    };
    std::vector<Hit> hits;

    // FLTK sends its OWN FL_ENTER / FL_MOVE around the event we hand it (that is exactly the
    // belowmouse bookkeeping we want it doing), so a test asks whether the event it wanted ARRIVED,
    // not whether it happened to be last.
    [[nodiscard]] const Hit* got(int event) const {
        for (auto it = hits.rbegin(); it != hits.rend(); ++it)
            if (it->event == event)
                return &*it;
        return nullptr;
    }

    void draw() override {}
    int handle(int event) override {
        if (event != FL_SHOW && event != FL_HIDE) // bookkeeping, not the pointer
            hits.push_back({event, Fl::event_x(), Fl::event_y(), Fl::event_state()});
        return 1; // claim everything, including FL_PUSH, so FLTK routes the drag/release here too
    }
};

// FLTK refuses to send_event() to a widget that is not visible_r(), and a window is not visible
// until it is shown -- which would need a display. Fl_Widget::show() sets the visible FLAG without
// mapping anything, which is all the routing asks for. (FL_ENTER would arrive regardless:
// Fl::belowmouse() calls handle() directly, bypassing the check. FL_MOVE would not.)
void makeRoutable(Fl_Double_Window& win) { win.Fl_Widget::show(); }

platform::TabletSample penAt(double x, double y, bool down, std::uint64_t surface) {
    platform::TabletSample s;
    s.pos = {x, y};
    s.pressure = down ? 0.5 : 0.0;
    s.inProximity = true;
    s.buttons = down ? 1u : 0u; // bit 0 = tip contact
    s.surface = surface;
    return s;
}

} // namespace

TEST_CASE("TabletPointerSynth: the pen lands on the widget under it, as a real click") {
    Fl_Double_Window win(400, 300);
    auto* spy = new EventSpy(100, 50, 120, 40); // a "toolbar button" at (100,50)-(220,90)
    win.end();
    makeRoutable(win);

    const auto surface = std::uint64_t{0xABCD};
    TabletPointerSynth synth;
    synth.addWindow(surface, &win);

    // Hover over the button: an FL_MOVE, which is the event that never existed on Wayland -- and
    // without which the brush reticle could not follow a hovering pen.
    // FLTK's own semantics: the first move ONTO a widget is its FL_ENTER (which is what makes the
    // canvas set "the pointer is inside", and so what makes the reticle appear at all); a move
    // WITHIN it is FL_MOVE. Both carry the position, and both were missing entirely on Wayland.
    CHECK(synth.send(FL_MOVE, penAt(150, 70, false, surface), 1.0));
    const EventSpy::Hit* enter = spy->got(FL_ENTER);
    REQUIRE(enter != nullptr);
    CHECK(enter->x == 150);
    CHECK(enter->y == 70);
    CHECK(Fl::belowmouse() == static_cast<Fl_Widget*>(spy));

    CHECK(synth.send(FL_MOVE, penAt(160, 80, false, surface), 1.0));
    const EventSpy::Hit* move = spy->got(FL_MOVE);
    REQUIRE(move != nullptr);
    CHECK(move->x == 160);
    CHECK(move->y == 80);
    CHECK((move->state & FL_BUTTON1) == 0); // hovering is not pressing

    // Tip down on it: a press, with the button bit set and the button number FLTK reads back.
    spy->hits.clear();
    CHECK(synth.send(FL_PUSH, penAt(150, 70, true, surface), 1.0));
    const EventSpy::Hit* push = spy->got(FL_PUSH);
    REQUIRE(push != nullptr);
    CHECK(push->x == 150);
    CHECK(push->y == 70);
    CHECK((push->state & FL_BUTTON1) != 0);
    CHECK(Fl::event_button() == FL_LEFT_MOUSE); // <- the pen presses buttons, which was the point

    // ... and the tip lifting clears the button bit again.
    spy->hits.clear();
    CHECK(synth.send(FL_RELEASE, penAt(150, 70, false, surface), 1.0));
    const EventSpy::Hit* rel = spy->got(FL_RELEASE);
    REQUIRE(rel != nullptr);
    CHECK((rel->state & FL_BUTTON1) == 0);
}

// The scaling trap, in the synthesis path this time. Backends report SURFACE-LOCAL DEVICE pixels;
// Fl::event_x/y() are logical. Skip the divide on a 2x display and every synthesized press lands at
// twice its offset -- which is the same bug the ingest path had, and it was measured, not theorized.
TEST_CASE("TabletPointerSynth: surface pixels are divided by the GUI scale") {
    Fl_Double_Window win(400, 300);
    auto* spy = new EventSpy(0, 0, 400, 300);
    win.end();
    makeRoutable(win);

    const auto surface = std::uint64_t{7};
    TabletPointerSynth synth;
    synth.addWindow(surface, &win);

    synth.send(FL_MOVE, penAt(300.0, 150.0, false, surface), 2.0); // the FL_ENTER
    synth.send(FL_MOVE, penAt(300.0, 150.0, false, surface), 2.0); // ... then a real FL_MOVE
    const EventSpy::Hit* move = spy->got(FL_MOVE);
    REQUIRE(move != nullptr);
    CHECK(move->x == 150); // 300 device px / 2 == 150 logical
    CHECK(move->y == 75);
}

TEST_CASE("TabletPointerSynth: a sample from a surface we do not drive goes nowhere") {
    Fl_Double_Window win(400, 300);
    auto* spy = new EventSpy(0, 0, 400, 300);
    win.end();
    makeRoutable(win);

    TabletPointerSynth synth;
    synth.addWindow(1, &win);

    CHECK_FALSE(synth.send(FL_PUSH, penAt(10, 10, true, /*surface=*/2), 1.0));
    CHECK(spy->hits.empty()); // a foreign surface must not press anything of ours

    // And a window that has been dropped (a dialog closing) stops receiving at once.
    CHECK(synth.send(FL_MOVE, penAt(10, 10, false, 1), 1.0));
    CHECK_FALSE(spy->hits.empty());
    synth.removeWindow(1);
    spy->hits.clear();
    CHECK_FALSE(synth.send(FL_MOVE, penAt(10, 10, false, 1), 1.0));
    CHECK(spy->hits.empty());
}

// The bug that made "the pen can paint but not click" survive the first fix: every window of ours
// has its own wl_surface, and only the CANVAS's was registered -- so the pen over the toolbar, the
// menu bar or a menu popup resolved to no window and the event was dropped. Registering surfaces one
// at a time registers the ones you thought of; a menu popup did not exist when you thought of them.
// So an unregistered surface is resolved LIVE, and nothing is cached (a popup is destroyed the moment
// it closes, and a cached pointer to one dangles exactly when leave() would use it).
TEST_CASE("TabletPointerSynth: a window it was never told about still gets the pen") {
    Fl_Double_Window canvas(400, 300);
    auto* canvasSpy = new EventSpy(0, 0, 400, 300);
    canvas.end();
    makeRoutable(canvas);

    Fl_Double_Window toolbar(200, 40); // stands in for the top-level nobody registered
    auto* toolbarSpy = new EventSpy(0, 0, 200, 40);
    toolbar.end();
    makeRoutable(toolbar);

    constexpr std::uint64_t kCanvasSurf = 0x11;
    constexpr std::uint64_t kToolbarSurf = 0x22;

    TabletPointerSynth synth;
    synth.addWindow(kCanvasSurf, &canvas); // the canvas: a SUB-window, which no resolver can find
    synth.setResolver([&](std::uint64_t surface) -> Fl_Window* {
        return surface == kToolbarSurf ? &toolbar : nullptr;
    });

    // Registered: straight through, as before.
    CHECK(synth.send(FL_PUSH, penAt(10, 10, true, kCanvasSurf), 1.0));
    CHECK(canvasSpy->got(FL_PUSH) != nullptr);

    // NOT registered -- and this is the whole fix: the pen presses it anyway.
    CHECK(synth.send(FL_PUSH, penAt(30, 20, true, kToolbarSurf), 1.0));
    const EventSpy::Hit* push = toolbarSpy->got(FL_PUSH);
    REQUIRE(push != nullptr);
    CHECK(push->x == 30);
    CHECK(push->y == 20);

    // A surface neither registered NOR resolvable is still nobody's.
    toolbarSpy->hits.clear();
    canvasSpy->hits.clear();
    CHECK_FALSE(synth.send(FL_PUSH, penAt(5, 5, true, /*surface=*/0x99), 1.0));
    CHECK(toolbarSpy->hits.empty());
    CHECK(canvasSpy->hits.empty());
}
