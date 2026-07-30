#include "common/image.hpp"
#include "core/blend_mode.hpp"
#include "core/brush/brush_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <vector>

// The accumulation axes added in S19 (docs/brushes.md §6.1): `PaintMode {Wash, Buildup}`,
// `StrokeMode {Paint, Erase}`, and a `BlendMode` applied at composite. All combinations are
// reachable, and the coverage buffer is maintained in every one of them because the Inpaint brush
// reads it as a hole mask.
//
// `Paint x Wash x Normal` -- the pre-existing behaviour -- is pinned byte-for-byte next door in
// test_brush_wash_golden.cpp. Nothing here re-asserts it; these cases only cover what is new.
namespace {

using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::core::BlendMode;
using mosaic::core::brush::BrushDynamics;
using mosaic::core::brush::BrushEngine;
using mosaic::core::brush::BrushParams;
using mosaic::core::brush::PaintMode;
using mosaic::core::brush::StrokeMode;
using mosaic::core::brush::StrokeInput;

Color8 pixel(const Image& img, int x, int y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

// A stroke that crosses itself at (48,48): out along the diagonal, then back across it. Every dab
// is at full pressure, so overlap -- not pressure -- is what drives the accumulation.
const std::vector<StrokeInput>& crossingPath() {
    static const std::vector<StrokeInput> path{
        StrokeInput{{20.0, 20.0}, 1.0}, StrokeInput{{76.0, 76.0}, 1.0},
        StrokeInput{{76.0, 20.0}, 1.0}, StrokeInput{{20.0, 76.0}, 1.0}};
    return path;
}

// Paint `path` onto a `fill`-coloured 96x96 target and return it.
Image paint(BrushParams p, const std::vector<StrokeInput>& path, Color8 fill) {
    Image img(96, 96);
    img.fill(fill);
    BrushEngine eng;
    eng.begin(96, 96, img, p, BrushDynamics{}, path.front());
    for (std::size_t i = 1; i < path.size(); ++i)
        eng.extendTo(path[i]);
    eng.flush(); // the walk lags one sample; lay the tail span before reading the pixels
    eng.composite();
    eng.end();
    return img;
}

BrushParams basePaint() {
    BrushParams p;
    p.diameter = 18.0;
    p.hardness = 0.9;
    p.flow = 1.0;
    p.opacity = 0.5;
    p.spacing = 0.05;
    p.color = Color8{0, 0, 0, 255};
    return p;
}

// Exactly two dabs, both landing solidly on pixel (48,48), with every quantity dyadic so the
// arithmetic is exact and the expected bytes can be derived by hand rather than recorded.
//
//   spacingPx = 0.1 * 20 = 2.0, and the segment is 2.0 long -> exactly one dab beyond the press.
//   hardness 1 and a 10 px radius -> both dabs have coverage 1.0 at that pixel, so a = flow = 0.5.
//   Wash:    coverage = 0.5 + 0.5*(1-0.5)     = 0.75    -> sa = 0.75 * cap(0.5) = 0.375
//   Buildup: acc      = 0.25 + 0.25*(1-0.25)  = 0.4375  -> sa = 0.4375
//
// Both the `(1 - acc)` damping and the per-dab ceiling are pinned two-sidedly by those numbers: an
// accumulation of `acc + da` gives 0.5, and one that drops the ceiling gives 0.75.
Image twoDabs(BrushParams p, Color8 fill) {
    p.diameter = 20.0;
    p.hardness = 1.0;
    p.flow = 0.5;
    p.opacity = 0.5;
    p.spacing = 0.1;

    Image img(96, 96);
    img.fill(fill);
    BrushEngine eng;
    eng.begin(96, 96, img, p, BrushDynamics{}, StrokeInput{{48.0, 48.0}, 1.0});
    eng.extendTo(StrokeInput{{50.0, 48.0}, 1.0});
    eng.flush();
    eng.composite();
    eng.end();
    return img;
}

} // namespace

TEST_CASE("two overlapping dabs pin the Buildup accumulation curve exactly") {
    // The mode's other cases are one-sided ("Buildup goes further than Wash"), which a regression
    // that overshoots -- dropping the `(1 - acc)` damping, or the per-dab ceiling -- also
    // satisfies. These two numbers are exact and admit neither.
    BrushParams wash = basePaint();
    BrushParams build = basePaint();
    build.paintMode = PaintMode::Buildup;

    // Over a transparent base the composite alpha IS the stroke alpha, so the accumulation is read
    // straight off the pixel with no backdrop arithmetic in the way.
    const Image w = twoDabs(wash, Color8{0, 0, 0, 0});
    const Image b = twoDabs(build, Color8{0, 0, 0, 0});

    CHECK(pixel(w, 48, 48).a == 96); // lround(0.375  * 255) -- capped at opacity 0.5
    CHECK(pixel(b, 48, 48).a ==
          112); // lround(0.4375 * 255) -- past it, by exactly the right amount
}

TEST_CASE("two overlapping erase dabs pin the erase ceiling exactly") {
    // Same numbers seen from the other side: the eraser carves `sa` out of an opaque base, so the
    // surviving alpha is 255*(1-sa). Without this, the erase axis' only ceiling check is one-sided
    // and an eraser that ignored `opacity` entirely (carving to 0) would pass.
    BrushParams wash = basePaint();
    wash.strokeMode = StrokeMode::Erase;
    BrushParams build = wash;
    build.paintMode = PaintMode::Buildup;

    const Image w = twoDabs(wash, Color8{10, 200, 90, 255});
    const Image b = twoDabs(build, Color8{10, 200, 90, 255});

    CHECK(pixel(w, 48, 48).a == 159); // lround((1 - 0.375)  * 255)
    CHECK(pixel(b, 48, 48).a == 143); // lround((1 - 0.4375) * 255)
    CHECK(pixel(w, 48, 48).r == 10);  // colour preserved on both paths
    CHECK(pixel(b, 48, 48).g == 200);
}

TEST_CASE("Buildup climbs past the opacity a Wash stroke is capped at") {
    // The whole distinction between the two modes, at the one pixel where the stroke crosses
    // itself. Black at opacity 0.5 over white: Wash caps at lround(127.5) = 128 however many dabs
    // overlap; Buildup lets the overlapping dabs' 0.5 shares accumulate "over" each other, well
    // past it.
    BrushParams wash = basePaint();
    BrushParams build = basePaint();
    build.paintMode = PaintMode::Buildup;

    const Image w = paint(wash, crossingPath(), Color8{255, 255, 255, 255});
    const Image b = paint(build, crossingPath(), Color8{255, 255, 255, 255});

    CHECK(pixel(w, 48, 48) == Color8{128, 128, 128, 255}); // capped, exactly as Wash promises
    const Color8 bc = pixel(b, 48, 48);
    CHECK(bc.r < 60); // far darker: the crossing built up
    CHECK(bc.a == 255);

    // Away from the crossing, on a stretch the stroke passes exactly once, Buildup's dabs still
    // overlap each other along the stroke -- so it is darker there too, but by less.
    CHECK(pixel(b, 30, 30).r < pixel(w, 30, 30).r);
}

TEST_CASE("Buildup on a transparent layer drives alpha toward opaque") {
    // With no backdrop the difference shows in alpha rather than colour: Wash tops out at the
    // opacity, Buildup keeps depositing.
    BrushParams wash = basePaint();
    BrushParams build = basePaint();
    build.paintMode = PaintMode::Buildup;

    const Image w = paint(wash, crossingPath(), Color8{0, 0, 0, 0});
    const Image b = paint(build, crossingPath(), Color8{0, 0, 0, 0});

    CHECK(pixel(w, 48, 48).a == 128); // 0.5 cap
    CHECK(pixel(b, 48, 48).a > 200);  // built well past it
    CHECK(pixel(b, 48, 48).a <= 255);
}

TEST_CASE("Buildup still maintains the coverage buffer the Inpaint brush reads") {
    // §6.1: the coverage buffer is the Inpaint brush's hole mask, so Buildup must keep it even
    // though it no longer decides the alpha. It is the geometric coverage, identical to Wash's.
    BrushParams wash = basePaint();
    BrushParams build = basePaint();
    build.paintMode = PaintMode::Buildup;

    const auto run = [](const BrushParams& p) {
        Image img(96, 96);
        BrushEngine eng;
        eng.begin(96, 96, img, p, BrushDynamics{}, crossingPath().front());
        for (std::size_t i = 1; i < crossingPath().size(); ++i)
            eng.extendTo(crossingPath()[i]);
        std::vector<float> cov = eng.coverage();
        eng.end();
        return cov;
    };
    const std::vector<float> cw = run(wash);
    const std::vector<float> cb = run(build);
    REQUIRE(cw.size() == cb.size());
    REQUIRE(!cw.empty());
    CHECK(cw == cb); // the paint mode does not touch the mask

    std::size_t touched = 0;
    for (float c : cb)
        if (c > 0.0f)
            ++touched;
    CHECK(touched > 1000);
}

TEST_CASE("the document size does not change any pixel the stroke paints") {
    // ensureCovers() re-homes coverage AND the base snapshot AND -- new in this slice -- the
    // Buildup accumulation. Getting the rect to grow is not enough to expose a dropped buffer:
    // composite() only rewrites the PENDING region, so an early pixel keeps its old bytes however
    // badly its accumulation was mangled. The stroke has to come BACK over that pixel afterwards.
    //
    // Rather than count dabs, run the identical stroke on two documents. On the 200x200 one the
    // very first ensureCovers() clamps the working rect to the whole layer, so it NEVER grows; on
    // the 1024x1024 one the excursion to (800,700) forces a growth and a re-home. Dab positions are
    // geometry, not document size, so every pixel the two share must come out the same. Any buffer
    // that fails to survive the move breaks that.
    BrushParams p = basePaint();
    p.paintMode = PaintMode::Buildup; // exercises all three buffers at once
    p.diameter = 24.0;
    p.spacing = 1.0;  // sparse dabs, so the accumulation is far from saturating at 255
    p.opacity = 0.15; // ... and each pass adds a visible, non-saturating share
    p.hardness = 1.0;

    const std::vector<StrokeInput> path{StrokeInput{{60.0, 60.0}, 1.0},
                                         StrokeInput{{120.0, 60.0}, 1.0},
                                         StrokeInput{{60.0, 60.0}, 1.0},
                                         StrokeInput{{800.0, 700.0}, 1.0}, // grows the big doc
                                         StrokeInput{{120.0, 60.0}, 1.0},
                                         StrokeInput{{60.0, 60.0}, 1.0}}; // re-stamps the early px

    const auto run = [&](std::uint32_t dim, std::uint32_t* finalCw) {
        Image img(dim, dim);
        img.fill(Color8{255, 255, 255, 255});
        BrushEngine eng;
        eng.begin(dim, dim, img, p, BrushDynamics{}, path.front());
        eng.composite();
        for (std::size_t i = 1; i < path.size(); ++i) {
            eng.extendTo(path[i]);
            eng.composite();
        }
        eng.flush(); // the tail span, which no lookahead sample is coming for
        eng.composite();
        *finalCw = eng.coverageWidth();
        eng.end();
        return img;
    };

    std::uint32_t smallCw = 0, bigCw = 0;
    const Image small = run(200, &smallCw);
    const Image big = run(1024, &bigCw);

    REQUIRE(smallCw == 200u); // clamped to the layer on the first dab: this run never grows
    REQUIRE(bigCw > 256u);    // ... and this one did grow, past its first 256 px tile

    // The early pixel really did accumulate across the excursion, rather than saturating or staying
    // at a single dab's worth -- otherwise the comparison below could not tell the two apart.
    const Color8 early = pixel(big, 90, 60);
    REQUIRE(early.a == 255);
    REQUIRE(early.r > 0);
    REQUIRE(early.r < 200);

    // Every pixel the two documents share must be identical. Reported as one assertion naming the
    // first offender, rather than 40 000 of them.
    int badX = -1, badY = -1;
    std::size_t differing = 0;
    for (int y = 0; y < 200 && differing == 0; ++y)
        for (int x = 0; x < 200; ++x)
            if (pixel(small, x, y) != pixel(big, x, y)) {
                badX = x;
                badY = y;
                ++differing;
                break;
            }
    INFO("first differing pixel: (" << badX << ", " << badY << ")");
    CHECK(differing == 0);
}

TEST_CASE("a single isolated dab is identical in Wash and Buildup at a dyadic ceiling") {
    // With no overlap there is nothing to build up: one dab deposits `flow * coverage * cap` in
    // both modes, because Buildup folds the same ceiling into each dab that Wash applies once at
    // the end. This is what pins the ceiling INTO the Buildup dab -- the mode's other tests only
    // ever check that it goes further than Wash, which dropping the ceiling also does.
    //
    // In REAL arithmetic that identity is unconditional. In floats it is not quite: Wash stores
    // `float(a)` and multiplies by the ceiling in double, while Buildup stores `float(a * cap)` --
    // the same value, rounded at a different moment. A sweep of 4.0e9 (a, cap) pairs finds 6026
    // that disagree, always by exactly one 8-bit level, the first at cap = 0.0085 (an opacity under
    // 1 %). The difference is intrinsic: `1 - prod(1 - a_i*cap)` is not a function of the uncapped
    // coverage, so Buildup must store the capped value, and making Wash round the same way would
    // change Wash's bytes -- which are pinned. Hence a ceiling of 0.5 here, where both are exact.
    // The bound itself is the next case.
    BrushParams wash = basePaint();
    wash.opacity = 0.5;
    BrushParams build = wash;
    build.paintMode = PaintMode::Buildup;

    const std::vector<StrokeInput> oneDab{StrokeInput{{48.0, 48.0}, 1.0}};
    const Image w = paint(wash, oneDab, Color8{200, 190, 180, 255});
    const Image b = paint(build, oneDab, Color8{200, 190, 180, 255});

    CHECK(w.rgba == b.rgba);          // byte for byte
    CHECK(pixel(w, 48, 48).a == 255); // over an opaque base
    CHECK(pixel(w, 48, 48).r == 100); // 0.5 of the way from 200 to 0
}

TEST_CASE("Wash and Buildup agree within one level on a single dab at any ceiling") {
    // The bound the case above leaves open. A soft dab lays down every partial coverage between 0
    // and 1, and these ceilings are deliberately non-dyadic (0.0085 is where the sweep's first
    // disagreement lives). More than one 8-bit level apart would mean the two modes are applying
    // the ceiling differently, not merely rounding it at different moments.
    for (const double opacity : {0.7, 0.3, 0.9, 0.0085, 0.123456}) {
        BrushParams wash = basePaint();
        wash.diameter = 41.0;
        wash.hardness = 0.0; // a long shoulder: many distinct partial coverages
        wash.flow = 1.0;
        wash.opacity = opacity;
        BrushParams build = wash;
        build.paintMode = PaintMode::Buildup;

        const std::vector<StrokeInput> oneDab{StrokeInput{{48.0, 48.0}, 1.0}};
        const Image w = paint(wash, oneDab, Color8{20, 30, 40, 255});
        const Image b = paint(build, oneDab, Color8{20, 30, 40, 255});
        REQUIRE(w.rgba.size() == b.rgba.size());

        int maxDelta = 0;
        for (std::size_t i = 0; i < w.rgba.size(); ++i)
            maxDelta = std::max(maxDelta, std::abs(int(w.rgba[i]) - int(b.rgba[i])));
        INFO("opacity=" << opacity);
        CHECK(maxDelta <= 1);
    }
}

TEST_CASE("Erase carves alpha and leaves the colour alone") {
    BrushParams p = basePaint();
    p.strokeMode = StrokeMode::Erase;
    p.opacity = 1.0;
    p.color = Color8{255, 0, 0, 255}; // must be ignored entirely

    const Image e = paint(p, crossingPath(), Color8{10, 200, 90, 255});
    const Color8 c = pixel(e, 48, 48);
    CHECK(c.a == 0);   // fully erased where the stroke is solid
    CHECK(c.r == 10);  // ... and the RGB is untouched, not pulled toward the
    CHECK(c.g == 200); //     paint colour and not premultiplied to black
    CHECK(c.b == 90);
    CHECK(pixel(e, 5, 5) == Color8{10, 200, 90, 255}); // outside the stroke: pristine
}

TEST_CASE("Erase honours the opacity ceiling in Wash and passes it in Buildup") {
    // The eraser's opacity is a ceiling on how much it removes, exactly as a paint stroke's is on
    // how much it adds -- and Buildup lifts that ceiling the same way.
    BrushParams wash = basePaint();
    wash.strokeMode = StrokeMode::Erase;
    wash.opacity = 0.5;
    BrushParams build = wash;
    build.paintMode = PaintMode::Buildup;

    const Image w = paint(wash, crossingPath(), Color8{10, 200, 90, 255});
    const Image b = paint(build, crossingPath(), Color8{10, 200, 90, 255});

    CHECK(pixel(w, 48, 48).a == 128); // half erased, however often the stroke crosses itself
    CHECK(pixel(b, 48, 48).a < 55);   // ... but Buildup keeps carving
    CHECK(pixel(w, 48, 48).g == 200); // colour preserved in both
    CHECK(pixel(b, 48, 48).g == 200);
}

TEST_CASE("Erase ignores the paint colour's own alpha") {
    // strokeAlphaCap() drops `color.a` for an eraser: a swatch that happens to be semi-transparent
    // must not make the eraser weaker. (For a Paint stroke it still scales the ceiling.)
    BrushParams p = basePaint();
    p.strokeMode = StrokeMode::Erase;
    p.opacity = 1.0;
    p.color = Color8{0, 0, 0, 7}; // nearly transparent, and entirely irrelevant

    const Image e = paint(p, crossingPath(), Color8{10, 200, 90, 255});
    CHECK(pixel(e, 48, 48).a == 0); // erased completely regardless
}

TEST_CASE("Multiply darkens against the backdrop where Normal would not") {
    // A mid-grey stroke over a mid-grey backdrop: Normal replaces, Multiply multiplies.
    BrushParams normal = basePaint();
    normal.opacity = 1.0;
    normal.color = Color8{128, 128, 128, 255};
    BrushParams mult = normal;
    mult.blendMode = BlendMode::Multiply;

    const Image n = paint(normal, crossingPath(), Color8{128, 128, 128, 255});
    const Image m = paint(mult, crossingPath(), Color8{128, 128, 128, 255});

    CHECK(pixel(n, 48, 48).r == 128); // source-over with the same colour: unchanged
    // 0.502 * 0.502 = 0.252 -> 64. The backdrop is opaque, so the blend is applied at full weight.
    CHECK(pixel(m, 48, 48).r == 64);
    CHECK(pixel(m, 48, 48).a == 255);
}

TEST_CASE("a blend mode fades out over a transparent backdrop") {
    // W3C: the blended colour is weighted into the source by the BACKDROP's alpha. Over nothing,
    // a Multiply stroke must deposit its own colour rather than multiplying against black.
    BrushParams mult = basePaint();
    mult.opacity = 1.0;
    mult.blendMode = BlendMode::Multiply;
    mult.color = Color8{200, 100, 50, 255};

    const Image m = paint(mult, crossingPath(), Color8{0, 0, 0, 0});
    const Color8 c = pixel(m, 48, 48);
    CHECK(c.a == 255);
    CHECK(c.r == 200); // not multiplied down to 0 by an absent backdrop
    CHECK(c.g == 100);
    CHECK(c.b == 50);
}

TEST_CASE("a non-separable blend mode reaches the brush") {
    // Hue takes the source's hue and the backdrop's luminosity/saturation -- it runs through
    // blendNonSeparable rather than the per-channel path, so it exercises the other branch.
    BrushParams hue = basePaint();
    hue.opacity = 1.0;
    hue.blendMode = BlendMode::Hue;
    hue.color = Color8{0, 0, 255, 255}; // a pure blue hue

    const Image h = paint(hue, crossingPath(), Color8{200, 60, 60, 255}); // a desaturated red
    const Color8 c = pixel(h, 48, 48);
    CHECK(c.a == 255);
    CHECK(c.b > c.r); // the hue swung toward blue...
    CHECK(c.b > c.g);
    CHECK(c != Color8{0, 0, 255, 255}); // ... without simply becoming the source colour
}

TEST_CASE("restore reverts a Buildup erase with a blend mode set") {
    // The composite path has four independent switches; restore() reads only the coverage buffer
    // and the base snapshot, so it must be blind to all of them.
    BrushParams p = basePaint();
    p.paintMode = PaintMode::Buildup;
    p.strokeMode = StrokeMode::Erase;
    p.blendMode = BlendMode::Multiply; // ignored by Erase, but set anyway
    p.opacity = 0.9;

    Image img(96, 96);
    img.fill(Color8{33, 44, 55, 200});
    const Image pristine = img;

    BrushEngine eng;
    eng.begin(96, 96, img, p, BrushDynamics{}, crossingPath().front());
    for (std::size_t i = 1; i < crossingPath().size(); ++i) {
        eng.extendTo(crossingPath()[i]);
        eng.composite(); // frame by frame, as the canvas does
    }
    REQUIRE(img.rgba != pristine.rgba); // it really erased
    eng.restore();
    eng.end();
    CHECK(img.rgba == pristine.rgba);
}
