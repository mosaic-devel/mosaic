#include "ui/status_bar.hpp"

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "ui/theme.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <optional>

// The status bar's readouts (PLAN S13-b): the pure formatting helpers, plus the strip's own draw
// pass -- which a StatusBar, being an Fl_Group and not an Fl_Window, will happily give to an
// Fl_Image_Surface (an unshown Fl_Window would come back black; a plain widget shoots fine). That
// matters for one contract in particular: "over a transparent texel there is no colour section" is
// a claim about pixels, and every pure helper here would keep passing either way.
namespace {

using mosaic::common::Color8;
using mosaic::ui::CursorReadout;
using mosaic::ui::formatColorReadout;
using mosaic::ui::formatCursorPosition;
using mosaic::ui::formatDocumentInfo;
using mosaic::ui::formatSelectionBounds;
using mosaic::ui::formatViewState;
using mosaic::ui::StatusBar;

constexpr int kBarW = 900;
constexpr int kBarH = mosaic::ui::kStatusBarHeight;
// The left flow is clipped at the colour-space slot: w - kPadX(10) - kViewW(104) - kSectionGap(18)
// - kSpaceW(170) - kSectionGap(18) = 580 at this width (status_bar.cpp's constants -- if they move,
// this moves). Everything under test lives inside it, and it is the one span the colour-space
// ScrollingLabel, whose marquee is time-based, never draws in.
constexpr int kFlowW = 580;

CursorReadout readout(bool inside, Color8 c) {
    CursorReadout r;
    r.docX = 12.7;
    r.docY = 34.2;
    r.insideDocument = inside;
    r.color = c;
    return r;
}

// One real draw() of the strip, off screen. No document info on purpose: the flow then starts at
// the far left, so the colour section AND the selection section behind it land well inside the
// clip -- two shots that agreed only because both were cut off would prove nothing.
[[nodiscard]] mosaic::common::Image shoot(const std::optional<CursorReadout>& cursor) {
    StatusBar bar(0, 0, kBarW, kBarH);
    bar.setSelectionBounds(mosaic::common::Rect{10.0, 20.0, 200.0, 100.0});
    bar.setCursor(cursor);

    auto* surf = new Fl_Image_Surface(kBarW, kBarH);
    Fl_Surface_Device::push_current(surf);
    surf->draw(&bar, 0, 0);
    Fl_Surface_Device::pop_current();

    Fl_RGB_Image* img = surf->image();
    const auto* src = reinterpret_cast<const unsigned char*>(img->data()[0]);
    const int d = img->d();
    const int w = img->w();
    const int h = img->h();
    mosaic::common::Image out(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = src + (static_cast<std::size_t>(y) * w + x) * d;
            std::uint8_t* q = &out.rgba[((static_cast<std::size_t>(y) * w) + x) * 4];
            q[0] = p[0];
            q[1] = p[1];
            q[2] = p[2];
            q[3] = 255;
        }
    delete img;
    delete surf;
    return out;
}

[[nodiscard]] Color8 pixelAt(const mosaic::common::Image& img, int x, int y) {
    const std::size_t i = ((static_cast<std::size_t>(y) * img.width) + x) * 4;
    return Color8{img.rgba[i], img.rgba[i + 1], img.rgba[i + 2], 255};
}

// Pixels that differ between two shots, over the left flow only.
[[nodiscard]] int differing(const mosaic::common::Image& a, const mosaic::common::Image& b) {
    int n = 0;
    for (int y = 0; y < kBarH; ++y)
        for (int x = 0; x < kFlowW; ++x)
            if (pixelAt(a, x, y) != pixelAt(b, x, y))
                ++n;
    return n;
}

[[nodiscard]] int countOf(const mosaic::common::Image& img, Color8 c) {
    int n = 0;
    for (int y = 0; y < kBarH; ++y)
        for (int x = 0; x < kFlowW; ++x)
            if (pixelAt(img, x, y) == c)
                ++n;
    return n;
}

// The rightmost column carrying ink -- how far the left flow reaches. Row 0 is skipped: the
// hairline against the canvas spans the whole strip and would answer "the right edge" every time.
[[nodiscard]] int inkRight(const mosaic::common::Image& img) {
    const Color8 bg = mosaic::ui::activePalette().panelBg;
    int right = -1;
    for (int y = 1; y < kBarH; ++y)
        for (int x = 0; x < kFlowW; ++x) {
            const Color8 p = pixelAt(img, x, y);
            if (p.r != bg.r || p.g != bg.g || p.b != bg.b)
                right = std::max(right, x);
        }
    return right;
}

} // namespace

TEST_CASE("document info: px + physical size at the document ppi + bit depth") {
    // 1920 px / 96 ppi = 20 in = 50.8 cm; 1080 px / 96 ppi = 11.25 in = 28.57 cm.
    CHECK(formatDocumentInfo(1920, 1080, 96.0, "8-bit integer", /*metric=*/true) ==
          "1920 × 1080 px · 50.8 × 28.57 cm @ 96 ppi · 8-bit integer");
    // Imperial expresses the same size in inches.
    CHECK(formatDocumentInfo(1920, 1080, 96.0, "8-bit integer", /*metric=*/false) ==
          "1920 × 1080 px · 20 × 11.25 in @ 96 ppi · 8-bit integer");
    // A non-positive ppi omits the physical size instead of dividing by zero (unit irrelevant).
    CHECK(formatDocumentInfo(64, 64, 0.0, "16-bit float", /*metric=*/true) ==
          "64 × 64 px · 16-bit float");
}

TEST_CASE("cursor position: the texel the pointer is in (floored document coords)") {
    CHECK(formatCursorPosition(12.7, 34.2) == "X 12  Y 34");
    CHECK(formatCursorPosition(0.0, 0.999) == "X 0  Y 0");
    CHECK(formatCursorPosition(-0.3, 5.0) == "X -1  Y 5"); // just off the canvas edge
}

TEST_CASE("colour readout: hex + decimal RGBA") {
    CHECK(formatColorReadout({94, 126, 255, 255}) == "#5E7EFF · 94, 126, 255, 255");
    CHECK(formatColorReadout({0, 0, 0, 128}) == "#000000 · 0, 0, 0, 128");
    // a == 0 never reaches this helper any more -- the strip draws no colour section at all there
    // (see the draw case below), rather than printing "#000000 · 0, 0, 0, 0" for an empty pixel.
}

TEST_CASE("view state: zoom percent + rotation degrees") {
    CHECK(formatViewState(1.0, 0.0) == "100% · 0.0°");
    CHECK(formatViewState(0.333, 45.26) == "33.3% · 45.3°");
    CHECK(formatViewState(64.0, -180.0) == "6400% · -180.0°");
    // Tiny negative rotations snap to a stable "0.0°" instead of flickering "-0.0°".
    CHECK(formatViewState(1.0, -0.04) == "100% · 0.0°");
}

TEST_CASE("selection bounds: size @ origin") {
    CHECK(formatSelectionBounds({10.0, 20.0, 200.0, 100.0}) == "200 × 100 @ (10, 20)");
}

// ⚠ Over a fully transparent texel the strip used to draw a checkerboard chip and then print
// "#000000 · 0, 0, 0, 0" beside it -- a colour reported for a pixel that has none. The section is
// now simply absent there, chip and text both, and the readouts around it must not notice.
TEST_CASE("a transparent texel draws NO colour section -- chip and text both") {
#if defined(__SANITIZE_ADDRESS__) ||                                                               \
    (defined(__has_feature) && __has_feature(address_sanitizer)) // NOLINT
    return; // FLTK/X11 internals leak on teardown under LeakSanitizer; this is not a memory test
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return;

    constexpr Color8 kChip{255, 0, 255, 255}; // nothing else in the strip is magenta

    const mosaic::common::Image opaque = shoot(readout(/*inside=*/true, kChip));
    const mosaic::common::Image clear = shoot(readout(/*inside=*/true, {0, 0, 0, 0}));
    // The reference layout: a cursor OUTSIDE the document never had a colour section to begin
    // with, so this is exactly what "the section is gone" has to look like.
    const mosaic::common::Image outside = shoot(readout(/*inside=*/false, kChip));
    const mosaic::common::Image noCursor = shoot(std::nullopt);
    REQUIRE(opaque.width == static_cast<std::uint32_t>(kBarW));

    // The premise, first: this comparison CAN see a colour section. Without it every check below
    // would pass just as happily if shoot() were drawing an empty strip.
    CHECK(differing(opaque, outside) > 0);
    // The 11x11 chip less the 1px border ring drawn over it is 81 px; the floor only leaves room
    // for a ring the backend antialiases inward, not for "no chip".
    CHECK(countOf(opaque, kChip) >= 49);

    // The contract: over a == 0 the flow is pixel-identical to one that never carried a colour
    // section. So the chip is gone, the hex/RGBA text is gone, and the selection section behind it
    // sits exactly where it sits without one -- a leftover gap, or a doubled one, would shift it
    // and land here.
    CHECK(differing(clear, outside) == 0);
    CHECK(countOf(clear, kChip) == 0);
    CHECK(inkRight(clear) == inkRight(outside));
    CHECK(inkRight(opaque) > inkRight(clear)); // ... and the section really did occupy that space

    // Only the COLOUR section goes: the position readout still holds its place in the flow (with
    // no cursor at all, the selection section slides left into it).
    CHECK(inkRight(clear) > inkRight(noCursor));

    // And the trigger is exactly a == 0. One unit of alpha is still a colour, chip and text.
    CHECK(differing(shoot(readout(/*inside=*/true, {0, 0, 0, 1})), outside) > 0);
}
