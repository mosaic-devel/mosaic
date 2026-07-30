#include "ui/right_dock.hpp"

#include "ui/brush_preset_panel.hpp"
#include "ui/layer_panel.hpp"
#include "ui/theme.hpp"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <utility>

namespace mosaic::ui {
namespace {

// What each region needs to still be worth showing: a layer list that shows layers, and a preset
// grid that shows a whole row of presets plus its header and search box.
constexpr int kMinLayersH = 180;
constexpr int kMinPresetH = 150;
constexpr int kGripW = 26; // the splitter's grip mark

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

} // namespace

DockSplit presetSplit(int dockHeight, int desiredPresetHeight, bool presetsVisible) {
    DockSplit split;
    if (!presetsVisible || dockHeight <= 0) {
        split.layersH = std::max(0, dockHeight);
        return split; // the section is hidden: the layer panel takes the whole dock
    }
    const int avail = dockHeight - RightDock::splitterHeight();
    if (avail <= 0) {
        split.layersH = std::max(0, dockHeight); // not even room for the strip
        return split;
    }
    const int mostThePresetsMayHave = avail - kMinLayersH;
    if (mostThePresetsMayHave < kMinPresetH) {
        // The dock is too short for both minimums. Halve what there is rather than let either region
        // vanish -- a layer list with no rows is still a layer list; a dock with no layer list is a
        // bug report.
        split.presetH = std::max(1, avail / 2);
        split.layersH = avail - split.presetH;
        return split;
    }
    split.presetH = std::clamp(desiredPresetHeight, kMinPresetH, mostThePresetsMayHave);
    split.layersH = avail - split.presetH;
    return split;
}

RightDock::RightDock(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    // The dock's own ground shows only in the splitter strip -- but it must still be ERASED there,
    // so the group carries a real box rather than FL_NO_BOX.
    box(MOSAIC_FLAT_BOX);
    color(toFl(activePalette().panelBg));
    begin();
    m_layers = new LayerPanel(X, Y, W, H);
    m_presets = new BrushPresetPanel(X, Y, W, 0);
    m_presets->hide(); // Brush-only: the host shows it on the first tool change (and at startup)
    end();
    resizable(nullptr); // layoutChildren() places both regions; see resize()
    layoutChildren();
}

void RightDock::setPresetsVisible(bool on) {
    if (on == m_presetsVisible)
        return;
    m_presetsVisible = on;
    if (m_presets != nullptr) {
        if (on)
            m_presets->show();
        else
            m_presets->hide();
    }
    layoutChildren(); // settle the grid's scroll geometry BEFORE the first paint (the tab-entry rule)
    redraw();
}

void RightDock::setPresetHeight(int px) {
    if (px == m_presetHeight)
        return;
    m_presetHeight = px; // the WISH; the clamp lives in presetSplit()
    layoutChildren();
    redraw();
}

int RightDock::effectivePresetHeight() const {
    return presetSplit(h(), m_presetHeight, m_presetsVisible).presetH;
}

int RightDock::splitterTop() const {
    if (!m_presetsVisible)
        return -1;
    const DockSplit split = presetSplit(h(), m_presetHeight, m_presetsVisible);
    if (split.presetH <= 0)
        return -1;
    return y() + split.layersH;
}

void RightDock::resize(int X, int Y, int W, int H) {
    Fl_Widget::resize(X, Y, W, H); // NOT Fl_Group::resize -- we place the two regions ourselves
    layoutChildren();
}

void RightDock::layoutChildren() {
    if (m_layers == nullptr || m_presets == nullptr)
        return;
    const DockSplit split = presetSplit(h(), m_presetHeight, m_presetsVisible);
    m_layers->resize(x(), y(), w(), std::max(1, split.layersH));
    if (split.presetH > 0) {
        const int top = y() + split.layersH + splitterHeight();
        m_presets->resize(x(), top, w(), split.presetH);
    }
}

void RightDock::reapplyTheme() {
    color(toFl(activePalette().panelBg));
    if (m_layers != nullptr)
        m_layers->reapplyTheme();
    if (m_presets != nullptr)
        m_presets->reapplyTheme();
    redraw();
}

void RightDock::draw() {
    Fl_Group::draw(); // the panel ground (which is all the strip is) + both regions

    const int top = splitterTop();
    if (top < 0)
        return;
    const Palette& pal = activePalette();
    const int strip = splitterHeight();

    // The strip's own slice of the canvas|dock junction hairline. Each element owns exactly one
    // slice: the layer panel and the preset panel draw their own left edges (Panel::EdgeLeft), so a
    // doubled 2 px line at the seam is impossible.
    fl_color(toFl(pal.border));
    fl_yxline(x(), top, top + strip - 1);
    fl_xyline(x(), top, x() + w() - 1); // the separator between the two regions

    // The grip: two short bars, so the strip reads as a handle and not as a gap. Accent while the
    // pointer is on it (or dragging), muted otherwise.
    const bool lit = m_splitHover || m_heightDrag;
    fl_color(toFl(lit ? pal.accent : pal.textMuted));
    const int gx = x() + (w() - kGripW) / 2;
    fl_rectf(gx, top + 2, kGripW, 1);
    fl_rectf(gx, top + 4, kGripW, 1);
}

int RightDock::handle(int event) {
    // Both splitters run BEFORE Fl_Group::handle() delegates to the children, so the bands win the
    // press even though the panels are stacked behind them. The regions inset past both (the layer
    // list, the preset grid and the search box all start past the left band), so nothing clickable
    // hides underneath.
    const bool overWidth = Fl::event_x() >= x() && Fl::event_x() < x() + splitterWidth();
    const int top = splitterTop();
    const bool overHeight = top >= 0 && !overWidth && Fl::event_y() >= top - 1 &&
                            Fl::event_y() < top + splitterHeight() + 1;

    switch (event) {
    case FL_ENTER:
    case FL_MOVE:
        if (!m_widthDrag && !m_heightDrag) {
            if (overWidth != m_widthCursor || overHeight != m_heightCursor) {
                m_widthCursor = overWidth;
                m_heightCursor = overHeight;
                fl_cursor(overWidth ? FL_CURSOR_WE
                                    : (overHeight ? FL_CURSOR_NS : FL_CURSOR_DEFAULT));
            }
            if (overHeight != m_splitHover) {
                m_splitHover = overHeight;
                redraw();
            }
            // Claiming the event over a band is what makes FLTK record us as belowmouse() -- without
            // it no FL_LEAVE is ever delivered here and the resize cursor sticks on the way out.
            if (overWidth || overHeight)
                return 1;
        }
        break;
    case FL_LEAVE:
        if (!m_widthDrag && !m_heightDrag) {
            if (m_widthCursor || m_heightCursor) {
                m_widthCursor = false;
                m_heightCursor = false;
                fl_cursor(FL_CURSOR_DEFAULT);
            }
            if (m_splitHover) {
                m_splitHover = false;
                redraw();
            }
        }
        break;
    case FL_PUSH:
        if (Fl::event_button() == FL_LEFT_MOUSE) {
            if (m_onWidthRequest && overWidth) {
                m_widthDrag = true;
                return 1;
            }
            if (overHeight) {
                m_heightDrag = true;
                return 1;
            }
        }
        break;
    case FL_DRAG:
        if (m_widthDrag) {
            // The dock's RIGHT edge is pinned to the window; the cursor drags the left one. The host
            // clamps (canvas minimum, dock min/max) -- we only report the intent.
            m_onWidthRequest(x() + w() - Fl::event_x(), /*committed=*/false);
            return 1;
        }
        if (m_heightDrag) {
            // The section's BOTTOM is pinned to the dock's; the cursor drags the strip's top edge.
            setPresetHeight(y() + h() - Fl::event_y() - splitterHeight());
            if (m_onPresetHeightChanged)
                m_onPresetHeightChanged(effectivePresetHeight(), /*committed=*/false);
            return 1;
        }
        break;
    case FL_RELEASE:
        if (m_widthDrag) {
            m_widthDrag = false;
            m_onWidthRequest(x() + w() - Fl::event_x(), /*committed=*/true); // the host persists here
            if (!Fl::event_inside(this)) {
                m_widthCursor = false;
                fl_cursor(FL_CURSOR_DEFAULT);
            }
            return 1;
        }
        if (m_heightDrag) {
            m_heightDrag = false;
            // Persist the EFFECTIVE height, not the wish: a drag that ran past the clamp must not
            // write a height the dock never showed.
            if (m_onPresetHeightChanged)
                m_onPresetHeightChanged(effectivePresetHeight(), /*committed=*/true);
            if (!Fl::event_inside(this)) {
                m_heightCursor = false;
                m_splitHover = false;
                fl_cursor(FL_CURSOR_DEFAULT);
            }
            redraw();
            return 1;
        }
        break;
    default:
        break;
    }
    return Fl_Group::handle(event);
}

} // namespace mosaic::ui
