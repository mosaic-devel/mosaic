// Gradient dithering (S22; docs/gradient-tool.md §7). The kinds are pure point functions of the
// destination pixel, so everything here is exact arithmetic against the published constructions --
// Bayer's 1973 matrix cell by cell, Ulichney's 1993 void-and-cluster tile by its defining
// properties -- plus the two guarantees the rest of the app leans on: `None` changes nothing at
// all, and a dithered sample never leaves [0,1].

#include "core/vector/paint.hpp"

#include "core/vector/geometry.hpp"
#include "core/vector/object.hpp"
#include "io/mosaic/docjson.hpp" // the wire format: the kind must persist, like SpreadMethod

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <set>
#include <variant>
#include <vector>

namespace vec = mosaic::core::vec;
using doctest::Approx;

namespace {

// The published Bayer 8x8 ordered matrix (Bayer, IEEE ICC 1973), transcribed independently of the
// implementation's own copy so the test really checks the table rather than echoing it.
constexpr std::array<int, 64> kBayerRef{{
    0,  32, 8,  40, 2,  34, 10, 42,  //
    48, 16, 56, 24, 50, 18, 58, 26,  //
    12, 44, 4,  36, 14, 46, 6,  38,  //
    60, 28, 52, 20, 62, 30, 54, 22,  //
    3,  35, 11, 43, 1,  33, 9,  41,  //
    51, 19, 59, 27, 49, 17, 57, 25,  //
    15, 47, 7,  39, 13, 45, 5,  37,  //
    63, 31, 55, 23, 61, 29, 53, 21,  //
}};

vec::Gradient blackToWhite(vec::DitherKind kind) {
    vec::Gradient g;
    g.type = vec::GradientType::Linear;
    g.stops = {{0.0, vec::ColorF{0, 0, 0, 1}}, {1.0, vec::ColorF{1, 1, 1, 1}}};
    g.dither = kind;
    return g;  // identity transform: the gradient parameter is just the local x coordinate
}

// The blue-noise rank a threshold implies. ditherOffsetLsb returns (rank + 0.5) / 4096 - 0.5, so
// the rank comes back exactly (the division is by a power of two and 4096 * 0.5 is integral).
int blueRank(int x, int y) {
    const double off = vec::ditherOffsetLsb(vec::DitherKind::BlueNoise, x, y, 0);
    return static_cast<int>(std::lround((off + 0.5) * 4096.0 - 0.5));
}

}  // namespace

// ---- None: the one kind that must change absolutely nothing -------------------------------------

TEST_CASE("DitherKind::None is exactly zero everywhere, and leaves the ramp bit-identical") {
    for (int y = -3; y < 12; ++y)
        for (int x = -3; x < 12; ++x)
            for (int ch = 0; ch < 4; ++ch)
                CHECK(vec::ditherOffsetLsb(vec::DitherKind::None, x, y, ch) == 0.0);

    // The real guarantee: sampling a None gradient WITH a pixel key gives bit-identical floats to
    // sampling it without one -- so no existing golden image can move.
    const vec::Paint paint{blackToWhite(vec::DitherKind::None)};
    for (int i = 0; i <= 40; ++i) {
        const double t = static_cast<double>(i) / 40.0;
        const vec::ColorF plain = vec::sampleAt(paint, {t, 0.0});
        const vec::ColorF keyed = vec::sampleAt(paint, {t, 0.0}, true, vec::SamplePixel{i, 7, true});
        CHECK(plain.r == keyed.r);  // exact float equality, not Approx
        CHECK(plain.g == keyed.g);
        CHECK(plain.b == keyed.b);
        CHECK(plain.a == keyed.a);
    }
    // Gradient::dither defaults to None, so a paint nobody touched is the pre-S22 paint.
    CHECK(vec::Gradient{}.dither == vec::DitherKind::None);
}

TEST_CASE("a dithered gradient still evaluates the exact ramp without a pixel key") {
    // A point query (a hit test, the flyout's colour probe, an SVG export) has no pixel grid, so it
    // must get the undithered value however the paint is configured.
    for (auto kind : {vec::DitherKind::Ordered, vec::DitherKind::BlueNoise, vec::DitherKind::Noise}) {
        const vec::Paint paint{blackToWhite(kind)};
        CHECK(vec::sampleAt(paint, {0.5, 0.0}).r == 0.5f);
        CHECK(vec::sampleAt(paint, {0.0, 0.0}).r == 0.0f);
        CHECK(vec::sampleAt(paint, {1.0, 0.0}).r == 1.0f);
    }
}

// ---- Ordered: Bayer 1973, cell for cell ---------------------------------------------------------

TEST_CASE("Ordered is the published Bayer 8x8 matrix, centred on zero") {
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const double want = (kBayerRef[static_cast<std::size_t>(y * 8 + x)] + 0.5) / 64.0 - 0.5;
            CHECK(vec::ditherOffsetLsb(vec::DitherKind::Ordered, x, y, 0) == Approx(want));
        }
    // Hand-computed corners of that formula: cell 0 -> 0.5/64 - 0.5, cell 63 -> 63.5/64 - 0.5.
    CHECK(vec::ditherOffsetLsb(vec::DitherKind::Ordered, 0, 0, 0) == Approx(-0.4921875));
    CHECK(vec::ditherOffsetLsb(vec::DitherKind::Ordered, 0, 7, 0) == Approx(0.4921875));

    // One threshold per PIXEL, shared by all four channels -- a luminance-only dither, so a grey
    // ramp gains no chroma speckle (the prepress convention).
    for (int ch = 1; ch < 4; ++ch)
        CHECK(vec::ditherOffsetLsb(vec::DitherKind::Ordered, 5, 3, ch) ==
              vec::ditherOffsetLsb(vec::DitherKind::Ordered, 5, 3, 0));

    // It tiles with period 8, into the negative half-plane too (a layer can sit at a negative
    // offset, and a discontinuity at x == 0 would draw a seam).
    for (int y = -9; y < 9; ++y)
        for (int x = -9; x < 9; ++x)
            CHECK(vec::ditherOffsetLsb(vec::DitherKind::Ordered, x, y, 0) ==
                  vec::ditherOffsetLsb(vec::DitherKind::Ordered, x + 8, y + 8, 0));

    // The matrix is a permutation of 0..63, so the mean offset over one tile is exactly zero: the
    // dither moves band EDGES, never the ramp's average level.
    double sum = 0.0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) sum += vec::ditherOffsetLsb(vec::DitherKind::Ordered, x, y, 0);
    CHECK(sum == Approx(0.0));
}

// ---- Blue noise: Ulichney 1993, by its defining properties --------------------------------------

TEST_CASE("BlueNoise is a deterministic permutation of the whole 64x64 tile") {
    std::set<int> seen;
    double sum = 0.0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const int rank = blueRank(x, y);
            CHECK(rank >= 0);
            CHECK(rank < 4096);
            seen.insert(rank);
            sum += vec::ditherOffsetLsb(vec::DitherKind::BlueNoise, x, y, 0);
        }
    CHECK(seen.size() == 4096);  // every rank exactly once -> a true threshold array
    CHECK(sum == Approx(0.0));   // ... so, like Bayer, it is mean-preserving

    // Deterministic (the tile is cached, but it is also reproducible run to run) and tiling.
    CHECK(blueRank(13, 41) == blueRank(13, 41));
    for (int y = -5; y < 5; ++y)
        for (int x = -5; x < 5; ++x) CHECK(blueRank(x, y) == blueRank(x + 64, y + 64));
    // Same threshold on every channel, exactly like Ordered.
    for (int ch = 1; ch < 4; ++ch)
        CHECK(vec::ditherOffsetLsb(vec::DitherKind::BlueNoise, 9, 21, ch) ==
              vec::ditherOffsetLsb(vec::DitherKind::BlueNoise, 9, 21, 0));
}

TEST_CASE("BlueNoise really is blue: its low-rank set is spaced, not clumped") {
    // THE property that makes it worth having next to plain Noise. Take the pattern the tile
    // produces at 1/16 density (rank < 256) -- the binary image a dither actually paints near an
    // extreme of the ramp. For an UNCORRELATED (white) pattern of that density, the chance a given
    // "on" cell has another "on" cell among its 4 neighbours is 1 - (15/16)^4 = 22.5%. Void-and-
    // cluster's whole purpose is to drive that to nearly nothing by keeping the points apart.
    int on = 0, clumped = 0;
    const auto isOn = [](int x, int y) { return blueRank((x + 64) % 64, (y + 64) % 64) < 256; };
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            if (!isOn(x, y)) continue;
            ++on;
            if (isOn(x + 1, y) || isOn(x - 1, y) || isOn(x, y + 1) || isOn(x, y - 1)) ++clumped;
        }
    CHECK(on == 256);
    CHECK(static_cast<double>(clumped) / static_cast<double>(on) < 0.10);  // white noise: ~0.225
}

// ---- Noise: the shipping TPDF, per channel ------------------------------------------------------

TEST_CASE("Noise is per-channel TPDF in [-1, 1], deterministic and unstructured") {
    double lo = 2.0, hi = -2.0, sum = 0.0;
    int n = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            for (int ch = 0; ch < 4; ++ch) {
                const double v = vec::ditherOffsetLsb(vec::DitherKind::Noise, x, y, ch);
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                sum += v;
                ++n;
            }
    CHECK(lo >= -1.0);
    CHECK(hi <= 1.0);
    CHECK(lo < -0.5);  // it really does reach out toward both ends
    CHECK(hi > 0.5);
    CHECK(std::abs(sum / n) < 0.02);  // zero-mean: no DC shift of the ramp

    // Deterministic, and INDEPENDENT per channel (unlike the two threshold kinds).
    CHECK(vec::ditherOffsetLsb(vec::DitherKind::Noise, 7, 9, 2) ==
          vec::ditherOffsetLsb(vec::DitherKind::Noise, 7, 9, 2));
    CHECK(vec::ditherOffsetLsb(vec::DitherKind::Noise, 7, 9, 0) !=
          vec::ditherOffsetLsb(vec::DitherKind::Noise, 7, 9, 1));
    // ... and it does NOT tile: that is the one thing it has over the other two.
    CHECK(vec::ditherOffsetLsb(vec::DitherKind::Noise, 3, 3, 0) !=
          vec::ditherOffsetLsb(vec::DitherKind::Noise, 67, 67, 0));
}

// ---- The kinds are genuinely different, and land on the sampled colour ------------------------

TEST_CASE("the three dithering kinds each produce a different image of the same ramp") {
    // A ramp so shallow it would band hard at 8 bits: 0.5 +/- a fraction of one LSB across the row.
    const auto rowOf = [](vec::DitherKind kind) {
        const vec::Paint paint{blackToWhite(kind)};
        std::vector<double> row;
        for (int x = 0; x < 64; ++x)
            row.push_back(vec::sampleAt(paint, {0.5, 0.0}, true, vec::SamplePixel{x, 3, true}).r);
        return row;
    };
    const std::vector<double> none = rowOf(vec::DitherKind::None);
    const std::vector<double> ordered = rowOf(vec::DitherKind::Ordered);
    const std::vector<double> blue = rowOf(vec::DitherKind::BlueNoise);
    const std::vector<double> noise = rowOf(vec::DitherKind::Noise);

    for (double v : none) CHECK(v == none.front());  // undithered: a flat 0.5 across the row
    CHECK(ordered != none);
    CHECK(blue != none);
    CHECK(noise != none);
    CHECK(ordered != blue);   // ... and each kind is a different image, not a relabelling
    CHECK(ordered != noise);
    CHECK(blue != noise);

    // Every kind stays within one LSB of the true ramp value -- a dither must be invisible as a
    // colour shift and only visible as texture.
    for (const std::vector<double>* row : {&ordered, &blue, &noise})
        for (double v : *row) CHECK(std::abs(v - 0.5) <= 1.0 / 255.0 + 1e-6);
}

TEST_CASE("dithering clamps to [0,1] and cannot fizz a saturated end of the ramp") {
    // At the ends of a black->white ramp the colour is already 0 or 1; a dither offset must not
    // push it out of range (which would show as stray pixels at the extremes).
    for (auto kind : {vec::DitherKind::Ordered, vec::DitherKind::BlueNoise, vec::DitherKind::Noise}) {
        const vec::Paint paint{blackToWhite(kind)};
        for (int x = 0; x < 32; ++x) {
            const vec::ColorF black =
                vec::sampleAt(paint, {0.0, 0.0}, true, vec::SamplePixel{x, 1, true});
            const vec::ColorF white =
                vec::sampleAt(paint, {1.0, 0.0}, true, vec::SamplePixel{x, 1, true});
            CHECK(black.r >= 0.0f);
            CHECK(black.r <= 1.0f);
            CHECK(white.r >= 0.0f);
            CHECK(white.r <= 1.0f);
            CHECK(black.a == 1.0f);  // the ramp is opaque at both ends; alpha has no room to move
            CHECK(white.a == 1.0f);
        }
    }
}

TEST_CASE("only gradients dither -- a solid paint is untouched by the pixel key") {
    const vec::Paint solid{vec::SolidPaint{vec::ColorF{0.4f, 0.4f, 0.4f, 1.0f}}};
    for (int x = 0; x < 16; ++x) {
        const vec::ColorF c = vec::sampleAt(solid, {0.0, 0.0}, true, vec::SamplePixel{x, 0, true});
        CHECK(c.r == 0.4f);
    }
}

// ---- Serialisation: the kind rides the PAINT, so it persists and reloads identically -----------
// It follows SpreadMethod's precedent exactly, with one addition: the key is OPTIONAL on the wire,
// written only when the kind is not None, so every gradient authored before S22 serialises to
// byte-identical JSON and reloads to the same pixels.

TEST_CASE("a gradient's dither kind round-trips through the document JSON") {
    const auto make = [](vec::DitherKind kind) {
        mosaic::core::vec::Object o;
        o.geometry = vec::ParametricShape{vec::RectShape{{40, 30}, 0.0}};
        o.fill = blackToWhite(kind);
        return o;
    };
    for (auto kind : {vec::DitherKind::None, vec::DitherKind::Ordered, vec::DitherKind::BlueNoise,
                      vec::DitherKind::Noise}) {
        const mosaic::core::vec::Object src = make(kind);
        const nlohmann::json j = mosaic::io::native::detail::vectorObjectToJson(src);
        const auto back = mosaic::io::native::detail::vectorObjectFromJson(j);
        REQUIRE(back.has_value());
        CHECK(std::get<vec::Gradient>(back->fill).dither == kind);
        CHECK(*back == src);  // the whole object, dither included (Gradient's operator== covers it)
    }

    // None writes NO key at all -- the pre-S22 wire format, unchanged.
    const nlohmann::json plain = mosaic::io::native::detail::vectorObjectToJson(make(vec::DitherKind::None));
    CHECK_FALSE(plain["fill"].contains("dither"));
    const nlohmann::json dithered =
        mosaic::io::native::detail::vectorObjectToJson(make(vec::DitherKind::BlueNoise));
    CHECK(dithered["fill"]["dither"] == "blue");

    // A file written before S22 (no key) loads as None...
    nlohmann::json legacy = dithered;
    legacy["fill"].erase("dither");
    const auto loaded = mosaic::io::native::detail::vectorObjectFromJson(legacy);
    REQUIRE(loaded.has_value());
    CHECK(std::get<vec::Gradient>(loaded->fill).dither == vec::DitherKind::None);
    // ... but a key we do not know is a hard error, like every other enum field in this format.
    nlohmann::json bogus = dithered;
    bogus["fill"]["dither"] = "riemersma";
    CHECK_FALSE(mosaic::io::native::detail::vectorObjectFromJson(bogus).has_value());
}
