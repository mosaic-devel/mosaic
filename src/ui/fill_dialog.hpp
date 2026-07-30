#pragma once

#include "common/image.hpp"
#include "core/blend_mode.hpp"
#include "core/vector/paint.hpp" // core::vec::Gradient / ProceduralPattern (the custom paint state)
#include "ui/preview_pane.hpp"   // PreviewContent (compositePreview's return)

#include <FL/Fl_Double_Window.H>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

// The Edit→Fill… dialog (PLAN S39 / settled design 2026-06-22): a transactional modal — set up the
// fill, click Fill to apply once, Cancel to discard. A two-column layout (controls left, a live
// preview pane right, since the modal dims the canvas behind it). Cheap Contents (Foreground /
// Background / a fixed grey) preview live and commit one undoable core::FillCommand; the Inpaint
// content greys out the blend controls and, on Fill, hands off to the proven async inpaint path
// (status-bar progress + cancel, results on the canvas). Mirrors the SettingsDialog host pattern.
namespace mosaic::ui {

// What the dialog needs to know about the current fill target — captured by the host at open time
// so the dialog can preview + commit without reaching into the document model.
struct FillContext {
    bool hasRasterTarget = false;  // false → no active raster layer (the dialog shows a notice)
    bool selectionActive = false;  // a selection limits the fill (else the whole layer)
    bool inpaintAvailable = false; // the engine has a runnable backend

    // The tight layer-local region a solid fill will touch (bbox of coverage>0) and the per-pixel
    // selection coverage over it (size == region.width*region.height; an empty vector == fully
    // covered). Empty `region` means the selection misses the layer → nothing to fill.
    common::Image region;
    std::vector<std::uint8_t> coverage;
    long originX = 0;
    long originY = 0;
};

// Callbacks the dialog invokes. Decouples it from MainWindow (the SettingsHost pattern).
struct FillHost {
    std::function<common::Color8()> foreground; // the active foreground colour
    std::function<common::Color8()> background; // the active background colour
    // The document-wide AA setting (the Move tool's AA combobox): hardens/softens pattern edges so
    // a pattern fill + its flyout preview read like the canvas. Absent → treated as antialiased.
    std::function<bool()> antialias;
    // Commit a solid fill: the already-computed layer-local region pixels at (ox, oy) → one
    // undoable core::FillCommand + a scoped recomposite.
    std::function<void(common::Image regionPixels, long ox, long oy)> commitFill;
    // Composite the document for the preview pane: temporarily apply `regionPixels` at (ox, oy) to
    // the active layer, composite a document region around it (all layers + this layer's blend mode
    // and opacity), restore, and return it as a PreviewContent (a straight-alpha image + the canvas
    // rect within it). The region is centred on the fill area, expanded to the (paneW x paneH) pane
    // aspect + a margin, and rendered UNCLAMPED so a fill near the canvas edge shows the off-canvas
    // backdrop.
    std::function<PreviewContent(const common::Image& regionPixels, long ox, long oy, int paneW,
                                 int paneH)>
        compositePreview;
    // Run the inpaint engine over the current selection SYNCHRONOUSLY and return the inpainted
    // layer region [(ox,oy), (w,h)] — for the in-dialog Preview button (cached, then committed on
    // Fill). `onProgress(fraction, stage)` is called each engine tick (the host repaints around
    // it); it returns false to request cancellation (the dialog returns its Esc state). nullopt =
    // no result (failed or cancelled).
    std::function<std::optional<common::Image>(
        long ox, long oy, std::uint32_t w, std::uint32_t h,
        const std::function<bool(float fraction, const std::string& stage)>& onProgress)>
        runInpaintRegion;
    // Run the inpaint engine over the current selection on the active layer (the S39-b async path).
    // Used by Fill when no cached preview exists; the dialog closes first.
    std::function<void()> runInpaintFill;
};

class FillDialog : public Fl_Double_Window {
public:
    explicit FillDialog(FillHost host);
    ~FillDialog() override;

    // Seed the target + reset the controls to defaults; call before show().
    void seed(const FillContext& ctx);

protected:
    int handle(int event) override; // Enter = Fill, Esc = Cancel

private:
    enum class Contents {
        Foreground,
        Background,
        White,
        Black,
        Gray,
        Custom,
        Gradient,
        Pattern,
        Inpaint
    };

    void onContentsChanged();
    void openColorFlyout(); // the "Color…" content: anchor the compact picker at the swatch chip
    void
    openGradientFlyout(); // the "Gradient…" content: anchor the gradient editor at the paint chip
    void openPatternFlyout(); // the "Pattern…" content: anchor the pattern editor at the paint chip
    void onGradTypeChanged(); // the Linear/Radial/Conic dropdown reshaped the working gradient
    void onGradDirChanged();  // the direction dial rotated the working gradient's transform
    void refreshPaintChip();  // push the active gradient/pattern into the paint chip
    [[nodiscard]] core::vec::Paint
    currentPaint() const; // the paint the current Contents fills with
    // Queue a preview recompute, coalesced to ~once per frame, so a continuous slider drag can't
    // pin the UI thread doing a full CPU compositeRegion per tick (the readout updates every tick;
    // only this expensive recompute is throttled).
    void requestPreview();
    static void previewTimer(void* self);
    void recomputePreview();
    void doPreview(); // inpaint: run the engine once, cache + show it in the pane
    void doFill();
    void doCancel();
    [[nodiscard]] Contents contents() const;
    [[nodiscard]] common::Color8 currentColour() const;
    [[nodiscard]] bool isInpaint() const { return contents() == Contents::Inpaint; }
    // Gradient/Pattern fill the region with a non-solid vec::Paint (via computeFillPaint) rather
    // than a flat colour; both use the paint chip + a flyout instead of the swatch.
    [[nodiscard]] bool isPaintContents() const {
        return contents() == Contents::Gradient || contents() == Contents::Pattern;
    }
    [[nodiscard]] core::BlendMode mode() const;
    [[nodiscard]] float opacityF() const;
    [[nodiscard]] bool protectAlpha() const;

    FillHost m_host;
    FillContext m_ctx;

    class Dropdown* m_contents = nullptr;
    class Dropdown* m_mode = nullptr;
    class Dropdown* m_gradType = nullptr;       // Linear/Radial/Conic (shown for "Gradient…")
    class Dial* m_gradDir = nullptr;            // gradient direction dial (shown for "Gradient…")
    class ColorFlyout* m_colorFlyout = nullptr; // the compact picker for "Color…"
    class GradientFlyout* m_gradientFlyout = nullptr; // the stops/spread editor for "Gradient…"
    class PatternFlyout* m_patternFlyout = nullptr;   // the kind/colour editor for "Pattern…"
    class ScrubRuler* m_ruler = nullptr; // shared precision HUD for the pattern flyout's sliders
    common::Color8 m_customColour{0, 0, 0, 255}; // the "Color…" pick (seeded from the foreground)
    core::vec::Gradient m_customGradient; // the "Gradient…" working gradient (kept across switches)
    core::vec::ProceduralPattern
        m_customPattern; // the "Pattern…" working pattern (kept across switches)
    class Slider* m_opacity = nullptr;
    Fl_Widget* m_opacityReadout = nullptr;  // "100 %" box
    Fl_Widget* m_protectAlpha = nullptr;    // CheckBox
    Fl_Widget* m_swatch = nullptr;          // contextual colour preview chip (solid contents)
    class PaintChip* m_paintChip = nullptr; // contextual paint chip (gradient / pattern contents)
    Fl_Widget* m_preview = nullptr;         // PreviewPane
    Fl_Widget* m_previewBtn = nullptr;      // "Preview" (inpaint only), under the pane
    Fl_Widget* m_header = nullptr;          // "Filling: …" line
    Fl_Widget* m_note = nullptr;            // contextual note (inpaint / no-target)
    class FlatButton* m_cancel = nullptr;
    Fl_Widget* m_fill = nullptr; // FilledButton (primary)

    common::Image
        m_inpaintCache; // the last Previewed inpaint region (committed by Fill, no re-run)
    bool m_inpaintCacheValid = false;
    bool m_previewPending =
        false; // a frame-coalesced recomputePreview() is queued (see requestPreview)
    bool m_previewing = false; // a synchronous inpaint Preview is running (re-entrancy guard)
    bool m_previewCancel =
        false; // Esc during a Preview run requests cancellation (read by onProgress)
};

} // namespace mosaic::ui
