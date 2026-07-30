// Windows implementation of platform/display_refresh.hpp.
//
// This translation unit REPLACES display_refresh.cpp on Windows (see platform/CMakeLists.txt):
// FLTK is built here with its native Win32/GDI backend, so neither the RandR nor the wl_output path
// exists, and the question is answered by the display driver's own mode record instead.

#include "platform/display_refresh.hpp"

#include <FL/Fl_Window.H>
#include <FL/platform.H> // fl_win32_xid()

// Named even though FL/platform.H has already dragged it in -- the calls below are OURS, so an FLTK
// that stopped leaking the system header must not break this file at a distance
// (native_window_win32.cpp carries the same note). NOMINMAX is set globally by the toolchain file.
#include <windows.h>

namespace mosaic::platform {

double displayRefreshHz(Fl_Window* win) {
    if (win == nullptr || win->shown() == 0)
        return 0.0;
    HWND hwnd = fl_win32_xid(win);
    if (hwnd == nullptr)
        return 0.0;
    // MONITOR_DEFAULTTONEAREST rather than ...TONULL: a window being dragged can momentarily sit
    // between two monitors, and "the closest panel" is a better pacing answer there than "no idea".
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr)
        return 0.0;
    // MONITORINFOEXW (not MONITORINFO): the wide variant is the one that carries szDevice, the
    // \\.\DISPLAYn name EnumDisplaySettingsW needs to identify which panel we are asking about. Its
    // cbSize is how the API tells the two structs apart, so it is not optional.
    MONITORINFOEXW info{};
    info.cbSize = sizeof(MONITORINFOEXW);
    if (GetMonitorInfoW(monitor, &info) == 0)
        return 0.0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(DEVMODEW);
    if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) == 0)
        return 0.0;
    // ⚠ dmDisplayFrequency is documented to come back as 0 OR 1 to mean "the hardware's default
    // rate", i.e. "I am not telling you a number" -- both must read as unknown, or a 1 Hz pacing
    // interval would freeze the canvas for a second at a time.
    if ((mode.dmFields & DM_DISPLAYFREQUENCY) == 0 || mode.dmDisplayFrequency <= 1)
        return 0.0;
    // An INTEGER frequency: a 59.94 Hz panel reports 60, a 143.98 Hz one reports 144. Rounding up
    // by a fraction of a percent costs at most one extra present every few thousand frames, which
    // is why this is not worth going to the DXGI/QueryDisplayConfig lengths to refine.
    return static_cast<double>(mode.dmDisplayFrequency);
}

} // namespace mosaic::platform
