// macOS implementation of platform/system_theme.hpp (PLAN.md S58), used INSTEAD of
// system_theme.cpp on Apple (see platform/CMakeLists.txt).
//
// system_theme.cpp discovers the host light/dark preference and accent colour by shelling out to
// `gdbus`/`gsettings` -- the XDG Desktop Portal that exists only on Linux desktops. macOS exposes
// the same facts natively through AppKit, so here we query Cocoa directly (no subprocess, no
// portal): the current NSAppearance for light/dark, and NSColor.controlAccentColor for the accent.
//
// The public behaviour matches the Linux backend: both functions are best-effort and never throw,
// returning NoPreference / std::nullopt when the answer cannot be determined.

#include "platform/system_theme.hpp"

#import <AppKit/AppKit.h>

#include <cmath>

namespace mosaic::platform {
namespace {

// Clamp a Cocoa CGFloat colour component in [0,1] to an 8-bit channel. (Same rule as the Linux
// backend's toByte; duplicated here because that helper lives in a separate translation unit.)
std::uint8_t toByte(double unit) {
    if (unit < 0.0)
        unit = 0.0;
    if (unit > 1.0)
        unit = 1.0;
    return static_cast<std::uint8_t>(std::lround(unit * 255.0));
}

// The NSAppearance Mosaic should follow. Normally the running app's effectiveAppearance, but that
// requires NSApp, which is nil until [NSApplication sharedApplication] has run. detectColorScheme()
// can be reached during early theme resolution (before FLTK creates the shared application), so we
// fall back to the process-wide drawing appearance, and finally to nil (handled by the caller).
NSAppearance* currentAppearance() {
    if (NSApp != nil) {
        if (NSAppearance* eff = [NSApp effectiveAppearance])
            return eff;
    }
    // No application object yet: use the appearance the system would draw with right now. This is
    // a class method (no NSApp needed) and defaults to Aqua when there is no drawing context.
    return [NSAppearance currentDrawingAppearance];
}

} // namespace

ColorScheme detectColorScheme() {
    @autoreleasepool {
        NSAppearance* appearance = currentAppearance();
        if (appearance == nil)
            return ColorScheme::NoPreference; // no basis to decide -> defer to Mosaic's default

        // Ask the appearance which of the two base looks it best matches, rather than string-testing
        // its name directly: an accent/tinted appearance still resolves to Aqua or DarkAqua here.
        NSAppearanceName best = [appearance
            bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
        if ([best isEqualToString:NSAppearanceNameDarkAqua])
            return ColorScheme::Dark;
        if ([best isEqualToString:NSAppearanceNameAqua])
            return ColorScheme::Light;
        return ColorScheme::NoPreference;
    }
}

std::optional<common::Color8> detectAccentColor() {
    @autoreleasepool {
        // The user's chosen accent (System Settings > Appearance > Accent colour). This is a dynamic
        // catalog colour; it always exists (>= macOS 10.14, well below our 13.3 floor), defaulting to
        // the "multicolour"/blue accent.
        NSColor* accent = [NSColor controlAccentColor];
        if (accent == nil)
            return std::nullopt;

        // Resolve the catalog colour to concrete sRGB components. controlAccentColor is not defined in
        // an RGB space, so redComponent/etc. would raise on it directly -- convert first. Returns nil
        // if the colour cannot be represented in sRGB (should not happen for the accent).
        NSColor* srgb = [accent colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
        if (srgb == nil)
            return std::nullopt;

        const CGFloat r = [srgb redComponent];
        const CGFloat g = [srgb greenComponent];
        const CGFloat b = [srgb blueComponent];
        return common::Color8{toByte(r), toByte(g), toByte(b), 255};
    }
}

} // namespace mosaic::platform
