#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "render/warp.hpp"
#include "ui/warp_gesture.hpp"

using namespace mosaic;
using core::WarpGrid;
using core::WarpKind;
using render::ResampleFilter;

// ---------------------------------------------------------------------------------------------
// S35-b Mesh Warp / Perspective Warp (docs/warp-tools.md). The properties pinned here are the ones
// the design is ABOUT, not a recorded output: an undeformed lattice is the identity byte for byte,
// the surface passes exactly through its control points, a 2-point axis is exactly linear, the
// homography solve reproduces its own correspondences, a non-convex quad is REFUSED, the walk leaves
// no holes, and a re-edit applies the difference rather than the whole displacement twice. There is
// no bless mechanism in this repo, so everything checkable in closed form is checked that way.
// ---------------------------------------------------------------------------------------------
namespace {

// A per-pixel-unique opaque image: (x, y) is recoverable from the colour, so a warp's mapping can be
// read straight off the output instead of inferred from an average.
common::Image tagImage(std::uint32_t w, std::uint32_t h) {
    common::Image img(w, h);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
            img.rgba[p] = static_cast<std::uint8_t>(x * 4);
            img.rgba[p + 1] = static_cast<std::uint8_t>(y * 4);
            img.rgba[p + 2] = 128;
            img.rgba[p + 3] = 255;
        }
    }
    return img;
}

std::uint8_t alphaAt(const common::Image& img, std::uint32_t x, std::uint32_t y) {
    return img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4 + 3];
}

WarpGrid meshGrid(int cols, int rows, double w, double h) {
    return core::identityWarpGrid(common::Rect{0, 0, w, h}, cols, rows, WarpKind::Mesh);
}

} // namespace

TEST_CASE("identityWarpGrid seeds a valid, undeformed lattice on its own source rect") {
    const WarpGrid g = meshGrid(4, 3, 90.0, 60.0);
    CHECK(g.valid());
    CHECK(g.identity());
    CHECK(g.cols == 4);
    CHECK(g.rows == 3);
    CHECK(g.points.size() == 12);
    // The OUTER ring sits exactly on the framed rect's edges -- exact ==, not Approx: latticePoint
    // recomputes the very expression the seeding used, so an untouched grid is bit-identical.
    CHECK(g.point(0, 0) == common::Vec2{0.0, 0.0});
    CHECK(g.point(3, 0) == common::Vec2{90.0, 0.0});
    CHECK(g.point(0, 2) == common::Vec2{0.0, 60.0});
    CHECK(g.point(3, 2) == common::Vec2{90.0, 60.0});
    // ... and identity() notices a single moved point.
    WarpGrid moved = g;
    moved.points[5] = moved.points[5] + common::Vec2{1.0, 0.0};
    CHECK_FALSE(moved.identity());
}

TEST_CASE("Perspective is forced to 2x2 whatever the caller asks for") {
    const WarpGrid g =
        core::identityWarpGrid(common::Rect{0, 0, 40, 40}, 5, 7, WarpKind::Perspective);
    CHECK(g.cols == 2);
    CHECK(g.rows == 2);
    CHECK(g.points.size() == 4);
    CHECK(g.valid());
    // A hand-built 3x3 "perspective" grid is NOT valid: one homography has four corners.
    WarpGrid bad = meshGrid(3, 3, 40.0, 40.0);
    bad.kind = WarpKind::Perspective;
    CHECK_FALSE(bad.valid());
}

TEST_CASE("valid() rejects the grids the kernel and the overlay could not agree about") {
    WarpGrid g = meshGrid(3, 3, 10.0, 10.0);
    CHECK(g.valid());
    SUBCASE("a point count that does not match the lattice") {
        g.points.pop_back();
        CHECK_FALSE(g.valid());
    }
    SUBCASE("a degenerate source rect") {
        g.source.w = 0.0;
        CHECK_FALSE(g.valid());
    }
    SUBCASE("a single-point axis") {
        WarpGrid line = meshGrid(1, 3, 10.0, 10.0); // identityWarpGrid clamps this back up to 2
        CHECK(line.cols == 2);
        line.cols = 1;
        CHECK_FALSE(line.valid());
    }
}

TEST_CASE("translatedWarpGrid slides the source rect AND every point") {
    const WarpGrid g = meshGrid(2, 2, 8.0, 8.0);
    const WarpGrid t = core::translatedWarpGrid(g, {-3.0, 5.0});
    CHECK(t.source.x == -3.0);
    CHECK(t.source.y == 5.0);
    CHECK(t.source.w == g.source.w);
    CHECK(t.point(0, 0) == common::Vec2{-3.0, 5.0});
    CHECK(t.point(1, 1) == common::Vec2{5.0, 13.0});
    // Sliding by the negation puts it back exactly -- the round trip the bake's re-homing depends on.
    CHECK(core::translatedWarpGrid(t, {3.0, -5.0}) == g);
}

TEST_CASE("the Catmull-Rom surface passes exactly through its control points") {
    WarpGrid g = meshGrid(4, 4, 30.0, 30.0);
    g.points[5] = g.points[5] + common::Vec2{7.0, -4.0}; // an interior node, pulled
    g.points[0] = g.points[0] + common::Vec2{-2.0, 3.0}; // ... and a corner
    for (int r = 0; r < g.rows; ++r) {
        for (int c = 0; c < g.cols; ++c) {
            const common::Vec2 s =
                render::warpSurfacePoint(g, static_cast<double>(c), static_cast<double>(r));
            CHECK(s.x == doctest::Approx(g.point(c, r).x).epsilon(1e-9));
            CHECK(s.y == doctest::Approx(g.point(c, r).y).epsilon(1e-9));
        }
    }
}

TEST_CASE("a 2-point axis is EXACTLY linear (which is why Perspective is its own engine)") {
    // The phantom extrapolation P[-1] = 2P[0] - P[1] cancels the quadratic and cubic terms, so a
    // 2x2 lattice interpolates bilinearly. If this ever stops being true, "a 2x2 mesh" would start
    // silently disagreeing with the bilinear quad the rest of the app assumes.
    WarpGrid g = meshGrid(2, 2, 10.0, 10.0);
    g.points[3] = {20.0, 30.0}; // pull the bottom-right corner
    for (double t = 0.0; t <= 1.0; t += 0.125) {
        const common::Vec2 top = render::warpSurfacePoint(g, t, 0.0);
        CHECK(top.x == doctest::Approx(10.0 * t).epsilon(1e-9));
        CHECK(top.y == doctest::Approx(0.0).epsilon(1e-9));
        const common::Vec2 bot = render::warpSurfacePoint(g, t, 1.0);
        CHECK(bot.x == doctest::Approx(t * 20.0).epsilon(1e-9));
        CHECK(bot.y == doctest::Approx(10.0 + t * 20.0).epsilon(1e-9));
    }
}

TEST_CASE("an undeformed mesh reproduces the source image byte for byte") {
    // The lattice is on its own nodes, so from == to and Auto resolves to Nearest: the sampler must
    // hand back the very pixels it was given, at the very offset it was given them. Anything less
    // means every no-op Apply would quietly degrade the layer.
    const common::Image src = tagImage(24, 16);
    const WarpGrid g = meshGrid(4, 3, 24.0, 16.0);
    const render::WarpResult r = render::warpImage(src, g, ResampleFilter::Auto);
    REQUIRE(r.ok);
    CHECK(r.offX == 0);
    CHECK(r.offY == 0);
    CHECK(r.px.width == 24);
    CHECK(r.px.height == 16);
    CHECK(r.px.rgba == src.rgba); // exact: no Approx has any business here
}

TEST_CASE("a pure lattice translation moves the extent, not the pixels") {
    const common::Image src = tagImage(20, 12);
    WarpGrid g = meshGrid(3, 3, 20.0, 12.0);
    for (common::Vec2& p : g.points) p = p + common::Vec2{5.0, -3.0};
    const render::WarpResult r = render::warpImage(src, g, ResampleFilter::Nearest);
    REQUIRE(r.ok);
    CHECK(r.offX == 5);
    CHECK(r.offY == -3);
    CHECK(r.px.width == 20);
    CHECK(r.px.height == 12);
    // A whole-pixel shift under Nearest is a copy: every interior pixel keeps its tag.
    for (std::uint32_t y = 1; y + 1 < r.px.height; ++y)
        for (std::uint32_t x = 1; x + 1 < r.px.width; ++x)
            CHECK(alphaAt(r.px, x, y) == 255);
}

TEST_CASE("the mesh walk leaves NO holes inside a deformed lattice") {
    // The one property a forward-mapping implementation cannot have. Every pixel well inside the
    // deformed region must be covered; a single transparent pixel in there is a crack.
    const common::Image src = tagImage(32, 32);
    WarpGrid g = meshGrid(4, 4, 32.0, 32.0);
    g.points[5] = g.points[5] + common::Vec2{6.0, 5.0};   // pull two interior nodes apart, which
    g.points[10] = g.points[10] + common::Vec2{-5.0, 7.0}; // is what makes the patches disagree
    const render::WarpResult r = render::warpImage(src, g, ResampleFilter::Bilinear);
    REQUIRE(r.ok);
    REQUIRE(r.px.width > 8);
    REQUIRE(r.px.height > 8);
    int transparent = 0;
    for (std::uint32_t y = 6; y + 6 < r.px.height; ++y)
        for (std::uint32_t x = 6; x + 6 < r.px.width; ++x)
            if (alphaAt(r.px, x, y) == 0) ++transparent;
    CHECK(transparent == 0);
}

TEST_CASE("solveHomography reproduces its own four correspondences") {
    const std::array<common::Vec2, 4> from{{{0, 0}, {100, 0}, {100, 80}, {0, 80}}};
    const std::array<common::Vec2, 4> to{{{10, 5}, {95, 20}, {110, 90}, {-5, 70}}};
    const std::optional<render::Homography> h = render::solveHomography(from, to);
    REQUIRE(h.has_value());
    for (std::size_t i = 0; i < 4; ++i) {
        const common::Vec2 mapped = h->apply(from[i]);
        CHECK(mapped.x == doctest::Approx(to[i].x).epsilon(1e-9));
        CHECK(mapped.y == doctest::Approx(to[i].y).epsilon(1e-9));
    }
    // ... and its inverse carries them back.
    const std::optional<render::Homography> hi = h->inverse();
    REQUIRE(hi.has_value());
    for (std::size_t i = 0; i < 4; ++i) {
        const common::Vec2 back = hi->apply(to[i]);
        CHECK(back.x == doctest::Approx(from[i].x).epsilon(1e-6));
        CHECK(back.y == doctest::Approx(from[i].y).epsilon(1e-6));
    }
}

TEST_CASE("convexQuad refuses the placements a projective map cannot honour") {
    CHECK(render::convexQuad({{{0, 0}, {10, 0}, {10, 10}, {0, 10}}}));      // a square
    CHECK(render::convexQuad({{{0, 0}, {10, 2}, {9, 11}, {-1, 8}}}));       // a sheared quad
    CHECK_FALSE(render::convexQuad({{{0, 0}, {10, 0}, {0, 10}, {10, 10}}})); // a bow tie
    CHECK_FALSE(render::convexQuad({{{0, 0}, {5, 5}, {10, 10}, {0, 10}}})); // a collinear corner
    CHECK_FALSE(render::convexQuad({{{0, 0}, {0, 0}, {0, 0}, {0, 0}}}));    // collapsed
}

TEST_CASE("a folded perspective quad is REFUSED, not rendered") {
    const common::Image src = tagImage(16, 16);
    WarpGrid g = core::identityWarpGrid(common::Rect{0, 0, 16, 16}, 2, 2, WarpKind::Perspective);
    // Swap the two bottom corners: the quad crosses itself, so there is no honest picture to make.
    std::swap(g.points[2], g.points[3]);
    const render::WarpResult r = render::warpImage(src, g, ResampleFilter::Bilinear);
    CHECK_FALSE(r.ok);
    CHECK(r.px.empty());
}

TEST_CASE("a perspective warp keeps the corners it was given and stays inside its quad") {
    const common::Image src = tagImage(40, 40);
    WarpGrid g = core::identityWarpGrid(common::Rect{0, 0, 40, 40}, 2, 2, WarpKind::Perspective);
    g.points[0] = {8.0, 0.0};  // TL pulled in: the classic "lay the plane back" drag
    g.points[1] = {32.0, 0.0}; // TR pulled in
    const render::WarpResult r = render::warpImage(src, g, ResampleFilter::Bilinear);
    REQUIRE(r.ok);
    CHECK(r.offX == 0);
    CHECK(r.offY == 0);
    CHECK(r.px.width == 40);
    CHECK(r.px.height == 40);
    // The top row's outer columns are OUTSIDE the trapezoid, so they must be empty; its middle is
    // inside, so it must be covered. That is the pre-image clip doing its job -- without it the
    // bounding box's corners fill with content the quad never claimed.
    CHECK(alphaAt(r.px, 1, 1) == 0);
    CHECK(alphaAt(r.px, 38, 1) == 0);
    CHECK(alphaAt(r.px, 20, 1) > 0);
    CHECK(alphaAt(r.px, 20, 38) > 0);
}

TEST_CASE("the two-grid form applies the DIFFERENCE, so a re-edit does not warp twice") {
    // This is the whole of the tool's re-editability. `once` is the first warp of a pristine image;
    // `again` re-warps the ALREADY-WARPED pixels with the stored grid as `from` and the same grid as
    // `to` -- a no-op deformation, which must leave them alone. Passing the stored grid as `to` with
    // the implicit undeformed `from` (the bug this form exists to prevent) would displace them a
    // second time and the extent would move again.
    const common::Image src = tagImage(24, 24);
    WarpGrid g = meshGrid(3, 3, 24.0, 24.0);
    g.points[4] = g.points[4] + common::Vec2{4.0, 4.0}; // pull the centre node
    const render::WarpResult once = render::warpImage(src, g, ResampleFilter::Bilinear);
    REQUIRE(once.ok);
    // Re-home the grid into the warped image's own pixel space, exactly as the gesture persists it.
    const WarpGrid stored = core::translatedWarpGrid(
        g, {-static_cast<double>(once.offX), -static_cast<double>(once.offY)});
    const render::WarpResult again =
        render::warpImage(once.px, stored, stored, ResampleFilter::Nearest);
    REQUIRE(again.ok);
    CHECK(again.offX == 0);
    CHECK(again.offY == 0);
    CHECK(again.px.width == once.px.width);
    CHECK(again.px.height == once.px.height);
    CHECK(again.px.rgba == once.px.rgba); // a null re-edit is byte-exact
}

TEST_CASE("warpImage refuses mismatched or invalid grids rather than guessing") {
    const common::Image src = tagImage(8, 8);
    const WarpGrid a = meshGrid(3, 3, 8.0, 8.0);
    const WarpGrid b = meshGrid(4, 4, 8.0, 8.0);
    CHECK_FALSE(render::warpImage(src, a, b, ResampleFilter::Bilinear).ok); // sizes disagree
    CHECK_FALSE(render::warpImage(common::Image{}, a, ResampleFilter::Bilinear).ok); // no pixels
    WarpGrid bad = a;
    bad.points.clear();
    CHECK_FALSE(render::warpImage(src, bad, ResampleFilter::Bilinear).ok);
}

TEST_CASE("warpIsolines draws one curve per lattice line, through the control points") {
    const WarpGrid g = meshGrid(4, 3, 30.0, 20.0);
    const auto runs = render::warpIsolines(g, 6);
    REQUIRE(runs.size() == static_cast<std::size_t>(g.rows + g.cols));
    // Each row line spans (cols-1)*steps + 1 samples; each column line (rows-1)*steps + 1.
    CHECK(runs[0].size() == static_cast<std::size_t>((g.cols - 1) * 6 + 1));
    CHECK(runs[static_cast<std::size_t>(g.rows)].size() ==
          static_cast<std::size_t>((g.rows - 1) * 6 + 1));
    // The first row line starts at the top-left node and ends at the top-right one.
    CHECK(runs[0].front().x == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(runs[0].back().x == doctest::Approx(30.0).epsilon(1e-9));
}

TEST_CASE("warpQuadLines gives the boundary plus the two diagonals") {
    const WarpGrid g =
        core::identityWarpGrid(common::Rect{0, 0, 10, 10}, 2, 2, WarpKind::Perspective);
    const auto runs = render::warpQuadLines(g);
    REQUIRE(runs.size() == 3);
    CHECK(runs[0].size() == 5);            // closed: the first corner repeats
    CHECK(runs[0].front() == runs[0].back());
    CHECK(runs[1].size() == 2);
    CHECK(runs[2].size() == 2);
    // The cyclic order is TL, TR, BR, BL -- NOT the row-major storage order.
    const auto q = render::warpQuadCorners(g);
    CHECK(q[0] == common::Vec2{0, 0});
    CHECK(q[1] == common::Vec2{10, 0});
    CHECK(q[2] == common::Vec2{10, 10});
    CHECK(q[3] == common::Vec2{0, 10});
}

// ---- ui/warp_gesture.hpp ---------------------------------------------------------------------

TEST_CASE("warpHandlePoints indexes row-major, and Perspective offers only its four corners") {
    const WarpGrid mesh = meshGrid(3, 2, 20.0, 10.0);
    const auto mh = ui::warpHandlePoints(mesh);
    REQUIRE(mh.size() == 6);
    CHECK(mh[0] == mesh.point(0, 0));
    CHECK(mh[2] == mesh.point(2, 0));
    CHECK(mh[3] == mesh.point(0, 1));
    const WarpGrid persp =
        core::identityWarpGrid(common::Rect{0, 0, 10, 10}, 2, 2, WarpKind::Perspective);
    CHECK(ui::warpHandlePoints(persp).size() == 4);
}

TEST_CASE("hitWarpHandle picks the NEAREST handle inside the screen-px radius, or nothing") {
    const std::vector<common::Vec2> handles{{0, 0}, {20, 0}, {0, 20}};
    CHECK(ui::hitWarpHandle(handles, {2, 2}, 9.0) == std::optional<int>(0));
    CHECK(ui::hitWarpHandle(handles, {18, 3}, 9.0) == std::optional<int>(1));
    CHECK_FALSE(ui::hitWarpHandle(handles, {10, 10}, 9.0).has_value()); // between all three
    CHECK_FALSE(ui::hitWarpHandle({}, {0, 0}, 9.0).has_value());
}

TEST_CASE("a handle drag follows the cursor's DELTA, never its absolute position") {
    // A press that lands a few px off the handle must not teleport it under the pointer.
    const WarpGrid base = meshGrid(3, 3, 20.0, 20.0);
    const common::Vec2 press{11.0, 9.0}; // near, but not on, the centre node at (10,10)
    const WarpGrid moved =
        ui::warpDragged(base, 4, press, {16.0, 9.0}, ui::WarpDragMode::Free);
    CHECK(moved.point(1, 1) == common::Vec2{15.0, 10.0}); // +5 in x, +0 in y
    CHECK(moved.point(0, 0) == base.point(0, 0));         // ... and nothing else moved
}

TEST_CASE("Shift locks a drag to the dominant axis; Alt slides the whole lattice") {
    const WarpGrid base = meshGrid(3, 3, 20.0, 20.0);
    const common::Vec2 press{10.0, 10.0};
    SUBCASE("x dominates") {
        const WarpGrid g =
            ui::warpDragged(base, 4, press, {18.0, 13.0}, ui::WarpDragMode::ConstrainAxis);
        CHECK(g.point(1, 1) == common::Vec2{18.0, 10.0});
    }
    SUBCASE("y dominates") {
        const WarpGrid g =
            ui::warpDragged(base, 4, press, {13.0, 18.0}, ui::WarpDragMode::ConstrainAxis);
        CHECK(g.point(1, 1) == common::Vec2{10.0, 18.0});
    }
    SUBCASE("Alt moves every node by the same delta and keeps the shape") {
        const WarpGrid g = ui::warpDragged(base, 0, press, {13.0, 17.0}, ui::WarpDragMode::MoveAll);
        for (int r = 0; r < base.rows; ++r)
            for (int c = 0; c < base.cols; ++c)
                CHECK(g.point(c, r) == base.point(c, r) + common::Vec2{3.0, 7.0});
    }
    SUBCASE("an out-of-range handle index changes nothing") {
        CHECK(ui::warpDragged(base, 99, press, {13.0, 17.0}, ui::WarpDragMode::Free) == base);
    }
}

TEST_CASE("warpDragModeFor: Alt outranks Shift") {
    CHECK(ui::warpDragModeFor(false, false) == ui::WarpDragMode::Free);
    CHECK(ui::warpDragModeFor(true, false) == ui::WarpDragMode::ConstrainAxis);
    CHECK(ui::warpDragModeFor(false, true) == ui::WarpDragMode::MoveAll);
    CHECK(ui::warpDragModeFor(true, true) == ui::WarpDragMode::MoveAll);
}

TEST_CASE("the drawn grid stays inside the overlay lane's vertex budget") {
    // The lattice rides render::kLassoMaxVerts, and the subdivision is halved until the estimate
    // fits. The biggest lattice the bar can ask for must still draw as a CURVE, not be clipped.
    const std::size_t budget = 4096;
    for (int n = ui::kWarpMinNodes; n <= ui::kWarpMaxNodes; ++n) {
        const WarpGrid g = meshGrid(n, n, 100.0, 100.0);
        const int steps = ui::warpLineSteps(g, budget);
        CHECK(steps >= 1);
        std::size_t total = 0;
        for (const auto& run : ui::warpGridLines(g, budget)) total += run.size() + 1;
        CHECK(total <= budget);
    }
}

TEST_CASE("warpBoundaryLines names the outer ring and nothing else") {
    const WarpGrid g = meshGrid(4, 3, 10.0, 10.0);
    const auto runs = ui::warpGridLines(g, 4096);
    const auto boundary = ui::warpBoundaryLines(g, runs.size());
    // rows row lines then cols column lines: the first and last of each.
    CHECK(boundary.size() == 4);
    CHECK(boundary[0] == 0);
    CHECK(boundary[1] == static_cast<std::size_t>(g.rows - 1));
    CHECK(boundary[2] == static_cast<std::size_t>(g.rows));
    CHECK(boundary[3] == static_cast<std::size_t>(g.rows + g.cols - 1));
    const WarpGrid p =
        core::identityWarpGrid(common::Rect{0, 0, 10, 10}, 2, 2, WarpKind::Perspective);
    CHECK(ui::warpBoundaryLines(p, 3).size() == 1); // the closed edge run alone
}

TEST_CASE("thickenPolyline doubles a run either side of its own normal") {
    const std::vector<common::Vec2> line{{0, 0}, {10, 0}, {20, 0}};
    const auto sides = ui::thickenPolyline(line, 0.5);
    REQUIRE(sides.size() == 2);
    REQUIRE(sides[0].size() == line.size());
    // A horizontal run's normal is vertical: one copy each side, x untouched. The offset direction
    // is the left-hand normal n = (-t.y, t.x), so side 0 of a +x run sits at +y -- but WHICH array
    // holds which side is not a contract: both runs are handed to the same overlay lane and both are
    // drawn, so pinning the order here would only freeze an arbitrary choice. What must hold is the
    // geometry: the two sides straddle the original symmetrically, exactly offsetPx apart, and the
    // thickening never leaks into x (an offset applied along the tangent instead of the normal, or a
    // normal built as the tangent, both fail these).
    for (std::size_t i = 0; i < line.size(); ++i) {
        CHECK(sides[0][i].x == doctest::Approx(line[i].x).epsilon(1e-9));
        CHECK(sides[1][i].x == doctest::Approx(line[i].x).epsilon(1e-9));
        CHECK(sides[0][i].y - line[i].y == doctest::Approx(0.5).epsilon(1e-9));
        CHECK(line[i].y - sides[1][i].y == doctest::Approx(0.5).epsilon(1e-9));
    }
    // A degenerate input comes back as itself rather than as two empty runs.
    CHECK(ui::thickenPolyline({{1, 1}}, 0.5).size() == 1);
}

TEST_CASE("the Quality choice's index map round-trips, and matches the Move tool's order") {
    CHECK(ui::warpQualityForChoice(0) == ResampleFilter::Auto);
    CHECK(ui::warpQualityForChoice(1) == ResampleFilter::Nearest);
    CHECK(ui::warpQualityForChoice(6) == ResampleFilter::Lanczos3);
    CHECK(ui::warpQualityForChoice(9) == ResampleFilter::Supersample);
    CHECK(ui::warpQualityForChoice(-1) == ResampleFilter::Auto); // out of range -> Auto
    CHECK(ui::warpQualityForChoice(99) == ResampleFilter::Auto);
    for (int i = 0; i < 10; ++i)
        CHECK(ui::warpQualityChoiceIndex(ui::warpQualityForChoice(i)) == i);
}

// ---- core::SetLayerWarpCommand ---------------------------------------------------------------

TEST_CASE("SetLayerWarpCommand restores pixels, placement AND grid on undo") {
    core::Document doc(32, 32);
    auto layer = std::make_unique<core::RasterLayer>(1, "r", 16, 16);
    layer->image() = tagImage(16, 16);
    layer->setTransform(common::Affine2D::translation(4.0, 6.0));
    const common::Image before = layer->image();
    core::RasterLayer* raster = layer.get();
    doc.root().addOnTop(std::move(layer));

    WarpGrid g = meshGrid(3, 3, 16.0, 16.0);
    g.points[4] = g.points[4] + common::Vec2{3.0, 3.0};
    const render::WarpResult r = render::warpImage(before, g, ResampleFilter::Bilinear);
    REQUIRE(r.ok);
    const common::Affine2D placement =
        common::Affine2D::translation(4.0, 6.0)
        * common::Affine2D::translation(static_cast<double>(r.offX), static_cast<double>(r.offY));

    core::SetLayerWarpCommand cmd(1, r.px, placement, g);
    cmd.apply(doc);
    CHECK(raster->image().rgba == r.px.rgba);
    CHECK(raster->transform() == placement);
    REQUIRE(raster->hasWarp());
    CHECK(*raster->warp() == g);

    cmd.undo(doc);
    CHECK(raster->image().rgba == before.rgba); // byte-exact: undo restores, it does not invert
    CHECK(raster->transform() == common::Affine2D::translation(4.0, 6.0));
    CHECK_FALSE(raster->hasWarp());

    cmd.apply(doc); // redo is the same apply
    CHECK(raster->image().rgba == r.px.rgba);
    REQUIRE(raster->hasWarp());
    CHECK(*raster->warp() == g);
}

TEST_CASE("SetLayerWarpCommand does NOT coalesce: one Apply is one history entry") {
    core::Document doc(8, 8);
    core::SetLayerWarpCommand a(1, common::Image(4, 4), common::Affine2D::identity(), std::nullopt);
    core::SetLayerWarpCommand b(1, common::Image(4, 4), common::Affine2D::identity(), std::nullopt);
    CHECK_FALSE(a.tryMergeWith(b));
    CHECK_FALSE(a.dirtyRegion(doc).has_value()); // the extent AND the placement move: recomposite all
}

TEST_CASE("warpRevision moves on every warp change, mirroring maskRevision") {
    core::RasterLayer l(1, "r", 4, 4);
    CHECK(l.warpRevision() == 0);
    CHECK_FALSE(l.hasWarp());
    l.setWarp(meshGrid(2, 2, 4.0, 4.0));
    CHECK(l.warpRevision() == 1);
    CHECK(l.hasWarp());
    l.bumpWarpRevision(); // an in-place edit through a future mutable accessor
    CHECK(l.warpRevision() == 2);
    l.clearWarp();
    CHECK(l.warpRevision() == 3);
    CHECK_FALSE(l.hasWarp());
}

TEST_CASE("warpKindName spells both kinds") {
    CHECK(core::warpKindName(WarpKind::Mesh) == "Mesh");
    CHECK(core::warpKindName(WarpKind::Perspective) == "Perspective");
}
