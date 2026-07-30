#pragma once

#include "ui/theme.hpp" // Palette / activePalette (self-styling, re-theme for free)

#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Widget.H>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// A professional date-selection control (ui::DatePicker). The collapsed control shows the date as
// locale-aware text; the PRIMARY interaction is a pop-up CALENDAR (month grid, prev/next month,
// weekday headers, click a day) -- which sidesteps every DD/MM vs MM/DD vs YYYY-MM-DD format war,
// because you point at a day rather than type digits. Self-styling like ui::NumberField /
// ui::TimeDrum (MOSAIC_INPUT_BOX hairline + semantic FLTK colours, so a runtime re-theme follows
// for free).
//
// The bulk of the correctness lives in the pure `date_detail` helpers below (leap years, month
// lengths, day-of-week, the calendar grid, the locale field-order selection) -- all GUI-free and
// unit-tested headlessly; the widget + pop-up are a thin visual shell over them.
//
// First consumer (planned): the Texture Generator's solar calculator (docs/texture-generator.md
// §4.2), which currently makes the user type year/month/day into NumberFields. The parent maps
// DatePicker::date() -> core::texture::UtcTime.
namespace mosaic::ui {

// A calendar date (proleptic Gregorian). Kept small and copyable; the widget clamps it valid.
struct Date {
    int year = 2026;
    int month = 1; // 1..12
    int day = 1;   // 1..daysInMonth(year, month)
};

[[nodiscard]] inline bool operator==(const Date& a, const Date& b) noexcept {
    return a.year == b.year && a.month == b.month && a.day == b.day;
}
[[nodiscard]] inline bool operator!=(const Date& a, const Date& b) noexcept { return !(a == b); }

// The pure, testable date logic. No FLTK, no clocks -- only `localeFieldOrder` /
// `localeFirstDayOfWeek` read the process locale (via nl_langinfo where available), and they fall
// back to an unambiguous default when locale info is missing.
namespace date_detail {

// Gregorian leap year: divisible by 4, except centuries unless divisible by 400.
[[nodiscard]] bool isLeapYear(int year) noexcept;

// Days in `month` (1..12, clamped) of `year`; 28/29/30/31 with the leap-year rule for February.
[[nodiscard]] int daysInMonth(int year, int month) noexcept;

// `d` clamped to a real date: month into [1, 12], then day into [1, daysInMonth]. So a 31st that
// lands on a short month collapses to that month's last day (the calendar-navigation convention).
[[nodiscard]] Date clampDate(Date d) noexcept;

// Day of week for `d`, 0 = Sunday .. 6 = Saturday (Sakamoto's method; valid for Gregorian dates).
[[nodiscard]] int dayOfWeek(const Date& d) noexcept;

// Step (year, month) by `delta` whole months, wrapping the month into [1, 12] and carrying the
// year. Used by the pop-up's prev/next arrows.
void stepMonth(int& year, int& month, int delta) noexcept;

// The 6x7 = 42-cell month grid for (year, month): each cell is a day-of-month (1..daysInMonth) or
// 0 for a leading/trailing blank. `firstDayOfWeek` (0 = Sunday .. 6 = Saturday) sets the left
// column. A fixed 6 rows always covers any month (no layout jump), trailing cells staying 0.
[[nodiscard]] std::array<int, 42> monthGrid(int year, int month, int firstDayOfWeek) noexcept;

// The display order of the day/month/year fields.
enum class FieldOrder { DMY, MDY, YMD };

// The field order implied by a strftime-style date format (e.g. glibc's D_FMT: "%d/%m/%y",
// "%m/%d/%y", "%Y-%m-%d"), decided by which of the day / month / year conversions appears first.
// Returns nullopt when the three fields cannot be identified (a garbage or field-less string).
[[nodiscard]] std::optional<FieldOrder> fieldOrderFromDateFormat(std::string_view dateFormat);

// The field order for the current C/C++ locale (nl_langinfo(D_FMT) where available), or DMY --
// the unambiguous "12 Mar 2026"-style default -- when locale info is unavailable/unparseable. The
// user joked DD-MM-YYYY is "the only real format"; the YYYY-MM-DD camp exists too, so this is
// DERIVED, never hardcoded.
[[nodiscard]] FieldOrder localeFieldOrder();

// The locale's first day of the week (0 = Sunday .. 6 = Saturday), via glibc's
// _NL_TIME_FIRST_WEEKDAY where available, else Monday (ISO-8601). Purely cosmetic (which column the
// calendar starts on).
[[nodiscard]] int localeFirstDayOfWeek();

// English three-letter month abbreviation ("Jan".."Dec"); month clamped to [1, 12].
[[nodiscard]] const char* monthAbbrev(int month) noexcept;
// English full month name ("January".."December"); month clamped to [1, 12].
[[nodiscard]] const char* monthName(int month) noexcept;
// Two-letter weekday abbreviation for a 0=Sunday..6=Saturday index ("Su".."Sa"); clamped.
[[nodiscard]] const char* weekdayAbbrev(int dayOfWeekSundayZero) noexcept;

// `d` rendered as collapsed-control text in `order`, always with a spelled month so the result is
// unambiguous regardless of order: DMY "12 Mar 2026", MDY "Mar 12, 2026", YMD "2026 Mar 12".
[[nodiscard]] std::string formatDate(const Date& d, FieldOrder order);

} // namespace date_detail

class CalendarPopup; // the pop-up month grid, defined in date_picker.cpp

class DatePicker : public Fl_Widget {
public:
    DatePicker(int X, int Y, int W, int H, const char* label = nullptr);
    ~DatePicker() override;

    // ---- Value API -----------------------------------------------------------------------------
    void setDate(int year, int month, int day); // clamped valid; redraws; does NOT fire onChange
    void setDate(const Date& d) { setDate(d.year, d.month, d.day); }
    [[nodiscard]] Date date() const noexcept { return m_date; }
    [[nodiscard]] int year() const noexcept { return m_date.year; }
    [[nodiscard]] int month() const noexcept { return m_date.month; }
    [[nodiscard]] int day() const noexcept { return m_date.day; }

    // Fired when the user picks a day in the pop-up (never from setDate()). Gives the new date.
    void setOnChange(std::function<void(const Date&)> cb) { m_onChange = std::move(cb); }

    // The collapsed-control text for the current date in the locale-derived field order.
    [[nodiscard]] std::string displayText() const;

    // ---- Pop-up control (also toggled by a click on the control) -------------------------------
    void openCalendar();
    void closeCalendar();
    [[nodiscard]] bool calendarOpen() const;

    // The pop-up window itself, for the headless tests: they drive its handle() directly with
    // staged WINDOW-LOCAL coordinates -- the translated form FLTK actually delivers to a child
    // sub-window (X11 children select no input; Fl_Group::send subtracts the offset).
    [[nodiscard]] Fl_Double_Window* calendarForTest();

protected:
    void draw() override;
    int handle(int event) override;

private:
    friend class CalendarPopup;
    void commitFromCalendar(const Date& d); // the pop-up calls this on a day pick (fires onChange)

    Date m_date{2026, 1, 1};
    date_detail::FieldOrder m_order = date_detail::FieldOrder::DMY;
    std::function<void(const Date&)> m_onChange;
    CalendarPopup* m_popup = nullptr; // child sub-window of the host top-level (the ui::Popover rule)
    bool m_hover = false;
};

// Dismiss the open calendar pop-up (if any) when (hostX, hostY) -- coordinates relative to the
// parent top-level -- lie outside both it and its owning control. The host calls this from its
// handle() on every FL_PUSH, exactly as it does for dismissActiveDropdownPopupOnOutsideClick.
void dismissActiveCalendarPopupOnOutsideClick(int hostX, int hostY);
void dismissActiveCalendarPopup();

} // namespace mosaic::ui
