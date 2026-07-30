#include "ui/channels_panel.hpp"

#include "common/image.hpp"

#include <cstdint>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <vector>

using namespace mosaic;

namespace {
// A 1x1 image with distinct per-channel values, so a mask/bin is unambiguous.
common::Image onePixel(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    common::Image img(1, 1);
    img.rgba = {r, g, b, a};
    return img;
}
} // namespace

TEST_CASE("ChannelViewMask::isNormal is exactly the full-colour composite") {
    CHECK(ui::ChannelViewMask{}.isNormal()); // default: r,g,b on, alpha off
    CHECK(ui::ChannelViewMask{true, true, true, false}.isNormal());
    CHECK_FALSE(ui::ChannelViewMask{true, false, true, false}.isNormal()); // a colour hidden
    CHECK_FALSE(ui::ChannelViewMask{true, true, true, true}.isNormal());   // alpha view
    CHECK_FALSE(ui::ChannelViewMask{false, false, false, false}.isNormal());
}

TEST_CASE("applyChannelViewMask: the normal view is a byte-for-byte no-op") {
    common::Image img = onePixel(10, 20, 30, 40);
    const common::Image before = img;
    ui::applyChannelViewMask(img, ui::ChannelViewMask{}); // normal
    CHECK(img == before);
}

TEST_CASE("applyChannelViewMask: a single colour channel isolates to grayscale, keeping alpha") {
    common::Image img = onePixel(10, 20, 30, 40);
    ui::applyChannelViewMask(img, ui::ChannelViewMask{true, false, false, false}); // red only
    CHECK(img.rgba[0] == 10);
    CHECK(img.rgba[1] == 10);
    CHECK(img.rgba[2] == 10);
    CHECK(img.rgba[3] == 40); // colour views keep the pixel's own alpha

    common::Image green = onePixel(10, 20, 30, 40);
    ui::applyChannelViewMask(green, ui::ChannelViewMask{false, true, false, false});
    CHECK(green.rgba[0] == 20);
    CHECK(green.rgba[1] == 20);
    CHECK(green.rgba[2] == 20);
}

TEST_CASE("applyChannelViewMask: two colour channels keep those, zero the hidden one") {
    common::Image img = onePixel(10, 20, 30, 40);
    ui::applyChannelViewMask(img, ui::ChannelViewMask{true, true, false, false}); // red+green, no blue
    CHECK(img.rgba[0] == 10);
    CHECK(img.rgba[1] == 20);
    CHECK(img.rgba[2] == 0);
    CHECK(img.rgba[3] == 40);
}

TEST_CASE("applyChannelViewMask: the alpha view shows coverage as opaque grayscale") {
    common::Image img = onePixel(10, 20, 30, 40);
    ui::applyChannelViewMask(img, ui::ChannelViewMask{false, false, false, true});
    CHECK(img.rgba[0] == 40);
    CHECK(img.rgba[1] == 40);
    CHECK(img.rgba[2] == 40);
    CHECK(img.rgba[3] == 255); // forced opaque: the values themselves are what you see
}

TEST_CASE("applyChannelViewMask: an empty image is a safe no-op") {
    common::Image img; // 0x0
    ui::applyChannelViewMask(img, ui::ChannelViewMask{true, false, false, false});
    CHECK(img.empty());
}

// ---------------------------------------------------------------------------------------------
// channelViewShaderCode -- the on-device form of the same mask (S60-a item 10). The on-canvas
// isolation is no longer a CPU pass over a copy of the composite; canvas_present.comp applies it
// where it samples the composite, driven by this code. applyChannelViewMask above stays the
// SPECIFICATION, so what these cases check is that the two cannot drift apart.
// ---------------------------------------------------------------------------------------------

namespace {

// The GLSL decode from canvas_present.comp's applyChannelView(), transcribed. Faithful because
// every branch of that function is a COPY, a SELECT, a ZERO or the constant 1.0 -- there is no
// arithmetic anywhere in it -- and the rgba8 <-> normalized-float mapping is a bijection on those,
// so doing it in 8-bit here reproduces the shader's output value for value.
common::Image decodeLikeShader(const common::Image& src, std::uint32_t view) {
    common::Image out = src;
    if (view == ui::kChannelViewNormal)
        return out; // the shader's identity early-out
    for (std::size_t p = 0; p + 3 < out.rgba.size(); p += 4) {
        const std::uint8_t r = out.rgba[p], g = out.rgba[p + 1], b = out.rgba[p + 2],
                           a = out.rgba[p + 3];
        if ((view & ui::kChannelViewAlpha) != 0u) {
            out.rgba[p] = out.rgba[p + 1] = out.rgba[p + 2] = a;
            out.rgba[p + 3] = 255;
            continue;
        }
        const bool sr = (view & ui::kChannelViewRed) != 0u;
        const bool sg = (view & ui::kChannelViewGreen) != 0u;
        const bool sb = (view & ui::kChannelViewBlue) != 0u;
        if (static_cast<int>(sr) + static_cast<int>(sg) + static_cast<int>(sb) == 1) {
            const std::uint8_t v = sr ? r : (sg ? g : b);
            out.rgba[p] = out.rgba[p + 1] = out.rgba[p + 2] = v;
        } else {
            out.rgba[p] = sr ? r : 0;
            out.rgba[p + 1] = sg ? g : 0;
            out.rgba[p + 2] = sb ? b : 0;
        }
    }
    return out;
}

ui::ChannelViewMask maskFromIndex(int i) {
    return ui::ChannelViewMask{(i & 1) != 0, (i & 2) != 0, (i & 4) != 0, (i & 8) != 0};
}

} // namespace

TEST_CASE("channelViewShaderCode: NORMAL is 0, which is the shader's identity") {
    // Load-bearing, not cosmetic: 0 is what the renderer starts at and what the shader early-outs
    // on, so "nothing isolated" must produce the value a canvas that never heard of channel views
    // already carries. Anything else and the default document would run the remap.
    CHECK(ui::channelViewShaderCode(ui::ChannelViewMask{}) == 0u);
    CHECK(ui::channelViewShaderCode(ui::ChannelViewMask{true, true, true, false}) == 0u);
    CHECK(ui::kChannelViewNormal == 0u);
}

TEST_CASE("channelViewShaderCode: every non-normal view is marked and carries its shown channels") {
    for (int i = 0; i < 16; ++i) {
        const ui::ChannelViewMask m = maskFromIndex(i);
        CAPTURE(i);
        const std::uint32_t code = ui::channelViewShaderCode(m);
        if (m.isNormal()) {
            CHECK(code == 0u);
            continue;
        }
        // The marker bit is what keeps the all-colours-hidden view (legal, and black) out of 0 --
        // without it that view would encode as 0 and read back as the normal composite.
        CHECK((code & ui::kChannelViewActive) != 0u);
        CHECK(((code & ui::kChannelViewRed) != 0u) == m.red);
        CHECK(((code & ui::kChannelViewGreen) != 0u) == m.green);
        CHECK(((code & ui::kChannelViewBlue) != 0u) == m.blue);
        CHECK(((code & ui::kChannelViewAlpha) != 0u) == m.alpha);
    }
    // The degenerate "nothing shown" view is distinct from normal, and is NOT zero.
    CHECK(ui::channelViewShaderCode(ui::ChannelViewMask{false, false, false, false}) ==
          ui::kChannelViewActive);
}

TEST_CASE("the present shader's channel view reproduces applyChannelViewMask exactly") {
    // The whole point of moving the remap onto the device is that it is the SAME remap. Run both
    // over every view and a spread of pixels -- opaque, transparent, partly covered, the 0/255
    // endpoints, and equal-channel pixels where a wrong channel pick would go unnoticed.
    const std::uint8_t px[] = {
        10,  20,  30,  40,  // distinct channels, partial alpha
        0,   0,   0,   0,   // transparent black (uncovered canvas)
        255, 255, 255, 255, // opaque white
        255, 0,   0,   255, // pure red
        0,   255, 0,   128, // pure green, half covered
        0,   0,   255, 1,   // pure blue, all but invisible
        77,  77,  77,  200, // equal channels
        1,   2,   3,   254,
    };
    common::Image src(static_cast<std::uint32_t>(std::size(px) / 4), 1);
    std::copy(std::begin(px), std::end(px), src.rgba.begin());

    for (int i = 0; i < 16; ++i) {
        const ui::ChannelViewMask m = maskFromIndex(i);
        CAPTURE(i);
        common::Image cpu = src;
        ui::applyChannelViewMask(cpu, m);
        // Exact equality, not an approximation: both paths only copy, select and zero bytes.
        CHECK(decodeLikeShader(src, ui::channelViewShaderCode(m)) == cpu);
    }
}

TEST_CASE("the present shader leaves the composite untouched while the view is normal") {
    // The identity assertion the move is worth writing: with nothing isolated the pass must produce
    // BYTE-identical pixels to a build without any of this. (The canvas texture itself is never
    // remapped either way now -- the isolation is applied on the way OUT, at sample time -- which
    // is what keeps the histogram, the cursor colour readout and Smart Resize reading true pixels.)
    const common::Image src = onePixel(10, 20, 30, 40);
    CHECK(decodeLikeShader(src, ui::channelViewShaderCode(ui::ChannelViewMask{})) == src);
    CHECK(decodeLikeShader(src, ui::kChannelViewNormal) == src);
}

TEST_CASE("computeHistogram bins each channel and reports the means") {
    const common::Image img = onePixel(10, 20, 30, 40);
    const ui::ChannelHistogram h = ui::computeHistogram(img);

    CHECK(h.totalPixels == 1);
    // Colour bins are ALPHA-WEIGHTED: this pixel is 40/255 visible, so it deposits 40 units.
    CHECK(h.r[10] == 40);
    CHECK(h.g[20] == 40);
    CHECK(h.b[30] == 40);
    CHECK(h.luma[18] == 40); // Rec.601: (10*77 + 20*150 + 30*29) >> 8 = 4640 >> 8 = 18
    CHECK(h.visibleAlpha == 40);
    // Alpha bins stay plain pixel counts -- that distribution is the point of the alpha channel.
    CHECK(h.a[40] == 1);

    // Means are unchanged: weighting scales numerator and denominator alike for a uniform image.
    CHECK(h.meanR == doctest::Approx(10.0));
    CHECK(h.meanG == doctest::Approx(20.0));
    CHECK(h.meanB == doctest::Approx(30.0));
    CHECK(h.meanA == doctest::Approx(40.0));
    CHECK(h.meanLuma == doctest::Approx(18.0));
}

TEST_CASE("histogram: an uncovered canvas plants NO phantom spike at bin 0") {
    // The user-reported defect (2026-07-23): "major spikes of colour even when the colour is
    // evenly distributed". The composite carries uncovered canvas as transparent BLACK, and the
    // histogram used to bin that stored RGB like any other pixel -- inventing a black pixel that
    // is nowhere on screen. An uncovered canvas is the common case, so the phantom bar at bin 0
    // dwarfed the real distribution.
    //
    // Build the pathological shape deliberately: a perfectly flat, fully-covered ramp over a
    // canvas that is mostly empty. The visible colour is uniform, so a correct histogram is FLAT.
    // Measured on this exact input, the old binning put 14344 in bin 0 against a tallest-other-bin
    // of 8 -- a 1793x phantom spike on content with no spike in it.
    constexpr std::uint32_t kW = 256, kH = 64;
    common::Image img(kW, kH);
    for (std::uint32_t y = 0; y < kH; ++y) {
        for (std::uint32_t x = 0; x < kW; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * kW + x) * 4;
            const bool covered = y < 8; // only the top eighth is painted; 7/8 is empty canvas
            // ⚠ Uncovered canvas must be transparent BLACK, exactly as the compositor emits it --
            // that is what makes this a faithful repro. (Giving the uncovered pixels the ramp
            // colour instead spreads them evenly over all 256 bins and hides the defect entirely;
            // this test passed for the wrong reason until that was corrected.)
            const auto v = static_cast<std::uint8_t>(covered ? x : 0);
            img.rgba[o] = v; // one pixel per value, per row
            img.rgba[o + 1] = v;
            img.rgba[o + 2] = v;
            img.rgba[o + 3] = covered ? 255 : 0;
        }
    }
    const ui::ChannelHistogram h = ui::computeHistogram(img);

    CHECK(h.totalPixels == kW * kH); // every pixel was examined ...
    // ... but only the covered ones carry colour, and they are perfectly evenly distributed.
    const std::uint64_t expected = h.r[1];
    CHECK(expected > 0);
    for (int v = 0; v < 256; ++v) {
        CAPTURE(v);
        CHECK(h.r[v] == expected); // FLAT -- no bin, including 0, stands above any other
        CHECK(h.g[v] == expected);
        CHECK(h.b[v] == expected);
    }
    // The alpha channel still reports the emptiness, because that is what it is for.
    CHECK(h.a[0] == kW * (kH - 8));
    CHECK(h.a[255] == kW * 8);
    CHECK(h.meanA == doctest::Approx(255.0 * 8.0 / kH));
    // The visible mean is the ramp's mean (127.5), NOT dragged toward black by empty canvas.
    CHECK(h.meanR == doctest::Approx(127.5));
    CHECK(h.meanLuma == doctest::Approx(127.0).epsilon(0.02)); // Rec.601 rounding on the ramp
}

TEST_CASE("histogram: partial coverage is weighted, not counted whole") {
    // A soft brush edge should not deposit as much colour as an opaque stroke. Two pixels of the
    // same colour at different coverage must land in the same bin with different weight.
    common::Image img(2, 1);
    const std::uint8_t px[] = {200, 100, 50, 255, 200, 100, 50, 51}; // same colour, 100% and 20%
    std::copy(std::begin(px), std::end(px), img.rgba.begin());
    const ui::ChannelHistogram h = ui::computeHistogram(img);

    CHECK(h.r[200] == 255u + 51u);
    CHECK(h.visibleAlpha == 255u + 51u);
    CHECK(h.totalPixels == 2);
    CHECK(h.meanR == doctest::Approx(200.0)); // same colour twice, whatever the coverage
}

TEST_CASE("histogram: a fully transparent image has no visible colour at all") {
    common::Image img(4, 4); // default-constructed = all zero, i.e. transparent black
    const ui::ChannelHistogram h = ui::computeHistogram(img);

    CHECK_FALSE(h.empty());       // it HAS pixels ...
    CHECK(h.totalPixels == 16);
    CHECK(h.visibleAlpha == 0);   // ... none of which you can see
    CHECK(h.a[0] == 16);
    for (int v = 0; v < 256; ++v) {
        CAPTURE(v);
        CHECK(h.r[v] == 0);
        CHECK(h.g[v] == 0);
        CHECK(h.b[v] == 0);
        CHECK(h.luma[v] == 0);
    }
    // No visible pixels => no visible mean colour. Reporting 0 (i.e. "black") would be a claim
    // about pixels that are not there; the means simply stay at their neutral default.
    CHECK(h.meanR == doctest::Approx(0.0));
    CHECK(h.meanA == doctest::Approx(0.0));
}

TEST_CASE("computeHistogram of an empty image is empty") {
    const ui::ChannelHistogram h = ui::computeHistogram(common::Image{});
    CHECK(h.empty());
    CHECK(h.totalPixels == 0);
}

// ---------------------------------------------------------------------------------------------
// histogramNormalizer -- the display scale (user-reported 2026-07-23: the plot "still doesn't
// appear to be all that trustable"). The DATA was right; normalizing to the tallest bin was not.
// ---------------------------------------------------------------------------------------------

namespace {

// A band with `count` bins of height `bulk`, plus `spikes` outliers of height `tower`.
std::array<std::uint64_t, 256> makeBand(int count, std::uint64_t bulk, int spikes,
                                        std::uint64_t tower) {
    std::array<std::uint64_t, 256> b{};
    for (int i = 0; i < count && i < 256; ++i)
        b[static_cast<std::size_t>(i)] = bulk;
    for (int i = 0; i < spikes && i < 256; ++i)
        b[static_cast<std::size_t>(255 - i)] = tower;
    return b;
}

} // namespace

TEST_CASE("histogramNormalizer ignores outliers instead of being defined by them") {
    // The reported shape: a broad bulk plus two towers ~20x taller. Normalizing to the max (what
    // the plot used to do) scales the bulk to ~5% of the plot -- an unreadable sliver.
    const auto band = makeBand(/*count=*/200, /*bulk=*/1000, /*spikes=*/2, /*tower=*/20000);
    const std::vector<const std::array<std::uint64_t, 256>*> bands{&band};

    const std::uint64_t norm = ui::histogramNormalizer(bands);
    CHECK(norm == 1000);  // the bulk, not the towers
    CHECK(norm < 20000);  // ... so the towers clip rather than dictate the scale
}

TEST_CASE("histogramNormalizer leaves a genuinely flat histogram alone") {
    // Nothing to be robust against: every bin equal must normalize to that value and fill the
    // plot. A percentile that "protected" against outliers here would be inventing them.
    const auto band = makeBand(/*count=*/256, /*bulk=*/777, /*spikes=*/0, /*tower=*/0);
    const std::vector<const std::array<std::uint64_t, 256>*> bands{&band};
    CHECK(ui::histogramNormalizer(bands) == 777);
}

TEST_CASE("histogramNormalizer counts only non-empty bins") {
    // Empty bins are the majority in a real image (a photo touches maybe half the range). Letting
    // them vote would drag any percentile to zero and blow every bar off the top of the plot.
    const auto sparse = makeBand(/*count=*/8, /*bulk=*/500, /*spikes=*/0, /*tower=*/0);
    const std::vector<const std::array<std::uint64_t, 256>*> bands{&sparse};
    CHECK(ui::histogramNormalizer(bands) == 500);
    CHECK(ui::histogramNormalizer(bands) > 0);
}

TEST_CASE("histogramNormalizer is safe on degenerate input") {
    const std::array<std::uint64_t, 256> allZero{};
    CHECK(ui::histogramNormalizer({&allZero}) == 1);        // never divides by zero
    CHECK(ui::histogramNormalizer({}) == 1);                // no bands at all
    CHECK(ui::histogramNormalizer({nullptr}) == 1);         // a null band is skipped, not deref'd
    const auto band = makeBand(64, 10, 0, 0);
    CHECK(ui::histogramNormalizer({&band}, 0) >= 1);        // percentile clamped into range
    CHECK(ui::histogramNormalizer({&band}, 100) == 10);
    CHECK(ui::histogramNormalizer({&band}, 1000) == 10);
}

TEST_CASE("histogramNormalizer spans every shown band") {
    // R, G and B are normalized together so their bars stay comparable to each other; a per-band
    // scale would make a flat channel look as tall as a peaked one.
    const auto low = makeBand(100, 100, 0, 0);
    const auto high = makeBand(100, 900, 0, 0);
    const std::uint64_t both = ui::histogramNormalizer({&low, &high});
    CHECK(both >= 100);
    CHECK(both <= 900);
    CHECK(both != ui::histogramNormalizer({&low}));
}
