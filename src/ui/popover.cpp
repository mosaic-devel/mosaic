#include "ui/popover.hpp"

#include "common/image.hpp"
#include "platform/native_window.hpp" // raiseNativeWindowToTop: overlay z-order on Windows
#include "ui/color_flyout.hpp" // dismissActiveColorFlyoutOnOutsideClick (a panel hosts the colour chip)
#include "ui/theme.hpp"
#include "ui/widgets.hpp" // dismissActiveDropdownPopupOnOutsideClick (a popover may host Dropdowns)

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Widget.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace mosaic::ui {
namespace {

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

constexpr int kAnchorGap = 4;    // px between the anchor and a plain (non-bubble) popover
constexpr int kBubbleGap = 11;   // bubble: equal left/bottom gap; native Wayland keeps this
constexpr int kBubbleGapBump = 2; // bubble: Xwayland/X11 add this to both gaps (user 2026-06-22)

// EVERY shown popover, oldest first -- not "the" one. A single slot was the original model and it
// was wrong the moment two could be open at once: the toolbar's overflow popover holds tool-slot
// buttons, and clicking one opens that slot's variant flyout ON TOP of it. The flyout took the
// slot, the overflow popover was forgotten, and no outside click ever closed it again -- it just
// sat there while the flyout came and went (user 2026-08-28).
//
// A vector rather than one pointer, so dismissal can act on all of them: clicking away closes the
// whole stack at once, which is what a user means by "click away".
std::vector<Popover*> g_shown;

void forgetShown(Popover* p) {
    g_shown.erase(std::remove(g_shown.begin(), g_shown.end(), p), g_shown.end());
}

} // namespace

// The four-argument (x, y, w, h) base form is essential: it lets FLTK make this a *sub-window* of the
// current group (the main window, which is open when the picker is built) — the two-arg (w, h) form
// instead forces a top-level, which is the whole bug we are escaping (taskbar entry, compositor
// centring on Wayland, orphaned on parent close). The 0,0 is a placeholder; showAnchored() positions it.
Popover::Popover(int W, int H) : Fl_Double_Window(0, 0, W, H), m_baseW(W), m_baseH(H) {
    const Palette& pal = activePalette();
    border(0); // a sub-window has no decorations anyway; this is belt-and-braces
    box(FL_NO_BOX); // draw() paints the panel (or bubble) ourselves, damage-aware
    color(toFl(pal.panelBg));
    end(); // children are added by derived classes between their own begin()/end()
    // Subscribe AFTER end(): a runtime re-theme re-fills the panelBg ground (the lambda dispatches
    // virtually, so a derived override runs once the object is fully constructed).
    m_themeSub = ThemeSubscription([this] { reapplyTheme(); });
}

void Popover::reapplyTheme() {
    color(toFl(activePalette().panelBg));
    redraw();
}

Popover::~Popover() {
    forgetShown(this);
}

void Popover::show() {
    Fl_Double_Window::show();
    platform::raiseNativeWindowToTop(this);
    // Same reason as ui::BubbleFlyout::show(): on Windows shape() is a window REGION applied when
    // the HWND is made, so a reused HWND keeps the region from its FIRST open while the pop-over
    // has since been re-placed under it. Applied unconditionally rather than behind
    // buildBubbleShape()'s unchanged-geometry cache -- that cache is about not rebuilding the
    // MASK, and says nothing about whether this HWND has ever carried a region.
    if (m_bubble && !m_shapeBuf.empty())
        platform::applyNativeWindowShape(this, m_shapeBuf.data(), w(), h());
}

void Popover::showAnchored(const Fl_Widget* anchor) {
    m_anchor = anchor;
    place();
    show();
    forgetShown(this); // a re-show must not list it twice
    g_shown.push_back(this);
}

void Popover::reanchor() {
    // After the parent window resizes, FLTK's group-resize has both moved the anchor (the swatch is
    // re-pinned to the toolbar bottom) and mangled this popover's own geometry. Re-run placement to
    // pin it back to the anchor at its fixed size. No-op unless we are currently shown.
    if (m_anchor != nullptr && shown())
        place();
}

void Popover::setBaseSize(int w, int h) {
    m_baseW = w;
    m_baseH = h;
    // Apply immediately; place() re-applies (with position) on the next show/reanchor.
    Fl_Double_Window::size(w, h);
}

void Popover::setCornerPlacement(Corner preferred, std::function<common::Rect()> region,
                                 std::function<std::optional<common::Rect>()> avoid) {
    m_cornerPlacement = true;
    m_cornerPreferred = preferred;
    m_cornerRegion = std::move(region);
    m_cornerAvoid = std::move(avoid);
}

void Popover::place() {
    // Place the popover just right of the anchor, top-aligned with it (so a flyout opens downward like
    // a menu); then clamp it to stay fully inside the parent window, which pulls it up when the anchor
    // sits near the bottom (e.g. the colour swatch -> the picker opens upward). Always use the base
    // size (m_baseW/H), never w()/h(): the latter may have just been stretched by the parent's
    // group-resize, and a popover keeps a constant size.
    //
    // The anchor's position must be in the top-level window's coords (the space this popover, a direct
    // child of that window, is positioned in). top_window_offset() gives exactly that and -- crucially
    // -- stays correct when the anchor lives inside ANOTHER sub-window (e.g. a tool button in the
    // toolbar overflow popover, whose own x()/y() are popover-relative). Reading m_anchor->x()/y()
    // directly only happened to work for anchors that are direct children of the main window.
    int ax = 0;
    int ay = 0;
    m_anchor->top_window_offset(ax, ay);
    m_anchorX = ax;
    m_anchorY = ay;
    m_anchorW = m_anchor->w();
    m_anchorH = m_anchor->h();

    const int W = m_baseW;
    const int H = m_baseH;
    const Fl_Window* parentWin = window();
    const int pw = parentWin != nullptr ? parentWin->w() : W;
    const int ph = parentWin != nullptr ? parentWin->h() : H;

    if (m_cornerPlacement) {
        // Pin within `region` (the canvas area), default the whole parent window, a constant inset
        // from the region's edges. `region` already excludes the status bar (it is the canvas rect),
        // so a uniform margin keeps a tidy gap on every side.
        constexpr int kMargin = 10;
        common::Rect region =
            m_cornerRegion ? m_cornerRegion() : common::Rect{0, 0, double(pw), double(ph)};
        if (region.empty())
            region = common::Rect{0, 0, double(pw), double(ph)};
        const auto cornerRect = [&](Corner c) {
            const int px = c == Corner::BottomRight
                               ? int(std::lround(region.right())) - W - kMargin
                               : int(std::lround(region.x)) + kMargin;
            const int py = int(std::lround(region.bottom())) - H - kMargin;
            return common::Rect{double(px), double(py), double(W), double(H)};
        };
        common::Rect r = cornerRect(m_cornerPreferred);
        // Flip to the opposite corner if the preferred one would cover the text being edited.
        if (m_cornerAvoid)
            if (const std::optional<common::Rect> obstacle = m_cornerAvoid();
                obstacle && r.intersects(*obstacle)) {
                const Corner other = m_cornerPreferred == Corner::BottomRight ? Corner::BottomLeft
                                                                              : Corner::BottomRight;
                if (const common::Rect flipped = cornerRect(other); !flipped.intersects(*obstacle))
                    r = flipped; // the flip helps; otherwise keep the preferred corner
            }
        int px = std::clamp(int(std::lround(r.x)), 0, std::max(0, pw - W));
        int py = std::clamp(int(std::lround(r.y)), 0, std::max(0, ph - H));
        resize(px, py, W, H);
        return;
    }

    if (m_bubbleEnabled && m_bubbleSide == BubbleSide::Up) {
        // Body opens BELOW the anchor (a top-bar button): the triangle points UP at the anchor's
        // horizontal centre, occupying the top margin; the body is centred under the anchor, clamped.
        const int gap = kBubbleGap + (m_bubble ? kBubbleGapBump : 0);
        const int margin = m_bubble ? kBubbleTri : 0;
        int px = ax + m_anchorW / 2 - W / 2;
        int py = ay + m_anchorH + gap - margin; // window top = triangle tip; body starts at +margin
        px = std::clamp(px, 0, std::max(0, pw - W));
        py = std::clamp(py, 0, std::max(0, ph - H));
        m_tipX = std::clamp(ax + m_anchorW / 2 - px, kBubbleTriH / 2 + 4, W - kBubbleTriH / 2 - 4);
        resize(px, py, W, H);
        if (m_bubble)
            buildBubbleShape();
        return;
    }

    if (m_bubbleEnabled) {
        // One equal gap on both sides: the body's left edge sits `gap` past the toolbar (the triangle,
        // when present, bridges that gap to the anchor); with a status-bar reference its bottom edge sits
        // the same gap above it, else it tracks the anchor's top. Xwayland/X11 nudge the gap out +2px.
        const int gap = kBubbleGap + (m_bubble ? kBubbleGapBump : 0);
        const int margin = m_bubble ? kBubbleTri : 0; // the body is inset by the triangle margin only with a bubble
        // The left reference is the right edge of whatever the anchor SITS IN, not always the
        // toolbar's. For an ordinary toolbar button the two are the same thing. But an anchor can
        // live inside ANOTHER popover -- a tool-slot button inside the toolbar's overflow popover
        // opens that slot's variant flyout -- and pinning both to m_toolbarRight put the flyout at
        // the overflow popover's own x, one stacked exactly behind the other (user 2026-08-28).
        // Referencing the anchor's host sub-window instead opens the second popover BESIDE the
        // first, which is what a cascading menu does everywhere else.
        int leftRef = m_toolbarRight > 0 ? m_toolbarRight : ax + m_anchorW;
        if (const Fl_Window* host = m_anchor->window();
            host != nullptr && host != m_anchor->top_window()) {
            int hx = 0;
            int hy = 0;
            host->top_window_offset(hx, hy);
            leftRef = std::max(leftRef, hx + host->w());
        }
        int px = leftRef + gap - margin; // body-left = leftRef + gap; triangle (if any) occupies [px, px+margin]
        int py = m_statusBarH > 0 ? (ph - m_statusBarH - gap - H) : ay;
        px = std::clamp(px, 0, std::max(0, pw - W));
        py = std::clamp(py, 0, std::max(0, ph - H));
        m_tipY = std::clamp(ay + m_anchorH / 2 - py, kBubbleTriH / 2 + 4, H - kBubbleTriH / 2 - 4);
        resize(px, py, W, H);
        if (m_bubble)
            buildBubbleShape();
        return;
    }

    int px = ax + m_anchorW + kAnchorGap;
    int py = ay;
    px = std::clamp(px, 0, std::max(0, pw - W));
    py = std::clamp(py, 0, std::max(0, ph - H));
    resize(px, py, W, H);
}

bool Popover::bubbleSupported() {
    // Every platform can cut the shape now that the painting runs inside the driver's
    // draw_begin()/draw_end() bracket (see draw()). Wayland and macOS reported "shape() doesn't
    // work here" for as long as they did only because Popover painted outside it.
    return true;
}

void Popover::enableBubble(BubbleSide side) {
    m_bubbleEnabled = true;
    m_bubbleSide = side;
    m_bubble = bubbleSupported();
    // Claim the shape NOW, from the subclass constructor, while this sub-window is still unmapped:
    // showAnchored() builds the real mask far too late for native Wayland. See seedOpaqueWindowShape.
    if (m_bubble)
        seedOpaqueWindowShape(*this, m_shapeBuf, m_shapeImg);
}

void Popover::setBubbleInsets(int toolbarRight, int statusBarH) {
    m_toolbarRight = toolbarRight;
    m_statusBarH = statusBarH;
}

void Popover::buildBubbleShape() {
    const int W = w();
    const int H = h();
    const int tipRef = m_bubbleSide == BubbleSide::Up ? m_tipX : m_tipY;
    if (m_shapeW == W && m_shapeH == H && m_shapeTipY == tipRef)
        return; // unchanged since the last build
    // RGBA mask: opaque body + triangle, transparent corner margins (cut by Fl_Window::shape()).
    m_shapeBuf.assign(static_cast<std::size_t>(W) * H * 4, 0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            bool opaque = true;
            if (m_bubbleSide == BubbleSide::Up) {
                if (y < kBubbleTri) { // top margin: opaque only inside the triangle at this row
                    const float half = (kBubbleTriH / 2.0F) * (static_cast<float>(y) / kBubbleTri);
                    opaque = y >= 1 && static_cast<float>(x) >= m_tipX - half && // drop the tip pixel
                             static_cast<float>(x) <= m_tipX + half;
                }
            } else if (x < kBubbleTri) { // left margin: opaque only inside the triangle at this column
                const float half = (kBubbleTriH / 2.0F) * (static_cast<float>(x) / kBubbleTri);
                opaque = x >= 1 && static_cast<float>(y) >= m_tipY - half && // drop the tip pixel
                         static_cast<float>(y) <= m_tipY + half;
            }
            unsigned char* q = &m_shapeBuf[(static_cast<std::size_t>(y) * W + x) * 4];
            q[0] = q[1] = q[2] = 255;
            q[3] = opaque ? 255 : 0;
        }
    }
    m_shapeImg = std::make_unique<Fl_RGB_Image>(m_shapeBuf.data(), W, H, 4);
    shape(m_shapeImg.get());
    m_shapeW = W;
    m_shapeH = H;
    m_shapeTipY = tipRef;
}

void Popover::draw() {
    // Everything must be painted between the platform driver's draw_begin() and draw_end(), because
    // that bracket IS the Fl_Window::shape() implementation on Wayland and macOS (theme.hpp,
    // MOSAIC_CHROME_BOX). Only Fl_Window::draw() runs it, so this routes the real painting through
    // the chrome box rather than drawing here directly -- which is what silently cost every popover
    // its shape off X11.
    ScopedChromePainter bind([](void* ud) { static_cast<Popover*>(ud)->drawBracketed(); }, this);
    box(MOSAIC_CHROME_BOX);
    Fl_Double_Window::draw();
}

void Popover::drawBracketed() {
    drawContent();
    // drawContent() drew the children itself (that is where subclasses paint over them), so stop
    // Fl_Window::draw()'s own child pass from drawing them a second time: clear the damage it keys
    // off, on the window and on every child, so both of its branches find nothing left to do.
    for (int i = 0; i < children(); ++i)
        child(i)->clear_damage();
    clear_damage(static_cast<uchar>(damage() & FL_DAMAGE_CHILD));
}

void Popover::drawContent() {
    const Palette& p = activePalette();
    // Repaint the panel/bubble chrome only on a full redraw; a partial child redraw (a focused/edited
    // field) keeps the prior chrome in the backbuffer, so the body fill can't bleed over a child's box.
    if ((damage() & FL_DAMAGE_ALL) != 0) {
        if (!m_bubble) {
            fl_color(toFl(p.panelBg)); // a plain panel (== the old MOSAIC_PANEL_BOX look)
            fl_rectf(0, 0, w(), h());
            fl_color(toFl(p.border));
            fl_rect(0, 0, w(), h());
        } else if (m_bubbleSide == BubbleSide::Up) {
            const int tri = kBubbleTri;
            fl_color(toFl(p.windowBg)); // top-margin ground (cut by shape())
            fl_rectf(0, 0, w(), h());
            fl_color(toFl(p.panelBg)); // body panel below the triangle margin
            fl_rectf(0, tri, w(), h() - tri);
            drawBubbleTriangleUp(tri, kBubbleTriH, m_tipX, p.panelBg, p.windowBg, p.border);
            fl_color(toFl(p.border)); // body frame, triangle base spliced out of the top edge
            fl_line(0, tri, 0, h() - 1);
            fl_line(w() - 1, tri, w() - 1, h() - 1);
            fl_line(0, h() - 1, w() - 1, h() - 1);
            fl_line(0, tri, m_tipX - kBubbleTriH / 2, tri);
            fl_line(m_tipX + kBubbleTriH / 2, tri, w() - 1, tri);
            // A crisp hairline along the triangle's two slant edges (the tip pixel itself is cut by
            // the shape mask, so the lines read from y=1 down to the base).
            fl_line(m_tipX, 0, m_tipX - kBubbleTriH / 2, tri);
            fl_line(m_tipX, 0, m_tipX + kBubbleTriH / 2, tri);
        } else {
            const int tri = kBubbleTri;
            fl_color(toFl(p.windowBg)); // corner-margin ground (cut by shape())
            fl_rectf(0, 0, w(), h());
            fl_color(toFl(p.panelBg)); // body panel
            fl_rectf(tri, 0, w() - tri, h());
            drawBubbleTriangleLeft(tri, kBubbleTriH, m_tipY, p.panelBg, p.windowBg, p.border);
            fl_color(toFl(p.border)); // body frame, triangle base spliced out of the left edge
            fl_line(tri, 0, w() - 1, 0);
            fl_line(w() - 1, 0, w() - 1, h() - 1);
            fl_line(tri, h() - 1, w() - 1, h() - 1);
            fl_line(tri, 0, tri, m_tipY - kBubbleTriH / 2);
            fl_line(tri, m_tipY + kBubbleTriH / 2, tri, h() - 1);
            // A crisp hairline along the triangle's two slant edges (the tip pixel itself is cut by
            // the shape mask, so the lines read from x=1 across to the base).
            fl_line(0, m_tipY, tri, m_tipY - kBubbleTriH / 2);
            fl_line(0, m_tipY, tri, m_tipY + kBubbleTriH / 2);
        }
    }
    draw_children();
}

bool Popover::spansPoint(int winX, int winY) const {
    const bool inWindow = winX >= x() && winX < x() + w() && winY >= y() && winY < y() + h();
    const bool inAnchor = winX >= m_anchorX && winX < m_anchorX + m_anchorW &&
                          winY >= m_anchorY && winY < m_anchorY + m_anchorH;
    return inWindow || inAnchor;
}

void Popover::hide() {
    forgetShown(this);
    Fl_Double_Window::hide();
}

int Popover::handle(int event) {
    // A popover may host Dropdowns (the picker's model/surface combos) and themed text fields (the hex
    // / numeric readouts), whose list / right-click menu is a child of the *top-level* (a sibling of
    // this popover). A press on this popover's own dead space outside them dismisses them -- forwarded
    // in top-level coords (this popover's x()/y() are top-level-relative; the event is popover-
    // relative), the space their geometry lives in.
    if (event == FL_PUSH) {
        dismissActiveDropdownPopupOnOutsideClick(x() + Fl::event_x(), y() + Fl::event_y());
        dismissActiveContextMenuOnOutsideClick(x() + Fl::event_x(), y() + Fl::event_y());
        // ... and the colour flyout (the Type/3D panels' colour line), whose anchor chip lives IN
        // this popover -- the main window never sees presses landing here, so this is the one spot
        // that can dismiss it when the user clicks the panel's other controls (anchor spared).
        dismissActiveColorFlyoutOnOutsideClick(x() + Fl::event_x(), y() + Fl::event_y());
    }
    // Escape closes an open text-field menu first, then the popover. A focused child (e.g. the hex
    // input) ignores Escape, so it bubbles up to us here.
    if (event == FL_KEYBOARD && Fl::event_key() == FL_Escape) {
        if (activeContextMenu() != nullptr) {
            dismissActiveContextMenu();
            return 1;
        }
        hide();
        return 1;
    }
    const int handled = Fl_Double_Window::handle(event);
    // Swallow clicks on our own dead space (labels, the preview box, padding): unconsumed, they
    // would bubble out of this sub-window to the main window, whose outside-click dismissal then
    // closes the popover -- a click *inside* a popover must never dismiss it (user-reported).
    if (handled == 0 && event == FL_PUSH)
        return 1;
    return handled;
}

Popover* activePopover() {
    return g_shown.empty() ? nullptr : g_shown.back(); // the topmost = the most recently shown
}

void reanchorActivePopover() {
    // ALL of them: a window resize moves every anchor, not only the topmost popover's.
    for (Popover* p : std::vector<Popover*>(g_shown))
        p->reanchor();
}

void dismissActivePopoverOnOutsideClick(int winX, int winY) {
    // A press that lands on a popover's own child pop-ups -- the themed Dropdown list or a text-
    // field context menu, both children of the *top-level* that can extend BEYOND the popover (over
    // the canvas) -- is part of the popover's interaction and must not dismiss anything. (winX/winY
    // and the pop-ups' geometry are all in top-level coords.) Without this, on native Wayland the
    // parent's handle() sees the press before it reaches the on-top pop-up child and closes the
    // popover.
    if (const ContextMenu* m = activeContextMenu(); m != nullptr && m->spansHostPoint(winX, winY))
        return;
    if (const DropdownPopup* d = activeDropdownPopup(); d != nullptr && d->spansHostPoint(winX, winY))
        return;
    // Every one that the click is outside of, not just the topmost -- clicking away means "close
    // this lot". A popover the click landed ON is spared, which is what keeps a cascade working:
    // clicking a tool-slot button inside the overflow popover closes the variant flyout hanging off
    // the previous one and leaves the overflow itself open, because the click was inside it.
    // (spansPoint spares a popover's ANCHOR rect too, so an opener can still toggle its own shut.)
    for (Popover* p : std::vector<Popover*>(g_shown))
        if (!p->pinned() && !p->spansPoint(winX, winY))
            p->hide(); // a pinned popover (the Type panel) is closed explicitly instead
}

void dismissActivePopover() {
    for (Popover* p : std::vector<Popover*>(g_shown))
        if (!p->pinned())
            p->hide();
}

} // namespace mosaic::ui
