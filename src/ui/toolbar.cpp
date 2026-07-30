#include "ui/toolbar.hpp"

#include "common/image.hpp"
#include "common/log.hpp"
#include "ui/icon_pack.hpp" // renderIconSource: vector or raster pack art
#include "ui/color_swatch.hpp"
#include "ui/theme.hpp"
#include "ui/tool.hpp"
#include "ui/tool_flyout.hpp"
#include "ui/toolbar_layout.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Widget.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mosaic::ui {
namespace {

constexpr int kBtn = 32;     // toolbar button square (kept square on resize -- see LeftToolbar)
constexpr int kBtnGap = 2;   // vertical gap between buttons within a group
constexpr int kGroupGap = 10; // extra space opened at a ToolGroup boundary (hosts a divider)
constexpr int kTopPad = 4;   // gap above the first button
constexpr int kIconPx = 20;  // rasterized glyph size (drawn 1:1, centered in the button)
constexpr int kDividerInset = 6; // horizontal inset of the group-divider hairline
constexpr int kSwatchPad = 8; // gap below the active-colour swatch at the column's bottom
constexpr int kTriHit = 12; // bottom-right corner region that opens a slot's flyout on left-click
// The bottom overflow chevron's hit box. Matches the options-bar chevron's footprint (S16-n:
// kChevronW x kCtlH = 26 x 22) so the padding around the drawn glyph reads the same, rather than the
// wide/short rectangle a full button width gave.
constexpr int kChevW = 26;
constexpr int kChevH = 22;
constexpr int kChevGap = 8; // padding between the overflow chevron and the swatch below it
constexpr int kPopPad = 5;  // inset around the overflow popover's stacked buttons

// Where the swatch sits: horizontally centred in the column, pinned to its bottom.
int swatchX(int columnX, int columnW) {
    return columnX + (columnW - kSwatchW) / 2;
}
int swatchY(int columnY, int columnH) {
    return columnY + columnH - kSwatchH - kSwatchPad;
}
// The overflow chevron sits above the swatch with kChevGap of breathing room between them.
int chevronY(int columnY, int columnH) {
    return swatchY(columnY, columnH) - kChevGap - kChevH;
}

spdlog::logger& uiLog() {
    static const auto logger = common::log::category("ui");
    return *logger;
}

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

// A toolbar *slot* button (S11-e): a themed square that draws the slot's currently-shown variant icon
// and fills with the accent when the active tool belongs to this slot. A left-click activates the
// shown variant. A slot that holds variants also carries a small corner triangle and opens its flyout
// on a right-click (or a left-click on the triangle); picking a variant there makes it the slot's icon.
class ToolButton : public Fl_Button {
public:
    ToolButton(int X, int Y, int W, int H, ToolManager& tools, ToolSlot slot)
        : Fl_Button(X, Y, W, H), m_tools(tools), m_slot(slot) {
        clear_visible_focus();
        const std::vector<Tool*> variants = m_tools.toolsInSlot(slot);
        m_hasFlyout = variants.size() > 1;
        // Rasterize each variant's (already-coloured) glyph once; draw() blits whichever is shown. Each
        // Fl_RGB_Image references its IconImg::pixels buffer (no copy), so both live together in m_icons.
        for (const Tool* t : variants) {
            IconImg ic;
            std::string err;
            ic.pixels = renderIconSource(t->icon(), kIconPx, &err); // vector or raster pack art
            if (ic.pixels.empty())
                uiLog().warn("tool icon rendering failed ({}): {}", t->name(), err);
            else {
                ic.img = std::make_unique<Fl_RGB_Image>(ic.pixels.rgba.data(),
                                                        static_cast<int>(ic.pixels.width),
                                                        static_cast<int>(ic.pixels.height), 4);
                // A disabled-state copy: scale alpha to ~35% so the glyph reads as muted over the
                // panel ground regardless of theme (no per-theme colour to recompute on re-theme).
                ic.dimPixels = ic.pixels;
                for (std::size_t a = 3; a < ic.dimPixels.rgba.size(); a += 4)
                    ic.dimPixels.rgba[a] = static_cast<unsigned char>(ic.dimPixels.rgba[a] * 90 / 255);
                ic.dim = std::make_unique<Fl_RGB_Image>(ic.dimPixels.rgba.data(),
                                                        static_cast<int>(ic.dimPixels.width),
                                                        static_cast<int>(ic.dimPixels.height), 4);
            }
            m_icons.emplace(t->id(), std::move(ic));
        }
        // Set the tooltip up front (NOT lazily in draw()): FLTK decides whether to arm a tooltip
        // the instant the pointer enters, which can precede this button's first draw -- so a
        // draw-time set silently loses the first hover. draw() still refreshes it when the shown
        // variant changes (the guard below).
        const ToolId shown = m_tools.shownToolForSlot(m_slot);
        if (const Tool* t = m_tools.find(shown))
            copy_tooltip(t->tooltip().c_str());
        m_tooltipFor = shown;
        m_tooltipInit = true;
    }
    void setFlyout(ToolFlyout* flyout) { m_flyout = flyout; }

protected:
    void draw() override {
        const Palette& pal = activePalette();
        // active_r(): the toolbar is greyed while an inpaint run locks the chrome. Disabled = no
        // accent/hover fill, the muted glyph, and a muted flyout triangle (see the chosen treatment).
        const bool enabled = active_r();
        const ToolId shown = m_tools.shownToolForSlot(m_slot);
        const bool active = enabled && m_tools.slotOf(m_tools.active()) == m_slot;
        if (active)
            draw_box(MOSAIC_BUTTON_DOWN_BOX, toFl(pal.accent)); // accent marks the active tool's slot
        else if (enabled && m_hover)
            draw_box(MOSAIC_BUTTON_UP_BOX, toFl(pal.controlHover));
        else { // clear our slot to the toolbar ground (erases a prior hover/active fill)
            fl_color(toFl(pal.panelBg));
            fl_rectf(x(), y(), w(), h());
        }
        if (const auto it = m_icons.find(shown); it != m_icons.end()) {
            // The dim copy when greyed; fall back to the full glyph if it never rasterized.
            Fl_RGB_Image* glyph = (!enabled && it->second.dim != nullptr) ? it->second.dim.get()
                                                                          : it->second.img.get();
            if (glyph != nullptr)
                glyph->draw(x() + (w() - kIconPx) / 2, y() + (h() - kIconPx) / 2);
        }
        if (m_hasFlyout)
            drawFlyoutTriangle(active ? pal.text : pal.textMuted);
        if (!m_tooltipInit || shown != m_tooltipFor) { // keep the tooltip on the shown variant
            if (const Tool* t = m_tools.find(shown))
                copy_tooltip(t->tooltip().c_str());
            m_tooltipFor = shown;
            m_tooltipInit = true;
        }
    }

    int handle(int event) override {
        switch (event) {
        case FL_ENTER:
            m_hover = true;
            redraw();
            return 1; // claim ENTER so FLTK also delivers LEAVE
        case FL_LEAVE:
            m_hover = false;
            redraw();
            return 1;
        case FL_PUSH: {
            // Right-click anywhere on a variant slot -- or a left-click on its corner triangle -- opens
            // the flyout; any other left-click activates the slot's shown variant. Outside-click
            // dismissal spares the anchor rect, so a click on *this* button while its own flyout is
            // open must close it here: triangle/right-click toggles it shut, and a plain activate
            // also dismisses (the user-reported stuck-flyout bug).
            const bool flyoutOpenHere = m_flyout != nullptr && m_flyout->shownFor(this);
            if (m_hasFlyout && (Fl::event_button() == FL_RIGHT_MOUSE ||
                                inTriangle(Fl::event_x(), Fl::event_y()))) {
                if (flyoutOpenHere)
                    m_flyout->hide();
                else
                    openFlyout();
                return 1;
            }
            if (Fl::event_button() == FL_LEFT_MOUSE) {
                m_tools.setActive(m_tools.shownToolForSlot(m_slot));
                if (flyoutOpenHere)
                    m_flyout->hide();
            }
            return 1;
        }
        default:
            return Fl_Button::handle(event);
        }
    }

private:
    struct IconImg {
        common::Image pixels; // backing store for img
        std::unique_ptr<Fl_RGB_Image> img;
        common::Image dimPixels;          // backing store for the disabled (alpha-reduced) glyph
        std::unique_ptr<Fl_RGB_Image> dim; // blitted when the toolbar is greyed (inpaint busy)
    };

    [[nodiscard]] bool inTriangle(int ex, int ey) const {
        return (ex - x()) >= w() - kTriHit && (ey - y()) >= h() - kTriHit;
    }
    void drawFlyoutTriangle(common::Color8 c) {
        const int s = 5;
        const int rx = x() + w() - s - 2;
        const int ry = y() + h() - s - 2;
        fl_color(toFl(c));
        fl_begin_polygon(); // a small filled wedge tucked into the bottom-right corner
        fl_vertex(rx + s, ry);
        fl_vertex(rx + s, ry + s);
        fl_vertex(rx, ry + s);
        fl_end_polygon();
    }
    void openFlyout() {
        if (m_flyout != nullptr)
            m_flyout->showForSlot(m_slot, this);
    }

    ToolManager& m_tools;
    ToolSlot m_slot;
    ToolFlyout* m_flyout = nullptr;
    bool m_hasFlyout = false;
    std::map<ToolId, IconImg> m_icons; // rasterized glyph per variant (the shown one is drawn)
    ToolId m_tooltipFor = ToolId::Move;
    bool m_tooltipInit = false;
    bool m_hover = false;
};

// The bottom overflow affordance (S16-o): a drawn double-chevron pointing DOWN ("more tools below"),
// pinned just above the swatch, that toggles the toolbar overflow popover. The vertical sibling of the
// options bar's OverflowChevron -- drawn with fl_line (host-font rule, never a glyph), re-click closes.
class OverflowChevronV : public Fl_Widget {
public:
    OverflowChevronV(int X, int Y, int W, int H, ToolbarOverflowPopover* pop)
        : Fl_Widget(X, Y, W, H), m_pop(pop) {
        copy_tooltip("More tools");
    }

protected:
    void draw() override {
        const Palette& pal = activePalette();
        const bool enabled = active_r(); // greyed with the rest of the chrome during an inpaint run
        fl_color(toFl(enabled && m_hover ? pal.controlHover : pal.panelBg));
        fl_rectf(x(), y(), w(), h());
        fl_color(toFl(enabled ? pal.text : pal.textMuted));
        const int midX = x() + w() / 2;
        constexpr int arm = 4; // half-width / depth of one chevron
        fl_line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, 2);
        for (int k = 0; k < 2; ++k) {
            const int by = y() + h() / 2 - 4 + k * 5; // two stacked downward chevrons
            fl_begin_line();
            fl_vertex(midX - arm, by);
            fl_vertex(midX, by + arm);
            fl_vertex(midX + arm, by);
            fl_end_line();
        }
        fl_line_style(0); // reset: line style is global FLTK draw state
    }

    int handle(int event) override {
        switch (event) {
        case FL_ENTER:
            m_hover = true;
            redraw();
            return 1; // claim ENTER so FLTK also delivers LEAVE
        case FL_LEAVE:
            m_hover = false;
            redraw();
            return 1;
        case FL_PUSH:
            return 1; // act on release
        case FL_RELEASE:
            if (m_pop != nullptr) {
                if (m_pop->shownFor(this))
                    m_pop->hide();
                else
                    m_pop->showAnchored(this); // already populated by the last rebuild()
            }
            return 1;
        default:
            return Fl_Widget::handle(event);
        }
    }

private:
    ToolbarOverflowPopover* m_pop;
    bool m_hover = false;
};

} // namespace

// ---- the pure overflow split (declared in toolbar_layout.hpp, kept FLTK-free) ----------------
ToolbarSplit splitToolbarSlots(std::size_t total, std::size_t fit, std::size_t activeIndex) {
    ToolbarSplit s;
    if (total == 0)
        return s;
    if (fit < 1)
        fit = 1;
    if (fit >= total) { // everything fits: no overflow
        for (std::size_t i = 0; i < total; ++i)
            s.visible.push_back(i);
        return s;
    }
    if (activeIndex >= fit && activeIndex < total) {
        // The active slot would overflow: give it the last visible spot, push the slot it displaces
        // (the old index fit-1) into the overflow. Both lists stay in ascending natural order.
        for (std::size_t i = 0; i + 1 < fit; ++i)
            s.visible.push_back(i);
        s.visible.push_back(activeIndex);
        for (std::size_t i = fit - 1; i < total; ++i)
            if (i != activeIndex)
                s.overflow.push_back(i);
    } else {
        for (std::size_t i = 0; i < fit; ++i)
            s.visible.push_back(i);
        for (std::size_t i = fit; i < total; ++i)
            s.overflow.push_back(i);
    }
    return s;
}

// ---- ToolbarOverflowPopover ------------------------------------------------------------------
ToolbarOverflowPopover::ToolbarOverflowPopover(ToolManager& tools)
    : Popover(kBtn + 2 * kPopPad + (Popover::bubbleSupported() ? Popover::kBubbleTri : 0),
              kBtn + 2 * kPopPad),
      m_tools(tools) {
    enableBubble(); // a left-pointing comic-book pointer aimed at the overflow chevron (Popover draws it)
}

void ToolbarOverflowPopover::rebuildWith(const std::vector<ToolSlot>& slots, ToolFlyout* flyout) {
    m_dividerYs.clear();
    clear(); // drop the previous overflow set's buttons
    begin();
    const int dx = bubbleActive() ? kBubbleTri : 0; // left margin reserved for the pointer triangle
    const int popW = kBtn + 2 * kPopPad + dx;
    int by = kPopPad;
    bool first = true;
    ToolGroup prevGroup = ToolGroup::SelectTransform;
    for (const ToolSlot slot : slots) {
        const ToolGroup g = m_tools.toolsInSlot(slot).front()->group(); // a slot never straddles a group
        if (!first) {
            // Carry the toolbar's group dividers into the overflow list: open the wider gap (with a
            // hairline centred in it) at a cluster boundary, else the ordinary inter-button gap.
            if (g != prevGroup) {
                m_dividerYs.push_back(by + (kGroupGap - kBtnGap) / 2);
                by += kGroupGap;
            } else {
                by += kBtnGap;
            }
        }
        auto* btn = new ToolButton(dx + kPopPad, by, kBtn, kBtn, m_tools, slot);
        btn->setFlyout(flyout);
        by += kBtn;
        prevGroup = g;
        first = false;
    }
    end();
    resizable(nullptr); // buttons are fixed; clear() had reset this to the group
    const int popH = slots.empty() ? kBtn + 2 * kPopPad : by + kPopPad;
    setBaseSize(popW, popH);
}

void ToolbarOverflowPopover::drawContent() {
    Popover::drawContent(); // themed panel (or bubble) chrome + the stacked tool buttons -- NOT the bare
                     // Fl_Double_Window::draw(), whose FL_NO_BOX left the background unpainted (black)
    const int dx = bubbleActive() ? kBubbleTri : 0; // clear the pointer's left margin
    fl_color(toFl(activePalette().border));
    for (const int dy : m_dividerYs) // hairlines inset from the popover edges, matching the toolbar
        fl_xyline(dx + kDividerInset, dy, w() - 1 - kDividerInset);
}

// ---- LeftToolbar -----------------------------------------------------------------------------
LeftToolbar::LeftToolbar(int X, int Y, int W, int H, ToolManager& tools, ColorState& colors)
    : Panel(X, Y, W, H), m_tools(tools) {
    borderEdges(EdgeRight); // the toolbar owns only the toolbar|canvas junction (see Panel)
    begin();
    // The active-colour swatch lives at the bottom of the column (S11-d); the tool buttons above it
    // are (re)built by rebuild(), which keeps this swatch and re-evaluates the vertical overflow.
    m_swatch = new ColorSwatch(swatchX(X, W), swatchY(Y, H), kSwatchW, kSwatchH, colors);
    end();
    // Keep children a fixed square size and top-anchored as the column grows: with no resizable child,
    // Fl_Group::resize() only *translates* children (the column's X/Y/W never change, only H).
    resizable(nullptr);
    rebuild();
}

LeftToolbar::~LeftToolbar() {
    Fl::remove_timeout(&LeftToolbar::deferredRelayoutCb, this);
}

void LeftToolbar::setColorPicker(ColorPicker* picker) {
    if (m_swatch != nullptr)
        m_swatch->attachPicker(picker);
}

void LeftToolbar::setToolFlyout(ToolFlyout* flyout) {
    m_flyout = flyout;
    rebuild(); // re-create the buttons so each one carries the flyout
}

void LeftToolbar::setOverflowPopover(ToolbarOverflowPopover* popover) {
    m_overflow = popover;
    rebuild(); // overflow now has somewhere to go: re-evaluate at the current height
}

int LeftToolbar::buttonsAvailHeight(bool reserveChevron) const {
    const int firstY = y() + kTopPad;
    // Buttons end a gap above the swatch, or a gap above the chevron (which itself clears the swatch
    // by kChevGap) when overflow is in play.
    const int bottomLimit =
        (reserveChevron ? chevronY(y(), h()) : swatchY(y(), h())) - kBtnGap;
    return bottomLimit - firstY;
}

namespace {
// The split for the current height + active tool, mapped back to slots. Shared by relayout()
// (cheap, no widget churn) and rebuild(). Conservative on the group-gap budget so the laid-out
// column never overruns the chevron/swatch even when the active-visible swap shifts a divider.
struct SlotSplit {
    std::vector<ToolSlot> visible;
    std::vector<ToolSlot> overflow;
};
} // namespace

static SlotSplit computeSlotSplit(const ToolManager& tools, int availNoChevron, int availChevron) {
    const std::vector<ToolSlot>& all = tools.slots();
    const std::size_t total = all.size();
    auto groupOf = [&](ToolSlot s) { return tools.toolsInSlot(s).front()->group(); };
    std::size_t groups = 0; // number of group transitions in the full list
    for (std::size_t i = 1; i < total; ++i)
        if (groupOf(all[i]) != groupOf(all[i - 1]))
            ++groups;
    // Worst-case height of n leading buttons: every possible divider among them is charged, so the
    // real layout (any subset of n) can only be shorter -- a fit computed here never overruns.
    auto maxHeight = [&](std::size_t n) {
        if (n == 0)
            return 0;
        const std::size_t bounds = std::min(n - 1, groups);
        return static_cast<int>(n) * kBtn + static_cast<int>(n - 1) * kBtnGap +
               static_cast<int>(bounds) * kGroupGap;
    };

    SlotSplit out;
    if (total == 0 || maxHeight(total) <= availNoChevron) { // all slots fit: no overflow, no chevron
        out.visible.assign(all.begin(), all.end());
        return out;
    }
    std::size_t fit = 0;
    while (fit < total && maxHeight(fit + 1) <= availChevron)
        ++fit;
    if (fit < 1)
        fit = 1;
    std::size_t activeIdx = total; // position of the active tool's slot in the natural order
    const ToolSlot activeSlot = tools.slotOf(tools.active());
    for (std::size_t i = 0; i < total; ++i)
        if (all[i] == activeSlot) {
            activeIdx = i;
            break;
        }
    const ToolbarSplit sp = splitToolbarSlots(total, fit, activeIdx);
    for (const std::size_t i : sp.visible)
        out.visible.push_back(all[i]);
    for (const std::size_t i : sp.overflow)
        out.overflow.push_back(all[i]);
    return out;
}

void LeftToolbar::relayout() {
    const SlotSplit sp =
        computeSlotSplit(m_tools, buttonsAvailHeight(false), buttonsAvailHeight(true));
    if (sp.visible == m_visibleSlots && sp.overflow == m_overflowSlots) {
        // Layout unchanged: the top-anchored buttons stay put; only the bottom-relative chevron moves.
        if (m_chevron != nullptr)
            m_chevron->resize(x() + (w() - kChevW) / 2, chevronY(y(), h()), kChevW, kChevH);
        return;
    }
    rebuild();
}

void LeftToolbar::reloadIcons() {
    // The split has not changed -- only the art has. rebuild() recreates the buttons, and each
    // ToolButton re-renders its variants' icon() in its constructor.
    rebuild();
    redraw();
}

void LeftToolbar::rebuild() {
    // The popover's anchor (chevron) and rows are about to be recreated; close it first so nothing
    // dangles and the row the user may have just clicked is gone cleanly (the relayout that reaches
    // here from a tool change is already deferred, so that click's handler has returned).
    if (m_overflow != nullptr && m_overflow->shown())
        m_overflow->hide();
    for (int i = children() - 1; i >= 0; --i) { // drop the tool buttons + chevron, keep the swatch
        Fl_Widget* c = child(i);
        if (c == m_swatch)
            continue;
        remove(c);
        delete c;
    }
    m_chevron = nullptr;
    m_dividerYs.clear();

    const SlotSplit sp =
        computeSlotSplit(m_tools, buttonsAvailHeight(false), buttonsAvailHeight(true));
    m_visibleSlots = sp.visible;
    m_overflowSlots = sp.overflow;

    begin();
    const int bx = x() + (w() - kBtn) / 2;
    int by = y() + kTopPad;
    bool first = true;
    ToolGroup prevGroup = ToolGroup::SelectTransform;
    for (const ToolSlot slot : m_visibleSlots) {
        const ToolGroup g = m_tools.toolsInSlot(slot).front()->group(); // a slot never straddles a group
        if (!first && g != prevGroup) {
            // New cluster: record a hairline centered in the gap, then open extra space for it.
            m_dividerYs.push_back(by + (kGroupGap - kBtnGap) / 2);
            by += kGroupGap;
        }
        auto* btn = new ToolButton(bx, by, kBtn, kBtn, m_tools, slot);
        btn->setFlyout(m_flyout);
        by += kBtn + kBtnGap;
        prevGroup = g;
        first = false;
    }
    if (!m_overflowSlots.empty() && m_overflow != nullptr) // the chevron, pinned above the swatch
        m_chevron = new OverflowChevronV(x() + (w() - kChevW) / 2, chevronY(y(), h()), kChevW,
                                         kChevH, m_overflow);
    end();
    resizable(nullptr);

    if (m_overflow != nullptr)
        m_overflow->rebuildWith(m_overflowSlots, m_flyout);
    redraw();
}

void LeftToolbar::deferredRelayoutCb(void* self) {
    auto* tb = static_cast<LeftToolbar*>(self);
    tb->m_deferredRebuild = false;
    tb->relayout();
}

void LeftToolbar::refresh() {
    redraw();
    // The active tool changed; when overflow is in play the active-visible swap may move slots, so
    // re-evaluate -- DEFERRED, because this can be reached from an overflow-popover button's own click
    // handler, and a synchronous rebuild would delete that button mid-event.
    if (!m_overflowSlots.empty() && !m_deferredRebuild) {
        m_deferredRebuild = true;
        Fl::add_timeout(0.0, &LeftToolbar::deferredRelayoutCb, this);
    }
}

void LeftToolbar::resize(int X, int Y, int W, int H) {
    Fl_Group::resize(X, Y, W, H); // translates children (no scaling: resizable is null)
    if (m_swatch != nullptr)      // re-pin the swatch to the (new) bottom of the column
        m_swatch->resize(swatchX(X, W), swatchY(Y, H), kSwatchW, kSwatchH);
    relayout(); // height changed: re-evaluate overflow (rebuilds only if the split actually changed)
}

void LeftToolbar::draw() {
    Panel::draw(); // panel fill + hairline border, then the child tool buttons (Fl_Group::draw)
    // Subtle group dividers sit in the inter-cluster gaps, so they never overlap a button.
    fl_color(toFl(activePalette().border));
    for (const int dy : m_dividerYs)
        fl_xyline(x() + kDividerInset, dy, x() + w() - 1 - kDividerInset);
}

} // namespace mosaic::ui
