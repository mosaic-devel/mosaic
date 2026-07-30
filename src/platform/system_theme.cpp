#include "platform/system_theme.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace mosaic::platform {
namespace {

// Run a shell command and capture its stdout. Best-effort: returns "" on any failure. The
// commands are fixed string literals (no user input), so there is no injection surface; each
// is wrapped in `timeout` so a stuck portal/D-Bus call can never hang startup.
std::string runCapture(const char* cmd) {
    std::string out;
    FILE* pipe = popen(cmd, "r");
    if (!pipe)
        return out;
    std::array<char, 256> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        out += buf.data();
    }
    pclose(pipe);
    return out;
}

std::uint8_t toByte(double unit) {
    if (unit < 0.0)
        unit = 0.0;
    if (unit > 1.0)
        unit = 1.0;
    return static_cast<std::uint8_t>(std::lround(unit * 255.0));
}

} // namespace

ColorScheme detectColorScheme() {
    // 1) XDG desktop portal (cross-desktop standard). color-scheme is a uint32:
    //    0 = no preference, 1 = prefer dark, 2 = prefer light.
    const std::string portal = runCapture("timeout 2 gdbus call --session "
                                          "--dest org.freedesktop.portal.Desktop "
                                          "--object-path /org/freedesktop/portal/desktop "
                                          "--method org.freedesktop.portal.Settings.Read "
                                          "org.freedesktop.appearance color-scheme 2>/dev/null");
    if (portal.find("uint32 1") != std::string::npos)
        return ColorScheme::Dark;
    if (portal.find("uint32 2") != std::string::npos)
        return ColorScheme::Light;

    // 2) GNOME gsettings fallback.
    const std::string gs =
        runCapture("timeout 2 gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null");
    if (gs.find("prefer-dark") != std::string::npos)
        return ColorScheme::Dark;
    if (gs.find("prefer-light") != std::string::npos || gs.find("default") != std::string::npos) {
        return ColorScheme::Light;
    }

    return ColorScheme::NoPreference;
}

std::optional<common::Color8> detectAccentColor() {
    // XDG portal accent-color: a (ddd) tuple of doubles in [0,1], or all-negative if unset.
    // Output looks like: (<(0.211765, 0.517647, 0.894118)>,)
    const std::string s = runCapture("timeout 2 gdbus call --session "
                                     "--dest org.freedesktop.portal.Desktop "
                                     "--object-path /org/freedesktop/portal/desktop "
                                     "--method org.freedesktop.portal.Settings.Read "
                                     "org.freedesktop.appearance accent-color 2>/dev/null");

    const std::size_t lt = s.find('<');
    const std::size_t lp = (lt == std::string::npos) ? std::string::npos : s.find('(', lt);
    if (lp == std::string::npos)
        return std::nullopt;

    double r = -1.0, g = -1.0, b = -1.0;
    if (std::sscanf(s.c_str() + lp, "(%lf, %lf, %lf)", &r, &g, &b) != 3)
        return std::nullopt;
    if (r < 0.0 || g < 0.0 || b < 0.0)
        return std::nullopt; // sentinel: no accent set
    return common::Color8{toByte(r), toByte(g), toByte(b), 255};
}

} // namespace mosaic::platform
