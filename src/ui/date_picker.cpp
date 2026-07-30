#include "ui/date_picker.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cstdio>

#if __has_include(<langinfo.h>)
#include <langinfo.h>
#define MOSAIC_HAVE_LANGINFO 1
#endif

namespace mosaic::ui {
namespace {
Fl_Color toFl(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }
} // namespace

// ================================================================================================
// date_detail -- the pure logic (GUI-free, unit-tested)
// ================================================================================================
namespace date_detail {

bool isLeapYear(int year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int daysInMonth(int year, int month) noexcept {
    const int m = std::clamp(month, 1, 12);
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && isLeapYear(year))
        return 29;
    return kDays[m - 1];
}

Date clampDate(Date d) noexcept {
    d.month = std::clamp(d.month, 1, 12);
    d.day = std::clamp(d.day, 1, daysInMonth(d.year, d.month));
    return d;
}

int dayOfWeek(const Date& d) noexcept {
    // Sakamoto's method: 0 = Sunday .. 6 = Saturday. Valid for any Gregorian date.
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = d.year;
    const int m = std::clamp(d.month, 1, 12);
    if (m < 3)
        y -= 1;
    const int r = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d.day) % 7;
    return r < 0 ? r + 7 : r;
}

void stepMonth(int& year, int& month, int delta) noexcept {
    // Work in 0-based months so the modular carry is clean for large |delta| too.
    long total = static_cast<long>(year) * 12 + (std::clamp(month, 1, 12) - 1) + delta;
    long y = total / 12;
    long m = total % 12;
    if (m < 0) { // C++ truncates toward zero; normalise into [0, 12)
        m += 12;
        y -= 1;
    }
    year = static_cast<int>(y);
    month = static_cast<int>(m) + 1;
}

std::array<int, 42> monthGrid(int year, int month, int firstDayOfWeek) noexcept {
    std::array<int, 42> grid{}; // zero-filled = all blank
    const int fdow = ((firstDayOfWeek % 7) + 7) % 7;
    const int dowFirst = dayOfWeek({year, std::clamp(month, 1, 12), 1}); // 0=Sun..6=Sat
    const int lead = ((dowFirst - fdow) % 7 + 7) % 7;                    // blank cells before day 1
    const int dim = daysInMonth(year, month);
    for (int day = 1; day <= dim; ++day) {
        const int cell = lead + day - 1;
        if (cell < 42)
            grid[static_cast<std::size_t>(cell)] = day;
    }
    return grid;
}

std::optional<FieldOrder> fieldOrderFromDateFormat(std::string_view dateFormat) {
    // Record the first appearance of each of the three field CATEGORIES as we scan the % escapes.
    // 0 = day, 1 = month, 2 = year; -1 = not yet seen.
    int firstDay = -1;
    int firstMonth = -1;
    int firstYear = -1;
    int pos = 0;
    for (std::size_t i = 0; i < dateFormat.size(); ++i) {
        if (dateFormat[i] != '%')
            continue;
        if (i + 1 >= dateFormat.size())
            break;
        const char c = dateFormat[i + 1];
        ++i; // consume the conversion char
        int cat = -1;
        switch (c) {
        case 'd': case 'e':                       cat = 0; break; // day of month
        case 'm': case 'b': case 'B': case 'h':   cat = 1; break; // month (numeric or named)
        case 'y': case 'Y': case 'G': case 'g':   cat = 2; break; // year
        default:                                  cat = -1; break; // %%, weekday, time, etc.
        }
        if (cat == 0 && firstDay < 0)
            firstDay = pos++;
        else if (cat == 1 && firstMonth < 0)
            firstMonth = pos++;
        else if (cat == 2 && firstYear < 0)
            firstYear = pos++;
    }
    if (firstDay < 0 || firstMonth < 0 || firstYear < 0)
        return std::nullopt; // not all three fields present -> can't determine an order
    // Decide by which field comes first, then the relative order of the other two.
    if (firstYear < firstDay && firstYear < firstMonth)
        return FieldOrder::YMD;
    if (firstDay < firstMonth)
        return FieldOrder::DMY;
    return FieldOrder::MDY;
}

FieldOrder localeFieldOrder() {
#ifdef MOSAIC_HAVE_LANGINFO
    const char* fmt = nl_langinfo(D_FMT);
    if (fmt != nullptr && *fmt != '\0')
        if (const auto o = fieldOrderFromDateFormat(fmt))
            return *o;
#endif
    return FieldOrder::DMY; // unambiguous "12 Mar 2026" default
}

int localeFirstDayOfWeek() {
#if defined(MOSAIC_HAVE_LANGINFO) && defined(_NL_TIME_FIRST_WEEKDAY)
    // glibc extension: a one-byte value, 1 = the first day listed in `day` (Sunday) .. 7 = Saturday.
    const char* w = nl_langinfo(_NL_TIME_FIRST_WEEKDAY);
    if (w != nullptr) {
        const int v = static_cast<unsigned char>(w[0]);
        if (v >= 1 && v <= 7)
            return v - 1; // -> 0 = Sunday .. 6 = Saturday
    }
#endif
    return 1; // Monday (ISO-8601)
}

const char* monthAbbrev(int month) noexcept {
    static const char* const kNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return kNames[std::clamp(month, 1, 12) - 1];
}

const char* monthName(int month) noexcept {
    static const char* const kNames[] = {"January", "February", "March",     "April",
                                         "May",     "June",     "July",      "August",
                                         "September", "October", "November", "December"};
    return kNames[std::clamp(month, 1, 12) - 1];
}

const char* weekdayAbbrev(int dayOfWeekSundayZero) noexcept {
    static const char* const kDays[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    return kDays[((dayOfWeekSundayZero % 7) + 7) % 7];
}

std::string formatDate(const Date& d, FieldOrder order) {
    char buf[32];
    switch (order) {
    case FieldOrder::MDY:
        std::snprintf(buf, sizeof(buf), "%s %d, %d", monthAbbrev(d.month), d.day, d.year);
        break;
    case FieldOrder::YMD:
        std::snprintf(buf, sizeof(buf), "%d %s %d", d.year, monthAbbrev(d.month), d.day);
        break;
    case FieldOrder::DMY:
    default:
        std::snprintf(buf, sizeof(buf), "%d %s %d", d.day, monthAbbrev(d.month), d.year);
        break;
    }
    return std::string(buf);
}

} // namespace date_detail

// The date logic is used unqualified below (this is a .cpp): bring it into mosaic::ui scope so both
// CalendarPopup and the DatePicker methods can call stepMonth/monthGrid/formatDate/etc. directly.
using namespace date_detail;

// ================================================================================================
// CalendarPopup -- the pop-up month grid (a child sub-window of the host top-level)
// ================================================================================================
namespace {

constexpr int kCalPad = 8;
constexpr int kCellW = 34;
constexpr int kCellH = 28;
constexpr int kCols = 7;
constexpr int kRows = 6;
constexpr int kHeaderH = 30;
constexpr int kWeekH = 22;
constexpr int kArrowW = 26; // clickable prev/next zone width at each end of the header
constexpr int kCalW = kCalPad * 2 + kCols * kCellW;
constexpr int kCalH = kCalPad * 2 + kHeaderH + kWeekH + kRows * kCellH;

// The one open calendar pop-up (at most one), for host-forwarded outside-click dismissal -- mirrors
// DropdownPopup's g_activeDropdown. CalendarPopup is forward-declared in the header; a pointer to the
// incomplete type is fine here.
CalendarPopup* g_activeCalendar = nullptr;
} // namespace

// The internal type name the header forward-declares. Kept as a distinct class so the header need
// not pull in Fl_Double_Window's grid geometry.
class CalendarPopup : public Fl_Double_Window {
public:
    explicit CalendarPopup(DatePicker* owner) : Fl_Double_Window(0, 0, kCalW, kCalH), m_owner(owner) {
        border(0); // a sub-window carries no decoration anyway (mirrors ui::Popover / DropdownPopup)
        color(toFl(activePalette().panelBg));
        m_firstDow = localeFirstDayOfWeek();
        end(); // fully custom-drawn; no child widgets
        // Start HIDDEN: an Fl_Double_Window (sub-window) is visible() by default, so it would be
        // auto-mapped the moment a parent modal maps -- the calendar popping open on dialog open.
        // ui::Popover / DropdownPopup hide() in their ctors for the same reason.
        hide();
    }

    ~CalendarPopup() override {
        if (g_activeCalendar == this)
            g_activeCalendar = nullptr;
    }

    // Place under the owning control (in this pop-up's top-level coordinates, like DropdownPopup)
    // and show. The grid opens on the owner's current month.
    void openUnder() {
        if (m_owner == nullptr)
            return;
        const Date d = m_owner->date();
        m_viewYear = d.year;
        m_viewMonth = d.month;
        m_hoverCell = -1;

        // Translate the owner's position into this pop-up's top-level coords: FLTK widget coords are
        // relative to the nearest enclosing Fl_Window, so add each enclosing sub-window's offset.
        int ox = m_owner->x();
        int oy = m_owner->y();
        for (Fl_Window* w = m_owner->window(); w != nullptr && w != window(); w = w->window()) {
            ox += w->x();
            oy += w->y();
        }
        m_ownerX = ox;
        m_ownerY = oy;
        m_ownerW = m_owner->w();
        m_ownerH = m_owner->h();

        int px = ox;
        int py = oy + m_owner->h() + 2; // just below the control
        if (const Fl_Window* parent = window()) {
            px = std::clamp(px, 0, std::max(0, parent->w() - kCalW));
            // Flip above the control if it would overflow the bottom of the parent window.
            if (py + kCalH > parent->h())
                py = std::max(0, oy - kCalH - 2);
        }
        resize(px, py, kCalW, kCalH);
        show();
        m_open = true;
        g_activeCalendar = this;
    }

    void hide() override {
        m_open = false;
        if (g_activeCalendar == this)
            g_activeCalendar = nullptr;
        Fl_Double_Window::hide();
    }

    // The LOGICAL open state: set by openUnder(), cleared by hide() (which every dismissal path
    // goes through). Neither visible() nor shown() can carry this truth: FLTK defers a child
    // sub-window's show() entirely while the host top-level is unmapped (both flags stay 0), so
    // the toggle state would under-report there -- and headlessly, where the tests drive it.
    [[nodiscard]] bool openRequested() const { return m_open; }

    // (hostX, hostY) in parent-top-level coords: inside the pop-up or its owner control?
    [[nodiscard]] bool spansHostPoint(int hostX, int hostY) const {
        const bool inPopup =
            hostX >= x() && hostX < x() + w() && hostY >= y() && hostY < y() + h();
        const bool inOwner = hostX >= m_ownerX && hostX < m_ownerX + m_ownerW &&
                             hostY >= m_ownerY && hostY < m_ownerY + m_ownerH;
        return inPopup || inOwner;
    }

protected:
    void draw() override {
        const Palette& p = activePalette();
        // A widget owns every pixel of its rect: fill the whole ground first ([[mosaic-ui-gotchas]]).
        fl_color(toFl(p.panelBg));
        fl_rectf(0, 0, w(), h());

        // ---- Header: prev arrow | "Month Year" | next arrow ----
        const int hy = kCalPad;
        char title[32];
        std::snprintf(title, sizeof(title), "%s %d", monthName(m_viewMonth), m_viewYear);
        fl_font(FL_HELVETICA_BOLD, 13);
        fl_color(toFl(p.text));
        fl_draw(title, kCalPad + kArrowW, hy, kCols * kCellW - 2 * kArrowW, kHeaderH, FL_ALIGN_CENTER);
        drawArrow(kCalPad + kArrowW / 2, hy + kHeaderH / 2, false, m_hoverCell == kPrevHit);
        drawArrow(w() - kCalPad - kArrowW / 2, hy + kHeaderH / 2, true, m_hoverCell == kNextHit);

        // ---- Weekday header row ----
        fl_font(FL_HELVETICA, 11);
        const int wy = kCalPad + kHeaderH;
        for (int c = 0; c < kCols; ++c) {
            const int dow = (m_firstDow + c) % 7;
            fl_color(toFl(p.textMuted));
            fl_draw(weekdayAbbrev(dow), gridX(c), wy, kCellW, kWeekH, FL_ALIGN_CENTER);
        }
        // hairline under the weekday row
        fl_color(toFl(p.border));
        fl_line(kCalPad, wy + kWeekH - 1, kCalPad + kCols * kCellW - 1, wy + kWeekH - 1);

        // ---- Day grid ----
        const std::array<int, 42> grid = monthGrid(m_viewYear, m_viewMonth, m_firstDow);
        const Date sel = m_owner != nullptr ? m_owner->date() : Date{};
        const bool selHere = m_owner != nullptr && sel.year == m_viewYear && sel.month == m_viewMonth;
        fl_font(FL_HELVETICA, 12);
        for (int cell = 0; cell < 42; ++cell) {
            const int day = grid[static_cast<std::size_t>(cell)];
            if (day == 0)
                continue;
            const int c = cell % kCols;
            const int r = cell / kCols;
            const int cx = gridX(c);
            const int cy = gridY(r);
            const bool isSel = selHere && day == sel.day;
            const bool isHover = cell == m_hoverCell;
            if (isSel) {
                fl_color(toFl(p.accent));
                fl_rectf(cx + 2, cy + 2, kCellW - 4, kCellH - 4);
            } else if (isHover) {
                fl_color(toFl(p.controlHover));
                fl_rectf(cx + 2, cy + 2, kCellW - 4, kCellH - 4);
            }
            char db[4];
            std::snprintf(db, sizeof(db), "%d", day);
            fl_color(toFl(isSel ? p.onAccent : p.text));
            fl_draw(db, cx, cy, kCellW, kCellH, FL_ALIGN_CENTER);
        }

        // ---- Outer frame ----
        fl_color(toFl(p.border));
        fl_rect(0, 0, w(), h());
    }

    int handle(int event) override {
        // Fl::event_x/y arrive WINDOW-LOCAL here: X11 child sub-windows select no input events
        // (only Exposure), so pointer events enter through the top-level and Fl_Group::send
        // subtracts this window's offset before delivering (the DropdownPopup convention).
        // Subtracting x()/y() again shifted every hit by the pop-up's position -- day picks and
        // the arrows landed on nothing (pinned in test_date_picker.cpp).
        switch (event) {
        case FL_PUSH: {
            const int hit = hitTest(Fl::event_x(), Fl::event_y());
            if (hit == kPrevHit) {
                stepMonth(m_viewYear, m_viewMonth, -1);
                redraw();
                return 1;
            }
            if (hit == kNextHit) {
                stepMonth(m_viewYear, m_viewMonth, +1);
                redraw();
                return 1;
            }
            if (hit >= 0) {
                const std::array<int, 42> grid = monthGrid(m_viewYear, m_viewMonth, m_firstDow);
                const int day = grid[static_cast<std::size_t>(hit)];
                if (day > 0 && m_owner != nullptr) {
                    m_owner->commitFromCalendar(Date{m_viewYear, m_viewMonth, day});
                    hide();
                }
            }
            return 1; // swallow presses inside the pop-up (outside ones are host-dismissed)
        }
        case FL_RELEASE:
            // Claim the release of every press we claimed (the house press/release-pair rule): a
            // day-pick hides this pop-up DURING the press, and an unclaimed release would then be
            // re-offered to whatever dialog chrome sits where the calendar was.
            return 1;
        case FL_MOVE:
        case FL_DRAG: {
            const int hit = hitTest(Fl::event_x(), Fl::event_y());
            if (hit != m_hoverCell) {
                m_hoverCell = hit;
                redraw();
            }
            return 1;
        }
        case FL_LEAVE:
            if (m_hoverCell != -1) {
                m_hoverCell = -1;
                redraw();
            }
            return 1;
        case FL_KEYBOARD:
            if (Fl::event_key() == FL_Escape) {
                hide();
                return 1;
            }
            if (Fl::event_key() == FL_Left) {
                stepMonth(m_viewYear, m_viewMonth, -1);
                redraw();
                return 1;
            }
            if (Fl::event_key() == FL_Right) {
                stepMonth(m_viewYear, m_viewMonth, +1);
                redraw();
                return 1;
            }
            return Fl_Double_Window::handle(event);
        default:
            return Fl_Double_Window::handle(event);
        }
    }

private:
    static constexpr int kPrevHit = -2;
    static constexpr int kNextHit = -3;

    [[nodiscard]] static int gridX(int col) { return kCalPad + col * kCellW; }
    [[nodiscard]] static int gridY(int row) { return kCalPad + kHeaderH + kWeekH + row * kCellH; }

    // Map a local (lx, ly) point to a hit: a grid cell index [0, 42), or kPrevHit / kNextHit for the
    // header arrows, or -1 for nothing actionable.
    [[nodiscard]] int hitTest(int lx, int ly) const {
        if (ly >= kCalPad && ly < kCalPad + kHeaderH) {
            if (lx >= kCalPad && lx < kCalPad + kArrowW)
                return kPrevHit;
            if (lx >= w() - kCalPad - kArrowW && lx < w() - kCalPad)
                return kNextHit;
            return -1;
        }
        const int gy0 = kCalPad + kHeaderH + kWeekH;
        if (ly < gy0 || lx < kCalPad)
            return -1;
        const int col = (lx - kCalPad) / kCellW;
        const int row = (ly - gy0) / kCellH;
        if (col < 0 || col >= kCols || row < 0 || row >= kRows)
            return -1;
        return row * kCols + col;
    }

    void drawArrow(int cx, int cy, bool pointRight, bool hover) const {
        const Palette& p = activePalette();
        fl_color(toFl(hover ? p.text : p.textMuted));
        fl_begin_polygon();
        if (pointRight) {
            fl_vertex(cx - 3, cy - 5);
            fl_vertex(cx + 4, cy);
            fl_vertex(cx - 3, cy + 5);
        } else {
            fl_vertex(cx + 3, cy - 5);
            fl_vertex(cx - 4, cy);
            fl_vertex(cx + 3, cy + 5);
        }
        fl_end_polygon();
    }

    DatePicker* m_owner = nullptr;
    bool m_open = false; // logically open (see openRequested)
    int m_viewYear = 2026;
    int m_viewMonth = 1;
    int m_firstDow = 1; // 0=Sun..6=Sat
    int m_hoverCell = -1;
    int m_ownerX = 0;
    int m_ownerY = 0;
    int m_ownerW = 0;
    int m_ownerH = 0;
};

// ================================================================================================
// DatePicker
// ================================================================================================

DatePicker::DatePicker(int X, int Y, int W, int H, const char* label)
    : Fl_Widget(X, Y, W, H, label) {
    box(MOSAIC_INPUT_BOX);
    m_order = date_detail::localeFieldOrder();
    // Build the pop-up now, while the host top-level is still unshown -- a sub-window added to an
    // already-realized parent is promoted by FLTK to a stray top-level (the ui::Popover rule,
    // [[mosaic-ui-gotchas]]). It is a child of the current group (the host), which owns it; we keep
    // only a back-pointer.
    m_popup = new CalendarPopup(this);
}

DatePicker::~DatePicker() = default;
// The host top-level group owns and deletes the pop-up (the ui::Popover / DropdownPopup ownership
// model). We deliberately touch nothing here: FLTK's Fl_Group::clear() deletes the later-added
// pop-up BEFORE this DatePicker during a wholesale teardown, so any m_popup access in the dtor would
// be a use-after-free. That is exactly why a Dropdown never touches its DropdownPopup in its dtor.

void DatePicker::setDate(int year, int month, int day) {
    m_date = date_detail::clampDate({year, month, day});
    redraw();
}

std::string DatePicker::displayText() const {
    return date_detail::formatDate(m_date, m_order);
}

void DatePicker::commitFromCalendar(const Date& d) {
    m_date = date_detail::clampDate(d);
    redraw();
    if (m_onChange)
        m_onChange(m_date);
}

void DatePicker::openCalendar() {
    if (m_popup != nullptr)
        m_popup->openUnder();
}

void DatePicker::closeCalendar() {
    // Unconditional: hide() of an already-hidden pop-up is harmless, and gating it on a
    // visibility flag would let the calendarGuard miss the auto-mapped-on-dialog-show pop-up.
    if (m_popup != nullptr)
        m_popup->hide();
}

bool DatePicker::calendarOpen() const {
    // The pop-up's LOGICAL open state, not shown()/visible(): FLTK defers a child sub-window's
    // show() while the host top-level is unmapped (both flags stay 0 -- and headlessly they
    // never rise), which would make the click toggle read permanently "closed".
    return m_popup != nullptr && m_popup->openRequested();
}

Fl_Double_Window* DatePicker::calendarForTest() {
    return m_popup;
}

void DatePicker::draw() {
    const Palette& p = activePalette();
    const bool on = active_r();
    draw_box(MOSAIC_INPUT_BOX, x(), y(), w(), h(), toFl(on && m_hover ? p.controlHover : p.controlBg));

    // A small calendar glyph on the right (hand-drawn -- the no-Unicode-in-labels rule).
    const int gW = 13;
    const int gH = 13;
    const int gx = x() + w() - gW - 8;
    const int gy = y() + (h() - gH) / 2;
    fl_color(toFl(on ? p.textMuted : p.border));
    fl_rect(gx, gy + 2, gW, gH - 2);              // calendar body
    fl_line(gx, gy + 5, gx + gW - 1, gy + 5);     // header rule
    fl_line(gx + 3, gy, gx + 3, gy + 3);          // left binding ring
    fl_line(gx + gW - 4, gy, gx + gW - 4, gy + 3); // right binding ring

    // The date text, clipped before the glyph.
    fl_color(toFl(on ? p.text : p.textMuted));
    fl_font(FL_HELVETICA, 12);
    const std::string txt = displayText();
    const int textW = std::max(0, w() - 8 - gW - 12);
    fl_push_clip(x() + 8, y(), textW, h());
    fl_draw(txt.c_str(), x() + 8, y(), w() - 8, h(), FL_ALIGN_LEFT);
    fl_pop_clip();
}

int DatePicker::handle(int event) {
    switch (event) {
    case FL_ENTER:
        m_hover = true;
        redraw();
        return 1;
    case FL_LEAVE:
        m_hover = false;
        redraw();
        return 1;
    // THE HOUSE CLICK CONVENTION (CheckBox / RailItem): claim the WHOLE gesture -- FL_PUSH *and*
    // FL_DRAG/FL_RELEASE all return 1 -- and act on the release that ends inside. A widget that
    // claims only the press lets the release/drag escape to Fl_Group::handle, which offers them
    // to the next child under the pointer: the recurring "clicks fall through to the chrome
    // underneath" bug (pinned in test_date_picker.cpp). Copy THIS shape, not push-only, into
    // every new clickable widget.
    case FL_PUSH:
        return 1; // claim; the toggle waits for a release that stays inside
    case FL_DRAG:
        return 1;
    case FL_RELEASE:
        if (Fl::event_inside(this)) {
            if (calendarOpen())
                closeCalendar();
            else
                openCalendar();
        }
        return 1;
    default:
        return Fl_Widget::handle(event);
    }
}

void dismissActiveCalendarPopup() {
    if (g_activeCalendar != nullptr)
        g_activeCalendar->hide();
}

void dismissActiveCalendarPopupOnOutsideClick(int hostX, int hostY) {
    if (g_activeCalendar != nullptr && !g_activeCalendar->spansHostPoint(hostX, hostY))
        g_activeCalendar->hide();
}

} // namespace mosaic::ui
