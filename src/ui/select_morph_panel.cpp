#include "ui/select_morph_panel.hpp"

#include "common/i18n.hpp"
#include "ui/scrub_slider.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/fl_draw.H>

#include <memory>
#include <utility>
#include <vector>

namespace mosaic::ui {

namespace {

constexpr int kPanelW = 250;
constexpr int kPad = 14;
constexpr int kHeaderH = 42;
constexpr int kRowH = 26;
constexpr int kRowGap = 8;
constexpr int kFooterH = 44;
constexpr int kFieldW = 150;
constexpr int kBtnW = 80;
constexpr int kBtnH = 28;

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

// A control's binding: the closure to run when it fires (FLTK callbacks are C thunks + a void*),
// mirroring the adjustment panel's pattern.
struct Binding {
    std::function<void()> fn;
};
void controlThunk(Fl_Widget*, void* b) {
    if (auto* bb = static_cast<Binding*>(b))
        bb->fn();
}

} // namespace

struct SelectMorphPanel::State {
    Dropdown* mode = nullptr;
    ScrubSlider* amount = nullptr;
    std::vector<std::unique_ptr<Binding>> bindings;
};

SelectMorphPanel::SelectMorphPanel()
    : Popover(kPanelW, 100), m_state(std::make_unique<State>()) {
    setPinned(true); // survives canvas/chrome clicks, like the Type panel; Esc still closes
    end();           // Popover's ctor leaves the group open; build() manages its own begin/end
    build();
}

SelectMorphPanel::~SelectMorphPanel() = default;

void SelectMorphPanel::setPlacementProviders(std::function<common::Rect()> region) {
    setCornerPlacement(Corner::BottomRight, std::move(region));
}

void SelectMorphPanel::setScrubRuler(ScrubRuler* r) {
    m_ruler = r;
    if (m_state->amount != nullptr) // build() ran in the ctor; push onto the live slider too
        m_state->amount->setRuler(r);
}

void SelectMorphPanel::build() {
    const Palette& pal = activePalette();
    clear();            // drop the previous controls (a theme rebuild)
    resizable(nullptr); // clear() re-arms proportional scaling; this layout is fixed (the LE rule)
    m_state->mode = nullptr;
    m_state->amount = nullptr;
    m_state->bindings.clear();

    const int contentH = kHeaderH + 2 * (kRowH + kRowGap) + kFooterH;
    setBaseSize(kPanelW, contentH);

    begin();

    const auto bind = [&](std::function<void()> fn) {
        auto b = std::make_unique<Binding>();
        b->fn = std::move(fn);
        Binding* raw = b.get();
        m_state->bindings.push_back(std::move(b));
        return raw;
    };
    const int fieldLeft = kPanelW - kPad - kFieldW;
    const int capW = fieldLeft - kPad - 8;
    const auto caption = [&](int cy, const char* text) {
        auto* c = new Fl_Box(kPad, cy, capW, kRowH, text);
        c->box(FL_NO_BOX);
        c->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        c->labelsize(12);
        c->labelcolor(toFl(pal.text));
    };

    // ---- Header ----
    auto* head = new Fl_Box(kPad, 12, kPanelW - 2 * kPad, 20, _("Modify Selection"));
    head->box(FL_NO_BOX);
    head->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    head->labelfont(FL_HELVETICA_BOLD);
    head->labelsize(13);
    head->labelcolor(toFl(pal.text));

    int cy = kHeaderH;

    // ---- Mode dropdown (Grow / Shrink / Feather / Smooth) ----
    caption(cy, _("Mode"));
    auto* mode = new Dropdown(fieldLeft, cy, kFieldW, kRowH);
    mode->add(_("Grow"), 0, nullptr, nullptr, 0);   // plain words -- no '/' path-parsing risk
    mode->add(_("Shrink"), 0, nullptr, nullptr, 0);
    mode->add(_("Feather"), 0, nullptr, nullptr, 0);
    mode->add(_("Smooth"), 0, nullptr, nullptr, 0);
    mode->value(static_cast<int>(m_mode));
    mode->callback(controlThunk, bind([this] {
                       if (m_syncing || m_state->mode == nullptr)
                           return;
                       m_mode = static_cast<SelectMorphMode>(m_state->mode->value());
                       if (m_onPreview)
                           m_onPreview();
                   }));
    m_state->mode = mode;
    cy += kRowH + kRowGap;

    // ---- Amount slider (px / radius) ----
    caption(cy, _("Amount"));
    auto* amount = new ScrubSlider(fieldLeft, cy, kFieldW, kRowH);
    amount->range(0.0, 1000.0); // 0 = identity (no change); the modal prompt clamped [1,1000]
    amount->step(1.0);
    amount->setSuffix(_("px"));
    amount->setCellColor(pal.panelBg);
    amount->setDefaultValue(4.0);
    amount->setRuler(m_ruler);
    amount->value(m_amount);
    amount->when(FL_WHEN_CHANGED);
    amount->callback(controlThunk, bind([this] {
                         if (m_syncing || m_state->amount == nullptr)
                             return;
                         m_amount = m_state->amount->value();
                         if (m_onPreview)
                             m_onPreview();
                     }));
    m_state->amount = amount;
    cy += kRowH + kRowGap;

    // ---- Footer: Cancel + Apply (right-aligned) ----
    const int btnY = cy + (kFooterH - kBtnH) / 2 - 2;
    const int applyX = kPanelW - kPad - kBtnW;
    const int cancelX = applyX - 8 - kBtnW;
    auto* cancel = new FlatButton(cancelX, btnY, kBtnW, kBtnH, _("Cancel"));
    cancel->callback(controlThunk, bind([this] {
                         if (m_onCancel)
                             m_onCancel();
                     }));
    auto* apply = new FlatButton(applyX, btnY, kBtnW, kBtnH, _("Apply"));
    apply->callback(controlThunk, bind([this] {
                        if (m_onApply)
                            m_onApply();
                    }));

    end();
}

void SelectMorphPanel::pushStateToControls() {
    m_syncing = true;
    if (m_state->mode != nullptr)
        m_state->mode->value(static_cast<int>(m_mode));
    if (m_state->amount != nullptr)
        m_state->amount->value(m_amount);
    m_syncing = false;
}

void SelectMorphPanel::configure(SelectMorphMode mode, double amount) {
    m_mode = mode;
    m_amount = amount;
    pushStateToControls();
}

void SelectMorphPanel::openFor(const Fl_Widget* anchor) {
    showAnchored(anchor); // corner placement drives geometry; the anchor is for bookkeeping
}

void SelectMorphPanel::reapplyTheme() {
    Popover::reapplyTheme();
    build(); // rebuild the controls in the new palette (build() seeds them from m_mode/m_amount)
}

int SelectMorphPanel::handle(int event) {
    // Enter commits (like the old modal's OK). Delegate to the base FIRST so a focused child -- the
    // amount slider's in-place type-in -- consumes its own Enter; only when nothing did do we Apply.
    // Esc falls through to Popover::handle, which hides us; the host restores the canvas selection
    // when it sees the panel closed (updateSelectMorph).
    if (event == FL_KEYBOARD || event == FL_SHORTCUT) {
        const int k = Fl::event_key();
        if (k == FL_Enter || k == FL_KP_Enter) {
            if (Popover::handle(event))
                return 1; // a child (the type-in) took it
            if (m_onApply)
                m_onApply();
            return 1;
        }
    }
    return Popover::handle(event);
}

} // namespace mosaic::ui
