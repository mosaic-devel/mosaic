#include "ui/shape_designer.hpp"

#include "ui/gizmo_canvas.hpp"  // the AA'd diagram (fl_pie / fl_arc handles read stair-stepped)
#include "ui/icon_pack.hpp"     // the kind gallery draws the toolbar's own shape art
#include "ui/scrub_slider.hpp"  // the app's value slider (type-in + precision drag), not Fl_Slider
#include "ui/shape_gesture.hpp" // ShapeKind, the kind catalogue, convertedShape
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include "common/i18n.hpp"
#include "common/image.hpp"
#include "core/vector/corner.hpp" // where a ROUNDED corner actually lands (on-diagram handles)
#include "core/vector/flatten.hpp"
#include "core/vector/raster.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mosaic::ui {
namespace {

namespace vec = core::vec;

// One rhythm for the whole surface (the Type panel's metrics, so the two panels read as siblings).
constexpr int kPad = 12;         // inset around the content
constexpr int kRowH = 24;        // a control row
constexpr int kRowGap = 7;       // between rows
constexpr int kSectionGap = 12;  // extra space above a (non-first) section header
constexpr int kHeaderH = 18;     // a section header (title + hairline)
constexpr int kContentW = 320;   // popover content width (NOT counting the bubble margin)
constexpr int kPreviewH = 172;   // the live diagram -- big enough to actually drag handles on
constexpr int kLabelW = 88;      // left caption
constexpr int kScrollW = 15;     // right gutter reserved for the controls' scrollbar
constexpr int kKindH = 26;       // the kind-gallery strip
constexpr int kKindGap = 2;      // between gallery cells
constexpr int kMaxScrollH = 246; // the controls band scrolls past this (the popover stays sane)
constexpr int kDialSide = 46;    // an ANGLE row: a rotary knob, big enough to read its own value

constexpr int kContentLeft = kPad;
constexpr int kFieldLeft = kPad + kLabelW;
constexpr int kFieldRight = kContentW - kPad - kScrollW;
constexpr int kFieldW = kFieldRight - kFieldLeft;  // a captioned control
constexpr int kFullW = kFieldRight - kContentLeft; // a label-less full-width row / a section rule

Fl_Color toFl(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }

double toDeg(double rad) {
    double d = std::fmod(rad * 180.0 / M_PI, 360.0);
    if (d < 0.0) d += 360.0;
    return d;
}

constexpr double kTwoPi = 2.0 * M_PI;
constexpr double kMinSweep = 0.15; // an arc thinner than this is invisible -- and easy to lose

// Move ONE end of an arc's sweep to `raw` radians without letting the figure flip across the
// 0/2*pi seam (the bug that made the arc vanish). The sweep must stay inside [kMinSweep, 2*pi], so
// each endpoint has a legal window relative to the OTHER one; `raw` (a bare direction, from a dial
// or from atan2 on a dragged handle) is unwrapped onto the 2*pi branch nearest its current value --
// that is what keeps a DRAG continuous, saturating at a full turn instead of snapping to nothing --
// and only if that branch is illegal do we try its neighbour before clamping, which is what lets a
// DIAL cut a full sweep down (a full ring's End is at 2*pi, so "90 deg" unwraps to 450 and would
// otherwise clamp straight back to full). Shared by both routes, so they cannot disagree about what
// "past 360" means: 359 -> 1 is a one-degree nudge, never a rewind.
void applySweepAngle(double& startA, double& endA, bool isStart, double raw) {
    const double other = isStart ? endA : startA;
    const double lo = isStart ? other - kTwoPi : other + kMinSweep;
    const double hi = isStart ? other - kMinSweep : other + kTwoPi;
    const double cur = isStart ? startA : endA;
    double t = raw + kTwoPi * std::round((cur - raw) / kTwoPi);
    if (t < lo || t > hi) {
        const double alt = t + (t < lo ? kTwoPi : -kTwoPi);
        if (alt >= lo && alt <= hi) t = alt;
    }
    (isStart ? startA : endA) = std::clamp(t, lo, hi);
}

// How far a rounded corner pushes its ON-OUTLINE point away from the sharp vertex. A handle whose
// meaning is the VERTEX's own position (a star's valley, a cross's arm, a banner's point) rides the
// outline at vertex + this, so its drag arm subtracts it again to recover the parameter. Zero at
// radius 0, so every mapping below degenerates to the pre-rounding one.
common::Vec2 cornerOffsetAt(const vec::CorneredPolygon& poly, std::size_t i) {
    const vec::CornerPoint cp = vec::cornerPointAt(poly, i);
    return cp.apex - cp.vertex;
}

// Which control a callback targets (so one thunk serves the whole popover). Grouped by the kind
// that owns it; the numeric ones also key numericValue() below, which is what lets an on-diagram
// drag push its result back into the matching slider without any per-case plumbing.
enum class Role {
    RectRadius, RectStyle, RectLink,
    EllStart, EllEnd, EllMode,
    PolyRadius, PolyStyle,
    StarPoint, StarValley,
    LineBorder,
    // Speech bubble / callout
    CalloutBody, CalloutRadius, CalloutTailKind, CalloutAngle, CalloutLength, CalloutWidth,
    CalloutSkew, CalloutPuffs,
    ArrowShaft, ArrowHead, ArrowNotch, ArrowDouble,
    RingInner, RingStart, RingEnd,
    CrossArm, CrossRadius, CrossStyle,
    HeartLobe, HeartCleft,
    BannerStyle, BannerPoint, BannerNotch, BannerRadius,
    // The shared outline-style controls (a line, or a pre-S26-c object that carries a stroke):
    OutlineStyle,   // Solid / Dashed / Dotted / Dash-dot
    OutlineSpacing, // scales the gap between dashes / dots
    OutlineRound,   // round the dash ends + the outline's corner joins
};

vec::CornerStyle rectStyleFor(int i) { // rect offers Round / Scoop / Bevel (None == radius 0)
    switch (i) {
    case 1: return vec::CornerStyle::Inverse;
    case 2: return vec::CornerStyle::Bevel;
    default: return vec::CornerStyle::Round;
    }
}
int rectStyleIndex(vec::CornerStyle s) {
    switch (s) {
    case vec::CornerStyle::Inverse: return 1;
    case vec::CornerStyle::Bevel: return 2;
    default: return 0; // Round (None maps to Round; a sharp corner is radius 0)
    }
}
vec::CornerStyle polyStyleFor(int i) { return i == 1 ? vec::CornerStyle::Bevel : vec::CornerStyle::Round; }
int polyStyleIndex(vec::CornerStyle s) { return s == vec::CornerStyle::Bevel ? 1 : 0; }

// The shorter side of a boxed shape -- the reference every "how big can this parameter get" bound
// is expressed in, so a slider's range scales with the shape instead of being hyper-sensitive.
double minorOf(common::Vec2 size) { return std::min(std::abs(size.x), std::abs(size.y)); }

// The live value a NUMERIC control shows, read straight off the working object. ONE definition,
// used both when a row is built and by syncControls() after an on-diagram drag, so a slider can
// never drift from the model it edits. nullopt for a role that is not a slider.
std::optional<double> numericValue(const vec::Object& obj, Role role, int idx) {
    const auto* ps = std::get_if<vec::ParametricShape>(&obj.geometry);
    if (ps == nullptr) return std::nullopt;
    switch (role) {
    case Role::RectRadius:
        if (const auto* r = std::get_if<vec::RectShape>(ps))
            return r->cornerRadius[static_cast<std::size_t>(std::clamp(idx, 0, 3))];
        break;
    case Role::EllStart:
        if (const auto* e = std::get_if<vec::EllipseShape>(ps)) return toDeg(e->startAngle);
        break;
    case Role::EllEnd:
        if (const auto* e = std::get_if<vec::EllipseShape>(ps)) return toDeg(e->endAngle);
        break;
    case Role::PolyRadius:
        if (const auto* p = std::get_if<vec::PolygonShape>(ps)) return p->cornerRadius;
        break;
    case Role::StarPoint:
        if (const auto* s = std::get_if<vec::StarShape>(ps)) return s->pointRadius;
        break;
    case Role::StarValley:
        if (const auto* s = std::get_if<vec::StarShape>(ps)) return s->valleyRadius;
        break;
    case Role::LineBorder:
        if (const auto* l = std::get_if<vec::LineShape>(ps)) return l->borderWidth;
        break;
    case Role::CalloutRadius:
        if (const auto* c = std::get_if<vec::CalloutShape>(ps)) return c->cornerRadius;
        break;
    case Role::CalloutAngle:
        if (const auto* c = std::get_if<vec::CalloutShape>(ps)) return toDeg(c->tailAngle);
        break;
    case Role::CalloutLength:
        if (const auto* c = std::get_if<vec::CalloutShape>(ps)) return c->tailLength;
        break;
    case Role::CalloutWidth:
        if (const auto* c = std::get_if<vec::CalloutShape>(ps)) return c->tailWidth;
        break;
    case Role::CalloutSkew:
        if (const auto* c = std::get_if<vec::CalloutShape>(ps)) return c->tailSkew * 100.0;
        break;
    case Role::CalloutPuffs:
        if (const auto* c = std::get_if<vec::CalloutShape>(ps)) return c->bubbleCount;
        break;
    case Role::ArrowShaft:
        if (const auto* a = std::get_if<vec::ArrowShape>(ps)) return a->shaftRatio * 100.0;
        break;
    case Role::ArrowHead:
        if (const auto* a = std::get_if<vec::ArrowShape>(ps)) return a->headRatio * 100.0;
        break;
    case Role::ArrowNotch:
        if (const auto* a = std::get_if<vec::ArrowShape>(ps)) return a->notchRatio * 100.0;
        break;
    case Role::RingInner:
        if (const auto* r = std::get_if<vec::RingShape>(ps)) return r->innerRatio * 100.0;
        break;
    case Role::RingStart:
        if (const auto* r = std::get_if<vec::RingShape>(ps)) return toDeg(r->startAngle);
        break;
    case Role::RingEnd:
        if (const auto* r = std::get_if<vec::RingShape>(ps)) return toDeg(r->endAngle);
        break;
    case Role::CrossArm:
        if (const auto* x = std::get_if<vec::CrossShape>(ps)) return x->armRatio * 100.0;
        break;
    case Role::CrossRadius:
        if (const auto* x = std::get_if<vec::CrossShape>(ps)) return x->cornerRadius;
        break;
    case Role::HeartLobe:
        if (const auto* hs = std::get_if<vec::HeartShape>(ps)) return hs->lobe * 100.0;
        break;
    case Role::HeartCleft:
        if (const auto* hs = std::get_if<vec::HeartShape>(ps)) return hs->cleft * 100.0;
        break;
    case Role::BannerPoint:
        if (const auto* b = std::get_if<vec::BannerShape>(ps)) return b->pointRatio * 100.0;
        break;
    case Role::BannerRadius:
        if (const auto* b = std::get_if<vec::BannerShape>(ps)) return b->cornerRadius;
        break;
    default: break; // dropdowns / toggles / the outline section carry no model-read value
    }
    return std::nullopt;
}

// The dash pattern a style index produces, scaled by the stroke width so it reads at any weight;
// `spacing` (a multiplier) widens / narrows the gaps. 0 Solid, 1 Dashed, 2 Dotted, 3 Dash-dot.
std::vector<double> dashPatternFor(int style, double width, double spacing) {
    const double w = std::max(1.0, width);
    const double g = std::max(0.1, spacing);
    switch (style) {
    case 1: return {3.0 * w, 2.0 * w * g};
    case 2: return {0.6 * w, 1.6 * w * g};
    case 3: return {3.0 * w, 1.6 * w * g, 0.6 * w, 1.6 * w * g};
    default: return {};
    }
}
int dashStyleOf(const std::vector<double>& a) { // reflect an existing array back to a style index
    if (a.empty()) return 0;
    if (a.size() >= 4) return 3;
    return (a.size() == 2 && a[0] < a[1] * 0.5) ? 2 : 1;
}

// Where the callout's tail leaves the body, and the body's tangent there -- derived from the SAME
// flattened body outline the geometry splices the tail into, so the on-diagram tail handles sit
// exactly on the tail the renderer draws rather than on an approximation of it.
struct CalloutAnchor {
    common::Vec2 base{0, 0};
    common::Vec2 tangent{0, 1};
};
CalloutAnchor calloutAnchor(const vec::CalloutShape& c) {
    CalloutAnchor out;
    vec::CalloutShape body = c;
    body.tailLength = 0.0; // the bare body ring
    const vec::Contours cs = vec::flatten(vec::Geometry{vec::ParametricShape{body}});
    if (cs.empty() || cs.front().points.size() < 3) return out;
    const std::vector<common::Vec2>& p = cs.front().points;
    const common::Vec2 dir{std::cos(c.tailAngle), std::sin(c.tailAngle)};
    std::size_t best = 0;
    double bestCos = -2.0;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const double len = p[i].length();
        if (len < 1e-9) continue;
        const double cosang = p[i].dot(dir) / len;
        if (cosang > bestCos) {
            bestCos = cosang;
            best = i;
        }
    }
    const std::size_t n = p.size();
    out.base = p[best];
    const common::Vec2 t = p[(best + 1) % n] - p[(best + n - 1) % n];
    const double tl = t.length();
    out.tangent = tl > 1e-12 ? t * (1.0 / tl) : common::Vec2{-dir.y, dir.x};
    return out;
}

// The shape's draggable on-diagram handles, in OBJECT-LOCAL coords (§7.4 -- the dragging the first
// cut deferred).
//
// Every handle on a corner-rounded kind is derived from core::vec's corner engine (the SAME math
// flatten() emits the outline with) rather than from the raw parameter box: a handle placed at the
// sharp vertex floats off a rounded outline, which is the user-reported "the handle doesn't sit on
// the shape". `cornerPointAt(...).apex` is the midpoint of whatever that corner actually became --
// a convex fillet, a concave scoop, a chamfer, or the sharp vertex at radius 0 -- so the handle is
// on the drawn edge for every style, and it stops dead (rather than jumping) when the radius
// saturates against the half-shorter-side clamp, because the apex is computed from the CLAMPED
// inset. Reflex vertices (a cross's inner corners, a star's valleys) need no special case: the
// bisector the apex rides is built from the two edge directions, so its sign inverts on its own.
//
// One id space per kind; applyDiagramDrag() reads the same table back:
//   Rect     0-3 the corner-radius handles (TL, TR, BR, BL)
//   Ellipse  0/1 the arc's start / end angle
//   Polygon  0   the corner radius, at the top vertex
//   Star     0   the inner (valley) radius
//   Callout  0   the tail TIP (its direction AND length in one grab), 1 the tail's base width,
//               2 the body corner radius (rounded-box body only)
//   Arrow    0   the head (length + width), 1 the shaft thickness
//   Ring     0   the hole radius, 1/2 the sweep's start / end
//   Cross    0   the arm thickness, 1 the corner radius
//   Heart    0   the cleft depth, 1 the shoulder height
//   Banner   0   the point / notch depth, 1 the corner radius
std::vector<std::pair<int, common::Vec2>> handlePointsOf(const vec::Object* obj) {
    std::vector<std::pair<int, common::Vec2>> out;
    if (obj == nullptr) return out;
    const auto* ps = std::get_if<vec::ParametricShape>(&obj->geometry);
    if (ps == nullptr) return out;
    std::visit(
        [&](const auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, vec::RectShape>) {
                const vec::CorneredPolygon poly = vec::rectPolygon(s);
                for (int i = 0; i < 4; ++i)
                    out.push_back(
                        {i, vec::cornerPointAt(poly, static_cast<std::size_t>(i)).apex});
            } else if constexpr (std::is_same_v<T, vec::EllipseShape>) {
                out.push_back(
                    {0, {s.radii.x * std::cos(s.startAngle), s.radii.y * std::sin(s.startAngle)}});
                out.push_back(
                    {1, {s.radii.x * std::cos(s.endAngle), s.radii.y * std::sin(s.endAngle)}});
            } else if constexpr (std::is_same_v<T, vec::PolygonShape>) {
                // The TOP vertex's corner: at radius 0 the sharp tip, then down its bisector.
                out.push_back({0, vec::cornerPointAt(vec::polygonPolygon(s), 0).apex});
            } else if constexpr (std::is_same_v<T, vec::StarShape>) {
                // The first VALLEY (ring index 1). A rounded valley never reaches its sharp vertex,
                // so the handle rides the fillet's apex instead of floating inside the figure.
                out.push_back({0, vec::cornerPointAt(vec::starPolygon(s), 1).apex});
            } else if constexpr (std::is_same_v<T, vec::CalloutShape>) {
                const CalloutAnchor a = calloutAnchor(s);
                const common::Vec2 dir{std::cos(s.tailAngle), std::sin(s.tailAngle)};
                const double skew = std::clamp(s.tailSkew, -1.0, 1.0);
                out.push_back({0, a.base + dir * s.tailLength + a.tangent * (skew * s.tailLength)});
                out.push_back({1, a.base + a.tangent * (s.tailWidth * 0.5)});
                const vec::CorneredPolygon body = vec::calloutBodyPolygon(s);
                if (!body.empty()) // the body's TL corner (an elliptical body has none to round)
                    out.push_back({2, vec::cornerPointAt(body, 0).apex});
            } else if constexpr (std::is_same_v<T, vec::ArrowShape>) {
                const double w = std::abs(s.size.x), h = std::abs(s.size.y);
                const double hw = w * 0.5, hh = h * 0.5;
                const double head =
                    std::clamp(std::abs(s.headRatio), 0.02, s.doubleHeaded ? 0.49 : 0.98) * w;
                out.push_back({0, {hw - head, -hh}});
                out.push_back({1, {-hw * 0.6, -std::clamp(std::abs(s.shaftRatio), 0.02, 1.0) * hh}});
            } else if constexpr (std::is_same_v<T, vec::RingShape>) {
                const double k = std::clamp(s.innerRatio, 0.0, 0.98);
                out.push_back({0, {s.radii.x * k, 0.0}});
                out.push_back(
                    {1, {s.radii.x * std::cos(s.startAngle), s.radii.y * std::sin(s.startAngle)}});
                out.push_back(
                    {2, {s.radii.x * std::cos(s.endAngle), s.radii.y * std::sin(s.endAngle)}});
            } else if constexpr (std::is_same_v<T, vec::CrossShape>) {
                // Ring index 1 = the vertical bar's top-RIGHT (the arm thickness), index 0 its
                // top-LEFT (the corner radius). Both ride their own corner's apex.
                const vec::CorneredPolygon poly = vec::crossPolygon(s);
                if (!poly.empty()) {
                    out.push_back({0, vec::cornerPointAt(poly, 1).apex});
                    out.push_back({1, vec::cornerPointAt(poly, 0).apex});
                }
            } else if constexpr (std::is_same_v<T, vec::HeartShape>) {
                const double h = std::abs(s.size.y), hh = h * 0.5, hw = std::abs(s.size.x) * 0.5;
                out.push_back({0, {0.0, -hh + std::clamp(s.cleft, 0.02, 0.60) * h}});
                out.push_back(
                    {1, {hw, -hh + (0.20 + 0.19 * std::clamp(s.lobe, 0.0, 1.0)) * h}});
            } else if constexpr (std::is_same_v<T, vec::BannerShape>) {
                // The vertex that CARRIES the depth is the top-right one on a chevron and the
                // mid-right notch on a ribbon; index 0 (top-left) carries the corner radius.
                const vec::CorneredPolygon poly = vec::bannerPolygon(s);
                if (!poly.empty()) {
                    const std::size_t depth =
                        s.style == vec::BannerShape::Style::Chevron ? 1u : 2u;
                    out.push_back({0, vec::cornerPointAt(poly, depth).apex});
                    out.push_back({1, vec::cornerPointAt(poly, 0).apex});
                }
            }
        },
        *ps);
    return out;
}

// A thin guide line drawn from the shape's centre to a handle, for the handles whose meaning is a
// DIRECTION rather than a position (a callout's tail, a ring's two sweep ends).
std::vector<std::pair<common::Vec2, common::Vec2>> guidesOf(const vec::Object* obj) {
    std::vector<std::pair<common::Vec2, common::Vec2>> out;
    if (obj == nullptr) return out;
    const auto* ps = std::get_if<vec::ParametricShape>(&obj->geometry);
    if (ps == nullptr) return out;
    if (const auto* c = std::get_if<vec::CalloutShape>(ps)) {
        const CalloutAnchor a = calloutAnchor(*c);
        out.emplace_back(common::Vec2{0, 0}, a.base);
    } else if (const auto* r = std::get_if<vec::RingShape>(ps)) {
        out.emplace_back(common::Vec2{0, 0},
                         common::Vec2{r->radii.x * std::cos(r->startAngle),
                                      r->radii.y * std::sin(r->startAngle)});
        out.emplace_back(common::Vec2{0, 0}, common::Vec2{r->radii.x * std::cos(r->endAngle),
                                                          r->radii.y * std::sin(r->endAngle)});
    }
    return out;
}

// The live diagram: the working object rasterized over a transparency checkerboard, with its
// on-diagram handles composed on top by the software-AA GizmoCanvas (FLTK's fl_pie / fl_arc are
// stair-stepped, and a jagged handle on a design surface reads as unfinished). One composed image
// is blitted per draw; the rasterized shape itself is cached until refresh().
class DiagramBox : public Fl_Widget {
public:
    DiagramBox(int X, int Y, int W, int H, const vec::Object* obj)
        : Fl_Widget(X, Y, W, H), m_obj(obj) {
        copy_tooltip(_("Drag the round handles to shape it"));
    }
    void refresh() {
        m_dirty = true;
        redraw();
    }
    // Reports a handle drag: the handle id + the dragged point in OBJECT-LOCAL coords.
    void setOnDrag(std::function<void(int, common::Vec2)> cb) { m_onDrag = std::move(cb); }

protected:
    void draw() override {
        const Palette& pal = activePalette();
        if (m_dirty) rebuildImage();
        GizmoCanvas gc(w(), h(), pal.controlBg);
        if (m_haveMap && !m_img.rgba.empty()) {
            const int iw = static_cast<int>(m_img.width), ih = static_cast<int>(m_img.height);
            const int ix = static_cast<int>(std::lround(m_imgX));
            const int iy = static_cast<int>(std::lround(m_imgY));
            // The shape may be translucent, so show what is under it the way the canvas would.
            gc.checker(ix, iy, ix + iw, iy + ih, pal.panelBg, pal.controlBg, 8);
            gc.blitImage(m_img, ix, iy);
        }
        for (const auto& g : guidesOf(m_obj)) {
            const std::optional<common::Vec2> a = widgetOf(g.first);
            const std::optional<common::Vec2> b = widgetOf(g.second);
            if (a && b) gc.stroke(*a, *b, 1.0, pal.accent, 0.35f);
        }
        for (const auto& h : handlePointsOf(m_obj)) {
            const std::optional<common::Vec2> wp = widgetOf(h.second);
            if (!wp) continue;
            const bool hot = h.first == m_drag || (m_drag < 0 && h.first == m_hover);
            gc.fillDisc(*wp, hot ? 5.5 : 4.5, pal.accent, 1.0f);
            gc.fillDisc(*wp, hot ? 2.4 : 2.0, pal.onAccent, 1.0f);
        }
        Fl_RGB_Image blit(gc.data(), w(), h(), 4);
        blit.draw(x(), y());
        fl_color(toFl(pal.border));
        fl_rect(x(), y(), w(), h());
    }

    int handle(int event) override {
        switch (event) {
        case FL_ENTER:
        case FL_MOVE: {
            const int over = hitHandle(Fl::event_x(), Fl::event_y());
            if (over != m_hover) {
                m_hover = over;
                redraw();
            }
            if (window() != nullptr)
                window()->cursor(over >= 0 ? FL_CURSOR_HAND : FL_CURSOR_DEFAULT);
            return 1;
        }
        case FL_LEAVE:
            if (m_hover >= 0) {
                m_hover = -1;
                redraw();
            }
            if (window() != nullptr) window()->cursor(FL_CURSOR_DEFAULT);
            return 1;
        case FL_PUSH:  // claim PUSH/RELEASE as a pair ([[mosaic-ui-gotchas]])
            m_drag = hitHandle(Fl::event_x(), Fl::event_y());
            if (m_drag >= 0) redraw();
            return m_drag >= 0 ? 1 : 0;
        case FL_DRAG:
            if (m_drag >= 0 && m_onDrag) {
                if (const std::optional<common::Vec2> lp = toLocal(Fl::event_x(), Fl::event_y()))
                    m_onDrag(m_drag, *lp);
                return 1;
            }
            return 0;
        case FL_RELEASE:
            if (m_drag >= 0) {
                m_drag = -1;
                redraw();
                return 1;
            }
            return 0;
        default:
            return Fl_Widget::handle(event);
        }
    }

private:
    // Object-local -> WIDGET-LOCAL (the GizmoCanvas's space, whose origin is the widget's corner).
    std::optional<common::Vec2> widgetOf(common::Vec2 local) const {
        if (!m_haveMap) return std::nullopt;
        const common::Vec2 ip = m_toPixel.apply(local);
        return common::Vec2{m_imgX + ip.x, m_imgY + ip.y};
    }
    std::optional<common::Vec2> toLocal(int ex, int ey) const {
        if (!m_haveMap) return std::nullopt;
        const std::optional<common::Affine2D> inv = m_toPixel.inverse();
        if (!inv) return std::nullopt;
        return inv->apply({static_cast<double>(ex - x()) - m_imgX,
                           static_cast<double>(ey - y()) - m_imgY});
    }
    int hitHandle(int ex, int ey) const {
        const common::Vec2 p{static_cast<double>(ex - x()), static_cast<double>(ey - y())};
        int best = -1;
        double bestD = 9.0; // grab radius, widget px
        for (const auto& h : handlePointsOf(m_obj))
            if (const std::optional<common::Vec2> wp = widgetOf(h.second)) {
                const double d = (p - *wp).length();
                if (d <= bestD) {
                    bestD = d;
                    best = h.first;
                }
            }
        return best;
    }
    // The box the diagram fits to. Deliberately the shape's FULL extent -- an ellipse at full
    // sweep, a polygon un-rounded, a callout with room reserved for its longest tail -- so
    // dragging a handle (which changes the real contentBounds) never rescales or jumps the
    // diagram under the cursor, and the handles ride a fixed frame.
    std::optional<common::Rect> fitBounds() const {
        if (m_obj == nullptr) return std::nullopt;
        if (const auto* ps = std::get_if<vec::ParametricShape>(&m_obj->geometry))
            if (const auto* c = std::get_if<vec::CalloutShape>(ps)) {
                // Body box + a fixed reserve on every side: the tail can point anywhere, and its
                // slider is capped at half the shorter side (0.75 covers the skew's diagonal too).
                const double w = std::abs(c->size.x), h = std::abs(c->size.y);
                const double pad = minorOf(c->size) * 0.75;
                return common::Rect{-w * 0.5 - pad, -h * 0.5 - pad, w + 2 * pad, h + 2 * pad};
            }
        vec::Object frame = *m_obj;
        if (auto* ps = std::get_if<vec::ParametricShape>(&frame.geometry))
            std::visit(
                [](auto& s) {
                    using T = std::decay_t<decltype(s)>;
                    if constexpr (std::is_same_v<T, vec::EllipseShape>) {
                        s.startAngle = 0.0;
                        s.endAngle = 2.0 * M_PI;
                    } else if constexpr (std::is_same_v<T, vec::RingShape>) {
                        s.startAngle = 0.0;
                        s.endAngle = 2.0 * M_PI;
                    } else if constexpr (std::is_same_v<T, vec::PolygonShape> ||
                                         std::is_same_v<T, vec::CrossShape> ||
                                         std::is_same_v<T, vec::BannerShape>) {
                        s.cornerRadius = 0.0; // rounding pulls those outlines inside their box
                    }
                    // (A rounded RECT still reaches its full box, so it needs no neutralising, and
                    // a callout is special-cased above -- its tail needs a reserve, not a reset.)
                },
                *ps);
        return vec::contentBounds(frame);
    }
    void rebuildImage() {
        m_dirty = false;
        m_img = {};
        m_haveMap = false;
        if (m_obj == nullptr) return;
        const std::optional<common::Rect> b = fitBounds();
        if (!b || b->empty()) return;
        const int boxW = w() - 2 * kPad;
        const int boxH = h() - 2 * kPad;
        if (boxW < 4 || boxH < 4) return;
        const double s = std::min(boxW / b->w, boxH / b->h);
        const int iw = std::max(1, static_cast<int>(std::lround(b->w * s)));
        const int ih = std::max(1, static_cast<int>(std::lround(b->h * s)));
        m_toPixel =
            common::Affine2D::translation(-b->x * s, -b->y * s) * common::Affine2D::scaling(s, s);
        m_imgX = (w() - iw) / 2.0; // widget-LOCAL: the GizmoCanvas draws from the widget's corner
        m_imgY = (h() - ih) / 2.0;
        m_haveMap = true;
        m_img = common::toImage8(vec::rasterizeObjectF(*m_obj, iw, ih, m_toPixel));
    }

    const vec::Object* m_obj;
    common::Image m_img;           // the rasterized shape (cached until refresh())
    bool m_dirty = true;
    std::function<void(int, common::Vec2)> m_onDrag;
    common::Affine2D m_toPixel;    // object-local -> image-pixel (the inverse maps a cursor back)
    double m_imgX = 0, m_imgY = 0; // the image's top-left in WIDGET-LOCAL coords
    bool m_haveMap = false;
    int m_drag = -1;  // the handle being dragged (-1 = none)
    int m_hover = -1; // the handle under the cursor
};

// One cell of the kind gallery: the kind's toolbar art over the panel ground, an accent frame while
// selected, a hover lift. Clicking converts the working shape to that kind (ui::convertedShape).
class KindCell : public Fl_Widget {
public:
    KindCell(int X, int Y, int W, int H, Fl_RGB_Image* art, bool on, std::function<void()> onPick)
        : Fl_Widget(X, Y, W, H), m_art(art), m_on(on), m_onPick(std::move(onPick)) {}
    void setOn(bool on) {
        if (on != m_on) {
            m_on = on;
            redraw();
        }
    }

protected:
    void draw() override {
        const Palette& pal = activePalette();
        const common::Color8 bg = m_on ? pal.controlActive : (m_hover ? pal.controlHover : pal.panelBg);
        fl_color(toFl(bg));
        fl_rectf(x(), y(), w(), h());
        if (m_art != nullptr)
            m_art->draw(x() + (w() - m_art->w()) / 2, y() + (h() - m_art->h()) / 2);
        if (m_on) {
            fl_color(toFl(pal.accent));
            fl_rect(x(), y(), w(), h());
        }
    }
    int handle(int event) override {
        switch (event) {
        case FL_ENTER:
        case FL_MOVE:
            if (!m_hover) {
                m_hover = true;
                redraw();
            }
            return 1;
        case FL_LEAVE:
            if (m_hover) {
                m_hover = false;
                redraw();
            }
            return 1;
        case FL_PUSH: return 1;  // claim the pair, act on RELEASE
        case FL_RELEASE:
            if (Fl::event_inside(this) && m_onPick) m_onPick();
            return 1;
        default: return Fl_Widget::handle(event);
        }
    }

private:
    Fl_RGB_Image* m_art; // owned by the designer's icon cache
    bool m_on;
    bool m_hover = false;
    std::function<void()> m_onPick;
};

// The collapsible "Outline" header: a clickable label with a disclosure triangle + a hairline,
// matching the section headers (the Type panel's "Advanced typography" pattern, kept file-local
// there too -- promote it to widgets.hpp when a third panel wants one).
class DisclosureHeader : public FlatButton {
public:
    DisclosureHeader(int X, int Y, int W, int H, const char* text)
        : FlatButton(X, Y, W, H), m_text(text) {}
    void setOpen(bool o) {
        m_open = o;
        redraw();
    }

protected:
    void draw() override {
        const Palette& pal = activePalette();
        fl_color(toFl(pal.panelBg)); // a clickable LABEL, not a button: erase, no flat-box chrome
        fl_rectf(x(), y(), w(), h());
        const int cx = x() + 6;
        const int cyc = y() + h() / 2;
        fl_color(toFl(pal.text));
        fl_begin_polygon();
        if (m_open) {
            fl_vertex(cx - 4, cyc - 2);
            fl_vertex(cx + 4, cyc - 2);
            fl_vertex(cx, cyc + 3);
        } else {
            fl_vertex(cx - 2, cyc - 4);
            fl_vertex(cx - 2, cyc + 4);
            fl_vertex(cx + 3, cyc);
        }
        fl_end_polygon();
        fl_font(FL_HELVETICA_BOLD, 12);
        fl_draw(m_text.c_str(), x() + 18, y(), w() - 18, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        fl_color(toFl(pal.border));
        fl_line(x(), y() + h() - 1, x() + w() - 1, y() + h() - 1);
    }

private:
    std::string m_text;
    bool m_open = false;
};

} // namespace

// A control's link back to the popover (FLTK callbacks are C thunks; the binding carries the rest).
struct Binding {
    ShapeDesigner* self = nullptr;
    Role role = Role::RectRadius;
    int idx = 0;
};

struct ShapeDesigner::State {
    std::vector<std::unique_ptr<Binding>> bindings;
    DiagramBox* diagram = nullptr;
    ScrollView* scroll = nullptr;
    Fl_Group* content = nullptr;   // the scrolled controls (rebuilt on a kind change)
    Fl_Group* outline = nullptr;   // the collapsible outline/dash section
    DisclosureHeader* disclosure = nullptr;
    std::vector<KindCell*> kindCells;
    // EVERY numeric control, so one syncControls() pass can re-read them all from the working
    // object after an on-diagram drag -- no per-handle slider poking anywhere.
    struct SliderRef {
        Role role;
        int idx;
        ScrubSlider* w;
    };
    std::vector<SliderRef> sliders;
    // ...and every angle DIAL, on the same footing: an angle is a cyclic quantity, so it is edited
    // on a rotary knob rather than a linear degree slider, but it still reads its value through the
    // one numericValue() the sliders do -- an on-diagram drag and its dial cannot drift apart.
    struct DialRef {
        Role role;
        int idx;
        Dial* w;
    };
    std::vector<DialRef> dials;
    std::array<Dropdown*, 4> rectStyle{};  // the rect's per-corner style (linked propagation)
    bool linkCorners = true;
    bool outlineOpen = false;
    int contentHFull = 0;      // the controls' height with the outline section open...
    int contentHCollapsed = 0; // ...and closed (the Type panel's disclosure pattern)
    // The kind gallery's art, cached per icon key. The Fl_RGB_Image is a VIEW over the pixels, so
    // the two maps must live and die together (and the pixels must outlive the image).
    std::map<std::string, common::Image> iconPixels;
    std::map<std::string, std::unique_ptr<Fl_RGB_Image>> icons;
    IconPacks* packs = nullptr;                     // the host's pack set (non-owning)
    std::string packId = std::string(kDefaultIconPackId);
    std::unique_ptr<IconPacks> ownPacks;            // fallback: the embedded default pack
    // Outline-style working values (the model only stores the resulting dashArray + cap/join).
    int dashStyle = 0;        // 0 Solid / 1 Dashed / 2 Dotted / 3 Dash-dot
    double dashSpacing = 1.0; // gap multiplier
    bool roundOutline = false;
};

namespace {
void onControlCb(Fl_Widget* w, void* u);    // the single control thunk (defined below)
void onDisclosureCb(Fl_Widget* w, void* u); // the outline section's disclosure (defined below)
} // namespace

ShapeDesigner::ShapeDesigner() : Popover(kContentW, 320), m_state(std::make_unique<State>()) {
    enableBubble(BubbleSide::Up); // a comic-book pointer aimed UP at the options-bar "Edit shape…" button
}
ShapeDesigner::~ShapeDesigner() = default;

void ShapeDesigner::setScrubRuler(ScrubRuler* r) {
    m_scrubRuler = r;
    if (shown())
        rebuild(); // hand the ruler to each slider as it rebuilds
}

void ShapeDesigner::setIconPacks(IconPacks* packs, std::string packId) {
    m_state->packs = packs;
    m_state->packId = std::move(packId);
    m_state->icons.clear(); // the views die before the pixels they point into
    m_state->iconPixels.clear();
    if (shown())
        rebuild();
}

void ShapeDesigner::reapplyTheme() {
    Popover::reapplyTheme();
    if (shown())
        rebuild();
}

void ShapeDesigner::openFor(const Fl_Widget* anchor, const vec::Object* obj) {
    m_hasObj = obj != nullptr;
    if (m_hasObj)
        m_obj = *obj;
    // Seed the outline-style working values from the object (spacing can't be reversed from the
    // array, so it starts at 1.0).
    m_state->dashStyle = m_hasObj ? dashStyleOf(m_obj.stroke.dashArray) : 0;
    m_state->dashSpacing = 1.0;
    m_state->roundOutline = m_hasObj && m_obj.stroke.cap == vec::LineCap::Round;
    rebuild();
    showAnchored(anchor);
}

void ShapeDesigner::emitEdit() {
    if (m_state->diagram != nullptr)
        m_state->diagram->refresh();
    if (m_onEdit && m_hasObj)
        m_onEdit(m_obj);
}

void ShapeDesigner::syncControls() {
    for (const State::SliderRef& s : m_state->sliders)
        if (s.w != nullptr)
            if (const std::optional<double> v = numericValue(m_obj, s.role, s.idx)) {
                s.w->value(*v);
                s.w->redraw();
            }
    for (const State::DialRef& d : m_state->dials)
        if (d.w != nullptr)
            if (const std::optional<double> v = numericValue(m_obj, d.role, d.idx)) {
                d.w->value(*v);
                d.w->redraw();
            }
}

void ShapeDesigner::applyKind(int kindInt) {
    if (!m_hasObj)
        return;
    const ShapeKind kind = static_cast<ShapeKind>(kindInt);
    if (shapeKindOf(m_obj) == kind)
        return;
    m_obj = convertedShape(m_obj, kind);
    for (std::size_t i = 0; i < m_state->kindCells.size() && i < shapeKindCatalog().size(); ++i)
        m_state->kindCells[i]->setOn(shapeKindCatalog()[i].kind == kind);
    m_state->outlineOpen = false; // a fresh kind starts with the essentials showing
    rebuildControls();            // NOT rebuild(): the clicked cell must survive its own handler
    emitEdit();
}

void ShapeDesigner::toggleOutline() {
    m_state->outlineOpen = !m_state->outlineOpen;
    if (m_state->disclosure != nullptr)
        m_state->disclosure->setOpen(m_state->outlineOpen);
    if (m_state->outline != nullptr) {
        if (m_state->outlineOpen)
            m_state->outline->show();
        else
            m_state->outline->hide();
    }
    if (m_state->content != nullptr)
        m_state->content->size(m_state->content->w(), m_state->outlineOpen
                                                          ? m_state->contentHFull
                                                          : m_state->contentHCollapsed);
    // Collapsing must not leave the view stranded in now-empty space. Zero the scrollbar WIDGET
    // first (Fl_Scroll re-derives from it) and PRESERVE xposition() -- passing X=0 yanks the rows
    // flush-left (the Settings-dialog scroll-0 trap, [[mosaic-ui-gotchas]]).
    if (m_state->scroll != nullptr) {
        m_state->scroll->scrollbar.value(0);
        m_state->scroll->scroll_to(m_state->scroll->xposition(), 0);
    }
    sizeToContent();
}

void ShapeDesigner::sizeToContent() {
    const int by = bubbleActive() ? kBubbleTri : 0; // TOP margin reserved for the triangle
    const int contentH = m_state->content != nullptr ? m_state->content->h() : kRowH;
    const int scrollH = std::clamp(contentH, kRowH, kMaxScrollH);
    if (m_state->scroll != nullptr)
        m_state->scroll->size(m_state->scroll->w(), scrollH);
    const int bodyH = kPad + kPreviewH + kRowGap + kKindH + kRowGap + scrollH + kPad;
    setBaseSize(kContentW, by + bodyH);
    resizable(nullptr);
    Fl_Double_Window::resize(x(), y(), kContentW, by + bodyH);
    if (shown())
        reanchor(); // the bubble tip must stay on the anchor after a height change
    redraw();
}

void ShapeDesigner::rebuild() {
    const Palette& pal = activePalette();
    m_state->diagram = nullptr;
    m_state->scroll = nullptr;
    m_state->content = nullptr;
    m_state->outline = nullptr;
    m_state->disclosure = nullptr;
    m_state->kindCells.clear();
    m_state->sliders.clear();
    m_state->dials.clear();
    m_state->rectStyle = {};

    const int by = bubbleActive() ? kBubbleTri : 0; // TOP margin reserved for the triangle
    // Normalize the window to a full-height footprint BEFORE laying out children: a hidden popover
    // is group-stretched by every main-window resize, and building design-coordinate children
    // inside a stretched box bakes an inconsistent baseline (the Type panel's rev-3 ghost).
    const int maxH = by + kPad + kPreviewH + kRowGap + kKindH + kRowGap + kMaxScrollH + kPad;
    resizable(nullptr);
    Fl_Double_Window::resize(x(), y(), kContentW, maxH);
    clear();                   // drop the previous widgets (no callback can fire after)...
    resizable(nullptr);        // ...re-pin (clear() resets resizable to the group)...
    m_state->bindings.clear(); // ...then their now-unreferenced bindings
    begin();

    fl_font(FL_HELVETICA, 12);
    int cy = by + kPad; // window-relative + below the triangle margin (children are 0-based)

    if (!m_hasObj || !std::holds_alternative<vec::ParametricShape>(m_obj.geometry)) {
        auto* hint = new Fl_Box(kContentLeft, cy, kContentW - 2 * kPad, kRowH * 2);
        hint->copy_label(m_hasObj ? _("This shape has no editable parameters.")
                                  : _("Select a shape with the Shape tool to edit it."));
        hint->box(FL_NO_BOX);
        hint->labelfont(FL_HELVETICA);
        hint->labelsize(12);
        hint->labelcolor(toFl(pal.textMuted));
        hint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        end();
        resizable(nullptr);
        setBaseSize(kContentW, by + kPad + kRowH * 2 + kPad);
        Fl_Double_Window::resize(x(), y(), kContentW, by + kPad + kRowH * 2 + kPad);
        redraw();
        return;
    }

    // ---- band 1: the live diagram -------------------------------------------------------------
    auto* diagram = new DiagramBox(kContentLeft, cy, kFullW, kPreviewH, &m_obj);
    diagram->setOnDrag([this](int h, common::Vec2 lp) { applyDiagramDrag(h, lp); });
    m_state->diagram = diagram;
    cy += kPreviewH + kRowGap;

    // ---- band 2: the kind gallery -------------------------------------------------------------
    // The art comes from the icon pack, exactly as the toolbar's does; the fallback instance holds
    // the embedded default pack, so the gallery is never blank even before a host wires its own.
    const auto artFor = [&](const char* key) -> Fl_RGB_Image* {
        const std::string k(key);
        if (const auto it = m_state->icons.find(k); it != m_state->icons.end())
            return it->second.get();
        IconPacks* packs = m_state->packs;
        if (packs == nullptr) {
            if (!m_state->ownPacks)
                m_state->ownPacks = std::make_unique<IconPacks>();
            packs = m_state->ownPacks.get();
        }
        common::Image img = packs->renderIcon(m_state->packId, k, 16);
        if (img.rgba.empty())
            return nullptr;
        common::Image& px = m_state->iconPixels[k];
        px = std::move(img);
        auto owned = std::make_unique<Fl_RGB_Image>(px.rgba.data(), static_cast<int>(px.width),
                                                    static_cast<int>(px.height), 4);
        Fl_RGB_Image* raw = owned.get();
        m_state->icons[k] = std::move(owned);
        return raw;
    };
    const std::vector<ShapeKindInfo>& kinds = shapeKindCatalog();
    const std::optional<ShapeKind> current = shapeKindOf(m_obj);
    const int n = static_cast<int>(kinds.size());
    const int cellW = (kFullW - (n - 1) * kKindGap) / std::max(1, n);
    for (int i = 0; i < n; ++i) {
        const ShapeKindInfo& info = kinds[static_cast<std::size_t>(i)];
        auto* cell = new KindCell(kContentLeft + i * (cellW + kKindGap), cy, cellW, kKindH,
                                  artFor(info.iconKey), current == info.kind,
                                  [this, k = info.kind] { applyKind(static_cast<int>(k)); });
        cell->copy_tooltip(_(info.name));
        m_state->kindCells.push_back(cell);
    }
    cy += kKindH + kRowGap;

    // ---- band 3: the controls (scrolled) ------------------------------------------------------
    // The content group is created FIRST (so its rows can be laid out and measured), then the
    // scroll view is built at the measured height and adopts it. Sizing the scroll after the fact
    // would be the other way round -- and this way the popover only ever resizes once.
    auto* content = new Fl_Group(kContentLeft, cy, kFullW + kScrollW, kMaxScrollH);
    content->box(FL_NO_BOX);
    // CRITICAL: a group's default resizable is ITSELF, so the later size() that trims it to the
    // real content height would proportionally scale every row. Pin it null.
    content->resizable(nullptr);
    content->end();
    m_state->content = content;

    auto* sv = new ScrollView(kContentLeft, cy, kFullW + kScrollW, kMaxScrollH);
    sv->type(Fl_Scroll::VERTICAL); // the gutter is reserved by the row metrics either way
    sv->box(FL_FLAT_BOX);          // a solid panelBg ground so scrolling leaves no artifact
    sv->color(toFl(pal.panelBg));
    sv->resizable(nullptr);
    sv->add(content); // reparent: coordinates are absolute, so nothing shifts
    sv->end();
    m_state->scroll = sv;

    end();
    resizable(nullptr);
    rebuildControls();
}

void ShapeDesigner::rebuildControls() {
    Fl_Group* content = m_state->content;
    if (content == nullptr)
        return;
    const Palette& pal = activePalette();
    const auto* ps = std::get_if<vec::ParametricShape>(&m_obj.geometry);
    if (ps == nullptr)
        return;

    // This runs OUTSIDE the popover's own begin()/end() bracket (a kind change refills the band on
    // its own), so restore whatever group was current rather than leaving the scroll view current.
    Fl_Group* const prevCurrent = Fl_Group::current();
    content->clear();
    content->resizable(nullptr); // clear() resets resizable to the group -- re-pin every time
    m_state->bindings.clear();
    m_state->sliders.clear();
    m_state->dials.clear();
    m_state->rectStyle = {};
    m_state->outline = nullptr;
    m_state->disclosure = nullptr;
    content->begin();

    fl_font(FL_HELVETICA, 12);
    const int top = content->y();
    int cy = top;

    // A line always strokes itself; a closed shape only carries a stroke if a pre-S26-c document
    // gave it one (the tool authors fills now -- outlines are a Stroke layer effect).
    const bool isLine = std::holds_alternative<vec::LineShape>(*ps);
    const bool hasStroke =
        isLine || (m_obj.stroke.enabled && !std::holds_alternative<vec::NoPaint>(m_obj.stroke.paint));

    const auto bind = [&](Role role, int idx) {
        auto b = std::make_unique<Binding>();
        b->self = this;
        b->role = role;
        b->idx = idx;
        Binding* raw = b.get();
        m_state->bindings.push_back(std::move(b));
        return raw;
    };
    const auto caption = [&](int rowY, const char* text) {
        auto* b = new Fl_Box(kContentLeft, rowY, kLabelW, kRowH);
        b->copy_label(text);
        b->box(FL_NO_BOX);
        b->labelfont(FL_HELVETICA);
        b->labelsize(12);
        b->labelcolor(toFl(pal.textMuted));
        b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    };
    const auto header = [&](const char* text, bool first) {
        cy += first ? 0 : kSectionGap;
        auto* b = new Fl_Box(kContentLeft, cy, kFullW, kHeaderH);
        b->copy_label(text);
        b->box(FL_NO_BOX);
        b->labelfont(FL_HELVETICA_BOLD);
        b->labelsize(12);
        b->labelcolor(toFl(pal.text));
        b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        auto* rule = new Fl_Box(kContentLeft, cy + kHeaderH - 1, kFullW, 1);
        rule->box(FL_FLAT_BOX);
        rule->color(toFl(pal.border));
        cy += kHeaderH + kRowGap;
    };
    // One precision slider (ScrubSlider draws its own value, so no separate readout box), seeded
    // and registered so syncControls() can re-read it after an on-diagram drag.
    const auto slider = [&](int X, int rowY, int W, Role role, int idx, double mn, double mx,
                            double step, const char* suffix, const char* tip) {
        auto* s = new ScrubSlider(X, rowY, W, kRowH);
        s->range(mn, mx);
        s->step(step);
        s->setSuffix(suffix);
        s->setCellColor(pal.panelBg);
        s->setRuler(m_scrubRuler);
        s->value(numericValue(m_obj, role, idx).value_or(mn));
        s->when(FL_WHEN_CHANGED);
        s->callback(onControlCb, bind(role, idx));
        if (tip != nullptr)
            s->copy_tooltip(tip);
        m_state->sliders.push_back({role, idx, s});
        return s;
    };
    const auto sliderRow = [&](const char* label, Role role, double mn, double mx, double step,
                               const char* suffix, const char* tip) {
        caption(cy, label);
        ScrubSlider* s = slider(kFieldLeft, cy, kFieldW, role, 0, mn, mx, step, suffix, tip);
        cy += kRowH + kRowGap;
        return s;
    };
    // An ANGLE row: a rotary knob, never a degree slider. A cyclic quantity has no endpoints to
    // hang a track between, and "point it down-left" is a gesture, not an arithmetic problem. The
    // knob prints its own live value, so the row needs no separate readout; the caption sits
    // centred against it so the band keeps its rhythm.
    const auto dialRow = [&](const char* label, Role role, const char* tip) {
        caption(cy + (kDialSide - kRowH) / 2, label);
        auto* d = new Dial(kFieldLeft, cy, kDialSide, kDialSide);
        d->range(0, 360); // a full turn -> Dial wraps instead of sticking at either end
        d->step(1);
        // The vector model measures angles from +x with y down (0 = due right, +pi/2 = straight
        // down), which is 90 deg clockwise from the dial's native 12 o'clock. Offsetting the NEEDLE
        // keeps value() in the model's own units, so what the readout says is what gets stored.
        d->setZeroOffset(90.0);
        d->setCellColor(pal.panelBg);
        d->setShowReadout(true);
        d->setDefaultValue(0.0); // middle / Ctrl click -> due right (the model's zero)
        d->value(numericValue(m_obj, role, 0).value_or(0.0));
        d->when(FL_WHEN_CHANGED);
        d->callback(onControlCb, bind(role, 0));
        if (tip != nullptr)
            d->copy_tooltip(tip);
        m_state->dials.push_back({role, 0, d});
        cy += kDialSide + kRowGap;
        return d;
    };
    const auto choiceRow = [&](const char* label, Role role,
                               std::initializer_list<const char*> items, int val, const char* tip) {
        caption(cy, label);
        auto* d = new Dropdown(kFieldLeft, cy, kFieldW, kRowH);
        for (const char* it : items)
            d->add(it);
        d->value(val);
        d->callback(onControlCb, bind(role, 0));
        if (tip != nullptr)
            d->copy_tooltip(tip);
        cy += kRowH + kRowGap;
        return d;
    };
    const auto checkRow = [&](const char* label, Role role, bool on, const char* tip) {
        auto* c = new CheckBox(kContentLeft, cy, kFullW, kRowH, label);
        c->setChecked(on);
        c->setGroundColor(pal.panelBg);
        c->setOnToggle([this, role, c](bool) { applyControl(c, static_cast<int>(role), 0); });
        if (tip != nullptr)
            c->copy_tooltip(tip);
        cy += kRowH + kRowGap;
        return c;
    };

    // ---- the per-kind sections ----------------------------------------------------------------
    std::visit(
        [&](const auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, vec::RectShape>) {
                header(_("Corners"), /*first=*/true);
                m_state->linkCorners =
                    std::all_of(s.cornerRadius.begin(), s.cornerRadius.end(),
                                [&](double r) { return r == s.cornerRadius[0]; }) &&
                    std::all_of(s.cornerStyle.begin(), s.cornerStyle.end(),
                                [&](vec::CornerStyle st) { return st == s.cornerStyle[0]; });
                checkRow(_("Link all four corners"), Role::RectLink, m_state->linkCorners,
                         _("Edit every corner together"));
                // The true max comes from the corner engine (half the shorter adjacent edge, which
                // for a rect is half the shorter side) -- shape-relative, so the slider is not
                // hyper-sensitive on a small shape, and it ends exactly where the outline stops
                // changing so the slider and the on-diagram handle saturate together.
                const double maxR = vec::maxCornerRadius(vec::rectPolygon(s));
                const char* names[4] = {_("Top-left"), _("Top-right"), _("Bot-right"),
                                        _("Bot-left")};
                const int styW = 84;
                const int radW = kFieldW - styW - 6;
                for (int i = 0; i < 4; ++i) {
                    caption(cy, names[i]);
                    slider(kFieldLeft, cy, radW, Role::RectRadius, i, 0, std::max(1.0, maxR), 1,
                           _("px"), nullptr);
                    auto* d = new Dropdown(kFieldLeft + radW + 6, cy, styW, kRowH);
                    for (const char* it : {_("Round"), _("Scoop"), _("Bevel")})
                        d->add(it);
                    d->value(rectStyleIndex(s.cornerStyle[i]));
                    d->callback(onControlCb, bind(Role::RectStyle, i));
                    m_state->rectStyle[static_cast<std::size_t>(i)] = d;
                    cy += kRowH + kRowGap;
                }
            } else if constexpr (std::is_same_v<T, vec::EllipseShape>) {
                header(_("Arc"), true);
                dialRow(_("Start"), Role::EllStart,
                        _("Where the arc begins (drag the handle on the diagram)"));
                dialRow(_("End"), Role::EllEnd,
                        _("Where the arc ends (drag the handle on the diagram)"));
                choiceRow(_("Closure"), Role::EllMode, {_("Open"), _("Chord"), _("Pie")},
                          static_cast<int>(s.arcMode), _("How a partial sweep closes"));
            } else if constexpr (std::is_same_v<T, vec::PolygonShape>) {
                header(_("Corners"), true);
                const double maxR = vec::maxCornerRadius(vec::polygonPolygon(s));
                sliderRow(_("Rounding"), Role::PolyRadius, 0, std::max(1.0, maxR), 1, _("px"),
                          nullptr);
                choiceRow(_("Style"), Role::PolyStyle, {_("Round"), _("Bevel")},
                          polyStyleIndex(s.cornerStyle), nullptr);
            } else if constexpr (std::is_same_v<T, vec::StarShape>) {
                header(_("Points"), true);
                // Tips and valleys have different edge geometry, so each gets its OWN ceiling from
                // the corner engine rather than a shared outer-radius guess.
                const vec::CorneredPolygon poly = vec::starPolygon(s);
                const double maxTip = std::max(1.0, vec::cornerPointAt(poly, 0).maxRadius);
                const double maxValley = std::max(1.0, vec::cornerPointAt(poly, 1).maxRadius);
                sliderRow(_("Tip round"), Role::StarPoint, 0, maxTip, 1, _("px"),
                          _("Rounding at the outer tips"));
                sliderRow(_("Valley round"), Role::StarValley, 0, maxValley, 1, _("px"),
                          _("Rounding at the inner valleys"));
            } else if constexpr (std::is_same_v<T, vec::LineShape>) {
                header(_("Line"), true);
                ScrubSlider* border =
                    sliderRow(_("Border"), Role::LineBorder, 1, 100, 1, _("px"),
                              _("The contrasting edge of a Hollow / Outlined line"));
                if (s.paint == vec::LineShape::Paint::Solid)
                    border->deactivate(); // a Solid line has no border
            } else if constexpr (std::is_same_v<T, vec::CalloutShape>) {
                const double minor = std::max(1.0, minorOf(s.size));
                const bool pointer = s.tail == vec::CalloutShape::Tail::Pointer;
                header(_("Body"), true);
                choiceRow(_("Shape"), Role::CalloutBody, {_("Rounded box"), _("Ellipse")},
                          static_cast<int>(s.body), _("The balloon's body"));
                const double maxBodyR =
                    std::max(1.0, vec::maxCornerRadius(vec::calloutBodyPolygon(s)));
                ScrubSlider* rad = sliderRow(_("Corner"), Role::CalloutRadius, 0, maxBodyR, 1,
                                             _("px"), nullptr);
                if (s.body == vec::CalloutShape::Body::Ellipse)
                    rad->deactivate(); // an elliptical body has no corners to round
                header(_("Tail"), false);
                choiceRow(_("Kind"), Role::CalloutTailKind, {_("Pointer"), _("Thought puffs")},
                          static_cast<int>(s.tail),
                          _("A speech balloon's pointer, or a thought balloon's trail"));
                dialRow(_("Direction"), Role::CalloutAngle,
                        _("Which way the tail leaves the body (drag the tip on the diagram)"));
                sliderRow(_("Length"), Role::CalloutLength, 0, minor * 0.5, 1, _("px"),
                          _("How far the tail reaches past the body"));
                sliderRow(_("Width"), Role::CalloutWidth, 0, minor, 1, _("px"),
                          _("The pointer's base width -- or the size of the leading puff"));
                // BOTH tail-kind rows are always built, the inapplicable one greyed: switching kind
                // must not reflow the panel (and rebuilding here would delete the live control).
                ScrubSlider* skew =
                    sliderRow(_("Curve"), Role::CalloutSkew, -100, 100, 1, _("%"),
                              _("Hook the tip sideways without moving where the tail joins"));
                ScrubSlider* puffs =
                    sliderRow(_("Puffs"), Role::CalloutPuffs, 1, 8, 1, "",
                              _("How many shrinking puffs trail a thought balloon"));
                (pointer ? puffs : skew)->deactivate();
            } else if constexpr (std::is_same_v<T, vec::ArrowShape>) {
                header(_("Arrow"), true);
                sliderRow(_("Shaft"), Role::ArrowShaft, 2, 100, 1, _("%"),
                          _("Shaft thickness, as a share of the head's width"));
                sliderRow(_("Head"), Role::ArrowHead, 2, 95, 1, _("%"),
                          _("Head length, as a share of the total length"));
                sliderRow(_("Barb"), Role::ArrowNotch, 0, 95, 1, _("%"),
                          _("Sweep the head's back edge into a barb"));
                checkRow(_("Head at both ends"), Role::ArrowDouble, s.doubleHeaded, nullptr);
            } else if constexpr (std::is_same_v<T, vec::RingShape>) {
                header(_("Ring"), true);
                sliderRow(_("Hole"), Role::RingInner, 0, 95, 1, _("%"),
                          _("Inner radius as a share of the outer (0 = a solid pie)"));
                header(_("Sweep"), false);
                dialRow(_("Start"), Role::RingStart,
                        _("Where the sweep begins (drag the handle on the diagram)"));
                dialRow(_("End"), Role::RingEnd,
                        _("Where the sweep ends (drag the handle on the diagram)"));
            } else if constexpr (std::is_same_v<T, vec::CrossShape>) {
                header(_("Cross"), true);
                sliderRow(_("Arms"), Role::CrossArm, 2, 98, 1, _("%"),
                          _("Arm thickness, as a share of the shorter side"));
                // A cross's twelve corners sit on SHORT edges, so half the shorter side is a wild
                // over-estimate: the engine's own ceiling is the only honest range for the slider.
                sliderRow(_("Corner"), Role::CrossRadius, 0,
                          std::max(1.0, vec::maxCornerRadius(vec::crossPolygon(s))), 1, _("px"),
                          nullptr);
                choiceRow(_("Style"), Role::CrossStyle, {_("Round"), _("Scoop"), _("Bevel")},
                          rectStyleIndex(s.cornerStyle), nullptr);
            } else if constexpr (std::is_same_v<T, vec::HeartShape>) {
                header(_("Heart"), true);
                sliderRow(_("Shoulders"), Role::HeartLobe, 0, 100, 1, _("%"),
                          _("How low and full the two lobes sit"));
                sliderRow(_("Cleft"), Role::HeartCleft, 2, 60, 1, _("%"),
                          _("Depth of the notch between the lobes"));
            } else if constexpr (std::is_same_v<T, vec::BannerShape>) {
                header(_("Banner"), true);
                choiceRow(_("End"), Role::BannerStyle, {_("Point"), _("Swallow-tail")},
                          static_cast<int>(s.style),
                          _("Push the right edge out into a point, or cut it in as a ribbon"));
                sliderRow(_("Depth"), Role::BannerPoint, 0, 45, 1, _("%"), nullptr);
                checkRow(_("Notch the tail"), Role::BannerNotch, s.notchTail,
                         _("Cut the matching notch into the left edge"));
                sliderRow(_("Corner"), Role::BannerRadius, 0,
                          std::max(1.0, vec::maxCornerRadius(vec::bannerPolygon(s))), 1, _("px"),
                          nullptr);
            }
        },
        *ps);

    // ---- the shared outline / dash section (collapsed by default) -----------------------------
    cy += kSectionGap;
    auto* disc = new DisclosureHeader(kContentLeft, cy, kFullW, kHeaderH, _("Outline & dashes"));
    disc->callback(onDisclosureCb, this);
    disc->setOpen(m_state->outlineOpen);
    disc->copy_tooltip(_("Dash pattern for a line, or for an outline this shape already carries"));
    m_state->disclosure = disc;
    cy += kHeaderH + kRowGap;

    const int outlineTop = cy;
    m_state->contentHCollapsed = outlineTop - top + kPad;
    auto* outline = new Fl_Group(kContentLeft, outlineTop, kFullW + kScrollW, kRowH);
    outline->box(FL_NO_BOX);
    outline->resizable(nullptr);
    outline->begin();
    {
        caption(cy, _("Style"));
        auto* style = new Dropdown(kFieldLeft, cy, kFieldW, kRowH);
        for (const char* it : {_("Solid"), _("Dashed"), _("Dotted"), _("Dash-dot")})
            style->add(it);
        style->value(m_state->dashStyle);
        style->callback(onControlCb, bind(Role::OutlineStyle, 0));
        cy += kRowH + kRowGap;
        caption(cy, _("Spacing"));
        const int roundW = 78;
        auto* sp = new ScrubSlider(kFieldLeft, cy, kFieldW - roundW - 6, kRowH);
        sp->range(50, 300);
        sp->step(1);
        sp->setSuffix(_("%"));
        sp->setCellColor(pal.panelBg);
        sp->setRuler(m_scrubRuler);
        sp->value(m_state->dashSpacing * 100.0);
        sp->when(FL_WHEN_CHANGED);
        sp->callback(onControlCb, bind(Role::OutlineSpacing, 0));
        auto* rnd = new CheckBox(kFieldLeft + kFieldW - roundW, cy, roundW, kRowH, _("Round"));
        rnd->setChecked(m_state->roundOutline);
        rnd->setGroundColor(pal.panelBg);
        rnd->setOnToggle(
            [this, rnd](bool) { applyControl(rnd, static_cast<int>(Role::OutlineRound), 0); });
        cy += kRowH + kRowGap;
        if (!hasStroke) { // nothing paints an outline: show the section, but greyed
            style->deactivate();
            sp->deactivate();
            rnd->deactivate();
        }
    }
    outline->end();
    outline->size(outline->w(), cy - outlineTop);
    outline->resizable(nullptr);
    m_state->outline = outline;
    if (!m_state->outlineOpen)
        outline->hide();

    content->end();
    content->resizable(nullptr);
    Fl_Group::current(prevCurrent);
    m_state->contentHFull = cy - top + kPad;
    content->size(content->w(),
                  m_state->outlineOpen ? m_state->contentHFull : m_state->contentHCollapsed);
    sizeToContent();
}

void ShapeDesigner::applyControl(Fl_Widget* w, int roleInt, int idx) {
    auto* ps = std::get_if<vec::ParametricShape>(&m_obj.geometry);
    if (ps == nullptr || w == nullptr)
        return;
    const Role role = static_cast<Role>(roleInt);
    const auto sval = [&] { return static_cast<ScrubSlider*>(w)->value(); };
    const auto dval = [&] { return static_cast<Dropdown*>(w)->value(); };
    const auto cval = [&] { return static_cast<CheckBox*>(w)->checked(); };
    // Angles come off a Dial, not a slider -- both are Fl_Valuators, so the read is the same one.
    const auto toRad = [&] { return static_cast<Fl_Valuator*>(w)->value() * M_PI / 180.0; };
    // A control that greys/ungreys ANOTHER control does it in place: rebuilding the section from a
    // live control's own callback would delete that control mid-handler.
    const auto sliderFor = [&](Role r, int i) -> ScrubSlider* {
        for (const State::SliderRef& s : m_state->sliders)
            if (s.role == r && s.idx == i) return s.w;
        return nullptr;
    };
    const auto setEnabled = [](Fl_Widget* c, bool on) {
        if (c == nullptr) return;
        if (on)
            c->activate();
        else
            c->deactivate();
        c->redraw();
    };

    switch (role) {
    case Role::RectRadius:
    case Role::RectStyle:
    case Role::RectLink: {
        auto* r = std::get_if<vec::RectShape>(ps);
        if (r == nullptr)
            return;
        const std::size_t slot = static_cast<std::size_t>(std::clamp(idx, 0, 3));
        if (role == Role::RectLink) {
            m_state->linkCorners = cval();
            if (m_state->linkCorners) // unify every corner onto corner 0
                for (std::size_t i = 1; i < 4; ++i) {
                    r->cornerRadius[i] = r->cornerRadius[0];
                    r->cornerStyle[i] = r->cornerStyle[0];
                }
        } else if (role == Role::RectRadius) {
            const double v = sval();
            r->cornerRadius[slot] = v;
            if (m_state->linkCorners) r->cornerRadius.fill(v);
        } else { // RectStyle
            const vec::CornerStyle st = rectStyleFor(dval());
            r->cornerStyle[slot] = st;
            if (m_state->linkCorners) r->cornerStyle.fill(st);
        }
        for (std::size_t i = 0; i < 4; ++i) // the style dropdowns follow the link (sliders: sync)
            if (m_state->rectStyle[i] != nullptr) {
                m_state->rectStyle[i]->value(rectStyleIndex(r->cornerStyle[i]));
                m_state->rectStyle[i]->redraw();
            }
        syncControls();
        break;
    }
    // The two sweep ends go through applySweepAngle, exactly as an on-diagram drag does: a dial
    // reports a bare [0,360), which must be unwrapped onto the branch that keeps the arc alive
    // before it is stored. syncControls() then re-reads the dial from the model, so if the sweep
    // clamp bit, the knob shows what was actually kept rather than what the cursor asked for.
    case Role::EllStart:
    case Role::EllEnd:
        if (auto* e = std::get_if<vec::EllipseShape>(ps)) {
            applySweepAngle(e->startAngle, e->endAngle, role == Role::EllStart, toRad());
            syncControls();
        }
        break;
    case Role::EllMode:
        if (auto* e = std::get_if<vec::EllipseShape>(ps))
            e->arcMode = static_cast<vec::EllipseShape::ArcMode>(dval());
        break;
    case Role::PolyRadius:
        if (auto* p = std::get_if<vec::PolygonShape>(ps)) p->cornerRadius = sval();
        break;
    case Role::PolyStyle:
        if (auto* p = std::get_if<vec::PolygonShape>(ps)) p->cornerStyle = polyStyleFor(dval());
        break;
    case Role::StarPoint:
        if (auto* st = std::get_if<vec::StarShape>(ps)) st->pointRadius = sval();
        break;
    case Role::StarValley:
        if (auto* st = std::get_if<vec::StarShape>(ps)) st->valleyRadius = sval();
        break;
    case Role::LineBorder:
        if (auto* l = std::get_if<vec::LineShape>(ps)) l->borderWidth = sval();
        break;

    // ---- speech bubble / callout ----
    case Role::CalloutBody:
        if (auto* c = std::get_if<vec::CalloutShape>(ps)) {
            c->body = static_cast<vec::CalloutShape::Body>(dval());
            setEnabled(sliderFor(Role::CalloutRadius, 0),
                       c->body == vec::CalloutShape::Body::RoundedRect);
        }
        break;
    case Role::CalloutRadius:
        if (auto* c = std::get_if<vec::CalloutShape>(ps))
            c->cornerRadius = std::clamp(sval(), 0.0, minorOf(c->size) * 0.5);
        break;
    case Role::CalloutTailKind:
        if (auto* c = std::get_if<vec::CalloutShape>(ps)) {
            c->tail = static_cast<vec::CalloutShape::Tail>(dval());
            const bool pointer = c->tail == vec::CalloutShape::Tail::Pointer;
            setEnabled(sliderFor(Role::CalloutSkew, 0), pointer);
            setEnabled(sliderFor(Role::CalloutPuffs, 0), !pointer);
        }
        break;
    case Role::CalloutAngle:
        if (auto* c = std::get_if<vec::CalloutShape>(ps)) c->tailAngle = toRad();
        break;
    case Role::CalloutLength:
        if (auto* c = std::get_if<vec::CalloutShape>(ps)) c->tailLength = std::max(0.0, sval());
        break;
    case Role::CalloutWidth:
        if (auto* c = std::get_if<vec::CalloutShape>(ps)) c->tailWidth = std::max(0.0, sval());
        break;
    case Role::CalloutSkew:
        if (auto* c = std::get_if<vec::CalloutShape>(ps))
            c->tailSkew = std::clamp(sval() / 100.0, -1.0, 1.0);
        break;
    case Role::CalloutPuffs:
        if (auto* c = std::get_if<vec::CalloutShape>(ps))
            c->bubbleCount = std::clamp(static_cast<int>(std::lround(sval())), 1, 8);
        break;

    // ---- arrow ----
    case Role::ArrowShaft:
        if (auto* a = std::get_if<vec::ArrowShape>(ps))
            a->shaftRatio = std::clamp(sval() / 100.0, 0.02, 1.0);
        break;
    case Role::ArrowHead:
        if (auto* a = std::get_if<vec::ArrowShape>(ps))
            a->headRatio = std::clamp(sval() / 100.0, 0.02, a->doubleHeaded ? 0.49 : 0.95);
        break;
    case Role::ArrowNotch:
        if (auto* a = std::get_if<vec::ArrowShape>(ps))
            a->notchRatio = std::clamp(sval() / 100.0, 0.0, 0.95);
        break;
    case Role::ArrowDouble:
        if (auto* a = std::get_if<vec::ArrowShape>(ps)) {
            a->doubleHeaded = cval();
            // Two heads must fit: clamp the head length and let the slider follow (syncControls),
            // so the number on the bar never disagrees with the shape being drawn.
            if (a->doubleHeaded) a->headRatio = std::min(a->headRatio, 0.49);
            syncControls();
        }
        break;

    // ---- ring / pie ----
    case Role::RingInner:
        if (auto* r = std::get_if<vec::RingShape>(ps))
            r->innerRatio = std::clamp(sval() / 100.0, 0.0, 0.95);
        break;
    case Role::RingStart:
    case Role::RingEnd:
        if (auto* r = std::get_if<vec::RingShape>(ps)) {
            applySweepAngle(r->startAngle, r->endAngle, role == Role::RingStart, toRad());
            syncControls();
        }
        break;

    // ---- cross ----
    case Role::CrossArm:
        if (auto* x = std::get_if<vec::CrossShape>(ps))
            x->armRatio = std::clamp(sval() / 100.0, 0.02, 0.98);
        break;
    case Role::CrossRadius:
        if (auto* x = std::get_if<vec::CrossShape>(ps))
            x->cornerRadius = std::max(0.0, sval());
        break;
    case Role::CrossStyle:
        if (auto* x = std::get_if<vec::CrossShape>(ps)) x->cornerStyle = rectStyleFor(dval());
        break;

    // ---- heart ----
    case Role::HeartLobe:
        if (auto* hs = std::get_if<vec::HeartShape>(ps))
            hs->lobe = std::clamp(sval() / 100.0, 0.0, 1.0);
        break;
    case Role::HeartCleft:
        if (auto* hs = std::get_if<vec::HeartShape>(ps))
            hs->cleft = std::clamp(sval() / 100.0, 0.02, 0.60);
        break;

    // ---- chevron / banner ----
    case Role::BannerStyle:
        if (auto* b = std::get_if<vec::BannerShape>(ps))
            b->style = static_cast<vec::BannerShape::Style>(dval());
        break;
    case Role::BannerPoint:
        if (auto* b = std::get_if<vec::BannerShape>(ps))
            b->pointRatio = std::clamp(sval() / 100.0, 0.0, 0.45);
        break;
    case Role::BannerNotch:
        if (auto* b = std::get_if<vec::BannerShape>(ps)) b->notchTail = cval();
        break;
    case Role::BannerRadius:
        if (auto* b = std::get_if<vec::BannerShape>(ps)) b->cornerRadius = std::max(0.0, sval());
        break;

    // ---- the shared outline / dash section ----
    case Role::OutlineStyle:
        m_state->dashStyle = dval();
        m_obj.stroke.dashArray =
            dashPatternFor(m_state->dashStyle, m_obj.stroke.width, m_state->dashSpacing);
        break;
    case Role::OutlineSpacing:
        m_state->dashSpacing = sval() / 100.0; // the slider is a percentage
        m_obj.stroke.dashArray =
            dashPatternFor(m_state->dashStyle, m_obj.stroke.width, m_state->dashSpacing);
        break;
    case Role::OutlineRound:
        m_state->roundOutline = cval();
        m_obj.stroke.cap = m_state->roundOutline ? vec::LineCap::Round : vec::LineCap::Butt;
        m_obj.stroke.join = m_state->roundOutline ? vec::LineJoin::Round : vec::LineJoin::Miter;
        break;
    }
    emitEdit();
}

// A drag on an on-diagram handle (§7.4): map the dragged local point to the parameter that handle
// stands for. The mapping is a PURE function of the object (so it is unit-tested against the
// handle table it inverts); the member below only stores the result and re-reads every control from
// it, so a dragged handle and its slider/dial can never disagree.
std::vector<std::pair<int, common::Vec2>> shapeHandlePoints(const vec::Object& obj) {
    return handlePointsOf(&obj);
}

std::optional<vec::Object> shapeAfterHandleDrag(const vec::Object& obj, int handle,
                                                common::Vec2 local, bool linkCorners) {
    vec::Object out = obj;
    auto* ps = std::get_if<vec::ParametricShape>(&out.geometry);
    if (ps == nullptr)
        return std::nullopt;
    // Move an angle handle to `local`, sharing applySweepAngle with the angle dials so the two
    // routes into the same parameter behave identically across the 0/2*pi seam.
    const auto dragAngle = [&](double rx, double ry, double& startA, double& endA, bool isStart) {
        if (std::abs(rx) < 1e-9 || std::abs(ry) < 1e-9) return;
        applySweepAngle(startA, endA, isStart,
                        std::atan2(local.y / std::abs(ry), local.x / std::abs(rx)));
    };

    // Every corner-radius arm below runs cornerRadiusForPoint -- the exact inverse of the apex the
    // handle is drawn at. So the knob stays under the cursor through the whole drag, and it
    // saturates at precisely the radius the outline saturates at instead of running on invisibly.
    if (auto* r = std::get_if<vec::RectShape>(ps)) { // a corner-radius handle (id 0-3)
        if (handle < 0 || handle > 3) return std::nullopt;
        const std::size_t slot = static_cast<std::size_t>(handle);
        const vec::CorneredPolygon poly = vec::rectPolygon(*r);
        const double rad = vec::cornerRadiusForPoint(poly.verts, slot, poly.styles[slot], local);
        r->cornerRadius[slot] = rad;
        if (linkCorners) r->cornerRadius.fill(rad);
    } else if (auto* p = std::get_if<vec::PolygonShape>(ps)) { // corner radius, at the top vertex
        const vec::CorneredPolygon poly = vec::polygonPolygon(*p);
        p->cornerRadius = vec::cornerRadiusForPoint(poly.verts, 0, p->cornerStyle, local);
    } else if (auto* st = std::get_if<vec::StarShape>(ps)) {   // the inner (valley) radius
        const int pts = std::max(2, st->points);
        const double va = -M_PI / 2.0 + M_PI / pts;
        const common::Vec2 radial{std::cos(va), std::sin(va)};
        // The handle rides the ROUNDED valley, so back its fillet offset out before reading the
        // radius off the valley radial (at valleyRadius 0 the offset is 0 and this is the old map).
        const double off = cornerOffsetAt(vec::starPolygon(*st), 1).dot(radial);
        const double outer = std::abs(st->outerRadius);
        st->innerRadius = std::clamp(local.dot(radial) - off, 0.05 * outer, 0.95 * outer);
    } else if (auto* e = std::get_if<vec::EllipseShape>(ps)) {
        dragAngle(e->radii.x, e->radii.y, e->startAngle, e->endAngle, handle == 0);
    } else if (auto* c = std::get_if<vec::CalloutShape>(ps)) {
        const double minor = minorOf(c->size);
        if (handle == 0) { // the TIP: its direction and its reach, in one grab (skew is untouched)
            if (local.length() > 1e-9) c->tailAngle = std::atan2(local.y, local.x);
            const CalloutAnchor a = calloutAnchor(*c);
            const common::Vec2 dir{std::cos(c->tailAngle), std::sin(c->tailAngle)};
            c->tailLength = std::clamp((local - a.base).dot(dir), 0.0, minor * 0.5);
        } else if (handle == 1) { // the base width, measured across the body's tangent there
            const CalloutAnchor a = calloutAnchor(*c);
            c->tailWidth = std::clamp(2.0 * std::abs((local - a.base).dot(a.tangent)), 0.0, minor);
        } else if (handle == 2) { // the body corner radius (the rect handle, on the TL corner)
            const vec::CorneredPolygon body = vec::calloutBodyPolygon(*c);
            if (!body.empty())
                c->cornerRadius =
                    vec::cornerRadiusForPoint(body.verts, 0, vec::CornerStyle::Round, local);
        }
    } else if (auto* a = std::get_if<vec::ArrowShape>(ps)) {
        const double w = std::abs(a->size.x), hh = std::abs(a->size.y) * 0.5;
        if (handle == 0 && w > 1e-9)
            a->headRatio = std::clamp((w * 0.5 - local.x) / w, 0.02, a->doubleHeaded ? 0.49 : 0.95);
        else if (handle == 1 && hh > 1e-9)
            a->shaftRatio = std::clamp(std::abs(local.y) / hh, 0.02, 1.0);
    } else if (auto* rg = std::get_if<vec::RingShape>(ps)) {
        const double rx = std::abs(rg->radii.x), ry = std::abs(rg->radii.y);
        if (handle == 0 && rx > 1e-9 && ry > 1e-9) {
            const double k = std::sqrt((local.x / rx) * (local.x / rx) +
                                       (local.y / ry) * (local.y / ry));
            rg->innerRatio = std::clamp(k, 0.0, 0.95);
        } else if (handle == 1 || handle == 2) {
            dragAngle(rg->radii.x, rg->radii.y, rg->startAngle, rg->endAngle, handle == 1);
        }
    } else if (auto* x = std::get_if<vec::CrossShape>(ps)) {
        const double minor = minorOf(x->size);
        const vec::CorneredPolygon poly = vec::crossPolygon(*x);
        if (poly.empty()) return std::nullopt;
        if (handle == 0 && minor > 1e-9) {
            // Vertex 1 sits at +ht; its rounded apex is pulled inward, so undo that before the
            // cursor's x reads as an arm half-thickness.
            const double off = cornerOffsetAt(poly, 1).x;
            x->armRatio = std::clamp(2.0 * std::abs(local.x - off) / minor, 0.02, 0.98);
        } else if (handle == 1) {
            x->cornerRadius = vec::cornerRadiusForPoint(poly.verts, 0, x->cornerStyle, local);
        }
    } else if (auto* hs = std::get_if<vec::HeartShape>(ps)) {
        const double h = std::abs(hs->size.y);
        if (h < 1e-9) return std::nullopt;
        const double frac = (local.y + h * 0.5) / h;
        if (handle == 0)
            hs->cleft = std::clamp(frac, 0.02, 0.60);
        else
            hs->lobe = std::clamp((frac - 0.20) / 0.19, 0.0, 1.0);
    } else if (auto* b = std::get_if<vec::BannerShape>(ps)) {
        const double w = std::abs(b->size.x), hw = w * 0.5;
        if (w < 1e-9) return std::nullopt;
        const vec::CorneredPolygon poly = vec::bannerPolygon(*b);
        if (poly.empty()) return std::nullopt;
        if (handle == 0) {
            const std::size_t depth = b->style == vec::BannerShape::Style::Chevron ? 1u : 2u;
            const double off = cornerOffsetAt(poly, depth).x;
            b->pointRatio = std::clamp((hw - (local.x - off)) / w, 0.0, 0.45);
        } else {
            b->cornerRadius =
                vec::cornerRadiusForPoint(poly.verts, 0, vec::CornerStyle::Round, local);
        }
    } else {
        return std::nullopt; // a kind with no draggable handles
    }
    return out;
}

void ShapeDesigner::applyDiagramDrag(int handle, common::Vec2 local) {
    const std::optional<vec::Object> edited =
        shapeAfterHandleDrag(m_obj, handle, local, m_state->linkCorners);
    if (!edited)
        return;
    m_obj = *edited;
    syncControls();
    emitEdit();
}

namespace {
void onControlCb(Fl_Widget* w, void* u) {
    auto* b = static_cast<Binding*>(u);
    if (b != nullptr && b->self != nullptr)
        b->self->applyControl(w, static_cast<int>(b->role), b->idx);
}
void onDisclosureCb(Fl_Widget* /*w*/, void* u) {
    if (auto* self = static_cast<ShapeDesigner*>(u))
        self->toggleOutline();
}
} // namespace

} // namespace mosaic::ui
