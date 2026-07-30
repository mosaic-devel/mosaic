#pragma once

#include <cstdint>

// Deterministic triangular-PDF (TPDF) dither in [-1, 1], a pure function of a pixel key and
// channel -- parallelism- and window-crop-exact wherever the caller derives the key from stable
// coordinates. Adding ~1 LSB of it to an 8-bit-encoded value before quantisation breaks visible
// banding on shallow gradients with no perceptible noise; two hashed uniforms summed give the
// TPDF that a good dither wants (flat noise + no distortion). Extracted from the S55 sky
// renderer (its banding fix, golden-blessed) so the layer-effects shading ramps and the gradient
// editor's ramp strip quantise through the SAME formula.
namespace mosaic::common {

[[nodiscard]] inline double ditherTPDF(std::uint32_t px, std::uint32_t py, int ch) noexcept {
    const auto mix = [](std::uint64_t v) {
        v ^= v >> 30;
        v *= 0xbf58476d1ce4e5b9ULL;
        v ^= v >> 27;
        v *= 0x94d049bb133111ebULL;
        v ^= v >> 31;
        return static_cast<double>(v >> 11) * (1.0 / 9007199254740992.0);  // [0, 1)
    };
    const std::uint64_t key = (static_cast<std::uint64_t>(px) << 32) ^
                              (static_cast<std::uint64_t>(py) << 3) ^
                              static_cast<std::uint64_t>(ch);
    return mix(key * 2 + 1) + mix(key * 2 + 2) - 1.0;
}

}  // namespace mosaic::common
