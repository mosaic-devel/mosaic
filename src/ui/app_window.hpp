#pragma once

#include "ui/theme.hpp"  // ThemeMode

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mosaic::ui {

struct RunOptions {
    // Documents to open once the window is up: the positional CLI arguments, which is how a desktop
    // hands a program its files (dropped on the icon, "Open with..." on a multi-selection, an
    // xdg-mime association, a shell glob). Each is dispatched by CONTENT, so a renamed .mosaic still
    // opens as a document. Several open as several tabs (S49), the last one active; empty => start
    // on the empty state.
    std::vector<std::string> openPaths;

    // >0: render this many frames then exit. A GUI smoke test that needs a display but no
    // human (used to validate the swapchain path); 0 runs normally until the window closes.
    int autoQuitFrames = 0;

    // Theme to apply at start-up. main() resolves this from the persisted settings; the in-app
    // theme picker (S51) will re-apply at runtime.
    ThemeMode themeMode = ThemeMode::Dark;

    // The persisted colour-picker surface key (Settings::pickerSurface) and where to write it
    // back when the user switches surfaces (empty path => don't persist). S12-a part 2.
    std::string pickerSurface = "field";
    std::filesystem::path settingsPath;

    // Persisted picker recents ("#RRGGBB", newest first; Settings::recentColors). S12-b.
    std::vector<std::string> recentColors;

    // Persisted recent files (absolute paths, newest first; Settings::recentFiles) -- the
    // File -> Open Recent submenu and the New Document dialog's Recent cards. S55.
    std::vector<std::string> recentFiles;

    // Persisted hand-entered custom sizes (tokens, newest first; Settings::recentSizes) -- the
    // New Document dialog's "Sizes" cards (round 5).
    std::vector<std::string> recentSizes;

    // Persisted width of the right-hand dock (Settings::dockWidth, S16-g). Clamped to the dock's
    // min/max at layout time, so a stale or hand-edited value cannot strand the canvas.
    int dockWidth = 280;

    // Persisted height of the dock's Brush-preset section (Settings::brushPresetHeight, S19 §8.2),
    // and the preset selected there, BY NAME (Settings::brushPreset; empty = no preset, i.e. the
    // engine's plain round tip). Both are clamped/resolved at startup: a height the dock cannot
    // afford is squeezed by ui::presetSplit, and a name no longer in the library selects nothing.
    int brushPresetHeight = 260;
    std::string brushPreset;
    // The Eraser's OWN preset (§8.4), by name. A separate slot: the dock shows the Brush every preset
    // that is not an eraser and the Eraser nothing but, so one tool must not re-point the other.
    std::string eraserPreset;
    // How the preset dock lists the library (Settings::brushPresetDisplay, §8.2): "cards" (default)
    // or "grid". Resolved by NAME, so an unrecognised value lands on the default rather than on
    // whatever an enum's zero happens to be.
    std::string brushPresetDisplay = "cards";

    // User ICC profile paths (S12-c; empty = built-in working space / vendored CMYK default).
    std::string workingProfile;
    std::string cmykProfile;

    // Resolved measurement system ("metric" / "imperial"); main() runs Settings::units through
    // common::resolveUnits() (so "auto" is already collapsed to the locale's system). Drives the
    // crop size HUD's unit readout (S16-e); the S51 settings UI will re-apply at runtime.
    std::string units = "imperial";

    // Switch away from the Crop tool to the previous tool after applying a crop (S16-p; Settings
    // re-applies it live). Default false = stay on Crop (industry behaviour).
    bool cropSwitchToolAfterApply = false;

    // Crop tool initial framing (S16-q): "whole-canvas" (default) | "draw" (GIMP-style, stage
    // nothing until the first drag). Mapped onto VulkanCanvas::CropFraming; Settings re-applies live.
    std::string cropInitialFraming = "whole-canvas";

    // Clear the staged crop selection when leaving the Crop tool (default false = keep it across tool
    // switches). Settings re-applies it live.
    bool cropClearSelectionOnLeave = false;

    // Multi-selection edits (S15-e): "disabled" (default) | "all" | "active" -- what a blend/opacity
    // edit does while several layers are selected. Mapped onto LayerPanel::MultiSelectMode; Settings
    // re-applies it live.
    std::string multiSelectionEdits = "disabled";

    // Round the freehand lasso path (Catmull-Rom) for both preview + commit. Default true (the raw
    // hand path is jagged). Mapped onto VulkanCanvas::setLassoSmoothing; Settings re-applies it live.
    bool lassoSmooth = true;

    // The Select brush's default combine op (S18, §9-B): a no-modifier stroke Adds when true, Alt
    // always subtracts. Mapped onto VulkanCanvas::setSelectBrushAddByDefault.
    bool selectBrushAddByDefault = true;

    // Tools/Eraser (S19 §8.4): the Brush's and Eraser's Size options are one shared value, edited
    // from either side. Mapped onto ToolManager::setEraserSizeTie; Settings re-applies it live.
    bool eraserSizeFollowsBrush = true;

    // Settings → Appearance "Selection & reticle line": how the content-keyed overlay line (lasso,
    // brush reticle, Type frames) colours itself -- "classic" | "rim" (default) | "adaptive".
    // Mapped onto VulkanCanvas::setOverlayLineStyle; Settings re-applies it live.
    std::string overlayLineStyle = "rim";

    // Settings → Appearance "Feathered selection indicator": how a soft-edged selection is drawn --
    // "bracket" (default: ants at the ~15%/~85% coverage contours) | "band" (the 50% ant + a faint
    // falloff tint). Mapped onto VulkanCanvas::setFeatherIndicator; Settings re-applies it live.
    std::string featherIndicator = "bracket";

    // Marching-ants direction experiment (S18, §5): the hidden `antsCirculate` key. Off = the default
    // diagonal crawl. Mapped onto VulkanCanvas::setAntsCirculate.
    bool antsCirculate = false;

    // Settings → Appearance → Icons (S52): the selected tool icon pack's id (Settings::iconPack;
    // "default" = the embedded set). Applied to the ToolManager before the toolbar rasterizes;
    // the Settings browser re-applies it live.
    std::string iconPack = "default";

    // Settings → Annoyances: "Cheesy motivational one-liners" (Settings::motivationalLines). Off by
    // default; drives the menu-bar ui::MotivationTicker; the Settings toggle re-applies live.
    bool motivationalLines = false;

    // Settings → Annoyances (S18-d): the unsaved-state window title. showUnsavedDuration adds the
    // "for N min" tail once the document has been unsaved past the threshold (on by default);
    // unsavedIncludeSeconds ticks that at second granularity (off by default). Live.
    bool showUnsavedDuration = true;
    bool unsavedIncludeSeconds = false;

    // Settings → Text (Type-tool language features, deferred §2). spellCheck gates the live squiggle
    // overlay; spellCheckAllCaps also flags all-caps words (D4); textLanguage is the app default
    // language ("" = OS locale) for spell-check + hyphenation (feeds TextShaper::setDefaultLanguage);
    // emojiFont is the preferred colour-emoji fallback family ("" = automatic, Type R5).
    bool spellCheck = true;
    bool spellCheckAllCaps = false;
    std::string textLanguage;
    std::string emojiFont;

    // Inpainting engine selection (Settings → Inpainting). `inpaintBackend` is the active backend id
    // ("offset-stats" default | "pde"); `inpaintPreset` is its quality/speed preset ("custom" when
    // hand-tuned); `inpaintParams` are the custom control overrides (only when preset == "custom").
    // Mapped onto core::inpaint::InpaintEngine + Params; Settings re-applies live.
    std::string inpaintBackend = "offset-stats";
    std::string inpaintPreset = "balanced";
    std::map<std::string, double> inpaintParams;

    // Settings → Tablet (docs/tablet.md §7): the global input policy, applied to every raw sample at
    // ingest. Mapped onto core::brush::TabletPolicy (the pressure curve / range / tilt offset) and
    // core::brush::SpeedParams (the `speed` sensor's EMA), both of which live on the canvas because
    // the canvas owns the tablet. Settings re-applies all of it live.
    std::string tabletPressureCurve = "0,0;1,1;";
    double tabletPressureMin = 0.0;
    double tabletPressureMax = 1.0;
    double tabletTiltOffsetDegrees = 0.0;
    double tabletSpeedMax = 3.0;
    double tabletSpeedWindowMs = 30.0;
    // Brush smoothing (core/brush/stroke_smoother.hpp): on/off, no dial. Applies to the MOUSE as much
    // as to the pen -- it steadies the pointer, not the tip.
    bool brushSmoothing = true;

    // The adapter the user asked for: `--device <index|name-substring>` / MOSAIC_DEVICE, empty when
    // unset (S60-f). main() has already applied it process-wide before the window exists -- device
    // selection has to be settled before anything creates one -- so this copy is purely so the
    // window can report what was ASKED FOR, which is not always what was picked: an unmatched
    // selector falls back to the automatic choice with a warning rather than failing to start.
    std::string gpuDevice;
};

// Build the main window (menu-bar skeleton + Vulkan canvas), show it, and run the FLTK event
// loop. Returns the process exit code. Requires a display (X11/Wayland); the GUI-less
// pipeline is the headless op-runner instead.
int runApp(const RunOptions& options = {});

} // namespace mosaic::ui
