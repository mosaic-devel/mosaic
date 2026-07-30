// Extrude-mesh tests (S30-c, docs/type-tool.md §10.2). Pure geometry on hand-built contours (no
// fonts, fully deterministic): watertightness by edge-manifold counting, analytic normals, cap
// areas, bevel blending, UV domain, and the §10.4 per-run ranges. Watertightness-by-counting is
// the load-bearing assert -- a gap or T-junction breaks the 2-manifold property immediately.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "core/text/extrude_mesh.hpp"

using namespace mosaic::core::text;
namespace vec = mosaic::core::vec;
using mosaic::common::Vec2;
using mosaic::common::Vec3;

namespace {

vec::Contour ring(std::initializer_list<Vec2> pts) {
    vec::Contour c;
    c.points = pts;
    c.closed = true;
    return c;
}

GlyphSolidInput square10(std::size_t run = 0) {
    // 10x10 square at the origin (visually clockwise in y-down space).
    return {{ring({{0, 0}, {10, 0}, {10, 10}, {0, 10}})}, run};
}

GlyphSolidInput squareWithHole(std::size_t run = 0) {
    // 10x10 outer, 4x4 hole centred (hole deliberately wound the SAME way -- classification must
    // not depend on input winding).
    return {{ring({{0, 0}, {10, 0}, {10, 10}, {0, 10}}), ring({{3, 3}, {7, 3}, {7, 7}, {3, 7}})},
            run};
}

// Every undirected edge of a closed 2-manifold is shared by exactly two triangles. Keyed by
// QUANTIZED positions, so band/cap junction vertices (duplicated for their normals) still count
// as the same geometric edge.
bool watertight(const ExtrudeMesh& m) {
    using Key = std::tuple<long long, long long, long long>;
    const auto q = [](const Vec3& p) -> Key {
        return {std::llround(p.x * 4096.0), std::llround(p.y * 4096.0),
                std::llround(p.z * 4096.0)};
    };
    std::map<std::pair<Key, Key>, int> edges;
    for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        for (int e = 0; e < 3; ++e) {
            Key a = q(m.vertices[m.indices[t + static_cast<std::size_t>(e)]].position);
            Key b = q(m.vertices[m.indices[t + static_cast<std::size_t>((e + 1) % 3)]].position);
            if (b < a) std::swap(a, b);
            if (a == b) return false;  // a degenerate (zero-length) edge
            ++edges[{a, b}];
        }
    }
    for (const auto& [k, count] : edges)
        if (count != 2) return false;
    return !edges.empty();
}

// Total 2D area of the FRONT cap (normal +z): the solid's face area, for coverage asserts.
double frontCapArea(const ExtrudeMesh& m) {
    double area = 0.0;
    for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const ExtrudeVertex& a = m.vertices[m.indices[t]];
        const ExtrudeVertex& b = m.vertices[m.indices[t + 1]];
        const ExtrudeVertex& c = m.vertices[m.indices[t + 2]];
        if (a.normal.z < 0.99 || b.normal.z < 0.99 || c.normal.z < 0.99) continue;
        area += std::abs((b.position.x - a.position.x) * (c.position.y - a.position.y) -
                         (c.position.x - a.position.x) * (b.position.y - a.position.y)) *
                0.5;
    }
    return area;
}

Extrude plain(float depth = 20.0f) {
    Extrude e;
    e.depth = depth;
    return e;  // default bevels are size 0 (off)
}

}  // namespace

TEST_CASE("empty input yields an empty mesh; open contours are ignored") {
    CHECK(buildExtrudeMesh({}, plain()).empty());

    vec::Contour open;
    open.points = {{0, 0}, {10, 0}, {10, 10}};
    open.closed = false;
    CHECK(buildExtrudeMesh({{{open}, 0}}, plain()).empty());
}

TEST_CASE("a square extrudes to the expected watertight box") {
    const ExtrudeMesh m = buildExtrudeMesh({square10()}, plain(20.0f));
    CHECK(m.triangleCount() == 12);  // 2 front + 2 back + 4 walls x 2
    CHECK(watertight(m));
    REQUIRE(m.ranges.size() == 1);
    CHECK(m.ranges[0].indexCount == m.indices.size());

    int front = 0, back = 0, walls = 0;
    for (const ExtrudeVertex& v : m.vertices) {
        CHECK(v.normal.length() == doctest::Approx(1.0));
        if (v.normal.z > 0.99) {
            ++front;
            CHECK(v.position.z == doctest::Approx(10.0));  // +depth/2 toward the viewer
        } else if (v.normal.z < -0.99) {
            ++back;
            CHECK(v.position.z == doctest::Approx(-10.0));
        } else {
            ++walls;
            CHECK(v.normal.z == doctest::Approx(0.0));  // wall normals are horizontal...
            // ...and point AWAY from the solid's centre (5,5,*).
            const double dx = v.position.x - 5.0, dy = v.position.y - 5.0;
            CHECK(v.normal.x * dx + v.normal.y * dy > 0.0);
        }
    }
    CHECK(front == 4);
    CHECK(back == 4);
    CHECK(walls > 0);
}

TEST_CASE("a hole carves the cap and its walls face into the hole") {
    const ExtrudeMesh m = buildExtrudeMesh({squareWithHole()}, plain(20.0f));
    CHECK(watertight(m));
    CHECK(frontCapArea(m) == doctest::Approx(100.0 - 16.0));

    // Wall vertices on the hole ring (|x-5|,|y-5| <= 2ish, z interior) must face the hole centre.
    bool sawHoleWall = false;
    for (const ExtrudeVertex& v : m.vertices) {
        if (std::abs(v.normal.z) > 0.01) continue;  // walls only
        if (v.position.x < 2.9 || v.position.x > 7.1 || v.position.y < 2.9 || v.position.y > 7.1)
            continue;  // outer walls
        sawHoleWall = true;
        const double dx = v.position.x - 5.0, dy = v.position.y - 5.0;
        CHECK(v.normal.x * dx + v.normal.y * dy < 0.0);  // inward = toward the hole's middle
    }
    CHECK(sawHoleWall);
}

TEST_CASE("input winding does not matter (containment depth drives roles)") {
    // The same square wound the other way round.
    GlyphSolidInput ccw{{ring({{0, 10}, {10, 10}, {10, 0}, {0, 0}})}, 0};
    const ExtrudeMesh a = buildExtrudeMesh({square10()}, plain());
    const ExtrudeMesh b = buildExtrudeMesh({ccw}, plain());
    CHECK(a.triangleCount() == b.triangleCount());
    CHECK(watertight(b));
    CHECK(frontCapArea(a) == doctest::Approx(frontCapArea(b)));
}

TEST_CASE("nested rings alternate solid/hole (ring in a hole is its own island)") {
    GlyphSolidInput nested{{ring({{0, 0}, {20, 0}, {20, 20}, {0, 20}}),
                           ring({{4, 4}, {16, 4}, {16, 16}, {4, 16}}),
                           ring({{8, 8}, {12, 8}, {12, 12}, {8, 12}})},
                          0};
    const ExtrudeMesh m = buildExtrudeMesh({nested}, plain());
    CHECK(watertight(m));
    // Face area = big minus mid plus the island: 400 - 144 + 16.
    CHECK(frontCapArea(m) == doctest::Approx(400.0 - 144.0 + 16.0));
}

TEST_CASE("every bevel profile stays watertight and blends its normals") {
    for (const Bevel::Profile prof : {Bevel::Profile::Flat, Bevel::Profile::Round,
                                      Bevel::Profile::Convex, Bevel::Profile::Concave}) {
        Extrude e = plain(20.0f);
        e.bevelFront.profile = prof;
        e.bevelFront.size = 2.0f;
        e.bevelFront.segments = 3;
        const ExtrudeMesh m = buildExtrudeMesh({square10()}, e);
        CHECK(watertight(m));
        // Somewhere on the bevel a normal genuinely blends between wall (z=0) and cap (z=1).
        bool blended = false;
        for (const ExtrudeVertex& v : m.vertices)
            if (v.normal.z > 0.05 && v.normal.z < 0.95) blended = true;
        CHECK(blended);
    }
}

TEST_CASE("a front bevel insets the front cap by its size") {
    Extrude e = plain(20.0f);
    e.bevelFront.size = 2.0f;
    const ExtrudeMesh m = buildExtrudeMesh({square10()}, e);
    CHECK(watertight(m));
    double maxX = -1e9;
    for (const ExtrudeVertex& v : m.vertices)
        if (v.normal.z > 0.99) maxX = std::max(maxX, v.position.x);
    CHECK(maxX == doctest::Approx(8.0));  // 10 minus the 2px bevel
    // The back cap is untouched.
    double maxXBack = -1e9;
    for (const ExtrudeVertex& v : m.vertices)
        if (v.normal.z < -0.99) maxXBack = std::max(maxXBack, v.position.x);
    CHECK(maxXBack == doctest::Approx(10.0));
}

TEST_CASE("oversized bevels are clamped to the depth instead of eating the solid") {
    Extrude e = plain(4.0f);
    e.bevelFront.size = 10.0f;  // front+back would be 5x the depth
    e.bevelBack.size = 10.0f;
    const ExtrudeMesh m = buildExtrudeMesh({square10()}, e);
    CHECK_FALSE(m.empty());
    CHECK(watertight(m));
}

TEST_CASE("an oversized bevel saturates at the local stroke width instead of folding the cap") {
    // A long thin bar (30 x 4): unclamped, a 10px inset would march the two long walls 10px past
    // each other -- the cap outline self-intersects and earcut emits garbage (the in-app ">4px
    // glitch"). The per-vertex clamp must keep every front-cap point inside the bar's footprint.
    GlyphSolidInput bar{{ring({{0, 0}, {30, 0}, {30, 4}, {0, 4}})}, 0};
    for (const Bevel::Profile prof : {Bevel::Profile::Flat, Bevel::Profile::Round,
                                      Bevel::Profile::Convex, Bevel::Profile::Concave}) {
        Extrude e = plain(40.0f);  // deep: the depth clamp never kicks in, only the stroke one
        e.bevelFront.profile = prof;
        e.bevelFront.size = 10.0f;
        const ExtrudeMesh m = buildExtrudeMesh({bar}, e);
        REQUIRE(!m.empty());
        CHECK(watertight(m));
        bool anyCap = false;
        for (const ExtrudeVertex& v : m.vertices) {
            if (std::abs(v.position.z - 20.0) > 1e-9) continue;  // front cap plane only
            anyCap = true;
            // The Convex bulge may poke OUTWARD (negative inset), but proportionately to the
            // stroke -- never the unclamped ~2px, let alone the glyph-swallowing full size.
            CHECK(v.position.x >= 0.0 - 2.0);
            CHECK(v.position.x <= 30.0 + 2.0);
            CHECK(v.position.y >= 0.0 - 2.0);
            CHECK(v.position.y <= 4.0 + 2.0);
        }
        CHECK(anyCap);
    }
    // And the flat case specifically must leave a genuine (uncrossed) cap sliver: every top-wall
    // inset point stays ABOVE every bottom-wall inset point.
    Extrude e = plain(40.0f);
    e.bevelFront.profile = Bevel::Profile::Flat;
    e.bevelFront.size = 10.0f;
    const ExtrudeMesh m = buildExtrudeMesh({bar}, e);
    double minCapY = 1e9, maxCapY = -1e9;
    for (const ExtrudeVertex& v : m.vertices) {
        if (std::abs(v.position.z - 20.0) > 1e-9 || v.normal.z < 0.99) continue;
        minCapY = std::min(minCapY, v.position.y);
        maxCapY = std::max(maxCapY, v.position.y);
    }
    CHECK(minCapY > 0.0);
    CHECK(maxCapY < 4.0);
    CHECK(minCapY < maxCapY);  // an actual sliver, not a degenerate line
}

TEST_CASE("an oversized bevel on a sharp wedge stays watertight (apex saturates first)") {
    // A thin wedge (a 'V' leg): the local safe travel shrinks toward the apex; the clamp must
    // saturate progressively there without breaking the 2-manifold property.
    GlyphSolidInput wedge{{ring({{0, 0}, {30, 0}, {30, 10}})}, 0};
    Extrude e = plain(40.0f);
    e.bevelFront.size = 8.0f;
    e.bevelFront.profile = Bevel::Profile::Round;
    const ExtrudeMesh m = buildExtrudeMesh({wedge}, e);
    REQUIRE(!m.empty());
    CHECK(watertight(m));
}

TEST_CASE("UVs span the design bounds in [0,1] for every vertex") {
    const ExtrudeMesh m = buildExtrudeMesh({square10()}, plain());
    CHECK(m.designBounds.w == doctest::Approx(10.0));
    CHECK(m.designBounds.h == doctest::Approx(10.0));
    for (const ExtrudeVertex& v : m.vertices) {
        CHECK(v.uv.x >= -1e-9);
        CHECK(v.uv.x <= 1.0 + 1e-9);
        CHECK(v.uv.y >= -1e-9);
        CHECK(v.uv.y <= 1.0 + 1e-9);
    }
}

TEST_CASE("per-run ranges partition the index buffer (and same-run glyphs merge)") {
    GlyphSolidInput a = square10(0);
    GlyphSolidInput b{{ring({{20, 0}, {30, 0}, {30, 10}, {20, 10}})}, 2};
    GlyphSolidInput c{{ring({{40, 0}, {50, 0}, {50, 10}, {40, 10}})}, 2};
    const ExtrudeMesh m = buildExtrudeMesh({a, b, c}, plain());
    REQUIRE(m.ranges.size() == 2);  // run 0, then runs 2+2 merged
    CHECK(m.ranges[0].runIndex == 0);
    CHECK(m.ranges[1].runIndex == 2);
    CHECK(m.ranges[0].firstIndex == 0);
    CHECK(m.ranges[0].indexCount + m.ranges[1].indexCount == m.indices.size());
    CHECK(m.ranges[1].firstIndex == m.ranges[0].indexCount);
    CHECK(watertight(m));  // disjoint solids: each closed on its own, the union still 2-manifold
}
