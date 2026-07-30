#include "common/image.hpp"
#include "common/image_svg.hpp"

#include <assets/app_icon_svg.hpp> // generated from assets/app_icon.svg
#include <assets/icon_close_svg.hpp>
#include <assets/icon_eye_closed_svg.hpp>
#include <assets/icon_eye_open_svg.hpp>
#include <assets/icon_group_layers_svg.hpp>
#include <assets/icon_lock_closed_svg.hpp>
#include <assets/icon_lock_open_svg.hpp>
#include <assets/icon_plus_svg.hpp>
#include <assets/icon_trash_svg.hpp>

#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>
#include <string>
#include <vector>

using namespace mosaic;

namespace {
common::Image rasterizeLiteral(const char* svg, int w, int h, std::string& err) {
    return common::rasterizeSvg(reinterpret_cast<const unsigned char*>(svg), std::strlen(svg), w, h,
                                &err);
}
} // namespace

TEST_CASE("rasterizeSvg renders a solid rect to opaque RGBA") {
    const char* svg =
        R"(<svg viewBox="0 0 10 10" width="10" height="10" xmlns="http://www.w3.org/2000/svg">)"
        R"(<rect x="0" y="0" width="10" height="10" fill="#FF0000"/></svg>)";
    std::string err;
    const common::Image img = rasterizeLiteral(svg, 16, 16, err);
    REQUIRE_MESSAGE(!img.empty(), err);
    CHECK(img.width == 16);
    CHECK(img.height == 16);
    REQUIRE(img.rgba.size() == std::size_t{16} * 16 * 4);

    const std::size_t center = (8 * 16 + 8) * 4; // a fully-covered interior pixel
    CHECK(img.rgba[center + 0] == 255);
    CHECK(img.rgba[center + 1] == 0);
    CHECK(img.rgba[center + 2] == 0);
    CHECK(img.rgba[center + 3] == 255);
}

TEST_CASE("rasterizeSvg rejects empty input") {
    std::string err;
    const common::Image img = common::rasterizeSvg(nullptr, 0, 8, 8, &err);
    CHECK(img.empty());
    CHECK_FALSE(err.empty());
}

TEST_CASE("rasterizeSvg rejects a non-positive target size") {
    const char* svg = R"(<svg width="4" height="4"><rect width="4" height="4"/></svg>)";
    std::string err;
    const common::Image img = rasterizeLiteral(svg, 0, 8, err);
    CHECK(img.empty());
    CHECK_FALSE(err.empty());
}

TEST_CASE("embedded app icon rasterizes to an opaque square") {
    std::string err;
    const common::Image img =
        common::rasterizeSvg(assets::app_icon_svg, assets::app_icon_svg_size, 64, 64, &err);
    REQUIRE_MESSAGE(!img.empty(), err);
    CHECK(img.width == 64);
    CHECK(img.height == 64);

    // The icon has an opaque background, so a center pixel must be fully opaque.
    const std::size_t center = (32 * 64 + 32) * 4;
    CHECK(img.rgba[center + 3] == 255);
}

// ---- panel-chrome icons (ui/icons.hpp) ---------------------------------------------------------

namespace {

struct ChromeIcon {
    const char* name;
    const unsigned char* data;
    std::size_t size;
};

const std::vector<ChromeIcon>& chromeIcons() {
    static const std::vector<ChromeIcon> icons{
        {"plus", assets::icon_plus_svg, assets::icon_plus_svg_size},
        {"trash", assets::icon_trash_svg, assets::icon_trash_svg_size},
        {"group_layers", assets::icon_group_layers_svg, assets::icon_group_layers_svg_size},
        {"eye_open", assets::icon_eye_open_svg, assets::icon_eye_open_svg_size},
        {"eye_closed", assets::icon_eye_closed_svg, assets::icon_eye_closed_svg_size},
        {"lock_open", assets::icon_lock_open_svg, assets::icon_lock_open_svg_size},
        {"lock_closed", assets::icon_lock_closed_svg, assets::icon_lock_closed_svg_size},
        {"close", assets::icon_close_svg, assets::icon_close_svg_size},
    };
    return icons;
}

constexpr int kChromePx = 16; // ui::kIconPx -- the ONLY size these are ever rasterized at

} // namespace

// A path typo (a malformed arc, a stray command letter) makes nanosvg drop the subpath SILENTLY --
// the icon still "renders", just blank or half-drawn. Assert every chrome icon puts real ink down.
TEST_CASE("panel-chrome icons rasterize with ink") {
    for (const ChromeIcon& icon : chromeIcons()) {
        CAPTURE(icon.name);
        std::string err;
        const common::Image img = common::rasterizeSvg(icon.data, icon.size, kChromePx, kChromePx, &err);
        REQUIRE_MESSAGE(!img.empty(), err);
        CHECK(img.width == static_cast<std::uint32_t>(kChromePx));

        int opaque = 0;
        for (std::size_t p = 3; p < img.rgba.size(); p += 4)
            if (img.rgba[p] >= 200)
                ++opaque;
        // A 1px stroke over a 16px box: a handful of pixels is far too few, the whole box far too
        // many. Both bounds catch a dropped subpath and a runaway fill.
        CHECK(opaque > 12);
        CHECK(opaque < kChromePx * kChromePx / 2);
    }
}

// The whole point of re-authoring these on a 16x16 grid: at scale 1.0 a 1px stroke on a half-integer
// covers exactly one pixel column, so most of the ink is FULLY opaque rather than a 50%-coverage
// smear. Rasterizing the same art at any other size drops this ratio through the floor -- which is
// what "the small icons look blurry" was. Pin it so nobody reintroduces a fractional scale.
TEST_CASE("panel-chrome icons are crisp at their native size") {
    for (const ChromeIcon& icon : chromeIcons()) {
        CAPTURE(icon.name);
        std::string err;
        const common::Image img =
            common::rasterizeSvg(icon.data, icon.size, kChromePx, kChromePx, &err);
        REQUIRE_MESSAGE(!img.empty(), err);
        int solid = 0; // fully-inked pixels
        int touched = 0; // any coverage at all
        for (std::size_t p = 3; p < img.rgba.size(); p += 4) {
            if (img.rgba[p] >= 240)
                ++solid;
            if (img.rgba[p] >= 24)
                ++touched;
        }
        REQUIRE(touched > 0);
        // Every icon must land SOME fully-inked pixels. An all-grey icon is the blur, by definition:
        // eye_closed once scored a flat zero here, drawn as 1px strokes on a pure curve.
        CHECK(solid > 0);
        // Curves (the eye almond, the lock shackle) legitimately anti-alias their shoulders, so this
        // is a floor, not a demand for perfection. Measured at 16px: plus and folder 1.00, trash
        // 0.96, locks 0.65, eye_open 0.44, eye_closed 0.33. Rasterized off-grid they collapse to
        // 0.00-0.47. The margin below the worst native score is deliberate.
        CHECK(static_cast<double>(solid) / touched > 0.25);
    }
}

// Rasterizing at a NON-native size is exactly the regression this guards. Scaling the pixel-aligned
// art to 18px must measurably soften it -- if this ever stops holding, the assets drifted off-grid.
TEST_CASE("rasterizing a chrome icon off its native size softens it") {
    const auto solidFraction = [](const unsigned char* data, std::size_t n, int px) {
        std::string err;
        const common::Image img = common::rasterizeSvg(data, n, px, px, &err);
        REQUIRE_MESSAGE(!img.empty(), err);
        int solid = 0, touched = 0;
        for (std::size_t p = 3; p < img.rgba.size(); p += 4) {
            if (img.rgba[p] >= 240) ++solid;
            if (img.rgba[p] >= 24) ++touched;
        }
        REQUIRE(touched > 0);
        return static_cast<double>(solid) / touched;
    };
    // The plus is all straight strokes, so the effect is unambiguous there.
    const double native = solidFraction(assets::icon_plus_svg, assets::icon_plus_svg_size, 16);
    const double off = solidFraction(assets::icon_plus_svg, assets::icon_plus_svg_size, 18);
    CHECK(native > off);
    CHECK(native > 0.75); // straight 2px strokes on integer centres: nearly every pixel is solid
}

// ui::drawIcon replaces each pixel's RGB with a palette ink and keeps the rasterizer's alpha as the
// coverage. That is only sound because the art is pure white -- a coloured pixel would silently
// lose its colour. Pin the invariant here so a future edit to the assets cannot break the tinting.
TEST_CASE("panel-chrome icons carry exactly one white ink") {
    for (const ChromeIcon& icon : chromeIcons()) {
        CAPTURE(icon.name);
        std::string err;
        const common::Image img = common::rasterizeSvg(icon.data, icon.size, 32, 32, &err);
        REQUIRE_MESSAGE(!img.empty(), err);
        for (std::size_t p = 0; p + 3 < img.rgba.size(); p += 4) {
            if (img.rgba[p + 3] < 200)
                continue; // partial coverage: un-premultiply rounding, not a colour claim
            CHECK(img.rgba[p + 0] >= 250);
            CHECK(img.rgba[p + 1] >= 250);
            CHECK(img.rgba[p + 2] >= 250);
        }
    }
}

// The eye and lock pairs differ in SHAPE, not colour (the ink is identical) -- so a copy-paste of
// the wrong asset would otherwise pass every check above while making the states indistinguishable.
TEST_CASE("panel-chrome icon state pairs are distinct rasters") {
    const auto raster = [](const unsigned char* d, std::size_t n) {
        std::string err;
        common::Image img = common::rasterizeSvg(d, n, 32, 32, &err);
        REQUIRE_MESSAGE(!img.empty(), err);
        return img;
    };
    const common::Image eyeOpen = raster(assets::icon_eye_open_svg, assets::icon_eye_open_svg_size);
    const common::Image eyeClosed =
        raster(assets::icon_eye_closed_svg, assets::icon_eye_closed_svg_size);
    const common::Image lockOpen = raster(assets::icon_lock_open_svg, assets::icon_lock_open_svg_size);
    const common::Image lockClosed =
        raster(assets::icon_lock_closed_svg, assets::icon_lock_closed_svg_size);
    CHECK(eyeOpen.rgba != eyeClosed.rgba);
    CHECK(lockOpen.rgba != lockClosed.rgba);
}
