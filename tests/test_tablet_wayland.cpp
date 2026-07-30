#include <doctest/doctest.h>

#include "platform/tablet_wayland.hpp"

#include <cstdint>
#include <vector>

// The zwp_tablet_v2 backend's parse/normalize + lifecycle path (docs/tablet.md §4), exercised
// headlessly per §9: WaylandTool folds hand-fed wire values into a running state and frame()
// dispatches the stroke lifecycle to a recording sink -- no compositor, no wl_display. Only
// TabletWayland's registry/seat plumbing needs a live server, and that half is covered by the
// spike (tools/tablet_spike_fltk), not by this binary. The protocol enum values used here are the
// frozen wire ABI (tablet_wayland.cpp static_asserts them against the generated header).

using mosaic::platform::TabletSample;
using mosaic::platform::TabletStrokeSink;
using mosaic::platform::WaylandTool;
using mosaic::platform::waylandButtonBit;
using mosaic::platform::waylandToolType;
namespace wl_tool_type = mosaic::platform::wl_tool_type;

namespace {

// The opaque id of the surface the tool entered. Every sample the tool then produces carries it:
// `pos` is surface-local, and the wiring watches more than one of our surfaces (the canvas and the
// settings dialog), so a sample has to say which one it is in.
constexpr std::uint64_t kSurface = 0xC0FFEE;

// zwp_tablet_tool_v2 wire constants the tests feed in (frozen protocol ABI).
constexpr std::uint32_t kCapTilt = 1;
constexpr std::uint32_t kCapPressure = 2;
constexpr std::uint32_t kCapSlider = 5;
constexpr std::uint32_t kBtnStylus = 0x14b;  // BTN_STYLUS
constexpr std::uint32_t kBtnStylus2 = 0x14c; // BTN_STYLUS2
constexpr std::uint32_t kBtnStylus3 = 0x149; // BTN_STYLUS3

// Records every lifecycle callback in order, with the sample that drove it.
struct RecordingSink : TabletStrokeSink {
    enum class Kind { Begin, Motion, End, Hover, ProxOut };
    struct Call {
        Kind kind;
        TabletSample sample;
    };
    std::vector<Call> calls;

    void tabletStrokeBegin(const TabletSample& s) override { calls.push_back({Kind::Begin, s}); }
    void tabletStrokeMotion(const TabletSample& s) override { calls.push_back({Kind::Motion, s}); }
    void tabletStrokeEnd(const TabletSample& s) override { calls.push_back({Kind::End, s}); }
    void tabletHover(const TabletSample& s) override { calls.push_back({Kind::Hover, s}); }
    void tabletProximityOut() override { calls.push_back({Kind::ProxOut, {}}); }

    [[nodiscard]] std::size_t count() const { return calls.size(); }
    [[nodiscard]] Kind lastKind() const { return calls.back().kind; }
    [[nodiscard]] const TabletSample& last() const { return calls.back().sample; }
};

} // namespace

// ---------------------------------------------------------------------------------------------
// Tool classification
// ---------------------------------------------------------------------------------------------

TEST_CASE("tablet wayland: every protocol tool type maps to the model") {
    CHECK(waylandToolType(wl_tool_type::kPen) == TabletSample::Tool::Pen);
    CHECK(waylandToolType(wl_tool_type::kBrush) == TabletSample::Tool::Pen);
    CHECK(waylandToolType(wl_tool_type::kPencil) == TabletSample::Tool::Pen);
    CHECK(waylandToolType(wl_tool_type::kEraser) == TabletSample::Tool::Eraser);
    CHECK(waylandToolType(wl_tool_type::kAirbrush) == TabletSample::Tool::Airbrush);
    CHECK(waylandToolType(wl_tool_type::kLens) == TabletSample::Tool::Puck);
    CHECK(waylandToolType(wl_tool_type::kMouse) == TabletSample::Tool::Mouse);
    CHECK(waylandToolType(wl_tool_type::kFinger) == TabletSample::Tool::Mouse);
    // An unknown future type is a drawing tool until proven otherwise.
    CHECK(waylandToolType(0x9999) == TabletSample::Tool::Pen);
}

TEST_CASE("tablet wayland: setType rides every sample") {
    WaylandTool t;
    t.setType(wl_tool_type::kEraser);
    CHECK(t.tool() == TabletSample::Tool::Eraser);
    CHECK(t.sample(0).tool == TabletSample::Tool::Eraser);
}

// ---------------------------------------------------------------------------------------------
// Axis normalization
// ---------------------------------------------------------------------------------------------

TEST_CASE("tablet wayland: pressure normalizes over the protocol's [0,65535]") {
    WaylandTool t;
    t.addCapability(kCapPressure);
    t.pressure(32768);
    CHECK(t.sample(0).pressure == doctest::Approx(32768.0 / 65535.0));
    t.pressure(65535);
    CHECK(t.sample(0).pressure == doctest::Approx(1.0));
    t.pressure(0);
    CHECK(t.sample(0).pressure == doctest::Approx(0.0)); // a real sensor reading zero IS zero
}

TEST_CASE("tablet wayland: a tool with no pressure reports 1.0, never 0 (§3.2)") {
    WaylandTool t; // e.g. a pressure-less screen digitizer; no pressure capability, no event
    CHECK(t.sample(0).pressure == doctest::Approx(1.0));
}

TEST_CASE("tablet wayland: a pressure event alone enables normalization (no capability burst)") {
    WaylandTool t; // capability event never arrived, but pressure events do
    t.pressure(16384);
    CHECK(t.sample(0).pressure == doctest::Approx(16384.0 / 65535.0));
}

TEST_CASE("tablet wayland: tilt and rotation pass through as degrees, unscaled") {
    WaylandTool t;
    t.addCapability(kCapTilt);
    t.tilt(30.0, -15.0); // zwp_tablet_v2 already speaks degrees -- no device-range scaling here
    t.rotation(90.0);
    const TabletSample s = t.sample(0);
    CHECK(s.xTilt == doctest::Approx(30.0));
    CHECK(s.yTilt == doctest::Approx(-15.0));
    CHECK(s.rotation == doctest::Approx(90.0));
}

TEST_CASE("tablet wayland: slider maps to tangential pressure, neutral at 0.5") {
    WaylandTool t;
    t.addCapability(kCapSlider);
    t.slider(0); // the protocol's logical-neutral position
    CHECK(t.sample(0).tangentialPressure == doctest::Approx(0.5)); // matches X11 wheel-at-midpoint
    t.slider(65535);
    CHECK(t.sample(0).tangentialPressure == doctest::Approx(1.0));
    t.slider(-65535);
    CHECK(t.sample(0).tangentialPressure == doctest::Approx(0.0));
}

TEST_CASE("tablet wayland: a tool with no slider reports tangential 0.0 (rest), not 0.5") {
    WaylandTool t;
    CHECK(t.sample(0).tangentialPressure == doctest::Approx(0.0));
}

TEST_CASE("tablet wayland: motion coordinates are sub-pixel doubles, straight through") {
    WaylandTool t;
    t.motion(752.4805, 42.0625); // the spike's live sub-pixel coordinate shape
    const TabletSample s = t.sample(0);
    CHECK(s.pos.x == doctest::Approx(752.4805));
    CHECK(s.pos.y == doctest::Approx(42.0625));
}

TEST_CASE("tablet wayland: the serial is assembled hi<<32 | lo") {
    WaylandTool t;
    t.setSerial(0x0000ABCD, 0x12345678);
    CHECK(t.sample(0).toolSerial == 0x0000ABCD12345678ULL);
}

TEST_CASE("tablet wayland: OUR timeUs is stamped verbatim") {
    WaylandTool t;
    CHECK(t.sample(123456789).timeUs == 123456789ULL);
}

TEST_CASE("tablet wayland: axis values persist across frames that omit them") {
    WaylandTool t;
    t.addCapability(kCapPressure);
    t.pressure(49152); // 0.75
    CHECK(t.sample(0).pressure == doctest::Approx(0.75));
    t.motion(5.0, 5.0); // a motion-only frame: pressure omitted, must carry
    CHECK(t.sample(0).pressure == doctest::Approx(0.75));
}

// ---------------------------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------------------------

TEST_CASE("tablet wayland: the barrel-button bit map matches the X11 numbering") {
    CHECK(waylandButtonBit(kBtnStylus) == 1);  // button 2 (lower barrel)
    CHECK(waylandButtonBit(kBtnStylus2) == 2); // button 3 (upper barrel)
    CHECK(waylandButtonBit(kBtnStylus3) == 3); // button 4
    CHECK(waylandButtonBit(0x110) == -1);      // BTN_LEFT: a puck's button, deliberately dropped
    CHECK(waylandButtonBit(0) == -1);
}

TEST_CASE("tablet wayland: the tip rides bit 0 off contact; barrels ride 1..") {
    WaylandTool t;
    CHECK(t.sample(0).buttons == 0u); // hovering, no buttons
    t.button(kBtnStylus, true);
    CHECK(t.sample(0).buttons == 0b10u); // lower barrel, no tip
    t.down();
    CHECK(t.sample(0).buttons == 0b11u); // tip contact adds bit 0
    t.button(kBtnStylus2, true);
    CHECK(t.sample(0).buttons == 0b111u);
    t.button(kBtnStylus, false);
    CHECK(t.sample(0).buttons == 0b101u); // released lower barrel; tip + upper barrel remain
    t.up();
    CHECK(t.sample(0).buttons == 0b100u); // tip lifted, upper barrel still held
}

// ---------------------------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------------------------

TEST_CASE("tablet wayland: a hover/stroke/lift sequence drives the sink in order") {
    WaylandTool t;
    t.addCapability(kCapPressure);
    RecordingSink sink;

    // Hover in.
    t.proximityIn(kSurface);
    t.motion(100.0, 100.0);
    t.frame(1, &sink);
    REQUIRE(sink.count() == 1);
    CHECK(sink.lastKind() == RecordingSink::Kind::Hover);
    CHECK(sink.last().pos.x == doctest::Approx(100.0));
    CHECK(sink.last().inProximity);

    // Tip down -> stroke begins.
    t.down();
    t.pressure(30000);
    t.frame(2, &sink);
    REQUIRE(sink.count() == 2);
    CHECK(sink.lastKind() == RecordingSink::Kind::Begin);
    CHECK((sink.last().buttons & 1u) == 1u); // tip contact reflected

    // Move while down -> motion.
    t.motion(110.0, 100.0);
    t.frame(3, &sink);
    CHECK(sink.lastKind() == RecordingSink::Kind::Motion);

    // Tip up -> stroke ends, still hovering.
    t.up();
    t.frame(4, &sink);
    CHECK(sink.lastKind() == RecordingSink::Kind::End);
    CHECK(sink.last().inProximity);
    CHECK((sink.last().buttons & 1u) == 0u); // tip lifted

    // Leave proximity -> proximity-out.
    t.proximityOut();
    t.frame(5, &sink);
    CHECK(sink.lastKind() == RecordingSink::Kind::ProxOut);
}

TEST_CASE("tablet wayland: down and motion in one frame begins the stroke (endpoint kept)") {
    WaylandTool t;
    RecordingSink sink;
    t.proximityIn(kSurface);
    t.frame(1, &sink); // hover
    // A pen that comes down mid-motion: both in the same frame. The begin must win, or the stroke
    // would start as a bare motion with no begin.
    t.down();
    t.motion(50.0, 60.0);
    t.frame(2, &sink);
    CHECK(sink.lastKind() == RecordingSink::Kind::Begin);
    CHECK(sink.last().pos.x == doctest::Approx(50.0));
}

TEST_CASE("tablet wayland: up + proximity_out in one frame ends the stroke AND leaves") {
    WaylandTool t;
    RecordingSink sink;
    t.proximityIn(kSurface);
    t.down();
    t.frame(1, &sink); // begin
    REQUIRE(sink.lastKind() == RecordingSink::Kind::Begin);

    // Pen lifted clear off the tablet: the protocol sends up, then proximity_out, in one frame.
    // ⚠ BOTH must reach the sink, End first. The first cut of this file dispatched exactly one
    // callback per frame ("End must win") -- and the swallowed leave meant no FL_LEAVE downstream:
    // the canvas kept the pointer "inside", and the pen's cursor came back over the NEXT surface
    // still wearing the canvas's Hidden. That is the sometimes-invisible cursor on native Wayland
    // (user-reported 2026-07-14): it only happened when the pen left the pad AT a stroke's end,
    // because a hover-then-lift puts the up and the proximity_out in separate frames.
    t.up();
    t.proximityOut();
    t.frame(2, &sink);
    REQUIRE(sink.count() == 3); // End AND ProxOut -- the leave is a separate fact, not an either/or
    CHECK(sink.calls[1].kind == RecordingSink::Kind::End);
    CHECK_FALSE(sink.calls[1].sample.inProximity); // the End itself already reports the pen gone
    CHECK(sink.calls[2].kind == RecordingSink::Kind::ProxOut); // ... and the leave follows the End
}

TEST_CASE("tablet wayland: a frame out of proximity with no transition dispatches nothing") {
    WaylandTool t;
    RecordingSink sink;
    t.motion(1.0, 1.0); // stray motion while not in proximity
    t.frame(1, &sink);
    CHECK(sink.count() == 0);
}

TEST_CASE("tablet wayland: per-frame transitions clear -- a stale down does not re-begin") {
    WaylandTool t;
    RecordingSink sink;
    t.proximityIn(kSurface);
    t.down();
    t.frame(1, &sink); // Begin
    t.frame(2, &sink); // no new transition; still down -> Motion, not a second Begin
    REQUIRE(sink.count() == 2);
    CHECK(sink.calls[0].kind == RecordingSink::Kind::Begin);
    CHECK(sink.calls[1].kind == RecordingSink::Kind::Motion);
}

TEST_CASE("tablet wayland: a null sink updates state but dispatches nothing") {
    WaylandTool t;
    t.proximityIn(kSurface);
    t.down();
    t.frame(1, nullptr); // the backend is not wired to a canvas yet
    CHECK(t.inProximity());
    CHECK(t.isDown());
    // And a later real sink sees the CURRENT state, not a replay.
    RecordingSink sink;
    t.motion(7.0, 7.0);
    t.frame(2, &sink);
    CHECK(sink.lastKind() == RecordingSink::Kind::Motion);
}

TEST_CASE("tablet wayland: a pressure pen hovering reports its real low pressure, not 1.0") {
    WaylandTool t;
    t.addCapability(kCapPressure);
    RecordingSink sink;
    t.proximityIn(kSurface);
    t.pressure(0); // hovering: a real sensor reads ~0, and that is honest, not the §3.2 fallback
    t.frame(1, &sink);
    CHECK(sink.lastKind() == RecordingSink::Kind::Hover);
    CHECK(sink.last().pressure == doctest::Approx(0.0));
}
