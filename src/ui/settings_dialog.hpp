#pragma once

#include "common/image.hpp" // the Icons pack browser's rendered previews travel as pixels
#include "common/settings.hpp"
#include "core/inpaint/inpaint_backend.hpp" // BackendInfo / BackendSettingsSchema for the Inpainting pane

#include <FL/Fl_Double_Window.H>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Fl_Group;
class Fl_Input;
class Fl_Output;
class Fl_Widget;

// The Settings window (PLAN S51-a): a modal, instant-apply preferences surface -- a left category
// rail + a right content pane, the Krita / VS Code / Blender shape. Modal per the creative-app
// convention (Affinity / Photoshop / Krita all block the app while Preferences is open; it also
// keeps the window off the taskbar). Each control writes its value the moment it changes (the host
// persists + applies it live); there is no OK / Apply / Cancel, only Done (Esc also closes). Live
// theme/units changes still render on the canvas behind the dialog. The category IA is
// deliberately coherent (see the
// `settings-dialog-coherence` memory): General / Appearance / Tools / Color Management /
// Annoyances. This first cut stands up the surface + the two already-backed settings (Units, the
// CMYK profile override); the remaining sections land alongside their feature work.
namespace mosaic::ui {

class Keymap; // Settings → Keybindings renders the LIVE keymap; see SettingsHost::keymap

// Callbacks the dialog invokes when a setting changes. Each implementation does a read-modify-write
// to settings.json -- so a concurrent picker-surface / recents write is never clobbered (the file
// is persistence, not the source of truth) -- AND applies the runtime effect. Keeping these as
// plain std::functions decouples the dialog from MainWindow.
struct SettingsHost {
    std::function<void(const std::string& units)> setUnits; // "auto"|"metric"|"imperial"
    // Apply + persist the theme mode ("system"|"dark"|"light"); re-themes the whole UI live.
    std::function<void(const std::string& themeMode)> setThemeMode;
    // Apply + persist the Appearance "Selection & reticle line" overlay style ("classic"|"rim"|
    // "adaptive", default "rim"); restyles whatever canvas chrome is on screen live.
    std::function<void(const std::string& style)> setOverlayLineStyle;
    // Apply + persist the Appearance "Feathered selection indicator" ("bracket" default | "band");
    // re-indicates whatever selection is on the canvas live.
    std::function<void(const std::string& style)> setFeatherIndicator;
    // --- Appearance → Icons (S52): the tool icon pack browser ------------------------------
    // The installed packs in browse order (the embedded default first), with the credit lines the
    // browser shows. Filled from ui::IconPacks when the dialog is built; a rescan needs a rebuild.
    struct IconPackDesc {
        std::string id;
        std::string name;
        std::string artist;
        std::string link;        // website / social handle / e-mail, one line ("" = hidden)
        std::string description; // one short paragraph ("" = hidden)
    };
    std::vector<IconPackDesc> iconPacks;
    // One tool key of one pack rendered at px (per-icon default fallback + vector-or-raster
    // handling already applied by IconPacks::renderIcon); the dialog builds its preview strips
    // through this and never touches an SVG itself.
    std::function<common::Image(const std::string& packId, const std::string& key, int px)>
        renderIcon;
    // Apply + persist the selected pack; re-rasterizes the toolbar live.
    std::function<void(const std::string& packId)> setIconPack;
    // Apply + persist the CMYK profile. "" reverts to the built-in default. Returns false if a
    // non-empty path failed to load as a CMYK profile (the dialog then warns + keeps the old value).
    std::function<bool(const std::string& cmykProfile)> setCmykProfile;
    // Apply + persist the Tools / Crop "switch to the previous tool after applying a crop" setting
    // (S16-p). True = switch away from Crop after Apply; false = stay on Crop (the default).
    std::function<void(bool on)> setCropSwitchToolAfterApply;
    // Apply + persist the Tools / Crop initial-framing setting (S16-q): "whole-canvas" (default),
    // "inset" or "draw". Applies the next time the tool stages a rect.
    std::function<void(const std::string& framing)> setCropInitialFraming;
    // Apply + persist the Tools / Crop "clear the staged selection when leaving the Crop tool"
    // setting. True = drop the staged rect on tool switch; false (default) = keep it.
    std::function<void(bool on)> setCropClearSelectionOnLeave;
    // Apply + persist the Tools / Move "Multi-selection edits" setting (S15-e): "disabled" (default),
    // "all" or "active". Re-applies live to the layer panel's blend/opacity strip.
    std::function<void(const std::string& mode)> setMultiSelectionEdits;
    // Apply + persist the Tools / Lasso "Smooth freehand lasso" setting. True = Catmull-Rom-round the
    // freehand path (preview + commit); false (default) = the raw path. Applies to the next lasso.
    std::function<void(bool on)> setLassoSmooth;
    // Apply + persist the Tools / Brush "preset dock" display mode (S19 §8.2): "cards" (the default
    // -- each preset gets a row carrying its tip and a stroke of that very brush, laid by the real
    // engine) or "grid" (a denser grid of tip tiles). Live: the dock re-lays itself.
    std::function<void(const std::string& mode)> setBrushPresetDisplay;
    // Apply + persist the Tools / Eraser "eraser size follows the brush" tie (S19 §8.4).
    // True (the default) = the Brush's and Eraser's Size options are one shared value
    // (ToolManager::setEraserSizeTie); false = independent sizes.
    std::function<void(bool on)> setEraserSizeFollowsBrush;
    // Apply + persist the Tools / Eraser "eraser preset follows the brush" tie (S19 §8.4). False
    // (the default) = the eraser keeps its own preset. ⚠ The two corpora do not overlap -- a preset
    // belongs to the Eraser because it ERASES -- so "follow" can at most point the eraser at a nib
    // its own picker never offers; the setting is carried because it is a real preference (Photoshop
    // ties the pair, Krita does not) and because the schema has held it since §8.4 landed.
    std::function<void(bool on)> setEraserPresetFollowsBrush;
    // Apply + persist the Annoyances / "Cheesy motivational one-liners" toggle. True = drift an
    // all-caps one-liner under the canvas every few minutes; false (default) = silence. Live.
    std::function<void(bool on)> setMotivationalLines;
    // Apply + persist the Annoyances / unsaved-title settings (S18-d). setShowUnsavedDuration: show
    // "unsaved for N min" past the threshold (default on); setUnsavedIncludeSeconds: tick it at
    // second granularity (default off). Both re-render the window title live.
    std::function<void(bool on)> setShowUnsavedDuration;
    std::function<void(bool on)> setUnsavedIncludeSeconds;

    // --- Text (Settings → Text; Type-tool language features, deferred §2) -------------------
    // Enable live spell-checking of the edited text (the red squiggle overlay). Live: turning it off
    // clears the squiggles at once, on rescans the current block.
    std::function<void(bool on)> setSpellCheck;
    // Also spell-check ALL-CAPS words (decision D4; default off).
    std::function<void(bool on)> setSpellCheckAllCaps;
    // The app-level default text language (BCP-47) behind an empty paragraph language, feeding both
    // spell-check and hyphenation (also wires TextShaper::setDefaultLanguage). "" = follow the OS locale.
    std::function<void(const std::string& lang)> setTextLanguage;
    // Colour-emoji families installed on this machine (FontDB::emojiFamilies), for the Emoji font
    // picker (Type R5); the chosen family becomes the preferred emoji fallback ("" = automatic).
    std::vector<std::string> emojiFamilies;
    std::function<void(const std::string& family)> setEmojiFont;
    // The built-in default CMYK profile's embedded name (e.g. "ISO Coated v2 300% (ECI)"), so the
    // Color Management pane can say *which* profile "Use default" means. "" if none is compiled in.
    std::string defaultCmykName;
    // The embedded description of a *user-chosen* CMYK .icc at `path` (so the field shows the real
    // profile name, like the default does). Recomputed on demand -- the path changes at runtime
    // (Browse...) -- and returns "" if the file can't be read (the field then falls back to the
    // basename). Wired to core::cmykProfileName so the dialog stays free of any lcms2 dependency.
    std::function<std::string(const std::string& path)> cmykProfileName;

    // --- Tablet (Settings → Tablet; docs/tablet.md §8) --------------------------------------
    // One detected device row: what the driver calls it, which tool we classified it as, and which
    // valuators it actually reports. Diagnostic, not configurable -- but it is the difference
    // between "my tablet does not work" and "my tablet has no pressure axis". Filled when the
    // dialog is built, from the live backend.
    struct TabletDeviceRow {
        std::string name;
        std::string tool;
        std::string valuators;
    };
    std::string tabletBackend; // "x11/xi2" | "wayland/zwp_tablet_v2" | "" when none came up
    std::vector<TabletDeviceRow> tabletDevices;
    // Apply + persist the §7 policy pieces. Each takes effect on the next SAMPLE (the policy is
    // applied at ingest), so a change is live even mid-stroke.
    std::function<void(const std::string& curve)> setTabletPressureCurve; // "x,y;x,y;"
    std::function<void(double lo, double hi)> setTabletPressureRange;
    std::function<void(double degrees)> setTabletTiltOffset;
    std::function<void(double maxSpeed, double windowMs)> setTabletSpeed;
    // The live readout the TEST AREA draws (§8: "the single most useful control on the page --
    // it answers 'is my tablet working' without the user having to paint"). Polled on a timer while
    // the pane is visible; reports the last sample the wiring saw, POST-policy, plus the resolved
    // sample rate. `valid` is false when nothing has arrived recently (the pen is away).
    struct TabletReading {
        bool valid = false;
        double pressure = 0.0;
        double xTilt = 0.0;
        double yTilt = 0.0;
        double rotation = 0.0;
        double rateHz = 0.0;
    };
    std::function<TabletReading()> tabletReading;
    // Read the pen over the DIALOG's own window, not just the canvas. Tablet events are delivered
    // per-window on both platforms, so without this the test area answers "no tablet" for exactly as
    // long as the pen is anywhere near it. Called from show() / hide() with the dialog itself; the
    // wiring only ever reads those samples (they are in the dialog's coordinates and can never
    // reach a stroke). See ui::TabletInput::watch.
    std::function<void(Fl_Window*)> tabletWatchWindow;
    std::function<void(Fl_Window*)> tabletUnwatchWindow;

    // --- Inpainting (Settings → Inpainting) -------------------------------------------------
    // One selectable backend, with its engine "spec sheet" + its tunable-control schema. The host
    // fills this from the global InpaintEngine (only available() backends), so the dialog renders
    // the Engine + Backend Settings tabs entirely from data.
    struct InpaintBackendDesc {
        std::string id;
        core::inpaint::BackendInfo info;
        core::inpaint::BackendSettingsSchema schema;
    };
    std::vector<InpaintBackendDesc> inpaintBackends;  // available backends, in engine order
    std::function<void(const std::string& id)> setInpaintBackend;       // select the active backend
    std::function<void(const std::string& presetId)> setInpaintPreset;  // pick a quality preset
    std::function<void(const std::string& key, double value)> setInpaintParam;  // live advanced edit

    // --- Keybindings (Settings → Keybindings; S51-b, docs/keybindings.md) -------------------
    // The LIVE keymap, borrowed, not copied. The pane reads the action list, each action's current
    // chord and whether it is remapped, and it asks Keymap::check() about a captured chord BEFORE
    // offering to apply it -- so the conflict rules exist in exactly one place and the list can
    // never show a binding the app is not actually dispatching. Null = the pane is not built.
    const Keymap* keymap = nullptr;
    // Apply + persist one remap. `chordText` is ui::chordToText()'s form ("" unbinds the action).
    // `steal` is the user's answer to the reassign question: without it a chord already held by
    // another action is refused, and the host returns false so the pane keeps the old binding.
    // Returns false for anything Keymap::check() refuses, too -- the pane has already asked, so a
    // false here means the model and the pane disagreed and the model wins.
    std::function<bool(const std::string& actionId, const std::string& chordText, bool steal)>
        setKeyChord;
    std::function<void(const std::string& actionId)> resetKeyChord; // back to the harvested default
    std::function<void()> resetAllKeyChords;

    // --- Rendering (Settings → Rendering; S60-b item 14, docs/s60-performance-plan.md §6) ----
    // Persist Settings::renderingMode ("auto" | "cpu-only"). NOT live, and the pane says so: the
    // GPU lanes are built on first use and then cached for the process, so flipping this mid-
    // session would leave whichever lanes already exist exactly where they are. It is a
    // start-up policy, and pretending otherwise would be the kind of half-applied setting that
    // makes a user distrust the whole dialog.
    std::function<void(const std::string& mode)> setRenderingMode;

    // --- General ----------------------------------------------------------------------------
    // Persist Settings::showAllExportFormats (docs/export-system-plan.md §0/§3): whether the
    // Export dialog's format list also offers the EXOTIC tier. Read when that modal opens, so
    // there is nothing live to re-apply here.
    std::function<void(bool on)> setShowAllExportFormats;
};

// The Tools -> Brush display-mode cards' preview art (mode 0 = Grid, 1 = Cards; docs/brushes.md §8.2).
//
// ⚠ EXPOSED SO IT CAN BE LOOKED AT. A SettingsDialog is an Fl_Window, and an unshown Fl_Window renders
// BLACK to an Fl_Image_Surface -- unlike a Panel or any other plain widget, which shoots fine. So the
// dialog itself cannot be judged headlessly, and its art was shipped once with a staircased stroke
// that nobody could see until a human ran the app. This one draw call CAN be shot, and is.
void drawPresetDisplayPreview(int x, int y, int w, int h, int mode, common::Color8 bg);

class SettingsDialog : public Fl_Double_Window {
public:
    explicit SettingsDialog(SettingsHost host);
    ~SettingsDialog() override; // removes the line-card animation timeout

    // Fl_Window show/hide, plus arming/disarming the Appearance line-style cards' animation
    // timeout -- the cards only burn cycles while the dialog is actually on screen.
    void show() override;
    void hide() override;

    // Seed every control from `s`; call before show() so a re-open reflects the live values.
    // Programmatic value() calls do not fire the per-control callbacks, so seeding never writes.
    void seed(const common::Settings& s);

    // Runtime theme change (the dialog is open while the user picks in Appearance): re-apply the
    // cached colours on the dialog chrome. Driven by MainWindow's theme observer.
    void reapplyTheme();

    // Select the Tablet section by the SAME index its readout timer keys off (kTabletSection in the
    // .cpp). Exists so a test can prove the positional rail and the pane order have not drifted
    // apart without restating the index -- restating it would pin nothing.
    void selectTabletSectionForTest();

    // Freeze / unfreeze the Inpainting pane while an inpaint run is in flight (you can't swap the
    // backend or its params mid-solve). Called by MainWindow alongside the rest of the run gating.
    void setInpaintEngineBusy(bool busy);

protected:
    int handle(int event) override; // dismiss an open Dropdown list on outside-click / Esc

private:
    void selectSection(int index);  // show one content group, highlight its nav row
    // Tablet pane: the test area's live readout, polled on a timer that only runs while the pane is
    // the visible one (a settings dialog has no business burning a wakeup a second on a hidden page).
    static void tabletTick(void* v);
    void armTabletTick();      // (re)arm or drop the timer to match what is on screen
    void refreshTabletTest();  // one poll -> the readout labels
    void selectToolTab(int index);  // Tools: show one per-tool sub-pane, highlight its sub-tab
    void selectAppearanceTab(int index); // Appearance: General | Icons sub-panes (S52)
    // Select pack card `index` (host order): highlight it, refresh the detail panel, and -- when
    // `fromUser` -- apply + persist through the host. seed() calls it with fromUser=false.
    void selectIconPack(int index, bool fromUser);
    // Keybindings pane (S51-b, docs/keybindings.md). The rows are built ONCE, grouped by category;
    // the search box only re-stacks and hides them (a rebuild per keystroke would delete widgets
    // from inside the search field's own callback, and Fl_Scroll excludes hidden children from its
    // content bounds for free).
    void buildKeybindingsPane();
    void relayoutKeyRows(); // apply the search filter, then re-stack whatever survived it
    void refreshKeyRows();  // re-read every row's chord + remapped state from the live keymap
    void beginKeyCapture(int row); // put one row into "Press a key…"
    void endKeyCapture();          // leave capture, changing nothing (Escape, or an outside click)
    // The keystroke that arrived while a row was capturing: build its chord, ask the keymap, and
    // either apply it, offer to reassign, or explain the refusal. Always ends the capture.
    void captureFromEvent();
    void resetKeyRow(int row);
    void onUnitsChanged();
    void onTabletRange(); // Tablet: the raw-pressure clamp (both sliders share one callback)
    void onTabletSpeed(); // Tablet: the speed-sensor calibration (max speed + EMA window)
    void onTextLanguageChanged();
    void onEmojiFontChanged();
    void browseCmyk();
    void clearCmyk();
    void updateCmykDisplay();

    // Inpainting pane (Settings → Inpainting). The Engine tab picks a backend + shows its spec; the
    // Backend Settings tab is rebuilt from the chosen backend's schema (presets + controls).
    void selectInpaintTab(int index);
    void selectInpaintBackend(int comboIndex, bool fromSeed); // set backend, rebuild spec + controls
    void rebuildInpaintBackendSettings();                     // (re)create the per-backend controls
    void addInpaintControl(const core::inpaint::ParamControl& c, int& y); // one control row
    void positionInpaintControls(); // set every control to the active preset/custom values + the chip
    void applyInpaintPreset(const std::string& presetId, bool fromUser); // select a preset + apply
    void onInpaintPresetChosen(int presetIndex);
    void onInpaintControlChanged(Fl_Widget* control);   // a control edited -> live override + "Custom"
    void resetInpaintParams();
    void setInpaintControlValue(int index, double value);
    [[nodiscard]] double readInpaintControlValue(int index) const;
    void updateInpaintReadout(int index, double value);
    [[nodiscard]] const SettingsHost::InpaintBackendDesc* currentInpaintBackend() const;

    SettingsHost m_host;

    std::vector<Fl_Widget*> m_nav;     // left rail rows (NavItem), one per section
    std::vector<Fl_Group*> m_panes;    // right content groups, parallel to m_nav
    std::vector<Fl_Widget*> m_themeCards; // Appearance: the theme-mode cards (OptionCard)
    std::vector<Fl_Widget*> m_lineStyleCards; // Appearance: overlay-line style cards (OptionCard)
    std::vector<Fl_Widget*> m_featherCards; // Appearance: feathered-selection indicator cards (A / F)
    std::vector<Fl_Widget*> m_cropCards;  // Tools/Crop: crop initial-framing cards (OptionCard, S16-q)
    std::vector<Fl_Widget*> m_multiSelectCards;  // Tools/Move: multi-selection-edit cards (S15-e)
    std::vector<Fl_Widget*> m_brushDisplayCards; // Tools/Brush: preset-dock Grid|Cards (§8.2)
    std::vector<Fl_Group*> m_toolPanes;   // Tools: per-tool sub-panes (parallel to the sub-tabs)
    std::vector<Fl_Group*> m_appearancePanes; // Appearance: General | Icons sub-panes (S52)
    std::vector<Fl_Widget*> m_iconPackCards;  // Icons: one card per pack (host order, OptionCard)
    int m_section = 0;
    int m_toolTab = 0;                  // active Tools sub-tab
    int m_appearanceTab = 0;            // active Appearance sub-tab (0 = General, 1 = Icons)

    // Appearance line-style cards: the shared animation state. The timeout is armed by show(),
    // removed by hide(); the tick advances the background drift and repaints the three cards
    // only while the Appearance pane is the visible one.
    static void lineCardTick(void* v);
    double m_linePhase = 0.0;           // background drift, px

    class Panel* m_rail = nullptr;      // left rail (re-themed on a runtime theme change)
    class Panel* m_footer = nullptr;    // bottom strip (windowBg, not the panel default)
    Fl_Widget* m_toolTabs = nullptr;    // Tools: the per-tool sub-tab strip (SubTabBar)
    Fl_Widget* m_appearanceTabs = nullptr; // Appearance: General|Icons sub-tab strip (SubTabBar)
    Fl_Widget* m_iconPackDetail = nullptr; // Icons: the selected pack's preview + credits panel
    // Icons: rasterized pack preview strips, built lazily per pack (definition in the .cpp).
    struct IconPreviewCache;
    std::unique_ptr<IconPreviewCache> m_iconPreviews;
    class Dropdown* m_units = nullptr;  // General: measurement system
    Fl_Widget* m_cropSwitchTool = nullptr; // Tools: "switch tool after crop" checkbox (CheckBox)
    Fl_Widget* m_cropClearOnLeave = nullptr; // Tools: "clear crop selection on leave" checkbox
    Fl_Widget* m_lassoSmooth = nullptr;      // Tools/Lasso: "smooth freehand lasso" checkbox
    Fl_Widget* m_eraserSizeTie = nullptr;    // Tools/Eraser: "size follows the brush" checkbox
    Fl_Widget* m_eraserPresetTie = nullptr;  // Tools/Eraser: "preset follows the brush" checkbox
    Fl_Widget* m_motivationalLines = nullptr; // Annoyances: "cheesy motivational one-liners" checkbox
    Fl_Widget* m_showUnsavedDuration = nullptr;  // Annoyances: "show how long unsaved" checkbox (S18-d)
    Fl_Widget* m_unsavedIncludeSeconds = nullptr; // Annoyances: "...include seconds" checkbox (S18-d)
    Fl_Widget* m_showAllExportFormats = nullptr; // General: "show all export formats" (M5/M7)
    Fl_Widget* m_renderingCpuOnly = nullptr;  // Rendering: "use the CPU only" checkbox (S60-b)
    Fl_Widget* m_spellCheck = nullptr;        // Text: "check spelling" checkbox
    Fl_Widget* m_spellCheckAllCaps = nullptr; // Text: "check ALL-CAPS words" checkbox
    class Dropdown* m_textLanguage = nullptr; // Text: default text language
    class Dropdown* m_emojiFont = nullptr;    // Text: preferred emoji fallback family (R5)
    Fl_Output* m_cmykField = nullptr;   // Color Management: chosen .icc basename (or the built-in)
    std::string m_cmyk;                 // current CMYK profile path ("" = built-in default)

    // Keybindings pane -----------------------------------------------------------------------
    class KeyRow;                          // one action's row: label + chord, click to capture
    class ScrollView* m_keyScroll = nullptr;
    Fl_Input* m_keySearch = nullptr;       // filters the list by action label, category or chord
    std::vector<Fl_Widget*> m_keyRows;     // KeyRow, in display order (grouped by category)
    std::vector<std::string> m_keyRowIds;  // the action id per row (parallel to m_keyRows)
    std::vector<Fl_Widget*> m_keyRowResets; // per-row Reset (parallel; shown only when remapped)
    std::vector<Fl_Widget*> m_keyHeaders;  // one category heading, indexed by ActionCategory ordinal
    Fl_Widget* m_keyBottomPad = nullptr;   // trailing spacer; re-placed so filtering shrinks the scroll
    int m_keyCapture = -1;                 // the row in "Press a key…" state (-1 = none)

    // Tablet pane (docs/tablet.md §8).
    class CurveEditor* m_tabletCurve = nullptr;  // pressure response (the §8.3 widget)
    Fl_Widget* m_tabletPressureMin = nullptr;    // raw-pressure clamp (ScrubSlider)
    Fl_Widget* m_tabletPressureMax = nullptr;
    Fl_Widget* m_tabletTiltOffset = nullptr;     // signed angle (Dial)
    Fl_Widget* m_tabletSpeedMax = nullptr;       // speed smoothing (ScrubSlider)
    Fl_Widget* m_tabletSpeedWindow = nullptr;
    Fl_Widget* m_tabletTestArea = nullptr;       // the live readout panel (TabletTestArea)
    bool m_tabletTicking = false;                // is the readout timer armed?

    // Inpainting pane ----------------------------------------------------------------------------
    Fl_Group* m_inpaintPane = nullptr;       // the whole pane (deactivated while a run is in flight)
    Fl_Widget* m_inpaintTabs = nullptr;      // Engine | Backend settings sub-tab strip (SubTabBar)
    std::vector<Fl_Group*> m_inpaintTabPanes; // [engine, backend-settings], parallel to the tabs
    int m_inpaintTab = 0;
    class Dropdown* m_inpaintBackendChoice = nullptr; // Engine: the backend selector
    Fl_Widget* m_inpaintSpec = nullptr;               // Engine: the spec-sheet panel (SpecPanel)
    class ScrollView* m_inpaintSettingsScroll = nullptr; // Backend settings: dynamic-content scroll
    class Dropdown* m_inpaintPresetChoice = nullptr;     // Backend settings: quality preset (or null)
    int m_inpaintDescIdx = -1;            // active descriptor index (== combobox index; 1:1)
    std::string m_inpaintPresetId;        // currently-shown preset id ("" = none, "custom" = hand-tuned)
    std::map<std::string, double> m_inpaintOverrides; // custom control values (when preset == "custom")
    std::vector<Fl_Widget*> m_inpaintDynamic; // every rebuilt widget, to remove on a backend switch
    std::vector<core::inpaint::ParamControl> m_inpaintCtrlSpecs; // one per control (parallel)
    std::vector<Fl_Widget*> m_inpaintCtrlWidgets;   // the primary value widget per control
    std::vector<Fl_Widget*> m_inpaintCtrlReadouts;  // Int/Real numeric readout box per control (or null)
};

} // namespace mosaic::ui
