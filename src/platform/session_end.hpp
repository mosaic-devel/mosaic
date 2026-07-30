#pragma once

#include <functional>
#include <string>

class Fl_Window; // forward-declared: this header stays free of FLTK and of <windows.h>.

// platform/session_end -- let the OS ask before it ends the session under us.
//
// The problem this exists for: a Windows shutdown/restart/log-off KILLS the process. FLTK 1.4.5
// handles neither WM_QUERYENDSESSION nor WM_ENDSESSION, so DefWindowProc grants permission and the
// user is never asked -- with unsaved work on screen. (The recovery journal means the work is
// RECOVERABLE on the next launch, which is why this is a courtesy bug rather than a data-loss bug,
// but nobody should have to discover that by accident.)
//
// ⚠ DELIBERATELY NOT A DIALOG. Windows allows an application roughly five seconds to answer
// WM_QUERYENDSESSION and force-closes one that takes longer; Microsoft explicitly discourages
// putting a modal window up inside that budget, and ours (ui::AskOrTellDialog) runs its own nested
// FLTK loop, so it would hang out the whole shutdown and then be killed mid-question. The supported
// mechanism is to answer "no" and hand Windows a REASON -- it then shows its own
// "these apps are preventing you from shutting down" screen, names the reason, and lets the user
// choose. The decision stays with the user; it just stops being made silently.
namespace mosaic::platform {

// Is there work that would be lost? Called on the UI thread from inside the OS's query, so it must
// be CHEAP and must not pump events.
//
// ⚠ POLARITY, because it is the opposite of the value the OS wants: **true means "there IS unsaved
// work", i.e. BLOCK the session end**. Returning false lets the session end.
using SessionEndQuery = std::function<bool()>;

// The session IS ending and this is the last call before the process is terminated. Last chance to
// make the work recoverable -- and nothing more. Must be fast (the same few-second budget applies)
// and must not show UI, pump events, or block on a background job.
//
// ⚠ HARD PROJECT RULE: this must NEVER write the user's document. A session end is not a Save. The
// recovery journal is the whole mechanism (io::native::JournalSession, over the app's own
// "recovery" directory under %LOCALAPPDATA%); syncing it is the right action, and discarding it
// would be the wrong one -- an OS-terminated process is an unclean exit, and the journal is exactly
// what the next launch offers to restore.
using SessionEndNotify = std::function<void()>;

// Install the hooks on `win`, which must already be SHOWN (the native window has to exist). Safe
// to call more than once: a second call on the same window replaces the callbacks and the reason
// rather than stacking a second hook.
//
// `reason` is a short, ALREADY-TRANSLATED sentence -- one line, the OS truncates past 256
// characters -- shown by Windows on its blocking screen. It arrives translated rather than being
// looked up here because a user-facing sentence is the UI layer's to own and to compose (the same
// split as ui::UnsavedTitleFormat and the macOS application-menu strings): keeping the _() next to
// the rest of the window's copy is what keeps the msgid where a translator can find it, and it
// keeps this module free of a dependency on i18n for one string.
//
// A NO-OP off Windows, where the symbol still exists so the one call site needs no platform
// conditional -- whether the host asks before ending a session is a fact about the host, and it
// belongs on this side of the boundary (the same reasoning as preferWaylandBackendIfUnset).
void installSessionEndGuard(Fl_Window* win, SessionEndQuery hasUnsavedWork,
                            SessionEndNotify onSessionEnd, const std::string& reason);

// Drop the hooks again and stop blocking. Not required for teardown -- the guard unhooks itself
// when the native window is destroyed -- but a window that wants to stop answering before then (or
// that is about to be reshown under a new native handle) can say so explicitly. A no-op for a
// window that was never guarded, and off Windows.
void removeSessionEndGuard(Fl_Window* win);

} // namespace mosaic::platform
