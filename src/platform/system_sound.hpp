#pragma once

// The host's own alert sounds -- the noise a native dialog makes when it interrupts you. Used by
// ui::AskOrTellDialog (docs/askortell-dialog.md), which is the app's only message box.
//
// Everything here is BEST-EFFORT and never throws: a desktop with no sound server, a build with no
// libcanberra, a user who muted event sounds -- all of them end in silence, never in an error the
// caller has to handle. That is deliberate: an alert sound is a courtesy, and a failure to play one
// must never change what the program does.
//
// The user's preference lives in the OS, not in Mosaic. Every backend below routes through the
// mechanism the desktop already owns -- the XDG sound theme and its event-sounds switch, the
// Windows sound scheme, the macOS alert setting -- so someone who has silenced UI sounds hears
// nothing without Mosaic ever knowing, and there is no second in-app toggle for the same
// preference (the no-toggle-for-strictly-better rule).
namespace mosaic::platform {

// The four freedesktop alert events, which are also exactly the four Windows MessageBeep icons.
// A caller picks by MEANING, not by sound: the mapping to any given desktop's actual audio is the
// backend's business.
enum class SystemSound {
    Information, // "here is a thing" -- a result, a completion
    Question,    // a choice is being asked for
    Warning,     // something is off, but nothing is broken
    Error,       // something IS broken
};

// Play `sound`, or do nothing. Returns immediately -- playback is asynchronous on every backend,
// so this never blocks the UI thread even if the sound server is slow to answer.
//
// A no-op until enableSystemSounds(true) has been called. See below for why.
void playSystemSound(SystemSound sound);

// Arm (or silence) the whole subsystem. Sounds are OFF until the application turns them on, and
// `main()` is the only caller -- so linking mosaic::platform into a test binary, a headless tool
// or the thumbnailer can never make the machine beep. Without this, the AskOrTell dialog's own
// unit tests (which present() thirteen faces) would play thirteen alerts through the developer's
// speakers on every `ctest` run.
void enableSystemSounds(bool enabled);

// Whether sounds are currently armed. Exists so that "a test run is silent" is an invariant a TEST
// can assert, rather than a convention that holds only while nobody wires enableSystemSounds() into
// library code by accident.
[[nodiscard]] bool systemSoundsEnabled();

} // namespace mosaic::platform
