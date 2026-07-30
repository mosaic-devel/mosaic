// macOS implementation of platform/display_refresh.hpp.
//
// This Objective-C++ translation unit REPLACES display_refresh.cpp on Apple platforms (see
// platform/CMakeLists.txt): FLTK is a pure-Cocoa build here, so neither the RandR nor the wl_output
// path exists, and the window's own NSScreen is what identifies the panel.

#include "platform/display_refresh.hpp"

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

#include <FL/Fl_Window.H>
#include <FL/platform.H> // fl_mac_xid() on macOS

namespace mosaic::platform {

double displayRefreshHz(Fl_Window* win) {
    if (win == nullptr || win->shown() == 0)
        return 0.0;
    NSWindow* nsWin = (NSWindow*)fl_mac_xid(win);
    if (nsWin == nil)
        return 0.0;
    // -[NSWindow screen] is nil for an off-screen (or fully hidden) window; the main screen is the
    // right guess then -- it is where the window will reappear.
    NSScreen* screen = [nsWin screen];
    if (screen == nil)
        screen = [NSScreen mainScreen];
    if (screen == nil)
        return 0.0;
    // The NSScreen -> CGDirectDisplayID hop is the documented one, and CGDisplayModeGetRefreshRate
    // is what carries the panel's cadence. NOT -[NSScreen maximumFramesPerSecond]: that is macOS
    // 12+, and this cross-build targets older SDKs.
    NSNumber* number = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
    if (number == nil)
        return 0.0;
    const CGDirectDisplayID display = (CGDirectDisplayID)[number unsignedIntValue];
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
    if (mode == nullptr)
        return 0.0;
    const double hz = CGDisplayModeGetRefreshRate(mode);
    CGDisplayModeRelease(mode);
    // ⚠ 0.0 is the DOCUMENTED answer for a display with no meaningful refresh rate to report, which
    // historically meant every built-in Mac panel. Handing it back as "unknown" is exactly right:
    // the caller's conservative assumption is 60 Hz, which is what those panels run at, and the
    // ProMotion ones do report their 120.
    return hz;
}

} // namespace mosaic::platform
