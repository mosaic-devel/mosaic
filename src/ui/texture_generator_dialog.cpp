#include "ui/texture_generator_dialog.hpp"

#include "common/i18n.hpp"
#include "core/texture/city_catalog.hpp"
#include "core/texture/render_worker.hpp"
#include "core/texture/sky_almanac.hpp"
#include "core/texture/sky_estimate_worker.hpp"
#include "core/texture/solar.hpp"
#include "ui/bubble_flyout.hpp"
#include "ui/color_flyout.hpp"
#include "ui/cursor_apply.hpp"
#include "ui/date_picker.hpp"
#include "ui/gizmo_canvas.hpp"
#include "ui/map_picker.hpp"
#include "ui/scrub_slider.hpp"
#include "ui/texture_gizmo_math.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Input_.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <numbers>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

// The Texture Generator modal (S55-f; docs/texture-generator.md §7). Layout per the §7.2 sketch:
// a generator RAIL (the suite grows -- wood/marble/… are S55-g), a Preset/Seed/Randomize top
// strip, a scrollable per-generator control stack with progressive disclosure, and the LIVE
// preview pane -- a GizmoCanvas over a background-rendered proxy carrying the §7.3 gizmos
// (sun-on-dome + in-frame sun, horizon/roll/FOV camera handles, wind vector, grain ring,
// raking-light dome). Every edit mutates the working TextureParams, re-syncs the controls and
// queues a coalesced proxy render on the TextureRenderWorker; Create bakes full resolution on the
// same worker behind a cancellable progress bar, then hands params + pixels to the host.
namespace mosaic::ui {

namespace {

namespace texture = core::texture;
using common::Vec2;
using texture::CanvasParams;
using texture::GrassParams;
using texture::MarbleParams;
using texture::MetalParams;
using texture::PaperParams;
using texture::SkyParams;
using texture::StoneParams;
using texture::TextureParams;
using texture::WoodParams;

constexpr int kWinW = 960;
constexpr int kWinH = 620;
constexpr int kFooterH = 56;
constexpr int kRailW = 150;
constexpr int kRailRowH = 34;
constexpr int kRowH = 26;
constexpr int kRowGap = 6;
constexpr int kTopY = 12;
constexpr int kStackY = 52;
constexpr int kCtlW = 340; // controls ScrollView width (scrollbar included)
constexpr int kPrevX = kRailW + kCtlW + 14;
constexpr int kPrevW = kWinW - kPrevX - 16;
constexpr int kPrevH = 372;
constexpr int kMaxDecks = 4; // §8.1 sanity: each enabled deck costs a density field per pixel

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

common::Color8 to8(common::ColorF c) {
    const auto q = [](float v) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    return {q(c.r), q(c.g), q(c.b), 255};
}

// The accent-filled primary button is the shared ui::FilledButton (widgets.hpp).

// A rail row (the settings dialog's NavItem look): accent-filled when active, a soft highlight on
// hover, otherwise the panel ground.
class RailItem : public Fl_Widget {
public:
    RailItem(int X, int Y, int W, int H, const char* label, std::function<void()> onClick)
        : Fl_Widget(X, Y, W, H), m_onClick(std::move(onClick)) {
        copy_label(label);
    }
    void setActive(bool a) {
        if (a != m_active) {
            m_active = a;
            redraw();
        }
    }

protected:
    void draw() override {
        const Palette& pal = activePalette();
        const bool on = active_r();
        fl_color(toFl(m_active ? pal.accent : m_hover && on ? pal.controlHover : pal.panelBg));
        fl_rectf(x(), y(), w(), h());
        fl_color(toFl(m_active ? pal.onAccent : on ? pal.text : pal.textMuted));
        fl_font(FL_HELVETICA, 13);
        fl_draw(label(), x() + 16, y(), w() - 20, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    }
    int handle(int event) override {
        if (!active_r()) return Fl_Widget::handle(event);
        switch (event) {
        case FL_ENTER:
            m_hover = true;
            redraw();
            return 1;
        case FL_LEAVE:
            m_hover = false;
            redraw();
            return 1;
        case FL_PUSH:
            return 1;
        case FL_RELEASE:
            if (Fl::event_inside(this) && m_onClick) m_onClick();
            return 1;
        default:
            return Fl_Widget::handle(event);
        }
    }

private:
    std::function<void()> m_onClick;
    bool m_active = false;
    bool m_hover = false;
};

// A collapsible section header (the Layer Effects InstanceHeader look): disclosure triangle +
// bold caption + hairline; the FlatButton base fires the callback.
class SectionHeader : public FlatButton {
public:
    SectionHeader(int X, int Y, int W, int H, const char* text) : FlatButton(X, Y, W, H, text) {}
    void setOpen(bool o) {
        m_open = o;
        redraw();
    }

protected:
    void draw() override {
        const Palette& pal = activePalette();
        fl_color(toFl(pal.windowBg));
        fl_rectf(x(), y(), w(), h());
        const int cx = x() + 6, cyc = y() + h() / 2;
        fl_color(toFl(pal.text));
        fl_begin_polygon();
        if (m_open) {
            fl_vertex(cx - 4, cyc - 2);
            fl_vertex(cx + 4, cyc - 2);
            fl_vertex(cx, cyc + 3);
        } else {
            fl_vertex(cx - 2, cyc - 4);
            fl_vertex(cx - 2, cyc + 4);
            fl_vertex(cx + 3, cyc);
        }
        fl_end_polygon();
        fl_font(FL_HELVETICA_BOLD, 12);
        fl_draw(label(), x() + 18, y(), w() - 18, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        fl_color(toFl(pal.border));
        fl_line(x(), y() + h() - 1, x() + w() - 1, y() + h() - 1);
    }

private:
    bool m_open = true;
};

// The inline "roll a new seed" affordance -- a small die drawn beside the seed field so the two
// read as ONE control (user 2026-07-15: a separate word-button "Randomize" felt clunky). Hand-drawn
// pips (the no-Unicode-glyphs rule): a rounded face with five dots.
class DiceButton : public FlatButton {
public:
    DiceButton(int X, int Y, int W, int H) : FlatButton(X, Y, W, H, nullptr) {}

protected:
    void draw() override {
        const Palette& pal = activePalette();
        const common::Color8 ground = m_hover && active_r() ? pal.controlHover : pal.controlBg;
        fl_color(toFl(ground));
        fl_rectf(x(), y(), w(), h());
        fl_color(toFl(pal.border));
        fl_rect(x(), y(), w(), h());
        // The die face, centred, with a 1px inner margin.
        const int s = std::min(w(), h()) - 10;
        const int fx = x() + (w() - s) / 2, fy = y() + (h() - s) / 2;
        const common::Color8 ink = active_r() ? pal.text : pal.textMuted;
        fl_color(toFl(ink));
        fl_rect(fx, fy, s, s);
        fl_rect(fx + 1, fy + 1, s - 2, s - 2);
        const double pr = std::max(1.0, s / 12.0);
        // All five pips in ONE anti-aliased patch (fl_pie stair-steps a 3px dot badly enough to see).
        // `under` restates the 2px frame as well as the button ground: at this button's size the pip
        // bboxes stop clear of it, but a smaller die's corner pips would reach it, and an opaque
        // patch erases whatever the sampler does not put back.
        const auto under = [&](int ux, int uy) {
            const bool inFace = ux >= fx && ux < fx + s && uy >= fy && uy < fy + s;
            const bool onFrame = ux <= fx + 1 || ux >= fx + s - 2 || uy <= fy + 1 || uy >= fy + s - 2;
            return (inFace && onFrame) ? ink : ground;
        };
        std::vector<AAArc> pips;
        const auto pip = [&](double u, double v) {
            const int px = fx + static_cast<int>(std::lround(u * s));
            const int py = fy + static_cast<int>(std::lround(v * s));
            pips.push_back(aaPieFromBox(px - static_cast<int>(pr), py - static_cast<int>(pr),
                                        static_cast<int>(pr * 2), static_cast<int>(pr * 2), 0, 360,
                                        ink));
        };
        pip(0.28, 0.28);
        pip(0.72, 0.28);
        pip(0.50, 0.50);
        pip(0.28, 0.72);
        pip(0.72, 0.72);
        drawAAArcs(under, pips);
    }
    int handle(int event) override {
        if (event == FL_ENTER) {
            m_hover = true;
            redraw();
            return 1;
        }
        if (event == FL_LEAVE) {
            m_hover = false;
            redraw();
            return 1;
        }
        return FlatButton::handle(event);
    }

private:
    bool m_hover = false;
};

// The under-preview info panel (user 2026-07-15: fill the dead space + a "night there" readout).
// A titled grid of label/value rows fed from computeSkyAlmanac(date, place): sky state, sun & moon
// visibility, the moon's phase name + illuminated %, and the nearest catalogued city -- plus, in a
// fixed slot on the right, a small drawn ANALOG CLOCK reading the observer's local (mean solar)
// date & time, its face tinted by the almanac's day/twilight/night state. Draws itself from the
// live palette so a re-theme follows for free.
class SkyInfoPanel : public Fl_Widget {
public:
    SkyInfoPanel(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}

    void setRows(std::vector<std::pair<std::string, std::string>> rows) {
        m_rows = std::move(rows);
        redraw();
    }
    void setTitle(std::string t) {
        m_title = std::move(t);
        redraw();
    }
    void setClock(const SkyClockState& c) {
        m_clock = c;
        // Format once per update, not per draw: the calendar line in the locale field order, the
        // wall time as HH:MM ("solar" says which clock this is -- UTC + longitude/15, so solar
        // noon reads 12; the Time control above is the UTC one).
        m_clockDate = date_detail::formatDate({c.year, c.month, c.day},
                                              date_detail::localeFieldOrder());
        const int hh = std::clamp(static_cast<int>(c.localHours), 0, 23);
        const int mm = std::clamp(static_cast<int>((c.localHours - hh) * 60.0), 0, 59);
        char t[24];
        std::snprintf(t, sizeof(t), _("%02d:%02d solar"), hh, mm);
        m_clockTime = t;
        redraw();
    }

protected:
    void draw() override {
        const Palette& pal = activePalette();
        fl_color(toFl(pal.panelBg));
        fl_rectf(x(), y(), w(), h());
        fl_color(toFl(pal.border));
        fl_rect(x(), y(), w(), h());
        const int textW = w() - 20 - (m_clock.visible ? kClockSlotW : 0);
        int ty = y() + 7;
        if (!m_title.empty()) {
            fl_font(FL_HELVETICA_BOLD, 12);
            fl_color(toFl(pal.text));
            fl_draw(m_title.c_str(), x() + 10, ty, textW, 15, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            ty += 18;
        }
        const int labelW = 104;
        fl_push_clip(x() + 10, y(), textW, h()); // fl_draw doesn't clip; keep off the clock slot
        for (const auto& [label, value] : m_rows) {
            fl_font(FL_HELVETICA, 11);
            fl_color(toFl(pal.textMuted));
            fl_draw(label.c_str(), x() + 10, ty, labelW, 15, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            fl_color(toFl(pal.text));
            fl_draw(value.c_str(), x() + 10 + labelW, ty, textW - labelW, 15,
                    FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            ty += 17;
        }
        fl_pop_clip();
        if (m_clock.visible)
            drawClock();
    }

private:
    static constexpr int kClockSlotW = 92; // the fixed right-hand slot (face + two text lines)
    static constexpr int kClockR = 28;     // face radius

    void drawClock() const {
        const Palette& pal = activePalette();
        const int cx = x() + w() - 6 - kClockSlotW / 2;
        const int cy = y() + 7 + kClockR;

        // Face: the control ground nudged toward the sky's state (the same thresholds the
        // almanac's Sun row uses) -- warm by day, violet through the twilights, blue at night.
        const auto mix = [](common::Color8 a, common::Color8 b, float t) {
            const auto ch = [t](std::uint8_t av, std::uint8_t bv) {
                return static_cast<std::uint8_t>(std::lround(av + (bv - av) * t));
            };
            return common::Color8{ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), 255};
        };
        common::Color8 face = pal.controlBg;
        if (m_clock.sunElevationDeg >= -0.833)
            face = mix(face, {247, 214, 138, 255}, 0.20f); // daylight
        else if (m_clock.sunElevationDeg >= -18.0)
            face = mix(face, {156, 116, 196, 255}, 0.20f); // twilight band
        else
            face = mix(face, {36, 52, 110, 255}, 0.28f); // night
        // Fill and rim compose in ONE patch, in this order, over the panel ground the widget filled
        // before the rows: a 56 px circle is where fl_pie/fl_arc's stair-stepping is most obvious.
        drawAAArcs(pal.panelBg,
                   {aaPieFromBox(cx - kClockR, cy - kClockR, kClockR * 2, kClockR * 2, 0, 360, face),
                    aaArcFromBox(cx - kClockR, cy - kClockR, kClockR * 2, kClockR * 2, 0, 360, 1.0,
                                 pal.border)});

        // Tick marks: subtle minor hours, slightly longer quarters.
        const auto onDial = [&](double angleRad, double radius) {
            return std::pair<int, int>{
                cx + static_cast<int>(std::lround(std::sin(angleRad) * radius)),
                cy - static_cast<int>(std::lround(std::cos(angleRad) * radius))};
        };
        fl_color(toFl(pal.textMuted));
        for (int i = 0; i < 12; ++i) {
            const double a = i * (std::numbers::pi / 6.0);
            const auto [x0, y0] = onDial(a, kClockR - 3.0);
            const auto [x1, y1] = onDial(a, i % 3 == 0 ? kClockR - 8.0 : kClockR - 5.0);
            fl_line(x0, y0, x1, y1);
        }

        // Hands from the local wall time (clockwise from 12).
        const double hours = std::clamp(m_clock.localHours, 0.0, 24.0);
        const double minutes = (hours - static_cast<int>(hours)) * 60.0;
        const double hourA = std::fmod(hours, 12.0) / 12.0 * 2.0 * std::numbers::pi;
        const double minA = minutes / 60.0 * 2.0 * std::numbers::pi;
        fl_color(toFl(pal.text));
        fl_line_style(FL_SOLID | FL_CAP_ROUND, 3);
        {
            const auto [hx, hy] = onDial(hourA, kClockR * 0.52);
            fl_line(cx, cy, hx, hy);
        }
        fl_line_style(FL_SOLID | FL_CAP_ROUND, 2);
        {
            const auto [mx, my] = onDial(minA, kClockR * 0.78);
            fl_line(cx, cy, mx, my);
        }
        fl_line_style(0);
        // The hub caps the hands, so its ground is the face fill. The hands are NOT restated -- they
        // are round-capped fl_line capsules, not arcs -- and the 5 px disc covers its whole patch bar
        // the four corner pixels, so at worst a hand pointing near a diagonal gives up one pixel
        // there, under the hub rather than beside it.
        drawAAArcs(face, {aaPieFromBox(cx - 2, cy - 2, 5, 5, 0, 360, pal.accent)});

        // The date under the face, the wall time under that.
        const int slotX = x() + w() - 6 - kClockSlotW;
        fl_font(FL_HELVETICA, 10);
        fl_color(toFl(pal.text));
        fl_draw(m_clockDate.c_str(), slotX, cy + kClockR + 3, kClockSlotW, 12, FL_ALIGN_CENTER);
        fl_color(toFl(pal.textMuted));
        fl_draw(m_clockTime.c_str(), slotX, cy + kClockR + 16, kClockSlotW, 12, FL_ALIGN_CENTER);
    }

    std::string m_title;
    std::vector<std::pair<std::string, std::string>> m_rows;
    SkyClockState m_clock;
    std::string m_clockDate; // formatted once in setClock
    std::string m_clockTime;
};

// A control's binding: the closure to run when it fires (FLTK callbacks are C thunks + a void*).
struct Binding {
    std::function<void()> fn;
};
void controlThunk(Fl_Widget*, void* b) {
    if (auto* bb = static_cast<Binding*>(b)) {
        // Run a COPY: several bindings (section headers, deck add/remove) call rebuildControls()
        // from inside their own body, which clears m_ui->bindings and would free the very closure
        // still executing. The copy owns its captures for the duration of the call.
        const std::function<void()> fn = bb->fn;
        fn();
    }
}

// Which gizmo overlay the preview pane draws for a generator. SKY carries the horizon/roll/FOV
// camera handles + the in-frame sun/moon + the dome & wind insets; LAWN the grass camera + light
// dome + wind; SURFACE is the flat lit sheet -- a raking-light dome plus, when the arm has a
// grain/vein/brush axis (surfaceFields below), the grain-direction ring. Paper and every S55-g
// material are SURFACE generators.
enum class GizmoLayout { Sky, Lawn, Surface };

// Per-generator dialog metadata, in enum order (the core registry's UI sibling; core traits carry
// name/token/render/presets, this table carries what only the dialog knows). `title` is N_-marked
// for extraction and translated at the use site; `presetAffectsLayout` says a preset apply can
// change the control-stack SHAPE (sky: the per-deck rows), so the stack rebuilds after it.
struct GeneratorUiMeta {
    const char* title;
    GizmoLayout gizmos;
    bool presetAffectsLayout;
};
constexpr GeneratorUiMeta kGeneratorUi[texture::kGeneratorCount] = {
    {N_("Sky"), GizmoLayout::Sky, true},
    {N_("Paper"), GizmoLayout::Surface, false},
    {N_("Grass"), GizmoLayout::Lawn, false},
    {N_("Wood"), GizmoLayout::Surface, false},
    {N_("Marble"), GizmoLayout::Surface, false},
    {N_("Stone"), GizmoLayout::Surface, false},
    {N_("Canvas"), GizmoLayout::Surface, false},
    {N_("Metal"), GizmoLayout::Surface, false},
};

const GeneratorUiMeta& generatorUi(texture::Generator g) {
    const auto i = static_cast<std::size_t>(g);
    return kGeneratorUi[i < static_cast<std::size_t>(texture::kGeneratorCount) ? i : 0];
}

const char* generatorTitle(texture::Generator g) {
    return _(generatorUi(g).title);
}

// The SURFACE gizmo's parameter hooks: every flat-sheet arm exposes its raking light here, and
// (when it has one) its grain/vein/weave/brush axis for the ring gizmo. One row per arm -- the
// preview pane stays generator-agnostic.
struct SurfaceFields {
    double* angleDeg = nullptr;  // nullptr = no direction ring (isotropic materials)
    double* lightAz = nullptr;
    double* lightEl = nullptr;
};
SurfaceFields surfaceFields(TextureParams& p) {
    if (auto* pp = std::get_if<PaperParams>(&p.spec))
        return {&pp->grainAngleDeg, &pp->lightAzimuthDeg, &pp->lightElevationDeg};
    if (auto* wo = std::get_if<WoodParams>(&p.spec))
        return {&wo->grainAngleDeg, &wo->lightAzimuthDeg, &wo->lightElevationDeg};
    if (auto* ma = std::get_if<MarbleParams>(&p.spec))
        return {&ma->veinAngleDeg, &ma->lightAzimuthDeg, &ma->lightElevationDeg};
    if (auto* st = std::get_if<StoneParams>(&p.spec))
        return {nullptr, &st->lightAzimuthDeg, &st->lightElevationDeg};  // isotropic: no ring
    if (auto* cv = std::get_if<CanvasParams>(&p.spec))
        return {&cv->weaveAngleDeg, &cv->lightAzimuthDeg, &cv->lightElevationDeg};
    if (auto* me = std::get_if<MetalParams>(&p.spec))
        return {&me->brushAngleDeg, &me->lightAzimuthDeg, &me->lightElevationDeg};
    return {};
}

std::uint64_t randomSeed() {
    static std::mt19937_64 rng{std::random_device{}()};
    return rng();
}

// Fl_Menu_::add() parses '/' as a submenu separator, '&' as a shortcut marker and '\' as its
// escape; preset names are plain prose ("Linen / canvas"), so escape them for the dropdown.
std::string menuEscape(const char* s) {
    std::string out;
    for (const char* p = s; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\' || *p == '&') out.push_back('\\');
        out.push_back(*p);
    }
    return out;
}

// A FlatButton that reports pointer enter/leave -- the estimate action row's hover opens the
// EstimateBubble (a deactivated button gets no crossing events, so no bubble; the tooltip
// carries the disabled hint instead).
class HoverButton : public FlatButton {
public:
    using FlatButton::FlatButton;
    std::function<void(bool)> onHover;

    int handle(int event) override {
        if ((event == FL_ENTER || event == FL_LEAVE) && onHover) onHover(event == FL_ENTER);
        return FlatButton::handle(event);
    }
};

// The estimate action row's hover bubble (design §7): a thumbnail of the source layer, its name,
// and the one-line promise. All DRAWN content (no child widgets), so moveContent is a no-op and
// the shift rides contentLeft(). Pre-show child construction, setAvoidRect(preview), Esc via the
// BubbleFlyout base; the dialog hides it on leave and on any press.
class EstimateBubble : public BubbleFlyout {
public:
    static constexpr int kThumbW = 160, kThumbH = 120;

    EstimateBubble()
        : BubbleFlyout(kContentX + kThumbW + kPad, kPad + kThumbH + 6 + 16 + 32 + kPad) {
        end();
    }

    // Seed from the (already downscaled, <= kThumbW x kThumbH) source thumbnail + name, place
    // beside the anchor, show. The RGBA thumb composites over the panel ground here so the
    // drawn image needs no alpha path.
    void openFor(const Fl_Widget* anchor, const common::Image& thumb, const std::string& name) {
        m_name = name;
        const Palette& pal = activePalette();
        m_tw = static_cast<int>(thumb.width);
        m_th = static_cast<int>(thumb.height);
        m_img.reset();
        m_rgb.assign(static_cast<std::size_t>(m_tw) * m_th * 3, 0);
        for (std::size_t i = 0; i < static_cast<std::size_t>(m_tw) * m_th; ++i) {
            const std::uint8_t* q = thumb.rgba.data() + i * 4;
            const int a = q[3];
            m_rgb[i * 3 + 0] = static_cast<unsigned char>((q[0] * a + pal.panelBg.r * (255 - a)) / 255);
            m_rgb[i * 3 + 1] = static_cast<unsigned char>((q[1] * a + pal.panelBg.g * (255 - a)) / 255);
            m_rgb[i * 3 + 2] = static_cast<unsigned char>((q[2] * a + pal.panelBg.b * (255 - a)) / 255);
        }
        if (m_tw > 0 && m_th > 0)
            m_img = std::make_unique<Fl_RGB_Image>(m_rgb.data(), m_tw, m_th, 3);
        placeBubble(anchor);
        show();
    }

    [[nodiscard]] bool shownForAnchor(const Fl_Widget* a) { return m_anchor == a && shown(); }

    void hide() override {
        Fl_Double_Window::hide();
        m_anchor = nullptr;
    }

protected:
    void drawContent() override {
        drawBubbleChrome();
        const Palette& pal = activePalette();
        const int cx = contentLeft();
        // Thumbnail, centred in its fixed box (border-framed like the layer dock's thumbnails).
        const int tx = cx + (kThumbW - m_tw) / 2;
        const int ty = kPad + (kThumbH - m_th) / 2;
        if (m_img != nullptr) {
            m_img->draw(tx, ty);
            fl_color(toFl(pal.border));
            fl_rect(tx - 1, ty - 1, m_tw + 2, m_th + 2);
        }
        int textY = kPad + kThumbH + 6;
        fl_font(FL_HELVETICA_BOLD, 12);
        fl_color(toFl(pal.text));
        fl_draw(m_name.c_str(), cx, textY, kThumbW, 14, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        textY += 16;
        fl_font(FL_HELVETICA, 11);
        fl_color(toFl(pal.textMuted));
        fl_draw(_("Analyzes this layer; sets horizon, camera, sun & sky to match."), cx, textY,
                kThumbW, 30, FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    }

    void moveContent(int) override {} // drawn content only: contentLeft() carries the shift

private:
    std::string m_name;
    std::vector<unsigned char> m_rgb; // RGB backing store (kept alive for Fl_RGB_Image)
    std::unique_ptr<Fl_RGB_Image> m_img;
    int m_tw = 0, m_th = 0;
};

// Box-downscale a doc-space source image into the bubble's thumbnail budget (<= 160 x 120,
// aspect kept). One call per dialog session (cached; the modal freezes the layer under it).
common::Image bubbleThumbnail(const common::Image& src) {
    if (src.empty()) return {};
    const double s = std::min({1.0, static_cast<double>(EstimateBubble::kThumbW) / src.width,
                               static_cast<double>(EstimateBubble::kThumbH) / src.height});
    const auto tw = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(src.width * s));
    const auto th = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(src.height * s));
    common::Image out(tw, th);
    for (std::uint32_t y = 0; y < th; ++y)
        for (std::uint32_t x = 0; x < tw; ++x) {
            const std::uint32_t x0 = x * src.width / tw, x1 = std::max(x0 + 1, (x + 1) * src.width / tw);
            const std::uint32_t y0 = y * src.height / th, y1 = std::max(y0 + 1, (y + 1) * src.height / th);
            std::uint32_t acc[4] = {0, 0, 0, 0}, n = 0;
            for (std::uint32_t yy = y0; yy < y1; ++yy)
                for (std::uint32_t xx = x0; xx < x1; ++xx) {
                    const std::uint8_t* q =
                        src.rgba.data() + (static_cast<std::size_t>(yy) * src.width + xx) * 4;
                    for (int c = 0; c < 4; ++c) acc[c] += q[c];
                    ++n;
                }
            std::uint8_t* o = out.rgba.data() + (static_cast<std::size_t>(y) * tw + x) * 4;
            for (int c = 0; c < 4; ++c) o[c] = static_cast<std::uint8_t>(acc[c] / std::max(1u, n));
        }
    return out;
}

// HH:MM for the estimate summary's time lines (fractional UTC hours, wrapped).
std::string hhmmUtc(double hourUtc) {
    double h = std::fmod(hourUtc, 24.0);
    if (h < 0.0) h += 24.0;
    int hi = static_cast<int>(h);
    int mi = static_cast<int>(std::lround((h - hi) * 60.0));
    if (mi == 60) {
        mi = 0;
        hi = (hi + 1) % 24;
    }
    char b[8];
    std::snprintf(b, sizeof(b), "%02d:%02d", hi, mi);
    return b;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// The preview pane: proxy blit + §7.3 gizmos on a GizmoCanvas
// ------------------------------------------------------------------------------------------------
class TexturePreviewPane : public Fl_Widget {
public:
    TexturePreviewPane(int X, int Y, int W, int H, TextureGeneratorDialog* owner)
        : Fl_Widget(X, Y, W, H), m_owner(owner) {
        // No catch-all tooltip (user 2026-07-15: the handle list read as a flood) -- each gizmo
        // announces itself as the pointer reaches it (see tooltipFor / the FL_MOVE hook).
    }

    void setProxy(common::Image img) {
        m_img = std::move(img);
        redraw();
    }
    void setPending(bool p) {
        if (p != m_pending) {
            m_pending = p;
            redraw();
        }
    }
    // Continuous view state: `zoom` is screen px per doc px (fit mode recomputes it from the pane).
    void resetView() {
        m_fit = true;
        m_panX = m_panY = 0.0;
        redraw();
    }
    [[nodiscard]] bool fitMode() const { return m_fit; }
    [[nodiscard]] double panX() const { return m_panX; } // document pixels (top-left visible)
    [[nodiscard]] double panY() const { return m_panY; }
    static constexpr double kZoomMax = 8.0;
    // The effective zoom for the current pane size: the fit zoom in fit mode, else the clamped
    // stored zoom (floored at fit so the whole doc always fits when zoomed all the way out).
    [[nodiscard]] double effectiveZoom(std::uint32_t paneW, std::uint32_t paneH) const {
        const double fit = texgizmo::previewFitZoom(m_owner->m_docW, m_owner->m_docH, paneW, paneH);
        return m_fit ? fit : std::clamp(m_zoom, fit, kZoomMax);
    }
    // The inner rect available to the proxy image (inside the 1px frame + a small margin).
    [[nodiscard]] int innerW() const { return w() - 2; }
    [[nodiscard]] int innerH() const { return h() - 2; }

protected:
    void draw() override;
    int handle(int event) override;

private:
    enum class Grab { None, Pan, Horizon, Roll, Sun, Moon, SunDome, Wind, FovLeft, FovRight,
                      Grain, LightDome };

    // Each gizmo announces itself; empty = no tooltip (bare preview ground).
    [[nodiscard]] static const char* tooltipFor(Grab g) {
        switch (g) {
        case Grab::Pan: return _("Drag to pan; scroll to zoom");
        case Grab::Horizon: return _("Horizon: drag up or down");
        case Grab::Roll: return _("Roll: drag to tilt the horizon");
        case Grab::Sun: return _("Sun: drag to place it in the sky");
        case Grab::Moon: return _("Moon: drag to place it in the sky");
        case Grab::SunDome: return _("Sun position: angle = direction, centre = overhead");
        case Grab::Wind: return _("Wind: direction and strength");
        case Grab::FovLeft:
        case Grab::FovRight: return _("View width: drag outward to zoom in");
        case Grab::Grain: return _("Grain direction");
        case Grab::LightDome: return _("Raking light: angle = direction, centre = overhead");
        case Grab::None: break;
        }
        return "";
    }

    // The FRAME the current view renders (proxy dims in Fit, the document in 1:1) and where its
    // origin lands in widget coordinates -- the single mapping the gizmos and the blit share.
    struct View {
        double frameW = 0.0, frameH = 0.0; // camera frame
        double originX = 0.0, originY = 0.0; // widget position of frame pixel (winX, winY)
        long winX = 0, winY = 0;             // window origin within the frame (1:1 pan)
        double viewW = 0.0, viewH = 0.0;     // visible frame pixels (window size)
    };
    [[nodiscard]] View view() const;
    [[nodiscard]] Vec2 frameToWidget(const View& v, double fx, double fy) const {
        return {v.originX + (fx - v.winX), v.originY + (fy - v.winY)};
    }
    [[nodiscard]] Vec2 widgetToFrame(const View& v, Vec2 p) const {
        return {p.x - v.originX + v.winX, p.y - v.originY + v.winY};
    }

    // Inset anchors (widget coords): the dome (sun / raking light) bottom-left, the wind compass
    // top-right, the grain ring bottom-right.
    static constexpr double kInsetR = 24.0;
    [[nodiscard]] Vec2 domeCenter() const { return {x() + 14.0 + kInsetR, y() + h() - 14.0 - kInsetR}; }
    [[nodiscard]] Vec2 windCenter() const { return {x() + w() - 14.0 - kInsetR, y() + 14.0 + kInsetR}; }
    [[nodiscard]] Vec2 grainCenter() const {
        return {x() + w() - 14.0 - kInsetR, y() + h() - 14.0 - kInsetR};
    }
    // "Up" on the sky insets is where the camera looks (azimuth 180); grass/paper use 0.
    [[nodiscard]] double insetRefAzimuth() const;

    void drawGizmos(GizmoCanvas& gc, const View& v);
    [[nodiscard]] Grab grabAt(Vec2 p, const View& v) const;
    void dragBy(Vec2 d, Vec2 p, const View& v);
    void zoomAt(Vec2 cursor, int wheelDy); // scroll-wheel zoom toward the cursor
    void clampPan(double zoom);
    [[nodiscard]] Vec2 eventPos() const {
        return {static_cast<double>(Fl::event_x()), static_cast<double>(Fl::event_y())};
    }

    TextureGeneratorDialog* m_owner;
    common::Image m_img;
    bool m_pending = false;
    bool m_fit = true;
    double m_zoom = 1.0;           // screen px per doc px (meaningful when !m_fit)
    double m_panX = 0.0, m_panY = 0.0; // document pixels (top-left of the visible window)
    Grab m_grab = Grab::None;
    Grab m_hover = Grab::None;
    Vec2 m_last{};
    Vec2 m_pressPan{}; // pan (doc px) at FL_PUSH (the gesture accumulator, the type3d m_drag lesson)
    MoveCursor m_moveCursor; // the pan affordance; Wayland substitution + its rasterized cache
};

// ------------------------------------------------------------------------------------------------
// Ui bag
// ------------------------------------------------------------------------------------------------
struct TextureGeneratorDialog::Ui {
    std::vector<std::unique_ptr<Binding>> bindings; // control callbacks (cleared per rebuild)
    std::vector<RailItem*> rail;
    Dropdown* preset = nullptr;
    NumberField* seed = nullptr;
    DiceButton* dice = nullptr;
    ScrollView* scroll = nullptr;
    Fl_Group* stack = nullptr;
    TexturePreviewPane* preview = nullptr;
    FlatButton* resetView = nullptr;
    Fl_Box* zoomLabel = nullptr;
    SkyInfoPanel* info = nullptr; // under-preview almanac readout (persistent)
    Fl_Box* note = nullptr;
    ProgressBar* progress = nullptr;
    FlatButton* cancel = nullptr;
    FilledButton* create = nullptr;
    ColorFlyout* colorFlyout = nullptr;
    MapFlyout* mapFlyout = nullptr;             // persistent world-map place flyout (pre-show child)
    std::vector<std::function<void()>> syncFns; // push params -> widgets (cleared per rebuild)
    Fl_Box* solarReadout = nullptr;             // "Sun: az … el …" (updated by observerChanged)
    DatePicker* datePicker = nullptr;           // persistent (owns a sub-window); re-parented per rebuild
    MapPicker* place = nullptr;                 // per-rebuild; nulled in rebuildControls
    Fl_Box* moonReadout = nullptr;              // per-rebuild moon phase-name readout
    HoverButton* estimate = nullptr;            // per-rebuild "Estimate from layer…" action row
    EstimateBubble* estimateBubble = nullptr;   // persistent hover bubble (pre-show child)
    CheckBox* conform = nullptr;                // footer "mask & harmonize" toggle (sky only)
};

// The ACCEPT-time mask & harmonize run (design §7's toggle path): S6 + S7 on their own thread so
// the 4K segmentation never pins the UI, cancellable through the engine's progress channel, its
// completion polled by the same pollOnce loop the bake uses. Inputs are COPIES -- the thread
// shares nothing with the dialog but this struct.
struct TextureGeneratorDialog::ConformRun {
    TextureParams params;                  // the accepted generator value (the bake's)
    texture::TextureRenderResult baked;    // the finished full-res bake, held for the commit
    common::Image photo;                   // the doc-space source the estimate analyzed
    texture::SkyEstimateResult estimate;   // its S2 carry (border, color model, gates)
    std::shared_ptr<texture::SkyEstimateProgress> progress =
        std::make_shared<texture::SkyEstimateProgress>();
    core::Selection selection;             // S6's product (empty = skipped)
    std::map<std::string, double> match;   // S7's product
    std::string note;                      // S6's honesty note on failure
    std::atomic<bool> done{false};
    std::thread thread;
};

int texturePresetIndex(const TextureParams& p) {
    return texture::generatorTraits(p.generator).matchPreset(p);
}

// ------------------------------------------------------------------------------------------------
// Construction
// ------------------------------------------------------------------------------------------------
TextureGeneratorDialog::TextureGeneratorDialog(TextureGenHost host)
    : Fl_Double_Window(kWinW, kWinH, _("Texture Generator")), m_host(std::move(host)),
      m_worker(std::make_unique<texture::TextureRenderWorker>()),
      m_estimator(std::make_unique<texture::SkyEstimateWorker>()),
      m_ui(std::make_unique<Ui>()) {
    for (int g = 0; g < texture::kGeneratorCount; ++g)
        m_working[static_cast<std::size_t>(g)] =
            texture::defaultTextureParams(static_cast<texture::Generator>(g));

    color(toFl(activePalette().windowBg));
    begin(); // keep `this` current through build() + the sub-window creation below
    build();

    // Modal dialog: its own flyout / popup / menu / ruler child sub-windows, created before
    // show() (the Fill/Settings/LayerEffects convention -- a sub-window added to a shown parent
    // is promoted to a stray top-level). Flyout FIRST so the DropdownPopup stacks above it.
    auto* flyout = new ColorFlyout();
    flyout->hide();
    flyout->setUseForeground(
        [this] { return m_host.foreground ? m_host.foreground() : common::Color8{0, 0, 0, 255}; });
    flyout->setOnPick([this](common::Color8 c) {
        if (m_onColorPick) m_onColorPick(c);
    });
    m_ui->colorFlyout = flyout;
    // The world-map place flyout (the sky solar section's MapPicker opens it): same pre-show
    // child-sub-window rules as the colour flyout; its pick is the observer's lat/lon.
    auto* mapFly = new MapFlyout();
    mapFly->hide();
    mapFly->setOnPick([this](double lat, double lon) { setObserverLatLon(lat, lon); });
    m_ui->mapFlyout = mapFly;
    // The estimate action row's hover bubble: same pre-show child-sub-window rules.
    auto* estBubble = new EstimateBubble();
    estBubble->hide();
    m_ui->estimateBubble = estBubble;
    (new DropdownPopup())->hide();
    (new ContextMenu())->hide();
    // The ScrubSlider precision-ruler HUD must share the sliders' top_window (the recurring
    // "ruler doesn't appear in a new window" bug) -- so this modal owns its own.
    m_ruler = new ScrubRuler();
    m_ruler->hide();

    end();
    size_range(kWinW, kWinH, kWinW, kWinH);
    set_modal();
    callback([](Fl_Widget*, void* v) { static_cast<TextureGeneratorDialog*>(v)->doCancel(); },
             this);
}

TextureGeneratorDialog::~TextureGeneratorDialog() {
    Fl::remove_timeout(pollTimer, this);
    Fl::remove_timeout(calendarGuard, this);
    Fl::remove_timeout(estimateHoverTimeout, this); // never leave one pointing at a freed dialog
    if (m_worker) m_worker->cancelAll();
    if (m_estimator) m_estimator->cancelAll();
    if (m_conform != nullptr && m_conform->thread.joinable()) {
        m_conform->progress->cancel.store(true, std::memory_order_relaxed);
        m_conform->thread.join();
    }
}

void TextureGeneratorDialog::show() {
    Fl_Double_Window::show();
    // The consumed ui::DatePicker builds its calendar pop-up visible() by default, so FLTK auto-maps
    // it the moment this modal maps (verified empirically). Close it over the first few frames --
    // after the map settles, before the user could click -- so the calendar only appears on a real
    // click. (A pre-show hide does NOT stick; the map re-shows it.)
    m_calendarGuardTicks = 8;
    Fl::remove_timeout(calendarGuard, this);
    Fl::add_timeout(0.0, calendarGuard, this);
}

void TextureGeneratorDialog::calendarGuard(void* self) {
    auto* d = static_cast<TextureGeneratorDialog*>(self);
    if (d->m_ui->datePicker != nullptr) d->m_ui->datePicker->closeCalendar();
    if (--d->m_calendarGuardTicks > 0) Fl::add_timeout(1.0 / 60.0, calendarGuard, self);
}

TextureParams& TextureGeneratorDialog::activeParams() {
    return m_working[static_cast<std::size_t>(m_generator)];
}
const TextureParams& TextureGeneratorDialog::activeParams() const {
    return m_working[static_cast<std::size_t>(m_generator)];
}
const TextureParams& TextureGeneratorDialog::params() const {
    return activeParams();
}

// ------------------------------------------------------------------------------------------------
// Chrome
// ------------------------------------------------------------------------------------------------
void TextureGeneratorDialog::build() {
    const Palette& pal = activePalette();

    // ---- Generator rail ----
    auto* rail = new Panel(0, 0, kRailW, kWinH - kFooterH);
    rail->borderEdges(Panel::EdgeRight);
    rail->begin();
    auto* railTitle = new Fl_Box(16, 12, kRailW - 24, 20, _("Generator"));
    railTitle->box(FL_NO_BOX);
    railTitle->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    railTitle->labelfont(FL_HELVETICA_BOLD);
    railTitle->labelsize(13);
    railTitle->labelcolor(toFl(pal.text));
    int ry = 44;
    for (int g = 0; g < texture::kGeneratorCount; ++g) {
        const auto gen = static_cast<texture::Generator>(g);
        auto* item = new RailItem(0, ry, kRailW, kRailRowH, generatorTitle(gen),
                                  [this, gen] { selectGenerator(gen); });
        m_ui->rail.push_back(item);
        ry += kRailRowH;
    }
    rail->end();

    // ---- Top strip: preset / seed / randomize ----
    const int cx = kRailW + 16;
    auto* presetCap = new Fl_Box(cx, kTopY, 48, kRowH, _("Preset"));
    presetCap->box(FL_NO_BOX);
    presetCap->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    presetCap->labelfont(FL_HELVETICA);
    presetCap->labelsize(12);
    presetCap->labelcolor(toFl(pal.textMuted));
    m_ui->preset = new Dropdown(cx + 52, kTopY, 200, kRowH);
    m_ui->preset->callback(
        [](Fl_Widget*, void* v) {
            auto* d = static_cast<TextureGeneratorDialog*>(v);
            if (d->m_seeding) return;
            const int idx = d->m_ui->preset->value();
            if (idx > 0) d->applyPreset(static_cast<std::size_t>(idx - 1));
        },
        this);
    m_ui->preset->copy_tooltip(_("A complete look for this generator; the sliders fine-tune "
                                 "from there (the readout says Custom once they diverge)"));

    // Seed + an inline "roll" die read as one control (the crop-bar value field + a dice affordance
    // glued to its right), replacing the old separate "Randomize" word-button.
    auto* seedCap = new Fl_Box(cx + 300, kTopY, 36, kRowH, _("Seed"));
    seedCap->box(FL_NO_BOX);
    seedCap->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    seedCap->labelfont(FL_HELVETICA);
    seedCap->labelsize(12);
    seedCap->labelcolor(toFl(pal.textMuted));
    m_ui->seed = new NumberField(cx + 338, kTopY, 156, kRowH); // the app-wide value field
    m_ui->seed->when(FL_WHEN_ENTER_KEY | FL_WHEN_RELEASE);
    m_ui->seed->callback(
        [](Fl_Widget*, void* v) {
            auto* d = static_cast<TextureGeneratorDialog*>(v);
            if (d->m_seeding) return;
            const char* s = d->m_ui->seed->value();
            d->applyEdit([s](TextureParams& p) {
                p.seed = std::strtoull(s != nullptr ? s : "0", nullptr, 10);
            });
        },
        this);
    m_ui->seed->copy_tooltip(_("Every render is reproducible from seed + settings"));
    m_ui->dice = new DiceButton(cx + 338 + 156, kTopY, kRowH, kRowH);
    m_ui->dice->callback(
        [](Fl_Widget*, void* v) { static_cast<TextureGeneratorDialog*>(v)->randomizeSeed(); },
        this);
    m_ui->dice->copy_tooltip(_("Roll a fresh seed (same settings, new variation)"));

    // The observer date-picker owns a pop-up sub-window, which FLTK parents to whatever group is
    // current at CONSTRUCTION and promotes to a stray top-level if built after the dialog is shown
    // (the ui::Popover rule). So build it ONCE here, while `this` is current and unshown; the pop-up
    // stays a child of the dialog forever, while the picker WIDGET is re-parented into the scrolled
    // control stack whenever the sky "date & place" section is open (see rebuildControls).
    m_ui->datePicker = new DatePicker(0, 0, 168, kRowH);
    m_ui->datePicker->hide();
    m_ui->datePicker->setOnChange([this](const Date& d) {
        setObserver(d.year, d.month, d.day, m_solHour, m_solLat, m_solLon);
    });

    // ---- Controls scroll ----
    auto* sv = new ScrollView(kRailW, kStackY, kCtlW, (kWinH - kFooterH) - kStackY);
    sv->type(Fl_Scroll::VERTICAL_ALWAYS);
    sv->box(FL_FLAT_BOX);
    sv->color(toFl(pal.windowBg));
    sv->begin();
    auto* stack = new Fl_Group(kRailW, kStackY, kCtlW - 18, 200);
    stack->resizable(nullptr);
    stack->box(FL_NO_BOX);
    stack->end();
    sv->end();
    m_ui->scroll = sv;
    m_ui->stack = stack;

    // ---- Preview pane + view row ----
    // Scroll to zoom (toward the cursor), drag to pan, double-click to reset (user 2026-07-15: the
    // Fit / 1:1 buttons were replaced with a continuous view). A small "Reset view" affordance + a
    // live zoom readout sit under the pane.
    const int vy = kStackY + kPrevH + 8;
    m_ui->resetView = new FlatButton(kPrevX, vy, 96, 24, _("Reset view"));
    m_ui->resetView->callback(
        [](Fl_Widget*, void* v) {
            auto* d = static_cast<TextureGeneratorDialog*>(v);
            d->m_ui->preview->resetView();
            d->requestProxy();
        },
        this);
    m_ui->resetView->copy_tooltip(_("Fit the whole document (or double-click the preview)"));
    m_ui->zoomLabel = new Fl_Box(kPrevX + 104, vy, kPrevW - 104, 24);
    m_ui->zoomLabel->box(FL_NO_BOX);
    m_ui->zoomLabel->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
    m_ui->zoomLabel->labelfont(FL_HELVETICA);
    m_ui->zoomLabel->labelsize(11);
    m_ui->zoomLabel->labelcolor(toFl(pal.textMuted));
    m_ui->zoomLabel->copy_label(_("Scroll to zoom, drag to pan"));

    const int infoY = vy + 30;
    const int infoH = (kWinH - kFooterH) - infoY - 6;
    m_ui->info = new SkyInfoPanel(kPrevX, infoY, kPrevW, infoH);

    // The preview is created LAST in this region so it sits above the info panel in z-order and its
    // gizmo overlay is never clipped by a sibling.
    m_ui->preview = new TexturePreviewPane(kPrevX, kStackY, kPrevW, kPrevH, this);

    // ---- Footer ----
    auto* rule = new Fl_Box(0, kWinH - kFooterH, kWinW, 1);
    rule->box(FL_FLAT_BOX);
    rule->color(toFl(pal.border));
    const int by = kWinH - kFooterH + (kFooterH - 30) / 2;
    m_ui->note = new Fl_Box(16, by, 420, 30);
    m_ui->note->box(FL_NO_BOX);
    m_ui->note->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    m_ui->note->labelfont(FL_HELVETICA);
    m_ui->note->labelsize(12);
    m_ui->note->labelcolor(toFl(pal.textMuted));
    m_ui->progress = new ProgressBar(16 + 430, by + 7, 180, 16);
    m_ui->progress->setCellColor(pal.windowBg);
    m_ui->progress->hide();
    // The estimate's "mask & harmonize" toggle (design §7): sky only, offered only once an
    // estimate's segmentation cleared its floor (updateEstimateUi), and NEVER pre-armed -- a
    // fresh dialog starts unchecked (the user-driven-compositing discipline). It shares the progress
    // bar's footer slot; the two are never visible together (the fence hides the toggle).
    m_ui->conform = new CheckBox(16 + 430, by, kWinW - 16 - 96 - 12 - 88 - 12 - (16 + 430), 30,
                                 _("Also mask the photo's sky and match it to this sky."));
    m_ui->conform->setGroundColor(pal.windowBg);
    m_ui->conform->setOnToggle([this](bool on) { setConformWanted(on); });
    m_ui->conform->copy_tooltip(
        _("On Create: after the full-resolution render, the photo's sky is masked out and a "
          "Photometric Match layer grades the foreground toward the new sky -- one undo step"));
    m_ui->conform->hide();
    m_ui->create = new FilledButton(kWinW - 16 - 96, by, 96, 30, _("Create"));
    m_ui->create->callback(
        [](Fl_Widget*, void* v) { static_cast<TextureGeneratorDialog*>(v)->create(); }, this);
    m_ui->cancel = new FlatButton(kWinW - 16 - 96 - 12 - 88, by, 88, 30, _("Cancel"));
    m_ui->cancel->callback(
        [](Fl_Widget*, void* v) { static_cast<TextureGeneratorDialog*>(v)->doCancel(); }, this);
}

// ------------------------------------------------------------------------------------------------
// Seeding / mode
// ------------------------------------------------------------------------------------------------
void TextureGeneratorDialog::seed(
    std::uint32_t docW, std::uint32_t docH,
    std::optional<std::pair<std::string, TextureParams>> editing) {
    m_docW = docW;
    m_docH = docH;
    for (int g = 0; g < texture::kGeneratorCount; ++g)
        m_working[static_cast<std::size_t>(g)] =
            texture::defaultTextureParams(static_cast<texture::Generator>(g));
    m_editing = editing.has_value();
    if (m_editing) {
        m_editLabel = editing->first;
        m_generator = editing->second.generator;
        m_working[static_cast<std::size_t>(m_generator)] = std::move(editing->second);
    } else {
        m_generator = texture::Generator::Sky;
    }
    // Seed the solar calculator with today (noon UTC): "where is the sun right now-ish" is the
    // natural starting question. UI-side clock use only -- core stays clock-free (§8.3).
    if (const std::time_t now = std::time(nullptr); now != static_cast<std::time_t>(-1)) {
        std::tm tm{};
        // gmtime_s on the MSVC-flavoured CRT: reversed arguments, and it answers errno_t rather
        // than a pointer, so the success test differs too (0 == ok).
#if defined(_WIN32)
        if (gmtime_s(&tm, &now) == 0) {
#else
        if (gmtime_r(&now, &tm) != nullptr) {
#endif
            m_solYear = tm.tm_year + 1900;
            m_solMonth = tm.tm_mon + 1;
            m_solDay = tm.tm_mday;
        }
    }
    // The estimate state starts fresh each session -- including the mask & harmonize toggle,
    // which must never persist as a silently-armed default .
    m_estimate.reset();
    m_estimateSnapshot.reset();
    m_estimating = false;
    m_estimateEpoch = 0;
    m_conformWanted = false;
    m_sourceImage = {};
    m_sourceName.clear();
    m_bubbleThumb = {};
    m_bubbleThumbReady = false;
    m_sourceAvailable = false;
    if (m_host.sourceLayer) {
        // Probe once: the dialog is modal, so the active layer cannot change underneath it.
        if (std::optional<TextureGenHost::SourceLayer> src = m_host.sourceLayer()) {
            m_sourceAvailable = !src->docImage.empty();
            m_sourceName = src->name;
        }
    }
    // The moon-phase latch starts fresh each session; in edit mode on a sky layer, seed the
    // observer clock + the latch from the layer so the info panel / master clock reflect it.
    m_moonSource = 0;
    if (m_editing && m_generator == texture::Generator::Sky) {
        if (const auto* s = std::get_if<SkyParams>(&activeParams().spec)) {
            m_solYear = std::clamp(s->obsYear, 1900, 2200);
            m_solMonth = std::clamp(s->obsMonth, 1, 12);
            m_solDay = std::clamp(s->obsDay, 1, 31);
            m_solHour = std::clamp(s->obsHourUtc, 0.0, 24.0);
            m_solLat = std::clamp(static_cast<double>(s->obsLatitudeDeg), -90.0, 90.0);
            m_solLon = std::clamp(static_cast<double>(s->obsLongitudeDeg), -180.0, 180.0);
            m_moonSource = s->moonPhaseMode == 2 ? 1 : s->moonPhaseMode == 1 ? 2 : 0;
        }
    }
    m_ui->create->copy_label(m_editing ? _("Apply") : _("Create"));
    selectGenerator(m_generator); // rebuilds controls + syncs + notes + queues the first proxy
}

void TextureGeneratorDialog::selectGenerator(texture::Generator g) {
    if (m_baking) return; // fenced during a bake
    m_generator = g;
    for (std::size_t i = 0; i < m_ui->rail.size(); ++i)
        m_ui->rail[i]->setActive(static_cast<int>(i) == static_cast<int>(g));
    // Preset items follow the generator. Repopulated HERE, never from rebuildControls():
    // applyPreset() rebuilds the stack from INSIDE the preset dropdown's own callback, and
    // clearing the menu the widget is still delivering from would be a use-after-free.
    m_ui->preset->clear();
    m_ui->preset->add(_("Custom"));
    const texture::GeneratorTraits& traits = texture::generatorTraits(g);
    for (std::size_t i = 0; i < traits.presetCount(); ++i)
        m_ui->preset->add(menuEscape(traits.presetName(i)).c_str());
    rebuildControls();
    syncControls();
    char note[160];
    if (m_editing)
        std::snprintf(note, sizeof(note), _("Applies to \"%s\"."), m_editLabel.c_str());
    else
        std::snprintf(note, sizeof(note), _("Creates a new \"%s\" layer."),
                      texture::generatorName(g));
    m_ui->note->copy_label(note);
    updateSkyInfo(); // populate / hide the under-preview almanac panel for this generator
    updateEstimateUi();
    requestProxy();
}

// ------------------------------------------------------------------------------------------------
// The control stack
// ------------------------------------------------------------------------------------------------
// The shared builder kit (S55-g): one per-rebuild context carrying the stack geometry plus the
// row helpers every generator's builder composes -- each helper binds a widget to a get/set pair
// over the ACTIVE params and registers its sync closure. A nested struct so the helpers reach the
// dialog's private applyEdit/bindings/sync machinery; the context itself dies with the rebuild,
// so every closure captures the DIALOG pointer (which outlives it), never `this`.
struct TextureGeneratorDialog::ControlsCtx {
    TextureGeneratorDialog& d;
    const Palette& pal;
    Fl_Group* stack;
    int cw;         // stack content width
    int left;       // left text margin
    int fieldW;     // control field width
    int fieldLeft;  // control field x
    int labelW;     // caption width
    int cy;         // the running row cursor (builders read and advance it)

    Binding* bind(std::function<void()> fn) {
        auto b = std::make_unique<Binding>();
        b->fn = std::move(fn);
        Binding* raw = b.get();
        d.m_ui->bindings.push_back(std::move(b));
        return raw;
    }
    void caption(int rowY, const char* text, const char* tip = nullptr) {
        auto* c = new Fl_Box(stack->x() + left, rowY, labelW, kRowH, nullptr);
        c->copy_label(text);
        c->box(FL_NO_BOX);
        c->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        c->labelfont(FL_HELVETICA);
        c->labelsize(12);
        c->labelcolor(toFl(pal.textMuted));
        if (tip != nullptr) c->copy_tooltip(tip);
    }
    // A labelled ScrubSlider bound to a get/set pair over the ACTIVE params.
    ScrubSlider* slider(const char* label, double mn, double mx, double step, const char* suffix,
                        std::function<double(const TextureParams&)> get,
                        std::function<void(TextureParams&, double)> set,
                        const char* tip = nullptr) {
        caption(cy, label, tip);
        auto* s = new ScrubSlider(fieldLeft, cy, fieldW, kRowH);
        s->range(mn, mx);
        s->step(step);
        s->setSuffix(suffix);
        s->setCellColor(pal.windowBg);
        s->setRuler(d.m_ruler);
        s->when(FL_WHEN_CHANGED);
        if (tip != nullptr) s->copy_tooltip(tip);
        TextureGeneratorDialog* dlg = &d;
        s->callback(controlThunk, bind([dlg, s, set] {
                        dlg->applyEdit([&](TextureParams& p) { set(p, s->value()); });
                    }));
        d.m_ui->syncFns.push_back([dlg, s, get] { s->value(get(dlg->activeParams())); });
        cy += kRowH + kRowGap;
        return s;
    }
    CheckBox* check(int px, int pw, const char* label,
                    std::function<bool(const TextureParams&)> get,
                    std::function<void(TextureParams&, bool)> set, const char* tip = nullptr) {
        auto* c = new CheckBox(px, cy, pw, kRowH, label);
        c->setGroundColor(pal.windowBg);
        if (tip != nullptr) c->copy_tooltip(tip);
        TextureGeneratorDialog* dlg = &d;
        c->setOnToggle([dlg, set](bool on) {
            if (dlg->m_seeding) return;
            dlg->applyEdit([&](TextureParams& p) { set(p, on); });
        });
        d.m_ui->syncFns.push_back([dlg, c, get] { c->setChecked(get(dlg->activeParams())); });
        return c;
    }
    Dropdown* dropdown(const char* label, std::initializer_list<const char*> items,
                       std::function<int(const TextureParams&)> get,
                       std::function<void(TextureParams&, int)> set, const char* tip = nullptr) {
        caption(cy, label, tip);
        auto* dd = new Dropdown(fieldLeft, cy, fieldW, kRowH);
        for (const char* it : items) dd->add(it);
        if (tip != nullptr) dd->copy_tooltip(tip);
        TextureGeneratorDialog* dlg = &d;
        dd->callback(controlThunk, bind([dlg, dd, set] {
                         dlg->applyEdit([&](TextureParams& p) { set(p, dd->value()); });
                     }));
        d.m_ui->syncFns.push_back([dlg, dd, get] { dd->value(get(dlg->activeParams())); });
        cy += kRowH + kRowGap;
        return dd;
    }
    SwatchChip* colorRow(const char* label,
                         std::function<common::ColorF(const TextureParams&)> get,
                         std::function<void(TextureParams&, common::ColorF)> set,
                         const char* tip = nullptr) {
        caption(cy, label, tip);
        auto* chip = new SwatchChip(fieldLeft, cy, fieldW, kRowH);
        chip->setGroundColor(pal.windowBg);
        chip->setInteractive(true);
        if (tip != nullptr) chip->copy_tooltip(tip);
        TextureGeneratorDialog* dlg = &d;
        chip->setOnClick([dlg, chip, get, set] {
            const common::ColorF cur = get(dlg->activeParams());
            dlg->openColorFlyout(chip, to8(cur), [dlg, set, cur](common::Color8 c) {
                dlg->applyEdit([&](TextureParams& p) {
                    set(p, common::ColorF{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, cur.a});
                });
            });
        });
        d.m_ui->syncFns.push_back(
            [dlg, chip, get] { chip->setColour(to8(get(dlg->activeParams()))); });
        cy += kRowH + kRowGap;
        return chip;
    }
    // A collapsible section; returns whether its contents should be emitted.
    bool section(const char* key, const char* title, bool defaultOpen = false) {
        const std::string k = key;
        const auto it = d.m_open.find(k);
        const bool open = it != d.m_open.end() ? it->second : defaultOpen;
        d.m_open[k] = open;
        auto* head = new SectionHeader(stack->x() + 2, cy, cw - 4, kRowH, nullptr);
        head->copy_label(title);
        head->setOpen(open);
        TextureGeneratorDialog* dlg = &d;
        head->callback(controlThunk, bind([dlg, k] {
                           dlg->m_open[k] = !dlg->m_open[k];
                           dlg->rebuildControls();
                           dlg->syncControls();
                       }));
        cy += kRowH + 6;
        return open;
    }
    void twoChecks(const char* l1, std::function<bool(const TextureParams&)> g1,
                   std::function<void(TextureParams&, bool)> s1, const char* t1, const char* l2,
                   std::function<bool(const TextureParams&)> g2,
                   std::function<void(TextureParams&, bool)> s2, const char* t2) {
        const int half = (cw - 2 * left - 8) / 2;
        check(stack->x() + left, half, l1, std::move(g1), std::move(s1), t1);
        check(stack->x() + left + half + 8, half, l2, std::move(g2), std::move(s2), t2);
        cy += kRowH + kRowGap;
    }
};

void TextureGeneratorDialog::rebuildControls() {
    Fl_Group* stack = m_ui->stack;
    // The persistent date-picker (it owns a pop-up sub-window) must NOT be deleted by stack->clear()
    // -- detach it back onto the dialog first, and re-add it to the stack only if the sky date &
    // place section is open below. This keeps its sub-window a stable child of the dialog while the
    // widget itself scrolls with the stack (the ui::Popover reparent trick).
    if (m_ui->datePicker != nullptr) {
        if (m_ui->datePicker->parent() != nullptr && m_ui->datePicker->parent() != this)
            m_ui->datePicker->parent()->remove(m_ui->datePicker);
        if (m_ui->datePicker->parent() != this) add(m_ui->datePicker);
        m_ui->datePicker->hide();
    }
    stack->clear();
    stack->resizable(nullptr); // Fl_Group::clear() resets resizable_ (the LayerEffects lesson)
    m_ui->bindings.clear();
    m_ui->syncFns.clear();
    m_ui->solarReadout = nullptr;
    m_ui->place = nullptr;
    m_ui->moonReadout = nullptr;
    m_ui->estimate = nullptr;

    const int cw = stack->w();
    const int left = 10;
    const int fieldW = 172;
    const int fieldLeft = stack->x() + cw - 8 - fieldW;
    const int labelW = fieldLeft - (stack->x() + left) - 6;
    ControlsCtx c{*this,  activePalette(), stack, cw, left, fieldW, fieldLeft, labelW,
                  stack->y() + 8};
    stack->begin();
    // One builder per generator, enum order (this table + the kGeneratorUi row are the dialog's
    // whole per-generator surface -- the S55-g registry discipline).
    using BuildFn = void (TextureGeneratorDialog::*)(ControlsCtx&);
    static constexpr BuildFn kBuilders[texture::kGeneratorCount] = {
        &TextureGeneratorDialog::buildSkyControls,
        &TextureGeneratorDialog::buildPaperControls,
        &TextureGeneratorDialog::buildGrassControls,
        &TextureGeneratorDialog::buildWoodControls,
        &TextureGeneratorDialog::buildMarbleControls,
        &TextureGeneratorDialog::buildStoneControls,
        &TextureGeneratorDialog::buildCanvasControls,
        &TextureGeneratorDialog::buildMetalControls,
    };
    (this->*kBuilders[static_cast<std::size_t>(m_generator)])(c);

    stack->end();
    stack->size(stack->w(), std::max(60, c.cy - stack->y() + 10));
    m_ui->scroll->scroll_to(0, 0);
    m_ui->scroll->redraw();
    // The solar / moon readouts are recreated empty by every rebuild (a section toggle, a deck
    // add/remove); refresh them here so they never flash blank until the next observer edit.
    updateSkyInfo();
}

void TextureGeneratorDialog::buildSkyControls(ControlsCtx& c) {
    // Aliases so the row recipes read exactly as they did when they lived inline in
    // rebuildControls (and as every other builder's do).
    const Palette& pal = c.pal;
    Fl_Group* stack = c.stack;
    const int left = c.left, fieldW = c.fieldW, fieldLeft = c.fieldLeft, cw = c.cw;
    int& cy = c.cy;
    const auto bind = [&c](std::function<void()> fn) { return c.bind(std::move(fn)); };
    const auto caption = [&c](auto&&... a) { c.caption(std::forward<decltype(a)>(a)...); };
    const auto slider = [&c](auto&&... a) { return c.slider(std::forward<decltype(a)>(a)...); };
    const auto check = [&c](auto&&... a) { return c.check(std::forward<decltype(a)>(a)...); };
    const auto dropdown = [&c](const char* label, std::initializer_list<const char*> items,
                               std::function<int(const TextureParams&)> get,
                               std::function<void(TextureParams&, int)> set,
                               const char* tip = nullptr) {
        return c.dropdown(label, items, std::move(get), std::move(set), tip);
    };
    const auto section = [&c](auto&&... a) { return c.section(std::forward<decltype(a)>(a)...); };
    const auto twoChecks = [&c](auto&&... a) { c.twoChecks(std::forward<decltype(a)>(a)...); };

    // Spec accessors (the active arm; applyEdit only ever runs while that arm is live).
    const auto skyOf = [](TextureParams& p) -> SkyParams& { return std::get<SkyParams>(p.spec); };
    const auto skyC = [](const TextureParams& p) -> const SkyParams& {
        return std::get<SkyParams>(p.spec);
    };

    // ---- "Estimate from layer" action row (S55 phase 2; design §7) ----
    // Behaves like a preset: one estimate lands as one applyEdit. Disabled with the wand's
    // copy-family hint when the active layer owns no pixels; a hover opens the EstimateBubble.
    {
        const int revertW = 70;
        const bool haveRevert = m_estimateSnapshot.has_value();
        const int estW = cw - 2 * left - (haveRevert ? revertW + 8 : 0);
        auto* est = new HoverButton(stack->x() + left, cy, estW, kRowH,
                                    _("Estimate from layer\xE2\x80\xA6"));
        est->callback(controlThunk, bind([this] { estimateFromLayer(); }));
        if (m_sourceAvailable) {
            est->copy_tooltip(
                _("Analyzes this layer; sets horizon, camera, sun & sky to match"));
            est->onHover = [this](bool inside) { estimateHover(inside); };
        } else {
            est->deactivate();
            est->copy_tooltip(_("The active layer has no pixels to analyze. Rasterize it, or "
                                "select a pixel layer."));
        }
        m_ui->estimate = est;
        if (haveRevert) {
            auto* rev = new FlatButton(stack->x() + left + estW + 8, cy, revertW, kRowH,
                                       _("Revert"));
            rev->callback(controlThunk, bind([this] { revertEstimate(); }));
            rev->copy_tooltip(_("Restore the settings from before the estimate"));
        }
        cy += kRowH + kRowGap;

        // The estimate summary: per-quantity values, confidence tags and the engine's honesty
        // lines, as compact muted rows (the info panel keeps its almanac).
        const std::vector<std::string> lines = estimateSummaryForTest();
        for (const std::string& line : lines) {
            auto* row = new Fl_Box(stack->x() + left + 4, cy, cw - 2 * left - 4, 15);
            row->copy_label(line.c_str());
            row->box(FL_NO_BOX);
            row->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
            row->labelfont(FL_HELVETICA);
            row->labelsize(11);
            row->labelcolor(toFl(pal.textMuted));
            cy += 16;
        }
        // The morning/afternoon ambiguity's one-click swap (a photo cannot resolve it; the
        // almanac offers the other crossing).
        if (const auto swap = estimateTimeSwap()) {
            char b[64];
            std::snprintf(b, sizeof(b), _("Use %s UTC instead"),
                          hhmmUtc(swap->hourUtc).c_str());
            auto* sw = new FlatButton(stack->x() + left, cy, 170, 22, nullptr);
            sw->copy_label(b);
            sw->labelsize(11);
            sw->callback(controlThunk, bind([this] { swapEstimateTime(); }));
            sw->copy_tooltip(_("The sun crosses this height twice a day; switch to the other "
                               "crossing"));
            cy += 22 + kRowGap;
        }
        if (!lines.empty()) cy += 2;
    }

    {
        twoChecks(_("Sky dome"), [skyC](const TextureParams& p) { return skyC(p).enableDome; },
                  [skyOf](TextureParams& p, bool v) { skyOf(p).enableDome = v; },
                  _("Off = a transparent layer carrying only sun/clouds (§3.4)"), _("Sun"),
                  [skyC](const TextureParams& p) { return skyC(p).enableSun; },
                  [skyOf](TextureParams& p, bool v) { skyOf(p).enableSun = v; },
                  _("The disc + aureole element (clouds stay sun-lit either way)"));
        twoChecks(_("Clouds"), [skyC](const TextureParams& p) { return skyC(p).enableClouds; },
                  [skyOf](TextureParams& p, bool v) { skyOf(p).enableClouds = v; }, nullptr,
                  _("Haze"), [skyC](const TextureParams& p) { return skyC(p).enableHaze; },
                  [skyOf](TextureParams& p, bool v) { skyOf(p).enableHaze = v; },
                  _("Aerial-perspective boost toward the horizon tint"));
        // The handful of high-impact knobs stay up top (Sun height sets day vs night); everything
        // else lives in the disclosure sections below (user 2026-07-15: the flat wall of sliders
        // read as "clunky and unintuitive").
        slider(_("Sun height"), -30, 90, 0.5, "\xC2\xB0",
               [skyC](const TextureParams& p) { return skyC(p).sunElevationDeg; },
               [skyOf](TextureParams& p, double v) { skyOf(p).sunElevationDeg = v; },
               _("0 = on the horizon; low = golden light; below 0 = twilight into night"));
        slider(_("Sun direction"), 0, 360, 0.5, "\xC2\xB0",
               [skyC](const TextureParams& p) { return skyC(p).sunAzimuthDeg; },
               [skyOf](TextureParams& p, double v) { skyOf(p).sunAzimuthDeg = v; },
               _("Compass direction; 180 faces the frame centre (drag the sun too)"));
        slider(_("Cloud cover"), 0, 1, 0.01, "",
               [skyC](const TextureParams& p) { return skyC(p).cloudCoverage; },
               [skyOf](TextureParams& p, double v) { skyOf(p).cloudCoverage = v; },
               _("The master dial; each deck multiplies it"));
        slider(_("Turbidity"), 1, 10, 0.05, "",
               [skyC](const TextureParams& p) { return skyC(p).turbidity; },
               [skyOf](TextureParams& p, double v) { skyOf(p).turbidity = v; },
               _("Atmosphere thickness: 1 crystalline, 10 murky"));

        if (section("sky:clouds", _("Clouds"))) {
            slider(_("Cloud scale"), 0.2, 5, 0.01, "x",
                   [](const TextureParams& p) { return p.scale; },
                   [](TextureParams& p, double v) { p.scale = v; },
                   _("Cloud feature size (the dome and camera stay physical, §8.3)"));
            dropdown(_("Quality"), {_("Volumetric (best)"), _("Fast 2D")},
                     [skyC](const TextureParams& p) { return skyC(p).volumetricClouds ? 0 : 1; },
                     [skyOf](TextureParams& p, int v) { skyOf(p).volumetricClouds = v == 0; },
                     _("Cumulus / storm towers ray-march a real 3D field (slower, rounder)"));
            const std::size_t n = skyC(activeParams()).cloudLayers.size();
            for (std::size_t i = 0; i < n; ++i) {
                auto* en = new CheckBox(stack->x() + left, cy, 24, kRowH, "");
                en->setGroundColor(pal.windowBg);
                en->copy_tooltip(_("Deck on/off"));
                en->setOnToggle([this, i, skyOf](bool on) {
                    if (m_seeding) return;
                    applyEdit([&](TextureParams& p) {
                        if (i < skyOf(p).cloudLayers.size()) skyOf(p).cloudLayers[i].enabled = on;
                    });
                });
                m_ui->syncFns.push_back([this, en, i, skyC] {
                    const auto& decks = skyC(activeParams()).cloudLayers;
                    en->setChecked(i < decks.size() && decks[i].enabled);
                });
                auto* d = new Dropdown(stack->x() + left + 28, cy, fieldW - 28 + 8, kRowH);
                for (int t = 0; t < texture::kCloudTypeCount; ++t)
                    d->add(texture::cloudTypeName(static_cast<texture::CloudType>(t)));
                d->callback(controlThunk, bind([this, d, i, skyOf] {
                                applyEdit([&](TextureParams& p) {
                                    if (i < skyOf(p).cloudLayers.size())
                                        skyOf(p).cloudLayers[i].type =
                                            static_cast<texture::CloudType>(
                                                std::clamp(d->value(), 0,
                                                           texture::kCloudTypeCount - 1));
                                });
                            }));
                m_ui->syncFns.push_back([this, d, i, skyC] {
                    const auto& decks = skyC(activeParams()).cloudLayers;
                    if (i < decks.size()) d->value(static_cast<int>(decks[i].type));
                });
                auto* rm = new FlatButton(fieldLeft + fieldW - 24, cy, 24, kRowH, "x");
                rm->copy_tooltip(_("Remove this deck"));
                rm->callback(controlThunk, bind([this, i, skyOf] {
                                 applyEdit([&](TextureParams& p) {
                                     auto& decks = skyOf(p).cloudLayers;
                                     if (i < decks.size())
                                         decks.erase(decks.begin() + static_cast<long>(i));
                                 });
                                 rebuildControls();
                                 syncControls();
                             }));
                cy += kRowH + 2;
                caption(cy, _("Amount"));
                auto* amt = new ScrubSlider(fieldLeft, cy, fieldW, kRowH);
                amt->range(0, 2);
                amt->step(0.01);
                amt->setSuffix("x");
                amt->setCellColor(pal.windowBg);
                amt->setRuler(m_ruler);
                amt->when(FL_WHEN_CHANGED);
                amt->copy_tooltip(_("This deck's multiplier on Cloud cover"));
                amt->callback(controlThunk, bind([this, amt, i, skyOf] {
                                  applyEdit([&](TextureParams& p) {
                                      if (i < skyOf(p).cloudLayers.size())
                                          skyOf(p).cloudLayers[i].coverageBias = amt->value();
                                  });
                              }));
                m_ui->syncFns.push_back([this, amt, i, skyC] {
                    const auto& decks = skyC(activeParams()).cloudLayers;
                    if (i < decks.size()) amt->value(decks[i].coverageBias);
                });
                cy += kRowH + kRowGap;
            }
            if (n < kMaxDecks) {
                auto* add = new FlatButton(stack->x() + left, cy, 110, kRowH, _("Add deck"));
                add->callback(controlThunk, bind([this, skyOf] {
                                  applyEdit([&](TextureParams& p) {
                                      if (skyOf(p).cloudLayers.size() < kMaxDecks)
                                          skyOf(p).cloudLayers.push_back(
                                              texture::CloudLayerParams{});
                                  });
                                  rebuildControls();
                                  syncControls();
                              }));
                cy += kRowH + kRowGap;
            }
        }

        if (section("sky:night", _("Night & moon"))) {
            check(stack->x() + left, cw - 2 * left, _("Moon"),
                  [skyC](const TextureParams& p) { return skyC(p).enableMoon; },
                  [skyOf](TextureParams& p, bool v) { skyOf(p).enableMoon = v; },
                  _("A real near-side Moon; its phase comes from the source below"));
            cy += kRowH + kRowGap;
            // Phase source: ephemeris (from the date & place clock) vs a manual illuminated
            // fraction. The FIRST of {date/place} or {manual} touched this session wins (the
            // dialog-local m_moonSource latch); this dropdown is the explicit override.
            caption(cy, _("Moon phase"));
            auto* src = new Dropdown(fieldLeft, cy, fieldW, kRowH);
            src->add(_("From date & place"));
            src->add(_("Manual"));
            src->value(m_moonSource == 2 ? 1 : 0);
            src->copy_tooltip(_("\"From date & place\" uses the real ephemeris; \"Manual\" sets the "
                                "phase by hand. Whichever you touch first is remembered."));
            src->callback(controlThunk,
                          bind([this, src] { selectMoonSource(src->value() == 1 ? 2 : 1); }));
            cy += kRowH + kRowGap;
            if (m_moonSource != 1) {
                // Manual illuminated fraction (shown until date & place has been latched).
                caption(cy, _("Illumination"), _("New at 0%, full at 100%"));
                auto* frac = new ScrubSlider(fieldLeft, cy, fieldW, kRowH);
                frac->range(0, 1);
                frac->step(0.01);
                frac->setCellColor(pal.windowBg);
                frac->setRuler(m_ruler);
                frac->when(FL_WHEN_CHANGED);
                frac->copy_tooltip(_("Manual moon phase: 0 = new, 0.5 = half, 1 = full"));
                frac->callback(controlThunk, bind([this, frac, src] {
                    if (m_seeding) return;
                    if (m_moonSource == 0) { // first-set-wins: touching manual latches it
                        m_moonSource = 2;
                        src->value(1);
                        src->redraw();
                    }
                    applyEdit([&](TextureParams& p) {
                        if (auto* s = std::get_if<SkyParams>(&p.spec)) {
                            s->moonIlluminatedFraction = std::clamp(frac->value(), 0.0, 1.0);
                            s->moonPhaseMode = 1;
                            s->enableMoon = true;
                        }
                    });
                    if (m_ui->moonReadout != nullptr) {
                        m_ui->moonReadout->copy_label(moonPhaseReadout().c_str());
                        m_ui->moonReadout->redraw();
                    }
                }));
                m_ui->syncFns.push_back([this, frac] {
                    if (const auto* s = std::get_if<SkyParams>(&activeParams().spec))
                        frac->value(s->moonIlluminatedFraction);
                });
                cy += kRowH + kRowGap;
            }
            // The current phase name, so the source's effect is legible either way.
            auto* mr = new Fl_Box(stack->x() + left, cy, cw - 2 * left, kRowH);
            mr->box(FL_NO_BOX);
            mr->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            mr->labelfont(FL_HELVETICA_ITALIC);
            mr->labelsize(11);
            mr->labelcolor(toFl(pal.textMuted));
            mr->copy_label(moonPhaseReadout().c_str());
            m_ui->moonReadout = mr;
            cy += kRowH + kRowGap;
            slider(_("Moon direction"), 0, 360, 1, "\xC2\xB0",
                   [skyC](const TextureParams& p) { return skyC(p).moonAzimuthDeg; },
                   [skyOf](TextureParams& p, double v) { skyOf(p).moonAzimuthDeg = v; },
                   _("Compass, like the sun (drag the moon in the preview too)"));
            slider(_("Moon height"), 0, 80, 0.5, "\xC2\xB0",
                   [skyC](const TextureParams& p) { return skyC(p).moonElevationDeg; },
                   [skyOf](TextureParams& p, double v) { skyOf(p).moonElevationDeg = v; });
            slider(_("Moon size"), 0.5, 4, 0.05, "x",
                   [skyC](const TextureParams& p) { return skyC(p).moonScale; },
                   [skyOf](TextureParams& p, double v) { skyOf(p).moonScale = v; },
                   _("1 = the honest 0.26\xC2\xB0 disc; postcards lie larger"));
            slider(_("Stars"), 0, 1, 0.01, "",
                   [skyC](const TextureParams& p) { return skyC(p).starsAmount; },
                   [skyOf](TextureParams& p, double v) { skyOf(p).starsAmount = v; },
                   _("Fade in once the sun is well below the horizon"));
        }

        if (section("sky:camera", _("Camera"))) {
            slider(_("View width"), 15, 150, 0.5, "\xC2\xB0",
                   [skyC](const TextureParams& p) { return skyC(p).fovDeg; },
                   [skyOf](TextureParams& p, double v) { skyOf(p).fovDeg = v; },
                   _("Horizontal field of view (the bracket handles drag it too)"));
            slider(_("Horizon"), -10, 85, 0.5, "\xC2\xB0",
                   [skyC](const TextureParams& p) { return skyC(p).pitchDeg; },
                   [skyOf](TextureParams& p, double v) { skyOf(p).pitchDeg = v; },
                   _("Camera pitch above the horizon (drag the horizon line too)"));
            slider(_("Roll"), -45, 45, 0.25, "\xC2\xB0",
                   [skyC](const TextureParams& p) { return skyC(p).rollDeg; },
                   [skyOf](TextureParams& p, double v) { skyOf(p).rollDeg = v; },
                   _("Horizon tilt, for matching a hand-held shot"));
            slider(_("Tilt-shift"), -0.5, 0.5, 0.005, "",
                   [skyC](const TextureParams& p) { return skyC(p).shiftY; },
                   [skyOf](TextureParams& p, double v) { skyOf(p).shiftY = v; },
                   _("Slides the horizon without pitching the rays (architectural look)"));
        }

        if (section("sky:solar", _("Sky by date & place"))) {
            // The master clock: a real date + a place drive the sun, and (while the moon source is
            // "from date & place") the moon and star field too. The info panel under the preview
            // reads the same almanac.
            caption(cy, _("Date (UTC)"));
            stack->add(m_ui->datePicker); // re-parent the persistent picker into the scroll here
            m_ui->datePicker->resize(fieldLeft, cy, fieldW, kRowH);
            m_ui->datePicker->setDate(m_solYear, m_solMonth, m_solDay);
            m_ui->datePicker->show();
            m_ui->datePicker->copy_tooltip(_("Pick a date; the sun, moon and stars follow it"));
            m_ui->syncFns.push_back(
                [this] { m_ui->datePicker->setDate(m_solYear, m_solMonth, m_solDay); });
            cy += kRowH + kRowGap;
            {
                // Time reads like a CLOCK, not a decimal (user 2026-07-15: "21.75 h" is nonsense)
                // -- the shared TimeDrum roller.
                constexpr int kDrumH = 54;
                caption(cy + (kDrumH - kRowH) / 2, _("Time (UTC)"));
                auto* drum = new TimeDrum(fieldLeft, cy, fieldW, kDrumH);
                drum->copy_tooltip(_("Drag or scroll a drum to spin it"));
                drum->callback(controlThunk, bind([this, drum] {
                                   if (m_seeding) return;
                                   m_solHour = drum->value();
                                   observerChanged(true);
                               }));
                m_ui->syncFns.push_back([this, drum] { drum->setValue(m_solHour); });
                cy += kDrumH + kRowGap;
            }
            {
                // The place, as a compact readout (nearest catalogued city + coordinates); a
                // click opens the world-map flyout, where a draggable pin IS the lat/lon (the
                // curated city dropdown's successor -- the catalogue itself stays, as the
                // nearest-city lookup and the pin's snap targets).
                caption(cy, _("Place"));
                auto* place = new MapPicker(fieldLeft, cy, fieldW, kRowH);
                place->showNearest(m_solLat, m_solLon);
                place->copy_tooltip(
                    _("Place the observer on a world map; the pin snaps to nearby cities"));
                place->setOnOpen([this, place] { openMapFlyout(place); });
                m_ui->place = place;
                cy += kRowH + kRowGap;
            }
            {
                caption(cy, _("Latitude"));
                auto* s = new ScrubSlider(fieldLeft, cy, fieldW, kRowH);
                s->range(-90, 90);
                s->step(0.1);
                s->setSuffix("\xC2\xB0");
                s->setCellColor(pal.windowBg);
                s->setRuler(m_ruler);
                s->when(FL_WHEN_CHANGED);
                s->callback(controlThunk, bind([this, s] {
                                if (m_seeding) return;
                                m_solLat = s->value();
                                observerChanged(true);
                            }));
                m_ui->syncFns.push_back([this, s] { s->value(m_solLat); });
                cy += kRowH + kRowGap;
            }
            {
                caption(cy, _("Longitude"));
                auto* s = new ScrubSlider(fieldLeft, cy, fieldW, kRowH);
                s->range(-180, 180);
                s->step(0.1);
                s->setSuffix("\xC2\xB0");
                s->setCellColor(pal.windowBg);
                s->setRuler(m_ruler);
                s->when(FL_WHEN_CHANGED);
                s->callback(controlThunk, bind([this, s] {
                                if (m_seeding) return;
                                m_solLon = s->value();
                                observerChanged(true);
                            }));
                m_ui->syncFns.push_back([this, s] { s->value(m_solLon); });
                cy += kRowH + kRowGap;
            }
            auto* out = new Fl_Box(stack->x() + left, cy, cw - 2 * left, kRowH);
            out->box(FL_NO_BOX);
            out->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            out->labelfont(FL_HELVETICA_ITALIC);
            out->labelsize(11);
            out->labelcolor(toFl(pal.textMuted));
            m_ui->solarReadout = out;
            cy += kRowH + kRowGap;
        }

        if (section("sky:advanced", _("Advanced"))) {
            slider(_("Exposure"), -3, 3, 0.05, " EV",
                   [skyC](const TextureParams& p) { return skyC(p).exposure; },
                   [skyOf](TextureParams& p, double v) { skyOf(p).exposure = v; });
            slider(_("Ground albedo"), 0, 1, 0.01, "",
                   [skyC](const TextureParams& p) { return skyC(p).groundAlbedo; },
                   [skyOf](TextureParams& p, double v) { skyOf(p).groundAlbedo = v; },
                   _("Light bounced back into the dome from the ground"));
            slider(_("Sun disc size"), 0.3, 4, 0.05, "x",
                   [skyC](const TextureParams& p) { return skyC(p).sunDiscScale; },
                   [skyOf](TextureParams& p, double v) { skyOf(p).sunDiscScale = v; },
                   _("1 = the physical 0.255\xC2\xB0 half-angle"));
            // Lens flare: a self-contained pair (toggle + strength), kept contiguous.
            check(stack->x() + left, cw - 2 * left, _("Lens flare"),
                  [skyC](const TextureParams& p) { return skyC(p).enableLensFlare; },
                  [skyOf](TextureParams& p, bool v) { skyOf(p).enableLensFlare = v; },
                  _("Photographic ghosts, halo and starburst cast by the sun"));
            cy += kRowH + kRowGap;
            slider(_("Flare strength"), 0, 1, 0.01, "",
                   [skyC](const TextureParams& p) { return skyC(p).flareStrength; },
                   [skyOf](TextureParams& p, double v) { skyOf(p).flareStrength = v; },
                   _("Ghost and starburst energy; the flare fades as the sun leaves the frame"));
            slider(_("Wind direction"), 0, 360, 1, "\xC2\xB0",
                   [skyC](const TextureParams& p) { return skyC(p).windDirectionDeg; },
                   [skyOf](TextureParams& p, double v) { skyOf(p).windDirectionDeg = v; },
                   _("Where the wind blows toward (the arrow drags it too)"));
            slider(_("Wind strength"), 0, 1, 0.01, "",
                   [skyC](const TextureParams& p) { return skyC(p).windStrength; },
                   [skyOf](TextureParams& p, double v) { skyOf(p).windStrength = v; },
                   _("Shear: streaks and wisp elongation along the wind"));
        }
    }
}

void TextureGeneratorDialog::buildPaperControls(ControlsCtx& c) {
    const auto slider = [&c](auto&&... a) { return c.slider(std::forward<decltype(a)>(a)...); };
    const auto dropdown = [&c](const char* label, std::initializer_list<const char*> items,
                               std::function<int(const TextureParams&)> get,
                               std::function<void(TextureParams&, int)> set,
                               const char* tip = nullptr) {
        return c.dropdown(label, items, std::move(get), std::move(set), tip);
    };
    const auto colorRow = [&c](auto&&... a) { return c.colorRow(std::forward<decltype(a)>(a)...); };
    const auto section = [&c](auto&&... a) { return c.section(std::forward<decltype(a)>(a)...); };
    const auto twoChecks = [&c](auto&&... a) { c.twoChecks(std::forward<decltype(a)>(a)...); };

    const auto paperOf = [](TextureParams& p) -> PaperParams& {
        return std::get<PaperParams>(p.spec);
    };
    const auto paperC = [](const TextureParams& p) -> const PaperParams& {
        return std::get<PaperParams>(p.spec);
    };
    {
        dropdown(_("Kind"), {_("Wove"), _("Laid"), _("Felt")},
                 [paperC](const TextureParams& p) { return static_cast<int>(paperC(p).kind); },
                 [paperOf](TextureParams& p, int v) {
                     paperOf(p).kind = static_cast<texture::PaperKind>(std::clamp(v, 0, 2));
                 },
                 _("Wove = machine paper; Laid carries laid + chain lines; Felt = coarse relief"));
        twoChecks(_("Deckle edge"),
                  [paperC](const TextureParams& p) { return paperC(p).deckleEdge; },
                  [paperOf](TextureParams& p, bool v) { paperOf(p).deckleEdge = v; },
                  _("A torn transparent fringe at the sheet border (§3.4)"), _("Print tooth"),
                  [paperC](const TextureParams& p) { return paperC(p).printTooth; },
                  [paperOf](TextureParams& p, bool v) { paperOf(p).printTooth = v; },
                  _("Toner speckle caught in the tooth valleys"));
        colorRow(_("Tint"), [paperC](const TextureParams& p) { return paperC(p).tint; },
                 [paperOf](TextureParams& p, common::ColorF c) { paperOf(p).tint = c; });
        slider(_("Roughness"), 0, 1, 0.01, "",
               [paperC](const TextureParams& p) { return paperC(p).roughness; },
               [paperOf](TextureParams& p, double v) { paperOf(p).roughness = v; },
               _("Tooth relief height"));
        slider(_("Fibre"), 0, 1, 0.01, "",
               [paperC](const TextureParams& p) { return paperC(p).fiber; },
               [paperOf](TextureParams& p, double v) { paperOf(p).fiber = v; },
               _("Spectral fibre streaks along the grain"));
        slider(_("Grain angle"), 0, 180, 0.5, "\xC2\xB0",
               [paperC](const TextureParams& p) { return paperC(p).grainAngleDeg; },
               [paperOf](TextureParams& p, double v) { paperOf(p).grainAngleDeg = v; },
               _("The fibre axis (the ring handle drags it too)"));
        slider(_("Grain strength"), 0, 1, 0.01, "",
               [paperC](const TextureParams& p) { return paperC(p).grainAnisotropy; },
               [paperOf](TextureParams& p, double v) { paperOf(p).grainAnisotropy = v; },
               _("0 isotropic, 1 strongly directional"));
        slider(_("Grain scale"), 0.25, 8, 0.01, "x",
               [](const TextureParams& p) { return p.scale; },
               [](TextureParams& p, double v) { p.scale = v; },
               _("Feature size per document pixel -- match your print DPI (§8.3)"));

        if (section("paper:ruling", _("Ruling (laid)"))) {
            slider(_("Laid pitch"), 2, 20, 0.1, "px",
                   [paperC](const TextureParams& p) { return paperC(p).laidSpacing; },
                   [paperOf](TextureParams& p, double v) { paperOf(p).laidSpacing = v; },
                   _("Fine parallel line spacing at Scale 1 (Laid kind)"));
            slider(_("Chain pitch"), 20, 300, 1, "px",
                   [paperC](const TextureParams& p) { return paperC(p).chainSpacing; },
                   [paperOf](TextureParams& p, double v) { paperOf(p).chainSpacing = v; },
                   _("Sparse perpendicular chain-line spacing (Laid kind)"));
            slider(_("Depth"), 0, 1, 0.01, "",
                   [paperC](const TextureParams& p) { return paperC(p).laidDepth; },
                   [paperOf](TextureParams& p, double v) { paperOf(p).laidDepth = v; },
                   _("Prominence of the ruling"));
        }
        if (section("paper:surface", _("Surface & light"))) {
            slider(_("Matte"), 0, 1, 0.01, "",
                   [paperC](const TextureParams& p) { return paperC(p).matte; },
                   [paperOf](TextureParams& p, double v) { paperOf(p).matte = v; },
                   _("Oren-Nayar roughness: 0 smooth diffuse, 1 very matte"));
            slider(_("Sheen"), 0, 1, 0.01, "",
                   [paperC](const TextureParams& p) { return paperC(p).sheen; },
                   [paperOf](TextureParams& p, double v) { paperOf(p).sheen = v; },
                   _("Coated-stock highlight (0 = uncoated)"));
            slider(_("Light direction"), 0, 360, 1, "\xC2\xB0",
                   [paperC](const TextureParams& p) { return paperC(p).lightAzimuthDeg; },
                   [paperOf](TextureParams& p, double v) { paperOf(p).lightAzimuthDeg = v; },
                   _("Raking light compass; 0 = from the top of the frame"));
            slider(_("Light height"), 5, 85, 0.5, "\xC2\xB0",
                   [paperC](const TextureParams& p) { return paperC(p).lightElevationDeg; },
                   [paperOf](TextureParams& p, double v) { paperOf(p).lightElevationDeg = v; },
                   _("Low = grazing light throws long micro-shadow"));
        }
        if (section("paper:edges", _("Edges & print"))) {
            slider(_("Deckle amount"), 0, 1, 0.01, "",
                   [paperC](const TextureParams& p) { return paperC(p).deckleAmount; },
                   [paperOf](TextureParams& p, double v) { paperOf(p).deckleAmount = v; });
            slider(_("Deckle inset"), 0.01, 0.2, 0.005, "",
                   [paperC](const TextureParams& p) { return paperC(p).deckleInset; },
                   [paperOf](TextureParams& p, double v) { paperOf(p).deckleInset = v; },
                   _("Fringe band width as a fraction of the sheet"));
            slider(_("Print amount"), 0, 1, 0.01, "",
                   [paperC](const TextureParams& p) { return paperC(p).printAmount; },
                   [paperOf](TextureParams& p, double v) { paperOf(p).printAmount = v; });
        }
    }
}

void TextureGeneratorDialog::buildGrassControls(ControlsCtx& c) {
    const auto slider = [&c](auto&&... a) { return c.slider(std::forward<decltype(a)>(a)...); };
    const auto colorRow = [&c](auto&&... a) { return c.colorRow(std::forward<decltype(a)>(a)...); };
    const auto section = [&c](auto&&... a) { return c.section(std::forward<decltype(a)>(a)...); };
    const auto twoChecks = [&c](auto&&... a) { c.twoChecks(std::forward<decltype(a)>(a)...); };

    const auto grassOf = [](TextureParams& p) -> GrassParams& {
        return std::get<GrassParams>(p.spec);
    };
    const auto grassC = [](const TextureParams& p) -> const GrassParams& {
        return std::get<GrassParams>(p.spec);
    };
    {
        twoChecks(_("Turf base"), [grassC](const TextureParams& p) { return grassC(p).enableTurf; },
                  [grassOf](TextureParams& p, bool v) { grassOf(p).enableTurf = v; },
                  _("Off = transparent ground, blade silhouettes only (§3.4)"), _("Blades"),
                  [grassC](const TextureParams& p) { return grassC(p).enableBlades; },
                  [grassOf](TextureParams& p, bool v) { grassOf(p).enableBlades = v; },
                  _("Off = the flat turf field alone"));
        colorRow(_("Blade root"), [grassC](const TextureParams& p) { return grassC(p).baseColor; },
                 [grassOf](TextureParams& p, common::ColorF c) { grassOf(p).baseColor = c; });
        colorRow(_("Blade tip"), [grassC](const TextureParams& p) { return grassC(p).tipColor; },
                 [grassOf](TextureParams& p, common::ColorF c) { grassOf(p).tipColor = c; });
        slider(_("Density"), 0, 1, 0.01, "",
               [grassC](const TextureParams& p) { return grassC(p).density; },
               [grassOf](TextureParams& p, double v) { grassOf(p).density = v; },
               _("Blade coverage: 0 bald, 1 dense sward"));
        slider(_("Blade height"), 0.2, 3, 0.01, "x",
               [grassC](const TextureParams& p) { return grassC(p).bladeHeight; },
               [grassOf](TextureParams& p, double v) { grassOf(p).bladeHeight = v; });
        slider(_("Blade width"), 0.2, 3, 0.01, "x",
               [grassC](const TextureParams& p) { return grassC(p).bladeWidth; },
               [grassOf](TextureParams& p, double v) { grassOf(p).bladeWidth = v; });
        slider(_("Droop"), 0, 1, 0.01, "",
               [grassC](const TextureParams& p) { return grassC(p).curvature; },
               [grassOf](TextureParams& p, double v) { grassOf(p).curvature = v; },
               _("Blade arc: 0 stiff upright, 1 drooping"));
        slider(_("Wind direction"), 0, 360, 1, "\xC2\xB0",
               [grassC](const TextureParams& p) { return grassC(p).windDirectionDeg; },
               [grassOf](TextureParams& p, double v) { grassOf(p).windDirectionDeg = v; },
               _("Where blades lean toward (the arrow drags it too)"));
        slider(_("Wind strength"), 0, 1, 0.01, "",
               [grassC](const TextureParams& p) { return grassC(p).windStrength; },
               [grassOf](TextureParams& p, double v) { grassOf(p).windStrength = v; },
               _("0 upright, 1 flattened"));
        slider(_("Growth scale"), 0.25, 4, 0.01, "x",
               [](const TextureParams& p) { return p.scale; },
               [](TextureParams& p, double v) { p.scale = v; },
               _("Blade and clump world size (§8.3)"));

        if (section("grass:camera", _("Camera"))) {
            slider(_("View width"), 20, 120, 0.5, "\xC2\xB0",
                   [grassC](const TextureParams& p) { return grassC(p).fovDeg; },
                   [grassOf](TextureParams& p, double v) { grassOf(p).fovDeg = v; },
                   _("Horizontal field of view (the bracket handles drag it too)"));
            slider(_("Horizon"), 2, 60, 0.25, "\xC2\xB0",
                   [grassC](const TextureParams& p) { return grassC(p).pitchDeg; },
                   [grassOf](TextureParams& p, double v) { grassOf(p).pitchDeg = v; },
                   _("Camera down-tilt: higher looks down at a fuller lawn"));
        }
        if (section("grass:variety", _("Ground & variety"))) {
            colorRow(_("Soil"), [grassC](const TextureParams& p) { return grassC(p).soilColor; },
                     [grassOf](TextureParams& p, common::ColorF c) { grassOf(p).soilColor = c; },
                     _("Earth showing between and under blades"));
            colorRow(_("Straw"), [grassC](const TextureParams& p) { return grassC(p).dryColor; },
                     [grassOf](TextureParams& p, common::ColorF c) { grassOf(p).dryColor = c; },
                     _("The dead-blade tint Dry blades mixes in"));
            slider(_("Dry blades"), 0, 1, 0.01, "",
                   [grassC](const TextureParams& p) { return grassC(p).dryAmount; },
                   [grassOf](TextureParams& p, double v) { grassOf(p).dryAmount = v; },
                   _("Fraction of straw blades"));
            slider(_("Patchiness"), 0, 1, 0.01, "",
                   [grassC](const TextureParams& p) { return grassC(p).patchiness; },
                   [grassOf](TextureParams& p, double v) { grassOf(p).patchiness = v; },
                   _("0 uniform lawn, 1 worn and patchy"));
            slider(_("Clump size"), 0.2, 4, 0.01, "x",
                   [grassC](const TextureParams& p) { return grassC(p).clumpScale; },
                   [grassOf](TextureParams& p, double v) { grassOf(p).clumpScale = v; });
        }
        if (section("grass:light", _("Light"))) {
            slider(_("Sun direction"), 0, 360, 1, "\xC2\xB0",
                   [grassC](const TextureParams& p) { return grassC(p).lightAzimuthDeg; },
                   [grassOf](TextureParams& p, double v) { grassOf(p).lightAzimuthDeg = v; },
                   _("The dome handle drags it too"));
            slider(_("Sun height"), 5, 85, 0.5, "\xC2\xB0",
                   [grassC](const TextureParams& p) { return grassC(p).lightElevationDeg; },
                   [grassOf](TextureParams& p, double v) { grassOf(p).lightElevationDeg = v; },
                   _("Low = raking sheen and backlight"));
        }
    }
}

// ---- the S55-g material builders. All SURFACE generators over the §5 engine: the raking-light
// dome (and, where the material has an axis, the grain ring) drag their light/angle fields via
// surfaceFields(); the stacks below are the slider view of the same recipes.

void TextureGeneratorDialog::buildWoodControls(ControlsCtx& c) {
    const auto slider = [&c](auto&&... a) { return c.slider(std::forward<decltype(a)>(a)...); };
    const auto colorRow = [&c](auto&&... a) { return c.colorRow(std::forward<decltype(a)>(a)...); };
    const auto section = [&c](auto&&... a) { return c.section(std::forward<decltype(a)>(a)...); };

    const auto of = [](TextureParams& p) -> WoodParams& { return std::get<WoodParams>(p.spec); };
    const auto cf = [](const TextureParams& p) -> const WoodParams& {
        return std::get<WoodParams>(p.spec);
    };
    {
        colorRow(_("Earlywood"), [cf](const TextureParams& p) { return cf(p).earlyColor; },
                 [of](TextureParams& p, common::ColorF v) { of(p).earlyColor = v; },
                 _("The pale band of each growth ring"));
        colorRow(_("Latewood"), [cf](const TextureParams& p) { return cf(p).lateColor; },
                 [of](TextureParams& p, common::ColorF v) { of(p).lateColor = v; },
                 _("The dark dense band"));
        slider(_("Ring spacing"), 4, 80, 0.5, "px",
               [cf](const TextureParams& p) { return cf(p).ringSpacing; },
               [of](TextureParams& p, double v) { of(p).ringSpacing = v; },
               _("Growth-ring pitch at Scale 1"));
        slider(_("Ring contrast"), 0, 1, 0.01, "",
               [cf](const TextureParams& p) { return cf(p).ringContrast; },
               [of](TextureParams& p, double v) { of(p).ringContrast = v; });
        slider(_("Waviness"), 0, 1, 0.01, "",
               [cf](const TextureParams& p) { return cf(p).waviness; },
               [of](TextureParams& p, double v) { of(p).waviness = v; },
               _("How far the rings meander"));
        slider(_("Knots"), 0, 1, 0.01, "",
               [cf](const TextureParams& p) { return cf(p).knots; },
               [of](TextureParams& p, double v) { of(p).knots = v; },
               _("0 = clear lumber"));
        slider(_("Fibre"), 0, 1, 0.01, "",
               [cf](const TextureParams& p) { return cf(p).fiber; },
               [of](TextureParams& p, double v) { of(p).fiber = v; },
               _("Streaks running along the grain"));
        slider(_("Grain angle"), 0, 180, 0.5, "\xC2\xB0",
               [cf](const TextureParams& p) { return cf(p).grainAngleDeg; },
               [of](TextureParams& p, double v) { of(p).grainAngleDeg = v; },
               _("The grain axis (the ring handle drags it too)"));
        slider(_("Grain scale"), 0.25, 8, 0.01, "x",
               [](const TextureParams& p) { return p.scale; },
               [](TextureParams& p, double v) { p.scale = v; },
               _("Feature size per document pixel (§8.3)"));
        if (section("wood:surface", _("Surface & light"))) {
            slider(_("Roughness"), 0, 1, 0.01, "",
                   [cf](const TextureParams& p) { return cf(p).roughness; },
                   [of](TextureParams& p, double v) { of(p).roughness = v; },
                   _("Open-grain relief height"));
            slider(_("Matte"), 0, 1, 0.01, "",
                   [cf](const TextureParams& p) { return cf(p).matte; },
                   [of](TextureParams& p, double v) { of(p).matte = v; });
            slider(_("Sheen"), 0, 1, 0.01, "",
                   [cf](const TextureParams& p) { return cf(p).sheen; },
                   [of](TextureParams& p, double v) { of(p).sheen = v; },
                   _("Satin finish (0 = raw wood)"));
            slider(_("Light direction"), 0, 360, 1, "\xC2\xB0",
                   [cf](const TextureParams& p) { return cf(p).lightAzimuthDeg; },
                   [of](TextureParams& p, double v) { of(p).lightAzimuthDeg = v; },
                   _("The dome handle drags it too"));
            slider(_("Light height"), 5, 85, 0.5, "\xC2\xB0",
                   [cf](const TextureParams& p) { return cf(p).lightElevationDeg; },
                   [of](TextureParams& p, double v) { of(p).lightElevationDeg = v; },
                   _("Low = grazing light throws long micro-shadow"));
        }
    }
}

void TextureGeneratorDialog::buildMarbleControls(ControlsCtx& c) {
    const auto slider = [&c](auto&&... a) { return c.slider(std::forward<decltype(a)>(a)...); };
    const auto colorRow = [&c](auto&&... a) { return c.colorRow(std::forward<decltype(a)>(a)...); };
    const auto section = [&c](auto&&... a) { return c.section(std::forward<decltype(a)>(a)...); };

    const auto of = [](TextureParams& p) -> MarbleParams& {
        return std::get<MarbleParams>(p.spec);
    };
    const auto cf = [](const TextureParams& p) -> const MarbleParams& {
        return std::get<MarbleParams>(p.spec);
    };
    {
        colorRow(_("Stone"), [cf](const TextureParams& p) { return cf(p).baseColor; },
                 [of](TextureParams& p, common::ColorF v) { of(p).baseColor = v; },
                 _("The polished ground"));
        colorRow(_("Veins"), [cf](const TextureParams& p) { return cf(p).veinColor; },
                 [of](TextureParams& p, common::ColorF v) { of(p).veinColor = v; });
        slider(_("Vein spacing"), 8, 200, 1, "px",
               [cf](const TextureParams& p) { return cf(p).veinSpacing; },
               [of](TextureParams& p, double v) { of(p).veinSpacing = v; },
               _("Primary vein pitch at Scale 1"));
        slider(_("Turbulence"), 0, 1, 0.01, "",
               [cf](const TextureParams& p) { return cf(p).turbulence; },
               [of](TextureParams& p, double v) { of(p).turbulence = v; },
               _("How wildly the veins meander"));
        slider(_("Contrast"), 0, 1, 0.01, "",
               [cf](const TextureParams& p) { return cf(p).contrast; },
               [of](TextureParams& p, double v) { of(p).contrast = v; },
               _("Vein strength against the ground"));
        slider(_("Vein angle"), 0, 180, 0.5, "\xC2\xB0",
               [cf](const TextureParams& p) { return cf(p).veinAngleDeg; },
               [of](TextureParams& p, double v) { of(p).veinAngleDeg = v; },
               _("The primary fracture direction (the ring handle drags it too)"));
        slider(_("Vein scale"), 0.25, 8, 0.01, "x",
               [](const TextureParams& p) { return p.scale; },
               [](TextureParams& p, double v) { p.scale = v; },
               _("Feature size per document pixel (§8.3)"));
        if (section("marble:surface", _("Surface & light"))) {
            slider(_("Roughness"), 0, 1, 0.01, "",
                   [cf](const TextureParams& p) { return cf(p).roughness; },
                   [of](TextureParams& p, double v) { of(p).roughness = v; },
                   _("Polished marble carries only a hair of relief"));
            slider(_("Matte"), 0, 1, 0.01, "",
                   [cf](const TextureParams& p) { return cf(p).matte; },
                   [of](TextureParams& p, double v) { of(p).matte = v; });
            slider(_("Polish"), 0, 1, 0.01, "",
                   [cf](const TextureParams& p) { return cf(p).sheen; },
                   [of](TextureParams& p, double v) { of(p).sheen = v; },
                   _("The tight specular sheen of a honed surface"));
            slider(_("Light direction"), 0, 360, 1, "\xC2\xB0",
                   [cf](const TextureParams& p) { return cf(p).lightAzimuthDeg; },
                   [of](TextureParams& p, double v) { of(p).lightAzimuthDeg = v; },
                   _("The dome handle drags it too"));
            slider(_("Light height"), 5, 85, 0.5, "\xC2\xB0",
                   [cf](const TextureParams& p) { return cf(p).lightElevationDeg; },
                   [of](TextureParams& p, double v) { of(p).lightElevationDeg = v; });
        }
    }
}

void TextureGeneratorDialog::buildStoneControls(ControlsCtx& c) {
    const auto slider = [&c](auto&&... a) { return c.slider(std::forward<decltype(a)>(a)...); };
    const auto colorRow = [&c](auto&&... a) { return c.colorRow(std::forward<decltype(a)>(a)...); };
    const auto section = [&c](auto&&... a) { return c.section(std::forward<decltype(a)>(a)...); };

    const auto of = [](TextureParams& p) -> StoneParams& { return std::get<StoneParams>(p.spec); };
    const auto cf = [](const TextureParams& p) -> const StoneParams& {
        return std::get<StoneParams>(p.spec);
    };
    {
        colorRow(_("Stone"), [cf](const TextureParams& p) { return cf(p).baseColor; },
                 [of](TextureParams& p, common::ColorF v) { of(p).baseColor = v; });
        slider(_("Cell size"), 4, 200, 1, "px",
               [cf](const TextureParams& p) { return cf(p).cellSize; },
               [of](TextureParams& p, double v) { of(p).cellSize = v; },
               _("Aggregate grain at Scale 1: small = granite, large = cobbles"));
        slider(_("Crack depth"), 0, 1, 0.01, "",
               [cf](const TextureParams& p) { return cf(p).crackDepth; },
               [of](TextureParams& p, double v) { of(p).crackDepth = v; },
               _("The joint network between cells"));
        slider(_("Relief"), 0, 1, 0.01, "",
               [cf](const TextureParams& p) { return cf(p).roughness; },
               [of](TextureParams& p, double v) { of(p).roughness = v; },
               _("Per-cell surface bumpiness"));
        slider(_("Variation"), 0, 1, 0.01, "",
               [cf](const TextureParams& p) { return cf(p).variation; },
               [of](TextureParams& p, double v) { of(p).variation = v; },
               _("Per-cell tone differences"));
        slider(_("Stone scale"), 0.25, 8, 0.01, "x",
               [](const TextureParams& p) { return p.scale; },
               [](TextureParams& p, double v) { p.scale = v; },
               _("Feature size per document pixel (§8.3)"));
        if (section("stone:surface", _("Surface & light"))) {
            slider(_("Matte"), 0, 1, 0.01, "",
                   [cf](const TextureParams& p) { return cf(p).matte; },
                   [of](TextureParams& p, double v) { of(p).matte = v; });
            slider(_("Sheen"), 0, 1, 0.01, "",
                   [cf](const TextureParams& p) { return cf(p).sheen; },
                   [of](TextureParams& p, double v) { of(p).sheen = v; },
                   _("Wet or sealed stone (0 = dry)"));
            slider(_("Light direction"), 0, 360, 1, "\xC2\xB0",
                   [cf](const TextureParams& p) { return cf(p).lightAzimuthDeg; },
                   [of](TextureParams& p, double v) { of(p).lightAzimuthDeg = v; },
                   _("The dome handle drags it too"));
            slider(_("Light height"), 5, 85, 0.5, "\xC2\xB0",
                   [cf](const TextureParams& p) { return cf(p).lightElevationDeg; },
                   [of](TextureParams& p, double v) { of(p).lightElevationDeg = v; },
                   _("Low = grazing light digs out the joints"));
        }
    }
}

void TextureGeneratorDialog::buildCanvasControls(ControlsCtx& c) {
    const auto slider = [&c](auto&&... a) { return c.slider(std::forward<decltype(a)>(a)...); };
    const auto colorRow = [&c](auto&&... a) { return c.colorRow(std::forward<decltype(a)>(a)...); };
    const auto section = [&c](auto&&... a) { return c.section(std::forward<decltype(a)>(a)...); };

    const auto of = [](TextureParams& p) -> CanvasParams& {
        return std::get<CanvasParams>(p.spec);
    };
    const auto cf = [](const TextureParams& p) -> const CanvasParams& {
        return std::get<CanvasParams>(p.spec);
    };
    {
        colorRow(_("Tint"), [cf](const TextureParams& p) { return cf(p).tint; },
                 [of](TextureParams& p, common::ColorF v) { of(p).tint = v; });
        slider(_("Thread pitch"), 2, 24, 0.1, "px",
               [cf](const TextureParams& p) { return cf(p).threadPitch; },
               [of](TextureParams& p, double v) { of(p).threadPitch = v; },
               _("Thread spacing at Scale 1"));
        slider(_("Irregularity"), 0, 1, 0.01, "",
               [cf](const TextureParams& p) { return cf(p).irregularity; },
               [of](TextureParams& p, double v) { of(p).irregularity = v; },
               _("Thread wobble and thickness jitter"));
        slider(_("Weave depth"), 0, 1, 0.01, "",
               [cf](const TextureParams& p) { return cf(p).weaveDepth; },
               [of](TextureParams& p, double v) { of(p).weaveDepth = v; },
               _("Over/under relief prominence"));
        slider(_("Weave angle"), 0, 180, 0.5, "\xC2\xB0",
               [cf](const TextureParams& p) { return cf(p).weaveAngleDeg; },
               [of](TextureParams& p, double v) { of(p).weaveAngleDeg = v; },
               _("Warp direction (the ring handle drags it too)"));
        slider(_("Fuzz"), 0, 1, 0.01, "",
               [cf](const TextureParams& p) { return cf(p).fuzz; },
               [of](TextureParams& p, double v) { of(p).fuzz = v; },
               _("Stray-fibre micro relief"));
        slider(_("Weave scale"), 0.25, 8, 0.01, "x",
               [](const TextureParams& p) { return p.scale; },
               [](TextureParams& p, double v) { p.scale = v; },
               _("Feature size per document pixel (§8.3)"));
        if (section("canvas:surface", _("Surface & light"))) {
            slider(_("Matte"), 0, 1, 0.01, "",
                   [cf](const TextureParams& p) { return cf(p).matte; },
                   [of](TextureParams& p, double v) { of(p).matte = v; });
            slider(_("Sheen"), 0, 1, 0.01, "",
                   [cf](const TextureParams& p) { return cf(p).sheen; },
                   [of](TextureParams& p, double v) { of(p).sheen = v; },
                   _("A primed/sized surface (0 = raw cloth)"));
            slider(_("Light direction"), 0, 360, 1, "\xC2\xB0",
                   [cf](const TextureParams& p) { return cf(p).lightAzimuthDeg; },
                   [of](TextureParams& p, double v) { of(p).lightAzimuthDeg = v; },
                   _("The dome handle drags it too"));
            slider(_("Light height"), 5, 85, 0.5, "\xC2\xB0",
                   [cf](const TextureParams& p) { return cf(p).lightElevationDeg; },
                   [of](TextureParams& p, double v) { of(p).lightElevationDeg = v; },
                   _("Low = grazing light raises the weave"));
        }
    }
}

void TextureGeneratorDialog::buildMetalControls(ControlsCtx& c) {
    const auto slider = [&c](auto&&... a) { return c.slider(std::forward<decltype(a)>(a)...); };
    const auto colorRow = [&c](auto&&... a) { return c.colorRow(std::forward<decltype(a)>(a)...); };
    const auto section = [&c](auto&&... a) { return c.section(std::forward<decltype(a)>(a)...); };

    const auto of = [](TextureParams& p) -> MetalParams& { return std::get<MetalParams>(p.spec); };
    const auto cf = [](const TextureParams& p) -> const MetalParams& {
        return std::get<MetalParams>(p.spec);
    };
    {
        colorRow(_("Tint"), [cf](const TextureParams& p) { return cf(p).tint; },
                 [of](TextureParams& p, common::ColorF v) { of(p).tint = v; },
                 _("The alloy: steel grey, brass yellow..."));
        slider(_("Brush angle"), 0, 180, 0.5, "\xC2\xB0",
               [cf](const TextureParams& p) { return cf(p).brushAngleDeg; },
               [of](TextureParams& p, double v) { of(p).brushAngleDeg = v; },
               _("Streaks run along it (the ring handle drags it too)"));
        slider(_("Roughness"), 0, 1, 0.01, "",
               [cf](const TextureParams& p) { return cf(p).roughness; },
               [of](TextureParams& p, double v) { of(p).roughness = v; },
               _("Brushed streak depth"));
        slider(_("Sheen"), 0, 1, 0.01, "",
               [cf](const TextureParams& p) { return cf(p).sheen; },
               [of](TextureParams& p, double v) { of(p).sheen = v; },
               _("The specular band across the brushing"));
        slider(_("Reflection"), 0, 1, 0.01, "",
               [cf](const TextureParams& p) { return cf(p).gradient; },
               [of](TextureParams& p, double v) { of(p).gradient = v; },
               _("A vertical sky-to-ground tone ramp"));
        slider(_("Brush scale"), 0.25, 8, 0.01, "x",
               [](const TextureParams& p) { return p.scale; },
               [](TextureParams& p, double v) { p.scale = v; },
               _("Feature size per document pixel (§8.3)"));
        if (section("metal:surface", _("Surface & light"))) {
            slider(_("Matte"), 0, 1, 0.01, "",
                   [cf](const TextureParams& p) { return cf(p).matte; },
                   [of](TextureParams& p, double v) { of(p).matte = v; });
            slider(_("Light direction"), 0, 360, 1, "\xC2\xB0",
                   [cf](const TextureParams& p) { return cf(p).lightAzimuthDeg; },
                   [of](TextureParams& p, double v) { of(p).lightAzimuthDeg = v; },
                   _("The dome handle drags it too"));
            slider(_("Light height"), 5, 85, 0.5, "\xC2\xB0",
                   [cf](const TextureParams& p) { return cf(p).lightElevationDeg; },
                   [of](TextureParams& p, double v) { of(p).lightElevationDeg = v; });
        }
    }
}

void TextureGeneratorDialog::syncControls() {
    m_seeding = true;
    for (const auto& fn : m_ui->syncFns) fn();
    m_ui->preset->value(texturePresetIndex(activeParams()));
    char b[24];
    std::snprintf(b, sizeof(b), "%llu",
                  static_cast<unsigned long long>(activeParams().seed));
    m_ui->seed->value(b);
    m_seeding = false;
}

void TextureGeneratorDialog::applyEdit(const std::function<void(TextureParams&)>& mutate) {
    if (m_seeding || m_baking) return;
    mutate(activeParams());
    syncControls();
    requestProxy();
}

void TextureGeneratorDialog::applyPreset(std::size_t i) {
    const texture::GeneratorTraits& traits = texture::generatorTraits(m_generator);
    applyEdit([&traits, i](TextureParams& p) {
        if (i < traits.presetCount()) traits.applyPreset(p, i);
    });
    // Some presets change the stack's SHAPE (sky: deck rows appear/vanish) -- rebuild for those.
    if (generatorUi(m_generator).presetAffectsLayout) {
        rebuildControls();
        syncControls();
    }
}

void TextureGeneratorDialog::randomizeSeed() {
    // Nine digits reads (and re-types) comfortably in the field; any full uint64 typed by hand
    // is still honoured -- this only shapes what the dice roll.
    applyEdit([](TextureParams& p) { p.seed = randomSeed() % 999999999ULL + 1; });
}

void TextureGeneratorDialog::applySolar() {
    // The classic §4.2 calculator: time & place -> the sun's az/el (only). The moon-phase readout
    // and the info panel are refreshed separately (updateSkyInfo).
    const texture::UtcTime t{m_solYear, m_solMonth, m_solDay, m_solHour};
    const texture::SunPosition sun = texture::sunPosition(t, m_solLat, m_solLon);
    applyEdit([&](TextureParams& p) {
        if (auto* s = std::get_if<SkyParams>(&p.spec)) {
            s->sunAzimuthDeg = sun.azimuthDeg;
            s->sunElevationDeg = std::clamp(sun.elevationDeg, -30.0, 90.0);
        }
    });
}

void TextureGeneratorDialog::setObserver(int year, int month, int day, double hourUtc,
                                         double latDeg, double lonDeg) {
    m_solYear = std::clamp(year, 1900, 2200);
    m_solMonth = std::clamp(month, 1, 12);
    m_solDay = std::clamp(day, 1, 31);
    m_solHour = std::clamp(hourUtc, 0.0, 24.0);
    m_solLat = std::clamp(latDeg, -90.0, 90.0);
    m_solLon = std::clamp(lonDeg, -180.0, 180.0);
    observerChanged(true);
}

void TextureGeneratorDialog::setObserverLatLon(double latDeg, double lonDeg) {
    m_solLat = std::clamp(latDeg, -90.0, 90.0);
    m_solLon = std::clamp(lonDeg, -180.0, 180.0);
    observerChanged(true);
}

void TextureGeneratorDialog::observerChanged(bool fromDatePlaceControl) {
    // First-set-wins latch: touching the date/place before any manual phase makes "date & place"
    // the moon-phase source for this session (an explicit pick in Night & moon still overrides).
    if (fromDatePlaceControl && m_moonSource == 0) m_moonSource = 1;
    if (m_moonSource == 1) {
        // Ephemeris: the whole sky (sun + moon + phase + observer clock) follows the clock.
        const texture::UtcTime t{m_solYear, m_solMonth, m_solDay, m_solHour};
        applyEdit([&](TextureParams& p) {
            if (auto* s = std::get_if<SkyParams>(&p.spec))
                texture::applyMasterClock(*s, t, m_solLat, m_solLon);
        });
    } else {
        applySolar(); // just the sun
    }
    if (m_ui->place != nullptr) m_ui->place->showNearest(m_solLat, m_solLon);
    // Keep the open flyout's pin honest when the coordinates move from elsewhere (typed lat/lon,
    // a picked date). During a pin drag this writes back the values the pin just sent: harmless.
    if (m_ui->mapFlyout != nullptr && m_ui->mapFlyout->shown())
        m_ui->mapFlyout->setPlace(m_solLat, m_solLon);
    updateSkyInfo();
}

void TextureGeneratorDialog::selectMoonSource(int source) {
    setMoonSourceInternal(source == 2 ? 2 : 1, /*applyClock=*/true); // explicit user pick
}

void TextureGeneratorDialog::openSectionForTest(const std::string& key) {
    m_open[key] = true;
    rebuildControls();
    syncControls();
    updateSkyInfo();
}

void TextureGeneratorDialog::setMoonSourceInternal(int source, bool applyClock) {
    m_moonSource = source;
    if (source == 1 && applyClock) {
        const texture::UtcTime t{m_solYear, m_solMonth, m_solDay, m_solHour};
        applyEdit([&](TextureParams& p) {
            if (auto* s = std::get_if<SkyParams>(&p.spec))
                texture::applyMasterClock(*s, t, m_solLat, m_solLon); // sets mode 2 + enables moon
        });
    } else {
        applyEdit([&](TextureParams& p) {
            if (auto* s = std::get_if<SkyParams>(&p.spec)) {
                s->enableMoon = true; // engaging a phase source implies wanting the moon
                s->moonPhaseMode = source == 1 ? 2 : 1;
            }
        });
    }
    rebuildControls(); // the manual illumination slider appears/disappears with the source
    syncControls();
    updateSkyInfo();
}

std::string TextureGeneratorDialog::moonPhaseReadout() const {
    char b[96];
    if (m_moonSource == 2) {
        const auto* s = std::get_if<SkyParams>(&activeParams().spec);
        const double f = s != nullptr ? s->moonIlluminatedFraction : 1.0;
        const char* name = f < 0.03  ? _("New moon")
                           : f < 0.47 ? _("Crescent")
                           : f < 0.53 ? _("Half moon")
                           : f < 0.97 ? _("Gibbous")
                                      : _("Full moon");
        std::snprintf(b, sizeof(b), _("Phase: %s (%.0f%% lit)"), name, f * 100.0);
    } else {
        const texture::UtcTime t{m_solYear, m_solMonth, m_solDay, m_solHour};
        const texture::SkyAlmanac a = texture::computeSkyAlmanac(t, m_solLat, m_solLon);
        std::snprintf(b, sizeof(b), _("Phase: %s (%.0f%% lit)"),
                      texture::moonPhaseNameText(a.moonPhaseName),
                      a.moonIlluminatedFraction * 100.0);
    }
    return b;
}

void TextureGeneratorDialog::updateSkyInfo() {
    if (m_ui->info == nullptr) return;
    if (m_generator != texture::Generator::Sky) {
        m_skyClock = SkyClockState{}; // visible = false: no clock without the sky panel
        m_ui->info->hide();
        return;
    }
    m_ui->info->show();
    const texture::UtcTime t{m_solYear, m_solMonth, m_solDay, m_solHour};
    const texture::SkyAlmanac a = texture::computeSkyAlmanac(t, m_solLat, m_solLon);

    // The clock: observer-local (mean solar) wall time = UTC + longitude/15 -- the almanac's own
    // utcHourToLocal convention -- with the calendar date stepped when the offset crosses
    // midnight. |offset| <= 12h, so one day step always suffices.
    {
        SkyClockState c;
        c.visible = true;
        c.year = m_solYear;
        c.month = m_solMonth;
        c.day = m_solDay;
        c.localHours = m_solHour + m_solLon / 15.0;
        if (c.localHours >= 24.0) {
            c.localHours -= 24.0;
            if (++c.day > date_detail::daysInMonth(c.year, c.month)) {
                c.day = 1;
                if (++c.month > 12) {
                    c.month = 1;
                    ++c.year;
                }
            }
        } else if (c.localHours < 0.0) {
            c.localHours += 24.0;
            if (--c.day < 1) {
                if (--c.month < 1) {
                    c.month = 12;
                    --c.year;
                }
                c.day = date_detail::daysInMonth(c.year, c.month);
            }
        }
        c.sunElevationDeg = a.sunElevationDeg;
        m_skyClock = c;
        m_ui->info->setClock(c);
    }
    std::vector<std::pair<std::string, std::string>> rows;
    rows.emplace_back(_("Sky"), a.skyStateText);
    {
        char b[64];
        if (a.sunElevationDeg >= -0.833)
            std::snprintf(b, sizeof(b), _("up  (%.0f\xC2\xB0 above)"), a.sunElevationDeg);
        else
            std::snprintf(b, sizeof(b), _("down  (%.0f\xC2\xB0 below)"), -a.sunElevationDeg);
        rows.emplace_back(_("Sun"), b);
    }
    {
        char b[112];
        std::snprintf(b, sizeof(b), _("%s \xC2\xB7 %.0f%% lit \xC2\xB7 %s"),
                      texture::moonPhaseNameText(a.moonPhaseName), a.moonIlluminatedFraction * 100.0,
                      a.moonElevationDeg > 0.0 ? _("up") : _("down"));
        rows.emplace_back(_("Moon"), b);
    }
    {
        char b[112];
        const texture::CityEntry& c = texture::cityAt(a.nearestCityIndex);
        std::snprintf(b, sizeof(b), "%s, %s \xC2\xB7 %.0f km", c.name, c.country,
                      a.nearestCityDistanceKm);
        rows.emplace_back(_("Nearest city"), b);
    }
    m_ui->info->setTitle(_("Sky at this date & place"));
    m_ui->info->setRows(std::move(rows));

    if (m_ui->solarReadout != nullptr) {
        char b[96];
        if (a.sunElevationDeg < -0.8)
            std::snprintf(b, sizeof(b), _("Night there: sun %.1f\xC2\xB0 below the horizon"),
                          -a.sunElevationDeg);
        else
            std::snprintf(b, sizeof(b), _("Sun: azimuth %.1f\xC2\xB0, elevation %.1f\xC2\xB0"),
                          a.sunAzimuthDeg, a.sunElevationDeg);
        m_ui->solarReadout->copy_label(b);
        m_ui->solarReadout->redraw();
    }
    if (m_ui->moonReadout != nullptr) {
        m_ui->moonReadout->copy_label(moonPhaseReadout().c_str());
        m_ui->moonReadout->redraw();
    }
}

// ------------------------------------------------------------------------------------------------
// Proxy renders + the bake (the worker/poll loop)
// ------------------------------------------------------------------------------------------------
TextureGeneratorDialog::ViewSpec TextureGeneratorDialog::viewSpec() const {
    ViewSpec v;
    if (m_docW == 0 || m_docH == 0 || m_ui->preview == nullptr) return v;
    const auto paneW = static_cast<std::uint32_t>(std::max(1, m_ui->preview->innerW()));
    const auto paneH = static_cast<std::uint32_t>(std::max(1, m_ui->preview->innerH()));
    // The proxy frame IS the document scaled by the continuous zoom (§8.2 -- same params, a
    // resolution to suit the view; frame-relative cameras keep it faithful), windowed to the
    // visible pan region. The one framing truth the proxy request and the gizmo mapping share.
    const double zoom = m_ui->preview->effectiveZoom(paneW, paneH);
    const texgizmo::PreviewView pv = texgizmo::previewView(m_docW, m_docH, paneW, paneH, zoom,
                                                           m_ui->preview->panX(),
                                                           m_ui->preview->panY());
    v.frameW = pv.frameW;
    v.frameH = pv.frameH;
    v.winX = pv.winX;
    v.winY = pv.winY;
    v.viewW = pv.viewW;
    v.viewH = pv.viewH;
    return v;
}

void TextureGeneratorDialog::requestProxy() {
    if (m_baking || m_ui->preview == nullptr) return;
    const ViewSpec vs = viewSpec();
    if (vs.frameW == 0 || vs.frameH == 0) return;
    texture::TextureRenderWorker::Job job;
    job.epoch = ++m_epoch;
    job.params = activeParams();
    job.frameW = vs.frameW;
    job.frameH = vs.frameH;
    // Paper/material features are pixel-sized (laid pitch in px at Scale 1), so the proxy scales
    // them with the frame's zoom to preview the DOCUMENT framing; sky/grass cameras are frame-
    // relative and need nothing (§8.3). At zoom 1 the multiplier is 1 (a byte-exact document
    // preview). The registry row says which semantics a generator's Scale carries.
    if (texture::generatorTraits(job.params.generator).pixelScaledFeatures)
        job.params.scale *= static_cast<double>(vs.frameW) / m_docW;
    // Only the visible window is evaluated whenever the frame is larger than the pane -- so a
    // zoomed-in preview costs the visible pixels, not the whole magnified frame.
    if (vs.viewW < vs.frameW || vs.viewH < vs.frameH)
        job.window = texture::TextureWindow{vs.winX, vs.winY, vs.viewW, vs.viewH};
    m_worker->request(std::move(job));
    m_ui->preview->setPending(true);
    // Reflect the live zoom in the readout under the pane.
    if (m_ui->zoomLabel != nullptr) {
        const double zoom = static_cast<double>(vs.frameW) / std::max(1u, m_docW);
        char b[48];
        std::snprintf(b, sizeof(b), _("Zoom %d%%  (scroll / drag)"),
                      static_cast<int>(std::lround(zoom * 100.0)));
        m_ui->zoomLabel->copy_label(b);
        m_ui->zoomLabel->redraw();
    }
    if (!m_polling) {
        m_polling = true;
        Fl::add_timeout(1.0 / 30.0, pollTimer, this);
    }
}

void TextureGeneratorDialog::pollTimer(void* self) {
    auto* d = static_cast<TextureGeneratorDialog*>(self);
    d->m_polling = false;
    d->pollOnce();
}

void TextureGeneratorDialog::pollOnce() {
    // The ACCEPT-time conform stages (mask 70-90%, harmonize 90-100%) own the poll while they
    // run: the bake is done, its result is stashed in the run, and nothing else may commit.
    if (m_conform != nullptr) {
        if (m_conform->done.load(std::memory_order_acquire)) {
            finishConform();
            return;
        }
        const double f =
            m_conform->progress->permille.load(std::memory_order_relaxed) / 1000.0;
        m_ui->progress->setFraction(0.7 + 0.2 * f);
        m_polling = true;
        Fl::add_timeout(1.0 / 30.0, pollTimer, this);
        return;
    }
    if (m_estimating && m_estimator != nullptr) {
        if (auto r = m_estimator->takeResult()) {
            if (r->epoch == m_estimateEpoch) {
                finishEstimate(std::move(r->estimate));
                return;
            }
        }
        m_ui->progress->setFraction(std::max(0.02, m_estimator->progressFraction()));
    }
    while (auto r = m_worker->takeResult()) {
        if (m_baking && r->epoch == m_bakeEpoch) {
            m_baking = false;
            m_bakeEpoch = 0;
            if (m_conformWanted && conformOffered()) {
                // The toggle path: keep the fence up and continue S6-S8 on the same progress
                // scale; the commit happens when the conform thread lands.
                startConform(activeParams(), std::move(r->render));
                return;
            }
            // The full-res bake landed: hand off and close. `commit` installs the cache, so
            // nothing re-renders synchronously.
            TextureGenHost host = m_host; // hide() may destroy us via the app; stay safe
            const TextureParams committed = activeParams();
            hide();
            if (host.commit) host.commit(committed, std::move(r->render));
            return;
        }
        if (!m_baking && r->epoch == m_epoch) {
            common::Image img;
            if (r->render.imageF)
                img = common::toImage8(*r->render.imageF);
            else if (r->render.image8)
                img = std::move(*r->render.image8);
            m_ui->preview->setProxy(std::move(img));
        }
        // Anything else is stale -- superseded before it was taken.
    }
    if (m_baking && m_ui->progress != nullptr) {
        const double f = m_worker->progressFraction();
        if (f <= 0.0) {
            m_ui->progress->setIndeterminate();
        } else {
            // When the mask & harmonize stages will follow, the bake owns 0-70% of the bar.
            m_ui->progress->setFraction(m_conformWanted && conformOffered() ? 0.7 * f : f);
        }
    }
    if (m_worker->busy() || m_baking || m_estimating ||
        (m_estimator != nullptr && m_estimator->busy())) {
        if (!m_polling) {
            m_polling = true;
            Fl::add_timeout(1.0 / 30.0, pollTimer, this);
        }
    } else {
        m_ui->preview->setPending(false);
    }
}

// Fence the edit surface while background work owns the params -- the bake, the estimate and
// the ACCEPT-time conform stages all share this (one fence: the two workers can never race for
// the dialog's state). The hover bubble closes with it; the footer toggle yields its slot to
// the progress bar.
void TextureGeneratorDialog::fenceForWork(const char* note) {
    if (m_ui->scroll != nullptr) m_ui->scroll->deactivate();
    for (RailItem* r : m_ui->rail) r->deactivate();
    m_ui->preset->deactivate();
    m_ui->seed->deactivate();
    m_ui->dice->deactivate();
    m_ui->resetView->deactivate();
    m_ui->create->deactivate();
    if (m_ui->conform != nullptr) m_ui->conform->hide();
    if (m_ui->estimateBubble != nullptr) m_ui->estimateBubble->hide();
    m_ui->note->copy_label(note);
    m_ui->note->redraw();
    m_ui->progress->setIndeterminate();
    m_ui->progress->show();
}

void TextureGeneratorDialog::unfenceAfterWork() {
    if (m_ui->scroll != nullptr) m_ui->scroll->activate();
    for (RailItem* r : m_ui->rail) r->activate();
    m_ui->preset->activate();
    m_ui->seed->activate();
    m_ui->dice->activate();
    m_ui->resetView->activate();
    m_ui->create->activate();
    m_ui->progress->hide();
    updateEstimateUi(); // the toggle returns to its slot when still offered
}

void TextureGeneratorDialog::create() {
    if (m_baking || m_estimating || m_conform != nullptr || m_docW == 0 || m_docH == 0) return;
    m_baking = true;
    texture::TextureRenderWorker::Job job;
    m_bakeEpoch = ++m_epoch;
    job.epoch = m_bakeEpoch;
    job.params = activeParams();
    job.frameW = m_docW;
    job.frameH = m_docH;
    m_worker->request(std::move(job));
    // Fence the dialog: the params are committed to the bake now.
    fenceForWork(_("Rendering at full resolution\xE2\x80\xA6"));
    if (!m_polling) {
        m_polling = true;
        Fl::add_timeout(1.0 / 30.0, pollTimer, this);
    }
}

void TextureGeneratorDialog::cancelBake() {
    if (!m_baking) return;
    m_worker->cancelAll();
    m_baking = false;
    m_bakeEpoch = 0;
    unfenceAfterWork();
    selectGenerator(m_generator); // restores the note + re-warms the proxy
}

void TextureGeneratorDialog::doCancel() {
    if (m_estimating) {
        cancelEstimate(); // first Cancel stops the estimate; a second closes
        return;
    }
    if (m_conform != nullptr) {
        cancelConform(); // stops the mask/harmonize stages; the dialog stays open
        return;
    }
    if (m_baking) {
        cancelBake(); // first Cancel stops the bake; a second closes
        return;
    }
    m_worker->cancelAll();
    hide();
}

void TextureGeneratorDialog::openColorFlyout(const Fl_Widget* anchor, common::Color8 current,
                                             std::function<void(common::Color8)> onPick) {
    if (m_ui->colorFlyout == nullptr) return;
    if (m_ui->colorFlyout->shownForAnchor(anchor)) { // a re-click toggles it shut
        m_ui->colorFlyout->hide();
        m_onColorPick = nullptr;
        return;
    }
    m_onColorPick = std::move(onPick);
    // Keep clear of the preview pane (the flyout would cover the very pixels being tinted).
    m_ui->colorFlyout->setAvoidRect(m_ui->preview->x(), m_ui->preview->y(), m_ui->preview->w(),
                                    m_ui->preview->h());
    m_ui->colorFlyout->openFor(anchor, current);
}

void TextureGeneratorDialog::openMapFlyout(const Fl_Widget* anchor) {
    if (m_ui->mapFlyout == nullptr) return;
    if (m_ui->mapFlyout->shownForAnchor(anchor)) { // a re-click toggles it shut
        m_ui->mapFlyout->hide();
        return;
    }
    // Keep clear of the preview pane (the sun moves live as the pin drags -- keep it watchable).
    m_ui->mapFlyout->setAvoidRect(m_ui->preview->x(), m_ui->preview->y(), m_ui->preview->w(),
                                  m_ui->preview->h());
    m_ui->mapFlyout->openFor(anchor, m_solLat, m_solLon);
}

MapFlyout* TextureGeneratorDialog::mapFlyoutForTest() const {
    return m_ui->mapFlyout;
}

// ------------------------------------------------------------------------------------------------
// "Estimate from layer" (S55 phase 2; docs/research-sky-estimate-from-layer.md §7)
// ------------------------------------------------------------------------------------------------
void TextureGeneratorDialog::estimateFromLayer() {
    if (m_baking || m_estimating || m_conform != nullptr) return;
    if (m_generator != texture::Generator::Sky || !m_host.sourceLayer) return;
    std::optional<TextureGenHost::SourceLayer> src = m_host.sourceLayer();
    if (!src || src->docImage.empty()) return;
    auto* sky = std::get_if<SkyParams>(&activeParams().spec);
    if (sky == nullptr) return;

    m_sourceImage = std::move(src->docImage); // S6 must analyze EXACTLY these pixels at ACCEPT
    m_sourceName = src->name;

    // The snapshot behind Revert: the pre-estimate params with the pre-estimate DIALOG observer
    // stamped on (captured BEFORE any metadata prefill below, so a revert un-does that too).
    {
        SkyParams snap = *sky;
        snap.obsYear = m_solYear;
        snap.obsMonth = m_solMonth;
        snap.obsDay = m_solDay;
        snap.obsHourUtc = m_solHour;
        snap.obsLatitudeDeg = m_solLat;
        snap.obsLongitudeDeg = m_solLon;
        m_estimateSnapshot = snap;
    }

    texture::SkyEstimateWorker::Job job;
    job.photo = m_sourceImage; // the worker owns its own copy
    // EXIF hints are measurements. Date + place prefill the observer (the estimate's clock time
    // still comes from the SKY -- S5 inverts the measured elevation on the photo's date at the
    // photo's place); the wall time becomes the morning/afternoon tie-break hint, converted from
    // local to mean-solar UTC by the almanac's own longitude/15 convention. Touching the
    // date/place latches the moon source exactly as typing them would (first-set-wins).
    if (src->exif.has_value()) {
        const common::ExifData& e = *src->exif;
        if (e.dateTimeOriginal.has_value() && e.gpsLatitude.has_value() &&
            e.gpsLongitude.has_value()) {
            const common::ExifDateTime& dt = *e.dateTimeOriginal;
            m_solYear = std::clamp(dt.year, 1900, 2200);
            m_solMonth = dt.month;
            m_solDay = dt.day;
            m_solLat = *e.gpsLatitude;
            m_solLon = *e.gpsLongitude;
            double hourUtc = dt.hour + dt.minute / 60.0 - m_solLon / 15.0;
            hourUtc = std::fmod(hourUtc, 24.0);
            if (hourUtc < 0.0) hourUtc += 24.0;
            m_solHour = hourUtc;
            if (m_moonSource == 0) m_moonSource = 1;
            job.options.datePlaceFromExif = true;
        }
        if (e.focalLength35mm.has_value() && *e.focalLength35mm > 0)
            job.options.fovDegFromExif =
                2.0 * std::atan(18.0 / *e.focalLength35mm) * 180.0 / std::numbers::pi;
    }
    // The engine sees the CURRENT dialog observer (metadata-prefilled or not) as the S5
    // inversion's date/place and tie-break clock.
    SkyParams cur = *sky;
    cur.obsYear = m_solYear;
    cur.obsMonth = m_solMonth;
    cur.obsDay = m_solDay;
    cur.obsHourUtc = m_solHour;
    cur.obsLatitudeDeg = m_solLat;
    cur.obsLongitudeDeg = m_solLon;
    job.options.current = cur;
    job.options.dateAndPlaceMode = m_moonSource == 1;
    m_estimateEpoch = ++m_epoch;
    job.epoch = m_estimateEpoch;
    m_estimating = true;
    m_estimator->request(std::move(job));
    fenceForWork(_("Analyzing the layer\xE2\x80\xA6"));
    if (!m_polling) {
        m_polling = true;
        Fl::add_timeout(1.0 / 30.0, pollTimer, this);
    }
}

void TextureGeneratorDialog::finishEstimate(texture::SkyEstimateResult est) {
    m_estimating = false;
    m_estimateEpoch = 0;
    unfenceAfterWork();
    if (est.cancelled) {
        m_estimateSnapshot.reset();
        selectGenerator(m_generator); // restores the note + re-warms the proxy
        return;
    }
    const bool timeApplied = est.timeUtc.applied;
    m_estimate = std::move(est);
    if (m_estimate->aborted) {
        m_estimateSnapshot.reset(); // nothing changed -> nothing to revert
    } else {
        if (timeApplied)
            m_solHour = std::clamp(m_estimate->params.obsHourUtc, 0.0, 24.0);
        const SkyParams landed = m_estimate->params;
        applyEdit([&landed](TextureParams& p) {
            if (auto* s = std::get_if<SkyParams>(&p.spec)) *s = landed;
        });
    }
    rebuildControls(); // the summary block + Revert/swap rows appear
    syncControls();
    updateEstimateUi();
    m_ui->note->copy_label(m_estimate->aborted
                               ? _("No sky or horizon found -- settings unchanged.")
                               : _("Estimate applied -- the summary sits above the controls."));
    m_ui->note->redraw();
}

void TextureGeneratorDialog::cancelEstimate() {
    if (!m_estimating) return;
    m_estimator->cancelAll();
    m_estimating = false;
    m_estimateEpoch = 0;
    m_estimateSnapshot.reset();
    unfenceAfterWork();
    selectGenerator(m_generator); // restores the note + re-warms the proxy
}

void TextureGeneratorDialog::revertEstimate() {
    if (m_baking || m_estimating || m_conform != nullptr) return;
    if (!m_estimateSnapshot.has_value()) return;
    const SkyParams snap = *m_estimateSnapshot;
    m_estimateSnapshot.reset();
    m_estimate.reset();
    m_solYear = std::clamp(snap.obsYear, 1900, 2200);
    m_solMonth = std::clamp(snap.obsMonth, 1, 12);
    m_solDay = std::clamp(snap.obsDay, 1, 31);
    m_solHour = std::clamp(snap.obsHourUtc, 0.0, 24.0);
    m_solLat = std::clamp(snap.obsLatitudeDeg, -90.0, 90.0);
    m_solLon = std::clamp(snap.obsLongitudeDeg, -180.0, 180.0);
    applyEdit([&snap](TextureParams& p) {
        if (auto* s = std::get_if<SkyParams>(&p.spec)) *s = snap;
    });
    rebuildControls(); // the summary block + Revert row disappear
    syncControls();
    updateSkyInfo();
    updateEstimateUi();
    m_ui->note->copy_label(_("Estimate reverted."));
    m_ui->note->redraw();
}

std::optional<texture::SkyTimeInversion> TextureGeneratorDialog::estimateTimeSwap() const {
    if (!m_estimate.has_value() || m_estimate->aborted || !m_estimate->timeUtc.applied)
        return std::nullopt;
    // Ask the S5 helper for the crossing FARTHEST from the current pick: +12h flips the
    // nearest-solution policy to the other one. A single-crossing day returns the same hour;
    // treat that as "no alternative".
    const SkyParams& p = m_estimate->params;
    const texture::UtcTime date{p.obsYear, p.obsMonth, p.obsDay, p.obsHourUtc};
    const texture::SkyTimeInversion inv = texture::invertTimeFromElevation(
        date, p.obsLatitudeDeg, p.obsLongitudeDeg, m_estimate->sunElevation.value,
        std::fmod(p.obsHourUtc + 12.0, 24.0));
    if (!inv.valid || !inv.hasAlternative) return std::nullopt;
    if (std::abs(inv.hourUtc - m_estimate->timeUtc.value) < 1.0 / 60.0) return std::nullopt;
    return inv;
}

void TextureGeneratorDialog::swapEstimateTime() {
    if (m_baking || m_estimating || m_conform != nullptr) return;
    const std::optional<texture::SkyTimeInversion> inv = estimateTimeSwap();
    if (!inv.has_value() || !m_estimate.has_value()) return;
    m_solHour = std::clamp(inv->hourUtc, 0.0, 24.0);
    const texture::UtcTime t{m_estimate->params.obsYear, m_estimate->params.obsMonth,
                             m_estimate->params.obsDay, inv->hourUtc};
    const double lat = m_estimate->params.obsLatitudeDeg;
    const double lon = m_estimate->params.obsLongitudeDeg;
    applyEdit([&](TextureParams& p) {
        if (auto* s = std::get_if<SkyParams>(&p.spec)) texture::applyMasterClock(*s, t, lat, lon);
    });
    // The estimate record follows the pick, so the summary + the swap row stay honest.
    m_estimate->timeUtc.value = inv->hourUtc;
    m_estimate->timeUtc.note = inv->note;
    if (auto* s = std::get_if<SkyParams>(&activeParams().spec)) m_estimate->params = *s;
    rebuildControls();
    syncControls();
    updateSkyInfo();
}

bool TextureGeneratorDialog::conformOffered() const {
    return m_generator == texture::Generator::Sky && !m_editing &&
           static_cast<bool>(m_host.commitConform) && m_estimate.has_value() &&
           !m_estimate->aborted && m_estimate->segmentationUsable && !m_sourceImage.empty();
}

void TextureGeneratorDialog::setConformWanted(bool on) {
    m_conformWanted = on && conformOffered();
    updateEstimateUi();
}

void TextureGeneratorDialog::updateEstimateUi() {
    if (m_ui->conform == nullptr) return;
    if (conformOffered() && !m_baking && !m_estimating && m_conform == nullptr) {
        m_ui->conform->setChecked(m_conformWanted);
        m_ui->conform->show();
    } else {
        m_ui->conform->hide();
    }
}

std::vector<std::string> TextureGeneratorDialog::estimateSummaryForTest() const {
    std::vector<std::string> lines;
    if (!m_estimate.has_value()) return lines;
    if (m_estimate->aborted) {
        lines.emplace_back(_("No sky or horizon found -- settings unchanged."));
        return lines;
    }
    char b[192];
    const auto conf = [](const texture::EstimatedQuantity& q) {
        return q.confidence < 0.65 ? _(" (low confidence)") : "";
    };
    if (m_estimate->pitch.applied) {
        std::snprintf(b, sizeof(b), _("Horizon: pitch %.1f deg, roll %.1f deg%s"),
                      m_estimate->pitch.value, m_estimate->roll.value, conf(m_estimate->pitch));
        lines.emplace_back(b);
    }
    if (m_estimate->timeUtc.applied) {
        std::snprintf(b, sizeof(b), _("Time: %s UTC -- %s"),
                      hhmmUtc(m_estimate->timeUtc.value).c_str(),
                      m_estimate->timeUtc.note.c_str());
        lines.emplace_back(b);
    } else if (m_estimate->sunElevation.applied) {
        std::snprintf(b, sizeof(b), _("Sun: elevation %.1f deg, azimuth %.0f deg%s"),
                      m_estimate->sunElevation.value, m_estimate->sunAzimuth.value,
                      conf(m_estimate->sunElevation));
        lines.emplace_back(b);
    }
    if (m_estimate->turbidity.applied) {
        std::string atmo;
        std::snprintf(b, sizeof(b), _("Atmosphere: turbidity %.1f, exposure %+.1f EV"),
                      m_estimate->turbidity.value, m_estimate->exposure.value);
        atmo = b;
        if (m_estimate->cloudCoverage.applied) {
            std::snprintf(b, sizeof(b), _(", clouds %.0f%%"),
                          m_estimate->cloudCoverage.value * 100.0);
            atmo += b;
        }
        lines.emplace_back(std::move(atmo));
    }
    // The engine's own honesty lines (FOV, metadata credits, "left unchanged" notes, the mask
    // gate) close the block verbatim.
    std::string::size_type at = 0;
    const std::string& s = m_estimate->summary;
    while (at < s.size()) {
        std::string::size_type nl = s.find('\n', at);
        if (nl == std::string::npos) nl = s.size();
        if (nl > at) lines.emplace_back(s.substr(at, nl - at));
        at = nl + 1;
    }
    return lines;
}

Fl_Widget* TextureGeneratorDialog::estimateButtonForTest() const {
    return m_ui->estimate;
}
Fl_Widget* TextureGeneratorDialog::estimateBubbleForTest() const {
    return m_ui->estimateBubble;
}

// ⚠ The hover NEVER shows or hides the bubble inline -- it records the wanted state and lets a
// zero-delay timeout apply it once the current event has fully unwound. This is not tidiness; it is
// a macOS crash fix (SIGSEGV, ad-hoc report 2026-07-24).
//
// The hover arrives from inside `fl_fix_focus()`, FLTK's focus/enter-leave reconciliation. Showing a
// child sub-window there reaches `Fl_Cocoa_Window_Driver::makeWindow()`, which -- uniquely on the
// Cocoa driver -- dispatches `Fl::handle(FL_FOCUS, w)` PART WAY THROUGH creating the window, and
// then goes on to touch the half-built FLWindow (`[cw setSubwindowFrame]`). That nested handle runs
// fl_fix_focus AGAIN; belowmouse has just moved to the appearing bubble, so the button gets its
// FL_LEAVE, we hid the very window being created, FLTK tore its Fl_X down and left the FLWindow's
// `w` pointer NULL -- and setSubwindowFrame dereferenced it on the way back out.
//
// Deferring breaks the loop at its root: no sub-window is ever created or destroyed while FLTK is
// mid-fixup. (Same family as the "first show() must come after the parent is mapped" rule.)
void TextureGeneratorDialog::estimateHover(bool inside) {
    if (m_ui->estimateBubble == nullptr || m_ui->estimate == nullptr) return;
    m_estimateHoverInside = inside;
    if (m_estimateHoverArmed) return; // one pending tick applies whatever the latest state is
    m_estimateHoverArmed = true;
    Fl::add_timeout(0.0, estimateHoverTimeout, this);
}

void TextureGeneratorDialog::estimateHoverTimeout(void* self) {
    auto* d = static_cast<TextureGeneratorDialog*>(self);
    d->m_estimateHoverArmed = false;
    d->applyEstimateHover();
}

void TextureGeneratorDialog::applyEstimateHover() {
    if (m_ui->estimateBubble == nullptr || m_ui->estimate == nullptr) return;
    const bool inside = m_estimateHoverInside;
    if (!inside) {
        m_ui->estimateBubble->hide();
        return;
    }
    if (m_baking || m_estimating || m_conform != nullptr || !m_sourceAvailable) return;
    if (m_ui->estimateBubble->shownForAnchor(m_ui->estimate)) return;
    if (!m_bubbleThumbReady) {
        // Lazily built on FIRST hover, then cached for the session (the modal freezes the
        // layer under it, so the content revision cannot move).
        if (!m_host.sourceLayer) return;
        std::optional<TextureGenHost::SourceLayer> src = m_host.sourceLayer();
        if (!src || src->docImage.empty()) return;
        m_bubbleThumb = bubbleThumbnail(src->docImage);
        m_sourceName = src->name;
        m_bubbleThumbReady = true;
    }
    // Keep clear of the preview pane (the estimate lands there; keep it watchable).
    m_ui->estimateBubble->setAvoidRect(m_ui->preview->x(), m_ui->preview->y(),
                                       m_ui->preview->w(), m_ui->preview->h());
    m_ui->estimateBubble->openFor(m_ui->estimate, m_bubbleThumb, m_sourceName);
}

void TextureGeneratorDialog::startConform(TextureParams params,
                                          texture::TextureRenderResult baked) {
    auto run = std::make_unique<ConformRun>();
    run->params = std::move(params);
    run->baked = std::move(baked);
    run->photo = m_sourceImage; // copies: the thread shares nothing with the dialog
    run->estimate = *m_estimate;
    ConformRun* raw = run.get();
    m_conform = std::move(run);
    m_ui->note->copy_label(_("Isolating the sky\xE2\x80\xA6"));
    m_ui->note->redraw();
    m_ui->progress->setFraction(0.7);
    m_conform->thread = std::thread([raw] {
        raw->selection = texture::skySelectionFromEstimate(raw->photo, raw->estimate,
                                                           raw->progress.get(), &raw->note);
        if (!raw->progress->cancel.load(std::memory_order_relaxed) && !raw->selection.isEmpty()) {
            texture::PhotometricMatchInput in;
            if (const auto* s = std::get_if<SkyParams>(&raw->params.spec)) in.sky = *s;
            in.photoElevationDeg = raw->estimate.photoElevationForMatch;
            in.photoTurbidity = raw->estimate.photoTurbidityForMatch;
            in.photoSkyExposureEv = raw->estimate.params.exposure;
            in.confidence = raw->estimate.segConfidence;
            raw->match = texture::photometricMatchParams(in, raw->photo, raw->selection);
        }
        raw->done.store(true, std::memory_order_release);
    });
    if (!m_polling) {
        m_polling = true;
        Fl::add_timeout(1.0 / 30.0, pollTimer, this);
    }
}

void TextureGeneratorDialog::finishConform() {
    std::unique_ptr<ConformRun> run = std::move(m_conform);
    if (run->thread.joinable()) run->thread.join();
    if (run->progress->cancel.load(std::memory_order_relaxed)) {
        // Cancelled between the stages: the bake is discarded, the dialog stays open.
        unfenceAfterWork();
        selectGenerator(m_generator);
        return;
    }
    m_ui->progress->setFraction(1.0);
    TextureGenHost host = m_host; // hide() may destroy us via the app; stay safe
    const TextureParams committed = run->params;
    hide();
    if (run->selection.isEmpty() || run->match.empty() || !host.commitConform) {
        // §5.3's degrade: mask & harmonize skipped, the plain sky layer still lands.
        if (host.commit) host.commit(committed, std::move(run->baked));
        return;
    }
    host.commitConform(committed, std::move(run->baked),
                       {std::move(run->selection), std::move(run->match)});
}

void TextureGeneratorDialog::cancelConform() {
    if (m_conform == nullptr) return;
    m_conform->progress->cancel.store(true, std::memory_order_relaxed);
    m_ui->note->copy_label(_("Cancelling\xE2\x80\xA6"));
    m_ui->note->redraw();
    // The poll loop reaps the thread (it aborts between stages) and unfences in finishConform.
}

void TextureGeneratorDialog::reapplyTheme() {
    color(toFl(activePalette().windowBg));
    redraw();
}

int TextureGeneratorDialog::handle(int event) {
    if (event == FL_PUSH) {
        dismissActiveDropdownPopupOnOutsideClick(Fl::event_x(), Fl::event_y());
        dismissActiveContextMenuOnOutsideClick(Fl::event_x(), Fl::event_y());
        dismissActiveColorFlyoutOnOutsideClick(Fl::event_x(), Fl::event_y());
        dismissActiveMapFlyoutOnOutsideClick(Fl::event_x(), Fl::event_y());
        dismissActiveCalendarPopupOnOutsideClick(Fl::event_x(), Fl::event_y());
        if (m_ui->estimateBubble != nullptr) m_ui->estimateBubble->hide(); // hover-only chrome
    }
    if (event == FL_KEYDOWN) {
        if (Fl::event_key() == FL_Escape) {
            if (m_ui->estimateBubble != nullptr && m_ui->estimateBubble->shown()) {
                m_ui->estimateBubble->hide();
                return 1;
            }
            if (m_ui->colorFlyout != nullptr && m_ui->colorFlyout->shown()) {
                m_ui->colorFlyout->hide();
                return 1;
            }
            if (m_ui->mapFlyout != nullptr && m_ui->mapFlyout->shown()) {
                m_ui->mapFlyout->hide();
                return 1;
            }
            if (m_ui->datePicker != nullptr && m_ui->datePicker->calendarOpen()) {
                m_ui->datePicker->closeCalendar();
                return 1;
            }
            doCancel();
            return 1;
        }
        if (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) {
            if (Fl::focus() == nullptr || dynamic_cast<Fl_Input_*>(Fl::focus()) == nullptr) {
                create();
                return 1;
            }
        }
    }
    return Fl_Double_Window::handle(event);
}

// ------------------------------------------------------------------------------------------------
// TexturePreviewPane implementation
// ------------------------------------------------------------------------------------------------
TexturePreviewPane::View TexturePreviewPane::view() const {
    // Framing comes from the dialog's single viewSpec() truth (the same numbers the proxy was
    // requested with); this widget only adds where that view lands on screen.
    View v;
    const TextureGeneratorDialog::ViewSpec vs = m_owner->viewSpec();
    if (vs.frameW == 0 || vs.frameH == 0) return v;
    v.frameW = vs.frameW;
    v.frameH = vs.frameH;
    v.winX = vs.winX;
    v.winY = vs.winY;
    v.viewW = vs.viewW;
    v.viewH = vs.viewH;
    v.originX = x() + 1 + std::floor((std::max(1, innerW()) - v.viewW) / 2.0);
    v.originY = y() + 1 + std::floor((std::max(1, innerH()) - v.viewH) / 2.0);
    return v;
}

double TexturePreviewPane::insetRefAzimuth() const {
    return m_owner->m_generator == texture::Generator::Sky ? 180.0 : 0.0;
}

void TexturePreviewPane::draw() {
    const Palette& pal = activePalette();
    GizmoCanvas gc(w(), h(), pal.windowBg);
    const View v = view();
    const int ix = static_cast<int>(v.originX) - x();
    const int iy = static_cast<int>(v.originY) - y();
    const int iw = static_cast<int>(v.viewW);
    const int ih = static_cast<int>(v.viewH);
    if (iw > 0 && ih > 0) {
        gc.checker(ix, iy, ix + iw, iy + ih, {198, 198, 198, 255}, {162, 162, 162, 255});
        if (!m_img.rgba.empty()) gc.blitImage(m_img, ix, iy);
    }
    drawGizmos(gc, v);
    Fl_RGB_Image blit(gc.data(), w(), h(), 4);
    blit.draw(x(), y());
    fl_rect(x(), y(), w(), h(), toFl(pal.border));
    if (m_pending) {
        fl_color(toFl(pal.textMuted));
        fl_font(FL_HELVETICA, 11);
        fl_draw(_("Rendering\xE2\x80\xA6"), x() + 8, y() + h() - 10);
    }
}

void TexturePreviewPane::drawGizmos(GizmoCanvas& gc, const View& v) {
    if (v.frameW <= 0.0 || m_owner->m_baking) return;
    const Palette& pal = activePalette();
    const Grab lit = m_grab != Grab::None ? m_grab : m_hover;
    const Vec2 off{static_cast<double>(x()), static_cast<double>(y())};
    const TextureParams& p = m_owner->activeParams();
    const common::Color8 sunCol{255, 220, 90, 255};
    const common::Color8 windCol{140, 200, 255, 255};

    const auto insetRing = [&](Vec2 c, bool hot) {
        gc.fillDisc(c - off, kInsetR + 5.0, pal.panelBg, 0.55f);
        gc.strokeCircle(c - off, kInsetR, hot ? 2.2 : 1.4, pal.border, hot ? 1.0f : 0.8f);
    };

    switch (generatorUi(m_owner->m_generator).gizmos) {
    case GizmoLayout::Sky: {
        const auto& s = std::get<SkyParams>(p.spec);
        // Horizon line (+ the roll nub riding its right end).
        const bool horizonHot = lit == Grab::Horizon;
        const double yl = texgizmo::skyHorizonRowAt(s, v.frameW, v.frameH, 0.0);
        const double yr = texgizmo::skyHorizonRowAt(s, v.frameW, v.frameH, v.frameW);
        const Vec2 a = frameToWidget(v, 0.0, yl) - off;
        const Vec2 b = frameToWidget(v, v.frameW, yr) - off;
        gc.stroke(a, b, horizonHot ? 2.6 : 1.6, pal.accent, horizonHot ? 0.95f : 0.7f);
        const bool rollHot = lit == Grab::Roll;
        const Vec2 nub = a + (b - a) * 0.86;
        gc.fillDisc(nub, rollHot ? 6.0 : 4.5, pal.accent, 1.0f);
        // FOV brackets at mid-height.
        const double midY = (a.y + b.y) * 0.5;
        const auto bracket = [&](double bx, bool hot) {
            gc.stroke({bx, midY - 16.0}, {bx, midY + 16.0}, hot ? 3.2 : 2.2, pal.text,
                      hot ? 0.95f : 0.55f);
        };
        bracket(frameToWidget(v, v.frameW * 0.06, 0).x - off.x, lit == Grab::FovLeft);
        bracket(frameToWidget(v, v.frameW * 0.94, 0).x - off.x, lit == Grab::FovRight);
        // The sun, in frame when visible (below the horizon line = night; it still drags).
        if (const auto sp = texgizmo::skySunScreen(s, v.frameW, v.frameH)) {
            const Vec2 sw = frameToWidget(v, sp->x, sp->y) - off;
            const bool sunHot = lit == Grab::Sun;
            gc.strokeCircle(sw, sunHot ? 10.0 : 8.0, 1.6, sunCol, 0.9f);
            gc.fillDisc(sw, sunHot ? 6.0 : 4.5, sunCol, 1.0f);
        }
        // The moon, when enabled: a pale ringed dot, dragging just like the sun.
        if (s.enableMoon) {
            if (const auto mp =
                    texgizmo::skyAzElScreen(s, s.moonAzimuthDeg, s.moonElevationDeg, v.frameW,
                                            v.frameH)) {
                const Vec2 mw = frameToWidget(v, mp->x, mp->y) - off;
                const bool moonHot = lit == Grab::Moon;
                const common::Color8 moonCol{225, 228, 238, 255};
                gc.strokeCircle(mw, moonHot ? 9.0 : 7.0, 1.4, moonCol, 0.85f);
                gc.fillDisc(mw, moonHot ? 5.0 : 3.8, moonCol, 1.0f);
            }
        }
        // Sun dome inset (bottom-left) + wind compass (top-right).
        {
            const bool hot = lit == Grab::SunDome;
            insetRing(domeCenter(), hot);
            const auto dp = texgizmo::domeDot(domeCenter().x, domeCenter().y, kInsetR,
                                              s.sunAzimuthDeg, std::max(0.0, s.sunElevationDeg),
                                              insetRefAzimuth());
            gc.fillDisc(Vec2{dp.x, dp.y} - off, hot ? 5.5 : 4.5, sunCol, 1.0f);
        }
        {
            const bool hot = lit == Grab::Wind;
            insetRing(windCenter(), hot);
            const auto tp = texgizmo::compassDot(windCenter().x, windCenter().y, kInsetR,
                                                 s.windDirectionDeg, s.windStrength,
                                                 insetRefAzimuth());
            gc.stroke(windCenter() - off, Vec2{tp.x, tp.y} - off, hot ? 2.4 : 1.6, windCol,
                      0.95f);
            gc.fillDisc(Vec2{tp.x, tp.y} - off, hot ? 5.5 : 4.0, windCol, 1.0f);
        }
        break;
    }
    case GizmoLayout::Lawn: {
        const auto& g = std::get<GrassParams>(p.spec);
        const bool horizonHot = lit == Grab::Horizon;
        const double hy = texgizmo::grassHorizonRow(g, v.frameW, v.frameH);
        const Vec2 a = frameToWidget(v, 0.0, hy) - off;
        const Vec2 b = frameToWidget(v, v.frameW, hy) - off;
        gc.stroke(a, b, horizonHot ? 2.6 : 1.6, pal.accent, horizonHot ? 0.95f : 0.7f);
        const auto bracket = [&](double bx, bool hot) {
            gc.stroke({bx, a.y + 24.0}, {bx, a.y + 56.0}, hot ? 3.2 : 2.2, pal.text,
                      hot ? 0.95f : 0.55f);
        };
        bracket(frameToWidget(v, v.frameW * 0.06, 0).x - off.x, lit == Grab::FovLeft);
        bracket(frameToWidget(v, v.frameW * 0.94, 0).x - off.x, lit == Grab::FovRight);
        {
            const bool hot = lit == Grab::LightDome;
            insetRing(domeCenter(), hot);
            const auto dp = texgizmo::domeDot(domeCenter().x, domeCenter().y, kInsetR,
                                              g.lightAzimuthDeg, g.lightElevationDeg, 0.0);
            gc.fillDisc(Vec2{dp.x, dp.y} - off, hot ? 5.5 : 4.5, sunCol, 1.0f);
        }
        {
            const bool hot = lit == Grab::Wind;
            insetRing(windCenter(), hot);
            const auto tp = texgizmo::compassDot(windCenter().x, windCenter().y, kInsetR,
                                                 g.windDirectionDeg, g.windStrength, 0.0);
            gc.stroke(windCenter() - off, Vec2{tp.x, tp.y} - off, hot ? 2.4 : 1.6, windCol,
                      0.95f);
            gc.fillDisc(Vec2{tp.x, tp.y} - off, hot ? 5.5 : 4.0, windCol, 1.0f);
        }
        break;
    }
    case GizmoLayout::Surface: {
        // The flat lit sheet (paper + the S55-g materials): a raking-light dome always, and the
        // direction ring whenever the arm has a grain/vein/weave/brush axis (surfaceFields).
        const SurfaceFields f = surfaceFields(m_owner->activeParams());
        if (f.lightAz != nullptr) {
            const bool hot = lit == Grab::LightDome;
            insetRing(domeCenter(), hot);
            const auto dp = texgizmo::domeDot(domeCenter().x, domeCenter().y, kInsetR,
                                              *f.lightAz, *f.lightEl, 0.0);
            gc.fillDisc(Vec2{dp.x, dp.y} - off, hot ? 5.5 : 4.5, sunCol, 1.0f);
        }
        if (f.angleDeg != nullptr) {
            const bool hot = lit == Grab::Grain;
            insetRing(grainCenter(), hot);
            const double a = *f.angleDeg * texgizmo::kDeg;
            const Vec2 c = grainCenter() - off;
            const Vec2 dir{std::cos(a), std::sin(a)};
            gc.stroke(c - dir * (kInsetR - 3.0), c + dir * (kInsetR - 3.0), hot ? 2.6 : 1.8,
                      pal.text, hot ? 0.95f : 0.7f);
            gc.fillDisc(c + dir * (kInsetR - 3.0), hot ? 5.0 : 4.0, pal.text, 1.0f);
        }
        break;
    }
    }
}

TexturePreviewPane::Grab TexturePreviewPane::grabAt(Vec2 p, const View& v) const {
    if (v.frameW <= 0.0 || m_owner->m_baking) return Grab::None;
    const TextureParams& tp = m_owner->activeParams();
    const auto near = [&](Vec2 c, double r) { return (p - c).length() <= r; };
    switch (generatorUi(m_owner->m_generator).gizmos) {
    case GizmoLayout::Sky: {
        const auto& s = std::get<SkyParams>(tp.spec);
        if (near(domeCenter(), kInsetR + 6.0)) return Grab::SunDome;
        if (near(windCenter(), kInsetR + 6.0)) return Grab::Wind;
        if (const auto sp = texgizmo::skySunScreen(s, v.frameW, v.frameH)) {
            const Vec2 sw = frameToWidget(v, sp->x, sp->y);
            if (near(sw, 12.0)) return Grab::Sun;
        }
        if (s.enableMoon) {
            if (const auto mp = texgizmo::skyAzElScreen(s, s.moonAzimuthDeg, s.moonElevationDeg,
                                                        v.frameW, v.frameH)) {
                const Vec2 mw = frameToWidget(v, mp->x, mp->y);
                if (near(mw, 11.0)) return Grab::Moon;
            }
        }
        const double yl = texgizmo::skyHorizonRowAt(s, v.frameW, v.frameH, 0.0);
        const double yr = texgizmo::skyHorizonRowAt(s, v.frameW, v.frameH, v.frameW);
        const Vec2 a = frameToWidget(v, 0.0, yl);
        const Vec2 b = frameToWidget(v, v.frameW, yr);
        const Vec2 nub = a + (b - a) * 0.86;
        if (near(nub, 9.0)) return Grab::Roll;
        const double midY = (a.y + b.y) * 0.5;
        const double lx = frameToWidget(v, v.frameW * 0.06, 0).x;
        const double rx = frameToWidget(v, v.frameW * 0.94, 0).x;
        if (std::abs(p.x - lx) < 7.0 && std::abs(p.y - midY) < 20.0) return Grab::FovLeft;
        if (std::abs(p.x - rx) < 7.0 && std::abs(p.y - midY) < 20.0) return Grab::FovRight;
        // Distance to the horizon segment.
        const Vec2 ab = b - a;
        const double len2 = ab.dot(ab);
        const double t = len2 > 0.0 ? std::clamp((p - a).dot(ab) / len2, 0.0, 1.0) : 0.0;
        if ((p - (a + ab * t)).length() <= 6.0) return Grab::Horizon;
        break;
    }
    case GizmoLayout::Lawn: {
        const auto& g = std::get<GrassParams>(tp.spec);
        if (near(domeCenter(), kInsetR + 6.0)) return Grab::LightDome;
        if (near(windCenter(), kInsetR + 6.0)) return Grab::Wind;
        const double hy = texgizmo::grassHorizonRow(g, v.frameW, v.frameH);
        const Vec2 a = frameToWidget(v, 0.0, hy);
        const double lx = frameToWidget(v, v.frameW * 0.06, 0).x;
        const double rx = frameToWidget(v, v.frameW * 0.94, 0).x;
        if (std::abs(p.x - lx) < 7.0 && p.y > a.y + 18.0 && p.y < a.y + 62.0)
            return Grab::FovLeft;
        if (std::abs(p.x - rx) < 7.0 && p.y > a.y + 18.0 && p.y < a.y + 62.0)
            return Grab::FovRight;
        if (std::abs(p.y - a.y) <= 6.0) return Grab::Horizon;
        break;
    }
    case GizmoLayout::Surface:
        if (near(domeCenter(), kInsetR + 6.0)) return Grab::LightDome;
        if (surfaceFields(m_owner->activeParams()).angleDeg != nullptr &&
            near(grainCenter(), kInsetR + 6.0))
            return Grab::Grain;
        break;
    }
    // Empty preview ground: pan when there is anything hidden past the pane edge, else nothing.
    const bool pannable = v.frameW > v.viewW || v.frameH > v.viewH;
    return pannable ? Grab::Pan : Grab::None;
}

void TexturePreviewPane::dragBy(Vec2 d, Vec2 p, const View& v) {
    auto& dlg = *m_owner;
    const auto edit = [&](const std::function<void(TextureParams&)>& fn) { dlg.applyEdit(fn); };
    switch (m_grab) {
    case Grab::Pan: {
        // Composes on the press-time pan (m_last stays at the press point for this gesture -- the
        // type3d "drag miles" lesson): the content follows the pointer. Pan is in DOC pixels, so it
        // is zoom-independent; the delta divides by the live zoom to convert widget px -> doc px.
        const auto paneW = static_cast<std::uint32_t>(std::max(1, innerW()));
        const auto paneH = static_cast<std::uint32_t>(std::max(1, innerH()));
        const double z = effectiveZoom(paneW, paneH);
        m_panX = m_pressPan.x + (m_last.x - p.x) / z;
        m_panY = m_pressPan.y + (m_last.y - p.y) / z;
        clampPan(z);
        redraw();
        dlg.requestProxy();
        break;
    }
    case Grab::Horizon: {
        const Vec2 fp = widgetToFrame(v, p);
        if (generatorUi(dlg.m_generator).gizmos == GizmoLayout::Sky) {
            edit([&](TextureParams& tp) {
                auto& s = std::get<SkyParams>(tp.spec);
                s.pitchDeg = std::clamp(
                    texgizmo::skyPitchForHorizonRow(s, v.frameW, v.frameH, fp.y), -10.0, 85.0);
            });
        } else {
            edit([&](TextureParams& tp) {
                auto& g = std::get<GrassParams>(tp.spec);
                g.pitchDeg = std::clamp(
                    texgizmo::grassPitchForHorizonRow(g, v.frameW, v.frameH, fp.y), 2.0, 60.0);
            });
        }
        break;
    }
    case Grab::Roll: {
        const Vec2 c{x() + w() * 0.5, y() + h() * 0.5};
        const Vec2 prev = p - d;
        double da = std::atan2(p.y - c.y, p.x - c.x) - std::atan2(prev.y - c.y, prev.x - c.x);
        while (da > std::numbers::pi) da -= 2.0 * std::numbers::pi;
        while (da < -std::numbers::pi) da += 2.0 * std::numbers::pi;
        edit([&](TextureParams& tp) {
            auto& s = std::get<SkyParams>(tp.spec);
            s.rollDeg = std::clamp(s.rollDeg + da / texgizmo::kDeg, -45.0, 45.0);
        });
        break;
    }
    case Grab::Sun: {
        const Vec2 fp = widgetToFrame(v, p);
        edit([&](TextureParams& tp) {
            texgizmo::skySunFromScreen(std::get<SkyParams>(tp.spec), v.frameW, v.frameH, fp.x,
                                       fp.y);
        });
        break;
    }
    case Grab::Moon: {
        const Vec2 fp = widgetToFrame(v, p);
        edit([&](TextureParams& tp) {
            auto& s = std::get<SkyParams>(tp.spec);
            double az = s.moonAzimuthDeg, el = s.moonElevationDeg;
            texgizmo::skyAzElFromScreen(s, v.frameW, v.frameH, fp.x, fp.y, az, el);
            s.moonAzimuthDeg = az;
            s.moonElevationDeg = std::clamp(el, 0.0, 80.0);
        });
        break;
    }
    case Grab::SunDome: {
        double az = 0.0, el = 0.0;
        texgizmo::domeFromPoint(domeCenter().x, domeCenter().y, kInsetR, p.x, p.y,
                                insetRefAzimuth(), az, el);
        edit([&](TextureParams& tp) {
            auto& s = std::get<SkyParams>(tp.spec);
            s.sunAzimuthDeg = az;
            s.sunElevationDeg = el;
        });
        break;
    }
    case Grab::Wind: {
        double dir = 0.0, strength = 0.0;
        texgizmo::compassFromPoint(windCenter().x, windCenter().y, kInsetR, p.x, p.y,
                                   insetRefAzimuth(), dir, strength);
        edit([&](TextureParams& tp) {
            if (auto* s = std::get_if<SkyParams>(&tp.spec)) {
                s->windDirectionDeg = dir;
                s->windStrength = strength;
            } else if (auto* g = std::get_if<GrassParams>(&tp.spec)) {
                g->windDirectionDeg = dir;
                g->windStrength = strength;
            }
        });
        break;
    }
    case Grab::FovLeft:
    case Grab::FovRight: {
        // Dragging a bracket OUTWARD zooms in (narrower view), photographic pinch intuition.
        const double outward = m_grab == Grab::FovRight ? d.x : -d.x;
        edit([&](TextureParams& tp) {
            if (auto* s = std::get_if<SkyParams>(&tp.spec))
                s->fovDeg = std::clamp(s->fovDeg - outward * 0.25, 15.0, 150.0);
            else if (auto* g = std::get_if<GrassParams>(&tp.spec))
                g->fovDeg = std::clamp(g->fovDeg - outward * 0.25, 20.0, 120.0);
        });
        break;
    }
    case Grab::Grain: {
        const Vec2 c = grainCenter();
        double ang = std::atan2(p.y - c.y, p.x - c.x) / texgizmo::kDeg;
        ang = std::fmod(ang + 360.0, 180.0);
        edit([&](TextureParams& tp) {
            if (double* angle = surfaceFields(tp).angleDeg) *angle = ang;
        });
        break;
    }
    case Grab::LightDome: {
        double az = 0.0, el = 0.0;
        texgizmo::domeFromPoint(domeCenter().x, domeCenter().y, kInsetR, p.x, p.y, 0.0, az, el);
        edit([&](TextureParams& tp) {
            if (const SurfaceFields f = surfaceFields(tp); f.lightAz != nullptr) {
                *f.lightAz = az;
                *f.lightEl = std::clamp(el, 5.0, 85.0);
            } else if (auto* g = std::get_if<GrassParams>(&tp.spec)) {
                g->lightAzimuthDeg = az;
                g->lightElevationDeg = std::clamp(el, 5.0, 85.0);
            }
        });
        break;
    }
    case Grab::None:
        break;
    }
}

void TexturePreviewPane::zoomAt(Vec2 cursor, int wheelDy) {
    const auto paneW = static_cast<std::uint32_t>(std::max(1, innerW()));
    const auto paneH = static_cast<std::uint32_t>(std::max(1, innerH()));
    const double fit = texgizmo::previewFitZoom(m_owner->m_docW, m_owner->m_docH, paneW, paneH);
    const double oldZoom = effectiveZoom(paneW, paneH);
    // The document point currently under the cursor (through the live frame mapping).
    const View v = view();
    const Vec2 fp = widgetToFrame(v, cursor);
    const double docX = oldZoom > 0.0 ? fp.x / oldZoom : 0.0;
    const double docY = oldZoom > 0.0 ? fp.y / oldZoom : 0.0;
    double nz = oldZoom * std::pow(1.2, static_cast<double>(-wheelDy)); // wheel up (dy<0) = zoom in
    nz = std::clamp(nz, fit, kZoomMax);
    if (std::abs(nz - oldZoom) < 1e-9) return;
    m_fit = nz <= fit * (1.0 + 1e-6);
    m_zoom = nz;
    // Keep that doc point under the cursor: with the frame larger than the pane the origin sits at
    // the pane's top-left, so pan(doc) = docPoint - cursorOffset/zoom; a frame smaller than the
    // pane is centred and clampPan pins the pan to 0.
    m_panX = docX - (cursor.x - (x() + 1)) / nz;
    m_panY = docY - (cursor.y - (y() + 1)) / nz;
    clampPan(nz);
    redraw();
    m_owner->requestProxy();
}

void TexturePreviewPane::clampPan(double zoom) {
    const auto paneW = static_cast<std::uint32_t>(std::max(1, innerW()));
    const auto paneH = static_cast<std::uint32_t>(std::max(1, innerH()));
    m_panX = std::clamp(m_panX, 0.0, texgizmo::previewMaxPanDoc(m_owner->m_docW, paneW, zoom));
    m_panY = std::clamp(m_panY, 0.0, texgizmo::previewMaxPanDoc(m_owner->m_docH, paneH, zoom));
}

int TexturePreviewPane::handle(int event) {
    switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
        const View v = view();
        const Grab over = grabAt(eventPos(), v);
        if (over != m_hover) {
            m_hover = over;
            // Per-element tooltip: re-enter the tooltip system with a small live area around
            // the pointer so leaving the element re-arms the next one (FLTK shows one tip per
            // widget entry otherwise).
            copy_tooltip(tooltipFor(over));
            Fl_Tooltip::enter_area(this, Fl::event_x() - x() - 4, Fl::event_y() - y() - 4, 8, 8,
                                   tooltip());
            redraw();
        }
        if (window() != nullptr) {
            // ⚠ The pan affordance is NOT the stock FL_CURSOR_MOVE on Wayland -- FLTK asks the
            // theme for `move`, and breeze answers with `dnd-move`, a closed grabbing hand whose
            // hotspot sits ~10 px from where the art appears to point. ui::MoveCursor substitutes
            // the four-way arrow there (Wayland only; docs/wayland.md §2.5).
            if (over == Grab::None)
                window()->cursor(FL_CURSOR_DEFAULT);
            else if (over == Grab::Pan)
                m_moveCursor.apply(window());
            else
                window()->cursor(FL_CURSOR_HAND);
        }
        return 1;
    }
    case FL_LEAVE:
        if (m_hover != Grab::None) {
            m_hover = Grab::None;
            redraw();
        }
        if (window() != nullptr) window()->cursor(FL_CURSOR_DEFAULT);
        return 1;
    case FL_PUSH: {
        // A double-click anywhere resets the view (the user's "double-click, or a tiny button").
        if (Fl::event_button() == FL_LEFT_MOUSE && Fl::event_clicks() > 0) {
            resetView();
            m_owner->requestProxy();
            m_grab = Grab::None;
            return 1;
        }
        const View v = view();
        m_last = eventPos();
        m_grab = grabAt(m_last, v);
        m_pressPan = {m_panX, m_panY};
        redraw();
        return 1;
    }
    case FL_MOUSEWHEEL: {
        // Zoom toward the cursor. MUST decline the wheel when the pointer is elsewhere, or it
        // starves the sibling controls / scroll of their wheel events ([[mosaic-ui-gotchas]]).
        if (!Fl::event_inside(this)) return 0;
        zoomAt(eventPos(), Fl::event_dy());
        return 1;
    }
    case FL_DRAG: {
        if (m_grab == Grab::None) return 1;
        const View v = view();
        const Vec2 p = eventPos();
        const Vec2 d = p - m_last;
        dragBy(d, p, v);
        if (m_grab != Grab::Pan) m_last = p; // pan composes on the press-time accumulator
        return 1;
    }
    case FL_RELEASE:
        m_grab = Grab::None;
        m_hover = grabAt(eventPos(), view());
        redraw();
        return 1;
    default:
        return Fl_Widget::handle(event);
    }
}

} // namespace mosaic::ui
