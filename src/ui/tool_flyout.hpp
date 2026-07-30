#pragma once

#include "ui/popover.hpp"
#include "ui/tool.hpp" // ToolSlot

class Fl_Widget;

// The toolbar slot flyout (PLAN S11-e): a small anchored popover listing the *variants* of a tool slot
// -- the marquee slot's rectangular / elliptical, the lasso slot's free / polygonal, the shape slot's
// rectangle / ellipse / line. Each row is the variant's colour icon + name; clicking it makes that
// variant active (so the slot's toolbar button now shows it) and closes the flyout. One reusable
// instance, owned by the main window as a child sub-window (like the colour picker) so it stacks over
// the canvas; the toolbar opens it for a slot via showForSlot(). It sizes itself to the row count.
namespace mosaic::ui {

class ToolManager;

class ToolFlyout : public Popover {
public:
    explicit ToolFlyout(ToolManager& tools);

    // Rebuild the rows for `slot`'s variants (resizing to fit) and show anchored to `anchor` (the
    // slot's toolbar button). A single-variant slot would show one row, but the toolbar only opens the
    // flyout for slots that actually have variants.
    void showForSlot(ToolSlot slot, const Fl_Widget* anchor);

private:
    ToolManager& m_tools;
};

} // namespace mosaic::ui
