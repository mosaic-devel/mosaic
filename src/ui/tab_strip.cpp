#include "ui/tab_strip.hpp"

#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp" // ellipsizeToWidth, localPathsFromDndText

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace mosaic::ui {
namespace {

Fl_Color toFl(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }

// Tab furniture, left to right: padding, label, gap, the dirty dot / read-only padlock, gap, the
// close X, padding. The marker slot is reserved even when a tab is clean so the X never shifts
// sideways the moment a document is edited.
constexpr int kPadLeft = 10;
constexpr int kPadRight = 6;
constexpr int kLabelGap = 6;
constexpr int kMarkerW = 10; // the dot's cell (kIconPx for the padlock, drawn centred in it)
constexpr int kMarkerGap = 2;
constexpr int kCloseW = kIconPx; // 16
constexpr int kDotRadius = 3;
constexpr int kFontPx = 12;

int furnitureWidth() { return kPadLeft + kLabelGap + kMarkerW + kMarkerGap + kCloseW + kPadRight; }

} // namespace

std::vector<int> fitTabWidths(const std::vector<int>& natural, int availW) {
    if (natural.empty())
        return {};
    const long total = std::accumulate(natural.begin(), natural.end(), 0L);
    if (availW <= 0 || total <= availW)
        return natural;
    // Shrink proportionally, but never past the floor. Tabs already at or below the floor keep
    // their width, so a wide tab beside several narrow ones gives up the room -- which is what a
    // proportional scale does anyway, and it saves a second redistribution pass.
    const double scale = static_cast<double>(availW) / static_cast<double>(total);
    std::vector<int> out;
    out.reserve(natural.size());
    for (const int n : natural)
        out.push_back(std::max(kTabMinWidth, static_cast<int>(std::lround(n * scale))));
    return out; // sum may still exceed availW: the caller scrolls
}

TabStrip::TabStrip(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}

void TabStrip::setTabs(std::vector<TabItem> tabs, std::size_t active) {
    const bool sameActive = active == m_active;
    bool same = sameActive && tabs.size() == m_tabs.size();
    for (std::size_t i = 0; same && i < tabs.size(); ++i) {
        same = tabs[i].label == m_tabs[i].label && tabs[i].dirty == m_tabs[i].dirty &&
               tabs[i].readOnly == m_tabs[i].readOnly;
    }
    if (same)
        return; // nothing changed: don't churn the display from the host's per-frame sync
    m_tabs = std::move(tabs);
    m_active = m_tabs.empty() ? 0 : std::min(active, m_tabs.size() - 1);
    if (m_hover >= m_tabs.size()) {
        m_hover = static_cast<std::size_t>(-1);
        m_hoverClose = false;
    }
    clampScroll();
    redraw();
}

std::vector<TabStrip::Slot> TabStrip::slots() const {
    std::vector<int> natural;
    natural.reserve(m_tabs.size());
    fl_font(FL_HELVETICA, kFontPx);
    for (const TabItem& t : m_tabs) {
        // CEIL, not truncate. draw() gives the label exactly `tw - furnitureWidth()` px, so this
        // measurement IS the label's box -- with no slack anywhere to absorb a rounding error. A
        // truncating cast leaves the box a fraction of a pixel short of the text it was measured
        // from, and ellipsizeToWidth duly cuts the last character off a name that fits. Invisible
        // wherever advances land near whole pixels and constant where they do not: macOS renders
        // this font with fractional advances, so "Untitled 2" ellipsized there and nowhere else.
        const int labelW = static_cast<int>(std::ceil(fl_width(t.label.c_str())));
        natural.push_back(std::clamp(labelW + furnitureWidth(), kTabMinWidth, kTabMaxWidth));
    }
    const std::vector<int> widths = fitTabWidths(natural, w());
    std::vector<Slot> out;
    out.reserve(widths.size());
    int cx = -m_scrollX;
    for (const int width : widths) {
        out.push_back({cx, width});
        cx += width;
    }
    return out;
}

int TabStrip::contentWidth() const {
    const std::vector<Slot> s = slots();
    return s.empty() ? 0 : s.back().x + s.back().w + m_scrollX;
}

void TabStrip::clampScroll() {
    const int overflow = std::max(0, contentWidth() - w());
    m_scrollX = std::clamp(m_scrollX, 0, overflow);
}

std::size_t TabStrip::tabAt(int localX, bool* overClose) const {
    if (overClose != nullptr)
        *overClose = false;
    const std::vector<Slot> s = slots();
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (localX < s[i].x || localX >= s[i].x + s[i].w)
            continue;
        if (overClose != nullptr) {
            const int closeL = s[i].x + s[i].w - kPadRight - kCloseW;
            *overClose = localX >= closeL && localX < closeL + kCloseW;
        }
        return i;
    }
    return static_cast<std::size_t>(-1);
}

void TabStrip::draw() {
    const Palette& pal = activePalette();
    // Erase the FULL widget rect before any text: FLTK keeps the back buffer between frames, so an
    // unfilled region keeps last frame's pixels and every label thickens on hover.
    fl_color(toFl(pal.panelBg));
    fl_rectf(x(), y(), w(), h());

    const std::vector<Slot> s = slots();
    fl_push_clip(x(), y(), w(), h()); // a scrolled-out tab must not paint over the dock
    for (std::size_t i = 0; i < s.size(); ++i) {
        const int tx = x() + s[i].x;
        const int tw = s[i].w;
        if (tx + tw <= x() || tx >= x() + w())
            continue; // wholly scrolled out
        const bool active = i == m_active;
        const bool hover = i == m_hover;
        const common::Color8 bg =
            active ? pal.controlActive : (hover ? pal.controlHover : pal.panelBg);
        fl_color(toFl(bg));
        fl_rectf(tx, y(), tw, h());
        if (active) { // an accent rule along the top edge: the "you are here" mark
            fl_color(toFl(pal.accent));
            fl_rectf(tx, y(), tw, 2);
        } else { // separator on the right edge of every inactive tab
            fl_color(toFl(pal.border));
            fl_line(tx + tw - 1, y() + 6, tx + tw - 1, y() + h() - 7);
        }

        // The close X and the dirty/read-only marker are anchored to the RIGHT edge, so the label
        // gets whatever is left. Both are always drawn (muted at rest): a cell that is empty until
        // hovered reads as a hole -- the rule the layer dock's lock cell settled.
        const int closeCx = tx + tw - kPadRight - kCloseW / 2;
        const int cy = y() + h() / 2;
        const bool closeHot = hover && m_hoverClose;
        drawIcon(Icon::Close, closeCx, cy, closeHot ? pal.accent : pal.textMuted);

        const int markerCx = tx + tw - kPadRight - kCloseW - kMarkerGap - kMarkerW / 2;
        if (s[i].w > kTabMinWidth / 2) { // a tab squeezed to nothing drops its marker, not its X
            if (m_tabs[i].dirty) {
                drawAAPrims(markerCx - kDotRadius - 1, cy - kDotRadius - 1, 2 * kDotRadius + 2,
                            2 * kDotRadius + 2, [&](int, int) { return bg; },
                            {{static_cast<double>(markerCx), static_cast<double>(cy),
                              static_cast<double>(kDotRadius), 0.0, pal.accent}});
            } else if (m_tabs[i].readOnly) {
                drawIcon(Icon::LockClosed, markerCx, cy, pal.textMuted);
            }
        }

        const int labelL = tx + kPadLeft;
        const int labelR = markerCx - kMarkerW / 2 - kLabelGap;
        fl_font(FL_HELVETICA, kFontPx);
        fl_color(toFl(active ? pal.text : pal.textMuted));
        const std::string shown = ellipsizeToWidth(m_tabs[i].label, labelR - labelL);
        fl_draw(shown.c_str(), labelL, y(), std::max(0, labelR - labelL), h(),
                FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    }
    fl_pop_clip();

    if (m_dropHot) {
        // A file drag is over the strip: dropping here OPENS it as a document, where dropping on
        // the canvas would place it as a magic layer. Nothing else says which, so say it.
        fl_color(toFl(pal.accent));
        fl_rect(x(), y(), w(), h());
        fl_rect(x() + 1, y() + 1, w() - 2, h() - 2);
        return; // the accent frame replaces the separating rule
    }
    fl_color(toFl(pal.border)); // the rule that separates the strip from the canvas below
    fl_line(x(), y() + h() - 1, x() + w(), y() + h() - 1);
}

int TabStrip::handle(int event) {
    switch (event) {
    case FL_ENTER:
        return 1; // opt in to FL_MOVE
    case FL_MOVE: {
        bool overClose = false;
        const std::size_t hit = tabAt(Fl::event_x() - x(), &overClose);
        if (hit != m_hover || overClose != m_hoverClose) {
            m_hover = hit;
            m_hoverClose = overClose;
            // The label is ellipsized, so the tooltip is the only place the full name shows.
            if (hit < m_tabs.size())
                copy_tooltip(m_tabs[hit].label.c_str());
            else
                tooltip(nullptr);
            redraw();
        }
        return 1;
    }
    case FL_LEAVE:
        if (m_hover != static_cast<std::size_t>(-1)) {
            m_hover = static_cast<std::size_t>(-1);
            m_hoverClose = false;
            redraw();
        }
        return 1;
    case FL_PUSH: {
        bool overClose = false;
        const std::size_t hit = tabAt(Fl::event_x() - x(), &overClose);
        if (hit >= m_tabs.size())
            return 1; // empty strip space: consume, do nothing
        const int button = Fl::event_button();
        // Middle-click closes, the browser convention; a left click on the X does too.
        if (button == FL_MIDDLE_MOUSE || (button == FL_LEFT_MOUSE && overClose)) {
            if (m_onClose)
                m_onClose(hit); // MAY delete tabs (and re-enter setTabs): touch nothing after this
            return 1;
        }
        if (button == FL_LEFT_MOUSE && hit != m_active && m_onSelect)
            m_onSelect(hit);
        return 1;
    }
    // S50: a file dropped on the strip opens as a document.
    case FL_DND_ENTER:
    case FL_DND_DRAG:
        if (!m_onFilesDropped)
            return 0; // refused: the drag source shows "no drop target here"
        if (!m_dropHot) {
            m_dropHot = true;
            redraw();
        }
        return 1;
    case FL_DND_LEAVE:
        if (m_dropHot) {
            m_dropHot = false;
            redraw();
        }
        return 1;
    case FL_DND_RELEASE:
        if (!m_onFilesDropped)
            return 0;
        m_dropHot = false;
        redraw();
        m_expectDropPaste = true; // the payload follows as an FL_PASTE aimed at us
        return 1;
    case FL_PASTE: {
        if (!m_expectDropPaste)
            return 0; // some other paste routing: not ours
        m_expectDropPaste = false;
        const char* dropped = Fl::event_text();
        const std::vector<std::string> paths =
            localPathsFromDndText(dropped != nullptr ? dropped : "");
        if (!paths.empty() && m_onFilesDropped)
            m_onFilesDropped(paths); // MAY rebuild the tabs (and re-enter setTabs): nothing after
        return 1;
    }
    case FL_MOUSEWHEEL: {
        // Decline the wheel unless the cursor is really over us: Fl_Group's second dispatch pass
        // offers it to siblings NOT under the pointer, and a blind `return 1` hijacks their scroll.
        if (!Fl::event_inside(this))
            return 0;
        const int overflow = std::max(0, contentWidth() - w());
        if (overflow == 0)
            return 0; // everything fits: leave the wheel to whoever else wants it
        const int dy = Fl::event_dy();
        const int dx = Fl::event_dx();
        m_scrollX = std::clamp(m_scrollX + (dx + dy) * 24, 0, overflow);
        redraw();
        return 1;
    }
    default:
        break;
    }
    return Fl_Widget::handle(event);
}

} // namespace mosaic::ui
