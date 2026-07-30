#include "core/brush/dab_cache.hpp"

#include "core/brush/math_util.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core::brush {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;

// A quantized parameter is an integer count of steps. Non-finite input quantizes to zero, which
// makes the dab degenerate rather than making the key meaningless.
[[nodiscard]] std::int32_t quantizeSteps(double value, double step, std::int32_t hi) noexcept {
    if (!std::isfinite(value) || !(step > 0.0) || value <= 0.0)
        return 0;
    const double n = std::floor(value / step + 0.5);
    if (!(n >= 1.0))
        return 0;
    return static_cast<std::int32_t>(std::min(n, static_cast<double>(hi)));
}

[[nodiscard]] int angleSteps(const DabQuantization& q) noexcept {
    return std::clamp(q.angleSteps, 1, 1 << 20);
}

// Largest step count a size may quantize to. `kMaxDabExtent` bounds the extent, and `dabExtent`
// refuses anything above it, so nothing legal can saturate here.
[[nodiscard]] std::int32_t maxSizeSteps(const DabQuantization& q) noexcept {
    const double step = q.sizeStep > 0.0 ? q.sizeStep : 1.0;
    return static_cast<std::int32_t>(std::min(kMaxDabExtent / step + 1.0, 2.0e9));
}

} // namespace

int effectiveSubPixelSteps(const DabQuantization& q) noexcept {
    return std::clamp(q.subPixelSteps, 1, kMaxSubPixelSteps);
}

std::size_t DabKeyHash::operator()(const DabKey& k) const noexcept {
    // A 64-bit mix (splitmix64's finalizer) folded over the fields. The fields are small and highly
    // correlated across a stroke -- consecutive dabs differ only in the phase -- so a cheap
    // shift-xor combiner would collide badly in exactly the case the cache exists to serve.
    const auto mix = [](std::uint64_t x) noexcept {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    };
    std::uint64_t h = mix(k.tipId);
    h = mix(h ^ static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.width)));
    h = mix(h ^ static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.height)));
    h = mix(h ^ static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.angle)));
    h = mix(h ^ static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.softness)));
    h = mix(h ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.frame)) << 16) ^
            (static_cast<std::uint64_t>(k.subX) << 8) ^ static_cast<std::uint64_t>(k.subY) ^
            (static_cast<std::uint64_t>(k.mirrorH) << 48) ^
            (static_cast<std::uint64_t>(k.mirrorV) << 49));
    return static_cast<std::size_t>(h);
}

DabShape dabShapeFromKey(const DabKey& key, const DabQuantization& q) noexcept {
    const double step = q.sizeStep > 0.0 ? q.sizeStep : 1.0;
    DabShape s;
    s.width = key.width * step;
    s.height = key.height * step;
    s.angleRad = static_cast<double>(key.angle) * kTwoPi / angleSteps(q);
    s.mirrorH = key.mirrorH;
    s.mirrorV = key.mirrorV;
    return s;
}

double dabSoftnessFromKey(const DabKey& key, const DabQuantization& q) noexcept {
    const double step = q.softnessStep > 0.0 ? q.softnessStep : 1.0;
    return key.softness * step;
}

QuantizedDab quantizeDab(const DabRequest& req, const DabQuantization& q) noexcept {
    QuantizedDab out;
    const std::int32_t hi = maxSizeSteps(q);
    out.key.tipId = req.tipId;
    out.key.width = quantizeSteps(req.width, q.sizeStep, hi);
    out.key.height = quantizeSteps(req.height, q.sizeStep, hi);
    out.key.softness = quantizeSteps(req.softness, q.softnessStep, 1 << 20);
    // Verbatim. Clamping it into a narrower type would map two frames of a hose onto one key, and
    // the cache would paint the second with the first's pixels.
    out.key.frame = req.frame;
    out.key.mirrorH = req.mirrorH;
    out.key.mirrorV = req.mirrorV;

    const int bins = angleSteps(q);
    const double wrapped = detail::wrapValue(req.angleRad, 0.0, kTwoPi);
    long bin = std::lround(wrapped / kTwoPi * bins);
    if (bin >= bins || bin < 0)
        bin = 0; // a full turn is no turn; wrapValue's half-open range makes this the only case
    out.key.angle = static_cast<std::int32_t>(bin);

    // The shape comes back OUT of the key, so the renderer and the placement agree on it by
    // construction rather than by the caller's discipline.
    out.shape = dabShapeFromKey(out.key, q);
    // The SAME step count reaches placeDab and the key's phase bin. It is capped at
    // `kMaxSubPixelSteps` because the bin has to fit in a byte: a finer setting would silently fold
    // the top phase onto phase 0 and hand a mis-registered dab back on the hit.
    const int steps = effectiveSubPixelSteps(q);
    out.placement = placeDab(out.shape, req.centerX, req.centerY, steps);
    out.key.subX = static_cast<std::uint8_t>(std::lround(out.placement.subX * steps));
    out.key.subY = static_cast<std::uint8_t>(std::lround(out.placement.subY * steps));
    return out;
}

QuantizedDab DabMaskCache::quantize(const DabRequest& req) const noexcept {
    return quantizeDab(req, m_quant);
}

void DabMaskCache::setQuantization(const DabQuantization& q) noexcept {
    m_quant = q;
    // Every resident key was minted under the old quantization and decodes to the wrong geometry
    // under this one. Dropping them is the only answer that keeps the cache transparent.
    clear();
}

void DabMaskCache::evictToFit() noexcept {
    while (!m_list.empty() &&
           (m_map.size() > m_limits.maxEntries || m_bytes > m_limits.maxBytes)) {
        const Entry& victim = m_list.back();
        m_bytes -= victim.bytes;
        m_map.erase(victim.key);
        m_list.pop_back();
        ++m_evictions;
    }
}

void DabMaskCache::clear() noexcept {
    m_list.clear();
    m_map.clear();
    m_bytes = 0;
}

} // namespace mosaic::core::brush
