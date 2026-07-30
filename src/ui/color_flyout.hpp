#pragma once

#include "common/image.hpp"     // Color8
#include "ui/bubble_flyout.hpp" // the shared speech-bubble base

#include <functional>

class Fl_Button;
class Fl_Input;

// A compact colour picker shown as a comic-book speech bubble (a triangle pointing at its anchor),
// for the Edit→Fill… dialog's "Color…" content (S39 follow-up D). It is a *child sub-window* of its
// host top-level — like ui::DropdownPopup / ui::ContextMenu — so it works inside a set_modal() dialog
// (events reach a modal window's own children) without the colour picker's MainWindow entanglement.
// It reuses the picker's three picking surfaces (ui::SvField / HueStrip / ColorWheel, shared via
// color_surfaces.hpp) at a smaller size and a surface combo to switch between them, plus a hex field
// and a live swatch. It edits a *local* colour and reports every change through setOnPick — it never
// touches ColorState. Build it in the host's constructor *before the host is shown* (an added-then-
// shown sub-window is promoted to a stray top-level, the bug ui::Popover documents).
namespace mosaic::ui {

class Dropdown;
class SvField;
class HueStrip;
class ColorWheel;
class SwatchButton;

class ColorFlyout : public BubbleFlyout {
public:
    ColorFlyout();
    ~ColorFlyout() override;

    // Fired live as the colour changes (drag, hex edit, surface switch).
    void setOnPick(std::function<void(common::Color8)> cb) { m_onPick = std::move(cb); }

    // Add a "Use foreground" action row (grows the bubble): a click seeds the flyout from `get`
    // (the toolbar's foreground swatch) and emits a pick -- the quick path when the colour you
    // want is the one already mixed. Call once, before the first openFor.
    void setUseForeground(std::function<common::Color8()> get);

    // Seed from `initial`, point the bubble's triangle at `anchor` (a widget under the same
    // top-level), and show.
    void openFor(const Fl_Widget* anchor, common::Color8 initial);

    [[nodiscard]] bool shownForAnchor(const Fl_Widget* a) { return m_anchor == a && shown(); }

    void hide() override;

protected:
    void drawContent() override;                 // the bubble chrome, then children, then the swatch(es)
    void moveContent(int delta) override; // shift the content widgets when the content shift changes

private:
    enum class Surface { Field = 0, WheelTriangle = 1, WheelSquare = 2 };

    void selectSurface(Surface s);  // show/hide the surface widgets + seed the visible one
    void pushToSurfaces();          // m_h/s/v -> the surface widgets + hex text + swatch (no emit)
    void onSurfaceEdited(Fl_Widget* who); // a surface widget moved -> read it back, sync, emit
    void onHexEdited();
    void emitPick();
    [[nodiscard]] common::Color8 currentColor() const; // not color() — that's Fl_Widget's setter

    std::function<void(common::Color8)> m_onPick;

    float m_h = 0.0F; // working HSV (kept across edits so greys don't lose their hue)
    float m_s = 0.0F;
    float m_v = 1.0F;
    Surface m_surface = Surface::Field;
    bool m_editingHex = false; // a hex keystroke drives the change: don't rewrite the field under it
    bool m_syncing = false;    // guards the re-entrant surface/hex callbacks

    Dropdown* m_surfaceCombo = nullptr;
    SvField* m_field = nullptr;
    HueStrip* m_strip = nullptr;
    ColorWheel* m_wheelTri = nullptr;
    ColorWheel* m_wheelSq = nullptr;
    Fl_Input* m_hex = nullptr;         // a themed ui::TextInput
    SwatchButton* m_fgSwatch = nullptr; // small foreground swatch: click reuses the fg (setUseForeground)
    std::function<common::Color8()> m_useFg;
};

// The flyout currently shown (at most one), or nullptr -- the DropdownPopup/Popover convention, so
// hosts whose flyout anchors sit in child sub-windows (the Type/3D panels: their clicks never reach
// the main window's handle) can still route outside-click dismissal.
[[nodiscard]] ColorFlyout* activeColorFlyout();

// Dismiss the active flyout if the press at (hostX, hostY) -- host-top-level coords -- is outside
// both it and its anchor (the anchor is spared so a re-click toggles it shut).
void dismissActiveColorFlyoutOnOutsideClick(int hostX, int hostY);

// Dismiss unconditionally (a no-op when none is open) -- for the canvas, whose clicks are by
// construction outside any flyout.
void dismissActiveColorFlyout();

} // namespace mosaic::ui
