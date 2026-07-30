#include <doctest/doctest.h>

#include "core/brush/bitmap_tip.hpp"
#include "core/brush/dab_cache.hpp"
#include "core/brush/dab_mask.hpp"
#include "core/brush/mask_generator.hpp"

#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <utility>
#include <vector>

using mosaic::core::brush::BitmapTip;
using mosaic::core::brush::DabKey;
using mosaic::core::brush::DabKeyHash;
using mosaic::core::brush::DabMask;
using mosaic::core::brush::DabMaskCache;
using mosaic::core::brush::DabQuantization;
using mosaic::core::brush::DabRequest;
using mosaic::core::brush::DabShape;
using mosaic::core::brush::dabShapeFromKey;
using mosaic::core::brush::MaskFalloff;
using mosaic::core::brush::MaskGenerator;
using mosaic::core::brush::MaskGeneratorParams;
using mosaic::core::brush::MaskShape;
using mosaic::core::brush::QuantizedDab;
using mosaic::core::brush::quantizeDab;
using mosaic::core::brush::renderDabMask;
using mosaic::core::brush::TipApplication;
using mosaic::core::brush::TipFrame;
using mosaic::core::brush::TipSourceKind;

namespace {

constexpr double kPi = 3.14159265358979323846;

// The renderer the cache is handed: it draws the shape the KEY names, never the raw request.
[[nodiscard]] DabMask renderFromKey(const DabKey& key, const DabQuantization& q, double subX,
                                    double subY) {
    const auto shape = dabShapeFromKey(key, q);
    MaskGeneratorParams p;
    p.shape = MaskShape::Circle;
    p.falloff = MaskFalloff::Default;
    p.diameter = shape.width;
    p.ratio = shape.width > 0.0 ? shape.height / shape.width : 0.0;
    p.softness = mosaic::core::brush::dabSoftnessFromKey(key, q);
    const MaskGenerator gen{p};
    return renderDabMask(gen, shape.angleRad, shape.mirrorH, shape.mirrorV, subX, subY);
}

// A cheap deterministic sequence, so a failure is reproducible.
struct Lcg {
    std::uint64_t s = 0x2545F4914F6CDD1Dull;
    double next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>((s >> 11) & ((1ull << 53) - 1)) / static_cast<double>(1ull << 53);
    }
};

[[nodiscard]] std::vector<DabRequest> randomRequests(int n) {
    Lcg r;
    std::vector<DabRequest> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        DabRequest q;
        q.tipId = static_cast<std::uint64_t>(i % 3);
        q.centerX = r.next() * 200.0;
        q.centerY = r.next() * 200.0;
        q.width = 2.0 + r.next() * 40.0;
        q.height = q.width * (0.2 + r.next() * 0.8);
        q.angleRad = (r.next() - 0.5) * 8.0;
        q.softness = 0.2 + r.next() * 0.8;
        q.frame = static_cast<int>(r.next() * 4);
        q.mirrorH = r.next() > 0.5;
        q.mirrorV = r.next() > 0.5;
        out.push_back(q);
    }
    return out;
}

} // namespace

TEST_CASE("a key round-trips through its shape, and the placement agrees with the render") {
    const DabQuantization q;
    for (const DabRequest& req : randomRequests(300)) {
        const QuantizedDab d = quantizeDab(req, q);
        if (d.empty())
            continue;
        const DabMask m = renderFromKey(d.key, q, d.placement.subX, d.placement.subY);
        // The blit trusts these to agree. They do, because the placement is computed from the shape
        // the key names -- not from the request.
        CHECK(m.width == d.placement.width);
        CHECK(m.height == d.placement.height);
        CHECK(m.coverage.size() == static_cast<std::size_t>(m.width) * m.height);
        // And the phase in the key is the phase the placement reports.
        CHECK(d.key.subX == static_cast<std::uint8_t>(std::lround(d.placement.subX * q.subPixelSteps)));
    }
}

TEST_CASE("the cache is exactly transparent at every capacity") {
    // The property the whole design rests on: a hit and a miss return the same bytes, so a stroke
    // never looks different because memory was tight. Capacity 1 forces a miss on nearly every dab;
    // capacity 0 disables retention entirely.
    const DabQuantization q;
    const std::vector<DabRequest> reqs = randomRequests(200);

    std::vector<DabMask> reference;
    reference.reserve(reqs.size());
    for (const DabRequest& r : reqs) {
        const QuantizedDab d = quantizeDab(r, q);
        reference.push_back(renderFromKey(d.key, q, d.placement.subX, d.placement.subY));
    }

    for (std::size_t cap : {std::size_t{0}, std::size_t{1}, std::size_t{4}, std::size_t{10000}}) {
        DabMaskCache cache{DabMaskCache::Limits{cap, 64u << 20}};
        for (std::size_t i = 0; i < reqs.size(); ++i) {
            const QuantizedDab d = quantizeDab(reqs[i], q);
            const auto mask = cache.get(d.key, [&] {
                return renderFromKey(d.key, q, d.placement.subX, d.placement.subY);
            });
            CAPTURE(cap);
            CAPTURE(i);
            REQUIRE(mask != nullptr);
            CHECK(mask->width == reference[i].width);
            CHECK(mask->height == reference[i].height);
            CHECK(mask->coverage == reference[i].coverage);
        }
        CHECK(cache.hits() + cache.misses() == reqs.size());
        if (cap == 0)
            CHECK(cache.entries() == 0);
    }
}

TEST_CASE("one cache never confuses two dabs that differ in a raster-affecting field") {
    // The transparency test above renders its reference FROM the key, so it cannot catch a key that
    // omits a field the raster depends on -- both dabs would collide onto one entry and agree. This
    // one goes through a single cache and demands the two masks differ, so a dropped field is a
    // returned-wrong-mask, which is what it would be in a stroke.
    const DabQuantization q;

    // An L-shaped bitmap tip: asymmetric about BOTH axes, so either mirror is observable. (A spiked
    // procedural tip will not do -- the generators fold about the x axis, so a vertical mirror is the
    // identity on them, which is a fact about the tip and not about the key.)
    TipFrame f;
    f.width = f.height = 16;
    f.rgba.assign(16 * 16 * 4, 255); // white == no paint
    for (std::size_t i = 0; i < 256; ++i)
        f.rgba[i * 4 + 3] = 255;
    const auto ink = [&](std::uint32_t x, std::uint32_t y) {
        const std::size_t i = (static_cast<std::size_t>(y) * 16 + x) * 4;
        f.rgba[i] = f.rgba[i + 1] = f.rgba[i + 2] = 0;
    };
    for (std::uint32_t y = 2; y < 12; ++y)
        ink(3, y);
    for (std::uint32_t x = 3; x < 10; ++x)
        ink(x, 11);
    const BitmapTip lTip{{f}, TipApplication::AlphaMask, TipSourceKind::Mask};

    const auto bitmapMaskFor = [&](DabMaskCache& cache, const DabRequest& r) {
        const QuantizedDab d = quantizeDab(r, q);
        // The renderer honours the REQUEST's mirrors, not the key's: if the key lost one, the cache
        // hands back the first dab's mask and the comparison below fails.
        return cache.get(d.key, [&] {
            DabShape s = dabShapeFromKey(d.key, q);
            s.mirrorH = r.mirrorH;
            s.mirrorV = r.mirrorV;
            return renderDabMask(lTip, 0, s, d.placement.subX, d.placement.subY);
        });
    };

    DabRequest base;
    base.width = 16.0;
    base.height = 16.0;
    base.softness = 1.0;

    SUBCASE("mirrorH") {
        DabMaskCache cache;
        DabRequest b = base;
        b.mirrorH = true;
        const auto a = bitmapMaskFor(cache, base);
        const auto m = bitmapMaskFor(cache, b);
        CHECK(cache.misses() == 2); // two keys, not one
        CHECK(a->coverage != m->coverage);
    }
    SUBCASE("mirrorV") {
        DabMaskCache cache;
        DabRequest b = base;
        b.mirrorV = true;
        const auto a = bitmapMaskFor(cache, base);
        const auto m = bitmapMaskFor(cache, b);
        CHECK(cache.misses() == 2);
        CHECK(a->coverage != m->coverage);
    }
    SUBCASE("softness") {
        DabMaskCache cache;
        DabRequest b = base;
        b.softness = 0.3;
        const auto maskFor = [&](const DabRequest& r) {
            const QuantizedDab d = quantizeDab(r, q);
            return cache.get(d.key, [&] {
                const auto shape = dabShapeFromKey(d.key, q);
                MaskGeneratorParams p;
                p.shape = MaskShape::Circle;
                p.falloff = MaskFalloff::Default;
                p.diameter = shape.width;
                p.ratio = 1.0;
                p.softness = r.softness; // the request's, so a key that drops it collides
                const MaskGenerator gen{p};
                return renderDabMask(gen, shape.angleRad, false, false, d.placement.subX,
                                     d.placement.subY);
            });
        };
        const auto a = maskFor(base);
        const auto m = maskFor(b);
        CHECK(cache.misses() == 2);
        CHECK(a->coverage != m->coverage);
    }
}

TEST_CASE("repeating a dab costs one render") {
    const DabQuantization q;
    DabMaskCache cache;
    DabRequest r;
    r.width = 24.0;
    r.height = 24.0;
    r.centerX = 10.0;
    r.centerY = 10.0;

    int rendered = 0;
    for (int i = 0; i < 50; ++i) {
        const QuantizedDab d = quantizeDab(r, q);
        (void)cache.get(d.key, [&] {
            ++rendered;
            return renderFromKey(d.key, q, d.placement.subX, d.placement.subY);
        });
    }
    CHECK(rendered == 1);
    CHECK(cache.misses() == 1);
    CHECK(cache.hits() == 49);
}

TEST_CASE("a straight stroke has only subPixelSteps^2 distinct masks") {
    // Why quantizing the phase is what buys the hit rate: a constant-size, constant-angle stroke can
    // land anywhere, but only 4x4 phases exist, so 400 dabs cost at most 16 renders.
    const DabQuantization q; // subPixelSteps = 4
    DabMaskCache cache;
    int rendered = 0;
    for (int i = 0; i < 400; ++i) {
        DabRequest r;
        r.width = 24.0;
        r.height = 24.0;
        r.centerX = 5.0 + i * 0.37; // an irrational-ish step, so every phase is visited
        r.centerY = 5.0 + i * 0.11;
        const QuantizedDab d = quantizeDab(r, q);
        (void)cache.get(d.key, [&] {
            ++rendered;
            return renderFromKey(d.key, q, d.placement.subX, d.placement.subY);
        });
    }
    CHECK(rendered <= q.subPixelSteps * q.subPixelSteps);
    CHECK(rendered > 1);
    CHECK(cache.hits() >= 384);
}

TEST_CASE("subPixelSteps = 1 collapses the phase entirely") {
    DabQuantization q;
    q.subPixelSteps = 1;
    DabMaskCache cache;
    int rendered = 0;
    for (int i = 0; i < 50; ++i) {
        DabRequest r;
        r.width = 24.0;
        r.height = 24.0;
        r.centerX = 5.0 + i * 0.37;
        const QuantizedDab d = quantizeDab(r, q);
        CHECK(d.key.subX == 0);
        CHECK(d.key.subY == 0);
        CHECK(d.placement.subX == 0.0);
        (void)cache.get(d.key, [&] {
            ++rendered;
            return renderFromKey(d.key, q, d.placement.subX, d.placement.subY);
        });
    }
    CHECK(rendered == 1);
}

TEST_CASE("distinct dab parameters never share a key") {
    const DabQuantization q;
    DabRequest base;
    base.width = 24.0;
    base.height = 18.0;
    base.angleRad = 0.5;
    base.softness = 0.5;

    std::set<std::vector<std::int64_t>> seen;
    const auto record = [&](const DabRequest& r) {
        const DabKey k = quantizeDab(r, q).key;
        std::vector<std::int64_t> v{static_cast<std::int64_t>(k.tipId), k.width, k.height,
                                    k.angle, k.softness, k.frame,
                                    k.subX, k.subY, k.mirrorH, k.mirrorV};
        CHECK(seen.insert(v).second); // no two of the variations below may collide
    };
    record(base);
    for (auto mutate : std::vector<std::function<void(DabRequest&)>>{
             [](DabRequest& r) { r.tipId = 7; },
             [](DabRequest& r) { r.width += 1.0; },
             [](DabRequest& r) { r.height += 1.0; },
             [](DabRequest& r) { r.angleRad += 0.2; },
             [](DabRequest& r) { r.softness += 0.2; },
             [](DabRequest& r) { r.frame = 3; },
             [](DabRequest& r) { r.mirrorH = true; },
             [](DabRequest& r) { r.mirrorV = true; },
             [](DabRequest& r) { r.centerX += 0.5; }, // shifts the phase, not the position
         }) {
        DabRequest r = base;
        mutate(r);
        record(r);
    }
}

TEST_CASE("position alone never changes the key -- only the phase does") {
    const DabQuantization q;
    DabRequest a;
    a.width = 24.0;
    a.height = 24.0;
    a.centerX = 10.0;
    a.centerY = 10.0;
    DabRequest b = a;
    b.centerX = 910.0; // whole pixels away: the same phase, so the same mask
    b.centerY = 610.0;
    const QuantizedDab da = quantizeDab(a, q);
    const QuantizedDab db = quantizeDab(b, q);
    CHECK(da.key == db.key);
    CHECK(da.placement.x != db.placement.x); // but they land in different places
    CHECK(db.placement.x - da.placement.x == 900);
}

TEST_CASE("eviction is least-recently-used") {
    const DabQuantization q;
    DabMaskCache cache{DabMaskCache::Limits{2, 64u << 20}};
    const auto key = [&](double w) {
        DabRequest r;
        r.width = w;
        r.height = w;
        return quantizeDab(r, q);
    };
    const auto touch = [&](const QuantizedDab& d) {
        return cache.get(d.key, [&] { return renderFromKey(d.key, q, d.placement.subX, d.placement.subY); });
    };
    const QuantizedDab a = key(10.0);
    const QuantizedDab b = key(20.0);
    const QuantizedDab c = key(30.0);

    touch(a);
    touch(b);
    touch(a); // a is now the most recent; b is the LRU victim
    CHECK(cache.entries() == 2);
    touch(c);
    CHECK(cache.entries() == 2);
    CHECK(cache.evictions() == 1);

    const std::size_t missesBefore = cache.misses();
    touch(a); // still resident
    CHECK(cache.misses() == missesBefore);
    touch(b); // evicted: a fresh miss
    CHECK(cache.misses() == missesBefore + 1);
}

TEST_CASE("a mask too large for the byte budget is returned but not retained") {
    const DabQuantization q;
    DabMaskCache cache{DabMaskCache::Limits{64, 1024}}; // 1 KiB
    DabRequest r;
    r.width = 200.0;
    r.height = 200.0;
    const QuantizedDab d = quantizeDab(r, q);
    const auto mask = cache.get(d.key, [&] { return renderFromKey(d.key, q, d.placement.subX, d.placement.subY); });
    REQUIRE(mask != nullptr);
    CHECK(mask->coverage.size() > 1024);
    CHECK(cache.entries() == 0); // rendered, handed over, not kept
    CHECK(cache.bytes() == 0);
    CHECK(mask->at(100, 100) == 255); // and the caller's copy is alive and correct
}

TEST_CASE("the byte budget is honoured across many entries") {
    const DabQuantization q;
    DabMaskCache cache{DabMaskCache::Limits{1000, 20000}};
    for (int i = 0; i < 60; ++i) {
        DabRequest r;
        r.width = 10.0 + i;
        r.height = 10.0 + i;
        const QuantizedDab d = quantizeDab(r, q);
        (void)cache.get(
            d.key, [&] { return renderFromKey(d.key, q, d.placement.subX, d.placement.subY); });
        CHECK(cache.bytes() <= cache.limits().maxBytes);
        CHECK(cache.entries() <= cache.limits().maxEntries);
    }
    CHECK(cache.evictions() > 0);
}

TEST_CASE("a retained mask survives its own eviction") {
    const DabQuantization q;
    DabMaskCache cache{DabMaskCache::Limits{1, 64u << 20}};
    DabRequest r;
    r.width = 16.0;
    r.height = 16.0;
    const QuantizedDab d = quantizeDab(r, q);
    const auto held = cache.get(d.key, [&] { return renderFromKey(d.key, q, d.placement.subX, d.placement.subY); });

    DabRequest r2 = r;
    r2.width = 32.0;
    const QuantizedDab d2 = quantizeDab(r2, q);
    (void)cache.get(
        d2.key, [&] { return renderFromKey(d2.key, q, d2.placement.subX, d2.placement.subY); });
    CHECK(cache.evictions() == 1);
    // The first mask was evicted from the cache while the caller still held it.
    CHECK(held->width == 16);
    CHECK(held->at(8, 8) == 255);
}

TEST_CASE("clear() empties the cache without disturbing its counters' meaning") {
    const DabQuantization q;
    DabMaskCache cache;
    DabRequest r;
    r.width = 12.0;
    r.height = 12.0;
    const QuantizedDab d = quantizeDab(r, q);
    (void)cache.get(
        d.key, [&] { return renderFromKey(d.key, q, d.placement.subX, d.placement.subY); });
    CHECK(cache.entries() == 1);
    cache.clear();
    CHECK(cache.entries() == 0);
    CHECK(cache.bytes() == 0);
    (void)cache.get(
        d.key, [&] { return renderFromKey(d.key, q, d.placement.subX, d.placement.subY); });
    CHECK(cache.misses() == 2); // it really was gone
}

TEST_CASE("a hostile request quantizes to an empty dab rather than a huge one") {
    const DabQuantization q;
    for (double w : {0.0, -5.0, std::nan(""), std::numeric_limits<double>::infinity(), 1e300}) {
        DabRequest r;
        r.width = w;
        r.height = 24.0;
        CAPTURE(w);
        const QuantizedDab d = quantizeDab(r, q);
        CHECK(d.empty());
        CHECK(d.placement.width == 0);
    }
    // A non-finite centre is equally refused.
    DabRequest r;
    r.centerX = std::nan("");
    CHECK(quantizeDab(r, q).empty());
    // And a non-finite angle wraps to zero rather than poisoning the key.
    DabRequest a;
    a.angleRad = std::nan("");
    CHECK(quantizeDab(a, q).key.angle == 0);
}

TEST_CASE("a sub-pixel precision finer than a byte can hold is capped, not wrapped") {
    // Regression: the phase bin lives in a uint8. `subPixelSteps = 300` would put bin 256 back at 0,
    // colliding a near-whole-pixel phase with a zero phase -- and then handing the wrong one back on
    // the hit. The step count is capped so the bin always fits, and placeDab is told the same cap.
    using mosaic::core::brush::effectiveSubPixelSteps;
    using mosaic::core::brush::kMaxSubPixelSteps;
    CHECK(effectiveSubPixelSteps(DabQuantization{1.0 / 16.0, 1024, 1.0 / 256.0, 4}) == 4);
    CHECK(effectiveSubPixelSteps(DabQuantization{1.0 / 16.0, 1024, 1.0 / 256.0, 0}) == 1);
    CHECK(effectiveSubPixelSteps(DabQuantization{1.0 / 16.0, 1024, 1.0 / 256.0, -7}) == 1);
    CHECK(effectiveSubPixelSteps(DabQuantization{1.0 / 16.0, 1024, 1.0 / 256.0, 300}) ==
          kMaxSubPixelSteps);

    DabQuantization q;
    q.subPixelSteps = 4096;
    std::set<std::pair<int, int>> bins;
    for (int i = 0; i < 500; ++i) {
        DabRequest r;
        r.width = 24.0;
        r.height = 24.0;
        r.centerX = i * 0.013;
        const QuantizedDab d = quantizeDab(r, q);
        // The key's bin and the placement's phase must still describe the same shift, and the bin
        // must be a legal byte.
        CHECK(d.key.subX == static_cast<std::uint8_t>(std::lround(d.placement.subX * kMaxSubPixelSteps)));
        CHECK(d.placement.subX < 1.0);
        bins.insert({d.key.subX, d.key.subY});
    }
    CHECK(bins.size() > 1);
    CHECK(bins.size() <= static_cast<std::size_t>(kMaxSubPixelSteps));
}

TEST_CASE("a frame index is kept verbatim, so no two frames share a mask") {
    const DabQuantization q;
    DabRequest a;
    a.width = a.height = 16.0;
    a.frame = 65535;
    DabRequest b = a;
    b.frame = 65536; // would collide if the key narrowed the frame to 16 bits
    DabRequest c = a;
    c.frame = 70000;
    CHECK_FALSE(quantizeDab(a, q).key == quantizeDab(b, q).key);
    CHECK_FALSE(quantizeDab(b, q).key == quantizeDab(c, q).key);
    // And a negative frame is its own key rather than folding onto zero.
    DabRequest n = a;
    n.frame = -1;
    DabRequest z = a;
    z.frame = 0;
    CHECK_FALSE(quantizeDab(n, q).key == quantizeDab(z, q).key);
}

TEST_CASE("the cache owns its quantization, and changing it empties the cache") {
    // A key is a tuple of step counts: it means nothing without the quantization that minted it.
    // Under a different one the same counts decode to a different shape, so residents must go.
    DabMaskCache cache;
    DabRequest r;
    r.width = 24.0;
    r.height = 24.0;
    const QuantizedDab d = cache.quantize(r);
    CHECK(d.key == quantizeDab(r, cache.quantization()).key);
    (void)cache.get(d.key, [&] {
        return renderFromKey(d.key, cache.quantization(), d.placement.subX, d.placement.subY);
    });
    CHECK(cache.entries() == 1);

    DabQuantization coarse;
    coarse.angleSteps = 64;
    coarse.sizeStep = 1.0;
    cache.setQuantization(coarse);
    CHECK(cache.entries() == 0); // otherwise the next dab would be served last precision's geometry
    CHECK(cache.quantization().angleSteps == 64);

    // And the shape the new quantization decodes really is different.
    const QuantizedDab d2 = cache.quantize(r);
    CHECK(d2.shape.width == doctest::Approx(24.0));
    DabRequest odd = r;
    odd.width = 24.4;
    CHECK(cache.quantize(odd).shape.width == doctest::Approx(24.0)); // 1 px steps now
}

TEST_CASE("an angle of a full turn is the same key as no turn") {
    const DabQuantization q;
    DabRequest a;
    a.width = 24.0;
    a.height = 12.0;
    DabRequest b = a;
    b.angleRad = 2.0 * kPi;
    DabRequest c = a;
    c.angleRad = -2.0 * kPi;
    CHECK(quantizeDab(a, q).key == quantizeDab(b, q).key);
    CHECK(quantizeDab(a, q).key == quantizeDab(c, q).key);
    for (const DabRequest& r : randomRequests(200))
        CHECK(quantizeDab(r, q).key.angle < q.angleSteps); // the bin is always in range
}

TEST_CASE("the cache serves a bitmap hose, keyed on the frame") {
    std::vector<TipFrame> frames;
    for (int f = 0; f < 3; ++f) {
        TipFrame t;
        t.width = t.height = 8;
        t.rgba.assign(8 * 8 * 4, 255);
        for (std::size_t i = 0; i < 64; ++i)
            t.rgba[i * 4] = t.rgba[i * 4 + 1] = t.rgba[i * 4 + 2] =
                static_cast<std::uint8_t>(f * 60); // each cell a different grey
        frames.push_back(t);
    }
    const BitmapTip tip{std::move(frames), TipApplication::AlphaMask, TipSourceKind::Mask};
    const DabQuantization q;
    DabMaskCache cache;

    int rendered = 0;
    const auto stamp = [&](int frame) {
        DabRequest r;
        r.tipId = 42;
        r.frame = frame;
        r.width = 8.0;
        r.height = 8.0;
        const QuantizedDab d = quantizeDab(r, q);
        return cache.get(d.key, [&] {
            ++rendered;
            return renderDabMask(tip, frame, d.shape, d.placement.subX, d.placement.subY);
        });
    };
    // Three cells, stamped round-robin: three renders, then all hits.
    for (int i = 0; i < 30; ++i)
        stamp(i % 3);
    CHECK(rendered == 3);
    CHECK(cache.entries() == 3);
    // And the cells really are different masks -- the frame is part of the key, not a detail.
    CHECK(stamp(0)->at(4, 4) != stamp(1)->at(4, 4));
    CHECK(stamp(1)->at(4, 4) != stamp(2)->at(4, 4));
}
