#include "platform/tablet_win32.hpp"

#include "common/log.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numbers>

namespace mosaic::platform {

namespace {

spdlog::logger& tabletLog() {
    static const auto logger = common::log::category("tablet");
    return *logger;
}

[[nodiscard]] double clamp01(double v) noexcept { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

[[nodiscard]] std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) noexcept {
    return haystack.find(needle) != std::string_view::npos;
}

// [min,max] stretched two-sidedly onto [0,1]. A degenerate span -> `fallback`: 1.0 for pressure
// (§3.2 -- never collapse the stroke), 0.0 for the airbrush wheel, whose rest position is 0. The
// same shape as the XI2 backend's remap01, deliberately: two backends normalizing pressure
// differently would make the same nib feel different on two platforms.
[[nodiscard]] double remap01(const WinTabAxis& axis, double raw, double fallback) noexcept {
    const double span = axis.max - axis.min;
    if (!axis.present || !(span > 0.0))
        return fallback;
    return clamp01((raw - axis.min) / span);
}

// Tenths of a degree is the long-standing convention -- Wacom declares 3600 units per circle, and
// other implementations of this ABI hardcode the /10 outright. Used only as the fallback for a
// driver that declares an angular axis with a resolution we cannot read.
inline constexpr double kConventionalDegPerUnit = 0.1;

inline constexpr double kDegToRad = std::numbers::pi / 180.0;
inline constexpr double kRadToDeg = 180.0 / std::numbers::pi;

// Wrap an angle in degrees into [-180, 180], the range TabletSample::rotation declares.
[[nodiscard]] double wrap180(double deg) noexcept {
    double r = std::fmod(deg, 360.0);
    if (r > 180.0)
        r -= 360.0;
    else if (r < -180.0)
        r += 360.0;
    return r;
}

// A FIX32 (16.16 fixed point) as a double.
[[nodiscard]] double fix32(wintab::FIX32 v) noexcept {
    return static_cast<double>(v) / 65536.0;
}

// A driver string, out of the process's ACTIVE CODE PAGE and into the UTF-8 the rest of Mosaic
// speaks. WTInfoA hands back an ANSI string (see LOGCONTEXTA's note on why the A entry points), and
// a device named in any non-Latin script arrives mangled if it is copied byte for byte.
[[nodiscard]] std::string ansiToUtf8(const char* ansi, std::size_t bytes) {
    if (ansi == nullptr || bytes == 0)
        return {};
    const int wide = MultiByteToWideChar(CP_ACP, 0, ansi, static_cast<int>(bytes), nullptr, 0);
    if (wide <= 0)
        return {};
    std::wstring w(static_cast<std::size_t>(wide), L'\0');
    MultiByteToWideChar(CP_ACP, 0, ansi, static_cast<int>(bytes), w.data(), wide);
    const int utf8 = WideCharToMultiByte(CP_UTF8, 0, w.data(), wide, nullptr, 0, nullptr, nullptr);
    if (utf8 <= 0)
        return {};
    std::string out(static_cast<std::size_t>(utf8), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wide, out.data(), utf8, nullptr, nullptr);
    // WTInfoA's byte count INCLUDES the terminator; a std::string must not.
    while (!out.empty() && out.back() == '\0')
        out.pop_back();
    return out;
}

// Is this HWND one of OURS? Asked of the process, not of FLTK: this file is FLTK-free by design
// (fl_win32_find lives on the other side of the boundary), and "does it belong to this process" is
// both the honest question and the one that cannot go stale.
[[nodiscard]] bool isOurWindow(HWND hwnd) noexcept {
    if (hwnd == nullptr)
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

// The message-only window a WinTab context posts its packets to.
//
// It exists because a context belongs to a WINDOW, and hanging ours off the main window would tie
// the packet stream to that window's activation -- while Settings -> Tablet's test area has to read
// the pen with a DIALOG in front (§8). A window that is never activated, never resized and never
// repainted has no such state to get wrong. This is the shape other implementations of this problem
// ship, and it works because the context is a SYSTEM context (WTI_DEFSYSCTX): a system context
// tracks the pen across the desktop rather than following one window's focus.
//
// The messages still reach us: FLTK's Windows event loop calls PeekMessageW with a null HWND, which
// retrieves messages for EVERY window of the calling thread, and hands each to the system handlers
// before dispatching it (Fl_win32.cxx). DefWindowProcW is a sufficient WndProc for the dispatch
// that follows -- nothing is expected to answer a WT_* message.
[[nodiscard]] HWND createPacketWindow() {
    static const wchar_t* kClassName = L"MosaicWinTabPacketSink";
    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = instance;
    cls.lpszClassName = kClassName;
    // A duplicate registration is the expected outcome of a second init(); anything else is fatal
    // to the WinTab path and falls through to Windows Ink.
    if (RegisterClassExW(&cls) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return nullptr;

    return CreateWindowExW(0, kClassName, L"Mosaic tablet packets", 0, 0, 0, 0, 0, HWND_MESSAGE,
                           nullptr, instance, nullptr);
}

// The virtual desktop, in physical pixels -- what a tablet whose driver mapping we distrust is
// mapped onto instead (TabletWin32Mapping::VirtualScreen).
[[nodiscard]] WinTabMap virtualScreenTarget(WinTabMap map) noexcept {
    map.outX = static_cast<double>(GetSystemMetrics(SM_XVIRTUALSCREEN));
    map.outY = static_cast<double>(GetSystemMetrics(SM_YVIRTUALSCREEN));
    map.outW = static_cast<double>(GetSystemMetrics(SM_CXVIRTUALSCREEN));
    map.outH = static_cast<double>(GetSystemMetrics(SM_CYVIRTUALSCREEN));
    return map;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// wintab -- the runtime load
// ---------------------------------------------------------------------------------------------

namespace wintab {
namespace {

// GetProcAddress hands back a FARPROC, which on MinGW is `INT_PTR(WINAPI*)()`. Casting that
// straight to the real signature trips -Wcast-function-type (-Wextra), and routing it through void*
// trips -Wpedantic's "no casting between object and function pointers". The one form both accept is
// a hop through the generic `void(*)()` function type, which GCC exempts by name -- so it is the
// hop this project takes, in exactly one place.
template <class Fn>
void resolve(HMODULE dll, const char* symbol, Fn& out) noexcept {
    using Generic = void(WINAPI*)();
    const Generic generic = reinterpret_cast<Generic>(GetProcAddress(dll, symbol));
    out = reinterpret_cast<Fn>(generic);
}

} // namespace

bool load(Api& out) {
    out = Api{};
    // No path, no extension games: the driver installs Wintab32.dll into the system directory, and
    // the default search order finds it there. Its ABSENCE is the normal case on a machine with no
    // tablet driver, and it is not an error -- it is the signal to use Windows Ink.
    out.dll = LoadLibraryW(L"Wintab32.dll");
    if (out.dll == nullptr)
        return false;

    resolve(out.dll, "WTInfoA", out.info);
    resolve(out.dll, "WTOpenA", out.open);
    resolve(out.dll, "WTClose", out.close);
    resolve(out.dll, "WTPacketsGet", out.packetsGet);
    resolve(out.dll, "WTOverlap", out.overlap);
    resolve(out.dll, "WTQueueSizeSet", out.queueSizeSet);

    if (!out.loaded()) {
        unload(out);
        return false;
    }
    return true;
}

void unload(Api& api) {
    if (api.dll != nullptr)
        FreeLibrary(api.dll);
    api = Api{};
}

} // namespace wintab

// ---------------------------------------------------------------------------------------------
// user32 -- the Windows Ink + pen-feedback entry points, resolved at RUNTIME
// ---------------------------------------------------------------------------------------------
//
// These five all exist on every Windows this build targets (the floor is 10 1809, they arrived in 8),
// so linking them statically would be correct on Windows and is what the first cut did. It is still
// the wrong call, for a reason that showed up the moment the .exe was run anywhere other than
// Windows: **a missing import is not an error the loader reports, it is a process that dies.** The
// first GUI run under Wine aborted with "Call from … to unimplemented function
// USER32.dll.SetWindowFeedbackSetting" -- and that function only switches off some pen feedback
// ANIMATIONS. Killing the application over a cosmetic tweak is indefensible whatever the platform.
//
// Resolving them instead means each one degrades to exactly what its absence should mean: no
// feedback suppression, or no Windows Ink backend (WinTab or plain mouse still serve). It also
// matches how this very file already treats Wintab32.dll one namespace up, so the TU now has one
// policy rather than two. Windows-on-Windows behaviour is unchanged -- the symbols are there and
// resolve on the first call.
//
// user32.dll is unconditionally already mapped into any GUI process, so GetModuleHandleW is right
// here rather than LoadLibraryW: no reference to own, nothing to free, and no chance of picking up a
// second copy from a search path.
namespace user32api {
namespace {

struct Api {
    bool tried = false;
    BOOL(WINAPI* setWindowFeedbackSetting)(HWND, FEEDBACK_TYPE, DWORD, UINT32, const VOID*) = nullptr;
    BOOL(WINAPI* getPointerType)(UINT32, POINTER_INPUT_TYPE*) = nullptr;
    BOOL(WINAPI* getPointerPenInfo)(UINT32, POINTER_PEN_INFO*) = nullptr;
    BOOL(WINAPI* getPointerPenInfoHistory)(UINT32, UINT32*, POINTER_PEN_INFO*) = nullptr;
    BOOL(WINAPI* getPointerDeviceRects)(HANDLE, RECT*, RECT*) = nullptr;
};

// Not thread-safe by construction and it does not need to be: every caller below runs on the UI
// thread, inside the message pump (the same single-threaded contract the rest of this file has).
Api& api() {
    static Api a;
    if (!a.tried) {
        a.tried = true;
        if (HMODULE u32 = GetModuleHandleW(L"user32.dll"); u32 != nullptr) {
            wintab::resolve(u32, "SetWindowFeedbackSetting", a.setWindowFeedbackSetting);
            wintab::resolve(u32, "GetPointerType", a.getPointerType);
            wintab::resolve(u32, "GetPointerPenInfo", a.getPointerPenInfo);
            wintab::resolve(u32, "GetPointerPenInfoHistory", a.getPointerPenInfoHistory);
            wintab::resolve(u32, "GetPointerDeviceRects", a.getPointerDeviceRects);
        }
    }
    return a;
}

// True when the pointer API is usable at all. GetPointerPenInfoHistory is deliberately NOT required:
// it is a fidelity win, not a capability, and handleInkMessage already falls back to the single
// newest sample when the history call fails.
bool inkAvailable() {
    const Api& a = api();
    return a.getPointerType != nullptr && a.getPointerPenInfo != nullptr;
}

} // namespace
} // namespace user32api

namespace {

// Switch off Windows' pen gesture VISUALS on one of our windows: the press-and-hold ring, the tap
// and right-tap ripples, the barrel-button splash. In a paint program every one of those draws
// itself over the artwork under the nib.
//
// ⚠ This kills the FEEDBACK, not the GESTURE. Disabling press-and-hold itself requires answering
// WM_TABLET_QUERYSYSTEMGESTURESTATUS with TABLET_DISABLE_PRESSANDHOLD -- and that message is SENT,
// not posted, so it never reaches a PeekMessage-based system handler at all, let alone one that can
// only swallow a message rather than return a value for it. Doing that would take a WndProc of our
// own; it is deliberately not attempted here (docs/tablet.md §5a).
void disablePenFeedback(HWND hwnd) {
    if (hwnd == nullptr)
        return;
    static constexpr FEEDBACK_TYPE kOff[] = {
        FEEDBACK_PEN_BARRELVISUALIZATION, FEEDBACK_PEN_TAP, FEEDBACK_PEN_DOUBLETAP,
        FEEDBACK_PEN_PRESSANDHOLD, FEEDBACK_PEN_RIGHTTAP, FEEDBACK_TOUCH_CONTACTVISUALIZATION};
    const auto setFeedback = user32api::api().setWindowFeedbackSetting;
    if (setFeedback == nullptr)
        return; // no feedback suppression; the pen still draws (see the user32api note above)
    const BOOL off = FALSE;
    for (const FEEDBACK_TYPE f : kOff)
        setFeedback(hwnd, f, 0, static_cast<UINT32>(sizeof(off)), &off);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// WinTab -- the pure normalization half
// ---------------------------------------------------------------------------------------------

double winTabDegreesPerUnit(const wintab::AXIS& axis) noexcept {
    // A zero resolution is how a driver says "this axis does not exist"; a zero RANGE says the same
    // thing about a driver that filled the resolution in anyway.
    if (axis.axResolution == 0 || axis.axMax == axis.axMin)
        return 0.0;
    if (axis.axUnits != wintab::kTuCircle)
        return kConventionalDegPerUnit; // declared angular by position, not by unit tag: trust
                                        // convention
    const double unitsPerCircle = fix32(axis.axResolution);
    if (!(unitsPerCircle > 0.0))
        return kConventionalDegPerUnit;
    return 360.0 / unitsPerCircle;
}

TabletSample::Tool winTabTool(std::string_view cursorName, unsigned cursorType) noexcept {
    // NAME FIRST, lowercased and by CONTAINS -- the identical classifier the XI2 backend runs over
    // device names, and identical on purpose: "the eraser end" must mean the same thing on both
    // platforms or a preset that keys off the tool behaves differently on each. Wacom's cursor
    // names are "Pressure Stylus" / "Eraser" / "Airbrush" / "Puck"; other vendors follow the
    // convention.
    const std::string lower = toLower(cursorName);
    if (contains(lower, "eraser"))
        return TabletSample::Tool::Eraser;
    if (contains(lower, "airbrush"))
        return TabletSample::Tool::Airbrush;
    if (contains(lower, "puck") || contains(lower, "cursor") || contains(lower, "lens") ||
        contains(lower, "mouse"))
        return TabletSample::Tool::Puck;
    if (contains(lower, "stylus") || contains(lower, "pen") || contains(lower, "pencil") ||
        contains(lower, "brush"))
        return TabletSample::Tool::Pen;

    // CSR_TYPE is a vendor bitfield, and only its tool-class nibbles are portable enough to read.
    // 0x0F06 is the mask other implementations of this ABI use over it.
    switch (cursorType & 0x0F06u) {
    case 0x0902u: return TabletSample::Tool::Airbrush;
    case 0x0006u: return TabletSample::Tool::Puck;
    case 0x0004u: return TabletSample::Tool::Mouse;
    default: break;
    }
    return TabletSample::Tool::Pen; // a stylus is the safe assumption for a drawing tool
}

common::Vec2 winTabTilt(double azimuthDeg, double altitudeDeg) noexcept {
    // Keep tan() off zero. A pen lying flat on the pad has altitude 0, which is a legitimate
    // reading and a division by zero: the epsilon turns it into the +/-90 degrees it means instead
    // of a NaN that would poison every dab downstream.
    constexpr double kMinAltitudeDeg = 0.01;
    const double altitude = std::max(std::abs(altitudeDeg), kMinAltitudeDeg) * kDegToRad;
    const double azimuth = azimuthDeg * kDegToRad;
    const double tanAltitude = std::tan(altitude);
    if (!(std::abs(tanAltitude) > 0.0))
        return {}; // unreachable given the clamp above, but a NaN here is a corrupted stroke
    const double degX = std::atan(std::sin(azimuth) / tanAltitude) * kRadToDeg;
    const double degY = std::atan(std::cos(azimuth) / tanAltitude) * kRadToDeg;
    return {degX, -degY};
}

double winTabRotation(double twistDeg) noexcept {
    // The twist axis counts the opposite way round from TabletSample::rotation, so 360 - twist.
    return wrap180(360.0 - twistDeg);
}

common::Vec2 winTabScreenPos(const WinTabMap& map, LONG pkX, LONG pkY) noexcept {
    if (!map.usable())
        return {};
    const double fx = (static_cast<double>(pkX) - map.inOrgX) / map.inExtX;
    const double fy = (static_cast<double>(pkY) - map.inOrgY) / map.inExtY;
    // NOT clamped to the target rect: the pen can legitimately sit outside the mapped area (a
    // tablet is usually a little larger than what it maps to), and pinning it to the edge would
    // slide a stroke along the border instead of letting it leave.
    return {map.outX + fx * map.outW, map.outY + (map.flipY ? (1.0 - fy) : fy) * map.outH};
}

TabletSample winTabParsePacket(const WinTabDevice& dev, const WinTabCursor& cursor,
                               const wintab::Packet& pkt, const WinTabMap& map,
                               std::uint64_t timeUs) noexcept {
    TabletSample s;
    s.pos = winTabScreenPos(map, pkt.pkX, pkt.pkY);
    s.pressure = remap01(dev.pressure, static_cast<double>(pkt.pkNormalPressure), 1.0);
    s.tangentialPressure = remap01(dev.tangential, static_cast<double>(pkt.pkTangentPressure), 0.0);

    if (dev.hasTilt()) {
        const double azimuth =
            static_cast<double>(pkt.pkOrientation.orAzimuth) * dev.azimuthDegPerUnit;
        const double altitude =
            static_cast<double>(pkt.pkOrientation.orAltitude) * dev.altitudeDegPerUnit;
        const common::Vec2 tilt = winTabTilt(azimuth, altitude);
        s.xTilt = tilt.x;
        s.yTilt = tilt.y;
    }
    if (dev.hasTwist())
        s.rotation =
            winTabRotation(static_cast<double>(pkt.pkOrientation.orTwist) * dev.twistDegPerUnit);

    // Absolute button mode: pkButtons IS the set of buttons currently down, bit 0 = the tip. That
    // makes "bit 0 == button 1 == tip" hold on all three platforms, which is what lets a brush
    // preset name a barrel button once.
    s.buttons = static_cast<std::uint32_t>(pkt.pkButtons);

    // TPS_INVERT is the driver telling us, per packet, that the stylus is upside down. It beats the
    // cursor's name: a stylus whose eraser end shares one cursor entry with its nib would otherwise
    // erase nothing.
    s.tool = (pkt.pkStatus & wintab::kTpsInvert) != 0 ? TabletSample::Tool::Eraser : cursor.tool;
    s.toolSerial = 0; // CSR_PHYSID needs hardware to validate against; 0 = unavailable (§2)
    s.inProximity = (pkt.pkStatus & wintab::kTpsProximity) != 0;
    s.timeUs = timeUs;
    s.surface = 0; // filled by TabletWin32 once the target window is known (header)
    return s;
}

// ---------------------------------------------------------------------------------------------
// Windows Ink -- the pure normalization half
// ---------------------------------------------------------------------------------------------

TabletSample inkParsePen(const POINTER_PEN_INFO& pen, const InkDeviceRects& rects,
                         std::uint64_t timeUs) noexcept {
    const POINTER_INFO& info = pen.pointerInfo;

    TabletSample s;
    if (rects.valid && rects.devW != 0.0 && rects.devH != 0.0) {
        // The digitizer's own 0.01 mm grid, mapped onto the display rectangle it covers. This is
        // where the sub-pixel position comes from: ptPixelLocation below is rounded to whole
        // pixels, and a stroke walked along whole pixels is the polygon docs/brushes.md §6.2 exists
        // to stop being.
        s.pos = {rects.dispX + (static_cast<double>(info.ptHimetricLocationRaw.x) - rects.devX) *
                                   rects.dispW / rects.devW,
                 rects.dispY + (static_cast<double>(info.ptHimetricLocationRaw.y) - rects.devY) *
                                   rects.dispH / rects.devH};
    } else {
        s.pos = {static_cast<double>(info.ptPixelLocation.x),
                 static_cast<double>(info.ptPixelLocation.y)};
    }

    // A pen that does not report pressure sends 0 with the mask bit clear. 1.0, never 0 (§3.2).
    const bool hasPressure = (pen.penMask & PEN_MASK_PRESSURE) != 0 && pen.pressure > 0;
    s.pressure = hasPressure ? clamp01(static_cast<double>(pen.pressure) / kInkPressureMax) : 1.0;

    // tiltX/tiltY are documented as DEGREES in [-90, 90]: pass them through unscaled, per
    // tablet.hpp's rule for a backend that already speaks degrees.
    if ((pen.penMask & PEN_MASK_TILT_X) != 0)
        s.xTilt = std::clamp(static_cast<double>(pen.tiltX), -90.0, 90.0);
    if ((pen.penMask & PEN_MASK_TILT_Y) != 0)
        s.yTilt = std::clamp(static_cast<double>(pen.tiltY), -90.0, 90.0);
    if ((pen.penMask & PEN_MASK_ROTATION) != 0)
        s.rotation = wrap180(static_cast<double>(pen.rotation));
    s.tangentialPressure = 0.0; // no airbrush wheel in this API (header)

    const bool inverted = (pen.penFlags & (PEN_FLAG_INVERTED | PEN_FLAG_ERASER)) != 0;
    s.tool = inverted ? TabletSample::Tool::Eraser : TabletSample::Tool::Pen;
    s.toolSerial = 0; // sourceDevice names the DIGITIZER, not the pen (header)

    s.inProximity = (info.pointerFlags & POINTER_FLAG_INRANGE) != 0;

    std::uint32_t buttons = 0;
    if ((info.pointerFlags & POINTER_FLAG_INCONTACT) != 0)
        buttons |= 1u; // the tip -- bit 0, exactly the X11/Wayland/macOS tip button
    if ((pen.penFlags & PEN_FLAG_BARREL) != 0 ||
        (info.pointerFlags & POINTER_FLAG_SECONDBUTTON) != 0)
        buttons |= 1u << 1;
    if ((info.pointerFlags & POINTER_FLAG_THIRDBUTTON) != 0)
        buttons |= 1u << 2;
    s.buttons = buttons;

    s.timeUs = timeUs; // OUR clock; info.dwTime is the system's tick count (§5)
    s.surface = 0;     // filled by TabletWin32 once the target window is known (header)
    return s;
}

// ---------------------------------------------------------------------------------------------
// TabletWin32 -- the live backend
// ---------------------------------------------------------------------------------------------

namespace {

// How many packets to lift out of the driver's queue at once, and how deep to ask that queue to be.
// A 200 Hz pen between two 60 Hz drains leaves ~4 packets, so this is three orders of magnitude of
// headroom -- sized for a stall, not for the steady state, exactly like SampleRing's capacity.
inline constexpr int kPacketBatch = 128;

// A coalesced WM_POINTERUPDATE stands for this many device samples at most before we stop believing
// the count and take only the newest. GetPointerPenInfoHistory refuses a buffer smaller than the
// reported historyCount, so an absurd count has to be rejected rather than truncated.
inline constexpr UINT32 kInkHistoryMax = 512;

} // namespace

TabletWin32::~TabletWin32() { stopWinTab(); }

std::string_view TabletWin32::name() const noexcept {
    switch (m_api) {
    case TabletWin32Api::WinTab: return "win32/wintab";
    case TabletWin32Api::PointerInk: return "win32/windows-ink";
    case TabletWin32Api::Auto: break;
    }
    return "win32/none";
}

bool TabletWin32::init(HWND window, const TabletWin32Config& cfg) {
    // Idempotent by construction: a second init() must not leak the first one's context or its
    // message window. The wiring already guards against calling it twice, but a backend whose
    // re-init leaks a driver context is a bug waiting for the second caller.
    stopWinTab();
    m_available = false;
    m_api = TabletWin32Api::Auto;
    m_packets.resize(static_cast<std::size_t>(kPacketBatch));

    // Windows draws a ring, a ripple and a splash for the pen's own gestures, on top of whatever is
    // under the nib. Switch them off before anything else: it is worth doing even when no tablet
    // backend comes up at all, because a Surface pen driving the plain mouse path still triggers
    // them.
    if (window != nullptr &&
        std::find(m_feedbackWindows.begin(), m_feedbackWindows.end(), window) ==
            m_feedbackWindows.end()) {
        disablePenFeedback(window);
        m_feedbackWindows.push_back(window);
    }

    if (cfg.api != TabletWin32Api::PointerInk && startWinTab(cfg)) {
        m_api = TabletWin32Api::WinTab;
        m_available = true;
        return true;
    }
    if (cfg.api == TabletWin32Api::WinTab) {
        // The user asked for WinTab by name and it did not come up. Fall through to Ink anyway and
        // SAY SO: silently painting with a dead tablet is the one outcome worth ruling out (§5).
        tabletLog().warn("WinTab was requested but did not come up; using Windows Ink instead");
    }

    // Windows Ink needs no device to be present and nothing to be opened: WM_POINTER* messages for
    // a pen arrive from the OS if there is a pen, and do not if there is not. So this path is
    // always "available" in the same sense a live XI2 backend with zero tablets is -- it is
    // listening, and with nothing to listen to the ring stays empty and the canvas synthesizes
    // pressure-1 samples from the mouse (§3.2). We do NOT call EnableMouseInPointer: that converts
    // the MOUSE into pointer messages too, which would take FLTK's WM_MOUSEMOVE stream away and
    // break every non-pen interaction in the program.
    //
    // The one thing that can rule this path out is the pointer API not being THERE (see the
    // user32api note): report unavailable rather than listening for messages nothing can decode, so
    // that Settings -> Tablet says "no backend" honestly instead of showing a live-looking backend
    // that can never produce a sample.
    // m_api is deliberately left as it is rather than gaining a `None` enumerator: name() already
    // answers "win32/none" for anything that is not one of the two real backends, so the absence is
    // already expressible, and widening a config-facing enum for an unreachable-on-Windows case
    // would put a third value in front of every caller that switches on it.
    if (!user32api::inkAvailable()) {
        tabletLog().warn("no tablet backend: WinTab absent and the pointer API did not resolve");
        m_available = false;
        return false;
    }
    m_api = TabletWin32Api::PointerInk;
    m_available = true;
    return true;
}

// No HWND parameter, deliberately: the context hangs off a message-only window of its own
// (createPacketWindow), never off one of the app's, so that the packet stream cannot be tied to any
// window's activation state.
bool TabletWin32::startWinTab(const TabletWin32Config& cfg) {
    if (!wintab::load(m_wt))
        return false; // no tablet driver on this machine: the expected, non-error case

    // WTInfoA(0, 0, nullptr) is the ABI's own "is there a tablet" question: it answers the size of
    // the whole interface description, and zero when the driver is installed but no tablet is
    // attached. Checking the device COUNT as well catches a driver that answers the first question
    // out of politeness.
    if (m_wt.info(0, 0, nullptr) == 0) {
        stopWinTab();
        return false;
    }
    UINT deviceCount = 0;
    if (m_wt.info(wintab::kWtiInterface, wintab::kIfcNDevices, &deviceCount) == 0 ||
        deviceCount == 0) {
        stopWinTab();
        return false;
    }

    wintab::LOGCONTEXTA lc{};
    if (m_wt.info(wintab::kWtiDefSysCtx, 0, &lc) == 0) {
        stopWinTab();
        return false;
    }
    // A context with no input extent has no position to report, and winTabScreenPos would answer
    // {0,0} for every packet -- i.e. paint at the desktop origin. Refuse the path instead and let
    // Windows Ink have it; a driver in that state is not going to give us pressure either.
    if (lc.lcInExtX == 0 || lc.lcInExtY == 0) {
        tabletLog().info("WinTab reports a zero-extent context; falling back");
        stopWinTab();
        return false;
    }

    // The driver's own screen mapping, captured BEFORE the output extents are overwritten: it is
    // what the user configured in the tablet control panel, and therefore the default target.
    WinTabMap map;
    map.inOrgX = static_cast<double>(lc.lcInOrgX);
    map.inExtX = static_cast<double>(lc.lcInExtX);
    map.inOrgY = static_cast<double>(lc.lcInOrgY);
    map.inExtY = static_cast<double>(lc.lcInExtY);
    map.outX = static_cast<double>(lc.lcSysOrgX);
    map.outY = static_cast<double>(lc.lcSysOrgY);
    map.outW = static_cast<double>(lc.lcSysExtX);
    map.outH = static_cast<double>(lc.lcSysExtY);
    switch (cfg.mapping) {
    case TabletWin32Mapping::Driver: break;
    case TabletWin32Mapping::VirtualScreen: map = virtualScreenTarget(map); break;
    case TabletWin32Mapping::Custom:
        map.outX = cfg.customX;
        map.outY = cfg.customY;
        map.outW = cfg.customW;
        map.outH = cfg.customH;
        break;
    }
    // A driver that declares a degenerate screen rect (it happens) would otherwise pin every sample
    // to one point; the virtual desktop is always a real rectangle.
    if (!(map.outW > 0.0) || !(map.outH > 0.0))
        map = virtualScreenTarget(map);

    lc.lcOptions |= wintab::kCxoMessages; // packets as window messages, which is our only tap (§5)
    lc.lcPktData = wintab::kPacketData;
    lc.lcPktMode = wintab::kPacketMode; // everything absolute (header)
    lc.lcMoveMask = wintab::kPacketData; // any field we asked for changing counts as movement
    lc.lcBtnUpMask = lc.lcBtnDnMask;     // report releases as well as presses

    // ⚠ IDENTITY OUTPUT MAPPING, and this is the sub-pixel decision. Left at its default the driver
    // maps the tablet onto whole SCREEN PIXELS and pkX/pkY arrive as integers -- the fidelity is
    // gone before we see it, and a stroke becomes the staircase docs/tablet.md §3.5 spent a release
    // fixing on X11. Asking for the context's own INPUT units instead (typically ~100 units per mm,
    // tens of times finer than a pixel) keeps it, and winTabScreenPos does the map in double.
    lc.lcOutOrgX = lc.lcInOrgX;
    lc.lcOutExtX = lc.lcInExtX;
    lc.lcOutOrgY = lc.lcInOrgY;
    lc.lcOutExtY = lc.lcInExtY;

    m_ctxWindow = createPacketWindow();
    if (m_ctxWindow == nullptr) {
        stopWinTab();
        return false;
    }
    m_ctx = m_wt.open(m_ctxWindow, &lc, TRUE);
    if (m_ctx == nullptr) {
        // A driver that refuses the context is the case §5 names: report it, and let the caller
        // fall through to Windows Ink rather than paint with a tablet that is not talking.
        tabletLog().info("WinTab present but WTOpen failed; falling back");
        stopWinTab();
        return false;
    }

    // lcMsgBase is the driver's choice, not a constant. Read it back off the context we just
    // configured rather than assuming WT_DEFBASE (header).
    m_msgBase = lc.lcMsgBase;
    m_map = map;

    // The default packet queue is a handful deep; a 200 Hz pen overruns it between two frames and
    // the driver drops the middle of the stroke. Best-effort: a driver without the 1.1 entry point
    // just keeps its default.
    if (m_wt.queueSizeSet != nullptr)
        m_wt.queueSizeSet(m_ctx, kPacketBatch);
    if (m_wt.overlap != nullptr)
        m_wt.overlap(m_ctx, TRUE); // put our context on top of any other app's

    enumerateWinTabDevices();
    return true;
}

void TabletWin32::stopWinTab() {
    if (m_ctx != nullptr && m_wt.close != nullptr)
        m_wt.close(m_ctx);
    m_ctx = nullptr;
    if (m_ctxWindow != nullptr)
        DestroyWindow(m_ctxWindow);
    m_ctxWindow = nullptr;
    // The window class outlives the window on purpose: it is process-global, RegisterClassExW
    // tolerates the duplicate on a second init(), and unregistering it while any window of the
    // class could still exist is the failure mode that buys nothing.
    wintab::unload(m_wt);
}

void TabletWin32::enumerateWinTabDevices() {
    m_devices.clear();
    m_cursors.clear();
    if (!m_wt.loaded())
        return;

    UINT deviceCount = 0;
    m_wt.info(wintab::kWtiInterface, wintab::kIfcNDevices, &deviceCount);
    for (UINT i = 0; i < deviceCount; ++i) {
        const UINT category = wintab::kWtiDevices + i;
        WinTabDevice dev;

        // Every WTInfoA string query answers its own length first, terminator included.
        const UINT nameBytes = m_wt.info(category, wintab::kDvcName, nullptr);
        if (nameBytes > 0) {
            std::string raw(nameBytes, '\0');
            if (m_wt.info(category, wintab::kDvcName, raw.data()) > 0)
                dev.name = ansiToUtf8(raw.data(), raw.size());
        }

        UINT firstCursor = 0;
        UINT cursorTypes = 0;
        if (m_wt.info(category, wintab::kDvcFirstCsr, &firstCursor) > 0)
            dev.firstCursor = firstCursor;
        if (m_wt.info(category, wintab::kDvcNCsrTypes, &cursorTypes) > 0)
            dev.cursorCount = cursorTypes;

        wintab::AXIS axis{};
        if (m_wt.info(category, wintab::kDvcNPressure, &axis) > 0 && axis.axMax != axis.axMin) {
            dev.pressure.min = static_cast<double>(axis.axMin);
            dev.pressure.max = static_cast<double>(axis.axMax);
            dev.pressure.present = true;
        }
        axis = wintab::AXIS{};
        if (m_wt.info(category, wintab::kDvcTPressure, &axis) > 0 && axis.axMax != axis.axMin) {
            dev.tangential.min = static_cast<double>(axis.axMin);
            dev.tangential.max = static_cast<double>(axis.axMax);
            dev.tangential.present = true;
        }

        // DVC_ORIENTATION answers THREE axes in one call: azimuth, altitude, twist.
        wintab::AXIS orientation[3]{};
        if (m_wt.info(category, wintab::kDvcOrientation, orientation) > 0) {
            dev.azimuthDegPerUnit = winTabDegreesPerUnit(orientation[0]);
            dev.altitudeDegPerUnit = winTabDegreesPerUnit(orientation[1]);
            dev.twistDegPerUnit = winTabDegreesPerUnit(orientation[2]);
        }

        m_devices.push_back(std::move(dev));
    }

    UINT cursorCount = 0;
    m_wt.info(wintab::kWtiInterface, wintab::kIfcNCursors, &cursorCount);
    m_cursors.resize(cursorCount);
}

const WinTabCursor& TabletWin32::resolveCursor(unsigned index) {
    // The cursor list is sized from IFC_NCURSORS, but a driver may report a cursor beyond what it
    // counted (a tool plugged in after we enumerated). Grow rather than reject: an unnamed cursor
    // still classifies as a Pen, which paints.
    if (index >= m_cursors.size())
        m_cursors.resize(static_cast<std::size_t>(index) + 1);
    WinTabCursor& cursor = m_cursors[index];
    if (cursor.resolved)
        return cursor;

    const UINT category = wintab::kWtiCursors + index;
    const UINT nameBytes = m_wt.info(category, wintab::kCsrName, nullptr);
    if (nameBytes > 0) {
        std::string raw(nameBytes, '\0');
        if (m_wt.info(category, wintab::kCsrName, raw.data()) > 0)
            cursor.name = ansiToUtf8(raw.data(), raw.size());
    }
    UINT type = 0;
    if (m_wt.info(category, wintab::kCsrType, &type) > 0)
        cursor.type = type;

    // Which device owns this cursor: each device declares the half-open range it covers.
    for (std::size_t i = 0; i < m_devices.size(); ++i) {
        const WinTabDevice& dev = m_devices[i];
        if (index >= dev.firstCursor && index < dev.firstCursor + dev.cursorCount) {
            cursor.device = i;
            break;
        }
    }

    cursor.tool = winTabTool(cursor.name, cursor.type);
    cursor.resolved = true;
    return cursor;
}

void TabletWin32::drainWinTabPackets() {
    if (m_ctx == nullptr || m_wt.packetsGet == nullptr || m_packets.empty())
        return;
    // WTPacketsGet lifts the whole queue in one go and removes what it returns, so one WT_PACKET
    // message can deliver every sample the pen produced since the last one -- which is exactly the
    // ~200 Hz stream §3.1 payoff 2 is about. Loop until the queue is dry: a batch that filled the
    // buffer means there is more behind it.
    const WinTabDevice noDevice; // stands in for a packet from a device we never enumerated
    for (;;) {
        const int got =
            m_wt.packetsGet(m_ctx, static_cast<int>(m_packets.size()), m_packets.data());
        if (got <= 0)
            return;
        const std::size_t count = std::min(static_cast<std::size_t>(got),
                                           m_packets.size()); // never trust a driver's count
        for (std::size_t i = 0; i < count; ++i) {
            // By value: resolveCursor can GROW the cursor list on a later iteration, and a
            // reference into the vector would not survive that.
            const wintab::Packet pkt = m_packets[i];
            const WinTabCursor cursor = resolveCursor(pkt.pkCursor);
            const WinTabDevice& dev =
                cursor.device < m_devices.size() ? m_devices[cursor.device] : noDevice;
            pushDesktopSample(winTabParsePacket(dev, cursor, pkt, m_map, ingestClockUs()), nullptr);
        }
        if (count < m_packets.size())
            return;
    }
}

void TabletWin32::handleInkMessage(const MSG& msg) {
    const user32api::Api& u32 = user32api::api();
    if (!user32api::inkAvailable())
        return;

    const UINT32 pointerId = GET_POINTERID_WPARAM(msg.wParam);
    POINTER_INPUT_TYPE type = 0;
    if (u32.getPointerType(pointerId, &type) == 0 ||
        type != static_cast<POINTER_INPUT_TYPE>(PT_PEN))
        return; // touch and mouse pointers are FLTK's business, not ours

    POINTER_PEN_INFO pen{};
    if (u32.getPointerPenInfo(pointerId, &pen) == 0)
        return;

    // ⚠ THE HISTORY IS THE FIDELITY. Windows coalesces a fast pen into ONE WM_POINTERUPDATE
    // carrying a historyCount of the samples it stands for; taking only the newest turns a 200 Hz
    // stroke back into a 60 Hz polygon, which is the same defect docs/tablet.md §3.1 payoff 2
    // exists to avoid. The entries come back NEWEST FIRST, so they are pushed in reverse to keep
    // the ring chronological -- SampleRing is ordered, and the dab walk depends on it.
    const UINT32 history = pen.pointerInfo.historyCount;
    if (u32.getPointerPenInfoHistory != nullptr && history > 1 && history <= kInkHistoryMax) {
        m_inkHistory.assign(history, POINTER_PEN_INFO{});
        UINT32 entries = history;
        if (u32.getPointerPenInfoHistory(pointerId, &entries, m_inkHistory.data()) != 0 &&
            entries > 0) {
            const UINT32 count = std::min(entries, history);
            for (UINT32 i = count; i > 0; --i)
                pushInk(m_inkHistory[i - 1]);
            return;
        }
    }
    pushInk(pen);
}

void TabletWin32::pushInk(const POINTER_PEN_INFO& pen) {
    m_inkSeenMask |= pen.penMask;
    m_inkSawPen = true;
    TabletSample sample =
        inkParsePen(pen, inkRectsFor(pen.pointerInfo.sourceDevice), ingestClockUs());
    m_inkTool = sample.tool;
    pushDesktopSample(sample, pen.pointerInfo.hwndTarget);
}

const InkDeviceRects& TabletWin32::inkRectsFor(HANDLE device) {
    for (std::size_t i = 0; i < m_inkRects.size(); ++i)
        if (m_inkRects[i].first == device)
            return m_inkRects[i].second;

    InkDeviceRects rects;
    RECT deviceRect{};
    RECT displayRect{};
    const auto getRects = user32api::api().getPointerDeviceRects;
    if (device != nullptr && getRects != nullptr &&
        getRects(device, &deviceRect, &displayRect) != 0) {
        rects.devX = static_cast<double>(deviceRect.left);
        rects.devY = static_cast<double>(deviceRect.top);
        rects.devW = static_cast<double>(deviceRect.right - deviceRect.left);
        rects.devH = static_cast<double>(deviceRect.bottom - deviceRect.top);
        rects.dispX = static_cast<double>(displayRect.left);
        rects.dispY = static_cast<double>(displayRect.top);
        rects.dispW = static_cast<double>(displayRect.right - displayRect.left);
        rects.dispH = static_cast<double>(displayRect.bottom - displayRect.top);
        rects.valid =
            rects.devW > 0.0 && rects.devH > 0.0 && rects.dispW > 0.0 && rects.dispH > 0.0;
    }
    m_inkRects.emplace_back(device, rects);
    return m_inkRects.back().second;
}

HWND TabletWin32::targetWindow(const common::Vec2& desktopPos, HWND hint) const {
    if (isOurWindow(hint))
        return hint;
    // ⚠ CAPTURE FIRST, and this is not a nicety. A brush stroke dragged off the edge of the canvas
    // is ordinary use, and while it is in flight FLTK holds the mouse capture (Fl_win32.cxx calls
    // SetCapture on the push) and keeps receiving FL_DRAG. Asking WindowFromPoint at that moment
    // answers with whatever is under the pen -- another app, the desktop -- and the sample would be
    // dropped, which makes the drain synthesize a pressure-1.0 fallback in the middle of a stroke.
    // GetCapture is per-thread, so it can only ever name a window of ours.
    if (HWND captured = GetCapture(); isOurWindow(captured))
        return captured;
    const POINT point{static_cast<LONG>(std::lround(desktopPos.x)),
                      static_cast<LONG>(std::lround(desktopPos.y))};
    HWND under = WindowFromPoint(point);
    return isOurWindow(under) ? under : nullptr;
}

void TabletWin32::pushDesktopSample(TabletSample sample, HWND hint) {
    HWND target = targetWindow(sample.pos, hint);
    if (target == nullptr)
        return; // the pen is over another application: `pos` would be a lie, so say nothing at all
    POINT origin{0, 0};
    if (ClientToScreen(target, &origin) == 0)
        return;
    // TabletSample::pos is SURFACE-LOCAL by contract (tablet.hpp), and `surface` says which surface
    // -- the wiring feeds a canvas stroke only from the canvas's own window, and reads everything
    // else for the Settings -> Tablet test area (§8). The subtraction stays in double, so the
    // sub-pixel part survives.
    sample.pos = {sample.pos.x - static_cast<double>(origin.x),
                  sample.pos.y - static_cast<double>(origin.y)};
    sample.surface = reinterpret_cast<std::uint64_t>(target);
    m_ring.push(sample);
}

bool TabletWin32::handleMessage(const MSG& msg) {
    if (!m_available)
        return false;

    // Driver ExpressKey suppression (§5): many tablet drivers emit F13-F24 for their hardware
    // buttons, and Mosaic's keymap would take those as real shortcuts. Swallowed unconditionally,
    // and it costs nothing: ui::keymap only ever produces F1-F12, so no binding can exist in this
    // range to lose.
    //
    // ⚠ Keys only. Nothing about a PEN gesture is ever made to depend on the key stream -- that is
    // the standing Wayland lesson, and it holds on every platform.
    switch (msg.message) {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        return msg.wParam >= static_cast<WPARAM>(VK_F13) &&
               msg.wParam <= static_cast<WPARAM>(VK_F24);
    default: break;
    }

    if (m_api == TabletWin32Api::WinTab) {
        if (m_ctx == nullptr)
            return false;
        if (msg.message == m_msgBase + wintab::kWtPacket) {
            drainWinTabPackets();
        } else if (msg.message == m_msgBase + wintab::kWtInfoChange) {
            // The device set or its configuration changed under us -- the WinTab analogue of XI2's
            // XI_HierarchyChanged. Rebuild the axis ranges and drop the cursor cache; the context
            // itself survives, so the packet stream is not interrupted.
            enumerateWinTabDevices();
        }
        // WT_PROXIMITY is deliberately not tracked. Every packet already carries TPS_PROXIMITY, and
        // a MISSED proximity-out would leave a latched flag saying a stylus is on the tablet
        // forever -- which is what the wiring branches on to defer a stroke's first dab, so a mouse
        // press would stop painting until it was dragged. The sample stream cannot get stuck that
        // way; a latched bool can.
        return false; // NEVER swallow: the promoted mouse stream is what drives the stroke (§3.1)
    }

    switch (msg.message) {
    case WM_POINTERDOWN:
    case WM_POINTERUPDATE:
    case WM_POINTERUP:
        handleInkMessage(msg);
        break;
    default: break;
    }
    return false; // NEVER swallow, for the same reason
}

void TabletWin32::watchWindow(HWND window) {
    if (!m_available || window == nullptr)
        return;
    if (std::find(m_feedbackWindows.begin(), m_feedbackWindows.end(), window) !=
        m_feedbackWindows.end())
        return;
    disablePenFeedback(window);
    m_feedbackWindows.push_back(window);
}

void TabletWin32::unwatchWindow(HWND window) {
    const auto it = std::find(m_feedbackWindows.begin(), m_feedbackWindows.end(), window);
    if (it != m_feedbackWindows.end())
        m_feedbackWindows.erase(it);
}

std::vector<TabletWin32DeviceInfo> TabletWin32::devices() const {
    const auto axisList = [](bool pressure, bool tilt, bool wheel, bool rotation) {
        std::string out;
        const auto add = [&out](const char* label, bool present) {
            if (!present)
                return;
            if (!out.empty())
                out += ", ";
            out += label;
        };
        add("pressure", pressure);
        add("tilt", tilt);
        add("wheel", wheel);
        add("rotation", rotation);
        return out;
    };

    std::vector<TabletWin32DeviceInfo> out;
    if (m_api == TabletWin32Api::WinTab) {
        out.reserve(m_devices.size());
        for (const WinTabDevice& dev : m_devices)
            out.push_back({dev.name.empty() ? std::string("Tablet") : dev.name,
                           axisList(dev.pressure.present, dev.hasTilt(), dev.tangential.present,
                                    dev.hasTwist()),
                           TabletSample::Tool::Pen, false});
        return out;
    }

    // Windows Ink enumerates nothing: the OS reports a pen when there is one, so the honest row is
    // what we have actually SEEN this pen send, and no row at all until it has sent something.
    if (m_api == TabletWin32Api::PointerInk && m_inkSawPen) {
        out.push_back({std::string("Pen (Windows Ink)"),
                       axisList((m_inkSeenMask & PEN_MASK_PRESSURE) != 0,
                                (m_inkSeenMask & (PEN_MASK_TILT_X | PEN_MASK_TILT_Y)) != 0, false,
                                (m_inkSeenMask & PEN_MASK_ROTATION) != 0),
                       m_inkTool, true});
    }
    return out;
}

} // namespace mosaic::platform
