#include <doctest/doctest.h>

#include "common/image.hpp"
#include "core/brush/bitmap_tip.hpp"
#include "core/brush/brush_engine.hpp"
#include "core/brush/brush_tip.hpp"
#include "core/brush/mask_generator.hpp"

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

// The §6.2 option pipeline, THROUGH THE ENGINE: sensors -> curves -> Dab -> stamped pixels. The pure
// resolution step is unit-tested in test_brush_dab.cpp; what these pin is the wiring -- that the dab
// is evaluated against the state that belongs to IT, exactly once, and that the tip finally has a
// shape.
namespace {

using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::common::Vec2;
using mosaic::core::brush::BrushDynamics;
using mosaic::core::brush::BrushEngine;
using mosaic::core::brush::BrushOptions;
using mosaic::core::brush::BrushParams;
using mosaic::core::brush::CurveOption;
using mosaic::core::brush::CurveOptionData;
using mosaic::core::brush::Sensor;
using mosaic::core::brush::SensorId;
using mosaic::core::brush::StrokeInput;

std::uint8_t alphaAt(const Image& img, int x, int y) {
    return img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4 + 3];
}

// One option: `sensor` with an identity curve, at full strength, checked.
[[nodiscard]] CurveOption option(const char* name, SensorId sensor, int rangeLength = 30) {
    CurveOptionData d;
    d.name = name;
    d.checked = true;
    d.strength = 1.0;
    Sensor s = Sensor::withDefaults(sensor);
    s.range.periodic = false;
    s.range.length = rangeLength;
    d.sensors.sensors = {s};
    return CurveOption(d);
}

[[nodiscard]] StrokeInput at(double x, double y, double pressure = 1.0, std::uint64_t timeUs = 0) {
    StrokeInput in;
    in.pos = {x, y};
    in.pressure = pressure;
    in.timeUs = timeUs;
    return in;
}

// Paint a whole stroke and hand back the composited image. end() flushes the tail span -- the walk
// lags the sample stream by one sample, so a stroke that is never flushed is missing its last span.
[[nodiscard]] Image paint(const BrushParams& p, const BrushDynamics& d,
                          const std::vector<StrokeInput>& path, std::uint32_t w = 128,
                          std::uint32_t h = 64) {
    Image img(w, h);
    BrushEngine eng;
    eng.begin(w, h, img, p, d, path.front());
    for (std::size_t i = 1; i < path.size(); ++i)
        eng.extendTo(path[i]);
    eng.end();
    eng.composite();
    return img;
}

// A black, hard, fully-opaque tip -- so a pixel's ALPHA reads back the dab's flow directly.
[[nodiscard]] BrushParams readableParams() {
    BrushParams p;
    p.diameter = 8.0;
    p.hardness = 1.0;
    p.spacing = 1.0; // dabs land tangent to each other: a dab's centre pixel is covered by it alone
    p.flow = 1.0;
    p.opacity = 1.0;
    p.color = Color8{0, 0, 0, 255};
    return p;
}

} // namespace

TEST_CASE("options: a dab reads the state that belonged to ITS span, not the lookahead's") {
    // ⚠ THE BUG THIS ARC EXISTED TO PREVENT. The dab walk lags the sample stream by one sample -- a
    // curve THROUGH a sample has to know where the path goes next -- so by the time a span is
    // stamped, the live stroke state has already advanced past it. Evaluate a dab against the live
    // state and every distance/speed/time/heading sensor reads ONE SAMPLE INTO THE FUTURE.
    //
    // Flow driven by `distance` makes that visible in the alpha: a dab `d` px along the stroke must
    // deposit d/80 of its paint. The stroke runs east from x=10 in 20 px steps, so a dab in the span
    // [10 -> 30] would read the state at x=50 (distance 40, flow 0.5) if the lag were not closed --
    // against the 0.1..0.25 it should be laying. Nothing subtle about the difference.
    BrushParams p = readableParams();
    auto o = std::make_shared<BrushOptions>();
    o->flow = option("Flow", SensorId::Distance, /*length=*/80);
    p.options = o;

    const Image img = paint(p, BrushDynamics{},
                            {at(10.0, 20.0), at(30.0, 20.0), at(50.0, 20.0), at(70.0, 20.0),
                             at(90.0, 20.0)});

    // Dabs land every 8 px (diameter 8 x spacing 1.0) from the press at x=10, so the dab at x is
    // `x - 10` px along the stroke and deposits (x - 10) / 80.
    for (const int x : {18, 26, 34, 42, 50, 58, 66, 74}) {
        const double expected = static_cast<double>(x - 10) / 80.0;
        const double got = alphaAt(img, x, 20) / 255.0;
        CHECK(got == doctest::Approx(expected).epsilon(0.03));
    }

    // And the ramp really is a ramp -- the first dabs are nearly bare, the last nearly solid. (A dab
    // reading the lookahead would have painted the early ones far too dark.)
    CHECK(alphaAt(img, 18, 20) < 40);
    CHECK(alphaAt(img, 74, 20) > 190);
}

TEST_CASE("options: the dab counter advances exactly once per dab") {
    // The trap the walk sidesteps by storing the RESOLVED diameter rather than re-deriving it: if the
    // spacing cadence re-ran the option pipeline to find the next step, every dab would be evaluated
    // twice -- drawing two `fuzzy` numbers and advancing the dab counter twice.
    //
    // `fade` ramps over DABS, so it reads that directly: dab n must deposit n/20, not 2n/20.
    BrushParams p = readableParams();
    auto o = std::make_shared<BrushOptions>();
    o->flow = option("Flow", SensorId::Fade, /*length=*/20);
    p.options = o;

    const Image img = paint(p, BrushDynamics{}, {at(10.0, 20.0), at(40.0, 20.0), at(70.0, 20.0),
                                                 at(100.0, 20.0)});

    // The press lays dab 0 at x=10; the walk lays dab n at x = 10 + 8n.
    for (int n = 1; n <= 8; ++n) {
        const double expected = static_cast<double>(n) / 20.0;
        const double got = alphaAt(img, 10 + 8 * n, 20) / 255.0;
        CHECK(got == doctest::Approx(expected).epsilon(0.03));
    }
}

TEST_CASE("options: a Size option reproduces the pressure bool, byte for byte") {
    // BrushDynamics::sizeFromPressure IS a Size option with a pressure sensor and an identity curve.
    // Two roads to the same dab, and they must arrive at the same bytes -- including the SPACING
    // cadence, which keys off the resolved size, so a divergence there would show up as dabs in
    // different places rather than merely a different width.
    const std::vector<StrokeInput> path = {at(20.0, 32.0, 0.2), at(50.0, 32.0, 0.6),
                                           at(80.0, 32.0, 1.0), at(110.0, 32.0, 0.4)};

    BrushParams viaBool = readableParams();
    viaBool.hardness = 0.5;
    viaBool.diameter = 20.0;
    viaBool.spacing = 0.15;
    BrushDynamics d;
    d.sizeFromPressure = true;
    const Image a = paint(viaBool, d, path);

    BrushParams viaOption = viaBool;
    auto o = std::make_shared<BrushOptions>();
    o->size = option("Size", SensorId::Pressure);
    viaOption.options = o;
    const Image b = paint(viaOption, BrushDynamics{}, path);

    CHECK(a.rgba == b.rgba);
    // ... and the stroke is not accidentally empty, which would make the comparison vacuous.
    CHECK(alphaAt(a, 50, 32) > 0);
}

TEST_CASE("options: a ratio gives the tip an ellipse") {
    // The tip finally has a SHAPE. `ratio` is height/width, so 0.4 on a 40 px tip is 40 wide and 16
    // tall -- a nib, lying flat.
    BrushParams p;
    p.diameter = 40.0;
    p.ratio = 0.4;
    p.hardness = 1.0;
    p.color = Color8{0, 0, 0, 255};

    Image img(128, 64);
    BrushEngine eng;
    eng.begin(128, 64, img, p, BrushDynamics{}, at(64.0, 32.0));
    eng.end();
    eng.composite();

    // ⚠ THE HALF-PIXEL TRAP: the pixel CONTAINING a point is floor(p), never lround(p). The dab is
    // centred on the corner (64, 32), so its centre pixel is (64, 32) and it reaches 20 px sideways
    // and 8 px vertically.
    CHECK(alphaAt(img, 64, 32) == 255); // the core
    CHECK(alphaAt(img, 78, 32) > 0);    // 14 px out along the LONG axis: still inside
    CHECK(alphaAt(img, 84, 32) == 0);   // 20 px out: past the rim
    CHECK(alphaAt(img, 64, 37) > 0);    // 5 px out along the SHORT axis: inside
    CHECK(alphaAt(img, 64, 43) == 0);   // 11 px out: well past a rim that is only 8 px away

    // ⚠ AND A CORNER OF THE DAB'S BOUNDING BOX -- inside the box, outside the ELLIPSE. Without this
    // the test cannot see a falloff that ignores the ratio at all: the bounding box is itself clipped
    // to the ellipse's extent, so a circular falloff inside an elliptical box still reads as an
    // ellipse from every point on the two axes. The backstop masks the check it is backing up.
    // (18, 7) from the centre: the box reaches 21 x 9, the ellipse only to sqrt(18^2 + (7/0.4)^2) =
    // 25 px of tip-frame radius, well past the 20 px rim.
    CHECK(alphaAt(img, 82, 39) == 0);

    // A circle at the same diameter reaches just as far DOWN as it does sideways -- which is exactly
    // what the ellipse above does not.
    p.ratio = 1.0;
    Image round(128, 64);
    BrushEngine circ;
    circ.begin(128, 64, round, p, BrushDynamics{}, at(64.0, 32.0));
    circ.end();
    circ.composite();
    CHECK(alphaAt(round, 64, 43) > 0);
    CHECK(alphaAt(round, 78, 32) > 0);
}

TEST_CASE("options: an angle turns the tip, and a quarter turn swaps its axes") {
    // A tip rotated a quarter turn is the same tip standing on end. The two images must therefore be
    // each other's transpose about the dab centre -- which is a far stronger claim than "it looks
    // different", and it is the one that catches a rotation applied in the wrong direction or in the
    // document's frame instead of the tip's.
    BrushParams flat;
    flat.diameter = 40.0;
    flat.ratio = 0.4;
    flat.hardness = 0.9;
    flat.color = Color8{0, 0, 0, 255};

    BrushParams upright = flat;
    upright.angleRad = 3.14159265358979323846 * 0.5;

    Image a(129, 129);
    Image b(129, 129);
    BrushEngine e1;
    e1.begin(129, 129, a, flat, BrushDynamics{}, at(64.5, 64.5));
    e1.end();
    e1.composite();
    BrushEngine e2;
    e2.begin(129, 129, b, upright, BrushDynamics{}, at(64.5, 64.5));
    e2.end();
    e2.composite();

    // Centred on the middle of pixel (64,64), so the transpose about that pixel is exact.
    int compared = 0;
    for (int y = 40; y < 89; ++y) {
        for (int x = 40; x < 89; ++x) {
            const int tx = 64 + (y - 64);
            const int ty = 64 + (x - 64);
            CHECK(static_cast<int>(alphaAt(b, tx, ty)) == static_cast<int>(alphaAt(a, x, y)));
            ++compared;
        }
    }
    CHECK(compared == 49 * 49);
    CHECK(alphaAt(a, 78, 64) > 0); // and the flat one really is wide ...
    CHECK(alphaAt(a, 64, 78) == 0);
    CHECK(alphaAt(b, 64, 78) > 0); // ... while the upright one is tall
    CHECK(alphaAt(b, 78, 64) == 0);
}

TEST_CASE("options: a rotation option turns the dab, a ratio option squashes it") {
    // The two shape options, through the engine rather than through evaluateDab: a Rotation driven by
    // `drawingangle` makes the nib follow the stroke, which is what 14 of the 82 shipped presets do.
    BrushParams p;
    p.diameter = 30.0;
    p.ratio = 0.3;
    p.hardness = 1.0;
    p.spacing = 0.5;
    p.color = Color8{0, 0, 0, 255};
    auto o = std::make_shared<BrushOptions>();
    o->rotation = option("Rotation", SensorId::DrawingAngle);
    p.options = o;

    // Two strokes of the same length, one east and one south. A nib that follows the heading paints a
    // ribbon of the SAME width in both -- a nib that ignores it paints a wide ribbon east and a thin
    // one south.
    const Image east = paint(p, BrushDynamics{}, {at(20.0, 32.0), at(50.0, 32.0), at(80.0, 32.0),
                                                  at(108.0, 32.0)});
    Image south(64, 128);
    {
        BrushEngine eng;
        eng.begin(64, 128, south, p, BrushDynamics{},
                  at(32.0, 20.0));
        eng.extendTo(at(32.0, 50.0));
        eng.extendTo(at(32.0, 80.0));
        eng.extendTo(at(32.0, 108.0));
        eng.end();
        eng.composite();
    }

    // Thickness across the ribbon, at its middle.
    const auto acrossEast = [&](int x) {
        int n = 0;
        for (int y = 0; y < 64; ++y)
            n += alphaAt(east, x, y) > 0 ? 1 : 0;
        return n;
    };
    const auto acrossSouth = [&](int y) {
        int n = 0;
        for (int x = 0; x < 64; ++x)
            n += alphaAt(south, x, y) > 0 ? 1 : 0;
        return n;
    };
    CHECK(acrossEast(64) > 0);
    CHECK(acrossEast(64) == acrossSouth(64));
}

TEST_CASE("options: a fuzzy-driven stroke replays from its seed") {
    // `fuzzy` is the 2nd-most-used sensor (23 of 82). Evaluating a dab DRAWS from the stroke's random
    // stream, so a stroke is only replayable because the seed is a parameter and never a clock read --
    // which is what golden images, the editor's live preview and undo/redo of a scattered stroke all
    // rest on.
    BrushParams p = readableParams();
    p.diameter = 16.0;
    auto o = std::make_shared<BrushOptions>();
    o->size = option("Size", SensorId::Fuzzy);
    p.options = o;
    p.seed = 0xFEED'BEEFULL;

    const std::vector<StrokeInput> path = {at(20.0, 32.0), at(50.0, 32.0), at(80.0, 32.0),
                                           at(108.0, 32.0)};
    const Image a = paint(p, BrushDynamics{}, path);
    const Image b = paint(p, BrushDynamics{}, path);
    CHECK(a.rgba == b.rgba); // same seed, same dabs

    p.seed = 0x1234'5678ULL;
    const Image c = paint(p, BrushDynamics{}, path);
    CHECK(c.rgba != a.rgba); // ... and a different seed really does scatter differently
}

TEST_CASE("options: a tall tip is not clipped to a circle's box") {
    // `ratio` is height/width, so it can exceed 1: a tip taller than it is wide. The dab's bounding
    // box has to follow the tip's ACTUAL semi-axes -- a box sized from the width alone is generous for
    // a squashed tip (a few wasted pixels, no harm) but CLIPS a tall one, shearing its ends flat.
    BrushParams p;
    p.diameter = 20.0;
    p.ratio = 2.0; // 20 wide, 40 tall
    p.hardness = 1.0;
    p.color = Color8{0, 0, 0, 255};

    Image img(96, 96);
    BrushEngine eng;
    eng.begin(96, 96, img, p, BrushDynamics{}, at(48.0, 48.0));
    eng.end();
    eng.composite();

    CHECK(alphaAt(img, 48, 48) == 255);
    CHECK(alphaAt(img, 48, 62) > 0);  // 14 px DOWN: inside a tip that reaches 20 px, and OUTSIDE the
                                      // 10 px box a width-sized bound would have given it
    CHECK(alphaAt(img, 48, 69) == 0); // 21 px down: past the rim
    CHECK(alphaAt(img, 56, 48) > 0);  // 8 px across: inside
    CHECK(alphaAt(img, 59, 48) == 0); // 11 px across: past a rim only 10 px away
}

TEST_CASE("options: on a CURVE, a dab's heading is the curve's tangent where the dab sits") {
    // ⚠ A STRAIGHT STROKE PROVES NOTHING HERE, AND THAT IS THE WHOLE TRAP. On a straight span every
    // sample carries the same heading, so blending the two bracketing samples' headings and reading
    // the curve's local tangent give the identical answer -- the two implementations are literally
    // indistinguishable. The stroke has to CURVE. (A mutant that dropped the tangent survived every
    // straight-line test in this file.)
    //
    // A thin nib whose Rotation follows `drawingangle` lies ALONG the path, so the ribbon it sweeps is
    // only as thick as the nib is short. Blend the chord directions instead of reading the tangent and
    // every dab in a span is canted off the path by half a span's worth of turn, which fattens the
    // ribbon by the nib's LENGTH times the sine of that error: measured, 4.25 px becomes 5.75.
    const double kPi = 3.14159265358979323846;

    BrushParams p;
    p.diameter = 30.0;
    p.ratio = 0.15; // a nib: 30 long, 4.5 across
    p.hardness = 1.0;
    p.spacing = 0.1;
    p.color = Color8{0, 0, 0, 255};
    auto o = std::make_shared<BrushOptions>();
    o->rotation = option("Rotation", SensorId::DrawingAngle);
    p.options = o;

    // Thickness of the painted ribbon across the path at `(mx, my)`, probed along the unit normal
    // `(nx, ny)` in quarter-pixel steps.
    const auto thickness = [](const Image& img, double mx, double my, double nx, double ny) {
        int n = 0;
        for (double s = -20.0; s <= 20.0; s += 0.25) {
            const int x = static_cast<int>(std::floor(mx + nx * s));
            const int y = static_cast<int>(std::floor(my + ny * s));
            if (x < 0 || y < 0 || x >= static_cast<int>(img.width) ||
                y >= static_cast<int>(img.height))
                continue;
            n += alphaAt(img, x, y) > 0 ? 1 : 0;
        }
        return n;
    };

    // The calibration: the SAME nib on a straight stroke lies exactly along the path, so the ribbon it
    // sweeps is precisely the nib's short axis. That is the width a correctly-oriented nib on a curve
    // has to sweep too -- so the test needs no magic number, only this one.
    const Image line = paint(p, BrushDynamics{},
                             {at(20.0, 80.0), at(60.0, 80.0), at(100.0, 80.0), at(140.0, 80.0)}, 160,
                             160);
    const int straight = thickness(line, 80.0, 80.0, 0.0, 1.0);
    CHECK(straight >= 15); // ~4.5 px at quarter-pixel steps; not a vacuous zero
    CHECK(straight <= 21);

    // A quarter circle, centre (20,100), radius 60: it starts at the top heading due east and ends at
    // the right heading due south. Six spans, so each turns 15 degrees.
    const double cx = 20.0;
    const double cy = 100.0;
    const double R = 60.0;
    const auto ptAt = [&](double phi) {
        return StrokeInput{{cx + R * std::sin(phi), cy - R * std::cos(phi)}, 1.0};
    };

    Image img(160, 160);
    BrushEngine eng;
    eng.begin(160, 160, img, p, BrushDynamics{}, ptAt(0.0));
    for (int i = 1; i <= 6; ++i)
        eng.extendTo(ptAt(kPi * 0.5 * i / 6.0));
    eng.end();
    eng.composite();

    // Probed RADIALLY at the arc's midpoint -- where a canted nib is at its worst, and where the curve
    // sits furthest from any of its chords.
    const double phi = kPi * 0.25;
    const int curved = thickness(img, cx + R * std::sin(phi), cy - R * std::cos(phi), std::sin(phi),
                                 -std::cos(phi));
    CHECK(curved > 0);
    CHECK(curved <= straight + 2); // it tracks the curve; a canted nib overshoots this by half again
}

// ------------------------------------------------------------------------------------------------
// §6.6d THROUGH THE ENGINE: scatter moves dabs, mirror flips tips, and both replay from the seed.

namespace {

using mosaic::core::brush::BitmapTip;
using mosaic::core::brush::makeTip;
using mosaic::core::brush::MirrorOption;
using mosaic::core::brush::ScatterOption;
using mosaic::core::brush::TipApplication;
using mosaic::core::brush::TipFrame;
using mosaic::core::brush::TipSourceKind;

// Scatter at `strength` on both axes, pressure-driven (identity curve): at pressure 1 the sensor
// value IS the strength, so the jitter amplitude is predictable while the draws stay random.
[[nodiscard]] ScatterOption scatterBoth(double strength, bool checked = true) {
    CurveOptionData d;
    d.name = "Scatter";
    d.checked = checked;
    d.strength = strength;
    d.strengthMax = 5.0;
    d.sensors.sensors = {Sensor::withDefaults(SensorId::Pressure)};
    return ScatterOption{CurveOption(d), true, true};
}

// A pressure-driven Mirror: at pressure 1 its value is 1 >= 0.5, so EVERY dab flips -- the
// deterministic form, for byte-level assertions.
[[nodiscard]] MirrorOption mirrorAlways(bool horizontal, bool vertical) {
    CurveOptionData d;
    d.name = "Mirror";
    d.checked = true;
    d.strength = 1.0;
    d.sensors.sensors = {Sensor::withDefaults(SensorId::Pressure)};
    return MirrorOption{CurveOption(d), horizontal, vertical};
}

// An 8x8 asymmetric mask frame: the left half paints, the right half does not. Mirroring it is
// visible, which no procedural tip can offer -- every generator's shape is flip-symmetric in its
// own frame.
[[nodiscard]] TipFrame lopsidedFrame() {
    TipFrame f;
    f.width = 8;
    f.height = 8;
    f.rgba.resize(8 * 8 * 4);
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            const std::uint8_t grey = x < 4 ? 0 : 255; // dark half paints (mask semantics)
            const std::size_t p = (y * 8 + x) * 4;
            f.rgba[p] = grey;
            f.rgba[p + 1] = grey;
            f.rgba[p + 2] = grey;
            f.rgba[p + 3] = 255;
        }
    }
    return f;
}

[[nodiscard]] TipFrame flippedH(const TipFrame& src) {
    TipFrame f = src;
    for (std::uint32_t y = 0; y < src.height; ++y) {
        for (std::uint32_t x = 0; x < src.width; ++x) {
            const std::size_t a = (y * src.width + x) * 4;
            const std::size_t b = (y * src.width + (src.width - 1 - x)) * 4;
            for (int c = 0; c < 4; ++c)
                f.rgba[a + c] = src.rgba[b + c];
        }
    }
    return f;
}

} // namespace

TEST_CASE("options: scatter throws dabs OFF the path, and replays from its seed") {
    // A straight horizontal stroke with an 8 px tip covers a band around y=32. With scatter at
    // strength 2 (both axes), dabs land up to 2 tip-extents away -- pixels no on-path dab could
    // reach. The walk itself must not move: the CADENCE is the path's, only the dabs jitter (the
    // reference scatters the dab position after the spacing was decided).
    //
    // Ratio 2 on purpose: the analytic (tipless) path's larger extent is diameter x ratio = 16,
    // not the diameter -- so a fallback that forgets the ratio arm jitters at half the amplitude
    // and moves these bytes.
    BrushParams p = readableParams();
    p.seed = 77;
    p.ratio = 2.0;
    const std::vector<StrokeInput> path = {at(20.0, 32.0), at(60.0, 32.0), at(108.0, 32.0)};

    const Image plain = paint(p, BrushDynamics{}, path);

    auto o = std::make_shared<BrushOptions>();
    o->scatter = scatterBoth(2.0);
    p.options = o;
    const Image scattered = paint(p, BrushDynamics{}, path);

    // Somewhere off the plain stroke's band, scattered paint landed.
    bool offBand = false;
    for (std::uint32_t y = 0; y < scattered.height && !offBand; ++y) {
        if (y >= 32 - 10 && y <= 32 + 10)
            continue; // the plain band (tip is 8 wide x 16 tall), with a phase margin
        for (std::uint32_t x = 0; x < scattered.width; ++x) {
            if (scattered.rgba[(y * scattered.width + x) * 4 + 3] != 0) {
                offBand = true;
                break;
            }
        }
    }
    CHECK(offBand);
    CHECK(scattered.rgba != plain.rgba);

    // Same seed, same stroke: the same bytes. A scattered stroke is still a REPLAYABLE stroke.
    const Image again = paint(p, BrushDynamics{}, path);
    CHECK(again.rgba == scattered.rgba);

    // A different seed scatters differently -- the jitter really is the stroke's random stream.
    p.seed = 78;
    const Image other = paint(p, BrushDynamics{}, path);
    CHECK(other.rgba != scattered.rgba);
}

TEST_CASE("options: an INERT scatter is byte-identical to no scatter at all") {
    // Unchecked, or axis-less: no draw, no offset -- the §6.2 identity, extended to the stream.
    BrushParams p = readableParams();
    p.seed = 5;
    const std::vector<StrokeInput> path = {at(20.0, 32.0), at(108.0, 32.0)};
    const Image plain = paint(p, BrushDynamics{}, path);

    auto unchecked = std::make_shared<BrushOptions>();
    unchecked->scatter = scatterBoth(2.0, /*checked=*/false);
    p.options = unchecked;
    CHECK(paint(p, BrushDynamics{}, path).rgba == plain.rgba);

    auto axisless = std::make_shared<BrushOptions>();
    axisless->scatter = scatterBoth(2.0);
    axisless->scatter->axisX = false;
    axisless->scatter->axisY = false;
    p.options = axisless;
    CHECK(paint(p, BrushDynamics{}, path).rgba == plain.rgba);
}

TEST_CASE("options: mirrorH IS painting with the flipped tip -- the conjugation, in bytes") {
    // The transcription's key equivalence (dab.hpp applyMirror): the reference mirrors by negating
    // the rotation and flipping the rendered raster; this pipeline flips the tip in its own frame
    // before rotating. A flip conjugates a rotation into its inverse, so the two agree -- which
    // collapses to something directly testable: a stroke whose Mirror flips EVERY dab must equal,
    // byte for byte, the same stroke laid with a pre-flipped tip and no option at all. The 8x8
    // frame at diameter 8 resamples 1:1, so not even a filter tap differs.
    const TipFrame frame = lopsidedFrame();

    BrushParams p = readableParams();
    p.tip = makeTip(std::make_shared<const BitmapTip>(
        BitmapTip{{frame}, TipApplication::AlphaMask, TipSourceKind::Mask}));
    auto o = std::make_shared<BrushOptions>();
    o->mirror = mirrorAlways(/*horizontal=*/true, /*vertical=*/false);
    p.options = o;
    const std::vector<StrokeInput> path = {at(20.0, 32.0), at(100.0, 32.0)};
    const Image mirrored = paint(p, BrushDynamics{}, path);

    BrushParams q = readableParams();
    q.tip = makeTip(std::make_shared<const BitmapTip>(
        BitmapTip{{flippedH(frame)}, TipApplication::AlphaMask, TipSourceKind::Mask}));
    const Image preflipped = paint(q, BrushDynamics{}, path);

    REQUIRE(mirrored.rgba.size() == preflipped.rgba.size());
    CHECK(mirrored.rgba == preflipped.rgba);

    // And the asymmetry is real -- the unmirrored stroke differs, or this case pins nothing.
    BrushParams r = readableParams();
    r.tip = makeTip(std::make_shared<const BitmapTip>(
        BitmapTip{{frame}, TipApplication::AlphaMask, TipSourceKind::Mask}));
    CHECK(paint(r, BrushDynamics{}, path).rgba != mirrored.rgba);
}

TEST_CASE("options: a fuzzy mirror flips SOME dabs and replays from its seed") {
    const TipFrame frame = lopsidedFrame();
    BrushParams p = readableParams();
    p.seed = 99;
    p.tip = makeTip(std::make_shared<const BitmapTip>(
        BitmapTip{{frame}, TipApplication::AlphaMask, TipSourceKind::Mask}));

    auto o = std::make_shared<BrushOptions>();
    CurveOptionData d;
    d.name = "Mirror";
    d.checked = true;
    d.strength = 1.0;
    d.sensors.sensors = {Sensor::withDefaults(SensorId::Fuzzy)};
    o->mirror = MirrorOption{CurveOption(d), true, false};
    p.options = o;

    const std::vector<StrokeInput> path = {at(16.0, 32.0), at(112.0, 32.0)};
    const Image coin = paint(p, BrushDynamics{}, path);
    CHECK(paint(p, BrushDynamics{}, path).rgba == coin.rgba); // the replay contract

    // A fair coin over 13 dabs flips at least one and leaves at least one: the stroke can match
    // neither the never-flipped nor the always-flipped stroke. (Deterministic under the seed.)
    BrushParams never = p;
    never.options = nullptr;
    const Image plain = paint(never, BrushDynamics{}, path);
    BrushParams always = p;
    auto ao = std::make_shared<BrushOptions>();
    ao->mirror = mirrorAlways(true, false);
    always.options = ao;
    const Image full = paint(always, BrushDynamics{}, path);
    CHECK(coin.rgba != plain.rgba);
    CHECK(coin.rgba != full.rgba);
}

TEST_CASE("options: scatter golden -- a rotated nib's jitter amplitude is the MASK's own extents") {
    // The reference feeds its jitter formula the mask-raster dims of the dab as it will stamp --
    // the ROTATED axis-aligned box -- not the tip's authored extents and not the dab's diameter.
    // A 16x4 nib at 45 degrees spans ~14.14 px either way, so an engine that reads the diameter
    // (16) or the unrotated larger extent (16) jitters ~13% too far and every draw lands
    // elsewhere. A hash pins that: the three sources cannot collide.
    //
    // Blessed 2026-07-14, debug and release identical.
    TipFrame nib;
    nib.width = 16;
    nib.height = 4;
    nib.rgba.assign(16 * 4 * 4, 0);
    for (std::size_t i = 3; i < nib.rgba.size(); i += 4)
        nib.rgba[i] = 255; // grey 0, alpha 255: a solid mask bar

    BrushParams p = readableParams();
    p.seed = 1234;
    p.diameter = 16.0;
    p.angleRad = 0.78539816339744830961; // pi/4
    p.tip = makeTip(std::make_shared<const BitmapTip>(
        BitmapTip{{nib}, TipApplication::AlphaMask, TipSourceKind::Mask}));
    auto o = std::make_shared<BrushOptions>();
    o->scatter = scatterBoth(1.0);
    p.options = o;

    const Image img =
        paint(p, BrushDynamics{}, {at(24.0, 32.0), at(64.0, 32.0), at(104.0, 32.0)});

    std::uint64_t h = 1469598103934665603ull;
    for (const std::uint8_t b : img.rgba) {
        h ^= b;
        h *= 1099511628211ull;
    }
    CHECK(h == 10877552810909508295ull);
}

TEST_CASE("options: scatter golden -- the tipless analytic envelope carries the RATIO arm") {
    // The no-tip fallback's larger extent is diameter x ratio when ratio > 1 -- the same envelope
    // the spacing cadence reads. An engine that forgets the ratio arm jitters this stroke at half
    // the amplitude, and the hash moves. Blessed 2026-07-14, debug and release identical.
    BrushParams p = readableParams();
    p.seed = 4321;
    p.ratio = 2.0;
    auto o = std::make_shared<BrushOptions>();
    o->scatter = scatterBoth(1.0);
    p.options = o;

    const Image img =
        paint(p, BrushDynamics{}, {at(24.0, 32.0), at(64.0, 32.0), at(104.0, 32.0)});

    std::uint64_t h = 1469598103934665603ull;
    for (const std::uint8_t b : img.rgba) {
        h ^= b;
        h *= 1099511628211ull;
    }
    CHECK(h == 13035665734000226876ull);
}

// ------------------------------------------------------------------------------------------------
// §6.6e: Sharpness -- the mask alpha THRESHOLD and the pixel-grid coordinate SNAP. The threshold is
// pinned pure (exact integer arithmetic) and through the engine (a soft rim goes 1-bit); the snap is
// pinned pure in test_brush_dab.cpp and end-to-end here (a snapped fractional dab equals the dab laid
// at the snapped integer position).

namespace {

using mosaic::core::brush::MaskFalloff;
using mosaic::core::brush::MaskGeneratorParams;
using mosaic::core::brush::MaskShape;
using mosaic::core::brush::SharpnessOption;
using mosaic::core::brush::sharpnessThreshold;

// A hard round procedural tip. Even at hardness 1 its edge keeps a ~0.75 px anti-aliased rim, so the
// composited stroke carries intermediate alpha -- exactly what the Sharpness threshold hardens.
[[nodiscard]] std::shared_ptr<const mosaic::core::brush::BrushTip> hardRoundTip() {
    MaskGeneratorParams g;
    g.shape = MaskShape::Circle;
    g.falloff = MaskFalloff::Default;
    g.hFade = g.vFade = 1.0;
    return mosaic::core::brush::makeTip(g);
}

// A Sharpness option resolving to a constant value (pressure sensor, identity curve, strength =
// value): at pressure 1 the per-dab value IS `value`, deterministically.
[[nodiscard]] SharpnessOption sharpnessOpt(double value, bool alignOutline, int softness,
                                           bool checked = true) {
    CurveOptionData d;
    d.name = "Sharpness";
    d.checkable = true;
    d.checked = checked;
    d.strength = value;
    d.sensors.sensors = {Sensor::withDefaults(SensorId::Pressure)};
    return SharpnessOption{CurveOption(d), alignOutline, softness};
}

} // namespace

TEST_CASE("sharpnessThreshold: value 1 is a hard 1-bit cut, whatever the softness") {
    // tolerance = (uint32)(255 - 255) = 0: every non-zero pixel goes opaque, zero goes transparent,
    // and the soft band `v <= (100-softness)*0/100 = 0` catches only the zeros -- so softness cannot
    // matter here. This is the pixel-art edge.
    for (const int s : {0, 25, 50, 100}) {
        CHECK(sharpnessThreshold(0, 1.0, s) == 0);
        CHECK(sharpnessThreshold(1, 1.0, s) == 255);
        CHECK(sharpnessThreshold(128, 1.0, s) == 255);
        CHECK(sharpnessThreshold(255, 1.0, s) == 255);
    }
}

TEST_CASE("sharpnessThreshold: value 0.5 cuts at the midpoint; softness opens a keep-band") {
    // tolerance = (uint32)(255 - 127.5) = 127 (truncated). v > 127 -> opaque.
    CHECK(sharpnessThreshold(128, 0.5, 0) == 255);
    CHECK(sharpnessThreshold(127, 0.5, 0) == 0); // softness 0: nothing is kept, so 127 goes transparent
    CHECK(sharpnessThreshold(0, 0.5, 0) == 0);

    // softness 50: the transparent bound is (100-50)*127/100 = 6350/100 = 63 (integer). So <=63
    // vanishes, 64..127 is KEPT verbatim, >127 is opaque.
    CHECK(sharpnessThreshold(63, 0.5, 50) == 0);
    CHECK(sharpnessThreshold(64, 0.5, 50) == 64);
    CHECK(sharpnessThreshold(127, 0.5, 50) == 127);
    CHECK(sharpnessThreshold(128, 0.5, 50) == 255);
}

TEST_CASE("sharpnessThreshold: value 0 erases the dab (tolerance 255)") {
    // tolerance = 255: `v > 255` is never true, and at softness 0 `v <= 255` is always true -- the
    // whole dab is set transparent. The reference does exactly this at a zero sharpness value.
    CHECK(sharpnessThreshold(255, 0.0, 0) == 0);
    CHECK(sharpnessThreshold(128, 0.0, 0) == 0);
    CHECK(sharpnessThreshold(1, 0.0, 0) == 0);
}

TEST_CASE("options: a checked Sharpness hardens the stroke to 1-bit alpha") {
    // A hard round tip still anti-aliases its rim, so a plain stroke carries intermediate alpha. A
    // Sharpness option at value 1 (softness 0) thresholds every dab's mask to 0/255, and with flow and
    // opacity at 1 the composited stroke is then purely 0 or 255 -- the pixel-art look.
    BrushParams p;
    p.diameter = 24.0;
    p.hardness = 1.0;
    p.flow = 1.0;
    p.opacity = 1.0;
    p.color = Color8{0, 0, 0, 255};
    p.tip = hardRoundTip();
    const std::vector<StrokeInput> path = {at(30.0, 32.0), at(60.0, 32.0), at(96.0, 32.0)};

    const Image soft = paint(p, BrushDynamics{}, path);
    bool hasIntermediate = false;
    for (std::size_t i = 3; i < soft.rgba.size() && !hasIntermediate; i += 4)
        if (soft.rgba[i] > 0 && soft.rgba[i] < 255)
            hasIntermediate = true;
    CHECK(hasIntermediate); // the rim really is soft, or the case pins nothing

    auto o = std::make_shared<BrushOptions>();
    o->sharpness = sharpnessOpt(1.0, /*alignOutline=*/false, /*softness=*/0);
    p.options = o;
    const Image hard = paint(p, BrushDynamics{}, path);
    for (std::size_t i = 3; i < hard.rgba.size(); i += 4)
        CHECK((hard.rgba[i] == 0 || hard.rgba[i] == 255));
    CHECK(hard.rgba != soft.rgba);

    // ... and the per-dab VALUE really reaches the threshold: at value 0 the tolerance is 255 and the
    // whole dab is erased, so the stroke paints NOTHING. A draw stuck at 1.0 would lay a hard 1-bit
    // stroke instead -- so this pins that resolveDab feeds the drawn value through, not a constant.
    auto z = std::make_shared<BrushOptions>();
    z->sharpness = sharpnessOpt(0.0, /*alignOutline=*/false, /*softness=*/0);
    p.options = z;
    const Image erased = paint(p, BrushDynamics{}, path);
    bool anyPaint = false;
    for (std::size_t i = 3; i < erased.rgba.size() && !anyPaint; i += 4)
        if (erased.rgba[i] != 0)
            anyPaint = true;
    CHECK_FALSE(anyPaint);
}

TEST_CASE("options: an INERT Sharpness is byte-identical to no sharpness at all") {
    // Unchecked: no threshold, no snap, no draw -- the §6.2 identity. (alignOutline and a fat softness
    // set on purpose: an unchecked option must ignore them.)
    BrushParams p;
    p.diameter = 24.0;
    p.hardness = 1.0;
    p.flow = 1.0;
    p.opacity = 1.0;
    p.color = Color8{0, 0, 0, 255};
    p.tip = hardRoundTip();
    const std::vector<StrokeInput> path = {at(30.0, 32.0), at(96.0, 32.0)};
    const Image plain = paint(p, BrushDynamics{}, path);

    auto o = std::make_shared<BrushOptions>();
    o->sharpness = sharpnessOpt(1.0, /*alignOutline=*/true, /*softness=*/50, /*checked=*/false);
    p.options = o;
    CHECK(paint(p, BrushDynamics{}, path).rgba == plain.rgba);
}

TEST_CASE("options: Sharpness alignOutline snaps the dab to the pixel grid") {
    // The snap moves the centre so the mask's top-left lands on an integer (zero sub-pixel phase). A
    // single hard dab of diameter 8 at a fractional centre (24.3, 24.3), with alignOutline, must equal
    // -- byte for byte -- the same dab laid at the SNAPPED integer centre (24.0, 24.0) with the snap
    // OFF: extent 8, top-left 24.3 - 4 = 20.3 -> round 20 -> centre 24.0. Both still threshold (value
    // 1), so only the snap can move them apart.
    BrushParams base;
    base.diameter = 8.0;
    base.hardness = 1.0;
    base.flow = 1.0;
    base.opacity = 1.0;
    base.color = Color8{0, 0, 0, 255};
    base.tip = hardRoundTip();

    BrushParams snapped = base;
    auto os = std::make_shared<BrushOptions>();
    os->sharpness = sharpnessOpt(1.0, /*alignOutline=*/true, /*softness=*/0);
    snapped.options = os;
    const Image a = paint(snapped, BrushDynamics{}, {at(24.3, 24.3)}, 64, 64);

    BrushParams atGrid = base;
    auto og = std::make_shared<BrushOptions>();
    og->sharpness = sharpnessOpt(1.0, /*alignOutline=*/false, /*softness=*/0); // no snap
    atGrid.options = og;
    const Image b = paint(atGrid, BrushDynamics{}, {at(24.0, 24.0)}, 64, 64);
    CHECK(a.rgba == b.rgba);

    // ... and the snap is what did it: the same fractional dab WITHOUT the snap differs.
    const Image c = paint(atGrid, BrushDynamics{}, {at(24.3, 24.3)}, 64, 64);
    CHECK(c.rgba != a.rgba);
}
