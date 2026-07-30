#pragma once

#include "common/image.hpp"
#include "core/brush/brush_engine.hpp"  // BrushParams: the panel caches one per preset (m_params)
#include "core/brush/stroke_preview.hpp" // StrokePreviewStyle: the card's paper and ink
#include "ui/icons.hpp"                // IconButton (the clear-filter button)
#include "ui/theme.hpp"                // Palette (the card's paper follows the theme)
#include "ui/widgets.hpp"              // Panel, ScrollView, TextInput

#include <FL/Fl_Widget.H>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Fl_RGB_Image;

namespace mosaic::io::brush {
struct LibraryPreset;
}

// The right dock's **Brush Presets** section (docs/brushes.md §8.2): a filterable grid of the
// shipped library, shown ONLY while the Brush tool is active (RightDock owns that rule -- presets
// belong to the Brush alone; a scatter-and-rotation tip on a *healing* brush is not a feature).
//
// It replaces the stand-in `Fl_Choice` of 117 preset names that lived on the brush context bar. The
// combobox was not merely off-spec, it was unusable: 117 names in a pull-down cannot be hunted
// through by eye, and a name is not what a brush looks like. So the two things this panel exists for
// are the SEARCH FIELD and the THUMBNAIL -- each preset shows the raster it shipped with (§3.1).
//
// Three rules the implementation is built around:
//
//  * ⚠ **Thumbnails decode LAZILY.** A 200x200 RGBA icon per preset is ~160 KB, and decoding all 117
//    up front would cost ~19 MB and stall startup for nothing (io/brush/library.hpp says so
//    explicitly). Only the cells the grid actually PAINTS ask for their icon, and the panel caches
//    what it decoded, downscaled to the cell -- so a scroll pays for the row it reveals and nothing
//    more.
//  * ⚠ **A preset is resolved when it is PICKED, never per stroke.** The panel does not resolve
//    anything; it reports the picked index and `BrushPresetStore::select()` mints the tip's raster id
//    once (a fresh id per stroke is a permanently cold dab cache).
//  * The **"Default round"** cell (slot 0, index -1) is NO preset: the engine's own analytic circle,
//    which is what the Brush painted before presets existed and what it still starts on. It is a real
//    cell, so "get me back to plain" is a click and not a mystery.
namespace mosaic::ui {

class BrushPresetStore;
class BrushPresetPanel;
class PresetSearchInput; // the search field (a TextInput with a placeholder); defined in the .cpp

// ---- Pure grid maths (no FLTK; unit-tested) --------------------------------------------------

// The cell grid for a viewport `viewWidth` px wide (the scroll's width MINUS its scrollbar gutter --
// ask a LAYOUT pass for that, never `scrollbar.visible()` from a draw()). Columns are derived from a
// minimum cell width and the leftover is spread across them, so the grid always fills the dock
// rather than stranding a ragged margin at whatever width the user dragged it to.
// How the dock draws the library (docs/brushes.md §8.2; chosen in Settings -> Tools -> Brush).
//
// `Cards` is the DEFAULT, and it is the mode the user asked for: a grid of tip icons tells you what
// a brush's picture looks like, which is not the question anyone has. The question is what MARK it
// makes -- so a card carries the tip icon AND a stroke of that very brush, laid by the real engine
// (core/brush/stroke_preview.hpp). `Grid` is the denser mode, for when you already know the library
// and want to reach a known cell fast.
enum class PresetDisplayMode : std::uint8_t {
    Grid,
    Cards,
};
[[nodiscard]] const char* presetDisplayModeKey(PresetDisplayMode mode) noexcept; // persisted name
[[nodiscard]] PresetDisplayMode presetDisplayModeFromKey(std::string_view key) noexcept;

struct PresetGridMetrics {
    PresetDisplayMode mode = PresetDisplayMode::Grid;
    int cols = 1;
    int cellW = 0;
    int cellH = 0;
    int thumb = 0; // the thumbnail square inside a cell (cellW minus the cell's own inset)
};
[[nodiscard]] PresetGridMetrics presetGridMetrics(int viewWidth,
                                                  PresetDisplayMode mode = PresetDisplayMode::Grid);

// Total height of `cellCount` cells laid out at `m` (0 for none), including the top/bottom padding.
[[nodiscard]] int presetGridContentHeight(int cellCount, const PresetGridMetrics& m);

// Widget-space pixel rect (common::Rect is a doc-space double rect -- the wrong currency here).
struct PresetCellRect {
    int x = 0, y = 0, w = 0, h = 0;
};

// Cell `slot`'s rect, for a grid whose content origin is (originX, originY).
[[nodiscard]] PresetCellRect presetCellRect(int slot, const PresetGridMetrics& m, int originX,
                                            int originY);

// The thumbnail square inside a cell: the icon, the framing hairline and the fidelity badge are all
// placed against THIS rect. Exposed because a test that re-derives it from kCellInset is a second
// copy of the rule, and the two copies drift.
//
// Grid: centred under the cell's label. Cards: hard against the cell's left edge, with the stroke
// strip taking the rest.
[[nodiscard]] PresetCellRect presetThumbRect(const PresetCellRect& cell, const PresetGridMetrics& m);

// The stroke strip inside a CARD -- the long rectangle the brush's own stroke is rendered into. An
// empty rect in `Grid` mode, which has no strip.
//
// ⚠ ITS WIDTH IS QUANTIZED (see kStrokeBucket in the .cpp) AND THAT IS LOAD-BEARING, not tidiness.
// A stroke preview costs ~1.7 ms to render through the real engine; a strip whose width tracked the
// dock's pixel for pixel would re-render every visible card on every frame of a width drag, which is
// precisely the lag the dock was just dug out of. The strip is rendered at the next bucket UP and
// CROPPED to fit -- cropped, never scaled, because a scaled stroke is a lie about the brush's size.
[[nodiscard]] PresetCellRect presetStrokeRect(const PresetCellRect& cell,
                                              const PresetGridMetrics& m);
// The width the strip's preview is actually RENDERED at (>= the strip's own width; see above).
[[nodiscard]] int presetStrokeRenderWidth(int stripWidth);

// The paper and ink a card's stroke is laid in, for a palette (docs/brushes.md §8.2).
//
// ⚠ IT FOLLOWS THE THEME, and that is a REVERSAL of what this shipped as. Black on white is right in
// the light theme and is what the light theme keeps -- but on a dark UI a white slab per card is a
// row of light boxes in a dark dock, and it hurts to look at. In the dark theme the paper is the
// panel's own ground and the ink is the muted text colour, so a card reads as part of the dock
// instead of as a hole punched in it. (The stroke preview itself is theme-FREE -- `core` never learns
// what a palette is; the style is simply a parameter, and this is the UI choosing it.)
//
// ⚠⚠ AN ERASER GETS ITS OWN PAPER, AND WITHOUT THAT IT WOULD PREVIEW AS NOTHING AT ALL. An eraser can
// only take paper AWAY, and what shows through the hole it bites is the DOCK. So if the paper were the
// dock's own ground -- which is exactly what the dark theme's paper is -- a carve would be a
// panel-coloured hole on a panel-coloured card: invisible. The eraser's paper is therefore the muted
// ink itself: a slab of paint, which the eraser removes to reveal the dock behind it. That contrasts
// in BOTH themes, which the old white-paper-on-a-dark-dock accident only managed in one.
[[nodiscard]] core::brush::StrokePreviewStyle presetStrokeStyle(const Palette& pal, bool eraser);

// The slot under a point in grid-content coords, or -1 for the gaps and past the end. Hit-testing is
// the cell's rect, not its column band: the gutters between cells belong to nobody.
[[nodiscard]] int presetSlotAt(int localX, int localY, int cellCount, const PresetGridMetrics& m);

// ---- Pure filtering (no FLTK; unit-tested) ----------------------------------------------------

// A preset name flattened for matching: lower-cased, with the separators the shipped corpus is full
// of -- `a)_Eraser_Circle`, `b)_Basic-1`, `i)_Wet_Knife` -- collapsed to single spaces. Typing "wet
// knife" must find `i)_Wet_Knife`, and nobody is going to type the parenthesis.
[[nodiscard]] std::string normalizePresetName(std::string_view s);

// True when every whitespace-separated token of `query` appears in `name`'s normalized form. Tokens,
// not a substring: "knife wet" finds it too, and an empty query matches everything.
[[nodiscard]] bool presetMatchesQuery(std::string_view name, std::string_view query);

// The indices of `names` matching `query`, in order.
[[nodiscard]] std::vector<int> filterPresetIndices(const std::vector<std::string>& names,
                                                   std::string_view query);

// The name a CELL wears: the corpus's sort prefix dropped and the underscores spent
// ("a)_Eraser_Circle" -> "Eraser Circle"). The tooltip still carries the exact name, and the filter
// still matches it either way -- this is a label, not an identity.
[[nodiscard]] std::string presetDisplayName(std::string_view name);

// ---- The taxonomy (no FLTK; unit-tested) ------------------------------------------------------
//
// 117 presets in one flat grid is a heap, not a library -- the search field made it usable, not
// organized. The tabs partition it.
//
// ⚠ THE PARTITION COMES FROM THE NAME PREFIX, and that is a deliberate choice over the bundle's own
// tags. The shipped bundle DOES carry 9 tags (Digital, Sketch, Textures, Paint, Ink, FX, Favorites,
// Erasers, Pixel Art) and we do not read them -- because they OVERLAP: 49 presets carry two or more
// and 3 carry none at all. Tags are a cloud; tabs need a PARTITION, or a preset appears in three
// places and answers to none of them. The letter prefix (`a)_`, `b)_`, ... `z)_`) puts every preset
// in exactly one group, and the grid already sorts by it -- invisibly, since presetDisplayName()
// strips it. This makes the taxonomy that was already load-bearing visible.
//
// (The tags remain worth reading LATER, as an orthogonal filter row under the tabs, where overlap is
// a feature rather than a contradiction. That needs ~15 lines in bundle.cpp; nothing here blocks it.)
enum class PresetGroup : std::uint8_t {
    Erasers, // a)  -- and see the note on the ERASER corpus below: this group is NOT how they are split
    Basics,  // b)  basics + airbrush: the dynamics demos
    Draw,    // c) d) e)  pencils, inks, markers
    Paint,   // f) g) h) j)  bristles, dry media, chalk/charcoal, watercolour
    Blend,   // i) k)  wet + blenders (all colorsmudge -- the real smudge engine since §6.6c)
    Texture, // y) z)  textures, screentones, stamps
    Effects, // l) x)  the blend-mode "Adjust" brushes and the filter brushes
    Special, // t) u) v) w)  shapes, pixel art, experimental, normal-map
    Other,   // no recognized prefix -- an imported brush that follows no convention
};
[[nodiscard]] PresetGroup presetGroupOf(std::string_view name) noexcept;

// The dock shows ONE of two corpora, and the split is SEMANTIC, not by name: a preset is an eraser
// because it carries `CompositeOp=erase` (io::brush::BrushPreset::eraserMode), which Mosaic already
// honours by putting the stroke into Erase mode. The `a)_` prefix merely happens to agree on the
// three shipped ones. A preset that erases belongs to the Eraser tool wherever it was filed.
enum class PresetCorpus : std::uint8_t {
    Brush,  // everything that is not an eraser
    Eraser, // ... and everything that is
};

// The tabs, in bar order. `All` is not a group -- it is the absence of one.
enum class PresetTab : std::uint8_t {
    All,
    Basics,
    Draw,
    Paint,
    Blend,
    Texture,
    Effects,
    Special,
    User, // presets from the USER's data dir rather than the shipped bundle
};
[[nodiscard]] std::string presetTabLabel(PresetTab tab); // translated

// ⚠ THE TAB THE DOCK OPENS ON, and there is exactly ONE of these on purpose. Basics, not All (the
// user's call, and it is right): 114 cards is not a place to start, and the six Basic presets plus
// the Default-round cell are. A corpus with no Basics tab -- the Eraser -- falls back to All, which
// PresetTabStrip::setTabs already does for any tab that is not there.
//
// ⚠ The panel and the tab strip BOTH need this, and they used to each carry their own copy. They
// were not even equals: rebuildTabs() overwrites the panel's from the strip's, so the panel's was
// dead code, and a mutant that changed it did not change a single pixel. Two copies of a rule, one of
// them inert, is how the reticle's angle bug happened.
inline constexpr PresetTab kDefaultPresetTab = PresetTab::Basics;

// Does `tab` admit a preset of this group and provenance? `All` admits everything EXCEPT the User
// tab's exclusive claim is not exclusive: a user preset also appears under its media tab, because
// hiding it there would make "All" a lie and force the user to remember where their own brush came
// from. `User` is a SHORTCUT to your own brushes, not a quarantine for them.
[[nodiscard]] bool presetTabAdmits(PresetTab tab, PresetGroup group, bool userInstalled) noexcept;

// The tabs worth SHOWING for a corpus: a tab with nothing in it is a dead affordance. `All` is
// always present when anything is. The User tab appears only once the user has installed a brush --
// which is the rule the user asked for, and it means the bar does not advertise an empty room.
struct PresetTabCounts {
    PresetTab tab = PresetTab::All;
    int count = 0;
};
[[nodiscard]] std::vector<PresetTabCounts> visiblePresetTabs(const std::vector<PresetGroup>& groups,
                                                             const std::vector<bool>& userInstalled);

// ---- Pure fling maths (no FLTK; unit-tested) ---------------------------------------------------
//
// The tab strip and the preset list both scroll by DIRECT DRAG, and a drag released at speed keeps
// the content moving -- a fling, decaying exponentially. The maths is pure so the tests can pin it;
// the widgets only own the timer.

inline constexpr double kPresetFlingTau = 0.35;     // s: the velocity's exponential time constant
inline constexpr double kPresetFlingDeadV = 20.0;   // px/s: below this the eye reads "stopped"
inline constexpr double kPresetFlingMaxV = 6000.0;  // px/s: a release cannot launch faster
inline constexpr double kPresetFlingWindowS = 0.12; // s: drag samples older than this do not vote

// One tick of a fling: the velocity after `dt` seconds of decay, and the distance travelled over
// them. The travel is the ANALYTIC integral, not an Euler step, so the destination does not depend
// on the tick rate -- a janky timer makes a fling later, never longer.
struct PresetFlingStep {
    double dx = 0.0;       // px travelled over the tick (signed)
    double velocity = 0.0; // px/s remaining after it
};
[[nodiscard]] PresetFlingStep presetFlingStep(double velocity, double dt);
[[nodiscard]] bool presetFlingDead(double velocity) noexcept; // below kPresetFlingDeadV

// The release velocity, read from the drag's own recent samples. Only the last kPresetFlingWindowS
// of movement votes: a drag that STOPS and then lets go is a hold, not a fling, and must release
// dead -- which is exactly how a finger on glass behaves.
class PresetFlingTracker {
public:
    void reset() noexcept { m_count = 0; }
    void push(double timeS, double pos) noexcept; // one sample, px along the scroll axis
    // px/s at release time `timeS`, clamped to +-kPresetFlingMaxV; 0 with fewer than two recent
    // samples (a click, or a hold).
    [[nodiscard]] double releaseVelocity(double timeS) const noexcept;

private:
    struct Sample {
        double t = 0.0;
        double pos = 0.0;
    };
    static constexpr std::size_t kCap = 8; // ~64 ms of 125 Hz mouse motion: covers the window
    std::array<Sample, kCap> m_ring{};
    std::size_t m_count = 0; // total ever pushed; the ring index is m_count % kCap
};

// The drag/fling clock (steady, in seconds). The tests pin it so a simulated drag has a definite
// velocity -- two handle() calls in a row are microseconds apart on the real clock, which is a
// velocity assertion cannot stand on. nullopt restores the steady clock.
[[nodiscard]] double presetUiNow();
void presetUiSetNowForTest(std::optional<double> seconds);

// ---- Pure thumbnail scaling (no FLTK; unit-tested) --------------------------------------------

// `src` aspect-fitted into a `box`x`box` OPAQUE thumbnail over `ground`, box-filtered on the way
// down (a 200x200 icon into a ~70 px cell drops 8 source pixels per target one; nearest-neighbour
// would alias the fine bristle tips into noise). Straight alpha is composited over the ground, so
// the result can be blitted opaquely.
[[nodiscard]] common::Image presetThumbnail(const common::Image& src, int box,
                                            common::Color8 ground);

// ---- The grid ---------------------------------------------------------------------------------

// The cell grid: ONE widget, not 118 of them. A widget per cell would mean deleting and rebuilding
// the whole child list on every keystroke in the search field, and would leave "which cells are
// actually visible" -- the question the lazy decode hangs on -- to FLTK's clipping rather than to
// arithmetic we can test. Lives inside the panel's ScrollView, sized to the full content height.
class PresetGrid : public Fl_Widget {
public:
    PresetGrid(int X, int Y, int W, int H, BrushPresetPanel* panel);
    ~PresetGrid() override; // a fling timer must not outlive the widget it scrolls

    // The slots currently displayed: preset indices into the store, with -1 meaning the "Default
    // round" (no-preset) cell. Set by the panel from the filter.
    void setSlots(std::vector<int> slots);
    [[nodiscard]] const std::vector<int>& slots() const noexcept { return m_slots; }
    void setMetrics(const PresetGridMetrics& m) { m_metrics = m; }
    [[nodiscard]] const PresetGridMetrics& metrics() const noexcept { return m_metrics; }
    // The slot showing preset `index` (-1 = the Default-round cell), or -1 when it is filtered out.
    [[nodiscard]] int slotOf(int presetIndex) const;

    // Kinetic scroll: true while a released drag is still carrying the list. flingTick is the
    // timer's own step, public so the tests can drive it deterministically without an event loop.
    [[nodiscard]] bool flinging() const noexcept { return m_flinging; }
    void flingTick(double dt);

protected:
    void draw() override;
    int handle(int event) override;

private:
    void drawCell(int slot, const PresetCellRect& r);
    void drawCardStroke(int index, const PresetCellRect& r); // Cards mode: the name + the stroke strip
    void setHover(int slot); // hover highlight + the cell's tooltip (name, fidelity, what was lost)
    void moveCursor(int delta); // keyboard navigation: pick the slot `delta` away, clamped

    // Drag-to-scroll (the strip's mechanic, vertical): the panel's ScrollView is the thing that
    // actually moves; the grid only steers it.
    [[nodiscard]] int listScrollMax() const;
    void scrollListTo(int target); // clamped
    void startFling(double velocity);
    void stopFling();
    static void cbFling(void* self);

    BrushPresetPanel* m_panel;
    std::vector<int> m_slots;
    PresetGridMetrics m_metrics;
    int m_hover = -1; // hovered SLOT (not a preset index)

    int m_pressX = -1;      // window coords of the press; -1 = no press in flight
    int m_pressY = -1;
    int m_pressScroll = 0;  // the list's yposition at the press
    bool m_dragged = false; // a drag that moved is a scroll, NOT a click on the cell it started over
    // ⚠ Read at PUSH, used at RELEASE. FLTK counts the click on the PUSH that opens it; by the
    // release the count is still standing, but the pick already moved to RELEASE (a press that
    // moved is a scroll), so the two halves of "this was a double-click on that cell" have to be
    // carried across the gesture rather than each asking FLTK at its own moment.
    bool m_pressDouble = false;
    PresetFlingTracker m_flingTracker;
    double m_flingV = 0.0;   // px/s, signed toward larger yposition
    double m_flingPos = 0.0; // fractional yposition the fling has integrated to
    bool m_flinging = false;
};

// ---- The tab strip ----------------------------------------------------------------------------

// One widget, like the grid, and for the same reason: a widget per tab would churn the child list
// every time the corpus changes, and would hand "which tab is under the cursor" to FLTK instead of to
// arithmetic that can be tested.
//
// It SCROLLS HORIZONTALLY, because it has to: eight or nine tabs do not fit in a 280 px dock and the
// dock can be dragged narrower than that. Drag it, or wheel over it. There is no scrollbar -- a
// horizontal bar under a 20 px strip is more chrome than content -- so the strip instead FADES at
// whichever edge it can still scroll toward, which is the affordance that says "there is more".
class PresetTabStrip : public Fl_Widget {
public:
    PresetTabStrip(int X, int Y, int W, int H, BrushPresetPanel* panel);
    ~PresetTabStrip() override; // a fling timer must not outlive the widget it scrolls

    void setTabs(std::vector<PresetTabCounts> tabs);
    [[nodiscard]] const std::vector<PresetTabCounts>& tabs() const noexcept { return m_tabs; }
    void setActive(PresetTab tab);
    [[nodiscard]] PresetTab active() const noexcept { return m_active; }

    // The tab under a widget-local x, or nullopt for the gaps and past the end.
    [[nodiscard]] std::optional<PresetTab> tabAt(int localX) const;
    // Widget-local x and width of tab `i`, in CONTENT coords (i.e. before m_scroll is subtracted).
    [[nodiscard]] PresetCellRect tabRect(std::size_t i) const;
    [[nodiscard]] int contentWidth() const;
    // Scroll the active tab fully into view (a tab you cannot see is a tab you cannot know you are on).
    void scrollActiveIntoView();
    [[nodiscard]] int scrollOffset() const noexcept { return m_scroll; } // px; for the tests

    // Kinetic scroll: true while a released drag is still carrying the strip. flingTick is the
    // timer's own step, public so the tests can drive it deterministically without an event loop.
    [[nodiscard]] bool flinging() const noexcept { return m_flinging; }
    void flingTick(double dt);

protected:
    void draw() override;
    int handle(int event) override;

private:
    void scrollBy(int dx);
    [[nodiscard]] int maxScroll() const;
    void startFling(double velocity);
    void stopFling();
    static void cbFling(void* self);

    BrushPresetPanel* m_panel;
    std::vector<PresetTabCounts> m_tabs;
    PresetTab m_active = kDefaultPresetTab;
    int m_scroll = 0; // px the content is shifted left by
    int m_hover = -1;
    int m_dragFrom = -1; // x where a drag-scroll began (-1 = not dragging)
    int m_dragScroll = 0;
    bool m_dragged = false; // a drag that moved is a scroll, NOT a click on the tab it started over
    PresetFlingTracker m_flingTracker;
    double m_flingV = 0.0;   // px/s, signed toward larger m_scroll
    double m_flingPos = 0.0; // fractional m_scroll the fling has integrated to
    bool m_flinging = false;
};

// ---- The panel --------------------------------------------------------------------------------

class BrushPresetPanel : public Panel {
public:
    BrushPresetPanel(int X, int Y, int W, int H);

    // Point the panel at the library (non-owning; null clears). Decodes nothing -- see the header
    // note. Called once, after the startup scan.
    void setStore(const BrushPresetStore* store);
    [[nodiscard]] const BrushPresetStore* store() const noexcept { return m_store; }

    // A cell was picked: the preset's index, or -1 for the Default-round (no-preset) cell. The HOST
    // resolves it -- BrushPresetStore::select() + seeding the bar's Size/Opacity -- because the
    // "resolve once, never per stroke" rule is the store's to keep, not a widget's.
    void setOnSelect(std::function<void(int index)> cb) { m_onSelect = std::move(cb); }

    // Open the modal brush editor on a preset (docs/brushes.md §8.3). The dock is the editor's
    // launch point -- the "Edit…" button beside the search field, and a DOUBLE-CLICK on a card --
    // because a menu item would be a menu-tree change, and the menu tree is all-or-nothing across
    // 74 catalogs (docs/i18n). The host owns the dialog; the panel only says which preset.
    //
    // ⚠ The index is never -1 here. "Default round" is the ABSENCE of a preset -- there is no
    // authored option table to edit and no file to save over -- so the button greys out on it and a
    // double-click on its cell does nothing (it still SELECTS, which is the whole point of that cell).
    void setOnEdit(std::function<void(int index)> cb) { m_onEdit = std::move(cb); }
    void requestEdit(int index); // called by the button and by PresetGrid's double-click

    // Re-read the store after it changed underneath us -- a preset saved or imported by the editor.
    // Drops the caches for the presets that moved and re-derives the taxonomy, exactly as setStore
    // does; kept apart from setStore so a save does not have to pretend the store is a new one.
    void refreshStore();

    // Mirror the store's active preset into the grid (-1 = Default round). Does NOT fire onSelect:
    // this is the host telling the panel what is selected, not the user picking. Scrolls the cell
    // into view, so restoring the persisted preset at startup shows it.
    void setSelected(int index);
    [[nodiscard]] int selected() const noexcept { return m_selected; }

    // Which corpus the panel shows. The Brush tool gets everything that is not an eraser; the Eraser
    // tool gets the erasers, and nothing else. Switching corpus re-runs the filter and re-derives the
    // tabs -- and drops the tab strip entirely for the Eraser, which has three presets and needs no
    // filing system for them.
    void setCorpus(PresetCorpus corpus);
    [[nodiscard]] PresetCorpus corpus() const noexcept { return m_corpus; }

    // Grid of icons, or cards carrying a rendered stroke (the default). Set from Settings ->
    // Tools -> Brush; persisted by NAME, like the preset and the tab.
    void setDisplayMode(PresetDisplayMode mode);
    [[nodiscard]] PresetDisplayMode displayMode() const noexcept { return m_displayMode; }

    // The active tab. Persisted BY NAME through settings, like the preset itself -- an enum's
    // ordinal would silently re-point the moment a tab is inserted.
    void setTab(PresetTab tab);
    [[nodiscard]] PresetTab tab() const noexcept { return m_tab; }
    [[nodiscard]] PresetTabStrip* tabStrip() const noexcept { return m_tabs; }
    // Called by the strip when a tab is clicked.
    void pickTab(PresetTab tab);

    // The search box's current text. setFilter() is for the tests and for the clear button.
    void setFilter(const std::string& text);
    [[nodiscard]] const std::string& filter() const noexcept { return m_filter; }

    // Fired by the search field's own callback and by the clear button.
    void onFilterEdited();

    // Called by the grid when a cell is picked (public so PresetGrid can reach it).
    void pick(int presetIndex);

    // What the grid needs to paint and narrate a cell (-1 = the Default-round cell throughout).
    [[nodiscard]] const io::brush::LibraryPreset* presetAt(int index) const;
    [[nodiscard]] std::string nameAt(int index) const;      // the EXACT library name
    [[nodiscard]] std::string tooltipFor(int index) const;  // name + fidelity + what was dropped
    // Did this preset come from the user's own data directory? The cell's USER BADGE, and the
    // taxonomy the User tab is built from -- read from the cached vector rather than from the store
    // so the grid and the tab strip can never disagree about which presets are whose.
    [[nodiscard]] bool userInstalled(int index) const;

    // The decoded, cell-sized thumbnail for `index` (-1 = the Default-round cell's drawn glyph, which
    // has no raster and returns null). Decodes + caches on first ask -- so ONLY the cells that
    // actually paint ever pay. Null when the preset carries no usable icon (its cell then draws the
    // fallback glyph, never an empty hole); the miss is cached too, so a broken .kpp is not re-opened
    // every frame.
    [[nodiscard]] Fl_RGB_Image* thumbnailFor(int index);
    // The same thumbnail's PIXELS, for compositing the fidelity dot's anti-aliased rim against the
    // icon it sits on. Decodes on first ask, exactly like thumbnailFor.
    [[nodiscard]] const common::Image* thumbnailPixels(int index);
    // ⚠ BOTH OF THESE ARE CACHE SIZES, AND A CACHE SIZE CANNOT WITNESS A RE-DECODE -- a re-decode
    // refills the cache to exactly the size it had. To assert that a resize did not go back to the
    // archive, read `PresetLibrary::iconLoads()`, which counts EVENTS. (Written down because the
    // first version of that test used the count below, and a mutant that dropped m_icons on every
    // re-metric -- the original bug, put back -- sailed straight through it.)
    //
    // The brush's own stroke, rendered by the real engine into the card's strip (Cards mode only).
    // Rendered + cached on first ask, exactly like a thumbnail, so only the cards that actually paint
    // ever pay -- one costs ~1.7 ms, which is three orders of magnitude more than a box filter and is
    // why the strip's width is bucketed rather than tracking the dock pixel for pixel.
    [[nodiscard]] Fl_RGB_Image* strokePreviewFor(int index, int renderWidth, int height);
    // ⚠⚠ HOW MANY STROKES HAVE BEEN RENDERED, EVER. An EVENT count: monotonic, never reset, and the
    // ONLY honest way to assert that a drag did not re-render. A CACHE SIZE CANNOT WITNESS A
    // RE-RENDER -- a re-render refills the cache to exactly the size it had, so a mutant that threw
    // the cache away every frame would sail straight through a size assertion. (That is not a
    // hypothetical: it happened to the icon cache's first test, and the mutant survived.)
    [[nodiscard]] std::size_t strokeRenders() const noexcept { return m_strokeRenders; }
    // The BrushParams a cell previews with (-1 = the Default-round cell, whose meaning depends on the
    // CORPUS: the Brush's plain nib paints, the Eraser's carves). For the tests.
    [[nodiscard]] const core::brush::BrushParams* paramsForTest(int index) { return paramsFor(index); }

    // How many presets are held decoded (~200x200 RGBA apiece). Survives a resize and a re-theme.
    [[nodiscard]] std::size_t cachedIconCount() const noexcept { return m_icons.size(); }
    // How many cells hold a thumbnail SIZED for the current cell + ground. Dropped whenever either
    // changes, which is often; that is cheap now, and it is not a decode.
    [[nodiscard]] std::size_t cachedThumbnailCount() const noexcept { return m_thumbs.size(); }

    void resize(int X, int Y, int W, int H) override;
    void reapplyTheme() override;

    // The grid's viewport, for the tests and for scrollCellIntoView.
    [[nodiscard]] ScrollView* scroll() const noexcept { return m_scroll; }
    [[nodiscard]] PresetGrid* grid() const noexcept { return m_grid; }
    // How many presets the current filter admits, and how many there are in all (the header's
    // "12 of 117" readout).
    [[nodiscard]] int matchCount() const;
    [[nodiscard]] int totalCount() const;
    // How many presets the ACTIVE corpus + tab hold, before the search. The header's denominator --
    // the Brush is never offered the erasers, so counting the whole library there would print a total
    // the grid can never reach.
    [[nodiscard]] int corpusCount() const;

protected:
    void draw() override; // the section header + the "12 of 117" readout + the empty-state caption

private:
    void layoutChildren();      // place the search row + the scroll; re-metric the grid
    void rebuildSlots();        // re-run the filter and re-lay the grid (no widget churn)
    void scrollCellIntoView(int slot);
    [[nodiscard]] common::Color8 cellGround() const; // what a thumbnail is composited over

    void rebuildTabs(); // re-derive which tabs the current corpus even has
    void syncEditButton(); // grey the "Edit…" button unless a real preset is selected
    // Is preset `index` in the corpus the panel is showing? The split is `eraserMode`, never the name.
    [[nodiscard]] bool inCorpus(int index) const;

    const BrushPresetStore* m_store = nullptr;
    std::vector<std::string> m_names;    // cached from the store (the filter's input)
    std::vector<PresetGroup> m_groups;   // ... and their tabs, derived once per store
    std::vector<bool> m_userInstalled;   // ... and whether each came from the user's own data dir
    int m_selected = -1;
    std::string m_filter;
    PresetCorpus m_corpus = PresetCorpus::Brush;
    PresetTab m_tab = kDefaultPresetTab;
    PresetDisplayMode m_displayMode = PresetDisplayMode::Cards;
    std::function<void(int)> m_onSelect;
    std::function<void(int)> m_onEdit;

    PresetSearchInput* m_search = nullptr;
    IconButton* m_clearButton = nullptr; // shown only while the filter is non-empty
    FlatButton* m_editButton = nullptr;  // "Edit…": the modal editor's launch point (§8.3)
    PresetTabStrip* m_tabs = nullptr;
    ScrollView* m_scroll = nullptr;
    PresetGrid* m_grid = nullptr;

    // TWO caches, and the split is the whole point.
    //
    // m_icons: the preset's icon as it came out of the archive (~200x200 RGBA, ~160 KB), keyed by
    // preset index. SIZE-INDEPENDENT and THEME-INDEPENDENT, so NOTHING below invalidates it and a
    // preset is decoded at most ONCE per session. An entry holding an empty image is a remembered
    // MISS (a .kpp whose icon would not decode) -- a broken preset is not re-opened every frame.
    //
    // m_thumbs: that icon downscaled to the CURRENT cell and composited over the CURRENT panel
    // ground. Both of those change, so this one is dropped when either does -- and it is dropped a
    // LOT: a 3-column grid re-metrics every 3 px of a dock-width drag.
    //
    // ⚠ THE BUG THIS SPLIT FIXES, because the old comment here CLAIMED the fix and the code did not
    // do it: with only m_thumbs, "re-scale from the source" meant going back to the SOURCE ARCHIVE.
    // Every 3 px of drag re-read a 17.5 MB zip a dozen times. `re-scale from the source` is now
    // literally true -- the source is m_icons, and it is in memory.
    struct Thumb {
        common::Image pixels;
        std::unique_ptr<Fl_RGB_Image> img; // views `pixels`: stored and dropped together
    };
    [[nodiscard]] const common::Image* iconFor(int index); // decode-once; null on a cached miss
    std::unordered_map<int, common::Image> m_icons;
    std::unordered_map<int, Thumb> m_thumbs;
    int m_thumbSize = 0; // the box m_thumbs were rendered for (0 = nothing cached yet)

    // A THIRD cache, and it is the expensive one: the brush's own stroke, laid by the real engine.
    //
    // m_params: `presetBrushParams()` for a preset, built ONCE. It is not merely slow to rebuild --
    // it MINTS THE TIP'S RASTER ID, and a fresh id is a permanently cold dab cache. Size- and
    // theme-independent; nothing drops it.
    //
    // m_strokes: the rendered strip. Keyed by preset index and dropped when the strip's SIZE changes
    // -- which is why the strip's width is bucketed (presetStrokeRenderWidth): at ~1.7 ms a card, a
    // strip that re-rendered on every pixel of a width drag would be the icon-cache bug again, in a
    // form a hundred times more expensive.
    struct Stroke {
        common::Image pixels;
        std::unique_ptr<Fl_RGB_Image> img; // views `pixels`: stored and dropped together
    };
    [[nodiscard]] const core::brush::BrushParams* paramsFor(int index); // build-once
    std::unordered_map<int, core::brush::BrushParams> m_params;
    std::unordered_map<int, Stroke> m_strokes;
    int m_strokeW = 0; // the strip m_strokes were rendered for (0 = nothing cached yet)
    int m_strokeH = 0;
    std::size_t m_strokeRenders = 0; // EVENTS, never reset: see strokeRenders()
};

} // namespace mosaic::ui
