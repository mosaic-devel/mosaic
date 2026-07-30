#include <doctest/doctest.h>

#include "platform/tablet.hpp"

// The unified tablet event model (docs/tablet.md §2): the sample struct's contract, the ring the
// backends fill, and the ingest clock. All headless -- no backend, no window, no device.

using mosaic::platform::ingestClockUs;
using mosaic::platform::SampleRing;
using mosaic::platform::TabletSample;

namespace {

[[nodiscard]] TabletSample at(double x, std::uint64_t t = 0) {
    TabletSample s;
    s.pos = {x, 0.0};
    s.timeUs = t;
    return s;
}

} // namespace

TEST_CASE("tablet: sample defaults are the safe fallback") {
    const TabletSample s{};
    // Pressure defaults to 1, not 0: a default-constructed (synthesized) sample must paint at
    // full strength rather than silently collapsing the stroke (docs/tablet.md §3.2).
    CHECK(s.pressure == 1.0);
    CHECK(s.tool == TabletSample::Tool::Pen);
    CHECK(s.xTilt == 0.0);
    CHECK(s.yTilt == 0.0);
    CHECK(s.rotation == 0.0);
    CHECK(s.tangentialPressure == 0.0);
    CHECK(s.toolSerial == 0);
    CHECK_FALSE(s.inProximity);
    CHECK(s.buttons == 0);
}

TEST_CASE("tablet: ring is FIFO and pop on empty is a refusal") {
    SampleRing ring(8);
    TabletSample out = at(-1.0);
    CHECK_FALSE(ring.pop(out));
    CHECK(out.pos.x == -1.0); // untouched on refusal

    ring.push(at(1.0));
    ring.push(at(2.0));
    ring.push(at(3.0));
    CHECK(ring.size() == 3);
    REQUIRE(ring.pop(out));
    CHECK(out.pos.x == 1.0);
    REQUIRE(ring.pop(out));
    CHECK(out.pos.x == 2.0);
    REQUIRE(ring.pop(out));
    CHECK(out.pos.x == 3.0);
    CHECK(ring.empty());
    CHECK(ring.overwritten() == 0);
}

TEST_CASE("tablet: ring capacity rounds up to a power of two with a floor of 2") {
    CHECK(SampleRing(3).capacity() == 4);
    CHECK(SampleRing(8).capacity() == 8);
    CHECK(SampleRing(0).capacity() == 2);
    CHECK(SampleRing(1).capacity() == 2);
    CHECK(SampleRing(513).capacity() == 1024);
}

TEST_CASE("tablet: a full ring overwrites the OLDEST and counts what it lost") {
    SampleRing ring(4); // capacity 4
    for (int i = 0; i < 7; ++i)
        ring.push(at(static_cast<double>(i)));
    // 7 pushed into 4 slots: 0..2 were overwritten, 3..6 remain, oldest-first.
    CHECK(ring.size() == 4);
    CHECK(ring.overwritten() == 3);
    TabletSample out;
    for (int expect = 3; expect <= 6; ++expect) {
        REQUIRE(ring.pop(out));
        CHECK(out.pos.x == static_cast<double>(expect)); // the ENDPOINT survives, interior detail goes
    }
    CHECK(ring.empty());
}

TEST_CASE("tablet: ring ordering survives many wraparounds") {
    SampleRing ring(4);
    TabletSample out;
    // Interleaved push/pop far past the capacity -- one sample stays buffered, so the head and
    // tail sweep the 4-slot buffer 25 times over. The masked free-running indices must keep FIFO
    // order across every wrap.
    for (int i = 0; i < 100; ++i) {
        ring.push(at(static_cast<double>(i)));
        if (i >= 1) {
            REQUIRE(ring.pop(out));
            CHECK(out.pos.x == static_cast<double>(i - 1));
        }
    }
    REQUIRE(ring.pop(out));
    CHECK(out.pos.x == 99.0);
    CHECK(ring.empty());
    CHECK(ring.overwritten() == 0); // never actually filled
}

TEST_CASE("tablet: clear drops the queue but keeps the lifetime loss count") {
    SampleRing ring(2);
    ring.push(at(1.0));
    ring.push(at(2.0));
    ring.push(at(3.0)); // overwrites
    CHECK(ring.overwritten() == 1);
    ring.clear();
    CHECK(ring.empty());
    CHECK(ring.overwritten() == 1); // a diagnostic, not queue state
    ring.push(at(4.0));
    TabletSample out;
    REQUIRE(ring.pop(out));
    CHECK(out.pos.x == 4.0);
}

TEST_CASE("tablet: the ingest clock is monotone") {
    const std::uint64_t a = ingestClockUs();
    const std::uint64_t b = ingestClockUs();
    const std::uint64_t c = ingestClockUs();
    CHECK(a <= b);
    CHECK(b <= c);
}
