#include "ui/history_panel.hpp"

#include "common/i18n.hpp"
#include "core/command.hpp"
#include "core/document.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp" // ScrollView (themed scrollbar)

#include <FL/Fl.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <string>
#include <utility>

namespace mosaic::ui {
namespace {

constexpr int kRowH = 24;

// A command's display name, translated. The names are literals in core (core/commands.hpp and the
// inpaint backends), where they are marked with N_() -- extractable by xgettext, but deliberately
// NOT translated at the point of definition: core is FLTK-free and locale-free, and a command
// object outlives any one language. So the lookup happens HERE, the moment a row is built, which
// is also the only place the string is ever seen.
//
// A name assembled at run time (one carrying a layer's own name, say) has no msgid to find, so
// gettext hands it straight back -- correct, and the reason this needs no special case.
std::string translatedCommandName(std::string_view name) {
    if (name.empty())
        return {};
    return std::string(_(std::string(name).c_str()));
}

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

bool hasTime(core::Command::Clock::time_point t) {
    return t.time_since_epoch().count() != 0;
}

// Format `t` in local time with a strftime pattern (UI thread only; the reentrant localtime for
// safety -- which is spelled localtime_s on the MSVC-flavoured CRT mingw targets, with the
// arguments the other way round. Same spelling as about_dialog.cpp and app_window.cpp).
std::string formatTime(core::Command::Clock::time_point t, const char* fmt) {
    const std::time_t tt = core::Command::Clock::to_time_t(t);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[64];
    return std::strftime(buf, sizeof(buf), fmt, &tm) ? std::string(buf) : std::string();
}

// The muted right-hand column: a compact age ("just now" / "45s" / "2m" / "3h") that flips to an
// absolute date once the entry is older than a day (the user's "threshold"). "" for no timestamp.
std::string relativeTime(core::Command::Clock::time_point t) {
    if (!hasTime(t))
        return {};
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                          core::Command::Clock::now() - t)
                          .count();
    if (secs < 0)
        return {}; // clock skew: say nothing rather than "-2s"
    if (secs < 10)
        return _("just now");
    if (secs < 60)
        return std::to_string(secs) + "s";
    if (secs < 3600)
        return std::to_string(secs / 60) + "m";
    if (secs < 86400)
        return std::to_string(secs / 3600) + "h";
    return formatTime(t, "%b %e"); // older than a day: the date instead
}

} // namespace

// ---- HistoryRow ------------------------------------------------------------------------------

HistoryRow::HistoryRow(int X, int Y, int W, int H, HistoryPanel* panel, std::size_t position,
                       std::string label, core::Command::Clock::time_point time)
    : Fl_Widget(X, Y, W, H), m_panel(panel), m_position(position), m_label(std::move(label)),
      m_time(time) {
    updateTooltip();
    m_age = relativeTime(m_time);
}

void HistoryRow::setLabel(std::string label) {
    if (label == m_label)
        return;
    m_label = std::move(label);
    redraw();
}

void HistoryRow::setTime(core::Command::Clock::time_point time) {
    if (time == m_time)
        return;
    m_time = time;
    updateTooltip();
    m_age = relativeTime(m_time);
    redraw();
}

void HistoryRow::updateTooltip() {
    if (hasTime(m_time))
        copy_tooltip(formatTime(m_time, "%Y-%m-%d %H:%M:%S").c_str()); // seconds: adjacent edits
                                                                       // otherwise read identical
    else
        tooltip(nullptr);
}

void HistoryRow::setScrollGutter(int px) {
    if (px == m_scrollGutter)
        return;
    m_scrollGutter = px;
    redraw();
}

bool HistoryRow::refreshAge() {
    std::string next = relativeTime(m_time);
    if (next == m_age)
        return false;
    m_age = std::move(next);
    return true;
}

void HistoryRow::draw() {
    const Palette& pal = activePalette();
    const std::size_t current = m_panel->position();
    const bool isCurrent = m_position == current;
    const bool isTail = m_position > current; // undone, not redone: shown muted

    const Fl_Color bg = isCurrent ? toFl(pal.controlActive)
                                  : (m_hover ? toFl(pal.controlHover) : toFl(pal.panelBg));
    fl_color(bg);
    fl_rectf(x(), y(), w(), h());
    if (isCurrent) { // the position marker: an accent bar on the row's left edge
        fl_color(toFl(pal.accent));
        fl_rectf(x(), y(), 3, h());
    }

    // Keep the age column clear of the scrollbar when it appears (rows span the full scroll width,
    // so a right-aligned tip would otherwise hide under the vertical scrollbar -- user report). The
    // width comes from the panel, not from scrollbar.visible(); see setScrollGutter.
    const int rightInset = 8 + m_scrollGutter;

    // The age column, muted and right-aligned; the label gives way to it so they never overlap.
    // m_age is NOT recomputed here -- see refreshAge(). A hover must not age the row.
    int labelW = w() - 12 - rightInset;
    if (!m_age.empty()) {
        fl_font(FL_HELVETICA, 11);
        const int tw = static_cast<int>(fl_width(m_age.c_str())) + 2;
        fl_color(toFl(pal.textMuted));
        fl_draw(m_age.c_str(), x() + w() - tw - rightInset, y(), tw, h(),
                FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        labelW = w() - tw - rightInset - 14;
    }

    fl_color(toFl(isTail ? pal.textMuted : pal.text));
    fl_font(m_position == 0 ? FL_HELVETICA_ITALIC : FL_HELVETICA, 12);
    fl_draw(m_label.c_str(), x() + 12, y(), std::max(labelW, 8), h(),
            FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    fl_color(toFl(pal.border));
    fl_line(x(), y() + h() - 1, x() + w(), y() + h() - 1); // row separator
}

int HistoryRow::handle(int event) {
    switch (event) {
    case FL_ENTER:
        m_hover = true;
        redraw();
        return 1;
    case FL_LEAVE:
        m_hover = false;
        redraw();
        return 1;
    case FL_PUSH:
        if (Fl::event_button() == FL_LEFT_MOUSE) {
            m_panel->jumpTo(m_position);
            return 1;
        }
        return 0;
    default:
        return Fl_Widget::handle(event);
    }
}

// ---- HistoryPanel ----------------------------------------------------------------------------

HistoryPanel::HistoryPanel(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    box(FL_NO_BOX); // the dock panel behind us owns the fill + border
    const Palette& pal = activePalette();
    begin();
    m_scroll = new ScrollView(X, Y, W, H);
    m_scroll->type(Fl_Scroll::VERTICAL);
    m_scroll->box(FL_NO_BOX);
    m_scroll->color(toFl(pal.panelBg));
    m_scroll->end();
    end();
    resizable(m_scroll);
}

HistoryPanel::~HistoryPanel() { Fl::remove_timeout(ageTick, this); }

void HistoryPanel::onTabShown() {
    // Both halves of the "labels jump on tab entry" fix: settle the gutter and the row widths BEFORE
    // the first paint, and re-tick the ages, which stopped moving the moment this tab was hidden.
    layoutRows();
    updateScrollGutter();
    scrollCurrentIntoView();
    for (HistoryRow* row : m_rows)
        row->refreshAge();
    m_scroll->redraw();
    armAgeTick();
}

void HistoryPanel::hide() {
    disarmAgeTick(); // a hidden tab must never wake the app to re-tick invisible rows
    Fl_Group::hide();
}

void HistoryPanel::resize(int X, int Y, int W, int H) {
    Fl_Group::resize(X, Y, W, H); // resizable(m_scroll): the list absorbs the delta
    layoutRows();                 // ... but Fl_Scroll only MOVES its children; it never re-widths them
    updateScrollGutter();         // a shorter viewport may have just summoned the scrollbar
}

// Rows span the scroll's width, and their y positions are stacked from its top. Fl_Scroll::resize
// translates its children but leaves their widths alone, so a dock-width drag would otherwise leave
// every row at the width it was built with -- the age column stranded mid-row (user, 2026-07-09).
void HistoryPanel::layoutRows() {
    if (m_scroll == nullptr)
        return;
    int ry = m_scroll->y() - m_scroll->yposition();
    for (HistoryRow* row : m_rows) {
        row->resize(m_scroll->x(), ry, m_scroll->w(), kRowH);
        ry += kRowH;
    }
}

void HistoryPanel::updateScrollGutter() {
    if (m_scroll == nullptr)
        return;
    const int gutter = m_scroll->scrollbarGutter(static_cast<int>(m_rows.size()) * kRowH);
    for (HistoryRow* row : m_rows)
        row->setScrollGutter(gutter);
}

void HistoryPanel::armAgeTick() {
    disarmAgeTick();
    if (!visible_r())
        return;
    const double delay = ageTickDelay();
    if (delay <= 0.0)
        return; // every entry is older than an hour: the column will not change on our watch
    Fl::add_timeout(delay, ageTick, this);
    m_ageTickArmed = true;
}

void HistoryPanel::disarmAgeTick() {
    if (!m_ageTickArmed)
        return;
    Fl::remove_timeout(ageTick, this);
    m_ageTickArmed = false;
}

double HistoryPanel::ageTickDelay() const {
    // Seconds until the SOONEST row's caption would read differently. Mirrors relativeTime()'s
    // bands: "just now" below 10s, whole seconds below a minute, whole minutes below an hour, and
    // whole hours below a day -- past which the caption is an absolute date and never moves again.
    double soonest = 0.0;
    const auto now = core::Command::Clock::now();
    for (const HistoryRow* row : m_rows) {
        const core::Command::Clock::time_point t = row->timestamp();
        if (!hasTime(t))
            continue;
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now - t).count();
        if (secs < 0)
            continue;
        double next = 0.0;
        if (secs < 10)
            next = static_cast<double>(10 - secs); // "just now" -> "10s"
        else if (secs < 60)
            next = 1.0;
        else if (secs < 3600)
            next = static_cast<double>(60 - (secs % 60));
        else if (secs < 86400)
            next = static_cast<double>(3600 - (secs % 3600));
        else
            continue; // an absolute date: frozen
        if (soonest == 0.0 || next < soonest)
            soonest = next;
    }
    return soonest > 0.0 ? std::max(soonest, 0.25) : 0.0; // never busier than 4 Hz, whatever happens
}

void HistoryPanel::ageTick(void* self) {
    auto* panel = static_cast<HistoryPanel*>(self);
    panel->m_ageTickArmed = false;
    if (!panel->visible_r())
        return; // tab switched away between arming and firing: let the tick die
    for (HistoryRow* row : panel->m_rows)
        if (row->refreshAge())
            row->redraw(); // only the rows whose caption actually moved
    panel->armAgeTick();
}

void HistoryPanel::reapplyTheme() {
    if (m_scroll != nullptr)
        m_scroll->color(toFl(activePalette().panelBg));
    redraw();
}

std::size_t HistoryPanel::position() const {
    return m_doc != nullptr ? m_doc->commands().position() : 0;
}

void HistoryPanel::setDocument(core::Document* doc) {
    m_doc = doc;
    refresh();
}

void HistoryPanel::refresh() {
    const std::size_t want = m_doc != nullptr ? m_doc->commands().size() + 1 : 0;
    if (want != m_rows.size()) {
        // The entry count changed (push/clear/new document) -- rebuild. Never reached from
        // inside a row's own handle(): a click-jump moves the position, never the count.
        rebuildRows();
    } else if (m_doc != nullptr) {
        // Same count: update IN PLACE and never delete widgets -- a click-jump refreshes from
        // inside the clicked row's handle(), so destroying rows here would free the very
        // widget the event is still running in. Labels can still change (a push that merely
        // replaced the redo tail keeps the count), so re-sync them too.
        for (std::size_t i = 1; i < m_rows.size(); ++i) {
            m_rows[i]->setLabel(translatedCommandName(m_doc->commands().nameAt(i - 1)));
            m_rows[i]->setTime(m_doc->commands().timeAt(i - 1));
        }
        scrollCurrentIntoView();
        m_scroll->redraw(); // the position highlight + muting are read back per draw
    }
    armAgeTick(); // a fresh entry restarts the seconds-band cadence (no-op while hidden)
    redraw();
}

void HistoryPanel::rebuildRows() {
    m_rows.clear();
    m_scroll->scroll_to(0, 0);
    m_scroll->clear(); // delete the row children (keeps the scrollbars)
    if (m_doc != nullptr) {
        const core::CommandStack& stack = m_doc->commands();
        m_scroll->begin();
        int ry = m_scroll->y();
        // Position 0 = the document as loaded/created; clicking it undoes everything.
        m_rows.push_back(
            new HistoryRow(m_scroll->x(), ry, m_scroll->w(), kRowH, this, 0, _("Original")));
        ry += kRowH;
        for (std::size_t i = 0; i < stack.size(); ++i) {
            m_rows.push_back(new HistoryRow(m_scroll->x(), ry, m_scroll->w(), kRowH, this, i + 1,
                                            translatedCommandName(stack.nameAt(i)),
                                            stack.timeAt(i)));
            ry += kRowH;
        }
        m_scroll->end();
        scrollCurrentIntoView();
    }
    updateScrollGutter(); // the row count just changed: so may the scrollbar's presence
    m_scroll->redraw();
}

void HistoryPanel::jumpTo(std::size_t position) {
    if (m_doc == nullptr)
        return;
    if (position == m_doc->commands().position())
        return; // clicking the highlighted row: nothing to walk, no host re-sync
    m_doc->commands().jumpTo(position); // fires the stack observer -> our refresh
    if (m_onJump)
        m_onJump(); // the host recomposites + refreshes its other views ONCE
}

void HistoryPanel::stepHistory(int delta) {
    if (m_doc == nullptr)
        return;
    const long cur = static_cast<long>(m_doc->commands().position());
    const long size = static_cast<long>(m_doc->commands().size());
    const auto target = static_cast<std::size_t>(std::clamp(cur + delta, 0L, size));
    jumpTo(target); // Up steps toward "Original" (undo), Down toward the newest (redo)
}

int HistoryPanel::handle(int event) {
    switch (event) {
    case FL_FOCUS:
    case FL_UNFOCUS:
        return 1; // take keyboard focus so Up/Down navigate history instead of scrolling the list
    case FL_PUSH:
        take_focus(); // a row click (dispatched below) also focuses us for subsequent key nav
        break;
    case FL_KEYBOARD:
    case FL_SHORTCUT:
        if (m_doc != nullptr && visible()) {
            if (Fl::event_key() == FL_Up) {
                stepHistory(-1);
                return 1;
            }
            if (Fl::event_key() == FL_Down) {
                stepHistory(+1);
                return 1;
            }
        }
        break;
    default:
        break;
    }
    return Fl_Group::handle(event);
}

void HistoryPanel::scrollCurrentIntoView() {
    if (m_rows.empty())
        return;
    const std::size_t cur = std::min(position(), m_rows.size() - 1);
    const int rowTop = static_cast<int>(cur) * kRowH; // offset within the scrolled content
    const int view = m_scroll->h();
    int target = m_scroll->yposition();
    if (rowTop < target)
        target = rowTop;
    else if (rowTop + kRowH > target + view)
        target = rowTop + kRowH - view;
    m_scroll->scroll_to(0, std::max(0, target));
}

void HistoryPanel::draw() {
    Fl_Group::draw();
    if (m_doc == nullptr) {
        const Palette& pal = activePalette();
        fl_font(FL_HELVETICA, 12);
        fl_color(toFl(pal.textMuted));
        fl_draw(_("No document"), m_scroll->x(), m_scroll->y(), m_scroll->w(), m_scroll->h(),
                FL_ALIGN_CENTER);
    }
}

} // namespace mosaic::ui
