#include "ui/canvas_view.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::ui {

using common::Affine2D;
using common::Vec2;

namespace {
constexpr double kPi = 3.14159265358979323846;
double clampZoom(double z) {
    return std::clamp(z, CanvasView::kMinZoom, CanvasView::kMaxZoom);
}
} // namespace

Affine2D CanvasView::docToScreen() const {
    const Vec2 c = viewCenter();
    const Vec2 dc = docCenter();
    // Pan + zoom in an unrotated frame: doc centre -> view centre + pan.
    const Affine2D base = Affine2D::translation(c.x + m_pan.x, c.y + m_pan.y) *
                          Affine2D::scaling(m_zoom, m_zoom) * Affine2D::translation(-dc.x, -dc.y);
    // Then rotate the whole view about the viewport centre.
    const Affine2D rc = Affine2D::translation(c.x, c.y) * Affine2D::rotation(m_rotation) *
                        Affine2D::translation(-c.x, -c.y);
    return rc * base;
}

Affine2D CanvasView::screenToDoc() const {
    if (const auto inv = docToScreen().inverse())
        return *inv;
    return Affine2D::identity();
}

void CanvasView::panByScreen(Vec2 deltaScreen) {
    // pan lives in the unrotated frame; un-rotate the screen delta into it.
    m_pan = m_pan + Affine2D::rotation(-m_rotation).applyVector(deltaScreen);
}

void CanvasView::setZoomAround(Vec2 screenAnchor, double newZoom) {
    const double z = clampZoom(newZoom);
    const Vec2 docP = toDoc(screenAnchor); // the doc point currently under the anchor
    const Vec2 c = viewCenter();
    const Vec2 dc = docCenter();
    // Keep docToScreen(docP) == screenAnchor with the new zoom:
    //   pan = R^-1(anchor - centre) - z * (docP - docCentre)
    m_zoom = z;
    m_pan = Affine2D::rotation(-m_rotation).applyVector(screenAnchor - c) - (docP - dc) * z;
}

void CanvasView::zoomAround(Vec2 screenAnchor, double factor) {
    if (factor > 0.0)
        setZoomAround(screenAnchor, m_zoom * factor);
}

void CanvasView::rotateBy(double deltaRadians) {
    m_rotation += deltaRadians;
}

void CanvasView::setRotation(double radians) {
    m_rotation = radians;
}

double CanvasView::rotationDegrees() const {
    double deg = std::fmod(m_rotation * 180.0 / kPi, 360.0);
    if (deg > 180.0)
        deg -= 360.0;
    if (deg <= -180.0)
        deg += 360.0;
    // Normalise -0.0 to 0.0 for a clean readout.
    return deg == 0.0 ? 0.0 : deg;
}

void CanvasView::fit() {
    m_pan = {0.0, 0.0};
    // Fit the document's *rotated* axis-aligned bounding box, so the whole page stays visible
    // even when the view is rotated.
    const double c = std::abs(std::cos(m_rotation));
    const double s = std::abs(std::sin(m_rotation));
    const double boundsW = m_docSize.x * c + m_docSize.y * s;
    const double boundsH = m_docSize.x * s + m_docSize.y * c;
    const double zx = m_viewSize.x / boundsW;
    const double zy = m_viewSize.y / boundsH;
    m_zoom = clampZoom(std::min(zx, zy));
}

void CanvasView::actualPixels() {
    m_pan = {0.0, 0.0};
    m_zoom = clampZoom(1.0);
}

} // namespace mosaic::ui
