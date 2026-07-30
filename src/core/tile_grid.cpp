#include "core/tile_grid.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <vector>

// The tile vocabulary's implementation (S60-a; tile_grid.hpp carries the design, and
// docs/s60-performance-plan.md section 3 carries why the grid is the store's grid).
//
// Two invariants carry the whole file:
//
//   1. EVERY pixel-space answer is clipped to the grid. Edge tiles are partial, so the union of
//      `tileBounds` over every tile is exactly the grid -- never a pixel more. An unclipped edge
//      tile is precisely how a dirty rect grows past the canvas and a composite walks off the end
//      of a buffer.
//   2. The bitset's TAIL WORD holds padding bits for tiles the grid does not have, and they are
//      NEVER set: `addAll` masks the tail, `add` refuses out-of-range coords, and `unite` only
//      ever ORs sets that agree about the grid. So `count`, `all` and `forEach` can trust the
//      words exactly as they stand, with no defensive re-masking on the hot path.
namespace mosaic::core {
namespace {

constexpr std::uint64_t kBitsPerWord = 64;

// ceil(a / b), computed in 64 bits so a 4-billion-pixel edge cannot wrap the `+ b - 1`.
[[nodiscard]] constexpr std::uint32_t ceilDiv(std::uint64_t a, std::uint64_t b) noexcept {
    return b == 0 ? 0u : static_cast<std::uint32_t>((a + b - 1) / b);
}

[[nodiscard]] constexpr std::size_t wordsFor(std::uint64_t bits) noexcept {
    return static_cast<std::size_t>((bits + kBitsPerWord - 1) / kBitsPerWord);
}

// Set bits [first, first + n) and return how many were NEWLY set. Whole-word writes: a fully
// dirty tile row costs one OR per 64 tiles rather than one per tile, which is the point of a
// dense set on the edit path.
[[nodiscard]] std::uint64_t setRun(std::vector<std::uint64_t>& words, std::uint64_t first,
                                  std::uint64_t n) noexcept {
    if (n == 0)
        return 0;
    const std::uint64_t last = first + n - 1;
    const std::size_t w0 = static_cast<std::size_t>(first / kBitsPerWord);
    const std::size_t w1 = static_cast<std::size_t>(last / kBitsPerWord);
    std::uint64_t added = 0;
    for (std::size_t w = w0; w <= w1 && w < words.size(); ++w) {
        std::uint64_t mask = ~std::uint64_t{0};
        if (w == w0)
            mask &= ~std::uint64_t{0} << (first % kBitsPerWord);
        if (w == w1) {
            const unsigned hi = static_cast<unsigned>(last % kBitsPerWord);
            // `1 << 64` is UB, so the full-word case is spelled out rather than computed.
            mask &= (hi == 63) ? ~std::uint64_t{0} : ((std::uint64_t{1} << (hi + 1)) - 1);
        }
        const std::uint64_t before = words[w];
        words[w] = before | mask;
        added += static_cast<std::uint64_t>(std::popcount(mask & ~before));
    }
    return added;
}

}  // namespace

// ---- TileGrid ---------------------------------------------------------------------------------

TileGrid::TileGrid(std::uint32_t width, std::uint32_t height, std::uint32_t tile) noexcept
    : m_width(width), m_height(height), m_tile(tile == 0 ? kTileSize : tile) {
    // A zero tile edge is a caller bug that would otherwise divide by zero in every accessor;
    // substituting the default keeps every invariant below true instead of trading one bug for
    // undefined behaviour.
    //
    // A zero-sized grid has NO tiles, rather than one degenerate row or column: `tileCount` has
    // to agree with `tilesCovering`, and over an empty grid that can only ever be an empty range.
    if (m_width != 0 && m_height != 0) {
        m_tilesX = ceilDiv(m_width, m_tile);
        m_tilesY = ceilDiv(m_height, m_tile);
    }
}

std::uint64_t TileGrid::index(TileCoord c) const noexcept {
    return static_cast<std::uint64_t>(c.ty) * m_tilesX + c.tx;
}

TileCoord TileGrid::coordOf(std::uint64_t index) const noexcept {
    if (m_tilesX == 0)
        return {};
    return {static_cast<std::uint32_t>(index % m_tilesX),
            static_cast<std::uint32_t>(index / m_tilesX)};
}

bool TileGrid::contains(TileCoord c) const noexcept {
    return c.tx < m_tilesX && c.ty < m_tilesY;
}

TileCoord TileGrid::tileAt(std::uint32_t px, std::uint32_t py) const noexcept {
    if (empty())
        return {};
    // Clamped, per the header: an out-of-range pixel yields the nearest edge tile rather than a
    // coord `contains` would reject.
    const std::uint32_t x = std::min(px, m_width - 1);
    const std::uint32_t y = std::min(py, m_height - 1);
    return {x / m_tile, y / m_tile};
}

common::Rect TileGrid::tileBounds(TileCoord c) const noexcept {
    if (!contains(c))
        return {};
    const std::uint64_t x0 = static_cast<std::uint64_t>(c.tx) * m_tile;
    const std::uint64_t y0 = static_cast<std::uint64_t>(c.ty) * m_tile;
    // The clip that makes the edge tiles partial, and makes the union of all tiles exactly the
    // grid. `x0 < width` holds because `contains` passed, so the extents stay positive.
    const std::uint64_t x1 = std::min<std::uint64_t>(x0 + m_tile, m_width);
    const std::uint64_t y1 = std::min<std::uint64_t>(y0 + m_tile, m_height);
    return {static_cast<double>(x0), static_cast<double>(y0), static_cast<double>(x1 - x0),
            static_cast<double>(y1 - y0)};
}

TileRange TileGrid::tilesCovering(const common::Rect& r) const noexcept {
    if (empty())
        return {};
    // A NaN edge cannot be ordered against anything, so it can neither be clamped nor converted
    // to an index (the double->uint32 conversion would be undefined). It covers nothing.
    const double rx1 = r.right();
    const double ry1 = r.bottom();
    if (std::isnan(r.x) || std::isnan(r.y) || std::isnan(rx1) || std::isnan(ry1))
        return {};

    // Clamp to the grid FIRST (the header's contract): a rect running off the canvas yields only
    // real tiles, a rect entirely outside yields an empty range, and an infinite edge simply
    // saturates at the grid. A zero- or negative-area rect fails the `<=` test and touches
    // nothing.
    const double l = std::max(r.x, 0.0);
    const double t = std::max(r.y, 0.0);
    const double rr = std::min(rx1, static_cast<double>(m_width));
    const double bb = std::min(ry1, static_cast<double>(m_height));
    if (rr <= l || bb <= t)
        return {};

    // Half-open in pixels AND in tiles: the right edge is exclusive, so `ceil` is exactly right.
    // A rect ending at 64.0 stops inside tile 0; one ending at 64.5 reaches into tile 1. Rects
    // are doubles, so this is the case that decides whether a sub-pixel brush dab dirties one
    // tile or two.
    const double ts = static_cast<double>(m_tile);
    TileRange out;
    out.x0 = std::min(static_cast<std::uint32_t>(std::floor(l / ts)), m_tilesX);
    out.y0 = std::min(static_cast<std::uint32_t>(std::floor(t / ts)), m_tilesY);
    out.x1 = std::min(static_cast<std::uint32_t>(std::ceil(rr / ts)), m_tilesX);
    out.y1 = std::min(static_cast<std::uint32_t>(std::ceil(bb / ts)), m_tilesY);
    return out;
}

common::Rect TileGrid::rangeBounds(TileRange r) const noexcept {
    const std::uint32_t x0 = std::min(r.x0, m_tilesX);
    const std::uint32_t y0 = std::min(r.y0, m_tilesY);
    const std::uint32_t x1 = std::min(r.x1, m_tilesX);
    const std::uint32_t y1 = std::min(r.y1, m_tilesY);
    if (x1 <= x0 || y1 <= y0)
        return {};
    const std::uint64_t px0 = static_cast<std::uint64_t>(x0) * m_tile;
    const std::uint64_t py0 = static_cast<std::uint64_t>(y0) * m_tile;
    const std::uint64_t px1 = std::min<std::uint64_t>(static_cast<std::uint64_t>(x1) * m_tile,
                                                     m_width);
    const std::uint64_t py1 = std::min<std::uint64_t>(static_cast<std::uint64_t>(y1) * m_tile,
                                                     m_height);
    return {static_cast<double>(px0), static_cast<double>(py0), static_cast<double>(px1 - px0),
            static_cast<double>(py1 - py0)};
}

// ---- TileSet ----------------------------------------------------------------------------------

TileSet::TileSet(const TileGrid& grid) { reset(grid); }

void TileSet::reset(const TileGrid& grid) {
    m_grid = grid;
    // Everything is cleared, deliberately: tile indices mean nothing across grids, and silently
    // reinterpreting them is exactly how a stale dirty set corrupts a composite.
    m_words.assign(wordsFor(grid.tileCount()), 0);
    m_count = 0;
}

void TileSet::clear() noexcept {
    std::fill(m_words.begin(), m_words.end(), std::uint64_t{0});
    m_count = 0;
}

void TileSet::addAll() noexcept {
    const std::uint64_t n = m_grid.tileCount();
    std::fill(m_words.begin(), m_words.end(), ~std::uint64_t{0});
    if (!m_words.empty()) {
        // Mask the tail. Padding bits belong to tiles the grid does not have; leaving them set
        // would make every popcount below -- and every `forEach` -- report tiles that do not
        // exist. When the count is an exact multiple of 64 there is no tail and the last word
        // stays full, which is why this is a condition and not an unconditional mask.
        const std::uint64_t tail = n % kBitsPerWord;
        if (tail != 0)
            m_words.back() = (std::uint64_t{1} << tail) - 1;
    }
    m_count = n;
}

void TileSet::add(TileCoord c) noexcept {
    // Ignored, not clamped: clamping would mark a real tile the caller never touched, which is a
    // silently wrong composite rather than a visibly missing one.
    if (!m_grid.contains(c))
        return;
    const std::uint64_t i = m_grid.index(c);
    std::uint64_t& w = m_words[static_cast<std::size_t>(i / kBitsPerWord)];
    const std::uint64_t bit = std::uint64_t{1} << (i % kBitsPerWord);
    if ((w & bit) == 0) {
        w |= bit;
        ++m_count;
    }
}

void TileSet::add(const common::Rect& pixelRect) noexcept {
    add(m_grid.tilesCovering(pixelRect));
}

void TileSet::add(TileRange r) noexcept {
    const std::uint32_t x0 = std::min(r.x0, m_grid.tilesX());
    const std::uint32_t y0 = std::min(r.y0, m_grid.tilesY());
    const std::uint32_t x1 = std::min(r.x1, m_grid.tilesX());
    const std::uint32_t y1 = std::min(r.y1, m_grid.tilesY());
    if (x1 <= x0 || y1 <= y0)
        return;
    for (std::uint32_t ty = y0; ty < y1; ++ty)
        m_count += setRun(m_words, m_grid.index({x0, ty}), x1 - x0);
}

bool TileSet::test(TileCoord c) const noexcept {
    if (!m_grid.contains(c))
        return false;
    const std::uint64_t i = m_grid.index(c);
    return (m_words[static_cast<std::size_t>(i / kBitsPerWord)]
            & (std::uint64_t{1} << (i % kBitsPerWord)))
           != 0;
}

void TileSet::unite(const TileSet& other) noexcept {
    // A no-op across grids, per the header: mixing grids is a bug, and ORing indices that mean
    // different things would dirty arbitrary tiles instead of surfacing the mistake. Equal grids
    // imply equal word counts, so the loop below needs no length reconciliation.
    if (!(m_grid == other.m_grid))
        return;
    for (std::size_t i = 0; i < m_words.size(); ++i) {
        const std::uint64_t before = m_words[i];
        const std::uint64_t merged = before | other.m_words[i];
        m_count += static_cast<std::uint64_t>(std::popcount(merged & ~before));
        m_words[i] = merged;
    }
}

void TileSet::forEach(const std::function<void(TileCoord)>& fn) const {
    if (!fn)
        return;
    const std::uint32_t tilesX = m_grid.tilesX();
    if (tilesX == 0)
        return;
    // Ascending words, and lowest set bit first within a word: that is row-major order, because
    // the index IS row-major.
    for (std::size_t wi = 0; wi < m_words.size(); ++wi) {
        std::uint64_t w = m_words[wi];
        while (w != 0) {
            const std::uint64_t idx =
                static_cast<std::uint64_t>(wi) * kBitsPerWord
                + static_cast<std::uint64_t>(std::countr_zero(w));
            w &= w - 1;
            fn(TileCoord{static_cast<std::uint32_t>(idx % tilesX),
                         static_cast<std::uint32_t>(idx / tilesX)});
        }
    }
}

common::Rect TileSet::boundingRect() const noexcept {
    if (m_count == 0)
        return {};
    const std::uint32_t tilesX = m_grid.tilesX();
    if (tilesX == 0)
        return {};

    // The row span falls out of the first and last set bit -- two word scans, no per-tile work.
    std::size_t firstW = 0;
    while (m_words[firstW] == 0)
        ++firstW;
    std::size_t lastW = m_words.size() - 1;
    while (m_words[lastW] == 0)
        --lastW;
    const std::uint64_t firstIdx = static_cast<std::uint64_t>(firstW) * kBitsPerWord
                                   + static_cast<std::uint64_t>(std::countr_zero(m_words[firstW]));
    const std::uint64_t lastIdx = static_cast<std::uint64_t>(lastW) * kBitsPerWord + 63
                                  - static_cast<std::uint64_t>(std::countl_zero(m_words[lastW]));

    // The column span does need every tile: one row's dirty span says nothing about its
    // neighbours'. The scan stops early once the span cannot widen further, which is the common
    // case for a full-width edit (a filter, a paste, a canvas resize).
    std::uint32_t minTx = tilesX - 1;
    std::uint32_t maxTx = 0;
    for (std::size_t wi = firstW; wi <= lastW; ++wi) {
        std::uint64_t w = m_words[wi];
        while (w != 0) {
            const std::uint64_t idx = static_cast<std::uint64_t>(wi) * kBitsPerWord
                                      + static_cast<std::uint64_t>(std::countr_zero(w));
            w &= w - 1;
            const std::uint32_t tx = static_cast<std::uint32_t>(idx % tilesX);
            minTx = std::min(minTx, tx);
            maxTx = std::max(maxTx, tx);
        }
        if (minTx == 0 && maxTx == tilesX - 1)
            break;
    }

    return m_grid.rangeBounds({minTx, static_cast<std::uint32_t>(firstIdx / tilesX), maxTx + 1,
                               static_cast<std::uint32_t>(lastIdx / tilesX) + 1});
}

TileSet TileSet::macrotiles(std::uint32_t shift) const {
    if (shift == 0)
        return *this;

    // Clamp the shift so `tileSize << shift` cannot overflow the u32 tile edge (and so the coord
    // shifts below stay defined). Anything past the clamp is already a one-tile grid for any
    // u32-sized document, so this changes no answer a real caller can observe.
    std::uint32_t s = std::min<std::uint32_t>(shift, 31);
    while (s > 0 && (static_cast<std::uint64_t>(m_grid.tileSize()) << s) > 0xFFFFFFFFull)
        --s;
    if (s == 0)
        return *this;

    // The macro grid covers the SAME pixel area with tiles 2^s times as wide, and both grids
    // share the origin -- which is what makes the projection a pure shift rather than a rounding
    // problem. ceil(ceil(w/tile) / 2^s) == ceil(w / (tile << s)), so the shifted coord is always
    // inside the macro grid; a dirty tile therefore lights exactly one macrotile, never none.
    TileSet out(TileGrid(m_grid.width(), m_grid.height(), m_grid.tileSize() << s));
    const std::uint32_t tilesX = m_grid.tilesX();
    if (tilesX == 0)
        return out;
    for (std::size_t wi = 0; wi < m_words.size(); ++wi) {
        std::uint64_t w = m_words[wi];
        while (w != 0) {
            const std::uint64_t idx =
                static_cast<std::uint64_t>(wi) * kBitsPerWord
                + static_cast<std::uint64_t>(std::countr_zero(w));
            w &= w - 1;
            out.add(TileCoord{static_cast<std::uint32_t>((idx % tilesX) >> s),
                              static_cast<std::uint32_t>((idx / tilesX) >> s)});
        }
    }
    return out;
}

}  // namespace mosaic::core
