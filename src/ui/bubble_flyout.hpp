#pragma once

#include "common/image.hpp" // Color8 (not used directly here but keeps the include graph obvious)

#include <FL/Fl_Double_Window.H>

#include <memory>
#include <vector>

class Fl_RGB_Image;

// The shared speech-bubble machinery for the compact editor flyouts (ui::ColorFlyout, ui::GradientFlyout)
// -- one implementation of everything that used to be hand-rolled (and repeatedly re-tuned) in each: the
// comic-book triangle, the ANCHOR GAP, the left/right auto-flip, the content shift, the transparent-corner
// window shape, and the native-Wayland plain-panel fallback. It mirrors how ui::Popover backs the main
// colour picker, so the flyouts read + behave identically to it by construction.
//
// A subclass:
//   * constructs BubbleFlyout(W, H) (a borderless child sub-window; build it BEFORE the host is shown --
//     the ui::Popover / DropdownPopup rule), then lays its own content out from kContentX (which reserves
//     the triangle margin on the X11 path);
//   * calls placeBubble(anchor) from its openFor() AFTER seeding its content and BEFORE show();
//   * calls drawBubbleChrome() from its drawContent() (NOT draw(), which is final -- see there) on a
//     full redraw, before its own content overlays, and positions any DRAWN overlay by
//     contentLeft()/contentShift();
//   * implements moveContent(delta) to shift its content WIDGETS when the content shift changes.
namespace mosaic::ui {

class BubbleFlyout : public Fl_Double_Window {
public:
    // Assert z-order on every show: on Windows this is a sibling of the Vulkan canvas (sunk to
    // HWND_BOTTOM as the chrome's backdrop) and would otherwise open UNDER it, invisibly. See
    // ui::Popover::show(). A no-op off Windows.
    void show() override;

    // Shared geometry (one definition -- referenced by every subclass's content layout).
    static constexpr int kTri = 10;               // triangle depth / strip width == the anchor GAP
    static constexpr int kTriH = 18;              // triangle base height
    static constexpr int kPad = 10;
    static constexpr int kContentX = kTri + kPad; // content-left on the X11 path (after the tri margin)

    // Whether (hostX, hostY) -- host-top-level coords -- lie within the flyout or its anchor (the anchor
    // is spared so a re-click toggles it shut; mirrors the dropdown/popover helpers).
    [[nodiscard]] bool spansHostPoint(int hostX, int hostY) const;

    // An optional "keep clear" rectangle in the PARENT (host top-level) coordinate system. If the
    // placed body would cover it, the bubble is shifted LEFT so its right edge clears the rect's left
    // edge, clamped to the left window padding (best effort when the body is too wide to fully clear).
    // Set it BEFORE openFor(); w<=0 disables. Used so a TALL flyout doesn't obscure the modal's
    // live-preview active area -- after the shift the triangle points toward the anchor across a wider
    // gap (accepted: the bubble can't move once shown, so we move before show, and not covering the
    // preview matters more than the pointer touching the anchor).
    void setAvoidRect(int x, int y, int w, int h) {
        m_avoidX = x;
        m_avoidY = y;
        m_avoidW = w;
        m_avoidH = h;
    }
    void clearAvoidRect() { m_avoidW = 0; }

protected:
    BubbleFlyout(int W, int H);

    // Place beside `anchor`: a kTri gap between the triangle tip and the anchor, opening to its RIGHT
    // (triangle points left) or flipping LEFT when there's no room, clamped into the parent; then shift
    // the content, resize, and cut the shape. Native Wayland (no shape) centres the content instead of
    // reserving a triangle margin. Records the anchor for spansHostPoint()/shownForAnchor().
    void placeBubble(const Fl_Widget* anchor);

    // The bubble chrome for a full redraw: a plain panel (native Wayland, no triangle) or the panelBg
    // body + comic-book triangle (X11/XWayland, corners cut transparent by the shape). Call before the
    // subclass draws its own content overlays.
    void drawBubbleChrome();

    // FINAL on purpose: it is the bracket that makes Fl_Window::shape() work off X11 (theme.hpp,
    // MOSAIC_CHROME_BOX). A subclass that painted by overriding draw() would step straight back
    // outside it and lose the bubble's shape on Wayland and macOS -- override drawContent().
    void draw() final;

    // Paints the flyout: drawBubbleChrome(), the subclass's content, draw_children(), any overlay.
    // Called from inside the platform bracket; the override owns the whole window, children included.
    virtual void drawContent();

    int handle(int event) override; // Esc -> hide(); a subclass overrides + chains for its own events

    // Move the subclass's content WIDGETS by `delta` px (called from placeBubble when the shift changes).
    virtual void moveContent(int delta) = 0;

    [[nodiscard]] int contentShift() const { return m_contentShift; }
    [[nodiscard]] int contentLeft() const { return kContentX + m_contentShift; } // drawn-overlay origin x

    const Fl_Widget* m_anchor = nullptr; // remembered anchor (subclass hide() clears it)

private:
    void drawBracketed();    // drawContent() + suppress Fl_Window::draw()'s duplicate child pass
    void applyBubbleShape(); // (re)build + apply the shape() mask cutting the triangle's corners

    int m_anchorX = 0, m_anchorY = 0, m_anchorW = 0, m_anchorH = 0;
    int m_avoidX = 0, m_avoidY = 0, m_avoidW = 0, m_avoidH = 0; // "keep clear" rect (w<=0 == none)
    int m_tipY = 0;         // triangle tip, window-local y (tracks the anchor centre)
    bool m_leftOpen = false; // opened LEFT of the anchor -> triangle points RIGHT
    int m_contentShift = 0;

    bool m_useShape = false;                  // backend clips Fl_Window::shape() (X11/XWayland)
    std::vector<unsigned char> m_shapeBuf;    // RGBA shape mask backing store (kept alive for shape())
    std::unique_ptr<Fl_RGB_Image> m_shapeImg; // the mask handed to shape()
};

} // namespace mosaic::ui
