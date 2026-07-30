#include "ui/tablet_input.hpp"

// THREE hosts, one ui::TabletInput. Every branch below fills the same sample ring and answers the
// same questions; what differs is only which device API supplies the valuators.
//
//   * macOS (S58) reads the pen off Cocoa's NSEvent stream through a local event monitor, which is
//     Objective-C++ and cannot live in a .cpp -- so it is compiled from tablet_input_macos.mm IN
//     THIS FILE'S PLACE (ui/CMakeLists.txt swaps the source on Apple), the same INSTEAD-of split
//     spell_checker_macos.mm uses. This TU therefore defines NOTHING on Apple.
//   * Windows (S57) taps WinTab packets or the Pointer Input Stack through
//     Fl::add_system_handler, which on Windows receives every raw MSG. The device work is
//     platform/tablet_win32.{hpp,cpp}; the branch here is the FLTK half.
//   * Linux is the original: XInput2 and zwp_tablet_v2.
#if defined(__APPLE__)

// Nothing: tablet_input_macos.mm is compiled instead of this file (see above).

#elif defined(_WIN32)

#include "common/log.hpp"
#include "platform/native_window.hpp"
#include "platform/tablet.hpp"
#include "platform/tablet_win32.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace mosaic::ui {
namespace {

spdlog::logger& tabletLog() {
    static const auto logger = common::log::category("tablet");
    return *logger;
}

// FLTK reports event coordinates in its own GUI-SCALED units while the sample carries the window's
// own device pixels, so the ingest divides by the same factor FLTK divides by. On Windows that
// factor is REAL and commonly not 1: FLTK folds the monitor's fractional DPI into
// Fl::screen_scale, so at the ubiquitous 150% setting an unscaled ingest would put the pen at 1.5x
// its offset from the canvas origin. (This is the one platform where the divide is load-bearing at
// default settings -- on Linux it is a no-op at scale 1 and on macOS the OS owns HiDPI.)
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

// Read four comma-separated whole numbers ("x,y,w,h"). All-or-nothing: a half-parsed rectangle
// would map the tablet somewhere nobody asked for, which is the exact failure the override exists
// to fix.
//
// std::strtol and NOT std::strtod, deliberately: a decimal separator means different things in
// different locales, this string is read after the locale is installed, and a monitor rectangle is
// whole pixels anyway (docs/i18n.md -- the LC_NUMERIC round-trip trap).
bool parseRect(const char* text, double& x, double& y, double& w, double& h) {
    long values[4] = {0, 0, 0, 0};
    const char* p = text;
    for (long& value : values) {
        char* end = nullptr;
        value = std::strtol(p, &end, 10);
        if (end == p)
            return false;
        p = end;
        while (*p == ',' || *p == ';' || *p == ' ')
            ++p;
    }
    x = static_cast<double>(values[0]);
    y = static_cast<double>(values[1]);
    w = static_cast<double>(values[2]);
    h = static_cast<double>(values[3]);
    return w > 0.0 && h > 0.0;
}

// The two Windows-only escape hatches from docs/tablet.md §5a.
//
// Environment variables rather than settings fields, and on purpose: the situation both exist for
// is a machine whose tablet driver is PRESENT BUT WRONG -- reporting no pressure, or mapping the
// stylus to the wrong monitor -- where the user has to change the answer before the program is
// usable enough to reach a dialog. That is the same reason FLTK_BACKEND=x11 is an environment
// variable. Settings -> Tablet's API selector (§8) supersedes the first of them when it lands.
platform::TabletWin32Config tabletConfigFromEnv() {
    platform::TabletWin32Config cfg;

    if (const char* api = std::getenv("MOSAIC_TABLET_API"); api != nullptr && *api != '\0') {
        const std::string_view value(api);
        if (value == "wintab")
            cfg.api = platform::TabletWin32Api::WinTab;
        else if (value == "ink" || value == "pointer")
            cfg.api = platform::TabletWin32Api::PointerInk;
        else if (value != "auto")
            tabletLog().warn("MOSAIC_TABLET_API='{}' is not wintab/ink/auto; ignoring", value);
    }

    if (const char* mapping = std::getenv("MOSAIC_TABLET_MAPPING");
        mapping != nullptr && *mapping != '\0') {
        const std::string_view value(mapping);
        if (value == "driver") {
            cfg.mapping = platform::TabletWin32Mapping::Driver;
        } else if (value == "screen" || value == "virtual-screen") {
            cfg.mapping = platform::TabletWin32Mapping::VirtualScreen;
        } else if (parseRect(mapping, cfg.customX, cfg.customY, cfg.customW, cfg.customH)) {
            cfg.mapping = platform::TabletWin32Mapping::Custom;
        } else {
            tabletLog().warn("MOSAIC_TABLET_MAPPING='{}' is not driver/screen/'x,y,w,h'; ignoring",
                             value);
        }
    }
    return cfg;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Impl -- the Windows backend, the ring it fills, and the shared ingest/note/drain policy seam
// ---------------------------------------------------------------------------------------------

struct TabletInput::Impl {
    TabletInput* owner = nullptr;
    Fl_Window* win = nullptr;

    platform::TabletWin32 win32;
    bool live = false;
    bool handlerInstalled = false;

    // The one window a stroke may be fed from. Every sample carries the window it came from, and
    // the ones from anywhere else -- a dialog, the toolbar's top-level, a menu popup -- are READ
    // (they drive Settings -> Tablet's live readout, §8) but never painted with: TabletSample::pos
    // is that window's own coordinates, and feeding it to a canvas stroke would paint at a lie.
    std::uint64_t canvasId = 0;

    double m_intervalUs = 0.0;

    [[nodiscard]] bool fromCanvas(const platform::TabletSample& s) const noexcept {
        return canvasId != 0 && s.surface == canvasId;
    }

    // ---- ingest: the ONE policy seam (§7), shared by the drain and the readout ----------------
    // A real device sample -> the engine's input. Position converts out of window device pixels
    // into FLTK's logical units; pressure/tilt go through the user's policy. Byte-for-byte the
    // Linux ingest, so a curve calibrated on one platform means the same thing on the other.
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
        // The default policy is an identity over every field it touches, so skip it outright rather
        // than push a sample through a remap and a LUT that cannot change it.
        return owner->m_policy.isIdentity() ? in : owner->m_policy.apply(in);
    }

    // Note it for Settings -> Tablet's live readout (§8) -- every real sample, whether or not a
    // stroke is in flight. This is what makes the test area answer "is my tablet working" while the
    // pen merely hovers.
    void note(const core::brush::StrokeInput& in) {
        if (owner->m_lastSampleTimeUs != 0 && in.timeUs > owner->m_lastSampleTimeUs) {
            const double dtUs = static_cast<double>(in.timeUs - owner->m_lastSampleTimeUs);
            // A time-constant EMA over the arrival interval, the SpeedSmoother's shape: the readout
            // must settle in a few samples at 200 Hz without flickering on one late frame.
            m_intervalUs = (m_intervalUs > 0.0) ? (0.9 * m_intervalUs + 0.1 * dtUs) : dtUs;
        }
        owner->m_lastSample = in;
        owner->m_lastSampleTimeUs = in.timeUs;
    }
};

namespace {

// The Windows system handler. FLTK's event loop hands us every raw MSG after PeekMessageW and
// BEFORE TranslateMessage/DispatchMessageW (Fl_win32.cxx), which is exactly the seam the X11 branch
// gets for XEvents -- so no HWND subclassing, no WndProc of ours, and FLTK's own message handling
// is untouched.
//
// A nonzero return SWALLOWS the message, and the backend asks for that in exactly one case: the
// driver ExpressKey range (docs/tablet.md §5). Tablet packets and pointer messages are never
// swallowed -- Windows promotes pen input into the legacy mouse stream, and that stream is what
// drives FLTK's FL_PUSH/FL_DRAG/FL_RELEASE and therefore the stroke itself (§3.1).
int win32SystemHandler(void* event, void* data) {
    auto* impl = static_cast<TabletInput::Impl*>(data);
    if (impl == nullptr || event == nullptr)
        return 0;
    return impl->win32.handleMessage(*static_cast<MSG*>(event)) ? 1 : 0;
}

} // namespace

// ---------------------------------------------------------------------------------------------

TabletInput::TabletInput() : m_impl(std::make_unique<Impl>()) { m_impl->owner = this; }

TabletInput::~TabletInput() {
    if (m_impl->handlerInstalled)
        Fl::remove_system_handler(win32SystemHandler);
}

void TabletInput::init(Fl_Window* win) {
    if (m_impl->live)
        return; // already up
    m_impl->win = win;

    platform::NativeSurfaceHandle handle;
    std::string err;
    if (!platform::nativeSurfaceHandle(win, handle, err)) {
        tabletLog().warn("no native handle for the tablet backend: {}", err);
        return; // no backend: the canvas keeps painting from synthesized pressure-1 samples (§3.2)
    }
    auto* hwnd = static_cast<HWND>(handle.window);
    m_impl->canvasId = reinterpret_cast<std::uint64_t>(hwnd);

    m_impl->live = m_impl->win32.init(hwnd, tabletConfigFromEnv());
    if (!m_impl->live) {
        tabletLog().info("no tablet backend on this system; pressure unavailable");
        return;
    }
    // The raw-MSG tap. It lives HERE and not in platform/ -- the platform tablet layer is
    // FLTK-free, and Fl::add_system_handler is as FLTK as it gets.
    Fl::add_system_handler(win32SystemHandler, m_impl.get());
    m_impl->handlerInstalled = true;
    tabletLog().info("tablet backend: {} ({} device(s))", m_impl->win32.name(),
                     m_impl->win32.devices().size());
}

// Unlike X11, per-window registration is NOT what makes the pen readable over another window: both
// Windows device paths report the pen across the whole desktop and every sample says which window
// of ours it landed on, so Settings -> Tablet's test area already works with the dialog in front.
// What this does is switch off Windows' own pen gesture VISUALS on that window -- the
// press-and-hold ring, the tap ripple, the barrel-button splash -- which otherwise draw themselves
// over the artwork under the nib.
void TabletInput::watch(Fl_Window* win) {
    platform::NativeSurfaceHandle handle;
    std::string err;
    if (!m_impl->live || win == nullptr || !platform::nativeSurfaceHandle(win, handle, err))
        return; // not shown yet: no HWND to configure
    m_impl->win32.watchWindow(static_cast<HWND>(handle.window));
}

void TabletInput::unwatch(Fl_Window* win) {
    platform::NativeSurfaceHandle handle;
    std::string err;
    if (!m_impl->live || win == nullptr || !platform::nativeSurfaceHandle(win, handle, err))
        return;
    m_impl->win32.unwatchWindow(static_cast<HWND>(handle.window));
}

void TabletInput::setToolCursor(Fl_Cursor) {
    // No-op on Windows, as on X11 and macOS: the stylus IS the system pointer here, so the cursor
    // FLTK already sets through the HWND is the one the pen shows. Only a tablet-aware Wayland
    // client, whose tool has no wl_pointer for FLTK's cursor calls to reach, has to name its own.
}

bool TabletInput::ringDriven() const noexcept { return m_impl->live; }

core::brush::StrokeInput TabletInput::pressSample(double fallbackX, double fallbackY) {
    core::brush::StrokeInput out;
    bool got = false;
    if (m_impl->live) {
        platform::SampleRing& ring = m_impl->win32.ring();
        platform::TabletSample s;
        while (ring.pop(s)) { // drain to the NEWEST: the press is where the pen is now
            const core::brush::StrokeInput in = m_impl->ingest(s);
            m_impl->note(in); // note every sample; only the canvas's may BEGIN a stroke
            if (m_impl->fromCanvas(s)) {
                out = in;
                got = true;
            }
        }
    }
    if (!got) {
        // No stylus sample buffered -- a mouse. Pressure 1, NOT 0 (§3.2), or size/flow dynamics
        // would silently collapse every mouse stroke to nothing. Deliberately NOT run through the
        // policy: there is no device pressure here for a pressure curve to reshape, and a curve
        // mapping 1.0 -> 0.8 must not quietly weaken the mouse.
        out = core::brush::StrokeInput{};
        out.pos = {fallbackX, fallbackY};
        out.pressure = 1.0;
        out.timeUs = platform::ingestClockUs();
    }
    return out;
}

std::size_t TabletInput::drain(double fallbackX, double fallbackY, const SampleFn& fn) {
    std::size_t n = 0;
    if (m_impl->live) {
        platform::SampleRing& ring = m_impl->win32.ring();
        platform::TabletSample s;
        while (ring.pop(s)) { // OLDEST FIRST -- the whole device stream, in order
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
    if (!m_impl->live)
        return 0;
    // NOTE every sample, then let them go: none is wanted by a stroke, but they are the pen telling
    // us where it IS and how fast it is talking -- the test area (§8), the resolved rate, and
    // stylusInProximity() all read that. EVERY sample, not just the newest: note() derives the rate
    // from the gaps between the samples it is shown, and those gaps are only the DEVICE's if it is
    // shown all of them.
    platform::SampleRing& ring = m_impl->win32.ring();
    platform::TabletSample s;
    std::size_t n = 0;
    while (ring.pop(s)) {
        m_impl->note(m_impl->ingest(s));
        ++n;
    }
    return n;
}

void TabletInput::discardBuffered() { pumpReadout(); }

bool TabletInput::stylusInProximity() const noexcept {
    if (m_lastSampleTimeUs == 0)
        return false;
    // Sample age, exactly the Linux heuristic, and deliberately NOT a latched proximity flag from
    // the driver: a missed proximity-out would leave that flag saying a stylus is on the tablet
    // forever, and this is what a press branches on to defer its first dab -- so a MOUSE press
    // would stop painting until it was dragged. A hovering pen streams continuously; a stale
    // reading means the user reached for the mouse. 150 ms is far below human reaction time.
    const std::uint64_t now = platform::ingestClockUs();
    return now <= m_lastSampleTimeUs || (now - m_lastSampleTimeUs) < 150'000;
}

platform::TabletPositionDiag TabletInput::positionDiag() const {
    // All-zero, like Wayland and macOS: there is no valuator-versus-server position guess to
    // diagnose here (the X11 mapping guard, §3.5). WinTab's mapping IS a guess, but a per-device
    // sticky one made once at init from the driver's own declaration -- not per sample -- and the
    // escape hatch for a wrong one is MOSAIC_TABLET_MAPPING (§5a), not a runtime cross-check.
    return {};
}

std::string TabletInput::backendName() const {
    return m_impl->live ? std::string(m_impl->win32.name()) : std::string();
}

std::vector<TabletDeviceInfo> TabletInput::devices() const {
    std::vector<TabletDeviceInfo> out;
    if (!m_impl->live)
        return out;
    for (const platform::TabletWin32DeviceInfo& d : m_impl->win32.devices())
        out.push_back({d.name, d.hasTool ? std::string(toolName(d.tool)) : std::string(),
                       d.valuators});
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

#else // Linux: XInput2 + zwp_tablet_v2

#include "common/log.hpp"
#include "platform/native_window.hpp"
#include "platform/tablet.hpp"
#include "platform/tablet_wayland.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

// Xlib LAST, deliberately: <X11/Xlib.h> #defines Status/Bool/None/True/False as bare macros, and
// anything it is included ahead of gets to fight them. Every header above is clean of it.
#include "platform/tablet_x11.hpp"

namespace mosaic::ui {
namespace {

spdlog::logger& tabletLog() {
    static const auto logger = common::log::category("tablet");
    return *logger;
}

// FLTK reports event coordinates in its own GUI-SCALED units, while both backends report the
// surface's own: XI2's event_x/event_y are device pixels on the X window, and zwp_tablet_v2's motion
// is surface-local. FLTK divides by the screen's scale factor before it hands you Fl::event_x(), so
// the wiring must divide by the same thing or a scaled UI paints where the pen is not. At the
// overwhelmingly common scale of 1 this is exactly a no-op.
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

} // namespace

// ---------------------------------------------------------------------------------------------
// TabletPointerSynth -- the pen, made into an FLTK pointer
// ---------------------------------------------------------------------------------------------

void TabletPointerSynth::addWindow(std::uint64_t surface, Fl_Window* win) {
    if (surface == 0 || win == nullptr || windowFor(surface) != nullptr)
        return;
    m_windows.emplace_back(surface, win);
}

void TabletPointerSynth::removeWindow(std::uint64_t surface) {
    std::erase_if(m_windows, [&](const auto& e) { return e.first == surface; });
    if (m_lastSurface == surface)
        m_lastSurface = 0; // never dispatch into a window that is going away
}

Fl_Window* TabletPointerSynth::windowFor(std::uint64_t surface) const {
    if (surface == 0)
        return nullptr;
    for (const auto& [id, w] : m_windows)
        if (id == surface)
            return w;
    return m_resolve ? m_resolve(surface) : nullptr;
}

bool TabletPointerSynth::send(int event, const platform::TabletSample& s, double scale) {
    Fl_Window* target = windowFor(s.surface);
    if (target == nullptr)
        return false; // a surface we do not drive
    const double k = scale > 0.0 ? 1.0 / scale : 1.0;
    const int ex = static_cast<int>(std::lround(s.pos.x * k));
    const int ey = static_cast<int>(std::lround(s.pos.y * k));
    Fl::e_x = ex;
    Fl::e_y = ey;
    Fl::e_x_root = target->x() + ex; // menus and popups place themselves off the ROOT coordinates
    Fl::e_y_root = target->y() + ey;
    // Preserve the keyboard modifiers: wl_keyboard is unaffected by any of this, and FLTK keeps
    // shift/ctrl/alt in the same word as the buttons. Only the tip's bit is ours to write.
    const bool down = (s.buttons & 1u) != 0;
    Fl::e_state = (Fl::e_state & ~FL_BUTTON1) | (down ? FL_BUTTON1 : 0);
    if (event == FL_PUSH || event == FL_RELEASE) {
        // FLTK carries the mouse button in the KEYSYM slot -- Fl::event_button() is defined as
        // e_keysym - FL_Button, and its own platform drivers set it exactly this way.
        Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
        Fl::e_is_click = 1;
        Fl::e_clicks = 0; // no double-click detection for the pen; a tap is a tap
    }
    m_lastSurface = s.surface;
    Fl::handle(event, target); // FLTK routes it, sends its own FL_ENTER/FL_LEAVE, sets pushed()
    return true;
}

void TabletPointerSynth::leave() {
    Fl_Window* target = windowFor(m_lastSurface); // re-resolved: a popup may be gone by now
    m_lastSurface = 0;
    if (target == nullptr)
        return;
    Fl::e_state &= ~FL_BUTTON1;
    Fl::handle(FL_LEAVE, target);
}

// ---------------------------------------------------------------------------------------------
// Impl -- both backends, the Wayland sink adapter, and the X11 system handler
// ---------------------------------------------------------------------------------------------

struct TabletInput::Impl : platform::TabletStrokeSink {
    TabletInput* owner = nullptr;
    Fl_Window* win = nullptr;

    platform::TabletX11 x11;
    platform::TabletWayland wayland;
    bool x11Live = false;
    bool waylandLive = false;
    bool handlerInstalled = false;

    TabletStrokeGate gate; // the surface rule; see the header

    // Wayland's ring. The BACKEND's stays empty (it delivers through the sink), so the wiring keeps
    // its own and fills it from the sink -- which makes the ring the single sample path on both
    // platforms, and lets the canvas drain it identically.
    platform::SampleRing wlRing{512};

    // The pen as an FLTK pointer. Wayland only: on X11 the server already moves the core pointer
    // with the pen, and a second, synthetic stream would double every event in the program.
    TabletPointerSynth synth;

    [[nodiscard]] bool backendLive() const noexcept { return x11Live || waylandLive; }
    [[nodiscard]] platform::SampleRing* activeRing() noexcept {
        if (x11Live)
            return &x11.ring();
        if (waylandLive)
            return &wlRing;
        return nullptr;
    }
    [[nodiscard]] bool fromCanvas(const platform::TabletSample& s) const noexcept {
        return gate.isCanvas(s.surface);
    }

    // ---- ingest: the ONE policy seam (§7), shared by the drain and the sink ------------------
    // A REAL device sample -> the engine's input. Position converts out of surface pixels into
    // FLTK's logical units; pressure/tilt go through the user's policy. Everything else rides
    // along: rotation, tangential pressure, and OUR ingest timestamp (§5).
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
        // The default policy is an identity over every field it touches, so skip it outright rather
        // than push a sample through a remap and a LUT that cannot change it.
        return owner->m_policy.isIdentity() ? in : owner->m_policy.apply(in);
    }

    // Note it for Settings -> Tablet's live readout (§8) -- every real sample, from either backend,
    // whether or not a stroke is in flight. This is what makes the test area answer "is my tablet
    // working" while the pen merely hovers.
    void note(const core::brush::StrokeInput& in) {
        if (owner->m_lastSampleTimeUs != 0 && in.timeUs > owner->m_lastSampleTimeUs) {
            const double dtUs = static_cast<double>(in.timeUs - owner->m_lastSampleTimeUs);
            // A time-constant EMA over the arrival interval, the SpeedSmoother's shape: the readout
            // must settle in a few samples at 200 Hz without flickering on one late frame.
            m_intervalUs = (m_intervalUs > 0.0) ? (0.9 * m_intervalUs + 0.1 * dtUs) : dtUs;
        }
        owner->m_lastSample = in;
        owner->m_lastSampleTimeUs = in.timeUs;
    }
    double m_intervalUs = 0.0;

    // ---- the Wayland sink: fill the ring, then BE the pointer the compositor withdrew -----------
    //
    // Binding zwp_tablet_manager_v2 makes KWin stop emulating pointer events for the pen (§4 finding
    // 4). The shipped answer was to let the backend own the stroke lifecycle itself and drive the
    // canvas directly -- which worked, and meant the pen could paint the canvas AND NOTHING ELSE:
    // it could not press a toolbar button, open a menu, reach a dialog, or even move the reticle on
    // hover (nothing was generating FL_MOVE).
    //
    // So instead: put the sample in the ring, then hand FLTK the event the compositor declined to
    // send. The pen becomes an ordinary pointer that happens to carry pressure, every tool works
    // under it, and the two platforms run the SAME lifecycle -- FLTK routes, the canvas drains.
    //
    // The ring push comes FIRST, deliberately: the canvas's FL_PUSH resolves its sample by draining,
    // so the contact sample has to be there before the press it belongs to. (X11 cannot do this --
    // the core ButtonPress overtakes the XI2 events carrying the same contact, which is why the
    // canvas defers its first dab there. Here the order is ours to choose, so we choose the right
    // one.)
    void tabletStrokeBegin(const platform::TabletSample& s) override { dispatch(FL_PUSH, s); }
    void tabletStrokeMotion(const platform::TabletSample& s) override { dispatch(FL_DRAG, s); }
    void tabletStrokeEnd(const platform::TabletSample& s) override { dispatch(FL_RELEASE, s); }
    void tabletHover(const platform::TabletSample& s) override {
        dispatch(FL_MOVE, s); // the canvas discards these at FL_MOVE, exactly as it does on X11
    }
    void tabletProximityOut() override {
        gate.proximityOut();
        // The pen left the tablet. FL_LEAVE is what tells the canvas the pointer is gone -- the
        // reticle goes away, the status readout clears -- and FLTK will not invent it for us.
        synth.leave();
    }

    void dispatch(int event, const platform::TabletSample& s) {
        wlRing.push(s);
        synth.send(event, s, guiScale(synth.windowFor(s.surface)));
    }
};

namespace {

// The X11 system handler. FLTK hands us every raw XEvent BEFORE dispatch; we fold the XI2 cookies
// into the ring and ALWAYS return 0 -- never swallowing, because the core pointer events FLTK reads
// from the same stream are what keep the stroke lifecycle alive (§3.1). One handler, keyed on the
// TabletInput::Impl it was installed with.
int x11SystemHandler(void* event, void* data) {
    auto* impl = static_cast<TabletInput::Impl*>(data);
    if (impl != nullptr && event != nullptr)
        impl->x11.handleEvent(*static_cast<XEvent*>(event));
    return 0; // NEVER consume
}

} // namespace

// ---------------------------------------------------------------------------------------------

TabletInput::TabletInput() : m_impl(std::make_unique<Impl>()) { m_impl->owner = this; }

TabletInput::~TabletInput() {
    if (m_impl->handlerInstalled)
        Fl::remove_system_handler(x11SystemHandler);
}

void TabletInput::init(Fl_Window* win) {
    if (m_impl->x11Live || m_impl->waylandLive)
        return; // already up
    m_impl->win = win;

    platform::NativeSurfaceHandle handle;
    std::string err;
    if (!platform::nativeSurfaceHandle(win, handle, err)) {
        tabletLog().warn("no native handle for the tablet backend: {}", err);
        return; // no backend: the canvas keeps painting from synthesized pressure-1 samples (§3.2)
    }

    // The canvas's own id, in whatever the platform calls one. Every sample is tagged with the
    // window it came from, and this is the one -- and only one -- a stroke may be fed from.
    const auto canvasId = reinterpret_cast<std::uint64_t>(handle.window);
    m_impl->gate.setCanvasSurface(canvasId);

    // The canvas is registered by hand because it is the one window the resolver below CANNOT find:
    // it is an FLTK sub-window, and Fl::next_window walks top-levels only. Everything else -- the
    // top-level carrying the toolbar and the menu bar, a dialog, a menu popup conjured a moment ago
    // -- is resolved live, against FLTK's own window list. That is the difference between the pen
    // working on the canvas and the pen working in the PROGRAM: registering surfaces one at a time
    // registers the ones you thought of, and the pen over a menu produces nothing at all.
    m_impl->synth.addWindow(canvasId, win);
    m_impl->synth.setResolver([](std::uint64_t surface) -> Fl_Window* {
        for (Fl_Window* w = Fl::first_window(); w != nullptr; w = Fl::next_window(w)) {
            platform::NativeSurfaceHandle h;
            std::string e;
            if (platform::nativeSurfaceHandle(w, h, e) &&
                reinterpret_cast<std::uint64_t>(h.window) == surface)
                return w;
        }
        return nullptr;
    });

    if (handle.system == platform::WindowSystem::Wayland) {
        // The backend binds its own wl_seat + zwp_tablet_manager_v2 and listens on the surface we
        // hand it -- the FLTK parent, because the Vulkan subsurface has an empty input region and
        // the pen falls through to us (§4 finding 2).
        m_impl->waylandLive =
            m_impl->wayland.init(static_cast<wl_display*>(handle.display),
                                 static_cast<wl_surface*>(handle.window));
        if (m_impl->waylandLive) {
            m_impl->wayland.setSink(m_impl.get());
            tabletLog().info("tablet backend: {}", m_impl->wayland.name());
        } else {
            tabletLog().info("no zwp_tablet_v2 on this compositor; pressure unavailable");
        }
        return;
    }

    auto* display = static_cast<Display*>(handle.display);
    const auto window = static_cast<Window>(reinterpret_cast<std::uintptr_t>(handle.window));
    m_impl->x11Live = m_impl->x11.init(display, window);
    if (!m_impl->x11Live) {
        tabletLog().info("no XInput2 on this display; pressure unavailable");
        return;
    }
    // The raw-XEvent tap. It lives HERE and not in platform/ -- the platform tablet layer is
    // FLTK-free, and Fl::add_system_handler is as FLTK as it gets.
    Fl::add_system_handler(x11SystemHandler, m_impl.get());
    m_impl->handlerInstalled = true;
    tabletLog().info("tablet backend: {} ({} device(s))", m_impl->x11.name(),
                     m_impl->x11.devices().size());
}

// X11 only, and by necessity: XI2 event selection is PER-WINDOW, so a window nobody selected on
// delivers no tablet events at all. Wayland needs nothing here -- the compositor reports the pen over
// every surface of ours already, and the wiring resolves each one to its window on the spot.
void TabletInput::watch(Fl_Window* win) {
    platform::NativeSurfaceHandle handle;
    std::string err;
    if (!m_impl->x11Live || win == nullptr || !platform::nativeSurfaceHandle(win, handle, err))
        return; // not shown yet, or not the X11 backend: nothing to select on
    m_impl->x11.watchWindow(
        static_cast<Window>(reinterpret_cast<std::uintptr_t>(handle.window)));
}

void TabletInput::unwatch(Fl_Window* win) {
    platform::NativeSurfaceHandle handle;
    std::string err;
    if (!m_impl->x11Live || win == nullptr || !platform::nativeSurfaceHandle(win, handle, err))
        return;
    const auto id = reinterpret_cast<std::uint64_t>(handle.window);
    if (m_impl->gate.isCanvas(id))
        return; // never unwatch the canvas: it is what the backend was brought up for
    m_impl->x11.unwatchWindow(static_cast<Window>(static_cast<std::uintptr_t>(id)));
}

void TabletInput::setToolCursor(Fl_Cursor cursor) {
    if (!m_impl->waylandLive)
        return; // X11: the pen drives the core pointer, whose cursor FLTK already sets
    // FLTK's cursor vocabulary is much wider than what a cursor-shape protocol names, and the ones
    // it does not name are not worth a bespoke cursor surface each: they collapse to the arrow.
    // FL_CURSOR_NONE is the load-bearing one -- it is what the canvas asks for under a brush.
    platform::TabletCursor c = platform::TabletCursor::Default;
    switch (cursor) {
    case FL_CURSOR_NONE: c = platform::TabletCursor::Hidden; break;
    case FL_CURSOR_CROSS: c = platform::TabletCursor::Crosshair; break;
    case FL_CURSOR_INSERT: c = platform::TabletCursor::Text; break;
    case FL_CURSOR_MOVE: c = platform::TabletCursor::Move; break;
    case FL_CURSOR_HAND: c = platform::TabletCursor::Pointer; break;
    case FL_CURSOR_WAIT: c = platform::TabletCursor::Wait; break;
    default: break;
    }
    m_impl->wayland.setCursor(c);
}

bool TabletInput::ringDriven() const noexcept { return m_impl->backendLive(); }

core::brush::StrokeInput TabletInput::pressSample(double fallbackX, double fallbackY) {
    core::brush::StrokeInput out;
    bool got = false;
    if (platform::SampleRing* ring = m_impl->activeRing()) {
        platform::TabletSample s;
        while (ring->pop(s)) { // drain to the NEWEST: the press is where the pen is now
            const core::brush::StrokeInput in = m_impl->ingest(s);
            m_impl->note(in); // note every sample; only the canvas's may BEGIN a stroke
            if (m_impl->fromCanvas(s)) {
                out = in;
                got = true;
            }
        }
    }
    if (!got) {
        // No stylus in proximity -- a mouse. Pressure 1, NOT 0 (§3.2), or size/flow dynamics would
        // silently collapse every mouse stroke to nothing. Deliberately NOT run through the policy:
        // there is no device pressure here for a pressure curve to reshape, and a curve mapping
        // 1.0 -> 0.8 must not quietly weaken the mouse.
        out = core::brush::StrokeInput{};
        out.pos = {fallbackX, fallbackY};
        out.pressure = 1.0;
        out.timeUs = platform::ingestClockUs();
    }
    return out;
}

std::size_t TabletInput::drain(double fallbackX, double fallbackY, const SampleFn& fn) {
    std::size_t n = 0;
    if (platform::SampleRing* ring = m_impl->activeRing()) {
        platform::TabletSample s;
        while (ring->pop(s)) { // OLDEST FIRST -- the whole ~200 Hz segment, in order
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
    platform::SampleRing* ring = m_impl->activeRing();
    if (ring == nullptr)
        return 0;
    // NOTE every sample, then let them go. None is wanted by a stroke -- but they are the device
    // telling us where the pen IS and how fast it is talking, and three things depend on that:
    // Settings -> Tablet's test area (§8: it must answer "is my tablet working" without painting),
    // the resolved sample RATE, and stylusInProximity(), which decides how a press begins.
    //
    // EVERY sample, not just the newest: note() derives the rate from the gap between the samples
    // it is shown, and those gaps are only the DEVICE's if it is shown all of them. Noting one per
    // pump would make a 200 Hz tablet report the pump's own rate -- and "your tablet is being read
    // at 60 Hz" is precisely the diagnosis this readout exists to deliver.
    platform::TabletSample s;
    std::size_t n = 0;
    while (ring->pop(s)) {
        m_impl->note(m_impl->ingest(s));
        ++n;
    }
    return n;
}

void TabletInput::discardBuffered() {
    // The canvas's name for the same drain, and the same one body: samples arrive at ~200 Hz
    // whenever the pen is over the window -- hovering, dragging a lasso, anything -- and nobody will
    // ever paint with those. Dropping them at each non-stroke event keeps SampleRing::overwritten()
    // an honest STALL counter instead of a count of samples nobody wanted.
    pumpReadout();
}

bool TabletInput::stylusInProximity() const noexcept {
    if (m_lastSampleTimeUs == 0)
        return false;
    // "A real device sample arrived just now." A hovering pen streams continuously (evdev's own
    // filtering plus the stack's ~50 ms silent-tool proximity-out guarantee it), so a fresh sample
    // means a stylus; a stale one means the user has moved to the mouse. Anything under the forced
    // proximity-out window would do; 150 ms is comfortably clear of it and still far below human
    // reaction time, so it can never mistake a mouse click for a pen.
    const std::uint64_t now = platform::ingestClockUs();
    return now <= m_lastSampleTimeUs || (now - m_lastSampleTimeUs) < 150'000;
}

platform::TabletPositionDiag TabletInput::positionDiag() const {
    if (!m_impl->x11Live)
        return {};
    return m_impl->x11.positionDiag();
}

std::string TabletInput::backendName() const {
    if (m_impl->x11Live)
        return std::string(m_impl->x11.name());
    if (m_impl->waylandLive)
        return std::string(m_impl->wayland.name());
    return {};
}

std::vector<TabletDeviceInfo> TabletInput::devices() const {
    std::vector<TabletDeviceInfo> out;
    if (m_impl->x11Live) {
        for (const platform::Xi2Device& d : m_impl->x11.devices()) {
            std::string axes;
            const auto add = [&axes](const char* name, bool present) {
                if (!present)
                    return;
                if (!axes.empty())
                    axes += ", ";
                axes += name;
            };
            add("pressure", d.pressure.present());
            add("tilt", d.tiltX.present() && d.tiltY.present());
            add("wheel", d.wheel.present());
            out.push_back({d.name, toolName(d.tool), axes});
        }
        return out;
    }
    if (m_impl->waylandLive) {
        // zwp_tablet_v2 announces the TABLET, and its tools arrive (and leave) as the pen touches
        // down. The compositor has already normalized every axis, so there is no per-axis range to
        // report and nothing useful to say about valuators here -- unlike XI2, where the device's
        // declared ranges ARE the diagnostic.
        const std::string_view name = m_impl->wayland.tabletName();
        if (!name.empty())
            out.push_back({std::string(name), "", ""});
    }
    return out;
}

double TabletInput::sampleRateHz() const noexcept {
    if (m_impl->m_intervalUs <= 0.0 || m_lastSampleTimeUs == 0)
        return 0.0;
    // Nothing has arrived recently -> the pen is away, and a stale rate would be a lie. 250 ms is
    // well past the gap even a slow 60 Hz stream leaves, and well inside the ~50 ms forced
    // proximity-out an event-silent tool gets anyway (§4 finding 5).
    const std::uint64_t now = platform::ingestClockUs();
    if (now > m_lastSampleTimeUs && (now - m_lastSampleTimeUs) > 250'000)
        return 0.0;
    return 1'000'000.0 / m_impl->m_intervalUs;
}

} // namespace mosaic::ui

#endif // host selection
