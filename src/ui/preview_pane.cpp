#include "ui/preview_pane.hpp"

#include "ui/theme.hpp"

#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mosaic::ui {

namespace {

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

// The in-canvas transparency checkerboard, matched to render::CompositeOptions' defaults.
constexpr int kCell = 8;
constexpr std::uint8_t kLight = 255;
constexpr std::uint8_t kDark = 205;

} // namespace

void PreviewPane::setImage(const common::Image& rgba, common::Rect canvasInImage) {
    if (rgba.empty()) {
        clearImage();
        return;
    }
    // Premultiply once: scaling premultiplied RGBA interpolates coverage correctly (no dark/bright
    // fringe at alpha edges), and compositing premultiplied over the opaque surround is a plain
    // rgb + surround*(1-a).
    m_premul = common::Image(rgba.width, rgba.height);
    for (std::size_t i = 0; i < rgba.rgba.size(); i += 4) {
        const float a = rgba.rgba[i + 3] / 255.0f;
        m_premul.rgba[i] = static_cast<std::uint8_t>(std::lround(rgba.rgba[i] * a));
        m_premul.rgba[i + 1] = static_cast<std::uint8_t>(std::lround(rgba.rgba[i + 1] * a));
        m_premul.rgba[i + 2] = static_cast<std::uint8_t>(std::lround(rgba.rgba[i + 2] * a));
        m_premul.rgba[i + 3] = rgba.rgba[i + 3];
    }
    m_canvasInImage = canvasInImage;
    m_hasImage = true;
    redraw();
}

void PreviewPane::clearImage() {
    m_hasImage = false;
    m_premul = {};
    redraw();
}

void PreviewPane::setNote(std::string note) {
    m_note = std::move(note);
    redraw();
}

void PreviewPane::draw() {
    const Palette& p = activePalette();
    const int W = w(), H = h();
    if (W <= 0 || H <= 0)
        return;

    // Empty state: a plain muted note on the panel ground (no live content to frame).
    if (!m_hasImage) {
        fl_color(toFl(p.panelBg));
        fl_rectf(x(), y(), W, H);
        if (!m_note.empty()) {
            fl_color(toFl(p.textMuted));
            fl_font(FL_HELVETICA, 12);
            fl_draw(m_note.c_str(), x() + 10, y(), W - 20, H, FL_ALIGN_CENTER | FL_ALIGN_WRAP);
        }
        fl_color(toFl(p.border));
        fl_rect(x(), y(), W, H);
        return;
    }

    // The preview is drawn INSET by the 1px frame, so the frame never overpaints content (which
    // used to drop a 1px edge line, and flicker it as the aspect-fit rescaled).
    const int iw = std::max(1, W - 2), ih = std::max(1, H - 2);

    // Content placement within the inner rect: aspect-fit. The host renders a region matched to the
    // pane aspect, so "fit" fills it; any residual sliver stays the off-canvas backdrop.
    const int sw = static_cast<int>(m_premul.width), sh = static_cast<int>(m_premul.height);
    const double s = std::min(double(iw) / sw, double(ih) / sh);
    const int dw = std::max(1, int(std::lround(sw * s)));
    const int dh = std::max(1, int(std::lround(sh * s)));
    const int ox = (iw - dw) / 2; // content origin, inner-local
    const int oy = (ih - dh) / 2;

    Fl_RGB_Image srcImg(m_premul.rgba.data(), sw, sh, 4);
    Fl_Image* scaled = srcImg.copy(dw, dh);
    const auto* sc = static_cast<Fl_RGB_Image*>(scaled);
    const unsigned char* sa = reinterpret_cast<const unsigned char*>(sc->array);
    const int sd = sc->d();
    int sld = sc->ld();
    if (sld == 0)
        sld = dw * sd;

    // The canvas region within the SOURCE image (integer pixels), as a 1-channel mask. Scaling THIS
    // by the same Fl_RGB_Image::copy(dw,dh) as the content means the mask's canvas edge lands on
    // exactly the same dest pixels as the content's -- so the checker/backdrop decision can never
    // leak a 1px sliver past the content at the canvas edge (the float-vs-FLTK-rounding seam this
    // used to show at the top/left). Independent of FLTK's scaling algorithm, since both go through
    // it identically.
    const int mx0 = std::clamp(int(std::lround(m_canvasInImage.x)), 0, sw);
    const int my0 = std::clamp(int(std::lround(m_canvasInImage.y)), 0, sh);
    const int mx1 = std::clamp(int(std::lround(m_canvasInImage.x + m_canvasInImage.w)), 0, sw);
    const int my1 = std::clamp(int(std::lround(m_canvasInImage.y + m_canvasInImage.h)), 0, sh);
    std::vector<unsigned char> mask(static_cast<std::size_t>(sw) * sh, 0);
    for (int my = my0; my < my1; ++my)
        std::fill(mask.begin() + static_cast<std::ptrdiff_t>(my) * sw + mx0,
                  mask.begin() + static_cast<std::ptrdiff_t>(my) * sw + mx1,
                  static_cast<unsigned char>(255));
    Fl_RGB_Image maskImg(mask.data(), sw, sh, 1);
    Fl_Image* scaledMask = maskImg.copy(dw, dh);
    const auto* mc = static_cast<Fl_RGB_Image*>(scaledMask);
    const unsigned char* ma = reinterpret_cast<const unsigned char*>(mc->array);
    const int md = mc->d();
    int mld = mc->ld();
    if (mld == 0)
        mld = dw * md;

    const common::Color8 back = p.canvasBg; // the off-canvas backdrop, as in the main canvas view

    // Backdrop everywhere first (covers the aspect-residual letterbox, which is always off-canvas);
    // then, over the scaled content area, paint the checker inside the canvas and composite
    // content.
    std::vector<unsigned char> buf(static_cast<std::size_t>(iw) * ih * 3);
    for (std::size_t i = 0; i < static_cast<std::size_t>(iw) * ih; ++i) {
        buf[i * 3] = back.r;
        buf[i * 3 + 1] = back.g;
        buf[i * 3 + 2] = back.b;
    }
    for (int cy = 0; cy < dh; ++cy) {
        const int py = oy + cy;
        if (py < 0 || py >= ih)
            continue;
        for (int cx = 0; cx < dw; ++cx) {
            const int px = ox + cx;
            if (px < 0 || px >= iw)
                continue;
            const std::size_t o = (static_cast<std::size_t>(py) * iw + px) * 3;
            // In-canvas (checker) vs off-canvas (leave backdrop), from the co-scaled mask.
            if (ma[static_cast<std::size_t>(cy) * mld + cx * md] >= 128) {
                const bool dark = ((px / kCell) + (py / kCell)) & 1;
                buf[o] = buf[o + 1] = buf[o + 2] = dark ? kDark : kLight;
            }
            // Composite the (premultiplied) content over the base.
            if (sd == 4) {
                const unsigned char* q = sa + static_cast<std::size_t>(cy) * sld + cx * sd;
                const float a = q[3] / 255.0f;
                if (a <= 0.0f)
                    continue;
                const float inv = 1.0f - a;
                buf[o] = static_cast<unsigned char>(std::lround(q[0] + buf[o] * inv)); // premul
                buf[o + 1] = static_cast<unsigned char>(std::lround(q[1] + buf[o + 1] * inv));
                buf[o + 2] = static_cast<unsigned char>(std::lround(q[2] + buf[o + 2] * inv));
            }
        }
    }
    delete scaled;
    delete scaledMask;

    Fl_RGB_Image out(buf.data(), iw, ih, 3);
    out.draw(x() + 1, y() + 1);

    fl_color(toFl(p.border));
    fl_rect(x(), y(), W, H); // the frame, on the 1px margin outside the inset content
}

} // namespace mosaic::ui
