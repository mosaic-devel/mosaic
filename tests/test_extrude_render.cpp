// CPU 3D render-lane tests (S30-c, docs/type-tool.md §10.3): the software z-buffer rasterizer on
// hand-built solids (deterministic, no fonts) plus one fonts-gated end-to-end renderTextF pass.
// Asserts follow the render-test lesson: measure INK (coverage, colour, luminance), not just math.
#include "common/geometry3d.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/layer_effects.hpp"
#include "core/text/extrude_mesh.hpp"
#include "core/text/extrude_overlay.hpp"
#include "core/text/extrude_render.hpp"
#include "core/text/shaping.hpp"
#include "core/text/text_layer_render.hpp"
#include "core/text/text_model.hpp"
#include "core/text/text_render.hpp"
#include "platform/font_db.hpp"

#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>

using namespace mosaic::core::text;
namespace vec = mosaic::core::vec;
using mosaic::common::Affine2D;
using mosaic::common::ColorF;
using mosaic::common::ImageF;
using mosaic::common::Quat;
using mosaic::common::Vec2;

namespace {

constexpr double kPi = 3.14159265358979323846;

GlyphSolidInput square10() {
    vec::Contour c;
    c.points = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    c.closed = true;
    return {{c}, 0};
}

struct Ink {
    int count = 0;
    double minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
    ColorF at{};  // last opaque pixel's colour
    double lum = 0.0;  // mean luminance of opaque pixels
};
Ink scan(const ImageF& img) {
    Ink s;
    double lum = 0.0;
    for (std::uint32_t y = 0; y < img.height; ++y)
        for (std::uint32_t x = 0; x < img.width; ++x) {
            const ColorF c = img.at(x, y);
            if (c.a < 0.5f) continue;
            ++s.count;
            s.minX = std::min(s.minX, double(x));
            s.minY = std::min(s.minY, double(y));
            s.maxX = std::max(s.maxX, double(x));
            s.maxY = std::max(s.maxY, double(y));
            s.at = c;
            lum += 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
        }
    if (s.count > 0) s.lum = lum / s.count;
    return s;
}

Extrude flatRed(float perspective = 0.0f) {
    Extrude e;
    e.depth = 20.0f;
    e.perspective = perspective;   // 0 = true ortho
    e.lightingEnabled = false;     // flat self-lit: exact colour asserts
    e.material.albedo = {1.0f, 0.0f, 0.0f, 1.0f};
    return e;
}

ImageF renderSquare(const Extrude& e) {
    const ExtrudeMesh mesh = buildExtrudeMesh({square10()}, e);
    ImageF img(40, 40);
    renderExtrudeMeshF(img, mesh, e, Affine2D::translation(15.0, 15.0), /*antialias=*/true);
    return img;
}

}  // namespace

TEST_CASE("face-on ortho solid inks exactly its face, in the exact flat colour") {
    const Extrude e = flatRed();
    const Ink ink = scan(renderSquare(e));
    // The silhouette is the 10x10 face translated to (15,15) -- walls hide exactly behind it.
    CHECK(ink.count > 80);
    CHECK(ink.count < 125);
    CHECK(ink.minX == doctest::Approx(15.0).epsilon(0.1));
    CHECK(ink.maxX == doctest::Approx(24.0).epsilon(0.1));
    CHECK(ink.at.r == doctest::Approx(1.0f));  // flat albedo, no shading applied
    CHECK(ink.at.g == doctest::Approx(0.0f));
    // Deterministic: a second render is identical.
    const ImageF again = renderSquare(e);
    const ImageF first = renderSquare(e);
    CHECK(first.rgba == again.rgba);
}

TEST_CASE("perspective grows the near face; ortho does not") {
    const Ink ortho = scan(renderSquare(flatRed(0.0f)));
    const Ink persp = scan(renderSquare(flatRed(30.0f)));
    // The front cap sits at +depth/2, nearer the eye than the scale-true z=0 plane.
    CHECK(persp.count > ortho.count);
    // Both stay centred on the same spot (the pivot projects scale-true).
    const double oc = (ortho.minX + ortho.maxX) * 0.5;
    const double pc = (persp.minX + persp.maxX) * 0.5;
    CHECK(oc == doctest::Approx(pc).epsilon(0.05));
}

TEST_CASE("a quarter turn about Y shows the solid's depth as its width") {
    Extrude e = flatRed();
    e.orientation = Quat::fromAxisAngle({0.0, 1.0, 0.0}, kPi / 2.0);
    const Ink ink = scan(renderSquare(e));
    // Edge-on: the silhouette's width is now the 20px DEPTH, its height still 10px.
    CHECK(ink.maxX - ink.minX == doctest::Approx(20.0).epsilon(0.08));
    CHECK(ink.maxY - ink.minY == doctest::Approx(10.0).epsilon(0.12));
}

TEST_CASE("lighting responds to the light direction (front lit vs back lit)") {
    Extrude lit = flatRed();
    lit.lightingEnabled = true;
    lit.material.albedo = {0.8f, 0.8f, 0.8f, 1.0f};
    lit.ambient = {0.15f, 0.15f, 0.15f, 1.0f};
    lit.lights = {Light{{0.0, 0.0, -1.0}, {1, 1, 1, 1}, 1.0f}};  // travels INTO the screen: front-lit
    const Ink front = scan(renderSquare(lit));

    lit.lights[0].direction = {0.0, 0.0, 1.0};  // travels toward the viewer: the face sees ambient only
    const Ink back = scan(renderSquare(lit));

    CHECK(front.lum > back.lum * 2.0);
    // And the lighting toggle actually gates all of it.
    lit.lightingEnabled = false;
    const Ink flat = scan(renderSquare(lit));
    CHECK(flat.at.r == doctest::Approx(0.8f));
}

TEST_CASE("metalness tints and roughness spreads the highlight") {
    Extrude e = flatRed();
    e.lightingEnabled = true;
    e.material.albedo = {0.9f, 0.5f, 0.1f, 1.0f};
    e.material.roughness = 0.15f;
    // Tilt slightly so the face catches an off-axis highlight.
    e.orientation = Quat::fromAxisAngle({1.0, 0.0, 0.0}, 0.3);
    e.lights = {Light{{0.4, 0.4, -0.8}, {1, 1, 1, 1}, 1.0f}};

    e.material.metalness = 0.0f;
    const Ink dielectric = scan(renderSquare(e));
    e.material.metalness = 1.0f;
    const Ink metal = scan(renderSquare(e));
    // A metal loses its diffuse body (kd ~ 0.15) -- overall darker except in the highlight.
    CHECK(metal.lum < dielectric.lum);
}

TEST_CASE("a metal mirrors the studio: brighter sky-facing than floor-facing") {
    // Tip the solid back so its front cap reflects the studio SKY, then forward for the FLOOR;
    // the zenith-vs-floor gradient must show up in the shaded luminance (the chrome fix).
    Extrude e;
    e.depth = 12.0f;
    e.perspective = 0.0f;
    e.lightingEnabled = true;
    e.material.albedo = {0.9f, 0.9f, 0.9f, 1.0f};
    e.material.metalness = 1.0f;
    e.material.roughness = 0.0f;
    e.lights.clear();  // isolate the environment term
    e.orientation = Quat::fromAxisAngle({1.0, 0.0, 0.0}, 0.9);  // face reflects downward = floor
    const Ink floorSide = scan(renderSquare(e));
    e.orientation = Quat::fromAxisAngle({1.0, 0.0, 0.0}, -0.9);  // face reflects upward = sky
    const Ink skySide = scan(renderSquare(e));
    // The scan averages every visible face (walls + cap), so the asymmetry is diluted -- assert
    // the direction with a real margin, not the raw zenith/floor ratio.
    CHECK(skySide.lum > floorSide.lum * 1.2);
}

TEST_CASE("reflectCanvas mirrors the snapshot; transparent snapshot falls through to the studio") {
    Extrude e;
    e.depth = 12.0f;
    e.perspective = 0.0f;
    e.lightingEnabled = true;
    e.material.albedo = {0.9f, 0.9f, 0.9f, 1.0f};
    e.material.metalness = 1.0f;
    e.material.roughness = 0.0f;
    e.lights.clear();
    // Tip PAST 45 degrees so the front cap's reflected rays head behind the solid (R.z < 0) and
    // hit the back-cap canvas plane.
    e.orientation = Quat::fromAxisAngle({1.0, 0.0, 0.0}, 0.9);
    e.reflectCanvas = true;
    const ExtrudeMesh mesh = buildExtrudeMesh({square10()}, e);

    ImageF red(8, 8);
    for (std::size_t i = 0; i < red.rgba.size(); i += 4) {
        red.rgba[i + 0] = 1.0f;
        red.rgba[i + 3] = 1.0f;
    }
    const ExtrudeEnv env{&red, Affine2D::scaling(0.2, 0.2)};  // everything lands in the red image

    ImageF with(40, 40), without(40, 40);
    renderExtrudeMeshF(with, mesh, e, Affine2D::translation(15.0, 15.0), true, &env);
    Extrude off = e;
    off.reflectCanvas = false;
    renderExtrudeMeshF(without, mesh, off, Affine2D::translation(15.0, 15.0), true, &env);
    // The mirror is grey without the snapshot and red-tinted with it.
    double biasWith = 0.0, biasWithout = 0.0;
    for (std::uint32_t y = 0; y < 40; ++y)
        for (std::uint32_t x = 0; x < 40; ++x) {
            const ColorF a = with.at(x, y), b = without.at(x, y);
            if (a.a > 0.5f) biasWith += a.r - a.g;
            if (b.a > 0.5f) biasWithout += b.r - b.g;
        }
    CHECK(biasWith > biasWithout + 1.0);

    // A fully transparent snapshot behaves exactly like no snapshot (studio fall-through).
    ImageF clear(8, 8);  // zero-initialized: alpha 0
    const ExtrudeEnv clearEnv{&clear, Affine2D::scaling(0.2, 0.2)};
    ImageF viaClear(40, 40);
    renderExtrudeMeshF(viaClear, mesh, e, Affine2D::translation(15.0, 15.0), true, &clearEnv);
    CHECK(viaClear.rgba == without.rgba);

    // reflectSidesOnly. Ortho geometry: a surface's reflected ray heads behind (into the canvas)
    // iff its normal sits >45 degrees off the view axis. At the 0.9 rad tilt above (>45), the CAP
    // mirrors and the walls do not -- so sides-only must be byte-identical to reflect-off here.
    Extrude sides = e;
    sides.reflectSidesOnly = true;
    ImageF viaSides(40, 40);
    renderExtrudeMeshF(viaSides, mesh, sides, Affine2D::translation(15.0, 15.0), true, &env);
    CHECK(viaSides.rgba == without.rgba);

    // And at 0.6 rad (<45) the roles swap: only the WALL mirrors, so sides-only is byte-identical
    // to the full reflect (the cap skip is a no-op) while both differ from reflect-off.
    Extrude shallow = e;
    shallow.orientation = Quat::fromAxisAngle({1.0, 0.0, 0.0}, 0.6);
    const ExtrudeMesh meshShallow = buildExtrudeMesh({square10()}, shallow);
    Extrude shallowSides = shallow;
    shallowSides.reflectSidesOnly = true;
    Extrude shallowOff = shallow;
    shallowOff.reflectCanvas = false;
    ImageF full06(40, 40), sides06(40, 40), off06(40, 40);
    renderExtrudeMeshF(full06, meshShallow, shallow, Affine2D::translation(15.0, 15.0), true, &env);
    renderExtrudeMeshF(sides06, meshShallow, shallowSides, Affine2D::translation(15.0, 15.0), true,
                       &env);
    renderExtrudeMeshF(off06, meshShallow, shallowOff, Affine2D::translation(15.0, 15.0), true,
                       &env);
    CHECK(sides06.rgba == full06.rgba);
    CHECK(sides06.rgba != off06.rgba);  // the wall genuinely mirrors
}

TEST_CASE("ExtrudePlaneMap: identity at rest (ortho); project/unproject round-trips rotated") {
    const mosaic::common::Rect bounds{10.0, 20.0, 100.0, 40.0};
    {
        Extrude e;
        e.perspective = 0.0f;  // true ortho
        e.depth = 24.0f;
        const auto m = ExtrudePlaneMap::from(bounds, e);
        const Vec2 q = m.project({37.0, 42.0});  // scale-true: editing chrome must not move
        CHECK(q.x == doctest::Approx(37.0));
        CHECK(q.y == doctest::Approx(42.0));
    }
    Extrude e;
    e.perspective = 25.0f;
    e.depth = 30.0f;
    e.orientation = Quat::fromAxisAngle({0.4, 0.8, 0.2}, 0.8);
    const auto m = ExtrudePlaneMap::from(bounds, e);
    for (const Vec2 p : {Vec2{10.0, 20.0}, Vec2{110.0, 60.0}, Vec2{60.0, 40.0}, Vec2{25.0, 55.0}}) {
        const auto back = m.unproject(m.project(p));
        REQUIRE(back.has_value());
        CHECK(back->x == doctest::Approx(p.x));
        CHECK(back->y == doctest::Approx(p.y));
    }
    e.perspective = 0.0f;  // the ortho ray path round-trips too
    const auto mo = ExtrudePlaneMap::from(bounds, e);
    const auto back = mo.unproject(mo.project({80.0, 30.0}));
    REQUIRE(back.has_value());
    CHECK(back->x == doctest::Approx(80.0));
    CHECK(back->y == doctest::Approx(30.0));
}

TEST_CASE("projectedExtrudeBounds contains every inked pixel") {
    Extrude e = flatRed(25.0f);
    e.orientation = Quat::fromAxisAngle({0.6, 0.7, 0.2}, 0.9);
    e.bevelFront.size = 1.5f;
    const ExtrudeMesh mesh = buildExtrudeMesh({square10()}, e);
    ImageF img(80, 80);
    renderExtrudeMeshF(img, mesh, e, Affine2D::translation(35.0, 35.0), true);
    const Ink ink = scan(img);
    REQUIRE(ink.count > 0);

    const mosaic::common::Rect b = projectedExtrudeBounds({0, 0, 10, 10}, e);
    CHECK(ink.minX >= b.x + 35.0 - 1.5);  // half-pixel + AA slack
    CHECK(ink.minY >= b.y + 35.0 - 1.5);
    CHECK(ink.maxX <= b.right() + 35.0 + 1.5);
    CHECK(ink.maxY <= b.bottom() + 35.0 + 1.5);
}

// ---------------------------------------------------------------------------------------------
// S30-e (docs/type-tool.md §12): Layer-Effects overlays mapped per face in glyph design space.
// ---------------------------------------------------------------------------------------------

TEST_CASE("overlay maps bake in design space: gradient direction, blend opacity, dedupe") {
    Extrude e = flatRed();
    const ExtrudeMesh mesh = buildExtrudeMesh({square10()}, e);
    REQUIRE_FALSE(mesh.empty());

    // Inactive stacks build nothing: disabled, and enabled-but-NoPaint (a z-order placeholder).
    mosaic::core::LayerEffects none;
    CHECK_FALSE(extrudeOverlaysActive(none));
    none.gradientOverlay.enabled = true;  // paint stays NoPaint
    CHECK_FALSE(extrudeOverlaysActive(none));
    CHECK(buildExtrudeOverlay(none, mesh, e, mesh.designBounds, 1.0, true).empty());

    // A linear left->right gradient (identity transform = across the normalised domain).
    mosaic::core::LayerEffects fx;
    fx.gradientOverlay.enabled = true;
    vec::Gradient g;
    g.stops = {{0.0, ColorF{1, 0, 0, 1}}, {1.0, ColorF{0, 0, 1, 1}}};
    fx.gradientOverlay.paint = g;
    const ExtrudeOverlay ov = buildExtrudeOverlay(fx, mesh, e, mesh.designBounds, 4.0, true);
    REQUIRE(ov.maps.size() == 1);
    REQUIRE(ov.mapForRun(0) == &ov.maps[0]);
    const ImageF& map = ov.maps[0];
    REQUIRE(map.width >= 8);  // 10 design units * scale 4 = 40 texels
    const ColorF left = map.at(0, map.height / 2);
    const ColorF right = map.at(map.width - 1, map.height / 2);
    CHECK(left.r > 0.8f);   // red end
    CHECK(left.b < 0.2f);
    CHECK(right.b > 0.8f);  // blue end
    CHECK(right.r < 0.2f);

    // Blend + opacity: a half-opacity green colour overlay over the red albedo = (0.5, 0.5, 0).
    mosaic::core::LayerEffects half;
    half.colorOverlay.enabled = true;
    half.colorOverlay.paint = vec::SolidPaint{ColorF{0, 1, 0, 1}};
    half.colorOverlay.opacity = 0.5f;
    const ExtrudeOverlay hov = buildExtrudeOverlay(half, mesh, e, mesh.designBounds, 1.0, true);
    REQUIRE(hov.maps.size() == 1);
    const ColorF mid = hov.maps[0].at(hov.maps[0].width / 2, hov.maps[0].height / 2);
    CHECK(mid.r == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(mid.g == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(mid.b == doctest::Approx(0.0f).epsilon(0.01));

    // Per-run materials: distinct albedos get distinct maps; equal albedos share one.
    GlyphSolidInput second = square10();
    second.runIndex = 1;
    GlyphSolidInput third = square10();
    third.runIndex = 2;
    Extrude multi = e;
    multi.runMaterials[1] = Material{{0.0f, 0.0f, 1.0f, 1.0f}, 0.0f, 0.5f};  // blue override
    multi.runMaterials[2] = multi.material;  // same as the default red -> shares its map
    const ExtrudeMesh meshMulti = buildExtrudeMesh({square10(), second, third}, multi);
    const ExtrudeOverlay mov =
        buildExtrudeOverlay(half, meshMulti, multi, meshMulti.designBounds, 1.0, true);
    CHECK(mov.maps.size() == 2);
    CHECK(mov.mapForRun(0) == mov.mapForRun(2));
    CHECK(mov.mapForRun(0) != mov.mapForRun(1));
}

TEST_CASE("the mesh rasteriser is banded by SCANLINE, so it stays deterministic and gapless") {
    // The fragment loop resolves every sample against a shared z-buffer, so it is split by ROW and
    // not by triangle: a texel belongs to exactly one band, and each band walks the ranges and
    // triangles in the same order the serial loop did, so its z-test resolves identically. Split by
    // triangle instead and two threads would race for the same texel with the winner decided by
    // timing.
    //
    // Rendered big enough to actually cross band boundaries -- parallelFor runs inline below its
    // minimum per band, so a small render would exercise the serial path and prove nothing.
    Extrude e = flatRed();
    e.depth = 24.0f;
    e.bevelFront.size = 2.0f;  // a bevel puts many small triangles near the silhouette, which is
    e.bevelFront.segments = 3; // where a band boundary is most likely to drop one
    e.orientation = Quat::fromAxisAngle({0.3, 1.0, 0.2}, 0.5); // off-axis: triangles span rows
    const ExtrudeMesh mesh = buildExtrudeMesh({square10()}, e);
    REQUIRE_FALSE(mesh.empty());

    constexpr std::uint32_t kW = 420, kH = 380;
    const auto render = [&] {
        ImageF img(kW, kH);
        renderExtrudeMeshF(img, mesh, e,
                           Affine2D::translation(60.0, 40.0) * Affine2D::scaling(28.0, 28.0),
                           /*antialias=*/true);
        return img;
    };
    const ImageF a = render();
    const ImageF b = render();
    CHECK(a.rgba == b.rgba); // a race in the z-buffer split shows up here and nowhere else

    // The solid must actually be large enough to have crossed bands, or the equality above is
    // comparing two serial renders.
    const Ink ink = scan(a);
    REQUIRE(ink.count > 2000);
    REQUIRE(ink.maxY - ink.minY > 100.0);

    // ... and every row inside the silhouette must still reach FULL coverage somewhere. A band that
    // failed to shade its first row leaves a stripe across the solid, which determinism cannot see
    // (two renders drop the same row) -- and which a "does this row have any coverage at all" test
    // cannot see either: 2x2 supersampling averages the missing row with its surviving twin, so a
    // dropped row reads as alpha 0.5, not 0. The peak is what moves.
    for (long y = static_cast<long>(ink.minY) + 1; y < static_cast<long>(ink.maxY); ++y) {
        float peak = 0.0f;
        for (std::uint32_t x = 0; x < kW; ++x)
            peak = std::max(peak, a.at(x, static_cast<std::uint32_t>(y)).a);
        INFO("row y=" << y << " peaks at alpha " << peak << ", inside [" << ink.minY << ", "
                      << ink.maxY << "]");
        CHECK(peak > 0.9f);
    }
}

TEST_CASE(
    "a mixed solid + gradient overlay stack bakes deterministically and varies in both axes") {
    // The map bake is banded over rows and resolves a SOLID overlay to one constant instead of
    // asking per texel. Two things that can break:
    //
    //   * the constant attaching to the wrong slot of the stack -- which only shows when a solid
    //     and a non-solid are BOTH present, so both are here, in that z-order;
    //   * a row band reading or writing outside its own rows, which is a data race and therefore
    //     shows as NON-DETERMINISM rather than as a wrong picture. Baking twice and requiring the
    //     two byte-for-byte is the direct pin on that.
    Extrude e = flatRed();     // lighting off: the shaded colour IS the (overlay) albedo
    e.overlayWrapSides = true; // ... and bake the wall map too, which is the second banded loop
    const ExtrudeMesh mesh = buildExtrudeMesh({square10()}, e);
    REQUIRE_FALSE(mesh.empty());

    mosaic::core::LayerEffects fx;
    fx.colorOverlay.enabled = true; // under...
    fx.colorOverlay.paint = vec::SolidPaint{ColorF{0, 1, 0, 1}};
    vec::Gradient g; // ... a radial, whose parameter is sqrt(u^2 + v^2): it varies in BOTH axes,
    g.type = vec::GradientType::Radial; // which a purely horizontal gradient would not, and a row
    g.stops = {{0.0, ColorF{1, 0, 0, 1}}, {1.0, ColorF{0, 0, 1, 1}}}; // band defect hides in y.
    fx.gradientOverlay.enabled = true;
    fx.gradientOverlay.paint = g;
    fx.gradientOverlay.opacity = 0.5f; // half, so the solid beneath it still shows through

    const ExtrudeOverlay a = buildExtrudeOverlay(fx, mesh, e, mesh.designBounds, 8.0, true);
    const ExtrudeOverlay b = buildExtrudeOverlay(fx, mesh, e, mesh.designBounds, 8.0, true);
    REQUIRE(a.maps.size() == 1);
    REQUIRE(b.maps.size() == 1);
    REQUIRE(a.wallMaps.size() == 1);
    REQUIRE(b.wallMaps.size() == 1);
    CHECK(a.maps[0].rgba == b.maps[0].rgba); // deterministic under banding
    CHECK(a.wallMaps[0].rgba == b.wallMaps[0].rgba);

    const ImageF& map = a.maps[0];
    REQUIRE(map.width >= 8);
    REQUIRE(map.height >= 8);
    // EVERY texel is written, and the bake's own contract says how: the stack composites over the
    // opaque albedo, so alpha stays exactly 1. A row a band failed to visit keeps the image's
    // calloc'd zero and shows up here -- which determinism alone cannot catch, since two bakes
    // skip the same rows.
    const auto everyTexelWritten = [](const ImageF& m) {
        for (std::uint32_t y = 0; y < m.height; ++y)
            for (std::uint32_t x = 0; x < m.width; ++x)
                if (m.at(x, y).a != 1.0f) {
                    INFO("unwritten texel at " << x << "," << y);
                    return false;
                }
        return true;
    };
    CHECK(everyTexelWritten(map));
    CHECK(everyTexelWritten(a.wallMaps[0]));
    // Varies along x AND along y -- so the equality above is not comparing two flat maps, and a
    // band that dropped its rows would have to reproduce the variation to pass.
    CHECK(map.at(0, 0).b != map.at(map.width - 1, 0).b);
    CHECK(map.at(0, 0).b != map.at(0, map.height - 1).b);

    // The SOLID under the gradient is really in the result: drop it and the map must change.
    mosaic::core::LayerEffects gradOnly;
    gradOnly.gradientOverlay = fx.gradientOverlay;
    const ExtrudeOverlay go = buildExtrudeOverlay(gradOnly, mesh, e, mesh.designBounds, 8.0, true);
    REQUIRE(go.maps.size() == 1);
    REQUIRE(go.maps[0].rgba.size() == map.rgba.size());
    CHECK(go.maps[0].rgba != map.rgba);
    // ... and its contribution is GREEN, on a red albedo, so the green channel has to rise.
    CHECK(map.at(map.width / 2, map.height / 2).g > go.maps[0].at(map.width / 2, map.height / 2).g);
}

TEST_CASE("the front face carries the overlay; walls join only in wrap mode; the back cap never") {
    Extrude e = flatRed();  // lighting off: the shaded colour IS the (overlay) albedo
    const ExtrudeMesh mesh = buildExtrudeMesh({square10()}, e);
    mosaic::core::LayerEffects fx;
    fx.colorOverlay.enabled = true;
    fx.colorOverlay.paint = vec::SolidPaint{ColorF{0, 1, 0, 1}};  // green design on a red solid

    const auto render = [&](const Extrude& params, const ExtrudeOverlay* ov) {
        ImageF img(60, 60);
        renderExtrudeMeshF(img, mesh, params, Affine2D::translation(25.0, 25.0), true, nullptr,
                           ov);
        return img;
    };

    // Face-on ortho: the whole silhouette IS the front cap -- every inked pixel turns green.
    const ExtrudeOverlay ov = buildExtrudeOverlay(fx, mesh, e, mesh.designBounds, 1.0, true);
    const Ink front = scan(render(e, &ov));
    REQUIRE(front.count > 0);
    CHECK(front.at.g == doctest::Approx(1.0f));
    CHECK(front.at.r == doctest::Approx(0.0f));

    // A quarter turn about Y shows ONLY walls: without wrap they keep the red material
    // (byte-identical to no overlay at all); with wrap they take the design.
    Extrude side = e;
    side.orientation = Quat::fromAxisAngle({0.0, 1.0, 0.0}, kPi / 2.0);
    const ExtrudeMesh meshSide = buildExtrudeMesh({square10()}, side);
    const auto renderSide = [&](const Extrude& params, const ExtrudeOverlay* o) {
        ImageF img(60, 60);
        renderExtrudeMeshF(img, meshSide, params, Affine2D::translation(25.0, 25.0), true,
                           nullptr, o);
        return img;
    };
    const ExtrudeOverlay ovSide =
        buildExtrudeOverlay(fx, meshSide, side, meshSide.designBounds, 1.0, true);
    const ImageF plainSide = renderSide(side, nullptr);
    CHECK(renderSide(side, &ovSide).rgba == plainSide.rgba);
    Extrude wrap = side;
    wrap.overlayWrapSides = true;
    const ExtrudeOverlay ovWrap =
        buildExtrudeOverlay(fx, meshSide, wrap, meshSide.designBounds, 1.0, true);
    const Ink wrapped = scan(renderSide(wrap, &ovWrap));
    REQUIRE(wrapped.count > 0);
    CHECK(wrapped.at.g == doctest::Approx(1.0f));

    // A half turn shows the BACK cap: never overlaid, wrap or not. Not byte-exact: at exactly pi
    // both caps project onto the same silhouette and the hidden front cap can win a z-TIE sliver
    // along shared edges -- assert the picture instead (no green body, same as no overlay at all,
    // to a sliver of edge pixels).
    Extrude back = e;
    back.orientation = Quat::fromAxisAngle({0.0, 1.0, 0.0}, kPi);
    const ExtrudeMesh meshBack = buildExtrudeMesh({square10()}, back);
    const ExtrudeOverlay ovBack =
        buildExtrudeOverlay(fx, meshBack, back, meshBack.designBounds, 1.0, true);
    ImageF plainBack(60, 60), overlaidBack(60, 60);
    renderExtrudeMeshF(plainBack, meshBack, back, Affine2D::translation(25.0, 25.0), true);
    renderExtrudeMeshF(overlaidBack, meshBack, back, Affine2D::translation(25.0, 25.0), true,
                       nullptr, &ovBack);
    const Ink backInk = scan(overlaidBack);
    REQUIRE(backInk.count > 0);
    int differing = 0;
    for (std::size_t i = 0; i < plainBack.rgba.size(); ++i)
        if (std::abs(plainBack.rgba[i] - overlaidBack.rgba[i]) > 1.0f / 255.0f) {
            ++differing;
            i |= 3;  // count a pixel once: skip to its last channel
        }
    CHECK(differing <= backInk.count / 20 + 4);
    int greenBody = 0;
    for (std::uint32_t y = 0; y < overlaidBack.height; ++y)
        for (std::uint32_t x = 0; x < overlaidBack.width; ++x) {
            const ColorF c = overlaidBack.at(x, y);
            if (c.a > 0.9f && c.g > 0.6f && c.r < 0.4f) ++greenBody;
        }
    CHECK(greenBody <= backInk.count / 20 + 4);

    // ...but WRAP MODE paints the whole solid: the back cap takes the design too (feedback
    // 2026-07-16: "the backside is ignored completely").
    Extrude backWrap = back;
    backWrap.overlayWrapSides = true;
    const ExtrudeOverlay ovBackWrap =
        buildExtrudeOverlay(fx, meshBack, backWrap, meshBack.designBounds, 1.0, true);
    ImageF wrappedBack(60, 60);
    renderExtrudeMeshF(wrappedBack, meshBack, backWrap, Affine2D::translation(25.0, 25.0), true,
                       nullptr, &ovBackWrap);
    const Ink wrappedBackInk = scan(wrappedBack);
    REQUIRE(wrappedBackInk.count > 0);
    CHECK(wrappedBackInk.at.g == doctest::Approx(1.0f));
    CHECK(wrappedBackInk.at.r == doctest::Approx(0.0f));
}

TEST_CASE("wrap mode tiles a pattern UNDISTORTED around the walls (no depth stretching)") {
    // A quarter turn about Y shows only walls, with screen-x = the solid's DEPTH axis. The old
    // mapping sampled the flat design UV there -- constant along depth, so any pattern stretched
    // into ruler lines (user 2026-07-16). The unrolled wall map varies along BOTH axes.
    Extrude e = flatRed();
    e.depth = 24.0f;  // room for a few pattern periods down the depth
    e.overlayWrapSides = true;
    e.orientation = Quat::fromAxisAngle({0.0, 1.0, 0.0}, kPi / 2.0);
    const ExtrudeMesh mesh = buildExtrudeMesh({square10()}, e);
    REQUIRE(mesh.sideLength == doctest::Approx(40.0));  // the 10x10 square's perimeter
    REQUIRE_FALSE(mesh.sideStations.empty());

    mosaic::core::LayerEffects fx;
    fx.patternOverlay.enabled = true;
    vec::ProceduralPattern pp;
    pp.kind = vec::ProceduralPattern::Kind::Checker;
    pp.fg = {1.0f, 1.0f, 1.0f, 1.0f};
    pp.bg = {0.0f, 0.0f, 0.0f, 1.0f};
    pp.scale = 4.0f;  // 4-design-px cells: several flips across the 24px depth
    fx.patternOverlay.paint = vec::Pattern{pp};

    const ExtrudeOverlay ov = buildExtrudeOverlay(fx, mesh, e, mesh.designBounds, 2.0, true);
    REQUIRE_FALSE(ov.wallMaps.empty());
    ImageF img(70, 70);
    renderExtrudeMeshF(img, mesh, e, Affine2D::translation(30.0, 30.0), true, nullptr, &ov);
    const Ink ink = scan(img);
    REQUIRE(ink.count > 0);
    // March along the DEPTH axis (screen-x) across the wall's midline: the checker must flip.
    const std::uint32_t midY = static_cast<std::uint32_t>((ink.minY + ink.maxY) * 0.5);
    int flips = 0;
    float prev = -1.0f;
    for (std::uint32_t x = static_cast<std::uint32_t>(ink.minX) + 2;
         x <= static_cast<std::uint32_t>(ink.maxX) - 2; ++x) {
        const ColorF c = img.at(x, midY);
        if (c.a < 0.9f) continue;
        const float v = c.g > 0.5f ? 1.0f : 0.0f;  // white vs black cell
        if (prev >= 0.0f && v != prev) ++flips;
        prev = v;
    }
    CHECK(flips >= 2);  // the old flat-UV mapping gave ZERO variation along depth
}

TEST_CASE("renderTextF bakes layer overlays onto an extruded block; flat text ignores them") {
    mosaic::platform::FontDB db;
    if (db.families().empty()) return;
    FontRef probe;
    probe.family = db.defaultFamily();
    if (!db.resolve(probe)) return;
    TextShaper shaper;

    CharStyle st;
    st.font.family = db.defaultFamily();
    st.sizePx = 48.0f;
    st.setSolidFill({0, 0, 0, 1});
    TextBlock flat = makeBlock("Hi", st);
    TextBlock solid = flat;
    solid.extrude = Extrude{};
    solid.extrude->lightingEnabled = false;
    solid.extrude->material.albedo = {1.0f, 0.0f, 0.0f, 1.0f};

    mosaic::core::LayerEffects fx;
    fx.colorOverlay.enabled = true;
    fx.colorOverlay.paint = vec::SolidPaint{ColorF{0, 1, 0, 1}};

    const Affine2D place = Affine2D::translation(20, 60);
    const ImageF plain = renderTextF(shaper, solid, db, 300, 120, place, 0.25, nullptr, nullptr);
    const ImageF overlaid = renderTextF(shaper, solid, db, 300, 120, place, 0.25, nullptr, &fx);
    const Ink pi = scan(plain);
    const Ink oi = scan(overlaid);
    REQUIRE(pi.count > 0);
    REQUIRE(oi.count > 0);
    CHECK(pi.at.r == doctest::Approx(1.0f));  // the bare solid is the red material
    CHECK(oi.at.g == doctest::Approx(1.0f));  // the overlaid face reads the green design
    CHECK(oi.at.r == doctest::Approx(0.0f));

    // Flat text never consumes effects here -- its overlays are the 2D effect pass's business.
    const ImageF flatPlain = renderTextF(shaper, flat, db, 300, 120, place);
    const ImageF flatWithFx = renderTextF(shaper, flat, db, 300, 120, place, 0.25, nullptr, &fx);
    CHECK(flatWithFx.rgba == flatPlain.rgba);
}

TEST_CASE("an effects edit re-renders an extruded block's pixel cache (and only then)") {
    mosaic::platform::FontDB db;
    if (db.families().empty()) return;
    FontRef probe;
    probe.family = db.defaultFamily();
    if (!db.resolve(probe)) return;
    TextShaper shaper;

    mosaic::core::Document doc(240, 120);
    auto* tl = doc.root().addOnTop(doc.makeText("T")).as<mosaic::core::TextLayer>();
    REQUIRE(tl != nullptr);
    CharStyle st;
    st.font.family = db.defaultFamily();
    st.sizePx = 40.0f;
    st.setSolidFill({0, 0, 0, 1});
    TextBlock solid = makeBlock("Hi", st);
    solid.extrude = Extrude{};
    solid.extrude->lightingEnabled = false;
    solid.extrude->material.albedo = {1.0f, 0.0f, 0.0f, 1.0f};
    tl->setBlock(solid);

    CHECK(refreshTextCache(*tl, shaper, db));
    CHECK_FALSE(refreshTextCache(*tl, shaper, db));  // current: no block/effects change
    const std::vector<std::uint8_t> before = tl->cachedImage()->rgba;

    mosaic::core::LayerEffects fx;
    fx.colorOverlay.enabled = true;
    fx.colorOverlay.paint = vec::SolidPaint{ColorF{0, 1, 0, 1}};
    tl->setEffects(fx);
    CHECK(refreshTextCache(*tl, shaper, db));  // the overlay key mismatch forces a re-render
    CHECK(tl->cachedImage()->rgba != before);  // and the pixels really changed
    CHECK_FALSE(refreshTextCache(*tl, shaper, db));  // stable again at the new key

    // Effects edits that DON'T touch the overlays never stale the 3D cache (they apply
    // post-composite): add a drop shadow to the same stack.
    fx.dropShadows.emplace_back();
    fx.dropShadows.back().enabled = true;
    tl->setEffects(fx);
    CHECK_FALSE(refreshTextCache(*tl, shaper, db));

    // But REMOVING the overlay does: the baked design must leave the pixels.
    tl->clearEffects();
    CHECK(refreshTextCache(*tl, shaper, db));
    CHECK(tl->cachedImage()->rgba == before);  // and the bare red solid is back, byte-exact

    // A FLAT block's cache ignores overlay edits entirely.
    TextBlock flat = makeBlock("Hi", st);
    tl->setBlock(flat);
    CHECK(refreshTextCache(*tl, shaper, db));
    tl->setEffects(fx);
    CHECK_FALSE(refreshTextCache(*tl, shaper, db));
}

TEST_CASE("renderTextF routes an extruded block through the 3D lane end to end") {
    mosaic::platform::FontDB db;
    if (db.families().empty()) return;
    FontRef probe;
    probe.family = db.defaultFamily();
    if (!db.resolve(probe)) return;
    TextShaper shaper;

    CharStyle st;
    st.font.family = db.defaultFamily();
    st.sizePx = 48.0f;
    st.setSolidFill({0, 0, 0, 1});
    TextBlock flat = makeBlock("Hi", st);
    TextBlock solid = flat;
    solid.extrude = Extrude{};  // defaults: near-ortho, lit, grey material

    const ImageF img2d = renderTextF(shaper, flat, db, 300, 120, Affine2D::translation(20, 60));
    const ImageF img3d = renderTextF(shaper, solid, db, 300, 120, Affine2D::translation(20, 60));
    const Ink flat2d = scan(img2d);
    const Ink lit3d = scan(img3d);
    REQUIRE(flat2d.count > 0);
    REQUIRE(lit3d.count > 0);
    // The 3D lane shades with the default grey material, not the run's black fill.
    CHECK(lit3d.lum > flat2d.lum + 0.05);
    // At identity orientation the solid sits where the flat text sat (scale-true z=0 plane):
    // the ink bboxes overlap substantially.
    CHECK(lit3d.minX < flat2d.maxX);
    CHECK(lit3d.maxX > flat2d.minX);
}

TEST_CASE("a bent extruded block renders the ARCHED solid (3D + bend compose, S30 2026-07-07)") {
    mosaic::platform::FontDB db;
    if (db.families().empty()) return;
    FontRef probe;
    probe.family = db.defaultFamily();
    if (!db.resolve(probe)) return;
    TextShaper shaper;

    CharStyle st;
    st.font.family = db.defaultFamily();
    st.sizePx = 48.0f;
    st.setSolidFill({0, 0, 0, 1});
    TextBlock solid = makeBlock("MOSAIC", st);
    solid.extrude = Extrude{};
    TextBlock bentSolid = solid;
    bentSolid.bend = 0.9f;

    // The bent layout is what the mesher sees: the shaped block carries the warped pens/arc.
    const ShapedBlock sb = shaper.layout(bentSolid, db);
    REQUIRE(sb.bentArc.active);

    const ImageF imgFlat = renderTextF(shaper, solid, db, 400, 260, Affine2D::translation(20, 160));
    const ImageF imgBent =
        renderTextF(shaper, bentSolid, db, 400, 260, Affine2D::translation(20, 160));
    const Ink flat3d = scan(imgFlat);
    const Ink bent3d = scan(imgBent);
    REQUIRE(flat3d.count > 0);
    REQUIRE(bent3d.count > 0);
    // The arch lifts the middle of the word: the bent solid inks clearly higher (smaller minY)
    // and clearly taller than the flat solid. (The ink WIDTH is no sturdy signal: the chord
    // contracts but the steeply tilted end glyphs poke wider than their flat selves.)
    CHECK(bent3d.minY < flat3d.minY - 10.0);
    CHECK(bent3d.maxY - bent3d.minY > (flat3d.maxY - flat3d.minY) + 10.0);
}
