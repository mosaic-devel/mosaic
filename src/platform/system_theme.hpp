#pragma once

#include "common/image.hpp"

#include <functional>
#include <optional>

// Best-effort host appearance detection (light/dark preference + accent color). Used by the
// theming engine's "System" mode (ui/theme). Everything here is best-effort and never throws:
// if the desktop exposes nothing, callers fall back to Mosaic's built-in defaults.
namespace mosaic::platform {

enum class ColorScheme {
    NoPreference,
    Dark,
    Light,
};

// The host light/dark preference. Queried from the cross-desktop XDG settings portal
// (org.freedesktop.appearance/color-scheme), falling back to GNOME gsettings. Returns
// NoPreference if neither is available.
[[nodiscard]] ColorScheme detectColorScheme();

// The host accent color, if exposed (XDG portal org.freedesktop.appearance/accent-color).
// std::nullopt when unknown.
[[nodiscard]] std::optional<common::Color8> detectAccentColor();

// Watch for host appearance changes (light/dark preference or accent color). `onChange` is invoked
// on the FLTK/UI thread whenever the OS appearance changes, so a "System"-mode theme can re-resolve
// itself live (Settings never changed -- only the OS did). Best-effort and permanent for the
// process: there is no unsubscribe, and it is a no-op where the platform exposes no change signal.
// Call once, AFTER the FLTK event loop is up (the Linux backend registers a bus fd with Fl::add_fd).
void watchSystemAppearance(std::function<void()> onChange);

} // namespace mosaic::platform
