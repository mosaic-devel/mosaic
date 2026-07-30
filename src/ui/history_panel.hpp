#pragma once

#include <FL/Fl_Group.H>
#include <FL/Fl_Widget.H>

#include "core/command.hpp" // core::Command::Clock (the per-entry timestamp)

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace mosaic::core {
class Document;
}

namespace mosaic::ui {
class ScrollView;
}

// The right dock's History tab (PLAN S16-b): the document's command stack as a chronological
// list — a baseline "Original" row, then one row per entry (oldest at the top), with the
// current stack position highlighted and the not-redone tail muted. Clicking a row JUMPS
// there through plain multi-undo/redo (core::CommandStack::jumpTo — no new command types, no
// snapshots, no branching). Deliberately a dumb VIEW: the pure list/jump model lives on
// CommandStack itself (size/position/nameAt/jumpTo, unit-tested FLTK-free); refreshes arrive
// via the stack's onChange observer, which MainWindow wires per document — edits push from
// the menu, the layer panel AND the canvas tools, so no single UI choke point sees them all.
namespace mosaic::ui {

class HistoryPanel;

// One history entry: its label, the current-position highlight, the muted not-redone style.
// State (position, muting) is read back from the stack each draw, so a jump needs no row
// rebuild — just a redraw.
class HistoryRow : public Fl_Widget {
public:
    HistoryRow(int X, int Y, int W, int H, HistoryPanel* panel, std::size_t position,
               std::string label, core::Command::Clock::time_point time = {});

    void setLabel(std::string label);                  // in-place refresh (see HistoryPanel::refresh)
    void setTime(core::Command::Clock::time_point time); // also refreshes the absolute-time tooltip
    // Pixels the vertical scrollbar occupies at the row's right edge, so the age column never hides
    // under it. Pushed by the panel rather than read from `scrollbar.visible()` at draw time: that
    // flag is only correct AFTER Fl_Scroll::draw() has recalculated it, so the first paint after a
    // tab switch used last frame's answer and the age column jumped one frame later (S16-g).
    void setScrollGutter(int px);
    // Recompute the age caption from the clock. draw() never does this itself -- it paints whatever
    // string was last computed here -- so a hover, a scroll or any other incidental repaint cannot
    // silently advance the row's age. Only the panel's re-tick (and setTime) move the clock forward,
    // which is what makes the column change when a minute passes rather than when you mouse over it.
    // Returns true when the caption actually changed (the caller then redraws just that row).
    bool refreshAge();
    [[nodiscard]] core::Command::Clock::time_point timestamp() const noexcept { return m_time; }

protected:
    void draw() override;
    int handle(int event) override;

private:
    void updateTooltip(); // absolute timestamp on hover (empty for the timeless baseline)

    HistoryPanel* m_panel;
    std::size_t m_position; // the stack position a click jumps to (0 = the baseline row)
    std::string m_label;
    core::Command::Clock::time_point m_time{}; // when the entry was recorded (epoch = none)
    bool m_hover = false;
    int m_scrollGutter = 0;
    std::string m_age; // the age caption to paint; only refreshAge() moves it
};

class HistoryPanel : public Fl_Group {
public:
    HistoryPanel(int X, int Y, int W, int H);
    ~HistoryPanel() override; // cancels the age re-tick timeout

    // Called by the dock the moment this tab becomes the visible one, BEFORE the first paint:
    // settles the scrollbar gutter (see HistoryRow::setScrollGutter) and re-ticks the age column,
    // which has been frozen while the tab was hidden.
    void onTabShown();
    void hide() override; // also disarms the age re-tick
    // A resize can bring the vertical scrollbar in or out (the window or the dock got shorter), so
    // the rows' gutter must be recomputed -- Fl_Group::resize alone would leave them insetting by
    // the previous answer and the age column would slide under the bar.
    void resize(int X, int Y, int W, int H) override;

    // Point the panel at the document whose stack to display (non-owning; null clears).
    void setDocument(core::Document* doc);
    // Rebuild the list from the stack. The host routes CommandStack::setOnChange here.
    void refresh();
    // Row click: walk the stack to `position` (one batched jump), then tell the host to
    // re-sync everything else (composite, layer list, status bar) once via onJump.
    void jumpTo(std::size_t position);
    void setOnJump(std::function<void()> cb) { m_onJump = std::move(cb); }
    void reapplyTheme(); // runtime theme change: re-fill the scroll ground; rows draw live

    [[nodiscard]] std::size_t position() const; // current stack position (rows read it)

protected:
    void draw() override;        // the empty-state caption
    int handle(int event) override; // Up/Down step through history (Photoshop-style); take focus

private:
    void rebuildRows(); // full row rebuild (only when the entry COUNT changed -- see refresh)
    void scrollCurrentIntoView();
    void stepHistory(int delta); // jump by `delta` entries, clamped (Up = -1 = undo, Down = +1)
    void layoutRows();          // re-place + re-width the rows after a resize (Fl_Scroll only moves)
    void updateScrollGutter();  // push the pending scrollbar width into every row

    // The age column ("just now" / "45s" / "2m") is computed at draw, so without a nudge it froze
    // until the pointer crossed a row. A repeating timeout redraws the rows -- but only while this
    // tab is actually visible, and only as often as the SOONEST label change needs: once a second
    // while some entry is still in the seconds band, once a minute after that, and never at all
    // once every entry has aged past an hour. That keeps an idle History tab from waking the app.
    void armAgeTick();
    void disarmAgeTick();
    [[nodiscard]] double ageTickDelay() const; // seconds until the next label change (0 = never)
    static void ageTick(void* self);

    core::Document* m_doc = nullptr;
    ScrollView* m_scroll = nullptr;
    std::vector<HistoryRow*> m_rows; // display order == chronological order (baseline first)
    std::function<void()> m_onJump;
    bool m_ageTickArmed = false;
};

} // namespace mosaic::ui
