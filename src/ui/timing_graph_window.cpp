#include "ui/timing_graph_window.hpp"


#include "common/profiler.hpp" // the collector (moved down out of ui/ in S60-a); this is its face
#include "ui/theme.hpp"

#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mosaic::ui {
namespace {

constexpr int kPad = 12;      // window margin
constexpr int kHeaderH = 54;  // header band (FPS + column titles start below it)
constexpr int kRowH = 21;     // table row height
constexpr int kColHeadH = 20; // column-title row

// Column widths (Operation flexes to fill the rest). Lane is a small tag; the four stat columns and
// the count are fixed-width and right-aligned so the numbers line up down the table.
constexpr int kLaneW = 46;
constexpr int kNumW = 58;
constexpr int kCountW = 54;
constexpr int kNumInset = 8; // right padding inside a numeric cell so columns do not touch

Fl_Color toFl(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }

// Linear blend a -> b by t (clamped). Used for the decaying row highlight.
common::Color8 mix(common::Color8 a, common::Color8 b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    const auto L = [&](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::lround(x + (static_cast<double>(y) - x) * t));
    };
    return {L(a.r, b.r), L(a.g, b.g), L(a.b, b.b), 255};
}

// Format a millisecond value into a compact, aligned string (1-2 decimals depending on magnitude).
void fmtMs(char* buf, std::size_t n, double ms) {
    if (ms >= 100.0)
        std::snprintf(buf, n, "%.0f", ms);
    else if (ms >= 10.0)
        std::snprintf(buf, n, "%.1f", ms);
    else
        std::snprintf(buf, n, "%.2f", ms);
}

} // namespace

TimingGraphWindow::TimingGraphWindow() : Fl_Double_Window(600, 340, "Mosaic - Timing Profiler") {
    // A plain top-level diagnostic window: non-modal, resizable, hides (not closes) on the WM button
    // so the MainWindow can reopen the same instance and hide it on quit.
    callback([](Fl_Widget* w, void*) { w->hide(); });
    size_range(460, 200);
    resizable(this);
    end();
}

void TimingGraphWindow::draw() {
    const Palette& pal = activePalette();

    // draw() must paint its own background (FLTK does not for a custom top-level draw()).
    fl_color(toFl(pal.panelBg));
    fl_rectf(0, 0, w(), h());

    const common::Profiler& prof = common::Profiler::instance();
    const double fps = prof.fps();
    const double frameMs = prof.frameMs();
    const std::vector<common::ProfileRow> rows = prof.snapshot();

    // ---- Header ---------------------------------------------------------------------------------
    fl_font(FL_HELVETICA_BOLD, 16);
    fl_color(toFl(pal.text));
    char head[64];
    std::snprintf(head, sizeof head, "%d FPS", static_cast<int>(fps + 0.5));
    fl_draw(head, kPad, kPad + 14);

    fl_font(FL_HELVETICA, 12);
    fl_color(toFl(pal.textMuted));
    char sub[96];
    std::snprintf(sub, sizeof sub, "%.1f ms/frame", frameMs);
    fl_draw(sub, kPad + 78, kPad + 14);
    fl_draw("slowest first; recent spikes glow", kPad, kPad + 32);

    // ---- Column geometry ------------------------------------------------------------------------
    const int x0 = kPad;
    const int right = w() - kPad;
    const int opX = x0;
    const int countX = right - kCountW;
    const int maxX = countX - kNumW;
    const int minX = maxX - kNumW;
    const int avgX = minX - kNumW;
    const int lastX = avgX - kNumW;
    const int laneX = lastX - kLaneW - 6;
    const int opW = laneX - opX - 6;

    // Column titles.
    int y = kHeaderH;
    fl_font(FL_HELVETICA_BOLD, 11);
    fl_color(toFl(pal.textMuted));
    fl_draw("Operation", opX, y, opW, kColHeadH, FL_ALIGN_LEFT);
    fl_draw("Lane", laneX, y, kLaneW, kColHeadH, FL_ALIGN_CENTER);
    fl_draw("last", lastX, y, kNumW - kNumInset, kColHeadH, FL_ALIGN_RIGHT);
    fl_draw("avg", avgX, y, kNumW - kNumInset, kColHeadH, FL_ALIGN_RIGHT);
    fl_draw("min", minX, y, kNumW - kNumInset, kColHeadH, FL_ALIGN_RIGHT);
    fl_draw("max", maxX, y, kNumW - kNumInset, kColHeadH, FL_ALIGN_RIGHT);
    fl_draw("count", countX, y, kCountW - kNumInset, kColHeadH, FL_ALIGN_RIGHT);
    y += kColHeadH;
    fl_color(toFl(pal.border));
    fl_line(x0, y, right, y);
    y += 2;

    if (rows.empty()) {
        fl_font(FL_HELVETICA_ITALIC, 12);
        fl_color(toFl(pal.textMuted));
        fl_draw("no operations recorded yet -- composite or render something", x0, y + 20);
        return;
    }

    // ---- Rows (slowest first) -------------------------------------------------------------------
    const common::Color8 warm{214, 108, 70}; // the decaying "recently-slow" highlight tint
    for (const common::ProfileRow& r : rows) {
        if (y + kRowH > h() - kPad)
            break; // clip to the window; the slowest are already at the top
        // Decaying highlight: a recently-fired-and-slow row glows and fades over a few seconds.
        if (r.heat > 0.02) {
            fl_color(toFl(mix(pal.panelBg, warm, r.heat)));
            fl_rectf(x0, y, right - x0, kRowH);
        }

        // Operation name (clipped to its column).
        fl_font(FL_HELVETICA, 12);
        fl_color(toFl(pal.text));
        fl_push_clip(opX, y, opW, kRowH);
        fl_draw(r.name.c_str(), opX, y, opW, kRowH, FL_ALIGN_LEFT);
        fl_pop_clip();

        // Lane tag: a small filled pill, coloured per lane, with the lane name in white. DEV is
        // real device time from a timestamp query (S60-a); a GPU row and a DEV row sharing a name
        // are the same operation measured at the call site and on the device, and the gap between
        // them is host + fence cost. See common/profiler.hpp's three-lane note.
        const common::Color8 laneCol = r.lane == common::Lane::Gpu ? common::Color8{74, 176, 130, 255}
                                       : r.lane == common::Lane::GpuDevice
                                           ? common::Color8{176, 138, 70, 255}
                                           : common::Color8{86, 128, 214, 255};
        const int tagH = 15;
        const int tagY = y + (kRowH - tagH) / 2;
        fl_color(toFl(laneCol));
        fl_rectf(laneX, tagY, kLaneW, tagH);
        fl_font(FL_HELVETICA_BOLD, 10);
        fl_color(FL_WHITE);
        fl_draw(common::laneName(r.lane), laneX, tagY, kLaneW, tagH, FL_ALIGN_CENTER);

        // Stat columns, right-aligned.
        fl_font(FL_HELVETICA, 12);
        fl_color(toFl(pal.text));
        char buf[32];
        fmtMs(buf, sizeof buf, r.last);
        fl_draw(buf, lastX, y, kNumW - kNumInset, kRowH, FL_ALIGN_RIGHT);
        fl_color(toFl(pal.textMuted));
        fmtMs(buf, sizeof buf, r.avg);
        fl_draw(buf, avgX, y, kNumW - kNumInset, kRowH, FL_ALIGN_RIGHT);
        fmtMs(buf, sizeof buf, r.min);
        fl_draw(buf, minX, y, kNumW - kNumInset, kRowH, FL_ALIGN_RIGHT);
        fl_color(toFl(pal.text));
        fmtMs(buf, sizeof buf, r.max);
        fl_draw(buf, maxX, y, kNumW - kNumInset, kRowH, FL_ALIGN_RIGHT);
        fl_color(toFl(pal.textMuted));
        std::snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(r.count));
        fl_draw(buf, countX, y, kCountW - kNumInset, kRowH, FL_ALIGN_RIGHT);

        y += kRowH;
    }
}

} // namespace mosaic::ui

