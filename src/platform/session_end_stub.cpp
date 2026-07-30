#include "platform/session_end.hpp"

// The no-op session-end guard, compiled INSTEAD of session_end_win32.cpp on Linux and macOS (see
// platform/CMakeLists.txt) -- the same substitution native_window.cpp / file_dialog.cpp make in the
// other direction. The symbols exist here so runApp's one call site needs no platform conditional,
// which is the same argument preferWaylandBackendIfUnset's empty Windows body is written down for:
// whether the host asks an application before it ends the session is a fact about the host, and
// deciding it belongs on this side of the boundary rather than in an #ifdef in the UI.
//
// WHY THERE IS NOTHING TO DO YET, per platform -- so that a later session finds the survey already
// done rather than re-doing it:
//
//   * Wayland has no session-end protocol at all. A compositor tearing the session down closes the
//     wl_display; there is no query, no veto, and nothing an application can answer. `xdg-session-
//     management` (still staging, and unimplemented by the compositors Mosaic runs on) is about
//     RESTORING a toplevel's placement, not about consent to end.
//   * X11 has one, and it is XSMP (libSM / ICE, an X11R6 protocol): a session manager sends
//     SaveYourself, the client answers with an interaction request and can refuse. Modern desktops
//     have largely stopped running an XSMP manager, so a correct implementation would frequently
//     do nothing -- and it needs libSM/libICE linked and an ICE connection watched on the FLTK
//     loop. Not worth it before somebody reports losing work to a Linux log-off.
//   * macOS has the good one: -[NSApplicationDelegate applicationShouldTerminate:] can answer
//     NSTerminateCancel for a log-out or restart, which is precisely this feature. It is a real
//     follow-up rather than an impossibility, and it would live in a session_end_macos.mm sibling
//     next to native_window_macos.mm -- but it also has to be reconciled with FLTK's own
//     application delegate, which already claims that selector.
//
// Until one of those is built, the recovery journal alone carries a Linux or macOS session end: the
// work is recoverable on the next launch, the user is simply never asked first. That is exactly the
// state Windows was in before this module existed.

namespace mosaic::platform {

// Unnamed parameters on purpose: -Wunused-parameter is part of the project's -Werror bar, and
// naming them only to ignore them would need a cast-to-void per line that says less than the shape
// already does.
void installSessionEndGuard(Fl_Window*, SessionEndQuery, SessionEndNotify, const std::string&) {}

void removeSessionEndGuard(Fl_Window*) {}

} // namespace mosaic::platform
