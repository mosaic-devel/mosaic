#include "ui/status_bar.hpp"

#include "common/i18n.hpp"
#include "ui/color_state.hpp" // hexString
#include "ui/theme.hpp"
#include "ui/widgets.hpp" // ScrollingLabel

#include <FL/Fl.H>
#include <FL/Fl_Widget.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>

namespace mosaic::ui {
namespace {

constexpr int kPadX = 10;       // strip end padding
constexpr int kSectionGap = 18; // gap between the left-flow sections
constexpr int kChipSide = 11;   // the colour-under-cursor chip
constexpr int kFontSize = 11;
constexpr int kViewW = 104;  // reserved right slot for "6400% · -180.0°"
constexpr int kSpaceW = 170; // the colour-space indicator slot (scrolls when longer)

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

} // namespace

// A determinate progress bar + stage label + cancel X, shown in the status bar's transient area
// while a worker (the inpaint engine) runs. The X is drawn with two fl_line strokes, never a
// Unicode glyph (host-font rule). Reads activePalette() live, so it re-themes on the global redraw.
class ProgressStrip : public Fl_Widget {
public:
    ProgressStrip(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) { hide(); }

    void set(float fraction, std::string stage) {
        m_frac = std::clamp(fraction, 0.0f, 1.0f);
        m_stage = std::move(stage);
        if (!visible()) {
            show();
        }
        redraw();
    }
    void onCancel(std::function<void()> cb) { m_cancel = std::move(cb); }

    void draw() override {
        const Palette& pal = activePalette();
        fl_font(FL_HELVETICA, kFontSize);
        char pct[8];
        std::snprintf(pct, sizeof(pct), " %d%%", static_cast<int>(std::lround(m_frac * 100.0f)));
        const std::string label = m_stage + pct;
        const int ty = y() + (h() + fl_height()) / 2 - fl_descent();
        fl_color(toFl(pal.text));
        fl_draw(label.c_str(), x(), ty);
        const int labelW = static_cast<int>(fl_width(label.c_str()));

        const int bx = xButtonX();
        const int barX = x() + labelW + 10;
        const int barW = std::max(0, bx - 8 - barX);
        constexpr int kBarH = 6;
        const int barY = y() + (h() - kBarH) / 2;
        if (barW > 0) {
            fl_rectf(barX, barY, barW, kBarH, toFl(pal.controlBg)); // track
            const int fillW = static_cast<int>(std::lround(barW * static_cast<double>(m_frac)));
            if (fillW > 0) {
                fl_rectf(barX, barY, fillW, kBarH, toFl(pal.accent)); // fill
            }
            fl_color(toFl(pal.border));
            fl_rect(barX, barY, barW, kBarH);
        }

        // Cancel X (host-font rule: two strokes, never a glyph).
        const int by = y() + (h() - kXSize) / 2;
        if (m_xHover) {
            fl_rectf(bx, by, kXSize, kXSize, toFl(pal.controlHover));
        }
        fl_color(toFl(m_xHover ? pal.text : pal.textMuted));
        fl_line_style(FL_SOLID, 2);
        constexpr int kInset = 5;
        fl_line(bx + kInset, by + kInset, bx + kXSize - kInset, by + kXSize - kInset);
        fl_line(bx + kXSize - kInset, by + kInset, bx + kInset, by + kXSize - kInset);
        fl_line_style(0);
    }

    int handle(int event) override {
        const int bx = xButtonX();
        const int by = y() + (h() - kXSize) / 2;
        const bool inX = Fl::event_x() >= bx && Fl::event_x() < bx + kXSize &&
                         Fl::event_y() >= by && Fl::event_y() < by + kXSize;
        switch (event) {
        case FL_ENTER:
            return 1;
        case FL_MOVE:
            if (inX != m_xHover) {
                m_xHover = inX;
                redraw();
            }
            return 1;
        case FL_LEAVE:
            if (m_xHover) {
                m_xHover = false;
                redraw();
            }
            return 1;
        case FL_PUSH:
            return inX ? 1 : 0; // claim a press on the X so we get the matching release
        case FL_RELEASE:
            if (inX && m_cancel) {
                m_cancel();
            }
            return 1;
        default:
            break;
        }
        return Fl_Widget::handle(event);
    }

private:
    [[nodiscard]] int xButtonX() const { return x() + w() - kXSize; }

    static constexpr int kXSize = 16;
    static constexpr int kFontSize = 11;
    float m_frac = 0.0f;
    std::string m_stage;
    bool m_xHover = false;
    std::function<void()> m_cancel;
};

// NB the formatting below is deliberately _()-free: it is numbers + unit abbreviations, and the
// pure helpers stay deterministic for the unit tests. Localising the unit strings joins the
// i18n refinement pass (S53/S54).

std::string formatDocumentInfo(std::uint32_t w, std::uint32_t h, double ppi,
                               std::string_view precision, bool metric) {
    char buf[176];
    if (ppi > 0.0) {
        // ppi is pixels-per-inch, so inches = px / ppi; cm scales that by 2.54.
        const double scale = metric ? 2.54 : 1.0;
        const char* unit = metric ? "cm" : "in";
        const double physW = static_cast<double>(w) / ppi * scale;
        const double physH = static_cast<double>(h) / ppi * scale;
        std::snprintf(buf, sizeof buf,
                      "%u \xC3\x97 %u px \xC2\xB7 %.4g \xC3\x97 %.4g %s @ %.4g ppi \xC2\xB7 %.*s",
                      w, h, physW, physH, unit, ppi, static_cast<int>(precision.size()),
                      precision.data());
    } else {
        std::snprintf(buf, sizeof buf, "%u \xC3\x97 %u px \xC2\xB7 %.*s", w, h,
                      static_cast<int>(precision.size()), precision.data());
    }
    return buf;
}

std::string formatCursorPosition(double docX, double docY) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "X %ld  Y %ld", std::lround(std::floor(docX)),
                  std::lround(std::floor(docY)));
    return buf;
}

std::string formatColorReadout(common::Color8 c) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s \xC2\xB7 %u, %u, %u, %u", hexString(c).c_str(), c.r, c.g,
                  c.b, c.a);
    return buf;
}

std::string formatViewState(double zoom, double rotationDegrees) {
    if (std::abs(rotationDegrees) < 0.05)
        rotationDegrees = 0.0; // avoid a flickering "-0.0°"
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.4g%% \xC2\xB7 %.1f\xC2\xB0", zoom * 100.0, rotationDegrees);
    return buf;
}

std::string formatSelectionBounds(const common::Rect& r) {
    char buf[72];
    std::snprintf(buf, sizeof buf, "%ld \xC3\x97 %ld @ (%ld, %ld)", std::lround(r.w),
                  std::lround(r.h), std::lround(r.x), std::lround(r.y));
    return buf;
}

StatusBar::StatusBar(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    box(FL_NO_BOX);
    const Palette& pal = activePalette();
    begin();
    m_spaceLabel = new ScrollingLabel(0, 0, kSpaceW, H - 8);
    m_spaceLabel->color(toFl(pal.panelBg)); // its self-erase must match the strip's fill
    m_spaceLabel->labelcolor(toFl(pal.textMuted));
    m_spaceLabel->labelsize(kFontSize);
    m_spaceLabel->tooltip(_("Document color space"));
    m_statusLabel = new ScrollingLabel(0, 0, 10, H); // positioned by placeChildren; shown on demand
    m_statusLabel->setAlign(ScrollingLabel::Align::Left);
    m_statusLabel->color(toFl(pal.panelBg)); // self-erase ground must match the strip fill
    m_statusLabel->labelcolor(toFl(pal.textMuted));
    m_statusLabel->labelsize(kFontSize);
    m_statusLabel->hide();
    m_progress = new ProgressStrip(0, 0, 10, H); // positioned by placeChildren; hidden at rest
    end();
    resizable(nullptr); // fixed strip: children are re-pinned, never scaled
    placeChildren();
}

void StatusBar::reapplyTheme() {
    const Palette& pal = activePalette();
    if (m_spaceLabel != nullptr) {
        m_spaceLabel->color(toFl(pal.panelBg)); // self-erase ground must track the strip fill
        m_spaceLabel->labelcolor(toFl(pal.textMuted));
    }
    if (m_statusLabel != nullptr) {
        m_statusLabel->color(toFl(pal.panelBg));
        m_statusLabel->labelcolor(toFl(pal.textMuted));
    }
    redraw();
}

void StatusBar::placeChildren() {
    m_spaceLabel->resize(x() + w() - kPadX - kViewW - kSectionGap - kSpaceW,
                         y() + (h() - m_spaceLabel->h()) / 2, kSpaceW, m_spaceLabel->h());
    // The transient message area: start just after the document-info readout (which stays
    // visible) and run to the colour-space slot — so the bar/message sits where status text does,
    // not spanning the whole strip. The progress strip and the status label share this rect (only
    // one shows at a time).
    fl_font(FL_HELVETICA, kFontSize);
    const int docW =
        m_docInfo.empty() ? 0 : static_cast<int>(fl_width(m_docInfo.c_str())) + kSectionGap;
    const int px = x() + kPadX + docW;
    const int pw = std::max(40, m_spaceLabel->x() - kSectionGap - px);
    if (m_progress != nullptr)
        m_progress->resize(px, y(), pw, h());
    if (m_statusLabel != nullptr)
        m_statusLabel->resize(px, y(), pw, h());
}

void StatusBar::resize(int X, int Y, int W, int H) {
    Fl_Group::resize(X, Y, W, H); // translates children (resizable is null); then re-pin
    placeChildren();
}

void StatusBar::setDocumentInfo(std::uint32_t w, std::uint32_t h, double ppi,
                                std::string_view precision) {
    m_docW = w;
    m_docH = h;
    m_docPpi = ppi;
    m_docPrecision = precision;
    // The colour-space indicator describes THIS document's working space: it appears with a
    // document and leaves with it (clearDocumentInfo), never naming a space nothing is in.
    if (m_spaceLabel != nullptr && m_spaceLabel->visible() == 0)
        m_spaceLabel->show();
    std::string info = formatDocumentInfo(w, h, ppi, precision, m_metric);
    if (info == m_docInfo)
        return;
    m_docInfo = std::move(info);
    redraw();
}

void StatusBar::clearDocumentInfo() {
    m_docW = 0;
    m_docH = 0;
    m_docPpi = 0.0;
    m_docPrecision.clear();
    if (m_spaceLabel != nullptr && m_spaceLabel->visible() != 0)
        m_spaceLabel->hide(); // no document -> no working space to name
    if (m_docInfo.empty())
        return;
    m_docInfo.clear();
    redraw();
}

void StatusBar::setMetric(bool metric) {
    if (metric == m_metric)
        return;
    m_metric = metric;
    if (m_docW == 0 && m_docH == 0)
        return; // no document (cleared readout): nothing to re-format
    std::string info = formatDocumentInfo(m_docW, m_docH, m_docPpi, m_docPrecision, m_metric);
    if (info == m_docInfo)
        return;
    m_docInfo = std::move(info);
    redraw();
}

void StatusBar::setColorSpaceName(const std::string& name) {
    m_spaceLabel->setText(name);
}

void StatusBar::setCursor(const std::optional<CursorReadout>& readout) {
    if (m_cursor == readout)
        return;
    m_cursor = readout;
    redraw();
}

void StatusBar::setViewState(double zoom, double rotationDegrees) {
    std::string view = formatViewState(zoom, rotationDegrees);
    if (view == m_viewState)
        return;
    m_viewState = std::move(view);
    redraw();
}

void StatusBar::setSelectionBounds(const std::optional<common::Rect>& bounds) {
    std::string sel = bounds ? formatSelectionBounds(*bounds) : std::string{};
    if (sel == m_selection)
        return;
    m_selection = std::move(sel);
    redraw();
}

void StatusBar::setStatus(const std::string& text) {
    if (text == m_status)
        return;
    m_status = text;
    m_statusLabel->setText(text);
    // Re-pin (its width depends on the doc-info readout's), then show iff there's text and no
    // worker is currently owning the area.
    placeChildren();
    const bool show = !m_status.empty() && !progressVisible();
    if (show != static_cast<bool>(m_statusLabel->visible())) {
        if (show)
            m_statusLabel->show();
        else
            m_statusLabel->hide();
    }
    redraw();
}

double StatusBar::statusScrollSeconds() const {
    return (m_statusLabel != nullptr && m_statusLabel->visible()) ? m_statusLabel->oneScrollSeconds()
                                                                  : 0.0;
}

void StatusBar::setProgress(float fraction, const std::string& stage) {
    m_statusLabel->hide(); // a running worker owns the transient area; the status label steps aside
    placeChildren();       // re-pin in case the window resized while hidden
    m_progress->set(fraction, stage);
    redraw();
}

void StatusBar::hideProgress() {
    if (m_progress->visible()) {
        m_progress->hide();
        if (!m_status.empty()) {
            placeChildren();
            m_statusLabel->show(); // restore any status message the worker had covered
        }
        redraw();
    }
}

bool StatusBar::progressVisible() const {
    return m_progress != nullptr && m_progress->visible() != 0;
}

void StatusBar::onProgressCancel(std::function<void()> cancel) {
    m_progress->onCancel(std::move(cancel));
}

void StatusBar::draw() {
    const Palette& pal = activePalette();
    fl_rectf(x(), y(), w(), h(), toFl(pal.panelBg));
    fl_color(toFl(pal.border));
    fl_xyline(x(), y(), x() + w()); // hairline against the canvas above

    fl_font(FL_HELVETICA, kFontSize);
    const int ty = y() + (h() + fl_height()) / 2 - fl_descent(); // text baseline, centred

    // Right slot: zoom % + rotation°, right-aligned at the strip's end.
    if (!m_viewState.empty()) {
        fl_color(toFl(pal.text));
        fl_draw(m_viewState.c_str(),
                x() + w() - kPadX - static_cast<int>(fl_width(m_viewState.c_str())), ty);
    }

    // While the progress strip OR a status message is up, it owns the transient area: keep the
    // document-info readout (so the message starts after it, not at the far left) but suppress the
    // cursor/selection sections — only the progress/status child draws in their place.
    if (progressVisible() || m_statusLabel->visible()) {
        if (!m_docInfo.empty()) {
            fl_color(toFl(pal.textMuted));
            fl_draw(m_docInfo.c_str(), x() + kPadX, ty);
        }
        draw_children();
        return;
    }
    fl_push_clip(x(), y(), m_spaceLabel->x() - kSectionGap - x(), h());
    int tx = x() + kPadX;
    const auto section = [&](const std::string& s, common::Color8 col) {
        if (s.empty())
            return;
        fl_color(toFl(col));
        fl_draw(s.c_str(), tx, ty);
        tx += static_cast<int>(fl_width(s.c_str())) + kSectionGap;
    };
    section(m_docInfo, pal.textMuted);
    if (m_cursor) {
        section(formatCursorPosition(m_cursor->docX, m_cursor->docY), pal.text);
        // The colour chip + readout for the texel under the cursor -- ABSENT over a fully
        // transparent one: there is no colour there to report, and the section said so as
        // "#000000 · 0, 0, 0, 0" beside a checkerboard chip. The composite carries TRUE alpha
        // since S19-c (the checkerboard is a screen-space present effect, never baked in), so
        // a == 0 is really "nothing here" and not a sampled checker. Nothing black can creep back
        // in through toFl() dropping alpha, because with a == 0 no chip is drawn at all; partial
        // alpha keeps its normal chip + text. The position readout above stays, and whatever
        // follows closes up on section()'s own gap -- no dangling gap, no double one.
        if (m_cursor->insideDocument && m_cursor->color.a != 0) {
            const int cy = y() + (h() - kChipSide) / 2;
            fl_rectf(tx, cy, kChipSide, kChipSide, toFl(m_cursor->color));
            fl_color(toFl(pal.border));
            fl_rect(tx, cy, kChipSide, kChipSide);
            tx += kChipSide + 6;
            section(formatColorReadout(m_cursor->color), pal.text);
        }
    }
    if (!m_selection.empty()) {
        section(_("Sel"), pal.textMuted);
        tx -= kSectionGap - 6; // the label hugs its value
        section(m_selection, pal.text);
    }
    fl_pop_clip();

    draw_children(); // the colour-space ScrollingLabel
}

} // namespace mosaic::ui
