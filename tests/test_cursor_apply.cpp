#include "ui/cursor_apply.hpp"

#include "platform/native_window.hpp"

#include <FL/Fl_RGB_Image.H>

#include <doctest/doctest.h>

// The FLTK side of the cursor contract (src/ui/cursor_apply.cpp). tests/test_cursors.cpp pins the
// ART -- that each builder reports a logical box and a logical hotspot which round-trip through the
// device scale. This file pins the HANDOVER: that makeCursorImage actually puts those numbers where
// Fl_Wayland_Window_Driver::set_cursor_4args reads them.
//
// It reads exactly two things off the Fl_RGB_Image -- `w()/h()` (which it treats as LOGICAL and
// multiplies by the window's buffer scale) and the pixel data (which it draws into that box under a
// matching cairo_scale). So the invariant is: w()/h() report the LOGICAL box while data_w()/data_h()
// stay at DEVICE resolution. Get it backwards and the cursor renders at scale x its intended size --
// the S59-a defect this idiom exists to close.
namespace {

using mosaic::ui::CursorImage;
using mosaic::ui::chromeCursorScale;
using mosaic::ui::fitTextCursor;
using mosaic::ui::makeCursorImage;
using mosaic::ui::MoveCursor;
using mosaic::ui::moveCursor;
using mosaic::ui::neswCursor;
using mosaic::ui::nwseCursor;
using mosaic::ui::panCursor;
using mosaic::ui::rotateCursor;
using mosaic::ui::textCursor;

} // namespace

TEST_CASE("makeCursorImage: logical drawing box over device-resolution data") {
    const auto handsOver = [](const CursorImage& c, int scale, const char* what) {
        INFO(what << " at scale " << scale);
        const auto img = makeCursorImage(c);
        REQUIRE(img != nullptr);
        // What set_cursor_4args multiplies by the buffer scale ...
        CHECK(img->w() == c.logicalW);
        CHECK(img->h() == c.logicalH);
        // ... and what it draws into that box: the full-resolution rasterization, untouched.
        CHECK(img->data_w() == static_cast<int>(c.image.width));
        CHECK(img->data_h() == static_cast<int>(c.image.height));
        CHECK(img->d() == 4); // RGBA: the cursors are transparent outside their art
        // The device box is the logical one grown by the scale -- so `w() * buffer_scale` lands
        // back on the pixels we rasterized, which is the whole point of the idiom.
        CHECK(img->w() * scale == img->data_w());
        CHECK(img->h() * scale == img->data_h());
    };
    for (const int s : {1, 2, 3}) {
        handsOver(moveCursor(/*darkMode=*/false, s), s, "moveCursor");
        handsOver(panCursor(/*grabbing=*/false, s), s, "panCursor(grab)");
        handsOver(panCursor(/*grabbing=*/true, s), s, "panCursor(grabbing)");
        handsOver(fitTextCursor(s), s, "fitTextCursor");
        handsOver(rotateCursor(0.4, /*darkMode=*/false, s), s, "rotateCursor");
        handsOver(textCursor(0.4, /*darkMode=*/true, s), s, "textCursor");
        handsOver(nwseCursor(/*darkMode=*/false, s), s, "nwseCursor");
        handsOver(neswCursor(/*darkMode=*/true, s), s, "neswCursor");
    }
}

TEST_CASE("makeCursorImage: a failed build yields nullptr, never a degenerate image") {
    // Every caller reads the null as "fall back to the stock cursor". Handing FLTK a zero-sized or
    // data-less Fl_RGB_Image instead would set an invisible pointer -- worse than the wrong one.
    CHECK(makeCursorImage(CursorImage{}) == nullptr);

    CursorImage noPixels = moveCursor(/*darkMode=*/false);
    noPixels.image.rgba.clear();
    CHECK(makeCursorImage(noPixels) == nullptr);

    CursorImage noBox = moveCursor(/*darkMode=*/false);
    noBox.logicalW = 0;
    CHECK(makeCursorImage(noBox) == nullptr);

    CursorImage noHeight = moveCursor(/*darkMode=*/false);
    noHeight.logicalH = -1;
    CHECK(makeCursorImage(noHeight) == nullptr);
}

TEST_CASE("MoveCursor / chromeCursorScale: no window, no cursor, no crash") {
    // The chrome sites all call these off a widget's window(), which is null before the widget is
    // added to one -- and FL_LEAVE can arrive during teardown. A null window must be inert.
    MoveCursor mc;
    mc.apply(nullptr);
    mc.apply(nullptr, /*darkMode=*/true, /*buildScale=*/2.0);
    mc.reset();
    mc.apply(nullptr);

    // X11, macOS and a null window have no per-window buffer scale to read: the builders must be
    // driven at 1, where the logical and device coordinate systems coincide.
    CHECK(mosaic::platform::windowBufferScale(nullptr) == 1);
    CHECK(chromeCursorScale(nullptr) == doctest::Approx(1.0));
}
