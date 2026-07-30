#pragma once

#include "common/image.hpp" // common::Color8
#include "core/brush/curve.hpp"

#include <FL/Fl_Widget.H>

#include <functional>

// The response-curve editor (docs/brushes.md §8.3's "net-new widget"): direct manipulation of a
// core::brush::Curve over the unit square. ONE widget, three consumers -- Settings -> Tablet's
// global pressure curve (docs/tablet.md §7) today, and in Arc D the brush editor's per-option
// sensor curves and the preset importer's round-trip surface. It edits the Curve type itself, which
// is what makes an imported preset's curve survive a visit to the editor byte-exactly ("x,y;x,y;",
// six-significant-digit %g, corner flags and all).
//
// Interaction, following the direct-manipulation convention every comparable editor uses:
//   - drag a point           move it. The two END points are pinned in x (at 0 and 1) and move only
//                            in y: a curve whose domain does not span [0,1] has no meaning for a
//                            sensor whose reading is normalized to it.
//   - click empty space      add a point there.
//   - right-click a point    remove it (never an endpoint -- the curve must keep its domain).
//   - double-click a point   toggle CORNER (the spline arrives and leaves with zero second
//                            derivative, so the two sides become independent -- curve.hpp). A corner
//                            reads as a diamond, a smooth point as a circle.
// A point dragged onto another's x is nudged off it: Curve drops points that share an x (a
// zero-width interval has no spline), and silently losing the point you are dragging is nobody's
// idea of direct manipulation.
namespace mosaic::ui {

class CurveEditor : public Fl_Widget {
public:
    CurveEditor(int X, int Y, int W, int H, const char* label = nullptr);

    // Seed the editor. Does NOT fire the change callback -- seeding is not editing, and a Settings
    // pane that re-seeded on open would write the value straight back to disk.
    void setCurve(const core::brush::Curve& c);
    [[nodiscard]] const core::brush::Curve& curve() const noexcept { return m_curve; }

    // Fired on every edit that changes the curve (drag, add, remove, corner toggle, reset), with
    // the curve as it now stands. The host persists + applies it.
    void onChanged(std::function<void(const core::brush::Curve&)> cb) { m_onChanged = std::move(cb); }

    // Back to the identity (0,0)->(1,1). FIRES the callback: it is an edit like any other.
    void reset();

    // An optional BACKDROP HISTOGRAM drawn under the plot (S34-a): `bins01` is 256 values already
    // normalised into [0,1] by the caller -- the very data path the Levels/Threshold strips have
    // always used (ui/adjustment_panel.cpp), reused rather than duplicated -- and `tint` is the
    // bar colour, so a per-channel tone curve can show its OWN channel's distribution and say so
    // without a legend. An empty vector clears it, which is the default state: the tablet-settings
    // consumer of this widget has no image to plot.
    //
    // Drawing a distribution is all this is. ⚠ INVARIANT: nothing in this widget (or anywhere else)
    // may DERIVE a curve from it -- no "Auto" button, no curve computed from image statistics. The
    // knots are always the user's. That is a hard constraint on the feature, not a missing nicety.
    void setHistogram(std::vector<float> bins01, common::Color8 tint);
    // What the plot is currently drawing behind itself (empty = none). Exposed so the panel's
    // headless test can pin that the per-channel picker really swaps the distribution.
    [[nodiscard]] const std::vector<float>& histogram() const noexcept { return m_bins; }

    // The ground the editor clears the margin AROUND its plot to (parity with ui::Slider /
    // ui::Dial / ScrubSlider). draw() owns every pixel of its rect: the plot is inset by kPad, and
    // if the ring outside it is not painted the widget shows whatever was under it -- stale content
    // inside a scrolling pane, which is exactly what Settings → Tablet shipped with. Defaults to the
    // panel ground; set it when the editor sits on a different one.
    void setCellColor(common::Color8 c) {
        m_cellColor = c;
        m_cellColorSet = true;
        redraw();
    }

protected:
    void draw() override;
    int handle(int event) override;

private:
    // The plot rectangle, inset from the widget so a handle at a corner is not half-clipped.
    [[nodiscard]] int plotX() const noexcept;
    [[nodiscard]] int plotY() const noexcept;
    [[nodiscard]] int plotW() const noexcept;
    [[nodiscard]] int plotH() const noexcept;
    // Curve space [0,1]^2 <-> widget pixels. y is FLIPPED: 1.0 is at the TOP, like every graph.
    [[nodiscard]] double toPixelX(double cx) const noexcept;
    [[nodiscard]] double toPixelY(double cy) const noexcept;
    [[nodiscard]] double toCurveX(int px) const noexcept;
    [[nodiscard]] double toCurveY(int py) const noexcept;
    // Index of the control point within grab range of (px, py), or -1.
    [[nodiscard]] int pointAt(int px, int py) const noexcept;
    void commit(); // rebuild m_curve from m_points and fire the callback

    core::brush::Curve m_curve;
    std::vector<core::brush::CurvePoint> m_points; // the live, editable copy (sorted by x)
    std::vector<float> m_bins;                     // optional backdrop histogram, 256 x [0,1]
    common::Color8 m_binTint{};
    std::function<void(const core::brush::Curve&)> m_onChanged;
    int m_drag = -1;  // index being dragged, or -1
    int m_hover = -1; // index under the cursor, or -1
    common::Color8 m_cellColor{};
    bool m_cellColorSet = false;
};

} // namespace mosaic::ui
