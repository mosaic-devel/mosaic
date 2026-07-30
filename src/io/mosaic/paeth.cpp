#include "io/mosaic/paeth.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace mosaic::io::native {
namespace {

// Paeth (1991): pick whichever of a (left), b (up), c (up-left) is nearest to p = a + b - c.
// Ties resolve a, then b -- the PNG rule; both sides of this file must agree with it exactly.
[[nodiscard]] inline std::uint8_t paethPredictor(int a, int b, int c) noexcept {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc)
        return static_cast<std::uint8_t>(a);
    if (pb <= pc)
        return static_cast<std::uint8_t>(b);
    return static_cast<std::uint8_t>(c);
}

} // namespace

void filterPaethRgba(std::span<const std::uint8_t> raw, std::uint32_t w, std::uint32_t h,
                     std::span<std::uint8_t> out) {
    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    assert(raw.size() == stride * h && out.size() == raw.size());
    assert(raw.data() != out.data() && "encode must not run in place");
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t* row = raw.data() + y * stride;
        const std::uint8_t* up = (y > 0) ? row - stride : nullptr;
        std::uint8_t* dst = out.data() + y * stride;
        for (std::size_t i = 0; i < stride; ++i) {
            const int a = (i >= 4) ? row[i - 4] : 0;
            const int b = (up != nullptr) ? up[i] : 0;
            const int c = (up != nullptr && i >= 4) ? up[i - 4] : 0;
            dst[i] = static_cast<std::uint8_t>(row[i] - paethPredictor(a, b, c));
        }
    }
}

namespace detail {

void unfilterPaethRgbaScalar(std::span<std::uint8_t> data, std::uint32_t w, std::uint32_t h) {
    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    assert(data.size() == stride * h);
    for (std::uint32_t y = 0; y < h; ++y) {
        std::uint8_t* row = data.data() + y * stride;
        const std::uint8_t* up = (y > 0) ? row - stride : nullptr;
        for (std::size_t i = 0; i < stride; ++i) {
            const int a = (i >= 4) ? row[i - 4] : 0;
            const int b = (up != nullptr) ? up[i] : 0;
            const int c = (up != nullptr && i >= 4) ? up[i - 4] : 0;
            row[i] = static_cast<std::uint8_t>(row[i] + paethPredictor(a, b, c));
        }
    }
}

} // namespace detail

#if defined(__SSE2__)
namespace {

// One row of 4-byte pixels, SIMD across the pixel's channels (the libpng-shape kernel): the
// x-direction dependency is real, so the win is doing all four channels' predictor math at
// once in 16-bit lanes. Selection uses the standard Paeth identities
// pa = |p-a| = |b-c|, pb = |p-b| = |a-c|, pc = |p-c| = |(b-c)+(a-c)| and PNG's tie order via
// "b beats a only when strictly nearer; c only when strictly nearer than the survivor".
inline void unfilterRowPaethSse2(std::uint8_t* row, const std::uint8_t* up, std::uint32_t w) {
    const __m128i zero = _mm_setzero_si128();
    __m128i a16 = zero; // reconstructed left pixel, widened to u16 lanes
    __m128i c16 = zero; // reconstructed up-left pixel, widened
    for (std::uint32_t x = 0; x < w; ++x) {
        std::uint8_t* px = row + static_cast<std::size_t>(x) * 4;
        // memcpy loads/stores: input spans carry no alignment promise (a future memory-mapped
        // reader hands us arbitrary offsets), and the compiler folds these to movd anyway.
        std::int32_t bWord = 0;
        if (up != nullptr)
            std::memcpy(&bWord, up + static_cast<std::size_t>(x) * 4, 4);
        std::int32_t dWord;
        std::memcpy(&dWord, px, 4);
        const __m128i b8 = _mm_cvtsi32_si128(bWord);
        const __m128i d8 = _mm_cvtsi32_si128(dWord);
        const __m128i b16 = _mm_unpacklo_epi8(b8, zero);
        const __m128i d16 = _mm_unpacklo_epi8(d8, zero);

        const __m128i paS = _mm_sub_epi16(b16, c16); // p - a, signed
        const __m128i pbS = _mm_sub_epi16(a16, c16); // p - b, signed
        const __m128i pcS = _mm_add_epi16(paS, pbS); // p - c, signed
        const __m128i pa = _mm_max_epi16(paS, _mm_sub_epi16(zero, paS));
        const __m128i pb = _mm_max_epi16(pbS, _mm_sub_epi16(zero, pbS));
        const __m128i pc = _mm_max_epi16(pcS, _mm_sub_epi16(zero, pcS));

        // nearest = a; if pb < pa -> b; if pc < min(pa,pb) -> c. (Matches the scalar tie rule.)
        __m128i nearest = a16;
        __m128i best = pa;
        const __m128i takeB = _mm_cmplt_epi16(pb, best);
        nearest = _mm_or_si128(_mm_and_si128(takeB, b16), _mm_andnot_si128(takeB, nearest));
        best = _mm_min_epi16(best, pb);
        const __m128i takeC = _mm_cmplt_epi16(pc, best);
        nearest = _mm_or_si128(_mm_and_si128(takeC, c16), _mm_andnot_si128(takeC, nearest));

        const __m128i recon16 =
            _mm_and_si128(_mm_add_epi16(d16, nearest), _mm_set1_epi16(0xFF));
        const __m128i recon8 = _mm_packus_epi16(recon16, recon16);
        const std::int32_t out = _mm_cvtsi128_si32(recon8);
        std::memcpy(px, &out, 4);

        c16 = b16;
        a16 = recon16;
    }
}

} // namespace
#endif // __SSE2__

void unfilterPaethRgba(std::span<std::uint8_t> data, std::uint32_t w, std::uint32_t h) {
#if defined(__SSE2__)
    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    assert(data.size() == stride * h);
    for (std::uint32_t y = 0; y < h; ++y) {
        std::uint8_t* row = data.data() + y * stride;
        unfilterRowPaethSse2(row, (y > 0) ? row - stride : nullptr, w);
    }
#else
    detail::unfilterPaethRgbaScalar(data, w, h);
#endif
}

} // namespace mosaic::io::native
