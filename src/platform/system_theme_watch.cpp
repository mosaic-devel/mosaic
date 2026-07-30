#include "platform/system_theme.hpp"

// Linux backend for watchSystemAppearance(): subscribe to the XDG desktop portal's
// org.freedesktop.portal.Settings.SettingChanged signal and fire the callback whenever the
// appearance namespace's color-scheme / accent-color changes. The bus fd is driven from the FLTK
// event loop (Fl::add_fd), so there is no extra thread. (macOS has its own .mm backend.)

#include <FL/Fl.H>
#include <systemd/sd-bus.h>

#include <functional>
#include <string_view>
#include <utility>

namespace mosaic::platform {
namespace {

struct AppearanceWatch {
    std::function<void()> onChange;
    sd_bus* bus = nullptr;
    sd_bus_slot* slot = nullptr;
};

// SettingChanged(s namespace, s key, v value). We only need namespace + key -- the callback re-reads
// the actual value via detectColorScheme()/detectAccentColor() -- so the variant is left unread.
int onSettingChanged(sd_bus_message* m, void* userdata, sd_bus_error* /*retError*/) {
    auto* w = static_cast<AppearanceWatch*>(userdata);
    const char* ns = nullptr;
    const char* key = nullptr;
    if (sd_bus_message_read(m, "ss", &ns, &key) < 0)
        return 0;
    if (ns != nullptr && key != nullptr && std::string_view(ns) == "org.freedesktop.appearance" &&
        (std::string_view(key) == "color-scheme" || std::string_view(key) == "accent-color")) {
        if (w->onChange)
            w->onChange();
    }
    return 0;
}

// Drain the bus whenever its fd is readable (registered with Fl::add_fd).
void busFdReady(FL_SOCKET /*fd*/, void* data) {
    auto* w = static_cast<AppearanceWatch*>(data);
    while (sd_bus_process(w->bus, nullptr) > 0) {
        // keep dispatching queued signals until the connection is idle
    }
}

} // namespace

void watchSystemAppearance(std::function<void()> onChange) {
    // Leaked on purpose: the subscription lives for the whole process (there is no unsubscribe).
    auto* w = new AppearanceWatch{std::move(onChange), nullptr, nullptr};
    if (sd_bus_open_user(&w->bus) < 0 || w->bus == nullptr)
        return; // no session bus -> no live switching (the startup-time detection still applied)
    if (sd_bus_match_signal(w->bus, &w->slot, "org.freedesktop.portal.Desktop",
                            "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Settings",
                            "SettingChanged", onSettingChanged, w) < 0) {
        sd_bus_unref(w->bus);
        w->bus = nullptr;
        return;
    }
    // Establish the match + drain anything already queued, then drive the fd from the FLTK loop.
    while (sd_bus_process(w->bus, nullptr) > 0) {
    }
    Fl::add_fd(sd_bus_get_fd(w->bus), busFdReady, w);
}

} // namespace mosaic::platform
