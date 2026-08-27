#include "ui/app_window.hpp"

#include "common/fs_path.hpp"
#include "common/i18n.hpp"
#include "common/image.hpp"
#include "common/image_svg.hpp"
#include "common/log.hpp"
#include "common/profiler.hpp" // the named-op perf collector + MOSAIC_PERF_SCOPE (runtime-gated)
#include "common/recent_files.hpp" // the File -> Open Recent ring buffer (S55)
#include "common/settings.hpp"
#include "common/version.hpp"
#include "core/adjustments.hpp"    // the S32 typed adjustment-parameter schema
#include "core/arrange.hpp"        // align + distribute geometry (Arrange menu)
#include "core/arrange_target.hpp" // ... and WHICH layers / WHICH box the menu acts on
#include "core/clipboard.hpp"
#include "core/color_management.hpp"
#include "core/color_sample.hpp" // eyedropper colour sampling (S24)
#include "core/command.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/edge_grow.hpp" // the L1 edge brush's release-time geodesic grow
#include "core/fill.hpp"
#include "core/inpaint/inpaint_engine.hpp"
#include "core/layer.hpp"
#include "core/layer_marquee.hpp" // layersInMarquee: Select -> Select All Layers (S53-b)
#include "core/red_eye.hpp"       // the S38-b eye retouch (flash red-eye / sclera de-redding)
#include "core/retarget/inpaint_fill.hpp" // the engine-as-FillFn adapter (Recompose, plan §2)
#include "core/retarget/keep_regions.hpp"
#include "core/retarget/recompose.hpp"
#include "core/retarget/smart_crop.hpp"
#include "core/selection.hpp"
#include "core/text/extrude_render.hpp" // setExtrudeRenderOverride (the GPU-lane seam, S30-c)
#include "core/text/language.hpp" // detectSystemLanguage (spell-check default language, deferred §2)
#include "core/text/spell_worker.hpp" // background spell-checker (deferred §2)
#include "core/text/text_layer_render.hpp"
#include "core/text/text_render.hpp"
#include "core/texture/sky_estimate_commit.hpp" // the mask & harmonize commit shape (S55 phase 2)
#include "core/texture/texture_layer_render.hpp"
#include "core/vector/boolean.hpp" // makeBooleanObject (Layer -> Combine Paths, S53-b)
#include "core/vector/flatten.hpp" // flatten + contourLength (Type -> Text on Selected Path)
#include "core/vector/to_path.hpp" // pathFromGeometry (Layer -> Convert to Path)
#include "io/document_profile.hpp" // profileDocument: what the Export modal's loss banner reads
#include "io/export_path.hpp"      // the export-path policy (§6): never the working directory
#include "io/format_registry.hpp"  // the backend registry: one-click re-export to the last target
#include "io/io.hpp"
#include "io/mosaic/compaction.hpp"      // history-preserving fold once the parity debt trips (S48)
#include "io/mosaic/docio.hpp"           // .mosaic document bridge (S48): Save/Save As/Open
#include "io/mosaic/file.hpp"            // buildCheckpoint + writeFileAtomic (the full write)
#include "io/mosaic/fileinfo.hpp"        // light manifest reader: the recents cards' dims/colour
#include "io/mosaic/journal_session.hpp" // recovery-journal autosave + crash restore (S48, flows 1/2)
#include "io/mosaic/lock.hpp"    // §2.10 advisory lock (S48, flow 6: already-open-elsewhere)
#include "io/mosaic/preview.hpp" // PRVW: the app supplies the composite, io stays render-free (S48-b)
#include "io/mosaic/preview.hpp" // readNewestPreview: .mosaic cards' embedded PRVW (S48-b)
#include "io/mosaic/salvage.hpp" // recovery past a gap (flows 3c/4): lineages + root conflict
#include "platform/display_refresh.hpp"
#include "platform/file_dialog.hpp"
#include "platform/font_db.hpp"
#include "platform/native_window.hpp"
#include "platform/session_end.hpp" // block a Windows session end while work is unsaved
#include "platform/system_theme.hpp"
#include "render/blur_gpu.hpp" // the Vulkan lane for the S33 blur adjustments (§8)
#include "render/compositor.hpp"
#include "render/document_ops.hpp" // the Image menu's whole-document operations (S53-a)
#include "render/extrude_gpu.hpp"  // the Vulkan lane for extruded text (S30-c §10.5)
#include "render/region_fill.hpp"  // computeFill: the shared S39 fill core (bucket fill, S21)
#include "render/texture_gpu.hpp"  // the Vulkan lane for the Texture Generator (S55-h §8.4)
#include "ui/about_dialog.hpp"
#include "ui/adjustment_panel.hpp"   // the S32 adjustment-layer param editor (pinned popover)
#include "ui/ask_or_tell_dialog.hpp" // close-with-unsaved-changes (S49); debug Help-menu exerciser
#include "ui/brush_editor.hpp"       // the modal brush editor (S19 Arc D, §8.3)
#include "ui/brush_preset_panel.hpp" // the dock's Brush-preset grid (S19 Arc D, §8.2)
#include "ui/brush_presets.hpp" // the brush-preset library + the Brush tool's active preset (S19 Arc D)
#include "ui/channels_panel.hpp" // the dock's Channels tab (per-channel histogram) source wiring
#include "ui/color_flyout.hpp" // the panels' colour line "Edit…" bubble (the Fill-dialog paradigm)
#include "ui/color_picker.hpp"
#include "ui/color_state.hpp"
#include "ui/crop_gesture.hpp"
#include "ui/export_dialog.hpp"
#include "ui/fill_dialog.hpp"
#include "ui/gradient_flyout.hpp" // the reusable stops/spread/blend-curve editor (S22 Gradient tool)
#include "ui/history_panel.hpp"
#include "ui/image_ops_panel.hpp" // the S53 Image-menu corner panel (Size / Canvas / Rotate)
#include "ui/keymap.hpp" // S51-b: the ONE source for every accelerator this window installs
#include "ui/layer_effects_dialog.hpp"
#include "ui/layer_panel.hpp"
#include "ui/loaded_history.hpp" // buildLoadedHistory: the loaded-save-history undo branch (spec 3.5)
#include "ui/menu_bar.hpp"
#include "ui/panel_arbiter.hpp"  // the unified corner-panel decision core (S32 round 5)
#include "ui/pattern_flyout.hpp" // the procedural-pattern editor (the Image-ops Fill = Pattern...)
#include "ui/recovery_flow.hpp"  // open-time recovery classification (flows 3a-3e + 4)
#include "ui/recovery_journal.hpp" // crash-restore classification (flows 1/2)
#include "ui/save_policy.hpp"      // the proactive early-fold decision (S48 Build 2)
#include "ui/texture_generator_dialog.hpp"
#include "ui/timing_graph_window.hpp" // the Help -> Timing Profiler window (see its header)
#include "ui/window_title.hpp"
#include "ui/xdg_thumbnails.hpp" // read-only desktop thumbnail cache for plain-image recents
#ifdef __APPLE__
#  include "ui/sys_menu_macos.hpp" // the system menu bar's application menu (About / Settings)

#  include <FL/platform.H> // fl_open_callback: Finder's "open documents" Apple Event
#endif
#include "ui/icon_pack.hpp"
#include "ui/menu_visibility.hpp"
#include "ui/motivation_ticker.hpp"
#include "ui/new_document_dialog.hpp"
#include "ui/popover.hpp"
#include "ui/red_eye_gesture.hpp"    // the S38-b eye tool's pure gesture math (scope + params)
#include "ui/resident_composite.hpp" // S60-a item 13: the device-resident composite lane (opt-in)
#include "ui/right_dock.hpp" // the dock container: Layers|History above the Brush presets (§8.2)
#include "ui/ruler.hpp"      // the canvas rulers (View -> Rulers): top/left gutter strips (px)
#include "ui/scrub_slider.hpp"
#include "ui/select_modify_dialog.hpp"
#include "ui/select_morph_panel.hpp" // the selection-morphology corner panel (live-preview)
#include "ui/settings_dialog.hpp"
#include "ui/shape_designer.hpp"
#include "ui/status_bar.hpp"
#include "ui/tab_strip.hpp"
#include "ui/theme.hpp"
#include "ui/tool.hpp"
#include "ui/tool_flyout.hpp"
#include "ui/tool_options.hpp"
#include "ui/toolbar.hpp"
#include "ui/type3d_panel.hpp" // the 3D popup (S30-d §8.4)
#include "ui/type_panel.hpp"
#include "ui/vulkan_canvas.hpp"
#include "ui/widgets.hpp"      // Dropdown, and the shared widget toolkit
#include "ui/window_hints.hpp" // installToplevelHintWatcher -- Wayland icon + dialog hints

#include <FL/Fl.H>
#include <FL/Fl_Copy_Surface.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input_.H> // the chrome-click unfocus in the global dispatch
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Shared_Image.H> // fl_register_images: external clipboard-image decode (S55)
#include <FL/fl_ask.H> // fl_beep ONLY -- message boxes are ui::AskOrTellDialog, never fl_alert
#include <FL/fl_draw.H>
#include <algorithm>
#include <assets/app_icon_svg.hpp> // generated: mosaic::assets::app_icon_svg[]
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mosaic::ui {
namespace {

// The globs the Open pickers offer, built from what this BUILD can actually decode. The four
// M4 codecs (WebP/AVIF/TIFF/GIF) are optional dependencies -- their symbols always exist but
// answer `*Supported() == false` when the library was absent at configure time -- so a static
// list would advertise formats that then fail to open on a stripped build. `loadImage` sniffs by
// magic bytes, not by extension, and JXL is deliberately absent: it is an ENCODE-only backend
// (no `ImageFormat::Jxl`, so nothing decodes it back in).
std::vector<std::string> openableImageGlobs() {
    std::vector<std::string> globs = {"*.png", "*.jpg", "*.jpeg"};
    if (io::webpSupported())
        globs.emplace_back("*.webp");
    if (io::avifSupported())
        globs.emplace_back("*.avif");
    if (io::tiffSupported()) {
        globs.emplace_back("*.tif");
        globs.emplace_back("*.tiff");
    }
    if (io::gifSupported())
        globs.emplace_back("*.gif");
    return globs;
}

#ifdef __APPLE__
// macOS never hands a double-clicked document to main() as argv. Finder launches the app and sends
// an "open documents" Apple Event, which FLTK surfaces through fl_open_callback -- and an app that
// ignores it sits there with an empty canvas while Finder waits out its launch timeout (that is the
// pause before the blank window, and why launching the app itself feels quicker).
//
// The event usually arrives BEFORE the first frame, and can arrive again at any time afterwards
// (double-clicking a second file while Mosaic runs). So the callback only QUEUES: run() drains
// whatever landed during startup, and the frame loop drains later arrivals.
std::vector<std::string>& macPendingOpens() {
    static std::vector<std::string> paths;
    return paths;
}

void macOpenDocumentCallback(const char* path) {
    if (path != nullptr && *path != '\0')
        macPendingOpens().emplace_back(path);
}
#endif

// The menu row's height inside the window. Zero on macOS (S58-b): the menus live in the system menu
// bar at the top of the SCREEN, so ui::MenuBar occupies no window row at all and the tool options
// bar starts at y=0. Every layout expression below stays written in terms of this constant, so the
// 28 px simply falls out of the arithmetic there.
#ifdef __APPLE__
constexpr int kMenuBarHeight = 0;
#else
constexpr int kMenuBarHeight = 28;
#endif
// The right-hand panel dock (Layers | History). Its width is user-draggable (the splitter on the
// dock's own left edge) and persisted in Settings::dockWidth; these bound it. The max keeps a wide
// window from turning into a dock with a canvas attached; the canvas minimum is what the clamp
// actually defends -- the dock may never squeeze the image area below it.
constexpr int kDockWidthDefault = 280;
constexpr int kDockMinWidth = 220; // below this the blend dropdown + opacity readout stop fitting
constexpr int kDockMaxWidth = 560;
constexpr int kCanvasMinWidth = 240;
// The IDLE heartbeat. Not a frame-rate cap: while the user is doing something the loop is paced by
// the display itself (see scheduleNextFrame), and this is what it falls back to when nothing is
// happening -- because a 144 Hz idle animation costs 2.4x the GPU of a 60 Hz one and looks
// identical.
constexpr double kFrameIntervalSec = 1.0 / 60.0;
// How long after the last sign of life the loop keeps running at display rate. Long enough to
// cover the gap between two keystrokes or a pause mid-drag, short enough that putting the mouse
// down drops the app back to the idle heartbeat within a blink.
constexpr double kActiveWindowSec = 0.35;
// Background rate: the window is not the active one, or is not on screen at all. Ten frames a
// second is enough for a progress bar or a marching-ants selection to look alive if the user
// glances over, and little enough that a Mosaic left open behind a browser costs nothing. It is
// NOT zero on purpose: background work (an inpaint, a save, an autosave tick) reports through the
// frame loop, and a window that stops drawing entirely also stops reporting.
constexpr double kBackgroundFrameIntervalSec = 1.0 / 10.0;
// The rate assumed when the platform cannot tell us the panel's (platform::displayRefreshHz
// returning 0: no RandR, a compositor that never reported a current mode, a Windows driver that
// answers "default"). ⚠ A CAP, never "no cap" -- an unbounded present rate is the defect the
// pacing exists to stop, and 60 Hz is the rate below which no panel Mosaic runs on is sold.
constexpr double kFallbackRefreshHz = 60.0;
// The believable range for a queried refresh rate. Outside it the number is not a refresh rate --
// it is a driver reporting nonsense -- and pacing on it would either freeze the canvas for a second
// at a time (too low) or defeat the whole bound (too high).
constexpr double kMinRefreshHz = 20.0;
constexpr double kMaxRefreshHz = 1000.0;
// How often the refresh rate is re-asked. The user has two panels at DIFFERENT rates and drags
// Mosaic between them, so this is a property of the window's current position, not of the session;
// four times a second re-paces within a blink of the window landing on the other monitor, and the
// query is cheap by construction (see platform/display_refresh.hpp).
constexpr double kRefreshRequerySec = 0.25;
// Quiet time after a font-hover preview before its DRAFT (half-res) canvas render is replaced by a
// crisp one. Short enough that a rest reads sharp immediately; long enough that scrubbing the list
// never pays a full-quality raster per row ("font hover still very laggy", user 2026-07-14).
constexpr double kTextHoverCrispSec = 0.18;
constexpr double kTextThumbSettleSec = 0.12; // quiet time after a text edit/hover before the layer
                                             // thumbnail re-renders (rev 11 -- not per drag tick)

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Map the persisted crop initial-framing key onto the canvas enum (S16-q). Anything but "draw"
// (including the default and unknown values) frames the whole canvas, the industry behaviour.
VulkanCanvas::CropFraming parseCropFraming(const std::string& key) {
    if (key == "draw")
        return VulkanCanvas::CropFraming::DrawToBegin;
    if (key == "inset")
        return VulkanCanvas::CropFraming::Inset;
    return VulkanCanvas::CropFraming::WholeCanvas;
}

// Map the persisted overlay-line-style key onto the present shader's style index (Settings →
// Appearance "Selection & reticle line"). Unknown values (a hand-edited file, a future style)
// fall back to the shipped default, Shadowed/rim.
int parseOverlayLineStyle(const std::string& key) {
    if (key == "classic")
        return 0;
    if (key == "adaptive")
        return 2;
    return 1;
}

// Map the persisted feathered-selection-indicator key onto the present shader's `featherStyle` index
// (Settings → Appearance "Feathered selection indicator"). Unknown values fall back to the default,
// the Bracketing ant pair (A).
int parseFeatherIndicator(const std::string& key) {
    if (key == "band")
        return 1;
    return 0; // "bracket" and anything unknown -> A, the default
}

LayerPanel::MultiSelectMode parseMultiSelectMode(const std::string& key) {
    if (key == "all")
        return LayerPanel::MultiSelectMode::All;
    if (key == "active")
        return LayerPanel::MultiSelectMode::Active;
    return LayerPanel::MultiSelectMode::Disabled;
}

spdlog::logger& uiLog() {
    static const auto logger = common::log::category("ui");
    return *logger;
}

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

void cbQuit(Fl_Widget*, void*) {
    // A file picker is modal: quitting while one is up would hide its parent out from under the
    // portal's nested wait loop, hanging the app on a reply that never comes (the reported freeze).
    // Ignore the quit -- the picker must be dealt with first.
    if (platform::fileDialogInFlight()) {
        fl_beep();
        return;
    }
    // Closing every window ends Fl::run(). Route through each window's own CLOSE CALLBACK rather
    // than hide(), which FLTK does not funnel through it: that is where the main window offers to
    // save the dirty documents and discards every open tab's recovery journal (S49). A window still
    // shown afterwards means the user cancelled -- stop quitting.
    while (Fl_Window* w = Fl::first_window()) {
        w->do_callback();
        if (w->shown())
            return;
    }
}

void cbAbout(Fl_Widget* w, void*) {
    showAboutDialog(w != nullptr ? w->window() : nullptr); // centre over the host window
}

#ifdef MOSAIC_DEBUG
void cbAskOrTellCrashDemo(Fl_Widget* w, void*) {
    runAskOrTellCrashRestoreDemo(w != nullptr ? w->window() : nullptr);
}

void cbAskOrTellRecoveryDemo(Fl_Widget* w, void*) {
    runAskOrTellFileRecoveryDemo(w != nullptr ? w->window() : nullptr);
}
#endif

// (The cbTodo placeholder callback retired with S53: the last five stub rows -- four in Image, one
// in Type -- became real commands, so nothing in the bar is a log line any more.)

// Menu actions that need the document/panel forward to MainWindow (which owns them); it and the
// thunks are defined below, and the menu is built in its constructor. File->New (S9); Edit
// Undo/Redo and Layer New/Delete drive the document + Layers panel (S10).
class MainWindow;
void cbFileNew(Fl_Widget*, void* mainWindow);
void cbFileOpen(Fl_Widget*, void* mainWindow);
void cbFileSave(Fl_Widget*, void* mainWindow);
void cbFileSaveAs(Fl_Widget*, void* mainWindow);
void cbRenameDocument(Fl_Widget*, void* mainWindow);
void cbFlattenHistory(Fl_Widget*, void* mainWindow);
void cbFileClose(Fl_Widget*, void* mainWindow);
void cbOpenAsLayer(Fl_Widget*, void* mainWindow);
// File -> Open Recent (S55). Menu items are found and rebuilt by CALLBACK identity (the
// setDocumentMenusVisible convention), so every dynamic row needs its own function address:
// one thunk per slot, plus the inactive "No Recent Files" placeholder's marker callback.
void cbClearRecents(Fl_Widget*, void* mainWindow);
void cbRecentNone(Fl_Widget*, void*) {} // never fired (the row is FL_MENU_INACTIVE)
void openRecentFromMenu(void* mainWindow, std::size_t index);
template <std::size_t N> void cbOpenRecentSlot(Fl_Widget*, void* mainWindow) {
    openRecentFromMenu(mainWindow, N);
}
constexpr Fl_Callback* kOpenRecentCbs[common::kMaxRecentFiles] = {
    cbOpenRecentSlot<0>, cbOpenRecentSlot<1>, cbOpenRecentSlot<2>, cbOpenRecentSlot<3>,
    cbOpenRecentSlot<4>, cbOpenRecentSlot<5>, cbOpenRecentSlot<6>, cbOpenRecentSlot<7>,
    cbOpenRecentSlot<8>, cbOpenRecentSlot<9>};
[[nodiscard]] bool isOpenRecentCallback(Fl_Callback* cb) {
    if (cb == cbRecentNone)
        return true;
    for (Fl_Callback* c : kOpenRecentCbs)
        if (cb == c)
            return true;
    return false;
}
#ifdef MOSAIC_DEBUG
void cbOpenDemoCanvas(Fl_Widget*, void* mainWindow);
void cbEnableAllControls(Fl_Widget*, void* mainWindow);
void cbDisableAllControls(Fl_Widget*, void* mainWindow);
#endif
void cbQuickExportPng(Fl_Widget*, void* mainWindow);
void cbQuickExportJpeg(Fl_Widget*, void* mainWindow);
void cbQuickExportJxl(Fl_Widget*, void* mainWindow);
void cbExportAs(Fl_Widget*, void* mainWindow);
void cbExportTo(Fl_Widget*, void* mainWindow);
void cbUndo(Fl_Widget*, void* mainWindow);
void cbRedo(Fl_Widget*, void* mainWindow);
void cbNewLayer(Fl_Widget*, void* mainWindow);
void cbDuplicateLayer(Fl_Widget*, void* mainWindow);
void cbDeleteLayer(Fl_Widget*, void* mainWindow);
void cbGroupLayers(Fl_Widget*, void* mainWindow);
void cbMergeDown(Fl_Widget*, void* mainWindow);
void cbLayerEffects(Fl_Widget*, void* mainWindow);
void cbTextureGenerator(Fl_Widget*, void* mainWindow);
void cbTimingGraph(Fl_Widget*, void* mainWindow); // Help -> Timing Profiler...
#ifdef MOSAIC_DEBUG
void cbToggleCanvasFps(Fl_Widget*, void* mainWindow); // Help -> Show Canvas FPS (toggle)
#endif
// Filter ▸ Adjustments (S32): one instantiation per kind -- each expands to its own C-compatible
// function pointer, so the nine menu items share a single implementation.
template <core::AdjustmentKind K> void cbNewAdjustment(Fl_Widget*, void* mainWindow);
// Arrange menu (align + distribute): one instantiation per edge / axis, each a distinct C-callable
// function pointer, so the eight menu items share one templated implementation.
template <core::AlignEdge E> void cbAlign(Fl_Widget*, void* mainWindow);
template <core::DistributeAxis A> void cbDistribute(Fl_Widget*, void* mainWindow);
void cbSelectAll(Fl_Widget*, void* mainWindow);
void cbDeselect(Fl_Widget*, void* mainWindow);
void cbInvertSelection(Fl_Widget*, void* mainWindow);
void cbGrowSelection(Fl_Widget*, void* mainWindow);
void cbShrinkSelection(Fl_Widget*, void* mainWindow);
void cbFeatherSelection(Fl_Widget*, void* mainWindow);
void cbSmoothSelection(Fl_Widget*, void* mainWindow);
void cbMaskFromSelection(Fl_Widget*, void* mainWindow);
void cbMaskFromInverseSelection(Fl_Widget*, void* mainWindow);
void cbAddToMask(Fl_Widget*, void* mainWindow);
void cbSubtractFromMask(Fl_Widget*, void* mainWindow);
void cbCut(Fl_Widget*, void* mainWindow);
void cbCopy(Fl_Widget*, void* mainWindow);
void cbCopyMerged(Fl_Widget*, void* mainWindow);
void cbPaste(Fl_Widget*, void* mainWindow);
void cbPasteInPlace(Fl_Widget*, void* mainWindow);
void cbClear(Fl_Widget*, void* mainWindow);
void cbFill(Fl_Widget*, void* mainWindow);
void cbSettings(Fl_Widget*, void* mainWindow);
// Image menu (S53). Each row needs its OWN function address: menu_visibility, the badge predicate
// and every state sync identify items by callback, never by position or path string -- so the
// orientation ops are a template with one instantiation per DocOrient rather than one shared
// callback reading its user_data.
void cbImageSize(Fl_Widget*, void* mainWindow);
void cbCanvasSize(Fl_Widget*, void* mainWindow);
void cbRotateArbitrary(Fl_Widget*, void* mainWindow);
void cbTrimToContent(Fl_Widget*, void* mainWindow);
template <render::DocOrient O> void cbOrient(Fl_Widget*, void* mainWindow);
// Type menu (S53-b). The three radio groups and the plain actions, one address each.
void cbRasterizeType(Fl_Widget*, void* mainWindow);
void cbTypeToShape(Fl_Widget*, void* mainWindow);
void cbTypeCharPanel(Fl_Widget*, void* mainWindow);
void cbType3dPanel(Fl_Widget*, void* mainWindow);
void cbTypeToPointText(Fl_Widget*, void* mainWindow);
void cbTypeToAreaText(Fl_Widget*, void* mainWindow);
void cbTypeOnPath(Fl_Widget*, void* mainWindow);
void cbTypeReleaseFromPath(Fl_Widget*, void* mainWindow);
void cbTypeWorkPath(Fl_Widget*, void* mainWindow);
void cbTypeUpdateAll(Fl_Widget*, void* mainWindow);
template <core::text::WritingMode M> void cbTypeWritingMode(Fl_Widget*, void* mainWindow);
template <core::text::AntiAlias A> void cbTypeAntiAlias(Fl_Widget*, void* mainWindow);
template <core::text::Kerning K> void cbTypeKerning(Fl_Widget*, void* mainWindow);
template <core::text::Paragraph::Direction D> void cbTypeDirection(Fl_Widget*, void* mainWindow);
// Layer menu (S53-b): the rows promoted out of the layer-row context menu, the two reorder steps,
// and the path booleans.
void cbRenameLayer(Fl_Widget*, void* mainWindow);
void cbRasterizeLayer(Fl_Widget*, void* mainWindow);
void cbConvertToPath(Fl_Widget*, void* mainWindow);
void cbMeshWarp(Fl_Widget*, void* mainWindow);        // S35-b: arm the warp tool on the active layer
void cbPerspectiveWarp(Fl_Widget*, void* mainWindow);
void cbAddMask(Fl_Widget*, void* mainWindow);
void cbDeleteMask(Fl_Widget*, void* mainWindow);
void cbToggleMaskEnabled(Fl_Widget*, void* mainWindow);
void cbToggleMaskLinked(Fl_Widget*, void* mainWindow);
void cbToggleLayerVisible(Fl_Widget*, void* mainWindow);
void cbToggleLayerLocked(Fl_Widget*, void* mainWindow);
void cbBringForward(Fl_Widget*, void* mainWindow);
void cbSendBackward(Fl_Widget*, void* mainWindow);
void cbFlattenToPath(Fl_Widget*, void* mainWindow);
template <core::vec::BoolOp Op> void cbCombinePaths(Fl_Widget*, void* mainWindow);
// Select menu (S53-b).
void cbReselect(Fl_Widget*, void* mainWindow);
void cbSelectAllLayers(Fl_Widget*, void* mainWindow);
// Filter menu (S53-b): re-run the last adjustment inserted from the Filter menu (Ctrl+F).
void cbLastFilter(Fl_Widget*, void* mainWindow);
void cbRulers(Fl_Widget*, void* mainWindow);      // View -> Rulers (toggle the canvas ruler gutter)
void cbShowGuides(Fl_Widget*, void* mainWindow);  // View -> Show Guides (per-document toggle)
void cbClearGuides(Fl_Widget*, void* mainWindow); // View -> Clear Guides (undoable)
void cbLockGuides(Fl_Widget*, void* mainWindow);  // View -> Lock Guides (per-document toggle)

// View-menu actions drive the canvas's view transform (S8); the canvas is passed as user_data.
void cbZoomIn(Fl_Widget*, void* c) {
    if (c != nullptr)
        static_cast<VulkanCanvas*>(c)->zoomIn();
}
void cbZoomOut(Fl_Widget*, void* c) {
    if (c != nullptr)
        static_cast<VulkanCanvas*>(c)->zoomOut();
}
void cbFitScreen(Fl_Widget*, void* c) {
    if (c != nullptr)
        static_cast<VulkanCanvas*>(c)->fitToWindow();
}
void cbActualPixels(Fl_Widget*, void* c) {
    if (c != nullptr)
        static_cast<VulkanCanvas*>(c)->actualPixels();
}

// The fixed zoom stops and the view-rotation set (S53-b). Every one of them already existed on
// CanvasView; the menu just never named them. They ride VulkanCanvas::viewState()/setViewState --
// the canvas's one public seam onto its view -- so the status-bar readouts and the frame kick
// follow for free. The rotation keeps the pan/zoom, and the reset stops keep the rotation, so each
// row does exactly the one thing its label says.
constexpr double kViewRotateStepDegrees = 15.0;
constexpr double kViewPi = 3.14159265358979323846;

void editCanvasView(void* c, const std::function<void(CanvasView::ViewState&)>& mutate) {
    if (c == nullptr)
        return;
    auto* canvas = static_cast<VulkanCanvas*>(c);
    CanvasView::ViewState s = canvas->viewState();
    mutate(s);
    canvas->setViewState(s);
}
void cbZoom200(Fl_Widget*, void* c) {
    editCanvasView(c, [](CanvasView::ViewState& s) { s.zoom = 2.0; });
}
void cbZoom50(Fl_Widget*, void* c) {
    editCanvasView(c, [](CanvasView::ViewState& s) { s.zoom = 0.5; });
}
void cbRotateViewCW(Fl_Widget*, void* c) {
    editCanvasView(c, [](CanvasView::ViewState& s) {
        s.rotation += kViewRotateStepDegrees * kViewPi / 180.0;
    });
}
void cbRotateViewCCW(Fl_Widget*, void* c) {
    editCanvasView(c, [](CanvasView::ViewState& s) {
        s.rotation -= kViewRotateStepDegrees * kViewPi / 180.0;
    });
}
void cbResetRotation(Fl_Widget*, void* c) {
    editCanvasView(c, [](CanvasView::ViewState& s) { s.rotation = 0.0; });
}
void cbResetView(Fl_Widget*, void* c) {
    if (c == nullptr)
        return;
    // Reset View = CanvasView::reset(): rotation cleared, then re-fit. Clearing FIRST matters --
    // fitToWindow frames the (possibly rotated) document, so fitting a still-turned view would
    // leave the zoom sized for a bounding box that is about to change.
    editCanvasView(c, [](CanvasView::ViewState& s) { s.rotation = 0.0; });
    static_cast<VulkanCanvas*>(c)->fitToWindow();
}
// View > Show Pixel Grid (S19-c): an FL_MENU_TOGGLE; its checked state is the grid on/off. The grid
// only renders at high zoom, so this just arms it (Photoshop's View > Show > Pixel Grid, default
// on).
void cbPixelGrid(Fl_Widget* w, void* c) {
    if (c == nullptr)
        return;
    const Fl_Menu_Item* item = static_cast<Fl_Menu_Bar*>(w)->mvalue();
    static_cast<VulkanCanvas*>(c)->setPixelGrid(item != nullptr && item->value() != 0);
}

// View > Snap + View > Snap To > (Guides/Canvas/Layer bounds): canvas-routed FL_MENU_TOGGLEs, like
// the pixel grid. Snap is the master switch; the three targets gate the candidate sources. Runtime
// toggles (not persisted), matching the canvas defaults (all on).
void cbSnap(Fl_Widget* w, void* c) {
    if (c == nullptr)
        return;
    const Fl_Menu_Item* item = static_cast<Fl_Menu_Bar*>(w)->mvalue();
    static_cast<VulkanCanvas*>(c)->setSnapEnabled(item != nullptr && item->value() != 0);
}
void cbSnapGuides(Fl_Widget* w, void* c) {
    if (c == nullptr)
        return;
    const Fl_Menu_Item* item = static_cast<Fl_Menu_Bar*>(w)->mvalue();
    static_cast<VulkanCanvas*>(c)->setSnapToGuides(item != nullptr && item->value() != 0);
}
void cbSnapCanvas(Fl_Widget* w, void* c) {
    if (c == nullptr)
        return;
    const Fl_Menu_Item* item = static_cast<Fl_Menu_Bar*>(w)->mvalue();
    static_cast<VulkanCanvas*>(c)->setSnapToCanvas(item != nullptr && item->value() != 0);
}
void cbSnapLayers(Fl_Widget* w, void* c) {
    if (c == nullptr)
        return;
    const Fl_Menu_Item* item = static_cast<Fl_Menu_Bar*>(w)->mvalue();
    static_cast<VulkanCanvas*>(c)->setSnapToLayers(item != nullptr && item->value() != 0);
}

// View > Smart Guides: dynamic alignment lines + snap while dragging a layer (canvas-routed toggle).
void cbSmartGuides(Fl_Widget* w, void* c) {
    if (c == nullptr)
        return;
    const Fl_Menu_Item* item = static_cast<Fl_Menu_Bar*>(w)->mvalue();
    static_cast<VulkanCanvas*>(c)->setSmartGuides(item != nullptr && item->value() != 0);
}

// Skeleton menu bar (PLAN S3). Most items are placeholders fleshed out in later sessions
// (S53); only Quit and About are wired up. user_data carries the (untranslated) action name
// for the not-yet-implemented log line. Labels are wrapped in _() for extraction; FLTK splits
// the translated string on '/', so translators must keep the path separators (the per-segment
// menu i18n refinement is part of S53/S54 -- see docs/i18n.md).
// A minimal themed one-line prompt (File -> Rename Document): caption, input, Cancel + accept.
// Enter accepts (the input's FL_WHEN_ENTER_KEY), Escape / WM-close cancels; the text arrives
// selected so typing replaces it. Returns nullopt on cancel.
std::optional<std::string> promptForText(const char* title, const char* caption,
                                         const char* acceptLabel, const std::string& initial,
                                         Fl_Window* host) {
    const Palette& pal = activePalette();
    constexpr int kW = 340;
    struct Ctx {
        Fl_Double_Window* win = nullptr;
        bool accepted = false;
    } ctx;
    Fl_Double_Window win(kW, 112, title);
    ctx.win = &win;
    win.color(toFl(pal.windowBg));
    win.begin();
    auto* cap = new Fl_Box(16, 12, kW - 32, 16, caption);
    cap->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    cap->box(FL_NO_BOX);
    cap->labelsize(11);
    cap->labelcolor(toFl(pal.textMuted));
    auto* input = new TextInput(16, 30, kW - 32, 26);
    input->box(MOSAIC_INPUT_BOX);
    input->color(toFl(pal.controlBg));
    input->textcolor(toFl(pal.text));
    input->cursor_color(toFl(pal.text));
    input->textsize(13);
    input->value(initial.c_str());
    input->when(FL_WHEN_ENTER_KEY);
    const auto accept = [](Fl_Widget*, void* v) {
        auto* c = static_cast<Ctx*>(v);
        c->accepted = true;
        c->win->hide();
    };
    input->callback(accept, &ctx);
    auto* ok = new FilledButton(kW - 16 - 96, 70, 96, 28, acceptLabel);
    ok->callback(accept, &ctx);
    auto* cancel = new FlatButton(kW - 16 - 96 - 8 - 88, 70, 88, 28, _("Cancel"));
    cancel->callback([](Fl_Widget*, void* v) { static_cast<Ctx*>(v)->win->hide(); }, &ctx);
    (new ContextMenu())->hide(); // the input's right-click menu host (created while unshown)
    win.end();
    win.callback([](Fl_Widget* w, void*) { w->hide(); }); // Escape / WM close = cancel
    win.set_modal();
    win.show();
    centerWindowOver(win, host);
    input->take_focus();
    input->insert_position(0, input->size()); // select all: typing replaces the old name
    while (win.shown())
        Fl::wait();
    if (!ctx.accepted)
        return std::nullopt;
    return std::string(input->value() != nullptr ? input->value() : "");
}

// --- Menu accelerators come from the keymap (S51-b) -------------------------------------------
// Every accelerator buildMenu() installs is looked up by ACTION ID instead of typed as a literal, so
// a remap in Settings ▸ Keybindings reaches the menu bar. `owner` is recorded on the way past,
// because re-applying a changed chord has to FIND the item again: item INDICES are not stable
// (rebuildRecentMenu inserts rows into File ▸ Open Recent, shifting everything after it) while
// callbacks are -- the same handle syncDynamicMenuItems and the badge predicate use.
//
// The list is filled by the one buildMenu() call this process makes and holds only the rows that
// actually ran: File ▸ Quit and Edit ▸ Settings are #ifdef'd out on macOS, where the system
// application menu owns them, so they are never recorded -- and never looked for -- there.
std::vector<std::pair<const char*, Fl_Callback*>>& menuAccelOwners() {
    static std::vector<std::pair<const char*, Fl_Callback*>> owners;
    return owners;
}

int accelFor(const Keymap& keys, const char* actionId, Fl_Callback* owner) {
    menuAccelOwners().emplace_back(actionId, owner);
    return keys.accel(actionId);
}

// The parameter is deliberately Fl_Menu_Bar*, not MenuBar*. Fl_Sys_Menu_Bar's add() HIDES the base
// one rather than overriding it (Fl_Menu_::add is not virtual), and its version rebuilds the whole
// macOS system menu after every single item. Taking the base type means these ~200 calls only fill
// the item array -- the caller publishes it once with a single MenuBar::update(), which is O(n)
// instead of O(n²) NSMenuItems at startup. Off macOS the two are the same function anyway.
void buildMenu(Fl_Menu_Bar* menu, VulkanCanvas* canvas, MainWindow* win, const Keymap& keys) {
    menuAccelOwners().clear(); // idempotent: a second build must not double the owner list
    menu->add(_("&File/&New..."), accelFor(keys, "file.new", cbFileNew), cbFileNew, win);
    menu->add(_("&File/&Open..."), accelFor(keys, "file.open", cbFileOpen), cbFileOpen, win);
    // Open Recent (S55): only the permanent tail -- Clear Recents -- is declared here (which also
    // creates the submenu); the file rows are inserted before it by rebuildRecentMenu().
    menu->add(_("&File/Open &Recent/&Clear Recents"), 0, cbClearRecents, win);
    menu->add(_("&File/Open as &Layer..."), 0, cbOpenAsLayer, win, FL_MENU_DIVIDER);
    menu->add(_("&File/&Save"), accelFor(keys, "file.save", cbFileSave), cbFileSave, win);
    menu->add(_("&File/Save &As..."), accelFor(keys, "file.save_as", cbFileSaveAs), cbFileSaveAs,
              win);
    // Rename Document = the document's own TITLE (the titlebar identity), not the file on disk.
    menu->add(_("&File/Rena&me Document..."), 0, cbRenameDocument, win);
    // Flatten History (S48, spec 3.6/3.7): the storage-reclaim escape hatch beside
    // history-on-by-default -- discard every retained undo state, keep only current content.
    menu->add(_("&File/Flatten &History..."), 0, cbFlattenHistory, win, FL_MENU_DIVIDER);
    // Export As... = the two-pane modal (Export & I/O plan §6); Quick Export -> {PNG,JPEG,JXL} are
    // the fast, no-dialog paths (§7). JPEG XL is offered only when libjxl was compiled in
    // (io::jxlSupported()); the divider lands after the last quick-export item either way.
    menu->add(_("&File/&Export As..."), accelFor(keys, "file.export_as", cbExportAs), cbExportAs,
              win);
    // "Export to <file>" = one-click re-export of this document's last target (§1, §9). The label
    // is rewritten and the row greyed by syncExportToMenuItem(); this is only its placeholder.
    menu->add(_("&File/Export to Last File"), 0, cbExportTo, win, FL_MENU_INACTIVE);
    menu->add(_("&File/&Quick Export as PNG"),
              accelFor(keys, "file.quick_export_png", cbQuickExportPng), cbQuickExportPng, win);
    menu->add(_("&File/Quick Export as &JPEG"), 0, cbQuickExportJpeg, win,
              io::jxlSupported() ? 0 : FL_MENU_DIVIDER);
    if (io::jxlSupported())
        menu->add(_("&File/Quick Export as JPEG &XL"), 0, cbQuickExportJxl, win, FL_MENU_DIVIDER);
#ifdef __APPLE__
    menu->add(_("&File/&Close"), accelFor(keys, "file.close", cbFileClose), cbFileClose, win);
    // No File ▸ Quit: macOS puts Quit (⌘Q) under the application's own name, and FLTK builds that
    // item itself. Same for Edit ▸ Settings and Help ▸ About below -- see installMacApplicationMenu.
    // Neither is recorded as a keymap owner there, so remapping them is a no-op on macOS (the OS
    // owns those two key equivalents); docs/keybindings.md says so.
#else
    menu->add(_("&File/&Close"), accelFor(keys, "file.close", cbFileClose), cbFileClose, win,
              FL_MENU_DIVIDER);
    menu->add(_("&File/&Quit"), accelFor(keys, "file.quit", cbQuit), cbQuit);
#endif

    menu->add(_("&Edit/&Undo"), accelFor(keys, "edit.undo", cbUndo), cbUndo, win);
    menu->add(_("&Edit/&Redo"), accelFor(keys, "edit.redo", cbRedo), cbRedo, win, FL_MENU_DIVIDER);
    menu->add(_("&Edit/Cu&t"), accelFor(keys, "edit.cut", cbCut), cbCut, win);
    menu->add(_("&Edit/&Copy"), accelFor(keys, "edit.copy", cbCopy), cbCopy, win);
    menu->add(_("&Edit/Copy &Merged"), accelFor(keys, "edit.copy_merged", cbCopyMerged),
              cbCopyMerged, win);
    menu->add(_("&Edit/&Paste"), accelFor(keys, "edit.paste", cbPaste), cbPaste, win);
    // Paste in Place (S53-b): the IN-APP clipboard back at the coordinates it was lifted from,
    // bypassing the OS-clipboard round trip (which re-centres any content it cannot recognise as
    // ours). Plain Paste stays the one that speaks to other applications.
    menu->add(_("&Edit/Paste in P&lace"), accelFor(keys, "edit.paste_in_place", cbPasteInPlace),
              cbPasteInPlace, win);
    // Clear = Cut's destructive half without the clipboard. Deliberately unbound: the canvas
    // already owns Delete/Backspace (text editing, S53-b), and a menu shortcut that only works
    // when the canvas happens not to want the key is worse than none.
    menu->add(_("&Edit/Cl&ear"), 0, cbClear, win, FL_MENU_DIVIDER);
#ifdef __APPLE__
    menu->add(_("&Edit/Fi&ll..."), accelFor(keys, "edit.fill", cbFill), cbFill, win);
#else
    menu->add(_("&Edit/Fi&ll..."), accelFor(keys, "edit.fill", cbFill), cbFill, win,
              FL_MENU_DIVIDER);
    menu->add(_("&Edit/&Settings..."), accelFor(keys, "edit.settings", cbSettings), cbSettings,
              win);
#endif

    // Image (S53, docs/image-operations.md): the whole-document operations, every one of them a
    // single undo step built on render::buildDocumentRemapCommand -- the same engine the Crop tool
    // uses, so a canvas resize rebases layers, masks, guides and the selection exactly as a crop
    // does. Image Size / Canvas Size / Rotate ▸ Arbitrary open the live-preview corner panel
    // (ImageOpsPanel); the four orientations and Trim are immediate. Each row wears a pictogram
    // badge (the Arrange precedent) so the two submenus read by eye rather than by label.
    menu->add(_("&Image/Image Si&ze..."), accelFor(keys, "image.image_size", cbImageSize),
              cbImageSize, win);
    menu->add(_("&Image/&Canvas Size..."), accelFor(keys, "image.canvas_size", cbCanvasSize),
              cbCanvasSize, win, FL_MENU_DIVIDER);
    menu->add(_("&Image/&Rotate/&90° Clockwise"), 0, cbOrient<render::DocOrient::Rotate90CW>, win);
    menu->add(_("&Image/&Rotate/9&0° Counter-Clockwise"), 0,
              cbOrient<render::DocOrient::Rotate90CCW>, win);
    menu->add(_("&Image/&Rotate/&180°"), 0, cbOrient<render::DocOrient::Rotate180>, win,
              FL_MENU_DIVIDER);
    // "Arbitrary…" is the only Rotate row that resamples; it lives with its siblings but opens the
    // panel (angle + resample kernel + the corner-wedge fill) instead of acting at once.
    menu->add(_("&Image/&Rotate/&Arbitrary..."), 0, cbRotateArbitrary, win);
    menu->add(_("&Image/&Flip/&Horizontal"), 0, cbOrient<render::DocOrient::FlipHorizontal>, win);
    menu->add(_("&Image/&Flip/&Vertical"), 0, cbOrient<render::DocOrient::FlipVertical>, win);
    menu->add(_("&Image/&Trim to Content"), 0, cbTrimToContent, win);

    // Layer Effects and Texture Generator are the two per-layer modal openers -- grouped together at
    // the top (each carries a clickable badge on its layer row), then the structural layer ops.
    menu->add(_("&Layer/Layer &Effects..."), 0, cbLayerEffects, win);
    menu->add(_("&Layer/Text&ure Generator..."), 0, cbTextureGenerator, win, FL_MENU_DIVIDER);
    menu->add(_("&Layer/&New Layer"), accelFor(keys, "layer.new", cbNewLayer), cbNewLayer, win);
    menu->add(_("&Layer/&Duplicate Layer"), accelFor(keys, "layer.duplicate", cbDuplicateLayer),
              cbDuplicateLayer, win);
    menu->add(_("&Layer/&Delete Layer"), 0, cbDeleteLayer, win);
    menu->add(_("&Layer/Rena&me Layer..."), 0, cbRenameLayer, win, FL_MENU_DIVIDER);
    menu->add(_("&Layer/&Group Layers"), accelFor(keys, "layer.group", cbGroupLayers),
              cbGroupLayers, win);
    menu->add(_("&Layer/Merge &Down"), accelFor(keys, "layer.merge_down", cbMergeDown), cbMergeDown,
              win);
    // Combine Paths (S53-b, docs/vector-model.md §9): the four polygon booleans over the Move
    // tool's multi-selection, folded into the BOTTOM-most selected vector layer (Merge Down's
    // rule) as ONE undo step. Greyed unless at least two vector layers are selected -- the items
    // are meaningless with one operand, and the boolean kernel says so by returning nullopt.
    // Flatten to Path bakes a live boolean object down to an editable polyline path.
    menu->add(_("&Layer/Co&mbine Paths/&Add"), 0, cbCombinePaths<core::vec::BoolOp::Union>, win);
    menu->add(_("&Layer/Co&mbine Paths/&Subtract"), 0,
              cbCombinePaths<core::vec::BoolOp::Subtract>, win);
    menu->add(_("&Layer/Co&mbine Paths/&Intersect"), 0,
              cbCombinePaths<core::vec::BoolOp::Intersect>, win);
    menu->add(_("&Layer/Co&mbine Paths/&Exclude"), 0,
              cbCombinePaths<core::vec::BoolOp::Exclude>, win);
    menu->add(_("&Layer/&Flatten to Path"), 0, cbFlattenToPath, win, FL_MENU_DIVIDER);
    // Reorder within the layer's own parent (MoveLayerCommand). Children are stored bottom->top,
    // so "forward" is +1 in the index.
    menu->add(_("&Layer/Bring &Forward"), accelFor(keys, "layer.bring_forward", cbBringForward),
              cbBringForward, win);
    menu->add(_("&Layer/Send Back&ward"), accelFor(keys, "layer.send_backward", cbSendBackward),
              cbSendBackward, win, FL_MENU_DIVIDER);
    // Reachable until now only from the layer-row context menu (layer_panel.cpp). Same commands,
    // same guards; the menu simply stops hiding them behind a right-click.
    menu->add(_("&Layer/&Rasterize"), 0, cbRasterizeLayer, win);
    menu->add(_("&Layer/Con&vert to Path"), 0, cbConvertToPath, win, FL_MENU_DIVIDER);
    // Warp (S35-b, docs/warp-tools.md) lives in the LAYER menu, beside Rasterize and Convert to
    // Path, because it is a per-layer geometry edit: it deforms ONE layer's pixels and stores its
    // lattice on that layer. The Image menu is documented right above as "the whole-document
    // operations", and putting a per-layer edit there would be a lie about its scope. A submenu,
    // like Rotate and Combine Paths, because the two variants belong together.
    menu->add(_("&Layer/&Warp/&Mesh Warp"), 0, cbMeshWarp, win);
    menu->add(_("&Layer/&Warp/&Perspective Warp"), 0, cbPerspectiveWarp, win);
    menu->add(_("&Layer/Add Mas&k"), 0, cbAddMask, win);
    menu->add(_("&Layer/Delete Mas&k"), 0, cbDeleteMask, win);
    // Two DYNAMIC rows: their labels flip with the active layer's mask flags (syncDynamicMenuItems
    // rewrites them through Fl_Menu_::replace). The placeholder text is the common case.
    menu->add(_("&Layer/Disable Mask"), 0, cbToggleMaskEnabled, win);
    menu->add(_("&Layer/Unlink Mask"), 0, cbToggleMaskLinked, win, FL_MENU_DIVIDER);
    menu->add(_("&Layer/Hide Layer"), 0, cbToggleLayerVisible, win);
    menu->add(_("&Layer/Lock Layer"), 0, cbToggleLayerLocked, win);

    // Type (S53-b). Everything here is re-exposure of an API that already exists -- the menu is
    // the surface the Type tool never had, not new engine work. Deliberately ABSENT, because
    // nothing backs them and a menu item is a promise: Photoshop's five anti-alias modes (the
    // engine is binary -- None or Grayscale), hinting (every FreeType load is FT_LOAD_NO_HINTING),
    // faux bold / faux italic, "Replace All Missing Fonts", and the non-arc warp styles.
    menu->add(_("&Type/&Rasterize Type"), 0, cbRasterizeType, win);
    menu->add(_("&Type/Convert to &Shape"), 0, cbTypeToShape, win, FL_MENU_DIVIDER);
    menu->add(_("&Type/&Character && Paragraph..."), 0, cbTypeCharPanel, win);
    menu->add(_("&Type/&3D Type..."), 0, cbType3dPanel, win, FL_MENU_DIVIDER);
    // Three radio groups over block/run/paragraph properties. FL_MENU_RADIO renders a filled dot
    // for the active member (S53-b menu-bar work); the group is the run of consecutive radio
    // items, so each set is added together and nothing but a divider may separate them.
    menu->add(_("&Type/&Orientation/&Horizontal"), 0,
              cbTypeWritingMode<core::text::WritingMode::HorizontalTB>, win, FL_MENU_RADIO);
    menu->add(_("&Type/&Orientation/Vertical (&right to left)"), 0,
              cbTypeWritingMode<core::text::WritingMode::VerticalRL>, win, FL_MENU_RADIO);
    menu->add(_("&Type/&Orientation/Vertical (&left to right)"), 0,
              cbTypeWritingMode<core::text::WritingMode::VerticalLR>, win, FL_MENU_RADIO);
    menu->add(_("&Type/&Anti-Alias/&None"), 0, cbTypeAntiAlias<core::text::AntiAlias::None>, win,
              FL_MENU_RADIO);
    menu->add(_("&Type/&Anti-Alias/&Grayscale"), 0,
              cbTypeAntiAlias<core::text::AntiAlias::Grayscale>, win, FL_MENU_RADIO);
    menu->add(_("&Type/&Kerning/&Metric"), 0, cbTypeKerning<core::text::Kerning::Metric>, win,
              FL_MENU_RADIO);
    menu->add(_("&Type/&Kerning/&Optical"), 0, cbTypeKerning<core::text::Kerning::Optical>, win,
              FL_MENU_RADIO);
    menu->add(_("&Type/&Kerning/N&one"), 0, cbTypeKerning<core::text::Kerning::None>, win,
              FL_MENU_RADIO);
    menu->add(_("&Type/&Direction/&Automatic"), 0,
              cbTypeDirection<core::text::Paragraph::Direction::Auto>, win, FL_MENU_RADIO);
    menu->add(_("&Type/&Direction/&Left to Right"), 0,
              cbTypeDirection<core::text::Paragraph::Direction::LTR>, win, FL_MENU_RADIO);
    // No FL_MENU_DIVIDER on the last radio of a group: FLTK reads a divider as the group's
    // TERMINATOR for setonly(), and -- because add() applies flags to the LEAF, never to the
    // submenu title -- it would also draw a stray separator inside the submenu rather than
    // between the groups and the rows below. The ▸ arrows are the separation here.
    menu->add(_("&Type/&Direction/&Right to Left"), 0,
              cbTypeDirection<core::text::Paragraph::Direction::RTL>, win, FL_MENU_RADIO);
    menu->add(_("&Type/Convert to &Point Text"), 0, cbTypeToPointText, win);
    menu->add(_("&Type/Convert to Area Te&xt"), 0, cbTypeToAreaText, win, FL_MENU_DIVIDER);
    menu->add(_("&Type/Text on Selected Pat&h"), 0, cbTypeOnPath, win);
    menu->add(_("&Type/Release from Path"), 0, cbTypeReleaseFromPath, win);
    menu->add(_("&Type/Create &Work Path"), 0, cbTypeWorkPath, win, FL_MENU_DIVIDER);
    menu->add(_("&Type/&Update All Text Layers"), 0, cbTypeUpdateAll, win);

    menu->add(_("&Select/Select &All"), accelFor(keys, "select.all", cbSelectAll), cbSelectAll, win);
    menu->add(_("&Select/&Deselect"), accelFor(keys, "select.deselect", cbDeselect), cbDeselect,
              win);
    // Reselect (S53-b): put back the last selection Deselect threw away, as one
    // SetSelectionCommand. Stays enabled and narrates its refusals (nothing remembered yet, or a
    // stash from a differently-sized canvas) rather than greying -- the Merge Down convention.
    menu->add(_("&Select/&Reselect"), accelFor(keys, "select.reselect", cbReselect), cbReselect, win);
    menu->add(_("&Select/&Inverse"), accelFor(keys, "select.inverse", cbInvertSelection),
              cbInvertSelection, win, FL_MENU_DIVIDER);
    // S18 morphology (docs/research-select-brush.md §4): each opens a small "N px" prompt and pushes
    // one SetSelectionCommand. Grouped under Inverse; a no-op on an empty selection (like Inverse).
    menu->add(_("&Select/&Grow..."), 0, cbGrowSelection, win);
    menu->add(_("&Select/S&hrink..."), 0, cbShrinkSelection, win);
    menu->add(_("&Select/&Feather..."), 0, cbFeatherSelection, win);
    menu->add(_("&Select/S&mooth..."), 0, cbSmoothSelection, win, FL_MENU_DIVIDER);
    // S31: the entry docs/research-select-brush.md §4.5 deliberately left ABSENT until now (a
    // greyed item is a promise). Makes a raster mask on the active layer from the selection.
    menu->add(_("&Select/&Mask from Selection"), 0, cbMaskFromSelection, win);
    menu->add(_("&Select/Mask from I&nverse Selection"), 0, cbMaskFromInverseSelection, win);
    // Combine the selection into the active layer's EXISTING mask instead of replacing it: Add
    // unions it in (max), Subtract carves it out (existing * (1 - selection)). Same guards + path
    // as the two above (maskFromSelectionEntry drives all four via a MaskCombine mode).
    menu->add(_("&Select/Add &to Mask"), 0, cbAddToMask, win);
    menu->add(_("&Select/&Subtract from Mask"), 0, cbSubtractFromMask, win, FL_MENU_DIVIDER);
    // Select All Layers (S53-b): every visible top-level unit, gathered exactly the way a
    // full-canvas marquee band would gather it (core::layersInMarquee -- a group counts as one
    // object). Feeds the Layers panel's multi-selection, which is what Combine Paths reads.
    menu->add(_("&Select/Select All &Layers"),
              accelFor(keys, "select.all_layers", cbSelectAllLayers), cbSelectAllLayers, win);

    // S32 (docs/adjustment-layers.md): each item inserts a non-destructive adjustment layer above
    // the active layer (scoped to its group purely by tree position) and opens the live param
    // editor. Only implemented kinds are offered; since S34 that is every kind.
    // Literal slashes in labels are escaped ("\\/"); FLTK splits menu paths on '/'.
    //
    // Last Filter (S53-b) re-runs the kind the Filter menu last inserted, with the same
    // insert-above-the-active-layer + automask behaviour. Born INACTIVE and switched on the first
    // time a filter runs (there is no "last" before then) -- the greying is the whole explanation.
    menu->add(_("Filte&r/&Last Filter"), accelFor(keys, "filter.last", cbLastFilter), cbLastFilter,
              win, FL_MENU_INACTIVE | FL_MENU_DIVIDER);
    menu->add(_("Filte&r/&Adjustments/&Brightness\\/Contrast..."), 0,
              cbNewAdjustment<core::AdjustmentKind::BrightnessContrast>, win);
    menu->add(_("Filte&r/&Adjustments/&Levels..."), 0,
              cbNewAdjustment<core::AdjustmentKind::Levels>, win);
    menu->add(_("Filte&r/&Adjustments/Cur&ves..."), 0,
              cbNewAdjustment<core::AdjustmentKind::Curves>, win);
    menu->add(_("Filte&r/&Adjustments/&Exposure..."), 0,
              cbNewAdjustment<core::AdjustmentKind::Exposure>, win);
    menu->add(_("Filte&r/&Adjustments/&Hue\\/Saturation..."), 0,
              cbNewAdjustment<core::AdjustmentKind::HueSaturation>, win);
    // S34-a: Vibrance sits with the saturation controls (it IS one -- weighted by the pixel's own
    // chroma); Photo Filter closes the colour-grade group.
    menu->add(_("Filte&r/&Adjustments/Vibr&ance..."), 0,
              cbNewAdjustment<core::AdjustmentKind::Vibrance>, win);
    menu->add(_("Filte&r/&Adjustments/&Color Balance..."), 0,
              cbNewAdjustment<core::AdjustmentKind::ColorBalance>, win);
    menu->add(_("Filte&r/&Adjustments/Photo &Filter..."), 0,
              cbNewAdjustment<core::AdjustmentKind::PhotoFilter>, win, FL_MENU_DIVIDER);
    menu->add(_("Filte&r/&Adjustments/&Posterize..."), 0,
              cbNewAdjustment<core::AdjustmentKind::Posterize>, win);
    menu->add(_("Filte&r/&Adjustments/&Threshold..."), 0,
              cbNewAdjustment<core::AdjustmentKind::Threshold>, win, FL_MENU_DIVIDER);
    menu->add(_("Filte&r/&Adjustments/&Grayscale..."), 0,
              cbNewAdjustment<core::AdjustmentKind::Grayscale>, win);
    // S34-a: a gradient map is Grayscale's colour sibling -- both remap the luma axis.
    menu->add(_("Filte&r/&Adjustments/Gradie&nt Map..."), 0,
              cbNewAdjustment<core::AdjustmentKind::GradientMap>, win);
    menu->add(_("Filte&r/&Adjustments/&Invert"), 0,
              cbNewAdjustment<core::AdjustmentKind::Invert>, win, FL_MENU_DIVIDER);
    // S34 (docs/adjustment-layers.md §2.2-§2.5): the photographic / compositing repairs. Same
    // insert mechanics; Shadows/Highlights and Defringe are SPATIAL (they read the backdrop's
    // neighbourhood), Matte Removal and Haze Removal are per-pixel.
    menu->add(_("Filte&r/&Adjustments/Shado&ws\\/Highlights..."), 0,
              cbNewAdjustment<core::AdjustmentKind::ShadowsHighlights>, win);
    menu->add(_("Filte&r/&Adjustments/Ha&ze Removal..."), 0,
              cbNewAdjustment<core::AdjustmentKind::HazeRemoval>, win);
    menu->add(_("Filte&r/&Adjustments/&Defringe..."), 0,
              cbNewAdjustment<core::AdjustmentKind::Defringe>, win);
    menu->add(_("Filte&r/&Adjustments/&Matte Removal..."), 0,
              cbNewAdjustment<core::AdjustmentKind::MatteRemoval>, win);
    // S33 (docs/blur-filters.md): the blur gallery -- same insert mechanics as Adjustments; the
    // spatial kinds blur the composited backdrop below them. Depth of Field additionally grows
    // on-canvas focus-band handles while its layer is active.
    menu->add(_("Filte&r/&Blur/&Gaussian Blur..."), 0,
              cbNewAdjustment<core::AdjustmentKind::GaussianBlur>, win);
    menu->add(_("Filte&r/&Blur/Bo&x Blur..."), 0,
              cbNewAdjustment<core::AdjustmentKind::BoxBlur>, win);
    menu->add(_("Filte&r/&Blur/&Motion Blur..."), 0,
              cbNewAdjustment<core::AdjustmentKind::MotionBlur>, win);
    menu->add(_("Filte&r/&Blur/&Radial Blur..."), 0,
              cbNewAdjustment<core::AdjustmentKind::RadialBlur>, win);
    menu->add(_("Filte&r/&Blur/&Surface Blur..."), 0,
              cbNewAdjustment<core::AdjustmentKind::SurfaceBlur>, win, FL_MENU_DIVIDER);
    menu->add(_("Filte&r/&Blur/&Lens Blur..."), 0,
              cbNewAdjustment<core::AdjustmentKind::LensBlur>, win);
    menu->add(_("Filte&r/&Blur/&Depth of Field..."), 0,
              cbNewAdjustment<core::AdjustmentKind::DofBlur>, win);
    // S35 (docs/filters-stylize.md): the artistic/stylize gallery -- same insert mechanics as
    // Blur (adjustment layers, automasked to an active selection). Grouped by what the filter
    // does to the image: sharpen | grain | abstraction | geometry & light.
    menu->add(_("Filte&r/&Stylize/&Sharpen..."), 0,
              cbNewAdjustment<core::AdjustmentKind::Sharpen>, win);
    menu->add(_("Filte&r/&Stylize/&Unsharp Mask..."), 0,
              cbNewAdjustment<core::AdjustmentKind::UnsharpMask>, win);
    // S34-a: the same Gaussian difference without the add-back, so it belongs in the sharpen
    // group -- "the layer you set to Overlay" is what people reach for next to it.
    menu->add(_("Filte&r/&Stylize/&High Pass..."), 0,
              cbNewAdjustment<core::AdjustmentKind::HighPass>, win, FL_MENU_DIVIDER);
    menu->add(_("Filte&r/&Stylize/&Add Noise..."), 0,
              cbNewAdjustment<core::AdjustmentKind::AddNoise>, win);
    menu->add(_("Filte&r/&Stylize/&Denoise..."), 0,
              cbNewAdjustment<core::AdjustmentKind::Denoise>, win, FL_MENU_DIVIDER);
    menu->add(_("Filte&r/&Stylize/&Pixelate..."), 0,
              cbNewAdjustment<core::AdjustmentKind::Pixelate>, win);
    menu->add(_("Filte&r/&Stylize/&Emboss..."), 0,
              cbNewAdjustment<core::AdjustmentKind::Emboss>, win);
    menu->add(_("Filte&r/&Stylize/&Oil Paint..."), 0,
              cbNewAdjustment<core::AdjustmentKind::OilPaint>, win, FL_MENU_DIVIDER);
    menu->add(_("Filte&r/&Stylize/&Wave..."), 0,
              cbNewAdjustment<core::AdjustmentKind::Wave>, win);
    menu->add(_("Filte&r/&Stylize/&Vignette..."), 0,
              cbNewAdjustment<core::AdjustmentKind::Vignette>, win);

    // Arrange (align + distribute the Move-tool layer selection). Chosen as its own top-level menu
    // (Illustrator/Affinity convention) so it inherits the document-only show/hide and keeps the
    // Layer menu uncluttered. Operates on the shift-click multi-selection (a single layer aligns
    // to the canvas); one undoable step each. Dividers split horizontal / vertical / distribute,
    // and every Align item wears its direction pictogram in the pop-up's right gutter (the
    // setItemBadge predicate) -- the user's gripe with other software is that the alignment
    // options are a pain to tell apart quickly by eye.
    menu->add(_("&Arrange/Align &Left"), 0, cbAlign<core::AlignEdge::Left>, win);
    menu->add(_("&Arrange/Align &Center"), 0, cbAlign<core::AlignEdge::HCenter>, win);
    menu->add(_("&Arrange/Align &Right"), 0, cbAlign<core::AlignEdge::Right>, win, FL_MENU_DIVIDER);
    menu->add(_("&Arrange/Align &Top"), 0, cbAlign<core::AlignEdge::Top>, win);
    menu->add(_("&Arrange/Align Mi&ddle"), 0, cbAlign<core::AlignEdge::VMiddle>, win);
    menu->add(_("&Arrange/Align &Bottom"), 0, cbAlign<core::AlignEdge::Bottom>, win, FL_MENU_DIVIDER);
    menu->add(_("&Arrange/Distribute &Horizontally"), 0,
              cbDistribute<core::DistributeAxis::Horizontal>, win);
    menu->add(_("&Arrange/Distribute &Vertically"), 0,
              cbDistribute<core::DistributeAxis::Vertical>, win);

    menu->add(_("&View/Zoom &In"), accelFor(keys, "view.zoom_in", cbZoomIn), cbZoomIn, canvas);
    menu->add(_("&View/Zoom &Out"), accelFor(keys, "view.zoom_out", cbZoomOut), cbZoomOut, canvas);
    menu->add(_("&View/&Fit on Screen"), accelFor(keys, "view.fit_on_screen", cbFitScreen),
              cbFitScreen, canvas);
    menu->add(_("&View/Actual Pi&xels"), 0, cbActualPixels, canvas);
    // The two fixed zoom stops and the view-rotation set (S53-b). All of it already existed on
    // CanvasView; only the menu rows were missing. The canvas is the user_data, exactly like the
    // four zoom rows above.
    menu->add(_("&View/Zoom &200%"), 0, cbZoom200, canvas);
    menu->add(_("&View/Zoom &50%"), 0, cbZoom50, canvas, FL_MENU_DIVIDER);
    menu->add(_("&View/Rotate View &Clockwise"), 0, cbRotateViewCW, canvas);
    menu->add(_("&View/Rotate View Counter-Clock&wise"), 0, cbRotateViewCCW, canvas);
    menu->add(_("&View/Reset Rotatio&n"), 0, cbResetRotation, canvas);
    menu->add(_("&View/Reset Vie&w"), 0, cbResetView, canvas, FL_MENU_DIVIDER);
    // Pixel grid (S19-c): a checkable toggle, default ON to match Photoshop (only visible at high
    // zoom). FL_MENU_VALUE pre-checks it so the menu state agrees with the renderer's default.
    menu->add(_("&View/Show Pi&xel Grid"), 0, cbPixelGrid, canvas,
              FL_MENU_TOGGLE | FL_MENU_VALUE | FL_MENU_DIVIDER);
    // Rulers (View -> Rulers, Ctrl+R): a top/left gutter showing document coordinates in px. Off by
    // default (it reflows the canvas), a runtime toggle like the pixel grid -- not persisted.
    menu->add(_("&View/&Rulers"), accelFor(keys, "view.rulers", cbRulers), cbRulers, win,
              FL_MENU_TOGGLE);
    // Guides (View -> Guides): draggable reference lines stored per document. Show Guides + Lock
    // Guides are per-document view flags (their checkmarks re-sync per tab); Clear Guides is an
    // undoable command. Drag a guide out of a ruler to create one.
    menu->add(_("&View/Show &Guides"), accelFor(keys, "view.show_guides", cbShowGuides),
              cbShowGuides, win, FL_MENU_TOGGLE | FL_MENU_VALUE);
    menu->add(_("&View/C&lear Guides"), 0, cbClearGuides, win);
    menu->add(_("&View/Loc&k Guides"), 0, cbLockGuides, win, FL_MENU_TOGGLE | FL_MENU_DIVIDER);
    // Snapping (View -> Snap + Snap To submenu): canvas-routed toggles, all on by default.
    menu->add(_("&View/&Snap"), 0, cbSnap, canvas, FL_MENU_TOGGLE | FL_MENU_VALUE);
    menu->add(_("&View/Snap &To/&Guides"), 0, cbSnapGuides, canvas, FL_MENU_TOGGLE | FL_MENU_VALUE);
    menu->add(_("&View/Snap &To/&Canvas"), 0, cbSnapCanvas, canvas, FL_MENU_TOGGLE | FL_MENU_VALUE);
    menu->add(_("&View/Snap &To/&Layer Bounds"), 0, cbSnapLayers, canvas,
              FL_MENU_TOGGLE | FL_MENU_VALUE);
    // Smart guides (View -> Smart Guides): magenta alignment lines + snap while dragging a layer.
    menu->add(_("&View/S&mart Guides"), 0, cbSmartGuides, canvas, FL_MENU_TOGGLE | FL_MENU_VALUE);

#ifdef MOSAIC_DEBUG
    // Debug builds only, below a separator (the FL_MENU_DIVIDER on About draws after it):
    // - Diagnostics: a live FPS readout in the canvas corner (a checkable toggle whose checkmark
    //   tracks the state) and a non-modal Timing Profiler window (per-op CPU/GPU min/max/avg,
    //   slowest first). Debug-only -- the user does not want this diagnostic code in release builds.
    // - Open Demo Canvas: the pre-S50 startup placeholder (blend-mode scenery), since the app
    //   now starts on the empty click-or-drop state.
    // - Test Ask-or-Tell: one exerciser per recovery flow (docs/askortell-dialog.md,
    //   recovery-family flows) -- crash restore and file recovery are DIFFERENT mechanisms
    //   with different faces, so each demo rehearses exactly one, wearing the settled copy.
    // - Enable/Disable all controls: greys (or restores) every chrome region at once, so the
    //   disabled rendering of every control kind can be compared side by side. The menu bar is
    //   deliberately left alive -- it is the way back.
    //
    // These live under their OWN top-level "Debug" title, not appended to Help. They are
    // deliberately not _()-wrapped (dev-only items must not burden the translation template), and
    // an unwrapped "&Help/..." path does not join the localized Help menu -- _() translates the
    // WHOLE path, so FLTK sees a different parent string and forks a SECOND, English "Help" title
    // beside "Hilfe"/"Aide". A separate Debug menu is what the items always meant anyway.
#ifndef __APPLE__
    // On macOS this is the whole of Help, and About belongs under the application's own name --
    // so there the Help title is not built at all rather than left standing empty (AppKit would
    // graft its own search field onto a menu literally titled "Help").
    menu->add(_("&Help/&About Mosaic"), 0, cbAbout);
#endif
    menu->add("&Debug/Show Canvas FPS", 0, cbToggleCanvasFps, win, FL_MENU_TOGGLE);
    menu->add("&Debug/Timing Profiler...", 0, cbTimingGraph, win, FL_MENU_DIVIDER);
    menu->add("&Debug/Open Demo Canvas", 0, cbOpenDemoCanvas, win);
    menu->add("&Debug/Test Ask-or-Tell: Crash Restore...", 0, cbAskOrTellCrashDemo);
    menu->add("&Debug/Test Ask-or-Tell: File Recovery...", 0, cbAskOrTellRecoveryDemo,
              nullptr, FL_MENU_DIVIDER);
    menu->add("&Debug/Disable All Controls", 0, cbDisableAllControls, win);
    menu->add("&Debug/Enable All Controls", 0, cbEnableAllControls, win);
#else
    // Release: the Timing Profiler is offered ONLY while profiling is on (--profile /
    // MOSAIC_PROFILE=1, parsed before the menu is built). Without the flag a release build shows
    // nothing but About -- the diagnostic surface is not part of the shipped product, it is part
    // of the flag. (S60-alpha; same Debug-title reasoning as the debug build above.)
#ifndef __APPLE__
    menu->add(_("&Help/&About Mosaic"), 0, cbAbout); // macOS: in the application menu (see above)
#endif
    if (Profiler::enabled()) {
        menu->add("&Debug/Timing Profiler...", 0, cbTimingGraph, win);
    }
#endif
}

common::Image appIconImage(int px) {
    std::string err;
    common::Image img = common::rasterizeSvg(mosaic::assets::app_icon_svg,
                                             mosaic::assets::app_icon_svg_size, px, px, &err);
    if (img.empty()) {
        uiLog().warn("app icon rasterization failed: {}", err);
    }
    return img;
}

#ifdef MOSAIC_DEBUG
// Fill the half-open rectangle [x0,x1) x [y0,y1) of `img` with `c` (clipped to bounds).
void fillRect(common::Image& img, int x0, int y0, int x1, int y1, common::Color8 c) {
    for (int y = std::max(0, y0); y < std::min(static_cast<int>(img.height), y1); ++y) {
        for (int x = std::max(0, x0); x < std::min(static_cast<int>(img.width), x1); ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
            img.rgba[p] = c.r;
            img.rgba[p + 1] = c.g;
            img.rgba[p + 2] = c.b;
            img.rgba[p + 3] = c.a;
        }
    }
}

// The original startup placeholder document -- a few overlapping raster layers with blend
// modes, on a canvas with a transparent border -- kept as DEBUG scenery (Help -> Open Demo
// Canvas) now that the app starts on the empty click-or-drop state (S50 prerequisite): handy
// for eyeballing the compositor without hunting for a file.
std::unique_ptr<core::Document> buildPlaceholderDocument() {
    auto doc = std::make_unique<core::Document>(480, 320);
    core::GroupLayer& root = doc->root();

    auto base = doc->makeRaster("Background");
    fillRect(base->image(), 24, 24, 456, 296, {54, 79, 140, 255}); // blue, transparent frame
    root.addOnTop(std::move(base));

    auto red = doc->makeRaster("Red x Multiply");
    fillRect(red->image(), 60, 80, 240, 250, {214, 73, 73, 255});
    red->setBlendMode(core::BlendMode::Multiply);
    root.addOnTop(std::move(red));

    auto green = doc->makeRaster("Green Screen");
    fillRect(green->image(), 180, 120, 360, 285, {80, 196, 108, 255});
    green->setBlendMode(core::BlendMode::Screen);
    green->setOpacity(0.85f);
    root.addOnTop(std::move(green));

    auto gold = doc->makeRaster("Highlight");
    fillRect(gold->image(), 300, 45, 432, 175, {240, 198, 86, 255});
    gold->setOpacity(0.6f);
    root.addOnTop(std::move(gold));
    return doc;
}
#endif // MOSAIC_DEBUG

class MainWindow : public Fl_Double_Window {
public:
    MainWindow(int W, int H, int autoQuitFrames)
        : Fl_Double_Window(W, H, "Mosaic"), m_autoQuitFrames(autoQuitFrames) {
        const Palette& pal = activePalette();
        color(toFl(pal.windowBg));
        begin();
        // Layout: menu (top), the tool options bar under it (full width), then the body -- a fixed
        // left toolbar, the canvas (which absorbs window resizes) in the middle, and the Layers
        // dock pinned to the right -- and the status bar along the bottom (S13-b). Canvas first, so
        // the View menu can bind its actions to it.
        const int bodyTop = kMenuBarHeight + kOptionsBarHeight;
        const int bodyH = H - bodyTop - kStatusBarHeight;
        const int canvasW = W - kToolbarWidth - kDockWidthDefault;
        m_canvas = new VulkanCanvas(kToolbarWidth, bodyTop, canvasW, bodyH);
        m_canvas->setClearColor(pal.canvasBg);
        // The no-document invitation is the canvas's own idle pass (the ripple field + baked
        // open-an-image quad, canvas_idle.comp) -- the old EmptyStateView sub-window could only
        // sit opaquely over the Vulkan surface and could never fade. The canvas takes the
        // invitation's input too; clearDocument()/adoptActiveDocument() toggle the state, and a
        // click routes to the same File->Open path.
        // S60-a item 13: the resident lane borrows the canvas's VkDevice, and hide() destroys that
        // device well before ~MainWindow runs -- so the lane is released HERE, on the canvas's own
        // teardown path, with the device still alive and idle. Clearing the latch too means a
        // re-shown canvas rebuilds the lane on its NEW device rather than keeping a dangling one.
        m_canvas->setOnRendererShutdown([this] {
            m_tiles.reset();
            m_tileLaneTried = false;
        });
        m_canvas->setOnIdleOpen([this] { openDocument(); });
        m_canvas->setOnIdleOpenPath([this](const std::string& path) { // a drop on the canvas
            uiLog().debug("empty-state drop: {}", path);
            openDocumentAtPath(path);
        });
        // The canvas rulers (View -> Rulers): gutter strips along the canvas top/left. Created after
        // the canvas (they read its CanvasView) and hidden until toggled on; applyDockWidth() places
        // them + insets the canvas. The horizontal strip spans the full canvas column so it also
        // fills the top-left corner (its ticks start at the canvas's own left edge).
        m_rulerH = new RulerStrip(kToolbarWidth, bodyTop, canvasW, kRulerSize,
                                  RulerStrip::Orientation::Horizontal, m_canvas);
        m_rulerV = new RulerStrip(kToolbarWidth, bodyTop + kRulerSize, kRulerSize,
                                  std::max(1, bodyH - kRulerSize), RulerStrip::Orientation::Vertical,
                                  m_canvas);
        m_rulerH->hide();
        m_rulerV->hide();
        // Drag OUT of a ruler to pull a new guide onto the canvas (View -> Guides): the ruler owns
        // the pointer capture and feeds document coordinates; the canvas previews + commits it.
        for (RulerStrip* r : {m_rulerH, m_rulerV}) {
            r->onGuideBegin = [this](bool horizontalGuide, double pos) {
                m_canvas->beginGuideCreate(horizontalGuide, pos);
            };
            r->onGuideUpdate = [this](double pos) { m_canvas->updateGuideCreate(pos); };
            r->onGuideEnd = [this](bool cancel) {
                if (cancel)
                    m_canvas->cancelGuideCreate(); // dropped back on the ruler: discard
                else
                    m_canvas->commitGuideCreate(); // land it as one AddGuideCommand
            };
        }
        // Deliberately no tooltip on the canvas: a popup over the work area would be a constant
        // irritation while editing (PLAN §S8). Tooltips live on the toolbar/panels instead.
        m_toolbar = new LeftToolbar(0, bodyTop, kToolbarWidth, bodyH, m_tools, m_colors);
        // The right dock (§8.2): the tabbed Layers|History panel, and -- while the Brush is the
        // active tool -- the preset grid below it, with a draggable splitter between them. The dock
        // is the container; m_layerPanel is still the panel every layer call site talks to.
        m_dock = new RightDock(W - kDockWidthDefault, bodyTop, kDockWidthDefault, bodyH);
        m_layerPanel = m_dock->layers();
        m_layerPanel->setOnChange([this] { requestRecomposite(/*fitView=*/false); });
        // The Channels tab's histogram reads the on-screen composite (visible layers) through this
        // provider, so it never copies the image; recompositeNow's notifyChanged re-bins it only when
        // the composite actually changed (or when the tab is (re)shown).
        if (ChannelsPanel* chans = m_layerPanel->channels()) {
            // Audit A5, routed through the explicit readback seam (S60-a item 12): with the
            // resident lane off this is the same `&m_lastComposite` it has always been; with it on
            // the histogram is a named, staleness-tolerant reader of the device accumulator, and
            // the mirror it gets is memoised on the composite revision it already re-bins against.
            chans->setSourceProvider([this]() -> const common::Image* {
                return &hostComposite(render::consumers::kHistogram, render::Freshness::AnyRecent);
            });
            // Toggling a channel eye can isolate/hide channels on the CANVAS (not just the
            // histogram). The view is a present-pass parameter (S60-a item 10), so all this owes
            // is a frame -- no recomposite, no re-mask, no upload.
            chans->setOnIsolationChanged([this] { refreshCanvasForIsolation(); });
        }
        m_layerPanel->setOnOpenEffects([this](core::LayerId) { openLayerEffects(); });
        // The texture badge's click path (mirrors the fx badge): openTextureFor sets the layer active
        // first, so openTextureGenerator sees a TextureLayer selected and opens in EDIT mode (§3.3).
        m_layerPanel->setOnOpenTexture([this](core::LayerId) { openTextureGenerator(); });
        m_layerPanel->setOnMergeDown([this] { mergeDownLayer(); }); // the row context menu's item
        m_layerPanel->setOnRasterize([this](core::LayerId id) { rasterizeLayerCommand(id); });
        m_layerPanel->setOnConvertToPath(
            [this](core::LayerId id) { convertLayerToPathCommand(id); });
        // The dock's own row grammar (Ctrl/Cmd toggles, Shift extends) edits the SAME selection the
        // Move tool's marquee does, so it is mirrored onto the canvas's move targets -- otherwise
        // Arrange and the transform handles keep answering to whatever the canvas was last clicked
        // with, which is the half-a-feature selectAllLayers already fixed in the other direction.
        // setMoveTargets re-asserts the panel's active row from the set's PRIMARY (and blanks it on
        // an emptied set), so the row the panel just chose is restored afterwards.
        m_layerPanel->setOnSelectionChanged([this](const std::vector<core::LayerId>& sel) {
            if (!m_canvas)
                return;
            const core::LayerId active = m_layerPanel->activeLayer();
            m_canvas->setMoveTargets(sel);
            m_layerPanel->setActive(active);
        });
        // The dock has no status bar of its own: its refusals (a Shift-clicked thumbnail on a layer
        // with no pixels anywhere) are narrated through the window's.
        m_layerPanel->setOnStatus([this](std::string message) { transientStatus(message); });
        m_dock->setOnWidthRequest([this](int width, bool committed) {
            setDockWidth(width);
            if (committed)
                persistDockWidth(); // once per gesture, not once per drag frame
        });
        m_dock->setOnPresetHeightChanged([this](int height, bool committed) {
            if (committed)
                persistBrushPresetHeight(height); // ... likewise: once, at the end of the drag
        });
        // A preset pick is NOT "an option changed" any more: the panel says which preset, and the
        // host resolves it ONCE (BrushPresetStore::select mints the tip's raster id -- a fresh id per
        // stroke would be a permanently cold dab cache) and seeds the bar's Size + Opacity from it.
        m_dock->presets()->setOnSelect([this](int index) {
            // The grid that produced the pick knows which corpus it was showing.
            applyPresetPick(m_dock->presets()->corpus(), index);
        });
        // §8.3's modal editor. Deliberately NOT a menu item: the menu tree is all-or-nothing
        // across 74 catalogs (docs/i18n.md), and a control beside the thing it edits is where a
        // user looks for it. The dialog owns the edit; the host only refreshes what it changed.
        m_dock->presets()->setOnEdit([this](int index) {
            ui::BrushEditorHost host;
            host.foreground = [this] { return m_colors.foreground(); };
            host.setEraserSizeFollowsBrush = [this](bool on) {
                m_tools.setEraserSizeTie(on);
                persistSetting([&](common::Settings& s) { s.eraserSizeFollowsBrush = on; },
                               "eraser size tie");
            };
            host.setEraserPresetFollowsBrush = [this](bool on) {
                persistSetting([&](common::Settings& s) { s.eraserPresetFollowsBrush = on; },
                               "eraser preset tie");
            };
            host.onSaved = [this](int saved) {
                // A save re-numbers the corpus from wherever it inserted: the panel drops its
                // index-keyed caches and re-derives the taxonomy, then the tool re-points at the
                // brush that was just written (select() resolves it ONCE, as always).
                if (m_dock != nullptr && m_dock->presets() != nullptr) {
                    const ui::PresetCorpus corpus = m_dock->presets()->corpus();
                    m_dock->presets()->refreshStore();
                    applyPresetPick(corpus, saved);
                    m_dock->presets()->setSelected(saved);
                }
            };
            // Feedback round 1 (§8.3 ①): the scratchpad reads the PEN. The same three seams the
            // Settings dialog's Tablet pane uses, and for the same reason -- tablet delivery is
            // per-window on both platforms, so a dialog that does not watch itself reads nothing.
            if (m_canvas != nullptr) {
                host.tabletReading = [this]() -> BrushEditorTabletSample {
                    BrushEditorTabletSample r;
                    if (m_canvas == nullptr)
                        return r;
                    TabletInput& t = m_canvas->tabletInput();
                    // Pump first: on X11 the CANVAS is what drains the ring, and the canvas is
                    // getting no events at all while this modal is up.
                    t.pumpReadout();
                    if (t.sampleRateHz() <= 0.0)
                        return r; // nothing arriving == the pen is away == a mouse
                    const core::brush::StrokeInput s = t.lastSample();
                    r.valid = true;
                    r.pressure = s.pressure;
                    r.xTilt = s.xTilt;
                    r.yTilt = s.yTilt;
                    r.rotation = s.rotation;
                    r.tangentialPressure = s.tangentialPressure;
                    return r;
                };
                host.tabletWatchWindow = [this](Fl_Window* win) {
                    if (m_canvas != nullptr)
                        m_canvas->tabletInput().watch(win);
                };
                host.tabletUnwatchWindow = [this](Fl_Window* win) {
                    if (m_canvas != nullptr)
                        m_canvas->tabletInput().unwatch(win);
                };
            }
            m_brushEditorDialog =
                std::make_unique<ui::BrushEditorDialog>(&m_brushPresets, std::move(host));
            if (!m_brushEditorDialog->seed(index)) {
                m_brushEditorDialog.reset();
                return;
            }
            const common::Settings s = common::loadSettings(m_settingsPath);
            m_brushEditorDialog->seedEraserTies(s.eraserSizeFollowsBrush,
                                                s.eraserPresetFollowsBrush);
            ui::centerWindowOver(*m_brushEditorDialog, this);
            m_brushEditorDialog->show();
        });
        // A History-tab jump (S16-b) is a multi-undo/redo: re-sync everything once, exactly
        // like the Edit-menu undo path. The history LIST itself refreshes via the command
        // stack's own observer (wired per document in presentDocument).
        m_layerPanel->history()->setOnJump([this] { syncAfterUndoRedo(); });
        // The open-document tabs (S49). Created hidden: with one document open the strip takes no
        // row at all, and applyDockWidth() reads tabStripHeight() to place the canvas under it.
        m_tabStrip = new TabStrip(kToolbarWidth, kMenuBarHeight + kOptionsBarHeight,
                                  canvasW, kTabStripHeight);
        m_tabStrip->setOnSelect([this](std::size_t i) { activateSession(i); });
        m_tabStrip->setOnClose([this](std::size_t i) { (void)closeSession(i); });
        // S50: a file dropped on the STRIP opens as a document; on the CANVAS it becomes a magic
        // layer. Two surfaces, two meanings -- the strip highlights so the difference is visible.
        m_tabStrip->setOnFilesDropped(
            [this](const std::vector<std::string>& p) { onTabStripFilesDropped(p); });
        m_tabStrip->hide();
        m_optionsBar = new ToolOptionsBar(0, kMenuBarHeight, W, kOptionsBarHeight, m_tools);
        initTextToolFonts(); // swap the Text tool's placeholder font list for the installed
                             // families
        initBrushPresets();  // ... and the Brush tool's placeholder preset list for the real library
        m_optionsBar
            ->setFontPreview( // the font picker draws each family in its own face (S29-c §8)
                [this](const std::string& fam, int w, int h) { return fontPreview(fam, w, h); });
        m_optionsBar->setFontHoverPreview([this](const std::string& fam) { // live preview on hover
            if (m_canvas == nullptr)
                return;
            if (fam.empty())
                m_canvas->clearStylePreview(); // cursor left the list / closed: revert
            else
                m_canvas->previewSelectionStyle(
                    [fam](core::text::CharStyle& s) { s.font.family = fam; });
        });
        m_statusBar = new StatusBar(0, H - kStatusBarHeight, W, kStatusBarHeight);
        m_canvas->setCursorCallback([this](double docX, double docY, bool overCanvas) {
            onCanvasCursor(docX, docY, overCanvas);
        });
        m_canvas->setViewChangedCallback([this] { onCanvasViewChanged(); });
        // Event-driven frame kicks (S15-b): canvas-affecting input fires the frame loop now
        // instead of waiting out the free-running heartbeat (up to 16.7 ms of pure latency).
        m_canvas->setFrameRequestCallback([this] { requestFrame(); });
        // The marquee/lasso tools (S14): the canvas runs the pointer gesture + live preview and
        // hands the final mask back here to land as one undoable SetSelectionCommand.
        m_canvas->setToolManager(&m_tools);
        // S50: files dropped on the canvas become magic layers. Refused while no document is open
        // (the canvas's idle state takes the drop as an Open instead -- its handle() intercepts).
        m_canvas->setFileDropHost([this](const std::vector<std::string>& p) {
            if (m_document)
                onCanvasFilesDropped(p);
        });
        m_canvas->setSelectionHost(
            {[this]() -> const core::Selection* {
                 return m_document ? &m_document->selection() : nullptr;
             },
             [this]() -> std::uint64_t {
                 return m_document ? m_document->selectionRevision() : 0;
             },
             [this](core::Selection sel, std::uint64_t coalesce, std::string_view label) {
                 commitToolSelection(std::move(sel), coalesce, label);
             }});
        // The Magic wand (S17): the canvas hands back the click point + boolean op; we own the seed
        // read, the source (active layer / merged), the flood, and land the single SetSelectionCommand.
        m_canvas->setMagicWandHost(
            {[this](common::Vec2 docPt, core::SelectOp op) { magicWandClick(docPt, op); }});
        // The Edge brush (L1): the canvas hands back the finished seed stroke; we own the source
        // (active layer / merged, the wand's Source pattern), the Reach/Edge Stop options and the
        // one edge-stopped geodesic grow. The canvas combines + commits what we return.
        m_canvas->setEdgeBrushHost(
            {[this](const core::Selection& seeds) { return edgeBrushGrow(seeds); }});
        // The Eye retouch (S38-b): the canvas hands back the scope the user painted; we own the
        // active layer, the document-selection clip, the colour math and the one undoable command.
        m_canvas->setRedEyeHost({[this](core::RedEyeMode mode, core::Selection scope,
                                        ui::RedEyeOptions options) {
            redEyeApply(mode, scope, options);
        }});
        // The Clone stamp (S38): the canvas owns the source anchor, the stroke and the deposit (it
        // rides the brush lane and lands through commitStroke like any other stroke); we owe it only
        // the two composited SAMPLE sources, which need the document and the compositor.
        m_canvas->setCloneStampHost(
            {[this](bool belowOnly) { return cloneSampleSnapshot(belowOnly); }, [this] {
                 transientStatus(
                     _("Ctrl-click to pick what the clone stamp copies, then paint over the target"));
             }});
        // Mesh Warp / Perspective Warp (S35-b, docs/warp-tools.md §5). The canvas owns the whole
        // gesture -- the lattice, the handles, the live preview bake and the overlay -- so it owes us
        // only what needs the document: which layer is active, one undoable command per Apply, a
        // recomposite when a preview bake lands, and a place to say the refusals out loud. The
        // canvas supplies the refusal WORDS: the reasons are the tool's own, and one generic
        // "cannot warp that" is exactly the message S36 replaced.
        m_canvas->setWarpToolHost(
            {[this]() { return m_document.get(); },
             [this]() -> core::LayerId {
                 return m_layerPanel ? m_layerPanel->activeLayer() : core::kInvalidLayerId;
             },
             [this](core::LayerId id, common::Image px, common::Affine2D placement,
                    core::WarpGrid grid) {
                 if (!m_document)
                     return;
                 // One Apply, one history entry (the command does not coalesce). Its dirtyRegion is
                 // deliberately nullopt -- a warp moves the layer's EXTENT and its placement -- so
                 // pushScopedPixelEdit falls through to the whole-document recomposite, which is the
                 // correct answer here rather than a missed optimisation.
                 pushScopedPixelEdit(std::make_unique<core::SetLayerWarpCommand>(
                     id, std::move(px), placement, std::move(grid)));
             },
             [this] { requestRecomposite(/*fitView=*/false); },
             [this](const std::string& why) { transientStatus(why); }});
        // The Bucket fill (S21): the canvas hands back the click point; we own the seed read, the
        // tolerance flood, the selection intersection and the single core::FillCommand.
        m_canvas->setBucketFillHost({[this](common::Vec2 docPt) { bucketFillClick(docPt); }});
        // The Eyedropper (S24): the canvas owns the pointer + the loupe; we own the pixels. `sample`
        // resolves the colour under a doc point (Source + sample-size average) for the loupe's live
        // readout; `commit` samples the same way and writes it into the fg/bg swatch; `previous`
        // reads the swatch a pick would replace, for the loupe's colour-comparison ring.
        //
        // ⚠ The two paths ask for DIFFERENT freshness, and the difference is the point. `sample` is
        // the live readout and runs once per frame for as long as the loupe is up, which
        // docs/s60-readback-consumers.md's standing rule puts squarely out of reach of a blocking
        // readback -- and it buys nothing anyway: during a hover nothing is changing, so a
        // stale-by-one-edit answer and a current one are the same pixels. `commit` writes the
        // picked colour into the fg/bg swatch, app state that outlives the gesture, and fires once
        // per press / per drag event -- "I painted that red, I picked it, and I got the colour it
        // used to be" is a defect the readout's one-frame lag is not.
        m_canvas->setEyedropperHost(
            {[this](common::Vec2 docPt) {
                 return eyedropperSample(docPt, render::Freshness::AnyRecent);
             },
             [this](common::Vec2 docPt, bool toBackground) {
                 if (const auto c = eyedropperSample(docPt, render::Freshness::Current)) {
                     if (toBackground)
                         m_colors.setBackground(*c);
                     else
                         m_colors.setForeground(*c);
                 }
             },
             [this](bool background) {
                 return background ? m_colors.background() : m_colors.foreground();
             }});
        // The Move tool (S15): the canvas hit-tests and runs the gesture; we land the results.
        m_canvas->setMoveToolHost(
            {[this]() { return m_document.get(); },
             [this](core::LayerId id) { // a canvas click-select mirrors into the panel; an
                                        // invalid id = a click on empty canvas deselected (S15-f),
                                        // so the panel drops its active row too
                 if (m_layerPanel)
                     m_layerPanel->setActive(id);
             },
             [this](const std::vector<std::pair<core::LayerId, common::Affine2D>>& xforms,
                    std::uint64_t coalesce) {
                 if (!m_document || xforms.empty())
                     return;
                 std::vector<core::SetTransformsCommand::Entry> entries;
                 entries.reserve(xforms.size());
                 for (const auto& [id, t] : xforms)
                     entries.push_back({id, t});
                 m_document->commands().push(
                     std::make_unique<core::SetTransformsCommand>(std::move(entries), coalesce));
                 driveTransformPreview(); // GPU-resident drag (S60-a) when it qualifies, else CPU
             },
             [this] { // drag ended: disarm the GPU pass, then land the final pixels once
                 if (m_gpuDragActive) {
                     m_canvas->endGpuDrag();
                     m_gpuDragActive = false;
                 }
                 m_gpuDragTried = false; // re-arm on the next gesture
                 // Name the gesture-END stall (docs/s60-gesture-start-stall.md finding G3): the
                 // full CPU walk + full-canvas upload this queues is ~1.5x the gesture-START cost
                 // (1712 ms quiet / 4672 ms loaded at 5000x8000) and it survived a whole benchmark
                 // pass because no profiler row was named after it. syncAfterEdit only QUEUES the
                 // work -- onFrame drains it -- so the flag carries the attribution to where the
                 // time is actually spent, and the row is recorded there.
                 m_gestureEndPending = true;
                 syncAfterEdit();
             },
             [this](
                 const std::vector<core::LayerId>& sel) { // move-selection set -> panel highlight
                 if (m_layerPanel)
                     m_layerPanel->setMoveSelection(sel);
             },
             [this] { // a transform was refused: name the reason, or the tool looks broken
                 transientStatus(_("This layer is locked. Unlock it to move or transform it."));
             }});
        // The Crop tool (S16): the canvas stages the rect and runs the gesture; we land the
        // crop as one undo step and mirror the staged size into the status bar.
        m_canvas->setCropToolHost(
            {[this]() { return m_document.get(); },
             [this](common::Rect r, double angle, common::Vec2 pivot) {
                 applyCrop(r, angle, pivot);
             },
             [this](const std::optional<common::Rect>& r) { onCropRectChanged(r); },
             // Rotation greys the Fill combo's Inpaint entry (§3.10 guardrail 3) and makes
             // the combo itself relevant (wedges need a fill).
             [this](double angle) {
                 refreshCropFillModes(angle != 0.0);
                 refreshCropFillVisibility();
             },
             [this] { return m_metric; },
             [this](double aspect, const std::vector<common::Rect>& protects,
                    const std::vector<common::Rect>& excludes) {
                 return smartCropSuggestion(aspect, protects, excludes);
             },
             [this]() -> std::vector<common::Rect> {
                 // The automatic keep-regions for the chips (same cached analysis).
                 if (!ensureSmartAnalysis())
                     return {};
                 return m_smartRegionRects;
             },
             // Smart Recompose (plan §1.3–§1.4): the offer gates the bar button; the review
             // trio drives the post-run preview (nudge re-assembles, apply/cancel resolve it).
             [this](bool on) { setRecomposeOffer(on); },
             [this](std::size_t index, common::Vec2 topLeft) { nudgeRecompose(index, topLeft); },
             [this] { applyRecompose(); }, [this] { cancelRecomposeReview(); }});
        // The Brush tool (S19-a): the canvas runs the stroke (core::brush engine + reticle) and the
        // active layer's pixels; we supply the document/colour and land the stroke as one undo
        // step.
        m_canvas->setBrushToolHost(
            {[this]() { return m_document.get(); },
             [this]() -> core::LayerId {
                 return m_layerPanel ? m_layerPanel->activeLayer() : core::kInvalidLayerId;
             },
             [this] { return m_colors.foreground(); },
             [this](common::Rect dirty, core::LayerId layer, common::Rect layerRect) {
                 // Live stroke preview: recomposite just the touched region + patch that rect of
                 // the canvas (S60-a). `layer`/`layerRect` are the SAME dab in the layer's own
                 // pixel space -- the space a device-resident upload copies out of -- and they ride
                 // this one callback precisely so the display claim and the upload claim cannot
                 // drift. A mask stroke names no layer (kInvalidLayerId): its pixels are not in the
                 // layer's image space, and the resident lane re-sends the layer on a mask step.
                 noteLayerPixelsChanged(layer, layerRect);
                 recompositeRegion(dirty);
             },
             [this](core::LayerId id, common::Image region, long ox, long oy) {
                 if (!m_document)
                     return;
                 // Store + recomposite only the stroke's bounding box (S60-a): the command keeps
                 // just the region, and the scoped recomposite avoids the whole-document composite
                 // that made every stroke END hiccup on a big canvas.
                 pushScopedPixelEdit(
                     std::make_unique<core::SetLayerPixelsCommand>(id, std::move(region), ox, oy));
             },
             [this] { transientStatus(_("The layer is locked — unlock it to paint")); },
             [this] {
                 transientStatus(
                     _("Can’t brush a vector layer — rasterize it first to paint on it"));
             },
             [this](core::LayerId id, core::Selection hole) { runInpaint(id, std::move(hole)); },
             [this]() { return m_brushPresets.activeParams(); },
             [this]() { return m_brushPresets.activeEraserParams(); },
             // S31 mask painting: the dock's aim (click the mask thumbnail) redirects the Brush
             // and Eraser to the active layer's mask; the stroke lands as one SetMaskPixelsCommand
             // through the same scoped-recomposite path as a pixel stroke.
             [this]() { return m_layerPanel != nullptr && m_layerPanel->maskEditTarget(); },
             [this](core::LayerId id, std::vector<std::uint8_t> region, std::uint32_t w,
                    std::uint32_t h, long ox, long oy) {
                 if (!m_document)
                     return;
                 pushScopedPixelEdit(std::make_unique<core::SetMaskPixelsCommand>(
                     id, std::move(region), w, h, ox, oy));
             },
             // The stroke ran off the layer's own pixel grid, so the grid GREW to take it (bounded
             // by the canvas -- core/layer_grow.hpp). Growth and paint are ONE undo step; the
             // command reports no region (the layer's extent and placement both moved), so this
             // falls back to a full recomposite, which is what changing a layer's size needs.
             [this](core::LayerId id, std::uint32_t newW, std::uint32_t newH, long offX, long offY,
                    common::Image region, long ox, long oy) {
                 if (!m_document)
                     return;
                 pushScopedPixelEdit(std::make_unique<core::GrowAndPaintLayerCommand>(
                     id, newW, newH, offX, offY, std::move(region), ox, oy));
             }});
        // The Shape tool (S26): the canvas runs the drag and draws its wireframe outline; we supply
        // the document and the active colours, and on RELEASE spawn + land the authored shape as a
        // new VectorLayer (S26-c -- nothing reaches the document before the pointer comes up).
        const auto toColorF = [](common::Color8 c) -> common::ColorF {
            return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
        };
        m_canvas->setShapeToolHost(
            {[this]() { return m_document.get(); },
             [this, toColorF]() { return toColorF(m_colors.foreground()); },
             [this, toColorF]() { return toColorF(m_colors.background()); },
             [this](const ui::ShapeDraft& draft) { spawnShape(draft); },
             [this](const std::string& name) { commitShape(name); }, [this] { cancelShape(); },
             [this](core::LayerId id, core::vec::Object obj,
                    std::optional<common::Affine2D> placement, std::uint64_t coalesce) {
                 if (!m_document)
                     return;
                 m_document->commands().push(std::make_unique<core::SetVectorObjectCommand>(
                     id, std::move(obj), _("Edit Shape"), coalesce, placement));
                 // A coalesced merge doesn't fire the command-stack onChange, and that hook only
                 // refreshes history anyway -- so recomposite explicitly or live edits wouldn't
                 // show.
                 requestRecomposite(/*fitView=*/false);
             },
             [this](core::LayerId id, common::Affine2D xform, std::uint64_t coalesce) {
                 if (!m_document)
                     return;
                 m_document->commands().push(
                     std::make_unique<core::SetTransformCommand>(id, xform, coalesce));
                 requestRecomposite(/*fitView=*/false); // coalesced merge: recomposite explicitly
             }});

        // The Pen tool (S28): the canvas owns the whole pointer side (authoring clicks + handle
        // drags, and the node editor on a committed path); we supply the document + colours and
        // land the results. A finished path is a ui::ShapeDraft, so it goes through the SAME
        // spawn/commit pair the Shape tool uses -- one AddLayerCommand, one undo step -- and its
        // node edits ride SetVectorObjectCommand exactly as a shape's parameter edits do.
        m_canvas->setPenToolHost(
            {[this]() { return m_document.get(); },
             [this, toColorF]() { return toColorF(m_colors.foreground()); },
             [this, toColorF]() { return toColorF(m_colors.background()); },
             [this](const ui::ShapeDraft& draft) { spawnShape(draft); },
             [this](const std::string& name) { commitShape(name); }, [this] { cancelShape(); },
             [this](core::LayerId id, core::vec::Object obj,
                    std::optional<common::Affine2D> placement, std::uint64_t coalesce) {
                 if (!m_document)
                     return;
                 m_document->commands().push(std::make_unique<core::SetVectorObjectCommand>(
                     id, std::move(obj), _("Edit Path"), coalesce, placement));
                 // A coalesced merge doesn't fire the command-stack onChange, so recomposite
                 // explicitly or a live node drag would not show (the editShape reasoning).
                 requestRecomposite(/*fitView=*/false);
             }});

        // S22 Gradient tool host: the working ramp + the document side (live preview layer, commit,
        // live handle edits). The gradient lands as a full-bleed, editable, maskable VectorLayer.
        m_canvas->setGradientToolHost(
            {[this]() { return m_document.get(); },
             [this]() { return workingGradient(); },
             [this](const ui::GradientDraft& draft, double opacity) {
                 previewGradient(draft, opacity);
             },
             [this] { commitGradient(); }, [this] { cancelGradient(); },
             [this](core::LayerId id, core::vec::Object obj, std::uint64_t coalesce) {
                 if (!m_document)
                     return;
                 m_document->commands().push(std::make_unique<core::SetVectorObjectCommand>(
                     id, std::move(obj), _("Edit Gradient"), coalesce));
                 requestRecomposite(/*fitView=*/false); // coalesced merge: recomposite explicitly
             }});

        // S29-b Type tool host: the font stack + document side of on-canvas text editing.
        m_canvas->setTypeToolHost(
            {[this]() { return m_document.get(); },
             [this]() {
                 return textDefaultStyle();
             }, // new/typed run style: bar controls + fg swatch
             [this](const core::text::TextBlock& b) { return m_textShaper.layout(b, m_fontDb); },
             [this](core::text::TextBlock b, common::Affine2D placement) {
                 return createTextLayer(std::move(b), placement);
             },
             [this](core::LayerId id, core::text::TextBlock b, std::uint64_t coalesce) {
                 if (!m_document)
                     return;
                 m_document->commands().push(std::make_unique<core::SetTextCommand>(
                     id, std::move(b), _("Edit Text"), coalesce));
                 // The live canvas re-raster runs once per frame in the frame loop's
                 // ensureTextCaches(); do NOT also re-raster + re-render the layer thumbnail per
                 // event here (rev 11). Mark the thumbnail dirty and let onFrame() refresh it once
                 // the edit/drag settles -- so a continuous size/style drag isn't re-shaping the
                 // whole block + its thumbnail per tick.
                 markTextThumbDirty();
                 // ⚠ A REGION recomposite, not a full one (S60-b): a keystroke re-renders one text
                 // layer's cache, and the refresh reports exactly the band it changed -- paying the
                 // ~64 ms full-document composite per keystroke is what made typing on a 1920x1080
                 // document "extremely laggy" (user 2026-07-14).
                 requestTextRecomposite();
             },
             [this](core::LayerId id) { finishTextLayer(id); },
             [this]() { return m_reuseLastTextBox; },
             [this](core::LayerId id) { // select-to-edit moves the panel's active row (fixlist #4)
                 if (m_layerPanel != nullptr && id != core::kInvalidLayerId)
                     m_layerPanel->setActive(id);
             },
             [this]() {
                 // Re-clip on edit enter/leave (#3) -- through the REGION path (S60-b): the clip
                 // flip re-renders the entered/left layer's cache and the refresh reports exactly
                 // the band that changed. Paying a full composite here is what made CLICKING
                 // BETWEEN type layers laggy on a big document (user 2026-07-14: enter+leave was
                 // two full composites back to back).
                 requestTextRecomposite();
             },
             [this](core::LayerId id, common::Affine2D xform, std::uint64_t coalesce) {
                 // Move / rotate from the Type-edit box: write the layer transform, one undo step
                 // per gesture (the resize handle goes through editText instead). Mirrors
                 // transformShape.
                 if (!m_document)
                     return;
                 m_document->commands().push(
                     std::make_unique<core::SetTransformCommand>(id, xform, coalesce));
                 requestRecomposite(/*fitView=*/false); // coalesced merge: recomposite explicitly
             },
             [this]() { // selection/style changed: re-sync the bar + Type panel (S29-c)
                 reflectTextOptions();
                 if (m_canvas != nullptr && m_canvas->textEditTarget() == core::kInvalidLayerId) {
                     closeTypePanel(); // the text session ended -> dismiss the panels
                     closeType3dPanel();
                 }
             },
             [this](core::LayerId id, core::text::TextBlock b) { // font-hover preview: display only
                 if (!m_document)
                     return;
                 core::Layer* l = m_document->find(id);
                 auto* tl = l != nullptr ? l->as<core::TextLayer>() : nullptr;
                 if (tl == nullptr)
                     return;
                 tl->setBlock(std::move(b)); // NO command pushed -- transient preview
                 markTextThumbDirty(); // re-raster + thumbnail deferred to the settle (rev 11)
                 // A hover preview renders DRAFT (half-res): scrubbing the list re-rasters the
                 // whole block per row, and full quality per row is what made it lag. The settle
                 // in onFrame() lands the crisp pass ~0.18 s after the pointer rests.
                 m_textHoverDraft = true;
                 m_textHoverDraftAt = nowSeconds();
                 requestTextRecomposite(); // the same region path as typing (S60-b)
             },
             // Spell-check (deferred §2): forward to the background worker the host owns. The learn
             // /
             // ignore actions also kick a rescan so the corrected word's squiggle clears at once.
             [this](const std::string& word, const std::string& lang) {
                 return m_spellWorker ? m_spellWorker->suggest(word, lang)
                                      : std::vector<std::string>{};
             },
             [this](const std::string& word, const std::string& lang) {
                 if (m_spellWorker) {
                     m_spellWorker->addToUserDict(word, lang);
                     forceSpellRescan();
                 }
             },
             [this](const std::string& word) {
                 if (m_spellWorker) {
                     m_spellWorker->ignore(word);
                     forceSpellRescan();
                 }
             },
             [this] { return spellAppLanguage(); }});

        // S33/S35 filter-centre gizmo host (docs/blur-filters.md §6): the canvas owns the chrome +
        // pointer side; this side owns the layer lookup (provider) and the params-bag command
        // (edit). Geometry only -- amounts stay in the popover.
        m_canvas->setDofGizmoProvider([this](VulkanCanvas::DofGizmoState& out) {
            if (!m_document || m_layerPanel == nullptr)
                return false;
            core::Layer* l = m_document->find(m_layerPanel->activeLayer());
            auto* adj = l != nullptr ? l->as<core::AdjustmentLayer>() : nullptr;
            if (adj == nullptr)
                return false;
            const core::AdjustmentKind kind = adj->adjustmentKind();
            const std::map<std::string, double>& p = adj->params();
            const auto get = [&p](const char* key, double fallback) {
                const auto it = p.find(key);
                return it != p.end() ? it->second : fallback;
            };
            const bool isDof = kind == core::AdjustmentKind::DofBlur;
            switch (kind) {
            case core::AdjustmentKind::DofBlur:
                out.kind = VulkanCanvas::BlurGizmoKind::Band;
                break;
            case core::AdjustmentKind::RadialBlur:
                out.kind = VulkanCanvas::BlurGizmoKind::Crosshair;
                break;
            case core::AdjustmentKind::Vignette:
                out.kind = VulkanCanvas::BlurGizmoKind::Ring; // centre + radius (S35)
                break;
            case core::AdjustmentKind::Wave:
                // Wave's centre is read by the RIPPLE mode only -- plain Wave is a directional
                // displacement with no origin. Drawing a handle there would offer a drag that
                // changes nothing on screen, so the gizmo simply does not appear until the mode
                // says it means something.
                if (static_cast<int>(std::lround(get("mode", 0.0))) !=
                    static_cast<int>(core::WaveMode::Ripple))
                    return false;
                out.kind = VulkanCanvas::BlurGizmoKind::Crosshair;
                break;
            default:
                return false; // every other kind is centreless: no canvas gizmo
            }
            out.center = {get("center_x", 0.0), get("center_y", 0.0)};
            if (isDof) { // the band geometry -- the crosshair/ring kinds carry no angle or band
                out.angleDeg = get("angle", 0.0);
                out.band = get("band", 0.0);
                out.feather = get("feather", 1.0);
            }
            if (out.kind == VulkanCanvas::BlurGizmoKind::Ring)
                out.radius = get("radius", 0.0);
            // The adjustment's parent space -> document: the ancestor groups' transforms,
            // innermost applied first -- the same chain the compositor walk composes, so a blur
            // nested in a transformed group keeps honest handles.
            common::Affine2D toDoc = common::Affine2D::identity();
            for (const core::GroupLayer* g = l->parent(); g != nullptr; g = g->parent())
                toDoc = g->transform() * toDoc;
            out.parentToDoc = toDoc;
            return true;
        });
        m_canvas->setDofGizmoEdit(
            [this](const char* coalesceId, const VulkanCanvas::DofGizmoState& s) {
                if (m_layerPanel == nullptr || !m_document)
                    return;
                // The kind comes from the LAYER, never from the gizmo shape: Crosshair is now worn
                // by two different adjustments (RadialBlur and Wave-in-Ripple) and Band/Ring by one
                // each, so inferring one from the other -- as the first cut did -- would clamp a
                // Wave against the RadialBlur schema.
                const core::LayerId id = m_layerPanel->activeLayer();
                core::Layer* l = m_document->find(id);
                const auto* adj = l != nullptr ? l->as<core::AdjustmentLayer>() : nullptr;
                if (adj == nullptr)
                    return;
                const core::AdjustmentKind kind = adj->adjustmentKind();
                const bool band = s.kind == VulkanCanvas::BlurGizmoKind::Band;
                const bool ring = s.kind == VulkanCanvas::BlurGizmoKind::Ring;
                applyAdjustmentFieldOn(
                    id, coalesceId, [&s, band, ring, kind](std::map<std::string, double>& p) {
                        p["center_x"] = s.center.x;
                        p["center_y"] = s.center.y;
                        if (band) { // the crosshair / ring own no angle/band/feather keys
                            p["angle"] = s.angleDeg;
                            p["band"] = s.band;
                            p["feather"] = s.feather;
                        }
                        if (ring) // the Vignette's falloff extent -- geometry, not an amount
                            p["radius"] = s.radius;
                        // Clamp to the schema's ranges: an unbounded canvas drag must not write
                        // values the popover sliders and the docio caps could never produce.
                        for (const core::AdjustmentParamDesc& d :
                             core::adjustmentParamSchema(kind)) {
                            const auto it = p.find(d.key);
                            if (it != p.end())
                                it->second = std::clamp(it->second, d.min, d.max);
                        }
                    });
            });
        // A colour-swatch change recolours the shape currently selected for editing, live (§7.1).
        m_colors.addObserver([this] {
            if (m_canvas) {
                m_canvas->onShapeColorEdited();
                m_canvas->onPenColorEdited(); // ... and a bound pen path (S28)
                onTextColorEdited(); // recolour the selected text run, live (S29-c)
            }
        });
        m_tools.setOnChange([this] { onToolChanged(); });
        m_tools.setOnOptionsChanged([this] {
            // (A preset pick no longer arrives here: it is not "an option changed" but a selection in
            // the dock's grid, which calls applyBrushPreset directly -- §8.2.)
            // One preference, three bars: whichever the user just dragged wins, and the other
            // brush-family tools follow. Persisted on a DELAY -- the slider fires its callback on
            // every drag frame, and writing the settings file sixty times a second to record a value
            // the user has not finished choosing is not persistence, it is thrashing.
            if (const int on = m_tools.syncBrushSmoothing(); on >= 0) {
                m_pendingBrushSmoothing = on != 0;
                Fl::remove_timeout(persistBrushSmoothingCb, this);
                Fl::add_timeout(0.5, persistBrushSmoothingCb, this);
            }
            // If the Ratio combo just toggled Custom on/off, the bar needs a *rebuild* (to add or
            // drop the ratioW/ratioH fields), not a value re-sync -- and it must be deferred, since
            // we're inside the changed control's own callback (see refreshCropCustomFields).
            // The Smart Resize toggle shows/hides the "Recompose" button the same deferred way.
            const bool rebuilding = refreshCropCustomFields();
            const bool rebuildingRecompose = refreshRecomposeButton();
            if (!rebuilding && !rebuildingRecompose && m_optionsBar)
                m_optionsBar
                    ->syncValues(); // keep surfaces in lockstep (twin Properties tab: S11-c)
            if (m_canvas)
                m_canvas->cropOptionsChanged(); // a ratio change re-conforms the staged rect
            if (m_canvas)
                m_canvas->warpOptionsChanged(); // ... and Rows/Columns re-stage the lattice (S35-b)
            // Only the Move tool's Anti-aliasing choice changes the rendered raster, so recomposite
            // ONLY for it. Every other tool option (brush size/hardness, crop ratio, ...) leaves
            // the composite untouched -- recompositing on those dragged the whole document through
            // the CPU compositor per slider tick, which on a big canvas froze the UI (S60-a
            // follow-up).
            if (m_tools.active() == ToolId::Move)
                requestRecomposite(/*fitView=*/false);
            if (m_canvas)
                m_canvas
                    ->onShapeOptionsEdited(); // a selected shape edits live (§7.1; no-op otherwise)
            if (m_canvas)
                m_canvas->onPenOptionsEdited(); // ... and a bound pen path re-paints live (S28)
            if (m_tools.active() == ToolId::Text)
                applyTextOptionEdits(); // a bar control edits the selected text live (S29-c)
            if (m_tools.active() == ToolId::Gradient)
                applyGradientOptionEdits(); // Type retypes / Opacity re-fades the bound layer (S22)
        });
        m_tools.setOnAction([this](const std::string& id) { // options-bar momentary buttons
            if (m_canvas == nullptr)
                return;
            // During a Recompose review, Apply/Cancel resolve the REVIEW (the crop's staged
            // rect is dormant beneath it until the review is dropped).
            // "apply" / "cancel" are SHARED option ids: the Crop tool and the two warp tools both
            // publish them, so the ACTIVE TOOL decides which staged thing the button resolves. Tested
            // first because a warp session and a staged crop can both be alive at once (S35-b).
            const bool warping = m_tools.active() == ToolId::MeshWarp ||
                                 m_tools.active() == ToolId::PerspectiveWarp;
            if (id == "apply") {
                if (warping)
                    m_canvas->commitWarp();
                else if (m_recomposeReview)
                    applyRecompose();
                else
                    m_canvas->commitCrop();
            } else if (id == "cancel") {
                if (warping)
                    m_canvas->cancelWarp();
                else if (m_recomposeReview)
                    cancelRecomposeReview();
                else
                    m_canvas->cancelCrop();
            } else if (id == "recompose")
                startRecompose();
            else if (id == "designer")
                openShapeDesigner();
            else if (id == "typePanel")
                openTypePanel();
            else if (id == "type3d")
                openType3dPanel();
            else if (id == "stops")
                openGradientStopsFlyout(); // S22: edit the gradient's stops / blend curves / spread
        });
        // ui::MenuBar (an Fl_Menu_Bar subclass): themed flat pop-up menus via its play_menu
        // override (their sub-windows are created in its ctor here, before show()). Stored as
        // Fl_Menu_Bar*; play_menu is virtual, so the themed pop-up is used polymorphically.
        m_menu = new MenuBar(0, 0, W, kMenuBarHeight);
        m_menu->box(MOSAIC_FLAT_BOX);
        m_menu->color(toFl(pal.windowBg));
        m_menu->textcolor(toFl(pal.text));
        m_menu->selection_color(toFl(pal.accent));
        // The tool letters are the keymap's, but they are SEEDED from the tool registry
        // (ui/tool.cpp's kToolDefs, walked here through ToolManager) rather than restated in
        // keymap.cpp -- so a tool added there arrives with its letter already remappable, and
        // ui::toolForShortcut stays exactly what it was. iconKeyFor() supplies the stable,
        // untranslated per-tool key the action id is built from; the tool's own name() is the label.
        // Variants sharing a slot letter collapse to one action, which is toolForShortcut's rule.
        for (const std::unique_ptr<Tool>& tool : m_tools.tools()) {
            if (tool->shortcut().empty())
                continue;
            if (!m_keymap.registerTool("tool." + std::string(iconKeyFor(tool->id())), tool->name(),
                                       tool->id(), tool->shortcut()[0])) {
                uiLog().warn("keymap: tool '{}' wants key '{}', which a non-tool action already "
                             "holds -- it gets no remappable binding",
                             tool->name(), tool->shortcut());
            }
        }
        buildMenu(m_menu, m_canvas, this, m_keymap);
        // A remap re-applies through applyKeymapToMenu(), never by rebuilding the item array: a
        // rebuild would throw away the dynamic state the array carries (the Open Recent rows, the
        // check marks, the greyed items, the two mask rows' rewritten labels).
        m_keymap.setOnChanged([this] { applyKeymapToMenu(); });
        // Badges ALWAYS shown on "Layer ▸ Layer Effects…" (the "fx" glyph) and "Layer ▸ Texture
        // Generator…" (the mini checkerboard) -- matching the Layers-panel chips: they augment the
        // labels as wayfinding ("this is where these live"), regardless of the active layer's
        // state. Each "Arrange ▸ Align …" item likewise wears its direction pictogram, so the six
        // options read at a glance instead of by label. Items identified by their callbacks.
        m_menu->setItemBadge([](const Fl_Menu_Item* it) {
            if (it == nullptr)
                return MenuBar::ItemBadge::None;
            if (it->callback() == cbLayerEffects)
                return MenuBar::ItemBadge::Fx;
            if (it->callback() == cbTextureGenerator)
                return MenuBar::ItemBadge::Texture;
            if (it->callback() == cbAlign<core::AlignEdge::Left>)
                return MenuBar::ItemBadge::AlignLeft;
            if (it->callback() == cbAlign<core::AlignEdge::HCenter>)
                return MenuBar::ItemBadge::AlignHCenter;
            if (it->callback() == cbAlign<core::AlignEdge::Right>)
                return MenuBar::ItemBadge::AlignRight;
            if (it->callback() == cbAlign<core::AlignEdge::Top>)
                return MenuBar::ItemBadge::AlignTop;
            if (it->callback() == cbAlign<core::AlignEdge::VMiddle>)
                return MenuBar::ItemBadge::AlignVMiddle;
            if (it->callback() == cbAlign<core::AlignEdge::Bottom>)
                return MenuBar::ItemBadge::AlignBottom;
            // Image (S53): the two size dialogs, the four orientations and Trim each wear their
            // own pictogram, for the same reason the Align set does -- "Rotate 90 CW" vs "90 CCW"
            // and "Flip Horizontal" vs "Vertical" are exactly the pairs that are read wrong at
            // speed, and a picture of the result settles them instantly.
            if (it->callback() == cbImageSize)
                return MenuBar::ItemBadge::ImageSize;
            if (it->callback() == cbCanvasSize)
                return MenuBar::ItemBadge::CanvasSize;
            if (it->callback() == cbOrient<render::DocOrient::Rotate90CW>)
                return MenuBar::ItemBadge::Rotate90CW;
            if (it->callback() == cbOrient<render::DocOrient::Rotate90CCW>)
                return MenuBar::ItemBadge::Rotate90CCW;
            if (it->callback() == cbOrient<render::DocOrient::Rotate180>)
                return MenuBar::ItemBadge::Rotate180;
            if (it->callback() == cbOrient<render::DocOrient::FlipHorizontal>)
                return MenuBar::ItemBadge::FlipH;
            if (it->callback() == cbOrient<render::DocOrient::FlipVertical>)
                return MenuBar::ItemBadge::FlipV;
            if (it->callback() == cbTrimToContent)
                return MenuBar::ItemBadge::TrimContent;
            // Layer ▸ Combine Paths: the four boolean Venn glyphs. The words "Subtract" and
            // "Exclude" are the ones nobody can keep apart without the picture.
            if (it->callback() == cbCombinePaths<core::vec::BoolOp::Union>)
                return MenuBar::ItemBadge::BoolUnion;
            if (it->callback() == cbCombinePaths<core::vec::BoolOp::Subtract>)
                return MenuBar::ItemBadge::BoolSubtract;
            if (it->callback() == cbCombinePaths<core::vec::BoolOp::Intersect>)
                return MenuBar::ItemBadge::BoolIntersect;
            if (it->callback() == cbCombinePaths<core::vec::BoolOp::Exclude>)
                return MenuBar::ItemBadge::BoolExclude;
            // Type ▸ Orientation: the two vertical writing modes differ only in which way the
            // COLUMNS advance, which no label makes obvious.
            if (it->callback() == cbTypeWritingMode<core::text::WritingMode::HorizontalTB>)
                return MenuBar::ItemBadge::TypeHorizontal;
            if (it->callback() == cbTypeWritingMode<core::text::WritingMode::VerticalRL> ||
                it->callback() == cbTypeWritingMode<core::text::WritingMode::VerticalLR>)
                return MenuBar::ItemBadge::TypeVertical;
            return MenuBar::ItemBadge::None;
        });
        // S53 shortcut audit: menu accelerators must not fire behind a live text caret. FLTK
        // dispatches item accelerators GLOBALLY -- the focus widget gets FL_KEYBOARD first, but the
        // moment it declines a chord the same keystroke comes back as FL_SHORTCUT and the menu
        // fires it behind the caret. Fl_Input_ declines every Ctrl/Alt combination it has no
        // binding for, and VulkanCanvas::onTextKey deliberately passes modified chords through, so
        // Ctrl+[ / Ctrl+] / Ctrl+F / Ctrl+Alt+I ... were all reordering, filtering and re-framing
        // the document mid-sentence. The bar refuses exactly those (menu_bar.hpp's guarded list)
        // while a text editor owns the keyboard; Ctrl+S / Ctrl+Z / Ctrl+W stay global.
        m_menu->setTextEditorActive([this] {
            if (m_canvas != nullptr && m_canvas->textEditTarget() != core::kInvalidLayerId)
                return true; // the Type tool's caret is live on the canvas
            // Any themed text field: the Layers panel's inline rename editor, a panel/dialog's
            // number field, the colour picker's hex box -- they all derive from Fl_Input_.
            return dynamic_cast<const Fl_Input_*>(Fl::focus()) != nullptr;
        });
#ifdef __APPLE__
        // The application menu -- "About Mosaic" and "Settings…" (⌘,), which macOS puts under the
        // app's own name rather than in Help and Edit (buildMenu leaves them out there).
        installMacApplicationMenu(_("Settings…"), cbAbout, cbSettings, this);
        // buildMenu ran before the badge predicate existed, so the system menu was mirrored without
        // badges. This is the one rebuild that attaches them; every later one (a visibility flip, a
        // recent-files edit) goes through MenuBar::update() and re-attaches them itself.
        m_menu->update();
#else
        // Settings -> Annoyances "Cheesy motivational one-liners" (docs/motivational-ticker.md):
        // every few minutes a fresh all-caps one-liner slides down into the menu bar's empty right
        // region, holds ~10 s, then slides up and out (the row is empty between lines). The bar draws
        // it itself (MenuBar::showTickerLine) -- no separate widget, no overlap, no placement to
        // track. MotivationTicker drives only the line pick + cadence (the reworked home of the
        // deleted GPU backdrop driver).
        //
        // Not on macOS: the menus are in the system menu bar there, so the empty right region the
        // ticker rides in does not exist -- and the Settings toggle that drives it is dropped from
        // the Annoyances pane to match (settings_dialog.cpp), rather than left promising a thing
        // that cannot appear.
        m_ticker = std::make_unique<MotivationTicker>(m_menu);
#endif

        // The colour picker is a child sub-window (see ui::Popover): built here, before show(),
        // which is what makes FLTK realize it as a genuine sub-surface instead of promoting it to a
        // stray top-level (which would get a taskbar entry, be centred on Wayland, and orphan the
        // app on close). It stays hidden until the swatch toggles it, and is created last so it
        // sits above the canvas sub-window in the child order. Owned by this window as a child
        // widget.
        m_colorPicker = new ColorPicker(m_colors);
        m_colorPicker->hide();
        // The picker's speech-bubble balances its left gap (to the toolbar) against its bottom gap
        // (to the status bar), so it needs those two layout references.
        m_colorPicker->setBubbleInsets(kToolbarWidth, kStatusBarHeight);
        // The toolbar slot flyout (S11-e) is a child sub-window on the same rules as the picker,
        // and gets the same comic-book pointer. statusBarH = 0: it tracks the anchor slot
        // vertically (mid-toolbar) rather than balancing against the status bar like the
        // bottom-pinned picker.
        m_toolFlyout = new ToolFlyout(m_tools);
        m_toolFlyout->hide();
        m_toolFlyout->setBubbleInsets(kToolbarWidth, 0);
        // The options-bar overflow list (S16-n): the controls that don't fit on a narrow bar move
        // into this child sub-window, opened by the bar's chevron. Built before show, like the
        // others.
        m_optionsOverflow = new OptionsOverflowPopover();
        m_optionsOverflow->hide();
        // The left toolbar's overflow list (S16-o): the tool slots that don't fit a short column
        // move into this child sub-window, opened by the toolbar's bottom chevron. Same
        // build-before-show rule.
        m_toolbarOverflow = new ToolbarOverflowPopover(m_tools);
        m_toolbarOverflow->hide();
        // Its comic-book pointer tracks the overflow chevron vertically (statusBarH = 0), like the
        // tool flyout, rather than balancing against the status bar.
        m_toolbarOverflow->setBubbleInsets(kToolbarWidth, 0);
        // The shape-designer popover (S26-b §7.4): opened by the Shape options bar's "Edit shape…"
        // button; the "everything" surface for the selected shape. Built before show, like the
        // others.
        m_shapeDesigner = new ShapeDesigner();
        m_shapeDesigner->hide();
        m_shapeDesigner->setOnEdit([this](core::vec::Object obj) {
            if (!m_document || !m_canvas)
                return;
            const core::LayerId id = m_canvas->shapeEditTarget();
            if (id == core::kInvalidLayerId)
                return;
            m_document->commands().push(std::make_unique<core::SetVectorObjectCommand>(
                id, std::move(obj), _("Edit Shape"), m_shapeDesignerCoalesce));
            requestRecomposite(/*fitView=*/false); // coalesced merge: recomposite explicitly
            m_canvas->reflectActiveShape();        // keep the basic bar's mirrored params in step
        });
        // The Type panel popover (S29-c §8): the bottom-right "everything" surface for the edited
        // text. A child sub-window on the same build-before-show rule, created before the shared
        // Dropdown list so that list stacks above it (the panel hosts the font picker). Wired to
        // the same selection funnel + font-preview the context bar uses.
        m_typePanel = new TypePanel();
        m_typePanel->hide();
        m_typePanel->setFontFamilies(m_fontDb.families());
        m_typePanel->setFontPreview(
            [this](const std::string& fam, int w, int h) { return fontPreview(fam, w, h); });
        m_typePanel->setFontHoverPreview([this](const std::string& fam) {
            if (m_canvas == nullptr)
                return;
            if (fam.empty())
                m_canvas->clearStylePreview();
            else
                m_canvas->previewSelectionStyle(
                    [fam](core::text::CharStyle& s) { s.font.family = fam; });
        });
        m_typePanel->setOnStyleEdit(
            [this](const std::string& id, std::function<void(core::text::CharStyle&)> mut) {
                applyTextStyleField(id, std::move(mut));
            });
        m_typePanel->setOnParagraphEdit(
            [this](const std::string& id, std::function<void(core::text::Paragraph&)> mut) {
                applyTextParagraphField(id, std::move(mut));
            });
        m_typePanel->setOnBlockEdit(
            [this](const std::string& id, std::function<void(core::text::TextBlock&)> mut) {
                applyTextBlockField(id, std::move(mut));
            });
        // The 3D popup (S30-d, docs/type-tool.md §8.4): the live viewport + gizmos. Edits stream
        // through the SAME block funnel, so the canvas is the real preview and undo composes.
        m_type3dPanel = new Type3dPanel();
        m_type3dPanel->hide();
        m_type3dPanel->setOnBlockEdit(
            [this](const std::string& id, std::function<void(core::text::TextBlock&)> mut) {
                applyTextBlockField(id, std::move(mut));
            });
        // The viewport preview: the CURRENT block with the panel's params, through the full
        // renderTextF pipeline (mesh, camera, lighting, the GPU lane) fitted into the widget.
        m_type3dPanel->setPreviewRenderer(
            [this](const core::text::Extrude& e, int w, int h, double* fitScale) -> common::Image {
                if (m_canvas == nullptr || w <= 0 || h <= 0)
                    return {};
                const core::text::TextBlock* b = m_canvas->textEditBlockForUi();
                if (b == nullptr || b->utf8.empty())
                    return {};
                core::text::TextBlock copy = *b;
                copy.extrude = e;
                const auto nat = core::text::layoutBounds(m_textShaper, copy, m_fontDb);
                if (!nat || nat->empty())
                    return {};
                const common::Rect all = nat->united(core::text::projectedExtrudeBounds(*nat, e));
                if (all.empty())
                    return {};
                const double sc = std::min({(w - 6.0) / all.w, (h - 6.0) / all.h, 4.0});
                if (sc <= 0.0)
                    return {};
                if (fitScale != nullptr)
                    *fitScale = sc; // the depth handle's gain reference
                const common::Affine2D toPixel =
                    common::Affine2D::translation((w - all.w * sc) * 0.5 - all.x * sc,
                                                  (h - all.h * sc) * 0.5 - all.y * sc) *
                    common::Affine2D::scaling(sc, sc);
                // The edited layer's reflect-canvas snapshot, so the popup viewport mirrors too,
                // and its Layer Effects, so the §12 per-face overlays show in the viewport
                // exactly as on the canvas (S30-e).
                core::text::ExtrudeEnv env;
                const core::text::ExtrudeEnv* envPtr = nullptr;
                const core::LayerEffects* fx = nullptr;
                if (m_document != nullptr) {
                    if (core::Layer* l = m_document->find(m_canvas->textEditTarget()))
                        if (auto* tl = l->as<core::TextLayer>(); tl != nullptr) {
                            if (e.reflectCanvas && tl->reflectionEnv() != nullptr) {
                                env.image = tl->reflectionEnv();
                                env.layerToEnv = tl->reflectionEnvTransform();
                                envPtr = &env;
                            }
                            if (tl->hasEffects())
                                fx = &tl->effects();
                        }
                }
                return core::text::renderText(m_textShaper, copy, m_fontDb,
                                              static_cast<std::uint32_t>(w),
                                              static_cast<std::uint32_t>(h), toPixel, 0.25, envPtr,
                                              fx);
            });
        // Corner placement within the canvas area, flipping away from the edited text object. Both
        // providers map canvas-widget-local logical px into main-window coords (the popover's
        // space). Shared by the Type panel AND the 3D popup -- the two are placed identically (and
        // are mutually exclusive, so they never fight over the corner).
        const std::function<common::Rect()> panelRegion = [this]() -> common::Rect {
            if (m_canvas == nullptr)
                return {};
            int ox = 0;
            int oy = 0;
            m_canvas->top_window_offset(ox, oy);
            return {double(ox), double(oy), double(m_canvas->w()), double(m_canvas->h())};
        };
        const std::function<std::optional<common::Rect>()> panelAvoid =
            [this]() -> std::optional<common::Rect> {
            if (m_canvas == nullptr)
                return std::nullopt;
            const std::optional<common::Rect> b = m_canvas->textEditScreenBounds();
            if (!b)
                return std::nullopt;
            int ox = 0;
            int oy = 0;
            m_canvas->top_window_offset(ox, oy);
            return common::Rect{b->x + ox, b->y + oy, b->w, b->h};
        };
        m_typePanel->setPlacementProviders(panelRegion, panelAvoid);
        m_type3dPanel->setPlacementProviders(panelRegion, panelAvoid);
        // The S32 adjustment editor: the Type panels' pinned-corner sibling, shown while the
        // active layer is an adjustment with parameters (updateAdjustmentPanel drives it off the
        // active-layer transitions each frame). Its edits stream through applyAdjustmentField --
        // one coalesced SetAdjustmentParamsCommand per control run -- so the canvas is the live
        // preview and undo composes for free. Created here (before the colour flyout / dropdown
        // sub-windows) so the stacking rule holds.
        m_adjustmentPanel = new AdjustmentPanel();
        m_adjustmentPanel->hide();
        m_adjustmentPanel->setOnEdit(
            [this](const std::string& id, std::function<void(std::map<std::string, double>&)> mut) {
                applyAdjustmentField(id, std::move(mut));
            });
        m_adjustmentPanel->setPlacementProviders(panelRegion);
        // The fade's under-image: reconstruct what the canvas shows beneath the panel rect --
        // apron color outside the document, elsewhere the CPU composite sampled through the view
        // transform over the present shader's screen-space 8px checkerboard (the same two greys).
        // Panel-sized and only queried while faded, so the cost is a few hundred kilopixels.
        m_adjustmentPanel->setUnderProvider([this](int w, int h) -> common::Image {
            if (m_canvas == nullptr || w <= 0 || h <= 0)
                return {};
            common::Image img(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
            const common::Color8 apron = activePalette().canvasBg;
            int ox = 0, oy = 0;
            m_canvas->top_window_offset(ox, oy);
            const int px = m_adjustmentPanel->x() - ox; // panel origin, canvas-local logical px
            const int py = m_adjustmentPanel->y() - oy;
            const CanvasView& view = m_canvas->view();
            // Audit A10, the scattered gather. Bound ONCE, outside the two loops, through the
            // named readback seam: under the resident lane this materialises the mirror a single
            // time per fade rather than once per pixel (S60-a item 12). Its real answer is
            // `requestScreenRect`, which needs a WindowRenderer screen-capture provider that does
            // not exist yet -- until it does, this keeps the document-space gather it always had.
            const common::Image& under =
                hostComposite(render::consumers::kPanelFade, render::Freshness::Settled);
            const bool haveDoc = m_document != nullptr && !under.empty();
            for (int j = 0; j < h; ++j) {
                for (int i = 0; i < w; ++i) {
                    const double sx = px + i + 0.5;
                    const double sy = py + j + 0.5;
                    common::Color8 out = apron;
                    if (haveDoc) {
                        const common::Vec2 d = view.toDoc({sx, sy});
                        const long dx = static_cast<long>(std::floor(d.x));
                        const long dy = static_cast<long>(std::floor(d.y));
                        if (dx >= 0 && dy >= 0 && dx < static_cast<long>(under.width) &&
                            dy < static_cast<long>(under.height)) {
                            const std::size_t p =
                                (static_cast<std::size_t>(dy) * under.width +
                                 static_cast<std::size_t>(dx)) *
                                4;
                            const double a = under.rgba[p + 3] / 255.0;
                            const bool dark = ((static_cast<long>(std::floor(sx / 8.0)) +
                                                static_cast<long>(std::floor(sy / 8.0))) &
                                               1) != 0;
                            const double ck = dark ? 205.0 : 255.0; // canvas_present.comp greys
                            const auto mix = [&](int ch) {
                                return static_cast<std::uint8_t>(
                                    std::lround(under.rgba[p + ch] * a + ck * (1.0 - a)));
                            };
                            out = {mix(0), mix(1), mix(2), 255};
                        }
                    }
                    const std::size_t dp = (static_cast<std::size_t>(j) * w + i) * 4;
                    img.rgba[dp + 0] = out.r;
                    img.rgba[dp + 1] = out.g;
                    img.rgba[dp + 2] = out.b;
                    img.rgba[dp + 3] = 255;
                }
            }
            return img;
        });
        // The Levels/Threshold histogram source: the target adjustment's backdrop (the scope
        // composite WITHOUT its own step), small and doc-proportional -- cost bounded by the
        // preview size, queried only when the panel changes target.
        m_adjustmentPanel->setBackdropProvider([this]() -> common::Image {
            core::Layer* l =
                m_document ? m_document->find(m_adjustmentPanel->target()) : nullptr;
            auto* adj = l != nullptr ? l->as<core::AdjustmentLayer>() : nullptr;
            if (adj == nullptr)
                return {};
            const double scale =
                160.0 / std::max<std::uint32_t>(1, std::max(m_document->width(),
                                                            m_document->height()));
            const auto pw = static_cast<std::uint32_t>(
                std::max(1.0, std::round(m_document->width() * scale)));
            const auto ph = static_cast<std::uint32_t>(
                std::max(1.0, std::round(m_document->height() * scale)));
            return render::adjustmentBackdrop(*adj, m_document->width(), m_document->height(),
                                              pw, ph);
        });
        // The selection-morphology panel (Grow/Shrink/Feather/Smooth): the Type/adjustment panels'
        // pinned-corner sibling, opened by the Select menu instead of the old modal "N px" prompt.
        // Its mode/amount changes stream through previewSelectMorph -- the host morphs a snapshot of
        // the selection and updates the marching ants live -- and Apply lands ONE SetSelectionCommand.
        // Created here (with the panels, before the flyout/dropdown sub-windows) so the stacking holds.
        m_selectMorphPanel = new SelectMorphPanel();
        m_selectMorphPanel->hide();
        m_selectMorphPanel->setPlacementProviders(panelRegion);
        m_selectMorphPanel->setOnPreview([this] { previewSelectMorph(); });
        m_selectMorphPanel->setOnApply([this] {
            applySelectMorph();
            closeSelectMorph();
        });
        m_selectMorphPanel->setOnCancel([this] { closeSelectMorph(); });
        // The Image-menu operations panel (S53): Image Size / Canvas Size / Rotate Arbitrary, the
        // morphology panel's twin -- every control change streams through previewImageOps (the host
        // stages a canvas overlay of the new canvas, NO command) and Apply lands ONE document
        // command. Created HERE, with the other panels and BEFORE the DropdownPopup below, because
        // it carries dropdowns of its own: a Popover built after the shared dropdown window has its
        // lists fall behind it and the clicks miss (the stacking rule spelled out at the popup).
        m_imageOpsPanel = new ImageOpsPanel();
        m_imageOpsPanel->hide();
        m_imageOpsPanel->setPlacementProviders(panelRegion);
        m_imageOpsPanel->setFillColorProviders([this] { return m_colors.foreground(); },
                                               [this] { return m_colors.background(); });
        m_imageOpsPanel->setOnPreview([this](const ImageOpsPanel::Request& r) {
            previewImageOps(r);
        });
        m_imageOpsPanel->setOnApply([this](const ImageOpsPanel::Request& r) {
            applyImageOps(r);
            closeImageOps();
        });
        m_imageOpsPanel->setOnCancel([this] { closeImageOps(); });
        // Dragging the staged preview's handles on the canvas is the same edit as typing a size:
        // the canvas reports the new rect (document pixels) and the PANEL stays the owner of the
        // numbers -- it adopts the size, derives the anchor cell the drag implies, refreshes its
        // fields and re-fires the preview through the ordinary coalesced path.
        m_canvas->setOnImageOpPreviewDrag(
            [this](long dx, long dy, std::uint32_t dw, std::uint32_t dh) {
                if (m_imageOpsPanel != nullptr && m_imageOpsPanel->shown())
                    m_imageOpsPanel->applyPreviewDrag(dx, dy, dw, dh);
            });
        // The panels' shared colour bubble (the Fill dialog's chip-"Edit…" paradigm): ONE
        // ColorFlyout child sub-window serves both colour lines -- Style and 3D are mutually
        // exclusive, so the pick router just asks which panel is up. ORDER MATTERS (the
        // fill-dialog rule): created AFTER the panels (it must stack above them) and BEFORE the
        // DropdownPopup below (its surface combo's list must stack above the flyout).
        m_panelColorFlyout = new ColorFlyout();
        m_panelColorFlyout->hide();
        m_panelColorFlyout->setUseForeground([this] { return m_colors.foreground(); });
        m_panelColorFlyout->setOnPick([this](common::Color8 c) {
            const common::ColorF f = colorToF(c);
            if (m_type3dPanel != nullptr && m_type3dPanel->shown()) {
                applyTextBlockField("extrude:albedo", [f](core::text::TextBlock& b) {
                    if (b.extrude)
                        b.extrude->material.albedo = {f.r, f.g, f.b, 1.0f};
                });
            } else if (m_typePanel != nullptr && m_typePanel->shown()) {
                applyTextStyleField("style:color",
                                    [f](core::text::CharStyle& s) { s.setSolidFill(f); });
            } else if (m_imageOpsPanel != nullptr && m_imageOpsPanel->shown()) {
                // The Image-ops panel's Fill = "Color…" chip. Same router, same rule: the panel
                // that is up owns the pick.
                m_imageOpsPanel->setCustomFillColor(c);
            } else if (m_adjustmentPanel != nullptr && m_adjustmentPanel->shown()) {
                // S34-a: Photo Filter's Custom swatch. The panel ignores the pick unless the kind
                // it is reflecting actually has a colour to take.
                m_adjustmentPanel->setPickedColor(c);
            }
        });
        const auto openPanelColor = [this](const Fl_Widget* anchor, common::ColorF current) {
            if (m_panelColorFlyout == nullptr)
                return;
            if (m_panelColorFlyout->shownForAnchor(anchor)) { // a re-click toggles it shut
                m_panelColorFlyout->hide();
                return;
            }
            m_panelColorFlyout->openFor(anchor, colorTo8(current));
        };
        m_typePanel->setOnEditColor(openPanelColor);
        m_type3dPanel->setOnEditColor(openPanelColor);
        // Same bubble for the Image-ops panel's Fill = "Color…" chip (it speaks Color8, the Type
        // panels speak ColorF; the flyout wants Color8 either way).
        m_imageOpsPanel->setOnEditFillColor(
            [openPanelColor](const Fl_Widget* anchor, common::Color8 current) {
                openPanelColor(anchor, colorToF(current));
            });
        // S22 Gradient tool: the reusable stops/spread/blend-curve editor, anchored to the options
        // bar's "Stops…" button. SAME ORDER RULE as the colour flyout (created before the
        // DropdownPopup so its spread combo's list stacks above it). Its edits update the tool's
        // working ramp AND, when a gradient layer is bound for edit, that layer live.
        m_gradientFlyout = new GradientFlyout();
        m_gradientFlyout->hide();
        m_gradientFlyout->setUseForeground([this] { return m_colors.foreground(); });
        m_gradientFlyout->setOnChange([this](const core::vec::Gradient& g) {
            m_gradientPaint.stops = g.stops; // the flyout owns stops + spread; type comes from the bar
            m_gradientPaint.spread = g.spread;
            applyGradientStopsToEditTarget(); // live-recolour the bound gradient layer, if any
        });
        // The Image-ops panel's own gradient + pattern editors (S53 Fill parity with Edit→Fill…).
        // Its OWN, not the Gradient tool's: that one's onChange is bound to the tool's working ramp
        // for the life of the window, and a panel that re-pointed it would leave the tool editing
        // the wrong thing after the panel closed. Created here, on the same ordering rule (before
        // the DropdownPopup, so their internal combos' lists stack above them). The panel drives
        // openFor/onChange itself; these hand over only what the HOST knows.
        m_imageOpsGradientFlyout = new GradientFlyout();
        m_imageOpsGradientFlyout->hide();
        m_imageOpsGradientFlyout->setUseForeground([this] { return m_colors.foreground(); });
        m_imageOpsPatternFlyout = new PatternFlyout();
        m_imageOpsPatternFlyout->hide();
        m_imageOpsPatternFlyout->setUseForeground([this] { return m_colors.foreground(); });
        m_imageOpsPanel->setPaintFlyouts(m_imageOpsGradientFlyout, m_imageOpsPatternFlyout);
        // S34-a: the Gradient Map adjustment's ramp editor. Its OWN flyout, on the same ordering
        // rule (created before the DropdownPopup so its spread combo's list stacks above it); the
        // panel re-points onChange on every open, so it never fights another opener.
        m_adjustmentGradientFlyout = new GradientFlyout();
        m_adjustmentGradientFlyout->hide();
        m_adjustmentGradientFlyout->setUseForeground([this] { return m_colors.foreground(); });
        m_adjustmentPanel->setGradientFlyout(m_adjustmentGradientFlyout);
        // ... and Photo Filter's Custom swatch shares the panels' colour bubble, exactly like the
        // Type / 3D / Image-ops colour lines above.
        m_adjustmentPanel->setOnEditColor(
            [openPanelColor](const Fl_Widget* anchor, common::Color8 current) {
                openPanelColor(anchor, colorToF(current));
            });
        // The options-bar slider precision ruler (the ScrubSlider HUD): a borderless child
        // sub-window on the same build-before-show rule, floated at the cursor during a precision
        // drag. Created here so it is a real sub-surface; the bar hands it to each slider it
        // builds.
        m_scrubRuler = new ScrubRuler();
        m_scrubRuler->hide();
        // The shared themed Dropdown list -- a child sub-window on the same rules (built before
        // show, so it's a real sub-surface). Every ui::Dropdown in this window opens through it.
        // Created AFTER the content popovers (picker / overflows / designer) so in the child
        // stacking order it sits ABOVE them -- otherwise a Dropdown opened inside a later-created
        // popover would have its flyout fall BEHIND the popover, and clicks over the popover's
        // controls miss the list.
        m_dropdownPopup = new DropdownPopup();
        m_dropdownPopup->hide();
        // The shared themed text-field right-click menu (same sub-surface rules). Every themed text
        // field whose top-level is this window opens through it -- the crop options bar's number
        // inputs AND the colour picker's hex / numeric readouts (the picker is a child sub-window
        // of this one, so its fields resolve to this menu via top_window(), exactly like its
        // Dropdowns).
        (new ContextMenu())->hide();
        end();
        resizable(m_canvas);
        m_toolbar->setColorPicker(m_colorPicker);
        m_toolbar->setToolFlyout(m_toolFlyout);
        m_toolbar->setOverflowPopover(m_toolbarOverflow); // S16-o: route the toolbar overflow here
        m_optionsBar->setOverflowPopover(m_optionsOverflow); // S16-n: route overflow to the popover
        m_optionsBar->setScrubRuler(m_scrubRuler);           // slider precision HUD
        m_typePanel->setScrubRuler(m_scrubRuler);   // ... shared with the Type panel sliders
        m_type3dPanel->setScrubRuler(m_scrubRuler); // ... and the 3D popup's
        m_adjustmentPanel->setScrubRuler(m_scrubRuler); // ... and the adjustment editor's (S32)
        m_selectMorphPanel->setScrubRuler(m_scrubRuler); // ... and the morphology panel's amount slider
        m_imageOpsPanel->setScrubRuler(m_scrubRuler);    // ... and the Image-ops size scrub sliders
        m_imageOpsPatternFlyout->setRuler(m_scrubRuler); // ... and its pattern editor's sliders
        // The corner-panel arbiter (S32 round 5, docs/adjustment-layers.md §5): the ONE place
        // that decides which corner panel is visible. Style/3D are EXPLICIT (button-toggled,
        // valid only during a text session with a live anchor); the adjustment editor is
        // CONDITIONAL (its token = the active adjustment layer's id, so Esc suppresses exactly
        // that layer until the selection moves). syncCornerPanels() reconciles widget reality
        // against the arbiter's answer each frame -- external hides (theme, bar rebuild, Esc)
        // self-heal there instead of desyncing hand-rolled per-panel state.
        m_panelArbiter.addExplicit(kPanelStyle, [this] {
            return m_canvas != nullptr && m_canvas->textEditTarget() != core::kInvalidLayerId &&
                   m_optionsBar != nullptr && m_optionsBar->typePanelButton() != nullptr;
        });
        m_panelArbiter.addExplicit(kPanelType3d, [this] {
            return m_canvas != nullptr && m_canvas->textEditTarget() != core::kInvalidLayerId &&
                   m_optionsBar != nullptr && m_optionsBar->type3dButton() != nullptr;
        });
        m_panelArbiter.addConditional(kPanelAdjustment, [this]() -> std::uint64_t {
            if (!m_document || m_layerPanel == nullptr)
                return 0;
            core::Layer* l = m_document->find(m_layerPanel->activeLayer());
            auto* adj = l != nullptr ? l->as<core::AdjustmentLayer>() : nullptr;
            if (adj == nullptr || core::adjustmentParamSchema(adj->adjustmentKind()).empty())
                return 0;
            return static_cast<std::uint64_t>(adj->id());
        });
        // The selection-morphology panel is EXPLICIT (opened by the Select menu). Its context is a
        // live selection: when the selection is cleared out from under it the request evaporates and
        // syncCornerPanels closes it -- updateSelectMorph then drops the transient preview.
        m_panelArbiter.addExplicit(kPanelSelectMorph, [this] {
            return m_document != nullptr && !m_document->selection().isEmpty() &&
                   m_canvas != nullptr && m_selectMorphPanel != nullptr;
        });
        // The Image-ops panel is EXPLICIT too (opened by the Image menu). Its context is simply a
        // document: close the tab out from under it and the request evaporates, syncCornerPanels
        // hides it, and updateImageOps then drops the staged canvas overlay.
        m_panelArbiter.addExplicit(kPanelImageOps, [this] {
            return m_document != nullptr && m_canvas != nullptr && m_imageOpsPanel != nullptr;
        });
        // All chrome exists now: subscribe to runtime theme changes (Settings -> Appearance). The
        // RAII subscription removes itself when this window is destroyed.
        m_themeSub = ThemeSubscription([this] { reapplyTheme(); });

        // FLTK's default window callback *closes the window on Escape*, which here would quit the
        // whole app. handle() already consumes Escape (to dismiss the picker), so it should never
        // reach this close path -- but keep the guard as a backstop so a stray Escape can never
        // exit. A real close request (the WM button) arrives as FL_CLOSE and falls through to
        // hide() -> app exits.
        callback([](Fl_Widget* w, void*) {
            if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape)
                return; // backstop: never close the window on Escape
            // The window-manager close button bypasses the deactivated chrome, so guard it too: a
            // close while a file picker is modal-up would hang the portal's nested loop (the freeze).
            if (platform::fileDialogInFlight()) {
                fl_beep();
                return;
            }
            // A genuine close (WM button). Offer to save every dirty document first (S49) -- and
            // walk them all, because each open tab holds its own recovery journal and a deliberate
            // quit must discard them; leaving one behind would make the next launch offer to restore
            // a document the user just closed on purpose.
            auto* self = static_cast<MainWindow*>(w);
            // A background save finishes FIRST, progress live (design note): the WM button
            // bypasses the deactivated chrome exactly like the file-picker case above, and a
            // second click while the wait pumps must not re-enter the close flow.
            if (self->m_waitingForSave) {
                fl_beep();
                return;
            }
            self->waitForBackgroundSave();
            if (!self->confirmQuit())
                return; // cancelled: stay open, nothing discarded
            w->hide();  // hide -> Fl::run() returns once no window is shown
        });
        // Floor the window size so the fixed toolbar + dock keep a usable canvas between them and
        // the options bar never shrinks past its controls (the min-width overlap). Width = toolbar
        // + a ~240px canvas + dock; height = menu + options bar + a usable body + status bar.
        size_range(kToolbarWidth + kCanvasMinWidth + kDockMinWidth,
                   kMenuBarHeight + kOptionsBarHeight + 300 + kStatusBarHeight);

        // Window icon, rasterized from the single SVG source (set before show()).
        m_iconImage = appIconImage(128);
        if (!m_iconImage.empty()) {
            Fl_RGB_Image rgb(m_iconImage.rgba.data(), m_iconImage.width, m_iconImage.height, 4);
            icon(&rgb); // FLTK copies the pixels into its platform icon representation
            // Also make it the default for windows created later (the Settings dialog, etc.).
            Fl_Window::default_icon(&rgb);
        }

        // Open on the empty click-or-drop state (the S50 prerequisite): no document until the
        // user opens or creates one. The old compositor placeholder lives on in debug builds as
        // Help -> Open Demo Canvas.
        clearDocument();
    }

    // A clean exit discards the recovery journal (spec 2.6): the file survives ONLY a crash, so
    // reaching this destructor -- the WM close or auto-quit both do -- means there is nothing
    // unsaved to restore. A crash never runs this, leaving the journal for the next open to find.
    ~MainWindow() override {
        Fl::remove_timeout(dockRelayoutTimer, this); // a pending splitter relayout must not outlive us
        Fl::remove_timeout(frameTimer, this); // ... nor a queued frame, which would otherwise fire
                                              // into a half-destroyed window
        // S60-a item 13: a BACKSTOP only. The resident lane holds a VulkanContext borrowing the
        // canvas's device, and the device is normally gone by the time we get here -- hide() tears
        // the renderer down before FLTK frees the native window, which is why the real release is
        // the setOnRendererShutdown hook. This covers the path where a canvas was built but never
        // shown, and is a no-op whenever the hook already ran.
        m_tiles.reset();
        discardJournal();
        releaseLock();
        // Background tabs each hold their own journal + lock (S49). We only get here on a clean
        // teardown, so discard them all -- a leftover journal would offer to "restore" a document
        // the user closed on purpose. (A crash never runs this, which is exactly the point.)
        for (DocumentSession& s : m_sessions) {
            if (s.journal.has_value())
                s.journal->discard();
            if (s.lock.has_value())
                s.lock->release();
        }
#ifdef __APPLE__
        // The system menu bar is NOT one of this window's children -- Fl_Sys_Menu_Bar removes
        // itself from its group in its constructor, so the Fl_Group destructor will not reach it.
        // It also owns the process-wide fl_sys_menu_bar pointer and every callback's user_data
        // (this window), so it must not outlive us.
        delete m_menu;
        m_menu = nullptr;
#endif
    }

    void run(const std::vector<std::string>& openPaths = {}) {
        show(); // no argv forwarding: Mosaic's own CLI args are parsed in main, not by FLTK
        // Windows shutdown / restart / log-off (platform/session_end.hpp): answer the OS's
        // WM_QUERYENDSESSION instead of being killed with unsaved work on screen. AFTER show(),
        // because the guard subclasses the native window and show() is what creates it. A no-op on
        // Linux and macOS, so no platform conditional -- see the header.
        platform::installSessionEndGuard(
            this, [this] { return anySessionDirty(); }, [this] { syncJournalsForSessionEnd(); },
            _("Mosaic has unsaved changes. They will be offered back the next time you open it."));
        // --profile / MOSAIC_PROFILE=1 asked for measurement, so give it a face: open the Timing
        // Profiler straight away rather than making the user find a menu item (S60-alpha). It is
        // non-modal and hides with the main window, so it never keeps Fl::run() alive on quit.
        if (Profiler::enabled())
            openTimingGraph();
        m_frameLoopStarted = true; // requestFrame may re-arm the timer from here on
        // First frame ASAP, then paced by the display. Through armFrameAt rather than a bare
        // add_timeout so the "is a frame already coming?" bookkeeping starts out true -- a request
        // arriving before this frame runs must find it, not queue a second one.
        armFrameAt(nowSeconds());
        std::vector<std::string> paths = openPaths;
#ifdef __APPLE__
        // Finder's open-documents event beats the first frame, so anything it queued belongs to
        // THIS startup and takes the command-line path below (including its skip of the untitled
        // restore scan) rather than arriving a frame later as an unrelated open.
        std::vector<std::string>& queued = macPendingOpens();
        paths.insert(paths.end(), queued.begin(), queued.end());
        queued.clear();
#endif
        if (!paths.empty()) {
            // Files handed to us on the command line (dropped on the icon, "Open with..." over a
            // multi-selection, a shell glob). The window is mapped, so each path's own recovery
            // faces -- damage, crash restore, the already-open-elsewhere lock -- can all show over
            // it, exactly as File->Open does. Each lands in its own tab (S49); the LAST stays
            // active, matching the order the shell listed them.
            //
            // The untitled crash-restore scan is deliberately SKIPPED here. Offering to restore
            // unsaved untitled work and then replacing it with the document the user actually
            // asked for would destroy the restore; the journal stays on disk untouched, and a
            // plain launch (no file argument) still finds it.
            for (const std::string& path : paths)
                openDocumentAtPath(path);
            return;
        }
        // Crash restore for untitled documents (flow 1): the window is mapped, so a restore modal
        // can show over the empty state. File-backed journals wait for their file to be opened.
        offerUntitledRestoresAtStart();
    }

    // Apply the persisted picker state (surface + recents) and arrange for write-back when the
    // user changes either (S12-a part 2 / S12-b). Load-modify-save keeps each write honest
    // against fields other code may persist.
    void configurePicker(const RunOptions& opts) {
        m_settingsPath = opts.settingsPath; // where Settings live; the Settings dialog writes here
        // Keybindings (S51-b): the user's SPARSE remaps, read from settings.json here rather than
        // carried on RunOptions -- the keymap is entirely ui's business and main() has no use for it.
        // Applied through applyKeymapToMenu(), the same path a live remap takes, so start-up exercises
        // the remap code every single launch instead of leaving it to the settings dialog to discover.
        if (!m_settingsPath.empty()) {
            std::string err;
            const common::Settings cfg = common::loadSettings(m_settingsPath, &err);
            m_keymap.setOverrides(cfg.keymap);
        }
        applyKeymapToMenu(); // unconditional: it also installs the live text-editor fence
        // The dock's persisted width. applyDockWidth() clamps it against the live window, so an
        // out-of-range value on disk simply lands at the nearest legal width.
        m_dockWidth = opts.dockWidth;
        applyDockWidth();
        // ... and the preset section's persisted height + selected preset (§8.2). presetSplit()
        // clamps the height against the live dock, and the preset is restored by NAME, so neither a
        // stale settings file nor a bundle the user has since removed can strand the dock.
        if (m_dock != nullptr) {
            m_dock->setPresetHeight(opts.brushPresetHeight);
            // Grid or Cards (§8.2). By NAME, and anything unrecognised -- including the "" a settings
            // file written before this field existed reads back as -- lands on the default, Cards.
            if (m_dock->presets() != nullptr)
                m_dock->presets()->setDisplayMode(
                    ui::presetDisplayModeFromKey(opts.brushPresetDisplay));
        }
        restorePreset(ui::PresetCorpus::Brush, opts.brushPreset);
        restorePreset(ui::PresetCorpus::Eraser, opts.eraserPreset);
        syncPresetSectionVisibility(); // the Brush may be the tool we started on
        m_workingProfile =
            opts.workingProfile;           // re-applied with the picker's CMYK on a CMYK change
        m_metric = opts.units == "metric"; // crop size HUD unit (S16-e); already locale-resolved
        m_cropSwitchToolAfterApply = opts.cropSwitchToolAfterApply; // S16-p
        m_cropClearSelectionOnLeave =
            opts.cropClearSelectionOnLeave; // clear staged crop on tool-leave
        m_tools.setEraserSizeTie(opts.eraserSizeFollowsBrush); // Tools/Eraser: shared size (§8.4)
        // Brush smoothing lives on the brush-family context bars (it steadies the POINTER, so it is
        // not a tablet-only setting and did not belong on the Tablet page). It is still PERSISTED,
        // unlike Size or Opacity: it is a set-once preference, not a per-stroke one.
        m_tools.setBrushSmoothingEnabled(opts.brushSmoothing);
        // Tool icon packs (S52): user packs live under dataDir()/icon_packs; tools were BORN with
        // the embedded default's art, so only a non-default persisted pack costs a toolbar reload.
        m_iconPacks.scan(common::dataDir() / "icon_packs");
        applyIconPack(opts.iconPack);
        if (m_canvas) {
            m_canvas->setCropFraming(parseCropFraming(opts.cropInitialFraming)); // S16-q
            m_canvas->setLassoSmoothing(opts.lassoSmooth); // Tools/Lasso: freehand smoothing
            m_canvas->setSelectBrushAddByDefault(
                opts.selectBrushAddByDefault); // S18: the select brush's no-modifier op (§9-B)
            m_canvas->setOverlayLineStyle(     // Appearance: overlay-line style
                parseOverlayLineStyle(opts.overlayLineStyle));
            m_canvas->setFeatherIndicator( // Appearance: feathered-selection indicator (A / F)
                parseFeatherIndicator(opts.featherIndicator));
            m_canvas->setAntsCirculate(opts.antsCirculate); // §5 hidden marching-ants experiment
            applyTabletPolicy(opts); // Settings → Tablet: the §7 policy, live from the first sample
        }
        if (m_ticker) // Annoyances (Settings): drive the menu-bar ticker, not the (retired) GPU
                      // path
            m_ticker->setEnabled(opts.motivationalLines);
        m_showUnsavedDuration = opts.showUnsavedDuration; // Annoyances: unsaved-title duration (S18-d)
        m_unsavedIncludeSeconds = opts.unsavedIncludeSeconds;
        updateWindowTitle();
        // Settings → Text (deferred §2): spell-check state + the app default text language, which
        // also seeds the shaper's default (feeding hyphenation) -- finally wiring
        // setDefaultLanguage.
        m_spellCheckEnabled = opts.spellCheck;
        m_spellCheckAllCaps = opts.spellCheckAllCaps;
        m_textLanguage = opts.textLanguage;
        m_textShaper.setDefaultLanguage(spellAppLanguage());
        m_emojiFont = opts.emojiFont; // preferred colour-emoji fallback family (R5)
        m_fontDb.setPreferredEmojiFamily(m_emojiFont);
        // Type 3D (S30-c): route extruded-text rendering through the Vulkan compute-raster lane,
        // created LAZILY on the first 3D render so flat-text users never pay for a second device;
        // any failure (no Vulkan, device loss) falls back to the CPU lane silently per call.
        core::text::setExtrudeRenderOverride(
            [](common::ImageF& dst, const core::text::ExtrudeMesh& mesh,
               const core::text::Extrude& ex, const common::Affine2D& toPixel, bool aa,
               const core::text::ExtrudeEnv* env, const core::text::ExtrudeOverlay* overlay) {
                static const std::unique_ptr<render::ExtrudeGpu> gpu = [] {
                    std::string err;
                    auto g = render::ExtrudeGpu::create(/*enableValidation=*/false, err);
                    if (!g)
                        uiLog().info("3D text: Vulkan lane unavailable ({}); CPU lane serves", err);
                    return g;
                }();
                MOSAIC_PERF_SCOPE("3D text raster", Lane::Gpu);
                const bool served =
                    gpu != nullptr && gpu->render(dst, mesh, ex, toPixel, aa, env, overlay);
                static bool logged = false; // one-time: which lane actually serves this machine
                if (!logged && gpu != nullptr) {
                    logged = true;
                    uiLog().info("3D text: {} lane serving", served ? "Vulkan" : "CPU");
                }
                return served;
            });
        // Texture Generator (S55-h): route sky/paper renders through the Vulkan compute lane,
        // created LAZILY on the first texture render; grass (and any refusal -- unsupported
        // feature, device loss) falls back to the CPU reference lane silently per call. The
        // mutex serialises the two callers that can race: the dialog's TextureRenderWorker
        // thread and the UI thread's pre-composite refreshTextureCaches.
        core::texture::setTextureRenderOverride(
            [](const core::texture::TextureParams& params, std::uint32_t w, std::uint32_t h,
               const core::texture::TextureWindow& window,
               core::texture::TextureRenderProgress* progress,
               core::texture::TextureRenderResult& out) {
                static std::mutex laneMutex;
                const std::lock_guard<std::mutex> lock(laneMutex);
                static const std::unique_ptr<render::TextureGpu> gpu = [] {
                    std::string err;
                    auto g = render::TextureGpu::create(/*enableValidation=*/false, err);
                    if (!g)
                        uiLog().info("textures: Vulkan lane unavailable ({}); CPU lane serves",
                                     err);
                    else
                        uiLog().info("textures: Vulkan lane on {}", g->deviceName());
                    return g;
                }();
                MOSAIC_PERF_SCOPE("Texture generate", Lane::Gpu);
                return gpu != nullptr && gpu->render(params, w, h, window, progress, out);
            });
        // S33 blur adjustments (docs/blur-filters.md §8): route the heavy blur kernels through
        // the Vulkan compute lane, created LAZILY on the first blur composite; any refusal
        // (Box/Motion/Radial, oversized buffers, device loss) falls back to the CPU reference
        // lane silently per call. The mutex guards the NOT-thread-safe lane object
        // (blur_gpu.hpp) -- composites run on the UI thread today, so it is uncontended.
        render::setBlurRenderOverride([](common::ImageF& img, const render::BlurOp& op) {
            static std::mutex laneMutex;
            const std::lock_guard<std::mutex> lock(laneMutex);
            static const std::unique_ptr<render::BlurGpu> gpu = [] {
                std::string err;
                auto g = render::BlurGpu::create(/*enableValidation=*/false, err);
                if (!g)
                    uiLog().info("blur: Vulkan lane unavailable ({}); CPU lane serves", err);
                else
                    uiLog().info("blur: Vulkan lane on {}", g->deviceName());
                return g;
            }();
            MOSAIC_PERF_SCOPE("Blur", Lane::Gpu);
            return gpu != nullptr && gpu->render(img, op);
        });
        if (m_layerPanel)
            m_layerPanel->setMultiSelectionMode(
                parseMultiSelectMode(opts.multiSelectionEdits)); // S15-e
        // Inpainting engine selection (Settings → Inpainting). Fall back to the engine's own
        // default if the persisted backend id is unknown (e.g. a removed backend).
        m_inpaintBackendId = opts.inpaintBackend;
        m_inpaintPresetId = opts.inpaintPreset;
        m_inpaintOverrides = opts.inpaintParams;
        if (!m_inpaintEngine.setActiveBackend(m_inpaintBackendId))
            m_inpaintBackendId = m_inpaintEngine.activeBackend();
        recomputeInpaintParams();
        if (m_statusBar)
            m_statusBar->setMetric(m_metric); // status-bar physical size follows the same unit
        // S12-c: user ICC overrides first (a custom working space outranks the document enum).
        const ColorPicker::ProfileLoad profiles =
            m_colorPicker->applyProfileSettings(opts.workingProfile, opts.cmykProfile);
        if (!profiles.workingOk)
            uiLog().warn("working ICC profile failed to load: {}", opts.workingProfile);
        if (!profiles.cmykOk)
            uiLog().warn("CMYK ICC profile failed to load: {}", opts.cmykProfile);
        syncColorSpaceName(); // a loaded working profile renames the status-bar indicator
        if (const auto s = parsePickerSurface(opts.pickerSurface))
            m_colorPicker->setSurface(*s);
        m_colorPicker->setRecentColors(opts.recentColors);
        const std::filesystem::path settingsPath = opts.settingsPath;
        m_colorPicker->setSurfaceChangedCallback([settingsPath](ColorPicker::Surface s) {
            if (settingsPath.empty())
                return;
            std::string err;
            common::Settings cfg = common::loadSettings(settingsPath, &err);
            cfg.pickerSurface = pickerSurfaceKey(s);
            if (!common::saveSettings(cfg, settingsPath, &err))
                uiLog().warn("could not persist picker surface: {}", err);
        });
        m_colorPicker->setRecentsChangedCallback(
            [settingsPath](const std::vector<std::string>& hex) {
                if (settingsPath.empty())
                    return;
                std::string err;
                common::Settings cfg = common::loadSettings(settingsPath, &err);
                cfg.recentColors = hex;
                if (!common::saveSettings(cfg, settingsPath, &err))
                    uiLog().warn("could not persist recent colours: {}", err);
            });
        m_recentFiles = opts.recentFiles; // persisted File -> Open Recent list (S55)
        m_recentSizes = opts.recentSizes; // persisted File -> New custom-size cards (round 5)
        rebuildRecentMenu();
        // The app-owned thumbnail cache is RETIRED (user 2026-07-22): .mosaic previews live in
        // the file's own PRVW chunk (S48-b) and plain images read the desktop's shared cache.
        // Delete the whole folder once -- derived, app-owned state; nothing else lives there.
        if (const std::filesystem::path state = common::stateDir(); !state.empty()) {
            std::error_code cleanupEc;
            std::filesystem::remove_all(state / "thumbnails", cleanupEc);
        }
    }

    // ---- Recent files + the New Document dialog's inputs (S55) ----

    // '&' starts a mnemonic and '/' a submenu path in FLTK menu labels; a filename must render
    // verbatim.
    static std::string menuEscapeLabel(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 4);
        for (const char c : s) {
            if (c == '&')
                out += "&&";
            else if (c == '/')
                out += "\\/";
            else if (c == '\\')
                out += "\\\\";
            else
                out += c;
        }
        return out;
    }

    void persistRecentFiles() {
        if (m_settingsPath.empty())
            return;
        std::string err;
        common::Settings cfg = common::loadSettings(m_settingsPath, &err);
        cfg.recentFiles = m_recentFiles;
        cfg.recentSizes = m_recentSizes;
        if (!common::saveSettings(cfg, m_settingsPath, &err))
            uiLog().warn("could not persist recent files: {}", err);
    }

    // Move-or-insert `canonicalPath` at the head of the recent list, persist, and refresh the
    // File menu. Called after every successful open and save (the path is then known good).
    void recordRecentFile(const std::string& canonicalPath) {
        if (canonicalPath.empty())
            return;
        common::pushRecentFile(m_recentFiles, canonicalPath);
        persistRecentFiles();
        rebuildRecentMenu();
    }

    // The shared "a save landed at `path`" tail: the file joins the recent list. Its dialog
    // preview needs no extra work -- the save itself embedded a PRVW chunk (S48-b).
    void noteDocumentSaved(const std::string& path) {
        recordRecentFile(canonicalPathOf(path));
    }

    // Repopulate File -> Open Recent: strip the previous dynamic rows (ours by callback
    // identity), then insert the current list before the permanent Clear Recents tail.
    void rebuildRecentMenu() {
        if (m_menu == nullptr)
            return;
        const Fl_Menu_Item* clearIt = m_menu->find_item(cbClearRecents);
        if (clearIt == nullptr)
            return;
        // Edit the item ARRAY through Fl_Menu_ (the base), then publish once via refreshMenuBar()
        // at the end. Fl_Sys_Menu_Bar's own remove()/insert() rebuild the entire macOS system menu
        // per call, so an unqualified pair of loops here would rebuild it ~20 times per save/open
        // for a result the single update() at the end produces anyway.
        int i = static_cast<int>(clearIt - m_menu->menu()) - 1;
        while (i >= 0 && isOpenRecentCallback(m_menu->menu()[i].callback())) {
            m_menu->Fl_Menu_::remove(i); // may reallocate the array: re-read menu() each round
            --i;
        }
        const int insertAt = static_cast<int>(m_menu->find_item(cbClearRecents) - m_menu->menu());
        if (m_recentFiles.empty()) {
            m_menu->Fl_Menu_::insert(insertAt, _("No Recent Files"), 0, cbRecentNone, this,
                                     FL_MENU_INACTIVE | FL_MENU_DIVIDER);
        } else {
            const std::size_t count = std::min(m_recentFiles.size(), common::kMaxRecentFiles);
            for (std::size_t k = 0; k < count; ++k) {
                const std::string label =
                    menuEscapeLabel(std::filesystem::path(m_recentFiles[k]).filename().string());
                m_menu->Fl_Menu_::insert(insertAt + static_cast<int>(k), label.c_str(), 0,
                                         kOpenRecentCbs[k], this,
                                         k + 1 == count ? FL_MENU_DIVIDER : 0);
            }
        }
        // Clear Recents greys out when there is nothing to clear.
        if (auto* it = const_cast<Fl_Menu_Item*>(m_menu->find_item(cbClearRecents))) {
            if (m_recentFiles.empty())
                it->deactivate();
            else
                it->activate();
        }
        refreshMenuBar();
    }

    void clearRecentFiles() {
        m_recentFiles.clear();
        persistRecentFiles();
        rebuildRecentMenu();
        transientStatus(_("Recent files cleared"));
    }

    void openRecentIndex(std::size_t index) {
        if (index >= m_recentFiles.size())
            return;
        openRecentPath(m_recentFiles[index]); // copy inside: the open mutates the list
    }

    // Open a recent entry, self-healing the list when the file has moved or vanished.
    void openRecentPath(std::string path) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            tellError(_("Could not open the file"),
                      _("It may have been moved or deleted; it was removed from the recent list."),
                      path);
            common::removeRecentFile(m_recentFiles, path);
            persistRecentFiles();
            rebuildRecentMenu();
            return;
        }
        openDocumentAtPath(path);
    }

    // Fit a cached 256px preview to the dialog's card cell (never upscaling; the card blits 1:1).
    static common::Image fitCardThumb(const common::Image& thumb) {
        const FitSize fit = fitPreservingAspect(thumb.width, thumb.height, kNewDocCardThumbW,
                                                kNewDocCardThumbH);
        if (fit.width <= 0)
            return {};
        if (static_cast<std::uint32_t>(fit.width) >= thumb.width)
            return thumb;
        return boxDownscale(thumb, fit.width, fit.height);
    }

    static std::string pixelSizeSubtitle(std::uint32_t w, std::uint32_t h) {
        if (w == 0)
            return {};
        return std::to_string(w) + " × " + std::to_string(h) + " px";
    }

    // The dialog's greyed-form seed for a .mosaic card: the manifest's real values (a .mosaic
    // DOES carry the ppi and colour state), defaults where a token is unknown to this build.
    static std::optional<NewDocumentSpec> specFromInfo(
        const std::optional<io::native::DocumentFileInfo>& info) {
        if (!info.has_value() || info->width == 0)
            return std::nullopt;
        NewDocumentSpec v;
        v.title = info->title; // the document's own name (may be empty; the card title backs it)
        v.width = info->width;
        v.height = info->height;
        v.unit = SizeUnit::Pixels;
        v.dpi = info->dpi > 0.0 ? info->dpi : 72.0;
        if (info->colorSpace.has_value())
            v.colorSpace = *info->colorSpace;
        if (info->precision.has_value())
            v.precision = *info->precision;
        return v;
    }

    // Fill a card's preview + summary lines from what the file itself offers: a .mosaic reads
    // its embedded PRVW + manifest (S48-b -- the app-owned thumbnail cache is gone); a plain
    // image reads the DESKTOP'S shared thumbnail cache (never writing one of our own) and a
    // header-only dimension probe.
    static void fillCardFromFile(NewDocumentCard& card) {
        const bool isMosaic =
            std::filesystem::path(card.path).extension() == ".mosaic";
        if (isMosaic) {
            // ONE read + ONE chunk walk for both halves: asking the two readers separately walked
            // every frame of the file twice, which is 231 ms per card on a 302 MB document.
            const io::native::DocumentCard data = io::native::readDocumentCard(card.path);
            if (data.preview.has_value())
                card.thumb = fitCardThumb(checkerCompose(*data.preview));
            if (data.info.has_value())
                card.detail = pixelSizeSubtitle(data.info->width, data.info->height);
            card.values = specFromInfo(data.info);
        } else {
            if (const auto thumb = loadXdgThumbnail(card.path))
                card.thumb = fitCardThumb(checkerCompose(*thumb));
            if (const auto dims = io::probeImageDimensions(card.path)) {
                card.detail = pixelSizeSubtitle(dims->width, dims->height);
                NewDocumentSpec v; // px exact; a plain image carries no ppi/colour state
                v.title.clear();   // no embedded name either -- the card title stands in
                v.width = dims->width;
                v.height = dims->height;
                v.unit = SizeUnit::Pixels;
                v.dpi = 72.0;
                card.values = v;
            }
        }
    }

    // Pull the OS clipboard image synchronously: FLTK reveals clipboard image data only through
    // an async FL_PASTE reply, so this pumps the loop (bounded) until it lands -- the same nested
    // pumping every modal wait already does. Owners answer in milliseconds; the timeout only
    // guards a hung owner, and a timeout simply means "no Clipboard card this time".
    std::optional<common::Image> fetchClipboardImage(double timeoutSeconds) {
        if (Fl::clipboard_contains(Fl::clipboard_image) == 0)
            return std::nullopt;
        m_clipboardFetch.reset();
        m_clipboardFetchDone = false;
        m_clipboardFetchPending = true;
        Fl::paste(*this, 1, Fl::clipboard_image);
        for (double t = 0.0; !m_clipboardFetchDone && t < timeoutSeconds; t += 0.02)
            Fl::wait(0.02);
        m_clipboardFetchPending = false;
        return std::move(m_clipboardFetch);
    }

    // Everything the New Document dialog shows beyond the built-in presets: the recent files
    // that still exist (with cached previews), the template gallery, and the pre-fetched
    // clipboard image's card (real preview + exact pixel size).
    NewDocumentContext buildNewDocumentContext(const std::optional<common::Image>& clipboard) {
        NewDocumentContext ctx;
        if (clipboard.has_value() && !clipboard->empty()) {
            ctx.clipboardHasImage = true;
            const FitSize fit = fitPreservingAspect(clipboard->width, clipboard->height,
                                                    kNewDocCardThumbW, kNewDocCardThumbH);
            ctx.clipboardThumb = fit.width > 0 &&
                                         static_cast<std::uint32_t>(fit.width) < clipboard->width
                                     ? boxDownscale(*clipboard, fit.width, fit.height)
                                     : *clipboard;
            ctx.clipboardSubtitle = std::to_string(clipboard->width) + " × " +
                                    std::to_string(clipboard->height) + " px";
            NewDocumentSpec v; // px size known exactly; the rest are best-effort defaults
            v.width = clipboard->width;
            v.height = clipboard->height;
            v.unit = SizeUnit::Pixels;
            v.dpi = 72.0; // a clipboard image carries no ppi
            ctx.clipboardValues = v;
        }

        const char* homeEnv = std::getenv("HOME");
        const std::string home = homeEnv != nullptr ? homeEnv : "";
        for (const std::string& path : m_recentFiles) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec))
                continue; // keep the entry; an open attempt self-heals it (openRecentPath)
            NewDocumentCard card;
            card.path = path;
            // A .mosaic shows its stem -- the native extension is ours, not information (user
            // 2026-07-22). Foreign images keep theirs: "shot.png" says what the file IS.
            const std::filesystem::path fsPath(path);
            card.title = fsPath.extension() == ".mosaic" ? fsPath.stem().string()
                                                         : fsPath.filename().string();
            card.subtitle = abbreviatedLocation(path, home); // WHERE it lives (user request)
            fillCardFromFile(card); // preview + dims + greyed-form values, from the file itself
            ctx.recents.push_back(std::move(card));
        }

        // Remembered hand-entered sizes ("Sizes" cards): stale/garbled tokens simply drop out.
        for (const std::string& token : m_recentSizes) {
            if (const auto spec = parseCustomSizeToken(token))
                ctx.customSizes.push_back(*spec);
        }

        // Templates: the shipped set first (data/presets -- the installed location arrives with
        // S59's packaging), then the user's own (dataDir()/presets) -- the brush-set precedent.
        std::vector<TemplateFile> files =
            scanDocumentTemplates(common::installedDataDir() / "presets");
        {
            std::vector<TemplateFile> user = scanDocumentTemplates(common::dataDir() / "presets");
            files.insert(files.end(), std::make_move_iterator(user.begin()),
                         std::make_move_iterator(user.end()));
        }
        // Right-click housekeeping (round 7): removing a dialog card updates the persisted
        // lists (and the File -> Open Recent menu) immediately.
        ctx.onRemoveRecentFile = [this](const std::string& path) {
            common::removeRecentFile(m_recentFiles, path);
            persistRecentFiles();
            rebuildRecentMenu();
        };
        ctx.onForgetRecentSize = [this](const NewDocumentSpec& size) {
            common::removeRecentFile(m_recentSizes, customSizeToken(size));
            persistRecentFiles();
        };

        for (const TemplateFile& t : files) {
            NewDocumentCard card;
            card.path = canonicalPathOf(t.path.string());
            card.title = t.name;
            // Preview + dims come straight from the file (PRVW + manifest). A pre-PRVW
            // template shows placeholder art until it is next saved -- the full-open-and-
            // composite fallback died with the thumbnail cache (too costly per dialog open).
            fillCardFromFile(card);
            card.subtitle = card.detail; // templates surface their dims as the card subtitle
            card.detail.clear();
            ctx.templates.push_back(std::move(card));
        }
        return ctx;
    }

    // Every open document's title -- the live one plus each parked session's (nextUntitledTitle
    // steps past them so a fresh document never repeats an open tab's name).
    [[nodiscard]] std::vector<std::string> openDocumentTitles() const {
        std::vector<std::string> titles;
        if (m_document)
            titles.push_back(m_document->title());
        for (const DocumentSession& s : m_sessions) {
            if (s.doc)
                titles.push_back(s.doc->title());
        }
        return titles;
    }

    // Remember a created Blank spec's size as a "Sizes" card when it matches no preset --
    // hand-entered sizes tend to recur (round 5). Exact repeats just move to the front.
    void recordRecentSize(const NewDocumentSpec& spec) {
        if (matchDocumentPreset(spec) >= 0)
            return;
        common::pushRecentFile(m_recentSizes, customSizeToken(spec), common::kMaxRecentSizes);
        persistRecentFiles();
    }

    // Instantiate a template: the document loads from the file but binds to NOTHING -- untitled,
    // unsaved, no path/lock/anchor -- so saving it can never write back into the template.
    void openTemplateAtPath(const std::string& path, const std::string& title) {
        std::vector<std::uint8_t> bytes;
        // (void): an unreadable file leaves `bytes` empty and the container reader below reports
        // the damage through the recovery flows -- exactly what the previous istreambuf_iterator
        // slurp did on a failed open. Preserved deliberately; wiring the bool into those flows is
        // a behaviour change, not a read-speed one.
        (void)common::readWholeFile(path, bytes);
        if (bytes.empty()) {
            tellError(_("Could not open the template"), _("The file could not be read."), path);
            return;
        }
        std::string err;
        auto res = io::native::documentFromReport(io::native::openDocument(bytes), &err);
        if (!res.has_value()) {
            uiLog().warn("template open failed for \"{}\": {}", path, err);
            tellError(_("Could not open the template"), err, path);
            return;
        }
        res->document->setTitle(title.empty() ? std::string("Untitled") : title);
        uiLog().info("new document from template \"{}\": {}x{}", path, res->document->width(),
                     res->document->height());
        presentDocument(std::move(res->document), /*fitView=*/true);
        beginJournal(nullptr); // untitled crash protection, the File->New convention
    }

    // New from Clipboard (the pre-fetched image): a document sized to it, the image as its
    // first (unlocked) layer -- the "opened an image" convention, not File->New's Background.
    void createDocumentFromClipboard(common::Image rgba, const std::string& title) {
        if (rgba.empty()) {
            transientStatus(_("The clipboard no longer holds an image"));
            return;
        }
        const auto docW = std::min<std::uint32_t>(rgba.width, kMaxCanvasDimension);
        const auto docH = std::min<std::uint32_t>(rgba.height, kMaxCanvasDimension);
        auto doc = std::make_unique<core::Document>(docW, docH);
        doc->setTitle(title.empty() ? std::string("Untitled") : title);
        std::unique_ptr<core::RasterLayer> layer =
            doc->makeRaster("Layer 1", rgba.width, rgba.height);
        layer->image() = std::move(rgba);
        doc->root().addOnTop(std::move(layer));
        uiLog().info("new document from clipboard: {}x{}", doc->width(), doc->height());
        presentDocument(std::move(doc), /*fitView=*/true);
        beginJournal(nullptr);
    }

    // File->New (S9, redesigned with S55): the dialog returns a CHOICE -- a blank spec, a
    // template to instantiate, a recent file to open, or "from clipboard" -- and this dispatches
    // each. The blank path is seeded from (and remembers) the last choice.
    void newDocument() {
        std::optional<common::Image> clipboard = fetchClipboardImage(0.5);
        // The offered Name auto-increments past every open document's title -- two fresh
        // documents never share a tab title (round 5). Size/colour still seed from last time.
        NewDocumentSpec seed = m_lastNewSpec;
        seed.title = nextUntitledTitle(openDocumentTitles());
        const std::optional<NewDocumentChoice> chosen =
            showNewDocumentDialog(buildNewDocumentContext(clipboard), &seed, this);
        if (!chosen)
            return; // cancelled
        switch (chosen->kind) {
        case NewDocumentChoice::Kind::Blank: {
            m_lastNewSpec = chosen->spec;
            recordRecentSize(chosen->spec);
            std::unique_ptr<core::Document> doc = buildDocument(chosen->spec);
            uiLog().info("new document \"{}\": {}x{}, {}, {}", doc->title(), doc->width(),
                         doc->height(), core::colorSpaceName(doc->colorSpace()),
                         core::precisionName(doc->precision()));
            presentDocument(std::move(doc), /*fitView=*/true);
            beginJournal(nullptr); // untitled crash protection: a self-contained journal (2.6)
            break;
        }
        case NewDocumentChoice::Kind::Template:
            openTemplateAtPath(chosen->path, chosen->spec.title);
            break;
        case NewDocumentChoice::Kind::RecentFile:
            openRecentPath(chosen->path); // self-heals the list if the file vanished meanwhile
            break;
        case NewDocumentChoice::Kind::Clipboard:
            // The image was pre-fetched for the dialog's card, so creation is synchronous.
            createDocumentFromClipboard(clipboard.has_value() ? std::move(*clipboard)
                                                              : common::Image{},
                                        chosen->spec.title);
            break;
        }
    }

    // File->Open (S18-b): pick a PNG/JPEG, decode it (io::loadImage) into a single-raster-layer
    // document, and show it. The lone layer is a normal, UNLOCKED raster named after the file --
    // you opened the image to edit it, so it must be paintable immediately (unlike File->New's
    // conventional locked Background). Save/Export are a later S18-b slice.
    void openDocument() {
        // The XDG portal picker (platform::showOpenDialog) is the desktop's real Open dialog on
        // X11/XWayland + Wayland + GNOME + KDE -- the fix for the wrong-looking GTK picker FLTK
        // otherwise lands on (Export & I/O plan, section 8).
        platform::FileDialogRequest req;
        req.title = _("Open");
        req.parent = this;
        req.startFolder = documentStartFolder();
        // ONE type covering everything Open can take -- images AND documents -- plus the "All
        // files" escape hatch. No per-kind refinements: they only restated subsets of the default
        // and cluttered the picker (user feedback 2026-07-16). Globs ONLY, deliberately: .mosaic
        // has no registered mime type, and KDE's picker filters by the mime list INSTEAD of the
        // globs when a filter carries both (see FileFilter in platform/file_dialog.hpp) -- an
        // image-mime list here made the .mosaic files vanish from the combined type.
        std::vector<std::string> openGlobs = openableImageGlobs();
        openGlobs.emplace_back("*.mosaic");
        req.filters = {{_("Images, Mosaic documents"), std::move(openGlobs), {}},
                       {_("All files"), {"*"}, {}}};
        const std::optional<std::string> picked = platform::showOpenDialog(req);
        if (!picked)
            return; // cancelled
        openDocumentAtPath(*picked);
    }

    // Surface a failure as a Warning-faced AskOrTell "tell" (docs/askortell-dialog.md) -- the
    // retired generic error dialog's successor. `details` (a path, a raw decoder/io error) joins
    // the body as its own paragraph and adds a "Copy details" button so a bug report keeps the
    // exact text. Blocks until dismissed; centred over this window, so it opens on the display
    // the app is actually on.
    void tellError(std::string_view title, std::string_view message,
                   std::string_view details = {}) {
        AskOrTellDialog::Stage st;
        st.icon = AskOrTellDialog::Icon::Warning;
        st.title = std::string(title);
        st.message = std::string(message);
        if (details.empty()) {
            st.buttons = {_("OK")};
        } else {
            st.message += "\n\n";
            st.message += details;
            st.buttons = {_("Copy details"), _("OK")};
            st.cancelButton = 1; // Escape/WM-close = OK; the copy is an action, not an exit
        }
        AskOrTellDialog dlg;
        dlg.present(st, this);
        while (dlg.run() == 0 && !details.empty()) {
            // "Copy details" stays up: copy, then re-present the SAME face to re-arm run().
            Fl::copy(details.data(), static_cast<int>(details.size()), /*clipboard=*/1);
            dlg.present(st, this);
        }
        dlg.hide();
    }

    // Does `path` carry the .mosaic preamble? Dispatch is by CONTENT, not by extension, matching
    // io::loadImage's sniff-first convention -- a renamed .mosaic still opens as a document.
    [[nodiscard]] static bool looksLikeMosaicDocument(const std::string& path) {
        std::ifstream probe(path, std::ios::binary);
        std::array<char, io::native::kPreambleSize> head{};
        probe.read(head.data(), static_cast<std::streamsize>(head.size()));
        return probe.gcount() == static_cast<std::streamsize>(head.size()) &&
               io::native::parsePreamble(reinterpret_cast<const std::uint8_t*>(head.data()),
                                         head.size())
                   .has_value();
    }

    // S50: place `path` as a MAGIC layer on the active document. The original pixels are kept whole
    // and the compositor resamples them from the source through the layer transform on every frame
    // (render/compositor.cpp), so repeated scale/rotate never compound the loss a baked raster
    // would. File->Open as Layer and a canvas file-drop both land here.
    //
    // A .mosaic is NOT a layer, so one dropped on the canvas opens as a document instead. (Placing
    // a whole document as a layer is the "linked/embedded document" feature; the model can carry it
    // -- MagicLayer keeps a source image -- but nothing flattens a .mosaic to one yet.)
    void addMagicLayerFromPath(const std::string& path) {
        if (!m_document || m_saveJob || m_waitingForSave)
            return; // quiescence: an edit mid-background-save would outrun the snapshot
        if (looksLikeMosaicDocument(path)) {
            openDocumentAtPath(path);
            return;
        }
        std::string err;
        std::optional<io::LoadedImage> img = io::loadImageWithMetadata(path, &err);
        if (!img) {
            uiLog().warn("open as layer failed for \"{}\": {}", path, err);
            tellError(_("Could not open the image"), err, path);
            return;
        }
        std::string name = std::filesystem::path(path).stem().string();
        if (name.empty())
            name = _("Placed image");

        const std::uint32_t srcW = img->image.width;
        const std::uint32_t srcH = img->image.height;
        std::unique_ptr<core::MagicLayer> layer =
            m_document->makeMagic(name, std::move(img->image));
        layer->setExif(std::move(img->exif)); // camera metadata rides the layer (EXIF READ slice)
        // Fit-and-centre. The layer TRANSFORM is all that is needed: the source stays whole at full
        // resolution and the compositor keeps resampling from it, so the placement is free to undo
        // and a later scale never compounds with this one.
        layer->setTransform(
            core::placedImageTransform(srcW, srcH, m_document->width(), m_document->height()));
        const core::LayerId newId = layer->id();
        // Above the active layer (the paste rule), or on top of the root stack.
        core::LayerId parentId = m_document->root().id();
        std::size_t index = m_document->root().childCount();
        if (m_layerPanel != nullptr) {
            if (const std::optional<core::Document::Location> loc =
                    m_document->locate(m_layerPanel->activeLayer())) {
                parentId = loc->parent->id();
                index = loc->index + 1;
            }
        }
        uiLog().info("placed \"{}\" as a magic layer: {}x{}", path, srcW, srcH);
        m_document->commands().push(
            std::make_unique<core::AddLayerCommand>(parentId, index, std::move(layer)));
        syncAfterEdit();
        if (m_layerPanel)
            m_layerPanel->setActive(newId); // refresh() ran via syncAfterEdit
    }

    // File->Open as Layer... -- the picker's twin of a canvas drop.
    void openAsLayer() {
        if (!m_document)
            return; // nothing to place it on; the menu item greys with the document
        platform::FileDialogRequest req;
        req.title = _("Open as Layer");
        req.parent = this;
        req.startFolder = documentStartFolder();
        // Globs only, and no mime list -- same reason the Open filter above carries none: KDE's
        // picker filters by the mime list INSTEAD of the globs when a filter has both, so a
        // png/jpeg mime pair would hide every WebP/AVIF/TIFF/GIF the globs just added.
        req.filters = {{_("Images"), openableImageGlobs(), {}}, {_("All files"), {"*"}, {}}};
        if (const std::optional<std::string> picked = platform::showOpenDialog(req))
            addMagicLayerFromPath(*picked);
    }

    // Which widget owns a file drag at the pointer (S50), or null for the chrome fallback (the
    // window itself accepts and opens the files as new tabs). The tab strip wins
    // where it overlaps nothing -- it sits above the canvas -- and is skipped while hidden (a single
    // open document), where its rect is stale and belongs to the canvas. Called with the pointer in
    // main-window coordinates, which is the space both children's x()/y() live in.
    [[nodiscard]] Fl_Widget* fileDropTargetUnderPointer() {
        if (m_tabStrip != nullptr && m_tabStrip->visible() != 0 && Fl::event_inside(m_tabStrip))
            return m_tabStrip;
        if (m_canvas != nullptr && Fl::event_inside(m_canvas))
            return m_canvas;
        return nullptr;
    }

    // A file drop on the CANVAS: each file becomes a magic layer, in drop order (S50).
    void onCanvasFilesDropped(const std::vector<std::string>& paths) {
        for (const std::string& p : paths)
            addMagicLayerFromPath(p);
    }

    // A file drop on the TAB STRIP: each file opens as its own document (S50).
    void onTabStripFilesDropped(const std::vector<std::string>& paths) {
        for (const std::string& p : paths)
            openDocumentAtPath(p);
    }

    // Open `path` and show it -- the shared tail of the Open dialog and an empty-state file
    // drop. Dispatch is by CONTENT, matching io::loadImage's sniff-first convention: a .mosaic
    // preamble routes to the native document path (so a renamed file still opens correctly);
    // anything else decodes as an image into a single-raster-layer document.
    void openDocumentAtPath(const std::string& path) {
        // Quiescence for the background save (design note): presentDocument/activateSession
        // swap the document + anchor + journal the running job's finalize will adopt into.
        // Every other route is blocked by the disabled controls; this one is reachable from
        // DND/paste, so it refuses on its own too.
        if (m_saveJob || m_waitingForSave) {
            transientStatus(_("Saving -- finishing the save first"));
            return;
        }
        // Already open? Raise its tab rather than making a second copy (S49). Every tabbed editor
        // does this, and here it also stops a file being opened READ-ONLY against this window's own
        // advisory lock -- which is exactly what a naive second open of the same path would find.
        if (const std::optional<std::size_t> existing = sessionForPath(path)) {
            activateSession(*existing);
            return;
        }
        if (looksLikeMosaicDocument(path)) {
            openMosaicAtPath(path);
            return;
        }
        std::string err;
        std::optional<io::LoadedImage> img = io::loadImageWithMetadata(path, &err);
        if (!img) {
            uiLog().warn("open failed for \"{}\": {}", path, err);
            tellError(_("Could not open the image"), err, path);
            return;
        }
        std::string name = std::filesystem::path(path).stem().string();
        if (name.empty())
            name = "Untitled";

        auto doc = std::make_unique<core::Document>(img->image.width, img->image.height);
        doc->setTitle(name);
        doc->setFilePath(path); // "loaded from" -- the window title shows the filename + extension
        std::unique_ptr<core::RasterLayer> layer =
            doc->makeRaster(name, img->image.width, img->image.height);
        layer->image() = std::move(img->image);
        layer->setExif(std::move(img->exif)); // camera metadata rides the layer (EXIF READ slice)
        doc->root().addOnTop(std::move(layer));
        uiLog().info("opened \"{}\": {}x{}", path, doc->width(), doc->height());
        presentDocument(std::move(doc), /*fitView=*/true);
        // A successful open joins the recent list; its dialog card reads the desktop's shared
        // thumbnail cache (plain images carry no embedded preview).
        recordRecentFile(canonicalPathOf(path));
    }

    // The raw owner id a tile/vector chunk KEY carries (LE64 at byte 0). A mask surface sets bit
    // 63 (io::native::kMaskSurfaceBit); the layer id below is that value with the bit cleared.
    static std::uint64_t rawOwnerOfKey(const io::native::ChunkKey& k) {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<std::uint64_t>(k.bytes[static_cast<std::size_t>(i)]) << (8 * i);
        return v;
    }

    // The OWNING layer id (mask bit stripped) -- turns salvage's flagged keys into human names.
    static std::uint64_t layerIdOfKey(const io::native::ChunkKey& k) {
        return rawOwnerOfKey(k) & ~io::native::kMaskSurfaceBit;
    }

    // " Affected: the Sky layer." / "... the Sky and Hills layers." -- the unique layers the given
    // salvage flags touch, resolved against `doc`. Empty when nothing resolves, so the caller
    // simply omits the sentence rather than printing a bare "Affected: .".
    static std::string affectedSentence(const core::Document& doc,
                                        const std::vector<io::native::DirtyKey>& keys) {
        std::vector<std::string> names;
        for (const auto& dk : keys) {
            const core::Layer* l = doc.find(layerIdOfKey(dk.key));
            if (l != nullptr && std::find(names.begin(), names.end(), l->name()) == names.end())
                names.push_back(l->name());
        }
        if (names.empty())
            return {};
        std::string joined;
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i > 0)
                joined += (i + 1 == names.size()) ? _(" and ") : ", ";
            joined += names[i];
        }
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      names.size() == 1 ? _(" Affected: the %s layer.")
                                        : _(" Affected: the %s layers."),
                      joined.c_str());
        return buf;
    }

    // Build a document from the checkpoint plus ONE salvage lineage's recovered chunks -- the
    // "recovered version" a flow-3c / flow-4 choice opens. documentFromReport composes base +
    // committed by highest generation, so feeding it a lineage's chunks yields exactly that
    // lineage's document (the conservative open is just this fed report.committed).
    static std::optional<io::native::DocumentReadResult>
    documentFromLineage(const io::native::OpenReport& report,
                        const io::native::SalvageLineage& lineage, std::string* err) {
        io::native::OpenReport synth = report;
        synth.committed = lineage.chunks;
        synth.commits = lineage.states;
        synth.committedAnomaly = false;
        return io::native::documentFromReport(synth, err);
    }

    // Flow 4: write the OTHER writer's recovered version to a path the user picks. It never
    // becomes m_document (writer A opens); this is a plain full write of a rebuilt foreign doc.
    void saveOtherVersionAs(core::Document& doc, const std::string& originalPath) {
        std::string stem = std::filesystem::path(originalPath).stem().string();
        if (stem.empty())
            stem = "untitled";
        platform::FileDialogRequest req;
        req.title = _("Save other version as");
        req.acceptLabel = _("Save");
        req.suggestedName = stem + " (other version).mosaic";
        req.parent = this;
        req.startFolder = io::exportStartFolder(io::documentPathInputs(originalPath));
        req.filters = {{_("Mosaic document"), {"*.mosaic"}, {}}, {_("All files"), {"*"}, {}}};
        const std::optional<std::string> chosen = platform::showSaveDialog(req);
        if (!chosen)
            return; // cancelled -- writer A still opens
        std::string p = *chosen;
        {
            std::string ext = std::filesystem::path(p).extension().string();
            for (char& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".mosaic")
                p += ".mosaic";
        }
        if (doc.uuid().empty())
            doc.setUuid(io::native::mintDocumentUuid());
        std::string err;
        const std::optional<common::Image> preview = compositeForPreview(doc);
        const auto input = io::native::buildDocumentCheckpoint(
            doc, &err, preview.has_value() ? &*preview : nullptr);
        if (!input.has_value() ||
            !io::native::writeFileAtomic(p, io::native::buildCheckpoint(*input), &err)) {
            tellError(_("Could not save the other version"), err, p);
            return;
        }
        char buf[256];
        std::snprintf(buf, sizeof buf, _("Saved the other version to %s"),
                      std::filesystem::path(p).filename().string().c_str());
        transientStatus(buf);
    }

    // A label for one loaded history step: the layer(s) whose content the save at `generation`
    // changed, resolved against `doc`. "Edited Sky" / "Edited Sky, Shapes" / a generic fallback.
    std::string changedLayerLabel(const core::Document& doc, const LoadedState& state) const {
        std::vector<std::string> names;
        for (const LoadedChunk& c : state.chunks) {
            if (c.type != io::native::kTypeTile && c.type != io::native::kTypeVector)
                continue;
            const core::Layer* l = doc.find(layerIdOfKey(c.key));
            if (l != nullptr && std::find(names.begin(), names.end(), l->name()) == names.end())
                names.push_back(l->name());
        }
        if (names.empty())
            return _("Saved change");
        std::string joined = names[0];
        for (std::size_t i = 1; i < names.size(); ++i)
            joined += ", " + names[i];
        char buf[256];
        std::snprintf(buf, sizeof buf, _("Edited %s"), joined.c_str());
        return buf;
    }

    // Load the file's save history into `doc`'s command stack so the History panel shows it and can
    // jump between saves (S48, spec 3.5 LiveUndoModel). The document already sits at its newest
    // saved state; buildLoadedHistory turns each earlier save into one pre-applied command (a
    // per-key LoadedDeltaCommand for a content-only save, the whole-tree LoadedStateCommand for a
    // structural/mask one) that moves the document between adjacent saves. Empty -> nothing to
    // adopt (no readable history, or a reconstruction failure -- history is a bonus).
    //
    // Returns FALSE when the file has a save history that could not be read (a rotted retained
    // frame). That is not the same as a file with no history: the panel ends up empty either way,
    // but only one of them lost something, and only one of them is worth telling the user about.
    bool loadCommittedHistory(core::Document& doc, const io::native::OpenReport& report) {
        // Name each step by the layers it touched, resolved from the same state list
        // buildLoadedHistory walks -- so a COMPACTED file, whose saves live in the checkpoint's
        // retained history rather than the committed region, still names its steps.
        const auto states = loadedStates(report);
        if (!states.has_value())
            return false;
        std::map<std::uint64_t, std::string> labels;
        for (const LoadedState& st : *states)
            labels[st.generation] = changedLayerLabel(doc, st);
        auto history = buildLoadedHistory(report, [&](std::uint64_t gen) {
            const auto it = labels.find(gen);
            return it == labels.end() ? std::string(_("Saved change")) : it->second;
        });
        if (!history.empty())
            doc.commands().adoptHistory(std::move(history));
        return true;
    }

    // Open a native .mosaic document (S48): the container's conservative open (checkpoint ladder +
    // committed-region replay), then the document bridge. When the reader reports damage, the
    // settled recovery dialogs fire (docs/askortell-dialog.md flows 3a-3e + 4): most are TELLS
    // (nothing to choose); flow 3c and flow 4 are ASKS whose choice picks which document opens
    // (and, for 4, saves the other writer's version off first). Journal flows (1/2) arrive with
    // the journal-autosave slice; the conservative open is the safe default under all of them.
    void openMosaicAtPath(const std::string& path) {
        std::vector<std::uint8_t> bytes;
        // (void) for the reason openTemplateAtPath gives: an empty buffer is how a failed
        // open has always reached the reader, and the recovery flows below read it there.
        (void)common::readWholeFile(path, bytes);
        if (bytes.empty()) {
            tellError(_("Could not open the document"), _("The file could not be read."),
                            path);
            return;
        }
        namespace nio = io::native;
        const nio::OpenReport report = nio::openDocument(bytes);
        std::string err;
        auto conservative = nio::documentFromReport(report, &err);
        if (!conservative.has_value()) {
            uiLog().warn("mosaic open failed for \"{}\": {}", path, err);
            tellError(_("Could not open the document"), err, path);
            return;
        }
        conservative->document->setFilePath(path);
        const std::string name = std::filesystem::path(path).filename().string();
        uiLog().info("opened mosaic \"{}\": {}x{}, {} layers, commits={}, retained={}, repaired={}, "
                     "lost={}, lostHistory={}, rejected={}, anomaly={}, rootFound={}, fullScan={}, "
                     "formatVersion={}",
                     path, conservative->document->width(), conservative->document->height(),
                     conservative->document->layerCount(), report.commits.size(),
                     report.base.retained.size(), report.base.rsReconstructed,
                     report.base.lostEntries, report.base.lostHistoryEntries,
                     conservative->rejectedChunks, report.committedAnomaly, report.base.rootFound,
                     report.base.usedFullScan, report.base.formatVersion);

        const std::size_t repaired = report.base.rsReconstructed;
        // Retained-history loss is deliberately NOT folded in here: history carries no parity by
        // design (spec 3.8), so a rotted undo state is unrepairable and permanent. Raising the "this
        // file is damaged" face over a document whose content is byte-perfect would train the user
        // to dismiss it. It gets a status line instead, below.
        const std::size_t checkpointLost = report.base.lostEntries + conservative->rejectedChunks;

        // §2.10 advisory lock (flow 6): is this document already open in another Mosaic window? Ask
        // BEFORE presenting anything -- Cancel abandons the open entirely; "Open read-only" proceeds
        // without the lock or a journal (the other window keeps sole write authority). Checked here,
        // once the uuid is known, on a lock file in the recovery dir -- never the user's document.
        bool readOnly = false;
        std::optional<nio::AdvisoryLock> lock;
        if (!conservative->document->uuid().empty()) {
            const std::string canonical = canonicalPathOf(path);
            const std::string lockPath =
                nio::recoveryLockPath(conservative->document->uuid(), canonical);
            std::string lerr;
            const auto status = nio::AdvisoryLock::tryAcquire(lockPath, lock, &lerr);
            if (status == nio::AdvisoryLock::Status::Busy) {
                char body[512];
                std::snprintf(body, sizeof body,
                              _("Another Mosaic window already has \"%s\" open. Opening it here too "
                                "could let the two windows overwrite each other's work.\n\nYou can "
                                "open it read-only to look at it -- saving will ask for a new file."),
                              name.c_str());
                AskOrTellDialog dlg;
                const int choice =
                    dlg.ask({AskOrTellDialog::Icon::Question,
                             _("This document is already open in another Mosaic window"), body,
                             {_("Cancel"), _("Open read-only")}, /*cancelButton=*/0},
                            this);
                if (choice != 1)
                    return; // Cancel (also the Escape default): do not open at all
                readOnly = true;
            } else if (status == nio::AdvisoryLock::Status::Error) {
                uiLog().warn("lock: could not acquire \"{}\": {} (opening without a lock)", lockPath,
                             lerr);
            }
        }

        // A committed-region anomaly warrants a read-only salvage probe (spec 2.8) -- it is what
        // tells flows 3c/3d/4 apart. Everywhere else the reader's own report is enough, so the
        // probe is skipped. classifyRecoveryFlow (headlessly tested) maps facts -> flow; this
        // method only renders the chosen face and carries out the choice.
        nio::SalvageReport sv;
        if (report.committedAnomaly) {
            std::array<std::uint8_t, nio::kLinkSize> seed{};
            for (std::size_t i = 0; i < nio::kLinkSize; ++i)
                seed[i] = report.base.rootChecksum[i];
            sv = nio::salvageLinkedRegion(
                bytes, static_cast<std::size_t>(report.base.walStartOffset), seed);
        }
        const RecoveryFlow flow =
            classifyRecoveryFlow(report, checkpointLost, report.committedAnomaly ? &sv : nullptr);

        // Which document the user ends up looking at: the conservative open unless a flow-3c
        // choice swaps in a recovered lineage. Each case sets this + shows at most one dialog;
        // presentDocument runs once at the end. recoveredView marks the one path whose states no
        // longer match `report` (the salvage lineage), so its committed history is not loaded.
        std::unique_ptr<core::Document> toShow;
        bool recoveredView = false;
        char body[1024];

        switch (flow) {
        case RecoveryFlow::BadlyDamaged: { // 3e -- no clean fallback: a tell that QUANTIFIES what
            // came back. Full-scan reassembly often looks perfect on open (all content found), so
            // the face has to name what was actually lost -- the index and the save history --
            // rather than leave the user thinking nothing happened.
            const std::size_t layers = conservative->document->layerCount();
            char lost[160] = "";
            if (checkpointLost > 0)
                std::snprintf(lost, sizeof lost, _(", and %zu area(s) could not be read"),
                              checkpointLost);
            std::snprintf(body, sizeof body,
                          _("The file's structure was destroyed. Mosaic rebuilt the document from "
                            "the pieces it could find -- %zu layer(s) recovered%s. The file's index "
                            "and its save history are gone, so what you see may be older than the "
                            "last save.\n\nNothing is written to your file until you save. Saving a "
                            "copy now protects what was found."),
                          layers, lost);
            AskOrTellDialog dlg;
            dlg.ask({AskOrTellDialog::Icon::CorruptFile, _("This file is badly damaged"), body,
                     {_("Open")}},
                    this);
            toShow = std::move(conservative->document);
            break;
        }
        case RecoveryFlow::DualWriter: { // 4 -- two writers saved into one file, both intact.
            AskOrTellDialog dlg;
            const int choice = dlg.ask(
                {AskOrTellDialog::Icon::Warning, _("Two programs saved into this file"),
                 _("This file holds two separate save histories -- it was open and saved in two "
                   "places at once. Both survived intact.\n\nMosaic will open the version that was "
                   "saved first. To keep the second one too, save it as its own file now -- a "
                   "later save may discard it."),
                 {_("Save other version as..."), _("Open")}, AskOrTellDialog::kCancelNone},
                this);
            if (choice == 0) {
                // The "other version" = the second seed-rooted lineage (primary is writer A).
                const nio::SalvageLineage* other = nullptr;
                bool sawPrimary = false;
                for (const auto& ln : sv.lineages) {
                    if (!ln.seedRooted)
                        continue;
                    if (!sawPrimary) {
                        sawPrimary = true;
                        continue;
                    }
                    other = &ln;
                    break;
                }
                std::string berr;
                auto otherDoc = other ? documentFromLineage(report, *other, &berr) : std::nullopt;
                if (otherDoc.has_value())
                    saveOtherVersionAs(*otherDoc->document, path);
                else
                    tellError(_("Could not rebuild the other version"), berr, path);
            }
            toShow = std::move(conservative->document); // writer A either way
            break;
        }
        case RecoveryFlow::Recover: { // 3c -- committed damage with intact saves past it: the ASK.
            // Honest counts only: how many newer saves salvage brought back, and how many areas it
            // couldn't read (they fall back to older content). The salvage lineage reports
            // recovered states + a flat flagged-key list, with no per-state attribution, so we do
            // NOT claim an "X of Y saves intact" split we cannot verify.
            const nio::SalvageLineage* prim = sv.primary();
            const std::string affected = affectedSentence(*conservative->document, prim->flagged);
            const char* imprecise =
                prim->precise ? ""
                              : _(" Part of the record was destroyed, so this list may be "
                                  "incomplete.");
            std::snprintf(body, sizeof body,
                          _("Part of \"%s\" can't be read.\n\nMosaic can open it as it was at the "
                            "last complete save, or open the recovered version: %llu newer save(s) "
                            "came back, and %zu area(s) that couldn't be read show older content "
                            "instead.%s%s\n\nNothing is written to your file until you save."),
                          name.c_str(), static_cast<unsigned long long>(prim->states.size()),
                          prim->flagged.size(), affected.c_str(), imprecise);
            AskOrTellDialog dlg;
            const int choice =
                dlg.ask({AskOrTellDialog::Icon::CorruptFile, _("This file is damaged"), body,
                         {_("Open recovered version"), _("Open last complete save")},
                         /*cancelButton=*/1},
                        this);
            if (choice == 0) {
                std::string rerr;
                auto rec = documentFromLineage(report, *prim, &rerr);
                if (rec.has_value()) {
                    rec->document->setFilePath(path);
                    toShow = std::move(rec->document);
                    recoveredView = true; // salvaged states != report.commits: skip history load
                } else {
                    tellError(_("Could not open the recovered version"), rerr, path);
                }
            }
            if (!toShow) // the conservative choice, its Escape, or a failed rebuild
                toShow = std::move(conservative->document);
            break;
        }
        case RecoveryFlow::TornTail: { // 3d -- unfinished save at the tail, nothing beyond: a tell.
            std::snprintf(body, sizeof body,
                          _("\"%s\" ends in an unfinished save -- whatever was writing it stopped "
                            "partway. The unfinished part was set aside; this file opens at the "
                            "last complete save."),
                          name.c_str());
            AskOrTellDialog dlg;
            dlg.ask({AskOrTellDialog::Icon::Warning, _("The last save didn't finish"), body,
                     {_("Open")}},
                    this);
            toShow = std::move(conservative->document);
            break;
        }
        case RecoveryFlow::Damaged: { // 3b -- checkpoint areas lost beyond parity: a tell.
            std::snprintf(body, sizeof body,
                          _("%zu areas of \"%s\" can't be read, even after repair. Where an "
                            "earlier version survived in the file's history it is shown in its "
                            "place; anything else is left blank.\n\nNothing is written to your "
                            "file until you save. Saving a copy now protects everything that "
                            "remains."),
                          checkpointLost, name.c_str());
            AskOrTellDialog dlg;
            dlg.ask({AskOrTellDialog::Icon::CorruptFile, _("This file is damaged"), body,
                     {_("Open")}},
                    this);
            toShow = std::move(conservative->document);
            break;
        }
        case RecoveryFlow::Repaired: // 3a -- status line only (below); no modal (crying wolf).
        case RecoveryFlow::None:
            toShow = std::move(conservative->document);
            break;
        }

        // Surface the file's save history in the History panel (S48). Skipped for the recovered
        // view (its states are the salvage lineage, not the file's own saves) -- that path shows
        // the recovered document without a reconstructed history.
        const bool historyReadable = recoveredView || loadCommittedHistory(*toShow, report);

        presentDocument(std::move(toShow), /*fitView=*/true);
        // Install the lock this open acquired (empty when read-only or lock-less). presentDocument
        // released the previous document's lock just above, so this cannot clobber a live one.
        m_lock = std::move(lock);
        m_documentReadOnly = readOnly;

        // Every successful open -- read-only and recovered included -- joins the recent list;
        // the dialog card reads the file's own embedded PRVW (S48-b).
        recordRecentFile(canonicalPathOf(path));

        // 3a -- parity silently fixed things and nothing was lost: a status line, not a modal. Lost
        // undo states ride the same line: the document is whole, only its past is shorter, and that
        // is worth saying once without a dialog. When the loss took the whole walk down with it
        // (a state's undo target is the very frame that rotted, so a partial history would move the
        // document to content that state never held), say THAT -- not a count that undersells it.
        const std::size_t lostHistory = report.base.lostHistoryEntries;
        const bool tellHistoryLoss = lostHistory > 0 || !historyReadable;
        if (flow == RecoveryFlow::Repaired || (flow == RecoveryFlow::None && tellHistoryLoss)) {
            char hist[160] = "";
            if (!historyReadable)
                std::snprintf(hist, sizeof hist, "%s",
                              _("this file's save history could not be read"));
            else if (lostHistory > 0)
                std::snprintf(hist, sizeof hist,
                              _("%zu earlier undo state(s) could not be read"), lostHistory);
            char buf[256];
            if (repaired > 0 && tellHistoryLoss)
                std::snprintf(buf, sizeof buf, _("Repaired %zu damaged block(s); %s"), repaired,
                              hist);
            else if (repaired > 0)
                std::snprintf(buf, sizeof buf, _("Repaired %zu damaged block(s) while opening"),
                              repaired);
            else
                std::snprintf(buf, sizeof buf, "%s", hist);
            transientStatus(buf);
        }

        if (readOnly) {
            // A borrowed open: the other window owns the lock and the journal. No crash protection
            // here, and Save routes to Save As (saveDocument enforces it) -- never overwrite the
            // file the other window is writing.
            transientStatus(_("Opened read-only -- this document is open in another window"));
            return;
        }

        // Crash restore (flows 1/2): a recovery journal from a previous unclean exit, composed
        // after any damage face. On restore it re-presents the restored document with its own
        // journal; otherwise it leaves the conservative/recovered document shown and we start a
        // fresh one -- incremental for the conservative open, self-contained for a salvaged view
        // (its states no longer match the file's committed region).
        if (!offerJournalRestore(path, report)) {
            beginJournal(recoveredView ? nullptr : &report);
            // Arm commit-append for a clean open: the conservative document IS the file's newest
            // committed state, so a File->Save appends just the delta (spec 2.6). A recovered/
            // salvaged view (recoveredView) does not match the committed region -- leave the anchor
            // empty so its next Save is a full write. setCommitAnchor no-ops on a full-scan open
            // (no tip to append onto). A restore (the true branch) re-anchors on its next full save.
            if (!recoveredView)
                setCommitAnchor(report, path);
        }
    }

    // The composite every save path hands buildDocumentCheckpoint as the PRVW source (S48-b).
    // The app owns the compositor -- io must not depend on render -- so the image travels down
    // as a parameter. CPU backend, real alpha (no checkerboard): deterministic for unchanged
    // content, so the differ can skip an unchanged downscale byte-for-byte. Best-effort: a
    // failed composite just means this save writes no preview (never a failed save), and the
    // file keeps the one it has.
    static std::optional<common::Image> compositeForPreview(const core::Document& doc) {
        // ⚠ compositeScaled, NOT composite-then-downscale. makePreviewChunk keeps 256 px on the
        // longest edge, so a full-resolution composite builds 39.8 MP to throw ~99.8% of it away --
        // the 31,717:1 shape MOSAIC-PERF.md §1 is entirely about, on the SAVE path, synchronously
        // on the UI thread (the snapshot half of a save is not on the worker). compositing AT the
        // target size is also better filtered: each layer's kernel resolves from its composed
        // placement, so the reduction is a real per-layer minification instead of one box filter
        // over straight-alpha RGBA afterwards.
        //
        // downscalePreview never upscales and returns its input unchanged when the longest edge is
        // already within budget, so handing it this image is a pass-through rather than a second
        // resample. Determinism -- which is what lets the differ skip an unchanged preview
        // byte-for-byte -- is unaffected: the same document still composites to the same bytes.
        const std::uint32_t longest = std::max(doc.width(), doc.height());
        if (longest == 0)
            return std::nullopt;
        const double scale = std::min(1.0, static_cast<double>(io::native::kPreviewEdge) / longest);
        const auto outW = std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>(std::lround(doc.width() * scale)));
        const auto outH = std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>(std::lround(doc.height() * scale)));
        const render::CompositeResult small = render::compositeScaled(
            doc, outW, outH, render::CompositeOptions{}, render::Backend::Cpu);
        if (!small.ok || small.image.empty()) {
            uiLog().warn("preview composite failed: {}", small.error);
            return std::nullopt;
        }
        return small.image;
    }

    // ---- Background full write, types (S48 Build 2, docs/async-save-design.md) ----------------
    //
    // The snapshot (composite + serialize + diff) stays synchronous on the UI thread; the worker
    // owns copies of everything it touches and never reads the document or any UI state. One job
    // at a time, the inpaint pattern: the worker publishes fraction/stage under `mutex`, the UI
    // polls ~20 Hz, and finalizeSaveJob() runs every UI-side consequence (adopt / conflict /
    // fallback) once `done` flips. The members (m_saveJob and friends) live with the rest below.
    enum class SaveJobKind : std::uint8_t {
        PlainWrite,     // buildCheckpoint of a serialized document (no fold)
        CompactionFold, // the threshold/proactive fold of the file at its own path
        SaveAsFold,     // the history-preserving fold into a (possibly different) target
    };
    struct SaveJob {
        SaveJobKind kind = SaveJobKind::PlainWrite;
        std::string sourcePath; // folds: the file whose history is folded through
        std::string targetPath;
        io::native::CommitTip tip; // folds: the late tail check's expectation
        std::vector<io::native::StateChunk> dirty; // folds: the snapshot's diff
        // The snapshot's serialization: the PlainWrite input, and -- for every kind -- the
        // baseline hint finalize rearms the anchor with, so a full write serializes ONCE.
        io::native::CheckpointInput after;

        std::mutex mutex; // guards the published progress below
        float fraction = 0.0f;
        std::string stage; // stage ids ("Saving"/"Writing"), localized UI-side

        std::atomic<bool> done{false};
        // Outcomes, written by the worker before `done`: exactly one of ok / tailStatus!=Ok /
        // foldFailed / error describes what happened.
        bool ok = false;
        bool foldFailed = false; // the fold could not be built: finalize runs the fallback
        io::native::SaveStatus tailStatus = io::native::SaveStatus::Ok;
        std::string error;
        std::vector<std::uint8_t> bytes; // what was written, for adoptWrittenFile
        std::string resultLine;          // one log line composed where the numbers were
        std::thread worker;

        ~SaveJob() { // a save is never cancelled (design note); wait it out
            if (worker.joinable())
                worker.join();
        }
    };
    enum class PendingSave : std::uint8_t { None, Save, SaveAs };

    // File->Save (S48, spec 2.6): commit-append -- append just the delta since the last save when
    // an anchor is armed (the O(changed) common path), else a full write (first save, a full-scan/
    // recovered open, or the compaction threshold). A document backed by a non-.mosaic file (an
    // opened PNG) routes to Save As rather than overwriting the original image with a different
    // format; a read-only (borrowed) open likewise routes to Save As.
    void saveDocument() {
        if (!m_document)
            return;
        // A save while a background save runs coalesces -- the newest request wins after the
        // running one completes (design note). Controls are disabled during a job, so this is
        // the accelerator-vs-worker race, not an ordinary path.
        if (m_saveJob) {
            m_pendingSave = PendingSave::Save;
            return;
        }
        // A read-only (borrowed) open never overwrites its source -- another window owns it (flow
        // 6). Save becomes Save As, which mints a new path + lock + journal for the copy.
        if (m_documentReadOnly) {
            saveDocumentAs();
            return;
        }
        const std::string path = m_document->filePath(); // copy: the save may rebind members
        std::string ext = std::filesystem::path(path).extension().string();
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (path.empty() || ext != ".mosaic") {
            saveDocumentAs();
            return;
        }
        // "Untitled N" takes the file's stem before the manifest serializes (round 7) -- this
        // also brings a legacy auto-titled file into line on its next plain Save.
        adoptTitleFromPathIfUntitled(path);
        // With an anchor armed, an ordinary Save appends just the delta since the last save --
        // O(changed) (spec 2.6). Once the append region's parity debt trips needsCompaction(), THAT
        // Save folds the file instead: a history-preserving full write (compaction.hpp) that carries
        // the older generations through byte-verbatim while the current edit takes a fresh one.
        // Never a background job -- every byte sits behind an explicit Ctrl+S, and no knob.
        //
        // A Save may also fold PROACTIVELY (Build 2 ruling, save_policy.hpp): when the live churn
        // signal says a journal->cas switch would pay -- reuse past switch-up AND an absolute
        // savings floor -- on a file small enough that the fold is never an unacceptable stall,
        // throttled on the signal actually moving between attempts. The parity-debt trigger stays
        // the backstop either way.
        if (m_commit.has_value()) {
            const bool debt =
                io::native::needsCompaction(m_commit->tip.fileSize, m_commit->walStart);
            if ((debt || proactiveFoldWanted()) && compactionSave(path))
                return; // adoptWrittenFile ran noteDocumentSaved
            // A fold that could not be built must NOT silently become a plain full write -- that
            // discards the very history it exists to preserve. Append instead: the parity debt
            // stays, and the next Save tries the fold again.
            if (commitAppendSave(path)) {
                noteDocumentSaved(path); // the one save path that skips adoptWrittenFile
                return;
            }
            // The delta could not be built (serialization failed): fall through to a full write.
        }
        writeMosaicTo(path);
    }

    // Should THIS Save fold proactively (Build 2 ruling)? The decision itself is the pure,
    // headlessly-tested save_policy.hpp; this gathers the anchor's live inputs and records the
    // attempt for the throttle. Precondition: m_commit is armed.
    bool proactiveFoldWanted() {
        if (!m_commit->churn.has_value())
            return false; // no signal (a large file, or a damaged history): passive only
        ProactiveFoldInputs in;
        in.mode = m_commit->mode;
        in.fileSize = m_commit->tip.fileSize;
        in.churnFraction = m_commit->churn->fraction();
        in.projectedSavings = m_commit->churn->projectedSavings();
        in.lastAttemptChurn = m_commit->lastProactiveChurn;
        if (!ui::proactiveFoldWanted(in))
            return false;
        m_commit->lastProactiveChurn = in.churnFraction; // an attempt, whether the fold builds
        uiLog().info("save: proactive fold (churn {:.2f}, ~{} duplicate bytes retained)",
                     in.churnFraction, in.projectedSavings);
        return true;
    }

    // The compaction Save (spec 2.6): fold the committed region back into a fresh checkpoint,
    // preserving its retained history, and commit this Save's edit as the newest state on the way.
    // Returns true when the save was HANDLED (folded, or a conflict surfaced); false asks
    // saveDocument to fall back to an ordinary commit-append. Precondition: m_commit is armed.
    bool compactionSave(const std::string& path) {
        namespace nio = io::native;
        std::string err;
        // The tail check matters MORE here than for an append: a full write replaces the file
        // wholesale, so a foreign writer's committed batches sit inside the very region we are about
        // to fold away. Refuse loudly rather than overwrite them.
        const nio::SaveStatus check = nio::verifyTail(path, m_commit->tip, &err);
        if (check != nio::SaveStatus::Ok) {
            surfaceSaveConflict(path, check, err);
            return true; // handled (the user chose Cancel / Save a copy)
        }
        return foldedWriteTo(path, path, SaveJobKind::CompactionFold);
    }

    // The history-preserving full write (spec 2.6 compaction, 3.3 copy-through, 3.9 adaptive
    // switching): fold the current file's checkpoint + committed region into a fresh checkpoint
    // at `targetPath`, committing this Save's diff as the newest state on the way. The fold
    // measures the document's own churn and picks the history encoding (H2/H4) by hysteresis --
    // this is the passive switch point every full write shares.
    //
    // Only the SNAPSHOT runs here (design note: composite + serialize + diff, the document
    // quiescent); reading, folding, the late tail check, and the atomic write happen on the
    // worker. Returns true when the save is HANDLED (a job is in flight); false asks the caller
    // to fall back right now (nothing serialized, nothing started). Fold failures discovered on
    // the worker run their fallback in finalizeSaveJob. Precondition: m_commit is armed and the
    // caller fast-checked the tail (the authoritative check re-runs on the worker).
    bool foldedWriteTo(const std::string& sourcePath, const std::string& targetPath,
                       SaveJobKind kind) {
        namespace nio = io::native;
        std::string err;
        const std::optional<common::Image> preview = compositeForPreview(*m_document);
        auto after = nio::buildDocumentCheckpoint(*m_document, &err,
                                                  preview.has_value() ? &*preview : nullptr);
        if (!after.has_value()) {
            uiLog().warn("fold: could not serialize \"{}\": {}", sourcePath, err);
            return false;
        }
        auto job = std::make_unique<SaveJob>();
        job->kind = kind;
        job->sourcePath = sourcePath;
        job->targetPath = targetPath;
        job->tip = m_commit->tip;
        // The same tombstone-aware differ Save and the recovery journal share (spec 2.6). An empty
        // diff is fine: the fold still refreshes parity, and no state consumes an id.
        job->dirty = nio::diffDocumentStates(m_commit->baseline, *after);
        job->after = std::move(*after);
        startSaveJob(std::move(job));
        return true;
    }

    // File->Save via commit-append (spec 2.6, Round 12): diff the document against the last durable
    // save and append just that state's dirty chunks + HIST + a closing CMIT -- O(changed), not a
    // full rewrite. Returns true when the save was HANDLED (committed, nothing-to-save, or a
    // conflict surfaced); false asks saveDocument to fall back to a full write. Precondition:
    // m_commit is armed (the on-screen document equals the file's newest committed state).
    bool commitAppendSave(const std::string& path) {
        std::string err;
        const std::optional<common::Image> preview = compositeForPreview(*m_document);
        auto after = io::native::buildDocumentCheckpoint(
            *m_document, &err, preview.has_value() ? &*preview : nullptr);
        if (!after.has_value()) {
            uiLog().warn("save: could not serialize \"{}\" for commit-append: {}", path, err);
            return false; // let the caller do a full write instead
        }
        // The tombstone-aware differ (journal_session): the SAME dirty-set mechanism the recovery
        // journal uses, so Save and autosave agree on what changed (spec 2.6).
        std::vector<io::native::StateChunk> dirty =
            io::native::diffDocumentStates(m_commit->baseline, *after);
        if (dirty.empty()) {
            // Serializes byte-identically to the last save (e.g. edited then undone back): leave the
            // file untouched, just advance the baseline and clear the dirty marker.
            m_commit->baseline = std::move(*after);
            m_document->commands().markSaved();
            transientStatus(savedStatusLine(path));
            return true;
        }
        io::native::SaveState st;
        st.stateId = m_commit->tip.commitId + 1; // only states consume ids; the next generation
        const std::size_t chunkCount = dirty.size();
        st.chunks = std::move(dirty);
        const io::native::SaveStatus status =
            io::native::appendSaveToFile(path, m_commit->tip, {&st, 1}, &err);
        if (status != io::native::SaveStatus::Ok) {
            surfaceSaveConflict(path, status, err); // refuse loudly; the file is untouched
            return true;                            // handled (the user chose Cancel / Save a copy)
        }
        // appendSaveToFile advanced m_commit->tip in place; `after` is now what is durable.
        m_commit->baseline = std::move(*after);
        // The appended state is retained history now: advance the live churn signal by exactly
        // this Save's chunks -- O(changed), the H4 autosave shape (spec 3.9).
        if (m_commit->churn.has_value())
            io::native::addStateToChurn(*m_commit->churn, st.chunks);
        m_document->commands().markSaved();
        // Rebind the recovery journal onto the just-appended commit (spec 2.6 A2: only now that the
        // batch is durable). A synthetic report carries the advanced tip; beginJournal re-derives
        // the incremental binding + baseline from it.
        io::native::OpenReport synth;
        synth.tipValid = true;
        synth.tip = m_commit->tip;
        beginJournal(&synth);
        uiLog().info("committed \"{}\": state {}, {} chunk(s), {} bytes on disk", path, st.stateId,
                     chunkCount, m_commit->tip.fileSize);
        transientStatus(savedStatusLine(path));
        return true;
    }

    // A Save's tail check failed: the file on disk is no longer the one this window last wrote
    // (spec 2.6, Round 13 -- a foreign write under a stale handle, or a real I/O error). Refuse to
    // overwrite; for a conflict, offer to save this window's version as a separate copy.
    void surfaceSaveConflict(const std::string& path, io::native::SaveStatus status,
                             const std::string& err) {
        if (status == io::native::SaveStatus::IoError) {
            tellError(_("Could not save the document"), err, path);
            return;
        }
        const std::string name = std::filesystem::path(path).filename().string();
        char body[512];
        std::snprintf(body, sizeof body,
                      _("\"%s\" changed on disk since you opened it -- another program saved over "
                        "it. Mosaic won't overwrite those changes.\n\nSave your version as a "
                        "separate copy to keep it."),
                      name.c_str());
        AskOrTellDialog dlg;
        const int choice = dlg.ask({AskOrTellDialog::Icon::Warning, _("This file changed on disk"),
                                    body, {_("Cancel"), _("Save a copy...")}, /*cancelButton=*/0},
                                   this);
        if (choice == 1)
            saveDocumentAs();
    }

    // Arm commit-append Save for the current file-backed document (spec 2.6): remember where the
    // committed region ends (report.tip, identity-stamped for the pre-Save tail check), the
    // serialization of what is now durable (the diff source), and the append-region start
    // (walStartOffset, sizes the compaction threshold). The on-screen document MUST equal the
    // file's newest committed state for the diff to be valid, so callers arm this only on a clean
    // open / a full write. A commit-less tip (full-scan open -> checksumSize 0) or a failed stamp
    // leaves the anchor empty -> the next Save is a full write.
    void setCommitAnchor(const io::native::OpenReport& report, const std::string& path,
                         std::optional<io::native::CheckpointInput> baselineHint = std::nullopt) {
        m_commit.reset();
        if (report.tip.checksumSize == 0)
            return; // nothing to append onto (a full-scan fallback open): a full write next
        std::string err;
        std::optional<io::native::CheckpointInput> baseline = std::move(baselineHint);
        if (!baseline.has_value())
            baseline = io::native::buildDocumentCheckpoint(*m_document, &err);
        if (!baseline.has_value()) {
            uiLog().warn("save: could not baseline \"{}\" for commit-append: {}", path, err);
            return;
        }
        // The baseline carries the file's stored PRVW (S48-b): the next Save's diff compares the
        // fresh composite's downscale against what is actually on disk, so an unchanged preview
        // writes nothing -- across sessions too, with no composite spent at open time.
        io::native::seedPreviewFromReport(*baseline, report);
        io::native::CommitTip tip = report.tip;
        if (!io::native::stampTipIdentity(path, tip, &err)) {
            uiLog().warn("save: could not stamp \"{}\" identity: {} (next Save is a full write)",
                         path, err);
            return;
        }
        CommitAnchor anchor{tip, std::move(*baseline), report.base.walStartOffset};
        // Build 2: remember the file's encoding and seed the live churn signal (spec 3.9) --
        // whole retained history, the same walk the fold itself measures. Only for files small
        // enough that a proactive fold could ever fire (save_policy's stall gate); a damaged
        // history yields no tracker, and no signal beats a wrong one.
        anchor.mode = report.base.mode;
        if (tip.fileSize <= kProactiveFoldMaxBytes)
            anchor.churn = io::native::churnFromOpen(report);
        m_commit = std::move(anchor);
    }

    // "Saved <name>" for the status bar, shared by both write paths.
    std::string savedStatusLine(const std::string& path) const {
        char buf[256];
        std::snprintf(buf, sizeof buf, _("Saved %s"),
                      std::filesystem::path(path).filename().string().c_str());
        return buf;
    }

    // An auto-generated "Untitled N" adopts the saved file's stem as the document title at the
    // moment of a save (round 7): the chosen filename is the strongest signal of what the
    // document is called, and without this the titlebar would announce "Untitled 2" forever. A
    // deliberate title never qualifies (isAutoUntitledTitle), so it is never clobbered. Runs
    // BEFORE serialization so the manifest carries the adopted name.
    void adoptTitleFromPathIfUntitled(const std::string& path) {
        if (!m_document || !isAutoUntitledTitle(m_document->title()))
            return;
        const std::string stem = std::filesystem::path(path).stem().string();
        if (stem.empty() || stem == m_document->title())
            return;
        m_document->setTitle(stem);
        updateWindowTitle();
        refreshTabStrip();
    }

    // File->Rename Document (round 7): renames the document's own TITLE -- the identity the
    // titlebar and the New-Document dialog's Recent cards announce -- through the command stack
    // (undoable, and it dirties the document so the next save's manifest carries the name).
    // The file on disk is untouched.
    void renameDocument() {
        if (!m_document)
            return;
        const std::optional<std::string> name = promptForText(
            _("Rename Document"), _("Name"), _("Rename"), m_document->title(), this);
        if (!name.has_value() || name->empty() || *name == m_document->title())
            return;
        m_document->commands().push(std::make_unique<core::RenameDocumentCommand>(*name));
    }

    // File->Flatten History (S48 Build 2 ride-along; spec 3.6/3.7): discard every retained undo
    // state, keep only current content -- the storage-reclaim escape hatch beside
    // history-on-by-default. For a .mosaic-backed document this is "a save that retains no
    // history": a PLAIN full write, deliberately never the folding path, which exists to
    // preserve exactly what this action discards. The image itself never changes.
    void flattenHistory() {
        if (!m_document || m_saveJob)
            return;
        const std::string path = m_document->filePath();
        std::string ext = std::filesystem::path(path).extension().string();
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool fileBacked = !path.empty() && ext == ".mosaic" && !m_documentReadOnly;
        AskOrTellDialog dlg;
        const char* body =
            fileBacked
                ? _("This discards the document's saved undo history -- every earlier state kept "
                    "in the file -- and saves the current content without it. The image itself "
                    "does not change.\n\nThis cannot be undone.")
                : _("This discards this session's undo history. The image itself does not "
                    "change.\n\nThis cannot be undone.");
        const int choice = dlg.ask({AskOrTellDialog::Icon::Warning, _("Flatten History?"), body,
                                    {_("Cancel"), _("Flatten History")}, /*cancelButton=*/0},
                                   this);
        if (choice != 1)
            return;
        // Like every full write with an armed anchor, a flatten replaces the file WHOLESALE --
        // a foreign writer's commits would go with it, silently. Refuse loudly first (the same
        // check compactionSave runs), and before the stack clears: an aborted flatten must
        // leave the session exactly as it was.
        if (fileBacked && m_commit.has_value()) {
            namespace nio = io::native;
            std::string err;
            const nio::SaveStatus check = nio::verifyTail(path, m_commit->tip, &err);
            if (check != nio::SaveStatus::Ok) {
                surfaceSaveConflict(path, check, err);
                return;
            }
        }
        // The in-memory walk goes first (the panel empties to its clean baseline), and the dirty
        // marker stays HONEST: never-saved until the flatten's own save is durable -- a failed
        // write must not leave a clean title over a file that still holds the old history.
        m_document->commands().clear();
        m_document->commands().markNeverSaved();
        updateWindowTitle();
        if (!fileBacked) {
            transientStatus(_("History flattened"));
            return;
        }
        m_commit.reset(); // nothing left to append onto: the next save is this full write
        plainWriteTo(path);
    }

    void saveDocumentAs() {
        if (!m_document)
            return;
        if (m_saveJob) { // coalesce: the newest request wins after the running save (design note)
            m_pendingSave = PendingSave::SaveAs;
            return;
        }
        std::string stem;
        if (!m_document->filePath().empty())
            stem = std::filesystem::path(m_document->filePath()).stem().string();
        if (stem.empty())
            stem = m_document->title();
        if (stem.empty())
            stem = "untitled";

        platform::FileDialogRequest req;
        req.title = _("Save As");
        req.acceptLabel = _("Save");
        req.suggestedName = stem + ".mosaic";
        req.parent = this;
        req.startFolder = documentStartFolder();
        req.filters = {{_("Mosaic document"), {"*.mosaic"}, {}}, {_("All files"), {"*"}, {}}};
        const std::optional<std::string> chosen = platform::showSaveDialog(req);
        if (!chosen)
            return; // cancelled
        std::string path = *chosen;
        {
            std::string ext = std::filesystem::path(path).extension().string();
            for (char& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".mosaic")
                path += ".mosaic";
        }
        adoptTitleFromPathIfUntitled(path); // "Untitled N" takes the chosen stem (round 7)
        writeMosaicTo(path);
    }

    // The FULL WRITE Save path: Save As, a first save, a full-scan/recovered open's first save, and
    // fold fallbacks all funnel here (commitAppendSave handles the O(changed) common case).
    // Both are explicit-Save-driven, so the section-0 hard rule holds -- the user's file is written
    // only on an explicit Save. Atomic full write; the dirty title clears through the command
    // stack's saved marker (S18-d -- markSaved() finally has its caller), and this arms the anchor
    // so subsequent Saves commit-append.
    bool writeMosaicTo(const std::string& path) {
        if (m_document->uuid().empty())
            m_document->setUuid(io::native::mintDocumentUuid());
        std::string err;
        // A full write of a cleanly-anchored .mosaic document folds its file's retained history
        // through instead of dropping it (spec 3.3: "Save As / compaction re-encodes nothing")
        // -- so Save As keeps the undo walk, and the fold's churn signal realizes any pending
        // H2<->H4 switch (spec 3.9, the passive switch point). Quietly gated on the tail check:
        // a source that changed under us cannot be folded against this session's baseline, and
        // Save As to a fresh path is not the place for a conflict dialog -- fall through to the
        // plain checkpoint, which is exactly the pre-fold behavior.
        if (m_commit.has_value() && !m_document->filePath().empty() &&
            io::native::verifyTail(m_document->filePath(), m_commit->tip, &err) ==
                io::native::SaveStatus::Ok &&
            foldedWriteTo(m_document->filePath(), path, SaveJobKind::SaveAsFold))
            return true;
        return plainWriteTo(path);
    }

    // The plain full write (no history to fold): snapshot here, build + write on the worker.
    bool plainWriteTo(const std::string& path) {
        std::string err;
        const std::optional<common::Image> preview = compositeForPreview(*m_document);
        auto input = io::native::buildDocumentCheckpoint(
            *m_document, &err, preview.has_value() ? &*preview : nullptr);
        if (!input.has_value()) {
            uiLog().warn("save failed for \"{}\": {}", path, err);
            tellError(_("Could not save the document"), err, path);
            return false;
        }
        auto job = std::make_unique<SaveJob>();
        job->kind = SaveJobKind::PlainWrite;
        job->targetPath = path;
        job->after = std::move(*input);
        startSaveJob(std::move(job));
        return true;
    }

    // ---- Background full write (S48 Build 2; docs/async-save-design.md) -----------------------

    // The worker half of a full write: owns only the job -- static, so it CANNOT touch the
    // window or the document. Publishes fraction/stage under the job mutex; sets exactly one
    // outcome, then flips `done`.
    static void runSaveJob(SaveJob* j) {
        namespace nio = io::native;
        const auto publish = [j](float fraction, const char* stage) {
            std::lock_guard<std::mutex> lk(j->mutex);
            j->fraction = fraction;
            j->stage = stage;
        };
        std::vector<std::uint8_t> out;
        publish(0.0f, "Saving");
        if (j->kind == SaveJobKind::PlainWrite) {
            out = nio::buildCheckpoint(j->after, [&](double f) {
                publish(static_cast<float>(f) * 0.95f, "Saving");
            });
        } else {
            std::vector<std::uint8_t> bytes;
            (void)common::readWholeFile(j->sourcePath, bytes);
            if (bytes.empty()) {
                j->foldFailed = true;
                j->error = "could not re-read the file to fold it";
                j->done.store(true);
                return;
            }
            const nio::OpenReport open = nio::openDocument(bytes);
            nio::CompactionOptions opts;
            opts.progress = [&](double f) { publish(static_cast<float>(f) * 0.9f, "Saving"); };
            std::string err;
            const auto folded = nio::buildCompactedCheckpoint(bytes, open, j->dirty, &err, opts);
            if (!folded.has_value()) {
                j->foldFailed = true;
                j->error = err;
                j->done.store(true);
                return;
            }
            char line[256];
            std::snprintf(line, sizeof line,
                          "state %llu, mode %s (churn %.2f), %zu current + %zu retained chunk(s) "
                          "(%zu blob), %zu re-encoded",
                          static_cast<unsigned long long>(folded->generation),
                          folded->mode.c_str(), folded->churnFraction, folded->currentChunks,
                          folded->retainedChunks, folded->blobChunks, folded->reEncodedChunks);
            j->resultLine = line;
            // The authoritative tail check, as late as possible (design note): the file may have
            // changed while the fold built. The target is written only past it.
            std::string terr;
            j->tailStatus = nio::verifyTail(j->sourcePath, j->tip, &terr);
            if (j->tailStatus != nio::SaveStatus::Ok) {
                j->error = terr;
                j->done.store(true);
                return;
            }
            out = std::move(folded->bytes);
        }
        publish(0.98f, "Writing");
        std::string werr;
        if (!nio::writeFileAtomic(j->targetPath, out, &werr)) {
            j->error = werr;
            j->done.store(true);
            return;
        }
        j->bytes = std::move(out);
        j->ok = true;
        j->done.store(true);
    }

    void startSaveJob(std::unique_ptr<SaveJob> job) {
        m_saveJob = std::move(job);
        setMainControlsEnabled(false); // the document must stay quiescent under the worker
        if (m_statusBar) {
            m_statusBar->onProgressCancel(nullptr); // a save is never cancelled (design note)
            m_statusBar->setProgress(0.0f, _("Saving"));
        }
        SaveJob* j = m_saveJob.get();
        j->worker = std::thread([j] { runSaveJob(j); });
        Fl::add_timeout(kSavePollSeconds, saveJobPollTimer, this);
    }

    static void saveJobPollTimer(void* self) { static_cast<MainWindow*>(self)->pollSaveJob(); }

    void pollSaveJob() {
        if (!m_saveJob)
            return;
        SaveJob* j = m_saveJob.get();
        float frac = 0.0f;
        std::string stage;
        {
            std::lock_guard<std::mutex> lk(j->mutex);
            frac = j->fraction;
            stage = j->stage;
        }
        if (m_statusBar)
            m_statusBar->setProgress(frac, stage == "Writing" ? _("Writing") : _("Saving"));
        if (j->done.load()) {
            finalizeSaveJob();
            return;
        }
        Fl::repeat_timeout(kSavePollSeconds, saveJobPollTimer, this);
    }

    // The UI-thread epilogue (design note FINALIZE): join, then exactly one consequence --
    // adopt, conflict dialog, fallback, or error -- and the coalesced request replays last.
    void finalizeSaveJob() {
        if (!m_saveJob)
            return;
        std::unique_ptr<SaveJob> job = std::move(m_saveJob);
        if (job->worker.joinable())
            job->worker.join();
        if (m_statusBar)
            m_statusBar->hideProgress();
        setMainControlsEnabled(true);
        namespace nio = io::native;

        if (job->ok) {
            if (!job->resultLine.empty())
                uiLog().info("folded \"{}\": {}, {} bytes", job->targetPath, job->resultLine,
                             job->bytes.size());
            else
                uiLog().info("saved \"{}\": {} bytes, {} layers", job->targetPath,
                             job->bytes.size(), m_document ? m_document->layerCount() : 0);
            adoptWrittenFile(job->targetPath, job->bytes, std::move(job->after));
        } else if (job->kind == SaveJobKind::CompactionFold) {
            if (job->tailStatus != nio::SaveStatus::Ok) {
                surfaceSaveConflict(job->targetPath, job->tailStatus, job->error);
            } else if (job->foldFailed) {
                // A fold that cannot be built must NOT become a plain full write -- that would
                // discard the very history it exists to preserve. Append instead: the parity
                // debt stays, and the next Save tries the fold again (spec 2.6).
                uiLog().warn("fold: could not fold \"{}\": {} (appending instead)",
                             job->targetPath, job->error);
                if (commitAppendSave(job->targetPath))
                    noteDocumentSaved(job->targetPath);
            } else {
                tellError(_("Could not save the document"), job->error, job->targetPath);
            }
        } else if (job->kind == SaveJobKind::SaveAsFold &&
                   (job->foldFailed || job->tailStatus != nio::SaveStatus::Ok)) {
            // The quiet Save As fallbacks, exactly the pre-fold behavior: a fold that cannot be
            // built (or a source that changed under us) still saves the DOCUMENT -- as a plain
            // checkpoint, without the retained history.
            uiLog().warn("fold: falling back to a plain write for \"{}\": {}", job->targetPath,
                         job->error);
            plainWriteTo(job->targetPath);
        } else {
            tellError(_("Could not save the document"), job->error, job->targetPath);
        }

        // A coalesced request replays through the ordinary entry points (newest wins) -- unless
        // a fallback just started a follow-up job, in which case it rides after that one.
        if (!m_saveJob) {
            const PendingSave pending = std::exchange(m_pendingSave, PendingSave::None);
            if (pending == PendingSave::Save)
                saveDocument();
            else if (pending == PendingSave::SaveAs)
                saveDocumentAs();
        }
    }

    // A close/quit (or a save-before-close) while a background save runs WAITS, with the
    // progress strip live (design note): the bytes are half-spent and the user's intent was
    // explicit. The poll timer keeps running inside Fl::wait, so finalize fires in here.
    void waitForBackgroundSave() {
        if (m_waitingForSave)
            return;
        m_waitingForSave = true;
        while (m_saveJob)
            Fl::wait(0.05);
        m_waitingForSave = false;
    }

    // The shared tail of every full write (Save As, a first save, a compaction): `path` now holds
    // exactly `bytes`, durably. Kept in one place so the callers cannot drift on the ownership,
    // journal-rebind, and anchor-rearm sequence. `baselineHint`, when present, is the snapshot's
    // own serialization of what was just written -- it spares the anchor rearm a second full
    // serialization of the document (design note).
    void adoptWrittenFile(const std::string& path, std::span<const std::uint8_t> bytes,
                          std::optional<io::native::CheckpointInput> baselineHint = std::nullopt) {
        m_document->setFilePath(path);
        m_document->commands().markSaved();
        // The window now owns the written file: it is no longer a read-only borrow, and it takes
        // the §2.10 lock for this path (a no-op on an ordinary re-Save; a real acquire after Save As
        // or a read-only Save As mints the copy).
        m_documentReadOnly = false;
        acquireLockForCurrentDocument();
        // Reset the recovery journal onto the just-saved commit (spec 2.6, Round 12 A2: only after
        // the Save's bytes are durable, which writeFileAtomic guarantees). Re-derive the binding by
        // reading back what we just wrote -- its root checksum is the new chain seed.
        const io::native::OpenReport fresh = io::native::openDocument(bytes);
        beginJournal(&fresh);
        // Arm commit-append for the next Save: the document on screen is exactly what we just wrote,
        // so subsequent Saves append only their delta (spec 2.6). A freshly written checkpoint binds
        // its tip to the root slot, so fresh.tip is valid here -- and its walStart resets the
        // compaction threshold, which the fold has just paid off.
        setCommitAnchor(fresh, path, std::move(baselineHint));
        transientStatus(savedStatusLine(path));
        noteDocumentSaved(path); // recents + the dialog card's preview follow every save (S55)
    }

    // ---- Recovery journal (S48, spec 2.6) ----

    // Canonicalize a path so the same file reached by different spellings shares one journal/lock
    // key. Empty stays empty (an untitled document).
    static std::string canonicalPathOf(const std::string& path) {
        if (path.empty())
            return {};
        std::error_code ec;
        const auto c = std::filesystem::weakly_canonical(path, ec);
        return ec ? path : c.string();
    }

    // The current document's canonical path (empty for an untitled document).
    std::string journalDocumentPath() const {
        return m_document ? canonicalPathOf(m_document->filePath()) : std::string{};
    }

    // Release the current document's advisory lock (a clean close, or switching documents). The OS
    // would release it on exit anyway; doing it explicitly frees the file for another window at
    // once. The lock file is left in place (removing it would race a concurrent acquire).
    void releaseLock() {
        if (m_lock.has_value())
            m_lock->release();
        m_lock.reset();
    }

    // Take the §2.10 advisory lock for the current (file-backed) document, unless it is already
    // held for this exact file. Called after a Save/Save As so a freshly written file -- including
    // the copy a read-only Save As mints -- is write-locked by this window. Untitled documents are
    // not locked. Best-effort: a busy/failed lock just leaves the save unlocked (logged).
    void acquireLockForCurrentDocument() {
        if (!m_document || m_document->uuid().empty())
            return;
        const std::string canonical = journalDocumentPath();
        if (canonical.empty())
            return; // untitled: no file to lock over
        const std::string lockPath =
            io::native::recoveryLockPath(m_document->uuid(), canonical);
        if (m_lock.has_value() && m_lock->path() == lockPath)
            return; // already held for this file (an ordinary re-Save)
        releaseLock();
        std::optional<io::native::AdvisoryLock> l;
        std::string lerr;
        const auto status = io::native::AdvisoryLock::tryAcquire(lockPath, l, &lerr);
        if (status == io::native::AdvisoryLock::Status::Acquired)
            m_lock = std::move(l);
        else
            uiLog().warn("lock: could not lock \"{}\" after saving (proceeding unlocked)", lockPath);
    }

    // Start a recovery journal for the current document. `report` non-null (a just-opened/saved
    // .mosaic, conservative open) makes it INCREMENTAL over that file's commit -- its diffs compose
    // onto the committed region. Null makes it SELF-CONTAINED (a New/untitled doc, or a salvaged
    // recovered view whose states no longer match the file): the first autosave carries the whole
    // document, so a restore rebuilds from the journal alone. Discards any prior journal first.
    void beginJournal(const io::native::OpenReport* report) {
        discardJournal();
        if (!m_document)
            return;
        if (m_document->uuid().empty())
            m_document->setUuid(io::native::mintDocumentUuid());
        const std::string uuid = m_document->uuid();
        const std::string canonical = journalDocumentPath();

        io::native::JournalBinding binding;
        std::optional<io::native::CheckpointInput> baseline;
        std::uint64_t firstState = 1;
        const bool incremental =
            report != nullptr && report->tipValid && report->tip.checksumSize > 0;
        if (incremental) {
            std::string err;
            auto in = io::native::buildDocumentCheckpoint(*m_document, &err);
            if (!in.has_value()) {
                uiLog().warn("journal: could not baseline \"{}\": {}", canonical, err);
                return; // no crash protection this session -- not fatal to editing
            }
            binding = io::native::bindingForTip(uuid, canonical, report->tip);
            baseline = std::move(*in);
            firstState = report->tip.commitId + 1;
        } else {
            binding = io::native::selfContainedBinding(uuid, canonical);
            // baseline stays nullopt: the first autosave carries the whole document.
        }

        const std::string jpath = io::native::recoveryJournalPath(uuid, canonical);
        std::string err;
        auto s = io::native::JournalSession::begin(jpath, binding, std::move(baseline), firstState,
                                                   &err);
        if (!s.has_value()) {
            uiLog().warn("journal: could not start at \"{}\": {}", jpath, err);
            return;
        }
        m_journal = std::move(s);
        m_journalDirty = false;
        uiLog().info("journal: started at \"{}\" ({})", jpath,
                     incremental ? "incremental" : "self-contained");
    }

    void discardJournal() {
        if (m_journal.has_value())
            m_journal->discard();
        m_journal.reset();
        m_journalDirty = false;
    }

    // Start a fresh self-contained journal for the current document AND snapshot it immediately.
    // Used after a restore: the restored content lives only in memory (its old journal has just
    // been truncated), so it must be re-captured at once -- a second crash before the first edit
    // would otherwise lose it. A New/blank document waits for its first edit instead (nothing to
    // protect yet), so this is deliberately not the path beginJournal(nullptr) takes on its own.
    void beginJournalWithSnapshot() {
        beginJournal(nullptr);
        if (m_journal.has_value() && m_journal->alive() && m_document) {
            std::string err;
            if (!m_journal->autosave(*m_document, &err))
                uiLog().warn("journal: could not snapshot the restored document: {}", err);
        }
    }

    // onFrame: autosave the current document once its edits settle (idle-triggered coalescing,
    // Round 12). The command-stack observer stamps m_journalDirty/Time; a still-editing document
    // keeps re-stamping, so the write fires only after a pause -- the low cadence the spec's
    // SSD-wear measurement assumed. A no-change diff writes nothing.
    void updateJournal() {
        if (!m_journal.has_value() || !m_journal->alive() || !m_document)
            return;
        if (!m_journalDirty || nowSeconds() - m_journalDirtyTime < kJournalSettleSec)
            return;
        m_journalDirty = false;
        std::string err;
        if (!m_journal->autosave(*m_document, &err))
            uiLog().warn("journal: autosave failed ({}); crash protection paused this session", err);
    }

    // Force the journal write updateJournal() is still waiting out the settle timer for, on EVERY
    // open tab -- a background tab carries its own journal and its own unsynced tail (S49). Called
    // from the Windows session-end guard, which runs as the OS is about to terminate us:
    // JournalSession::autosave already fdatasyncs and a no-change diff writes nothing, so this is
    // exactly one durable append per genuinely dirty document. The background save worker never
    // reads the document, so this is safe with one in flight.
    //
    // ⚠ It must NOT write the user's file -- a session end is not a Save -- and must NOT discard a
    // journal: being killed by the OS is an unclean exit, and the journals staying behind is
    // precisely what makes the next launch offer this work back.
    void syncJournalsForSessionEnd() {
        m_journalDirty = false;
        if (m_journal.has_value() && m_journal->alive() && m_document) {
            std::string err;
            if (!m_journal->autosave(*m_document, &err))
                uiLog().warn("journal: session-end sync failed: {}", err);
        }
        // The ACTIVE slot's doc is null (it lives in the register, unloadActiveSession moves it
        // back), so this loop naturally covers only the background tabs.
        for (DocumentSession& s : m_sessions) {
            s.journalDirty = false;
            if (!s.journal.has_value() || !s.journal->alive() || !s.doc)
                continue;
            std::string err;
            if (!s.journal->autosave(*s.doc, &err))
                uiLog().warn("journal: session-end sync failed for a background tab: {}", err);
        }
    }

    // A journal file's last-write time as a short local string, for flow 1's "the last from %s".
    // The journal lives in app state, not the user's file, so surfacing its mtime leaks nothing
    // (unlike a save time embedded in the document -- deliberately not stored, see the History
    // panel). Portable file_time -> system_clock conversion (no clock_cast dependency).
    static std::string journalMtime(const std::string& jpath) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const auto ft = fs::last_write_time(jpath, ec);
        if (ec)
            return _("earlier");
        const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        const std::time_t t = std::chrono::system_clock::to_time_t(sctp);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[64];
        return std::strftime(buf, sizeof buf, "%b %e at %H:%M", &tm) ? std::string(buf)
                                                                     : std::string(_("earlier"));
    }

    // A document rebuilt from the recovery journal is unsaved work that never reached disk: mark it
    // permanently dirty and refresh the title/duration now (S48). Only an explicit Save clears it.
    void markDocumentDirtyAfterRestore() {
        if (!m_document)
            return;
        m_document->commands().markNeverSaved();
        onCommandStackChanged();
    }

    // Crash restore (docs/askortell-dialog.md flows 1/2): offer to bring back a journal that a
    // previous unclean exit left for `path`. Returns true when the user restored -- the restored
    // document is now shown under its own fresh (self-contained) journal; false otherwise (no
    // journal, declined, or an orphan handled), leaving `report`'s conservative document in place
    // for the caller to journal normally. Fired AFTER any damage face so a damaged open can be
    // followed by this in the same window (the compose rule).
    bool offerJournalRestore(const std::string& path, const io::native::OpenReport& report) {
        namespace nio = io::native;
        if (!m_document || m_document->uuid().empty())
            return false;
        const std::string uuid = m_document->uuid();
        const std::string canonical = journalDocumentPath();
        const std::string jpath = nio::recoveryJournalPath(uuid, canonical);
        std::error_code ec;
        if (!std::filesystem::exists(jpath, ec))
            return false;

        std::vector<std::uint8_t> jbytes;
        (void)common::readWholeFile(jpath, jbytes);
        const nio::JournalBinding binding = nio::bindingForTip(uuid, canonical, report.tip);
        const nio::JournalReplay replay = nio::replayJournal(jbytes, binding);
        const JournalRecoveryDecision decision =
            classifyJournalRecovery(replay.binding, replay);

        switch (decision.kind) {
        case JournalRecovery::None:
            std::remove(jpath.c_str()); // stale/empty/garbage: nothing to offer, clear it
            return false;
        case JournalRecovery::Orphan: {
            AskOrTellDialog dlg;
            const int choice = dlg.ask(
                {AskOrTellDialog::Icon::Warning,
                 _("Old unsaved changes no longer match this file"),
                 _("Mosaic kept unsaved changes for this document from an earlier session, but "
                   "the file has since been changed or replaced outside Mosaic, so they can no "
                   "longer be restored into it.\n\nKeep the recovery file if you want to inspect "
                   "it later; otherwise it can be discarded."),
                 {_("Keep the file"), _("Discard")}, /*cancelButton=*/AskOrTellDialog::kCancelAuto},
                this);
            if (choice == 1) {
                std::remove(jpath.c_str()); // Discard
            } else {
                // Keep (also the Escape default): move it aside so a fresh journal can start here.
                std::error_code mec;
                std::filesystem::rename(jpath, jpath + ".orphan", mec);
            }
            return false;
        }
        case JournalRecovery::Restore: {
            char body[1024];
            const std::string when = journalMtime(jpath);
            const char* torn = decision.tornTail
                                   ? _(" The very last change was cut off and couldn't be kept.")
                                   : "";
            std::snprintf(body, sizeof body,
                          _("Mosaic didn't close cleanly the last time this document was open. "
                            "Your unsaved work -- %zu change(s), the last from %s -- was kept "
                            "safe.%s\n\nRestore picks up exactly where you left off. Nothing is "
                            "written to your file until you save."),
                          decision.changeCount, when.c_str(), torn);
            AskOrTellDialog dlg;
            const int choice =
                dlg.ask({AskOrTellDialog::Icon::Restore, _("Unsaved changes found"), body,
                         {_("Discard changes"), _("Restore")}, AskOrTellDialog::kCancelNone},
                        this);
            if (choice != 1) {              // Discard changes (forced choice -- Escape is inert)
                std::remove(jpath.c_str());
                return false;
            }
            nio::OpenReport synth = report; // compose the journal's states onto the committed region
            for (const nio::RecoveredChunk& c : replay.chunks)
                synth.committed.push_back(c);
            std::string err;
            auto restored = nio::documentFromReport(synth, &err);
            if (!restored.has_value()) {
                tellError(_("Could not restore the unsaved changes"), err, path);
                std::remove(jpath.c_str());
                return false;
            }
            restored->document->setFilePath(path);
            presentDocument(std::move(restored->document), /*fitView=*/true);
            markDocumentDirtyAfterRestore();
            beginJournalWithSnapshot(); // self-contained; capture the restored state at once
            char st[256];
            if (decision.tornTail)
                std::snprintf(st, sizeof st,
                              _("Restored %zu of %zu unsaved changes -- the last was incomplete"),
                              decision.changeCount, decision.changeCount + 1);
            else
                std::snprintf(st, sizeof st, _("Restored %zu unsaved change(s)"),
                              decision.changeCount);
            transientStatus(st);
            return true;
        }
        }
        return false;
    }

    // App start (empty state): scan the recovery directory for UNTITLED journals (JHDR path empty)
    // a previous unclean exit left, and offer to restore them (flow 1, untitled variant). A
    // file-backed journal is skipped here -- it fires when its file is opened. Single-window, so we
    // offer newest-first and stop at the first the user restores; the rest resurface next launch.
    // Never runs in the headless frame-count smoke (a modal would hang it).
    void offerUntitledRestoresAtStart() {
        if (m_autoQuitFrames > 0 || m_document)
            return;
        namespace nio = io::native;
        namespace fs = std::filesystem;
        const fs::path dir = fs::path(nio::recoveryJournalPath("x", "")).parent_path();
        std::error_code ec;
        if (!fs::exists(dir, ec))
            return;
        struct Candidate {
            std::string path;
            fs::file_time_type mtime;
        };
        std::vector<Candidate> cands;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec)
                break;
            if (!entry.is_regular_file(ec))
                continue;
            // A torn growth-compaction temp (spec 2.6) is not a journal: the rename never
            // happened, so the REAL journal beside it replays fine -- offering both would show
            // the same document twice.
            if (entry.path().extension() == io::native::kJournalCompactSuffix)
                continue;
            std::vector<std::uint8_t> bytes;
            (void)common::readWholeFile(common::utf8FromPath(entry.path()), bytes);
            const auto info = nio::readJournalHeader(bytes);
            if (!info.has_value() || !info->documentPath.empty())
                continue; // not a readable untitled journal
            cands.push_back({entry.path().string(), entry.last_write_time(ec)});
        }
        std::sort(cands.begin(), cands.end(),
                  [](const Candidate& a, const Candidate& b) { return a.mtime > b.mtime; });
        for (const Candidate& c : cands)
            if (offerUntitledRestore(c.path))
                break; // restored one into the single window; the rest wait for next launch
    }

    // Restore one untitled journal into a new window (flow 1, untitled variant). Returns true when
    // a document was restored and shown. A declined or contentless journal is discarded.
    bool offerUntitledRestore(const std::string& jpath) {
        namespace nio = io::native;
        std::vector<std::uint8_t> bytes;
        (void)common::readWholeFile(jpath, bytes);
        const auto info = nio::readJournalHeader(bytes);
        if (!info.has_value()) {
            std::remove(jpath.c_str());
            return false;
        }
        const nio::JournalBinding binding = info->toBinding();
        const nio::JournalReplay replay = nio::replayJournal(bytes, binding);
        const JournalRecoveryDecision decision =
            classifyJournalRecovery(replay.binding, replay);
        if (decision.kind != JournalRecovery::Restore) {
            std::remove(jpath.c_str());
            return false;
        }

        char body[1024];
        const std::string when = journalMtime(jpath);
        const char* torn = decision.tornTail
                               ? _(" The very last change was cut off and couldn't be kept.")
                               : "";
        std::snprintf(body, sizeof body,
                      _("Mosaic didn't close cleanly with an untitled document open. Your unsaved "
                        "work -- %zu change(s), the last from %s -- was kept safe.%s\n\nRestore "
                        "opens it in a new window. Nothing is written to disk until you save."),
                      decision.changeCount, when.c_str(), torn);
        AskOrTellDialog dlg;
        const int choice =
            dlg.ask({AskOrTellDialog::Icon::Restore, _("Unsaved untitled document found"), body,
                     {_("Discard changes"), _("Restore")}, AskOrTellDialog::kCancelNone},
                    this);
        if (choice != 1) {
            std::remove(jpath.c_str());
            return false;
        }
        nio::OpenReport synth; // no file to compose onto: the journal is the whole document
        for (const nio::RecoveredChunk& c : replay.chunks)
            synth.committed.push_back(c);
        std::string err;
        auto restored = nio::documentFromReport(synth, &err);
        if (!restored.has_value()) {
            tellError(_("Could not restore the untitled document"), err, jpath);
            std::remove(jpath.c_str());
            return false;
        }
        presentDocument(std::move(restored->document), /*fitView=*/true); // untitled: no file path
        markDocumentDirtyAfterRestore();
        beginJournalWithSnapshot(); // capture the restored untitled state at once (not on disk)
        transientStatus(_("Restored unsaved untitled document"));
        return true;
    }

#ifdef MOSAIC_DEBUG
    // Help->Open Demo Canvas: the pre-S50 startup placeholder (four blend-mode rasters), kept
    // as debug scenery for eyeballing the compositor without opening a file.
    void openDemoCanvas() { presentDocument(buildPlaceholderDocument(), /*fitView=*/true); }

    // Help->Enable/Disable All Controls: grey every chrome region in one shot so each control
    // kind's disabled rendering can be compared. Fl_Group::deactivate() cascades through
    // active_r(), so one call per region covers every widget inside it. The MENU BAR is never
    // touched (it is how you get back), and neither is the canvas -- it is a render surface, not
    // a control, and deactivating it would only mute the tools' own cursors. Re-enabling calls
    // the panels' own sync paths afterwards, because a control that was ALREADY disabled on its
    // own terms (an empty layer panel's blend dropdown, a gated crop toggle) must stay disabled
    // rather than be woken up by a blanket activate().
    void setAllControlsEnabled(bool on) {
        for (Fl_Widget* region : {static_cast<Fl_Widget*>(m_toolbar),
                                  static_cast<Fl_Widget*>(m_optionsBar),
                                  static_cast<Fl_Widget*>(m_dock), // both regions: layers + presets
                                  static_cast<Fl_Widget*>(m_statusBar)}) {
            if (region == nullptr)
                continue;
            if (on)
                region->activate();
            else
                region->deactivate();
        }
        if (on) {
            if (m_layerPanel != nullptr)
                m_layerPanel->refresh(); // re-derives the strip's own enabled state
            if (m_optionsBar != nullptr)
                m_optionsBar->syncValues(); // re-applies each ToolOption::enabled gate
        }
        redraw();
    }
#endif

    // File->Quick Export as PNG (Export & I/O plan, Milestone 1): flatten the document and write it
    // straight to a PNG -- no options modal, no loss warnings (the full Export... modal is a later
    // milestone). The shared native Save dialog (portal) picks the path. An export is NOT a Save,
    // so it never touches the command-stack saved marker (GIMP-style; the title stays "unsaved").
    void quickExportPng() {
        if (!m_document)
            return;

        // Suggested filename: the document's file stem (or its title) + ".png".
        std::string stem;
        if (!m_document->filePath().empty())
            stem = std::filesystem::path(m_document->filePath()).stem().string();
        if (stem.empty())
            stem = m_document->title();
        if (stem.empty())
            stem = "untitled";

        platform::FileDialogRequest req;
        req.title = _("Export");
        req.acceptLabel = _("Export");
        req.suggestedName = stem + ".png";
        req.parent = this;
        req.startFolder = exportStartFolder();
        req.filters = {{_("PNG image"), {"*.png"}, {"image/png"}}, {_("All files"), {"*"}, {}}};
        const std::optional<std::string> chosen = platform::showSaveDialog(req);
        if (!chosen)
            return; // cancelled

        // Guarantee a .png extension (the user may have typed a bare name).
        std::string path = *chosen;
        {
            std::string ext = std::filesystem::path(path).extension().string();
            for (char& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".png")
                path += ".png";
        }

        // Flatten the document exactly as the canvas shows it (true alpha, the same resample
        // filter), on the deterministic CPU path -- the whole-doc export flatten (plan section 5).
        settleTextCaches(); // an export must not be the only consumer of a band the canvas needs
        render::CompositeOptions opts;
        opts.checkerboard = false;
        opts.resampleFilter = currentResampleFilter();
        const render::CompositeResult flat =
            render::composite(*m_document, opts, render::Backend::Cpu);
        if (!flat.ok) {
            uiLog().warn("quick export: composite failed: {}", flat.error);
            tellError(_("Could not export the image"), flat.error, path);
            return;
        }

        std::string err;
        io::PngSaveOptions pngOpts;
        pngOpts.metadata = io::documentMetadata(*m_document); // EXIF + profile + density (§7d)
        if (!io::savePng(flat.image, path, pngOpts, &err)) {
            uiLog().warn("quick export failed for \"{}\": {}", path, err);
            tellError(_("Could not export the image"), err, path);
            return;
        }
        uiLog().info("quick-exported PNG \"{}\": {}x{}", path, flat.image.width, flat.image.height);
        noteExported(path, "png"); // a quick export is an export: it sets the re-export target too
    }

    // Shared by the JPEG/JXL quick exports + the Export As... modal: the whole-doc flatten (true
    // alpha, checkerboard off, the canvas's own resample filter) on the deterministic CPU path
    // (plan §5). Returns nullopt and, when non-null, *error on failure.
    std::optional<common::Image> compositeForExport(std::string* error) {
        if (!m_document)
            return std::nullopt;
        settleTextCaches(); // ... and the same here: the flatten is for a FILE, the canvas still
                            // needs whatever this refresh moved
        render::CompositeOptions opts;
        opts.checkerboard = false;
        opts.resampleFilter = currentResampleFilter();
        render::CompositeResult flat = render::composite(*m_document, opts, render::Backend::Cpu);
        if (!flat.ok) {
            if (error)
                *error = flat.error;
            return std::nullopt;
        }
        return std::move(flat.image);
    }

    // The suggested export filename stem: the document's file stem, else its title, else "untitled".
    std::string exportStem() const {
        std::string stem;
        if (m_document != nullptr && !m_document->filePath().empty())
            stem = std::filesystem::path(m_document->filePath()).stem().string();
        if (stem.empty() && m_document != nullptr)
            stem = m_document->title();
        if (stem.empty())
            stem = "untitled";
        return stem;
    }

    // Where the system picker OPENS, for an export of this document (Export & I/O plan §6): the
    // directory of this document's last export (sticky per document, never leaked to the next one),
    // else the directory the document itself came from, else the OS pictures folder, else $HOME.
    //
    // The rule that matters is the one that is NOT here: the process working directory. Every one
    // of these picker requests used to leave startFolder empty, and each backend then invented its
    // own answer -- FLTK's kdialog driver literally getcwd()s -- so an export landed wherever the
    // binary happened to be launched from (user report). io::exportStartFolder cannot produce it.
    std::string exportStartFolder() const {
        return io::exportStartFolder(io::exportPathInputs(
            m_exportTarget.path, m_document != nullptr ? m_document->filePath() : std::string{}));
    }

    // The same, for opening or saving a DOCUMENT: its own directory, else the OS documents folder.
    std::string documentStartFolder() const {
        return io::exportStartFolder(io::documentPathInputs(
            m_document != nullptr ? m_document->filePath() : std::string{}));
    }

    // File->Quick Export as JPEG (plan §7, the sibling of Quick Export PNG): flatten + write a JPEG
    // with default options, no modal. Alpha is flattened onto the JpegSaveOptions default matte.
    void quickExportJpeg() {
        if (!m_document)
            return;
        platform::FileDialogRequest req;
        req.title = _("Export");
        req.acceptLabel = _("Export");
        req.suggestedName = exportStem() + ".jpg";
        req.parent = this;
        req.startFolder = exportStartFolder();
        req.filters = {{_("JPEG image"), {"*.jpg", "*.jpeg"}, {"image/jpeg"}},
                       {_("All files"), {"*"}, {}}};
        const std::optional<std::string> chosen = platform::showSaveDialog(req);
        if (!chosen)
            return; // cancelled

        std::string path = *chosen;
        {
            std::string ext = std::filesystem::path(path).extension().string();
            for (char& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".jpg" && ext != ".jpeg")
                path += ".jpg";
        }

        std::string err;
        const std::optional<common::Image> flat = compositeForExport(&err);
        if (!flat) {
            uiLog().warn("quick export (jpeg): composite failed: {}", err);
            tellError(_("Could not export the image"), err, path);
            return;
        }
        io::JpegSaveOptions jpegOpts;
        jpegOpts.metadata = io::documentMetadata(*m_document);
        if (!io::saveJpeg(*flat, path, jpegOpts, &err)) {
            uiLog().warn("quick export (jpeg) failed for \"{}\": {}", path, err);
            tellError(_("Could not export the image"), err, path);
            return;
        }
        uiLog().info("quick-exported JPEG \"{}\": {}x{}", path, flat->width, flat->height);
        noteExported(path, "jpeg");
    }

    // File->Quick Export as JPEG XL (plan §7): as quickExportJpeg, through the optional libjxl codec.
    // The menu item is only added when io::jxlSupported(); the guard keeps the method honest anyway.
    void quickExportJxl() {
        if (!m_document)
            return;
        if (!io::jxlSupported()) {
            tellError(_("Could not export the image"),
                      _("JPEG XL support was not compiled into this build."));
            return;
        }
        platform::FileDialogRequest req;
        req.title = _("Export");
        req.acceptLabel = _("Export");
        req.suggestedName = exportStem() + ".jxl";
        req.parent = this;
        req.startFolder = exportStartFolder();
        req.filters = {{_("JPEG XL image"), {"*.jxl"}, {"image/jxl"}}, {_("All files"), {"*"}, {}}};
        const std::optional<std::string> chosen = platform::showSaveDialog(req);
        if (!chosen)
            return; // cancelled

        std::string path = *chosen;
        {
            std::string ext = std::filesystem::path(path).extension().string();
            for (char& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".jxl")
                path += ".jxl";
        }

        std::string err;
        const std::optional<common::Image> flat = compositeForExport(&err);
        if (!flat) {
            uiLog().warn("quick export (jxl): composite failed: {}", err);
            tellError(_("Could not export the image"), err, path);
            return;
        }
        io::JxlSaveOptions jxlOpts;
        jxlOpts.metadata = io::documentMetadata(*m_document);
        if (!io::saveJxl(*flat, path, jxlOpts, &err)) {
            uiLog().warn("quick export (jxl) failed for \"{}\": {}", path, err);
            tellError(_("Could not export the image"), err, path);
            return;
        }
        uiLog().info("quick-exported JXL \"{}\": {}x{}", path, flat->width, flat->height);
        noteExported(path, "jxl");
    }

    // An export landed at `path` (plan §6 "Path behavior"): remember it FOR THIS DOCUMENT so
    // re-export is one click (File -> "Export to <file>"), and so the next Export As… opens on the
    // same folder and format. Deliberately app state on the document's session -- never a sidecar
    // file beside the user's image -- and presentDocument() clears it, so it can never leak to the
    // next document (the annoyance §6 calls out). S48 persists it into the .mosaic manifest.
    //
    // An export is NOT a Save: nothing here touches the command-stack saved marker.
    void noteExported(const std::string& path, std::string formatId,
                      io::OptionValues values = {}) {
        m_exportTarget.path = path;
        m_exportTarget.formatId = std::move(formatId);
        m_exportTarget.values = std::move(values); // empty = "the backend's defaults"
        syncExportToMenuItem();
        char buf[256];
        std::snprintf(buf, sizeof buf, _("Exported %s"),
                      std::filesystem::path(path).filename().string().c_str());
        transientStatus(buf);
    }

    // File->Export As... (plan §6): flatten the document, then open the two-pane Export modal (format
    // choice, per-format options, output size, live preview, output path). The modal writes the file;
    // an Export is NOT a Save (the command-stack saved marker is untouched).
    void exportAs() {
        if (!m_document)
            return;
        std::string err;
        const std::optional<common::Image> flat = compositeForExport(&err);
        if (!flat) {
            uiLog().warn("export as: composite failed: {}", err);
            tellError(_("Could not export the image"), err);
            return;
        }
        ExportRequest request;
        request.composited = &*flat;
        // With the flatten in hand the profile's hasAlpha and colour count are EXACT rather than
        // conservative -- and the banner is the one surface that must never over-warn either.
        request.profile = io::profileDocument(*m_document, *flat);
        request.target = m_exportTarget;
        request.suggestedStem = exportStem();
        request.documentPath = m_document->filePath();
        request.dpi = m_document->dpi();
        // The metadata payload the modal's Colour & metadata section offers (§7d). Resolved HERE,
        // once: the provenance rule that picks which layer's EXIF an export writes is
        // io::documentExif, and turning a working space or a custom .icc into embeddable bytes is
        // core::documentIccProfile. Neither belongs in a dialog.
        request.exif = io::documentExif(*m_document);
        request.documentIcc = core::documentIccProfile(*m_document);
        request.documentIccName = m_document->iccProfileName().empty()
                                      ? std::string(core::colorSpaceName(m_document->colorSpace()))
                                      : m_document->iccProfileName();
        // §3: the exotic tier hides behind Settings -> General -> "Show all export formats". Read at
        // open time, the way the brush editor seeds its eraser ties: the modal is built fresh each
        // time, so there is no live state to keep in sync.
        request.showAllFormats = common::loadSettings(m_settingsPath).showAllExportFormats;
        const std::optional<ExportResult> res = showExportDialog(request, this);
        if (!res)
            return; // cancelled or nothing written
        uiLog().info("exported \"{}\" as {}", res->path, res->formatId);
        noteExported(res->path, res->formatId, res->values);
    }

    // File -> "Export to <file>": re-run the last export of THIS document with no dialog at all
    // (§1's Overwrite verb). Greyed until there has been one.
    void exportToLastTarget() {
        if (!m_document || m_exportTarget.path.empty())
            return;
        const io::FormatBackend* backend =
            io::FormatRegistry::instance().findByPath(m_exportTarget.path);
        if (backend == nullptr || !backend->available()) {
            exportAs(); // the format went away with a rebuild: fall back to the modal
            return;
        }
        std::string err;
        const std::optional<common::Image> flat = compositeForExport(&err);
        if (!flat) {
            uiLog().warn("export to target: composite failed: {}", err);
            tellError(_("Could not export the image"), err);
            return;
        }
        // The same payload an Export As... of this document would write, so a one-click re-export
        // does not quietly produce a different file (§7d).
        const std::optional<common::ExifData> exif = io::documentExif(*m_document);
        io::RenderInput input;
        input.pixels = &*flat;
        input.dpi = m_document->dpi();
        input.exif = exif.has_value() ? &*exif : nullptr;
        input.iccProfile = core::documentIccProfile(*m_document);
        // The remembered options, made legal against today's schema (an empty bag simply becomes
        // the defaults) -- so the re-export repeats the encode the user actually chose.
        io::OptionValues values = m_exportTarget.values;
        backend->optionsSchema().coerce(values);
        if (!io::encodeToFile(*backend, input, values, m_exportTarget.path, &err)) {
            uiLog().warn("export to \"{}\" failed: {}", m_exportTarget.path, err);
            tellError(_("Could not export the image"), err, m_exportTarget.path);
            return;
        }
        noteExported(m_exportTarget.path, std::string(io::formatIdName(backend->id())),
                     std::move(values));
    }

    // Keep File -> "Export to <file>" honest: it names the remembered file and greys out until
    // this document has been exported once. Cheap enough to reconcile from updateWindowTitle()
    // (once per frame + on every document switch), which is what keeps it correct across tab
    // switches without a hook in every path that changes the active document.
    void syncExportToMenuItem() {
        if (m_menu == nullptr)
            return;
        auto* item = const_cast<Fl_Menu_Item*>(m_menu->find_item(cbExportTo));
        if (item == nullptr)
            return;
        std::string want;
        if (m_exportTarget.path.empty()) {
            // TRANSLATORS: File menu item, greyed until the document has been exported once.
            want = _("Export to Last File");
        } else {
            // Only '&' needs escaping: the label goes in through Fl_Menu_::replace, which does
            // no '/'-path parsing (that is add()'s rule, and menuEscapeLabel's reason to exist),
            // but the mnemonic rule still applies at draw time.
            std::string name = std::filesystem::path(m_exportTarget.path).filename().string();
            for (std::size_t i = name.find('&'); i != std::string::npos;
                 i = name.find('&', i + 2))
                name.insert(i, 1, '&');
            char buf[320];
            // TRANSLATORS: File menu item; %s is a file name. One-click re-export of the last
            // export target (GIMP's "Overwrite <file>").
            std::snprintf(buf, sizeof buf, _("Export to %s"), name.c_str());
            want = buf;
        }
        // Greying first: replace() is allowed to reallocate the item array, which would leave
        // `item` dangling.
        const bool enable = !m_exportTarget.path.empty();
        bool changed = false;
        if (enable != (item->active() != 0)) {
            if (enable)
                item->activate();
            else
                item->deactivate();
            changed = true;
        }
        if (want != m_exportToLabel) {
            m_exportToLabel = std::move(want); // the cache that keeps this a string compare
            // Fl_Menu_::replace, NOT Fl_Menu_Item::label: the item array owns its label strings
            // (alloc > 1) and frees them in clear()/~Fl_Menu_, so writing a foreign pointer into
            // `text` would have it free()d on window teardown. replace() frees the old and
            // strdup's the new. Qualified like rebuildRecentMenu's insert/remove, so the macOS
            // system menu is rebuilt once by refreshMenuBar() rather than per call.
            const int index = static_cast<int>(item - m_menu->menu());
            m_menu->Fl_Menu_::replace(index, m_exportToLabel.c_str());
            changed = true;
        }
        if (changed)
            refreshMenuBar();
    }

    // ---- The S53-b dynamic Type / Layer rows ----------------------------------------------------
    //
    // Three kinds of state have to reach the menu bar: the four Type radio groups' dots, the
    // flip-flop labels (Disable/Enable Mask, Unlink/Link Mask, Hide/Show Layer, Lock/Unlock
    // Layer), and the greying of every row whose target does not exist. All of it is reconciled
    // ONCE PER FRAME from updateWindowTitle -- the same seam "Export to <file>" rides -- so it
    // stays right across tab switches, undo, canvas clicks and panel edits without a hook in each.
    //
    // A fingerprint gates the work: publishing a menu edit rebuilds the entire macOS system menu,
    // so the usual frame must do nothing at all.
    struct DynamicMenuState {
        int writingMode = -1;  // -1 = no text target (the group is greyed)
        int antiAlias = -1;
        int kerning = -1;
        int direction = -1;
        bool textTarget = false;   // a text layer is active / being edited
        bool textSession = false;  // a live Type session (the two panel rows need one)
        bool textOnPath = false;
        bool hasLayer = false;
        bool hasMask = false;
        bool maskEnabled = false;
        bool maskLinked = false;
        bool layerVisible = false;
        bool layerLocked = false;
        bool canRasterize = false;
        bool canConvertToPath = false;
        bool canFlattenToPath = false;
        bool combineReady = false;
        bool operator==(const DynamicMenuState&) const = default;
    };

    [[nodiscard]] DynamicMenuState readDynamicMenuState() {
        DynamicMenuState s;
        if (!m_document)
            return s;
        if (const core::TextLayer* tl = typeMenuTarget(/*narrate=*/false)) {
            const core::text::TextBlock& b = tl->block();
            s.textTarget = true;
            s.writingMode = static_cast<int>(b.writingMode);
            s.antiAlias = static_cast<int>(b.aa);
            // Kerning and direction are per-run / per-paragraph. The menu shows the FIRST one --
            // a radio dot is a single answer, and a mixed block simply reads as whatever it
            // starts with until the user picks (which then applies to the whole block).
            s.kerning = static_cast<int>(b.runs.empty() ? b.emptyStyle.kerning
                                                        : b.runs.front().style.kerning);
            s.direction = static_cast<int>(b.paragraphs.empty()
                                               ? core::text::Paragraph::Direction::Auto
                                               : b.paragraphs.front().direction);
            s.textOnPath = b.pathFit.has_value();
        }
        s.textSession =
            m_canvas != nullptr && m_canvas->textEditTarget() != core::kInvalidLayerId;
        if (const core::Layer* l = activeLayerPtr()) {
            s.hasLayer = true;
            s.layerVisible = l->visible();
            s.layerLocked = l->locked();
            s.hasMask = l->hasMask();
            if (const core::RasterMask* mk = l->mask(); mk != nullptr) {
                s.maskEnabled = mk->enabled;
                s.maskLinked = mk->linked;
            }
            // The same kind rules the layer-row context menu applies (layer_panel.cpp): a raster
            // is already rasterized and an adjustment has no pixels; only text and shapes carry
            // an outline to convert.
            s.canRasterize = l->kind() != core::LayerKind::Raster &&
                             l->kind() != core::LayerKind::Adjustment;
            const auto* vl = l->as<core::VectorLayer>();
            s.canConvertToPath = l->as<core::TextLayer>() != nullptr ||
                                 (vl != nullptr && vl->hasObject());
            s.canFlattenToPath = vl != nullptr && vl->hasObject();
        }
        s.combineReady = combinePathOperands().size() >= 2;
        return s;
    }

    void syncDynamicMenuItems() {
        if (m_menu == nullptr)
            return;
        const DynamicMenuState s = readDynamicMenuState();
        if (s == m_dynamicMenuState)
            return;
        m_dynamicMenuState = s;

        // ⚠ Fl_Menu_::replace may REALLOCATE the item array, so every helper re-finds its item by
        // callback rather than holding a pointer across calls.
        const auto enable = [this](Fl_Callback* cb, bool on) {
            auto* it = const_cast<Fl_Menu_Item*>(m_menu->find_item(cb));
            if (it == nullptr)
                return;
            if (on)
                it->activate();
            else
                it->deactivate();
        };
        const auto radio = [this](Fl_Callback* cb, bool on, bool selected) {
            auto* it = const_cast<Fl_Menu_Item*>(m_menu->find_item(cb));
            if (it == nullptr)
                return;
            if (on)
                it->activate();
            else
                it->deactivate();
            if (selected)
                it->setonly(); // turns this radio on and clears the rest of its group
        };
        const auto relabel = [this](Fl_Callback* cb, const char* text) {
            const Fl_Menu_Item* it = m_menu->find_item(cb);
            if (it == nullptr)
                return;
            // Fl_Menu_::replace, NEVER Fl_Menu_Item::label: the array owns its label strings and
            // frees them, so writing a foreign pointer into `text` would have it free()d on
            // teardown. Qualified so the macOS system menu is rebuilt once, at the end.
            m_menu->Fl_Menu_::replace(static_cast<int>(it - m_menu->menu()), text);
        };

        // ---- Type ----
        enable(cbRasterizeType, s.textTarget);
        enable(cbTypeToShape, s.textTarget);
        enable(cbTypeCharPanel, s.textSession);
        enable(cbType3dPanel, s.textSession);
        enable(cbTypeToPointText, s.textTarget);
        enable(cbTypeToAreaText, s.textTarget);
        enable(cbTypeOnPath, s.textTarget);
        enable(cbTypeReleaseFromPath, s.textTarget && s.textOnPath);
        enable(cbTypeWorkPath, s.textTarget);
        radio(cbTypeWritingMode<core::text::WritingMode::HorizontalTB>, s.textTarget,
              s.writingMode == static_cast<int>(core::text::WritingMode::HorizontalTB));
        radio(cbTypeWritingMode<core::text::WritingMode::VerticalRL>, s.textTarget,
              s.writingMode == static_cast<int>(core::text::WritingMode::VerticalRL));
        radio(cbTypeWritingMode<core::text::WritingMode::VerticalLR>, s.textTarget,
              s.writingMode == static_cast<int>(core::text::WritingMode::VerticalLR));
        radio(cbTypeAntiAlias<core::text::AntiAlias::None>, s.textTarget,
              s.antiAlias == static_cast<int>(core::text::AntiAlias::None));
        radio(cbTypeAntiAlias<core::text::AntiAlias::Grayscale>, s.textTarget,
              s.antiAlias == static_cast<int>(core::text::AntiAlias::Grayscale));
        radio(cbTypeKerning<core::text::Kerning::Metric>, s.textTarget,
              s.kerning == static_cast<int>(core::text::Kerning::Metric));
        radio(cbTypeKerning<core::text::Kerning::Optical>, s.textTarget,
              s.kerning == static_cast<int>(core::text::Kerning::Optical));
        radio(cbTypeKerning<core::text::Kerning::None>, s.textTarget,
              s.kerning == static_cast<int>(core::text::Kerning::None));
        radio(cbTypeDirection<core::text::Paragraph::Direction::Auto>, s.textTarget,
              s.direction == static_cast<int>(core::text::Paragraph::Direction::Auto));
        radio(cbTypeDirection<core::text::Paragraph::Direction::LTR>, s.textTarget,
              s.direction == static_cast<int>(core::text::Paragraph::Direction::LTR));
        radio(cbTypeDirection<core::text::Paragraph::Direction::RTL>, s.textTarget,
              s.direction == static_cast<int>(core::text::Paragraph::Direction::RTL));

        // ---- Layer ----
        enable(cbRenameLayer, s.hasLayer);
        enable(cbRasterizeLayer, s.hasLayer && s.canRasterize && !s.layerLocked);
        enable(cbConvertToPath, s.hasLayer && s.canConvertToPath && !s.layerLocked);
        enable(cbFlattenToPath, s.hasLayer && s.canFlattenToPath && !s.layerLocked);
        enable(cbAddMask, s.hasLayer && !s.hasMask && !s.layerLocked);
        enable(cbDeleteMask, s.hasLayer && s.hasMask && !s.layerLocked);
        // The two mask flag flips stay live on a LOCKED layer -- they are visibility-like, exactly
        // as the eye is (the layer-row menu's rule).
        enable(cbToggleMaskEnabled, s.hasLayer && s.hasMask);
        enable(cbToggleMaskLinked, s.hasLayer && s.hasMask);
        enable(cbToggleLayerVisible, s.hasLayer);
        enable(cbToggleLayerLocked, s.hasLayer);
        enable(cbBringForward, s.hasLayer);
        enable(cbSendBackward, s.hasLayer);
        enable(cbCombinePaths<core::vec::BoolOp::Union>, s.combineReady);
        enable(cbCombinePaths<core::vec::BoolOp::Subtract>, s.combineReady);
        enable(cbCombinePaths<core::vec::BoolOp::Intersect>, s.combineReady);
        enable(cbCombinePaths<core::vec::BoolOp::Exclude>, s.combineReady);
        // The flip-flop labels last: replace() may move the array, and nothing above holds a
        // pointer past its own call.
        relabel(cbToggleMaskEnabled, s.maskEnabled ? _("Disable Mask") : _("Enable Mask"));
        relabel(cbToggleMaskLinked, s.maskLinked ? _("Unlink Mask") : _("Link Mask"));
        relabel(cbToggleLayerVisible, s.layerVisible ? _("Hide Layer") : _("Show Layer"));
        relabel(cbToggleLayerLocked, s.layerLocked ? _("Unlock Layer") : _("Lock Layer"));

        refreshMenuBar();
    }

    // Push the keymap's current chords onto the menu items (S51-b). This is the ONE path a remap
    // takes, and start-up takes it too (configurePicker, after the persisted overrides are loaded),
    // so it cannot rot into a code path only the settings dialog exercises.
    //
    // Items are found by CALLBACK, never by index: rebuildRecentMenu inserts rows into File ▸ Open
    // Recent and shifts everything after it, while a callback is a stable handle (the same one
    // syncDynamicMenuItems and the badge predicate use). And it ends in refreshMenuBar(), which is
    // MenuBar::update() -- the path that rebuilds the macOS SYSTEM menu bar from the item array and
    // re-attaches the badges FLTK's mirror drops. A remap that wrote the array without going through
    // there would change the shortcut text on Linux and leave the ⌘-equivalents stale on macOS.
    void applyKeymapToMenu() {
        if (m_menu == nullptr)
            return;
        for (const auto& [actionId, owner] : menuAccelOwners()) {
            auto* it = const_cast<Fl_Menu_Item*>(m_menu->find_item(owner));
            if (it == nullptr) {
                // Loud, because it means the accelFor() row and the item parted company -- the
                // accelerator would then be frozen at whatever buildMenu installed.
                uiLog().warn("keymap: no menu item owns action {}", actionId);
                continue;
            }
            it->shortcut(m_keymap.accel(actionId));
        }
        // The text-editor fence names ACTIONS (menu_bar.hpp); resolve them NOW, so a remapped chord
        // is the one refused behind a live caret rather than the chord it used to be.
        std::vector<int> fence;
        fence.reserve(textEditorGuardedActions().size());
        for (const std::string_view id : textEditorGuardedActions())
            fence.push_back(m_keymap.accel(id));
        m_menu->setTextEditorGuardedShortcuts(std::move(fence));
        refreshMenuBar();
    }

    // Filter ▸ Last Filter: remember what the Filter menu last inserted and switch the row on the
    // first time (there is no "last" before then). App-global on purpose -- "again" means the last
    // thing you did, not the last thing you did in this tab.
    void noteFilterUsed(core::AdjustmentKind kind) {
        const bool first = !m_lastFilterKind.has_value();
        m_lastFilterKind = kind;
        if (!first || m_menu == nullptr)
            return;
        if (auto* it = const_cast<Fl_Menu_Item*>(m_menu->find_item(cbLastFilter)))
            it->activate();
        refreshMenuBar();
    }
    void repeatLastFilter() {
        if (!m_lastFilterKind) {
            transientStatus(_("No filter has been applied yet"));
            return;
        }
        insertAdjustmentLayer(*m_lastFilterKind);
    }

    // S39-b Inpaint (async): fill the masked region (`hole`, coverage > 0) of layer `id` with the
    // inpainting engine OFF THE UI THREAD, showing a determinate progress bar + cancel X in the
    // status bar and a throttled live preview on the canvas, with every other control disabled
    // until it finishes. The canvas has already dropped any red overlay (restored the pre-stroke
    // pixels), so layer `id`'s current pixels are the engine input. The result lands as one
    // undoable edit; a cancel or failure restores the original pixels.
    // Resolve m_inpaintParams from the active backend + preset, then layer any custom overrides on
    // top. Called at start-up and whenever the Settings dialog changes the backend / preset / a
    // knob.
    void recomputeInpaintParams() {
        const auto* b = m_inpaintEngine.backend(m_inpaintBackendId);
        if (b == nullptr)
            return;
        m_inpaintParams = core::inpaint::paramsForPreset(*b, m_inpaintPresetId);
        for (const auto& [key, value] : m_inpaintOverrides) // empty unless preset == "custom"
            b->applyParam(m_inpaintParams, key, value);
    }

    // Persist the keybinding overrides (S51-b). The whole SPARSE map goes in one write, replacing
    // whatever was there: a remap can also UNBIND another action (the reassign answer), so writing
    // only the row that was touched would leave the loser's stale chord on disk.
    void persistKeymap() {
        persistSetting([&](common::Settings& s) { s.keymap = m_keymap.overrides(); }, "keybindings");
    }

    // Persist the whole inpaint selection (backend + preset + custom overrides) in one write.
    void persistInpaint() {
        persistSetting(
            [&](common::Settings& s) {
                s.inpaintBackend = m_inpaintBackendId;
                s.inpaintPreset = m_inpaintPresetId;
                s.inpaintParams = m_inpaintOverrides;
            },
            "inpaint settings");
    }

    void runInpaint(core::LayerId id, core::Selection hole) {
        if (!m_document || m_inpaintRunning)
            return; // one inpaint at a time
        core::Layer* layer = m_document->find(id);
        auto* raster = layer != nullptr ? layer->as<core::RasterLayer>() : nullptr;
        if (raster == nullptr)
            return;

        auto job = std::make_unique<InpaintJob>();
        job->layerId = id;
        job->input = common::toFloat(raster->image()); // owned: the worker borrows it by reference
        job->originalPixels = raster->image();         // restore target (we paint previews live)
        job->hole = std::move(hole);
        job->params = m_inpaintParams; // active backend's preset (+ any live advanced override)
        // The hole bbox (clamped) sizes the preview buffer; everything outside it stays = input.
        if (const auto b = job->hole.bounds()) {
            const double iw = job->input.width, ih = job->input.height;
            job->bx0 = static_cast<std::uint32_t>(std::clamp(b->x, 0.0, iw));
            job->by0 = static_cast<std::uint32_t>(std::clamp(b->y, 0.0, ih));
            const auto x1 = static_cast<std::uint32_t>(std::clamp(b->x + b->w, 0.0, iw));
            const auto y1 = static_cast<std::uint32_t>(std::clamp(b->y + b->h, 0.0, ih));
            job->bw = x1 > job->bx0 ? x1 - job->bx0 : 0;
            job->bh = y1 > job->by0 ? y1 - job->by0 : 0;
        }
        if (job->bw > 0 && job->bh > 0)
            job->preview = common::ImageF(job->bw, job->bh);

        // Optionally preview the neighbourhood the active backend will analyse (S39): a faint green
        // wash on the canvas, when "Show sampled area while working" is enabled. The region comes
        // from the backend (analysedRegion), so this works for any backend that reports one.
        if (job->params.showSampleArea) {
            if (const auto region = m_inpaintEngine.analysedRegion(
                    job->input.width, job->input.height, job->hole.bounds(), job->params))
                m_canvas->setInpaintSampleArea(*region);
        }

        m_inpaintJob = std::move(job);
        m_inpaintRunning = true;
        setMainControlsEnabled(false); // nothing else can be done while it runs
        m_statusBar->onProgressCancel([this] {
            if (m_inpaintJob)
                m_inpaintJob->cancelRequested.store(true);
        });
        m_statusBar->setProgress(0.0f, _("Analyzing"));

        InpaintJob* j = m_inpaintJob.get();
        j->worker = std::thread([this, j] {
            const core::inpaint::InpaintRequest req{j->input, j->hole, j->params,
                                                    &j->cancelRequested};
            const core::inpaint::ProgressFn fn =
                [j](const core::inpaint::InpaintProgress& p) -> bool {
                {
                    std::lock_guard<std::mutex> lk(j->mutex);
                    j->fraction = p.fraction;
                    j->stage.assign(p.stage);
                    if (p.preview != nullptr && j->bw > 0 && j->bh > 0) {
                        // Copy ONLY the hole bbox out of the (full-image) preview — the pointer is
                        // valid only during this call, and the rest of the image never changes.
                        for (std::uint32_t yy = 0; yy < j->bh; ++yy) {
                            for (std::uint32_t xx = 0; xx < j->bw; ++xx) {
                                j->preview.set(xx, yy, p.preview->at(j->bx0 + xx, j->by0 + yy));
                            }
                        }
                        j->hasNewPreview = true;
                    }
                }
                return !j->cancelRequested.load();
            };
            j->result = m_inpaintEngine.run(req, fn);
            j->done.store(true);
        });
        Fl::add_timeout(kInpaintPollSeconds, inpaintPollTimer, this);
    }

    // Map an engine stage id to a localized label for the progress strip.
    [[nodiscard]] static std::string localizedInpaintStage(const std::string& s) {
        if (s == "Analyzing")
            return _("Analyzing");
        if (s == "Solving")
            return _("Solving");
        if (s == "Blending")
            return _("Blending");
        return s;
    }

    static void inpaintPollTimer(void* self) { static_cast<MainWindow*>(self)->pollInpaint(); }

    // UI-thread poll (~20 Hz): pull the latest progress + preview the worker published and reflect
    // them; naturally throttles the live preview (only the newest frame per tick is applied).
    void pollInpaint() {
        if (!m_inpaintJob)
            return;
        InpaintJob* j = m_inpaintJob.get();
        float frac = 0.0f;
        std::string stage;
        bool previewApplied = false;
        {
            std::lock_guard<std::mutex> lk(j->mutex);
            frac = j->fraction;
            stage = j->stage;
            // The progress bar updates every poll, but the expensive part — blitting the preview
            // and recompositing the whole document — is throttled by wall-clock so a fast-streaming
            // stage (the Poisson blend) can't saturate the UI thread. A throttled frame is left
            // pending (hasNewPreview stays set); the next eligible poll applies the latest one.
            if (j->hasNewPreview) {
                const auto now = std::chrono::steady_clock::now();
                const double since =
                    std::chrono::duration<double>(now - m_lastInpaintPreview).count();
                // Budget preview recomposites against their MEASURED cost (m_lastRecompositeCost,
                // timed in onFrame): wait at least kBudgetFactor× that, so they take ≤1/factor of
                // the UI thread and a big-photo recomposite doesn't pin it. Also gate on no
                // recomposite already pending so we never queue faster than the frame loop drains
                // it.
                const double gate = std::max(kInpaintPreviewMinSeconds,
                                             kInpaintPreviewBudgetFactor * m_lastRecompositeCost);
                if (since >= gate && !m_recompositePending) {
                    // Blit the (bbox-sized) preview to the live layer under the lock so the worker
                    // can't overwrite it mid-copy. The layer is UI-thread-owned; the worker never
                    // touches it.
                    blitPreviewToLayer();
                    j->hasNewPreview = false;
                    previewApplied = true;
                    m_lastInpaintPreview = now;
                }
            }
        }
        if (m_statusBar)
            m_statusBar->setProgress(frac, localizedInpaintStage(stage));
        if (previewApplied)
            // S60-a: only the hole's bounding box changed, so recomposite + re-upload just that
            // rect (the whole-document path was the inpaint-preview hiccup this perf pass kills).
            recompositeRegion({static_cast<double>(j->bx0), static_cast<double>(j->by0),
                               static_cast<double>(j->bw), static_cast<double>(j->bh)});
        if (j->done.load()) {
            finishInpaint();
            return;
        }
        Fl::repeat_timeout(kInpaintPollSeconds, inpaintPollTimer, this);
    }

    // Paint the latest intermediate hole region onto the live layer (non-undoable) for the preview.
    // Only the hole's bounding box differs from the input, so j->preview is just that box. Caller
    // holds j->mutex (the worker mustn't overwrite j->preview mid-blit).
    void blitPreviewToLayer() {
        InpaintJob* j = m_inpaintJob.get();
        if (j == nullptr || j->bw == 0 || j->bh == 0)
            return;
        core::Layer* layer = m_document ? m_document->find(j->layerId) : nullptr;
        auto* raster = layer != nullptr ? layer->as<core::RasterLayer>() : nullptr;
        if (raster == nullptr)
            return;
        common::Image& dst = raster->image();
        if (dst.width != j->input.width || dst.height != j->input.height)
            return;
        const auto to8 = [](float v) {
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            return static_cast<std::uint8_t>(std::lround(v * 255.0f));
        };
        for (std::uint32_t yy = 0; yy < j->bh; ++yy) {
            for (std::uint32_t xx = 0; xx < j->bw; ++xx) {
                const common::ColorF c = j->preview.at(xx, yy);
                const std::size_t p =
                    (static_cast<std::size_t>(j->by0 + yy) * dst.width + (j->bx0 + xx)) * 4;
                dst.rgba[p] = to8(c.r);
                dst.rgba[p + 1] = to8(c.g);
                dst.rgba[p + 2] = to8(c.b);
                dst.rgba[p + 3] = to8(c.a);
            }
        }
        raster->invalidateContentBounds();
        // The hole's bounding box is already layer-local (j->input shares the layer's grid, checked
        // above), so the resident lane re-uploads the box rather than the layer (S60-a item 13).
        noteLayerPixelsChanged(raster->id(),
                               common::Rect{static_cast<double>(j->bx0), static_cast<double>(j->by0),
                                            static_cast<double>(j->bw),
                                            static_cast<double>(j->bh)});
    }

    // The worker finished (or was cancelled): join it, restore the original pixels (we'd painted
    // previews over them) and then either land the result as one undoable edit or, on
    // cancel/failure, leave the layer as it was. Re-enables the UI and tears down the progress
    // strip.
    void finishInpaint() {
        if (!m_inpaintJob)
            return;
        m_canvas->clearInpaintSampleArea(); // the run is over: drop the sample-area wash (S39)
        InpaintJob* j = m_inpaintJob.get();
        if (j->worker.joinable())
            j->worker.join();
        const bool cancelled = j->cancelRequested.load();

        if (core::Layer* layer = m_document ? m_document->find(j->layerId) : nullptr) {
            if (auto* raster = layer->as<core::RasterLayer>()) {
                raster->image() = j->originalPixels; // undo the live-preview mutations
                raster->invalidateContentBounds();
            }
        }

        m_statusBar->hideProgress();
        m_statusBar->onProgressCancel(nullptr);
        setMainControlsEnabled(true);
        m_inpaintRunning = false;

        if (!cancelled && j->result.ok) {
            // Only the hole's bounding box changed, so store + recomposite just that region
            // (S60-a): a full-layer command + full recomposite hiccuped the whole UI on a big
            // canvas. Original pixels are restored, so the command captures original->result for
            // that region's undo.
            const common::Image full = common::toImage8(j->result.image);
            common::Image region = common::copyRegion(full, static_cast<long>(j->bx0),
                                                      static_cast<long>(j->by0), j->bw, j->bh);
            pushScopedPixelEdit(std::make_unique<core::SetLayerPixelsCommand>(
                j->layerId, std::move(region), static_cast<long>(j->bx0),
                static_cast<long>(j->by0)));
        } else {
            if (!cancelled && !j->result.ok) {
                uiLog().warn("inpaint failed: {}", j->result.detail);
                transientStatus(j->result.detail.empty()
                                    ? std::string(_("Inpaint failed"))
                                    : _("Inpaint failed: ") + j->result.detail);
            } else if (cancelled) {
                transientStatus(_("Inpaint cancelled"));
            }
            requestRecomposite(/*fitView=*/false); // the restored pixels return to screen
        }
        m_inpaintJob.reset();
    }

    // Lock the editing UI while an inpaint runs. Every direct child is disabled EXCEPT the status
    // bar (keeps its cancel X live) and the canvas — the canvas stays active so pan/zoom/rotate
    // keep working; it's put in "inpaint busy" mode instead, which blocks editing gestures and
    // padlocks the brush reticle. (A full pass over individual controls' disabled states is a later
    // session.)
    void setMainControlsEnabled(bool on) {
        for (int i = 0; i < children(); ++i) {
            Fl_Widget* c = child(i);
            if (c == static_cast<Fl_Widget*>(m_statusBar) || c == static_cast<Fl_Widget*>(m_canvas))
                continue;
            if (on)
                c->activate();
            else
                c->deactivate();
        }
        if (m_canvas)
            m_canvas->setInpaintBusy(!on);
        if (m_settingsDialog) // freeze the Inpainting pane while a run is in flight
            m_settingsDialog->setInpaintEngineBusy(!on);
    }

    // ---- Crop expansion Inpaint fill (S16-f; research doc §3.10 guardrails) --------------------
    //
    // The user pre-chose Fill = Inpaint on the crop bar and Applied a rect reaching past the
    // canvas. The worker heals the expansion ring on a copy of the flatten (the rebase preserves
    // world positions shifted by (dx,dy), so the old flatten blitted at its post-crop offset IS
    // the post-crop composite wherever the old canvas showed anything) and the result lands
    // through the SAME buildCropCommand as the solid fills — one undo step, healed pixels on the
    // bottom of the stack. No preview-image chooser, no automatic trigger: mode picked in a
    // persistent dropdown, run only on the explicit Apply.

    void startCropExpandFill(const CropPixels& px, bool deletePixels) {
        if (!m_document || m_inpaintRunning || m_recomposeRunning)
            return;
        // Audit A9, through the named readback seam: a whole-canvas read, once per discrete user
        // action, so a blocking transfer is invisible inside it (S60-a item 12).
        const common::Image& flat =
            hostComposite(render::consumers::kCropExpandFill, render::Freshness::Current);
        if (flat.empty() || flat.width != m_document->width() ||
            flat.height != m_document->height()) {
            transientStatus(_("Try again in a moment (the canvas is refreshing)"));
            return; // mid-resize transient: the next frame's recomposite refreshes it
        }
        auto job = std::make_unique<CropExpandJob>();
        job->px = px;
        job->deletePixels = deletePixels;
        job->params = m_inpaintParams; // active backend + preset, like every engine run
        const long dx = -px.x;
        const long dy = -px.y;
        common::Image seed(px.w, px.h);
        common::blitRegion(seed, flat, dx, dy);
        job->input = common::toFloat(seed);
        core::Selection ring(px.w, px.h);
        std::vector<std::uint8_t>& rd = ring.data();
        const long oldW = static_cast<long>(m_document->width());
        const long oldH = static_cast<long>(m_document->height());
        for (long y = 0; y < static_cast<long>(px.h); ++y) {
            const bool rowInOld = y >= dy && y < dy + oldH;
            for (long x = 0; x < static_cast<long>(px.w); ++x) {
                if (!(rowInOld && x >= dx && x < dx + oldW))
                    rd[static_cast<std::size_t>(y) * px.w + static_cast<std::size_t>(x)] = 255;
            }
        }
        job->hole = std::move(ring);

        m_cropExpandJob = std::move(job);
        m_inpaintRunning = true; // one engine run at a time, sharing the inpaint busy-state
        setMainControlsEnabled(false);
        m_statusBar->onProgressCancel([this] {
            if (m_cropExpandJob)
                m_cropExpandJob->cancelRequested.store(true);
        });
        m_statusBar->setProgress(0.0f, _("Analyzing"));

        CropExpandJob* j = m_cropExpandJob.get();
        j->worker = std::thread([this, j] {
            const core::inpaint::InpaintRequest req{j->input, j->hole, j->params,
                                                    &j->cancelRequested};
            const core::inpaint::ProgressFn fn =
                [j](const core::inpaint::InpaintProgress& p) -> bool {
                {
                    std::lock_guard<std::mutex> lk(j->mutex);
                    j->fraction = p.fraction;
                    j->stage.assign(p.stage);
                }
                return !j->cancelRequested.load();
            };
            j->result = m_inpaintEngine.run(req, fn);
            j->done.store(true);
        });
        Fl::add_timeout(kInpaintPollSeconds, cropExpandPollTimer, this);
    }

    static void cropExpandPollTimer(void* self) {
        static_cast<MainWindow*>(self)->pollCropExpand();
    }

    void pollCropExpand() {
        if (!m_cropExpandJob)
            return;
        CropExpandJob* j = m_cropExpandJob.get();
        float frac = 0.0f;
        std::string stage;
        {
            std::lock_guard<std::mutex> lk(j->mutex);
            frac = j->fraction;
            stage = j->stage;
        }
        if (m_statusBar)
            m_statusBar->setProgress(frac, localizedInpaintStage(stage));
        if (j->done.load()) {
            finishCropExpandFill();
            return;
        }
        Fl::repeat_timeout(kInpaintPollSeconds, cropExpandPollTimer, this);
    }

    void finishCropExpandFill() {
        if (!m_cropExpandJob)
            return;
        CropExpandJob* j = m_cropExpandJob.get();
        if (j->worker.joinable())
            j->worker.join();
        const bool cancelled = j->cancelRequested.load();
        m_statusBar->hideProgress();
        m_statusBar->onProgressCancel(nullptr);
        setMainControlsEnabled(true);
        m_inpaintRunning = false;

        if (!cancelled && j->result.ok && m_document) {
            common::Image healed = common::toImage8(j->result.image);
            // The fill sits at the BOTTOM of the stack, opaque like the solid modes (engines
            // preserve the seed's alpha, which is 0 in the ring). Only the ring is ever read.
            for (std::size_t i = 3; i < healed.rgba.size(); i += 4)
                healed.rgba[i] = 255;
            render::CropFill fill{{0, 0, 0, 255}, _("Canvas fill")};
            fill.pixels = std::move(healed);
            m_document->commands().push(render::buildCropCommand(
                *m_document, j->px.x, j->px.y, j->px.w, j->px.h, j->deletePixels, fill));
            syncAfterEdit();
            char buf[128];
            std::snprintf(buf, sizeof buf, _("Resized canvas to %u × %u px"), j->px.w, j->px.h);
            transientStatus(buf);
            // S16-p (opt-in), same as the synchronous apply: a one-off crop returns to the
            // previous tool — but only on success, so a cancel leaves the user in Crop.
            if (m_cropSwitchToolAfterApply && m_tools.active() == ToolId::Crop) {
                const ToolId back =
                    m_tools.previous() == ToolId::Crop ? ToolId::Move : m_tools.previous();
                m_tools.setActive(back);
            }
        } else if (cancelled) {
            transientStatus(_("Inpaint cancelled"));
        } else if (!j->result.ok) {
            uiLog().warn("crop expansion inpaint failed: {}", j->result.detail);
            transientStatus(j->result.detail.empty() ? std::string(_("Inpaint failed"))
                                                     : _("Inpaint failed: ") + j->result.detail);
        }
        m_cropExpandJob.reset();
    }

    // ---- Smart Recompose (S16-f second tier; docs/smart-recompose-plan.md §1.3–§1.4) ----------
    //
    // The user pressed the bar's "Recompose" button (its offer enabled it: the marked regions
    // cannot all fit a crop window at the chosen ratio, but a rigid placement is feasible). The
    // worker runs prepareRecompose — placement, feathered cuts, inpaint heal (the dominant cost,
    // through the engine FillFn adapter), background window — with progress + cancel exactly
    // like the InpaintJob. On success the assembled preview replaces the canvas display and the
    // canvas enters the REVIEW: placements drag (each nudge re-assembles in ms; the heal never
    // re-runs), Enter / bar-Apply lands ONE undo step, Esc / bar-Cancel returns to the crop
    // suggestion. The user invokes and the user commits — nothing automatic (§3.9 guardrail 4).

    void startRecompose() {
        if (!m_document || m_recomposeRunning || m_inpaintRunning || m_recomposeReview)
            return;
        const auto ask = m_canvas->recomposeRequest();
        if (!ask)
            return;
        // Audit A8: whole canvas, once per user action, named (S60-a item 12).
        const common::Image& flat =
            hostComposite(render::consumers::kSmartRecompose, render::Freshness::Current);
        if (flat.empty() || flat.width != m_document->width() ||
            flat.height != m_document->height())
            return; // mid-resize transient: the next frame's recomposite refreshes it
        auto job = std::make_unique<RecomposeJob>();
        job->src = flat;
        job->aspect = ask->first;
        job->regions.reserve(ask->second.size());
        for (const common::Rect& r : ask->second)
            job->regions.push_back({r, 0.0, core::retarget::KeepRegion::Source::User});
        job->params = m_inpaintParams; // the user's configured inpaint quality heals the holes
        m_recomposeJob = std::move(job);
        m_recomposeRunning = true;
        setMainControlsEnabled(false); // modal like an inpaint run; pan/zoom + cancel stay live
        m_statusBar->onProgressCancel([this] {
            if (m_recomposeJob)
                m_recomposeJob->cancelRequested.store(true);
        });
        m_statusBar->setProgress(0.0f, _("Analyzing"));
        RecomposeJob* j = m_recomposeJob.get();
        j->worker = std::thread([this, j] {
            const core::retarget::FillFn fill = core::retarget::makeInpaintFill(
                m_inpaintEngine, j->params, &j->cancelRequested,
                [j](float f, std::string_view stage) {
                    std::lock_guard<std::mutex> lk(j->mutex);
                    j->fraction = 0.05f + f * 0.9f; // the heal dominates the pipeline
                    j->stage.assign(stage);
                });
            core::retarget::RecomposeStaged staged =
                core::retarget::prepareRecompose(j->src, j->aspect, j->regions, fill);
            // The first assembly runs here too — the seam-blended pass costs a few hundred ms
            // on a big band, which belongs on the worker, not the UI thread.
            if (staged.ok && !j->cancelRequested.load())
                j->preview = core::retarget::assembleRecompose(staged, /*blendSeams=*/true);
            {
                std::lock_guard<std::mutex> lk(j->mutex);
                j->staged = std::move(staged);
                j->fraction = 1.0f;
            }
            j->done.store(true);
        });
        Fl::add_timeout(kInpaintPollSeconds, recomposePollTimer, this);
    }

    static void recomposePollTimer(void* self) { static_cast<MainWindow*>(self)->pollRecompose(); }

    void pollRecompose() {
        RecomposeJob* j = m_recomposeJob.get();
        if (j == nullptr)
            return;
        float frac = 0.0f;
        std::string stage;
        {
            std::lock_guard<std::mutex> lk(j->mutex);
            frac = j->fraction;
            stage = j->stage;
        }
        if (!stage.empty())
            m_statusBar->setProgress(frac, localizedInpaintStage(stage)); // engine stage ids
        if (j->done.load()) {
            finishRecompose();
            return;
        }
        Fl::repeat_timeout(kInpaintPollSeconds, recomposePollTimer, this);
    }

    void finishRecompose() {
        RecomposeJob* j = m_recomposeJob.get();
        if (j == nullptr)
            return;
        if (j->worker.joinable())
            j->worker.join();
        const bool cancelled = j->cancelRequested.load();
        core::retarget::RecomposeStaged staged = std::move(j->staged);
        common::Image preview = std::move(j->preview);
        m_recomposeJob.reset();
        m_statusBar->hideProgress();
        m_statusBar->onProgressCancel(nullptr);
        setMainControlsEnabled(true);
        m_recomposeRunning = false;
        if (cancelled) {
            transientStatus(_("Recompose cancelled"));
            return;
        }
        if (!staged.ok) {
            uiLog().warn("recompose failed: {}", staged.detail);
            transientStatus(_("Recompose failed — try a different ratio or fewer regions"));
            return;
        }
        m_recomposeStaged = std::move(staged);
        m_recomposePreview = !preview.empty()
                                 ? std::move(preview)
                                 : core::retarget::assembleRecompose(m_recomposeStaged);
        m_recomposeNudged = false;
        m_recomposeReview = true;
        m_canvas->enterRecomposeReview(m_recomposeStaged.placed);
        presentComposite(m_recomposePreview, /*fitView=*/true);
        transientStatus(_("Reviewing: drag a region to adjust — Enter applies, Esc cancels"));
    }

    // A placement chip moved in the review: write it through and re-assemble (no heal re-run).
    // Feather-only at drag rate; the seam-blended pass re-runs once before Apply lands.
    void nudgeRecompose(std::size_t index, common::Vec2 topLeft) {
        if (!m_recomposeReview || index >= m_recomposeStaged.placed.size())
            return;
        common::Rect& r = m_recomposeStaged.placed[index];
        r.x = topLeft.x;
        r.y = topLeft.y;
        m_recomposeNudged = true;
        m_recomposePreview =
            core::retarget::assembleRecompose(m_recomposeStaged, /*blendSeams=*/false);
        presentComposite(m_recomposePreview, /*fitView=*/false);
    }

    // Land the reviewed result as ONE undo step (fork F-b, mode (b)): canvas at the target
    // size, the original layers preserved but hidden beneath, the flattened result a new top
    // layer. A single undo restores everything.
    void applyRecompose() {
        if (!m_recomposeReview || !m_document || m_recomposePreview.empty())
            return;
        // Nudged since the last blended assembly: land the polished version, not the drag-rate
        // feather (a few hundred ms at worst on a huge band — acceptable at commit).
        if (m_recomposeNudged)
            m_recomposePreview = core::retarget::assembleRecompose(m_recomposeStaged);
        const std::uint32_t w = m_recomposePreview.width;
        const std::uint32_t h = m_recomposePreview.height;
        auto cmd = std::make_unique<core::CompositeCommand>("Recompose");
        cmd->add(std::make_unique<core::ResizeCanvasCommand>(w, h));
        for (const std::unique_ptr<core::Layer>& child : m_document->root().children())
            if (child->visible())
                cmd->add(std::make_unique<core::SetVisibleCommand>(child->id(), false));
        std::unique_ptr<core::RasterLayer> layer = m_document->makeRaster(_("Recomposed"), w, h);
        layer->image() = m_recomposePreview; // copied: the preview stays shown until the exit
        const core::LayerId newId = layer->id();
        cmd->add(std::make_unique<core::AddLayerCommand>(
            m_document->root().id(), m_document->root().childCount(), std::move(layer)));
        // A selection of the old canvas means nothing on the recomposed one.
        if (!m_document->selection().isEmpty())
            cmd->add(std::make_unique<core::SetSelectionCommand>(core::Selection{}));
        exitRecomposeReviewState();
        m_canvas->resetCropTool(); // the staged rect belonged to the old canvas
        m_document->commands().push(std::move(cmd));
        syncAfterEdit();
        if (m_layerPanel)
            m_layerPanel->setActive(newId);
        char buf[128];
        std::snprintf(buf, sizeof buf, _("Recomposed to %u × %u px"), w, h);
        transientStatus(buf);
    }

    // Esc / bar-Cancel / tool switch: drop the preview, restore the document display; the crop
    // suggestion (still staged beneath) shows again.
    void cancelRecomposeReview() {
        if (!m_recomposeReview)
            return;
        exitRecomposeReviewState();
        if (m_tiles != nullptr)
            // The review held the canvas; the mirror behind it may be several revisions old, so ask
            // for a fresh frame rather than re-pushing pixels the resident lane no longer owns.
            requestRecomposite(/*fitView=*/true);
        else if (m_canvas != nullptr && !m_lastComposite.empty())
            presentComposite(m_lastComposite, /*fitView=*/true);
        transientStatus(_("Recompose discarded"));
    }

    void exitRecomposeReviewState() {
        m_recomposeReview = false;
        m_recomposeNudged = false;
        m_recomposeStaged = {};
        m_recomposePreview = {};
        if (m_canvas)
            m_canvas->exitRecomposeReview();
    }

    // The canvas recomputed the Recompose offer (per frame, on change only): grey/enable the bar
    // button. A direct option write — not a value change, so no options-changed feedback loop.
    void setRecomposeOffer(bool on) {
        Tool* crop = m_tools.find(ToolId::Crop);
        if (crop == nullptr)
            return;
        for (ToolOption& o : crop->options())
            if (o.id == "recompose") {
                if (o.enabled == on)
                    return;
                o.enabled = on;
                if (m_optionsBar)
                    m_optionsBar->syncValues(); // pushes enabled state into the live control
                return;
            }
    }

    // View -> Rulers: show/hide the canvas ruler gutter (a runtime toggle, not persisted). Reflows
    // the canvas through applyDockWidth() and redraws the strips at their new positions.
    void setRulersVisible(bool on) {
        if (m_rulersVisible == on)
            return;
        m_rulersVisible = on;
        applyDockWidth();
        if (m_rulersVisible && m_rulerH != nullptr) {
            m_rulerH->redraw();
            m_rulerV->redraw();
        }
    }

    // View -> Show Guides / Lock Guides: per-document view flags. Show toggles guide visibility;
    // Lock prevents grabbing/moving them. Both are plain flag flips (not undoable, like a View
    // toggle); a frame refresh re-renders the overlay.
    void setShowGuides(bool on) {
        if (m_document == nullptr)
            return;
        m_document->setShowGuides(on);
        requestFrame();
    }
    void setLockGuides(bool on) {
        if (m_document != nullptr)
            m_document->setLockGuides(on);
    }
    // View -> Clear Guides: remove every guide as one undoable step.
    void clearGuides() {
        if (m_document != nullptr && !m_document->guides().empty())
            m_document->commands().push(std::make_unique<core::ClearGuidesCommand>());
        requestFrame();
    }
    // Sync the per-document guide-menu checkmarks (Show/Lock) to the active document's flags. Called
    // on tab switch / document open (adoptActiveDocument), since these flags travel with the doc.
    void syncGuideMenuState() {
        if (m_menu == nullptr || m_document == nullptr)
            return;
        // Look items up by their (unique) callback, not a path string -- exact + mnemonic-proof.
        if (auto* it = const_cast<Fl_Menu_Item*>(m_menu->find_item(cbShowGuides))) {
            if (m_document->showGuides())
                it->set();
            else
                it->clear();
        }
        if (auto* it = const_cast<Fl_Menu_Item*>(m_menu->find_item(cbLockGuides))) {
            if (m_document->lockGuides())
                it->set();
            else
                it->clear();
        }
        refreshMenuBar(); // the themed pop-up reads these live; the macOS snapshot must be rebuilt
    }

    // Arrange menu: align / distribute the current layer selection as one undoable step. Each
    // target's document-space AABB drives the maths -- core::arrangeBounds, which is the content
    // box, the MASK's coverage box, or their intersection, so a masked adjustment/filter (no content
    // at all) and a canvas-filling generator/gradient (content the size of the canvas) finally have
    // a position the menu can line up. The resulting doc-space translation is converted back to the
    // layer's parent-local frame (the pushSelectionTransform recipe: invParent * delta * world).
    void alignSelection(core::AlignEdge edge) {
        applyArrange([this, edge](const std::vector<common::Rect>& boxes)
                         -> std::vector<common::Vec2> {
            // A single layer aligns to the CANVAS (its own union box would make every align a
            // no-op); two or more keep the align-to-selection convention (arrange.hpp).
            if (boxes.size() == 1 && m_document != nullptr) {
                const common::Rect canvas{0.0, 0.0, static_cast<double>(m_document->width()),
                                          static_cast<double>(m_document->height())};
                return core::alignTranslations(boxes, edge, canvas);
            }
            return core::alignTranslations(boxes, edge);
        }, /*minCount=*/1, /*distribute=*/false);
    }
    void distributeSelection(core::DistributeAxis axis) {
        applyArrange([axis](const std::vector<common::Rect>& boxes)
                         -> std::vector<common::Vec2> {
            return core::distributeTranslations(boxes, axis);
        }, /*minCount=*/3, /*distribute=*/true);
    }
    // WHICH layers Arrange acts on, down a four-rung ladder -- a layer selection is made on four
    // different surfaces and this menu used to see only the first:
    //   1-2. the canvas (VulkanCanvas::arrangeTargets): its move selection, else the ACTIVE TOOL's
    //        own edit target -- the shape / path / gradient object bound for editing, or the text
    //        block being typed into. Every one of those tools clears the move selection on its way
    //        in (onToolChanged -> clearMoveTarget), so with anything but Move active Arrange saw an
    //        empty set and did nothing: the user had to switch to Move and re-select what was
    //        already selected.
    //   3.   the Layers panel's own multi-selection -- normally mirrored onto the canvas, but the
    //        panel is the surface that holds it, so ask it rather than assume the mirror survived.
    //   4.   the active layer alone. This is the answer for every tool that binds no layer of its
    //        own (brush, eraser, clone, bucket, eyedropper, the selection tools): they all act on
    //        the panel's active row, and so should Arrange.
    // The result is validated and filtered by core::arrangeTargets -- dead ids, hidden layers and
    // LOCKED layers drop out (a lock refuses transform edits everywhere else in the app; Arrange
    // moved them anyway, which was an oversight).
    [[nodiscard]] std::vector<core::LayerId> arrangeTargetIds() const {
        if (m_document == nullptr)
            return {};
        std::vector<core::LayerId> ids;
        if (m_canvas != nullptr)
            ids = m_canvas->arrangeTargets();
        if (ids.empty() && m_layerPanel != nullptr)
            ids = m_layerPanel->moveSelection();
        if (ids.empty() && m_layerPanel != nullptr &&
            m_layerPanel->activeLayer() != core::kInvalidLayerId) {
            ids.push_back(m_layerPanel->activeLayer());
        }
        return core::arrangeTargets(ids, [this](core::LayerId id) -> const core::Layer* {
            return m_document->find(id);
        });
    }
    void applyArrange(
        const std::function<std::vector<common::Vec2>(const std::vector<common::Rect>&)>& compute,
        std::size_t minCount, bool distribute) {
        if (m_document == nullptr || m_canvas == nullptr)
            return;
        struct Item {
            core::LayerId id;
            common::Rect box;
            core::Layer* layer;
        };
        std::vector<Item> items;
        for (const core::LayerId id : arrangeTargetIds()) {
            core::Layer* l = m_document->find(id);
            if (l == nullptr)
                continue;
            // nullopt = nothing visible to line up (no content and no mask, an empty content box --
            // the skip this loop always had -- or a mask that reveals none of the layer).
            const std::optional<common::Rect> box = core::arrangeBounds(*l);
            if (!box)
                continue;
            items.push_back({id, *box, l});
        }
        if (items.size() < minCount) {
            // No tool is named any more: the selection may come from the Move tool, from any object
            // tool, from the Layers panel or simply from the active layer, so the hint says what is
            // MISSING instead of where to go and get it.
            transientStatus(distribute ? _("Select three or more objects to distribute.")
                                       : _("Select an object to align."));
            return;
        }
        std::vector<common::Rect> boxes;
        boxes.reserve(items.size());
        for (const Item& it : items)
            boxes.push_back(it.box);
        const std::vector<common::Vec2> deltas = compute(boxes);
        std::vector<core::SetTransformsCommand::Entry> entries;
        std::vector<core::SetMaskPlacementCommand::Entry> sheets;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (deltas[i].x == 0.0 && deltas[i].y == 0.0)
                continue; // already in place -- no move, no undo noise
            const std::optional<common::Affine2D> invParent =
                core::parentWorldTransform(*items[i].layer).inverse();
            if (!invParent)
                continue; // a singular ancestor: nothing sane to edit through it
            const common::Affine2D delta =
                common::Affine2D::translation(deltas[i].x, deltas[i].y);
            // The transform is written for EVERY target, including an adjustment/filter layer whose
            // transform composites nothing on its own: it is what carries a LINKED mask
            // (maskToDocument folds the layer's world transform for it), so on those kinds it is the
            // only thing that moves them at all. Where it is inert it is harmless bookkeeping, and
            // one recipe for every kind is worth more than a special case that has to stay in sync.
            entries.push_back(
                {items[i].id, *invParent * delta * core::worldTransform(*items[i].layer)});
            // An UNLINKED mask deliberately does NOT ride that transform, so it needs the same
            // doc-space delta applied to its sheet. Without this the pixels slide out from under a
            // stationary mask and the visible blob does not move at all -- the whole defect for a
            // masked generator or filter.
            if (const std::optional<common::Affine2D> placement =
                    core::translatedMaskPlacement(*items[i].layer, deltas[i])) {
                sheets.push_back({items[i].id, *placement});
            }
        }
        if (entries.empty())
            return; // everything was already aligned/distributed: no empty undo step
        if (sheets.empty()) {
            m_document->commands().push(
                std::make_unique<core::SetTransformsCommand>(std::move(entries), /*coalesceId=*/0));
        } else {
            // Both halves are ONE user action, so they are ONE undo step. The composite borrows
            // SetTransformsCommand's own label, so History reads identically either way.
            auto composite = std::make_unique<core::CompositeCommand>("Transform Layers");
            composite->add(
                std::make_unique<core::SetTransformsCommand>(std::move(entries), /*coalesceId=*/0));
            composite->add(std::make_unique<core::SetMaskPlacementCommand>(std::move(sheets)));
            m_document->commands().push(std::move(composite));
        }
        syncAfterEdit();
    }

    // Edit -> Settings (S51-a): a modeless, instant-apply preferences window. Built lazily the
    // first time and kept around (a re-open just re-seeds + raises it). Each control's host
    // callback does a load-modify-write to settings.json -- so the picker's own surface/recents
    // writes are never clobbered -- and applies the change live.
    void openSettings() {
        if (!m_settingsDialog) {
            SettingsHost host;
            host.setUnits = [this](const std::string& units) {
                persistSetting([&](common::Settings& s) { s.units = units; }, "units");
                m_metric = common::resolveUnits(units) == "metric"; // crop HUD: frame loop re-reads
                if (m_statusBar)
                    m_statusBar->setMetric(m_metric); // status-bar physical size updates live
            };
            host.setCmykProfile = [this](const std::string& cmyk) -> bool {
                // Apply first; persist only what actually took effect, so a bad file never sticks.
                bool ok = true;
                if (m_colorPicker) {
                    if (cmyk.empty()) {
                        m_colorPicker->resetCmykToDefault(); // revert the live engine to built-in
                    } else {
                        ok = m_colorPicker->applyProfileSettings(m_workingProfile, cmyk).cmykOk;
                        if (!ok)
                            uiLog().warn("CMYK ICC profile failed to load: {}", cmyk);
                    }
                    syncColorSpaceName();
                }
                if (ok)
                    persistSetting([&](common::Settings& s) { s.cmykProfile = cmyk; },
                                   "CMYK profile");
                return ok;
            };
            host.setThemeMode = [this](const std::string& mode) {
                setThemeMode(parseThemeMode(mode).value_or(ThemeMode::Dark));
            };
            host.setCropSwitchToolAfterApply = [this](bool on) {
                m_cropSwitchToolAfterApply =
                    on; // applies to the next Apply (no live tool change now)
                persistSetting([&](common::Settings& s) { s.cropSwitchToolAfterApply = on; },
                               "crop post-apply tool switch");
            };
            host.setCropInitialFraming = [this](const std::string& framing) {
                if (m_canvas) // applies the next time the tool stages a rect (re-entry / new doc)
                    m_canvas->setCropFraming(parseCropFraming(framing));
                persistSetting([&](common::Settings& s) { s.cropInitialFraming = framing; },
                               "crop initial framing");
            };
            host.setCropClearSelectionOnLeave = [this](bool on) {
                m_cropClearSelectionOnLeave = on; // takes effect on the next tool switch
                persistSetting([&](common::Settings& s) { s.cropClearSelectionOnLeave = on; },
                               "crop clear-on-leave");
            };
            host.setMultiSelectionEdits = [this](const std::string& mode) {
                if (m_layerPanel) // re-applies live to the blend/opacity strip (S15-e)
                    m_layerPanel->setMultiSelectionMode(parseMultiSelectMode(mode));
                persistSetting([&](common::Settings& s) { s.multiSelectionEdits = mode; },
                               "multi-selection edits");
            };
            host.setLassoSmooth = [this](bool on) {
                if (m_canvas) // takes effect on the next lasso (and live if one is in flight)
                    m_canvas->setLassoSmoothing(on);
                persistSetting([&](common::Settings& s) { s.lassoSmooth = on; }, "lasso smoothing");
            };
            host.setBrushPresetDisplay = [this](const std::string& mode) {
                if (m_dock != nullptr && m_dock->presets() != nullptr)
                    m_dock->presets()->setDisplayMode(ui::presetDisplayModeFromKey(mode));
                persistSetting([&](common::Settings& s) { s.brushPresetDisplay = mode; },
                               "brush preset display");
            };
            host.setEraserPresetFollowsBrush = [this](bool on) {
                // Schema-only in effect (§8.4: the two corpora do not overlap), so there is
                // nothing live to apply -- it is persisted and the eraser reads it when the tie
                // ever comes to mean something.
                persistSetting([&](common::Settings& s) { s.eraserPresetFollowsBrush = on; },
                               "eraser preset tie");
            };
            host.setEraserSizeFollowsBrush = [this](bool on) {
                // Re-tying seeds the eraser from the brush and refreshes the options bar itself
                // (the manager fires notifyOptionsChanged when the mirrored value moves).
                m_tools.setEraserSizeTie(on);
                persistSetting([&](common::Settings& s) { s.eraserSizeFollowsBrush = on; },
                               "eraser size tie");
            };
            host.setOverlayLineStyle = [this](const std::string& style) {
                if (m_canvas) // restyles whatever overlay chrome is on screen right now
                    m_canvas->setOverlayLineStyle(parseOverlayLineStyle(style));
                persistSetting([&](common::Settings& s) { s.overlayLineStyle = style; },
                               "overlay line style");
            };
            host.setFeatherIndicator = [this](const std::string& style) {
                if (m_canvas) // re-indicates whatever selection is on screen right now
                    m_canvas->setFeatherIndicator(parseFeatherIndicator(style));
                persistSetting([&](common::Settings& s) { s.featherIndicator = style; },
                               "feather indicator");
            };
            // Appearance → Icons (S52): the pack browser's data + its apply path.
            for (const IconPackInfo& p : m_iconPacks.packs())
                host.iconPacks.push_back({p.id, p.name, p.artist, p.link, p.description});
            host.renderIcon = [this](const std::string& packId, const std::string& key, int px) {
                return m_iconPacks.renderIcon(packId, key, px);
            };
            host.setIconPack = [this](const std::string& packId) {
                applyIconPack(packId); // re-rasterizes the toolbar live
                persistSetting([&](common::Settings& s) { s.iconPack = packId; }, "icon pack");
            };
            host.setMotivationalLines = [this](bool on) {
                if (m_ticker) // Annoyances: start / stop the menu-bar ticker live
                    m_ticker->setEnabled(on);
                persistSetting([&](common::Settings& s) { s.motivationalLines = on; },
                               "motivational lines");
            };
            host.setShowUnsavedDuration = [this](bool on) { // Annoyances (S18-d): live
                m_showUnsavedDuration = on;
                updateWindowTitle();
                persistSetting([&](common::Settings& s) { s.showUnsavedDuration = on; },
                               "unsaved-title duration");
            };
            host.setUnsavedIncludeSeconds = [this](bool on) {
                m_unsavedIncludeSeconds = on;
                updateWindowTitle();
                persistSetting([&](common::Settings& s) { s.unsavedIncludeSeconds = on; },
                               "unsaved-title seconds");
            };
            // Settings → Rendering (S60-b item 14). Persist only: every GPU lane is built on first
            // use and cached for the process, so flipping this live would leave whichever lanes
            // already exist exactly where they are. main() reads it at start-up and hands it to
            // render::setGpuPolicy; the pane says so in as many words.
            host.setRenderingMode = [this](const std::string& mode) {
                persistSetting([&](common::Settings& s) { s.renderingMode = mode; },
                               "rendering mode");
            };
            // Settings -> General (M5): persist only. The Export modal reads it when it opens.
            host.setShowAllExportFormats = [this](bool on) {
                persistSetting([&](common::Settings& s) { s.showAllExportFormats = on; },
                               "show all export formats");
            };
            // Settings → Text (deferred §2): all live.
            host.setSpellCheck = [this](bool on) {
                m_spellCheckEnabled = on;
                // updateSpellCheck() clears the squiggles when off, and (via forceSpellRescan)
                // scans the current block when re-enabled -- so the change shows on the next frame.
                if (on)
                    forceSpellRescan();
                persistSetting([&](common::Settings& s) { s.spellCheck = on; }, "spell check");
            };
            host.setSpellCheckAllCaps = [this](bool on) {
                m_spellCheckAllCaps = on;
                forceSpellRescan(); // re-scan so ALL-CAPS words get flagged / unflagged now
                persistSetting([&](common::Settings& s) { s.spellCheckAllCaps = on; },
                               "spell check all-caps");
            };
            host.setTextLanguage = [this](const std::string& tag) {
                m_textLanguage = tag;
                m_textShaper.setDefaultLanguage(spellAppLanguage()); // feeds hyphenation too
                forceSpellRescan(); // re-scan the current block in the new default language
                persistSetting([&](common::Settings& s) { s.textLanguage = tag; }, "text language");
            };
            host.emojiFamilies = m_fontDb.emojiFamilies();
            host.setEmojiFont = [this](const std::string& family) {
                m_emojiFont = family;
                m_fontDb.setPreferredEmojiFamily(family);
                // Re-shape every text layer NOW: the shaped-block caches key on contentRevision,
                // which knows nothing of the fallback cascade -- bump them all so emoji already on
                // canvas re-render in the new family instead of only future edits.
                if (m_document != nullptr) {
                    const std::function<void(core::Layer&)> bump = [&](core::Layer& l) {
                        if (auto* tl = l.as<core::TextLayer>())
                            tl->invalidateContentBounds();
                        if (auto* g = l.as<core::GroupLayer>())
                            for (const auto& child : g->children())
                                bump(*child);
                    };
                    bump(m_document->root());
                }
                if (m_layerPanel != nullptr)
                    m_layerPanel->refreshThumbnails();
                requestRecomposite(/*fitView=*/false);
                persistSetting([&](common::Settings& s) { s.emojiFont = family; }, "emoji font");
            };
            // Inpainting (Settings → Inpainting): enumerate the runnable backends with their spec +
            // schema for the dialog, and wire the live setters. The engine is global, so these
            // drive the same instance the brush / Fill use.
            for (const std::string& id : m_inpaintEngine.backendIds()) {
                const auto* b = m_inpaintEngine.backend(id);
                if (b == nullptr || !b->available())
                    continue; // hide e.g. the Script backend until S40 registers a provider
                host.inpaintBackends.push_back({id, b->info(), b->settingsSchema()});
            }
            host.setInpaintBackend = [this](const std::string& id) {
                if (!m_inpaintEngine.setActiveBackend(id))
                    return;
                m_inpaintBackendId = id;
                // A switch resets to the new backend's default preset (the old preset / overrides
                // may not apply to it).
                m_inpaintPresetId.clear();
                if (const auto* b = m_inpaintEngine.backend(id))
                    m_inpaintPresetId = b->settingsSchema().defaultPreset;
                m_inpaintOverrides.clear();
                recomputeInpaintParams();
                persistInpaint();
            };
            host.setInpaintPreset = [this](const std::string& presetId) {
                m_inpaintPresetId = presetId;
                m_inpaintOverrides.clear(); // a named preset replaces any hand-tuned values
                recomputeInpaintParams();
                persistInpaint();
            };
            host.setInpaintParam = [this](const std::string& key, double value) {
                // A hand-tuned knob: the configuration is now "custom" and the override persists.
                m_inpaintPresetId = "custom";
                m_inpaintOverrides[key] = value;
                if (const auto* b = m_inpaintEngine.backend(m_inpaintBackendId))
                    b->applyParam(m_inpaintParams, key, value);
                persistInpaint();
            };
            // Settings → Tablet (docs/tablet.md §8) -------------------------------------------
            if (m_canvas != nullptr) {
                TabletInput& tablet = m_canvas->tabletInput();
                host.tabletBackend = tablet.backendName();
                for (const TabletDeviceInfo& d : tablet.devices())
                    host.tabletDevices.push_back({d.name, d.tool, d.valuators});
                // The live readout the test area polls. POST-policy on purpose: what it shows is
                // what the brush engine is about to get, so a curve that flattens the top of the
                // stroke or a range the nib never reaches is visible right there.
                host.tabletReading = [this]() -> SettingsHost::TabletReading {
                    SettingsHost::TabletReading r;
                    if (m_canvas == nullptr)
                        return r;
                    TabletInput& t = m_canvas->tabletInput();
                    // Pump first. On X11 the canvas is what drains the sample ring, and the canvas
                    // is getting no events at all while this dialog has the focus -- so nothing
                    // would ever advance the readout, and the pen hovering the dialog (the one place
                    // a test area is any use) would read as no tablet at all.
                    t.pumpReadout();
                    const core::brush::StrokeInput s = t.lastSample();
                    r.rateHz = t.sampleRateHz();
                    r.valid = r.rateHz > 0.0; // 0 Hz == nothing has arrived recently == pen away
                    r.pressure = s.pressure;
                    r.xTilt = s.xTilt;
                    r.yTilt = s.yTilt;
                    r.rotation = s.rotation;
                    return r;
                };
                host.tabletWatchWindow = [this](Fl_Window* win) {
                    if (m_canvas != nullptr)
                        m_canvas->tabletInput().watch(win);
                };
                host.tabletUnwatchWindow = [this](Fl_Window* win) {
                    if (m_canvas != nullptr)
                        m_canvas->tabletInput().unwatch(win);
                };
            }
            host.setTabletPressureCurve = [this](const std::string& curve) {
                if (m_canvas)
                    m_canvas->tabletInput().policy().setPressureCurve(
                        core::brush::Curve::fromString(curve));
                persistSetting([&](common::Settings& s) { s.tabletPressureCurve = curve; },
                               "tablet pressure curve");
            };
            host.setTabletPressureRange = [this](double lo, double hi) {
                if (m_canvas) // TabletPolicy clamps + swaps: any pair the UI can produce is defined
                    m_canvas->tabletInput().policy().setPressureRange(lo, hi);
                persistSetting(
                    [&](common::Settings& s) {
                        s.tabletPressureMin = lo;
                        s.tabletPressureMax = hi;
                    },
                    "tablet pressure range");
            };
            host.setTabletTiltOffset = [this](double degrees) {
                if (m_canvas)
                    m_canvas->tabletInput().policy().setTiltOffsetDegrees(degrees);
                persistSetting([&](common::Settings& s) { s.tabletTiltOffsetDegrees = degrees; },
                               "tablet tilt offset");
            };
            host.setTabletSpeed = [this](double maxSpeed, double windowMs) {
                if (m_canvas) // takes effect at the start of the next stroke, never mid-stroke
                    m_canvas->setSpeedParams({maxSpeed, windowMs});
                persistSetting(
                    [&](common::Settings& s) {
                        s.tabletSpeedMax = maxSpeed;
                        s.tabletSpeedWindowMs = windowMs;
                    },
                    "tablet speed smoothing");
            };
            // Settings → Keybindings (S51-b). The dialog RENDERS the live keymap and asks it about
            // conflicts; it never holds a copy, so the list is always what the app is actually
            // dispatching. Each apply below mutates the one keymap, whose onChanged hook re-points
            // the menu items (and re-mirrors them on macOS), then persists the SPARSE override map.
            host.keymap = &m_keymap;
            host.setKeyChord = [this](const std::string& id, const std::string& chordText,
                                      bool steal) {
                const std::optional<KeyChord> chord = chordFromText(chordText);
                if (!chord.has_value() || !m_keymap.rebind(id, *chord, steal))
                    return false;
                persistKeymap();
                return true;
            };
            host.resetKeyChord = [this](const std::string& id) {
                m_keymap.reset(id);
                persistKeymap();
            };
            host.resetAllKeyChords = [this] {
                m_keymap.resetAll();
                persistKeymap();
            };
            host.defaultCmykName = core::defaultCmykProfileName(); // name the built-in in the UI
            host.cmykProfileName = [](const std::string& path) {
                return core::cmykProfileName(path); // a user-picked profile's embedded name
            };
            m_settingsDialog = std::make_unique<SettingsDialog>(std::move(host));
        }
        std::string err;
        const common::Settings current = m_settingsPath.empty()
                                             ? common::Settings{}
                                             : common::loadSettings(m_settingsPath, &err);
        m_settingsDialog->seed(current);
        // Centre the modal dialog over the main window.
        m_settingsDialog->position(x() + (w() - m_settingsDialog->w()) / 2,
                                   y() + (h() - m_settingsDialog->h()) / 2);
        m_settingsDialog->show();
    }

    // Edit→Fill… (S39): a transactional modal that fills the current selection (whole active layer
    // when none) with a solid colour or, for the Inpaint content, the inpainting engine. Cheap
    // solid fills preview live + commit one core::FillCommand; Inpaint hands off to runInpaint().
    void openFillDialog() {
        FillHost host;
        host.foreground = [this] { return m_colors.foreground(); };
        host.background = [this] { return m_colors.background(); };
        // The document-wide AA setting (Move tool's AA combobox), so a pattern fill + its flyout
        // previews read like the canvas -- the same source the compositor uses for pattern edges.
        host.antialias = [this] {
            return currentResampleFilter() != render::ResampleFilter::Nearest;
        };
        host.commitFill = [this](common::Image region, long ox, long oy) {
            core::Layer* l = activeLayerPtr();
            if (l == nullptr || !m_document)
                return;
            pushScopedPixelEdit(
                std::make_unique<core::FillCommand>(l->id(), std::move(region), ox, oy));
        };
        host.compositePreview = [this](const common::Image& region, long ox, long oy, int paneW,
                                       int paneH) {
            return fillCompositePreview(region, ox, oy, paneW, paneH);
        };
        host.runInpaintRegion =
            [this](long ox, long oy, std::uint32_t w, std::uint32_t h,
                   const std::function<bool(float, const std::string&)>& onProgress) {
                return fillInpaintRegion(ox, oy, w, h, onProgress);
            };
        host.runInpaintFill = [this] {
            core::Layer* l = activeLayerPtr();
            if (l == nullptr || !m_document || m_document->selection().isEmpty())
                return;
            runInpaint(l->id(), m_document->selection()); // the proven S39-b async path
        };
        m_fillDialog = std::make_unique<FillDialog>(std::move(host));
        m_fillDialog->seed(buildFillContext());
        m_fillDialog->position(x() + (w() - m_fillDialog->w()) / 2,
                               y() + (h() - m_fillDialog->h()) / 2);
        m_fillDialog->show();
    }

    // Layer ▸ Layer Effects… (LE-b): open the transactional effects modal for the active layer. The
    // host closures capture the target id + its pre-edit effects; every edit sets the effects LIVE
    // (previewing on the canvas behind the movable modal), OK commits one SetLayerEffectsCommand
    // and Cancel reverts to the captured original.
    void openLayerEffects() {
        core::Layer* layer = activeLayerPtr();
        if (layer == nullptr || !m_document)
            return;
        const core::LayerId id = layer->id();
        std::optional<core::LayerEffects> orig =
            layer->hasEffects() ? std::optional<core::LayerEffects>(layer->effects())
                                : std::nullopt;

        LayerEffectsHost host;
        host.foreground = [this] { return m_colors.foreground(); };
        // The document-wide AA setting (Move tool's AA combobox) so the pattern flyout's previews
        // read like the canvas -- the same source the compositor uses for pattern/vector edges.
        host.antialias = [this] {
            return currentResampleFilter() != render::ResampleFilter::Nearest;
        };
        host.applyLive = [this, id](const std::optional<core::LayerEffects>& fx) {
            core::Layer* l = m_document ? m_document->find(id) : nullptr;
            if (l == nullptr)
                return;
            if (fx)
                l->setEffects(*fx);
            else
                l->clearEffects();
            m_dragCache.invalidate();
            requestRecomposite(
                /*fitView=*/false); // shows behind the movable modal (frame-coalesced)
            refreshType3dPanel();   // the 3D viewport renders through the effects (S30-e)
        };
        host.renderPreview = [this, id](int paneW, int paneH) {
            return layerEffectsPreview(id, paneW, paneH);
        };
        host.commit = [this, id, orig](const std::optional<core::LayerEffects>& fx) {
            core::Layer* l = m_document ? m_document->find(id) : nullptr;
            if (l == nullptr)
                return;
            // Restore the pre-edit effects so the pushed command captures THEM as its undo state,
            // then apply the final effects as one undoable step.
            if (orig)
                l->setEffects(*orig);
            else
                l->clearEffects();
            m_document->commands().push(std::make_unique<core::SetLayerEffectsCommand>(id, fx));
            syncAfterEdit();
            refreshType3dPanel();  // the 3D viewport renders through the effects (S30-e)
        };
        m_layerEffectsDialog = std::make_unique<LayerEffectsDialog>(std::move(host));
        m_layerEffectsDialog->seed(layer->name(), orig);
        m_layerEffectsDialog->position(x() + (w() - m_layerEffectsDialog->w()) / 2,
                                       y() + (h() - m_layerEffectsDialog->h()) / 2);
        m_layerEffectsDialog->show();
    }

    // Filter ▸ Adjustments ▸ … (S32): insert a non-destructive adjustment layer ABOVE the active
    // layer -- inside the active layer's group, so it scopes to that group (or globally downward at
    // the root), purely by tree position (docs/adjustment-layers.md §3). One AddLayerCommand; the
    // new layer becomes active; kinds with parameters open the live editor at once.
    //
    // With an active selection the new layer is born MASKED to it (core::maskFromSelection, the
    // same resampling Select ▸ Mask from Selection uses), so "select, then adjust" grades only what
    // the user marked -- the industry-standard reflex. The mask rides INTO the AddLayerCommand
    // rather than following as a second command, so it is one History step: undo removes the layer
    // and its mask together, and redo brings both back. The selection is deliberately LEFT active
    // (the ants then read as "this is what got masked"); nothing here consumes it.
    void insertAdjustmentLayer(core::AdjustmentKind kind) {
        if (!m_document)
            return;
        noteFilterUsed(kind); // Filter ▸ Last Filter now has something to repeat (S53-b)
        std::unique_ptr<core::AdjustmentLayer> layer =
            m_document->makeAdjustment(std::string(core::adjustmentKindName(kind)), kind);
        core::seedAdjustmentDefaults(*layer); // the editor opens on honest, identity defaults
        // Every center-carrying kind (Radial / Depth of Field, and the S35 Wave / Vignette) starts
        // at the document center -- the schema's 0,0 default cannot know the canvas
        // (docs/blur-filters.md §2). Keyed on the KEY, not a kind list, so a new one is seeded the
        // day its schema gains the row; the canvas gizmos read the same pair.
        if (layer->params().contains("center_x")) {
            layer->params()["center_x"] = m_document->width() * 0.5;
            layer->params()["center_y"] = m_document->height() * 0.5;
        }
        // S35: the Vignette's radius is parent-space px like every other geometric filter
        // parameter, and "a bit over half the canvas" is the only honest default for a frame
        // darkening -- the schema's fixed px number cannot know the canvas any more than the
        // centres could (docs/filters-stylize.md §2).
        if (kind == core::AdjustmentKind::Vignette) {
            const double halfDiag = 0.5 * std::hypot(static_cast<double>(m_document->width()),
                                                     static_cast<double>(m_document->height()));
            layer->params()["radius"] = 0.55 * halfDiag;
        }
        // Automask. A brand-new adjustment carries the identity transform, so maskFromSelection
        // takes its 1:1 fast path and the mask IS the selection's coverage -- feather, AA edge and
        // all -- on the document grid the compositor folds an adjustment mask against.
        // anySelected() is the whole gate: it is false both for "no selection" (where a mask would
        // be a reveal-all lie in the dock) and for the degenerate active-selection-of-nothing
        // (where the layer would grade zero pixels and read as "the menu item did nothing").
        const bool masked = m_document->selection().anySelected();
        if (masked)
            layer->setMask(core::maskFromSelection(*layer, m_document->selection(),
                                                   m_document->width(), m_document->height()));
        const core::LayerId id = layer->id();
        core::LayerId parentId = m_document->root().id();
        std::size_t index = m_document->root().childCount();
        if (m_layerPanel != nullptr) {
            if (const std::optional<core::Document::Location> loc =
                    m_document->locate(m_layerPanel->activeLayer())) {
                parentId = loc->parent->id();
                index = loc->index + 1;
            }
        }
        m_document->commands().push(
            std::make_unique<core::AddLayerCommand>(parentId, index, std::move(layer)));
        syncAfterEdit();
        if (m_layerPanel != nullptr)
            m_layerPanel->setActive(id);
        syncCornerPanels(); // the new layer is active: kinds with parameters open the editor
        if (masked)
            transientStatus(_("Masked to the selection"));
    }

    // ---- The corner-panel sync step (S32 round 5): widget reality vs the arbiter's answer ----
    [[nodiscard]] bool cornerPanelShown(int id) {
        switch (id) {
            case kPanelStyle: return m_typePanel != nullptr && m_typePanel->shown();
            case kPanelType3d: return m_type3dPanel != nullptr && m_type3dPanel->shown();
            case kPanelAdjustment:
                return m_adjustmentPanel != nullptr && m_adjustmentPanel->shown();
            case kPanelSelectMorph:
                return m_selectMorphPanel != nullptr && m_selectMorphPanel->shown();
            case kPanelImageOps:
                return m_imageOpsPanel != nullptr && m_imageOpsPanel->shown();
            default: return false;
        }
    }

    void hideCornerPanel(int id) {
        switch (id) {
            case kPanelStyle: closeTypePanel(); break;
            case kPanelType3d: closeType3dPanel(); break;
            case kPanelAdjustment:
                if (m_adjustmentPanel != nullptr && m_adjustmentPanel->shown())
                    m_adjustmentPanel->hide();
                break;
            case kPanelSelectMorph:
                if (m_selectMorphPanel != nullptr && m_selectMorphPanel->shown())
                    m_selectMorphPanel->hide();
                break;
            case kPanelImageOps:
                if (m_imageOpsPanel != nullptr && m_imageOpsPanel->shown())
                    m_imageOpsPanel->hide();
                break;
            default: break;
        }
    }

    // Show `id`, seeding it exactly as its old bespoke opener did. Returns false when its
    // context evaporated between resolve() and here (a null anchor mid-bar-rebuild).
    [[nodiscard]] bool showCornerPanel(int id) {
        switch (id) {
            case kPanelStyle: {
                Fl_Widget* anchor =
                    m_optionsBar != nullptr ? m_optionsBar->typePanelButton() : nullptr;
                if (anchor == nullptr || m_typePanel == nullptr)
                    return false;
                // ENSURE shown (idempotent), never the state-dependent toggle(): showCornerPanel's
                // contract is "make this panel visible" -- the arbiter already owns the open/close
                // decision. toggle() would HIDE a panel that is somehow already up when we get here,
                // and the next frame's reconcile would then read it unshown and permanently clear
                // the explicit request (a conditional panel re-arms from its state; an explicit one
                // does not) -- exactly the "Style/3D won't open while the adjustment editor does"
                // asymmetry. shownFor(anchor) makes the reopen a no-op and any other state re-show.
                if (!m_typePanel->shownFor(anchor))
                    m_typePanel->toggle(anchor); // hidden (or anchored elsewhere) -> opens
                reflectTextOptions(); // seed the panel + the Justify-allowed state
                setTypePanelButtonOpen(true);
                return true;
            }
            case kPanelType3d: {
                Fl_Widget* anchor =
                    m_optionsBar != nullptr ? m_optionsBar->type3dButton() : nullptr;
                if (anchor == nullptr || m_type3dPanel == nullptr)
                    return false;
                if (!m_type3dPanel->shownFor(anchor)) // ensure shown (idempotent), see kPanelStyle
                    m_type3dPanel->toggle(anchor);
                reflectTextOptions(); // seed the controls + render the first viewport frame
                setType3dButtonOpen(true);
                return true;
            }
            case kPanelAdjustment: {
                if (m_adjustmentPanel == nullptr || m_layerPanel == nullptr || !m_document)
                    return false;
                core::Layer* l = m_document->find(m_layerPanel->activeLayer());
                auto* adj = l != nullptr ? l->as<core::AdjustmentLayer>() : nullptr;
                if (adj == nullptr)
                    return false;
                m_lastAdjustFieldId.clear(); // a fresh open never coalesces into an old edit
                m_adjustmentPanel->reflect(*adj);
                m_adjustmentPanel->openFor(m_layerPanel); // focus-preserving corner show
                return true;
            }
            case kPanelSelectMorph: {
                if (m_selectMorphPanel == nullptr || m_canvas == nullptr)
                    return false;
                // Corner-placed against the canvas region (the anchor is only bookkeeping). Ensure
                // shown (idempotent), then seed the marching-ants preview at the current mode/amount.
                if (!m_selectMorphPanel->shownFor(m_canvas))
                    m_selectMorphPanel->openFor(m_canvas);
                previewSelectMorph();
                return true;
            }
            case kPanelImageOps: {
                if (m_imageOpsPanel == nullptr || m_canvas == nullptr || !m_document)
                    return false;
                // Corner-placed against the canvas region (the anchor is only bookkeeping, and is
                // the CANVAS, never a bar button -- those are destroyed on every bar rebuild).
                // openFor rebuilds for the pending mode, re-seeds the size from the document and
                // fires the first preview, so it is the ensure-shown path as well as the open one.
                m_imageOpsPanel->openFor(m_pendingImageOpMode, m_canvas, m_document->width(),
                                         m_document->height(), m_document->dpi());
                return true;
            }
            default: return false;
        }
    }

    void syncCornerPanels() {
        // Reconcile first: the panel we believe visible may have been hidden behind our back
        // (Esc in the popover, a theme close, the bar rebuild dropping its anchor). Reporting
        // that to the arbiter is what makes an Esc-dismissed conditional panel stay away (its
        // token is suppressed) and a closed explicit request evaporate.
        if (m_cornerVisible != 0 && !cornerPanelShown(m_cornerVisible)) {
            m_panelArbiter.hidden(m_cornerVisible);
            m_cornerVisible = 0;
        }
        const int target = m_panelArbiter.resolve().value_or(0);
        if (target == m_cornerVisible)
            return;
        hideCornerPanel(m_cornerVisible);
        m_cornerVisible = target != 0 && showCornerPanel(target) ? target : 0;
    }

    // The panel's edit funnel (the applyTextBlockField twin): read-modify-write the target
    // layer's bag as ONE undoable step, coalescing consecutive edits from the SAME control
    // (fieldId) into a single step -- switching controls starts a new one.
    void applyAdjustmentField(const std::string& fieldId,
                              std::function<void(std::map<std::string, double>&)> mutate) {
        if (m_adjustmentPanel == nullptr)
            return;
        applyAdjustmentFieldOn(m_adjustmentPanel->target(), fieldId, std::move(mutate));
    }

    // The id-targeted body: the panel resolves its target above; the S33 DoF canvas gizmo edits
    // the ACTIVE layer directly (the popover can sit faded/hidden mid-gesture -- the canvas drag
    // must not depend on it). Canvas and panel edits share ONE coalesce stream, so a handle drag
    // and a slider run each stay a single undo step and never merge into each other.
    void applyAdjustmentFieldOn(core::LayerId target, const std::string& fieldId,
                                std::function<void(std::map<std::string, double>&)> mutate) {
        if (!m_document)
            return;
        core::Layer* l = m_document->find(target);
        auto* adj = l != nullptr ? l->as<core::AdjustmentLayer>() : nullptr;
        if (adj == nullptr)
            return;
        std::map<std::string, double> next = adj->params();
        mutate(next);
        if (fieldId != m_lastAdjustFieldId) {
            ++m_adjustCoalesceSeq; // a fresh coalesce id: this edit starts its own undo step
            m_lastAdjustFieldId = fieldId;
        }
        m_document->commands().push(std::make_unique<core::SetAdjustmentParamsCommand>(
            adj->id(), std::move(next),
            std::string("Edit ") + std::string(core::adjustmentKindName(adj->adjustmentKind())),
            m_adjustCoalesceSeq));
        // A param edit rebuilds no dock rows (name/badges hold), so skip syncAfterEdit's full
        // refresh -- at slider-drag rate it is real churn. Recomposite (frame-coalesced) +
        // drag-cache invalidation are what the edit needs NOW; the adjustment row's preview
        // thumbnail follows on the settle timer below (the text-thumbnail pattern), so a drag
        // re-renders it once at rest instead of per tick.
        m_dragCache.invalidate();
        requestRecomposite(/*fitView=*/false);
        m_adjustThumbDirty = true;
        m_lastAdjustEditTime = nowSeconds();
    }

    // A blur-parameter scrub in flight? While the pointer is pushed inside the adjustment panel
    // and the active layer is a spatial (S33 blur) adjustment -- or a DoF focus-band handle drag
    // is live on the canvas -- full recomposites run in DRAFT mode: the heavy gathers subsample
    // taps (docs/blur-filters.md §3), and the release settles at full quality (the transition
    // watcher in updateAdjustmentPanel).
    [[nodiscard]] bool blurScrubInFlight() {
        if (m_canvas != nullptr && m_canvas->dofHandleGestureActive())
            return true;
        if (m_adjustmentPanel == nullptr || !m_adjustmentPanel->shown() || !m_document)
            return false;
        bool inPanel = false;
        for (Fl_Widget* p = Fl::pushed(); p != nullptr; p = p->parent()) {
            if (p == m_adjustmentPanel) {
                inPanel = true;
                break;
            }
        }
        if (!inPanel)
            return false;
        core::Layer* l = m_document->find(m_layerPanel->activeLayer());
        auto* adj = l != nullptr ? l->as<core::AdjustmentLayer>() : nullptr;
        return adj != nullptr && core::adjustmentIsSpatial(adj->adjustmentKind());
    }

    // The corner panels' per-frame drive: ONE arbiter decision (which panel is visible), then
    // content upkeep for the shown adjustment editor -- re-sync from the live bag so undo/redo
    // moves its controls (retargeting when the selection moved to a DIFFERENT adjustment layer,
    // which changes the arbiter token but not the visible panel), and the occlusion fade.
    void updateAdjustmentPanel() {
        // The draft-mode settle: the frame a blur scrub ends, recomposite once at full quality
        // (the scrub's own edits composited in draft; nothing else would clean them up).
        const bool scrub = blurScrubInFlight();
        if (m_blurScrubWasActive && !scrub)
            requestRecomposite(/*fitView=*/false);
        m_blurScrubWasActive = scrub;
        syncCornerPanels();
        if (m_adjustmentPanel == nullptr || !m_adjustmentPanel->shown() || !m_document)
            return;
        core::Layer* l = m_document->find(m_layerPanel->activeLayer());
        auto* adj = l != nullptr ? l->as<core::AdjustmentLayer>() : nullptr;
        if (adj != nullptr)
            m_adjustmentPanel->reflect(*adj);
        updateAdjustmentPanelFade();
    }

    // While the panel overlaps the VISIBLE document image and the pointer is elsewhere, it goes
    // translucent so the graded pixels beneath stay visible (user 2026-07-17); pointing at it --
    // or having a drag in flight on one of its controls -- restores full opacity, and a panel
    // sitting on the canvas apron never fades (it occludes nothing).
    void updateAdjustmentPanelFade() {
        const auto within = [this](Fl_Widget* leaf) {
            for (Fl_Widget* p = leaf; p != nullptr; p = p->parent())
                if (p == m_adjustmentPanel)
                    return true;
            return false;
        };
        bool occludes = false;
        if (m_document && m_canvas != nullptr) {
            int ox = 0, oy = 0;
            m_canvas->top_window_offset(ox, oy);
            const double W = m_document->width();
            const double H = m_document->height();
            const CanvasView& view = m_canvas->view();
            double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
            for (const common::Vec2 corner :
                 {common::Vec2{0, 0}, {W, 0}, {0, H}, {W, H}}) {
                const common::Vec2 s = view.toScreen(corner);
                minX = std::min(minX, s.x);
                minY = std::min(minY, s.y);
                maxX = std::max(maxX, s.x);
                maxY = std::max(maxY, s.y);
            }
            const common::Rect docOnScreen{minX + ox, minY + oy, maxX - minX, maxY - minY};
            const common::Rect panelRect{static_cast<double>(m_adjustmentPanel->x()),
                                         static_cast<double>(m_adjustmentPanel->y()),
                                         static_cast<double>(m_adjustmentPanel->w()),
                                         static_cast<double>(m_adjustmentPanel->h())};
            occludes = !docOnScreen.intersected(panelRect).empty();
        }
        const bool engaged = within(Fl::belowmouse()) || within(Fl::pushed());
        m_adjustmentPanel->setFade(occludes && !engaged ? 0.45 : 1.0);
        // Rebuild the cached blend when anything under it changed: a recomposite, the view
        // moving (pan / zoom / rotate never recomposite -- the transform is GPU-side, so the
        // composite revision alone missed them: the round-3 "ignores panning" bug), the panel
        // re-anchoring, or the panel's own content syncing. Built HERE (never in draw()).
        if (m_adjustmentPanel->fade() < 1.0 && m_canvas != nullptr) {
            std::uint64_t fp = 1469598103934665603ull;
            const auto fold = [&fp](std::uint64_t v) { fp = (fp ^ v) * 1099511628211ull; };
            fold(m_compositeRevision);
            const CanvasView& view = m_canvas->view();
            fold(std::bit_cast<std::uint64_t>(view.zoom()));
            fold(std::bit_cast<std::uint64_t>(view.rotation()));
            fold(std::bit_cast<std::uint64_t>(view.pan().x));
            fold(std::bit_cast<std::uint64_t>(view.pan().y));
            fold(static_cast<std::uint64_t>(m_adjustmentPanel->x()));
            fold(static_cast<std::uint64_t>(m_adjustmentPanel->y()));
            fold(static_cast<std::uint64_t>(m_adjustmentPanel->w()));
            fold(static_cast<std::uint64_t>(m_adjustmentPanel->h()));
            if (fp != m_adjPanelFadeFp || m_adjustmentPanel->blendDirty()) {
                m_adjPanelFadeFp = fp;
                m_adjustmentPanel->refreshFadeBlend();
            }
        }
    }

#ifdef MOSAIC_DEBUG
    // Help -> Show Canvas FPS (debug only): flip the corner FPS readout. `on` is the menu checkbox's
    // new value.
    void toggleCanvasFps(bool on) noexcept { m_showCanvasFps = on; }
#endif // MOSAIC_DEBUG

    // Help -> Timing Profiler...: open (or re-reveal) the ONE non-modal profiler window. It reads
    // the process-wide common::Profiler directly; onFrame redraws it while shown. In release this
    // is reachable only while profiling is on (S60-alpha) -- see timing_graph_window.hpp.
    void openTimingGraph() {
        if (m_timingGraph == nullptr)
            m_timingGraph = std::make_unique<TimingGraphWindow>();
        m_timingGraph->show();
    }

    // Layer ▸ Texture Generator… (S55-f): the transactional generator modal. Create mode inserts a
    // fresh TextureLayer above the active layer (the paste rule); with a TextureLayer already
    // selected the dialog EDITS it (§3.3 select-to-edit) and commits one SetTextureCommand. Either
    // way the dialog hands over the FULL-RES bake it rendered behind its progress bar, installed
    // via applyBakedTextureCache so the pre-composite refresh finds a CURRENT cache -- Create
    // never re-renders synchronously on the UI thread.
    void openTextureGenerator() {
        if (!m_document)
            return;
        std::optional<std::pair<std::string, core::texture::TextureParams>> editing;
        core::LayerId editId{};
        if (core::Layer* l = activeLayerPtr(); l != nullptr) {
            if (auto* t = l->as<core::TextureLayer>()) {
                editing = std::make_pair(l->name(), t->params());
                editId = l->id();
            }
        }
        const bool isEdit = editing.has_value();
        // The estimate's source + conform target: the layer active at open time. The dialog is
        // modal, so it cannot change underneath (activeLayerPtr stays this layer throughout).
        core::LayerId photoId{};
        if (core::Layer* l = activeLayerPtr(); l != nullptr)
            photoId = l->id();

        TextureGenHost host;
        host.foreground = [this] { return m_colors.foreground(); };
        // The active layer's doc-space pixels + name + EXIF for "Estimate from layer" -- the
        // same activeLayerDocImage machinery the magic wand samples through, so the two
        // features' idea of "the layer's pixels" can never drift. nullopt disables the button.
        host.sourceLayer = [this]() -> std::optional<TextureGenHost::SourceLayer> {
            if (!m_document)
                return std::nullopt;
            common::Image scratch;
            const common::Image* src = activeLayerDocImage(scratch);
            if (src == nullptr || src->empty())
                return std::nullopt;
            TextureGenHost::SourceLayer out;
            if (core::Layer* l = activeLayerPtr(); l != nullptr) {
                out.name = l->name();
                out.exif = l->exif();
            }
            out.docImage = src == &scratch ? std::move(scratch) : *src;
            return out;
        };
        host.commit = [this, isEdit, editId](core::texture::TextureParams params,
                                             core::texture::TextureRenderResult baked) {
            if (!m_document)
                return;
            // The cache bounds describe the PIXELS being installed, so measure the bake itself
            // rather than trusting the document is still the size the dialog was seeded with;
            // if it somehow changed, the next refresh sees the size mismatch and re-renders.
            std::uint32_t dw = 0, dh = 0;
            if (baked.image8) {
                dw = baked.image8->width;
                dh = baked.image8->height;
            } else if (baked.imageF) {
                dw = baked.imageF->width;
                dh = baked.imageF->height;
            }
            if (isEdit) {
                core::Layer* l = m_document->find(editId);
                auto* t = l != nullptr ? l->as<core::TextureLayer>() : nullptr;
                if (t == nullptr)
                    return; // the layer vanished while the dialog was open
                // Push FIRST (setParams bumps the revision), then stamp the baked cache current.
                m_document->commands().push(
                    std::make_unique<core::SetTextureCommand>(editId, std::move(params)));
                core::texture::applyBakedTextureCache(*t, std::move(baked), dw, dh);
                m_dragCache.invalidate();
                syncAfterEdit();
                return;
            }
            auto layer = m_document->makeTexture(
                core::texture::generatorName(params.generator), std::move(params));
            core::texture::applyBakedTextureCache(*layer, std::move(baked), dw, dh);
            const core::LayerId newId = layer->id();
            // Above the active layer (the paste rule), or on top of the root stack.
            core::LayerId parentId = m_document->root().id();
            std::size_t index = m_document->root().childCount();
            if (m_layerPanel != nullptr) {
                if (const std::optional<core::Document::Location> loc =
                        m_document->locate(m_layerPanel->activeLayer())) {
                    parentId = loc->parent->id();
                    index = loc->index + 1;
                }
            }
            m_document->commands().push(
                std::make_unique<core::AddLayerCommand>(parentId, index, std::move(layer)));
            syncAfterEdit();
            if (m_layerPanel)
                m_layerPanel->setActive(newId);
        };
        // The estimate's "mask & harmonize" ACCEPT (S55 phase 2; design §6.3): ONE undo step --
        // sky below the photo, the photo masked to its foreground, the PhotometricMatch grade
        // clipped above it. Reached only through the user's explicit toggle + Create (the ip
        // no auto-chained replace command exists). Falls back to the plain commit
        // when the shape cannot be built (the sky layer still lands; nothing half-commits).
        host.commitConform = [this, isEdit, photoId, plain = host.commit](
                                 core::texture::TextureParams params,
                                 core::texture::TextureRenderResult baked,
                                 TextureGenHost::ConformPayload conform) {
            if (!m_document)
                return;
            const core::Layer* photo = m_document->find(photoId);
            if (!isEdit && photo != nullptr && conform.skySelection.anySelected() &&
                std::holds_alternative<core::texture::SkyParams>(params.spec)) {
                core::texture::SkyConformPlan plan;
                plan.skyParams = params;
                plan.baked = std::move(baked);
                plan.skySelection = std::move(conform.skySelection);
                plan.matchParams = std::move(conform.matchParams);
                char label[160];
                std::snprintf(label, sizeof(label), _("Sky from \"%s\""), photo->name().c_str());
                plan.label = label;
                if (auto cmd = core::texture::buildSkyConformCommand(*m_document, photoId,
                                                                     std::move(plan))) {
                    m_document->commands().push(std::move(cmd));
                    m_dragCache.invalidate(); // the stack gained two layers + a mask
                    syncAfterEdit();
                    if (m_layerPanel)
                        m_layerPanel->setActive(photoId);
                    return;
                }
                // The plan consumed `baked`; the degraded plain layer re-renders its cache on
                // the next composite (correct, just not pre-warmed). Never expected in practice
                // -- every precondition was checked above.
                baked = {};
            }
            plain(std::move(params), std::move(baked));
        };
        m_textureGenDialog = std::make_unique<TextureGeneratorDialog>(std::move(host));
        m_textureGenDialog->seed(m_document->width(), m_document->height(), std::move(editing));
        m_textureGenDialog->position(x() + (w() - m_textureGenDialog->w()) / 2,
                                     y() + (h() - m_textureGenDialog->h()) / 2);
        m_textureGenDialog->show();
    }

    // Capture the current fill target for the dialog: the layer-local region a solid fill will
    // touch (the bbox of selection coverage > 0; the whole layer when there is no selection) + the
    // per-pixel coverage, plus a padded copy for the preview pane. Coverage maps each layer pixel's
    // centre through the layer's world transform into document space and samples the selection, the
    // same way the clipboard's selection-aware ops do.
    FillContext buildFillContext() {
        FillContext ctx;
        core::Layer* layer = activeLayerPtr();
        auto* raster = layer != nullptr ? layer->as<core::RasterLayer>() : nullptr;
        if (raster == nullptr || raster->image().empty() || !m_document)
            return ctx; // hasRasterTarget stays false
        ctx.hasRasterTarget = true;
        ctx.inpaintAvailable = m_inpaintEngine.backend(m_inpaintBackendId) != nullptr;
        const common::Image& src = raster->image();
        const core::Selection& sel = m_document->selection();
        ctx.selectionActive = !sel.isEmpty();
        const common::Affine2D t = core::worldTransform(*layer);
        const bool identity = t == common::Affine2D::identity();

        std::vector<std::uint8_t> cov(src.pixelCount(), 0);
        long minx = src.width, miny = src.height, maxx = -1, maxy = -1;
        for (std::uint32_t y = 0; y < src.height; ++y) {
            for (std::uint32_t x = 0; x < src.width; ++x) {
                int c = 255; // no selection: the whole layer is editable
                if (!sel.isEmpty()) {
                    common::Vec2 d{x + 0.5, y + 0.5};
                    if (!identity)
                        d = t.apply(d);
                    const long dx = static_cast<long>(std::floor(d.x));
                    const long dy = static_cast<long>(std::floor(d.y));
                    c = (dx < 0 || dy < 0) ? 0
                                           : sel.at(static_cast<std::uint32_t>(dx),
                                                    static_cast<std::uint32_t>(dy));
                }
                cov[static_cast<std::size_t>(y) * src.width + x] = static_cast<std::uint8_t>(c);
                if (c > 0) {
                    minx = std::min(minx, static_cast<long>(x));
                    miny = std::min(miny, static_cast<long>(y));
                    maxx = std::max(maxx, static_cast<long>(x));
                    maxy = std::max(maxy, static_cast<long>(y));
                }
            }
        }
        if (maxx < 0)
            return ctx; // selection misses the layer: region stays empty (Fill no-ops)

        auto cropCoverage = [&](long rx, long ry, std::uint32_t rw, std::uint32_t rh) {
            std::vector<std::uint8_t> out(static_cast<std::size_t>(rw) * rh, 0);
            for (std::uint32_t yy = 0; yy < rh; ++yy)
                for (std::uint32_t xx = 0; xx < rw; ++xx) {
                    const long sxp = rx + xx, syp = ry + yy;
                    if (sxp >= 0 && syp >= 0 && sxp < static_cast<long>(src.width) &&
                        syp < static_cast<long>(src.height))
                        out[static_cast<std::size_t>(yy) * rw + xx] =
                            cov[static_cast<std::size_t>(syp) * src.width + sxp];
                }
            return out;
        };

        const auto rw = static_cast<std::uint32_t>(maxx - minx + 1);
        const auto rh = static_cast<std::uint32_t>(maxy - miny + 1);
        ctx.region = common::copyRegion(src, minx, miny, rw, rh);
        ctx.coverage = cropCoverage(minx, miny, rw, rh);
        ctx.originX = minx;
        ctx.originY = miny;
        return ctx;
    }

    // Composite the document for the Fill dialog's preview pane: temporarily apply `regionPixels`
    // at (ox, oy) to the active raster layer, composite a padded document region around it (so the
    // pane reflects ALL layers + this layer's blend mode/opacity), restore the pixels, and return
    // it.
    PreviewContent fillCompositePreview(const common::Image& regionPixels, long ox, long oy,
                                        int paneW, int paneH) {
        core::Layer* layer = activeLayerPtr();
        auto* raster = layer != nullptr ? layer->as<core::RasterLayer>() : nullptr;
        if (raster == nullptr || !m_document || regionPixels.empty() || paneW <= 0 || paneH <= 0)
            return {};
        const auto rw = static_cast<long>(regionPixels.width);
        const auto rh = static_cast<long>(regionPixels.height);
        // Affected rect in DOCUMENT space: map the four corners through the layer transform.
        const common::Affine2D t = core::worldTransform(*layer);
        const common::Vec2 c[4] = {
            t.apply({double(ox), double(oy)}), t.apply({double(ox + rw), double(oy)}),
            t.apply({double(ox + rw), double(oy + rh)}), t.apply({double(ox), double(oy + rh)})};
        double dx0 = c[0].x, dy0 = c[0].y, dx1 = c[0].x, dy1 = c[0].y;
        for (const common::Vec2& v : c) {
            dx0 = std::min(dx0, v.x);
            dy0 = std::min(dy0, v.y);
            dx1 = std::max(dx1, v.x);
            dy1 = std::max(dy1, v.y);
        }
        // Frame the fill area to the pane aspect (+ margin), rendered UNCLAMPED so a fill near the
        // canvas edge shows the off-canvas backdrop, not a hard panel margin.
        const common::Rect roi =
            matchRegionToPaneAspect({dx0, dy0, dx1 - dx0, dy1 - dy0}, paneW, paneH);
        if (roi.w < 1.0 || roi.h < 1.0)
            return {};

        // Apply → composite the ROI → restore (the document is unchanged when we return).
        common::Image saved =
            common::copyRegion(raster->image(), ox, oy, regionPixels.width, regionPixels.height);
        common::blitRegion(raster->image(), regionPixels, ox, oy);
        raster->invalidateContentBounds();
        render::CompositeOptions opts;
        opts.checkerboard = false;
        common::Image out = render::compositeRegion(*m_document, roi, opts, render::Backend::Cpu,
                                                    /*clampToCanvas=*/false)
                                .image;
        common::blitRegion(raster->image(), saved, ox, oy);
        raster->invalidateContentBounds();
        return {std::move(out), canvasRectInImage(roi)};
    }

    // The Layer Effects dialog's in-modal preview pane: composite the target layer's effect
    // neighbourhood (its effectsBounds, mapped to document space) with the layer's CURRENT effects
    // already applied (the dialog's applyLive set them), over transparency (the pane draws the
    // checker). Reflects all layers in that region, so pane + canvas agree. `maxPx` is advisory --
    // the pane aspect-fits, and a layer-effect target's content box is normally small.
    PreviewContent layerEffectsPreview(core::LayerId id, int paneW, int paneH) {
        if (!m_document || paneW <= 0 || paneH <= 0)
            return {};
        core::Layer* layer = m_document->find(id);
        if (layer == nullptr)
            return {};
        // A 3D text layer BAKES the overlay effects into its pixel cache (S30-e §12), so a live
        // overlay tweak in the modal stales the cache -- refresh before compositing or the pane
        // lags the canvas by a frame.
        settleTextCaches();
        const std::optional<common::Rect> eb = layer->effectsBounds(); // layer-local
        if (!eb || eb->empty())
            return {};
        // Frame the effect neighbourhood: centre the region on the layer's effectsBounds (in
        // document space), add a margin so the content doesn't touch the pane edge, then expand to
        // the PANE's aspect so the pane fills edge to edge (matchRegionToPaneAspect). Rendered
        // UNCLAMPED so an effect spilling past the canvas edge stays visible; the pane paints the
        // off-canvas surround.
        const common::Rect docBox = core::worldTransform(*layer).mapBounds(*eb);
        const common::Rect roi = matchRegionToPaneAspect(docBox, paneW, paneH);
        if (roi.w < 1.0 || roi.h < 1.0)
            return {};
        render::CompositeOptions opts;
        opts.checkerboard = false; // the pane composites over its own checker + backdrop
        common::Image img = render::compositeRegion(*m_document, roi, opts, render::Backend::Cpu,
                                                    /*clampToCanvas=*/false)
                                .image;
        return {std::move(img), canvasRectInImage(roi)};
    }

    // The document canvas rectangle expressed in the coordinates of a region image produced by an
    // UNCLAMPED compositeRegion(roi): the image origin is floor(roi.x/y) (compositeRegion's
    // flooring), so document pixel 0 lands at image pixel -floor(roi.x). The pane uses this to
    // paint the off-canvas surround (outside this rect) distinctly from in-canvas transparency
    // (checker).
    common::Rect canvasRectInImage(const common::Rect& roi) const {
        const double x0 = std::floor(roi.x);
        const double y0 = std::floor(roi.y);
        return {-x0, -y0, static_cast<double>(m_document->width()),
                static_cast<double>(m_document->height())};
    }

    // Grow `content` (a document-space rect) into the region a live-preview pane should composite:
    // a margin around the content (so it doesn't touch the edge), then an expansion of the
    // deficient axis so the region matches the pane's aspect ratio -- the pane draws the result
    // aspect-fit, so a matched aspect fills it with no letterbox. Both dimensions are capped so a
    // pathological aspect can't demand a huge composite every frame (the tiny residual aspect error
    // just letterboxes as checker). The rect is NOT clamped to the canvas: the caller renders it
    // unclamped so the off-canvas surround reads as checker.
    static common::Rect matchRegionToPaneAspect(const common::Rect& content, int paneW, int paneH) {
        const double cx = content.x + content.w / 2.0;
        const double cy = content.y + content.h / 2.0;
        const double margin = 0.08 * std::max(content.w, content.h) + 3.0;
        double rw = content.w + 2.0 * margin;
        double rh = content.h + 2.0 * margin;
        const double aspect = static_cast<double>(paneW) / static_cast<double>(paneH);
        if (rw / rh < aspect)
            rw = rh * aspect; // too tall for the pane -> widen
        else
            rh = rw / aspect;               // too wide -> heighten
        constexpr double kMaxSide = 4096.0; // bound the per-frame composite (canvas cap is 16384)
        rw = std::min(rw, kMaxSide);
        rh = std::min(rh, kMaxSide);
        return {cx - rw / 2.0, cy - rh / 2.0, rw, rh};
    }

    // Synchronously inpaint the current selection and return the filled layer region
    // [(ox,oy),(w,h)] — the Fill dialog's Preview button caches this, and Fill commits the cache
    // (no second run). Drives the status-bar progress bar + the dialog's pane note via
    // `onProgress`, pumping the event loop each tick so both repaint (and the status-bar cancel X
    // can fire if it is reachable; note the modal dialog grabs events, so the in-dialog Esc — wired
    // to onProgress's return — is the dependable cancel). Re-entrancy from the pump is fenced off
    // by the dialog (it guards its buttons + recompute while a Preview runs).
    std::optional<common::Image>
    fillInpaintRegion(long ox, long oy, std::uint32_t w, std::uint32_t h,
                      const std::function<bool(float, const std::string&)>& onProgress) {
        core::Layer* layer = activeLayerPtr();
        auto* raster = layer != nullptr ? layer->as<core::RasterLayer>() : nullptr;
        if (raster == nullptr || !m_document || m_document->selection().isEmpty())
            return std::nullopt;
        const common::ImageF input = common::toFloat(raster->image());
        std::atomic<bool> cancel{false};
        if (m_statusBar != nullptr) {
            m_statusBar->onProgressCancel([&cancel] { cancel.store(true); });
            m_statusBar->setProgress(0.0f, _("Analyzing"));
        }
        const core::inpaint::InpaintRequest req{input, m_document->selection(), m_inpaintParams,
                                                &cancel};
        const core::inpaint::ProgressFn fn = [&](const core::inpaint::InpaintProgress& p) -> bool {
            const std::string stage = localizedInpaintStage(std::string(p.stage));
            if (m_statusBar != nullptr)
                m_statusBar->setProgress(p.fraction, stage);
            bool keep = true;
            if (onProgress)
                keep = onProgress(p.fraction,
                                  stage); // updates the dialog pane + returns its Esc state
            Fl::check(); // repaint both indicators (and process the cancel X if events reach it)
            return keep && !cancel.load();
        };
        const core::inpaint::InpaintResult res = m_inpaintEngine.run(req, fn);
        if (m_statusBar != nullptr) {
            m_statusBar->hideProgress();
            m_statusBar->onProgressCancel(nullptr); // drop the &cancel reference before it dies
        }
        if (!res.ok || cancel.load())
            return std::nullopt;
        const common::Image full = common::toImage8(res.image);
        return common::copyRegion(full, ox, oy, w, h);
    }

    // Switch the theme mode at runtime (Settings -> Appearance). Persist it, then re-resolve +
    // apply the palette: applyTheme() updates FLTK's colour map + boxtypes, fires the theme
    // observers (our reapplyTheme below re-applies the chrome's cached colours), and requests a
    // global redraw. The Vulkan canvas is repainted separately (it presents outside FLTK's draw).
    void setThemeMode(ThemeMode mode) {
        m_themeMode = mode;
        persistSetting([&](common::Settings& s) { s.theme = std::string(themeModeKey(mode)); },
                       "theme");
        applyTheme(resolvePalette(mode));
    }

    // Follow the OS light/dark + accent preference live while in System mode (the OS changed, not
    // our Settings). Called once after the window + chrome exist; the subscription lasts the whole
    // process. Explicit Light/Dark modes ignore the OS. The OS-change callback lands on the FLTK
    // thread, so applyTheme (+ its reapplyTheme observers) is safe.
    void watchSystemTheme(ThemeMode initialMode) {
        m_themeMode = initialMode;
        platform::watchSystemAppearance([this] {
            if (m_themeMode == ThemeMode::System)
                applyTheme(resolvePalette(ThemeMode::System));
        });
    }

    // Fired by applyTheme() on a re-theme (registered via m_themeSub). Re-applies the colours the
    // chrome cached at construction; widgets that read activePalette() live re-theme on the redraw.
    void reapplyTheme() {
        const Palette& pal = activePalette();
        m_fontPreviewCache.clear(); // previews bake the text colour -> re-render in the new palette
        color(toFl(pal.windowBg));  // the window ground showing in the inter-widget gaps
        if (m_canvas != nullptr) {
            m_canvas->setClearColor(pal.canvasBg);
            requestFrame(); // re-present the Vulkan backdrop in the new colour
        }
        if (m_toolbar != nullptr)
            m_toolbar->reapplyTheme();
        closeTypePanel(); // the bar reapplyTheme rebuilds the "Style…" button (the panel's anchor)
        closeType3dPanel(); // ... and the "3D…" one (same rebuild)
        if (m_optionsBar != nullptr)
            m_optionsBar->reapplyTheme();
        if (m_typePanel != nullptr)
            m_typePanel->reapplyTheme(); // rebuild its controls in the new palette (hidden now)
        // The Image-ops panel generates its controls too, so it needs the same rebuild. Close it
        // first: the rebuild drops every widget, and a half-rebuilt panel over a live preview is
        // not a state worth supporting -- closeImageOps also drops the staged canvas overlay.
        if (m_imageOpsPanel != nullptr) {
            if (m_imageOpsPanel->shown())
                closeImageOps();
            m_imageOpsPanel->reapplyTheme();
        }
        if (m_dock != nullptr)
            m_dock->reapplyTheme(); // both regions: the layer panel and the preset grid
        if (m_statusBar != nullptr)
            m_statusBar->reapplyTheme();
        if (m_settingsDialog) // open during the switch (the picker lives in it)
            m_settingsDialog->reapplyTheme();
        if (m_layerEffectsDialog && m_layerEffectsDialog->shown())
            m_layerEffectsDialog->reapplyTheme();
        if (m_textureGenDialog && m_textureGenDialog->shown())
            m_textureGenDialog->reapplyTheme();
        // The menu-bar ticker reads activePalette() live in MenuBar::draw(), so it re-themes on the
        // redraw() below (no cached colours to refresh).
        redraw();
    }

    // Settings → Tablet (docs/tablet.md §7). The policy lives on the canvas's TabletInput, which
    // applies it at ingest -- so every one of these takes effect on the NEXT SAMPLE, mid-stroke
    // included, with nothing to reload. The speed calibration is the engine's and lands at the start
    // of the next stroke (re-scaling a stroke already in flight is not a thing anyone wants).
    void applyTabletPolicy(const RunOptions& opts) {
        if (m_canvas == nullptr)
            return;
        core::brush::TabletPolicy& policy = m_canvas->tabletInput().policy();
        policy.setPressureCurve(core::brush::Curve::fromString(opts.tabletPressureCurve));
        policy.setPressureRange(opts.tabletPressureMin, opts.tabletPressureMax);
        policy.setTiltOffsetDegrees(opts.tabletTiltOffsetDegrees);
        m_canvas->setSpeedParams({opts.tabletSpeedMax, opts.tabletSpeedWindowMs});
    }

    // Load-modify-write one setting to disk; `mutate` edits the freshly-loaded struct. Keeps each
    // write honest against fields other code (picker surface, recents) may persist concurrently.
    // The debounced brush-smoothing write. A static thunk because Fl::add_timeout takes one.
    static void persistBrushSmoothingCb(void* v) {
        auto* self = static_cast<MainWindow*>(v);
        const bool on = self->m_pendingBrushSmoothing;
        self->persistSetting([&](common::Settings& s) { s.brushSmoothing = on; },
                             "brush smoothing");
    }
    bool m_pendingBrushSmoothing = true;

    void persistSetting(const std::function<void(common::Settings&)>& mutate, const char* what) {
        if (m_settingsPath.empty())
            return;
        std::string err;
        common::Settings cfg = common::loadSettings(m_settingsPath, &err);
        mutate(cfg);
        if (!common::saveSettings(cfg, m_settingsPath, &err))
            uiLog().warn("could not persist {}: {}", what, err);
    }

    // Closing the main window must also dismiss the modeless Settings dialog -- otherwise it stays
    // shown, keeping Fl::run() alive and orphaning the app (user-reported, 2026-06-14).
    void hide() override {
        if (m_settingsDialog)
            m_settingsDialog->hide();
        // ... and the Timing Profiler window, for the same reason: a shown top-level window keeps
        // Fl::run() alive, so closing the main window would otherwise never quit the app.
        if (m_timingGraph)
            m_timingGraph->hide();
        // A debounced smoothing write may still be in flight. Flush it -- quitting half a second
        // after moving the slider must not silently discard the value the user just chose -- and then
        // cancel the timeout, so it can never fire into a window that is going away.
        if (Fl::has_timeout(persistBrushSmoothingCb, this)) {
            Fl::remove_timeout(persistBrushSmoothingCb, this);
            persistBrushSmoothingCb(this);
        }
        Fl_Double_Window::hide();
    }

    // Edit menu (S10): undo/redo the document, then re-composite (scoped to the edit's region when
    // the command reports one -- S60-a) + rebuild the layer list.
    void undo() {
        if (m_document && m_document->commands().canUndo()) {
            m_document->commands().undo();
            syncAfterUndoRedo();
        }
    }
    void redo() {
        if (m_document && m_document->commands().canRedo()) {
            m_document->commands().redo();
            syncAfterUndoRedo();
        }
    }
    // Layer menu (S10): forward to the Layers panel, which edits via the command stack and
    // re-composites itself through the onChange callback.
    void newLayer() {
        if (m_layerPanel)
            m_layerPanel->addRasterLayer();
    }
    void duplicateLayer() {
        if (m_layerPanel)
            m_layerPanel->duplicateActive();
    }
    // Copy the properties that style how a layer composites INTO its parent, not how its own pixels
    // are made. rasterizeLayer() bakes the rest (transform, mask, effects), so carrying exactly
    // these four across leaves the document compositing to identical pixels. (Merge Down's baked
    // route deliberately does NOT use this -- see mergeDownLayer: mergeDownBaked has already
    // consumed all four, so carrying them would apply them twice.)
    static void carryCompositingProps(const core::Layer& from, core::Layer& to) {
        to.setVisible(from.visible());
        to.setOpacity(from.opacity());
        to.setBlendMode(from.blendMode());
        to.setClipToBelow(from.clipToBelow());
    }

    // Layer -> Rasterize: bake a Text / Vector / Magic layer (or a whole group) into a RasterLayer
    // of its pixels, at the same place in the tree, as ONE undo step. Unblocks the brush, which
    // refuses every non-raster target.
    void rasterizeLayerCommand(core::LayerId id) {
        if (!m_document)
            return;
        core::Layer* layer = m_document->find(id);
        if (layer == nullptr || layer->locked())
            return;
        settleTextCaches(); // a TextLayer's pixels live in a renderer-filled cache; fill it first
        // The SAME filter the canvas is composited with: rasterizing must not change the picture.
        common::Image baked = render::rasterizeLayer(*layer, m_document->width(),
                                                     m_document->height(), currentResampleFilter());
        if (baked.empty()) {
            transientStatus(_("Rasterize: this layer has no pixels to bake"));
            return;
        }
        std::unique_ptr<core::RasterLayer> raster =
            m_document->makeRaster(layer->name(), baked.width, baked.height);
        raster->image() = std::move(baked);
        carryCompositingProps(*layer, *raster);
        // transform stays identity: rasterizeLayer rendered into the parent's own coordinate space.
        const core::LayerId newId = raster->id();
        m_document->commands().push(
            std::make_unique<core::ReplaceLayerCommand>(id, std::move(raster), "Rasterize"));
        syncAfterEdit();
        if (m_layerPanel)
            m_layerPanel->setActive(newId);
    }

    // Layer -> Convert to Path: a Text layer's glyph outlines, or a parametric shape, promoted to
    // the editable cubic node model (core::vec::Path) on a VectorLayer. Exact -- see
    // core/vector/to_path.hpp and TextShaper::glyphPath. One undo step.
    void convertLayerToPathCommand(core::LayerId id) {
        if (!m_document)
            return;
        core::Layer* layer = m_document->find(id);
        if (layer == nullptr || layer->locked())
            return;

        core::vec::Object obj;
        std::string name = layer->name();
        if (auto* vl = layer->as<core::VectorLayer>()) {
            if (!vl->hasObject())
                return;
            obj = *vl->object(); // fill, stroke and paint order all survive the promotion
            obj.geometry = core::vec::pathFromGeometry(obj.geometry);
        } else if (auto* tl = layer->as<core::TextLayer>()) {
            std::size_t skipped = 0;
            const core::vec::Path outlines = textBlockOutlines(*tl, &skipped);
            if (outlines.subpaths.empty()) {
                transientStatus(_("Convert to Path: this text has no outlines to convert"));
                return;
            }
            if (skipped > 0) {
                // Colour glyphs (COLR/CPAL, emoji bitmaps) carry no outline. Say so rather than
                // silently dropping them: the converted path really is missing those characters.
                char buf[160];
                std::snprintf(buf, sizeof buf,
                              _("Convert to Path: %zu colour glyph(s) have no outline and were "
                                "left out"),
                              skipped);
                transientStatus(buf);
            }
            obj.geometry = outlines;
            // Text paints PER RUN; one vector Object carries ONE fill. The first run's paint wins,
            // and a block whose runs disagree says so -- silently recolouring half the letters would
            // be worse than either alternative. (Per-run fills want a group of objects; that does
            // not belong in this command.)
            const auto& runs = tl->block().runs;
            if (!runs.empty()) {
                obj.fill = runs.front().style.paint;
                const bool mixed = std::any_of(runs.begin(), runs.end(), [&](const auto& r) {
                    return r.length() > 0 && !(r.style.paint == runs.front().style.paint);
                });
                if (mixed)
                    transientStatus(_("Convert to Path: the text has several colours; the path "
                                      "takes the first"));
            }
            obj.stroke = core::vec::Stroke{}; // text has no stroke of its own yet
            if (tl->block().extrude.has_value()) {
                // A path is 2D. The flat glyph outlines are the honest answer for an extruded block,
                // but the user is about to lose a solid they can see, so say it out loud.
                transientStatus(_("Convert to Path: 3D depth is not a path; the flat outlines were "
                                  "taken"));
            }
        } else {
            return;
        }

        std::unique_ptr<core::VectorLayer> vec = m_document->makeVector(std::move(name));
        vec->setObject(std::move(obj));
        vec->setTransform(layer->transform()); // the path is in the old layer's local space
        carryCompositingProps(*layer, *vec);
        if (layer->mask() != nullptr)
            vec->setMask(*layer->mask()); // a converted outline keeps whatever masked the original
        if (layer->hasEffects())
            vec->setEffects(layer->effects());
        const core::LayerId newId = vec->id();
        m_document->commands().push(
            std::make_unique<core::ReplaceLayerCommand>(id, std::move(vec), "Convert to Path"));
        syncAfterEdit();
        if (m_layerPanel)
            m_layerPanel->setActive(newId);
    }

    // Every glyph outline of `tl`'s block, merged into one Path in the layer's local space. Colour
    // glyphs have no outline; they are counted into `skipped` rather than faked.
    [[nodiscard]] core::vec::Path textBlockOutlines(const core::TextLayer& tl,
                                                    std::size_t* skipped) {
        core::vec::Path out;
        out.fillRule = core::vec::FillRule::NonZero; // glyph winding
        const core::text::ShapedBlock shaped = m_textShaper.layout(tl.block(), m_fontDb);
        for (const core::text::ShapedGlyph& g : shaped.glyphs) {
            if (g.whitespace)
                continue;
            if (g.colorGlyph) {
                if (skipped != nullptr)
                    ++*skipped;
                continue;
            }
            core::vec::Path glyph = m_textShaper.glyphPath(g);
            for (core::vec::SubPath& sp : glyph.subpaths)
                out.subpaths.push_back(std::move(sp));
        }
        return out;
    }

    // Layer -> Merge Down (Ctrl+E), S36. The pair's KINDS pick the route -- there is no single
    // "the layer below must be a raster" gate any more:
    //   * shape + shape          -> ONE vector layer, whenever the two objects combine losslessly
    //                               (render::mergeDownVector); otherwise both rasterize, below.
    //   * adjustment over anything -> its grade is BAKED into the layer below. An adjustment has no
    //                               pixels of its own, so this is the only thing "merge it down"
    //                               can mean; its live scope was the whole backdrop under it, so
    //                               the status bar says so whenever there was more below.
    //   * everything else        -> both sides go through the ONE rasterizer (render::rasterizeLayer
    //                               via mergeDownBaked) and the result replaces the lower layer.
    //                               Text, shapes, gradients, textures, magic layers and whole
    //                               GROUPS all land here; a group below is flattened, which is what
    //                               merging into it means (and is narrated).
    // An ADJUSTMENT below is the one kind that cannot receive a merge: it has no pixels to hold it.
    // Every route is ONE undo step (CompositeCommand): the lower layer is rewritten and the upper
    // one removed together, so undo restores both.
    void mergeDownLayer() {
        if (!m_document)
            return;
        core::Layer* upper = activeLayerPtr();
        if (upper == nullptr)
            return;
        const std::optional<core::Document::Location> loc = m_document->locate(upper->id());
        if (!loc || loc->index == 0) { // children are bottom->top: below = index - 1
            transientStatus(_("Merge Down: no layer below to merge into"));
            return;
        }
        core::Layer* lower = &loc->parent->child(loc->index - 1);
        if (!upper->visible() || !lower->visible()) {
            transientStatus(_("Merge Down needs both layers visible"));
            return;
        }
        if (upper->locked() || lower->locked()) {
            transientStatus(_("Merge Down: a locked layer cannot be merged"));
            return;
        }
        if (lower->kind() == core::LayerKind::Adjustment) {
            transientStatus(_("Merge Down: an adjustment layer below has no pixels to merge into"));
            return;
        }
        settleTextCaches(); // text/texture pixels live in renderer-filled caches; fill them first
        const std::uint32_t docW = m_document->width();
        const std::uint32_t docH = m_document->height();
        const render::ResampleFilter filter = currentResampleFilter();
        const core::LayerId upperId = upper->id();
        const core::LayerId lowerId = lower->id();
        // Nothing composites BELOW the bottom child of the root, which is the one situation in
        // which a non-Normal blend mode can be baked at all (blending onto an empty backdrop is
        // just the source). See render::mergeDownBaked.
        const bool emptyBackdrop = loc->parent == &m_document->root() && loc->index == 1;

        std::unique_ptr<core::Command> rewriteLower; // what happens to the layer below
        core::LayerId nextActive = lowerId;
        const char* note = nullptr; // an honest caveat, narrated once the merge has landed

        // ---- shape + shape -> one path -----------------------------------------------------
        if (const auto* uv = upper->as<core::VectorLayer>()) {
            if (const auto* lv = lower->as<core::VectorLayer>()) {
                if (std::optional<core::vec::Object> combined = render::mergeDownVector(*uv, *lv)) {
                    std::unique_ptr<core::VectorLayer> merged = m_document->makeVector(lv->name());
                    merged->setObject(std::move(*combined));
                    merged->setTransform(lv->transform()); // the combine is in lower's local space
                    carryCompositingProps(*lv, *merged);
                    nextActive = merged->id();
                    rewriteLower = std::make_unique<core::ReplaceLayerCommand>(
                        lowerId, std::move(merged), "Merge Down");
                    note = _("Merge Down: the two shapes are one editable path now");
                }
            }
        }

        // ---- an adjustment baked into the layer below --------------------------------------
        if (rewriteLower == nullptr) {
            if (const auto* adj = upper->as<core::AdjustmentLayer>()) {
                auto* lr = lower->as<core::RasterLayer>();
                const bool plainRaster = lr != nullptr && !lr->hasMask() &&
                                         !(lr->hasEffects() && !lr->effects().empty());
                if (plainRaster) {
                    // Grade the raster's OWN pixels: nothing resamples and the layer keeps its
                    // transform, its mask-free grid placing the adjustment's document-window sheet.
                    common::Image graded = render::applyAdjustmentToImage(
                        *adj, lr->image(), lr->transform(), docW, docH);
                    rewriteLower =
                        std::make_unique<core::SetLayerPixelsCommand>(lowerId, std::move(graded));
                } else {
                    common::Image baked = render::rasterizeLayer(*lower, docW, docH, filter);
                    if (baked.rgba.empty()) {
                        transientStatus(_("Merge Down: the layer below has no pixels to grade"));
                        return;
                    }
                    common::Image graded = render::applyAdjustmentToImage(
                        *adj, baked, common::Affine2D::identity(), docW, docH);
                    std::unique_ptr<core::RasterLayer> raster =
                        m_document->makeRaster(lower->name(), graded.width, graded.height);
                    raster->image() = std::move(graded);
                    carryCompositingProps(*lower, *raster); // the bake folded transform/mask/effects
                    nextActive = raster->id();
                    rewriteLower = std::make_unique<core::ReplaceLayerCommand>(
                        lowerId, std::move(raster), "Merge Down");
                }
                if (loc->index > 1)
                    note = _("Merge Down: the adjustment now applies to that one layer, not to "
                             "everything below it");
            }
        }

        // ---- everything else: bake to pixels -----------------------------------------------
        if (rewriteLower == nullptr) {
            // Two honesty gates first. Both are about the STACK rather than the pair, which is why
            // render::mergeDownBaked cannot make them (its header spells out the arithmetic).
            if (lower->clipToBelow()) {
                transientStatus(_("Merge Down: the layer below is clipped to the layer under it; "
                                  "merge that pair first"));
                return;
            }
            if (lower->blendMode() != core::BlendMode::Normal && !emptyBackdrop) {
                transientStatus(_("Merge Down: the blend mode of the layer below cannot be baked; "
                                  "it would re-blend the merged pixels against what is under it"));
                return;
            }
            // The one pair that keeps its own pixel space: raster/magic onto a plain raster. It
            // resamples nothing, and it is the path a live coverage partition MUST take -- its two
            // halves recombine with the disjoint operator, never with `over`.
            const bool plainLower = lower->as<core::RasterLayer>() != nullptr &&
                                    !lower->hasMask() &&
                                    !(lower->hasEffects() && !lower->effects().empty()) &&
                                    lower->opacity() >= 1.0f;
            const bool pixelUpper = upper->as<core::RasterLayer>() != nullptr ||
                                    upper->as<core::MagicLayer>() != nullptr;
            const bool blendBakes = upper->blendMode() == core::BlendMode::Normal || emptyBackdrop;
            if (plainLower && pixelUpper &&
                (blendBakes || core::partitionPairLive(*lower, *upper))) {
                std::optional<common::Image> merged =
                    render::mergeDown(*upper, *lower->as<core::RasterLayer>());
                if (!merged) {
                    transientStatus(_("Merge Down: the active layer has no pixels to merge"));
                    return;
                }
                rewriteLower =
                    std::make_unique<core::SetLayerPixelsCommand>(lowerId, std::move(*merged));
            } else {
                render::MergeDownBake bake =
                    render::mergeDownBaked(*upper, *lower, docW, docH, filter, emptyBackdrop);
                if (bake.status == render::MergeDownBake::Status::UpperBlendUnbaked) {
                    transientStatus(_("Merge Down: this layer's blend mode cannot be baked; the "
                                      "layer below is not opaque everywhere this layer paints"));
                    return;
                }
                if (bake.status != render::MergeDownBake::Status::Ok || bake.image.rgba.empty()) {
                    transientStatus(_("Merge Down: there are no pixels to merge"));
                    return;
                }
                const bool flattenedGroup = lower->kind() == core::LayerKind::Group ||
                                            upper->kind() == core::LayerKind::Group;
                std::unique_ptr<core::RasterLayer> raster =
                    m_document->makeRaster(lower->name(), bake.image.width, bake.image.height);
                raster->image() = std::move(bake.image);
                // Deliberately NOT carryCompositingProps: mergeDownBaked consumed both layers'
                // opacity, blend mode and clip flag into the pixels, so carrying any of them across
                // would apply them a second time (its header proves why they cannot ride along).
                raster->setVisible(true);
                nextActive = raster->id();
                if (flattenedGroup && note == nullptr)
                    note = _("Merge Down: the group was flattened into the merged layer");
                rewriteLower = std::make_unique<core::ReplaceLayerCommand>(
                    lowerId, std::move(raster), "Merge Down");
            }
        }

        if (rewriteLower == nullptr)
            return; // every branch above has already explained itself
        auto cmd = std::make_unique<core::CompositeCommand>("Merge Down");
        cmd->add(std::move(rewriteLower));
        cmd->add(std::make_unique<core::RemoveLayerCommand>(upperId));
        m_document->commands().push(std::move(cmd));
        syncAfterEdit();
        if (m_layerPanel)
            m_layerPanel->setActive(nextActive); // the merged layer carries on as the active one
        if (note != nullptr)
            transientStatus(note);
    }
    // The panel refuses a structural edit on a locked layer silently (its own context menu greys
    // the item, so the "why" is already on screen). The Layer menu never greys, so it must say so.
    void deleteLayer() {
        if (m_layerPanel == nullptr)
            return;
        if (m_layerPanel->activeLayerLocked()) {
            transientStatus(_("This layer is locked. Unlock it to delete it."));
            return;
        }
        m_layerPanel->deleteActive();
    }
    void groupLayers() {
        if (m_layerPanel == nullptr)
            return;
        if (m_layerPanel->activeLayerLocked()) {
            transientStatus(_("This layer is locked. Unlock it to group it."));
            return;
        }
        m_layerPanel->groupActive();
    }

    // Select menu (S13). Each pushes a SetSelectionCommand (undoable) and re-syncs the canvas's
    // marching ants directly -- the composite itself is untouched, so no recomposite is queued.
    void selectAll() {
        if (!m_document)
            return;
        const auto w = m_document->width();
        const auto h = m_document->height();
        m_document->commands().push(
            std::make_unique<core::SetSelectionCommand>(core::Selection::rectangle(
                w, h, {0.0, 0.0, static_cast<double>(w), static_cast<double>(h)})));
        syncSelection();
    }
    void deselect() {
        if (!m_document || m_document->selection().isEmpty())
            return; // already no selection; don't add an undo step that does nothing
        m_lastDeselected = m_document->selection(); // what Select ▸ Reselect puts back (S53-b)
        m_document->commands().push(std::make_unique<core::SetSelectionCommand>(core::Selection{}));
        syncSelection();
    }
    void invertSelection() {
        if (!m_document || m_document->selection().isEmpty())
            return; // "no selection" has no complement (Photoshop disables Inverse too)
        m_document->commands().push(
            std::make_unique<core::SetSelectionCommand>(m_document->selection().inverted()));
        syncSelection();
    }

    // S18 Select-menu morphology (docs/research-select-brush.md §4), now a LIVE-PREVIEW corner panel
    // (supersedes the old modal "N px" prompt): the four menu items open the SelectMorphPanel seeded
    // to their op, every mode/amount change previews the morphed selection on the marching ants, and
    // Apply lands ONE SetSelectionCommand through the marquee's commit funnel. Gated on a real
    // selection, like Inverse -- there is nothing to grow / feather without one. The result may
    // collapse to "no selection" (e.g. Shrink past the whole mask), which commitToolSelection lands
    // honestly. Grow/Shrink take integer px; Feather/Smooth a fractional radius.
    static core::Selection morphApply(ui::SelectMorphMode mode, const core::Selection& s,
                                      double amount) {
        switch (mode) {
            case ui::SelectMorphMode::Grow: return s.grown(static_cast<int>(std::lround(amount)));
            case ui::SelectMorphMode::Shrink:
                return s.shrunk(static_cast<int>(std::lround(amount)));
            case ui::SelectMorphMode::Feather: return s.feathered(amount);
            case ui::SelectMorphMode::Smooth: return s.smoothed(amount);
        }
        return s;
    }
    [[nodiscard]] static const char* morphLabel(ui::SelectMorphMode mode) {
        switch (mode) {
            case ui::SelectMorphMode::Grow: return _("Grow Selection");
            case ui::SelectMorphMode::Shrink: return _("Shrink Selection");
            case ui::SelectMorphMode::Feather: return _("Feather Selection");
            case ui::SelectMorphMode::Smooth: return _("Smooth Selection");
        }
        return _("Modify Selection");
    }
    // Open (or re-point) the morphology panel on `mode`. Snapshots the current selection as the
    // preview base, requests the panel through the arbiter, and seeds the first live preview.
    void openSelectMorph(ui::SelectMorphMode mode) {
        if (!m_document || m_document->selection().isEmpty() || m_selectMorphPanel == nullptr)
            return;
        m_selectMorphOriginal = m_document->selection(); // the base every preview morphs
        m_selectMorphPanel->configure(mode, 4.0);        // seed the op + the default amount
        m_selectMorphActive = true;
        m_selectMorphPreviewValid = false; // fresh base + op: the next frame must re-morph
        if (m_cornerVisible != kPanelSelectMorph)
            m_panelArbiter.toggle(kPanelSelectMorph); // request open (the menu never toggles shut)
        syncCornerPanels();
        previewSelectMorph(); // seed/refresh the ants (also covers re-opening with a different op)
    }
    // Request a live preview. This does NOT morph inline: it marks the preview dirty and lets the
    // frame loop (updateSelectMorph -> flushSelectMorphPreview) run at most ONE morph per frame with
    // the latest mode/amount. That coalescing is what keeps the amount slider from wedging: a slider
    // drag streams a burst of value-changed callbacks, and morphing synchronously on each (a feather
    // is a full-plane blur) buried the UI under a per-event backlog it could never drain. Deferred by
    // one frame (~16 ms) the ants still track the slider live.
    void previewSelectMorph() { m_selectMorphPreviewDirty = true; }
    // The deferred morph, run from the frame loop: build the morphed snapshot at the panel's current
    // mode/amount and push it to the canvas ants directly -- NO command, so nothing lands on the undo
    // stack until Apply. Guards against re-entry and skips a re-morph when nothing actually changed.
    void flushSelectMorphPreview() {
        if (m_selectMorphPreviewing) // never morph on top of a morph (belt-and-braces re-entry guard)
            return;
        if (!m_document || m_canvas == nullptr || m_selectMorphPanel == nullptr)
            return;
        m_selectMorphPreviewDirty = false;
        const ui::SelectMorphMode mode = m_selectMorphPanel->mode();
        const double amount = m_selectMorphPanel->amount();
        // Skip a redundant re-morph: identical op + amount to the last preview (repeated callbacks,
        // the open + arbiter-show double seed, a frame with no slider motion). openSelectMorph clears
        // m_selectMorphPreviewValid so a fresh base/op always re-morphs even at the same numbers.
        if (m_selectMorphPreviewValid && mode == m_selectMorphPreviewMode &&
            amount == m_selectMorphPreviewAmount)
            return;
        m_selectMorphPreviewing = true;
        const core::Selection out = morphApply(mode, m_selectMorphOriginal, amount);
        if (out.isEmpty())
            m_canvas->setSelectionMask(0, 0, nullptr);
        else
            m_canvas->setSelectionMask(out.width(), out.height(), out.data().data());
        if (m_statusBar)
            m_statusBar->setSelectionBounds(out.bounds());
        m_selectMorphPreviewMode = mode;
        m_selectMorphPreviewAmount = amount;
        m_selectMorphPreviewValid = true;
        m_selectMorphPreviewing = false;
    }
    // Apply: land the morphed selection as one undoable step (commitToolSelection re-syncs the ants
    // to the committed mask).
    void applySelectMorph() {
        if (!m_document || m_selectMorphPanel == nullptr)
            return;
        core::Selection out = morphApply(m_selectMorphPanel->mode(), m_selectMorphOriginal,
                                         m_selectMorphPanel->amount());
        commitToolSelection(std::move(out), 0, morphLabel(m_selectMorphPanel->mode()));
    }
    // Close the panel via the arbiter, then reconcile the canvas to the document's ACTUAL selection
    // (Apply committed the morphed mask there; Cancel left the original) -- so the button paths drop
    // the transient preview with no one-frame flash.
    void closeSelectMorph() {
        if (m_cornerVisible == kPanelSelectMorph)
            m_panelArbiter.toggle(kPanelSelectMorph); // clears the request
        syncCornerPanels();
        m_selectMorphActive = false;
        m_selectMorphPreviewDirty = false;
        m_selectMorphPreviewValid = false;
        restoreSelectionCanvas();
    }
    // Re-upload the document's real selection to the canvas, discarding any transient morph preview
    // (the preview bypassed the revision gate, so syncSelection alone would skip the re-upload).
    void restoreSelectionCanvas() {
        if (!m_document || m_canvas == nullptr)
            return;
        const core::Selection& sel = m_document->selection();
        if (sel.isEmpty())
            m_canvas->setSelectionMask(0, 0, nullptr);
        else
            m_canvas->setSelectionMask(sel.width(), sel.height(), sel.data().data());
        if (m_statusBar)
            m_statusBar->setSelectionBounds(sel.bounds());
        m_syncedSelectionRev = m_document->selectionRevision(); // the canvas now matches the document
    }
    // Per-frame: (1) when the morph panel has closed by a route that did NOT run closeSelectMorph
    // (Esc, or the selection cleared out from under it so the arbiter dropped the request), drop the
    // transient preview and show the document's real selection again; (2) otherwise flush a pending
    // live preview -- at most one morph per frame, so a slider drag's event storm collapses to one.
    void updateSelectMorph() {
        if (!m_selectMorphActive)
            return;
        const bool shown = m_selectMorphPanel != nullptr && m_selectMorphPanel->shown();
        if (!shown) {
            restoreSelectionCanvas();
            m_selectMorphActive = false;
            m_selectMorphPreviewDirty = false;
            m_selectMorphPreviewValid = false;
            return;
        }
        if (m_selectMorphPreviewDirty)
            flushSelectMorphPreview();
    }
    void growSelection() { openSelectMorph(ui::SelectMorphMode::Grow); }
    void shrinkSelection() { openSelectMorph(ui::SelectMorphMode::Shrink); }
    void featherSelection() { openSelectMorph(ui::SelectMorphMode::Feather); }
    void smoothSelection() { openSelectMorph(ui::SelectMorphMode::Smooth); }

    // ---- Image menu (S53, docs/image-operations.md) ---------------------------------------------
    //
    // Three of the seven operations take parameters, so they open the ImageOpsPanel (the
    // morphology panel's live-preview shape, NOT a modal): every control change stages a canvas
    // overlay of the new canvas with no command behind it, and Apply lands exactly one. The other
    // four -- the orientations and Trim -- have nothing to ask, so they act on the click.
    static constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

    // Open (or re-point) the panel on `mode`. Re-picking a DIFFERENT Image item while the panel is
    // already up has to rebuild it here: the arbiter sees no visibility transition, so
    // syncCornerPanels would return without ever reaching showCornerPanel.
    void openImageOps(ImageOpsPanel::Mode mode) {
        if (!m_document || m_imageOpsPanel == nullptr || m_canvas == nullptr)
            return;
        m_pendingImageOpMode = mode;
        m_imageOpsActive = true;
        m_imageOpRequest.reset();
        m_imageOpPreviewDirty = false;
        // Refresh the Fill = "Pattern…" editor's preview fidelity from the document-wide AA
        // setting (the Move tool's AA combo), so its previews read like the canvas -- re-read per
        // open, since that setting moves. Same source the Fill dialog passes its own flyout.
        if (m_imageOpsPatternFlyout != nullptr)
            m_imageOpsPatternFlyout->setAntialias(currentResampleFilter() !=
                                                  render::ResampleFilter::Nearest);
        if (m_cornerVisible == kPanelImageOps) {
            m_imageOpsPanel->openFor(mode, m_canvas, m_document->width(), m_document->height(),
                                     m_document->dpi());
            return;
        }
        m_panelArbiter.toggle(kPanelImageOps); // request open (the menu never toggles shut)
        syncCornerPanels();
    }

    // A control moved. Like the morphology panel this does NOT rebuild the overlay inline: it
    // records the request and marks it dirty, and the frame loop stages at most one overlay per
    // frame. A number field fires a callback per keystroke, and a proportions-locked edit fires
    // two -- collapsing that burst to one frame is what keeps typing a size smooth.
    void previewImageOps(const ImageOpsPanel::Request& r) {
        m_imageOpRequest = r;
        m_imageOpPreviewDirty = true;
    }

    // Stage the canvas overlay for the pending request. NO command: the overlay is the whole
    // preview, and nothing reaches the undo stack until Apply.
    void flushImageOpPreview() {
        m_imageOpPreviewDirty = false;
        if (!m_document || m_canvas == nullptr || !m_imageOpRequest)
            return;
        const ImageOpsPanel::Request& r = *m_imageOpRequest;
        const auto docW = static_cast<double>(m_document->width());
        const auto docH = static_cast<double>(m_document->height());
        VulkanCanvas::ImageOpPreview p;
        std::uint32_t hudW = r.width;
        std::uint32_t hudH = r.height;
        switch (r.mode) {
            case ImageOpsPanel::Mode::CanvasSize: {
                // canvasRectFor answers with the OLD canvas's top-left in NEW-canvas coordinates.
                // The overlay wants the mirror image of that -- the staged NEW canvas expressed in
                // the CURRENT document's coordinates -- which is exactly (-x, -y, newW, newH).
                const render::CanvasRect rect = render::canvasRectFor(
                    m_document->width(), m_document->height(), r.width, r.height, r.anchor);
                p.x = -rect.x;
                p.y = -rect.y;
                p.w = r.width;
                p.h = r.height;
                break;
            }
            case ImageOpsPanel::Mode::ImageSize:
                // An Image Size RESAMPLES. Staging the rect at the size it already is drew the same
                // rectangle and read as no change at all ("Image Size does not yield a visual
                // indicator of growth"). Stage it at the NEW pixel size, anchored on the current
                // origin: the crop channel's existing language then does the work -- a grow washes
                // + hatches the region outside the document in kExpandGreen, a shrink dims what the
                // result will no longer reach -- and `scale` tells the canvas to add the ghost
                // outline of the frame being replaced plus the scale factor beside the HUD.
                p.x = 0;
                p.y = 0;
                p.w = r.width;
                p.h = r.height;
                p.scale = true;
                break;
            case ImageOpsPanel::Mode::RotateArbitrary: {
                // The canvas grows to the rotated document's bounding box, about the canvas
                // centre, so in CURRENT document coordinates the staged frame is that box centred
                // on the same point -- and TURNED THE OTHER WAY. The overlay can only rotate the
                // quad, not the live composite under it, and turning the picture clockwise is the
                // same picture inside a frame turned counter-clockwise around it: the new canvas's
                // axes are the document's mapped through the INVERSE rotation. Hence the negated
                // angle below (the crop channel rotates the drawn rect by +angle about its centre,
                // ui/crop_gesture.cpp cropFrameToDoc).
                const double rad = r.angleDeg * kDegToRad;
                const double ca = std::abs(std::cos(rad));
                const double sa = std::abs(std::sin(rad));
                const double nw = docW * ca + docH * sa;
                const double nh = docW * sa + docH * ca;
                p.x = std::lround((docW - nw) * 0.5);
                p.y = std::lround((docH - nh) * 0.5);
                p.w = static_cast<std::uint32_t>(std::max<long>(1, std::lround(nw)));
                p.h = static_cast<std::uint32_t>(std::max<long>(1, std::lround(nh)));
                p.angleRad = -rad; // about the staged rect's centre, which IS the canvas centre
                hudW = p.w;
                hudH = p.h;
                break;
            }
        }
        char hud[64];
        // TRANSLATORS: the canvas HUD's size readout during an Image-menu preview, e.g.
        // "2048 × 1536 px". Both numbers are pixel counts; the separator is U+00D7, matching the
        // Crop tool's own size HUD (vulkan_canvas.cpp) so the two read identically.
        std::snprintf(hud, sizeof hud, _("%u × %u px"), static_cast<unsigned>(hudW),
                      static_cast<unsigned>(hudH));
        p.hud = hud;
        m_canvas->setImageOpPreview(p);
    }

    // The new canvas's extent for a request -- r.width/r.height for a Canvas Size, and the rotated
    // bounding box for an arbitrary rotation (whose canvas is a FUNCTION of the angle, not typed).
    void imageOpCanvasExtent(const ImageOpsPanel::Request& r, std::uint32_t& w,
                             std::uint32_t& h) const {
        w = r.width;
        h = r.height;
        if (r.mode != ImageOpsPanel::Mode::RotateArbitrary || !m_document)
            return;
        const double rad = r.angleDeg * kDegToRad;
        const double ca = std::abs(std::cos(rad));
        const double sa = std::abs(std::sin(rad));
        const auto docW = static_cast<double>(m_document->width());
        const auto docH = static_cast<double>(m_document->height());
        w = static_cast<std::uint32_t>(std::max<long>(1, std::lround(docW * ca + docH * sa)));
        h = static_cast<std::uint32_t>(std::max<long>(1, std::lround(docW * sa + docH * ca)));
    }

    // Materialize the expansion fill the panel deliberately left unresolved. The solid kinds
    // arrive already resolved in r.fill; Transparent resolves to nothing; Inpaint is the async
    // path below and never reaches here. What is left is Gradient / Pattern, rasterized across the
    // WHOLE new canvas through render::computeFillPaint -- the very function Edit→Fill…'s own
    // gradient/pattern fills go through, so one paint is evaluated one way everywhere. The command
    // reads only the expansion region out of the image, so painting all of it costs a buffer, not
    // a wrong result.
    [[nodiscard]] std::optional<render::CropFill>
    resolveImageOpFill(const ImageOpsPanel::Request& r) const {
        if (r.fillMode != ImageOpsPanel::FillMode::Gradient &&
            r.fillMode != ImageOpsPanel::FillMode::Pattern)
            return r.fill;
        std::uint32_t w = 0;
        std::uint32_t h = 0;
        imageOpCanvasExtent(r, w, h);
        if (w == 0 || h == 0)
            return std::nullopt;
        const common::Image blank(w, h); // transparent: the paint composites over nothing
        common::Image painted = render::computeFillPaint(
            blank, std::vector<std::uint8_t>{}, r.paint, 0, 0, core::BlendMode::Normal, 1.0f,
            /*protectAlpha=*/false,
            currentResampleFilter() != render::ResampleFilter::Nearest);
        // The expansion sits at the BOTTOM of the stack and is opaque, like every solid mode: a
        // gradient that fades to transparent must still leave an opaque canvas behind it.
        for (std::size_t i = 3; i < painted.rgba.size(); i += 4)
            painted.rgba[i] = 255;
        render::CropFill fill{{0, 0, 0, 255}, _("Canvas fill")};
        fill.pixels = std::move(painted);
        return fill;
    }

    // Apply: build the one command this request means and land it. Every builder returns null for
    // a no-op, which is narrated rather than pushed (see landDocumentOp).
    void applyImageOps(const ImageOpsPanel::Request& r) {
        if (!m_document)
            return;
        switch (r.mode) {
            case ImageOpsPanel::Mode::CanvasSize:
                // Inpaint is asynchronous by nature (the engine takes seconds on a real canvas):
                // hand off to the worker, which lands the SAME single resize command when it is
                // done. Every other fill resolves synchronously, right here.
                if (r.fillMode == ImageOpsPanel::FillMode::Inpaint) {
                    startImageOpExpandFill(r);
                    return;
                }
                landDocumentOp(render::buildCanvasResizeCommand(*m_document, r.width, r.height,
                                                                r.anchor, resolveImageOpFill(r)),
                               _("Canvas Size: the canvas is already that size"));
                break;
            case ImageOpsPanel::Mode::ImageSize:
                landDocumentOp(render::buildImageResizeCommand(*m_document, r.width, r.height,
                                                              r.filter),
                               _("Image Size: the image is already that size"));
                break;
            case ImageOpsPanel::Mode::RotateArbitrary:
                landDocumentOp(render::buildRotateDocumentCommand(*m_document,
                                                                  r.angleDeg * kDegToRad, r.filter,
                                                                  resolveImageOpFill(r)),
                               _("Rotate: that angle is a whole number of turns"));
                break;
        }
    }

    // ---- Canvas Size expansion Inpaint fill (S53 fill parity) ---------------------------------
    //
    // The Crop tool's startCropExpandFill, pointed at the Image menu's own resize: the worker
    // heals the newly exposed margin on a copy of the flatten placed at its post-resize offset
    // (the remap is a pure translation, so the old flatten blitted there IS the post-resize
    // composite wherever the old canvas showed anything), and the healed pixels land through the
    // very same buildCanvasResizeCommand as a solid fill -- ONE undo step, on the bottom of the
    // stack.
    //
    // WHAT IT ACTUALLY DOES AT AN EXPANSION EDGE, plainly: the hole touches the canvas border and
    // has picture on ONE side only, so this is an extrapolation outward rather than a hole being
    // closed. The engine reads the picture's own edge band and continues it -- a sky stays sky, a
    // wall keeps its texture, a gradient keeps ramping. Convincing for a modest margin, and it
    // washes out into a soft smear the further from real pixels it has to invent, which is why it
    // is offered as a fill rather than promised as a result. A pure shrink exposes nothing at all
    // and short-circuits to the plain resize below without spinning the engine up.
    void startImageOpExpandFill(const ImageOpsPanel::Request& r) {
        if (!m_document || m_inpaintRunning || m_recomposeRunning)
            return;
        const std::uint32_t oldW = m_document->width();
        const std::uint32_t oldH = m_document->height();
        if (r.width == oldW && r.height == oldH) {
            transientStatus(_("Canvas Size: the canvas is already that size"));
            return;
        }
        // A9's twin (the Image menu's Canvas Size, same expansion seed, same consumer class).
        const common::Image& flat =
            hostComposite(render::consumers::kCropExpandFill, render::Freshness::Current);
        if (flat.empty() || flat.width != oldW || flat.height != oldH) {
            transientStatus(_("Try again in a moment (the canvas is refreshing)"));
            return; // mid-resize transient: the next frame's recomposite refreshes it
        }
        // canvasRectFor answers with the OLD canvas's top-left inside the NEW one -- exactly the
        // offset the flatten has to be blitted at for the healed image to line up.
        const render::CanvasRect rect =
            render::canvasRectFor(oldW, oldH, r.width, r.height, r.anchor);
        common::Image seed(r.width, r.height);
        common::blitRegion(seed, flat, rect.x, rect.y);
        core::Selection margin(r.width, r.height);
        std::vector<std::uint8_t>& md = margin.data();
        bool anyExposed = false;
        for (long y = 0; y < static_cast<long>(r.height); ++y) {
            const bool rowInOld = y >= rect.y && y < rect.y + static_cast<long>(oldH);
            for (long x = 0; x < static_cast<long>(r.width); ++x) {
                if (rowInOld && x >= rect.x && x < rect.x + static_cast<long>(oldW))
                    continue;
                md[static_cast<std::size_t>(y) * r.width + static_cast<std::size_t>(x)] = 255;
                anyExposed = true;
            }
        }
        if (!anyExposed) {
            // A pure shrink: nothing was exposed, so there is nothing to reconstruct. Land the
            // resize straight away rather than running an engine over an empty hole.
            landDocumentOp(render::buildCanvasResizeCommand(*m_document, r.width, r.height,
                                                            r.anchor, std::nullopt),
                           _("Canvas Size: the canvas is already that size"));
            return;
        }

        auto job = std::make_unique<ImageOpExpandJob>();
        job->width = r.width;
        job->height = r.height;
        job->anchor = r.anchor;
        job->params = m_inpaintParams; // active backend + preset, like every engine run
        job->input = common::toFloat(seed);
        job->hole = std::move(margin);

        m_imageOpExpandJob = std::move(job);
        m_inpaintRunning = true; // one engine run at a time, sharing the inpaint busy-state
        setMainControlsEnabled(false);
        m_statusBar->onProgressCancel([this] {
            if (m_imageOpExpandJob)
                m_imageOpExpandJob->cancelRequested.store(true);
        });
        m_statusBar->setProgress(0.0f, _("Analyzing"));

        ImageOpExpandJob* j = m_imageOpExpandJob.get();
        j->worker = std::thread([this, j] {
            const core::inpaint::InpaintRequest req{j->input, j->hole, j->params,
                                                    &j->cancelRequested};
            const core::inpaint::ProgressFn fn =
                [j](const core::inpaint::InpaintProgress& p) -> bool {
                {
                    std::lock_guard<std::mutex> lk(j->mutex);
                    j->fraction = p.fraction;
                    j->stage.assign(p.stage);
                }
                return !j->cancelRequested.load();
            };
            j->result = m_inpaintEngine.run(req, fn);
            j->done.store(true);
        });
        Fl::add_timeout(kInpaintPollSeconds, imageOpExpandPollTimer, this);
    }

    static void imageOpExpandPollTimer(void* self) {
        static_cast<MainWindow*>(self)->pollImageOpExpand();
    }

    void pollImageOpExpand() {
        if (!m_imageOpExpandJob)
            return;
        ImageOpExpandJob* j = m_imageOpExpandJob.get();
        float frac = 0.0f;
        std::string stage;
        {
            std::lock_guard<std::mutex> lk(j->mutex);
            frac = j->fraction;
            stage = j->stage;
        }
        if (m_statusBar)
            m_statusBar->setProgress(frac, localizedInpaintStage(stage));
        if (j->done.load()) {
            finishImageOpExpandFill();
            return;
        }
        Fl::repeat_timeout(kInpaintPollSeconds, imageOpExpandPollTimer, this);
    }

    void finishImageOpExpandFill() {
        if (!m_imageOpExpandJob)
            return;
        ImageOpExpandJob* j = m_imageOpExpandJob.get();
        if (j->worker.joinable())
            j->worker.join();
        const bool cancelled = j->cancelRequested.load();
        m_statusBar->hideProgress();
        m_statusBar->onProgressCancel(nullptr);
        setMainControlsEnabled(true);
        m_inpaintRunning = false;

        if (!cancelled && j->result.ok && m_document) {
            common::Image healed = common::toImage8(j->result.image);
            // The fill sits at the BOTTOM of the stack, opaque like the solid modes (engines
            // preserve the seed's alpha, which is 0 in the margin). Only the margin is ever read.
            for (std::size_t i = 3; i < healed.rgba.size(); i += 4)
                healed.rgba[i] = 255;
            render::CropFill fill{{0, 0, 0, 255}, _("Canvas fill")};
            fill.pixels = std::move(healed);
            landDocumentOp(render::buildCanvasResizeCommand(*m_document, j->width, j->height,
                                                            j->anchor, fill),
                           _("Canvas Size: the canvas is already that size"));
            char buf[128];
            std::snprintf(buf, sizeof buf, _("Resized canvas to %u × %u px"), j->width, j->height);
            transientStatus(buf);
        } else if (cancelled) {
            transientStatus(_("Inpaint cancelled"));
        } else if (!j->result.ok) {
            uiLog().warn("canvas-size expansion inpaint failed: {}", j->result.detail);
            transientStatus(j->result.detail.empty() ? std::string(_("Inpaint failed"))
                                                     : _("Inpaint failed: ") + j->result.detail);
        }
        m_imageOpExpandJob.reset();
    }

    // Close the panel through the arbiter and drop the staged overlay. Apply has already landed
    // its command by the time this runs, so the canvas simply goes back to showing the document.
    void closeImageOps() {
        if (m_cornerVisible == kPanelImageOps)
            m_panelArbiter.toggle(kPanelImageOps); // clears the request
        syncCornerPanels();
        m_imageOpsActive = false;
        m_imageOpPreviewDirty = false;
        m_imageOpRequest.reset();
        if (m_canvas != nullptr)
            m_canvas->setImageOpPreview(std::nullopt);
    }

    // Per-frame safety net, beside updateSelectMorph: (1) when the panel closed by a route that
    // did NOT run closeImageOps (Esc in the popover, the document closing out from under it, a
    // theme rebuild), drop the staged overlay; (2) otherwise stage at most one pending preview.
    void updateImageOps() {
        if (!m_imageOpsActive)
            return;
        const bool shown = m_imageOpsPanel != nullptr && m_imageOpsPanel->shown();
        if (!shown) {
            if (m_canvas != nullptr)
                m_canvas->setImageOpPreview(std::nullopt);
            m_imageOpsActive = false;
            m_imageOpPreviewDirty = false;
            m_imageOpRequest.reset();
            return;
        }
        if (m_imageOpPreviewDirty)
            flushImageOpPreview();
    }

    // The ONE landing path for every Image-menu operation. A null command means the builder found
    // nothing to do; an empty undo step is worse than none (History would fill with rows that
    // changed nothing), so the refusal is narrated instead -- the Merge Down convention.
    void landDocumentOp(std::unique_ptr<core::Command> cmd, const char* noopMessage) {
        if (!m_document)
            return;
        if (cmd == nullptr) {
            transientStatus(noopMessage);
            return;
        }
        settleTextCaches(); // a bake reads TextLayer/TextureLayer pixels out of renderer caches
        m_document->commands().push(std::move(cmd));
        syncAfterEdit();
        // ⚠ ResizeCanvasCommand moves no layer revision, so the dock's cached group / adjustment /
        // vector thumbnails would keep rendering the OLD canvas framing. syncAfterEdit's refresh()
        // rebuilds the ROWS; this is what re-renders their pictures.
        if (m_layerPanel != nullptr)
            m_layerPanel->refreshThumbnails();
        // The remap rebased (or cleared) the selection; the ants bypass the revision gate while a
        // panel preview is live, so re-upload the document's real mask rather than trusting it.
        restoreSelectionCanvas();
    }

    // The four lossless orientations and Trim to Content: no parameters, so no panel. NOTE the
    // orientation builder deliberately takes no ResampleFilter -- a quarter turn is a signed axis
    // permutation and must stay byte-exact, so currentResampleFilter() must never reach it.
    void orientDocument(render::DocOrient op) {
        if (!m_document)
            return;
        landDocumentOp(render::buildOrientCommand(*m_document, op),
                       _("Rotate: there is nothing to turn"));
    }
    void trimToContent() {
        if (!m_document)
            return;
        landDocumentOp(render::buildTrimToContentCommand(*m_document),
                       _("Trim to Content: the canvas already hugs the visible content"));
    }

    // ---- Type menu (S53-b) ----------------------------------------------------------------------
    //
    // Every item here is re-exposure of an API that already existed; nothing new reaches the
    // engine. All of it lands as ONE core::SetTextCommand on the target block (coalesceId 0 -- a
    // menu pick is discrete, unlike the Type panel's dragged controls, so each is its own step).
    //
    // The target is the block being EDITED when a Type session is live, else the active layer when
    // it happens to be text. Items that need a target are greyed by syncDynamicMenuItems rather
    // than accepting the click and doing nothing.
    [[nodiscard]] core::TextLayer* typeMenuTarget(bool narrate = true) {
        if (!m_document)
            return nullptr;
        core::LayerId id =
            m_canvas != nullptr ? m_canvas->textEditTarget() : core::kInvalidLayerId;
        if (id == core::kInvalidLayerId && m_layerPanel != nullptr)
            id = m_layerPanel->activeLayer();
        core::Layer* l = m_document->find(id);
        auto* tl = l != nullptr ? l->as<core::TextLayer>() : nullptr;
        if (tl == nullptr && narrate)
            transientStatus(_("Select a text layer first"));
        return tl;
    }

    // One Type-menu edit = one SetTextCommand. A mutation that changes nothing (re-picking the
    // radio item that is already on) pushes NO command -- an undo step that undoes nothing is a
    // lie in the History panel.
    void applyTypeMenuEdit(const char* label,
                           const std::function<void(core::text::TextBlock&)>& mutate) {
        core::TextLayer* tl = typeMenuTarget();
        if (tl == nullptr)
            return;
        core::text::TextBlock next = tl->block();
        mutate(next);
        if (next == tl->block())
            return;
        m_document->commands().push(
            std::make_unique<core::SetTextCommand>(tl->id(), std::move(next), label, 0));
        syncAfterEdit();       // setBlock invalidated the bounds; re-shape + recomposite
        markTextThumbDirty();  // ... and bring the dock row's picture current once it settles
    }

    void setTypeWritingMode(core::text::WritingMode m) {
        applyTypeMenuEdit(_("Text Orientation"),
                          [m](core::text::TextBlock& b) { b.writingMode = m; });
    }
    void setTypeAntiAlias(core::text::AntiAlias a) {
        // Two modes on purpose: the engine is binary (None / Grayscale). Subpixel exists in the
        // enum but only ever survives on an opaque, axis-aligned, unrotated, non-3D backdrop --
        // the renderer silently degrades it everywhere else -- so offering it from a menu would
        // promise a look the canvas usually will not deliver.
        applyTypeMenuEdit(_("Text Anti-Aliasing"), [a](core::text::TextBlock& b) { b.aa = a; });
    }
    void setTypeKerning(core::text::Kerning k) {
        // Kerning is a per-RUN char style, so this walks every run (and the empty-block style, so
        // the next character typed inherits the choice too).
        applyTypeMenuEdit(_("Kerning"), [k](core::text::TextBlock& b) {
            for (core::text::StyleRun& r : b.runs)
                r.style.kerning = k;
            b.emptyStyle.kerning = k;
        });
    }
    void setTypeDirection(core::text::Paragraph::Direction d) {
        applyTypeMenuEdit(_("Text Direction"), [d](core::text::TextBlock& b) {
            for (core::text::Paragraph& p : b.paragraphs)
                p.direction = d;
        });
    }

    // Point <-> Area. Going to Area seeds the wrapping box from the block's own laid-out bounds,
    // so the text does not reflow the instant it becomes wrappable; going to Point drops the box
    // (Point text does not wrap, and a stale areaSize would come back on the next round trip).
    void setTypeFrame(core::text::TextFrame frame) {
        core::TextLayer* tl = typeMenuTarget();
        if (tl == nullptr)
            return;
        std::optional<common::Rect> box;
        if (frame == core::text::TextFrame::Area)
            box = core::text::layoutBounds(m_textShaper, tl->block(), m_fontDb);
        applyTypeMenuEdit(frame == core::text::TextFrame::Area ? _("Convert to Area Text")
                                                               : _("Convert to Point Text"),
                          [frame, box](core::text::TextBlock& b) {
                              b.frame = frame;
                              if (frame == core::text::TextFrame::Area) {
                                  if (box && box->w > 0.0 && box->h > 0.0)
                                      b.areaSize = {box->w, box->h};
                              } else {
                                  b.areaSize = {0.0, 0.0};
                              }
                          });
    }

    // The one vector layer in the Move tool's multi-selection that is NOT `exclude` -- the path a
    // "Text on Selected Path" is meant to flow along. Null when the selection does not name
    // exactly one.
    [[nodiscard]] core::VectorLayer* selectedPathLayer(core::LayerId exclude) {
        if (!m_document || m_layerPanel == nullptr)
            return nullptr;
        core::VectorLayer* found = nullptr;
        core::GroupLayer& root = m_document->root();
        for (std::size_t i = 0; i < root.childCount(); ++i) {
            core::Layer& child = root.child(i);
            if (child.id() == exclude || !m_layerPanel->isInMoveSelection(child.id()))
                continue;
            auto* vl = child.as<core::VectorLayer>();
            if (vl == nullptr || !vl->hasObject())
                continue;
            if (found != nullptr)
                return nullptr; // ambiguous: two paths, no way to pick one for the user
            found = vl;
        }
        return found;
    }

    // Type -> Text on Selected Path (§9). The block references the vector layer BY ID, so editing
    // or moving the path re-flows the text non-destructively; `baked` is derived state we seed
    // here (in the TEXT layer's local space) so the very first composite already flows, and the
    // frame loop's updateTextPathFits keeps it current from then on.
    void typeOnSelectedPath() {
        core::TextLayer* tl = typeMenuTarget();
        if (tl == nullptr)
            return;
        core::VectorLayer* path = selectedPathLayer(tl->id());
        if (path == nullptr) {
            transientStatus(
                _("Text on Path: select the text layer and exactly one shape layer together"));
            return;
        }
        const common::Affine2D toLocal =
            core::worldTransform(*tl).inverse().value_or(common::Affine2D::identity()) *
            core::worldTransform(*path);
        core::vec::Contours baked = core::vec::flatten(path->object()->geometry);
        for (core::vec::Contour& c : baked)
            for (common::Vec2& pt : c.points)
                pt = toLocal.apply(pt);
        const double total = core::vec::contourLength(baked);
        if (total <= 1e-9) {
            transientStatus(_("Text on Path: that shape has no outline to flow along"));
            return;
        }
        core::text::PathFit fit;
        fit.layer = path->id();
        fit.baked = std::move(baked);
        fit.s0 = 0.0;
        fit.s1 = total; // the whole path; the on-canvas brackets slide these afterwards
        applyTypeMenuEdit(_("Text on Path"),
                          [&fit](core::text::TextBlock& b) { b.pathFit = fit; });
    }

    void typeReleaseFromPath() {
        core::TextLayer* tl = typeMenuTarget();
        if (tl == nullptr)
            return;
        if (!tl->block().pathFit) {
            transientStatus(_("Release from Path: this text is not on a path"));
            return;
        }
        // The block keeps its own transform, so the text stays where the path put it and simply
        // stops following it -- which is what "release" means everywhere else that has it.
        applyTypeMenuEdit(_("Release from Path"),
                          [](core::text::TextBlock& b) { b.pathFit.reset(); });
    }

    // Type -> Create Work Path: the glyph outlines as a NEW vector layer beside the text, leaving
    // the text itself alone (that is the whole difference from Convert to Shape, which consumes
    // it). One AddLayerCommand.
    void typeCreateWorkPath() {
        core::TextLayer* tl = typeMenuTarget();
        if (tl == nullptr)
            return;
        std::size_t skipped = 0;
        core::vec::Path outlines = textBlockOutlines(*tl, &skipped);
        if (outlines.subpaths.empty()) {
            transientStatus(_("Create Work Path: this text has no outlines to convert"));
            return;
        }
        if (skipped > 0) {
            char buf[160];
            std::snprintf(buf, sizeof buf,
                          _("Create Work Path: %zu colour glyph(s) have no outline and were "
                            "left out"),
                          skipped);
            transientStatus(buf);
        }
        core::vec::Object obj;
        obj.geometry = std::move(outlines);
        obj.fill = core::vec::NoPaint{}; // a WORK path: geometry to edit, not a shape to look at
        std::unique_ptr<core::VectorLayer> vec = m_document->makeVector(_("Work Path"));
        vec->setObject(std::move(obj));
        vec->setTransform(tl->transform()); // the outlines are in the text layer's local space
        const core::LayerId newId = vec->id();
        core::LayerId parentId = m_document->root().id();
        std::size_t index = m_document->root().childCount();
        if (const std::optional<core::Document::Location> loc = m_document->locate(tl->id())) {
            parentId = loc->parent->id();
            index = loc->index + 1;
        }
        m_document->commands().push(
            std::make_unique<core::AddLayerCommand>(parentId, index, std::move(vec)));
        syncAfterEdit();
        if (m_layerPanel != nullptr)
            m_layerPanel->setActive(newId);
    }

    // Type -> Update All Text Layers: re-shape every block against the fonts as they are NOW.
    // The point is the font situation changing under a document -- a family installed, a
    // fontconfig cache rebuilt, a file opened on another machine -- after which the cached
    // layouts are stale and nothing else invalidates them. Not undoable: it changes no document
    // state, only derived caches.
    void updateAllTextLayers() {
        if (!m_document)
            return;
        std::size_t count = 0;
        const auto walk = [&](core::GroupLayer& g, const auto& self) -> void {
            for (const auto& child : g.children()) {
                if (auto* grp = child->as<core::GroupLayer>()) {
                    self(*grp, self);
                    continue;
                }
                if (auto* tl = child->as<core::TextLayer>(); tl != nullptr) {
                    tl->invalidateContentBounds();
                    ++count;
                }
            }
        };
        walk(m_document->root(), walk);
        if (count == 0) {
            transientStatus(_("Update All Text Layers: this document has no text"));
            return;
        }
        settleTextCaches(); // superseded by the full composite below, and correct without it
        requestRecomposite(/*fitView=*/false);
        if (m_layerPanel != nullptr)
            m_layerPanel->refreshThumbnails();
        char buf[128];
        std::snprintf(buf, sizeof buf, _("Re-shaped %zu text layer(s)"), count);
        transientStatus(buf);
    }

    // ---- Layer menu (S53-b) ---------------------------------------------------------------------

    void renameActiveLayer() {
        if (m_layerPanel == nullptr || m_layerPanel->activeLayer() == core::kInvalidLayerId)
            return;
        m_layerPanel->beginRename(m_layerPanel->activeLayer());
    }

    // Type ▸ Character && Paragraph… / 3D Type… open the SAME panels the options bar's buttons
    // toggle (kPanelStyle / kPanelType3d). Public wrappers because the menu callbacks are free
    // functions and the openers themselves live in the class's private half.
    void openTypePanelFromMenu() { openTypePanel(); }
    void openType3dPanelFromMenu() { openType3dPanel(); }

    // Type ▸ Rasterize Type / Convert to Shape: the Layer-menu commands aimed at the TYPE target
    // (the block being edited wins over the active row), refusing anything that is not text --
    // "Rasterize Type" must never quietly rasterize whatever shape layer happens to be selected.
    void rasterizeActiveTypeLayer() {
        if (core::TextLayer* tl = typeMenuTarget())
            rasterizeLayerCommand(tl->id());
    }
    void convertActiveTypeLayerToShape() {
        if (core::TextLayer* tl = typeMenuTarget())
            convertLayerToPathCommand(tl->id());
    }

    // Layer ▸ Rasterize / Convert to Path: the same two commands on the ACTIVE row, whatever kind
    // it is -- the layer-row context menu's behaviour, now reachable without a right-click.
    void rasterizeActiveLayer() {
        if (m_layerPanel != nullptr)
            rasterizeLayerCommand(m_layerPanel->activeLayer());
    }
    void convertActiveLayerToPath() {
        if (m_layerPanel != nullptr)
            convertLayerToPathCommand(m_layerPanel->activeLayer());
    }

    // Layer ▸ Warp ▸ Mesh / Perspective (S35-b). The menu row's whole job is to SELECT the tool: a
    // warp is a framed on-canvas gesture with its own Apply, not a one-shot command, so there is
    // nothing else for a menu item to do. onToolChanged -> selectLayerForActiveTool then binds the
    // active layer's lattice (restoring a stored grid when there is one) and names the refusal when
    // that layer cannot be warped -- so the row never silently does nothing.
    void activateWarpTool(bool perspective) {
        m_tools.setActive(perspective ? ToolId::PerspectiveWarp : ToolId::MeshWarp);
    }

    void toggleActiveLayerVisible() {
        if (m_layerPanel != nullptr && m_layerPanel->activeLayer() != core::kInvalidLayerId)
            m_layerPanel->toggleVisible(m_layerPanel->activeLayer());
    }
    void toggleActiveLayerLocked() {
        if (m_layerPanel != nullptr && m_layerPanel->activeLayer() != core::kInvalidLayerId)
            m_layerPanel->toggleLocked(m_layerPanel->activeLayer());
    }

    // The four mask rows. Each is the LayerPanel's own command (one undoable step, seeded from the
    // active selection where that applies); the menu only picks which one.
    enum class MaskAction { Add, Delete, ToggleEnabled, ToggleLinked };
    void layerMaskAction(MaskAction action) {
        if (m_layerPanel == nullptr)
            return;
        const core::LayerId id = m_layerPanel->activeLayer();
        if (id == core::kInvalidLayerId)
            return;
        switch (action) {
            case MaskAction::Add: m_layerPanel->addMaskTo(id); break;
            case MaskAction::Delete: m_layerPanel->deleteMask(id); break;
            case MaskAction::ToggleEnabled: m_layerPanel->toggleMaskEnabled(id); break;
            case MaskAction::ToggleLinked: m_layerPanel->toggleMaskLinked(id); break;
        }
    }

    // Bring Forward / Send Backward: reorder within the layer's OWN parent (never a reparent).
    // Children are stored bottom -> top, so "forward" is +1 in the index. MoveLayerCommand's
    // index is the position AFTER the layer has been lifted out, which for a same-parent move
    // means the target index is simply the neighbour's.
    void reorderActiveLayer(int delta) {
        if (!m_document || m_layerPanel == nullptr)
            return;
        const core::LayerId id = m_layerPanel->activeLayer();
        const std::optional<core::Document::Location> loc = m_document->locate(id);
        if (!loc)
            return;
        const std::size_t count = loc->parent->childCount();
        if (delta > 0 && loc->index + 1 >= count) {
            transientStatus(_("This layer is already at the top of its group"));
            return;
        }
        if (delta < 0 && loc->index == 0) {
            transientStatus(_("This layer is already at the bottom of its group"));
            return;
        }
        const std::size_t target = delta > 0 ? loc->index + 1 : loc->index - 1;
        m_document->commands().push(
            std::make_unique<core::MoveLayerCommand>(id, loc->parent->id(), target));
        syncAfterEdit();
        m_layerPanel->setActive(id);
    }

    // Layer -> Flatten to Path: bake a LIVE boolean compound (or a parametric shape) down to an
    // editable polyline path IN PLACE. The difference from Convert to Path is the layer identity:
    // this keeps the LayerId, so the panel selection and any Move-tool multi-selection survive.
    void flattenToPath() {
        if (!m_document)
            return;
        core::Layer* l = activeLayerPtr();
        auto* vl = l != nullptr ? l->as<core::VectorLayer>() : nullptr;
        if (vl == nullptr || !vl->hasObject()) {
            transientStatus(_("Flatten to Path needs a shape layer"));
            return;
        }
        if (vl->locked()) {
            transientStatus(_("Flatten to Path: this layer is locked"));
            return;
        }
        core::vec::Object next = *vl->object();
        next.geometry = core::vec::pathFromGeometry(next.geometry);
        if (next == *vl->object()) {
            transientStatus(_("Flatten to Path: this shape is already a plain path"));
            return;
        }
        m_document->commands().push(std::make_unique<core::SetVectorObjectCommand>(
            vl->id(), std::move(next), _("Flatten to Path"), /*coalesceId=*/0));
        syncAfterEdit();
    }

    // Combine Paths' operands: every VECTOR layer in the Move tool's multi-selection, in stack
    // order (bottom first). The panel mirrors that set and answers per id, so the walk is over the
    // document. Top-level only -- the same reading of the group model the marquee applies, where
    // a group is one object rather than a bag of leaves.
    [[nodiscard]] std::vector<core::VectorLayer*> combinePathOperands() {
        std::vector<core::VectorLayer*> out;
        if (!m_document || m_layerPanel == nullptr || !m_layerPanel->multiSelectActive())
            return out;
        core::GroupLayer& root = m_document->root();
        for (std::size_t i = 0; i < root.childCount(); ++i) {
            core::Layer& child = root.child(i);
            if (!m_layerPanel->isInMoveSelection(child.id()))
                continue;
            if (auto* vl = child.as<core::VectorLayer>(); vl != nullptr && vl->hasObject())
                out.push_back(vl);
        }
        return out;
    }

    [[nodiscard]] static const char* boolOpLabel(core::vec::BoolOp op) {
        switch (op) {
            case core::vec::BoolOp::Union: return _("Add Paths");
            case core::vec::BoolOp::Subtract: return _("Subtract Paths");
            case core::vec::BoolOp::Intersect: return _("Intersect Paths");
            case core::vec::BoolOp::Exclude: return _("Exclude Paths");
        }
        return _("Combine Paths");
    }

    // Layer -> Combine Paths -> {Add, Subtract, Intersect, Exclude}, as ONE undo step.
    void combinePaths(core::vec::BoolOp op) {
        if (!m_document)
            return;
        const std::vector<core::VectorLayer*> operands = combinePathOperands();
        if (operands.size() < 2) {
            transientStatus(_("Combine Paths needs two or more shape layers selected"));
            return;
        }
        for (const core::VectorLayer* vl : operands) {
            if (vl->locked()) {
                transientStatus(_("Combine Paths: a locked layer cannot be combined"));
                return;
            }
        }
        // The HOST is the BOTTOM-most selected shape -- Merge Down's rule, so the result lands
        // where the stack already put the lower shape. It must also be operands.front(), because
        // the kernel gives the result the FIRST operand's fill/stroke/paint order (the
        // Illustrator/Figma "the host's appearance wins" rule).
        core::VectorLayer* host = operands.front();
        const common::Affine2D hostWorld = core::worldTransform(*host);
        std::vector<std::pair<core::vec::Object, common::Affine2D>> world;
        world.reserve(operands.size());
        for (const core::VectorLayer* vl : operands)
            world.emplace_back(*vl->object(), core::worldTransform(*vl));
        std::optional<core::vec::Object> combined =
            core::vec::makeBooleanObject(op, world, hostWorld);
        if (!combined) {
            transientStatus(_("Combine Paths: these shapes cannot be combined"));
            return;
        }
        const char* label = boolOpLabel(op);
        auto composite = std::make_unique<core::CompositeCommand>(std::string(label));
        // ORDER MATTERS. The host is rewritten FIRST (SetVectorObjectCommand, not
        // ReplaceLayerCommand -- the latter mints a fresh LayerId and would drop the panel
        // selection), then the consumed operands are removed in DESCENDING index order:
        // RemoveLayerCommand captures its parent + index on apply, and CompositeCommand::undo runs
        // the children in reverse, so each re-insertion lands in a tree that still holds every
        // later sibling exactly where that capture saw it.
        composite->add(std::make_unique<core::SetVectorObjectCommand>(
            host->id(), std::move(*combined), std::string(label), /*coalesceId=*/0));
        for (std::size_t k = operands.size(); k-- > 1;)
            composite->add(std::make_unique<core::RemoveLayerCommand>(operands[k]->id()));
        const core::LayerId hostId = host->id();
        m_document->commands().push(std::move(composite));
        syncAfterEdit();
        if (m_layerPanel != nullptr) {
            m_layerPanel->setMoveSelection({hostId}); // the operands are gone; the host remains
            m_layerPanel->setActive(hostId);
        }
    }

    // ---- Select menu (S53-b) --------------------------------------------------------------------

    // Reselect: put back the selection Deselect threw away. deselect() is the only thing that
    // stashes one, which is exactly Photoshop's rule -- Reselect restores what you deselected, not
    // "the previous selection" in general (undo already covers that).
    void reselect() {
        if (!m_document)
            return;
        if (m_lastDeselected.isEmpty() || !m_lastDeselected.anySelected()) {
            transientStatus(_("Reselect: there is no previous selection to restore"));
            return;
        }
        // The stash is app-global (one register, like the export target), so it can outlive a tab
        // switch. A mask sized for another canvas is not this document's selection -- refuse it
        // rather than landing a mismatched coverage plane.
        if (m_lastDeselected.width() != m_document->width() ||
            m_lastDeselected.height() != m_document->height()) {
            transientStatus(_("Reselect: that selection belongs to a differently-sized canvas"));
            return;
        }
        m_document->commands().push(
            std::make_unique<core::SetSelectionCommand>(m_lastDeselected));
        syncSelection();
    }

    // Select All Layers: every visible TOP-LEVEL unit, gathered exactly the way a full-canvas
    // marquee band would gather it (core::layersInMarquee -- a group counts as one object).
    //
    // BOTH surfaces are fed, because the two menus that consume a layer selection read different
    // ones: Combine Paths and the row highlight read the LAYERS PANEL's multi-selection, while the
    // Arrange menu reads the CANVAS's move targets. Feeding only the panel made this half a
    // feature -- Arrange stayed empty until the user happened to click the canvas.
    void selectAllLayers() {
        if (!m_document || m_layerPanel == nullptr)
            return;
        const common::Rect whole{0.0, 0.0, static_cast<double>(m_document->width()),
                                 static_cast<double>(m_document->height())};
        std::vector<core::LayerId> ids = core::layersInMarquee(m_document->root(), whole);
        if (ids.empty()) {
            transientStatus(_("Select All Layers: nothing visible to select"));
            return;
        }
        const core::LayerId top = ids.back();
        if (m_canvas)
            m_canvas->setMoveTargets(ids);
        m_layerPanel->setMoveSelection(std::move(ids));
        m_layerPanel->setActive(top);
    }

    // ---- Edit menu additions (S53-b) ------------------------------------------------------------

    // Edit -> Clear: Cut's destructive half without touching the clipboard.
    void clearSelection() {
        if (!m_document)
            return;
        core::Layer* layer = activeLayerPtr();
        if (layer == nullptr)
            return;
        std::optional<common::Image> cleared =
            core::imageWithSelectionCleared(*layer, m_document->selection());
        if (!cleared) {
            transientStatus(_("Nothing to clear: the selection covers no editable pixels"));
            return;
        }
        m_document->commands().push(
            std::make_unique<core::SetLayerPixelsCommand>(layer->id(), std::move(*cleared)));
        syncAfterEdit();
    }

    // Edit -> Paste in Place: the IN-APP clipboard at the coordinates it was lifted from. Plain
    // Paste prefers the OS clipboard (so it speaks to other applications), and content the OS
    // hands back that we cannot recognise as ours is CENTRED -- which is the behaviour this item
    // exists to bypass.
    void pasteInPlace() {
        if (!m_document)
            return;
        if (!m_clipboard) {
            transientStatus(_("Paste in Place: nothing was copied inside Mosaic"));
            return;
        }
        pasteContent(*m_clipboard, /*atSource=*/true);
    }

    // How the four Select-menu mask items fold the current selection into the active layer's mask.
    // They share every guard and the resampling (maskFromSelectionEntry) and differ only in this
    // final combine (the coverage math lives in the method below).
    enum class MaskCombine {
        Replace,   // the mask BECOMES the selection's coverage (the original "Mask from Selection")
        Inverse,   // Replace, built from the selection's complement (Selection::inverted())
        Add,       // union into the current mask: coverage = max(existing, selection)
        Subtract,  // carve out: coverage = existing * (1 - selection); needs a mask to carve from
    };

    // The mode's undo-command name AND its status-bar prefix, as raw English (N_ marks it for i18n
    // extraction without translating now). The command name matches the sibling mask commands
    // ("Add Mask"/"Delete Mask"); the status prefix is translated at the call site with _().
    static const char* maskCombineLabel(MaskCombine mode) {
        switch (mode) {
        case MaskCombine::Replace:  return N_("Mask from Selection");
        case MaskCombine::Inverse:  return N_("Mask from Inverse Selection");
        case MaskCombine::Add:      return N_("Add to Mask");
        case MaskCombine::Subtract: return N_("Subtract from Mask");
        }
        return N_("Mask from Selection");
    }

    // Select -> Mask from Selection (S31) and its three siblings: one SetLayerMaskCommand putting a
    // raster mask on the active layer built from the selection, resampled onto the layer's mask
    // grid (core::maskFromSelection). Refusals are narrated in the status bar, Merge-Down style --
    // the item stays enabled, the message explains.
    //
    // `mode` (MaskCombine, above) picks how the selection meets the layer's EXISTING mask:
    //   Replace / Inverse - the mask becomes the selection's (or its complement's) coverage; the
    //                       inverting happens on the Selection so every gate + the resampling are
    //                       shared verbatim, only the source coverage differs.
    //   Add               - union it in: new = max(existing, selection). With no mask this is just
    //                       a create-from-selection (max over nothing = the selection itself).
    //   Subtract          - carve it out: new = existing * (1 - selection) = clamp(existing -
    //                       selection). A narrated no-op on a maskless layer (nothing to subtract).
    void maskFromSelectionEntry(MaskCombine mode = MaskCombine::Replace) {
        if (!m_document)
            return;
        const std::string op = _(maskCombineLabel(mode)); // translated prefix for the refusals
        if (m_document->selection().isEmpty()) {
            transientStatus(op + _(": make a selection first"));
            return;
        }
        core::Layer* layer =
            m_layerPanel ? m_document->find(m_layerPanel->activeLayer()) : nullptr;
        if (layer == nullptr) {
            transientStatus(op + _(": no active layer"));
            return;
        }
        if (layer->locked()) {
            transientStatus(_("This layer is locked. Unlock it to add a mask."));
            return;
        }
        if (layer->kind() == core::LayerKind::Text) {
            // A text layer's pixels live in a renderer-filled cache whose grid moves with the
            // text; a raster mask glued to it would swim. Honest refusal over broken behavior.
            transientStatus(op + _(": not available on text layers yet"));
            return;
        }
        const bool hadMask = layer->hasMask();
        // Subtract needs an existing mask to carve from; with none there is nothing to remove, so
        // it is a narrated no-op (the item stays enabled, like every other refusal here).
        if (mode == MaskCombine::Subtract && !hadMask) {
            transientStatus(op + _(": this layer has no mask"));
            return;
        }

        // The selection resampled onto the layer's mask grid -- the coverage every mode starts
        // from. Inverse builds it from the selection's complement; the rest from the selection.
        const core::Selection source = mode == MaskCombine::Inverse
                                           ? m_document->selection().inverted()
                                           : m_document->selection();
        core::RasterMask mask =
            core::maskFromSelection(*layer, source, m_document->width(), m_document->height());

        // Add / Subtract fold that coverage into the mask already on the layer. Both grids come
        // from maskFromSelection's maskDimsFor for the SAME layer at the SAME doc size, so they
        // match (the size guard is belt-and-braces); the existing mask's enabled/linked flags carry
        // over so a disabled or unlinked mask stays that way.
        if (hadMask && (mode == MaskCombine::Add || mode == MaskCombine::Subtract)) {
            const core::RasterMask& existing = *layer->mask();
            if (existing.width == mask.width && existing.height == mask.height &&
                existing.coverage.size() == mask.coverage.size()) {
                for (std::size_t i = 0; i < mask.coverage.size(); ++i) {
                    const int e = existing.coverage[i];
                    const int s = mask.coverage[i];
                    // Add: union (max). Subtract: existing * (1 - selection), i.e.
                    // e * (255 - s) / 255 rounded to nearest -- clamp(existing - selection).
                    mask.coverage[i] = static_cast<std::uint8_t>(
                        mode == MaskCombine::Add ? (e > s ? e : s)
                                                 : (e * (255 - s) + 127) / 255);
                }
            }
            mask.enabled = existing.enabled;
            mask.linked = existing.linked;
        }

        m_document->commands().push(std::make_unique<core::SetLayerMaskCommand>(
            layer->id(), std::move(mask), maskCombineLabel(mode)));
        syncAfterEdit();
        switch (mode) {
        case MaskCombine::Replace:
            transientStatus(hadMask ? _("Mask replaced from the selection")
                                    : _("Mask added from the selection"));
            break;
        case MaskCombine::Inverse:
            transientStatus(hadMask ? _("Mask replaced from the inverse selection")
                                    : _("Mask added from the inverse selection"));
            break;
        case MaskCombine::Add:
            transientStatus(hadMask ? _("Selection added to the mask")
                                    : _("Mask added from the selection"));
            break;
        case MaskCombine::Subtract:
            transientStatus(_("Selection subtracted from the mask"));
            break;
        }
    }

    // Edit menu clipboard (S14-b). Copy fills the in-app clipboard (pixels + source position +
    // alpha) AND the OS clipboard (flattened over white via Fl_Copy_Surface -- cross-app
    // clipboards are RGB). Paste asks the OS first, recognising our own copy when it
    // round-trips back (so in-app pastes keep alpha + land at the source position).
    void copySelection(bool merged) {
        if (!m_document)
            return;
        std::optional<core::ClipboardContent> content;
        if (merged) {
            render::CompositeOptions opts;
            opts.checkerboard = false; // the flatten must carry real alpha, not the display bg
            const render::CompositeResult flat =
                render::composite(*m_document, opts, render::Backend::Cpu);
            if (!flat.ok) {
                uiLog().warn("copy merged: composite failed: {}", flat.error);
                return;
            }
            content = core::copyMerged(flat.image, m_document->selection());
            if (content)
                content->sourceName = _("merged"); // pastes as "Selection from merged"
        } else {
            const core::Layer* layer = activeLayerPtr();
            if (layer == nullptr)
                return;
            content = core::copyFromLayer(*layer, m_document->selection(), m_document->width(),
                                          m_document->height());
        }
        if (!content) {
            // Without feedback this reads as "copied nothing but a later paste worked" (the
            // clipboard still holds the previous copy).
            transientStatus(_("Nothing to copy: the selection covers no pixels"));
            return;
        }
        pushToOsClipboard(content->image);
        // Say WHAT was taken: the pixel selection wins over the Move-tool target (industry
        // semantics, kept by decision 2026-06-11) — invisible state without this line.
        char buf[256];
        if (content->style)
            std::snprintf(buf, sizeof buf, _("Copied layer '%s'"), content->style->name.c_str());
        else
            std::snprintf(buf, sizeof buf, _("Copied selection from %s"),
                          content->sourceName.c_str());
        transientStatus(buf);
        m_clipboard = std::move(*content);
    }

    void cutSelection() {
        if (!m_document)
            return;
        core::Layer* layer = activeLayerPtr();
        if (layer == nullptr)
            return;
        std::optional<core::ClipboardContent> content = core::copyFromLayer(
            *layer, m_document->selection(), m_document->width(), m_document->height());
        if (!content) {
            transientStatus(_("Nothing to cut: the selection covers no pixels"));
            return;
        }
        std::optional<common::Image> cleared =
            core::imageWithSelectionCleared(*layer, m_document->selection());
        pushToOsClipboard(content->image);
        char buf[256];
        if (content->style)
            std::snprintf(buf, sizeof buf, _("Cut layer '%s'"), content->style->name.c_str());
        else
            std::snprintf(buf, sizeof buf, _("Cut selection from %s"), content->sourceName.c_str());
        transientStatus(buf);
        m_clipboard = std::move(*content);
        if (cleared) { // raster layers only; the copy half still worked for e.g. magic layers
            const bool partitionable =
                !m_document->selection().isEmpty() && core::partitionEligibleSource(*layer);
            m_document->commands().push(
                std::make_unique<core::SetLayerPixelsCommand>(layer->id(), std::move(*cleared)));
            syncAfterEdit();
            // Record what the fragment was lifted OFF, so pasting it back in place can re-link the
            // two halves as a coverage partition and the compositor recombines them disjointly
            // instead of leaving the `over` seam along the feathered (or merely AA'd) edge. The
            // revision is read AFTER the erase: that is the residual the fragment complements.
            if (partitionable)
                m_clipboard->lift =
                    core::ClipboardContent::Lift{layer->id(), layer->contentRevision()};
        }
    }

    void pasteClipboard() {
        if (!m_document)
            return;
        if (Fl::clipboard_contains(Fl::clipboard_image) != 0) {
            Fl::paste(*this, 1, Fl::clipboard_image); // async: arrives as FL_PASTE below
            return;
        }
        if (m_clipboard) // owners that publish no image type (or an empty OS clipboard)
            pasteContent(*m_clipboard, /*atSource=*/true);
    }

    // The FL_PASTE delivery: decide whether the OS image is our own copy round-tripping back
    // (same size and identical once flattened over white -- the exact form we exported) and
    // paste with the richer in-app content when it is.
    void onClipboardImage(Fl_RGB_Image* img) {
        if (!m_document || img == nullptr || img->w() <= 0 || img->h() <= 0)
            return;
        common::Image rgba = fromFlImage(*img);
        if (rgba.empty())
            return;
        if (m_clipboard && rgba.width == m_clipboard->image.width &&
            rgba.height == m_clipboard->image.height &&
            core::flattenedOverWhite(rgba).rgba ==
                core::flattenedOverWhite(m_clipboard->image).rgba) {
            pasteContent(*m_clipboard, /*atSource=*/true);
            return;
        }
        pasteContent(core::ClipboardContent{std::move(rgba), 0, 0}, /*atSource=*/false);
    }

    void pasteContent(core::ClipboardContent content, bool atSource) {
        if (!m_document || content.image.empty())
            return;
        const auto [px, py] = core::pastePosition(
            content.image.width, content.image.height,
            atSource ? std::optional(std::pair{content.docX, content.docY}) : std::nullopt,
            m_document->width(), m_document->height());
        // The paste-semantics rules (2026-06-11): a whole-layer copy pastes like
        // Layer→Duplicate (source name verbatim + opacity/blend restored); raw pixel data
        // pastes as "Selection from <source>" ("Pasted image" for external content) wearing
        // the pasted marker until the user renames it.
        std::string name;
        if (content.style) {
            name = content.style->name;
        } else if (content.sourceName.empty()) {
            name = _("Pasted image");
        } else {
            char buf[256];
            std::snprintf(buf, sizeof buf, _("Selection from %s"), content.sourceName.c_str());
            name = buf;
        }
        std::unique_ptr<core::RasterLayer> layer =
            m_document->makeRaster(std::move(name), content.image.width, content.image.height);
        layer->image() = std::move(content.image);
        if (content.style) {
            layer->setOpacity(content.style->opacity);
            layer->setBlendMode(content.style->blend);
        } else {
            layer->setPastedMarker(true);
        }
        if (px != 0 || py != 0)
            layer->setTransform(common::Affine2D::translation(px, py));
        const core::LayerId newId = layer->id();
        // Insert just above the active layer (Photoshop's rule), or on top of the root stack.
        core::LayerId parentId = m_document->root().id();
        std::size_t index = m_document->root().childCount();
        if (m_layerPanel != nullptr) {
            if (const std::optional<core::Document::Location> loc =
                    m_document->locate(m_layerPanel->activeLayer())) {
                parentId = loc->parent->id();
                index = loc->index + 1;
            }
        }
        m_document->commands().push(
            std::make_unique<core::AddLayerCommand>(parentId, index, std::move(layer)));
        syncAfterEdit();
        if (atSource && content.lift)
            linkPastedPartition(*content.lift, newId);
        if (m_layerPanel)
            m_layerPanel->setActive(newId); // refresh() ran via syncAfterEdit
    }

    // Cut + Paste in place: re-link the pasted fragment to the residual it was lifted off, so the
    // compositor recombines the two disjointly (core::CoveragePartition) and the cut edge shows no
    // seam. Every precondition is re-checked here rather than trusted from cut time — the clipboard
    // survives arbitrary edits, other documents and other sessions, and the link only means
    // anything if the residual is still sitting untouched directly beneath the fragment.
    void linkPastedPartition(const core::ClipboardContent::Lift& lift, core::LayerId pastedId) {
        core::Layer* residual = m_document->find(lift.sourceLayer);
        core::Layer* fragment = m_document->find(pastedId);
        if (residual == nullptr || fragment == nullptr)
            return;
        if (residual->contentRevision() != lift.sourceRevision)
            return; // the hole was repainted / undone since the cut: no longer complementary
        const std::optional<core::Document::Location> rl = m_document->locate(lift.sourceLayer);
        const std::optional<core::Document::Location> fl = m_document->locate(pastedId);
        if (!rl || !fl || rl->parent != fl->parent || fl->index != rl->index + 1)
            return; // the fragment must sit directly on top of its own hole
        core::linkCoveragePartition(*residual, *fragment);
        requestRecomposite(/*fitView=*/false);
    }

    // S26-c shape authoring: the DRAG is a wireframe the canvas draws on the overlay and nothing
    // else -- the document is untouched until the pointer comes up. spawnShape() then creates the
    // real, FILLED VectorLayer from the final draft and commitShape() immediately bakes it into one
    // undoable command; cancelShape() drops a spawn that never got committed. The layer is inserted
    // OUTSIDE the command stack (a direct insert) so the spawn+commit pair is a single undo step.
    void spawnShape(const ui::ShapeDraft& draft) {
        if (!m_document)
            return;
        core::VectorLayer* vl = nullptr;
        if (m_shapePreviewLayer != core::kInvalidLayerId) {
            core::Layer* l = m_document->find(m_shapePreviewLayer);
            vl = l != nullptr ? l->as<core::VectorLayer>() : nullptr;
        }
        if (vl == nullptr) { // the usual path: create + directly insert the shape's layer
            std::unique_ptr<core::VectorLayer> layer = m_document->makeVector(_("Shape"));
            m_shapePreviewLayer = layer->id();
            core::GroupLayer* parent = &m_document->root();
            std::size_t index = parent->childCount();
            if (m_layerPanel != nullptr) {
                if (const std::optional<core::Document::Location> loc =
                        m_document->locate(m_layerPanel->activeLayer())) {
                    if (auto* g = loc->parent->as<core::GroupLayer>()) {
                        parent = g;
                        index = loc->index + 1;
                    }
                }
            }
            vl = static_cast<core::VectorLayer*>(&parent->insert(index, std::move(layer)));
            if (m_layerPanel != nullptr) { // the new row shows + becomes active while you draw
                m_layerPanel->refresh();
                m_layerPanel->setActive(m_shapePreviewLayer);
            }
        }
        vl->setObject(draft.object);
        vl->setTransform(draft.placement);
        requestRecomposite(/*fitView=*/false);
    }

    void commitShape(const std::string& name) {
        if (!m_document || m_shapePreviewLayer == core::kInvalidLayerId)
            return;
        const std::optional<core::Document::Location> loc = m_document->locate(m_shapePreviewLayer);
        const core::LayerId id = m_shapePreviewLayer;
        m_shapePreviewLayer = core::kInvalidLayerId;
        if (!loc)
            return;
        // Detach the just-spawned layer and re-add it THROUGH a command, so the whole gesture is
        // one undo step.
        std::unique_ptr<core::Layer> layer = loc->parent->removeAt(loc->index);
        layer->setName(name);
        auto cmd = std::make_unique<core::CompositeCommand>(std::string(_("Add ")) + name);
        cmd->add(std::make_unique<core::AddLayerCommand>(loc->parent->id(), loc->index,
                                                         std::move(layer)));
        m_document->commands().push(std::move(cmd));
        syncAfterEdit();
        if (m_layerPanel != nullptr)
            m_layerPanel->setActive(id);
    }

    // --- S29-c: the Type context bar <-> selection wiring (the Type twin of shape select-to-edit)
    // --- The Text tool's option named `id` (mutable), or null. The bar's "hot" type controls
    // (font, size, bold, italic, align, aa) live as ToolOptions so the generic options bar renders
    // + persists them.
    [[nodiscard]] ui::ToolOption* textOpt(const char* id) {
        ui::Tool* t = m_tools.find(ToolId::Text);
        if (t == nullptr)
            return nullptr;
        for (ui::ToolOption& o : t->options())
            if (o.id == id)
                return &o;
        return nullptr;
    }
    [[nodiscard]] double textOptValue(const char* id, double fallback) {
        const ui::ToolOption* o = textOpt(id);
        return o != nullptr ? o->value : fallback;
    }

    // S29-b Type tool: the Size option's current value (the create/typing em size, §7).
    [[nodiscard]] double textToolSize() { return textOptValue("size", 48.0); }

    // The "font" choice's currently-selected family name (its label), or the OS default.
    [[nodiscard]] std::string textToolFontFamily() {
        const ui::ToolOption* o = textOpt("font");
        if (o != nullptr) {
            const auto i = static_cast<std::size_t>(std::max(0, static_cast<int>(o->value)));
            if (i < o->choices.size())
                return o->choices[i];
        }
        return m_fontDb.defaultFamily();
    }

    static common::ColorF colorToF(common::Color8 c) {
        return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
    }
    static common::Color8 colorTo8(common::ColorF c) {
        const auto q = [](float v) {
            return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        };
        return {q(c.r), q(c.g), q(c.b), q(c.a)};
    }

    // The CharStyle a freshly created/typed run uses, from the bar's hot controls + the fg swatch
    // (§7). Shared by the Type host's defaultStyle and the live-recolour path so they never
    // diverge.
    [[nodiscard]] core::text::CharStyle textDefaultStyle() {
        core::text::CharStyle st;
        st.sizePx = static_cast<float>(textToolSize());
        st.font.family = textToolFontFamily();
        st.font.weight = textOptValue("bold", 0.0) != 0.0 ? 700.0f : 400.0f;
        st.font.italic = textOptValue("italic", 0.0) != 0.0;
        st.underline = textOptValue("underline", 0.0) != 0.0;
        st.strikethrough = textOptValue("strikethrough", 0.0) != 0.0;
        st.setSolidFill(colorToF(m_colors.foreground()));
        return st;
    }

    // The font-picker preview cell for `family` at `w`x`h` px: the family's coverage-aware sample
    // rendered in its own face (S29-c §8), through the SAME FreeType/HarfBuzz renderer the canvas
    // uses
    // -- so emoji/CJK/RTL preview correctly. Lazily rendered + cached (only visible rows ask),
    // keyed by family + size + theme; the Fl_RGB_Image references the cached pixel buffer, so both
    // live together.
    [[nodiscard]] Fl_RGB_Image* fontPreview(const std::string& family, int w, int h) {
        if (w <= 0 || h <= 0)
            return nullptr;
        const Palette& pal = activePalette();
        std::string key =
            family + '|' + std::to_string(w) + 'x' + std::to_string(h) + (pal.dark ? "|d" : "|l");
        const auto it = m_fontPreviewCache.find(key);
        if (it != m_fontPreviewCache.end())
            return it->second.img.get();
        const float sizePx = std::min(static_cast<float>(h) * 0.62f, 22.0f);
        FontPreview entry;
        entry.pixels = core::text::renderFontSample(m_textShaper, m_fontDb, family,
                                                    m_fontDb.sampleTextFor(family), sizePx,
                                                    colorToF(pal.text), w, h, /*rightAlign=*/true);
        entry.img = std::make_unique<Fl_RGB_Image>(entry.pixels.rgba.data(), w, h, 4);
        Fl_RGB_Image* raw = entry.img.get();
        m_fontPreviewCache.emplace(std::move(key), std::move(entry));
        return raw;
    }

    [[nodiscard]] ui::ToolOption* toolOpt(ToolId tool, const char* id) {
        ui::Tool* t = m_tools.find(tool);
        if (t == nullptr)
            return nullptr;
        for (ui::ToolOption& o : t->options())
            if (o.id == id)
                return &o;
        return nullptr;
    }
    [[nodiscard]] ui::ToolOption* brushOpt(const char* id) { return toolOpt(ToolId::Brush, id); }
    [[nodiscard]] ui::ToolOption* eraserOpt(const char* id) { return toolOpt(ToolId::Eraser, id); }

    // Scan the preset library and hand it to the dock's preset grid (§8.2). A scan that finds nothing
    // (no data dir, an unreadable bundle) leaves the Brush exactly as it was: on the engine's own
    // analytic circle, which is what it painted before presets existed.
    //
    // Nothing is DECODED here. The grid pulls each preset's embedded thumbnail lazily, for the cells
    // it actually shows -- 117 icons up front would be ~19 MB and a stalled startup.
    void initBrushPresets() {
        const int n = m_brushPresets.scan();
        const io::brush::LibraryCounters& c = m_brushPresets.library().counters();
        common::log::category("brush")->info(
            "presets: {} loaded, {} failed; tips: {} by md5, {} by name, {} fallback", n,
            c.presetsFailed, c.tipsResolvedByMd5, c.tipsResolvedByFilename, c.tipsFallback);
        if (m_dock != nullptr)
            m_dock->presets()->setStore(&m_brushPresets);
    }

    // Restore the persisted preset, BY NAME (Settings::brushPreset): the library's order is a
    // directory scan, so an index would silently point at a different brush the moment a bundle is
    // added or renamed. A name that no longer resolves selects nothing -- i.e. the round tip, which
    // is exactly what a user who has never picked a preset already has.
    void restorePreset(ui::PresetCorpus corpus, const std::string& name) {
        if (name.empty())
            return;
        const int index = m_brushPresets.indexOfName(name);
        if (index < 0) {
            common::log::category("brush")->info("saved preset '{}' is no longer installed", name);
            return;
        }
        applyPresetPick(corpus, index, /*persist=*/false); // it came FROM the file; don't rewrite it
    }

    // A cell was picked (or a preset was restored at startup): resolve it ONCE and seed the bar's two
    // live sliders from it. Seeding rather than overriding is the point -- Size and Opacity stay the
    // user's to steer, and the preset is merely where they start.
    //
    // ⚠ select() is the one place a preset is resolved. It mints the tip's raster id, and a fresh id
    // per stroke would be a permanently cold dab cache -- which is why the canvas copies from
    // activeParams() and never resolves anything itself.
    // A pick from the dock's grid, into ONE of the two preset slots (§8.4). Which slot is decided by
    // the CORPUS the grid was showing -- not by the active tool -- because the corpus is what the
    // user was actually looking at when they clicked, and it is the thing that cannot be out of step
    // with the cell they hit. The two corpora never overlap: the Brush is offered no eraser and the
    // Eraser nothing else. A single shared slot would mean reaching for the Eraser silently changed
    // which brush you were painting with, and reaching back changed it again.
    void applyPresetPick(ui::PresetCorpus corpus, int index, bool persist = true) {
        const bool eraser = corpus == ui::PresetCorpus::Eraser;
        if (index == (eraser ? m_brushPresets.activeEraserIndex() : m_brushPresets.activeIndex()))
            return;
        if (!(eraser ? m_brushPresets.selectEraser(index) : m_brushPresets.select(index)))
            return;
        // ... only when the panel is SHOWING that corpus, or restoring the eraser's preset at startup
        // would move the Brush grid's selection to a cell it does not even contain.
        if (m_dock != nullptr && m_dock->presets() != nullptr &&
            m_dock->presets()->corpus() == corpus)
            m_dock->presets()->setSelected(index); // no-op when the pick came FROM the panel
        if (persist)
            persistBrushPreset();
        const core::brush::BrushParams* p =
            eraser ? m_brushPresets.activeEraserParams() : m_brushPresets.activeParams();
        if (p == nullptr)
            return; // "Default round": no preset, so nothing to seed from -- the bar keeps its values
        // Each tool's bar carries its own Size and Opacity; seed the one the preset landed on.
        // (The Eraser has no Flow -- see currentBrushParams.)
        if (ui::ToolOption* size = eraser ? eraserOpt("size") : brushOpt("size"))
            size->value = std::clamp(p->diameter, size->min, size->max);
        if (ui::ToolOption* op = eraser ? eraserOpt("opacity") : brushOpt("opacity"))
            op->value = std::clamp(p->opacity * 100.0, op->min, op->max);
        m_tools.notifyOptionsChanged(); // the bar, the eraser's size tie, and any twin surface
    }

    // Populate the "font" choice with the real installed families (the tool registry is built
    // before the FontDB, so the option ships a placeholder; this swaps in the live list), selecting
    // the OS default. Called once at startup; the bar reads the choices on its next rebuild.
    void initTextToolFonts() {
        ui::ToolOption* o = textOpt("font");
        if (o == nullptr)
            return;
        std::vector<std::string> fams = m_fontDb.families();
        if (fams.empty())
            return;
        const std::string def = m_fontDb.defaultFamily();
        const auto it = std::find(fams.begin(), fams.end(), def);
        o->value = it != fams.end() ? static_cast<double>(std::distance(fams.begin(), it)) : 0.0;
        o->choices = std::move(fams);
    }

    // Read-back: push the CURRENT selection's common style into the bar's controls (model -> bar),
    // then re-sync the bar. Guarded so the resulting notifyOptionsChanged isn't taken as a user
    // edit. No-op without an active session, so leaving editing keeps the bar as the new-text
    // authoring set.
    void reflectTextOptions() {
        if (m_canvas == nullptr || m_canvas->textEditTarget() == core::kInvalidLayerId)
            return;
        const core::text::CommonStyle cs = m_canvas->selectionStyle();
        const core::text::CommonParagraph cp = m_canvas->selectionParagraph();
        if (ui::ToolOption* o = textOpt("font")) {
            const auto it = std::find(o->choices.begin(), o->choices.end(), cs.style.font.family);
            if (it !=
                o->choices.end()) // leave the prior index for a family not in the list (mixed)
                o->value = static_cast<double>(std::distance(o->choices.begin(), it));
        }
        if (ui::ToolOption* o = textOpt("size"))
            o->value = cs.style.sizePx;
        // B/I/U/S glyph toggles: reflect the selection's common style (a mixed field shows its
        // representative value, matching the panel's toggles). Align lives in the Type panel.
        if (ui::ToolOption* o = textOpt("bold"))
            o->value = cs.style.font.weight >= 600.0f ? 1.0 : 0.0;
        if (ui::ToolOption* o = textOpt("italic"))
            o->value = cs.style.font.italic ? 1.0 : 0.0;
        if (ui::ToolOption* o = textOpt("underline"))
            o->value = cs.style.underline ? 1.0 : 0.0;
        if (ui::ToolOption* o = textOpt("strikethrough"))
            o->value = cs.style.strikethrough ? 1.0 : 0.0;
        // Writing mode + orientation + anti-alias are Type-panel controls (block-level), reflected
        // via reflect() below (aa moved off the bar in R4).
        captureTextBarSnapshot();
        // Only a real caret/selection MOVE starts a fresh undo group; a content-only change (our
        // own style edit bumping the block revision, at the same range) keeps the drag coalescing.
        const core::text::TextSelection sel = m_canvas->textSelection();
        if (sel != m_lastReflectedTextSel) {
            m_lastTextEditOptionId.clear();
            m_lastReflectedTextSel = sel;
        }
        m_reflectingTextOptions = true; // the resulting sync is our own write, not a user edit
        m_tools.notifyOptionsChanged();
        m_reflectingTextOptions = false;
        // The Type panel is the bar's lockstep twin: push the same common style/paragraph into it
        // (its own controls cover the fields the bar doesn't). reflect() is guarded + a no-op while
        // the panel is hidden, so this is cheap when the panel isn't open.
        if (m_typePanel != nullptr && m_typePanel->shown()) {
            const core::text::TextBlock* b = m_canvas->textEditBlockForUi();
            const auto wm = b != nullptr ? b->writingMode : core::text::WritingMode::HorizontalTB;
            const auto orient = b != nullptr ? b->orientation : core::text::TextOrientation::Mixed;
            const auto aa = b != nullptr ? b->aa : core::text::AntiAlias::Grayscale;
            // R4 §3.4: the selection's variable axes (cached per face in the shaper, so this is
            // cheap per reflect). A mixed-family selection resolves nothing -> no axis sliders.
            std::vector<core::text::VariableAxis> axes;
            if (cs.agree.family)
                if (const auto face = m_fontDb.resolve(cs.style.font))
                    axes = m_textShaper.variableAxes(*face);
            m_typePanel->reflect(cs, cp, wm, orient, aa, axes);
        }
        refreshType3dPanel();
    }

    // Re-render the 3D popup's viewport from the edited block's current state. Split out of
    // reflectTextOptions because the viewport renders THROUGH the layer's effects (S30-e): a
    // Layer-Effects edit must refresh it even though no block field changed (user 2026-07-16:
    // "3D live-preview does not update until transforming when enabling/disabling layer effects").
    void refreshType3dPanel() {
        if (m_type3dPanel == nullptr || !m_type3dPanel->shown() || m_canvas == nullptr)
            return;
        const core::text::TextBlock* b = m_canvas->textEditBlockForUi();
        m_type3dPanel->reflect(b != nullptr ? b->extrude : std::optional<core::text::Extrude>{},
                               b != nullptr);
    }

    void captureTextBarSnapshot() {
        m_textBarSnapshot.clear();
        if (const ui::Tool* t = m_tools.find(ToolId::Text))
            for (const ui::ToolOption& o : t->options())
                m_textBarSnapshot[o.id] = o.value;
    }

    // Apply a bar edit to the selection (bar -> model). Diff each control against the last snapshot
    // so ONLY the touched control writes its field -- mixed fields stay mixed unless edited, and
    // one control's change never flattens another. Same-control consecutive edits (a size drag)
    // coalesce into one undo step; switching controls starts a new one.
    void applyTextOptionEdits() {
        if (m_reflectingTextOptions || m_canvas == nullptr ||
            m_canvas->textEditTarget() == core::kInvalidLayerId)
            return;
        const ui::Tool* t = m_tools.find(ToolId::Text);
        if (t == nullptr)
            return;
        for (const ui::ToolOption& o : t->options()) {
            const auto prev = m_textBarSnapshot.find(o.id);
            if (prev != m_textBarSnapshot.end() && prev->second == o.value)
                continue; // unchanged
            const bool coalesce = (o.id == m_lastTextEditOptionId);
            applyTextOption(o.id, o.value, coalesce);
            m_lastTextEditOptionId = o.id;
            m_textBarSnapshot[o.id] = o.value;
        }
    }

    // Route one changed bar control to its model target via the selection-style funnel. Align and
    // every block-level property (writing mode, orientation, anti-alias) live in the Type panel now
    // (applyTextStyleField / applyTextParagraphField / applyTextBlockField), not the bar.
    void applyTextOption(const std::string& id, double value, bool coalesce) {
        if (id == "font") {
            std::string family = textToolFontFamily();
            m_canvas->applySelectionStyle(
                [f = std::move(family)](core::text::CharStyle& s) { s.font.family = f; }, coalesce);
        } else if (id == "size") {
            const auto px = static_cast<float>(value);
            m_canvas->applySelectionStyle([px](core::text::CharStyle& s) { s.sizePx = px; },
                                          coalesce);
        } else if (id == "bold") {
            const bool on = value != 0.0;
            m_canvas->applySelectionStyle(
                [on](core::text::CharStyle& s) { s.font.weight = on ? 700.0f : 400.0f; }, coalesce);
        } else if (id == "italic") {
            const bool on = value != 0.0;
            m_canvas->applySelectionStyle([on](core::text::CharStyle& s) { s.font.italic = on; },
                                          coalesce);
        } else if (id == "underline") {
            const bool on = value != 0.0;
            m_canvas->applySelectionStyle([on](core::text::CharStyle& s) { s.underline = on; },
                                          coalesce);
        } else if (id == "strikethrough") {
            const bool on = value != 0.0;
            m_canvas->applySelectionStyle([on](core::text::CharStyle& s) { s.strikethrough = on; },
                                          coalesce);
        }
    }

    // The Type panel's edit funnels (S29-c §8). They share m_lastTextEditOptionId with the bar so a
    // continuous drag is one undo step and switching controls -- on EITHER surface -- starts a new
    // one (the canvas's coalesce counter is the single source). After a panel edit the block
    // revision bumps, so the per-frame onSelectionChanged re-runs reflectTextOptions, keeping the
    // bar in step.
    void applyTextStyleField(const std::string& id,
                             std::function<void(core::text::CharStyle&)> mutate) {
        if (m_canvas == nullptr || m_canvas->textEditTarget() == core::kInvalidLayerId)
            return;
        const bool coalesce = (id == m_lastTextEditOptionId);
        m_canvas->applySelectionStyle(mutate, coalesce);
        m_lastTextEditOptionId = id;
    }
    void applyTextParagraphField(const std::string& id,
                                 std::function<void(core::text::Paragraph&)> mutate) {
        if (m_canvas == nullptr || m_canvas->textEditTarget() == core::kInvalidLayerId)
            return;
        const bool coalesce = (id == m_lastTextEditOptionId);
        m_canvas->applySelectionParagraph(mutate, coalesce);
        m_lastTextEditOptionId = id;
    }
    void applyTextBlockField(const std::string& id,
                             std::function<void(core::text::TextBlock&)> mutate) {
        if (m_canvas == nullptr || m_canvas->textEditTarget() == core::kInvalidLayerId)
            return;
        const bool coalesce = (id == m_lastTextEditOptionId);
        m_canvas->applyTextBlockEdit(std::move(mutate), coalesce);
        m_lastTextEditOptionId = id;
    }

    // The foreground swatch changed while editing text: recolour the selection (fill = fg), one
    // undo step per change. The Type twin of onShapeColorEdited. No-op unless the Type tool is
    // editing.
    void onTextColorEdited() {
        if (m_canvas == nullptr || m_tools.active() != ToolId::Text ||
            m_canvas->textEditTarget() == core::kInvalidLayerId)
            return;
        const common::ColorF c = colorToF(m_colors.foreground());
        m_canvas->applySelectionStyle([c](core::text::CharStyle& s) { s.setSolidFill(c); });
    }

    // Create a TextLayer carrying `block`, placed by `placement`, as one undoable step (Add Text),
    // inserted above the active layer like a paste; returns its id so the canvas edits it live.
    core::LayerId createTextLayer(core::text::TextBlock block, const common::Affine2D& placement) {
        if (!m_document)
            return core::kInvalidLayerId;
        std::unique_ptr<core::TextLayer> layer = m_document->makeText(_("Text"));
        layer->setBlock(std::move(block));
        layer->setTransform(placement);
        const core::LayerId id = layer->id();
        core::LayerId parentId = m_document->root().id();
        std::size_t index = m_document->root().childCount();
        if (m_layerPanel != nullptr) {
            if (const std::optional<core::Document::Location> loc =
                    m_document->locate(m_layerPanel->activeLayer())) {
                parentId = loc->parent->id();
                index = loc->index + 1;
            }
        }
        m_document->commands().push(
            std::make_unique<core::AddLayerCommand>(parentId, index, std::move(layer)));
        syncAfterEdit();
        if (m_layerPanel != nullptr)
            m_layerPanel->setActive(id);
        return id;
    }

    // Commit finished: an abandoned EMPTY text block is discarded (§6), removed as its own undo
    // step.
    void finishTextLayer(core::LayerId id) {
        if (!m_document)
            return;
        core::Layer* l = m_document->find(id);
        auto* tl = l != nullptr ? l->as<core::TextLayer>() : nullptr;
        if (tl != nullptr && tl->block().empty()) {
            m_document->commands().push(std::make_unique<core::RemoveLayerCommand>(id));
            syncAfterEdit();
        }
    }

    void cancelShape() {
        if (!m_document || m_shapePreviewLayer == core::kInvalidLayerId)
            return;
        if (const std::optional<core::Document::Location> loc =
                m_document->locate(m_shapePreviewLayer)) {
            const std::unique_ptr<core::Layer> dropped = loc->parent->removeAt(loc->index);
            (void)dropped; // discard the spawned-but-uncommitted layer
        }
        m_shapePreviewLayer = core::kInvalidLayerId;
        if (m_layerPanel != nullptr)
            m_layerPanel->refresh();
        requestRecomposite(/*fitView=*/false);
    }

    // --- S22 Gradient tool host --------------------------------------------------------------
    // The tool's working ramp (stops + spread), seeded lazily foreground -> transparent (the most
    // useful default). Only its stops/spread are read by the canvas; the type + transform come from
    // the drag. The "Stops…" flyout edits it in place.
    [[nodiscard]] core::vec::Gradient workingGradient() {
        if (!m_gradientPaintSeeded) {
            const common::ColorF fg = colorToF(m_colors.foreground());
            const common::ColorF fade{fg.r, fg.g, fg.b, 0.0f}; // fg -> transparent
            m_gradientPaint.type = core::vec::GradientType::Linear;
            m_gradientPaint.spread = core::vec::SpreadMethod::Pad;
            m_gradientPaint.stops = {{0.0, fg}, {1.0, fade}};
            m_gradientPaintSeeded = true;
        }
        return m_gradientPaint;
    }

    // Live gradient authoring (Affinity-style; the Shape tool worked this way too until S26-c): the
    // preview IS a real full-bleed gradient VectorLayer the compositor renders each drag frame,
    // inserted outside the command stack so dragging never spams undo. commitGradient() bakes it
    // into one Add-Layer step.
    //
    // With an active selection the layer is born MASKED to it (the same "select, then apply" reflex
    // Filter ▸ Adjustments got -- see insertAdjustmentLayer for the shared reasoning). The mask is
    // built ONCE, on the frame the layer is born and AFTER its transform is set: the drag preview
    // therefore shows the masked result from the first frame -- a preview that ignored the mask
    // would lie about what release produces -- while the per-frame path stays exactly as cheap as
    // it was (object + transform + opacity; the sheet is never rebuilt). Because the mask lives ON
    // the layer, it rides into commitGradient's AddLayerCommand for free: one History step, undo
    // takes layer and mask together, and cancelGradient drops both with the layer.
    void previewGradient(const ui::GradientDraft& draft, double opacity) {
        if (!m_document)
            return;
        core::VectorLayer* vl = nullptr;
        if (m_gradientPreviewLayer != core::kInvalidLayerId) {
            core::Layer* l = m_document->find(m_gradientPreviewLayer);
            vl = l != nullptr ? l->as<core::VectorLayer>() : nullptr;
        }
        const bool born = vl == nullptr;
        if (born) { // first frame of this drag: create + directly insert the live layer
            std::unique_ptr<core::VectorLayer> layer = m_document->makeVector(_("Gradient"));
            m_gradientPreviewLayer = layer->id();
            core::GroupLayer* parent = &m_document->root();
            std::size_t index = parent->childCount();
            if (m_layerPanel != nullptr) {
                if (const std::optional<core::Document::Location> loc =
                        m_document->locate(m_layerPanel->activeLayer())) {
                    if (auto* g = loc->parent->as<core::GroupLayer>()) {
                        parent = g;
                        index = loc->index + 1;
                    }
                }
            }
            vl = static_cast<core::VectorLayer*>(&parent->insert(index, std::move(layer)));
            if (m_layerPanel != nullptr) { // the new row shows + becomes active while you draw
                m_layerPanel->refresh();
                m_layerPanel->setActive(m_gradientPreviewLayer);
            }
        }
        vl->setObject(draft.object);
        vl->setTransform(draft.placement);
        vl->setOpacity(static_cast<float>(opacity));
        // Automask, AFTER the transform: a gradient layer is a full-bleed VECTOR layer, so its mask
        // sheet is the document window placed by RasterMask::toLocal = the inverse of the layer's
        // world transform AT BUILD TIME (the grid contract in core/layer.hpp). Building it before
        // setTransform would capture the identity and then slide the sheet by the placement's
        // half-document translation -- the shape-layer "three erased quadrants" bug exactly. Built
        // here, maskToDocument is the identity, maskFromSelection takes its 1:1 fast path and the
        // sheet IS the selection's coverage, feather and AA edge intact. anySelected() is the whole
        // gate (false for "no selection" AND for an active selection of nothing), like adjustments.
        if (born && m_document->selection().anySelected())
            vl->setMask(core::maskFromSelection(*vl, m_document->selection(), m_document->width(),
                                                m_document->height()));
        requestRecomposite(/*fitView=*/false);
    }

    void commitGradient() {
        if (!m_document || m_gradientPreviewLayer == core::kInvalidLayerId)
            return;
        const std::optional<core::Document::Location> loc =
            m_document->locate(m_gradientPreviewLayer);
        const core::LayerId id = m_gradientPreviewLayer;
        m_gradientPreviewLayer = core::kInvalidLayerId;
        if (!loc)
            return;
        // Detach the live layer and re-add it THROUGH a command, so the whole drag is one undo step.
        std::unique_ptr<core::Layer> layer = loc->parent->removeAt(loc->index);
        // The automask previewGradient built rides INSIDE that layer, so it is part of the same
        // step; nothing is rebuilt here (a second build would re-capture the placement for no gain).
        const bool masked = layer != nullptr && layer->mask() != nullptr;
        auto cmd = std::make_unique<core::CompositeCommand>(std::string(_("Add Gradient")));
        cmd->add(std::make_unique<core::AddLayerCommand>(loc->parent->id(), loc->index,
                                                         std::move(layer)));
        m_document->commands().push(std::move(cmd));
        syncAfterEdit();
        if (m_layerPanel != nullptr)
            m_layerPanel->setActive(id);
        if (m_canvas != nullptr) {
            m_canvas->bindGradientEditToActiveLayer(id); // its handles show at once for re-editing
            syncGradientBarToEditTarget(); // ... and the bar's Type/Opacity now drive THAT layer
        }
        // Same wording as the adjustment automask -- one msgid, one thing being said. The selection
        // is deliberately left active: the ants read as "this is what got masked".
        if (masked)
            transientStatus(_("Masked to the selection"));
    }

    void cancelGradient() {
        if (!m_document || m_gradientPreviewLayer == core::kInvalidLayerId)
            return;
        if (const std::optional<core::Document::Location> loc =
                m_document->locate(m_gradientPreviewLayer)) {
            const std::unique_ptr<core::Layer> dropped = loc->parent->removeAt(loc->index);
            (void)dropped; // discard the live preview layer
        }
        m_gradientPreviewLayer = core::kInvalidLayerId;
        if (m_layerPanel != nullptr)
            m_layerPanel->refresh();
        requestRecomposite(/*fitView=*/false);
    }

    // The layer the Gradient tool currently has bound for editing, when it really is a gradient
    // VectorLayer (null otherwise). Shared by the bar/flyout read-backs and the option writers.
    [[nodiscard]] core::VectorLayer* boundGradientLayer() {
        if (m_canvas == nullptr || !m_document)
            return nullptr;
        const core::LayerId id = m_canvas->gradientEditTarget();
        if (id == core::kInvalidLayerId)
            return nullptr;
        core::Layer* l = m_document->find(id);
        auto* vl = l != nullptr ? l->as<core::VectorLayer>() : nullptr;
        if (vl == nullptr || !vl->hasObject() || !ui::gradientShapeOf(*vl->object()))
            return nullptr;
        return vl;
    }

    // The Gradient bar's Type / Opacity applied to the layer bound for editing (S22). Without a bound
    // layer these are simply the authoring defaults the next drag uses; WITH one they are live
    // controls, which is what "picking Radial does nothing" was: the choice only ever reached a
    // future drag. A retype keeps the gradient's centre + axis (ui::retypeGradient), so switching
    // kind re-reads the geometry already on the canvas instead of throwing it away. Edits coalesce
    // into one undo step per binding (m_gradientOptionCoalesce), like the flyout's stop session.
    void applyGradientOptionEdits() {
        if (m_reflectingGradientOptions || !m_document)
            return;
        // Dithering rides the PAINT (core::vec::Gradient::dither), so the bar's choice is read
        // BEFORE the bound-layer check: with no layer bound it is simply what the next drag
        // authors, exactly like the working ramp's stops and spread.
        if (const ui::ToolOption* o = toolOpt(ToolId::Gradient, "dither"))
            m_gradientPaint.dither = ui::gradientDitherFromChoice(static_cast<int>(o->value));
        core::VectorLayer* vl = boundGradientLayer();
        if (vl == nullptr)
            return;
        const core::LayerId id = vl->id();
        bool changed = false;
        // ... and with one bound it is live on that layer, like Type and Opacity. Applied first so
        // a retype in the same notification re-reads the dither that is already on the object.
        if (const auto* g = std::get_if<core::vec::Gradient>(&vl->object()->fill);
            g != nullptr && g->dither != m_gradientPaint.dither) {
            core::vec::Object edited = *vl->object();
            std::get<core::vec::Gradient>(edited.fill).dither = m_gradientPaint.dither;
            m_document->commands().push(std::make_unique<core::SetVectorObjectCommand>(
                id, std::move(edited), _("Edit Gradient"), m_gradientOptionCoalesce));
            changed = true;
        }
        if (const ui::ToolOption* o = toolOpt(ToolId::Gradient, "type")) {
            const std::optional<ui::GradientShape> have = ui::gradientShapeOf(*vl->object());
            const ui::GradientShape want = ui::gradientShapeFromChoice(static_cast<int>(o->value));
            if (have && want != *have) {
                m_document->commands().push(std::make_unique<core::SetVectorObjectCommand>(
                    id, ui::retypeGradient(*vl->object(), want), _("Edit Gradient"),
                    m_gradientOptionCoalesce));
                changed = true;
            }
        }
        if (const ui::ToolOption* o = toolOpt(ToolId::Gradient, "opacity")) {
            const auto want = static_cast<float>(std::clamp(o->value / 100.0, 0.0, 1.0));
            if (std::abs(want - vl->opacity()) > 1e-4f) {
                m_document->commands().push(
                    std::make_unique<core::SetOpacityCommand>(id, want, m_gradientOptionCoalesce));
                changed = true;
            }
        }
        if (changed)
            requestRecomposite(/*fitView=*/false); // a coalesced merge fires no command onChange
    }

    // Read-back: the bound gradient layer's shape + opacity pushed INTO the bar (model -> bar), so
    // re-entering the tool on a radial layer shows "Radial" rather than whatever the last drag used
    // -- and so the very next bar edit is a no-op instead of silently retyping the layer. Guarded so
    // the resulting notifyOptionsChanged is not taken as a user edit. Also opens a fresh coalescing
    // session, from the canvas's gradient-edit sequence, so bar edits never merge into a handle drag.
    void syncGradientBarToEditTarget() {
        if (m_canvas == nullptr)
            return;
        m_gradientOptionCoalesce = m_canvas->beginGradientEditSession();
        const core::VectorLayer* vl = boundGradientLayer();
        if (vl == nullptr)
            return;
        const std::optional<ui::GradientShape> shape = ui::gradientShapeOf(*vl->object());
        if (ui::ToolOption* o = toolOpt(ToolId::Gradient, "type"); o != nullptr && shape)
            o->value = static_cast<double>(ui::gradientChoiceForShape(*shape));
        if (const auto* g = std::get_if<core::vec::Gradient>(&vl->object()->fill); g != nullptr) {
            m_gradientPaint.dither = g->dither; // the layer's own kind becomes the working one
            if (ui::ToolOption* o = toolOpt(ToolId::Gradient, "dither"))
                o->value = static_cast<double>(ui::gradientDitherChoice(g->dither));
        }
        if (ui::ToolOption* o = toolOpt(ToolId::Gradient, "opacity"))
            o->value = std::clamp(static_cast<double>(vl->opacity()) * 100.0, o->min, o->max);
        m_reflectingGradientOptions = true; // our own write-back, not a user edit
        m_tools.notifyOptionsChanged();
        m_reflectingGradientOptions = false;
    }

    // A flyout stops/spread edit reaches the gradient layer currently bound for editing (if any), as
    // one coalesced undo step per flyout session -- the ramp changes without touching the geometry.
    void applyGradientStopsToEditTarget() {
        if (m_canvas == nullptr || !m_document)
            return;
        const core::LayerId id = m_canvas->gradientEditTarget();
        if (id == core::kInvalidLayerId)
            return;
        core::Layer* l = m_document->find(id);
        auto* vl = l != nullptr ? l->as<core::VectorLayer>() : nullptr;
        if (vl == nullptr || !vl->hasObject())
            return;
        auto* g = std::get_if<core::vec::Gradient>(&vl->object()->fill);
        if (g == nullptr)
            return;
        core::vec::Object edited = *vl->object();
        auto& eg = std::get<core::vec::Gradient>(edited.fill);
        eg.stops = m_gradientPaint.stops; // keep the layer's type + transform (its geometry)
        eg.spread = m_gradientPaint.spread;
        m_document->commands().push(std::make_unique<core::SetVectorObjectCommand>(
            id, std::move(edited), _("Edit Gradient"), m_gradientStopsCoalesce));
        requestRecomposite(/*fitView=*/false);
    }

    // Open (or toggle shut) the gradient stops/spread/blend-curve flyout, anchored to the options
    // bar's "Stops…" button and seeded from the working ramp (its current type is preserved).
    void openGradientStopsFlyout() {
        if (m_gradientFlyout == nullptr || m_optionsBar == nullptr)
            return;
        Fl_Widget* anchor = m_optionsBar->gradientStopsButton();
        if (anchor == nullptr)
            return;
        if (m_gradientFlyout->shownForAnchor(anchor)) { // a re-click toggles it shut
            m_gradientFlyout->hide();
            return;
        }
        core::vec::Gradient seed = workingGradient();
        // With a gradient layer bound for editing, the flyout edits THAT layer's ramp: seeding from
        // the tool's working stops would show the wrong colours and overwrite the layer's on the
        // first edit. The working ramp follows it, so the next fresh drag continues where you left.
        if (const core::VectorLayer* vl = boundGradientLayer()) {
            const auto& bound = std::get<core::vec::Gradient>(vl->object()->fill);
            m_gradientPaint.stops = bound.stops;
            m_gradientPaint.spread = bound.spread;
            m_gradientPaintSeeded = true;
            seed.stops = bound.stops;
            seed.spread = bound.spread;
        }
        // One undo step for this flyout session's edits, from the canvas's gradient-edit sequence so
        // it never collides with a handle drag on the same layer.
        m_gradientStopsCoalesce =
            m_canvas != nullptr ? m_canvas->beginGradientEditSession() : m_gradientStopsCoalesce + 1;
        m_gradientFlyout->openFor(anchor, seed);
    }

    // File->Close / Ctrl+W (S49). The last tab closes to the empty state, never quitting: the
    // window is not the document. Public because the menu thunk is a free function.
    void closeActiveSession() {
        if (m_sessions.empty())
            return;
        (void)closeSession(m_activeSession);
    }

private:
    // The active layer in the panel, resolved in the document (null when none/stale).
    [[nodiscard]] core::Layer* activeLayerPtr() const {
        if (!m_document || m_layerPanel == nullptr)
            return nullptr;
        return m_document->find(m_layerPanel->activeLayer());
    }

    // Put `img` on the OS clipboard. Fl_Copy_Surface records drawing commands as clipboard
    // image data; alpha doesn't survive the trip, so transparent pixels are pre-flattened
    // over white (the in-app clipboard keeps the real alpha).
    static void pushToOsClipboard(const common::Image& img) {
        const common::Image flat = core::flattenedOverWhite(img);
        Fl_Copy_Surface surface(static_cast<int>(flat.width), static_cast<int>(flat.height));
        Fl_Surface_Device::push_current(&surface);
        fl_draw_image(flat.rgba.data(), 0, 0, static_cast<int>(flat.width),
                      static_cast<int>(flat.height), 4, 0);
        Fl_Surface_Device::pop_current();
    }

    // An Fl_RGB_Image (1/3/4 channels, possibly row-padded) as straight RGBA.
    [[nodiscard]] static common::Image fromFlImage(const Fl_RGB_Image& img) {
        const int w = img.w();
        const int h = img.h();
        const int d = img.d();
        if (img.array == nullptr || w <= 0 || h <= 0 || d < 1 || d > 4)
            return {};
        const int stride = img.ld() != 0 ? img.ld() : w * d;
        common::Image out(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
        for (int y = 0; y < h; ++y) {
            const unsigned char* row = img.array + static_cast<std::ptrdiff_t>(y) * stride;
            for (int x = 0; x < w; ++x) {
                const unsigned char* s = row + static_cast<std::ptrdiff_t>(x) * d;
                const std::size_t p = (static_cast<std::size_t>(y) * out.width + x) * 4;
                out.rgba[p + 0] = s[0];
                out.rgba[p + 1] = d >= 3 ? s[1] : s[0];
                out.rgba[p + 2] = d >= 3 ? s[2] : s[0];
                out.rgba[p + 3] = d == 4 ? s[3] : 255;
            }
        }
        return out;
    }

public:
    // Popovers (the colour picker, the tool flyout) are fixed-size sub-windows, not layout
    // participants. The base resize moves the canvas/dock/toolbar (re-pinning the swatch and slot
    // buttons) and, since a popover is a child sub-window, also stretches and strands an open one
    // -- so afterwards we re-pin whichever popover is open to its (moved) anchor at its fixed size.
    // ---- the resizable dock ----------------------------------------------------------------
    // `m_dockWidth` is the width the USER asked for. What actually gets laid out is that value
    // clamped to whatever the current window can afford, and the unclamped wish is kept so the dock
    // springs back to its full size when the window grows again.
    [[nodiscard]] int effectiveDockWidth() const {
        const int maxForWindow = std::max(kDockMinWidth, w() - kToolbarWidth - kCanvasMinWidth);
        return std::clamp(m_dockWidth, kDockMinWidth, std::min(kDockMaxWidth, maxForWindow));
    }

    // Place the three body regions (toolbar | canvas | dock) for the current window + dock width.
    // The canvas is an Fl_Window: resizing it recreates the swapchain, so only call this when the
    // geometry has genuinely moved.
    void applyDockWidth() {
        if (m_canvas == nullptr || m_dock == nullptr)
            return;
        // The tab strip (S49) sits over the CANVAS column, between the options bar and the canvas
        // itself -- the tabs belong to the document area, not to the tool column or the dock, and
        // both of those run the strip's full height beside it. It takes no row at all while hidden
        // (<= 1 document), so it is part of the canvas's top, not a fixed offset.
        const int colTop = kMenuBarHeight + kOptionsBarHeight; // toolbar + dock + strip start here
        const int strip = tabStripHeight();
        const int colH = std::max(1, h() - colTop - kStatusBarHeight);
        const int bodyTop = colTop + strip; // ... the canvas starts under the strip
        const int bodyH = std::max(1, colH - strip);
        // With no document the dock has nothing to show (Layers/History/presets all describe one),
        // so it takes no column at all and the empty-state invitation runs to the window's edge.
        const bool dockVisible = m_document != nullptr;
        const int dock = dockVisible ? effectiveDockWidth() : 0;
        const int canvasW = std::max(1, w() - kToolbarWidth - dock);
        // Cheap and idempotent; done before the early-out so a splitter drag re-widths the strip.
        // (The left toolbar needs no re-placement here: colTop and colH do not move when the strip
        // toggles -- only the canvas's own top does.)
        if (dockVisible != (m_dock->visible() != 0)) {
            if (dockVisible)
                m_dock->show();
            else
                m_dock->hide();
        }
        if (dockVisible)
            m_dock->resize(kToolbarWidth + canvasW, colTop, dock, colH);
        if (m_tabStrip != nullptr && strip > 0)
            m_tabStrip->resize(kToolbarWidth, colTop, canvasW, strip);
        // Rulers (View -> Rulers) inset the canvas by a gutter along its top + left edges. Hidden
        // while no document is open (there are no coordinates to show). The horizontal strip spans
        // the whole canvas column so it also covers the top-left corner square.
        const bool rulersOn = m_rulersVisible && m_document != nullptr;
        const int gutter = rulersOn ? kRulerSize : 0;
        if (m_rulerH != nullptr && m_rulerV != nullptr) {
            if (rulersOn) {
                m_rulerH->resize(kToolbarWidth, bodyTop, canvasW, kRulerSize);
                m_rulerV->resize(kToolbarWidth, bodyTop + kRulerSize, kRulerSize,
                                 std::max(1, bodyH - kRulerSize));
                if (m_rulerH->visible() == 0) {
                    m_rulerH->show();
                    m_rulerV->show();
                }
            } else if (m_rulerH->visible() != 0) {
                m_rulerH->hide();
                m_rulerV->hide();
            }
        }
        const int cx = kToolbarWidth + gutter;
        const int cy = bodyTop + gutter;
        const int cw = std::max(1, canvasW - gutter);
        const int ch = std::max(1, bodyH - gutter);
        if (m_canvas->x() == cx && m_canvas->y() == cy && m_canvas->w() == cw && m_canvas->h() == ch)
            return; // nothing moved: don't churn the Vulkan swapchain
        m_canvas->resize(cx, cy, cw, ch);
        redraw();
    }

    // A splitter drag calls this once per pointer event. applyDockWidth() reconfigures the canvas's
    // native child sub-window and marks the swapchain dirty -- notifyResize only sets a flag, so at
    // most one swapchain is rebuilt per RENDERED frame, but a 120 Hz pointer would still reconfigure
    // the native surface twice per frame for nothing. The zero-delay timeout collapses a whole batch
    // of queued motion events into one relayout, and FLTK runs timeouts before the next draw -- so
    // the dock still tracks the cursor with no visible lag.
    static void dockRelayoutTimer(void* self) {
        auto* win = static_cast<MainWindow*>(self);
        win->m_dockRelayoutPending = false;
        win->applyDockWidth();
    }

    void setDockWidth(int width) {
        const int next = std::clamp(width, kDockMinWidth, kDockMaxWidth);
        if (next == m_dockWidth)
            return;
        m_dockWidth = next;
        dismissActiveColorFlyout(); // anchored to a chip that just moved
        reanchorActivePopover();
        if (!m_dockRelayoutPending) {
            m_dockRelayoutPending = true;
            Fl::add_timeout(0.0, dockRelayoutTimer, this);
        }
    }

    void persistDockWidth() const {
        if (m_settingsPath.empty())
            return;
        std::string err;
        common::Settings cfg = common::loadSettings(m_settingsPath, &err);
        cfg.dockWidth = m_dockWidth;
        if (!common::saveSettings(cfg, m_settingsPath, &err))
            uiLog().warn("could not persist dock width: {}", err);
    }

    // The preset section's height (§8.2) -- window LAYOUT state, exactly like the dock's width, and
    // persisted on the same terms: once at the END of a splitter drag, never once per drag frame.
    void persistBrushPresetHeight(int height) const {
        if (m_settingsPath.empty())
            return;
        std::string err;
        common::Settings cfg = common::loadSettings(m_settingsPath, &err);
        cfg.brushPresetHeight = height;
        if (!common::saveSettings(cfg, m_settingsPath, &err))
            uiLog().warn("could not persist the preset section's height: {}", err);
    }

    // The selected preset, BY NAME (never by index -- the library's order is a directory scan).
    void persistBrushPreset() const {
        if (m_settingsPath.empty())
            return;
        const io::brush::LibraryPreset* p = m_brushPresets.activePreset();
        const io::brush::LibraryPreset* e = m_brushPresets.activeEraserPreset();
        std::string err;
        common::Settings cfg = common::loadSettings(m_settingsPath, &err);
        cfg.brushPreset = p != nullptr ? p->preset.name : std::string(); // "" = the round tip
        cfg.eraserPreset = e != nullptr ? e->preset.name : std::string(); // ... and the Eraser's own
        if (!common::saveSettings(cfg, m_settingsPath, &err))
            uiLog().warn("could not persist the brush preset: {}", err);
    }

    void resize(int X, int Y, int W, int H) override {
        Fl_Double_Window::resize(X, Y, W, H);
        // Pin the menu bar to a FIXED height + full width, independent of the group's resizable()
        // stretch, so it never grows taller or scales oddly on a resize / maximise. resizable() is
        // the canvas (below the top chrome), so this normally already holds -- pinning makes the
        // invariant explicit and defends it. (The themed pop-up submenus size themselves from their
        // content and swallow window-driven stretching; see MenuPopup::resize in menu_bar.cpp.)
        // (Not on macOS: the bar is not a child of this window there -- Fl_Sys_Menu_Bar removes
        // itself from its group -- and kMenuBarHeight is 0, so there is no row to pin.)
#ifndef __APPLE__
        if (m_menu != nullptr)
            m_menu->resize(0, 0, W, kMenuBarHeight);
#endif
        // The group's resizable() gave the whole width delta to the canvas; re-pin the dock to its
        // (clamped) width so a shrinking window eats the canvas, not the dock -- and a growing one
        // hands the space back to the canvas rather than stretching the dock.
        applyDockWidth();
        // (The menu-bar motivational ticker needs no re-placement: the bar draws it in its own
        // right region, recomputed from the resized geometry on the next draw.)
        // A width change rebuilds the options bar, destroying + recreating the Type panel's anchor
        // button. If the panel is open, re-point it at the NEW button (place() would otherwise
        // deref the freed one -> a resize crash) and restore its pressed look, then re-place it at
        // its corner.
        if (m_typePanel != nullptr && m_typePanel->shown() && m_optionsBar != nullptr) {
            if (Fl_Widget* btn = m_optionsBar->typePanelButton()) {
                m_typePanel->retargetAnchor(btn);
                setTypePanelButtonOpen(true);
                m_typePanel
                    ->reanchor(); // re-fix to the corner at its base size (undo the group scaling)
            } else {
                closeTypePanel(); // no fresh anchor to re-point to -> close rather than deref a
                                  // freed one
            }
        }
        // The 3D popup's twin block, a SIBLING of the Style one -- Style and 3D are mutually
        // exclusive, so nesting this inside the Style branch left an open 3D popup un-retargeted
        // (its anchor freed by the bar rebuild) and reanchorActivePopover() below crashed the
        // resize (user report 2026-07-03).
        if (m_type3dPanel != nullptr && m_type3dPanel->shown() && m_optionsBar != nullptr) {
            if (Fl_Widget* btn = m_optionsBar->type3dButton()) {
                m_type3dPanel->retargetAnchor(btn);
                setType3dButtonOpen(true);
                m_type3dPanel->reanchor();
            } else {
                closeType3dPanel(); // no fresh anchor -> close rather than deref a freed one
            }
        }
        // The adjustment editor's anchor (the layer panel) survives resizes, so re-pin in place.
        if (m_adjustmentPanel != nullptr && m_adjustmentPanel->shown())
            m_adjustmentPanel->reanchor();
        // The morphology panel is corner-placed against the canvas region -- re-pin it on resize.
        if (m_selectMorphPanel != nullptr && m_selectMorphPanel->shown())
            m_selectMorphPanel->reanchor();
        dismissActiveColorFlyout(); // its anchor chip just moved with the panel; simplest is shut
        reanchorActivePopover();
    }

    // Window-level input. A press the main window receives dismisses an open popover (the colour
    // picker) unless it lands on the popover's anchor -- clicks inside the popover (a child
    // sub-window) go to that sub-window, not here, so they never reach this handler. Coordinates
    // are window- relative, matching the popover/anchor child geometry. Plain-letter shortcuts
    // (V/M/L/... for tools; X swaps and D resets the active colours) are handled on FL_SHORTCUT,
    // the "unclaimed key" phase, so a focused widget keeps first crack; modifier combos are left to
    // the menu accelerators. Since S51-b the KEYMAP supplies which letter means what -- the phase
    // design above is unchanged, and Keymap::check() enforces it from the other end by refusing to
    // store a modified chord for anything that dispatches here.
    int handle(int event) override {
        // Whether this window is the one the user is looking at. FL_FOCUS/FL_UNFOCUS are OBSERVED
        // here, never consumed -- the return value below still comes from the real dispatch, so
        // focus semantics are untouched. FL_HIDE covers iconify and workspace switches, which is
        // the case where drawing at all is pure waste.
        switch (event) {
        case FL_FOCUS:
        case FL_SHOW:
            m_windowActive = true;
            break;
        case FL_UNFOCUS:
        case FL_HIDE:
            m_windowActive = false;
            break;
        default:
            break;
        }
        // NB while an inpaint runs we intentionally do NOT swallow input here: pan/zoom/rotate stay
        // live (the canvas keeps focus + its key handling), editing is blocked by the canvas's
        // inpaint-busy mode + the deactivated panels, and the status bar's cancel X must keep
        // working.
        // Window-level file drag-and-drop.
        //
        // With NO document open the whole window is the empty state's "drop a file" invitation.
        // With one open, the two surfaces with a SPECIFIC meaning take the drop first: the canvas
        // places magic layers, the tab strip opens documents (S50). Everywhere else -- the chrome:
        // menu, toolbar, options bar, dock, status bar -- a drop opens the files as new tabs, the
        // browser convention (drop-anywhere opens; only the canvas means "place INTO this one").
        //
        // The routing is explicit, and it has to be. FLTK's own dispatch will hand a DND event down
        // to a child SUB-window (the canvas) but not to a plain child widget (the tab strip) --
        // verified empirically, not assumed. So we pick the target by coordinates and pin
        // Fl::belowmouse() to it on FL_DND_ENTER: FLTK then delivers the later FL_DND_RELEASE, and
        // the FL_PASTE carrying the URI list, straight to that widget (fl_selection_requestor is
        // belowmouse), and it emits the FL_DND_LEAVE on the previous target for free when the
        // pointer crosses between them. A chrome drag accepts HERE instead (belowmouse becomes the
        // window itself, like the empty-state path), so its FL_PASTE lands in this handler.
        if (event == FL_DND_ENTER || event == FL_DND_DRAG || event == FL_DND_LEAVE ||
            event == FL_DND_RELEASE) {
            // A background save's finalize adopts the written file into the document its
            // snapshot came from; a drop that opens or places files mid-job would swap or edit
            // that document under the job. Controls are already disabled -- refuse the drag the
            // same way (DND bypasses child deactivation: it lands here and on belowmouse).
            if (m_saveJob || m_waitingForSave)
                return 0;
            if (m_document) {
                if (Fl_Widget* target = fileDropTargetUnderPointer()) {
                    if (event == FL_DND_ENTER)
                        Fl::belowmouse(target);
                    // Hand-routing bypasses Fl_Group::send, which is what normally puts
                    // Fl::event_x/y into a child SUB-window's own frame (and restores them after).
                    // The canvas is one, and it reads that pair as canvas-local -- so do the same
                    // two lines here rather than deliver it a position in our frame. A plain child
                    // widget (the tab strip) shares our frame and must NOT be shifted.
                    if (target->as_window() == nullptr)
                        return target->handle(event);
                    const int savedX = Fl::e_x;
                    const int savedY = Fl::e_y;
                    Fl::e_x -= target->x();
                    Fl::e_y -= target->y();
                    const int handled = target->handle(event);
                    Fl::e_x = savedX;
                    Fl::e_y = savedY;
                    return handled;
                }
                // Chrome: accept, and open the payload as documents (new tabs) on release.
                if (event == FL_DND_RELEASE)
                    m_dndOpenPending = true;
                return 1;
            }
            if (m_canvas)
                m_canvas->setIdleDropHot(event == FL_DND_ENTER || event == FL_DND_DRAG);
            // Accepting FL_DND_RELEASE makes FLTK deliver the dropped URI list as the NEXT
            // FL_PASTE; the pending flag keeps that delivery separate from Edit->Paste's
            // clipboard-image reply below.
            if (event == FL_DND_RELEASE)
                m_dndOpenPending = true;
            return 1;
        }
        if (event == FL_PASTE && m_clipboardFetchPending &&
            Fl::event_clipboard_type() == Fl::clipboard_image) {
            // fetchClipboardImage()'s reply (File->New, S55): store the image for the caller's
            // bounded pump -- it never pastes into a document. Checked before the DnD branch --
            // a drop's payload is text, so no collision.
            m_clipboardFetchDone = true;
            auto* img = static_cast<Fl_RGB_Image*>(Fl::event_clipboard());
            if (img != nullptr && img->w() > 0 && img->h() > 0) {
                common::Image rgba = fromFlImage(*img);
                if (!rgba.empty())
                    m_clipboardFetch = std::move(rgba);
            }
            return 1;
        }
        if (event == FL_PASTE && m_dndOpenPending) { // the accepted drop's payload
            m_dndOpenPending = false;
            if (m_canvas)
                m_canvas->setIdleDropHot(false);
            // Every dropped file opens as its own document/tab, in drop order -- the same meaning
            // a tab-strip drop has (a multi-file drop on chrome must not silently keep only one).
            const char* dropped = Fl::event_text();
            for (const std::string& path : localPathsFromDndText(dropped != nullptr ? dropped : ""))
                openDocumentAtPath(path);
            return 1;
        }
        if (event == FL_PASTE &&
            Fl::event_clipboard_type() == Fl::clipboard_image) { // Edit→Paste's async reply
            onClipboardImage(static_cast<Fl_RGB_Image*>(Fl::event_clipboard()));
            return 1;
        }
        if (event == FL_PUSH) {
            dismissActivePopoverOnOutsideClick(Fl::event_x(), Fl::event_y());
            dismissActiveDropdownPopupOnOutsideClick(Fl::event_x(), Fl::event_y());
            dismissActiveContextMenuOnOutsideClick(Fl::event_x(), Fl::event_y());
            dismissActiveColorFlyoutOnOutsideClick(Fl::event_x(), Fl::event_y());
            dismissActiveMenuOnOutsideClick(Fl::event_x(), Fl::event_y());
            commitActiveScrubEditOnOutsideClick(Fl::event_x(),
                                                Fl::event_y()); // close a scrub editor
        }
        // Modifier keys change what a click WOULD do (the canvas op badge, the layer panel's
        // thumbnail ants + op glyph). Key events go to the focus widget and walk up to us
        // unconsumed -- the hovered widget never sees them -- and waiting for the next pointer
        // move fails a motionless cursor (trackballs; user report 2026-06-12). This is the one
        // spot that sees the keys regardless of focus: fan out, never consume.
        if (event == FL_KEYDOWN || event == FL_KEYUP) {
            int bit = 0;
            int left = 0;
            int right = 0;
            switch (Fl::event_key()) {
            case FL_Shift_L:
            case FL_Shift_R:
                bit = FL_SHIFT;
                left = FL_Shift_L;
                right = FL_Shift_R;
                break;
            case FL_Control_L:
            case FL_Control_R:
                bit = FL_CTRL;
                left = FL_Control_L;
                right = FL_Control_R;
                break;
            case FL_Alt_L:
            case FL_Alt_R:
                bit = FL_ALT;
                left = FL_Alt_L;
                right = FL_Alt_R;
                break;
            default:
                break;
            }
            if (bit != 0) {
                // X11 delivers a key event with the modifier state "as it existed immediately
                // PRIOR to the event", so for the transitioning key itself event_state() is
                // one event behind: Shift-down reads as unheld, Shift-up as still held (the
                // affordance appeared on RELEASE -- second user report). Force the bit to the
                // post-event truth before fanning out; a no-op on backends that already
                // report the post-event state. A release only clears the bit when the live
                // keyboard (Fl::get_key) holds neither sibling key -- both-Shifts-down minus
                // one Shift must stay Shift.
                const bool held = event == FL_KEYDOWN || Fl::get_key(left) || Fl::get_key(right);
                if (held)
                    Fl::e_state |= bit;
                else
                    Fl::e_state &= ~bit;
                if (m_layerPanel)
                    m_layerPanel->modifiersChanged();
                if (m_canvas)
                    m_canvas->modifiersChanged();
                if (bit == FL_ALT && m_menu)
                    m_menu->setShowMnemonics(held); // reveal the title underlines while Alt is held
            }
        }
        // Escape dismisses an open popover (the colour picker). A focused popover swallows its own
        // Escape (Popover::handle); but when focus is elsewhere -- the usual case, since opening
        // the picker doesn't move focus -- the unhandled key reaches us here as FL_SHORTCUT.
        // Consume it either way so FLTK's default "Escape closes the window" never fires and quits
        // the app.
        if (event == FL_SHORTCUT && Fl::event_key() == FL_Escape) {
            dismissActiveDropdownPopup();
            dismissActiveColorFlyout();
            dismissActivePopover();
            dismissActiveMenu();
            return 1;
        }
        // While an inpaint runs the chrome is locked (greyed menu/toolbar, deactivated panels); the
        // plain-letter shortcuts are handled here, NOT by those widgets, so gate them too --
        // otherwise V/M/L/J... would still switch tools and X/D swap colours behind the busy state.
        const bool inpaintBusy = m_canvas != nullptr && m_canvas->inpaintBusy();
        if (event == FL_SHORTCUT && !inpaintBusy &&
            (Fl::event_state() & (FL_CTRL | FL_ALT | FL_META)) == 0) {
            const char* t = Fl::event_text();
            if (t != nullptr && t[0] != '\0' && t[1] == '\0') { // exactly one character
                // The KEYMAP answers now, but the question is byte-for-byte the old one: ONE typed
                // character, no Ctrl/Alt/Cmd (the gate above), matched case-insensitively -- which
                // is why Shift+B has always picked the Brush and still does. The mask is deliberately
                // Shift-free and deliberately excludes nothing else: modifier chords belong to the
                // menu accelerators, which FLTK matches after this handler declines, and a
                // remappable chord must not be able to reach in and change that order.
                // Keymap::check() refuses a modified chord for anything dispatched here, so a
                // binding this phase cannot see is never storable in the first place.
                if (const Action* a = m_keymap.actionForDirectKey(t[0])) {
                    if (a->tool.has_value()) {
                        m_tools.setActive(*a->tool);
                        return 1;
                    }
                    if (a->id == "color.swap") { // Photoshop-standard: swap fg/bg
                        m_colors.swap();
                        return 1;
                    }
                    if (a->id == "color.reset") { // ... and default colours (black/white)
                        m_colors.reset();
                        return 1;
                    }
                }
            }
        }
        return Fl_Double_Window::handle(event);
    }

private:
    // The dock's preset section serves the BRUSH and the ERASER, and each sees its own corpus (§8.2,
    // §8.4). The other brush-family tools -- Inpaint, Heal, Clone, Smudge, Select-brush -- keep a
    // plain circular tip, because a scatter-and-rotation tip on a *healing* brush is not a feature.
    // When it hides, the layer panel takes the dock's full height. This is the first tool-conditional
    // docked chrome in the tree, so it is also the precedent.
    //
    // ⚠ The two corpora do not overlap and the split is SEMANTIC: a preset carrying CompositeOp=erase
    // is an eraser wherever it was filed, and the mapper already paints it in Erase mode. So the
    // Brush never offers one and the Eraser offers nothing else -- which is the whole of "move the
    // erasers to the Eraser tool", without moving a file or dropping a preset.
    void syncPresetSectionVisibility() {
        if (m_dock == nullptr)
            return;
        const ToolId tool = m_tools.active();
        const bool brush = tool == ToolId::Brush;
        const bool eraser = tool == ToolId::Eraser;
        if ((brush || eraser) && m_dock->presets() != nullptr) {
            m_dock->presets()->setCorpus(eraser ? ui::PresetCorpus::Eraser
                                                : ui::PresetCorpus::Brush);
            // ... and the grid must show THAT tool's selection. Each slot remembers its own, so
            // switching back and forth returns you to the brush you left, on both sides.
            m_dock->presets()->setSelected(eraser ? m_brushPresets.activeEraserIndex()
                                                  : m_brushPresets.activeIndex());
        }
        m_dock->setPresetsVisible(brush || eraser);
    }

    // The active tool changed (toolbar click or keyboard shortcut): restyle the toolbar. The tool
    // options bar + Properties tab will also refresh from the active tool here in S11-b.
    void onToolChanged() {
        syncPresetSectionVisibility();
        closeTypePanel();   // its anchor (the bar's "Style…" button) is about to be rebuilt away
        closeType3dPanel(); // same for the 3D popup's anchor
        if (m_toolbar)
            m_toolbar->refresh();
        if (m_optionsBar)
            m_optionsBar->rebuild(); // repopulate the options bar for the new tool
        if (m_canvas) {
            if (m_recomposeReview)
                cancelRecomposeReview(); // the review is crop-modal: leaving the tool drops it
            m_canvas->cancelSelectionGesture(); // don't leave a half-built marquee behind (S14)
            m_canvas->clearMoveTarget();        // ... or stray transform handles (S15)
            m_canvas->cancelBrushStroke();      // ... or a half-painted brush stroke (S19-a)
            m_canvas->cancelShapeGesture();     // ... or a half-dragged shape (S26)
            m_canvas->cancelShapeEdit();        // ... or a shape bound for editing (§7.1; kept when
                                                // the switch is the select-to-edit kind change)
            // S28: leaving the Pen FINISHES an open path (it does not throw it away) -- the same
            // rule Enter/Esc/double-click follow -- and then drops the node-edit binding.
            m_canvas->commitPenPath();
            m_canvas->cancelPenEdit();
            m_canvas->cancelGradientGesture();  // ... or a half-dragged gradient (S22)
            // S35-b: leaving a warp tool -- or switching between its two variants -- DROPS the staged
            // deformation and restores the layer's pre-warp pixels. Nothing was committed, so there is
            // nothing to keep; and a staged warp is a rendered image of the layer, not a cheap rect
            // like the crop's, so keeping one alive across tools would leave the document on screen
            // disagreeing with the document in the command stack. selectLayerForActiveTool re-binds
            // below when the newly active tool is still a warp tool, which is what re-stages the
            // lattice at the other variant's shape.
            m_canvas->cancelWarpSession();
            m_canvas->cancelGradientEdit();     // ... or a gradient bound for editing (re-bound below
                                                // by selectLayerForActiveTool if still the Gradient tool)
            if (m_gradientFlyout != nullptr)
                m_gradientFlyout->hide();       // the flyout's anchor button is about to be rebuilt away
            m_canvas->commitTextEdit(); // ... and commit/leave any text-edit session (S29-b)
            if (m_cropClearSelectionOnLeave && m_tools.previous() == ToolId::Crop &&
                m_tools.active() != ToolId::Crop) {
                m_canvas
                    ->resetCropTool(); // opt-in: drop the staged rect when leaving the Crop tool
            } else {
                m_canvas->cancelCropGesture(); // drop a half-built crop drag but KEEP the staged
                                               // rect, so leaving Crop and returning restores the
                                               // same framing (the re-entry inconsistency).
            }
            m_canvas->ensureCropRect(); // ... staging a ratio-conformed one only if none yet.
            // Selection continuity (user 2026-06-29): carry the panel's active layer into the
            // newly- active tool, AFTER the cleanup above cleared the prior tool's handles/session.
            // So the Move tool frames a layer you were just editing with Type, and switching to
            // Type re-opens editing on a selected text layer -- the selection follows you across
            // the switch.
            if (m_layerPanel != nullptr)
                m_canvas->selectLayerForActiveTool(m_layerPanel->activeLayer());
            // ... and, if that just re-bound a gradient layer, show ITS kind + opacity on the bar
            // rather than the last drag's (S22).
            if (m_tools.active() == ToolId::Gradient)
                syncGradientBarToEditTarget();
        }
        if (const Tool* t = m_tools.activeTool())
            uiLog().debug("active tool: {}", t->name());
    }

    // S16-e: show the Crop tool's ratioW/ratioH Number fields exactly when "Custom" is the chosen
    // Ratio. They live in the option set permanently but start non-primary (hidden from the bar);
    // we flip their `primary` flag on the Free/preset <-> Custom transition. Because this runs from
    // inside the Ratio combo's (or a field's) own FLTK callback, a synchronous rebuild() -- which
    // clear()s and deletes that very widget -- would be a use-after-free; so we defer it to an
    // add_timeout(0) tick. Returns true when a rebuild was scheduled (the caller then skips the
    // value re-sync, which the rebuild subsumes).
    bool refreshCropCustomFields() {
        Tool* crop = m_tools.find(ToolId::Crop);
        if (crop == nullptr || m_tools.active() != ToolId::Crop)
            return false;
        int ratioChoice = 0;
        ToolOption* rw = nullptr;
        ToolOption* rh = nullptr;
        for (ToolOption& o : crop->options()) {
            if (o.id == "ratio")
                ratioChoice = static_cast<int>(o.value);
            else if (o.id == "ratioW")
                rw = &o;
            else if (o.id == "ratioH")
                rh = &o;
        }
        if (rw == nullptr || rh == nullptr)
            return false;
        const bool wantCustom = ratioChoice == kCropRatioCustom;
        if (rw->primary == wantCustom)
            return false; // no visibility change -> a plain value re-sync is enough
        rw->primary = rh->primary = wantCustom;
        Fl::remove_timeout(rebuildOptionsBarCb, this); // coalesce; never rebuild synchronously here
        Fl::add_timeout(0.0, rebuildOptionsBarCb, this);
        return true;
    }

    static void rebuildOptionsBarCb(void* self) {
        auto* win = static_cast<MainWindow*>(self);
        if (win->m_optionsBar != nullptr)
            win->m_optionsBar->rebuild();
    }

    // S16-f rotate x Inpaint guardrail (research doc §3.10 #3): while the staged crop carries a
    // rotation, the Fill combo's Inpaint entry greys out under an honest label — content-aware
    // fill of rotation wedges is US 10,109,093's claimed subject matter, so the two are
    // mutually exclusive by construction. Solid fills stay available (the claim requires
    // content-aware fill). Same deferred-rebuild pattern as the Custom fields above.
    void refreshCropFillModes(bool rotated) {
        if (rotated == m_cropFillInpaintDisabled)
            return;
        m_cropFillInpaintDisabled = rotated;
        Tool* crop = m_tools.find(ToolId::Crop);
        if (crop == nullptr)
            return;
        for (ToolOption& o : crop->options()) {
            if (o.id != "fillMode")
                continue;
            if (o.choices.size() > 5) {
                o.choices[5] = rotated ? _("Inpaint (unavailable when rotated)") : _("Inpaint");
                o.disabledChoices = rotated ? std::vector<int>{5} : std::vector<int>{};
                if (rotated && static_cast<int>(o.value) == 5)
                    o.value = 0.0; // never leave the selection ON the fenced entry
            }
            break;
        }
        if (m_tools.active() == ToolId::Crop) {
            Fl::remove_timeout(rebuildOptionsBarCb, this); // coalesce; never rebuild in-callback
            Fl::add_timeout(0.0, rebuildOptionsBarCb, this);
        }
    }
    bool m_cropFillInpaintDisabled = false; // last-applied greyed state (edge-gates rebuilds)
    bool m_cropStagedExpands = false;       // staged rect reaches outside the canvas
    bool m_cropFillVisible = false;         // last-applied Fill-combo visibility

    // Show the Fill combo exactly while a fill is meaningful — the staged box expands or is
    // rotated (wedges). Keeps the crowded crop bar honest (user 2026-07-02): plain crops never
    // see it. Same flip-primary + deferred-rebuild pattern as the Custom ratio fields.
    void refreshCropFillVisibility() {
        const bool want = m_cropStagedExpands || m_cropFillInpaintDisabled;
        if (want == m_cropFillVisible)
            return;
        m_cropFillVisible = want;
        Tool* crop = m_tools.find(ToolId::Crop);
        if (crop == nullptr)
            return;
        for (ToolOption& o : crop->options()) {
            if (o.id != "fillMode")
                continue;
            o.primary = want;
            break;
        }
        if (m_tools.active() == ToolId::Crop) {
            Fl::remove_timeout(rebuildOptionsBarCb, this);
            Fl::add_timeout(0.0, rebuildOptionsBarCb, this);
        }
    }

    // Show the Crop bar's "Recompose" button exactly while Smart Resize is ON (plan §1.3) — the
    // same flip-primary + deferred-rebuild trick as the Custom ratio fields above. Returns true
    // when a rebuild was scheduled.
    bool refreshRecomposeButton() {
        Tool* crop = m_tools.find(ToolId::Crop);
        if (crop == nullptr || m_tools.active() != ToolId::Crop)
            return false;
        bool smart = false;
        ToolOption* btn = nullptr;
        for (ToolOption& o : crop->options()) {
            if (o.id == "smartResize")
                smart = o.value != 0.0;
            else if (o.id == "recompose")
                btn = &o;
        }
        if (btn == nullptr || btn->primary == smart)
            return false;
        btn->primary = smart;
        if (!smart)
            btn->enabled = false; // rests greyed next time it shows (the offer re-enables)
        Fl::remove_timeout(rebuildOptionsBarCb, this); // coalesce with the Custom-fields rebuild
        Fl::add_timeout(0.0, rebuildOptionsBarCb, this);
        return true;
    }

    // A selection tool finished its gesture (S14): land the mask as the gesture's single
    // undoable command and re-sync the ants + status-bar bounds. (The live preview already
    // showed this exact result -- the canvas mask just becomes official.) `coalesce` is non-zero
    // only for an S16-i arrow-key nudge burst, which collapses to one History step.
    void commitToolSelection(core::Selection sel, std::uint64_t coalesce = 0,
                             std::string_view label = "Select") {
        if (!m_document)
            return;
        m_document->commands().push(std::make_unique<core::SetSelectionCommand>(
            std::move(sel), coalesce, std::string(label)));
        syncSelection();
    }

    // A magic-wand click (S17, docs/research-selection.md §8): read the options, resolve the
    // document-space source (active layer / merged composite), flood, combine with the current
    // selection by the press-time boolean op, and land ONE SetSelectionCommand -- the identical
    // canvas->host commit funnel the marquee/lasso use.
    void magicWandClick(common::Vec2 docPt, core::SelectOp op) {
        if (!m_document)
            return;

        // Options -> WandParams. The 0-100 tolerance slider maps linearly onto the metric's [0,1].
        core::WandParams params;
        int source = 0; // 0 = Active Layer, 1 = All Layers (mirrors the Eyedropper's Source)
        if (const Tool* t = m_tools.find(ToolId::MagicWand))
            for (const ToolOption& o : t->options()) {
                if (o.id == "tolerance")
                    params.tolerance = std::clamp(o.value / 100.0, 0.0, 1.0);
                else if (o.id == "contiguous")
                    params.contiguous = o.value > 0.5;
                else if (o.id == "antialias")
                    params.antialias = o.value > 0.5;
                else if (o.id == "source")
                    source = static_cast<int>(o.value);
            }

        // Resolve the document-space image the flood reads. Current, not AnyRecent: this is a click
        // that lands an undoable selection, so it must see every edit committed before it -- once
        // per click is exactly the cadence a blocking read is for (audit A3).
        common::Image scratch; // holds a resampled active layer (the merged source memoises itself)
        const common::Image* src =
            source == 1
                ? wandMergedSource(render::consumers::kMagicWand, render::Freshness::Current)
                : activeLayerDocImage(scratch);
        if (src == nullptr || src->empty()) {
            // A silent no-op reads as a broken tool. The common cause is a non-pixel active layer
            // under "Active Layer" -- name the way out (like the brush's unpaintable hint).
            if (source == 0)
                transientStatus(_("The active layer has no pixels to sample. Rasterize it, or set "
                                  "the Magic Wand's Source to All Layers."));
            return;
        }

        const int seedX = static_cast<int>(std::floor(docPt.x));
        const int seedY = static_cast<int>(std::floor(docPt.y));
        const core::Selection wand = core::magicWandSelection(*src, seedX, seedY, params);
        core::Selection combined = core::Selection::combine(m_document->selection(), wand, op);
        if (!combined.anySelected())
            combined = core::Selection{}; // land "no selection", not an active selection of nothing
        commitToolSelection(std::move(combined), 0, _("Magic Wand"));
    }

    // The edge brush's release-time grow (L1): resolve the
    // document-space source per the tool's Source option (the wand's Active-Layer / All-Layers
    // pattern), map the bar's Reach/Edge Stop onto core::EdgeGrowParams, and run the ONE
    // edge-stopped geodesic solve. The canvas owns the combine + the single SetSelectionCommand;
    // an empty return tells it nothing grew (no source -> we hint, like the wand does).
    core::Selection edgeBrushGrow(const core::Selection& seeds) {
        if (!m_document)
            return {};
        core::EdgeGrowParams params;
        int source = 0; // 0 = Active Layer, 1 = All Layers (mirrors the wand's Source)
        if (const Tool* t = m_tools.find(ToolId::EdgeBrush))
            for (const ToolOption& o : t->options()) {
                if (o.id == "reach")
                    params.reach = o.value;
                else if (o.id == "edgeStop")
                    params.edgeStop = std::clamp(o.value / 100.0, 0.0, 1.0);
                else if (o.id == "source")
                    source = static_cast<int>(o.value);
            }
        // Current for the same reason as the wand (A4): once per stroke, on RELEASE, landing one
        // SetSelectionCommand -- a discrete act with an undo entry, not a live readout.
        common::Image scratch; // holds a resampled active layer (the merged source memoises itself)
        const common::Image* src =
            source == 1
                ? wandMergedSource(render::consumers::kEdgeBrush, render::Freshness::Current)
                : activeLayerDocImage(scratch);
        if (src == nullptr || src->empty()) {
            if (source == 0)
                transientStatus(_("The active layer has no pixels to read edges from. Rasterize "
                                  "it, or set the Edge Select Brush's Source to All Layers."));
            return {};
        }
        return core::edgeGrowSelection(*src, seeds, params);
    }

    // The eye retouch (S38-b, docs/red-eye-tool.md §3.1/§3.2): the canvas hands back the scope the
    // user painted; we own the active layer, the document-selection clip (§2.4), the colour math
    // and the ONE region-scoped SetLayerPixelsCommand a gesture is worth. Nothing here scans the
    // image for an eye -- the correction only ever touches what the ring covered.
    void redEyeApply(core::RedEyeMode mode, const core::Selection& scope,
                     const ui::RedEyeOptions& opts) {
        if (!m_document)
            return;
        core::Layer* layer = activeLayerPtr();
        if (layer == nullptr)
            return;
        if (layer->locked()) {
            transientStatus(_("The layer is locked — unlock it to retouch it"));
            return;
        }
        if (layer->as<core::RasterLayer>() == nullptr) {
            transientStatus(_("Can’t retouch a vector layer — rasterize it first"));
            return;
        }
        // §2.4: the document's own selection scopes this exactly as it scopes a brush stroke, and
        // the result is then carried onto the layer's own pixel grid (a rotated layer included).
        const core::Selection onLayer =
            ui::redEyeScopeOnLayer(*layer, ui::redEyeScope(scope, m_document->selection()));
        if (onLayer.isEmpty())
            return;
        core::RedEyeParams params = ui::redEyeParams(opts);
        params.mode = mode;
        const auto* raster = layer->as<core::RasterLayer>();
        core::RetouchPatch patch = core::retouchEye(raster->image(), onLayer, params);
        if (patch.empty()) {
            // A scope with nothing red enough in it is a legitimate outcome, not a failure: say so
            // and push NO undo step, rather than leaving a no-op "Edit Pixels" in History.
            transientStatus(mode == core::RedEyeMode::Flash
                                ? _("No red-eye glow found inside the ring")
                                : _("Nothing red enough to correct where you painted"));
            return;
        }
        pushScopedPixelEdit(std::make_unique<core::SetLayerPixelsCommand>(
            layer->id(), std::move(patch.pixels), patch.originX, patch.originY));
    }

    // S38 Clone stamp (docs/clone-stamp.md §4): the DOCUMENT-SPACE pixels a stroke samples when the
    // tool is set to something other than "Current layer". Taken ONCE, at the press, so the source
    // is a still picture for the stroke's whole life -- which is the same pre-stroke-snapshot rule
    // the deposit itself obeys, and the reason a clone stroke can safely sample a composite that
    // includes the very layer it is painting on.
    //
    // "Current & below" is the composite with everything that draws ABOVE the active layer
    // temporarily hidden -- every sibling above it, and every sibling above each of its ancestor
    // groups. The active layer's own ancestors stay visible, so a layer inside a TRANSFORMED GROUP
    // clones through that group's transform, opacity, blend mode and mask: "below" is read in the
    // finished document, which is the only place the words mean anything once a group can move its
    // children. (Photoshop's own answer, and the only one that survives a rotated group.)
    common::Image cloneSampleSnapshot(bool belowOnly) {
        if (!m_document)
            return {};
        render::CompositeOptions opts;
        opts.checkerboard = false; // real alpha: a transparent source must clone as transparent
        opts.resampleFilter = currentResampleFilter();
        // ⚠ The resident lane (S60-a item 13) takes this shortcut AWAY on purpose. m_lastComposite
        // is then a lazily materialised mirror that may be several revisions behind, and a clone
        // source is the one thing that must not be silently one edit old. The clone stamp is not in
        // `render::consumers`, so it cannot name itself at the readback seam yet -- until it can,
        // it pays the honest CPU walk below.
        // ... and `m_hostCompositeStale` covers the frame AFTER the lane refuses, where serving()
        // already reads false but the mirror is still whatever the last readback left behind.
        const bool residentOwnsComposite =
            m_tiles != nullptr && (m_tiles->serving() || m_hostCompositeStale);
        if (!belowOnly && !residentOwnsComposite && !m_lastComposite.empty() &&
            m_lastComposite.width == m_document->width() &&
            m_lastComposite.height == m_document->height()) {
            // "All layers" is the picture already on screen, and the recomposite flow keeps it
            // current -- so COPY it rather than paying a full CPU walk at every press (that walk is
            // the named gesture-START stall, docs/s60-gesture-start-stall.md, and it is over a
            // second at 5000x8000). It must be a copy and not a reference: the stroke's own live
            // preview patches m_lastComposite region by region, and a source that changed under a
            // stroke would stop being a snapshot at all.
            return m_lastComposite;
        }
        // The visibility flips are UNDONE before this returns, on every path -- nothing between the
        // hide and the restore reads the document but the composite call itself, and nothing
        // observes core::Layer::setVisible.
        std::vector<core::Layer*> hidden;
        if (belowOnly && m_layerPanel != nullptr)
            hideLayersAbove(m_document->root(), m_layerPanel->activeLayer(), hidden);
        render::CompositeResult r = render::composite(*m_document, opts, render::Backend::Cpu);
        for (core::Layer* l : hidden)
            l->setVisible(true);
        if (!r.ok)
            return {};
        return std::move(r.image);
    }

    // Hide every layer that composites ABOVE `target`, recording what was flipped so the caller can
    // put it back. Returns true once `target` has been found in this subtree, which is what makes
    // the ancestors' siblings above it fall into the "above" half too. Children are bottom(0)->top.
    static bool hideLayersAbove(core::GroupLayer& group, core::LayerId target,
                                std::vector<core::Layer*>& hidden) {
        bool found = false;
        for (std::size_t i = 0; i < group.childCount(); ++i) {
            core::Layer& child = group.child(i);
            if (found) {
                if (child.visible()) {
                    child.setVisible(false);
                    hidden.push_back(&child);
                }
                continue;
            }
            if (child.id() == target) {
                found = true; // the target itself stays visible: "current AND below"
                continue;
            }
            if (auto* sub = child.as<core::GroupLayer>();
                sub != nullptr && hideLayersAbove(*sub, target, hidden))
                found = true;
        }
        return found;
    }

    // A bucket-fill click (S21, docs/bucket-fill.md): flood the active raster layer from the seed,
    // intersect with the active selection, and land ONE core::FillCommand of the foreground
    // colour -- the same commit funnel Edit ▸ Fill uses (render::computeFill + FillCommand), so
    // History reads "Fill" once. Fill targets the current selection (whole layer if none) and never
    // touches a non-raster / locked layer. Pattern/gradient fills stay in Edit ▸ Fill (S39); the
    // bucket paints the active foreground.
    void bucketFillClick(common::Vec2 docPt) {
        core::Layer* layer = activeLayerPtr();
        auto* raster = layer != nullptr ? layer->as<core::RasterLayer>() : nullptr;
        if (raster == nullptr || raster->image().empty() || !m_document) {
            // A silent no-op reads as a broken tool -- name the way out (like the wand's hint).
            transientStatus(_("The active layer has no pixels to fill. Rasterize it first."));
            return;
        }
        if (layer->locked()) {
            transientStatus(_("This layer is locked. Unlock it to fill."));
            return;
        }
        const common::Image& src = raster->image(); // layer-local pixels (what FillCommand patches)

        // Map the click into the layer's own pixel space (inverse of the layer transform), so the
        // flood + the FillCommand region share the raster's coordinates.
        const common::Affine2D t = core::worldTransform(*layer);
        const bool identity = t == common::Affine2D::identity();
        common::Vec2 lp = docPt;
        if (!identity) {
            const auto inv = t.inverse();
            if (!inv)
                return; // singular transform -> nothing to fill
            lp = inv->apply(docPt);
        }
        const int seedX = static_cast<int>(std::floor(lp.x));
        const int seedY = static_cast<int>(std::floor(lp.y));
        if (seedX < 0 || seedY < 0 || seedX >= static_cast<int>(src.width) ||
            seedY >= static_cast<int>(src.height))
            return; // clicked outside the layer's pixels

        // Options -> FillParams. The 0-255 tolerance slider maps onto the wand metric's [0,1].
        core::FillParams params;
        float opacity = 1.0f;
        if (const Tool* tool = m_tools.find(ToolId::BucketFill))
            for (const ToolOption& o : tool->options()) {
                if (o.id == "tolerance")
                    params.tolerance = std::clamp(o.value / 255.0, 0.0, 1.0);
                else if (o.id == "contiguous")
                    params.contiguous = o.value > 0.5;
                else if (o.id == "antialias")
                    params.antialias = o.value > 0.5;
                else if (o.id == "opacity")
                    opacity = static_cast<float>(std::clamp(o.value / 100.0, 0.0, 1.0));
            }

        std::vector<std::uint8_t> flood = core::bucketFillCoverage(src, seedX, seedY, params);
        if (flood.empty())
            return; // nothing floods (bad seed / coverage-free result)

        // Intersect the flood with the active selection (mapped into layer-pixel space like
        // buildFillContext) and find the covered bbox in one pass.
        const core::Selection& sel = m_document->selection();
        const bool haveSel = !sel.isEmpty();
        long minx = src.width, miny = src.height, maxx = -1, maxy = -1;
        for (std::uint32_t y = 0; y < src.height; ++y)
            for (std::uint32_t x = 0; x < src.width; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * src.width + x;
                int c = flood[i];
                if (c > 0 && haveSel) {
                    common::Vec2 d{x + 0.5, y + 0.5};
                    if (!identity)
                        d = t.apply(d);
                    const long dx = static_cast<long>(std::floor(d.x));
                    const long dy = static_cast<long>(std::floor(d.y));
                    const int s = (dx < 0 || dy < 0) ? 0
                                                     : sel.at(static_cast<std::uint32_t>(dx),
                                                              static_cast<std::uint32_t>(dy));
                    c = static_cast<int>(std::lround(c * (s / 255.0)));
                    flood[i] = static_cast<std::uint8_t>(c);
                }
                if (c > 0) {
                    minx = std::min(minx, static_cast<long>(x));
                    miny = std::min(miny, static_cast<long>(y));
                    maxx = std::max(maxx, static_cast<long>(x));
                    maxy = std::max(maxy, static_cast<long>(y));
                }
            }
        if (maxx < 0)
            return; // the flood misses the selection entirely -> nothing to fill

        // Crop the region + its coverage to the bbox and produce the filled pixels via the shared
        // S39 fill core, then commit one FillCommand at the region's layer-local origin.
        const auto rw = static_cast<std::uint32_t>(maxx - minx + 1);
        const auto rh = static_cast<std::uint32_t>(maxy - miny + 1);
        common::Image region = common::copyRegion(src, minx, miny, rw, rh);
        std::vector<std::uint8_t> coverage(static_cast<std::size_t>(rw) * rh, 0);
        for (std::uint32_t yy = 0; yy < rh; ++yy)
            for (std::uint32_t xx = 0; xx < rw; ++xx)
                coverage[static_cast<std::size_t>(yy) * rw + xx] =
                    flood[static_cast<std::size_t>(miny + yy) * src.width + (minx + xx)];

        common::Image filled =
            render::computeFill(region, coverage, m_colors.foreground(), core::BlendMode::Normal,
                                opacity, /*protectAlpha=*/false);
        pushScopedPixelEdit(
            std::make_unique<core::FillCommand>(layer->id(), std::move(filled), minx, miny));
    }

    // The eyedropper's colour resolver (S24): read the tool options (Source = active layer / merged
    // composite, and the sample-size average), resolve the document-space source image the SAME way
    // the Magic Wand does (activeLayerDocImage / wandMergedSource -- shared, so what the two tools'
    // "Source" values MEAN can never drift; their defaults differ deliberately, see tool.cpp), and
    // average the pixel(s) under `docPt` with core::sampleColor. Returns nullopt when there is
    // nothing to pick: no document, the pointer is off the pixels, or a non-raster active layer
    // under "Active Layer" (which owns no pixels of its own).
    //
    // ⚠ It reads the EYEDROPPER's options whichever tool is active, and that is the whole wiring
    // of the temporary loupe: Ctrl over a stroke tool (VulkanCanvas::temporaryEyedropperActive)
    // borrows the eyedropper rather than owning a second set of settings, so Source/Sample mean
    // exactly the same thing there. Both the live loupe readout and the commit come through here --
    // which is why `freshness` is the caller's to declare; see setEyedropperHost.
    std::optional<common::Color8> eyedropperSample(common::Vec2 docPt,
                                                   render::Freshness freshness) {
        if (!m_document)
            return std::nullopt;
        int sourceOpt = 1; // 0 = Active Layer, 1 = All Layers (the tool's default -- see tool.cpp)
        core::SampleSize size = core::SampleSize::Point;
        if (const Tool* t = m_tools.find(ToolId::Eyedropper))
            for (const ToolOption& o : t->options()) {
                if (o.id == "source")
                    sourceOpt = static_cast<int>(o.value);
                else if (o.id == "sample")
                    size = core::sampleSizeFromIndex(static_cast<int>(o.value));
            }
        common::Image scratch;
        const common::Image* src =
            sourceOpt == 1 ? wandMergedSource(render::consumers::kEyedropper, freshness)
                           : activeLayerDocImage(scratch);
        if (src == nullptr || src->empty())
            return std::nullopt;
        const int px = static_cast<int>(std::floor(docPt.x));
        const int py = static_cast<int>(std::floor(docPt.y));
        return core::sampleColor(*src, px, py, size);
    }

    // The merged (checkerboard-free) composite in document space -- the wand's "All Layers" source,
    // shared by the Magic Wand (audit A3), the Edge Select Brush (A4) and the Eyedropper (A2), each
    // of which names ITSELF and declares its own `freshness`: the seam's registry is per consumer,
    // not per helper, and so is the staleness claim. m_lastComposite is normally kept current by
    // the recomposite flow (or, under the resident lane, materialised from the device by
    // hostComposite) and is then handed back BY REFERENCE -- no copy, no cache, the ordinary path.
    //
    // The fallback is the half that bites. When the mirror is empty or the wrong size there is
    // nothing to read and this walks the whole document on the CPU -- and it used to do that on
    // EVERY CALL, into a caller-owned scratch that was thrown away and rebuilt next time. The
    // eyedropper calls per FRAME while the loupe is up (VulkanCanvas::syncLoupe), so with Source =
    // All Layers over a mirror that will not serve -- the resident lane owns the canvas and no CPU
    // walk has ever filled m_lastComposite, or its readback failed -- that was a full composite of
    // a possibly 5000x8000 document per hovered frame. Audit finding F4
    // (docs/s60-readback-consumers.md §8), and the reason it was the #1 dangerous consumer in §4.
    //
    // Memoised on m_compositeRevision, exactly as ensureSmartAnalysis memoises the importance map:
    // a hover with no edits pays for ONE composite and then nothing, and any mutation of the
    // composite moves the revision and releases the pixels (bumpCompositeRevision). A VIEW change
    // does not -- this image is document space, and pan/zoom/rotate touch neither the revision nor
    // the document.
    //
    // ⚠ The memo is refused outright while a recomposite is QUEUED and not yet drained. That is the
    // window where the document has already changed and the revision has not caught up, and the
    // wand floods an UNDOABLE selection off these pixels: a one-frame-old answer that is invisible
    // in a live readout is a wrong selection there. nullptr on a composite failure.
    const common::Image* wandMergedSource(std::string_view consumer, render::Freshness freshness) {
        const common::Image& flat = hostComposite(consumer, freshness);
        const bool stale = flat.empty() || flat.width != m_document->width() ||
                           flat.height != m_document->height();
        if (!stale)
            return &flat;
        const bool queued = m_recompositePending || m_pendingRegion.queued;
        if (!queued && m_mergedSourceRev == m_compositeRevision &&
            m_mergedSource.width == m_document->width() &&
            m_mergedSource.height == m_document->height())
            return &m_mergedSource;
        render::CompositeOptions opts;
        opts.checkerboard = false;
        render::CompositeResult r = render::composite(*m_document, opts, render::Backend::Cpu);
        if (!r.ok)
            return nullptr;
        m_mergedSource = std::move(r.image);
        // A walk made while an edit is still queued is transient -- superseded the moment the queue
        // drains -- so don't claim currency for it (ensureSmartAnalysis refuses to stamp its own
        // stale-composite fallback for the same reason). 0 never equals a live revision.
        m_mergedSourceRev = queued ? 0 : m_compositeRevision;
        return &m_mergedSource;
    }

    // The active layer's pixels in document space -- the wand's "Active Layer" source. A document-
    // sized, untransformed raster/magic layer is returned by reference (no copy, the common case); a
    // transformed one is inverse-sampled (nearest) into `scratch`, exactly like selectionFromLayer-
    // Pixels. A non-raster active layer (group/vector/text/adjustment/texture) owns no pixels -> null.
    const common::Image* activeLayerDocImage(common::Image& scratch) {
        core::Layer* layer = m_layerPanel ? m_document->find(m_layerPanel->activeLayer()) : nullptr;
        if (layer == nullptr)
            return nullptr;
        const common::Image* pixels = nullptr;
        if (const auto* raster = layer->as<core::RasterLayer>())
            pixels = &raster->image();
        else if (const auto* magic = layer->as<core::MagicLayer>())
            pixels = &magic->source();
        if (pixels == nullptr || pixels->empty())
            return nullptr;
        const std::uint32_t docW = m_document->width();
        const std::uint32_t docH = m_document->height();
        const common::Affine2D t = core::worldTransform(*layer);
        if (t == common::Affine2D::identity() && pixels->width == docW && pixels->height == docH)
            return pixels; // 1:1 fast path -- the source is already document space
        const auto inv = t.inverse();
        if (!inv)
            return nullptr; // singular transform collapses to nothing (like the leaf walk)
        scratch = common::Image(docW, docH);
        for (std::uint32_t y = 0; y < docH; ++y)
            for (std::uint32_t x = 0; x < docW; ++x) {
                const common::Vec2 p = inv->apply({x + 0.5, y + 0.5});
                const long sx = static_cast<long>(std::floor(p.x));
                const long sy = static_cast<long>(std::floor(p.y));
                if (sx >= 0 && sy >= 0 && sx < static_cast<long>(pixels->width) &&
                    sy < static_cast<long>(pixels->height)) {
                    const std::size_t si = (static_cast<std::size_t>(sy) * pixels->width + sx) * 4;
                    const std::size_t di = (static_cast<std::size_t>(y) * docW + x) * 4;
                    scratch.rgba[di + 0] = pixels->rgba[si + 0];
                    scratch.rgba[di + 1] = pixels->rgba[si + 1];
                    scratch.rgba[di + 2] = pixels->rgba[si + 2];
                    scratch.rgba[di + 3] = pixels->rgba[si + 3];
                }
            }
        return &scratch;
    }

    // The Crop tool applied its staged rect (S16): land it as the single undoable "Crop" step
    // (canvas resize + per-layer rebase; "Delete Cropped" bakes top-level rasters -- see
    // render::buildCropCommand). A full-canvas rect is a no-op, not an empty undo entry.
    void applyCrop(const common::Rect& rect, double angle, common::Vec2 pivot) {
        if (!m_document)
            return;
        const CropPixels px = snapCropRect(rect, m_document->width(), m_document->height());
        if (px.w == 0 || px.h == 0)
            return;
        if (angle == 0.0 && px.x == 0 && px.y == 0 && px.w == m_document->width() &&
            px.h == m_document->height())
            return;
        bool deletePixels = true;
        int fillMode = 0;
        if (const Tool* t = m_tools.find(ToolId::Crop))
            for (const ToolOption& o : t->options()) {
                if (o.id == "delete")
                    deletePixels = o.value != 0.0;
                else if (o.id == "fillMode")
                    fillMode = static_cast<int>(o.value);
            }
        // S16-f: a rect past the canvas stages an EXPANSION; the Fill combo colours the added
        // area (Transparent = index 0 = no fill sub-command at all). A rotated crop always has
        // fill portions (the wedges).
        const bool expands =
            angle != 0.0 || px.x < 0 || px.y < 0 ||
            px.x + static_cast<long>(px.w) > static_cast<long>(m_document->width()) ||
            px.y + static_cast<long>(px.h) > static_cast<long>(m_document->height());
        if (fillMode == 5 && angle != 0.0) {
            // Cannot happen through the UI (the entry greys while rotated — §3.10 guardrail 3);
            // defensive so no code path ever content-aware-fills rotation wedges.
            fillMode = 0;
            transientStatus(_("Inpaint fill is not available on a rotated crop"));
        }
        if (fillMode == 5 && expands) {
            // Inpaint fill: the engine run takes seconds, so it goes through an async job and
            // the command lands as the same ONE undo step when the worker finishes. Guardrails
            // (research doc §3.10): a persistent pre-chosen mode + an explicit Apply — never a
            // post-crop chooser presenting operation previews.
            startCropExpandFill(px, deletePixels);
            return;
        }
        std::optional<render::CropFill> fill;
        switch (fillMode) {
        case 1:
            fill = render::CropFill{{255, 255, 255, 255}, _("Canvas fill")};
            break;
        case 2:
            fill = render::CropFill{{0, 0, 0, 255}, _("Canvas fill")};
            break;
        case 3:
            fill = render::CropFill{m_colors.foreground(), _("Canvas fill")};
            break;
        case 4:
            fill = render::CropFill{m_colors.background(), _("Canvas fill")};
            break;
        default:
            break;
        }
        m_document->commands().push(render::buildCropCommand(*m_document, px.x, px.y, px.w, px.h,
                                                             deletePixels, fill, angle, pivot));
        syncAfterEdit();
        char buf[128];
        std::snprintf(buf, sizeof buf,
                      expands ? _("Resized canvas to %u × %u px") : _("Cropped to %u × %u px"),
                      px.w, px.h);
        transientStatus(buf);
        // S16-p (opt-in): leave the Crop tool after Apply, returning to the previously-active tool
        // (or Move if Crop was the first/only tool), so a one-off crop doesn't strand the user in
        // Crop.
        if (m_cropSwitchToolAfterApply && m_tools.active() == ToolId::Crop) {
            const ToolId back =
                m_tools.previous() == ToolId::Crop ? ToolId::Move : m_tools.previous();
            m_tools.setActive(back);
        }
    }

    // Smart Resize (S16-f): (re)build the cached analysis — the ONE fused importance map plus
    // the automatic keep-regions extracted from it — from the on-canvas composite (kept current
    // by the recomposite flow; carries true alpha), falling back to a fresh CPU composite only
    // when it is stale. Keyed on m_compositeRevision so ratio changes / chip toggles between
    // edits reuse the map instead of re-walking the whole document. False = no analysis possible.
    bool ensureSmartAnalysis() {
        // Flagged by the readback audit as per-frame, full-canvas and unmemoized on its stale path.
        MOSAIC_PERF_SCOPE("Smart Resize analysis", Lane::Cpu);
        if (!m_document)
            return false;
        // Audit A7: staleness-tolerant and already memoised on the composite revision, so it asks
        // for AnyRecent and never for a fence (S60-a item 12).
        const common::Image& flat =
            hostComposite(render::consumers::kSmartResize, render::Freshness::AnyRecent);
        const bool stale = flat.empty() || flat.width != m_document->width() ||
                           flat.height != m_document->height();
        if (!stale && m_smartMapRev == m_compositeRevision && !m_smartMap.empty())
            return true;
        common::Image fresh;
        const common::Image* src = &flat;
        if (stale) {
            render::CompositeOptions opts;
            opts.checkerboard = false;
            render::CompositeResult r = render::composite(*m_document, opts, render::Backend::Cpu);
            if (!r.ok)
                return false;
            fresh = std::move(r.image);
            src = &fresh;
        }
        m_smartMap = core::retarget::buildImportanceMap(*src);
        m_smartRegionRects.clear();
        for (const core::retarget::KeepRegion& k : core::retarget::extractKeepRegions(m_smartMap))
            m_smartRegionRects.push_back(k.rect);
        // A stale-composite fallback is transient (mid-resize): don't mark the cache current.
        m_smartMapRev = stale ? m_smartMapRev : m_compositeRevision;
        return !m_smartMap.empty();
    }

    // The Crop tool asked for the suggested window at `targetAspect` (<= 0 = Free = smart trim);
    // `protects` are the enabled keep-region chips (never sliced), `excludes` the toggled-off
    // ones (actively ignored: their mass is masked out). One map, one suggestion (§3.8).
    [[nodiscard]] std::optional<common::Rect>
    smartCropSuggestion(double targetAspect, const std::vector<common::Rect>& protects,
                        const std::vector<common::Rect>& excludes) {
        if (!ensureSmartAnalysis())
            return std::nullopt;
        core::retarget::SmartCropOptions opts;
        opts.protectRects = protects;
        opts.excludeRects = excludes;
        const common::Rect r = core::retarget::chooseCropWindow(m_smartMap, targetAspect, opts);
        if (r.empty())
            return std::nullopt;
        return r;
    }

    // The staged crop rect changed (S16): the selection-bounds slot doubles as the live crop
    // size readout while a rect is staged; clearing restores the actual selection's bounds.
    void onCropRectChanged(const std::optional<common::Rect>& r) {
        // S16-f: the Fill combo appears exactly while the staged rect reaches outside.
        m_cropStagedExpands =
            r && m_document &&
            (r->x < 0.0 || r->y < 0.0 || r->right() > static_cast<double>(m_document->width()) ||
             r->bottom() > static_cast<double>(m_document->height()));
        refreshCropFillVisibility();
        if (m_statusBar == nullptr || !m_document)
            return;
        if (r) {
            const CropPixels px = snapCropRect(*r, m_document->width(), m_document->height());
            m_statusBar->setSelectionBounds(
                common::Rect{static_cast<double>(px.x), static_cast<double>(px.y),
                             static_cast<double>(px.w), static_cast<double>(px.h)});
        } else {
            m_statusBar->setSelectionBounds(m_document->selection().bounds());
        }
    }

    // Cancel every in-flight canvas interaction: no gesture, text session, or GPU drag may
    // straddle a document swap (or the swap to no document at all).
    void cancelCanvasInteractions() {
        if (!m_canvas)
            return;
        m_canvas->cancelSelectionGesture();
        m_canvas->clearMoveTarget();
        m_canvas->resetCropTool();
        m_canvas->cancelBrushStroke();
        m_canvas->clearCloneSource(); // S38: an anchor is a point in a document that is going away
        m_canvas->cancelWarpSession(); // S35-b: a lattice describes a layer that is going away too
        m_canvas->cancelShapeGesture();
        m_canvas->cancelPenGesture(); // S28: a half-drawn path is DROPPED across a document swap
        m_canvas->cancelPenEdit();    //      (unlike a tool switch, which finishes it)
        m_canvas->cancelGradientGesture(); // S22: no gradient drag/preview across a document swap
        m_canvas->cancelGradientEdit();
        if (m_gradientFlyout != nullptr)
            m_gradientFlyout->hide();
        m_canvas->commitTextEdit(); // a text session must not straddle two documents (S29-b)
        if (m_gpuDragActive) {      // never carry a GPU drag across documents (S60-a)
            m_canvas->endGpuDrag();
            m_gpuDragActive = false;
        }
        m_gpuDragTried = false;
    }

    // Menu items that need an open document are hidden while none is open. What survives:
    //   - File, keeping just New / Open / Open Recent / Quit;
    //   - Help (About), and Debug where that title exists;
    //   - Edit, reduced to Settings alone -- except on macOS (see kSettingsLivesInEditMenu).
    // Everything else -- the whole Image / Layer / Type / Select / Filter / View menus, the rest of
    // Edit, plus File's Open-as-Layer / Save / Save As / Export As / Quick Export / Close -- comes
    // back when a document opens. Wired to the document 0<->1 boundary (adoptActiveDocument
    // restores, clearDocument hides).
    // FLTK's Fl_Menu_Item::hide()/show() flip FL_MENU_INVISIBLE; our themed MenuBar skips invisible
    // titles + rows (see menu_bar.cpp), so we just redraw the bar afterwards.
    // Whether reaching Settings means opening the Edit menu. On macOS it does not: the platform
    // convention puts Preferences in the application menu (⌘, from anywhere), so surfacing a
    // one-item Edit menu on an empty window would be the wrong shape there -- Edit simply stays
    // hidden until a document opens, like every other document menu.
#ifdef __APPLE__
    static constexpr bool kSettingsLivesInEditMenu = false;
#else
    static constexpr bool kSettingsLivesInEditMenu = true;
#endif

    void setDocumentMenusVisible(bool visible) {
        if (m_menu == nullptr)
            return;
        auto* items = const_cast<Fl_Menu_Item*>(m_menu->menu());
        if (items == nullptr)
            return;
        // The walk itself lives in ui/menu_visibility.cpp so it can be tested headlessly -- a
        // wrong answer here is a subtly wrong menu bar, which nothing catches until someone opens
        // the menu. Menus are named by the callbacks they contain, never by position.
        mosaic::ui::DocumentMenuMarkers markers;
        markers.fileNew = cbFileNew;
        markers.fileOpen = cbFileOpen;
        markers.quit = cbQuit;
        markers.about = cbAbout;
        markers.settings = cbSettings;
        markers.settingsLivesInEditMenu = kSettingsLivesInEditMenu;
#if defined(MOSAIC_DEBUG)
        markers.debugTool = cbTimingGraph; // the Debug title, present in debug builds
#else
        // Release only grows a Debug title under --profile; without it the marker stays null and
        // simply matches nothing.
        markers.debugTool = Profiler::enabled() ? cbTimingGraph : nullptr;
#endif
        mosaic::ui::setDocumentMenusVisible(items, markers, visible);
        refreshMenuBar();
    }

    // Publish item-array edits -- visibility, check marks, inserted/removed rows -- to whatever is
    // showing the menu. The in-window bar reads the array live, so a redraw suffices; the macOS
    // system menu is a snapshot FLTK has to rebuild, which MenuBar::update() does (and where it
    // re-attaches the item badges). update() is inert off macOS, so both calls are unconditional.
    void refreshMenuBar() {
        if (m_menu == nullptr)
            return;
        m_menu->update();
        m_menu->redraw();
    }

    // Drop to the no-document state (the S50-prerequisite startup face): the canvas shows
    // nothing (pushing an empty image tears the renderer's texture down), the panels blank, and
    // the canvas's idle pass invites a click / file drop. Every document-consuming path already
    // guards !m_document, so the rest of the app simply goes inert.
    void clearDocument() {
        cancelCanvasInteractions();
        discardJournal(); // closing to the empty state ends the session cleanly (no restore)
        releaseLock();
        m_documentReadOnly = false;
        m_activeSession = 0; // no sessions: the index means nothing, but keep it in range
        // ⚠ DETACH EVERY DEPENDENT BEFORE DESTROYING THE DOCUMENT. The dock panels cache a RAW
        // core::Document* (LayerPanel::setDocument fans out to History and Channels), so any of
        // them touched between the reset and the detach is holding a dangling pointer -- and they
        // ARE touched: presentComposite below drives a selection sync whose callback lands in
        // LayerPanel::syncProperties -> syncActionButtons -> Document::find. That crashed on tab
        // close (SIGSEGV, release build, 2026-07-23, three cores in one session).
        //
        // A null check cannot fix this and would hide it: m_doc is not null there, it is stale.
        // Order is the only correct fix -- nothing may outlive the document holding a pointer to
        // it. Keep the detach FIRST if this function grows.
        if (m_layerPanel)
            m_layerPanel->setDocument(nullptr);
        m_document.reset();
        m_dragCache.invalidate();
        m_syncedSelectionRev = std::numeric_limits<std::uint64_t>::max();
        m_unsavedSince.reset();
        m_exportTarget.clear();
        m_lastComposite = common::Image{}; // stale pixels must not linger behind the empty state
        m_mergedSource = common::Image{};  // ... and neither may its "All Layers" fallback memo
        m_mergedSourceRev = 0;
        if (m_tiles != nullptr)
            m_tiles->noteDocumentReplaced(); // the document is GONE; its layer ids mean nothing now
        if (m_canvas)
            presentComposite(m_lastComposite, /*fitView=*/false); // empty -> "show nothing"
        if (m_statusBar) {
            m_statusBar->clearDocumentInfo();
            m_statusBar->setCursor(std::nullopt);
            m_statusBar->setSelectionBounds(std::nullopt); // no document -> no selection to preview
        }
        // Tools have nothing to act on: grey the toolbar + its context bar until a document
        // arrives (user feedback 2026-07-06; the menu keeps File + Help live for New/Open/Quit).
        if (m_toolbar)
            m_toolbar->deactivate();
        if (m_optionsBar)
            m_optionsBar->deactivate();
        setDocumentMenusVisible(false); // only File + Help remain until a document opens
        updateWindowTitle(); // no document -> the bare "Mosaic"
        applyDockWidth();    // the dock leaves with the document; the empty state takes its column
        if (m_canvas) {
            m_canvas->setIdleDropHot(false); // never resurface mid-highlight
            m_canvas->setIdleEnabled(true);  // the canvas renders + mans the invitation itself
        }
    }

    // Open `doc` in a NEW TAB and make it active (S49). Every arrival of a document goes through
    // here -- File->New, File->Open, a crash restore, the demo canvas -- so opening never closes
    // what is already open. The outgoing document is parked in m_sessions with its journal, lock
    // and view intact; it is NOT torn down (that is closeSession's job, and only it discards a
    // journal). `fitView` frames the whole document.
    //
    // The live register is left EMPTY for the incoming document, which is why openMosaicAtPath can
    // still assign m_lock / m_documentReadOnly straight after this returns.
    void presentDocument(std::unique_ptr<core::Document> doc, bool fitView) {
        cancelCanvasInteractions();
        unloadActiveSession(); // park what is on screen; its journal and lock stay alive with it
        m_sessions.emplace_back();
        m_activeSession = m_sessions.size() - 1;
        // A fresh register: no journal, no lock, no commit anchor, not read-only, not dirty.
        m_journal.reset();
        m_journalDirty = false;
        m_journalDirtyTime = -1.0e9;
        m_lock.reset();
        m_documentReadOnly = false;
        m_commit.reset();
        m_unsavedSince.reset(); // a freshly presented document starts clean (S18-d)
        m_exportTarget.clear();  // ... and has never been exported: §6's "never leaks to the next
                                 // document" is exactly this line
        m_document = std::move(doc);
        adoptActiveDocument(fitView);
    }

    // Spill the on-screen document's whole state into its session slot. A no-op when the canvas is
    // empty (the startup / last-tab-closed state), which is also when m_activeSession means nothing.
    void unloadActiveSession() {
        if (!m_document || m_activeSession >= m_sessions.size())
            return;
        DocumentSession& s = m_sessions[m_activeSession];
        s.doc = std::move(m_document);
        s.journal = std::move(m_journal);
        m_journal.reset(); // a moved-from optional still ENGAGED would double-discard
        s.journalDirty = m_journalDirty;
        s.journalDirtyTime = m_journalDirtyTime;
        s.lock = std::move(m_lock);
        m_lock.reset();
        s.readOnly = m_documentReadOnly;
        s.commit = std::move(m_commit);
        m_commit.reset();
        s.unsavedSince = m_unsavedSince;
        s.exportTarget = m_exportTarget;
        if (m_canvas) {
            s.view = m_canvas->viewState();
            s.hasView = true; // it has been on screen: restore exactly where it was left
        }
    }

    // Fill the live register from session `i` and show it. The caller must have unloaded whatever
    // was on screen first (or there was nothing).
    void loadSession(std::size_t i) {
        if (i >= m_sessions.size())
            return;
        m_activeSession = i;
        DocumentSession& s = m_sessions[i];
        const bool hasView = s.hasView;
        const CanvasView::ViewState view = s.view;
        m_document = std::move(s.doc);
        m_journal = std::move(s.journal);
        s.journal.reset();
        m_journalDirty = s.journalDirty;
        m_journalDirtyTime = s.journalDirtyTime;
        m_lock = std::move(s.lock);
        s.lock.reset();
        m_documentReadOnly = s.readOnly;
        m_commit = std::move(s.commit);
        s.commit.reset();
        m_unsavedSince = s.unsavedSince;
        m_exportTarget = s.exportTarget;
        adoptActiveDocument(/*fitView=*/!hasView);
        if (hasView && m_canvas)
            m_canvas->setViewState(view); // zoom/pan/rotation are absolute: order vs the composite
                                          // does not matter
    }

    // Point every panel, cache and readout at whatever now sits in m_document. Shared by
    // presentDocument (a brand-new tab) and loadSession (a switch back to an existing one).
    void adoptActiveDocument(bool fitView) {
        if (m_canvas)
            m_canvas->setIdleEnabled(false); // a real document replaces the invitation (the
                                             // field settles over it -- the arrival crossfade)
        if (m_toolbar)
            m_toolbar->activate(); // the no-document greying ends with the document's arrival
        if (m_optionsBar)
            m_optionsBar->activate();
        setDocumentMenusVisible(true); // the document-only menus (Edit..View + File's Save etc.) return
        syncGuideMenuState(); // the Show/Lock Guides checkmarks travel with the document
        // Nothing derived from the outgoing document may survive the swap.
        m_dragCache.invalidate(); // never replay buffers across documents (S15-b)
        m_syncedSelectionRev = std::numeric_limits<std::uint64_t>::max(); // new revision space
        m_lastComposite = common::Image{}; // the cursor colour readout must not sample the old tab
        m_mergedSource = common::Image{};  // ... and the eyedropper's "All Layers" memo even less:
        m_mergedSourceRev = 0;             // a same-size sibling tab would pass its own size check
        // ... and neither may the resident lane's source cache: it is keyed by core::LayerId, and
        // ids are unique only WITHIN one document, so a carried-over entry would serve the other
        // tab's pixels for a layer whose id and revision both happen to match (S60-a item 13).
        if (m_tiles != nullptr)
            m_tiles->noteDocumentReplaced();
        m_smartMapRev = 0;                 // the crop suggestion's importance map is per document
        // The History tab follows the stack itself (S16-b): edits push from the menu, the layer
        // panel AND the canvas tools, so the stack observer is the one hook that sees them all. It
        // also drives the unsaved-state title (dirty transitions ride the same signal). Re-wired
        // per document — the stack dies with its document. Set even without a layer panel so the
        // title still tracks edits.
        m_document->commands().setOnChange([this] {
            if (m_layerPanel)
                m_layerPanel->history()->refresh();
            onCommandStackChanged();
            // Mark the recovery journal dirty; onFrame autosaves once editing settles (S48). Undo/
            // redo/jumpTo stamp too -- the journal mirrors the current in-memory state, whatever it
            // is. A stamp with no real content change autosaves to an empty diff (a no-op tick).
            m_journalDirty = true;
            m_journalDirtyTime = nowSeconds();
        });
        // Render every text block's pixel cache BEFORE the panel builds its rows. A TextLayer keeps
        // its pixels in a renderer-populated cache, and layerThumbnail has nothing to draw without
        // one -- so an opened document's text (3D text most visibly) showed a blank placeholder
        // until the next edit re-rendered it. This costs one text render at open; the first
        // composite would have done it a frame later anyway (user report, 2026-07-09).
        settleTextCaches();
        if (m_layerPanel)
            m_layerPanel->setDocument(m_document.get());
        updateShapeRadiusRange(); // the rect "Corner" slider tops out at the document size, not
                                  // 2000px
        if (m_colorPicker) // the picker's lcms2 engine follows the document working space (S12-b)
            m_colorPicker->setWorkingSpace(m_document->colorSpace(),
                                           m_document->iccProfilePath());
        if (m_statusBar) {
            m_statusBar->setDocumentInfo(m_document->width(), m_document->height(),
                                         m_document->dpi(),
                                         core::precisionName(m_document->precision()));
            syncColorSpaceName();
        }
        updateWindowTitle();    // reflect the new document's name (S18-d)
        refreshTabStrip();      // ... and may make the strip appear (the 2nd document) ...
        applyTabStripVisible(); // ... which changes where the canvas starts
        applyDockWidth();       // the dock returns with the document (hidden while none was open)
        requestRecomposite(fitView);
    }

    // Switch to tab `i`.
    void activateSession(std::size_t i) {
        if (i >= m_sessions.size() || (i == m_activeSession && m_document))
            return;
        cancelCanvasInteractions();
        unloadActiveSession();
        loadSession(i);
    }

    // The document in slot `i`, wherever it currently lives (the active one is in the register).
    [[nodiscard]] core::Document* documentAt(std::size_t i) {
        if (i >= m_sessions.size())
            return nullptr;
        return (i == m_activeSession && m_document) ? m_document.get() : m_sessions[i].doc.get();
    }

    // Is there work an abrupt exit would take with it? Every open tab, not just the visible one:
    // each holds its own document and its own recovery journal (S49), and a background tab's
    // unsaved edits are exactly as unsaved as the foreground one's.
    [[nodiscard]] bool anySessionDirty() {
        for (std::size_t i = 0; i < m_sessions.size(); ++i) {
            const core::Document* d = documentAt(i);
            if (d != nullptr && d->dirty())
                return true;
        }
        return false;
    }

    // The tab already showing `path`, if any. Compared canonically, so ./a.png and /abs/a.png are
    // one document. Untitled documents (no file path) never match.
    [[nodiscard]] std::optional<std::size_t> sessionForPath(const std::string& path) {
        const std::string canonical = canonicalPathOf(path);
        if (canonical.empty())
            return std::nullopt;
        for (std::size_t i = 0; i < m_sessions.size(); ++i) {
            const core::Document* d = documentAt(i);
            if (d != nullptr && !d->filePath().empty() &&
                canonicalPathOf(d->filePath()) == canonical) {
                return i;
            }
        }
        return std::nullopt;
    }

    // Close tab `i`, prompting when it has unsaved changes. Returns false when the user cancelled,
    // so a caller closing several (the window's quit path) can abort the whole run.
    //
    // Closing a CLEAN background tab must not steal the canvas, so it is torn down where it sits.
    // A DIRTY one is brought forward first -- nobody should answer "save your changes?" about a
    // document they cannot see -- and the tab that was on screen comes back afterwards.
    bool closeSession(std::size_t i) {
        core::Document* target = documentAt(i);
        if (target == nullptr)
            return true;
        const bool closingVisible = (i == m_activeSession) && m_document != nullptr;
        std::size_t restore = m_activeSession; // the tab to return to (meaningless if it IS i)

        if (target->dirty()) {
            activateSession(i); // a no-op when it is already on screen
            if (!confirmDiscardActiveDocument()) {
                if (!closingVisible)
                    activateSession(restore); // cancelled: put the user back where they were
                return false;
            }
        }
        // A deliberate close: discard the journal (a crash must leave it behind, this must not) and
        // let go of the file's lock so another window can take it. Slot i is in the live register
        // iff it is the active one -- which it now is whenever it was dirty.
        if (i == m_activeSession && m_document) {
            discardJournal();
            releaseLock();
            // Same invariant as clearDocument: no panel may point at a document being destroyed.
            // Nothing between here and the clearDocument/loadSession below touches the dock today,
            // so this is defensive -- but the rule is "detach before destroy", uniformly, and the
            // crash it prevents (SIGSEGV on tab close) was exactly a gap of a few lines.
            if (m_layerPanel)
                m_layerPanel->setDocument(nullptr);
            m_document.reset();
            m_commit.reset();
            m_unsavedSince.reset();
            m_documentReadOnly = false;
        } else {
            DocumentSession& s = m_sessions[i];
            if (s.journal.has_value())
                s.journal->discard();
            if (s.lock.has_value())
                s.lock->release();
        }
        m_sessions.erase(m_sessions.begin() + static_cast<std::ptrdiff_t>(i));
        if (m_sessions.empty()) {
            m_activeSession = 0;
            clearDocument(); // back to the click-or-drop invitation
            refreshTabStrip();
            applyTabStripVisible();
            return true;
        }
        if (i < restore)
            --restore; // the erase slid every later slot one to the left
        if (closingVisible) {
            loadSession(std::min(i, m_sessions.size() - 1)); // the tab that took its place
        } else if (m_document) {
            m_activeSession = restore; // a clean background close: the canvas never moved
            refreshTabStrip();
            applyTabStripVisible();
        } else {
            loadSession(restore); // a dirty background close: bring the user's tab back
        }
        return true;
    }

    // "Save / Don't save / Cancel" for the active document. True = go ahead and close it.
    // Rightmost is the accent default (Save); Escape and the WM close land on Cancel, NOT on the
    // leftmost "Don't save" -- a stray Escape must never throw work away (docs/askortell-dialog.md).
    [[nodiscard]] bool confirmDiscardActiveDocument() {
        if (!m_document)
            return true;
        std::string name = m_document->title();
        if (!m_document->filePath().empty())
            name = std::filesystem::path(m_document->filePath()).filename().string();
        char body[512];
        std::snprintf(body, sizeof body,
                      _("\"%s\" has unsaved changes. If you don't save them, they are lost."),
                      name.c_str());
        AskOrTellDialog dlg;
        AskOrTellDialog::Stage stage;
        stage.icon = AskOrTellDialog::Icon::Warning;
        stage.title = _("Save your changes?");
        stage.message = body;
        stage.buttons = {_("Don't save"), _("Cancel"), _("Save")};
        stage.cancelButton = 1; // Cancel, not the leftmost
        const int choice = dlg.ask(stage, this);
        if (choice == 2) {
            saveDocument();          // may open Save As; may fail or be cancelled
            waitForBackgroundSave(); // a full write runs async; the answer below must be honest
            return m_document && !m_document->dirty();
        }
        return choice == 0; // Don't save; Cancel / Escape / WM close keep the document open
    }

    // The window's close button, and File->Quit later. Walks every open document, prompting for the
    // dirty ones, then tears them ALL down -- their journals must be discarded, or the next launch
    // would offer to restore documents the user consciously closed. False = the user cancelled.
    [[nodiscard]] bool confirmQuit() {
        while (!m_sessions.empty()) {
            if (!closeSession(m_sessions.size() - 1))
                return false; // cancelled: the remaining tabs stay open, nothing was discarded
        }
        return true;
    }

    // Rebuild the strip's labels from the sessions. Cheap: TabStrip::setTabs early-outs on no change.
    void refreshTabStrip() {
        if (m_tabStrip == nullptr)
            return;
        std::vector<TabStrip::TabItem> items;
        items.reserve(m_sessions.size());
        for (std::size_t i = 0; i < m_sessions.size(); ++i) {
            // The active document lives in the register, not in its slot. Every slot yields exactly
            // one item, even the (transiently) empty one presentDocument has just pushed -- the
            // strip addresses tabs by index, so skipping a slot would misroute every later click.
            const bool live = i == m_activeSession && m_document != nullptr;
            const core::Document* d = live ? m_document.get() : m_sessions[i].doc.get();
            const bool readOnly = live ? m_documentReadOnly : m_sessions[i].readOnly;
            if (d == nullptr) {
                items.emplace_back();
                continue;
            }
            std::string label = d->title();
            if (!d->filePath().empty())
                label = std::filesystem::path(d->filePath()).filename().string();
            items.push_back({std::move(label), d->dirty(), readOnly});
        }
        m_tabStrip->setTabs(std::move(items), m_activeSession);
    }

    // Show the strip only with 2+ documents open, and re-place the body regions around it.
    void applyTabStripVisible() {
        if (m_tabStrip == nullptr)
            return;
        const bool want = m_sessions.size() > 1;
        if (want == (m_tabStrip->visible() != 0))
            return; // no layout change
        if (want)
            m_tabStrip->show();
        else
            m_tabStrip->hide();
        applyDockWidth(); // recomputes bodyTop from tabStripHeight() and re-places canvas + dock
        redraw();
    }

    // The row the strip occupies right now: 0 while it is hidden (a lone document must not pay a
    // pixel for a tab bar that says nothing the title bar does not).
    [[nodiscard]] int tabStripHeight() const {
        return (m_tabStrip != nullptr && m_tabStrip->visible() != 0) ? kTabStripHeight : 0;
    }

    // The command stack changed (push / undo / redo / jumpTo): retrack the unsaved state and refresh
    // the window title. Dirty is derived from the stack's saved marker, so the clean<->dirty
    // transitions ride this one signal (S18-d).
    void onCommandStackChanged() {
        refreshTabStrip(); // the active tab's unsaved dot follows the stack's saved marker (S49)
        const bool dirty = m_document && m_document->dirty();
        if (dirty) {
            if (!m_unsavedSince) // stamp the moment it first went dirty (for "unsaved for N min")
                m_unsavedSince = nowSeconds();
        } else {
            m_unsavedSince.reset();
        }
        updateWindowTitle();
    }

    // Recompute the window title and push it to the label only when it changes (S18-d). Called from
    // onCommandStackChanged for the dirty transitions and once per frame so the "unsaved for N min"
    // duration ticks off the existing frame timer -- copy_label only actually fires when the minute
    // (or second) rolls over.
    void updateWindowTitle() {
        // File ▸ "Export to <file>" rides this same once-per-frame reconcile: it is a string
        // compare unless the target actually changed, and hanging it here means every path that
        // switches, opens or closes a document keeps the row correct without its own hook.
        syncExportToMenuItem();
        syncDynamicMenuItems(); // ... and the S53-b Type/Layer rows, on exactly the same terms
        if (!m_document) {
            // The no-document state: the bare app name -- "Untitled" would imply an open
            // (saveable) document that is not there.
            if (m_currentTitle != "Mosaic") {
                m_currentTitle = "Mosaic";
                copy_label(m_currentTitle.c_str());
            }
            return;
        }
        std::string name;
        if (m_document) {
            if (!m_document->filePath().empty()) {
                // A .mosaic-backed document is announced by its OWN name (the manifest title) --
                // the file name is the tab strip's job (user 2026-07-22: "the titlebar
                // definitely needs to be the actual document name"). Foreign extensions keep
                // the filename: "photo.png" says the document is still backed by an image file
                // (and its title is the stem anyway).
                const std::string fileName =
                    std::filesystem::path(m_document->filePath()).filename().string();
                std::string ext = std::filesystem::path(fileName).extension().string();
                for (char& c : ext)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext == ".mosaic" && !m_document->title().empty())
                    name = m_document->title();
                else
                    name = displayDocumentName(fileName);
            } else {
                name = m_document->title();
            }
        }
        const bool dirty = m_document && m_document->dirty();
        int unsavedSecs = -1;
        if (dirty && m_unsavedSince)
            unsavedSecs = static_cast<int>(nowSeconds() - *m_unsavedSince);
        // The words come from here, not from inside the formatter, so it stays pure and
        // golden-testable (window_title.hpp). Each carries its own TRANSLATORS note: xgettext
        // attaches a comment only to the call on the line(s) directly below it, and three of these
        // are far too short to translate blind.
        // TRANSLATORS: window title, whole phrase: "<document> • unsaved for 5 min — Mosaic".
        // This is the mark that a document has changes that are not written to disk.
        const std::string_view unsavedWord = _("unsaved");
        // TRANSLATORS: window title connective, between "unsaved" and a duration:
        // "unsaved FOR 5 min".
        const std::string_view forWord = _("for");
        // TRANSLATORS: abbreviation for MINUTES in the window title's unsaved duration ("5 min").
        const std::string_view minWord = _("min");
        // TRANSLATORS: abbreviation for SECONDS in the window title's unsaved duration ("12 sec").
        const std::string_view secWord = _("sec");
        const std::string title = formatWindowTitle(name, dirty, unsavedSecs,
                                                    {.showDuration = m_showUnsavedDuration,
                                                     .includeSeconds = m_unsavedIncludeSeconds,
                                                     .thresholdSeconds = 300,
                                                     .untitled = _("Untitled"),
                                                     .unsaved = unsavedWord,
                                                     .durationFor = forWord,
                                                     .minutes = minWord,
                                                     .seconds = secWord});
        if (title != m_currentTitle) {
            m_currentTitle = title;
            copy_label(m_currentTitle.c_str());
        }
    }

    // Cap the rect tool's "Corner" radius slider at half the document's shorter side (clamped to a
    // sane minimum) instead of a flat 2000px -- on a small canvas the 2000 range made the slider
    // hyper-sensitive (user feedback). Picked up by the next bar rebuild, so rebuild now in case
    // the shape tool is already active.
    void updateShapeRadiusRange() {
        if (!m_document)
            return;
        const double maxR =
            std::max(20.0, std::min(m_document->width(), m_document->height()) * 0.5);
        if (Tool* t = m_tools.find(ToolId::RectShape))
            for (ToolOption& o : t->options())
                if (o.id == "radius")
                    o.max = maxR;
        if (m_optionsBar)
            m_optionsBar->rebuild();
    }

    // Refresh the status bar's colour-space indicator from the picker's lcms2 engine (the name
    // can change via presentDocument's setWorkingSpace or configurePicker's profile overrides).
    void syncColorSpaceName() {
        if (m_statusBar && m_colorPicker)
            m_statusBar->setColorSpaceName(m_colorPicker->workingName());
    }

    // The pointer moved over (or left) the canvas: update the status bar's position + colour
    // readout. The colour is read from the last composite shown on the canvas -- which since S19-c
    // carries TRUE alpha (the checkerboard is a screen-space present-shader effect, no longer baked
    // in), so this reports the real pixel value, transparency included. The future Info panel (§11)
    // shares this source so the two readouts can never disagree.
    void onCanvasCursor(double docX, double docY, bool overCanvas) {
        if (m_rulersVisible && m_rulerH != nullptr) {
            m_rulerH->setCursor(docX, docY, overCanvas); // the moving tick follows the cursor
            m_rulerV->setCursor(docX, docY, overCanvas);
        }
        if (m_statusBar == nullptr)
            return;
        if (!overCanvas || !m_document) {
            m_statusBar->setCursor(std::nullopt);
            return;
        }
        CursorReadout r;
        r.docX = docX;
        r.docY = docY;
        // Audit A1. It fires per POINTER EVENT, so under the resident lane the pixels it wants are
        // on the device and every way of getting them there costs a submit and a fence.
        //
        // ⚠ THIS DOES NOT READ THE DEVICE, and the pinned mirror is deliberately not used here.
        // The mirror made the READ free but not the FILL: seeding a pin is a macrotile transfer
        // plus a fence, the pointer crosses macrotiles constantly, and each seed measured ~20 ms
        // on a live canvas because the fence waits behind the frame already in flight. Throttling
        // it only made the stall rarer -- the user's report was 2 per second, which is 2 hitches
        // per second. A readout is not worth a hitch, ever.
        //
        // So the readout composites THE ONE PIXEL IT NEEDS on the CPU: the same walk the whole
        // canvas used to take, over a 1x1 region, with no device contact at all. It costs
        // microseconds, it cannot fence, and it is the same reference the GPU lane is held to
        // within 1 LSB by the parity tests -- so the number in the status bar is the number on the
        // screen, to the precision anyone can act on.
        if (m_tiles != nullptr && m_tiles->serving() && !m_recomposeReview) {
            r.insideDocument = docX >= 0.0 && docY >= 0.0 &&
                               docX < static_cast<double>(m_document->width()) &&
                               docY < static_cast<double>(m_document->height());
            if (r.insideDocument) {
                const common::Rect one{std::floor(docX), std::floor(docY), 1.0, 1.0};
                // The same options the canvas composite uses, so the readout reports the colour
                // that is actually on screen rather than a differently-configured one. No
                // checkerboard (the present shader draws that in screen space), and never the
                // live-drag filter -- a readout is not a drag.
                render::CompositeOptions opts;
                opts.checkerboard = false;
                opts.resampleFilter = currentResampleFilter();
                const common::Image px1 =
                    render::compositeRegion(*m_document, one, opts, render::Backend::Cpu).image;
                if (px1.width >= 1 && px1.height >= 1 && px1.rgba.size() >= 4)
                    r.color = {px1.rgba[0], px1.rgba[1], px1.rgba[2], px1.rgba[3]};
                else
                    r.insideDocument = false;
            }
            m_statusBar->setCursor(r);
            return;
        }
        // During a Recompose review the canvas displays the PREVIEW (its own coordinate space),
        // so the readout must sample it — m_lastComposite is the old document's pixels.
        const common::Image& shown =
            m_recomposeReview && !m_recomposePreview.empty() ? m_recomposePreview : m_lastComposite;
        const long px = std::lround(std::floor(docX));
        const long py = std::lround(std::floor(docY));
        if (px >= 0 && py >= 0 && px < static_cast<long>(shown.width) &&
            py < static_cast<long>(shown.height)) {
            const std::size_t p =
                (static_cast<std::size_t>(py) * shown.width + static_cast<std::size_t>(px)) * 4;
            r.insideDocument = true;
            r.color = {shown.rgba[p], shown.rgba[p + 1], shown.rgba[p + 2], shown.rgba[p + 3]};
        }
        m_statusBar->setCursor(r);
    }

    // The canvas view transform changed (zoom/rotate, not pan): refresh the zoom/rotation slot.
    void onCanvasViewChanged() {
        if (m_statusBar && m_canvas)
            m_statusBar->setViewState(m_canvas->view().zoom(), m_canvas->view().rotationDegrees());
        if (m_rulersVisible && m_rulerH != nullptr) { // ticks re-scale with zoom / re-place on pan
            m_rulerH->redraw();
            m_rulerV->redraw();
        }
    }

    // Mark the canvas composite stale; the next frame rebuilds it (at most once per frame). Edits
    // -- especially a continuous gesture like the opacity slider -- must NOT recomposite
    // synchronously in their callback: the CPU compositor is heavy enough (whole document, debug
    // build) that doing it per drag event starves the FLTK event loop and freezes the whole UI
    // until the backlog drains. Coalescing to the ~60 Hz frame keeps input responsive while still
    // giving a live preview. (The GPU-resident, dirty-tile re-composite is the S60 perf pass.)
    void requestRecomposite(bool fitView) {
        m_recompositePending = true;
        m_pendingFitView = m_pendingFitView || fitView;
        requestFrame(); // S15-b: composite this edit now, not at the next heartbeat
    }

    // Open (or toggle shut) the shape-designer popover (S26-b §7.4), anchored to the options bar's
    // "Edit shape…" button, seeded from the shape currently selected for editing.
    void openShapeDesigner() {
        if (m_shapeDesigner == nullptr || m_canvas == nullptr || m_optionsBar == nullptr)
            return;
        Fl_Widget* anchor = m_optionsBar->designerButton();
        if (anchor == nullptr)
            return;
        if (m_shapeDesigner->shownFor(anchor)) { // re-click the button closes it
            m_shapeDesigner->hide();
            return;
        }
        const core::vec::Object* obj = nullptr;
        const core::LayerId id = m_canvas->shapeEditTarget();
        if (id != core::kInvalidLayerId && m_document)
            if (core::Layer* l = m_document->find(id))
                if (auto* vl = l->as<core::VectorLayer>())
                    obj = vl->object();
        // Draw the session's coalesce id from the canvas's shared sequence (never collides with the
        // bar / box edits on the same layer).
        m_shapeDesignerCoalesce = m_canvas->beginShapeEditSession();
        m_shapeDesigner->openFor(anchor, obj);
    }

    // Open (or toggle shut) the Type panel (S29-c §8), anchored to the options bar's "Style…"
    // button and seeded from the current selection's common style. A no-op without an active text
    // session.
    void openTypePanel() {
        if (m_typePanel == nullptr || m_canvas == nullptr || m_optionsBar == nullptr)
            return;
        if (m_optionsBar->typePanelButton() == nullptr)
            return;
        if (!m_typePanel->shown() && m_canvas->textEditTarget() == core::kInvalidLayerId)
            return; // nothing to edit -- the panel only makes sense over a live text session
        // The arbiter owns the rest: re-toggling closes, toggling over "3D…" replaces it (the
        // shared-corner exclusivity), and closing promotes whatever queued behind it.
        m_panelArbiter.toggle(kPanelStyle);
        syncCornerPanels();
    }

    // Close the Type panel if it is open (a tool switch / text session end / new document). Safe
    // always.
    void closeTypePanel() {
        if (m_typePanel != nullptr && m_typePanel->shown())
            m_typePanel->hide();
        if (m_panelColorFlyout != nullptr)
            m_panelColorFlyout->hide(); // its anchor chip lives in the panel
        setTypePanelButtonOpen(false);
    }

    // Mirror the panel's open/closed state on its bar button: a momentary button drawn pressed
    // while the panel is up, so "Style…" reads like a toggle (the panel stays open across canvas
    // clicks now, so the button is the only on-screen cue that it's open). No-op when the button
    // isn't built.
    void setTypePanelButtonOpen(bool open) {
        if (m_optionsBar == nullptr)
            return;
        if (auto* b = static_cast<Fl_Button*>(m_optionsBar->typePanelButton())) {
            b->value(open ? 1 : 0);
            b->redraw();
        }
    }

    // The 3D popup's open/close/button-state trio -- the Type panel's twin (S30-d §8.4).
    void openType3dPanel() {
        if (m_type3dPanel == nullptr || m_canvas == nullptr || m_optionsBar == nullptr)
            return;
        if (m_optionsBar->type3dButton() == nullptr)
            return;
        if (!m_type3dPanel->shown() && m_canvas->textEditTarget() == core::kInvalidLayerId)
            return; // the popup sculpts the edited text object; nothing to sculpt without one
        m_panelArbiter.toggle(kPanelType3d);
        syncCornerPanels();
    }
    void closeType3dPanel() {
        if (m_type3dPanel != nullptr && m_type3dPanel->shown())
            m_type3dPanel->hide();
        if (m_panelColorFlyout != nullptr)
            m_panelColorFlyout->hide(); // its anchor chip lives in the popup
        setType3dButtonOpen(false);
    }
    void setType3dButtonOpen(bool open) {
        if (m_optionsBar == nullptr)
            return;
        if (auto* b = static_cast<Fl_Button*>(m_optionsBar->type3dButton())) {
            b->value(open ? 1 : 0);
            b->redraw();
        }
    }

    // The live Move/Resize/Rotate preview each transform frame (S60-a). When a single, top-level,
    // qualifying raster layer is being dragged, the document below it is static and only its affine
    // changes -- so the GPU composites it over a cached `below` texture each frame (no CPU
    // recomposite, no whole-canvas upload). Anything else (multi-select, nested, masked, non-
    // separable blend) falls through to the CPU coalesced recomposite (DragCompositeCache).
    void driveTransformPreview() {
        const core::LayerId target = m_canvas->activeDragLayer();
        // ⚠ THE RESIDENT LANE OUTRANKS THE DRAG PASS, and the arbitration has to happen HERE rather
        // than at resolve time, because both write the same canvas VkImage (resident_composite.hpp,
        // "Who writes the canvas texture"). If the lane drew the frame this gesture started from it
        // will draw this gesture's frames too -- a Move drag changes a placement, and nothing the
        // lane refuses on is a placement -- and it does so for free: no `below` composite, no
        // per-gesture textures, no uploads at all, against the drag pass's full CPU walk + two
        // whole-canvas uploads at gesture start (docs/s60-gesture-start-stall.md).
        //
        // Decided ONCE, by borrowing `m_gpuDragTried`: an arming that happened three frames into a
        // drag would pay that gesture-start walk as a mid-drag freeze. So if the lane then refuses
        // after all, the fallback for the rest of the gesture is the drag-scoped CPU cache (S15-b),
        // which is bit-identical to the full walk -- a slower frame, never a different picture.
        if (!m_gpuDragTried && target != core::kInvalidLayerId && residentLaneOwnsDrag())
            m_gpuDragTried = true;
        // Only "give up" (mark tried) once we actually have a drag target to evaluate -- early
        // gesture frames can report an invalid target, and we must not permanently fall back on
        // those. Building `below` is a full composite, so we still evaluate eligibility just ONCE.
        //
        // ⚠ A CHANNEL ISOLATION USED TO DISQUALIFY THIS PASS, and the reason was real: the remap
        // was a CPU pass on the way to the canvas texture, and the drag pass writes that texture
        // itself from inside drawFrame -- so a drag under an isolation showed unisolated colour.
        // Since S60-a item 10 canvas_present.comp applies the view where it SAMPLES that texture,
        // which is downstream of every writer including this one. The exception is gone, not moved.
        if (!m_gpuDragTried && target != core::kInvalidLayerId &&
            render::canUseGpuDrag(*m_document, target)) {
            m_gpuDragTried = true;
            core::Layer* l = m_document->find(target);
            auto* raster = l != nullptr ? l->as<core::RasterLayer>() : nullptr;
            if (raster != nullptr) {
                const std::uint32_t dragW = raster->image().width;
                const std::uint32_t dragH = raster->image().height;
                // The backdrop is BOUNDED (docs/s60-gesture-start-stall.md §3.2): the quality bound
                // the view asks for, then -- only if the device refuses it -- coarser still.
                std::uint32_t divisor = backdropDivisor();
                BackdropSize below = backdropSizeFor(m_document->width(), m_document->height(),
                                                     divisor);
                // ⚠ Ask whether the device can host the textures BEFORE building the composite.
                // That walk costs seconds on a large document and it used to be evaluated as
                // beginGpuDrag's ARGUMENT, so on a device whose maxImageDimension2D cannot hold the
                // canvas (Vulkan 1.0 guarantees only 4096, and a 5000x8000 document is the live
                // case) the entire bill was paid and then refused. Now the refusal is free -- and
                // `below` is the ONE drag texture we are free to shrink, so a floor device gets a
                // coarser backdrop instead of losing the whole fast lane. If it is the DRAGGED
                // layer that does not fit, no amount of halving helps and the search runs out into
                // the CPU path, which is the right answer.
                while (!m_canvas->canHostGpuDrag(below.w, below.h, dragW, dragH) &&
                       divisor < kMaxBackdropDeviceDivisor) {
                    divisor *= 2;
                    below = backdropSizeFor(m_document->width(), m_document->height(), divisor);
                }
                if (m_canvas->canHostGpuDrag(below.w, below.h, dragW, dragH)) {
                    MOSAIC_PERF_SCOPE("Drag gesture start", Lane::Cpu);
                    // pass the layer pixels by reference -- no extra copy
                    m_gpuDragActive = m_canvas->beginGpuDrag(
                        buildBelowComposite(target, below.w, below.h), raster->image());
                }
            }
        }
        if (m_gpuDragActive) {
            // ⚠ THE THIRD WRITER ANNOUNCES ITSELF. The drag pass dispatches into the canvas texture
            // from inside drawFrame -- outside presentComposite, outside presentCompositeRegion,
            // outside every seam that funnels noteCpuFrame() -- and these frames queue no
            // recomposite at all, so residentRecompositeNow never runs to notice. Without this the
            // lane would still believe it was serving when the gesture ended, and its first partial
            // resolve afterwards would leave the drag pass's own output standing in every macrotile
            // the drag had not dirtied. Idempotent and free after the first frame (noteCpuFrame is
            // a bool once m_serving is down).
            if (m_tiles != nullptr)
                m_tiles->noteCpuFrame();
            // The S60-a GPU-resident drag: the compositing happens on the GPU, so this op is tagged
            // to the GPU lane in the profiler (ballpark CPU wall-clock of the per-frame submit).
            MOSAIC_PERF_SCOPE("Transform drag (GPU)", Lane::Gpu);
            if (const core::Layer* l = m_document->find(target)) {
                const common::Affine2D inv =
                    core::worldTransform(*l).inverse().value_or(common::Affine2D::identity());
                m_canvas->setGpuDragTransform(inv, static_cast<int>(l->blendMode()), l->opacity());
            }
            requestFrame(); // present the GPU drag this frame
        } else {
            requestRecomposite(/*fitView=*/false); // CPU fallback (coalesced)
        }
    }

    // --- The BOUNDED drag backdrop (S60, docs/s60-gesture-start-stall.md §3.2) ------------------
    // `below` is the texture the drag shader samples for everything EXCEPT the dragged layer, so it
    // may never be coarser than what the screen can resolve -- and it never needs to be finer. At
    // view zoom z the whole document occupies docW*z x docH*z screen pixels, and that is the
    // backdrop's honest size: fit-to-window on a 5000x8000 document (the case the stall was
    // measured on -- 919 ms quiet, 1337 ms at load 6, ~3.1 s under the load the first table was
    // taken at) puts z near 1/5, dropping the composite from 40 Mpx to ~1.6 Mpx. At 1:1 and above
    // the divisor is 1 and nothing changes, which is CORRECT rather than merely safe: a
    // whole-document texture bounded below the zoom would visibly soften the canvas mid-drag, and
    // the backdrop is most of what the user is looking at while dragging.
    struct BackdropSize {
        std::uint32_t w = 0;
        std::uint32_t h = 0;
    };
    // Halving past the view's own bound is a DEVICE concession, not a quality choice -- it is what
    // a floor device (Vulkan 1.0 guarantees maxImageDimension2D 4096, and this document is 5000
    // wide) gets instead of losing the fast lane outright. 16 bounds the search; past that the
    // dragged layer itself is the thing that does not fit and no amount of halving helps.
    static constexpr std::uint32_t kMaxBackdropDeviceDivisor = 16;
    [[nodiscard]] static BackdropSize backdropSizeFor(std::uint32_t docW, std::uint32_t docH,
                                                      std::uint32_t divisor) {
        const std::uint32_t d = std::max<std::uint32_t>(1, divisor);
        return {std::max<std::uint32_t>(1, (docW + d - 1) / d),
                std::max<std::uint32_t>(1, (docH + d - 1) / d)};
    }
    [[nodiscard]] std::uint32_t backdropDivisor() {
        const double z = m_canvas != nullptr ? m_canvas->view().zoom() : 1.0;
        if (!(z > 0.0) || z >= 1.0)
            return 1; // 1:1 or magnified: the document's own pixels are the bound
        return static_cast<std::uint32_t>(std::floor(1.0 / z));
    }

    // Composite the document with `target` left out, straight into an outW x outH buffer -> the
    // static `below` texture for the GPU drag, and the 3D reflect environment. ONE composite, once
    // per gesture, and now bounded: it used to render the full canvas and throw most of it away.
    [[nodiscard]] common::Image buildBelowComposite(core::LayerId target, std::uint32_t outW,
                                                    std::uint32_t outH) {
        // The once-per-gesture CPU composite that seeds the GPU drag -- the CPU-lane counterpart to
        // "Transform drag (GPU)" above (a clean example of one feature spanning both lanes).
        MOSAIC_PERF_SCOPE("Drag below-composite", Lane::Cpu);
        core::Layer* l = m_document ? m_document->find(target) : nullptr;
        if (l == nullptr)
            return {};
        // Nothing composites below `target` when it is the ONLY top-level child: hand back a 1x1
        // transparent backdrop (the shader samples `below` normalized) instead of compositing +
        // uploading a full empty document -- the bulk of the gesture-start cost on a single-layer
        // doc (S60-a).
        //
        // ⚠ Both halves of that test are load-bearing. The root's child count alone asks "is the
        // root nearly empty?", but what the shortcut MEANS is "is there nothing below target?",
        // and those coincide only when target is top-level. canUseGpuDrag guarantees that for the
        // drag caller; updateReflectionEnv's caller takes ANY text layer in the tree, so a text
        // layer nested in a group under a single-child root took a 1x1 transparent backdrop and
        // mirrored nothing instead of the content beside it in its group.
        if (l->parent() == &m_document->root() && m_document->root().childCount() <= 1)
            return common::Image(1, 1);
        render::CompositeOptions opts;
        opts.checkerboard = false;
        // The READ-ONLY exclusion (finding G6). This used to flip the layer's own visibility and
        // flip it back -- an uncommanded, unobserved mutation of the user's document, safe only
        // because the walk is synchronous and non-reentrant on the UI thread, and a hard blocker
        // for ever moving it off that thread.
        opts.skipLayer = target;
        // Finding G2: the backdrop stands in for the frames around it, so it resamples like them --
        // the same filter the ordinary composite path uses, rather than CompositeOptions' Nearest
        // default, which at a reduced size would alias the whole canvas.
        //
        // `liveDrag` is deliberately NOT matched, and that is the reconciliation rather than a gap:
        // it exists to spend less per frame while the picture is moving, and this buffer is built
        // ONCE and reused for every frame of the gesture. Paying full quality once is both cheaper
        // over the gesture and closer to the frame the gesture ends on.
        opts.resampleFilter = currentResampleFilter();
        render::CompositeResult r =
            render::compositeScaled(*m_document, outW, outH, opts, render::Backend::Cpu);
        return r.ok ? std::move(r.image) : common::Image{};
    }

    // --- 3D reflect-canvas snapshot (S30-d follow-up, feedback 2026-07-03) --------------------
    // When the edited block wants Extrude::reflectCanvas, build the environment its metal
    // mirrors: the document composited WITHOUT the text layer itself (self-reflection would be a
    // feedback loop), box-downsampled to a bounded texture and stored on the TextLayer for the
    // render lanes. STALENESS is decided by a FINGERPRINT of everything the mirror can see --
    // every other layer's identity/visibility/opacity/blend/transform/content revision, the doc
    // size, and the text layer's own placement -- NOT by a whitelist of update sites (round-2
    // feedback: layer drags/paste/new layers all have to count). A short settle batches
    // mid-gesture churn so a Move drag or brush stroke rebuilds once, on release.
    static constexpr std::uint32_t kReflectEnvMaxDim = 768;
    static constexpr double kReflectEnvSettleSec = 0.30;
    [[nodiscard]] std::uint64_t reflectStackFingerprint(const core::TextLayer& target) {
        std::uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](const void* data, std::size_t bytes) {
            const auto* p = static_cast<const unsigned char*>(data);
            for (std::size_t i = 0; i < bytes; ++i) {
                h ^= p[i];
                h *= 1099511628211ull;
            }
        };
        const auto mixLayer = [&](const core::Layer& l, const auto& self) -> void {
            const core::LayerId id = l.id();
            mix(&id, sizeof(id));
            const bool vis = l.visible();
            mix(&vis, sizeof(vis));
            const float op = l.opacity();
            mix(&op, sizeof(op));
            const auto blend = l.blendMode();
            mix(&blend, sizeof(blend));
            const common::Affine2D& t = l.transform();
            mix(&t, sizeof(t));
            if (&l != &target) { // the mirror never sees the text layer's own content
                const std::uint64_t rev = l.contentRevision();
                mix(&rev, sizeof(rev));
            }
            const bool masked = l.mask() != nullptr; // add/remove counts; in-place mask paints
            mix(&masked, sizeof(masked));            // ride the layer's content revision
            if (const auto* g = l.as<core::GroupLayer>())
                for (const auto& child : g->children())
                    self(*child, self);
        };
        for (const auto& child : m_document->root().children())
            mixLayer(*child, mixLayer);
        const std::uint32_t dw = m_document->width(), dh = m_document->height();
        mix(&dw, sizeof(dw));
        mix(&dh, sizeof(dh));
        return h;
    }
    // Fit-to-path reflow (S30 §9): every frame, re-bake each path-fitted text block's DERIVED
    // baked contours from its source vector layer (rebakeTextPathFit is a no-op returning false
    // when nothing changed -- the usual case costs a flatten + compare per path-text layer).
    // Editing or MOVING the path (or the text layer) thus re-flows the text non-destructively.
    void updateTextPathFits() {
        if (m_document == nullptr)
            return;
        bool any = false;
        const auto walk = [&](core::GroupLayer& g, const auto& self) -> void {
            for (const auto& child : g.children()) {
                if (auto* grp = child->as<core::GroupLayer>()) {
                    self(*grp, self);
                    continue;
                }
                auto* tl = child->as<core::TextLayer>();
                if (tl == nullptr || !tl->block().pathFit)
                    continue;
                if (core::rebakeTextPathFit(*m_document, *tl)) {
                    tl->invalidateContentBounds(); // re-shape + thumbs + recomposite pick it up
                    any = true;
                }
            }
        };
        walk(m_document->root(), walk);
        // REQUEST, don't composite. This hook runs AFTER the frame's recomposite drain, so an
        // immediate recompositeNow() here is a SECOND full-canvas walk in the same frame -- and
        // updateReflectionEnv() below did the same thing, so opening a document with both a
        // path-fitted text layer and a reflecting 3D solid cost THREE full composites where the
        // drain had already done one. Requesting instead lets the two collapse into a single
        // coalesced composite on the next frame, which is how every other edit path in this file
        // already behaves (requestRecomposite has a dozen callers). The cost is one frame of
        // latency on a re-flow that only happens when the path actually moved.
        if (any)
            requestRecomposite(/*fitView=*/false);
    }

    void updateReflectionEnv() {
        if (m_canvas == nullptr || m_document == nullptr)
            return;
        // EVERY text layer with reflections refreshes, session or not (round 3: gating on the
        // edited block meant a Move-tool drag -- of the text or of any other layer -- never
        // updated the mirror). So this walk runs per frame BY DESIGN; what it must not do is cost
        // anything on the overwhelmingly common document that has no reflecting 3D text at all.
        //
        // ⚠ Filter on kind() -- a plain enum compare -- BEFORE as<T>(), which is a dynamic_cast.
        // This used to run two dynamic_casts per layer per frame just to discover there were no
        // text layers: ~50ns each, scaling with layer count, 60x a second, for nothing. (Spotted
        // by the user off the profiler's own readout, 2026-07-23 -- which is what it is for.)
        bool any = false;
        const auto walk = [&](core::GroupLayer& g, const auto& self) -> void {
            for (const auto& child : g.children()) {
                switch (child->kind()) {
                case core::LayerKind::Group:
                    if (auto* grp = child->as<core::GroupLayer>())
                        self(*grp, self);
                    break;
                case core::LayerKind::Text:
                    if (auto* tl = child->as<core::TextLayer>())
                        any |= refreshLayerReflection(*tl);
                    break;
                default:
                    break; // every other kind: one enum compare and out
                }
            }
        };
        walk(m_document->root(), walk);
        if (!any)
            return;
        // Only a real rebuild is worth a profiler row. Scoping the whole function reported "61
        // calls" of doing nothing, which buries the one case that costs something -- the readback
        // audit's finding that a rebuild pays TWO full composites in the same frame.
        //
        // The row now times the ENV BUILD (the refreshLayerReflection walk above, whose own
        // below-composites carry the "Drag below-composite" row) rather than the env build PLUS a
        // full canvas walk: this used to call recompositeNow() directly, which -- because this hook
        // runs after the frame's drain -- was an extra uncoalesced full composite. Its COUNT is
        // still the signal it was added for: how many frames actually rebuilt a mirror.
        MOSAIC_PERF_SCOPE("3D reflect env rebuild", Lane::Cpu);
        requestRecomposite(/*fitView=*/false);
    }
    // Returns true when the layer's snapshot (or its removal) changed something on screen.
    bool refreshLayerReflection(core::TextLayer& tl) {
        const auto& ex = tl.block().extrude;
        const bool wants = ex.has_value() && ex->reflectCanvas;
        if (!wants) {
            m_reflectPending.erase(tl.id());
            if (tl.reflectionEnv() != nullptr) { // toggled off: free the snapshot + re-render
                tl.setReflectionEnv(std::nullopt, common::Affine2D::identity());
                return true;
            }
            return false;
        }
        const std::uint64_t fp = reflectStackFingerprint(tl);
        const bool missing = tl.reflectionEnv() == nullptr;
        if (!missing && fp == tl.reflectionEnvFingerprint()) {
            m_reflectPending.erase(tl.id()); // current: clear any half-armed settle
            return false;
        }
        if (!missing) { // stale, not absent: wait out the gesture before recompositing
            const double now = nowSeconds();
            const auto [it, inserted] = m_reflectPending.try_emplace(tl.id(), fp, now);
            if (!inserted && it->second.first != fp)
                it->second = {fp, now}; // still churning: re-arm on the newest state
            if (now - it->second.second < kReflectEnvSettleSec)
                return false;
        }
        // Ask the compositor for the env-sized picture directly (S60,
        // docs/s60-gesture-start-stall.md §3.2). This used to composite the WHOLE canvas -- 40 Mpx
        // on the documents that hurt -- and then hand-box-downsample it to <= 768 px, i.e. build
        // and throw away ~98% of the pixels. Compositing AT the small size is not merely cheaper,
        // it is better filtered: every layer's kernel is resolved from its composed placement, so
        // the reduction is a minification and `Auto` resolves to a real box filter, applied per
        // layer before blending rather than once over straight-alpha RGBA afterwards (which bleeds
        // colour out of transparent pixels).
        const std::uint32_t docW = m_document->width(), docH = m_document->height();
        const std::uint32_t step = std::max<std::uint32_t>(
            1, (std::max(docW, docH) + kReflectEnvMaxDim - 1) / kReflectEnvMaxDim);
        const common::Image below = buildBelowComposite(tl.id(),
                                                        std::max<std::uint32_t>(1, docW / step),
                                                        std::max<std::uint32_t>(1, docH / step));
        if (below.empty())
            return false;
        // Straight-alpha 8-bit -> float, 1:1. ⚠ The env's size comes from what came BACK, not from
        // what was asked for: buildBelowComposite still short-circuits to a 1x1 transparent
        // backdrop when nothing composites below the layer (finding G1 -- a pre-existing limit for
        // this consumer, unchanged here, not introduced by the bound).
        const std::uint32_t ew = below.width, eh = below.height;
        common::ImageF env(ew, eh);
        for (std::size_t i = 0, n = below.rgba.size(); i < n; ++i)
            env.rgba[i] = static_cast<float>(below.rgba[i]) / 255.0f;
        // layer-local design point -> env pixel: the layer's placement, then the doc->env scale the
        // composite actually used (derived per axis from the returned size, so the 1x1 short-circuit
        // and any rounding in the bound both stay consistent with the pixels above).
        const common::Affine2D layerToEnv =
            common::Affine2D::scaling(static_cast<double>(ew) / docW,
                                      static_cast<double>(eh) / docH) *
            tl.transform();
        tl.setReflectionEnv(std::move(env), layerToEnv); // bumps revision -> cache re-renders
        // Stamp the fingerprint AFTER the write: setReflectionEnv bumped the text layer's own
        // revision, which the fingerprint deliberately ignores (mirror never sees the text).
        tl.setReflectionEnvFingerprint(fp);
        m_reflectPending.erase(tl.id());
        return true;
    }

    // Fire the frame loop for freshly-landed input instead of letting it wait out the idle
    // heartbeat (up to 16.7 ms, avg ~8, of pure latency — S15-b), bounded by what the display can
    // actually show. Every input, edit and animation step in the app funnels through here, so this
    // is the ONE door the ~120 canvas requestHostFrame() call sites reach the frame loop by.
    void requestFrame() {
        // Everything that calls this is a sign of life -- input, an edit, an animation step -- so
        // this is also where the loop learns it should be running at display rate.
        m_lastActivitySec = nowSeconds();
        if (!m_frameLoopStarted)
            return;
        // The earliest this input may reach the eye: one refresh interval after the last present.
        // ⚠ NOT "now". Pointer motion arrives faster than any panel refreshes -- a plain hover over
        // the canvas is enough -- and a present per event is how the frame rate reached THREE TIMES
        // the display's (600 fps on a 200 Hz monitor, user report): two frames in three composited,
        // presented and thrown away, for GPU power and heat and nothing on screen. armFrameAt fires
        // immediately once that interval has passed, so this costs no latency; it only refuses to
        // run ahead of the display.
        armFrameAt(m_lastFrameTime + displayIntervalSec());
    }

    // Is the loop running at display rate? Only while something is actually happening: an idle
    // canvas has no reason to redraw 200 times a second, and saying so here keeps the whole policy
    // in one predicate instead of spread across the call sites.
    [[nodiscard]] bool frameActive() const {
        // ⚠ Deliberately NOT gated on focus. Input reaches a window that does not have focus --
        // hovering an unfocused Mosaic still delivers motion, and the brush reticle has to track
        // the pointer there exactly as smoothly as it does when focused. INPUT is the signal;
        // focus only decides how cheap the loop gets once input STOPS.
        return nowSeconds() - m_lastActivitySec < kActiveWindowSec;
    }

    // The pacing quantum: how long a present must be spaced from the one before it, i.e. the
    // refresh interval of the panel THIS WINDOW is on. Re-asked rather than read once at start-up,
    // the answer belongs to the window and not to the process -- a two-monitor desk at 200 Hz and
    // 60 Hz gives two different answers and the user drags Mosaic between them.
    [[nodiscard]] double displayIntervalSec() {
        const double now = nowSeconds();
        if (now - m_refreshQueriedAt >= kRefreshRequerySec) {
            m_refreshQueriedAt = now;
            const double hz = platform::displayRefreshHz(this);
            const bool believable = hz >= kMinRefreshHz && hz <= kMaxRefreshHz;
            const double useHz = believable ? hz : kFallbackRefreshHz;
            const double interval = 1.0 / useHz;
            // One line per CHANGE, not per query -- so dragging the window to the other monitor
            // leaves exactly one entry saying which rate the loop switched to, and a session that
            // never moves says it once. This is also the only place the fallback is visible, which
            // is why it says so rather than quietly reading as a 60 Hz panel.
            if (std::abs(interval - m_refreshIntervalSec) > 1.0e-9) {
                m_refreshIntervalSec = interval;
                uiLog().info("display refresh: {:.3g} Hz{} -> one present per {:.2f} ms", useHz,
                             believable ? "" : " (assumed: the display did not say)",
                             interval * 1000.0);
            }
        }
        return m_refreshIntervalSec;
    }

    // Arm the frame timer for the wall-clock instant `at`. THE COALESCER: every request landing
    // inside one refresh interval resolves to the same instant, so the first arms a frame and the
    // rest find one already armed for it and cost a comparison. An EARLIER instant always wins --
    // an idle heartbeat must never outrank freshly-landed input -- and a later one is ignored, which
    // is what stops a burst of motion events from pushing the frame away from itself.
    //
    // ⚠ Always Fl::add_timeout from NOW, never Fl::repeat_timeout. repeat_timeout schedules
    // relative to when the timeout SHOULD have fired, so after an immediate (0.0) arm it can fire
    // back to back to "catch up" -- which is where the ~124 fps bursts that then settle to 60 came
    // from. They were never a real 124 fps; they were two frames landing in one interval.
    void armFrameAt(double at) {
        if (m_frameArmed && m_nextFrameDue <= at)
            return;
        m_frameArmed = true;
        m_nextFrameDue = at;
        Fl::remove_timeout(frameTimer, this); // exactly one frame timer stays queued
        Fl::add_timeout(std::max(0.0, at - nowSeconds()), frameTimer, this);
    }

    // Schedule the next frame. Two regimes, and the choice is re-made every frame:
    //   - something is happening -> one frame per refresh interval of the panel the window is on.
    //   - otherwise -> the idle heartbeat.
    void scheduleNextFrame() {
        // Idle. The background rate is reached by INPUT STOPPING, not by a timer deciding the
        // window is unimportant: a window nobody is touching and nobody is looking at drops to 10,
        // and the very next pointer motion pulls it straight back to display rate through
        // requestFrame -- there is no cull to wait out.
        const double idle = m_windowActive ? kFrameIntervalSec : kBackgroundFrameIntervalSec;
        armFrameAt(m_lastFrameTime + (frameActive() ? displayIntervalSec() : idle));
    }

    // The current Transform Anti-aliasing kernel: the Move tool's "aa" choice index mapped to a
    // render::ResampleFilter. The map order mirrors the tool.cpp choice labels (kept explicit so a
    // reordering of either list can't silently mis-map). Read regardless of the active tool -- it's
    // a document-wide render-quality setting that simply lives in the Move options bar.
    [[nodiscard]] render::ResampleFilter currentResampleFilter() const {
        using F = render::ResampleFilter;
        static constexpr F kMap[] = {F::Auto,     F::Nearest,    F::Bilinear, F::Bicubic,
                                     F::Mitchell, F::Lanczos2,   F::Lanczos3, F::Area,
                                     F::Gaussian, F::Supersample};
        if (const Tool* t = m_tools.find(ToolId::Move))
            for (const ToolOption& o : t->options())
                if (o.id == "aa") {
                    const int i = static_cast<int>(o.value);
                    if (i >= 0 && i < static_cast<int>(std::size(kMap)))
                        return kMap[i];
                }
        return F::Auto;
    }

    // Re-run the CPU compositor on the current document and hand the result to the canvas. Called
    // ONLY from the frame loop (via the pending flag), never directly from an edit callback.
    // Bring every TextLayer's pixel + bounds caches up to date before a composite (the compositor
    // is font-free; docs/type-tool.md §5.4). A cheap no-op once the caches reflect the current
    // blocks. A text edit / font-hover just changed a TextLayer's content. The canvas re-rasters it
    // live each frame (recompositeNow -> ensureTextCaches); the layer-panel thumbnail, though, only
    // needs to be current once the gesture settles -- re-rendering it per event was a big slice of
    // the live-edit lag (rev 11). Mark it dirty + stamp the time; onFrame() flushes it once editing
    // pauses.
    void markTextThumbDirty() {
        m_textThumbDirty = true;
        m_lastTextEditTime = nowSeconds();
    }

    // The app-level default text language for spell-check + hyphenation: the Settings "default text
    // language" if set, else the OS locale, with "en" as the ultimate fallback. Fed to
    // TextShaper::setDefaultLanguage; a paragraph's own `language` still wins over it via
    // resolveLanguage.
    [[nodiscard]] std::string spellAppLanguage() const {
        if (!m_textLanguage.empty())
            return m_textLanguage;
        const std::string lang = core::text::detectSystemLanguage();
        return lang.empty() ? std::string{"en"} : lang;
    }

    // Drive the background spell-checker for the active text-edit session (deferred §2). Called
    // every frame: (re)arms a settle when the edited block changes, submits a whole-block rescan
    // once typing pauses (bumping the epoch so a stale in-flight result is dropped), and applies
    // the newest result whose epoch is still current onto the canvas squiggle overlay. No session
    // => nothing to do.
    void updateSpellCheck() {
        if (m_canvas == nullptr)
            return;
        if (!m_spellCheckEnabled) {
            // Disabled: make sure no squiggles linger, and reset so re-enabling forces a fresh
            // scan.
            if (m_spellActiveTarget != core::kInvalidLayerId) {
                m_canvas->setTextMisspelledRanges({});
                requestFrame();
            }
            m_spellActiveTarget = core::kInvalidLayerId;
            m_spellLastRev = static_cast<std::uint64_t>(-1);
            m_spellDirty = false;
            return;
        }
        const core::LayerId target = m_canvas->textEditTarget();
        if (target == core::kInvalidLayerId) {
            // Session ended: reset so re-entering ANY layer (even the same one) forces a fresh
            // scan; the canvas already cleared its squiggles when the session closed.
            m_spellActiveTarget = core::kInvalidLayerId;
            m_spellLastRev = static_cast<std::uint64_t>(-1);
            m_spellDirty = false;
            return;
        }
        core::Layer* l = m_document != nullptr ? m_document->find(target) : nullptr;
        auto* tl = l != nullptr ? l->as<core::TextLayer>() : nullptr;
        if (tl == nullptr)
            return;
        if (!m_spellWorker) // lazily spawned on first use (commit 5 gates this on the Settings
                            // toggle)
            m_spellWorker = std::make_unique<core::text::SpellCheckWorker>();

        // (Re)arm the settle whenever the target or its content changes.
        const std::uint64_t rev = tl->contentRevision();
        if (target != m_spellActiveTarget || rev != m_spellLastRev) {
            m_spellActiveTarget = target;
            m_spellLastRev = rev;
            m_spellDirty = true;
            m_spellDirtyTime = nowSeconds();
        }
        // Once editing pauses, submit a whole-block rescan (a newer epoch cancels any stale
        // result).
        if (m_spellDirty && nowSeconds() - m_spellDirtyTime >= kSpellSettleSec) {
            m_spellDirty = false;
            m_spellPendingEpoch = ++m_spellEpoch;
            core::text::SpellScanOptions opts;
            opts.checkAllCaps = m_spellCheckAllCaps;
            m_spellWorker->request(tl->block(), /*documentDefault=*/"", spellAppLanguage(), opts,
                                   m_spellPendingEpoch);
        }
        // Apply the newest completed result if it still matches the latest request.
        if (auto r = m_spellWorker->takeResult(); r && r->epoch == m_spellPendingEpoch) {
            m_canvas->setTextMisspelledRanges(std::move(r->ranges));
            requestFrame(); // redraw the overlay with the fresh squiggles
        }
    }

    // Force a spell rescan on the next frame even though the block content did not change -- used
    // after Add to Dictionary / Ignore All, which make a flagged word correct without editing it,
    // so the settle would otherwise never re-fire. updateSpellCheck() submits the scan (the word
    // now passes, its squiggle clears).
    void forceSpellRescan() {
        m_spellDirty = true;
        m_spellDirtyTime = -1.0e9; // the settle has "already" elapsed -> rescan next frame
    }

    // Returns the DOCUMENT-space rect the refresh visibly changed (empty = nothing re-rendered):
    // the typing region path (S60-b) unions it into its dirty rect; full composites ignore it.
    //
    // ⚠ [[nodiscard]] IS LOAD-BEARING, not tidiness. The report is made EXACTLY ONCE -- a second
    // call in the same frame finds the caches current and says nothing -- so a caller that drops it
    // does not merely lose an optimisation, it loses the only notice that pixels moved. That has
    // now shipped twice (MOSAIC_TILE_COMPOSITOR=1 in S60-a; the pre-drain settle in 0.3.2), both
    // times as invisible typing, so the compiler holds the invariant instead of the reviewer.
    // `settleTextCaches()` below is the disposition almost every caller wants.
    [[nodiscard]] common::Rect ensureTextCaches() {
        if (!m_document)
            return {};
        // The block being edited renders UNCLIPPED so its Area overflow stays visible while you
        // type; every other Area block clips overset text to its box (round-4 #3).
        const core::LayerId editing =
            m_canvas != nullptr ? m_canvas->textEditTarget() : core::kInvalidLayerId;
        // A transform gesture in flight freezes the baked text transform (item 8): the dragged
        // layer keeps its current crisp bake and the compositor resamples the small delta cheaply;
        // the gesture's commit (no longer live) re-renders the settled transform crisp.
        const bool liveDrag = m_canvas != nullptr && m_canvas->transformGestureActive();
        // DRAFT (half-res) rendering for the edited layer while its block is being re-rendered
        // per event anyway -- a live size/bend/path drag, or a font-hover preview. The draft cache
        // cannot read as current (its halved bake fails the linear key), so the gesture's release
        // / the hover settle lands a crisp pass the moment one is requested.
        const bool draft = (m_canvas != nullptr && m_canvas->textBlockEditGestureActive()) ||
                           m_textHoverDraft;
        common::Rect dirty{};
        {
            MOSAIC_PERF_SCOPE("Text cache refresh", Lane::Cpu);
            core::text::refreshTextCaches(*m_document, m_textShaper, m_fontDb, editing, liveDrag,
                                          &dirty, draft);
        }
        // Texture-generator caches ride the same pre-composite seam (S55-a): params edits and
        // canvas resizes re-render here; a current cache is a cheap revision check per layer.
        common::Rect texDirty{};
        {
            MOSAIC_PERF_SCOPE("Texture cache refresh", Lane::Cpu);
            core::texture::refreshTextureCaches(*m_document, &texDirty);
        }
        if (!texDirty.empty()) dirty = dirty.empty() ? texDirty : dirty.united(texDirty);
        return dirty;
    }

    // Refresh the caches and QUEUE what that visibly changed, so the next frame patches it. The
    // right disposition for every caller that is NOT itself about to composite: a menu command, a
    // panel build, an export flatten, the frame's own pre-drain settle. Over-queueing costs one
    // small region patch of already-correct pixels; under-queueing costs the edit. Returns whether
    // anything was queued, for the callers that also want to wake the frame loop.
    bool settleTextCaches() {
        const common::Rect dirty = ensureTextCaches();
        if (dirty.empty())
            return false;
        m_pendingRegion.add(dirty);
        return true;
    }

    // ---- S60-a item 13: the device-resident composite lane (opt-in, DEFAULT OFF) ---------------
    //
    // Everything below is dead code unless MOSAIC_TILE_COMPOSITOR=1 is in the environment: the
    // lane is never constructed, `m_tiles` stays null, and every seam that mentions it is a branch
    // not taken. THE FLIP IS NOT MADE HERE. `render::composite(..., Backend::Cpu)` still serves
    // every call site in this file; what this adds is the machinery the flip needs, so that turning
    // it on is a decision (docs/s60-performance-plan.md §7, "Item 13: the gate") and not a project.

    // Build the lane once, on the first frame where the Vulkan canvas actually has a renderer -- it
    // is created lazily inside renderFrame(), so on the first frames there is no device to adopt
    // and the CPU walk serves, exactly as it does today.
    void ensureResidentLane() {
        if (m_tileLaneTried || m_canvas == nullptr)
            return;
        render::WindowRenderer* wr = m_canvas->renderer();
        if (wr == nullptr)
            return; // not up yet; try again next frame (this costs one null check per frame)
        m_tileLaneTried = true;
        if (!residentCompositorRequested())
            return; // the ordinary case: no env opt-in, no lane, no cost
        m_tiles = ResidentComposite::createIfRequested(*wr);
    }

    // Is WindowRenderer's GPU drag pass armed? Read off the RENDERER, not off our own
    // `m_gpuDragActive` mirror: the question is "will drawFrame dispatch into the canvas texture
    // behind the lane's back this frame", and the renderer's own flag is the thing that decides
    // that. A mirror that drifted by one frame here would cost a wrong picture, not a slow one.
    [[nodiscard]] bool gpuDragPassArmed() const {
        if (m_canvas == nullptr)
            return false;
        const render::WindowRenderer* wr = m_canvas->renderer();
        return wr != nullptr && wr->gpuDragActive();
    }

    // Can the resident lane own THIS frame? Two app-side facts the compositor cannot see, each of
    // which means the canvas texture is not simply "the document composited":
    //   - a Recompose review shows a PREVIEW in its own coordinate space, not the document,
    //   - the GPU drag pass is armed, and it writes the canvas texture from inside drawFrame.
    //
    // ⚠ CHANNEL ISOLATION IS NOT ONE OF THEM ANY MORE (S60-a item 10), and it was the sharpest of
    // the three: closing one eye in the Channels tab dropped the WHOLE document back to the CPU
    // walk plus a full-canvas upload every frame, because the remap was a host pass over a copy of
    // the composite. It is now a handful of instructions in canvas_present.comp, applied where the
    // present pass samples the canvas texture -- which still holds the true composite either way,
    // so there is nothing left here for a gate to protect.
    //
    // ⚠ A LIVE MOVE DRAG IS NOT ONE OF THEM ANY MORE. It used to be the third entry -- every frame
    // of every Move gesture went to the CPU walk, which is what "moving text layers around is still
    // CPU lane" was. A drag changes a layer's transform and nothing else: the plan diff hashes
    // placement into Step::fingerprint, dirties the union of where the layer was and where it is,
    // and the layer's pixels never move, so the gesture uploads ZERO bytes. That is the lane's best
    // case, not its exception. What is left is the narrow physical fact above -- two writers, one
    // texture -- and it is decided once per gesture in driveTransformPreview.
    //
    // The decision itself is `ui::residentGateSkip`, a pure function over the facts below, so
    // its ORDER (which is what the once-per-reason log line names) is pinned by a test rather than
    // by the shape of this function.
    [[nodiscard]] ResidentSkip residentEligibility() const {
        ResidentGate g;
        g.haveLane = m_tiles != nullptr;
        if (!g.haveLane)
            return residentGateSkip(g); // the default build stops here; nothing below is evaluated
        g.haveDocument = m_document != nullptr && m_canvas != nullptr;
        g.haveRenderer = m_canvas != nullptr && m_canvas->renderer() != nullptr;
        g.recomposeReview = m_recomposeReview;
        g.gpuDragPass = gpuDragPassArmed();
        return residentGateSkip(g);
    }

    // Will the resident lane composite the Move gesture that is starting? Asked once, by
    // driveTransformPreview, to decide whether the GPU drag pass may arm at all.
    //
    // `serving()` -- the lane drew the PREVIOUS frame -- is the honest predictor and not merely a
    // convenient one: a Move drag changes a placement and nothing else, and the lane's refusals are
    // all structural (a group, an adjustment, layer effects, a leaf kind it cannot source, a caps
    // or budget floor). None of those can appear because a layer moved, so "it served the frame
    // before the press" implies "it will serve the frames after it". The eligibility gate is asked
    // as well because the app-side facts CAN change on the press (a Recompose review dropped or
    // entered) -- and note it is asked while the pass is still disarmed, so it is
    // answering about the lane, not about itself.
    [[nodiscard]] bool residentLaneOwnsDrag() const {
        return m_tiles != nullptr && m_tiles->serving() &&
               residentEligibility() == ResidentSkip::None;
    }

    // Serve one frame from the resident accumulator: composite the dirty macrotiles on the device
    // and resolve them straight into the present texture. False means the CPU walk owns this frame
    // -- always a clean fallback, never a failure -- and the reason has been logged once.
    [[nodiscard]] bool residentRecompositeNow(bool fitView) {
        const ResidentSkip pre = residentEligibility();
        if (pre != ResidentSkip::None) {
            // ⚠ This return is BEFORE the cache refresh below, deliberately: nothing one-shot has
            // been consumed yet, so the CPU fallback in this same frame refreshes the caches itself
            // and gets the dirty report first-hand. Anything added above this line that CONSUMES
            // must hand back, exactly like the refusal path does.
            if (m_tiles != nullptr) {
                ResidentServeResult r;
                r.skip = pre;
                m_tiles->noteCpuFrame();
                m_tiles->logSkipOnce(r);
            }
            return false;
        }
        MOSAIC_PERF_SCOPE("Composite (resident)", Lane::Gpu);
        // Text/texture layers keep their pixel caches on the CPU, but the refresh is what makes
        // their bounds honest -- and it is a no-op when nothing changed.
        //
        // ⚠ THE RETURN VALUE IS NOT OPTIONAL. The refresh reports the doc-space band it re-rendered
        // (old cache extent ∪ new) exactly ONCE: a second call in the same frame finds the caches
        // current and reports nothing. Swallowing it here is what made typing invisible under
        // MOSAIC_TILE_COMPOSITOR=1 -- the lane refused every document with a text layer back then,
        // the CPU fallback ran in the same frame, asked the refresh again, got an empty rect,
        // unioned it with requestTextRecomposite's empty seed and patched NOTHING. The lane sources
        // text leaves now, so the hand-back fires less often and matters exactly as much.
        const common::Rect textDirty = ensureTextCaches();
        // ... and the lane gets it too, on the way in. Over-dirtying the resident set costs time,
        // never pixels; the case it covers is a text/texture cache re-render that does NOT move the
        // layer's contentRevision -- the draft→crisp bake, the font-hover preview -- which the plan
        // diff cannot see. Harmless while the lane still refuses these leaves; load-bearing the day
        // it composites them from those same CPU-side caches.
        if (!textDirty.empty())
            m_tiles->markDirty(textDirty);
        render::CompositeOptions opts;
        opts.checkerboard = false; // the present shader draws the checker in screen space (S19-c)
        opts.resampleFilter = currentResampleFilter();
        // ⚠ CHARACTER-FOR-CHARACTER recompositeNow's expression, and it has to stay that way: the
        // two lanes must resolve the SAME filter for the same frame or a fallback would visibly
        // change the picture's sharpness rather than only its cost. Auto drops to cheap Bilinear
        // while a Move gesture (single or multi-layer) is in flight, and S33's heavy blur kernels
        // draft mid-scrub.
        //
        // And the RELEASE frame really does snap back, on this lane as on the other one. The
        // gesture's end clears transformGestureActive() before it queues its recomposite, so that
        // frame plans with liveDrag=false; `resolveTileFilter(opts.resampleFilter, place, liveDrag)`
        // is hashed into Step::fingerprint (tile_compositor.cpp, `filterI`), so the plan diff sees
        // the filter change even though the placement is identical to the last drag frame, and
        // re-composites the layer's footprint at full quality. The one case where nothing is
        // re-composited is the one where nothing needs to be: a linear-identity placement on the
        // integer grid resolves to Nearest under BOTH values, and Nearest there is an exact
        // whole-pixel copy, so the cheap frame and the quality frame are the same pixels.
        opts.liveDrag = (m_canvas != nullptr && m_canvas->transformGestureActive()) ||
                        blurScrubInFlight();
        const ResidentServeResult r =
            m_tiles->serve(*m_document, opts, *m_canvas->renderer(), m_canvas->anyGestureActive());
        m_tiles->logSkipOnce(r);
        if (!r.served) {
            // The refusal is ordinary; losing the refresh's one report would not be. Give the band
            // back so the CPU fallback later in THIS onFrame pass patches it (recompositeRegionNow
            // asks the refresh again and is told nothing changed, because nothing has since).
            m_pendingRegion.add(textDirty);
            return false;
        }
        // The pixels are already in the canvas texture; all that is left is the view bookkeeping
        // (and cancelling anything the CPU lane had queued for this frame).
        m_canvas->adoptResidentDocument(m_document->width(), m_document->height(), fitView);
        // From here the CANVAS is the device's and `m_lastComposite` is only a lazily materialised
        // mirror of it -- possibly several revisions behind, since materialise() is memoised and
        // skips gesture frames. A dirty-region patch composited onto that base would land fresh
        // pixels on a stale image, so recompositeRegionNow refuses one until a full CPU composite
        // has re-established the mirror. Cleared there, and nowhere else.
        m_hostCompositeStale = true;
        bumpCompositeRevision();
        syncSelection();
        // ⚠ `r.uploadBytes` staying at 0 across a stream of frames IS residency; a steady non-zero
        // means the source cache is being re-sent and the totals are measuring something else. It
        // is not a profiler row because a byte count is not a duration -- read it off
        // TileCompositeStats / the `Tile upload` DEV row instead (plan §7, condition 4).
        //
        // A MOVE DRAG is the sharpest reading of that number, now that this lane serves one: the
        // gesture changes placements only, so every frame of it must report 0. The two honest
        // exceptions both land on the RELEASE frame, not during the drag: a text or texture leaf
        // re-bakes at the settled transform (item 8 freezes the bake while the gesture is live), so
        // its cacheGeneration moves and its source is re-sent once.
        return true;
    }

    // The host-side view of the composite (S60-a item 12; docs/s60-readback-consumers.md §10).
    //
    // With the resident lane off -- the default -- this returns `m_lastComposite` and does nothing
    // else, so every consumer below behaves exactly as it always has. With the lane on, the pixels
    // live on the device and `m_lastComposite` becomes a lazily materialised MIRROR: this is the
    // one place that brings it back, memoised on the accumulator's revision, named by the consumer
    // that asked, and counted in the profiler.
    //
    // ⚠ It is NOT for a consumer that fires per pointer event. That is the status-bar cursor
    // readout, and it composites its one pixel on the CPU instead -- see updateCursorReadout. A
    // per-event consumer cannot be served from the device at any frequency worth having.
    const common::Image& hostComposite(std::string_view consumer,
                                       render::Freshness freshness = render::Freshness::AnyRecent) {
        if (m_tiles != nullptr)
            m_tiles->materialise(m_lastComposite, consumer, freshness);
        return m_lastComposite;
    }

    // A pixel edit landed on `id`, at `layerRect` in the LAYER'S OWN pixel space (empty == the
    // whole layer). One funnel, so the compositor's dirty set can never drift from the edit seams
    // that feed it. A no-op without the lane.
    //
    // ⚠ Not load-bearing for correctness: TileCompositor re-sends any layer whose contentRevision
    // moved without an accompanying rect. This is what turns a 256 px dab into 256 KiB of transfer
    // instead of a whole layer -- the row the S60-a gate's condition 3 is read on.
    void noteLayerPixelsChanged(core::LayerId id, const common::Rect& layerRect) {
        if (m_tiles == nullptr || !m_document || id == core::kInvalidLayerId)
            return;
        core::Layer* l = m_document->find(id);
        auto* raster = l != nullptr ? l->as<core::RasterLayer>() : nullptr;
        if (raster == nullptr)
            return;
        m_tiles->markLayerPixels(*raster, layerRect);
    }

    void recompositeNow(bool fitView) {
        if (!m_document)
            return;
        // A document change mid-review invalidates the reviewed Recompose (it was computed from
        // the previous composite): drop it — the fresh composite below takes the display back.
        if (m_recomposeReview)
            exitRecomposeReviewState();
        // THE one legitimate discard in this file: the composite three lines down redraws the whole
        // canvas, so a rect naming part of it adds nothing. Every other caller queues.
        (void)ensureTextCaches();
        render::CompositeOptions opts;
        // S19-c: the display composite carries TRUE alpha now -- the transparency checkerboard is
        // drawn in screen space by the present shader, not baked here in doc space. This also makes
        // the cursor colour readout report the real pixel value instead of the checker grey.
        opts.checkerboard = false;
        // Transform Anti-aliasing: the Move tool's "Anti-aliasing" choice picks how rotated/scaled
        // layers are resampled; liveDrag drops Auto to cheap Bilinear while a Move gesture is in
        // flight (single or multi-layer) and snaps back to full quality once it commits.
        opts.resampleFilter = currentResampleFilter();
        opts.liveDrag = (m_canvas != nullptr && m_canvas->transformGestureActive()) ||
                        blurScrubInFlight();  // S33: heavy blur kernels draft mid-scrub
        // A live Move drag composites through the drag-scoped cache (S15-b): only the dragged
        // layer is re-rasterised, the rest of the stack replays from cached buffers —
        // bit-identical to the full walk, which stays the fallback (nested targets, budget).
        std::optional<common::Image> image;
        const core::LayerId dragTarget = m_canvas->activeDragLayer();
        if (dragTarget != core::kInvalidLayerId)
            image = m_dragCache.composite(*m_document, dragTarget, opts);
        if (!image) {
            render::CompositeResult composite =
                render::composite(*m_document, opts, render::Backend::Cpu);
            if (!composite.ok) {
                uiLog().warn("composite failed: {}", composite.error);
                return;
            }
            image = std::move(composite.image);
        }
        m_lastComposite = std::move(*image); // kept for the cursor colour readout (S13-b)
        // The host walk just wrote the whole document, so the mirror IS the canvas again -- this is
        // the one place that can say so, and the frame the resident→CPU handover converges on.
        m_hostCompositeStale = false;
        bumpCompositeRevision();
        presentComposite(m_lastComposite, fitView); // through the active channel isolation, if any
        syncSelection(); // covers selection changes that arrive via edits/undo/redo
    }

    // S60-a dirty-region recomposite (queue side): mark the document-pixel rect `dirty` for a
    // region re-composite on the next frame, coalescing several edits in one frame into a single
    // pass over their union. Live brush strokes fire one of these per FL_DRAG event -- several can
    // arrive between two frames -- so doing the work here (per event) instead of once per frame
    // re-composited the same area repeatedly and re-throttled the UI on a big canvas. The actual
    // pass runs in onFrame via recompositeRegionNow.
    void recompositeRegion(const common::Rect& dirty) {
        if (dirty.empty())
            return;
        // The empty-seed invariant (requestTextRecomposite queues a pass with no rect, and the
        // refresh supplies it at frame time) lives in PendingRegion, not in each caller.
        m_pendingRegion.add(dirty);
        requestFrame(); // present the patch this frame, not at the next heartbeat
    }

    // A text edit dirtied a TextLayer, but the affected rect is only knowable when the cache
    // refreshes (the OLD pixels' extent ∪ the NEW's -- ensureTextCaches reports it): queue a
    // region pass with an EMPTY seed; recompositeRegionNow unions in whatever the refresh reports.
    // This is the typing path (S60-b): a keystroke used to pay recompositeNow's FULL document
    // composite (~64 ms at 1920x1080 -- "typing is extremely laggy", user 2026-07-14); the region
    // pass pays for the text band alone (a few ms).
    void requestTextRecomposite() {
        m_pendingRegion.queueUnnamed();
        requestFrame();
    }

    // The region re-composite itself (drained once per frame by onFrame). Re-composites ONLY
    // `dirty` through the full layer stack and patches just that rect of the canvas texture + the
    // cached composite, instead of re-compositing and re-uploading the whole document. Falls back
    // to a full requestRecomposite whenever the region path can't run -- no full composite has
    // established the canvas texture yet, the document size moved out from under m_lastComposite,
    // the resident lane drew the last frame (so m_lastComposite is a mirror, not the canvas), or a
    // Move drag owns the composite (the drag cache, not the dirty rect, scopes that). Returns the
    // seconds it took (0 on the no-op / fallback paths) so the caller can budget the inpaint
    // live-preview throttle.
    double recompositeRegionNow(const common::Rect& dirty) {
        // ⚠ m_hostCompositeStale is the resident→CPU handover's guard, and it is not an
        // optimisation: patching a region into a mirror that lags the canvas by an unknown number
        // of revisions puts fresh pixels on a stale image, and every host consumer (histogram,
        // wand "All Layers", Smart Resize) then reads a composite that was never on screen. The
        // full composite it asks for instead costs one frame and clears the flag, so the handover
        // converges rather than repeating -- while the lane is serving, this function is not
        // reached at all (residentRecompositeNow drains the queue).
        if (!m_document || m_canvas == nullptr || m_hostCompositeStale || m_lastComposite.empty() ||
            m_lastComposite.width != m_document->width() ||
            m_lastComposite.height != m_document->height() ||
            m_canvas->activeDragLayer() != core::kInvalidLayerId) {
            requestRecomposite(/*fitView=*/false);
            return 0.0;
        }
        const auto t0 = std::chrono::steady_clock::now();
        // The refresh reports what it visibly changed (old cache extent ∪ new, in doc px): union
        // it in, so a text edit's region pass covers the re-rendered band even though the edit
        // itself could not know the rect (requestTextRecomposite queues an empty seed).
        const common::Rect textDirty = ensureTextCaches();
        common::Rect region = dirty;
        if (!textDirty.empty())
            region = region.empty() ? textDirty : rectUnion(region, textDirty);
        if (region.empty()) // an empty seed and the refresh changed nothing: nothing to patch
            return 0.0;
        render::CompositeOptions opts;
        opts.checkerboard = false; // the present shader draws the checker in screen space (S19-c)
        opts.resampleFilter = currentResampleFilter();
        opts.liveDrag = m_canvas->transformGestureActive();
        const render::CompositeResult r =
            render::compositeRegion(*m_document, region, opts, render::Backend::Cpu);
        if (!r.ok) // empty / out-of-canvas rect: nothing to patch
            return 0.0;
        // compositeRegion floors/clamps the rect to the canvas; its result sits at this origin.
        const auto x0 = static_cast<std::uint32_t>(std::max(0.0, std::floor(region.x)));
        const auto y0 = static_cast<std::uint32_t>(std::max(0.0, std::floor(region.y)));
        patchComposite(r.image, x0, y0);            // keep the cursor-readout cache coherent (pristine)
        presentCompositeRegion(r.image, x0, y0);    // partial GPU upload, through the channel view
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    // Smallest rect covering both `a` and `b` (their union's bounding box). For coalescing dirty
    // regions; assumes both are non-empty (the caller guards empties).
    [[nodiscard]] static common::Rect rectUnion(const common::Rect& a, const common::Rect& b) {
        const double x0 = std::min(a.x, b.x);
        const double y0 = std::min(a.y, b.y);
        const double x1 = std::max(a.right(), b.right());
        const double y1 = std::max(a.bottom(), b.bottom());
        return {x0, y0, x1 - x0, y1 - y0};
    }

    // m_lastComposite changed: bump the revision EVERY consumer keyed on it, and tell the ones
    // that need telling. Both mutation sites (the full recomposite and the dirty-region patch) go
    // through here -- the region path used to bump the revision and skip the notify, so the
    // Channels histogram stayed stale through brush strokes, typing and inpaint for as long as
    // the tab remained open (found by the S60-a readback audit, 2026-07-23).
    void bumpCompositeRevision() {
        ++m_compositeRevision; // the Smart Resize analysis cache keys on this
        // ... and so does the eyedropper/wand merged-source memo, which unlike the importance map
        // is a whole document-sized RGBA image (160 MB at 5000x8000). Keying it would be enough to
        // keep it CORRECT; releasing it here keeps it from staying RESIDENT between edits. It is
        // only ever populated on wandMergedSource's degraded path (no usable host mirror), so on
        // every ordinary bump -- every dab, every keystroke -- this is one empty-check and nothing
        // else, and on the degraded path the walk that refills it dwarfs the free.
        if (!m_mergedSource.empty())
            m_mergedSource = common::Image{};
        m_mergedSourceRev = 0;
        // The histogram re-bins iff the revision moved AND the Channels tab is visible (else it is
        // deferred to onTabShown) -- so this is on-change, never per frame.
        if (m_layerPanel != nullptr)
            if (ChannelsPanel* chans = m_layerPanel->channels())
                chans->notifyChanged(m_compositeRevision);
    }

    // Copy `sub` into m_lastComposite at (x,y) so the cursor colour readout stays consistent with
    // the on-screen pixels after a dirty-region patch (the GPU texture is patched separately).
    void patchComposite(const common::Image& sub, std::uint32_t x, std::uint32_t y) {
        if (sub.empty() || x + sub.width > m_lastComposite.width ||
            y + sub.height > m_lastComposite.height)
            return;
        bumpCompositeRevision();
        for (std::uint32_t row = 0; row < sub.height; ++row) {
            const std::uint8_t* src = &sub.rgba[static_cast<std::size_t>(row) * sub.width * 4];
            std::uint8_t* dst =
                &m_lastComposite
                     .rgba[(static_cast<std::size_t>(y + row) * m_lastComposite.width + x) * 4];
            std::memcpy(dst, src, static_cast<std::size_t>(sub.width) * 4);
        }
    }

    // ---- On-canvas channel isolation (Channels tab) -------------------------------------------
    // The Channels tab's eyes can isolate/hide colour channels on the DISPLAY. Since S60-a item 10
    // that is a PRESENT-PASS PARAMETER, not a pixel edit: canvas_present.comp applies the view
    // where it samples the canvas texture (ui::channelViewShaderCode -> setChannelView), so nothing
    // on this side ever copies or remaps a composite. Three things fall out of that, and each of
    // them used to be a cost or a defect:
    //   - m_lastComposite AND the canvas texture both keep holding the TRUE composite, which is
    //     what the histogram, the cursor colour readout and Smart Resize read;
    //   - the remap applies to whatever WROTE that texture -- the resident tile resolve, a host
    //     upload, or the renderer's GPU drag pass -- so no lane needs an exception for it, and a
    //     drag under an isolation no longer shows unisolated colour;
    //   - closing an eye costs nothing: no copy, no O(pixels) pass, no recomposite, no upload.
    [[nodiscard]] ChannelsPanel* channelsPanel() const {
        return m_layerPanel != nullptr ? m_layerPanel->channels() : nullptr;
    }
    // Push the tab's current view to the renderer. Once per FRAME, not on change: the renderer is
    // created lazily inside renderFrame() and destroyed on hide(), so a push-on-change would be
    // lost across either edge. It is a null check and a uint store.
    void syncChannelView() {
        if (m_canvas == nullptr)
            return;
        render::WindowRenderer* wr = m_canvas->renderer();
        if (wr == nullptr)
            return; // no device yet; the next frame catches up, and the default view is normal
        const ChannelsPanel* c = channelsPanel();
        wr->setChannelView(c != nullptr ? c->viewShaderCode() : kChannelViewNormal);
    }
    void presentComposite(const common::Image& composite, bool fitView) {
        if (m_canvas == nullptr)
            return;
        // A host upload is about to write the canvas texture the resident lane resolves into, so
        // its next resolve has to cover the whole canvas rather than only what it recomposited --
        // the macrotiles it skips would otherwise keep showing this image. One funnel for both
        // present seams, so a new CPU-upload path cannot forget.
        if (m_tiles != nullptr)
            m_tiles->noteCpuFrame();
        // The full-canvas upload. Not covered by any --bench scenario -- those stop at the
        // composite -- so the profiler is the only place this cost is visible at all.
        MOSAIC_PERF_SCOPE("Canvas upload (full)", Lane::Gpu);
        // The TRUE composite goes up, isolation or not: the channel view is applied on the way OUT.
        m_canvas->setDocumentImage(composite, fitView);
    }
    void presentCompositeRegion(const common::Image& sub, std::uint32_t x, std::uint32_t y) {
        if (m_canvas == nullptr)
            return;
        if (m_tiles != nullptr)
            m_tiles->noteCpuFrame(); // see presentComposite: the host is writing the texture
        MOSAIC_PERF_SCOPE("Canvas upload (region)", Lane::Gpu);
        m_canvas->setDocumentRegion(sub, x, y);
    }
    // The Channels tab's isolation-changed hook. There is nothing to re-mask, re-upload or
    // recomposite -- the view is a present-pass parameter, and onFrame pushes the new code just
    // before the pass that reads it. All a toggle owes is a frame.
    void refreshCanvasForIsolation() { requestFrame(); }

    // Hand the document's current selection mask to the canvas (the marching-ants present pass,
    // S13). Called by the Select actions and from recompositeNow, so undo/redo and panel-driven
    // selection edits (shift-click a thumbnail) stay in sync without extra plumbing.
    void syncSelection() {
        if (!m_document || m_canvas == nullptr)
            return;
        // Key the status-bar selection preview off the document's LIVE empty state every refresh, so
        // a cleared/empty selection can never leave a stale readout behind if the revision gate below
        // short-circuits (isEmpty() is O(1); the full bounds() rescan stays gated). While the Crop
        // tool is active it OWNS this slot as a live crop-size readout (onCropRectChanged), so leave
        // it be there.
        if (m_statusBar && !m_canvas->cropToolActive() && m_document->selection().isEmpty())
            m_statusBar->setSelectionBounds(std::nullopt);
        if (m_document->selectionRevision() == m_syncedSelectionRev)
            return; // unchanged: skip the mask copy + upload + bounds rescan (per-frame path)
        m_syncedSelectionRev = m_document->selectionRevision();
        const core::Selection& sel = m_document->selection();
        if (sel.isEmpty())
            m_canvas->setSelectionMask(0, 0, nullptr);
        else
            m_canvas->setSelectionMask(sel.width(), sel.height(), sel.data().data());
        if (m_statusBar)
            m_statusBar->setSelectionBounds(sel.bounds()); // nullopt when none/nothing selected
    }

    // Show `text` in the status bar's free-form slot and clear it after a few seconds (unless
    // something newer replaced it -- re-arming cancels the pending clear). For lightweight
    // feedback like "copy found nothing"; long worker operations own setStatus directly (§4).
    void transientStatus(const std::string& text) {
        if (m_statusBar == nullptr)
            return;
        m_statusBar->setStatus(text);
        Fl::remove_timeout(statusClearTimer, this);
        // Hold long enough to read it: at least 4 s, but a message too long to fit stays up until
        // it has scrolled through once (else it would vanish mid-scroll).
        const double hold = std::max(4.0, m_statusBar->statusScrollSeconds() + 0.5);
        Fl::add_timeout(hold, statusClearTimer, this);
    }

    static void statusClearTimer(void* self) {
        auto* win = static_cast<MainWindow*>(self);
        if (win->m_statusBar != nullptr)
            win->m_statusBar->setStatus("");
    }

    // After an edit made outside the panel (undo/redo): re-composite and rebuild the layer list.
    // Also the drag cache's invalidation point (S15-b): every path that changes anything other
    // than the dragged transform (drag end, undo/redo, panel edits, paste, merge) runs through
    // here, so the cache can never replay stale buffers. It rebuilds lazily on the next drag.
    void syncAfterEdit() {
        m_dragCache.invalidate();
        requestRecomposite(/*fitView=*/false);
        if (m_layerPanel)
            m_layerPanel->refresh();
        // The canvas size can change under an edit now (crop apply/undo/redo, S16): keep the
        // status bar's document readout honest.
        if (m_statusBar && m_document)
            m_statusBar->setDocumentInfo(m_document->width(), m_document->height(),
                                         m_document->dpi(),
                                         core::precisionName(m_document->precision()));
    }

    // Push a pixel edit whose changed region is known and scope the recomposite to it (S60-a)
    // instead of the whole document. The live preview (brush) may already show the result; the
    // scoped recomposite cheaply guarantees the committed pixels match the screen. Falls back to a
    // full recomposite if the command can't report a region (layer gone / transformed off-canvas).
    void pushScopedPixelEdit(std::unique_ptr<core::Command> cmd) {
        if (!m_document)
            return;
        const std::optional<common::Rect> region = cmd->dirtyRegion(*m_document);
        // ... and the SAME edit in the layer's own pixel space, for the resident lane's incremental
        // upload (S60-a item 13). Read before push() because the claim describes the rect the
        // command STORES, not the document AABB it projects onto; applied after, because the
        // contract is "bump contentRevision first, then name the rect". Asked for only when there
        // is a lane to tell, so a build without the opt-in does not even pay the virtual call.
        std::optional<core::LayerPixelEdit> claim;
        if (m_tiles != nullptr)
            claim = cmd->dirtyLayerPixels(*m_document);
        m_document->commands().push(std::move(cmd));
        if (claim)
            noteLayerPixelsChanged(claim->layer, claim->rect);
        m_dragCache.invalidate();
        if (region)
            recompositeRegion(*region);
        else
            requestRecomposite(/*fitView=*/false);
        if (m_layerPanel)
            m_layerPanel->refresh();
    }

    // After an undo/redo/history-jump: recomposite only the region the stepped command(s) touched
    // (S60-a) when known -- a brush-stroke undo no longer drags the whole document through the CPU
    // compositor -- else fall back to a full recomposite. Mirrors syncAfterEdit's bookkeeping.
    void syncAfterUndoRedo() {
        m_dragCache.invalidate();
        const std::optional<common::Rect> region =
            m_document ? m_document->commands().lastAffectedRegion() : std::nullopt;
        // The stepped commands' layer-local claims, so undoing a stroke re-uploads the stroke's
        // macrotiles rather than the layer (S60-a item 13). Same funnel as pushScopedPixelEdit.
        if (m_tiles != nullptr && m_document) {
            if (region)
                for (const core::LayerPixelEdit& e : m_document->commands().lastAffectedLayerEdits())
                    noteLayerPixelsChanged(e.layer, e.rect);
            else
                // A step that cannot name a region is STRUCTURAL, and the worst of those swaps the
                // whole layer tree (the loaded-save-history branch), handing back layers that keep
                // their ids and restart their content revision -- which a cache keyed by
                // (id, revision) reads as "nothing changed". See noteLayerTreeReplaced().
                m_tiles->noteLayerTreeReplaced();
        }
        if (region)
            recompositeRegion(*region);
        else
            requestRecomposite(/*fitView=*/false);
        if (m_layerPanel)
            m_layerPanel->refresh();
        if (m_statusBar && m_document)
            m_statusBar->setDocumentInfo(m_document->width(), m_document->height(),
                                         m_document->dpi(),
                                         core::precisionName(m_document->precision()));
    }

    static void frameTimer(void* self) { static_cast<MainWindow*>(self)->onFrame(); }

    void onFrame() {
        // The armed frame is being spent HERE, so the flag is cleared before the work: a
        // requestFrame() made from INSIDE this frame (the text-thumbnail settle queues one, and so
        // does every edit a frame's own callbacks trigger) has to be able to arm the NEXT frame
        // rather than be absorbed as a duplicate of this one.
        m_frameArmed = false;
        // The wall-clock interval since the previous frame feeds the FPS/frame-time readouts; guard
        // the very first frame (no prior stamp) so it does not report a nonsense multi-second gap.
        // NOT debug-gated: the Timing Profiler window's own header line is frame rate and frame
        // time, so a release --profile run without this would open a window that cannot say how
        // fast the app is running.
        //
        // ⚠ This measures PRESENTS, not requests, and that is the point: every path out of this
        // function ends in one renderFrame() (there is no early return between here and it), while
        // the ~130 things that ASK for a frame are coalesced upstream. So the readout answers "how
        // often is the screen being redrawn", which is the number a user comparing it against their
        // monitor's refresh rate is reading it as.
        const double frameNow = nowSeconds();
        const double frameIntervalMs =
            m_prevFrameStamp > 0.0 ? (frameNow - m_prevFrameStamp) * 1000.0 : 0.0;
        m_prevFrameStamp = frameNow;
        m_lastFrameTime = frameNow; // the pacing interval is measured from here
        if (Profiler::enabled())
            Profiler::instance().frameTick(frameIntervalMs);
        m_canvas->flushMoveDrag();     // the Move gesture lands here, before the recomposite check,
                                       // so the composite drawn this frame is current (S15)
        m_canvas->flushShapeBoxDrag(); // ... and the selected shape's resize/transform (S26-b §7.1)
        // The renderer's one-time start-up notice (S60-b Level 2, plan §6.2) -- today: "this is a
        // software rasterizer". Polled per frame because the renderer is created lazily inside
        // renderFrame(), so the first frame that HAS one is not knowable in advance;
        // takeStartupNotice() empties itself, so the user is told exactly once and this costs one
        // null check and one empty-string test thereafter.
        if (render::WindowRenderer* wr = m_canvas->renderer(); wr != nullptr) {
            if (std::string notice = wr->takeStartupNotice(); !notice.empty())
                transientStatus(notice);
        }
#ifdef __APPLE__
        // Documents double-clicked in Finder while Mosaic is already running (macOpenDocumentCallback).
        // Drained HERE rather than from the callback itself: it fires from an Apple Event handler,
        // and opening a document runs the whole recovery-face machinery -- modals included -- which
        // has no business executing inside someone else's event dispatch.
        if (std::vector<std::string>& queued = macPendingOpens(); !queued.empty()) {
            const std::vector<std::string> paths = std::move(queued);
            queued.clear();
            for (const std::string& path : paths)
                openDocumentAtPath(path);
        }
#endif
        // ---- Pre-drain settle -------------------------------------------------------------
        //
        // These two used to run AFTER the recomposite drain below, which cost a whole extra
        // full-canvas composite: each one invalidates the picture, so the frame that had just
        // composited was immediately out of date and a second composite had to follow. Opening the
        // S60 fixture paid 2 x 23.6 s for that. Running them FIRST lets the same frame's drain
        // absorb their work, and one composite serves.
        //
        // ⚠ ensureTextCaches() is why they could not simply be moved. It is otherwise called only
        // from recompositeNow, so before the drain the text pixel caches may not exist yet --
        // buildBelowComposite would then mirror a document with no text in it, and the path re-bake
        // would measure an unshaped block. Calling it explicitly here removes that dependency; the
        // drain's own call then finds the caches current and costs a revision check per layer.
        //
        // Path fits BEFORE the reflection env, which is also the correct order rather than merely
        // the cheap one: the mirror should see settled path text. The old arrangement built the
        // snapshot first and re-flowed the text after it, leaving the mirror a frame stale.
        //
        // ⚠ AND THE REPORT IS QUEUED, NOT SWALLOWED. This is the third site in this file that has
        // to say so, and the one that shipped broken (0.3.2): the refresh names the band it
        // re-rendered EXACTLY ONCE, and moving a call ahead of the drain moved that one report
        // ahead of it too. The drain's own call then found the caches current, reported nothing,
        // unioned nothing with requestTextRecomposite's empty seed and patched NOTHING -- so every
        // edit that rides the region path (typing, a style change, a 3D param, the font-hover
        // preview, the edit-enter/leave clip flip) re-rendered the cache and never reached the
        // canvas. Feeding it into m_pendingRegion is what makes an EARLIER call harmless: whoever
        // asks first, the band still lands in this frame's queue.
        settleTextCaches();
        updateTextPathFits();  // fit-to-path (§9): re-bake + re-flow path text when its path moved
        updateReflectionEnv(); // 3D reflect-canvas: build the below-composite snapshot when needed

        // S60-a item 13: build the resident lane on the first frame that has a renderer to adopt,
        // and roll the readback budget. Both are no-ops without MOSAIC_TILE_COMPOSITOR=1 -- the
        // first is one null check, the second does not happen at all.
        ensureResidentLane();
        if (m_tiles != nullptr)
            m_tiles->beginFrame(m_canvas != nullptr && m_canvas->anyGestureActive());
        // The resident lane serves the FULL and the REGION request alike: its dirty set already
        // scopes the work, so a queued region is a queued composite with the tiles marked. The
        // short-circuit on `m_tiles` is what makes the default build reach the two branches below
        // with nothing evaluated and nothing changed.
        bool residentServed = false;
        double residentCost = 0.0;
        if (m_tiles != nullptr && (m_recompositePending || m_pendingRegion.queued)) {
            const auto t0 = std::chrono::steady_clock::now();
            // ⚠ A refusal in here can GROW m_pendingRegion (it hands back the band the text/texture
            // cache refresh reported, which the fallback below could no longer obtain), so the two
            // branches must read the queue AFTER this call, not before.
            residentServed = residentRecompositeNow(m_pendingFitView);
            residentCost =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
        if (residentServed) {
            m_lastRecompositeCost = residentCost;
            // The same two rows the CPU branch records, on Lane::Gpu -- so a --profile run shows
            // the two lanes side by side under names that already mean something, and the
            // gesture-end stall (docs/s60-gesture-start-stall.md) stays measurable across the flip.
            Profiler::instance().record("Composite (full)", Lane::Gpu,
                                        m_lastRecompositeCost * 1000.0);
            if (m_gestureEndPending) {
                Profiler::instance().record("Move gesture end (composite+upload)", Lane::Gpu,
                                            m_lastRecompositeCost * 1000.0);
                m_gestureEndPending = false;
            }
            m_recompositePending = false;
            m_pendingFitView = false;
            m_pendingRegion.clear();
        } else if (m_recompositePending) { // coalesced: rapid edits collapse into one composite per frame
            const auto t0 = std::chrono::steady_clock::now();
            recompositeNow(m_pendingFitView);
            m_lastRecompositeCost =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            // Release-available since S60-alpha (runtime-gated): the composite cost is THE
            // number this arc exists to move, and it has to be measurable in the build the
            // user actually runs.
            Profiler::instance().record("Composite (full)", Lane::Cpu,
                                        m_lastRecompositeCost * 1000.0);
            // ... and, when this drain is the one a Move/Resize/Rotate gesture queued on release,
            // the SAME cost under its own name. Two rows on purpose: "Composite (full)" is the
            // population, "Move gesture end" is the once-per-gesture stall that hid inside it --
            // read its max/last, not its avg (docs/s60-gesture-start-stall.md §6.1).
            if (m_gestureEndPending) {
                Profiler::instance().record("Move gesture end (composite+upload)", Lane::Cpu,
                                            m_lastRecompositeCost * 1000.0);
                m_gestureEndPending = false;
            }
            m_recompositePending = false;
            m_pendingFitView = false;
            m_pendingRegion.clear(); // a full recomposite supersedes any queued region
        } else if (m_pendingRegion.queued) { // coalesced dirty-region patch (brush / inpaint preview)
            const double cost = recompositeRegionNow(m_pendingRegion.rect);
            if (cost > 0.0)
                m_lastRecompositeCost = cost; // the inpaint-preview throttle budgets off this
            if (cost > 0.0)
                Profiler::instance().record("Composite (region)", Lane::Cpu, cost * 1000.0);
            m_pendingRegion.clear();
        }
        // The font-hover preview rendered DRAFT (half-res); once the pointer rests, land the crisp
        // pass. Clearing the flag BEFORE requesting is what makes the next region pass render at
        // full quality -- the draft cache cannot read as current, so the request re-renders.
        if (m_textHoverDraft && nowSeconds() - m_textHoverDraftAt >= kTextHoverCrispSec) {
            m_textHoverDraft = false;
            requestTextRecomposite();
        }
        // Deferred text-layer thumbnail (rev 11): once the live edit/font-hover has settled, bring
        // the edited layer's thumbnail current ONCE. The per-frame recomposite above already kept
        // the canvas live, so this is the only thing the gesture left stale. ensureTextCaches() is
        // a no-op when the last recomposite already refreshed the caches.
        if (m_textThumbDirty && nowSeconds() - m_lastTextEditTime >= kTextThumbSettleSec) {
            // ⚠ THE SAME SWALLOW, in a block that repaints THUMBNAILS and not the canvas. Usually
            // the refresh reports nothing (the frame's recomposite already did the work), but when
            // the settle is what lands the crisp bake after a draft gesture, this is the ONE report
            // of a band the canvas is still showing at half resolution -- and a report nobody acts
            // on is a report that is gone. Queue it; the next frame patches it.
            if (settleTextCaches())
                requestFrame();
            if (m_layerPanel != nullptr)
                m_layerPanel->refreshThumbnails();
            m_textThumbDirty = false;
        }
        // The adjustment row's scope preview, deferred the same way (S32): the live drag already
        // recomposites the canvas per frame; the thumbnail re-renders once the sliders rest
        // (cachedThumbnail's scope fingerprint sees the params drift and rebuilds just that row).
        if (m_adjustThumbDirty && nowSeconds() - m_lastAdjustEditTime >= kTextThumbSettleSec) {
            if (m_layerPanel != nullptr)
                m_layerPanel->refreshThumbnails();
            m_adjustThumbDirty = false;
        }
        updateSpellCheck();    // (deferred §2) rescan the edited block when it settles; apply the
                               // result
        updateJournal();       // (S48) autosave the recovery journal once editing settles
        updateAdjustmentPanel(); // S32: the editor follows the active adjustment layer
        updateSelectMorph();     // drop the morphology preview once its panel closes (Esc / clear)
        updateImageOps();        // S53: stage the Image-op overlay, and drop it when its panel goes
        // Keep the Type panel clear of the text it edits as that text MOVES/GROWS (the corner-flip
        // is only evaluated in place()). Re-place it when the edit box's screen bounds change --
        // but NEVER mid-drag: re-placing while a panel slider is being dragged moves the slider out
        // from under the cursor, and the relative scrub then jumps to the max (user report).
        // Fl::pushed()==nullptr means no widget is being dragged, so the panel re-settles on
        // release / after a discrete move instead.
        if (m_canvas != nullptr && Fl::pushed() == nullptr &&
            ((m_typePanel != nullptr && m_typePanel->shown()) ||
             (m_type3dPanel != nullptr && m_type3dPanel->shown()))) {
            const std::optional<common::Rect> box = m_canvas->textEditScreenBounds();
            if (box != m_lastTextEditBox) {
                m_lastTextEditBox = box;
                if (m_typePanel != nullptr && m_typePanel->shown())
                    m_typePanel->reanchor();
                if (m_type3dPanel != nullptr && m_type3dPanel->shown())
                    m_type3dPanel->reanchor(); // the 3D popup dodges the moved text too (round 3)
            }
        }
        updateWindowTitle(); // S18-d: ticks "unsaved for N min" off the frame timer (copy_label
                             // only fires when the minute/second actually rolls over)
#ifdef MOSAIC_DEBUG
        // The canvas-corner FPS overlay stays debug-only (it draws on the canvas); the "Present"
        // timing below does not -- it is the per-frame submit cost, which is exactly the sort of
        // number a --profile run exists to show.
        m_canvas->setFpsOverlay(m_showCanvasFps,
                                static_cast<int>(std::lround(Profiler::instance().fps())));
#endif
        // The Channels tab's on-canvas channel view (S60-a item 10) is a present-pass parameter, so
        // it is pushed HERE -- immediately before the pass that reads it -- rather than when an eye
        // is toggled: the renderer is created lazily inside renderFrame() and destroyed on hide(),
        // and a push-on-change would be lost across either edge. Costs a null check.
        syncChannelView();
        {
            MOSAIC_PERF_SCOPE("Present (submit)", Lane::Gpu);
            m_canvas->renderFrame();
        }
        if (m_timingGraph != nullptr && m_timingGraph->shown() != 0)
            m_timingGraph->redraw();
        reportCanvasErrorOnce();
        ++m_frameCount;
        if (m_autoQuitFrames > 0 && m_frameCount >= m_autoQuitFrames) {
            uiLog().info("auto-quit after {} frame(s)", m_frameCount);
            hide();
            return; // do not re-arm; Fl::run() returns once no window is shown
        }
        scheduleNextFrame();
    }

    // Apply icon pack `id` to the tool registry and re-rasterize the toolbar (S52). An unknown id
    // renders as the default pack -- the persisted CHOICE is the caller's to keep, so a pack folder
    // that reappears wins again on the next launch. Per-icon fallback happens inside IconPacks.
    void applyIconPack(const std::string& id) {
        std::string resolved =
            m_iconPacks.find(id) != nullptr ? id : std::string(kDefaultIconPackId);
        if (resolved == m_iconPackId)
            return; // includes startup with the default pack: the tools are already carrying it
        m_iconPackId = std::move(resolved);
        m_tools.applyIcons(
            [this](ToolId tool) { return m_iconPacks.iconForTool(m_iconPackId, tool); });
        if (m_toolbar != nullptr)
            m_toolbar->reloadIcons();
    }

    // If the Vulkan canvas failed to initialize, tell the user once. Skipped in the headless
    // smoke path (autoQuitFrames > 0) so a modal never blocks an automated frame-count run.
    void reportCanvasErrorOnce() {
        if (m_errorReported || !m_canvas->initFailed())
            return;
        m_errorReported = true;
        uiLog().error("canvas initialization failed: {}", m_canvas->lastError());
        if (m_autoQuitFrames == 0) {
            tellError(_("Hardware acceleration unavailable"),
                            _("Mosaic could not initialize its Vulkan canvas, so the image area "
                              "will not display. Check that your GPU drivers and the Vulkan loader "
                              "are installed and up to date."),
                            m_canvas->lastError());
        }
    }

    int m_dockWidth = kDockWidthDefault; // the width the user asked for; see effectiveDockWidth()
    bool m_dockRelayoutPending = false;  // a coalesced applyDockWidth() is queued (splitter drag)
    MenuBar* m_menu = nullptr;
    // The keymap (S51-b): the harvested defaults plus the user's sparse overrides. THE source for
    // every accelerator the menu carries, the plain-letter phase in handle(), and the text-editor
    // fence -- there is no second copy of a chord anywhere in this file.
    Keymap m_keymap;
    std::unique_ptr<MotivationTicker> m_ticker; // the menu-bar one-liner's line-pick + cadence driver
    VulkanCanvas* m_canvas = nullptr;
    RulerStrip* m_rulerH = nullptr;         // top ruler gutter (View -> Rulers)
    RulerStrip* m_rulerV = nullptr;         // left ruler gutter
    bool m_rulersVisible = false;           // runtime toggle (not persisted), like the pixel grid
    bool m_dndOpenPending = false; // an accepted file drop's FL_PASTE payload is on its way
    bool m_clipboardFetchPending = false; // fetchClipboardImage() is pumping for its reply (S55)
    bool m_clipboardFetchDone = false;    // that reply arrived (image or not)
    std::optional<common::Image> m_clipboardFetch; // where it lands
    std::vector<std::string> m_recentFiles; // canonical absolute paths, newest first (S55)
    std::vector<std::string> m_recentSizes; // custom-size tokens, newest first (round 5)
    // The right dock (§8.2) and, for convenience, the Layers panel inside it -- the panel the ~30
    // layer call sites already talk to. The dock OWNS it (it is an FLTK child); m_layerPanel is a
    // borrowed pointer, never deleted here.
    RightDock* m_dock = nullptr;
    LayerPanel* m_layerPanel = nullptr;
    ToolManager m_tools;              // tool registry + active-tool selection (S11)
    IconPacks m_iconPacks;            // installed tool icon packs (S52); m_iconPackId = applied one
    std::string m_iconPackId = std::string(kDefaultIconPackId);
    ColorState m_colors;              // active foreground/background colours (S11-d; tools read it)
    LeftToolbar* m_toolbar = nullptr; // the left tool column
    ToolOptionsBar* m_optionsBar = nullptr; // the options strip under the menu
    ColorPicker* m_colorPicker = nullptr;   // the swatch's flat-colour picker (a child sub-window)
    ToolFlyout* m_toolFlyout = nullptr;     // the toolbar slot variant flyout (a child sub-window)
    OptionsOverflowPopover* m_optionsOverflow =
        nullptr; // options-bar overflow list (S16-n sub-window)
    ToolbarOverflowPopover* m_toolbarOverflow =
        nullptr;                               // left-toolbar overflow list (S16-o sub-window)
    ShapeDesigner* m_shapeDesigner = nullptr;  // shape-designer popover (S26-b §7.4 sub-window)
    std::uint64_t m_shapeDesignerCoalesce = 0; // one designer session = one undo step
    TypePanel* m_typePanel = nullptr;          // Type panel popover (S29-c §8 sub-window)
    Type3dPanel* m_type3dPanel = nullptr;      // the 3D popup (S30-d §8.4 sub-window)
    ColorFlyout* m_panelColorFlyout = nullptr; // the panels' shared colour bubble (chip "Edit…")
    ScrubRuler* m_scrubRuler = nullptr; // options-bar slider precision HUD (a child sub-window)
    DropdownPopup* m_dropdownPopup = nullptr; // shared themed list for this window's Dropdowns
    StatusBar* m_statusBar = nullptr;         // the bottom readout strip (S13-b)
    common::Image m_iconImage;
    common::Image m_lastComposite; // the composite on the canvas; the cursor colour readout source
    // S60-a item 13: the device-resident composite lane, or NULL -- which is the default and is
    // what every build without MOSAIC_TILE_COMPOSITOR=1 in its environment has. While it is null
    // every seam that mentions it is a branch not taken, m_lastComposite is exactly what it has
    // always been, and the canvas is composited by render::composite(..., Backend::Cpu).
    //
    // ⚠ DESTRUCTION ORDER. This holds a VulkanContext borrowing the canvas's device. Members are
    // destroyed before the Fl_Group base deletes its children, so it goes before ~VulkanCanvas
    // takes the device down. Do not move it into the canvas.
    std::unique_ptr<ResidentComposite> m_tiles;
    bool m_tileLaneTried = false; // one creation attempt, on the first frame with a live renderer
    // Smart Resize analysis cache (S16-f): the fused importance map + automatic keep-regions of
    // the composite at m_smartMapRev. m_compositeRevision bumps whenever m_lastComposite changes
    // (full recomposite or dirty-region patch), so a ratio change / chip toggle between edits
    // reuses the analysis instead of re-walking the document.
    std::uint64_t m_compositeRevision = 1;
    std::uint64_t m_smartMapRev = 0;
    core::retarget::ImportanceMap m_smartMap;
    std::vector<common::Rect> m_smartRegionRects;
    // The eyedropper / wand "All Layers" source and the revision it was walked at (0 = "none", and
    // never a live revision, which starts at 1). Filled ONLY on wandMergedSource's fallback -- when
    // there is no usable host mirror to read -- and released by bumpCompositeRevision, so it lives
    // exactly as long as a hover between two edits, the case it exists for. In the ordinary build
    // it stays empty for the whole session: m_lastComposite serves every call by reference.
    std::uint64_t m_mergedSourceRev = 0;
    common::Image m_mergedSource;
    std::optional<core::ClipboardContent> m_clipboard; // in-app clipboard: alpha + source pos
    std::unique_ptr<core::Document> m_document;        // the document currently shown on the canvas
    // S18-d unsaved-state window title. m_unsavedSince stamps (nowSeconds) the clean->dirty
    // transition so the title can show "unsaved for N min"; m_currentTitle caches the last label so
    // the per-frame recompute only calls copy_label when the string actually changes. The two bools
    // mirror the Annoyances settings.
    std::optional<double> m_unsavedSince;
    std::string m_currentTitle;
    // The ACTIVE document's export memory (plan §6): the last path + format it was exported to.
    // It rides the same register as m_unsavedSince -- spilled into the session on a tab switch,
    // cleared by presentDocument -- because "never leaks to the next document" is the whole
    // point of the rule. m_exportToLabel owns the File menu row's label text (Fl_Menu_Item::label
    // stores the pointer, it does not copy).
    io::ExportTarget m_exportTarget;
    std::string m_exportToLabel;
    bool m_showUnsavedDuration = true;
    bool m_unsavedIncludeSeconds = false;
    // The font stack for on-canvas text (S29). The compositor is deliberately font-free, so the app
    // shapes + rasterizes each TextLayer's pixel cache before every composite (ensureTextCaches):
    // FontDB resolves families/fallback cross-platform, the shaper is reused for its FreeType face
    // cache. Both are cheap to keep resident on the UI thread.
    platform::FontDB m_fontDb;
    // The brush-preset library, scanned once at startup, and the ONE preset the Brush paints with.
    // Resolving a preset is not free (it builds the tip and mints its raster id), so the store does
    // it when the user PICKS one -- never per stroke.
    BrushPresetStore m_brushPresets;
    core::text::TextShaper m_textShaper;
    // The Type §7 "new click text reuses the last box size" toggle (default off = Point text at the
    // size slider). Settings UI wiring is a follow-up; the host reads it via reuseLastBoxSize().
    bool m_reuseLastTextBox = false;
    // S29-c context-bar <-> selection wiring. m_reflectingTextOptions guards the read-back's
    // notifyOptionsChanged from being taken as a user edit; m_textBarSnapshot is the last-applied
    // bar value per option (so only a touched control writes its field); m_lastTextEditOptionId
    // coalesces consecutive edits of the same control (a size drag) into one undo step.
    bool m_reflectingTextOptions = false;
    std::map<std::string, double> m_textBarSnapshot;
    std::string m_lastTextEditOptionId;
    core::text::TextSelection m_lastReflectedTextSel; // last selection range the bar reflected
    // Font-picker preview cache (S29-c §8): family|size|theme -> the rendered cell. The
    // Fl_RGB_Image references `pixels`, so they are stored together and cleared as a unit on a
    // re-theme.
    struct FontPreview {
        common::Image pixels;
        std::unique_ptr<Fl_RGB_Image> img;
    };
    std::map<std::string, FontPreview> m_fontPreviewCache;
    // The shared inpainting engine (S37): offset-stats default, PdeBackend fallback. The Inpaint
    // brush (S39) runs it on release; Edit→Fill→Inpaint (S39-b) shares this instance.
    core::inpaint::InpaintEngine m_inpaintEngine = core::inpaint::makeDefaultEngine();
    // Active backend + preset (Settings → Inpainting) and the resolved Params a run uses. Seeded
    // from settings at start-up and updated live by the Settings dialog. m_inpaintParams = the
    // chosen preset's values with m_inpaintOverrides applied on top (preset == "custom"); see
    // recomputeInpaintParams(). m_inpaintOverrides persists so a hand-tuned setup survives a
    // relaunch.
    std::string m_inpaintBackendId = "offset-stats";
    std::string m_inpaintPresetId = "balanced";
    std::map<std::string, double> m_inpaintOverrides;
    core::inpaint::Params m_inpaintParams{};

    // S39-b async inpaint. One job at a time; the worker thread runs the engine, publishes progress
    // + preview frames under `mutex`, and observes `cancelRequested`. The UI thread polls (~20 Hz)
    // and, on `done`, joins + commits/cancels. NB declared AFTER m_inpaintEngine so it is destroyed
    // FIRST: its destructor cancels + joins the worker before the engine it uses goes away.
    static constexpr double kInpaintPollSeconds = 0.05;
    struct InpaintJob {
        core::LayerId layerId{};
        common::ImageF input;         // engine borrows this by reference for the thread's lifetime
        core::Selection hole;         // owned alongside input
        common::Image originalPixels; // pre-inpaint layer pixels, to restore on cancel/commit
        core::inpaint::Params params{};
        // The hole's bounding box (clamped to the image). Only this region ever differs from the
        // input, so the live preview copies/blits just this box — cheap even on a 36 MP photo.
        std::uint32_t bx0 = 0, by0 = 0, bw = 0, bh = 0;

        std::mutex mutex; // guards the published progress below
        float fraction = 0.0f;
        std::string stage;
        common::ImageF preview; // bbox-sized (bw x bh); the latest intermediate hole region
        bool hasNewPreview = false;

        std::atomic<bool> cancelRequested{false};
        std::atomic<bool> done{false};
        core::inpaint::InpaintResult result;
        std::thread worker;

        ~InpaintJob() {
            cancelRequested.store(true); // ask the worker to bail, then wait it out
            if (worker.joinable())
                worker.join();
        }
    };
    std::unique_ptr<InpaintJob> m_inpaintJob;
    bool m_inpaintRunning = false;
    // Smart Recompose (plan §1.4). The job mirrors InpaintJob: one at a time, the worker
    // publishes progress under `mutex` and observes `cancelRequested`; the UI polls and on
    // `done` joins + enters the review. Declared AFTER m_inpaintEngine for the same destruction-
    // order reason — the FillFn adapter runs the engine on this worker.
    struct RecomposeJob {
        common::Image src; // the composite the pipeline reads (owned for the thread's lifetime)
        std::vector<core::retarget::KeepRegion> regions;
        double aspect = 1.0;
        core::inpaint::Params params{}; // borrowed by the FillFn for the thread's lifetime

        std::mutex mutex; // guards the published progress below
        float fraction = 0.0f;
        std::string stage; // engine stage ids ("Analyzing"/"Solving"/"Blending")

        std::atomic<bool> cancelRequested{false};
        std::atomic<bool> done{false};
        core::retarget::RecomposeStaged staged; // written by the worker before `done`
        common::Image preview; // the first SEAM-BLENDED assembly (worker-side; read after join)
        std::thread worker;

        ~RecomposeJob() {
            cancelRequested.store(true); // ask the worker to bail, then wait it out
            if (worker.joinable())
                worker.join();
        }
    };
    std::unique_ptr<RecomposeJob> m_recomposeJob;
    bool m_recomposeRunning = false;
    // Crop expansion Inpaint fill (S16-f). Mirrors InpaintJob: the worker heals the expansion
    // ring on the translated flatten and the UI lands buildCropCommand(fill.pixels) on `done`.
    // Declared after m_inpaintEngine for the same destruction-order reason. Shares
    // m_inpaintRunning (one engine run at a time). No live preview: the document does not have
    // the expansion until the command lands, so there is nowhere to paint one.
    struct CropExpandJob {
        ui::CropPixels px{};
        bool deletePixels = true;
        common::ImageF input; // new-canvas-sized: the old flatten at its post-crop offset
        core::Selection hole; // the expansion ring
        core::inpaint::Params params{};

        std::mutex mutex; // guards the published progress below
        float fraction = 0.0f;
        std::string stage;

        std::atomic<bool> cancelRequested{false};
        std::atomic<bool> done{false};
        core::inpaint::InpaintResult result;
        std::thread worker;

        ~CropExpandJob() {
            cancelRequested.store(true); // ask the worker to bail, then wait it out
            if (worker.joinable())
                worker.join();
        }
    };
    std::unique_ptr<CropExpandJob> m_cropExpandJob;
    // The Canvas Size panel's Fill = Inpaint (S53 fill parity). Same shape as CropExpandJob, but
    // it lands buildCanvasResizeCommand instead of buildCropCommand -- the panel's own request is
    // what the healed pixels have to be attached to.
    struct ImageOpExpandJob {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        render::CanvasAnchor anchor = render::CanvasAnchor::Center;
        common::ImageF input; // new-canvas-sized: the old flatten at its post-resize offset
        core::Selection hole; // the newly exposed margin
        core::inpaint::Params params{};

        std::mutex mutex; // guards the published progress below
        float fraction = 0.0f;
        std::string stage;

        std::atomic<bool> cancelRequested{false};
        std::atomic<bool> done{false};
        core::inpaint::InpaintResult result;
        std::thread worker;

        ~ImageOpExpandJob() {
            cancelRequested.store(true); // ask the worker to bail, then wait it out
            if (worker.joinable())
                worker.join();
        }
    };
    std::unique_ptr<ImageOpExpandJob> m_imageOpExpandJob;
    // The review (job done, preview on canvas): the staged pipeline state (`placed` is nudged
    // in place), the assembled preview the canvas displays, and the modal flag. A nudge shows
    // the fast feather-only assembly; m_recomposeNudged remembers to re-run the seam-blended
    // one before Apply lands.
    core::retarget::RecomposeStaged m_recomposeStaged;
    common::Image m_recomposePreview;
    bool m_recomposeReview = false;
    bool m_recomposeNudged = false;
    // Adaptive throttle for the live-preview recomposite. Each applied preview triggers a full-
    // document recomposite (S60 region-recomposite is still pending), so during the Poisson blend —
    // which streams a frame every few sweeps — the 20 Hz poll would recomposite the document
    // several times a second and saturate the UI thread (badly on a big photo, where one
    // recomposite is hundreds of ms). The progress bar still updates every poll, but a preview is
    // only blit+recomposited once at least max(kMin, kBudgetFactor × lastRecompositeCost) has
    // elapsed — so preview recomposites take at most ~1/kBudgetFactor of the UI thread no matter
    // the doc size, self-tuning from the measured cost.
    static constexpr double kInpaintPreviewMinSeconds = 0.20;
    static constexpr double kInpaintPreviewBudgetFactor = 4.0;
    std::chrono::steady_clock::time_point m_lastInpaintPreview{};
    NewDocumentSpec m_lastNewSpec = defaultNewDocumentSpec(); // seeds the next File->New dialog
    render::DragCompositeCache m_dragCache; // S15-b: reused across the frames of one Move drag
    bool m_recompositePending = false;      // a frame-coalesced canvas re-composite is queued
    bool m_textThumbDirty =
        false; // a text edit/hover needs the layer thumbnail re-rendered (rev 11)
    double m_lastTextEditTime = -1.0e9; // when markTextThumbDirty last ran (the settle reference)
    bool m_textHoverDraft = false;      // a font-hover preview rendered DRAFT; crisp lands on settle
    double m_textHoverDraftAt = -1.0e9; // when the last hover preview applied (the settle reference)
    std::optional<common::Rect>
        m_lastTextEditBox; // last edit-box screen bounds (re-place the Type panel on change)
    // Per-layer reflect-env settle state: layer id -> (stale fingerprint, first seen at). The
    // fingerprint a snapshot actually mirrors lives ON the TextLayer.
    std::map<core::LayerId, std::pair<std::uint64_t, double>> m_reflectPending;
    // Spell-check (deferred §2): the background worker + its settle/epoch bookkeeping. The worker
    // is spawned lazily on first text edit and lives for the app; updateSpellCheck() drives it per
    // frame.
    static constexpr double kSpellSettleSec =
        0.25;                         // quiet time after an edit before a rescan fires
    bool m_spellCheckEnabled = true;  // Settings → Text "Check spelling" (gates the worker live)
    bool m_spellCheckAllCaps = false; // Settings → Text "Check ALL-CAPS words" (decision D4)
    std::string m_textLanguage;       // Settings → Text default language ("" = OS locale)
    std::string m_emojiFont;          // Settings → Text emoji fallback family ("" = automatic, R5)
    std::unique_ptr<core::text::SpellCheckWorker> m_spellWorker;
    core::LayerId m_spellActiveTarget = core::kInvalidLayerId; // the layer the last scan was for
    std::uint64_t m_spellLastRev =
        static_cast<std::uint64_t>(-1); // its contentRevision when scanned
    bool m_spellDirty = false;          // the block changed; a rescan is pending the settle
    double m_spellDirtyTime = -1.0e9;   // when the pending change was noticed (settle reference)
    std::uint64_t m_spellEpoch = 0;     // monotonic request tag (bumped per submitted scan)
    std::uint64_t m_spellPendingEpoch =
        0; // the epoch of the latest submitted scan (result must match)
    // Recovery journal (S48, spec 2.6): the app-owned autosave sidecar for the CURRENT document,
    // under $XDG_STATE_HOME/mosaic/recovery -- NEVER the user's file (the section-0 hard rule). An
    // edit stamps m_journalDirtyTime; onFrame autosaves once editing settles (idle-triggered
    // coalescing, Round 12). Reset on Save, discarded on a clean close -- a crash leaves it for the
    // next open to find (crash restore, flows 1/2).
    static constexpr double kJournalSettleSec = 1.5; // quiet time after an edit before autosaving
    std::optional<io::native::JournalSession> m_journal;
    bool m_journalDirty = false;        // an edit is pending the settle
    double m_journalDirtyTime = -1.0e9; // when it was noticed (settle reference)
    // §2.10 advisory lock (flow 6): held while a file-backed document is open, so a second Mosaic
    // finds it busy and opens read-only. NEVER a lock on the user's file -- a recovery-dir lock
    // file. m_documentReadOnly marks a borrowed (lock-busy) open: no lock, no journal, Save routes
    // to Save As so it can never overwrite the file the other window owns.
    std::optional<io::native::AdvisoryLock> m_lock;
    bool m_documentReadOnly = false;
    // Commit-append Save anchor (spec 2.6, Round 12): the durable-on-disk state a File->Save
    // appends its delta onto, present ONLY while the on-screen document equals the file's newest
    // committed state (a clean .mosaic open or a full write arms it; a recovered/salvaged view, a
    // read-only borrow, or an untitled document leave it empty -> the next Save is a full write).
    // `baseline` is the serialization of that durable state (the diff source), `tip` is where the
    // committed region ends (identity-stamped for the O(1) pre-Save tail check), `walStart` is the
    // append-region start (the needsCompaction() input that decides when a Save folds the file
    // instead of appending; see saveDocument). Reset on every document swap.
    struct CommitAnchor {
        io::native::CommitTip tip;
        io::native::CheckpointInput baseline;
        std::uint64_t walStart = 0;
        // Build 2 (spec 3.9): the file's history-encoding mode as of the anchor, the live
        // whole-history churn signal (seeded from the open report, advanced O(changed) by every
        // appended Save), and the signal level at the last proactive fold attempt -- the
        // save_policy throttle. The tracker stays empty for files past the proactive size cap
        // (they can never fold proactively, so the open never pays for seeding one).
        std::string mode = io::native::kModeJournal;
        std::optional<io::native::ChurnTracker> churn;
        double lastProactiveChurn = -1.0;
    };
    std::optional<CommitAnchor> m_commit;
    // ---- Background full write, state (S48 Build 2; the types live above saveDocument) --------
    std::unique_ptr<SaveJob> m_saveJob;
    PendingSave m_pendingSave = PendingSave::None; // coalesced request; newest wins (design note)
    bool m_waitingForSave = false;                 // waitForBackgroundSave is pumping
    static constexpr double kSavePollSeconds = 0.05;
    // ---- S49 open-document tabs ---------------------------------------------------------------
    //
    // The ACTIVE document's state stays in the plain members above (m_document, m_journal, m_lock,
    // m_documentReadOnly, m_commit, m_unsavedSince): they are the register the whole window already
    // reads, and leaving them there kept ~250 call sites untouched. `m_sessions` is the backing
    // store for the documents that are NOT on screen. unloadActiveSession() spills the register
    // into m_sessions[m_activeSession]; loadSession() fills it from another slot.
    //
    // Every open document keeps its OWN journal and lock -- a background tab is still protected
    // against a crash, and still holds the file against a second Mosaic. A deliberate close
    // discards its journal; a crash must not.
    struct DocumentSession {
        std::unique_ptr<core::Document> doc;
        std::optional<io::native::JournalSession> journal;
        bool journalDirty = false;
        double journalDirtyTime = -1.0e9;
        std::optional<io::native::AdvisoryLock> lock;
        bool readOnly = false;
        std::optional<CommitAnchor> commit;
        std::optional<double> unsavedSince;
        io::ExportTarget exportTarget; // per-document export memory (§6); never shared between tabs
        bool hasView = false; // false = never shown, so its first activation fits the view
        CanvasView::ViewState view;
    };
    std::vector<DocumentSession> m_sessions;
    std::size_t m_activeSession = 0; // index into m_sessions; meaningless while m_sessions is empty
    TabStrip* m_tabStrip = nullptr;  // hidden while <= 1 document is open (user, 2026-07-09)
    core::LayerId m_shapePreviewLayer =
        core::kInvalidLayerId;    // live shape-tool preview layer (S26)
    // S22 Gradient tool: the live authoring-preview layer, the tool's working ramp (stops + spread),
    // the "Stops…" flyout, and the coalesce id of the current flyout stops-edit session.
    core::LayerId m_gradientPreviewLayer = core::kInvalidLayerId;
    core::vec::Gradient m_gradientPaint;      // working ramp (seeded lazily fg -> transparent)
    bool m_gradientPaintSeeded = false;
    GradientFlyout* m_gradientFlyout = nullptr;
    std::uint64_t m_gradientStopsCoalesce = 0;
    std::uint64_t m_gradientOptionCoalesce = 0; // ... and of the current bar Type/Opacity session
    bool m_reflectingGradientOptions = false;   // bar write-back in flight (not a user edit)
    bool m_gpuDragActive = false; // a GPU-resident Move/transform drag is in flight (S60-a)
    bool m_gpuDragTried = false;  // arming attempted this gesture (don't rebuild below/frame)
    // Diagnostics only: the next full recomposite drain is a Move-gesture RELEASE, so onFrame
    // records it under its own profiler row as well (docs/s60-gesture-start-stall.md G3). Nothing
    // reads it outside the profiler; it changes no composite.
    bool m_gestureEndPending = false;
    // The frame-coalesced dirty-region patch (S60-a): the union of this frame's dirty rects and
    // whether a pass is queued at all -- one type, because "queued with no rect yet" is a state the
    // typing path depends on and a pair of loose members kept losing (see ui::PendingRegion).
    PendingRegion m_pendingRegion;
    // The resident lane drew the canvas, so m_lastComposite is a mirror of it rather than the
    // picture itself. Set on a served frame, cleared by the full CPU composite that re-establishes
    // the mirror; recompositeRegionNow refuses to patch a region while it stands. Always false
    // without MOSAIC_TILE_COMPOSITOR=1 -- nothing sets it when there is no lane.
    bool m_hostCompositeStale = false;
    double m_lastRecompositeCost =
        0.0; // seconds the last recompositeNow took (live-preview throttle budget)
    bool m_frameLoopStarted = false; // run() armed the frame timer; kicks may re-arm it
    double m_lastFrameTime = -1.0e9; // when onFrame last ran (the pacing interval's reference)
    // When the app last saw a sign of life. Drives the display-paced/idle choice -- see
    // scheduleNextFrame. Starts "long ago", so a window that opens and is left alone idles.
    double m_lastActivitySec = -1.0e9;
    bool m_windowActive = true; // FL_FOCUS/FL_UNFOCUS/FL_SHOW/FL_HIDE -- see handle()
    // The coalescer's whole state: whether a frame is queued and for WHEN. The pair is what lets
    // requestFrame tell "a frame is already coming soon enough" from "a frame is coming, but at the
    // idle rate" -- conflating those two is how input landing during a background heartbeat would
    // sit for 100 ms.
    bool m_frameArmed = false;
    double m_nextFrameDue = 0.0;
    // The cached refresh interval of the panel the window is on, and when it was last asked for.
    // Starts at the conservative fallback so the very first frames are paced even before the window
    // is mapped (platform::displayRefreshHz needs a shown window and answers 0.0 before that).
    double m_refreshIntervalSec = 1.0 / kFallbackRefreshHz;
    double m_refreshQueriedAt = -1.0e9;
#ifdef MOSAIC_DEBUG
    // Frame-timing diagnostics (Help menu, debug builds only): the FPS-readout toggle, the previous
    // frame's timestamp (for the wall-clock interval feeding the profiler), and the Timing Profiler
    // window. The per-op stats live in the process-wide common::Profiler singleton, not here.
    bool m_showCanvasFps = false;
#endif
    double m_prevFrameStamp = 0.0; // previous frame's wall clock -- feeds the FPS/frame-time rows
    // The Timing Profiler window. Not debug-gated: in release it is only ever created when
    // profiling is on (S60-alpha), and a null unique_ptr costs nothing when it is not.
    std::unique_ptr<TimingGraphWindow> m_timingGraph;
    // The document selection revision last handed to the canvas; the max sentinel forces the
    // first sync (a fresh document restarts its revision counter at 0).
    std::uint64_t m_syncedSelectionRev = std::numeric_limits<std::uint64_t>::max();
    bool m_pendingFitView = false; // ... and it should re-fit the view (new/opened document)
    int m_autoQuitFrames = 0;
    long m_frameCount = 0;
    bool m_errorReported = false;
    bool m_metric =
        false; // crop size HUD unit (S16-e): true = cm, false = in (configurePicker sets)
    bool m_cropSwitchToolAfterApply = false; // S16-p: leave Crop for the previous tool after Apply
    bool m_cropClearSelectionOnLeave =
        false;                            // clear the staged crop rect when leaving the Crop tool
    std::filesystem::path m_settingsPath; // settings.json location (configurePicker sets)
    std::string m_workingProfile;         // RGB working ICC override; re-applied with a CMYK change
    std::unique_ptr<SettingsDialog> m_settingsDialog; // lazily built, reused across opens (S51-a)
    std::unique_ptr<FillDialog> m_fillDialog;         // Edit→Fill… (S39); rebuilt per open (cheap)
    std::unique_ptr<LayerEffectsDialog>
        m_layerEffectsDialog; // Layer→Layer Effects… (LE-b); per open
    std::unique_ptr<BrushEditorDialog>
        m_brushEditorDialog; // the dock's Edit… / a card double-click (§8.3); per open
    AdjustmentPanel* m_adjustmentPanel = nullptr; // the S32 editor (pinned popover sub-window)
    SelectMorphPanel* m_selectMorphPanel = nullptr; // Grow/Shrink/Feather/Smooth live-preview panel
    // The selection-morphology preview's base mask (a snapshot at open time; every preview morphs
    // THIS, not the compounding previous result) + a flag latched while the panel drives the ants.
    core::Selection m_selectMorphOriginal;
    bool m_selectMorphActive = false;
    // Live-preview coalescing (the "amount slider hangs" fix): the slider/mode callback only marks
    // the preview dirty; the frame loop morphs the snapshot ONCE per frame (flushSelectMorphPreview),
    // skipping when nothing changed and guarding against re-entry. Paired with the O(n) large-radius
    // feather/smooth in core::Selection so a single morph can never wedge the frame either.
    bool m_selectMorphPreviewDirty = false; // a morph is pending for the frame loop to run
    bool m_selectMorphPreviewing = false;   // a morph is in progress (re-entrancy guard)
    bool m_selectMorphPreviewValid = false; // the two fields below hold the last previewed op/amount
    ui::SelectMorphMode m_selectMorphPreviewMode = ui::SelectMorphMode::Grow;
    double m_selectMorphPreviewAmount = 0.0;
    ImageOpsPanel* m_imageOpsPanel = nullptr; // the S53 Image-menu live-preview panel
    // Which mode the next show opens on (the menu item picks it; showCornerPanel reads it), plus
    // the live-preview bookkeeping -- the SelectMorphPanel's shape exactly: the panel callback only
    // records the request and marks it dirty, and the frame loop stages at most one canvas overlay
    // per frame (a number field fires per keystroke, twice with the proportions lock engaged).
    ImageOpsPanel::Mode m_pendingImageOpMode = ImageOpsPanel::Mode::CanvasSize;
    bool m_imageOpsActive = false;       // a panel session is live (the frame loop's gate)
    bool m_imageOpPreviewDirty = false;  // an overlay rebuild is pending
    std::optional<ImageOpsPanel::Request> m_imageOpRequest; // the last request the panel sent
    // The panel's Fill = "Gradient…" / "Pattern…" editors (its own, not the Gradient tool's).
    GradientFlyout* m_imageOpsGradientFlyout = nullptr;
    GradientFlyout* m_adjustmentGradientFlyout = nullptr;  // S34-a: the Gradient Map ramp editor
    PatternFlyout* m_imageOpsPatternFlyout = nullptr;
    // The selection Deselect threw away, for Select ▸ Reselect. Only deselect() stashes one --
    // that is what Reselect means (undo already covers "the previous selection" in general).
    core::Selection m_lastDeselected;
    // Filter ▸ Last Filter: the kind the Filter menu last inserted (app-global, like Photoshop's).
    std::optional<core::AdjustmentKind> m_lastFilterKind;
    // The S53-b dynamic Type/Layer menu rows' last published state -- the fingerprint that keeps
    // the once-per-frame reconcile from republishing (and, on macOS, rebuilding) the whole menu.
    DynamicMenuState m_dynamicMenuState;
    // The corner-panel arbiter (round 5): the ids, the decision core, and which panel the last
    // sync actually showed (0 = none) -- the reconciliation reference for external hides.
    enum : int {
        kPanelStyle = 1,
        kPanelType3d = 2,
        kPanelAdjustment = 3,
        kPanelSelectMorph = 4,
        kPanelImageOps = 5,
    };
    PanelArbiter m_panelArbiter;
    int m_cornerVisible = 0;
    std::string m_lastAdjustFieldId;   // last edited control (the coalesce rule's memory)
    std::uint64_t m_adjustCoalesceSeq = 1; // bumped when the edited control changes
    bool m_adjustThumbDirty = false;   // a panel edit left the adjustment row's preview stale
    double m_lastAdjustEditTime = -1.0e9; // the settle reference for the preview refresh
    bool m_blurScrubWasActive = false; // last frame's draft-mode state (S33: settle on release)
    std::uint64_t m_adjPanelFadeFp = 0; // what the faded panel last blended over (view+composite)
    std::unique_ptr<TextureGeneratorDialog>
        m_textureGenDialog; // Layer→Texture Generator… (S55-f); per open
    // Declared last so it is destroyed FIRST -- unsubscribes from theme changes before any chrome
    // it would touch is torn down. Assigned in the ctor once all chrome exists.
    ThemeSubscription m_themeSub;
    // current mode; the System watcher re-resolves on OS change
    ThemeMode m_themeMode = ThemeMode::Dark;
};

void cbSettings(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->openSettings();
}

void cbRulers(Fl_Widget* w, void* mainWindow) {
    if (mainWindow == nullptr)
        return;
    const Fl_Menu_Item* item = static_cast<Fl_Menu_Bar*>(w)->mvalue();
    static_cast<MainWindow*>(mainWindow)->setRulersVisible(item != nullptr && item->value() != 0);
}

void cbShowGuides(Fl_Widget* w, void* mainWindow) {
    if (mainWindow == nullptr)
        return;
    const Fl_Menu_Item* item = static_cast<Fl_Menu_Bar*>(w)->mvalue();
    static_cast<MainWindow*>(mainWindow)->setShowGuides(item != nullptr && item->value() != 0);
}

void cbClearGuides(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->clearGuides();
}

void cbLockGuides(Fl_Widget* w, void* mainWindow) {
    if (mainWindow == nullptr)
        return;
    const Fl_Menu_Item* item = static_cast<Fl_Menu_Bar*>(w)->mvalue();
    static_cast<MainWindow*>(mainWindow)->setLockGuides(item != nullptr && item->value() != 0);
}

template <core::AlignEdge E> void cbAlign(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->alignSelection(E);
}

template <core::DistributeAxis A> void cbDistribute(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->distributeSelection(A);
}

void cbFill(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->openFillDialog();
}

// ---- Image menu (S53) --------------------------------------------------------------------------

void cbImageSize(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->openImageOps(ImageOpsPanel::Mode::ImageSize);
}
void cbCanvasSize(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->openImageOps(ImageOpsPanel::Mode::CanvasSize);
}
void cbRotateArbitrary(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->openImageOps(ImageOpsPanel::Mode::RotateArbitrary);
}
void cbTrimToContent(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->trimToContent();
}
// One instantiation per orientation, so each menu row has its own callback ADDRESS -- which is how
// the badge predicate and menu_visibility identify items (never by position or path string).
template <render::DocOrient O> void cbOrient(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->orientDocument(O);
}

// ---- Type menu (S53-b) -------------------------------------------------------------------------

void cbRasterizeType(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->rasterizeActiveTypeLayer();
}
void cbTypeToShape(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->convertActiveTypeLayerToShape();
}
void cbTypeCharPanel(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->openTypePanelFromMenu();
}
void cbType3dPanel(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->openType3dPanelFromMenu();
}
void cbTypeToPointText(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->setTypeFrame(core::text::TextFrame::Point);
}
void cbTypeToAreaText(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->setTypeFrame(core::text::TextFrame::Area);
}
void cbTypeOnPath(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->typeOnSelectedPath();
}
void cbTypeReleaseFromPath(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->typeReleaseFromPath();
}
void cbTypeWorkPath(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->typeCreateWorkPath();
}
void cbTypeUpdateAll(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->updateAllTextLayers();
}
template <core::text::WritingMode M> void cbTypeWritingMode(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->setTypeWritingMode(M);
}
template <core::text::AntiAlias A> void cbTypeAntiAlias(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->setTypeAntiAlias(A);
}
template <core::text::Kerning K> void cbTypeKerning(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->setTypeKerning(K);
}
template <core::text::Paragraph::Direction D> void cbTypeDirection(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->setTypeDirection(D);
}

// ---- Layer menu (S53-b) ------------------------------------------------------------------------

void cbRenameLayer(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->renameActiveLayer();
}
void cbRasterizeLayer(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->rasterizeActiveLayer();
}
void cbConvertToPath(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->convertActiveLayerToPath();
}
void cbMeshWarp(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->activateWarpTool(/*perspective=*/false);
}
void cbPerspectiveWarp(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->activateWarpTool(/*perspective=*/true);
}
void cbAddMask(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->layerMaskAction(MainWindow::MaskAction::Add);
}
void cbDeleteMask(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->layerMaskAction(MainWindow::MaskAction::Delete);
}
void cbToggleMaskEnabled(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->layerMaskAction(MainWindow::MaskAction::ToggleEnabled);
}
void cbToggleMaskLinked(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->layerMaskAction(MainWindow::MaskAction::ToggleLinked);
}
void cbToggleLayerVisible(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->toggleActiveLayerVisible();
}
void cbToggleLayerLocked(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->toggleActiveLayerLocked();
}
void cbBringForward(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->reorderActiveLayer(+1);
}
void cbSendBackward(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->reorderActiveLayer(-1);
}
void cbFlattenToPath(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->flattenToPath();
}
template <core::vec::BoolOp Op> void cbCombinePaths(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->combinePaths(Op);
}

// ---- Select / Filter menu (S53-b) --------------------------------------------------------------

void cbReselect(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->reselect();
}
void cbSelectAllLayers(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->selectAllLayers();
}
void cbLastFilter(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->repeatLastFilter();
}
void cbPasteInPlace(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->pasteInPlace();
}
void cbClear(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->clearSelection();
}

void cbFileNew(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->newDocument();
}

void cbFileOpen(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->openDocument();
}

void cbClearRecents(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->clearRecentFiles();
}

void openRecentFromMenu(void* mainWindow, std::size_t index) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->openRecentIndex(index);
}

void cbFileSave(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->saveDocument();
}

// File->Close (Ctrl+W): close the ACTIVE tab, prompting when it has unsaved changes. The last one
// closes to the empty state rather than quitting -- the window is not the document (S49).
void cbFileClose(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->closeActiveSession();
}

// File->Open as Layer... (S50): place an image on the active document as a magic layer.
void cbOpenAsLayer(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->openAsLayer();
}

void cbFileSaveAs(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->saveDocumentAs();
}

// File->Rename Document... (round 7): the document's own name -- the titlebar identity -- gets
// its affordance; the file on disk is untouched.
void cbRenameDocument(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->renameDocument();
}

void cbFlattenHistory(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->flattenHistory();
}
#ifdef MOSAIC_DEBUG
void cbOpenDemoCanvas(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->openDemoCanvas();
}
void cbEnableAllControls(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->setAllControlsEnabled(true);
}
void cbDisableAllControls(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->setAllControlsEnabled(false);
}
#endif
void cbQuickExportPng(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->quickExportPng();
}
void cbQuickExportJpeg(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->quickExportJpeg();
}
void cbQuickExportJxl(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->quickExportJxl();
}
void cbExportAs(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->exportAs();
}
void cbExportTo(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->exportToLastTarget();
}
void cbUndo(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->undo();
}
void cbRedo(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->redo();
}
void cbNewLayer(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->newLayer();
}
void cbDuplicateLayer(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->duplicateLayer();
}
void cbDeleteLayer(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->deleteLayer();
}
void cbGroupLayers(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->groupLayers();
}
void cbMergeDown(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->mergeDownLayer();
}
void cbLayerEffects(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->openLayerEffects();
}
void cbTextureGenerator(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->openTextureGenerator();
}
#ifdef MOSAIC_DEBUG
void cbToggleCanvasFps(Fl_Widget* w, void* mainWindow) {
    if (mainWindow == nullptr)
        return;
    // The FL_MENU_TOGGLE item carries the new checked state after picked() ran (like cbPixelGrid).
    const Fl_Menu_Item* item = static_cast<Fl_Menu_Bar*>(w)->mvalue();
    static_cast<MainWindow*>(mainWindow)->toggleCanvasFps(item != nullptr && item->value() != 0);
}
#endif // MOSAIC_DEBUG
void cbTimingGraph(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->openTimingGraph();
}
template <core::AdjustmentKind K> void cbNewAdjustment(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->insertAdjustmentLayer(K);
}
void cbSelectAll(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->selectAll();
}
void cbDeselect(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->deselect();
}
void cbInvertSelection(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->invertSelection();
}
void cbGrowSelection(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->growSelection();
}
void cbShrinkSelection(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->shrinkSelection();
}
void cbFeatherSelection(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->featherSelection();
}
void cbSmoothSelection(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->smoothSelection();
}
void cbMaskFromSelection(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->maskFromSelectionEntry(
            MainWindow::MaskCombine::Replace);
}
void cbMaskFromInverseSelection(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->maskFromSelectionEntry(
            MainWindow::MaskCombine::Inverse);
}
void cbAddToMask(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->maskFromSelectionEntry(MainWindow::MaskCombine::Add);
}
void cbSubtractFromMask(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->maskFromSelectionEntry(
            MainWindow::MaskCombine::Subtract);
}
void cbCut(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->cutSelection();
}
void cbCopy(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->copySelection(/*merged=*/false);
}
void cbCopyMerged(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->copySelection(/*merged=*/true);
}
void cbPaste(Fl_Widget*, void* mainWindow) {
    if (mainWindow != nullptr)
        static_cast<MainWindow*>(mainWindow)->pasteClipboard();
}

// Global event dispatch: while a native file dialog is modal-up, SWALLOW all pointer/keyboard input
// to our windows so the app truly cannot be driven behind it. This is the portable, backend-agnostic
// backstop the modal picker actually needs: the XDG portal dialog runs OUT OF PROCESS, so passing it
// a parent handle + modal:true only ASKS the compositor to enforce modality -- and the xdg-dialog-v1
// spec explicitly permits a compositor to keep "delivering all events unfiltered" to the parent
// ("Clients must implement the logic to filter events in the parent toplevel on their own"). Qt's own
// portal file dialog relies purely on that compositor hint and does NOT block its parent, which is
// why a Qt app's picker can float non-modal on Wayland+KDE too; we don't take that chance.
//
// Deactivating the main window (DialogGuard) greys it but does NOT block input (FLTK gates DRAWING on
// active_r() -- recursive -- but event delivery on takesevents(), which checks only the widget's own
// flag), so a greyed-but-live main window still reacted to clicks/keys during the portal picker's
// pumped loop (user report). Non-input events (redraw, timers, the WM close -> handled by the quit
// guard) pass through untouched.
//
// The predicate is fileDialogGrabsInput(), NOT fileDialogInFlight(): the latter is also true while
// Fl_Native_File_Chooser's last-resort backend -- FLTK's own in-process Fl_File_Chooser window --
// is up, and swallowing input there would leave a modal chooser on screen that cannot be driven or
// cancelled (see platform/file_dialog.hpp).
int fileDialogInputGuard(int event, Fl_Window* win) {
    if (platform::fileDialogGrabsInput()) {
        switch (event) {
        case FL_PUSH:
        case FL_DRAG:
        case FL_RELEASE:
        case FL_MOVE:
        case FL_MOUSEWHEEL:
        case FL_KEYBOARD: // == FL_KEYDOWN
        case FL_KEYUP:
        case FL_SHORTCUT:
            return 1; // consumed here; never delivered to our widgets
        case FL_DND_ENTER:
        case FL_DND_DRAG:
        case FL_DND_LEAVE:
        case FL_DND_RELEASE:
            // REFUSE the drop: returning 0 tells the drag source "not accepted here", so FLTK never
            // follows up with the FL_PASTE that carries the URI list -- otherwise a file dropped on
            // the canvas/strip would open a document (or a magic layer) behind the live picker.
            return 0;
        case FL_PASTE:
            return 1; // belt-and-suspenders: swallow any paste payload that still reached us
        default:
            break;
        }
    }
    const int handled = Fl::handle_(event, win); // the normal dispatch
    // A click on bare chrome -- a panel ground, a label, dead space; no widget consumed the
    // push -- takes keyboard focus OUT of the text field holding it, committing the field like
    // Tab would (NumberField evaluates its expression on unfocus). App-wide by request (user
    // 2026-07-22): a value field must not keep its caret once you click past it.
    // Two FLTK facts shape the mechanism (both probe-verified on 1.4.5): the dispatcher's
    // RETURN VALUE cannot detect a chrome click -- Fl::handle_ answers 1 for every FL_PUSH (it
    // raises the clicked window and claims the event even when no widget wanted it), so the
    // honest signal is Fl::pushed(), which after dispatch holds the deepest consuming widget
    // and the WINDOW itself (or null) when the click fell through to bare chrome. And
    // Fl::focus(nullptr) does NOT survive the click: FLTK's focus fixup re-assigns a null
    // focus to the first willing widget on the very next FL_RELEASE. Parking focus on the
    // CLICKED WINDOW sticks -- the field still gets its FL_UNFOCUS (committing the edit), and
    // keyboard routes to the window, where the dialogs' Enter/Escape handling lives anyway.
    if (event == FL_PUSH && win != nullptr) {
        Fl_Widget* consumer = Fl::pushed();
        if ((consumer == nullptr || consumer->as_window() != nullptr) &&
            dynamic_cast<Fl_Input_*>(Fl::focus()) != nullptr)
            Fl::focus(win);
    }
    return handled;
}

} // namespace

// FLTK's clipboard paste (X11 and Wayland alike) writes an external image/png offer to a temp
// file and decodes it through Fl_Shared_Image::get() -- which knows NO formats until a handler
// is registered, so a browser's copy "contained an image" but pasted nothing (probe-verified
// 2026-07-22; in-app copies always worked because Fl_Copy_Surface round-trips as image/bmp,
// which the core decodes itself). Rather than linking fltk_images -- whose bundled nanosvg
// collides with our vendored one -- the handler is OURS: the same hardened io::loadImage that
// backs File->Open decodes the clipboard. PNG/JPEG by magic bytes; anything else declined.
Fl_Image* clipboardSharedImageHandler(const char* name, uchar* header, int headerlen) {
    const bool png = headerlen >= 8 && std::memcmp(header, "\x89PNG\r\n\x1a\n", 8) == 0;
    const bool jpeg = headerlen >= 3 && std::memcmp(header, "\xFF\xD8\xFF", 3) == 0;
    if (!png && !jpeg)
        return nullptr;
    const std::optional<common::Image> img = io::loadImage(name);
    if (!img.has_value() || img->empty())
        return nullptr;
    auto* data = new uchar[img->rgba.size()];
    std::memcpy(data, img->rgba.data(), img->rgba.size());
    auto* rgb = new Fl_RGB_Image(data, static_cast<int>(img->width),
                                 static_cast<int>(img->height), 4);
    rgb->alloc_array = 1; // the image owns `data`
    return rgb;
}

int runApp(const RunOptions& options) {
#ifdef __APPLE__
    // FLTK builds the application menu ONCE, the first time anything opens the display, and it
    // keeps these pointers -- so the strings have to be in place before the first FLTK call that
    // could trigger that, which is why this is the very first line here (S58-b). The items
    // themselves (About / Settings) are wired later, from the window that owns their callbacks.
    setMacApplicationMenuText({.about = _("About Mosaic"),
                               .services = _("Services"),
                               .hide = _("Hide Mosaic"),
                               .hideOthers = _("Hide Others"),
                               .showAll = _("Show All"),
                               .quit = _("Quit Mosaic")});
    // Registered before the window exists on purpose: Finder's open-documents event for a
    // double-clicked file arrives during launch, and the queue is what carries it to run().
    fl_open_callback(macOpenDocumentCallback);
#endif
    platform::preferWaylandBackendIfUnset();
    // The Wayland taskbar/window icon is matched app_id -> .desktop -> Icon=, and FLTK maps its
    // xclass onto the toplevel's app_id (and onto WM_CLASS on X11, which is the same match there).
    // Unset, the compositor finds no .desktop for us and shows a generic placeholder. Pin it to the
    // basename data/desktop/mosaic.desktop installs under -- the StartupWMClass it already declares.
    // Must precede the first window, which is why it sits next to the backend pin (S59-a).
    Fl_Window::default_xclass("mosaic");
    // The other half of the same chain, for the case the .desktop cannot cover (an AppImage is
    // never installed, so app_id resolves to nothing): hand the compositor the icon directly, and
    // tell it which toplevels are modal dialogs. Installed HERE, before the first window, so it
    // catches every toplevel the app will ever open -- including the ones opened from inside a
    // modal dialog's own nested event loop. See ui/window_hints.hpp for why this is not a line
    // after each show().
    installToplevelHintWatcher();
    Fl_Shared_Image::add_handler(clipboardSharedImageHandler); // external image paste (S55)
    platform::initNativeFileDialog(); // Phase 0: sharpen the native-chooser fallback on KDE
    Fl::event_dispatch(fileDialogInputGuard); // make "modal behind a native dialog" actually modal

#if !defined(__APPLE__) && !defined(_WIN32)
    // X11/Wayland need a display-server env var. macOS (Cocoa, S58) and Windows (the window
    // station, S57) have no such thing -- their window server is always present -- so a guard that
    // reads DISPLAY there refuses to start a perfectly working GUI. This exact check is what made
    // the first .app quit at launch on a real Mac; it would do the same on every Windows box.
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr) {
        uiLog().error(
            "no display found (DISPLAY/WAYLAND_DISPLAY unset); use --headless for the GUI-less "
            "pipeline");
        return 1;
    }
#endif

    const Palette pal = resolvePalette(options.themeMode);
    applyTheme(pal);
    Fl::visible_focus(0); // pro-app feel: no dotted focus rectangles
    uiLog().info("theme: {} ({} mode), accent #{:02X}{:02X}{:02X}", pal.dark ? "dark" : "light",
                 themeModeKey(options.themeMode), pal.accent.r, pal.accent.g, pal.accent.b);

    MainWindow win(1100, 720, options.autoQuitFrames);
    win.configurePicker(options);
    win.watchSystemTheme(options.themeMode); // live-follow the OS appearance while in System mode
    win.run(options.openPaths);
    return Fl::run();
}

} // namespace mosaic::ui
