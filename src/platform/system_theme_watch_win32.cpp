#include "platform/system_theme.hpp"

// Windows backend for watchSystemAppearance(): watch the registry keys system_theme_win32.cpp reads
// and fire the callback when either changes, so a "System"-mode theme re-resolves itself live.
// (Linux has an sd-bus/portal backend, macOS an NSNotification one.)
//
// The mechanism is RegNotifyChangeKeyValue with an event handle. Two of its properties shape this
// whole file:
//   * REG_NOTIFY_THREAD_AGNOSTIC is required, not an optimisation. Without it the registration is
//     owned by the THREAD that made it and is torn down when that thread exits, and the per-thread
//     bookkeeping limits how many can be outstanding. With it the registration belongs to the
//     PROCESS, which is what "permanent for the process" in the header actually needs. It has
//     existed since Windows 8 -- well under Mosaic's Windows 10 1809 floor.
//   * a registry notification is ONE-SHOT. The event is signalled once and the registration is then
//     spent, so every fire has to re-arm. That is why arm() is called again from the poll and not
//     only at setup.
//
// ⚠ Fl::add_fd cannot be handed a Win32 event HANDLE. On FLTK's Windows backend add_fd is a SOCKET
// facility (it goes through WSAAsyncSelect); an event handle is not a socket, so registering one
// would compile and then simply never fire. The event is therefore POLLED from a re-armed
// Fl::add_timeout with a zero-timeout WaitForSingleObject -- the same "poll from the UI thread,
// never call into FLTK from another thread" discipline the rest of the app already uses for its
// background work (app_window's inpaint / async-save / spell-check poll timers). A thread that
// blocked on the event and then invoked the callback is the one shape not allowed here: onChange
// re-themes live widgets.

#include <FL/Fl.H>

#include <array>
#include <functional>
#include <utility>
#include <windows.h>

namespace mosaic::platform {
namespace {

// How often the UI thread asks whether a notification landed. One second is chosen for what the
// user perceives rather than for the machine: flipping Windows into dark mode repaints the shell
// over roughly that long anyway, so a Mosaic that follows within a second reads as "at the same
// time", while going faster buys nothing anyone can see. The cost is one WaitForSingleObject with a
// zero timeout per key per second -- a few hundred nanoseconds -- and it wakes nothing that was
// asleep, because FLTK is already running a timeout-driven loop for the canvas frame clock.
constexpr double kPollSeconds = 1.0;

// One watched registry key and the auto-reset event its change notification signals.
struct WatchedKey {
    HKEY key = nullptr;
    HANDLE event = nullptr;

    [[nodiscard]] bool live() const { return key != nullptr && event != nullptr; }

    // (Re-)arm the one-shot notification. False means this registration is finished for good, and
    // the caller retires the slot rather than polling a handle nothing will signal again.
    [[nodiscard]] bool arm() const {
        return live() && RegNotifyChangeKeyValue(
                             key, /*bWatchSubtree=*/FALSE,
                             REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC, event,
                             /*fAsynchronous=*/TRUE) == ERROR_SUCCESS;
    }

    void close() {
        if (event != nullptr)
            CloseHandle(event);
        if (key != nullptr)
            RegCloseKey(key);
        event = nullptr;
        key = nullptr;
    }
};

struct AppearanceWatch {
    std::function<void()> onChange;
    // [0] the light/dark preference (what detectColorScheme reads), [1] DWM's colorization colour
    // (what detectAccentColor reads). TWO keys because the header promises the callback fires for
    // either, and Windows keeps the two facts in unrelated places -- unlike the XDG portal, where
    // they are two keys of one namespace and a single signal covers both.
    std::array<WatchedKey, 2> keys{};
};

// Open a key for notification only and wire it to a fresh auto-reset event. KEY_NOTIFY is the only
// access right needed: the VALUE is re-read through detectColorScheme()/detectAccentColor() when
// the callback runs, exactly as the Linux backend re-reads the portal rather than trusting the
// signal's payload. Any failure leaves the slot dead, which every reader below reads as unwatched.
void openWatched(WatchedKey& w, const wchar_t* subKey) {
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_NOTIFY, &w.key) != ERROR_SUCCESS) {
        w.key = nullptr; // RegOpenKeyEx leaves the out-parameter unspecified on failure
        return;
    }
    // Auto-reset (bManualReset FALSE): a satisfied wait consumes the signal, which is what makes
    // "one satisfied wait == one change" hold without an explicit ResetEvent anywhere.
    w.event = CreateEventW(nullptr, /*bManualReset=*/FALSE, /*bInitialState=*/FALSE, nullptr);
    if (!w.arm())
        w.close();
}

void pollTimer(void* data) {
    auto* w = static_cast<AppearanceWatch*>(data);
    bool changed = false;
    bool anyLive = false;
    for (WatchedKey& k : w->keys) {
        if (!k.live())
            continue;
        if (WaitForSingleObject(k.event, 0) == WAIT_OBJECT_0) {
            changed = true;
            // Re-arm BEFORE the callback runs. Applying a theme re-resolves every live widget's
            // colours and is not instant, so a second flip landing during it must not be lost; the
            // wait above has already cleared the event, so the new registration starts clean.
            if (!k.arm())
                k.close();
        }
        anyLive = anyLive || k.live();
    }
    // ONE callback per tick even when both keys fired: switching Windows into dark mode moves the
    // colour scheme and the colorization colour together, and the theme is re-resolved wholesale
    // either way, so firing twice would only pay for the same work again.
    if (changed && w->onChange)
        w->onChange();
    if (!anyLive)
        return; // nothing left to watch -- stop rather than spin on dead handles for the session
    // Always from NOW, never Fl::repeat_timeout, which schedules relative to when the tick SHOULD
    // have fired and would then fire back to back to "catch up" after a long theme apply (the same
    // trap documented on the canvas frame loop in app_window).
    Fl::add_timeout(kPollSeconds, pollTimer, w);
}

} // namespace

void watchSystemAppearance(std::function<void()> onChange) {
    // Leaked on purpose: the subscription lives for the whole process (there is no unsubscribe),
    // and the key + event handles have to outlive every poll tick. Same shape as the Linux backend.
    auto* w = new AppearanceWatch{std::move(onChange), {}};
    openWatched(w->keys[0], L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize");
    openWatched(w->keys[1], L"Software\\Microsoft\\Windows\\DWM");
    if (!w->keys[0].live() && !w->keys[1].live())
        return; // nothing watchable -> no live switching (the startup-time detection still applied)
    Fl::add_timeout(kPollSeconds, pollTimer, w);
}

} // namespace mosaic::platform
