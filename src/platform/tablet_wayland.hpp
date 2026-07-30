#pragma once

#include "common/geometry.hpp"
#include "platform/tablet.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

// The Wayland / zwp_tablet_v2 tablet backend (docs/tablet.md §4) -- the path native-Wayland
// sessions hit. Unlike the X11 backend, this one OWNS the stroke lifecycle: the §4 built-note
// (finding 4) proved that the moment a client binds zwp_tablet_manager_v2, KWin stops synthesizing
// wl_pointer events for the pen, so FLTK sees NOTHING during a pen stroke (no FL_PUSH/DRAG/RELEASE).
// The backend therefore drives the canvas itself, through a TabletStrokeSink, instead of filling a
// ring that an FL_DRAG drains (that is the X11 design, §3.1).
//
// Split for headless testing (§9), the same way tablet_x11.hpp is: everything from a wire event to
// a normalized TabletSample and a lifecycle decision -- the per-frame accumulation, the axis
// normalization, the down/up/hover transitions -- lives in WaylandTool and the free functions here,
// as pure logic over plain values (doubles, ints), so the tests drive it with hand values and never
// open a wl_display. Only TabletWayland itself (registry bind + seat/tool proxies + dispatch) needs
// a live compositor, and that half is what tools/tablet_spike{,_fltk} proved against KWin.
//
// This header is deliberately free of <wayland-client.h> and the generated protocol header: the wl
// proxies hide behind TabletWayland's pimpl, and the protocol enum values are duplicated (and
// compile-time pinned to the generated ones, in the .cpp) so a consumer -- the headless test, the
// FLTK-free wiring TU -- includes this without the generated include path. The two wl handles
// init() needs are forward-declared as the opaque C structs they are (in C++, the struct tag is
// injected as a type name), so no wayland header is pulled in here.
struct wl_display;
struct wl_surface;

namespace mosaic::platform {

// zwp_tablet_tool_v2.type values (tablet-unstable-v2). Duplicated from the generated header so the
// classifier stays testable without the protocol include; tablet_wayland.cpp static_asserts each
// against ZWP_TABLET_TOOL_V2_TYPE_* so this copy can never drift from the wire ABI.
namespace wl_tool_type {
inline constexpr std::uint32_t kPen = 0x140;
inline constexpr std::uint32_t kEraser = 0x141;
inline constexpr std::uint32_t kBrush = 0x142;
inline constexpr std::uint32_t kPencil = 0x143;
inline constexpr std::uint32_t kAirbrush = 0x144;
inline constexpr std::uint32_t kFinger = 0x145;
inline constexpr std::uint32_t kMouse = 0x146;
inline constexpr std::uint32_t kLens = 0x147;
} // namespace wl_tool_type

// Map a protocol tool type onto the unified model's Tool enum. Pen/brush/pencil all paint like a
// pen; the lens cursor is the puck; a bare mouse/finger is the non-stylus fallback. Unknown future
// types default to Pen (a stylus is the safe assumption for a drawing tool).
[[nodiscard]] TabletSample::Tool waylandToolType(std::uint32_t protocolType) noexcept;

// Map a Linux evdev button code (as delivered by zwp_tablet_tool_v2.button) to a TabletSample
// button bit, or -1 to drop it. The tip is NOT here: it rides bit 0 straight off the down/up
// contact state (WaylandTool::sample), keeping "bit 0 == button 1 == tip" identical to the X11
// backend. The barrel buttons map to the X11 button-2/3/4 bits so a preset that reads "the lower
// barrel button" means the same thing on either backend. Codes are pinned to <linux/input-event-
// codes.h> in the .cpp.
[[nodiscard]] int waylandButtonBit(std::uint32_t code) noexcept;

// What a lifecycle-owning backend calls to drive a stroke when there is no host pointer stream to
// hang it on. The wiring layer implements it -- it is the ONLY part of the tablet path allowed to
// touch FLTK/Vulkan; the platform backend never does. The samples handed here are normalized
// (TabletSample) but NOT yet run through the core TabletPolicy: the wiring applies that at ingest,
// exactly where the X11 ring drain applies it, so both backends share one policy seam (§10 step 5).
class TabletStrokeSink {
public:
    virtual ~TabletStrokeSink() = default;

    virtual void tabletStrokeBegin(const TabletSample& s) = 0;  // tip made contact (down)
    virtual void tabletStrokeMotion(const TabletSample& s) = 0; // moving while in contact
    virtual void tabletStrokeEnd(const TabletSample& s) = 0;    // tip lifted (up); s.inProximity
                                                                //   still says if the pen hovers on
    virtual void tabletHover(const TabletSample& s) = 0;        // in proximity, not in contact
    virtual void tabletProximityOut() = 0;                      // pen left proximity (no stroke)
};

// One tablet tool's running state and per-frame accumulator. The zwp_tablet_v2 protocol streams
// partial updates (motion, then pressure, then tilt, ...) that are only coherent at the `frame`
// boundary -- the spec: "all events within a frame should be considered one hardware event" -- so
// the wire events fold into this state and frame() emits ONE sample. Pure over plain values: the
// live listeners convert wl_fixed to double and call these; the tests call them directly.
//
// Axis values PERSIST across frames (like the X11 backend's running valuators): a frame that omits
// an axis keeps its last value. Only the lifecycle transitions are per-frame and cleared by frame().
class WaylandTool {
public:
    // --- the descriptive burst, before the tool's first frame (protocol: sent before `done`) ---
    void setType(std::uint32_t protocolType) noexcept { m_tool = waylandToolType(protocolType); }
    void setSerial(std::uint32_t hi, std::uint32_t lo) noexcept {
        m_serial = (static_cast<std::uint64_t>(hi) << 32) | lo;
    }
    void addCapability(std::uint32_t capability) noexcept;

    // --- per-frame lifecycle + axis updates (wl_fixed already converted to double) ---
    // `surface` is the opaque id of the one the tool entered; it rides on every sample the tool
    // then produces, because `pos` is SURFACE-LOCAL and we watch more than one (tablet.hpp).
    void proximityIn(std::uint64_t surface) noexcept {
        m_inProximity = true;
        m_surface = surface;
    }
    void proximityOut() noexcept;
    void down() noexcept;
    void up() noexcept;
    void motion(double x, double y) noexcept { m_pos = {x, y}; }
    void pressure(std::uint32_t raw) noexcept;
    void tilt(double xDeg, double yDeg) noexcept {
        m_xTiltDeg = xDeg;
        m_yTiltDeg = yDeg;
    }
    void rotation(double deg) noexcept { m_rotationDeg = deg; }
    void slider(std::int32_t position) noexcept;
    void button(std::uint32_t code, bool pressed) noexcept;

    // Build the normalized sample for the accumulated state, stamping OUR timeUs (§5). Pure -- the
    // tests read it directly; frame() uses it.
    [[nodiscard]] TabletSample sample(std::uint64_t timeUs) const noexcept;

    // Close a frame: build the sample and dispatch the one lifecycle callback this frame implies
    // (down > up > motion-while-down > proximity-out > hover), then clear the per-frame
    // transitions. A null sink updates state but dispatches nothing (the backend is not wired yet).
    void frame(std::uint64_t timeUs, TabletStrokeSink* sink) noexcept;

    [[nodiscard]] bool inProximity() const noexcept { return m_inProximity; }
    [[nodiscard]] bool isDown() const noexcept { return m_down; }
    [[nodiscard]] TabletSample::Tool tool() const noexcept { return m_tool; }
    [[nodiscard]] std::uint64_t surface() const noexcept { return m_surface; }

private:
    common::Vec2 m_pos{};
    std::uint64_t m_surface = 0; // which of our surfaces the tool is over (0 = none)
    double m_pressureRaw = 0.0;  // [0,65535] as delivered by the compositor
    bool m_hasPressure = false;  // no pressure capability/event -> report 1.0, never 0 (§3.2)
    double m_xTiltDeg = 0.0;     // zwp_tablet_v2 already speaks degrees -- pass through (tablet.hpp)
    double m_yTiltDeg = 0.0;
    double m_rotationDeg = 0.0;
    double m_sliderRaw = 0.0;    // [-65535,65535], 0 = neutral
    bool m_hasSlider = false;    // no slider -> tangential 0.0; present-at-neutral -> 0.5
    std::uint32_t m_barrelButtons = 0; // bits 1.. from button events; bit 0 (tip) added in sample()
    TabletSample::Tool m_tool = TabletSample::Tool::Pen;
    std::uint64_t m_serial = 0;
    bool m_inProximity = false;
    bool m_down = false;
    bool m_downThisFrame = false;
    bool m_upThisFrame = false;
    bool m_proxOutThisFrame = false;
};

// The live backend. FLTK-free: init() takes the wl_display and OUR wl_surface from the caller (the
// wiring TU passes fl_wl_display() and fl_wl_surface(fl_wl_xid(win))), exactly as TabletX11::init
// takes a Display*/Window. The wl proxies live in the pimpl so this header stays protocol-header-
// free. Tablet events dispatch from the host's Fl::wait() (the proxies ride the default queue after
// the private-queue hand-back), so there is no event loop here.
class TabletWayland final : public TabletBackend {
public:
    TabletWayland();
    ~TabletWayland() override;
    TabletWayland(const TabletWayland&) = delete;
    TabletWayland& operator=(const TabletWayland&) = delete;

    // Binds a private wl_seat + zwp_tablet_manager_v2 and opens the tablet seat. `display` is the
    // live Wayland connection; `surface` is the FLTK parent surface tablet events target (the
    // Vulkan subsurface has an empty input region, so proximity falls through to the parent -- §4
    // finding 2). False = not a Wayland session, or no tablet manager global: the caller falls
    // back to the X11 backend or synthesized pressure-1 samples. Takes opaque wl_* pointers.
    bool init(wl_display* display, wl_surface* surface);

    // Also deliver tablet events for `surface` -- another top-level surface of OURS. The pen over
    // Settings -> Tablet's dialog must reach the test area (§8); a tool that enters a surface we do
    // not watch is ignored outright. Samples carry their source in TabletSample::surface, and their
    // `pos` is THAT surface's -- never the canvas's.
    // What the pen's cursor looks like. A client that binds the tablet manager OWNS its tool cursor:
    // the compositor stops drawing one, and one that never sets it shows the compositor's default
    // (KWin: a crosshair, over every pixel of the UI). FLTK cannot do this for us -- its cursor calls
    // go through wl_pointer.set_cursor, and a tablet tool has no wl_pointer. Applied to every tool
    // in proximity, and re-applied whenever one enters (the compositor forgets it on the way out).
    void setCursor(TabletCursor cursor);

    void watchSurface(wl_surface* surface);
    void unwatchSurface(wl_surface* surface);

    // The consumer the backend drives. Must be set (by the wiring layer) for strokes to reach the
    // canvas; until then samples are dropped (state still updates, so a poll of a tool works).
    void setSink(TabletStrokeSink* sink) noexcept;

    [[nodiscard]] bool available() const noexcept override;
    [[nodiscard]] std::string_view name() const noexcept override { return "wayland/zwp_tablet_v2"; }

    // Satisfies the TabletBackend contract; STAYS EMPTY on Wayland. Samples are delivered through
    // the sink because the backend owns the lifecycle (§4 finding 4) -- nothing pushes here, so
    // overwritten() cannot lie about a drain that does not exist. A consumer that wants samples
    // subscribes a sink, it does not poll this.
    [[nodiscard]] SampleRing& ring() noexcept override;

    // The tablet's advertised name, for Settings->Tablet's device row (§8). Empty until a tablet
    // is announced.
    [[nodiscard]] std::string_view tabletName() const noexcept;

    // Opaque; defined in the .cpp. Public only so the backend's C-style wl listener callbacks
    // (free functions) can name it -- nothing about it leaks, it is merely forward-declared here.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace mosaic::platform
