#pragma once

#include <cstdint>
#include <span>

// mosaic/paeth -- PNG's Paeth spatial predictor over a whole RGBA8 tile (spec 2.5): every byte
// is predicted from its left / up / up-left neighbours (same channel), whichever the linear
// predictor a+b-c lands closest to, and the residual is what gets compressed. Measured in the
// research to close most of the compression gap with plain per-layer PNG (~25-30% smaller on
// photographic content).
//
// Encode has no sequential dependency (all neighbours are raw values) and auto-vectorizes.
// Decode reconstructs left-to-right, top-to-bottom -- the genuine 2D dependency the spec warns
// about -- so it ships a hand-vectorized SSE2 lane (one pixel's 4 channels per step, the
// solved-problem shape libpng uses) with the scalar path as the portable fallback and the
// equivalence oracle for tests.
namespace mosaic::io::native {

// raw -> filtered residuals. `raw` and `out` are w*h*4 bytes, tightly packed; they must not
// alias (encode reads raw neighbours the decode side will not have yet).
void filterPaethRgba(std::span<const std::uint8_t> raw, std::uint32_t w, std::uint32_t h,
                     std::span<std::uint8_t> out);

// filtered residuals -> raw, in place (dispatches to the SSE2 lane where available).
void unfilterPaethRgba(std::span<std::uint8_t> data, std::uint32_t w, std::uint32_t h);

namespace detail {
// The portable reference decode -- public so tests can pin the SIMD lane against it.
void unfilterPaethRgbaScalar(std::span<std::uint8_t> data, std::uint32_t w, std::uint32_t h);
} // namespace detail

} // namespace mosaic::io::native
