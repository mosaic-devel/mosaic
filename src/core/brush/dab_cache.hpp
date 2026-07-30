#pragma once

#include "core/brush/dab_mask.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>
#include <utility>

// The dab-mask LRU (docs/brushes.md §6.2). Without it every dab pays for the mask-generator's
// trigonometry, or a bitmap tip's resample, over the dab's whole area -- per dab, thousands of times
// a stroke.
//
// **The cache is exactly transparent, and that is the point.** A cached mask is never an
// approximation of an uncached one: the dab's continuous parameters are quantized FIRST, and the mask
// is then rendered from the quantized values. So a hit and a miss return the same bytes, and turning
// the cache off (capacity 0) changes performance and nothing else. Anything softer than that would
// make a stroke's appearance depend on how much memory happened to be free, which is not a thing a
// painting program may do.
//
// Quantization is therefore a FIDELITY decision, taken once, in `DabQuantization` -- not a caching
// detail. It buys the hit rate: a stroke at constant size and angle has only `subPixelSteps^2`
// distinct masks, however many dabs it lays.
//
// FLTK-, Vulkan- and platform-free. The cache knows nothing about tip kinds: the caller hands it a
// closure that renders the shape the key names, so the procedural and bitmap paths share it.
namespace mosaic::core::brush {

// `DabKey::subX`/`subY` are one byte, so a phase finer than this cannot be represented. A quarter of
// a pixel is the default; 256 bins would already be a 512th, far beyond what an 8-bit mask shows.
inline constexpr int kMaxSubPixelSteps = 256;

// How finely each continuous dab parameter is resolved. The defaults resolve size to a sixteenth of
// a pixel, angle to about a third of a degree, and position to a quarter pixel -- all well under what
// an 8-bit mask can express -- while collapsing a whole stroke's worth of dabs onto a handful of keys.
struct DabQuantization {
    double sizeStep = 1.0 / 16.0;     // px
    int angleSteps = 1024;            // bins in a full turn
    double softnessStep = 1.0 / 256.0;
    int subPixelSteps = 4; // per axis; 1 disables sub-pixel placement, as precision level 3 does
};

// The `subPixelSteps` actually used: clamped into [1, kMaxSubPixelSteps]. `quantizeDab` and the
// `placeDab` it calls must agree on this, or the key's phase bin and the placement's phase disagree.
[[nodiscard]] int effectiveSubPixelSteps(const DabQuantization& q) noexcept;

// A dab as the engine asks for it: continuous, unquantized.
struct DabRequest {
    // Identifies the tip's RASTER, not the tip object: bump it whenever anything that changes a
    // rendered mask changes -- the frames themselves, the brush application, or the brightness /
    // contrast / midpoint adjustments. Those are baked into a `BitmapTip`'s coverage planes at
    // construction and so are invisible to this key. A tip edited in place while its id stays fixed
    // will be painted from stale masks for as long as they remain resident, which is the one way to
    // make this cache lie. (Constructing a fresh `BitmapTip` and giving it a fresh id is the intended
    // route; `DabMaskCache::clear()` is the blunt one.)
    std::uint64_t tipId = 0;
    double centerX = 0.0;
    double centerY = 0.0;
    double width = 24.0; // the tip's unrotated extents in document px
    double height = 24.0;
    double angleRad = 0.0;
    double softness = 1.0; // procedural tips only; a bitmap tip leaves it at its default
    int frame = 0;
    bool mirrorH = false;
    bool mirrorV = false;
};

// The quantized identity of a dab's raster. Position is absent by construction -- only the sub-pixel
// phase survives -- which is exactly why two dabs far apart on a stroke share one mask.
struct DabKey {
    std::uint64_t tipId = 0;
    std::int32_t width = 0;  // in units of `sizeStep`
    std::int32_t height = 0;
    std::int32_t angle = 0;    // in bins of a full turn
    std::int32_t softness = 0; // in units of `softnessStep`
    std::int32_t frame = 0;    // verbatim: narrowing it would alias two frames onto one mask
    std::uint8_t subX = 0;     // in bins of `subPixelSteps`, which is capped at kMaxSubPixelSteps
    std::uint8_t subY = 0;
    bool mirrorH = false;
    bool mirrorV = false;

    [[nodiscard]] bool operator==(const DabKey&) const noexcept = default;
};

struct DabKeyHash {
    [[nodiscard]] std::size_t operator()(const DabKey& k) const noexcept;
};

// A request, resolved. `shape` and `placement.subX/subY` are what the renderer MUST draw with -- they
// come from the key, not from the request -- and `placement` is where the result is blitted. Nothing
// else may be used to size the blit: `DabMask::width`/`height` are the buffer's truth.
struct QuantizedDab {
    DabKey key;
    DabShape shape;
    DabPlacement placement;

    [[nodiscard]] bool empty() const noexcept { return placement.empty(); }
};

[[nodiscard]] DabShape dabShapeFromKey(const DabKey& key, const DabQuantization& q) noexcept;
[[nodiscard]] double dabSoftnessFromKey(const DabKey& key, const DabQuantization& q) noexcept;

// Quantize a request. The sub-pixel phase is derived from the QUANTIZED shape's extent, so the key,
// the shape and the placement can never disagree about how wide the mask is.
[[nodiscard]] QuantizedDab quantizeDab(const DabRequest& req, const DabQuantization& q) noexcept;

class DabMaskCache {
public:
    struct Limits {
        std::size_t maxEntries = 64;
        std::size_t maxBytes = 16u << 20; // a 500 px dab is a quarter megabyte; bound both
    };

    DabMaskCache() = default;
    explicit DabMaskCache(Limits limits, DabQuantization q = {}) : m_quant(q), m_limits(limits) {}

    // **The cache owns its quantization.** A key is a tuple of step COUNTS: it means nothing without
    // the `DabQuantization` that produced it, and the same counts decode to a different shape under a
    // different one. Keeping the two together is what stops a precision change from serving masks
    // rendered at the old geometry -- so quantize through the cache, and changing the quantization
    // empties it.
    [[nodiscard]] QuantizedDab quantize(const DabRequest& req) const noexcept;
    [[nodiscard]] const DabQuantization& quantization() const noexcept { return m_quant; }
    void setQuantization(const DabQuantization& q) noexcept;

    // The mask for `key`, calling `render` only on a miss. `render` must draw the shape the key
    // names -- use `QuantizedDab::shape`, never the raw request -- or the cache stops being
    // transparent. The returned mask outlives any eviction; callers may hold it across dabs.
    //
    // A mask too large for the byte budget is rendered, returned, and not retained.
    //
    // Templated on the renderer rather than taking a `std::function`: this is called once per dab,
    // hit or miss, and type-erasing a closure that captures four references would heap-allocate
    // thousands of times a stroke to describe work a hit never does.
    template <class Render>
    [[nodiscard]] std::shared_ptr<const DabMask> get(const DabKey& key, Render&& render) {
        if (const auto it = m_map.find(key); it != m_map.end()) {
            ++m_hits;
            m_list.splice(m_list.begin(), m_list, it->second); // touch: move to front
            return it->second->mask;
        }
        ++m_misses;
        auto mask = std::make_shared<const DabMask>(std::forward<Render>(render)());
        const std::size_t size = mask->coverage.size();

        // Capacity 0 -- or a mask that alone blows the byte budget -- is rendered and returned but
        // never retained. The caller's shared_ptr keeps it alive for as long as the dab needs it.
        if (m_limits.maxEntries == 0 || size > m_limits.maxBytes)
            return mask;

        m_list.push_front(Entry{key, mask, size});
        m_map.emplace(key, m_list.begin());
        m_bytes += size;
        evictToFit();
        return mask;
    }

    void clear() noexcept;

    [[nodiscard]] std::size_t hits() const noexcept { return m_hits; }
    [[nodiscard]] std::size_t misses() const noexcept { return m_misses; }
    [[nodiscard]] std::size_t evictions() const noexcept { return m_evictions; }
    [[nodiscard]] std::size_t entries() const noexcept { return m_map.size(); }
    [[nodiscard]] std::size_t bytes() const noexcept { return m_bytes; }
    [[nodiscard]] const Limits& limits() const noexcept { return m_limits; }

private:
    struct Entry {
        DabKey key;
        std::shared_ptr<const DabMask> mask;
        std::size_t bytes = 0;
    };
    // Front is most-recently used. The map's values are iterators into the list, which `splice`
    // keeps valid, so a hit is a splice and a rehash-free map lookup.
    using List = std::list<Entry>;

    void evictToFit() noexcept;

    List m_list;
    std::unordered_map<DabKey, List::iterator, DabKeyHash> m_map;
    DabQuantization m_quant{};
    Limits m_limits{};
    std::size_t m_bytes = 0;
    std::size_t m_hits = 0;
    std::size_t m_misses = 0;
    std::size_t m_evictions = 0;
};

} // namespace mosaic::core::brush
