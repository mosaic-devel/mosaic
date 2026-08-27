// S32 non-destructive adjustment framework (docs/adjustment-layers.md): the typed parameter
// schema, the params-bag command, and the compositor math for every scalar kind. The math cases
// pin ANALYTIC invariants (identity-at-defaults byte-exactness, exposure doubling linear light,
// hue rotation landing on the complementary primary, threshold binarity, posterize level counts)
// rather than golden pixels -- the formulas are the spec, so the tests restate them.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common/image.hpp"
#include "core/adjustments.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/selection.hpp"
#include "render/compositor.hpp"
#include "render/stylize.hpp" // S34-a: High Pass rides the S35 family's reach table

using namespace mosaic;

namespace {

common::Image flatten(const core::Document& doc) {
    const render::CompositeResult r = render::composite(doc, {}, render::Backend::Cpu);
    REQUIRE(r.ok);
    return r.image;
}

common::Color8 px(const common::Image& img, std::uint32_t x, std::uint32_t y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

// Analytic IEC 61966-2-1 curves -- the reference the Exposure case computes its expectation with
// (the compositor's LUT pair interpolates the same curves, so a +-2/255 tolerance absorbs it).
double srgbDecode(double e) {
    return e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4);
}
double srgbEncode(double l) {
    return l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}

// Seed a 6x1 test card: black, white, mid-gray, and the three primaries -- enough spread that a
// non-identity transfer cannot slip past a byte comparison. (Document is not movable, so the
// caller constructs `doc(6, 1)` and this fills it.)
void seedTestCard(core::Document& doc) {
    auto base = doc.makeRaster("base", 6, 1);
    base->image().rgba = {0,   0,   0,   255, 255, 255, 255, 255, 128, 128, 128, 255,
                          255, 0,   0,   255, 0,   255, 0,   255, 0,   0,   255, 255};
    doc.root().addOnTop(std::move(base));
}

// Add an adjustment of `kind` with `params` on top of the document root.
core::AdjustmentLayer* addAdjustment(core::Document& doc, core::AdjustmentKind kind,
                                     std::map<std::string, double> params) {
    auto layer = doc.makeAdjustment("adj", kind);
    layer->params() = std::move(params);
    core::AdjustmentLayer* raw = layer.get();
    doc.root().addOnTop(std::move(layer));
    return raw;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// The typed parameter schema (core/adjustments.hpp)
// ---------------------------------------------------------------------------------------------

TEST_CASE("adjustment schema: every kind's table is well-formed") {
    using enum core::AdjustmentKind;
    for (const auto kind : {BrightnessContrast, Levels, Curves, Exposure, HueSaturation,
                            ColorBalance, Grayscale, Invert, Threshold, Posterize,
                            PhotometricMatch, GaussianBlur, BoxBlur, MotionBlur, RadialBlur,
                            SurfaceBlur, LensBlur, DofBlur, ShadowsHighlights, Defringe,
                            MatteRemoval, HazeRemoval, GradientMap, Vibrance, PhotoFilter,
                            HighPass}) {
        std::set<std::string> keys;
        for (const core::AdjustmentParamDesc& d : core::adjustmentParamSchema(kind)) {
            CAPTURE(d.key);
            CHECK(keys.insert(d.key).second); // keys unique within a kind
            CHECK(d.min < d.max);
            CHECK(d.def >= d.min);
            CHECK(d.def <= d.max);
            CHECK(d.step > 0.0);
            if (d.type == core::AdjustmentParamType::Choice) {
                // a Choice row's labels and range must agree: index range = [0, count-1]
                CHECK(d.choices != nullptr);
                CHECK(d.choiceCount >= 2);
                CHECK(d.min == 0.0);
                CHECK(d.max == static_cast<double>(d.choiceCount - 1));
            } else {
                CHECK(d.choices == nullptr);
            }
        }
    }
    // No knobs by design: Invert alone (parameterless). Curves declares exactly ONE row -- the
    // channel picker -- because an EMPTY schema is how the corner-panel arbiter decides a kind
    // has no editor; its curve knots are stored outside the schema on purpose (S34).
    CHECK(core::adjustmentParamSchema(Invert).empty());
    CHECK(core::adjustmentParamSchema(Curves).size() == 1);
    CHECK(core::adjustmentParamDesc(Curves, "channel") != nullptr);
    // Grayscale grew method + strength (S32 follow-up); the default must stay the old formula.
    const core::AdjustmentParamDesc* method = core::adjustmentParamDesc(Grayscale, "method");
    REQUIRE(method != nullptr);
    CHECK(method->def == static_cast<double>(core::GrayscaleMethod::Luma));
    // S34 closed the last gap: every kind now has real compositor math.
    for (const auto kind : {BrightnessContrast, Levels, Curves, Exposure, HueSaturation,
                            ColorBalance, Grayscale, Invert, Threshold, Posterize,
                            PhotometricMatch, GaussianBlur, BoxBlur, MotionBlur, RadialBlur,
                            SurfaceBlur, LensBlur, DofBlur, ShadowsHighlights, Defringe,
                            MatteRemoval, HazeRemoval, GradientMap, Vibrance, PhotoFilter,
                            HighPass}) {
        CAPTURE(core::adjustmentKindName(kind));
        CHECK(core::adjustmentImplemented(kind));
    }
    // The two S34 spatial kinds read past the pixel (a blurred mask / a resampled channel).
    CHECK(core::adjustmentIsSpatial(ShadowsHighlights));
    CHECK(core::adjustmentIsSpatial(Defringe));
    CHECK_FALSE(core::adjustmentIsSpatial(MatteRemoval));
    CHECK_FALSE(core::adjustmentIsSpatial(HazeRemoval));
    CHECK_FALSE(core::adjustmentIsSpatial(Curves));
    CHECK(core::adjustmentImplemented(Levels));
    CHECK(core::adjustmentImplemented(PhotometricMatch));
    // The S33 blur family: implemented, spatial, and menu-offered (visible defaults by design).
    for (const auto kind :
         {GaussianBlur, BoxBlur, MotionBlur, RadialBlur, SurfaceBlur, LensBlur, DofBlur}) {
        CHECK(core::adjustmentImplemented(kind));
        CHECK(core::adjustmentIsSpatial(kind));
    }
    CHECK_FALSE(core::adjustmentIsSpatial(Levels));
    CHECK_FALSE(core::adjustmentIsSpatial(PhotometricMatch));
    // S34-a: three per-pixel colour kinds and one windowed one. High Pass MUST be spatial or the
    // region/group-buffer machinery never asks for its reach and region != crop(full).
    CHECK(core::adjustmentIsSpatial(HighPass));
    CHECK_FALSE(core::adjustmentIsSpatial(GradientMap));
    CHECK_FALSE(core::adjustmentIsSpatial(Vibrance));
    CHECK_FALSE(core::adjustmentIsSpatial(PhotoFilter));
    // Gradient Map's RAMP is not a schema row (it is indexed stops in the same bag), so its table
    // holds only the Reverse toggle -- the Curves shape.
    CHECK(core::adjustmentParamSchema(GradientMap).size() == 1);
    CHECK(core::adjustmentParamDesc(GradientMap, "reverse") != nullptr);
    // Photo Filter's Custom colour rows are schema-declared even though the editor shows them as
    // one swatch: that is what earns them the clamp / seed / Reset machinery.
    for (const char* key : {"filter", "density", "preserve_luminosity", "color_r", "color_g",
                            "color_b"})
        CHECK(core::adjustmentParamDesc(PhotoFilter, key) != nullptr);
}

TEST_CASE("adjustment schema: seeding fills every declared default; reads clamp and fall back") {
    core::Document doc(1, 1);
    auto layer = doc.makeAdjustment("lv", core::AdjustmentKind::Levels);
    core::seedAdjustmentDefaults(*layer);
    const auto schema = core::adjustmentParamSchema(core::AdjustmentKind::Levels);
    CHECK(layer->params().size() == schema.size());
    for (const core::AdjustmentParamDesc& d : schema)
        CHECK(layer->params().at(d.key) == d.def);

    const core::AdjustmentParamDesc* gamma =
        core::adjustmentParamDesc(core::AdjustmentKind::Levels, "gamma");
    REQUIRE(gamma != nullptr);
    layer->params()["gamma"] = 99.0; // a hostile bag clamps to the declared range
    CHECK(core::adjustmentParamValue(*layer, *gamma) == gamma->max);
    layer->params().erase("gamma"); // an absent key falls back to the default
    CHECK(core::adjustmentParamValue(*layer, *gamma) == gamma->def);
    CHECK(core::adjustmentParamDesc(core::AdjustmentKind::Levels, "no_such_key") == nullptr);
}

// ---------------------------------------------------------------------------------------------
// SetAdjustmentParamsCommand (commands.hpp)
// ---------------------------------------------------------------------------------------------

TEST_CASE("SetAdjustmentParamsCommand: applies, undoes, and coalesces within a gesture") {
    core::Document doc(1, 1);
    core::AdjustmentLayer* adj =
        addAdjustment(doc, core::AdjustmentKind::Exposure, {{"exposure", 0.0}});
    const core::LayerId id = adj->id();

    // Two edits sharing a coalesce id merge into ONE undo step holding the newest bag.
    doc.commands().push(std::make_unique<core::SetAdjustmentParamsCommand>(
        id, std::map<std::string, double>{{"exposure", 1.0}}, "Edit Exposure", 7));
    doc.commands().push(std::make_unique<core::SetAdjustmentParamsCommand>(
        id, std::map<std::string, double>{{"exposure", 2.0}}, "Edit Exposure", 7));
    CHECK(doc.commands().undoCount() == 1);
    CHECK(adj->params().at("exposure") == 2.0);
    doc.commands().undo();
    CHECK(adj->params().at("exposure") == 0.0); // the pre-gesture bag, not the mid-drag one

    // A different coalesce id (or zero) starts a fresh step.
    doc.commands().push(std::make_unique<core::SetAdjustmentParamsCommand>(
        id, std::map<std::string, double>{{"exposure", 1.0}}, "Edit Exposure", 8));
    doc.commands().push(std::make_unique<core::SetAdjustmentParamsCommand>(
        id, std::map<std::string, double>{{"exposure", 2.0}}, "Edit Exposure", 9));
    CHECK(doc.commands().undoCount() == 2);
    doc.commands().push(std::make_unique<core::SetAdjustmentParamsCommand>(
        id, std::map<std::string, double>{{"exposure", 3.0}}, "Edit Exposure", 0));
    doc.commands().push(std::make_unique<core::SetAdjustmentParamsCommand>(
        id, std::map<std::string, double>{{"exposure", 4.0}}, "Edit Exposure", 0));
    CHECK(doc.commands().undoCount() == 4); // zero never merges
}

// ---------------------------------------------------------------------------------------------
// Compositor math (render/compositor.cpp applyAdjustment)
// ---------------------------------------------------------------------------------------------

TEST_CASE("a defaults bag is a byte-level no-op for the identity-default kinds") {
    using enum core::AdjustmentKind;
    core::Document plain(6, 1);
    seedTestCard(plain);
    const common::Image before = flatten(plain);
    for (const auto kind : {Levels, Exposure, HueSaturation, ColorBalance, Curves}) {
        CAPTURE(core::adjustmentKindName(kind));
        core::Document doc(6, 1);
    seedTestCard(doc);
        core::AdjustmentLayer* adj = addAdjustment(doc, kind, {});
        core::seedAdjustmentDefaults(*adj);
        CHECK(flatten(doc).rgba == before.rgba); // seeded defaults
        adj->params().clear();
        CHECK(flatten(doc).rgba == before.rgba); // and the empty bag reads the same defaults
    }
}

TEST_CASE("Levels: input window, gamma, and output remap") {
    // in_black at mid-gray: 128 collapses to (near) black, white stays white.
    core::Document doc(6, 1);
    seedTestCard(doc);
    addAdjustment(doc, core::AdjustmentKind::Levels, {{"in_black", 0.5}});
    common::Image out = flatten(doc);
    CHECK(px(out, 2, 0).r <= 2);
    CHECK(px(out, 1, 0) == common::Color8{255, 255, 255, 255});

    // gamma > 1 brightens the midtone: 128 -> pow(0.502, 1/2) = 0.709 -> ~181.
    core::Document doc2(6, 1);
    seedTestCard(doc2);
    addAdjustment(doc2, core::AdjustmentKind::Levels, {{"gamma", 2.0}});
    out = flatten(doc2);
    CHECK(px(out, 2, 0).r >= 179);
    CHECK(px(out, 2, 0).r <= 183);

    // Output remap squeezes black/white into [64, 191].
    core::Document doc3(6, 1);
    seedTestCard(doc3);
    addAdjustment(doc3, core::AdjustmentKind::Levels,
                  {{"out_black", 0.25}, {"out_white", 0.75}});
    out = flatten(doc3);
    CHECK(std::abs(static_cast<int>(px(out, 0, 0).r) - 64) <= 1);
    CHECK(std::abs(static_cast<int>(px(out, 1, 0).r) - 191) <= 1);
}

TEST_CASE("Levels at gamma 1 is exactly the linear range remap, pow or no pow") {
    // The gamma slider is the one most people leave alone, so `pow(t, 1)` was the everyday case --
    // three powf calls per pixel to multiply by one. The guard that skips them is only sound
    // because pow(x, 1) IS x (C99 F.10.4.4), which makes the whole adjustment a straight-line map
    // from the input window onto the output window. Checked against that closed form rather than
    // against the pow path, so it pins what the adjustment MEANS at gamma 1 and would catch a
    // guard that skipped the wrong term.
    const double inB = 0.2, inW = 0.9, outB = 0.1, outW = 0.8;
    core::Document doc(6, 1);
    seedTestCard(doc);
    addAdjustment(doc, core::AdjustmentKind::Levels,
                  {{"in_black", inB}, {"in_white", inW}, {"gamma", 1.0},
                   {"out_black", outB}, {"out_white", outW}});
    const common::Image out = flatten(doc);
    const common::Image before = [] {
        core::Document d(6, 1);
        seedTestCard(d);
        return flatten(d);
    }();
    for (std::uint32_t x = 0; x < 6; ++x) {
        const double v = px(before, x, 0).r / 255.0;
        const double t = std::clamp((v - inB) / (inW - inB), 0.0, 1.0);
        const int want = static_cast<int>(std::lround((outB + (outW - outB) * t) * 255.0));
        INFO("x=" << x << " in=" << v);
        CHECK(std::abs(static_cast<int>(px(out, x, 0).r) - want) <= 1);
    }
    // ... and gamma exactly 1 must not be a different picture from gamma one-ULP away being
    // rounded to it: the guard tests the float, so 1.0 takes the fast arm and nothing else does.
    core::Document doc2(6, 1);
    seedTestCard(doc2);
    addAdjustment(doc2, core::AdjustmentKind::Levels,
                  {{"in_black", inB}, {"in_white", inW}, {"gamma", 1.0000001},
                   {"out_black", outB}, {"out_white", outW}});
    const common::Image nearOne = flatten(doc2);
    for (std::uint32_t x = 0; x < 6; ++x)
        CHECK(std::abs(static_cast<int>(px(nearOne, x, 0).r) -
                       static_cast<int>(px(out, x, 0).r)) <= 1);
}

TEST_CASE("Exposure: +1 EV doubles linear light (through the sRGB curve)") {
    core::Document doc(6, 1);
    seedTestCard(doc);
    addAdjustment(doc, core::AdjustmentKind::Exposure, {{"exposure", 1.0}});
    const common::Image out = flatten(doc);
    const int expected =
        static_cast<int>(std::lround(srgbEncode(2.0 * srgbDecode(128.0 / 255.0)) * 255.0));
    CHECK(std::abs(static_cast<int>(px(out, 2, 0).r) - expected) <= 2);
    CHECK(px(out, 0, 0).r == 0);           // black has no light to double
    CHECK(px(out, 1, 0).r == 255);         // white clamps
    CHECK(px(out, 2, 0).a == 255);         // alpha untouched
}

TEST_CASE("Hue/Saturation: rotation, desaturation, lightness") {
    // +120 degrees carries red onto green (and green onto blue).
    core::Document doc(6, 1);
    seedTestCard(doc);
    addAdjustment(doc, core::AdjustmentKind::HueSaturation, {{"hue", 120.0}});
    common::Image out = flatten(doc);
    CHECK(px(out, 3, 0).r <= 2);
    CHECK(px(out, 3, 0).g >= 253);
    CHECK(px(out, 3, 0).b <= 2);
    CHECK(px(out, 4, 0).b >= 253); // green -> blue
    CHECK(px(out, 2, 0).r == 128); // gray has no hue to rotate

    // Saturation -100 lands every pixel on its HSL lightness (red: L = 0.5).
    core::Document doc2(6, 1);
    seedTestCard(doc2);
    addAdjustment(doc2, core::AdjustmentKind::HueSaturation, {{"saturation", -100.0}});
    out = flatten(doc2);
    const common::Color8 red = px(out, 3, 0);
    CHECK(red.r == red.g);
    CHECK(red.g == red.b);
    CHECK(std::abs(static_cast<int>(red.r) - 128) <= 1);

    // Lightness rides to the rails.
    core::Document doc3(6, 1);
    seedTestCard(doc3);
    addAdjustment(doc3, core::AdjustmentKind::HueSaturation, {{"lightness", 100.0}});
    CHECK(px(flatten(doc3), 3, 0) == common::Color8{255, 255, 255, 255});
    core::Document doc4(6, 1);
    seedTestCard(doc4);
    addAdjustment(doc4, core::AdjustmentKind::HueSaturation, {{"lightness", -100.0}});
    CHECK(px(flatten(doc4), 1, 0) == common::Color8{0, 0, 0, 255});
}

TEST_CASE("Color Balance: midtone shift with and without preserved luminosity") {
    // Full cyan-red on the midtones, preservation off: mid-gray gains red, keeps green/blue.
    core::Document doc(6, 1);
    seedTestCard(doc);
    addAdjustment(doc, core::AdjustmentKind::ColorBalance,
                  {{"midtones_cr", 100.0}, {"preserve_luminosity", 0.0}});
    common::Image out = flatten(doc);
    CHECK(px(out, 2, 0).r >= 200);
    CHECK(std::abs(static_cast<int>(px(out, 2, 0).g) - 128) <= 1);
    CHECK(std::abs(static_cast<int>(px(out, 2, 0).b) - 128) <= 1);

    // Preservation on: the shift is visible but the W3C luminosity stays put.
    core::Document doc2(6, 1);
    seedTestCard(doc2);
    addAdjustment(doc2, core::AdjustmentKind::ColorBalance, {{"midtones_cr", 100.0}});
    out = flatten(doc2);
    const common::Color8 c = px(out, 2, 0);
    CHECK(c.r > c.g); // still reads red...
    const double lum = 0.3 * c.r + 0.59 * c.g + 0.11 * c.b;
    CHECK(std::abs(lum - 128.0) <= 2.5); // ...at the original luminosity
    // Black and white have no midtone weight to shift.
    CHECK(px(out, 0, 0).r <= 2);
    CHECK(px(out, 1, 0).r >= 253);
}

TEST_CASE("Threshold: strictly binary at the level, alpha preserved") {
    core::Document doc(6, 1);
    seedTestCard(doc);
    addAdjustment(doc, core::AdjustmentKind::Threshold, {}); // default level 0.5
    const common::Image out = flatten(doc);
    for (std::uint32_t x = 0; x < 6; ++x) {
        const common::Color8 c = px(out, x, 0);
        CHECK((c.r == 0 || c.r == 255));
        CHECK(c.r == c.g);
        CHECK(c.g == c.b);
        CHECK(c.a == 255);
    }
    CHECK(px(out, 0, 0).r == 0);   // black below the level
    CHECK(px(out, 1, 0).r == 255); // white above
    CHECK(px(out, 4, 0).r == 255); // green: W3C luma 0.59 >= 0.5
    CHECK(px(out, 5, 0).r == 0);   // blue: 0.11 < 0.5
}

TEST_CASE("Posterize: quantizes each channel onto N evenly spaced levels") {
    core::Document doc(6, 1);
    seedTestCard(doc);
    addAdjustment(doc, core::AdjustmentKind::Posterize, {{"levels", 4.0}});
    const common::Image out = flatten(doc);
    const std::set<int> allowed{0, 85, 170, 255}; // round(v*3)/3 on the 8-bit lattice
    for (std::uint32_t x = 0; x < 6; ++x) {
        const common::Color8 c = px(out, x, 0);
        for (const int v : {static_cast<int>(c.r), static_cast<int>(c.g), static_cast<int>(c.b)}) {
            CAPTURE(x);
            CAPTURE(v);
            bool near = false;
            for (const int a : allowed) near = near || std::abs(v - a) <= 1;
            CHECK(near);
        }
    }
    // 128/255 = 0.502 -> round(1.506)/3 = 2/3 -> 170.
    CHECK(std::abs(static_cast<int>(px(out, 2, 0).r) - 170) <= 1);
}

TEST_CASE("Grayscale: methods project as declared, strength mixes, default = the old formula") {
    using core::GrayscaleMethod;
    const auto method = [](GrayscaleMethod m, double strength = 100.0) {
        return std::map<std::string, double>{{"method", static_cast<double>(m)},
                                             {"strength", strength}};
    };
    // An absent bag reads the defaults = the pre-S32 formula (0.30/0.59/0.11 on encoded values).
    core::Document doc(6, 1);
    seedTestCard(doc);
    addAdjustment(doc, core::AdjustmentKind::Grayscale, {});
    common::Image out = flatten(doc);
    CHECK(std::abs(static_cast<int>(px(out, 3, 0).r) - 77) <= 1);  // red: 0.30
    CHECK(std::abs(static_cast<int>(px(out, 4, 0).r) - 150) <= 1); // green: 0.59
    CHECK(px(out, 2, 0).r == 128); // any neutral is a fixed point of every method

    // The channel methods read their channel (the photographer's mono filters); No chrominance
    // keeps the photometric luminance (any neutral is a fixed point; a primary lands on its
    // linear-light Rec 709 share re-encoded).
    core::Document doc2(6, 1);
    seedTestCard(doc2);
    addAdjustment(doc2, core::AdjustmentKind::Grayscale, method(GrayscaleMethod::Red));
    out = flatten(doc2);
    CHECK(px(out, 3, 0) == common::Color8{255, 255, 255, 255}); // red -> its own channel = white
    CHECK(px(out, 5, 0) == common::Color8{0, 0, 0, 255});       // blue has no red
    core::Document doc3(6, 1);
    seedTestCard(doc3);
    addAdjustment(doc3, core::AdjustmentKind::Grayscale,
                  method(GrayscaleMethod::NoChrominance));
    out = flatten(doc3);
    CHECK(px(out, 2, 0).r == 128); // neutral fixed point
    const int lumRed =
        static_cast<int>(std::lround(srgbEncode(0.2126) * 255.0)); // pure red's luminance gray
    CHECK(std::abs(static_cast<int>(px(out, 3, 0).r) - lumRed) <= 2);

    // Strength 0 is a byte-level no-op; 50% lands halfway between original and gray.
    core::Document plain(6, 1);
    seedTestCard(plain);
    const common::Image before = flatten(plain);
    core::Document doc5(6, 1);
    seedTestCard(doc5);
    addAdjustment(doc5, core::AdjustmentKind::Grayscale, method(GrayscaleMethod::Luma, 0.0));
    CHECK(flatten(doc5).rgba == before.rgba);
    core::Document doc6(6, 1);
    seedTestCard(doc6);
    addAdjustment(doc6, core::AdjustmentKind::Grayscale, method(GrayscaleMethod::Luma, 50.0));
    out = flatten(doc6);
    CHECK(std::abs(static_cast<int>(px(out, 3, 0).r) - 166) <= 1); // lerp(1.0, 0.30, 0.5)
    CHECK(std::abs(static_cast<int>(px(out, 3, 0).g) - 38) <= 1);  // lerp(0.0, 0.30, 0.5)

    // The gray PALETTE ("how do I show this image with 3 grays?"): every output gray lands on
    // the N-level lattice; 256 (the default) is continuous and byte-identical to no quantizer.
    core::Document doc7(6, 1);
    seedTestCard(doc7);
    auto bag3 = method(GrayscaleMethod::Luma);
    bag3["grays"] = 3.0;
    addAdjustment(doc7, core::AdjustmentKind::Grayscale, bag3);
    out = flatten(doc7);
    for (std::uint32_t x = 0; x < 6; ++x) {
        CAPTURE(x);
        const int v = px(out, x, 0).r;
        CHECK((v <= 1 || std::abs(v - 128) <= 1 || v >= 254)); // {0, 1/2, 1} x 255
        CHECK(px(out, x, 0).r == px(out, x, 0).g);             // still gray
    }
    core::Document doc8(6, 1);
    seedTestCard(doc8);
    addAdjustment(doc8, core::AdjustmentKind::Grayscale, method(GrayscaleMethod::Luma));
    const common::Image continuous = flatten(doc8);
    core::Document doc9(6, 1);
    seedTestCard(doc9);
    auto bag256 = method(GrayscaleMethod::Luma);
    bag256["grays"] = 256.0;
    addAdjustment(doc9, core::AdjustmentKind::Grayscale, bag256);
    CHECK(flatten(doc9).rgba == continuous.rgba);
}

namespace {
// A raster of one solid RGBA colour (its own doc, since Document is not movable).
void seedSolid(core::Document& doc, std::uint32_t w, std::uint32_t h, common::Color8 c) {
    auto base = doc.makeRaster("base", w, h);
    common::Image& img = base->image();
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        img.rgba[i * 4] = c.r;
        img.rgba[i * 4 + 1] = c.g;
        img.rgba[i * 4 + 2] = c.b;
        img.rgba[i * 4 + 3] = c.a;
    }
    doc.root().addOnTop(std::move(base));
}
// Defined further down with the blur tests; forward-declared here (same unnamed namespace) so the
// adaptive-threshold region test below can reuse the structured blur scene.
void seedBlurScene(core::Document& doc);
} // namespace

TEST_CASE("Grayscale MaxChannel: gray = round(max(R,G,B)); still per-pixel") {
    using core::GrayscaleMethod;
    core::Document doc(6, 1);
    seedTestCard(doc);
    addAdjustment(doc, core::AdjustmentKind::Grayscale,
                  {{"method", static_cast<double>(GrayscaleMethod::MaxChannel)}});
    const common::Image out = flatten(doc);
    // Test-card columns: black, white, mid-gray(128), red, green, blue -> max channel each.
    const std::array<int, 6> wantMax{0, 255, 128, 255, 255, 255};
    for (std::uint32_t x = 0; x < 6; ++x) {
        CAPTURE(x);
        const common::Color8 g = px(out, x, 0);
        CHECK(g.r == g.g);                    // output is neutral gray
        CHECK(g.g == g.b);
        CHECK(std::abs(g.r - wantMax[x]) <= 1);
        CHECK(g.a == px(out, x, 0).a);        // alpha untouched (opaque card stays 255)
    }
    // MaxChannel is a pure per-pixel projection -- NOT spatial.
    core::Document probe(1, 1);
    auto* adj = addAdjustment(probe, core::AdjustmentKind::Grayscale,
                              {{"method", static_cast<double>(GrayscaleMethod::MaxChannel)}});
    CHECK_FALSE(core::adjustmentIsSpatial(*adj));
}

TEST_CASE("Grayscale MinChannel: gray = round(min(R,G,B)); still per-pixel") {
    using core::GrayscaleMethod;
    core::Document doc(6, 1);
    seedTestCard(doc);
    addAdjustment(doc, core::AdjustmentKind::Grayscale,
                  {{"method", static_cast<double>(GrayscaleMethod::MinChannel)}});
    const common::Image out = flatten(doc);
    // Test-card columns: black, white, mid-gray(128), red, green, blue -> min channel each. The
    // primaries collapse to 0 (their other two channels are 0) -- the low-key mirror of Max.
    const std::array<int, 6> wantMin{0, 255, 128, 0, 0, 0};
    for (std::uint32_t x = 0; x < 6; ++x) {
        CAPTURE(x);
        const common::Color8 g = px(out, x, 0);
        CHECK(g.r == g.g);                    // output is neutral gray
        CHECK(g.g == g.b);
        CHECK(std::abs(g.r - wantMin[x]) <= 1);
        CHECK(g.a == px(out, x, 0).a);        // alpha untouched (opaque card stays 255)
    }
    // MinChannel is a pure per-pixel projection -- NOT spatial (index 8, appended after Adaptive).
    core::Document probe(1, 1);
    auto* adj = addAdjustment(probe, core::AdjustmentKind::Grayscale,
                              {{"method", static_cast<double>(GrayscaleMethod::MinChannel)}});
    CHECK_FALSE(core::adjustmentIsSpatial(*adj));
}

TEST_CASE("Grayscale Dithered: 1-bit Floyd-Steinberg, tone-preserving, spatial") {
    using core::GrayscaleMethod;
    // A uniform mid-gray field: every output pixel is pure black or pure white (1-bit), and the
    // dither PRESERVES the mean tone -- the white fraction tracks lum(0.502) = 0.502.
    core::Document doc(48, 48);
    seedSolid(doc, 48, 48, {128, 128, 128, 255});
    core::AdjustmentLayer* adj = addAdjustment(
        doc, core::AdjustmentKind::Grayscale,
        {{"method", static_cast<double>(GrayscaleMethod::Dithered)}});
    CHECK(core::adjustmentIsSpatial(*adj)); // method-dependent spatiality

    const common::Image out = flatten(doc);
    std::size_t white = 0;
    for (std::uint32_t y = 0; y < 48; ++y)
        for (std::uint32_t x = 0; x < 48; ++x) {
            const common::Color8 g = px(out, x, y);
            REQUIRE((g.r == 0 || g.r == 255)); // binary
            CHECK(g.r == g.g);
            CHECK(g.g == g.b);
            if (g.r == 255) ++white;
        }
    const double frac = static_cast<double>(white) / (48.0 * 48.0);
    CHECK(frac == doctest::Approx(0.502).epsilon(0.06)); // mean tone held

    // Strength 0 stays a byte-level no-op even for a spatial method.
    core::Document plain(48, 48);
    seedSolid(plain, 48, 48, {128, 128, 128, 255});
    const common::Image before = flatten(plain);
    core::Document doc0(48, 48);
    seedSolid(doc0, 48, 48, {128, 128, 128, 255});
    addAdjustment(doc0, core::AdjustmentKind::Grayscale,
                  {{"method", static_cast<double>(GrayscaleMethod::Dithered)}, {"strength", 0.0}});
    CHECK(flatten(doc0).rgba == before.rgba);
}

TEST_CASE("Grayscale AdaptiveThreshold: binary, local, spatial, region == crop(full)") {
    using core::GrayscaleMethod;
    // A single dark pixel on a bright field: adaptive thresholding keeps the flat field white
    // (each pixel sits at ~its own local mean) and drops the locally-dark pixel to black.
    core::Document doc(24, 24);
    seedSolid(doc, 24, 24, {255, 255, 255, 255});
    doc.root().child(0).as<core::RasterLayer>()->image().rgba[(12 * 24 + 12) * 4] = 0;
    doc.root().child(0).as<core::RasterLayer>()->image().rgba[(12 * 24 + 12) * 4 + 1] = 0;
    doc.root().child(0).as<core::RasterLayer>()->image().rgba[(12 * 24 + 12) * 4 + 2] = 0;
    core::AdjustmentLayer* adj = addAdjustment(
        doc, core::AdjustmentKind::Grayscale,
        {{"method", static_cast<double>(GrayscaleMethod::AdaptiveThreshold)}});
    CHECK(core::adjustmentIsSpatial(*adj));

    const common::Image out = flatten(doc);
    std::size_t black = 0;
    for (std::uint32_t y = 0; y < 24; ++y)
        for (std::uint32_t x = 0; x < 24; ++x) {
            const common::Color8 g = px(out, x, y);
            REQUIRE((g.r == 0 || g.r == 255)); // binary
            CHECK(g.r == g.g);
            if (g.r == 0) ++black;
        }
    CHECK(px(out, 12, 12).r == 0);          // the locally-dark pixel binarizes to black
    CHECK(px(out, 0, 0).r == 255);          // the flat bright corner stays white
    CHECK(black >= 1);                       // at least the dark spot fired

    // Region == crop(full) byte-exact: adaptive threshold has a finite window reach + clamp-to-
    // edge reads, so the S60-a dirty-rect path matches the full composite (the blur money test).
    core::Document scene(64, 48);
    seedBlurScene(scene);
    addAdjustment(scene, core::AdjustmentKind::Grayscale,
                  {{"method", static_cast<double>(GrayscaleMethod::AdaptiveThreshold)}});
    const common::Image full = flatten(scene);
    const common::Rect roi{24.0, 12.0, 16.0, 16.0};
    const render::CompositeResult r =
        render::compositeRegion(scene, roi, {}, render::Backend::Cpu);
    REQUIRE(r.ok);
    const auto x0 = static_cast<std::uint32_t>(roi.x);
    const auto y0 = static_cast<std::uint32_t>(roi.y);
    for (std::uint32_t y = 0; y < r.image.height; ++y)
        for (std::uint32_t x = 0; x < r.image.width; ++x) {
            const common::Color8 got = px(r.image, x, y);
            const common::Color8 want = px(full, x0 + x, y0 + y);
            REQUIRE(got.r == want.r);
            REQUIRE(got.g == want.g);
            REQUIRE(got.b == want.b);
            REQUIRE(got.a == want.a);
        }
}

TEST_CASE("color-balance plane mapping round-trips and preserves the achromatic mean") {
    // Pure cyan-red projects onto +x and returns exactly.
    const core::ColorBalancePoint p = core::colorBalanceToPlane({100.0, 0.0, 0.0});
    CHECK(p.x == doctest::Approx(100.0 * 2.0 / 3.0));
    CHECK(p.y == doctest::Approx(0.0));
    const core::ColorBalanceTriple back =
        core::colorBalanceFromPlane(p, (100.0 + 0.0 + 0.0) / 3.0);
    CHECK(back.cr == doctest::Approx(100.0));
    CHECK(back.mg == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(back.yb == doctest::Approx(0.0).epsilon(1e-9));

    // A general triple round-trips through (plane, mean) exactly.
    const core::ColorBalanceTriple v{35.0, -60.0, 10.0};
    const double mean = (v.cr + v.mg + v.yb) / 3.0;
    const core::ColorBalanceTriple rt =
        core::colorBalanceFromPlane(core::colorBalanceToPlane(v), mean);
    CHECK(rt.cr == doctest::Approx(v.cr));
    CHECK(rt.mg == doctest::Approx(v.mg));
    CHECK(rt.yb == doctest::Approx(v.yb));

    // The achromatic component is invisible to the plane (the wheel edits chroma only).
    const core::ColorBalancePoint zero = core::colorBalanceToPlane({40.0, 40.0, 40.0});
    CHECK(zero.x == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(zero.y == doctest::Approx(0.0).epsilon(1e-9));
}

// ---------------------------------------------------------------------------------------------
// S34: Curves (storage + composited math)
// ---------------------------------------------------------------------------------------------

namespace {

// A curve through (0,0), (mx,my), (1,1) -- the one-handle shape a user drags first.
core::brush::Curve curveVia(double mx, double my) {
    return core::brush::Curve(
        std::vector<core::brush::CurvePoint>{{0.0, 0.0, false}, {mx, my, false}, {1.0, 1.0, false}});
}

} // namespace

TEST_CASE("Curves storage: knots round-trip through the double bag, absent == identity") {
    std::map<std::string, double> bag;
    // Nothing stored: every channel reads as the identity, and so does the layer.
    for (int i = 0; i < core::kCurveChannelCount; ++i)
        CHECK(core::adjustmentCurve(bag, static_cast<core::CurveChannel>(i)).isIdentity());

    const core::brush::Curve lift = curveVia(0.5, 0.75);
    core::setAdjustmentCurve(bag, core::CurveChannel::Red, lift);
    // Only the RED channel's keys exist; the others stay absent (= identity), and the composite
    // channel's prefix "curve_rgb" must not collide with red's "curve_r".
    CHECK(bag.count("curve_r_n") == 1);
    CHECK(bag.count("curve_rgb_n") == 0);
    CHECK(core::adjustmentCurve(bag, core::CurveChannel::Composite).isIdentity());
    CHECK(core::adjustmentCurve(bag, core::CurveChannel::Green).isIdentity());

    const core::brush::Curve back = core::adjustmentCurve(bag, core::CurveChannel::Red);
    REQUIRE(back.points().size() == lift.points().size());
    for (std::size_t i = 0; i < back.points().size(); ++i) {
        CHECK(back.points()[i].x == lift.points()[i].x); // doubles: exact, not approximate
        CHECK(back.points()[i].y == lift.points()[i].y);
        CHECK(back.points()[i].corner == lift.points()[i].corner);
    }
    // Re-encoding the decoded curve reproduces the same bag byte-for-byte: that IS the .mosaic
    // round trip, since docio writes the bag as a JSON number map and reads it straight back.
    std::map<std::string, double> again;
    core::setAdjustmentCurve(again, core::CurveChannel::Red, back);
    CHECK(again == bag);

    // Corner flags survive; a smooth knot writes no flag key at all (bags stay small).
    core::brush::Curve cornered(std::vector<core::brush::CurvePoint>{
        {0.0, 0.0, false}, {0.4, 0.6, true}, {1.0, 1.0, false}});
    std::map<std::string, double> cbag;
    core::setAdjustmentCurve(cbag, core::CurveChannel::Composite, cornered);
    CHECK(cbag.count("curve_rgb_1_c") == 1);
    CHECK(cbag.count("curve_rgb_0_c") == 0);
    CHECK(core::adjustmentCurve(cbag, core::CurveChannel::Composite).points()[1].corner);

    // A shorter curve must not leave the longer one's tail behind.
    core::setAdjustmentCurve(cbag, core::CurveChannel::Composite, curveVia(0.5, 0.9));
    CHECK(cbag.count("curve_rgb_2_x") == 1); // the 3-knot curve's last knot
    CHECK(cbag.count("curve_rgb_3_x") == 0);
    core::setAdjustmentCurve(cbag, core::CurveChannel::Composite, core::brush::Curve{});
    CHECK(cbag.empty()); // writing the identity ERASES: absent is how the identity is spelled
}

TEST_CASE("Curves storage: a corrupt or hostile bag degrades to a sane curve") {
    std::map<std::string, double> bag;
    bag["curve_r_n"] = 1e9;              // absurd count: the decode is capped, never unbounded
    bag["curve_r_0_x"] = -5.0;           // outside the unit square: clamped in
    bag["curve_r_0_y"] = 7.0;
    bag["curve_r_1_x"] = 1.0;
    bag["curve_r_1_y"] = 1.0;
    const core::brush::Curve c = core::adjustmentCurve(bag, core::CurveChannel::Red);
    REQUIRE(c.points().size() == 2);
    CHECK(c.points()[0].x == 0.0);
    CHECK(c.points()[0].y == 1.0);
    // Only one usable knot left -> the identity, not a constant that would flatten the image.
    std::map<std::string, double> lone;
    lone["curve_g_n"] = 2.0;
    lone["curve_g_0_x"] = 0.0;
    lone["curve_g_0_y"] = 0.0; // knot 1 is missing entirely
    CHECK(core::adjustmentCurve(lone, core::CurveChannel::Green).isIdentity());
}

TEST_CASE("Curves: identity curves composite byte-identically to no layer") {
    core::Document plain(6, 1);
    seedTestCard(plain);
    const common::Image before = flatten(plain);

    // A seeded layer (the Filter-menu insert), an empty bag, a bag full of unrelated junk, and
    // an explicitly-stored identity curve must ALL be byte-level no-ops.
    core::Document doc(6, 1);
    seedTestCard(doc);
    core::AdjustmentLayer* adj = addAdjustment(doc, core::AdjustmentKind::Curves, {});
    core::seedAdjustmentDefaults(*adj);
    CHECK(flatten(doc).rgba == before.rgba);
    adj->params().clear();
    CHECK(flatten(doc).rgba == before.rgba);
    adj->params() = {{"gamma", 1.8}, {"black_point", 0.02}}; // a pre-S34 document's stale keys
    CHECK(flatten(doc).rgba == before.rgba);
    adj->params().clear();
    core::setAdjustmentCurve(adj->params(), core::CurveChannel::Composite, core::brush::Curve{});
    CHECK(core::adjustmentCurvesIdentity(*adj));
    CHECK(flatten(doc).rgba == before.rgba);
}

TEST_CASE("Curves: the composite curve maps every channel through the drawn shape") {
    core::Document doc(6, 1);
    seedTestCard(doc);
    core::AdjustmentLayer* adj = addAdjustment(doc, core::AdjustmentKind::Curves, {});
    // Mid-gray -> 0.75. The spline is monotone through (0,0),(0.5,0.75),(1,1), so 128/255 lands
    // near 0.75*255 = 191; the natural-spline shape near the knot is what the tolerance covers.
    core::setAdjustmentCurve(adj->params(), core::CurveChannel::Composite, curveVia(0.5, 0.75));
    CHECK_FALSE(core::adjustmentCurvesIdentity(*adj));
    const common::Image out = flatten(doc);
    // Endpoints are pinned by the curve itself: black stays black, white stays white.
    CHECK(px(out, 0, 0) == common::Color8{0, 0, 0, 255});
    CHECK(px(out, 1, 0) == common::Color8{255, 255, 255, 255});
    CHECK(std::abs(static_cast<int>(px(out, 2, 0).r) - 191) <= 3);
    CHECK(px(out, 2, 0).r == px(out, 2, 0).g); // a composite curve treats the channels alike
    CHECK(px(out, 2, 0).g == px(out, 2, 0).b);
    // The pure red patch: red is at 1.0 (pinned), green/blue at 0 (pinned) -- unchanged.
    CHECK(px(out, 3, 0) == common::Color8{255, 0, 0, 255});
}

TEST_CASE("Curves: a per-channel curve leaves the other channels BYTE-identical") {
    core::Document plain(6, 1);
    seedTestCard(plain);
    const common::Image before = flatten(plain);
    core::Document doc(6, 1);
    seedTestCard(doc);
    core::AdjustmentLayer* adj = addAdjustment(doc, core::AdjustmentKind::Curves, {});
    core::setAdjustmentCurve(adj->params(), core::CurveChannel::Green, curveVia(0.5, 0.25));
    const common::Image out = flatten(doc);
    for (std::uint32_t x = 0; x < 6; ++x) {
        CAPTURE(x);
        // Red and blue never went through a lookup at all -- the untouched-channel rule.
        CHECK(px(out, x, 0).r == px(before, x, 0).r);
        CHECK(px(out, x, 0).b == px(before, x, 0).b);
        CHECK(px(out, x, 0).a == px(before, x, 0).a);
    }
    CHECK(px(out, 2, 0).g < px(before, 2, 0).g); // ... and green really did darken
    CHECK(px(out, 4, 0).g == 255); // the pure-green patch sits on the curve's pinned (1,1) end
}

TEST_CASE("Curves: per-channel runs FIRST, then the composite curve on its result") {
    // Green through a channel curve mapping 0.5 -> 0.25, then a composite curve mapping
    // 0.25 -> ~0.5 must land back near where it started; the reverse order would not.
    core::Document doc(6, 1);
    seedTestCard(doc);
    core::AdjustmentLayer* adj = addAdjustment(doc, core::AdjustmentKind::Curves, {});
    core::setAdjustmentCurve(adj->params(), core::CurveChannel::Green, curveVia(0.5, 0.25));
    core::setAdjustmentCurve(adj->params(), core::CurveChannel::Composite, curveVia(0.25, 0.5));
    const common::Image out = flatten(doc);
    CHECK(std::abs(static_cast<int>(px(out, 2, 0).g) - 128) <= 8);
}

TEST_CASE("Curves: the layer's opacity gates the graded result") {
    core::Document doc(6, 1);
    seedTestCard(doc);
    core::AdjustmentLayer* adj = addAdjustment(doc, core::AdjustmentKind::Curves, {});
    core::setAdjustmentCurve(adj->params(), core::CurveChannel::Composite, curveVia(0.5, 0.75));
    const int full = px(flatten(doc), 2, 0).r;
    adj->setOpacity(0.5f);
    const int half = px(flatten(doc), 2, 0).r;
    CHECK(half > 128);
    CHECK(half < full);
    CHECK(std::abs(half - (128 + (full - 128) / 2)) <= 2);
}

// ---------------------------------------------------------------------------------------------
// Automask (Filter -> Adjustments with an active selection)
// ---------------------------------------------------------------------------------------------

TEST_CASE("a new adjustment born masked to the selection grades only inside it") {
    // The composite half of MainWindow::insertAdjustmentLayer's automask: the mask it hands the
    // layer comes from core::maskFromSelection, so the graded set is exactly the selection --
    // FRACTIONAL coverage included (a feathered / anti-aliased selection must not harden into a
    // binary cut-out on its way to becoming a mask).
    core::Document plain(6, 1);
    seedTestCard(plain);
    const common::Image untouched = flatten(plain);

    core::Document full(6, 1);
    seedTestCard(full);
    addAdjustment(full, core::AdjustmentKind::Invert, {});
    const common::Image invertedFull = flatten(full);

    core::Document doc(6, 1);
    seedTestCard(doc);
    auto layer = doc.makeAdjustment("adj", core::AdjustmentKind::Invert);
    core::Selection sel(6, 1);
    sel.data() = {0, 0, 255, 255, 128, 128}; // out, out, in, in, half, half
    layer->setMask(core::maskFromSelection(*layer, sel, 6, 1));
    doc.root().addOnTop(std::move(layer));
    const common::Image out = flatten(doc);

    for (std::uint32_t x = 0; x < 6; ++x) {
        const common::Color8 got = px(out, x, 0);
        const common::Color8 off = px(untouched, x, 0);
        const common::Color8 on = px(invertedFull, x, 0);
        if (x < 2) { // unselected: byte-identical to no adjustment at all
            CHECK(got.r == off.r);
            CHECK(got.g == off.g);
            CHECK(got.b == off.b);
            CHECK(got.a == off.a);
        } else if (x < 4) { // fully selected: the unmasked result, byte for byte
            CHECK(got.r == on.r);
            CHECK(got.g == on.g);
            CHECK(got.b == on.b);
            CHECK(got.a == on.a);
        } else { // half covered: halfway between (the ramp survived, +-1 for the round trip)
            CHECK(std::abs(int{got.r} - (int{off.r} + int{on.r}) / 2) <= 1);
            CHECK(std::abs(int{got.g} - (int{off.g} + int{on.g}) / 2) <= 1);
            CHECK(std::abs(int{got.b} - (int{off.b} + int{on.b}) / 2) <= 1);
        }
    }
}

TEST_CASE("automask: no selection leaves the adjustment maskless (never a reveal-all mask)") {
    // insertAdjustmentLayer gates the automask on isEmpty() *and* anySelected(): an empty
    // selection would give maskFromSelection's reveal-all mask (harmless but a lie in the dock),
    // and an active-selection-of-nothing would give an all-zero one -- a layer that grades no
    // pixel and reads as "the menu item did nothing". Both must stay off the new layer.
    const core::Selection none;
    CHECK(none.isEmpty());
    CHECK_FALSE(none.anySelected());

    core::Selection empty(6, 1); // active, but covering nothing
    CHECK_FALSE(empty.isEmpty());
    CHECK_FALSE(empty.anySelected());

    // What the guard admits: any coverage at all.
    core::Selection some(6, 1);
    some.data()[3] = 1;
    CHECK(some.anySelected());
}

// ---------------------------------------------------------------------------------------------
// adjustmentPreview (the dock thumbnail's affected-scope composite, S32)
// ---------------------------------------------------------------------------------------------

TEST_CASE("adjustmentPreview: the affected scope with the effect applied") {
    // Root scope: a red base under an Invert previews cyan (the backdrop as the adjustment
    // sees it, with it applied) at full alpha.
    core::Document doc(2, 1);
    auto base = doc.makeRaster("base", 2, 1);
    base->image().fill({255, 0, 0, 255});
    doc.root().addOnTop(std::move(base));
    core::AdjustmentLayer* inv = addAdjustment(doc, core::AdjustmentKind::Invert, {});
    const common::Image out = render::adjustmentPreview(*inv, 2, 1, 2, 1);
    REQUIRE(!out.empty());
    CHECK(px(out, 0, 0) == common::Color8{0, 255, 255, 255});

    // An INVISIBLE adjustment previews the plain backdrop -- the document shows no effect either.
    inv->setVisible(false);
    const common::Image plain = render::adjustmentPreview(*inv, 2, 1, 2, 1);
    CHECK(px(plain, 0, 0) == common::Color8{255, 0, 0, 255});
}

TEST_CASE("adjustmentPreview: scoped to the parent group, layers outside stay out") {
    // Red base at the root; a group holds [green-left-pixel, Invert]. The invert's preview shows
    // the GROUP's backdrop only: inverted green on the left, TRANSPARENT on the right -- the red
    // base never enters the scope (exactly the compositor's scoping).
    core::Document doc(2, 1);
    auto base = doc.makeRaster("base", 2, 1);
    base->image().fill({255, 0, 0, 255});
    doc.root().addOnTop(std::move(base));
    auto group = doc.makeGroup("grp");
    auto green = doc.makeRaster("green", 2, 1);
    green->image().rgba = {0, 255, 0, 255, /*right*/ 0, 0, 0, 0};
    group->addOnTop(std::move(green));
    auto adj = doc.makeAdjustment("inv", core::AdjustmentKind::Invert);
    const core::AdjustmentLayer* raw = adj.get();
    group->addOnTop(std::move(adj));
    doc.root().addOnTop(std::move(group));

    const common::Image out = render::adjustmentPreview(*raw, 2, 1, 2, 1);
    REQUIRE(!out.empty());
    CHECK(px(out, 0, 0) == common::Color8{255, 0, 255, 255}); // green inverted -> magenta
    CHECK(px(out, 1, 0).a == 0);                              // the base red is out of scope
}

TEST_CASE("adjustmentPreview: params drive the preview and the buffer downscales") {
    // Threshold at its mid-gray default over the test card, previewed at HALF width: still
    // strictly binary (the walk renders at preview resolution; Area averaging feeds the math
    // encoded values, and Threshold binarizes after).
    core::Document doc(6, 1);
    seedTestCard(doc);
    core::AdjustmentLayer* th = addAdjustment(doc, core::AdjustmentKind::Threshold, {});
    const common::Image out = render::adjustmentPreview(*th, 6, 1, 3, 1);
    REQUIRE(!out.empty());
    CHECK(out.width == 3);
    for (std::uint32_t x = 0; x < 3; ++x) {
        CHECK((px(out, x, 0).r == 0 || px(out, x, 0).r == 255));
        CHECK(px(out, x, 0).a == 255);
    }
    // A detached adjustment (no parent -- mid-undo) previews nothing rather than crashing.
    core::Document doc2(1, 1);
    auto orphan = doc2.makeAdjustment("o", core::AdjustmentKind::Invert);
    CHECK(render::adjustmentPreview(*orphan, 1, 1, 1, 1).empty());
}

TEST_CASE("the new kinds honor opacity like the original ones") {
    // Threshold at half opacity: mid-gray (0.502 -> binary 255) lands halfway back.
    core::Document doc(6, 1);
    seedTestCard(doc);
    core::AdjustmentLayer* adj = addAdjustment(doc, core::AdjustmentKind::Threshold, {});
    adj->setOpacity(0.5f);
    const common::Image out = flatten(doc);
    const int expected = (128 + 255) / 2;
    CHECK(std::abs(static_cast<int>(px(out, 2, 0).r) - expected) <= 1);
}

// ---------------------------------------------------------------------------------------------
// The S33 blur family (docs/blur-filters.md). These pin the COMPOSITOR-level contracts -- the
// kernel-level signatures live in test_blur_kernels.cpp: identity-at-zero, visible defaults,
// alpha diffusion, byte-exact mask gating, region == crop(full) under the reach expansion,
// the DoF band/plateau invariants, preview-scale proportionality, and golden pins.
// ---------------------------------------------------------------------------------------------

namespace {

// A deterministic 64x48 scene with structure a blur cannot fake: a dark-to-mid horizontal
// gradient background, a white square, and a half-transparent red block over the seam.
void seedBlurScene(core::Document& doc) {
    auto base = doc.makeRaster("base", 64, 48);
    common::Image& img = base->image();
    for (std::uint32_t y = 0; y < 48; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * 64 + x) * 4;
            const auto g = static_cast<std::uint8_t>(30 + x * 2);
            img.rgba[p] = g;
            img.rgba[p + 1] = g;
            img.rgba[p + 2] = static_cast<std::uint8_t>(60 + y);
            img.rgba[p + 3] = 255;
        }
    }
    for (std::uint32_t y = 14; y < 26; ++y) // the white square
        for (std::uint32_t x = 20; x < 32; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * 64 + x) * 4;
            img.rgba[p] = img.rgba[p + 1] = img.rgba[p + 2] = 255;
            img.rgba[p + 3] = 255;
        }
    for (std::uint32_t y = 20; y < 40; ++y) // the translucent red block
        for (std::uint32_t x = 40; x < 56; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * 64 + x) * 4;
            img.rgba[p] = 220;
            img.rgba[p + 1] = 30;
            img.rgba[p + 2] = 30;
            img.rgba[p + 3] = 128;
        }
    doc.root().addOnTop(std::move(base));
}

[[nodiscard]] bool imagesEqual(const common::Image& a, const common::Image& b) {
    return a.width == b.width && a.height == b.height && a.rgba == b.rgba;
}

// FNV-1a over the raw bytes -- the golden-pin hash (the S55 texture-golden convention).
[[nodiscard]] std::uint64_t fnv1a(const std::uint8_t* data, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) h = (h ^ data[i]) * 1099511628211ull;
    return h;
}

} // namespace

TEST_CASE("blur kinds: zero amount is a byte-level no-op") {
    core::Document ref(64, 48);
    seedBlurScene(ref);
    const common::Image before = flatten(ref);

    using enum core::AdjustmentKind;
    const std::pair<core::AdjustmentKind, std::map<std::string, double>> zeroed[] = {
        {GaussianBlur, {{"radius", 0.0}}},
        {BoxBlur, {{"radius", 0.0}}},
        {MotionBlur, {{"angle", 30.0}, {"distance", 0.0}}},
        {RadialBlur, {{"mode", 0.0}, {"amount", 0.0}, {"center_x", 32.0}, {"center_y", 24.0}}},
        {LensBlur, {{"radius", 0.0}, {"blades", 6.0}}},
        {DofBlur, {{"radius", 0.0}, {"band", 10.0}, {"feather", 20.0}}},
        // SurfaceBlur is absent on purpose: its schema floors radius at 1 px, so it has no
        // zero state -- like Threshold, it is visible at every legal parameter value.
    };
    for (const auto& [kind, params] : zeroed) {
        CAPTURE(core::adjustmentKindName(kind));
        core::Document doc(64, 48);
        seedBlurScene(doc);
        addAdjustment(doc, kind, params);
        CHECK(imagesEqual(flatten(doc), before));
    }
}

TEST_CASE("blur kinds: seeded defaults are VISIBLE (the anti-broken-promise rule)") {
    core::Document ref(64, 48);
    seedBlurScene(ref);
    const common::Image before = flatten(ref);
    using enum core::AdjustmentKind;
    for (const auto kind :
         {GaussianBlur, BoxBlur, MotionBlur, RadialBlur, SurfaceBlur, LensBlur, DofBlur}) {
        CAPTURE(core::adjustmentKindName(kind));
        core::Document doc(64, 48);
        seedBlurScene(doc);
        core::AdjustmentLayer* adj = addAdjustment(doc, kind, {});
        core::seedAdjustmentDefaults(*adj);
        if (adj->params().contains("center_x")) { // the menu-insert center seeding
            adj->params()["center_x"] = 32.0;
            adj->params()["center_y"] = 24.0;
        }
        if (kind == DofBlur) { // defaults' band would cover this small canvas: tighten it
            adj->params()["band"] = 4.0;
            adj->params()["feather"] = 4.0;
        }
        CHECK_FALSE(imagesEqual(flatten(doc), before));
    }
}

TEST_CASE("a blur diffuses alpha and color together") {
    // An opaque square on a TRANSPARENT canvas: after a Gaussian blur layer, coverage must
    // spread beyond the original footprint (a color adjustment never touches alpha; a blur
    // must -- docs/blur-filters.md §2).
    core::Document doc(48, 48);
    auto base = doc.makeRaster("sq", 48, 48);
    for (std::uint32_t y = 20; y < 28; ++y)
        for (std::uint32_t x = 20; x < 28; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * 48 + x) * 4;
            base->image().rgba[p] = 255;
            base->image().rgba[p + 1] = 255;
            base->image().rgba[p + 2] = 255;
            base->image().rgba[p + 3] = 255;
        }
    doc.root().addOnTop(std::move(base));
    addAdjustment(doc, core::AdjustmentKind::GaussianBlur, {{"radius", 6.0}});
    const common::Image out = flatten(doc);
    CHECK(px(out, 17, 24).a > 0);   // alpha crossed the old footprint edge
    CHECK(px(out, 24, 17).a > 0);
    CHECK(px(out, 24, 24).a < 255); // the center lost coverage to the spread
    CHECK(px(out, 2, 2).a == 0);    // far field stays empty (3-sigma support)
}

TEST_CASE("a mask gates a blur byte-exactly (the DoF cut-out workflow)") {
    core::Document ref(64, 48);
    seedBlurScene(ref);
    const common::Image unblurred = flatten(ref);

    core::Document blurRef(64, 48);
    seedBlurScene(blurRef);
    addAdjustment(blurRef, core::AdjustmentKind::GaussianBlur, {{"radius", 5.0}});
    const common::Image blurredFull = flatten(blurRef);

    core::Document doc(64, 48);
    seedBlurScene(doc);
    core::AdjustmentLayer* adj =
        addAdjustment(doc, core::AdjustmentKind::GaussianBlur, {{"radius", 5.0}});
    core::RasterMask mask(64, 48, 255);
    for (std::uint32_t y = 0; y < 48; ++y) // left half masked out
        for (std::uint32_t x = 0; x < 32; ++x)
            mask.coverage[static_cast<std::size_t>(y) * 64 + x] = 0;
    adj->setMask(std::move(mask));
    const common::Image out = flatten(doc);

    for (std::uint32_t y = 0; y < 48; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
            const common::Color8 got = px(out, x, y);
            const common::Color8 want = x < 32 ? px(unblurred, x, y) : px(blurredFull, x, y);
            REQUIRE(got.r == want.r);
            REQUIRE(got.g == want.g);
            REQUIRE(got.b == want.b);
            REQUIRE(got.a == want.a);
        }
    }
}

TEST_CASE("region composite == crop(full) under blur reach expansion") {
    // THE money test of the reach machinery (docs/blur-filters.md §5): the S60-a dirty-rect
    // path must stay byte-identical to the full composite with blurs anywhere in the stack.
    const auto checkRegion = [](core::Document& doc, const common::Rect& roi) {
        const common::Image full = flatten(doc);
        const render::CompositeResult r =
            render::compositeRegion(doc, roi, {}, render::Backend::Cpu);
        REQUIRE(r.ok);
        const auto x0 = static_cast<std::uint32_t>(roi.x);
        const auto y0 = static_cast<std::uint32_t>(roi.y);
        for (std::uint32_t y = 0; y < r.image.height; ++y)
            for (std::uint32_t x = 0; x < r.image.width; ++x) {
                const common::Color8 got = px(r.image, x, y);
                const common::Color8 want = px(full, x0 + x, y0 + y);
                REQUIRE(got.r == want.r);
                REQUIRE(got.g == want.g);
                REQUIRE(got.b == want.b);
                REQUIRE(got.a == want.a);
            }
    };

    SUBCASE("root Gaussian") {
        core::Document doc(64, 48);
        seedBlurScene(doc);
        addAdjustment(doc, core::AdjustmentKind::GaussianBlur, {{"radius", 6.0}});
        checkRegion(doc, {24.0, 12.0, 16.0, 16.0});
    }
    SUBCASE("root Radial spin, off-center") {
        core::Document doc(64, 48);
        seedBlurScene(doc);
        addAdjustment(doc, core::AdjustmentKind::RadialBlur,
                      {{"mode", 0.0}, {"amount", 25.0}, {"center_x", 10.0}, {"center_y", 40.0}});
        checkRegion(doc, {30.0, 8.0, 20.0, 14.0});
    }
    SUBCASE("root Zoom") {
        core::Document doc(64, 48);
        seedBlurScene(doc);
        addAdjustment(doc, core::AdjustmentKind::RadialBlur,
                      {{"mode", 1.0}, {"amount", 30.0}, {"center_x", 32.0}, {"center_y", 24.0}});
        checkRegion(doc, {4.0, 4.0, 18.0, 18.0});
    }
    SUBCASE("stacked blurs compound their reach") {
        core::Document doc(64, 48);
        seedBlurScene(doc);
        addAdjustment(doc, core::AdjustmentKind::GaussianBlur, {{"radius", 4.0}});
        addAdjustment(doc, core::AdjustmentKind::BoxBlur, {{"radius", 3.0}});
        checkRegion(doc, {26.0, 14.0, 12.0, 12.0});
    }
    SUBCASE("masked root blur (the mask must not shift under the crop)") {
        core::Document doc(64, 48);
        seedBlurScene(doc);
        core::AdjustmentLayer* adj =
            addAdjustment(doc, core::AdjustmentKind::GaussianBlur, {{"radius", 5.0}});
        core::RasterMask mask(64, 48, 255);
        for (std::uint32_t y = 0; y < 48; ++y)
            for (std::uint32_t x = 0; x < 32; ++x)
                mask.coverage[static_cast<std::size_t>(y) * 64 + x] = 0;
        adj->setMask(std::move(mask));
        checkRegion(doc, {24.0, 10.0, 20.0, 20.0}); // straddles the mask edge
    }
    SUBCASE("group-nested blur (the local-extent growth)") {
        core::Document doc(64, 48);
        auto group = doc.makeGroup("g");
        auto inner = doc.makeRaster("inner", 64, 48);
        for (std::uint32_t y = 10; y < 30; ++y)
            for (std::uint32_t x = 10; x < 30; ++x) {
                const std::size_t p = (static_cast<std::size_t>(y) * 64 + x) * 4;
                inner->image().rgba[p] = 240;
                inner->image().rgba[p + 1] = 200;
                inner->image().rgba[p + 2] = 40;
                inner->image().rgba[p + 3] = 255;
            }
        group->addOnTop(std::move(inner));
        auto adj = doc.makeAdjustment("blur", core::AdjustmentKind::GaussianBlur);
        adj->params() = {{"radius", 5.0}};
        group->addOnTop(std::move(adj));
        doc.root().addOnTop(std::move(group));
        checkRegion(doc, {6.0, 6.0, 20.0, 20.0}); // covers the blurred spill past content
    }
}

TEST_CASE("Depth of Field: the focus band is untouched, the far field is fully blurred") {
    // Horizontal focus line through the center (angle 0 -> the band measures |dy|): rows inside
    // band half-width are byte-identical to no layer at all; rows past band+feather equal the
    // same scene under a plain Gaussian of the same radius (the pyramid's top level -- byte-
    // equal through the amt==1 fast path).
    core::Document ref(64, 48);
    seedBlurScene(ref);
    const common::Image unblurred = flatten(ref);

    core::Document gaussRef(64, 48);
    seedBlurScene(gaussRef);
    addAdjustment(gaussRef, core::AdjustmentKind::GaussianBlur, {{"radius", 6.0}});
    const common::Image fullBlur = flatten(gaussRef);

    core::Document doc(64, 48);
    seedBlurScene(doc);
    addAdjustment(doc, core::AdjustmentKind::DofBlur,
                  {{"radius", 6.0},
                   {"band", 6.0},
                   {"feather", 4.0},
                   {"angle", 0.0},
                   {"center_x", 32.0},
                   {"center_y", 24.0},
                   {"bokeh", 0.0}});
    const common::Image out = flatten(doc);

    for (std::uint32_t x = 0; x < 64; ++x) {
        // |py - 24| with py = y + 0.5: rows 19..28 sit strictly inside the 6 px band.
        for (const std::uint32_t y : {20u, 24u, 27u}) {
            const common::Color8 got = px(out, x, y);
            const common::Color8 want = px(unblurred, x, y);
            REQUIRE(got.r == want.r);
            REQUIRE(got.g == want.g);
            REQUIRE(got.b == want.b);
            REQUIRE(got.a == want.a);
        }
        // rows 0..13 and 35..47 are past band+feather = 10 px: the saturated plateau.
        for (const std::uint32_t y : {2u, 10u, 40u, 46u}) {
            const common::Color8 got = px(out, x, y);
            const common::Color8 want = px(fullBlur, x, y);
            REQUIRE(got.r == want.r);
            REQUIRE(got.g == want.g);
            REQUIRE(got.b == want.b);
            REQUIRE(got.a == want.a);
        }
    }
}

TEST_CASE("scope previews scale blur radii with the buffer (two-resolution equivalence)") {
    // The same scene authored at 2x and 1x resolution, blurs scaled to match: the 2x doc's
    // HALF-size preview must agree with the 1x doc's full-size composite. Without the §4 scale
    // threading the preview would blur at double strength. Tolerance absorbs the preview walk's
    // Area-filter resample of the block-constant scene (exact blocks -> tiny residue).
    core::Document big(64, 48);
    {
        auto base = big.makeRaster("b", 64, 48);
        for (std::uint32_t y = 0; y < 48; ++y)
            for (std::uint32_t x = 0; x < 64; ++x) {
                const std::size_t p = (static_cast<std::size_t>(y) * 64 + x) * 4;
                const bool on = (x / 16 + y / 16) % 2 == 0; // 16 px checker: block-constant
                base->image().rgba[p] = on ? 230 : 40;
                base->image().rgba[p + 1] = on ? 230 : 40;
                base->image().rgba[p + 2] = on ? 90 : 180;
                base->image().rgba[p + 3] = 255;
            }
        big.root().addOnTop(std::move(base));
    }
    core::AdjustmentLayer* bigAdj =
        addAdjustment(big, core::AdjustmentKind::GaussianBlur, {{"radius", 8.0}});
    const common::Image preview = render::adjustmentPreview(*bigAdj, 64, 48, 32, 24);
    REQUIRE(!preview.empty());

    core::Document small(32, 24);
    {
        auto base = small.makeRaster("s", 32, 24);
        for (std::uint32_t y = 0; y < 24; ++y)
            for (std::uint32_t x = 0; x < 32; ++x) {
                const std::size_t p = (static_cast<std::size_t>(y) * 32 + x) * 4;
                const bool on = (x / 8 + y / 8) % 2 == 0;
                base->image().rgba[p] = on ? 230 : 40;
                base->image().rgba[p + 1] = on ? 230 : 40;
                base->image().rgba[p + 2] = on ? 90 : 180;
                base->image().rgba[p + 3] = 255;
            }
        small.root().addOnTop(std::move(base));
    }
    addAdjustment(small, core::AdjustmentKind::GaussianBlur, {{"radius", 4.0}});
    const common::Image direct = flatten(small);

    double sum = 0.0;
    for (std::size_t i = 0; i < preview.rgba.size(); ++i)
        sum += std::abs(static_cast<int>(preview.rgba[i]) - static_cast<int>(direct.rgba[i]));
    const double mean = sum / static_cast<double>(preview.rgba.size());
    CHECK(mean < 4.0); // unscaled radii put this in the tens
}

TEST_CASE("blur golden pins") {
    // One deterministic scene per kind, hashes pinned (the S55 golden convention): any
    // unintended kernel/blend change trips these. Re-bless deliberately, with a comment.
    using enum core::AdjustmentKind;
    const std::pair<core::AdjustmentKind, std::map<std::string, double>> cases[] = {
        {GaussianBlur, {{"radius", 6.0}}},
        {BoxBlur, {{"radius", 4.0}}},
        {MotionBlur, {{"angle", 30.0}, {"distance", 12.0}}},
        {RadialBlur, {{"mode", 0.0}, {"amount", 20.0}, {"center_x", 32.0}, {"center_y", 24.0}}},
        {SurfaceBlur, {{"radius", 6.0}, {"threshold", 20.0}}},
        {LensBlur,
         {{"radius", 8.0}, {"blades", 6.0}, {"curvature", 30.0}, {"rotation", 15.0},
          {"boost", 40.0}, {"boost_threshold", 70.0}}},
        {DofBlur,
         {{"radius", 6.0}, {"band", 5.0}, {"feather", 6.0}, {"angle", 20.0},
          {"center_x", 32.0}, {"center_y", 24.0}, {"bokeh", 1.0}}},
    };
    std::vector<std::uint64_t> hashes;
    for (const auto& [kind, params] : cases) {
        core::Document doc(64, 48);
        seedBlurScene(doc);
        addAdjustment(doc, kind, params);
        const common::Image out = flatten(doc);
        hashes.push_back(fnv1a(out.rgba.data(), out.rgba.size()));
    }
    REQUIRE(hashes.size() == 7);
    CAPTURE(hashes[0]);
    CAPTURE(hashes[1]);
    CAPTURE(hashes[2]);
    CAPTURE(hashes[3]);
    CAPTURE(hashes[4]);
    CAPTURE(hashes[5]);
    CAPTURE(hashes[6]);
    CHECK(hashes[0] == 14534353921952774300ull); // Gaussian -- S33 initial bless
    CHECK(hashes[1] == 10728610032150922959ull); // Box
    CHECK(hashes[2] == 243999122878526659ull);   // Motion
    CHECK(hashes[3] == 6534982011210028259ull);  // Radial spin
    CHECK(hashes[4] == 8873746476341734358ull);  // Surface
    CHECK(hashes[5] == 9963595668395696797ull);  // Lens (hex iris, boost on)
    CHECK(hashes[6] == 17140248179955071085ull); // Depth of Field (iris levels, angled band)
}

// ---------------------------------------------------------------------------------------------
// S34: Shadows/Highlights, Defringe, Matte Removal, Haze Removal
// ---------------------------------------------------------------------------------------------

TEST_CASE("S34 kinds: seeded defaults are a byte-level no-op") {
    // Unlike the blur family these are REPAIRS -- which end needs repairing is the photograph's
    // business -- so they follow the §1 identity-at-defaults rule rather than the "inherently
    // visible" deviation. Matte Removal is the exception and is checked separately below.
    using enum core::AdjustmentKind;
    core::Document ref(64, 48);
    seedBlurScene(ref);
    const common::Image before = flatten(ref);
    for (const auto kind : {ShadowsHighlights, Defringe, HazeRemoval}) {
        CAPTURE(core::adjustmentKindName(kind));
        core::Document doc(64, 48);
        seedBlurScene(doc);
        core::AdjustmentLayer* adj = addAdjustment(doc, kind, {});
        core::seedAdjustmentDefaults(*adj);
        if (adj->params().contains("center_x")) { // the menu-insert center seeding
            adj->params()["center_x"] = 32.0;
            adj->params()["center_y"] = 24.0;
        }
        CHECK(imagesEqual(flatten(doc), before)); // seeded defaults
        adj->params().clear();
        CHECK(imagesEqual(flatten(doc), before)); // and an empty bag reads the same defaults
    }
}

namespace {

// A 64x48 card whose LEFT half is black and RIGHT half white, with an identical mid-gray patch
// centred in each: the same pixel value in two different neighbourhoods, which is exactly what
// separates a local tone repair from a global gamma.
void seedLocalityCard(core::Document& doc) {
    auto base = doc.makeRaster("base", 64, 48);
    common::Image& img = base->image();
    for (std::uint32_t y = 0; y < 48; ++y)
        for (std::uint32_t x = 0; x < 64; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * 64 + x) * 4;
            const auto v = static_cast<std::uint8_t>(x < 32 ? 0 : 255);
            img.rgba[p] = img.rgba[p + 1] = img.rgba[p + 2] = v;
            img.rgba[p + 3] = 255;
        }
    for (const std::uint32_t cx : {std::uint32_t{12}, std::uint32_t{48}})
        for (std::uint32_t y = 20; y < 28; ++y)
            for (std::uint32_t x = cx - 4; x < cx + 4; ++x) {
                const std::size_t p = (static_cast<std::size_t>(y) * 64 + x) * 4;
                img.rgba[p] = img.rgba[p + 1] = img.rgba[p + 2] = 90;
                img.rgba[p + 3] = 255;
            }
    doc.root().addOnTop(std::move(base));
}

} // namespace

TEST_CASE("Shadows/Highlights: the LIFT is local -- the mask, not the pixel, decides") {
    core::Document doc(64, 48);
    seedLocalityCard(doc);
    addAdjustment(doc, core::AdjustmentKind::ShadowsHighlights,
                  {{"shadows", 100.0}, {"shadows_tone", 50.0}, {"radius", 20.0}});
    const common::Image out = flatten(doc);
    const int dark = px(out, 12, 24).r;   // the patch inside the dark surround
    const int bright = px(out, 48, 24).r; // the identical patch inside the bright surround
    // The patch on black sits under a dark mask -> a strong lift; the one on white sits under a
    // mask past the shadow range -> weight 0 -> exponent exactly 1 -> BYTE-identical.
    CHECK(bright == 90);
    CHECK(dark > 120);
    CHECK(dark > bright + 20);
    // Pure black and pure white are the curve's fixed points whatever the exponent.
    CHECK(px(out, 2, 2) == common::Color8{0, 0, 0, 255});
    CHECK(px(out, 61, 2) == common::Color8{255, 255, 255, 255});
}

TEST_CASE("Shadows/Highlights: the highlight arm recovers, and the two arms are independent") {
    core::Document ref(64, 48);
    seedLocalityCard(ref);
    const common::Image before = flatten(ref);
    core::Document doc(64, 48);
    seedLocalityCard(doc);
    addAdjustment(doc, core::AdjustmentKind::ShadowsHighlights,
                  {{"highlights", 100.0}, {"highlights_tone", 50.0}, {"radius", 20.0}});
    const common::Image out = flatten(doc);
    CHECK(px(out, 48, 24).r < px(before, 48, 24).r); // the patch in the bright surround darkens
    CHECK(px(out, 12, 24).r == px(before, 12, 24).r); // the one in the dark surround is untouched
}

TEST_CASE("Shadows/Highlights: a mask gates it byte-exactly") {
    core::Document ref(64, 48);
    seedLocalityCard(ref);
    const common::Image plain = flatten(ref);
    core::Document lit(64, 48);
    seedLocalityCard(lit);
    addAdjustment(lit, core::AdjustmentKind::ShadowsHighlights,
                  {{"shadows", 100.0}, {"shadows_tone", 50.0}, {"radius", 20.0}});
    const common::Image full = flatten(lit);

    core::Document doc(64, 48);
    seedLocalityCard(doc);
    core::AdjustmentLayer* adj =
        addAdjustment(doc, core::AdjustmentKind::ShadowsHighlights,
                      {{"shadows", 100.0}, {"shadows_tone", 50.0}, {"radius", 20.0}});
    core::RasterMask mask(64, 48, 0); // reveal the left half only
    for (std::uint32_t y = 0; y < 48; ++y)
        for (std::uint32_t x = 0; x < 32; ++x)
            mask.coverage[static_cast<std::size_t>(y) * 64 + x] = 255;
    adj->setMask(std::move(mask));
    const common::Image out = flatten(doc);
    for (std::uint32_t y = 0; y < 48; ++y)
        for (std::uint32_t x = 0; x < 64; ++x) {
            const common::Color8 want = x < 32 ? px(full, x, y) : px(plain, x, y);
            REQUIRE(px(out, x, y) == want); // masked-out pixels are the untouched backdrop
        }
}

namespace {

// A 3x1 card: a violet fringe colour (hue ~285 deg -- the purple band's centre), pure blue
// (hue 240, outside the band) and pure green (the green band's centre).
void seedFringeCard(core::Document& doc) {
    auto base = doc.makeRaster("base", 3, 1);
    base->image().rgba = {180, 60, 220, 255, 0, 0, 255, 255, 0, 255, 0, 255};
    doc.root().addOnTop(std::move(base));
}

} // namespace

TEST_CASE("Defringe: the purple band desaturates, neighbouring hues stay BYTE-identical") {
    core::Document ref(3, 1);
    seedFringeCard(ref);
    const common::Image before = flatten(ref);
    core::Document doc(3, 1);
    seedFringeCard(doc);
    addAdjustment(doc, core::AdjustmentKind::Defringe,
                  {{"purple", 100.0}, {"green", 0.0}, {"threshold", 0.0}});
    const common::Image out = flatten(doc);
    // The violet patch collapses onto its own lightness: (0.8627 + 0.2353)/2 = 0.549 -> 140.
    const common::Color8 fixed = px(out, 0, 0);
    CHECK(std::abs(static_cast<int>(fixed.r) - 140) <= 2);
    // "Neutral" to within the 8-bit lattice: full suppression leaves s ~ 5e-4, not exactly 0
    // (the band weight is a smoothstep, and the patch sits a hair off the band's centre).
    CHECK(std::abs(static_cast<int>(fixed.r) - static_cast<int>(fixed.g)) <= 1);
    CHECK(std::abs(static_cast<int>(fixed.g) - static_cast<int>(fixed.b)) <= 1);
    CHECK(fixed.a == 255); // alpha is never touched
    // Blue is 0.125 turns away -- outside the band's ~0.11 half-width -- so the pixel never
    // enters the HSL round trip at all.
    CHECK(px(out, 1, 0) == px(before, 1, 0));
    CHECK(px(out, 2, 0) == px(before, 2, 0)); // green needs the GREEN slider, not this one
}

TEST_CASE("Defringe: the green band is its own slider, and the chroma threshold protects") {
    core::Document ref(3, 1);
    seedFringeCard(ref);
    const common::Image before = flatten(ref);
    core::Document doc(3, 1);
    seedFringeCard(doc);
    addAdjustment(doc, core::AdjustmentKind::Defringe, {{"green", 100.0}, {"threshold", 0.0}});
    const common::Image out = flatten(doc);
    CHECK(px(out, 2, 0).r == px(out, 2, 0).g); // the green patch went neutral
    CHECK(px(out, 0, 0) == px(before, 0, 0));  // the violet one did not move

    // A threshold above the patch's chroma gates the whole thing off: fully saturated patches
    // have s == 1, so only a threshold at the very top can suppress them -- checked the other
    // way round here, with a threshold that leaves a MID-chroma pixel alone.
    core::Document soft(1, 1);
    auto base = soft.makeRaster("base", 1, 1);
    base->image().rgba = {150, 120, 165, 255}; // a faint violet: s is small
    soft.root().addOnTop(std::move(base));
    const common::Image softBefore = flatten(soft);
    addAdjustment(soft, core::AdjustmentKind::Defringe,
                  {{"purple", 100.0}, {"threshold", 80.0}});
    CHECK(imagesEqual(flatten(soft), softBefore));
}

TEST_CASE("Defringe: the lateral-CA rescale moves the red channel and nothing else") {
    core::Document ref(64, 48);
    seedBlurScene(ref);
    const common::Image before = flatten(ref);
    core::Document doc(64, 48);
    seedBlurScene(doc);
    addAdjustment(doc, core::AdjustmentKind::Defringe,
                  {{"ca_red", 1.0}, {"center_x", 32.0}, {"center_y", 24.0}});
    const common::Image out = flatten(doc);
    CHECK_FALSE(imagesEqual(out, before));
    bool greenMoved = false;
    bool blueMoved = false;
    bool redMoved = false;
    for (std::uint32_t y = 0; y < 48; ++y)
        for (std::uint32_t x = 0; x < 64; ++x) {
            greenMoved = greenMoved || px(out, x, y).g != px(before, x, y).g;
            blueMoved = blueMoved || px(out, x, y).b != px(before, x, y).b;
            redMoved = redMoved || px(out, x, y).r != px(before, x, y).r;
        }
    CHECK(redMoved);
    CHECK_FALSE(greenMoved); // green is the reference channel: it never moves
    CHECK_FALSE(blueMoved);  // ... and ca_blue was left at zero
}

TEST_CASE("region composite == crop(full) under the S34 spatial kinds") {
    const auto checkRegion = [](core::Document& doc, const common::Rect& roi) {
        const common::Image full = flatten(doc);
        const render::CompositeResult r =
            render::compositeRegion(doc, roi, {}, render::Backend::Cpu);
        REQUIRE(r.ok);
        const auto x0 = static_cast<std::uint32_t>(roi.x);
        const auto y0 = static_cast<std::uint32_t>(roi.y);
        for (std::uint32_t y = 0; y < r.image.height; ++y)
            for (std::uint32_t x = 0; x < r.image.width; ++x)
                REQUIRE(px(r.image, x, y) == px(full, x0 + x, y0 + y));
    };
    SUBCASE("Shadows/Highlights (the blurred mask's reach)") {
        core::Document doc(64, 48);
        seedBlurScene(doc);
        addAdjustment(doc, core::AdjustmentKind::ShadowsHighlights,
                      {{"shadows", 80.0}, {"highlights", 40.0}, {"radius", 12.0}});
        checkRegion(doc, {24.0, 12.0, 16.0, 16.0});
    }
    SUBCASE("Defringe with lateral CA (a center-dependent reach)") {
        core::Document doc(64, 48);
        seedBlurScene(doc);
        addAdjustment(doc, core::AdjustmentKind::Defringe,
                      {{"purple", 60.0}, {"ca_red", 1.0}, {"ca_blue", -1.0},
                       {"center_x", 10.0}, {"center_y", 40.0}});
        checkRegion(doc, {30.0, 8.0, 20.0, 14.0});
    }
    SUBCASE("stacked with a blur (reaches sum)") {
        core::Document doc(64, 48);
        seedBlurScene(doc);
        addAdjustment(doc, core::AdjustmentKind::GaussianBlur, {{"radius", 4.0}});
        addAdjustment(doc, core::AdjustmentKind::ShadowsHighlights,
                      {{"shadows", 100.0}, {"radius", 10.0}});
        checkRegion(doc, {26.0, 14.0, 12.0, 12.0});
    }
}

namespace {

// A 1x1 card carrying a known straight colour + coverage, for the matte algebra.
void seedMatteCard(core::Document& doc, std::uint8_t v, std::uint8_t a) {
    auto base = doc.makeRaster("base", 1, 1);
    base->image().rgba = {v, v, v, a};
    doc.root().addOnTop(std::move(base));
}

} // namespace

TEST_CASE("Matte Removal: the four modes are the compositing algebra, alpha untouched") {
    // a = 128/255 = 0.501961 throughout; the expectations are computed from that, so they are
    // the formula restated rather than a golden.
    SUBCASE("remove white matte") {
        core::Document doc(1, 1);
        seedMatteCard(doc, 204, 128); // 0.8 composited over white at 50% coverage
        addAdjustment(doc, core::AdjustmentKind::MatteRemoval,
                      {{"mode", static_cast<double>(core::MatteMode::RemoveWhite)}});
        const common::Color8 c = px(flatten(doc), 0, 0);
        CHECK(std::abs(static_cast<int>(c.r) - 153) <= 2); // (0.8 - 0.498039)/0.501961 = 0.6016
        CHECK(c.a == 128);                                 // coverage is never rewritten
    }
    SUBCASE("remove black matte == unpremultiply") {
        core::Document a(1, 1);
        seedMatteCard(a, 102, 128);
        addAdjustment(a, core::AdjustmentKind::MatteRemoval,
                      {{"mode", static_cast<double>(core::MatteMode::RemoveBlack)}});
        core::Document b(1, 1);
        seedMatteCard(b, 102, 128);
        addAdjustment(b, core::AdjustmentKind::MatteRemoval,
                      {{"mode", static_cast<double>(core::MatteMode::Unpremultiply)}});
        const common::Color8 ca = px(flatten(a), 0, 0);
        CHECK(ca == px(flatten(b), 0, 0));                 // the same algebra, both names
        CHECK(std::abs(static_cast<int>(ca.r) - 203) <= 2); // 0.4/0.501961 = 0.7969
        CHECK(ca.a == 128);
    }
    SUBCASE("premultiply") {
        core::Document doc(1, 1);
        seedMatteCard(doc, 102, 128);
        addAdjustment(doc, core::AdjustmentKind::MatteRemoval,
                      {{"mode", static_cast<double>(core::MatteMode::Premultiply)}});
        const common::Color8 c = px(flatten(doc), 0, 0);
        CHECK(std::abs(static_cast<int>(c.r) - 51) <= 2); // 0.4 * 0.501961 = 0.2008
        CHECK(c.a == 128);
    }
    SUBCASE("over an OPAQUE backdrop every mode is the identity") {
        // The 6x1 card is fully opaque throughout: with a == 1 all four transfers collapse to
        // the identity, which is the honest answer -- there is no matte to remove.
        core::Document plain(6, 1);
        seedTestCard(plain);
        const common::Image before = flatten(plain);
        for (int mode = 0; mode < 4; ++mode) {
            CAPTURE(mode);
            core::Document doc(6, 1);
            seedTestCard(doc);
            addAdjustment(doc, core::AdjustmentKind::MatteRemoval,
                          {{"mode", static_cast<double>(mode)}});
            CHECK(flatten(doc).rgba == before.rgba);
        }
    }
}

TEST_CASE("Haze Removal: amount 0 is a no-op; a positive amount stretches about the airlight") {
    core::Document plain(6, 1);
    seedTestCard(plain);
    const common::Image before = flatten(plain);
    core::Document zero(6, 1);
    seedTestCard(zero);
    addAdjustment(zero, core::AdjustmentKind::HazeRemoval,
                  {{"amount", 0.0}, {"airlight", 95.0}, {"tint", 0.0}, {"saturation", 100.0}});
    CHECK(flatten(zero).rgba == before.rgba);

    core::Document doc(6, 1);
    seedTestCard(doc);
    addAdjustment(doc, core::AdjustmentKind::HazeRemoval,
                  {{"amount", 50.0}, {"airlight", 95.0}, {"tint", 0.0}, {"saturation", 100.0}});
    const common::Image out = flatten(doc);
    // t = 1 - 0.9*0.5 = 0.55, so J = 0.95 + (I - 0.95)/0.55. Mid-gray 0.502 -> 0.1345 -> 34.
    CHECK(std::abs(static_cast<int>(px(out, 2, 0).r) - 34) <= 2);
    CHECK(px(out, 1, 0) == common::Color8{255, 255, 255, 255}); // white clips back to white
    CHECK(px(out, 0, 0) == common::Color8{0, 0, 0, 255});       // black stays black
    CHECK(px(out, 2, 0).a == 255);                              // alpha untouched

    // A warm tint pushes the red airlight up, which pulls the recovered red DOWN relative to
    // neutral -- the mid-gray patch must no longer come out neutral.
    core::Document tinted(6, 1);
    seedTestCard(tinted);
    addAdjustment(tinted, core::AdjustmentKind::HazeRemoval,
                  {{"amount", 50.0}, {"airlight", 95.0}, {"tint", 100.0},
                   {"saturation", 100.0}});
    const common::Color8 t = px(flatten(tinted), 2, 0);
    CHECK(t.r != t.b);
}

// ---------------------------------------------------------------------------------------------
// S34-a: the remainder of the galleries (docs/adjustment-layers.md §2.6-§2.9). Gradient Map,
// Vibrance and Photo Filter are per-pixel colour transfers; High Pass is the unsharp mask's own
// Gaussian difference, so it rides the S35 stylize branch and owes the region invariant.
// ---------------------------------------------------------------------------------------------

namespace {

// A gradient map ramp of two opaque stops a(0) -> b(1).
core::vec::Gradient rampOf(common::ColorF a, common::ColorF b) {
    core::vec::Gradient g = core::defaultGradientMap();
    g.stops = {{0.0, a, 0.5}, {1.0, b, 0.5}};
    return g;
}

// The linear-light Rec 709 luminance of an 8-bit colour, computed from the analytic sRGB curve --
// the quantity Photo Filter's "preserve luminosity" promises to hold constant.
double linearLuminance(common::Color8 c) {
    return 0.2126 * srgbDecode(c.r / 255.0) + 0.7152 * srgbDecode(c.g / 255.0) +
           0.0722 * srgbDecode(c.b / 255.0);
}

} // namespace

TEST_CASE("Gradient Map storage: stops round-trip through the double bag, absent == the default") {
    std::map<std::string, double> bag;
    // Nothing stored is NOT an identity here -- it is the classic black-to-white ramp, because a
    // gradient map has no identity setting to spell (§2.6).
    CHECK(core::adjustmentGradientMap(bag).stops == core::defaultGradientMap().stops);

    const core::vec::Gradient duotone =
        rampOf({0.1F, 0.0F, 0.3F, 1.0F}, {1.0F, 0.85F, 0.2F, 1.0F});
    core::setAdjustmentGradientMap(bag, duotone);
    CHECK(bag.at("gm_n") == 2.0);
    CHECK(bag.at("gm_0_t") == 0.0);
    CHECK(bag.count("gm_0_m") == 0); // a straight linear blend writes no midpoint key
    const core::vec::Gradient back = core::adjustmentGradientMap(bag);
    REQUIRE(back.stops.size() == 2);
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(back.stops[i].offset == duotone.stops[i].offset); // doubles: exact, not approximate
        CHECK(back.stops[i].color == duotone.stops[i].color);
        CHECK(back.stops[i].midpoint == duotone.stops[i].midpoint);
    }
    // Re-encoding the decoded ramp reproduces the same bag byte-for-byte: that IS the .mosaic
    // round trip, since docio writes the bag as a JSON number map and reads it straight back.
    std::map<std::string, double> again;
    core::setAdjustmentGradientMap(again, back);
    CHECK(again == bag);

    // A blend midpoint off 0.5 is stored; a shorter ramp leaves no tail; the default ERASES.
    core::vec::Gradient biased = duotone;
    biased.stops[0].midpoint = 0.25;
    core::vec::Gradient three = duotone;
    three.stops.insert(three.stops.begin() + 1, {0.5, {0.5F, 0.5F, 0.5F, 1.0F}, 0.5});
    core::setAdjustmentGradientMap(bag, three);
    CHECK(bag.at("gm_n") == 3.0);
    core::setAdjustmentGradientMap(bag, biased);
    CHECK(bag.at("gm_n") == 2.0);
    CHECK(bag.at("gm_0_m") == 0.25);
    CHECK(bag.count("gm_2_t") == 0);
    core::setAdjustmentGradientMap(bag, core::defaultGradientMap());
    CHECK(bag.empty()); // absent is how the default ramp is spelled
}

TEST_CASE("Gradient Map storage: a corrupt or hostile bag degrades to a sane ramp") {
    std::map<std::string, double> bag;
    bag["gm_n"] = 1e9;    // absurd count: the decode is capped, never unbounded
    bag["gm_0_t"] = 5.0;  // outside [0,1]: clamped in
    bag["gm_0_r"] = -2.0; // ... and so is the colour
    bag["gm_0_g"] = 0.0;
    bag["gm_0_b"] = 0.0;
    bag["gm_0_a"] = 1.0;
    bag["gm_1_t"] = 0.0;  // stored OUT OF ORDER: the decode must sort, sampleStops assumes it
    bag["gm_1_r"] = 1.0;
    bag["gm_1_g"] = 1.0;
    bag["gm_1_b"] = 1.0;
    bag["gm_1_a"] = 1.0;
    const core::vec::Gradient g = core::adjustmentGradientMap(bag);
    REQUIRE(g.stops.size() == 2);
    CHECK(g.stops[0].offset == 0.0);
    CHECK(g.stops[1].offset == 1.0);
    CHECK(g.stops[0].color.r == 1.0F); // the white stop sorted to the front
    CHECK(g.stops[1].color.r == 0.0F); // the clamped black one to the back

    // One usable stop is not a ramp -> the default, not a constant that flattens the image.
    std::map<std::string, double> lone;
    lone["gm_n"] = 2.0;
    lone["gm_0_t"] = 0.0;
    lone["gm_0_r"] = 1.0; // knot 1 is missing entirely
    lone["gm_0_g"] = 0.0;
    lone["gm_0_b"] = 0.0;
    lone["gm_0_a"] = 1.0;
    CHECK(core::adjustmentGradientMap(lone).stops == core::defaultGradientMap().stops);
}

TEST_CASE("Gradient Map: luma indexes the ramp; the endpoints are exact") {
    core::Document plain(6, 1);
    seedTestCard(plain);
    const common::Image before = flatten(plain);

    // The DEFAULT ramp is real work, not a no-op: it is the luma projection, so a fresh layer is
    // VISIBLE (the Threshold/Posterize class) and every pixel comes out neutral.
    core::Document def(6, 1);
    seedTestCard(def);
    core::AdjustmentLayer* d = addAdjustment(def, core::AdjustmentKind::GradientMap, {});
    core::seedAdjustmentDefaults(*d);
    const common::Image gray = flatten(def);
    CHECK_FALSE(imagesEqual(gray, before));
    for (std::uint32_t x = 0; x < 6; ++x) {
        CAPTURE(x);
        CHECK(px(gray, x, 0).r == px(gray, x, 0).g);
        CHECK(px(gray, x, 0).g == px(gray, x, 0).b);
        CHECK(px(gray, x, 0).a == 255); // alpha is never touched
    }
    CHECK(px(gray, 0, 0).r == 0);   // black -> the ramp's black end
    CHECK(px(gray, 1, 0).r == 255); // white -> its white end

    // A black -> red ramp: the ends are pinned by the ramp itself, whatever the luma in between.
    core::Document doc(6, 1);
    seedTestCard(doc);
    core::AdjustmentLayer* adj = addAdjustment(doc, core::AdjustmentKind::GradientMap, {});
    core::setAdjustmentGradientMap(adj->params(),
                                   rampOf({0.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}));
    const common::Image out = flatten(doc);
    CHECK(px(out, 0, 0) == common::Color8{0, 0, 0, 255});
    CHECK(px(out, 1, 0) == common::Color8{255, 0, 0, 255});
    CHECK(px(out, 2, 0).g == 0); // mid-gray lands mid-ramp: red only, no green or blue
    CHECK(px(out, 2, 0).r > 100);
    CHECK(px(out, 2, 0).r < 155);
}

TEST_CASE("Gradient Map: Reverse flips the ramp; stop ALPHA is the per-tone strength") {
    core::Document doc(6, 1);
    seedTestCard(doc);
    core::AdjustmentLayer* adj = addAdjustment(doc, core::AdjustmentKind::GradientMap,
                                               {{"reverse", 1.0}});
    core::setAdjustmentGradientMap(adj->params(),
                                   rampOf({0.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}));
    const common::Image out = flatten(doc);
    CHECK(px(out, 0, 0) == common::Color8{255, 0, 0, 255}); // black now reads the ramp's far end
    CHECK(px(out, 1, 0) == common::Color8{0, 0, 0, 255});

    // A fully TRANSPARENT ramp grades nothing: the stop alpha is how much of the mapped colour
    // lands, so a = 0 leaves that tone byte-identical -- which is how one ramp can recolour the
    // shadows and let the highlights through.
    core::Document plain(6, 1);
    seedTestCard(plain);
    const common::Image before = flatten(plain);
    core::Document clear(6, 1);
    seedTestCard(clear);
    core::AdjustmentLayer* c = addAdjustment(clear, core::AdjustmentKind::GradientMap, {});
    core::setAdjustmentGradientMap(c->params(),
                                   rampOf({0.0F, 0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F, 0.0F}));
    CHECK(imagesEqual(flatten(clear), before));
}

TEST_CASE("Gradient Map: the layer's opacity gates the mapped result") {
    core::Document doc(6, 1);
    seedTestCard(doc);
    core::AdjustmentLayer* adj = addAdjustment(doc, core::AdjustmentKind::GradientMap, {});
    core::setAdjustmentGradientMap(adj->params(),
                                   rampOf({0.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}));
    adj->setOpacity(0.5f);
    const common::Color8 white = px(flatten(doc), 1, 0); // (255,255,255) halfway to (255,0,0)
    CHECK(white.r == 255);
    CHECK(std::abs(static_cast<int>(white.g) - 128) <= 1);
    CHECK(std::abs(static_cast<int>(white.b) - 128) <= 1);
}

TEST_CASE("Vibrance: muted colour moves, vivid colour and neutrals stay BYTE-identical") {
    // A 3x1 card: a muted mauve, a fully saturated red, and a neutral gray.
    const auto seedVibranceCard = [](core::Document& doc) {
        auto base = doc.makeRaster("base", 3, 1);
        base->image().rgba = {140, 120, 130, 255, 255, 0, 0, 255, 128, 128, 128, 255};
        doc.root().addOnTop(std::move(base));
    };
    core::Document plain(3, 1);
    seedVibranceCard(plain);
    const common::Image before = flatten(plain);

    // Identity at the default (the §1 rule): which way a photograph needs to go is its business.
    core::Document zero(3, 1);
    seedVibranceCard(zero);
    core::AdjustmentLayer* z = addAdjustment(zero, core::AdjustmentKind::Vibrance, {});
    core::seedAdjustmentDefaults(*z);
    CHECK(imagesEqual(flatten(zero), before));

    core::Document doc(3, 1);
    seedVibranceCard(doc);
    addAdjustment(doc, core::AdjustmentKind::Vibrance, {{"vibrance", 100.0}});
    const common::Image out = flatten(doc);
    // The muted patch spreads: s' = s*(1 + 1*(1 - s)) nearly doubles a small saturation.
    const int wasSpread = static_cast<int>(px(before, 0, 0).r) - px(before, 0, 0).g;
    const int nowSpread = static_cast<int>(px(out, 0, 0).r) - px(out, 0, 0).g;
    CHECK(nowSpread > wasSpread + 5);
    // Fully saturated red is at s == 1, where the weight (1 - s) is exactly 0: s' == s, the HSL
    // round trip is skipped outright, and the pixel comes out byte-for-byte as it went in. THIS
    // is what "protects already-vivid colour" means, and it is exact, not approximate.
    CHECK(px(out, 1, 0) == px(before, 1, 0));
    CHECK(px(out, 2, 0) == px(before, 2, 0)); // ... and a neutral has no chroma to weight
}

TEST_CASE("Vibrance: a negative amount collapses the muted end first") {
    const auto seed = [](core::Document& doc) {
        auto base = doc.makeRaster("base", 2, 1);
        base->image().rgba = {140, 120, 130, 255, 255, 0, 0, 255};
        doc.root().addOnTop(std::move(base));
    };
    core::Document plain(2, 1);
    seed(plain);
    const common::Image before = flatten(plain);
    core::Document doc(2, 1);
    seed(doc);
    addAdjustment(doc, core::AdjustmentKind::Vibrance, {{"vibrance", -100.0}});
    const common::Image out = flatten(doc);
    // s' = s^2 at the full negative slider, so the muted patch COLLAPSES toward neutral without
    // reaching it -- and the residue is predictable rather than "essentially zero". Worked through:
    // (140,120,130) is L = 0.5098, s = delta/(2-max-min) = 0.0784/0.9804 = 0.08, so s' = 0.0064 and
    // the surviving chroma is C = (1-|2L-1|)*s' = 0.00627, i.e. 1.6/255. That lands as a 2-LSB
    // spread (131,129,130), NOT a 1-LSB one: an assertion of <= 1 here would be demanding that the
    // squaring go all the way to grey, which is not what the curve does at s = 0.08.
    const auto spread = [](common::Color8 c) {
        return static_cast<int>(std::max({c.r, c.g, c.b})) - std::min({c.r, c.g, c.b});
    };
    CHECK(spread(px(before, 0, 0)) == 20);         // the patch starts 20 LSB wide...
    CHECK(spread(px(out, 0, 0)) == 2);             // ... and s^2 collapses it by an order of magnitude
    CHECK(px(out, 1, 0) == px(before, 1, 0)); // ... while pure red is still exactly untouched
}

TEST_CASE("Photo Filter: density 0 is byte-exact; a full density IS the filter colour") {
    core::Document plain(6, 1);
    seedTestCard(plain);
    const common::Image before = flatten(plain);
    core::Document zero(6, 1);
    seedTestCard(zero);
    addAdjustment(zero, core::AdjustmentKind::PhotoFilter,
                  {{"filter", static_cast<double>(core::PhotoFilterPreset::Warming85)},
                   {"density", 0.0}, {"preserve_luminosity", 1.0}});
    CHECK(imagesEqual(flatten(zero), before)); // the EFFECTIVE no-op, not default-equality

    // Density 100 with the luminance NOT restored: white light through an absorptive filter comes
    // out as exactly the filter's own colour, which is the physical statement the maths makes.
    core::Document doc(6, 1);
    seedTestCard(doc);
    addAdjustment(doc, core::AdjustmentKind::PhotoFilter,
                  {{"filter", static_cast<double>(core::PhotoFilterPreset::Warming85)},
                   {"density", 100.0}, {"preserve_luminosity", 0.0}});
    const common::Color8 white = px(flatten(doc), 1, 0);
    const common::Color8 gel = core::photoFilterPresetColor(core::PhotoFilterPreset::Warming85);
    CHECK(std::abs(static_cast<int>(white.r) - gel.r) <= 2);
    CHECK(std::abs(static_cast<int>(white.g) - gel.g) <= 2);
    CHECK(std::abs(static_cast<int>(white.b) - gel.b) <= 2);
    CHECK(white.a == 255); // alpha untouched
}

TEST_CASE("Photo Filter: seeded defaults are VISIBLE, and warming/cooling pull opposite ways") {
    core::Document plain(6, 1);
    seedTestCard(plain);
    const common::Image before = flatten(plain);
    core::Document def(6, 1);
    seedTestCard(def);
    core::AdjustmentLayer* d = addAdjustment(def, core::AdjustmentKind::PhotoFilter, {});
    core::seedAdjustmentDefaults(*d);
    CHECK_FALSE(imagesEqual(flatten(def), before)); // the anti-broken-promise rule

    const auto midGrayUnder = [](core::PhotoFilterPreset p, double customB) {
        core::Document doc(6, 1);
        seedTestCard(doc);
        addAdjustment(doc, core::AdjustmentKind::PhotoFilter,
                      {{"filter", static_cast<double>(p)}, {"density", 60.0},
                       {"preserve_luminosity", 0.0}, {"color_r", 0.0}, {"color_g", 0.0},
                       {"color_b", customB}});
        return px(flatten(doc), 2, 0);
    };
    const common::Color8 warm = midGrayUnder(core::PhotoFilterPreset::Warming85, 0.0);
    CHECK(warm.r > warm.b);
    const common::Color8 cool = midGrayUnder(core::PhotoFilterPreset::Cooling80, 0.0);
    CHECK(cool.b > cool.r);
    // Custom reads the three colour rows instead of the preset table -- a pure blue gel here, so
    // red and green are absorbed by the SAME amount (both filter channels are 0) and blue is
    // passed untouched. Density is a mix, so "absorbed" means dimmed, not zeroed.
    const common::Color8 custom = midGrayUnder(core::PhotoFilterPreset::Custom, 255.0);
    CHECK(custom.b > custom.r);
    CHECK(custom.r == custom.g);
    // An unabsorbed channel comes back through the linear round trip where it started (within the
    // LUT pair's interpolation, which is why this is a level and not a byte equality).
    CHECK(std::abs(static_cast<int>(custom.b) - 128) <= 1);
}

TEST_CASE("Photo Filter: Preserve luminosity holds the backdrop's linear luminance") {
    const auto graded = [](bool preserve) {
        core::Document doc(6, 1);
        seedTestCard(doc);
        addAdjustment(doc, core::AdjustmentKind::PhotoFilter,
                      {{"filter", static_cast<double>(core::PhotoFilterPreset::Warming85)},
                       {"density", 100.0}, {"preserve_luminosity", preserve ? 1.0 : 0.0}});
        return px(flatten(doc), 2, 0);
    };
    const double source = linearLuminance({128, 128, 128, 255});
    const common::Color8 kept = graded(true);
    const common::Color8 dimmed = graded(false);
    // With the option ON the filtered patch carries the SAME linear luminance it started with
    // (the whole point: a dense gel otherwise just darkens the frame)...
    CHECK(linearLuminance(kept) == doctest::Approx(source).epsilon(0.02));
    // ... and with it OFF it is markedly darker, because an absorptive filter absorbs.
    CHECK(linearLuminance(dimmed) < source * 0.6);
    CHECK(kept.r > dimmed.r);
}

TEST_CASE("High Pass: a flat field is mid-grey, an edge straddles it, alpha is untouched") {
    // A flat opaque field: the Gaussian of a constant IS that constant, so the difference is zero
    // everywhere and the whole frame lands on the 1/2 bias. (Within a level: the separable
    // kernel's normalisation is float, and 0.5 sits exactly on the 8-bit rounding boundary.)
    core::Document flat(16, 16);
    auto base = flat.makeRaster("base", 16, 16);
    for (std::size_t p = 0; p < base->image().rgba.size(); p += 4) {
        base->image().rgba[p] = 90;
        base->image().rgba[p + 1] = 150;
        base->image().rgba[p + 2] = 210;
        base->image().rgba[p + 3] = 255;
    }
    flat.root().addOnTop(std::move(base));
    addAdjustment(flat, core::AdjustmentKind::HighPass, {{"radius", 6.0}});
    const common::Image flatOut = flatten(flat);
    for (std::uint32_t y = 0; y < 16; ++y)
        for (std::uint32_t x = 0; x < 16; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            REQUIRE(std::abs(static_cast<int>(px(flatOut, x, y).r) - 128) <= 1);
            REQUIRE(std::abs(static_cast<int>(px(flatOut, x, y).b) - 128) <= 1);
            REQUIRE(px(flatOut, x, y).a == 255); // a high pass recolours; it moves no coverage
        }

    // A hard vertical edge: the light side overshoots above mid-grey, the dark side undershoots
    // below it, and far from the edge both settle back onto it. That IS the high-pass signature.
    core::Document edge(64, 8);
    auto step = edge.makeRaster("step", 64, 8);
    for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 64; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * 64 + x) * 4;
            const auto v = static_cast<std::uint8_t>(x < 32 ? 40 : 220);
            step->image().rgba[p] = step->image().rgba[p + 1] = step->image().rgba[p + 2] = v;
            step->image().rgba[p + 3] = 255;
        }
    edge.root().addOnTop(std::move(step));
    addAdjustment(edge, core::AdjustmentKind::HighPass, {{"radius", 6.0}});
    const common::Image edgeOut = flatten(edge);
    CHECK(px(edgeOut, 31, 4).r < 118); // the dark side of the boundary
    CHECK(px(edgeOut, 32, 4).r > 138); // the light side
    CHECK(std::abs(static_cast<int>(px(edgeOut, 2, 4).r) - 128) <= 2);  // far left: flat again
    CHECK(std::abs(static_cast<int>(px(edgeOut, 61, 4).r) - 128) <= 2); // far right
}

TEST_CASE("High Pass: reach is 3 sigma and region == crop(full)") {
    core::Document doc(64, 48);
    seedBlurScene(doc);
    core::AdjustmentLayer* hp =
        addAdjustment(doc, core::AdjustmentKind::HighPass, {{"radius", 6.0}});
    // sigma = radius/2, support 3 sigma -- the same reading Unsharp Mask and Gaussian Blur give
    // "radius", which is what lets the three stack without anyone learning a second unit.
    CHECK(render::stylizeAdjustmentReach(*hp, {0.0, 0.0, 64.0, 48.0}) == 9.0);

    const common::Image full = flatten(doc);
    const common::Rect roi{24.0, 12.0, 16.0, 16.0};
    const render::CompositeResult r = render::compositeRegion(doc, roi, {}, render::Backend::Cpu);
    REQUIRE(r.ok);
    for (std::uint32_t y = 0; y < r.image.height; ++y)
        for (std::uint32_t x = 0; x < r.image.width; ++x)
            REQUIRE(px(r.image, x, y) == px(full, x + 24, y + 12));
}
