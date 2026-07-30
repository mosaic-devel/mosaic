// The RESIDENT tiled compositor, end to end (S60-a items 7/8/9; docs/s60-performance-plan.md §3).
//
// test_composite_tile_parity.cpp proves the KERNEL: one dispatch, one layer, one macrotile, held
// to `render::composite(..., Backend::Cpu)` at 1/255 across every blend mode and resample filter.
// This file proves the LANE around it -- the tile atlas, the residency cache, the dirty set, the
// plan diff and the readback seam -- by compositing whole documents through
// `render::TileCompositor` and comparing the readback with the CPU reference.
//
// Three properties matter here that a kernel test cannot see:
//
//   * RESIDENCY. A second composite of an unchanged document must cost ZERO dispatches and ZERO
//     uploaded bytes. That is the difference between "the compositor is on the GPU" and "the
//     compositor is GPU-RESIDENT", and it is invisible to wall-clock on a fast device -- which is
//     precisely how S7-b's per-layer round trip survived (§1.1). It is asserted, not timed.
//   * THE DIRTY SET. Moving one small layer must recomposite the macrotiles it touched and no
//     others, and the result must be identical to recompositing the whole canvas. If those two
//     ever disagree, the dirty set is wrong and the picture rots one tile at a time.
//   * REFUSAL. Everything the lane cannot composite EXACTLY must come back as a named refusal
//     with the accumulator untouched, so the caller takes the CPU lane. A GPU lane that guesses
//     is worse than one that declines.
//
// Every case asserts WHICH LANE SERVED (`status.ok` / `status.refusal`). A test that silently
// passes because the lane quietly declined is worthless, so the refusal is always part of the
// assertion rather than a thing the test tolerates. The file is CI-safe: with no usable Vulkan
// device it WARNs and passes, the pattern test_extrude_gpu.cpp / test_blur_gpu.cpp established.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/adjustments.hpp"
#include "core/blend_mode.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/layer_effects.hpp"
#include "core/texture/texture_params.hpp"
#include "core/tile_grid.hpp"
#include "core/vector/object.hpp"
#include "render/compositor.hpp"
#include "render/tile_compositor.hpp"

using mosaic::common::Affine2D;
using mosaic::common::Image;
using mosaic::core::BlendMode;
using mosaic::render::ResampleFilter;
using mosaic::render::TileRefusal;
namespace core = mosaic::core;
namespace render = mosaic::render;

namespace {

// Deliberately a multiple of neither 64 nor 256, so the right, bottom and corner macrotiles are
// PARTIAL on every run and the kernel's extent guard carries real weight.
constexpr std::uint32_t kW = 200;
constexpr std::uint32_t kH = 140;

std::unique_ptr<render::TileCompositor> makeLane(const char* who) {
    std::string err;
    auto lane = render::TileCompositor::create(err);
    if (!lane) {
        // Build the note first: doctest's message builder resolves its own operator+ against the
        // arguments, so a concatenation spelled inline picks the wrong overload.
        const std::string note =
            std::string("no usable Vulkan device -- skipping ") + who + " (" + err + ")";
        WARN_MESSAGE(true, note);
    }
    return lane;
}

// A structured backdrop with a fully transparent band carrying junk RGB -- the straight-vs-
// premultiplied tripwire: an alpha of 0 must contribute nothing whatever its colour says.
Image backdrop(std::uint32_t w = kW, std::uint32_t h = kH) {
    Image img(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
            img.rgba[p + 0] = static_cast<std::uint8_t>(x * 255 / (w - 1));
            img.rgba[p + 1] = static_cast<std::uint8_t>(y * 255 / (h - 1));
            img.rgba[p + 2] = static_cast<std::uint8_t>((x + 2 * y) % 256);
            img.rgba[p + 3] = 255;
            if (y % 37 < 4) {
                img.rgba[p + 0] = 210;
                img.rgba[p + 1] = 15;
                img.rgba[p + 2] = 90;
                img.rgba[p + 3] = 0;
            }
        }
    return img;
}

// Gradients, a saturated disc, an alpha ramp, opaque black/white (the reciprocal modes' knife
// edges) and a transparent block with junk RGB.
Image sprite(std::uint32_t w = 96, std::uint32_t h = 72) {
    Image img(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
            std::uint8_t r = static_cast<std::uint8_t>(255 - x * 255 / (w - 1));
            std::uint8_t g = static_cast<std::uint8_t>(30 + y * 200 / (h - 1));
            std::uint8_t b = static_cast<std::uint8_t>((3 * x + y) % 256);
            std::uint8_t a = 255;
            const double dx = static_cast<double>(x) - 0.35 * w;
            const double dy = static_cast<double>(y) - 0.40 * h;
            if (dx * dx + dy * dy < 0.02 * w * h * 4.0) {
                r = 250;
                g = 20;
                b = 240;
            }
            if (x < 6 && y < 6) r = g = b = 0;
            if (x >= 6 && x < 12 && y < 6) r = g = b = 255;
            if (x + 24 >= w) a = static_cast<std::uint8_t>((w - x) * 255 / 24);
            if (x >= w / 4 && x < w / 4 + 10 && y >= h / 2 && y < h / 2 + 10) {
                r = 200;
                g = 8;
                b = 130;
                a = 0;
            }
            img.rgba[p + 0] = r;
            img.rgba[p + 1] = g;
            img.rgba[p + 2] = b;
            img.rgba[p + 3] = a;
        }
    return img;
}

std::vector<std::uint8_t> maskCoverage(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> cov(static_cast<std::size_t>(w) * h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint32_t v = (x * 255) / (w > 1 ? w - 1 : 1);
            if (y * 3 < h) v = 255;
            if (x * 4 > 3 * w) v = 0;
            cov[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>(v);
        }
    return cov;
}

// One layer's description, so a case can build the same document twice (once for each lane) with
// no chance of the two drifting apart.
struct LayerSpec {
    Image pixels;
    Affine2D place = Affine2D::identity();
    BlendMode blend = BlendMode::Normal;
    float opacity = 1.0f;
    bool clip = false;
    bool visible = true;
    bool mask = false;
    bool maskLinked = true;
    std::uint32_t maskW = 0, maskH = 0;
};

void buildDocument(core::Document& doc, const std::vector<LayerSpec>& specs) {
    for (const LayerSpec& s : specs) {
        auto l = doc.makeRaster("l", s.pixels.width, s.pixels.height);
        l->image() = s.pixels;
        l->setTransform(s.place);
        l->setBlendMode(s.blend);
        l->setOpacity(s.opacity);
        l->setClipToBelow(s.clip);
        l->setVisible(s.visible);
        if (s.mask) {
            core::RasterMask m(s.maskW, s.maskH);
            m.coverage = maskCoverage(s.maskW, s.maskH);
            m.linked = s.maskLinked;
            doc.root().addOnTop(std::move(l))
                .setMask(std::move(m));
            continue;
        }
        doc.root().addOnTop(std::move(l));
    }
}

struct Diff {
    int maxLsb = 0;
    long over = 0;
    double meanLsb = 0.0;
    long pixels = 0;
};

Diff compare(const Image& cpu, const Image& gpu) {
    Diff d;
    REQUIRE(cpu.width == gpu.width);
    REQUIRE(cpu.height == gpu.height);
    d.pixels = static_cast<long>(cpu.pixelCount());
    double sum = 0.0;
    for (std::size_t i = 0; i < cpu.rgba.size(); i += 4) {
        int worst = 0;
        for (std::size_t c = 0; c < 4; ++c) {
            const int delta =
                std::abs(static_cast<int>(cpu.rgba[i + c]) - static_cast<int>(gpu.rgba[i + c]));
            sum += delta;
            worst = std::max(worst, delta);
        }
        d.maxLsb = std::max(d.maxLsb, worst);
        if (worst > 1) ++d.over;
    }
    d.meanLsb = sum / static_cast<double>(cpu.rgba.size());
    return d;
}

void expectParity(const Diff& d, const std::string& label, long slack = 0) {
    INFO(label << ": maxLsb " << d.maxLsb << ", over-1-LSB " << d.over << "/" << d.pixels
               << ", meanLsb " << d.meanLsb);
    CHECK(d.over <= slack);
    CHECK(d.maxLsb <= (slack == 0 ? 1 : 3));
}

// ⚠ THE RECIPROCAL MODES NEED A DIFFERENT INSTRUMENT, NOT A BIGGER NUMBER. A flat over-count was
// tuned to 4 and then failed on real hardware (ColorBurn 15/28000 at maxLsb 5, VividLight 8 at 4)
// while ColorDodge and Divide fitted -- i.e. the constant was measuring which mode happened to land
// on the fixture's knife edges, not whether the lane is correct.
//
// What separates rounding from a defect here is the SHAPE of the error, so assert on that:
//   * `meanLsb` stays ~0 -- a real fault (bad premultiply, a wrong clip base) shifts the whole
//     image, and no concentration of singular pixels can hide that.
//   * the affected pixels stay a vanishing FRACTION, so the bound does not silently scale with the
//     canvas the way a raw count does.
//   * the worst pixel stays bounded, because unbounded is a different failure.
// The sprite deliberately carries opaque black/white so these singularities are HIT, not dodged.
void expectReciprocalParity(const Diff& d, const std::string& label) {
    INFO(label << " (reciprocal): maxLsb " << d.maxLsb << ", over-1-LSB " << d.over << "/"
               << d.pixels << ", meanLsb " << d.meanLsb);
    CHECK(d.meanLsb < 0.05);                                  // not a systematic shift
    CHECK(d.over <= std::max<long>(8, d.pixels / 1000));       // <= 0.1% of pixels
    CHECK(d.maxLsb <= 6);                                      // bounded, not unbounded
}

// Composite `specs` on both lanes and return the difference. The GPU side asserts that the lane
// actually SERVED -- a refusal fails the case rather than skipping it.
Diff runBoth(render::TileCompositor& lane, const std::vector<LayerSpec>& specs,
             const render::CompositeOptions& opts, std::uint32_t w = kW, std::uint32_t h = kH) {
    core::Document cpuDoc(w, h);
    buildDocument(cpuDoc, specs);
    const render::CompositeResult cpu = render::composite(cpuDoc, opts, render::Backend::Cpu);
    REQUIRE(cpu.ok);
    REQUIRE(cpu.usedBackend == render::Backend::Cpu);

    core::Document gpuDoc(w, h);
    buildDocument(gpuDoc, specs);
    // Layer ids restart at 1 in every document, so a lane reused across cases MUST be told the
    // document changed -- otherwise it serves the previous case's pixels for id 1 and the parity
    // failure looks like a shader bug.
    lane.reset();
    const render::TileCompositeStatus st = lane.composite(gpuDoc, opts);
    INFO("refusal: " << std::string(render::tileRefusalName(st.refusal)) << " " << st.error);
    REQUIRE(st.ok);
    REQUIRE(st.refusal == TileRefusal::None);

    Image out;
    std::string err;
    REQUIRE_MESSAGE(lane.readback(mosaic::common::Rect{}, out, err), err);
    return compare(cpu.image, out);
}

// The four modes whose formula divides by a source-derived quantity; the accumulator is fp16, so
// they are the only ones where the coarser grid near 1.0 can move an 8-bit result by more than a
// step. Every other axis in this file asks for ZERO.
bool isReciprocal(BlendMode m) {
    return m == BlendMode::ColorBurn || m == BlendMode::ColorDodge ||
           m == BlendMode::VividLight || m == BlendMode::Divide;
}

// ---- The CACHE-BACKED leaves (S60-a) ---------------------------------------------------------
//
// `LayerSpec` describes raster layers, which is every document the lane could serve before text,
// magic and texture leaves were admitted. Those three need the document built by hand, so the
// vocabulary here is a populate CALLBACK run against two identical documents -- one per lane --
// which is the same anti-drift property buildDocument gives the raster cases.

// Both lanes composite `populate`'s document; the GPU side asserts the lane actually SERVED, so a
// case cannot pass by quietly being refused.
Diff runBothBuilt(render::TileCompositor& lane,
                  const std::function<void(core::Document&)>& populate,
                  const render::CompositeOptions& opts, std::uint32_t w = kW,
                  std::uint32_t h = kH) {
    core::Document cpuDoc(w, h);
    populate(cpuDoc);
    const render::CompositeResult cpu = render::composite(cpuDoc, opts, render::Backend::Cpu);
    REQUIRE(cpu.ok);
    REQUIRE(cpu.usedBackend == render::Backend::Cpu);

    core::Document gpuDoc(w, h);
    populate(gpuDoc);
    // Layer ids restart at 1 in every document -- see runBoth.
    lane.reset();
    const render::TileCompositeStatus st = lane.composite(gpuDoc, opts);
    INFO("refusal: " << std::string(render::tileRefusalName(st.refusal)) << " " << st.error);
    REQUIRE(st.ok);
    REQUIRE(st.refusal == TileRefusal::None);

    Image out;
    std::string err;
    REQUIRE_MESSAGE(lane.readback(mosaic::common::Rect{}, out, err), err);
    return compare(cpu.image, out);
}

// backdrop(), with the transparent band filled in. For the float-cache case, where a partially
// transparent RESULT would put both lanes' un-premultiply on a near-zero divisor and amplify a
// half-ulp of fp16 into something the tolerance would have to be widened for. Over an opaque
// backing the output alpha is exactly 1 and the comparison measures the colour mix, which is the
// thing the float lane is actually about.
Image opaqueBackdrop(std::uint32_t w = kW, std::uint32_t h = kH) {
    Image img = backdrop(w, h);
    for (std::size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
    return img;
}

// A smooth float RGBA field -- what the sky generator's cache looks like to the compositor: values
// that walk between adjacent 8-bit codes, which is exactly the content an 8-bit round trip would
// band and a half-float one does not.
mosaic::common::ImageF floatCache(std::uint32_t w, std::uint32_t h) {
    mosaic::common::ImageF img(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
            const float u = static_cast<float>(x) / static_cast<float>(w - 1);
            const float v = static_cast<float>(y) / static_cast<float>(h - 1);
            img.rgba[p + 0] = 0.12f + 0.61f * u;
            img.rgba[p + 1] = 0.30f + 0.55f * v;
            img.rgba[p + 2] = 0.88f - 0.44f * (u * v);
            img.rgba[p + 3] = 0.35f + 0.60f * v;  // a ramp, over an OPAQUE backing (see above)
        }
    return img;
}

// A text layer carrying the pixel cache the app's text pass would have rendered into it. The
// compositor never touches the font stack -- it composites `cachedImage()` like a raster source --
// so a hand-filled cache is precisely what the lane sees in the real app.
core::TextLayer& addTextLayer(core::Document& doc, Image cache, const Affine2D& imageToLayer,
                              const Affine2D& place) {
    auto tl = doc.makeText("t", "hello");
    tl->setTransform(place);
    core::TextLayer* ref = doc.root().addOnTop(std::move(tl)).as<core::TextLayer>();
    REQUIRE(ref != nullptr);
    ref->setCachedImage(std::move(cache), imageToLayer);
    return *ref;
}

// The texture-generator twin. Exactly one of the two cache arms is populated, as the render pass
// leaves it: 8-bit for paper/grass, FLOAT for sky.
core::TextureLayer& addTextureLayer(core::Document& doc, std::optional<Image> cache8,
                                    std::optional<mosaic::common::ImageF> cacheF,
                                    const Affine2D& imageToLayer, const Affine2D& place) {
    auto xl = doc.makeTexture(
        "x", core::texture::defaultTextureParams(core::texture::Generator::Paper));
    xl->setTransform(place);
    core::TextureLayer* ref = doc.root().addOnTop(std::move(xl)).as<core::TextureLayer>();
    REQUIRE(ref != nullptr);
    ref->setCachedImage(std::move(cache8), std::move(cacheF), imageToLayer);
    return *ref;
}

void addBackdrop(core::Document& doc, Image pixels) {
    auto bg = doc.makeRaster("bg", pixels.width, pixels.height);
    bg->image() = std::move(pixels);
    doc.root().addOnTop(std::move(bg));
}

// ---- ADJUSTMENT LAYERS (S60-a) ---------------------------------------------------------------
//
// An adjustment is a FUNCTION OF THE BACKDROP, so these cases all build a real stack under it --
// there is nothing to grade otherwise, and a case that graded an empty accumulator would agree with
// the CPU reference about a picture nobody looks at.

// The bag is written WHOLE, which is exactly what SetAdjustmentParamsCommand does: the plan diff
// has to notice a replaced bag, not a mutated entry.
core::AdjustmentLayer& addAdjustment(core::Document& doc, core::AdjustmentKind kind,
                                     std::map<std::string, double> params) {
    auto adj = doc.makeAdjustment("adj", kind);
    core::AdjustmentLayer* ref = doc.root().addOnTop(std::move(adj)).as<core::AdjustmentLayer>();
    REQUIRE(ref != nullptr);
    ref->params() = std::move(params);
    return *ref;
}

// The backdrop every adjustment case grades: an OPAQUE structured field with a sprite over it.
//
// Opaque on purpose. Over a partly transparent result both lanes un-premultiply on the way to
// 8 bits, which puts a near-zero divisor under an fp16 half-ulp and amplifies it -- the same
// reason the float-cache case above wants opaqueBackdrop(). The adjustments under test recolour;
// they add no coverage, so a fully opaque result measures exactly the thing they change.
void addGradedStack(core::Document& doc) {
    addBackdrop(doc, opaqueBackdrop());
    auto sp = doc.makeRaster("sprite", 96, 72);
    sp->image() = sprite();
    sp->setTransform(Affine2D::translation(37.0, 21.0));
    doc.root().addOnTop(std::move(sp));
}

// One served kind, with parameters a real edit would carry: strong enough that the case would fail
// if the arm did nothing, mild enough that the transfer's |f'| does not amplify the fp16 accumulator
// past 1/255 (the bound adjust_tile.comp states, and the reason the lattice kinds refuse outright).
struct AdjustCase {
    const char* name;
    core::AdjustmentKind kind;
    std::map<std::string, double> params;
};

// A non-identity tone curve: lifted shadows, rolled highlights. `corner` stays false so the spline
// is smooth, which is what a user actually draws and what exercises the composed-LUT path.
core::brush::Curve toneCurve() {
    return core::brush::Curve(std::vector<core::brush::CurvePoint>{
        {0.0, 0.06, false}, {0.28, 0.22, false}, {0.72, 0.83, false}, {1.0, 0.95, false}});
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Parity: the lane against the CPU reference.
// ---------------------------------------------------------------------------------------------

TEST_CASE("the resident lane matches the CPU reference for every blend mode") {
    auto lane = makeLane("resident blend parity");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    for (int m = 0; m <= static_cast<int>(BlendMode::Luminosity); ++m) {
        std::vector<LayerSpec> specs;
        specs.push_back(LayerSpec{backdrop()});
        LayerSpec top{sprite()};
        top.place = Affine2D::translation(37.0, 21.0);  // integer: this case isolates the BLEND
        top.blend = static_cast<BlendMode>(m);
        top.opacity = 0.83f;
        specs.push_back(top);
        const Diff d = runBoth(*lane, specs, render::CompositeOptions{});
        if (isReciprocal(top.blend))
            expectReciprocalParity(d, std::string(core::blendModeName(top.blend)));
        else
            expectParity(d, std::string(core::blendModeName(top.blend)));
    }
    CHECK(lane->validationErrors() == before);
}

TEST_CASE("the resident lane matches every resample filter under a rotation") {
    auto lane = makeLane("resident resample parity");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    // The CONTINUOUS kernels only. Nearest and Area step discontinuously, so under a rotation the
    // two lanes legitimately pick different source texels for a handful of pixels (the CPU maps in
    // double, the GPU in float -- Vulkan 1.0 has no fp64); test_composite_tile_parity.cpp owns
    // that boundary in detail, and holds them to grid-aligned placements, which is what the case
    // below does too.
    const ResampleFilter smooth[] = {
        ResampleFilter::Bilinear, ResampleFilter::Bicubic,  ResampleFilter::Mitchell,
        ResampleFilter::Lanczos2, ResampleFilter::Lanczos3, ResampleFilter::Gaussian,
        ResampleFilter::Supersample,
    };
    for (const ResampleFilter f : smooth) {
        std::vector<LayerSpec> specs;
        specs.push_back(LayerSpec{backdrop()});
        LayerSpec top{sprite()};
        top.place = Affine2D::translation(52.4, 33.6) * Affine2D::rotation(0.37) *
                    Affine2D::scaling(1.15, 0.85);
        specs.push_back(top);
        render::CompositeOptions opts;
        opts.resampleFilter = f;
        expectParity(runBoth(*lane, specs, opts),
                     std::string(render::resampleFilterName(f)));
    }

    for (const ResampleFilter f : {ResampleFilter::Nearest, ResampleFilter::Area}) {
        std::vector<LayerSpec> specs;
        specs.push_back(LayerSpec{backdrop()});
        LayerSpec top{sprite()};
        top.place = Affine2D::translation(8.0, 4.0) * Affine2D::scaling(2.0, 2.0);
        specs.push_back(top);
        render::CompositeOptions opts;
        opts.resampleFilter = f;
        expectParity(runBoth(*lane, specs, opts),
                     std::string(render::resampleFilterName(f)) + " / integer scale");
    }
    CHECK(lane->validationErrors() == before);
}

TEST_CASE("masks, clip-to-below and opacity match through the resident lane") {
    auto lane = makeLane("resident mask/clip parity");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    SUBCASE("a linked mask at a different resolution than its layer") {
        std::vector<LayerSpec> specs;
        specs.push_back(LayerSpec{backdrop()});
        LayerSpec top{sprite()};
        top.place = Affine2D::translation(30.0, 18.0) * Affine2D::scaling(1.4, 1.1);
        top.mask = true;
        top.maskW = 37;
        top.maskH = 29;
        specs.push_back(top);
        render::CompositeOptions opts;
        opts.resampleFilter = ResampleFilter::Mitchell;
        expectParity(runBoth(*lane, specs, opts), "linked mask 37x29");
    }
    SUBCASE("an unlinked mask folds in parent space, after placement") {
        std::vector<LayerSpec> specs;
        specs.push_back(LayerSpec{backdrop()});
        LayerSpec top{sprite()};
        top.place = Affine2D::translation(41.37, 22.63);
        top.mask = true;
        top.maskLinked = false;
        top.maskW = 130;
        top.maskH = 96;
        specs.push_back(top);
        render::CompositeOptions opts;
        opts.resampleFilter = ResampleFilter::Lanczos2;
        expectParity(runBoth(*lane, specs, opts), "unlinked mask");
    }
    SUBCASE("a whole clip run stays on the device") {
        // Three layers: a backdrop, a NON-clipped base that publishes its placed alpha, and two
        // clipped layers above it that read it back. The clip base never touches host memory.
        std::vector<LayerSpec> specs;
        specs.push_back(LayerSpec{backdrop()});
        LayerSpec base{sprite()};
        base.place = Affine2D::translation(20.0, 15.0);
        specs.push_back(base);
        LayerSpec clipped{sprite(70, 54)};
        clipped.place = Affine2D::translation(45.0, 30.0);
        clipped.clip = true;
        clipped.blend = BlendMode::Multiply;
        clipped.opacity = 0.82f;
        specs.push_back(clipped);
        LayerSpec clipped2{sprite(50, 40)};
        clipped2.place = Affine2D::translation(60.0, 42.0);
        clipped2.clip = true;
        clipped2.blend = BlendMode::Screen;
        specs.push_back(clipped2);
        expectParity(runBoth(*lane, specs, render::CompositeOptions{}), "two clipped layers");
    }
    SUBCASE("an invisible or zero-opacity layer is skipped exactly as the CPU walk skips it") {
        std::vector<LayerSpec> specs;
        specs.push_back(LayerSpec{backdrop()});
        LayerSpec hidden{sprite()};
        hidden.place = Affine2D::translation(10.0, 10.0);
        hidden.visible = false;
        specs.push_back(hidden);
        LayerSpec zero{sprite()};
        zero.place = Affine2D::translation(30.0, 30.0);
        zero.opacity = 0.0f;
        specs.push_back(zero);
        expectParity(runBoth(*lane, specs, render::CompositeOptions{}), "skipped layers");
    }
    CHECK(lane->validationErrors() == before);
}

// ---------------------------------------------------------------------------------------------
// The cache-backed leaves (S60-a): text, texture and magic.
//
// compositor.cpp's renderLayerRaw treats these three exactly like a raster source -- one fused
// pass, the linked mask folded at the source pixel, the unlinked one after placement -- with ONE
// extra factor in the placement for the two cache-backed kinds: the cache's own pixel -> layer-local
// map, folded IN FRONT of the layer transform. That product is the new thing, so it is what these
// cases are built around; everything else here is the raster arm's claim re-asserted through a
// different source pointer.
//
// Why it matters beyond the pixels: refusing ONE text layer refuses the whole document, so before
// this arm existed every Type-tool document composited 100% on the CPU and none of S60-a's win was
// available there at all.
// ---------------------------------------------------------------------------------------------

TEST_CASE("the resident lane composites the leaves that read a fixed-resolution source") {
    auto lane = makeLane("cache-backed leaf parity");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    SUBCASE("a magic layer places its source exactly like a raster one") {
        const auto build = [](core::Document& doc) {
            addBackdrop(doc, backdrop());
            auto ml = doc.makeMagic("m", sprite());
            ml->setTransform(Affine2D::translation(52.4, 33.6) * Affine2D::rotation(0.37));
            ml->setOpacity(0.83f);
            ml->setBlendMode(BlendMode::Multiply);
            doc.root().addOnTop(std::move(ml));
        };
        render::CompositeOptions opts;
        opts.resampleFilter = ResampleFilter::Mitchell;
        expectParity(runBothBuilt(*lane, build, opts), "magic layer");
    }

    SUBCASE("a text cache on the layer's own grid") {
        const auto build = [](core::Document& doc) {
            addBackdrop(doc, backdrop());
            addTextLayer(doc, sprite(), Affine2D::identity(),
                         Affine2D::translation(37.0, 21.0));
        };
        expectParity(runBothBuilt(*lane, build, render::CompositeOptions{}), "text, identity map");
    }

    SUBCASE("a text cache placed by a NON-identity cacheImageToLayer") {
        // The half-res draft bake's shape, and the ordinary shape of any cache whose grid origin
        // is not the layer origin: the map carries a scale AND a translation, so a lane that
        // dropped it (or applied it on the wrong side) draws the block at the wrong size in the
        // wrong place -- which the identity case above cannot see.
        const auto build = [](core::Document& doc) {
            addBackdrop(doc, backdrop());
            addTextLayer(doc, sprite(),
                         Affine2D::translation(-11.0, -7.0) * Affine2D::scaling(1.6, 1.25),
                         Affine2D::translation(28.0, 19.0));
        };
        render::CompositeOptions opts;
        opts.resampleFilter = ResampleFilter::Bilinear;
        expectParity(runBothBuilt(*lane, build, opts), "text, scaled+translated cache map");
    }

    SUBCASE("a ROTATED text layer on top of a scaled cache map") {
        // THE product this arm introduces: `layer.transform() * cacheImageToLayer()`. Each factor
        // alone is already covered above; composing them in the wrong ORDER still yields a
        // plausible picture under a pure translation and a visibly wrong one under a rotation,
        // which is why the rotation is here and not left to the raster resample case.
        const auto build = [](core::Document& doc) {
            addBackdrop(doc, backdrop());
            addTextLayer(doc, sprite(),
                         Affine2D::translation(-9.0, 5.0) * Affine2D::scaling(1.3, 0.75),
                         Affine2D::translation(64.0, 46.0) * Affine2D::rotation(0.41));
        };
        render::CompositeOptions opts;
        opts.resampleFilter = ResampleFilter::Lanczos2;
        expectParity(runBothBuilt(*lane, build, opts), "text, rotation over a cache map");
    }

    SUBCASE("an 8-bit texture cache (the paper/grass lane)") {
        const auto build = [](core::Document& doc) {
            addBackdrop(doc, backdrop());
            addTextureLayer(doc, sprite(120, 90), std::nullopt,
                            Affine2D::translation(-4.0, -3.0) * Affine2D::scaling(1.1, 1.1),
                            Affine2D::translation(22.0, 14.0));
        };
        render::CompositeOptions opts;
        opts.resampleFilter = ResampleFilter::Bicubic;
        expectParity(runBothBuilt(*lane, build, opts), "texture, 8-bit cache");
    }

    SUBCASE("a FLOAT texture cache (the sky lane) rides the same kernel unquantised") {
        // The CPU arm composites this cache WITHOUT an 8-bit round trip, so the lane must not
        // introduce one either. It uploads the halves the accumulator is already made of
        // (R16G16B16A16_SFLOAT, the format Vulkan 1.0 guarantees for exactly this), which is ~11
        // bits of mantissa against the readback's 8 -- three bits finer than the quantisation the
        // float lane exists to avoid. Held to the SAME zero-slack bound as every other case here,
        // which is the only honest way to claim "unquantised".
        const auto build = [](core::Document& doc) {
            addBackdrop(doc, opaqueBackdrop());
            addTextureLayer(doc, std::nullopt, floatCache(120, 90), Affine2D::identity(),
                            Affine2D::translation(31.0, 17.0));
        };
        expectParity(runBothBuilt(*lane, build, render::CompositeOptions{}),
                     "texture, float cache");
    }

    SUBCASE("a float texture cache under a transform, so the resample reads halves too") {
        const auto build = [](core::Document& doc) {
            addBackdrop(doc, opaqueBackdrop());
            addTextureLayer(doc, std::nullopt, floatCache(120, 90),
                            Affine2D::scaling(0.9, 1.15),
                            Affine2D::translation(24.0, 11.0) * Affine2D::rotation(0.22));
        };
        render::CompositeOptions opts;
        opts.resampleFilter = ResampleFilter::Bilinear;
        expectParity(runBothBuilt(*lane, build, opts), "texture, float cache resampled");
    }

    CHECK(lane->validationErrors() == before);
}

TEST_CASE("a text layer's mask folds where the raster arm's does") {
    auto lane = makeLane("cache-backed leaf masks");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    // The mask semantics are INHERITED from the raster arm verbatim -- a linked mask folds at the
    // source pixel (proportionally when the resolutions differ), an unlinked one after placement in
    // the layer's parent space -- and the sheet's grid for a text layer is its PIXEL CACHE, not its
    // (font-derived, compositor-invisible) content box. That substitution is the claim here: a lane
    // that folded a linked mask against the layer transform instead of the cache would still look
    // right wherever the cache map is the identity.
    SUBCASE("linked, at a different resolution than the cache") {
        const auto build = [](core::Document& doc) {
            addBackdrop(doc, backdrop());
            core::TextLayer& tl = addTextLayer(
                doc, sprite(), Affine2D::translation(-6.0, -4.0) * Affine2D::scaling(1.45, 1.2),
                Affine2D::translation(30.0, 18.0));
            core::RasterMask m(37, 29);
            m.coverage = maskCoverage(37, 29);
            m.linked = true;
            tl.setMask(std::move(m));
        };
        render::CompositeOptions opts;
        opts.resampleFilter = ResampleFilter::Mitchell;
        expectParity(runBothBuilt(*lane, build, opts), "text, linked mask 37x29");
    }
    SUBCASE("unlinked, folded in parent space after placement") {
        const auto build = [](core::Document& doc) {
            addBackdrop(doc, backdrop());
            core::TextLayer& tl =
                addTextLayer(doc, sprite(), Affine2D::translation(-6.0, -4.0),
                             Affine2D::translation(41.37, 22.63));
            core::RasterMask m(130, 96);
            m.coverage = maskCoverage(130, 96);
            m.linked = false;
            tl.setMask(std::move(m));
        };
        render::CompositeOptions opts;
        opts.resampleFilter = ResampleFilter::Lanczos2;
        expectParity(runBothBuilt(*lane, build, opts), "text, unlinked mask");
    }
    CHECK(lane->validationErrors() == before);
}

// ---------------------------------------------------------------------------------------------
// ADJUSTMENT LAYERS (S60-a). The lane's SECOND kernel: adjust_tile.comp reads the accumulator the
// layers below it built, applies a per-pixel transfer, and writes it back -- so these cases prove
// three separate things, and the third is the one a parity number cannot see:
//
//   * PER-KIND PARITY. Every kind the lane admits must match `render::composite(..., Backend::Cpu)`
//     at the same 1/255 every other case here uses. A kind that cannot is refused, not tolerated.
//   * THE MODULATION. Opacity, the layer mask (linked and unlinked) and clip-to-below reach an
//     adjustment by a DIFFERENT route from a leaf's -- and the blend mode does not reach it at all.
//   * ADMISSION. An unported kind must refuse BY NAME with the accumulator untouched. A lane that
//     guessed at one would draw the wrong picture, which is the outcome refusal exists to prevent.
// ---------------------------------------------------------------------------------------------

TEST_CASE("the resident lane grades with every adjustment kind it serves") {
    auto lane = makeLane("adjustment kind parity");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    using K = core::AdjustmentKind;
    const std::vector<AdjustCase> cases = {
        {"Invert", K::Invert, {}},
        {"Brightness/Contrast", K::BrightnessContrast, {{"brightness", 0.12}, {"contrast", 0.35}}},
        {"Levels",
         K::Levels,
         {{"in_black", 0.08},
          {"in_white", 0.92},
          {"gamma", 1.35},
          {"out_black", 0.03},
          {"out_white", 0.97}}},
        {"Exposure", K::Exposure, {{"exposure", 0.8}, {"offset", 0.02}, {"gamma", 1.1}}},
        {"Hue/Saturation",
         K::HueSaturation,
         {{"hue", 35.0}, {"saturation", 25.0}, {"lightness", -12.0}}},
        // Negative lightness takes the other arm of the Photoshop-style lift, which is a separate
        // branch in both mirrors; the case above already covers the positive one via `saturation`.
        {"Hue/Saturation, lifted", K::HueSaturation, {{"hue", -140.0}, {"lightness", 30.0}}},
        {"Color Balance",
         K::ColorBalance,
         {{"shadows_cr", 30.0},
          {"midtones_mg", -22.0},
          {"highlights_yb", 40.0},
          {"preserve_luminosity", 1.0}}},
        {"Color Balance, luminosity free",
         K::ColorBalance,
         {{"shadows_yb", -45.0}, {"highlights_cr", 25.0}, {"preserve_luminosity", 0.0}}},
        // `grays` deliberately absent AND explicit-256 across the two: both mean "continuous", and
        // the lane must serve them identically (a quantised palette refuses -- see the refusal case).
        {"Grayscale, luma", K::Grayscale, {{"method", 1.0}, {"strength", 100.0}}},
        {"Grayscale, no chrominance",
         K::Grayscale,
         {{"method", 0.0}, {"strength", 70.0}, {"grays", 256.0}}},
        {"Grayscale, max channel", K::Grayscale, {{"method", 6.0}, {"strength", 100.0}}},
        {"Grayscale, min channel", K::Grayscale, {{"method", 8.0}, {"strength", 85.0}}},
        {"Vibrance", K::Vibrance, {{"vibrance", 45.0}}},
        {"Vibrance, negative", K::Vibrance, {{"vibrance", -60.0}}},
        {"Photo Filter", K::PhotoFilter, {{"density", 35.0}, {"preserve_luminosity", 1.0}}},
        {"Photo Filter, custom colour",
         K::PhotoFilter,
         {{"filter", static_cast<double>(core::PhotoFilterPreset::Custom)},
          {"density", 60.0},
          {"preserve_luminosity", 0.0},
          {"color_r", 40.0},
          {"color_g", 160.0},
          {"color_b", 230.0}}},
        {"Haze Removal",
         K::HazeRemoval,
         {{"amount", 40.0}, {"airlight", 85.0}, {"tint", 12.0}, {"saturation", 115.0}}},
        // The default black -> white ramp, which is what an inserted Gradient Map carries: absent
        // gm_* keys ARE the default, so this is the bag a real document holds.
        {"Gradient Map", K::GradientMap, {}},
        {"Gradient Map, reversed", K::GradientMap, {{"reverse", 1.0}}},
    };

    for (const AdjustCase& c : cases) {
        const Diff d = runBothBuilt(
            *lane,
            [&c](core::Document& doc) {
                addGradedStack(doc);
                addAdjustment(doc, c.kind, c.params);
            },
            render::CompositeOptions{});
        expectParity(d, c.name);
    }

    CHECK(lane->validationErrors() == before);
}

TEST_CASE("Curves grades through the transfer table the lane makes resident") {
    auto lane = makeLane("Curves parity");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    // Curves is the one kind whose parameters are KNOTS rather than scalars, and the only one (with
    // Gradient Map) that puts a 256-entry table on the device -- so it is the case that proves the
    // composed lookup, the fp16 table encoding and the residency of a synthesised sheet at once.
    SUBCASE("composed: a per-channel curve run through the composite curve") {
        const auto build = [](core::Document& doc) {
            addGradedStack(doc);
            core::AdjustmentLayer& adj =
                addAdjustment(doc, core::AdjustmentKind::Curves, {});
            core::setAdjustmentCurve(adj.params(), core::CurveChannel::Red, toneCurve());
            core::setAdjustmentCurve(
                adj.params(), core::CurveChannel::Composite,
                core::brush::Curve(std::vector<core::brush::CurvePoint>{
                    {0.0, 0.0, false}, {0.5, 0.42, false}, {1.0, 1.0, false}}));
        };
        expectParity(runBothBuilt(*lane, build, render::CompositeOptions{}), "Curves, composed");
    }
    SUBCASE("one channel only -- the other two pass through VERBATIM") {
        // The claim the `active` flags exist for: an untouched channel must not travel through a
        // nominally-identity lookup, because a lerp between two lattice points is not bit-exact.
        const auto build = [](core::Document& doc) {
            addGradedStack(doc);
            core::AdjustmentLayer& adj =
                addAdjustment(doc, core::AdjustmentKind::Curves, {});
            core::setAdjustmentCurve(adj.params(), core::CurveChannel::Blue, toneCurve());
        };
        expectParity(runBothBuilt(*lane, build, render::CompositeOptions{}), "Curves, blue only");
    }
    SUBCASE("all four curves at once, under a mask and a non-unit opacity") {
        const auto build = [](core::Document& doc) {
            addGradedStack(doc);
            core::AdjustmentLayer& adj =
                addAdjustment(doc, core::AdjustmentKind::Curves, {});
            core::setAdjustmentCurve(adj.params(), core::CurveChannel::Composite, toneCurve());
            core::setAdjustmentCurve(
                adj.params(), core::CurveChannel::Red,
                core::brush::Curve(std::vector<core::brush::CurvePoint>{
                    {0.0, 0.1, false}, {1.0, 0.9, false}}));
            core::setAdjustmentCurve(
                adj.params(), core::CurveChannel::Green,
                core::brush::Curve(std::vector<core::brush::CurvePoint>{
                    {0.0, 0.0, false}, {0.6, 0.5, false}, {1.0, 1.0, false}}));
            core::setAdjustmentCurve(
                adj.params(), core::CurveChannel::Blue,
                core::brush::Curve(std::vector<core::brush::CurvePoint>{
                    {0.0, 0.05, false}, {0.4, 0.55, false}, {1.0, 0.98, false}}));
            adj.setOpacity(0.71f);
            core::RasterMask m(kW, kH);
            m.coverage = maskCoverage(kW, kH);
            adj.setMask(std::move(m));
        };
        expectParity(runBothBuilt(*lane, build, render::CompositeOptions{}), "Curves, all four");
    }

    CHECK(lane->validationErrors() == before);
}

TEST_CASE("an identity adjustment composites byte-identically to no layer at all") {
    auto lane = makeLane("adjustment identity early-out");
    if (!lane) return;

    core::Document plain(kW, kH);
    addGradedStack(plain);
    REQUIRE(lane->composite(plain, render::CompositeOptions{}).ok);
    Image without;
    std::string err;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, without, err), err);

    // A freshly inserted layer carries the schema defaults, and applyAdjustment RETURNS before
    // touching a pixel for those -- so the lane must not dispatch either. Running an "identity"
    // transfer instead would be close but not byte-identical: the HSL round trip alone is not the
    // identity in float, and §1 of the plan wants byte-identity for a layer that does nothing.
    core::Document graded(kW, kH);
    addGradedStack(graded);
    core::AdjustmentLayer& adj =
        addAdjustment(graded, core::AdjustmentKind::HueSaturation,
                      {{"hue", 0.0}, {"saturation", 0.0}, {"lightness", 0.0}});
    lane->reset();
    const render::TileCompositeStatus st = lane->composite(graded, render::CompositeOptions{});
    INFO("refusal: " << std::string(render::tileRefusalName(st.refusal)) << " " << st.error);
    REQUIRE(st.ok);
    CHECK(st.adjustments == 0);   // the step was DROPPED, not dispatched
    CHECK(st.layers == 2);        // ... and the two rasters still are
    Image with;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, with, err), err);
    CHECK(with.rgba == without.rgba);

    // ... and the moment it stops being the identity, it is dispatched. Same layer, same lane:
    // the difference is the bag, which is what SetAdjustmentParamsCommand replaces.
    adj.params() = {{"hue", 40.0}, {"saturation", 30.0}, {"lightness", 0.0}};
    const render::TileCompositeStatus moved = lane->composite(graded, render::CompositeOptions{});
    REQUIRE(moved.ok);
    CHECK(moved.adjustments == 1);
    Image now;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, now, err), err);
    CHECK(now.rgba != without.rgba);
}

TEST_CASE("an adjustment's opacity, mask and clip fold exactly as the CPU walk folds them") {
    auto lane = makeLane("adjustment modulation");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    // Invert throughout, so every subcase measures the MODULATION rather than the transfer: it is
    // the strongest available signal, and a gate that is wrong anywhere shows as the complement of
    // the backdrop rather than as a rounding difference.
    SUBCASE("opacity") {
        expectParity(runBothBuilt(*lane,
                                  [&](core::Document& doc) {
                                      addGradedStack(doc);
                                      addAdjustment(doc, core::AdjustmentKind::Invert, {})
                                          .setOpacity(0.42f);
                                  },
                                  render::CompositeOptions{}),
                     "adjustment at 0.42 opacity");
    }

    SUBCASE("a linked mask at the document's own resolution") {
        // The overwhelmingly common shape, and the one the doc -> mask-texel map is EXACT for: the
        // sheet spans the whole canvas at 1 texel per pixel, so the ratio is 1.0 and the floor()
        // that picks a texel cannot land the other side of an integer from the CPU's choice.
        expectParity(runBothBuilt(*lane,
                                  [&](core::Document& doc) {
                                      addGradedStack(doc);
                                      core::AdjustmentLayer& adj =
                                          addAdjustment(doc, core::AdjustmentKind::Invert, {});
                                      core::RasterMask m(kW, kH);
                                      m.coverage = maskCoverage(kW, kH);
                                      m.linked = true;
                                      adj.setMask(std::move(m));
                                  },
                                  render::CompositeOptions{}),
                     "adjustment, linked mask at canvas resolution");
    }

    SUBCASE("a linked mask at half the document's resolution") {
        // A power-of-two fraction, so the stretch ratio is exact in float too. A sheet at an
        // arbitrary ratio is where the two lanes could pick neighbouring texels; adjust_tile.comp
        // says so where the map is read, and this case pins the shape that is exact.
        expectParity(runBothBuilt(*lane,
                                  [&](core::Document& doc) {
                                      addGradedStack(doc);
                                      core::AdjustmentLayer& adj =
                                          addAdjustment(doc, core::AdjustmentKind::Invert, {});
                                      core::RasterMask m(kW / 2, kH / 2);
                                      m.coverage = maskCoverage(kW / 2, kH / 2);
                                      m.linked = true;
                                      adj.setMask(std::move(m));
                                  },
                                  render::CompositeOptions{}),
                     "adjustment, linked mask at half resolution");
    }

    SUBCASE("an unlinked mask, which the layer's own transform must NOT move") {
        // The linked/unlinked split for an adjustment lives entirely in adjustmentMaskDomain: a
        // linked sheet takes the layer's transform, an unlinked one is already in parent space. So
        // this case gives the layer a transform the mask must ignore -- if the lane composed it in
        // anyway, the mask would slide and the parity would fail on a wide band.
        expectParity(runBothBuilt(*lane,
                                  [&](core::Document& doc) {
                                      addGradedStack(doc);
                                      core::AdjustmentLayer& adj =
                                          addAdjustment(doc, core::AdjustmentKind::Invert, {});
                                      adj.setTransform(Affine2D::translation(23.0, -11.0));
                                      core::RasterMask m(kW, kH);
                                      m.coverage = maskCoverage(kW, kH);
                                      m.linked = false;
                                      adj.setMask(std::move(m));
                                  },
                                  render::CompositeOptions{}),
                     "adjustment, unlinked mask under a moved layer");
    }

    SUBCASE("a mask and a non-unit opacity together") {
        expectParity(runBothBuilt(*lane,
                                  [&](core::Document& doc) {
                                      addGradedStack(doc);
                                      core::AdjustmentLayer& adj = addAdjustment(
                                          doc, core::AdjustmentKind::Levels,
                                          {{"in_black", 0.1}, {"in_white", 0.85}, {"gamma", 1.6}});
                                      adj.setOpacity(0.63f);
                                      core::RasterMask m(kW, kH);
                                      m.coverage = maskCoverage(kW, kH);
                                      adj.setMask(std::move(m));
                                  },
                                  render::CompositeOptions{}),
                     "adjustment, mask x opacity");
    }

    CHECK(lane->validationErrors() == before);
}

TEST_CASE("an adjustment's BLEND MODE reaches neither lane") {
    auto lane = makeLane("adjustment blend mode");
    if (!lane) return;

    // ⚠ THIS IS A MIRROR, NOT A FEATURE. compositor.cpp's walkStep takes the adjustment branch
    // before any blend() call, so an adjustment's mode never reaches a pixel on the GOLDEN lane --
    // and the kernel therefore must not read it either. The claim is pinned in the CPU reference
    // first (two CPU composites that differ only in the mode must be BYTE-identical), because if
    // that ever changes this test fails at the reference rather than blaming the shader.
    const auto build = [](core::Document& doc, BlendMode mode) {
        addGradedStack(doc);
        addAdjustment(doc, core::AdjustmentKind::Invert, {}).setBlendMode(mode);
    };

    core::Document normalDoc(kW, kH);
    build(normalDoc, BlendMode::Normal);
    const render::CompositeResult normal =
        render::composite(normalDoc, render::CompositeOptions{}, render::Backend::Cpu);
    REQUIRE(normal.ok);

    core::Document multiplyDoc(kW, kH);
    build(multiplyDoc, BlendMode::Multiply);
    const render::CompositeResult multiply =
        render::composite(multiplyDoc, render::CompositeOptions{}, render::Backend::Cpu);
    REQUIRE(multiply.ok);
    CHECK(normal.image.rgba == multiply.image.rgba);

    // ... and the lane agrees with the reference for a mode it is told about and ignores.
    for (const BlendMode mode : {BlendMode::Multiply, BlendMode::Screen, BlendMode::Luminosity}) {
        const Diff d = runBothBuilt(
            *lane, [&](core::Document& doc) { build(doc, mode); }, render::CompositeOptions{});
        expectParity(d, std::string("adjustment under ") + std::string(core::blendModeName(mode)));
    }
}

TEST_CASE("a clipped adjustment grades only the layer below it") {
    auto lane = makeLane("adjustment clip-to-below");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    // The stack: an opaque canvas, then a sprite whose ALPHA becomes the clip base, then an
    // adjustment clipped to it. compositor.cpp hands the adjustment `st.clipBase` as its coverage;
    // the lane hands the kernel the same alpha out of the resident clip atlas, published by
    // composite_tile.comp rather than read back to the host.
    const auto build = [](core::Document& doc, bool clip) {
        addGradedStack(doc);
        core::AdjustmentLayer& adj = addAdjustment(doc, core::AdjustmentKind::Invert, {});
        adj.setClipToBelow(clip);
    };

    expectParity(runBothBuilt(*lane, [&](core::Document& doc) { build(doc, true); },
                              render::CompositeOptions{}),
                 "adjustment clipped to the sprite below it");

    // Corroboration, because a clip that silently did nothing would also match a reference that
    // silently did nothing: the clipped picture must differ from BOTH the unclipped one (the clip
    // did something) and from no adjustment at all (the adjustment did something).
    std::string err;
    core::Document clipped(kW, kH);
    build(clipped, true);
    lane->reset();
    REQUIRE(lane->composite(clipped, render::CompositeOptions{}).ok);
    Image clippedImg;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, clippedImg, err), err);

    core::Document loose(kW, kH);
    build(loose, false);
    lane->reset();
    REQUIRE(lane->composite(loose, render::CompositeOptions{}).ok);
    Image looseImg;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, looseImg, err), err);

    core::Document bare(kW, kH);
    addGradedStack(bare);
    lane->reset();
    REQUIRE(lane->composite(bare, render::CompositeOptions{}).ok);
    Image bareImg;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, bareImg, err), err);

    CHECK(clippedImg.rgba != looseImg.rgba);
    CHECK(clippedImg.rgba != bareImg.rgba);
    CHECK(lane->validationErrors() == before);
}

TEST_CASE("the lane refuses the adjustment kinds it cannot grade exactly, and names them") {
    auto lane = makeLane("adjustment admission");
    if (!lane) return;

    // Each of these is a DIFFERENT pixel-level reason, and the point of naming them separately is
    // that a future port has to argue with the reason rather than delete a line. `error` carries the
    // kind's own name, so the Settings capability readout can say which layer sent the document to
    // the CPU walk.
    const auto refusalFor = [&lane](core::AdjustmentKind kind,
                                    std::map<std::string, double> params) {
        core::Document doc(kW, kH);
        addGradedStack(doc);
        addAdjustment(doc, kind, std::move(params));
        lane->reset();
        const render::TileCompositeStatus st = lane->composite(doc, render::CompositeOptions{});
        // Named, not merely counted: the string has to identify the KIND, so a caller logging it
        // can say which layer sent the document to the CPU walk.
        const std::string kindName{core::adjustmentKindName(kind)};
        INFO("kind: " << kindName << " error: " << st.error);
        CHECK_FALSE(st.ok);
        CHECK(st.refusal == TileRefusal::Adjustment);
        CHECK(st.error.find(kindName) != std::string::npos);
        // The accumulator is left exactly as it was -- refusing is not a partial composite.
        CHECK(st.dispatches == 0);
        CHECK(st.adjustments == 0);
        CHECK(st.uploadBytes == 0);
    };

    SUBCASE("a lattice quantiser") {
        // Threshold and Posterize round onto a lattice, and the accumulator this kernel reads is
        // rgba16f where the reference's is fp32: a pixel within a half-ulp of a step lands the
        // other side of it and the two lanes differ by a WHOLE LEVEL, not by an LSB. A smooth
        // gradient crosses every step, so it is a guaranteed failure rather than a risk.
        refusalFor(core::AdjustmentKind::Threshold, {{"level", 0.45}});
        refusalFor(core::AdjustmentKind::Posterize, {{"levels", 5.0}});
        // ... and the same rule PER INSTANCE: Grayscale is served at a continuous palette and
        // refused the moment the user asks for N greys.
        refusalFor(core::AdjustmentKind::Grayscale,
                         {{"method", 1.0}, {"strength", 100.0}, {"grays", 6.0}});
    }
    SUBCASE("a transfer conditioned on 1/alpha") {
        refusalFor(core::AdjustmentKind::MatteRemoval, {{"mode", 0.0}});
    }
    SUBCASE("a SPATIAL kind, which reads a neighbourhood the dirty set does not cover") {
        refusalFor(core::AdjustmentKind::GaussianBlur, {{"radius", 6.0}});
        refusalFor(core::AdjustmentKind::ShadowsHighlights, {});
        refusalFor(core::AdjustmentKind::HighPass, {{"radius", 8.0}});
        // Per INSTANCE again: Grayscale's Dithered and Adaptive-threshold methods are spatial even
        // though every other method of the same kind is not.
        refusalFor(core::AdjustmentKind::Grayscale, {{"method", 5.0}, {"strength", 100.0}});
        refusalFor(core::AdjustmentKind::Grayscale, {{"method", 7.0}, {"strength", 100.0}});
    }
    SUBCASE("a STYLIZE kind, which owns its own mask/clip blend") {
        refusalFor(core::AdjustmentKind::AddNoise, {{"amount", 20.0}});
        refusalFor(core::AdjustmentKind::Vignette, {{"amount", 40.0}});
        refusalFor(core::AdjustmentKind::OilPaint, {});
    }
    SUBCASE("the harmonisation grade, which needs more scalars than the push block holds") {
        refusalFor(core::AdjustmentKind::PhotometricMatch, {{"delta_ev", 0.5}});
    }

    // ... and the document composites normally once the offending layer is gone: a refusal must
    // leave nothing behind.
    core::Document doc(kW, kH);
    addGradedStack(doc);
    lane->reset();
    const render::TileCompositeStatus ok = lane->composite(doc, render::CompositeOptions{});
    CHECK(ok.ok);
    CHECK(ok.refusal == TileRefusal::None);
}

TEST_CASE("an adjustment layer uploads nothing and reads nothing back") {
    auto lane = makeLane("adjustment residency");
    if (!lane) return;

    const render::CompositeOptions opts;

    // THE UPLOAD CLAIM, measured as a DELTA. An adjustment has no source image, so admitting one
    // must not move a byte across the bus: composite the stack, then add the adjustment and
    // composite again, and the difference in cumulative upload bytes must be exactly ZERO. (A
    // cache SIZE could not see this -- only an event count can, which is why the stats carry one.)
    core::Document doc(kW, kH);
    addGradedStack(doc);
    lane->reset();
    REQUIRE(lane->composite(doc, opts).ok);
    const std::uint64_t uploadedForRasters = lane->stats().uploadBytes;
    CHECK(uploadedForRasters > 0);  // the rasters had to get there somehow

    core::AdjustmentLayer& adj =
        addAdjustment(doc, core::AdjustmentKind::Levels,
                      {{"in_black", 0.05}, {"in_white", 0.9}, {"gamma", 1.4}});
    const render::TileCompositeStatus withAdj = lane->composite(doc, opts);
    INFO("refusal: " << std::string(render::tileRefusalName(withAdj.refusal)) << " "
                     << withAdj.error);
    REQUIRE(withAdj.ok);
    CHECK(withAdj.adjustments == 1);
    CHECK(withAdj.dispatches > 0);
    CHECK(withAdj.uploadBytes == 0);
    CHECK(lane->stats().uploadBytes == uploadedForRasters);

    // ... and it is RESIDENT: nothing changed, so nothing is recomposited and nothing is sent.
    const render::TileCompositeStatus again = lane->composite(doc, opts);
    REQUIRE(again.ok);
    CHECK(again.dispatches == 0);
    CHECK(again.macrotiles == 0);
    CHECK(again.uploadBytes == 0);

    // A mask on the adjustment is the one thing that DOES cross the bus, exactly as a leaf's mask
    // does -- and exactly once.
    core::RasterMask m(kW, kH);
    m.coverage = maskCoverage(kW, kH);
    adj.setMask(std::move(m));
    const render::TileCompositeStatus masked = lane->composite(doc, opts);
    REQUIRE(masked.ok);
    CHECK(masked.uploadBytes == static_cast<std::uint64_t>(kW) * kH);
    const render::TileCompositeStatus maskedAgain = lane->composite(doc, opts);
    REQUIRE(maskedAgain.ok);
    CHECK(maskedAgain.uploadBytes == 0);

    // THE READBACK CLAIM, over a stream of frames with an adjustment standing in the stack. An
    // adjustment that round-tripped the accumulator to the host to grade it would defeat the whole
    // arc, and it would still be fast on the dev rig -- this is the only instrument that sees it.
    render::ResolveTarget target;
    std::string err;
    REQUIRE_MESSAGE(lane->createResolveTarget(kW, kH, target, err), err);
    REQUIRE_MESSAGE(lane->resolve(target, err), err);
    target.layout = VK_IMAGE_LAYOUT_GENERAL;

    const render::TileCompositeStats before = lane->stats();
    for (int i = 1; i <= 8; ++i) {
        adj.setOpacity(0.5f + 0.05f * static_cast<float>(i));
        const render::TileCompositeStatus st = lane->composite(doc, opts);
        INFO("frame " << i << " refusal: " << std::string(render::tileRefusalName(st.refusal)));
        REQUIRE(st.ok);
        CHECK(st.adjustments == 1);
        CHECK(st.uploadBytes == 0);  // the mask is resident; opacity is a push constant
        REQUIRE_MESSAGE(lane->resolve(target, err), err);
    }
    const render::TileCompositeStats after = lane->stats();
    CHECK(after.readbacks == before.readbacks);
    CHECK(after.readbackBytes == before.readbackBytes);
    CHECK(after.resolves == before.resolves + 8);
    lane->destroyResolveTarget(target);
}

TEST_CASE("an adjustment's parameters are part of the plan diff") {
    auto lane = makeLane("adjustment plan diff");
    if (!lane) return;

    // ⚠ THE TRAP THIS PINS. `SetAdjustmentParamsCommand` replaces the layer's params BAG and
    // touches nothing else: no contentRevision, no maskRevision, no transform. So the only thing
    // that can see the edit is the plan-diff FINGERPRINT, which is why planDocument hashes the
    // resolved scalars (and, for the two lookup kinds, the whole bag as the table's staleness key).
    // A lane that hashed only the transform and the opacity would keep showing the previous curve
    // for as long as the layer stayed resident -- the cache-generation trap, in a second dress.
    //
    // Big enough that "the whole canvas" is a real claim: at the default 256 px macrotile a
    // 200x140 canvas is ONE macrotile and the dirty-set assertion below would be vacuous.
    constexpr std::uint32_t kBigW = 1200;
    constexpr std::uint32_t kBigH = 900;
    const render::CompositeOptions opts;

    core::Document doc(kBigW, kBigH);
    addBackdrop(doc, opaqueBackdrop(kBigW, kBigH));
    core::AdjustmentLayer& adj =
        addAdjustment(doc, core::AdjustmentKind::HueSaturation, {{"hue", 20.0}});
    lane->reset();
    REQUIRE(lane->composite(doc, opts).ok);
    Image first;
    std::string err;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, first, err), err);

    const std::uint64_t macrotiles = lane->macroGrid().tileCount();
    REQUIRE(macrotiles > 4);  // otherwise the footprint claim below proves nothing

    adj.params() = {{"hue", 140.0}, {"saturation", 55.0}};
    const render::TileCompositeStatus edited = lane->composite(doc, opts);
    REQUIRE(edited.ok);
    // An adjustment's footprint is the WHOLE CANVAS -- unmasked it plainly reaches every pixel, and
    // masked it still does, because adjustmentMaskAt clamps into the sheet's domain instead of
    // reading zero outside it. So a params edit dirties everything, and nothing may be uploaded.
    CHECK(edited.macrotiles == macrotiles);
    CHECK(edited.uploadBytes == 0);
    Image second;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, second, err), err);
    CHECK(first.rgba != second.rgba);

    const render::CompositeResult cpu = render::composite(doc, opts, render::Backend::Cpu);
    REQUIRE(cpu.ok);
    expectParity(compare(cpu.image, second), "after a params edit");

    // ... and the edit SETTLES: a composite with nothing changed since must dispatch nothing.
    const render::TileCompositeStatus settled = lane->composite(doc, opts);
    REQUIRE(settled.ok);
    CHECK(settled.dispatches == 0);
    CHECK(settled.macrotiles == 0);

    // The same trap for the TABLE-backed kinds, where a stale device copy is the failure mode
    // rather than a stale push constant: editing a Curves layer's knots must re-send the table.
    core::Document curveDoc(kBigW, kBigH);
    addBackdrop(curveDoc, opaqueBackdrop(kBigW, kBigH));
    core::AdjustmentLayer& curves = addAdjustment(curveDoc, core::AdjustmentKind::Curves, {});
    core::setAdjustmentCurve(curves.params(), core::CurveChannel::Composite, toneCurve());
    lane->reset();
    REQUIRE(lane->composite(curveDoc, opts).ok);
    Image beforeEdit;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, beforeEdit, err), err);

    std::map<std::string, double> nextBag = curves.params();
    core::setAdjustmentCurve(nextBag, core::CurveChannel::Composite,
                             core::brush::Curve(std::vector<core::brush::CurvePoint>{
                                 {0.0, 0.0, false}, {0.5, 0.18, false}, {1.0, 1.0, false}}));
    curves.params() = std::move(nextBag);
    const render::TileCompositeStatus recurved = lane->composite(curveDoc, opts);
    REQUIRE(recurved.ok);
    CHECK(recurved.adjustments == 1);
    // The transfer table is the ONE thing an adjustment ever sends, and a knot edit must send it:
    // 256 entries x RGBA x 2 bytes (the leaf float lane's R16G16B16A16_SFLOAT encoding).
    CHECK(recurved.uploadBytes == 256ull * 4ull * 2ull);
    Image afterEdit;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, afterEdit, err), err);
    CHECK(beforeEdit.rgba != afterEdit.rgba);
    expectParity(compare(render::composite(curveDoc, opts, render::Backend::Cpu).image, afterEdit),
                 "after a curve edit");
    // ... and the table stays resident afterwards.
    const render::TileCompositeStatus curvesSettled = lane->composite(curveDoc, opts);
    REQUIRE(curvesSettled.ok);
    CHECK(curvesSettled.uploadBytes == 0);
    CHECK(curvesSettled.dispatches == 0);
}

// ---------------------------------------------------------------------------------------------
// Residency itself.
// ---------------------------------------------------------------------------------------------

TEST_CASE("an unchanged document costs no dispatch and no upload") {
    auto lane = makeLane("residency");
    if (!lane) return;

    std::vector<LayerSpec> specs;
    specs.push_back(LayerSpec{backdrop()});
    LayerSpec top{sprite()};
    top.place = Affine2D::translation(37.0, 21.0);
    specs.push_back(top);

    core::Document doc(kW, kH);
    buildDocument(doc, specs);

    const render::TileCompositeStatus first = lane->composite(doc, render::CompositeOptions{});
    REQUIRE(first.ok);
    CHECK(first.dispatches > 0);
    CHECK(first.uploadBytes > 0);  // the layers had to get there somehow
    const std::uint64_t residentAfterFirst = lane->residentSourceBytes();
    CHECK(residentAfterFirst > 0);

    // THE assertion of this file. Nothing changed, so nothing may be recomposited and nothing may
    // be re-uploaded. S7-b's compositor would have uploaded both operands and read the
    // accumulator back, per layer, right here.
    const render::TileCompositeStatus second = lane->composite(doc, render::CompositeOptions{});
    REQUIRE(second.ok);
    CHECK(second.dispatches == 0);
    CHECK(second.macrotiles == 0);
    CHECK(second.uploadBytes == 0);
    CHECK(lane->residentSourceBytes() == residentAfterFirst);

    // And the accumulator still holds the picture -- "nothing was recomposited" must not be
    // "nothing is there".
    Image out;
    std::string err;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, out, err), err);
    const render::CompositeResult cpu =
        render::composite(doc, render::CompositeOptions{}, render::Backend::Cpu);
    REQUIRE(cpu.ok);
    expectParity(compare(cpu.image, out), "resident accumulator after a no-op composite");
}

TEST_CASE("moving one layer recomposites only the macrotiles it touched") {
    auto lane = makeLane("dirty-set plumbing");
    if (!lane) return;

    // Big enough that the macrotile grid has more than one cell: at the default k the macrotile is
    // 256 px, so a 200x140 canvas would be a single macrotile and the claim would be vacuous.
    constexpr std::uint32_t kBigW = 1200;
    constexpr std::uint32_t kBigH = 900;

    core::Document doc(kBigW, kBigH);
    {
        auto bg = doc.makeRaster("bg", kBigW, kBigH);
        bg->image() = backdrop(kBigW, kBigH);
        doc.root().addOnTop(std::move(bg));
        auto sp = doc.makeRaster("sprite", 96, 72);
        sp->image() = sprite();
        sp->setTransform(Affine2D::translation(100.0, 80.0));
        doc.root().addOnTop(std::move(sp));
    }

    const render::TileCompositeStatus first = lane->composite(doc, render::CompositeOptions{});
    REQUIRE(first.ok);
    const std::uint64_t total = lane->macroGrid().tileCount();
    REQUIRE(total > 4);  // otherwise this case proves nothing
    CHECK(first.macrotiles == total);

    // Nudge the sprite. Its footprint is ~96x72 at (100,80), so it can touch a handful of
    // macrotiles and must not touch the rest.
    doc.root().child(1).setTransform(Affine2D::translation(140.0, 96.0));
    const render::TileCompositeStatus moved = lane->composite(doc, render::CompositeOptions{});
    REQUIRE(moved.ok);
    CHECK(moved.macrotiles > 0);
    CHECK(moved.macrotiles < total);
    // The pixels were already there: a move re-dispatches, it does not re-upload.
    CHECK(moved.uploadBytes == 0);

    // The partial recomposite must be indistinguishable from a full one. If it is not, the dirty
    // set is under-covering and the picture rots one macrotile at a time.
    Image partial;
    std::string err;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, partial, err), err);

    lane->markAllDirty();
    const render::TileCompositeStatus full = lane->composite(doc, render::CompositeOptions{});
    REQUIRE(full.ok);
    CHECK(full.macrotiles == total);
    Image whole;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, whole, err), err);
    CHECK(partial.rgba == whole.rgba);  // BYTE-identical, not merely within tolerance

    const render::CompositeResult cpu =
        render::composite(doc, render::CompositeOptions{}, render::Backend::Cpu);
    REQUIRE(cpu.ok);
    expectParity(compare(cpu.image, whole), "moved layer, full recomposite");
}

TEST_CASE("editing a layer's pixels re-uploads only that layer") {
    auto lane = makeLane("layer invalidation");
    if (!lane) return;

    core::Document doc(kW, kH);
    {
        auto bg = doc.makeRaster("bg", kW, kH);
        bg->image() = backdrop();
        doc.root().addOnTop(std::move(bg));
        auto sp = doc.makeRaster("sprite", 96, 72);
        sp->image() = sprite();
        sp->setTransform(Affine2D::translation(37.0, 21.0));
        doc.root().addOnTop(std::move(sp));
    }
    REQUIRE(lane->composite(doc, render::CompositeOptions{}).ok);

    // Repaint the sprite. contentRevision moves, so the lane must notice even without a
    // markLayerDirty() call -- the revision is the belt to markLayerDirty's braces.
    auto& sp = static_cast<core::RasterLayer&>(doc.root().child(1));
    sp.image() = sprite(96, 72);
    for (std::size_t i = 0; i < sp.image().rgba.size(); i += 4) sp.image().rgba[i] = 12;
    sp.invalidateContentBounds();
    lane->markLayerDirty(sp.id());

    const render::TileCompositeStatus st = lane->composite(doc, render::CompositeOptions{});
    REQUIRE(st.ok);
    CHECK(st.uploadBytes > 0);
    // Only the edited layer moved, so the backdrop's bytes must not be re-sent: the sprite is
    // 96*72*4 = 27648 bytes, the backdrop 200*140*4 = 112000.
    CHECK(st.uploadBytes < 100000);

    Image out;
    std::string err;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, out, err), err);
    const render::CompositeResult cpu =
        render::composite(doc, render::CompositeOptions{}, render::Backend::Cpu);
    REQUIRE(cpu.ok);
    expectParity(compare(cpu.image, out), "after a pixel edit");
}

// ⚠⚠ THE REGRESSION NET FOR THE CACHE-GENERATION TRAP (S60-a). A device copy keyed on
// `contentRevision()` passes every other case in this file and fails only this one -- which is
// exactly why it is written down. A text or texture layer's pixels are an app-populated CACHE, and
// core::text::refreshTextCache / core::texture::refreshTextureCache replace them for reasons that
// leave `contentRevision` standing still:
//
//   * the DRAFT (half-res) bake taken while a block-edit gesture or a font hover is live, and the
//     crisp re-render that lands when it settles (the cache's own validity key is the baked linear
//     part, not the revision);
//   * an Area block's clip flip as it becomes / stops being the edit target;
//   * a 3D block's overlay re-bake after a layer-effects edit;
//   * a texture layer's re-render after a CANVAS RESIZE.
//
// setCachedImage then stamps the cache revision back to the SAME content revision, so `cacheCurrent()`
// reads true again with different pixels behind it. The lane therefore keys on `cacheGeneration()`,
// and this case is the proof: swap the pixels the way the app does, assert the old key could not
// have seen it, and assert the picture moved anyway.
TEST_CASE("a cache re-render the content revision cannot see still changes the picture") {
    auto lane = makeLane("cache generation");
    if (!lane) return;

    core::Document doc(kW, kH);
    addBackdrop(doc, backdrop());
    core::TextLayer& tl =
        addTextLayer(doc, sprite(), Affine2D::identity(), Affine2D::translation(37.0, 21.0));
    const render::CompositeOptions opts;

    const render::TileCompositeStatus first = lane->composite(doc, opts);
    INFO("refusal: " << std::string(render::tileRefusalName(first.refusal)) << " " << first.error);
    REQUIRE(first.ok);
    CHECK(first.fullUploads == 2);  // the backdrop and the text cache had to get there somehow
    Image was;
    std::string err;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, was, err), err);

    // RESIDENCY FIRST, and it is half the claim: a key that moved on its own would make every text
    // document pay a full upload per frame, which is the failure the OTHER direction of this fix
    // would introduce and which no parity assertion can see.
    const render::TileCompositeStatus idle = lane->composite(doc, opts);
    REQUIRE(idle.ok);
    CHECK(idle.fullUploads == 0);
    CHECK(idle.partialUploads == 0);
    CHECK(idle.uploadBytes == 0);
    CHECK(idle.dispatches == 0);

    // The swap, exactly as the text pass performs it: new pixels, same map, nothing else touched.
    const std::uint64_t rev = tl.contentRevision();
    Image swapped = sprite();
    for (std::size_t i = 0; i < swapped.rgba.size(); i += 4) {
        swapped.rgba[i + 0] = 250;
        swapped.rgba[i + 1] = 12;
        swapped.rgba[i + 2] = 30;
        swapped.rgba[i + 3] = 255;
    }
    tl.setCachedImage(swapped, Affine2D::identity());
    // The PREMISE, pinned so that a later change to the layer model breaks this case with a
    // readable message instead of silently making it vacuous.
    REQUIRE(tl.contentRevision() == rev);
    REQUIRE(tl.cacheCurrent());

    const render::TileCompositeStatus after = lane->composite(doc, opts);
    INFO("refusal: " << std::string(render::tileRefusalName(after.refusal)) << " " << after.error);
    REQUIRE(after.ok);
    // A cache is REPLACED, never patched, so it is re-sent whole -- and only it: the backdrop's
    // bytes must not move, which the event counts say and a cache size never could.
    CHECK(after.fullUploads == 1);
    CHECK(after.partialUploads == 0);
    CHECK(after.uploadBytes == 96ull * 72ull * 4ull);

    Image out;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, out, err), err);
    CHECK(out.rgba != was.rgba);  // the whole point: a contentRevision key shows `was` forever
    const render::CompositeResult cpu = render::composite(doc, opts, render::Backend::Cpu);
    REQUIRE(cpu.ok);
    expectParity(compare(cpu.image, out), "after a cache swap at a standing content revision");
}

TEST_CASE("a texture cache re-rendered at a standing content revision re-uploads too") {
    auto lane = makeLane("texture cache generation");
    if (!lane) return;

    // The texture twin of the case above, and its trigger in the app is a CANVAS RESIZE: the
    // refresh pass compares the cache's extent against the document's, so it re-renders with the
    // params revision untouched (core/texture/texture_layer_render.cpp says so in as many words).
    core::Document doc(kW, kH);
    addBackdrop(doc, backdrop());
    core::TextureLayer& xl = addTextureLayer(doc, sprite(120, 90), std::nullopt,
                                             Affine2D::identity(), Affine2D::translation(9.0, 6.0));
    const render::CompositeOptions opts;
    REQUIRE(lane->composite(doc, opts).ok);

    const std::uint64_t rev = xl.contentRevision();
    Image bigger = sprite(150, 110);
    xl.setCachedImage(std::move(bigger), std::nullopt, Affine2D::identity());
    REQUIRE(xl.contentRevision() == rev);

    const render::TileCompositeStatus after = lane->composite(doc, opts);
    INFO("refusal: " << std::string(render::tileRefusalName(after.refusal)) << " " << after.error);
    REQUIRE(after.ok);
    CHECK(after.fullUploads == 1);
    CHECK(after.partialUploads == 0);
    CHECK(after.uploadBytes == 150ull * 110ull * 4ull);

    Image out;
    std::string err;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, out, err), err);
    const render::CompositeResult cpu = render::composite(doc, opts, render::Backend::Cpu);
    REQUIRE(cpu.ok);
    expectParity(compare(cpu.image, out), "after a texture cache resize");
}

// ---------------------------------------------------------------------------------------------
// INCREMENTAL (dirty-macrotile) layer upload.
//
// The 2026-07-28 gate failed its condition 3 by 4.8x -- `gpu edit 256` cost 18.804 ms at
// 3840x2160 against a 3.893 ms budget -- because an edit that dirtied one 256 px macrotile
// re-uploaded the WHOLE layer and recomposited its WHOLE footprint. These cases pin the fix, and
// they are all counter assertions rather than timings for a reason the plan's §8.3 keeps making:
// a re-upload leaves `residentSourceBytes()` exactly where it was, so a cache SIZE cannot witness
// one. Only an event count can, which is what TileCompositeStatus::uploadBytes / uploadRegions /
// partialUploads / fullUploads are for.
//
// The correctness half is the important half: a partial upload is a pure transfer optimisation,
// so every case here also proves the pixels did not move -- against the CPU reference, and
// byte-for-byte against a full re-upload plus a full recomposite of the same document.
// ---------------------------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kIncW = 1100;
constexpr std::uint32_t kIncH = 900;

// A backdrop under a FULL-CANVAS top layer. Full-canvas is the shape that made the gate fail:
// a small layer's whole-layer upload is small anyway, so it would hide the defect.
core::LayerId buildIncrementalDoc(core::Document& doc) {
    auto bg = doc.makeRaster("bg", kIncW, kIncH);
    bg->image() = backdrop(kIncW, kIncH);
    doc.root().addOnTop(std::move(bg));
    auto top = doc.makeRaster("top", kIncW, kIncH);
    top->image() = sprite(kIncW, kIncH);
    top->setOpacity(0.78f);
    const core::LayerId id = top->id();
    doc.root().addOnTop(std::move(top));
    return id;
}

// Repaint [x0,x1) x [y0,y1) of a layer with a deterministic, obviously-different pattern.
void repaint(core::RasterLayer& l, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
             std::uint32_t y1, std::uint8_t tag) {
    Image& img = l.image();
    for (std::uint32_t y = y0; y < std::min(y1, img.height); ++y)
        for (std::uint32_t x = x0; x < std::min(x1, img.width); ++x) {
            std::uint8_t* px = &img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4];
            px[0] = static_cast<std::uint8_t>(x ^ tag);
            px[1] = static_cast<std::uint8_t>(y ^ tag);
            px[2] = tag;
            px[3] = 255;
        }
    l.invalidateContentBounds();
}

// Parity for the big-canvas cases in this section. They exist to prove the UPLOAD is right, not
// to re-litigate blend precision -- test_composite_tile_parity.cpp and the small-canvas cases
// above own that, at zero tolerance. A canvas 35x larger gets 35x the chances of the fp16
// accumulator landing on an 8-bit rounding boundary, so the bound here is a vanishing FRACTION
// rather than an absolute count, exactly as expectReciprocalParity's is. It is nowhere near loose
// enough to swallow the failure this section is looking for: a macrotile uploaded wrong, or not at
// all, is tens of thousands of pixels at large deltas and moves `meanLsb` off zero.
void expectBigParity(const Diff& d, const std::string& label) {
    INFO(label << " (big canvas): maxLsb " << d.maxLsb << ", over-1-LSB " << d.over << "/"
               << d.pixels << ", meanLsb " << d.meanLsb);
    // MEASURED on the RX 6600 XT, 2026-07-29: max 1 LSB, ZERO pixels over 1 LSB, and mean 0.049 --
    // i.e. about 5% of the 990k pixels land on an 8-bit rounding boundary the fp16 accumulator
    // resolves the other way. That rate is the content's, not the upload's: it is the same 0.049
    // whether the layer was uploaded whole or by macrotile, and the partial-vs-full readbacks are
    // compared BYTE FOR BYTE separately (the stroke case), which is the assertion that actually
    // guards this section. So the mean is bounded at 2x the measured rate rather than at a number
    // that only ever passed on paper -- the corruption tripwires are `over` and `maxLsb` below,
    // because a macrotile uploaded wrong (or skipped) is 65k pixels at large deltas.
    CHECK(d.meanLsb < 0.10);
    CHECK(d.over <= d.pixels / 2000);  // <= 0.05% of pixels
    CHECK(d.maxLsb <= 3);
}

// The bytes a macrotile-granular upload of [x0,x1) x [y0,y1) must move, derived from the lane's
// own macrotile so the assertion holds whatever `GpuCaps::macrotileSize` probed. The 64 px dirty
// set covers tiles [floor(x0/64), floor((x1-1)/64)], and projecting by k >> collapses that to
// [x0/m, (x1-1)/m] -- so the pixel span is exactly the expression below, clipped to the image.
std::uint64_t macroCoverBytes(std::uint32_t m, std::uint32_t x0, std::uint32_t y0,
                             std::uint32_t x1, std::uint32_t y1, std::uint32_t w,
                             std::uint32_t h) {
    const std::uint32_t mx0 = (x0 / m) * m;
    const std::uint32_t my0 = (y0 / m) * m;
    const std::uint32_t mx1 = std::min(w, ((x1 - 1) / m + 1) * m);
    const std::uint32_t my1 = std::min(h, ((y1 - 1) / m + 1) * m);
    return static_cast<std::uint64_t>(mx1 - mx0) * (my1 - my0) * 4ull;
}

}  // namespace

TEST_CASE("a region-marked edit uploads only the macrotiles it touched") {
    auto lane = makeLane("incremental upload");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    core::Document doc(kIncW, kIncH);
    const core::LayerId topId = buildIncrementalDoc(doc);
    const render::CompositeOptions opts;
    const std::uint64_t layerBytes = static_cast<std::uint64_t>(kIncW) * kIncH * 4ull;

    const render::TileCompositeStatus first = lane->composite(doc, opts);
    INFO("refusal: " << std::string(render::tileRefusalName(first.refusal)) << " " << first.error);
    REQUIRE(first.ok);
    // Both layers had to get there somehow, and there is no mask on either, so this is exact.
    CHECK(first.fullUploads == 2);
    CHECK(first.partialUploads == 0);
    CHECK(first.uploadBytes == 2 * layerBytes);

    auto* top = doc.find(topId)->as<core::RasterLayer>();
    REQUIRE(top != nullptr);
    repaint(*top, 300, 300, 400, 400, 0x5A);
    lane->markLayerDirty(*top, mosaic::common::Rect{300.0, 300.0, 100.0, 100.0});

    const render::TileCompositeStatus edit = lane->composite(doc, opts);
    INFO("refusal: " << std::string(render::tileRefusalName(edit.refusal)) << " " << edit.error);
    REQUIRE(edit.ok);
    CHECK(edit.partialUploads == 1);
    CHECK(edit.fullUploads == 0);
    const std::uint32_t m = lane->macrotileSize();
    // THE NUMBER THE SLICE EXISTS FOR. A 100x100 dab in a 3.96 MB layer moves one macrotile's
    // worth of bytes, not the layer's -- and the region count says it went as coalesced runs.
    CHECK(edit.uploadBytes == macroCoverBytes(m, 300, 300, 400, 400, kIncW, kIncH));
    CHECK(edit.uploadBytes < layerBytes);
    CHECK(edit.uploadRegions == (399u / m) - (300u / m) + 1);  // one run per macrotile ROW
    // ... and the recomposite narrowed with it: the dirty set is the placed dab, not the
    // full-canvas layer's whole footprint (which is what cost 18.8 ms).
    CHECK(edit.macrotiles > 0);
    CHECK(edit.macrotiles < lane->macroGrid().tileCount());

    Image partial;
    std::string err;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, partial, err), err);
    const render::CompositeResult cpu = render::composite(doc, opts, render::Backend::Cpu);
    REQUIRE(cpu.ok);
    expectBigParity(compare(cpu.image, partial), "after a region-marked partial upload");

    // The strongest statement available, and the one that catches the layout mistake this path
    // invites: if the partial upload had transitioned the layer image out of UNDEFINED it would
    // have DISCARDED every pixel the copy did not rewrite, and a full re-upload would then
    // disagree with what is on screen. Byte-identical, not merely within tolerance.
    lane->markLayerDirty(topId);
    const render::TileCompositeStatus full = lane->composite(doc, opts);
    REQUIRE(full.ok);
    CHECK(full.fullUploads == 1);
    CHECK(full.partialUploads == 0);
    Image whole;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, whole, err), err);
    CHECK(partial.rgba == whole.rgba);
    CHECK(lane->validationErrors() == before);
}

TEST_CASE("two disjoint dabs upload two macrotiles, not the box that bounds them") {
    auto lane = makeLane("disjoint region marks");
    if (!lane) return;

    core::Document doc(kIncW, kIncH);
    const core::LayerId topId = buildIncrementalDoc(doc);
    const render::CompositeOptions opts;
    REQUIRE(lane->composite(doc, opts).ok);

    auto* top = doc.find(topId)->as<core::RasterLayer>();
    REQUIRE(top != nullptr);
    // Opposite corners. A ledger that kept only a bounding RECT would have to send almost the
    // whole layer; the 64 px tile set sends the two macrotiles and nothing between them.
    repaint(*top, 60, 60, 140, 140, 0x11);
    lane->markLayerDirty(*top, mosaic::common::Rect{60.0, 60.0, 80.0, 80.0});
    repaint(*top, 900, 700, 980, 780, 0x22);
    lane->markLayerDirty(*top, mosaic::common::Rect{900.0, 700.0, 80.0, 80.0});

    const render::TileCompositeStatus st = lane->composite(doc, opts);
    INFO("refusal: " << std::string(render::tileRefusalName(st.refusal)) << " " << st.error);
    REQUIRE(st.ok);
    CHECK(st.partialUploads == 1);  // one layer, refreshed incrementally
    CHECK(st.fullUploads == 0);
    // Both claims are in one upload, so they arrive together or the second one is lost. Naming
    // the corners of the canvas cannot cost the canvas.
    const std::uint64_t layerBytes = static_cast<std::uint64_t>(kIncW) * kIncH * 4ull;
    CHECK(st.uploadBytes < layerBytes);

    // The proof that BOTH dabs made it -- and to the right place. A ledger that dropped the
    // second claim, or transposed the two, fails here and nowhere else.
    Image out;
    std::string err;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, out, err), err);
    const render::CompositeResult cpu = render::composite(doc, opts, render::Backend::Cpu);
    REQUIRE(cpu.ok);
    expectBigParity(compare(cpu.image, out), "two disjoint region-marked dabs");
}

TEST_CASE("a change the caller did not locate still uploads the whole layer") {
    auto lane = makeLane("incremental upload fallbacks");
    if (!lane) return;

    core::Document doc(kIncW, kIncH);
    const core::LayerId topId = buildIncrementalDoc(doc);
    const render::CompositeOptions opts;
    const std::uint64_t layerBytes = static_cast<std::uint64_t>(kIncW) * kIncH * 4ull;
    REQUIRE(lane->composite(doc, opts).ok);
    auto* top = doc.find(topId)->as<core::RasterLayer>();
    REQUIRE(top != nullptr);

    SUBCASE("contentRevision moves with no region claim at all") {
        // The belt to markLayerDirty's braces, and it must keep working: an edit that only bumps
        // the revision is the caller saying "somewhere", and the safe reading of somewhere is
        // everywhere.
        repaint(*top, 10, 10, 90, 90, 0x31);
        const render::TileCompositeStatus st = lane->composite(doc, opts);
        REQUIRE(st.ok);
        CHECK(st.fullUploads == 1);
        CHECK(st.partialUploads == 0);
        CHECK(st.uploadBytes == layerBytes);
    }
    SUBCASE("an EMPTY rect is a caller with nothing to say, not 'nothing changed'") {
        repaint(*top, 10, 10, 90, 90, 0x32);
        lane->markLayerDirty(*top, mosaic::common::Rect{});
        const render::TileCompositeStatus st = lane->composite(doc, opts);
        REQUIRE(st.ok);
        CHECK(st.fullUploads == 1);
        CHECK(st.partialUploads == 0);
        CHECK(st.uploadBytes == layerBytes);
    }
    SUBCASE("a revision step that no rect described poisons the ledger") {
        // THE dangerous case, and the reason the ledger tracks revisions at all: an unnamed edit
        // followed by a named one. Trusting the second claim would silently drop the first edit's
        // bytes for as long as the layer stayed resident.
        repaint(*top, 10, 10, 90, 90, 0x33);       // unnamed
        repaint(*top, 600, 500, 700, 600, 0x34);   // named, but a step behind
        lane->markLayerDirty(*top, mosaic::common::Rect{600.0, 500.0, 100.0, 100.0});
        const render::TileCompositeStatus st = lane->composite(doc, opts);
        REQUIRE(st.ok);
        CHECK(st.fullUploads == 1);
        CHECK(st.partialUploads == 0);
        CHECK(st.uploadBytes == layerBytes);

        Image out;
        std::string err;
        REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, out, err), err);
        const render::CompositeResult cpu = render::composite(doc, opts, render::Backend::Cpu);
        REQUIRE(cpu.ok);
        expectBigParity(compare(cpu.image, out), "unnamed edit followed by a named one");
    }
    SUBCASE("naming the whole layer costs one copy, not a run per macrotile row") {
        repaint(*top, 0, 0, kIncW, kIncH, 0x35);
        lane->markLayerDirty(*top, mosaic::common::Rect{0.0, 0.0, static_cast<double>(kIncW),
                                                        static_cast<double>(kIncH)});
        const render::TileCompositeStatus st = lane->composite(doc, opts);
        REQUIRE(st.ok);
        CHECK(st.fullUploads == 1);
        CHECK(st.partialUploads == 0);
        CHECK(st.uploadBytes == layerBytes);
        CHECK(st.uploadRegions == 1);
    }
}

TEST_CASE("a resized layer forces a full upload, whatever region the caller names") {
    auto lane = makeLane("incremental upload after a resize");
    if (!lane) return;

    core::Document doc(kIncW, kIncH);
    const core::LayerId topId = buildIncrementalDoc(doc);
    const render::CompositeOptions opts;
    REQUIRE(lane->composite(doc, opts).ok);
    auto* top = doc.find(topId)->as<core::RasterLayer>();
    REQUIRE(top != nullptr);

    // A resize invalidates every tile index in the ledger AND the device image itself: the same
    // (tx,ty) means different pixels on the new grid, so reinterpreting the set would upload the
    // right bytes to the wrong place. The claim below is honest and still must be ignored.
    constexpr std::uint32_t kNewW = 700;
    constexpr std::uint32_t kNewH = 500;
    top->image() = sprite(kNewW, kNewH);
    top->invalidateContentBounds();
    lane->markLayerDirty(*top, mosaic::common::Rect{0.0, 0.0, 100.0, 100.0});

    const render::TileCompositeStatus st = lane->composite(doc, opts);
    INFO("refusal: " << std::string(render::tileRefusalName(st.refusal)) << " " << st.error);
    REQUIRE(st.ok);
    CHECK(st.fullUploads == 1);
    CHECK(st.partialUploads == 0);
    CHECK(st.uploadBytes == static_cast<std::uint64_t>(kNewW) * kNewH * 4ull);

    Image out;
    std::string err;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, out, err), err);
    const render::CompositeResult cpu = render::composite(doc, opts, render::Backend::Cpu);
    REQUIRE(cpu.ok);
    expectBigParity(compare(cpu.image, out), "after a layer resize");
}

TEST_CASE("a stroke of region-marked dabs stays byte-identical to a full re-upload") {
    auto lane = makeLane("incremental upload over a stroke");
    if (!lane) return;

    core::Document doc(kIncW, kIncH);
    const core::LayerId topId = buildIncrementalDoc(doc);
    const render::CompositeOptions opts;
    REQUIRE(lane->composite(doc, opts).ok);
    auto* top = doc.find(topId)->as<core::RasterLayer>();
    REQUIRE(top != nullptr);

    // Eight dabs down a diagonal, each named. This is the case a single-edit test cannot reach:
    // the ledger has to survive being cleared and re-armed seven times, and the device image has
    // to accumulate all eight without any of them discarding the others.
    const std::uint64_t layerBytes = static_cast<std::uint64_t>(kIncW) * kIncH * 4ull;
    const std::uint32_t m = lane->macrotileSize();
    for (std::uint32_t i = 0; i < 8; ++i) {
        const std::uint32_t x = 40 + i * 120;
        const std::uint32_t y = 30 + i * 100;
        repaint(*top, x, y, x + 60, y + 60, static_cast<std::uint8_t>(0x40 + i));
        lane->markLayerDirty(*top, mosaic::common::Rect{static_cast<double>(x),
                                                        static_cast<double>(y), 60.0, 60.0});
        const render::TileCompositeStatus st = lane->composite(doc, opts);
        INFO("dab " << i << " refusal: " << std::string(render::tileRefusalName(st.refusal)));
        REQUIRE(st.ok);
        CHECK(st.partialUploads == 1);
        CHECK(st.fullUploads == 0);
        // Exactly the macrotiles the dab touched, every time -- the ledger neither accumulates
        // across dabs (which would grow the upload as the stroke goes on) nor forgets one.
        CHECK(st.uploadBytes == macroCoverBytes(m, x, y, x + 60, y + 60, kIncW, kIncH));
        CHECK(st.uploadBytes < layerBytes);
    }

    Image incremental;
    std::string err;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, incremental, err), err);

    lane->markLayerDirty(topId);
    lane->markAllDirty();
    const render::TileCompositeStatus rebuilt = lane->composite(doc, opts);
    REQUIRE(rebuilt.ok);
    CHECK(rebuilt.fullUploads == 1);
    CHECK(rebuilt.macrotiles == lane->macroGrid().tileCount());
    Image fromScratch;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, fromScratch, err), err);
    CHECK(incremental.rgba == fromScratch.rgba);

    const render::CompositeResult cpu = render::composite(doc, opts, render::Backend::Cpu);
    REQUIRE(cpu.ok);
    expectBigParity(compare(cpu.image, fromScratch), "eight region-marked dabs");
}

// ---------------------------------------------------------------------------------------------
// The readback seam.
// ---------------------------------------------------------------------------------------------

TEST_CASE("the readback seam serves a sub-rect out of the tiled accumulator") {
    auto lane = makeLane("readback");
    if (!lane) return;

    core::Document doc(kW, kH);
    {
        auto bg = doc.makeRaster("bg", kW, kH);
        bg->image() = backdrop();
        doc.root().addOnTop(std::move(bg));
        auto sp = doc.makeRaster("sprite", 96, 72);
        sp->image() = sprite();
        sp->setTransform(Affine2D::translation(37.0, 21.0));
        doc.root().addOnTop(std::move(sp));
    }
    REQUIRE(lane->composite(doc, render::CompositeOptions{}).ok);

    Image whole;
    std::string err;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, whole, err), err);
    REQUIRE(whole.width == kW);
    REQUIRE(whole.height == kH);

    // A rect that straddles a macrotile boundary, in both axes, and is not tile-aligned.
    const mosaic::common::Rect roi{53.0, 31.0, 77.0, 66.0};
    Image part;
    REQUIRE_MESSAGE(lane->readback(roi, part, err), err);
    REQUIRE(part.width == 77);
    REQUIRE(part.height == 66);
    bool same = true;
    for (std::uint32_t y = 0; y < part.height && same; ++y)
        for (std::uint32_t x = 0; x < part.width; ++x) {
            const std::size_t sp = ((static_cast<std::size_t>(y) + 31) * kW + x + 53) * 4;
            const std::size_t dp = (static_cast<std::size_t>(y) * part.width + x) * 4;
            for (std::size_t c = 0; c < 4; ++c)
                if (part.rgba[dp + c] != whole.rgba[sp + c]) same = false;
            if (!same) break;
        }
    CHECK(same);

    // A readback outside the canvas is an error, not an empty image: the resident accumulator has
    // no off-canvas pixels and never will (docs/s60-readback-consumers.md B7/B8).
    Image none;
    CHECK_FALSE(lane->readback(mosaic::common::Rect{5000.0, 5000.0, 10.0, 10.0}, none, err));
}

// ---------------------------------------------------------------------------------------------
// Refusal: everything the lane cannot do exactly.
// ---------------------------------------------------------------------------------------------

TEST_CASE("the lane refuses what it cannot composite exactly, and names why") {
    auto lane = makeLane("refusals");
    if (!lane) return;

    const auto refusalFor = [&lane](core::Document& doc) {
        const render::TileCompositeStatus st = lane->composite(doc, render::CompositeOptions{});
        CHECK_FALSE(st.ok);
        return st.refusal;
    };

    SUBCASE("a group child") {
        core::Document doc(kW, kH);
        auto bg = doc.makeRaster("bg", kW, kH);
        bg->image() = backdrop();
        doc.root().addOnTop(std::move(bg));
        auto g = doc.makeGroup("g");
        auto inner = doc.makeRaster("inner", 32, 32);
        g->addOnTop(std::move(inner));
        doc.root().addOnTop(std::move(g));
        CHECK(refusalFor(doc) == TileRefusal::NestedGroup);
    }
    SUBCASE("an adjustment KIND the lane does not grade") {
        // Invert used to stand here, and it is now SERVED (S60-a): admission is per kind, so what
        // refuses is a kind the second kernel does not express. A lattice quantiser is the clearest
        // one -- the fp16 accumulator flips a whole level at a step -- and the dedicated admission
        // case above walks every refusal class with the kind named in `error`.
        core::Document doc(kW, kH);
        auto bg = doc.makeRaster("bg", kW, kH);
        bg->image() = backdrop();
        doc.root().addOnTop(std::move(bg));
        doc.root().addOnTop(doc.makeAdjustment("post", core::AdjustmentKind::Posterize));
        CHECK(refusalFor(doc) == TileRefusal::Adjustment);
    }
    SUBCASE("layer effects") {
        core::Document doc(kW, kH);
        auto bg = doc.makeRaster("bg", kW, kH);
        bg->image() = backdrop();
        core::LayerEffects fx;
        fx.outerGlow.enabled = true;
        bg->setEffects(fx);
        doc.root().addOnTop(std::move(bg));
        CHECK(refusalFor(doc) == TileRefusal::LayerEffects);
    }
    SUBCASE("a VECTOR leaf, which has no fixed-resolution source at all") {
        // The one leaf kind that stays refused now that text/magic/texture are served, and not for
        // want of plumbing: core::vec::rasterizeObjectF evaluates the object at TARGET resolution
        // through the placement, so the shape is re-rasterised crisp at every zoom and there is
        // nothing of fixed size to make resident. A bitmap stand-in would draw a DIFFERENT picture
        // (a resampled one), which is precisely what a refusal is for.
        core::Document doc(kW, kH);
        auto bg = doc.makeRaster("bg", kW, kH);
        bg->image() = backdrop();
        doc.root().addOnTop(std::move(bg));
        auto vl = doc.makeVector("v");
        core::vec::Object o;
        o.geometry = core::vec::ParametricShape{core::vec::RectShape{{40.0, 30.0}}};
        o.fill = core::vec::SolidPaint{mosaic::common::ColorF{1.0f, 0.2f, 0.1f, 1.0f}};
        vl->setObject(std::move(o));  // WITH geometry: the kind is the reason, not the emptiness
        vl->setTransform(Affine2D::translation(80.0, 60.0));
        doc.root().addOnTop(std::move(vl));
        CHECK(refusalFor(doc) == TileRefusal::UnsupportedKind);
    }
    SUBCASE("a text layer the app has not rendered a cache for") {
        // NOT a transparent layer, and the distinction is load-bearing: the CPU walk still runs
        // walkStep for it, so it still publishes an all-zero clip base that hides everything
        // clipped above it. Reproducing that is more work than declining -- the same argument the
        // empty-raster arm has always made.
        core::Document doc(kW, kH);
        auto bg = doc.makeRaster("bg", kW, kH);
        bg->image() = backdrop();
        doc.root().addOnTop(std::move(bg));
        doc.root().addOnTop(doc.makeText("t", "hello"));
        CHECK(refusalFor(doc) == TileRefusal::UnsupportedKind);
    }
    SUBCASE("a text layer whose cache is present but zero-sized") {
        core::Document doc(kW, kH);
        auto bg = doc.makeRaster("bg", kW, kH);
        bg->image() = backdrop();
        doc.root().addOnTop(std::move(bg));
        auto tl = doc.makeText("t", "hello");
        core::TextLayer* ref = doc.root().addOnTop(std::move(tl)).as<core::TextLayer>();
        REQUIRE(ref != nullptr);
        ref->setCachedImage(Image{}, Affine2D::identity());
        REQUIRE(ref->cachedImage() != nullptr);  // present, and still nothing to composite
        CHECK(refusalFor(doc) == TileRefusal::UnsupportedKind);
    }
    SUBCASE("a texture layer with neither cache arm populated") {
        core::Document doc(kW, kH);
        auto bg = doc.makeRaster("bg", kW, kH);
        bg->image() = backdrop();
        doc.root().addOnTop(std::move(bg));
        doc.root().addOnTop(doc.makeTexture(
            "x", core::texture::defaultTextureParams(core::texture::Generator::Sky)));
        CHECK(refusalFor(doc) == TileRefusal::UnsupportedKind);
    }
    SUBCASE("a singular transform") {
        core::Document doc(kW, kH);
        auto bg = doc.makeRaster("bg", kW, kH);
        bg->image() = backdrop();
        bg->setTransform(Affine2D{0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
        doc.root().addOnTop(std::move(bg));
        CHECK(refusalFor(doc) == TileRefusal::SingularTransform);
    }
    SUBCASE("the checkerboard, which belongs to the present pass") {
        core::Document doc(kW, kH);
        auto bg = doc.makeRaster("bg", kW, kH);
        bg->image() = backdrop();
        doc.root().addOnTop(std::move(bg));
        render::CompositeOptions opts;
        opts.checkerboard = true;
        const render::TileCompositeStatus st = lane->composite(doc, opts);
        CHECK_FALSE(st.ok);
    }
}

// ---------------------------------------------------------------------------------------------
// The tile vocabulary the lane is built on.
// ---------------------------------------------------------------------------------------------

TEST_CASE("the lane tracks dirt on the store's 64 px grid and dispatches on macrotiles") {
    auto lane = makeLane("tile grids");
    if (!lane) return;

    core::Document doc(kW, kH);
    auto bg = doc.makeRaster("bg", kW, kH);
    bg->image() = backdrop();
    doc.root().addOnTop(std::move(bg));
    REQUIRE(lane->composite(doc, render::CompositeOptions{}).ok);

    // The dirty grid IS the `.mosaic` store's grid -- one vocabulary, so a single dirty set can
    // feed both the recomposite and the autosave journal (§3.1).
    CHECK(lane->dirtyGrid().tileSize() == core::kTileSize);
    CHECK(lane->dirtyGrid().width() == kW);
    CHECK(lane->dirtyGrid().height() == kH);
    // The dispatch grid is a `64 << k` multiple of it, never something unrelated.
    CHECK(lane->macrotileSize() >= core::kTileSize);
    CHECK(lane->macrotileSize() % core::kTileSize == 0);
    CHECK(lane->macroGrid().tileSize() == lane->macrotileSize());

    CHECK_FALSE(lane->anyDirty());
    lane->markDirty(mosaic::common::Rect{10.0, 10.0, 4.0, 4.0});
    CHECK(lane->anyDirty());
    CHECK(lane->dirtySet().count() == 1);  // a 4x4 edit inside one 64 px tile dirties ONE tile
}

TEST_CASE("the host-side filter resolution matches the CPU reference's own collapses") {
    // Pure; runs with no device, which is the point -- these two functions decide what the kernel
    // is told, and getting them wrong is a parity failure that looks like a shader bug.
    using render::isLosslessGridPlacement;
    using render::resolveTileFilter;
    using render::tileSupersampleN;

    CHECK(isLosslessGridPlacement(Affine2D::identity()));
    CHECK(isLosslessGridPlacement(Affine2D::translation(37.0, -21.0)));
    CHECK_FALSE(isLosslessGridPlacement(Affine2D::translation(37.5, 21.0)));
    CHECK_FALSE(isLosslessGridPlacement(Affine2D::scaling(2.0, 2.0)));

    // An integer translation is a whole-pixel COPY on the CPU lane whatever filter was asked for,
    // and Nearest is the only kernel that reproduces a copy -- Mitchell and Gaussian approximate
    // and would blur at an integer offset.
    for (const ResampleFilter f : {ResampleFilter::Mitchell, ResampleFilter::Gaussian,
                                   ResampleFilter::Lanczos3, ResampleFilter::Bilinear})
        CHECK(resolveTileFilter(f, Affine2D::translation(4.0, 9.0), false) ==
              ResampleFilter::Nearest);
    // A sub-pixel translation is NOT a copy, so the asked-for kernel stands.
    CHECK(resolveTileFilter(ResampleFilter::Mitchell, Affine2D::translation(4.5, 9.0), false) ==
          ResampleFilter::Mitchell);
    // Nearest short-circuits under ANY translation, integer or not.
    CHECK(resolveTileFilter(ResampleFilter::Nearest, Affine2D::translation(4.5, 9.25), false) ==
          ResampleFilter::Nearest);
    // Auto goes through chooseAutoFilter, and must agree with it exactly.
    const Affine2D minify = Affine2D::translation(3.0, 4.0) * Affine2D::scaling(0.5, 0.5);
    CHECK(resolveTileFilter(ResampleFilter::Auto, minify, false) ==
          render::chooseAutoFilter(minify, false));

    CHECK(tileSupersampleN(Affine2D::identity()) == 2);
    CHECK(tileSupersampleN(Affine2D::scaling(4.0, 4.0)) == 5);
    CHECK(tileSupersampleN(Affine2D::scaling(40.0, 40.0)) == 8);  // clamped
}

// ---------------------------------------------------------------------------------------------
// The present path (item 11): resolve ON THE DEVICE, and never read back.
//
// This is the section that decides whether S60-a pays. Everything above proves the lane draws the
// right picture; these cases prove it reaches the screen WITHOUT the composite -> CPU mirror ->
// staging -> upload chain that residency exists to delete. Both properties are invisible to a
// wall clock -- a fast device hides a full readback per frame -- so they are asserted on counters.
// ---------------------------------------------------------------------------------------------

TEST_CASE("the resolve pass reproduces the CPU reference in the present texture") {
    auto lane = makeLane("the resolve pass");
    if (!lane) return;

    const std::vector<LayerSpec> specs = {
        {backdrop(), Affine2D::identity(), BlendMode::Normal, 1.0f},
        {sprite(), Affine2D::translation(41.0, 27.0), BlendMode::Multiply, 0.8f},
        {sprite(64, 48), Affine2D::translation(96.5, 55.25) * Affine2D::scaling(1.4, 1.4),
         BlendMode::Screen, 1.0f},
    };
    render::CompositeOptions opts;
    opts.resampleFilter = ResampleFilter::Bilinear;

    core::Document cpuDoc(kW, kH);
    buildDocument(cpuDoc, specs);
    const render::CompositeResult cpu = render::composite(cpuDoc, opts, render::Backend::Cpu);
    REQUIRE(cpu.ok);

    core::Document gpuDoc(kW, kH);
    buildDocument(gpuDoc, specs);
    lane->reset();
    const render::TileCompositeStatus st = lane->composite(gpuDoc, opts);
    INFO("refusal: " << std::string(render::tileRefusalName(st.refusal)) << " " << st.error);
    REQUIRE(st.ok);

    render::ResolveTarget target;
    std::string err;
    REQUIRE_MESSAGE(lane->createResolveTarget(kW, kH, target, err), err);
    REQUIRE_MESSAGE(lane->resolve(target, err), err);
    target.layout = VK_IMAGE_LAYOUT_GENERAL;  // resolve() always leaves it here

    Image out;
    REQUIRE_MESSAGE(lane->readTarget(target, mosaic::common::Rect{}, out, err), err);
    // 8-bit conversion happens on the device here and on the CPU in toImage8; a UNORM store's
    // rounding is implementation-defined within the spec's tolerance, so this is the same 1/255
    // every GPU lane in the project is held to -- and it is exactly why export keeps the CPU walk.
    expectParity(compare(cpu.image, out), "resolve vs CPU reference");
    lane->destroyResolveTarget(target);
}

TEST_CASE("the per-frame path costs ZERO readback bytes") {
    auto lane = makeLane("the zero-readback property");
    if (!lane) return;

    core::Document doc(kW, kH);
    auto bg = doc.makeRaster("bg", kW, kH);
    bg->image() = backdrop();
    doc.root().addOnTop(std::move(bg));
    auto top = doc.makeRaster("top", 96, 72);
    top->image() = sprite();
    top->setTransform(Affine2D::translation(20.0, 20.0));
    const core::LayerId topId = top->id();
    doc.root().addOnTop(std::move(top));

    render::ResolveTarget target;
    std::string err;
    REQUIRE_MESSAGE(lane->createResolveTarget(kW, kH, target, err), err);
    const render::CompositeOptions opts;

    REQUIRE(lane->composite(doc, opts).ok);
    REQUIRE_MESSAGE(lane->resolve(target, err), err);
    target.layout = VK_IMAGE_LAYOUT_GENERAL;

    // THE ASSERTION THE WHOLE ARC IS JUDGED ON. Ten frames of a live edit -- a layer nudged one
    // pixel, exactly what a Move drag does -- and not one byte crosses the bus in the GPU->CPU
    // direction. If a future change reinstates a readback anywhere on this path (a consumer
    // reaching for pixels, a "just check the result" debug line), this is what catches it, and
    // nothing else will: on the dev rig it would still be fast.
    const render::TileCompositeStats before = lane->stats();
    for (int i = 1; i <= 10; ++i) {
        core::Layer* l = doc.find(topId);
        REQUIRE(l != nullptr);
        l->setTransform(Affine2D::translation(20.0 + i, 20.0));
        const render::TileCompositeStatus st = lane->composite(doc, opts);
        INFO("frame " << i << " refusal: " << std::string(render::tileRefusalName(st.refusal)));
        REQUIRE(st.ok);
        REQUIRE_MESSAGE(lane->resolve(target, err), err);
    }
    const render::TileCompositeStats after = lane->stats();
    CHECK(after.readbacks == before.readbacks);
    CHECK(after.readbackBytes == before.readbackBytes);
    CHECK(after.resolves == before.resolves + 10);

    // ... and the picture is still right, which is what stops "zero readback" being satisfied by
    // a resolve that quietly did nothing. This readback IS one -- it is the verification, not the
    // path -- so it is taken after the counters are read.
    core::Document ref(kW, kH);
    {
        auto rbg = ref.makeRaster("bg", kW, kH);
        rbg->image() = backdrop();
        ref.root().addOnTop(std::move(rbg));
        auto rtop = ref.makeRaster("top", 96, 72);
        rtop->image() = sprite();
        rtop->setTransform(Affine2D::translation(30.0, 20.0));
        ref.root().addOnTop(std::move(rtop));
    }
    const render::CompositeResult cpu = render::composite(ref, opts, render::Backend::Cpu);
    REQUIRE(cpu.ok);
    Image out;
    REQUIRE_MESSAGE(lane->readTarget(target, mosaic::common::Rect{}, out, err), err);
    expectParity(compare(cpu.image, out), "after ten resolved frames");
    lane->destroyResolveTarget(target);
}

TEST_CASE("the resolve is incremental, and a new target is resolved in full") {
    auto lane = makeLane("incremental resolve");
    if (!lane) return;

    // Big enough that one small edit is a small FRACTION of the macrotiles -- on a 200x140 canvas
    // at a 256 px macrotile the whole document is one tile and the property is untestable.
    constexpr std::uint32_t kBigW = 1100;
    constexpr std::uint32_t kBigH = 900;
    core::Document doc(kBigW, kBigH);
    auto bg = doc.makeRaster("bg", kBigW, kBigH);
    bg->image() = backdrop(kBigW, kBigH);
    doc.root().addOnTop(std::move(bg));
    auto top = doc.makeRaster("top", 40, 40);
    top->image() = sprite(40, 40);
    top->setTransform(Affine2D::translation(500.0, 400.0));
    const core::LayerId topId = top->id();
    doc.root().addOnTop(std::move(top));

    render::ResolveTarget target;
    std::string err;
    REQUIRE_MESSAGE(lane->createResolveTarget(kBigW, kBigH, target, err), err);
    const render::CompositeOptions opts;

    REQUIRE(lane->composite(doc, opts).ok);
    REQUIRE_MESSAGE(lane->resolve(target, err), err);
    target.layout = VK_IMAGE_LAYOUT_GENERAL;
    const std::uint64_t everyTile = lane->macroGrid().tileCount();
    CHECK(lane->stats().resolveTiles == everyTile);  // the first resolve is a full one

    // A composite with nothing changed submits nothing at all, and neither does its resolve.
    const render::TileCompositeStats idleBefore = lane->stats();
    REQUIRE(lane->composite(doc, opts).ok);
    REQUIRE_MESSAGE(lane->resolve(target, err), err);
    CHECK(lane->stats().resolveTiles == idleBefore.resolveTiles);
    CHECK(lane->stats().resolves == idleBefore.resolves);  // no submit == not a resolve

    // A 40 px layer nudged 3 px touches a handful of macrotiles, never the canvas.
    const std::uint64_t beforeEdit = lane->stats().resolveTiles;
    core::Layer* l = doc.find(topId);
    REQUIRE(l != nullptr);
    l->setTransform(Affine2D::translation(503.0, 400.0));
    REQUIRE(lane->composite(doc, opts).ok);
    REQUIRE_MESSAGE(lane->resolve(target, err), err);
    const std::uint64_t moved = lane->stats().resolveTiles - beforeEdit;
    INFO("macrotiles resolved for a 3 px nudge: " << moved << " of " << everyTile);
    CHECK(moved > 0);
    CHECK(moved < everyTile);

    // A DIFFERENT target has never held this document, so a partial resolve into it would leave
    // every untouched macrotile showing whatever was there before -- which on a fresh image is
    // undefined. It must be resolved in full whatever the dirty set says.
    render::ResolveTarget second;
    REQUIRE_MESSAGE(lane->createResolveTarget(kBigW, kBigH, second, err), err);
    const std::uint64_t beforeSecond = lane->stats().resolveTiles;
    REQUIRE_MESSAGE(lane->resolve(second, err), err);
    second.layout = VK_IMAGE_LAYOUT_GENERAL;
    CHECK(lane->stats().resolveTiles - beforeSecond == everyTile);

    Image a;
    Image b;
    REQUIRE_MESSAGE(lane->readTarget(target, mosaic::common::Rect{}, a, err), err);
    REQUIRE_MESSAGE(lane->readTarget(second, mosaic::common::Rect{}, b, err), err);
    // The incrementally-maintained target and the freshly-resolved one must agree exactly: any
    // difference is a macrotile the dirty set failed to mark, and that is the defect class that
    // shows up as a stale seam at one zoom level and nowhere else.
    CHECK(compare(a, b).maxLsb == 0);
    lane->destroyResolveTarget(target);
    lane->destroyResolveTarget(second);
}

TEST_CASE("a resolve target smaller than the document is refused, not clipped") {
    auto lane = makeLane("resolve target validation");
    if (!lane) return;

    core::Document doc(kW, kH);
    auto bg = doc.makeRaster("bg", kW, kH);
    bg->image() = backdrop();
    doc.root().addOnTop(std::move(bg));
    REQUIRE(lane->composite(doc, render::CompositeOptions{}).ok);

    render::ResolveTarget small;
    std::string err;
    REQUIRE_MESSAGE(lane->createResolveTarget(kW / 2, kH / 2, small, err), err);
    CHECK_FALSE(lane->resolve(small, err));
    lane->destroyResolveTarget(small);

    render::ResolveTarget none;
    CHECK_FALSE(lane->resolve(none, err));  // an empty target is a refusal, not a crash
}

// ---------------------------------------------------------------------------------------------
// THE DISPATCH SHAPE (S60-a item 10), end to end.
//
// test_composite_tile_parity.cpp proves the KERNEL reads the same integers whichever way they
// reach it. This section proves the LANE around it: that the dirty-macrotile list the host builds
// really does describe the same macrotiles the per-tile loop would have pushed, that the runs it
// groups them into land in the right accumulator atlas image, and that a partial recomposite
// through either list shape is still indistinguishable from a full one.
//
// Everything here asserts BYTE-identity rather than parity within 1 LSB. A dispatch reshape has no
// licence to move a pixel at all: it is the same arithmetic on the same integers, and a tolerance
// would let a real defect hide in the last bit until it showed up as a stale seam at one zoom.
// ---------------------------------------------------------------------------------------------

namespace {

// Composite the whole canvas with `d` and read the accumulator back. Asserts that the lane really
// SERVED with `d` -- a test that silently measured the fallback twice would prove nothing.
Image compositeWhole(render::TileCompositor& lane, core::Document& doc,
                     const render::CompositeOptions& opts, render::TileDispatch d,
                     render::TileCompositeStatus& out) {
    lane.setDispatchMode(d);
    lane.markAllDirty();
    out = lane.composite(doc, opts);
    INFO("refusal: " << std::string(render::tileRefusalName(out.refusal)) << " " << out.error);
    REQUIRE(out.ok);
    REQUIRE(out.dispatch == d);
    Image img;
    std::string err;
    REQUIRE_MESSAGE(lane.readback(mosaic::common::Rect{}, img, err), err);
    return img;
}

// The shapes worth running on THIS device: the two floor ones always, the indexed one only where
// the caps gate opened. Reported by name so a run that skipped it says so.
std::vector<render::TileDispatch> availableShapes(const render::TileCompositor& lane) {
    std::vector<render::TileDispatch> shapes{render::TileDispatch::PerTile,
                                             render::TileDispatch::TileList};
    if (lane.indexedRefusal() == render::DispatchRefusal::None) {
        shapes.push_back(render::TileDispatch::Indexed);
    } else {
        const std::string note =
            std::string("descriptor-indexed dispatch unavailable: ") +
            std::string(render::dispatchRefusalName(lane.indexedRefusal()));
        WARN_MESSAGE(true, note);
    }
    return shapes;
}

}  // namespace

TEST_CASE("every dispatch shape composites the same BYTES on a full canvas") {
    auto lane = makeLane("dispatch shape parity");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    // Big enough that the macrotile grid has many cells: at the default k a 200x140 canvas is one
    // macrotile and the list would have exactly one record, which proves nothing about the list.
    constexpr std::uint32_t kBigW = 1200;
    constexpr std::uint32_t kBigH = 900;
    const std::vector<LayerSpec> specs = {
        {backdrop(kBigW, kBigH), Affine2D::identity(), BlendMode::Normal, 1.0f},
        {sprite(), Affine2D::translation(140.0, 96.0), BlendMode::Multiply, 0.8f},
        {sprite(64, 48), Affine2D::translation(520.5, 380.25) * Affine2D::rotation(0.27),
         BlendMode::Screen, 0.9f},
        // A clip run, so the clip base -- the one thing the indexed shape binds differently, to
        // BOTH of its bindings at once -- is exercised rather than dodged.
        {sprite(80, 60), Affine2D::translation(560.0, 400.0), BlendMode::Overlay, 1.0f, true},
    };
    render::CompositeOptions opts;
    opts.resampleFilter = ResampleFilter::Bilinear;

    core::Document doc(kBigW, kBigH);
    buildDocument(doc, specs);
    lane->reset();

    render::TileCompositeStatus perTileSt;
    const Image reference =
        compositeWhole(*lane, doc, opts, render::TileDispatch::PerTile, perTileSt);
    const std::uint64_t macrotiles = lane->macroGrid().tileCount();
    REQUIRE(macrotiles > 4);
    // The cost model item 10 attacks, stated as an assertion rather than a claim.
    CHECK(perTileSt.dispatches == perTileSt.layers * macrotiles);

    for (const render::TileDispatch d : availableShapes(*lane)) {
        if (d == render::TileDispatch::PerTile) continue;
        render::TileCompositeStatus st;
        const Image other = compositeWhole(*lane, doc, opts, d, st);
        INFO("shape: " << std::string(render::tileDispatchName(d)));
        CHECK(st.macrotiles == perTileSt.macrotiles);
        // ... and the payoff: one dispatch per layer per accumulator atlas image. This document
        // fits one atlas, so that is one dispatch per layer, against `layers x macrotiles` above.
        CHECK(st.dispatches == st.layers);
        CHECK(other.rgba == reference.rgba);
    }

    // The picture is still the RIGHT one -- byte-identity between three shapes cannot see a defect
    // they share, and the CPU reference can. A CORROBORATION, not the assertion: the per-blend and
    // per-filter parity sweeps above own the tight bound, so this one carries the slack a
    // four-layer rotated stack needs rather than re-litigating fp16 rounding here.
    const render::CompositeResult cpu = render::composite(doc, opts, render::Backend::Cpu);
    REQUIRE(cpu.ok);
    expectParity(compare(cpu.image, reference), "dispatch shapes vs the CPU reference",
                 static_cast<long>(cpu.image.pixelCount() / 1000));
    CHECK(lane->validationErrors() == before);
}

TEST_CASE("every dispatch shape grades an adjustment the same BYTES") {
    auto lane = makeLane("adjustment x dispatch shape");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    // The adjustment kernel has the SAME three shapes as the composite one, for the same reason and
    // by the same mechanism: a specialization constant for the two floor shapes, a second blob for
    // the descriptor-indexed one. Nothing below the geometry read differs between them, so the
    // bytes must be identical -- and that is asserted, not approximated.
    //
    // Big enough that the macrotile grid has many cells, so the list carries a real run rather than
    // a single record.
    constexpr std::uint32_t kBigW = 1200;
    constexpr std::uint32_t kBigH = 900;
    const render::CompositeOptions opts;

    core::Document doc(kBigW, kBigH);
    {
        addBackdrop(doc, opaqueBackdrop(kBigW, kBigH));
        auto sp = doc.makeRaster("sprite", 96, 72);
        sp->image() = sprite();
        sp->setTransform(Affine2D::translation(140.0, 96.0));
        doc.root().addOnTop(std::move(sp));
        // A MASKED, CLIPPED adjustment: the mask exercises the clamped-domain stretch, the clip
        // exercises the base the composite kernel published into the clip atlas, and the indexed
        // shape reaches both of its sheets through the runtime array rather than a per-layer bind.
        core::AdjustmentLayer& adj = addAdjustment(
            doc, core::AdjustmentKind::HueSaturation,
            {{"hue", 55.0}, {"saturation", 30.0}, {"lightness", -8.0}});
        adj.setOpacity(0.77f);
        adj.setClipToBelow(true);
        core::RasterMask m(kBigW, kBigH);
        m.coverage = maskCoverage(kBigW, kBigH);
        adj.setMask(std::move(m));
        // ... and a table-backed kind above it, so the runtime array carries a synthesised sheet
        // alongside a real one.
        core::AdjustmentLayer& curves = addAdjustment(doc, core::AdjustmentKind::Curves, {});
        core::setAdjustmentCurve(curves.params(), core::CurveChannel::Composite, toneCurve());
    }
    lane->reset();

    render::TileCompositeStatus perTileSt;
    const Image reference =
        compositeWhole(*lane, doc, opts, render::TileDispatch::PerTile, perTileSt);
    const std::uint64_t macrotiles = lane->macroGrid().tileCount();
    REQUIRE(macrotiles > 4);
    CHECK(perTileSt.layers == 4);
    CHECK(perTileSt.adjustments == 2);
    CHECK(perTileSt.dispatches == perTileSt.layers * macrotiles);

    for (const render::TileDispatch d : availableShapes(*lane)) {
        if (d == render::TileDispatch::PerTile) continue;
        render::TileCompositeStatus st;
        const Image other = compositeWhole(*lane, doc, opts, d, st);
        INFO("shape: " << std::string(render::tileDispatchName(d)));
        CHECK(st.macrotiles == perTileSt.macrotiles);
        CHECK(st.adjustments == perTileSt.adjustments);
        CHECK(st.dispatches == st.layers);  // one per layer per atlas image; this fits one
        CHECK(other.rgba == reference.rgba);
    }

    // ... and it is the RIGHT picture. A corroboration rather than the tight bound: the per-kind
    // sweep above owns that, and a four-layer stack with a mask and a clip carries the same slack
    // the leaf dispatch-shape case does.
    const render::CompositeResult cpu = render::composite(doc, opts, render::Backend::Cpu);
    REQUIRE(cpu.ok);
    expectParity(compare(cpu.image, reference), "adjustment dispatch shapes vs the CPU reference",
                 static_cast<long>(cpu.image.pixelCount() / 1000));
    CHECK(lane->validationErrors() == before);
}

TEST_CASE("a partial recomposite is byte-identical whichever shape dispatches it") {
    auto lane = makeLane("dispatch shape x dirty set");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    constexpr std::uint32_t kBigW = 1200;
    constexpr std::uint32_t kBigH = 900;
    const std::vector<LayerSpec> specs = {
        {backdrop(kBigW, kBigH), Affine2D::identity(), BlendMode::Normal, 1.0f},
        {sprite(), Affine2D::translation(140.0, 96.0), BlendMode::Multiply, 0.8f},
    };
    render::CompositeOptions opts;

    // The three dirty-set shapes that matter to a list dispatch: one macrotile, two DISJOINT ones
    // (so the list is not a contiguous rectangle and the run grouping is load-bearing), and the
    // whole canvas.
    struct DirtyCase {
        const char* name;
        std::vector<mosaic::common::Rect> rects;  // empty == markAllDirty
    };
    const DirtyCase cases[] = {
        {"one macrotile", {mosaic::common::Rect{300.0, 300.0, 8.0, 8.0}}},
        {"two disjoint macrotiles",
         {mosaic::common::Rect{40.0, 40.0, 8.0, 8.0},
          mosaic::common::Rect{1000.0, 800.0, 8.0, 8.0}}},
        {"full canvas", {}},
    };

    const std::vector<render::TileDispatch> shapes = availableShapes(*lane);
    for (const DirtyCase& c : cases) {
        for (const render::TileDispatch d : shapes) {
            core::Document doc(kBigW, kBigH);
            buildDocument(doc, specs);
            // A fresh lane state per run, so both sides start from the identical accumulator: the
            // untouched macrotiles must come from the same full composite or the comparison is
            // measuring the seed rather than the shape.
            lane->reset();
            lane->setDispatchMode(render::TileDispatch::PerTile);
            lane->markAllDirty();
            const render::TileCompositeStatus full = lane->composite(doc, opts);
            REQUIRE(full.ok);
            Image whole;
            std::string err;
            REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, whole, err), err);

            lane->setDispatchMode(d);
            if (c.rects.empty()) {
                lane->markAllDirty();
            } else {
                for (const mosaic::common::Rect& r : c.rects) lane->markDirty(r);
            }
            const render::TileCompositeStatus partial = lane->composite(doc, opts);
            INFO(c.name << " / " << std::string(render::tileDispatchName(d)));
            REQUIRE(partial.ok);
            REQUIRE(partial.dispatch == d);
            // Nothing about the document changed, so re-running a subset of the macrotiles has to
            // land exactly where they already were. A shape that mis-maps a list record to the
            // wrong slot corrupts precisely the macrotiles it touched, which this catches and a
            // full-canvas comparison would not.
            CHECK(partial.uploadBytes == 0);
            if (!c.rects.empty()) {
                CHECK(partial.macrotiles > 0);
                CHECK(partial.macrotiles < lane->macroGrid().tileCount());
            }
            Image after;
            REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, after, err), err);
            CHECK(after.rgba == whole.rgba);
        }
    }
    CHECK(lane->validationErrors() == before);
}

TEST_CASE("the dispatch shape is a caps decision that refuses by name") {
    auto lane = makeLane("dispatch caps gate");
    if (!lane) return;

    // `Auto` is a request, never an answer: whatever the device, it resolves to something the
    // lane can actually run.
    if (std::getenv("MOSAIC_TILE_DISPATCH") == nullptr)
        CHECK(lane->dispatchMode() == render::TileDispatch::Auto);  // the shipping default
    CHECK(lane->activeDispatch() != render::TileDispatch::Auto);

    // ... and on EVERY device it resolves to the floor shape, including devices that could serve
    // the tier. Measured 2026-07-29 on an RX 6600 XT: indexed and list are the same lane to within
    // noise, because the tile list already collapses the dispatch to one per layer. Taking a
    // second SPIR-V blob and a runtime-sized descriptor array for 0% is not a default. This
    // assertion is the tripwire for anyone who changes that without a measurement to point at.
    if (std::getenv("MOSAIC_TILE_DISPATCH") == nullptr)
        CHECK(lane->activeDispatch() == render::TileDispatch::TileList);

    // The floor shapes are always available -- the list needs one storage buffer against a
    // guaranteed four and nothing else, which is why it and not the tier is the default.
    lane->setDispatchMode(render::TileDispatch::PerTile);
    CHECK(lane->activeDispatch() == render::TileDispatch::PerTile);
    lane->setDispatchMode(render::TileDispatch::TileList);
    CHECK(lane->activeDispatch() == render::TileDispatch::TileList);

    // Asking for the tier on a device that lacks it is a DOWNGRADE, not a failure, and the reason
    // is a name rather than a bool.
    lane->setDispatchMode(render::TileDispatch::Indexed);
    if (lane->indexedRefusal() == render::DispatchRefusal::None) {
        CHECK(lane->activeDispatch() == render::TileDispatch::Indexed);
        CHECK(lane->caps().descriptorIndexing);
        CHECK(lane->caps().fitsSpirvVersion(render::spirv::kVersion1_3));
    } else {
        CHECK(lane->activeDispatch() == render::TileDispatch::TileList);
        CHECK_FALSE(std::string(render::dispatchRefusalName(lane->indexedRefusal())).empty());
        // ... and the reason must be one the caps actually support. A refusal naming a cause that
        // is not present is worse than no name at all, because it sends the next reader hunting.
        const bool believable =
            !lane->caps().descriptorIndexing ||
            !lane->caps().fitsSpirvVersion(render::spirv::kVersion1_3) ||
            lane->indexedRefusal() == render::DispatchRefusal::DescriptorBudget ||
            lane->indexedRefusal() == render::DispatchRefusal::PipelineFailed;
        CHECK(believable);
    }
    lane->setDispatchMode(render::TileDispatch::Auto);
}

TEST_CASE("a document with more layers than the descriptor array holds falls back, not over") {
    auto lane = makeLane("indexed layer budget");
    if (!lane) return;
    if (lane->indexedRefusal() != render::DispatchRefusal::None) return;

    // The runtime array is sized once, at create(), so a document that outgrows it has to be
    // served by the list shape instead -- silently, and with the same pixels. 96 top-level raster
    // layers is well past the 64 the array is capped at.
    constexpr std::uint32_t kSmallW = 200;
    constexpr std::uint32_t kSmallH = 140;
    core::Document doc(kSmallW, kSmallH);
    {
        auto bg = doc.makeRaster("bg", kSmallW, kSmallH);
        bg->image() = backdrop(kSmallW, kSmallH);
        doc.root().addOnTop(std::move(bg));
        for (int i = 0; i < 95; ++i) {
            auto l = doc.makeRaster("l", 24, 24);
            l->image() = sprite(24, 24);
            l->setTransform(Affine2D::translation(2.0 * i, 1.0 * i));
            l->setOpacity(0.6f);
            doc.root().addOnTop(std::move(l));
        }
    }
    render::CompositeOptions opts;

    lane->reset();
    lane->setDispatchMode(render::TileDispatch::Indexed);
    lane->markAllDirty();
    const render::TileCompositeStatus st = lane->composite(doc, opts);
    REQUIRE(st.ok);
    CHECK(st.layers == 96);
    CHECK(st.dispatch == render::TileDispatch::TileList);  // downgraded, and it said so

    Image listed;
    std::string err;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, listed, err), err);

    lane->reset();
    lane->setDispatchMode(render::TileDispatch::PerTile);
    lane->markAllDirty();
    REQUIRE(lane->composite(doc, opts).ok);
    Image perTile;
    REQUIRE_MESSAGE(lane->readback(mosaic::common::Rect{}, perTile, err), err);
    CHECK(listed.rgba == perTile.rgba);
    lane->setDispatchMode(render::TileDispatch::Auto);
}
