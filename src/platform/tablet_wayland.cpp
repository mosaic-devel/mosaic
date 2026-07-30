#include "platform/tablet_wayland.hpp"

#include "common/log.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <linux/input-event-codes.h> // BTN_STYLUS / BTN_STYLUS2 / BTN_STYLUS3
#include <wayland-client.h>

#include "cursor-shape-v1-client.h"    // the tool's cursor: we own it, nobody else will draw one
#include "tablet-unstable-v2-client.h" // wayland-scanner output; PRIVATE include dir (CMakeLists)

namespace mosaic::platform {

namespace {

spdlog::logger& plog() {
    static const auto logger = common::log::category("platform");
    return *logger;
}

[[nodiscard]] double clamp01(double v) noexcept { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

// zwp_tablet_tool_v2.capability values -- the two whose ABSENCE needs a non-zero default (see
// WaylandTool::addCapability). Pinned to the generated enum so this copy cannot drift.
namespace wl_tool_cap {
constexpr std::uint32_t kPressure = 2;
constexpr std::uint32_t kSlider = 5;
} // namespace wl_tool_cap

// Compile-time proof that the header's duplicated protocol values match the wire ABI. If the
// generated header ever changes (a new protocol revision), these fail here instead of silently
// misclassifying a tool at runtime.
static_assert(wl_tool_type::kPen == ZWP_TABLET_TOOL_V2_TYPE_PEN);
static_assert(wl_tool_type::kEraser == ZWP_TABLET_TOOL_V2_TYPE_ERASER);
static_assert(wl_tool_type::kBrush == ZWP_TABLET_TOOL_V2_TYPE_BRUSH);
static_assert(wl_tool_type::kPencil == ZWP_TABLET_TOOL_V2_TYPE_PENCIL);
static_assert(wl_tool_type::kAirbrush == ZWP_TABLET_TOOL_V2_TYPE_AIRBRUSH);
static_assert(wl_tool_type::kFinger == ZWP_TABLET_TOOL_V2_TYPE_FINGER);
static_assert(wl_tool_type::kMouse == ZWP_TABLET_TOOL_V2_TYPE_MOUSE);
static_assert(wl_tool_type::kLens == ZWP_TABLET_TOOL_V2_TYPE_LENS);
static_assert(wl_tool_cap::kPressure == ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE);
static_assert(wl_tool_cap::kSlider == ZWP_TABLET_TOOL_V2_CAPABILITY_SLIDER);

} // namespace

// ---------------------------------------------------------------------------------------------
// Pure normalization -- headless-tested, no compositor
// ---------------------------------------------------------------------------------------------

TabletSample::Tool waylandToolType(std::uint32_t protocolType) noexcept {
    switch (protocolType) {
    case wl_tool_type::kEraser:
        return TabletSample::Tool::Eraser;
    case wl_tool_type::kAirbrush:
        return TabletSample::Tool::Airbrush;
    case wl_tool_type::kLens:
        return TabletSample::Tool::Puck; // the Wacom lens cursor is the puck
    case wl_tool_type::kMouse:
    case wl_tool_type::kFinger:
        return TabletSample::Tool::Mouse; // not a stylus
    default:
        return TabletSample::Tool::Pen; // pen, brush, pencil, and any unknown future type
    }
}

int waylandButtonBit(std::uint32_t code) noexcept {
    switch (code) {
    case BTN_STYLUS: // 0x14b -> button 2 (lower barrel), X11 bit 1
        return 1;
    case BTN_STYLUS2: // 0x14c -> button 3 (upper barrel), X11 bit 2
        return 2;
    case BTN_STYLUS3: // 0x149 -> button 4, X11 bit 3
        return 3;
    default:
        return -1; // BTN_TOUCH is the tip (a down/up event, not a button); everything else dropped
    }
}

void WaylandTool::addCapability(std::uint32_t cap) noexcept {
    // Only the axes whose ABSENCE needs a non-zero default are gated: pressure (missing -> 1.0, so
    // a pressure-less digitizer does not paint nothing, §3.2) and slider (missing -> 0.0, though
    // present-at-neutral -> 0.5). Tilt/rotation default to 0 either way; distance/wheel are
    // unmodeled. A later axis EVENT also flips these, in case a compositor skips the capability
    // burst.
    if (cap == wl_tool_cap::kPressure)
        m_hasPressure = true;
    else if (cap == wl_tool_cap::kSlider)
        m_hasSlider = true;
}

void WaylandTool::proximityOut() noexcept {
    m_inProximity = false;
    m_down = false; // protocol sends `up` before `proximity_out` if it was down; belt-and-suspenders
    m_proxOutThisFrame = true;
}

void WaylandTool::down() noexcept {
    m_down = true;
    m_downThisFrame = true;
}

void WaylandTool::up() noexcept {
    m_down = false;
    m_upThisFrame = true;
}

void WaylandTool::pressure(std::uint32_t raw) noexcept {
    m_pressureRaw = static_cast<double>(raw);
    m_hasPressure = true; // a pressure event is itself proof of the capability
}

void WaylandTool::slider(std::int32_t position) noexcept {
    m_sliderRaw = static_cast<double>(position);
    m_hasSlider = true;
}

void WaylandTool::button(std::uint32_t code, bool pressed) noexcept {
    const int bit = waylandButtonBit(code);
    if (bit < 0)
        return; // unmapped (e.g. a puck's extra buttons) -- dropped, not crammed into a stray bit
    const std::uint32_t mask = 1u << bit;
    if (pressed)
        m_barrelButtons |= mask;
    else
        m_barrelButtons &= ~mask;
}

TabletSample WaylandTool::sample(std::uint64_t timeUs) const noexcept {
    TabletSample s;
    s.pos = m_pos;
    s.pressure = m_hasPressure ? clamp01(m_pressureRaw / 65535.0) : 1.0; // §3.2: 1.0, never 0
    s.xTilt = m_xTiltDeg; // already degrees off the wire -- pass through (tablet.hpp)
    s.yTilt = m_yTiltDeg;
    s.rotation = m_rotationDeg;
    // Slider [-65535,65535] two-sidedly onto [0,1]; neutral (0) -> 0.5, matching the X11 airbrush
    // wheel at its range midpoint. A missing slider stays 0.0 (rest), like a missing wheel on X11.
    s.tangentialPressure = m_hasSlider ? clamp01((m_sliderRaw + 65535.0) / 131070.0) : 0.0;
    s.tool = m_tool;
    s.toolSerial = m_serial;
    s.inProximity = m_inProximity;
    std::uint32_t buttons = m_barrelButtons;
    if (m_down)
        buttons |= 1u; // bit 0 = tip contact (button 1) -- cross-platform with the X11 tip button
    s.buttons = buttons;
    s.timeUs = timeUs;
    s.surface = m_surface; // `pos` is local to THIS surface; the wiring has to know which (§8)
    return s;
}

void WaylandTool::frame(std::uint64_t timeUs, TabletStrokeSink* sink) noexcept {
    if (sink != nullptr) {
        const TabletSample s = sample(timeUs);
        // One STROKE callback per frame. down/up win over a coincident move so a stroke's
        // endpoints are never swallowed.
        if (m_downThisFrame)
            sink->tabletStrokeBegin(s);
        else if (m_upThisFrame)
            sink->tabletStrokeEnd(s);
        else if (m_down)
            sink->tabletStrokeMotion(s);
        else if (m_inProximity)
            sink->tabletHover(s);
        // else: out of proximity with no transition -- nothing meaningful to report.

        // ⚠ The leave is NOT part of that chain, because the protocol batches `up` and
        // `proximity_out` into ONE frame when the pen lifts clear of the pad at a stroke's end --
        // and the old one-callback-per-frame chain delivered the End and SWALLOWED the leave.
        // Downstream that meant no FL_LEAVE: the canvas kept the pointer "inside", the reticle
        // stayed, and the pen's cursor came back over whatever surface it approached next still
        // wearing the canvas's HIDDEN -- the sometimes-invisible cursor on native Wayland
        // (user-reported 2026-07-14). After the End, deliberately: a stroke closes before its
        // pointer leaves, which is also the order FLTK needs (release clears pushed(), then leave).
        if (m_proxOutThisFrame)
            sink->tabletProximityOut();
    }
    m_downThisFrame = false;
    m_upThisFrame = false;
    m_proxOutThisFrame = false;
}

// ---------------------------------------------------------------------------------------------
// Live backend -- binds the compositor; behaviour proven by tools/tablet_spike_fltk
// ---------------------------------------------------------------------------------------------

struct TabletWayland::Impl {
    wl_display* display = nullptr;
    wl_surface* surface = nullptr; // OUR (FLTK parent) surface -- the canvas's
    wl_seat* seat = nullptr;
    zwp_tablet_manager_v2* manager = nullptr;
    zwp_tablet_seat_v2* tabletSeat = nullptr;
    TabletStrokeSink* sink = nullptr;
    SampleRing ring{2}; // stays empty; the sink is the delivery path on Wayland (header)
    std::string tabletName;
    bool available = false;

    // A tool proxy plus its state. Owned by `tools`; the listener's `data` is the raw ToolEntry*,
    // which is stable because the entries are heap-boxed (unique_ptr) and never move on push_back.
    struct ToolEntry {
        zwp_tablet_tool_v2* proxy = nullptr;
        WaylandTool tool;
        Impl* owner = nullptr;
        // The serial of the tool's last proximity_in. set_cursor is only valid against it, so it is
        // kept and the current cursor is re-applied on every entry (the compositor forgets ours the
        // moment the tool leaves).
        std::uint32_t proximitySerial = 0;
        // What this tool's cursor was last actually SENT as -- per tool, not per app. `applied`
        // false means "the compositor is not holding our cursor", which is the state after every
        // proximity_out (the protocol drops it) and before the first proximity_in. Tracking it
        // per tool is what makes the wanted value re-assertable: a value dedup on the app-wide
        // `cursor` alone silently dropped every change made while the pen was off the tablet, and
        // a pen that comes back to a tool whose wanted value never changed got nothing at all --
        // the compositor's own default (KWin: a crosshair) or a stale shape. That is the
        // "the cursor randomly stops being what it should be, then comes back" report.
        bool applied = false;
        TabletCursor appliedCursor = TabletCursor::Default;
        wp_cursor_shape_device_v1* shapeDevice = nullptr; // lazily made; null if the compositor
                                                          // has no cursor-shape protocol
    };
    std::vector<std::unique_ptr<ToolEntry>> tools;
    std::vector<zwp_tablet_v2*> tablets;

    wp_cursor_shape_manager_v1* cursorShape = nullptr;
    TabletCursor cursor = TabletCursor::Default; // what the app last asked for

    // Tell the compositor what this tool's cursor is. A tablet client OWNS its tool cursor -- one
    // that never calls this gets the compositor's default (KWin: a crosshair, over everything).
    void applyCursor(ToolEntry& e) {
        if (e.proxy == nullptr || e.proximitySerial == 0 || !e.tool.inProximity())
            return; // out of proximity: nothing to send, and `applied` already says so
        if (e.applied && e.appliedCursor == cursor)
            return; // this tool is already showing it -- the dedup that IS safe (per tool, and
                    // only while the compositor is still holding what we sent)
        e.applied = true;
        e.appliedCursor = cursor;
        if (cursor == TabletCursor::Hidden || cursorShape == nullptr) {
            // A null surface HIDES the cursor -- which is exactly right over a brush (the reticle
            // ring is the cursor), and the honest fallback when we cannot name a shape at all: no
            // cursor beats the wrong cursor.
            zwp_tablet_tool_v2_set_cursor(e.proxy, e.proximitySerial, nullptr, 0, 0);
            return;
        }
        if (e.shapeDevice == nullptr)
            e.shapeDevice = wp_cursor_shape_manager_v1_get_tablet_tool_v2(cursorShape, e.proxy);
        if (e.shapeDevice == nullptr) {
            e.applied = false; // nothing went out: never remember a send that did not happen
            return;
        }
        wp_cursor_shape_device_v1_set_shape(e.shapeDevice, e.proximitySerial, shapeEnum(cursor));
    }

    static std::uint32_t shapeEnum(TabletCursor c) {
        switch (c) {
        case TabletCursor::Crosshair: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR;
        case TabletCursor::Text: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
        case TabletCursor::Move: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE;
        case TabletCursor::Pointer: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
        case TabletCursor::Wait: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT;
        case TabletCursor::Hidden: // handled by the caller; never reaches the protocol
        case TabletCursor::Default: break;
        }
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    }

    void removeTool(zwp_tablet_tool_v2* proxy) {
        const auto it = std::find_if(tools.begin(), tools.end(),
                                     [&](const auto& e) { return e->proxy == proxy; });
        if (it != tools.end()) {
            if ((*it)->shapeDevice != nullptr)
                wp_cursor_shape_device_v1_destroy((*it)->shapeDevice); // before the tool it names
            zwp_tablet_tool_v2_destroy(proxy); // protocol: the client must destroy on `removed`
            tools.erase(it);
        }
    }
    void removeTablet(zwp_tablet_v2* tablet) {
        const auto it = std::find(tablets.begin(), tablets.end(), tablet);
        if (it != tablets.end()) {
            zwp_tablet_v2_destroy(tablet);
            tablets.erase(it);
        }
    }
};

namespace {

// ---- zwp_tablet_tool_v2 listener: each event folds into the ToolEntry's WaylandTool ----
using ToolEntry = TabletWayland::Impl::ToolEntry;

void toolType(void* data, zwp_tablet_tool_v2*, std::uint32_t t) {
    static_cast<ToolEntry*>(data)->tool.setType(t);
}
void toolHwSerial(void* data, zwp_tablet_tool_v2*, std::uint32_t hi, std::uint32_t lo) {
    static_cast<ToolEntry*>(data)->tool.setSerial(hi, lo);
}
void toolHwWacom(void*, zwp_tablet_tool_v2*, std::uint32_t, std::uint32_t) {}
void toolCapability(void* data, zwp_tablet_tool_v2*, std::uint32_t cap) {
    static_cast<ToolEntry*>(data)->tool.addCapability(cap);
}
void toolDone(void*, zwp_tablet_tool_v2*) {}
void toolRemoved(void* data, zwp_tablet_tool_v2* proxy) {
    auto* entry = static_cast<ToolEntry*>(data);
    entry->owner->removeTool(proxy); // NB: frees `entry` -- touch nothing after this
}
void toolProximityIn(void* data, zwp_tablet_tool_v2*, std::uint32_t serial, zwp_tablet_v2*,
                     wl_surface* surface) {
    auto* entry = static_cast<ToolEntry*>(data);
    // set_cursor is only valid against the serial of a proximity_in, and the compositor forgets the
    // cursor we set the moment the tool leaves -- so keep the serial and re-apply on every entry.
    entry->proximitySerial = serial;
    // The compositor forgot our cursor when the tool left, so this entry starts from "nothing
    // applied" -- otherwise the re-apply below would dedup against a value the compositor no
    // longer holds and the pen would arrive wearing the compositor's default.
    entry->applied = false;
    // EVERY surface, and no list to be on. A wl_surface named in an event delivered to US is one of
    // ours by construction -- the compositor routes a tool over another client's surface to that
    // client, and it never reaches this callback at all. Filtering against a list of surfaces we had
    // thought to register was therefore not a safety check, it was a way to DROP our own windows: the
    // canvas is an FLTK sub-window with its own surface, so registering it registered ONLY it, and the
    // pen over the toolbar, the menu bar or a menu popup produced nothing whatsoever. (The Vulkan
    // subsurface has an empty input region, so the pen falls through it to the canvas -- §4 finding 2.)
    //
    // The tool REMEMBERS which surface it entered, because `pos` is SURFACE-LOCAL and every one of
    // our windows has a different origin. Resolving that id back to an Fl_Window is the wiring's job.
    entry->tool.proximityIn(reinterpret_cast<std::uint64_t>(surface));
    entry->owner->applyCursor(*entry); // ours to draw now; the compositor has stopped
}
void toolProximityOut(void* data, zwp_tablet_tool_v2*) {
    auto* entry = static_cast<ToolEntry*>(data);
    entry->tool.proximityOut();
    entry->applied = false; // the protocol drops the tool's cursor on leave -- so do we
}
void toolDown(void* data, zwp_tablet_tool_v2*, std::uint32_t) {
    static_cast<ToolEntry*>(data)->tool.down();
}
void toolUp(void* data, zwp_tablet_tool_v2*) { static_cast<ToolEntry*>(data)->tool.up(); }
void toolMotion(void* data, zwp_tablet_tool_v2*, wl_fixed_t x, wl_fixed_t y) {
    static_cast<ToolEntry*>(data)->tool.motion(wl_fixed_to_double(x), wl_fixed_to_double(y));
}
void toolPressure(void* data, zwp_tablet_tool_v2*, std::uint32_t p) {
    static_cast<ToolEntry*>(data)->tool.pressure(p);
}
void toolDistance(void*, zwp_tablet_tool_v2*, std::uint32_t) {} // unmodeled (no TabletSample field)
void toolTilt(void* data, zwp_tablet_tool_v2*, wl_fixed_t x, wl_fixed_t y) {
    static_cast<ToolEntry*>(data)->tool.tilt(wl_fixed_to_double(x), wl_fixed_to_double(y));
}
void toolRotation(void* data, zwp_tablet_tool_v2*, wl_fixed_t deg) {
    static_cast<ToolEntry*>(data)->tool.rotation(wl_fixed_to_double(deg));
}
void toolSlider(void* data, zwp_tablet_tool_v2*, std::int32_t position) {
    static_cast<ToolEntry*>(data)->tool.slider(position);
}
void toolWheel(void*, zwp_tablet_tool_v2*, wl_fixed_t, std::int32_t) {} // relative delta; unmodeled
void toolButton(void* data, zwp_tablet_tool_v2*, std::uint32_t, std::uint32_t button,
                std::uint32_t state) {
    static_cast<ToolEntry*>(data)->tool.button(button,
                                               state == ZWP_TABLET_TOOL_V2_BUTTON_STATE_PRESSED);
}
void toolFrame(void* data, zwp_tablet_tool_v2*, std::uint32_t) {
    auto* entry = static_cast<ToolEntry*>(data);
    // OUR clock at ingest, never the frame's `time` field (§5); the sink drives the canvas.
    entry->tool.frame(ingestClockUs(), entry->owner->sink);
}
const zwp_tablet_tool_v2_listener kToolListener = {
    toolType,     toolHwSerial,     toolHwWacom, toolCapability, toolDone,
    toolRemoved,  toolProximityIn,  toolProximityOut, toolDown,  toolUp,
    toolMotion,   toolPressure,     toolDistance, toolTilt,      toolRotation,
    toolSlider,   toolWheel,        toolButton,  toolFrame,
};

// ---- zwp_tablet_v2 listener: only the name, for the Settings->Tablet device row (§8) ----
void tabName(void* data, zwp_tablet_v2*, const char* name) {
    auto* impl = static_cast<TabletWayland::Impl*>(data);
    impl->tabletName = (name != nullptr) ? name : "";
    plog().info("wayland tablet: '{}'", impl->tabletName);
}
void tabId(void*, zwp_tablet_v2*, std::uint32_t, std::uint32_t) {}
void tabPath(void*, zwp_tablet_v2*, const char*) {}
void tabDone(void*, zwp_tablet_v2*) {}
void tabRemoved(void* data, zwp_tablet_v2* t) {
    static_cast<TabletWayland::Impl*>(data)->removeTablet(t);
}
const zwp_tablet_v2_listener kTabletListener = {tabName, tabId, tabPath, tabDone, tabRemoved};

// ---- zwp_tablet_seat_v2 listener: tablets, tools, pads announced on the seat ----
void seatTabletAdded(void* data, zwp_tablet_seat_v2*, zwp_tablet_v2* tablet) {
    auto* impl = static_cast<TabletWayland::Impl*>(data);
    impl->tablets.push_back(tablet);
    zwp_tablet_v2_add_listener(tablet, &kTabletListener, impl);
}
void seatToolAdded(void* data, zwp_tablet_seat_v2*, zwp_tablet_tool_v2* tool) {
    auto* impl = static_cast<TabletWayland::Impl*>(data);
    auto entry = std::make_unique<ToolEntry>();
    entry->proxy = tool;
    entry->owner = impl;
    zwp_tablet_tool_v2_add_listener(tool, &kToolListener, entry.get());
    impl->tools.push_back(std::move(entry));
}
void seatPadAdded(void*, zwp_tablet_seat_v2*, zwp_tablet_pad_v2* pad) {
    zwp_tablet_pad_v2_destroy(pad); // Mosaic ignores pad buttons / rings / strips
}
const zwp_tablet_seat_v2_listener kSeatListener = {seatTabletAdded, seatToolAdded, seatPadAdded};

// ---- registry: bind our own wl_seat + zwp_tablet_manager_v2 (FLTK exposes neither) ----
struct RegistryTargets {
    wl_seat* seat = nullptr;
    zwp_tablet_manager_v2* manager = nullptr;
    wp_cursor_shape_manager_v1* cursorShape = nullptr; // optional: absent -> we hide the cursor
};
void regGlobal(void* data, wl_registry* reg, std::uint32_t name, const char* iface, std::uint32_t) {
    auto* t = static_cast<RegistryTargets*>(data);
    if (std::strcmp(iface, wl_seat_interface.name) == 0)
        t->seat = static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, 1));
    else if (std::strcmp(iface, zwp_tablet_manager_v2_interface.name) == 0)
        t->manager = static_cast<zwp_tablet_manager_v2*>(
            wl_registry_bind(reg, name, &zwp_tablet_manager_v2_interface, 1));
    else if (std::strcmp(iface, wp_cursor_shape_manager_v1_interface.name) == 0)
        t->cursorShape = static_cast<wp_cursor_shape_manager_v1*>(
            wl_registry_bind(reg, name, &wp_cursor_shape_manager_v1_interface, 1));
}
void regGlobalRemove(void*, wl_registry*, std::uint32_t) {}
const wl_registry_listener kRegistryListener = {regGlobal, regGlobalRemove};

} // namespace

TabletWayland::TabletWayland() : m_impl(std::make_unique<Impl>()) {}

TabletWayland::~TabletWayland() {
    // Tear down in reverse dependency order. The proxies live on the default queue; by contract we
    // are destroyed while the display is still alive (the wiring TU owns it), so the client-side
    // destructors simply drop them.
    for (auto& entry : m_impl->tools)
        zwp_tablet_tool_v2_destroy(entry->proxy);
    for (zwp_tablet_v2* tablet : m_impl->tablets)
        zwp_tablet_v2_destroy(tablet);
    if (m_impl->tabletSeat != nullptr)
        zwp_tablet_seat_v2_destroy(m_impl->tabletSeat);
    if (m_impl->manager != nullptr)
        zwp_tablet_manager_v2_destroy(m_impl->manager);
    if (m_impl->seat != nullptr)
        wl_seat_destroy(m_impl->seat);
}

bool TabletWayland::init(wl_display* display, wl_surface* surface) {
    m_impl->display = display;
    m_impl->surface = surface;
    m_impl->available = false;
    if (display == nullptr || surface == nullptr)
        return false;

    // Bind seat + manager on a PRIVATE queue so the roundtrip cannot dispatch (and swallow) the
    // host's pending events, then hand the proxies back to the default queue -- from then on the
    // host's Fl::wait() dispatches tablet events. This is wayland_subsurface.cpp's pattern, proven
    // by the spike (docs/tablet.md §4 finding 1).
    wl_event_queue* queue = wl_display_create_queue(display);
    if (queue == nullptr)
        return false;
    wl_registry* registry = wl_display_get_registry(display);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(registry), queue);
    RegistryTargets targets;
    wl_registry_add_listener(registry, &kRegistryListener, &targets);
    wl_display_roundtrip_queue(display, queue); // process the global announcements
    for (void* p : {static_cast<void*>(targets.seat), static_cast<void*>(targets.manager),
                    static_cast<void*>(targets.cursorShape)})
        if (p != nullptr)
            wl_proxy_set_queue(static_cast<wl_proxy*>(p), nullptr);
    wl_registry_destroy(registry);
    wl_event_queue_destroy(queue);

    m_impl->seat = targets.seat;
    m_impl->manager = targets.manager;
    m_impl->cursorShape = targets.cursorShape; // may be null: we then hide the tool cursor instead
    if (m_impl->seat == nullptr || m_impl->manager == nullptr)
        return false; // no tablet manager: caller falls back to X11 / synthesized samples (§4)

    m_impl->tabletSeat = zwp_tablet_manager_v2_get_tablet_seat(m_impl->manager, m_impl->seat);
    zwp_tablet_seat_v2_add_listener(m_impl->tabletSeat, &kSeatListener, m_impl.get());
    wl_display_flush(display);
    m_impl->available = true;
    return true;
}

void TabletWayland::setCursor(TabletCursor cursor) {
    if (!m_impl->available)
        return;
    // NO app-wide value dedup here. The wanted value and what each TOOL is actually showing are
    // different facts (ToolEntry::applied): a change made while the pen is out of proximity sends
    // nothing, and a later request for the same value would then be dropped by a value dedup and
    // never reach the compositor at all. applyCursor holds the per-tool dedup instead, which is
    // the one that cannot lie.
    m_impl->cursor = cursor;
    for (const auto& e : m_impl->tools)
        m_impl->applyCursor(*e); // only the ones actually in proximity will send anything
    if (m_impl->display != nullptr)
        wl_display_flush(m_impl->display); // a cursor change nobody flushes is a cursor that lags
}

void TabletWayland::setSink(TabletStrokeSink* sink) noexcept { m_impl->sink = sink; }

bool TabletWayland::available() const noexcept { return m_impl->available; }

SampleRing& TabletWayland::ring() noexcept { return m_impl->ring; }

std::string_view TabletWayland::tabletName() const noexcept { return m_impl->tabletName; }

} // namespace mosaic::platform
