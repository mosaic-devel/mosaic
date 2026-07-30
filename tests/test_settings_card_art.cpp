// The Tools -> Brush display-mode cards' preview art (docs/brushes.md §8.2).
//
// ⚠ THIS FILE EXISTS BECAUSE THE DIALOG CANNOT BE SHOT. A `SettingsDialog` is an `Fl_Window`, and an
// unshown Fl_Window renders BLACK to an `Fl_Image_Surface` -- unlike a Panel or any other plain
// widget, which shoots fine. So the pane itself cannot be judged headlessly, and its art shipped once
// with a staircased stroke that nobody could see until a human ran the app. The one draw call CAN be
// shot, so it is.
#include "common/image.hpp"
#include "io/io.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/theme.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>

#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

using namespace mosaic;

namespace {

// The real card's preview area: kCardW 132 minus the OptionCard's 8 px inset on each side, and
// kCardPreviewH 92 likewise. (settings_dialog.cpp's constants -- if they move, this moves.)
constexpr int kW = 116;
constexpr int kH = 76;
constexpr int kPad = 12; // a magenta margin around the art: anything that lands in it ESCAPED

[[nodiscard]] common::Image shoot(int mode, common::Color8 bg) {
    const int w = kW + 2 * kPad;
    const int h = kH + 2 * kPad;
    auto* surf = new Fl_Image_Surface(w, h);
    Fl_Surface_Device::push_current(surf);
    fl_color(fl_rgb_color(255, 0, 255)); // the "outside the box" detector
    fl_rectf(0, 0, w, h);
    // ⚠ NO CLIP. The real OptionCard pushes one, which would HIDE a spill rather than show it -- and
    // a stroke clipped at the box edge is exactly what "going outside the box" LOOKS like. Draw it
    // naked, and let the margin catch what escapes.
    ui::drawPresetDisplayPreview(kPad, kPad, kW, kH, mode, bg);
    Fl_Surface_Device::pop_current();

    Fl_RGB_Image* img = surf->image();
    const auto* px = reinterpret_cast<const unsigned char*>(img->data()[0]);
    const int d = img->d();
    common::Image out(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = px + (static_cast<std::size_t>(y) * w + x) * d;
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

[[nodiscard]] bool isMagenta(const common::Image& img, int x, int y) {
    const std::size_t i = ((static_cast<std::size_t>(y) * img.width) + x) * 4;
    return img.rgba[i] > 250 && img.rgba[i + 1] < 5 && img.rgba[i + 2] > 250;
}

} // namespace

// ⚠ A USER-REPORTED BUG: "the stroke is also going outside the preview box there". The art is drawn
// inside an OptionCard's fl_push_clip, so a spill does not paint over the dialog -- it gets CUT at
// the box's edge, which is exactly what a stroke running out of its box looks like. The clip hides
// the cause and shows only the symptom, so this case draws the art WITHOUT one and puts a magenta
// margin around it: anything that lands out there was never going to fit.
TEST_CASE("the display-mode card art stays inside the box it is given") {
#if defined(__SANITIZE_ADDRESS__) ||                                                               \
    (defined(__has_feature) && __has_feature(address_sanitizer)) // NOLINT
    return; // FLTK/X11 internals leak on teardown under LeakSanitizer; this is not a memory test
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return;

    const common::Color8 bg = ui::activePalette().controlBg;

    for (const int mode : {0, 1}) {
        const common::Image art = shoot(mode, bg);
        int escaped = 0;
        for (int y = 0; y < static_cast<int>(art.height); ++y)
            for (int x = 0; x < static_cast<int>(art.width); ++x) {
                const bool inside = x >= kPad && x < kPad + kW && y >= kPad && y < kPad + kH;
                if (!inside && !isMagenta(art, x, y))
                    ++escaped;
            }
        INFO("mode " << mode << " (0 = Grid, 1 = Cards)");
        CHECK(escaped == 0);

        // ... and it actually DREW something: a case that painted nothing would pass the line above
        // with room to spare.
        int painted = 0;
        for (int y = kPad; y < kPad + kH; ++y)
            for (int x = kPad; x < kPad + kW; ++x)
                if (!isMagenta(art, x, y))
                    ++painted;
        CHECK(painted > (kW * kH) / 2);

        // ⚠⚠ THE ACTUAL BUG, AND THE ONE THE MARGIN ABOVE CANNOT SEE. The stroke never escaped the
        // CARD -- the blit clips it -- it ran into the strip's own FRAME and got guillotined there,
        // which is what "going outside the preview box" looks like from the outside. So: no pixel of
        // the accent ink may sit ON or NEXT TO a frame line. A stroke keeping one clear pixel from
        // every border it is drawn inside is a stroke that fits.
        //
        // No geometry is re-derived here on purpose: the frame is simply "wherever the border ink
        // is", so the case cannot drift from the layout it is checking.
        const ui::Palette& pal = ui::activePalette();
        const auto at = [&](int x, int y) {
            const std::size_t i = ((static_cast<std::size_t>(y) * art.width) + x) * 4;
            return common::Color8{art.rgba[i], art.rgba[i + 1], art.rgba[i + 2], 255};
        };
        int touching = 0;
        for (int y = kPad + 1; y < kPad + kH - 1; ++y)
            for (int x = kPad + 1; x < kPad + kW - 1; ++x) {
                if (at(x, y) != pal.accent)
                    continue; // only the stroke's own solid core
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (at(x + dx, y + dy) == pal.border)
                            ++touching;
            }
        CHECK(touching == 0);

        if (const char* dir = std::getenv("MOSAIC_CARD_SHOT"); dir != nullptr) {
            std::string err;
            (void)io::savePng(art, std::string(dir) + "/card-art-" + std::to_string(mode) + ".png",
                              {}, &err); // best-effort debug shot; failure just skips the file
        }
    }
}
