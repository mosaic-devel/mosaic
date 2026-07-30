// macOS (S58) tablet backend for ui::TabletInput -- Cocoa/NSEvent, compiled in place of the body of
// tablet_input.cpp on Apple (see ui/CMakeLists.txt; the .cpp's #else is empty there). The Linux
// XInput2 + zwp_tablet_v2 implementation lives in tablet_input.cpp and is untouched by this file.
//
// THE MECHANISM (docs/tablet.md §6). A stylus reaches a Cocoa app through the ordinary mouse stream:
// a pen event is an NSEvent whose SUBTYPE is NSEventSubtypeTabletPoint / NSEventSubtypeTabletProximity
// and which carries `pressure` (0..1), `tilt` (an NSPoint of ±1), `rotation` (degrees) and
// `tangentialPressure` (±1) directly on the event. FLTK 1.4 on macOS owns the NSWindow/NSView and the
// run loop and never surfaces any of that -- Fl::event_x/y() is all it exposes. So this backend
// installs an NSEvent LOCAL monitor (the same seam Krita uses on macOS): it observes each event during
// -[NSApplication sendEvent:], BEFORE FLTK dispatches it, fills the sample ring exactly as the X11
// system handler does, and returns the event UNTOUCHED so FLTK still generates the FL_PUSH/FL_DRAG/
// FL_MOVE/FL_RELEASE the canvas drains. One lifecycle, both platforms: the ring is the sample path,
// FLTK routes, the tool drains (§3.1) -- and NOTHING in the canvas changes.
//
// This is the macOS analogue of the X11 path, not the Wayland one: like the X server (and unlike a
// tablet-aware Wayland client), Cocoa keeps driving the system pointer with the pen, so FLTK's event
// stream arrives by itself and the monitor merely supplies the valuators. There is therefore no
// TabletPointerSynth and no setToolCursor work here -- the pen IS the pointer FLTK already dresses.
//
// ⚠ Compile/link-verified only; there is no Mac in the loop. Runtime assumptions are flagged
// "Mac-side" in the comments below and collected in the session report.

#include "ui/tablet_input.hpp"

#include "common/log.hpp"
#include "platform/tablet.hpp"

#import <AppKit/AppKit.h>

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H> // fl_mac_xid() on macOS

#include <cmath>
#include <string>
#include <vector>

namespace mosaic::ui {
namespace {

spdlog::logger& tabletLog() {
    static const auto logger = common::log::category("tablet");
    return *logger;
}

// FLTK reports event coordinates in its own GUI-SCALED units; the sample carries the surface's own.
// On macOS FLTK does not apply programmatic scaling (the OS owns HiDPI), so Fl::screen_scale is 1 and
// this is a no-op -- but keep the divide for parity with the Linux ingest, which is the one seam a
// scaled UI would otherwise paint at twice the offset through.
double guiScale(Fl_Window* win) {
    const float s = Fl::screen_scale(win != nullptr ? win->screen_num() : 0);
    return s > 0.0f ? static_cast<double>(s) : 1.0;
}

const char* toolName(platform::TabletSample::Tool t) {
    switch (t) {
    case platform::TabletSample::Tool::Pen: return "Pen";
    case platform::TabletSample::Tool::Eraser: return "Eraser";
    case platform::TabletSample::Tool::Airbrush: return "Airbrush";
    case platform::TabletSample::Tool::Puck: return "Puck";
    case platform::TabletSample::Tool::Mouse: return "Mouse";
    }
    return "Pen";
}

double clampUnit(double v) noexcept { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
double clampSigned(double v) noexcept { return v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v); }

} // namespace

// ---------------------------------------------------------------------------------------------
// Impl -- the NSEvent monitor, the ring it fills, and the shared ingest/note/drain policy seam
// ---------------------------------------------------------------------------------------------

struct TabletInput::Impl {
    TabletInput* owner = nullptr;
    Fl_Window* win = nullptr;

    // The single sample path, filled by the monitor and drained by the canvas -- identical role to
    // the X11 backend's ring. Single-producer/single-consumer on the main thread: the monitor block
    // fires from inside the FLTK run loop (Cocoa dispatches events there), and so does the drain, so
    // plain indices suffice with no locking (docs/tablet.md §3.1).
    platform::SampleRing ring{512};

    id monitor = nil;         // the NSEvent local-monitor token (retained; released in the dtor)
    NSView* canvasView = nil; // the canvas window's content view (retained): identity + coord frame
    std::uint64_t canvasId = 0; // (uintptr)canvasView -- the one surface a stroke may be fed from

    // Proximity/tool state, updated from NSEventTypeTabletProximity (or a mouse event carrying the
    // proximity subtype). The pen streams samples while it merely hovers, so this rarely matters --
    // but a still, silent hover keeps stylusInProximity() honest that the pen is on the tablet.
    bool proximity = false;
    platform::TabletSample::Tool tool = platform::TabletSample::Tool::Pen;
    std::uint64_t serial = 0;
    std::string deviceName;
    bool sawDevice = false;

    double m_intervalUs = 0.0;

    [[nodiscard]] bool fromCanvas(const platform::TabletSample& s) const noexcept {
        return canvasId != 0 && s.surface == canvasId;
    }

    // ---- ingest: the ONE policy seam (§7), shared by the drain and the readout -----------------
    // A real device sample -> the engine's input. Position converts out of surface points into FLTK's
    // logical units (a no-op at macOS's scale 1); pressure/tilt go through the user's policy.
    [[nodiscard]] core::brush::StrokeInput ingest(const platform::TabletSample& s) const {
        const double k = 1.0 / guiScale(win);
        core::brush::StrokeInput in;
        in.pos = {s.pos.x * k, s.pos.y * k};
        in.pressure = s.pressure;
        in.xTilt = s.xTilt;
        in.yTilt = s.yTilt;
        in.rotation = s.rotation;
        in.tangentialPressure = s.tangentialPressure;
        in.timeUs = s.timeUs;
        return owner->m_policy.isIdentity() ? in : owner->m_policy.apply(in);
    }

    // Note it for Settings -> Tablet's live readout (§8) -- every real sample, whether or not a stroke
    // is in flight. Byte-for-byte the Linux backend's note(): a time-constant EMA over the arrival
    // interval that settles in a few samples at 200 Hz without flickering on one late frame.
    void note(const core::brush::StrokeInput& in) {
        if (owner->m_lastSampleTimeUs != 0 && in.timeUs > owner->m_lastSampleTimeUs) {
            const double dtUs = static_cast<double>(in.timeUs - owner->m_lastSampleTimeUs);
            m_intervalUs = (m_intervalUs > 0.0) ? (0.9 * m_intervalUs + 0.1 * dtUs) : dtUs;
        }
        owner->m_lastSample = in;
        owner->m_lastSampleTimeUs = in.timeUs;
    }

    // ---- the Cocoa seam: everything below touches NSEvent ---------------------------------------
    void handleEvent(NSEvent* ev);
    void handleProximity(NSEvent* ev);
    void pushSample(NSEvent* ev);
    // Convert an event's location into canvas-local, top-left, FLTK-logical coordinates, and report
    // whether the pen is actually over the canvas (the surface a stroke may be fed from).
    [[nodiscard]] common::Vec2 locateInCanvas(NSEvent* ev, bool& overCanvas) const;
};

void TabletInput::Impl::handleEvent(NSEvent* ev) {
    @autoreleasepool {
        const NSEventType t = [ev type];
        bool mouseish = false;
        switch (t) {
        case NSEventTypeLeftMouseDown:
        case NSEventTypeLeftMouseUp:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDown:
        case NSEventTypeRightMouseUp:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDown:
        case NSEventTypeOtherMouseUp:
        case NSEventTypeOtherMouseDragged:
        case NSEventTypeMouseMoved: mouseish = true; break;
        default: break;
        }
        // -subtype is only defined for the mouse-family events (and app-defined/system ones); reading
        // it on the dedicated tablet types below would be meaningless, so gate it on `mouseish`.
        NSEventSubtype sub = NSEventSubtypeMouseEvent;
        if (mouseish)
            sub = [ev subtype];

        if (t == NSEventTypeTabletProximity ||
            (mouseish && sub == NSEventSubtypeTabletProximity)) {
            handleProximity(ev);
            return;
        }
        // A pen sample rides an ordinary mouse event as the TabletPoint subtype -- the SAME NSEvent
        // FLTK turns into FL_PUSH/FL_DRAG/FL_MOVE, so ONE push here lines up with ONE canvas drain.
        // Standalone NSEventTypeTabletPoint is deliberately NOT subscribed: it would double every
        // movement the mouse event already carries, and a doubled dab double-applies flow. An ordinary
        // mouse (subtype NSEventSubtypeMouseEvent) is left alone -- the canvas synthesizes its
        // pressure-1 sample, exactly the mouse path on Linux (§3.2).
        if (mouseish && sub == NSEventSubtypeTabletPoint)
            pushSample(ev);
    }
}

void TabletInput::Impl::handleProximity(NSEvent* ev) {
    const bool entering = [ev isEnteringProximity];
    proximity = entering;
    if (!entering)
        return; // the pen left the tablet; the sample-age fallback lets stylusInProximity() lapse
    switch ([ev pointingDeviceType]) {
    case NSPointingDeviceTypePen: tool = platform::TabletSample::Tool::Pen; break;
    case NSPointingDeviceTypeEraser: tool = platform::TabletSample::Tool::Eraser; break;
    case NSPointingDeviceTypeCursor: tool = platform::TabletSample::Tool::Puck; break;
    default: tool = platform::TabletSample::Tool::Pen; break;
    }
    serial = static_cast<std::uint64_t>([ev uniqueID]);
    if (!sawDevice) {
        // NSEvent names a tablet by numeric vendor/device ids, not a string, so compose a stable label
        // from what it gives -- enough for Settings -> Tablet to show a row. A real product name would
        // need IOKit/HID, which v1 does not reach for.
        deviceName = std::string("Tablet (") + toolName(tool) + ")";
        sawDevice = true;
    }
}

void TabletInput::Impl::pushSample(NSEvent* ev) {
    platform::TabletSample s;

    bool overCanvas = false;
    s.pos = locateInCanvas(ev, overCanvas);
    // The stroke-eligible surface is the canvas view; anything else (a dialog, a panel) is tagged with
    // its own NSWindow so the readout can note it but no stroke is ever fed from it (§8, the gate). An
    // NSView pointer and an NSWindow pointer never collide, so the canvas can never be mistaken for
    // another window nor vice versa.
    s.surface = overCanvas ? canvasId
                           : reinterpret_cast<std::uint64_t>((__bridge void*)[ev window]);

    s.pressure = clampUnit([ev pressure]);

    const NSPoint tilt = [ev tilt]; // each component normalized to [-1, 1]
    s.xTilt = clampSigned(tilt.x) * platform::kTiltFullScaleDegrees;
    s.yTilt = clampSigned(tilt.y) * platform::kTiltFullScaleDegrees;

    double rot = static_cast<double>([ev rotation]); // barrel/art-pen rotation, degrees
    rot = std::fmod(rot, 360.0);
    if (rot > 180.0)
        rot -= 360.0;
    else if (rot < -180.0)
        rot += 360.0;
    s.rotation = rot;

    // NSEvent tangentialPressure is [-1, 1] with 0 at rest; the sample field is [0, 1]. Map neutral ->
    // 0.5, matching the Wayland slider (docs/tablet.md §4 backend-note 2) so both feed the airbrush
    // finger wheel the same shape.
    s.tangentialPressure = clampUnit((static_cast<double>([ev tangentialPressure]) + 1.0) * 0.5);

    s.tool = tool;
    s.toolSerial = serial;
    s.inProximity = proximity;

    std::uint32_t buttons = 0;
    const NSEventType t = [ev type];
    if (t == NSEventTypeLeftMouseDown || t == NSEventTypeLeftMouseDragged)
        buttons |= 1u; // the tip -- bit 0, exactly the X11/Wayland tip button
    const NSUInteger bm = [ev buttonMask]; // the stylus's barrel buttons (valid on tablet-point events)
    for (int i = 0; i < 3; ++i)
        if ((bm & (static_cast<NSUInteger>(1) << i)) != 0)
            buttons |= 1u << (i + 1); // barrel buttons ride above the tip bit
    s.buttons = buttons;

    s.timeUs = platform::ingestClockUs(); // OUR clock, never the driver's (§5)
    ring.push(s);
}

common::Vec2 TabletInput::Impl::locateInCanvas(NSEvent* ev, bool& overCanvas) const {
    overCanvas = false;
    if (canvasView == nil)
        return {};
    NSWindow* cvWin = [canvasView window];
    if (cvWin == nil)
        return {};

    NSWindow* evWin = [ev window];
    const NSPoint winPt = [ev locationInWindow];
    // Route through SCREEN space so the result is correct whether FLTK backs the canvas subwindow with
    // its own NSWindow or with a child NSView of the top-level window -- both are things FLTK 1.4 has
    // done, and native_window_macos.mm already leans on `[fl_mac_xid(win) contentView]` being the
    // canvas's own content view. (Mac-side: confirm the stroke lands under the nib.)
    NSPoint screenPt = winPt;
    if (evWin != nil)
        screenPt = [evWin convertPointToScreen:winPt];
    const NSPoint cvWinPt = [cvWin convertPointFromScreen:screenPt];
    const NSPoint local = [canvasView convertPoint:cvWinPt fromView:nil];

    const NSRect b = [canvasView bounds];
    const BOOL flipped = [canvasView isFlipped];
    // Cocoa's view space is bottom-left origin; FLTK (and the rest of Mosaic) is top-left. A flipped
    // view already matches FLTK; an unflipped one is mirrored in y.
    const double x = local.x;
    const double y = flipped ? local.y : (b.size.height - local.y);

    // Over the canvas iff the event belongs to the canvas's own window AND lands within the view's
    // rectangle. The window test keeps a popup that merely overlaps the canvas -- or the toolbar in a
    // shared top-level window -- from being painted as if it were the canvas.
    overCanvas = (evWin == cvWin) && (NSMouseInRect(local, b, flipped) == YES);
    return {x, y};
}

// ---------------------------------------------------------------------------------------------

TabletInput::TabletInput() : m_impl(std::make_unique<Impl>()) { m_impl->owner = this; }

TabletInput::~TabletInput() {
    if (m_impl->monitor != nil) {
        [NSEvent removeMonitor:m_impl->monitor];
        [m_impl->monitor release]; // balances the retain in init() (this .mm is MRC, not ARC)
        m_impl->monitor = nil;
    }
    if (m_impl->canvasView != nil) {
        [m_impl->canvasView release];
        m_impl->canvasView = nil;
    }
}

void TabletInput::init(Fl_Window* win) {
    if (m_impl->monitor != nil)
        return; // already up
    m_impl->win = win;
    if (win == nullptr || win->shown() == 0)
        return; // both the identity check and the coordinate frame need the shown window's NSView

    NSWindow* nsWin = (NSWindow*)fl_mac_xid(win);
    if (nsWin == nil) {
        tabletLog().warn("no Cocoa window for the tablet backend");
        return; // no backend: the canvas keeps painting from synthesized pressure-1 samples (§3.2)
    }
    NSView* content = [nsWin contentView];
    if (content == nil)
        return;
    m_impl->canvasView = [content retain];
    m_impl->canvasId = reinterpret_cast<std::uint64_t>((__bridge void*)content);

    // The local monitor: mouse events (which carry the pen's TabletPoint subtype) plus dedicated
    // proximity events. We do NOT subscribe NSEventMaskTabletPoint -- those standalone samples would
    // double the mouse-carried ones (see handleEvent). The handler returns the event untouched so
    // FLTK still routes it; the ring push has already happened by the time FLTK's FL_PUSH lands, which
    // -- unlike X11, where the core ButtonPress overtakes the XI2 contact -- means the contact sample
    // is present at the press. The canvas's deferral (m_brushPressPending) is harmless either way.
    const NSEventMask mask = NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp |
                             NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDown |
                             NSEventMaskRightMouseUp | NSEventMaskRightMouseDragged |
                             NSEventMaskOtherMouseDown | NSEventMaskOtherMouseUp |
                             NSEventMaskOtherMouseDragged | NSEventMaskMouseMoved |
                             NSEventMaskTabletProximity;
    Impl* impl = m_impl.get();
    m_impl->monitor = [[NSEvent addLocalMonitorForEventsMatchingMask:mask
                                                             handler:^NSEvent*(NSEvent* ev) {
                          impl->handleEvent(ev);
                          return ev; // NEVER consume: FLTK must still drive the stroke lifecycle
                      }] retain];

    tabletLog().info("tablet backend: Cocoa NSEvent (local monitor)");
}

// Per-window event selection is an X11 requirement (XISelectEvents); on macOS the local monitor is
// APP-GLOBAL and already reads the pen over every window of ours -- the Settings -> Tablet test area's
// dialog included -- so there is nothing to select on and nothing to release.
void TabletInput::watch(Fl_Window*) {}
void TabletInput::unwatch(Fl_Window*) {}

void TabletInput::setToolCursor(Fl_Cursor) {
    // No-op on macOS: a stylus IS the system pointer here (as on X11), so FLTK's own cursor() calls
    // through the NSWindow already dress the pen. Only a tablet-aware Wayland client, whose tool has no
    // wl_pointer, has to name its own cursor.
}

bool TabletInput::ringDriven() const noexcept { return m_impl->monitor != nil; }

core::brush::StrokeInput TabletInput::pressSample(double fallbackX, double fallbackY) {
    core::brush::StrokeInput out;
    bool got = false;
    if (m_impl->monitor != nil) {
        platform::TabletSample s;
        while (m_impl->ring.pop(s)) { // drain to the NEWEST: the press is where the pen is now
            const core::brush::StrokeInput in = m_impl->ingest(s);
            m_impl->note(in); // note every sample; only the canvas's may BEGIN a stroke
            if (m_impl->fromCanvas(s)) {
                out = in;
                got = true;
            }
        }
    }
    if (!got) {
        // No stylus sample buffered -- a mouse. Pressure 1, NOT 0 (§3.2), and deliberately NOT run
        // through the policy: there is no device pressure here for a curve to reshape.
        out = core::brush::StrokeInput{};
        out.pos = {fallbackX, fallbackY};
        out.pressure = 1.0;
        out.timeUs = platform::ingestClockUs();
    }
    return out;
}

std::size_t TabletInput::drain(double fallbackX, double fallbackY, const SampleFn& fn) {
    std::size_t n = 0;
    if (m_impl->monitor != nil) {
        platform::TabletSample s;
        while (m_impl->ring.pop(s)) { // OLDEST FIRST -- the whole motion segment, in order
            const core::brush::StrokeInput in = m_impl->ingest(s);
            m_impl->note(in);
            if (!m_impl->fromCanvas(s))
                continue; // another window's sample (the settings dialog): read, never painted with
            if (fn)
                fn(in);
            ++n;
        }
    }
    if (n == 0) { // mouse (or a stalled device): one synthesized sample, as at a press
        if (fn)
            fn(pressSample(fallbackX, fallbackY));
        n = 1;
    }
    return n;
}

std::size_t TabletInput::pumpReadout() {
    if (m_impl->monitor == nil)
        return 0;
    // NOTE every sample, then let them go: none is wanted by a stroke, but they are the pen telling us
    // where it IS and how fast it is talking -- the test area (§8), the resolved rate, and
    // stylusInProximity() all read that. Every sample, not just the newest: note() derives the rate
    // from the gap between the samples it is shown.
    platform::TabletSample s;
    std::size_t n = 0;
    while (m_impl->ring.pop(s)) {
        m_impl->note(m_impl->ingest(s));
        ++n;
    }
    return n;
}

void TabletInput::discardBuffered() { pumpReadout(); }

bool TabletInput::stylusInProximity() const noexcept {
    if (m_impl->proximity)
        return true; // a proximity_in with no matching proximity_out yet: the pen is on the tablet
    // Fallback to the Linux sample-age heuristic -- also the whole answer for a driver that never
    // sends NSEventTypeTabletProximity. A hovering pen streams samples through mouse-moved; a stale
    // reading means the user reached for the mouse. 150 ms is far below human reaction time.
    if (m_lastSampleTimeUs == 0)
        return false;
    const std::uint64_t now = platform::ingestClockUs();
    return now <= m_lastSampleTimeUs || (now - m_lastSampleTimeUs) < 150'000;
}

platform::TabletPositionDiag TabletInput::positionDiag() const {
    // All-zero, like Wayland: Cocoa hands us window-local coordinates directly, so there is no
    // valuator-vs-server position guess to diagnose (the X11 mapping guard, §3.5).
    return {};
}

std::string TabletInput::backendName() const {
    return m_impl->monitor != nil ? std::string("Cocoa NSEvent") : std::string();
}

std::vector<TabletDeviceInfo> TabletInput::devices() const {
    std::vector<TabletDeviceInfo> out;
    if (m_impl->sawDevice) {
        // NSEvent normalizes every axis, so there is no per-axis range to report; a tablet stylus
        // always carries pressure, tilt and rotation on the event.
        out.push_back({m_impl->deviceName, toolName(m_impl->tool), "pressure, tilt, rotation"});
    }
    return out;
}

double TabletInput::sampleRateHz() const noexcept {
    if (m_impl->m_intervalUs <= 0.0 || m_lastSampleTimeUs == 0)
        return 0.0;
    const std::uint64_t now = platform::ingestClockUs();
    if (now > m_lastSampleTimeUs && (now - m_lastSampleTimeUs) > 250'000)
        return 0.0; // nothing recent: the pen is away, and a stale rate would be a lie
    return 1'000'000.0 / m_impl->m_intervalUs;
}

} // namespace mosaic::ui
