#include "ui/shape_gesture.hpp"

#include "common/i18n.hpp"          // N_() marks the catalogue's kind names for extraction
#include "core/vector/flatten.hpp"  // contentBounds (the resize box) + flatten (the drag wireframe)

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mosaic::ui {
namespace {

using common::Affine2D;
using common::Vec2;
namespace vec = core::vec;

constexpr double kMinExtent = 1.0;  // a drag smaller than this (doc px) is a click -> author nothing

double signOf(double v) { return v < 0.0 ? -1.0 : 1.0; }

// Round a document-space point to the nearest whole pixel (for the pixel-snap option).
Vec2 snapToPixel(Vec2 p) { return {std::round(p.x), std::round(p.y)}; }

// Set a line's weight / cap from `opts`. A line has no interior, so its stroke IS the shape -- the
// one stroke the Shape tool still authors (S26-c). The line colour is the Stroke paint (fg);
// `keepColors` preserves an existing one (an options edit) instead of taking it fresh from the
// swatch (authoring). The LineShape's own paint mode + border are left alone: they are the shape
// designer's, and a pre-S26-c Hollow/Outlined line must keep looking like one.
void applyLineStroke(vec::Object& o, const ShapeOptions& opts, bool keepColors) {
    o.stroke.enabled = true;
    o.stroke.width = std::max(0.0, opts.lineWidth);  // the weight
    o.stroke.cap = opts.cap;
    if (!keepColors || std::holds_alternative<vec::NoPaint>(o.stroke.paint))
        o.stroke.paint = vec::SolidPaint{opts.foreground};  // the line colour
}

// The parametric primitive a `size`-sized drag box describes, for every CLOSED kind (the line is
// authored from its two endpoints instead). Shared by authoring and by the designer's kind gallery,
// so converting a shape to another kind yields exactly the parameters a freshly drawn one would --
// there is one definition of "what a new heart/callout/arrow looks like", not two.
//
// The proportions the library shapes are born with are RATIOS of the box wherever the look must
// survive a resize (an arrow's head, a cross's arms, a banner's point); only genuine distances (a
// corner radius, a callout's tail) are absolute, and those are derived from the box's shorter side
// so a shape drawn at any scale reads the same.
std::optional<vec::ParametricShape> shapeInBox(ShapeKind kind, Vec2 size, const ShapeOptions& opts) {
    const double minor = std::min(std::abs(size.x), std::abs(size.y));
    switch (kind) {
        case ShapeKind::Rect:
            return vec::ParametricShape{
                vec::RectShape::uniform(size, std::clamp(opts.cornerRadius, 0.0, minor * 0.5))};
        case ShapeKind::Ellipse:
            return vec::ParametricShape{vec::EllipseShape{size * 0.5}};
        case ShapeKind::Polygon:
            // Regular: a single radius inscribed in the box's smaller half-extent (no anisotropic
            // scale, so the polygon stays regular). Shift makes the box square.
            return vec::ParametricShape{
                vec::PolygonShape{std::max(3, opts.sides), minor * 0.5, 0.0}};
        case ShapeKind::Star: {
            const double ro = minor * 0.5;
            return vec::ParametricShape{
                vec::StarShape{std::max(2, opts.points), ro,
                               ro * std::clamp(opts.innerRatio, 0.01, 0.99), 0.0, 0.0}};
        }
        case ShapeKind::Callout: {
            vec::CalloutShape c;
            c.size = size;
            // A balloon only reads as one with a soft body and a tail long enough to point at
            // something, so both are born sized to the box; the bar's corner radius overrides the
            // default body rounding when the user has set one.
            c.cornerRadius = opts.cornerRadius > 0.0
                                 ? std::clamp(opts.cornerRadius, 0.0, minor * 0.5)
                                 : minor * 0.18;
            c.tailLength = minor * 0.42;
            c.tailWidth = minor * 0.22;
            return vec::ParametricShape{c};
        }
        case ShapeKind::Arrow: {
            vec::ArrowShape a;
            a.size = size;  // length (x) x head width (y); the arrow points along +x
            return vec::ParametricShape{a};
        }
        case ShapeKind::Ring: {
            vec::RingShape r;
            r.radii = size * 0.5;
            r.innerRatio = std::clamp(opts.innerRatio, 0.0, 0.95);  // shares the star's "Inner %"
            return vec::ParametricShape{r};
        }
        case ShapeKind::Cross: {
            vec::CrossShape x;
            x.size = size;
            x.cornerRadius = std::clamp(opts.cornerRadius, 0.0, minor * 0.5);
            return vec::ParametricShape{x};
        }
        case ShapeKind::Heart: {
            vec::HeartShape hs;
            hs.size = size;
            return vec::ParametricShape{hs};
        }
        case ShapeKind::Banner: {
            vec::BannerShape b;
            b.size = size;
            b.cornerRadius = std::clamp(opts.cornerRadius, 0.0, minor * 0.5);
            return vec::ParametricShape{b};
        }
        case ShapeKind::Line: break;  // authored from its two endpoints, never from a box
    }
    return std::nullopt;
}

}  // namespace

std::optional<ShapeKind> shapeKindFor(ToolId id) {
    switch (id) {
        case ToolId::RectShape: return ShapeKind::Rect;
        case ToolId::EllipseShape: return ShapeKind::Ellipse;
        case ToolId::PolygonShape: return ShapeKind::Polygon;
        case ToolId::StarShape: return ShapeKind::Star;
        case ToolId::LineShape: return ShapeKind::Line;
        default: return std::nullopt;
    }
}

std::optional<ShapeDraft> buildShapeDraft(ShapeKind kind, Vec2 p0, Vec2 p1, bool shift, bool alt,
                                          const ShapeOptions& opts) {
    if (kind == ShapeKind::Line) {
        Vec2 d = p1 - p0;
        if (shift) {  // snap the direction to the nearest 45 degrees, keeping the length
            const double len = d.length();
            if (len > 1e-9) {
                const double a = std::round(std::atan2(d.y, d.x) / (M_PI / 4.0)) * (M_PI / 4.0);
                d = {len * std::cos(a), len * std::sin(a)};
            }
        }
        Vec2 a = alt ? p0 - d : p0;  // Alt: drag is centred on the press point
        Vec2 b = p0 + d;
        if (opts.snapToPixel) {  // land both ends on whole pixels for a crisp stroke
            a = snapToPixel(a);
            b = snapToPixel(b);
        }
        if ((b - a).length() < kMinExtent) return std::nullopt;
        const Vec2 centre = (a + b) * 0.5;

        vec::Object o;
        o.geometry = vec::ParametricShape{vec::LineShape{a - centre, b - centre}};
        o.fill = vec::NoPaint{};                         // no interior to fill -- see the header
        applyLineStroke(o, opts, /*keepColors=*/false);  // the line IS a stroke: weight/cap/colour
        return ShapeDraft{std::move(o), Affine2D::translation(centre.x, centre.y)};
    }

    Vec2 delta = p1 - p0;
    if (shift) {  // constrain to a square / circle / equal-radius box
        const double m = std::max(std::abs(delta.x), std::abs(delta.y));
        delta = {signOf(delta.x) * m, signOf(delta.y) * m};
    }
    Vec2 lo, hi;
    if (alt) {  // anchor at the centre (press point), so the box grows symmetrically
        const Vec2 h{std::abs(delta.x), std::abs(delta.y)};
        lo = p0 - h;
        hi = p0 + h;
    } else {
        lo = {std::min(p0.x, p0.x + delta.x), std::min(p0.y, p0.y + delta.y)};
        hi = {std::max(p0.x, p0.x + delta.x), std::max(p0.y, p0.y + delta.y)};
    }
    if (opts.snapToPixel) {  // snap the box to whole pixels: axis-aligned edges land on the grid
        lo = snapToPixel(lo);
        hi = snapToPixel(hi);
    }
    const Vec2 size = hi - lo;
    if (size.x < kMinExtent && size.y < kMinExtent) return std::nullopt;
    const Vec2 centre = (lo + hi) * 0.5;

    // Every closed kind is born from the box through the one shared definition (shapeInBox); a kind
    // with no box authoring (only the Line, handled above) authors nothing rather than silently
    // authoring the wrong primitive.
    const std::optional<vec::ParametricShape> shape = shapeInBox(kind, size, opts);
    if (!shape) return std::nullopt;
    vec::Object o;
    o.geometry = *shape;
    // A closed shape is a FILL and nothing else (S26-c): the outline is a Stroke layer effect now,
    // so `o.stroke` is left disabled and no second colour is consumed.
    o.fill = vec::SolidPaint{opts.foreground};
    return ShapeDraft{std::move(o), Affine2D::translation(centre.x, centre.y)};
}

const std::vector<ShapeKindInfo>& shapeKindCatalog() {
    // The five toolbar kinds first, then the S26-c library in the order a picker reads best:
    // the balloons, then the pointers, then the decorative marks.
    static const std::vector<ShapeKindInfo> kCatalog = {
        {ShapeKind::Rect, "shape_rect", N_("Rectangle")},
        {ShapeKind::Ellipse, "shape_ellipse", N_("Ellipse")},
        {ShapeKind::Polygon, "shape_polygon", N_("Polygon")},
        {ShapeKind::Star, "shape_star", N_("Star")},
        {ShapeKind::Line, "shape_line", N_("Line")},
        {ShapeKind::Callout, "shape_callout", N_("Speech bubble")},
        {ShapeKind::Arrow, "shape_arrow", N_("Arrow")},
        {ShapeKind::Ring, "shape_ring", N_("Ring / pie")},
        {ShapeKind::Cross, "shape_cross", N_("Cross")},
        {ShapeKind::Heart, "shape_heart", N_("Heart")},
        {ShapeKind::Banner, "shape_banner", N_("Chevron / banner")},
    };
    return kCatalog;
}

const ShapeKindInfo& shapeKindInfo(ShapeKind kind) {
    const std::vector<ShapeKindInfo>& all = shapeKindCatalog();
    for (const ShapeKindInfo& info : all)
        if (info.kind == kind) return info;
    return all.front();  // unreachable: the catalogue covers the whole enum
}

vec::Object convertedShape(const vec::Object& base, ShapeKind kind) {
    if (!std::holds_alternative<vec::ParametricShape>(base.geometry)) return base;
    if (shapeKindOf(base) == kind) return base;
    const std::optional<common::Rect> box = vec::contentBounds(base);
    if (!box || box->empty()) return base;
    vec::Object o = base;  // paint (and the caller's placement) ride across untouched
    if (kind == ShapeKind::Line) {  // a 1-D primitive: the box's leading diagonal
        const Vec2 h{box->w * 0.5, box->h * 0.5};
        o.geometry = vec::ParametricShape{vec::LineShape{{-h.x, -h.y}, {h.x, h.y}}};
        return o;
    }
    // Fresh defaults, sized to the box the old shape occupied, so the object keeps its footprint.
    // (A shape whose box is not centred on its origin -- a callout with a tail, a partial ring --
    // can shift by up to that offset; the designer edits geometry only, never the placement.)
    const ShapeOptions defaults;
    if (const std::optional<vec::ParametricShape> made = shapeInBox(kind, {box->w, box->h}, defaults))
        o.geometry = *made;
    return o;
}

std::vector<Vec2> shapeOutlinePolyline(const ShapeDraft& draft, const Affine2D& docToDevice) {
    // Flatten in the layer's own frame, with the local -> DEVICE transform so the curve tolerance
    // tracks zoom + HiDPI (the rasteriser's rule), then lift the points into document space.
    const vec::Contours contours =
        vec::flatten(draft.object.geometry, 0.25, docToDevice * draft.placement);
    const vec::Contour* best = nullptr;
    double bestArea = -1.0;
    for (const vec::Contour& c : contours) {  // the silhouette = the largest contour (see header)
        if (c.points.size() < 2) continue;
        Vec2 lo = c.points.front(), hi = c.points.front();
        for (const Vec2& p : c.points) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
        }
        const double area = (hi.x - lo.x) * (hi.y - lo.y);
        if (area > bestArea) {
            bestArea = area;
            best = &c;
        }
    }
    if (best == nullptr) return {};
    std::vector<Vec2> out;
    out.reserve(best->points.size() + 1);
    for (const Vec2& p : best->points) out.push_back(draft.placement.apply(p));
    if (best->closed && out.size() > 2) out.push_back(out.front());  // draw it as one open polyline
    return out;
}

std::optional<ShapeKind> shapeKindOf(const vec::Object& obj) {
    const auto* ps = std::get_if<vec::ParametricShape>(&obj.geometry);
    if (ps == nullptr) return std::nullopt;  // a Path (or future BooleanCompound) has no ShapeKind
    return std::visit(
        [](const auto& s) -> std::optional<ShapeKind> {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, vec::RectShape>) return ShapeKind::Rect;
            else if constexpr (std::is_same_v<T, vec::EllipseShape>) return ShapeKind::Ellipse;
            else if constexpr (std::is_same_v<T, vec::PolygonShape>) return ShapeKind::Polygon;
            else if constexpr (std::is_same_v<T, vec::StarShape>) return ShapeKind::Star;
            else if constexpr (std::is_same_v<T, vec::LineShape>) return ShapeKind::Line;
            else if constexpr (std::is_same_v<T, vec::CalloutShape>) return ShapeKind::Callout;
            else if constexpr (std::is_same_v<T, vec::ArrowShape>) return ShapeKind::Arrow;
            else if constexpr (std::is_same_v<T, vec::RingShape>) return ShapeKind::Ring;
            else if constexpr (std::is_same_v<T, vec::CrossShape>) return ShapeKind::Cross;
            else if constexpr (std::is_same_v<T, vec::HeartShape>) return ShapeKind::Heart;
            else if constexpr (std::is_same_v<T, vec::BannerShape>) return ShapeKind::Banner;
            else return std::nullopt;  // a primitive with no ShapeKind in this build
        },
        *ps);
}

void readShapeOptions(const vec::Object& obj, ShapeOptions& io) {
    const auto* ps = std::get_if<vec::ParametricShape>(&obj.geometry);
    if (ps == nullptr) return;
    if (std::holds_alternative<vec::LineShape>(*ps)) {  // the one stroke the bar still owns (S26-c)
        io.lineWidth = obj.stroke.width;
        io.cap = obj.stroke.cap;
        return;
    }
    std::visit(
        [&](const auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, vec::RectShape>)
                io.cornerRadius = *std::max_element(s.cornerRadius.begin(), s.cornerRadius.end());
            else if constexpr (std::is_same_v<T, vec::PolygonShape>) {
                io.sides = s.sides;
                io.cornerRadius = s.cornerRadius;
            } else if constexpr (std::is_same_v<T, vec::StarShape>) {
                io.points = s.points;
                if (std::abs(s.outerRadius) > 1e-9) io.innerRatio = s.innerRadius / s.outerRadius;
            } else if constexpr (std::is_same_v<T, vec::RingShape>) {
                io.innerRatio = s.innerRatio;  // the ring shares the star's "Inner %" hot control
            } else if constexpr (requires { s.cornerRadius; }) {
                // Rect + polygon are handled above, so this arm is the library's scalar-rounding
                // shapes only: the callout body, the cross and the banner.
                io.cornerRadius = s.cornerRadius;
            }
            // Ellipse / Line / arrow / heart: no extra hot parameter in the basic options bar.
        },
        *ps);
}

vec::Object editedObject(const vec::Object& base, const ShapeOptions& opts) {
    vec::Object o = base;
    auto* ps = std::get_if<vec::ParametricShape>(&o.geometry);
    if (ps == nullptr) return o;

    // Edit only the bar's hot parameter; size + designer-only params are preserved.
    std::visit(
        [&](auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, vec::RectShape>) {
                const double maxR = std::min(std::abs(s.size.x), std::abs(s.size.y)) * 0.5;
                const double r = std::clamp(opts.cornerRadius, 0.0, maxR);
                s.cornerRadius = {r, r, r, r};  // the bar's uniform radius (corner styles kept)
            } else if constexpr (std::is_same_v<T, vec::PolygonShape>) {
                s.sides = std::max(3, opts.sides);
            } else if constexpr (std::is_same_v<T, vec::StarShape>) {
                s.points = std::max(2, opts.points);
                s.innerRadius = s.outerRadius * std::clamp(opts.innerRatio, 0.01, 0.99);
            } else if constexpr (std::is_same_v<T, vec::RingShape>) {
                s.innerRatio = std::clamp(opts.innerRatio, 0.0, 0.95);  // the star's "Inner %"
            } else if constexpr (requires { s.cornerRadius; } && requires { s.size; }) {
                // The library's scalar-rounding shapes (callout body / cross / banner): the bar's
                // uniform radius, clamped to half the shorter side exactly as the rect's is.
                const double maxR = std::min(std::abs(s.size.x), std::abs(s.size.y)) * 0.5;
                s.cornerRadius = std::clamp(opts.cornerRadius, 0.0, maxR);
            }
        },
        *ps);

    if (std::holds_alternative<vec::LineShape>(*ps))  // weight + cap, keeping the existing colour
        applyLineStroke(o, opts, /*keepColors=*/true);
    // Every other kind: paint is untouched. The bar has no paint controls any more (S26-c), and a
    // pre-S26-c object's stroke must survive an unrelated edit to its radius / sides / points.
    return o;
}

namespace {

// Scale the size PARAMETERS of a parametric shape by (sx, sy) in place (the polygon/star get a
// single uniform factor -- they have one radius). Corner-rounding radii stay ABSOLUTE on resize
// (the Figma convention), clamped by flatten(); only the size grows. Returns false for a shape with
// no size parameter this code knows how to scale.
//
// The last two arms are STRUCTURAL, not per-type, so the shape library can grow without this
// function growing with it: any primitive whose extent is a full-size `size` or a pair of `radii`
// (which is every boxed shape in the library, by the S25 convention that the size lives in the
// parameters) resizes correctly the moment it exists. Only a primitive that spells its extent some
// other way needs an arm of its own.
bool scaleShapeParams(vec::ParametricShape& ps, double sx, double sy, double uniform) {
    return std::visit(
        [&](auto& s) -> bool {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, vec::PolygonShape>) {
                s.radius *= uniform;  // regular -> one radius scales uniformly
            } else if constexpr (std::is_same_v<T, vec::StarShape>) {
                s.outerRadius *= uniform;  // ratio preserved: scale both radii by the same factor
                s.innerRadius *= uniform;
            } else if constexpr (std::is_same_v<T, vec::LineShape>) {
                s.a = {s.a.x * sx, s.a.y * sy};  // endpoint offsets about the local origin (centre)
                s.b = {s.b.x * sx, s.b.y * sy};
            } else if constexpr (std::is_same_v<T, vec::CalloutShape>) {
                // The one library shape whose bounding box is NOT just its `size`: the tail reaches
                // past the body, so leaving the tail absolute (the corner-radius convention) would
                // make the framed box scale by less than asked and drag the anchor off. The tail
                // points in an arbitrary direction, so it takes the uniform factor.
                s.size = {s.size.x * sx, s.size.y * sy};
                s.tailLength *= uniform;
                s.tailWidth *= uniform;
            } else if constexpr (requires { s.size; }) {   // Rect + every boxed library shape
                s.size = {s.size.x * sx, s.size.y * sy};
            } else if constexpr (requires { s.radii; }) {  // Ellipse + the ring/pie family
                s.radii = {s.radii.x * sx, s.radii.y * sy};
            } else {
                return false;
            }
            return true;
        },
        ps);
}

}  // namespace

std::optional<ShapeResize> resizeShape(const vec::Object& base, const Affine2D& placement,
                                       int handle, Vec2 docPt, bool keepAspect, bool fromCenter) {
    const auto* ps = std::get_if<vec::ParametricShape>(&base.geometry);
    if (ps == nullptr || handle < 0 || handle > 7) return std::nullopt;
    const std::optional<common::Rect> box = vec::contentBounds(base);  // the framed (handle) box
    if (!box || box->empty()) return std::nullopt;
    const std::optional<Affine2D> inv = placement.inverse();
    if (!inv) return std::nullopt;
    const Vec2 pLocal = inv->apply(docPt);  // cursor in the layer's own (possibly rotated) frame

    // The handle's dragged point + the anchor (opposite handle) on the box, and which axes scale.
    // Corner handles (0-3) scale both axes; edge handles (4-7) one. fromCenter pins the centre.
    const Vec2 lo{box->x, box->y}, hi{box->right(), box->bottom()}, mid = box->center();
    const bool activeX = handle != 4 && handle != 6;  // T / B edges don't scale x
    const bool activeY = handle != 5 && handle != 7;  // L / R edges don't scale y
    Vec2 hPt, aPt;
    switch (handle) {
        case 0: hPt = lo;            aPt = hi;            break;  // TL <- BR
        case 1: hPt = {hi.x, lo.y};  aPt = {lo.x, hi.y};  break;  // TR <- BL
        case 2: hPt = hi;            aPt = lo;            break;  // BR <- TL
        case 3: hPt = {lo.x, hi.y};  aPt = {hi.x, lo.y};  break;  // BL <- TR
        case 4: hPt = {mid.x, lo.y}; aPt = {mid.x, hi.y}; break;  // T  <- B
        case 5: hPt = {hi.x, mid.y}; aPt = {lo.x, mid.y}; break;  // R  <- L
        case 6: hPt = {mid.x, hi.y}; aPt = {mid.x, lo.y}; break;  // B  <- T
        default: hPt = {lo.x, mid.y}; aPt = {hi.x, mid.y}; break;  // L  <- R
    }
    if (fromCenter) aPt = mid;

    // Per-axis box ratio (new dragged-edge offset / old), clamped so the box can't collapse / flip.
    const auto ratio = [](double pNew, double anchor, double pOld) -> double {
        const double oldSpan = pOld - anchor;
        if (std::abs(oldSpan) < 1e-9) return 1.0;
        const double newSpan = pNew - anchor;
        const double minSpan = 1.0 * (oldSpan < 0 ? -1.0 : 1.0);  // keep >= 1 doc px, same sign
        const double clamped = (oldSpan < 0) ? std::min(newSpan, minSpan) : std::max(newSpan, minSpan);
        return clamped / oldSpan;
    };
    double sx = activeX ? ratio(pLocal.x, aPt.x, hPt.x) : 1.0;
    double sy = activeY ? ratio(pLocal.y, aPt.y, hPt.y) : 1.0;
    if (keepAspect) {  // lock the aspect: the larger active factor drives both axes
        const double f = std::max(activeX ? sx : 0.0, activeY ? sy : 0.0);
        sx = activeX || activeY ? f : 1.0;
        sy = sx;
    }
    // The uniform factor a regular polygon/star takes (one radius): the dominant active axis.
    const double uniform = (activeX && activeY) ? std::max(sx, sy) : (activeX ? sx : sy);

    vec::Object o = base;
    auto* ops = std::get_if<vec::ParametricShape>(&o.geometry);
    if (!scaleShapeParams(*ops, sx, sy, uniform)) return std::nullopt;

    // Re-anchor: scaling the size params keeps the geometry centred on the local origin, so the
    // layer must shift so the conceptual scale-about-anchor S (S(p) = aPt + (p-aPt)*scale) holds.
    // The local origin maps to S(0) = aPt*(1 - scale); composing that translation onto the placement
    // pins the opposite handle in document space (and folds to no shift when fromCenter, aPt==mid==0).
    const Vec2 shift{aPt.x * (1.0 - sx), aPt.y * (1.0 - sy)};
    const Affine2D newPlacement = placement * Affine2D::translation(shift.x, shift.y);
    return ShapeResize{std::move(o), newPlacement};
}

vec::Object recoloredObject(const vec::Object& base, common::ColorF fg, common::ColorF bg) {
    vec::Object o = base;
    // A line's colour is its Stroke paint (the primary -> fg); an Outlined line's border is the fill
    // slot (the secondary -> bg). Hollow/Solid have no border colour.
    if (const auto* ps = std::get_if<vec::ParametricShape>(&o.geometry))
        if (const auto* line = std::get_if<vec::LineShape>(ps)) {
            o.stroke.paint = vec::SolidPaint{fg};
            if (line->paint == vec::LineShape::Paint::Outlined)
                o.fill = vec::SolidPaint{bg};
            return o;
        }
    const bool hasFill = !std::holds_alternative<vec::NoPaint>(o.fill);
    const bool hasStroke = o.stroke.enabled && !std::holds_alternative<vec::NoPaint>(o.stroke.paint);
    // The fill is primary (fg); a lone outline is also primary (fg); an object carrying BOTH -- only
    // a pre-S26-c document does, since the tool authors fills alone now -- keeps the old convention
    // and gives its outline the secondary accent (bg), so those shapes stay recolourable.
    if (hasFill)
        o.fill = vec::SolidPaint{fg};
    if (hasStroke)
        o.stroke.paint = vec::SolidPaint{hasFill ? bg : fg};
    return o;
}

}  // namespace mosaic::ui
