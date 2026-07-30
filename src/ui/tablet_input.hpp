#pragma once

#include "core/brush/stroke_state.hpp"  // StrokeInput -- what the engine and the policy speak
#include "core/brush/tablet_policy.hpp" // the §7 policy, applied at ingest
#include "platform/tablet.hpp"          // TabletSample; TabletPositionDiag

#include <FL/Enumerations.H> // Fl_Cursor -- the pen names its own cursor on Wayland

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Fl_Window;

// The app-side tablet wiring (docs/tablet.md §10 step 5): the ONE place FLTK meets the platform
// tablet layer. Everything below it -- platform::TabletX11, platform::TabletWayland, the unified
// TabletSample -- is FLTK-free and headless-tested; everything above it (VulkanCanvas, the engine)
// speaks core::brush::StrokeInput and has never heard of XI2 or wl_fixed. This TU owns both seams:
// Fl::add_system_handler on X11, and the FLTK surface handles the Wayland backend binds to.
//
// TWO backends, ONE lifecycle. To FLTK the pen is a mouse that happens to carry pressure: it fills a
// sample ring, FLTK's ordinary FL_PUSH/FL_DRAG/FL_MOVE/FL_RELEASE stream drives whichever tool is
// active, and the tool DRAINS the ring for the pressure/tilt of every sample the device produced in
// between (drain(), at the device's ~200 Hz, not the frame rate). Every tool works with a pen for
// exactly the reason every tool works with a mouse: it is the same code.
//
// What differs is only who produces that event stream:
//
//  - X11/XI2 (§3.1) -- what a pure-Xorg session, or anyone who sets FLTK_BACKEND=x11, gets. This
//    was the DEFAULT path until S59-a flipped the backend pin (Mosaic used to pin FLTK to X11
//    whenever FLTK_BACKEND was unset, so XWayland was the shipped session). The X server keeps
//    moving the core pointer with the pen, so the stream arrives by itself and XI2 merely supplies
//    the valuators.
//
//  - Wayland/zwp_tablet_v2 (§4 finding 4) -- the DEFAULT path since S59-a, which pins
//    FLTK_BACKEND=wayland on a Wayland session (docs/wayland.md). The moment a client binds the
//    tablet manager, KWin STOPS emulating pointer events for the pen. So the wiring synthesizes
//    them: every tool frame becomes the Fl::handle() call the compositor declined to make
//    (TabletPointerSynth below).
//    Without that the pen could paint the canvas AND NOTHING ELSE -- no toolbar, no menus, no
//    dialogs, and no hover, because nothing was generating FL_MOVE and the reticle never followed a
//    hovering pen. The same binding also makes the tool's CURSOR ours to name (setToolCursor):
//    FLTK's cursor calls go through wl_pointer, and a tablet tool has none, so a client that never
//    sets it shows the compositor's default -- KWin's crosshair, over every pixel of the app.
//
// The POLICY (docs/tablet.md §7) applies at exactly one seam, shared by both: raw sample ->
// StrokeInput -> policy.apply(). The Wayland backend deliberately hands its samples over
// un-policied for this reason. Synthesized samples (no stylus in proximity) are NOT run through it
// -- a mouse has no pressure for a pressure curve to reshape, and a curve that mapped 1.0 to 0.8
// would otherwise quietly weaken every mouse stroke in the program.
namespace mosaic::ui {

// The surface rules the Wayland sink applies. Pure and separate because they encode things that are
// easy to get wrong and impossible to notice once they are:
//
//  1. **A `down` is only ours when the pen is in PROXIMITY.** The backend sets that flag only when
//     `proximity_in` named a surface of ours -- but the compositor still delivers down/motion/up for
//     a pen over any *other* surface of this same client, and the backend's frame() dispatches those
//     to the sink all the same, carrying that surface's coordinates.
//  2. **An `up` is gated on the STROKE, not on proximity.** A pen flicked off the tablet delivers
//     `up` and `proximity_out` in ONE frame, so the sample carrying that up already reads out of
//     proximity. A stroke that ended that way still has to end.
//  3. **A sample only strokes when it came from the CANVAS's own window.** A backend delivers
//     tablet events for every window of ours that asked for them -- Settings -> Tablet's test area
//     has to read the pen while it hovers THE DIALOG, which is the one place the test area is any
//     use. But TabletSample::pos is SURFACE-LOCAL: the same pen, over the dialog, reports the
//     dialog's coordinates. Fed to a stroke they would paint wherever the dialog happens to sit.
//     Rules 1 and 3 are not the same check: proximity says the pen is on a surface of OURS, the
//     surface id says it is on the RIGHT one.
class TabletStrokeGate {
public:
    // The window a stroke may be driven from -- the canvas's. Everything else is read-only.
    void setCanvasSurface(std::uint64_t surface) noexcept { m_canvasSurface = surface; }
    [[nodiscard]] bool isCanvas(std::uint64_t surface) const noexcept {
        return surface == m_canvasSurface;
    }

    // True = this `down` starts a stroke.
    [[nodiscard]] bool begin(bool inProximity, std::uint64_t surface) noexcept {
        if (!inProximity || !isCanvas(surface))
            return false;
        m_stroking = true;
        return true;
    }
    // True = a stroke is live and this motion belongs to it. A `down` we refused cannot move.
    [[nodiscard]] bool motion(std::uint64_t surface) const noexcept {
        return m_stroking && isCanvas(surface);
    }
    // True = a stroke was live and this `up` ends it. Consumes the stroke either way.
    //
    // NOT gated on the surface, deliberately: a live stroke must END. (It cannot in fact arrive from
    // elsewhere -- the compositor holds an implicit grab on the surface a tool is DOWN on -- but a
    // stroke stuck on forever is a worse failure than a final dab in the wrong place.)
    [[nodiscard]] bool end() noexcept {
        const bool was = m_stroking;
        m_stroking = false;
        return was;
    }
    // True = the reticle should follow this hover. Same surface rule; no stroke involved.
    [[nodiscard]] bool hover(std::uint64_t surface) const noexcept { return isCanvas(surface); }
    // The pen left the tablet: never leave a stroke hanging on the sink's side.
    void proximityOut() noexcept { m_stroking = false; }
    [[nodiscard]] bool stroking() const noexcept { return m_stroking; }

private:
    bool m_stroking = false;
    std::uint64_t m_canvasSurface = 0;
};

// The pen, made into a pointer (docs/tablet.md §4 finding 4).
//
// Binding zwp_tablet_manager_v2 makes the compositor stop emulating pointer events for the pen. The
// canvas can still be painted -- the backend delivers samples -- but FLTK never hears about the pen
// at all, so it cannot press a toolbar button, open a menu, reach a dialog, or move the reticle on
// hover: there is no FL_MOVE for any of it. So the wiring hands FLTK the events the compositor
// declined to send. Fl::e_* and Fl::handle are public API, and FLTK's own platform drivers do
// precisely this.
//
// Split out of the sink so it can be driven headlessly: an Fl_Window routes an event with no display
// behind it, so a test can assert that a sample at a surface coordinate lands on the right widget at
// the right place -- including the GUI-scale divide, which is the part that silently paints at twice
// the offset when it is wrong.
class TabletPointerSynth {
public:
    // Bind a surface id to the FLTK window it names. Needed only for windows the RESOLVER cannot
    // find -- in practice exactly one, the canvas, which is an FLTK SUB-window (Fl::next_window
    // walks top-levels only) and yet has a surface of its own that the pen enters directly.
    void addWindow(std::uint64_t surface, Fl_Window* win);
    void removeWindow(std::uint64_t surface);

    // How an unregistered surface is turned into a window. Every top-level of ours has a surface the
    // pen can enter -- the one carrying the toolbar and the menu bar, a dialog, a MENU POPUP that did
    // not exist a moment ago -- and registering them one by one is a list that is wrong the instant
    // it is written. So resolve on demand, against FLTK's live window list, and cache NOTHING: a
    // popup is destroyed the moment it closes, and a cached pointer to one is a dangling pointer to a
    // window we would then dispatch into.
    void setResolver(std::function<Fl_Window*(std::uint64_t)> fn) { m_resolve = std::move(fn); }

    [[nodiscard]] Fl_Window* windowFor(std::uint64_t surface) const;

    // Dispatch one sample as one FLTK event (FL_PUSH / FL_DRAG / FL_MOVE / FL_RELEASE). False when
    // the sample came from a surface we do not drive. `scale` is the window's GUI scale factor:
    // sample positions are SURFACE-LOCAL DEVICE pixels and Fl::event_x/y() are logical, so a scaled
    // UI that skips this divide reports the pen at twice its offset.
    bool send(int event, const platform::TabletSample& s, double scale);

    // The pen left the tablet: FL_LEAVE into whichever window it was last over, or nothing.
    void leave();

private:
    std::vector<std::pair<std::uint64_t, Fl_Window*>> m_windows;
    std::function<Fl_Window*(std::uint64_t)> m_resolve;
    // The surface last dispatched into -- an ID, deliberately, NOT the window. A menu popup is gone
    // by the time the pen leaves the tablet, and a stored Fl_Window* to it would be dangling exactly
    // when leave() came to use it. Re-resolve instead; a window that no longer exists resolves to
    // nothing and there is nothing to leave.
    std::uint64_t m_lastSurface = 0;
};

// One detected device, for Settings -> Tablet's diagnostic row (§8). Purely descriptive.
struct TabletDeviceInfo {
    std::string name;      // the driver's device name ("Wacom Intuos Pro S Pen")
    std::string tool;      // the classified tool ("Pen" / "Eraser" / "Airbrush" / "Puck" / "Mouse")
    std::string valuators; // which axes it actually reports ("pressure, tilt" -- "" if none)
};

class TabletInput {
public:
    // A sample handed to the canvas. `pos` is in CANVAS-LOCAL LOGICAL coordinates -- exactly the
    // space Fl::event_x/y() reports in, so the canvas maps it to the document the same way it maps a
    // mouse press -- but sub-pixel, which is the whole point (§3.1 payoff 3). Pressure/tilt/rotation
    // have already been through TabletPolicy.
    using SampleFn = std::function<void(const core::brush::StrokeInput&)>;

    TabletInput();
    ~TabletInput();
    TabletInput(const TabletInput&) = delete;
    TabletInput& operator=(const TabletInput&) = delete;

    // Bring up whichever backend this session runs on. `win` must already be SHOWN (both backends
    // need its native handle). Safe to call twice -- the second call is a no-op -- and safe to fail:
    // with no backend the canvas simply keeps painting from synthesized pressure-1 samples, which is
    // exactly what it did before any of this existed (§3.2).
    //
    // ONE lifecycle, both platforms: the pen fills a sample ring, and FLTK's ordinary
    // FL_PUSH/FL_DRAG/FL_MOVE/FL_RELEASE stream drives the tools, which drain the ring for pressure.
    // On X11 the X server already moves the core pointer with the pen, so that stream arrives by
    // itself. On Wayland it does NOT -- binding the tablet manager makes the compositor stop
    // emulating pointer events for the pen entirely (docs/tablet.md §4 finding 4) -- so the wiring
    // SYNTHESIZES it: every tool frame becomes the Fl::handle() call the compositor declined to
    // make. Without that the pen could paint the canvas and do nothing else in the program: no
    // toolbar, no menus, no dialogs, and no hover (there is no FL_MOVE, so the reticle never
    // followed a hovering pen). This is what FLTK's own platform drivers do; Fl::e_x and
    // Fl::handle are public API.
    void init(Fl_Window* win);

    // Read the pen over ANOTHER of our windows too. Tablet event delivery is per-window on both
    // platforms, so a backend brought up on the canvas sees NOTHING while the pen hovers a dialog --
    // which is why Settings -> Tablet's test area used to sit dead unless you held the pen over the
    // canvas behind it, the one place a test area is no use.
    //
    // A watched window feeds the live readout (lastSample / sampleRateHz / stylusInProximity) and
    // NOTHING ELSE: its samples are in ITS coordinates, so they can never begin, move or end a
    // canvas stroke. `win` must be SHOWN (we need its native handle), and unwatch() must be called
    // BEFORE it is destroyed.
    void watch(Fl_Window* win);
    void unwatch(Fl_Window* win);

    // --- FLTK owns the lifecycle; the canvas drains the ring (§3.1) ---------------------------
    // True when a backend is live and filling the ring -- i.e. a press should resolve its sample
    // from the device. False when no backend came up at all, and the canvas falls back to
    // synthesized pressure-1 samples.
    [[nodiscard]] bool ringDriven() const noexcept;

    // The sample a stroke BEGINS from, for FL_PUSH. Drains the ring and returns the newest buffered
    // sample; with nothing buffered (no stylus in proximity -- a mouse), synthesizes one at
    // (fallbackX, fallbackY) with pressure 1 (§3.2: 1, never 0, or dynamics collapse the stroke).
    [[nodiscard]] core::brush::StrokeInput pressSample(double fallbackX, double fallbackY);

    // Drain the ring into `fn`, OLDEST FIRST, for FL_DRAG: every sample the device produced since
    // the last drain reaches the engine, in order, so a fast stroke is a curve and not a polygon
    // (§3.1 payoff 2). An empty ring synthesizes ONE sample at (fallbackX, fallbackY), pressure 1.
    // Returns how many samples were fed (always >= 1).
    std::size_t drain(double fallbackX, double fallbackY, const SampleFn& fn);

    // Drop whatever the ring has buffered. The XI2 stream fills at ~200 Hz whenever the pen is over
    // the canvas, including while it hovers or drags a lasso -- samples nobody will ever consume.
    // Dropping them at every non-stroke event keeps SampleRing::overwritten() an honest STALL
    // counter instead of a count of samples that were never wanted. Every one is NOTED on the way
    // out (they are the device's live state -- the test area and stylusInProximity read it).
    void discardBuffered();

    // The same drain, under the name the Settings dialog calls it by, returning how many samples it
    // noted. On X11 NOTHING ELSE drains the ring while a dialog holds the focus -- the canvas is not
    // getting events -- so without this the live readout would freeze the moment it became useful.
    // Returns 0 on Wayland, where the sink notes each sample as it lands.
    std::size_t pumpReadout();

    // Is a stylus on the tablet RIGHT NOW (as opposed to the user reaching for the mouse)? True iff
    // a real device sample landed within the last breath.
    //
    // ⚠ This is what a press has to branch on, and the reason is measured, not theoretical
    // (XWayland / KWin 6.7.0, 2026-07-11): when the tip makes contact, the X server's CORE
    // ButtonPress reaches the client BEFORE the XI2 events carrying that contact, so at FL_PUSH the
    // ring is still EMPTY. A press that resolved its sample right there would begin every tablet
    // stroke at the synthesized pressure 1.0 -- a full-size blob before the stroke drops to the
    // pressure the nib actually made. The canvas therefore DEFERS the first dab to the first real
    // sample whenever this is true, and begins immediately (pressure 1, unchanged) when it is not.
    [[nodiscard]] bool stylusInProximity() const noexcept;

    // The cursor the PEN shows. A no-op unless the pen is its own pointer (native Wayland), where
    // binding the tablet manager makes the tool cursor ours to draw and FLTK's cursor calls -- which
    // go through wl_pointer, and a tablet tool has none -- cannot reach it. Anything that sets the
    // window's cursor must set this alongside, or the pen keeps whatever the compositor defaulted to
    // (KWin: a crosshair) no matter what the app asked for. FL_CURSOR_NONE hides it, which is what
    // the canvas wants under a brush: the reticle ring IS the cursor.
    void setToolCursor(Fl_Cursor cursor);

    // --- shared -------------------------------------------------------------------------------
    // The live policy (§7). Settings -> Tablet writes through this; the ingest paths above read it.
    [[nodiscard]] core::brush::TabletPolicy& policy() noexcept { return m_policy; }
    [[nodiscard]] const core::brush::TabletPolicy& policy() const noexcept { return m_policy; }

    // Everything Settings -> Tablet needs to say "is my tablet working" without painting (§8).
    // X11 only: how each sample's POSITION was resolved, and how the mapping guard is behaving.
    // All-zero on Wayland, where the compositor hands us surface-local coordinates directly and
    // there is no mapping to guess at.
    [[nodiscard]] platform::TabletPositionDiag positionDiag() const;

    [[nodiscard]] std::string backendName() const;    // "" when no backend came up
    [[nodiscard]] std::vector<TabletDeviceInfo> devices() const;

    // The most recent sample any backend delivered, and OUR clock when it landed (0 = never). The
    // Settings -> Tablet test area polls this: it is the whole live readout, and it works while the
    // pen merely HOVERS, which is what makes it answer the question without a stroke.
    [[nodiscard]] core::brush::StrokeInput lastSample() const noexcept { return m_lastSample; }
    [[nodiscard]] std::uint64_t lastSampleTimeUs() const noexcept { return m_lastSampleTimeUs; }
    // Samples per second over the recent stream, 0 when nothing is arriving. The "resolved sample
    // rate" of §8 -- what tells a user their 200 Hz tablet is being read at 200 Hz.
    [[nodiscard]] double sampleRateHz() const noexcept;

    // Opaque; defined in the .cpp, where it holds both backends and implements the Wayland sink.
    // Public only so the X11 system handler -- a C-style free function, which is what
    // Fl::add_system_handler takes -- can name it. Nothing about it leaks: it is a forward
    // declaration, and this header stays free of Xlib and its macros.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
    core::brush::TabletPolicy m_policy;
    core::brush::StrokeInput m_lastSample{};
    std::uint64_t m_lastSampleTimeUs = 0;
};

} // namespace mosaic::ui
