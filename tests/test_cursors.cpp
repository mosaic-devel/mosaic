#include "ui/cursors.hpp"

#include <doctest/doctest.h>

#include <clocale>
#include <cstddef>
#include <cstdlib>
#include <string>

// The selection tools' procedural cursor (ui::selectionCursor): crosshair geometry, the black
// halo / white core contrast pair, the per-op badges, and HiDPI scaling.
namespace {

using mosaic::core::SelectOp;
using mosaic::ui::CursorImage;
using mosaic::ui::fitTextCursor;
using mosaic::ui::moveCursor;
using mosaic::ui::neswCursor;
using mosaic::ui::nwseCursor;
using mosaic::ui::panCursor;
using mosaic::ui::rotateCursor;
using mosaic::ui::selectionCursor;
using mosaic::ui::textCursor;

// White-core pixels (r=255, opaque) inside the half-open box [x0,x1) x [y0,y1).
int countWhite(const mosaic::common::Image& img, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
            if (img.rgba[p] == 255 && img.rgba[p + 3] == 255)
                ++n;
        }
    return n;
}

} // namespace

TEST_CASE("selectionCursor: crosshair core, halo, hotspot, transparent ground") {
    const CursorImage c = selectionCursor(SelectOp::Replace);
    CHECK(c.image.width == 25);
    CHECK(c.image.height == 25);
    CHECK(c.hotX == 12);
    CHECK(c.hotY == 12);
    const auto at = [&](int x, int y, int ch) {
        return c.image.rgba[(static_cast<std::size_t>(y) * c.image.width + x) * 4 + ch];
    };
    CHECK(at(12, 12, 0) == 255); // white core at the hotspot...
    CHECK(at(12, 12, 3) == 255);
    CHECK(at(11, 11, 0) == 0); // ...black halo on the diagonal neighbour...
    CHECK(at(11, 11, 3) == 255);
    CHECK(at(0, 0, 3) == 0);   // ...and a transparent ground around it
    CHECK(at(24, 24, 3) == 0);
    CHECK(countWhite(c.image, 16, 16, 23, 23) == 0); // Replace carries no badge
}

TEST_CASE("selectionCursor: op badges and HiDPI scaling") {
    const int add = countWhite(selectionCursor(SelectOp::Add).image, 16, 16, 23, 23);
    const int sub = countWhite(selectionCursor(SelectOp::Subtract).image, 16, 16, 23, 23);
    const int isect = countWhite(selectionCursor(SelectOp::Intersect).image, 16, 16, 23, 23);
    CHECK(add == 13); // a 7+7 plus sharing its centre pixel
    CHECK(sub == 7);  // the bare bar
    CHECK(isect == 13); // two 7-px diagonals sharing the centre

    const CursorImage big = selectionCursor(SelectOp::Add, 2);
    CHECK(big.image.width == 50);
    CHECK(big.image.height == 50);
    CHECK(big.hotX == 25); // the centre pixel's centre after the 2x upscale
    CHECK(big.hotY == 25);
    const int base = countWhite(selectionCursor(SelectOp::Add).image, 0, 0, 25, 25);
    CHECK(countWhite(big.image, 0, 0, 50, 50) == base * 4); // nearest upscale: exactly 4x
}

namespace {

// Opaque, near-black pixels (the hand fill). Thresholds, not exact values: nanosvg anti-aliases.
int countDark(const mosaic::common::Image& img) {
    int n = 0;
    for (std::size_t p = 0; p + 3 < img.rgba.size(); p += 4)
        if (img.rgba[p + 3] > 200 && img.rgba[p] < 50 && img.rgba[p + 1] < 50 && img.rgba[p + 2] < 50)
            ++n;
    return n;
}

// Opaque, near-white pixels (the outline rim).
int countBright(const mosaic::common::Image& img) {
    int n = 0;
    for (std::size_t p = 0; p + 3 < img.rgba.size(); p += 4)
        if (img.rgba[p + 3] > 200 && img.rgba[p] > 210 && img.rgba[p + 1] > 210 &&
            img.rgba[p + 2] > 210)
            ++n;
    return n;
}

} // namespace

TEST_CASE("panCursor: apple_cursor hands (white fill / black outline), hotspot, HiDPI, open!=closed") {
    const CursorImage grab = panCursor(/*grabbing=*/false); // hand1 -> open hand
    CHECK(grab.image.width == 28);
    CHECK(grab.image.height == 28);
    CHECK(grab.hotX == 15); // apple hand1 hotspot 134/257 of the box
    CHECK(grab.hotY == 9);  // 81/257 of the box
    CHECK(countBright(grab.image) > 80); // the white hand fill...
    CHECK(countDark(grab.image) > 8);    // ...inside the black outline + finger strokes

    const CursorImage grabbing = panCursor(/*grabbing=*/true); // move -> closed fist
    CHECK(grabbing.image.width == 28);
    CHECK(countBright(grabbing.image) > 80);
    // The open palm and the closed fist are different shapes.
    CHECK(countBright(grab.image) != countBright(grabbing.image));

    const CursorImage big = panCursor(/*grabbing=*/false, 2); // HiDPI: 2x box, hotspot follows
    CHECK(big.image.width == 56);
    CHECK(big.hotX == 29);
    CHECK(big.hotY == 18);
}

TEST_CASE("rotateCursor: recoloured spun double-arrow, centred hotspot, HiDPI, theme + orientation") {
    const CursorImage c = rotateCursor(0.0, /*darkMode=*/false);
    REQUIRE(c.image.width > 0);
    CHECK(c.image.width == c.image.height);               // the spin canvas is square
    CHECK(c.hotX == static_cast<int>(c.image.width) / 2); // the hotspot (pivot) is its centre
    CHECK(c.hotY == static_cast<int>(c.image.height) / 2);

    // Light mode = black outline / white inner: both opaque tones are present, on a transparent
    // ground (robust to nanosvg's edge antialiasing).
    bool anyLight = false;
    bool anyDark = false;
    for (std::size_t p = 0; p + 3 < c.image.rgba.size(); p += 4) {
        if (c.image.rgba[p + 3] < 200)
            continue;
        if (c.image.rgba[p] > 200)
            anyLight = true;
        if (c.image.rgba[p] < 60)
            anyDark = true;
    }
    CHECK(anyLight); // the white inner
    CHECK(anyDark);  // the black outline

    const CursorImage big = rotateCursor(0.0, false, 2); // HiDPI: 2x art -> a larger spin canvas
    CHECK(big.image.width > c.image.width);
    CHECK(big.hotX == static_cast<int>(big.image.width) / 2);

    // Theme matters: dark mode inverts the two-tone, so the bitmap differs (same geometry).
    CHECK(rotateCursor(0.0, true).image.width == c.image.width);
    CHECK(rotateCursor(0.0, true).image.rgba != c.image.rgba);

    // Orientation matters: a quarter-turn produces a different bitmap (same size, spun content).
    CHECK(rotateCursor(1.5707963, false).image.width == c.image.width);
    CHECK(rotateCursor(1.5707963, false).image.rgba != c.image.rgba);
}

TEST_CASE("textCursor: recoloured spun I-beam, centred hotspot, HiDPI, theme + orientation") {
    const CursorImage c = textCursor(0.0, /*darkMode=*/false);
    REQUIRE(c.image.width > 0);
    CHECK(c.image.width == c.image.height);               // square spin canvas
    CHECK(c.hotX == static_cast<int>(c.image.width) / 2); // hotspot (insertion point) at the centre
    CHECK(c.hotY == static_cast<int>(c.image.height) / 2);

    // Light mode = black outline / white inner: both opaque tones present on a transparent ground.
    bool anyLight = false;
    bool anyDark = false;
    for (std::size_t p = 0; p + 3 < c.image.rgba.size(); p += 4) {
        if (c.image.rgba[p + 3] < 200)
            continue;
        if (c.image.rgba[p] > 200)
            anyLight = true;
        if (c.image.rgba[p] < 60)
            anyDark = true;
    }
    CHECK(anyLight); // the white inner
    CHECK(anyDark);  // the black outline

    const CursorImage big = textCursor(0.0, false, 2); // HiDPI: a larger canvas, same centred hotspot
    CHECK(big.image.width > c.image.width);
    CHECK(big.hotX == static_cast<int>(big.image.width) / 2);

    // Theme inverts the two-tone; orientation spins the glyph -- both change the bitmap, same size.
    CHECK(textCursor(0.0, true).image.width == c.image.width);
    CHECK(textCursor(0.0, true).image.rgba != c.image.rgba);
    CHECK(textCursor(1.5707963, false).image.width == c.image.width);
    CHECK(textCursor(1.5707963, false).image.rgba != c.image.rgba);
}

TEST_CASE("rotateCursor: rotation survives a comma-decimal locale") {
    // Regression: the rotation is baked into the SVG as a `rotate(...)` transform. Formatting the
    // angle with std::to_string(double) would emit the locale decimal separator (',' in many
    // non-English locales), which nanosvg mis-parses as an argument separator -> the cursor silently
    // stops rotating. Guard it by switching to a comma-decimal locale (if one is installed) and
    // confirming distinct angles still produce distinct glyphs.
    std::string saved = std::setlocale(LC_ALL, nullptr) ? std::setlocale(LC_ALL, nullptr) : "C";
    for (const char* l : {"de_DE.UTF-8", "fr_FR.UTF-8", "es_ES.UTF-8", "nl_NL.UTF-8"})
        if (std::setlocale(LC_ALL, l))
            break; // a comma-decimal locale is active (skipped silently if none are installed)
    CHECK(rotateCursor(0.0, false).image.rgba != rotateCursor(1.5707963, false).image.rgba);
    std::setlocale(LC_ALL, saved.c_str());
}

TEST_CASE("fitTextCursor: the apple_cursor hand2 fit-to-path hand, hotspot at the fingertip") {
    const CursorImage hand = fitTextCursor();
    CHECK(hand.image.width == 28);
    CHECK(hand.image.height == 28);
    CHECK(countBright(hand.image) > 80); // the white palm fill...
    CHECK(countDark(hand.image) > 8);    // ...inside the black outline + finger strokes
    // The fingertip hotspot sits in the upper-middle of the box, inside it.
    CHECK(hand.hotX > 0);
    CHECK(hand.hotX < 28);
    CHECK(hand.hotY >= 0);
    CHECK(hand.hotY < 14);
    // ... and it is a DIFFERENT shape from both pan hands (a click affordance, not a grab).
    CHECK(countBright(hand.image) != countBright(panCursor(false).image));
    CHECK(countBright(hand.image) != countBright(panCursor(true).image));

    const CursorImage big = fitTextCursor(2); // HiDPI: the box and the hotspot both scale
    CHECK(big.image.width == 56);
    CHECK(big.hotX == 2 * hand.hotX + (big.hotX % 2)); // rounding may add the half-step
}

// ---- The device/logical split (S59-a) ------------------------------------------------------
//
// Every builder reports the LOGICAL box it was asked for alongside the device bitmap, because FLTK
// (and the Wayland compositor under it) re-applies the buffer scale to whatever it is handed: pass
// the device size + device hotspot and the cursor renders at scale x its intended size with an
// off-centre hotspot scale x too far from the art. The invariant these cases pin is that the
// logical numbers are SCALE-INVARIANT while the device ones track the scale.

TEST_CASE("CursorImage: the logical box is scale-invariant; the device box tracks the scale") {
    const CursorImage sel1 = selectionCursor(SelectOp::Add, 1);
    const CursorImage sel2 = selectionCursor(SelectOp::Add, 2);
    CHECK(sel1.logicalW == 25);
    CHECK(sel1.logicalH == 25);
    CHECK(sel2.logicalW == 25); // ...unchanged at 2x, while the bitmap doubled
    CHECK(sel2.image.width == 50);
    CHECK(sel1.logicalHotX == 12);
    CHECK(sel2.logicalHotX == 12);
    CHECK(sel2.logicalHotY == 12);
    CHECK(sel2.hotX == 25); // the DEVICE hotspot still follows the upscale

    // The two off-centre hotspots -- the pair a device/logical mix-up actually shows up on.
    const CursorImage pan1 = panCursor(/*grabbing=*/false, 1);
    const CursorImage pan2 = panCursor(/*grabbing=*/false, 2);
    CHECK(pan1.logicalW == 28);
    CHECK(pan2.logicalW == 28);
    CHECK(pan2.image.width == 56);
    CHECK(pan1.logicalHotX == 15); // apple hand1: 134/257 of a 28 box
    CHECK(pan1.logicalHotY == 9);  // 81/257 of it
    CHECK(pan2.logicalHotX == 15); // NOT 29 -- that is pan2.hotX, the device value
    CHECK(pan2.logicalHotY == 9);
    CHECK(pan2.hotX == 29);
    CHECK(pan2.hotY == 18);

    const CursorImage fit1 = fitTextCursor(1);
    const CursorImage fit2 = fitTextCursor(2);
    CHECK(fit1.logicalW == 28);
    CHECK(fit2.logicalW == 28);
    CHECK(fit2.logicalHotX == fit1.logicalHotX);
    CHECK(fit2.logicalHotY == fit1.logicalHotY);

    // The two SVG cursors rasterize at a (possibly fractional) device scale; their logical box is
    // the 24-square both are authored on, and the hotspot is its centre.
    const CursorImage rot1 = rotateCursor(0.0, false, 1.0);
    const CursorImage rot2 = rotateCursor(0.0, false, 2.0);
    CHECK(rot1.logicalW == 24);
    CHECK(rot1.logicalH == 24);
    CHECK(rot2.logicalW == 24);
    CHECK(rot2.image.width == 48);
    CHECK(rot1.logicalHotX == 12);
    CHECK(rot2.logicalHotX == 12);
    CHECK(rot2.logicalHotY == 12);
    CHECK(rot2.hotX == 24);

    const CursorImage txt2 = textCursor(0.0, false, 2.0);
    CHECK(txt2.logicalW == 24);
    CHECK(txt2.logicalHotX == 12);
    CHECK(txt2.image.width == 48);

    // At scale 1 -- X11 and macOS, where cursorBuildScale() is pinned to 1 -- the two coordinate
    // systems coincide, so the whole mechanism is an identity there.
    for (const CursorImage& c : {sel1, pan1, fit1, rot1}) {
        CHECK(c.logicalW == static_cast<int>(c.image.width));
        CHECK(c.logicalH == static_cast<int>(c.image.height));
        CHECK(c.logicalHotX == c.hotX);
        CHECK(c.logicalHotY == c.hotY);
    }
}

TEST_CASE("CursorImage: FLTK's own arithmetic on the logical numbers reproduces the device art") {
    // ⚠ THE ROUND TRIP, pinned end to end -- the one the offset-hotspot report was really about.
    // The canvas hands FLTK the LOGICAL box (Fl_RGB_Image::scale(logicalW, logicalH)) and the
    // LOGICAL hotspot; Fl_Wayland_Window_Driver::set_cursor_4args then computes its own device
    // numbers back out as `rgb->w() * buffer_scale` and `hot * buffer_scale`, and draws our
    // full-resolution data into that box. So for the compositor's hotspot to land on the art pixel
    // the builder aimed at, `logicalHot * scale` has to come back to `hotX/hotY`. It cannot be
    // exact -- each is rounded independently against its own box -- but it must not DRIFT: an error
    // that GROWS with the scale is precisely the bug (the pan hands were ~10-15 px out at 2x).
    // ONE DEVICE PIXEL is the whole budget, at every scale, and that is what the builders deliver.
    const auto roundTrips = [](const CursorImage& c, int scale, const char* what) {
        INFO(what << " at scale " << scale);
        REQUIRE(c.image.width > 0);
        CHECK(static_cast<int>(c.image.width) == c.logicalW * scale);
        CHECK(static_cast<int>(c.image.height) == c.logicalH * scale);
        CHECK(std::abs(c.logicalHotX * scale - c.hotX) <= 1);
        CHECK(std::abs(c.logicalHotY * scale - c.hotY) <= 1);
        // ... and the hotspot must sit INSIDE the art in both systems, or the compositor clamps it
        // somewhere nobody chose.
        CHECK(c.hotX >= 0);
        CHECK(c.hotY >= 0);
        CHECK(c.hotX < static_cast<int>(c.image.width));
        CHECK(c.hotY < static_cast<int>(c.image.height));
        CHECK(c.logicalHotX < c.logicalW);
        CHECK(c.logicalHotY < c.logicalH);
    };
    for (const int s : {1, 2, 3}) {
        roundTrips(selectionCursor(SelectOp::Replace, s), s, "selectionCursor");
        roundTrips(selectionCursor(SelectOp::Intersect, s), s, "selectionCursor(badge)");
        roundTrips(panCursor(/*grabbing=*/false, s), s, "panCursor(grab)");
        roundTrips(panCursor(/*grabbing=*/true, s), s, "panCursor(grabbing)");
        roundTrips(fitTextCursor(s), s, "fitTextCursor");
        roundTrips(rotateCursor(0.4, /*darkMode=*/false, s), s, "rotateCursor");
        roundTrips(textCursor(0.4, /*darkMode=*/true, s), s, "textCursor");
        roundTrips(nwseCursor(/*darkMode=*/false, s), s, "nwseCursor");
        roundTrips(neswCursor(/*darkMode=*/true, s), s, "neswCursor");
        roundTrips(moveCursor(/*darkMode=*/false, s), s, "moveCursor");
    }
    // A FRACTIONAL device scale (the SVG builders accept one; a 1.5x output is real) still has to
    // leave the LOGICAL numbers alone -- they describe the box FLTK is told about, which never
    // depends on how crisply we rasterized into it.
    const CursorImage rot15 = rotateCursor(0.0, /*darkMode=*/false, 1.5);
    CHECK(rot15.logicalW == 24);
    CHECK(rot15.logicalH == 24);
    CHECK(rot15.logicalHotX == 12);
    CHECK(rot15.logicalHotY == 12);
    CHECK(rot15.image.width == 36); // 24 * 1.5, rasterized crisp
    // A scale BELOW 1 is clamped, never honoured: a sub-logical cursor bitmap would be blurred up
    // by the compositor and its hotspot rounded into a different pixel.
    const CursorImage rotTiny = rotateCursor(0.0, /*darkMode=*/false, 0.25);
    CHECK(rotTiny.image.width == 24);
    CHECK(rotTiny.logicalW == 24);
    CHECK(rotTiny.hotX == rotTiny.logicalHotX);
    const CursorImage panTiny = panCursor(/*grabbing=*/false, 0);
    CHECK(panTiny.image.width == 28);
    CHECK(panTiny.logicalHotX == panTiny.hotX);
}

namespace {

// Opaque mass within `band` px of the image's MAIN diagonal (top-left -> bottom-right) and of its
// ANTI-diagonal (top-right -> bottom-left). A straight arrow aimed along one of them piles almost
// all of its pixels into that count -- an orientation check that doesn't depend on the exact art.
void diagonalMass(const mosaic::common::Image& img, int& mainDiag, int& antiDiag, int band = 2) {
    mainDiag = 0;
    antiDiag = 0;
    const int n = static_cast<int>(img.width);
    for (int y = 0; y < static_cast<int>(img.height); ++y)
        for (int x = 0; x < n; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
            if (img.rgba[p + 3] <= 200)
                continue;
            if (std::abs(x - y) <= band)
                ++mainDiag;
            if (std::abs(x + y - (n - 1)) <= band)
                ++antiDiag;
        }
}

} // namespace

TEST_CASE("nwse/neswCursor: the diagonal resize arrows lie on their own diagonal") {
    const CursorImage nwse = nwseCursor(/*darkMode=*/false);
    const CursorImage nesw = neswCursor(/*darkMode=*/false);
    REQUIRE(nwse.image.width > 0);
    REQUIRE(nesw.image.width > 0);
    CHECK(nwse.image.width == nwse.image.height); // square spin canvas, like the rotate cursor
    CHECK(nwse.hotX == static_cast<int>(nwse.image.width) / 2); // centred hotspot
    CHECK(nwse.hotY == static_cast<int>(nwse.image.height) / 2);
    CHECK(nwse.logicalW == 24);
    CHECK(nwse.logicalHotX == 12);
    CHECK(nesw.logicalW == 24);
    CHECK(nesw.logicalHotY == 12);

    // Orientation: screen y grows DOWNWARD, so NW->SE runs along the MAIN diagonal and NE->SW along
    // the anti-diagonal. This is the assertion that catches a flipped sign.
    int mainDiag = 0;
    int antiDiag = 0;
    diagonalMass(nwse.image, mainDiag, antiDiag);
    CHECK(mainDiag > antiDiag);
    diagonalMass(nesw.image, mainDiag, antiDiag);
    CHECK(antiDiag > mainDiag);

    // ...and the two are genuinely different bitmaps, both distinct from the horizontal arrow.
    CHECK(nwse.image.rgba != nesw.image.rgba);
    CHECK(nwse.image.rgba != rotateCursor(0.0, false).image.rgba);

    // Same art family as the rotate cursor: +-45 deg of it, so the theme two-tone applies and the
    // exact angle convention is pinned here rather than left to a comment.
    CHECK(nwse.image.rgba == rotateCursor(0.78539816339744830962, false).image.rgba);
    CHECK(nesw.image.rgba == rotateCursor(-0.78539816339744830962, false).image.rgba);
    CHECK(nwseCursor(/*darkMode=*/true).image.rgba != nwse.image.rgba);

    // HiDPI: a larger device bitmap, the same logical box and centred hotspot.
    const CursorImage big = nwseCursor(false, 2.0);
    CHECK(big.image.width == 48);
    CHECK(big.logicalW == 24);
    CHECK(big.logicalHotX == 12);
    CHECK(big.hotX == 24);
}

namespace {

// Opaque mass within `band` px of the image's horizontal centre ROW and of its vertical centre
// COLUMN. A FOUR-way arrow puts comparable weight on both; a single double-arrow puts nearly all of
// it on one. Art-independent, like diagonalMass above.
void axisMass(const mosaic::common::Image& img, int& horiz, int& vert, int band = 2) {
    horiz = 0;
    vert = 0;
    const int cx = static_cast<int>(img.width) / 2;
    const int cy = static_cast<int>(img.height) / 2;
    for (int y = 0; y < static_cast<int>(img.height); ++y)
        for (int x = 0; x < static_cast<int>(img.width); ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
            if (img.rgba[p + 3] <= 200)
                continue;
            if (std::abs(y - cy) <= band)
                ++horiz;
            if (std::abs(x - cx) <= band)
                ++vert;
        }
}

} // namespace

TEST_CASE("moveCursor: a four-way arrow, not a hand -- both axes, centred hotspot, theme + HiDPI") {
    const CursorImage mv = moveCursor(/*darkMode=*/false);
    REQUIRE(mv.image.width > 0);
    CHECK(mv.image.width == mv.image.height); // square canvas, like the rest of the arrow family
    CHECK(mv.logicalW == 24);
    CHECK(mv.logicalH == 24);
    CHECK(mv.logicalHotX == 12); // the hotspot is the art's centre, where the four heads meet
    CHECK(mv.logicalHotY == 12);
    CHECK(mv.hotX == static_cast<int>(mv.image.width) / 2);
    CHECK(mv.hotY == static_cast<int>(mv.image.height) / 2);

    // Light mode = black outline / white inner on a transparent ground (the rotate cursor's pair).
    bool anyLight = false;
    bool anyDark = false;
    for (std::size_t p = 0; p + 3 < mv.image.rgba.size(); p += 4) {
        if (mv.image.rgba[p + 3] < 200)
            continue;
        if (mv.image.rgba[p] > 200)
            anyLight = true;
        if (mv.image.rgba[p] < 60)
            anyDark = true;
    }
    CHECK(anyLight);
    CHECK(anyDark);

    // The load-bearing shape assertion: FOUR ways, not two. The move arrow carries comparable mass
    // on both centre axes, where the horizontal double-arrow (the rotate art at 0 rad) has almost
    // none on the vertical one -- so this cannot silently degrade into another double-arrow.
    int h = 0;
    int v = 0;
    axisMass(mv.image, h, v);
    CHECK(h > 0);
    CHECK(v > 0);
    CHECK(v * 3 > h);
    CHECK(h * 3 > v);
    int rh = 0;
    int rv = 0;
    axisMass(rotateCursor(0.0, false).image, rh, rv);
    CHECK(rv * 3 < rh);

    // And it is not one of the HANDS. That is the whole point of the substitution: FLTK asks the
    // Wayland backend for the Xcursor name `move`, which breeze_cursors symlinks to `dnd-move` -- a
    // closed grabbing hand -- so merely hovering a Move-tool selection announced a drag in the pan
    // gesture's vocabulary. Nor is it the diagonal resize pair or the rotate arrow.
    CHECK(mv.image.rgba != panCursor(/*grabbing=*/true).image.rgba);
    CHECK(mv.image.rgba != panCursor(/*grabbing=*/false).image.rgba);
    CHECK(mv.image.rgba != nwseCursor(false).image.rgba);
    CHECK(mv.image.rgba != rotateCursor(0.0, false).image.rgba);

    // Theme inverts the two-tone; HiDPI grows the device bitmap and leaves the logical box alone.
    CHECK(moveCursor(/*darkMode=*/true).image.rgba != mv.image.rgba);
    const CursorImage big = moveCursor(false, 2.0);
    CHECK(big.image.width == 48);
    CHECK(big.logicalW == 24);
    CHECK(big.logicalHotX == 12);
    CHECK(big.hotX == 24);
}
