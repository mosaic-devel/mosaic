#pragma once

#include "common/image.hpp"       // Color8
#include "core/vector/pattern.hpp" // core::vec::ProceduralPattern
#include "ui/bubble_flyout.hpp"    // the shared speech-bubble base

#include <functional>

class Fl_Input;
class Fl_Box;
class Fl_Group;

// A compact procedural-pattern editor shown as a speech-bubble child sub-window -- the sibling of
// ui::ColorFlyout / ui::GradientFlyout (docs/layer-effects.md §7, LE-d). It edits a
// core::vec::ProceduralPattern across TWO panes:
//   * LEFT pane -- COLOUR only: fg + bg edited one-at-a-time via a shared SvField + HueStrip + hex +
//     alpha (a small target-swatch pair picks which colour those surfaces drive) + a Use-foreground
//     swatch, exactly as the gradient flyout edits its selected stop.
//   * RIGHT pane -- PATTERN specifics, SCROLLABLE: the per-kind controls on top (Scale + Weight/
//     Distance as ScrubSliders, Angle as a rotary Dial + an "anchor to canvas" toggle), then the
//     grid of live kind previews, then the (not-yet-built) image-tile section (LE-d2).
// Every edit fires onChange.
//
// Like the other flyouts it is a CHILD sub-window of its host top-level: build it in the host's
// constructor BEFORE the host is shown (an added-then-shown sub-window becomes a stray top-level),
// and create it BEFORE the host's shared DropdownPopup so any popup list stacks above it. The host
// also injects its ScrubRuler (setRuler) so the Scale/Weight scrub sliders show the precision HUD.
namespace mosaic::ui {

class SvField;
class HueStrip;
class Slider;
class ScrubSlider;
class ScrubRuler;
class Dial;
class CheckBox;
class SwatchButton;
class PatternGrid; // the file-local 5x5 live-preview picker (defined in the .cpp, ns mosaic::ui)

class PatternFlyout : public BubbleFlyout {
public:
    PatternFlyout();
    ~PatternFlyout() override;

    // Fired live as the pattern changes (kind / scale / angle / weight / spacing / anchor / fg / bg).
    void setOnChange(std::function<void(const core::vec::ProceduralPattern&)> cb) {
        m_onChange = std::move(cb);
    }

    // Add a "Use foreground" action (sets the active fg/bg target to the app foreground). Call once,
    // before the first openFor.
    void setUseForeground(std::function<common::Color8()> get);

    // The shared precision-ruler HUD for the Scale / Weight scrub sliders. Non-owning; must share the
    // sliders' top_window (the host modal owns it). Null disables only the HUD (scrubbing still works).
    void setRuler(ScrubRuler* r);

    // Whether the grid previews anti-alias, so the flyout reads like the canvas: the host passes the
    // document-wide AA setting (the Move tool's AA combobox) so a crisp document shows crisp previews.
    void setAntialias(bool aa);

    // Seed from `initial`, point the triangle at `anchor`, and show.
    void openFor(const Fl_Widget* anchor, const core::vec::ProceduralPattern& initial);

    [[nodiscard]] bool shownForAnchor(const Fl_Widget* a) { return m_anchor == a && shown(); }

    void hide() override;

protected:
    void drawContent() override;
    void moveContent(int delta) override;

private:
    enum class Target { Fg, Bg };

    void seedTargetColour();  // active target colour -> working HSV -> surfaces + hex
    void pushToSurfaces();    // working HSV -> surfaces + hex (no emit)
    void onSurfaceEdited(Fl_Widget* who);
    void onHexEdited();
    void writeTargetColour();  // working HSV -> active target colour + emit
    void setTarget(Target t);
    void updateKindControls();  // show Weight or Distance (or neither) for the current kind + relabel
    void refreshSliderLabels(); // scale / weight / distance captions -> "Name  value"
    void emitChange();

    std::function<void(const core::vec::ProceduralPattern&)> m_onChange;

    core::vec::ProceduralPattern m_pat;
    Target m_target = Target::Fg;

    float m_h = 0.0F, m_s = 0.0F, m_v = 0.0F, m_a = 1.0F;  // the active target's working HSV + alpha
    bool m_editingHex = false;
    bool m_syncing = false;
    bool m_weightShown = true;  // is the Weight/Distance row visible? (drives the realign below it)

    // --- right pane (pattern specifics; scrollable) ---
    Fl_Group* m_rightPane = nullptr;      // the scroll viewport
    ScrubSlider* m_scale = nullptr;
    ScrubSlider* m_weight = nullptr;      // relabels between Weight / Distance per kind
    ScrubSlider* m_offset = nullptr;      // tiling phase, 0..100%
    Dial* m_angle = nullptr;
    CheckBox* m_anchorCheck = nullptr;  // "anchor to canvas" (NB: base BubbleFlyout owns m_anchor)
    PatternGrid* m_grid = nullptr;
    Fl_Box* m_scaleLabel = nullptr;
    Fl_Box* m_weightLabel = nullptr;
    Fl_Box* m_offsetLabel = nullptr;
    Fl_Box* m_angleLabel = nullptr;
    Fl_Box* m_angleValue = nullptr;       // the "123°" readout, right of the dial (caption stays "Angle")
    Fl_Box* m_ld2 = nullptr;              // the LE-d2 "image tiles" placeholder
    ScrubRuler* m_ruler = nullptr;        // shared precision HUD (non-owning)
    bool m_previewAA = true;              // grid previews honour the document AA setting

    // --- left pane (colour) ---
    SwatchButton* m_fgSwatch = nullptr;
    SwatchButton* m_bgSwatch = nullptr;
    SwatchButton* m_useFgSwatch = nullptr;
    Fl_Box* m_fgLabel = nullptr;
    Fl_Box* m_bgLabel = nullptr;
    SvField* m_field = nullptr;
    HueStrip* m_hueStrip = nullptr;
    Slider* m_alpha = nullptr;
    Fl_Box* m_alphaLabel = nullptr;
    Fl_Input* m_hex = nullptr;
    std::function<common::Color8()> m_useFg;
};

// One pattern flyout open at a time; mirrors the color/gradient registries so a host can route
// outside-click dismissal even when the anchor lives in a child sub-window.
[[nodiscard]] PatternFlyout* activePatternFlyout();
void dismissActivePatternFlyoutOnOutsideClick(int hostX, int hostY);
void dismissActivePatternFlyout();

// A default procedural pattern (Dots, black on transparent) -- what a parent control seeds when the
// user first switches a paint to a pattern.
[[nodiscard]] core::vec::ProceduralPattern defaultProceduralPattern(common::Color8 fg);

}  // namespace mosaic::ui
