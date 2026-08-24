#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Persistent user settings, stored as JSON under the per-user config dir. Deliberately a plain
// struct of serializable, UI-free fields so the lowest module (common) can own it; ui/ maps
// `theme` onto ui::ThemeMode (parseThemeMode), app/ maps `logLevel` onto common::log::Level.
// The JSON dependency stays in settings.cpp so including this header is cheap.
namespace mosaic::common {

struct Settings {
    // On-disk schema version; bump when a field changes incompatibly (drives migrations later).
    static constexpr int kSchemaVersion = 1;

    // Dark is the DEFAULT, not just an option (2026-08-24, user call): it is the identity the
    // application is designed around, and an image editor's chrome competing with the image is
    // the thing a neutral default gets wrong. "system" remains fully supported and is one click
    // away in Settings, where it also picks up the OS accent colour and live theme changes.
    std::string theme = "dark";     // "system" | "dark" | "light"
    std::string logLevel = "info";  // trace|debug|info|warn|error|critical|off
    std::string language;           // empty => follow the system locale; else e.g. "de", "ja"
    std::string units = "auto";     // measurement system: "auto" (follow the locale) | "metric" |
                                    // "imperial". Drives px<->cm/in conversions (the crop readout,
                                    // rulers). "auto"/"" resolve through resolveUnits() below.
    std::string pickerSurface = "field";  // colour-picker surface: "field"|"hsl-wheel"|"sv-wheel"
                                          // (ui maps it onto ColorPicker::Surface; S12-a part 2)
    std::vector<std::string> recentColors; // picker recents, "#RRGGBB", newest first (S12-b)
    std::vector<std::string> recentFiles;  // File -> Open Recent: absolute paths, newest first,
                                           // capped at kMaxRecentFiles (recent_files.hpp; S55)
    std::vector<std::string> recentSizes;  // File -> New "Sizes" cards: hand-entered custom
                                           // sizes as "W;H;unit;dpi" tokens, newest first,
                                           // capped at kMaxRecentSizes (ui parses/formats)
    std::string workingProfile; // path to an RGB .icc overriding the working space ("" = built-ins)
    std::string cmykProfile;    // path to a CMYK .icc overriding the vendored default (S12-c)

    // After applying a crop, switch away from the Crop tool to the previously-active tool (falling
    // back to Move). Default false = stay on Crop, the industry behaviour (PS/GIMP/Affinity). S16-p.
    bool cropSwitchToolAfterApply = false;

    // What the Crop tool stages when first picked. "whole-canvas" (default, the industry behaviour)
    // frames the entire canvas; "draw" stages nothing (GIMP-style) so the first drag draws the rect.
    // "inset" is reserved for a future margin-based option. ui maps it onto VulkanCanvas::CropFraming
    // via parseCropFraming(). S16-q.
    std::string cropInitialFraming = "whole-canvas";

    // Clear the staged crop selection when you switch away from the Crop tool. Default false keeps the
    // staged rect across tool switches (S16-c) so leaving and returning restores the framing; true
    // drops it so re-entering the Crop tool stages a fresh rect. Tools/Crop setting.
    bool cropClearSelectionOnLeave = false;

    // What a blend-mode / opacity edit does while several layers are selected (S15-e): "disabled"
    // (default; edit one layer at a time -- the strip is inert on a multi-selection), "all" (apply to
    // every selected layer in one undo step, the Affinity/Figma behaviour), "active" (apply to the
    // active layer only, the Photoshop behaviour). The selection is the Move tool's (S15-c);
    // persistent panel-owned selection is a follow-up. ui maps it onto LayerPanel::MultiSelectMode.
    std::string multiSelectionEdits = "disabled";

    // Round the freehand lasso's hand-drawn path with Catmull-Rom smoothing, for both the in-flight
    // preview and the committed selection. Default true: the raw hand path is jagged, the smoothed one
    // tracks the intent far better. Affects only the freehand lasso (the polygonal lasso's straight
    // segments are intentional). Tools/Lasso setting; ui maps it onto VulkanCanvas::setLassoSmoothing.
    bool lassoSmooth = true;

    // The Select brush's default combine op (S18, docs/research-select-brush.md §9-B). A no-modifier
    // stroke uses this op (Add when true), and holding Alt always subtracts -- the press-time-modifier
    // convention the marquee/wand use. True (Add) reads naturally for a brush that builds a selection.
    // A serializable key with no Settings-dialog control, like antsCirculate; ui maps it onto
    // VulkanCanvas::setSelectBrushAddByDefault. The "no-toggle-for-strictly-better" rule is
    // deliberately overridden here -- the user asked for the preference (2026-07-15).
    bool selectBrushAddByDefault = true;

    // The eraser ties (S19 §8.4, Tools/Eraser). Both are genuine user preferences -- Photoshop ties
    // size, Krita does not -- so neither trips the no-toggle-for-strictly-better rule.
    // eraserSizeFollowsBrush: the Eraser's Size and the Brush's are ONE shared value, edited from
    // either side (ui maps it onto ToolManager::setEraserSizeTie). eraserPresetFollowsBrush is
    // schema-ready for the preset system (Arc B/D): until presets exist there is nothing to tie
    // and no UI shows it.
    bool eraserSizeFollowsBrush = true;
    bool eraserPresetFollowsBrush = false;

    // Settings → Appearance "Selection & reticle line" (2026-07-07 design rounds): how the shared
    // content-keyed overlay line -- the in-flight lasso, the brush reticle, and the Type
    // frames/baseline -- picks its colour over the image. "classic" (default) = the original hard
    // per-pixel luminance key (pure white on dark content, pure black on light); "rim" (the
    // DEFAULT) = a white plateau-core line with a snug dark rim that fades in over light content
    // (design 'R'); "adaptive" = the core tone ramps white -> graphite -> black with the (blurred)
    // content and a faint rim shadow bridges the low-contrast crossover (design 'P3d'). The Type
    // caret is box-blue chrome (a tight always-on shadow, like the handles) and ignores this
    // setting. ui maps it onto VulkanCanvas::setOverlayLineStyle.
    std::string overlayLineStyle = "rim";

    // Settings → Appearance "Feathered selection indicator": how a soft-edged (feathered) selection
    // is drawn on the canvas, so the artist can read the feather's width and location that a single
    // 50% marching ant cannot convey. "bracket" (the DEFAULT) = a Bracketing ant pair, ants at the
    // ~15% and ~85% coverage contours whose gap is the feather band; "band" = True-edge ant + soft
    // band, the familiar crisp 50% ant plus a faint falloff tint. Both collapse to today's single
    // ant on a hard edge. ui maps it onto VulkanCanvas::setFeatherIndicator.
    std::string featherIndicator = "bracket";

    // Marching-ants direction experiment (S18, docs/research-select-brush.md §5). Off = the default
    // diagonal crawl (calm, uniform); on = the ants dash along the local boundary tangent, so they
    // circulate around the contour like Photoshop's. A hidden key with no Settings-dialog control (it
    // shimmers on feathered / lasso edges -- see the doc), the sole A/B switch for the experiment; ui
    // maps it onto VulkanCanvas::setAntsCirculate. Default false keeps the render byte-unchanged.
    bool antsCirculate = false;

    // Settings → Appearance → Icons: the selected tool icon pack's id ("default" = the embedded
    // Smalti set; otherwise a folder name under dataDir()/icon_packs/). An id that no longer
    // resolves falls back to the default pack at apply time -- the setting keeps the user's choice
    // so a temporarily-missing pack folder comes back on its own (S52).
    std::string iconPack = "default";

    // Settings → Annoyances: "Cheesy motivational one-liners". When on, an all-caps one-liner crawls
    // across the menu bar's empty right region every few minutes (ui::MotivationTicker,
    // docs/motivational-ticker.md). Off by default -- it lives in Annoyances for a reason. The
    // Settings toggle re-applies it live.
    bool motivationalLines = false;

    // Settings → Annoyances (S18-d, unsaved-state window title). showUnsavedDuration adds the
    // "unsaved for N min" tally to the title once the document has been unsaved past a threshold (on
    // by default); unsavedIncludeSeconds ticks that tally every second rather than every minute (off
    // by default -- a per-second title re-render is motion in the eye-line, and chatty for some WMs
    // and screen readers). The bare "• unsaved" mark is unconditional.
    bool showUnsavedDuration = true;
    bool unsavedIncludeSeconds = false;

    // Settings → Text (Type tool language features, deferred §2). Live spell-checking of the text you
    // are editing: a red wavy underline under misspellings (an overlay, never baked/exported), only
    // while a text-edit session is active. On by default; the ui gates the background SpellCheckWorker
    // on it and re-applies live.
    bool spellCheck = true;
    // Also spell-check ALL-CAPS words (decision D4). Off by default: acronyms (NASA, HTTP) are usually
    // not in a dictionary and flagging them is noise; on catches capitalised typos.
    bool spellCheckAllCaps = false;
    // The app-level default text language (BCP-47, e.g. "en-US", "de") behind an empty per-paragraph
    // Paragraph::language -- the top of the resolveLanguage chain for spell-check AND hyphenation.
    // Empty => follow the OS locale (detectSystemLanguage). ui feeds it to TextShaper::setDefaultLanguage.
    std::string textLanguage;

    // The colour-emoji family preferred when text needs an emoji fallback face (Settings → Text →
    // Emoji font, Type R5). Empty => automatic (the OS font cascade picks). ui maps it onto
    // platform::FontDB::setPreferredEmojiFamily; a codepoint the family lacks still falls through.
    std::string emojiFont;

    // Width in px of the right-hand dock (Layers | History), dragged by the splitter on its left
    // edge. Window LAYOUT state, not a user-facing preference -- it has no Settings-dialog control
    // and belongs to no category; it persists so the dock reopens where the user left it. Clamped
    // to the dock's min/max on load, so a hand-edited or stale value can never strand the canvas.
    int dockWidth = 280;

    // Height in px of the dock's Brush-preset section (docs/brushes.md §8.2), below the
    // Layers|History panel and dragged by the horizontal splitter between them. Window LAYOUT
    // state, exactly like dockWidth: no Settings-dialog control, no category -- it persists so the
    // section reopens at the size the user left it. Clamped against the live dock height on load
    // (ui::presetSplit), so a stale value can never squeeze the layer list out of existence.
    int brushPresetHeight = 260;

    // The selected brush preset, by NAME (docs/brushes.md §8.2); empty = no preset, i.e. the
    // engine's own analytic circle, which is what the Brush painted before presets existed and what
    // it still starts on. Stored by name and NOT by index on purpose: the library's order is a
    // directory scan, so an index would silently point at a different brush the moment a bundle is
    // added, removed or renamed. A name that no longer resolves simply selects nothing.
    std::string brushPreset;
    // The ERASER's own preset (§8.4). A SEPARATE slot: the dock offers the Brush every preset that is
    // not an eraser and the Eraser nothing but, so reaching for one tool must not silently re-point
    // the other. Same by-name rule, same "" = the round tip.
    std::string eraserPreset;

    // How the preset dock draws the library (Settings → Tools → Brush): "cards" or "grid".
    //
    // "cards" is the DEFAULT and the mode the user asked for: each row carries the brush's tip icon
    // AND a strip showing a stroke of that very brush, laid by the real engine. A grid of tip icons
    // answers "what does this brush's picture look like", which is nobody's question; a stroke
    // answers "what mark does it make", which is everybody's.
    //
    // A STRING, not an enum ordinal, for the same reason brushPreset is a name: an ordinal silently
    // re-points the moment a mode is inserted. An unrecognised value -- including the "" that a
    // settings file written before this field existed reads back as -- lands on the default.
    std::string brushPresetDisplay = "cards";

    // Settings → Tablet (docs/tablet.md §7/§8) — the global input policy, applied to every raw
    // sample at ingest, BEFORE the engine's per-preset dynamics read it. The engine therefore sees a
    // device that already behaves the way the user wants, which is what keeps presets portable
    // across people and hardware.
    //
    // tabletPressureCurve: the response curve, serialized as "x,y;x,y;" — the SAME interchange
    // string the presets use (core::brush::Curve), so it shares the §8.3 editor widget. The identity
    // short-circuits the whole apply().
    std::string tabletPressureCurve = "0,0;1,1;";
    // tabletPressureMin/Max: the raw-pressure clamp, re-spanned to the full [0,1]. Worn nibs and
    // cheap digitizers never reach 0 or 1; without this they are unusable. Inverted or degenerate
    // pairs are well-defined (TabletPolicy swaps / thresholds them), so a hand-edited file cannot
    // break the ingest path.
    double tabletPressureMin = 0.0;
    double tabletPressureMax = 1.0;
    // A signed angle added to the derived `ascension` bearing, for users who hold the pen rotated.
    // Applied by rotating the (xTilt, yTilt) pair at ingest, never in the sensor.
    double tabletTiltOffsetDegrees = 0.0;
    // The `speed` sensor's EMA calibration: the speed that reads as 1.0 (document px per ms), and
    // the time constant it smooths over. Driven by elapsed time rather than sample count, so a
    // 200 Hz tablet and a 60 Hz mouse smooth over the same window.
    double tabletSpeedMax = 3.0;
    double tabletSpeedWindowMs = 30.0;
    // Brush smoothing (core/brush/stroke_smoother.hpp): ON or OFF, no dial -- there is no useful "a
    // little bit of rattle". Lives on the brush-family CONTEXT BARS, not on a settings page: it
    // steadies the POINTER, so it applies to the mouse every bit as much as to the pen.
    //
    // ON by default. It is kept as a toggle rather than being made unconditional -- which the "no
    // toggle for strictly-better behaviour" rule would otherwise demand -- because smoothing is NOT
    // strictly better: it DISCARDS the user's actual input. Pixel-precise and technical work has a
    // legitimate claim on unmanipulated data, and that is a real choice, not a quality dial.
    bool brushSmoothing = true;

    // Settings → Rendering (S60-b item 14, docs/s60-performance-plan.md §6). Whether Mosaic may use
    // the GPU for COMPUTE -- the composite, the blurs, the texture generators, extruded 3D text.
    // "auto" (the default) lets each lane ask its own capability questions and build if it fits;
    // "cpu-only" makes every one of them decline to build, so the CPU path each lane already owns
    // -- and is parity-tested against -- serves instead. The pixels are the same either way; only
    // the cost moves.
    //
    // The window is still PRESENTED through Vulkan in both modes. §6.1 is explicit that retiring
    // presentation is a different, far larger thing (the marching ants, handles, loupe, caret and
    // empty-state field are shader code with no CPU twin) and is out of S60's scope.
    //
    // This does NOT trip the "no toggle for strictly-better behaviour" rule: on a driver that
    // hangs, corrupts or crashes, the CPU lanes are the only way to get the app to run at all, and
    // no probe can distinguish "this driver is wrong" from "this driver is slow".
    //
    // A STRING rather than a bool so §6.3's eventual third value ("GPU required" -- fail loudly
    // instead of falling back) can be added without a schema type change (see the brushSmoothing
    // migration below for what a type change costs). Anything unrecognised -- including the "" a
    // file written before this field existed reads back as -- means "auto". `--cpu` on the command
    // line overrides this for one run; render::decideGpuPolicy owns that precedence.
    std::string renderingMode = "auto";  // "auto" | "cpu-only"

    // Settings -> General: "Show all export formats" (docs/export-system-plan.md §0/§3). Off by
    // default. It gates the EXOTIC tier only -- PCX, XPM, Sun Raster, FITS and the rest, which
    // arrive at M7; the curated pro set (BMP, TGA, PNM/PAM, QOI, ICO, Radiance HDR) is always on
    // and this flag cannot hide it. The wording is settled: "legacy" and "uncommon" were both
    // rejected because they imply the formats are bad, and they are not -- they are simply not
    // what most people export. Feeds FormatRegistry::exportOrder(includeExotic).
    bool showAllExportFormats = false;

    // Settings → Keybindings (S51-b, docs/keybindings.md): the user's remapped shortcuts, as
    // action id -> canonical chord text ("file.open" -> "Ctrl+O"). A value of "" means the action
    // was deliberately UNBOUND, which is a different thing from being absent.
    //
    // SPARSE on purpose: only what the user actually changed is stored. A full snapshot would freeze
    // the shipped table into their settings file, so improving a default later would reach nobody
    // who had ever opened the Keybindings page. The chord text is ui::chordToText()'s form -- ASCII,
    // never localized, exact round trip -- and ui maps it back through ui::Keymap::setOverrides(),
    // which drops any entry naming an action this build does not have or a chord it cannot parse.
    std::map<std::string, std::string> keymap;

    // Active inpainting backend id (Settings → Inpainting). "offset-stats" (He & Sun, the default,
    // higher quality) | "pde" (fast diffusion). ui maps it onto InpaintEngine::setActiveBackend.
    std::string inpaintBackend = "offset-stats";

    // Quality/speed preset id for the active backend's tunables ("fast"|"balanced"|"best"; backends
    // without presets ignore it), or "custom" when the user has hand-tuned the controls. The chosen
    // preset feeds core::inpaint::Params for a run.
    std::string inpaintPreset = "balanced";

    // The active backend's hand-tuned control overrides (control key -> value), populated only when
    // inpaintPreset == "custom"; selecting a preset (or switching backend) clears it. Applied on top
    // of the backend's defaults to rebuild core::inpaint::Params. ui maps the keys onto Params via
    // each backend's applyParam().
    std::map<std::string, double> inpaintParams;

    bool operator==(const Settings&) const = default;
};

// Per-user config directory: $XDG_CONFIG_HOME/mosaic, else $HOME/.config/mosaic (Unix).
// Empty path if neither variable is set.
[[nodiscard]] std::filesystem::path configDir();

// Per-user data directory: $XDG_DATA_HOME/mosaic, else $HOME/.local/share/mosaic (Unix); empty
// when neither variable is set. App-owned runtime data the user accumulates -- brush presets
// land in dataDir()/"brushes" (docs/brushes.md §7). Nothing here is a document sidecar: user
// FILES are only ever written by an explicit Save/Export (the standing hard rule).
[[nodiscard]] std::filesystem::path dataDir();

// Per-user state directory: $XDG_STATE_HOME/mosaic, else $HOME/.local/state/mosaic (Unix);
// LOCALAPPDATA/mosaic on Windows; empty when nothing resolves. Derived app state that is neither
// config nor user data -- e.g. the recent-file preview thumbnails (S55). The recovery journals
// live under the same root but compute it independently (io/mosaic/journal.cpp) with a deliberate
// "." fallback: recovery must always land somewhere, while callers here may simply skip caching.
[[nodiscard]] std::filesystem::path stateDir();

// The read-only data directory the app ships with (the default brush set, docs/brushes.md §4).
// Resolution order, following i18n's localedir: the $MOSAIC_DATA_DIR environment override
// (taken verbatim), the install prefix baked in as MOSAIC_DATA_DIR (when it exists on disk),
// then the source tree's data/ -- the dev fallback, so a build-tree binary finds the shipped
// set without an install step. Empty when none of the three resolves.
[[nodiscard]] std::filesystem::path installedDataDir();

// configDir() / "settings.json" (empty if configDir() is empty).
[[nodiscard]] std::filesystem::path defaultSettingsPath();

// Load settings from `path`. A missing file is not an error: defaults are returned and
// `*existed` (if given) is set to false. A malformed/incompatible file also returns defaults
// but reports via `*error`. Unknown keys are ignored.
[[nodiscard]] Settings loadSettings(const std::filesystem::path& path, std::string* error = nullptr,
                                    bool* existed = nullptr);

// Write `settings` to `path` atomically (temp file + rename), creating parent directories.
// Returns false and sets `*error` on failure.
bool saveSettings(const Settings& settings, const std::filesystem::path& path,
                  std::string* error = nullptr);

// The host's measurement system, "metric" or "imperial", read from the locale's measurement
// category — NOT the UI language (a German user may run a US locale, or vice versa). Falls back to
// "metric" (the global majority) when the platform cannot report it.
[[nodiscard]] std::string detectMeasurementSystem();

// Resolve a Settings::units preference to a concrete "metric"/"imperial": "auto" (or empty) means
// follow the locale via detectMeasurementSystem(); any other value passes through.
[[nodiscard]] std::string resolveUnits(const std::string& pref);

}  // namespace mosaic::common
