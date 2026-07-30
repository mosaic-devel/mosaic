// Settings -> Appearance "Selection & reticle line" card previews: the pure CPU mirror of the
// present shader's overlay-line styles (ui/line_style_preview). The values here restate the
// design contract (2026-07-07 rounds): Classic flips hard at mid-grey, Shadowed keeps a white
// core and earns its rim only over light content, Adaptive ramps white -> graphite 0.42 (held)
// -> black with the shadow rising only around the tone == content crossing.

#include "ui/line_style_preview.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

using mosaic::ui::lineStyleShade;
using mosaic::ui::renderLineStylePreview;

TEST_CASE("classic overlay shade: hard black/white key, background untouched off the line") {
    // On the stroke's centre: pure white over dark content, pure black over light.
    CHECK(lineStyleShade(0, 0.1f, 0.0f, 0.1f) == doctest::Approx(1.0f));
    CHECK(lineStyleShade(0, 0.9f, 0.0f, 0.9f) == doctest::Approx(0.0f));
    // Past the ~1.5px profile nothing changes.
    CHECK(lineStyleShade(0, 0.3f, 2.0f, 0.3f) == doctest::Approx(0.3f));
}

TEST_CASE("shadowed overlay shade: white core everywhere, rim only over light content") {
    CHECK(lineStyleShade(1, 0.05f, 0.0f, 0.05f) == doctest::Approx(1.0f));
    CHECK(lineStyleShade(1, 0.95f, 0.0f, 0.95f) == doctest::Approx(1.0f));
    // In the rim zone (2px out): bare over dark content (the key is 0 below luma 0.25),
    // visibly darkened over light.
    CHECK(lineStyleShade(1, 0.1f, 2.0f, 0.1f) == doctest::Approx(0.1f));
    CHECK(lineStyleShade(1, 0.9f, 2.0f, 0.9f) < 0.85f);
}

TEST_CASE("adaptive overlay shade: white -> graphite dwell -> black core") {
    CHECK(lineStyleShade(2, 0.0f, 0.0f, 0.0f) == doctest::Approx(1.0f));  // white on black
    CHECK(lineStyleShade(2, 0.5f, 0.0f, 0.5f) == doctest::Approx(1.0f));  // still white at mid
    CHECK(lineStyleShade(2, 0.77f, 0.0f, 0.77f) == doctest::Approx(0.42f));  // the graphite hold
    CHECK(lineStyleShade(2, 1.0f, 0.0f, 1.0f) == doctest::Approx(0.0f));  // black on white
    // The rim shadow rises around the tone == content crossing (k ~0.64) ...
    CHECK(lineStyleShade(2, 0.62f, 2.0f, 0.62f) < 0.58f);
    // ... and is fully gone where the core has real contrast (black core on white content).
    CHECK(lineStyleShade(2, 1.0f, 2.0f, 1.0f) == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("line-style preview frames render and animate") {
    std::vector<std::uint8_t> a, b, c;
    renderLineStylePreview(a, 116, 76, 0, 0.0);
    CHECK(a.size() == 116u * 76u * 3u);
    renderLineStylePreview(b, 116, 76, 0, 30.0);  // a drifted background must differ
    CHECK(a != b);
    renderLineStylePreview(c, 116, 76, 2, 0.0);  // a different style must differ
    CHECK(a != c);
    renderLineStylePreview(b, 116, 76, 0, 0.0, 146.0);  // a sibling card's window onto the shared
    CHECK(a != b);                                      // field shows a different slice
    renderLineStylePreview(a, 0, 0, 1, 0.0);  // degenerate sizes render nothing, never crash
    CHECK(a.empty());
}

TEST_CASE("line-style preview background is the warm photo-noise field") {
    // The card background is the design bench's 4th torture tile: dark loam -> cream, so red
    // dominates blue everywhere the (grey) chrome isn't covering. A synthetic grey ramp would
    // fail this -- the point is a real-image stand-in, not a sweep.
    std::vector<std::uint8_t> px;
    renderLineStylePreview(px, 116, 76, 1, 0.0);
    int warm = 0, total = 0;
    for (std::size_t o = 0; o + 2 < px.size(); o += 3) {
        ++total;
        if (px[o] > px[o + 2])
            ++warm;
    }
    CHECK(warm > total * 3 / 4);
    // And the field must not be flat: the luma range should span dark to light so every style
    // has something to key against.
    std::uint8_t lo = 255, hi = 0;
    for (std::size_t o = 0; o + 2 < px.size(); o += 3) {
        const auto l = static_cast<std::uint8_t>((299 * px[o] + 587 * px[o + 1] + 114 * px[o + 2]) / 1000);
        lo = std::min(lo, l);
        hi = std::max(hi, l);
    }
    CHECK(lo < 60);
    CHECK(hi > 180);
}
