#include "ui/texture_generator_dialog.hpp"

#include "core/texture/paper_render.hpp"
#include "core/texture/sky_camera.hpp"
#include "core/texture/sky_render.hpp"
#include "core/texture/solar.hpp"
#include "ui/texture_gizmo_math.hpp"
#include "ui/widgets.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <thread>
#include <variant>

// S55-f: the Texture Generator modal (headless, the test_type3d_panel precedent -- building the
// dialog without show() is safe) + the pure gizmo screen<->parameter mappings it drives.
namespace {

using namespace mosaic;
namespace texture = core::texture;
namespace texgizmo = ui::texgizmo;

ui::TextureGenHost nullHost() {
    return ui::TextureGenHost{};
}

} // namespace

TEST_CASE("gizmo math: the sun projects where the camera says, and round-trips") {
    texture::SkyParams s; // az 180 el 28: dead-centre column, in frame at the default camera
    const double w = 400, h = 300;
    const auto p = texgizmo::skySunScreen(s, w, h);
    REQUIRE(p.has_value());
    CHECK(p->x == doctest::Approx(w / 2).epsilon(0.01));
    CHECK(p->y > 0.0);
    CHECK(p->y < h);
    // Round-trip: aiming the sun at its own screen position must not move it.
    texture::SkyParams s2 = s;
    texgizmo::skySunFromScreen(s2, w, h, p->x, p->y);
    CHECK(s2.sunAzimuthDeg == doctest::Approx(180.0).epsilon(0.01));
    CHECK(s2.sunElevationDeg == doctest::Approx(28.0).epsilon(0.01));
    // Behind the camera (azimuth 0 = due north, the camera faces south): no in-frame position.
    texture::SkyParams behind = s;
    behind.sunAzimuthDeg = 0.0;
    CHECK_FALSE(texgizmo::skySunScreen(behind, w, h).has_value());
}

TEST_CASE("gizmo math: the horizon row solves rayAt(..).z == 0 and pitch round-trips") {
    const double w = 400, h = 300;
    SUBCASE("sky, roll and tilt-shift included") {
        texture::SkyParams s;
        s.rollDeg = 8.0;
        s.shiftY = 0.1;
        const double row = texgizmo::skyHorizonRowAt(s, w, h, w / 2.0);
        const auto cam = texture::SkyCamera::fromParams(s, 400, 300);
        CHECK(std::abs(cam.rayAt(w / 2.0, row).z) < 1e-9);
        // Drag round-trip: ask for the horizon at some row, then recompute where it sits.
        texture::SkyParams s2 = s;
        s2.pitchDeg = texgizmo::skyPitchForHorizonRow(s2, w, h, 90.0);
        CHECK(texgizmo::skyHorizonRowAt(s2, w, h, w / 2.0) == doctest::Approx(90.0).epsilon(0.01));
    }
    SUBCASE("grass") {
        texture::GrassParams g;
        const double row = texgizmo::grassHorizonRow(g, w, h);
        CHECK(row > 0.0);
        CHECK(row < h / 2.0); // pitched down: the horizon sits in the upper half
        texture::GrassParams g2 = g;
        g2.pitchDeg = texgizmo::grassPitchForHorizonRow(g2, w, h, 60.0);
        CHECK(texgizmo::grassHorizonRow(g2, w, h) == doctest::Approx(60.0).epsilon(0.01));
    }
}

TEST_CASE("gizmo math: dome and compass insets round-trip") {
    const double cx = 50, cy = 60, R = 24;
    for (const double ref : {0.0, 180.0}) {
        for (const double az : {10.0, 135.0, 300.0}) {
            const auto d = texgizmo::domeDot(cx, cy, R, az, 35.0, ref);
            double az2 = 0.0, el2 = 0.0;
            texgizmo::domeFromPoint(cx, cy, R, d.x, d.y, ref, az2, el2);
            CHECK(az2 == doctest::Approx(az).epsilon(0.001));
            CHECK(el2 == doctest::Approx(35.0).epsilon(0.001));
            const auto c = texgizmo::compassDot(cx, cy, R, az, 0.6, ref);
            double dir = 0.0, str = 0.0;
            texgizmo::compassFromPoint(cx, cy, R, c.x, c.y, ref, dir, str);
            CHECK(dir == doctest::Approx(az).epsilon(0.001));
            CHECK(str == doctest::Approx(0.6).epsilon(0.001));
        }
    }
}

TEST_CASE("texturePresetIndex: defaults read Custom; a preset spec reads its own name") {
    texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Paper);
    CHECK(ui::texturePresetIndex(p) == 0); // the default spec matches no preset
    p.spec = texture::paperPreset(2).params;
    CHECK(ui::texturePresetIndex(p) == 3); // 1-based past "Custom"
    p.seed = 999;                          // seed/scale are the user's own -- still the preset
    p.scale = 3.0;
    CHECK(ui::texturePresetIndex(p) == 3);
    texture::TextureParams sky = texture::defaultTextureParams(texture::Generator::Sky);
    sky.spec = texture::skyPreset(0).params;
    CHECK(ui::texturePresetIndex(sky) == 1);
}

TEST_CASE("texture dialog: rail switching keeps per-generator edits; randomize touches only seed") {
    ui::TextureGeneratorDialog dlg(nullHost());
    dlg.seed(64, 48, std::nullopt); // tiny doc: the background proxies stay cheap
    CHECK(dlg.params().generator == texture::Generator::Sky);
    CHECK_FALSE(dlg.baking());

    // Apply a sky preset, flip to Paper and back: the sky edit survives (§7.2 per-arm state).
    dlg.applyPreset(2); // Golden hour
    CHECK(std::get<texture::SkyParams>(dlg.params().spec) == texture::skyPreset(2).params);
    dlg.selectGenerator(texture::Generator::Paper);
    CHECK(dlg.params().generator == texture::Generator::Paper);
    dlg.selectGenerator(texture::Generator::Sky);
    CHECK(std::get<texture::SkyParams>(dlg.params().spec) == texture::skyPreset(2).params);

    const auto specBefore = dlg.params().spec;
    const auto seedBefore = dlg.params().seed;
    dlg.randomizeSeed();
    CHECK(dlg.params().seed != seedBefore);
    CHECK(dlg.params().spec == specBefore);
}

TEST_CASE("texture dialog: every registry generator builds its stack and serves its presets") {
    // S55-g: the rail is registry-driven -- flipping to EVERY generator must build a control
    // stack headlessly, and each generator's preset library must apply through the dialog and
    // read back by name position (the dropdown is fed from the same traits).
    ui::TextureGeneratorDialog dlg(nullHost());
    dlg.seed(64, 48, std::nullopt);
    for (int gi = 0; gi < texture::kGeneratorCount; ++gi) {
        const auto g = static_cast<texture::Generator>(gi);
        CAPTURE(texture::generatorName(g));
        dlg.selectGenerator(g);
        CHECK(dlg.params().generator == g);
        const texture::GeneratorTraits& traits = texture::generatorTraits(g);
        REQUIRE(traits.presetCount() > 0);
        dlg.applyPreset(traits.presetCount() - 1);  // the last library entry applies cleanly
        CHECK(ui::texturePresetIndex(dlg.params()) == static_cast<int>(traits.presetCount()));
    }
    // Per-arm working state survives the grand tour: the sky arm is still the sky's.
    dlg.selectGenerator(texture::Generator::Sky);
    CHECK(dlg.params().generator == texture::Generator::Sky);
}

TEST_CASE("texture dialog: edit mode seeds from a material layer's params") {
    // The §3.3 select-to-edit path for an S55-g arm: params seed the dialog whole.
    ui::TextureGeneratorDialog dlg(nullHost());
    texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Marble);
    p.seed = 123;
    std::get<texture::MarbleParams>(p.spec).veinSpacing = 150.0;
    dlg.seed(64, 48, std::make_pair(std::string("Counter top"), p));
    CHECK(dlg.params().generator == texture::Generator::Marble);
    CHECK(dlg.params().seed == 123);
    CHECK(std::get<texture::MarbleParams>(dlg.params().spec).veinSpacing ==
          doctest::Approx(150.0));
}

TEST_CASE("texture dialog: edit mode seeds from the layer's params") {
    ui::TextureGeneratorDialog dlg(nullHost());
    texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Grass);
    p.seed = 77;
    p.scale = 1.5;
    std::get<texture::GrassParams>(p.spec).density = 0.33;
    dlg.seed(64, 48, std::make_pair(std::string("My lawn"), p));
    CHECK(dlg.params().generator == texture::Generator::Grass);
    CHECK(dlg.params().seed == 77);
    CHECK(std::get<texture::GrassParams>(dlg.params().spec).density ==
          doctest::Approx(0.33));
}

TEST_CASE("TimeDrum: values snap to the 5-minute grid and wrap like a clock") {
    ui::TimeDrum drum(0, 0, 96, 54);
    drum.setValue(21.75);  // 21:45 sits ON the grid
    CHECK(drum.value() == doctest::Approx(21.75));
    drum.setValue(13.51);  // 13:30.6 -> snaps to 13:30
    CHECK(drum.value() == doctest::Approx(13.5));
    drum.setValue(23.999);  // rounds to 24:00 -> wraps to midnight
    CHECK(drum.value() == doctest::Approx(0.0));
    drum.setValue(-0.5);  // negative wraps backward
    CHECK(drum.value() == doctest::Approx(23.5));
}

TEST_CASE("NumberField helpers: locale-independent format and parse") {
    CHECK(ui::formatFieldNumber(3.0, 1.0) == "3");
    CHECK(ui::formatFieldNumber(3.5, 0.1) == "3.5");
    double v = 0.0;
    CHECK(ui::parseFieldNumber("2,5", v));
    CHECK(v == doctest::Approx(2.5));
    CHECK(ui::parseFieldNumber("+7", v));
    CHECK(v == doctest::Approx(7.0));
    CHECK_FALSE(ui::parseFieldNumber("abc", v));
    CHECK_FALSE(ui::parseFieldNumber(nullptr, v));
}

TEST_CASE("gizmo math: the moon projects and round-trips like the sun") {
    texture::SkyParams s;
    s.enableMoon = true;
    s.moonAzimuthDeg = 200.0;
    s.moonElevationDeg = 30.0;
    const double w = 400, h = 300;
    const auto p = texgizmo::skyAzElScreen(s, s.moonAzimuthDeg, s.moonElevationDeg, w, h);
    REQUIRE(p.has_value());
    double az = 0.0, el = 0.0;
    texgizmo::skyAzElFromScreen(s, w, h, p->x, p->y, az, el);
    CHECK(az == doctest::Approx(200.0).epsilon(0.01));
    CHECK(el == doctest::Approx(30.0).epsilon(0.01));
}

TEST_CASE("preview framing: fit zoom fills the pane, zoomed-in windows and clamps the pan") {
    // Fit never upscales past 1:1: a big document scales down, a tiny one stays 1:1.
    CHECK(texgizmo::previewFitZoom(1000, 500, 400, 400) == doctest::Approx(0.4)); // width-limited
    CHECK(texgizmo::previewFitZoom(64, 48, 440, 370) == doctest::Approx(1.0));    // capped at 1:1

    // Fit: the frame IS the whole document scaled down, no window.
    const auto fitV = texgizmo::previewView(1000, 500, 400, 400, 0.4, 0.0, 0.0);
    CHECK(fitV.frameW == 400);
    CHECK(fitV.frameH == 200);
    CHECK(fitV.viewW == 400);
    CHECK(fitV.viewH == 200);
    CHECK(fitV.winX == 0);
    CHECK(fitV.winY == 0);

    // Zoomed to 1:1: the frame is the full doc, the pane windows it, the pan lands in frame px.
    const auto oneV = texgizmo::previewView(1000, 500, 400, 400, 1.0, 300.0, 50.0);
    CHECK(oneV.frameW == 1000);
    CHECK(oneV.frameH == 500);
    CHECK(oneV.viewW == 400);
    CHECK(oneV.viewH == 400); // 400 <= 500, so the whole pane height shows document
    CHECK(oneV.winX == 300);
    CHECK(oneV.winY == 50);

    // Pan past the edge clamps to keep the window inside the frame.
    const auto clamped = texgizmo::previewView(1000, 500, 400, 400, 1.0, 5000.0, 5000.0);
    CHECK(clamped.winX == 600); // 1000 - 400
    CHECK(clamped.winY == 100); // 500 - 400
    CHECK(texgizmo::previewMaxPanDoc(1000, 400, 1.0) == doctest::Approx(600.0));
    CHECK(texgizmo::previewMaxPanDoc(1000, 400, 0.4) == doctest::Approx(0.0)); // fit: nothing to pan
}

TEST_CASE("texture dialog: the moon-phase source latches first-set-wins") {
    SUBCASE("touching date & place first latches ephemeris") {
        ui::TextureGeneratorDialog dlg(nullHost());
        dlg.seed(64, 48, std::nullopt);
        CHECK(dlg.moonSource() == 0); // unset
        dlg.setObserver(2026, 3, 20, 12.0, 48.85, 2.35);
        CHECK(dlg.moonSource() == 1); // date & place won
        const auto& s = std::get<texture::SkyParams>(dlg.params().spec);
        CHECK(s.moonPhaseMode == 2); // ephemeris
        CHECK(s.enableMoon);
    }
    SUBCASE("choosing manual first, then editing date & place keeps manual for the phase") {
        ui::TextureGeneratorDialog dlg(nullHost());
        dlg.seed(64, 48, std::nullopt);
        dlg.selectMoonSource(2); // manual
        CHECK(dlg.moonSource() == 2);
        CHECK(std::get<texture::SkyParams>(dlg.params().spec).moonPhaseMode == 1);
        // Editing the observer now still moves the SUN (calculator), but the phase stays manual.
        dlg.setObserver(2026, 3, 20, 12.0, 48.85, 2.35);
        CHECK(dlg.moonSource() == 2);
        CHECK(std::get<texture::SkyParams>(dlg.params().spec).moonPhaseMode == 1);
    }
    SUBCASE("an explicit source pick overrides an earlier latch") {
        ui::TextureGeneratorDialog dlg(nullHost());
        dlg.seed(64, 48, std::nullopt);
        dlg.setObserver(2026, 3, 20, 12.0, 48.85, 2.35); // latches ephemeris
        CHECK(dlg.moonSource() == 1);
        dlg.selectMoonSource(2); // explicit switch to manual
        CHECK(dlg.moonSource() == 2);
        CHECK(std::get<texture::SkyParams>(dlg.params().spec).moonPhaseMode == 1);
    }
}

TEST_CASE("texture dialog: the info panel's clock follows the observer's local date & time") {
    ui::TextureGeneratorDialog dlg(nullHost());
    dlg.seed(64, 48, std::nullopt); // Sky is the default generator: the clock is showing

    // Paris, noon UTC: local (mean solar) time = UTC + lon/15, same calendar day.
    dlg.setObserver(2026, 3, 20, 12.0, 48.85, 2.35);
    ui::SkyClockState c = dlg.skyClockForTest();
    CHECK(c.visible);
    CHECK(c.year == 2026);
    CHECK(c.month == 3);
    CHECK(c.day == 20);
    CHECK(c.localHours == doctest::Approx(12.0 + 2.35 / 15.0));
    CHECK(c.sunElevationDeg > 0.0); // equinox noon: the sun is up (the DAY face tint)

    // The calendar carries across midnight: 23:00 UTC in Tokyo (+139.69 E) is the NEXT local day
    // -- across the year boundary here.
    dlg.setObserver(2026, 12, 31, 23.0, 35.68, 139.69);
    c = dlg.skyClockForTest();
    CHECK(c.year == 2027);
    CHECK(c.month == 1);
    CHECK(c.day == 1);
    CHECK(c.localHours == doctest::Approx(23.0 + 139.69 / 15.0 - 24.0));

    // ... and backward: 01:00 UTC in Honolulu (-157.86 E) is still the previous local afternoon
    // (across a non-leap February boundary here).
    dlg.setObserver(2026, 3, 1, 1.0, 21.31, -157.86);
    c = dlg.skyClockForTest();
    CHECK(c.year == 2026);
    CHECK(c.month == 2);
    CHECK(c.day == 28);
    CHECK(c.localHours == doctest::Approx(1.0 - 157.86 / 15.0 + 24.0));

    // No sky panel, no clock; flipping back brings it back (updateSkyInfo runs on the switch).
    dlg.selectGenerator(texture::Generator::Paper);
    CHECK_FALSE(dlg.skyClockForTest().visible);
    dlg.selectGenerator(texture::Generator::Sky);
    CHECK(dlg.skyClockForTest().visible);
}

TEST_CASE("texture dialog: opening the date & place section re-parents the picker cleanly") {
    // The persistent DatePicker owns a pop-up sub-window, so it is re-parented into the scrolled
    // control stack when this section opens and detached before every rebuild -- exercise both the
    // reparent (open) and the park-before-clear (rebuild while open, then a preset rebuild).
    ui::TextureGeneratorDialog dlg(nullHost());
    dlg.seed(64, 48, std::nullopt);
    dlg.openSectionForTest("sky:solar"); // builds the DatePicker + City rows
    dlg.openSectionForTest("sky:night"); // rebuilds again (parks + re-adds the picker)
    dlg.applyPreset(0);                  // a sky preset rebuild with the section still open
    // A moon preset (manual phase) then flipping to Paper and back must not crash the reparent.
    dlg.selectGenerator(texture::Generator::Paper);
    dlg.selectGenerator(texture::Generator::Sky);
    CHECK(dlg.params().generator == texture::Generator::Sky);
}

TEST_CASE("texture dialog: observer lat/lon drives the sun (manual moon keeps the calculator)") {
    ui::TextureGeneratorDialog dlg(nullHost());
    dlg.seed(64, 48, std::nullopt);
    dlg.selectMoonSource(2); // manual: the observer controls drive the sun via the calculator
    dlg.setObserver(2026, 6, 21, 12.0, 0.0, 0.0);
    dlg.setObserverLatLon(35.68, 139.69); // Tokyo
    const texture::SunPosition sun =
        texture::sunPosition(texture::UtcTime{2026, 6, 21, 12.0}, 35.68, 139.69);
    const auto& s = std::get<texture::SkyParams>(dlg.params().spec);
    CHECK(s.sunAzimuthDeg == doctest::Approx(sun.azimuthDeg).epsilon(0.001));
    CHECK(s.sunElevationDeg ==
          doctest::Approx(std::clamp(sun.elevationDeg, -30.0, 90.0)).epsilon(0.001));
}

TEST_CASE("texture dialog: Create bakes full resolution and commits params + pixels") {
    texture::TextureParams committedParams;
    texture::TextureRenderResult committedBake;
    int commits = 0;
    ui::TextureGenHost host;
    host.commit = [&](texture::TextureParams p, texture::TextureRenderResult baked) {
        committedParams = std::move(p);
        committedBake = std::move(baked);
        ++commits;
    };
    ui::TextureGeneratorDialog dlg(std::move(host));
    dlg.seed(48, 32, std::nullopt);
    dlg.selectGenerator(texture::Generator::Paper); // 8-bit lane, quick at 48x32
    dlg.create();
    CHECK(dlg.baking());
    // Drive the poll loop by hand (no FLTK event loop headlessly); bounded, never hangs.
    for (int i = 0; i < 4000 && commits == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        dlg.pollOnce();
    }
    REQUIRE(commits == 1);
    CHECK_FALSE(dlg.baking());
    CHECK(committedParams.generator == texture::Generator::Paper);
    REQUIRE(committedBake.image8.has_value());
    CHECK(committedBake.image8->width == 48);
    CHECK(committedBake.image8->height == 32);
}
