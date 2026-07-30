#include <doctest/doctest.h>

#include "common/image.hpp"
#include "core/brush/mask_stroke.hpp"
#include "core/layer.hpp"
#include "core/red_eye.hpp"
#include "core/selection.hpp"
#include "ui/red_eye_gesture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

// The S38-b eye retouch (docs/red-eye-tool.md §3.1/§3.2), tested headlessly on SYNTHETIC swatches
// and figures generated right here -- no fixture files, no photographs in the tree. The colours
// below were chosen to match what real flash-red-eye and bloodshot-eye photographs actually
// measure, so a change that "passes on grey squares" but ruins a real portrait fails here.
namespace {

using mosaic::common::ColorF;
using mosaic::common::Image;
using mosaic::core::RedEyeMode;
using mosaic::core::RedEyeParams;
using mosaic::core::Selection;

// ---- The swatches ------------------------------------------------------------------------------
// Each is the MEAN of the corresponding pixels measured across a set of real photographs; they are
// stated as constants here so the thresholds in red_eye.hpp are pinned against the thing they were
// tuned for rather than against a convenient invention.
constexpr ColorF kFlashPupil{0.63f, 0.03f, 0.06f, 1.0f};   // classic bright retinal reflection
constexpr ColorF kMagentaPupil{0.55f, 0.21f, 0.39f, 1.0f}; // the same reflection tinted by the iris
constexpr ColorF kPaleSkin{0.85f, 0.68f, 0.60f, 1.0f};     // bright skin next to the eye
constexpr ColorF kWarmSkin{0.72f, 0.52f, 0.44f, 1.0f};     // the reddest ordinary skin tone
constexpr ColorF kRedJumper{0.72f, 0.06f, 0.06f, 1.0f};    // a saturated red garment in the frame
constexpr ColorF kBlueIris{0.44f, 0.63f, 0.65f, 1.0f};     // the iris ring around a red pupil
constexpr ColorF kWhiteGlint{0.96f, 0.94f, 0.95f, 1.0f};   // an untinted specular catchlight
constexpr ColorF kRedGlint{1.00f, 0.42f, 0.46f, 1.0f};     // ... and one the flash tinted red
constexpr ColorF kSclera{0.86f, 0.86f, 0.89f, 1.0f};       // healthy blue-white sclera
constexpr ColorF kVessel{0.84f, 0.34f, 0.39f, 1.0f};       // a conjunctival vessel over it
constexpr ColorF kDarkIris{0.11f, 0.15f, 0.16f, 1.0f};     // the dark iris a glow fades into
constexpr ColorF kLidShadow{0.10f, 0.14f, 0.20f, 1.0f};    // lash / lid shadow inside the brush
constexpr ColorF kBloodPatch{0.47f, 0.28f, 0.21f, 1.0f};   // solid subconjunctival hemorrhage

// The W3C luminance the whole module preserves (core/blend_math.hpp's weights).
[[nodiscard]] float lumOf(ColorF c) { return 0.30f * c.r + 0.59f * c.g + 0.11f * c.b; }

// The redness axis the metrics ride, restated here so a test can measure "how red is this pixel"
// without reaching into the module's private helpers.
[[nodiscard]] float redExcessOf(ColorF c) { return c.r - (0.70f * c.g + 0.30f * c.b); }

[[nodiscard]] float satOf(ColorF c) {
    return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b});
}

[[nodiscard]] Image solid(std::uint32_t w, std::uint32_t h, ColorF c) {
    Image img(w, h);
    img.fill(mosaic::common::toColor8(c));
    return img;
}

// A HARD-edged disc (no anti-aliasing on purpose): every pixel is exactly one swatch or the other,
// so a test can talk about "the pupil pixels" without a partial-coverage ramp muddying the claim.
void paintDisc(Image& img, double cx, double cy, double r, ColorF c) {
    const auto packed = mosaic::common::toColor8(c);
    for (std::uint32_t y = 0; y < img.height; ++y)
        for (std::uint32_t x = 0; x < img.width; ++x) {
            const double dx = (x + 0.5) - cx;
            const double dy = (y + 0.5) - cy;
            if (dx * dx + dy * dy <= r * r) {
                const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 4;
                img.rgba[i] = packed.r;
                img.rgba[i + 1] = packed.g;
                img.rgba[i + 2] = packed.b;
                img.rgba[i + 3] = packed.a;
            }
        }
}

void paintRect(Image& img, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1,
               ColorF c) {
    const auto packed = mosaic::common::toColor8(c);
    for (std::uint32_t y = y0; y < std::min(y1, img.height); ++y)
        for (std::uint32_t x = x0; x < std::min(x1, img.width); ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 4;
            img.rgba[i] = packed.r;
            img.rgba[i + 1] = packed.g;
            img.rgba[i + 2] = packed.b;
            img.rgba[i + 3] = packed.a;
        }
}

[[nodiscard]] ColorF at(const Image& img, std::uint32_t x, std::uint32_t y) {
    const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[i] / 255.0f, img.rgba[i + 1] / 255.0f, img.rgba[i + 2] / 255.0f,
            img.rgba[i + 3] / 255.0f};
}

// Run the retouch and paste its patch back, so a test reads the image exactly as the app's
// region-scoped SetLayerPixelsCommand would leave it. An empty patch means "no undo step", which
// here means "the image comes back untouched".
[[nodiscard]] Image applied(const Image& src, const Selection& scope, const RedEyeParams& p) {
    Image out = src;
    const mosaic::core::RetouchPatch patch = mosaic::core::retouchEye(src, scope, p);
    if (!patch.empty())
        mosaic::common::blitRegion(out, patch.pixels, patch.originX, patch.originY);
    return out;
}

// The default Tier-1 parameters, with `darken` at its shipped value.
[[nodiscard]] RedEyeParams flashParams() {
    RedEyeParams p;
    p.mode = RedEyeMode::Flash;
    return p;
}

[[nodiscard]] RedEyeParams scleraParams() {
    RedEyeParams p;
    p.mode = RedEyeMode::Sclera;
    return p;
}

// Total absolute 8-bit difference between two same-sized images -- "how much did that pass move?".
[[nodiscard]] long long totalDelta(const Image& a, const Image& b) {
    long long sum = 0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i)
        sum += std::abs(static_cast<int>(a.rgba[i]) - static_cast<int>(b.rgba[i]));
    return sum;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Tier 1 -- the redness metric
// ------------------------------------------------------------------------------------------------

TEST_CASE("the flash redness metric fires on a red pupil and is silent on skin and iris") {
    using mosaic::core::flashGlowScore;

    // A classic retinal reflection saturates the gate outright.
    CHECK(flashGlowScore(kFlashPupil) > 0.99f);
    // A magenta-tinted one still scores well past half -- the whole reason the metric weights green
    // 0.70 and blue 0.30 instead of using the textbook R - max(G,B), which reads ~0 on this colour.
    CHECK(flashGlowScore(kMagentaPupil) > 0.40f);
    CHECK(flashGlowScore(kMagentaPupil) < 0.95f); // ... and it under-corrects it, on purpose

    // Skin is rejected OUTRIGHT, not merely damped: bright skin carries a real red excess but
    // spends it on being bright, so the purity gate zeroes it. Exactness is the point here -- a
    // brush that slips onto an eyelid must not bleach it even slightly.
    CHECK(flashGlowScore(kPaleSkin) == 0.0f);
    CHECK(flashGlowScore(kWarmSkin) < 0.02f); // the reddest ordinary skin: a rounding error at most
    CHECK(flashGlowScore(kBlueIris) == 0.0f);
    CHECK(flashGlowScore(kWhiteGlint) == 0.0f);

    // A NEUTRAL colour scores exactly zero at every level. This is not an aesthetic property, it is
    // what makes the Tier-1 transform idempotent: its own output is a fixed point.
    CHECK(flashGlowScore({0.0f, 0.0f, 0.0f, 1.0f}) == 0.0f);
    CHECK(flashGlowScore({0.4f, 0.4f, 0.4f, 1.0f}) == 0.0f);
    CHECK(flashGlowScore({1.0f, 1.0f, 1.0f, 1.0f}) == 0.0f);
}

TEST_CASE("the metric cannot tell a red jumper from a red pupil -- the scope is what protects it") {
    using mosaic::core::flashGlowScore;
    // Stated as a test rather than hidden in a comment, because it is the load-bearing honesty of
    // the whole design: a saturated red garment and a retinal reflection are THE SAME COLOUR, and
    // no per-pixel colour metric can separate them. The tool is safe because the user says where
    // (red_eye.hpp's invariant 1), not because the metric is clever -- which is exactly why this
    // module has no detector and never scans the image. The end-to-end guarantee is the next test;
    // a future change that tries to "fix" this by adding an auto-detector breaks the module's
    // standing invariant and must not land.
    CHECK(flashGlowScore(kRedJumper) > 0.9f);
}

TEST_CASE("a red jumper outside the scope comes back byte-identical") {
    Image src = solid(64, 64, kPaleSkin);
    paintDisc(src, 20.5, 20.5, 6.0, kBlueIris);  // an iris ring ...
    paintDisc(src, 20.5, 20.5, 3.0, kFlashPupil); // ... with a glowing pupil inside it
    paintRect(src, 0, 44, 64, 64, kRedJumper);    // and a red jumper filling the bottom quarter

    // The scope is a small ellipse over the pupil, exactly what the tool's size ring covers.
    const Selection scope = Selection::ellipse(64, 64, {14.0, 14.0, 13.0, 13.0});
    const Image out = applied(src, scope, flashParams());

    // The jumper is nowhere near the scope, so not one of its bytes may move. Accumulated into a
    // single assertion rather than 1280 of them, so a failure names the property, not a pixel.
    bool jumperUntouched = true;
    for (std::uint32_t y = 44; y < 64; ++y)
        for (std::uint32_t x = 0; x < 64; ++x)
            jumperUntouched = jumperUntouched && at(out, x, y) == at(src, x, y);
    CHECK(jumperUntouched);

    // Everything outside the scope's bounding box, jumper or not, is untouched too.
    bool topRowUntouched = true;
    for (std::uint32_t x = 0; x < 64; ++x)
        topRowUntouched = topRowUntouched && at(out, x, 0) == at(src, x, 0);
    CHECK(topRowUntouched);

    // ... while the pupil itself really was corrected: darker and no longer red.
    const ColorF pupil = at(out, 20, 20);
    CHECK(redExcessOf(pupil) < redExcessOf(kFlashPupil));
    CHECK(redExcessOf(pupil) < 0.02f);
    CHECK(lumOf(pupil) < lumOf(kFlashPupil));

    // The iris ring is a blue-green, so the redness gate never touched it: it is byte-identical
    // even though it sits INSIDE the scope. Scope says where the tool may act; the gate says where
    // it does.
    CHECK(at(out, 20, 15) == at(src, 20, 15));
}

// ------------------------------------------------------------------------------------------------
// Tier 1 -- luminance preservation and the catchlight
// ------------------------------------------------------------------------------------------------

TEST_CASE("at zero darken the correction only collapses chroma and keeps the luminance") {
    // This is the classical chroma-collapse move on its own, with the darkening dialled out: the
    // corrected pixel is achromatic and sits at the luminance the pupil HONESTLY has once the
    // glow's inflated red is replaced by avg(G,B). Keeping the luminance keeps the catchlight.
    Image src = solid(16, 16, kFlashPupil);
    RedEyeParams p = flashParams();
    p.darken = 0.0;
    const Image out = applied(src, Selection::rectangle(16, 16, {0.0, 0.0, 16.0, 16.0}), p);

    const ColorF c = at(out, 8, 8);
    // Achromatic to within 8-bit rounding.
    CHECK(std::abs(c.r - c.g) <= 2.0f / 255.0f);
    CHECK(std::abs(c.g - c.b) <= 2.0f / 255.0f);
    // ... at the red-free luminance: lum({avg(G,B), G, B}) for the source swatch.
    // An ABSOLUTE tolerance, not a relative one: the target luminance here is ~0.04, where a single
    // 8-bit quantization step is already ~10% of the value.
    const float mid = 0.5f * (kFlashPupil.g + kFlashPupil.b);
    const float expected = lumOf({mid, kFlashPupil.g, kFlashPupil.b, 1.0f});
    CHECK(std::abs(lumOf(c) - expected) < 0.01f);
}

TEST_CASE("the catchlight survives a red-eye correction and stays the brightest point") {
    Image src = solid(48, 48, kFlashPupil);
    paintDisc(src, 26.5, 20.5, 3.0, kRedGlint); // a bright, flash-tinted catchlight in the pupil

    const Selection scope = Selection::rectangle(48, 48, {0.0, 0.0, 48.0, 48.0});
    const Image kept = applied(src, scope, flashParams());

    RedEyeParams off = flashParams();
    off.keepCatchlight = false;
    const Image lost = applied(src, scope, off);

    const float glintKept = lumOf(at(kept, 26, 20));
    const float glintLost = lumOf(at(lost, 26, 20));
    const float pupilKept = lumOf(at(kept, 6, 6));

    // §3.1: the correction leaves the catchlight. It is still there, still far brighter than the
    // pupil around it, and the ordering the photograph had is never inverted.
    CHECK(glintKept > pupilKept);
    CHECK(glintKept > 3.0f * pupilKept);
    // Turning the guard off measurably darkens it -- so the toggle does the thing it claims.
    CHECK(glintKept > glintLost);
    // The chroma still collapses there either way: a red-tinted glare comes out white, not pink.
    CHECK(redExcessOf(at(kept, 26, 20)) < 0.05f);
    // And the pupil around it really did go dark -- the catchlight guard is local, not a global
    // opt-out of the correction.
    CHECK(pupilKept < 0.15f);
}

// ------------------------------------------------------------------------------------------------
// Tier 1 -- the rim
// ------------------------------------------------------------------------------------------------

namespace {

// A glow does not end at a threshold: it FADES into the iris over a pixel or two, because the
// reflection blooms and because the sensor and the JPEG both blur it. This figure is that fade,
// measured off a real flash frame -- pure glow to radius `rCore`, then a linear mix into a dark
// iris out to `rFade`. The mixed pixels are the whole problem: they are unmistakably red (excess
// 0.12-0.27) but they read at a fraction of the core's redness, which is exactly where a single
// threshold gives up.
[[nodiscard]] Image glowWithFadedRim(std::uint32_t size, double cx, double cy, double rCore,
                                     double rFade) {
    Image img = solid(size, size, kDarkIris);
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            const double dx = (x + 0.5) - cx;
            const double dy = (y + 0.5) - cy;
            const double d = std::sqrt(dx * dx + dy * dy);
            if (d > rFade)
                continue;
            const double t = d <= rCore ? 1.0 : 1.0 - (d - rCore) / (rFade - rCore);
            const ColorF c{static_cast<float>(kFlashPupil.r * t + kDarkIris.r * (1.0 - t)),
                           static_cast<float>(kFlashPupil.g * t + kDarkIris.g * (1.0 - t)),
                           static_cast<float>(kFlashPupil.b * t + kDarkIris.b * (1.0 - t)), 1.0f};
            const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 4;
            const auto packed = mosaic::common::toColor8(c);
            img.rgba[i] = packed.r;
            img.rgba[i + 1] = packed.g;
            img.rgba[i + 2] = packed.b;
            img.rgba[i + 3] = packed.a;
        }
    return img;
}

// The reddest pixel left, among exactly those pixels the SOURCE says are glow. Bounding the
// residual there is the honest form of "no rim": it is a claim about the pixels the tool undertakes
// to correct, and it says nothing about the fade further out, where the source is mostly iris and
// a correction that reached it would be bleaching the iris -- the artifact on the other side of
// this trade.
[[nodiscard]] float worstResidualWhereSourceRedderThan(const Image& src, const Image& out,
                                                       float level) {
    float worst = -1.0f;
    for (std::uint32_t y = 0; y < src.height; ++y)
        for (std::uint32_t x = 0; x < src.width; ++x)
            if (redExcessOf(at(src, x, y)) > level)
                worst = std::max(worst, redExcessOf(at(out, x, y)));
    return worst;
}

} // namespace

TEST_CASE("a corrected pupil comes back with no red rim around it") {
    // The user-visible defect this pass exists to remove: the correction killed the glow's core and
    // left a bright red ring at the radius where the glow met the iris. The strict gate scores
    // those transition pixels near zero, so they kept nearly all of their red while the disc inside
    // them went black -- which is what draws the ring. Verified against the real flash corpus: on a
    // photograph whose glow ends at radius 33, the shipped gate left excess +0.10 at r=34 and
    // +0.16 at r=35, and the eye reads that as a rim, not as a fade.
    Image src = glowWithFadedRim(64, 32.0, 32.0, 9.0, 13.0);
    const Selection scope = Selection::ellipse(64, 64, {12.0, 12.0, 40.0, 40.0});

    RedEyeParams shipped = flashParams();       // hysteresis on (rimReach derived from the tip)
    RedEyeParams bare = flashParams();
    bare.rimReach = 0.0;                        // ... and off, which is the old behaviour exactly

    const Image withRim = applied(src, scope, bare);
    const Image fixed = applied(src, scope, shipped);

    // The level is the measurement, not a round number: on the real photograph the ring the
    // shipped gate left measured +0.16 at its brightest and +0.10 one pixel in. So the claim is
    // that every pixel at least as red as that ring now comes back neutral -- which is the same
    // sentence as "the ring is gone", stated so that a future change cannot satisfy it by moving
    // the ring a pixel. The check runs over every pixel of the figure, not a sampled radius.
    constexpr float kRingLevel = 0.15f;
    CHECK(worstResidualWhereSourceRedderThan(src, withRim, kRingLevel) > 0.10f); // the defect ...
    CHECK(worstResidualWhereSourceRedderThan(src, fixed, kRingLevel) < 0.02f);   // ... and gone
    // The pass only ever removes red: no pixel anywhere comes back redder than it went in.
    bool neverRedder = true;
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 64; ++x)
            neverRedder = neverRedder && redExcessOf(at(fixed, x, y)) <=
                                             redExcessOf(at(src, x, y)) + 0.01f;
    CHECK(neverRedder);

    // ... and the fade is corrected toward the IRIS, not toward neutral. This is the sharper
    // property and the one that actually removes the ring: a blue-green iris measures excess
    // -0.105 in the corpus, so a band corrected to a perfect neutral 0.00 still reads as a warm
    // circle drawn on it. Only landing on the iris's own tone makes the boundary disappear.
    float fadeSum = 0.0f;
    int fadeCount = 0;
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 64; ++x) {
            const double dx = (x + 0.5) - 32.0;
            const double dy = (y + 0.5) - 32.0;
            const double d = std::sqrt(dx * dx + dy * dy);
            if (d >= 10.5 && d < 12.5) { // the outer half of the fade: iris with a glow over it
                fadeSum += redExcessOf(at(fixed, x, y));
                ++fadeCount;
            }
        }
    REQUIRE(fadeCount > 0);
    const float fadeMean = fadeSum / static_cast<float>(fadeCount);
    CHECK(fadeMean < 0.0f); // ... past neutral, which is the whole point
    CHECK(std::abs(fadeMean - redExcessOf(kDarkIris)) < 0.05f);

    // The core is corrected identically either way -- the pass adds the rim, it does not change
    // what the strict gate already did.
    CHECK(at(fixed, 32, 32) == at(withRim, 32, 32));
    CHECK(lumOf(at(fixed, 32, 32)) < 0.15f);
}

TEST_CASE("a soft scope edge cannot draw a ring of its own across the glow") {
    // The other half of the rim, and the one the app actually hit: the scope's coverage used to
    // SCALE the correction, so wherever the brush's own shoulder crossed live glow it left a
    // half-corrected annulus -- a red ring drawn by the tool, at the radius of the ring the user
    // dragged. Since the tool asks you to size the ring to the pupil, that is where the shoulder
    // always lands. In this mode coverage is a GATE.
    Image src = solid(64, 64, kDarkIris);
    paintDisc(src, 32.0, 32.0, 20.0, kFlashPupil); // glow wider than the scope below
    // A scope whose feathered edge falls right across the glow, exactly like a brush ring lined up
    // a little inside the red disc.
    const Selection scope = Selection::ellipse(64, 64, {17.0, 17.0, 30.0, 30.0}).feathered(4.0);
    const Image out = applied(src, scope, flashParams());

    // Every glow pixel the scope meaningfully covers comes back corrected -- not scaled down by
    // how much of the brush's shoulder happened to reach it.
    float worstCovered = -1.0f;
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 64; ++x)
            if (scope.at(x, y) >= 128 && redExcessOf(at(src, x, y)) > 0.15f)
                worstCovered = std::max(worstCovered, redExcessOf(at(out, x, y)));
    CHECK(worstCovered < 0.02f);

    // And nothing outside the scope moved, so the gate widened no one's authority to write.
    bool outsideUntouched = true;
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 64; ++x)
            if (scope.at(x, y) == 0)
                outsideUntouched = outsideUntouched && at(out, x, y) == at(src, x, y);
    CHECK(outsideUntouched);
}

TEST_CASE("the flash ring is a promise: the scope reaches what the ring shows") {
    // The tip is HARD in this mode. At the shipped 0.92 a 70 px ring's coverage was already past
    // half by r = 33.3 and gone by r = 34.5 -- so the outer ~2 px of the circle the user dragged
    // corrected weakly or not at all, and lining the ring up with the glow (what the tool asks for)
    // left precisely the glow's edge behind. A pixel rim, caused by the ring meaning less than it
    // showed. MaskStroke still anti-aliases at hardness 1, so this costs no jaggedness.
    using mosaic::ui::RedEyeOptions;
    RedEyeOptions flash;
    flash.mode = RedEyeMode::Flash;
    flash.size = 70.0;
    const auto tip = mosaic::ui::redEyeStrokeParams(flash);
    CHECK(tip.hardness == doctest::Approx(1.0));

    // Painted for real, the scope has to still be solid a pixel inside the ring's own radius.
    mosaic::core::brush::MaskStroke stroke;
    stroke.begin(120, 120, tip, {60.0, 60.0});
    stroke.end();
    const Selection scope = stroke.toSelection();
    REQUIRE_FALSE(scope.isEmpty());
    CHECK(scope.at(60, 60 - 34) >= 128); // r = 34 of a 35 px ring: solidly inside the promise
    CHECK(scope.at(60, 60 - 40) == 0);   // ... and it still stops where the ring does
}

TEST_CASE("the rim pass cannot bleach the iris the glow sits in") {
    // Hysteresis is only ever as safe as what it refuses to admit. The permissive ramp is wide
    // enough to catch a glow's fading rim, so the thing that keeps it off the iris is PURITY --
    // the fraction of a pixel's red that is excess red. A rim pixel spends its red on being red
    // (0.7-0.9); an iris, even a warm brown one, spends it on being bright (0.2-0.4). This pins
    // that the difference is doing the work, on the iris colours that actually surround a red-eye.
    Image src = glowWithFadedRim(64, 32.0, 32.0, 9.0, 13.0);
    paintRect(src, 0, 0, 64, 8, kWarmSkin);   // the reddest ordinary skin, right in the scope
    paintRect(src, 0, 56, 64, 64, kPaleSkin);
    const Selection scope = Selection::rectangle(64, 64, {0.0, 0.0, 64.0, 64.0});
    const Image out = applied(src, scope, flashParams());

    bool skinUntouched = true;
    for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 64; ++x)
            skinUntouched = skinUntouched && at(out, x, y) == at(src, x, y);
    for (std::uint32_t y = 56; y < 64; ++y)
        for (std::uint32_t x = 0; x < 64; ++x)
            skinUntouched = skinUntouched && at(out, x, y) == at(src, x, y);
    CHECK(skinUntouched);

    // ... and the iris the glow fades into, which is closer to the core than the skin is and
    // therefore has the most support behind it, is byte-identical too.
    CHECK(at(out, 32, 50) == at(src, 32, 50));
    CHECK(at(out, 50, 32) == at(src, 50, 32));
}

// ------------------------------------------------------------------------------------------------
// Tier 1 -- idempotence and no-ops
// ------------------------------------------------------------------------------------------------

TEST_CASE("running the red-eye correction twice over a hard scope is a no-op the second time") {
    // A hard-edged scope over hard-edged pixels: every covered pixel is either fully corrected or
    // fully rejected, so the second pass has nothing left with a non-zero glow score and the module
    // reports NO patch at all -- which is what makes the tool safe to click repeatedly.
    Image src = solid(32, 32, kPaleSkin);
    paintDisc(src, 16.5, 16.5, 6.0, kFlashPupil);
    const Selection scope = Selection::rectangle(32, 32, {6.0, 6.0, 21.0, 21.0});

    const Image once = applied(src, scope, flashParams());
    CHECK(totalDelta(src, once) > 0); // the first pass definitely did something
    CHECK(mosaic::core::retouchEye(once, scope, flashParams()).empty());
}

TEST_CASE("repeated passes over a feathered scope converge instead of darkening without end") {
    // A feathered scope has partial-coverage pixels, so a second pass is not a no-op by
    // construction the way the hard-scope case is. What must hold is that it CONVERGES -- each pass
    // moves strictly less than the one before -- rather than grinding the pupil toward black.
    Image src = solid(40, 40, kPaleSkin);
    paintDisc(src, 20.5, 20.5, 7.0, kFlashPupil);
    // Radii 9 with a 2 px feather: the ramp crosses the pupil's rim (partial coverage over RED
    // pixels, which is where accrual would show) while the core still reaches full coverage, so the
    // fixed-point claim below is about a genuinely fully-corrected pixel.
    const Selection scope = Selection::ellipse(40, 40, {11.5, 11.5, 18.0, 18.0}).feathered(2.0);

    const Image pass1 = applied(src, scope, flashParams());
    const Image pass2 = applied(pass1, scope, flashParams());
    const Image pass3 = applied(pass2, scope, flashParams());

    const long long d1 = totalDelta(src, pass1);
    const long long d2 = totalDelta(pass1, pass2);
    const long long d3 = totalDelta(pass2, pass3);
    CHECK(d1 > 0);
    CHECK(d2 < d1); // strictly less every time: the operator contracts, it does not accumulate
    CHECK(d3 <= d2);
    // The fully-covered core is a byte-exact fixed point -- it was corrected to a neutral, and a
    // neutral scores zero, so no later pass can touch it again.
    CHECK(at(pass2, 20, 20) == at(pass1, 20, 20));
    CHECK(at(pass3, 20, 20) == at(pass1, 20, 20));
}

TEST_CASE("an image with nothing red in it lands no undo step at all") {
    const Image blueGrey = solid(24, 24, {0.42f, 0.48f, 0.55f, 1.0f});
    const Selection scope = Selection::rectangle(24, 24, {2.0, 2.0, 20.0, 20.0});
    CHECK(mosaic::core::retouchEye(blueGrey, scope, flashParams()).empty());
    CHECK(mosaic::core::retouchEye(blueGrey, scope, scleraParams()).empty());

    // An empty scope, a mismatched scope and an empty image are all no-ops rather than crashes.
    CHECK(mosaic::core::retouchEye(blueGrey, Selection{}, flashParams()).empty());
    CHECK(mosaic::core::retouchEye(blueGrey, Selection(8, 8), flashParams()).empty());
    CHECK(mosaic::core::retouchEye(Image{}, scope, flashParams()).empty());
}

// ------------------------------------------------------------------------------------------------
// Tier 2 -- sclera de-redding and vein suppression
// ------------------------------------------------------------------------------------------------

namespace {

// A synthetic bloodshot sclera: blue-white, with 1 px vertical vessels every 4 px. That spacing and
// width is what a real conjunctival injection looks like at a face-filling crop, and it is what the
// base/detail split has to be able to separate.
[[nodiscard]] Image syntheticSclera(std::uint32_t w, std::uint32_t h) {
    Image img = solid(w, h, kSclera);
    for (std::uint32_t x = 2; x < w; x += 4)
        paintRect(img, x, 0, x + 1, h, kVessel);
    return img;
}

} // namespace

TEST_CASE("the scope's reference tone is its least-red pixels, not a fixed white") {
    const Image img = syntheticSclera(48, 48);
    const Selection scope = Selection::rectangle(48, 48, {0.0, 0.0, 48.0, 48.0});
    const ColorF ref = mosaic::core::scleraReference(img, scope);
    REQUIRE(ref.a > 0.0f);
    // The reference lands on the sclera, not on a vessel and not on pure white -- harmonization
    // pulls toward the neighbourhood's own tone, which is what keeps it from being a flat wash.
    CHECK(ref.r == doctest::Approx(kSclera.r).epsilon(0.05));
    CHECK(ref.g == doctest::Approx(kSclera.g).epsilon(0.05));
    CHECK(ref.b == doctest::Approx(kSclera.b).epsilon(0.05));
    CHECK(lumOf(ref) < 1.0f);

    // A scope over nothing usable has no reference at all (alpha 0), rather than a made-up white.
    CHECK(mosaic::core::scleraReference(img, Selection{}).a == 0.0f);
}

TEST_CASE("the reference tone is the least-red of the BRIGHT pixels, never the lashes") {
    // The regression that made the whole tier under-correct. Red excess falls to zero on anything
    // dark, so ranking the brushed pixels by redness alone elects the lid shadow and the lashes --
    // which are the LEAST red thing in any real photograph of a bloodshot eye. Measured on a real
    // conjunctivitis frame, that returned luminance 0.51 for an eye whose sclera measures 0.87,
    // and since every downstream term is relative to this tone, the luminance lift then had
    // nothing to lift toward and the veins came out grey. A brush over a real eye ALWAYS catches
    // some of this, so the figure includes it deliberately.
    Image img = syntheticSclera(48, 48);
    paintRect(img, 0, 0, 48, 16, kLidShadow); // a third of the scope is lash / lid shadow
    const Selection scope = Selection::rectangle(48, 48, {0.0, 0.0, 48.0, 48.0});

    REQUIRE(redExcessOf(kLidShadow) < redExcessOf(kSclera)); // it really is the least-red thing
    const ColorF ref = mosaic::core::scleraReference(img, scope);
    REQUIRE(ref.a > 0.0f);
    CHECK(lumOf(ref) > 0.75f);                       // the sclera ...
    CHECK(lumOf(ref) > 2.0f * lumOf(kLidShadow));    // ... not the shadow it is brushed with
    CHECK(ref.r == doctest::Approx(kSclera.r).epsilon(0.08));
}

TEST_CASE("how plausible a tone is as an eye white is what licenses whitening it") {
    using mosaic::core::scleraWhiteness;
    // A sclera is barely saturated and barely red, and that is the whole test: the licence is a
    // fixed-threshold function of ONE COLOUR. It asks nothing about the image, finds nothing in
    // it, and is the mechanism by which the tool declines to invent a white it cannot see.
    CHECK(scleraWhiteness(kSclera) == doctest::Approx(1.0f));
    CHECK(scleraWhiteness({0.79f, 0.79f, 0.87f, 1.0f}) == doctest::Approx(1.0f)); // measured, real
    CHECK(scleraWhiteness(kBloodPatch) == doctest::Approx(0.0f));
    CHECK(scleraWhiteness(kVessel) == doctest::Approx(0.0f));
    CHECK(scleraWhiteness({0.0f, 0.0f, 0.0f, 0.0f}) == 0.0f); // no tone at all licenses nothing
}

TEST_CASE("vein suppression replaces the vessel with the white it lies on") {
    const Image src = syntheticSclera(48, 48);
    // The scope avoids the outermost columns so the vessel we measure has real neighbours on both
    // sides -- the local white is a neighbourhood statistic, and a boundary vessel is a different
    // (and less interesting) case.
    const Selection scope = Selection::rectangle(48, 48, {8.0, 8.0, 32.0, 32.0});

    RedEyeParams p = scleraParams();
    p.suppressVeins = true;
    p.veinRadius = 6.0;
    p.amount = 1.0;
    p.vascularityFloor = 0.0; // "everything you have" -- what the user actually reaches for
    const Image out = applied(src, scope, p);

    const ColorF vesselIn = at(src, 22, 24);  // x = 22 is a vessel column (2 + 4k)
    const ColorF vesselOut = at(out, 22, 24);
    const ColorF scleraIn = at(src, 24, 24);  // x = 24 is sclera
    const ColorF scleraOut = at(out, 24, 24);
    REQUIRE(redExcessOf(vesselIn) > 0.3f); // sanity: the figure really is a red vessel on white

    // The vessel loses its colour ...
    CHECK(redExcessOf(vesselOut) < 0.1f * redExcessOf(vesselIn));
    CHECK(satOf(vesselOut) < 0.25f * satOf(vesselIn));
    // ... AND its darkness, which is the half that was missing. Attenuating only the chroma left a
    // vessel sitting at its own luminance on a white sclera -- a grey streak where a red one had
    // been, which is what "it only makes the veins a dull colour" was describing. The vessel is
    // REPLACED by the white beside it, so it has to come back within reach of that white.
    const float contrastIn = lumOf(scleraIn) - lumOf(vesselIn);
    const float contrastOut = lumOf(scleraOut) - lumOf(vesselOut);
    REQUIRE(contrastIn > 0.3f);
    CHECK(contrastOut < 0.2f * contrastIn);
    // It never overshoots past the white it was pulled toward: the replacement is that white's own
    // tone at the brighter of the two luminances, so a vessel cannot come back BRIGHTER than the
    // sclera around it (which would read as a glowing worm, the opposite artifact).
    CHECK(lumOf(vesselOut) <= lumOf(scleraOut) + 0.01f);
}

TEST_CASE("the vascularity floor still leaves a believable vessel at the shipped default") {
    // The other half of the same coin: at the DEFAULT settings the eye must keep reading as an eye.
    // "Remove the veins" is what Amount 100 / Keep-veins 0 is for; the default under-corrects, and
    // this pins that the default still leaves a vessel visible as a vessel -- both its colour and
    // its darkness -- so the fix above cannot quietly become a bleach.
    const Image src = syntheticSclera(48, 48);
    const Selection scope = Selection::rectangle(48, 48, {8.0, 8.0, 32.0, 32.0});
    const Image out = applied(src, scope, scleraParams()); // amount 0.55, floor 0.35

    const ColorF vesselIn = at(src, 22, 24);
    const ColorF vesselOut = at(out, 22, 24);
    const ColorF scleraOut = at(out, 24, 24);
    CHECK(redExcessOf(vesselOut) < redExcessOf(vesselIn));   // it did fade ...
    CHECK(redExcessOf(vesselOut) > 0.15f * redExcessOf(vesselIn)); // ... and it is still there
    CHECK(lumOf(vesselOut) < lumOf(scleraOut));              // still reads as a vessel, not a wash
}

TEST_CASE("an eye that shows no white anywhere is barely touched, not confidently greyed") {
    // The honesty property, and the reason the whitening licence exists. A solid subconjunctival
    // hemorrhage has no healthy sclera in the frame to harmonize toward, so there is no reference
    // that is a white -- and desaturating it toward a plausible-sclera chroma without one produces
    // a flat, confident grey patch that looks nothing like an eye and hides what the photograph
    // actually shows. Reconstructing that eye is Tier 3, which is deliberately NOT built; until it
    // is, the correct answer here is "very little".
    const Image blood = solid(48, 48, kBloodPatch);
    const Selection scope = Selection::rectangle(48, 48, {8.0, 8.0, 32.0, 32.0});

    RedEyeParams hard = scleraParams();
    hard.amount = 1.0;
    hard.vascularityFloor = 0.0;
    const Image out = applied(blood, scope, hard);
    const ColorF c = at(out, 24, 24);
    CHECK(satOf(c) > 0.8f * satOf(kBloodPatch));       // its colour survives ...
    CHECK(lumOf(c) < lumOf(kBloodPatch) + 0.05f);      // ... and it is not lifted toward a white

    // Whereas the same settings on an eye that DOES show white between its vessels correct hard --
    // the licence is what tells those two cases apart, and it is the tone, not a detector.
    const Image bloodshot = syntheticSclera(48, 48);
    const ColorF vessel = at(applied(bloodshot, scope, hard), 22, 24);
    CHECK(satOf(vessel) < 0.25f * satOf(kVessel));
}

TEST_CASE("harmonize-only de-reddens the sclera without running the frequency split") {
    const Image src = syntheticSclera(48, 48);
    const Selection scope = Selection::rectangle(48, 48, {8.0, 8.0, 32.0, 32.0});

    RedEyeParams p = scleraParams();
    p.suppressVeins = false;
    p.amount = 1.0;
    const Image out = applied(src, scope, p);

    CHECK(redExcessOf(at(out, 22, 24)) < redExcessOf(at(src, 22, 24)));
    // Without the split, a pure-sclera pixel scores no redness at all, so it is left exactly alone
    // -- harmonization is targeted, not a wash over the whole brushed region.
    CHECK(at(out, 24, 24) == at(src, 24, 24));
}

TEST_CASE("the vascularity floor at 100% makes the sclera mode an exact no-op") {
    // The floor is the tool's safety property: it binds the desaturation, the luminance lift AND
    // the vein attenuation, so "keep everything" means the pixels are not touched at all -- however
    // hard Amount is pushed. A change that lets any half of the effect escape the floor fails here.
    const Image src = syntheticSclera(48, 48);
    const Selection scope = Selection::rectangle(48, 48, {8.0, 8.0, 32.0, 32.0});

    RedEyeParams p = scleraParams();
    p.amount = 1.0;
    p.vascularityFloor = 1.0;
    CHECK(mosaic::core::retouchEye(src, scope, p).empty());
}

TEST_CASE("a higher vascularity floor always leaves more of the eye's own colour") {
    const Image src = syntheticSclera(48, 48);
    const Selection scope = Selection::rectangle(48, 48, {8.0, 8.0, 32.0, 32.0});

    RedEyeParams low = scleraParams();
    low.amount = 1.0;
    low.vascularityFloor = 0.10;
    RedEyeParams high = low;
    high.vascularityFloor = 0.70;

    const float lowRed = redExcessOf(at(applied(src, scope, low), 22, 24));
    const float highRed = redExcessOf(at(applied(src, scope, high), 22, 24));
    CHECK(highRed > lowRed);
    CHECK(lowRed > 0.0f); // even at a 10% floor, some vascularity survives
}

TEST_CASE("repeated sclera passes converge instead of bleaching the eye") {
    // The workflow the defect was reported from: "multiple uses of the tool at 100% amount and keep
    // veins at 0%". Each pass recomputes the reference and the local white from what the last one
    // left, so the danger is a runaway -- an eye that gets whiter every click until it is a plastic
    // ball. What must hold is that the operator CONTRACTS. Measured on the real conjunctivitis
    // frame the report came from, the mean absolute move per pass runs 2.44 -> 0.31 -> 0.10.
    const Image src = syntheticSclera(64, 64);
    const Selection scope = Selection::rectangle(64, 64, {8.0, 8.0, 48.0, 48.0});
    RedEyeParams p = scleraParams();
    p.amount = 1.0;
    p.vascularityFloor = 0.0;

    const Image pass1 = applied(src, scope, p);
    const Image pass2 = applied(pass1, scope, p);
    const Image pass3 = applied(pass2, scope, p);
    const long long d1 = totalDelta(src, pass1);
    const long long d2 = totalDelta(pass1, pass2);
    const long long d3 = totalDelta(pass2, pass3);
    CHECK(d1 > 0);
    CHECK(d2 < d1);
    CHECK(d3 <= d2);

    // ... and the eye it converges TO is still an eye: the sclera keeps its own faint tint rather
    // than grinding down to a flat neutral, because every term aims at a tone sampled from the
    // photograph and none of them aims at white.
    const ColorF sclera = at(pass3, 32, 32);
    CHECK(lumOf(sclera) > 0.6f);
    CHECK(satOf(sclera) > 0.01f);
}

TEST_CASE("the sclera mode leaves pixels outside the painted scope byte-identical") {
    const Image src = syntheticSclera(48, 48);
    const Selection scope = Selection::rectangle(48, 48, {16.0, 16.0, 16.0, 16.0});
    const Image out = applied(src, scope, scleraParams());

    bool outsideUntouched = true;
    for (std::uint32_t y = 0; y < 48; ++y)
        for (std::uint32_t x = 0; x < 48; ++x)
            if (x < 16 || x >= 32 || y < 16 || y >= 32)
                outsideUntouched = outsideUntouched && at(out, x, y) == at(src, x, y);
    CHECK(outsideUntouched);
    CHECK(totalDelta(src, out) > 0); // ... and it did something inside the scope
}

// ------------------------------------------------------------------------------------------------
// The scoping model (ui/red_eye_gesture.hpp)
// ------------------------------------------------------------------------------------------------

TEST_CASE("the gesture's scope is the painted stroke clipped by the document's own selection") {
    using mosaic::ui::redEyeScope;

    const Selection stroke = Selection::rectangle(32, 32, {4.0, 4.0, 16.0, 16.0});

    // No document selection means the whole document is editable, so the stroke stands alone.
    CHECK(redEyeScope(stroke, Selection{}) == stroke);

    // An active selection clips the correction exactly as it clips a brush stroke (§2.4).
    const Selection docSel = Selection::rectangle(32, 32, {12.0, 4.0, 16.0, 16.0});
    const Selection clipped = redEyeScope(stroke, docSel);
    REQUIRE_FALSE(clipped.isEmpty());
    CHECK(clipped.at(8, 8) == 0);      // in the stroke, outside the selection
    CHECK(clipped.at(14, 8) == 255);   // in both
    CHECK(clipped.at(24, 8) == 0);     // in the selection, outside the stroke

    // Painting entirely outside your own selection lands nothing at all -- not an empty edit.
    const Selection elsewhere = Selection::rectangle(32, 32, {24.0, 24.0, 6.0, 6.0});
    CHECK(redEyeScope(stroke, elsewhere).isEmpty());
    CHECK(redEyeScope(Selection{}, docSel).isEmpty());
}

TEST_CASE("the scope is carried onto the layer's own pixel grid, transform and all") {
    using mosaic::ui::redEyeScopeOnLayer;

    mosaic::core::RasterLayer layer(1, "raster", 32, 32);
    const Selection docScope = Selection::rectangle(32, 32, {4.0, 4.0, 8.0, 8.0});

    // Untransformed and document-sized: the 1:1 fast path hands the coverage straight through.
    const Selection same = redEyeScopeOnLayer(layer, docScope);
    REQUIRE_FALSE(same.isEmpty());
    CHECK(same.width() == 32);
    CHECK(same.at(6, 6) == 255);
    CHECK(same.at(20, 20) == 0);

    // Translate the layer by +10 px: a document pixel at (14,14) is layer pixel (4,4), so the
    // scope has to move the OTHER way on the layer's grid or the correction lands in the wrong
    // place -- the bug this mapping exists to prevent.
    layer.setTransform(mosaic::common::Affine2D::translation(10.0, 10.0));
    const Selection moved = redEyeScopeOnLayer(layer, docScope);
    REQUIRE_FALSE(moved.isEmpty());
    CHECK(moved.at(6, 6) == 0);
    CHECK(moved.at(0, 0) == 255); // layer (0,0) sits at document (10,10), inside the scope

    // A layer with nothing under the scope produces no scope at all, so no command is pushed.
    layer.setTransform(mosaic::common::Affine2D::translation(1000.0, 1000.0));
    CHECK(redEyeScopeOnLayer(layer, docScope).isEmpty());
}

TEST_CASE("the eye tool's options map onto core parameters and a scope-only stroke tip") {
    using mosaic::ui::RedEyeOptions;

    RedEyeOptions o;
    o.mode = RedEyeMode::Sclera;
    o.size = 50.0;
    o.amount = 80.0;
    o.vascularity = 25.0;
    o.strength = 100.0;
    o.darken = 50.0;

    const RedEyeParams p = mosaic::ui::redEyeParams(o);
    CHECK(p.mode == RedEyeMode::Sclera);
    CHECK(p.amount == doctest::Approx(0.80));
    CHECK(p.vascularityFloor == doctest::Approx(0.25));
    CHECK(p.darken == doctest::Approx(0.50));
    // The vein radius is derived from the tip, never exposed: a bigger brush is a bigger eye.
    CHECK(p.veinRadius == doctest::Approx(6.0));
    // ... and so is the flash mode's rim reach, from the same tip and by the same argument.
    CHECK(p.rimReach == doctest::Approx(5.0));
    RedEyeOptions tiny = o;
    tiny.size = 4.0;
    CHECK(mosaic::ui::redEyeParams(tiny).rimReach == doctest::Approx(2.0)); // clamped, never 0

    // The scope stroke is a SCOPE, not paint: full flow and opacity, so crossing it twice within
    // one gesture cannot deepen the correction (Strength / Amount own that).
    const auto tip = mosaic::ui::redEyeStrokeParams(o);
    CHECK(tip.diameter == doctest::Approx(50.0));
    CHECK(tip.flow == 1.0);
    CHECK(tip.opacity == 1.0);
    // The flash mode wants a crisp scope so the correction does not creep onto the iris; the sclera
    // mode wants a soft one so a de-redded patch blends into the rest of the white.
    RedEyeOptions flash = o;
    flash.mode = RedEyeMode::Flash;
    CHECK(mosaic::ui::redEyeStrokeParams(flash).hardness > tip.hardness);
}

TEST_CASE("the two registered eye tools map onto the two shipping tiers and nothing else") {
    using mosaic::ui::redEyeModeFor;
    using mosaic::ui::ToolId;
    CHECK(redEyeModeFor(ToolId::RedEye) == std::optional<RedEyeMode>(RedEyeMode::Flash));
    CHECK(redEyeModeFor(ToolId::RedEyeSclera) == std::optional<RedEyeMode>(RedEyeMode::Sclera));
    CHECK_FALSE(redEyeModeFor(ToolId::Brush).has_value());
    CHECK_FALSE(redEyeModeFor(ToolId::InpaintBrush).has_value());
}
