#pragma once

#include "common/geometry.hpp"
#include "platform/tablet.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// <windows.h> LAST, deliberately, the same rule tablet_input.cpp applies to Xlib: it #defines a
// crowd of bare macros (and, without WIN32_LEAN_AND_MEAN, drags in most of the Win32 API), and
// anything it is included ahead of gets to fight them. Every header above is clean of it. The
// toolchain defines NOMINMAX project-wide, so std::min/std::max survive.
#include <windows.h>

// The Windows tablet/stylus backend (docs/tablet.md §5) -- the S57 sibling of tablet_x11.{hpp,cpp}
// and tablet_wayland.{hpp,cpp}. Windows has no vendor-neutral tablet stack, so this TU carries TWO
// device paths behind one TabletBackend, and picks between them at init():
//
//   * WinTab -- the 1991 Wacom-authored de-facto standard, shipped by the VENDOR DRIVER as
//     Wintab32.dll and absent on a machine with no such driver. Full pressure resolution, the art
//     pen's barrel twist, the airbrush finger wheel, and the physical barrel buttons.
//   * The Windows Pointer Input Stack ("Windows Ink") -- WM_POINTER* + GetPointerPenInfo, shipped
//     by the OS itself. Driver-independent, so it is the only path on the Surface / Elan /
//     Synaptics digitizers that ship no WinTab driver at all. Pressure, tilt and rotation, but no
//     airbrush wheel, and barrel buttons that many drivers simply do not surface through it.
//
// DEFAULT POLICY: WinTab when its DLL loads, a tablet answers, and a context opens; Windows Ink
// otherwise -- including when WinTab was asked for explicitly and failed, which is logged rather
// than left to paint with a dead tablet (§5). The reasoning, and the evidence behind it, is in
// docs/tablet.md §5a. A machine with neither degrades to plain mouse input at pressure 1.0, exactly
// as the Linux backends do with no tablet present (§3.2).
//
// LIFECYCLE: this is the X11 design (§3.1), not the Wayland one. Windows keeps promoting pen input
// to the legacy mouse stream for compatibility, so FLTK's FL_PUSH/FL_DRAG/FL_MOVE/FL_RELEASE
// arrives by itself and this backend only has to supply the valuators: it fills the sample ring,
// and the canvas drains it. Nothing synthesizes pointer events here (that is Wayland's problem,
// where binding the tablet manager takes the pointer stream away), and nothing owns the tool
// cursor (Windows draws it).
//
// Split for headless testing (§9) the same way the other two backends are: everything from a raw
// packet to a normalized TabletSample -- axis normalization, the orientation-to-tilt conversion,
// tool classification, the coordinate map -- is a pure function over plain structs, so a test can
// hand-build a packet and never open a driver. Only TabletWin32 itself (LoadLibraryW, the context,
// the message tap) needs a live Windows session.
//
// This header includes <windows.h>; it is included only by tablet_win32.cpp, the headless test, and
// the app-side wiring TU that installs the Fl::add_system_handler -- the platform tablet MODEL
// (tablet.hpp) stays Windows-free.
namespace mosaic::platform {

// ---------------------------------------------------------------------------------------------
// The slice of the WinTab ABI this backend touches, declared BY HAND
// ---------------------------------------------------------------------------------------------
//
// ⚠ THERE IS NO WINTAB HEADER TO INCLUDE. wintab.h and pktdef.h are the tablet vendor's SDK
// headers (LCS/Telegraphics, 1991-1998), not Microsoft's: MinGW-w64 ships neither, and vendoring a
// third-party SDK into this tree is not something we do. The interface itself is a published,
// three-decade-old ABI, and what follows is only the handful of handles, structs, constants and
// entry points Mosaic calls -- written out with the field ORDER and TYPES the ABI fixes.
// Cross-checked field by field against the Wine project's own independent declaration of the same
// interface, which is a factual API reference in exactly the way Krita's tablet code is.
//
// ⚠ AND IT IS LOADED AT RUNTIME, NEVER LINKED. Wintab32.dll is part of the tablet DRIVER, so a
// machine with no tablet driver does not have it -- an import-table reference would make Mosaic
// fail to START there, which is the worst possible outcome for a courtesy feature. LoadLibraryW +
// GetProcAddress, and a clean fall through to Windows Ink when it is missing.
namespace wintab {

// DECLARE_HANDLE(HCTX) -- an opaque context handle. Written out rather than macro-expanded so this
// declaration block reads as the ABI description it is.
struct HCTX__;
using HCTX = HCTX__*;

using WTPKT = DWORD; // a mask of PK_* bits: which fields a packet carries, in bit order
using FIX32 = DWORD; // 16.16 fixed point; the integer part is the high word

// The messages a CXO_MESSAGES context posts to its window. These are OFFSETS from the context's own
// lcMsgBase, which defaults to WT_DEFBASE but is the driver's to choose -- so the backend reads
// lcMsgBase from the context it opened and never assumes 0x7FF0 (a context whose base was moved
// would otherwise deliver packets we silently ignored).
// (WT_PROXIMITY, offset 5, is deliberately absent -- see TabletWin32::handleMessage: a latched
// proximity bool is a hazard the per-packet TPS_PROXIMITY bit does not have.)
inline constexpr UINT kWtDefBase = 0x7FF0;
inline constexpr UINT kWtPacket = 0;     // WT_PACKET:     wParam = serial, lParam = HCTX
inline constexpr UINT kWtInfoChange = 6; // WT_INFOCHANGE: the device set or its config changed

// WTPKT bits, in the order pktdef.h lays the corresponding fields out. Packet is built from exactly
// kPacketData below, so this order IS the struct order.
inline constexpr WTPKT kPkStatus = 0x0002;
inline constexpr WTPKT kPkCursor = 0x0020;
inline constexpr WTPKT kPkButtons = 0x0040;
inline constexpr WTPKT kPkX = 0x0080;
inline constexpr WTPKT kPkY = 0x0100;
inline constexpr WTPKT kPkNormalPressure = 0x0400;
inline constexpr WTPKT kPkTangentPressure = 0x0800;
inline constexpr WTPKT kPkOrientation = 0x1000;

// pkStatus bits.
inline constexpr UINT kTpsProximity = 0x0001; // the cursor is within the context
inline constexpr UINT kTpsInvert = 0x0010;    // the stylus is upside down: its ERASER end

// Context options and the info categories/indices the backend queries.
inline constexpr UINT kCxoMessages = 0x0004; // post WT_* messages to the context's window

inline constexpr UINT kWtiInterface = 1;
inline constexpr UINT kIfcNDevices = 4;
inline constexpr UINT kIfcNCursors = 5;

inline constexpr UINT kWtiDefSysCtx = 4; // the SYSTEM context template: not tied to window focus

inline constexpr UINT kWtiDevices = 100;
inline constexpr UINT kDvcName = 1;
inline constexpr UINT kDvcFirstCsr = 4;
inline constexpr UINT kDvcNCsrTypes = 3;
inline constexpr UINT kDvcNPressure = 15;
inline constexpr UINT kDvcTPressure = 16;
inline constexpr UINT kDvcOrientation = 17;

inline constexpr UINT kWtiCursors = 200;
inline constexpr UINT kCsrName = 1;
inline constexpr UINT kCsrType = 20;

// AXIS unit specifiers. Only TU_CIRCLE matters here: it is what makes the ORIENTATION axes ANGULAR
// rather than a unitless range (see WinTabDevice below).
inline constexpr UINT kTuCircle = 3;

struct AXIS {
    LONG axMin;
    LONG axMax;
    UINT axUnits;
    FIX32 axResolution;
};

struct ORIENTATION {
    int orAzimuth;  // compass bearing of the lean, in axResolution units per full circle
    int orAltitude; // elevation above the pad; negative when the stylus is inverted
    int orTwist;    // barrel rotation (the art pen), same angular units
};

inline constexpr std::size_t kNameLen = 40; // LCNAMELEN

// LOGCONTEXTA -- the A (ANSI) form, which is the one every driver since 1991 exports. The W form
// exists in the 1.1 spec and differs ONLY in lcName's element type, which shifts every field after
// it: supporting both would mean two layouts and two code paths for a string we use for nothing.
// Device names are read through WTInfoA and converted out of the active code page instead.
struct LOGCONTEXTA {
    char lcName[kNameLen];
    UINT lcOptions;
    UINT lcStatus;
    UINT lcLocks;
    UINT lcMsgBase;
    UINT lcDevice;
    UINT lcPktRate;
    WTPKT lcPktData;
    WTPKT lcPktMode;
    WTPKT lcMoveMask;
    DWORD lcBtnDnMask;
    DWORD lcBtnUpMask;
    LONG lcInOrgX;
    LONG lcInOrgY;
    LONG lcInOrgZ;
    LONG lcInExtX;
    LONG lcInExtY;
    LONG lcInExtZ;
    LONG lcOutOrgX;
    LONG lcOutOrgY;
    LONG lcOutOrgZ;
    LONG lcOutExtX;
    LONG lcOutExtY;
    LONG lcOutExtZ;
    FIX32 lcSensX;
    FIX32 lcSensY;
    FIX32 lcSensZ;
    BOOL lcSysMode;
    int lcSysOrgX;
    int lcSysOrgY;
    int lcSysExtX;
    int lcSysExtY;
    FIX32 lcSysSensX;
    FIX32 lcSysSensY;
};

// What the backend asks every packet to carry, and the struct that layout produces.
//
// PK_CONTEXT is deliberately NOT requested: it is the one field that is pointer-sized, and leaving
// it out keeps every member of Packet four bytes wide -- so the struct has no padding on any ABI
// and cannot silently disagree with what the driver writes. We know which context a packet came
// from anyway; there is only ever one.
//
// PACKETMODE is 0 (every field ABSOLUTE). In relative mode pkButtons would carry a transition
// instead of a state and pkNormalPressure would be signed, which is a different struct AND a
// different normalization.
inline constexpr WTPKT kPacketData = kPkStatus | kPkCursor | kPkButtons | kPkX | kPkY |
                                     kPkNormalPressure | kPkTangentPressure | kPkOrientation;
inline constexpr WTPKT kPacketMode = 0;

struct Packet {
    UINT pkStatus;
    UINT pkCursor;
    DWORD pkButtons;
    LONG pkX;
    LONG pkY;
    UINT pkNormalPressure;
    UINT pkTangentPressure;
    ORIENTATION pkOrientation;
};
static_assert(sizeof(Packet) == 10 * 4,
              "WinTab writes this struct itself: every field must be 4 bytes with no padding, or "
              "the driver and Mosaic disagree about where pkX is");

using WTInfoAFn = UINT(WINAPI*)(UINT, UINT, LPVOID);
using WTOpenAFn = HCTX(WINAPI*)(HWND, LOGCONTEXTA*, BOOL);
using WTCloseFn = BOOL(WINAPI*)(HCTX);
using WTPacketsGetFn = int(WINAPI*)(HCTX, int, LPVOID);
using WTOverlapFn = BOOL(WINAPI*)(HCTX, BOOL);
using WTQueueSizeSetFn = BOOL(WINAPI*)(HCTX, int);

// The resolved entry points -- and only the ones this backend actually calls, so that this block
// stays an honest inventory of what Mosaic asks of a tablet driver. `overlap` and `queueSizeSet`
// are OPTIONAL (both are 1.1 additions, and a driver without them merely keeps its defaults), so
// loaded() requires the four that the packet path cannot work without.
struct Api {
    HMODULE dll = nullptr;
    WTInfoAFn info = nullptr;
    WTOpenAFn open = nullptr;
    WTCloseFn close = nullptr;
    WTPacketsGetFn packetsGet = nullptr;
    WTOverlapFn overlap = nullptr;
    WTQueueSizeSetFn queueSizeSet = nullptr;

    [[nodiscard]] bool loaded() const noexcept {
        return dll != nullptr && info != nullptr && open != nullptr && close != nullptr &&
               packetsGet != nullptr;
    }
};

// Resolve Wintab32.dll and its entry points. False = no tablet driver on this machine (the common,
// expected case), or one too old to have the entry points above.
[[nodiscard]] bool load(Api& out);
void unload(Api& api);

} // namespace wintab

// ---------------------------------------------------------------------------------------------
// WinTab: the pure normalization half
// ---------------------------------------------------------------------------------------------

// A driver-declared axis, reduced to what normalization needs. `present` is false when the driver
// reports the axis at all -- a pressure axis that is absent reports 1.0, never 0 (§3.2).
struct WinTabAxis {
    double min = 0.0;
    double max = 0.0;
    bool present = false;
};

// One WinTab device -- a tablet -- as the parse path understands it.
struct WinTabDevice {
    std::string name;         // DVC_NAME, converted out of the active code page into UTF-8
    unsigned firstCursor = 0; // DVC_FIRSTCSR: this device owns cursors [first, first + count)
    unsigned cursorCount = 0; // DVC_NCSRTYPES
    WinTabAxis pressure;      // DVC_NPRESSURE
    WinTabAxis tangential;    // DVC_TPRESSURE -- the airbrush finger wheel / barrel pressure

    // ⚠ WinTab's tilt is NOT a unitless range, and that decides how it is normalized.
    //
    // tablet.hpp's rule: a backend whose driver reports tilt as a unitless range maps the device's
    // full range onto kTiltFullScaleDegrees, while a backend that already speaks degrees passes its
    // value through unscaled. WinTab reports ORIENTATION -- azimuth, altitude and twist -- as
    // ANGLES, in axis units whose AXIS declares `axUnits = TU_CIRCLE` and `axResolution` = units
    // per full circle (Wacom declares 3600, i.e. tenths of a degree). So the units are known and
    // convertible: these fields hold the DEGREES PER UNIT read off that declaration, the parse
    // converts to real degrees, and the derived x/y tilt passes through UNSCALED like Wayland's and
    // NSEvent's. Nothing here is multiplied by kTiltFullScaleDegrees.
    //
    // 0 = the axis is not reported (a declared resolution of zero is how a driver says "no tilt").
    // Falls back to a tenth of a degree per unit when a driver declares TU_CIRCLE with a nonsense
    // resolution, which is both the Wacom convention and what other implementations hardcode.
    double azimuthDegPerUnit = 0.0;
    double altitudeDegPerUnit = 0.0;
    double twistDegPerUnit = 0.0;

    [[nodiscard]] bool hasTilt() const noexcept {
        return azimuthDegPerUnit > 0.0 && altitudeDegPerUnit > 0.0;
    }
    [[nodiscard]] bool hasTwist() const noexcept { return twistDegPerUnit > 0.0; }
};

// One WinTab cursor -- a physical TOOL, not a device: a stylus's nib end, the same stylus's eraser
// end, an airbrush, a puck. Every packet names its cursor, and the cursor is what decides the
// TabletSample::Tool.
struct WinTabCursor {
    std::string name;    // CSR_NAME ("Pressure Stylus", "Eraser", "Airbrush", "Puck", ...)
    unsigned type = 0;   // CSR_TYPE: a vendor-defined bitfield, cross-checked below
    std::size_t device = 0; // index into the device list this cursor belongs to
    TabletSample::Tool tool = TabletSample::Tool::Pen;
    bool resolved = false; // filled lazily, the first time a packet names this cursor
};

// Where the context's INPUT range lands on the virtual desktop. The backend asks the driver for an
// IDENTITY output mapping -- so packets arrive in the context's own device units, which are far
// finer than screen pixels -- and does this map itself in double precision. That is where the
// sub-pixel position comes from (§3.1 payoff 3); a driver-side mapping onto screen pixels would
// hand us whole integers and throw the fidelity away before we ever saw it.
struct WinTabMap {
    double inOrgX = 0.0; // the context's lcInOrgX / lcInExtX, in device units
    double inExtX = 0.0;
    double inOrgY = 0.0;
    double inExtY = 0.0;
    double outX = 0.0; // the target rect on the virtual desktop, in pixels
    double outY = 0.0;
    double outW = 0.0;
    double outH = 0.0;
    // WinTab's input y grows AWAY from the user; a screen's grows down. Every shipping
    // implementation flips it unconditionally, and so do we.
    bool flipY = true;

    [[nodiscard]] bool usable() const noexcept {
        return inExtX != 0.0 && inExtY != 0.0 && outW > 0.0 && outH > 0.0;
    }
};

// Degrees per axis unit for an angular (TU_CIRCLE) AXIS: 360 / units-per-circle. Returns 0 for an
// axis the driver does not report, and the Wacom convention (0.1 deg/unit) for one that declares
// itself angular with an unusable resolution.
[[nodiscard]] double winTabDegreesPerUnit(const wintab::AXIS& axis) noexcept;

// Classify a cursor into the unified model's Tool. NAME FIRST, case-insensitively and by CONTAINS,
// which is the same classifier the XI2 backend uses on device names and for the same reason: it is
// the one thing every driver gets right. CSR_TYPE's vendor bitfield is the fallback; an unknown
// cursor is a Pen, because a stylus is the safe assumption for a drawing tool.
[[nodiscard]] TabletSample::Tool winTabTool(std::string_view cursorName,
                                            unsigned cursorType) noexcept;

// ORIENTATION's azimuth/altitude (already in DEGREES) as the x/y tilt pair TabletSample carries.
//
// WinTab describes the lean as a compass bearing plus an elevation above the pad; every other
// backend reports two independent lean angles. The conversion is the standard one:
//   xTilt =  atan(sin(azimuth) / tan(|altitude|)),  yTilt = -atan(cos(azimuth) / tan(|altitude|))
// An upright pen (altitude 90 deg) comes out exactly (0, 0); a pen flat on the pad comes out at
// +/-90, which the sensor layer saturates against its own full scale (core::brush::kMaxTiltDegrees)
// rather than rescaling.
//
// |altitude| because a NEGATIVE altitude means the stylus is inverted -- the eraser end -- which
// says nothing about which way it leans.
[[nodiscard]] common::Vec2 winTabTilt(double azimuthDeg, double altitudeDeg) noexcept;

// ORIENTATION's twist (in DEGREES) as TabletSample::rotation, wrapped into [-180, 180].
[[nodiscard]] double winTabRotation(double twistDeg) noexcept;

// A packet's pkX/pkY as a position on the VIRTUAL DESKTOP, sub-pixel. An unusable map yields
// {0,0}; the caller drops such a sample rather than painting at the origin.
[[nodiscard]] common::Vec2 winTabScreenPos(const WinTabMap& map, LONG pkX, LONG pkY) noexcept;

// One packet -> one normalized sample. Pure over the structs, so a test hand-builds the packet.
//
// ⚠ `pos` comes out in VIRTUAL-DESKTOP pixels and `surface` is left 0: a WinTab context reports the
// pen wherever it is on the desktop, with no notion of which window that is. TabletWin32 resolves
// the target window and rebases `pos` into its client area, because TabletSample::pos is
// SURFACE-LOCAL by contract (tablet.hpp).
//
// Normalization:
//  - pressure: (raw - min) / (max - min), clamped. No pressure axis, or a degenerate declared
//    range, reports 1.0 -- NOT 0 -- so dynamics do not silently collapse the stroke (§3.2).
//  - tangentialPressure: the same two-sided remap of DVC_TPRESSURE; absent -> 0.0 (rest).
//  - tilt / rotation: real degrees, unscaled (see WinTabDevice).
//  - buttons: pkButtons IS a state mask in absolute mode -- bit 0 is the tip, exactly as on X11
//    and Wayland, and the barrel buttons follow above it.
//  - inProximity: pkStatus's TPS_PROXIMITY.
//  - tool: the cursor's, but TPS_INVERT overrides it to Eraser -- an inverted stylus IS the eraser
//    end, and that bit is the driver saying so per packet rather than us inferring it from a name.
//  - timeUs: the caller's ingest clock. pkTime is not even requested; driver clocks are unreliable
//    by design (§5).
[[nodiscard]] TabletSample winTabParsePacket(const WinTabDevice& dev, const WinTabCursor& cursor,
                                             const wintab::Packet& pkt, const WinTabMap& map,
                                             std::uint64_t timeUs) noexcept;

// ---------------------------------------------------------------------------------------------
// Windows Ink: the pure normalization half
// ---------------------------------------------------------------------------------------------

// Pen pressure's full scale in the Pointer Input Stack. Fixed by the API, not by the device: a
// driver that does not report pressure sends 0 and clears PEN_MASK_PRESSURE.
inline constexpr double kInkPressureMax = 1024.0;

// A pen digitizer's HIMETRIC extent and the display rectangle it maps onto, from
// GetPointerDeviceRects. This is where Ink's sub-pixel position comes from: POINTER_INFO's
// ptPixelLocation is rounded to whole pixels, while ptHimetricLocationRaw is the digitizer's own
// 0.01 mm grid -- far finer than a pixel -- and these two rectangles are the only way to turn one
// into the other. `valid` false = we could not read them, and the parse falls back to the integer
// pixel location (a stroke still paints; it is merely quantized).
struct InkDeviceRects {
    double devX = 0.0; // HIMETRIC
    double devY = 0.0;
    double devW = 0.0;
    double devH = 0.0;
    double dispX = 0.0; // pixels on the virtual desktop
    double dispY = 0.0;
    double dispW = 0.0;
    double dispH = 0.0;
    bool valid = false;
};

// One POINTER_PEN_INFO -> one normalized sample. Pure, so a test hand-builds the struct.
//
// ⚠ As with winTabParsePacket, `pos` comes out in VIRTUAL-DESKTOP pixels and `surface` is 0;
// TabletWin32 rebases both.
//
// Normalization:
//  - pressure: pressure / 1024, clamped. PEN_MASK_PRESSURE clear (or a zero reading) -> 1.0 (§3.2).
//  - tilt: tiltX/tiltY are ALREADY DEGREES in [-90, 90] and pass through unscaled (tablet.hpp).
//  - rotation: `rotation` is degrees in [0, 359]; wrapped into [-180, 180] to match the model.
//  - tangentialPressure: 0.0. The Pointer Input Stack has no airbrush wheel -- that is one of the
//    two things WinTab still exists for, and inventing a value would be a lie the airbrush reads.
//  - tool: PEN_FLAG_INVERTED / PEN_FLAG_ERASER -> Eraser, else Pen. The API has no airbrush or
//    puck concept at all.
//  - toolSerial: 0. There is no per-stylus serial here; sourceDevice identifies the DIGITIZER, not
//    the pen, and reporting it as a tool serial would make two different pens look like one.
//  - buttons: bit 0 = POINTER_FLAG_INCONTACT (the tip, as on every other backend), bit 1 = the
//    barrel button, bit 2 = the third button. ⚠ Many drivers never set these at all -- see the
//    barrel-button note in docs/tablet.md §5a: the right/middle click still arrives, as an ordinary
//    Windows mouse message that FLTK routes for us.
[[nodiscard]] TabletSample inkParsePen(const POINTER_PEN_INFO& pen, const InkDeviceRects& rects,
                                       std::uint64_t timeUs) noexcept;

// ---------------------------------------------------------------------------------------------
// The live backend
// ---------------------------------------------------------------------------------------------

// Which device path to use. Auto is the shipped policy (WinTab, then Ink); the other two exist for
// the diagnostic override the wiring reads out of the environment, and for Settings -> Tablet's
// API selector when it lands (docs/tablet.md §8).
enum class TabletWin32Api {
    Auto,
    WinTab,
    PointerInk,
};

// How WinTab's input range is mapped onto the desktop. `Driver` honours the tablet driver's own
// mapping, which is what the user configured in its control panel and therefore the default.
//
// The two overrides exist because drivers get this wrong in the field -- mapping the stylus to the
// wrong monitor, or to a rectangle that is not any monitor -- and a wrong mapping does not wobble,
// it paints hundreds of pixels from the pen (§5, the desktop-rect escape hatch).
enum class TabletWin32Mapping {
    Driver,
    VirtualScreen,
    Custom,
};

struct TabletWin32Config {
    TabletWin32Api api = TabletWin32Api::Auto;
    TabletWin32Mapping mapping = TabletWin32Mapping::Driver;
    // Only read when mapping == Custom, as a left/top/width/height rect on the virtual desktop.
    double customX = 0.0;
    double customY = 0.0;
    double customW = 0.0;
    double customH = 0.0;
};

// One detected device, for Settings -> Tablet's diagnostic row (§8). Purely descriptive, and it
// stops at the classification: naming a Tool for a human to read is the UI's job, exactly as it is
// for the XI2 backend's Xi2Device.
struct TabletWin32DeviceInfo {
    std::string name;
    std::string valuators; // which axes it actually reports ("pressure, tilt"), "" if none
    // The classified tool, WHEN THERE IS ONE to attribute to the row. WinTab names the tablet, and
    // its tools are separate cursors that come and go as the pen is picked up, so a WinTab row has
    // no single tool; a Windows Ink row has exactly the pen the OS is reporting.
    TabletSample::Tool tool = TabletSample::Tool::Pen;
    bool hasTool = false;
};

// The live backend. FLTK-free: init() takes the HWND the caller already has (the wiring passes
// fl_win32_xid(win) via platform::nativeSurfaceHandle), exactly as TabletX11::init takes a
// Display*/Window, and the wiring owns the Fl::add_system_handler registration that feeds
// handleMessage().
class TabletWin32 final : public TabletBackend {
public:
    TabletWin32() = default;
    ~TabletWin32() override;
    TabletWin32(const TabletWin32&) = delete;
    TabletWin32& operator=(const TabletWin32&) = delete;

    // Bring up whichever device path `cfg` selects (see TabletWin32Api). `window` is one of our
    // top-level windows -- used only to switch off Windows' pen gesture VISUALS on it, never to
    // receive packets -- and may be null. False = neither path came up, and the canvas falls back
    // to synthesized pressure-1 samples (§3.2).
    bool init(HWND window, const TabletWin32Config& cfg = {});

    // Feed one raw message, as FLTK's Windows driver hands it to a system handler (after
    // PeekMessageW, before TranslateMessage/DispatchMessageW).
    //
    // Returns true to SWALLOW the message -- which happens for exactly one thing, the driver
    // ExpressKey range (§5): many tablet drivers emit F13-F24 for their hardware buttons, and those
    // keystrokes would otherwise reach the keymap as real shortcuts. Tablet packets and pointer
    // messages are NEVER swallowed: the legacy mouse stream Windows promotes them into is what
    // keeps FLTK's stroke lifecycle alive, the same bargain the X11 backend strikes with the core
    // pointer (§3.1).
    bool handleMessage(const MSG& msg);

    [[nodiscard]] bool available() const noexcept override { return m_available; }
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] SampleRing& ring() noexcept override { return m_ring; }

    // Switch Windows' pen gesture visuals off for another window of ours. Unlike the X11 backend's
    // watchWindow this is NOT what makes the pen readable there -- both device paths report the pen
    // over every window of ours already, and each sample carries the window it came from -- so it
    // is a courtesy, not a requirement, and a window nobody registers still feeds the test area.
    void watchWindow(HWND window);
    void unwatchWindow(HWND window);

    // Which path actually came up, for the log line and for Settings -> Tablet.
    [[nodiscard]] TabletWin32Api activeApi() const noexcept { return m_api; }

    // Diagnostics for Settings -> Tablet's "detected devices" row (§8).
    [[nodiscard]] std::vector<TabletWin32DeviceInfo> devices() const;

private:
    bool startWinTab(const TabletWin32Config& cfg);
    void stopWinTab();
    void enumerateWinTabDevices();
    [[nodiscard]] const WinTabCursor& resolveCursor(unsigned index);
    void drainWinTabPackets();
    void handleInkMessage(const MSG& msg);
    void pushInk(const POINTER_PEN_INFO& pen);
    [[nodiscard]] const InkDeviceRects& inkRectsFor(HANDLE device);

    // Rebase a sample whose `pos` is on the virtual desktop into the client area of whichever
    // window of ours it belongs to, tag it with that window, and ring it. Drops the sample when the
    // pen is over another application: `pos` would be meaningless there, and the readout would
    // report a live stylus while the user is somewhere else entirely.
    void pushDesktopSample(TabletSample sample, HWND hint);
    [[nodiscard]] HWND targetWindow(const common::Vec2& desktopPos, HWND hint) const;

    bool m_available = false;
    // Which path is live. Only meaningful while m_available; Auto never survives init().
    TabletWin32Api m_api = TabletWin32Api::Auto;
    SampleRing m_ring;

    // ---- WinTab state ----
    wintab::Api m_wt{};
    wintab::HCTX m_ctx = nullptr;
    HWND m_ctxWindow = nullptr; // the message-only window the context posts to; ours to destroy
    UINT m_msgBase = wintab::kWtDefBase;
    WinTabMap m_map{};
    std::vector<WinTabDevice> m_devices;
    std::vector<WinTabCursor> m_cursors; // indexed by WinTab cursor number
    std::vector<wintab::Packet> m_packets; // the drain buffer, allocated once

    // ---- Windows Ink state ----
    // GetPointerDeviceRects is a per-digitizer constant, so it is cached; the list is one entry
    // long in practice and never long enough to want a map.
    std::vector<std::pair<HANDLE, InkDeviceRects>> m_inkRects;
    std::vector<POINTER_PEN_INFO> m_inkHistory; // the coalesced-sample buffer, allocated once
    PEN_MASK m_inkSeenMask = 0;                 // which axes this machine's pen has actually sent
    TabletSample::Tool m_inkTool = TabletSample::Tool::Pen;
    bool m_inkSawPen = false;

    // Windows we have already switched the pen gesture visuals off for, so a second watchWindow of
    // the same one is a no-op. unwatchWindow only forgets it -- it does NOT restore the default
    // setting, for the same reason TabletX11::unwatchWindow does not deselect events: a window is
    // unwatched precisely when it is going away, and the setting dies with it.
    std::vector<HWND> m_feedbackWindows;
};

} // namespace mosaic::platform
