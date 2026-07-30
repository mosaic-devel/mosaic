#pragma once

#include "common/image.hpp"      // Color8
#include "core/vector/paint.hpp" // core::vec::Gradient
#include "ui/bubble_flyout.hpp"  // the shared speech-bubble base

#include <functional>

class Fl_Input;

// A compact gradient editor shown as a speech-bubble child sub-window -- the sibling of
// ui::ColorFlyout (docs/layer-effects.md: "the reusable Gradient flyout with in-flyout handles").
// It edits a core::vec::Gradient's STOPS + SPREAD (the gradient TYPE is owned by the parent control
// -- the effect row / the future Gradient tool's context bar -- so it is NOT shown here). A
// horizontal preview strip draws the stop ramp over a checkerboard, with draggable stop handles
// beneath it (never on the canvas): drag to move a stop, click an empty band to add one, "Remove
// stop" deletes the selected one. The selected stop's colour is edited with the shared SvField +
// HueStrip surfaces + a hex field
// + an optional "Use foreground". Every edit fires onChange with the live gradient.
//
// Like ColorFlyout it is a CHILD sub-window of its host top-level: build it in the host's
// constructor BEFORE the host is shown (an added-then-shown sub-window is promoted to a stray
// top-level), and create it BEFORE the host's shared DropdownPopup so the spread combo's list
// stacks above it.
namespace mosaic::ui {

class Dropdown;
class SvField;
class HueStrip;
class FlatButton;
class Slider;

class GradientFlyout : public BubbleFlyout {
public:
    GradientFlyout();
    ~GradientFlyout() override;

    // Fired live as the gradient changes (stop drag/add/remove, colour edit, spread change). The
    // gradient's `type` + `transform` are preserved from what openFor() was seeded with -- the
    // flyout never touches them (the parent owns the type + the direction).
    void setOnChange(std::function<void(const core::vec::Gradient&)> cb) {
        m_onChange = std::move(cb);
    }

    // Add a "Use foreground" action for the selected stop (grows the bubble). Call once, before the
    // first openFor.
    void setUseForeground(std::function<common::Color8()> get);

    // Seed from `initial` (its stops/spread are edited; its type/transform are kept and
    // re-emitted), point the triangle at `anchor`, and show.
    void openFor(const Fl_Widget* anchor, const core::vec::Gradient& initial);

    [[nodiscard]] bool shownForAnchor(const Fl_Widget* a) { return m_anchor == a && shown(); }

    void hide() override;

protected:
    void drawContent() override;
    int handle(int event) override;
    void
    moveContent(int delta) override; // shift the content widgets when the content shift changes

private:
    void seedSelectionColour(); // selected stop -> working HSV -> surfaces + hex
    void pushToSurfaces();      // working HSV -> surfaces + hex + swatch (no emit)
    void onSurfaceEdited(Fl_Widget* who);
    void onHexEdited();
    void writeSelectedColour(); // working HSV -> selected stop colour + redraw strip + emit
    void emitChange();
    void addStopAt(double offset); // insert a stop (colour = ramp there), select it
    void addStopInGap();           // insert a stop in the widest gap (the "+" action)
    void removeSelected();         // erase the selected stop (kept >= 2), reselect

    [[nodiscard]] int handleXFor(double offset) const; // handle centre x for an offset
    [[nodiscard]] int hitHandle(int localX) const;     // nearest handle to window-x, or -1

    std::function<void(const core::vec::Gradient&)> m_onChange;

    core::vec::Gradient m_grad; // the working gradient (stops kept sorted by offset)
    int m_selected = 0;         // selected stop index (into m_grad.stops)

    float m_h = 0.0F, m_s = 0.0F, m_v = 1.0F, m_a = 1.0F; // the selected stop's working HSV + alpha
    bool m_editingHex = false;
    bool m_syncing = false; // guards the surface/hex re-entrancy while reseeding
    int m_dragStop = -1;    // stop being dragged (-1 = none)

    Dropdown* m_spread = nullptr;
    SvField* m_field = nullptr;
    HueStrip* m_hueStrip = nullptr;
    Slider* m_alpha = nullptr;         // the selected stop's alpha (0..100%)
    Fl_Widget* m_alphaLabel = nullptr; // "Alpha" caption (moved with the content shift)
    Fl_Input* m_hex = nullptr;
    FlatButton* m_addBtn = nullptr;    // small "+" (add a stop in the largest gap)
    FlatButton* m_removeBtn = nullptr; // small "-" (remove the selected stop)
    Fl_Widget* m_fgSwatch = nullptr;   // foreground swatch: click -> set the selected stop to it
    std::function<common::Color8()> m_useFg;
};

// The registry mirrors ColorFlyout's: at most one gradient flyout open, so hosts can route outside-
// click dismissal even when the anchor sits in a child sub-window.
[[nodiscard]] GradientFlyout* activeGradientFlyout();
void dismissActiveGradientFlyoutOnOutsideClick(int hostX, int hostY);
void dismissActiveGradientFlyout();

// The default TRANSFORM (gradient unit-space -> the normalised content box [0,1]^2) for a type:
// linear runs left->right; radial is centred with radius to the edge; conic is centred. A parent
// control uses this when it switches an existing gradient's type so the new type is well-placed.
[[nodiscard]] common::Affine2D defaultGradientTransform(core::vec::GradientType type);

// The gradient DIRECTION helpers (a parent control -- the Fill modal, the LE Gradient Overlay row
// -- owns the direction via a dial, since the flyout only edits stops/spread).
// `directedGradientTransform` turns the type's default `deg` degrees about the normalised
// content-box centre (0.5,0.5): it rotates a LINEAR gradient's axis and a CONIC's sweep start; it
// is a no-op for a circular RADIAL. `gradientDirectionDeg` recovers the baked-in rotation ([0,360))
// from a gradient's transform so the dial can seed itself.
[[nodiscard]] common::Affine2D directedGradientTransform(core::vec::GradientType type, double deg);
[[nodiscard]] double gradientDirectionDeg(const core::vec::Gradient& g);

// A gradient with two stops a(0)->b(1) and defaultGradientTransform(type) -- what a parent control
// seeds when the user first switches a paint to this gradient type.
[[nodiscard]] core::vec::Gradient defaultGradient(core::vec::GradientType type, common::Color8 a,
                                                  common::Color8 b);

} // namespace mosaic::ui
