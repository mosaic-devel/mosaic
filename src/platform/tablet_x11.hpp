#pragma once

#include "platform/tablet.hpp"

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The X11 / XInput2 tablet backend (docs/tablet.md §3) -- the path XWayland sessions hit, which
// makes it the one most users get. XI2 fills the sample ring at the device's rate (~200 Hz); FLTK's
// FL_PUSH/DRAG/RELEASE stream keeps the stroke lifecycle exactly as today, and the canvas drains the
// ring on each FL_DRAG (§3.1). That division is X11-ONLY: on Wayland, binding the tablet manager
// suppresses pointer emulation entirely (§4 built-note, finding 4), so the Wayland backend owns its
// lifecycle itself.
//
// ⚠ POSITION COMES FROM THE VALUATORS, not from ev.event_x/event_y. This header used to promise
// "sub-pixel doubles straight from the wire" of event_x, and that promise was FALSE in the way that
// matters: event_x is the X server's screen-mapped POINTER, and while it does carry a fraction, the
// fraction is an artefact of the server's mapping rather than the pen's position. It wobbles about
// half a pixel, sample to sample, in a sawtooth -- and THAT was the staircased X11 stroke users
// reported while the same pen on native Wayland was smooth. See xi2ParseEvent.
//
// Split for headless testing (§9): everything from a raw event to a normalized TabletSample --
// device classification and the stateful valuator walk -- is a pure function over the XI structs,
// so the tests construct XIDeviceEvent/XIDeviceInfo by hand and never open a Display. Only
// TabletX11 itself (enumerate + select + cookie handling) needs a live server.
//
// This header includes Xlib (macro pollution and all); it is included only by tablet_x11.cpp, the
// headless test, and the app-side wiring TU that installs the Fl::add_system_handler -- the
// platform tablet MODEL (tablet.hpp) stays X11-free.
namespace mosaic::platform {

// The interned Atom values of the valuator labels we recognize (§3). Resolved once from the live
// Display at init; the headless tests fill them with arbitrary distinct nonzero values, which is
// all the classifier compares against. A label we could not intern stays 0 and matches nothing
// (0 = X's None).
struct Xi2AxisLabels {
    unsigned long absPressure = 0; // "Abs Pressure"
    unsigned long absTiltX = 0;    // "Abs Tilt X"
    unsigned long absTiltY = 0;    // "Abs Tilt Y"
    unsigned long absWheel = 0;    // "Abs Wheel"
    unsigned long absX = 0;        // "Abs X" -- the device's OWN position; see xi2ParseEvent
    unsigned long absY = 0;        // "Abs Y"
};

// The screen the device's absolute range is mapped onto, in pixels. Needed to turn the position
// valuators into coordinates (see xi2ParseEvent). Left at 0 -- as the headless tests leave it --
// the valuator path is off and position comes from event_x/event_y exactly as it always did.
struct Xi2Screen {
    double width = 0.0;
    double height = 0.0;
};

// How far the valuator-derived screen position may sit from the server's own root_x/root_y before we
// stop believing the mapping -- PLUS however far the pen just travelled. Read that twice: the
// tolerance is not a constant, and it took two goes to work out why.
//
// The two disagreements this has to tell apart scale with completely different things:
//
//   * A MAPPING error -- a tablet mapped to one output of several by a Coordinate Transformation
//     Matrix, so its range does not span the screen -- is proportional to POSITION. It grows the
//     further the pen is from wherever the two happen to coincide, and it reaches hundreds of pixels.
//     This is the thing the guard exists to catch, because painting hundreds of pixels from the pen
//     is far worse than any wobble.
//
//   * The LAG between the server's pointer and the device's valuators is proportional to SPEED. In
//     the same event, root_x can sit about ONE SAMPLE OF TRAVEL behind the valuators. It is not a
//     mapping error at all; it is the price of asking two subsystems where the pen is.
//
// ⚠ A FIXED tolerance cannot separate those, and a fixed 8 px one silently broke fast strokes: at
// ~8.4 px of travel per sample the lag reached 9.00 px, tripped the 8 px bar, and kicked a device
// whose mapping was PERFECTLY CORRECT back onto the server's wobbling pointer. Measured on a fast
// arc: 41 of 66 samples fell back, the stroke MIXED the two sources, and the result (0.31 px RMS off
// the circle) was worse than either source alone would have been. The user saw it as "still a slight
// wobble on fast strokes" -- and only on curves, because a straight line never moves fast enough per
// sample to trip it.
//
// So the bar moves with the pen: `kXi2MapTolerancePx + travel`. A one-sample lag always fits under
// it; a position-proportional mapping error never does, at any speed.
inline constexpr double kXi2MapTolerancePx = 8.0;

// ... and the decision is STICKY, per device, never per event: a stroke that switched position
// source halfway through would jump. A hovering pen streams continuously, so a device is settled
// long before it can touch down and paint.
inline constexpr int kXi2MapTrustSamples = 8;

// One recognized axis on one device: which valuator number carries it, the driver's declared
// range, and the RUNNING value. The running value is load-bearing: an XI2 event carries only the
// valuators that CHANGED, so a motion event with constant pressure omits the pressure axis
// entirely and the parse must remember it. Seeded from the valuator class's current value at
// enumerate time so the first event does not read a stale zero.
struct Xi2Axis {
    int number = -1;
    double min = 0.0;
    double max = 0.0;
    double value = 0.0;

    [[nodiscard]] bool present() const noexcept { return number >= 0; }
};

// Everything the parse path knows about one slave device.
struct Xi2Device {
    int deviceId = 0;
    std::string name;
    TabletSample::Tool tool = TabletSample::Tool::Pen;
    Xi2Axis pressure;
    Xi2Axis tiltX;
    Xi2Axis tiltY;
    Xi2Axis wheel;
    Xi2Axis absX; // the device's own position -- NOT the server's; see xi2ParseEvent
    Xi2Axis absY;

    // Does this device's absolute range really span the screen? Established from the events
    // themselves (the valuator position agreeing with the server's own), and STICKY once it is --
    // see kXi2MapTrustSamples. Mutable parse state, like the running axis values above.
    int mapAgreements = 0;
    bool mapTrusted = false;
    // The previous event's valuator-derived screen position, so the guard can tell how far the pen
    // travelled between events -- which is exactly how far the server's pointer is allowed to lag.
    bool hasPrevPos = false;
    double prevSx = 0.0;
    double prevSy = 0.0;
    double worstMapDelta = 0.0;   // diagnostic: the largest disagreement ever seen for this device
    // Diagnostic: what kicked an ALREADY-TRUSTED device back out. That is the number that matters --
    // a big delta before trust is earned is just a stale seeded valuator, and harmless.
    int untrustEvents = 0;
    double worstUntrustDelta = 0.0;
};

// Classify one enumerated device. Returns a device for every slave pointer that looks like a
// tablet tool; nullopt for masters, keyboards, and ordinary pointers (mice, touchpads) -- those
// stay FLTK's, per §3.1.
//
// "Looks like a tablet tool" = has a pressure valuator, OR its name says stylus/pen/eraser (a
// pressure-less screen digitizer still deserves the sub-pixel path; §3.2 gives it pressure 1.0).
// Tool identity is by name, case-insensitively: the inverted end is a SEPARATE slave device whose
// name carries "eraser" -- CONTAINS, not ends-with, because XWayland suffixes its emulated
// devices with a client ordinal ("xwayland-tablet eraser:13") and XWayland is the default
// session. "airbrush" and "cursor"/"puck" (the Wacom puck's driver name is "cursor") are matched
// the same way; everything else is a Pen. The Wacom driver's "Wacom Tool Type" property could
// refine this someday, but it needs a live Display and real hardware to validate against, so the
// name convention -- which every shipping driver follows -- is the classifier.
[[nodiscard]] std::optional<Xi2Device> xi2Classify(const XIDeviceInfo& info,
                                                   const Xi2AxisLabels& labels);

// Fold one device event into the device's running axis state and produce the normalized sample.
// Pure over the structs: reads nothing but `dev` and `ev`, so tests hand-build the event. The
// caller routes: `ev` must belong to `dev` (TabletX11 matches deviceid before calling).
//
// `timeUs` is stamped by the caller from ingestClockUs() -- the event's own `time` field is the
// server's clock and is never read (§5).
//
// Normalization (§3.2 + tablet.hpp):
//  - pressure: (value - min) / (max - min), clamped to [0,1]. No pressure axis, or a degenerate
//    declared range, reports 1.0 -- NOT 0 -- so dynamics do not silently collapse the stroke.
//  - tilt: value / max(|min|, |max|), clamped to [-1,1], times kTiltFullScaleDegrees. Dividing by
//    the range's peak magnitude (rather than remapping [min,max] two-sidedly) keeps hardware zero
//    at exactly 0 degrees when the declared range is asymmetric, and the Wacom driver's is
//    (-64..63): an upright pen must not read as leaning.
//  - wheel: the Wacom driver routes BOTH the airbrush's finger wheel and the art pen's barrel
//    rotation through "Abs Wheel", so the tool type decides: Airbrush -> tangentialPressure in
//    [0,1] (two-sided remap); anything else -> rotation in degrees, peak-magnitude scaled to
//    +/-180 like tilt (zero must stay zero).
//  - buttons: the event's own button mask (state BEFORE the event), then this event's press/
//    release applied, so the sample reflects the state the event produced.
//  - inProximity is always true: an X11 tablet slave only reports while in proximity; there are
//    no proximity events in XI 2.x for it to say otherwise.
//
//  - position: from the "Abs X"/"Abs Y" VALUATORS -- the device's own coordinates -- and NOT from
//    ev.event_x/event_y, which is the X server's screen-mapped POINTER position.
//
//    ⚠ This is the fix for the staircased X11 stroke, and the reason is measured, not theoretical
//    (XWayland / KWin 6.7.0, 2026-07-11; tools/xi2_valuator_probe). event_x/event_y does carry a
//    fraction -- so it LOOKS sub-pixel, and the old comment here claimed it was -- but the fraction
//    is an artefact of the server's own mapping, not the pen's location. Driven along a dead
//    straight line, a pen reports through event_x with a 0.49 px RMS sawtooth that flips sign on 4
//    of every 5 samples and sometimes steps BACKWARDS along the stroke. The same events' valuators
//    are 43x more faithful (0.011 px RMS) -- exactly as clean as the same pen on native Wayland,
//    which is the backend that was never jagged. The device's own coordinates are the position; the
//    server's pointer is a rendering of it.
//
//    The device's [min,max] spans the SCREEN, so the valuators give a screen position; the window's
//    origin comes from the event itself (root_x - event_x), which is exact because both endpoints
//    carry the same server wobble and it cancels in the difference.
//
//    Guarded, because the mapping is an assumption and a bad one is far worse than the bug: if a
//    Coordinate Transformation Matrix maps the tablet to ONE output of several, the device range no
//    longer spans the screen and the valuator position would be flatly WRONG -- strokes landing
//    somewhere else, not merely wobbling. So every event cross-checks its valuator-derived screen
//    position against the server's own root_x/root_y and falls back to event_x/event_y whenever they
//    disagree by more than kXi2MapTolerancePx. `screen` left at {0,0} disables the path outright.
[[nodiscard]] TabletSample xi2ParseEvent(Xi2Device& dev, const XIDeviceEvent& ev,
                                         std::uint64_t timeUs, const Xi2Screen& screen = {});

// The live backend. FLTK-free: the wiring site owns the Fl::add_system_handler registration and
// forwards raw XEvents into handleEvent().
class TabletX11 final : public TabletBackend {
public:
    // False = XI2 < 2.2 or no XInputExtension; the backend stays unavailable and the canvas
    // falls back to synthesized pressure-1 samples (§3.2). Selects events on `window` for every
    // classified tablet device, plus hierarchy changes for hotplug.
    bool init(Display* display, Window window);

    // Feed one raw event. Returns true when the event was one of ours (an XI2 device event from
    // a classified tablet, or a hierarchy change we re-enumerated on). The caller must NOT
    // swallow the event from FLTK either way: FLTK never reads GenericEvent cookies, and the
    // core-pointer stream it does read is what keeps the stroke lifecycle alive (§3.1).
    // Non-const because the GenericEvent cookie is fetched and freed in place
    // (XGetEventData / XFreeEventData are ours to manage, §3).
    bool handleEvent(XEvent& ev);

    [[nodiscard]] bool available() const noexcept override { return m_available; }
    [[nodiscard]] std::string_view name() const noexcept override { return "x11/xi2"; }
    [[nodiscard]] SampleRing& ring() noexcept override { return m_ring; }

    // Also deliver tablet events for `window` -- another top-level window of OURS (§8: Settings ->
    // Tablet's test area must read the pen while it hovers the DIALOG, and XI2 selection is
    // per-window, so selecting only on the canvas is why it saw nothing there). Samples carry the
    // window they came from in TabletSample::surface; their `pos` is that window's, not the
    // canvas's, and it is the wiring's job never to confuse the two.
    //
    // unwatchWindow() must be called BEFORE the window is destroyed.
    void watchWindow(Window window);
    void unwatchWindow(Window window);

    // Diagnostics for Settings->Tablet's "detected devices" row (§8).
    [[nodiscard]] const std::vector<Xi2Device>& devices() const noexcept { return m_devices; }

    // How each sample's POSITION was resolved, and how the mapping guard is behaving. Diagnostic
    // only -- but not decoration: a stroke that MIXES the two sources is worse than one that uses
    // either consistently, so `fromServer` being nonzero mid-stroke on a device that HAS position
    // axes is a bug, and this is how it was caught. (tools/tablet_diag_x11 prints it.)
    [[nodiscard]] TabletPositionDiag positionDiag() const noexcept {
        TabletPositionDiag d;
        d.fromValuators = m_posValuator;
        d.fromServer = m_posServer;
        for (const Xi2Device& dev : m_devices) {
            d.untrusts += dev.untrustEvents;
            d.worstDelta = std::max(d.worstDelta, dev.worstMapDelta);
            d.worstUntrustDelta = std::max(d.worstUntrustDelta, dev.worstUntrustDelta);
        }
        return d;
    }

private:
    void enumerate();          // XIQueryDevice + classify + select; re-run on hierarchy changes
    void selectOn(Window window); // XISelectEvents for the current device list, on one window

    Display* m_display = nullptr;
    Window m_window = 0;                // the canvas window (m_windows[0])
    std::vector<Window> m_windows;      // every window we deliver tablet events for
    int m_opcode = -1;
    Xi2Screen m_screen{}; // what the devices' absolute ranges map onto (xi2ParseEvent)
    Xi2AxisLabels m_labels{};
    std::vector<Xi2Device> m_devices;
    SampleRing m_ring;
    std::size_t m_posValuator = 0;
    std::size_t m_posServer = 0;
    bool m_available = false;
};

} // namespace mosaic::platform
