#include "ui/gradient_gesture.hpp"

#include "ui/shape_gesture.hpp" // shapeKindOf: the Shape tool's half of the select-to-edit split

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <variant>
#include <vector>

namespace mosaic::ui {

namespace vec = core::vec;
using common::Affine2D;
using common::Vec2;

namespace {

constexpr double kAxisFloor =
    1.0; // min axis length (layer units) so the transform stays invertible

// The signed axis length + angle of a drag, with an optional 45 deg snap. Returns {length, angle}.
struct AxisPolar {
    double len;
    double angle;
};
AxisPolar axisPolar(Vec2 press, Vec2 current, bool shift) {
    const Vec2 d = current - press;
    double len = std::max(std::hypot(d.x, d.y), kAxisFloor);
    double angle = std::atan2(d.y, d.x);
    if (shift) {
        constexpr double kStep = M_PI / 4.0; // 45 deg
        angle = std::round(angle / kStep) * kStep;
    }
    return {len, angle};
}

// rx, ry (the two scale magnitudes) and the rotation angle baked into a gradient transform.
struct AxisFrame {
    double rx;
    double ry;
    double angle;
};
AxisFrame frameOf(const Affine2D& t) {
    const double rx = std::hypot(t.m00, t.m10); // |x-basis|
    const double ry = std::hypot(t.m01, t.m11); // |y-basis|
    const double angle = std::atan2(t.m10, t.m00);
    return {rx, ry, angle};
}

} // namespace

GradientShape gradientShapeFromChoice(int choice) {
    switch (choice) {
    case 1:
        return GradientShape::Radial;
    case 2:
        return GradientShape::Elliptical;
    case 3:
        return GradientShape::Conic;
    default:
        return GradientShape::Linear;
    }
}

int gradientChoiceForShape(GradientShape shape) {
    switch (shape) {
    case GradientShape::Radial:
        return 1;
    case GradientShape::Elliptical:
        return 2;
    case GradientShape::Conic:
        return 3;
    case GradientShape::Linear:
        return 0;
    }
    return 0;
}

vec::DitherKind gradientDitherFromChoice(int choice) {
    switch (choice) {
    case 1:
        return vec::DitherKind::Ordered;
    case 2:
        return vec::DitherKind::BlueNoise;
    case 3:
        return vec::DitherKind::Noise;
    default:
        return vec::DitherKind::None;
    }
}

int gradientDitherChoice(vec::DitherKind kind) {
    switch (kind) {
    case vec::DitherKind::Ordered:
        return 1;
    case vec::DitherKind::BlueNoise:
        return 2;
    case vec::DitherKind::Noise:
        return 3;
    case vec::DitherKind::None:
        return 0;
    }
    return 0;
}

bool gradientToolBinds(const core::vec::Object& obj) {
    return vec::isGradient(obj.fill);
}

bool shapeToolBinds(const core::vec::Object& obj) {
    // A gradient object is excluded FIRST: its geometry is a plain full-bleed rect, so shapeKindOf
    // happily calls it a Rect and the Shape bar would bind (and then flat-fill) a gradient layer.
    return !vec::isGradient(obj.fill) && shapeKindOf(obj).has_value();
}

vec::Gradient buildGradient(GradientShape shape, Vec2 pressLocal, Vec2 currentLocal, double aspect,
                            const std::vector<vec::GradientStop>& stops, vec::SpreadMethod spread,
                            bool shift, vec::DitherKind dither) {
    const AxisPolar ap = axisPolar(pressLocal, currentLocal, shift);
    aspect = std::clamp(aspect, 1e-3, 1e3);

    vec::Gradient g;
    g.stops = stops;
    g.spread = spread;
    g.dither = dither;
    switch (shape) {
    case GradientShape::Linear:
        g.type = vec::GradientType::Linear;
        g.transform = Affine2D::trs(pressLocal, ap.angle, {ap.len, ap.len});
        break;
    case GradientShape::Radial:
        g.type = vec::GradientType::Radial; // isotropic
        g.transform = Affine2D::trs(pressLocal, ap.angle, {ap.len, ap.len});
        break;
    case GradientShape::Elliptical:
        g.type = vec::GradientType::Radial; // anisotropic -> an ellipse
        g.transform =
            Affine2D::trs(pressLocal, ap.angle, {ap.len, std::max(ap.len * aspect, kAxisFloor)});
        break;
    case GradientShape::Conic:
        g.type = vec::GradientType::Conic;
        g.transform = Affine2D::trs(pressLocal, ap.angle, {ap.len, ap.len});
        break;
    }
    return g;
}

std::optional<GradientDraft> buildGradientDraft(GradientShape shape, Vec2 pressDoc, Vec2 currentDoc,
                                                double docW, double docH,
                                                const std::vector<vec::GradientStop>& stops,
                                                vec::SpreadMethod spread, bool shift,
                                                vec::DitherKind dither) {
    const Vec2 d = currentDoc - pressDoc;
    if (std::hypot(d.x, d.y) < 1.0)
        return std::nullopt; // a sub-pixel drag makes no gradient

    const Vec2 centre{docW * 0.5, docH * 0.5};
    const Vec2 pressLocal = pressDoc - centre;
    const Vec2 currentLocal = currentDoc - centre;

    GradientDraft draft;
    draft.object.geometry = vec::ParametricShape{vec::RectShape{{docW, docH}, 0.0}};
    draft.object.fill = buildGradient(shape, pressLocal, currentLocal, kGradientDefaultAspect,
                                      stops, spread, shift, dither);
    draft.object.stroke.enabled = false;
    draft.placement = Affine2D::translation(centre.x, centre.y);
    return draft;
}

std::optional<GradientShape> gradientShapeOf(const core::vec::Object& obj) {
    const auto* g = std::get_if<vec::Gradient>(&obj.fill);
    if (g == nullptr)
        return std::nullopt;
    switch (g->type) {
    case vec::GradientType::Linear:
        return GradientShape::Linear;
    case vec::GradientType::Conic:
        return GradientShape::Conic;
    case vec::GradientType::Radial: {
        const AxisFrame f = frameOf(g->transform);
        const double m = std::max(f.rx, 1e-9);
        // A near-uniform scale is a circular radial; a clearly non-uniform one is elliptical.
        return std::abs(f.rx - f.ry) / m > 1e-3 ? GradientShape::Elliptical : GradientShape::Radial;
    }
    }
    return GradientShape::Linear;
}

GradientHandles gradientHandles(const core::vec::Object& obj, const Affine2D& layerXform) {
    GradientHandles h;
    const auto shape = gradientShapeOf(obj);
    const auto* g = std::get_if<vec::Gradient>(&obj.fill);
    if (!shape || g == nullptr)
        return h; // valid stays false
    h.shape = *shape;
    // The gradient transform maps unit-space -> layer-local; the layer transform maps -> document.
    const Affine2D unitToDoc = layerXform * g->transform;
    h.start = unitToDoc.apply({0.0, 0.0}); // centre / linear start
    h.end = unitToDoc.apply({1.0, 0.0});   // primary-axis edge
    h.minor = unitToDoc.apply({0.0, 1.0}); // minor-axis edge -- every shape's outline needs it
    // Only the Elliptical kind offers a DRAGGABLE minor handle: pulling a circular Radial's minor
    // axis would silently turn it into an Elliptical behind the bar's Type choice, so that switch
    // stays where the user can see it (the bar), and the handle appears with the shape.
    h.hasMinor = h.shape == GradientShape::Elliptical;
    h.valid = true;
    return h;
}

bool gradientHasRing(const GradientHandles& h) {
    // A linear gradient's extent IS its axis line; ringing it would be noise.
    return h.valid && h.shape != GradientShape::Linear;
}

double gradientRingDistance(Vec2 centre, Vec2 ux, Vec2 uy, Vec2 p) {
    // M = [ux uy] maps the unit circle onto the ring, so the ring is the zero set of
    // F(p) = |M^-1 (p - centre)|^2 - 1, and the signed distance to it is F / |grad F| to first
    // order, with grad F = 2 (M^-1)^T M^-1 (p - centre).
    constexpr double kNoRing = std::numeric_limits<double>::infinity();
    const double det = ux.x * uy.y - uy.x * ux.y;
    if (std::abs(det) < 1e-12)
        return kNoRing; // degenerate basis (a collapsed or collinear gradient): no ring at all
    const Vec2 d{p.x - centre.x, p.y - centre.y};
    const Vec2 q{(uy.y * d.x - uy.x * d.y) / det, (ux.x * d.y - ux.y * d.x) / det}; // M^-1 d
    const Vec2 gr{(uy.y * q.x - ux.y * q.y) / det, (ux.x * q.y - uy.x * q.x) / det}; // (M^-1)^T q
    const double gl = std::hypot(gr.x, gr.y);
    if (gl < 1e-12)
        return kNoRing; // the exact centre: grad F vanishes there, and it is never ON the ring
    return (q.x * q.x + q.y * q.y - 1.0) / (2.0 * gl);
}

core::vec::Object retypeGradient(const core::vec::Object& base, GradientShape shape) {
    const auto* g = std::get_if<vec::Gradient>(&base.fill);
    if (g == nullptr)
        return base;
    // The two points the user dragged, recovered from the transform exactly as gradientHandles does
    // -- retyping re-authors from them, so the geometry is carried across the switch untouched.
    const Vec2 centreLocal = g->transform.apply({0.0, 0.0});
    const Vec2 edgeLocal = g->transform.apply({1.0, 0.0});
    const AxisFrame f = frameOf(g->transform);
    double aspect = f.rx > 1e-9 ? f.ry / f.rx : 1.0;
    if (shape == GradientShape::Elliptical) {
        if (std::abs(aspect - 1.0) <= 1e-3)
            aspect = kGradientDefaultAspect; // a circle asked to be an ellipse must really squash
    } else {
        aspect = 1.0; // Linear/Conic ignore ry, and a Radial is isotropic by definition
    }
    core::vec::Object out = base;
    out.fill = buildGradient(shape, centreLocal, edgeLocal, aspect, g->stops, g->spread,
                             /*shift=*/false, g->dither);
    return out;
}

int hitGradientHandle(const GradientHandles& h, Vec2 docPt, double pickDoc) {
    if (!h.valid)
        return -1;
    const double r2 = pickDoc * pickDoc;
    const auto near = [&](Vec2 p) {
        const Vec2 d = docPt - p;
        return d.x * d.x + d.y * d.y <= r2;
    };
    // End/minor win over start so a coincident pair (a tiny gradient) stays grabbable.
    if (near(h.end))
        return 1;
    if (h.hasMinor && near(h.minor))
        return 2;
    if (near(h.start))
        return 0;
    // The round MIDPOINT handle the gizmo draws for every shape -> a rigid move of the gradient.
    // Without this the radial family drew a handle that did nothing (its centre handle already
    // moves it rigidly, but the drawn midpoint knob was inert).
    if (near((h.start + h.end) * 0.5))
        return 3;
    // Body (linear only): anywhere along the axis line between the two ends also moves the whole
    // gradient -- a long axis is a much bigger target than its midpoint knob.
    if (h.shape == GradientShape::Linear) {
        const Vec2 ab = h.end - h.start;
        const double len2 = ab.x * ab.x + ab.y * ab.y;
        if (len2 > 1e-9) {
            double t = ((docPt.x - h.start.x) * ab.x + (docPt.y - h.start.y) * ab.y) / len2;
            t = std::clamp(t, 0.0, 1.0);
            const Vec2 proj{h.start.x + ab.x * t, h.start.y + ab.y * t};
            if (near(proj))
                return 3;
        }
    }
    return -1;
}

core::vec::Object dragGradientHandle(const core::vec::Object& base, const Affine2D& layerXform,
                                     int handle, Vec2 pressDoc, Vec2 currentDoc, bool shift) {
    const auto shape = gradientShapeOf(base);
    const auto* g = std::get_if<vec::Gradient>(&base.fill);
    if (!shape || g == nullptr)
        return base;

    const std::optional<Affine2D> inv = layerXform.inverse();
    if (!inv)
        return base;
    const Vec2 deltaLocal = inv->apply(currentDoc) - inv->apply(pressDoc);

    // The current defining points, in layer-local space.
    Vec2 centreLocal = g->transform.apply({0.0, 0.0});
    Vec2 edgeLocal = g->transform.apply({1.0, 0.0});
    const Vec2 minorLocal = g->transform.apply({0.0, 1.0});
    const AxisFrame f = frameOf(g->transform);
    double aspect = f.rx > 1e-9 ? f.ry / f.rx : 1.0;

    switch (handle) {
    case 0: // start / centre
        if (*shape == GradientShape::Linear) {
            centreLocal = centreLocal + deltaLocal; // move start, keep end
        } else {
            centreLocal = centreLocal + deltaLocal; // rigid: centre + edge move together
            edgeLocal = edgeLocal + deltaLocal;
        }
        break;
    case 1: // end / edge
        edgeLocal = edgeLocal + deltaLocal;
        break;
    case 2: // elliptical minor edge -> sets ry (the aspect)
        if (*shape == GradientShape::Elliptical) {
            // Only the component ALONG the minor axis counts: the handle owns ry and nothing else,
            // so sliding it sideways (along the major axis) must not squash the ellipse as well.
            const Vec2 axis = edgeLocal - centreLocal;
            const Vec2 normal{-axis.y, axis.x}; // the minor-axis direction (major rotated +90 deg)
            const double rx = std::hypot(normal.x, normal.y); // == |axis|
            if (rx > 1e-9) {
                const Vec2 d = (minorLocal + deltaLocal) - centreLocal;
                aspect = std::abs(d.x * normal.x + d.y * normal.y) / (rx * rx);
            }
        }
        break;
    case 3: // body -> rigid translate
    default:
        centreLocal = centreLocal + deltaLocal;
        edgeLocal = edgeLocal + deltaLocal;
        break;
    }

    core::vec::Object out = base;
    out.fill = buildGradient(*shape, centreLocal, edgeLocal, aspect, g->stops, g->spread,
                             shift && handle == 1, g->dither);
    return out;
}

} // namespace mosaic::ui
