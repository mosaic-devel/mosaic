#pragma once

// The menu-bar "motivational one-liner" ticker driver (docs/motivational-ticker.md). Owns the
// cadence: when enabled it feeds a random all-caps one-liner (the `motivate` gettext domain) into the
// MenuBar every few minutes. The bar draws it in its own empty right region -- a line slides down into
// the row (MenuBar::showTickerLine), holds ~10 s, then slides up and out, leaving the row empty until
// the next scheduled line. This is the reworked home of what VulkanCanvas::updateMotivation/
// spawnMotivation used to drive for the (deleted) GPU backdrop path -- the content + cadence are kept;
// only WHERE it renders changed. The bar owns the placement + the region-too-short gate (it draws the
// ticker itself, so there is no separate widget to position); this owns only the content + timing.
namespace mosaic::ui {

class MenuBar;

class MotivationTicker {
public:
    explicit MotivationTicker(MenuBar* menu);
    ~MotivationTicker();

    MotivationTicker(const MotivationTicker&) = delete;
    MotivationTicker& operator=(const MotivationTicker&) = delete;

    // The Settings -> Annoyances toggle. On: schedule the first line soon (so the effect is
    // discoverable), then settle into the rare 2-5 min cadence. Off: cancel the timer and blank the
    // bar (the line is dropped so a re-enable starts fresh).
    void setEnabled(bool on);

private:
    void schedule(double delaySeconds); // arm the one-shot cadence timer
    void spawn();                        // pick a line + feed the bar
    static void onTimeout(void* self);

    MenuBar* m_menu;   // not owned (the main window's menu bar)
    bool m_enabled = false;
    bool m_timerArmed = false;
};

} // namespace mosaic::ui
