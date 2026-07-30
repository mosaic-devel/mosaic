#pragma once

#include <FL/Fl_Group.H>

#include <functional>

// The right dock (docs/brushes.md §8.2). Until now the dock simply *was* the LayerPanel, with the
// History panel tabbed inside it; the Brush-preset section is the first thing that has to sit
// BESIDE that rather than inside it, so the dock finally becomes a container of its own:
//
//     RightDock
//       |-- LayerPanel        (tabbed Layers | History -- unchanged)
//       |-- a horizontal splitter, with a remembered height
//       `-- BrushPresetPanel  (shown ONLY while the Brush tool is active)
//
// Two things are deliberate:
//
//  * **The width splitter is on the dock's own LEFT EDGE**, not a sibling widget between the canvas
//    and the dock. VulkanCanvas is an Fl_Window, and no sibling widget may paint over a child
//    sub-window -- so the grab band has to belong to something that is *not* beside the canvas. It
//    moved here from LayerPanel, which no longer owns the dock's edge.
//  * **The preset section is Brush-only** (§8.2). The other brush-family tools -- Inpaint, Heal,
//    Clone, Smudge, Select-brush -- keep a plain circular tip: a scatter-and-rotation tip on a
//    *healing* brush is not a feature. When the section hides, the layer panel takes the full
//    height. This is the first tool-conditional docked chrome in the tree.
namespace mosaic::ui {

class BrushPresetPanel;
class LayerPanel;

// How the dock's height divides between the layers panel and the preset section. Pure; unit-tested.
struct DockSplit {
    int layersH = 0;
    int presetH = 0; // 0 when the section is hidden
};

// `desiredPresetHeight` is what the USER asked for (the persisted Settings::brushPresetHeight, or a
// splitter drag in flight) -- it is clamped here, never at the source, so a dock that is briefly too
// short does not overwrite the height the user chose. A dock too short for both minimums splits what
// there is rather than letting either region vanish.
[[nodiscard]] DockSplit presetSplit(int dockHeight, int desiredPresetHeight, bool presetsVisible);

class RightDock : public Fl_Group {
public:
    RightDock(int X, int Y, int W, int H);

    // The dock's two regions. `layers()` is the panel the ~30 host call sites already talk to; the
    // dock forwards nothing it does not have to.
    [[nodiscard]] LayerPanel* layers() const noexcept { return m_layers; }
    [[nodiscard]] BrushPresetPanel* presets() const noexcept { return m_presets; }

    // Show/hide the preset section (the host calls this on every tool change).
    void setPresetsVisible(bool on);
    [[nodiscard]] bool presetsVisible() const noexcept { return m_presetsVisible; }

    // The section's height, as the user last left it (persisted). Clamped at layout time, so a stale
    // or hand-edited value can never squeeze the layer list out of existence.
    void setPresetHeight(int px);
    [[nodiscard]] int presetHeight() const noexcept { return m_presetHeight; }
    // The height actually in force right now (after the clamp) -- what the host persists.
    [[nodiscard]] int effectivePresetHeight() const;

    // The left-edge width splitter: the host owns the body layout and the clamp, so the dock only
    // reports the intent. `committed` marks the end of the gesture, where the host persists it.
    void setOnWidthRequest(std::function<void(int width, bool committed)> cb) {
        m_onWidthRequest = std::move(cb);
    }
    // The horizontal splitter, same contract: the height is already applied when this fires; the
    // host persists on `committed`.
    void setOnPresetHeightChanged(std::function<void(int height, bool committed)> cb) {
        m_onPresetHeightChanged = std::move(cb);
    }

    // The grab band on the dock's left edge, and the strip between the two regions. The regions
    // inset past both, so neither splitter ever steals a click aimed at a control.
    [[nodiscard]] static constexpr int splitterWidth() { return 5; }
    [[nodiscard]] static constexpr int splitterHeight() { return 7; }

    void resize(int X, int Y, int W, int H) override;
    void reapplyTheme();

protected:
    void draw() override;           // the splitter strip's hairline + grip
    int handle(int event) override; // both splitter drags, ahead of the children

private:
    void layoutChildren();
    // The horizontal splitter strip's top edge, or -1 while the section is hidden.
    [[nodiscard]] int splitterTop() const;

    LayerPanel* m_layers = nullptr;
    BrushPresetPanel* m_presets = nullptr;
    bool m_presetsVisible = false;
    int m_presetHeight = 260; // the user's wish; presetSplit() clamps it to what the dock can afford

    std::function<void(int, bool)> m_onWidthRequest;
    std::function<void(int, bool)> m_onPresetHeightChanged;
    bool m_widthDrag = false;
    bool m_widthCursor = false;  // the WE cursor is showing (don't re-set it every motion)
    bool m_heightDrag = false;
    bool m_heightCursor = false; // ... the NS one
    bool m_splitHover = false;   // the strip's grip lights up under the pointer
};

} // namespace mosaic::ui
