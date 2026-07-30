#pragma once

#include "common/image.hpp"
#include "core/brush/brush_engine.hpp"
#include "core/brush/sensors.hpp"
#include "io/brush/library.hpp"

#include <FL/Fl_Double_Window.H>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The modal brush editor (docs/brushes.md §8.3) -- the one surface where a preset's own options are
// authored rather than merely applied. Structurally a clone of `LayerEffectsDialog`: a fixed
// `size_range`, `set_modal()` after `end()`, and its own themed dropdown / ruler child sub-windows,
// because a modal is its own top level and a sub-window added to an already-shown parent is promoted
// to a stray one.
//
// ⚠ BUT THERE IS NO `applyLive`, AND THAT IS THE STRUCTURAL DIFFERENCE. The Layer Effects modal
// edits a layer and shows the pending result on the canvas behind it; this one never touches the
// document at all. `commit()` writes a PRESET, not pixels -- so there is nothing to revert, and a
// cancel is simply a close.
//
// Layout, 1040x640:
//   header   thumbnail | name field + source engine + fidelity badge + dirty dot | Save/Save As/Import
//   rail     the preset list (browse) above a CHECKABLE option list, grouped General/Colour/Texture/Tip
//   centre   the option page for whatever the rail has selected (ScrubSlider / Dial / CurveEditor)
//   right    ONE preview surface doing two jobs: the auto stroke, and a scratchpad you can paint on
//   footer   the two eraser ties, and Close
//
// Merging the preview and the scratchpad is what makes a MODAL editor tolerable: the reason anyone
// wants a modeless one is "let me try it on something real", and a paintable surface answers most of
// that without ever risking the artwork. The scratch surface is a plain CPU `common::Image` driven
// by the same `BrushEngine` -- it never becomes a layer, never enters the undo stack, and never
// reaches the document.
namespace mosaic::ui {

class BrushPresetStore;
class ScrubRuler;

// ---- The rail's taxonomy (no FLTK; unit-tested) ------------------------------------------------
//
// The option list is GROUPED, and the grouping is a PARTITION of every base name the dab pipeline
// reads (`core::brush::kDrivenOptions`). That is the preset-tab rule from §8.2 applied one level
// down, and for the same reason: an option that belongs to two groups appears twice and answers to
// neither, and one that belongs to none is an option the user cannot reach. A test walks
// kDrivenOptions and demands exactly one group each.
enum class BrushOptionGroup : std::uint8_t {
    General, // what the mark is: size, opacity, flow, and the two cadence options
    Colour,  // what colour it lays, and how it moves colour already there
    Texture, // the grain on the paper
    Tip,     // the shape of the nib itself -- and the mark-geometry options of the second engine kind
};

// The group `base` files under. Total: an unknown base -- one from a newer preset, or a paintop
// Mosaic has not met -- files under `Tip` rather than vanishing, because a row the user can see and
// switch off is better than a silently ignored option.
[[nodiscard]] BrushOptionGroup brushOptionGroupOf(std::string_view base) noexcept;

// Translated captions. `brushOptionLabel` maps the FILE's base name -- which is a wire name, not a
// label: `h`/`s`/`v` are hue/saturation/value, `SmudgeRate` is "Smudge length", and
// `Texture/Strength/` still carries its trailing slash (§3.2).
[[nodiscard]] std::string brushOptionGroupLabel(BrushOptionGroup group);
[[nodiscard]] std::string brushOptionLabel(std::string_view base);
// The sensor's UI name. ⚠ `ascension` and `declination` are the WIRE names of what a user calls
// tilt direction and tilt elevation (sensors.hpp); this is the only place the UI spelling is written.
[[nodiscard]] std::string brushSensorLabel(core::brush::SensorId id);

// The pseudo-row that is not an option: the TIP page (diameter, angle, spacing). It is in the list
// because the tip is the first thing anyone reaches for and the last thing that is a curve option,
// and it wears a base name no preset format can produce -- '#' is not a legal leading character for
// any base in §3.2 -- so it can share the page-key type with the real bases without a second enum.
inline constexpr std::string_view kBrushTipPage = "#tip";

// One row of the rail's option list: a group caption, or an option.
struct BrushEditorRow {
    std::string base;  // the option's base name, or kBrushTipPage; empty on a header
    BrushOptionGroup group = BrushOptionGroup::General;
    bool header = false;    // a group caption -- not selectable, not checkable
    bool checkable = false; // Opacity and Flow are ALWAYS ON: their `Pressure{X}` bit is written to
                            // shipped files and ignored by the reader, so a checkbox there would be
                            // a control that does nothing (§3.2)
    bool checked = false;
};

// The rail's rows for `preset`: its OWN options, grouped, with a caption above each non-empty group
// and the Tip pseudo-row first inside `Tip`. Options the preset never mentioned are absent -- an
// absent option is not a disabled one (§6.2), and offering a control for one would invent an option
// the file does not carry.
[[nodiscard]] std::vector<BrushEditorRow> brushEditorRows(const io::brush::BrushPreset& preset);

// ---- The host seam -----------------------------------------------------------------------------

// What the pen is doing RIGHT NOW, for the scratchpad (feedback round 1: "Scratchpad does not care
// about tablet pressure"). It is the same shape as `SettingsHost::TabletReading` and for the same
// reason: `ui::TabletInput` reads the device, and neither dialog may reach into the canvas to get at
// it. POST-policy (docs/tablet.md §7), so what the scratchpad paints with is exactly what the brush
// engine gets on the real canvas.
//
// ⚠ `valid` is FALSE for a mouse, and then pressure stays 1.0 -- never 0 (§3.2), or every
// size/flow-driven preset would collapse the stroke to nothing the moment the tablet was unplugged.
struct BrushEditorTabletSample {
    bool valid = false; // a real device sample landed within the last breath
    double pressure = 1.0;
    double xTilt = 0.0;
    double yTilt = 0.0;
    double rotation = 0.0;
    double tangentialPressure = 0.0;
};

// What the editor needs from the app, as plain std::functions (the SettingsHost / LayerEffectsHost
// pattern) so the dialog never reaches into MainWindow.
struct BrushEditorHost {
    // The active foreground colour.
    //
    // ⚠ NOT what the scratchpad paints in any more -- see the ruling in the .cpp (feedback round 1,
    // item 4). The surface is ONE surface: its paper is the preview's paper, so its ink has to be
    // the preview's ink or the two halves of the same canvas contradict each other. Kept because the
    // preview's own ink is derived from the palette and a future "preview in my paint colour"
    // control would want this; nothing reads it today.
    std::function<common::Color8()> foreground;
    // The store changed -- a preset was SAVED, IMPORTED or DELETED -- and `index` is the preset the
    // dock and the active tool should now point at, or -1 for "Default round" (the absence of a
    // preset, which is what a delete of the selected brush falls back to). The host refreshes the
    // dock and re-points the tool. Never fired on close.
    std::function<void(int index)> onSaved;
    // Apply + persist the two eraser ties (§8.4) -- the same two settings the Settings dialog's
    // Tools panes carry, offered here because this is where a user is thinking about the pair.
    std::function<void(bool on)> setEraserSizeFollowsBrush;
    std::function<void(bool on)> setEraserPresetFollowsBrush;

    // ---- The pen (feedback round 1, item 1) ---------------------------------------------------
    // The live device reading. Called from the scratchpad's own FL_PUSH/FL_DRAG, so the host's
    // implementation must PUMP the ring first: on X11 the canvas is what drains it, and the canvas
    // is getting no events at all while this modal is up -- exactly the reason Settings → Tablet's
    // test area has to pump too. Null (or a `valid == false` answer) means "mouse", which is
    // pressure 1 and nothing else.
    std::function<BrushEditorTabletSample()> tabletReading;
    // ⚠ TABLET EVENTS ARE DELIVERED PER-WINDOW ON BOTH PLATFORMS. A backend brought up on the canvas
    // sees NOTHING while the pen hovers this dialog, so the scratchpad would answer "no pressure"
    // for exactly as long as the pen was over it -- which is the whole time. The dialog watches
    // itself on show() and unwatches on hide(), BEFORE it is destroyed (ui::TabletInput::unwatch).
    std::function<void(Fl_Window* win)> tabletWatchWindow;
    std::function<void(Fl_Window* win)> tabletUnwatchWindow;
};

class BrushEditorDialog : public Fl_Double_Window {
public:
    // `store` is the live preset store -- non-const, because Save writes through it. It must outlive
    // the dialog (MainWindow owns both).
    BrushEditorDialog(BrushPresetStore* store, BrushEditorHost host);
    ~BrushEditorDialog() override;

    // Point the editor at a preset (an index into the store). Seeds the working copy, the rail, the
    // page and the preview; call before show(). Returns false for an index the store does not hold
    // -- including -1, the "Default round" cell, which is the ABSENCE of a preset and has no option
    // table to edit.
    bool seed(int presetIndex);
    [[nodiscard]] int presetIndex() const noexcept { return m_index; }

    // Has the working copy been edited since it was seeded (or since the last Save)? The header's
    // dirty dot, and the one thing a close throws away.
    [[nodiscard]] bool dirty() const noexcept { return m_dirty; }

    // Seed the two eraser-tie checkboxes from the live settings. Call before show(), like seed().
    void seedEraserTies(bool sizeFollows, bool presetFollows);

    void reapplyTheme(); // runtime theme change while the dialog is open (MainWindow's observer)

    // ⚠ The pen has to be READ OVER THIS WINDOW, and tablet delivery is per-window on both platforms
    // (host.tabletWatchWindow). Watching on show and unwatching on hide is what makes the scratchpad
    // pressure-sensitive at all -- and the unwatch must happen while the window still exists.
    void show() override;
    void hide() override;

    // ---- For the tests (an Fl_Widget takes events with no display; a WINDOW cannot be shot) -----
    // The rail's current page key, and a way to switch it without a click.
    [[nodiscard]] const std::string& page() const noexcept { return m_page; }
    void selectPage(std::string_view base);
    [[nodiscard]] const std::vector<BrushEditorRow>& rows() const noexcept { return m_rows; }
    // The working copy, exactly as Save would write it.
    [[nodiscard]] const io::brush::LibraryPreset& working() const noexcept { return m_working; }
    // How many times the preview surface has RE-RENDERED the auto stroke, ever. An EVENT count,
    // never reset: a cache size cannot witness a re-render, because a re-render refills the cache to
    // exactly the size it had (the §8.2 lesson, paid for once already).
    [[nodiscard]] std::size_t previewRenders() const noexcept;
    // Run the coalesced preview/params rebuild NOW, instead of on the timeout. The tests have no
    // event loop, and a preview that only ever lands on a timer cannot be asserted on.
    void flushPreviewForTest();
    // Set the working name (what the header's field holds), as typing it would.
    void setWorkingName(std::string name);
    // Save / Save As, as the header's buttons do. Returns the new preset index, or -1 (with the
    // reason in the header's status line) on a refusal.
    int saveForTest(bool saveAs);
    // Delete the preset being edited, WITHOUT the confirmation sheet (a test has no one to ask).
    // Returns what the real button's confirmed branch returns: true when the store lost it.
    bool deleteForTest();
    // Export the working copy to `path`, as the Export… button does once a path has been chosen.
    bool exportForTest(const std::string& path);
    // Can the preset being edited be deleted at all? (The Delete button's enabled state: only one
    // of the user's OWN loose presets, never a shipped one and never one inside a bundle.)
    [[nodiscard]] bool canDeleteWorking() const;
    // How many strokes the RAIL's preset list has rendered, ever. An EVENT count, never reset --
    // the §8.2 rule, because a cache SIZE cannot witness a re-render (a re-render refills the cache
    // to exactly the size it had).
    [[nodiscard]] std::size_t presetListStrokeRenders() const;
    // The rail's preset list row height -- so a test can pin the "rendered wide, drawn cropped"
    // contract without guessing at the layout.
    [[nodiscard]] static int presetListRowHeight() noexcept;
    // The rail's preset list, as a plain widget. ⚠ A test drives THIS rather than the dialog: an
    // unshown `Fl_Window` renders BLACK to an `Fl_Image_Surface`, but an ordinary `Fl_Widget` draws
    // (and takes events) with no display behind it at all.
    [[nodiscard]] Fl_Widget* presetListForTest() const;

protected:
    int handle(int event) override; // Esc closes; routes the outside-click dropdown dismissal

private:
    struct Ui; // widget pointers + per-control bindings (defined in the .cpp)

    void build();        // the fixed chrome: header, rail frame, page frame, preview, footer
    void rebuildRail();  // the preset list + the option list, from the working copy
    void rebuildPage();  // the centre stack for m_page
    // ⚠ DEFERRED, and that is not tidiness. Several of a page's own controls change what the page
    // HOLDS -- picking a different sensor, switching one on, sharing one curve across all of them --
    // and rebuilding the page from inside such a control's callback would `Fl_Group::clear()` the
    // very widget whose handle() is still on the stack. One zero-length timeout puts the rebuild
    // after the event returns. (The same rule the history dock paid for: never free a widget an
    // event is still running inside.)
    void requestPageRebuild();
    static void pageRebuildTimer(void* self);
    void syncHeader();   // name, engine line, fidelity badge, dirty dot, status line

    // An edit happened: mark dirty, repaint the header, and queue the params + preview rebuild.
    void markDirty();
    // ⚠ COALESCED, and that is the §8.2 lesson restated. A preview stroke costs ~1.0-1.7 ms through
    // the real engine, and rebuilding the params RE-MINTS the tip's raster id (a cold dab cache). A
    // ScrubSlider drag fires FL_WHEN_CHANGED on every motion event, so doing either inline would put
    // both on the drag's critical path -- which is exactly the bug the dock's stroke strips were dug
    // out of, one layer up. One rebuild per timeout tick, whatever the drag did.
    void requestPreview();
    static void previewTimer(void* self);
    void rebuildParams(); // working copy -> BrushParams -> the preview surface

    [[nodiscard]] core::brush::CurveOptionData* option(std::string_view base);

    // The new preset's index, or -1 with the reason in the header's status line.
    int doSave(bool saveAs);
    void doImport();
    void doExport();
    // Ask, then delete. `confirmed` skips the sheet (the tests, which have nobody to ask).
    bool doDelete(bool confirmed);
    void doClose();
    void setStatus(std::string text); // the header's one-line result/refusal readout
    // Grey Save / Delete / Export against what the working preset actually permits.
    void syncActionButtons();
    // Drop this window's tablet registration. Idempotent; called from hide() and the destructor.
    void unwatchTablet();
    // The store changed under us: re-seed onto `index` (-1 = nothing left to edit, which closes the
    // dialog) and tell the host so the dock and the tool follow.
    void adoptStoreChange(int index);

    BrushPresetStore* m_store = nullptr;
    BrushEditorHost m_host;
    int m_index = -1;                     // the preset being edited, in the store's numbering
    io::brush::LibraryPreset m_working;   // the mutable copy: params + options + tip reference
    io::brush::LibraryPreset m_original;  // ... as it was seeded. A close restores nothing because
                                          // nothing was ever applied; this is what `dirty` compares
                                          // the user's intent against, and what Revert would use.
    std::vector<BrushEditorRow> m_rows;
    std::string m_page;   // the selected row's base (or kBrushTipPage)
    bool m_dirty = false;
    bool m_seeding = false; // guard: value-sets during seed/rebuild must not fire edits
    bool m_previewPending = false;
    bool m_pageRebuildPending = false;
    // Is this window currently registered with the tablet wiring? show()/hide() can each be called
    // more than once, and the DESTRUCTOR has to unwatch too -- ~Fl_Window calls hide(), but from a
    // base destructor, where our override no longer exists. One flag makes all three idempotent.
    bool m_tabletWatched = false;
    std::shared_ptr<const core::brush::BrushParams> m_params;
    ScrubRuler* m_ruler = nullptr; // owned by this window (a child sub-window), handed to each slider

    std::unique_ptr<Ui> m_ui;
};

} // namespace mosaic::ui
