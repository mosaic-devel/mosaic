#include <doctest/doctest.h>

#include "common/image.hpp"
#include "core/brush/bitmap_tip.hpp"
#include "core/brush/brush_engine.hpp"
#include "core/brush/brush_tip.hpp"
#include "core/brush/stroke_preview.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <utility>
#include <vector>

// THE PER-STROKE RANDOM SEED (docs/brushes.md §6.6i) -- a USER-REPORTED bug: "single dabs don't take
// the next shape, it's one shape only on dabs", on `t)_Shapes_Mecha`, whose tip is a 23-cell hose
// with `sel0:random`.
//
// ⚠ WHAT EACH CASE CAN SEE, before it was written:
//   * The SEED cases read `strokeSeedFor` directly, so they see that each channel of the first
//     sample reaches the mix -- which a dab count could never distinguish from "the RNG is fine".
//   * The FRAME cases lay SINGLE-DAB strokes and read which hose cell each one stamped. A
//     multi-dab stroke cannot see this bug at all: the stream advances within a stroke, so the
//     cells vary there whatever the seed is. The report is about the FIRST draw of a stroke, and a
//     tap is exactly one first draw.
//   * The frame is read as the dab's own coverage: each of the four cells is a solid block of its
//     own grey, so the mark's alpha names the cell that made it. (A cell INDEX is not observable
//     from outside the engine, and asserting on one would test HoseState -- which already has its
//     own tests -- rather than the seeding.)
//   * The OFF cases are what protects every golden in the suite: with the flag clear the stroke
//     must be the stroke it always was, to the byte.
namespace cb = mosaic::core::brush;

using mosaic::common::Color8;
using mosaic::common::Image;

namespace {

[[nodiscard]] cb::StrokeInput at(double x, double y, std::uint64_t timeUs = 0) {
    cb::StrokeInput in;
    in.pos = {x, y};
    in.pressure = 1.0;
    in.timeUs = timeUs;
    return in;
}

// One solid 8x8 cell in the TIP IMAGE convention (white = no paint), so cell `k`'s coverage is
// `255 - grey[k]` at every pixel of the mark it stamps.
constexpr std::uint8_t kCellGrey[4] = {0, 60, 120, 180};

[[nodiscard]] cb::TipFrame solidCell(std::uint8_t grey) {
    cb::TipFrame f;
    f.width = 8;
    f.height = 8;
    f.rgba.assign(8u * 8u * 4u, std::uint8_t{255});
    for (std::size_t i = 0; i < 64u; ++i) {
        f.rgba[i * 4] = grey;
        f.rgba[i * 4 + 1] = grey;
        f.rgba[i * 4 + 2] = grey;
    }
    return f;
}

// A four-cell hose that picks its cell at RANDOM, exactly as `shapes_mech_random.gih` does:
// `dim:1 ncells:4 rank0:4 sel0:random`, so the stride is 4/4 == 1 and the cell IS the draw.
[[nodiscard]] cb::BrushParams randomHoseBrush() {
    std::vector<cb::TipFrame> frames;
    for (const std::uint8_t g : kCellGrey)
        frames.push_back(solidCell(g));
    cb::HoseParams hose;
    hose.dim = 1;
    hose.declaredCells = 4;
    hose.rank[0] = 4;
    hose.selection[0] = cb::FrameSelection::Random;

    cb::BrushParams p;
    p.diameter = 8.0;  // the cells are 8x8, so a dab resamples them 1:1
    p.spacing = 0.5;   // 4 px between dabs
    p.flow = 1.0;
    p.opacity = 1.0;
    p.color = Color8{0, 0, 0, 255};
    p.tip = cb::makeTip(std::make_shared<const cb::BitmapTip>(
        std::move(frames), cb::TipApplication::AlphaMask, cb::TipSourceKind::Mask,
        cb::TipAdjustments{}, hose));
    return p;
}

// ONE dab, and exactly one: `begin()` lays the press dab itself, and a stroke that never moves has
// no span for `end()` to flush -- which is precisely a TAP. Returns the mark's peak alpha, which
// names the cell: a solid cell stays solid through the resample, so its interior carries its own
// coverage whatever the sub-pixel phase is, and one dab cannot accumulate over another.
[[nodiscard]] int tapCell(const cb::BrushParams& params, double x, std::uint64_t timeUs) {
    Image img(64, 32);
    cb::BrushEngine eng;
    eng.begin(64, 32, img, params, cb::BrushDynamics{}, at(x, 16.0, timeUs));
    eng.end();
    eng.composite();
    int peak = 0;
    for (std::size_t i = 3; i < img.rgba.size(); i += 4)
        peak = std::max(peak, static_cast<int>(img.rgba[i]));
    return peak;
}

// The cells a run of single-dab taps stamped. Each tap is at its own place and its own time, which
// is what two real taps differ in.
[[nodiscard]] std::set<int> tapCells(const cb::BrushParams& params, int taps) {
    std::set<int> seen;
    for (int i = 0; i < taps; ++i)
        seen.insert(tapCell(params, 6.0 + 2.0 * i, 40'000 + 17'000ull * static_cast<unsigned>(i)));
    return seen;
}

[[nodiscard]] std::vector<std::uint8_t> lay(const cb::BrushParams& params) {
    Image img(64, 32);
    cb::BrushEngine eng;
    eng.begin(64, 32, img, params, cb::BrushDynamics{}, at(6.0, 16.0, 1000));
    eng.extendTo(at(24.0, 16.0, 21'000));
    eng.extendTo(at(48.0, 16.0, 44'000));
    eng.end();
    eng.composite();
    return img.rgba;
}

} // namespace

TEST_CASE("strokeSeedFor: every channel of the first sample reaches the seed") {
    const cb::StrokeInput base = at(12.0, 30.0, 7'000);
    const std::uint64_t seed = cb::strokeSeedFor(0, base);

    // Pure: the same base and the same sample always derive the same seed. This is the whole replay
    // contract, and without it a recorded stroke could not reproduce.
    CHECK(cb::strokeSeedFor(0, base) == seed);

    // ... and each channel moves it, INCLUDING a change far below a pixel: two presses a hundredth
    // of a pixel apart are two strokes, and the bit pattern is what goes in rather than a rounded
    // value.
    const auto moved = [&](auto mutate) {
        cb::StrokeInput in = base;
        mutate(in);
        return cb::strokeSeedFor(0, in);
    };
    CHECK(moved([](cb::StrokeInput& i) { i.pos.x += 0.01; }) != seed);
    CHECK(moved([](cb::StrokeInput& i) { i.pos.y += 0.01; }) != seed);
    CHECK(moved([](cb::StrokeInput& i) { i.pressure = 0.5; }) != seed);
    CHECK(moved([](cb::StrokeInput& i) { i.xTilt = 3.0; }) != seed);
    CHECK(moved([](cb::StrokeInput& i) { i.yTilt = 3.0; }) != seed);
    // ⚠ THE ONE THAT SEPARATES TWO TAPS AT ONE POINT, and it must move for a SINGLE microsecond:
    // a user tapping the same pixel twice differs in nothing else at all.
    CHECK(moved([](cb::StrokeInput& i) { i.timeUs += 1; }) != seed);

    // The caller's own seed still separates two otherwise identical strokes -- it is folded in, not
    // replaced.
    CHECK(cb::strokeSeedFor(1, base) != seed);
    CHECK(cb::strokeSeedFor(0xFFFF'FFFF'FFFF'FFFFULL, base) != seed);
}

TEST_CASE("hose: single-dab taps walk the cells -- the user-reported bug") {
    // ⚠⚠ THE REGRESSION CASE. `t)_Shapes_Mecha`'s tip picks its cell at random, and the store's
    // params are resolved ONCE for the preset -- so before §6.6i every stroke re-seeded the stream
    // to the same number and drew the same first value. A moving stroke hid it (the stream advances
    // within a stroke); a TAP is exactly one first draw, so tapping stamped one shape forever.
    cb::BrushParams p = randomHoseBrush();

    // The old behaviour, still reachable and still pinned: with the derivation off, the seed alone
    // fixes the stream, so twenty-four taps stamp ONE cell. That is the contract goldens and the
    // preview cards rest on -- and it is exactly the wrong contract for a canvas.
    p.seedFromFirstSample = false;
    const std::set<int> frozen = tapCells(p, 24);
    CHECK(frozen.size() == 1);

    // With it on, the taps differ -- and they differ because the SAMPLES differ, which is the whole
    // of the mechanism.
    p.seedFromFirstSample = true;
    const std::set<int> varied = tapCells(p, 24);
    CHECK(varied.size() >= 2);

    // ... and the marks really are the hose's cells, not noise: every alpha seen is one of the four
    // cells' own coverages. (Without this the case above would pass on an engine that jittered the
    // opacity instead of the cell.)
    const std::set<int> cellAlphas{255 - kCellGrey[0], 255 - kCellGrey[1], 255 - kCellGrey[2],
                                   255 - kCellGrey[3]};
    for (const int a : varied) {
        CAPTURE(a);
        CHECK(cellAlphas.count(a) == 1u);
    }
}

TEST_CASE("hose: two taps at the SAME point differ only by their clock, and that is enough") {
    // The case the position cannot carry: a user tapping one pixel over and over. The timestamp is
    // the engine's own monotonic ingest clock, so it always moves between two presses.
    cb::BrushParams p = randomHoseBrush();
    p.seedFromFirstSample = true;
    std::set<int> seen;
    for (int i = 0; i < 24; ++i)
        seen.insert(tapCell(p, 20.0, 1'000'000 + 13'337ull * static_cast<unsigned>(i)));
    CHECK(seen.size() >= 2);
}

TEST_CASE("seed derivation: the mark stays a pure function of the sample stream") {
    // ⚠ THE REPLAY CONTRACT. The derivation is a hash of the stroke's own first sample and NOT a
    // clock read, so the same stream lays the same bytes however many times it is replayed -- which
    // is what undo/redo, the goldens and the editor's preview all rest on. A per-stroke seed drawn
    // from a global RNG would pass no such test, and would have to be recorded to replay at all.
    cb::BrushParams p = randomHoseBrush();
    p.seedFromFirstSample = true;
    const std::vector<std::uint8_t> first = lay(p);
    CHECK(lay(p) == first);
    CHECK(lay(p) == first);

    // ... and the converse, without which the first half is vacuous: a different first sample is a
    // different stroke, and its randomness moves with it.
    cb::BrushParams same = p;
    Image img(64, 32);
    cb::BrushEngine eng;
    eng.begin(64, 32, img, same, cb::BrushDynamics{}, at(6.0, 16.0, 999)); // 1 us earlier
    eng.extendTo(at(24.0, 16.0, 21'000));
    eng.extendTo(at(48.0, 16.0, 44'000));
    eng.end();
    eng.composite();
    CHECK_FALSE(img.rgba == first);
}

TEST_CASE("seed derivation: OFF leaves the stroke BYTE-IDENTICAL") {
    // The hard rule (§6.2), and the reason the flag defaults to false: every golden in the suite was
    // laid with the bare seed, and a stroke that does not ask for the derivation must not move one
    // byte. `seed` still separates two strokes on its own, exactly as it always did.
    cb::BrushParams p = randomHoseBrush();
    p.seed = 12345;
    const std::vector<std::uint8_t> reference = lay(p);
    CHECK(lay(p) == reference);

    cb::BrushParams other = p;
    other.seed = 12346;
    CHECK_FALSE(lay(other) == reference);

    // ... and turning the derivation on genuinely changes which cells land, so "byte-identical" is
    // a statement about the flag and not about a hose that never varied.
    cb::BrushParams derived = p;
    derived.seedFromFirstSample = true;
    CHECK_FALSE(lay(derived) == reference);
}

TEST_CASE("preview: a card does NOT shimmer, even when the params ask for fresh randomness") {
    // ⚠ THE OPPOSITE REQUIREMENT, and it is not a preference: the store hands out params with the
    // derivation ON (that is what makes a canvas stroke vary), and the dock renders its cards from
    // those same params on every expose. renderStrokePreview clears BOTH the seed and the flag --
    // pinning `seed` alone would not be enough, because the derivation folds the preview path's own
    // first sample in.
    cb::BrushParams p = randomHoseBrush();
    p.seedFromFirstSample = true;
    p.seed = 99;
    cb::StrokePreviewStyle style;
    const Image a = cb::renderStrokePreview(p, 96, 40, style);
    const Image b = cb::renderStrokePreview(p, 96, 40, style);
    REQUIRE(!a.rgba.empty());
    CHECK(a.rgba == b.rgba);

    // ... and it is the SAME picture a preset that never asked would draw, so no card moved.
    cb::BrushParams plain = p;
    plain.seedFromFirstSample = false;
    plain.seed = 0;
    CHECK(cb::renderStrokePreview(plain, 96, 40, style).rgba == a.rgba);
}
