#pragma once

#include "ui/popover.hpp"

#include "common/geometry.hpp"       // Rect (corner placement region / obstacle)
#include "common/image.hpp"          // Image (the rendered preview) / ColorF
#include "core/text/extrude.hpp"     // Extrude params
#include "core/text/text_model.hpp"  // TextBlock (the block-edit funnel)

#include <functional>
#include <memory>
#include <optional>
#include <string>

class Fl_Widget;

namespace mosaic::ui {

class ScrubRuler;

// The Type tool's 3D popup (docs/type-tool.md §8.4, S30-d): opened by the bar's "3D…" button,
// showing a LIVE viewport of the edited text solid with on-handle manipulation -- free trackball
// orbit, the three constrained rotation rings, a depth handle along the solid's +z axis, a bevel
// knob, and a light-direction sphere when lighting is on -- plus the hottest numeric controls
// (depth / bevel / perspective / lighting / material). Every edit streams through the SAME
// block-level funnel the Type panel uses (applyTextBlockField -> applyTextBlockEdit, coalesced per
// control), so the CANVAS is the real live preview and undo composes for free; the viewport just
// re-renders the same block through the same renderTextF pipeline (GPU lane and all) at panel
// scale -- what you sculpt is exactly what composites (§8.4).
class Type3dPanel : public Popover {
public:
    Type3dPanel();
    ~Type3dPanel() override;

    // The block-level edit funnel (shared undo-coalescing with the bar/Type panel via `id`).
    void setOnBlockEdit(std::function<void(const std::string& id,
                                           std::function<void(core::text::TextBlock&)>)> cb) {
        m_onBlockEdit = std::move(cb);
    }
    // Render the CURRENT edited block with `params` applied, fitted into w x h (straight-alpha
    // 8-bit RGBA). The host routes this through renderTextF so the preview IS the canvas pipeline.
    // `fitScale` (out, may be null) reports the design-px -> viewport-px fit factor -- the depth
    // handle's gain rides it so a drag moves the solid about as fast as the pointer.
    void setPreviewRenderer(std::function<common::Image(const core::text::Extrude& params, int w,
                                                        int h, double* fitScale)>
                                cb) {
        m_renderPreview = std::move(cb);
    }
    // The material colour line's "Edit…" click: the host opens the shared ColorFlyout anchored at
    // `anchor`, seeded with `current` (picks flow back through the block funnel, not this panel).
    void setOnEditColor(std::function<void(const Fl_Widget* anchor, common::ColorF current)> cb) {
        m_onEditColor = std::move(cb);
    }
    void setScrubRuler(ScrubRuler* ruler);

    // Corner placement (the Type panel's behaviour, docs/type-tool.md §8): pin to the canvas
    // region's bottom-right, flipping away from the edited text -- NOT hanging off the bar button.
    void setPlacementProviders(std::function<common::Rect()> region,
                               std::function<std::optional<common::Rect>()> avoid);

    // Push the edited block's state into the controls + re-render the viewport. `hasSession` false
    // greys everything (no text being edited); `ex` nullopt = 3D off (the enable toggle governs).
    void reflect(const std::optional<core::text::Extrude>& ex, bool hasSession);

    void toggle(const Fl_Widget* anchor);
    void reapplyTheme() override;

    // Internal thunk targets (public for the C callbacks only).
    void applyControl(int role);

private:
    void build();
    // Footprint = content height clamped to the placement region (the canvas) -- the ScrollView
    // takes any overflow, so a short window scrolls the panel instead of overflowing it.
    void resizeToContent();
    // The single write path: read-modify-write the block's optional Extrude through the funnel
    // (dropped when 3D is off -- the controls are greyed then, so it's a stale event). Gizmos
    // pass their own coalesce ids ("extrude:orientation" etc.).
    void commitExtrude(const char* id, std::function<void(core::text::Extrude&)> mutate);
    struct State;
    std::unique_ptr<State> m_state;
    std::function<common::Rect()> m_region;  // the canvas rect (resizeToContent's height clamp)
    int m_contentH = 0;                      // laid-out content height (build() computes it)
    std::optional<core::text::Extrude> m_current;  // last reflected state (viewport + gizmo basis)
    bool m_hasSession = false;
    bool m_reflecting = false;
    double m_viewScale = 1.0;  // design px -> viewport px (from the last preview render)

    std::function<void(const std::string&, std::function<void(core::text::TextBlock&)>)>
        m_onBlockEdit;
    std::function<common::Image(const core::text::Extrude&, int, int, double*)> m_renderPreview;
    std::function<void(const Fl_Widget*, common::ColorF)> m_onEditColor;
    ScrubRuler* m_scrubRuler = nullptr;

    friend class Extrude3dViewport;
};

}  // namespace mosaic::ui
