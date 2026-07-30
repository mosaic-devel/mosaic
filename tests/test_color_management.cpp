#include "core/color_management.hpp"

#include <doctest/doctest.h>

#include <cmath>

// lcms2-backed working-space <-> CIELAB transforms + the gamut probe (PLAN S12-b).
namespace {

using mosaic::core::ColorEngine;
using mosaic::core::ColorSpace;
using mosaic::core::Lab;
using mosaic::core::RgbF;

bool near(float a, float b, float eps) {
    return std::abs(a - b) <= eps;
}

} // namespace

TEST_CASE("sRGB <-> Lab: known anchor values (D50 PCS)") {
    const ColorEngine eng(ColorSpace::SRGB);

    // White and black pin the L axis; both are neutral (a, b ~ 0).
    const Lab white = eng.toLab({255, 255, 255, 255});
    CHECK(near(white.l, 100.0F, 0.3F));
    CHECK(near(white.a, 0.0F, 0.3F));
    CHECK(near(white.b, 0.0F, 0.3F));
    const Lab black = eng.toLab({0, 0, 0, 255});
    CHECK(near(black.l, 0.0F, 0.3F));

    // sRGB pure red in CIELAB-D50 (Bradford-adapted): the textbook ~ (54.3, 80.8, 69.9).
    const Lab red = eng.toLab({255, 0, 0, 255});
    CHECK(near(red.l, 54.3F, 1.5F));
    CHECK(near(red.a, 80.8F, 2.0F));
    CHECK(near(red.b, 69.9F, 2.5F));

    // Mid grey is neutral at L ~ 53.6 (the L* of 50% sRGB).
    const Lab grey = eng.toLab({128, 128, 128, 255});
    CHECK(near(grey.a, 0.0F, 0.3F));
    CHECK(near(grey.b, 0.0F, 0.3F));
}

TEST_CASE("round-trip: rgb -> Lab -> rgb is identity within rounding") {
    const ColorEngine eng(ColorSpace::SRGB);
    const std::uint8_t samples[] = {0, 51, 128, 204, 255};
    for (std::uint8_t r : samples)
        for (std::uint8_t g : samples)
            for (std::uint8_t b : samples) {
                const mosaic::common::Color8 in{r, g, b, 255};
                const mosaic::common::Color8 out = eng.toRgbClamped(eng.toLab(in));
                CHECK(std::abs(int{in.r} - int{out.r}) <= 1);
                CHECK(std::abs(int{in.g} - int{out.g}) <= 1);
                CHECK(std::abs(int{in.b} - int{out.b}) <= 1);
            }
}

TEST_CASE("gamut probe: in-working-space colours pass, wild Lab values fail and clamp") {
    const ColorEngine eng(ColorSpace::SRGB);

    // Anything that came *from* the working space must be in gamut.
    const RgbF back = eng.toRgbUnclamped(eng.toLab({30, 200, 90, 255}));
    CHECK(ColorEngine::inGamut(back));

    // A super-saturated green-ish Lab lies far outside sRGB: probe fails, snap stays in range.
    const Lab wild{60.0F, -120.0F, 80.0F};
    CHECK_FALSE(ColorEngine::inGamut(eng.toRgbUnclamped(wild)));
    const mosaic::common::Color8 snapped = eng.toRgbClamped(wild);
    CHECK(snapped.r == 0); // a = -120 drives red far negative; the snap clamps it at the floor
    CHECK(ColorEngine::inGamut(eng.toRgbUnclamped(eng.toLab(snapped)))); // the snap IS in gamut
}

TEST_CASE("CMYK: the vendored FOGRA39 default loads and behaves like a press profile") {
    const ColorEngine eng(ColorSpace::SRGB);
    REQUIRE(eng.hasCmyk());

    // Working white ~ paper white (relative colorimetric + BPC): all inks near zero.
    const mosaic::core::Cmyk paper = eng.toCmyk({255, 255, 255, 255});
    CHECK(paper.c < 2.0F);
    CHECK(paper.m < 2.0F);
    CHECK(paper.y < 2.0F);
    CHECK(paper.k < 2.0F);

    // Black leans on the K plate, and total coverage respects the profile's 300% ink limit.
    const mosaic::core::Cmyk black = eng.toCmyk({0, 0, 0, 255});
    CHECK(black.k > 70.0F);
    CHECK(black.c + black.m + black.y + black.k <= 305.0F);

    // Round-trip on a muted, printable colour stays close (CMYK gamuts are small; be generous).
    const mosaic::common::Color8 muted{170, 130, 90, 255};
    const mosaic::common::Color8 back = eng.cmykToRgb(eng.toCmyk(muted));
    CHECK(std::abs(int{muted.r} - int{back.r}) <= 10);
    CHECK(std::abs(int{muted.g} - int{back.g}) <= 10);
    CHECK(std::abs(int{muted.b} - int{back.b}) <= 10);

    // Garbage data is rejected and leaves the working transforms untouched.
    ColorEngine eng2(ColorSpace::SRGB);
    const char junk[16] = {};
    CHECK_FALSE(eng2.loadCmykProfile(junk, sizeof junk));
    CHECK(eng2.hasCmyk());
}

TEST_CASE("S12-c file loading: CMYK from a path works; non-RGB working profiles are rejected") {
    ColorEngine eng(ColorSpace::SRGB);
    const std::string cmykPath = std::string(MOSAIC_ICC_DIR) + "/ISOcoated_v2_300_eci.icc";
    CHECK(eng.loadCmykProfileFile(cmykPath.c_str()));
    CHECK(eng.hasCmyk());

    // A CMYK profile is not a working space: the load is rejected and the engine is untouched --
    // the name still reports the built-in enum and the Lab anchor still holds.
    CHECK_FALSE(eng.loadWorkingProfileFile(cmykPath.c_str()));
    CHECK(eng.workingName() == std::string(mosaic::core::colorSpaceName(ColorSpace::SRGB)));
    CHECK(near(eng.toLab({255, 255, 255, 255}).l, 100.0F, 0.3F));

    // Missing files are rejected the same way.
    CHECK_FALSE(eng.loadWorkingProfileFile("/nonexistent/profile.icc"));
    CHECK(eng.hasCmyk()); // and none of it disturbed the CMYK transforms
}

TEST_CASE("wider working spaces differ: AdobeRGB red is not sRGB red") {
    const ColorEngine srgb(ColorSpace::SRGB);
    const ColorEngine argb(ColorSpace::AdobeRGB);
    const Lab a = srgb.toLab({255, 0, 0, 255});
    const Lab b = argb.toLab({255, 0, 0, 255});
    CHECK(std::abs(a.a - b.a) > 2.0F); // AdobeRGB's red primary is more saturated

    // The sRGB-out-of-gamut green from above fits inside Rec.2020.
    const ColorEngine rec(ColorSpace::Rec2020);
    CHECK(ColorEngine::inGamut(rec.toRgbUnclamped(Lab{60.0F, -90.0F, 60.0F})));
    CHECK_FALSE(ColorEngine::inGamut(srgb.toRgbUnclamped(Lab{60.0F, -90.0F, 60.0F})));
}
