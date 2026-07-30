#include "ui/paint_chip.hpp"

#include "common/i18n.hpp"
#include "ui/gradient_flyout.hpp" // defaultGradient / defaultGradientTransform
#include "ui/pattern_flyout.hpp"  // defaultProceduralPattern
#include "ui/theme.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <variant>

namespace mosaic::ui {

namespace {
Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}
} // namespace

const char* const kPaintKindNames[5] = {"Solid", "Linear", "Radial", "Conic", "Pattern"};
const char* const kGradTypeNames[3] = {"Linear", "Radial", "Conic"};

int paintKindIndex(const core::vec::Paint& p) {
    if (std::holds_alternative<core::vec::Pattern>(p))
        return kPatternKindIndex;
    if (const auto* g = std::get_if<core::vec::Gradient>(&p))
        return 1 + static_cast<int>(g->type); // Linear=1, Radial=2, Conic=3
    return 0;                                 // Solid / NoPaint -> Solid
}

common::Color8 paintSeedColor(const core::vec::Paint& p, common::Color8 fallback) {
    if (const auto* s = std::get_if<core::vec::SolidPaint>(&p))
        return common::toColor8(s->color);
    if (const auto* g = std::get_if<core::vec::Gradient>(&p))
        if (!g->stops.empty())
            return common::toColor8(g->stops.front().color);
    if (const auto* pat = std::get_if<core::vec::Pattern>(&p))
        if (const auto* pp = std::get_if<core::vec::ProceduralPattern>(pat))
            return common::toColor8(pp->fg);
    return fallback;
}

void setPaintKind(core::vec::Paint& p, int kind, common::Color8 seed) {
    if (kind == kPatternKindIndex) {
        if (!std::holds_alternative<core::vec::Pattern>(p))
            p = core::vec::Pattern{defaultProceduralPattern(seed)};
        return;
    }
    if (kind <= 0) {
        p = core::vec::SolidPaint{common::toColorF(seed)};
        return;
    }
    const auto t = static_cast<core::vec::GradientType>(kind - 1);
    if (auto* g = std::get_if<core::vec::Gradient>(&p)) {
        g->type = t;
        g->transform = defaultGradientTransform(t);
        return;
    }
    common::Color8 fade = seed;
    fade.a = 0; // seed colour -> transparent: a clear "this is a gradient now" default
    p = defaultGradient(t, seed, fade);
}

PaintChip::PaintChip(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {
    m_ground = activePalette().windowBg;
}

void PaintChip::setPaint(const core::vec::Paint& p) {
    m_paint = p;
    redraw();
}

int PaintChip::handle(int e) {
    switch (e) {
    case FL_ENTER:
    case FL_MOVE:
        if (active_r() && !m_hover) {
            m_hover = true;
            if (window() != nullptr)
                window()->cursor(FL_CURSOR_HAND);
            redraw();
        }
        return 1;
    case FL_LEAVE:
        if (m_hover) {
            m_hover = false;
            if (window() != nullptr)
                window()->cursor(FL_CURSOR_DEFAULT);
            redraw();
        }
        return 1;
    case FL_PUSH:
        return active_r() ? 1 : 0;
    case FL_RELEASE:
        if (active_r() && Fl::event_inside(this) && m_onClick)
            m_onClick();
        return 1;
    default:
        return Fl_Widget::handle(e);
    }
}

void PaintChip::draw() {
    const Palette& p = activePalette();
    fl_color(toFl(m_ground));
    fl_rectf(x(), y(), w(), h());
    const int sw = std::min(28, w());
    if (const auto* g = std::get_if<core::vec::Gradient>(&m_paint)) {
        // A ramp of the stops over a checkerboard in the swatch cell.
        for (int xx = 0; xx < sw; ++xx) {
            const double t = sw > 1 ? double(xx) / (sw - 1) : 0.0;
            common::ColorF c{0, 0, 0, 0};
            if (!g->stops.empty()) {
                c = g->stops.front().color;
                for (std::size_t i = 1; i < g->stops.size(); ++i)
                    if (t <= g->stops[i].offset) {
                        const double span = g->stops[i].offset - g->stops[i - 1].offset;
                        const double f = span > 1e-9 ? (t - g->stops[i - 1].offset) / span : 0.0;
                        const auto& a = g->stops[i - 1].color;
                        const auto& b = g->stops[i].color;
                        const float ft = static_cast<float>(f);
                        c = {a.r + (b.r - a.r) * ft, a.g + (b.g - a.g) * ft, a.b + (b.b - a.b) * ft,
                             a.a + (b.a - a.a) * ft};
                        break;
                    } else if (t >= g->stops.back().offset) {
                        c = g->stops.back().color;
                    }
            }
            for (int yy = 0; yy < h(); ++yy) {
                const bool dark = (((xx) / 6) + (yy / 6)) & 1;
                const float bg = dark ? 205.0f : 255.0f;
                const float a = std::clamp(c.a, 0.0f, 1.0f);
                auto ch = [&](float v) {
                    return static_cast<std::uint8_t>(
                        std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f * a + bg * (1.0f - a)));
                };
                fl_color(fl_rgb_color(ch(c.r), ch(c.g), ch(c.b)));
                fl_point(x() + xx, y() + yy);
            }
        }
        fl_color(toFl(m_hover ? p.accent : p.border));
        fl_rect(x(), y(), sw, h());
        fl_color(toFl(p.textMuted));
        fl_font(FL_HELVETICA, 12);
        fl_draw(kGradTypeNames[std::clamp(static_cast<int>(g->type), 0, 2)], x() + sw + 10, y(),
                w() - sw - 10, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    } else if (const auto* pat = std::get_if<core::vec::Pattern>(&m_paint)) {
        // A live mini-preview of the pattern in the swatch cell, over a checker, + its kind name.
        const char* kindName = "Pattern";
        if (const auto* pp = std::get_if<core::vec::ProceduralPattern>(pat)) {
            kindName = core::vec::patternKindName(pp->kind);
            core::vec::ProceduralPattern preview = *pp;
            preview.scale = 7.0f; // a few tiles fit the small cell
            const core::vec::Pattern pv = preview;
            for (int xx = 0; xx < sw; ++xx)
                for (int yy = 0; yy < h(); ++yy) {
                    const common::ColorF c = core::vec::samplePattern(pv, {xx + 0.5, yy + 0.5});
                    const bool dark = ((xx / 6) + (yy / 6)) & 1;
                    const float bg = dark ? 205.0f : 255.0f;
                    const float a = std::clamp(c.a, 0.0f, 1.0f);
                    const auto ch = [&](float v) {
                        return static_cast<std::uint8_t>(
                            std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f * a + bg * (1.0f - a)));
                    };
                    fl_color(fl_rgb_color(ch(c.r), ch(c.g), ch(c.b)));
                    fl_point(x() + xx, y() + yy);
                }
        }
        fl_color(toFl(m_hover ? p.accent : p.border));
        fl_rect(x(), y(), sw, h());
        fl_color(toFl(p.textMuted));
        fl_font(FL_HELVETICA, 12);
        fl_draw(kindName, x() + sw + 10, y(), w() - sw - 10, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    } else {
        const bool none = std::holds_alternative<core::vec::NoPaint>(m_paint);
        const common::ColorF cf =
            none ? common::ColorF{0, 0, 0, 0} : std::get<core::vec::SolidPaint>(m_paint).color;
        const common::Color8 c = common::toColor8(cf);
        // Composite the swatch over a checker so a (semi-)transparent colour reads (as the picker).
        for (int xx = 0; xx < sw; ++xx)
            for (int yy = 0; yy < h(); ++yy) {
                const bool dark = ((xx / 6) + (yy / 6)) & 1;
                const float bg = dark ? 205.0f : 255.0f;
                const float a = std::clamp(cf.a, 0.0f, 1.0f);
                const auto ch = [&](float v) {
                    return static_cast<std::uint8_t>(
                        std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f * a + bg * (1.0f - a)));
                };
                fl_color(fl_rgb_color(ch(cf.r), ch(cf.g), ch(cf.b)));
                fl_point(x() + xx, y() + yy);
            }
        fl_color(toFl(m_hover ? p.accent : p.border));
        fl_rect(x(), y(), sw, h());
        char buf[16];
        if (none)
            std::snprintf(buf, sizeof(buf), "%s", "None");
        else
            std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
        fl_color(toFl(p.textMuted));
        fl_font(FL_HELVETICA, 12);
        fl_draw(buf, x() + sw + 10, y(), w() - sw - 10, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    }
    // The chevron always signals "clickable"; the "Edit..." word only when the chip is wide enough
    // that it won't collide with the swatch/hex or the gradient type label (a narrow chip sharing
    // its row with a kind dropdown omits the word).
    if (w() >= 150) {
        fl_color(toFl(m_hover ? p.text : p.textMuted));
        fl_font(FL_HELVETICA, 12);
        fl_draw(_("Edit\xE2\x80\xA6"), x(), y(), w() - 22, h(), FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
    }
    fl_color(toFl(m_hover ? p.text : p.textMuted));
    const int cx = x() + w() - 12, cy = y() + h() / 2;
    fl_begin_polygon();
    fl_vertex(cx - 4, cy - 2);
    fl_vertex(cx + 4, cy - 2);
    fl_vertex(cx, cy + 3);
    fl_end_polygon();
}

} // namespace mosaic::ui
