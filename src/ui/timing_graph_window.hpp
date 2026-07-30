#pragma once

#include <FL/Fl_Double_Window.H>

namespace mosaic::ui {

// A non-modal diagnostic window (Help -> Timing Profiler) that shows the live per-operation table
// from the process-wide common::Profiler (common/profiler.hpp -- the collector lives in the
// FLTK-free base since S60-a; this window is the UI half): for each named render/composite op its
// lane (CPU/GPU) and last/avg/min/max/count, SORTED SLOWEST-FIRST so the operations that take a
// long time to render stay at the top, with a decaying highlight that keeps a recently-slow op
// glowing for a few seconds so a one-off spike is catchable. A header line shows the live FPS +
// frame time. It never blocks -- a plain top-level window the MainWindow redraws each frame while
// shown, reopening the SAME window on a repeat menu click. Closing it (WM button) hides, not
// destroys, so the MainWindow can reveal it again and close it on quit.
//
// REACHABILITY (S60-alpha): compiled into every build, but in a RELEASE build it is only reachable
// when profiling is switched on (--profile / MOSAIC_PROFILE=1), where it opens at startup and the
// Help menu gains its item. Without the flag a release build offers no way in. Debug builds keep
// the menu item unconditionally, as before. The collector now ships either way, so the flag that
// turns it on had better come with the surface that makes it worth something.
class TimingGraphWindow : public Fl_Double_Window {
public:
    TimingGraphWindow();

protected:
    void draw() override;
};

} // namespace mosaic::ui
