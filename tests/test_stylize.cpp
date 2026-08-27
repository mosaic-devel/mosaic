// S35 artistic / stylize filters (docs/filters-stylize.md): the schema rows, the three
// compositor seam functions (isStylizeKind / applyStylizeAdjustment / stylizeAdjustmentReach) and
// the composite-level invariants the family has to hold -- identity at zero amount, opacity
// gating, and REGION == crop(FULL) for the three kinds whose output is a pure function of the
// pixel's parent-space position or cell (the money invariant of docs/blur-filters.md §5).
//
// Like test_adjustments.cpp these are ANALYTIC pins, not golden pixels: each expectation is
// re-derived from the filter's definition, so the test states the spec rather than a snapshot.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "common/image.hpp"
#include "core/adjustments.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "render/compositor.hpp"
#include "render/stylize.hpp"

using namespace mosaic;

namespace {

// The nine S35 kinds, in the order they were appended to AdjustmentKind.
constexpr core::AdjustmentKind kStylizeKinds[] = {
    core::AdjustmentKind::Sharpen,  core::AdjustmentKind::UnsharpMask,
    core::AdjustmentKind::AddNoise, core::AdjustmentKind::Denoise,
    core::AdjustmentKind::Pixelate, core::AdjustmentKind::Emboss,
    core::AdjustmentKind::OilPaint, core::AdjustmentKind::Wave,
    core::AdjustmentKind::Vignette,
};

common::Image flatten(const core::Document& doc) {
    const render::CompositeResult r = render::composite(doc, {}, render::Backend::Cpu);
    REQUIRE(r.ok);
    return r.image;
}

common::Color8 px(const common::Image& img, std::uint32_t x, std::uint32_t y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

// An opaque deterministic pattern with structure in both axes -- enough spread that a smoothing
// or sharpening kernel cannot slip past a byte comparison.
void seedPattern(core::Document& doc, std::uint32_t w, std::uint32_t h) {
    auto base = doc.makeRaster("base", w, h);
    common::Image& img = base->image();
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
            img.rgba[p] = static_cast<std::uint8_t>((x * 9 + y * 3) & 0xFF);
            img.rgba[p + 1] = static_cast<std::uint8_t>((x * 5 + y * 13) & 0xFF);
            img.rgba[p + 2] = static_cast<std::uint8_t>((x * 3 + y * 7 + 40) & 0xFF);
            img.rgba[p + 3] = 255;
        }
    doc.root().addOnTop(std::move(base));
}

void seedFlat(core::Document& doc, std::uint32_t w, std::uint32_t h, common::Color8 c) {
    auto base = doc.makeRaster("base", w, h);
    base->image().fill(c);
    doc.root().addOnTop(std::move(base));
}

core::AdjustmentLayer* addAdjustment(core::Document& doc, core::AdjustmentKind kind,
                                     std::map<std::string, double> params) {
    auto layer = doc.makeAdjustment("adj", kind);
    layer->params() = std::move(params);
    core::AdjustmentLayer* raw = layer.get();
    doc.root().addOnTop(std::move(layer));
    return raw;
}

// Every byte of the two images equal.
void checkIdentical(const common::Image& a, const common::Image& b) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    REQUIRE(a.rgba == b.rgba);
}

// compositeRegion(roi) must be byte-identical to the matching window of the full composite.
void checkRegionMatchesFull(const core::Document& doc, const common::Image& full, long rx, long ry,
                            std::uint32_t rw, std::uint32_t rh) {
    const render::CompositeResult r = render::compositeRegion(
        doc,
        {static_cast<double>(rx), static_cast<double>(ry), static_cast<double>(rw),
         static_cast<double>(rh)},
        {}, render::Backend::Cpu);
    REQUIRE(r.ok);
    REQUIRE(r.image.width == rw);
    REQUIRE(r.image.height == rh);
    for (std::uint32_t y = 0; y < rh; ++y)
        for (std::uint32_t x = 0; x < rw; ++x)
            REQUIRE(px(r.image, x, y) == px(full, static_cast<std::uint32_t>(rx) + x,
                                            static_cast<std::uint32_t>(ry) + y));
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Schema + the core predicates
// ---------------------------------------------------------------------------------------------

TEST_CASE("stylize schema: every S35 kind's table is well-formed") {
    for (const core::AdjustmentKind kind : kStylizeKinds) {
        std::set<std::string> keys;
        const auto rows = core::adjustmentParamSchema(kind);
        CHECK(!rows.empty());  // every S35 kind has knobs
        for (const core::AdjustmentParamDesc& d : rows) {
            CAPTURE(d.key);
            REQUIRE(d.key != nullptr);
            REQUIRE(d.label != nullptr);
            CHECK(keys.insert(d.key).second);  // keys unique within the kind
            CHECK(d.min <= d.def);
            CHECK(d.def <= d.max);
            CHECK(d.step > 0.0);
            if (d.type == core::AdjustmentParamType::Choice) {
                REQUIRE(d.choices != nullptr);
                CHECK(d.choiceCount >= 2);
                CHECK(d.min == 0.0);
                CHECK(d.max == static_cast<double>(d.choiceCount - 1));
            }
        }
    }
}

TEST_CASE("stylize kinds: predicates agree with the family") {
    for (const core::AdjustmentKind kind : kStylizeKinds) {
        CHECK(render::isStylizeKind(kind));
        CHECK(core::adjustmentImplemented(kind));
    }
    // Not stylize: the S32 colour kinds and the S33 blur family route elsewhere.
    CHECK_FALSE(render::isStylizeKind(core::AdjustmentKind::Levels));
    CHECK_FALSE(render::isStylizeKind(core::AdjustmentKind::Grayscale));
    CHECK_FALSE(render::isStylizeKind(core::AdjustmentKind::GaussianBlur));
    CHECK_FALSE(render::isStylizeKind(core::AdjustmentKind::DofBlur));

    // Spatiality: the seven windowed/resampling kinds must be declared spatial so the region and
    // group-buffer machinery grows by their reach; the two per-pixel kinds must not be.
    CHECK(core::adjustmentIsSpatial(core::AdjustmentKind::Sharpen));
    CHECK(core::adjustmentIsSpatial(core::AdjustmentKind::UnsharpMask));
    CHECK(core::adjustmentIsSpatial(core::AdjustmentKind::Denoise));
    CHECK(core::adjustmentIsSpatial(core::AdjustmentKind::Pixelate));
    CHECK(core::adjustmentIsSpatial(core::AdjustmentKind::Emboss));
    CHECK(core::adjustmentIsSpatial(core::AdjustmentKind::OilPaint));
    CHECK(core::adjustmentIsSpatial(core::AdjustmentKind::Wave));
    CHECK_FALSE(core::adjustmentIsSpatial(core::AdjustmentKind::AddNoise));
    CHECK_FALSE(core::adjustmentIsSpatial(core::AdjustmentKind::Vignette));
}

TEST_CASE("stylizeAdjustmentReach reports each kind's support") {
    core::Document doc(8, 8);
    const common::Rect domain{0.0, 0.0, 8.0, 8.0};

    // Per-pixel kinds spread nothing.
    core::AdjustmentLayer* noise = addAdjustment(doc, core::AdjustmentKind::AddNoise, {});
    CHECK(render::stylizeAdjustmentReach(*noise, domain) == 0.0);
    core::AdjustmentLayer* vig = addAdjustment(doc, core::AdjustmentKind::Vignette, {});
    CHECK(render::stylizeAdjustmentReach(*vig, domain) == 0.0);

    // A pixelate cell reaches exactly one cell: that is what makes a region's blocks identical
    // to the full composite's.
    core::AdjustmentLayer* pix =
        addAdjustment(doc, core::AdjustmentKind::Pixelate, {{"size", 9.0}});
    CHECK(render::stylizeAdjustmentReach(*pix, domain) == 9.0);

    // Unsharp uses the blur family's sigma = radius/2, so 3 sigma == 1.5 * radius.
    core::AdjustmentLayer* usm =
        addAdjustment(doc, core::AdjustmentKind::UnsharpMask, {{"radius", 4.0}});
    CHECK(render::stylizeAdjustmentReach(*usm, domain) == doctest::Approx(6.0));

    core::AdjustmentLayer* den =
        addAdjustment(doc, core::AdjustmentKind::Denoise, {{"radius", 5.0}});
    CHECK(render::stylizeAdjustmentReach(*den, domain) == doctest::Approx(5.0));

    core::AdjustmentLayer* wav =
        addAdjustment(doc, core::AdjustmentKind::Wave, {{"amplitude", 12.0}});
    CHECK(render::stylizeAdjustmentReach(*wav, domain) == doctest::Approx(13.0));

    // A hostile value is clamped by the schema read before it ever reaches the kernel.
    core::AdjustmentLayer* huge =
        addAdjustment(doc, core::AdjustmentKind::Pixelate, {{"size", 1.0e9}});
    const core::AdjustmentParamDesc* d =
        core::adjustmentParamDesc(core::AdjustmentKind::Pixelate, "size");
    REQUIRE(d != nullptr);
    CHECK(render::stylizeAdjustmentReach(*huge, domain) == d->max);
}

// ---------------------------------------------------------------------------------------------
// Identity: a zero-amount layer must not touch a single byte
// ---------------------------------------------------------------------------------------------

TEST_CASE("stylize: zero-amount parameters composite byte-identically to no layer") {
    core::Document plain(24, 16);
    seedPattern(plain, 24, 16);
    const common::Image bare = flatten(plain);

    const std::pair<core::AdjustmentKind, std::map<std::string, double>> cases[] = {
        {core::AdjustmentKind::Sharpen, {{"amount", 0.0}}},
        {core::AdjustmentKind::UnsharpMask, {{"amount", 0.0}, {"radius", 3.0}}},
        {core::AdjustmentKind::AddNoise, {{"amount", 0.0}}},
        {core::AdjustmentKind::Denoise, {{"noise", 0.0}, {"radius", 3.0}}},
        // A sub-pixel cell cannot read as a block: the honest answer is the untouched backdrop.
        {core::AdjustmentKind::Pixelate, {{"size", 1.0}}},
        {core::AdjustmentKind::Wave, {{"amplitude", 0.0}}},
        {core::AdjustmentKind::Vignette, {{"exposure", 0.0}, {"radius", 4.0}}},
    };
    for (const auto& entry : cases) {
        const int kindId = static_cast<int>(entry.first);
        CAPTURE(kindId);
        core::Document doc(24, 16);
        seedPattern(doc, 24, 16);
        addAdjustment(doc, entry.first, entry.second);
        checkIdentical(flatten(doc), bare);
    }
}

TEST_CASE("stylize: opacity 0 composites byte-identically to no layer") {
    core::Document bareDoc(24, 16);
    seedPattern(bareDoc, 24, 16);
    const common::Image bare = flatten(bareDoc);

    for (const core::AdjustmentKind kind : kStylizeKinds) {
        const int kindId = static_cast<int>(kind);
        CAPTURE(kindId);
        core::Document doc(24, 16);
        seedPattern(doc, 24, 16);
        core::AdjustmentLayer* adj = addAdjustment(doc, kind, {});
        core::seedAdjustmentDefaults(*adj);
        adj->setOpacity(0.0f);
        checkIdentical(flatten(doc), bare);
    }
}

TEST_CASE("stylize: the in-place arm and the copy-and-blend arm agree byte for byte") {
    // applyStylizeAdjustment has two arms. UNMODULATED -- full opacity, no mask, not clipped --
    // runs the kernel straight into the accumulator, because `amt` is then >= 1 everywhere and the
    // blend would only copy the scratch back over the original. Anything else copies the
    // accumulator, transforms the copy, and lerps it back under the coverage.
    //
    // A FULLY WHITE mask is what puts the two side by side: adjustmentMaskAt divides the 255 by
    // 255.0f, which is exactly 1.0f, so `amt` is exactly 1.0f at every pixel and the modulated arm
    // is forced to take the same picture the in-place arm produces directly. If the fast arm is
    // ever entered for a case the blend would have changed, or the kernels stop being pure
    // in-place transforms, these two stop matching.
    //
    // Every kind, because the split is in the seam and not in any one kernel.
    for (const core::AdjustmentKind kind : kStylizeKinds) {
        const int kindId = static_cast<int>(kind);
        CAPTURE(kindId);

        core::Document inPlace(24, 16);
        seedPattern(inPlace, 24, 16);
        core::AdjustmentLayer* a = addAdjustment(inPlace, kind, {});
        core::seedAdjustmentDefaults(*a);
        a->setOpacity(1.0f);

        core::Document blended(24, 16);
        seedPattern(blended, 24, 16);
        core::AdjustmentLayer* b = addAdjustment(blended, kind, {});
        core::seedAdjustmentDefaults(*b);
        b->setOpacity(1.0f);
        core::RasterMask mask;
        mask.width = 24;
        mask.height = 16;
        mask.enabled = true;
        mask.coverage.assign(static_cast<std::size_t>(24) * 16, std::uint8_t{255});
        b->setMask(std::move(mask));

        const common::Image direct = flatten(inPlace);
        checkIdentical(flatten(blended), direct);

        // ... and the equality above is NOT enough on its own: it holds for any predicate that
        // sends this case either way, so it cannot catch one that takes the fast arm when the
        // coverage really does modulate. These two do. A HALF mask and a HALF opacity each have to
        // land somewhere the raw kernel output is not -- which is only possible if the blend ran.
        core::Document bareDoc(24, 16);
        seedPattern(bareDoc, 24, 16);
        const common::Image bare = flatten(bareDoc);
        if (direct.rgba == bare.rgba)
            continue; // this kind's defaults are the identity: nothing to modulate

        core::Document halfMask(24, 16);
        seedPattern(halfMask, 24, 16);
        core::AdjustmentLayer* c = addAdjustment(halfMask, kind, {});
        core::seedAdjustmentDefaults(*c);
        c->setOpacity(1.0f);
        core::RasterMask grey;
        grey.width = 24;
        grey.height = 16;
        grey.enabled = true;
        grey.coverage.assign(static_cast<std::size_t>(24) * 16, std::uint8_t{128});
        c->setMask(std::move(grey));
        CHECK(flatten(halfMask).rgba != direct.rgba);

        core::Document halfOpacity(24, 16);
        seedPattern(halfOpacity, 24, 16);
        core::AdjustmentLayer* d = addAdjustment(halfOpacity, kind, {});
        core::seedAdjustmentDefaults(*d);
        d->setOpacity(0.5f);
        CHECK(flatten(halfOpacity).rgba != direct.rgba);
    }
}

// ---------------------------------------------------------------------------------------------
// Per-filter analytic behaviour
// ---------------------------------------------------------------------------------------------

TEST_CASE("emboss: a flat field embosses to exact mid-gray, alpha untouched") {
    // Angle 0 with height 2 puts the two taps on whole pixels either side, so the bilinear read
    // is exact and the luma difference over a constant field is exactly zero -- 0.5 -> 128.
    core::Document doc(12, 8);
    seedFlat(doc, 12, 8, {90, 140, 200, 255});
    addAdjustment(doc, core::AdjustmentKind::Emboss,
                  {{"angle", 0.0}, {"height", 2.0}, {"amount", 100.0}});
    const common::Image out = flatten(doc);
    for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 12; ++x) {
            const common::Color8 c = px(out, x, y);
            CHECK(c.r == 128);
            CHECK(c.g == 128);
            CHECK(c.b == 128);
            CHECK(c.a == 255);  // a relief replaces colour; it never carves coverage
        }
}

TEST_CASE("oil paint and denoise leave a flat field alone") {
    // Kuwahara on a constant field: all four quadrant means are the constant, so whichever
    // quadrant wins the variance test returns it. Lee's filter sees zero variance, so k = 0 and
    // the output is the local mean -- the constant again. Both are exact up to the running-sum
    // rounding of a box mean, hence the +-1 byte window.
    core::Document doc(16, 12);
    seedFlat(doc, 16, 12, {70, 130, 190, 255});
    core::Document doc2(16, 12);
    seedFlat(doc2, 16, 12, {70, 130, 190, 255});
    addAdjustment(doc, core::AdjustmentKind::OilPaint, {{"radius", 5.0}});
    addAdjustment(doc2, core::AdjustmentKind::Denoise, {{"radius", 4.0}, {"noise", 10.0}});

    const auto checkFlat = [](const common::Image& out) {
        for (std::uint32_t y = 0; y < 12; ++y)
            for (std::uint32_t x = 0; x < 16; ++x) {
                const common::Color8 c = px(out, x, y);
                CHECK(std::abs(static_cast<int>(c.r) - 70) <= 1);
                CHECK(std::abs(static_cast<int>(c.g) - 130) <= 1);
                CHECK(std::abs(static_cast<int>(c.b) - 190) <= 1);
                CHECK(c.a == 255);
            }
    };
    checkFlat(flatten(doc));
    checkFlat(flatten(doc2));
}

TEST_CASE("pixelate: cells are uniform and anchored to the document grid") {
    core::Document doc(8, 8);
    seedPattern(doc, 8, 8);
    addAdjustment(doc, core::AdjustmentKind::Pixelate, {{"size", 2.0}});
    const common::Image out = flatten(doc);
    // The lattice is floor(parent / size), so with size 2 at the document origin the cells are
    // exactly the 2x2 blocks starting at even coordinates. Every pixel of a block shares one
    // mean, so the four bytes must be EQUAL -- not merely close.
    for (std::uint32_t by = 0; by < 8; by += 2)
        for (std::uint32_t bx = 0; bx < 8; bx += 2) {
            const common::Color8 ref = px(out, bx, by);
            CHECK(px(out, bx + 1, by) == ref);
            CHECK(px(out, bx, by + 1) == ref);
            CHECK(px(out, bx + 1, by + 1) == ref);
        }
    // ... and a block is the mean of the four source pixels it covers (opaque input, so the
    // premultiplied mean is the plain mean).
    core::Document plain(8, 8);
    seedPattern(plain, 8, 8);
    const common::Image src = flatten(plain);
    const int want = (static_cast<int>(px(src, 0, 0).r) + px(src, 1, 0).r + px(src, 0, 1).r +
                      px(src, 1, 1).r + 2) / 4;
    CHECK(std::abs(static_cast<int>(px(out, 0, 0).r) - want) <= 1);
}

TEST_CASE("vignette: the core is byte-identical, the corners darken") {
    core::Document doc(64, 48);
    seedFlat(doc, 64, 48, {160, 160, 160, 255});
    core::Document plain(64, 48);
    seedFlat(plain, 64, 48, {160, 160, 160, 255});
    const common::Image bare = flatten(plain);

    addAdjustment(doc, core::AdjustmentKind::Vignette,
                  {{"center_x", 32.0},
                   {"center_y", 24.0},
                   {"radius", 10.0},
                   {"feather", 60.0},
                   {"roundness", 0.0},
                   {"exposure", -1.2}});
    const common::Image out = flatten(doc);

    // Inside q <= 1 the kernel skips the pixel outright: no decode/encode round trip, so the
    // un-vignetted core is provably untouched.
    CHECK(px(out, 32, 24) == px(bare, 32, 24));
    CHECK(px(out, 31, 23) == px(bare, 31, 23));
    // A corner sits far past the feather band, so it takes the full -1.2 EV.
    const common::Color8 corner = px(out, 0, 0);
    CHECK(corner.r < px(bare, 0, 0).r);
    CHECK(corner.a == 255);  // a vignette dims the backdrop; it never erases it
    // 2^-1.2 in linear light is a large, unmistakable drop -- well past half.
    CHECK(corner.r < 140);
}

TEST_CASE("add noise: a seed changes the grain, the amount changes the image") {
    core::Document plain(32, 24);
    seedFlat(plain, 32, 24, {128, 128, 128, 255});
    const common::Image bare = flatten(plain);

    core::Document a(32, 24);
    seedFlat(a, 32, 24, {128, 128, 128, 255});
    addAdjustment(a, core::AdjustmentKind::AddNoise, {{"amount", 20.0}, {"seed", 1.0}});
    const common::Image ia = flatten(a);
    CHECK(ia.rgba != bare.rgba);  // 20% noise on a flat field is not subtle

    core::Document b(32, 24);
    seedFlat(b, 32, 24, {128, 128, 128, 255});
    addAdjustment(b, core::AdjustmentKind::AddNoise, {{"amount", 20.0}, {"seed", 7.0}});
    CHECK(flatten(b).rgba != ia.rgba);  // the seed is a real knob

    // Monochromatic draws ONE sample per pixel, so the three channels move together. The base is
    // neutral gray, so the output must stay neutral.
    core::Document m(32, 24);
    seedFlat(m, 32, 24, {128, 128, 128, 255});
    addAdjustment(m, core::AdjustmentKind::AddNoise,
                  {{"amount", 20.0}, {"seed", 1.0}, {"monochrome", 1.0}});
    const common::Image im = flatten(m);
    for (std::uint32_t y = 0; y < 24; ++y)
        for (std::uint32_t x = 0; x < 32; ++x) {
            const common::Color8 c = px(im, x, y);
            CHECK(c.r == c.g);
            CHECK(c.g == c.b);
            CHECK(c.a == 255);  // noise recolours; it adds no coverage
        }
}

// ---------------------------------------------------------------------------------------------
// Region == crop(full): the money invariant
// ---------------------------------------------------------------------------------------------

TEST_CASE("region composite == crop(full) for the S35 kinds") {
    // Add Noise and Vignette are pure functions of the pixel's PARENT-space position, and
    // Pixelate's lattice is anchored in parent space with a one-cell reach -- so all three must
    // survive a dirty-rect recomposite byte for byte. A buffer-index-keyed RNG or a
    // buffer-anchored cell grid fails here immediately, which is exactly why the case exists.
    const auto run = [](core::AdjustmentKind kind, std::map<std::string, double> params) {
        core::Document doc(64, 48);
        seedPattern(doc, 64, 48);
        addAdjustment(doc, kind, std::move(params));
        const common::Image full = flatten(doc);
        checkRegionMatchesFull(doc, full, 16, 8, 17, 13);
        checkRegionMatchesFull(doc, full, 0, 0, 12, 9);  // touching the canvas edge
    };

    SUBCASE("add noise") { run(core::AdjustmentKind::AddNoise, {{"amount", 30.0}}); }
    SUBCASE("vignette") {
        run(core::AdjustmentKind::Vignette,
            {{"center_x", 32.0}, {"center_y", 24.0}, {"radius", 12.0}, {"exposure", -2.0}});
    }
    SUBCASE("pixelate") { run(core::AdjustmentKind::Pixelate, {{"size", 7.0}}); }
}
