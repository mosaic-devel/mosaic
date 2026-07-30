#include "core/texture/sky_estimate.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <thread>

#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/selection.hpp"
#include "core/texture/sky_camera.hpp"
#include "core/texture/sky_estimate_commit.hpp"
#include "core/texture/sky_estimate_worker.hpp"
#include "core/texture/texture_render.hpp"
#include "io/mosaic/docio.hpp"
#include "io/mosaic/file.hpp"
#include "render/compositor.hpp"

// S55 "Estimate from layer" (docs/research-sky-estimate-from-layer.md): the CLOSED-LOOP battery.
// Every fixture is rendered by our own renderTexture within the test run and fed back to the
// estimator (analysis-by-synthesis against the same forward model the probes use), so these
// tests are renderer-evolution-proof: no pixel value or hash of today's renderer output is
// pinned anywhere -- only physically-motivated recovery tolerances. Plus: the confidence gates'
// degrade-to-no-change contract, the S6 segmentation IoU + holes policy, the S7 harmonization
// scalars, the PhotometricMatch compositor math + docio round-trip, the worker's coalescing,
// and the S5 almanac inversion policy.
namespace {

using namespace mosaic;
namespace texture = core::texture;

constexpr double kRadToDeg = 180.0 / std::numbers::pi;

texture::SkyParams defaultSky() {
    return std::get<texture::SkyParams>(
        texture::defaultTextureParams(texture::Generator::Sky).spec);
}

// The TRUE horizon line of a parameter set, straight from the camera (the same construction the
// estimator's Gauss-Newton refine uses; also the roll-sign pinning device).
struct Line {
    double y0, yW;  // y at x = 0 and x = W
    double yAt(double x, double w) const { return y0 + (yW - y0) * (x / w); }
};

Line trueHorizon(const texture::SkyParams& s, std::uint32_t w, std::uint32_t h) {
    const texture::SkyCamera cam = texture::SkyCamera::fromParams(s, w, h);
    double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
    REQUIRE(cam.project(texture::directionFromAzEl(168.0, 0.0), x1, y1));
    REQUIRE(cam.project(texture::directionFromAzEl(192.0, 0.0), x2, y2));
    const double m = (y2 - y1) / (x2 - x1);
    const double b = y1 - m * x1;
    return {b, m * w + b};
}

std::uint32_t hash2(std::uint32_t x, std::uint32_t y) {
    std::uint32_t h = x * 374761393u + y * 668265263u + 0x9E3779B9u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// A deterministic textured "ground": dark browns/greens with real high-frequency detail, so the
// sky/ground boundary carries honest edge + texture + color evidence (and the sanity gates see
// a genuinely non-sky lower class).
common::Color8 groundColor(std::uint32_t x, std::uint32_t y) {
    const std::uint32_t h = hash2(x, y);
    return {static_cast<std::uint8_t>(55 + (h % 55)),
            static_cast<std::uint8_t>(45 + ((h >> 8) % 45)),
            static_cast<std::uint8_t>(20 + ((h >> 16) % 25)), 255};
}

// Render a sky with our own renderer and composite the textured ground below the true horizon.
// `groundDim` scales the ground's brightness: 1.0 is a daylight lawn/soil, ~0.25 the dark
// silhouette a real dusk landscape shows (a twilight sky over a ground BRIGHTER than itself is
// a documented degrade-to-unchanged case, not a recovery claim).
common::Image skyFixture(const texture::SkyParams& s, std::uint32_t w, std::uint32_t h,
                         bool ground = true, double groundDim = 1.0) {
    texture::TextureParams tp;
    tp.generator = texture::Generator::Sky;
    tp.seed = 42;
    tp.scale = 1.0;
    tp.spec = s;
    const texture::TextureRenderResult r = texture::renderTexture(tp, w, h);
    REQUIRE(r.imageF.has_value());
    common::Image img = common::toImage8(*r.imageF);
    if (ground) {
        const Line hz = trueHorizon(s, w, h);
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x)
                if (y + 0.5 > hz.yAt(x + 0.5, w)) {
                    const common::Color8 c = groundColor(x, y);
                    const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
                    img.rgba[p] = static_cast<std::uint8_t>(c.r * groundDim);
                    img.rgba[p + 1] = static_cast<std::uint8_t>(c.g * groundDim);
                    img.rgba[p + 2] = static_cast<std::uint8_t>(c.b * groundDim);
                    img.rgba[p + 3] = 255;
                }
    }
    return img;
}

texture::SkyEstimateOptions optionsWith(const texture::SkyParams& current) {
    texture::SkyEstimateOptions o;
    o.current = current;
    return o;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// §3.1's sign-pinning contract: measured image tilt tau_cw (atan2 of dy/dx in image coords,
// y down, positive = right end lower) EQUALS SkyCamera's rollDeg. Pinned against project() of
// level world directions -- prose is not trusted, including the design doc's own.
// ---------------------------------------------------------------------------------------------
TEST_CASE("sky estimate: roll sign -- tau_cw of the projected horizon equals rollDeg") {
    for (const double roll : {5.0, -5.0, 0.0}) {
        texture::SkyParams s = defaultSky();
        s.rollDeg = roll;
        s.pitchDeg = 12.0;
        const Line hz = trueHorizon(s, 640, 480);
        const double tauCw = std::atan2(hz.yW - hz.y0, 640.0) * kRadToDeg;
        CHECK(tauCw == doctest::Approx(roll).epsilon(0.03));
    }
}

// ---------------------------------------------------------------------------------------------
// Closed-loop camera recovery: render a known sky over textured ground, feed it back, and the
// horizon must land within the DP-grid + refine tolerance. The options' current camera is set
// WRONG on purpose, so an estimator that silently "leaves it unchanged" fails loudly.
// ---------------------------------------------------------------------------------------------
TEST_CASE("sky estimate: closed-loop pitch and roll recovery (incl. the +-5 deg roll signs)") {
    // Pitches chosen so the horizon stays INSIDE a 3:2 frame at the default 62 deg FOV
    // (vertical half-FOV ~21.8 deg: past pitch ~22 the horizon leaves the bottom edge and the
    // photo becomes the separate sky-only case).
    const struct {
        double pitch, roll;
    } cases[] = {{5.0, 0.0}, {12.0, 5.0}, {18.0, -5.0}};
    for (const auto& tc : cases) {
        CAPTURE(tc.pitch);
        CAPTURE(tc.roll);
        texture::SkyParams s = defaultSky();
        s.pitchDeg = tc.pitch;
        s.rollDeg = tc.roll;
        s.cloudCoverage = 0.0;  // a clean dome makes the geometry claim exact
        const common::Image photo = skyFixture(s, 480, 320);

        texture::SkyParams wrong = defaultSky();
        wrong.pitchDeg = 3.0;  // NOT the fixture's camera
        wrong.rollDeg = 0.0;
        const texture::SkyEstimateResult est =
            texture::estimateSkyFromLayer(photo, optionsWith(wrong));
        REQUIRE_FALSE(est.aborted);
        REQUIRE(est.pitch.applied);
        REQUIRE(est.roll.applied);
        CHECK(std::abs(est.params.pitchDeg - tc.pitch) < 1.5);
        CHECK(std::abs(est.params.rollDeg - tc.roll) < 1.2);
        CHECK(est.params.shiftY == 0.0);  // the estimate resets the tilt-shift by design
        // The FOV honesty line is always present (Mosaic keeps no EXIF).
        CHECK(est.summary.find("Field of view unchanged") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------------------------
// EXIF hints (phase 2): a lens-metadata FOV is a MEASUREMENT -- the whole pipeline runs at it
// and the closed loop must still land -- and the date/place credit joins the summary.
// ---------------------------------------------------------------------------------------------
TEST_CASE("sky estimate: EXIF fov hint drives the camera math; metadata credits the summary") {
    const double fovDeg = 2.0 * std::atan(18.0 / 24.0) * kRadToDeg;  // a 24mm-equivalent lens
    texture::SkyParams s = defaultSky();
    s.fovDeg = fovDeg;
    s.pitchDeg = 12.0;
    s.cloudCoverage = 0.0;
    const common::Image photo = skyFixture(s, 480, 320);

    texture::SkyEstimateOptions opts = optionsWith(defaultSky());  // current fov = 62: WRONG
    opts.current.pitchDeg = 3.0;
    opts.fovDegFromExif = fovDeg;
    opts.datePlaceFromExif = true;
    const texture::SkyEstimateResult est = texture::estimateSkyFromLayer(photo, opts);
    REQUIRE_FALSE(est.aborted);
    REQUIRE(est.fov.applied);
    CHECK(est.fov.confidence == 1.0);
    CHECK(est.params.fovDeg == doctest::Approx(fovDeg).epsilon(0.001));
    REQUIRE(est.pitch.applied);
    CHECK(std::abs(est.params.pitchDeg - 12.0) < 1.5);  // inverted at the REAL fov
    CHECK(est.summary.find("lens metadata") != std::string::npos);
    CHECK(est.summary.find("photo's metadata") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// Closed-loop sun disc: a visible sun's crater + glow maps back through the estimated camera.
// ---------------------------------------------------------------------------------------------
TEST_CASE("sky estimate: closed-loop visible-sun azimuth and elevation") {
    texture::SkyParams s = defaultSky();
    s.pitchDeg = 18.0;
    s.cloudCoverage = 0.0;
    s.sunElevationDeg = 25.0;
    s.sunAzimuthDeg = 192.0;  // right of frame centre
    const common::Image photo = skyFixture(s, 480, 320);

    texture::SkyParams wrong = defaultSky();
    wrong.sunElevationDeg = 60.0;
    wrong.sunAzimuthDeg = 180.0;
    const texture::SkyEstimateResult est =
        texture::estimateSkyFromLayer(photo, optionsWith(wrong));
    REQUIRE_FALSE(est.aborted);
    REQUIRE(est.sunElevation.applied);
    REQUIRE(est.sunAzimuth.applied);
    CHECK(std::abs(est.params.sunElevationDeg - 25.0) < 2.5);
    CHECK(std::abs(est.params.sunAzimuthDeg - 192.0) < 2.5);
}

// ---------------------------------------------------------------------------------------------
// Closed-loop atmosphere: with no sun disc the probe match must recover elevation across the
// twilight-to-day range (where the dome signature is genuinely discriminative), plus turbidity
// within a grid step and exposure near the render's own 0 EV.
// ---------------------------------------------------------------------------------------------
TEST_CASE("sky estimate: closed-loop elevation/turbidity/exposure from the dome signature") {
    // `ev` plays the camera: a real twilight photo is exposed UP (nobody keeps a -6 EV frame),
    // so the deep-twilight fixtures carry the exposure a photographer would have used -- and
    // the estimator must hand that exposure back.
    const struct {
        double el, turb, ev;
    } cases[] = {{-6.0, 2.5, 6.0}, {0.0, 2.5, 1.0}, {8.0, 2.5, 0.0},
                 {-3.0, 6.0, 2.5}, {3.0, 6.0, 0.0}};
    for (const auto& tc : cases) {
        CAPTURE(tc.el);
        CAPTURE(tc.turb);
        texture::SkyParams s = defaultSky();
        s.enableSun = false;  // force the S4 analysis-by-synthesis path
        s.enableClouds = false;
        s.enableMoon = false;
        s.starsAmount = 0.0;
        s.cloudCoverage = 0.0;
        s.sunElevationDeg = tc.el;
        s.turbidity = tc.turb;
        s.exposure = tc.ev;
        // Dark silhouette ground: what a real dusk/golden-hour landscape shows. (A ground as
        // bright as the twilight glow itself defeats the border honestly -- the estimator then
        // degrades to "unchanged", covered by the failure-mode docs, not by this test.)
        const common::Image photo = skyFixture(s, 480, 320, /*ground=*/true, /*groundDim=*/0.25);

        texture::SkyParams wrong = defaultSky();
        wrong.sunElevationDeg = 45.0;
        wrong.turbidity = 9.0;
        const texture::SkyEstimateResult est =
            texture::estimateSkyFromLayer(photo, optionsWith(wrong));
        REQUIRE_FALSE(est.aborted);
        REQUIRE(est.sunElevation.applied);
        // Elevation, turbidity and exposure are COUPLED look-alikes near the horizon (a
        // slightly lower sun at a bit more exposure reproduces the same appearance), so the
        // recovered triple trades within these bands while reproducing the photo's look --
        // which is the estimate's actual contract.
        CHECK(std::abs(est.params.sunElevationDeg - tc.el) < 3.5);
        REQUIRE(est.turbidity.applied);
        CHECK(est.params.turbidity >= tc.turb / 2.5);
        CHECK(est.params.turbidity <= tc.turb * 2.5);
        REQUIRE(est.exposure.applied);
        CHECK(std::abs(est.params.exposure - tc.ev) < 2.5);
    }
}

// ---------------------------------------------------------------------------------------------
// The degrade-to-no-change contract: an indoor-like frame (dark textured top, bright textured
// bottom -- every sky gate inverted) must abort cleanly with the params untouched.
// ---------------------------------------------------------------------------------------------
TEST_CASE("sky estimate: indoor-like input aborts with settings unchanged") {
    common::Image img(320, 240);
    for (std::uint32_t y = 0; y < img.height; ++y)
        for (std::uint32_t x = 0; x < img.width; ++x) {
            const std::uint32_t h = hash2(x, y);
            const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
            // Busy warm browns everywhere: no smooth region, no blue, no bright neutral, no
            // boundary line -- a wood-grain closeup, the design's canonical indoor rejection.
            img.rgba[p] = static_cast<std::uint8_t>(70 + (h % 70));
            img.rgba[p + 1] = static_cast<std::uint8_t>(50 + ((h >> 8) % 55));
            img.rgba[p + 2] = static_cast<std::uint8_t>(25 + ((h >> 16) % 35));
            img.rgba[p + 3] = 255;
        }
    const texture::SkyParams current = defaultSky();
    const texture::SkyEstimateResult est =
        texture::estimateSkyFromLayer(img, optionsWith(current));
    CHECK(est.aborted);
    CHECK(est.params == current);  // byte-for-byte untouched
    CHECK_FALSE(est.pitch.applied);
    CHECK_FALSE(est.sunElevation.applied);
}

// ---------------------------------------------------------------------------------------------
// Sky-only input: horizon honestly "left unchanged", segmentation still valid (mask covers
// everything).
// ---------------------------------------------------------------------------------------------
TEST_CASE("sky estimate: sky-only photo leaves the horizon unchanged but masks everything") {
    texture::SkyParams s = defaultSky();
    s.pitchDeg = 40.0;  // horizon falls below the frame: pure dome
    s.cloudCoverage = 0.0;
    const common::Image photo = skyFixture(s, 320, 240, /*ground=*/false);

    texture::SkyParams current = defaultSky();
    current.pitchDeg = 10.0;
    const texture::SkyEstimateResult est =
        texture::estimateSkyFromLayer(photo, optionsWith(current));
    REQUIRE_FALSE(est.aborted);
    CHECK_FALSE(est.pitch.applied);
    CHECK(est.params.pitchDeg == 10.0);
    CHECK(est.segmentationUsable);
    CHECK(est.skyFraction > 0.9);

    std::string note;
    const core::Selection sel = texture::skySelectionFromEstimate(photo, est, nullptr, &note);
    REQUIRE_FALSE(sel.isEmpty());
    std::size_t covered = 0;
    for (const std::uint8_t v : sel.data()) covered += v >= core::kAntsCoverageThreshold;
    CHECK(static_cast<double>(covered) / sel.data().size() > 0.97);
}

// ---------------------------------------------------------------------------------------------
// S6 full-resolution segmentation: IoU against the fixture's known truth mask, plus the holes
// policy -- a tiny speck in the sky is filled (dust rule), a chimney crossing the border stays
// foreground (it is connected to the ground mass).
// ---------------------------------------------------------------------------------------------
TEST_CASE("sky estimate: S6 segmentation IoU + holes policy on a known composite") {
    texture::SkyParams s = defaultSky();
    s.pitchDeg = 18.0;
    s.cloudCoverage = 0.0;
    const std::uint32_t W = 480, H = 320;
    common::Image photo = skyFixture(s, W, H);
    const Line hz = trueHorizon(s, W, H);

    // A 3x3 dark speck well inside the sky (must be swallowed by the holes policy)...
    const std::uint32_t spX = 120, spY = 40;
    for (std::uint32_t y = spY; y < spY + 3; ++y)
        for (std::uint32_t x = spX; x < spX + 3; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * W + x) * 4;
            photo.rgba[p] = 30;
            photo.rgba[p + 1] = 25;
            photo.rgba[p + 2] = 20;
        }
    // ...and a chimney: a dark column rising from the ground 60 px above the horizon line
    // (must STAY foreground -- it is connected to the ground mass).
    const std::uint32_t chX = 320, chW = 14;
    const std::uint32_t chTop = static_cast<std::uint32_t>(hz.yAt(chX, W)) - 60;
    for (std::uint32_t x = chX; x < chX + chW; ++x)
        for (std::uint32_t y = chTop; y < H; ++y) {
            if (y + 0.5 > hz.yAt(x + 0.5, W)) break;  // below the line it is ground already
            const common::Color8 c = groundColor(x, y);
            const std::size_t p = (static_cast<std::size_t>(y) * W + x) * 4;
            photo.rgba[p] = c.r / 2;
            photo.rgba[p + 1] = c.g / 2;
            photo.rgba[p + 2] = c.b / 2;
        }

    const texture::SkyEstimateResult est =
        texture::estimateSkyFromLayer(photo, optionsWith(defaultSky()));
    REQUIRE_FALSE(est.aborted);
    REQUIRE(est.segmentationUsable);

    std::string note;
    const core::Selection sel = texture::skySelectionFromEstimate(photo, est, nullptr, &note);
    REQUIRE_FALSE(sel.isEmpty());

    // IoU against the truth: sky = above the line, minus the chimney (the speck is small enough
    // that the fill rule makes it sky by design).
    std::size_t inter = 0, uni = 0;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const bool chimney =
                x >= chX && x < chX + chW && y >= chTop && y + 0.5 <= hz.yAt(x + 0.5, W);
            const bool truth = (y + 0.5 <= hz.yAt(x + 0.5, W)) && !chimney;
            const bool got = sel.at(x, y) >= core::kAntsCoverageThreshold;
            inter += truth && got;
            uni += truth || got;
        }
    const double iou = static_cast<double>(inter) / static_cast<double>(uni);
    CAPTURE(iou);
    CHECK(iou > 0.93);

    // Holes policy, pointwise.
    CHECK(sel.at(spX + 1, spY + 1) >= core::kAntsCoverageThreshold);  // speck filled into sky
    CHECK(sel.at(chX + chW / 2, chTop + 20) < core::kAntsCoverageThreshold);  // chimney kept
}

// ---------------------------------------------------------------------------------------------
// S7 harmonization: deterministic, range-clamped scalars; the night response is monotone in the
// target elevation and dead by day.
// ---------------------------------------------------------------------------------------------
TEST_CASE("sky estimate: S7 harmonization scalars -- determinism, ranges, night monotonicity") {
    texture::SkyParams s = defaultSky();
    s.cloudCoverage = 0.0;
    const common::Image photo = skyFixture(s, 320, 240);
    const texture::SkyEstimateResult est =
        texture::estimateSkyFromLayer(photo, optionsWith(defaultSky()));
    REQUIRE(est.segmentationUsable);
    const core::Selection sel = texture::skySelectionFromEstimate(photo, est);
    REQUIRE_FALSE(sel.isEmpty());

    const auto bagFor = [&](double elTarget, double strength = 1.0) {
        texture::PhotometricMatchInput in;
        in.sky = defaultSky();
        in.sky.sunElevationDeg = elTarget;
        in.photoElevationDeg = est.photoElevationForMatch;
        in.photoTurbidity = est.photoTurbidityForMatch;
        in.photoSkyExposureEv = est.params.exposure;
        in.strength = strength;
        return texture::photometricMatchParams(in, photo, sel);
    };

    // Determinism: two identical calls, identical bags.
    const auto a = bagFor(10.0);
    const auto b = bagFor(10.0);
    CHECK(a == b);

    // Ranges.
    for (const char* k : {"gain_r", "gain_g", "gain_b"}) {
        CHECK(a.at(k) >= 0.6);
        CHECK(a.at(k) <= 1.6);
    }
    CHECK(a.at("delta_ev") >= -6.0);
    CHECK(a.at("delta_ev") <= 2.0);
    CHECK(a.at("sigma_ratio") >= 0.7);
    CHECK(a.at("sigma_ratio") <= 1.3);
    CHECK(std::abs(a.at("gradient")) <= 0.12);

    // Day: no rod signal, full saturation. Deepening night: rod grows monotonically, delta_ev
    // falls (the scene dims), saturation drains.
    const auto day = bagFor(10.0);
    const auto dusk = bagFor(-8.0);
    const auto night = bagFor(-16.0);
    CHECK(day.at("rod") == 0.0);
    CHECK(dusk.at("rod") > 0.0);
    CHECK(night.at("rod") > dusk.at("rod"));
    CHECK(night.at("rod") <= 1.0);
    CHECK(dusk.at("delta_ev") < day.at("delta_ev"));
    CHECK(night.at("delta_ev") <= dusk.at("delta_ev"));
    CHECK(night.at("saturation") < day.at("saturation"));
    CHECK(day.at("saturation") == doctest::Approx(1.0 - 0.5 * day.at("rod")));

    // Strength 0 collapses to the identity grade.
    const auto off = bagFor(-16.0, 0.0);
    CHECK(off.at("gain_r") == doctest::Approx(1.0));
    CHECK(off.at("delta_ev") == doctest::Approx(0.0));
    CHECK(off.at("rod") == doctest::Approx(0.0));
    CHECK(off.at("sigma_ratio") == doctest::Approx(1.0));
}

// ---------------------------------------------------------------------------------------------
// AdjustmentKind::PhotometricMatch in the compositor: the fused transfer against a reference
// implementation, the empty-bag no-op, opacity scaling and mask gating.
// ---------------------------------------------------------------------------------------------
namespace {

double srgbDecodeD(double e) {
    return e <= 0.04045 ? std::max(0.0, e) / 12.92 : std::pow((e + 0.055) / 1.055, 2.4);
}
double srgbEncodeD(double l) {
    return l <= 0.0031308 ? std::max(0.0, l) * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}

// The reference transfer (the research doc's §6.2, double precision).
common::Color8 referenceMatch(common::Color8 in, const std::map<std::string, double>& bag,
                              double yFrac) {
    const auto get = [&](const char* k, double d) {
        const auto it = bag.find(k);
        return it == bag.end() ? d : it->second;
    };
    double r = srgbDecodeD(in.r / 255.0) * get("gain_r", 1.0);
    double g = srgbDecodeD(in.g / 255.0) * get("gain_g", 1.0);
    double b = srgbDecodeD(in.b / 255.0) * get("gain_b", 1.0);
    const auto lum = [](double rr, double gg, double bb) {
        return 0.2126 * rr + 0.7152 * gg + 0.0722 * bb;
    };
    const double L = std::max(1e-6, lum(r, g, b));
    const double mu = get("mu_log", std::log(0.18));
    const double logL2 = mu + get("delta_ev", 0.0) * std::numbers::ln2 +
                         (std::log(L) - mu) * get("sigma_ratio", 1.0);
    double f = std::exp(logL2 - std::log(L));
    const double Lh = L * f;
    const double pivot = std::max(L, 0.7);
    if (Lh > pivot) {
        const double head = std::max(1e-4, 1.0 - pivot);
        const double over = Lh - pivot;
        f *= (pivot + head * over / (head + over)) / Lh;
    }
    const double rowF = 1.0 + get("gradient", 0.0) * (1.0 - 2.0 * yFrac);
    r *= f * rowF;
    g *= f * rowF;
    b *= f * rowF;
    const double rod = get("rod", 0.0);
    if (rod > 0.0) {
        const double X = 0.4124 * r + 0.3576 * g + 0.1805 * b;
        const double Y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        const double Z = 0.0193 * r + 0.1192 * g + 0.9505 * b;
        const double V = std::max(0.0, Y * (1.33 * (1.0 + (Y + Z) / std::max(1e-6, X)) - 1.68));
        const double n = V * get("night_gain", 1.0);
        r = std::lerp(r, n * get("night_r", 0.42), rod);
        g = std::lerp(g, n * get("night_g", 0.55), rod);
        b = std::lerp(b, n * get("night_b", 1.0), rod);
    }
    const double sat = get("saturation", 1.0);
    if (sat != 1.0) {
        const double Ls = lum(r, g, b);
        r = Ls + (r - Ls) * sat;
        g = Ls + (g - Ls) * sat;
        b = Ls + (b - Ls) * sat;
    }
    const auto enc = [](double v) {
        return static_cast<std::uint8_t>(std::clamp(srgbEncodeD(v), 0.0, 1.0) * 255.0 + 0.5);
    };
    return {enc(r), enc(g), enc(b), in.a};
}

common::Color8 pxAt(const common::Image& img, std::uint32_t x, std::uint32_t y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

int chanDiff(common::Color8 a, common::Color8 b) {
    return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
}

}  // namespace

TEST_CASE("PhotometricMatch: compositor math matches the reference transfer") {
    core::Document doc(8, 8);
    auto base = doc.makeRaster("base");
    for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 8; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * 8 + x) * 4;
            base->image().rgba[p] = static_cast<std::uint8_t>(20 + x * 30);
            base->image().rgba[p + 1] = static_cast<std::uint8_t>(15 + y * 28);
            base->image().rgba[p + 2] = static_cast<std::uint8_t>(200 - x * 12);
            base->image().rgba[p + 3] = 255;
        }
    const common::Image original = base->image();
    doc.root().addOnTop(std::move(base));

    std::map<std::string, double> bag{{"gain_r", 1.25}, {"gain_g", 1.0},   {"gain_b", 0.8},
                                      {"mu_log", -1.8}, {"delta_ev", 0.8}, {"sigma_ratio", 1.15},
                                      {"gradient", 0.1}, {"rod", 0.3},     {"saturation", 0.85}};
    auto adj = doc.makeAdjustment("match", core::AdjustmentKind::PhotometricMatch);
    adj->params() = bag;
    doc.root().addOnTop(std::move(adj));

    const render::CompositeResult r = render::composite(doc, {}, render::Backend::Cpu);
    REQUIRE(r.ok);
    for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 8; ++x) {
            const common::Color8 want = referenceMatch(pxAt(original, x, y), bag, y / 8.0);
            CAPTURE(x);
            CAPTURE(y);
            CHECK(chanDiff(pxAt(r.image, x, y), want) <= 2);  // LUT lerp + float vs double
        }
}

TEST_CASE("PhotometricMatch: empty bag is a no-op; opacity halves; the mask gates") {
    core::Document doc(6, 6);
    auto base = doc.makeRaster("base");
    for (std::uint32_t y = 0; y < 6; ++y)
        for (std::uint32_t x = 0; x < 6; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * 6 + x) * 4;
            base->image().rgba[p] = static_cast<std::uint8_t>(10 + x * 45);
            base->image().rgba[p + 1] = static_cast<std::uint8_t>(240 - y * 40);
            base->image().rgba[p + 2] = 128;
            base->image().rgba[p + 3] = 255;
        }
    const common::Image original = base->image();
    doc.root().addOnTop(std::move(base));

    auto adj = doc.makeAdjustment("match", core::AdjustmentKind::PhotometricMatch);
    core::AdjustmentLayer* adjPtr = adj.get();
    doc.root().addOnTop(std::move(adj));

    // Empty bag: identity within one code value (the LUT decode/encode round trip).
    const render::CompositeResult noop = render::composite(doc, {}, render::Backend::Cpu);
    REQUIRE(noop.ok);
    for (std::uint32_t y = 0; y < 6; ++y)
        for (std::uint32_t x = 0; x < 6; ++x)
            CHECK(chanDiff(pxAt(noop.image, x, y), pxAt(original, x, y)) <= 1);

    // A strong dimming grade at full vs half opacity: half sits between original and full.
    adjPtr->params() = {{"delta_ev", -2.0}, {"mu_log", -1.6}};
    const render::CompositeResult full = render::composite(doc, {}, render::Backend::Cpu);
    adjPtr->setOpacity(0.5f);
    const render::CompositeResult half = render::composite(doc, {}, render::Backend::Cpu);
    REQUIRE(full.ok);
    REQUIRE(half.ok);
    const common::Color8 o = pxAt(original, 3, 3);
    const common::Color8 f = pxAt(full.image, 3, 3);
    const common::Color8 h = pxAt(half.image, 3, 3);
    CHECK(f.g < o.g);  // the grade really dims
    const int mid = (static_cast<int>(o.g) + f.g) / 2;
    CHECK(std::abs(static_cast<int>(h.g) - mid) <= 2);  // encoded-space lerp semantics

    // Mask: coverage 0 on the left half -> untouched there, graded on the right.
    adjPtr->setOpacity(1.0f);
    core::RasterMask mask(6, 6, 255);
    for (std::uint32_t y = 0; y < 6; ++y)
        for (std::uint32_t x = 0; x < 3; ++x) mask.coverage[y * 6 + x] = 0;
    adjPtr->setMask(std::move(mask));
    const render::CompositeResult masked = render::composite(doc, {}, render::Backend::Cpu);
    REQUIRE(masked.ok);
    CHECK(chanDiff(pxAt(masked.image, 1, 3), pxAt(original, 1, 3)) <= 1);
    CHECK(pxAt(masked.image, 4, 3).g < pxAt(original, 4, 3).g);
}

// ---------------------------------------------------------------------------------------------
// docio: the new kind + its params bag round-trip through the .mosaic container (additive
// schema growth -- a name token plus the generic params map).
// ---------------------------------------------------------------------------------------------
TEST_CASE("PhotometricMatch: docio round-trips the kind and every scalar") {
    auto doc = std::make_unique<core::Document>(32, 24);
    doc->setUuid(io::native::mintDocumentUuid());
    auto adj = doc->makeAdjustment("Match sky", core::AdjustmentKind::PhotometricMatch);
    adj->params() = {{"gain_r", 1.21},    {"gain_g", 0.97},   {"gain_b", 0.81},
                     {"mu_log", -1.83},   {"delta_ev", -2.5}, {"sigma_ratio", 1.12},
                     {"gradient", -0.07}, {"rod", 0.66},      {"night_r", 0.42},
                     {"night_g", 0.55},   {"night_b", 1.0},   {"night_gain", 1.0},
                     {"saturation", 0.67}, {"strength", 1.0}};
    adj->setClipToBelow(true);
    const auto originalParams = adj->params();
    doc->root().addOnTop(std::move(adj));

    std::string error;
    const auto input = io::native::buildDocumentCheckpoint(*doc, &error);
    REQUIRE_MESSAGE(input.has_value(), error);
    const std::vector<std::uint8_t> bytes = io::native::buildCheckpoint(*input);
    const io::native::OpenReport report = io::native::openDocument(bytes);
    auto back = io::native::documentFromReport(report, &error);
    REQUIRE_MESSAGE(back.has_value(), error);
    REQUIRE(back->document != nullptr);
    CHECK(back->rejectedChunks == 0);

    REQUIRE(back->document->root().childCount() == 1);
    const auto* backAdj = back->document->root().child(0).as<core::AdjustmentLayer>();
    REQUIRE(backAdj != nullptr);
    CHECK(backAdj->adjustmentKind() == core::AdjustmentKind::PhotometricMatch);
    CHECK(backAdj->params() == originalParams);
    CHECK(backAdj->clipToBelow());
}

// ---------------------------------------------------------------------------------------------
// The commit-shape helper: ONE CompositeCommand assembling sky-below + foreground mask +
// clipped harmonization, fully undoable.
// ---------------------------------------------------------------------------------------------
TEST_CASE("sky estimate: buildSkyConformCommand shapes the commit and undoes cleanly") {
    core::Document doc(64, 48);
    auto bg = doc.makeRaster("background");
    doc.root().addOnTop(std::move(bg));
    auto photo = doc.makeRaster("photo");
    for (std::uint32_t y = 0; y < 48; ++y)
        for (std::uint32_t x = 0; x < 64; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * 64 + x) * 4;
            photo->image().rgba[p] = 90;
            photo->image().rgba[p + 1] = 120;
            photo->image().rgba[p + 2] = 160;
            photo->image().rgba[p + 3] = 255;
        }
    const core::LayerId photoId = photo->id();
    doc.root().addOnTop(std::move(photo));
    REQUIRE(doc.root().childCount() == 2);

    // Sky = the top half of the document.
    core::Selection sky = core::Selection::rectangle(64, 48, {0, 0, 64, 24});

    texture::SkyConformPlan plan;
    plan.skyParams = texture::defaultTextureParams(texture::Generator::Sky);
    plan.baked = texture::renderTexture(plan.skyParams, 64, 48);
    plan.skySelection = sky;
    plan.matchParams = {{"delta_ev", -1.0}, {"mu_log", -1.7}};
    auto cmd = texture::buildSkyConformCommand(doc, photoId, std::move(plan));
    REQUIRE(cmd != nullptr);
    doc.commands().push(std::move(cmd));

    // Order bottom->top: [background, sky, photo, match adjustment].
    REQUIRE(doc.root().childCount() == 4);
    CHECK(doc.root().child(0).name() == "background");
    CHECK(doc.root().child(1).kind() == core::LayerKind::Texture);
    CHECK(doc.root().child(2).id() == photoId);
    const auto* adj = doc.root().child(3).as<core::AdjustmentLayer>();
    REQUIRE(adj != nullptr);
    CHECK(adj->adjustmentKind() == core::AdjustmentKind::PhotometricMatch);
    CHECK(adj->clipToBelow());
    CHECK(adj->params().at("delta_ev") == -1.0);

    // The photo's mask reveals the FOREGROUND: hidden over the sky half, visible below.
    const core::Layer* photoLayer = doc.find(photoId);
    REQUIRE(photoLayer != nullptr);
    REQUIRE(photoLayer->hasMask());
    const core::RasterMask* mask = photoLayer->mask();
    CHECK(mask->coverage[static_cast<std::size_t>(6) * mask->width + 10] == 0);     // sky half
    CHECK(mask->coverage[static_cast<std::size_t>(40) * mask->width + 10] == 255);  // foreground

    // The texture layer landed with the sky arm intact.
    const auto* tex = doc.root().child(1).as<core::TextureLayer>();
    REQUIRE(tex != nullptr);
    CHECK(std::holds_alternative<texture::SkyParams>(tex->params().spec));

    // One undo step restores everything.
    doc.commands().undo();
    REQUIRE(doc.root().childCount() == 2);
    CHECK(doc.root().child(0).name() == "background");
    CHECK(doc.root().child(1).id() == photoId);
    CHECK_FALSE(doc.find(photoId)->hasMask());
}

// ---------------------------------------------------------------------------------------------
// The worker: epoch coalescing (a newer request supersedes and cancels the one in flight) and
// cancelAll (no result surfaces).
// ---------------------------------------------------------------------------------------------
TEST_CASE("sky estimate: worker coalesces to the newest epoch and honors cancelAll") {
    texture::SkyParams s = defaultSky();
    s.cloudCoverage = 0.0;
    const common::Image photo = skyFixture(s, 900, 600);

    texture::SkyEstimateWorker worker;
    texture::SkyEstimateWorker::Job job;
    job.photo = photo;
    job.options = optionsWith(defaultSky());

    job.epoch = 1;
    worker.request(job);
    job.epoch = 2;
    worker.request(job);  // supersedes epoch 1 (and cancels it if it already started)

    bool saw2 = false;
    for (int i = 0; i < 600 && !saw2; ++i) {
        if (const auto r = worker.takeResult()) {
            CHECK(r->epoch >= 1);
            if (r->epoch == 2) {
                saw2 = true;
                CHECK_FALSE(r->estimate.cancelled);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    CHECK(saw2);

    // cancelAll: the in-flight estimate is discarded, nothing surfaces.
    job.epoch = 3;
    worker.request(job);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));  // let it start
    worker.cancelAll();
    for (int i = 0; i < 200 && worker.busy(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    CHECK_FALSE(worker.busy());
    CHECK_FALSE(worker.takeResult().has_value());
}

// ---------------------------------------------------------------------------------------------
// S5 almanac inversion: the picking policy, the unreachable clamp and the polar note.
// ---------------------------------------------------------------------------------------------
TEST_CASE("sky estimate: invertTimeFromElevation picks, clamps and reports honestly") {
    const texture::UtcTime midsummer{2026, 6, 21, 12.0};

    // London, sun at 12 deg: two crossings; the current clock picks the nearer, the note
    // carries the other.
    const auto morning =
        texture::invertTimeFromElevation(midsummer, 51.5, -0.13, 12.0, /*current=*/7.0);
    REQUIRE(morning.valid);
    CHECK(morning.hasAlternative);
    CHECK(morning.hourUtc < 12.0);             // the morning crossing
    CHECK(morning.alternativeHourUtc > 12.0);  // the afternoon one
    CHECK(morning.note.find("morning assumed") != std::string::npos);

    const auto evening =
        texture::invertTimeFromElevation(midsummer, 51.5, -0.13, 12.0, /*current=*/20.0);
    REQUIRE(evening.valid);
    CHECK(evening.hourUtc > 12.0);
    CHECK(evening.note.find("afternoon assumed") != std::string::npos);

    // Unreachable elevation clamps to solar noon and says so.
    const auto clamped = texture::invertTimeFromElevation(midsummer, 51.5, -0.13, 70.0, 12.0);
    REQUIRE(clamped.valid);
    CHECK(clamped.note.find("solar noon") != std::string::npos);

    // Longyearbyen midnight sun: the sun never dips to -5 deg in June; polar-clamped.
    const auto polar = texture::invertTimeFromElevation(midsummer, 78.22, 15.65, -5.0, 12.0);
    REQUIRE(polar.valid);
    CHECK(polar.note.find("midnight sun") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// Stage-cost report (design §5.4's budget table): measured on a 720p day fixture and printed
// for the session log. Ceilings are deliberately loose -- this guards against pathological
// regressions, not micro-drift.
// ---------------------------------------------------------------------------------------------
TEST_CASE("sky estimate: stage timing report at 1280x720") {
    texture::SkyParams s = defaultSky();
    s.cloudCoverage = 0.0;
    s.enableSun = false;  // no crater: times the FULL 80-probe analysis-by-synthesis grid
    s.pitchDeg = 8.0;     // real ground share at 16:9, so S6 does representative work
    const common::Image photo = skyFixture(s, 1280, 720);

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const texture::SkyEstimateResult est =
        texture::estimateSkyFromLayer(photo, optionsWith(defaultSky()));
    const auto t1 = clock::now();
    REQUIRE_FALSE(est.aborted);
    REQUIRE(est.segmentationUsable);
    const core::Selection sel = texture::skySelectionFromEstimate(photo, est);
    const auto t2 = clock::now();
    REQUIRE_FALSE(sel.isEmpty());
    texture::PhotometricMatchInput in;
    in.sky = est.params;
    in.photoElevationDeg = est.photoElevationForMatch;
    in.photoTurbidity = est.photoTurbidityForMatch;
    const auto bag = texture::photometricMatchParams(in, photo, sel);
    const auto t3 = clock::now();
    CHECK(bag.size() >= 10);

    const auto ms = [](auto a, auto b) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    };
    MESSAGE("estimate (S0-S5): " << ms(t0, t1) << " ms; segmentation (S6): " << ms(t1, t2)
                                 << " ms; harmonization (S7): " << ms(t2, t3) << " ms");
    CHECK(ms(t0, t1) < 30000);
    CHECK(ms(t1, t2) < 30000);
    CHECK(ms(t2, t3) < 10000);
}
