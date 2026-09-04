#include "ui/cursors.hpp"

#include "common/image_svg.hpp"

#include <algorithm>
#include <assets/cursor_fit_text_svg.hpp> // generated: ...::cursor_fit_text_svg[] (apple hand2)
#include <assets/cursor_grab_svg.hpp> // generated: mosaic::assets::cursor_grab_svg[] (apple hand1)
#include <assets/cursor_grabbing_svg.hpp> // generated: ...::cursor_grabbing_svg[] (apple move)
#include <assets/move_cursor_svg.hpp>     // generated: ...::move_cursor_svg[] (apple all-scroll)
#include <assets/rotate_cursor_svg.hpp>   // generated: ...::rotate_cursor_svg[] (Move-tool rotate)
#include <assets/text_cursor_svg.hpp>     // generated: ...::text_cursor_svg[] (Type-tool I-beam)
#include <assets/zoom_in_cursor_svg.hpp>  // generated: ...::zoom_in_cursor_svg[] (apple zoom-in)
#include <assets/zoom_out_cursor_svg.hpp> // generated: ...::zoom_out_cursor_svg[] (apple zoom-out)
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mosaic::ui {
namespace {

constexpr int kSize = 25;          // logical cursor canvas (odd: an exact centre pixel)
constexpr int kCenter = kSize / 2; // the crosshair centre = the hotspot
constexpr int kArm = 8;            // crosshair arm length, from the centre
constexpr int kBadge = 16;         // badge box top-left (x and y)
constexpr int kGlyph = 7;          // badge glyphs are kGlyph x kGlyph

using Points = std::vector<std::pair<int, int>>;

void addCross(Points& pts) {
    for (int d = -kArm; d <= kArm; ++d) {
        pts.emplace_back(kCenter + d, kCenter);
        pts.emplace_back(kCenter, kCenter + d);
    }
}

void addBadge(Points& pts, core::SelectOp op) {
    const int mid = kGlyph / 2;
    switch (op) {
    case core::SelectOp::Add: // +
        for (int i = 0; i < kGlyph; ++i) {
            pts.emplace_back(kBadge + i, kBadge + mid);
            pts.emplace_back(kBadge + mid, kBadge + i);
        }
        break;
    case core::SelectOp::Subtract: // -
        for (int i = 0; i < kGlyph; ++i)
            pts.emplace_back(kBadge + i, kBadge + mid);
        break;
    case core::SelectOp::Intersect: // x
        for (int i = 0; i < kGlyph; ++i) {
            pts.emplace_back(kBadge + i, kBadge + i);
            pts.emplace_back(kBadge + kGlyph - 1 - i, kBadge + i);
        }
        break;
    case core::SelectOp::Replace:
        break; // the plain crosshair
    }
}

void put(common::Image& img, int x, int y, common::Color8 c) {
    if (x < 0 || y < 0 || x >= static_cast<int>(img.width) || y >= static_cast<int>(img.height))
        return;
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    img.rgba[p] = c.r;
    img.rgba[p + 1] = c.g;
    img.rgba[p + 2] = c.b;
    img.rgba[p + 3] = c.a;
}

} // namespace

CursorImage selectionCursor(core::SelectOp op, int scale) {
    if (scale < 1)
        scale = 1;
    Points core;
    addCross(core);
    addBadge(core, op);

    common::Image base(kSize, kSize); // zero-initialised: fully transparent ground
    // The 1-px black halo first (the 8-neighbourhood of every core pixel), then the white core
    // on top -- a two-tone line that stays visible over any canvas content.
    for (const auto& [x, y] : core)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                put(base, x + dx, y + dy, {0, 0, 0, 255});
    for (const auto& [x, y] : core)
        put(base, x, y, {255, 255, 255, 255});

    if (scale == 1)
        return {std::move(base), kCenter, kCenter, kSize, kSize, kCenter, kCenter};

    // Nearest-neighbour upscale for HiDPI; the hotspot moves to the centre pixel's centre.
    common::Image big(static_cast<std::uint32_t>(kSize * scale),
                      static_cast<std::uint32_t>(kSize * scale));
    for (std::uint32_t y = 0; y < big.height; ++y) {
        for (std::uint32_t x = 0; x < big.width; ++x) {
            const std::size_t src =
                (static_cast<std::size_t>(y / scale) * base.width + x / scale) * 4;
            const std::size_t dst = (static_cast<std::size_t>(y) * big.width + x) * 4;
            for (int ch = 0; ch < 4; ++ch)
                big.rgba[dst + ch] = base.rgba[src + ch];
        }
    }
    const int hot = kCenter * scale + scale / 2;
    return {std::move(big), hot, hot, kSize, kSize, kCenter, kCenter};
}

namespace {

// The hands are the vendored apple_cursor SVGs (third_party/apple_cursor): white fill + black
// outline, drawn on a 257-unit square canvas (the SVG viewBox). nanosvg ignores their drop-shadow
// <filter>, which is what we want for a cursor. The hotspots are apple_cursor's own X11 values
// (configs/x.build.toml: hand1 grab, move grabbing), expressed in that 257 canvas.
constexpr double kAppleBox = 257.0;
constexpr double kGrabHotX = 134.0;     // hand1 -> open "grab"
constexpr double kGrabHotY = 81.0;
constexpr double kGrabbingHotX = 139.0; // move  -> closed "grabbing"
constexpr double kGrabbingHotY = 86.0;
constexpr int kHandSize = 28; // logical cursor box (on a par with selectionCursor's 25)

} // namespace

CursorImage panCursor(bool grabbing, int scale) {
    if (scale < 1)
        scale = 1;
    const int size = kHandSize * scale;
    const unsigned char* svg =
        grabbing ? mosaic::assets::cursor_grabbing_svg : mosaic::assets::cursor_grab_svg;
    const std::size_t len =
        grabbing ? mosaic::assets::cursor_grabbing_svg_size : mosaic::assets::cursor_grab_svg_size;
    common::Image img = common::rasterizeSvg(svg, len, size, size);
    const double hx = grabbing ? kGrabbingHotX : kGrabHotX;
    const double hy = grabbing ? kGrabbingHotY : kGrabHotY;
    const int hotX = static_cast<int>(hx / kAppleBox * size + 0.5);
    const int hotY = static_cast<int>(hy / kAppleBox * size + 0.5);
    // The same hotspot in the LOGICAL box (derived from kHandSize, not hotX/scale, so the rounding
    // is done once against the art rather than twice against a rounded value). These hands are the
    // two OFF-CENTRE hotspots in the set, so they are exactly the pair a device/logical mix-up
    // shows up on -- see CursorImage.
    const int logicalHotX = static_cast<int>(hx / kAppleBox * kHandSize + 0.5);
    const int logicalHotY = static_cast<int>(hy / kAppleBox * kHandSize + 0.5);
    return {std::move(img), hotX, hotY, kHandSize, kHandSize, logicalHotX, logicalHotY};
}

CursorImage fitTextCursor(int scale) {
    if (scale < 1)
        scale = 1;
    const int size = kHandSize * scale;
    common::Image img = common::rasterizeSvg(mosaic::assets::cursor_fit_text_svg,
                                             mosaic::assets::cursor_fit_text_svg_size, size, size);
    // hand2's raised index fingertip in its 257 canvas (the finger column tops out around
    // (96, 44)); apple_cursor ships no X11 hotspot for it under that name, so this is read off
    // the art the way the pan hands' were verified.
    const int hotX = static_cast<int>(96.0 / kAppleBox * size + 0.5);
    const int hotY = static_cast<int>(44.0 / kAppleBox * size + 0.5);
    const int logicalHotX = static_cast<int>(96.0 / kAppleBox * kHandSize + 0.5);
    const int logicalHotY = static_cast<int>(44.0 / kAppleBox * kHandSize + 0.5);
    return {std::move(img), hotX, hotY, kHandSize, kHandSize, logicalHotX, logicalHotY};
}

namespace {

// The rotate cursor reuses the vendored apple_cursor left_side double-arrow (embedded as
// rotate_cursor_svg). The art ships BLUE (#0000FF) for the outline shape and GREEN (#00FF00) for the
// inner shape; we recolour those to a per-theme two-tone (light = black outline / white inner; dark =
// the reverse) so it reads on any canvas. nanosvg drops the drop-shadow <filter>, as wanted. The
// arrow lies along +-x at rest; the caller passes the screen direction it should point along, and the
// rotation is BAKED INTO THE SVG so nanosvg renders the turned art in one antialiased pass (crisp --
// a bitmap spin blurred it).
constexpr int kRotateSize = 24;          // logical cursor box (the arrow is wide + thin)
constexpr double kRotateHotFrac = 0.50;  // hotspot = the arrow's centre (the rotation pivot)

// Replace every occurrence of `from` with `to` in `s` (both 6-digit hex here, so length is preserved).
void replaceAll(std::string& s, std::string_view from, std::string_view to) {
    for (std::size_t i = s.find(from); i != std::string::npos; i = s.find(from, i + to.size()))
        s.replace(i, from.size(), to);
}

// Strip every  attr="..."  (and a leading space) from `s` -- used to drop the I-beam's clip-path so
// a rotated cursor's ends are not cropped by its 256-square clip rect (§6.1).
void stripAttr(std::string& s, std::string_view attr) {
    const std::string key = std::string(attr) + "=\"";
    for (std::size_t i = s.find(key); i != std::string::npos; i = s.find(key)) {
        const std::size_t close = s.find('"', i + key.size());
        if (close == std::string::npos) break;
        const std::size_t start = (i > 0 && s[i - 1] == ' ') ? i - 1 : i;
        s.erase(start, close + 1 - start);
    }
}

// The Type tool's I-beam (mirrors rotateCursor): a wide-but-thin glyph drawn upright (vertical) at
// rest, so the baked rotation IS the baseline angle (no +90 offset). Bucketed by the caller.
constexpr int kTextSize = 24;          // logical cursor box
constexpr double kTextHotFrac = 0.50;  // hotspot = the viewBox centre = the I-beam's insertion point

} // namespace

CursorImage rotateCursor(double angleRad, bool darkMode, double scale) {
    // Rasterize at the device resolution (scale may be fractional, e.g. 1.5) so the arrow is crisp at
    // any HiDPI factor instead of a 1x raster the compositor has to blur up to fit.
    const int size = std::max(1, static_cast<int>(std::lround(kRotateSize * std::max(1.0, scale))));
    std::string svg(reinterpret_cast<const char*>(mosaic::assets::rotate_cursor_svg),
                    mosaic::assets::rotate_cursor_svg_size);
    // Recolour to the theme's two-tone: blue (#0000FF) = outline, green (#00FF00) = inner.
    replaceAll(svg, "#0000FF", darkMode ? "#FFFFFF" : "#000000");
    replaceAll(svg, "#00FF00", darkMode ? "#000000" : "#FFFFFF");
    // Bake the rotation into the SVG (rotate the outer group about the art centre 128.5) so nanosvg
    // renders the turned arrow in one AA pass. The arrow lies along +-x at rest; rotate(deg) aims it
    // along angleRad. Use INTEGER degrees + a literal centre so the string is locale-independent:
    // std::to_string(double) emits the locale decimal separator (',' in many non-English locales),
    // which nanosvg mis-parses as an argument separator and the rotation silently breaks. (Bucketed,
    // so integer degrees are plenty.) Max arrow radius (~115 of 257) < 128.5, so it never clips.
    constexpr double kPi = 3.14159265358979323846;
    const long deg = std::lround(angleRad * 180.0 / kPi);
    if (const std::size_t g = svg.find("<g "); g != std::string::npos)
        svg.insert(g + 3, "transform=\"rotate(" + std::to_string(deg) + " 128.5 128.5)\" ");
    common::Image img = common::rasterizeSvg(
        reinterpret_cast<const unsigned char*>(svg.data()), svg.size(), size, size);
    if (img.rgba.empty())
        return {common::Image{}, 0, 0}; // parse/raster failure: the caller falls back to a stock cursor
    const int hot = static_cast<int>(kRotateHotFrac * size + 0.5);
    const int logicalHot = static_cast<int>(kRotateHotFrac * kRotateSize + 0.5);
    return {std::move(img), hot, hot, kRotateSize, kRotateSize, logicalHot, logicalHot};
}

CursorImage textCursor(double angleRad, bool darkMode, double scale) {
    const int size = std::max(1, static_cast<int>(std::lround(kTextSize * std::max(1.0, scale))));
    std::string svg(reinterpret_cast<const char*>(mosaic::assets::text_cursor_svg),
                    mosaic::assets::text_cursor_svg_size);
    // Theme two-tone: blue (#0000FF) = outline, green (#00FF00) = inner -- the same placeholders the
    // rotate cursor recolours, so a text cursor reads on any canvas content.
    replaceAll(svg, "#0000FF", darkMode ? "#FFFFFF" : "#000000");
    replaceAll(svg, "#00FF00", darkMode ? "#000000" : "#FFFFFF");
    // Drop the clip-path so the turned I-beam's tips are not cropped by its 256-square clip rect.
    stripAttr(svg, "clip-path");
    // Bake the baseline rotation about the viewBox centre (257x256 -> 128.5,128). Integer degrees,
    // space-separated args, literal centre -- locale-independent (nanosvg mis-parses a ',' decimal).
    constexpr double kPi = 3.14159265358979323846;
    const long deg = std::lround(angleRad * 180.0 / kPi);
    if (const std::size_t g = svg.find("<g "); g != std::string::npos)
        svg.insert(g + 3, "transform=\"rotate(" + std::to_string(deg) + " 128.5 128)\" ");
    common::Image img = common::rasterizeSvg(
        reinterpret_cast<const unsigned char*>(svg.data()), svg.size(), size, size);
    if (img.rgba.empty())
        return {common::Image{}, 0, 0};  // parse/raster failure: caller falls back to a stock cursor
    const int hot = static_cast<int>(kTextHotFrac * size + 0.5);
    const int logicalHot = static_cast<int>(kTextHotFrac * kTextSize + 0.5);
    return {std::move(img), hot, hot, kTextSize, kTextSize, logicalHot, logicalHot};
}

namespace {

// A box diagonal is 45 deg off the axes. Screen y grows DOWNWARD, so the NW->SE direction is +45
// deg and NE->SW is -45 deg. Spelled as a literal (not pi/4) so no local `kPi` has to escape the
// two functions that already define one, and so rotateCursor's integer-degree bake lands on an
// exact +-45 with no rounding to argue about.
constexpr double kDiagonalRad = 0.78539816339744830962;

} // namespace

CursorImage nwseCursor(bool darkMode, double scale) {
    // The diagonal resize arrows ARE the rotate cursor's double-arrow, aimed instead of swept --
    // so they inherit its recolouring, its baked-rotation crispness and its centred hotspot for
    // free, and a corner handle reads as the same family as the rotate band beside it.
    return rotateCursor(kDiagonalRad, darkMode, scale);
}

CursorImage neswCursor(bool darkMode, double scale) {
    return rotateCursor(-kDiagonalRad, darkMode, scale);
}

namespace {

// The four-way move arrow is the vendored apple_cursor `all-scroll` (embedded as move_cursor_svg),
// drawn on the same 257 canvas and in the same two placeholder colours as the double-arrow the
// rotate/resize cursors use -- blue (#0000FF) outline, green (#00FF00) inner -- so it recolours
// through the identical two lines and comes out of the same family. The glyph is centred in the
// canvas (it spans ~25..230 on both axes), so the hotspot is the box centre, like every other
// arrow in the set. No rotation to bake: this one points four ways at once.
constexpr int kMoveSize = 24;         // logical cursor box -- the same as kRotateSize, so a box's
constexpr double kMoveHotFrac = 0.50; // move, resize and rotate cursors match on screen

} // namespace

CursorImage moveCursor(bool darkMode, double scale) {
    const int size = std::max(1, static_cast<int>(std::lround(kMoveSize * std::max(1.0, scale))));
    std::string svg(reinterpret_cast<const char*>(mosaic::assets::move_cursor_svg),
                    mosaic::assets::move_cursor_svg_size);
    replaceAll(svg, "#0000FF", darkMode ? "#FFFFFF" : "#000000");
    replaceAll(svg, "#00FF00", darkMode ? "#000000" : "#FFFFFF");
    common::Image img = common::rasterizeSvg(
        reinterpret_cast<const unsigned char*>(svg.data()), svg.size(), size, size);
    if (img.rgba.empty())
        return {common::Image{}, 0, 0}; // parse/raster failure: caller falls back to a stock cursor
    const int hot = static_cast<int>(kMoveHotFrac * size + 0.5);
    const int logicalHot = static_cast<int>(kMoveHotFrac * kMoveSize + 0.5);
    return {std::move(img), hot, hot, kMoveSize, kMoveSize, logicalHot, logicalHot};
}

namespace {

// The magnifiers are the vendored apple_cursor zoom-in / zoom-out glyphs, on the same 257 canvas
// and in the same two placeholder colours as the arrows above. The lens is drawn UP-LEFT of the
// canvas centre with its handle running down-right, so unlike every other cursor here the hotspot
// is not the box centre -- it is the lens centre, read off each glyph's own circle (the `fill`
// ellipse path: cx ~100.26/98.36 for zoom-in, ~100.46/100.92 for zoom-out).
constexpr int kZoomSize = 26; // logical cursor box (the lens + its handle need the room)
constexpr double kZoomInHotX = 100.26;
constexpr double kZoomInHotY = 98.36;
constexpr double kZoomOutHotX = 100.46;
constexpr double kZoomOutHotY = 100.92;

} // namespace

CursorImage zoomCursor(bool out, bool darkMode, double scale) {
    const int size = std::max(1, static_cast<int>(std::lround(kZoomSize * std::max(1.0, scale))));
    std::string svg(reinterpret_cast<const char*>(out ? mosaic::assets::zoom_out_cursor_svg
                                                      : mosaic::assets::zoom_in_cursor_svg),
                    out ? mosaic::assets::zoom_out_cursor_svg_size
                        : mosaic::assets::zoom_in_cursor_svg_size);
    replaceAll(svg, "#0000FF", darkMode ? "#FFFFFF" : "#000000");
    replaceAll(svg, "#00FF00", darkMode ? "#000000" : "#FFFFFF");
    common::Image img = common::rasterizeSvg(reinterpret_cast<const unsigned char*>(svg.data()),
                                             svg.size(), size, size);
    if (img.rgba.empty())
        return {common::Image{}, 0, 0}; // parse/raster failure: caller falls back to a stock cursor
    const double hx = out ? kZoomOutHotX : kZoomInHotX;
    const double hy = out ? kZoomOutHotY : kZoomInHotY;
    // Device px and the logical box separately, each rounded once against the art -- the off-centre
    // hotspot rule at CursorImage (a lens is exactly the case a device/logical mix-up shows up on).
    const int hotX = static_cast<int>(hx / kAppleBox * size + 0.5);
    const int hotY = static_cast<int>(hy / kAppleBox * size + 0.5);
    const int logicalHotX = static_cast<int>(hx / kAppleBox * kZoomSize + 0.5);
    const int logicalHotY = static_cast<int>(hy / kAppleBox * kZoomSize + 0.5);
    return {std::move(img), hotX, hotY, kZoomSize, kZoomSize, logicalHotX, logicalHotY};
}

} // namespace mosaic::ui
