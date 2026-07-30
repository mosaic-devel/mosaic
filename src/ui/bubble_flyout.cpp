#include "ui/bubble_flyout.hpp"

#include "ui/popover.hpp" // Popover::bubbleSupported (shape() is backend-gated)
#include "ui/theme.hpp"

#include <FL/Fl.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>

#include <algorithm>

namespace mosaic::ui {

namespace {
Fl_Color toFl(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }
} // namespace

BubbleFlyout::BubbleFlyout(int W, int H) : Fl_Double_Window(0, 0, W, H) {
    border(0);                             // a borderless child sub-window (mirrors DropdownPopup)
    color(toFl(activePalette().windowBg)); // triangle-margin ground (re-set in placeBubble for re-theme)
    m_useShape = Popover::bubbleSupported();
    // Claim the shape while this sub-window is still unmapped -- placeBubble() builds the real mask
    // far too late for native Wayland. See seedOpaqueWindowShape (theme.hpp).
    if (m_useShape)
        seedOpaqueWindowShape(*this, m_shapeBuf, m_shapeImg);
}

void BubbleFlyout::placeBubble(const Fl_Widget* anchor) {
    m_anchor = anchor;
    color(toFl(activePalette().windowBg)); // refresh the ground (re-theme safe)

    // Anchor rect in the host top-level's coordinates (walk up any nested sub-windows).
    int ax = anchor->x();
    int ay = anchor->y();
    for (Fl_Window* p = anchor->window(); p != nullptr && p != window(); p = p->window()) {
        ax += p->x();
        ay += p->y();
    }
    m_anchorX = ax;
    m_anchorY = ay;
    m_anchorW = anchor->w();
    m_anchorH = anchor->h();

    // A kTri GAP between the triangle tip and the anchor: the body opens to the RIGHT (triangle points
    // left across the gap), flipping LEFT when there's no room. Same idea + gap as the Popover bubble.
    int px = ax + anchor->w() + kTri;
    int py = ay + anchor->h() / 2 - h() / 2;
    m_leftOpen = false;
    if (const Fl_Window* parent = window()) {
        if (px + w() + kPad > parent->w() && ax - w() - kTri >= kPad) {
            px = ax - w() - kTri;
            m_leftOpen = true;
        }
        px = std::clamp(px, kPad, std::max(kPad, parent->w() - w() - kPad));
        py = std::clamp(py, kPad, std::max(kPad, parent->h() - h() - kPad));

        // Keep clear of an important region (the modal's preview active area): if the placed body
        // overlaps it in BOTH axes, shift LEFT so its right edge clears the rect's left edge, clamped
        // to the left wall. The body is then left of the anchor -> force left-open so the triangle
        // points right toward it. (Done before the content shift + resize, so contents move with it.)
        if (m_avoidW > 0) {
            const bool yOverlap = py < m_avoidY + m_avoidH && py + h() > m_avoidY;
            const bool xOverlap = px < m_avoidX + m_avoidW && px + w() > m_avoidX;
            if (yOverlap && xOverlap) {
                constexpr int kAvoidGap = 2;  // sit right up against the kept-clear region
                px = std::clamp(m_avoidX - kAvoidGap - w(), kPad,
                                std::max(kPad, parent->w() - w() - kPad));
                m_leftOpen = true;
            }
        }
    }

    // Content shift: reserve the triangle margin on the pointing side (right-open keeps it on the left,
    // left-open moves the body left). With no triangle (native Wayland) there is no margin to reserve,
    // so centre the content instead (-kTri/2, since kContentX == kTri+kPad and the window is kTri wider).
    const int desiredShift = !m_useShape ? -kTri / 2 : (m_leftOpen ? -kTri : 0);
    if (const int delta = desiredShift - m_contentShift; delta != 0) {
        moveContent(delta);
        m_contentShift = desiredShift;
    }

    // The triangle tip tracks the anchor centre even after the body is clamped into the window.
    m_tipY = std::clamp((ay + anchor->h() / 2) - py, kTriH / 2 + kPad, h() - kTriH / 2 - kPad);
    resize(px, py, w(), h());
    applyBubbleShape(); // before show(), like the Popover
}

void BubbleFlyout::applyBubbleShape() {
    if (!m_useShape) return; // no triangle at all (drawBubbleChrome draws a plain panel instead)
    buildBubbleShapeMask(m_shapeBuf, w(), h(), kTri, kTriH, m_tipY, /*rightSide=*/m_leftOpen);
    m_shapeImg = std::make_unique<Fl_RGB_Image>(m_shapeBuf.data(), w(), h(), 4);
    shape(m_shapeImg.get());
}

void BubbleFlyout::draw() {
    // See Popover::draw(): the platform driver's draw_begin()/draw_end() bracket IS shape() off X11,
    // and only Fl_Window::draw() runs it, so the painting goes through the chrome box.
    ScopedChromePainter bind([](void* ud) { static_cast<BubbleFlyout*>(ud)->drawBracketed(); }, this);
    box(MOSAIC_CHROME_BOX);
    Fl_Double_Window::draw();
}

void BubbleFlyout::drawBracketed() {
    drawContent();
    // drawContent() drew the children itself; stop Fl_Window::draw()'s own child pass repeating it.
    for (int i = 0; i < children(); ++i)
        child(i)->clear_damage();
    clear_damage(static_cast<uchar>(damage() & FL_DAMAGE_CHILD));
}

void BubbleFlyout::drawContent() {
    drawBubbleChrome();
    draw_children();
}

void BubbleFlyout::drawBubbleChrome() {
    const Palette& p = activePalette();
    if (!m_useShape) {
        // A platform that cannot cut the corners gets NO triangle stripe -- a plain panel (the main
        // colour picker's fallback), not an opaque-corner bubble. Every current platform can.
        fl_color(toFl(p.panelBg));
        fl_rectf(0, 0, w(), h());
        fl_color(toFl(p.border));
        fl_rect(0, 0, w(), h());
        return;
    }
    // The whole window first paints the triangle-margin ground; shape() cuts the corners transparent.
    fl_color(toFl(p.windowBg));
    fl_rectf(0, 0, w(), h());
    if (m_leftOpen) {
        // Body on the LEFT, triangle margin on the RIGHT pointing at the anchor. NB: the shape() mask
        // makes column x==rx (== W-kTri) opaque ONLY inside the triangle wedge, so the body's right
        // edge must be drawn on x==rx-1 (the last fully-opaque body column) -- drawing it at rx got
        // CUT above/below the triangle, leaving that side of the bubble with no outline (user-reported;
        // the right-open case below is unaffected because its base column x==kTri stays body-opaque).
        const int rx = w() - kTri; // triangle base / strip left
        const int be = rx - 1;     // body right edge (last opaque body column)
        fl_color(toFl(p.panelBg));
        fl_rectf(0, 0, rx, h());
        drawBubbleTriangleRight(rx, kTri, kTriH, m_tipY, p.panelBg, p.windowBg, p.border);
        fl_color(toFl(p.border));
        fl_line(0, 0, be, 0);
        fl_line(0, 0, 0, h() - 1);
        fl_line(0, h() - 1, be, h() - 1);
        fl_line(be, 0, be, m_tipY - kTriH / 2);
        fl_line(be, m_tipY + kTriH / 2, be, h() - 1);
    } else {
        const int bx = kTri; // body left edge / triangle base
        fl_color(toFl(p.panelBg));
        fl_rectf(bx, 0, w() - bx, h());
        drawBubbleTriangleLeft(kTri, kTriH, m_tipY, p.panelBg, p.windowBg, p.border);
        fl_color(toFl(p.border));
        fl_line(bx, 0, w() - 1, 0);
        fl_line(w() - 1, 0, w() - 1, h() - 1);
        fl_line(bx, h() - 1, w() - 1, h() - 1);
        fl_line(bx, 0, bx, m_tipY - kTriH / 2);
        fl_line(bx, m_tipY + kTriH / 2, bx, h() - 1);
    }
}

bool BubbleFlyout::spansHostPoint(int hostX, int hostY) const {
    const bool inFlyout = hostX >= x() && hostX < x() + w() && hostY >= y() && hostY < y() + h();
    const bool inAnchor = hostX >= m_anchorX && hostX < m_anchorX + m_anchorW && hostY >= m_anchorY &&
                          hostY < m_anchorY + m_anchorH;
    return inFlyout || inAnchor;
}

int BubbleFlyout::handle(int event) {
    if (event == FL_KEYBOARD && Fl::event_key() == FL_Escape) {
        hide();
        return 1;
    }
    if (const int r = Fl_Double_Window::handle(event); r != 0) return r;
    // The flyout body is OPAQUE to pointer presses: a press/drag/release that missed every child
    // widget is eaten here so it can't fall through this borderless sub-window to whatever host
    // control sits behind it (user-reported: clicking the bubble chrome hit the control underneath).
    // The transparent shaped corners are a separate matter -- XShape drops their input by design, so a
    // click there still passes through (they are, visually, not part of the bubble).
    if (event == FL_PUSH || event == FL_DRAG || event == FL_RELEASE) return 1;
    return 0;
}

} // namespace mosaic::ui
