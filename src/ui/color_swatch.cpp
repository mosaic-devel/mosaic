#include "ui/color_swatch.hpp"

#include "common/image.hpp"
#include "ui/color_picker.hpp"
#include "ui/color_state.hpp"
#include "ui/theme.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/fl_draw.H>

namespace mosaic::ui {
namespace {

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

// Local-coordinate layout of the swatch (designed for kSwatchW x kSwatchH). The two colour chips
// overlap diagonally; the swap and reset glyphs sit clear of them in the spare corners. draw() and
// swatchHitRegion() share these so the visuals and hit-testing never drift apart.
struct IRect {
    int x;
    int y;
    int w;
    int h;
    [[nodiscard]] bool contains(int px, int py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

constexpr IRect kFgRect{2, 2, 20, 20};    // foreground chip (top-left, drawn on top)
constexpr IRect kBgRect{12, 14, 20, 20};  // background chip (bottom-right, behind)
constexpr IRect kSwapRect{24, 1, 9, 9};   // swap glyph (top-right)
constexpr IRect kResetRect{1, 30, 9, 9};  // default-reset glyph (bottom-left)

// Draw a colour chip: fill, then a two-tone 1px border so its edge reads against any fill colour
// (dark outer + light inner). `front` chips get the full two-tone; the half-hidden back chip a single
// themed hairline.
void drawChip(int ox, int oy, const IRect& r, common::Color8 fill, const Palette& pal, bool front) {
    const int rx = ox + r.x;
    const int ry = oy + r.y;
    fl_color(toFl(fill));
    fl_rectf(rx, ry, r.w, r.h);
    if (front) {
        fl_color(fl_rgb_color(40, 40, 40));
        fl_rect(rx, ry, r.w, r.h);
        fl_color(fl_rgb_color(225, 225, 225));
        fl_rect(rx + 1, ry + 1, r.w - 2, r.h - 2);
    } else {
        fl_color(toFl(pal.border));
        fl_rect(rx, ry, r.w, r.h);
    }
}

// A small double-headed diagonal arrow = "swap".
void drawSwapGlyph(int ox, int oy, const Palette& pal) {
    const int gx = ox + kSwapRect.x;
    const int gy = oy + kSwapRect.y;
    fl_color(toFl(pal.textMuted));
    fl_line_style(FL_SOLID, 1);
    fl_line(gx + 1, gy + 7, gx + 7, gy + 1); // shaft, bottom-left -> top-right
    fl_line(gx + 7, gy + 1, gx + 4, gy + 1); // top-right arrowhead: horizontal barb
    fl_line(gx + 7, gy + 1, gx + 7, gy + 5); // ... and its vertical barb, one px longer so the head reads symmetric
    fl_line(gx + 1, gy + 7, gx + 5, gy + 7); // bottom-left arrowhead: horizontal barb, one px longer to match the top-right head on-device
    fl_line(gx + 1, gy + 7, gx + 1, gy + 3); // ... and its vertical barb
    fl_line_style(0);
}

// Two tiny overlapping squares (black over white) = the default-colours reset.
void drawResetGlyph(int ox, int oy) {
    const int gx = ox + kResetRect.x;
    const int gy = oy + kResetRect.y;
    fl_color(fl_rgb_color(255, 255, 255)); // white (behind, lower-right)
    fl_rectf(gx + 2, gy + 2, 5, 5);
    fl_color(fl_rgb_color(120, 120, 120));
    fl_rect(gx + 2, gy + 2, 5, 5);
    fl_color(fl_rgb_color(0, 0, 0)); // black (front, upper-left)
    fl_rectf(gx, gy, 5, 5);
    fl_color(fl_rgb_color(120, 120, 120));
    fl_rect(gx, gy, 5, 5);
}

} // namespace

SwatchRegion swatchHitRegion(int localX, int localY, int w, int h) {
    if (localX < 0 || localY < 0 || localX >= w || localY >= h)
        return SwatchRegion::None;
    // Priority: the small corner glyphs sit above the chips; the foreground chip is above the
    // background chip where they overlap.
    if (kSwapRect.contains(localX, localY))
        return SwatchRegion::Swap;
    if (kResetRect.contains(localX, localY))
        return SwatchRegion::Reset;
    if (kFgRect.contains(localX, localY))
        return SwatchRegion::Foreground;
    if (kBgRect.contains(localX, localY))
        return SwatchRegion::Background;
    return SwatchRegion::None;
}

ColorSwatch::ColorSwatch(int X, int Y, int W, int H, ColorState& colors)
    : Fl_Widget(X, Y, W, H), m_colors(colors) {
    tooltip("Foreground / background color\nClick front: pick · back / X: swap · corner / D: reset");
    m_colors.addObserver([this] { onColorsChanged(); });
}

ColorSwatch::~ColorSwatch() = default;

void ColorSwatch::draw() {
    const Palette& pal = activePalette();
    fl_color(toFl(pal.panelBg)); // clear to the toolbar ground (erases the prior chips)
    fl_rectf(x(), y(), w(), h());

    // Greyed with the rest of the chrome during an inpaint run: blend the chips toward the panel
    // ground and drop the swap/reset affordances (they are interactive and won't respond).
    const bool enabled = active_r();
    auto muted = [&](common::Color8 c) -> common::Color8 {
        if (enabled)
            return c;
        const common::Color8 g = pal.panelBg;
        return {static_cast<unsigned char>((c.r * 45 + g.r * 55) / 100),
                static_cast<unsigned char>((c.g * 45 + g.g * 55) / 100),
                static_cast<unsigned char>((c.b * 45 + g.b * 55) / 100), 255};
    };

    drawChip(x(), y(), kBgRect, muted(m_colors.background()), pal, /*front=*/false);
    drawChip(x(), y(), kFgRect, muted(m_colors.foreground()), pal, /*front=*/true);
    if (enabled) {
        drawSwapGlyph(x(), y(), pal);
        drawResetGlyph(x(), y());
    }
}

int ColorSwatch::handle(int event) {
    switch (event) {
    case FL_ENTER:
        fl_cursor(FL_CURSOR_HAND);
        return 1; // claim ENTER so FLTK also delivers LEAVE
    case FL_LEAVE:
        fl_cursor(FL_CURSOR_DEFAULT);
        return 1;
    case FL_PUSH:
        switch (swatchHitRegion(Fl::event_x() - x(), Fl::event_y() - y(), w(), h())) {
        case SwatchRegion::Foreground:
            openPicker();
            break;
        case SwatchRegion::Background:
        case SwatchRegion::Swap:
            m_colors.swap();
            break;
        case SwatchRegion::Reset:
            m_colors.reset();
            break;
        case SwatchRegion::None:
            break;
        }
        return 1; // the whole widget is ours
    default:
        return Fl_Widget::handle(event);
    }
}

void ColorSwatch::openPicker() {
    // The picker is built once by the main window (a child sub-window) and handed to us via
    // attachPicker(); we only toggle it. Clicking the foreground square with it open closes it.
    if (m_picker == nullptr)
        return;
    if (m_picker->shown())
        m_picker->hide();
    else
        m_picker->showFor(this);
}

void ColorSwatch::onColorsChanged() {
    redraw();
    if (m_picker && m_picker->shown())
        m_picker->syncFromState(); // keep an open picker in step with X / reset / back-square swaps
}

} // namespace mosaic::ui
