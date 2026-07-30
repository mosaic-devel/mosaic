#pragma once

#include "ui/popover.hpp" // ToolbarOverflowPopover is a Popover
#include "ui/tool.hpp"    // ToolSlot
#include "ui/widgets.hpp" // Panel

#include <vector>

namespace mosaic::ui {

class ColorPicker;
class ColorState;
class ColorSwatch;
class ToolFlyout;
class ToolManager;

// The left toolbar's overflow list (S16-o): a vertical stack of the tool slots that don't fit in a
// short column, opened by the toolbar's bottom chevron. Mirrors the options-bar OptionsOverflowPopover
// (S16-n) -- a Popover subclass the toolbar sizes + populates; the main window creates the one instance
// before it is shown (the child-sub-window rule) and injects it via LeftToolbar::setOverflowPopover.
class ToolbarOverflowPopover : public Popover {
public:
    explicit ToolbarOverflowPopover(ToolManager& tools);
    // Resize to the slot count and (re)build one tool button per overflowed slot. `flyout` is handed
    // to each button so a multi-variant slot still opens its flyout from here. Does not show.
    void rebuildWith(const std::vector<ToolSlot>& slots, ToolFlyout* flyout);

protected:
    void drawContent() override; // panel + buttons, then the same group-boundary hairlines as the toolbar

private:
    ToolManager& m_tools;
    std::vector<int> m_dividerYs; // y of each ToolGroup-boundary hairline among the overflowed slots
};

// The left vertical toolbar (PLAN S11): one themed icon button per registered tool. Clicking a
// button -- or pressing the tool's keyboard shortcut (handled by the main window) -- makes it the
// active tool via the ToolManager; the active button is accent-filled. Buttons read the live active
// tool from the manager each draw, so a selection change just needs a redraw. Tool icons are SVGs
// rasterized + tinted to the theme when the buttons are built. The active-colour swatch (S11-d) is
// pinned to the bottom of the column.
class LeftToolbar : public Panel {
public:
    LeftToolbar(int X, int Y, int W, int H, ToolManager& tools, ColorState& colors);

    // Restyle after the active tool changed elsewhere (a keyboard shortcut or a programmatic set).
    // Also re-evaluates the vertical overflow (deferred) so the active tool is pulled into view.
    void refresh();

    // Re-render every button's glyph from the tools' CURRENT icon() (the icon-pack switch,
    // S52): the buttons cache pixels at construction, so a pack change rebuilds them. Flyout rows
    // rasterize on every open and need nothing.
    void reloadIcons();

    // Give the bottom colour swatch the picker it should toggle (built + owned by the main window as
    // a child sub-window; see ColorSwatch::attachPicker).
    void setColorPicker(ColorPicker* picker);

    // Give the multi-variant slot buttons the flyout they open on long-press / triangle-click (also a
    // main-window-owned child sub-window). Wired after construction, like setColorPicker.
    void setToolFlyout(ToolFlyout* flyout);

    // Inject the shared overflow popover (S16-o). Non-owning; the main window owns it and must create
    // it before show(). Wiring it triggers a relayout so a short startup window overflows immediately.
    void setOverflowPopover(ToolbarOverflowPopover* popover);

    ~LeftToolbar() override;

protected:
    void draw() override; // panel + buttons (Fl_Group), then the subtle group-boundary dividers

    // Re-pin the bottom swatch and re-evaluate the vertical overflow as the column's height tracks the
    // window. Tool buttons stay top-anchored (the column's X/Y/W never change, only H).
    void resize(int X, int Y, int W, int H) override;

private:
    // Recompute which slots fit; if that changed, rebuild() the buttons, else just re-pin the chevron.
    void relayout();
    // Tear down + recreate the tool buttons (and the overflow chevron / popover contents) for the
    // current height + active tool. Hides the overflow popover first (its anchor + rows are recreated).
    void rebuild();
    [[nodiscard]] int buttonsAvailHeight(bool reserveChevron) const;
    static void deferredRelayoutCb(void* self); // Fl::add_timeout trampoline for a tool-change relayout

    ToolManager& m_tools;
    ColorSwatch* m_swatch = nullptr;
    ToolFlyout* m_flyout = nullptr;            // applied to each (re)built button
    ToolbarOverflowPopover* m_overflow = nullptr; // shared, main-window-owned; null until injected
    Fl_Widget* m_chevron = nullptr;            // the bottom overflow chevron, or null when all slots fit
    std::vector<int> m_dividerYs;              // y of each ToolGroup-boundary hairline among visible slots
    std::vector<ToolSlot> m_visibleSlots;      // current column slots (display order); the split signature
    std::vector<ToolSlot> m_overflowSlots;     // current overflowed slots (natural order)
    bool m_deferredRebuild = false;            // a tool-change relayout is queued (see relayout())
};

// Fixed width of the toolbar column (the dock-style left strip).
inline constexpr int kToolbarWidth = 38;

} // namespace mosaic::ui
