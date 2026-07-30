#include "ui/curve_editor.hpp"

#include "ui/theme.hpp"
#include "ui/widgets.hpp" // drawAAArcs: the round handles, anti-aliased (fl_pie is not)

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mosaic::ui {
namespace {

Fl_Color toFl(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }

constexpr int kPad = 8;      // inset from the widget edge to the plot
constexpr int kGrab = 7;     // px radius that counts as "on" a control point
constexpr int kHandleR = 4;  // drawn handle radius
constexpr int kGridCells = 4;
// Coverage of the optional backdrop histogram's bars. Low on purpose: it is the distribution you
// are drawing ONTO, not the subject, and the curve has to stay readable where it crosses a peak.
constexpr float kHistoCoverage = 0.38f;

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

// Coverage of an anti-aliased line of half-width `hw` at distance `d` from its centre. A 1px
// feather, the same shoulder the rest of the chrome's software AA uses -- FLTK's fl_line is not
// anti-aliased, and a jagged curve in a settings pane would look exactly as cheap as it is.
float lineCoverage(double d, double hw) {
    const double t = (hw + 0.5) - d;
    return static_cast<float>(std::clamp(t, 0.0, 1.0));
}

// Blend `src` over the RGB triple at `p` with coverage `a`.
void blend(std::uint8_t* p, common::Color8 src, float a) {
    if (a <= 0.0f)
        return;
    p[0] = static_cast<std::uint8_t>(std::lround(p[0] * (1.0f - a) + src.r * a));
    p[1] = static_cast<std::uint8_t>(std::lround(p[1] * (1.0f - a) + src.g * a));
    p[2] = static_cast<std::uint8_t>(std::lround(p[2] * (1.0f - a) + src.b * a));
}

} // namespace

CurveEditor::CurveEditor(int X, int Y, int W, int H, const char* label)
    : Fl_Widget(X, Y, W, H, label) {
    m_points = m_curve.points(); // the identity
}

int CurveEditor::plotX() const noexcept { return x() + kPad; }
int CurveEditor::plotY() const noexcept { return y() + kPad; }
int CurveEditor::plotW() const noexcept { return std::max(1, w() - 2 * kPad); }
int CurveEditor::plotH() const noexcept { return std::max(1, h() - 2 * kPad); }

double CurveEditor::toPixelX(double cx) const noexcept { return plotX() + cx * plotW(); }
double CurveEditor::toPixelY(double cy) const noexcept {
    return plotY() + (1.0 - cy) * plotH(); // y up
}
double CurveEditor::toCurveX(int px) const noexcept {
    return clamp01(static_cast<double>(px - plotX()) / plotW());
}
double CurveEditor::toCurveY(int py) const noexcept {
    return clamp01(1.0 - static_cast<double>(py - plotY()) / plotH());
}

void CurveEditor::setCurve(const core::brush::Curve& c) {
    m_curve = c;
    m_points = m_curve.points();
    m_drag = m_hover = -1;
    redraw();
}

void CurveEditor::setHistogram(std::vector<float> bins01, common::Color8 tint) {
    m_bins = std::move(bins01);
    m_binTint = tint;
    redraw();
}

void CurveEditor::reset() {
    m_curve = core::brush::Curve{};
    m_points = m_curve.points();
    m_drag = m_hover = -1;
    redraw();
    if (m_onChanged)
        m_onChanged(m_curve); // a reset IS an edit -- it must persist like any other
}

void CurveEditor::commit() {
    m_curve = core::brush::Curve(m_points);
    // Curve sorts and de-duplicates; adopt its view so the editor and the curve can never disagree
    // about which point is which (the drag index has to keep meaning something).
    m_points = m_curve.points();
    redraw();
    if (m_onChanged)
        m_onChanged(m_curve);
}

int CurveEditor::pointAt(int px, int py) const noexcept {
    for (int i = 0; i < static_cast<int>(m_points.size()); ++i) {
        const double dx = px - toPixelX(m_points[static_cast<std::size_t>(i)].x);
        const double dy = py - toPixelY(m_points[static_cast<std::size_t>(i)].y);
        if (dx * dx + dy * dy <= static_cast<double>(kGrab) * kGrab)
            return i;
    }
    return -1;
}

void CurveEditor::draw() {
    const Palette& pal = activePalette();
    const bool on = active_r();

    // Erase the whole cell FIRST. The plot is inset by kPad, so the ring outside it belongs to this
    // widget and nothing else will paint it: inside a scrolling pane (Settings -> Tablet) an
    // unpainted ring shows whatever the scroll last had there.
    fl_color(toFl(m_cellColorSet ? m_cellColor : pal.panelBg));
    fl_rectf(x(), y(), w(), h());

    // The plot is composed into an RGB buffer and blitted once: the curve is drawn with software
    // coverage (see lineCoverage), and per-pixel fl_rectf calls would be both jagged and slow.
    const int pw = plotW();
    const int ph = plotH();
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(pw) * ph * 3);

    const common::Color8 bg = pal.controlBg;
    for (std::size_t i = 0; i < buf.size(); i += 3) {
        buf[i] = bg.r;
        buf[i + 1] = bg.g;
        buf[i + 2] = bg.b;
    }
    const auto px = [&](int ix, int iy) -> std::uint8_t* {
        return &buf[(static_cast<std::size_t>(iy) * pw + ix) * 3];
    };

    // The backdrop histogram, under everything else: bars rising from the bottom at a LOW
    // coverage, because it is context -- the grid, the identity diagonal and the curve itself all
    // have to stay legible over it. Empty (the default) draws nothing at all.
    if (!m_bins.empty()) {
        for (int ix = 0; ix < pw; ++ix) {
            const auto bin =
                std::min<std::size_t>(m_bins.size() - 1, static_cast<std::size_t>(ix) *
                                                             m_bins.size() /
                                                             static_cast<std::size_t>(pw));
            const int bar = static_cast<int>(
                std::lround(std::clamp(m_bins[bin], 0.0f, 1.0f) * (ph - 1)));
            for (int iy = ph - 1; iy > ph - 1 - bar; --iy)
                blend(px(ix, iy), m_binTint, kHistoCoverage);
        }
    }

    // Grid + the identity diagonal, both muted: they are a reference, not the subject.
    for (int g = 1; g < kGridCells; ++g) {
        const int gx = g * pw / kGridCells;
        const int gy = g * ph / kGridCells;
        for (int iy = 0; iy < ph; ++iy)
            blend(px(gx, iy), pal.border, 0.55f);
        for (int ix = 0; ix < pw; ++ix)
            blend(px(ix, gy), pal.border, 0.55f);
    }
    for (int ix = 0; ix < pw; ++ix) { // the identity: what "no curve at all" would look like
        const int iy = (ph - 1) - ix * (ph - 1) / std::max(1, pw - 1);
        if (((ix / 4) % 2) == 0) // dashed, so it never reads as the curve itself
            blend(px(ix, iy), pal.textMuted, 0.45f);
    }

    // The curve. Sampled per pixel column, then rasterized as AA segments between the samples --
    // a steep curve covers many rows per column, and plotting one dot per column would comb it.
    const common::Color8 ink = on ? pal.accent : pal.textMuted;
    std::vector<double> ys(static_cast<std::size_t>(pw));
    for (int ix = 0; ix < pw; ++ix) {
        const double cx = static_cast<double>(ix) / std::max(1, pw - 1);
        ys[static_cast<std::size_t>(ix)] = (1.0 - clamp01(m_curve.eval(cx))) * (ph - 1);
    }
    constexpr double kHalfWidth = 0.9;
    for (int ix = 0; ix + 1 < pw; ++ix) {
        const double y0 = ys[static_cast<std::size_t>(ix)];
        const double y1 = ys[static_cast<std::size_t>(ix) + 1];
        const int lo = static_cast<int>(std::floor(std::min(y0, y1) - kHalfWidth - 1.0));
        const int hi = static_cast<int>(std::ceil(std::max(y0, y1) + kHalfWidth + 1.0));
        for (int iy = std::max(0, lo); iy <= std::min(ph - 1, hi); ++iy) {
            // Distance from the pixel centre to the segment (x=ix..ix+1, y=y0..y1).
            const double dy = static_cast<double>(iy) - y0;
            const double sy = y1 - y0;
            const double t = std::clamp(dy * sy / (1.0 + sy * sy), 0.0, 1.0);
            const double ex = t;               // the closest point's x, relative to ix
            const double ey = y0 + t * sy;     // ... and its y
            const double d = std::hypot(ex, static_cast<double>(iy) - ey);
            blend(px(ix, iy), ink, lineCoverage(d, kHalfWidth));
        }
    }
    fl_draw_image(buf.data(), plotX(), plotY(), pw, ph, 3);

    // Frame, then the handles on top. The round handles are composed with software coverage like
    // everything else in this widget: an 8 px fl_pie disc sitting in the middle of a plot that is
    // itself anti-aliased was the last stair-stepped mark here, and on Windows/X11 it always was one
    // (FLTK only anti-aliases arcs on its Cairo backends).
    fl_color(toFl(pal.border));
    fl_rect(plotX(), plotY(), pw, ph);

    // What a handle's patch is painted over: the frame hairline, else the plot buffer just blitted,
    // else the cell ground -- the first and last control points sit ON the plot edge, so their
    // patches straddle all three. NB `px` is this function's plot-buffer accessor, hence ux/uy here.
    const common::Color8 cell = m_cellColorSet ? m_cellColor : pal.panelBg;
    const auto under = [&](int ux, int uy) -> common::Color8 {
        const int lx = ux - plotX();
        const int ly = uy - plotY();
        if (lx < 0 || ly < 0 || lx >= pw || ly >= ph)
            return cell;
        if (lx == 0 || ly == 0 || lx == pw - 1 || ly == ph - 1)
            return pal.border; // the frame, drawn over the buffer's outermost ring
        const std::uint8_t* q = px(lx, ly);
        return {q[0], q[1], q[2], 255};
    };

    for (int i = 0; i < static_cast<int>(m_points.size()); ++i) {
        const auto& p = m_points[static_cast<std::size_t>(i)];
        const int hx = static_cast<int>(std::lround(toPixelX(p.x)));
        const int hy = static_cast<int>(std::lround(toPixelY(p.y)));
        const bool lit = on && (i == m_drag || i == m_hover);
        // A CORNER is a diamond, a smooth point a circle -- the shape says which it is without a
        // legend, and the two behave differently enough that the user has to be able to tell.
        const common::Color8 ink = lit ? pal.accent : pal.text;
        if (p.corner) {
            fl_color(toFl(ink)); // a diamond is not an arc: fl_polygon still owns this one
            fl_begin_polygon();
            fl_vertex(hx, hy - kHandleR - 1);
            fl_vertex(hx + kHandleR + 1, hy);
            fl_vertex(hx, hy + kHandleR + 1);
            fl_vertex(hx - kHandleR - 1, hy);
            fl_end_polygon();
            continue;
        }
        // Disc and hole in ONE patch: the hole is concentric with the disc, so a second patch would
        // have to erase the disc to draw it. The hole keeps a handle over the curve reading as a
        // handle.
        const int inner = kHandleR - 2;
        std::vector<AAArc> parts{aaPieFromBox(hx - kHandleR, hy - kHandleR, 2 * kHandleR,
                                              2 * kHandleR, 0, 360, ink)};
        if (inner > 0)
            parts.push_back(
                aaPieFromBox(hx - inner, hy - inner, 2 * inner, 2 * inner, 0, 360, pal.controlBg));
        drawAAArcs(under, parts);
    }
}

int CurveEditor::handle(int event) {
    if (!active_r())
        return 0;

    switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
        const int was = m_hover;
        m_hover = pointAt(Fl::event_x(), Fl::event_y());
        if (m_hover != was)
            redraw();
        return 1;
    }
    case FL_LEAVE:
        if (m_hover != -1) {
            m_hover = -1;
            redraw();
        }
        return 1;

    case FL_PUSH: {
        const int hit = pointAt(Fl::event_x(), Fl::event_y());

        if (Fl::event_button() == FL_RIGHT_MOUSE) {
            // Remove -- but never an endpoint: the curve's domain must stay [0,1] or every sensor
            // reading outside the surviving span silently clamps to the ends.
            if (hit > 0 && hit + 1 < static_cast<int>(m_points.size())) {
                m_points.erase(m_points.begin() + hit);
                m_drag = m_hover = -1;
                commit();
            }
            return 1;
        }
        if (hit >= 0) {
            if (Fl::event_clicks() > 0) { // double-click: smooth <-> corner
                m_points[static_cast<std::size_t>(hit)].corner =
                    !m_points[static_cast<std::size_t>(hit)].corner;
                m_drag = -1;
                commit();
                return 1;
            }
            m_drag = hit;
            redraw();
            return 1;
        }
        // Empty space: add a point where the click landed, and drag it straight away, so
        // "click, then drag it where you meant" is one gesture and not two.
        core::brush::CurvePoint np;
        np.x = toCurveX(Fl::event_x());
        np.y = toCurveY(Fl::event_y());
        m_points.push_back(np);
        commit();
        // Re-derive the drag index from the curve's OWN view. commit() rebuilds m_points through
        // Curve, which sorts and drops any point sharing an x with an earlier one -- so an index
        // computed against the pre-commit vector (or a float-equality search for `np`) is exactly
        // the kind of thing that silently drags the wrong point.
        m_drag = -1;
        double best = 0.0;
        for (int i = 0; i < static_cast<int>(m_points.size()); ++i) {
            const double d = std::abs(m_points[static_cast<std::size_t>(i)].x - np.x);
            if (m_drag < 0 || d < best) {
                best = d;
                m_drag = i;
            }
        }
        return 1;
    }

    case FL_DRAG: {
        if (m_drag < 0 || m_drag >= static_cast<int>(m_points.size()))
            return 1;
        auto& p = m_points[static_cast<std::size_t>(m_drag)];
        const bool isEnd = (m_drag == 0) || (m_drag + 1 == static_cast<int>(m_points.size()));
        p.y = toCurveY(Fl::event_y());
        if (!isEnd) {
            // Keep it strictly between its neighbours. Curve() DROPS a point that shares an x with
            // an earlier one, so letting the drag land on a neighbour's x would delete the point
            // under the cursor mid-gesture.
            const double lo = m_points[static_cast<std::size_t>(m_drag) - 1].x;
            const double hi = m_points[static_cast<std::size_t>(m_drag) + 1].x;
            constexpr double kEps = 1e-4;
            p.x = std::clamp(toCurveX(Fl::event_x()), lo + kEps, hi - kEps);
        }
        // Endpoints keep their x (0 and 1) and move in y only -- see the header.
        commit();
        return 1;
    }

    case FL_RELEASE:
        m_drag = -1;
        redraw();
        return 1;

    default:
        break;
    }
    return Fl_Widget::handle(event);
}

} // namespace mosaic::ui
