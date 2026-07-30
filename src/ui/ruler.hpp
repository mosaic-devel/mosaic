#pragma once

#include "common/geometry.hpp"

#include <FL/Fl_Widget.H>
#include <functional>

// Canvas rulers (View -> Rulers, Ctrl+R): slim strips along the canvas's top and left edges showing
// document coordinates (unit = px), tracking the view's pan + zoom, with a moving tick that follows
// the cursor. Drawn as ordinary FLTK widgets in a gutter reserved beside the Vulkan canvas -- FLTK
// cannot paint over the Vulkan surface, but the gutter is outside it. Each strip reads the canvas's
// CanvasView every draw so it stays in lockstep with pan/zoom (the app redraws it on view + cursor
// changes). Dragging OUT of a strip pulls a new guide onto the canvas (the guide feature wires the
// hooks below; they stay inert until then).
namespace mosaic::ui {

class VulkanCanvas;

inline constexpr int kRulerSize = 18; // gutter thickness, logical px

class RulerStrip : public Fl_Widget {
public:
    enum class Orientation { Horizontal, Vertical };
    RulerStrip(int x, int y, int w, int h, Orientation orientation, VulkanCanvas* canvas);

    // The pointer moved over the canvas (document coords); moves the follow tick. `over` false hides
    // it (the pointer left the canvas). The app pushes this from the canvas cursor callback.
    void setCursor(double docX, double docY, bool over);

    // Guide drag-out hooks (set by the guide feature; unset = a plain, non-interactive ruler). Fired
    // with a document coordinate along this ruler's CROSS axis: a doc Y for the horizontal ruler (it
    // pulls a horizontal guide), a doc X for the vertical ruler. `horizontalGuide` says which.
    std::function<void(bool horizontalGuide, double docPos)> onGuideBegin;
    std::function<void(double docPos)> onGuideUpdate;
    std::function<void(bool cancel)> onGuideEnd; // cancel = released back over the ruler/gutter

    void draw() override;
    int handle(int event) override;

private:
    // Map an event (top-level window coords) to the document coordinate along this ruler's cross
    // axis; `overCanvas` reports whether the pointer currently sits over the canvas (not the gutter).
    [[nodiscard]] double eventDocPos(bool& overCanvas) const;

    Orientation m_orientation;
    VulkanCanvas* m_canvas;
    common::Vec2 m_cursorDoc{};
    bool m_cursorOver = false;
    bool m_dragging = false;
};

} // namespace mosaic::ui
