#pragma once

#include "ui/popover.hpp" // the child-sub-window host (built before the parent is shown)

#include "common/geometry.hpp"     // common::Rect (placement region / caret obstacle)
#include "core/text/shaping.hpp"   // VariableAxis (the selected face's fvar axes, R4 §3.4)
#include "core/text/text_model.hpp" // CommonStyle / CommonParagraph / CharStyle / Paragraph

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Fl_Widget;
class Fl_RGB_Image;

namespace mosaic::ui {

class ScrubRuler; // the shared precision-slider HUD (a child sub-window); injected, non-owning

// The Type panel (S29-c Round 3, docs/type-tool.md §8): the bottom-right transient "everything"
// surface for the selected/edited text object. A ui::Popover sub-window like the colour flyout /
// shape designer, but with NO speech bubble (it pins to the canvas corner, not a widget, so a
// pointer would be meaningless) and a bottom-right corner placement that FLIPS to bottom-left when
// it would cover the text being edited. Opened/toggled by the Type context bar's "Style…" button.
//
// Two grouped sections -- Character (font, size, leading, tracking, baseline shift, B/I/U/S, the run
// colour) and Paragraph (alignment, space before/after, indents, direction). Every control edits the
// CURRENT selection's CharStyle/Paragraph through the host funnel (the same VulkanCanvas
// applySelectionStyle / applySelectionParagraph path the context bar uses), keyed by a coalesce id so
// a continuous drag is one undo step and is shared with the bar. reflect() pushes the selection's
// common style back into the controls (mixed fields read "—"); the host calls it on every
// selection/style change, keeping the panel and the bar in lockstep. The control set is FIXED, so it
// is built once; only a runtime re-theme rebuilds (to re-bake the palette).
//
// Run colour is READ-ONLY here for now (R2 deferral): the chip mirrors the run fill (hatched when
// mixed); editing flows through the toolbar foreground swatch, which already recolours live. The full
// solid/gradient paint editor (docs §8.2) is a later concern.
class TypePanel : public Popover {
public:
    TypePanel();
    ~TypePanel() override;

    // Live edit funnels. The host routes the mutator to VulkanCanvas::applySelectionStyle /
    // applySelectionParagraph, deciding undo-coalescing from `id` (consecutive same-id edits merge;
    // a new id starts a fresh step -- shared with the context bar so the two surfaces never split a
    // drag or fuse two distinct edits).
    void setOnStyleEdit(
        std::function<void(const std::string& id,
                           std::function<void(core::text::CharStyle&)> mutate)>
            cb) {
        m_onStyleEdit = std::move(cb);
    }
    void setOnParagraphEdit(
        std::function<void(const std::string& id,
                           std::function<void(core::text::Paragraph&)> mutate)>
            cb) {
        m_onParagraphEdit = std::move(cb);
    }
    // Block-level edits (a whole-object property, e.g. text-orientation): routed like the style/
    // paragraph funnels to VulkanCanvas::applyTextBlockEdit, coalesced by `id`.
    void setOnBlockEdit(
        std::function<void(const std::string& id,
                           std::function<void(core::text::TextBlock&)> mutate)>
            cb) {
        m_onBlockEdit = std::move(cb);
    }

    // The Color line's "Edit…" click: the host opens the shared ColorFlyout anchored at `anchor`,
    // seeded with `current` (the selection's representative fill; picks flow back through the
    // style funnel, not this panel).
    void setOnEditColor(std::function<void(const Fl_Widget* anchor, common::ColorF current)> cb) {
        m_onEditColor = std::move(cb);
    }

    // The font family list (call once at startup, like the bar's initTextToolFonts) + the picker's
    // per-row preview (each family in its own face) and live hover preview (S29-c §8). Mirrors the
    // ToolOptionsBar font-picker wiring so the two pickers look and behave identically.
    void setFontFamilies(std::vector<std::string> families);
    void setFontPreview(std::function<Fl_RGB_Image*(const std::string& family, int w, int h)> cb) {
        m_fontPreview = std::move(cb);
    }
    void setFontHoverPreview(std::function<void(const std::string& family)> cb) {
        m_fontHover = std::move(cb);
    }

    // The shared precision-ruler overlay (owned by the main window) handed to every panel ScrubSlider,
    // so a panel drag floats the same HUD as the context bar (S29-c rev 5). Non-owning; set before the
    // panel is first built (a rebuild re-wires each slider to it).
    void setScrubRuler(ScrubRuler* ruler);

    // Bottom-right corner placement providers (window coords): `region` is the canvas area to pin
    // within; `avoid` is the edited text object's AABB the panel flips away from. Forwarded to
    // Popover::setCornerPlacement.
    void setPlacementProviders(std::function<common::Rect()> region,
                               std::function<std::optional<common::Rect>()> avoid);

    // Push the current selection's common style/paragraph into the controls (model -> panel). Mixed
    // fields show a blank/— state. `wm`/`orientation`/`aa` are the edited block's whole-object writing
    // mode, Latin orientation and anti-alias mode: the Orientation control shows only when the block is
    // vertical. `axes` are the variable axes of the selection's resolved face (empty for a static face
    // or a mixed-family selection); when the set differs from the built one the panel rebuilds with one
    // slider per axis (R4 §3.4). No-op while hidden; guarded so the value-sets fire no edits.
    void reflect(const core::text::CommonStyle& cs, const core::text::CommonParagraph& cp,
                 core::text::WritingMode wm, core::text::TextOrientation orientation,
                 core::text::AntiAlias aa, const std::vector<core::text::VariableAxis>& axes);

    // Open (or, if already shown for `anchor`, close) the panel anchored to the bar's "Style…" button.
    // The host fills it via reflect() right after opening.
    void toggle(const Fl_Widget* anchor);

    void reapplyTheme() override; // rebuild the controls so they bake the new palette

    // A control changed -> route its one field through the funnel. `role` is the file-local Role enum
    // (passed as int so the enum can stay in the .cpp); `idx` is its slot (an align/direction value).
    // Public only so the C callback thunk can reach it; not part of the panel's API.
    void applyControl(int role, int idx);

    // Flip the "Advanced typography" section open/closed and resize the panel to fit. Public only so
    // the disclosure button's C callback thunk can reach it.
    void toggleAdvanced();

private:
    void build(); // (re)create the sectioned controls (the fixed set), sizing the content

    struct State; // per-control bindings + widget pointers (defined in the .cpp)
    std::unique_ptr<State> m_state;
    std::vector<std::string> m_families;
    // The variable axes the sliders were BUILT for (R4 §3.4). reflect() compares the incoming set
    // and rebuilds on change (a family switch), so between rebuilds the rows are fixed like every
    // other control. Persists across build() calls (theme, font list).
    std::vector<core::text::VariableAxis> m_axes;
    int m_contentH = 0;        // natural (unscrolled) height of the built content (current disclosure)
    int m_contentHCollapsed = 0; // content height with the Advanced section closed
    int m_contentHFull = 0;      // content height with it open
    bool m_advancedOpen = false;  // "Advanced typography" disclosure state (persists across rebuilds)
    bool m_reflecting = false; // guard: value-sets in reflect() must not fire edits

    void resizeToContent(); // setBaseSize to m_contentH (+ reanchor if shown); shared by open/toggle

    std::function<void(const std::string&, std::function<void(core::text::CharStyle&)>)> m_onStyleEdit;
    std::function<void(const std::string&, std::function<void(core::text::Paragraph&)>)>
        m_onParagraphEdit;
    std::function<void(const std::string&, std::function<void(core::text::TextBlock&)>)> m_onBlockEdit;
    std::function<void(const Fl_Widget*, common::ColorF)> m_onEditColor;
    std::function<Fl_RGB_Image*(const std::string&, int, int)> m_fontPreview;
    std::function<void(const std::string&)> m_fontHover;
    std::function<common::Rect()> m_region;
    std::function<std::optional<common::Rect>()> m_avoid;
    ScrubRuler* m_scrubRuler = nullptr; // shared precision HUD handed to each panel slider (non-owning)
};

} // namespace mosaic::ui
