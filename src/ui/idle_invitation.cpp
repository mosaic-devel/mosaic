#include "ui/idle_invitation.hpp"

#include "common/i18n.hpp"
#include "common/image_svg.hpp"

#include <assets/app_icon_svg.hpp>

#include <FL/Fl.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace mosaic::ui {
namespace {

// The settled copy (option A + the drag-over headline swap). N_() marks them for extraction
// without translating here -- these are constexpr, evaluated long before i18n::init() runs, so the
// lookup has to happen at the point of use (every read below goes through _()). The bake is built
// on demand, well after start-up, so it picks up the active catalog.
constexpr const char* kHeadline = N_("Open an image");
constexpr const char* kHeadlineHot = N_("Drop it anywhere");
constexpr const char* kHintDrop = N_("click anywhere, or drop a file into this window");
// TRANSLATORS: "File > New" names the menu path -- use the same wording as the File and New menu
// entries in this catalog so the hint points at something the user can actually see.
constexpr const char* kHintNew = N_("File > New starts a blank canvas");

constexpr int kIconLogicalPx = 96; // the faded app-icon watermark (logical px)

int scaled(double v, double s) {
    return std::max(1, static_cast<int>(std::lround(v * s)));
}

common::Color8 mix8(common::Color8 a, common::Color8 b, double t) {
    const auto ch = [t](std::uint8_t ca, std::uint8_t cb) {
        return static_cast<std::uint8_t>(std::lround(ca + (cb - ca) * t));
    };
    return {ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), ch(a.a, b.a)};
}

// One text line of the bake: position is the line box's top-left; the baseline is derived from
// the face metrics at draw time (fl_height/fl_descent), exactly as the old EmptyStateView drew.
struct BakeLine {
    std::string text;
    Fl_Font face;
    int fontPx;
    int x; // line box top-left, device px
    int yTop;
};

// Rasterize `lines` white-on-black at device resolution and return per-pixel coverage. FLTK's
// offscreen surface is RGB-only (the rasterizeOverlayTile lesson), so colour is applied later
// by tintCoverage; grayscale coverage tinted in CPU is exactly grayscale AA -- no subpixel
// fringing over the animated field. Empty result if the surface yields no image (no display).
std::vector<float> renderTextCoverage(const std::vector<BakeLine>& lines, int w, int h) {
    Fl_Image_Surface surf(w, h);
    Fl_Surface_Device::push_current(&surf);
    fl_color(FL_BLACK);
    fl_rectf(0, 0, w, h);
    fl_color(FL_WHITE);
    for (const BakeLine& line : lines) {
        fl_font(line.face, line.fontPx);
        fl_draw(line.text.c_str(), line.x, line.yTop + fl_height() - fl_descent());
    }
    Fl_RGB_Image* img = surf.image();
    Fl_Surface_Device::pop_current();
    if (img == nullptr)
        return {};

    std::vector<float> cov(static_cast<std::size_t>(w) * h, 0.0f);
    const auto* src = static_cast<const std::uint8_t*>(static_cast<const void*>(img->array));
    const int d = img->d() > 0 ? img->d() : 3;
    const int ld = img->ld() != 0 ? img->ld() : img->w() * d;
    const int cw = std::min(w, img->w());
    const int ch = std::min(h, img->h());
    for (int yy = 0; yy < ch; ++yy)
        for (int xx = 0; xx < cw; ++xx)
            cov[static_cast<std::size_t>(yy) * w + xx] =
                static_cast<float>(src[yy * ld + xx * d]) / 255.0f;
    delete img;
    return cov;
}

void blendPixel(common::Image& img, int x, int y, common::Color8 color, double alpha) {
    if (x < 0 || y < 0 || x >= static_cast<int>(img.width) || y >= static_cast<int>(img.height) ||
        alpha <= 0.0)
        return;
    std::uint8_t* p = &img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4];
    const double sa = std::clamp(alpha, 0.0, 1.0);
    const double da = p[3] / 255.0;
    const double outA = sa + da * (1.0 - sa);
    if (outA <= 0.0)
        return;
    for (int c = 0; c < 3; ++c) {
        const double sc = (c == 0 ? color.r : c == 1 ? color.g : color.b) / 255.0;
        const double dc = p[c] / 255.0;
        p[c] = static_cast<std::uint8_t>(
            std::lround(255.0 * (sc * sa + dc * da * (1.0 - sa)) / outA));
    }
    p[3] = static_cast<std::uint8_t>(std::lround(255.0 * outA));
}

} // namespace

void compositeOver(common::Image& img, const common::Image& src, int x, int y) {
    for (std::uint32_t sy = 0; sy < src.height; ++sy) {
        for (std::uint32_t sx = 0; sx < src.width; ++sx) {
            const std::uint8_t* sp = &src.rgba[(static_cast<std::size_t>(sy) * src.width + sx) * 4];
            if (sp[3] == 0)
                continue;
            blendPixel(img, x + static_cast<int>(sx), y + static_cast<int>(sy),
                       {sp[0], sp[1], sp[2], 255}, sp[3] / 255.0);
        }
    }
}

void fadeAlpha(common::Image& img, double factor) {
    const double f = std::clamp(factor, 0.0, 1.0);
    for (std::size_t i = 3; i < img.rgba.size(); i += 4)
        img.rgba[i] = static_cast<std::uint8_t>(std::lround(img.rgba[i] * f));
}

void tintCoverageBand(common::Image& img, const std::vector<float>& cov, int covW, int y0, int y1,
                      common::Color8 color) {
    const double ca = color.a / 255.0;
    const int yEnd = std::min(y1, static_cast<int>(cov.size() / std::max(1, covW)));
    for (int yy = std::max(0, y0); yy < yEnd; ++yy)
        for (int xx = 0; xx < covW; ++xx) {
            const float c = cov[static_cast<std::size_t>(yy) * covW + xx];
            if (c > 0.0f)
                blendPixel(img, xx, yy, color, c * ca);
        }
}

void drawInvitationFrame(common::Image& img, double x, double y, double w, double h,
                         double radius, double stroke, double dashOn, double dashOff,
                         common::Color8 color) {
    // Analytic coverage against pixel centres; only the stroke's cross section is anti-aliased
    // (dash ends stay crisp, like fl_line_style's). ONE dash pattern flows around the whole
    // perimeter -- straight runs and quarter-arc corners share a single clockwise arc-length
    // parameter (feedback 2026-07-23: solid corners read as a mistake next to dashed runs),
    // and the period is snapped so the perimeter holds a whole number of dashes: no stub at
    // the seam, and the pattern closes on itself.
    constexpr double kPi = 3.14159265358979323846;
    const double half = stroke * 0.5;
    const double cxL = x + radius, cxR = x + w - radius;
    const double cyT = y + radius, cyB = y + h - radius;
    const double runW = cxR - cxL, runH = cyB - cyT;
    const double arc = 0.5 * kPi * radius;
    const double perimeter = 2.0 * (runW + runH) + 4.0 * arc;
    const double nominal = std::max(1.0, dashOn + dashOff);
    const double duty = dashOn / nominal;
    const double period = perimeter / std::max(1.0, std::round(perimeter / nominal));
    // Clockwise arc-length starts, from the top-left/top junction.
    const double sTR = runW;             // top run ends, TR corner arc begins
    const double sRight = sTR + arc;     // right run, top -> bottom
    const double sBR = sRight + runH;    // BR corner arc
    const double sBottom = sBR + arc;    // bottom run, right -> left
    const double sBL = sBottom + runW;   // BL corner arc
    const double sLeft = sBL + arc;      // left run, bottom -> top
    const double sTL = sLeft + runH;     // TL corner arc (closes at `perimeter`)

    const auto band = [half](double d) { return std::clamp(half + 0.5 - std::abs(d), 0.0, 1.0); };
    const int x0 = std::max(0, static_cast<int>(std::floor(x - half - 1)));
    const int y0 = std::max(0, static_cast<int>(std::floor(y - half - 1)));
    const int x1 = std::min(static_cast<int>(img.width), static_cast<int>(std::ceil(x + w + half + 2)));
    const int y1 = std::min(static_cast<int>(img.height), static_cast<int>(std::ceil(y + h + half + 2)));
    const double ca = color.a / 255.0;

    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            const double cx = px + 0.5, cy = py + 0.5;
            double cov = 0.0;
            const auto add = [&](double d, double s) {
                const double b = band(d);
                if (b > cov && std::fmod(s, period) < duty * period)
                    cov = b;
            };
            if (cx >= cxL && cx <= cxR) {
                add(cy - y, cx - cxL);                 // top run
                add(cy - (y + h), sBottom + (cxR - cx)); // bottom run
            }
            if (cy >= cyT && cy <= cyB) {
                add(cx - (x + w), sRight + (cy - cyT)); // right run
                add(cx - x, sLeft + (cyB - cy));        // left run
            }
            // Corner arcs: the angle from the arc's entry junction (clockwise) scales by the
            // radius into the same arc-length parameter the runs use.
            if (cx > cxR && cy < cyT) { // TR: from the top junction toward the right one
                const double vx = cx - cxR, vy = cy - cyT;
                add(std::hypot(vx, vy) - radius, sTR + std::atan2(vx, -vy) * radius);
            } else if (cx > cxR && cy > cyB) { // BR
                const double vx = cx - cxR, vy = cy - cyB;
                add(std::hypot(vx, vy) - radius, sBR + std::atan2(vy, vx) * radius);
            } else if (cx < cxL && cy > cyB) { // BL
                const double vx = cx - cxL, vy = cy - cyB;
                add(std::hypot(vx, vy) - radius, sBL + std::atan2(-vx, vy) * radius);
            } else if (cx < cxL && cy < cyT) { // TL
                const double vx = cx - cxL, vy = cy - cyT;
                add(std::hypot(vx, vy) - radius, sTL + std::atan2(-vy, -vx) * radius);
            }
            if (cov > 0.0)
                blendPixel(img, px, py, color, cov * ca);
        }
    }
}

InvitationBake bakeInvitationAtlas(const Palette& pal, double scale) {
    InvitationBake bake;
    const double s = scale > 0.0 ? scale : 1.0;

    // Fonts + metrics (device px throughout; the sizes are the EmptyStateView's, scaled).
    const int headlinePx = scaled(16, s);
    const int hintDropPx = scaled(13, s);
    const int hintNewPx = scaled(12, s);
    fl_font(FL_HELVETICA_BOLD, headlinePx);
    const int headlineW = static_cast<int>(std::ceil(fl_width(_(kHeadline))));
    const int headlineHotW = static_cast<int>(std::ceil(fl_width(_(kHeadlineHot))));
    const int headlineH = fl_height();
    fl_font(FL_HELVETICA, hintDropPx);
    const int hintDropW = static_cast<int>(std::ceil(fl_width(_(kHintDrop))));
    const int hintDropH = fl_height();
    fl_font(FL_HELVETICA, hintNewPx);
    const int hintNewW = static_cast<int>(std::ceil(fl_width(_(kHintNew))));
    const int hintNewH = fl_height();
    if (headlineW <= 0 || headlineH <= 0)
        return bake; // no usable font metrics (headless): the field renders bare

    const int iconPx = scaled(kIconLogicalPx, s);
    const int iconToText = scaled(20, s);
    const int lineGap = scaled(6, s);
    const int framePadX = scaled(44, s);
    const int framePadY = scaled(30, s);
    const int margin = scaled(4, s); // AA slack so the frame never clips at the quad edge
    const double radius = 10.0 * s;

    const int blockW =
        std::max({iconPx, headlineW, headlineHotW, hintDropW, hintNewW});
    const int blockH = iconPx + iconToText + headlineH + lineGap + hintDropH + lineGap + hintNewH;
    bake.rowW = blockW + 2 * framePadX + 2 * margin;
    bake.rowH = blockH + 2 * framePadY + 2 * margin;

    // The watermark icon, shared by every row.
    std::string err;
    common::Image icon = common::rasterizeSvg(assets::app_icon_svg, assets::app_icon_svg_size,
                                              iconPx, iconPx, &err);
    if (!icon.empty())
        fadeAlpha(icon, 0.45); // scenery, not a button

    bake.atlas = common::Image(static_cast<std::uint32_t>(bake.rowW),
                               static_cast<std::uint32_t>(bake.rowH * InvitationBake::kRows));

    for (int row = 0; row < InvitationBake::kRows; ++row) {
        const bool hot = row == 2;
        common::Image rowImg(static_cast<std::uint32_t>(bake.rowW),
                             static_cast<std::uint32_t>(bake.rowH));

        // The dashed frame ladder: quiet at rest, darker on hover, accent (and heavier) while a
        // drag hovers. On the LIGHT ground the palette border nearly vanishes against canvasBg
        // (user report, 2026-07-23), so light mode pulls the resting frame toward textMuted and
        // the hover frame toward text -- the ladder keeps its three distinct steps.
        const common::Color8 frameRest =
            pal.dark ? pal.border : mix8(pal.border, pal.textMuted, 0.55);
        const common::Color8 frameHover =
            pal.dark ? pal.textMuted : mix8(pal.textMuted, pal.text, 0.30);
        const common::Color8 frameColor = hot ? pal.accent : (row == 1 ? frameHover : frameRest);
        const double stroke = (hot ? 2.0 : 1.2) * s;
        drawInvitationFrame(rowImg, margin, margin, bake.rowW - 2.0 * margin,
                            bake.rowH - 2.0 * margin, radius, stroke, (hot ? 8.0 : 5.0) * s,
                            (hot ? 6.0 : 4.0) * s, frameColor);

        const int yIcon = margin + framePadY;
        if (!icon.empty())
            compositeOver(rowImg, icon, (bake.rowW - iconPx) / 2, yIcon);

        // Text: coverage baked white-on-black at final positions, tinted per line.
        const char* headline = hot ? _(kHeadlineHot) : _(kHeadline);
        fl_font(FL_HELVETICA_BOLD, headlinePx);
        const int hw = static_cast<int>(std::ceil(fl_width(headline)));
        const int yHeadline = yIcon + iconPx + iconToText;
        const int yHintDrop = yHeadline + headlineH + lineGap;
        const int yHintNew = yHintDrop + hintDropH + lineGap;
        const std::vector<BakeLine> lines = {
            {headline, FL_HELVETICA_BOLD, headlinePx, (bake.rowW - hw) / 2, yHeadline},
            {_(kHintDrop), FL_HELVETICA, hintDropPx, (bake.rowW - hintDropW) / 2, yHintDrop},
            {_(kHintNew), FL_HELVETICA, hintNewPx, (bake.rowW - hintNewW) / 2, yHintNew},
        };
        const std::vector<float> cov = renderTextCoverage(lines, bake.rowW, bake.rowH);
        if (!cov.empty()) {
            // Tint by line band (lines never overlap): headline in text, hints in textMuted.
            tintCoverageBand(rowImg, cov, bake.rowW, yHeadline, yHintDrop, pal.text);
            tintCoverageBand(rowImg, cov, bake.rowW, yHintDrop, yHintNew + hintNewH + lineGap,
                             pal.textMuted);
        }

        // Stack the finished row into the atlas.
        for (int yy = 0; yy < bake.rowH; ++yy) {
            const std::size_t srcOff = static_cast<std::size_t>(yy) * bake.rowW * 4;
            const std::size_t dstOff =
                (static_cast<std::size_t>(row * bake.rowH + yy) * bake.rowW) * 4;
            std::copy_n(rowImg.rgba.begin() + static_cast<std::ptrdiff_t>(srcOff),
                        static_cast<std::size_t>(bake.rowW) * 4,
                        bake.atlas.rgba.begin() + static_cast<std::ptrdiff_t>(dstOff));
        }
    }
    return bake;
}

} // namespace mosaic::ui
