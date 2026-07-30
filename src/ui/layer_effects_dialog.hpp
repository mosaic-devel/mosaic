#pragma once

#include "common/image.hpp"
#include "core/layer_effects.hpp"
#include "ui/preview_pane.hpp" // PreviewContent (renderPreview's return)

#include <FL/Fl_Double_Window.H>

#include <functional>
#include <memory>
#include <optional>
#include <string>

// The Layer Effects modal (LE-b; docs/layer-effects.md §6): a transactional, two-pane editor for a
// layer's non-destructive effects, reached from `Layer ▸ Layer Effects…` (and the fx badge). Built on
// the Settings/Fill modal substrate (a modal Fl_Double_Window with its own DropdownPopup / ContextMenu
// / ColorFlyout child sub-windows). A slim left CATALOGUE rail (checkbox + a −[n]+ stepper for
// stackable effects), a live PREVIEW pane pinned at the top of the content area, and a scrollable STACK
// of per-instance control panels below it. Every edit updates the working effects and previews live on
// the canvas behind the (movable) modal; OK commits one undoable SetLayerEffectsCommand, Cancel reverts.
//
// LE-b renders and edits STROKE + fill-opacity; the remaining catalogue rows are shown but disabled
// until their render tier lands (overlays LE-c/-d, shadows/glows LE-e, bevel/satin LE-f).
namespace mosaic::ui {

class ScrubRuler; // the precision-slider HUD; the modal owns its own (must share the sliders' top_window)
class BubbleFlyout; // shared base of the colour/gradient/pattern flyouts (for the preview keep-clear)

// Callbacks the dialog invokes; decouples it from MainWindow (the SettingsHost/FillHost pattern). The
// host captures the target layer at open time, so the dialog never reaches into the document model.
struct LayerEffectsHost {
    // Apply `fx` to the target layer LIVE (no undo step) and recomposite the canvas, so the pending
    // result shows behind the movable modal. std::nullopt clears the layer's effects.
    std::function<void(const std::optional<core::LayerEffects>& fx)> applyLive;
    // Composite the target layer's effect neighbourhood for the in-modal preview pane, as a
    // straight-alpha image the pane frames over its checkerboard + off-canvas backdrop. The region is
    // centred on the layer's effectsBounds, expanded to the pane's (paneW x paneH) aspect + a margin,
    // and rendered UNCLAMPED so an effect spilling past the canvas edge stays visible; the returned
    // PreviewContent also says where the canvas sits within the image (so the pane splits checker vs
    // backdrop). Reads the layer's CURRENT effects (applyLive has already set them).
    std::function<PreviewContent(int paneW, int paneH)> renderPreview;
    // Commit: restore the layer to its original effects, then push ONE SetLayerEffectsCommand(target,
    // fx) as the undo step (so undo returns to the original). Called on OK.
    std::function<void(const std::optional<core::LayerEffects>& fx)> commit;
    // The active foreground colour, for the colour flyout's "Use foreground" shortcut.
    std::function<common::Color8()> foreground;
    // The document-wide anti-aliasing setting (the Move tool's AA combobox; false only for the
    // Nearest kernel), so the pattern flyout's grid previews read like the canvas will. Optional --
    // the dialog treats an unset getter as "antialiased".
    std::function<bool()> antialias;
};

class LayerEffectsDialog : public Fl_Double_Window {
public:
    explicit LayerEffectsDialog(LayerEffectsHost host);
    ~LayerEffectsDialog() override;

    // Point the dialog at a layer: `label` names it in the header, `initial` is its current effect
    // stack (std::nullopt = none). Seeds the working copy + controls + preview; call before show().
    void seed(std::string label, std::optional<core::LayerEffects> initial);

    void reapplyTheme(); // runtime theme change while the dialog is open (MainWindow's observer)

protected:
    int handle(int event) override; // Enter = OK, Esc = Cancel; routes outside-click flyout dismissal

private:
    struct Ui; // widget pointers + per-control bindings (defined in the .cpp)

    void build();               // (re)create the fixed rail + preview + footer chrome
    void rebuildStack();        // (re)create the instance-panel stack for the current effect set
    void syncCatalog();         // push the working enable/count state into the rail rows
    void applyLive();           // working -> host.applyLive + queue a preview recompute
    void requestPreview();      // coalesce the preview recompute to ~once per frame
    static void previewTimer(void* self);
    void recomputePreview();
    // Open the shared colour / gradient flyout anchored at `anchor`, seeded from the current paint;
    // each edit is routed to `onPick`/`onChange` (the control that owns the paint writes it back).
    // Opening one closes the other (a single bubble on screen). A re-click on the same anchor toggles.
    void openColorFlyout(const Fl_Widget* anchor, common::Color8 current,
                         std::function<void(common::Color8)> onPick);
    void openGradientFlyout(const Fl_Widget* anchor, const core::vec::Gradient& g,
                            std::function<void(const core::vec::Gradient&)> onChange);
    void openPatternFlyout(const Fl_Widget* anchor, const core::vec::ProceduralPattern& pat,
                           std::function<void(const core::vec::ProceduralPattern&)> onChange);
    // Tell a flyout to keep clear of the preview's active area (a tall flyout would otherwise cover
    // it); the flyout shifts left before it is shown. Called before every openFor.
    void applyPreviewAvoid(BubbleFlyout* flyout);

    [[nodiscard]] std::optional<core::LayerEffects> currentEffects() const; // nullopt when empty()

    void doOk();
    void doCancel();

    LayerEffectsHost m_host;
    core::LayerEffects m_working;               // the live working copy the controls edit
    std::optional<core::LayerEffects> m_original; // the layer's effects at open (for revert)
    std::string m_label;
    std::function<void(common::Color8)> m_onColorPick;                  // active colour-flyout sink
    std::function<void(const core::vec::Gradient&)> m_onGradientChange; // active gradient-flyout sink
    std::function<void(const core::vec::ProceduralPattern&)> m_onPatternChange; // pattern-flyout sink
    bool m_seeding = false;      // guard: value-sets during seed/rebuild must not fire edits
    bool m_previewPending = false;
    ScrubRuler* m_ruler = nullptr; // owned by this window (a child sub-window), handed to each slider

    std::unique_ptr<Ui> m_ui;
};

} // namespace mosaic::ui
