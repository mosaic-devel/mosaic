#pragma once

#include "ui/theme.hpp" // ThemeSubscription

#include "common/geometry.hpp" // common::Rect (corner-placement region / obstacle)

#include <FL/Fl_Double_Window.H>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class Fl_RGB_Image;

// A lightweight, themed, modeless popover (PLAN S11-d). It is a **genuine child sub-window of the
// main window** (on X11 a nested child window, on native Wayland a wl_subsurface) — emphatically NOT
// a separate top-level. That distinction is the whole design:
//   * A sub-surface is, by protocol, an integral part of its parent, so the compositor cannot give it
//     its own taskbar entry, cannot centre it (it is positioned *relative to the parent*), and cannot
//     outlive it. A borderless top-level, by contrast, showed up as a stray window in the KDE
//     taskbar, got centred on Wayland (no app-set toplevel coords), and — left shown when the main
//     window closed — kept Fl::run() alive, orphaning the app. All of that is gone here.
//   * It must be created **before the parent is show()n** (the main window builds it in its
//     constructor) — a window added to an already-realized parent and only then shown is promoted by
//     FLTK to an independent top-level, which is exactly the bug above. So it is built eagerly and
//     kept hidden, then shown/hidden on demand.
// Being a real sub-window it draws above the Vulkan-canvas sub-window and takes keyboard focus (its
// text fields work), and it reuses ordinary FLTK widgets — so the richer S12 picker (model combo,
// colour wheel, hex) just works.
//
// The main window owns it (it is one of its child widgets); openers keep only a non-owning pointer.
// Dismissal: Esc, or an outside click forwarded via dismissActivePopover{,OnOutsideClick}. At most
// one popover is open at a time.
namespace mosaic::ui {

class Popover : public Fl_Double_Window {
public:
    Popover(int W, int H);
    ~Popover() override;

    // Every overlay must assert its z-order on EVERY show, not rely on creation order. On Windows
    // it is a sibling sub-window of the Vulkan canvas, which nativeSurfaceHandle() sinks to
    // HWND_BOTTOM to act as the chrome's backdrop; FLTK creates a sub-window's HWND lazily on
    // first show(), so without this the pop-up opens UNDER the canvas and is simply invisible.
    // Overriding show() rather than patching call sites covers every subclass and every caller.
    // A no-op off Windows.
    void show() override;

    // Position next to `anchor` (a sibling under the same top-level window) and show. The popover is
    // placed just to the right of the anchor, bottom-aligned so it opens upward, clamped to stay
    // within the parent window, at its fixed construction size. Remembers the anchor (for reanchor())
    // and records its window-relative rect (for dismissal).
    void showAnchored(const Fl_Widget* anchor);

    // Re-pin to the remembered anchor at the fixed size. Call after the parent window resized (which
    // moves the anchor and would otherwise leave the popover stretched / stranded). No-op if not shown.
    void reanchor();

    // True if (winX, winY) — coordinates relative to the parent (main) window — lie within the popover
    // or its anchor rect. Used to spare the anchor and the popover from outside-click dismissal.
    [[nodiscard]] bool spansPoint(int winX, int winY) const;

    // True while this popover is shown anchored to `a`. Because outside-click dismissal spares the
    // anchor rect (so an opener *can* toggle), every anchor widget is responsible for closing its
    // own open popover on re-click — this is the query that makes that one line.
    // (Non-const only because FLTK's shown() is non-const.)
    [[nodiscard]] bool shownFor(const Fl_Widget* a) { return m_anchor == a && shown(); }

    void hide() override; // also clears the active-popover slot

    // A "pinned" popover is NOT auto-dismissed by a click outside it -- neither a canvas-work-area
    // click (dismissActivePopover) nor a click on other chrome (dismissActivePopoverOnOutsideClick).
    // The colour picker / fly-outs are not pinned (any outside click closes them); the Type panel IS
    // pinned -- it stays put while you click around the canvas/chrome and is closed explicitly instead
    // (tool switch / session end / theme change / re-clicking its button).
    [[nodiscard]] bool pinned() const noexcept { return m_pinned; }

    // Re-point a SHOWN popover at a different anchor widget without re-placing it. Needed when the
    // anchor is volatile: the Type panel's "Style…" button is destroyed + recreated whenever the options
    // bar rebuilds (e.g. on a window-width change), leaving m_anchor dangling -- the host re-points it to
    // the fresh button before the next reanchor(), so place() never derefs the freed one (a resize crash).
    void retargetAnchor(const Fl_Widget* anchor) noexcept { m_anchor = anchor; }

    // ---- Optional comic-book speech bubble (S39 follow-up) -----------------------------------
    // A left-pointing triangle aimed at the anchor, with the body balanced a tidy gap from the toolbar
    // (and, when statusBarH>0, the same gap above the status bar). Shared by the colour picker + the
    // toolbar fly-outs so they read identically. A subclass that wants it must (1) reserve kBubbleTri of
    // left margin — lay its content out that far right and size its window that much wider, only when
    // bubbleSupported() — and (2) call enableBubble() once built. setBubbleInsets() supplies the layout
    // references (the host knows the toolbar/status-bar geometry).
    static constexpr int kBubbleTri = 11;  // triangle depth (left margin reserved for it)
    static constexpr int kBubbleTriH = 20; // triangle base height
    // Whether the running platform can cut the triangle's corner margins transparent via
    // Fl_Window::shape(). False on native Wayland and on macOS (the corners are not cut on either) →
    // the popover stays a plain panel with the same balanced margins, no triangle. Authoritative at
    // construction: macOS is decided at compile time, and off it the backend is fixed by FLTK_BACKEND
    // before any window exists (activeBackend() would need a shown window).
    [[nodiscard]] static bool bubbleSupported();
    // The toolbar's right edge (left-gap reference) and the status bar's height (>0 balances the bottom
    // gap; 0 tracks the anchor vertically, for a mid-toolbar fly-out). Set by the host post-construction.
    void setBubbleInsets(int toolbarRight, int statusBarH);

protected:
    int handle(int event) override; // Esc closes

    // Which edge the bubble triangle points from: Left (the body opens to the RIGHT of a left-toolbar
    // anchor -- the picker / tool flyout) or Up (the body opens BELOW a top-bar anchor -- the shape
    // designer). The default is Left to keep existing callers unchanged.
    enum class BubbleSide : std::uint8_t { Left, Up };
    void enableBubble(BubbleSide side = BubbleSide::Left); // turn the bubble on; backend-gates the triangle
    [[nodiscard]] bool bubbleActive() const { return m_bubble; } // triangle drawn (enabled + supported)

    // Re-fill the popover's cached panelBg ground after a runtime theme change (panelBg has no FLTK
    // semantic colour, so unlike the leaf widgets it can't follow the colour map automatically).
    // Child widgets that use semantic colours / draw live re-theme on the global redraw. Virtual so
    // a content-rich popover can re-apply its own cached children too.
    virtual void reapplyTheme();

    // Change the popover's footprint. Most popovers keep their construction size, but a variable-
    // content one (the tool flyout, which sizes to its row count) calls this before each show; place()
    // and reanchor() then preserve whatever size was last set.
    void setBaseSize(int w, int h);

    // ---- Corner placement (the Type panel, docs/type-tool.md §8) ------------------------------
    // Pin to a corner of a region instead of next to the anchor. The anchor is still remembered (for
    // toggle / dismissal-sparing), it just no longer drives geometry -- so this kind of popover wants no
    // speech bubble (a pointer to a far-off button would be meaningless). `region` returns the rect, in
    // parent-window coords, the popover pins WITHIN (e.g. the canvas area); a null/empty result falls
    // back to the whole parent window. `avoid` returns an optional rect, same space, the popover should
    // not occlude -- when the chosen corner would overlap it, place() FLIPS to the horizontally-opposite
    // corner (so the panel never covers the caret/text it edits). Both are polled on each place().
    enum class Corner : std::uint8_t { BottomRight, BottomLeft };
    void setCornerPlacement(Corner preferred, std::function<common::Rect()> region,
                            std::function<std::optional<common::Rect>()> avoid = {});

    // Mark this popover pinned (survives outside clicks; see pinned()). Used by the Type panel.
    void setPinned(bool pinned) noexcept { m_pinned = pinned; }

protected:
    // Compute + apply geometry from m_anchor at the current base size. Virtual so a future popover can
    // customise placement; the base handles both the plain (right-of-anchor) and bubble layouts.
    virtual void place();

    // FINAL on purpose: it is the bracket that makes Fl_Window::shape() work off X11 (theme.hpp,
    // MOSAIC_CHROME_BOX). A subclass that painted by overriding draw() would step straight back
    // outside it and lose the popover's shape on Wayland and macOS -- override drawContent().
    void draw() final;

    // Paints the popover: chrome, then its own content, then draw_children(). Called from inside the
    // platform bracket. The base paints the panel (or the speech bubble when active), damage-aware.
    // An override owns the WHOLE window, children included, exactly as a draw() override used to.
    virtual void drawContent();

    int m_baseW; // current footprint; placement uses this, never the (possibly mid-resize) w()/h()
    int m_baseH;
    bool m_cornerPlacement = false;                            // pin to a region corner, not the anchor
    Corner m_cornerPreferred = Corner::BottomRight;            // the corner before any caret flip
    std::function<common::Rect()> m_cornerRegion;             // region (window coords) to pin within
    std::function<std::optional<common::Rect>()> m_cornerAvoid; // a rect to never occlude (flip away)
    bool m_pinned = false;                                     // true = survives outside clicks (Type panel)
    const Fl_Widget* m_anchor = nullptr; // remembered for reanchor() after a parent resize
    int m_anchorX = 0;                   // anchor rect, in parent-window coordinates
    int m_anchorY = 0;
    int m_anchorW = 0;
    int m_anchorH = 0;

private:
    void drawBracketed();    // drawContent() + suppress Fl_Window::draw()'s duplicate child pass
    void buildBubbleShape(); // (re)build + apply the shape() mask cutting the triangle's corners

    bool m_bubbleEnabled = false;          // the bubble layout/placement is on (balanced margins)
    bool m_bubble = false;                 // the triangle is actually drawn (enabled && bubbleSupported())
    BubbleSide m_bubbleSide = BubbleSide::Left;
    int m_tipY = 0;               // triangle tip, window-local y (Left bubble: tracks the anchor centre)
    int m_tipX = 0;               // triangle tip, window-local x (Up bubble: tracks the anchor centre)
    int m_toolbarRight = 0;       // left-gap reference (see setBubbleInsets)
    int m_statusBarH = 0;         // >0 balances the bottom gap; 0 tracks the anchor
    int m_shapeW = 0;             // geometry the current shape mask was built for (rebuild on change)
    int m_shapeH = 0;
    int m_shapeTipY = -1;
    std::vector<unsigned char> m_shapeBuf;    // RGBA mask backing store (kept alive for shape())
    std::unique_ptr<Fl_RGB_Image> m_shapeImg; // the mask handed to Fl_Window::shape()

    ThemeSubscription m_themeSub; // re-fills the panelBg ground on a runtime theme change
};

// The popover currently shown (at most one), or nullptr.
[[nodiscard]] Popover* activePopover();

// Re-pin the active popover to its anchor at its fixed size (after the parent window resized). No-op
// when none is open. The main window calls this from resize(), so it covers any popover (picker,
// tool flyout, future ones) without enumerating them.
void reanchorActivePopover();

// Dismiss the active popover if the press at (winX, winY) — relative to the main window — is outside
// both it and its anchor. Called by the main window on every FL_PUSH it receives (clicks land on the
// chrome widgets it hosts; the anchor is spared so the swatch can toggle the popover shut).
void dismissActivePopoverOnOutsideClick(int winX, int winY);

// Dismiss the active popover unconditionally (a no-op when none is open). For callers whose every
// click is by construction outside the popover — chiefly the canvas sub-window, which (being its own
// window) never sees clicks that landed on the popover stacked above it, and never hosts the anchor.
void dismissActivePopover();

} // namespace mosaic::ui
