#include "common/geometry.hpp"

#include <doctest/doctest.h>

#include <cmath>

using namespace mosaic::common;
using doctest::Approx;

TEST_CASE("Vec2 arithmetic, dot and length") {
    constexpr Vec2 a{3.0, 4.0};
    constexpr Vec2 b{1.0, 2.0};
    CHECK((a + b) == Vec2{4.0, 6.0});
    CHECK((a - b) == Vec2{2.0, 2.0});
    CHECK((a * 2.0) == Vec2{6.0, 8.0});
    CHECK((2.0 * a) == Vec2{6.0, 8.0});
    CHECK(a.dot(b) == Approx(11.0));
    CHECK(a.length() == Approx(5.0));
}

TEST_CASE("Rect union, intersection and containment") {
    constexpr Rect r{0, 0, 10, 10};
    CHECK(r.contains({5, 5}));
    CHECK_FALSE(r.contains({10, 10}));  // right/bottom edges are exclusive

    constexpr Rect s{5, 5, 10, 10};
    CHECK(r.intersects(s));
    CHECK(r.intersected(s) == Rect{5, 5, 5, 5});
    CHECK(r.united(s) == Rect{0, 0, 15, 15});

    // Empty rect is the identity for union and absorbs intersection.
    CHECK(r.united(Rect{}) == r);
    CHECK(Rect{}.united(r) == r);
    CHECK(r.intersected(Rect{20, 20, 5, 5}).empty());

    CHECK(Rect::fromCorners({10, 2}, {2, 10}) == Rect{2, 2, 8, 8});
}

TEST_CASE("Affine2D translate/scale and point vs vector application") {
    const Affine2D t = Affine2D::translation(5, -3);
    CHECK(t.apply({1, 1}) == Vec2{6, -2});
    CHECK(t.applyVector({1, 1}) == Vec2{1, 1});  // translation does not affect directions

    const Affine2D s = Affine2D::scaling(2, 3);
    CHECK(s.apply({4, 5}) == Vec2{8, 15});
}

TEST_CASE("Affine2D rotation by 90 degrees") {
    const Affine2D r = Affine2D::rotation(M_PI / 2.0);
    const Vec2 p = r.apply({1, 0});
    CHECK(p.x == Approx(0.0).epsilon(1e-9));
    CHECK(p.y == Approx(1.0).epsilon(1e-9));
}

TEST_CASE("Affine2D composition order is matrix order") {
    // (A * B).apply(p) == A.apply(B.apply(p)): scale first, then translate.
    const Affine2D scaleThenTranslate = Affine2D::translation(10, 0) * Affine2D::scaling(2, 2);
    CHECK(scaleThenTranslate.apply({3, 4}) == Vec2{16, 8});
}

TEST_CASE("Affine2D inverse round-trips a point") {
    const Affine2D m = Affine2D::trs({30, -10}, 0.7, {2.0, 1.5});
    const auto inv = m.inverse();
    REQUIRE(inv.has_value());
    const Vec2 p{12.5, -4.0};
    const Vec2 back = inv->apply(m.apply(p));
    CHECK(back.x == Approx(p.x));
    CHECK(back.y == Approx(p.y));
}

TEST_CASE("Affine2D reports a singular transform as non-invertible") {
    const Affine2D degenerate = Affine2D::scaling(0.0, 1.0);
    CHECK_FALSE(degenerate.inverse().has_value());
}

TEST_CASE("Affine2D mapBounds covers a rotated rectangle") {
    const Affine2D r = Affine2D::rotation(M_PI / 2.0);
    const Rect b = r.mapBounds({0, 0, 2, 4});
    // A 2x4 box rotated 90 deg occupies a 4x2 area; bounds are axis-aligned.
    CHECK(b.w == Approx(4.0).epsilon(1e-9));
    CHECK(b.h == Approx(2.0).epsilon(1e-9));
}
