#include <doctest/doctest.h>

#include "common/settings.hpp"
#include "ui/brush_presets.hpp"

#include <filesystem>

// The app's preset store (docs/brushes.md §8): the startup scan, and the ONE preset the Brush paints
// with. FLTK-free, so the whole thing is exercised here rather than through a window.
namespace cb = mosaic::core::brush;

using mosaic::ui::BrushPresetStore;

TEST_CASE("preset store: the shipped bundle scans, and a preset resolves ONCE") {
    BrushPresetStore store;
    const int n = store.scanDir(mosaic::common::installedDataDir() / "brushes");
    REQUIRE(n == 117);
    CHECK(store.names().size() == 117);

    // Nothing selected: the Brush keeps the engine's own analytic round tip -- exactly what it
    // painted with before presets existed, and still the default.
    CHECK(store.activeIndex() == -1);
    CHECK(store.activeParams() == nullptr);
    CHECK(store.activePreset() == nullptr);

    REQUIRE(store.select(0));
    const cb::BrushParams* first = store.activeParams();
    REQUIRE(first != nullptr);
    CHECK(store.activeIndex() == 0);
    REQUIRE(store.activePreset() != nullptr);
    CHECK(store.activePreset()->preset.name == store.names()[0]);
    REQUIRE(first->tip != nullptr);

    // ⚠ RESOLVED ONCE, NOT PER STROKE. Re-selecting the same preset is allowed to rebuild it, but
    // ASKING for the params must not: a fresh BrushTip id every stroke is a permanently cold dab
    // cache, and every dab of every stroke would re-rasterize its own mask.
    const std::uint64_t id = first->tip->id;
    for (int i = 0; i < 5; ++i) {
        REQUIRE(store.activeParams() == first);   // the same object...
        CHECK(store.activeParams()->tip->id == id); // ... and the same raster
    }

    // A different preset is a different tip.
    REQUIRE(store.select(1));
    REQUIRE(store.activeParams() != nullptr);
    REQUIRE(store.activeParams()->tip != nullptr);
    CHECK(store.activeParams()->tip->id != id);

    // ... and -1 puts the plain round tip back.
    REQUIRE(store.select(-1));
    CHECK(store.activeIndex() == -1);
    CHECK(store.activeParams() == nullptr);

    // Out of range selects nothing and says so, rather than resolving garbage.
    CHECK_FALSE(store.select(117));
    CHECK(store.activeIndex() == -1);
}

TEST_CASE("preset store: the resolved params ask for FRESH randomness per stroke") {
    // ⚠⚠ THE OTHER HALF OF "RESOLVED ONCE" (docs/brushes.md §6.6i, a USER-REPORTED bug). Because the
    // params are built once and shared by every stroke, `seed` is shared too -- so every stroke drew
    // the identical random sequence and a single-dab TAP, which is exactly one draw, stamped the
    // same hose cell forever ("single dabs don't take the next shape"). The store is where the live
    // canvas's params are minted, so the store is where the flag belongs; the engine then folds each
    // stroke's own first sample into the seed, which varies per stroke without costing the replay
    // contract anything.
    BrushPresetStore store;
    REQUIRE(store.scanDir(mosaic::common::installedDataDir() / "brushes") == 117);

    REQUIRE(store.select(0));
    REQUIRE(store.activeParams() != nullptr);
    CHECK(store.activeParams()->seedFromFirstSample);
    // ... and the ERASER's corpus is minted through the same path, so it must carry it too: an
    // eraser preset's tip can be a random hose like any other.
    REQUIRE(store.selectEraser(0));
    REQUIRE(store.activeEraserParams() != nullptr);
    CHECK(store.activeEraserParams()->seedFromFirstSample);
}

TEST_CASE("preset store: a directory with no bundles is not an error") {
    // A build with no data dir, or a user who has installed nothing: the store is empty, the Brush
    // falls back to its round tip, and nothing anywhere throws.
    BrushPresetStore store;
    CHECK(store.scanDir(std::filesystem::temp_directory_path() / "mosaic-no-such-dir-ever") == 0);
    CHECK(store.presets().empty());
    CHECK(store.activeParams() == nullptr);
    CHECK(store.names().empty());
    CHECK_FALSE(store.select(0)); // nothing to select
}
