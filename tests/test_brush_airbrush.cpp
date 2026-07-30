#include <doctest/doctest.h>

#include "common/image.hpp"
#include "core/brush/brush_engine.hpp"
#include "core/brush/brush_tip.hpp"
#include "io/brush/library.hpp"
#include "io/brush/preset_brush.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// THE AIRBRUSH (docs/brushes.md §6.6h): the stroke's second, TIME-driven dab cadence.
//
// ⚠ WHAT EACH CASE CAN SEE, before it was written:
//   * The INTERVAL cases read `airbrushIntervalMs` directly -- so they see the division by the Rate
//     value, the floor and the "never" sentinel, none of which a dab count can distinguish from a
//     brush that simply painted less.
//   * The CADENCE cases count DABS (`strokeState().dabIndex()`), which is the quantity the option
//     actually moves. A pixel metric could not: a timed dab landing on top of a distance dab at the
//     same opacity leaves the image unchanged, so "more dabs" is invisible in the bytes there.
//   * The DETERMINISM case runs the same sample stream twice and compares BYTES, then runs it again
//     with the timestamps changed and requires the bytes to MOVE. The second half is what makes the
//     first half mean something: a cadence that ignored the clock would pass "same input, same
//     output" trivially.
//   * The INERT case is the one that protects every existing golden, and it is a byte comparison
//     against a stroke whose airbrush block is simply off.
namespace cb = mosaic::core::brush;

using mosaic::common::Color8;
using mosaic::common::Image;

namespace {

[[nodiscard]] cb::StrokeInput at(double x, double y, std::uint64_t timeUs, double pressure = 1.0) {
    cb::StrokeInput in;
    in.pos = {x, y};
    in.pressure = pressure;
    in.timeUs = timeUs;
    return in;
}

[[nodiscard]] cb::BrushParams roundBrush() {
    cb::BrushParams p;
    p.diameter = 8.0;
    p.spacing = 0.5; // 4 px between dabs: coarse enough that the timed cadence can outrun it
    // A LOW flow, deliberately: at flow 1 the coverage saturates within a few overlapping dabs and
    // "twice as many dabs" prints the same bytes -- so a byte comparison could not see the cadence
    // at all. At 0.25 each extra dab still darkens the mark.
    p.flow = 0.25;
    p.color = Color8{0, 0, 0, 255};
    cb::MaskGeneratorParams g;
    g.diameter = 8.0;
    g.hFade = 1.0;
    g.vFade = 1.0;
    g.antialiasEdges = true;
    p.tip = cb::makeTip(g);
    return p;
}

// One straight 60 px stroke over `spanMs` milliseconds per sample. Returns (dabs, image).
struct Run {
    int dabs = 0;
    Image image;
};

[[nodiscard]] Run stroke(const cb::BrushParams& params, std::uint64_t stepUs) {
    Run r;
    r.image = Image(96, 32);
    cb::BrushEngine eng;
    eng.begin(96, 32, r.image, params, cb::BrushDynamics{}, at(10.0, 16.0, 0));
    for (int i = 1; i <= 5; ++i)
        eng.extendTo(at(10.0 + 12.0 * i, 16.0, stepUs * static_cast<std::uint64_t>(i)));
    eng.end();
    eng.composite();
    r.dabs = eng.strokeState().dabIndex() + 1; // beginDab() makes the first dab index 0
    return r;
}

// A stroke that MOVES ONCE and is then held perfectly still for `holdSamples` samples of `stepUs`.
// The held samples repeat the same position exactly, which is what the reference's tool
// synthesizes while a pointer rests.
[[nodiscard]] Run heldStroke(const cb::BrushParams& params, int holdSamples,
                             std::uint64_t stepUs) {
    Run r;
    r.image = Image(96, 32);
    cb::BrushEngine eng;
    eng.begin(96, 32, r.image, params, cb::BrushDynamics{}, at(20.0, 16.0, 0));
    eng.extendTo(at(24.0, 16.0, stepUs));
    std::uint64_t t = stepUs;
    for (int i = 0; i < holdSamples; ++i) {
        t += stepUs;
        eng.extendTo(at(24.0, 16.0, t));
    }
    eng.end();
    eng.composite();
    r.dabs = eng.strokeState().dabIndex() + 1;
    return r;
}

// Drive a stroke one sample at a time and record how many dabs EACH span laid. `travelPx` is the
// distance between consecutive samples and `stepUs` the time between them, so the two axes the
// timed cadence lives on can be swept independently.
[[nodiscard]] std::vector<int> perSpanDabs(const cb::BrushParams& params, double travelPx,
                                           std::uint64_t stepUs, int samples) {
    Image img(256, 64);
    cb::BrushEngine eng;
    eng.begin(256, 64, img, params, cb::BrushDynamics{}, at(8.0, 32.0, 0));
    std::vector<int> out;
    int seen = 0;
    double x = 8.0;
    std::uint64_t t = 0;
    for (int i = 0; i < samples; ++i) {
        x += travelPx;
        t += stepUs;
        eng.extendTo(at(x, 32.0, t));
        const int now = eng.strokeState().dabIndex() + 1;
        out.push_back(now - seen);
        seen = now;
    }
    eng.end();
    out.push_back(eng.strokeState().dabIndex() + 1 - seen);
    return out;
}

} // namespace

TEST_CASE("airbrush: NO span can lay more dabs than its elapsed time paid for") {
    // ⚠⚠ THE REGRESSION CASE FOR A USER-REPORTED HANG (§6.6h). The timed cadence lays
    // `elapsed / interval` dabs and the shipped airbrush presets author rate 1000 -- an interval of
    // ONE MILLISECOND -- so a two-second stall between two samples asked for 2000 dabs inside a
    // single extendTo(), and a bogus timestamp asked for a billion. The only thing that stopped the
    // loop was a 100,000-dab backstop, which is not a safety net: it IS the freeze.
    //
    // ⚠ THE ASSERTION IS A COUNT, NEVER A DURATION. A wall-clock bound would pass on a fast machine
    // and flake on a loaded one, and it would say nothing about WHY. `kMaxSpanBudgetMs /
    // kMinTimedIntervalMs` is the engine's own ceiling on what one span can pay for; the backstop
    // sits two above it, so a span that reaches the backstop fails here.
    const double hardCeiling = cb::kMaxSpanBudgetMs / cb::kMinTimedIntervalMs;

    cb::BrushParams air = roundBrush();
    air.airbrush.enabled = true;

    struct Case {
        const char* what;
        double travelPx;
        std::uint64_t stepUs;
        double rate;
    };
    // The sweep the fix has to survive: normal travel, tiny travel, travel just above the
    // zero-travel gate (the case the arc conversion was accused of exploding on), and the stalls
    // that actually did it -- each at the fastest rate the shipped set authors and at a slow one.
    const Case cases[] = {
        {"normal drag", 12.0, 16'700, 1000.0},
        {"slow drag", 1.0, 16'700, 1000.0},
        {"crawl", 0.01, 16'700, 1000.0},
        {"barely moving", 1e-6, 16'700, 1000.0},
        {"at the zero-travel gate", 1e-9, 16'700, 1000.0},
        {"a 2 second stall", 12.0, 2'000'000, 1000.0},
        {"a 20 second stall", 12.0, 20'000'000, 1000.0},
        {"a bogus timestamp", 12.0, 1'000'000'000, 1000.0},
        {"a stall at a slow rate", 12.0, 2'000'000, 20.0},
        {"a stall with no travel at all", 0.0, 2'000'000, 1000.0},
    };
    for (const Case& c : cases) {
        CAPTURE(c.what);
        cb::BrushParams p = air;
        p.airbrush.rate = c.rate;
        const std::vector<int> spans = perSpanDabs(p, c.travelPx, c.stepUs, 8);
        const double interval = cb::airbrushIntervalMs(c.rate, 1.0);
        // The budget invariant, per span: the timed dabs a span may lay are what its elapsed time
        // can pay for, and the budget itself saturates. `+ 1` is the boundary dab the carried
        // remainder arms; the distance cadence's own dabs are bounded by the travel, which is at
        // most a couple here.
        const int bound = static_cast<int>(cb::kMaxSpanBudgetMs / interval) + 4;
        for (const int n : spans) {
            CAPTURE(n);
            CHECK(n <= bound);
            CHECK(static_cast<double>(n) < hardCeiling);
        }
    }
}

TEST_CASE("airbrush: repeated stalls do not accumulate -- the carried remainder is clamped too") {
    // The second unbounded input: the remainder an earlier span could not spend is carried to the
    // next one, so without a clamp a run of stalls banks credit that a later span pays out. Clamped
    // to the same budget, every stalled span looks like every other -- the count must be FLAT, not
    // climbing. (A growing sequence here is the signature of a backlog, and it is the one thing a
    // single-span assertion cannot see.)
    cb::BrushParams air = roundBrush();
    air.airbrush.enabled = true;
    air.airbrush.rate = 1000.0;

    const std::vector<int> spans = perSpanDabs(air, 12.0, 2'000'000, 8);
    REQUIRE(spans.size() >= 6);
    // The first entries are the walk's one-sample lag warming up; compare the settled ones.
    for (std::size_t i = 3; i + 1 < spans.size(); ++i) {
        CAPTURE(i);
        CAPTURE(spans[i]);
        CHECK(spans[i] == spans[3]);
    }
}

TEST_CASE("airbrush: the budget saturates rather than dumping a stall's backlog") {
    // What the clamp MEANS, stated as behaviour: past the budget, more elapsed time buys no more
    // paint. A 2 s stall and a 20 s stall lay the same mark -- the engine refuses to dump either.
    cb::BrushParams air = roundBrush();
    air.airbrush.enabled = true;
    air.airbrush.rate = 1000.0;
    air.airbrush.ignoreSpacing = true; // every dab is a timed one, so the count is the cadence

    const Run twoSeconds = stroke(air, 2'000'000);
    const Run twentySeconds = stroke(air, 20'000'000);
    CHECK(twoSeconds.dabs == twentySeconds.dabs);
    CHECK(twoSeconds.image.rgba == twentySeconds.image.rgba);

    // ... and BELOW the budget the cadence is still the clock's, so the saturation is a ceiling and
    // not a constant. (Without this the case above would pass on an airbrush that ignored time.)
    const Run brief = stroke(air, 40'000);
    CHECK(brief.dabs < twoSeconds.dabs);
}

TEST_CASE("airbrush: the timed interval is 1000/rate, divided by the Rate value and floored") {
    // The reference's own arithmetic. `rate` is dabs per second, so the interval is its reciprocal
    // in milliseconds; the Rate option's per-dab value DIVIDES it, which is why a value below 1
    // spreads the dabs out rather than packing them.
    CHECK(cb::airbrushIntervalMs(1000.0, 1.0) == doctest::Approx(1.0));
    CHECK(cb::airbrushIntervalMs(20.0, 1.0) == doctest::Approx(50.0));
    CHECK(cb::airbrushIntervalMs(20.0, 0.5) == doctest::Approx(100.0));
    CHECK(cb::airbrushIntervalMs(20.0, 2.0) == doctest::Approx(25.0));

    // ⚠ A rate value of ZERO is "never", not "instantly" -- the reference substitutes a length of
    // time no stroke will last. Inverting that sense would make a zero-Rate airbrush the FASTEST
    // one there is, and would also spin the walk.
    CHECK(cb::airbrushIntervalMs(20.0, 0.0) > 1.0e9);
    CHECK(cb::airbrushIntervalMs(0.0, 1.0) > 1.0e9);

    // And the floor: half a millisecond, whatever the rate asks for.
    CHECK(cb::airbrushIntervalMs(1.0e9, 1.0) == doctest::Approx(cb::kMinTimedIntervalMs));
}

TEST_CASE("airbrush: an inert airbrush leaves the stroke BYTE-IDENTICAL") {
    // The hard rule (§6.2). Every golden in the suite was laid by the distance cadence alone, and
    // an airbrush block that is off must not move one byte of it.
    const cb::BrushParams plain = roundBrush();
    const Run reference = stroke(plain, 20'000);

    cb::BrushParams off = plain;
    off.airbrush.enabled = false;
    off.airbrush.rate = 1000.0; // a rate the gate must ignore
    const Run same = stroke(off, 20'000);
    CHECK(same.image.rgba == reference.image.rgba);
    CHECK(same.dabs == reference.dabs);

    // A rate of zero is not an airbrush either: the gate refuses it rather than dividing by it.
    cb::BrushParams zero = plain;
    zero.airbrush.enabled = true;
    zero.airbrush.rate = 0.0;
    CHECK(stroke(zero, 20'000).image.rgba == reference.image.rgba);
}

TEST_CASE("airbrush: the timed cadence adds dabs a distance cadence would not have laid") {
    const cb::BrushParams plain = roundBrush();
    const Run byDistance = stroke(plain, 20'000); // 20 ms per 12 px sample

    cb::BrushParams air = plain;
    air.airbrush.enabled = true;
    air.airbrush.rate = 500.0; // one dab every 2 ms -- far faster than 4 px of travel takes
    const Run byTime = stroke(air, 20'000);
    CHECK(byTime.dabs > byDistance.dabs);

    // ... and it is the CLOCK doing it, not the rate merely enabling something else: the same
    // geometry walked ten times SLOWER lays far more timed dabs, because the stroke had ten times
    // as long to breathe. A cadence that read anything but the samples' timestamps could not tell
    // these two strokes apart at all -- their positions and pressures are identical.
    const Run slower = stroke(air, 200'000);
    CHECK(slower.dabs > byTime.dabs);

    // The distance cadence, by contrast, does NOT care how long the stroke took.
    CHECK(stroke(plain, 200'000).dabs == byDistance.dabs);
}

TEST_CASE("airbrush: `ignoreSpacing` switches the distance cadence off entirely") {
    cb::BrushParams air = roundBrush();
    air.airbrush.enabled = true;
    air.airbrush.rate = 20.0; // one dab every 50 ms
    const Run both = stroke(air, 20'000);

    cb::BrushParams timedOnly = air;
    timedOnly.airbrush.ignoreSpacing = true;
    const Run timed = stroke(timedOnly, 20'000);

    // 60 px at 4 px spacing is ~15 distance dabs; 100 ms at one per 50 ms is 2 timed ones. With the
    // distance cadence off, only the timed dabs remain -- which is a LARGE drop, not a small one,
    // so a flag read the wrong way round cannot hide inside sampling noise.
    CHECK(timed.dabs < both.dabs);
    CHECK(timed.dabs <= 4);
    CHECK(both.dabs > 10);
}

TEST_CASE("airbrush: the mark is a pure function of the SAMPLES, timestamps included") {
    // ⚠ THE REPLAY CONTRACT, and the reason the cadence reads `StrokeInput::timeUs` rather than a
    // clock. Same samples -> same bytes, every time; and the second half proves the first is not
    // vacuous by moving ONLY the timestamps and requiring the bytes to move with them.
    cb::BrushParams air = roundBrush();
    air.airbrush.enabled = true;
    air.airbrush.rate = 200.0;

    const Run first = stroke(air, 20'000);
    const Run second = stroke(air, 20'000);
    CHECK(first.image.rgba == second.image.rgba);
    CHECK(first.dabs == second.dabs);

    const Run slower = stroke(air, 60'000);
    CHECK_FALSE(slower.image.rgba == first.image.rgba);
}

TEST_CASE("airbrush: a HELD pointer keeps laying dabs, and only under the airbrush") {
    // The behaviour the option is named for. A stroke that moves 4 px and is then held still for
    // 20 samples of 10 ms lays exactly the dabs the distance cadence earned -- unless the airbrush
    // is on, in which case the clock keeps laying them at the rest point.
    cb::BrushParams plain = roundBrush();
    const Run still = heldStroke(plain, 20, 10'000);

    cb::BrushParams air = plain;
    air.airbrush.enabled = true;
    air.airbrush.rate = 100.0; // one dab every 10 ms: roughly one per held sample
    const Run pumped = heldStroke(air, 20, 10'000);
    CHECK(pumped.dabs > still.dabs + 10);

    // ⚠ AND THE HELD DABS LAND AT THE REST POINT, not strung along the path: everything the stroke
    // painted must sit inside the tip's own footprint around (24, 16). (A pump that walked the
    // dabs along the span would spill outside it -- which is exactly the bug a dab COUNT cannot
    // see.)
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 96; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * 96 + x) * 4;
            if (pumped.image.rgba[i + 3] == 0)
                continue;
            CAPTURE(x);
            CAPTURE(y);
            CHECK(x >= 14);
            CHECK(x <= 30);
        }
    }
}

TEST_CASE("airbrush: the Rate option scales the timed cadence per dab") {
    cb::BrushParams air = roundBrush();
    air.airbrush.enabled = true;
    air.airbrush.ignoreSpacing = true; // isolate the timed cadence
    air.airbrush.rate = 200.0;
    const Run full = stroke(air, 40'000);

    // A checked Rate at half strength halves the value, so the interval DOUBLES and roughly half
    // as many dabs land. (Unchecked, the option contributes exactly 1.0 and changes nothing --
    // `KisStandardOption::apply`'s own rule, which is what `standardOptionValue` is.)
    auto options = std::make_shared<cb::BrushOptions>();
    cb::CurveOptionData d;
    d.name = "Rate";
    d.checked = true;
    d.strength = 0.5;
    d.sensors.sensors = {cb::Sensor::withDefaults(cb::SensorId::Pressure)};
    options->rate.emplace(d);
    cb::BrushParams halved = air;
    halved.options = options;
    const Run half = stroke(halved, 40'000);
    CHECK(half.dabs < full.dabs);
    CHECK(half.dabs * 2 >= full.dabs - 2);

    // An UNCHECKED Rate contributes the identity, not its strength.
    auto unchecked = std::make_shared<cb::BrushOptions>();
    cb::CurveOptionData u = d;
    u.checked = false;
    unchecked->rate.emplace(u);
    cb::BrushParams inert = air;
    inert.options = unchecked;
    CHECK(stroke(inert, 40'000).dabs == full.dabs);
}

TEST_CASE("airbrush corpus: only the presets the REFERENCE airbrushes reach the timed cadence") {
    // ⚠⚠ THIS CASE USED TO PIN THE BUG AS THE CONTRACT. It asserted 10 carriers and said in a
    // comment that "reading only the modern keys would leave [six of them] all disabled" -- which
    // is exactly what the reference does. Its airbrush reader reads `PaintOpSettings/isAirbrushing`,
    // `PaintOpSettings/rate` and `PaintOpSettings/ignoreSpacing`, and NOTHING in it reads the
    // Krita-2-era `AirbrushOption/*` prefix, so a file carrying only those keys does not airbrush
    // there at all. Honouring them ran a dab-every-millisecond cadence under a 600 px soft nib and
    // froze the program (docs/brushes.md §6.6h).
    static const mosaic::io::brush::PresetLibrary lib = [] {
        mosaic::io::brush::PresetLibrary l;
        std::string error;
        const int n = l.addBundleFile(
            std::string(MOSAIC_SHIPPED_DATA_DIR) + "/brushes/Krita_4_Default_Resources.bundle",
            &error);
        REQUIRE_MESSAGE(n == 117, error);
        return l;
    }();

    std::vector<std::string> airbrushing;
    for (const mosaic::io::brush::LibraryPreset& p : lib.presets()) {
        if (!p.preset.airbrush.enabled)
            continue;
        airbrushing.push_back(p.preset.name);
        CAPTURE(p.preset.name);
        const cb::BrushParams bp = mosaic::io::brush::presetBrushParams(p);
        CHECK(bp.airbrush.enabled);
        CHECK(bp.airbrush.rate == p.preset.airbrush.rate);
    }
    // FOUR across the whole set, and every one of them spells the modern keys: one `paintbrush`
    // (y)_Texture_Spray) and three `spraybrush`es. The other six of the bundle's ten "carriers"
    // spell only `AirbrushOption/*` and are plain distance-cadence brushes upstream.
    const std::vector<std::string> expected{"y)_Texture_Spray", "y)_Texture_Starfield",
                                            "z)_Stamp_Hearts", "z)_Stamp_Shoujo_Bubbles"};
    std::sort(airbrushing.begin(), airbrushing.end());
    CHECK(airbrushing == expected);

    const auto find = [&](std::string_view name) -> const mosaic::io::brush::LibraryPreset* {
        for (const mosaic::io::brush::LibraryPreset& p : lib.presets())
            if (p.preset.name == name)
                return &p;
        return nullptr;
    };
    // ⚠ THE PRESET THE USER REACHES FOR, and the one that hung: it is NOT an airbrush here, because
    // it is not one there. Its `AirbrushOption/rate` of 1000 -- the maximum the reference's own
    // slider allows, one dab per millisecond -- is read by nobody, and the rate field keeps the
    // reference reader's default rather than the file's number.
    const mosaic::io::brush::LibraryPreset* soft = find("b)_Airbrush_Soft");
    REQUIRE(soft != nullptr);
    CHECK_FALSE(soft->preset.airbrush.enabled);
    CHECK(soft->preset.airbrush.rate == doctest::Approx(20.0));
    // ... and the dead key costs it no fidelity, because the reference ignores it too. (Its own
    // stale `SmudgeRate` is the same story -- §6.6i.)
    CHECK(soft->preset.provenance.fidelity == mosaic::io::brush::PresetFidelity::Exact);

    // The modern keys ARE read, all three of them, so the case above is not passing by reading
    // nothing at all.
    const mosaic::io::brush::LibraryPreset* spray = find("y)_Texture_Spray");
    REQUIRE(spray != nullptr);
    CHECK(spray->preset.airbrush.enabled);
    CHECK(spray->preset.airbrush.rate == doctest::Approx(1000.0)); // PaintOpSettings/rate
    CHECK_FALSE(spray->preset.airbrush.ignoreSpacing);
}
