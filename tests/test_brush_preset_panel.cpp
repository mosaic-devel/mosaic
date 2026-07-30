// The dock's Brush-preset section (docs/brushes.md §8.2) -- the grid that replaced the stand-in
// combobox -- and the RightDock container that holds it under Layers|History.
//
// An Fl_Widget needs no display to exist or to take an event, and Fl::e_x / e_y / e_keysym are
// public, so the whole interaction surface is testable without a window. The layout maths and the
// filter are pure free functions on purpose: they are the parts that can be wrong in a way a
// screenshot would not show.
#include "common/image.hpp"
#include "common/settings.hpp"
#include "core/brush/stroke_preview.hpp"
#include "io/brush/library.hpp"
#include "io/brush/preset_brush.hpp"
#include "io/io.hpp" // savePng: the panel was judged by looking at it (MOSAIC_PANEL_SHOT)
#include "ui/brush_preset_panel.hpp"
#include "ui/brush_presets.hpp"
#include "ui/layer_panel.hpp"
#include "ui/right_dock.hpp"
#include "ui/theme.hpp"

#include <FL/Fl.H>
#include <FL/Enumerations.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace mosaic;
using mosaic::ui::BrushPresetPanel;
using mosaic::ui::PresetGridMetrics;

namespace {

// The shipped bundle, replayed straight from the source tree (MOSAIC_SHIPPED_DATA_DIR): the tests
// stay hermetic -- they never consult installedDataDir() or the user's data dir.
std::filesystem::path shippedBrushDir() {
    return std::filesystem::path(MOSAIC_SHIPPED_DATA_DIR) / "brushes";
}

void send(Fl_Widget& w, int event) {
    w.handle(event); // the house idiom: the override is protected, the base's handle() is public
}

} // namespace

// ---- the grid's layout maths -----------------------------------------------------------------

TEST_CASE("presetGridMetrics fills the viewport at any dock width") {
    // The default dock (280 px) minus the splitter band and the scroll's own inset.
    const PresetGridMetrics m = ui::presetGridMetrics(270);
    CHECK(m.cols >= 2);
    CHECK(m.thumb == m.cellW - 8);
    CHECK(m.cellH > m.thumb); // the name line lives under the thumbnail

    // Whatever the width, the columns plus their gaps fit inside it -- a cell must never be laid
    // out past the right edge (which is what a fixed cell width would eventually do).
    for (int viewW = 60; viewW <= 700; viewW += 7) {
        const PresetGridMetrics g = ui::presetGridMetrics(viewW);
        CHECK(g.cols >= 1);
        CHECK(g.cellW >= 24);
        const int used = g.cols * g.cellW + (g.cols - 1) * 6 + 2 * 6; // cells + gaps + padding
        CHECK(used <= std::max(viewW, 60));
    }

    // Wider dock -> more columns, never fewer.
    CHECK(ui::presetGridMetrics(500).cols >= ui::presetGridMetrics(280).cols);
    CHECK(ui::presetGridMetrics(120).cols == 1);
}

TEST_CASE("presetGridContentHeight counts rows, not cells") {
    const PresetGridMetrics m = ui::presetGridMetrics(270);
    REQUIRE(m.cols == 3);
    CHECK(ui::presetGridContentHeight(0, m) == 0);
    // 1..3 cells are one row; 4 starts a second.
    CHECK(ui::presetGridContentHeight(1, m) == ui::presetGridContentHeight(3, m));
    CHECK(ui::presetGridContentHeight(4, m) > ui::presetGridContentHeight(3, m));
    CHECK(ui::presetGridContentHeight(6, m) == ui::presetGridContentHeight(4, m));
}

TEST_CASE("presetSlotAt hits cells and misses the gutters between them") {
    const PresetGridMetrics m = ui::presetGridMetrics(270);
    REQUIRE(m.cols == 3);
    const ui::PresetCellRect r0 = ui::presetCellRect(0, m, 0, 0);
    const ui::PresetCellRect r4 = ui::presetCellRect(4, m, 0, 0);

    CHECK(ui::presetSlotAt(r0.x + 2, r0.y + 2, 10, m) == 0);
    CHECK(ui::presetSlotAt(r0.x + r0.w - 1, r0.y + r0.h - 1, 10, m) == 0);
    CHECK(ui::presetSlotAt(r4.x + r4.w / 2, r4.y + r4.h / 2, 10, m) == 4);
    // The gap between column 0 and column 1 belongs to nobody.
    CHECK(ui::presetSlotAt(r0.x + r0.w + 2, r0.y + 4, 10, m) == -1);
    // ... as does the space past the last cell, and the padding above the first.
    CHECK(ui::presetSlotAt(r0.x + 2, r0.y - 3, 10, m) == -1);
    CHECK(ui::presetSlotAt(r0.x + 2, r0.y + 2, 0, m) == -1);
    CHECK(ui::presetSlotAt(r4.x + r4.w / 2, r4.y + r4.h / 2, 3, m) == -1); // filtered down to 3
}

// ---- the fling maths -------------------------------------------------------------------------

TEST_CASE("presetFlingStep: analytic decay -- the tick rate changes nothing but the frame times") {
    // Composability is the whole point of the analytic step: one 32 ms tick lands exactly where
    // two 16 ms ticks do, so a janky timer delays a fling but never lengthens it.
    const ui::PresetFlingStep one = ui::presetFlingStep(1000.0, 0.032);
    const ui::PresetFlingStep a = ui::presetFlingStep(1000.0, 0.016);
    const ui::PresetFlingStep b = ui::presetFlingStep(a.velocity, 0.016);
    CHECK(one.velocity == doctest::Approx(b.velocity).epsilon(1e-12));
    CHECK(one.dx == doctest::Approx(a.dx + b.dx).epsilon(1e-12));

    // The velocity decays every tick, the travel follows its sign, and the whole run is v0*tau
    // minus the sub-pixel tail the dead threshold shaves off.
    double v = 1000.0;
    double travelled = 0.0;
    int ticks = 0;
    while (!ui::presetFlingDead(v) && ticks < 10000) {
        const ui::PresetFlingStep s = ui::presetFlingStep(v, 1.0 / 60.0);
        CHECK(std::abs(s.velocity) < std::abs(v));
        CHECK(s.dx > 0.0);
        travelled += s.dx;
        v = s.velocity;
        ++ticks;
    }
    CHECK(ticks < 10000); // it does stop
    CHECK(travelled > 0.9 * 1000.0 * ui::kPresetFlingTau);
    CHECK(travelled < 1000.0 * ui::kPresetFlingTau);

    // Sign symmetry, and a degenerate dt moves nothing and loses nothing.
    CHECK(ui::presetFlingStep(-1000.0, 0.016).dx ==
          doctest::Approx(-ui::presetFlingStep(1000.0, 0.016).dx));
    CHECK(ui::presetFlingStep(1000.0, 0.0).dx == 0.0);
    CHECK(ui::presetFlingStep(1000.0, 0.0).velocity == 1000.0);
    CHECK(ui::presetFlingDead(ui::kPresetFlingDeadV - 1e-9));
    CHECK_FALSE(ui::presetFlingDead(ui::kPresetFlingDeadV));
}

TEST_CASE("PresetFlingTracker: the RECENT movement is the fling; a hold releases dead") {
    ui::PresetFlingTracker t;
    CHECK(t.releaseVelocity(0.0) == 0.0); // nothing pushed
    t.push(0.00, 0.0);
    CHECK(t.releaseVelocity(0.01) == 0.0); // one sample has no velocity in it

    t.push(0.02, 10.0);
    t.push(0.04, 20.0);
    t.push(0.06, 30.0);
    CHECK(t.releaseVelocity(0.07) == doctest::Approx(500.0)); // 30 px over 60 ms

    // ⚠ The pointer STOPPED, then let go: a hold, not a fling. The release is dead even though the
    // drag itself was fast -- which is exactly what a finger putting a list down does.
    CHECK(t.releaseVelocity(0.30) == 0.0);

    // Only the recent window votes: a drag that crawls and then whips flings at the LATE rate, not
    // at the whole gesture's average.
    ui::PresetFlingTracker late;
    late.push(0.00, 0.0);
    late.push(0.10, 5.0);
    late.push(0.20, 10.0);
    late.push(0.22, 40.0); // 1500 px/s at the end (the average would read ~180)
    CHECK(late.releaseVelocity(0.23) == doctest::Approx(1500.0));

    // The clamp: two samples a millisecond apart must not launch the list into orbit.
    ui::PresetFlingTracker fast;
    fast.push(0.000, 0.0);
    fast.push(0.001, 100.0);
    CHECK(fast.releaseVelocity(0.001) == ui::kPresetFlingMaxV);
    ui::PresetFlingTracker fastNeg;
    fastNeg.push(0.000, 0.0);
    fastNeg.push(0.001, -100.0);
    CHECK(fastNeg.releaseVelocity(0.001) == -ui::kPresetFlingMaxV);

    // reset() forgets the gesture.
    t.reset();
    t.push(1.00, 0.0);
    CHECK(t.releaseVelocity(1.01) == 0.0);
}

// ---- the filter ------------------------------------------------------------------------------

TEST_CASE("the filter reads through the corpus's separators") {
    // The shipped names are `a)_Eraser_Circle`, `b)_Basic-1`, `i)_Wet_Knife`... Nobody is going to
    // type the parenthesis, and the underscores are not words.
    CHECK(ui::normalizePresetName("i)_Wet_Knife") == "i wet knife");
    CHECK(ui::normalizePresetName("b)_Basic-1") == "b basic 1");
    CHECK(ui::normalizePresetName("  Shoujo__Bubbles  ") == "shoujo bubbles");

    CHECK(ui::presetMatchesQuery("i)_Wet_Knife", "wet knife"));
    CHECK(ui::presetMatchesQuery("i)_Wet_Knife", "WET"));
    CHECK(ui::presetMatchesQuery("i)_Wet_Knife", "knife wet")); // tokens, not a substring
    CHECK(ui::presetMatchesQuery("i)_Wet_Knife", ""));          // an empty query matches everything
    CHECK_FALSE(ui::presetMatchesQuery("i)_Wet_Knife", "dry knife"));
    CHECK_FALSE(ui::presetMatchesQuery("a)_Eraser_Circle", "knife"));
}

TEST_CASE("filterPresetIndices keeps the library's order") {
    const std::vector<std::string> names = {"a)_Eraser_Circle", "b)_Basic-1", "i)_Wet_Knife",
                                            "Shoujo_Bubbles"};
    CHECK(ui::filterPresetIndices(names, "").size() == 4);
    const std::vector<int> hits = ui::filterPresetIndices(names, "b");
    CHECK(hits == std::vector<int>{1, 3}); // "b)_Basic-1" and "Shoujo_Bubbles"
    CHECK(ui::filterPresetIndices(names, "eraser") == std::vector<int>{0});
    CHECK(ui::filterPresetIndices(names, "zzz").empty());
}

TEST_CASE("a cell's label sheds the corpus's sort prefix, and the name survives it") {
    CHECK(ui::presetDisplayName("a)_Eraser_Circle") == "Eraser Circle");
    CHECK(ui::presetDisplayName("i)_Wet_Knife") == "Wet Knife");
    CHECK(ui::presetDisplayName("Shoujo_Bubbles") == "Shoujo Bubbles");
    CHECK(ui::presetDisplayName("b)_Basic-1") == "Basic-1"); // the hyphen is part of the name
    CHECK(ui::presetDisplayName("Plain") == "Plain");
    CHECK(ui::presetDisplayName("") == "");
}

// ---- the thumbnail scaler --------------------------------------------------------------------

TEST_CASE("presetThumbnail box-filters into an opaque, square cell") {
    common::Image src(200, 100); // deliberately non-square: it must be LETTERBOXED, never stretched
    src.fill(common::Color8{20, 200, 40, 255});

    const common::Color8 ground{18, 20, 24, 255};
    const common::Image t = ui::presetThumbnail(src, 64, ground);
    CHECK(t.width == 64);
    CHECK(t.height == 64);
    for (std::size_t i = 3; i < t.rgba.size(); i += 4)
        CHECK(t.rgba[i] == 255); // opaque throughout: it is blitted, not composited

    const auto at = [&](int x, int y) {
        const std::size_t p = ((static_cast<std::size_t>(y) * 64) + x) * 4;
        return common::Color8{t.rgba[p], t.rgba[p + 1], t.rgba[p + 2], t.rgba[p + 3]};
    };
    CHECK(at(32, 32) == common::Color8{20, 200, 40, 255}); // the image, dead centre
    CHECK(at(32, 1) == ground);                            // the letterbox above it
    CHECK(at(32, 62) == ground);                           // ... and below

    // A transparent source composites over the ground rather than punching a hole in it.
    common::Image clear(10, 10);
    const common::Image over = ui::presetThumbnail(clear, 32, ground);
    CHECK(over.rgba[0] == ground.r);
    CHECK(over.rgba[3] == 255);

    CHECK(ui::presetThumbnail(common::Image(), 16, ground).width == 16); // empty source: no crash
}

// ---- the dock's split ------------------------------------------------------------------------

TEST_CASE("presetSplit gives the whole dock to the layers when the section is hidden") {
    const ui::DockSplit s = ui::presetSplit(800, 260, /*presetsVisible=*/false);
    CHECK(s.layersH == 800);
    CHECK(s.presetH == 0);
}

TEST_CASE("presetSplit honours the remembered height, and clamps it against the dock") {
    const int strip = ui::RightDock::splitterHeight();

    const ui::DockSplit s = ui::presetSplit(800, 260, true);
    CHECK(s.presetH == 260);
    CHECK(s.layersH == 800 - strip - 260);

    // A height the dock cannot afford is squeezed -- but the layer list keeps its minimum, and the
    // section keeps its own. Neither ever vanishes on a dock that can hold both.
    const ui::DockSplit tall = ui::presetSplit(600, 5000, true);
    CHECK(tall.presetH == 600 - strip - 180); // the list's minimum survives
    CHECK(tall.layersH == 180);
    const ui::DockSplit tiny = ui::presetSplit(600, 10, true);
    CHECK(tiny.presetH == 150); // ... and so does the grid's
    CHECK(tiny.layersH == 600 - strip - 150);

    // A dock too short for BOTH minimums halves what there is rather than dropping either region.
    const ui::DockSplit squeezed = ui::presetSplit(200, 260, true);
    CHECK(squeezed.presetH > 0);
    CHECK(squeezed.layersH > 0);
    CHECK(squeezed.presetH + squeezed.layersH == 200 - strip);
    // Degenerate docks do not go negative or divide by zero.
    CHECK(ui::presetSplit(0, 260, true).presetH == 0);
    CHECK(ui::presetSplit(4, 260, true).layersH >= 0);
}

TEST_CASE("RightDock stacks the two regions, and the layers panel takes over when presets hide") {
    ui::RightDock dock(700, 30, 280, 800);
    REQUIRE(dock.layers() != nullptr);
    REQUIRE(dock.presets() != nullptr);
    CHECK_FALSE(dock.presetsVisible()); // Brush-only: the host turns it on

    CHECK(dock.layers()->h() == 800);
    CHECK_FALSE(dock.presets()->visible());

    dock.setPresetHeight(260);
    dock.setPresetsVisible(true);
    CHECK(dock.presets()->visible());
    CHECK(dock.presets()->h() == 260);
    CHECK(dock.layers()->h() == 800 - ui::RightDock::splitterHeight() - 260);
    // The section is pinned to the dock's bottom edge; the splitter sits between them.
    CHECK(dock.presets()->y() + dock.presets()->h() == 30 + 800);
    CHECK(dock.presets()->y() ==
          dock.layers()->y() + dock.layers()->h() + ui::RightDock::splitterHeight());
    // Both regions span the dock's full width (the left grab band runs down their shared edge).
    CHECK(dock.presets()->w() == 280);
    CHECK(dock.presets()->x() == 700);

    dock.setPresetsVisible(false);
    CHECK(dock.layers()->h() == 800); // the layers take the whole dock back
}

TEST_CASE("RightDock: dragging the horizontal splitter resizes the section and reports once") {
    ui::RightDock dock(700, 30, 280, 800);
    dock.setPresetHeight(260);
    dock.setPresetsVisible(true);

    int committed = 0;
    int lastHeight = 0;
    dock.setOnPresetHeightChanged([&](int h, bool commit) {
        lastHeight = h;
        if (commit)
            ++committed;
    });

    const int splitTop = dock.layers()->y() + dock.layers()->h();
    Fl::e_x = 800;
    Fl::e_y = splitTop + 2;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    send(dock, FL_PUSH);

    Fl::e_y = splitTop - 100; // drag the strip UP: the section grows
    send(dock, FL_DRAG);
    CHECK(dock.presets()->h() == 360);
    CHECK(committed == 0); // a drag frame is not a commit -- the settings file is not a scratchpad
    send(dock, FL_RELEASE);
    CHECK(committed == 1);
    CHECK(lastHeight == 360);

    // A drag past the clamp reports the height the dock ACTUALLY shows, never the wish.
    Fl::e_y = dock.layers()->y() + dock.layers()->h() + 2;
    send(dock, FL_PUSH);
    Fl::e_y = dock.y() - 500; // way above the dock
    send(dock, FL_DRAG);
    send(dock, FL_RELEASE);
    CHECK(lastHeight == dock.presets()->h());
    CHECK(dock.layers()->h() == 180); // the layer list's minimum held
}

TEST_CASE("RightDock: the left edge is the dock-width splitter (it moved off the LayerPanel)") {
    ui::RightDock dock(700, 30, 280, 800);
    int widths = 0;
    int commits = 0;
    int last = 0;
    dock.setOnWidthRequest([&](int width, bool commit) {
        last = width;
        ++widths;
        if (commit)
            ++commits;
    });

    Fl::e_x = 702; // inside the 5 px grab band on the dock's left edge
    Fl::e_y = 400;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    send(dock, FL_PUSH);
    Fl::e_x = 660; // drag left: the dock gets wider
    send(dock, FL_DRAG);
    CHECK(widths == 1);
    CHECK(last == 320); // (dock right edge 980) - 660
    send(dock, FL_RELEASE);
    CHECK(commits == 1);
}

// ---- the panel -------------------------------------------------------------------------------

TEST_CASE("BrushPresetPanel: an empty store still lays out, and offers the round tip") {
    BrushPresetPanel panel(0, 0, 280, 400);
    CHECK(panel.totalCount() == 0);
    CHECK(panel.matchCount() == 0);
    // Slot 0 is "Default round" = NO preset, and it exists even with no library at all: it is how a
    // user gets back to the engine's own analytic circle.
    REQUIRE(panel.grid() != nullptr);
    CHECK(panel.grid()->slots() == std::vector<int>{-1});
    CHECK(panel.selected() == -1);
    CHECK(panel.cachedThumbnailCount() == 0);
}

TEST_CASE("BrushPresetPanel: the shipped library, filtered, picked -- and decoded LAZILY") {
    ui::BrushPresetStore store;
    const int n = store.scanDir(shippedBrushDir());
    REQUIRE_MESSAGE(n > 0, "the shipped bundle should carry presets");

    // The BRUSH corpus is the library MINUS the erasers -- they belong to the Eraser tool (§8.4), and
    // `totalCount` still reports the whole library because that is what it means.
    int erasers = 0;
    for (const io::brush::LibraryPreset& p : store.presets())
        if (p.preset.eraserMode)
            ++erasers;
    REQUIRE(erasers > 0);
    const int brushes = n - erasers;

    BrushPresetPanel panel(0, 0, 280, 400);
    panel.setStore(&store);
    // ⚠ ALL, EXPLICITLY. The dock OPENS on Basics now (the user's call), so a case about the whole
    // corpus has to say so -- an implicit default would quietly turn this into a case about the six
    // Basic presets, and it would still pass every line below by measuring the wrong thing.
    panel.setTab(ui::PresetTab::All);
    CHECK(panel.totalCount() == n);
    CHECK(panel.matchCount() == brushes);
    // Default round + every non-eraser preset.
    CHECK(static_cast<int>(panel.grid()->slots().size()) == brushes + 1);

    // ⚠ THE LAZY-DECODE GUARANTEE. Handing the panel 117 presets must decode NOTHING: a 200x200 RGBA
    // icon apiece is ~19 MB and a stalled startup. Only a cell that actually paints asks for one.
    CHECK(store.library().iconLoads() == 0);
    CHECK(panel.cachedIconCount() == 0);
    CHECK(panel.cachedThumbnailCount() == 0);

    // The filter narrows the grid without touching a widget.
    panel.setFilter("wet");
    CHECK(panel.matchCount() > 0);
    CHECK(panel.matchCount() < brushes);
    for (const int index : panel.grid()->slots())
        CHECK(ui::presetMatchesQuery(panel.nameAt(index), "wet"));
    CHECK(panel.cachedIconCount() == 0); // ... and still decodes nothing

    panel.setFilter("no-such-brush-anywhere");
    CHECK(panel.matchCount() == 0);
    CHECK(panel.grid()->slots().empty()); // not even Default round matches
    panel.setFilter("");
    CHECK(panel.matchCount() == brushes);

    // Asking for ONE thumbnail decodes exactly one.
    const int wet = store.indexOfName("i)_Wet_Knife");
    REQUIRE(wet >= 0);
    CHECK(panel.thumbnailFor(wet) != nullptr);
    CHECK(panel.cachedIconCount() == 1);
    CHECK(panel.cachedThumbnailCount() == 1);
    CHECK(panel.thumbnailFor(wet) != nullptr);
    CHECK(panel.cachedIconCount() == 1); // cached: the bundle is not re-opened

    // A pick reports the index; the HOST resolves it (select() mints the tip's raster id ONCE).
    int picked = -999;
    int fired = 0;
    panel.setOnSelect([&](int index) {
        picked = index;
        ++fired;
    });
    panel.pick(wet);
    CHECK(picked == wet);
    CHECK(fired == 1);
    CHECK(panel.selected() == wet);

    // ... and setSelected() is the host telling the panel, so it must NOT fire back.
    panel.setSelected(-1);
    CHECK(fired == 1);
    CHECK(panel.selected() == -1);
}

TEST_CASE("BrushPresetPanel: a dock-width drag re-scales from memory, never from the archive") {
    ui::BrushPresetStore store;
    REQUIRE(store.scanDir(shippedBrushDir()) > 0);

    BrushPresetPanel panel(0, 0, 280, 400);
    // ⚠ GRID, EXPLICITLY. This case is about the thumbnail that RE-SCALES with the cell, and only the
    // grid has one: a card's tip icon is a fixed 54 px, so a width drag never re-metrics it and the
    // premise below could not hold. (The panel defaults to Cards -- the mode the user asked for -- so
    // an implicit default here would silently turn this into a case that proves nothing. The premise
    // check caught exactly that when Cards landed.)
    panel.setDisplayMode(ui::PresetDisplayMode::Grid);
    panel.setStore(&store);

    const int wet = store.indexOfName("i)_Wet_Knife");
    REQUIRE(wet >= 0);
    REQUIRE(panel.thumbnailFor(wet) != nullptr);
    REQUIRE(panel.cachedIconCount() == 1);
    REQUIRE(panel.cachedThumbnailCount() == 1);
    REQUIRE(store.library().iconLoads() == 1);
    REQUIRE(store.library().archiveOpens() == 1);

    // ⚠ THE PREMISE, CHECKED FIRST -- and it is the whole case. A width that does NOT move the cell
    // size drops no thumbnail, and then "it did not re-decode" would hold on the BROKEN code too:
    // the backstop would be standing in for the primary. So assert the SIZED cache actually went.
    panel.resize(0, 0, 232, 400);
    REQUIRE_MESSAGE(panel.cachedThumbnailCount() == 0,
                    "this width must re-metric the grid, or the case proves nothing");

    // ⚠⚠ THE PRIMARY, AND IT MUST BE `iconLoads`, NOT `cachedIconCount`. The cache SIZE is 1 whether
    // the icon was kept or thrown away and decoded again -- a re-decode refills it. Only an EVENT
    // counter can tell those two apart, and the first cut of this case could not: a mutant that
    // dropped m_icons on every re-metric (the original bug, put straight back) SURVIVED it.
    CHECK(panel.thumbnailFor(wet) != nullptr);
    CHECK(store.library().iconLoads() == 1);
    CHECK(store.library().archiveOpens() == 1);

    // A whole drag, one pixel at a time, over a cell that is on screen the entire way.
    for (int width = 232; width <= 320; ++width) {
        panel.resize(0, 0, width, 400);
        CHECK(panel.thumbnailFor(wet) != nullptr);
    }
    CHECK(store.library().iconLoads() == 1);    // 89 widths, one decode
    CHECK(store.library().archiveOpens() == 1); // 89 widths, one read of the file

    // The theme is the OTHER thing a sized thumbnail is bound to (it is composited over the panel
    // ground). It must drop the thumbnails and, equally, must NOT drop the icons.
    panel.reapplyTheme();
    CHECK(panel.cachedThumbnailCount() == 0);
    CHECK(panel.cachedIconCount() == 1);
    CHECK(panel.thumbnailFor(wet) != nullptr);
    CHECK(store.library().iconLoads() == 1);
    CHECK(store.library().archiveOpens() == 1);
}

TEST_CASE("BrushPresetPanel: a click picks the cell under the cursor; the arrows walk the grid") {
    ui::BrushPresetStore store;
    REQUIRE(store.scanDir(shippedBrushDir()) > 0);

    BrushPresetPanel panel(0, 0, 280, 400);
    panel.setStore(&store);
    ui::PresetGrid* grid = panel.grid();
    REQUIRE(grid != nullptr);

    std::vector<int> picks;
    panel.setOnSelect([&](int index) { picks.push_back(index); });

    // Slot 1 = the first library preset (slot 0 is Default round).
    const ui::PresetCellRect r =
        ui::presetCellRect(1, grid->metrics(), grid->x(), grid->y());
    Fl::e_x = r.x + r.w / 2;
    Fl::e_y = r.y + r.h / 2;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    send(*grid, FL_PUSH);
    // ⚠ A press is not yet a pick: it may still become a drag-scroll of the list. The RELEASE is
    // the click.
    CHECK(picks.empty());
    send(*grid, FL_RELEASE);
    REQUIRE(picks.size() == 1);
    CHECK(picks.back() == grid->slots()[1]);

    // Right walks one cell; Down walks one ROW.
    Fl::e_keysym = FL_Right;
    send(*grid, FL_KEYBOARD);
    CHECK(picks.back() == grid->slots()[2]);
    Fl::e_keysym = FL_Down;
    send(*grid, FL_KEYBOARD);
    CHECK(picks.back() == grid->slots()[2 + grid->metrics().cols]);
    Fl::e_keysym = FL_Home;
    send(*grid, FL_KEYBOARD);
    CHECK(picks.back() == -1); // Home = the Default-round cell
    // The walk is CLAMPED, never wrapped: Left at the first cell stays there.
    Fl::e_keysym = FL_Left;
    send(*grid, FL_KEYBOARD);
    CHECK(panel.selected() == -1);

    // A click in the gutter between two cells picks nothing (rather than the nearest neighbour).
    const std::size_t before = picks.size();
    Fl::e_x = r.x + r.w + 2;
    Fl::e_y = r.y + 2;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    send(*grid, FL_PUSH);
    send(*grid, FL_RELEASE);
    CHECK(picks.size() == before);
}

TEST_CASE("PresetGrid: a DRAG scrolls the list, a moving release FLINGS it, and neither picks") {
    ui::BrushPresetStore store;
    REQUIRE(store.scanDir(shippedBrushDir()) > 0);

    BrushPresetPanel panel(0, 0, 280, 400);
    panel.setStore(&store);
    panel.setTab(ui::PresetTab::All); // the whole corpus, so the list actually overflows
    ui::PresetGrid* grid = panel.grid();
    ui::ScrollView* scroll = panel.scroll();
    REQUIRE(grid != nullptr);
    REQUIRE(scroll != nullptr);
    REQUIRE_MESSAGE(grid->h() > scroll->h(),
                    "the list must overflow the viewport, or the scroll case is vacuous");

    std::vector<int> picks;
    panel.setOnSelect([&](int index) { picks.push_back(index); });

    // ⚠ The clock is PINNED. Two handle() calls are microseconds apart on the real clock, and a
    // velocity assertion cannot stand on that.
    ui::presetUiSetNowForTest(0.0);
    Fl::e_x = grid->x() + 40;
    Fl::e_y = scroll->y() + 300;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    send(*grid, FL_PUSH);
    CHECK(scroll->yposition() == 0);

    ui::presetUiSetNowForTest(0.02);
    Fl::e_y -= 120; // drag UP: the content follows the finger, so the list scrolls DOWN
    send(*grid, FL_DRAG);
    CHECK(scroll->yposition() == 120);
    CHECK(picks.empty());

    ui::presetUiSetNowForTest(0.04);
    send(*grid, FL_RELEASE);
    CHECK(picks.empty()); // ⚠ a drag that MOVED is a scroll, and must not also pick a cell
    REQUIRE(grid->flinging()); // it let go at speed (120 px over 20 ms, clamped to the max)

    const int atRelease = scroll->yposition();
    grid->flingTick(1.0 / 60.0);
    CHECK(scroll->yposition() > atRelease); // the release kept the list moving

    int guard = 0;
    while (grid->flinging() && ++guard < 10000)
        grid->flingTick(1.0 / 60.0);
    CHECK_FALSE(grid->flinging()); // it puts itself down
    const int rest = scroll->yposition();
    CHECK(rest > atRelease);
    CHECK(rest <= grid->h() - scroll->h()); // never past the end

    // ... and a tick after death moves nothing: dead is dead.
    grid->flingTick(1.0 / 60.0);
    CHECK(scroll->yposition() == rest);

    // A drag that STOPS before it lets go is a HOLD: it must release dead -- and still not pick.
    const int held = scroll->yposition();
    ui::presetUiSetNowForTest(10.0);
    Fl::e_x = grid->x() + 40;
    Fl::e_y = scroll->y() + 200;
    send(*grid, FL_PUSH);
    ui::presetUiSetNowForTest(10.02);
    Fl::e_y -= 50;
    send(*grid, FL_DRAG);
    CHECK(scroll->yposition() == held + 50);
    ui::presetUiSetNowForTest(10.50); // half a second of standing still, then the release
    send(*grid, FL_RELEASE);
    CHECK_FALSE(grid->flinging());
    CHECK(picks.empty());

    ui::presetUiSetNowForTest(std::nullopt); // leave the clock as we found it
}

TEST_CASE("BrushPresetStore::indexOfName is how a preset is persisted (never by index)") {
    ui::BrushPresetStore store;
    REQUIRE(store.scanDir(shippedBrushDir()) > 0);
    const int wet = store.indexOfName("i)_Wet_Knife");
    REQUIRE(wet >= 0);
    CHECK(store.presets()[static_cast<std::size_t>(wet)].preset.name == "i)_Wet_Knife");
    CHECK(store.indexOfName("no such preset") == -1);
    CHECK(store.indexOfName("") == -1); // "" is how the settings spell NO preset
}

// ---- rendered pixels -------------------------------------------------------------------------

// ⚠ A widget in an Fl_Scroll that does not erase its WHOLE rect shows the content that scrolled past
// it -- FLTK never clears behind a child. The grid is exactly such a widget, so the guarantee is
// pinned in real pixels: paint the surface magenta, draw the dock over it, and demand that not one
// magenta pixel survives anywhere in the dock's rect.
//
// Set MOSAIC_PANEL_SHOT=/some/dir to also write the frame out as a PNG (that is how the panel's look
// was actually judged; a pane nobody has seen is a pane nobody should ship).
// ---- Cards (docs/brushes.md §8.2) -------------------------------------------------------------

TEST_CASE("a card is a ROW: one per line, the tip on the left and the strip taking the rest") {
    const PresetGridMetrics g = ui::presetGridMetrics(270, ui::PresetDisplayMode::Grid);
    const PresetGridMetrics c = ui::presetGridMetrics(270, ui::PresetDisplayMode::Cards);

    CHECK(c.mode == ui::PresetDisplayMode::Cards);
    CHECK(c.cols == 1);       // a strip only reads as a stroke if it is LONG
    CHECK(c.cellW > g.cellW); // ... so a card takes the whole width, whatever the grid was doing

    const ui::PresetCellRect cell = ui::presetCellRect(0, c, 0, 0);
    const ui::PresetCellRect th = ui::presetThumbRect(cell, c);
    const ui::PresetCellRect sr = ui::presetStrokeRect(cell, c);

    CHECK(sr.w > 0);
    CHECK(sr.h > 0);
    CHECK(sr.x >= th.x + th.w);                     // the strip is to the RIGHT of the tip icon
    CHECK(sr.x + sr.w <= cell.x + cell.w);          // ... and inside the cell
    CHECK(sr.y >= cell.y);
    CHECK(sr.y + sr.h <= cell.y + cell.h);
    CHECK(th.y + th.h <= cell.y + cell.h);
    CHECK(sr.w > th.w); // the strip is the point of the card: it must dominate it

    // ⚠ THE TIP ICON IS THE SAME BOX AS THE STRIP -- same top, same bottom, same height. Two boxes
    // side by side that ALMOST line up read as a mistake, and they did: the icon was centred against
    // the whole cell while the strip hung below the name line.
    CHECK(th.y == sr.y);
    CHECK(th.h == sr.h);
    CHECK(th.y + th.h == sr.y + sr.h);

    // The grid has no strip at all, and asking for one is not an error -- it is an empty rect.
    const ui::PresetCellRect none = ui::presetStrokeRect(ui::presetCellRect(0, g, 0, 0), g);
    CHECK(none.w == 0);
    CHECK(none.h == 0);
}

TEST_CASE("the card's paper follows the theme, and an ERASER gets a slab it can bite") {
    const ui::Palette dark = ui::darkPalette();
    const ui::Palette light = ui::lightPalette();

    // ⚠ Black on white is right in the LIGHT theme and stays there. On a dark UI a white slab per row
    // is a line of lightboxes in a dark dock, and it hurts to look at (the user's words, and the
    // reason this is not theme-independent any more).
    const core::brush::StrokePreviewStyle lightStyle = ui::presetStrokeStyle(light, false);
    CHECK(lightStyle.paper == common::Color8{255, 255, 255, 255});
    CHECK(lightStyle.ink == common::Color8{0, 0, 0, 255});

    const core::brush::StrokePreviewStyle darkStyle = ui::presetStrokeStyle(dark, false);
    CHECK(darkStyle.paper == dark.panelBg); // the card belongs to the dock it sits in
    CHECK(darkStyle.ink == dark.textMuted);
    CHECK(darkStyle.paper != lightStyle.paper); // ... and the two themes really do differ

    // ⚠⚠ AN ERASER'S PAPER IS NEVER THE DOCK'S OWN GROUND. An eraser can only take paper AWAY, and
    // what shows through the hole is the DOCK -- so paper made of the dock's ground would carve a
    // panel-coloured hole in a panel-coloured card. Perfectly invisible, in the theme the user is
    // actually using.
    for (const ui::Palette& pal : {dark, light}) {
        const core::brush::StrokePreviewStyle eraser = ui::presetStrokeStyle(pal, true);
        CHECK(eraser.paper != pal.panelBg);
        CHECK(eraser.paper.a == 255); // and OPAQUE, or there is nothing to bite
    }

    // Every paper is opaque, whoever picked it.
    CHECK(darkStyle.paper.a == 255);
    CHECK(lightStyle.paper.a == 255);
}

TEST_CASE("a re-theme drops the cached strokes -- they were laid in the OLD paper") {
#if defined(__SANITIZE_ADDRESS__) ||                                                               \
    (defined(__has_feature) && __has_feature(address_sanitizer)) // NOLINT
    return;
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return;

    ui::BrushPresetStore store;
    REQUIRE(store.scanDir(shippedBrushDir()) > 0);

    const ui::Palette orig = ui::activePalette();
    ui::applyTheme(ui::darkPalette());

    ui::RightDock dock(0, 0, 280, 760);
    dock.setPresetHeight(300);
    dock.setPresetsVisible(true);
    dock.presets()->setStore(&store);
    dock.presets()->setDisplayMode(ui::PresetDisplayMode::Cards);

    const auto paint = [&] {
        auto* surf = new Fl_Image_Surface(280, 760);
        Fl_Surface_Device::push_current(surf);
        surf->draw(&dock, 0, 0);
        Fl_Surface_Device::pop_current();
        delete surf;
    };

    paint();
    const std::size_t afterDark = dock.presets()->strokeRenders();
    REQUIRE(afterDark > 0);
    paint(); // ... and a repaint alone re-renders NOTHING (the premise: the cache works)
    REQUIRE(dock.presets()->strokeRenders() == afterDark);

    // ⚠ The strokes were laid in the DARK theme's paper and ink. Keeping them across a re-theme would
    // leave dark-paper cards sitting in a light dock until something else happened to evict them.
    ui::applyTheme(ui::lightPalette());
    dock.presets()->reapplyTheme();
    paint();
    CHECK(dock.presets()->strokeRenders() > afterDark);

    ui::applyTheme(orig); // leave the palette exactly as we found it
}

TEST_CASE("the Eraser's Default round CARVES -- it is the tool's own plain nib, not the brush's") {
    ui::BrushPresetStore store;
    REQUIRE(store.scanDir(shippedBrushDir()) > 0);

    BrushPresetPanel panel(0, 0, 280, 400);
    panel.setStore(&store);
    panel.setDisplayMode(ui::PresetDisplayMode::Cards);

    // The Brush's Default round paints...
    panel.setCorpus(ui::PresetCorpus::Brush);
    const core::brush::BrushParams* brush = panel.paramsForTest(-1);
    REQUIRE(brush != nullptr);
    CHECK(brush->strokeMode == core::brush::StrokeMode::Paint);

    // ... and the ERASER's Default round carves, because that is what the Eraser does when it holds no
    // preset. A card previewing it as a stroke of paint would be advertising the wrong tool.
    panel.setCorpus(ui::PresetCorpus::Eraser);
    const core::brush::BrushParams* eraser = panel.paramsForTest(-1);
    REQUIRE(eraser != nullptr);
    CHECK(eraser->strokeMode == core::brush::StrokeMode::Erase);

    // ⚠ Index -1 is the ONE cache key whose MEANING depends on the corpus, and both caches are keyed
    // by index alone. Switching back must hand back the BRUSH's again, not the eraser's leftovers.
    panel.setCorpus(ui::PresetCorpus::Brush);
    CHECK(panel.paramsForTest(-1)->strokeMode == core::brush::StrokeMode::Paint);
}

TEST_CASE("the strip's render width is bucketed UP -- the contract that keeps a drag cheap") {
    // ⚠ A stroke preview costs ~1.7 ms through the real engine. Rendering at the strip's exact width
    // would re-render every visible card on every frame of a width drag. The rendered strip must
    // therefore always COVER the strip it is cropped into (>=), and must change only once per bucket.
    for (int w = 1; w <= 600; ++w) {
        const int r = ui::presetStrokeRenderWidth(w);
        CHECK(r >= w);       // it covers the strip: a crop can never run off the end of it
        CHECK(r % 32 == 0);  // ... and it lands on a bucket
        CHECK(r - w < 32);   // ... the NEXT one up, not some distant one
    }
    CHECK(ui::presetStrokeRenderWidth(0) == 0);
    CHECK(ui::presetStrokeRenderWidth(32) == 32); // exactly on a bucket stays on it
    CHECK(ui::presetStrokeRenderWidth(33) == 64);
}

TEST_CASE("the display mode round-trips by NAME, and an unknown key lands on the DEFAULT") {
    CHECK(std::string(ui::presetDisplayModeKey(ui::PresetDisplayMode::Cards)) == "cards");
    CHECK(std::string(ui::presetDisplayModeKey(ui::PresetDisplayMode::Grid)) == "grid");
    CHECK(ui::presetDisplayModeFromKey("cards") == ui::PresetDisplayMode::Cards);
    CHECK(ui::presetDisplayModeFromKey("grid") == ui::PresetDisplayMode::Grid);
    // ⚠ Cards is the DEFAULT, so a settings file written before this field existed -- which reads
    // back as the empty string -- must land on Cards, not on Grid.
    CHECK(ui::presetDisplayModeFromKey("") == ui::PresetDisplayMode::Cards);
    CHECK(ui::presetDisplayModeFromKey("nonsense") == ui::PresetDisplayMode::Cards);
}

// ⚠⚠ THE PERF CONTRACT, AND IT IS ASSERTED ON AN EVENT COUNT, NOT A CACHE SIZE.
//
// A stroke costs ~1.7 ms -- a thousand times a thumbnail's box filter. The dock was JUST dug out of a
// bug where a width drag re-read a 17.5 MB archive a dozen times a frame; a card mode that re-rendered
// the engine on every pixel of that same drag would be the same bug, only more expensive.
//
// The instrument matters as much as the claim. `strokeRenders()` counts EVENTS: monotonic, never
// reset. A CACHE SIZE COULD NOT SEE THIS -- a re-render refills the cache to exactly the size it had,
// so a mutant that threw the cache away every frame would leave a size assertion completely green.
// (Not hypothetical: that is precisely what happened to the icon cache's first test, and the mutant
// survived it.)
TEST_CASE("dragging the dock's width does not re-render one stroke") {
#if defined(__SANITIZE_ADDRESS__) ||                                                               \
    (defined(__has_feature) && __has_feature(address_sanitizer)) // NOLINT
    return;
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return;

    ui::BrushPresetStore store;
    REQUIRE(store.scanDir(shippedBrushDir()) > 0);

    constexpr int kH = 760;
    ui::RightDock dock(0, 0, 280, kH);
    dock.setPresetHeight(300);
    dock.setPresetsVisible(true);
    dock.presets()->setStore(&store);
    dock.presets()->setDisplayMode(ui::PresetDisplayMode::Cards);

    const auto paint = [&](int width) {
        dock.resize(0, 0, width, kH);
        auto* surf = new Fl_Image_Surface(width, kH);
        Fl_Surface_Device::push_current(surf);
        surf->draw(&dock, 0, 0);
        Fl_Surface_Device::pop_current();
        delete surf;
    };

    // The width the strip is CURRENTLY rendered at, straight off the grid's own metrics -- never
    // re-derived here, or the case would be measuring a second copy of the layout rule.
    const auto renderWidth = [&] {
        const ui::PresetGridMetrics& m = dock.presets()->grid()->metrics();
        const ui::PresetCellRect cell = ui::presetCellRect(0, m, 0, 0);
        return ui::presetStrokeRenderWidth(ui::presetStrokeRect(cell, m).w);
    };

    paint(280);
    const std::size_t afterFirst = dock.presets()->strokeRenders();
    CHECK(afterFirst > 0);
    // LAZY: only the handful of cards actually on screen ever rendered -- not the whole corpus.
    CHECK(afterFirst < static_cast<std::size_t>(store.presets().size()) / 2);

    // ⚠ A WHOLE DRAG, ONE PIXEL AT A TIME -- and the contract asserted at every step of it, rather
    // than over some hand-picked range that happens to sit inside a bucket. (The first cut of this
    // case DID hand-pick one, and it broke the moment the card's geometry was retuned: the range it
    // had been told was "inside a bucket" quietly stopped being inside one, and the failure looked
    // like a caching bug rather than a stale premise.)
    //
    // The rule is exactly this: a stroke is re-laid ONLY when the bucketed render width MOVES.
    int prev = renderWidth();
    std::size_t prevRenders = dock.presets()->strokeRenders();
    int quietFrames = 0;
    for (int w = 281; w <= 460; ++w) {
        paint(w);
        const int now = renderWidth();
        const std::size_t renders = dock.presets()->strokeRenders();
        if (now == prev) {
            CHECK(renders == prevRenders); // the bucket did not move, so nothing was re-laid
            ++quietFrames;
        }
        prev = now;
        prevRenders = renders;
    }
    // ⚠ THE PREMISE, CHECKED. If every frame of that drag had moved the bucket, the loop above would
    // have asserted nothing at all while passing -- a backstop standing in for the primary, again.
    CHECK(quietFrames > 120);

    // ... and over the whole 180 px drag it DID re-render some: a cache that never missed would mean
    // the strip had stopped tracking the dock at all, which is a different bug wearing this test as a
    // disguise. What it must not be is one render per pixel.
    const std::size_t total = dock.presets()->strokeRenders();
    CHECK(total > afterFirst);
    CHECK(total < afterFirst + 180); // one per bucket, not one per pixel
}

TEST_CASE("the GRID mode renders no strokes at all -- nobody pays for a strip they cannot see") {
#if defined(__SANITIZE_ADDRESS__) ||                                                               \
    (defined(__has_feature) && __has_feature(address_sanitizer)) // NOLINT
    return;
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return;

    ui::BrushPresetStore store;
    REQUIRE(store.scanDir(shippedBrushDir()) > 0);

    ui::RightDock dock(0, 0, 280, 760);
    dock.setPresetHeight(300);
    dock.setPresetsVisible(true);
    dock.presets()->setStore(&store);
    dock.presets()->setDisplayMode(ui::PresetDisplayMode::Grid);

    auto* surf = new Fl_Image_Surface(280, 760);
    Fl_Surface_Device::push_current(surf);
    surf->draw(&dock, 0, 0);
    Fl_Surface_Device::pop_current();
    delete surf;

    CHECK(dock.presets()->strokeRenders() == 0);
    CHECK(dock.presets()->cachedThumbnailCount() > 0); // ... but it did draw its icons
}

TEST_CASE("the dock paints every pixel it owns (and can be looked at)") {
#if defined(__SANITIZE_ADDRESS__) ||                                                               \
    (defined(__has_feature) && __has_feature(address_sanitizer)) // NOLINT
    return; // FLTK/X11 internals leak on teardown under LeakSanitizer; this is not a memory test
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return;

    ui::BrushPresetStore store;
    REQUIRE(store.scanDir(shippedBrushDir()) > 0);

    constexpr int kW = 280;
    constexpr int kH = 760;
    ui::RightDock dock(0, 0, kW, kH);
    dock.setPresetHeight(300);
    dock.setPresetsVisible(true);
    dock.presets()->setStore(&store);
    dock.presets()->setDisplayMode(ui::PresetDisplayMode::Cards);
    dock.presets()->setSelected(store.indexOfName("i)_Wet_Knife"));

    auto* surf = new Fl_Image_Surface(kW, kH);
    Fl_Surface_Device::push_current(surf);
    fl_color(fl_rgb_color(255, 0, 255)); // the stale content the dock sits on
    fl_rectf(0, 0, kW, kH);
    surf->draw(&dock, 0, 0);
    Fl_Surface_Device::pop_current();

    Fl_RGB_Image* img = surf->image();
    REQUIRE(img != nullptr);
    const auto* px = reinterpret_cast<const unsigned char*>(img->data()[0]);
    const int d = img->d();

    common::Image shot(kW, kH);
    int stale = 0;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const unsigned char* p = px + (static_cast<std::size_t>(y) * kW + x) * d;
            if (p[0] > 250 && p[1] < 5 && p[2] > 250)
                ++stale;
            std::uint8_t* q = &shot.rgba[((static_cast<std::size_t>(y) * kW) + x) * 4];
            q[0] = p[0];
            q[1] = p[1];
            q[2] = p[2];
            q[3] = 255;
        }
    }
    CHECK(stale == 0);
    // The grid decoded only what it PAINTED -- a screenful, not the whole library.
    CHECK(dock.presets()->cachedThumbnailCount() > 0);
    CHECK(dock.presets()->cachedThumbnailCount() < static_cast<std::size_t>(store.presets().size()));

    if (const char* dir = std::getenv("MOSAIC_PANEL_SHOT"); dir != nullptr) {
        std::string err;
        if (!io::savePng(shot, std::string(dir) + "/dock.png", {}, &err))
            MESSAGE("savePng: " << err);

        // ... and the top of the grid, unselected, in the LIGHT palette: the Default-round cell (the
        // no-preset one) only shows at the top, and the light theme is where a hand-picked ink can
        // quietly stop reading.
        const ui::Palette dark = ui::activePalette();
        ui::applyTheme(ui::lightPalette());
        ui::RightDock light(0, 0, kW, kH);
        light.setPresetHeight(300);
        light.setPresetsVisible(true);
        light.presets()->setStore(&store);
        auto* lsurf = new Fl_Image_Surface(kW, kH);
        Fl_Surface_Device::push_current(lsurf);
        lsurf->draw(&light, 0, 0);
        Fl_Surface_Device::pop_current();
        Fl_RGB_Image* limg = lsurf->image();
        const auto* lpx = reinterpret_cast<const unsigned char*>(limg->data()[0]);
        const int ld = limg->d();
        common::Image lshot(kW, kH);
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) {
                const unsigned char* p = lpx + (static_cast<std::size_t>(y) * kW + x) * ld;
                std::uint8_t* q = &lshot.rgba[((static_cast<std::size_t>(y) * kW) + x) * 4];
                q[0] = p[0];
                q[1] = p[1];
                q[2] = p[2];
                q[3] = 255;
            }
        if (!io::savePng(lshot, std::string(dir) + "/dock-light.png", {}, &err))
            MESSAGE("savePng: " << err);
        delete limg;
        delete lsurf;
        ui::applyTheme(dark); // leave the palette exactly as we found it
    }
    delete img;
    delete surf;
}

// ⚠ Hover on a CARD is an OUTLINE, not a fill. A card is a full-width row, and a row that turned
// lighter under the cursor was a slab of light sweeping the dock -- the user's word was "busy".
// The dense grid keeps its fill (a small tile tinting is a highlight, not a slab), so this pins
// the Cards mode specifically: the ring on the edge, and the dock's own ground just inside it.
TEST_CASE("Cards hover is an OUTLINE -- the card must not turn into a lighter slab") {
#if defined(__SANITIZE_ADDRESS__) ||                                                               \
    (defined(__has_feature) && __has_feature(address_sanitizer)) // NOLINT
    return; // FLTK/X11 internals leak on teardown under LeakSanitizer; this is not a memory test
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return;

    ui::BrushPresetStore store;
    REQUIRE(store.scanDir(shippedBrushDir()) > 0);

    constexpr int kW = 280;
    constexpr int kH = 400;
    BrushPresetPanel panel(0, 0, kW, kH);
    panel.setStore(&store); // Cards is the default mode, but the premise is checked below
    ui::PresetGrid* grid = panel.grid();
    REQUIRE(grid != nullptr);
    REQUIRE(grid->metrics().mode == ui::PresetDisplayMode::Cards);

    // Hover slot 1 (slot 0 is the SELECTED Default-round cell, which wears its own chrome) through
    // the real path: FL_MOVE is what sets the hover.
    const ui::PresetCellRect r = ui::presetCellRect(1, grid->metrics(), grid->x(), grid->y());
    Fl::e_x = r.x + 10;
    Fl::e_y = r.y + 10;
    send(*grid, FL_MOVE);

    auto* surf = new Fl_Image_Surface(kW, kH);
    Fl_Surface_Device::push_current(surf);
    surf->draw(&panel, 0, 0);
    Fl_Surface_Device::pop_current();
    Fl_RGB_Image* img = surf->image();
    REQUIRE(img != nullptr);
    const auto* px = reinterpret_cast<const unsigned char*>(img->data()[0]);
    const int d = img->d();
    const auto at = [&](int X, int Y) {
        return px + (static_cast<std::size_t>(Y) * kW + X) * d;
    };

    const ui::Palette& pal = ui::activePalette();
    // The card's own corner wears the hover ring, and the ring is 2 px WIDE (the user's call: one
    // was too faint)...
    const unsigned char* edge = at(r.x, r.y);
    CHECK(edge[0] == pal.controlActive.r);
    CHECK(edge[1] == pal.controlActive.g);
    CHECK(edge[2] == pal.controlActive.b);
    const unsigned char* ring2 = at(r.x + 1, r.y + 1);
    CHECK(ring2[0] == pal.controlActive.r);
    CHECK(ring2[1] == pal.controlActive.g);
    CHECK(ring2[2] == pal.controlActive.b);
    // ... and the ground just inside it stays the DOCK's ground. This is the pixel the old hover
    // FILL painted controlHover, and it is the whole point of the change.
    const unsigned char* inside = at(r.x + 2, r.y + 2);
    CHECK(inside[0] == pal.panelBg.r);
    CHECK(inside[1] == pal.panelBg.g);
    CHECK(inside[2] == pal.panelBg.b);

    // The SELECTED cell (slot 0, the Default-round the panel starts on) is a ring too now: 2 px of
    // accent, and NO controlActive slab under the row -- the same slab the hover fill was thrown
    // out for (the user's call, both times).
    REQUIRE(panel.selected() == -1); // the premise: slot 0 IS the selected cell
    REQUIRE(grid->slots().front() == -1);
    const ui::PresetCellRect s = ui::presetCellRect(0, grid->metrics(), grid->x(), grid->y());
    const unsigned char* sEdge = at(s.x, s.y);
    CHECK(sEdge[0] == pal.accent.r);
    CHECK(sEdge[1] == pal.accent.g);
    CHECK(sEdge[2] == pal.accent.b);
    const unsigned char* sRing2 = at(s.x + 1, s.y + 1);
    CHECK(sRing2[0] == pal.accent.r);
    CHECK(sRing2[1] == pal.accent.g);
    CHECK(sRing2[2] == pal.accent.b);
    const unsigned char* sInside = at(s.x + 2, s.y + 2);
    CHECK(sInside[0] == pal.panelBg.r);
    CHECK(sInside[1] == pal.panelBg.g);
    CHECK(sInside[2] == pal.panelBg.b);

    delete img;
    delete surf;
}

// ⚠ THE FIDELITY BADGE ATE THE THUMBNAIL'S FRAME, and the user saw it before any test did ("the
// compatibility circle eats the outline"). The badge is an OPAQUE 13x13 blit whose ground is the
// thumbnail's own PIXELS -- which know nothing about a hairline painted OVER them -- and its patch
// reaches the thumbnail's last row and column, which is exactly where that hairline runs. Drawn
// after the frame, it re-laid the icon across the corner and bit ~12 px out of the right and bottom
// edges of every non-Exact preset's frame.
//
// The frame is `fl_rect`: one solid, un-anti-aliased ink. So the honest assertion is EQUALITY on
// every pixel of the outline, and a single icon pixel surviving anywhere on it fails.
//
// ⚠ The badge only exists on a NON-EXACT preset, so a test that picks any cell can pass on a cell
// that has no badge to eat anything. Pick the badged cell deliberately, and REQUIRE that we found one.
TEST_CASE("the fidelity badge does not eat the thumbnail's frame") {
#if defined(__SANITIZE_ADDRESS__) ||                                                               \
    (defined(__has_feature) && __has_feature(address_sanitizer)) // NOLINT
    return; // FLTK/X11 internals leak on teardown under LeakSanitizer; this is not a memory test
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return;

    ui::BrushPresetStore store;
    REQUIRE(store.scanDir(shippedBrushDir()) > 0);

    constexpr int kW = 280;
    constexpr int kH = 760;
    ui::RightDock dock(0, 0, kW, kH);
    dock.setPresetHeight(300);
    dock.setPresetsVisible(true);
    dock.presets()->setStore(&store);
    // No selection: the grid stays scrolled to the top, so the cells we measure are the ones drawn.
    // ⚠ AND THE TAB IS CHOSEN FOR ITS BADGES, not left on the default. The dock opens on Basics, and
    // Basics used to contain a badged cell only because `b)_Airbrush_Soft` was badged for a stale
    // `SmudgeRate` key it shares with four `l)_Adjust_*` presets -- a badge that should never have
    // fired and no longer does (§6.6i). With 7 badged presets left in 117, all of them under `v`,
    // `x`, `y` and `z`, this case has to go where they are or it measures nothing and REQUIREs its
    // way out at the bottom.
    dock.presets()->setTab(ui::PresetTab::Texture); // y) + z): four of the seven live here

    ui::PresetGrid* grid = dock.presets()->grid();
    REQUIRE(grid != nullptr);
    ui::ScrollView* scroll = dock.presets()->scroll();
    REQUIRE(scroll != nullptr);

    auto* surf = new Fl_Image_Surface(kW, kH);
    Fl_Surface_Device::push_current(surf);
    surf->draw(&dock, 0, 0);
    Fl_Surface_Device::pop_current();
    Fl_RGB_Image* img = surf->image();
    REQUIRE(img != nullptr);
    const auto* px = reinterpret_cast<const unsigned char*>(img->data()[0]);
    const int d = img->d();
    const auto at = [&](int x, int y) -> common::Color8 {
        const unsigned char* p = px + (static_cast<std::size_t>(y) * kW + x) * d;
        return {p[0], p[1], p[2], 255};
    };

    const ui::Palette& pal = ui::activePalette();
    const ui::PresetGridMetrics& m = grid->metrics();

    // The first BADGED cell that is drawn whole inside the scroll's viewport.
    int checked = 0;
    for (int slot = 0; slot < static_cast<int>(grid->slots().size()); ++slot) {
        const int index = grid->slots()[static_cast<std::size_t>(slot)];
        if (index < 0)
            continue; // the Default-round cell carries no badge
        const io::brush::LibraryPreset* p = dock.presets()->presetAt(index);
        if (p == nullptr || p->preset.provenance.fidelity == io::brush::PresetFidelity::Exact)
            continue; // ... and neither does an Exact one

        const ui::PresetCellRect cell = ui::presetCellRect(slot, m, grid->x(), grid->y());
        const ui::PresetCellRect th = ui::presetThumbRect(cell, m);
        if (th.y < scroll->y() || th.y + th.h > scroll->y() + scroll->h())
            continue; // clipped by the viewport: those pixels are not this cell's to own
        if (++checked > 3)
            break;

        // Walk the frame. The bug lived in the last ~12 px of the right and bottom edges, so the
        // corners are the primary check and the far edges are the backstop -- walk all four whole.
        int broken = 0;
        for (int x = th.x; x < th.x + th.w; ++x) {
            broken += static_cast<int>(at(x, th.y) != pal.border);              // top
            broken += static_cast<int>(at(x, th.y + th.h - 1) != pal.border);   // bottom
        }
        for (int y = th.y; y < th.y + th.h; ++y) {
            broken += static_cast<int>(at(th.x, y) != pal.border);              // left
            broken += static_cast<int>(at(th.x + th.w - 1, y) != pal.border);   // right
        }
        INFO("preset " << dock.presets()->nameAt(index) << " frame " << th.x << "," << th.y << " "
                       << th.w << "x" << th.h);
        CHECK(broken == 0);
    }
    REQUIRE(checked > 0); // a test that measured no badged cell measured nothing

    delete img;
    delete surf;
}

TEST_CASE("Settings carry the dock section's height and the selected preset, by name") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "mosaic-preset-settings-test.json";
    std::filesystem::remove(path);

    common::Settings s;
    CHECK(s.brushPresetHeight == 260);
    CHECK(s.brushPreset.empty()); // no preset = the engine's round tip, which is where the Brush starts
    s.brushPresetHeight = 315;
    s.brushPreset = "i)_Wet_Knife";
    std::string err;
    REQUIRE(common::saveSettings(s, path, &err));

    const common::Settings back = common::loadSettings(path, &err);
    CHECK(back.brushPresetHeight == 315);
    CHECK(back.brushPreset == "i)_Wet_Knife");
    std::filesystem::remove(path);
}

TEST_CASE("Settings carry the dock's display mode, and it DEFAULTS to cards") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "mosaic-preset-display-test.json";
    std::filesystem::remove(path);

    common::Settings s;
    // The user asked for cards as the default: a grid of tip icons answers a question nobody has.
    CHECK(s.brushPresetDisplay == "cards");
    CHECK(ui::presetDisplayModeFromKey(s.brushPresetDisplay) == ui::PresetDisplayMode::Cards);

    s.brushPresetDisplay = "grid";
    std::string err;
    REQUIRE(common::saveSettings(s, path, &err));
    const common::Settings back = common::loadSettings(path, &err);
    CHECK(back.brushPresetDisplay == "grid");
    CHECK(ui::presetDisplayModeFromKey(back.brushPresetDisplay) == ui::PresetDisplayMode::Grid);

    // A settings file written BEFORE this field existed has no key at all -- and must open on the
    // default, not on whatever an enum's zero happens to be.
    std::filesystem::remove(path);
    REQUIRE(common::saveSettings(common::Settings{}, path, &err));
    std::string json;
    {
        std::ifstream in(path);
        json.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string key = "\"brushPresetDisplay\"";
    REQUIRE(json.find(key) != std::string::npos); // it is written...
    {
        std::ofstream out(path);
        std::string stripped = json;
        const std::size_t at = stripped.find(key);
        const std::size_t end = stripped.find(',', at);
        stripped.erase(at, end - at + 1); // ... and here is a file that predates it
        out << stripped;
    }
    const common::Settings old = common::loadSettings(path, &err);
    CHECK(old.brushPresetDisplay == "cards");
    std::filesystem::remove(path);
}

// ---- The taxonomy, the tabs and the two corpora ------------------------------------------------

TEST_CASE("presetGroupOf: the letter prefix IS the partition") {
    CHECK(ui::presetGroupOf("a)_Eraser_Circle") == ui::PresetGroup::Erasers);
    CHECK(ui::presetGroupOf("b)_Basic-1") == ui::PresetGroup::Basics);
    CHECK(ui::presetGroupOf("c)_Pencil-2") == ui::PresetGroup::Draw);
    CHECK(ui::presetGroupOf("d)_Ink-3_Gpen") == ui::PresetGroup::Draw);
    CHECK(ui::presetGroupOf("e)_Marker_Dry") == ui::PresetGroup::Draw);
    CHECK(ui::presetGroupOf("f)_Bristles-5_Flat") == ui::PresetGroup::Paint);
    CHECK(ui::presetGroupOf("h)_Chalk_Soft") == ui::PresetGroup::Paint);
    CHECK(ui::presetGroupOf("j)_Watercolor_Texture") == ui::PresetGroup::Paint);
    CHECK(ui::presetGroupOf("i)_Wet_Knife") == ui::PresetGroup::Blend);
    CHECK(ui::presetGroupOf("k)_Blender_Smear") == ui::PresetGroup::Blend);
    CHECK(ui::presetGroupOf("l)_Adjust_Dodge") == ui::PresetGroup::Effects);
    CHECK(ui::presetGroupOf("x)_Filter_Blur") == ui::PresetGroup::Effects);
    CHECK(ui::presetGroupOf("t)_Shapes_Square") == ui::PresetGroup::Special);
    CHECK(ui::presetGroupOf("v)_Clone_Tool") == ui::PresetGroup::Special);
    CHECK(ui::presetGroupOf("y)_Screentones_Regular") == ui::PresetGroup::Texture);
    CHECK(ui::presetGroupOf("z)_Stamp_Grass") == ui::PresetGroup::Texture);

    // Anything not wearing the convention is somebody's own brush.
    CHECK(ui::presetGroupOf("My Brush") == ui::PresetGroup::Other);
    CHECK(ui::presetGroupOf("m)_Unused") == ui::PresetGroup::Other); // m)..s) are unused upstream
    CHECK(ui::presetGroupOf("a") == ui::PresetGroup::Other);         // too short to carry a prefix
    CHECK(ui::presetGroupOf("") == ui::PresetGroup::Other);
    CHECK(ui::presetGroupOf("a)Eraser") == ui::PresetGroup::Other);  // no underscore: not the form
}

TEST_CASE("EVERY shipped preset lands in exactly ONE tab -- the partition is the whole point") {
    // ⚠ THE LOAD-BEARING CASE. A tab set that is not a partition is a filing system that loses
    // things: a preset in no tab cannot be found, and one in three tabs answers to none of them.
    // This is also why the bundle's own 9 TAGS are not used -- they overlap 49-deep and strand 3.
    ui::BrushPresetStore store;
    REQUIRE(store.scanDir(shippedBrushDir()) > 0);

    constexpr ui::PresetTab kMediaTabs[] = {ui::PresetTab::Basics,  ui::PresetTab::Draw,
                                            ui::PresetTab::Paint,   ui::PresetTab::Blend,
                                            ui::PresetTab::Texture, ui::PresetTab::Effects,
                                            ui::PresetTab::Special};
    for (const std::string& name : store.names()) {
        const ui::PresetGroup g = ui::presetGroupOf(name);
        int homes = 0;
        for (const ui::PresetTab tab : kMediaTabs)
            if (ui::presetTabAdmits(tab, g, /*userInstalled=*/false))
                ++homes;
        CHECK_MESSAGE(homes == 1, name << " lands in " << homes << " media tabs, not 1");
        CHECK(ui::presetTabAdmits(ui::PresetTab::All, g, false)); // ... and All admits everything
    }
}

TEST_CASE("visiblePresetTabs: an empty tab is not shown, and the User tab waits to be earned") {
    // Nothing installed -> no User tab. This is the rule the user asked for by name.
    const std::vector<ui::PresetGroup> shipped = {ui::PresetGroup::Basics, ui::PresetGroup::Draw,
                                                  ui::PresetGroup::Draw};
    const std::vector<bool> noneMine = {false, false, false};
    const std::vector<ui::PresetTabCounts> tabs = ui::visiblePresetTabs(shipped, noneMine);

    std::vector<ui::PresetTab> got;
    for (const ui::PresetTabCounts& t : tabs)
        got.push_back(t.tab);
    CHECK(got == std::vector<ui::PresetTab>{ui::PresetTab::All, ui::PresetTab::Basics,
                                            ui::PresetTab::Draw});
    CHECK(tabs[0].count == 3); // All
    CHECK(tabs[1].count == 1); // Basics
    CHECK(tabs[2].count == 2); // Draw
    // Paint, Blend, Texture, Effects, Special and User are all absent: nothing is in them.

    // Install one, and the User tab appears -- WITHOUT vanishing from its medium. A brush of yours
    // that could only be found under "User" would make "All" a lie.
    const std::vector<bool> oneMine = {false, false, true};
    const std::vector<ui::PresetTabCounts> withUser = ui::visiblePresetTabs(shipped, oneMine);
    REQUIRE(withUser.back().tab == ui::PresetTab::User);
    CHECK(withUser.back().count == 1);
    for (const ui::PresetTabCounts& t : withUser)
        if (t.tab == ui::PresetTab::Draw)
            CHECK(t.count == 2); // still both, the user's included
}

TEST_CASE("BrushPresetPanel: the Brush is offered no eraser, and the Eraser nothing else") {
    // The user's ruling: erasers move to the Eraser tool. The split is SEMANTIC (CompositeOp=erase ->
    // eraserMode), not the `a)_` prefix -- so a preset that erases is filed with the erasers wherever
    // its name happens to sort.
    ui::BrushPresetStore store;
    const int n = store.scanDir(shippedBrushDir());
    REQUIRE(n > 0);

    int erasers = 0;
    for (const io::brush::LibraryPreset& p : store.presets())
        if (p.preset.eraserMode)
            ++erasers;
    REQUIRE_MESSAGE(erasers > 0, "the shipped bundle must carry erasers or this case proves nothing");

    BrushPresetPanel panel(0, 0, 280, 400);
    panel.setStore(&store);
    panel.setTab(ui::PresetTab::All); // the dock opens on Basics; this case is about the whole corpus
    REQUIRE(panel.corpus() == ui::PresetCorpus::Brush);
    CHECK(panel.matchCount() == n - erasers);
    for (const int index : panel.grid()->slots()) {
        if (index < 0)
            continue; // the Default-round cell is a BRUSH
        CHECK_FALSE(store.presets()[static_cast<std::size_t>(index)].preset.eraserMode);
    }

    panel.setCorpus(ui::PresetCorpus::Eraser);
    CHECK(panel.matchCount() == erasers);
    for (const int index : panel.grid()->slots()) {
        if (index < 0)
            continue; // ⚠ the Eraser has a Default-round cell too now (the user's call): its own
                      // plain round nib, which is what it carves with when it holds no preset
        CHECK(store.presets()[static_cast<std::size_t>(index)].preset.eraserMode);
    }

    // The two corpora PARTITION the library: nothing is in both, nothing is in neither.
    CHECK((n - erasers) + erasers == n);
}

TEST_CASE("BrushPresetPanel: a tab filters the grid, and the search filters WITHIN it") {
    ui::BrushPresetStore store;
    REQUIRE(store.scanDir(shippedBrushDir()) > 0);

    BrushPresetPanel panel(0, 0, 280, 400);
    panel.setStore(&store);
    CHECK(panel.tab() == ui::PresetTab::Basics); // ⚠ the dock OPENS here (the user's call)
    panel.setTab(ui::PresetTab::All);
    const int all = panel.matchCount();

    panel.setTab(ui::PresetTab::Draw);
    const int draw = panel.matchCount();
    CHECK(draw > 0);
    CHECK(draw < all);
    for (const int index : panel.grid()->slots()) {
        REQUIRE(index >= 0); // "Default round" belongs to no OTHER medium: All and Basics only
        CHECK(ui::presetGroupOf(panel.nameAt(index)) == ui::PresetGroup::Draw);
    }

    // The search narrows the TAB, it does not escape it: a pencil is in Draw, a knife is not.
    panel.setFilter("pencil");
    CHECK(panel.matchCount() > 0);
    CHECK(panel.matchCount() < draw);
    panel.setFilter("knife"); // i)_Wet_Knife is a BLEND preset
    CHECK(panel.matchCount() == 0);

    panel.setFilter("");
    panel.setTab(ui::PresetTab::All);
    CHECK(panel.matchCount() == all);
}

// ⚠⚠ THE CARD'S PAPER AND INK FOLLOW THE THEME NOW, AND THAT MOVES WHICH PRESETS GO BLIND.
//
// In the LIGHT theme the ink is black -- darker than the paper in every channel -- so `Lighten`,
// `ColorDodge`, `Color`, `Overlay` and `Screen` are the identity, and FIVE presets cannot mark it.
// In the DARK theme the ink is the muted text colour, which is BRIGHTER than the panel ground in
// every channel -- so the blind set would flip to `Darken` / `Multiply` / `Burn`, and it turns out
// **the shipped corpus has none of those**: measured, NOTHING is blind in the dark theme.
//
// ⚠ That asymmetry is exactly why the fallback triggers on the RESULT and not on a list. No list was
// written down, so none of this had to be predicted -- and this case is what turned "the five" from a
// fact about one palette into a fact that is checked against every palette we ship.
TEST_CASE("every preset previews in BOTH themes -- the blind set moves, and the fallback follows") {
    ui::BrushPresetStore store;
    REQUIRE(store.scanDir(shippedBrushDir()) > 0);

    constexpr int kW = 196;
    constexpr int kH = 58;

    int fellBackAnywhere = 0;
    for (const ui::Palette& pal : {ui::darkPalette(), ui::lightPalette()}) {
        int fellBack = 0;
        std::vector<std::string> blank;

        for (int i = 0; i < static_cast<int>(store.presets().size()); ++i) {
            const io::brush::LibraryPreset& lp = store.presets()[static_cast<std::size_t>(i)];
            const bool eraser = lp.preset.eraserMode;
            const core::brush::StrokePreviewStyle style = ui::presetStrokeStyle(pal, eraser);

            core::brush::BrushParams p = io::brush::presetBrushParams(lp);
            const common::Image img = core::brush::renderStrokePreview(p, kW, kH, style);

            // ⚠ The paper the stroke was ACTUALLY laid on is the image's own corner, never the style's
            // -- the renderer may have fallen back to the grey, and a case that assumed otherwise
            // would count a perfectly good card as blank.
            const common::Color8 paper{img.rgba[0], img.rgba[1], img.rgba[2], img.rgba[3]};
            if (paper != style.paper)
                ++fellBack;

            int marked = 0;
            for (std::size_t q = 0; q + 3 < img.rgba.size(); q += 4)
                marked += static_cast<int>(img.rgba[q] != paper.r || img.rgba[q + 1] != paper.g ||
                                           img.rgba[q + 2] != paper.b || img.rgba[q + 3] != paper.a);
            if (marked < 20)
                blank.push_back(lp.preset.name);
        }

        INFO("palette dark=" << pal.dark << " fellBack=" << fellBack << " blank=" << [&] {
            std::string s;
            for (const std::string& n : blank)
                s += n + " ";
            return s;
        }());
        CHECK(blank.empty());
        CHECK(fellBack < 20); // a handful at most, never the library
        fellBackAnywhere += fellBack;
    }

    // ⚠ AND THE FALLBACK IS NOT DEAD CODE. Without this, the case above would pass just as happily on
    // a build with the fallback deleted -- it would simply be reporting that nothing was ever blind.
    // Something IS blind (the five, in the light theme), and that is what makes the rest of it mean
    // anything.
    CHECK(fellBackAnywhere == 5);
}

TEST_CASE("the dock opens on Basics, with Default round first in it") {
    ui::BrushPresetStore store;
    REQUIRE(store.scanDir(shippedBrushDir()) > 0);

    BrushPresetPanel panel(0, 0, 280, 400);
    panel.setStore(&store);

    // 114 cards is not a place to start; the six Basic presets and the plain round nib are.
    CHECK(panel.tab() == ui::PresetTab::Basics);

    // ⚠ AND THE DEFAULT-ROUND CELL IS IN IT, FIRST. The plain round nib is the most basic thing in the
    // library, and Basics is the tab the dock opens on -- a Basics tab without it would open on a page
    // that cannot get you back to plain.
    const std::vector<int>& slots = panel.grid()->slots();
    REQUIRE_FALSE(slots.empty());
    CHECK(slots.front() == -1);
    CHECK(std::count(slots.begin(), slots.end(), -1) == 1);
    for (std::size_t i = 1; i < slots.size(); ++i)
        CHECK(ui::presetGroupOf(panel.nameAt(slots[i])) == ui::PresetGroup::Basics);

    // ... and it is FIRST on All too, where it has always been.
    panel.setTab(ui::PresetTab::All);
    REQUIRE_FALSE(panel.grid()->slots().empty());
    CHECK(panel.grid()->slots().front() == -1);

    // ... and on no OTHER medium tab: repeating it in seven places is noise in seven places.
    for (const ui::PresetTab t : {ui::PresetTab::Draw, ui::PresetTab::Paint, ui::PresetTab::Blend,
                                  ui::PresetTab::Texture, ui::PresetTab::Effects}) {
        panel.setTab(t);
        for (const int index : panel.grid()->slots())
            CHECK(index >= 0);
    }

    // ⚠ The ERASER corpus has no Basics tab at all, so the dock's opening tab cannot exist there --
    // and it must fall back to All rather than opening on an empty grid.
    panel.setCorpus(ui::PresetCorpus::Eraser);
    CHECK(panel.tab() == ui::PresetTab::All);
    CHECK(panel.matchCount() > 0);

    // ⚠ AND THE ERASER GETS A DEFAULT-ROUND CELL TOO, FIRST (the user's call). It is NOT the brush's:
    // the Eraser's plain round nib CARVES, which is exactly what the tool does when it holds no
    // preset. A cell that previewed it as a stroke of paint would be advertising the wrong tool.
    REQUIRE_FALSE(panel.grid()->slots().empty());
    CHECK(panel.grid()->slots().front() == -1);
}

TEST_CASE("PresetTabStrip: it scrolls, and a DRAG is a scroll rather than a pick") {
    ui::BrushPresetStore store;
    REQUIRE(store.scanDir(shippedBrushDir()) > 0);

    BrushPresetPanel panel(0, 0, 280, 400);
    panel.setStore(&store);
    ui::PresetTabStrip* strip = panel.tabStrip();
    REQUIRE(strip != nullptr);
    REQUIRE(strip->tabs().size() > 1);

    // ⚠ The premise: the strip must actually OVERFLOW, or "it scrolls" proves nothing. Squeeze it.
    strip->resize(0, 0, 90, 22);
    REQUIRE_MESSAGE(strip->contentWidth() > strip->w(),
                    "the strip must overflow at this width, or the scroll case is vacuous");

    // A press-drag-release that MOVED scrolls the strip and must NOT also pick the tab it began over.
    const ui::PresetTab before = panel.tab();
    Fl::e_x = strip->x() + 60;
    Fl::e_y = strip->y() + 10;
    send(*strip, FL_PUSH);
    Fl::e_x = strip->x() + 10; // 50 px left: well past the drag slop
    send(*strip, FL_DRAG);
    send(*strip, FL_RELEASE);
    CHECK(panel.tab() == before); // the drag scrolled; it did not pick

    // A press-release that did NOT move IS a pick.
    Fl::e_x = strip->x() + 4;
    send(*strip, FL_PUSH);
    send(*strip, FL_RELEASE);
    const std::optional<ui::PresetTab> hit = strip->tabAt(4);
    if (hit)
        CHECK(panel.tab() == *hit);
}

TEST_CASE("PresetTabStrip: a moving release FLINGS the strip, and the fling parks on the edge") {
    ui::BrushPresetStore store;
    REQUIRE(store.scanDir(shippedBrushDir()) > 0);

    BrushPresetPanel panel(0, 0, 280, 400);
    panel.setStore(&store);
    ui::PresetTabStrip* strip = panel.tabStrip();
    REQUIRE(strip != nullptr);
    strip->resize(0, 0, 90, 22);
    const int max = strip->contentWidth() - strip->w();
    REQUIRE_MESSAGE(max > 60, "the strip must overflow well past the drag, or the clamp is untested");

    const ui::PresetTab before = panel.tab();
    ui::presetUiSetNowForTest(0.0); // the pinned clock again: velocity needs definite times
    Fl::e_x = strip->x() + 70;
    Fl::e_y = strip->y() + 10;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    send(*strip, FL_PUSH);
    ui::presetUiSetNowForTest(0.02);
    Fl::e_x = strip->x() + 20; // 50 px left in 20 ms: 2500 px/s
    send(*strip, FL_DRAG);
    CHECK(strip->scrollOffset() == 50);
    ui::presetUiSetNowForTest(0.03);
    send(*strip, FL_RELEASE);
    CHECK(panel.tab() == before); // still a scroll, never a pick
    REQUIRE(strip->flinging());

    const int atRelease = strip->scrollOffset();
    strip->flingTick(1.0 / 60.0);
    CHECK(strip->scrollOffset() > atRelease); // the release kept the strip moving

    int guard = 0;
    while (strip->flinging() && ++guard < 10000)
        strip->flingTick(1.0 / 60.0);
    CHECK_FALSE(strip->flinging());
    // 2500 px/s wants ~875 px of travel -- far past this strip's end -- so it parks ON the edge
    // rather than integrating an overshoot nobody can see.
    CHECK(strip->scrollOffset() == max);

    ui::presetUiSetNowForTest(std::nullopt); // leave the clock as we found it
}

TEST_CASE("BrushPresetStore: a bare scanDir owns NOTHING -- the User tab is earned, not assumed") {
    // ⚠ The trap this guards, and it survived a mutant until it got its own case. `isUserPreset` is
    // an INDEX RANGE: everything at or past the boundary drawn between the shipped scan and the
    // user's is theirs. But a bare scanDir() -- what every test does, and what an importer would --
    // draws NO boundary. Without the `>= 0` guard, `index >= m_userSplitAt` reads `index >= -1`,
    // which is true for every preset in the library: the whole shipped bundle would file itself
    // under the user's own brushes, and the User tab would open on 114 brushes they never installed.
    ui::BrushPresetStore store;
    const int n = store.scanDir(shippedBrushDir());
    REQUIRE(n > 0);

    CHECK(store.userPresetCount() == 0);
    for (int i = 0; i < n; ++i)
        CHECK_FALSE(store.isUserPreset(i));

    // ... so the dock must not offer a User tab at all.
    BrushPresetPanel panel(0, 0, 280, 400);
    panel.setStore(&store);
    REQUIRE(panel.tabStrip() != nullptr);
    for (const ui::PresetTabCounts& t : panel.tabStrip()->tabs())
        CHECK(t.tab != ui::PresetTab::User);
}

// ---- ⭐ USER PRESETS LEAD THE DOCK, AND WEAR A BADGE (feedback round 1, items 3 + 7) ------------

namespace {

// An env var, saved and put back (the same shape test_brush_editor.cpp uses, and for the same
// reason: half the suite reads dataDir(), and a leaked override would follow it).
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : m_name(name) {
        if (const char* old = std::getenv(name); old != nullptr) {
            m_had = true;
            m_old = old;
        }
        ::setenv(name, value.c_str(), 1);
    }
    ~ScopedEnv() {
        if (m_had)
            ::setenv(m_name, m_old.c_str(), 1);
        else
            ::unsetenv(m_name);
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* m_name;
    bool m_had = false;
    std::string m_old;
};

io::brush::LibraryPreset simpleAutoPreset(std::string name) {
    io::brush::LibraryPreset lp;
    lp.preset.name = std::move(name);
    lp.preset.tip.kind = io::brush::TipXml::Kind::Auto;
    lp.preset.tip.type = "auto_brush";
    lp.preset.tip.spacing = 0.1;
    lp.preset.tip.autoTip.generator.diameter = 24.0;
    lp.masterDiameter = 24.0;
    lp.tipResolution = io::brush::TipResolution::AutoTip;
    return lp;
}

} // namespace

TEST_CASE("the dock puts the USER's own presets first, and marks them as theirs") {
    // ⚠ THE ORDERING MOVED (the user's call), and the dock is where it shows. `presets()` is now
    // "yours, then Mosaic's", so the very first library cell after "Default round" has to be the
    // brush the user made -- and `isUserPreset`, which used to be a SUFFIX range past the shipped
    // scan, is now a PREFIX one. The two have to agree, or the User tab and the grid order would
    // disagree about the same preset.
    const std::filesystem::path home =
        std::filesystem::temp_directory_path() / "mosaic_test_dock_user_first";
    std::error_code ec;
    std::filesystem::remove_all(home, ec);
    std::filesystem::create_directories(home, ec);
    const ScopedEnv xdg("XDG_DATA_HOME", home.string());
    // The SHIPPED half is the source tree's own bundle directory, exactly as the other cases read
    // it -- so this stays hermetic and never consults a real install.
    const ScopedEnv installed("MOSAIC_DATA_DIR",
                              std::filesystem::path(MOSAIC_SHIPPED_DATA_DIR).string());

    ui::BrushPresetStore store;
    {
        // Seed one user preset by WRITING it, which is the only way one is ever created.
        ui::BrushPresetStore seeder;
        std::string error;
        REQUIRE_MESSAGE(seeder.writeUserPreset(simpleAutoPreset("My Own Brush"), {}, -1, &error) >= 0,
                        error);
    }
    const int total = store.scan();
    REQUIRE(total > 1);

    REQUIRE(store.presets()[0].preset.name == "My Own Brush");
    CHECK(store.isUserPreset(0));
    CHECK(store.userPresetCount() == 1);
    CHECK_FALSE(store.isUserPreset(1)); // ... and the shipped run starts right behind it

    BrushPresetPanel panel(0, 0, 280, 500);
    panel.setStore(&store);
    CHECK(panel.userInstalled(0));
    CHECK_FALSE(panel.userInstalled(1));
    // The badge, in words as well as in ink: the tooltip has to say what the ring means.
    CHECK(panel.tooltipFor(0).find("Your own preset") != std::string::npos);
    CHECK(panel.tooltipFor(1).find("Your own preset") == std::string::npos);

    // The User tab is EARNED, and it now has something to hold.
    REQUIRE(panel.tabStrip() != nullptr);
    bool sawUserTab = false;
    for (const ui::PresetTabCounts& t : panel.tabStrip()->tabs())
        sawUserTab = sawUserTab || t.tab == ui::PresetTab::User;
    CHECK(sawUserTab);

    // In `All`, slot 0 is Default round and slot 1 is the user's brush -- first of the library.
    panel.setTab(ui::PresetTab::All);
    REQUIRE(panel.grid() != nullptr);
    REQUIRE(panel.grid()->slots().size() > 2);
    CHECK(panel.grid()->slots()[0] == -1);
    CHECK(panel.grid()->slots()[1] == 0);
    CHECK(panel.nameAt(panel.grid()->slots()[1]) == "My Own Brush");

    // ⚠ AND INDEX -1 IS STILL THE ONE CACHE KEY WHOSE MEANING DEPENDS ON THE CORPUS. The reorder
    // moved every OTHER index around; it must not have touched the cell that is not a preset at all.
    // The Brush's plain nib paints and the Eraser's carves, and both caches are keyed by index
    // alone, so switching corpus has to evict exactly that key.
    const core::brush::BrushParams* brushPlain = panel.paramsForTest(-1);
    REQUIRE(brushPlain != nullptr);
    CHECK(brushPlain->strokeMode == core::brush::StrokeMode::Paint);
    panel.setCorpus(ui::PresetCorpus::Eraser);
    const core::brush::BrushParams* eraserPlain = panel.paramsForTest(-1);
    REQUIRE(eraserPlain != nullptr);
    CHECK(eraserPlain->strokeMode == core::brush::StrokeMode::Erase);

    std::filesystem::remove_all(home, ec);
}
