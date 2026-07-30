#pragma once

#include "common/exif.hpp"
#include "common/image.hpp"
#include "core/selection.hpp"
#include "core/texture/sky_estimate.hpp"
#include "core/texture/texture_params.hpp"
#include "core/texture/texture_render.hpp"

#include <FL/Fl_Double_Window.H>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The Texture Generator modal (S55-f; docs/texture-generator.md §7): Layer ▸ Texture Generator…
// opens a transactional two-pane editor -- a settings-style generator RAIL on the left (the suite
// grows, §7.2), per-generator controls with progressive disclosure in the middle, and a LIVE
// preview + gizmo pane on the right. The preview is a low-res proxy rendered on a background
// worker (core::texture::TextureRenderWorker) so a gizmo drag or a volumetric march never pins
// the UI; Fit shows the whole document framing, 1:1 pans a byte-exact window of the full-res
// frame (the §8.2 TextureWindow). Create bakes the FULL document resolution on the same worker
// behind a progress bar (cancellable), then hands params + pixels to the host -- one undo step,
// no synchronous re-render. Re-opening with a TextureLayer selected edits THAT layer (§3.3).
namespace mosaic::core::texture {
class TextureRenderWorker; // render_worker.hpp (the dialog's background renderer)
class SkyEstimateWorker;   // sky_estimate_worker.hpp (the "Estimate from layer" runner)
}

namespace mosaic::ui {

class MapFlyout;
class ScrubRuler;

// Callbacks the dialog invokes; decouples it from MainWindow (the FillHost/LayerEffectsHost
// pattern). The host captures the target document (and, in edit mode, the layer) at open time.
struct TextureGenHost {
    // The active foreground colour, for the colour flyout's "use foreground" shortcut.
    std::function<common::Color8()> foreground;
    // Commit the transaction: `params` is the final value and `baked` its COMPLETE full-document
    // render (the dialog baked it behind the progress bar). Create mode: insert a fresh
    // TextureLayer (one AddLayerCommand); edit mode: one SetTextureCommand on the seeded layer.
    // Either way install `baked` via texture::applyBakedTextureCache so the pre-composite refresh
    // finds a current cache instead of re-rendering synchronously. Called at most once; the
    // dialog hides itself right after.
    std::function<void(core::texture::TextureParams params,
                       core::texture::TextureRenderResult baked)>
        commit;

    // ---- "Estimate from layer" (S55; docs/research-sky-estimate-from-layer.md §7) ----
    // The active layer's pixels in DOCUMENT space (the activeLayerDocImage machinery the magic
    // wand shares) + its name and any EXIF the photo carried. nullopt when the active layer owns
    // no pixels -- the estimate button disables with the wand's copy-family hint. The dialog is
    // modal, so the active layer cannot change while it is open.
    struct SourceLayer {
        std::string name;
        common::Image docImage;
        std::optional<common::ExifData> exif;
    };
    std::function<std::optional<SourceLayer>()> sourceLayer;

    // The ACCEPT-time "mask & harmonize" product (design §6.3), computed by the dialog after the
    // full-res bake: the doc-space sky Selection (S6) and the PhotometricMatch scalar bag (S7).
    struct ConformPayload {
        core::Selection skySelection;
        std::map<std::string, double> matchParams;
    };
    // Commit the conform shape instead of the plain layer: ONE CompositeCommand assembling
    // sky-below + foreground mask on the source layer + the clipped harmonization grade
    // (core::texture::buildSkyConformCommand). Only ever invoked when the user armed the toggle
    // THIS session and the estimate's segmentation cleared its floor -- the estimate, the toggle
    // and ACCEPT stay individually-invoked user steps (the user-driven-compositing discipline). Optional:
    // when absent the dialog never offers the toggle.
    std::function<void(core::texture::TextureParams params,
                       core::texture::TextureRenderResult baked, ConformPayload conform)>
        commitConform;
};

// The preset the current spec equals: 0 = "Custom", else 1 + the library index for the active
// generator (sky/paper/grass preset libraries). Seed and Scale are the user's own -- only the
// spec arm is compared (a preset is a LOOK, not a placement). Pure; exposed for tests.
[[nodiscard]] int texturePresetIndex(const core::texture::TextureParams& p);

// The state behind the info panel's drawn date/time clock ("Sky at this date & place"): the
// observer's LOCAL calendar + wall time -- mean solar time, UTC + longitude/15, the almanac's own
// utcHourToLocal convention (honest without a timezone database; solar noon reads 12) -- plus the
// sun elevation that tints the face day/twilight/night. Refreshed by every updateSkyInfo();
// exposed whole for the headless tests (assert state, not pixels).
struct SkyClockState {
    bool visible = false;         // the sky info panel (and with it the clock) is showing
    int year = 0;                 // observer-local calendar date (may differ from the UTC date)
    int month = 0;                // 1..12
    int day = 0;                  // 1..daysInMonth
    double localHours = 0.0;      // observer-local wall time, fractional hours [0, 24)
    double sunElevationDeg = 0.0; // apparent; drives the face tint (day / twilight / night)
};

class TextureGeneratorDialog : public Fl_Double_Window {
public:
    explicit TextureGeneratorDialog(TextureGenHost host);
    ~TextureGeneratorDialog() override;

    // Point the dialog at a document: the full-res bake target. `editing` (nullopt = create mode)
    // carries the selected TextureLayer's name + params -- the dialog seeds from them and the
    // footer reads "Applies to …" with an Apply button (§3.3 select-to-edit). Call before show().
    void seed(std::uint32_t docW, std::uint32_t docH,
              std::optional<std::pair<std::string, core::texture::TextureParams>> editing);

    void reapplyTheme(); // runtime theme change while open (MainWindow's observer)

    // Programmatic control (also the headless-test surface, the test_type3d_panel precedent).
    void selectGenerator(core::texture::Generator g);
    void applyPreset(std::size_t libraryIndex); // index into the active generator's preset library
    void randomizeSeed();
    void create();   // start the full-res bake (the Create/Apply button + Enter)
    void pollOnce(); // the poll timer's body: take worker results, drive progress/commit
    [[nodiscard]] const core::texture::TextureParams& params() const;
    [[nodiscard]] bool baking() const { return m_baking; }

    // ---- "Estimate from layer" (S55 phase 2; docs/research-sky-estimate-from-layer.md §7) ----
    // Start the background estimate against the host's source layer (the sky stack's action row;
    // also the headless-test surface). No-op while a bake/estimate runs or without a source.
    void estimateFromLayer();
    // Restore the pre-estimate SkyParams snapshot (the action row's Revert button).
    void revertEstimate();
    // Swap the almanac inversion's morning/afternoon solution (the summary's alternative link).
    void swapEstimateTime();
    [[nodiscard]] bool estimating() const { return m_estimating; }
    [[nodiscard]] bool conforming() const { return m_conform != nullptr; }
    // The last completed estimate (nullopt until one lands), the mask & harmonize toggle's state,
    // and whether the toggle is currently offered -- the headless-test surface.
    [[nodiscard]] const std::optional<core::texture::SkyEstimateResult>& estimateForTest() const {
        return m_estimate;
    }
    [[nodiscard]] bool conformWanted() const { return m_conformWanted; }
    [[nodiscard]] bool conformOffered() const;
    void setConformWanted(bool on); // the toggle's programmatic twin (tests)
    // The estimate summary lines exactly as the control stack shows them (headless assertions).
    [[nodiscard]] std::vector<std::string> estimateSummaryForTest() const;
    // The action row's button (nullptr outside the sky stack) and the hover bubble (constructed
    // pre-show in the constructor) -- the widget-level headless-test surface.
    [[nodiscard]] Fl_Widget* estimateButtonForTest() const;
    [[nodiscard]] Fl_Widget* estimateBubbleForTest() const;

    // ---- Observer clock / moon-phase source (S55 night dialog rework) --------------------------
    // Set the observer date/time (UTC) + place; drives the sun (and, while the moon-phase source is
    // "from date & place", the whole master clock) and the info panel. Also the map pin's sink.
    void setObserver(int year, int month, int day, double hourUtc, double latDeg, double lonDeg);
    void setObserverLatLon(double latDeg, double lonDeg); // typed/map-picked/dragged coordinates
    // Explicitly choose the moon-phase source (1 = from date & place / ephemeris, 2 = manual). An
    // explicit choice always wins over the first-set-wins latch.
    void selectMoonSource(int source);
    // 0 = not yet latched, 1 = from date & place (ephemeris), 2 = manual (dialog-local, NOT a
    // SkyParams field -- exposed for the latch tests).
    [[nodiscard]] int moonSource() const { return m_moonSource; }
    // Open a disclosure section (key e.g. "sky:solar") and rebuild -- the headless-test surface for
    // the sections that are collapsed by default (exercising the date-picker re-parent + city row).
    void openSectionForTest(const std::string& key);
    // The info panel's clock as last pushed by updateSkyInfo() (the headless-test surface).
    [[nodiscard]] SkyClockState skyClockForTest() const { return m_skyClock; }
    // The world-map place flyout (the headless-test surface for the pin -> observer pick flow).
    [[nodiscard]] MapFlyout* mapFlyoutForTest() const;

    void show() override; // + neutralise the consumed date-picker's auto-opening pop-up

protected:
    int handle(int event) override; // Enter = Create/Apply, Esc = Cancel; flyout dismissal

private:
    struct Ui; // widget pointers + bindings (defined in the .cpp)

    // The ONE Fit/1:1 framing truth: what frame the current view renders and which window of it
    // is visible. Both the proxy request and the preview pane's gizmo mapping read this -- two
    // hand-kept copies of the fit math would let the handles drift off the rendered pixels.
    struct ViewSpec {
        std::uint32_t frameW = 0, frameH = 0; // the camera frame (proxy dims in Fit; doc in 1:1)
        long winX = 0, winY = 0;              // window origin within the frame (1:1 pan)
        std::uint32_t viewW = 0, viewH = 0;   // visible frame pixels (== frame in Fit)
    };
    [[nodiscard]] ViewSpec viewSpec() const;

    void build();            // (re)create rail/top-strip/preview/footer chrome
    void rebuildControls();  // (re)create the middle control stack for the active generator
    // The shared control-stack builder kit (caption/slider/check/dropdown/colour/section row
    // helpers over the ACTIVE params; defined in the .cpp) and the per-generator stacks built
    // with it -- rebuildControls dispatches through a builder table, so a new generator adds one
    // builder here plus its table row (the S55-g registry discipline).
    struct ControlsCtx;
    void buildSkyControls(ControlsCtx& c);
    void buildPaperControls(ControlsCtx& c);
    void buildGrassControls(ControlsCtx& c);
    void buildWoodControls(ControlsCtx& c);
    void buildMarbleControls(ControlsCtx& c);
    void buildStoneControls(ControlsCtx& c);
    void buildCanvasControls(ControlsCtx& c);
    void buildMetalControls(ControlsCtx& c);
    void syncControls();     // push m_params values into the existing widgets (no rebuild)
    void applyEdit(const std::function<void(core::texture::TextureParams&)>& mutate);
    void requestProxy();     // queue a coalesced proxy render for the current params/view
    static void pollTimer(void* self);
    void applySolar();       // time & place -> sun azimuth/elevation (the §4.2 calculator)
    static void calendarGuard(void* self); // close the date-picker pop-up over the first few frames
    void observerChanged(bool fromDatePlaceControl); // recompute sun / master clock + info panel
    void updateSkyInfo();    // push the computed almanac into the under-preview info panel
    void setMoonSourceInternal(int source, bool applyClock); // the latch mechanics (see .cpp)
    [[nodiscard]] std::string moonPhaseReadout() const;      // the current phase name for the readout
    void cancelBake();
    void doCancel();
    void openColorFlyout(const Fl_Widget* anchor, common::Color8 current,
                         std::function<void(common::Color8)> onPick);
    void openMapFlyout(const Fl_Widget* anchor); // toggle the world-map place flyout
    // ---- estimate-from-layer plumbing (phase 2) ----
    void fenceForWork(const char* note);   // deactivate the edit surface (bake + estimate share it)
    void unfenceAfterWork();               // reactivate after a bake/estimate ends or cancels
    void finishEstimate(core::texture::SkyEstimateResult est); // apply + summarize a result
    void cancelEstimate();                 // the footer Cancel while an estimate runs
    void updateEstimateUi();               // button/revert/toggle enable states + labels
    void estimateHover(bool inside);       // the action row's hover -> the EstimateBubble
    void applyEstimateHover();             // ... the deferred half (see the .cpp: re-entrancy)
    static void estimateHoverTimeout(void* self);
    void startConform(core::texture::TextureParams params,
                      core::texture::TextureRenderResult baked); // ACCEPT: S6+S7 off-thread
    void finishConform();                  // conform thread done: commit + hide (or fall back)
    void cancelConform();                  // Cancel during the mask/harmonize stages

    [[nodiscard]] core::texture::TextureParams& activeParams();
    [[nodiscard]] const core::texture::TextureParams& activeParams() const;
    // The morning/afternoon alternative for the last estimate's time, recomputed from the S5
    // helper (nullopt when no time was applied or the day has a single crossing).
    [[nodiscard]] std::optional<core::texture::SkyTimeInversion> estimateTimeSwap() const;

    TextureGenHost m_host;
    // One working value PER generator, so flipping the rail never loses edits (§7.2). The active
    // arm is m_generator's.
    std::array<core::texture::TextureParams, core::texture::kGeneratorCount> m_working;
    core::texture::Generator m_generator = core::texture::Generator::Sky;
    std::uint32_t m_docW = 0, m_docH = 0;
    bool m_editing = false;
    std::string m_editLabel;

    std::unique_ptr<core::texture::TextureRenderWorker> m_worker;
    std::uint64_t m_epoch = 0;      // proxy request generation (stale results dropped)
    std::uint64_t m_bakeEpoch = 0;  // the full-res job's epoch (0 = no bake running)
    bool m_baking = false;

    // ---- "Estimate from layer" state (phase 2). All per-session: seed() resets it. ----
    std::unique_ptr<core::texture::SkyEstimateWorker> m_estimator;
    std::uint64_t m_estimateEpoch = 0;  // the in-flight estimate's epoch (0 = none)
    bool m_estimating = false;
    std::optional<core::texture::SkyEstimateResult> m_estimate;  // the last completed result
    std::optional<core::texture::SkyParams> m_estimateSnapshot;  // pre-estimate value (Revert)
    common::Image m_sourceImage;   // the doc-space photo the estimate analyzed (S6 reads THIS)
    std::string m_sourceName;      // its layer name (the bubble + commit label)
    bool m_conformWanted = false;  // the footer toggle; NEVER persists across seed()
    // The ACCEPT-time S6+S7 run (one worker thread, cancellable via its progress channel);
    // non-null only between the bake landing and the conform commit/cancel.
    struct ConformRun;
    std::unique_ptr<ConformRun> m_conform;
    common::Image m_bubbleThumb;   // the hover bubble's lazily-built thumbnail (session cache)
    bool m_bubbleThumbReady = false;
    bool m_estimateHoverInside = false; // the hover state the deferred tick will apply
    bool m_estimateHoverArmed = false;  // an estimateHoverTimeout is pending
    bool m_sourceAvailable = false;  // probed once per seed() (modal: it cannot change after)
    bool m_polling = false;         // the poll timeout is armed
    bool m_seeding = false;         // guard: programmatic value-sets must not fire edits
    int m_calendarGuardTicks = 0;   // frames left to keep the auto-opened date pop-up closed
    std::map<std::string, bool> m_open; // disclosure-section state, per "generator:section" key

    // The solar calculator's inputs are DIALOG state, not SkyParams (§4.2: time/place is a
    // calculator over az/el, which is what the document stores). Seeded to today noon UTC.
    int m_solYear = 2026, m_solMonth = 6, m_solDay = 21;
    double m_solHour = 12.0, m_solLat = 51.5, m_solLon = -0.1;

    // What the info panel's clock currently shows (kept dialog-side so tests can read it).
    SkyClockState m_skyClock;

    // The moon-phase source latch (first-set-wins), DIALOG-local -- NOT a SkyParams field. 0 = not
    // yet latched, 1 = from date & place (ephemeris, SkyParams.moonPhaseMode 2), 2 = manual
    // (moonPhaseMode 1 + the illuminated-fraction slider). Whichever of {the date/place controls}
    // or {the manual phase} the user touches first this session latches; an explicit source pick in
    // "Night & moon" always overrides.
    int m_moonSource = 0;

    ScrubRuler* m_ruler = nullptr; // owned child sub-window (must share the sliders' top_window)
    std::function<void(common::Color8)> m_onColorPick; // active colour-flyout sink

    std::unique_ptr<Ui> m_ui;

    friend class TexturePreviewPane;
};

} // namespace mosaic::ui
