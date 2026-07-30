#pragma once

#include <cmath>
#include <limits>

// The brush reticle's SCREEN geometry (docs/brushes.md §6.3).
//
// The ruling, settled and not a setting: the reticle traces the TIP'S SHAPE at its CONFIGURED size.
// So the moment a preset gives a tip a `ratio` and an `angle`, the ring stops being a circle and
// becomes that ellipse -- otherwise it is drawing a promise the dab does not keep.
//
// ⚠ And an ellipse is only the truth for an ELLIPTICAL tip. A bristle, a spatter, a ring or a spiked
// star is not one, and an oval drawn over it is the same lie in a smaller size: a user who picks
// `i)_Wet_Bristles_Rough` and gets a smooth oval has been told the wrong thing about where paint will
// land. So this file supplies only the tip's screen FRAME (its two semi-axes and its rotation); what
// fills that frame is either the analytic ellipse -- for the tips that genuinely are one -- or the
// tip's own traced contour, a signed distance field the shader samples
// (core/brush/tip_outline.hpp, `tipNeedsSdf` decides which).
//
// Two things it deliberately does NOT do:
//   - track live pressure. A pressure-thinned dab under a ring that stays put is correct, and it is
//     what every comparable editor does; a ring that breathed with the pen would be unusable as a
//     size gauge, which is the one job it has.
//   - offer a choice. There is no setting.
//
// ⚠ But it DOES track DIRECTION, and that is not a contradiction -- it is the other half of the same
// ruling. A magnitude (pressure, speed) says how much paint; an ORIENTATION says WHERE THE PAINT WILL
// LAND, which is the reticle's whole job. 14 of the 82 shipped presets turn their nib to follow the
// stroke and the ring used to sit there showing the authored slant, which is a lie of exactly the kind
// §6.3 exists to prevent. The rule and the machinery live in core::brush::reticleDabAngle; the heading
// it needs comes from HoverHeading below (on hover) or from the live stroke (during one).
//
// Pure, header-only and FLTK-free so that it can be unit-tested. That matters here more than usual:
// the shader that actually draws the ellipse cannot be tested at all, so the half that CAN be is the
// half worth getting under test -- and it is the half that goes wrong quietly (an angle that turns
// the wrong way under a rotated canvas, a semi-axis that forgets the zoom).
namespace mosaic::ui {

// The tip's outline in the canvas widget's logical px, in the SCREEN frame.
struct ReticleShape {
    double semiX = 0.5;    // half-width along the tip's own x
    double semiY = 0.5;    // ... and its y. Equal to semiX for a round tip.
    double angleRad = 0.0; // rotation on SCREEN: the tip's own angle plus the view's
};

// The tip's own EXTENT (`extentW` x `extentH`, DOCUMENT px, before rotation), turned into a screen
// shape. This is the general form, and the one a real tip must go through: a bitmap tip's box is NOT
// `diameter x diameter*ratio` -- `diameter` sets its LONG axis and the frame's own aspect fills the
// rest in (core::brush::tipDabShape), so a 300x80 stamp under the diameter-and-ratio form below would
// be ringed by a box nearly four times too tall.
//
// `zoom` and `viewRotation` come from ui::CanvasView, whose transform is
//     docToScreen = Rc * (T(centre + pan) * S(zoom) * T(-docCentre))
// -- so the zoom is ISOTROPIC (it scales both semi-axes alike and cannot shear a circle into an
// ellipse) and the view's rotation simply ADDS to any direction expressed in the document. There is
// no mirror in the transform, so no term flips the angle's sense.
[[nodiscard]] inline ReticleShape reticleShapeFromExtent(double extentW, double extentH,
                                                         double angleRad, double zoom,
                                                         double viewRotation) noexcept {
    ReticleShape s;
    // A half-pixel floor, matching the shader's own: a zero or nonsensical tip still shows the user
    // WHERE the brush is, which is the more useful of the reticle's two jobs when the other has
    // broken down. (A preset is an untrusted file; a `ratio` of 0 is legal and paints nothing.)
    const double z = (std::isfinite(zoom) && zoom > 0.0) ? zoom : 1.0;
    const double w = (std::isfinite(extentW) && extentW > 0.0) ? extentW : 0.0;
    const double h = (std::isfinite(extentH) && extentH > 0.0) ? extentH : 0.0;
    s.semiX = std::fmax(0.5 * w * z, 0.5);
    s.semiY = std::fmax(0.5 * h * z, 0.5);
    s.angleRad = (std::isfinite(angleRad) ? angleRad : 0.0) +
                 (std::isfinite(viewRotation) ? viewRotation : 0.0);
    return s;
}

// `diameter` is the tip's width in DOCUMENT px, `ratio` its height/width, `angleRad` its authored
// rotation in the DOCUMENT's frame (core::brush::BrushParams). The shape of the engine's built-in
// analytic circle -- i.e. of a NULL tip, which is what every brush that predates the preset library
// lays.
[[nodiscard]] inline ReticleShape reticleShape(double diameter, double ratio, double angleRad,
                                               double zoom, double viewRotation) noexcept {
    const double d = (std::isfinite(diameter) && diameter > 0.0) ? diameter : 0.0;
    const double r = (std::isfinite(ratio) && ratio > 0.0) ? ratio : 1.0;
    return reticleShapeFromExtent(d, d * r, angleRad, zoom, viewRotation);
}

// Is the tip big enough ON SCREEN for its traced outline to be worth drawing?
//
// Below this the contour of a bristly or spattered tip degenerates into a blob a pixel or two wide --
// it says nothing about the tip's shape and rather less about its size than a plain ring does. The
// analytic ellipse (which the shader floors at a half pixel) is the better answer there, and it is
// the fallback the reference takes too. Above it, the outline is the truth and the ellipse is a lie.
inline constexpr double kReticleTraceMinSemiPx = 1.5;
[[nodiscard]] inline bool reticleTracesTip(const ReticleShape& s) noexcept {
    return s.semiX >= kReticleTraceMinSemiPx && s.semiY >= kReticleTraceMinSemiPx;
}

// How far the pointer must travel before it is deemed to have a direction, in the canvas widget's
// LOGICAL px.
//
// ⚠ It is a SCREEN threshold on purpose, even though the heading it gates is a DOCUMENT one. The
// noise being filtered is screen-space input jitter -- an integer mouse position wobbling by a pixel,
// a hand resting on a stylus. A document-space threshold would demand a 2 px hand tremor at 10 % zoom
// (impossible: one screen px is ten document px) and wave a 200 px sweep through at 1000 %. The frames
// are different because the questions are: "has the pointer moved?" is about the pointer.
inline constexpr double kHoverHeadingMinTravelPx = 3.0;

// The pointer's heading, for a reticle that has no stroke to read one from.
//
// It re-anchors only once the pointer has actually travelled, and holds its last heading in between.
// That low-passes the jitter for free, and -- the part that matters -- a pointer that has NEVER moved
// has NO heading (NaN), not a fictitious one of zero. A tip must not snap to "pointing east" the
// instant the cursor enters the canvas.
class HoverHeading {
public:
    void reset() noexcept {
        m_armed = false;
        m_heading = std::numeric_limits<double>::quiet_NaN();
    }

    // One pointer position. `screen` decides whether it MOVED; `doc` is what the heading is measured
    // in, because that is the frame a dab's angle lives in -- the view's rotation is added back when
    // the ring is drawn (reticleShapeFromExtent), and measuring the heading on screen would apply it
    // twice.
    void moveTo(double screenX, double screenY, double docX, double docY) noexcept {
        if (!m_armed) {
            m_armed = true;
            m_sx = screenX;
            m_sy = screenY;
            m_dx = docX;
            m_dy = docY;
            return;
        }
        const double travel = std::hypot(screenX - m_sx, screenY - m_sy);
        if (!(travel >= kHoverHeadingMinTravelPx))
            return; // NaN-safe: an unreadable position moves nothing
        const double ddx = docX - m_dx;
        const double ddy = docY - m_dy;
        if (ddx != 0.0 || ddy != 0.0)
            m_heading = std::atan2(ddy, ddx);
        m_sx = screenX;
        m_sy = screenY;
        m_dx = docX;
        m_dy = docY;
    }

    // NaN until the pointer has travelled: "no direction yet", which the angle rule reads as "keep the
    // tip's authored angle".
    [[nodiscard]] double headingRad() const noexcept { return m_heading; }

private:
    double m_sx = 0.0;
    double m_sy = 0.0;
    double m_dx = 0.0;
    double m_dy = 0.0;
    double m_heading = std::numeric_limits<double>::quiet_NaN();
    bool m_armed = false;
};

} // namespace mosaic::ui
