#include "ui/tool_options.hpp"

#include "ui/scrub_slider.hpp" // the options-bar value slider (replaces ui::Slider here)
#include "ui/theme.hpp"
#include "ui/tool.hpp"
#include "ui/widgets.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Float_Input.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mosaic::ui {
namespace {

constexpr int kCtlH = 22;     // control height
constexpr int kPadX = 12;     // left padding
constexpr int kLabelGap = 6;  // label -> control
constexpr int kScrubW = 150;  // ScrubSlider width (value drawn on the bar; wider than the combo)
constexpr int kChoiceW = 118; // dropdown width
constexpr int kFontControlW = 150; // font picker closed control (family names run long)
constexpr int kFontRowH = 30;      // font picker open-list row height (room for the in-face preview)
constexpr int kFontListW = 300;    // font picker open-list minimum width (name gutter + preview)
constexpr int kNumberW = 52;  // outlined numeric entry field width
constexpr int kGlyphW = kCtlH + 4; // a styled B/I/U/S toggle button (square-ish glyph cell)
constexpr int kGlyphGap = 2;  // a hair of space between flush B/I/U/S toggles so they don't kiss
constexpr int kGroupGap = 16; // between option groups
constexpr int kSepW = 18;     // centred separator zone for a joinPrev control (the "W : H" colon)
constexpr int kChevronW = 26; // the overflow ">>" button (S16-n)
constexpr int kPopPad = 8;    // inset around the overflow popover's rows
constexpr int kPopRowGap = 8; // vertical gap between overflow rows

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

// Map the FLTK-free option-model curve onto the widget's enum (the model can't name ui::ScrubCurve).
ScrubCurve toScrubCurve(ResponseCurve c) {
    switch (c) {
    case ResponseCurve::Gamma:
        return ScrubCurve::Gamma;
    case ResponseCurve::Log:
        return ScrubCurve::Log;
    case ResponseCurve::Linear:
        break;
    }
    return ScrubCurve::Linear;
}

// Per-control link back to its ToolOption: everything a callback needs, so the (file-local) thunks
// don't touch ToolOptionsBar internals.
struct Binding {
    ToolManager* tools = nullptr;
    std::size_t index = 0; // index into activeTool()->options()
    ToolOptionKind kind = ToolOptionKind::Slider;
    Fl_Widget* control = nullptr; // ScrubSlider / Dropdown / FlatButton (the ScrubSlider draws its
                                  // own value, so sliders no longer carry a separate readout box)
};

// Hover help for an options-bar control (and its caption): set at construction so the first hover
// already has it. A no-op for options that publish no tooltip.
void setTip(Fl_Widget* w, const ToolOption& opt) {
    if (w != nullptr && !opt.tooltip.empty())
        w->copy_tooltip(opt.tooltip.c_str());
}

// Lighten a colour toward white by t (0..1) -- the hover lift for a filled action button.
common::Color8 lighten(common::Color8 c, float t) {
    auto L = [t](std::uint8_t v) {
        return static_cast<std::uint8_t>(std::lround(v + (255.0f - v) * t));
    };
    return {L(c.r), L(c.g), L(c.b), c.a};
}

// The solid fill for an accented action button. Affirmative = the app accent (the emphasised
// "primary", e.g. crop Apply); Destructive = a danger red, reserved for genuinely destructive actions.
// A None-accent Button renders as a plain neutral FlatButton (e.g. crop Cancel). Returns a fully
// transparent colour for None to signal "no fill" to the caller.
common::Color8 accentFill(ToolAccent a, const Palette& pal) {
    switch (a) {
    case ToolAccent::Affirmative: return pal.accent;
    case ToolAccent::Destructive: return {198, 78, 72, 255}; // danger red
    default: return {0, 0, 0, 0};                            // none -> neutral FlatButton
    }
}

// A word-labelled toggle (the non-glyph Toggle kind: "Guides", "Delete Cropped Pixels", ...). It
// exists only to own its label's colour: FLTK's draw_label() greys an inactive label by blending it
// toward the background (fl_inactive), which lands somewhere other than the palette's textMuted --
// so a disabled word-toggle and the disabled Dropdown beside it read as two different greys. Here
// the label is drawn by hand with the Dropdown's exact rule (active_r() ? text : textMuted), which
// is also what GlyphButton now does; all three greys match. The box is Fl_Button's, untouched.
class LabelToggle : public FlatButton {
public:
    LabelToggle(int X, int Y, int W, int H) : FlatButton(X, Y, W, H) {}

protected:
    void draw() override {
        const Fl_Color fill = value() != 0 ? selection_color() : color();
        draw_box(value() != 0 ? (down_box() != 0 ? down_box() : fl_down(box())) : box(), fill);
        const Palette& p = activePalette();
        fl_color(toFl(active_r() ? p.text : p.textMuted));
        fl_font(labelfont(), labelsize());
        fl_draw(label(), x(), y(), w(), h(), FL_ALIGN_CENTER);
    }
};

// A solid colour-filled action button: a coloured fill + readable light text, brightening slightly on
// hover. Replaces the old 1-px coloured outline (it read cheap) plus the red-on-Cancel (semantically
// wrong -- Cancel isn't destructive). One emphasised primary (Apply) + a quiet neutral secondary
// (Cancel) is a stronger pattern than two equally-weighted coloured buttons (user 2026-06-14).
// NOT the shared ui::FilledButton (that one is always accent-filled): this takes an ARBITRARY
// fixed fill (a tool's accent colour) and luminance-picks its label -- hence the distinct name.
class TintedButton : public FlatButton {
public:
    TintedButton(int X, int Y, int W, int H, common::Color8 fill)
        : FlatButton(X, Y, W, H), m_fill(fill) {
        color(toFl(m_fill));
        selection_color(toFl(lighten(m_fill, 0.10f))); // pressed
        labelcolor(toFl(onColorFor(m_fill)));
    }

protected:
    int handle(int event) override {
        switch (event) {
        case FL_ENTER:
            color(toFl(lighten(m_fill, 0.12f)));
            redraw();
            return 1;
        case FL_LEAVE:
            color(toFl(m_fill));
            redraw();
            return 1;
        default:
            return Fl_Button::handle(event); // NOT FlatButton::handle (that resets to controlBg/hover)
        }
    }

    // This button has a FIXED fill (not a palette colour), so on a re-theme restore m_fill -- never
    // the base's semantic controlBg. Un-freezes a baked hover colour the same way. Chains to the
    // base first per the FlatButton::reapplyTheme contract, then puts BOTH our fills back: the tint
    // and its pressed lightening are derived from m_fill, not from the palette, so the base's
    // controlActive would be wrong here.
    void reapplyTheme() override {
        FlatButton::reapplyTheme();
        color(toFl(m_fill));
        selection_color(toFl(lighten(m_fill, 0.10f)));
        labelcolor(toFl(onColorFor(m_fill)));
        redraw();
    }

private:
    // A readable label colour over the fill, luminance-picked (theme onAccent is light anyway).
    static common::Color8 onColorFor(common::Color8 fill) {
        const float lum = (0.299f * fill.r + 0.587f * fill.g + 0.114f * fill.b) / 255.0f;
        return lum > 0.6f ? common::Color8{20, 20, 20, 255} : common::Color8{245, 245, 245, 255};
    }
    common::Color8 m_fill;
};

// The numeric entry / its format + parse moved to the shared ui::NumberField (widgets.hpp) --
// user 2026-07-15: every value field in the app must read identically. formatNumber/parseNumber
// below are thin wrappers over the shared helpers, kept for the option-shaped call sites.
std::string formatNumber(const ToolOption& opt) {
    return formatFieldNumber(opt.value, opt.step);
}

bool parseNumber(const char* text, double& out) {
    return parseFieldNumber(text, out);
}

// Resolve a binding's live ToolOption (the active tool's), or nullptr if it no longer applies.
ToolOption* optionFor(const Binding& b) {
    Tool* t = b.tools != nullptr ? b.tools->activeTool() : nullptr;
    if (t == nullptr || b.index >= t->options().size())
        return nullptr;
    return &t->options()[b.index];
}

void cbSlider(Fl_Widget* w, void* u) {
    auto* b = static_cast<Binding*>(u);
    if (ToolOption* opt = optionFor(*b)) {
        opt->value = static_cast<ScrubSlider*>(w)->value();
        b->tools->notifyOptionsChanged();
    }
}
void cbChoice(Fl_Widget* w, void* u) {
    auto* b = static_cast<Binding*>(u);
    if (ToolOption* opt = optionFor(*b)) {
        opt->value = static_cast<Dropdown*>(w)->value();
        b->tools->notifyOptionsChanged();
    }
}
void cbToggle(Fl_Widget* w, void* u) {
    auto* b = static_cast<Binding*>(u);
    if (ToolOption* opt = optionFor(*b)) {
        opt->value = static_cast<FlatButton*>(w)->value() != 0 ? 1.0 : 0.0;
        b->tools->notifyOptionsChanged();
    }
}
void cbButton(Fl_Widget* /*w*/, void* u) {
    auto* b = static_cast<Binding*>(u);
    if (const ToolOption* opt = optionFor(*b))
        b->tools->notifyAction(opt->id); // momentary: fire the action, store nothing
}
void cbNumber(Fl_Widget* w, void* u) {
    auto* b = static_cast<Binding*>(u);
    if (ToolOption* opt = optionFor(*b)) {
        double v = 0.0;
        if (parseNumber(static_cast<Fl_Input*>(w)->value(), v))
            opt->value = std::clamp(v, opt->min, opt->max);
        b->tools->notifyOptionsChanged();
    }
}

// The overflow affordance (S16-n): a drawn double-chevron ">>" button pinned at the bar's right that
// toggles the overflow popover. Drawn with fl_line (host-font rule -- never a Unicode glyph), like the
// toolbar's variant triangle. Re-click closes (the popover's outside-click dismissal spares its
// anchor, so each anchor closes its own).
class OverflowChevron : public Fl_Widget {
public:
    OverflowChevron(int X, int Y, int W, int H, OptionsOverflowPopover* pop)
        : Fl_Widget(X, Y, W, H), m_pop(pop) {
        copy_tooltip("More options");
    }

protected:
    void draw() override {
        const Palette& pal = activePalette();
        const bool enabled = active_r(); // greyed with the rest of the chrome during an inpaint run
        fl_color(toFl(enabled && m_hover ? pal.controlHover : pal.panelBg));
        fl_rectf(x(), y(), w(), h());
        fl_color(toFl(enabled ? pal.text : pal.textMuted));
        const int midY = y() + h() / 2;
        constexpr int arm = 4; // half-height / width of one chevron
        // 2px strokes with round caps + joins -- a 1px chevron reads as pixel art; each ">" is one
        // polyline so the join() rounds its vertex rather than butting two segments.
        fl_line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, 2);
        for (int k = 0; k < 2; ++k) {
            const int bx = x() + w() / 2 - 5 + k * 5; // two stacked ">" 5px apart
            fl_begin_line();
            fl_vertex(bx, midY - arm);
            fl_vertex(bx + arm, midY);
            fl_vertex(bx, midY + arm);
            fl_end_line();
        }
        fl_line_style(0); // reset: line style is global FLTK draw state
    }

    int handle(int event) override {
        switch (event) {
        case FL_ENTER:
            m_hover = true;
            redraw();
            return 1; // claim ENTER so FLTK also delivers LEAVE
        case FL_LEAVE:
            m_hover = false;
            redraw();
            return 1;
        case FL_PUSH:
            return 1; // act on release
        case FL_RELEASE:
            if (m_pop != nullptr) {
                if (m_pop->shownFor(this))
                    m_pop->hide();
                else
                    m_pop->showAnchored(this); // already populated by the last rebuild()
            }
            return 1;
        default:
            return Fl_Widget::handle(event);
        }
    }

private:
    OptionsOverflowPopover* m_pop;
    bool m_hover = false;
};

// (The shape tools' paint-preview swatch lived here until S26-c. It previewed which of the fg/bg
// swatches the "paint" option's Fill / Outline / Fill+Outline mode would apply -- and that option
// is gone: the Shape tool authors a fill, and an outline is a Stroke layer effect, edited in the
// Layer Effects modal where its own colour control lives. With no paint mode there is nothing for a
// paint-mode preview to show, so the swatch went with it.)

} // namespace

struct ToolOptionsBar::State {
    std::vector<std::unique_ptr<Binding>> bindings;
    Fl_Widget* designerButton = nullptr;  // the "Edit shape…" action button (null otherwise)
    Fl_Widget* typePanelButton = nullptr; // the Type tool's "Style…" action button (null otherwise)
    Fl_Widget* type3dButton = nullptr;    // ... and its "3D…" sibling (the 3D popup anchor)
    Fl_Widget* gradientStopsButton = nullptr; // the Gradient tool's "Stops…" button (S22; null else)
};

OptionsOverflowPopover::OptionsOverflowPopover() : Popover(10, 10) {
    enableBubble(BubbleSide::Up); // an up-pointing comic-book pointer aimed at the bar's overflow chevron
}

void OptionsOverflowPopover::beginRebuild(int w, int h) {
    setBaseSize(w, h);
    resize(x(), y(), w, h); // size the window now so children lay out against the right footprint
    clear();                // drop the previous tool's overflow rows (and their dead callbacks)
    begin();
}

void OptionsOverflowPopover::endRebuild() {
    end();
    resizable(nullptr); // rows are fixed; clear() reset this to the group
}

ToolOptionsBar::ToolOptionsBar(int X, int Y, int W, int H, ToolManager& tools)
    : Panel(X, Y, W, H), m_tools(tools), m_state(std::make_unique<State>()) {
    borderEdges(EdgeTop | EdgeBottom); // the bar owns the menu|bar and bar|body junctions
    end();
    // Controls are laid out left-aligned at fixed sizes; don't let a window-widen stretch them.
    resizable(nullptr);
    rebuild();
}

ToolOptionsBar::~ToolOptionsBar() = default;

void ToolOptionsBar::reapplyTheme() {
    Panel::reapplyTheme(); // re-fill the strip ground
    rebuild();             // recreate the controls so they bake the new palette
}

void ToolOptionsBar::rebuild() {
    clear(); // delete the previous controls (incl. the chevron)...
    m_state->bindings.clear(); // ...then their bindings (widgets are gone; no callbacks pending)
    m_state->designerButton = nullptr;
    m_state->typePanelButton = nullptr;
    m_state->type3dButton = nullptr;
    m_state->gradientStopsButton = nullptr;
    if (m_overflow != nullptr) {
        if (m_overflow->shown())
            m_overflow->hide(); // a stale list must not outlive the controls about to be cleared
        m_overflow->beginRebuild(m_overflow->w(), m_overflow->h()); // clear() drops the old rows +
        m_overflow->endRebuild();                                   // their now-dangling callbacks
    }

    Tool* tool = m_tools.activeTool();
    if (tool == nullptr) {
        redraw();
        return;
    }

    const Palette& pal = activePalette();
    const int cy = y() + (h() - kCtlH) / 2;
    begin();
    fl_font(FL_HELVETICA, 12); // so fl_width() measures the label font we use below

    const auto labelW = [](const ToolOption& opt) {
        return static_cast<int>(std::ceil(fl_width(opt.label.c_str()))) + 2;
    };
    // Width of an option's left caption: a joinPrev option draws a centred separator zone instead;
    // an empty label takes no room at all (no caption box, no gap -- so a field like crop's ratioW
    // sits flush, evening the margins between the Ratio combo, the custom fields and the toggles).
    const auto capW = [&](const ToolOption& opt) -> int {
        if (opt.joinPrev)
            return kSepW;
        return opt.label.empty() ? 0 : labelW(opt) + kLabelGap;
    };
    const auto needW = [&](const ToolOption& opt) -> int {
        switch (opt.kind) {
        case ToolOptionKind::Slider:
            return capW(opt) + kScrubW;
        case ToolOptionKind::Choice:
            return capW(opt) + kChoiceW;
        case ToolOptionKind::Font:
            return capW(opt) + kFontControlW; // wider closed control (family names run long)
        case ToolOptionKind::Toggle:
            return opt.glyph != ToolGlyph::None
                       ? kGlyphW + kGlyphGap  // styled B/I/U/S: the button + a hair of trailing gap
                       : labelW(opt) + 20;    // text label drawn on the button
        case ToolOptionKind::Button:
            return labelW(opt) + 24;
        case ToolOptionKind::Number:
            return capW(opt) + kNumberW;
        }
        return 0;
    };

    // Create `opt`'s controls at (startX, rowY) and record its binding (caption left of the control;
    // toggles/buttons carry their own label). Used for both the horizontal bar (rowY = the bar's
    // centred cy) and the overflow popover (rowY = each stacked row's top).
    const auto emit = [&](const ToolOption& opt, std::size_t i, int startX, int rowY) {
        auto binding = std::make_unique<Binding>();
        binding->tools = &m_tools;
        binding->index = i;
        binding->kind = opt.kind;
        int cx = startX;
        if (opt.kind != ToolOptionKind::Toggle && opt.kind != ToolOptionKind::Button) {
            if (opt.joinPrev) {
                // The label is a centred, enlarged SEPARATOR between this field and the previous
                // control (e.g. the crop custom ratio's colon), not a left caption (S16-l). The
                // colon's ink sits low in the cell, so raise the box a couple px to optically
                // centre it between the two fields (S16-k feedback).
                auto* sep = new Fl_Box(cx, rowY - 2, kSepW, kCtlH);
                sep->copy_label(opt.label.c_str());
                sep->box(FL_NO_BOX);
                sep->labelfont(FL_HELVETICA);
                sep->labelsize(16);
                sep->labelcolor(toFl(pal.textMuted));
                sep->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
                setTip(sep, opt);
                cx += kSepW;
            } else if (!opt.label.empty()) {
                auto* label = new Fl_Box(cx, rowY,labelW(opt), kCtlH);
                label->copy_label(opt.label.c_str());
                label->box(FL_NO_BOX);
                label->labelfont(FL_HELVETICA);
                label->labelsize(12);
                label->labelcolor(toFl(pal.textMuted));
                label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
                setTip(label, opt);
                cx += labelW(opt) + kLabelGap;
            }
            // An empty label with no join takes no caption space at all.
        }
        switch (opt.kind) {
        case ToolOptionKind::Slider: {
            auto* s = new ScrubSlider(cx, rowY, kScrubW, kCtlH);
            s->range(opt.min, opt.max);
            s->step(opt.step);
            s->value(opt.value);
            s->setSuffix(opt.suffix);
            s->setResponseCurve(toScrubCurve(opt.curve), opt.curveK);
            s->setSnapStep(opt.snapStep);
            if (opt.hasDefault)
                s->setDefaultValue(opt.defaultValue);
            s->setRuler(m_scrubRuler);
            s->when(FL_WHEN_CHANGED);
            s->callback(cbSlider, binding.get());
            binding->control = s;
            setTip(s, opt);
            break;
        }
        case ToolOptionKind::Choice: {
            auto* d = new Dropdown(cx, rowY, kChoiceW, kCtlH);
            for (const std::string& c : opt.choices)
                d->add(c.c_str());
            for (const int di : opt.disabledChoices) // greyed + unpickable (see ToolOption)
                if (di >= 0 && di < d->size() - 1)
                    d->mode(di, d->mode(di) | FL_MENU_INACTIVE);
            d->value(static_cast<int>(opt.value));
            d->callback(cbChoice, binding.get());
            binding->control = d;
            setTip(d, opt);
            break;
        }
        case ToolOptionKind::Font: {
            // A Choice over the family list (value = index), but the open list renders each family in
            // its own face -- taller rows, a wider list, per-row previews from the host (S29-c §8).
            auto* d = new Dropdown(cx, rowY, kFontControlW, kCtlH);
            for (const std::string& c : opt.choices)
                d->add(c.c_str());
            d->value(static_cast<int>(opt.value));
            d->callback(cbChoice, binding.get());
            if (m_fontPreview) {
                d->setRowHeight(kFontRowH);
                d->setListMinWidth(kFontListW);
                const ToolOption* o = &opt; // stable: a tool option outlives the controls it builds
                d->setRowPreview([this, o](int i, int cw, int ch) -> Fl_RGB_Image* {
                    if (i < 0 || i >= static_cast<int>(o->choices.size()))
                        return nullptr;
                    return m_fontPreview(o->choices[static_cast<std::size_t>(i)], cw, ch);
                });
                if (m_fontHoverPreview) // map the hovered row to its family (or "" on leave) for the host
                    d->setHoverPreview([this, o](int i) {
                        m_fontHoverPreview(i >= 0 && i < static_cast<int>(o->choices.size())
                                               ? o->choices[static_cast<std::size_t>(i)]
                                               : std::string());
                    });
            }
            binding->control = d;
            setTip(d, opt);
            break;
        }
        case ToolOptionKind::Toggle: {
            FlatButton* btn = nullptr;
            if (opt.glyph != ToolGlyph::None) { // styled B/I/U/S: a compact square glyph toggle
                using K = GlyphButton::Kind;
                const K kind = opt.glyph == ToolGlyph::Bold      ? K::Bold
                               : opt.glyph == ToolGlyph::Italic  ? K::Italic
                               : opt.glyph == ToolGlyph::Underline ? K::Underline
                                                                   : K::Strike;
                btn = new GlyphButton(cx, rowY, kGlyphW, kCtlH, kind); // no label; kGlyphGap trails it
            } else {
                btn = new LabelToggle(cx, rowY, labelW(opt) + 20, kCtlH);
                btn->copy_label(opt.label.c_str());
                btn->labelsize(12);
            }
            btn->type(FL_TOGGLE_BUTTON);
            btn->value(opt.value != 0.0 ? 1 : 0);
            btn->callback(cbToggle, binding.get());
            binding->control = btn;
            setTip(btn, opt);
            break;
        }
        case ToolOptionKind::Button: {
            const common::Color8 fill = accentFill(opt.accent, pal);
            FlatButton* btn = (fill.a != 0)
                                  ? new TintedButton(cx, rowY, labelW(opt) + 24, kCtlH, fill)
                                  : new FlatButton(cx, rowY, labelW(opt) + 24, kCtlH);
            btn->copy_label(opt.label.c_str());
            btn->labelsize(12);
            btn->callback(cbButton, binding.get());
            binding->control = btn;
            if (opt.id == "designer") // the shape-designer popover anchors to this button
                m_state->designerButton = btn;
            else if (opt.id == "typePanel") // the Type panel popover anchors to this "Style…" button
                m_state->typePanelButton = btn;
            else if (opt.id == "type3d") // the 3D popup anchors to this "3D…" button (S30-d)
                m_state->type3dButton = btn;
            else if (opt.id == "stops") // the gradient flyout anchors to this "Stops…" button (S22)
                m_state->gradientStopsButton = btn;
            setTip(btn, opt);
            break;
        }
        case ToolOptionKind::Number: {
            auto* in = new NumberField(cx, rowY, kNumberW, kCtlH); // the shared self-styled field
            in->when(FL_WHEN_CHANGED);
            in->value(formatNumber(opt).c_str());
            in->callback(cbNumber, binding.get());
            binding->control = in;
            setTip(in, opt);
            break;
        }
        }
        if (!opt.enabled && binding->control != nullptr)
            binding->control->deactivate(); // a reserved control (e.g. the Type bar's "3D…"): greyed
        m_state->bindings.push_back(std::move(binding));
    };

    // Action Buttons (Apply/Cancel) are pinned to the RIGHT so they never scroll off and stay where
    // the eye expects a commit; everything else fills from the left, overflowing into the popover.
    const std::vector<ToolOption>& opts = tool->options();

    // Right-anchored action buttons: primary Buttons EXCEPT inlineFlow ones (those flow with the
    // left-fill controls, e.g. the Type bar's "Style…" / "3D…", §8.2).
    const auto rightAnchored = [](const ToolOption& opt) {
        return opt.primary && opt.kind == ToolOptionKind::Button && !opt.inlineFlow;
    };
    int rightReserve = 0;
    for (const ToolOption& opt : opts)
        if (rightAnchored(opt))
            rightReserve += needW(opt) + kGroupGap;

    // Left-fill GROUPS: a base primary option + any immediately following joinPrev ones (e.g. crop's
    // "W : H"), kept together so a group never splits across the bar / overflow boundary. An inlineFlow
    // Button (Style…/3D…) joins the left fill as its own group; only right-anchored Buttons are excluded.
    std::vector<std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < opts.size(); ++i) {
        const ToolOption& opt = opts[i];
        if (!opt.primary || rightAnchored(opt))
            continue;
        if (opt.joinPrev && !groups.empty())
            groups.back().push_back(i);
        else
            groups.push_back({i});
    }
    const auto groupW = [&](const std::vector<std::size_t>& g) {
        int sum = 0;
        for (const std::size_t idx : g)
            sum += needW(opts[idx]);
        return sum;
    };
    // How many leading groups fit left of `rightBoundary` (groups stay whole; a gap precedes each).
    const auto fitCount = [&](int rightBoundary) {
        int gx = x() + kPadX;
        std::size_t n = 0;
        for (const std::vector<std::size_t>& g : groups) {
            const int gap = (n == 0) ? 0 : kGroupGap;
            if (gx + gap + groupW(g) > rightBoundary)
                break;
            gx += gap + groupW(g);
            ++n;
        }
        return n;
    };

    const int rightNoChevron = x() + w() - kPadX - rightReserve;
    // If everything fits, no chevron; otherwise reserve it -- a kGroupGap on EACH side (the left-fill
    // controls before it, the action buttons after it, both spaced like the buttons are to each other).
    const bool overflow = m_overflow != nullptr && fitCount(rightNoChevron) < groups.size();
    const int chevronX = rightNoChevron - kGroupGap - kChevronW; // gap to the action buttons at its right
    const int rightBoundary = overflow ? chevronX - kGroupGap : rightNoChevron;
    const std::size_t fit = fitCount(rightBoundary);


    int cx = x() + kPadX; // emit the fitting groups left-to-right on the bar
    for (std::size_t gi = 0; gi < fit; ++gi) {
        cx += (gi == 0) ? 0 : kGroupGap;
        for (const std::size_t idx : groups[gi]) {
            emit(opts[idx], idx, cx, cy);
            cx += needW(opts[idx]);
        }
    }

    if (overflow) // the chevron, a kGroupGap left of the action-button reserve
        new OverflowChevron(chevronX, cy, kChevronW, kCtlH, m_overflow);

    int rx = x() + w() - kPadX - rightReserve; // right-anchored action buttons (never overflow)
    for (std::size_t i = 0; i < opts.size(); ++i) {
        const ToolOption& opt = opts[i];
        if (!rightAnchored(opt))
            continue;
        emit(opt, i, rx, cy);
        rx += needW(opt) + kGroupGap;
    }
    end();
    resizable(nullptr); // clear() above reset resizable() to the group; keep our fixed layout

    // Populate the overflow popover with the groups that didn't fit -- a vertical stack, each group
    // one horizontal row (so a joinPrev trio stays intact). Their bindings joined m_state->bindings in
    // emit(), so syncValues() drives both surfaces; the chevron shows it on demand.
    if (overflow) {
        int rowW = 0;
        for (std::size_t gi = fit; gi < groups.size(); ++gi)
            rowW = std::max(rowW, groupW(groups[gi]));
        const int rows = static_cast<int>(groups.size() - fit);
        // Reserve the up-pointing pointer's top margin (backend-gated, like the flyout/shape designer).
        const int by = Popover::bubbleSupported() ? Popover::kBubbleTri : 0;
        m_overflow->beginRebuild(rowW + 2 * kPopPad,
                                 by + 2 * kPopPad + rows * kCtlH + (rows - 1) * kPopRowGap);
        int ry = by + kPopPad;
        for (std::size_t gi = fit; gi < groups.size(); ++gi) {
            int rcx = kPopPad;
            for (const std::size_t idx : groups[gi]) {
                emit(opts[idx], idx, rcx, ry);
                rcx += needW(opts[idx]);
            }
            ry += kCtlH + kPopRowGap;
        }
        m_overflow->endRebuild();
    }
    redraw();
}

void ToolOptionsBar::setOverflowPopover(OptionsOverflowPopover* popover) {
    m_overflow = popover;
    rebuild(); // controls that don't fit move into it now (and on every later rebuild)
}

void ToolOptionsBar::setScrubRuler(ScrubRuler* ruler) {
    m_scrubRuler = ruler;
    rebuild(); // hand the ruler to each slider as it (re)builds
}

void ToolOptionsBar::setFontPreview(
    std::function<Fl_RGB_Image*(const std::string&, int, int)> cb) {
    m_fontPreview = std::move(cb);
    rebuild(); // a Font-kind control now wires its open-list preview to the provider
}

void ToolOptionsBar::setFontHoverPreview(std::function<void(const std::string&)> cb) {
    m_fontHoverPreview = std::move(cb);
    rebuild(); // a Font-kind control now wires its open-list hover preview to the provider
}

Fl_Widget* ToolOptionsBar::designerButton() const { return m_state->designerButton; }

Fl_Widget* ToolOptionsBar::typePanelButton() const { return m_state->typePanelButton; }
Fl_Widget* ToolOptionsBar::type3dButton() const { return m_state->type3dButton; }
Fl_Widget* ToolOptionsBar::gradientStopsButton() const { return m_state->gradientStopsButton; }

void ToolOptionsBar::syncValues() {
    // An option value changed on the twin surface (S11-c): push the live values back into our
    // controls without firing their callbacks (FLTK value setters don't).
    for (const std::unique_ptr<Binding>& b : m_state->bindings) {
        const ToolOption* opt = optionFor(*b);
        if (opt == nullptr || b->control == nullptr)
            continue;
        // A dynamically-gated control (the Crop bar's "Recompose" offer) re-syncs its enabled
        // state — rebuild() only bakes it at construction. BEFORE the focus skip: greying must
        // land even on the control that was just clicked (deactivate drops its focus itself).
        if (opt->enabled != (b->control->active() != 0)) {
            if (opt->enabled)
                b->control->activate();
            else
                b->control->deactivate();
            b->control->redraw();
        }
        // Never push a value back into the control the user is editing right now: a Number field
        // fires FL_WHEN_CHANGED per keystroke, and re-formatting its text mid-entry would eat a
        // half-typed decimal ("1." -> "1"). The value is already committed to the option; the
        // display re-syncs once focus leaves. (Harmless to skip for the other kinds too.)
        if (Fl::focus() == b->control)
            continue;
        switch (b->kind) {
        case ToolOptionKind::Slider:
            static_cast<ScrubSlider*>(b->control)->value(opt->value); // draws its own value
            break;
        case ToolOptionKind::Choice:
        case ToolOptionKind::Font:
            static_cast<Dropdown*>(b->control)->value(static_cast<int>(opt->value));
            break;
        case ToolOptionKind::Toggle:
            static_cast<FlatButton*>(b->control)->value(opt->value != 0.0 ? 1 : 0);
            break;
        case ToolOptionKind::Number:
            static_cast<Fl_Float_Input*>(b->control)->value(formatNumber(*opt).c_str());
            break;
        case ToolOptionKind::Button:
            break; // momentary: no VALUE to sync (enabled synced above)
        }
        b->control->redraw();
    }
    redraw();
}

void ToolOptionsBar::resize(int X, int Y, int W, int H) {
    // Set OUR geometry only -- do NOT let Fl_Group resize/scale the child controls. rebuild()'s
    // clear() resets resizable() to the group, so the default behaviour stretches the fixed-size
    // controls as the window widens (the giant-controls-on-maximize bug). On a width change, rebuild
    // so the controls re-lay-out left-aligned at their fixed sizes and the overflow-drop re-applies.
    const bool widthChanged = (W != w());
    Fl_Widget::resize(X, Y, W, H);
    if (widthChanged)
        rebuild();
}

} // namespace mosaic::ui
