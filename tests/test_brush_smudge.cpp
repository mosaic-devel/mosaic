#include "common/image.hpp"
#include "core/brush/brush_engine.hpp"
#include "core/brush/brush_tip.hpp"
#include "core/brush/mask_generator.hpp"

#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <memory>
#include <vector>

// The smudge engine (docs/brushes.md §6.6c, S19 Arc D): the stroke reads the canvas and drags it,
// transcribed from the reference's colorsmudge in its LEGACY parameterization -- the one every
// shipped colorsmudge preset runs. The engine keeps a stroke-local premultiplied float state
// buffer seeded from the pristine base (the §6.6b DabSource snapshot) and NEVER reads the live
// target, which is what the composite-cadence test at the bottom pins: the reference reads and
// writes its device per dab, and the deterministic equivalent must not care when composite() runs.
//
// The formula pins are hand-derived (the Halton values are the van der Corput sequences in base 2
// and 3, scaled by the reference's integer rounding). The engine pins drive REAL strokes over
// canvases built so the property is loud: a red field for a smear to drag, a red/blue boundary for
// a dulling average to mix.
namespace {

using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::core::brush::BrushDynamics;
using mosaic::core::brush::BrushEngine;
using mosaic::core::brush::BrushOptions;
using mosaic::core::brush::BrushParams;
using mosaic::core::brush::CurveOptionData;
using mosaic::core::brush::HaltonSequence;
using mosaic::core::brush::makeTip;
using mosaic::core::brush::MaskGeneratorParams;
using mosaic::core::brush::PaintMode;
using mosaic::core::brush::Sensor;
using mosaic::core::brush::SensorId;
using mosaic::core::brush::smudgeColorRateOpacity;
using mosaic::core::brush::SmudgeRect;
using mosaic::core::brush::smudgeSampleRect;
using mosaic::core::brush::StrokeInput;
using mosaic::core::brush::StrokeMode;

[[nodiscard]] Color8 pixel(const Image& img, int x, int y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

void fillRect(Image& img, int x0, int y0, int x1, int y1, Color8 c) {
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
            img.rgba[p] = c.r;
            img.rgba[p + 1] = c.g;
            img.rgba[p + 2] = c.b;
            img.rgba[p + 3] = c.a;
        }
    }
}

// One checked curve option with a live identity-curve pressure sensor: at pressure 1 its
// size-like value is exactly `strength`.
[[nodiscard]] CurveOptionData pressureOption(const char* name, double strength, bool checked,
                                             double strengthMax = 1.0) {
    CurveOptionData d;
    d.name = name;
    d.checkable = true;
    d.checked = checked;
    d.strength = strength;
    d.strengthMax = strengthMax;
    d.sensors.sensors = {Sensor::withDefaults(SensorId::Pressure)};
    return d;
}

// The baseline smudge brush the engine tests drive: a hard 12 px circle tip, full rate, no colour
// rate -- a pure smear. Individual tests override from here.
[[nodiscard]] BrushParams smudgeParams() {
    BrushParams p;
    MaskGeneratorParams gen;
    gen.diameter = 12.0;
    p.tip = makeTip(gen);
    p.diameter = 12.0;
    p.spacing = 0.25; // 3 px steps: dense enough that the smear chain is loud
    p.smudge.enabled = true;
    p.color = {20, 220, 40, 255};
    return p;
}

[[nodiscard]] StrokeInput at(double x, double y, double pressure = 1.0) {
    StrokeInput in;
    in.pos = {x, y};
    in.pressure = pressure;
    return in;
}

// Drive one straight stroke through `img`, compositing once at the end (unless the test composites
// itself). Returns the engine so the caller can restore()/inspect coverage.
void stroke(BrushEngine& eng, Image& img, const BrushParams& p, double x0, double y0, double x1,
            double y1, int steps = 24) {
    eng.begin(img.width, img.height, img, p, BrushDynamics{}, at(x0, y0));
    for (int i = 1; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        eng.extendTo(at(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t));
    }
    eng.flush();
    eng.composite();
    eng.end();
}

// ------------------------------------------------------------------ the pure, transcribed pieces

TEST_CASE("smudgeColorRateOpacity: the legacy lerp with its 0.2 floor") {
    // maxSmudgeRate 1 leaves the floor: 0.2 * colorRate * opacity.
    CHECK(smudgeColorRateOpacity(1.0, 1.0, 1.0) == doctest::Approx(0.2));
    CHECK(smudgeColorRateOpacity(0.5, 1.0, 1.0) == doctest::Approx(0.1));
    CHECK(smudgeColorRateOpacity(0.5, 0.5, 1.0) == doctest::Approx(0.05));
    // A weaker maxSmudgeRate raises the ceiling: max(1 - 0.25, 0.2) = 0.75.
    CHECK(smudgeColorRateOpacity(1.0, 1.0, 0.25) == doctest::Approx(0.75));
    // maxSmudgeRate 0: the ceiling is 1, the whole colour rate lands.
    CHECK(smudgeColorRateOpacity(0.5, 1.0, 0.0) == doctest::Approx(0.5));
    // Zero colour rate is zero paint whatever the rest says.
    CHECK(smudgeColorRateOpacity(0.0, 1.0, 0.0) == 0.0);
}

TEST_CASE("HaltonSequence: the reference's integer generator, hand-walked") {
    // Base 2 walks the van der Corput sequence 1/2, 1/4, 3/4, 1/8, 5/8, 3/8, 7/8; scaled onto
    // [0,7] by (n*max + d/2)/d that is 4, 2, 5, 1, 4, 3, 6.
    HaltonSequence h2(2);
    const int expect2[] = {4, 2, 5, 1, 4, 3, 6};
    for (const int e : expect2)
        CHECK(h2.generate(7) == e);
    // Base 3: 1/3, 2/3, 1/9, 4/9, 7/9, 2/9 -> on [0,8] by (n*8 + d/2)/d: 3, 5, 1, 4, 6, 2
    // (2/3 lands on 17/3 = 5 -- the integer rounding, not the nearest-real 5.33 -> 5, agrees here,
    // but writing the division out is what keeps this pin honest).
    HaltonSequence h3(3);
    const int expect3[] = {3, 5, 1, 4, 6, 2};
    for (const int e : expect3)
        CHECK(h3.generate(8) == e);
    // maxRange 0 (a 1 px wide sample rect) always answers 0.
    HaltonSequence h(2);
    for (int i = 0; i < 5; ++i)
        CHECK(h.generate(0) == 0);
}

TEST_CASE("smudgeSampleRect: blow, shrink, floor at the centre pixel") {
    const SmudgeRect src{10, 20, 8, 6};
    // radius <= 0: the single pixel at the closed rect's integer midpoint.
    {
        const SmudgeRect r = smudgeSampleRect(src, 0.0);
        CHECK(r.x == 13);
        CHECK(r.y == 22);
        CHECK(r.w == 1);
        CHECK(r.h == 1);
    }
    // radius 1: coeff 0, the source rect itself.
    {
        const SmudgeRect r = smudgeSampleRect(src, 1.0);
        CHECK(r.x == 10);
        CHECK(r.y == 20);
        CHECK(r.w == 8);
        CHECK(r.h == 6);
    }
    // radius 3: blown outward by one full extent per side.
    {
        const SmudgeRect r = smudgeSampleRect(src, 3.0);
        CHECK(r.x == 2);
        CHECK(r.y == 14);
        CHECK(r.w == 24);
        CHECK(r.h == 18);
    }
    // radius 0.5 SHRINKS (the reference's blowRect truncates extent * coeff toward zero).
    {
        const SmudgeRect r = smudgeSampleRect(src, 0.5);
        CHECK(r.x == 12);
        CHECK(r.y == 21);
        CHECK(r.w == 4);
        CHECK(r.h == 4);
    }
    // Truncation means a small rect never shrinks at all: 2 * (0.5 * (0.01 - 1)) truncates to 0,
    // and a positive radius can never empty a rect (coeff > -0.5 keeps at least one pixel).
    {
        const SmudgeRect tiny{0, 0, 2, 2};
        const SmudgeRect r = smudgeSampleRect(tiny, 0.01);
        CHECK(r.x == 0);
        CHECK(r.y == 0);
        CHECK(r.w == 2);
        CHECK(r.h == 2);
    }
    // Negative coordinates keep the reference's trunc-toward-zero midpoint, not floor's.
    {
        const SmudgeRect neg{-5, -5, 2, 2};
        const SmudgeRect r = smudgeSampleRect(neg, 0.0);
        CHECK(r.x == -4);
        CHECK(r.y == -4);
    }
}

// --------------------------------------------------------------------------- the engine's smudge

TEST_CASE("smudge: the first dab paints nothing and a one-dab stroke is a no-op") {
    Image img(64, 64);
    fillRect(img, 0, 0, 64, 64, {200, 30, 30, 255});
    const Image before = img;

    BrushEngine eng;
    const BrushParams p = smudgeParams();
    eng.begin(img.width, img.height, img, p, BrushDynamics{}, at(32, 32));
    eng.flush();
    eng.composite();
    eng.end();

    CHECK(img.rgba == before.rgba); // the press only plants the anchor, exactly the reference
    CHECK(eng.dirtyBounds().empty());
}

TEST_CASE("smudge: a smear drags paint across the canvas") {
    // Left half red, right half transparent; the stroke crosses the boundary rightward.
    Image img(160, 48);
    fillRect(img, 0, 0, 64, 48, {200, 30, 30, 255});

    BrushEngine eng;
    stroke(eng, img, smudgeParams(), 20, 24, 140, 24);

    // Red travelled well past the boundary: the CHAIN (each dab reading what the previous wrote)
    // is the mechanism, and a base-only read could carry it at most one dab-step.
    const Color8 dragged = pixel(img, 100, 24);
    CHECK(dragged.a > 0);
    CHECK(dragged.r > dragged.g);
    CHECK(dragged.r > dragged.b);
    // A pure smear deposits no paint of its own: far from the stroke nothing moved.
    CHECK(pixel(img, 100, 4).a == 0);
    CHECK(pixel(img, 30, 24).r == 200); // behind the boundary the field is still red
}

TEST_CASE("smudge: smearing transparency over paint EATS the paint (alpha lerps down)") {
    // Right half red; the stroke starts on transparency and drags it into the paint.
    Image img(160, 48);
    fillRect(img, 80, 0, 160, 48, {200, 30, 30, 255});

    BrushEngine eng;
    stroke(eng, img, smudgeParams(), 20, 24, 140, 24);

    // Inside the red field, along the stroke, alpha dropped below opaque: the final COPY lerps
    // alpha too. A source-OVER there would leave alpha at 255 exactly.
    CHECK(pixel(img, 100, 24).a < 255);
    // Off the stroke the field is untouched.
    CHECK(pixel(img, 100, 4).a == 255);
}

TEST_CASE("smudge: dulling floods the dab with the sampled average") {
    // A red/blue boundary; a dulling stroke along it mixes both into purple.
    Image img(160, 64);
    fillRect(img, 0, 0, 160, 32, {220, 20, 20, 255});
    fillRect(img, 0, 32, 160, 64, {20, 20, 220, 255});

    BrushParams p = smudgeParams();
    p.smudge.dulling = true;
    auto o = std::make_shared<BrushOptions>();
    o->smudgeRadius.emplace(pressureOption("SmudgeRadius", 1.0, true, 3.0));
    p.options = std::move(o);

    BrushEngine eng;
    stroke(eng, img, p, 20, 32, 140, 32);

    // On the boundary line, inside the stroke, both reds and blues survive in one pixel -- the
    // averaged fill, not either field verbatim.
    const Color8 mixed = pixel(img, 80, 32);
    CHECK(mixed.r > 40);
    CHECK(mixed.b > 40);
    // Far rows keep their fields.
    CHECK(pixel(img, 80, 4).r == 220);
    CHECK(pixel(img, 80, 60).b == 220);
}

TEST_CASE("smudge: the colour rate deposits paint and its ceiling reads maxSmudgeRate") {
    // On a fully transparent canvas a pure blender leaves nothing...
    {
        Image img(160, 48);
        BrushEngine eng;
        stroke(eng, img, smudgeParams(), 20, 24, 140, 24);
        CHECK(pixel(img, 80, 24).a == 0);
    }
    // ...a colour rate deposits the paint colour...
    auto withColorRate = [](double maxSmudgeRate) {
        Image img(160, 48);
        BrushParams p = smudgeParams();
        auto o = std::make_shared<BrushOptions>();
        o->colorRate.emplace(pressureOption("ColorRate", 1.0, true));
        p.options = std::move(o);
        p.smudge.maxSmudgeRate = maxSmudgeRate;
        BrushEngine eng;
        stroke(eng, img, p, 20, 24, 140, 24);
        return pixel(img, 80, 24);
    };
    const Color8 floor = withColorRate(1.0);
    CHECK(floor.a > 0);
    CHECK(floor.g > floor.r); // the paint colour, not the (empty) canvas
    // ...and a LOWER static smudge strength raises the colour-rate ceiling: max(1 - 0.2, 0.2)
    // = 0.8 against the floor's 0.2, so the deposit is decisively heavier.
    const Color8 raised = withColorRate(0.2);
    CHECK(raised.a > floor.a);
}

TEST_CASE("smudge: unchecked options use the reference's hard-coded fallbacks, not strength") {
    // An UNCHECKED SmudgeRate with strength 0 still smears fully (the fallback is 1.0)...
    {
        Image img(160, 48);
        fillRect(img, 0, 0, 64, 48, {200, 30, 30, 255});
        BrushParams p = smudgeParams();
        auto o = std::make_shared<BrushOptions>();
        o->smudgeRate.emplace(pressureOption("SmudgeRate", 0.0, false));
        p.options = std::move(o);
        BrushEngine eng;
        stroke(eng, img, p, 20, 24, 140, 24);
        CHECK(pixel(img, 100, 24).a > 0); // it smeared
    }
    // ...an UNCHECKED ColorRate with strength 1 deposits nothing (the fallback is 0.0)...
    {
        Image img(160, 48);
        BrushParams p = smudgeParams();
        auto o = std::make_shared<BrushOptions>();
        o->colorRate.emplace(pressureOption("ColorRate", 1.0, false));
        p.options = std::move(o);
        BrushEngine eng;
        stroke(eng, img, p, 20, 24, 140, 24);
        CHECK(pixel(img, 80, 24).a == 0);
    }
    // ...and a CHECKED SmudgeRate at strength 0 is a stroke that touches nothing at all.
    {
        Image img(160, 48);
        fillRect(img, 0, 0, 160, 48, {200, 30, 30, 255});
        const Image before = img;
        BrushParams p = smudgeParams();
        auto o = std::make_shared<BrushOptions>();
        o->smudgeRate.emplace(pressureOption("SmudgeRate", 0.0, true));
        p.options = std::move(o);
        BrushEngine eng;
        stroke(eng, img, p, 20, 24, 140, 24);
        CHECK(img.rgba == before.rgba);
        CHECK(eng.dirtyBounds().empty()); // f == 0 marks nothing pending either
    }
}

TEST_CASE("smudge: composite cadence cannot change the stroke (the walk never reads the target)") {
    auto canvas = [] {
        Image img(200, 48);
        fillRect(img, 0, 0, 60, 48, {200, 30, 30, 255});
        fillRect(img, 60, 0, 120, 48, {30, 30, 200, 200});
        return img;
    };
    const BrushParams p = smudgeParams();

    // A: one composite at the end.
    Image a = canvas();
    {
        BrushEngine eng;
        stroke(eng, a, p, 20, 24, 180, 24, 32);
    }
    // B: a composite after EVERY sample -- the live-canvas cadence.
    Image b = canvas();
    {
        BrushEngine eng;
        eng.begin(b.width, b.height, b, p, BrushDynamics{}, at(20, 24));
        for (int i = 1; i <= 32; ++i) {
            eng.extendTo(at(20 + 160.0 * i / 32, 24));
            eng.composite();
        }
        eng.flush();
        eng.composite();
        eng.end();
    }
    // The reference reads its live device, so its result depends on when the canvas refreshes;
    // the deterministic transcription must not. Byte for byte.
    CHECK(a.rgba == b.rgba);
}

TEST_CASE("smudge: restore() returns the exact pre-stroke bytes across working-rect growth") {
    // The stroke starts in a small corner and travels far: ensureCovers re-homes every smudge
    // buffer (the state, the fill flags) several times on the way. restore() must still land on
    // the pristine canvas -- this is the §6.6b DabSource snapshot's contract.
    //
    // ⚠ composite() runs INSIDE the stroke, every sample -- the live-canvas cadence -- because a
    // base snapshot that wrongly re-reads the target AFTER a composite has written it reads the
    // stroke back as "pristine", and a restore test whose only composite comes after the last dab
    // can never see that. That exact mutant survived this case's first form (the sixth
    // instrument-not-contract lesson of the arc).
    Image img(300, 200);
    fillRect(img, 0, 0, 300, 200, {90, 140, 60, 255});
    fillRect(img, 100, 50, 200, 150, {200, 30, 30, 128});
    const Image before = img;

    BrushParams p = smudgeParams();
    auto o = std::make_shared<BrushOptions>();
    o->colorRate.emplace(pressureOption("ColorRate", 0.7, true));
    p.options = std::move(o);

    BrushEngine eng;
    eng.begin(img.width, img.height, img, p, BrushDynamics{}, at(8, 8));
    for (int i = 1; i <= 48; ++i) {
        eng.extendTo(at(8 + 280.0 * i / 48, 8 + 180.0 * i / 48));
        eng.composite(); // live cadence: later dabs stamp over already-composited pixels
    }
    eng.flush();
    eng.composite();

    CHECK(img.rgba != before.rgba); // it painted...
    eng.restore();
    eng.end();
    CHECK(img.rgba == before.rgba); // ...and came back exactly
}

TEST_CASE("smudge: coverage stays the plain geometric accumulation in [0,1]") {
    Image img(160, 48);
    fillRect(img, 0, 0, 160, 48, {200, 30, 30, 255});

    BrushEngine eng;
    stroke(eng, img, smudgeParams(), 20, 24, 140, 24);

    const std::vector<float>& cov = eng.coverage();
    bool any = false;
    for (const float c : cov) {
        CHECK(c >= 0.0f);
        CHECK(c <= 1.0f);
        any = any || c > 0.0f;
    }
    CHECK(any);
}

TEST_CASE("smudge: an authored Buildup mode is normalized away, float for float") {
    // The smudge engine owns the whole accumulation, so an authored PaintOpAction must change
    // NOTHING -- image bytes and the coverage buffer both (the Buildup-gate lesson: the image
    // alone can be blind).
    auto run = [](PaintMode mode) {
        Image img(160, 48);
        fillRect(img, 0, 0, 64, 48, {200, 30, 30, 255});
        BrushParams p = smudgeParams();
        p.paintMode = mode;
        BrushEngine eng;
        stroke(eng, img, p, 20, 24, 140, 24);
        return std::make_pair(std::move(img), eng.coverage());
    };
    const auto [washImg, washCov] = run(PaintMode::Wash);
    const auto [buildImg, buildCov] = run(PaintMode::Buildup);
    CHECK(washImg.rgba == buildImg.rgba);
    REQUIRE(washCov.size() == buildCov.size());
    for (std::size_t i = 0; i < washCov.size(); ++i)
        CHECK(washCov[i] == buildCov[i]);
}

TEST_CASE("smudge: an Erase stroke or a tipless brush ignores the smudge flag") {
    // Erase: the stroke carves alpha exactly as a plain eraser would -- smudge is Paint-only.
    {
        Image img(64, 64);
        fillRect(img, 0, 0, 64, 64, {200, 30, 30, 255});
        BrushParams p = smudgeParams();
        p.strokeMode = StrokeMode::Erase;
        BrushEngine eng;
        stroke(eng, img, p, 10, 32, 54, 32, 12);
        CHECK(pixel(img, 32, 32).a < 255); // it carved from the very first dab
    }
    // No tip: the analytic circle paints plainly (a smudge walk needs a real tip raster).
    {
        Image img(64, 64);
        BrushParams p = smudgeParams();
        p.tip = nullptr;
        BrushEngine eng;
        stroke(eng, img, p, 10, 32, 54, 32, 12);
        CHECK(pixel(img, 32, 32).a > 0); // painted, first dab included
    }
}

TEST_CASE("smudge: coverage reaches exactly 1.0 under a hard tip whatever the rate") {
    // Coverage accumulates the GEOMETRIC mask (m), not the blt factor (m * rate * opacity): the
    // Inpaint contract wants where the stroke touched, and folding the rate in would make the
    // same footprint read differently at different rates. A hard tip's centre is mask 1, so ONE
    // dab there puts the coverage at exactly 1.0f -- the rate-folded mutant lands on 0.5 and can
    // then never reach 1.0 exactly.
    Image img(160, 48);
    fillRect(img, 0, 0, 160, 48, {200, 30, 30, 255});
    BrushParams p = smudgeParams();
    auto o = std::make_shared<BrushOptions>();
    o->smudgeRate.emplace(pressureOption("SmudgeRate", 0.5, true));
    p.options = std::move(o);
    BrushEngine eng;
    stroke(eng, img, p, 20, 24, 140, 24);
    float maxCov = 0.0f;
    for (const float c : eng.coverage())
        maxCov = std::max(maxCov, c);
    CHECK(maxCov == 1.0f);
}

TEST_CASE("smudge: the per-dab opacity keeps its STRENGTH (direct painting, no stroke cap)") {
    // Colorsmudge is direct painting: the Opacity option evaluates WITH its static strength per
    // dab (the reference's useStrengthValue is true there), and no stroke-level cap exists. At
    // strength 0 the whole stroke is a no-op; the wash-style strength-less evaluation would smear
    // at full pressure regardless.
    Image img(160, 48);
    fillRect(img, 0, 0, 64, 48, {200, 30, 30, 255});
    const Image before = img;
    BrushParams p = smudgeParams();
    CurveOptionData op;
    op.name = "Opacity";
    op.checkable = false;
    op.strength = 0.0;
    op.sensors.sensors = {Sensor::withDefaults(SensorId::Pressure)};
    auto o = std::make_shared<BrushOptions>();
    o->opacity.emplace(std::move(op));
    p.options = std::move(o);
    BrushEngine eng;
    stroke(eng, img, p, 20, 24, 140, 24);
    CHECK(img.rgba == before.rgba);
}

TEST_CASE("smudge: a translucent paint colour composites OVER, not a plain lerp") {
    // The reference's paint is opaque, where OVER-at-opacity and a lerp coincide; Mosaic's
    // foreground carries alpha, and the true premultiplied over -- paint*oc + blend*(1 - pa*oc)
    // -- must hold. On an OPAQUE canvas the distinguishing fact is alpha: OVER keeps it at 255,
    // the lerp-with-alpha mutant drags it toward the paint's 128.
    Image img(160, 48);
    fillRect(img, 0, 0, 160, 48, {40, 40, 200, 255});
    BrushParams p = smudgeParams();
    p.color = {20, 220, 40, 128};
    auto o = std::make_shared<BrushOptions>();
    o->colorRate.emplace(pressureOption("ColorRate", 1.0, true));
    p.options = std::move(o);
    p.smudge.maxSmudgeRate = 0.0; // ceiling 1: the whole colour rate lands
    BrushEngine eng;
    stroke(eng, img, p, 20, 24, 140, 24);
    const Color8 c = pixel(img, 80, 24);
    CHECK(c.a == 255);
    CHECK(c.g > 100); // the paint arrived...
    CHECK(c.b < 200); // ...displacing the canvas colour
}

TEST_CASE("smudge golden: a fixed stroke pins its bytes (blessed 2026-07-14)") {
    // A compact byte-exact pin over one smear stroke, blessed from the build the property tests
    // above vetted. What it exists to kill is the family the property tests are too loose for:
    // the in-place read (the scratch copy skipped, so a dab reads pixels it already wrote), the
    // smear offset's rounding, the premultiplied lerp's grouping -- and the RANDOM-STREAM
    // discipline, which is why the options carry a fuzzy sensor beside pressure: any mutant that
    // evaluates an option twice (or skips one) shifts every later draw and moves these bytes.
    Image img(200, 64);
    fillRect(img, 0, 0, 80, 64, {200, 30, 30, 255});
    fillRect(img, 80, 0, 200, 64, {30, 120, 200, 90});

    BrushParams p = smudgeParams();
    p.seed = 7;
    CurveOptionData op;
    op.name = "Opacity";
    op.checkable = false;
    op.strength = 1.0;
    op.sensors.sensors = {Sensor::withDefaults(SensorId::Pressure),
                          Sensor::withDefaults(SensorId::Fuzzy)};
    auto o = std::make_shared<BrushOptions>();
    o->opacity.emplace(std::move(op));
    o->smudgeRate.emplace(pressureOption("SmudgeRate", 0.9, true));
    o->colorRate.emplace(pressureOption("ColorRate", 0.4, true));
    p.options = std::move(o);

    BrushEngine eng;
    eng.begin(img.width, img.height, img, p, BrushDynamics{}, at(20, 30, 0.6));
    for (int i = 1; i <= 24; ++i)
        eng.extendTo(at(20 + 160.0 * i / 24, 30 + 6.0 * std::sin(i * 0.5), 0.6 + 0.4 * i / 24));
    eng.flush();
    eng.composite();
    eng.end();

    // The whole image folds into one checksum (order-sensitive), plus three probe pixels for a
    // readable failure. Blessed values from the vetted build; a legitimate engine change may
    // re-bless them, an accidental one may not.
    std::uint64_t sum = 1469598103934665603ULL; // FNV-1a
    for (const std::uint8_t b : img.rgba)
        sum = (sum ^ b) * 1099511628211ULL;
    const Color8 p1 = pixel(img, 90, 30);
    const Color8 p2 = pixel(img, 140, 33);
    const Color8 p3 = pixel(img, 60, 28);
    CAPTURE(sum);
    CAPTURE(static_cast<int>(p1.r));
    CAPTURE(static_cast<int>(p1.a));
    CAPTURE(static_cast<int>(p2.b));
    CAPTURE(static_cast<int>(p3.r));
    CHECK(sum == 16149243891476545587ULL);
}

TEST_CASE("smudge: smearing disables the sub-pixel phase (aligned reads)") {
    // Two identical strokes whose rows differ by a quarter pixel: with the phase disabled both
    // quantize to the SAME aligned masks and rows, so the images match exactly. With sub-pixel
    // placement live (as dulling keeps it) the two lay visibly different coverage.
    auto run = [](double y) {
        Image img(160, 48);
        fillRect(img, 0, 0, 64, 48, {200, 30, 30, 255});
        BrushEngine eng;
        stroke(eng, img, smudgeParams(), 20, y, 140, y);
        return img;
    };
    const Image a = run(24.1);
    const Image b = run(24.3);
    CHECK(a.rgba == b.rgba);
}

} // namespace

TEST_CASE("smudge: scatter rides the smudge walk -- the reference's colorsmudge scatters too") {
    // The reference applies its Scatter option to the dab position on colorsmudge exactly as on
    // the pixel brush (the smear anchor then tracks the scattered rect centres). Three shipped
    // presets author it. Structure on the canvas, or a blender shows nothing (§6.6c).
    using mosaic::core::brush::ScatterOption;
    using mosaic::core::brush::CurveOption;

    const auto scatteredParams = [](double strength) {
        BrushParams p = smudgeParams();
        p.seed = 41;
        CurveOptionData d;
        d.name = "Scatter";
        d.checked = true;
        d.strength = strength;
        d.strengthMax = 5.0;
        d.sensors.sensors = {Sensor::withDefaults(SensorId::Pressure)};
        auto o = std::make_shared<BrushOptions>();
        o->scatter = ScatterOption{CurveOption(d), true, true};
        p.options = o;
        return p;
    };

    const auto run = [](const BrushParams& p) {
        Image img(96, 64);
        fillRect(img, 0, 0, 40, 64, Color8{200, 30, 30, 255}); // the ink to drag
        BrushEngine eng;
        stroke(eng, img, p, 30.0, 32.0, 80.0, 32.0);
        return img;
    };

    const Image plain = run(smudgeParams());
    const Image scattered = run(scatteredParams(1.5));
    CHECK(scattered.rgba != plain.rgba); // the dabs really moved

    const Image again = run(scatteredParams(1.5));
    CHECK(again.rgba == scattered.rgba); // and still replay from the seed

    // Inert scatter: byte-identical to no option at all, stream included.
    BrushParams inertParams = smudgeParams();
    inertParams.seed = 41;
    {
        CurveOptionData d;
        d.name = "Scatter";
        d.checked = false; // authored off: must not perturb the stroke
        d.strength = 1.5;
        d.strengthMax = 5.0;
        d.sensors.sensors = {Sensor::withDefaults(SensorId::Pressure)};
        auto o = std::make_shared<BrushOptions>();
        o->scatter = ScatterOption{CurveOption(d), true, true};
        inertParams.options = o;
    }
    BrushParams plainSeeded = smudgeParams();
    plainSeeded.seed = 41;
    CHECK(run(inertParams).rgba == run(plainSeeded).rgba);
}
