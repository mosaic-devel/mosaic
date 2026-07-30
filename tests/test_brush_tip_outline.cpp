#include <doctest/doctest.h>

#include "core/brush/bitmap_tip.hpp"
#include "core/brush/brush_tip.hpp"
#include "core/brush/mask_generator.hpp"
#include "core/brush/tip_outline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

// The reticle's TIP OUTLINE (docs/brushes.md §6.3): the traced silhouette of the tip the stroke will
// actually lay, as a signed distance field the present shader samples in place of its analytic
// ellipse. The bug this closes is a user's: pick `i)_Wet_Bristles_Rough`, get a perfect oval drawn
// over a bristly tip, and be told the wrong thing about where paint will land.
//
// ⚠ THE TRAP THIS FILE IS WRITTEN AROUND. Every on-axis probe of a tip's outline passes on code that
// ignores the tip's SHAPE entirely -- because the bounding box is sized right either way, and the box
// touches the shape exactly on the axes. The probes that can tell a traced contour from a stretched
// oval are the ones INSIDE the box and OUTSIDE the shape (a corner, a notch, the gap between two
// bristles), and every shape test below has one.
namespace {

using mosaic::core::brush::BitmapTip;
using mosaic::core::brush::BrushTip;
using mosaic::core::brush::buildTipSdf;
using mosaic::core::brush::kTipSdfMaxCells;
using mosaic::core::brush::kTipSdfRes;
using mosaic::core::brush::makeTip;
using mosaic::core::brush::MaskFalloff;
using mosaic::core::brush::MaskGeneratorParams;
using mosaic::core::brush::MaskShape;
using mosaic::core::brush::TipApplication;
using mosaic::core::brush::tipNeedsSdf;
using mosaic::core::brush::TipFrame;
using mosaic::core::brush::TipSdf;
using mosaic::core::brush::TipSourceKind;

constexpr double kPi = 3.14159265358979323846;

[[nodiscard]] MaskGeneratorParams circleGen(int spikes = 2) {
    MaskGeneratorParams g;
    g.shape = MaskShape::Circle;
    g.falloff = MaskFalloff::Default;
    g.diameter = 24.0;
    g.spikes = spikes;
    return g;
}

// A bitmap tip from a `w x h` ink stencil: `ink(x, y)` true means full paint. The TIP IMAGE
// convention is white = NO paint (docs/brushes.md §3.6.1), so ink is black.
template <typename Fn>
[[nodiscard]] std::shared_ptr<const BrushTip> stencilTip(std::uint32_t w, std::uint32_t h, Fn ink) {
    TipFrame f;
    f.width = w;
    f.height = h;
    f.rgba.assign(static_cast<std::size_t>(w) * h * 4, 255);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            if (ink(x, y)) {
                const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
                f.rgba[i] = f.rgba[i + 1] = f.rgba[i + 2] = 0;
            }
    return makeTip(std::make_shared<const BitmapTip>(std::vector<TipFrame>{std::move(f)},
                                                     TipApplication::AlphaMask, TipSourceKind::Mask));
}

// ---- the shader's SDF path, mirrored ---------------------------------------------------------
//
// A parity lane, exactly as `tests/test_brush_reticle.cpp` mirrors the analytic `tipDist` and
// `extrude_render` mirrors `extrude_raster.comp`: the GLSL is unreachable from a test, so the formula
// is pinned here and any divergence is a divergence from something that HAS a test. Line for line
// with `sdfSample`/`tipDist` in shaders/canvas_present.comp.

[[nodiscard]] double sdfSample(const TipSdf& s, double gx, double gy) {
    const double hix = static_cast<double>(s.w) - 0.5;
    const double hiy = static_cast<double>(s.h) - 0.5;
    const double cx = std::clamp(gx, 0.5, hix);
    const double cy = std::clamp(gy, 0.5, hiy);
    const double tx = cx - 0.5;
    const double ty = cy - 0.5;
    const double fx = tx - std::floor(tx);
    const double fy = ty - std::floor(ty);
    const int i0x = std::clamp(static_cast<int>(std::floor(tx)), 0, s.w - 1);
    const int i0y = std::clamp(static_cast<int>(std::floor(ty)), 0, s.h - 1);
    const int i1x = std::min(i0x + 1, s.w - 1);
    const int i1y = std::min(i0y + 1, s.h - 1);
    const double s00 = s.at(i0x, i0y);
    const double s10 = s.at(i1x, i0y);
    const double s01 = s.at(i0x, i1y);
    const double s11 = s.at(i1x, i1y);
    const double top = s00 + (s10 - s00) * fx;
    const double bot = s01 + (s11 - s01) * fx;
    const double v = top + (bot - top) * fy;
    // Beyond the grid the field keeps GROWING -- it is not clamped to the border cell, or a pixel far
    // from a square tip would read "half a cell from the outline", which is what a pixel ON the ring
    // reads.
    const double outx = gx - cx;
    const double outy = gy - cy;
    return v + std::sqrt(outx * outx + outy * outy) / std::fmax(s.boxW, 1e-6);
}

// The signed distance in SCREEN px from `(dx, dy)` (screen px, from the reticle's centre) to the
// tip's outline, sampled out of the field: the shader's `tipDist` with sdfActive != 0.
[[nodiscard]] double tipDistSdf(const TipSdf& s, double dx, double dy, double a, double b,
                                double theta) {
    const double c = std::cos(theta);
    const double sn = std::sin(theta);
    const double qx = dx * c + dy * sn;
    const double qy = -dx * sn + dy * c;
    const double kx = s.boxW / std::fmax(2.0 * a, 1e-6);
    const double ky = s.boxH / std::fmax(2.0 * b, 1e-6);
    const double ox = (s.pad - 0.5) + s.boxW * 0.5; // the pad, the pixel centre, half the box
    const double oy = (s.pad - 0.5) + s.boxH * 0.5;
    const auto at = [&](double x, double y) { return sdfSample(s, x * kx + ox, y * ky + oy); };
    const double f0 = at(qx, qy);
    const double e = 1.0;
    const double gx = at(qx + e, qy) - at(qx - e, qy);
    const double gy = at(qx, qy + e) - at(qx, qy - e);
    // ⚠ The gradient VANISHES on the field's medial axis (a disc's centre, the spine of a gap between
    // two bristles). Floored at the smallest it can honestly be -- exact for an isotropic map.
    const double gmin = std::fmin(kx, ky) / std::fmax(s.boxW, 1e-6);
    const double gm = std::fmax(std::sqrt(gx * gx + gy * gy) / (2.0 * e), gmin);
    return f0 / gm;
}

// The analytic ellipse the shader draws when the tip does NOT trace -- here only as the thing every
// shape test below must DISAGREE with. If a "traced" outline agrees with this everywhere, it is not
// traced.
[[nodiscard]] double tipDistEllipse(double dx, double dy, double a, double b, double theta) {
    const double c = std::cos(theta);
    const double s = std::sin(theta);
    const double qx = dx * c + dy * s;
    const double qy = -dx * s + dy * c;
    const double k = a / b;
    const double le = std::fmax(std::sqrt(qx * qx + (qy * k) * (qy * k)), 1e-6);
    const double grad = std::fmax(std::sqrt(qx * qx + (qy * k * k) * (qy * k * k)) / le, 1e-6);
    return (le - a) / grad;
}

} // namespace

// ---- tipNeedsSdf: who traces --------------------------------------------------------------------

TEST_CASE("tip outline: a NULL tip never traces") {
    // ⚠ The one that must not move. A NULL tip is the engine's built-in analytic circle -- the tip
    // every golden in the suite and all 125 bit-exact antialiasing equalities were laid by. Trace it
    // and the ring would come out a rasterized approximation of the circle it already draws exactly.
    CHECK_FALSE(tipNeedsSdf(nullptr));
}

TEST_CASE("tip outline: a plain round generator does not trace -- the ellipse IS its outline") {
    const auto tip = makeTip(circleGen());
    CHECK_FALSE(tipNeedsSdf(tip.get()));

    // ... at every falloff. What fills the tip changes; its SILHOUETTE is the dab's ellipse either
    // way, and the reticle traces the silhouette.
    for (const MaskFalloff f : {MaskFalloff::Default, MaskFalloff::Soft, MaskFalloff::Gauss}) {
        MaskGeneratorParams g = circleGen();
        g.falloff = f;
        CHECK_FALSE(tipNeedsSdf(makeTip(g).get()));
    }
}

TEST_CASE("tip outline: a rect generator traces -- the shader's closed form knows only ellipses") {
    MaskGeneratorParams g = circleGen();
    g.shape = MaskShape::Rect;
    CHECK(tipNeedsSdf(makeTip(g).get()));
}

TEST_CASE("tip outline: a spiked generator traces") {
    CHECK_FALSE(tipNeedsSdf(makeTip(circleGen(/*spikes=*/1)).get())); // 1 and 2 are ordinary tips
    CHECK_FALSE(tipNeedsSdf(makeTip(circleGen(2)).get()));
    CHECK(tipNeedsSdf(makeTip(circleGen(3)).get())); // > 2 folds the tip into a star
    CHECK(tipNeedsSdf(makeTip(circleGen(9)).get()));
}

TEST_CASE("tip outline: every bitmap tip traces") {
    // The whole reason this exists: 47 of the 82 shipped pixel-brush presets carry one, and not one
    // of them is an ellipse.
    const auto tip = stencilTip(16, 16, [](std::uint32_t, std::uint32_t) { return true; });
    CHECK(tipNeedsSdf(tip.get()));
}

// ---- buildTipSdf: the field ----------------------------------------------------------------------

TEST_CASE("tip outline: the field's box is the TIP's -- not a square it happens to fit in") {
    // A 64x16 stamp paints a dab four times wider than it is tall (its `diameter` sets the LONG axis
    // and the frame's aspect fills in the rest). A field built on a square box would put the outline
    // where the tip is not.
    const auto tip = stencilTip(64, 16, [](std::uint32_t, std::uint32_t) { return true; });
    const TipSdf s = buildTipSdf(*tip, 0, /*ratio=*/1.0, /*res=*/64);
    REQUIRE_FALSE(s.empty());
    CHECK(s.boxW == doctest::Approx(64.0));
    CHECK(s.boxH == doctest::Approx(16.0));
    CHECK(s.w == 64 + 2 * s.pad); // the box, padded with background all round
    CHECK(s.h == 16 + 2 * s.pad);
    CHECK(s.pad == mosaic::core::brush::kTipSdfPad);
    CHECK(s.d.size() == static_cast<std::size_t>(s.w) * s.h);
}

TEST_CASE("tip outline: the field fits the renderer's storage") {
    // The renderer REFUSES an oversized field (and falls back to the ellipse) rather than clamping
    // one; the core must therefore never hand it one. A tip built at the core's own resolution is the
    // largest it can produce.
    const auto tip = stencilTip(64, 64, [](std::uint32_t, std::uint32_t) { return true; });
    const TipSdf s = buildTipSdf(*tip, 0, /*ratio=*/1.0, kTipSdfRes);
    REQUIRE_FALSE(s.empty());
    CHECK(s.d.size() <= kTipSdfMaxCells);
    // (That the RENDERER's storage is at least kTipSdfMaxCells is a static_assert at the seam --
    // src/ui/vulkan_canvas.cpp, the one file that sees both constants.)
}

TEST_CASE("tip outline: inside is negative and outside is positive") {
    const auto tip = stencilTip(32, 32, [](std::uint32_t x, std::uint32_t y) {
        const double dx = x + 0.5 - 16.0;
        const double dy = y + 0.5 - 16.0;
        return dx * dx + dy * dy <= 12.0 * 12.0; // a disc well inside its own frame
    });
    const TipSdf s = buildTipSdf(*tip, 0, /*ratio=*/1.0, /*res=*/64);
    REQUIRE_FALSE(s.empty());

    const int cx = s.w / 2;
    const int cy = s.h / 2;
    CHECK(s.at(cx, cy) < 0.0F);  // the centre of the disc
    CHECK(s.at(1, 1) > 0.0F);    // the box's corner: inside the BOX, outside the DISC
    CHECK(s.at(0, 0) > 0.0F);    // the pad
    CHECK(s.at(cx, 1) > 0.0F);   // above the disc, still inside the box

    // The disc's radius is 12/32 of the frame -> 24 of the field's 64 build px. The zero crossing is
    // between those two cells, not on either.
    CHECK(s.at(cx + 20, cy) < 0.0F);
    CHECK(s.at(cx + 28, cy) > 0.0F);
}

TEST_CASE("tip outline: a tip that paints nothing has no outline") {
    // A degenerate tip falls back to the analytic ellipse -- which at least still says WHERE the
    // cursor is, the more useful of the reticle's two jobs once the other has broken down.
    const auto blank = stencilTip(16, 16, [](std::uint32_t, std::uint32_t) { return false; });
    CHECK(buildTipSdf(*blank, 0, 1.0, 64).empty());

    const auto tip = stencilTip(16, 16, [](std::uint32_t, std::uint32_t) { return true; });
    CHECK(buildTipSdf(*tip, /*frame=*/7, 1.0, 64).empty()); // no such frame

    // A preset is an untrusted file, and neither a nonsense resolution nor a nonsense ratio may
    // produce a field the shader would then sample: each falls back to a sane one rather than
    // dividing by it.
    CHECK_FALSE(buildTipSdf(*tip, 0, 1.0, /*res=*/0).empty());
    CHECK_FALSE(buildTipSdf(*tip, 0, /*ratio=*/0.0, 64).empty());
    CHECK_FALSE(buildTipSdf(*tip, 0, /*ratio=*/std::nan(""), 64).empty());

    // A tall tip (`ratio` > 1) still fits: `res` sizes the LONG axis, whichever one that is -- a grid
    // scaled off the width alone would overrun the storage the renderer sized for `res`.
    const TipSdf tall = buildTipSdf(*tip, 0, /*ratio=*/3.0, /*res=*/64);
    REQUIRE_FALSE(tall.empty());
    CHECK(tall.boxH == doctest::Approx(64.0)); // the long axis IS the res
    CHECK(tall.boxW == doctest::Approx(64.0 / 3.0));
    CHECK(tall.h <= 64 + 2 * tall.pad);
}

// ---- the field, read back through the shader's own sampler ---------------------------------------

TEST_CASE("tip outline: a traced circle agrees with the ellipse it is") {
    // The sanity floor. A round bitmap stamp and the analytic ellipse describe the SAME curve, so the
    // traced field must reproduce the closed form to within its own grid -- otherwise the mapping,
    // the normalization or the gradient correction is wrong, and every shaped tip below would be
    // wrong in a way no test could name.
    //
    // ⚠ The stencil is the BUILD RESOLUTION, 1:1, and that is not incidental. A tip rasterized at any
    // other scale is RESAMPLED, and a bilinear resample spreads every non-zero source pixel into its
    // neighbours -- so under the `coverage != 0` rule (any coverage at all is inside, §6.3) the
    // silhouette of a MAGNIFIED tip is honestly a pixel or two fatter than the shape it was drawn
    // from. That is the rule working, not the mapping failing, and pinning the mapping means taking
    // it out of the picture. (It is also why the reference's own outline is a shade generous, and why
    // being generous is the right side to err on: a ring INSIDE the paint would understate the brush.)
    const auto tip = stencilTip(kTipSdfRes, kTipSdfRes, [](std::uint32_t x, std::uint32_t y) {
        const double dx = x + 0.5 - kTipSdfRes / 2.0;
        const double dy = y + 0.5 - kTipSdfRes / 2.0;
        return dx * dx + dy * dy <= (kTipSdfRes / 2.0) * (kTipSdfRes / 2.0);
    });
    const TipSdf s = buildTipSdf(*tip, 0, /*ratio=*/1.0, kTipSdfRes);
    REQUIRE_FALSE(s.empty());

    const double a = 40.0; // screen semi-axes: a 40 px document tip at 2x zoom
    const double b = 40.0;

    // ⚠ NEAR THE OUTLINE is where the agreement has to be tight, and it is the only place it has to
    // be: the ring is drawn over a band of a few px either side (`brushReticle` culls at |d| <= 5.5),
    // and the antialiasing reads the distance directly. Sub-pixel here or the ring is visibly wrong.
    for (int i = 0; i < 24; ++i) {
        const double t = kPi * 2.0 * i / 24.0;
        for (const double r : {34.0, 38.0, 40.0, 42.0, 46.0}) {
            const double dx = r * std::cos(t);
            const double dy = r * std::sin(t);
            CHECK(std::fabs(tipDistSdf(s, dx, dy, a, b, 0.0) -
                            tipDistEllipse(dx, dy, a, b, 0.0)) < 0.75);
        }
    }

    // DEEP INSIDE the tip the two drift apart by a px or two, and that is inherent rather than a bug
    // worth chasing: the field's gradient is finite-differenced over one SCREEN px, and near the tip's
    // centre the distance field is a cone whose curvature over that step is real. Every one of these
    // points is 20+ px from the outline -- culled long before anything is drawn -- and the sign, which
    // is the only thing the ring reads out here, is exactly right.
    for (int i = 0; i < 24; ++i) {
        const double t = kPi * 2.0 * i / 24.0;
        for (const double r : {2.0, 12.0, 24.0}) {
            const double dx = r * std::cos(t);
            const double dy = r * std::sin(t);
            const double traced = tipDistSdf(s, dx, dy, a, b, 0.0);
            CHECK(traced < 0.0);
            CHECK(std::fabs(traced - tipDistEllipse(dx, dy, a, b, 0.0)) < 3.0);
        }
    }

    // ⚠ The exact CENTRE is excluded above, and not because the field is wrong there -- because the
    // ANALYTIC formula is. It has no direction to a rim at r == 0 (its own test excludes the point for
    // the same reason: tests/test_brush_reticle.cpp) and divides by its 1e-6 floor, reading -4e7 px
    // instead of -40. The field, whose gradient is floored at something honest, just says "-40, deep
    // inside" -- and both answers cull to the same thing, which is why neither has ever been visible.
    CHECK(tipDistSdf(s, 0.0, 0.0, a, b, 0.0) == doctest::Approx(-40.0).epsilon(0.05));

    // Far outside the grid the field is EXTRAPOLATED rather than sampled, so it is no longer exact --
    // but it must still be big, positive and growing, or the ring's cull would fire in open space.
    CHECK(tipDistSdf(s, 200.0, 0.0, a, b, 0.0) > 100.0);
    CHECK(tipDistSdf(s, 400.0, 400.0, a, b, 0.0) > tipDistSdf(s, 200.0, 200.0, a, b, 0.0));
}

TEST_CASE("tip outline: a SQUARE tip is not an oval -- the corner probe") {
    // ⚠ THE PRIMARY CHECK, and the one an on-axis probe cannot make. A solid square stamp and an
    // ellipse of the same box agree exactly on the four axes and nowhere else. So probe the CORNER:
    // deep inside the square (negative), well outside the inscribed ellipse (the old reticle's
    // answer, and positive). A ring that still drew the oval passes every other assertion in this
    // file and fails this one.
    const auto tip = stencilTip(64, 64, [](std::uint32_t, std::uint32_t) { return true; });
    const TipSdf s = buildTipSdf(*tip, 0, /*ratio=*/1.0, kTipSdfRes);
    REQUIRE_FALSE(s.empty());

    const double a = 40.0;
    const double b = 40.0;

    // 90% of the way to the corner: 4 px inside the square's two edges, ~11 px OUTSIDE the ellipse.
    const double q = 36.0;
    CHECK(tipDistSdf(s, q, q, a, b, 0.0) < -2.0);  // the square says: inside
    CHECK(tipDistEllipse(q, q, a, b, 0.0) > 2.0);  // the oval says: outside. One of them is a lie.

    // The corner itself is ON the square's outline.
    CHECK(std::fabs(tipDistSdf(s, a, b, a, b, 0.0)) < 2.5);
    // ... and the edge midpoints are too -- which is exactly what the ellipse ALSO says, and why an
    // on-axis probe proves nothing.
    CHECK(std::fabs(tipDistSdf(s, a, 0.0, a, b, 0.0)) < 2.0);
    CHECK(std::fabs(tipDistSdf(s, 0.0, b, a, b, 0.0)) < 2.0);
    // Beyond the corner, the field keeps growing -- it is not clamped to the border cell (which would
    // smear the ring across the whole screen for a square tip).
    CHECK(tipDistSdf(s, a + 30.0, b + 30.0, a, b, 0.0) > 20.0);
}

TEST_CASE("tip outline: a tip's HOLES are traced -- the notch probe") {
    // A bristle tip is mostly gaps, and the gaps are the point: the outline runs around them. A ring
    // that only knew the tip's silhouette from the outside would show a smooth blob.
    //
    // A C-shape: a ring with its right quarter bitten out. The notch is inside the bounding box, far
    // from any edge, and OUTSIDE the tip.
    const auto tip = stencilTip(64, 64, [](std::uint32_t x, std::uint32_t y) {
        const double dx = x + 0.5 - 32.0;
        const double dy = y + 0.5 - 32.0;
        const double r = std::sqrt(dx * dx + dy * dy);
        if (r > 31.0 || r < 16.0)
            return false;                      // an annulus...
        return !(dx > 0.0 && std::fabs(dy) < dx); // ... with the right wedge bitten out
    });
    const TipSdf s = buildTipSdf(*tip, 0, /*ratio=*/1.0, kTipSdfRes);
    REQUIRE_FALSE(s.empty());

    const double a = 32.0; // screen: 1 screen px per source px
    const double b = 32.0;
    CHECK(tipDistSdf(s, -24.0, 0.0, a, b, 0.0) < 0.0); // on the ring's left limb: inside
    CHECK(tipDistSdf(s, 0.0, -24.0, a, b, 0.0) < 0.0); // ... and its top limb
    CHECK(tipDistSdf(s, 0.0, 0.0, a, b, 0.0) > 5.0);   // the HOLE: the ring's own centre is OUTSIDE
    CHECK(tipDistSdf(s, 24.0, 0.0, a, b, 0.0) > 2.0);  // the NOTCH: inside the box, outside the tip

    // The ellipse is blind to every one of those: it calls the centre the deepest point of the tip.
    CHECK(tipDistEllipse(0.0, 0.0, a, b, 0.0) < -20.0);
    CHECK(tipDistEllipse(24.0, 0.0, a, b, 0.0) < 0.0);
}

TEST_CASE("tip outline: the zero level set survives the squash and the rotation") {
    // The field is stored in the TIP'S OWN frame at ratio 1 and angle 0 -- the dab's aspect and the
    // view's rotation are applied when it is SAMPLED. So the outline has to land on the tip wherever
    // those two put it, and the distance it reports has to stay a distance in SCREEN px through an
    // anisotropic map. (That is the metric correction: without it, a squashed tip's ring visibly
    // thins out at its flat ends -- the same trap the analytic path already dodges.)
    const auto tip = stencilTip(64, 64, [](std::uint32_t x, std::uint32_t y) {
        const double dx = x + 0.5 - 32.0;
        const double dy = y + 0.5 - 32.0;
        return dx * dx + dy * dy <= 32.0 * 32.0;
    });
    const TipSdf s = buildTipSdf(*tip, 0, /*ratio=*/1.0, kTipSdfRes);
    REQUIRE_FALSE(s.empty());

    const double a = 60.0; // a 4:1 nib, well past where a naive squash-and-measure goes wrong
    const double b = 15.0;
    for (const double theta : {0.0, 0.6, kPi * 0.5, -1.2}) {
        for (int i = 0; i < 24; ++i) {
            const double t = kPi * 2.0 * i / 24.0;
            const double ex = a * std::cos(t);
            const double ey = b * std::sin(t);
            const double dx = ex * std::cos(theta) - ey * std::sin(theta);
            const double dy = ex * std::sin(theta) + ey * std::cos(theta);
            CHECK(std::fabs(tipDistSdf(s, dx, dy, a, b, theta)) < 2.0);
        }
    }

    // ⚠ The metric check, and the reason the gradient correction exists. One screen px outside the
    // SHORT axis' end must read ~1 px -- not 1 px stretched by a/b (4x here), which would draw the
    // ring a quarter of its proper width there.
    CHECK(tipDistSdf(s, 0.0, b + 4.0, a, b, 0.0) == doctest::Approx(4.0).epsilon(0.3));
    CHECK(tipDistSdf(s, a + 4.0, 0.0, a, b, 0.0) == doctest::Approx(4.0).epsilon(0.3));
}

TEST_CASE("tip outline: a spiked tip keeps its spikes -- and only a SQUASHED one has any") {
    // ⚠ The whole reason `buildTipSdf` takes a `ratio` instead of building the tip once in its own
    // frame and stretching it when sampled.
    //
    // `spikes > 2` folds the RAW, un-normalized offset into an angular wedge before the falloff
    // normalizes it (mask_generator.cpp's `fixRotation`). That fold is a ROTATION -- and a rotation
    // preserves x^2 + y^2. So at ratio 1 the normalizer is isotropic, the fold cancels out of the
    // silhouette exactly, and a five-spiked circle is... a circle. Squash it and the two stop
    // commuting: the star appears.
    //
    // Which means a field built at ratio 1 and stretched afterwards would draw a smooth oval over the
    // one tip whose entire point is that it is not one.
    MaskGeneratorParams g = circleGen(/*spikes=*/5);
    const auto tip = makeTip(g);
    REQUIRE(tipNeedsSdf(tip.get()));

    // Round: the fold is invisible, and the traced outline agrees with the ellipse (which is the
    // truth here -- the SDF costs nothing but says the same thing).
    {
        const TipSdf s = buildTipSdf(*tip, 0, /*ratio=*/1.0);
        REQUIRE_FALSE(s.empty());
        double worst = 0.0;
        for (int i = 0; i < 72; ++i) {
            const double t = kPi * 2.0 * i / 72.0;
            worst = std::fmax(worst, std::fabs(tipDistSdf(s, 50.0 * std::cos(t), 50.0 * std::sin(t),
                                                          50.0, 50.0, 0.0)));
        }
        CHECK(worst < 2.0);
    }

    // Squashed: the star is real, and the ellipse (which reads 0 all the way round its own rim) is
    // wrong by many pixels somewhere on it.
    {
        const double ratio = 0.5;
        const TipSdf s = buildTipSdf(*tip, 0, ratio);
        REQUIRE_FALSE(s.empty());
        CHECK(s.boxH == doctest::Approx(s.boxW * ratio)); // the box carries the squash

        const double a = 64.0; // the dab's own screen semi-axes at this ratio
        const double b = 32.0;
        double worst = 0.0;
        for (int i = 0; i < 72; ++i) {
            const double t = kPi * 2.0 * i / 72.0;
            const double d = tipDistSdf(s, a * std::cos(t), b * std::sin(t), a, b, 0.0);
            worst = std::fmax(worst, std::fabs(d));
        }
        CHECK(worst > 4.0);
    }
}
