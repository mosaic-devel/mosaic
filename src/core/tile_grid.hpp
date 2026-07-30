#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "common/geometry.hpp"
#include "core/layer.hpp"

// The tile model shared by the compositor, the GPU residency layer and the `.mosaic` store
// (S60-a; docs/s60-performance-plan.md §3).
//
// ---- Why one grid, and why 64 ------------------------------------------------------------------
//
// DIRTY-TRACKING granularity is the store's tile size, `io::native::kTileSize` == 64 px, and that
// is a decision rather than a coincidence (settled with the user 2026-07-23). One `TileKey`
// vocabulary is shared by the compositor's dirty set and the autosave journal's `TILE` frames, so
// a single dirty set feeds both "recomposite this" and "write this to the journal", and an
// in-memory tile maps 1:1 onto a stored tile with no re-tiling on save. It removes an entire class
// of two-dirty-sets-disagree bugs.
//
// GPU DISPATCH granularity is deliberately NOT the same number. Per-dispatch cost (descriptor set,
// barrier, command-buffer bytes) is roughly constant, while a 64 px rgba16f tile is only 32 KB of
// work — at 64 px the fixed overhead dominates, and worst on the weak hardware this arc targets.
// So the compositor coalesces into MACROTILES of `64 << k`, with k probed per device
// (`render::GpuCaps::macrotileSize`). A dirty 64 px tile marks its containing macrotile dirty;
// the cost is recompositing some clean tiles inside a dirty macrotile, which is bounded and
// tunable by moving k. k = 0 stays reachable without a redesign if measurement ever wants it.
//
// This header is FLTK-free and Vulkan-free: it is pure geometry + set algebra, so it is fully
// unit-testable without a device, and the GPU layer builds on it rather than the other way round.

namespace mosaic::core {

// The tile edge, in pixels. MUST equal io::native::kTileSize and render::kDirtyTileSize; a test
// pins all three together, because the whole point is that they are one number.
inline constexpr std::uint32_t kTileSize = 64;

// A tile's position in its grid (tile units, not pixels).
struct TileCoord {
    std::uint32_t tx = 0;
    std::uint32_t ty = 0;
    [[nodiscard]] bool operator==(const TileCoord&) const = default;
};

// A tile's full identity, matching the `.mosaic` store's TILE key layout byte for byte
// (docs/mosaic-native-format.md §2.2: `layer_id(u64) + tx(u32) + ty(u32)`), so the compositor's
// dirty set can be handed to the journal without translation.
struct TileKey {
    LayerId layer = kInvalidLayerId;
    std::uint32_t tx = 0;
    std::uint32_t ty = 0;
    [[nodiscard]] bool operator==(const TileKey&) const = default;
};

// Half-open tile-index range [x0,x1) x [y0,y1). Empty when either axis is empty.
struct TileRange {
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    [[nodiscard]] bool empty() const noexcept { return x1 <= x0 || y1 <= y0; }
    [[nodiscard]] std::uint32_t width() const noexcept { return empty() ? 0 : x1 - x0; }
    [[nodiscard]] std::uint32_t height() const noexcept { return empty() ? 0 : y1 - y0; }
    [[nodiscard]] std::uint64_t count() const noexcept {
        return static_cast<std::uint64_t>(width()) * height();
    }
    [[nodiscard]] bool operator==(const TileRange&) const = default;
};

// A pixel grid divided into `tile`-sized tiles. The right and bottom edge tiles are PARTIAL when
// the size is not a multiple of the tile edge -- `tileBounds` clips them, so the union of every
// tile's bounds is exactly the grid, never more.
class TileGrid {
public:
    TileGrid() = default;
    TileGrid(std::uint32_t width, std::uint32_t height, std::uint32_t tile = kTileSize) noexcept;

    [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
    [[nodiscard]] std::uint32_t tileSize() const noexcept { return m_tile; }
    [[nodiscard]] std::uint32_t tilesX() const noexcept { return m_tilesX; }
    [[nodiscard]] std::uint32_t tilesY() const noexcept { return m_tilesY; }
    [[nodiscard]] std::uint64_t tileCount() const noexcept {
        return static_cast<std::uint64_t>(m_tilesX) * m_tilesY;
    }
    [[nodiscard]] bool empty() const noexcept { return m_width == 0 || m_height == 0; }

    // Row-major index of a tile, and its inverse. `index` is UB-free only for in-range coords;
    // callers holding an untrusted coord should test `contains` first.
    [[nodiscard]] std::uint64_t index(TileCoord c) const noexcept;
    [[nodiscard]] TileCoord coordOf(std::uint64_t index) const noexcept;
    [[nodiscard]] bool contains(TileCoord c) const noexcept;

    // The tile containing a pixel. Clamped into the grid, so an out-of-range pixel yields the
    // nearest edge tile rather than a coord the grid does not have.
    [[nodiscard]] TileCoord tileAt(std::uint32_t px, std::uint32_t py) const noexcept;

    // The tile's pixel bounds, CLIPPED to the grid (edge tiles are partial).
    [[nodiscard]] common::Rect tileBounds(TileCoord c) const noexcept;

    // Every tile the pixel rect touches. The rect is clamped to the grid first, so a rect that
    // runs off the canvas yields only real tiles, and a rect entirely outside yields an empty
    // range. A zero-area rect touches nothing.
    [[nodiscard]] TileRange tilesCovering(const common::Rect& r) const noexcept;

    // The pixel bounds of a whole tile range, clipped to the grid. Empty range -> empty rect.
    [[nodiscard]] common::Rect rangeBounds(TileRange r) const noexcept;

    [[nodiscard]] bool operator==(const TileGrid&) const = default;

private:
    std::uint32_t m_width = 0, m_height = 0, m_tile = kTileSize, m_tilesX = 0, m_tilesY = 0;
};

// A dense set of dirty tiles over one TileGrid.
//
// Dense on purpose: a 5000x8000 document is 79 x 125 = 9875 tiles, i.e. ~1.2 KB as a bitset, and a
// bitset makes union/iterate branch-free and allocation-free on the hot path. A sparse set would
// win only on documents where the win does not matter.
class TileSet {
public:
    TileSet() = default;
    explicit TileSet(const TileGrid& grid);

    // Re-target to a new grid. Everything is cleared: tile indices mean nothing across grids, and
    // silently reinterpreting them is exactly how a stale dirty set corrupts a composite.
    void reset(const TileGrid& grid);

    [[nodiscard]] const TileGrid& grid() const noexcept { return m_grid; }
    [[nodiscard]] bool empty() const noexcept { return m_count == 0; }
    [[nodiscard]] std::uint64_t count() const noexcept { return m_count; }
    [[nodiscard]] bool all() const noexcept { return m_count == m_grid.tileCount(); }

    void clear() noexcept;
    void addAll() noexcept;                       // the whole grid is dirty (a full recomposite)
    void add(TileCoord c) noexcept;               // out-of-range coords are ignored, not clamped
    void add(const common::Rect& pixelRect) noexcept; // every tile the rect touches
    void add(TileRange r) noexcept;
    [[nodiscard]] bool test(TileCoord c) const noexcept;

    // Set union. A no-op when the grids differ -- mixing grids is a bug, and quietly producing a
    // wrong answer would hide it; the caller resets to a common grid first.
    void unite(const TileSet& other) noexcept;

    // Visit every dirty tile in row-major order.
    void forEach(const std::function<void(TileCoord)>& fn) const;

    // The tightest pixel rect covering every dirty tile (empty when the set is empty). This is
    // what a region composite takes as its ROI.
    [[nodiscard]] common::Rect boundingRect() const noexcept;

    // ---- Macrotile projection (the GPU dispatch granularity, §3.1) ---------------------------
    //
    // Project onto a grid of `tileSize << shift` tiles: a macrotile is dirty iff any tile inside
    // it is. `shift` 0 returns an equivalent set on the same grid. The result's grid covers the
    // same pixel area, so `boundingRect` and `tileBounds` stay meaningful on it.
    [[nodiscard]] TileSet macrotiles(std::uint32_t shift) const;

private:
    TileGrid m_grid;
    std::vector<std::uint64_t> m_words; // bitset, row-major, 64 tiles per word
    std::uint64_t m_count = 0;
};

}  // namespace mosaic::core
