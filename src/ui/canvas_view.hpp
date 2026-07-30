#pragma once

#include "common/geometry.hpp"

namespace mosaic::ui {

// The canvas viewport: an interactive pan / zoom / rotate transform mapping document pixels
// <-> on-screen canvas pixels. Pure logic (no FLTK / no Vulkan), so it is unit-tested directly
// (tests/test_canvas_view.cpp) and reused by VulkanCanvas for both interaction and rendering.
//
// Screen coordinates here are the canvas widget's **logical** pixels (the space FLTK delivers
// mouse events in); the renderer multiplies in the HiDPI content scale separately.
//
// Transform model: pan and zoom act in an *unrotated* screen frame, and the whole view then
// rotates about the viewport centre:
//     docToScreen = Rc * ( T(centre + pan) * S(zoom) * T(-docCentre) )
//     Rc          = T(centre) * R(rotation) * T(-centre)
// so rotating spins the content about the middle of the view regardless of pan/zoom -- what a
// user expects from "rotate the canvas".
class CanvasView {
public:
    static constexpr double kMinZoom = 0.01; //   1%
    static constexpr double kMaxZoom = 64.0; // 6400%

    // ---- geometry inputs ----
    void setDocumentSize(common::Vec2 sizePx) { m_docSize = clampPositive(sizePx); }
    void setViewportSize(common::Vec2 sizePx) { m_viewSize = clampPositive(sizePx); }
    [[nodiscard]] common::Vec2 documentSize() const { return m_docSize; }
    [[nodiscard]] common::Vec2 viewportSize() const { return m_viewSize; }

    // ---- view state ----
    [[nodiscard]] double zoom() const { return m_zoom; }
    [[nodiscard]] double rotation() const { return m_rotation; } // radians
    [[nodiscard]] double rotationDegrees() const;                // normalised to (-180, 180]
    [[nodiscard]] common::Vec2 pan() const { return m_pan; }

    // ---- mappings ----
    [[nodiscard]] common::Affine2D docToScreen() const;
    [[nodiscard]] common::Affine2D screenToDoc() const; // inverse (identity if singular)
    [[nodiscard]] common::Vec2 toDoc(common::Vec2 screen) const {
        return screenToDoc().apply(screen);
    }
    [[nodiscard]] common::Vec2 toScreen(common::Vec2 doc) const { return docToScreen().apply(doc); }

    // ---- interactions ----
    void panByScreen(common::Vec2 deltaScreen);                // drag content by a screen delta
    void zoomAround(common::Vec2 screenAnchor, double factor); // *= factor, keep anchor fixed
    void setZoomAround(common::Vec2 screenAnchor, double newZoom);
    void rotateBy(double deltaRadians); // about the viewport centre
    void setRotation(double radians);
    void resetRotation() { m_rotation = 0.0; }

    void fit();          // centre + zoom so the (possibly rotated) document fits the viewport
    void actualPixels(); // 100% zoom, centred (rotation kept)
    void reset() {       // fit + rotation cleared
        m_rotation = 0.0;
        fit();
    }

    // ---- save / restore (S49: each document tab remembers where it was left) ----
    // Just the user-controlled part. The document and viewport sizes are deliberately absent: the
    // canvas re-supplies those from the incoming document and its own widget rect, so a view saved
    // under one document restores cleanly under another (and after a window resize). All three
    // fields are absolute -- none is derived from the sizes -- so the order of restore vs
    // setDocumentSize does not matter.
    struct ViewState {
        double zoom = 1.0;
        double rotation = 0.0; // radians
        common::Vec2 pan{0.0, 0.0};
    };
    [[nodiscard]] ViewState state() const { return {m_zoom, m_rotation, m_pan}; }
    void setState(const ViewState& s) {
        m_zoom = s.zoom < kMinZoom ? kMinZoom : (s.zoom > kMaxZoom ? kMaxZoom : s.zoom);
        m_rotation = s.rotation;
        m_pan = s.pan;
    }

private:
    common::Vec2 m_docSize{1.0, 1.0};
    common::Vec2 m_viewSize{1.0, 1.0};
    double m_zoom = 1.0;
    double m_rotation = 0.0;      // radians
    common::Vec2 m_pan{0.0, 0.0}; // offset of doc centre from view centre, in unrotated screen px

    [[nodiscard]] common::Vec2 viewCenter() const {
        return {m_viewSize.x * 0.5, m_viewSize.y * 0.5};
    }
    [[nodiscard]] common::Vec2 docCenter() const { return {m_docSize.x * 0.5, m_docSize.y * 0.5}; }
    static common::Vec2 clampPositive(common::Vec2 v) {
        return {v.x > 1.0 ? v.x : 1.0, v.y > 1.0 ? v.y : 1.0};
    }
};

} // namespace mosaic::ui
