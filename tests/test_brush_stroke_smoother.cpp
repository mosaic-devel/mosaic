#include <doctest/doctest.h>

#include "core/brush/stroke_smoother.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <vector>

// Brush smoothing: a FIXED-window Gaussian average of the recent input positions
// (core/brush/stroke_smoother.hpp).
//
// This is the one place in the brush pipeline that FILTERS -- that moves the user's input points --
// and it exists because interpolation could not fix the mouse and never could have: a curve THROUGH
// the samples reproduces the samples exactly, and a mouse's samples are integer positions at 60 Hz.

using mosaic::common::Vec2;
using mosaic::core::brush::kMaxSmoothingWindow;
using mosaic::core::brush::SmoothingParams;
using mosaic::core::brush::smoothingWindow;
using mosaic::core::brush::StrokeInput;
using mosaic::core::brush::StrokeSmoother;

namespace {

[[nodiscard]] StrokeInput at(double x, double y, double pressure = 1.0) {
    StrokeInput s;
    s.pos = {x, y};
    s.pressure = pressure;
    return s;
}

} // namespace

TEST_CASE("smoother: OFF is an exact identity") {
    // Not "close to" the input: the same bits. This is what keeps every existing golden, and every
    // tablet stroke, unchanged unless the user actually asks for smoothing.
    StrokeSmoother sm;
    sm.setParams(SmoothingParams{}); // strength 0
    sm.begin(at(10.0, 10.0));

    const StrokeInput raw = at(13.25, 17.5, 0.42);
    const StrokeInput out = sm.smooth(raw);
    CHECK(out.pos.x == raw.pos.x);
    CHECK(out.pos.y == raw.pos.y);
    CHECK(out.pressure == raw.pressure);
    CHECK(sm.flush().empty()); // nothing was ever held back, so nothing is owed
}

TEST_CASE("smoother: ⚠ THE WINDOW IS A FUNCTION OF STRENGTH AND NOTHING ELSE") {
    // ⚠⚠ THE FIXED-WINDOW INVARIANT, PINNED. The window is a function of the user's strength setting
    // and NOTHING else. It must never adapt to how fast the pointer is moving -- not behind a flag,
    // not as an option, not "just for the tablet".
    //
    // "Shrink the window when the pointer moves fast, so it feels more responsive" is precisely the
    // improvement a later session would reach for. If someone adds a speed, a velocity, a sample
    // spacing or a timestamp to smoothingWindow(), THIS TEST WILL NOT COMPILE -- and that is the
    // point of writing it as a signature check rather than a behavioural one.
    static_assert(std::is_invocable_r_v<std::size_t, decltype(smoothingWindow), double>,
                  "smoothingWindow must take the user's strength and NOTHING else -- the window is "
                  "FIXED and must never adapt to pointer speed (core/brush/stroke_smoother.hpp)");

    CHECK(smoothingWindow(0.0) == 1); // off: an average of one sample is that sample
    CHECK(smoothingWindow(1.0) == kMaxSmoothingWindow);
    CHECK(smoothingWindow(0.5) > 1);
    CHECK(smoothingWindow(0.5) < kMaxSmoothingWindow);
    // Monotone in strength, and bounded. A "window" that could grow without bound is not a fixed one.
    CHECK(smoothingWindow(0.25) <= smoothingWindow(0.75));
    CHECK(smoothingWindow(5.0) == kMaxSmoothingWindow); // clamped, not extrapolated
}

TEST_CASE("smoother: the same input, moved FAST or SLOW, is smoothed the SAME") {
    // The behavioural half of the constraint above. Two strokes trace the identical path; one is
    // sampled as if the pointer were crawling, the other as if it were flying (the timestamps differ
    // by 50x). The filter must not care. If it ever starts caring, it has become velocity-adaptive.
    const std::vector<Vec2> path{{0, 0}, {10, 1}, {20, -1}, {30, 2}, {40, 0}, {50, 1}};
    SmoothingParams p;
    p.strength = 0.6;

    const auto run = [&](std::uint64_t dt) {
        StrokeSmoother sm;
        sm.setParams(p);
        StrokeInput first = at(path[0].x, path[0].y);
        first.timeUs = 0;
        sm.begin(first);
        std::vector<Vec2> out;
        for (std::size_t i = 1; i < path.size(); ++i) {
            StrokeInput s = at(path[i].x, path[i].y);
            s.timeUs = static_cast<std::uint64_t>(i) * dt; // the ONLY difference between the runs
            out.push_back(sm.smooth(s).pos);
        }
        return out;
    };

    const std::vector<Vec2> slow = run(50'000); // 20 Hz -- a crawl
    const std::vector<Vec2> fast = run(1'000);  // 1 kHz -- a flick
    REQUIRE(slow.size() == fast.size());
    for (std::size_t i = 0; i < slow.size(); ++i) {
        CHECK(slow[i].x == doctest::Approx(fast[i].x));
        CHECK(slow[i].y == doctest::Approx(fast[i].y));
    }
}

TEST_CASE("smoother: it actually removes the noise it exists to remove") {
    // THE JOB. A straight drag with the pixel-grid rattle a 60 Hz mouse delivers: the true path is
    // y = 0, and every sample is off it by +-0.5 px because Fl::event_x/y() are INTEGERS.
    SmoothingParams p;
    p.strength = 0.7;
    StrokeSmoother sm;
    sm.setParams(p);
    sm.begin(at(0.0, 0.0));

    double rawErr = 0.0, smoothErr = 0.0;
    int n = 0;
    for (int i = 1; i <= 40; ++i) {
        const double jitter = (i % 2 == 0) ? 0.5 : -0.5; // the quantisation rattle
        const StrokeInput raw = at(static_cast<double>(i) * 4.0, jitter);
        const StrokeInput out = sm.smooth(raw);
        if (i > 10) { // let the window fill
            rawErr += raw.pos.y * raw.pos.y;
            smoothErr += out.pos.y * out.pos.y;
            ++n;
        }
    }
    const double rawRms = std::sqrt(rawErr / n);
    const double smoothRms = std::sqrt(smoothErr / n);
    CHECK(rawRms == doctest::Approx(0.5)); // the input really is that noisy
    CHECK(smoothRms < rawRms / 5.0);       // ... and the filter really does take it out
}

TEST_CASE("smoother: a stroke ENDS where the pointer ended") {
    // An averaged point necessarily TRAILS the raw input -- that is what a filter does. So without a
    // flush, a stroke would fall SHORT of the last thing the user did: the pen lifts at the end of a
    // flick and the paint never gets there. flush() ramps the window down and finishes on the user's
    // own final sample, unsmoothed.
    SmoothingParams p;
    p.strength = 0.8;
    StrokeSmoother sm;
    sm.setParams(p);
    sm.begin(at(0.0, 0.0));

    StrokeInput lastRaw = at(0.0, 0.0);
    for (int i = 1; i <= 20; ++i) {
        lastRaw = at(static_cast<double>(i) * 5.0, 0.0);
        const StrokeInput out = sm.smooth(lastRaw);
        CHECK(out.pos.x < lastRaw.pos.x); // it lags, as a filter must
    }

    const std::vector<StrokeInput> owed = sm.flush();
    REQUIRE(!owed.empty());
    // The tail marches forward and lands exactly on the user's last point.
    for (std::size_t i = 1; i < owed.size(); ++i)
        CHECK(owed[i].pos.x >= owed[i - 1].pos.x - 1e-9);
    CHECK(owed.back().pos.x == doctest::Approx(lastRaw.pos.x));
    CHECK(owed.back().pos.y == doctest::Approx(lastRaw.pos.y));

    CHECK(sm.flush().empty()); // idempotent: nothing is owed twice
}

TEST_CASE("smoother: it never invents a point outside the path the user drew") {
    // A weighted average of points is a CONVEX COMBINATION of them, so the smoothed point can only
    // ever lie inside the hull of the recent samples. It cannot overshoot, ring, or fly off -- which
    // is a property a naive IIR filter would NOT have, and is worth pinning.
    SmoothingParams p;
    p.strength = 1.0;
    StrokeSmoother sm;
    sm.setParams(p);
    sm.begin(at(0.0, 0.0));

    std::vector<Vec2> seen{{0.0, 0.0}};
    for (int i = 1; i <= 30; ++i) {
        // a hard right-angle turn -- the case that makes an overshooting filter ring
        const Vec2 q = (i < 15) ? Vec2{static_cast<double>(i), 0.0}
                                : Vec2{14.0, static_cast<double>(i - 14)};
        seen.push_back(q);
        const StrokeInput out = sm.smooth(at(q.x, q.y));
        double minx = 1e9, maxx = -1e9, miny = 1e9, maxy = -1e9;
        for (const Vec2& v : seen) {
            minx = std::min(minx, v.x);
            maxx = std::max(maxx, v.x);
            miny = std::min(miny, v.y);
            maxy = std::max(maxy, v.y);
        }
        CHECK(out.pos.x >= minx - 1e-9);
        CHECK(out.pos.x <= maxx + 1e-9);
        CHECK(out.pos.y >= miny - 1e-9);
        CHECK(out.pos.y <= maxy + 1e-9);
    }
}

TEST_CASE("smoother: pressure, tilt and time pass through UNTOUCHED") {
    // Position only. Averaging pressure would mush the very dynamics the tablet work exists to
    // deliver, and averaging the timestamp would lie about when the sample happened.
    SmoothingParams p;
    p.strength = 0.9;
    StrokeSmoother sm;
    sm.setParams(p);
    sm.begin(at(0.0, 0.0, 0.1));

    for (int i = 1; i <= 10; ++i) {
        StrokeInput raw = at(static_cast<double>(i) * 3.0, 0.0, 0.05 * i);
        raw.xTilt = 12.0 * i;
        raw.timeUs = 1000u * static_cast<std::uint64_t>(i);
        const StrokeInput out = sm.smooth(raw);
        CHECK(out.pressure == raw.pressure); // exactly, not approximately
        CHECK(out.xTilt == raw.xTilt);
        CHECK(out.timeUs == raw.timeUs);
        CHECK(out.pos.x != raw.pos.x); // ... while the POSITION really was filtered
    }
}
