#include <doctest/doctest.h>

#include "platform/tablet_x11.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// The XI2 backend's parse/normalize path (docs/tablet.md §3), exercised headlessly per §9: the
// tests construct XIDeviceInfo / XIDeviceEvent structures directly -- no X server, no Display.
// Only xi2Classify and xi2ParseEvent are under test here; TabletX11's enumerate/select plumbing
// needs a live server and is covered by the spike-style probe, not by this binary.

using mosaic::platform::kTiltFullScaleDegrees;
using mosaic::platform::TabletSample;
using mosaic::platform::Xi2AxisLabels;
using mosaic::platform::xi2Classify;
using mosaic::platform::Xi2Device;
using mosaic::platform::xi2ParseEvent;

namespace {

// Arbitrary distinct nonzero Atom values -- the classifier only ever compares them (the live
// backend interns the real ones; the header documents that contract).
constexpr unsigned long kAbsPressure = 101;
constexpr unsigned long kAbsTiltX = 102;
constexpr unsigned long kAbsTiltY = 103;
constexpr unsigned long kAbsWheel = 104;
constexpr unsigned long kAbsX = 105;         // "Abs X" -- the device's own position (§3.1)
constexpr unsigned long kAbsY = 106;         // "Abs Y"
constexpr unsigned long kUnknownLabel = 199; // a label the backend does not recognize at all

[[nodiscard]] Xi2AxisLabels labels() {
    Xi2AxisLabels l;
    l.absPressure = kAbsPressure;
    l.absTiltX = kAbsTiltX;
    l.absTiltY = kAbsTiltY;
    l.absWheel = kAbsWheel;
    l.absX = kAbsX;
    l.absY = kAbsY;
    return l;
}

// Owns the storage an XIDeviceInfo points into. The XI structs are plain C with interior
// pointers, so the builder must outlive the info it hands out.
struct DeviceBuilder {
    std::string name;
    int deviceId = 7;
    int use = XISlavePointer;
    std::vector<XIValuatorClassInfo> valuators;
    std::vector<XIAnyClassInfo*> classPtrs;

    DeviceBuilder& axis(int number, unsigned long label, double min, double max, double value = 0.0) {
        XIValuatorClassInfo v{};
        v.type = XIValuatorClass;
        v.number = number;
        v.label = static_cast<Atom>(label);
        v.min = min;
        v.max = max;
        v.value = value;
        v.mode = XIModeAbsolute;
        valuators.push_back(v);
        return *this;
    }

    [[nodiscard]] XIDeviceInfo build() {
        classPtrs.clear();
        for (XIValuatorClassInfo& v : valuators)
            classPtrs.push_back(reinterpret_cast<XIAnyClassInfo*>(&v));
        XIDeviceInfo info{};
        info.deviceid = deviceId;
        info.name = name.data();
        info.use = use;
        info.enabled = 1;
        info.num_classes = static_cast<int>(classPtrs.size());
        info.classes = classPtrs.data();
        return info;
    }
};

// Seed one axis's enumerate-time value BY LABEL. Indexing DeviceBuilder::valuators positionally is
// a trap: stylus() grew an Abs Y axis and every [1] silently became a different axis.
void seed(DeviceBuilder& b, unsigned long label, double value) {
    for (XIValuatorClassInfo& v : b.valuators)
        if (v.label == static_cast<Atom>(label))
            v.value = value;
}

// Classify-or-fail: a clean REQUIRE instead of the UB of dereferencing an empty optional, so a
// classification regression fails the parse tests loudly rather than by luck.
[[nodiscard]] Xi2Device classified(DeviceBuilder& b) {
    const XIDeviceInfo info = b.build();
    std::optional<Xi2Device> dev = xi2Classify(info, labels());
    REQUIRE(dev.has_value());
    return *dev;
}

// A stylus-shaped device: pressure 0..2048 on axis 2, tilt -64..63 on axes 3/4 (the Wacom
// driver's real, ASYMMETRIC declared range).
[[nodiscard]] DeviceBuilder stylus(std::string name = "Wacom Intuos Pro S Pen stylus") {
    DeviceBuilder b;
    b.name = std::move(name);
    b.axis(0, kAbsX, 0, 44704);
    b.axis(1, kAbsY, 0, 27940);
    b.axis(2, kAbsPressure, 0, 2048);
    b.axis(3, kAbsTiltX, -64, 63);
    b.axis(4, kAbsTiltY, -64, 63);
    return b;
}

// Owns the storage an XIDeviceEvent points into, and packs the valuator values by SET BIT order
// -- the wire format's invariant the parser depends on.
struct EventBuilder {
    int evtype = XI_Motion;
    int deviceId = 7;
    int detail = 0;
    double x = 0.0;     // event_x: the server's position, RELATIVE TO THE EVENT WINDOW
    double y = 0.0;
    double rootX = 0.0; // root_x: the same position on the SCREEN. Their difference is the
    double rootY = 0.0; // window's origin, which is how the valuator path places itself.
    std::vector<std::pair<int, double>> axes; // (valuator number, value)
    std::vector<int> heldButtons;             // X button numbers (1-based)

    std::vector<unsigned char> valMask;
    std::vector<double> values;
    std::vector<unsigned char> btnMask;

    EventBuilder& axis(int number, double value) {
        axes.emplace_back(number, value);
        return *this;
    }

    [[nodiscard]] XIDeviceEvent build() {
        std::sort(axes.begin(), axes.end()); // packed by bit index, whatever order the test wrote
        valMask.clear();
        values.clear();
        for (const auto& [number, value] : axes) {
            valMask.resize(std::max<std::size_t>(valMask.size(), number / 8 + 1), 0);
            valMask[static_cast<std::size_t>(number) / 8] |= static_cast<unsigned char>(1u << (number % 8));
            values.push_back(value);
        }
        btnMask.clear();
        for (const int b : heldButtons) {
            btnMask.resize(std::max<std::size_t>(btnMask.size(), static_cast<std::size_t>(b) / 8 + 1), 0);
            btnMask[static_cast<std::size_t>(b) / 8] |= static_cast<unsigned char>(1u << (b % 8));
        }

        XIDeviceEvent ev{};
        ev.type = GenericEvent;
        ev.evtype = evtype;
        ev.deviceid = deviceId;
        ev.detail = detail;
        ev.event_x = x;
        ev.event_y = y;
        ev.root_x = rootX;
        ev.root_y = rootY;
        ev.valuators.mask_len = static_cast<int>(valMask.size());
        ev.valuators.mask = valMask.data();
        ev.valuators.values = values.data();
        ev.buttons.mask_len = static_cast<int>(btnMask.size());
        ev.buttons.mask = btnMask.data();
        return ev;
    }
};

} // namespace

// ---------------------------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------------------------

TEST_CASE("tablet x11: a pressure-bearing slave classifies with its axes recorded") {
    DeviceBuilder b = stylus();
    seed(b, kAbsPressure, 512); // driver-reported current pressure at enumerate time
    const XIDeviceInfo info = b.build();
    const std::optional<Xi2Device> dev = xi2Classify(info, labels());
    REQUIRE(dev.has_value());
    CHECK(dev->deviceId == 7);
    CHECK(dev->tool == TabletSample::Tool::Pen);
    CHECK(dev->pressure.number == 2);
    CHECK(dev->pressure.min == 0);
    CHECK(dev->pressure.max == 2048);
    CHECK(dev->pressure.value == 512); // seeded: the first event may omit the axis entirely
    CHECK(dev->tiltX.number == 3);
    CHECK(dev->tiltY.number == 4);
    CHECK_FALSE(dev->wheel.present());
}

TEST_CASE("tablet x11: masters and keyboards never classify") {
    DeviceBuilder master = stylus("Virtual core pointer");
    master.use = XIMasterPointer;
    CHECK_FALSE(xi2Classify(master.build(), labels()).has_value());

    DeviceBuilder kbd = stylus("AT Translated Set 2 keyboard");
    kbd.use = XISlaveKeyboard;
    CHECK_FALSE(xi2Classify(kbd.build(), labels()).has_value());
}

TEST_CASE("tablet x11: an ordinary mouse stays FLTK's") {
    DeviceBuilder mouse;
    mouse.name = "Logitech USB Optical Mouse";
    mouse.axis(0, kAbsX, 0, 65535); // no pressure, no tool name
    CHECK_FALSE(xi2Classify(mouse.build(), labels()).has_value());
}

TEST_CASE("tablet x11: eraser matching is CONTAINS because XWayland suffixes its devices") {
    // XWayland -- the default session -- names its emulated devices with a trailing client
    // ordinal. An ends-with match would misread the eraser as a Pen and erase in paint.
    for (const char* name : {"Wacom Intuos Pro S Pen eraser", "xwayland-tablet eraser:13",
                             "WACOM TABLET ERASER"}) {
        DeviceBuilder b = stylus(name);
        const std::optional<Xi2Device> dev = xi2Classify(b.build(), labels());
        REQUIRE(dev.has_value());
        CHECK(dev->tool == TabletSample::Tool::Eraser);
    }
}

TEST_CASE("tablet x11: airbrush and puck classify by their conventional names") {
    DeviceBuilder air = stylus("Wacom Intuos4 8x13 airbrush");
    CHECK(classified(air).tool == TabletSample::Tool::Airbrush);

    // The Wacom puck's driver name is "cursor"; XWayland follows it.
    for (const char* name : {"Wacom Intuos4 8x13 cursor", "xwayland-tablet cursor:13"}) {
        DeviceBuilder puck = stylus(name);
        CHECK(classified(puck).tool == TabletSample::Tool::Puck);
    }
}

TEST_CASE("tablet x11: a pressure-less digitizer with a tool name still classifies") {
    DeviceBuilder b;
    b.name = "Some Screen Digitizer Stylus";
    b.axis(0, kAbsX, 0, 32767); // §3.2's case: sub-pixel worthy, no pressure axis
    const std::optional<Xi2Device> dev = xi2Classify(b.build(), labels());
    REQUIRE(dev.has_value());
    CHECK(dev->tool == TabletSample::Tool::Pen);
    CHECK_FALSE(dev->pressure.present());
}

TEST_CASE("tablet x11: unlabeled valuators are ignored") {
    DeviceBuilder b;
    b.name = "Odd Tablet Pen";
    b.axis(2, 0, 0, 2048); // label 0 = unlabeled / could-not-intern: must not match anything
    const std::optional<Xi2Device> dev = xi2Classify(b.build(), labels());
    REQUIRE(dev.has_value()); // name says pen
    CHECK_FALSE(dev->pressure.present());
}

TEST_CASE("tablet x11: a floating slave still classifies") {
    DeviceBuilder b = stylus();
    b.use = XIFloatingSlave;
    CHECK(xi2Classify(b.build(), labels()).has_value());
}

// ---------------------------------------------------------------------------------------------
// Parse / normalize
// ---------------------------------------------------------------------------------------------

TEST_CASE("tablet x11: a full event normalizes every recognized axis") {
    DeviceBuilder db = stylus();
    Xi2Device dev = classified(db);

    EventBuilder eb;
    eb.x = 95.9375; // the spike's live sub-pixel coordinate, straight through
    eb.y = 42.0625;
    eb.axis(2, 1024).axis(3, 32).axis(4, 0);
    const XIDeviceEvent ev = eb.build();

    const TabletSample s = xi2ParseEvent(dev, ev, 777);
    CHECK(s.pos.x == 95.9375);
    CHECK(s.pos.y == 42.0625);
    CHECK(s.pressure == doctest::Approx(0.5)); // 1024 of [0,2048]
    // Peak-magnitude scaling of the ASYMMETRIC -64..63 range: 32 -> 32/64 of full scale. A
    // two-sided remap would put hardware zero at ~0.5 degrees of phantom lean.
    CHECK(s.xTilt == doctest::Approx(0.5 * kTiltFullScaleDegrees));
    CHECK(s.yTilt == 0.0); // and zero stays EXACTLY zero
    CHECK(s.tool == TabletSample::Tool::Pen);
    CHECK(s.inProximity);
    CHECK(s.timeUs == 777); // OUR clock, passed through verbatim -- never the event's time field
    CHECK(s.buttons == 0);
}

TEST_CASE("tablet x11: no pressure axis reports 1 and never 0") {
    DeviceBuilder b;
    b.name = "Some Screen Digitizer Stylus";
    b.axis(0, kAbsX, 0, 32767);
    Xi2Device dev = classified(b);

    EventBuilder eb;
    eb.axis(0, 100);
    const XIDeviceEvent ev = eb.build();
    CHECK(xi2ParseEvent(dev, ev, 0).pressure == 1.0); // §3.2: dynamics must not collapse the stroke
}

TEST_CASE("tablet x11: a degenerate pressure range reports 1 and never divides") {
    DeviceBuilder b;
    b.name = "Broken Digitizer Pen";
    b.axis(2, kAbsPressure, 100, 100); // min == max: the driver's claim is unusable
    Xi2Device dev = classified(b);

    EventBuilder eb;
    eb.axis(2, 100);
    const XIDeviceEvent ev = eb.build();
    CHECK(xi2ParseEvent(dev, ev, 0).pressure == 1.0);
}

TEST_CASE("tablet x11: valuators persist across events that omit them") {
    DeviceBuilder db = stylus();
    Xi2Device dev = classified(db);

    EventBuilder first;
    first.axis(2, 1024);
    const XIDeviceEvent ev1 = first.build();
    CHECK(xi2ParseEvent(dev, ev1, 0).pressure == doctest::Approx(0.5));

    // Constant pressure through a motion: XI2 omits the axis. The running value must carry.
    EventBuilder second;
    second.x = 10.0;
    const XIDeviceEvent ev2 = second.build();
    CHECK(xi2ParseEvent(dev, ev2, 0).pressure == doctest::Approx(0.5));
}

TEST_CASE("tablet x11: the enumerate-time value seeds the first event") {
    DeviceBuilder b = stylus();
    seed(b, kAbsPressure, 1536); // the pen was already down at enumerate time
    Xi2Device dev = classified(b);

    EventBuilder eb; // first event carries no valuators at all
    const XIDeviceEvent ev = eb.build();
    CHECK(xi2ParseEvent(dev, ev, 0).pressure == doctest::Approx(0.75));
}

TEST_CASE("tablet x11: an unrecognized valuator still consumes its packed slot") {
    DeviceBuilder db = stylus();
    db.axis(6, kUnknownLabel, 0, 65535); // a label the backend knows nothing about
    Xi2Device dev = classified(db);

    // Axis 6 is unrecognized, SET, and packed BEFORE nothing -- but the walk indexes the values
    // array by set-bit order, so a walker that skips the mask bit without consuming the value
    // would misread the axes that follow it. (Axis 0 cannot play this role any more: Abs X is a
    // recognized POSITION axis now.)
    EventBuilder eb;
    eb.axis(2, 1024).axis(6, 30000);
    const XIDeviceEvent ev = eb.build();
    const TabletSample s = xi2ParseEvent(dev, ev, 0);
    CHECK(s.pressure == doctest::Approx(0.5));
}

TEST_CASE("tablet x11: the wheel is tangential pressure on an airbrush") {
    DeviceBuilder b = stylus("Wacom Intuos4 8x13 airbrush");
    b.axis(5, kAbsWheel, -900, 900);
    Xi2Device dev = classified(b);
    REQUIRE(dev.tool == TabletSample::Tool::Airbrush);

    EventBuilder eb;
    eb.axis(5, 0); // wheel at rest position (range midpoint)
    const XIDeviceEvent ev = eb.build();
    const TabletSample s = xi2ParseEvent(dev, ev, 0);
    CHECK(s.tangentialPressure == doctest::Approx(0.5)); // two-sided remap to [0,1]
    CHECK(s.rotation == 0.0);                            // never both from one axis
}

TEST_CASE("tablet x11: the wheel is barrel rotation on an art pen") {
    DeviceBuilder b = stylus("Wacom Intuos4 8x13 Art Pen stylus");
    b.axis(5, kAbsWheel, -900, 900);
    Xi2Device dev = classified(b);
    REQUIRE(dev.tool == TabletSample::Tool::Pen);

    EventBuilder eb;
    eb.axis(5, 450);
    const XIDeviceEvent ev = eb.build();
    const TabletSample s = xi2ParseEvent(dev, ev, 0);
    CHECK(s.rotation == doctest::Approx(90.0)); // 450 of a +/-900 range -> a quarter turn
    CHECK(s.tangentialPressure == 0.0);

    EventBuilder rest;
    rest.axis(5, 0);
    const XIDeviceEvent evRest = rest.build();
    CHECK(xi2ParseEvent(dev, evRest, 0).rotation == 0.0); // zero stays exactly zero
}

TEST_CASE("tablet x11: buttons combine the held mask with this event's transition") {
    DeviceBuilder db = stylus();
    Xi2Device dev = classified(db);

    // Button 1 already held (the mask is the state BEFORE the event); button 2 pressed NOW.
    EventBuilder press;
    press.evtype = XI_ButtonPress;
    press.detail = 2;
    press.heldButtons = {1};
    const XIDeviceEvent evPress = press.build();
    CHECK(xi2ParseEvent(dev, evPress, 0).buttons == 0b11u);

    // Button 1 released while it was the only one held: the sample reflects the state AFTER.
    EventBuilder release;
    release.evtype = XI_ButtonRelease;
    release.detail = 1;
    release.heldButtons = {1};
    const XIDeviceEvent evRelease = release.build();
    CHECK(xi2ParseEvent(dev, evRelease, 0).buttons == 0u);

    // Motion with a held button passes the mask through; detail (0) must not clear bit... 0.
    EventBuilder motion;
    motion.heldButtons = {3};
    const XIDeviceEvent evMotion = motion.build();
    CHECK(xi2ParseEvent(dev, evMotion, 0).buttons == 0b100u);
}

TEST_CASE("tablet x11: the tool identity rides every sample") {
    DeviceBuilder b = stylus("xwayland-tablet eraser:13");
    Xi2Device dev = classified(b);
    EventBuilder eb;
    const XIDeviceEvent ev = eb.build();
    CHECK(xi2ParseEvent(dev, ev, 0).tool == TabletSample::Tool::Eraser);
}

// ---------------------------------------------------------------------------------------------
// Position: the DEVICE's valuators, not the server's pointer (the staircased-stroke fix)
// ---------------------------------------------------------------------------------------------
// Measured on XWayland/KWin (tools/xi2_valuator_probe): driven along a dead straight line, a pen
// reports through event_x with a 0.49 px RMS sawtooth -- flipping sign on 4 of every 5 samples and
// sometimes stepping BACKWARDS along the stroke -- while the same events' Abs X/Abs Y valuators are
// 43x more faithful (0.011 px RMS), exactly as clean as the same pen on native Wayland. event_x is
// the server's screen-mapped POINTER, and its fraction is an artefact of that mapping.

// A stylus whose declared range spans a 3840x1080 screen at exactly 10 device units per pixel, so
// every expected coordinate below is exact rather than an Approx of a division.
[[nodiscard]] DeviceBuilder screenSpanningStylus() {
    DeviceBuilder b;
    b.name = "xwayland-tablet stylus:11";
    b.axis(0, kAbsX, 0, 38400); // 3840 px * 10 units/px
    b.axis(1, kAbsY, 0, 10800); // 1080 px * 10 units/px
    b.axis(2, kAbsPressure, 0, 2048);
    return b;
}

constexpr mosaic::platform::Xi2Screen kScreen{3840.0, 1080.0};

// Settle the device's mapping verdict. It is STICKY and needs kXi2MapTrustSamples agreeing events
// before the valuator path engages (a stroke must not change position source halfway through), and
// a hovering pen streams continuously, so in life this is long settled before the tip touches down.
void warmUp(Xi2Device& dev) {
    for (int i = 0; i < mosaic::platform::kXi2MapTrustSamples; ++i) {
        EventBuilder eb;
        eb.axis(0, 12345).axis(1, 5432);
        eb.rootX = 1234.5;
        eb.rootY = 543.2;
        eb.x = 1234.5;
        eb.y = 543.2;
        const XIDeviceEvent ev = eb.build();
        (void)xi2ParseEvent(dev, ev, 0, kScreen);
    }
}

TEST_CASE("tablet x11: position comes from the valuators, not the server's wobbling pointer") {
    DeviceBuilder db = screenSpanningStylus();
    Xi2Device dev = classified(db);
    warmUp(dev);

    // The device says it is at screen (1234.5, 543.2) -- exactly. The server, reporting the same
    // instant, is off by a few tenths in each axis: that error is the bug.
    EventBuilder eb;
    eb.axis(0, 12345).axis(1, 5432).axis(2, 1024);
    eb.rootX = 1234.9; // the server's version of 1234.5
    eb.rootY = 542.8;  // ... and of 543.2
    eb.x = 834.9;      // the event window's origin is therefore (400, 300)
    eb.y = 242.8;
    const XIDeviceEvent ev = eb.build();

    const TabletSample s = xi2ParseEvent(dev, ev, 0, kScreen);
    // The DEVICE's position, in the window's coordinates: 1234.5 - 400, 543.2 - 300. Note it is NOT
    // event_x (834.9 / 242.8), which is what shipped and what wobbles.
    CHECK(s.pos.x == doctest::Approx(834.5));
    CHECK(s.pos.y == doctest::Approx(243.2));
    CHECK(s.pressure == doctest::Approx(0.5)); // and the rest of the parse is untouched
}

TEST_CASE("tablet x11: the valuator position is sub-pixel, and the server's rounding cannot reach it") {
    DeviceBuilder db = screenSpanningStylus();
    Xi2Device dev = classified(db);
    warmUp(dev);

    // Two consecutive samples one device unit apart -- a TENTH of a pixel. The server reports both
    // at the same rounded place; the valuators resolve them, which is the whole point of the axis.
    const auto sampleAt = [&](double units) {
        EventBuilder eb;
        eb.axis(0, units).axis(1, 5400);
        eb.rootX = 1234.0; // the server, unmoved and unhelpful
        eb.rootY = 540.0;
        eb.x = 1234.0;
        eb.y = 540.0;
        const XIDeviceEvent ev = eb.build();
        return xi2ParseEvent(dev, ev, 0, kScreen);
    };
    CHECK(sampleAt(12340).pos.x == doctest::Approx(1234.0));
    CHECK(sampleAt(12341).pos.x == doctest::Approx(1234.1)); // one device unit = 0.1 px, resolved
}

TEST_CASE("tablet x11: a device that does NOT span the screen keeps the server's position") {
    DeviceBuilder db = screenSpanningStylus();
    Xi2Device dev = classified(db);

    // The CTM case: the tablet is mapped to ONE output of several, so the device's range no longer
    // spans the screen and the valuator-derived position is flatly wrong -- it would put the stroke
    // on the other monitor. The server's own root_x is the tell, and the parse must not guess.
    EventBuilder eb;
    eb.axis(0, 12345).axis(1, 5432);
    eb.rootX = 617.0; // half of what the valuators imply: a tablet mapped to the left-hand output
    eb.rootY = 271.0;
    eb.x = 217.0;
    eb.y = 71.0;
    const XIDeviceEvent ev = eb.build();

    const TabletSample s = xi2ParseEvent(dev, ev, 0, kScreen);
    CHECK(s.pos.x == doctest::Approx(217.0)); // event_x, exactly as it shipped
    CHECK(s.pos.y == doctest::Approx(71.0));
}

TEST_CASE("tablet x11: with no screen, or no position axes, the position is the server's") {
    SUBCASE("no screen given -- the headless default") {
        DeviceBuilder db = screenSpanningStylus();
        Xi2Device dev = classified(db);
        EventBuilder eb;
        eb.axis(0, 12345).axis(1, 5432);
        eb.rootX = 1234.5;
        eb.rootY = 543.2;
        eb.x = 834.5;
        eb.y = 243.2;
        const XIDeviceEvent ev = eb.build();
        const TabletSample s = xi2ParseEvent(dev, ev, 0); // Xi2Screen{} -- path off
        CHECK(s.pos.x == doctest::Approx(834.5));
        CHECK(s.pos.y == doctest::Approx(243.2));
    }
    SUBCASE("a device with no Abs X / Abs Y at all") {
        DeviceBuilder b;
        b.name = "Screen digitizer stylus";
        b.axis(2, kAbsPressure, 0, 1024);
        Xi2Device dev = classified(b);
        EventBuilder eb;
        eb.x = 640.25;
        eb.y = 360.75;
        eb.rootX = 640.25;
        eb.rootY = 360.75;
        const XIDeviceEvent ev = eb.build();
        const TabletSample s = xi2ParseEvent(dev, ev, 0, kScreen);
        CHECK(s.pos.x == doctest::Approx(640.25));
        CHECK(s.pos.y == doctest::Approx(360.75));
    }
    SUBCASE("a degenerate declared range cannot be divided by") {
        DeviceBuilder b;
        b.name = "broken stylus";
        b.axis(0, kAbsX, 500, 500); // min == max
        b.axis(1, kAbsY, 0, 10800);
        b.axis(2, kAbsPressure, 0, 1024);
        Xi2Device dev = classified(b);
        EventBuilder eb;
        eb.axis(0, 500).axis(1, 5400);
        eb.x = 111.5;
        eb.y = 222.5;
        eb.rootX = 111.5;
        eb.rootY = 222.5;
        const XIDeviceEvent ev = eb.build();
        const TabletSample s = xi2ParseEvent(dev, ev, 0, kScreen);
        CHECK(s.pos.x == doctest::Approx(111.5));
        CHECK(s.pos.y == doctest::Approx(222.5));
    }
}

TEST_CASE("tablet x11: a motion event that omits the position axes reuses the running value") {
    DeviceBuilder db = screenSpanningStylus();
    Xi2Device dev = classified(db);
    warmUp(dev);

    // ⚠ The event window's origin is (400, 300) here, and that is load-bearing: it makes the
    // valuator answer (834.5) DIFFER from event_x (834.9). An earlier version of this test put the
    // window at the origin, so the two agreed -- and a mutant that never remembered the position
    // valuators at all still passed, because falling back to event_x happened to give the same
    // number. A test that cannot tell the two sources apart is not testing the source.
    EventBuilder first;
    first.axis(0, 12345).axis(1, 5432).axis(2, 1024);
    first.rootX = 1234.9;
    first.rootY = 542.8;
    first.x = 834.9;
    first.y = 242.8;
    const XIDeviceEvent e1 = first.build();
    CHECK(xi2ParseEvent(dev, e1, 0, kScreen).pos.x == doctest::Approx(834.5));

    // Pressure alone changed. XI2 sends only the axes that MOVED, so the position axes are absent
    // from this event -- and the parse must remember them, exactly as it already does for pressure
    // and tilt. Forgetting them would collapse the position to the device's minimum and (via the
    // guard, which would then see a wild disagreement) quietly drop back to the server's event_x.
    EventBuilder second;
    second.axis(2, 2048);
    second.rootX = 1234.9;
    second.rootY = 542.8;
    second.x = 834.9;
    second.y = 242.8;
    const XIDeviceEvent e2 = second.build();
    const TabletSample s = xi2ParseEvent(dev, e2, 0, kScreen);
    CHECK(s.pos.x == doctest::Approx(834.5)); // the remembered valuator, NOT event_x's 834.9
    CHECK(s.pos.y == doctest::Approx(243.2)); // ... nor event_y's 242.8
    CHECK(s.pressure == doctest::Approx(1.0));
}

TEST_CASE("tablet x11: the mapping verdict is sticky, so a stroke never changes position source") {
    DeviceBuilder db = screenSpanningStylus();
    Xi2Device dev = classified(db);

    // One agreeing event is NOT enough. Until the device has settled, position stays the server's --
    // which is what shipped, and is never worse than what shipped.
    const auto agreeingSample = [&] {
        EventBuilder eb;
        eb.axis(0, 12345).axis(1, 5432);
        eb.rootX = 1234.9; // the server, a fraction off the device's 1234.5 -- its usual noise
        eb.rootY = 542.8;
        eb.x = 834.9;
        eb.y = 242.8;
        const XIDeviceEvent ev = eb.build();
        return xi2ParseEvent(dev, ev, 0, kScreen);
    };
    CHECK(agreeingSample().pos.x == doctest::Approx(834.9)); // event_x: not yet trusted
    for (int i = 1; i < mosaic::platform::kXi2MapTrustSamples - 1; ++i)
        (void)agreeingSample();
    CHECK(agreeingSample().pos.x == doctest::Approx(834.5)); // settled: the DEVICE's position

    // ⚠ The bug this stickiness exists to prevent. The server's own position carries about a pixel
    // of noise, so a per-event check sized to that noise FLAPS -- and a stroke whose position source
    // changes sample to sample is worse than one that never had the valuators at all (measured: it
    // put backward steps into a stroke that only ever moved forward). Noise must not un-settle it.
    EventBuilder noisy;
    noisy.axis(0, 12345).axis(1, 5432);
    noisy.rootX = 1236.4; // ~2 px out -- comfortably inside the tolerance, and it must stay trusted
    noisy.rootY = 541.3;
    noisy.x = 836.4;
    noisy.y = 241.3;
    const XIDeviceEvent nev = noisy.build();
    const TabletSample s = xi2ParseEvent(dev, nev, 0, kScreen);
    CHECK(s.pos.x == doctest::Approx(1234.5 - (1236.4 - 836.4))); // still the device's
}

TEST_CASE("tablet x11: a tablet re-mapped to one output stops being trusted") {
    DeviceBuilder db = screenSpanningStylus();
    Xi2Device dev = classified(db);
    warmUp(dev);

    // The device was spanning the screen and is now mapped to a single output: the valuator position
    // is suddenly hundreds of pixels from where the server says the pen is. Painting there would be
    // far worse than the wobble, so the parse drops straight back to the server's position.
    EventBuilder eb;
    eb.axis(0, 12345).axis(1, 5432);
    eb.rootX = 617.0; // half of the 1234.5 the valuators imply
    eb.rootY = 271.0;
    eb.x = 217.0;
    eb.y = 71.0;
    const XIDeviceEvent ev = eb.build();
    const TabletSample s = xi2ParseEvent(dev, ev, 0, kScreen);
    CHECK(s.pos.x == doctest::Approx(217.0));
    CHECK(s.pos.y == doctest::Approx(71.0));
}

TEST_CASE("tablet x11: a FAST stroke does not unseat a device whose mapping is fine") {
    // ⚠ THE BUG THIS EXISTS TO PREVENT, and it shipped once. The guard compares the valuator-derived
    // screen position against the server's own root_x/root_y. Those two disagree for TWO completely
    // different reasons, and a fixed tolerance cannot tell them apart:
    //
    //   * a MAPPING error scales with POSITION (a tablet mapped to one output lands hundreds of px
    //     out) -- that is what the guard is for;
    //   * the server's pointer LAGS the valuators by about one sample of travel, which scales with
    //     SPEED -- and that is not an error at all.
    //
    // At ~8.4 px of travel per sample the lag reached 9 px, tripped a fixed 8 px bar, and kicked a
    // perfectly-mapped device back onto the server's wobbling pointer -- mid-stroke. The stroke then
    // MIXED the two sources, which is worse than either alone. Users saw it as a wobble that only
    // appeared on FAST strokes, and only on CURVES (a straight line never moves far enough per
    // sample to trip it). So the bar moves with the pen.
    DeviceBuilder db = screenSpanningStylus();
    Xi2Device dev = classified(db);
    warmUp(dev);
    REQUIRE(dev.mapTrusted);

    // The pen sweeps fast: 90 device units per event = 9 px of travel, with the server's pointer
    // lagging by exactly that. A fixed 8 px tolerance rejects this; the travel-scaled one must not.
    double val = 12345.0;
    for (int i = 0; i < 20; ++i) {
        const double prev = val;
        val += 90.0; // 9 px of travel per event
        EventBuilder eb;
        eb.axis(0, val).axis(1, 5432);
        // The server reports where the pen WAS one sample ago -- the lag, not a mapping error.
        eb.rootX = prev / 10.0; // 10 device units per px on this fixture
        eb.rootY = 543.2;
        eb.x = eb.rootX - 400.0;
        eb.y = eb.rootY - 300.0;
        const XIDeviceEvent ev = eb.build();
        const TabletSample s = xi2ParseEvent(dev, ev, 0, kScreen);

        CHECK_MESSAGE(dev.mapTrusted, "a fast stroke unseated a correctly-mapped device");
        // ... and the position keeps coming from the DEVICE, which is ahead of the server's lagging
        // pointer by exactly the travel.
        CHECK(s.pos.x == doctest::Approx(val / 10.0 - 400.0));
    }
}

TEST_CASE("tablet x11: speed does not excuse a MAPPING error, however fast the pen moves") {
    // The other side of the same coin: the tolerance grows with travel, so make sure that does not
    // hand a mis-mapped tablet a free pass. A device mapped to one output of several is out by
    // HUNDREDS of px -- vastly more than any travel -- so it must never be trusted, at any speed.
    DeviceBuilder db = screenSpanningStylus();
    Xi2Device dev = classified(db);

    double val = 12345.0;
    for (int i = 0; i < 30; ++i) {
        val += 90.0; // moving fast, so `travel` is large and the tolerance is generous
        EventBuilder eb;
        eb.axis(0, val).axis(1, 5432);
        eb.rootX = val / 20.0; // HALF of what the valuators imply: mapped to one output
        eb.rootY = 271.6;
        eb.x = eb.rootX - 400.0;
        eb.y = eb.rootY - 300.0;
        const XIDeviceEvent ev = eb.build();
        const TabletSample s = xi2ParseEvent(dev, ev, 0, kScreen);
        CHECK_MESSAGE(!dev.mapTrusted, "a mis-mapped tablet was trusted because it moved fast");
        CHECK(s.pos.x == doctest::Approx(eb.x)); // the server's position, exactly as it shipped
    }
}
