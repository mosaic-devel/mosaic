#include <doctest/doctest.h>

#include "core/brush/bitmap_tip.hpp"
#include "core/brush/dab_mask.hpp"

#include <cmath>
#include <numeric>
#include <set>
#include <vector>

using mosaic::core::brush::BitmapTip;
using mosaic::core::brush::bitmapDabShape;
using mosaic::core::brush::DabMask;
using mosaic::core::brush::DabPlacement;
using mosaic::core::brush::DabShape;
using mosaic::core::brush::FrameSelection;
using mosaic::core::brush::frameSelectionFromName;
using mosaic::core::brush::frameSelectionName;
using mosaic::core::brush::HoseParams;
using mosaic::core::brush::HoseState;
using mosaic::core::brush::placeDab;
using mosaic::core::brush::renderDabMask;
using mosaic::core::brush::StrokeInput;
using mosaic::core::brush::StrokeState;
using mosaic::core::brush::TipAdjustments;
using mosaic::core::brush::TipApplication;
using mosaic::core::brush::TipFrame;
using mosaic::core::brush::TipSourceKind;

namespace {

constexpr double kPi = 3.14159265358979323846;

// A frame in the TIP IMAGE convention: `grey` is the stored greyscale (white = no paint).
[[nodiscard]] TipFrame frameFromGrey(std::uint32_t w, std::uint32_t h,
                                     const std::vector<std::uint8_t>& grey,
                                     std::uint8_t alpha = 255) {
    TipFrame f;
    f.width = w;
    f.height = h;
    f.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < grey.size(); ++i) {
        f.rgba[i * 4] = grey[i];
        f.rgba[i * 4 + 1] = grey[i];
        f.rgba[i * 4 + 2] = grey[i];
        f.rgba[i * 4 + 3] = alpha;
    }
    return f;
}

// What a `.gbr` loader hands us: it has already inverted the file's raw bytes into the tip image.
[[nodiscard]] TipFrame gbrFrame(std::uint32_t w, std::uint32_t h,
                                const std::vector<std::uint8_t>& rawBytes) {
    std::vector<std::uint8_t> grey(rawBytes.size());
    for (std::size_t i = 0; i < rawBytes.size(); ++i)
        grey[i] = static_cast<std::uint8_t>(255 - rawBytes[i]);
    return frameFromGrey(w, h, grey);
}

[[nodiscard]] TipFrame solidFrame(std::uint32_t w, std::uint32_t h, std::uint8_t grey = 0) {
    return frameFromGrey(w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h, grey));
}

[[nodiscard]] double totalCoverage(const DabMask& m) {
    return std::accumulate(m.coverage.begin(), m.coverage.end(), 0.0);
}

} // namespace

TEST_CASE("a GBR's raw byte survives the round trip as its coverage") {
    // docs/brushes.md §3.6.1: the loader inverts, the mask path inverts again, and the two cancel.
    // If this test says 255-raw, some tip somewhere is rendering as its own photo-negative.
    const std::vector<std::uint8_t> raw{0, 1, 64, 128, 200, 254, 255, 17, 99};
    const BitmapTip tip{{gbrFrame(3, 3, raw)}, TipApplication::AlphaMask, TipSourceKind::Mask};
    REQUIRE(tip.frameCount() == 1);
    for (std::size_t i = 0; i < raw.size(); ++i)
        CHECK(tip.level(0, 0).coverage[i] == raw[i]);
}

TEST_CASE("a PNG's grey is inverted exactly once") {
    // The opposite convention: a PNG stores white for background, so coverage is 255 - grey.
    const std::vector<std::uint8_t> grey{0, 1, 64, 128, 200, 254, 255, 17, 99};
    const BitmapTip tip{{frameFromGrey(3, 3, grey)}, TipApplication::AlphaMask, TipSourceKind::Mask};
    for (std::size_t i = 0; i < grey.size(); ++i)
        CHECK(tip.level(0, 0).coverage[i] == 255 - grey[i]);
}

TEST_CASE("alpha multiplies coverage, and the other three applications ARE the alpha") {
    std::vector<std::uint8_t> grey{0, 0, 255, 255};
    TipFrame f = frameFromGrey(2, 2, grey);
    f.rgba[3] = 255; // black, opaque   -> full paint
    f.rgba[7] = 128; // black, half     -> half paint
    f.rgba[11] = 255; // white, opaque  -> nothing
    f.rgba[15] = 128; // white, half    -> nothing

    const BitmapTip mask{{f}, TipApplication::AlphaMask, TipSourceKind::Mask};
    CHECK(mask.level(0, 0).coverage[0] == 255);
    CHECK(mask.level(0, 0).coverage[1] == 128);
    CHECK(mask.level(0, 0).coverage[2] == 0);
    CHECK(mask.level(0, 0).coverage[3] == 0);

    // LightnessMap/GradientMap/ImageStamp take the tip's alpha and leave the grey to the colour path.
    for (auto app : {TipApplication::LightnessMap, TipApplication::GradientMap,
                     TipApplication::ImageStamp}) {
        const BitmapTip t{{f}, app, TipSourceKind::Image};
        CHECK(t.level(0, 0).coverage[0] == 255);
        CHECK(t.level(0, 0).coverage[1] == 128);
        CHECK(t.level(0, 0).coverage[2] == 255); // white but opaque: full coverage, white lightness
        CHECK(t.level(0, 0).coverage[3] == 128);
    }
}

TEST_CASE("adjustments run on an Image source in every application but ImageStamp") {
    TipAdjustments adj;
    adj.brightness = 0.5; // shifts the hinge upward -> a lighter tip -> LESS coverage
    const std::vector<std::uint8_t> grey{0, 64, 128, 255};

    const BitmapTip plain{{frameFromGrey(2, 2, grey)}, TipApplication::AlphaMask,
                          TipSourceKind::Mask, adj};
    const BitmapTip adjusted{{frameFromGrey(2, 2, grey)}, TipApplication::AlphaMask,
                             TipSourceKind::Image, adj};
    const BitmapTip stamped{{frameFromGrey(2, 2, grey)}, TipApplication::ImageStamp,
                            TipSourceKind::Image, adj};

    // A Mask source ignores the adjustments entirely.
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(plain.level(0, 0).coverage[i] == 255 - grey[i]);
    // An Image source in AlphaMask honours them: brighter grey means less paint.
    bool anyDifferent = false;
    for (std::size_t i = 0; i < 4; ++i)
        if (adjusted.level(0, 0).coverage[i] != plain.level(0, 0).coverage[i])
            anyDifferent = true;
    CHECK(anyDifferent);
    // The curve is hinged, not offset: it maps 0 to 0 and 255 to 255 whatever the brightness, so
    // pure black stays fully painted and only the midtones move. Grey 64 lightens, losing coverage.
    CHECK(adjusted.level(0, 0).coverage[0] == 255);
    CHECK(adjusted.level(0, 0).coverage[3] == plain.level(0, 0).coverage[3]);
    CHECK(adjusted.level(0, 0).coverage[1] < plain.level(0, 0).coverage[1]);
    // ImageStamp coverage is alpha, which no adjustment touches.
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(stamped.level(0, 0).coverage[i] == 255);
}

TEST_CASE("a neutral adjustment is the identity") {
    const std::vector<std::uint8_t> grey{0, 37, 128, 255};
    const BitmapTip a{{frameFromGrey(2, 2, grey)}, TipApplication::AlphaMask, TipSourceKind::Image};
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(a.level(0, 0).coverage[i] == 255 - grey[i]);
}

TEST_CASE("autoMidPoint hinges the curve on the frame's own average grey") {
    // Two frames of one hose with different averages must get different curves -- the hinge is a
    // property of the image, not of the tip. A shared table would be wrong for at least one of them.
    TipAdjustments adj;
    adj.autoMidPoint = true;
    CHECK_FALSE(adj.neutral()); // the flag alone is enough to make the curve non-identity

    const std::vector<std::uint8_t> dark(64, 32);  // average 32
    const std::vector<std::uint8_t> light(64, 200); // average 200
    const BitmapTip tip{{frameFromGrey(8, 8, dark), frameFromGrey(8, 8, light)},
                        TipApplication::AlphaMask, TipSourceKind::Image, adj};
    REQUIRE(tip.frameCount() == 2);

    // A flat frame whose every pixel IS the average lands exactly on the hinge, which brightness 0
    // puts at 127. So both frames come out mid-grey -- coverage 255 - 127 = 128 -- despite starting
    // 168 levels apart. That is what "hinge on the average" means, and it is per frame.
    CHECK(tip.level(0, 0).coverage[0] == 128);
    CHECK(tip.level(1, 0).coverage[0] == 128);

    // Without the flag, the same two frames keep their very different coverage.
    const BitmapTip plain{{frameFromGrey(8, 8, dark), frameFromGrey(8, 8, light)},
                          TipApplication::AlphaMask, TipSourceKind::Image};
    CHECK(plain.level(0, 0).coverage[0] == 255 - 32);
    CHECK(plain.level(1, 0).coverage[0] == 255 - 200);
}

TEST_CASE("velocity, like tilt, indexes one past the last cell at full scale") {
    // The format scales by `rank - 0.5`, not `rank - 1`, so full speed rounds to `rank` and the
    // final modulo wraps it to cell 0. Asymmetric with `pressure`, which stays in range. Pinned so
    // that nobody "corrects" it into a clamp: that would change which cell paints at full speed.
    HoseParams p;
    p.dim = 1;
    p.declaredCells = 4;
    p.rank[0] = 4;
    p.selection[0] = FrameSelection::Velocity;

    HoseState hose;
    StrokeState st;
    StrokeInput in;
    st.begin(in, 5);

    // Drive the speed EMA to saturation: long hops, small time steps.
    for (int d = 0; d < 60; ++d) {
        in.pos = {in.pos.x + 500.0, 0.0};
        in.timeUs += 1000;
        st.extendTo(in);
        st.beginDab();
        (void)hose.selectFrame(p, 4, st, in); // advances incremental state; pick checked below
    }
    REQUIRE(st.speed() == doctest::Approx(1.0).epsilon(0.01));
    CHECK(hose.selectFrame(p, 4, st, in) == 0); // rank == 4 folds to 0, not to 3

    // Pressure is the contrasting case: full pressure selects the LAST cell, in range.
    p.selection[0] = FrameSelection::Pressure;
    in.pressure = 1.0;
    CHECK(hose.selectFrame(p, 4, st, in) == 3);
}

TEST_CASE("the adjustment table never divides by zero and never emits a non-value") {
    using mosaic::core::brush::detail::adjustmentTable;
    // The reference divides by `midPoint` and by `255 - midPoint`, and a preset may ask for either
    // end. Contrast of exactly 1 is its own guarded singularity.
    for (double mid : {0.0, 1.0, 127.0, 254.0, 255.0, -50.0, 400.0, std::nan("")}) {
        for (double b : {-1.0, -0.5, 0.0, 0.5, 1.0}) {
            for (double c : {-1.0, -0.999, 0.0, 0.5, 0.999, 1.0, 2.0}) {
                TipAdjustments adj;
                adj.midPoint = mid;
                adj.brightness = b;
                adj.contrast = c;
                const auto lut = adjustmentTable(adj);
                CAPTURE(mid);
                CAPTURE(b);
                CAPTURE(c);
                for (int v = 0; v < 256; ++v)
                    CHECK(lut[static_cast<std::size_t>(v)] <= 255); // i.e. it is a value at all
            }
        }
    }
}

TEST_CASE("frame selection names round-trip, and an unknown name is Constant") {
    for (auto s : {FrameSelection::Constant, FrameSelection::Incremental, FrameSelection::Angular,
                   FrameSelection::Velocity, FrameSelection::Random, FrameSelection::Pressure,
                   FrameSelection::TiltX, FrameSelection::TiltY})
        CHECK(frameSelectionFromName(frameSelectionName(s)) == s);

    CHECK(frameSelectionFromName("constant") == FrameSelection::Constant);
    CHECK(frameSelectionFromName("") == FrameSelection::Constant);
    CHECK(frameSelectionFromName("nonsense") == FrameSelection::Constant);
    CHECK(frameSelectionFromName("Random") == FrameSelection::Constant); // case sensitive
    CHECK(frameSelectionFromName("xtilt") == FrameSelection::TiltX);
    CHECK(frameSelectionFromName("ytilt") == FrameSelection::TiltY);
}

TEST_CASE("incremental walks the cells from zero, once per dab") {
    HoseParams p;
    p.dim = 1;
    p.declaredCells = 4;
    p.rank[0] = 4;
    p.selection[0] = FrameSelection::Incremental;

    HoseState hose;
    StrokeState st;
    StrokeInput in;
    st.begin(in, 7);
    for (int dab = 0; dab < 9; ++dab) {
        st.beginDab();
        CHECK(hose.selectFrame(p, 4, st, in) == dab % 4); // the FIRST dab is cell 0, not cell 1
    }
}

TEST_CASE("random draws once per dab, deterministically from the stroke seed") {
    HoseParams p;
    p.dim = 1;
    p.declaredCells = 8;
    p.rank[0] = 8;
    p.selection[0] = FrameSelection::Random;

    const auto run = [&p](std::uint64_t seed) {
        HoseState hose;
        StrokeState st;
        StrokeInput in;
        st.begin(in, seed);
        std::vector<int> frames;
        for (int d = 0; d < 40; ++d) {
            st.beginDab();
            frames.push_back(hose.selectFrame(p, 8, st, in));
        }
        return frames;
    };
    const std::vector<int> a = run(99);
    CHECK(run(99) == a); // replayable: same seed, same cells
    CHECK(run(100) != a);
    for (int f : a)
        CHECK((f >= 0 && f < 8));
    CHECK(std::set<int>(a.begin(), a.end()).size() > 3); // and it is actually varying
}

TEST_CASE("fairy-dust.gih: ncells is a claim, not a count") {
    // The shipped file declares 4 cells and contains 1. Every random draw in 0..3 must fold onto the
    // one frame that exists -- and must not index past it.
    HoseParams p;
    p.dim = 1;
    p.declaredCells = 4;
    p.rank[0] = 4;
    p.selection[0] = FrameSelection::Random;

    HoseState hose;
    StrokeState st;
    StrokeInput in;
    st.begin(in, 3);
    for (int d = 0; d < 50; ++d) {
        st.beginDab();
        CHECK(hose.selectFrame(p, /*frameCount=*/1, st, in) == 0);
    }
}

TEST_CASE("A_bamboo-leaves.gih: rank 5 over 3 cells strides by zero") {
    // ncells/rank0 = 3/5 = 0, so the hose has three cells and paints only the first, forever.
    // A reader that maps the dimension index straight onto the cell would cycle all three.
    HoseParams p;
    p.dim = 1;
    p.declaredCells = 3;
    p.rank[0] = 5;
    p.selection[0] = FrameSelection::Random;

    HoseState hose;
    StrokeState st;
    StrokeInput in;
    st.begin(in, 11);
    for (int d = 0; d < 50; ++d) {
        st.beginDab();
        CHECK(hose.selectFrame(p, /*frameCount=*/3, st, in) == 0);
    }
}

TEST_CASE("full tilt indexes one past the last cell, and the modulo folds it home") {
    HoseParams p;
    p.dim = 1;
    p.declaredCells = 4;
    p.rank[0] = 4;
    p.selection[0] = FrameSelection::TiltX;

    HoseState hose;
    StrokeState st;
    StrokeInput in;
    st.begin(in, 1);
    st.beginDab();

    in.xTilt = 60.0; // full scale: round(1/2 * 4) + 2 = 4, which is cell 4 of 4
    CHECK(hose.selectFrame(p, 4, st, in) == 0);
    in.xTilt = -60.0; // round(-2) + 2 = 0
    CHECK(hose.selectFrame(p, 4, st, in) == 0);
    in.xTilt = 0.0;
    CHECK(hose.selectFrame(p, 4, st, in) == 2);
    // Beyond full scale must not run off either end.
    for (double t : {-1000.0, 1000.0}) {
        in.xTilt = t;
        const int f = hose.selectFrame(p, 4, st, in);
        CHECK((f >= 0 && f < 4));
    }
}

TEST_CASE("an absent parasite is a static single-cell tip") {
    HoseParams p; // dim = 0, everything zero
    HoseState hose;
    StrokeState st;
    StrokeInput in;
    st.begin(in, 1);
    for (int d = 0; d < 5; ++d) {
        st.beginDab();
        CHECK(hose.selectFrame(p, 6, st, in) == 0);
    }
}

TEST_CASE("a zero rank cannot be cycled and degrades to constant") {
    for (auto mode : {FrameSelection::Incremental, FrameSelection::Angular, FrameSelection::Random,
                      FrameSelection::Pressure, FrameSelection::TiltX, FrameSelection::Velocity}) {
        HoseParams p;
        p.dim = 1;
        p.declaredCells = 3;
        p.rank[0] = 0;
        p.selection[0] = mode;
        HoseState hose;
        StrokeState st;
        StrokeInput in;
        st.begin(in, 1);
        st.beginDab();
        const int f = hose.selectFrame(p, 3, st, in);
        CHECK((f >= 0 && f < 3)); // above all: no division by zero, no negative index
    }
}

TEST_CASE("a two-dimensional hose is a mixed-radix index") {
    HoseParams p;
    p.dim = 2;
    p.declaredCells = 6; // rank 3 x rank 2, strides 2 and 1
    p.rank[0] = 3;
    p.rank[1] = 2;
    p.selection[0] = FrameSelection::Incremental;
    p.selection[1] = FrameSelection::Constant;

    HoseState hose;
    StrokeState st;
    StrokeInput in;
    st.begin(in, 1);
    // stride[0] = 6/3 = 2, stride[1] = 2/2 = 1. Dimension 1 stays at index 0.
    for (int d = 0; d < 6; ++d) {
        st.beginDab();
        CHECK(hose.selectFrame(p, 6, st, in) == (d % 3) * 2);
    }
}

TEST_CASE("degenerate frames are dropped and counted") {
    std::vector<TipFrame> frames;
    frames.push_back(solidFrame(4, 4));
    frames.push_back(TipFrame{}); // zero-sized
    TipFrame ragged;
    ragged.width = 4;
    ragged.height = 4;
    ragged.rgba.resize(7); // not w * h * 4
    frames.push_back(ragged);
    frames.push_back(solidFrame(2, 2));

    const BitmapTip tip{std::move(frames), TipApplication::AlphaMask, TipSourceKind::Mask};
    CHECK(tip.frameCount() == 2);
    CHECK(tip.droppedFrames() == 2); // a hose that lost a cell must not look intact
    CHECK(tip.frameWidth(0) == 4);
    CHECK(tip.frameWidth(1) == 2);
    CHECK(tip.frameWidth(9) == 0); // out of range is answered, not undefined
    CHECK(tip.baseSize(9) == 0.0);

    // level() must guard `frame` too, not just `lvl`. A caller that walked `declaredCells` -- which
    // is what a hose whose ncells lies invites -- would otherwise read past the frame vector.
    CHECK(tip.levelCount(9) == 0);
    CHECK(tip.level(9, 0).width == 0);
    CHECK(tip.level(-1, 0).coverage.empty());
    CHECK(BitmapTip{}.level(0, 0).coverage.empty());
    CHECK(tip.level(0, 999).width == 1); // lvl still clamps to the deepest mip
}

TEST_CASE("the mip chain halves down to a single pixel and conserves coverage") {
    const BitmapTip tip{{solidFrame(64, 32, 0)}, TipApplication::AlphaMask, TipSourceKind::Mask};
    REQUIRE(tip.frameCount() == 1);
    CHECK(tip.levelCount(0) == 7); // 64x32 -> 32x16 -> ... -> 1x1

    for (int l = 0; l < tip.levelCount(0); ++l) {
        const auto& m = tip.level(0, l);
        CHECK(m.coverage.size() == static_cast<std::size_t>(m.width) * m.height);
        for (auto c : m.coverage)
            CHECK(c == 255); // a solid tip stays solid at every level
    }
    CHECK(tip.level(0, 6).width == 1);
    CHECK(tip.level(0, 6).height == 1);
}

TEST_CASE("pickLevel never magnifies a level that threw detail away") {
    const BitmapTip tip{{solidFrame(64, 64)}, TipApplication::AlphaMask, TipSourceKind::Mask};
    CHECK(tip.pickLevel(0, 64.0, 64.0) == 0);
    CHECK(tip.pickLevel(0, 100.0, 100.0) == 0); // magnifying: stay at full resolution
    CHECK(tip.pickLevel(0, 32.0, 32.0) == 1);
    CHECK(tip.pickLevel(0, 33.0, 33.0) == 0);
    CHECK(tip.pickLevel(0, 8.0, 8.0) == 3);
    CHECK(tip.pickLevel(0, 0.5, 0.5) == tip.levelCount(0) - 1);
    // The chosen level is at least as large as the target on BOTH axes.
    for (double w : {1.0, 5.0, 17.0, 63.0, 64.0}) {
        const int l = tip.pickLevel(0, w, w);
        CAPTURE(w);
        CHECK(static_cast<double>(tip.level(0, l).width) >= w);
    }
}

TEST_CASE("renderDabMask agrees with placeDab on the mask's dimensions") {
    const BitmapTip tip{{solidFrame(40, 20)}, TipApplication::AlphaMask, TipSourceKind::Mask};
    for (double dia : {4.0, 20.0, 55.5}) {
        for (double ratio : {1.0, 0.5}) {
            for (double angle : {0.0, 0.4, kPi / 2, -1.3}) {
                for (int bin = 0; bin < 4; ++bin) {
                    const DabShape s = bitmapDabShape(tip, 0, dia, ratio, angle);
                    const DabPlacement p = placeDab(s, 10.0 + bin / 4.0, 20.0, 4);
                    const DabMask m = renderDabMask(tip, 0, s, p.subX, p.subY);
                    CAPTURE(dia);
                    CAPTURE(ratio);
                    CAPTURE(angle);
                    CAPTURE(bin);
                    CHECK(m.width == p.width);
                    CHECK(m.height == p.height);
                    CHECK(m.coverage.size() == static_cast<std::size_t>(m.width) * m.height);
                }
            }
        }
    }
}

TEST_CASE("bitmapDabShape scales the tip's long axis to the requested diameter") {
    const BitmapTip tip{{solidFrame(40, 10)}, TipApplication::AlphaMask, TipSourceKind::Mask};
    CHECK(tip.baseSize(0) == 40.0);

    const DabShape s = bitmapDabShape(tip, 0, 80.0, 1.0, 0.0);
    CHECK(s.width == doctest::Approx(80.0)); // the long axis becomes the diameter
    CHECK(s.height == doctest::Approx(20.0)); // and the frame's aspect is preserved

    const DabShape squashed = bitmapDabShape(tip, 0, 80.0, 0.5, 0.0);
    CHECK(squashed.width == doctest::Approx(80.0));
    CHECK(squashed.height == doctest::Approx(10.0)); // ratio squashes the short axis

    // Degenerate inputs give a shape that paints nothing rather than a NaN.
    CHECK(bitmapDabShape(tip, 9, 80.0, 1.0, 0.0).width == 0.0);
    CHECK(bitmapDabShape(tip, 0, std::nan(""), 1.0, 0.0).width == 0.0);
    const DabShape inf = bitmapDabShape(tip, 0, INFINITY, 1.0, 0.0);
    CHECK(mosaic::core::brush::dabExtent(inf).empty());
}

TEST_CASE("at 1:1 and zero phase the resample is exactly the identity") {
    // Worth pinning: the sample points land dead on the source pixel centres, so an unrotated,
    // unscaled, unshifted bitmap dab reproduces its tip byte for byte -- no rim, no softening.
    std::vector<std::uint8_t> grey(32 * 32);
    for (std::uint32_t i = 0; i < grey.size(); ++i)
        grey[i] = static_cast<std::uint8_t>((i * 37) % 256);
    const BitmapTip tip{{frameFromGrey(32, 32, grey)}, TipApplication::AlphaMask,
                        TipSourceKind::Mask};
    const DabShape s = bitmapDabShape(tip, 0, 32.0, 1.0, 0.0);
    const DabMask m = renderDabMask(tip, 0, s, 0.0, 0.0);
    REQUIRE(m.width == 32);
    REQUIRE(m.height == 32);
    for (std::uint32_t y = 0; y < 32; ++y)
        for (std::uint32_t x = 0; x < 32; ++x)
            CHECK(m.at(x, y) == 255 - grey[y * 32 + x]);
}

TEST_CASE("a sub-pixel phase fades the tip against the zero border") {
    const BitmapTip tip{{solidFrame(32, 32)}, TipApplication::AlphaMask, TipSourceKind::Mask};
    const DabShape s = bitmapDabShape(tip, 0, 32.0, 1.0, 0.0);
    const DabMask m = renderDabMask(tip, 0, s, 0.5, 0.0);
    REQUIRE(m.width == 33); // the half-pixel shift widens the footprint
    REQUIRE(m.height == 32);
    for (std::uint32_t y = 0; y < 32; ++y) {
        CHECK(m.at(0, y) == 128); // half in, half out
        CHECK(m.at(32, y) == 128);
        for (std::uint32_t x = 1; x < 32; ++x)
            CHECK(m.at(x, y) == 255);
    }
}

TEST_CASE("minifying a fine pattern averages it rather than sampling one pixel of it") {
    // The point of the mip chain. A 1px checkerboard at 128x128 stamped as an 8x8 dab must come out
    // mid-grey. Plain bilinear on the base level would land on single texels and produce a hard
    // black-and-white sparkle -- exactly the aliasing a hose tip shows as flicker.
    std::vector<std::uint8_t> grey(128 * 128);
    for (std::uint32_t y = 0; y < 128; ++y)
        for (std::uint32_t x = 0; x < 128; ++x)
            grey[y * 128 + x] = ((x + y) % 2 == 0) ? 0 : 255; // black/white checker

    const BitmapTip tip{{frameFromGrey(128, 128, grey)}, TipApplication::AlphaMask,
                        TipSourceKind::Mask};
    const DabShape s = bitmapDabShape(tip, 0, 8.0, 1.0, 0.0);
    const DabMask m = renderDabMask(tip, 0, s, 0.0, 0.0);
    REQUIRE(m.width == 8);

    for (std::uint32_t y = 2; y < 6; ++y)
        for (std::uint32_t x = 2; x < 6; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            CHECK(m.at(x, y) > 100);
            CHECK(m.at(x, y) < 155); // a plain bilinear tap would be 0 or 255 here
        }
}

TEST_CASE("coverage scales with the dab's area") {
    const BitmapTip tip{{solidFrame(64, 64)}, TipApplication::AlphaMask, TipSourceKind::Mask};
    const double big = totalCoverage(renderDabMask(tip, 0, bitmapDabShape(tip, 0, 64, 1, 0), 0, 0));
    const double half = totalCoverage(renderDabMask(tip, 0, bitmapDabShape(tip, 0, 32, 1, 0), 0, 0));
    // Halving the diameter quarters the painted area, give or take the anti-aliased rim.
    CHECK(half == doctest::Approx(big / 4.0).epsilon(0.05));
}

TEST_CASE("an out-of-range frame renders nothing rather than reading out of bounds") {
    const BitmapTip tip{{solidFrame(8, 8)}, TipApplication::AlphaMask, TipSourceKind::Mask};
    const DabShape s = bitmapDabShape(tip, 0, 8.0, 1.0, 0.0);
    CHECK(renderDabMask(tip, -1, s, 0.0, 0.0).empty());
    CHECK(renderDabMask(tip, 1, s, 0.0, 0.0).empty());
    CHECK(renderDabMask(tip, 99, s, 0.0, 0.0).empty());
    CHECK(renderDabMask(BitmapTip{}, 0, s, 0.0, 0.0).empty());
}

TEST_CASE("rotating a bitmap dab preserves its coverage mass") {
    const BitmapTip tip{{solidFrame(48, 24)}, TipApplication::AlphaMask, TipSourceKind::Mask};
    const double base = totalCoverage(renderDabMask(tip, 0, bitmapDabShape(tip, 0, 48, 1, 0), 0, 0));
    for (double a : {0.3, 1.0, kPi / 2, 2.5, -0.9}) {
        const DabShape s = bitmapDabShape(tip, 0, 48, 1, a);
        const double m = totalCoverage(renderDabMask(tip, 0, s, 0.0, 0.0));
        CAPTURE(a);
        CHECK(m == doctest::Approx(base).epsilon(0.03));
    }
}

TEST_CASE("mirroring a bitmap dab is a real reflection") {
    // Asymmetric: a bright wedge on the left half only.
    std::vector<std::uint8_t> grey(16 * 16, 255);
    for (std::uint32_t y = 0; y < 16; ++y)
        for (std::uint32_t x = 0; x < 6; ++x)
            grey[y * 16 + x] = 0;
    const BitmapTip tip{{frameFromGrey(16, 16, grey)}, TipApplication::AlphaMask,
                        TipSourceKind::Mask};

    DabShape s = bitmapDabShape(tip, 0, 16.0, 1.0, 0.0);
    const DabMask plain = renderDabMask(tip, 0, s, 0.0, 0.0);
    s.mirrorH = true;
    const DabMask flipped = renderDabMask(tip, 0, s, 0.0, 0.0);
    REQUIRE(plain.width == flipped.width);
    for (std::uint32_t y = 0; y < plain.height; ++y)
        for (std::uint32_t x = 0; x < plain.width; ++x)
            CHECK(plain.at(x, y) == flipped.at(plain.width - 1 - x, y));
    CHECK(plain.at(2, 8) == 255);
    CHECK(flipped.at(2, 8) == 0);
}
