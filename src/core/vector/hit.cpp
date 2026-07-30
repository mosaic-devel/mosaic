#include "core/vector/hit.hpp"

#include <algorithm>
#include <limits>
#include <variant>

#include "core/vector/flatten.hpp"

namespace mosaic::core::vec {
namespace {

using common::Vec2;

// 2D cross product of vectors; sign tells which side of a->b a point lies on (Sunday's isLeft).
double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }

// Sunday winding number: net signed crossings of an upward ray; nonzero == inside (NonZero rule).
int windingNumber(const Contours& cs, Vec2 p) {
    int wn = 0;
    for (const auto& c : cs) {
        const auto& pts = c.points;
        const std::size_t n = pts.size();
        if (n < 2) continue;
        for (std::size_t i = 0; i < n; ++i) {  // every contour closes for fill
            const Vec2 a = pts[i];
            const Vec2 b = pts[(i + 1) % n];
            if (a.y <= p.y) {
                if (b.y > p.y && cross(b - a, p - a) > 0.0) ++wn;
            } else {
                if (b.y <= p.y && cross(b - a, p - a) < 0.0) --wn;
            }
        }
    }
    return wn;
}

// Parity of ray crossings; odd == inside (EvenOdd rule).
bool crossingsOdd(const Contours& cs, Vec2 p) {
    bool in = false;
    for (const auto& c : cs) {
        const auto& pts = c.points;
        const std::size_t n = pts.size();
        if (n < 2) continue;
        for (std::size_t i = 0; i < n; ++i) {
            const Vec2 a = pts[i];
            const Vec2 b = pts[(i + 1) % n];
            if ((a.y > p.y) != (b.y > p.y)) {
                const double xint = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
                if (p.x < xint) in = !in;
            }
        }
    }
    return in;
}

double segmentDistance(Vec2 a, Vec2 b, Vec2 p) {
    const Vec2 ab = b - a;
    const double len2 = ab.dot(ab);
    if (len2 <= 1e-20) return (p - a).length();
    double t = (p - a).dot(ab) / len2;
    t = std::clamp(t, 0.0, 1.0);
    return (p - (a + ab * t)).length();
}

}  // namespace

bool contains(const Contours& cs, Vec2 p, FillRule rule) {
    return rule == FillRule::EvenOdd ? crossingsOdd(cs, p) : (windingNumber(cs, p) != 0);
}

double distanceToOutline(const Contours& cs, Vec2 p) {
    double best = std::numeric_limits<double>::infinity();
    for (const auto& c : cs) {
        const auto& pts = c.points;
        const std::size_t n = pts.size();
        if (n == 0) continue;
        if (n == 1) {
            best = std::min(best, (pts[0] - p).length());
            continue;
        }
        const std::size_t segs = c.closed ? n : n - 1;
        for (std::size_t i = 0; i < segs; ++i)
            best = std::min(best, segmentDistance(pts[i], pts[(i + 1) % n], p));
    }
    return best;
}

FillRule fillRuleOf(const Geometry& g) {
    if (const auto* path = std::get_if<Path>(&g)) return path->fillRule;
    // Primitives are NonZero, and so is a BooleanCompound -- full stop. The boolean kernel emits
    // outers wound positive and holes negative (core/vector/boolean.cpp §5), so its result is
    // unambiguously correct under NonZero and there is no rule to negotiate here.
    return FillRule::NonZero;
}

bool hitTest(const Object& obj, Vec2 pLocal, double pickRadiusLocal, double flattenTol) {
    const Contours cs = flatten(obj.geometry, flattenTol);
    if (cs.empty()) return false;
    const bool fillable = !std::holds_alternative<NoPaint>(obj.fill);
    if (fillable && contains(cs, pLocal, fillRuleOf(obj.geometry))) return true;
    double band = std::max(pickRadiusLocal, 0.0);
    if (obj.stroke.enabled) band += obj.stroke.width * 0.5;
    return distanceToOutline(cs, pLocal) <= band;
}

}  // namespace mosaic::core::vec
