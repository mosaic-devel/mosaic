#include "ui/export_dialog.hpp"

#include "common/geometry.hpp"
#include "common/i18n.hpp"
#include "core/color_management.hpp" // iccProfileName -- a chosen profile's real name for the note
#include "io/caps.hpp"
#include "io/format_registry.hpp"
#include "io/io.hpp"
#include "io/options_schema.hpp"
#include "platform/file_dialog.hpp"
#include "render/compositor.hpp" // chooseAutoFilter -- the resize seam's Auto resolver
#include "render/render.hpp"     // render::ResampleFilter
#include "ui/color_flyout.hpp"
#include "ui/export_preview.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mosaic::ui {

// ---- the modal's pure arithmetic (declared in the header, tested headlessly) ------------------

std::string humanFileSize(std::size_t bytes) {
    char b[64];
    if (bytes < 1024)
        std::snprintf(b, sizeof b, "%zu B", bytes);
    else if (bytes < 1024 * 1024)
        std::snprintf(b, sizeof b, "%.1f KB", static_cast<double>(bytes) / 1024.0);
    else
        std::snprintf(b, sizeof b, "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return b;
}

namespace {

// One dimension, clamped into the legal export range. Every size entry funnels through here, so
// a pasted 1e9 or a typed 0 can never reach the resizer.
std::uint32_t clampDim(double v) {
    if (!std::isfinite(v))
        return 1;
    const long r = std::lround(std::clamp(v, 1.0, static_cast<double>(kMaxExportDim)));
    return static_cast<std::uint32_t>(r);
}

} // namespace

ExportPixelSize exportSizeFromScale(std::uint32_t baseW, std::uint32_t baseH, double percent) {
    if (baseW == 0 || baseH == 0)
        return {1, 1};
    const double p = std::clamp(std::isfinite(percent) ? percent : 100.0, 0.001, 1000000.0);
    return {clampDim(baseW * p / 100.0), clampDim(baseH * p / 100.0)};
}

ExportPixelSize exportSizeFromWidth(std::uint32_t baseW, std::uint32_t baseH, double width,
                                    std::uint32_t currentH, bool lockAspect) {
    ExportPixelSize out;
    out.w = clampDim(width);
    if (lockAspect && baseW > 0)
        out.h = clampDim(static_cast<double>(out.w) * baseH / baseW);
    else
        out.h = clampDim(static_cast<double>(currentH));
    return out;
}

ExportPixelSize exportSizeFromHeight(std::uint32_t baseW, std::uint32_t baseH, double height,
                                     std::uint32_t currentW, bool lockAspect) {
    ExportPixelSize out;
    out.h = clampDim(height);
    if (lockAspect && baseH > 0)
        out.w = clampDim(static_cast<double>(out.h) * baseW / baseH);
    else
        out.w = clampDim(static_cast<double>(currentW));
    return out;
}

double exportScalePercent(std::uint32_t baseW, std::uint32_t outW) {
    if (baseW == 0)
        return 100.0;
    return 100.0 * static_cast<double>(outW) / static_cast<double>(baseW);
}

namespace {

Fl_Color toFl(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }

// ---- the shared visual vocabulary ------------------------------------------------------------
//
// The New Document dialog settled the look this one now speaks: a muted caption above every
// control, a bold section head trailed by a hairline, one consistent vertical rhythm, and no
// stock-FLTK chrome anywhere. These three small widgets are what carry it here. They stay
// file-local on purpose -- the caption/head pair is a LAYOUT decision this dialog makes, not a
// component another dialog would want handed to it.

constexpr int kWinW = 1000;
constexpr int kWinH = 700;
constexpr int kBarH = 68;    // the full-width action bar under both panes
constexpr int kSideW = 356;  // the settings column
constexpr int kMargin = 16;
constexpr int kPad = 16;     // the settings column's own inset
constexpr int kSectionGap = 18;
constexpr int kBodyH = kWinH - kBarH;
constexpr int kSideX = kWinW - kSideW;
constexpr int kStatusH = 140; // the pinned info + loss banner at the column's foot

// A muted caption above its control ("Width", "Scale") -- the form reads as labelled groups
// rather than a grid of naked boxes.
Fl_Box* fieldCaption(int x, int y, int w, const char* text, const Palette& p) {
    auto* b = new Fl_Box(x, y, w, 15, text);
    b->box(FL_NO_BOX);
    b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    b->labelfont(FL_HELVETICA);
    b->labelsize(11);
    b->labelcolor(toFl(p.textMuted));
    return b;
}

// A muted body line (the format facts, the "no options" note). Wraps, and is given room for TWO
// lines, because a wrapped Fl_Box neither grows to fit its label nor clips it -- it draws the
// overflow outside its own rect, straight over whatever sits below (the standing trap; a long
// translation is exactly how it fires).
Fl_Box* mutedLine(int x, int y, int w, int h, const Palette& p) {
    auto* b = new Fl_Box(x, y, w, h, "");
    b->box(FL_NO_BOX);
    b->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    b->labelfont(FL_HELVETICA);
    b->labelsize(11);
    b->labelcolor(toFl(p.textMuted));
    return b;
}

// A section head: a bold caption with a hairline running out to the column edge. The rule is what
// turns a tall stack of controls into readable sections without boxing anything in.
class SectionHeader : public Fl_Widget {
public:
    SectionHeader(int X, int Y, int W, int H, const char* text) : Fl_Widget(X, Y, W, H) {
        copy_label(text);
    }

protected:
    void draw() override {
        const Palette& p = activePalette();
        fl_color(toFl(p.panelBg)); // erase first: the back buffer keeps last frame's pixels
        fl_rectf(x(), y(), w(), h());
        fl_font(FL_HELVETICA_BOLD, 11);
        fl_color(toFl(p.text));
        fl_draw(label(), x(), y(), w(), h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE, nullptr, 0);
        const int tw = static_cast<int>(std::ceil(fl_width(label())));
        const int lx = x() + tw + 10;
        if (lx < x() + w() - 1) {
            fl_color(toFl(p.border));
            const int ly = y() + h() / 2 + 1;
            fl_line(lx, ly, x() + w() - 1, ly);
        }
    }
};

// The options panel's collapsible group head ("Advanced"): the Type panel's DisclosureButton look
// -- a drawn triangle plus a bold caption, no button chrome. The triangle is a POLYGON, never a
// Unicode arrow: the host font may not carry one and a tofu box in the settings panel is exactly
// the class of bug the no-glyph rule exists to prevent.
class GroupHeaderButton : public FlatButton {
public:
    GroupHeaderButton(int X, int Y, int W, int H) : FlatButton(X, Y, W, H) {}

    void setOpen(bool open) {
        m_open = open;
        redraw();
    }
    void setText(std::string text) {
        m_text = std::move(text);
        redraw();
    }

protected:
    void draw() override {
        const Palette& p = activePalette();
        fl_color(toFl(p.panelBg));
        fl_rectf(x(), y(), w(), h());
        const int cx = x() + 5;
        const int cy = y() + h() / 2;
        fl_color(toFl(p.text));
        fl_begin_polygon();
        if (m_open) {
            fl_vertex(cx - 4, cy - 2);
            fl_vertex(cx + 4, cy - 2);
            fl_vertex(cx, cy + 3);
        } else {
            fl_vertex(cx - 2, cy - 4);
            fl_vertex(cx - 2, cy + 4);
            fl_vertex(cx + 3, cy);
        }
        fl_end_polygon();
        fl_font(FL_HELVETICA_BOLD, 11);
        fl_draw(m_text.c_str(), x() + 16, y(), w() - 16, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE,
                nullptr, 0);
    }

private:
    std::string m_text;
    bool m_open = false;
};

// The accent-filled primary button is the shared ui::FilledButton (widgets.hpp).

void styleInput(Fl_Input* in, const Palette& p) {
    in->box(MOSAIC_INPUT_BOX);
    in->color(toFl(p.controlBg));
    in->textcolor(toFl(p.text));
    in->cursor_color(toFl(p.text));
    in->labelcolor(toFl(p.text));
    in->textsize(13);
}

double readNum(Fl_Input* f, double fallback) {
    double v = 0.0;
    if (f != nullptr && parseFieldNumber(f->value(), v))
        return v;
    return fallback;
}

void setNum(Fl_Input* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.6g", v);
    f->value(b);
}

// ---- a self-contained separable triangle-filter resampler ----------------------------------
// The compositor's Lanczos kernels are file-local (compositor.cpp); promoting a clean
// render::resizeImage entry point is its own S53-a slice (plan §5). Until then this pure,
// dependency-free triangle (tent) filter stands in behind the seam below: good on both minify and
// magnify, premultiplied so transparent pixels don't bleed colour into their neighbours.
// Straight-alpha in, straight-alpha out. NOTHING calls it directly -- resizeForExport does.

struct Contrib {
    int index;
    float weight;
};

// Per-output-sample contributor lists for a 1-D resample srcSize -> dstSize; each list's weights
// sum to 1. Clamp-to-edge at the borders. On minify the triangle widens (support / scale) so the
// filter averages the pixels being collapsed (an area-like antialias); on magnify it interpolates.
std::vector<std::vector<Contrib>> buildContribs(std::uint32_t srcSize, std::uint32_t dstSize) {
    std::vector<std::vector<Contrib>> lists(dstSize);
    if (srcSize == 0 || dstSize == 0)
        return lists;
    const double scale = static_cast<double>(dstSize) / static_cast<double>(srcSize);
    const double support = scale < 1.0 ? 1.0 / scale : 1.0; // triangle radius in source px
    const double invScale = 1.0 / scale;
    for (std::uint32_t o = 0; o < dstSize; ++o) {
        const double center = (static_cast<double>(o) + 0.5) * invScale - 0.5;
        const int left = static_cast<int>(std::floor(center - support));
        const int right = static_cast<int>(std::ceil(center + support));
        std::vector<Contrib> c;
        double wsum = 0.0;
        for (int s = left; s <= right; ++s) {
            const double dist = static_cast<double>(s) - center;
            const double t = scale < 1.0 ? std::abs(dist) * scale : std::abs(dist);
            const double w = t < 1.0 ? (1.0 - t) : 0.0;
            if (w <= 0.0)
                continue;
            const int idx = std::clamp(s, 0, static_cast<int>(srcSize) - 1); // clamp-to-edge
            c.push_back({idx, static_cast<float>(w)});
            wsum += w;
        }
        if (wsum > 0.0)
            for (Contrib& e : c)
                e.weight = static_cast<float>(e.weight / wsum);
        lists[o] = std::move(c);
    }
    return lists;
}

common::Image resampleImage(const common::Image& src, std::uint32_t outW, std::uint32_t outH) {
    if (src.empty() || outW == 0 || outH == 0)
        return {};
    if (outW == src.width && outH == src.height)
        return src;
    const std::uint32_t sw = src.width, sh = src.height;

    // Premultiplied float source.
    std::vector<float> in(static_cast<std::size_t>(sw) * sh * 4);
    for (std::size_t i = 0; i < static_cast<std::size_t>(sw) * sh; ++i) {
        const float a = src.rgba[i * 4 + 3] / 255.0f;
        in[i * 4 + 0] = src.rgba[i * 4 + 0] / 255.0f * a;
        in[i * 4 + 1] = src.rgba[i * 4 + 1] / 255.0f * a;
        in[i * 4 + 2] = src.rgba[i * 4 + 2] / 255.0f * a;
        in[i * 4 + 3] = a;
    }

    // Horizontal pass: sw -> outW (height unchanged).
    const std::vector<std::vector<Contrib>> hc = buildContribs(sw, outW);
    std::vector<float> mid(static_cast<std::size_t>(outW) * sh * 4, 0.0f);
    for (std::uint32_t y = 0; y < sh; ++y) {
        const float* row = &in[static_cast<std::size_t>(y) * sw * 4];
        float* orow = &mid[static_cast<std::size_t>(y) * outW * 4];
        for (std::uint32_t x = 0; x < outW; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (const Contrib& c : hc[x]) {
                const float* p = &row[static_cast<std::size_t>(c.index) * 4];
                r += p[0] * c.weight;
                g += p[1] * c.weight;
                b += p[2] * c.weight;
                a += p[3] * c.weight;
            }
            float* op = &orow[static_cast<std::size_t>(x) * 4];
            op[0] = r;
            op[1] = g;
            op[2] = b;
            op[3] = a;
        }
    }

    // Vertical pass: sh -> outH.
    const std::vector<std::vector<Contrib>> vc = buildContribs(sh, outH);
    std::vector<float> outbuf(static_cast<std::size_t>(outW) * outH * 4, 0.0f);
    for (std::uint32_t y = 0; y < outH; ++y) {
        for (std::uint32_t x = 0; x < outW; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (const Contrib& c : vc[y]) {
                const float* p = &mid[(static_cast<std::size_t>(c.index) * outW + x) * 4];
                r += p[0] * c.weight;
                g += p[1] * c.weight;
                b += p[2] * c.weight;
                a += p[3] * c.weight;
            }
            float* op = &outbuf[(static_cast<std::size_t>(y) * outW + x) * 4];
            op[0] = r;
            op[1] = g;
            op[2] = b;
            op[3] = a;
        }
    }

    // Unpremultiply back to 8-bit straight alpha.
    common::Image dst(outW, outH);
    const auto to8 = [](float v) {
        return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    for (std::size_t i = 0; i < static_cast<std::size_t>(outW) * outH; ++i) {
        const float a = std::clamp(outbuf[i * 4 + 3], 0.0f, 1.0f);
        if (a > 0.0f) {
            dst.rgba[i * 4 + 0] = to8(outbuf[i * 4 + 0] / a);
            dst.rgba[i * 4 + 1] = to8(outbuf[i * 4 + 1] / a);
            dst.rgba[i * 4 + 2] = to8(outbuf[i * 4 + 2] / a);
        }
        dst.rgba[i * 4 + 3] = to8(a);
    }
    return dst;
}

// Point-sample resize -- the one kernel the seam below can honour honestly on its own, and the
// one that matters most for an export (pixel art must not be blurred by a "quality" default).
common::Image nearestResize(const common::Image& src, std::uint32_t outW, std::uint32_t outH) {
    common::Image dst(outW, outH);
    for (std::uint32_t y = 0; y < outH; ++y) {
        const std::uint32_t sy = std::min(src.height - 1, static_cast<std::uint32_t>(
                                                              (static_cast<std::uint64_t>(y) *
                                                               src.height) / outH));
        for (std::uint32_t x = 0; x < outW; ++x) {
            const std::uint32_t sx = std::min(
                src.width - 1,
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * src.width) / outW));
            const std::size_t s = (static_cast<std::size_t>(sy) * src.width + sx) * 4;
            const std::size_t d = (static_cast<std::size_t>(y) * outW + x) * 4;
            dst.rgba[d + 0] = src.rgba[s + 0];
            dst.rgba[d + 1] = src.rgba[s + 1];
            dst.rgba[d + 2] = src.rgba[s + 2];
            dst.rgba[d + 3] = src.rgba[s + 3];
        }
    }
    return dst;
}

// ---- THE render::resizeImage SEAM ------------------------------------------------------------
//
// Stage 2 of the §5 pipeline (composite -> RESIZE -> encode). The export pipeline calls exactly
// this one function, so wiring the real thing is a ONE-LINE change here:
//
//     return render::resizeImage(src, outW, outH, filter);
//
// render::resizeImage does not exist yet -- it is a slice of S53-a and render/compositor.cpp was
// being rewritten for GPU residency (S60-a) when M2 and M3 were built, so this session must not
// add it. §5 specifies it precisely (8-bit straight alpha in and out, sampling in PREMULTIPLIED
// alpha, Auto through chooseAutoFilter, empty/zero -> empty, an exact-size request returning a
// bit-exact copy). Until then the seam keeps the same contract with what it can honestly do:
// Auto is resolved through the REAL render::chooseAutoFilter, Nearest point-samples, and every
// smooth kernel falls to the local premultiplied triangle (resampleImage above), which is a
// reasonable stand-in for Bilinear/Area but is NOT Lanczos.
common::Image resizeForExport(const common::Image& src, std::uint32_t outW, std::uint32_t outH,
                              render::ResampleFilter filter) {
    if (src.empty() || outW == 0 || outH == 0)
        return {};
    if (outW == src.width && outH == src.height)
        return src; // bit-exact, whatever the filter
    render::ResampleFilter resolved = filter;
    if (resolved == render::ResampleFilter::Auto)
        resolved = render::chooseAutoFilter(
            common::Affine2D::scaling(static_cast<double>(outW) / static_cast<double>(src.width),
                                      static_cast<double>(outH) / static_cast<double>(src.height)),
            /*liveDrag=*/false);
    if (resolved == render::ResampleFilter::Nearest)
        return nearestResize(src, outW, outH);
    return resampleImage(src, outW, outH);
}

// The resize-quality dropdown's contents (§6.4): the full render::ResampleFilter range, Auto
// first and default. Kept as a table so the labels and the enum can never drift apart.
struct FilterChoice {
    render::ResampleFilter filter;
    const char* label;
};
const FilterChoice kFilterChoices[] = {
    {render::ResampleFilter::Auto, N_("Auto  (recommended)")},
    {render::ResampleFilter::Nearest, N_("Nearest  (hard pixels)")},
    {render::ResampleFilter::Bilinear, N_("Bilinear")},
    {render::ResampleFilter::Bicubic, N_("Bicubic")},
    {render::ResampleFilter::Mitchell, N_("Mitchell")},
    {render::ResampleFilter::Lanczos2, N_("Lanczos 2")},
    {render::ResampleFilter::Lanczos3, N_("Lanczos 3")},
    {render::ResampleFilter::Area, N_("Area  (shrinking)")},
    {render::ResampleFilter::Gaussian, N_("Gaussian")},
    {render::ResampleFilter::Supersample, N_("Supersample")},
};

// The quick-scale chips under the percentage slider: the four ratios people actually ask for.
const double kQuickScales[] = {25.0, 50.0, 100.0, 200.0};
const char* const kQuickScaleLabels[] = {N_("25%"), N_("50%"), N_("100%"), N_("200%")};

// ---- the loss banner's words -----------------------------------------------------------------
//
// io::diff() returns UNTRANSLATED English plus a stable LossCode, deliberately: io is gettext-free
// and translating inside diff() would make its goldens locale-dependent (§4.1). So the banner
// translates BY CODE here, and diff()'s own strings stay the fallback for a code this switch does
// not know yet (a new one added by a future backend must not silently show nothing).
std::string lossMessage(const io::LossWarning& w) {
    switch (w.code) {
    case io::LossCode::AlphaDropped:
        return _("Transparency is lost -- transparent areas are filled with the matte colour.");
    case io::LossCode::AlphaReducedToBinary:
        return _("Soft transparency is lost -- each pixel ends up either fully opaque or fully "
                 "transparent.");
    case io::LossCode::LayersFlattened:
        return _("Layers are flattened into one picture.");
    case io::LossCode::VectorRasterized:
        return _("Vector shapes are rasterised -- they stop being editable geometry.");
    case io::LossCode::TextRasterized:
        return _("Text is rasterised -- it stops being editable type.");
    case io::LossCode::EffectsBaked:
        return _("Layer effects are baked into the pixels.");
    case io::LossCode::Extrude3dBaked:
        return _("3D text is baked into the pixels.");
    case io::LossCode::AdjustmentsBaked:
        return _("Adjustment layers are baked into the pixels.");
    case io::LossCode::BitDepthReduced:
        return _("Colour precision is reduced to what this format stores.");
    case io::LossCode::HdrClipped:
        return _("High dynamic range is clipped to the format's range.");
    case io::LossCode::ColorsQuantized:
        return _("Colours are quantised down to this format's palette.");
    case io::LossCode::LossyEncode:
        return _("The picture is re-compressed lossily -- fine detail changes.");
    case io::LossCode::ChromaSubsampled:
        return _("Colour resolution is halved (chroma subsampling) -- edges and text soften.");
    case io::LossCode::IccDropped:
        return _("The colour profile is not written -- the file is read as plain sRGB.");
    case io::LossCode::ColorSpaceConverted:
        return _("The working colour space is converted on the way out.");
    case io::LossCode::ExifDropped:
        return _("Camera metadata (EXIF) is not written.");
    case io::LossCode::XmpDropped:
        return _("XMP metadata is not written.");
    case io::LossCode::DpiDropped:
        return _("The resolution (DPI) is not recorded in the file.");
    case io::LossCode::ConicGradientRasterized:
        return _("Conic gradients have no equivalent here and are rasterised.");
    case io::LossCode::StrokeAlignOutlined:
        return _("Inside/outside strokes are converted to outlines -- they look identical but "
                 "stop being strokes.");
    case io::LossCode::BlendModesFlattened:
        return _("Blend modes are flattened into the artwork.");
    }
    return w.feature + ": " + w.consequence; // an unknown code still says something true
}

// Blend `c` towards `ground` -- the banner tints its own ground so one pair of hues reads in both
// the light and the dark theme without a second palette.
common::Color8 towards(common::Color8 c, common::Color8 ground, double t) {
    const auto mix = [t](std::uint8_t a, std::uint8_t b) {
        return static_cast<std::uint8_t>(std::lround(a * (1.0 - t) + b * t));
    };
    return common::Color8{mix(c.r, ground.r), mix(c.g, ground.g), mix(c.b, ground.b), 255};
}

common::Color8 severityColor(io::Severity sev) {
    switch (sev) {
    case io::Severity::HardLoss: return common::Color8{0xD9, 0x3B, 0x3B, 0xFF};
    case io::Severity::Lossy: return common::Color8{0xD1, 0x86, 0x16, 0xFF};
    case io::Severity::Fine: break;
    }
    return common::Color8{0x38, 0x9E, 0x5A, 0xFF};
}

// The live loss banner (§6.7): a tinted callout with a severity rule down its left edge, a DRAWN
// severity badge, and the top warnings. It draws its own badge rather than prefixing the text
// with a check / warning / cross character, because those are Unicode symbols and the host font
// may render them as tofu -- the same rule that keeps glyphs out of every other label in the app.
class LossBanner : public Fl_Widget {
public:
    LossBanner(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}

    void setContent(io::Severity sev, std::string text) {
        m_sev = sev;
        m_text = std::move(text);
        redraw();
    }

protected:
    void draw() override {
        const Palette& p = activePalette();
        const common::Color8 hue = severityColor(m_sev);
        const common::Color8 ground = towards(hue, p.panelBg, 0.86);
        fl_color(toFl(ground)); // erase the WHOLE rect before any text
        fl_rectf(x(), y(), w(), h());
        fl_color(toFl(towards(hue, p.panelBg, 0.45)));
        fl_rectf(x(), y(), 3, h()); // the severity rule

        drawBadge(x() + 18, y() + 15, ground);

        fl_push_clip(x(), y(), w(), h()); // a wrapped label draws OUTSIDE its rect otherwise
        fl_font(FL_HELVETICA, 11);
        fl_color(toFl(towards(hue, p.text, 0.3)));
        fl_draw(m_text.c_str(), x() + 32, y() + 6, w() - 40, h() - 10,
                FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP, nullptr, 0);
        fl_pop_clip();
    }

private:
    // A filled disc (anti-aliased through the shared coverage painter -- fl_pie is stair-stepped)
    // carrying a drawn tick / bang / cross.
    void drawBadge(int cx, int cy, common::Color8 ground) const {
        const common::Color8 hue = severityColor(m_sev);
        const std::vector<AAPrim> prims{AAPrim{static_cast<double>(cx), static_cast<double>(cy),
                                               7.0, 0.0, hue}};
        drawAAPrims(cx - 9, cy - 9, 18, 18, [ground](int, int) { return ground; }, prims);
        fl_color(FL_WHITE);
        fl_line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, 2);
        switch (m_sev) {
        case io::Severity::Fine: // a tick
            fl_begin_line();
            fl_vertex(cx - 3, cy);
            fl_vertex(cx - 1, cy + 3);
            fl_vertex(cx + 4, cy - 3);
            fl_end_line();
            break;
        case io::Severity::Lossy: // an exclamation (the dot is a rect: a zero-length round-capped
            fl_line(cx, cy - 4, cx, cy + 1); // line is not reliably painted by every backend)
            fl_line_style(0);
            fl_rectf(cx - 1, cy + 3, 2, 2);
            break;
        case io::Severity::HardLoss: // a cross
            fl_line(cx - 3, cy - 3, cx + 3, cy + 3);
            fl_line(cx - 3, cy + 3, cx + 3, cy - 3);
            break;
        }
        fl_line_style(0);
    }

    io::Severity m_sev = io::Severity::Fine;
    std::string m_text;
};

// What the chosen format can carry, straight off its CAPS -- a one-line answer to "is this the
// right format?" that costs no per-format code and grows with every backend added.
std::string formatFacts(const io::FormatCaps& c) {
    std::vector<std::string> bits;
    if (c.lossless && c.lossy)
        bits.emplace_back(_("lossless or lossy"));
    else if (c.lossless)
        bits.emplace_back(_("lossless"));
    else if (c.lossy)
        bits.emplace_back(_("lossy"));
    switch (c.alpha) {
    case io::AlphaKind::None: bits.emplace_back(_("no transparency")); break;
    case io::AlphaKind::Binary: bits.emplace_back(_("on/off transparency")); break;
    default: bits.emplace_back(_("full transparency")); break;
    }
    char buf[64];
    if (c.maxColors > 0)
        std::snprintf(buf, sizeof buf, _("up to %d colours"), c.maxColors);
    else
        // TRANSLATORS: bit depth per colour channel of the exported file.
        std::snprintf(buf, sizeof buf, _("%d-bit colour"), c.maxBitDepth);
    bits.emplace_back(buf);
    if (c.icc)
        bits.emplace_back(_("ICC profile"));
    if (c.animation)
        bits.emplace_back(_("animation"));
    std::string out;
    for (const std::string& s : bits) {
        if (!out.empty())
            out += " \xC2\xB7 "; // a middle dot, as everywhere else in the app's readouts
        out += s;
    }
    return out;
}

// ---- the async export pipeline (§5) ----------------------------------------------------------
//
// Cloned from the InpaintJob template (app_window.cpp): a worker std::thread, an atomic `done`,
// cancellation through an atomic the encoder's ProgressFn polls, and a re-armed Fl::add_timeout on
// the UI side. The worker NEVER touches FLTK and never reads the dialog.
//
// It carries no mutex, unlike InpaintJob, and that is deliberate rather than sloppy: none of our
// encoders reports intermediate progress, so there is nothing to publish DURING the run. Every
// result field is written before `done` is stored and read only after the join, and the join is
// the happens-before edge. The moment an encoder grows real progress reporting, a mutex around a
// published fraction joins it -- exactly as InpaintJob has.
struct PipelineJob {
    // ---- inputs (owned or shared; never a pointer into the dialog) ----
    std::shared_ptr<const common::Image> source; // stage 1: the composite
    std::shared_ptr<const common::Image> cached; // stage 2's cached output, when still valid
    std::uint32_t outW = 0, outH = 0;
    render::ResampleFilter filter = render::ResampleFilter::Auto;
    const io::FormatBackend* backend = nullptr;
    io::OptionValues values;
    common::Color8 matte{255, 255, 255, 255};
    double dpi = 72.0;
    // The metadata payload, OWNED by the job (never a pointer into the dialog): the worker outlives
    // any number of edits to the settings column, and a job reading the live state would be reading
    // it from another thread.
    std::optional<common::ExifData> exif;
    std::vector<std::uint8_t> icc;
    bool keepMetadata = true;
    std::string key; // what this job answers for; see ExportDialog::pipelineKey

    std::atomic<bool> cancelRequested{false};
    std::atomic<bool> done{false};

    // ---- results, valid after done + join ----
    std::shared_ptr<const common::Image> resized; // stage 2
    std::shared_ptr<const common::Image> preview; // the ENCODED bytes decoded back, when we can
    std::size_t byteCount = 0;                    // stage 3: the exact file size, not an estimate
    bool ok = false;
    std::string error;
    std::thread worker;

    ~PipelineJob() {
        cancelRequested.store(true);
        if (worker.joinable())
            worker.join();
    }
};

void runPipeline(PipelineJob* job) {
    const common::Image* pixels = nullptr;
    if (job->cached) {
        job->resized = job->cached; // stage 2 hit: only the encode is re-run
        pixels = job->cached.get();
    } else {
        auto fresh = std::make_shared<common::Image>(
            resizeForExport(*job->source, job->outW, job->outH, job->filter));
        job->resized = fresh;
        pixels = fresh.get();
    }
    if (job->cancelRequested.load() || pixels->empty() || job->backend == nullptr) {
        job->error = pixels->empty() ? "the resized image is empty" : "";
        job->done.store(true);
        return;
    }

    io::RenderInput input;
    input.pixels = pixels;
    input.matte = job->matte;
    input.dpi = job->dpi;
    // The real payload, so the size readout counts the metadata the file will actually carry (an
    // embedded press profile is megabytes -- an "exact expected file size" that ignored it would be
    // the largest lie in the info block).
    input.stripMetadata = !job->keepMetadata;
    input.exif = job->exif.has_value() ? &*job->exif : nullptr;
    input.iccProfile = job->icc;
    const io::ProgressFn progress = [job](float) { return !job->cancelRequested.load(); };
    io::EncodeResult r = job->backend->encode(input, job->values, progress);
    if (r.ok) {
        job->byteCount = r.bytes.size();
        // §5: the ENCODE stage is the source of both the preview and the size. Decoding our own
        // bytes back is what makes the preview show the artefacts the quality slider actually
        // causes. Only PNG and JPEG decode today; anything else falls back to the resize, which
        // is the same picture minus the codec's own damage.
        if (std::optional<common::Image> back =
                io::decodeImageBytes(r.bytes.data(), r.bytes.size(), nullptr))
            job->preview = std::make_shared<const common::Image>(std::move(*back));
    }
    job->ok = r.ok;
    job->error = std::move(r.error);
    job->done.store(true);
}

// One rendered option control, bound to its schema key. The panel is built from the backend's
// OptionsSchema, so this is the ONLY place in ui/ that knows anything about encoder options -- and
// it knows them as types, never as formats.
struct OptionRow {
    class ExportDialog* dlg = nullptr;
    io::OptionDesc desc;      // the row's OWN copy: every label/tooltip pointer handed to FLTK
                              // must outlive the schema temporary it was read from
    bool isHeader = false;    // a collapsible group header rather than a control
    std::string groupId;      // the group this row belongs to ("" = the always-visible section)
    Fl_Widget* label = nullptr;
    Fl_Widget* control = nullptr;
    Fl_Box* readout = nullptr;
    int height = 26;
};

// One preset (§6.1): a format plus the option values that define the intent. Beginners never open
// the options panel; pros ignore the row.
struct ExportPreset {
    const char* label;
    io::FormatId format;
    std::vector<std::pair<const char*, io::OptionValue>> values;
};

class ExportDialog : public Fl_Double_Window {
public:
    explicit ExportDialog(const ExportRequest& req);
    ~ExportDialog() override;

    [[nodiscard]] std::optional<ExportResult> result() const {
        if (!m_accepted)
            return std::nullopt;
        return ExportResult{m_resultPath, m_resultFormat, m_resultValues};
    }

    // Called back by the generated option controls (OptionRow is a free struct so the FLTK
    // callbacks can carry it as user data without a std::function per widget).
    void onOptionEdited(const OptionRow& row);

protected:
    int handle(int event) override;

private:
    // ---- format helpers, all answered by the registry ----
    [[nodiscard]] const io::FormatBackend* backend() const {
        if (m_backends.empty())
            return nullptr;
        const int i = std::clamp(m_format->value(), 0, static_cast<int>(m_backends.size()) - 1);
        return m_backends[static_cast<std::size_t>(i)];
    }
    [[nodiscard]] std::string primaryExt() const {
        const io::FormatBackend* b = backend();
        if (b == nullptr || b->extensions().empty())
            return ".png";
        return "." + b->extensions().front();
    }
    [[nodiscard]] std::vector<platform::FileFilter> currentFilters() const;
    [[nodiscard]] io::OptionValues& valuesForCurrentFormat();

    // ---- callbacks / logic ----
    void onFormatChanged();
    void onPresetPicked();
    void rebuildOptionsPanel();  // schema -> widgets
    void relayoutSide();         // the whole settings column, top to bottom, in one pass
    int layoutOptionRows(int top, int cx, int cw); // -> the y after the last visible row
    void toggleGroup(const std::string& groupId);
    [[nodiscard]] bool groupCollapsed(const io::OptionsSchema& schema,
                                      const std::string& groupId) const;
    void applySize(ExportPixelSize s, bool fromScaleField);
    void onScaleEdit();
    void onScaleSlider();
    void onWidthEdit();
    void onHeightEdit();
    void onAspectToggle();
    void updateInfo();
    void updateLossBanner();

    // The 3-stage cache's driver: mark dirty, debounce, run, poll (§5).
    void requestEstimate(); // kept as the name every size/option edit already calls
    [[nodiscard]] std::string pipelineKey();
    void startPipeline();
    void pollPipeline();
    void applyJobResult(const PipelineJob& job);
    static void debounceThunk(void* v) {
        auto* d = static_cast<ExportDialog*>(v);
        d->m_debouncePending = false;
        // A job still running takes the request on its next poll; nothing pending means the
        // readouts already answer (requestEstimate short-circuits an unchanged key).
        if (d->m_job == nullptr && d->m_pipelineDirty)
            d->startPipeline();
    }
    static void pollThunk(void* v) { static_cast<ExportDialog*>(v)->pollPipeline(); }

    void openMatteFlyout();
    // The Colour & metadata section: reconcile the three rows against the chosen format's CAPS
    // (what it can carry) and the document (what there is to carry). A control the format cannot
    // honour is DISABLED WITH A STATED REASON -- never silently ignored, and never driven from a
    // list of format names, which is the whole point of the caps architecture.
    void refreshMetadataSection();
    void chooseIccProfile();   // the "Choose a profile…" pick, through the system Open picker
    // The profile bytes the current Colour row selection resolves to; empty for "embed none".
    [[nodiscard]] const std::vector<std::uint8_t>& selectedIcc() const;
    // What the user asked the export to carry, for io::diff (io/caps.hpp MetadataRequest).
    [[nodiscard]] io::MetadataRequest metadataRequest() const;
    void doBrowse();          // fill the path field from the system Save picker
    void doExport();
    void doCancel() { hide(); }

    // Ask the system Save picker for a path, seeded through the io::seedExportTarget policy.
    // nullopt on cancel. Shared by [...] and the empty-path Export flow.
    [[nodiscard]] std::optional<std::string> askForPath();
    [[nodiscard]] io::ExportSeed seed() const;

    // ---- state ----
    std::shared_ptr<const common::Image> m_base;
    io::DocumentProfile m_profile;
    io::ExportPathInputs m_pathInputs;
    std::uint32_t m_baseW = 0, m_baseH = 0;
    std::uint32_t m_outW = 0, m_outH = 0;
    std::string m_stem;
    double m_dpi = 72.0;
    std::vector<const io::FormatBackend*> m_backends;   // the combobox, in exportOrder()
    std::map<int, io::OptionValues> m_valuesByFormat;   // keyed by FormatId, so a format switch and
                                                        // back restores what you had set
    render::ResampleFilter m_filter = render::ResampleFilter::Auto;
    common::Color8 m_matte{255, 255, 255, 255};

    // ---- Colour & metadata (§6.6) ----
    // Which profile the Colour row embeds. The order IS the dropdown's item order, so the two can
    // never drift apart.
    enum class IccChoice { None = 0, Document = 1, Custom = 2 };
    std::optional<common::ExifData> m_exif;   // the document's provenance record, or none
    std::vector<std::uint8_t> m_docIcc;       // the document's own profile, or empty
    std::string m_docIccName;
    std::vector<std::uint8_t> m_customIcc;    // a profile the user picked, or empty
    std::string m_customIccName;
    IccChoice m_iccChoice = IccChoice::None;
    bool m_keepMetadata = true;  // §6's Metadata toggle. On by default: keeping a photograph's own
                                 // camera data is what every other editor does, and the row states
                                 // exactly what "keep" means for the chosen format.

    bool m_syncing = false;      // suppress the size fields' callbacks during programmatic writes
    bool m_accepted = false;
    std::string m_resultPath;
    std::string m_resultFormat;
    io::OptionValues m_resultValues;

    // ---- the 3-stage cache ----
    std::unique_ptr<PipelineJob> m_job;
    std::vector<std::unique_ptr<OptionRow>> m_rows;
    // Group id -> open?, but ONLY for groups the user has actually toggled. Anything absent falls
    // back to the schema's own collapsedByDefault, so "Advanced is collapsed by default" (§6.5)
    // holds for every format including ones added later, while a group the user opened stays open
    // across a format switch.
    std::map<std::string, bool> m_groupOpen;
    std::shared_ptr<const common::Image> m_resized;   // stage 2's cached output ...
    std::uint32_t m_resizedW = 0, m_resizedH = 0;     // ... and what it is valid for
    render::ResampleFilter m_resizedFilter = render::ResampleFilter::Auto;
    std::string m_settledKey;    // the key the current readouts answer for
    bool m_pipelineDirty = true;
    bool m_debouncePending = false;
    bool m_polling = false;

    // ---- widgets ----
    ExportPreview* m_preview = nullptr;
    ScrollView* m_side = nullptr;

    SectionHeader* m_hPreset = nullptr;
    Dropdown* m_preset = nullptr;
    SectionHeader* m_hFormat = nullptr;
    Dropdown* m_format = nullptr;
    Fl_Box* m_formatFacts = nullptr;

    SectionHeader* m_hSize = nullptr;
    Fl_Box* m_capScale = nullptr;
    Slider* m_scaleSlider = nullptr;
    NumberField* m_scale = nullptr;
    FlatButton* m_quick[std::size(kQuickScales)] = {};
    Fl_Box* m_capW = nullptr;
    NumberField* m_wField = nullptr;
    Fl_Box* m_capH = nullptr;
    NumberField* m_hField = nullptr;
    CheckBox* m_aspect = nullptr;
    Fl_Box* m_capDpi = nullptr;
    NumberField* m_dpiField = nullptr;

    SectionHeader* m_hQuality = nullptr;
    Dropdown* m_filterPick = nullptr;

    SectionHeader* m_hOptions = nullptr;
    Fl_Box* m_optionsNote = nullptr;   // "this format has no settings"
    Fl_Group* m_optionsGroup = nullptr;

    SectionHeader* m_hColor = nullptr;
    Fl_Box* m_matteLabel = nullptr;
    SwatchChip* m_matteChip = nullptr;
    Fl_Box* m_iccLabel = nullptr;
    Dropdown* m_iccPick = nullptr;
    Fl_Box* m_iccNote = nullptr;       // which profile, or why none can be embedded
    CheckBox* m_metaCheck = nullptr;
    Fl_Box* m_metaNote = nullptr;      // what this format will actually store, or why it cannot
    Fl_Box* m_bottomSpacer = nullptr;  // the column's tail padding, as a real child (Fl_Scroll
                                       // measures its range from the children's bounding box)

    Fl_Box* m_infoDims = nullptr;
    ScrollingLabel* m_infoSize = nullptr;
    LossBanner* m_banner = nullptr;

    TextInput* m_pathField = nullptr;
    ColorFlyout* m_matteFlyout = nullptr;
    FilledButton* m_exportBtn = nullptr;
};

// The preset row (§6.1). Index 0 is always "Custom" -- editing anything falls back to it, so the
// row never claims a preset the settings no longer match.
const ExportPreset kPresets[] = {
    {N_("Web PNG"), io::FormatId::Png, {{"compression", io::intValue(6)}}},
    {N_("Web JPEG"),
     io::FormatId::Jpeg,
     {{"quality", io::intValue(82)},
      {"subsampling", io::textValue("4:2:0")},
      {"progressive", io::boolValue(true)}}},
    {N_("Photo JPEG (high quality)"),
     io::FormatId::Jpeg,
     {{"quality", io::intValue(95)}, {"subsampling", io::textValue("4:4:4")}}},
    {N_("Lossless archive"), io::FormatId::Jxl, {{"lossless", io::boolValue(true)}}},
};

ExportDialog::ExportDialog(const ExportRequest& req)
    : Fl_Double_Window(kWinW, kWinH, _("Export")),
      m_base(std::make_shared<const common::Image>(*req.composited)), m_profile(req.profile),
      m_pathInputs(io::exportPathInputs(req.target.path, req.documentPath)),
      m_baseW(req.composited->width), m_baseH(req.composited->height),
      m_outW(req.composited->width), m_outH(req.composited->height), m_stem(req.suggestedStem),
      m_dpi(req.dpi), m_exif(req.exif), m_docIcc(req.documentIcc),
      m_docIccName(req.documentIccName),
      // A document that HAS a profile of its own defaults to embedding it -- the colour it was
      // edited in is the colour the file should claim. A plain sRGB document has nothing of its own
      // (core::documentIccProfile returns empty for sRGB, deliberately), so it defaults to none.
      m_iccChoice(req.documentIcc.empty() ? IccChoice::None : IccChoice::Document) {
    const Palette& pal = activePalette();
    color(toFl(pal.windowBg));

    // The format list IS the registry's export order (§3: Common tier by popularity, then the
    // curated set alphabetically); an unavailable backend -- JXL without libjxl -- is simply
    // absent. Nothing here knows a format's name.
    m_backends = io::FormatRegistry::instance().exportOrder(req.showAllFormats);

    begin();

    // ---- left: the interactive preview, edge to edge (the settings panel's own left hairline
    // is the only separator; a margin here would read as a gap, not as a viewport).
    m_preview = new ExportPreview(0, 0, kSideX, kBodyH);
    m_preview->setNote(_("Nothing to preview"));

    // ---- the settings column's ground + its separating hairline. Plain boxes rather than a
    // ui::Panel: everything on this side is positioned absolutely by relayoutSide(), so a group
    // would only add a coordinate space to translate through.
    {
        auto* ground = new Fl_Box(kSideX, 0, kSideW, kBodyH);
        ground->box(FL_FLAT_BOX);
        ground->color(toFl(pal.panelBg));
        auto* edge = new Fl_Box(kSideX, 0, 1, kBodyH);
        edge->box(FL_FLAT_BOX);
        edge->color(toFl(pal.border));
    }

    // ---- the scrolling settings stack ------------------------------------------------------
    m_side = new ScrollView(kSideX + 1, 0, kSideW - 1, kBodyH - kStatusH);
    m_side->type(Fl_Scroll::VERTICAL);
    // A FILLED box, not FL_NO_BOX: rows appear and vanish as conditions and group collapse
    // change, and the scroll has to erase what a hidden row left behind.
    m_side->box(MOSAIC_FLAT_BOX);
    m_side->color(toFl(pal.panelBg));
    m_side->begin();

    const int cx0 = kSideX + kPad;
    const int cw0 = kSideW - 2 * kPad - Fl::scrollbar_size();

    m_hPreset = new SectionHeader(cx0, 0, cw0, 18, _("Preset"));
    m_preset = new Dropdown(cx0, 0, cw0, 26);
    m_preset->add(_("Custom"), 0, nullptr, nullptr, FL_MENU_DIVIDER);
    for (const ExportPreset& p : kPresets)
        m_preset->add(_(p.label));
    m_preset->value(0);
    m_preset->callback([](Fl_Widget*, void* v) { static_cast<ExportDialog*>(v)->onPresetPicked(); },
                       this);

    m_hFormat = new SectionHeader(cx0, 0, cw0, 18, _("Format"));
    m_format = new Dropdown(cx0, 0, cw0, 26);
    for (std::size_t i = 0; i < m_backends.size(); ++i) {
        const io::FormatBackend* b = m_backends[i];
        // One divider under the Common tier, above the curated set (§3). Fl_Menu_::add COPIES the
        // label, so the temporary is safe.
        const bool common = b->tier() == io::FormatTier::Common;
        const bool lastCommon = common && i + 1 < m_backends.size() &&
                                m_backends[i + 1]->tier() != io::FormatTier::Common;
        m_format->add(_(std::string(b->displayName()).c_str()), 0, nullptr, nullptr,
                      lastCommon ? FL_MENU_DIVIDER : 0);
    }
    m_format->value(0);
    // Re-open on the format -- and with the options -- this document was last exported with
    // (§6's sticky memory). coerce() makes a bag stored against an older schema legal again.
    if (!req.target.formatId.empty()) {
        for (std::size_t i = 0; i < m_backends.size(); ++i) {
            if (io::formatIdName(m_backends[i]->id()) != req.target.formatId)
                continue;
            m_format->value(static_cast<int>(i));
            io::OptionValues values = req.target.values;
            m_backends[i]->optionsSchema().coerce(values);
            m_valuesByFormat[static_cast<int>(m_backends[i]->id())] = std::move(values);
        }
    }
    m_format->callback([](Fl_Widget*, void* v) { static_cast<ExportDialog*>(v)->onFormatChanged(); },
                       this);
    m_formatFacts = mutedLine(cx0, 0, cw0, 28, pal);

    // ---- output size ------------------------------------------------------------------------
    m_hSize = new SectionHeader(cx0, 0, cw0, 18, _("Output size"));
    m_capScale = fieldCaption(cx0, 0, cw0, _("Scale (%)"), pal);
    m_scaleSlider = new Slider(cx0, 0, cw0 - 66, 18);
    m_scaleSlider->range(1.0, 400.0);
    m_scaleSlider->step(1.0);
    m_scaleSlider->value(100.0);
    m_scaleSlider->setCellColor(pal.panelBg);
    m_scaleSlider->when(FL_WHEN_CHANGED);
    m_scaleSlider->callback([](Fl_Widget*, void* v) { static_cast<ExportDialog*>(v)->onScaleSlider(); },
                            this);
    m_scale = new NumberField(cx0, 0, 60, 24);
    styleInput(m_scale, pal);
    m_scale->when(FL_WHEN_CHANGED);
    m_scale->callback([](Fl_Widget*, void* v) { static_cast<ExportDialog*>(v)->onScaleEdit(); },
                      this);
    for (std::size_t i = 0; i < std::size(kQuickScales); ++i) {
        auto* b = new FlatButton(cx0, 0, 40, 22, _(kQuickScaleLabels[i]));
        b->labelsize(11);
        b->user_data(this);
        b->callback([](Fl_Widget* w, void* v) {
            auto* d = static_cast<ExportDialog*>(v);
            // The chips are a shortcut into the SAME path a typed percentage takes.
            for (std::size_t k = 0; k < std::size(kQuickScales); ++k)
                if (d->m_quick[k] == w) {
                    setNum(d->m_scale, kQuickScales[k]);
                    d->onScaleEdit();
                }
        });
        m_quick[i] = b;
    }
    m_capW = fieldCaption(cx0, 0, 100, _("Width (px)"), pal);
    m_wField = new NumberField(cx0, 0, 100, 26);
    styleInput(m_wField, pal);
    m_wField->when(FL_WHEN_CHANGED);
    m_wField->callback([](Fl_Widget*, void* v) { static_cast<ExportDialog*>(v)->onWidthEdit(); },
                       this);
    m_capH = fieldCaption(cx0, 0, 100, _("Height (px)"), pal);
    m_hField = new NumberField(cx0, 0, 100, 26);
    styleInput(m_hField, pal);
    m_hField->when(FL_WHEN_CHANGED);
    m_hField->callback([](Fl_Widget*, void* v) { static_cast<ExportDialog*>(v)->onHeightEdit(); },
                       this);
    m_aspect = new CheckBox(cx0, 0, cw0, 20, _("Lock aspect ratio"),
                            [this](bool) { onAspectToggle(); });
    m_aspect->setChecked(true);
    m_aspect->setGroundColor(pal.panelBg);
    m_capDpi = fieldCaption(cx0, 0, 100, _("Resolution (ppi)"), pal);
    m_dpiField = new NumberField(cx0, 0, 100, 26);
    styleInput(m_dpiField, pal);
    m_dpiField->when(FL_WHEN_CHANGED);
    m_dpiField->callback(
        [](Fl_Widget* w, void* v) {
            auto* d = static_cast<ExportDialog*>(v);
            d->m_dpi = std::clamp(readNum(static_cast<Fl_Input*>(w), d->m_dpi), 1.0, 10000.0);
            d->requestEstimate();
        },
        this);

    // ---- resize quality (§6.4). The Move tool's own AA setting is a DIFFERENT knob -- this one
    // governs only the export resize.
    m_hQuality = new SectionHeader(cx0, 0, cw0, 18, _("Resize quality"));
    m_filterPick = new Dropdown(cx0, 0, cw0, 26);
    for (const FilterChoice& fc : kFilterChoices)
        m_filterPick->add(_(fc.label));
    m_filterPick->value(0);
    m_filterPick->callback(
        [](Fl_Widget* w, void* v) {
            auto* d = static_cast<ExportDialog*>(v);
            const int i = std::clamp(static_cast<Dropdown*>(w)->value(), 0,
                                     static_cast<int>(std::size(kFilterChoices)) - 1);
            d->m_filter = kFilterChoices[static_cast<std::size_t>(i)].filter;
            d->requestEstimate();
        },
        this);

    // ---- the format's own options, RENDERED FROM ITS SCHEMA ---------------------------------
    m_hOptions = new SectionHeader(cx0, 0, cw0, 18, _("Format settings"));
    m_optionsNote = mutedLine(cx0, 0, cw0, 26, pal);
    m_optionsNote->copy_label(_("This format has no settings to choose."));
    m_optionsNote->hide();
    // The generated rows live in their own group so a format switch can clear() exactly them --
    // never the ScrollView itself, whose clear() would take its scrollbars with it.
    m_optionsGroup = new Fl_Group(cx0, 0, cw0, 1);
    m_optionsGroup->box(FL_NO_BOX);
    m_optionsGroup->resizable(nullptr); // rows are placed absolutely; never rescaled
    m_optionsGroup->end();

    // ---- colour & metadata (§6.6). None of these three is an encoder option: they are what the
    // FILE carries beside the pixels, and which of them mean anything is answered by the chosen
    // format's caps, never by its name (refreshMetadataSection).
    m_hColor = new SectionHeader(cx0, 0, cw0, 18, _("Colour and metadata"));
    m_matteLabel = fieldCaption(cx0, 0, cw0, _("Matte colour"), pal);
    m_matteChip = new SwatchChip(cx0, 0, cw0, 24);
    m_matteChip->setColour(m_matte);
    m_matteChip->setInteractive(true);
    m_matteChip->setGroundColor(pal.panelBg);
    m_matteChip->tooltip(_("Colour that fills transparency in a format without an alpha channel"));
    m_matteChip->setOnClick([this] { openMatteFlyout(); });

    // The profile row. The item ORDER is IccChoice's order. Labels are fixed source strings and
    // the profile's real name goes in the note beneath: a profile description routinely contains a
    // '/' ("Coated FOGRA39 / ISO 12647-2"), and Fl_Menu_ reads a '/' in an item label as a submenu
    // separator -- a name pasted into a menu item is how that trap fires.
    m_iccLabel = fieldCaption(cx0, 0, cw0, _("Colour profile"), pal);
    m_iccPick = new Dropdown(cx0, 0, cw0, 26);
    m_iccPick->add(_("Do not embed a profile"));
    m_iccPick->add(_("This document's profile"));
    m_iccPick->add(_("Choose a profile…"));
    m_iccPick->value(static_cast<int>(m_iccChoice));
    m_iccPick->callback(
        [](Fl_Widget* w, void* v) {
            auto* d = static_cast<ExportDialog*>(v);
            const int picked = std::clamp(static_cast<Dropdown*>(w)->value(), 0, 2);
            if (picked == static_cast<int>(IccChoice::Custom)) {
                d->chooseIccProfile(); // may cancel, and then restores the previous selection
                return;
            }
            d->m_iccChoice = static_cast<IccChoice>(picked);
            d->refreshMetadataSection();
            d->updateLossBanner();
            d->requestEstimate();
        },
        this);
    m_iccNote = mutedLine(cx0, 0, cw0, 28, pal);

    // The metadata row. One switch over the whole side-car payload -- EXIF and the print
    // resolution -- because that is how the privacy question is actually asked ("does this file
    // tell anyone where I was?"), and io::buildMetadata honours it in exactly one place.
    m_metaCheck = new CheckBox(cx0, 0, cw0, 20, _("Keep camera metadata"), [this](bool on) {
        m_keepMetadata = on;
        refreshMetadataSection();
        updateLossBanner();
        requestEstimate();
    });
    m_metaCheck->setChecked(m_keepMetadata);
    m_metaCheck->setGroundColor(pal.panelBg);
    m_metaNote = mutedLine(cx0, 0, cw0, 28, pal);

    m_bottomSpacer = new Fl_Box(cx0, 0, cw0, 1);
    m_bottomSpacer->box(FL_NO_BOX);

    m_side->end();

    // ---- pinned at the column's foot: what the current settings actually produce -------------
    {
        const int sx = kSideX + kPad;
        // The same column width the scroll's content uses (its gutter included), so the info block
        // and the banner line up with the controls above them instead of running 16px wider.
        const int sw = kSideW - 2 * kPad - Fl::scrollbar_size();
        int sy = kBodyH - kStatusH;
        auto* rule = new Fl_Box(kSideX + 1, sy, kSideW - 1, 1);
        rule->box(FL_FLAT_BOX);
        rule->color(toFl(pal.border));
        sy += 12;
        m_infoDims = new Fl_Box(sx, sy, sw, 16, "");
        m_infoDims->box(FL_NO_BOX);
        m_infoDims->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        m_infoDims->labelfont(FL_HELVETICA_BOLD);
        m_infoDims->labelsize(12);
        m_infoDims->labelcolor(toFl(pal.text));
        sy += 18;
        // A ScrollingLabel, because an encoder error lands here and those are arbitrarily long
        // (a truncated failure message is the worst kind of readout).
        m_infoSize = new ScrollingLabel(sx, sy, sw, 16);
        m_infoSize->setAlign(ScrollingLabel::Align::Left);
        m_infoSize->labelsize(12);
        m_infoSize->labelcolor(toFl(pal.textMuted));
        // ScrollingLabel erases its cell with color() before drawing (that is how it avoids
        // smearing a scrolled line); the default is the WINDOW ground, and this one sits on the
        // panel -- unset, it would paint a visibly darker slab across the info block.
        m_infoSize->color(toFl(pal.panelBg));
        sy += 22;
        m_banner = new LossBanner(sx, sy, sw, kBodyH - 12 - sy);
    }

    // ---- the action bar, spanning both panes ------------------------------------------------
    {
        auto* rule = new Fl_Box(0, kBodyH, kWinW, 1);
        rule->box(FL_FLAT_BOX);
        rule->color(toFl(pal.border));
        const int by = kBodyH + (kBarH - 32) / 2;
        m_exportBtn = new FilledButton(kWinW - kMargin - 116, by, 116, 32, _("Export"));
        m_exportBtn->callback([](Fl_Widget*, void* v) { static_cast<ExportDialog*>(v)->doExport(); },
                              this);
        auto* cancel = new FlatButton(kWinW - kMargin - 116 - 10 - 96, by, 96, 32, _("Cancel"));
        cancel->callback([](Fl_Widget*, void* v) { static_cast<ExportDialog*>(v)->doCancel(); },
                         this);

        const int browseW = 36;
        const int browseX = cancel->x() - 20 - browseW;
        const int fieldX = kMargin + 62;
        auto* cap = fieldCaption(kMargin, kBodyH + (kBarH - 15) / 2, 56, _("Save to"), pal);
        cap->labelsize(12);
        m_pathField = new TextInput(fieldX, kBodyH + (kBarH - 28) / 2, browseX - 8 - fieldX, 28);
        styleInput(m_pathField, pal);
        m_pathField->tooltip(_("Where the file is written. Leave it empty and Export will ask."));
        auto* browse = new FlatButton(browseX, kBodyH + (kBarH - 28) / 2, browseW, 28, "...");
        browse->tooltip(_("Choose a file…"));
        browse->callback([](Fl_Widget*, void* v) { static_cast<ExportDialog*>(v)->doBrowse(); },
                         this);
    }

    // The interactive Matte swatch opens a ColorFlyout -- a child sub-window created BEFORE the
    // DropdownPopup so the popup stacks above it (the fill_dialog ordering rule). Built here, before
    // show(), so FLTK realizes them as real sub-surfaces of this modal.
    m_matteFlyout = new ColorFlyout();
    m_matteFlyout->hide();
    m_matteFlyout->setOnPick([this](common::Color8 c) {
        m_matte = c;
        m_matteChip->setColour(c);
        requestEstimate();
    });
    m_matteFlyout->setUseForeground([] { return common::Color8{255, 255, 255, 255}; });
    (new DropdownPopup())->hide(); // the themed list for the preset / format / option dropdowns
    (new ContextMenu())->hide();   // the themed right-click menu for the text fields

    end();
    size_range(kWinW, kWinH, kWinW, kWinH); // fixed layout (also keeps the child sub-windows fixed)
    callback([](Fl_Widget* w, void*) { static_cast<ExportDialog*>(w)->doCancel(); }); // Esc / WM close
    set_modal();

    // Seed the fields + preview.
    setNum(m_scale, 100.0);
    setNum(m_wField, static_cast<double>(m_baseW));
    setNum(m_hField, static_cast<double>(m_baseH));
    setNum(m_dpiField, m_dpi);
    // §6.8: the path is EMPTY on a first export (clicking Export then opens the system picker) and
    // pre-filled with the remembered target afterwards. It is never seeded with a bare file name:
    // a relative path is what made exports land in the process working directory.
    m_pathField->value(req.target.path.c_str());
    m_preview->setImage(*m_base);
    rebuildOptionsPanel(); // ... which ends in relayoutSide()
    updateInfo();
    updateLossBanner();
    requestEstimate();
}

ExportDialog::~ExportDialog() {
    Fl::remove_timeout(debounceThunk, this);
    Fl::remove_timeout(pollThunk, this);
    // m_job's destructor asks the worker to bail and joins it.
}

std::vector<platform::FileFilter> ExportDialog::currentFilters() const {
    std::vector<platform::FileFilter> f;
    if (const io::FormatBackend* b = backend()) {
        platform::FileFilter one;
        one.name = _(std::string(b->displayName()).c_str());
        for (const std::string& e : b->extensions())
            one.globs.push_back("*." + e);
        one.mimeTypes.push_back(std::string(b->mimeType()));
        f.push_back(std::move(one));
    }
    f.push_back({_("All files"), {"*"}, {}});
    return f;
}

io::OptionValues& ExportDialog::valuesForCurrentFormat() {
    const io::FormatBackend* b = backend();
    static io::OptionValues empty;
    if (b == nullptr)
        return empty;
    const int key = static_cast<int>(b->id());
    if (auto it = m_valuesByFormat.find(key); it != m_valuesByFormat.end())
        return it->second;
    return m_valuesByFormat.emplace(key, b->optionsSchema().defaults()).first->second;
}

// ---- the schema-driven options panel ---------------------------------------------------------
//
// THIS is the payoff of OptionsSchema: the panel is rendered from data, so a new backend needs no
// UI code at all. Nothing below mentions a format, only a type, a range and a condition.

void ExportDialog::rebuildOptionsPanel() {
    dismissActiveDropdownPopup(); // a live popup must not outlive the Dropdown it belongs to
    m_optionsGroup->clear();      // delete the widgets FIRST: they carry OptionRow* as user data
    m_rows.clear();               // ... and only then the rows they pointed at
    m_optionsGroup->resizable(nullptr); // Fl_Group::clear() resets resizable to self

    const io::FormatBackend* b = backend();
    if (b == nullptr) {
        m_optionsNote->show();
        m_hColor->hide();
        m_matteLabel->hide();
        m_matteChip->hide();
        m_iccLabel->hide();
        m_iccPick->hide();
        m_iccNote->hide();
        m_metaCheck->hide();
        m_metaNote->hide();
        relayoutSide();
        return;
    }
    const Palette& pal = activePalette();
    const io::OptionsSchema schema = b->optionsSchema();
    io::OptionValues& values = valuesForCurrentFormat();
    schema.coerce(values); // a stored bag from an older schema is made complete and legal

    m_formatFacts->copy_label(formatFacts(b->caps()).c_str());

    const int x = m_optionsGroup->x();
    const int w = m_optionsGroup->w();

    m_optionsGroup->begin();
    // Order: the always-visible common section first (group ""), then each declared group behind
    // its own disclosure header. Groups are a FLAT table in the schema, so this is one pass.
    std::vector<std::string> groupOrder{std::string()};
    for (const io::OptionGroup& g : schema.groups)
        groupOrder.push_back(g.id);

    for (const std::string& gid : groupOrder) {
        const io::OptionGroup* group = nullptr;
        for (const io::OptionGroup& g : schema.groups)
            if (g.id == gid)
                group = &g;
        bool any = false;
        for (const io::OptionDesc& d : schema.options)
            any = any || d.group == gid;
        if (!any)
            continue;

        if (group != nullptr) {
            auto* header = new GroupHeaderButton(x, 0, w, 22);
            header->setText(_(group->label.c_str()));
            auto row = std::make_unique<OptionRow>();
            row->dlg = this;
            row->isHeader = true;
            row->groupId = gid;
            row->control = header;
            row->height = 22;
            header->user_data(row.get());
            header->callback([](Fl_Widget* wid, void*) {
                auto* r = static_cast<OptionRow*>(wid->user_data());
                r->dlg->toggleGroup(r->groupId);
            });
            m_rows.push_back(std::move(row));
        }

        for (const io::OptionDesc& d : schema.options) {
            if (d.group != gid)
                continue;
            auto row = std::make_unique<OptionRow>();
            row->dlg = this;
            row->desc = d; // the row owns its descriptor; FLTK label pointers must not dangle
            row->groupId = gid;
            const char* label = _(row->desc.label.c_str());
            switch (d.type) {
            case io::OptionType::Bool: {
                auto* cb = new CheckBox(x, 0, w, 20, nullptr);
                cb->copy_label(label);
                cb->setGroundColor(pal.panelBg);
                cb->setChecked(values.boolean(d.key));
                OptionRow* raw = row.get();
                cb->setOnToggle([raw](bool on) {
                    raw->dlg->valuesForCurrentFormat().set(raw->desc.key, io::boolValue(on));
                    raw->dlg->onOptionEdited(*raw);
                });
                row->control = cb;
                row->height = 20;
                break;
            }
            case io::OptionType::Int:
            case io::OptionType::Real: {
                row->label = fieldCaption(x, 0, 96, nullptr, pal);
                row->label->copy_label(label);
                row->label->size(96, 20);
                auto* s = new Slider(x, 0, 40, 18);
                s->range(d.min, d.max);
                s->step(d.type == io::OptionType::Int ? std::max(1.0, d.step)
                                                      : std::max(0.001, d.step));
                s->value(d.type == io::OptionType::Int
                             ? static_cast<double>(values.integer(d.key))
                             : values.number(d.key));
                s->setCellColor(pal.panelBg);
                s->when(FL_WHEN_CHANGED);
                s->user_data(row.get());
                s->callback([](Fl_Widget* wid, void*) {
                    auto* r = static_cast<OptionRow*>(wid->user_data());
                    const double v = static_cast<Fl_Slider*>(wid)->value();
                    r->dlg->valuesForCurrentFormat().set(
                        r->desc.key, r->desc.type == io::OptionType::Int
                                         ? io::intValue(static_cast<int>(std::lround(v)))
                                         : io::realValue(v));
                    r->dlg->onOptionEdited(*r);
                });
                row->control = s;
                auto* out = new Fl_Box(x, 0, 44, 20, "");
                out->box(FL_NO_BOX);
                out->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
                out->labelfont(FL_HELVETICA);
                out->labelsize(11);
                out->labelcolor(toFl(pal.text));
                row->readout = out;
                row->height = 20;
                break;
            }
            case io::OptionType::Enum: {
                row->label = fieldCaption(x, 0, 96, nullptr, pal);
                row->label->copy_label(label);
                row->label->size(96, 24);
                auto* dd = new Dropdown(x, 0, 40, 24);
                int sel = 0;
                for (std::size_t k = 0; k < d.choices.size(); ++k) {
                    dd->add(_(d.choices[k].label.c_str()));
                    if (d.choices[k].id == values.text(d.key))
                        sel = static_cast<int>(k);
                }
                dd->value(sel);
                dd->user_data(row.get());
                dd->callback([](Fl_Widget* wid, void*) {
                    auto* r = static_cast<OptionRow*>(wid->user_data());
                    const int i = std::clamp(static_cast<Dropdown*>(wid)->value(), 0,
                                             static_cast<int>(r->desc.choices.size()) - 1);
                    if (!r->desc.choices.empty())
                        r->dlg->valuesForCurrentFormat().set(
                            r->desc.key,
                            io::textValue(r->desc.choices[static_cast<std::size_t>(i)].id));
                    r->dlg->onOptionEdited(*r);
                });
                row->control = dd;
                row->height = 24;
                break;
            }
            case io::OptionType::Text: {
                row->label = fieldCaption(x, 0, 96, nullptr, pal);
                row->label->copy_label(label);
                row->label->size(96, 24);
                auto* in = new TextInput(x, 0, 40, 24);
                styleInput(in, pal);
                in->value(values.text(d.key).c_str());
                in->when(FL_WHEN_CHANGED);
                in->user_data(row.get());
                in->callback([](Fl_Widget* wid, void*) {
                    auto* r = static_cast<OptionRow*>(wid->user_data());
                    const char* t = static_cast<Fl_Input*>(wid)->value();
                    r->dlg->valuesForCurrentFormat().set(r->desc.key,
                                                         io::textValue(t != nullptr ? t : ""));
                    r->dlg->onOptionEdited(*r);
                });
                row->control = in;
                row->height = 24;
                break;
            }
            }
            if (row->control != nullptr && !row->desc.help.empty())
                row->control->copy_tooltip(_(row->desc.help.c_str()));
            m_rows.push_back(std::move(row));
        }
    }
    m_optionsGroup->end();
    Fl_Group::current(nullptr); // end() leaves the SCROLL current; nothing else may drift into it

    if (m_rows.empty())
        m_optionsNote->show();
    else
        m_optionsNote->hide();

    refreshMetadataSection();
    relayoutSide();
}

// Every row here answers to FormatCaps. Two rules, and they are the section's whole design:
//
//   * A row that means NOTHING for this format is hidden (the matte, for a format with an alpha
//     channel -- there is no transparency to fill).
//   * A row that means something but that the format CANNOT HONOUR is shown, DISABLED, and says
//     why in its own note. Silently ignoring the control would make it a lie, and hiding it would
//     leave the user wondering where the profile went.
//
// Nothing below names a format. It could not: the dialog has no way to ask.
void ExportDialog::refreshMetadataSection() {
    const io::FormatBackend* b = backend();
    if (b == nullptr)
        return;
    const io::FormatCaps caps = b->caps();

    // ---- matte: only for a format with no alpha, over a document that has some ----
    const bool wantMatte = caps.alpha == io::AlphaKind::None && m_profile.hasAlpha;
    if (wantMatte) {
        m_matteLabel->show();
        m_matteChip->show();
    } else {
        m_matteLabel->hide();
        m_matteChip->hide();
    }

    // ---- colour profile ----
    m_iccLabel->show();
    m_iccPick->show();
    m_iccNote->show();
    // "This document's profile" is only a choice when there IS one. Deactivating the ITEM rather
    // than dropping it keeps the item indices equal to IccChoice for every document.
    m_iccPick->mode(static_cast<int>(IccChoice::Document),
                    m_docIcc.empty() ? FL_MENU_INACTIVE : 0);
    m_iccPick->value(static_cast<int>(m_iccChoice));
    std::string iccNote;
    if (!caps.icc) {
        m_iccPick->deactivate();
        iccNote = _("This format cannot embed a colour profile. The file will be read as sRGB.");
    } else {
        m_iccPick->activate();
        switch (m_iccChoice) {
        case IccChoice::None:
            iccNote = _("No profile is written; the file will be read as sRGB.");
            break;
        case IccChoice::Document:
            if (m_docIcc.empty())
                iccNote = _("This document has no profile of its own -- it is plain sRGB, which "
                            "needs no tag.");
            else
                iccNote = std::string(_("Embedding: ")) + m_docIccName;
            break;
        case IccChoice::Custom:
            iccNote = m_customIcc.empty()
                          ? std::string(_("No profile chosen yet."))
                          : std::string(_("Embedding: ")) + m_customIccName;
            break;
        }
    }
    m_iccNote->copy_label(iccNote.c_str());

    // ---- camera metadata ----
    m_metaCheck->show();
    m_metaNote->show();
    m_metaCheck->setChecked(m_keepMetadata);
    std::string metaNote;
    if (caps.metadata == io::MetadataKind::None) {
        // Nothing to switch: the container has nowhere to put any of it.
        m_metaCheck->deactivate();
        metaNote = _("This format stores no metadata of any kind.");
    } else {
        m_metaCheck->activate();
        if (!m_keepMetadata) {
            metaNote = _("Nothing beside the pixels is written -- no camera data, no location, no "
                         "print resolution.");
        } else {
            // What will ACTUALLY be written: the intersection of what the document carries with
            // what this format can hold. Assembled from caps bits, so a backend that earns a bit
            // starts appearing here with no change to this function.
            std::vector<std::string> kept;
            std::vector<std::string> lost;
            const auto note = [&](bool present, bool carried, const char* what) {
                if (!present)
                    return;
                (carried ? kept : lost).emplace_back(what);
            };
            note(m_exif.has_value(), io::has(caps.metadata, io::MetadataKind::Exif),
                 _("camera and lens data"));
            note(std::abs(m_dpi - 72.0) > 1e-9, io::has(caps.metadata, io::MetadataKind::Dpi),
                 _("the print resolution"));
            const auto join = [](const std::vector<std::string>& parts) {
                std::string out;
                for (const std::string& p : parts) {
                    if (!out.empty())
                        out += ", ";
                    out += p;
                }
                return out;
            };
            // The "label: value" idiom the info block already uses, so the fragments a translator
            // sees are whole phrases rather than a sentence assembled out of words.
            if (!kept.empty())
                metaNote = std::string(_("Stores: ")) + join(kept);
            if (!lost.empty()) {
                if (!metaNote.empty())
                    metaNote += " \xC2\xB7 "; // a middle dot, as everywhere else in the readouts
                metaNote += std::string(_("cannot store: ")) + join(lost);
            }
            if (metaNote.empty())
                metaNote = _("This document carries no camera metadata to keep.");
        }
    }
    m_metaNote->copy_label(metaNote.c_str());

    // The section head belongs to whichever rows are up; the profile and metadata rows always are,
    // so it always is -- but the check stays, because a future row could be the only one left.
    if (m_matteChip->visible() || m_iccPick->visible() || m_metaCheck->visible())
        m_hColor->show();
    else
        m_hColor->hide();
}

// What the USER picked, deliberately WITHOUT consulting the format's caps. It matters: a format
// that cannot embed a profile must still see that one was on the table, because that is precisely
// what the loss banner has to warn about. Filtering it here instead would silence the warning --
// the format's own encoder ignores a payload it cannot carry, which is where the filtering belongs.
const std::vector<std::uint8_t>& ExportDialog::selectedIcc() const {
    static const std::vector<std::uint8_t> kNone;
    switch (m_iccChoice) {
    case IccChoice::Document: return m_docIcc;
    case IccChoice::Custom: return m_customIcc;
    case IccChoice::None: break;
    }
    return kNone;
}

io::MetadataRequest ExportDialog::metadataRequest() const {
    io::MetadataRequest want;
    want.keepMetadata = m_keepMetadata;
    // "The user wants a profile embedded" -- not "one can be", which is the caps question diff()
    // asks for itself.
    want.embedIcc = m_iccChoice != IccChoice::None;
    return want;
}

void ExportDialog::chooseIccProfile() {
    const IccChoice previous = m_iccChoice;
    platform::FileDialogRequest req;
    req.title = _("Choose a colour profile");
    req.acceptLabel = _("Choose");
    req.parent = this;
    req.startFolder = seed().folder;
    // Globs only, no mime list: KDE's picker filters by the mime list INSTEAD of the globs when a
    // filter carries both, and there is no settled mime type for .icc/.icm (the standing rule in
    // platform/file_dialog.hpp).
    req.filters = {{_("ICC colour profile"), {"*.icc", "*.icm", "*.ICC", "*.ICM"}, {}},
                   {_("All files"), {"*"}, {}}};
    const std::optional<std::string> chosen = platform::showOpenDialog(req);
    if (!chosen || chosen->empty()) {
        m_iccPick->value(static_cast<int>(previous)); // cancelled: the selection never moved
        return;
    }
    // Validate before adopting: "the user picked a file" is not evidence that it is a profile, and
    // the one place to find that out is here, not on the encode thread.
    std::vector<std::uint8_t> bytes = io::readIccProfile(*chosen);
    if (bytes.empty()) {
        m_iccPick->value(static_cast<int>(previous));
        m_iccNote->copy_label(_("That file is not a colour profile."));
        m_iccNote->redraw();
        return;
    }
    m_customIcc = std::move(bytes);
    m_customIccName = core::iccProfileName(*chosen);
    if (m_customIccName.empty())
        m_customIccName = io::fileNameOf(*chosen); // no description tag: show the file name
    m_iccChoice = IccChoice::Custom;
    m_iccPick->value(static_cast<int>(IccChoice::Custom));
    refreshMetadataSection();
    relayoutSide();
    updateLossBanner();
    requestEstimate();
}

bool ExportDialog::groupCollapsed(const io::OptionsSchema& schema,
                                  const std::string& groupId) const {
    if (groupId.empty())
        return false; // the always-visible common section has no header and never collapses
    if (const auto it = m_groupOpen.find(groupId); it != m_groupOpen.end())
        return !it->second; // the user decided
    for (const io::OptionGroup& g : schema.groups)
        if (g.id == groupId)
            return g.collapsedByDefault;
    return false;
}

void ExportDialog::toggleGroup(const std::string& groupId) {
    const io::FormatBackend* b = backend();
    if (b == nullptr)
        return;
    const bool nowCollapsed = groupCollapsed(b->optionsSchema(), groupId);
    m_groupOpen[groupId] = nowCollapsed; // collapsed -> open, open -> collapsed
    relayoutSide();
}

int ExportDialog::layoutOptionRows(int top, int cx, int cw) {
    const io::FormatBackend* b = backend();
    if (b == nullptr)
        return top;
    const io::OptionsSchema schema = b->optionsSchema();
    const io::OptionValues& values = valuesForCurrentFormat();

    int y = top;
    for (const std::unique_ptr<OptionRow>& row : m_rows) {
        const bool isHeader = row->isHeader;
        const bool collapsed = groupCollapsed(schema, row->groupId);
        // A hidden option is a UI affordance only -- its value STAYS in the bag (§2.1), which is
        // why encode() consults visible() too.
        const bool conditionOk = isHeader || schema.visible(row->desc, values);
        const bool shown = conditionOk && (isHeader || !collapsed);
        if (!shown) {
            const auto park = [&](Fl_Widget* wid) {
                if (wid == nullptr)
                    return;
                wid->position(cx, y);
                wid->hide();
            };
            park(row->label);
            park(row->control);
            park(row->readout);
            continue;
        }
        if (isHeader) {
            auto* header = static_cast<GroupHeaderButton*>(row->control);
            header->setOpen(!collapsed);
            header->resize(cx, y + 6, cw, row->height);
            header->show();
            y += row->height + 6 + 6;
            continue;
        }
        const int labelW = row->label != nullptr ? 96 : 0;
        const int gap = labelW > 0 ? 8 : 0;
        const int readoutW = row->readout != nullptr ? 44 : 0;
        const int controlX = cx + labelW + gap;
        const int controlW = std::max(30, cw - labelW - gap - readoutW);
        if (row->label != nullptr) {
            row->label->resize(cx, y + (row->height - row->label->h()) / 2, labelW,
                               row->label->h());
            row->label->show();
        }
        if (row->control != nullptr) {
            row->control->resize(controlX, y + (row->height - row->control->h()) / 2, controlW,
                                 row->control->h());
            row->control->show();
        }
        if (row->readout != nullptr) {
            row->readout->resize(cx + cw - readoutW, y, readoutW, row->height);
            row->readout->show();
            char buf[32];
            if (row->desc.type == io::OptionType::Int)
                std::snprintf(buf, sizeof buf, "%d", values.integer(row->desc.key));
            else
                std::snprintf(buf, sizeof buf, "%.*f", std::max(0, row->desc.decimals),
                              values.number(row->desc.key));
            row->readout->copy_label(buf);
        }
        y += row->height + 8;
    }
    return y;
}

// The settings column in ONE pass, top to bottom. Every control's position is decided here (and
// only here), which is what keeps the vertical rhythm consistent as the options section grows and
// shrinks under it. Positions are relative to the scroll's own offset, because Fl_Scroll scrolls
// by physically moving its children.
void ExportDialog::relayoutSide() {
    const int cx = m_side->x() + kPad - 1;
    const int cw = std::max(120, m_side->w() - 2 * kPad - Fl::scrollbar_size() + 1);
    const int col = (cw - 12) / 2;
    const int col2 = cx + cw - col;
    int y = m_side->y() + 12 - m_side->yposition();

    const auto put = [](Fl_Widget* wid, int X, int Y, int W) {
        if (wid != nullptr)
            wid->resize(X, Y, W, wid->h());
    };

    put(m_hPreset, cx, y, cw);
    y += m_hPreset->h() + 6;
    put(m_preset, cx, y, cw);
    y += m_preset->h() + kSectionGap;

    put(m_hFormat, cx, y, cw);
    y += m_hFormat->h() + 6;
    put(m_format, cx, y, cw);
    y += m_format->h() + 5;
    put(m_formatFacts, cx, y, cw);
    y += m_formatFacts->h() + kSectionGap;

    put(m_hSize, cx, y, cw);
    y += m_hSize->h() + 8;
    put(m_capScale, cx, y, cw);
    y += m_capScale->h() + 2;
    put(m_scaleSlider, cx, y + 3, cw - 68);
    put(m_scale, cx + cw - 60, y, 60);
    y += m_scale->h() + 8;
    {
        const int n = static_cast<int>(std::size(kQuickScales));
        const int bw = (cw - (n - 1) * 6) / n;
        for (int i = 0; i < n; ++i)
            put(m_quick[static_cast<std::size_t>(i)], cx + i * (bw + 6), y, bw);
        y += m_quick[0]->h() + 14;
    }
    put(m_capW, cx, y, col);
    put(m_capH, col2, y, col);
    y += m_capW->h() + 2;
    put(m_wField, cx, y, col);
    put(m_hField, col2, y, col);
    y += m_wField->h() + 8;
    put(m_aspect, cx, y, cw);
    y += m_aspect->h() + 10;
    put(m_capDpi, cx, y, col);
    y += m_capDpi->h() + 2;
    put(m_dpiField, cx, y, col);
    y += m_dpiField->h() + kSectionGap;

    put(m_hQuality, cx, y, cw);
    y += m_hQuality->h() + 6;
    put(m_filterPick, cx, y, cw);
    y += m_filterPick->h() + kSectionGap;

    put(m_hOptions, cx, y, cw);
    y += m_hOptions->h() + 6;
    put(m_optionsNote, cx, y, cw); // placed either way; a hidden note is parked in-bounds
    if (m_optionsNote->visible())
        y += m_optionsNote->h() + 4;
    const int optTop = y;
    const int optEnd = layoutOptionRows(optTop, cx, cw);
    // Fl_Widget::resize, EXPLICITLY: Fl_Group::resize would translate the rows this function has
    // just placed. The group is only a bounding box for Fl_Scroll's range.
    m_optionsGroup->Fl_Widget::resize(cx, optTop, cw, std::max(1, optEnd - optTop));
    y = optEnd;
    if (optEnd > optTop)
        y += kSectionGap - 8; // the last row already carries its own 8px trailer

    // ---- colour & metadata. Which rows are up depends on the format's caps (and the matte row
    // also on the document), so this is a RUNNING CURSOR rather than a fixed block: every widget is
    // positioned either way -- a hidden one parked in-bounds is what keeps Fl_Scroll's range from
    // stretching -- but only a visible row advances the cursor.
    const auto stack = [&](Fl_Widget* wid, int trailer) {
        if (wid == nullptr)
            return;
        wid->resize(cx, y, cw, wid->h());
        if (wid->visible())
            y += wid->h() + trailer;
    };
    const bool anyColorRow = m_hColor->visible();
    stack(m_hColor, 8);
    stack(m_matteLabel, 2);
    stack(m_matteChip, 14);
    stack(m_iccLabel, 2);
    stack(m_iccPick, 4);
    stack(m_iccNote, 12);
    stack(m_metaCheck, 4);
    stack(m_metaNote, 0);
    if (anyColorRow)
        y += kSectionGap;

    // The column's tail padding has to be a real child: Fl_Scroll derives its range from the
    // children's bounding box, so a bare `y += 12` would scroll the last control flush to the edge.
    put(m_bottomSpacer, cx, y + 12, cw);
    m_side->redraw();
}

void ExportDialog::onOptionEdited(const OptionRow&) {
    // One re-layout answers both jobs: a condition source may have flipped (dependent rows appear
    // or vanish) and the numeric readouts are refreshed from the values.
    relayoutSide();
    m_preset->value(0); // the settings no longer match a preset
    updateLossBanner();
    requestEstimate();
}

void ExportDialog::onPresetPicked() {
    const int i = m_preset->value() - 1; // index 0 is "Custom"
    if (i < 0 || i >= static_cast<int>(std::size(kPresets)))
        return;
    const ExportPreset& p = kPresets[static_cast<std::size_t>(i)];
    int target = -1;
    for (std::size_t k = 0; k < m_backends.size(); ++k)
        if (m_backends[k]->id() == p.format)
            target = static_cast<int>(k);
    if (target < 0) { // e.g. "Lossless archive" on a build without libjxl
        m_preset->value(0);
        return;
    }
    m_format->value(target);
    io::OptionValues& values = valuesForCurrentFormat();
    values = m_backends[static_cast<std::size_t>(target)]->optionsSchema().defaults();
    for (const auto& [key, value] : p.values)
        values.set(key, value);
    const int keep = m_preset->value();
    onFormatChanged();
    m_preset->value(keep); // onFormatChanged's edit path resets it to Custom; the preset stands
}

void ExportDialog::onFormatChanged() {
    // Re-point the path field's extension at the new format (keep the directory + stem).
    const std::string cur = m_pathField->value() != nullptr ? m_pathField->value() : "";
    if (!cur.empty()) {
        std::filesystem::path p(cur);
        p.replace_extension(primaryExt());
        m_pathField->value(p.string().c_str());
    }
    // A colour flyout for the (possibly now hidden) matte swatch must not linger.
    if (m_matteFlyout != nullptr && m_matteFlyout->shown())
        m_matteFlyout->hide();
    m_preset->value(0);
    rebuildOptionsPanel();
    updateLossBanner();
    requestEstimate();
}

void ExportDialog::updateLossBanner() {
    const io::FormatBackend* b = backend();
    if (b == nullptr || m_banner == nullptr)
        return;
    // The banner asks "will the profile ON THE TABLE survive?", which is not quite what the
    // document profile records: a user may have picked a profile for an export of a document that
    // has none of its own. So hasICC is answered from what the encoder will actually be handed.
    // (The MetadataRequest carries the other half -- whether the user asked for it at all.)
    io::DocumentProfile profile = m_profile;
    profile.hasICC = !selectedIcc().empty();
    const std::vector<io::LossWarning> warnings =
        io::diff(profile, b->caps(), valuesForCurrentFormat(), metadataRequest());
    const io::Severity worst = io::worstSeverity(warnings);

    std::string text;
    if (warnings.empty()) {
        text = _("Nothing is lost -- this format carries everything the document uses.");
    } else {
        // diff() returns HardLoss first, then Lossy, in a deterministic order, so the top of the
        // list is always the most important thing to say.
        const std::size_t show = std::min<std::size_t>(warnings.size(), 3);
        for (std::size_t i = 0; i < show; ++i) {
            if (!text.empty())
                text += "\n";
            text += lossMessage(warnings[i]);
        }
        if (warnings.size() > show) {
            char more[64];
            // TRANSLATORS: tail of the export loss banner when more warnings than fit are listed.
            std::snprintf(more, sizeof more, _("+ %zu more"), warnings.size() - show);
            text += "\n";
            text += more;
        }
    }
    m_banner->setContent(worst, std::move(text));
}

// The one place the three linked size controls agree: whatever moved, the other two follow and
// the pipeline is asked for a fresh answer.
void ExportDialog::applySize(ExportPixelSize s, bool fromScaleField) {
    m_outW = s.w;
    m_outH = s.h;
    m_syncing = true;
    setNum(m_wField, static_cast<double>(m_outW));
    setNum(m_hField, static_cast<double>(m_outH));
    const double pct = exportScalePercent(m_baseW, m_outW);
    if (!fromScaleField)
        setNum(m_scale, pct);
    m_scaleSlider->value(std::clamp(pct, m_scaleSlider->minimum(), m_scaleSlider->maximum()));
    m_scaleSlider->redraw();
    m_syncing = false;
    updateInfo();
    requestEstimate();
}

void ExportDialog::onScaleEdit() {
    if (m_syncing)
        return;
    applySize(exportSizeFromScale(m_baseW, m_baseH, readNum(m_scale, 100.0)),
              /*fromScaleField=*/true);
}

void ExportDialog::onScaleSlider() {
    if (m_syncing)
        return;
    const double pct = m_scaleSlider->value();
    m_syncing = true;
    setNum(m_scale, pct);
    m_syncing = false;
    applySize(exportSizeFromScale(m_baseW, m_baseH, pct), /*fromScaleField=*/true);
}

void ExportDialog::onWidthEdit() {
    if (m_syncing)
        return;
    applySize(exportSizeFromWidth(m_baseW, m_baseH, readNum(m_wField, m_outW), m_outH,
                                  m_aspect->checked()),
              /*fromScaleField=*/false);
}

void ExportDialog::onHeightEdit() {
    if (m_syncing)
        return;
    applySize(exportSizeFromHeight(m_baseW, m_baseH, readNum(m_hField, m_outH), m_outW,
                                   m_aspect->checked()),
              /*fromScaleField=*/false);
}

void ExportDialog::onAspectToggle() {
    if (!m_aspect->checked())
        return;
    // Re-lock: keep width, snap height back to the source ratio.
    applySize(exportSizeFromWidth(m_baseW, m_baseH, static_cast<double>(m_outW), m_outH,
                                  /*lockAspect=*/true),
              /*fromScaleField=*/false);
}

void ExportDialog::updateInfo() {
    char b[160];
    std::snprintf(b, sizeof b, "%u \xC3\x97 %u px \xC2\xB7 RGBA 8-bit", m_outW, m_outH);
    m_infoDims->copy_label(b);
    m_infoDims->redraw();
}

// ---- the three-stage cache's driver (§5) ------------------------------------------------------
//
// composite (given) -> resize (invalidated by size/filter) -> encode (invalidated by format or
// option). The KEY is what makes the caching honest: it names everything the answer depends on, so
// "nothing actually changed" costs nothing and a slider tick only ever re-runs the stages below
// the thing that moved.
std::string ExportDialog::pipelineKey() {
    const io::FormatBackend* b = backend();
    std::string k = b != nullptr ? std::string(io::formatIdName(b->id())) : std::string("-");
    char dims[96];
    std::snprintf(dims, sizeof dims, "|%ux%u|f%d|d%.4f|", m_outW, m_outH,
                  static_cast<int>(m_filter), m_dpi);
    k += dims;
    if (b != nullptr) {
        // OptionValues is an ORDERED map, so this string is deterministic.
        for (const auto& [key, value] : valuesForCurrentFormat().all())
            k += key + "=" + io::optionValueToString(value) + ";";
        if (b->caps().alpha == io::AlphaKind::None) {
            char m[32];
            std::snprintf(m, sizeof m, "m%02x%02x%02x", m_matte.r, m_matte.g, m_matte.b);
            k += m;
        }
        // The metadata payload changes the FILE SIZE (a press profile is megabytes), so it has to
        // be in the key or the readout would keep answering the previous question. The profile's
        // choice + length identifies it: two different profiles of the same length would have to
        // be picked through the same dropdown item to collide, which cannot happen.
        char meta[48];
        std::snprintf(meta, sizeof meta, "|k%d|i%d|n%zu|x%d", m_keepMetadata ? 1 : 0,
                      static_cast<int>(m_iccChoice), selectedIcc().size(),
                      m_exif.has_value() ? 1 : 0);
        k += meta;
    }
    return k;
}

void ExportDialog::requestEstimate() {
    if (pipelineKey() == m_settledKey && m_job == nullptr)
        return; // the readouts already answer this exact question
    m_pipelineDirty = true;
    m_infoSize->setText(_("File size: rendering…"));
    m_preview->setBusy(true);
    if (m_debouncePending)
        return;
    m_debouncePending = true;
    // Debounced (§5): dragging a quality slider must not launch an encode per tick.
    Fl::add_timeout(0.25, debounceThunk, this);
}

void ExportDialog::startPipeline() {
    const io::FormatBackend* b = backend();
    if (b == nullptr || m_base == nullptr || m_base->empty())
        return;
    m_pipelineDirty = false;

    auto job = std::make_unique<PipelineJob>();
    job->source = m_base;
    // Stage 2 is skipped entirely when the resize we already have still answers (a quality slider
    // drag re-encodes only).
    if (m_resized != nullptr && m_resizedW == m_outW && m_resizedH == m_outH &&
        m_resizedFilter == m_filter)
        job->cached = m_resized;
    job->outW = m_outW;
    job->outH = m_outH;
    job->filter = m_filter;
    job->backend = b;
    job->values = valuesForCurrentFormat();
    b->optionsSchema().coerce(job->values);
    job->matte = m_matte;
    job->dpi = m_dpi;
    job->exif = m_exif;              // copied, not referenced: the worker outlives any edit here
    job->icc = selectedIcc();
    job->keepMetadata = m_keepMetadata;
    job->key = pipelineKey();

    PipelineJob* raw = job.get();
    m_job = std::move(job);
    m_job->worker = std::thread([raw] { runPipeline(raw); });
    if (!m_polling) {
        m_polling = true;
        Fl::add_timeout(0.05, pollThunk, this);
    }
}

void ExportDialog::pollPipeline() {
    if (m_job != nullptr && m_job->done.load()) {
        m_job->worker.join();
        applyJobResult(*m_job);
        m_job.reset();
    }
    if (m_job == nullptr && m_pipelineDirty && !m_debouncePending)
        startPipeline();
    if (m_job != nullptr)
        Fl::repeat_timeout(0.05, pollThunk, this);
    else
        m_polling = false;
    m_preview->setBusy(m_job != nullptr || m_pipelineDirty || m_debouncePending);
}

void ExportDialog::applyJobResult(const PipelineJob& job) {
    m_resized = job.resized;
    m_resizedW = job.outW;
    m_resizedH = job.outH;
    m_resizedFilter = job.filter;
    m_settledKey = job.key;

    if (job.ok) {
        // The exact size, from a real trial encode -- not an estimate.
        m_infoSize->setText(std::string(_("File size: ")) + humanFileSize(job.byteCount));
    } else {
        m_infoSize->setText(std::string(_("File size: ")) +
                            (job.error.empty() ? _("unavailable") : job.error));
    }

    // The preview is the ENCODED result decoded back where we can (so a JPEG really does show its
    // own artefacts), else the resize.
    const std::shared_ptr<const common::Image> shown = job.preview ? job.preview : job.resized;
    if (shown != nullptr && !shown->empty())
        m_preview->setImage(*shown);
}

void ExportDialog::openMatteFlyout() {
    if (m_matteFlyout == nullptr)
        return;
    if (m_matteFlyout->shownForAnchor(m_matteChip)) { // re-click toggles it shut
        m_matteFlyout->hide();
        return;
    }
    m_matteFlyout->openFor(m_matteChip, m_matte);
}

io::ExportSeed ExportDialog::seed() const {
    // The whole path policy in one call: last export -> document directory -> pictures -> home,
    // and never the process working directory (io/export_path.hpp).
    return io::seedExportTarget(m_pathInputs, m_stem, primaryExt());
}

std::optional<std::string> ExportDialog::askForPath() {
    const io::ExportSeed s = seed();
    platform::FileDialogRequest req;
    req.title = _("Export");
    req.acceptLabel = _("Export");
    req.parent = this;
    // A BASE NAME and an ABSOLUTE FOLDER, kept apart. Handing the picker a bare "shot.png" as its
    // start location is what produced "http://shot.png" on KDE (platform/file_dialog.cpp).
    const std::string typed = m_pathField->value() != nullptr ? m_pathField->value() : "";
    std::string name = io::fileNameOf(typed);
    req.suggestedName = name.empty() ? s.name : name;
    req.startFolder = s.folder;
    if (const std::string typedDir = io::isAbsolutePath(typed)
                                         ? std::filesystem::path(typed).parent_path().string()
                                         : std::string();
        !typedDir.empty())
        req.startFolder = typedDir; // whatever the user last picked wins over the policy default
    req.filters = currentFilters();
    return platform::showSaveDialog(req);
}

void ExportDialog::doBrowse() {
    if (const std::optional<std::string> chosen = askForPath())
        m_pathField->value(chosen->c_str());
}

void ExportDialog::doExport() {
    const io::FormatBackend* b = backend();
    if (b == nullptr)
        return;
    std::string typed = m_pathField->value() != nullptr ? m_pathField->value() : "";
    // Trim whitespace.
    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    typed.erase(typed.begin(), std::find_if(typed.begin(), typed.end(), notSpace));
    typed.erase(std::find_if(typed.rbegin(), typed.rend(), notSpace).base(), typed.end());

    // §6.8: empty (a first export) => Export opens the system Save picker. A hand-typed relative
    // name resolves against the SEED FOLDER, never the process working directory -- that is the
    // rule that used to be missing, and it is why exports landed wherever the binary was started.
    std::string path = io::resolveExportPath(typed, seed().folder);
    if (path.empty()) {
        const std::optional<std::string> chosen = askForPath();
        if (!chosen)
            return; // cancelled -- stay in the modal
        path = *chosen;
    }
    path = io::withExtension(path, primaryExt()); // guarantee the format's extension

    // Reuse the pipeline's own cached resize when it is the one this export wants; otherwise do
    // the resize here (a synchronous export is what the button promises).
    std::shared_ptr<const common::Image> pixels;
    if (m_resized != nullptr && m_resizedW == m_outW && m_resizedH == m_outH &&
        m_resizedFilter == m_filter)
        pixels = m_resized;
    else
        pixels = std::make_shared<const common::Image>(
            resizeForExport(*m_base, m_outW, m_outH, m_filter));
    if (pixels == nullptr || pixels->empty()) {
        m_infoSize->setText(_("Export failed: could not resize the image"));
        return;
    }

    io::RenderInput input;
    input.pixels = pixels.get();
    input.matte = m_matte;
    input.dpi = m_dpi;
    // The same payload the trial encode used, so the file that lands is the file the info block
    // and the loss banner have been describing.
    input.stripMetadata = !m_keepMetadata;
    input.exif = m_exif.has_value() ? &*m_exif : nullptr;
    input.iccProfile = selectedIcc();
    io::OptionValues values = valuesForCurrentFormat();
    b->optionsSchema().coerce(values);
    std::string err;
    if (!io::encodeToFile(*b, input, values, path, &err)) {
        m_infoSize->setText(std::string(_("Export failed: ")) +
                            (err.empty() ? _("unknown error") : err));
        return; // keep the modal open so the user can retry / pick another path
    }
    m_accepted = true;
    m_resultPath = path;
    m_resultFormat = io::formatIdName(b->id());
    m_resultValues = std::move(values); // so "Export to <file>" repeats this encode exactly
    hide();
}

int ExportDialog::handle(int event) {
    // A press outside an open themed list / context menu / colour flyout closes it (the modeless
    // pop-ups are children of this modal; mirrors FillDialog::handle).
    if (event == FL_PUSH) {
        dismissActiveDropdownPopupOnOutsideClick(Fl::event_x(), Fl::event_y());
        dismissActiveContextMenuOnOutsideClick(Fl::event_x(), Fl::event_y());
        if (m_matteFlyout != nullptr && m_matteFlyout->shown() &&
            !m_matteFlyout->spansHostPoint(Fl::event_x(), Fl::event_y()))
            m_matteFlyout->hide();
    }
    if (event == FL_KEYBOARD) {
        const int key = Fl::event_key();
        if (key == FL_Escape) {
            if (activeContextMenu() != nullptr) {
                dismissActiveContextMenu();
                return 1;
            }
            if (activeDropdownPopup() != nullptr) {
                dismissActiveDropdownPopup();
                return 1;
            }
            if (m_matteFlyout != nullptr && m_matteFlyout->shown()) {
                m_matteFlyout->hide();
                return 1;
            }
            doCancel();
            return 1;
        }
        if (key == FL_Enter || key == FL_KP_Enter) {
            // Enter exports, unless focus is in a field being edited (there it commits the value).
            // Any text field counts, not just the fixed four: the options panel generates its own
            // from the schema (a PNG text chunk, an HDR header variable).
            Fl_Widget* f = Fl::focus();
            const bool inField = dynamic_cast<Fl_Input_*>(f) != nullptr;
            if (!inField) {
                doExport();
                return 1;
            }
        }
    }
    return Fl_Double_Window::handle(event);
}

} // namespace

std::optional<ExportResult> showExportDialog(const ExportRequest& request, Fl_Window* host) {
    if (request.composited == nullptr || request.composited->empty())
        return std::nullopt;
    ExportDialog dlg(request);
    dlg.show();
    centerWindowOver(dlg, host);
    while (dlg.shown())
        Fl::wait(); // pumps the pipeline's debounce + poll timeouts too
    return dlg.result();
}

} // namespace mosaic::ui
