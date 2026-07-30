#include "common/image.hpp"
#include "core/brush/brush_engine.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <doctest/doctest.h>
#include <string>
#include <vector>

// Golden pin for the `Uniform x Wash` accumulation path (docs/brushes.md §6.1).
//
// The S19 accumulator rework replaces the single-channel coverage buffer + fixed-colour composite
// with `StrokeAccumulator {Uniform, Colored}` x `PaintMode {Wash, Buildup}`. `Uniform x Wash` IS
// today's engine, and §6.1 requires it to come out of that rework BYTE-IDENTICAL. These hashes
// record that requirement against the pre-rework engine, so the rework cannot quietly move a pixel.
//
// Why hashes rather than the usual `alpha > 0` probes: the existing engine tests
// (test_brush_engine) assert behaviour in ranges, so a systematic drift of a few 8-bit levels --
// exactly what a reassociated blend expression produces -- passes all of them. A hash over the
// whole document catches any moved pixel, including ones outside the dirty rect that should never
// have been written at all.
//
// Two hashes, deliberately pinned at different fidelities:
//   * the composited RGBA is hashed BYTE-EXACT -- it is the output the §6.1 requirement is about,
//     and it was verified to hash identically at -O0 and -O3 before these values were recorded.
//   * the coverage buffer is hashed at 16-bit FIXED POINT, not over its raw IEEE-754 float bytes.
//     Coverage is an internal mask (the Inpaint brush reads it), so pinning float bit patterns
//     would over-specify it: any future FP-affecting build flag would fail the golden without a
//     single pixel having moved, and a golden that cries wolf stops being read. A 1/65535 step is
//     still orders of magnitude finer than any real accumulation regression -- a mere 0.1 % drift
//     in the build-up term moves it.
// Coverage's [0,1] range is asserted separately rather than clamped into the hash, so a buffer that
// escapes its range is caught rather than rounded out of sight.
//
// If one of these fails: do NOT re-bless the hash until you know which pixel moved and why. The
// point of the test is that `Uniform x Wash` has no licence to change.
//
// One case is expected to change at S19 step 7, when spacing starts keying off the effective
// (pressure-scaled) size (§6.2, a deliberate behaviour change): "pressure drives size and flow" is
// the only case here that enables `sizeFromPressure`, so it is the only one whose dab placement
// moves. Re-blessing it there is correct; re-blessing any other case is a bug.
namespace {

using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::common::Rect;
using mosaic::common::Vec2;
using mosaic::core::brush::BrushDynamics;
using mosaic::core::brush::BrushEngine;
using mosaic::core::brush::BrushParams;
using mosaic::core::brush::StrokeInput;

std::uint64_t fnv1a(const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Coverage at 16-bit fixed point, fed in a fixed byte order so the hash does not depend on the
// host's endianness either. Callers must have checked the [0,1] range first (see coverageInRange).
std::uint64_t hashCoverage(const std::vector<float>& cov) {
    std::uint64_t h = 1469598103934665603ull;
    const auto feed = [&h](std::uint8_t b) {
        h ^= b;
        h *= 1099511628211ull;
    };
    for (float c : cov) {
        const double v = c < 0.0f ? 0.0 : (c > 1.0f ? 1.0 : static_cast<double>(c));
        const auto q = static_cast<std::uint16_t>(std::lround(v * 65535.0));
        feed(static_cast<std::uint8_t>(q >> 8));
        feed(static_cast<std::uint8_t>(q & 0xFF));
    }
    return h;
}

bool coverageInRange(const std::vector<float>& cov) {
    for (float c : cov)
        if (!(c >= 0.0f && c <= 1.0f)) // also false for NaN
            return false;
    return true;
}

std::string hex(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(v));
    return buf;
}

struct StrokeResult {
    std::uint64_t image = 0; // hash of the WHOLE document's RGBA -- catches strays outside the dab
    std::uint64_t coverage = 0; // 16-bit hash of the coverage buffer (the Inpaint brush's mask)
    std::uint32_t cw = 0, ch = 0;
    std::int32_t ox = 0, oy = 0;
    Rect dirty;
    std::size_t painted =
        0;     // pixels with coverage > 0 -- guards against hashing a degenerate buffer
    Image img; // kept so a case can probe a pixel as well as pin the hash
};

// Run one stroke: `samples[0]` is the press, the rest are motion. composite() once at the end.
StrokeResult runStroke(const BrushParams& p, const BrushDynamics& d,
                       const std::vector<StrokeInput>& samples, Color8 baseFill,
                       std::uint32_t w = 96, std::uint32_t h = 96) {
    Image img(w, h);
    img.fill(baseFill);
    BrushEngine eng;
    eng.begin(w, h, img, p, d, samples.front());
    for (std::size_t i = 1; i < samples.size(); ++i)
        eng.extendTo(samples[i]);
    eng.flush(); // the walk lags one sample; lay the tail span before hashing the pixels
    StrokeResult r;
    r.dirty = eng.composite();
    const auto& cov = eng.coverage();
    for (float c : cov)
        if (c > 0.0f)
            ++r.painted;
    // Checked here rather than folded into the hash: coverage that escapes [0,1] must fail loudly,
    // not be quietly clamped into a value that still hashes to the golden.
    CHECK_MESSAGE(coverageInRange(cov), "coverage escaped [0,1]");
    r.image = fnv1a(img.rgba.data(), img.rgba.size());
    r.coverage = hashCoverage(cov);
    r.cw = eng.coverageWidth();
    r.ch = eng.coverageHeight();
    r.ox = eng.coverageOriginX();
    r.oy = eng.coverageOriginY();
    eng.end();
    r.img = std::move(img);
    return r;
}

Color8 pixel(const Image& img, int x, int y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

// A curved pressure-varying path reused by several cases.
std::vector<StrokeInput> curvedPath() {
    return {StrokeInput{{12.0, 20.0}, 0.15}, StrokeInput{{40.0, 33.0}, 0.7},
            StrokeInput{{70.0, 55.0}, 1.0}, StrokeInput{{80.0, 84.0}, 0.4}};
}

} // namespace

TEST_CASE("wash golden: hard opaque dab over transparent") {
    // Pins dabCoverage's solid core + the guaranteed ~0.75 px AA rim, and source-over onto alpha 0.
    BrushParams p;
    p.diameter = 20.0;
    p.hardness = 1.0;
    p.color = Color8{200, 50, 25, 255};
    const StrokeResult r =
        runStroke(p, BrushDynamics{}, {StrokeInput{{48.0, 48.0}, 1.0}}, Color8{0, 0, 0, 0});
    REQUIRE(r.painted > 200); // a 20 px tip really did paint
    INFO("image=" << hex(r.image) << " coverage=" << hex(r.coverage));
    CHECK(r.image == 0x3d10545768d7a78bull);
    CHECK(r.coverage == 0x5c156f818471b36bull);
    CHECK(pixel(r.img, 48, 48) == p.color); // solid core carries the active colour exactly
}

TEST_CASE("wash golden: soft low-flow stroke over an opaque base") {
    // The shoulder + flow build-up + the RGB blend path (an opaque base makes colour mixing
    // matter).
    BrushParams p;
    p.diameter = 33.0;
    p.hardness = 0.0;
    p.flow = 0.25;
    p.opacity = 0.8;
    p.spacing = 0.07;
    p.color = Color8{13, 200, 77, 255};
    const StrokeResult r = runStroke(p, BrushDynamics{}, curvedPath(), Color8{40, 60, 90, 255});
    REQUIRE(r.painted > 2000);
    INFO("image=" << hex(r.image) << " coverage=" << hex(r.coverage));
// ⚠ RE-BLESSED 2026-07-11, deliberately: the dab walk lays dabs along a CURVE through the samples
// (core/brush/stroke_path.hpp) instead of along straight chords between them. Chords made a 60 Hz
// mouse stroke a literal 60-gon; the curve is what fixes it, and it necessarily MOVES dab positions
// on any path that bends.
//
// What did NOT move, and must not: every STRAIGHT stroke and every single-dab golden in this file is
// byte-for-byte unchanged. A straight path flattens to the one chord it always was and runs the very
// same walk arithmetic (test_brush_stroke_path.cpp pins that identity). Only the goldens whose paths
// genuinely curve -- curvedPath() and the self-crossing `cross` -- moved. If a straight-path golden
// ever moves, that is a bug, not a re-bless.
    CHECK(r.image == 0x9b06d3b46206a424ull);
    CHECK(r.coverage == 0x186dfcd3f338818cull);
}

TEST_CASE("wash golden: pressure drives size and flow") {
    // effectiveDiameter/effectiveFlow, and the per-dab pressure lerp along each segment.
    //
    // Re-blessed ONCE, at S19 step 7, exactly as the header promised: spacing now keys off the
    // effective (pressure-scaled) size, so this case's dab cadence tightens as its pressure ramps
    // and every dab past the press moved. Every other case passed unchanged in the same run, and
    // re-blessing any of them remains a bug.
    BrushParams p;
    p.diameter = 17.5;
    p.hardness = 0.55;
    p.flow = 0.6;
    p.opacity = 0.9;
    p.spacing = 0.23;
    p.color = Color8{255, 255, 255, 200}; // colour alpha < 255 exercises the opacity*color.a cap
    BrushDynamics d;
    d.sizeFromPressure = true;
    d.flowFromPressure = true;
    const StrokeResult r = runStroke(p, d, curvedPath(), Color8{40, 60, 90, 255});
    REQUIRE(r.painted > 500);
    INFO("image=" << hex(r.image) << " coverage=" << hex(r.coverage));
    // Re-blessed with the curved dab walk -- see the note on the first golden in this file.
    CHECK(r.image == 0xa1f1739127e05304ull);
    CHECK(r.coverage == 0xb1da416828011b2aull);
}

TEST_CASE("wash golden: a dark colour on a nearly transparent backdrop") {
    // The composite resolves the source colour in DOUBLE. Routing Normal through the blend path
    // instead -- `(1 - ba) * fr + ba * blendChannel(Normal, br, fr)` -- is algebraically the
    // identity, but `blendChannel` is float, so the source colour makes a round-trip through
    // `float` and comes back a few ulps off. A brute-force sweep of every 8-bit source channel and
    // backdrop alpha finds 58278 combinations where that flips an output byte; this is one of them.
    //
    // opacity 13/64 is exactly representable, so the stroke alpha lands on the counterexample:
    // colour 1 over a backdrop of 34 at alpha 1/255 composites to 1, and to 2 through the float
    // path. Verified to be the PRE-REWORK engine's value by building the old brush_engine.cpp.
    BrushParams p;
    p.diameter = 12.0;
    p.hardness = 1.0;
    p.flow = 1.0;
    p.opacity = 0.203125;
    p.color = Color8{1, 1, 1, 255};
    const StrokeResult r = runStroke(p, BrushDynamics{}, {StrokeInput{{16.0, 16.0}, 1.0}},
                                     Color8{34, 34, 34, 1}, 32, 32);
    REQUIRE(r.painted > 60);
    CHECK(pixel(r.img, 16, 16) == Color8{1, 1, 1, 53});
    INFO("image=" << hex(r.image) << " coverage=" << hex(r.coverage));
    CHECK(r.image == 0xeab5c55bbed8db03ull);
    CHECK(r.coverage == 0x8ac23df8c94affa3ull);
}

TEST_CASE("wash golden: a self-crossing stroke is capped at opacity") {
    // The stroke crosses itself; Wash caps the whole accumulated coverage at `opacity` exactly
    // once. This is the invariant Buildup is defined by NOT having -- pin it before the mode axis
    // exists.
    BrushParams p;
    p.diameter = 18.0;
    p.hardness = 0.9;
    p.flow = 1.0;
    p.opacity = 0.5;
    p.spacing = 0.05;
    p.color = Color8{0, 0, 0, 255};
    const std::vector<StrokeInput> cross{
        StrokeInput{{20.0, 20.0}, 1.0}, StrokeInput{{76.0, 76.0}, 1.0},
        StrokeInput{{76.0, 20.0}, 1.0}, StrokeInput{{20.0, 76.0}, 1.0}};
    const StrokeResult r = runStroke(p, BrushDynamics{}, cross, Color8{255, 255, 255, 255});
    REQUIRE(r.painted > 2000);
    INFO("image=" << hex(r.image) << " coverage=" << hex(r.coverage));
    // Re-blessed with the curved dab walk -- see the note on the first golden in this file. The
    // self-crossing path turns hard corners, so its dabs move the most of any golden here.
    CHECK(r.image == 0xca09dbd0b8a0b9d0ull);
    CHECK(r.coverage == 0xf864be0a0dbbe33bull);
    // (48,48) is the crossing: coverage saturates to 1 there, so the cap alone decides the pixel.
    // Black at opacity 0.5 over white = lround(127.5) = 128 -- emphatically not 0 (uncapped
    // buildup).
    CHECK(pixel(r.img, 48, 48) == Color8{128, 128, 128, 255});
}

TEST_CASE("wash golden: a dab clipped by the document corner") {
    // Most of the tip is off-canvas; the in-bounds quarter paints and nothing is written out of it.
    BrushParams p;
    p.diameter = 20.0;
    p.hardness = 0.7;
    p.color = Color8{10, 20, 30, 255};
    const StrokeResult r =
        runStroke(p, BrushDynamics{}, {StrokeInput{{0.0, 0.0}, 1.0}}, Color8{0, 0, 0, 0}, 32, 32);
    REQUIRE(r.painted > 20);
    INFO("image=" << hex(r.image) << " coverage=" << hex(r.coverage));
    CHECK(r.image == 0x797c8379f64dcb0dull);
    CHECK(r.coverage == 0x412fc2cff51dc8ceull);
}

TEST_CASE("wash golden: a sub-pixel tip still marks its centre pixel") {
    // `stamp()` floors the coverage radius at 0.6 px so a 1 px tip deposits onto the pixel it is
    // centred on rather than vanishing. Nothing else in this file has an effective diameter under
    // 1.2 px, so without this case the floor is unpinned -- a mutation lowering it to 0.5 survives.
    BrushParams p;
    p.diameter = 1.0;
    p.hardness = 1.0;
    p.color = Color8{0, 0, 0, 255};
    // Centred exactly on pixel (48,48)'s centre: that pixel sits at d = 0, its neighbours at d
    // = 1.0, which is outside the 0.6 radius. Exactly one pixel may be painted.
    const StrokeResult r =
        runStroke(p, BrushDynamics{}, {StrokeInput{{48.5, 48.5}, 1.0}}, Color8{0, 0, 0, 0});
    CHECK(r.painted == 1);
    // R = 0.6, hardness 1 -> the AA rim widens the shoulder to 0.75, so t = 0.6/0.75 = 0.8 and
    // coverage = smoothstep(0.8) = 0.896 -> lround(0.896 * 255) = 228. At the mutated floor of 0.5
    // it would be 189.
    CHECK(pixel(r.img, 48, 48).a == 228);
    INFO("image=" << hex(r.image) << " coverage=" << hex(r.coverage));
    CHECK(r.image == 0xb4bd35ddfed9ef2full);
    CHECK(r.coverage == 0x1b4ba7bf9e8ec03full);
}

TEST_CASE("wash golden: the working rect tiles the same way on a large document") {
    // ensureCovers() grows the working rect in 256 px tiles, and the Inpaint brush (S39) maps
    // coverage indices through (originX/Y). On a 96x96 document every rect clamps to the whole
    // layer, so the tiling is invisible; here it is not. This dab's centre is chosen so that each
    // of the four bounds sits within 1 px of a tile edge -- shrinking the dab's bbox padding by
    // half a pixel moves cw, ch, ox and oy all at once, even though not one painted pixel changes.
    BrushParams p;
    p.diameter = 12.0;
    p.color = Color8{255, 0, 0, 255};
    const StrokeResult r = runStroke(p, BrushDynamics{}, {StrokeInput{{249.2, 262.7}, 1.0}},
                                     Color8{0, 0, 0, 0}, 1024, 1024);
    REQUIRE(r.painted > 80);
    CHECK(r.ox == 0);
    CHECK(r.oy == 0);
    CHECK(r.cw == 512u);
    CHECK(r.ch == 512u);
    INFO("image=" << hex(r.image) << " coverage=" << hex(r.coverage));
    CHECK(r.image == 0x8dcda83980e60ad1ull);
    CHECK(r.coverage == 0x02377103dc4cc158ull);
}

TEST_CASE("wash golden: the bounded working rect is placed where it was") {
    // The coverage buffer's geometry is part of the contract -- the Inpaint brush (S39) maps its
    // indices through (originX/Y). A rework that changed the tiling would move every index.
    BrushParams p;
    p.diameter = 12.0;
    const StrokeResult r = runStroke(p, BrushDynamics{}, curvedPath(), Color8{0, 0, 0, 0});
    CHECK(r.cw == 96u); // a 96x96 doc is one 256 px tile clamped to the layer
    CHECK(r.ch == 96u);
    CHECK(r.ox == 0);
    CHECK(r.oy == 0);
    CHECK(r.dirty.x == doctest::Approx(6.0));
    CHECK(r.dirty.y == doctest::Approx(14.0));
    CHECK(r.dirty.right() == doctest::Approx(86.0));
    // 90 -> 89: the curve bows the stroke's tail slightly differently than the chord did, so its
    // footprint is one pixel shorter. The origin and the other three edges are untouched.
    CHECK(r.dirty.bottom() == doctest::Approx(89.0));
}

TEST_CASE("wash golden: incremental composites equal one composite at the end") {
    // The canvas composites every frame; a batch boundary must not change a single byte. This is an
    // equivalence rather than a literal -- it stays true by construction if the rework is correct.
    BrushParams p;
    p.diameter = 22.0;
    p.hardness = 0.4;
    p.flow = 0.35;
    p.opacity = 0.75;
    p.spacing = 0.09;
    p.color = Color8{90, 30, 160, 255};
    const std::vector<StrokeInput> path = curvedPath();

    Image incremental(96, 96);
    incremental.fill(Color8{200, 190, 180, 255});
    BrushEngine a;
    a.begin(96, 96, incremental, p, BrushDynamics{}, path[0]);
    a.composite(); // composite after the press dab...
    for (std::size_t i = 1; i < path.size(); ++i) {
        a.extendTo(path[i]);
        a.composite(); // ... and after every motion sample
    }
    a.flush(); // ... and the tail span, which arrives with no lookahead sample behind it
    a.composite();
    a.end();

    Image oneShot(96, 96);
    oneShot.fill(Color8{200, 190, 180, 255});
    BrushEngine b;
    b.begin(96, 96, oneShot, p, BrushDynamics{}, path[0]);
    for (std::size_t i = 1; i < path.size(); ++i)
        b.extendTo(path[i]);
    b.flush();
    b.composite();
    b.end();

    CHECK(fnv1a(incremental.rgba.data(), incremental.rgba.size()) ==
          fnv1a(oneShot.rgba.data(), oneShot.rgba.size()));
}

TEST_CASE("wash golden: restore puts every byte back") {
    // restore() must return the target to its pre-stroke bytes exactly -- the paint tool relies on
    // it to capture the correct "old" region for the undo command.
    BrushParams p;
    p.diameter = 25.0;
    p.hardness = 0.3;
    p.flow = 0.5;
    p.color = Color8{7, 240, 120, 190};

    Image img(96, 96);
    img.fill(Color8{33, 44, 55, 200});
    const std::uint64_t before = fnv1a(img.rgba.data(), img.rgba.size());

    BrushEngine eng;
    eng.begin(96, 96, img, p, BrushDynamics{}, curvedPath()[0]);
    for (const StrokeInput& s : curvedPath())
        eng.extendTo(s);
    eng.composite();
    REQUIRE(fnv1a(img.rgba.data(), img.rgba.size()) != before); // it really painted
    eng.restore();
    eng.end();
    CHECK(fnv1a(img.rgba.data(), img.rgba.size()) == before);
}

TEST_CASE("wash golden: restore reverts a stroke composited frame by frame") {
    // The case above composites once and then restores, which is not what the canvas does. The
    // canvas composites EVERY frame, so by the time a stroke is cancelled the target already holds
    // painted pixels from many earlier batches -- and dabs stamped after a composite() land on
    // pixels the engine has already written. Only a base snapshot taken on the stroke's first touch
    // of a pixel (never refreshed from the painted target) can revert those.
    //
    // The stroke is long enough to cross 256 px tile boundaries, so ensureCovers() re-homes the
    // coverage and base buffers mid-stroke: the early batches' snapshots must survive being moved.
    BrushParams p;
    p.diameter = 30.0;
    p.hardness = 0.35;
    p.flow = 0.45;
    p.opacity = 0.85;
    p.spacing = 0.08;
    p.color = Color8{7, 240, 120, 190};

    Image img(1024, 1024);
    img.fill(Color8{33, 44, 55, 200});
    const std::uint64_t before = fnv1a(img.rgba.data(), img.rgba.size());

    const std::vector<StrokeInput> path{
        StrokeInput{{100.0, 100.0}, 0.3}, StrokeInput{{400.0, 300.0}, 0.9},
        StrokeInput{{700.0, 600.0}, 1.0}, StrokeInput{{300.0, 800.0}, 0.5}};

    BrushEngine eng;
    eng.begin(1024, 1024, img, p, BrushDynamics{}, path[0]);
    eng.composite(); // ... after the press dab
    for (std::size_t i = 1; i < path.size(); ++i) {
        eng.extendTo(path[i]);
        eng.composite(); // ... and after every motion sample, as the canvas does
    }
    REQUIRE(fnv1a(img.rgba.data(), img.rgba.size()) != before); // it really painted
    REQUIRE(eng.coverageWidth() > 512u);                        // the working rect really did grow
    eng.restore();
    eng.end();
    CHECK(fnv1a(img.rgba.data(), img.rgba.size()) == before); // not one byte of residue
}
