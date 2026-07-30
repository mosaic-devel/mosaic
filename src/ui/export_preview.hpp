#pragma once

#include "common/image.hpp"
#include "ui/cursor_apply.hpp" // MoveCursor: the substituted move cursor (see setCursorFor)
#include "ui/theme.hpp"        // ThemeSubscription: the composed buffer bakes palette colours

#include <FL/Fl_Group.H>

#include <memory>
#include <string>
#include <vector>

class Fl_RGB_Image;

// The Export modal's live preview surface: the encoded result on a transparency checkerboard,
// with cursor-anchored wheel zoom, drag-to-pan, and the two zoom buttons ("1:1" / fit) in its
// lower-right corner.
//
// It is NOT ui::PreviewPane. That one is the small, passive, always-aspect-fit viewport the
// Fill / Layer-Effects dialogs embed; this one is a viewport the user drives, so the whole
// transform is state and every draw resolves it. Keeping them apart means the passive pane stays
// as simple as it reads and no other dialog inherits a gesture it never asked for.
//
// The zoom/pan MATHS is pure and lives outside the widget (below), because the one property that
// actually matters -- the point under the pointer stays under the pointer -- is a property of a
// function, not of a window. tests/test_export_dialog.cpp pins it headlessly.
namespace mosaic::ui {

class FlatButton;

// ---- the pure zoom/pan model -----------------------------------------------------------------

// Image space -> widget space: a widget-local point p maps back to the image pixel
// (p - offset) / scale. `offset` is where the image's top-left corner lands, in widget-local px.
struct PreviewTransform {
    double scale = 1.0;
    double offsetX = 0.0;
    double offsetY = 0.0;
    bool operator==(const PreviewTransform&) const = default;
};

// One wheel detent's zoom factor, and the absolute ceiling (a generous pixel-peeping range; the
// FLOOR is relative to the fit, see previewMinScale).
inline constexpr double kPreviewZoomStep = 1.15;
inline constexpr double kPreviewMaxZoom = 32.0;

// The scale at which the whole image fits inside the viewport. 0 for a degenerate input (an
// empty image or a zero-sized viewport) -- callers treat that as "nothing to show".
[[nodiscard]] double previewFitScale(int imageW, int imageH, int viewW, int viewH);

// The zoom range. The floor is min(fit, 1) -- zooming out past the fit only shrinks a centred
// picture, but a THUMBNAIL-sized image (fit > 1) must still be viewable at its true 100%. The
// ceiling is at least the fit, so a tiny image whose fit exceeds the ceiling is not clamped
// below its own resting scale.
[[nodiscard]] double previewMinScale(double fitScale) noexcept;
[[nodiscard]] double previewMaxScale(double fitScale) noexcept;
[[nodiscard]] double clampPreviewScale(double scale, double fitScale) noexcept;

// The resting transform: the image scaled to fit and centred.
[[nodiscard]] PreviewTransform previewFitTransform(int imageW, int imageH, int viewW, int viewH);

// Re-scale about a widget-local cursor: THE contract of this file -- the image point under
// (cursorX, cursorY) is the same before and after. Does no clamping (compose it with
// clampPreviewScale / clampPreviewPan), so the property is exactly testable.
[[nodiscard]] PreviewTransform previewZoomAt(const PreviewTransform& t, double newScale,
                                             double cursorX, double cursorY);

// Keep the picture where it can be seen: an axis whose scaled extent is SMALLER than the
// viewport is centred on it (there is no meaningful pan there), a larger one is clamped so its
// edge can never be dragged past the matching viewport edge.
[[nodiscard]] PreviewTransform clampPreviewPan(const PreviewTransform& t, int imageW, int imageH,
                                               int viewW, int viewH);

// `scale` after `wheelSteps` detents (FLTK's Fl::event_dy(): positive = wheel down = zoom out).
[[nodiscard]] double previewWheelScale(double scale, int wheelSteps) noexcept;

// ---- the widget -------------------------------------------------------------------------------

class ExportPreview : public Fl_Group {
public:
    ExportPreview(int X, int Y, int W, int H);

    // A STRAIGHT-alpha RGBA image (the encoded bytes decoded back, or the resize behind them).
    // The zoom/pan survives a same-size refresh -- a quality-slider drag must not throw the user
    // out of the detail they were inspecting -- and refits whenever the output size changes.
    void setImage(const common::Image& rgba);
    void clearImage();
    void setNote(std::string note); // the empty-state caption

    // While a pipeline job runs: draws a small "rendering" pill and keeps the LAST good frame
    // (§6 "last good render stays visible during recompute").
    void setBusy(bool busy);

    void fitToView();    // the "reset zoom" button
    void actualPixels(); // the "1:1" button, anchored on the viewport centre

    [[nodiscard]] PreviewTransform transform() const noexcept { return m_view; }
    [[nodiscard]] double zoom() const noexcept { return m_view.scale; }
    [[nodiscard]] bool hasImage() const noexcept { return m_hasImage; }

    void reapplyTheme(); // the composed buffer bakes palette colours: drop it and repaint

protected:
    void draw() override;
    int handle(int event) override;
    void resize(int X, int Y, int W, int H) override;

private:
    void layoutButtons();
    void compose();                       // (re)build m_buf for the current transform
    void applyView(const PreviewTransform& t, bool userDriven);
    [[nodiscard]] bool overButton(int ex, int ey) const;
    void setCursorFor(int ex, int ey);

    common::Image m_premul;               // the preview, PREMULTIPLIED (band-free minification)
    int m_imgW = 0, m_imgH = 0;
    bool m_hasImage = false;
    bool m_busy = false;
    std::string m_note;

    PreviewTransform m_view;
    bool m_userZoomed = false;            // the transform is the user's, not the resting fit
    bool m_panning = false;
    int m_dragX = 0, m_dragY = 0;         // last pointer position during a pan

    std::vector<unsigned char> m_buf;     // composed RGB (depth 3 -- never blit depth 4)
    bool m_bufValid = false;

    FlatButton* m_btnActual = nullptr;    // "1:1"
    FlatButton* m_btnFit = nullptr;       // reset zoom
    ThemeSubscription m_themeSub;

    // The four-way move arrow, substituted for the stock FL_CURSOR_MOVE on Wayland only (and
    // caching its rasterized bitmap, which reapplyTheme can invalidate under us).
    MoveCursor m_moveCursor;
};

} // namespace mosaic::ui
