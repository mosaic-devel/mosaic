// Windows implementation of platform/system_theme.hpp (PLAN.md S57), used INSTEAD of
// system_theme.cpp on Windows (see platform/CMakeLists.txt).
//
// system_theme.cpp discovers the host light/dark preference and accent colour by shelling out to
// `gdbus`/`gsettings` -- the XDG Desktop Portal, which exists only on Linux desktops. Windows keeps
// both facts in the REGISTRY under HKCU, and that is what we read.
//
// ⚠ The registry, deliberately, and NOT WinRT's Windows.UI.ViewManagement.UISettings, which is the
// API Microsoft documents for this. Three reasons, in order of weight: MinGW's support for the
// C++/WinRT projection headers is uneven and Mosaic cross-builds with MinGW only (never MSVC), so a
// UISettings dependency would put the whole Windows build at the mercy of one boolean; these values
// are what the SHELL ITSELF acts on, so reading them cannot disagree with what the user is looking
// at; and there is no COM apartment to initialise, which matters because detectColorScheme() can be
// reached during early theme resolution, before FLTK exists.
//
// The public behaviour matches the other two backends: both functions are best-effort and never
// throw, returning NoPreference / std::nullopt when the answer cannot be determined.

#include "platform/system_theme.hpp"

#include <cstdint>
#include <windows.h>

namespace mosaic::platform {
namespace {

// Read one REG_DWORD from under HKEY_CURRENT_USER. Best-effort: false on any failure -- absent key,
// absent value, wrong type -- which both callers turn into "no answer" rather than into a guess.
//
// RegGetValueW rather than RegOpenKeyEx + RegQueryValueEx + RegCloseKey because it is that whole
// sequence in one call AND type-checks on our behalf: RRF_RT_REG_DWORD makes a value of any other
// type an error instead of four bytes of something else reinterpreted as a number.
bool readCurrentUserDword(const wchar_t* subKey, const wchar_t* value, DWORD& out) {
    DWORD data = 0;
    DWORD size = sizeof(data);
    const LONG rc =
        RegGetValueW(HKEY_CURRENT_USER, subKey, value, RRF_RT_REG_DWORD, nullptr, &data, &size);
    if (rc != ERROR_SUCCESS || size != sizeof(data))
        return false;
    out = data;
    return true;
}

} // namespace

ColorScheme detectColorScheme() {
    // AppsUseLightTheme: 0 = dark, 1 = light. Written by Settings > Personalisation > Colours
    // ("Choose your mode"). SystemUsesLightTheme sits beside it and is the *shell* chrome's colour
    // -- taskbar and Start -- which the user can legitimately set the other way round; an
    // application follows the APPS value, so that is the one read here.
    //
    // Absent means the value has never been written on this profile, which is NOT the same fact as
    // "light": NoPreference lets Mosaic's own default stand, exactly as it does on a portal-less
    // Linux box.
    DWORD light = 0;
    if (!readCurrentUserDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                              L"AppsUseLightTheme", light)) {
        return ColorScheme::NoPreference;
    }
    return light == 0 ? ColorScheme::Dark : ColorScheme::Light;
}

std::optional<common::Color8> detectAccentColor() {
    // ⚠ This is DWM's COLORIZATION colour -- the tint the compositor puts on window borders, the
    // taskbar and the title bars -- not a separate "accent" of the kind NSColor.controlAccentColor
    // or the XDG portal's accent-color is. Windows has no other user-chosen accent to read: the
    // Settings "Accent colour" picker writes exactly this value, and the AccentPalette shades
    // beside it are derived from it.
    DWORD argb = 0;
    if (!readCurrentUserDword(L"Software\\Microsoft\\Windows\\DWM", L"ColorizationColor", argb))
        return std::nullopt;

    // ⚠ The channel shuffle is deliberate, not a byte-order bug: DWM stores this as 0xAARRGGBB
    // while common::Color8 is {r, g, b, a}.
    //
    // The stored alpha is dropped on purpose. It is DWM's blend STRENGTH against the blurred
    // backdrop, not the opacity of the accent as a UI colour, and a translucent accent would make
    // every accented chip and focus ring Mosaic draws see-through. Both other backends return an
    // opaque colour here; so does this one.
    return common::Color8{static_cast<std::uint8_t>((argb >> 16) & 0xFFu),
                          static_cast<std::uint8_t>((argb >> 8) & 0xFFu),
                          static_cast<std::uint8_t>(argb & 0xFFu), 255};
}

} // namespace mosaic::platform
