#include "ui/ruler.hpp"

#include "ui/canvas_view.hpp"
#include "ui/theme.hpp"
#include "ui/vulkan_canvas.hpp"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mosaic::ui {

namespace {

Fl_Color col8(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }

// The 1-2-5 "nice" step at or just above `raw` -- the tick spacing so majors land on round pixel
// values (10, 20, 50, 100 ...). Never below 1 (the ruler's unit is px; sub-pixel labels read as junk).
double niceStep(double raw) {
    if (!(raw > 0.0))
        return 1.0;
    const double p = std::pow(10.0, std::floor(std::log10(raw)));
    const double f = raw / p; // 1 .. 10
    const double nice = f <= 1.0 ? 1.0 : f <= 2.0 ? 2.0 : f <= 5.0 ? 5.0 : 10.0;
    return std::max(1.0, nice * p);
}

} // namespace

RulerStrip::RulerStrip(int x, int y, int w, int h, Orientation orientation, VulkanCanvas* canvas)
    : Fl_Widget(x, y, w, h), m_orientation(orientation), m_canvas(canvas) {}

void RulerStrip::setCursor(double docX, double docY, bool over) {
    m_cursorDoc = {docX, docY};
    m_cursorOver = over;
    redraw();
}

double RulerStrip::eventDocPos(bool& overCanvas) const {
    // Canvas-widget-local logical px: the canvas is a child window whose x()/y() sit in the same
    // top-level coordinate space FLTK reports events in, so subtract its origin.
    const double lx = static_cast<double>(Fl::event_x() - m_canvas->x());
    const double ly = static_cast<double>(Fl::event_y() - m_canvas->y());
    overCanvas = lx >= 0.0 && lx < static_cast<double>(m_canvas->w()) && ly >= 0.0 &&
                 ly < static_cast<double>(m_canvas->h());
    const common::Vec2 doc = m_canvas->view().toDoc({lx, ly});
    return m_orientation == Orientation::Horizontal ? doc.y : doc.x;
}

void RulerStrip::draw() {
    const Palette& pal = activePalette();
    // Background + the hairline seam against the canvas (the inner edge).
    fl_color(col8(pal.panelBg));
    fl_rectf(x(), y(), w(), h());
    fl_color(col8(pal.border));
    if (m_orientation == Orientation::Horizontal)
        fl_xyline(x(), y() + h() - 1, x() + w() - 1);
    else
        fl_yxline(x() + w() - 1, y(), y() + h() - 1);

    const CanvasView& v = m_canvas->view();
    const double pxPerDoc = v.zoom();
    if (!(pxPerDoc > 0.0) || m_canvas->w() <= 0 || m_canvas->h() <= 0)
        return;

    const double majorStep = niceStep(72.0 / pxPerDoc);
    const double minorStep = majorStep / 5.0;
    const bool horizontal = m_orientation == Orientation::Horizontal;

    // The document range this strip spans (its screen extent maps 1:1 to the canvas's). Rotation is
    // not modelled here -- rulers assume the common axis-aligned view (a rotated view still draws,
    // just approximately).
    const double s0 = horizontal ? v.toDoc({0.0, 0.0}).x : v.toDoc({0.0, 0.0}).y;
    const double s1 = horizontal ? v.toDoc({static_cast<double>(m_canvas->w()), 0.0}).x
                                 : v.toDoc({0.0, static_cast<double>(m_canvas->h())}).y;
    const double lo = std::min(s0, s1);
    const double hi = std::max(s0, s1);

    const long iFirst = static_cast<long>(std::floor(lo / minorStep));
    const long iLast = static_cast<long>(std::ceil(hi / minorStep));
    if (iLast - iFirst > 20000)
        return; // pathological zoom -- draw nothing rather than churn

    fl_font(FL_HELVETICA, 8);
    const int origin = horizontal ? m_canvas->x() : m_canvas->y();
    const double majorLen = kRulerSize * 0.55;
    const double minorLen = kRulerSize * 0.30;
    char buf[32];
    for (long i = iFirst; i <= iLast; ++i) {
        const double d = static_cast<double>(i) * minorStep;
        const bool major = (((i % 5) + 5) % 5) == 0;
        const common::Vec2 sc = horizontal ? v.toScreen({d, 0.0}) : v.toScreen({0.0, d});
        const int p = origin + static_cast<int>(std::lround(horizontal ? sc.x : sc.y));
        const int len = static_cast<int>(std::lround(major ? majorLen : minorLen));
        fl_color(col8(pal.textMuted));
        if (horizontal) {
            if (p < x() - 1 || p > x() + w())
                continue;
            fl_line(p, y() + h() - 1 - len, p, y() + h() - 1);
        } else {
            if (p < y() - 1 || p > y() + h())
                continue;
            fl_line(x() + w() - 1 - len, p, x() + w() - 1, p);
        }
        if (major) {
            std::snprintf(buf, sizeof(buf), "%ld", std::lround(d));
            fl_color(col8(pal.text));
            if (horizontal)
                fl_draw(buf, p + 2, y() + 8);
            else
                fl_draw(90, buf, x() + 8, p - 2); // rotated, reading bottom-to-top
        }
    }

    // The moving cursor tick: a thin accent line across the strip at the pointer's position.
    if (m_cursorOver) {
        const common::Vec2 sc = v.toScreen(m_cursorDoc);
        fl_color(col8(pal.accent));
        if (horizontal) {
            const int p = m_canvas->x() + static_cast<int>(std::lround(sc.x));
            if (p >= x() && p <= x() + w() - 1)
                fl_line(p, y(), p, y() + h() - 1);
        } else {
            const int p = m_canvas->y() + static_cast<int>(std::lround(sc.y));
            if (p >= y() && p <= y() + h() - 1)
                fl_line(x(), p, x() + w() - 1, p);
        }
    }
}

int RulerStrip::handle(int event) {
    // Drag OUT of the ruler to pull a new guide onto the canvas. Inert until the guide feature sets
    // the hooks. The strip owns the whole PUSH/DRAG/RELEASE gesture (FLTK grabs the mouse for the
    // widget the press landed on), converting event coords to a document position each step.
    switch (event) {
    case FL_PUSH:
        if (!onGuideBegin)
            return 0; // no guide wiring: let the press fall through
        {
            bool over = false;
            const double pos = eventDocPos(over);
            m_dragging = true;
            onGuideBegin(m_orientation == Orientation::Horizontal, pos);
        }
        return 1;
    case FL_DRAG:
        if (m_dragging && onGuideUpdate) {
            bool over = false;
            onGuideUpdate(eventDocPos(over));
        }
        return 1;
    case FL_RELEASE:
        if (m_dragging) {
            m_dragging = false;
            bool over = false;
            static_cast<void>(eventDocPos(over)); // only the over-canvas flag matters here
            if (onGuideEnd)
                onGuideEnd(/*cancel=*/!over); // dropped back on the ruler/gutter = cancel
        }
        return 1;
    default:
        return 0;
    }
}

} // namespace mosaic::ui
