// 3D math tests (S30-c, docs/type-tool.md §10): the Vec3/Vec4/Mat4/Quat kit behind the Type
// extrude engine. Pure and deterministic -- exact identities where the math is exact, Approx
// where trig enters.
#include <doctest/doctest.h>

#include <cmath>

#include "common/geometry3d.hpp"

using namespace mosaic::common;

namespace {
constexpr double kPi = 3.14159265358979323846;

bool approxVec(Vec3 a, Vec3 b, double eps = 1e-12) {
    return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps && std::abs(a.z - b.z) < eps;
}
}  // namespace

TEST_CASE("Vec3 algebra: dot, cross handedness, normalization") {
    constexpr Vec3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
    CHECK(x.dot(y) == 0.0);
    CHECK(x.cross(y) == z);  // right-handed: x cross y = z
    CHECK(y.cross(x) == -z);
    CHECK(Vec3{3, 4, 0}.length() == doctest::Approx(5.0));
    CHECK(approxVec(Vec3{0, 0, 7}.normalized(), z));
    CHECK(Vec3{}.normalized() == Vec3{});  // degenerate stays zero (visible failure, no invented dir)
}

TEST_CASE("Mat4 identity, composition order, and point vs direction transforms") {
    const Mat4 t = Mat4::translation({10, 20, 30});
    const Mat4 s = Mat4::scale({2, 2, 2});

    CHECK(Mat4::identity().transformPoint({1, 2, 3}) == Vec3{1, 2, 3});
    // (t * s) applies the scale FIRST: p -> 2p + t.
    CHECK((t * s).transformPoint({1, 1, 1}) == Vec3{12, 22, 32});
    // ...and (s * t) translates first: p -> 2(p + t).
    CHECK((s * t).transformPoint({1, 1, 1}) == Vec3{22, 42, 62});
    // Directions ignore translation.
    CHECK(t.transformDirection({0, 0, 1}) == Vec3{0, 0, 1});
}

TEST_CASE("perspective projection maps the frustum to NDC with Vulkan's [0,1] z") {
    const Mat4 p = Mat4::perspective(kPi / 2.0, 1.0, 1.0, 101.0);  // 90 deg, unit aspect
    // A point straight ahead on the near plane (camera looks down -z): z_ndc = 0, centre of screen.
    const Vec3 nearPt = p.transformPoint({0, 0, -1.0});
    CHECK(nearPt.z == doctest::Approx(0.0));
    // On the far plane: z_ndc = 1.
    const Vec3 farPt = p.transformPoint({0, 0, -101.0});
    CHECK(farPt.z == doctest::Approx(1.0));
    // At 90 deg FOV the frustum edge at depth d sits at |y| = d: y lands on the NDC edge.
    const Vec3 edge = p.transformPoint({0, 10.0, -10.0});
    CHECK(edge.y == doctest::Approx(1.0));
    // Perspective foreshortening: the same lateral offset shrinks with distance.
    const Vec3 near2 = p.transformPoint({2.0, 0, -4.0});
    const Vec3 far2 = p.transformPoint({2.0, 0, -40.0});
    CHECK(std::abs(far2.x) < std::abs(near2.x));
}

TEST_CASE("orthographic projection has no foreshortening and the same z range") {
    const Mat4 o = Mat4::orthographic(10.0, 10.0, 1.0, 101.0);
    CHECK(o.transformPoint({0, 0, -1.0}).z == doctest::Approx(0.0));
    CHECK(o.transformPoint({0, 0, -101.0}).z == doctest::Approx(1.0));
    const Vec3 a = o.transformPoint({5.0, 0, -2.0});
    const Vec3 b = o.transformPoint({5.0, 0, -90.0});
    CHECK(a.x == doctest::Approx(b.x));  // depth does not move x
    CHECK(a.x == doctest::Approx(0.5));  // half of the half-extent -> NDC 0.5
}

TEST_CASE("Quat axis-angle rotation, composition order, and Mat4 agreement") {
    // 90 deg about +z takes +x to +y (right-handed).
    const Quat qz = Quat::fromAxisAngle({0, 0, 1}, kPi / 2.0);
    CHECK(approxVec(qz.rotate({1, 0, 0}), {0, 1, 0}));
    // The matrix form agrees with the direct rotation.
    CHECK(approxVec(qz.toMat4().transformPoint({1, 0, 0}), qz.rotate({1, 0, 0})));

    // (a * b) rotates by b first: 90 deg z then 90 deg x takes +x -> +y -> +z.
    const Quat qx = Quat::fromAxisAngle({1, 0, 0}, kPi / 2.0);
    CHECK(approxVec((qx * qz).rotate({1, 0, 0}), {0, 0, 1}, 1e-12));

    // Unit by construction; conjugate inverts.
    CHECK(qz.length() == doctest::Approx(1.0));
    CHECK(approxVec(qz.conjugate().rotate(qz.rotate({1, 2, 3})), {1, 2, 3}));

    // A full turn is the identity rotation (up to quaternion double-cover).
    const Quat full = Quat::fromAxisAngle({0, 1, 0}, 2.0 * kPi);
    CHECK(approxVec(full.rotate({1, 2, 3}), {1, 2, 3}));
}

TEST_CASE("Quat renormalization holds a long compose chain together") {
    // Simulate a drag loop: many small orbit increments, renormalized as the gizmo will.
    Quat q = Quat::identity();
    const Quat step = Quat::fromAxisAngle({0.3, 1.0, 0.2}, 0.01);
    for (int i = 0; i < 10000; ++i) q = (step * q).normalized();
    CHECK(q.length() == doctest::Approx(1.0));
    // Still a pure rotation: lengths are preserved.
    CHECK(q.rotate({0, 0, 5}).length() == doctest::Approx(5.0));
}
