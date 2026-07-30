#include <doctest/doctest.h>

#include "common/image.hpp"
#include "core/brush/brush_engine.hpp"
#include "core/brush/brush_tip.hpp"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

// THE TIP, THROUGH THE ENGINE (docs/brushes.md §6.2). Everything under it -- MaskGenerator,
// BitmapTip, DabMask, DabMaskCache -- was built and unit-tested in Arc A, and nothing stamped
// through any of it: the engine walked its own analytic circle, parameterized by `hardness`. What
// these pin is the wiring, and the four claims that wiring makes:
//
//   1. A NULL tip is not "no falloff" -- it is the analytic circle the engine has always stamped,
//      bit for bit. (Pinned by every golden in the suite, which is why none of them moved.)
//   2. A REAL tip supersedes `hardness` entirely: a tip carries its own edge.
//   3. The mask is rendered from the cache's KEY, never from the raw request -- so the cache stays
//      exactly transparent and two dabs in one size bin are the same dab.
//   4. The frame an animated tip stamps is chosen ONCE PER DAB, in the once-per-dab step -- before
//      any clipping -- so a stroke at the document's edge lays the cell sequence it would have laid
//      in the middle of the canvas.
namespace {

using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::core::brush::BitmapTip;
using mosaic::core::brush::BrushDynamics;
using mosaic::core::brush::BrushEngine;
using mosaic::core::brush::BrushOptions;
using mosaic::core::brush::BrushParams;
using mosaic::core::brush::BrushTip;
using mosaic::core::brush::CurveOption;
using mosaic::core::brush::CurveOptionData;
using mosaic::core::brush::FrameSelection;
using mosaic::core::brush::HoseParams;
using mosaic::core::brush::makeTip;
using mosaic::core::brush::MaskFalloff;
using mosaic::core::brush::MaskGeneratorParams;
using mosaic::core::brush::MaskShape;
using mosaic::core::brush::Sensor;
using mosaic::core::brush::SensorId;
using mosaic::core::brush::StrokeInput;
using mosaic::core::brush::TipApplication;
using mosaic::core::brush::TipFrame;
using mosaic::core::brush::TipSourceKind;

int alphaAt(const Image& img, int x, int y) {
    return img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4 + 3];
}

[[nodiscard]] StrokeInput at(double x, double y, double pressure = 1.0) {
    StrokeInput in;
    in.pos = {x, y};
    in.pressure = pressure;
    return in;
}

[[nodiscard]] Image paint(const BrushParams& p, const std::vector<StrokeInput>& path,
                          std::uint32_t w = 128, std::uint32_t h = 64) {
    Image img(w, h);
    BrushEngine eng;
    eng.begin(w, h, img, p, BrushDynamics{}, path.front());
    for (std::size_t i = 1; i < path.size(); ++i)
        eng.extendTo(path[i]);
    eng.end();
    eng.composite();
    return img;
}

// One dab, centred on a pixel CENTRE: the tip's box then lands on a half-pixel phase, which the
// default quantization (4 bins per axis) represents exactly -- so a probe's offset from the dab
// centre is a whole number of pixels and the arithmetic below can be done by hand.
[[nodiscard]] Image oneDab(const BrushParams& p, double pressure = 1.0) {
    return paint(p, {at(32.5, 32.5, pressure)});
}

// A black, fully-opaque brush, so a pixel's ALPHA reads back the tip's coverage directly.
[[nodiscard]] BrushParams readableParams() {
    BrushParams p;
    p.diameter = 24.0;
    p.spacing = 1.0;
    p.flow = 1.0;
    p.opacity = 1.0;
    p.color = Color8{0, 0, 0, 255};
    return p;
}

// `hFade`/`vFade` are the shoulder's start: 1 is a hard tip (the shoulder begins at the rim), 0 is a
// cone from the centre. NOT the other way round -- the attribute's own default is 0, i.e. fully soft.
[[nodiscard]] MaskGeneratorParams circleGen(double fade, double ratio = 1.0) {
    MaskGeneratorParams g;
    g.shape = MaskShape::Circle;
    g.falloff = MaskFalloff::Default;
    g.hFade = g.vFade = fade;
    g.ratio = ratio; // the AUTHORED ratio; the dab's own ratio is what actually squashes it
    return g;
}

// One option: `sensor`, identity curve, full strength, checked unless told otherwise.
[[nodiscard]] CurveOption option(const char* name, SensorId sensor, double strengthMin,
                                 double strengthMax, bool checked = true) {
    CurveOptionData d;
    d.name = name;
    d.checked = checked;
    d.strength = 1.0;
    d.strengthMin = strengthMin;
    d.strengthMax = strengthMax;
    d.sensors.sensors = {Sensor::withDefaults(sensor)};
    return CurveOption(d);
}

// A hose of `n` cells, each a solid square of a different grey. The TIP IMAGE convention is white =
// no paint, so cell k covers `255 - k*60`: the alpha under a dab says which cell stamped it.
[[nodiscard]] std::shared_ptr<const BitmapTip> greyHose(int n, FrameSelection sel) {
    std::vector<TipFrame> frames;
    for (int f = 0; f < n; ++f) {
        TipFrame t;
        t.width = t.height = 8;
        t.rgba.assign(8 * 8 * 4, 255);
        for (std::size_t i = 0; i < 64; ++i) {
            const auto grey = static_cast<std::uint8_t>(f * 60);
            t.rgba[i * 4] = t.rgba[i * 4 + 1] = t.rgba[i * 4 + 2] = grey;
        }
        frames.push_back(std::move(t));
    }
    HoseParams hose;
    hose.dim = 1;
    hose.declaredCells = n;
    hose.rank[0] = n;
    hose.selection[0] = sel;
    return std::make_shared<const BitmapTip>(std::move(frames), TipApplication::AlphaMask,
                                            TipSourceKind::Mask, mosaic::core::brush::TipAdjustments{},
                                            hose);
}

} // namespace

TEST_CASE("tip: a real tip carries its own edge, and `hardness` stops meaning anything") {
    // The claim is not that the two differ -- it is that the tip is the ONLY thing that decides the
    // falloff. So: hold the tip, sweep `hardness` across its whole range, and demand the very same
    // bytes. A `hardness` that still reached the falloff would move them.
    BrushParams p = readableParams();
    p.tip = makeTip(circleGen(1.0)); // a hard tip

    p.hardness = 0.0;
    const Image soft = oneDab(p);
    p.hardness = 1.0;
    const Image hard = oneDab(p);
    CHECK(soft.rgba == hard.rgba);

    // And the tip really is consulted: swap it for a soft one and the same dab changes. (Without
    // this, "hardness does nothing" would also pass on an engine that stamped nothing at all.)
    p.tip = makeTip(circleGen(0.0)); // a cone from the centre
    const Image cone = oneDab(p);
    CHECK(cone.rgba != hard.rgba);

    // A hard tip is solid two-thirds of the way out; a cone is well down by there.
    CHECK(alphaAt(hard, 32 + 8, 32) == 255);
    CHECK(alphaAt(cone, 32 + 8, 32) < 200);
    CHECK(alphaAt(cone, 32 + 8, 32) > 20);
    // Both still fill the centre and both still stop at the rim.
    CHECK(alphaAt(cone, 32, 32) == 255);
    CHECK(alphaAt(hard, 32 + 13, 32) == 0);
    CHECK(alphaAt(cone, 32 + 13, 32) == 0);
}

TEST_CASE("tip: the dab's ratio squashes the tip -- probed at a CORNER of its box, not just on axis") {
    // ⚠ THE BACKSTOP THAT MASKS THE PRIMARY CHECK. A falloff that ignored the ratio entirely still
    // passes every probe on the two AXES, because the mask's BOX is correctly sized either way and
    // clips the overspill. Only a probe INSIDE the box and OUTSIDE the ellipse can tell them apart.
    BrushParams p = readableParams();
    p.tip = makeTip(circleGen(1.0));
    p.ratio = 0.25; // 24 px wide, 6 px tall

    const Image img = oneDab(p);

    // On the long axis, out to 10 of its 12 px: painted. Off the short one, at 3 of its 3: gone.
    CHECK(alphaAt(img, 32 + 10, 32) > 0);
    CHECK(alphaAt(img, 32, 32 + 2) > 200);
    CHECK(alphaAt(img, 32, 32 + 3) == 0);

    // THE CORNER. (42, 34) is inside the 24x6 box -- 10 px along, 2 px up -- and outside the
    // ellipse: (10/12)^2 + (2/3)^2 = 1.14 > 1. A round tip of the same diameter would paint it.
    CHECK(alphaAt(img, 32 + 10, 32 + 2) == 0);
    BrushParams round = p;
    round.ratio = 1.0;
    CHECK(alphaAt(oneDab(round), 32 + 10, 32 + 2) > 0); // ... and does
}

TEST_CASE("tip: the dab's angle turns the tip with it") {
    BrushParams p = readableParams();
    p.tip = makeTip(circleGen(1.0));
    p.ratio = 0.25;
    p.angleRad = 1.57079632679489661923; // a quarter turn: the long axis stands up

    const Image img = oneDab(p);
    CHECK(alphaAt(img, 32, 32 + 10) > 0);  // long axis, now vertical
    CHECK(alphaAt(img, 32 + 3, 32) == 0);  // short axis, now horizontal
    CHECK(alphaAt(img, 32 + 2, 32) > 200);
}

TEST_CASE("tip: Softness scales the tip's fade -- and an UNCHECKED Softness contributes the identity") {
    // The option that could not be wired until the tip landed: it scales a MASK GENERATOR's softness
    // ("1 = as authored"), and the engine's analytic falloff was parameterized by hardness, which is
    // a different quantity. Now there is a generator to scale.
    BrushParams p = readableParams();
    p.tip = makeTip(circleGen(1.0)); // authored hard

    const Image plain = oneDab(p, /*pressure=*/0.2); // no option at all: pressure is inert
    CHECK(alphaAt(plain, 32 + 8, 32) == 255);

    auto opts = std::make_shared<BrushOptions>();
    opts->softness = option("Softness", SensorId::Pressure, 0.1, 1.0);
    p.options = opts;

    // At full pressure the option resolves to 1 -- as authored -- so the tip is the hard one again.
    CHECK(alphaAt(oneDab(p, 1.0), 32 + 8, 32) == 255);
    // At low pressure it resolves near its 0.1 floor, and the same tip goes soft.
    const int softened = alphaAt(oneDab(p, 0.2), 32 + 8, 32);
    CHECK(softened < 200);
    CHECK(softened > 20);

    // ⚠ RULE 2. An option that is PRESENT but UNCHECKED contributes the identity, not its strength.
    // A leak here would soften every dab of the 3 shipped presets that carry a switched-off Softness.
    auto unchecked = std::make_shared<BrushOptions>();
    unchecked->softness = option("Softness", SensorId::Pressure, 0.1, 1.0, /*checked=*/false);
    p.options = unchecked;
    const Image gated = oneDab(p, 0.2);
    CHECK(gated.rgba == plain.rgba); // byte for byte the dab with no option at all
}

TEST_CASE("tip: the mask is rendered from the cache's KEY, not from the request") {
    // The cache is exactly transparent only because the dab's continuous parameters are quantized
    // FIRST and the mask is drawn from the quantized values. The observable consequence -- and the
    // only one a caller can check without reaching inside -- is that two dabs in the same size bin
    // are THE SAME DAB. Render from the raw request instead and they would differ by a hair.
    BrushParams p = readableParams();
    p.tip = makeTip(circleGen(1.0));

    p.diameter = 20.0; // 320 steps of the default 1/16 px size bin, exactly
    const Image a = oneDab(p);
    p.diameter = 20.03; // 320.48 steps -> the same bin
    const Image b = oneDab(p);
    CHECK(a.rgba == b.rgba);

    p.diameter = 20.10; // 321.6 steps -> rounds into the NEXT bin, and must not be the same dab
    const Image c = oneDab(p);
    CHECK(c.rgba != a.rgba);
}

TEST_CASE("tip: a bitmap tip stamps its own raster") {
    // An L, asymmetric about both axes -- so a tip stamped mirrored, rotated or transposed does not
    // read as the one that was asked for.
    TipFrame f;
    f.width = f.height = 16;
    f.rgba.assign(16 * 16 * 4, 255); // white == no paint
    const auto ink = [&](std::uint32_t x, std::uint32_t y) {
        const std::size_t i = (static_cast<std::size_t>(y) * 16 + x) * 4;
        f.rgba[i] = f.rgba[i + 1] = f.rgba[i + 2] = 0; // black == full paint
    };
    for (std::uint32_t y = 2; y < 12; ++y) // the upright
        ink(3, y);
    for (std::uint32_t x = 3; x < 10; ++x) // the foot
        ink(x, 11);

    BrushParams p = readableParams();
    p.diameter = 16.0; // 1:1 with the frame
    p.tip = makeTip(std::make_shared<const BitmapTip>(std::vector<TipFrame>{f},
                                                      TipApplication::AlphaMask, TipSourceKind::Mask));

    // Centred on a pixel CORNER, not a centre: the 16x16 box then lands at a ZERO sub-pixel phase, so
    // frame pixel (fx, fy) is document pixel (24 + fx, 24 + fy) exactly. (At the half-pixel phase the
    // rest of this file uses, the resample would split the L's one-pixel-wide upright across two
    // columns at 50% each -- correct bilinear behaviour, and useless for reading a tip's shape back.)
    const Image img = paint(p, {at(32.0, 32.0)});
    CHECK(alphaAt(img, 24 + 3, 24 + 6) > 200); // on the upright
    CHECK(alphaAt(img, 24 + 6, 24 + 11) > 200); // on the foot
    CHECK(alphaAt(img, 24 + 12, 24 + 6) == 0);  // the empty quadrant the L leaves open
    CHECK(alphaAt(img, 24 + 6, 24 + 3) == 0);
}

TEST_CASE("tip: an animated tip advances a cell per dab") {
    BrushParams p = readableParams();
    p.diameter = 8.0;
    p.spacing = 2.0; // 16 px apart: the dabs do not touch, so each cell reads back alone
    p.tip = makeTip(greyHose(3, FrameSelection::Incremental));

    // Dabs at x = 20, 36, 52, 68 (the first lands under the press).
    const Image img = paint(p, {at(20.0, 32.0), at(68.0, 32.0)});
    // Cell k covers 255 - 60k: an Incremental hose keyed off the dab counter walks 0, 1, 2, 0.
    CHECK(alphaAt(img, 20, 32) == 255);
    CHECK(alphaAt(img, 36, 32) == 195);
    CHECK(alphaAt(img, 52, 32) == 135);
    CHECK(alphaAt(img, 68, 32) == 255);
}

TEST_CASE("tip: a stroke at the document's edge lays the cell sequence it would lay in the middle") {
    // ⚠ The frame is chosen in the once-per-dab step, BEFORE the dab can be clipped away -- and a
    // `Random` hose dimension draws from the stroke's per-dab random stream to do it. Choose it in
    // stamp() instead, after the off-document early return, and the dabs that fall outside the canvas
    // stop drawing: every visible dab downstream gets a cell that belongs to an earlier one, and the
    // stream every `fuzzy` sensor shares is left short by one draw per clipped dab.
    //
    // The stroke's dab sequence is a property of its GEOMETRY, so translating the whole stroke into
    // the clear must reproduce it exactly.
    BrushParams p = readableParams();
    p.diameter = 8.0;
    p.spacing = 2.0;
    p.seed = 4; // fixes the stream; chosen so that all three cells fall among the VISIBLE dabs
    p.tip = makeTip(greyHose(3, FrameSelection::Random));

    // Dabs at x = -30, -14, 2, 18, 34, 50. The first two are wholly off the canvas.
    const Image clipped = paint(p, {at(-30.0, 32.0), at(50.0, 32.0)});
    // The same stroke, 40 px to the right: nothing clips.
    const Image clear = paint(p, {at(10.0, 32.0), at(90.0, 32.0)});

    std::vector<int> cells;
    for (const int x : {2, 18, 34, 50}) {
        CHECK(alphaAt(clipped, x, 32) == alphaAt(clear, x + 40, 32));
        cells.push_back(alphaAt(clipped, x, 32));
    }
    // ... and the hose really is animating -- all three cells land among these four dabs -- or the
    // check above would hold just as well on an engine that never advanced a cell at all.
    CHECK(std::set<int>(cells.begin(), cells.end()).size() == 3);
}

