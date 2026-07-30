#include "io/mosaic/reedsolomon.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <utility>

namespace mosaic::io::native {
namespace {

// GF(256) with the primitive polynomial 0x11D (x^8+x^4+x^3+x^2+1, the QR/RS textbook choice),
// log/antilog tables built once. The exp table is doubled so mul never needs a modulo.
struct Gf256 {
    std::array<std::uint8_t, 256> logT{};
    std::array<std::uint8_t, 512> expT{};

    Gf256() {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            expT[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x);
            logT[static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(i);
            x <<= 1;
            if ((x & 0x100) != 0)
                x ^= 0x11D;
        }
        for (int i = 255; i < 512; ++i)
            expT[static_cast<std::size_t>(i)] = expT[static_cast<std::size_t>(i - 255)];
    }

    [[nodiscard]] std::uint8_t mul(std::uint8_t a, std::uint8_t b) const noexcept {
        if (a == 0 || b == 0)
            return 0;
        return expT[static_cast<std::size_t>(logT[a]) + logT[b]];
    }

    [[nodiscard]] std::uint8_t inv(std::uint8_t a) const noexcept {
        assert(a != 0);
        return expT[static_cast<std::size_t>(255 - logT[a])];
    }
};

[[nodiscard]] const Gf256& gf() {
    static const Gf256 tables;
    return tables;
}

// Gauss-Jordan inversion of a k x k matrix over GF(256). nullopt on a zero pivot -- with the
// Cauchy construction any k rows of [I; C] are independent, so this is a defensive guard, not
// an expected path.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> invertMatrix(
    std::vector<std::uint8_t> a, std::size_t k) {
    const Gf256& g = gf();
    std::vector<std::uint8_t> inv(k * k, 0);
    for (std::size_t i = 0; i < k; ++i)
        inv[i * k + i] = 1;
    for (std::size_t col = 0; col < k; ++col) {
        std::size_t pivot = col;
        while (pivot < k && a[pivot * k + col] == 0)
            ++pivot;
        if (pivot == k)
            return std::nullopt;
        if (pivot != col)
            for (std::size_t j = 0; j < k; ++j) {
                std::swap(a[pivot * k + j], a[col * k + j]);
                std::swap(inv[pivot * k + j], inv[col * k + j]);
            }
        const std::uint8_t scale = g.inv(a[col * k + col]);
        for (std::size_t j = 0; j < k; ++j) {
            a[col * k + j] = g.mul(a[col * k + j], scale);
            inv[col * k + j] = g.mul(inv[col * k + j], scale);
        }
        for (std::size_t row = 0; row < k; ++row) {
            if (row == col || a[row * k + col] == 0)
                continue;
            const std::uint8_t factor = a[row * k + col];
            for (std::size_t j = 0; j < k; ++j) {
                a[row * k + j] = static_cast<std::uint8_t>(a[row * k + j] ^
                                                           g.mul(factor, a[col * k + j]));
                inv[row * k + j] = static_cast<std::uint8_t>(inv[row * k + j] ^
                                                             g.mul(factor, inv[col * k + j]));
            }
        }
    }
    return inv;
}

} // namespace

ReedSolomon::ReedSolomon(std::size_t k, std::size_t m) : m_k(k), m_m(m) {
    assert(k >= 1 && m >= 1 && k + m <= 255);
    // Cauchy points: parity rows use x_i = i, data columns use y_j = m + j -- disjoint by
    // construction, so x_i ^ y_j is never zero and every square submatrix is nonsingular
    // (the property that makes the code MDS: any k survivors suffice).
    const Gf256& g = gf();
    m_cauchy.resize(m * k);
    for (std::size_t i = 0; i < m; ++i)
        for (std::size_t j = 0; j < k; ++j)
            m_cauchy[i * k + j] =
                g.inv(static_cast<std::uint8_t>(i) ^ static_cast<std::uint8_t>(m + j));
}

std::vector<std::vector<std::uint8_t>> ReedSolomon::encodeParity(
    const std::vector<std::vector<std::uint8_t>>& data) const {
    assert(data.size() == m_k);
    const std::size_t len = data.empty() ? 0 : data[0].size();
    const Gf256& g = gf();
    std::vector<std::vector<std::uint8_t>> parity(m_m, std::vector<std::uint8_t>(len, 0));
    for (std::size_t i = 0; i < m_m; ++i)
        for (std::size_t j = 0; j < m_k; ++j) {
            assert(data[j].size() == len);
            const std::uint8_t c = m_cauchy[i * m_k + j];
            if (c == 0)
                continue;
            for (std::size_t b = 0; b < len; ++b)
                parity[i][b] = static_cast<std::uint8_t>(parity[i][b] ^ g.mul(c, data[j][b]));
        }
    return parity;
}

std::optional<std::vector<std::vector<std::uint8_t>>> ReedSolomon::reconstruct(
    const std::vector<std::optional<std::vector<std::uint8_t>>>& shards) const {
    assert(shards.size() == m_k + m_m);

    // Fast path: all data shards present -- nothing to solve.
    bool allData = true;
    for (std::size_t j = 0; j < m_k; ++j)
        allData = allData && shards[j].has_value();
    if (allData) {
        std::vector<std::vector<std::uint8_t>> out;
        out.reserve(m_k);
        for (std::size_t j = 0; j < m_k; ++j)
            out.push_back(*shards[j]);
        return out;
    }

    // Pick the first k surviving shards; fewer than k = more erasures than parity can carry:
    // declined, never guessed (spec 2.7).
    std::vector<std::size_t> rows;
    for (std::size_t r = 0; r < shards.size() && rows.size() < m_k; ++r)
        if (shards[r].has_value())
            rows.push_back(r);
    if (rows.size() < m_k)
        return std::nullopt;
    const std::size_t len = shards[rows[0]]->size();

    // Rows of the systematic generator [I_k; C] for the chosen shards; invert; apply.
    std::vector<std::uint8_t> a(m_k * m_k, 0);
    for (std::size_t r = 0; r < m_k; ++r) {
        const std::size_t shard = rows[r];
        if (shard < m_k)
            a[r * m_k + shard] = 1;
        else
            for (std::size_t j = 0; j < m_k; ++j)
                a[r * m_k + j] = m_cauchy[(shard - m_k) * m_k + j];
    }
    const auto ainv = invertMatrix(std::move(a), m_k);
    if (!ainv.has_value())
        return std::nullopt; // defensive; unreachable with the Cauchy construction

    const Gf256& g = gf();
    std::vector<std::vector<std::uint8_t>> out(m_k, std::vector<std::uint8_t>(len, 0));
    for (std::size_t j = 0; j < m_k; ++j) {
        if (shards[j].has_value()) {
            out[j] = *shards[j]; // survivors are exact; only solve for the erased
            continue;
        }
        for (std::size_t r = 0; r < m_k; ++r) {
            const std::uint8_t c = (*ainv)[j * m_k + r];
            if (c == 0)
                continue;
            const std::vector<std::uint8_t>& src = *shards[rows[r]];
            assert(src.size() == len);
            for (std::size_t b = 0; b < len; ++b)
                out[j][b] = static_cast<std::uint8_t>(out[j][b] ^ g.mul(c, src[b]));
        }
    }
    return out;
}

} // namespace mosaic::io::native
