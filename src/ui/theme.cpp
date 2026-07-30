#include "ui/theme.hpp"

#include "platform/system_theme.hpp"

#include <FL/Fl.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Tooltip.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>

namespace mosaic::ui {
namespace {

// The palette the custom boxtype draw callbacks read. They are plain C function pointers with
// no user-data, so the active palette lives here. applyTheme() keeps it in sync.
Palette g_active = darkPalette();

// Theme-change observers (S51-a ③). Kept in a token->callback map so a widget can remove its own
// entry on destruction without disturbing the others. `g_themeApplied` gates the FIRST applyTheme()
// (startup) from firing observers + a redraw, when no widgets exist yet.
std::map<int, std::function<void()>> g_themeObservers;
int g_nextThemeToken = 1;
bool g_themeApplied = false;

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

constexpr int kCornerRadius = 5;

void drawFlatBox(int x, int y, int w, int h, Fl_Color c) {
    fl_color(c);
    fl_rectf(x, y, w, h);
}

void drawPanelBox(int x, int y, int w, int h, Fl_Color c) {
    fl_color(c);
    fl_rectf(x, y, w, h);
    fl_color(toFl(g_active.border));
    fl_rect(x, y, w, h);
}

void drawButtonUpBox(int x, int y, int w, int h, Fl_Color c) {
    fl_color(c);
    fl_rounded_rectf(x, y, w, h, kCornerRadius);
    fl_color(toFl(g_active.border));
    fl_rounded_rect(x, y, w, h, kCornerRadius);
}

void drawButtonDownBox(int x, int y, int w, int h, Fl_Color c) {
    fl_color(c);
    fl_rounded_rectf(x, y, w, h, kCornerRadius);
    fl_color(toFl(g_active.accent)); // accent outline marks the pressed/active state
    fl_rounded_rect(x, y, w, h, kCornerRadius);
}

// The painter bound for the duration of one window draw (see ScopedChromePainter). Drawing is
// single-threaded in FLTK, so a plain pair of globals is enough; the binder still saves/restores
// so a window drawn from inside another's draw would nest correctly.
ScopedChromePainter::Fn g_chromeFn = nullptr;
void* g_chromeUserData = nullptr;

void drawChromeBox(int /*x*/, int /*y*/, int /*w*/, int /*h*/, Fl_Color /*c*/) {
    if (g_chromeFn != nullptr)
        g_chromeFn(g_chromeUserData);
}

void registerBoxtypesOnce() {
    static bool done = false;
    if (done)
        return;
    done = true;

    const Fl_Boxtype base = FL_FREE_BOXTYPE;
    MOSAIC_FLAT_BOX = static_cast<Fl_Boxtype>(base + 0);
    MOSAIC_PANEL_BOX = static_cast<Fl_Boxtype>(base + 1);
    MOSAIC_INPUT_BOX = static_cast<Fl_Boxtype>(base + 2);
    MOSAIC_BUTTON_UP_BOX = static_cast<Fl_Boxtype>(base + 3);
    MOSAIC_BUTTON_DOWN_BOX = static_cast<Fl_Boxtype>(base + 4);
    MOSAIC_CHROME_BOX = static_cast<Fl_Boxtype>(base + 5);
    // No interior inset: the bound painter owns the whole window rect (it draws the popover's own
    // frame), and a nonzero inset would shift every child.
    Fl::set_boxtype(MOSAIC_CHROME_BOX, drawChromeBox, 0, 0, 0, 0);

    // (boxtype, draw fn, dx, dy, dw, dh): the d* values are the interior inset FLTK reserves
    // for the widget's label/children, i.e. the border thickness.
    Fl::set_boxtype(MOSAIC_FLAT_BOX, drawFlatBox, 0, 0, 0, 0);
    Fl::set_boxtype(MOSAIC_PANEL_BOX, drawPanelBox, 1, 1, 2, 2);
    // Same 1px frame as the panel box, but a wider horizontal interior inset so an input's text
    // doesn't kiss the outline (FLTK insets the text by dx/dw). 4px each side clears the narrowest
    // field (the 34px colour readouts) without clipping a 3-digit value.
    Fl::set_boxtype(MOSAIC_INPUT_BOX, drawPanelBox, 4, 1, 8, 2);
    Fl::set_boxtype(MOSAIC_BUTTON_UP_BOX, drawButtonUpBox, 2, 1, 4, 2);
    Fl::set_boxtype(MOSAIC_BUTTON_DOWN_BOX, drawButtonDownBox, 2, 1, 4, 2);
}

} // namespace

// Sensible built-in fallbacks until applyTheme() swaps in the custom boxtypes.
Fl_Boxtype MOSAIC_FLAT_BOX = FL_FLAT_BOX;
Fl_Boxtype MOSAIC_PANEL_BOX = FL_BORDER_BOX;
Fl_Boxtype MOSAIC_INPUT_BOX = FL_BORDER_BOX;
Fl_Boxtype MOSAIC_BUTTON_UP_BOX = FL_UP_BOX;
Fl_Boxtype MOSAIC_BUTTON_DOWN_BOX = FL_DOWN_BOX;
Fl_Boxtype MOSAIC_CHROME_BOX = FL_NO_BOX;

void seedOpaqueWindowShape(Fl_Window& win, std::vector<unsigned char>& buf,
                           std::unique_ptr<Fl_RGB_Image>& img) {
    buf.assign(static_cast<std::size_t>(win.w()) * static_cast<std::size_t>(win.h()) * 4, 255);
    img = std::make_unique<Fl_RGB_Image>(buf.data(), win.w(), win.h(), 4);
    win.shape(img.get());
}

ScopedChromePainter::ScopedChromePainter(Fn fn, void* userData) noexcept
    : m_prevFn(g_chromeFn), m_prevUserData(g_chromeUserData) {
    g_chromeFn = fn;
    g_chromeUserData = userData;
}

ScopedChromePainter::~ScopedChromePainter() {
    g_chromeFn = m_prevFn;
    g_chromeUserData = m_prevUserData;
}

std::string_view themeModeKey(ThemeMode mode) {
    switch (mode) {
        case ThemeMode::System: return "system";
        case ThemeMode::Dark: return "dark";
        case ThemeMode::Light: return "light";
    }
    return "system";
}

std::optional<ThemeMode> parseThemeMode(std::string_view key) {
    if (key == "system" || key == "System") return ThemeMode::System;
    if (key == "dark" || key == "Dark") return ThemeMode::Dark;
    if (key == "light" || key == "Light") return ThemeMode::Light;
    return std::nullopt;
}

Palette darkPalette() {
    Palette p;
    p.dark = true;
    p.windowBg = {30, 33, 46, 255};
    p.panelBg = {37, 41, 56, 255};
    p.canvasBg = {23, 27, 43, 255}; // matches the app-icon ground
    p.controlBg = {49, 54, 72, 255};
    p.controlHover = {61, 67, 88, 255};
    p.controlActive = {73, 80, 104, 255};
    p.controlSelected = {48, 53, 71, 255}; // ~45% of the way from panelBg to controlHover
    p.text = {229, 231, 240, 255};
    p.textMuted = {149, 156, 179, 255};
    p.accent = {94, 126, 255, 255}; // the app-icon blue (#5E7EFF)
    p.onAccent = {255, 255, 255, 255};
    p.border = {54, 60, 79, 255};
    p.tooltipBg = {44, 49, 66, 255};
    p.tooltipText = {229, 231, 240, 255};
    return p;
}

Palette lightPalette() {
    Palette p;
    p.dark = false;
    p.windowBg = {240, 241, 245, 255};
    p.panelBg = {248, 249, 251, 255};
    p.canvasBg = {223, 226, 232, 255};
    p.controlBg = {255, 255, 255, 255};
    p.controlHover = {236, 238, 243, 255};
    p.controlActive = {222, 226, 236, 255};
    // The same fraction of a much tighter ramp (the light theme spends only ~12 levels on hover),
    // so the tint stays proportional to the theme it lives in rather than to an absolute step.
    p.controlSelected = {242, 244, 248, 255};
    p.text = {28, 31, 42, 255};
    p.textMuted = {104, 112, 130, 255};
    p.accent = {74, 104, 237, 255};
    p.onAccent = {255, 255, 255, 255};
    p.border = {210, 214, 222, 255};
    p.tooltipBg = {44, 49, 66, 255};
    p.tooltipText = {244, 245, 248, 255};
    return p;
}

Palette resolvePalette(ThemeMode mode) {
    bool dark = true;
    if (mode == ThemeMode::Light) {
        dark = false;
    } else if (mode == ThemeMode::System) {
        // Pro creative app: default to dark unless the host explicitly prefers light.
        dark = platform::detectColorScheme() != platform::ColorScheme::Light;
    }

    Palette pal = dark ? darkPalette() : lightPalette();
    if (mode == ThemeMode::System) {
        if (const std::optional<common::Color8> accent = platform::detectAccentColor()) {
            pal.accent = *accent;
        }
    }
    return pal;
}

void applyTheme(const Palette& pal) {
    g_active = pal;
    registerBoxtypesOnce();

    // A flat base scheme; our custom boxtypes + color map do the rest.
    Fl::scheme("gtk+");

    // Global color map. background() also spreads the surrounding gray ramp; background2() is
    // the text/input field background; foreground() is label/text.
    Fl::background(pal.windowBg.r, pal.windowBg.g, pal.windowBg.b);
    Fl::background2(pal.controlBg.r, pal.controlBg.g, pal.controlBg.b);
    Fl::foreground(pal.text.r, pal.text.g, pal.text.b);
    Fl::set_color(FL_SELECTION_COLOR, pal.accent.r, pal.accent.g, pal.accent.b);
    Fl::set_color(FL_INACTIVE_COLOR, pal.textMuted.r, pal.textMuted.g, pal.textMuted.b);

    // Tooltips: themed surface, snappy delay, globally enabled.
    Fl_Tooltip::color(toFl(pal.tooltipBg));
    Fl_Tooltip::textcolor(toFl(pal.tooltipText));
    Fl_Tooltip::enable(1);
    Fl_Tooltip::delay(0.7f);      // initial hover before a tip shows (0.4 read as "instant")
    Fl_Tooltip::hoverdelay(0.5f); // and a tip does not re-pop the instant you slide to a neighbour
                                  // widget (sweeping the history rows otherwise flickers tips)

    // On a re-theme (every call after the first) let cached-colour widgets re-apply and repaint the
    // whole UI. The first call is startup -- no widgets, no observers -- so skip the churn there.
    if (g_themeApplied) {
        for (const auto& [token, cb] : g_themeObservers)
            if (cb)
                cb();
        Fl::redraw();
    }
    g_themeApplied = true;
}

const Palette& activePalette() {
    return g_active;
}

int addThemeObserver(std::function<void()> onThemeChanged) {
    const int token = g_nextThemeToken++;
    g_themeObservers.emplace(token, std::move(onThemeChanged));
    return token;
}

void removeThemeObserver(int token) {
    g_themeObservers.erase(token);
}

common::Image renderAAPrims(int originX, int originY, int w, int h,
                            const std::function<common::Color8(int x, int y)>& under,
                            const std::vector<AAPrim>& prims) {
    if (w <= 0 || h <= 0)
        return {};
    common::Image img(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    std::size_t i = 0;
    for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
            const common::Color8 u = under(originX + px, originY + py);
            double r = u.r;
            double g = u.g;
            double b = u.b;
            const double sx = originX + px + 0.5; // coverage against the pixel centre
            const double sy = originY + py + 0.5;
            for (const AAPrim& p : prims) {
                const double d = std::hypot(sx - p.cx, sy - p.cy);
                const double cov =
                    p.stroke <= 0.0
                        ? std::clamp(p.r - d + 0.5, 0.0, 1.0)
                        : std::clamp(p.stroke * 0.5 + 0.5 - std::abs(d - p.r), 0.0, 1.0);
                if (cov <= 0.0)
                    continue;
                r += (p.color.r - r) * cov;
                g += (p.color.g - g) * cov;
                b += (p.color.b - b) * cov;
            }
            img.rgba[i++] = static_cast<std::uint8_t>(std::lround(r));
            img.rgba[i++] = static_cast<std::uint8_t>(std::lround(g));
            img.rgba[i++] = static_cast<std::uint8_t>(std::lround(b));
            img.rgba[i++] = 255;
        }
    }
    return img;
}

void drawAAPrims(int originX, int originY, int w, int h,
                 const std::function<common::Color8(int x, int y)>& under,
                 const std::vector<AAPrim>& prims) {
    const common::Image img = renderAAPrims(originX, originY, w, h, under, prims);
    if (img.empty())
        return;
    fl_draw_image(img.rgba.data(), originX, originY, w, h, 4, 0); // opaque: alpha is padding
}

namespace {
common::Color8 mixColor(common::Color8 a, common::Color8 b, float t) {
    const auto ch = [t](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::lround(x + (y - x) * t));
    };
    return {ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), 255};
}
float segDistance(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax;
    const float vy = by - ay;
    const float wx = px - ax;
    const float wy = py - ay;
    const float len2 = vx * vx + vy * vy;
    const float t = len2 > 0.0F ? std::clamp((wx * vx + wy * vy) / len2, 0.0F, 1.0F) : 0.0F;
    const float dx = wx - t * vx;
    const float dy = wy - t * vy;
    return std::sqrt(dx * dx + dy * dy);
}
} // namespace

void drawBubbleTriangleLeft(int tri, int triH, int tipY, common::Color8 bodyBg,
                            common::Color8 marginBg, common::Color8 border) {
    const float tipX = 0.0F;
    const float tipYf = static_cast<float>(tipY);
    const float baseX = static_cast<float>(tri);
    const float topYc = tipY - triH / 2.0F;
    const float botYc = tipY + triH / 2.0F;
    const int y0 = tipY - triH / 2 - 1;
    const int ph = triH + 2;
    if (ph <= 0 || tri <= 0)
        return;
    std::vector<unsigned char> buf(static_cast<std::size_t>(tri) * ph * 3);
    const auto inTri = [&](float sx, float sy) {
        const float e0 = (sx - tipX) * (topYc - tipYf) - (sy - tipYf) * (baseX - tipX);
        const float e1 = (sx - baseX) * (botYc - topYc) - (sy - topYc) * (baseX - baseX);
        const float e2 = (sx - baseX) * (tipYf - botYc) - (sy - botYc) * (tipX - baseX);
        return (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
    };
    std::size_t i = 0;
    for (int row = 0; row < ph; ++row) {
        for (int col = 0; col < tri; ++col) {
            int inside = 0;
            for (int sj = 0; sj < 4; ++sj)
                for (int si = 0; si < 4; ++si)
                    if (inTri(col + (si + 0.5F) / 4.0F, y0 + row + (sj + 0.5F) / 4.0F))
                        ++inside;
            common::Color8 c = mixColor(marginBg, bodyBg, inside / 16.0F);
            const float cxp = col + 0.5F;
            const float cyp = y0 + row + 0.5F;
            const float bd = std::min(segDistance(cxp, cyp, tipX, tipYf, baseX, topYc),
                                      segDistance(cxp, cyp, tipX, tipYf, baseX, botYc));
            c = mixColor(c, border, std::clamp(1.0F - bd, 0.0F, 1.0F));
            buf[i++] = c.r;
            buf[i++] = c.g;
            buf[i++] = c.b;
        }
    }
    fl_draw_image(buf.data(), 0, y0, tri, ph, 3);
}

void drawBubbleTriangleRight(int xLeft, int tri, int triH, int tipY, common::Color8 bodyBg,
                             common::Color8 marginBg, common::Color8 border) {
    // Mirror of ...Left within a tri-wide strip: tip on the RIGHT (image x == tri), base on the LEFT
    // (image x == 0); the strip is blitted at window x `xLeft`.
    const float tipX = static_cast<float>(tri);
    const float tipYf = static_cast<float>(tipY);
    const float baseX = 0.0F;
    const float topYc = tipY - triH / 2.0F;
    const float botYc = tipY + triH / 2.0F;
    const int y0 = tipY - triH / 2 - 1;
    const int ph = triH + 2;
    if (ph <= 0 || tri <= 0)
        return;
    std::vector<unsigned char> buf(static_cast<std::size_t>(tri) * ph * 3);
    const auto inTri = [&](float sx, float sy) {
        const float e0 = (sx - tipX) * (topYc - tipYf) - (sy - tipYf) * (baseX - tipX);
        const float e1 = (sx - baseX) * (botYc - topYc) - (sy - topYc) * (baseX - baseX);
        const float e2 = (sx - baseX) * (tipYf - botYc) - (sy - botYc) * (tipX - baseX);
        return (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
    };
    std::size_t i = 0;
    for (int row = 0; row < ph; ++row) {
        for (int col = 0; col < tri; ++col) {
            int inside = 0;
            for (int sj = 0; sj < 4; ++sj)
                for (int si = 0; si < 4; ++si)
                    if (inTri(col + (si + 0.5F) / 4.0F, y0 + row + (sj + 0.5F) / 4.0F))
                        ++inside;
            common::Color8 c = mixColor(marginBg, bodyBg, inside / 16.0F);
            const float cxp = col + 0.5F;
            const float cyp = y0 + row + 0.5F;
            const float bd = std::min(segDistance(cxp, cyp, tipX, tipYf, baseX, topYc),
                                      segDistance(cxp, cyp, tipX, tipYf, baseX, botYc));
            c = mixColor(c, border, std::clamp(1.0F - bd, 0.0F, 1.0F));
            buf[i++] = c.r;
            buf[i++] = c.g;
            buf[i++] = c.b;
        }
    }
    fl_draw_image(buf.data(), xLeft, y0, tri, ph, 3);
}

void drawBubbleTriangleUp(int tri, int triW, int tipX, common::Color8 bodyBg,
                          common::Color8 marginBg, common::Color8 border) {
    const float tipY = 0.0F;
    const float tipXf = static_cast<float>(tipX);
    const float baseY = static_cast<float>(tri);
    const float leftXc = tipX - triW / 2.0F;
    const float rightXc = tipX + triW / 2.0F;
    const int x0 = tipX - triW / 2 - 1;
    const int pw = triW + 2;
    if (pw <= 0 || tri <= 0)
        return;
    std::vector<unsigned char> buf(static_cast<std::size_t>(pw) * tri * 3);
    const auto inTri = [&](float sx, float sy) { // tip(tipX,0), baseLeft(leftXc,tri), baseRight(rightXc,tri)
        const float e0 = (sx - tipXf) * (baseY - tipY) - (sy - tipY) * (leftXc - tipXf);
        const float e1 = (sx - leftXc) * (baseY - baseY) - (sy - baseY) * (rightXc - leftXc);
        const float e2 = (sx - rightXc) * (tipY - baseY) - (sy - baseY) * (tipXf - rightXc);
        return (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
    };
    std::size_t i = 0;
    for (int row = 0; row < tri; ++row) {
        for (int col = 0; col < pw; ++col) {
            int inside = 0;
            for (int sj = 0; sj < 4; ++sj)
                for (int si = 0; si < 4; ++si)
                    if (inTri(x0 + col + (si + 0.5F) / 4.0F, row + (sj + 0.5F) / 4.0F))
                        ++inside;
            common::Color8 c = mixColor(marginBg, bodyBg, inside / 16.0F);
            const float cxp = x0 + col + 0.5F;
            const float cyp = row + 0.5F;
            const float bd = std::min(segDistance(cxp, cyp, tipXf, tipY, leftXc, baseY),
                                      segDistance(cxp, cyp, tipXf, tipY, rightXc, baseY));
            c = mixColor(c, border, std::clamp(1.0F - bd, 0.0F, 1.0F));
            buf[i++] = c.r;
            buf[i++] = c.g;
            buf[i++] = c.b;
        }
    }
    fl_draw_image(buf.data(), x0, 0, pw, tri, 3);
}

void buildBubbleShapeMask(std::vector<unsigned char>& buf, int W, int H, int tri, int triH, int tipY,
                          bool rightSide) {
    buf.assign(static_cast<std::size_t>(std::max(0, W)) * std::max(0, H) * 4, 0);
    if (W <= 0 || H <= 0) return;
    const float half = triH / 2.0F;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            bool opaque = true;
            if (!rightSide && x < tri) {
                // Left margin: opaque only inside the triangle (tip at x==0, base at x==tri).
                const float h = half * (static_cast<float>(x) / tri);
                opaque = x >= 1 && static_cast<float>(y) >= tipY - h && static_cast<float>(y) <= tipY + h;
            } else if (rightSide && x >= W - tri) {
                // Right margin: tip at x==W-1, base at x==W-tri (mirror of the left case).
                const float h = half * (static_cast<float>(W - 1 - x) / tri);
                opaque = x <= W - 2 && static_cast<float>(y) >= tipY - h && static_cast<float>(y) <= tipY + h;
            }
            unsigned char* q = &buf[(static_cast<std::size_t>(y) * W + x) * 4];
            q[0] = q[1] = q[2] = 255;
            q[3] = opaque ? 255 : 0;
        }
    }
}

} // namespace mosaic::ui
