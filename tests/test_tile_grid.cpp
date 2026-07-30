#include "core/tile_grid.hpp"

#include "io/mosaic/docio.hpp"
#include "render/gpu_caps.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using namespace mosaic::core;
using mosaic::common::Rect;

// The tile vocabulary (S60-a, docs/s60-performance-plan.md section 3). Everything here is pure
// geometry + set algebra, so it needs no device and no window -- which is exactly why the header
// was written FLTK-free and Vulkan-free.
//
// The recurring hazard is the PARTIAL EDGE TILE: a 100x100 grid at tile 64 is 2x2 tiles whose
// right and bottom members are 36 px. Almost every test below is really asking "did anyone assume
// tiles are square and complete?".

namespace {

std::vector<TileCoord> collect(const TileSet& s) {
    std::vector<TileCoord> v;
    s.forEach([&v](TileCoord c) { v.push_back(c); });
    return v;
}

bool sameSet(const TileSet& a, const TileSet& b) {
    return a.grid() == b.grid() && a.count() == b.count() && collect(a) == collect(b);
}

// `outer` fully covers `inner` (an empty inner is covered by anything).
bool covers(const Rect& outer, const Rect& inner) {
    if (inner.empty())
        return true;
    return !outer.empty() && inner.x >= outer.x && inner.y >= outer.y
           && inner.right() <= outer.right() && inner.bottom() <= outer.bottom();
}

// Walk every tile of the grid and assert the tiling is a PARTITION of the pixel rect: each tile
// is non-empty, lies inside the grid, is at most one tile edge across, and every pixel is claimed
// by exactly one tile. `tileAt` must agree with the tile that claims the pixel.
void checkTilingIsExact(std::uint32_t w, std::uint32_t h, std::uint32_t tile) {
    CAPTURE(w);
    CAPTURE(h);
    CAPTURE(tile);
    const TileGrid g(w, h, tile);
    REQUIRE(g.tilesX() == (w + tile - 1) / tile);
    REQUIRE(g.tilesY() == (h + tile - 1) / tile);
    REQUIRE(g.tileCount() == static_cast<std::uint64_t>(g.tilesX()) * g.tilesY());

    std::vector<int> cover(static_cast<std::size_t>(w) * h, 0);
    Rect uni{};
    for (std::uint32_t ty = 0; ty < g.tilesY(); ++ty) {
        for (std::uint32_t tx = 0; tx < g.tilesX(); ++tx) {
            const TileCoord c{tx, ty};
            REQUIRE(g.contains(c));
            const Rect b = g.tileBounds(c);
            REQUIRE_FALSE(b.empty());
            CHECK(b.w <= static_cast<double>(tile));
            CHECK(b.h <= static_cast<double>(tile));
            CHECK(b.x >= 0.0);
            CHECK(b.y >= 0.0);
            CHECK(b.right() <= static_cast<double>(w));   // the clip that makes edges partial
            CHECK(b.bottom() <= static_cast<double>(h));
            uni = uni.united(b);

            for (std::uint32_t py = static_cast<std::uint32_t>(b.y);
                 py < static_cast<std::uint32_t>(b.bottom()); ++py) {
                for (std::uint32_t px = static_cast<std::uint32_t>(b.x);
                     px < static_cast<std::uint32_t>(b.right()); ++px) {
                    ++cover[static_cast<std::size_t>(py) * w + px];
                    CHECK(g.tileAt(px, py) == c);
                }
            }
            // index <-> coord is a bijection over the grid.
            CHECK(g.coordOf(g.index(c)) == c);
            CHECK(g.index(c) < g.tileCount());
        }
    }
    // The union of every tile is EXACTLY the grid -- never one pixel more.
    CHECK(uni == Rect{0.0, 0.0, static_cast<double>(w), static_cast<double>(h)});
    for (std::size_t i = 0; i < cover.size(); ++i) {
        if (cover[i] != 1) {
            FAIL_CHECK("pixel " << i << " covered " << cover[i] << " times");
            break;
        }
    }
    // ... and the whole-grid range says the same thing.
    CHECK(g.rangeBounds({0, 0, g.tilesX(), g.tilesY()})
          == Rect{0.0, 0.0, static_cast<double>(w), static_cast<double>(h)});
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The three constants are ONE number
// ---------------------------------------------------------------------------------------------

// A compile-time trip-wire as well as a runtime one: the whole tile design rests on the store's
// tile, the dirty tile and the compositor's tile being the same 64, so a drift should be as loud
// as possible, as early as possible.
static_assert(kTileSize == mosaic::io::native::kTileSize,
              "core::kTileSize must equal io::native::kTileSize -- an in-memory tile maps 1:1 "
              "onto a stored tile, with no re-tiling on save");
static_assert(kTileSize == mosaic::render::kDirtyTileSize,
              "core::kTileSize must equal render::kDirtyTileSize -- one dirty set feeds both the "
              "recomposite and the autosave journal");

TEST_CASE("core, io and render agree on the tile size -- all three, pinned together") {
    // docs/s60-performance-plan.md section 3.1: one TileKey vocabulary is shared by the
    // compositor's dirty set and the .mosaic journal's TILE frames. If any one of these three
    // moves, that alignment is silently gone, so this fails rather than the format drifting.
    CHECK(kTileSize == mosaic::io::native::kTileSize);
    CHECK(kTileSize == mosaic::render::kDirtyTileSize);
    CHECK(mosaic::io::native::kTileSize == mosaic::render::kDirtyTileSize);
    CHECK(kTileSize == 64u);

    // The macrotile is the GPU DISPATCH granule and is deliberately a different number -- but it
    // must remain an exact multiple of the tracking tile, or a macrotile could not be described
    // as a whole number of dirty tiles.
    CHECK((kTileSize << mosaic::render::kMacrotileDefaultShift) % kTileSize == 0u);
    CHECK(TileGrid(1024, 1024).tileSize() == kTileSize);  // ... and it is the grid's default
}

// ---------------------------------------------------------------------------------------------
// TileGrid: the partial-edge-tile minefield
// ---------------------------------------------------------------------------------------------

TEST_CASE("the tiling is an exact partition at awkward sizes") {
    checkTilingIsExact(1, 1, 64);        // one tile, 1x1 px
    checkTilingIsExact(63, 63, 64);      // one tile, short on both axes
    checkTilingIsExact(64, 64, 64);      // one tile, exact
    checkTilingIsExact(65, 65, 64);      // 2x2, with 1-px edge tiles
    checkTilingIsExact(100, 100, 64);    // 2x2, with 36-px edge tiles (the canonical case)
    checkTilingIsExact(128, 64, 64);     // exact multiple on both axes
    checkTilingIsExact(4096, 1, 64);     // one tile row, 1 px tall
    checkTilingIsExact(1, 4096, 64);     // ... and its transpose
    checkTilingIsExact(100, 100, 16);    // a non-default tile edge still partitions
    checkTilingIsExact(100, 100, 7);     // ... including one that divides nothing
    checkTilingIsExact(300, 200, 256);   // tile larger than one axis
}

TEST_CASE("edge tiles are partial and interior tiles are not") {
    const TileGrid g(100, 100);
    REQUIRE(g.tilesX() == 2);
    REQUIRE(g.tilesY() == 2);
    CHECK(g.tileCount() == 4);
    CHECK(g.tileBounds({0, 0}) == Rect{0, 0, 64, 64});
    CHECK(g.tileBounds({1, 0}) == Rect{64, 0, 36, 64});
    CHECK(g.tileBounds({0, 1}) == Rect{0, 64, 64, 36});
    CHECK(g.tileBounds({1, 1}) == Rect{64, 64, 36, 36});
    // Out-of-range coords have no bounds at all, rather than bounds outside the grid.
    CHECK(g.tileBounds({2, 0}).empty());
    CHECK(g.tileBounds({0, 2}).empty());
    CHECK(g.tileBounds({99, 99}).empty());
    CHECK_FALSE(g.contains({2, 0}));
    CHECK_FALSE(g.contains({0, 2}));
    CHECK(g.contains({1, 1}));
}

TEST_CASE("an empty grid has no tiles, not one degenerate tile") {
    for (const TileGrid g : {TileGrid(), TileGrid(0, 0), TileGrid(0, 100), TileGrid(100, 0)}) {
        CHECK(g.empty());
        CHECK(g.tilesX() == 0);
        CHECK(g.tilesY() == 0);
        CHECK(g.tileCount() == 0);
        CHECK_FALSE(g.contains({0, 0}));
        CHECK(g.tileBounds({0, 0}).empty());
        CHECK(g.tilesCovering({0, 0, 100, 100}).empty());
        CHECK(g.rangeBounds({0, 0, 4, 4}).empty());
        CHECK(g.tileAt(0, 0) == TileCoord{0, 0});   // clamped, and harmless
        CHECK(g.tileAt(999, 999) == TileCoord{0, 0});
        CHECK(g.coordOf(17) == TileCoord{0, 0});    // no division by a zero row length
    }
}

TEST_CASE("a zero tile edge falls back to the default rather than dividing by zero") {
    // Implementation choice, not header contract: a 0 tile is a caller bug, and substituting the
    // default keeps every invariant true instead of trading one bug for undefined behaviour.
    const TileGrid g(100, 100, 0);
    CHECK(g.tileSize() == kTileSize);
    CHECK(g.tilesX() == 2);
    CHECK(g.tilesY() == 2);
}

TEST_CASE("tileAt clamps out-of-range pixels to the nearest edge tile") {
    const TileGrid g(100, 100);
    CHECK(g.tileAt(0, 0) == TileCoord{0, 0});
    CHECK(g.tileAt(63, 63) == TileCoord{0, 0});
    CHECK(g.tileAt(64, 63) == TileCoord{1, 0});
    CHECK(g.tileAt(99, 99) == TileCoord{1, 1});
    CHECK(g.tileAt(100, 100) == TileCoord{1, 1});  // one past the edge -> the edge tile
    CHECK(g.tileAt(4'000'000'000u, 4'000'000'000u) == TileCoord{1, 1});
    for (std::uint32_t p = 0; p < 100; ++p)
        CHECK(g.contains(g.tileAt(p, p)));
}

TEST_CASE("index and coordOf are inverse over a very wide grid") {
    const TileGrid g(4096, 1);  // 64 x 1 tiles: the row-major arithmetic has nowhere to hide
    REQUIRE(g.tilesX() == 64);
    REQUIRE(g.tilesY() == 1);
    for (std::uint32_t tx = 0; tx < 64; ++tx) {
        CHECK(g.index({tx, 0}) == tx);
        CHECK(g.coordOf(tx) == TileCoord{tx, 0});
    }
    const TileGrid tall(1, 4096);
    REQUIRE(tall.tilesX() == 1);
    REQUIRE(tall.tilesY() == 64);
    for (std::uint32_t ty = 0; ty < 64; ++ty) {
        CHECK(tall.index({0, ty}) == ty);
        CHECK(tall.coordOf(ty) == TileCoord{0, ty});
    }
}

// ---------------------------------------------------------------------------------------------
// tilesCovering: clamp first, half-open always
// ---------------------------------------------------------------------------------------------

TEST_CASE("tilesCovering clamps to the grid before it converts to tiles") {
    const TileGrid g(100, 100);

    SUBCASE("aligned rects") {
        CHECK(g.tilesCovering({0, 0, 100, 100}) == TileRange{0, 0, 2, 2});
        CHECK(g.tilesCovering({0, 0, 64, 64}) == TileRange{0, 0, 1, 1});
        CHECK(g.tilesCovering({0, 0, 65, 65}) == TileRange{0, 0, 2, 2});
        CHECK(g.tilesCovering({64, 64, 36, 36}) == TileRange{1, 1, 2, 2});
        CHECK(g.tilesCovering({63, 0, 1, 1}) == TileRange{0, 0, 1, 1});
        CHECK(g.tilesCovering({63, 0, 2, 1}) == TileRange{0, 0, 2, 1});
    }
    SUBCASE("a rect running off the canvas yields only REAL tiles") {
        CHECK(g.tilesCovering({-1000, -1000, 2000, 2000}) == TileRange{0, 0, 2, 2});
        CHECK(g.tilesCovering({80, 80, 1000, 1000}) == TileRange{1, 1, 2, 2});
        CHECK(g.tilesCovering({-10, 10, 20, 20}) == TileRange{0, 0, 1, 1});
        // ... and the range never exceeds the grid, which is what stops a composite walking off.
        const TileRange r = g.tilesCovering({-1e9, -1e9, 2e9, 2e9});
        CHECK(r.x1 <= g.tilesX());
        CHECK(r.y1 <= g.tilesY());
        CHECK(r.count() == g.tileCount());
    }
    SUBCASE("a rect entirely outside touches nothing") {
        CHECK(g.tilesCovering({100, 0, 50, 50}).empty());     // flush against the right edge
        CHECK(g.tilesCovering({0, 100, 50, 50}).empty());
        CHECK(g.tilesCovering({-50, 0, 50, 50}).empty());     // flush against the left edge
        CHECK(g.tilesCovering({0, -50, 50, 50}).empty());
        CHECK(g.tilesCovering({1000, 1000, 10, 10}).empty());
        CHECK(g.tilesCovering({-1000, -1000, 10, 10}).empty());
    }
    SUBCASE("a zero-area or inverted rect touches nothing") {
        CHECK(g.tilesCovering({}).empty());
        CHECK(g.tilesCovering({10, 10, 0, 0}).empty());
        CHECK(g.tilesCovering({10, 10, 0, 50}).empty());
        CHECK(g.tilesCovering({10, 10, 50, 0}).empty());
        CHECK(g.tilesCovering({10, 10, -5, -5}).empty());
        CHECK(g.tilesCovering({64, 64, 0, 0}).empty());  // exactly on a tile corner
    }
    SUBCASE("non-integer rects: half-open, so 0.5 .. 64.5 touches tiles 0 AND 1") {
        CHECK(g.tilesCovering({0.5, 0.5, 64.0, 64.0}) == TileRange{0, 0, 2, 2});
        CHECK(g.tilesCovering({0.5, 0.5, 63.5, 63.5}) == TileRange{0, 0, 1, 1});  // ends at 64.0
        CHECK(g.tilesCovering({63.5, 0, 0.5, 1}) == TileRange{0, 0, 1, 1});       // ends at 64.0
        CHECK(g.tilesCovering({63.5, 0, 0.75, 1}) == TileRange{0, 0, 2, 1});      // ends at 64.25
        CHECK(g.tilesCovering({64.0, 0, 1, 1}) == TileRange{1, 0, 2, 1});
        CHECK(g.tilesCovering({99.5, 99.5, 0.25, 0.25}) == TileRange{1, 1, 2, 2});
        // A sub-pixel dab straddling a tile boundary dirties both tiles, not one.
        CHECK(g.tilesCovering({63.9, 63.9, 0.2, 0.2}) == TileRange{0, 0, 2, 2});
    }
    SUBCASE("infinite and NaN edges degrade to something safe") {
        const double inf = std::numeric_limits<double>::infinity();
        const double nan = std::numeric_limits<double>::quiet_NaN();
        CHECK(g.tilesCovering({0, 0, inf, inf}) == TileRange{0, 0, 2, 2});
        CHECK(g.tilesCovering({-inf, -inf, inf, inf}).empty());  // right() is NaN
        CHECK(g.tilesCovering({nan, 0, 10, 10}).empty());
        CHECK(g.tilesCovering({0, nan, 10, 10}).empty());
        CHECK(g.tilesCovering({0, 0, nan, 10}).empty());
        CHECK(g.tilesCovering({0, 0, 10, nan}).empty());
    }
}

TEST_CASE("rangeBounds is the range's pixel extent, clipped to the grid") {
    const TileGrid g(100, 100);
    CHECK(g.rangeBounds({0, 0, 2, 2}) == Rect{0, 0, 100, 100});
    CHECK(g.rangeBounds({1, 1, 2, 2}) == Rect{64, 64, 36, 36});
    CHECK(g.rangeBounds({0, 0, 1, 2}) == Rect{0, 0, 64, 100});
    CHECK(g.rangeBounds({0, 0, 9, 9}) == Rect{0, 0, 100, 100});  // over-wide range is clamped
    CHECK(g.rangeBounds({}).empty());
    CHECK(g.rangeBounds({1, 1, 1, 1}).empty());  // x1 <= x0
    CHECK(g.rangeBounds({5, 5, 7, 7}).empty());  // entirely past the grid
    CHECK(g.rangeBounds({2, 0, 3, 1}).empty());
}

TEST_CASE("tilesCovering and rangeBounds round-trip through the whole grid") {
    for (const TileGrid g : {TileGrid(1, 1), TileGrid(63, 63), TileGrid(64, 64), TileGrid(65, 65),
                             TileGrid(100, 100), TileGrid(4096, 1)}) {
        const Rect all{0, 0, static_cast<double>(g.width()), static_cast<double>(g.height())};
        const TileRange r = g.tilesCovering(all);
        CHECK(r == TileRange{0, 0, g.tilesX(), g.tilesY()});
        CHECK(r.count() == g.tileCount());
        CHECK(g.rangeBounds(r) == all);
    }
}

// ---------------------------------------------------------------------------------------------
// TileSet: the bitset, and its tail word
// ---------------------------------------------------------------------------------------------

TEST_CASE("a fresh set is empty and a reset set forgets everything") {
    TileSet s{TileGrid(100, 100)};
    CHECK(s.empty());
    CHECK(s.count() == 0);
    CHECK_FALSE(s.all());
    CHECK(s.boundingRect().empty());
    CHECK(collect(s).empty());

    s.add(TileCoord{1, 1});
    CHECK(s.count() == 1);
    CHECK(s.test({1, 1}));
    CHECK_FALSE(s.test({0, 0}));

    s.clear();
    CHECK(s.empty());
    CHECK(s.count() == 0);
    CHECK_FALSE(s.test({1, 1}));
    CHECK(s.grid() == TileGrid(100, 100));  // clear() keeps the grid

    s.add(TileCoord{1, 1});
    s.reset(TileGrid(512, 512));
    CHECK(s.grid() == TileGrid(512, 512));
    CHECK(s.empty());
    CHECK(collect(s).empty());
}

TEST_CASE("add is idempotent and refuses out-of-range coords instead of clamping") {
    TileSet s{TileGrid(100, 100)};
    s.add(TileCoord{1, 0});
    s.add(TileCoord{1, 0});
    s.add(TileCoord{1, 0});
    CHECK(s.count() == 1);

    // A clamp here would dirty a real tile the caller never touched -- silently wrong pixels
    // rather than visibly missing ones.
    s.add(TileCoord{2, 0});
    s.add(TileCoord{0, 2});
    s.add(TileCoord{99, 99});
    s.add(TileCoord{0xFFFFFFFFu, 0xFFFFFFFFu});
    CHECK(s.count() == 1);
    CHECK(collect(s) == std::vector<TileCoord>{{1, 0}});
    CHECK_FALSE(s.test({2, 0}));
    CHECK_FALSE(s.test({0xFFFFFFFFu, 0}));
}

TEST_CASE("addAll never sets the tail word's padding bits") {
    // The 64-tiles-per-word boundary is where a padding bit becomes a phantom tile: count(), all()
    // and forEach() would all report a tile the grid does not have.
    SUBCASE("tile count is NOT a multiple of 64 (a live tail)") {
        TileSet s{TileGrid(65 * 64, 64)};  // 65 x 1 tiles -> 2 words, 1 live bit in the tail
        REQUIRE(s.grid().tileCount() == 65);
        s.addAll();
        CHECK(s.count() == 65);
        CHECK(s.all());
        CHECK(collect(s).size() == 65);
        CHECK(collect(s).back() == TileCoord{64, 0});
        CHECK(s.test({64, 0}));
        CHECK_FALSE(s.test({65, 0}));
        CHECK(s.boundingRect() == Rect{0, 0, 65 * 64, 64});
    }
    SUBCASE("tile count is EXACTLY a multiple of 64 (no tail to mask)") {
        TileSet s{TileGrid(64 * 64, 64)};  // 64 x 1 tiles -> exactly one full word
        REQUIRE(s.grid().tileCount() == 64);
        s.addAll();
        CHECK(s.count() == 64);  // the mask must not eat the last word here
        CHECK(s.all());
        CHECK(collect(s).size() == 64);
    }
    SUBCASE("a small grid, where the whole word is tail") {
        TileSet s{TileGrid(100, 100)};
        s.addAll();
        CHECK(s.count() == 4);
        CHECK(s.all());
        CHECK(collect(s) == std::vector<TileCoord>{{0, 0}, {1, 0}, {0, 1}, {1, 1}});
        CHECK(s.boundingRect() == Rect{0, 0, 100, 100});
    }
    SUBCASE("addAll then clear leaves nothing behind") {
        TileSet s{TileGrid(65 * 64, 64)};
        s.addAll();
        s.clear();
        CHECK(s.count() == 0);
        CHECK(collect(s).empty());
        CHECK_FALSE(s.all());
    }
}

TEST_CASE("a set over an empty grid is inert") {
    TileSet s{TileGrid(0, 0)};
    CHECK(s.empty());
    s.addAll();
    CHECK(s.count() == 0);
    CHECK(s.all());  // vacuously: count() == tileCount() == 0
    s.add(TileCoord{0, 0});
    s.add(Rect{0, 0, 100, 100});
    s.add(TileRange{0, 0, 4, 4});
    CHECK(s.count() == 0);
    CHECK(s.boundingRect().empty());
    CHECK(collect(s).empty());
    CHECK(collect(s.macrotiles(3)).empty());

    TileSet def;  // default-constructed: same story, and no allocation to trip over
    def.addAll();
    def.add(TileCoord{0, 0});
    CHECK(def.count() == 0);
    CHECK(def.boundingRect().empty());
}

TEST_CASE("adding a pixel rect dirties exactly the tiles it touches") {
    TileSet s{TileGrid(100, 100)};
    s.add(Rect{0, 0, 1, 1});
    CHECK(collect(s) == std::vector<TileCoord>{{0, 0}});

    s.clear();
    s.add(Rect{63.5, 63.5, 1, 1});  // straddles both boundaries
    CHECK(s.count() == 4);
    CHECK(s.all());

    s.clear();
    s.add(Rect{-500, -500, 1000, 1000});  // clamped, so no phantom tiles
    CHECK(s.count() == 4);

    s.clear();
    s.add(Rect{200, 200, 10, 10});  // entirely outside
    CHECK(s.empty());

    s.clear();
    s.add(Rect{10, 10, 0, 0});  // zero area
    CHECK(s.empty());
}

TEST_CASE("adding a tile range sets whole rows, and clamps to the grid") {
    // Deliberately wide: the run-setter writes whole 64-bit words, so a row crossing a word
    // boundary is the interesting case.
    TileSet s{TileGrid(200 * 64, 3 * 64)};  // 200 x 3 tiles
    REQUIRE(s.grid().tileCount() == 600);

    s.add(TileRange{0, 1, 200, 2});  // the whole middle row
    CHECK(s.count() == 200);
    for (std::uint32_t tx = 0; tx < 200; ++tx) {
        CHECK(s.test({tx, 1}));
        CHECK_FALSE(s.test({tx, 0}));
        CHECK_FALSE(s.test({tx, 2}));
    }

    s.add(TileRange{0, 1, 200, 2});  // idempotent -- no double counting
    CHECK(s.count() == 200);

    s.add(TileRange{190, 0, 1000, 1000});  // over-wide range is clamped, not wrapped
    CHECK(s.count() == 200 + 10 * 3 - 10);
    CHECK(s.test({199, 2}));
    CHECK_FALSE(s.test({189, 2}));

    s.clear();
    s.add(TileRange{5, 5, 3, 3});  // entirely past the grid
    CHECK(s.empty());
    s.add(TileRange{2, 2, 1, 1});  // inverted: x1 < x0 and y1 < y0
    CHECK(s.empty());
    s.add(TileRange{});
    CHECK(s.empty());

    // A run that starts and ends mid-word, inside one word.
    s.clear();
    s.add(TileRange{3, 0, 9, 1});
    CHECK(s.count() == 6);
    for (std::uint32_t tx = 0; tx < 200; ++tx)
        CHECK(s.test({tx, 0}) == (tx >= 3 && tx < 9));

    // A run spanning several whole words plus partial ends.
    s.clear();
    s.add(TileRange{60, 0, 140, 1});
    CHECK(s.count() == 80);
    for (std::uint32_t tx = 0; tx < 200; ++tx)
        CHECK(s.test({tx, 0}) == (tx >= 60 && tx < 140));
}

TEST_CASE("forEach visits every dirty tile once, in row-major order") {
    TileSet s{TileGrid(5 * 64, 4 * 64)};  // 5 x 4 tiles
    s.add(TileCoord{4, 3});
    s.add(TileCoord{0, 0});
    s.add(TileCoord{2, 1});
    s.add(TileCoord{0, 3});

    const std::vector<TileCoord> v = collect(s);
    REQUIRE(v.size() == 4);
    CHECK(v == std::vector<TileCoord>{{0, 0}, {2, 1}, {0, 3}, {4, 3}});
    CHECK(s.count() == v.size());

    // Order is genuinely row-major, not merely sorted by some accident of the word layout.
    for (std::size_t i = 1; i < v.size(); ++i) {
        const std::uint64_t prev = s.grid().index(v[i - 1]);
        CHECK(prev < s.grid().index(v[i]));
    }

    // A null std::function is not a crash.
    s.forEach(std::function<void(TileCoord)>{});
}

TEST_CASE("unite is a set union over one grid, and a no-op across grids") {
    SUBCASE("same grid") {
        TileSet a{TileGrid(100, 100)};
        TileSet b{TileGrid(100, 100)};
        a.add(TileCoord{0, 0});
        a.add(TileCoord{1, 0});
        b.add(TileCoord{1, 0});  // deliberately overlapping
        b.add(TileCoord{1, 1});

        a.unite(b);
        CHECK(a.count() == 3);  // the overlap is counted once
        CHECK(collect(a) == std::vector<TileCoord>{{0, 0}, {1, 0}, {1, 1}});
        CHECK(b.count() == 2);  // the argument is untouched

        a.unite(a);  // self-union is a no-op, not a doubling
        CHECK(a.count() == 3);

        TileSet empty{TileGrid(100, 100)};
        a.unite(empty);
        CHECK(a.count() == 3);
        empty.unite(a);
        CHECK(empty.count() == 3);
        CHECK(collect(empty) == collect(a));
    }
    SUBCASE("different grids: refused, and nothing is corrupted") {
        // Mixing grids is a bug; quietly producing a wrong answer would hide it. The important
        // half of this test is that the REFUSAL leaves the receiver byte-identical.
        TileSet a{TileGrid(100, 100)};
        a.add(TileCoord{1, 1});
        const std::vector<TileCoord> before = collect(a);

        TileSet bigger{TileGrid(1000, 1000)};  // more tiles
        bigger.addAll();
        a.unite(bigger);
        CHECK(a.count() == 1);
        CHECK(collect(a) == before);

        TileSet smaller{TileGrid(64, 64)};  // fewer tiles -- the read-past-the-end direction
        smaller.addAll();
        a.unite(smaller);
        CHECK(a.count() == 1);
        CHECK(collect(a) == before);

        // Same pixel size, different TILE size: still a different grid, still refused.
        TileSet finer{TileGrid(100, 100, 32)};
        finer.addAll();
        a.unite(finer);
        CHECK(a.count() == 1);
        CHECK(collect(a) == before);
        CHECK(a.grid() == TileGrid(100, 100));

        // ... and the other direction: the small set must not grow from the big one.
        smaller.clear();
        smaller.add(TileCoord{0, 0});
        smaller.unite(bigger);
        CHECK(smaller.count() == 1);
        CHECK(smaller.grid() == TileGrid(64, 64));

        TileSet none;  // default grid vs a real one
        none.unite(a);
        CHECK(none.count() == 0);
        a.unite(none);
        CHECK(a.count() == 1);
    }
}

TEST_CASE("boundingRect is the tightest pixel rect over the dirty tiles") {
    const TileGrid g(100, 100);
    TileSet s{g};
    CHECK(s.boundingRect().empty());

    s.add(TileCoord{1, 1});
    CHECK(s.boundingRect() == Rect{64, 64, 36, 36});  // clipped: the edge tile is 36 px

    s.clear();
    s.add(TileCoord{1, 0});
    CHECK(s.boundingRect() == Rect{64, 0, 36, 64});

    s.clear();
    s.add(TileCoord{0, 0});
    s.add(TileCoord{1, 1});
    CHECK(s.boundingRect() == Rect{0, 0, 100, 100});  // the diagonal spans everything

    // A sparse set far from the origin, on a grid big enough that the answer is not trivially
    // the whole canvas.
    const TileGrid wide(10 * 64, 10 * 64);
    TileSet t{wide};
    t.add(TileCoord{3, 7});
    t.add(TileCoord{6, 2});
    CHECK(t.boundingRect() == Rect{3 * 64, 2 * 64, 4 * 64, 6 * 64});

    // One tile row: the column scan has to work with a single word, and the row arithmetic with
    // a 1-px-tall grid.
    TileSet row{TileGrid(4096, 1)};
    row.add(TileCoord{10, 0});
    CHECK(row.boundingRect() == Rect{640, 0, 64, 1});
    row.add(TileCoord{63, 0});
    CHECK(row.boundingRect() == Rect{640, 0, 4096 - 640, 1});

    // The full-width early-out path in the column scan.
    TileSet full{wide};
    full.addAll();
    CHECK(full.boundingRect() == Rect{0, 0, 640, 640});
}

TEST_CASE("boundingRect covers every dirty tile and nothing beyond the grid") {
    const TileGrid g(300, 170);  // 5 x 3 tiles, both edges partial
    std::mt19937 rng(0xB0A7Du);
    std::uniform_int_distribution<std::uint32_t> xs(0, g.tilesX() - 1);
    std::uniform_int_distribution<std::uint32_t> ys(0, g.tilesY() - 1);

    for (int iter = 0; iter < 500; ++iter) {
        TileSet s{g};
        const int n = 1 + (iter % 6);
        for (int k = 0; k < n; ++k)
            s.add(TileCoord{xs(rng), ys(rng)});
        const Rect bb = s.boundingRect();
        REQUIRE_FALSE(bb.empty());
        CHECK(covers(Rect{0, 0, 300, 170}, bb));

        Rect tight{};
        s.forEach([&](TileCoord c) {
            CHECK(covers(bb, g.tileBounds(c)));
            tight = tight.united(g.tileBounds(c));
        });
        CHECK(bb == tight);  // tightest, not merely covering
    }
}

// ---------------------------------------------------------------------------------------------
// Macrotile projection (the GPU dispatch granularity)
// ---------------------------------------------------------------------------------------------

TEST_CASE("a single dirty tile lights exactly one macrotile") {
    const TileGrid g(1024, 1024);  // 16 x 16 tiles
    TileSet s{g};
    s.add(TileCoord{5, 7});

    const TileSet m = s.macrotiles(2);  // 256 px macrotiles -> 4 x 4
    CHECK(m.grid().tileSize() == 256);
    CHECK(m.grid().tilesX() == 4);
    CHECK(m.grid().tilesY() == 4);
    CHECK(m.count() == 1);
    CHECK(collect(m) == std::vector<TileCoord>{{1, 1}});
    CHECK(m.boundingRect() == Rect{256, 256, 256, 256});
    CHECK(covers(m.boundingRect(), s.boundingRect()));

    // Every tile inside that macrotile projects onto the same one -- 16 tiles, one dispatch.
    TileSet block{g};
    block.add(TileRange{4, 4, 8, 8});
    REQUIRE(block.count() == 16);
    CHECK(block.macrotiles(2).count() == 1);
}

TEST_CASE("shift 0 returns an equivalent set on the same grid") {
    TileSet s{TileGrid(100, 100)};
    s.add(TileCoord{0, 1});
    s.add(TileCoord{1, 1});
    const TileSet m = s.macrotiles(0);
    CHECK(m.grid() == s.grid());
    CHECK(sameSet(m, s));

    TileSet e{TileGrid(100, 100)};
    CHECK(sameSet(e.macrotiles(0), e));
}

TEST_CASE("projecting twice by 1 equals projecting once by 2") {
    const TileGrid g(1000, 700);  // 16 x 11 tiles, both edges partial
    std::mt19937 rng(0x5EED1234u);
    std::uniform_int_distribution<std::uint32_t> xs(0, g.tilesX() - 1);
    std::uniform_int_distribution<std::uint32_t> ys(0, g.tilesY() - 1);

    for (int iter = 0; iter < 200; ++iter) {
        TileSet s{g};
        for (int k = 0; k <= iter % 9; ++k)
            s.add(TileCoord{xs(rng), ys(rng)});

        CHECK(sameSet(s.macrotiles(1).macrotiles(1), s.macrotiles(2)));
        CHECK(sameSet(s.macrotiles(1).macrotiles(2), s.macrotiles(3)));
        CHECK(sameSet(s.macrotiles(2).macrotiles(1), s.macrotiles(3)));
        CHECK(sameSet(s.macrotiles(1).macrotiles(1).macrotiles(1), s.macrotiles(3)));
    }
}

TEST_CASE("a macrotile projection covers the same pixels, over awkward grids") {
    const std::vector<TileGrid> grids{TileGrid(1, 1),      TileGrid(65, 65),
                                      TileGrid(100, 100),  TileGrid(4096, 1),
                                      TileGrid(1000, 700), TileGrid(5000, 8000)};
    std::mt19937 rng(0xF00DFACEu);

    for (const TileGrid& g : grids) {
        CAPTURE(g.width());
        CAPTURE(g.height());
        std::uniform_int_distribution<std::uint32_t> xs(0, g.tilesX() - 1);
        std::uniform_int_distribution<std::uint32_t> ys(0, g.tilesY() - 1);

        for (std::uint32_t shift = 0; shift <= 4; ++shift) {
            TileSet s{g};
            for (int k = 0; k < 12; ++k)
                s.add(TileCoord{xs(rng), ys(rng)});

            const TileSet m = s.macrotiles(shift);
            CAPTURE(shift);
            // Same pixel area, so the projection's grid describes the same canvas ...
            CHECK(m.grid().width() == g.width());
            CHECK(m.grid().height() == g.height());
            // ... coarser, so it can never hold more dirty units than the original ...
            CHECK(m.count() <= s.count());
            CHECK(m.count() >= 1);
            // ... and it must cover everything the original did.
            CHECK(covers(m.boundingRect(), s.boundingRect()));

            // The real invariant, checked WITHOUT re-deriving the projection: every pixel of
            // every dirty tile lands in a macrotile that is dirty.
            s.forEach([&](TileCoord c) {
                const Rect b = g.tileBounds(c);
                const std::uint32_t x1 = static_cast<std::uint32_t>(b.right()) - 1;
                const std::uint32_t y1 = static_cast<std::uint32_t>(b.bottom()) - 1;
                CHECK(m.test(m.grid().tileAt(static_cast<std::uint32_t>(b.x),
                                             static_cast<std::uint32_t>(b.y))));
                CHECK(m.test(m.grid().tileAt(x1, y1)));
            });

            // Conversely, no macrotile is dirty without a dirty tile inside it.
            m.forEach([&](TileCoord mc) {
                const Rect mb = m.grid().tileBounds(mc);
                bool found = false;
                s.forEach([&](TileCoord c) { found = found || g.tileBounds(c).intersects(mb); });
                CHECK(found);
            });
        }
    }
}

TEST_CASE("a full set projects to a full set, and an empty one to an empty one") {
    for (const TileGrid g : {TileGrid(100, 100), TileGrid(65, 65), TileGrid(4096, 1),
                             TileGrid(1000, 700)}) {
        TileSet full{g};
        full.addAll();
        for (std::uint32_t shift = 0; shift <= 3; ++shift) {
            const TileSet m = full.macrotiles(shift);
            CHECK(m.all());
            CHECK(m.count() == m.grid().tileCount());
            CHECK(m.boundingRect()
                  == Rect{0, 0, static_cast<double>(g.width()), static_cast<double>(g.height())});
        }
        TileSet none{g};
        CHECK(none.macrotiles(2).empty());
        CHECK(none.macrotiles(2).boundingRect().empty());
    }
}

TEST_CASE("an absurd shift saturates instead of overflowing") {
    // Nothing in the app asks for this (k is 0..4, gpu_caps.hpp), but the shift is a u32 in the
    // API and `tileSize << 40` would be undefined.
    TileSet s{TileGrid(1000, 700)};
    s.add(TileCoord{3, 4});
    for (const std::uint32_t shift : {26u, 31u, 32u, 63u, 64u, 1000u, 0xFFFFFFFFu}) {
        CAPTURE(shift);
        const TileSet m = s.macrotiles(shift);
        CHECK(m.grid().width() == 1000);
        CHECK(m.grid().height() == 700);
        CHECK(m.count() >= 1);
        CHECK(covers(m.boundingRect(), s.boundingRect()));
    }
}

// ---------------------------------------------------------------------------------------------
// Property tests (deterministic seeds -- no std::random_device anywhere)
// ---------------------------------------------------------------------------------------------

TEST_CASE("property: tilesCovering is exactly the set of tiles the rect touches") {
    struct Case {
        std::uint32_t w, h, tile;
    };
    const Case cases[] = {{100, 100, 64}, {137, 83, 64}, {64, 64, 64},
                          {1, 1, 64},     {200, 40, 16}, {97, 61, 7}};
    std::mt19937 rng(0x51DE5EEDu);
    // Quarter-pixel coordinates: exactly representable, and dividing by a tile edge stays exact
    // for the power-of-two edges -- so the property tests the ARITHMETIC, not the rounding of a
    // pathological double.
    std::uniform_int_distribution<int> quarters(-4 * 40, 4 * 260);

    for (const Case& cs : cases) {
        const TileGrid g(cs.w, cs.h, cs.tile);
        const Rect canvas{0, 0, static_cast<double>(cs.w), static_cast<double>(cs.h)};
        CAPTURE(cs.w);
        CAPTURE(cs.h);
        CAPTURE(cs.tile);

        for (int iter = 0; iter < 400; ++iter) {
            const double x = quarters(rng) * 0.25;
            const double y = quarters(rng) * 0.25;
            const double w = quarters(rng) * 0.25;
            const double h = quarters(rng) * 0.25;
            const Rect r{x, y, w, h};
            const Rect clamped = r.intersected(canvas);
            const TileRange got = g.tilesCovering(r);

            if (clamped.empty()) {
                CHECK(got.empty());
                continue;
            }
            REQUIRE_FALSE(got.empty());
            CHECK(got.x1 <= g.tilesX());
            CHECK(got.y1 <= g.tilesY());
            // rangeBounds of the answer must cover the clamped rect ...
            CHECK(covers(g.rangeBounds(got), clamped));
            // ... no covered tile is disjoint from the rect (the range is TIGHT) ...
            for (std::uint32_t ty = got.y0; ty < got.y1; ++ty)
                for (std::uint32_t tx = got.x0; tx < got.x1; ++tx)
                    CHECK(g.tileBounds({tx, ty}).intersects(clamped));
            // ... and every pixel the rect touches lives in a covered tile.
            const std::uint32_t px0 = static_cast<std::uint32_t>(std::floor(clamped.x));
            const std::uint32_t py0 = static_cast<std::uint32_t>(std::floor(clamped.y));
            const std::uint32_t px1 =
                std::min(static_cast<std::uint32_t>(std::ceil(clamped.right())), cs.w);
            const std::uint32_t py1 =
                std::min(static_cast<std::uint32_t>(std::ceil(clamped.bottom())), cs.h);
            for (std::uint32_t py = py0; py < py1; ++py) {
                for (std::uint32_t px = px0; px < px1; ++px) {
                    const TileCoord c = g.tileAt(px, py);
                    const bool inside =
                        c.tx >= got.x0 && c.tx < got.x1 && c.ty >= got.y0 && c.ty < got.y1;
                    if (!inside) {
                        FAIL_CHECK("pixel " << px << "," << py << " outside the covered range");
                        break;
                    }
                }
            }
        }
    }
}

TEST_CASE("property: a set built from rects agrees with a set built tile by tile") {
    // The two ways an edit reaches the dirty set -- a pixel rect from a tool, and explicit tile
    // coords from the store -- must produce identical sets.
    const TileGrid g(423, 311);  // 7 x 5 tiles, both edges partial
    std::mt19937 rng(0x2B1CE500u);
    std::uniform_int_distribution<int> quarters(-200, 2000);

    for (int iter = 0; iter < 300; ++iter) {
        TileSet byRect{g};
        TileSet byCoord{g};
        TileSet byRange{g};
        for (int k = 0; k < 4; ++k) {
            const Rect r{quarters(rng) * 0.25, quarters(rng) * 0.25, quarters(rng) * 0.25,
                         quarters(rng) * 0.25};
            byRect.add(r);
            const TileRange tr = g.tilesCovering(r);
            byRange.add(tr);
            for (std::uint32_t ty = tr.y0; ty < tr.y1; ++ty)
                for (std::uint32_t tx = tr.x0; tx < tr.x1; ++tx)
                    byCoord.add(TileCoord{tx, ty});
        }
        CHECK(sameSet(byRect, byCoord));
        CHECK(sameSet(byRect, byRange));

        // A union of the pieces equals the set built in one go.
        TileSet united{g};
        united.unite(byRect);
        CHECK(sameSet(united, byRect));
    }
}

TEST_CASE("property: count, all and test stay consistent under random edits") {
    const TileGrid g(65 * 64 + 13, 3 * 64 + 1);  // 66 x 4 tiles: 264 tiles, a live tail word
    REQUIRE(g.tileCount() == 264);
    std::mt19937 rng(0xACED0001u);
    std::uniform_int_distribution<std::uint32_t> xs(0, g.tilesX() - 1);
    std::uniform_int_distribution<std::uint32_t> ys(0, g.tilesY() - 1);

    TileSet s{g};
    std::vector<char> shadow(static_cast<std::size_t>(g.tileCount()), 0);
    std::uint64_t live = 0;
    for (int iter = 0; iter < 5000; ++iter) {
        const TileCoord c{xs(rng), ys(rng)};
        s.add(c);
        char& slot = shadow[static_cast<std::size_t>(g.index(c))];
        if (slot == 0) {
            slot = 1;
            ++live;
        }
        REQUIRE(s.count() == live);
        REQUIRE(s.test(c));
    }
    CHECK(s.all() == (live == g.tileCount()));
    CHECK(collect(s).size() == s.count());
    for (std::uint32_t ty = 0; ty < g.tilesY(); ++ty)
        for (std::uint32_t tx = 0; tx < g.tilesX(); ++tx)
            REQUIRE(s.test({tx, ty}) == (shadow[static_cast<std::size_t>(g.index({tx, ty}))] != 0));

    s.addAll();
    CHECK(s.count() == 264);
    CHECK(s.all());
    CHECK(collect(s).size() == 264);
}

// ---------------------------------------------------------------------------------------------
// TileKey -- the store's key layout, as a value type
// ---------------------------------------------------------------------------------------------

TEST_CASE("TileKey compares on all three fields") {
    // The compositor hands these straight to the journal (docs/mosaic-native-format.md 2.2:
    // layer_id(u64) + tx(u32) + ty(u32)), so two keys differing anywhere must not compare equal.
    const TileKey a{7, 1, 2};
    CHECK(a == TileKey{7, 1, 2});
    CHECK_FALSE(a == TileKey{8, 1, 2});
    CHECK_FALSE(a == TileKey{7, 2, 2});
    CHECK_FALSE(a == TileKey{7, 1, 3});
    CHECK(TileKey{}.layer == mosaic::core::kInvalidLayerId);
}

TEST_CASE("TileRange reports its own extent") {
    CHECK(TileRange{}.empty());
    CHECK(TileRange{}.count() == 0);
    CHECK(TileRange{2, 2, 2, 5}.empty());  // x1 == x0
    CHECK(TileRange{2, 5, 5, 2}.empty());  // y1 < y0
    CHECK(TileRange{2, 5, 5, 2}.width() == 0);
    CHECK(TileRange{2, 5, 5, 2}.height() == 0);
    const TileRange r{1, 2, 4, 8};
    CHECK_FALSE(r.empty());
    CHECK(r.width() == 3);
    CHECK(r.height() == 6);
    CHECK(r.count() == 18);
    CHECK(r == TileRange{1, 2, 4, 8});
    // A full u32 range must not wrap the count.
    CHECK(TileRange{0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu}.count() == 0xFFFFFFFFull * 0xFFFFFFFFull);
}
