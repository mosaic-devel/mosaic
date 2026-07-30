#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// mosaic/reedsolomon -- a from-scratch GF(256) Cauchy-matrix systematic Reed-Solomon erasure
// coder (spec 2.7). k data shards get m parity shards; any k surviving shards of the k+m
// reconstruct the data exactly, or reconstruction is honestly declined -- never approximated.
//
// KNOWN-ERASURE DECODE ONLY, by design: the directory already identifies WHICH shard is damaged
// (per-chunk checksums), so the cheap erasure path suffices and a blind error locator -- which
// would need roughly twice the redundancy for the same protection -- is deliberately absent.
//
// Provenance: the 1960 Reed-Solomon algorithm is textbook and everywhere (CDs, QR, RAID6, PAR2);
// the Cauchy systematic construction is Blomer et al. 1995. Written from the mathematics, not
// ported from any library.
namespace mosaic::io::native {

inline constexpr std::size_t kRsDataShards = 8;   // k -- spec 2.7 default stripe width
inline constexpr std::size_t kRsParityShards = 2; // m -- ~25% redundancy, PAR2's typical range

class ReedSolomon {
public:
    // Requires 1 <= k, 1 <= m, k + m <= 255 (GF(256) element budget for the Cauchy points).
    ReedSolomon(std::size_t k, std::size_t m);

    [[nodiscard]] std::size_t dataShards() const noexcept { return m_k; }
    [[nodiscard]] std::size_t parityShards() const noexcept { return m_m; }

    // data: exactly k shards, all the same length. Returns m parity shards of that length.
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> encodeParity(
        const std::vector<std::vector<std::uint8_t>>& data) const;

    // shards: exactly k+m entries in index order (k data, then m parity); nullopt = erased.
    // Returns the k data shards, exactly reconstructed -- or nullopt when fewer than k shards
    // survive (erasures exceeded m: declined, never guessed).
    [[nodiscard]] std::optional<std::vector<std::vector<std::uint8_t>>> reconstruct(
        const std::vector<std::optional<std::vector<std::uint8_t>>>& shards) const;

private:
    std::size_t m_k;
    std::size_t m_m;
    std::vector<std::uint8_t> m_cauchy; // m x k, row-major: parity_i = sum_j C[i][j] * data_j
};

} // namespace mosaic::io::native
