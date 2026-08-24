#include "ui/date_picker.hpp"

#include <doctest/doctest.h>

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>

#include <array>
#include <cstdlib>
#include <string>

using namespace mosaic::ui;
using namespace mosaic::ui::date_detail;

// The DatePicker's correctness lives in these pure helpers (leap years, month lengths, day-of-week,
// the calendar grid, the locale field-order selection) -- all GUI-free. The pop-up calendar's visual
// behaviour (clicking a day, prev/next paging) is verified by the user's visual pass; here we pin the
// grid MATHS the pop-up draws from, so a day cell can never map to the wrong date.

TEST_CASE("isLeapYear: the full Gregorian rule") {
    CHECK(isLeapYear(2024));
    CHECK_FALSE(isLeapYear(2023));
    CHECK(isLeapYear(2000)); // divisible by 400
    CHECK_FALSE(isLeapYear(1900)); // century, not by 400
    CHECK(isLeapYear(1600));
    CHECK_FALSE(isLeapYear(2100));
}

TEST_CASE("daysInMonth: month lengths, February, and month clamping") {
    CHECK(daysInMonth(2023, 1) == 31);
    CHECK(daysInMonth(2023, 2) == 28);
    CHECK(daysInMonth(2024, 2) == 29); // leap
    CHECK(daysInMonth(1900, 2) == 28); // century non-leap
    CHECK(daysInMonth(2000, 2) == 29); // 400 leap
    CHECK(daysInMonth(2023, 4) == 30);
    CHECK(daysInMonth(2023, 12) == 31);
    // Out-of-range month clamps into [1, 12].
    CHECK(daysInMonth(2023, 0) == daysInMonth(2023, 1));
    CHECK(daysInMonth(2023, 13) == daysInMonth(2023, 12));
}

TEST_CASE("clampDate: invalid days collapse to the month's last valid day") {
    CHECK(clampDate({2023, 2, 31}) == Date{2023, 2, 28});
    CHECK(clampDate({2024, 2, 31}) == Date{2024, 2, 29});
    CHECK(clampDate({2023, 4, 31}) == Date{2023, 4, 30});
    CHECK(clampDate({2023, 13, 5}) == Date{2023, 12, 5}); // month clamps first
    CHECK(clampDate({2023, 0, 10}) == Date{2023, 1, 10});
    CHECK(clampDate({2023, 6, 0}) == Date{2023, 6, 1});
    CHECK(clampDate({2026, 7, 15}) == Date{2026, 7, 15}); // already valid: unchanged
}

TEST_CASE("dayOfWeek: known anchors (0=Sunday..6=Saturday)") {
    CHECK(dayOfWeek({2000, 1, 1}) == 6); // Saturday
    CHECK(dayOfWeek({2023, 1, 1}) == 0); // Sunday
    CHECK(dayOfWeek({1970, 1, 1}) == 4); // Thursday (the Unix epoch)
    CHECK(dayOfWeek({2026, 7, 15}) == 3); // Wednesday
}

TEST_CASE("stepMonth: wraps the month and carries the year, for any delta") {
    int y = 2026, m = 1;
    stepMonth(y, m, -1);
    CHECK(y == 2025);
    CHECK(m == 12);

    y = 2026; m = 12;
    stepMonth(y, m, +1);
    CHECK(y == 2027);
    CHECK(m == 1);

    y = 2026; m = 6;
    stepMonth(y, m, +12);
    CHECK(y == 2027);
    CHECK(m == 6);

    y = 2026; m = 1;
    stepMonth(y, m, +25); // Jan 2026 + 25 months = Feb 2028
    CHECK(y == 2028);
    CHECK(m == 2);

    y = 2026; m = 3;
    stepMonth(y, m, -5); // Mar 2026 - 5 = Oct 2025
    CHECK(y == 2025);
    CHECK(m == 10);
}

TEST_CASE("monthGrid: leading blanks, day placement, and firstDayOfWeek") {
    // July 2026: the 1st is a Wednesday (dayOfWeek == 3).
    REQUIRE(dayOfWeek({2026, 7, 1}) == 3);

    // Sunday-first (0): 3 leading blanks, day 1 in column 3.
    const std::array<int, 42> gSun = monthGrid(2026, 7, 0);
    CHECK(gSun[0] == 0);
    CHECK(gSun[1] == 0);
    CHECK(gSun[2] == 0);
    CHECK(gSun[3] == 1);
    CHECK(gSun[3 + 30] == 31); // July has 31 days
    CHECK(gSun[3 + 31] == 0);  // trailing blank

    // Monday-first (1): Wednesday is column 2, so 2 leading blanks.
    const std::array<int, 42> gMon = monthGrid(2026, 7, 1);
    CHECK(gMon[0] == 0);
    CHECK(gMon[1] == 0);
    CHECK(gMon[2] == 1);

    // Whatever the start day: exactly daysInMonth non-zero cells, contiguous and monotonic 1..dim.
    for (int fdow = 0; fdow < 7; ++fdow) {
        const std::array<int, 42> g = monthGrid(2026, 7, fdow);
        int count = 0, prev = 0;
        for (int cell : g) {
            if (cell == 0)
                continue;
            ++count;
            CHECK(cell == prev + 1); // consecutive, in order
            prev = cell;
        }
        CHECK(count == daysInMonth(2026, 7));
    }

    // A month that needs 6 rows still fits (fixed 42-cell grid): May 2021 (1st = Saturday), 31 days.
    const std::array<int, 42> may = monthGrid(2021, 5, 0); // Sunday-first
    REQUIRE(dayOfWeek({2021, 5, 1}) == 6);
    CHECK(may[6] == 1);   // Saturday column
    CHECK(may[36] == 31); // day 31 lands in the 6th row (index 36)
}

TEST_CASE("fieldOrderFromDateFormat: derived from the first date field seen") {
    using FO = FieldOrder;
    CHECK(fieldOrderFromDateFormat("%d/%m/%y") == FO::DMY);   // en_GB / most of the world
    CHECK(fieldOrderFromDateFormat("%m/%d/%y") == FO::MDY);   // en_US
    CHECK(fieldOrderFromDateFormat("%Y-%m-%d") == FO::YMD);   // ISO 8601 / many CJK locales
    CHECK(fieldOrderFromDateFormat("%d.%m.%Y") == FO::DMY);   // de_DE
    CHECK(fieldOrderFromDateFormat("%A %B %e, %Y") == FO::MDY); // spelled month before day -> MDY
    // Not determinable: no date fields, or fewer than all three.
    CHECK_FALSE(fieldOrderFromDateFormat("nonsense with no percent").has_value());
    CHECK_FALSE(fieldOrderFromDateFormat("%H:%M:%S").has_value());
    CHECK_FALSE(fieldOrderFromDateFormat("%m only, no day or year").has_value());
    CHECK_FALSE(fieldOrderFromDateFormat("%%").has_value()); // literal percent is not a field
}

TEST_CASE("localeFieldOrder / localeFirstDayOfWeek: always valid, never crash") {
    const FieldOrder o = localeFieldOrder();
    CHECK((o == FieldOrder::DMY || o == FieldOrder::MDY || o == FieldOrder::YMD));
    const int fdow = localeFirstDayOfWeek();
    CHECK(fdow >= 0);
    CHECK(fdow <= 6);
}

TEST_CASE("names and formatDate: unambiguous spelled-month output in each order") {
    CHECK(std::string(monthAbbrev(3)) == "Mar");
    CHECK(std::string(monthAbbrev(0)) == "Jan");  // clamps
    CHECK(std::string(monthAbbrev(13)) == "Dec"); // clamps
    CHECK(std::string(monthName(7)) == "July");
    CHECK(std::string(weekdayAbbrev(0)) == "Su");
    CHECK(std::string(weekdayAbbrev(6)) == "Sa");
    CHECK(std::string(weekdayAbbrev(7)) == "Su"); // wraps

    const Date d{2026, 3, 12};
    CHECK(formatDate(d, FieldOrder::DMY) == "12 Mar 2026");
    CHECK(formatDate(d, FieldOrder::MDY) == "Mar 12, 2026");
    CHECK(formatDate(d, FieldOrder::YMD) == "2026 Mar 12");
}

// ---- Widget smoke (needs a display for FLTK widget construction) --------------------------------
TEST_CASE("DatePicker widget: value round-trip, clamping, and display text") {
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return; // headless CI: the pure logic above is the substance; the pop-up is a visual pass

    Fl_Double_Window win(0, 0, 320, 60);
    win.begin();
    DatePicker dp(10, 10, 200, 28);
    win.end();

    dp.setDate(2026, 7, 15);
    CHECK(dp.year() == 2026);
    CHECK(dp.month() == 7);
    CHECK(dp.day() == 15);
    CHECK(dp.date() == Date{2026, 7, 15});

    // setDate clamps an impossible day.
    dp.setDate(2023, 2, 31);
    CHECK(dp.date() == Date{2023, 2, 28});

    // Display text carries the (unambiguous, spelled) month and the year, whatever the locale order.
    dp.setDate(2026, 3, 12);
    const std::string txt = dp.displayText();
    CHECK(txt.find("Mar") != std::string::npos);
    CHECK(txt.find("2026") != std::string::npos);
    CHECK(txt.find("12") != std::string::npos);

    // The calendar starts closed.
    CHECK_FALSE(dp.calendarOpen());
}

// ---- The click fall-through regression -----------------------------------------------------------
// FLTK's Fl_Group::handle offers FL_RELEASE / FL_DRAG to the children under the pointer, topmost
// first, until one answers 1 -- whenever such an event reaches a GROUP instead of going straight to
// Fl::pushed(). That happens in every direct headless handle() drive, and live whenever pushed()
// moved off the widget mid-gesture (Fl_Group::clear() re-points it at the group being cleared --
// measured on FLTK 1.4.5 -- and the texture dialog's rebuildControls() clears constantly). So a
// widget that claims FL_PUSH but lets FL_RELEASE/FL_DRAG escape (returns 0) passes them on to
// whatever sits UNDER it: the recurring "clicks fall through to the chrome underneath" bug. These
// tests pin the house convention (CheckBox / RailItem): claim the WHOLE press/release pair and act
// on the release that ends inside.

namespace {

// A house-convention widget that records what FLTK hands it, standing in for whatever chrome sits
// under the picker. Any press/drag/release it receives from a gesture that began on the DatePicker
// IS the leak.
class ChromeSensor : public Fl_Widget {
public:
    ChromeSensor(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}
    int pushes = 0;
    int drags = 0;
    int releases = 0;

protected:
    void draw() override {}
    int handle(int event) override {
        switch (event) {
        case FL_PUSH: ++pushes; return 1;
        case FL_DRAG: ++drags; return 1;
        case FL_RELEASE: ++releases; return 1;
        default: return Fl_Widget::handle(event);
        }
    }
};

// The house idiom for staging a synthetic pointer event (the test_canvas_cursor precedent):
// Fl::e_x / e_y / e_keysym / e_state are plain public fields.
void stagePointer(int x, int y) {
    Fl::e_x = x;
    Fl::e_y = y;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
}

} // namespace

TEST_CASE("DatePicker claims the whole press/release pair (the click fall-through bug)") {
    // 420 tall, not 120: the calendar pop-up is 254x236, so a 120px window cannot contain it at
    // all. openUnder() then took its "does not fit below" branch, flipped above, and clamped to
    // y=0 -- landing the pop-up ON TOP of the picker that opened it, where it swallowed the second
    // click and the calendar could never be toggled shut. That is a degenerate placement no real
    // host reaches: the modals that use DatePicker are fixed-size and comfortably larger (the
    // Texture Generator is 960x620). This subcase is about dispatch routing, so give it a window
    // that can actually hold the widget under test.
    Fl_Double_Window win(0, 0, 320, 420);
    win.begin();
    ChromeSensor chrome(10, 10, 200, 28); // the chrome UNDER the picker (same rect, below in z)
    DatePicker dp(10, 10, 200, 28);       // on top; its pop-up becomes the next (hidden) child
    win.end();

    SUBCASE("group-delivered release: the picker answers, nothing reaches the chrome under it") {
        // The exact post-rebuild reality: the release re-enters group dispatch, which offers it
        // to the children under the pointer, topmost first. The picker must answer for its own
        // gesture instead of letting the release (or drag) fall through to the chrome beneath.
        stagePointer(40, 24); // inside the picker (and the chrome under it)
        Fl::e_state = FL_BUTTON1;
        CHECK(win.handle(FL_PUSH) == 1);
        CHECK(chrome.pushes == 0); // the topmost widget claimed the press
        CHECK(win.handle(FL_DRAG) == 1);
        CHECK(chrome.drags == 0); // an escaping drag falls through just like a release
        Fl::e_state = 0;
        CHECK(win.handle(FL_RELEASE) == 1);
        CHECK(chrome.releases == 0); // THE regression: the release must never fall through
        CHECK(dp.calendarOpen()); // and the click still toggles the calendar (on release)
    }

    SUBCASE("dispatcher-faithful gesture: pushed() routing, toggle on release-inside") {
        // Through the real Fl::handle pipeline: the PUSH makes the picker Fl::pushed(), and the
        // RELEASE -- routed straight to it -- must be answered (1), not dropped back into the
        // dispatcher, with the calendar toggling on the release.
        stagePointer(40, 24);
        Fl::e_state = FL_BUTTON1;
        CHECK(Fl::handle(FL_PUSH, &win) == 1);
        CHECK(Fl::pushed() == &dp);
        CHECK_FALSE(dp.calendarOpen()); // the press alone must not toggle yet
        Fl::e_state = 0;
        CHECK(Fl::handle(FL_RELEASE, &win) == 1);
        CHECK(dp.calendarOpen());

        // A second click toggles it shut again.
        Fl::e_state = FL_BUTTON1;
        CHECK(Fl::handle(FL_PUSH, &win) == 1);
        Fl::e_state = 0;
        CHECK(Fl::handle(FL_RELEASE, &win) == 1);
        CHECK_FALSE(dp.calendarOpen());
    }

    SUBCASE("a press that ends outside does not toggle") {
        stagePointer(40, 24);
        Fl::e_state = FL_BUTTON1;
        CHECK(Fl::handle(FL_PUSH, &win) == 1);
        stagePointer(300, 110); // slide off the control before letting go
        CHECK(Fl::handle(FL_DRAG, &win) == 1);
        Fl::e_state = 0;
        CHECK(Fl::handle(FL_RELEASE, &win) == 1);
        CHECK_FALSE(dp.calendarOpen());
    }

    // Don't let staged globals bleed into other tests.
    Fl::pushed(nullptr);
    Fl::belowmouse(nullptr);
    Fl::e_state = 0;
}

TEST_CASE("CalendarPopup hit-tests in its own coordinates (day picks land on the day)") {
    // X11 child sub-windows select no input events, so FLTK routes pointer events through the
    // top-level and Fl_Group::send translates Fl::e_x/e_y to WINDOW-LOCAL before delivering (the
    // DropdownPopup convention). The pop-up used to subtract x()/y() a second time, shifting
    // every hit by its own position: with the calendar open anywhere but (0,0), a click on a day
    // silently landed on nothing.
    Fl_Double_Window win(0, 0, 400, 400);
    win.begin();
    DatePicker dp(60, 40, 200, 28);
    win.end();

    dp.setDate(2026, 7, 1);
    Date picked{};
    int changes = 0;
    dp.setOnChange([&](const Date& d) {
        picked = d;
        ++changes;
    });

    dp.openCalendar(); // places the pop-up under the control (non-zero x/y) and opens on July 2026
    Fl_Double_Window* cal = dp.calendarForTest();
    REQUIRE(cal != nullptr);
    REQUIRE(dp.calendarOpen());
    CHECK(cal->x() > 0); // the shifted-hit bug needs a pop-up away from the parent origin
    CHECK(cal->y() > 0);

    // Locate day 15's cell centre in the pop-up's own coordinates, from the same grid maths the
    // pop-up draws with (pad 8, header 30, weekday row 22; cell size derived from the window).
    const int pad = 8;
    const int cellW = (cal->w() - 2 * pad) / 7;
    const int cellH = (cal->h() - 2 * pad - 30 - 22) / 6;
    const std::array<int, 42> grid = monthGrid(2026, 7, localeFirstDayOfWeek());
    int cell = -1;
    for (int i = 0; i < 42; ++i)
        if (grid[static_cast<std::size_t>(i)] == 15) cell = i;
    REQUIRE(cell >= 0);
    Fl::e_x = pad + (cell % 7) * cellW + cellW / 2;
    Fl::e_y = pad + 30 + 22 + (cell / 7) * cellH + cellH / 2;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;

    // Deliver the press exactly as FLTK does: window-local coords, straight to the pop-up.
    CHECK(cal->handle(FL_PUSH) == 1);
    CHECK(changes == 1);
    CHECK(picked == Date{2026, 7, 15});
    CHECK(dp.date() == Date{2026, 7, 15}); // committed to the owner
    CHECK_FALSE(dp.calendarOpen());        // a day pick closes the pop-up
    CHECK(cal->handle(FL_RELEASE) == 1);   // the pop-up claims its release (the pair rule)

    Fl::pushed(nullptr);
    Fl::belowmouse(nullptr);
    Fl::e_state = 0;
}
