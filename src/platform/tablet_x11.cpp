#include "platform/tablet_x11.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>

namespace mosaic::platform {

namespace {

[[nodiscard]] std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) noexcept {
    return haystack.find(needle) != std::string_view::npos;
}

[[nodiscard]] double clamp01(double v) noexcept { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

// XIMaskIsSet with the bounds check the macro does not do: a hand-built (or hostile) event may
// carry any mask_len, including 0 with a null mask.
[[nodiscard]] bool maskBit(const unsigned char* mask, int maskLen, int bit) noexcept {
    if (mask == nullptr || bit < 0 || bit >= maskLen * 8)
        return false;
    return (mask[bit >> 3] & (1 << (bit & 7))) != 0;
}

// [min,max] stretched two-sidedly onto [0,1]. Degenerate span -> `fallback` (1.0 for pressure per
// §3.2 -- never collapse the stroke; 0.0 for the airbrush wheel, whose rest position is 0).
[[nodiscard]] double remap01(const Xi2Axis& a, double fallback) noexcept {
    const double span = a.max - a.min;
    if (!(span > 0.0))
        return fallback;
    return clamp01((a.value - a.min) / span);
}

// Peak-magnitude scaling onto [-fullScale, +fullScale]: value / max(|min|, |max|). Chosen over a
// two-sided remap so hardware zero stays EXACTLY zero when the declared range is asymmetric (the
// Wacom driver declares tilt as -64..63; an upright pen must not read as leaning). Degenerate
// range -> 0.
[[nodiscard]] double scaleSigned(const Xi2Axis& a, double fullScale) noexcept {
    const double peak = std::max(std::abs(a.min), std::abs(a.max));
    if (!(peak > 0.0))
        return 0.0;
    return std::clamp(a.value / peak, -1.0, 1.0) * fullScale;
}

// A position valuator, mapped onto the span its declared range covers -- i.e. the screen. NOT
// clamped: the pen may sit outside the mapped area, and pinning it to the edge would slide a stroke
// along the border instead of letting the guard in xi2Position() reject the sample.
[[nodiscard]] double remapToSpan(const Xi2Axis& a, double span) noexcept {
    const double range = a.max - a.min;
    if (!(range > 0.0))
        return 0.0;
    return (a.value - a.min) / range * span;
}

// The pen's position in the event window's coordinates, taken from the DEVICE's valuators rather
// than the server's screen-mapped pointer (the whole point -- see the header).
//
// Returns nullopt when the valuator path cannot be trusted, and the caller keeps event_x/event_y:
// no screen given, no position axes, a degenerate declared range, or the device's range not in fact
// spanning the screen. That last one is what makes this safe to ship -- a tablet mapped to ONE
// output of several (a Coordinate Transformation Matrix) would otherwise paint hundreds of pixels
// from the pen, which is a far worse bug than the wobble being fixed here.
[[nodiscard]] std::optional<common::Vec2> xi2Position(Xi2Device& dev, const XIDeviceEvent& ev,
                                                     const Xi2Screen& screen) noexcept {
    if (!(screen.width > 0.0) || !(screen.height > 0.0))
        return std::nullopt;
    if (!dev.absX.present() || !dev.absY.present())
        return std::nullopt;
    if (!(dev.absX.max > dev.absX.min) || !(dev.absY.max > dev.absY.min))
        return std::nullopt;

    const double sx = remapToSpan(dev.absX, screen.width); // where the DEVICE says it is, on screen
    const double sy = remapToSpan(dev.absY, screen.height);
    const double delta = std::max(std::abs(sx - ev.root_x), std::abs(sy - ev.root_y));
    dev.worstMapDelta = std::max(dev.worstMapDelta, delta);

    // How far the pen travelled since the last event -- and therefore how far the server's pointer is
    // entitled to lag behind the valuators in THIS one. See kXi2MapTolerancePx: a mapping error
    // scales with position, this lag scales with speed, and a fixed bar cannot tell them apart.
    const double travel =
        dev.hasPrevPos
            ? std::hypot(sx - dev.prevSx, sy - dev.prevSy)
            : 0.0;
    dev.prevSx = sx;
    dev.prevSy = sy;
    dev.hasPrevPos = true;
    const double allowed = kXi2MapTolerancePx + travel;

    // Does the range really span the screen? Ask the server -- whose position is NOISY and LAGGING
    // but not WRONG -- and make the verdict STICKY per device. Deciding it per event let it flip
    // inside a single stroke: the position source changed sample to sample, which was worse than
    // either source alone.
    if (delta <= allowed) {
        if (dev.mapAgreements < kXi2MapTrustSamples && ++dev.mapAgreements >= kXi2MapTrustSamples)
            dev.mapTrusted = true;
    } else {
        if (dev.mapTrusted) { // an already-trusted device is being kicked out -- by WHAT?
            ++dev.untrustEvents;
            dev.worstUntrustDelta = std::max(dev.worstUntrustDelta, delta);
        }
        dev.mapAgreements = 0;
        dev.mapTrusted = false; // never spanned the screen, or was re-mapped under us
    }
    if (!dev.mapTrusted)
        return std::nullopt; // event_x/event_y, exactly as it shipped

    // The event window's origin, from the event itself. Exact: root_x and event_x are the SAME
    // position expressed twice, so the server's sub-pixel wobble cancels in their difference.
    return common::Vec2{sx - (ev.root_x - ev.event_x), sy - (ev.root_y - ev.event_y)};
}

} // namespace

std::optional<Xi2Device> xi2Classify(const XIDeviceInfo& info, const Xi2AxisLabels& labels) {
    // Masters and keyboards are never tablets; ordinary slave pointers are FLTK's (§3.1). A
    // floating slave still classifies -- it re-attaches without a hierarchy re-shuffle on some
    // setups, and carrying it is harmless.
    if (info.use != XISlavePointer && info.use != XIFloatingSlave)
        return std::nullopt;
    if (info.name == nullptr)
        return std::nullopt;

    Xi2Device dev;
    dev.deviceId = info.deviceid;
    dev.name = info.name;

    const int numClasses = info.classes != nullptr ? info.num_classes : 0;
    for (int c = 0; c < numClasses; ++c) {
        const XIAnyClassInfo* any = info.classes[c];
        if (any == nullptr || any->type != XIValuatorClass)
            continue;
        const auto* v = reinterpret_cast<const XIValuatorClassInfo*>(any);
        Xi2Axis axis;
        axis.number = v->number;
        axis.min = v->min;
        axis.max = v->max;
        axis.value = v->value; // seed the running value: the first event may omit this axis
        if (v->label == 0)
            continue; // unlabeled axis; 0 also means "label we could not intern" (header)
        if (v->label == static_cast<Atom>(labels.absPressure))
            dev.pressure = axis;
        else if (v->label == static_cast<Atom>(labels.absTiltX))
            dev.tiltX = axis;
        else if (v->label == static_cast<Atom>(labels.absTiltY))
            dev.tiltY = axis;
        else if (v->label == static_cast<Atom>(labels.absWheel))
            dev.wheel = axis;
        else if (v->label == static_cast<Atom>(labels.absX))
            dev.absX = axis;
        else if (v->label == static_cast<Atom>(labels.absY))
            dev.absY = axis;
    }

    // CONTAINS, not ends-with: XWayland appends a client ordinal ("xwayland-tablet eraser:13"),
    // and XWayland is the default session (header comment).
    const std::string lower = toLower(dev.name);
    const bool toolName = contains(lower, "stylus") || contains(lower, "pen") ||
                          contains(lower, "eraser") || contains(lower, "airbrush") ||
                          contains(lower, "cursor") || contains(lower, "puck");
    if (!dev.pressure.present() && !toolName)
        return std::nullopt; // a mouse or touchpad, not a tablet tool

    if (contains(lower, "eraser"))
        dev.tool = TabletSample::Tool::Eraser;
    else if (contains(lower, "airbrush"))
        dev.tool = TabletSample::Tool::Airbrush;
    else if (contains(lower, "cursor") || contains(lower, "puck"))
        dev.tool = TabletSample::Tool::Puck;
    else
        dev.tool = TabletSample::Tool::Pen;
    return dev;
}

TabletSample xi2ParseEvent(Xi2Device& dev, const XIDeviceEvent& ev, std::uint64_t timeUs,
                           const Xi2Screen& screen) {
    // Fold the changed valuators into the running state. The values array is packed by SET BIT
    // order, not by axis number, so an unrecognized axis still consumes its slot.
    const XIValuatorState& val = ev.valuators;
    int k = 0;
    for (int i = 0; i < val.mask_len * 8; ++i) {
        if (!maskBit(val.mask, val.mask_len, i))
            continue;
        const double v = val.values[k++];
        if (i == dev.pressure.number)
            dev.pressure.value = v;
        else if (i == dev.tiltX.number)
            dev.tiltX.value = v;
        else if (i == dev.tiltY.number)
            dev.tiltY.value = v;
        else if (i == dev.wheel.number)
            dev.wheel.value = v;
        else if (i == dev.absX.number)
            dev.absX.value = v;
        else if (i == dev.absY.number)
            dev.absY.value = v;
    }

    TabletSample s;
    // The DEVICE's own position, not the server's screen-mapped pointer -- that one carries a
    // sub-pixel wobble that IS the staircased X11 stroke (header). event_x/event_y remains the
    // fallback whenever the valuators cannot be trusted, which is what shipped before.
    s.pos = xi2Position(dev, ev, screen).value_or(common::Vec2{ev.event_x, ev.event_y});
    s.pressure = dev.pressure.present() ? remap01(dev.pressure, 1.0) : 1.0; // §3.2: 1.0, never 0
    s.xTilt = dev.tiltX.present() ? scaleSigned(dev.tiltX, kTiltFullScaleDegrees) : 0.0;
    s.yTilt = dev.tiltY.present() ? scaleSigned(dev.tiltY, kTiltFullScaleDegrees) : 0.0;

    // The Wacom driver routes both the airbrush finger wheel and the art pen's barrel rotation
    // through "Abs Wheel"; the tool type is what disambiguates (header comment).
    if (dev.wheel.present()) {
        if (dev.tool == TabletSample::Tool::Airbrush)
            s.tangentialPressure = remap01(dev.wheel, 0.0);
        else
            s.rotation = scaleSigned(dev.wheel, 180.0);
    }

    // The event's mask is the state BEFORE the event; apply this event's own transition so the
    // sample reflects the state it produced.
    std::uint32_t buttons = 0;
    for (int b = 1; b <= 32; ++b)
        if (maskBit(ev.buttons.mask, ev.buttons.mask_len, b))
            buttons |= 1u << (b - 1);
    if (ev.detail >= 1 && ev.detail <= 32) {
        if (ev.evtype == XI_ButtonPress)
            buttons |= 1u << (ev.detail - 1);
        else if (ev.evtype == XI_ButtonRelease)
            buttons &= ~(1u << (ev.detail - 1));
    }
    s.buttons = buttons;

    s.tool = dev.tool;
    s.toolSerial = 0;   // Wacom's serial-number property needs hardware to validate against; 0 = unavailable (§2)
    s.inProximity = true; // an XI2 tablet slave only reports while in proximity (header comment)
    s.timeUs = timeUs;
    s.surface = static_cast<std::uint64_t>(ev.event); // the window the event was selected on
    return s;
}

bool TabletX11::init(Display* display, Window window) {
    m_display = display;
    m_window = window;
    m_windows.clear();
    m_available = false;
    m_devices.clear();
    if (display == nullptr || window == 0)
        return false;
    m_windows.push_back(window); // the canvas window; watchWindow() adds the dialogs

    int event = 0;
    int error = 0;
    if (!XQueryExtension(display, "XInputExtension", &m_opcode, &event, &error))
        return false;
    int major = 2;
    int minor = 2;
    if (XIQueryVersion(display, &major, &minor) != Success)
        return false; // server speaks < 2.2: report unavailable, the canvas falls back (§3.2)

    // only_if_exists: an atom that was never interned means no driver ever labeled an axis with
    // it, so None (0) -- which the classifier matches against nothing -- is exactly right.
    m_labels.absPressure = XInternAtom(display, "Abs Pressure", True);
    m_labels.absTiltX = XInternAtom(display, "Abs Tilt X", True);
    m_labels.absTiltY = XInternAtom(display, "Abs Tilt Y", True);
    m_labels.absWheel = XInternAtom(display, "Abs Wheel", True);
    m_labels.absX = XInternAtom(display, "Abs X", True);
    m_labels.absY = XInternAtom(display, "Abs Y", True);

    // What a device's absolute range maps onto, so its position valuators can be read as pixels
    // (xi2ParseEvent). The whole X screen -- which on a multi-head setup is the whole desktop, not
    // one monitor; the per-event guard is what catches a device that does not in fact span it.
    const int scr = DefaultScreen(display);
    m_screen.width = static_cast<double>(DisplayWidth(display, scr));
    m_screen.height = static_cast<double>(DisplayHeight(display, scr));

    enumerate();
    m_available = true; // available even with zero devices: hotplug arrives via hierarchy events
    return true;
}

void TabletX11::enumerate() {
    m_devices.clear();
    if (m_display == nullptr)
        return;

    int n = 0;
    XIDeviceInfo* infos = XIQueryDevice(m_display, XIAllDevices, &n);
    if (infos != nullptr) {
        for (int i = 0; i < n; ++i)
            if (std::optional<Xi2Device> dev = xi2Classify(infos[i], m_labels))
                m_devices.push_back(std::move(*dev));
        XIFreeDeviceInfo(infos);
    }

    for (const Window w : m_windows)
        selectOn(w);
}

void TabletX11::selectOn(Window window) {
    if (m_display == nullptr || window == 0)
        return;
    // Select per SLAVE device: selecting on the master would double every sample against the core
    // stream FLTK keeps (§3.1). Hierarchy changes are selected regardless of the device count --
    // they are how a tablet plugged in later is noticed at all.
    unsigned char devBits[XIMaskLen(XI_LASTEVENT)] = {};
    XISetMask(devBits, XI_Motion);
    XISetMask(devBits, XI_ButtonPress);
    XISetMask(devBits, XI_ButtonRelease);
    unsigned char hierBits[XIMaskLen(XI_LASTEVENT)] = {};
    XISetMask(hierBits, XI_HierarchyChanged);

    std::vector<XIEventMask> masks;
    masks.reserve(m_devices.size() + 1);
    for (const Xi2Device& dev : m_devices)
        masks.push_back({dev.deviceId, static_cast<int>(sizeof(devBits)), devBits});
    masks.push_back({XIAllDevices, static_cast<int>(sizeof(hierBits)), hierBits});
    XISelectEvents(m_display, window, masks.data(), static_cast<int>(masks.size()));
}

void TabletX11::watchWindow(Window window) {
    if (!m_available || window == 0)
        return;
    if (std::find(m_windows.begin(), m_windows.end(), window) != m_windows.end())
        return;
    m_windows.push_back(window);
    selectOn(window); // the device list is already enumerated; just add this window to it
}

void TabletX11::unwatchWindow(Window window) {
    // Drop it from the list and do NOT call XISelectEvents to deselect: a watched window is
    // unwatched precisely when it is going away, and selecting on an already-destroyed Window is a
    // BadWindow. The server drops the selection with the window; all we owe it is never to name
    // that window again on the next enumerate().
    const auto it = std::find(m_windows.begin(), m_windows.end(), window);
    if (it != m_windows.end())
        m_windows.erase(it);
}

bool TabletX11::handleEvent(XEvent& ev) {
    if (!m_available || ev.type != GenericEvent || ev.xcookie.extension != m_opcode)
        return false;
    // The cookie is ours to fetch and free (§3): FLTK never reads GenericEvent data, and nothing
    // else in Mosaic selects XI2 events.
    if (!XGetEventData(m_display, &ev.xcookie))
        return false;
    bool ours = false;
    switch (ev.xcookie.evtype) {
    case XI_HierarchyChanged:
        enumerate(); // hotplug: devices (and their event selections) are rebuilt from scratch
        ours = true;
        break;
    case XI_Motion:
    case XI_ButtonPress:
    case XI_ButtonRelease: {
        const auto* de = static_cast<const XIDeviceEvent*>(ev.xcookie.data);
        for (Xi2Device& dev : m_devices) {
            if (dev.deviceId == de->deviceid) {
                m_ring.push(xi2ParseEvent(dev, *de, ingestClockUs(), m_screen));
                if (dev.absX.present() && dev.absY.present())
                    (dev.mapTrusted ? m_posValuator : m_posServer)++;
                ours = true;
                break;
            }
        }
        break;
    }
    default:
        break;
    }
    XFreeEventData(m_display, &ev.xcookie);
    return ours;
}

} // namespace mosaic::platform
