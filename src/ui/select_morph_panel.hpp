#pragma once

#include "common/geometry.hpp" // Rect (corner placement region)
#include "ui/popover.hpp"

#include <functional>
#include <memory>

class Fl_Widget;

namespace mosaic::ui {

class ScrubRuler;

// The four Select-menu morphology ops, in the dropdown's display order (also the widget's value()).
enum class SelectMorphMode : int { Grow = 0, Shrink = 1, Feather = 2, Smooth = 3 };

// The selection-morphology corner panel (supersedes the S18 modal "N px" prompts): a pinned corner
// popover -- same placement/pinned model as the Type "Style…"/"3D…" panels and the adjustment
// editor -- with a mode dropdown (Grow/Shrink/Feather/Smooth) and one amount slider. Every mode or
// amount change fires onPreview, which the host turns into a LIVE marching-ants preview (it morphs a
// snapshot of the selection and uploads the result to the canvas WITHOUT a command). Apply lands one
// SetSelectionCommand through the marquee's commit funnel; Cancel/Esc close and the host restores the
// canvas from the document's untouched selection. Pure UI: all the morphology math is core::Selection.
class SelectMorphPanel : public Popover {
public:
    SelectMorphPanel();
    ~SelectMorphPanel() override;

    // Corner placement (the Type panel's behaviour): pin to the canvas region's bottom-right.
    void setPlacementProviders(std::function<common::Rect()> region);
    void setScrubRuler(ScrubRuler* r); // shared precision HUD for the amount slider

    // The host wires these. onPreview fires on every live mode/amount change (and once on show);
    // onApply / onCancel fire once from the footer buttons (onApply also from Enter).
    void setOnPreview(std::function<void()> cb) { m_onPreview = std::move(cb); }
    void setOnApply(std::function<void()> cb) { m_onApply = std::move(cb); }
    void setOnCancel(std::function<void()> cb) { m_onCancel = std::move(cb); }

    // Seed the op + amount before showing (the four menu items pick the initial mode). Pushes the
    // values into the widgets without firing onPreview (the host seeds that itself after the show).
    void configure(SelectMorphMode mode, double amount);
    [[nodiscard]] SelectMorphMode mode() const noexcept { return m_mode; }
    [[nodiscard]] double amount() const noexcept { return m_amount; }

    void openFor(const Fl_Widget* anchor); // corner-place + show (pinned)
    void reapplyTheme() override;          // re-theme + rebuild the controls in place

protected:
    int handle(int event) override; // Enter = Apply (Esc closes via Popover, the host restores)

private:
    void build();
    void pushStateToControls(); // reflect m_mode/m_amount into the widgets (m_syncing-guarded)

    struct State;
    std::unique_ptr<State> m_state;
    SelectMorphMode m_mode = SelectMorphMode::Grow;
    double m_amount = 4.0;
    bool m_syncing = false; // a value-set during configure()/rebuild must not fire onPreview

    std::function<void()> m_onPreview;
    std::function<void()> m_onApply;
    std::function<void()> m_onCancel;
    ScrubRuler* m_ruler = nullptr;
};

} // namespace mosaic::ui
