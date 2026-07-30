#pragma once

#include "core/adjustments.hpp"

#include "common/geometry.hpp" // Rect (corner placement region)
#include "common/image.hpp"    // Image (the histogram's backdrop source)
#include "ui/popover.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Fl_Widget;

namespace mosaic::ui {

class ScrubRuler;
class GradientFlyout;

// The S32 adjustment editor (docs/adjustment-layers.md §5): a pinned corner popover on the
// Type/"3D…" panels' placement, shown while the ACTIVE layer is an adjustment layer with
// parameters (the host drives show/hide from the active-layer transitions; the dock badge and
// context menu reopen it explicitly). Rows are GENERATED from the kind's schema -- a ScrubSlider
// per scalar, a CheckBox per toggle -- and every edit streams through the host funnel
// (applyAdjustmentField -> one coalesced SetAdjustmentParamsCommand per control run), so the
// canvas is the live preview and undo composes for free. No OK/Cancel: the layer IS the state
// and undo is the escape hatch; Reset re-seeds the schema defaults as an ordinary undoable edit.
class AdjustmentPanel : public Popover {
public:
    AdjustmentPanel();
    ~AdjustmentPanel() override;

    // The edit funnel (the Type panel's setOnBlockEdit twin): `fieldId` scopes undo coalescing
    // per control ("adjust:<key>", "adjust:reset"); `mutate` rewrites the params bag.
    void setOnEdit(std::function<void(const std::string& fieldId,
                                      std::function<void(std::map<std::string, double>&)>)>
                       cb) {
        m_onEdit = std::move(cb);
    }
    void setScrubRuler(ScrubRuler* ruler) { m_ruler = ruler; }
    // The histogram source for the Levels/Threshold strips: the target adjustment's BACKDROP
    // (render::adjustmentBackdrop through the host), a small doc-proportional image. Re-queried
    // when the panel starts showing a different layer.
    void setBackdropProvider(std::function<common::Image()> cb) { m_backdrop = std::move(cb); }
    // Corner placement (the Type panel's behaviour): pin to the canvas region's bottom-right.
    void setPlacementProviders(std::function<common::Rect()> region);

    // ---- S34-a: the two rows that need a host-owned editor bubble ----------------------------
    // Gradient Map's ramp row opens the host's shared GradientFlyout on its PaintChip (the
    // ImageOpsPanel Fill pattern -- the flyout's onChange is re-pointed on every open, so one
    // bubble can serve several openers). Without one the chip is inert but the layer still
    // renders and still round-trips; the ramp simply cannot be edited from here.
    void setGradientFlyout(GradientFlyout* f) { m_gradientFlyout = f; }
    // Photo Filter's "Custom" colour opens the host's shared colour bubble on its swatch, the
    // Type/Image-ops `setOnEditColor` contract verbatim. The host's pick router then hands the
    // chosen colour back through setPickedColor while this panel is the one on screen. With no
    // hook wired the swatch is a read-only preview and the fourteen named presets still work.
    void setOnEditColor(std::function<void(const Fl_Widget* anchor, common::Color8 current)> cb) {
        m_onEditColor = std::move(cb);
    }
    void setPickedColor(common::Color8 c);

    // Point the panel at `layer`: rebuilds the rows when the kind changes, otherwise just
    // re-syncs drifted values (undo/redo moved the bag under us). Show/hide stays with the host.
    void reflect(const core::AdjustmentLayer& layer);
    [[nodiscard]] core::LayerId target() const noexcept { return m_target; }

    void openFor(const Fl_Widget* anchor); // place at the corner + show (pinned)
    void hide() override;                  // ... and take any editor bubble of ours down with it
    void reapplyTheme() override;          // re-theme + rebuild the generated rows in place

    // Fake translucency while the panel occludes the document (a child sub-window gets no
    // compositor alpha on X11): the host supplies "what the canvas shows beneath the panel
    // rect" and a fade level; refreshFadeBlend() then renders ground + children offscreen,
    // blends them over the under-image, and draw() blits the CACHED result. The blend is built
    // OUTSIDE draw() on purpose -- creating an Fl_Image_Surface inside a draw() corrupts the
    // active graphics context (the round-3 crash) -- so the HOST calls refreshFadeBlend when
    // the blend is stale: blendDirty() (a content sync / fade change) or its own fingerprint of
    // what the canvas shows beneath (composite revision + view zoom/pan/rotation + panel rect).
    void setUnderProvider(std::function<common::Image(int w, int h)> cb) {
        m_under = std::move(cb);
    }
    void setFade(double opacity); // clamped to [0.25, 1]; marks the blend stale on change
    [[nodiscard]] double fade() const noexcept { return m_fade; }
    [[nodiscard]] bool blendDirty() const noexcept { return m_blendDirty; }
    void refreshFadeBlend(); // rebuild the cached blend + redraw (host-called, never from draw)

protected:
    void drawContent() override; // opaque Popover draw at fade 1; blits the cached blend below it

private:
    void build(core::AdjustmentKind kind); // (re)generate header + rows + reset from the schema
    void syncValues(const std::map<std::string, double>& bag);
    void openGradientFlyout(); // Gradient Map's ramp chip -> the host's shared bubble
    void openFilterColor();    // Photo Filter's Custom swatch -> the host's shared colour bubble

    struct State;
    std::unique_ptr<State> m_state;
    core::LayerId m_target = core::kInvalidLayerId;
    core::AdjustmentKind m_kind = core::AdjustmentKind::Invert;
    bool m_built = false;

    std::function<void(const std::string&, std::function<void(std::map<std::string, double>&)>)>
        m_onEdit;
    std::function<common::Image()> m_backdrop; // histogram source (see setBackdropProvider)
    std::function<void(const Fl_Widget*, common::Color8)> m_onEditColor; // Photo Filter's swatch
    GradientFlyout* m_gradientFlyout = nullptr;                          // host-owned, not ours
    std::function<common::Image(int, int)> m_under; // what the canvas shows beneath (fade)
    double m_fade = 1.0;
    std::vector<unsigned char> m_blend; // cached faded raster (RGB triples, device res)
    int m_blendRW = 0;                  // its raster size (device px under a scaled screen)
    int m_blendRH = 0;
    bool m_blendDirty = true; // content/fade changed since the last refreshFadeBlend
    ScrubRuler* m_ruler = nullptr;
};

} // namespace mosaic::ui
