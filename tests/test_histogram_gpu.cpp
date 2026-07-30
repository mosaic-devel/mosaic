// Histogram Vulkan-lane parity tests (S60-e; docs/s60-readback-consumers.md consumer A5): the
// compute lane against ui::computeHistogram, which is and stays the reference. Gated on a usable
// Vulkan device (the test_blur_gpu.cpp CI-safe pattern -- a machine without one WARNs and passes).
//
// ⚠ BYTE-IDENTICAL, NOT TOLERANCE-BASED, and the difference is the point. The blur lane compares
// within an epsilon because both its lanes run float and the residue is fused-multiply-add and
// transcendental rounding. Bins are INTEGER COUNTS: there is no rounding to absorb, so any epsilon
// here would be a licence for a real defect -- a dropped dispatch, a lost workgroup, a wrong
// weight -- to pass as "drift". A single count out of place is a bug.
//
// The cases below are chosen for the two things that can actually go wrong in this lane:
//   * the ALPHA-WEIGHTING RULE (transparent pixels contribute no colour and every pixel's alpha),
//     which came from a user-reported defect and is the reason the histogram was ever trusted;
//   * the DISPATCH SHAPE (partial workgroups, odd sizes, more than one chunk), where an off-by-one
//     bins the wrong pixel set and nothing else notices.
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "common/image.hpp"
#include "render/gpu_policy.hpp"
#include "render/histogram_gpu.hpp"
#include "ui/channels_panel.hpp"

using namespace mosaic;

namespace {

std::unique_ptr<render::HistogramGpu> makeLane(const char* who) {
    std::string err;
    auto gpu = render::HistogramGpu::create(/*enableValidation=*/true, err);
    if (!gpu) {
        const std::string why =
            std::string("no Vulkan device -- skipping ") + who + " (" + err + ")";
        WARN_MESSAGE(true, why);
    }
    return gpu;
}

void setPixel(common::Image& img, std::uint32_t x, std::uint32_t y, std::uint8_t r, std::uint8_t g,
              std::uint8_t b, std::uint8_t a) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    img.rgba[p] = r;
    img.rgba[p + 1] = g;
    img.rgba[p + 2] = b;
    img.rgba[p + 3] = a;
}

// Two smooth colour ramps, a spread of PARTIAL coverages (the weighting is a multiply, not a
// threshold, so it has to be exercised with alphas that are neither 0 nor 255), and a hard bright
// block so one bin is genuinely tall.
common::Image gradientImage(std::uint32_t w, std::uint32_t h) {
    common::Image img(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto r = static_cast<std::uint8_t>(x * 255u / (w > 1 ? w - 1 : 1));
            const auto g = static_cast<std::uint8_t>(y * 255u / (h > 1 ? h - 1 : 1));
            const auto b = static_cast<std::uint8_t>((x + y) & 0xffu);
            auto a = static_cast<std::uint8_t>(40u + ((x * 7u + y * 3u) % 216u));
            if (x < w / 8 && y < h / 8) {
                setPixel(img, x, y, 250, 250, 250, 255); // a tall spike for the atomics to fight over
                continue;
            }
            if (a == 0) a = 1; // 0 belongs to the transparency case, not this one
            setPixel(img, x, y, r, g, b, a);
        }
    return img;
}

// The alpha-weighting tripwire. Three populations, all of which the composite really produces:
//   * opaque pixels, which bin normally;
//   * transparent BLACK -- uncovered canvas, the common case, and the one whose stored RGB planted
//     a 1793x phantom spike at bin 0 when it was binned like a real pixel (reported 2026-07-23);
//   * transparent JUNK -- a pixel that was painted and then erased, so its stored RGB is neither
//     black nor meaningful. Binning it would show up somewhere other than bin 0, i.e. loudly.
// Every one of them must land in the ALPHA bins (how much canvas is empty is what that channel
// describes) and none of the transparent ones may touch R/G/B/luma.
common::Image transparencyImage() {
    common::Image img(96, 64);
    for (std::uint32_t y = 0; y < img.height; ++y)
        for (std::uint32_t x = 0; x < img.width; ++x) {
            if (y < 16)
                setPixel(img, x, y, 200, 120, 60, 255);      // opaque
            else if (y < 32)
                setPixel(img, x, y, 0, 0, 0, 0);             // uncovered canvas
            else if (y < 48)
                setPixel(img, x, y, 255, 0, 255, 0);         // erased: junk RGB behind a=0
            else
                setPixel(img, x, y, 30, 200, 90, 1);         // 1/255 visible: weighted, never dropped
        }
    return img;
}

struct BandDiff {
    std::size_t bin = 256; // 256 == the bands are identical
    std::uint64_t cpu = 0;
    std::uint64_t gpu = 0;
};

BandDiff firstDiff(const std::array<std::uint64_t, 256>& cpu,
                   const std::array<std::uint64_t, 256>& gpu) {
    for (std::size_t v = 0; v < cpu.size(); ++v)
        if (cpu[v] != gpu[v]) return BandDiff{v, cpu[v], gpu[v]};
    return BandDiff{};
}

void checkBand(const char* label, const char* band, const std::array<std::uint64_t, 256>& cpu,
               const std::array<std::uint64_t, 256>& gpu) {
    const BandDiff d = firstDiff(cpu, gpu);
    INFO(std::string(label) << " / " << band << ": first differing bin " << d.bin << " -- cpu "
                            << d.cpu << ", gpu " << d.gpu);
    CHECK(d.bin == 256);
}

void checkParity(render::HistogramGpu& gpu, const common::Image& img, const char* label) {
    const ui::ChannelHistogram cpu = ui::computeHistogram(img);
    render::HistogramBins bins;
    REQUIRE(gpu.bin(img, bins));
    checkBand(label, "red", cpu.r, bins.r);
    checkBand(label, "green", cpu.g, bins.g);
    checkBand(label, "blue", cpu.b, bins.b);
    checkBand(label, "alpha", cpu.a, bins.a);
    checkBand(label, "luma", cpu.luma, bins.luma);
}

} // namespace

TEST_CASE("GPU histogram bins a flat image exactly like the CPU reference") {
    auto gpu = makeLane("flat parity");
    if (!gpu) return;
    // The worst case for a naive kernel: every pixel hits ONE bin per band, so the shared
    // sub-histogram takes the whole contention. Also the clearest overflow probe available at this
    // size -- an opaque flat bin holds 255 * pixelCount.
    common::Image opaque(200, 150);
    opaque.fill(common::Color8{17, 200, 134, 255});
    checkParity(*gpu, opaque, "flat opaque");

    common::Image half(200, 150);
    half.fill(common::Color8{17, 200, 134, 128}); // half coverage: the weight is 128, not 1
    checkParity(*gpu, half, "flat half-covered");

    common::Image clear(200, 150);
    clear.fill(common::Color8{0, 0, 0, 0}); // an entirely uncovered canvas: NO colour at all
    checkParity(*gpu, clear, "flat transparent");
    render::HistogramBins bins;
    REQUIRE(gpu->bin(clear, bins));
    CHECK(bins.r[0] == 0); // the phantom spike, stated as an assertion rather than an intention
    CHECK(bins.g[0] == 0);
    CHECK(bins.b[0] == 0);
    CHECK(bins.luma[0] == 0);
    CHECK(bins.a[0] == static_cast<std::uint64_t>(clear.pixelCount())); // every pixel, still
}

TEST_CASE("GPU histogram bins a gradient exactly like the CPU reference") {
    auto gpu = makeLane("gradient parity");
    if (!gpu) return;
    // 320x240 = 76800 px, several workgroups' worth, so a lost or double-counted group shows up.
    checkParity(*gpu, gradientImage(320, 240), "gradient 320x240");
}

TEST_CASE("GPU histogram weights colour by coverage and alpha by pixel") {
    auto gpu = makeLane("alpha weighting");
    if (!gpu) return;
    const common::Image img = transparencyImage();
    checkParity(*gpu, img, "mixed transparency");

    render::HistogramBins bins;
    REQUIRE(gpu->bin(img, bins));
    const std::uint64_t quarter = static_cast<std::uint64_t>(img.width) * 16;
    // Two whole bands of transparent pixels contribute NOTHING to colour...
    CHECK(bins.r[0] == 0);   // the transparent-black band's stored red
    CHECK(bins.r[255] == 0); // the erased band's junk red
    CHECK(bins.b[255] == 0); // ... and its junk blue
    // ... but every one of them is counted by alpha, which is what that channel is for.
    CHECK(bins.a[0] == 2 * quarter);
    CHECK(bins.a[1] == quarter);
    CHECK(bins.a[255] == quarter);
    // The barely-visible band is weighted, not thresholded away: 1 unit of alpha per pixel.
    CHECK(bins.g[200] == quarter);
    // And the opaque band carries its full 255 per pixel.
    CHECK(bins.r[200] == 255 * quarter);
}

TEST_CASE("GPU histogram is exact at sizes that do not divide the dispatch") {
    auto gpu = makeLane("ragged sizes");
    if (!gpu) return;
    // The kernel walks a FLAT pixel array with a grid-stride loop, so a row length that is not a
    // multiple of the 64-invocation workgroup cannot skew a row -- but the tail of the last
    // partially-filled workgroup is real, and so is a single-invocation dispatch.
    checkParity(*gpu, gradientImage(61, 37), "61x37 (width not a multiple of 64)");
    checkParity(*gpu, gradientImage(65, 1), "65x1 (one ragged row)");
    checkParity(*gpu, gradientImage(1, 1), "1x1 (a single invocation)");
    checkParity(*gpu, gradientImage(63, 63), "63x63 (one short workgroup)");
}

TEST_CASE("GPU histogram refuses what the CPU reference must serve") {
    auto gpu = makeLane("refusals");
    if (!gpu) return;
    render::HistogramBins bins;
    // An empty image is not a histogram of zero pixels, it is nothing to bin: the panel clears its
    // bins for this case and never calls the lane, and the lane says so rather than guessing.
    CHECK_FALSE(gpu->bin(common::Image{}, bins));
    // A malformed image (a short buffer for its stated size) is a refusal too -- reading past the
    // bound range of a storage buffer is not something to be relaxed about.
    common::Image ragged(8, 8);
    ragged.rgba.resize(8 * 8 * 4 - 1);
    CHECK_FALSE(gpu->bin(ragged, bins));
}

TEST_CASE("GPU histogram is deterministic per device") {
    auto gpu = makeLane("determinism");
    if (!gpu) return;
    // Atomics, but on integers with a bound that forbids wraparound -- so the bins may not vary
    // with scheduling order. If this ever flakes, the chunk bound is wrong, not the hardware.
    const common::Image img = gradientImage(320, 240);
    render::HistogramBins once, twice;
    REQUIRE(gpu->bin(img, once));
    REQUIRE(gpu->bin(img, twice));
    CHECK(once.r == twice.r);
    CHECK(once.g == twice.g);
    CHECK(once.b == twice.b);
    CHECK(once.a == twice.a);
    CHECK(once.luma == twice.luma);
}

TEST_CASE("GPU histogram sums its chunks correctly past 2^24 pixels") {
    auto gpu = makeLane("multi-chunk parity");
    if (!gpu) return;
    // ⚠ THIS CASE IS EXPENSIVE ON PURPOSE (~64 MiB of pixels, twice: the host image and the mapped
    // upload). It is the only way to reach the chunking, and the chunking is the one piece of
    // arithmetic in the lane that a small image cannot exercise at all: a colour bin is a uint32
    // whose unit is ALPHA, so it wraps at 16,843,010 opaque pixels and the lane splits the image at
    // 2^24. 4096x4097 is the smallest size that produces a second chunk, and it makes that chunk a
    // single 4096-pixel row -- a partial descriptor range and a non-zero bin base in one shot.
    common::Image big(4096, 4097);
    for (std::size_t p = 0; p + 3 < big.rgba.size(); p += 4) {
        const auto t = static_cast<std::uint8_t>((p >> 2) & 0xffu);
        big.rgba[p] = t;
        big.rgba[p + 1] = static_cast<std::uint8_t>(255u - t);
        big.rgba[p + 2] = static_cast<std::uint8_t>((t * 3u) & 0xffu);
        big.rgba[p + 3] = static_cast<std::uint8_t>(t | 1u); // never 0: keep the colour bins loaded
    }
    render::HistogramBins bins;
    if (!gpu->bin(big, bins)) {
        // A refusal here is legitimate (a 64 MiB host-visible allocation is not guaranteed on a
        // small part) and is not a defect -- the CPU reference serves. Say so rather than fail.
        WARN_MESSAGE(true, "the lane refused a 4096x4097 image -- skipping the multi-chunk case");
        return;
    }
    const ui::ChannelHistogram cpu = ui::computeHistogram(big);
    checkBand("4096x4097", "red", cpu.r, bins.r);
    checkBand("4096x4097", "green", cpu.g, bins.g);
    checkBand("4096x4097", "blue", cpu.b, bins.b);
    checkBand("4096x4097", "alpha", cpu.a, bins.a);
    checkBand("4096x4097", "luma", cpu.luma, bins.luma);
}

TEST_CASE("the device binner rebuilds every total and mean the CPU reference reports") {
    auto gpu = makeLane("binner round-trip");
    if (!gpu) return;
    const ui::HistogramBinner binner = ui::gpuHistogramBinner(gpu.get());
    REQUIRE(static_cast<bool>(binner));
    const common::Image img = gradientImage(320, 240);
    const ui::ChannelHistogram cpu = ui::computeHistogram(img);
    ui::ChannelHistogram via;
    REQUIRE(binner(img, via));
    CHECK(via.totalPixels == cpu.totalPixels);
    CHECK(via.visibleAlpha == cpu.visibleAlpha);
    // EXACT, not approximate. The sums behind these are integers recovered from the bins, and the
    // two float steps are the reference's own operations in the reference's own order (one shared
    // reciprocal for the alpha-weighted trio, a plain division for meanA). An Approx here would
    // hide a divergence rather than tolerate one -- and Approx(x).epsilon(0.0) is unsatisfiable.
    CHECK(via.meanR == cpu.meanR);
    CHECK(via.meanG == cpu.meanG);
    CHECK(via.meanB == cpu.meanB);
    CHECK(via.meanA == cpu.meanA);
    CHECK(via.meanLuma == cpu.meanLuma);
}

TEST_CASE("the histogram lane refuses CPU-only mode BY NAME, and the reference still serves") {
    using namespace mosaic::render;
    // Restore whatever the process was running under, whatever this test does or throws -- the
    // whole suite shares one policy, and leaving it flipped would silently skip every GPU test
    // that runs after this one (the test_settings.cpp idiom).
    struct Restore {
        GpuPolicy saved = gpuPolicy();
        ~Restore() { setGpuPolicy(saved); }
    };
    [[maybe_unused]] const Restore restore;

    setGpuPolicy(GpuPolicy{GpuUse::CpuOnly});
    std::string error;
    const std::unique_ptr<HistogramGpu> lane = HistogramGpu::create(/*enableValidation=*/false,
                                                                    error);
    CHECK(lane == nullptr);
    // A refusal is a named, actionable sentence -- never an empty string and never a failure.
    CHECK_FALSE(error.empty());
    CHECK(error.find("histogram") != std::string::npos);
    CHECK(error.find("CPU-only") != std::string::npos);
    CHECK(error.find("--cpu") != std::string::npos);

    // And the panel's side of the refusal: no lane means no binner, which means computeHistogram.
    // The fallback is not a degraded mode -- it is the definition the lane is measured against.
    CHECK_FALSE(static_cast<bool>(ui::gpuHistogramBinner(nullptr)));
}
