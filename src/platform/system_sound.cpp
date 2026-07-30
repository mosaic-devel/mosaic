#include "platform/system_sound.hpp"

#include <atomic>

#ifdef MOSAIC_HAVE_CANBERRA
#include "common/log.hpp" // only the libcanberra backend has anything to report
#include <canberra.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __APPLE__
// NSBeep is plain C in AppKit (APPKIT_EXTERN void NSBeep(void)), so this one needs no Objective-C++
// sibling the way native_window/system_theme do -- declaring it here and linking the framework
// (already done for this module) is the whole macOS backend.
extern "C" void NSBeep(void);
#endif

namespace mosaic::platform {
namespace {

// Off until main() arms it -- see the header. Atomic because the flag is read from whatever thread
// reaches a dialog and written once at startup; relaxed is enough (no ordering is implied, and the
// worst a race can do is play or skip a single sound).
std::atomic<bool> g_enabled{false};

// Latched after a failure that cannot fix itself (no sound server, event sounds disabled, no
// theme). A box that will never make a sound must not pay a failed attempt per dialog.
std::atomic<bool> g_dead{false};

#ifdef MOSAIC_HAVE_CANBERRA
// freedesktop Sound Naming Specification event ids, best match FIRST -- the user hears THEIR
// desktop's alert rather than one of ours.
//
// ⚠ The spec listing a name does not mean a theme ships it. The reference `freedesktop` theme has
// dialog-information / dialog-warning / dialog-error but **no dialog-question**, and an id a theme
// lacks fails with CA_ERROR_NOTFOUND -- silently. Without a fallback that would mute exactly the
// face Mosaic shows most (Save your changes? / Restore? / Flatten History?), which is the opposite
// of what an alert sound is for. So each meaning carries a chain and we play the first id that
// resolves; `bell` is the last resort because every theme has it.
constexpr int kChainLen = 3;
constexpr int kSoundCount = 4;
// The per-meaning memo below indexes by the enum's value, so a new SystemSound must widen it.
static_assert(static_cast<int>(SystemSound::Error) == kSoundCount - 1,
              "SystemSound gained a value: widen kSoundCount (and give it a candidate chain)");

struct SoundIds {
    const char* ids[kChainLen];
};
SoundIds candidatesFor(SystemSound s) {
    switch (s) {
    case SystemSound::Information: return {{"dialog-information", "message", "bell"}};
    case SystemSound::Question: return {{"dialog-question", "dialog-information", "bell"}};
    case SystemSound::Warning: return {{"dialog-warning", "dialog-information", "bell"}};
    case SystemSound::Error: return {{"dialog-error", "dialog-warning", "bell"}};
    }
    return {{"dialog-information", "message", "bell"}};
}

// One process-wide context, created on first use and deliberately never destroyed: a static local's
// destructor would race the sound server at process exit for no benefit, and the OS reclaims it.
ca_context* soundContext() {
    static ca_context* ctx = [] {
        ca_context* c = nullptr;
        if (ca_context_create(&c) != CA_SUCCESS)
            return static_cast<ca_context*>(nullptr);
        // Identify ourselves so the desktop can show + remember a per-app volume, and mark the
        // stream an "event" so it mixes like a notification rather than like media.
        ca_context_change_props(c, CA_PROP_APPLICATION_NAME, "Mosaic", CA_PROP_APPLICATION_ID,
                                "org.mosaic.Mosaic", CA_PROP_MEDIA_ROLE, "event", nullptr);
        return c;
    }();
    return ctx;
}
#endif // MOSAIC_HAVE_CANBERRA

} // namespace

void enableSystemSounds(bool enabled) {
    g_enabled.store(enabled, std::memory_order_relaxed);
}

bool systemSoundsEnabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

void playSystemSound([[maybe_unused]] SystemSound sound) {
    if (!g_enabled.load(std::memory_order_relaxed) || g_dead.load(std::memory_order_relaxed))
        return;

#if defined(MOSAIC_HAVE_CANBERRA)
    ca_context* ctx = soundContext();
    if (ctx == nullptr) {
        g_dead.store(true, std::memory_order_relaxed);
        return;
    }
    // Which candidate won last time for this meaning -- or kChainLen for "this theme has none of
    // them", so a themeless box stops re-probing. The sound theme cannot change under a running
    // process in any way worth tracking, so remembering keeps the steady state at exactly one call;
    // without it a Question would eat a guaranteed CA_ERROR_NOTFOUND before every single dialog.
    static std::atomic<int> resolved[kSoundCount] = {{-1}, {-1}, {-1}, {-1}};
    std::atomic<int>& slot = resolved[static_cast<int>(sound)];
    const SoundIds cand = candidatesFor(sound);
    const int known = slot.load(std::memory_order_relaxed);
    for (int i = known >= 0 ? known : 0; i < kChainLen; ++i) {
        // Asynchronous: returns as soon as the request is queued, so a slow or absent sound server
        // never stalls the UI thread that is about to put a modal up. A missing id, though, fails
        // SYNCHRONOUSLY (CA_ERROR_NOTFOUND) -- which is what makes probing down the chain work.
        const int rc = ca_context_play(ctx, 0, CA_PROP_EVENT_ID, cand.ids[i], nullptr);
        if (rc == CA_SUCCESS) {
            slot.store(i, std::memory_order_relaxed);
            return;
        }
        if (rc == CA_ERROR_DISABLED || rc == CA_ERROR_NOTAVAILABLE || rc == CA_ERROR_NOTSUPPORTED ||
            rc == CA_ERROR_NODRIVER) {
            // The user muted event sounds, or this box has no way to make one. Both are FINE, both
            // are permanent for the session, and neither is about this particular id -- go quiet
            // everywhere. Debug level on purpose: a muted desktop is a normal state, not something
            // the user needs told about on every launch.
            common::log::category("platform")
                ->debug("system sounds unavailable ({}); staying silent", ca_strerror(rc));
            g_dead.store(true, std::memory_order_relaxed);
            return;
        }
        if (known >= 0)
            break; // the remembered id stopped resolving; do not walk the chain again per call
    }
    // Fell off the end of a FIRST probe: nothing in this meaning's chain exists in the user's theme.
    // Remember that too (kChainLen makes the loop above a no-op next time) rather than re-walking
    // three doomed ids per dialog forever. Not g_dead: another meaning may still have a sound.
    if (known < 0)
        slot.store(kChainLen, std::memory_order_relaxed);
#elif defined(__APPLE__)
    // macOS has ONE alert sound -- the one the user picked in Sound settings -- for every alert
    // class; the per-type alert sounds of the classic Mac OS era are long gone from the HIG. So all
    // four meanings correctly land on the same NSBeep, which also honours their mute/volume choice.
    NSBeep();
#elif defined(_WIN32)
    // MessageBeep names a SCHEME EVENT, not a waveform: each icon class selects one of the four
    // entries the user can reassign under Settings > System > Sound > More sound settings > Sounds
    // ("Asterisk", "Question", "Exclamation", "Critical Stop"). So the user hears what they chose
    // there -- including nothing at all, if they set "(None)" or muted the system-sounds channel,
    // which is the header's "the preference lives in the OS" satisfied exactly and with nothing to
    // link. It returns as soon as the sound is queued, so the UI thread never waits on audio.
    //
    // The four PRIMARY macro names on purpose. MB_ICONINFORMATION / MB_ICONWARNING / MB_ICONERROR
    // are aliases of MB_ICONASTERISK / MB_ICONEXCLAMATION / MB_ICONHAND, and it is the latter that
    // name the scheme events -- spelling them this way makes the mapping read the way the Sounds
    // control panel lists them, rather than looking like four unrelated icons.
    //
    // ⚠ Windows is the one platform where FLTK's own beep is not actively wrong (its Windows driver
    // does call MessageBeep, unlike the X11/Wayland paths above) -- but it is right only in
    // MECHANISM. Fl_WinAPI_Screen_Driver::beep switches on the Fl_Beep enum, which has no *warning*
    // class at all: FL_BEEP_MESSAGE and FL_BEEP_NOTIFICATION both land on MB_ICONASTERISK and
    // FL_BEEP_ERROR on MB_ICONERROR, so routing through fl_beep would collapse two of our four
    // meanings into one sound. Calling MessageBeep ourselves costs nothing and keeps the four
    // apart.
    switch (sound) {
    case SystemSound::Information: MessageBeep(MB_ICONASTERISK); break;
    case SystemSound::Question: MessageBeep(MB_ICONQUESTION); break;
    case SystemSound::Warning: MessageBeep(MB_ICONEXCLAMATION); break;
    case SystemSound::Error: MessageBeep(MB_ICONHAND); break;
    }
#endif
    // No backend compiled in (a Linux build without libcanberra): silence, by design. Deliberately
    // NOT a fall back to fl_beep -- FLTK's X11 path is XBell (routed nowhere on a modern desktop)
    // and its Wayland path writes a BEL byte to stderr, which spams the launching terminal instead
    // of making any sound at all.
}

} // namespace mosaic::platform
