#include "io/mosaic/reedsolomon.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <random>
#include <vector>

// The .mosaic Reed-Solomon coder (S48 Build 1, parity slice; spec 2.7): exact reconstruction
// under EVERY erasure pattern within the parity budget, honest decline beyond it, tail-stripe
// shapes, and awkward shard lengths. Known-erasure decode only, by design.
namespace {

using namespace mosaic::io::native;

std::vector<std::vector<std::uint8_t>> randomShards(std::size_t k, std::size_t len,
                                                    std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<std::vector<std::uint8_t>> shards(k, std::vector<std::uint8_t>(len));
    for (auto& s : shards)
        for (auto& b : s)
            b = static_cast<std::uint8_t>(rng());
    return shards;
}

} // namespace

TEST_CASE("mosaic rs: every erasure pattern within the budget reconstructs exactly") {
    const std::size_t k = kRsDataShards, m = kRsParityShards;
    const ReedSolomon rs(k, m);
    const auto data = randomShards(k, 1000, 42);
    const auto parity = rs.encodeParity(data);
    REQUIRE(parity.size() == m);

    std::vector<std::optional<std::vector<std::uint8_t>>> full;
    for (const auto& d : data)
        full.emplace_back(d);
    for (const auto& p : parity)
        full.emplace_back(p);

    // All single and double erasures -- every C(10,1) + C(10,2) pattern, data and parity alike.
    for (std::size_t a = 0; a < k + m; ++a) {
        for (std::size_t b = a; b < k + m; ++b) {
            auto shards = full;
            shards[a] = std::nullopt;
            shards[b] = std::nullopt; // a == b covers the single-erasure case
            const auto out = rs.reconstruct(shards);
            REQUIRE_MESSAGE(out.has_value(), "erasures at ", a, ",", b);
            for (std::size_t j = 0; j < k; ++j)
                CHECK((*out)[j] == data[j]);
        }
    }
}

TEST_CASE("mosaic rs: beyond the budget is declined, never guessed") {
    const ReedSolomon rs(kRsDataShards, kRsParityShards);
    const auto data = randomShards(kRsDataShards, 200, 7);
    const auto parity = rs.encodeParity(data);
    std::vector<std::optional<std::vector<std::uint8_t>>> shards;
    for (const auto& d : data)
        shards.emplace_back(d);
    for (const auto& p : parity)
        shards.emplace_back(p);
    shards[0] = std::nullopt;
    shards[3] = std::nullopt;
    shards[9] = std::nullopt; // three erasures, m == 2
    CHECK(!rs.reconstruct(shards).has_value());
}

TEST_CASE("mosaic rs: tail-stripe shapes and awkward lengths") {
    // Trailing checkpoint stripes have fewer than k members (spec 2.7 / buildCheckpoint);
    // every (k, m) shape the container can produce must hold the same guarantee.
    for (std::size_t k = 2; k <= 7; ++k) {
        const std::size_t m = std::min<std::size_t>(2, k);
        const ReedSolomon rs(k, m);
        for (const std::size_t len : {std::size_t{1}, std::size_t{37}, std::size_t{4096}}) {
            const auto data = randomShards(k, len, static_cast<std::uint32_t>(100 + k));
            const auto parity = rs.encodeParity(data);
            std::vector<std::optional<std::vector<std::uint8_t>>> shards;
            for (const auto& d : data)
                shards.emplace_back(d);
            for (const auto& p : parity)
                shards.emplace_back(p);
            shards[k - 1] = std::nullopt;
            if (m == 2 && k >= 2)
                shards[0] = std::nullopt;
            const auto out = rs.reconstruct(shards);
            REQUIRE(out.has_value());
            for (std::size_t j = 0; j < k; ++j)
                CHECK((*out)[j] == data[j]);
        }
    }
}
