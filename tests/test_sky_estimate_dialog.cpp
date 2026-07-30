#include "ui/texture_generator_dialog.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <numbers>
#include <thread>
#include <variant>

#include "core/selection.hpp"
#include "core/texture/sky_camera.hpp"
#include "core/texture/texture_render.hpp"

// S55 "Estimate from layer" phase 2: the dialog wiring (headless, the test_texture_gen_dialog
// precedent -- building the dialog without show() is safe). A stub TextureGenHost feeds a
// synthetic sky rendered by our own renderer, so the flow assertions stay closed-loop and
// renderer-evolution-proof like the engine battery in test_sky_estimate.cpp.
namespace {

using namespace mosaic;
namespace texture = core::texture;

texture::SkyParams defaultSky() {
    return std::get<texture::SkyParams>(
        texture::defaultTextureParams(texture::Generator::Sky).spec);
}

std::uint32_t hash2(std::uint32_t x, std::uint32_t y) {
    std::uint32_t h = x * 374761393u + y * 668265263u + 0x9E3779B9u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// A known sky over a textured ground below its true horizon (the phase-1 fixture recipe).
common::Image skyFixture(const texture::SkyParams& s, std::uint32_t w, std::uint32_t h) {
    texture::TextureParams tp;
    tp.generator = texture::Generator::Sky;
    tp.seed = 42;
    tp.scale = 1.0;
    tp.spec = s;
    const texture::TextureRenderResult r = texture::renderTexture(tp, w, h);
    REQUIRE(r.imageF.has_value());
    common::Image img = common::toImage8(*r.imageF);
    const texture::SkyCamera cam = texture::SkyCamera::fromParams(s, w, h);
    double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
    REQUIRE(cam.project(texture::directionFromAzEl(168.0, 0.0), x1, y1));
    REQUIRE(cam.project(texture::directionFromAzEl(192.0, 0.0), x2, y2));
    const double m = (y2 - y1) / (x2 - x1);
    const double b = y1 - m * x1;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            if (y + 0.5 > m * (x + 0.5) + b) {
                const std::uint32_t hh = hash2(x, y);
                const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
                img.rgba[p] = static_cast<std::uint8_t>(55 + (hh % 55));
                img.rgba[p + 1] = static_cast<std::uint8_t>(45 + ((hh >> 8) % 45));
                img.rgba[p + 2] = static_cast<std::uint8_t>(20 + ((hh >> 16) % 25));
                img.rgba[p + 3] = 255;
            }
    return img;
}

// The stub host: a fixed source layer + commit counters the assertions read back.
struct StubState {
    bool available = true;
    std::string name = "Photo";
    common::Image image;
    std::optional<common::ExifData> exif;
    int commits = 0;
    int conformCommits = 0;
    texture::TextureParams lastParams;
    ui::TextureGenHost::ConformPayload lastConform;
};

ui::TextureGenHost stubHost(std::shared_ptr<StubState> st, bool withConform = true) {
    ui::TextureGenHost h;
    h.sourceLayer = [st]() -> std::optional<ui::TextureGenHost::SourceLayer> {
        if (!st->available || st->image.empty()) return std::nullopt;
        return ui::TextureGenHost::SourceLayer{st->name, st->image, st->exif};
    };
    h.commit = [st](texture::TextureParams params, texture::TextureRenderResult) {
        ++st->commits;
        st->lastParams = std::move(params);
    };
    if (withConform)
        h.commitConform = [st](texture::TextureParams params, texture::TextureRenderResult,
                               ui::TextureGenHost::ConformPayload conform) {
            ++st->conformCommits;
            st->lastParams = std::move(params);
            st->lastConform = std::move(conform);
        };
    return h;
}

const texture::SkyParams& skyOf(const ui::TextureGeneratorDialog& d) {
    return std::get<texture::SkyParams>(d.params().spec);
}

// Drive the dialog's poll loop until the estimate (or bake + conform) settles.
void pump(ui::TextureGeneratorDialog& d, int maxSeconds = 120) {
    const auto t0 = std::chrono::steady_clock::now();
    while (d.estimating() || d.baking() || d.conforming()) {
        d.pollOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(maxSeconds)) break;
    }
}

} // namespace

TEST_CASE("estimate dialog: gating -- no source, wrong generator, and busy fences") {
    auto st = std::make_shared<StubState>();
    st->available = false;
    ui::TextureGeneratorDialog dlg(stubHost(st));
    // The hover bubble is a pre-show child sub-window (the ui::Popover rule): constructed with
    // the dialog, never yet mapped.
    REQUIRE(dlg.estimateBubbleForTest() != nullptr);
    CHECK_FALSE(dlg.estimateBubbleForTest()->visible());
    dlg.seed(320, 214, std::nullopt);

    // No source: the action row is present but DISABLED (the wand's copy-family hint rides its
    // tooltip), and the programmatic entry no-ops.
    REQUIRE(dlg.estimateButtonForTest() != nullptr);
    CHECK_FALSE(dlg.estimateButtonForTest()->active());
    dlg.estimateFromLayer();
    CHECK_FALSE(dlg.estimating());
    CHECK_FALSE(dlg.estimateForTest().has_value());

    // A source appears mid-session -- but availability was probed at seed() (the dialog is
    // modal; the layer under it cannot change), so a re-seed is what re-probes.
    st->available = true;
    st->image = skyFixture(defaultSky(), 320, 214);
    dlg.seed(320, 214, std::nullopt);
    REQUIRE(dlg.estimateButtonForTest() != nullptr);
    CHECK(dlg.estimateButtonForTest()->active());

    // Wrong generator: the estimate is the sky's business -- no action row off the sky stack.
    dlg.selectGenerator(texture::Generator::Paper);
    CHECK(dlg.estimateButtonForTest() == nullptr);
    dlg.estimateFromLayer();
    CHECK_FALSE(dlg.estimating());
    dlg.selectGenerator(texture::Generator::Sky);

    // The estimate fences the bake and vice versa.
    dlg.estimateFromLayer();
    REQUIRE(dlg.estimating());
    dlg.create();
    CHECK_FALSE(dlg.baking()); // fenced: the estimate owns the params
    pump(dlg);
    CHECK_FALSE(dlg.estimating());
}

TEST_CASE("estimate dialog: end-to-end flow -- applyEdit lands, summary fills, revert restores") {
    texture::SkyParams truth = defaultSky();
    truth.pitchDeg = 12.0;
    truth.cloudCoverage = 0.0;
    truth.sunElevationDeg = 25.0;
    truth.sunAzimuthDeg = 192.0;

    auto st = std::make_shared<StubState>();
    st->image = skyFixture(truth, 480, 320);
    ui::TextureGeneratorDialog dlg(stubHost(st));
    dlg.seed(480, 320, std::nullopt);
    const double pitchBefore = skyOf(dlg).pitchDeg; // the default camera (18): NOT the truth

    dlg.estimateFromLayer();
    REQUIRE(dlg.estimating());
    pump(dlg);
    REQUIRE_FALSE(dlg.estimating());
    REQUIRE(dlg.estimateForTest().has_value());
    REQUIRE_FALSE(dlg.estimateForTest()->aborted);

    // The estimate landed as one applyEdit on the working sky arm.
    CHECK(std::abs(skyOf(dlg).pitchDeg - truth.pitchDeg) < 1.5);
    CHECK(std::abs(skyOf(dlg).sunElevationDeg - truth.sunElevationDeg) < 2.5);
    CHECK(std::abs(skyOf(dlg).sunAzimuthDeg - truth.sunAzimuthDeg) < 2.5);
    CHECK(skyOf(dlg).shiftY == 0.0);

    // The summary block: per-quantity lines + the engine's honesty lines.
    const std::vector<std::string> lines = dlg.estimateSummaryForTest();
    REQUIRE_FALSE(lines.empty());
    bool horizonLine = false, fovLine = false;
    for (const std::string& l : lines) {
        if (l.find("Horizon:") != std::string::npos) horizonLine = true;
        if (l.find("Field of view unchanged") != std::string::npos) fovLine = true;
    }
    CHECK(horizonLine);
    CHECK(fovLine); // no EXIF on this source -> the honesty line, not the metadata credit

    // Revert restores the snapshot and clears the estimate record.
    dlg.revertEstimate();
    CHECK(skyOf(dlg).pitchDeg == doctest::Approx(pitchBefore));
    CHECK_FALSE(dlg.estimateForTest().has_value());
    CHECK(dlg.estimateSummaryForTest().empty());
}

TEST_CASE("estimate dialog: EXIF fov + date/place ride the estimate with metadata credits") {
    // The fixture is rendered AT the lens's claimed FOV, so the geometry is self-consistent.
    const double f35 = 24.0;
    const double fovDeg = 2.0 * std::atan(18.0 / f35) * 180.0 / std::numbers::pi;
    texture::SkyParams truth = defaultSky();
    truth.fovDeg = fovDeg;
    truth.pitchDeg = 12.0;
    truth.cloudCoverage = 0.0;
    truth.sunElevationDeg = 25.0;
    truth.sunAzimuthDeg = 188.0;

    auto st = std::make_shared<StubState>();
    st->image = skyFixture(truth, 480, 320);
    common::ExifData exif;
    exif.focalLength35mm = static_cast<int>(f35);
    exif.dateTimeOriginal = common::ExifDateTime{2026, 6, 21, 10, 30, 0};
    exif.gpsLatitude = 51.5;
    exif.gpsLongitude = -0.13;
    st->exif = exif;

    ui::TextureGeneratorDialog dlg(stubHost(st));
    dlg.seed(480, 320, std::nullopt);
    CHECK(dlg.moonSource() == 0); // nothing latched yet
    const ui::SkyClockState preClock = dlg.skyClockForTest(); // the session's own observer

    dlg.estimateFromLayer();
    pump(dlg);
    REQUIRE(dlg.estimateForTest().has_value());
    const texture::SkyEstimateResult& est = *dlg.estimateForTest();
    REQUIRE_FALSE(est.aborted);

    // FOV is a measurement: applied at confidence 1.0, credited in the summary.
    REQUIRE(est.fov.applied);
    CHECK(est.fov.confidence == 1.0);
    CHECK(skyOf(dlg).fovDeg == doctest::Approx(fovDeg).epsilon(0.01));
    // ...and the horizon inversion ran at the REAL fov, so the pitch still lands.
    CHECK(std::abs(skyOf(dlg).pitchDeg - truth.pitchDeg) < 1.5);

    // Date & place prefilled the observer and latched the moon source (first-set-wins).
    CHECK(skyOf(dlg).obsYear == 2026);
    CHECK(skyOf(dlg).obsMonth == 6);
    CHECK(skyOf(dlg).obsDay == 21);
    CHECK(skyOf(dlg).obsLatitudeDeg == doctest::Approx(51.5));
    CHECK(skyOf(dlg).obsLongitudeDeg == doctest::Approx(-0.13));
    CHECK(dlg.moonSource() == 1);

    // With date & place live, the sun landed as CLOCK TIME: the almanac inverted the measured
    // elevation on the photo's date; the EXIF wall time (10:30 local) picks the MORNING crossing.
    REQUIRE(est.timeUtc.applied);
    CHECK(skyOf(dlg).obsHourUtc < 12.0);

    bool fovCredit = false, metaCredit = false, timeLine = false;
    for (const std::string& l : dlg.estimateSummaryForTest()) {
        if (l.find("lens metadata") != std::string::npos) fovCredit = true;
        if (l.find("photo's metadata") != std::string::npos) metaCredit = true;
        if (l.find("Time:") != std::string::npos) timeLine = true;
    }
    CHECK(fovCredit);
    CHECK(metaCredit);
    CHECK(timeLine);

    // The morning/afternoon swap: one click flips to the other crossing (and back).
    const double morning = skyOf(dlg).obsHourUtc;
    dlg.swapEstimateTime();
    CHECK(skyOf(dlg).obsHourUtc > 12.0);
    dlg.swapEstimateTime();
    CHECK(skyOf(dlg).obsHourUtc == doctest::Approx(morning).epsilon(0.01));

    // Revert un-does the metadata prefill too: the observer clock returns to the session's own
    // pre-estimate state (asserted through the info panel's clock, the observer's test surface).
    dlg.revertEstimate();
    CHECK(skyOf(dlg).fovDeg == doctest::Approx(62.0)); // the default camera is back
    const ui::SkyClockState postClock = dlg.skyClockForTest();
    CHECK(postClock.year == preClock.year);
    CHECK(postClock.month == preClock.month);
    CHECK(postClock.day == preClock.day);
    CHECK(postClock.localHours == doctest::Approx(preClock.localHours).epsilon(1e-9));
}

TEST_CASE("estimate dialog: the mask & harmonize toggle gates, never pre-arms, and commits") {
    texture::SkyParams truth = defaultSky();
    truth.cloudCoverage = 0.0;
    auto st = std::make_shared<StubState>();
    st->image = skyFixture(truth, 480, 320);
    ui::TextureGeneratorDialog dlg(stubHost(st));
    dlg.seed(480, 320, std::nullopt);

    // Not offered before an estimate; arming it is refused.
    CHECK_FALSE(dlg.conformOffered());
    dlg.setConformWanted(true);
    CHECK_FALSE(dlg.conformWanted());

    dlg.estimateFromLayer();
    pump(dlg);
    REQUIRE(dlg.estimateForTest().has_value());
    REQUIRE(dlg.estimateForTest()->segmentationUsable);
    CHECK(dlg.conformOffered());
    dlg.setConformWanted(true);
    CHECK(dlg.conformWanted());

    // A fresh session NEVER inherits the armed toggle (no silent auto-chain next time).
    dlg.seed(480, 320, std::nullopt);
    CHECK_FALSE(dlg.conformWanted());
    CHECK_FALSE(dlg.conformOffered());

    // Estimate again, arm, ACCEPT: bake -> S6 mask -> S7 grade -> ONE conform commit.
    dlg.estimateFromLayer();
    pump(dlg);
    REQUIRE(dlg.estimateForTest().has_value());
    dlg.setConformWanted(true);
    REQUIRE(dlg.conformWanted());
    dlg.create();
    REQUIRE(dlg.baking());
    pump(dlg);
    CHECK_FALSE(dlg.baking());
    CHECK_FALSE(dlg.conforming());
    CHECK(st->conformCommits == 1);
    CHECK(st->commits == 0); // the conform path committed INSTEAD of the plain one

    // The payload: a doc-space sky selection covering the sky half, plus the scalar bag.
    const core::Selection& sel = st->lastConform.skySelection;
    REQUIRE_FALSE(sel.isEmpty());
    CHECK(sel.at(240, 10) >= core::kAntsCoverageThreshold);   // sky
    CHECK(sel.at(240, 316) < core::kAntsCoverageThreshold);   // ground
    CHECK(st->lastConform.matchParams.count("gain_r") == 1);
    CHECK(st->lastConform.matchParams.count("delta_ev") == 1);
    CHECK(std::holds_alternative<texture::SkyParams>(st->lastParams.spec));
}

TEST_CASE("estimate dialog: ACCEPT without the toggle commits plain even when offered") {
    texture::SkyParams truth = defaultSky();
    truth.cloudCoverage = 0.0;
    auto st = std::make_shared<StubState>();
    st->image = skyFixture(truth, 480, 320);
    ui::TextureGeneratorDialog dlg(stubHost(st));
    dlg.seed(480, 320, std::nullopt);

    dlg.estimateFromLayer();
    pump(dlg);
    REQUIRE(dlg.estimateForTest().has_value());
    CHECK(dlg.conformOffered());
    // The user never armed the toggle: Create stays the plain one-layer commit (the
    // conform shape is an explicit, per-session opt-in).
    dlg.create();
    pump(dlg);
    CHECK(st->commits == 1);
    CHECK(st->conformCommits == 0);
}
