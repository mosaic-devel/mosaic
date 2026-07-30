#include "common/charconv_compat.hpp"
#include "ui/scrub_slider.hpp"

#include "common/i18n.hpp" // _() for the context-menu labels
#include "ui/theme.hpp"    // Palette, activePalette
#include "ui/widgets.hpp"  // ContextMenu / ContextAction / contextMenuFor (themed right-click menu)

#include <FL/Enumerations.H> // fl_rgb_color, FL_* keys/buttons
#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace mosaic::ui {

namespace {
Fl_Color toFl(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }

// Geometry / feel constants (the width is set by the caller; these are the look + gesture tuning).
constexpr int kEdgePad = 4;       // value-travel inset from each end (so the marker shows at min/max)
constexpr int kCorner = 4;        // bar corner radius (matches the combobox / themed controls)
constexpr int kDeadzonePx = 24;   // |dy| within this -> normal speed, no ruler (user-approved)
constexpr int kRampPx = 150;      // |dy| past the deadzone to reach max precision (user: shorter)
constexpr double kMaxDivisor = 24.0; // finest sensitivity = 1/24 of the local pixel gain
constexpr int kMoveThresh = 3;    // px of motion before a press counts as a scrub (not a click)

// The slider whose value is currently being type-edited (at most one), for the main window's
// outside-click commit. Set in beginEdit, cleared in endEditMode / the dtor.
ScrubSlider* g_activeScrubEdit = nullptr;

// Linear per-channel blend a->b (t in [0,1]); for the subtle value fill.
common::Color8 blend(common::Color8 a, common::Color8 b, double t) {
    const auto mix = [&](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::lround(x + (y - x) * std::clamp(t, 0.0, 1.0)));
    };
    return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), 255};
}
} // namespace

// ============================================================================================
//  Pure value model
// ============================================================================================
namespace scrub_detail {

double curveToValue(double t, ScrubCurve curve, double k, double mn, double mx) {
    t = std::clamp(t, 0.0, 1.0);
    switch (curve) {
    case ScrubCurve::Gamma:
        if (k > 0.0)
            return mn + (mx - mn) * std::pow(t, k);
        break;
    case ScrubCurve::Log:
        if (mn > 0.0 && mx > 0.0)
            return mn * std::pow(mx / mn, t);
        break; // ill-defined range -> fall through to linear
    case ScrubCurve::Linear:
        break;
    }
    return mn + (mx - mn) * t;
}

double valueToTrack(double v, ScrubCurve curve, double k, double mn, double mx) {
    if (mx == mn)
        return 0.0;
    switch (curve) {
    case ScrubCurve::Gamma:
        if (k > 0.0) {
            const double f = std::clamp((v - mn) / (mx - mn), 0.0, 1.0);
            return std::pow(f, 1.0 / k);
        }
        break;
    case ScrubCurve::Log:
        if (mn > 0.0 && mx > 0.0 && v > 0.0)
            return std::clamp(std::log(v / mn) / std::log(mx / mn), 0.0, 1.0);
        break;
    case ScrubCurve::Linear:
        break;
    }
    return std::clamp((v - mn) / (mx - mn), 0.0, 1.0);
}

double unitsPerPixel(double v, ScrubCurve curve, double k, double mn, double mx, int trackPx) {
    const int w = std::max(1, trackPx);
    const double t = valueToTrack(v, curve, k, mn, mx);
    const double dt = 0.5 / w; // half a pixel each side
    const double lo = curveToValue(t - dt, curve, k, mn, mx);
    const double hi = curveToValue(t + dt, curve, k, mn, mx);
    const double upp = std::fabs(hi - lo); // value units spanned by one pixel here
    // Floor so a Gamma/Log low end (derivative -> 0 at t==0) can never fully stall: at least the
    // whole range crossed over a generous 50k px of travel.
    const double floorUpp = std::fabs(mx - mn) / 50000.0;
    return std::max(upp, floorUpp);
}

double precision01(int dyAbs, int deadzonePx, int rampPx) {
    if (rampPx <= 0)
        return dyAbs > deadzonePx ? 1.0 : 0.0;
    return std::clamp(static_cast<double>(dyAbs - deadzonePx) / rampPx, 0.0, 1.0);
}

double gainDivisor(double p01, double maxDiv) {
    p01 = std::clamp(p01, 0.0, 1.0);
    // Eased (p^2) so the first pixels out of the deadzone stay close to 1:1 and it tightens as you
    // pull further -- a gentle entry into precision.
    return 1.0 + p01 * p01 * (std::max(1.0, maxDiv) - 1.0);
}

double snapTo(double v, double snap) {
    if (snap <= 0.0)
        return v;
    return std::round(v / snap) * snap;
}

double quantize(double v, double step, double mn, double mx) {
    const double lo = std::min(mn, mx);
    const double hi = std::max(mn, mx);
    if (step > 0.0)
        v = std::round(v / step) * step;
    return std::clamp(v, lo, hi);
}

// Decimals needed to represent `step` (0 for whole steps); capped so the readout never looks busy.
int decimalsForStep(double step) {
    if (step >= 1.0 || step <= 0.0)
        return step <= 0.0 ? 2 : 0;
    int d = 0;
    double s = step;
    while (d < 3 && std::fabs(s - std::round(s)) > 1e-9) {
        s *= 10.0;
        ++d;
    }
    return d;
}

std::string format(double v, double step, const std::string& suffix) {
    if (v == 0.0)
        v = 0.0; // normalise -0.0 -> 0.0 so it never prints "-0"
    // Only as many decimals as the step needs (so a 0.1-step slider shows at most one), then trim
    // trailing zeros: 24.0 -> "24", 24.50 -> "24.5", 1000.0 -> "1000".
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.*f", decimalsForStep(step), v);
    std::string s(buf);
    // Trim trailing zeros + the decimal separator. snprintf honours the C locale, so the separator may
    // be ',' (the user-reported "24,0px"): check for either, or the zero never gets trimmed.
    if (s.find_first_of(".,") != std::string::npos) {
        while (!s.empty() && s.back() == '0')
            s.pop_back();
        if (!s.empty() && (s.back() == '.' || s.back() == ','))
            s.pop_back();
    }
    return s + suffix;
}

} // namespace scrub_detail

using namespace scrub_detail;

// ============================================================================================
//  ScrubSlider
// ============================================================================================
ScrubSlider::ScrubSlider(int X, int Y, int W, int H, const char* label)
    : Fl_Valuator(X, Y, W, H, label) {
    box(FL_FLAT_BOX);
    // Keep visible_focus ON: it is what lets take_focus()/keyboard delivery work for the in-place
    // type-in. We never draw the focus ring (draw() is fully custom and never calls draw_focus()), so
    // there is no visual cost; FL_FOCUS below still refuses focus unless we are actually editing, so
    // the slider is not a Tab-stop at rest.
    m_themeSub = ThemeSubscription([this] { redraw(); });
}

ScrubSlider::~ScrubSlider() {
    if (g_activeScrubEdit == this)
        g_activeScrubEdit = nullptr;
    Fl::remove_timeout(blinkCb, this); // a torn-down slider must not leave a live caret-blink callback
    // NB: do NOT touch m_ruler here. On window teardown the shared ScrubRuler (a sibling child of the
    // main window) may already be destroyed, so m_ruler->hide() would be a use-after-free (the crash
    // on close). A drag can't outlive a rebuild, so there is nothing to hide at destruction anyway.
}

void ScrubSlider::setSuffix(std::string s) {
    m_suffix = std::move(s);
    redraw();
}
void ScrubSlider::setResponseCurve(ScrubCurve c, double k) {
    m_curve = c;
    m_curveK = k;
}
void ScrubSlider::setSnapStep(double s) { m_snapStep = s; }
void ScrubSlider::setDefaultValue(double v) {
    m_default = v;
    m_hasDefault = true;
}
void ScrubSlider::setCellColor(common::Color8 c) {
    m_cellColor = c;
    m_cellColorSet = true;
    redraw();
}

int ScrubSlider::trackX0() const { return x() + kEdgePad; }
int ScrubSlider::trackW() const { return std::max(1, w() - 2 * kEdgePad); }

common::Color8 ScrubSlider::cellColor() const {
    return m_cellColorSet ? m_cellColor : activePalette().panelBg;
}

double ScrubSlider::effectiveSnap() const {
    if (m_snapStep > 0.0)
        return m_snapStep;
    // Derive a sensible coarse grid: half-unit when the step is sub-integer, else a 1-2-5 multiple
    // giving ~20 stops across the range.
    if (step() < 1.0)
        return 0.5;
    const double rough = std::fabs(maximum() - minimum()) / 20.0;
    const double mag = std::pow(10.0, std::floor(std::log10(std::max(1.0, rough))));
    const double n = rough / mag;
    const double nice = (n < 1.5) ? 1 : (n < 3) ? 2 : (n < 7) ? 5 : 10;
    return std::max(step(), nice * mag);
}

double ScrubSlider::trackFractionAt(int eventX) const {
    return std::clamp(static_cast<double>(eventX - trackX0()) / trackW(), 0.0, 1.0);
}

void ScrubSlider::pushAndDrag(double v) {
    // Route a discrete change (wheel / reset / type-in) through the valuator so step rounding, when()
    // and the callback fire exactly as during a drag. Clamp ourselves first: Fl_Valuator::handle_drag
    // soft-clamps (it lets the wheel overshoot past min/max), so a hard clamp keeps values in range.
    v = std::clamp(v, std::min(minimum(), maximum()), std::max(minimum(), maximum()));
    handle_push();
    handle_drag(v);
    handle_release();
    redraw();
}

void ScrubSlider::draw() {
    const Palette& p = activePalette();
    const bool on = active_r();
    const int cy = y() + h() / 2;

    // Clear our cell with a raw fill (not draw_box, which greys a disabled widget's ground) so the
    // panel ground stays put behind the rounded bar.
    fl_color(toFl(cellColor()));
    fl_rectf(x(), y(), w(), h());

    // The bar: a full-height rounded control, same height + corner as the combobox (no thin track,
    // no ball handle). It is the whole widget rect inset by 1px so the border isn't clipped.
    const int bx = x() + 1;
    const int by = y() + 1;
    const int bw = w() - 2;
    const int bh = h() - 2;
    const double t = valueToTrack(value(), m_curve, m_curveK, minimum(), maximum());

    fl_color(toFl(on && m_hover ? p.controlHover : p.controlBg)); // bar ground (subtle hover lift)
    fl_rounded_rectf(bx, by, bw, bh, kCorner);

    fl_font(FL_HELVETICA, 12);
    const int baseY = y() + (h() + fl_height()) / 2 - fl_descent(); // vertical-centred text baseline
    const int centreX = x() + w() / 2;

    if (m_editing) {
        // In-place editing: NO pop-up. The number + muted suffix are centred as a group, with a caret
        // and selection we draw ourselves. No fill/marker while typing (it would show the stale value).
        const int suffixW = m_suffix.empty() ? 0 : static_cast<int>(fl_width(m_suffix.c_str()));
        const int gapW = m_suffix.empty() ? 0 : 4;
        const int numW = static_cast<int>(fl_width(m_editBuf.c_str()));
        const int numLeft = centreX - (numW + gapW + suffixW) / 2;

        const int a = std::min(m_selAnchor, m_caret);
        const int b = std::max(m_selAnchor, m_caret);
        if (a != b) {
            const int ax = numLeft + static_cast<int>(fl_width(m_editBuf.c_str(), a));
            const int bxs = numLeft + static_cast<int>(fl_width(m_editBuf.c_str(), b));
            fl_color(toFl(p.accent));
            fl_rectf(ax, cy - 8, std::max(1, bxs - ax), 16);
        }
        fl_color(toFl(p.text));
        fl_draw(m_editBuf.c_str(), numLeft, baseY);
        if (a != b) {
            const int ax = numLeft + static_cast<int>(fl_width(m_editBuf.c_str(), a));
            fl_color(toFl(p.onAccent));
            fl_draw(m_editBuf.substr(a, b - a).c_str(), ax, baseY);
        }
        if (m_caretOn) {
            const int caretX = numLeft + static_cast<int>(fl_width(m_editBuf.c_str(), m_caret));
            fl_color(toFl(p.text));
            fl_line(caretX, cy - 8, caretX, cy + 8);
        }
        if (!m_suffix.empty()) {
            fl_color(toFl(p.textMuted));
            fl_draw(m_suffix.c_str(), numLeft + numW + gapW, baseY);
        }
        fl_color(toFl(p.border)); // border last so it stays crisp over the masked glyphs
        fl_rounded_rect(bx, by, bw, bh, kCorner);
        return;
    }

    // The value FILL: the full rounded bar redrawn in a muted-accent colour but CLIPPED to the left
    // portion, so it keeps the bar's rounded LEFT corners and ends square at the value position. The
    // fill edge is the position indicator -- there is NO separate line marker (dropped per user). It
    // spans the FULL bar width, so it's empty at min (t=0) and fills completely at max (t=1).
    const common::Color8 fillCol = on ? blend(p.controlBg, p.accent, 0.6) : p.controlBg;
    const int fillW = std::clamp(static_cast<int>(std::lround(t * bw)), 0, bw);
    if (on && fillW > 0) {
        fl_push_clip(bx, by, fillW, bh);
        fl_color(toFl(fillCol));
        fl_rounded_rectf(bx, by, bw, bh, kCorner);
        fl_pop_clip();
    }

    // The optional value-ramp strip (S32): a thin per-pixel band along the bar's bottom edge --
    // the Hue slider's spectrum and friends. Drawn AFTER the fill so it stays readable across
    // the whole range. Full width: within the corner zones each column's bottom follows the
    // rounded boundary (drop = r - sqrt(r^2 - dx^2)) so the ramp hugs the curve instead of
    // stopping short of it (user 2026-07-17).
    if (m_trackFill) {
        const int gh = 4;
        const double r = kCorner;
        for (int i = 0; i < bw; ++i) {
            int drop = 0;
            if (i < kCorner) {
                const double dx = kCorner - i - 0.5;
                drop = static_cast<int>(std::ceil(r - std::sqrt(std::max(0.0, r * r - dx * dx))));
            } else if (i >= bw - kCorner) {
                const double dx = i - (bw - kCorner) + 0.5;
                drop = static_cast<int>(std::ceil(r - std::sqrt(std::max(0.0, r * r - dx * dx))));
            }
            const int colBottom = by + bh - 2 - drop; // 1px inside the border hairline
            const common::Color8 c =
                m_trackFill(static_cast<double>(i) / std::max(1, bw - 1));
            fl_color(toFl(c));
            fl_yxline(bx + i, colBottom - (gh - 1), colBottom);
        }
    }

    // The value, centred (Krita-style). Two-tone: text colour, then the part over the fill in on-accent
    // so it reads on both grounds. Suppressed while the precision ruler is up (it shows the same value).
    if (!m_precision) {
        const std::string label = scrub_detail::format(value(), step(), m_suffix);
        const int lw = static_cast<int>(fl_width(label.c_str()));
        const int lx = centreX - lw / 2;
        fl_color(toFl(on ? p.text : p.textMuted));
        fl_draw(label.c_str(), lx, baseY);
        if (on && fillW > 0) {
            fl_push_clip(bx, by, fillW, bh);
            fl_color(toFl(p.onAccent));
            fl_draw(label.c_str(), lx, baseY);
            fl_pop_clip();
        }
    }

    fl_color(toFl(p.border)); // hairline border on top
    fl_rounded_rect(bx, by, bw, bh, kCorner);
}

void ScrubSlider::beginEdit() {
    if (m_editing)
        return;
    m_editing = true;
    // Seed the buffer with the current value as a minimal numeric string (no suffix) -- reuses the same
    // step-derived, trailing-zero-trimmed, locale-aware formatting the readout uses.
    m_editBuf = scrub_detail::format(value(), step(), "");
    editSelectAll();
    m_caretOn = true;
    g_activeScrubEdit = this;
    Fl::focus(this);
    Fl::remove_timeout(blinkCb, this);
    Fl::add_timeout(0.53, blinkCb, this);
    redraw();
}

void ScrubSlider::commitEdit() {
    if (!m_editing)
        return;
    // Parse locale-independently: normalise a ',' decimal to '.' then from_chars (which is always
    // '.'-based), so a comma-locale buffer ("24,5") and a dot one both read correctly.
    std::string norm = m_editBuf;
    for (char& c : norm)
        if (c == ',')
            c = '.';
    double v = 0.0;
    const char* first = norm.c_str();
    const auto* last = first + norm.size();
    const auto res = mosaic::common::fromChars(first, last, v);
    const bool parsed = res.ec == std::errc{} && res.ptr != first;
    endEditMode();
    if (parsed)
        pushAndDrag(v); // step rounding + range clamp + the FL_WHEN_CHANGED callback
}

void ScrubSlider::cancelEdit() { endEditMode(); }

void ScrubSlider::endEditMode() {
    if (!m_editing)
        return;
    m_editing = false;
    if (g_activeScrubEdit == this)
        g_activeScrubEdit = nullptr;
    Fl::remove_timeout(blinkCb, this);
    if (Fl::focus() == this && window() != nullptr)
        window()->take_focus(); // release keyboard focus so global shortcuts work again
    redraw();
}

void ScrubSlider::blinkCb(void* self) {
    auto* s = static_cast<ScrubSlider*>(self);
    if (!s->m_editing)
        return;
    s->m_caretOn = !s->m_caretOn;
    s->redraw();
    Fl::repeat_timeout(0.53, blinkCb, self);
}

void ScrubSlider::deleteSelection() {
    if (!hasSelection())
        return;
    const int a = std::min(m_selAnchor, m_caret);
    const int b = std::max(m_selAnchor, m_caret);
    m_editBuf.erase(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a));
    m_caret = a;
    m_selAnchor = a;
}

void ScrubSlider::editInsert(const char* s) {
    if (s == nullptr)
        return;
    deleteSelection();
    for (const char* c = s; *c != '\0'; ++c) {
        const char ch = *c;
        const bool digit = std::isdigit(static_cast<unsigned char>(ch)) != 0;
        // Accept either decimal separator, but only one total (re-checked against the growing buffer).
        const bool sep = (ch == '.' || ch == ',') && m_editBuf.find_first_of(".,") == std::string::npos;
        const bool minus = ch == '-' && m_caret == 0 && m_editBuf.find('-') == std::string::npos;
        if (!digit && !sep && !minus)
            continue; // numeric-only field
        m_editBuf.insert(static_cast<std::size_t>(m_caret), 1, ch);
        ++m_caret;
    }
    m_selAnchor = m_caret;
}

void ScrubSlider::editSelectAll() {
    m_selAnchor = 0;
    m_caret = static_cast<int>(m_editBuf.size());
}

void ScrubSlider::editClipboard(bool cut) {
    std::string text;
    if (hasSelection()) {
        const int a = std::min(m_selAnchor, m_caret);
        const int b = std::max(m_selAnchor, m_caret);
        text = m_editBuf.substr(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a));
    } else {
        text = m_editBuf; // nothing selected -> copy the whole value (matches the text-field menu)
    }
    Fl::copy(text.c_str(), static_cast<int>(text.size()), 1);
    if (cut && hasSelection())
        deleteSelection();
}

void ScrubSlider::caretFromX(int eventX) {
    fl_font(FL_HELVETICA, 12);
    const int suffixW = m_suffix.empty() ? 0 : static_cast<int>(fl_width(m_suffix.c_str()));
    const int gapW = m_suffix.empty() ? 0 : 4;
    const int numW = static_cast<int>(fl_width(m_editBuf.c_str()));
    const int numLeft = (x() + w() / 2) - (numW + gapW + suffixW) / 2; // matches draw()'s centred group
    int best = 0;
    int bestDx = std::abs(eventX - numLeft);
    for (int i = 1; i <= static_cast<int>(m_editBuf.size()); ++i) {
        const int xi = numLeft + static_cast<int>(fl_width(m_editBuf.c_str(), i));
        const int dx = std::abs(eventX - xi);
        if (dx < bestDx) {
            bestDx = dx;
            best = i;
        }
    }
    m_caret = best;
}

void ScrubSlider::openValueContextMenu() {
    ContextMenu* menu = contextMenuFor(top_window());
    if (menu == nullptr)
        return; // no themed host on this top-level -> no menu (the field is still editable)
    const bool hasSel = hasSelection();
    const bool hasText = !m_editBuf.empty();
    std::vector<ContextAction> actions;
    actions.push_back({_("Cut"), [this] { editClipboard(true); redraw(); }, hasSel});
    actions.push_back({_("Copy"), [this] { editClipboard(false); }, hasText});
    actions.push_back({_("Paste"), [this] { Fl::paste(*this, 1); }, true});
    actions.push_back({_("Delete"), [this] { deleteSelection(); redraw(); }, hasSel, /*divider=*/true});
    actions.push_back({_("Select All"), [this] { editSelectAll(); redraw(); }, hasText});
    // Anchor at the cursor in the menu's top-level coords (add each sub-window offset up the chain).
    int hx = Fl::event_x();
    int hy = Fl::event_y();
    for (Fl_Window* w = window(); w != nullptr && w != menu->window(); w = w->window()) {
        hx += w->x();
        hy += w->y();
    }
    menu->openWith(hx, hy, std::move(actions));
}

void ScrubSlider::updateRuler() {
    // Only drive the shared HUD when it lives under the same TOP-LEVEL window as us. A slider nested in
    // a child sub-window (the options overflow popover, the Type panel) has a different immediate
    // window() than the ruler, but the same top_window() -- and place() maps the anchor through
    // top_window_offset(), so its position is still correct and the ruler (created after those popovers)
    // stacks above them. Comparing top_window() rather than window() is what lets those sliders show it.
    if (m_ruler == nullptr || top_window() != m_ruler->top_window())
        return;
    if (!m_precision) {
        m_ruler->hide();
        return;
    }
    RulerState st;
    st.value = value();
    st.minVal = minimum();
    st.maxVal = maximum();
    st.step = step();
    st.snapStep = effectiveSnap();
    st.pxPerUnit = m_pxPerUnit;
    st.precision01 = precision01(std::abs(Fl::event_y() - m_pushY), kDeadzonePx, kRampPx);
    st.snapping = (Fl::event_state() & FL_SHIFT) != 0;
    st.text = scrub_detail::format(value(), step(), m_suffix); // minimal decimals (integer if step>=1)
    m_ruler->present(this, st);
}

int ScrubSlider::handle(int event) {
    switch (event) {
    case FL_ENTER:
        m_hover = true;
        redraw(); // repaint the hover lift now -- without this the highlight lags / sticks after a drag
        return 1;
    case FL_LEAVE:
        m_hover = false;
        redraw(); // clear the hover lift immediately (else it stays painted until the next redraw)
        return 1;

    case FL_FOCUS:
        return m_editing ? 1 : 0; // only grab keyboard focus while editing (no Tab-stop otherwise)
    case FL_UNFOCUS:
        // Commit when focus genuinely leaves (clicking another focusable control / the canvas). But
        // our OWN right-click context menu also steals focus -- don't close the field then, or the
        // menu's Cut/Copy/Paste would act on an already-gone editor.
        if (m_editing && activeContextMenu() == nullptr)
            commitEdit();
        return 1;

    case FL_PASTE:
        if (m_editing) {
            editInsert(Fl::event_text());
            redraw();
            return 1;
        }
        return 0;

    case FL_KEYBOARD: {
        if (!m_editing)
            return 0;
        const int key = Fl::event_key();
        const bool cmd = (Fl::event_state() & FL_COMMAND) != 0;
        const bool shift = (Fl::event_state() & FL_SHIFT) != 0;
        const int len = static_cast<int>(m_editBuf.size());
        switch (key) {
        case FL_Enter:
        case FL_KP_Enter:
            commitEdit();
            return 1;
        case FL_Escape:
            cancelEdit();
            return 1;
        case FL_BackSpace:
            if (hasSelection())
                deleteSelection();
            else if (m_caret > 0) {
                m_editBuf.erase(static_cast<std::size_t>(--m_caret), 1);
                m_selAnchor = m_caret;
            }
            redraw();
            return 1;
        case FL_Delete:
            if (hasSelection())
                deleteSelection();
            else if (m_caret < len)
                m_editBuf.erase(static_cast<std::size_t>(m_caret), 1);
            redraw();
            return 1;
        case FL_Left:
            if (m_caret > 0)
                --m_caret;
            if (!shift)
                m_selAnchor = m_caret;
            m_caretOn = true;
            redraw();
            return 1;
        case FL_Right:
            if (m_caret < len)
                ++m_caret;
            if (!shift)
                m_selAnchor = m_caret;
            m_caretOn = true;
            redraw();
            return 1;
        case FL_Home:
            m_caret = 0;
            if (!shift)
                m_selAnchor = 0;
            redraw();
            return 1;
        case FL_End:
            m_caret = len;
            if (!shift)
                m_selAnchor = len;
            redraw();
            return 1;
        default:
            break;
        }
        if (cmd && key == 'a') {
            editSelectAll();
            redraw();
            return 1;
        }
        if (cmd && key == 'c') {
            editClipboard(/*cut=*/false);
            return 1;
        }
        if (cmd && key == 'x') {
            editClipboard(/*cut=*/true);
            redraw();
            return 1;
        }
        if (cmd && key == 'v') {
            Fl::paste(*this, 1);
            return 1;
        }
        if (!cmd) { // a typed character (filtered to the numeric set by editInsert)
            editInsert(Fl::event_text());
            m_caretOn = true;
            redraw();
        }
        return 1;
    }

    case FL_PUSH: {
        const int b = Fl::event_button();
        if (m_editing) {
            if (b == FL_RIGHT_MOUSE) {
                openValueContextMenu();
                return 1;
            }
            if (b == FL_LEFT_MOUSE) { // reposition the caret (a drag extends the selection)
                caretFromX(Fl::event_x());
                m_selAnchor = m_caret;
                m_caretOn = true;
                redraw();
            }
            return 1;
        }
        if (b == FL_RIGHT_MOUSE) { // right-click the value -> edit it + the themed Cut/Copy/Paste menu
            beginEdit();
            openValueContextMenu();
            return 1;
        }
        const bool resetGesture =
            (b == FL_MIDDLE_MOUSE) || (b == FL_LEFT_MOUSE && (Fl::event_state() & FL_CTRL));
        if (resetGesture) {
            if (m_hasDefault)
                pushAndDrag(m_default);
            return 1;
        }
        if (b != FL_LEFT_MOUSE)
            return 1;
        m_dragging = true;
        m_moved = false;
        m_precision = false;
        m_pushX = m_lastX = Fl::event_x();
        m_pushY = Fl::event_y();
        m_accum = value();
        handle_push();
        return 1;
    }

    case FL_DRAG: {
        if (m_editing) { // drag-select within the edit text
            caretFromX(Fl::event_x());
            m_caretOn = true;
            redraw();
            return 1;
        }
        if (!m_dragging)
            return 1;
        const int ex = Fl::event_x();
        const int ey = Fl::event_y();
        if (std::abs(ex - m_pushX) > kMoveThresh || std::abs(ey - m_pushY) > kMoveThresh)
            m_moved = true;

        const int dyAbs = std::abs(ey - m_pushY);
        const double p01 = precision01(dyAbs, kDeadzonePx, kRampPx);
        m_precision = p01 > 0.0;

        const double upp =
            unitsPerPixel(m_accum, m_curve, m_curveK, minimum(), maximum(), trackW());
        const double div = gainDivisor(p01, kMaxDivisor);
        const double gain = upp / div;       // value units per screen pixel right now
        m_accum += (ex - m_lastX) * gain;     // integrate relatively -- never jumps
        m_lastX = ex;
        const double lo = std::min(minimum(), maximum());
        const double hi = std::max(minimum(), maximum());
        m_accum = std::clamp(m_accum, lo, hi);
        m_pxPerUnit = (gain > 0.0) ? (1.0 / gain) : 0.0;

        const bool shift = (Fl::event_state() & FL_SHIFT) != 0;
        // Regular dragging lands on round values for easy whole-number picks; the fine (step) resolution
        // is unlocked by pulling into precision mode. A sub-integer step (e.g. a 0.5pt size) IS the round
        // grid, so its decimals are reachable on a normal drag too. Shift snaps to the coarse grid either way.
        const double coarse = step() < 1.0 ? step() : 1.0;
        const double grid = m_precision ? step() : coarse;
        const double v = shift ? quantize(snapTo(m_accum, effectiveSnap()), step(), minimum(), maximum())
                               : quantize(m_accum, grid, minimum(), maximum());
        handle_drag(v); // clamps + rounds to step() + fires the FL_WHEN_CHANGED callback
        updateRuler();
        redraw();
        return 1;
    }

    case FL_RELEASE: {
        if (!m_dragging)
            return 1;
        m_dragging = false;
        m_precision = false;
        if (m_ruler != nullptr && top_window() == m_ruler->top_window())
            m_ruler->hide(); // top_window (not window) so a panel/overflow slider hides it too -- else it sticks
        if (!m_moved)
            beginEdit();    // a click with no scrub -> type the value
        else
            handle_release(); // fires FL_WHEN_RELEASE
        redraw();
        return 1;
    }

    case FL_MOUSEWHEEL: {
        // FLTK's Fl_Group::handle(FL_MOUSEWHEEL) has a SECOND pass that offers the wheel to every
        // sibling NOT under the cursor once nothing under the cursor consumed it -- so a slider that
        // blindly returns 1 hijacks wheel events aimed at the surrounding chrome/labels/empty space
        // (grabbing the last slider in the group) AND swallows them from the enclosing Fl_Scroll. That
        // is the app-wide "scroll random-slider / content won't scroll unless right over the scrollbar"
        // bug. Only act when the cursor is genuinely over us; otherwise decline so the event falls
        // through to the scroll (or whatever is beneath), exactly as a wheel over blank chrome should.
        if (!Fl::event_inside(this))
            return 0;
        if (m_editing)
            return 1; // editing: the wheel must not scrub the value out from under the caret
        if (Fl::event_dx() != 0) // horizontal wheel: leave to scrolling
            return 0;
        const int dy = Fl::event_dy();
        if (dy == 0)
            return 0;
        // Scrolling yields WHOLE values: snap to the integer grid that brackets the current value in
        // the scroll direction, then step by `coarse`. (Shift = a coarser jump.)
        const double coarse = (Fl::event_state() & FL_SHIFT) ? 5.0 : 1.0;
        const double cur = value();
        const double whole = std::round(cur);
        double target = 0.0;
        if (dy < 0) // wheel up -> increase
            target = (whole > cur + 1e-9) ? whole : whole + coarse;
        else // wheel down -> decrease
            target = (whole < cur - 1e-9) ? whole : whole - coarse;
        pushAndDrag(target);
        return 1;
    }

    default:
        return Fl_Valuator::handle(event);
    }
}

void commitActiveScrubEditOnOutsideClick(int winX, int winY) {
    ScrubSlider* s = g_activeScrubEdit;
    if (s == nullptr)
        return;
    const bool inside = winX >= s->x() && winX < s->x() + s->w() && //
                        winY >= s->y() && winY < s->y() + s->h();
    if (!inside)
        s->commitEdit(); // a click on chrome / another control applies the typed value and closes
}

// ============================================================================================
//  ScrubRuler (the comic-book precision bubble -- a ui::Popover)
// ============================================================================================
namespace {
// Fixed footprint: the bubble is placed once and never resized/moved while open (Xwayland does not
// re-realise a shaped sub-window after show(); growing/nudging it left the visible window + triangle
// behind the redrawn content). The body height carries an extra few px so the labels get a bottom gap.
constexpr int kRulerBodyW = 188;
constexpr int kRulerBodyH = 66;
constexpr double kArcBow = 11.0; // how high the centre ticks lift -> a clearly curved fan
constexpr int kLabelPad = 4;     // min horizontal gap between adjacent tick labels (collision guard)

// A "nice" 1-2-5 step at or above `rough` (for tick spacing / label density).
double niceStep(double rough) {
    if (rough <= 0.0)
        return 1.0;
    const double mag = std::pow(10.0, std::floor(std::log10(rough)));
    const double n = rough / mag;
    const double nice = (n < 1.5) ? 1 : (n < 3) ? 2 : (n < 7) ? 5 : 10;
    return nice * mag;
}

// Decimals to print a tick label at the given unit: none for whole units, else just enough.
int labelDecimals(double unit) {
    if (unit >= 1.0)
        return 0;
    if (unit >= 0.1)
        return 1;
    return 2;
}
} // namespace

ScrubRuler::ScrubRuler() : Popover(kRulerBodyW, kRulerBodyH + Popover::kBubbleTri) {
    enableBubble(BubbleSide::Up); // points UP at the slider; backend-gated triangle + shape()
}

void ScrubRuler::present(const Fl_Widget* slider, const RulerState& st) {
    m_st = st;
    // Ease pxPerUnit toward its target so the tick LOD doesn't snap as the gain changes (esp. near
    // min/max with a Gamma curve). Seed it on the first frame of a drag.
    if (!m_live) {
        m_smoothPxPerUnit = st.pxPerUnit;
        m_live = true;
    } else if (st.pxPerUnit > 0.0) {
        m_smoothPxPerUnit += (st.pxPerUnit - m_smoothPxPerUnit) * 0.3;
    }
    // Cap the ruler ZOOM so it always shows ~6 steps of context. With the Gamma curve the gain (and so
    // pxPerUnit) explodes near the min, which scrolled the min tick -- and everything -- off the bubble
    // (user: "pull the ruler all the way off screen"). This bounds only the ruler's tick scale; the
    // value GAIN (fine control) is untouched.
    if (st.step > 0.0) {
        const double maxPpu = (kRulerBodyW - 16.0) / (6.0 * st.step);
        m_smoothPxPerUnit = std::min(m_smoothPxPerUnit, maxPpu);
    }
    // Pick the labelled-major unit HERE, with hysteresis: it only changes when its on-screen spacing
    // leaves a wide comfortable band, so the constant tiny vertical jitter of a real drag can't flip
    // the tick LOD (which made large-value labels flicker in and out). draw() just renders it.
    if (m_smoothPxPerUnit > 0.0) {
        constexpr double kTargetLabelPx = 42.0; // generous: labels never crowd, so each major can label
        const double want = std::max(niceStep(kTargetLabelPx / m_smoothPxPerUnit), st.step);
        if (m_lastTickUnit < st.step - 1e-12) {
            m_lastTickUnit = want; // first frame of this bubble
        } else {
            const double sp = m_lastTickUnit * m_smoothPxPerUnit;
            if (sp < 26.0 || sp > 92.0)
                m_lastTickUnit = want; // only re-pick when clearly out of band
        }
    }
    if (!shown()) { // place ONCE, then only the content updates (Xwayland-safe; see header)
        m_anchor = slider; // protected; place() reads it (no g_active churn)
        place();
        show();
    }
    redraw();
}

void ScrubRuler::hide() {
    m_live = false;
    m_lastTickUnit = 0.0; // fresh LOD next time the bubble appears
    Popover::hide();
}

void ScrubRuler::drawContent() {
    Popover::drawContent(); // the bubble chrome (panel + up-triangle), repainted on a full redraw
    const Palette& p = activePalette();

    const int bodyTop = bubbleActive() ? kBubbleTri : 0;
    const int cxr = w() / 2;
    const int margin = 8;
    const int bx = margin;
    const int bw = w() - 2 * margin;
    const int labelY = h() - 6;          // major labels, with a few px of bottom padding
    const int arcBaseY = h() - 22;       // tick centres ride a parabola lifting toward the middle

    // The value, large, near the top of the body (this is the only value shown while the bubble is up).
    fl_font(FL_HELVETICA_BOLD, 14);
    fl_color(toFl(p.text));
    const int vw = static_cast<int>(fl_width(m_st.text.c_str()));
    fl_draw(m_st.text.c_str(), cxr - vw / 2, bodyTop + 4 + (fl_height() - fl_descent()));

    const double ppu = m_smoothPxPerUnit;
    if (ppu > 0.0 && m_lastTickUnit > 0.0) {
        // The labelled-major unit was chosen with hysteresis in present() and is spaced generously, so
        // every major can carry a label without colliding -> no greedy on/off guard, hence no flicker.
        // Minors fill in between (unlabelled), never finer than the slider's own step.
        const double tickUnit = m_lastTickUnit;
        const double minorUnit = std::max(tickUnit / 5.0, m_st.step);
        const bool minorsDistinct = minorUnit < tickUnit - 1e-9;
        const double minorAlpha =
            minorsDistinct ? std::clamp((minorUnit * ppu - 3.0) / 7.0, 0.0, 1.0) : 0.0; // fade past ~3px
        const int dec = labelDecimals(tickUnit);
        // Cover the FULL half-width so a min/0 tick stays visible as long as it's within the bubble
        // (the inset I added before shrank coverage and dropped the "0" tick). Edge labels are kept
        // on-screen by clamping their x below instead of by shrinking the range.
        const double halfUnits = (bw / 2.0) / ppu;
        const double lo = std::max(m_st.minVal, m_st.value - halfUnits);
        const double hi = std::min(m_st.maxVal, m_st.value + halfUnits);

        const auto envAt = [&](int sxp) {
            const double dxn = (sxp - cxr) / (bw / 2.0);
            return 1.0 - dxn * dxn; // 1 at the centre, 0 at the edges
        };
        const auto tickMidY = [&](int sxp) {
            return arcBaseY - static_cast<int>(std::lround(kArcBow * envAt(sxp)));
        };
        const auto xOf = [&](double v) {
            return cxr + static_cast<int>(std::lround((v - m_st.value) * ppu));
        };

        // Minor subticks first (under the majors), if they are a distinct level and visible.
        if (minorsDistinct && minorAlpha > 0.03) {
            const double startMinor = std::ceil(lo / minorUnit - 1e-6) * minorUnit;
            fl_color(toFl(blend(p.panelBg, p.textMuted, minorAlpha)));
            for (double v = startMinor; v <= hi + 1e-6; v += minorUnit) {
                if (v < m_st.minVal - 1e-9 || v > m_st.maxVal + 1e-9)
                    continue;
                if (std::fabs(v / tickUnit - std::round(v / tickUnit)) < 0.01)
                    continue; // a major sits here; skip the minor
                const int sxp = xOf(v);
                if (sxp < bx || sxp > bx + bw)
                    continue;
                const int len = static_cast<int>(std::lround(5 * (0.55 + 0.45 * envAt(sxp))));
                const int my = tickMidY(sxp);
                fl_line(sxp, my - len / 2, sxp, my + len / 2);
            }
        }

        // Ticks come from three sources, with a priority so the right one wins a label collision:
        //   2 = the CURRENT VALUE (always dead-centre; the "you are here" tick -- accent + a touch
        //       taller; it REPLACES the old separate centre marker line, which used to hide a tick
        //       sitting under it and made the value look like it had snapped to the next round major);
        //   1 = the min / max endpoints (so a non-round limit like 0.5 always gets its own tick);
        //   0 = the round-number grid.
        fl_font(FL_HELVETICA, 9);
        const auto inView = [&](int sxp) { return sxp >= bx - 1 && sxp <= bx + bw + 1; };
        const auto labelStr = [&](double v) {
            if (v == 0.0)
                v = 0.0; // normalise -0.0 so a tick at zero never reads "-0"
            char b[24];
            std::snprintf(b, sizeof(b), "%.*f", dec, v);
            return std::string(b);
        };

        struct Cand {
            double v;
            int pri;
        };
        std::vector<Cand> cands;
        const auto addCand = [&](double v, int pri) { // merge into a near-coincident tick, keep max pri
            for (auto& c : cands)
                if (std::fabs(c.v - v) < tickUnit * 0.25) {
                    c.pri = std::max(c.pri, pri);
                    return;
                }
            cands.push_back({v, pri});
        };
        for (double v = std::ceil(lo / tickUnit - 1e-6) * tickUnit; v <= hi + 1e-6; v += tickUnit)
            if (v >= m_st.minVal - 1e-9 && v <= m_st.maxVal + 1e-9 && inView(xOf(v)))
                cands.push_back({v, 0});
        for (const double e : {m_st.minVal, m_st.maxVal})
            if (inView(xOf(e)))
                addCand(e, 1);
        addCand(m_st.value, 2); // always centred + in view

        for (const Cand& c : cands) {
            const int sxp = xOf(c.v);
            const bool snapTick = m_st.snapStep > 0.0 &&
                                  std::fabs(c.v / m_st.snapStep - std::round(c.v / m_st.snapStep)) < 0.01;
            const int len = static_cast<int>(std::lround((c.pri == 2 ? 14 : 11) * (0.5 + 0.5 * envAt(sxp))));
            const int my = tickMidY(sxp);
            fl_color(toFl(c.pri == 2                   ? p.accent
                          : (m_st.snapping && snapTick) ? p.accent
                                                        : p.text)); // vertical ticks -> no staircase
            fl_line_style(FL_SOLID, c.pri == 2 ? 2 : 1);
            fl_line(sxp, my - len / 2, sxp, my + len / 2);
        }
        fl_line_style(0);

        // Labels by priority (value, endpoints, round majors); each clamped inside the bubble and
        // skipped if it would overlap one already placed -- so the value/limit labels always win.
        std::vector<std::pair<int, int>> placed;
        const auto place = [&](double v) {
            const std::string t = labelStr(v);
            const int lw = static_cast<int>(fl_width(t.c_str()));
            const int lx = std::clamp(xOf(v) - lw / 2, bx, std::max(bx, bx + bw - lw));
            for (const auto& b : placed)
                if (!(lx + lw < b.first - 2 || lx > b.second + 2))
                    return;
            fl_color(toFl(p.textMuted));
            fl_draw(t.c_str(), lx, labelY);
            placed.emplace_back(lx, lx + lw);
        };
        for (int wantPri = 2; wantPri >= 0; --wantPri)
            for (const Cand& c : cands)
                if (c.pri == wantPri)
                    place(c.v);
    }
}

} // namespace mosaic::ui
