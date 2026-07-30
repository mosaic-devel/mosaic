#include "ui/export_preview.hpp"

#include "common/i18n.hpp"
#include "ui/widgets.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace mosaic::ui {

namespace {

Fl_Color toFl(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }

// The transparency checkerboard, matched to render::CompositeOptions' defaults and to
// ui::PreviewPane so the two previews read as the same surface. The cells live in WIDGET space,
// not image space -- that is what every editor does, and it keeps the checker from turning into
// giant slabs at 32x.
constexpr int kCell = 8;
constexpr double kCheckLight = 255.0;
constexpr double kCheckDark = 205.0;

// Box-filter budget per destination pixel when minifying. 4x4 is enough to kill the shimmer a
// point sample shows on a photo at fit scale, and it bounds the cost of a recompose at
// (viewport px x 16) regardless of how large the exported image is. At scale >= 1 the box is
// under one source pixel wide, so this collapses to a point sample -- which is exactly right for
// pixel peeping: 1:1 must show pixels, not a blur.
constexpr int kMaxSamples = 4;

constexpr int kBtnSize = 28;  // the corner zoom buttons
constexpr int kBtnGap = 6;
constexpr int kBtnInset = 12;

std::uint8_t to8(double v) {
    return static_cast<std::uint8_t>(std::clamp(v, 0.0, 255.0) + 0.5);
}

// The two zoom buttons. Their marks are DRAWN -- bars and strokes placed by hand, never a font
// glyph and never a Unicode symbol (the standing no-glyph-in-labels rule): the host font may not
// carry the character, and a preview button that renders as tofu is worse than no button.
// The same discipline ui::GlyphButton follows for B/I/U/S.
class ZoomGlyphButton : public FlatButton {
public:
    enum class Kind { ActualPixels, FitToWindow };

    ZoomGlyphButton(int X, int Y, int W, int H, Kind kind) : FlatButton(X, Y, W, H), m_kind(kind) {}

protected:
    void draw() override {
        Fl_Button::draw(); // the themed rounded box + hover / pressed fills
        fl_color(toFl(active_r() ? activePalette().text : activePalette().textMuted));
        if (m_kind == Kind::ActualPixels)
            drawOneToOne();
        else
            drawFitBrackets();
    }

private:
    // "1:1": two drawn digit-ones (stem + top flag + base serif) around a two-square colon.
    void drawOneToOne() {
        constexpr int kDigitW = 7, kDigitH = 11, kStroke = 2, kColonGap = 5;
        const int total = kDigitW * 2 + kColonGap + kStroke;
        const int left = x() + (w() - total) / 2;
        const int top = y() + (h() - kDigitH) / 2;
        const auto digitOne = [&](int dx) {
            fl_rectf(dx + 3, top, kStroke, kDigitH - kStroke);       // stem
            fl_rectf(dx, top + kDigitH - kStroke, kDigitW, kStroke); // base serif
            fl_line_style(FL_SOLID | FL_CAP_ROUND, 1);
            fl_line(dx + 1, top + 3, dx + 3, top + 1); // the flag
            fl_line_style(0);
        };
        digitOne(left);
        const int colonX = left + kDigitW + (kColonGap - kStroke) / 2;
        fl_rectf(colonX, top + 3, kStroke, kStroke);
        fl_rectf(colonX, top + kDigitH - 3 - kStroke, kStroke, kStroke);
        digitOne(left + kDigitW + kColonGap + kStroke);
    }

    // Reset zoom: four corner brackets opening outward -- the settled "fit to window" mark.
    void drawFitBrackets() {
        constexpr int kW = 16, kH = 12, kArm = 5, kStroke = 2;
        const int x0 = x() + (w() - kW) / 2;
        const int y0 = y() + (h() - kH) / 2;
        const int x1 = x0 + kW;
        const int y1 = y0 + kH;
        fl_rectf(x0, y0, kArm, kStroke);
        fl_rectf(x0, y0, kStroke, kArm);
        fl_rectf(x1 - kArm, y0, kArm, kStroke);
        fl_rectf(x1 - kStroke, y0, kStroke, kArm);
        fl_rectf(x0, y1 - kStroke, kArm, kStroke);
        fl_rectf(x0, y1 - kArm, kStroke, kArm);
        fl_rectf(x1 - kArm, y1 - kStroke, kArm, kStroke);
        fl_rectf(x1 - kStroke, y1 - kArm, kStroke, kArm);
    }

    Kind m_kind;
};

// A small plate behind an overlay caption, so the text reads over any artwork underneath.
void drawPlate(int px, int py, int pw, int ph, const Palette& p) {
    fl_color(toFl(p.panelBg));
    fl_rectf(px, py, pw, ph);
    fl_color(toFl(p.border));
    fl_rect(px, py, pw, ph);
}

} // namespace

// ---- the pure zoom/pan model -----------------------------------------------------------------

double previewFitScale(int imageW, int imageH, int viewW, int viewH) {
    if (imageW <= 0 || imageH <= 0 || viewW <= 0 || viewH <= 0)
        return 0.0;
    return std::min(static_cast<double>(viewW) / imageW, static_cast<double>(viewH) / imageH);
}

double previewMinScale(double fitScale) noexcept {
    if (!(fitScale > 0.0))
        return 1.0;
    return std::min(fitScale, 1.0);
}

double previewMaxScale(double fitScale) noexcept {
    if (!(fitScale > 0.0))
        return kPreviewMaxZoom;
    return std::max(fitScale, kPreviewMaxZoom);
}

double clampPreviewScale(double scale, double fitScale) noexcept {
    if (!(scale > 0.0) || !std::isfinite(scale))
        return previewMinScale(fitScale);
    return std::clamp(scale, previewMinScale(fitScale), previewMaxScale(fitScale));
}

PreviewTransform previewFitTransform(int imageW, int imageH, int viewW, int viewH) {
    PreviewTransform t;
    t.scale = previewFitScale(imageW, imageH, viewW, viewH);
    if (!(t.scale > 0.0)) {
        t.scale = 1.0;
        t.offsetX = t.offsetY = 0.0;
        return t;
    }
    t.offsetX = (viewW - imageW * t.scale) * 0.5;
    t.offsetY = (viewH - imageH * t.scale) * 0.5;
    return t;
}

PreviewTransform previewZoomAt(const PreviewTransform& t, double newScale, double cursorX,
                               double cursorY) {
    PreviewTransform out = t;
    if (!(t.scale > 0.0) || !(newScale > 0.0))
        return out;
    // The image point under the cursor, before ...
    const double imgX = (cursorX - t.offsetX) / t.scale;
    const double imgY = (cursorY - t.offsetY) / t.scale;
    out.scale = newScale;
    // ... and pinned to the same widget point after.
    out.offsetX = cursorX - imgX * newScale;
    out.offsetY = cursorY - imgY * newScale;
    return out;
}

PreviewTransform clampPreviewPan(const PreviewTransform& t, int imageW, int imageH, int viewW,
                                 int viewH) {
    PreviewTransform out = t;
    const auto axis = [](double offset, double extent, double view) {
        if (extent <= view)
            return (view - extent) * 0.5; // smaller than the viewport: centred, no pan to be had
        return std::clamp(offset, view - extent, 0.0);
    };
    out.offsetX = axis(t.offsetX, imageW * t.scale, viewW);
    out.offsetY = axis(t.offsetY, imageH * t.scale, viewH);
    return out;
}

double previewWheelScale(double scale, int wheelSteps) noexcept {
    if (!(scale > 0.0))
        return scale;
    // FLTK's dy is positive for a wheel-down, which zooms OUT.
    return scale * std::pow(kPreviewZoomStep, -static_cast<double>(wheelSteps));
}

// ---- the widget --------------------------------------------------------------------------------

ExportPreview::ExportPreview(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    box(FL_NO_BOX);
    // Children are corner-anchored by layoutButtons(); FLTK's proportional child rescale would
    // only fight it (the new-document dialog's "size the group before adding" trap, from the
    // other end).
    resizable(nullptr);
    begin();
    auto* actual = new ZoomGlyphButton(0, 0, kBtnSize, kBtnSize, ZoomGlyphButton::Kind::ActualPixels);
    actual->tooltip(_("Actual pixels (100%)"));
    actual->callback([](Fl_Widget* w, void*) {
        static_cast<ExportPreview*>(w->parent())->actualPixels();
    });
    auto* fit = new ZoomGlyphButton(0, 0, kBtnSize, kBtnSize, ZoomGlyphButton::Kind::FitToWindow);
    fit->tooltip(_("Reset zoom (fit to window)"));
    fit->callback([](Fl_Widget* w, void*) { static_cast<ExportPreview*>(w->parent())->fitToView(); });
    end();
    m_btnActual = actual;
    m_btnFit = fit;
    m_btnActual->hide(); // no picture, no zoom controls
    m_btnFit->hide();
    layoutButtons();
    m_themeSub = ThemeSubscription([this] { reapplyTheme(); });
}

void ExportPreview::layoutButtons() {
    if (m_btnFit == nullptr || m_btnActual == nullptr)
        return;
    const int by = y() + h() - kBtnInset - kBtnSize;
    const int fx = x() + w() - kBtnInset - kBtnSize;
    m_btnFit->position(fx, by);
    m_btnActual->position(fx - kBtnGap - kBtnSize, by);
}

void ExportPreview::reapplyTheme() {
    m_bufValid = false;
    redraw();
}

void ExportPreview::setNote(std::string note) {
    m_note = std::move(note);
    if (!m_hasImage)
        redraw();
}

void ExportPreview::setBusy(bool busy) {
    if (busy == m_busy)
        return;
    m_busy = busy;
    redraw(); // the pill is overlay chrome; the composed buffer is untouched
}

void ExportPreview::setImage(const common::Image& rgba) {
    if (rgba.empty()) {
        clearImage();
        return;
    }
    const bool sameSize = m_hasImage && m_imgW == static_cast<int>(rgba.width) &&
                          m_imgH == static_cast<int>(rgba.height);
    // Premultiply once: scaling premultiplied RGBA interpolates coverage correctly (no dark or
    // bright fringe at a soft edge), and compositing it over the checker is a plain
    // rgb + base * (1 - a).
    m_premul = common::Image(rgba.width, rgba.height);
    for (std::size_t i = 0; i < rgba.rgba.size(); i += 4) {
        const float a = rgba.rgba[i + 3] / 255.0f;
        m_premul.rgba[i] = static_cast<std::uint8_t>(std::lround(rgba.rgba[i] * a));
        m_premul.rgba[i + 1] = static_cast<std::uint8_t>(std::lround(rgba.rgba[i + 1] * a));
        m_premul.rgba[i + 2] = static_cast<std::uint8_t>(std::lround(rgba.rgba[i + 2] * a));
        m_premul.rgba[i + 3] = rgba.rgba[i + 3];
    }
    m_imgW = static_cast<int>(rgba.width);
    m_imgH = static_cast<int>(rgba.height);
    m_hasImage = true;
    m_btnActual->show();
    m_btnFit->show();
    // A same-size refresh (a quality slider tick re-encoding) must not throw the user out of the
    // detail they were inspecting; a size change means it is a different picture, so refit.
    if (!sameSize)
        fitToView();
    else
        applyView(clampPreviewPan(m_view, m_imgW, m_imgH, w(), h()), m_userZoomed);
    m_bufValid = false;
    redraw();
}

void ExportPreview::clearImage() {
    m_hasImage = false;
    m_premul = {};
    m_imgW = m_imgH = 0;
    m_panning = false;
    if (m_btnActual != nullptr)
        m_btnActual->hide();
    if (m_btnFit != nullptr)
        m_btnFit->hide();
    m_bufValid = false;
    redraw();
}

void ExportPreview::applyView(const PreviewTransform& t, bool userDriven) {
    m_view = t;
    m_userZoomed = userDriven;
    m_bufValid = false;
    redraw();
}

void ExportPreview::fitToView() {
    applyView(previewFitTransform(m_imgW, m_imgH, w(), h()), /*userDriven=*/false);
}

void ExportPreview::actualPixels() {
    if (!m_hasImage)
        return;
    const double fit = previewFitScale(m_imgW, m_imgH, w(), h());
    const double target = clampPreviewScale(1.0, fit);
    PreviewTransform t = previewZoomAt(m_view, target, w() * 0.5, h() * 0.5);
    applyView(clampPreviewPan(t, m_imgW, m_imgH, w(), h()), /*userDriven=*/true);
}

void ExportPreview::resize(int X, int Y, int W, int H) {
    // Fl_Widget::resize, not Fl_Group::resize: the children are corner-anchored furniture that
    // layoutButtons() places, never content to be rescaled with the pane.
    Fl_Widget::resize(X, Y, W, H);
    layoutButtons();
    if (m_hasImage) {
        if (m_userZoomed)
            m_view = clampPreviewPan(m_view, m_imgW, m_imgH, w(), h());
        else
            m_view = previewFitTransform(m_imgW, m_imgH, w(), h());
    }
    m_bufValid = false;
}

void ExportPreview::compose() {
    m_bufValid = true;
    const int W = w(), H = h();
    if (W <= 0 || H <= 0) {
        m_buf.clear();
        return;
    }
    const Palette& p = activePalette();
    m_buf.assign(static_cast<std::size_t>(W) * H * 3, 0);
    const common::Color8 back = p.canvasBg; // the surround, as behind the main canvas
    for (std::size_t i = 0; i < static_cast<std::size_t>(W) * H; ++i) {
        m_buf[i * 3] = back.r;
        m_buf[i * 3 + 1] = back.g;
        m_buf[i * 3 + 2] = back.b;
    }
    if (!m_hasImage || m_imgW <= 0 || m_imgH <= 0 || !(m_view.scale > 0.0))
        return;

    const double s = m_view.scale;
    const double ox = m_view.offsetX;
    const double oy = m_view.offsetY;
    const int dx0 = std::max(0, static_cast<int>(std::floor(ox)));
    const int dy0 = std::max(0, static_cast<int>(std::floor(oy)));
    const int dx1 = std::min(W, static_cast<int>(std::ceil(ox + m_imgW * s)));
    const int dy1 = std::min(H, static_cast<int>(std::ceil(oy + m_imgH * s)));
    const double inv = 1.0 / s;
    const std::uint8_t* src = m_premul.rgba.data();

    for (int dy = dy0; dy < dy1; ++dy) {
        const double v0 = (dy - oy) * inv;
        const double v1 = (dy + 1 - oy) * inv;
        const int ny = std::clamp(static_cast<int>(std::ceil(v1 - v0)), 1, kMaxSamples);
        for (int dx = dx0; dx < dx1; ++dx) {
            const double u0 = (dx - ox) * inv;
            const double u1 = (dx + 1 - ox) * inv;
            const int nx = std::clamp(static_cast<int>(std::ceil(u1 - u0)), 1, kMaxSamples);
            double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
            int taken = 0;
            for (int j = 0; j < ny; ++j) {
                const int sy = static_cast<int>(std::floor(v0 + (j + 0.5) * (v1 - v0) / ny));
                if (sy < 0 || sy >= m_imgH)
                    continue;
                for (int i = 0; i < nx; ++i) {
                    const int sx = static_cast<int>(std::floor(u0 + (i + 0.5) * (u1 - u0) / nx));
                    if (sx < 0 || sx >= m_imgW)
                        continue;
                    const std::uint8_t* q =
                        src + (static_cast<std::size_t>(sy) * m_imgW + sx) * 4;
                    r += q[0];
                    g += q[1];
                    b += q[2];
                    a += q[3];
                    ++taken;
                }
            }
            if (taken == 0)
                continue; // a fractional edge pixel with no source under it: leave the surround
            const double n = 1.0 / taken;
            r *= n;
            g *= n;
            b *= n;
            a *= n;
            const bool dark = (((dx / kCell) + (dy / kCell)) & 1) != 0;
            const double base = dark ? kCheckDark : kCheckLight;
            const double clear = 1.0 - a / 255.0;
            const std::size_t o = (static_cast<std::size_t>(dy) * W + dx) * 3;
            m_buf[o] = to8(r + base * clear); // premultiplied over the checker
            m_buf[o + 1] = to8(g + base * clear);
            m_buf[o + 2] = to8(b + base * clear);
        }
    }
}

void ExportPreview::draw() {
    const int W = w(), H = h();
    if (W <= 0 || H <= 0)
        return;
    const Palette& p = activePalette();

    if (!m_hasImage) {
        // A widget owns every pixel of its rect: erase before any text (the double buffer keeps
        // the previous frame otherwise, and unerased text thickens on every repaint).
        fl_color(toFl(p.canvasBg));
        fl_rectf(x(), y(), W, H);
        if (!m_note.empty()) {
            fl_color(toFl(p.textMuted));
            fl_font(FL_HELVETICA, 12);
            fl_draw(m_note.c_str(), x() + 12, y(), W - 24, H, FL_ALIGN_CENTER | FL_ALIGN_WRAP,
                    nullptr, 0);
        }
        return;
    }

    if (!m_bufValid)
        compose();
    if (!m_buf.empty()) {
        // Depth 3, always: an RGBA fl_draw_image / Fl_RGB_Image blit is read inconsistently
        // across backends (the standing "never blit depth 4" rule), and the checker is opaque
        // anyway. The image is a stack temporary because ~Fl_RGB_Image uncaches through the
        // graphics driver -- a cached one must never outlive FLTK's statics.
        Fl_RGB_Image out(m_buf.data(), W, H, 3);
        out.draw(x(), y());
    }

    fl_push_clip(x(), y(), W, H);
    // The exported picture's own edge: under zoom the checker fills the whole pane, so without
    // this hairline there is no telling where the file ends and the surround begins.
    fl_color(toFl(p.border));
    fl_rect(x() + static_cast<int>(std::lround(m_view.offsetX)) - 1,
            y() + static_cast<int>(std::lround(m_view.offsetY)) - 1,
            static_cast<int>(std::lround(m_imgW * m_view.scale)) + 2,
            static_cast<int>(std::lround(m_imgH * m_view.scale)) + 2);

    // The zoom readout, lower-left, on its own plate (digits and '%' only -- no symbol font).
    char pct[32];
    std::snprintf(pct, sizeof pct, "%d%%",
                  static_cast<int>(std::lround(m_view.scale * 100.0)));
    fl_font(FL_HELVETICA, 11);
    const int pw = static_cast<int>(std::ceil(fl_width(pct))) + 14;
    const int px = x() + kBtnInset;
    const int py = y() + h() - kBtnInset - 20;
    drawPlate(px, py, pw, 20, p);
    fl_color(toFl(p.textMuted));
    fl_draw(pct, px, py, pw, 20, FL_ALIGN_CENTER, nullptr, 0);

    if (m_busy) { // the last good frame stays up; this says a fresher one is on the way
        const char* msg = _("Rendering…");
        fl_font(FL_HELVETICA, 11);
        const int bw = static_cast<int>(std::ceil(fl_width(msg))) + 18;
        drawPlate(x() + kBtnInset, y() + kBtnInset, bw, 20, p);
        fl_color(toFl(p.text));
        fl_draw(msg, x() + kBtnInset, y() + kBtnInset, bw, 20, FL_ALIGN_CENTER, nullptr, 0);
    }
    fl_pop_clip();

    // Draw the corner buttons EXPLICITLY rather than through draw_children(): a hover on one of
    // them arrives as FL_DAMAGE_CHILD, and draw_children() would then repaint only that child --
    // but we have just repainted the whole pane underneath it, so both must go back down.
    if (m_btnActual != nullptr && m_btnActual->visible())
        draw_child(*m_btnActual);
    if (m_btnFit != nullptr && m_btnFit->visible())
        draw_child(*m_btnFit);
}

bool ExportPreview::overButton(int ex, int ey) const {
    const auto inside = [ex, ey](const Fl_Widget* wid) {
        return wid != nullptr && wid->visible() && ex >= wid->x() && ex < wid->x() + wid->w() &&
               ey >= wid->y() && ey < wid->y() + wid->h();
    };
    return inside(m_btnActual) || inside(m_btnFit);
}

void ExportPreview::setCursorFor(int ex, int ey) {
    if (window() == nullptr)
        return;
    if (!m_hasImage || overButton(ex, ey)) {
        window()->cursor(FL_CURSOR_DEFAULT);
        return;
    }
    // ⚠ NOT the stock FL_CURSOR_MOVE on Wayland. FLTK resolves it by the legacy Xcursor name
    // `move`, and breeze_cursors -- the KDE default -- symlinks `move` to `dnd-move`: a CLOSED
    // GRABBING HAND carrying a dead-centre hotspot. The art appears to point with its fingertips,
    // so the click lands ~10 px from where the cursor looks like it is aiming -- the standing
    // "chrome cursor hotspot is offset" family (docs/wayland.md §2.5). X11 asks for XC_fleur and
    // gets a properly centred four-way arrow, so the substitution is Wayland-only. ui::MoveCursor
    // owns the decision, the rasterized cache and the stock fallback, and builds at the window's
    // buffer scale -- so this dialog gets the same crisp, correctly-aimed pointer the canvas does.
    m_moveCursor.apply(window());
}

int ExportPreview::handle(int event) {
    switch (event) {
    case FL_MOUSEWHEEL: {
        // Decline the wheel when the cursor is elsewhere: Fl_Group offers it to every child, not
        // just the one under the pointer, and a blind `return 1` starves the settings column's
        // scroll (the standing FL_MOUSEWHEEL rule).
        if (!Fl::event_inside(this) || !m_hasImage)
            return 0;
        const int dy = Fl::event_dy();
        if (dy == 0)
            return 0;
        const double fit = previewFitScale(m_imgW, m_imgH, w(), h());
        const double target = clampPreviewScale(previewWheelScale(m_view.scale, dy), fit);
        if (target == m_view.scale)
            return 1;
        const double cx = Fl::event_x() - x(); // widget-local: the transform lives in that space
        const double cy = Fl::event_y() - y();
        PreviewTransform t = previewZoomAt(m_view, target, cx, cy);
        applyView(clampPreviewPan(t, m_imgW, m_imgH, w(), h()), /*userDriven=*/true);
        return 1;
    }
    case FL_ENTER:
    case FL_MOVE:
        setCursorFor(Fl::event_x(), Fl::event_y());
        Fl_Group::handle(event); // the buttons still need their hover
        return 1;
    case FL_LEAVE:
        if (window() != nullptr)
            window()->cursor(FL_CURSOR_DEFAULT);
        Fl_Group::handle(event);
        return 1;
    case FL_PUSH:
        if (Fl_Group::handle(event)) // a corner button claimed it
            return 1;
        // Clicking the picture is "I am done with that field": park focus on the WINDOW, never on
        // null (FLTK's focus fixup hands null focus straight back on the next release), so the
        // field being edited still gets its FL_UNFOCUS and commits. This widget consumes the push
        // itself, so the app-wide chrome-click unfocus never sees it.
        if (window() != nullptr)
            Fl::focus(window());
        // Claim the WHOLE press pair, not just the push: an unclaimed release is offered to
        // whatever else sits under the pointer (the recurring new-widget bug).
        if (m_hasImage && Fl::event_button() == FL_LEFT_MOUSE) {
            m_panning = true;
            m_dragX = Fl::event_x();
            m_dragY = Fl::event_y();
        }
        return 1;
    case FL_DRAG:
        if (!m_panning)
            return 1;
        {
            PreviewTransform t = m_view;
            t.offsetX += Fl::event_x() - m_dragX;
            t.offsetY += Fl::event_y() - m_dragY;
            m_dragX = Fl::event_x();
            m_dragY = Fl::event_y();
            applyView(clampPreviewPan(t, m_imgW, m_imgH, w(), h()), /*userDriven=*/true);
        }
        return 1;
    case FL_RELEASE:
        m_panning = false;
        setCursorFor(Fl::event_x(), Fl::event_y());
        return 1;
    default:
        return Fl_Group::handle(event);
    }
}

} // namespace mosaic::ui
