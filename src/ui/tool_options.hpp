#pragma once

#include "ui/popover.hpp" // Popover (the overflow list is a child sub-window)
#include "ui/widgets.hpp" // Panel

#include "common/image.hpp" // common::Image (Fl_RGB_Image previews) + the shared colour types

#include <functional>
#include <memory>

namespace mosaic::ui {

class ToolManager;
class ScrubRuler;

// The options-bar overflow list (S16-n): a vertical stack of the controls that don't fit on the bar,
// opened by the bar's chevron. A Popover subclass only so the bar can size it to its content
// (setBaseSize is protected) and drive its child group; ToolOptionsBar owns the *population*, this
// owns the lifetime/placement. The main window creates ONE (before it is shown, per the Popover
// child-sub-window rule) and hands it to the bar via setOverflowPopover().
class OptionsOverflowPopover : public Popover {
public:
    OptionsOverflowPopover();
    // Resize to (w,h), drop the previous rows, and begin() so the caller adds the overflowed controls
    // as children; pair with endRebuild(). (Thin wrappers exposing the protected setBaseSize.)
    void beginRebuild(int w, int h);
    void endRebuild();
};

// The tool options bar (PLAN S11-b): a slim horizontal strip under the menu bar that surfaces the
// active tool's most-changed options as compact controls (sliders / dropdowns / toggles). It
// observes the ToolManager -- when the active tool changes it rebuilds its controls, and when an
// option value is edited elsewhere it re-syncs (the lockstep twin, the Properties tab, is S11-c).
// Each control writes straight back into the tool's ToolOption (the single source of truth) and
// calls ToolManager::notifyOptionsChanged().
class ToolOptionsBar : public Panel {
public:
    ToolOptionsBar(int X, int Y, int W, int H, ToolManager& tools);
    ~ToolOptionsBar() override; // out-of-line: m_state holds an incomplete type here

    void rebuild();    // active tool changed: recreate the controls from its option set
    void syncValues(); // an option value changed elsewhere: push values back into the controls

    // Inject the shared overflow popover (S16-n). Non-owning; the main window owns it and must create
    // it before being shown. Triggers a rebuild so any controls that don't fit move into it now.
    void setOverflowPopover(OptionsOverflowPopover* popover);

    // Inject the shared precision-ruler overlay handed to every ScrubSlider the bar builds. Non-owning;
    // the main window owns it and must create it before being shown (it is a child sub-window).
    void setScrubRuler(ScrubRuler* ruler);

    // Supply the font-picker preview renderer (S29-c): given a family name and the preview cell size in
    // px, return a cached image of that family drawn in its own face (or null). The host (which owns the
    // shaper + FontDB) renders + caches; the bar wires it to every Font-kind option's open list. Set
    // before the first rebuild that shows a Font control; triggers a rebuild so the list picks it up.
    void setFontPreview(std::function<Fl_RGB_Image*(const std::string& family, int w, int h)> cb);

    // Live hover preview (S29-c §8): invoked with the family the user is hovering in the open list, or
    // "" when the cursor leaves it / the list closes (revert). The host applies it transiently to the
    // edited text; the bar maps the hovered row index to its family before calling this. Wired to every
    // Font-kind option's open list on rebuild.
    void setFontHoverPreview(std::function<void(const std::string& family)> cb);

    // The "Edit shape…" button widget (the shape tools' "designer" action), or null when the active
    // tool has none. The shape-designer popover anchors to it. Refreshed on every rebuild.
    [[nodiscard]] Fl_Widget* designerButton() const;

    // The Type tool's "Style…" button widget (the "typePanel" action), or null otherwise. The Type
    // panel popover anchors to it. Refreshed on every rebuild (S29-c §8).
    [[nodiscard]] Fl_Widget* typePanelButton() const;
    // The Type tool's "3D…" button (the Type3dPanel's anchor); null when not built.
    [[nodiscard]] Fl_Widget* type3dButton() const;
    // The Gradient tool's "Stops…" button (the GradientFlyout's anchor); null when not built (S22).
    [[nodiscard]] Fl_Widget* gradientStopsButton() const;

    // Runtime theme change: re-fill the panel ground, then rebuild the controls so they pick up the
    // new palette (each control bakes its colours when built).
    void reapplyTheme() override;

    // Re-lay-out the controls ourselves on a width change rather than letting Fl_Group scale them
    // (rebuild()'s clear() resets resizable() to the group, which would stretch the fixed-size
    // controls when the window widens). See the .cpp.
    void resize(int X, int Y, int W, int H) override;

private:
    ToolManager& m_tools;
    struct State; // holds the per-control bindings (defined in the .cpp)
    std::unique_ptr<State> m_state;
    OptionsOverflowPopover* m_overflow = nullptr; // shared, main-window-owned; null until injected
    ScrubRuler* m_scrubRuler = nullptr;           // shared precision HUD; null until injected
    std::function<Fl_RGB_Image*(const std::string&, int, int)> m_fontPreview; // font preview (null=off)
    std::function<void(const std::string&)> m_fontHoverPreview; // live hover preview (null=off)
};

// Height of the options-bar strip (between the menu bar and the toolbar/canvas/dock body).
inline constexpr int kOptionsBarHeight = 34;

} // namespace mosaic::ui
