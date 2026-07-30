#pragma once

#include "common/geometry.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mosaic::common {

// An 8-bit-per-channel RGBA color.
struct Color8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
    bool operator==(const Color8&) const = default;
};

// A float RGBA color with straight (non-premultiplied) alpha, channels nominally in [0,1].
// This is the working representation for the compositor's blend math (S7): defaulting to
// transparent (a = 0) so a freshly cleared working buffer is empty.
struct ColorF {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
    bool operator==(const ColorF&) const = default;
};

// The Color8 <-> ColorF bridge (LE-b): the UI speaks 8-bit Color8 (swatch chips, the colour
// flyout), effect/vector paints speak float ColorF. A plain per-channel /255 <-> *255 map -- the
// effect colours are authored in the SAME encoded space the compositor blends in (like the Fill
// dialog's Color8 fills), so no working-space (lcms2) transform is warranted here.
[[nodiscard]] inline ColorF toColorF(Color8 c) noexcept {
    return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
}
[[nodiscard]] inline Color8 toColor8(ColorF c) noexcept {
    const auto q = [](float v) {
        v = std::clamp(v, 0.0f, 1.0f);
        return static_cast<std::uint8_t>(v * 255.0f + 0.5f);
    };
    return {q(c.r), q(c.g), q(c.b), q(c.a)};
}

// A simple, tightly-packed 8-bit RGBA image buffer (row-major, 4 bytes per pixel).
// This is the shared CPU-side pixel container produced by the renderer and (later)
// consumed by the io module.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;  // size == width * height * 4

    Image() = default;
    Image(std::uint32_t w, std::uint32_t h)
        : width(w), height(h), rgba(static_cast<std::size_t>(w) * h * 4, 0) {}

    [[nodiscard]] std::size_t pixelCount() const noexcept {
        return static_cast<std::size_t>(width) * height;
    }
    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }

    // Fill every pixel with `c`.
    void fill(Color8 c);

    bool operator==(const Image&) const = default;
};

// A float RGBA image (straight alpha), the compositor's working buffer (PLAN §3.6). Blending
// happens in float to avoid the banding and clamping of repeated 8-bit rounds; the result is
// converted back to an 8-bit `Image` for display/export. Layer pixel storage stays 8-bit in
// the model for now -- the float/tiled migration begins here (S7) and completes in S48.
struct ImageF {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> rgba;  // size == width * height * 4

    ImageF() = default;
    ImageF(std::uint32_t w, std::uint32_t h)
        : width(w), height(h), rgba(static_cast<std::size_t>(w) * h * 4, 0.0f) {}

    [[nodiscard]] std::size_t pixelCount() const noexcept {
        return static_cast<std::size_t>(width) * height;
    }
    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }

    // Unchecked pixel access (caller guarantees x < width, y < height).
    [[nodiscard]] ColorF at(std::uint32_t x, std::uint32_t y) const noexcept {
        const std::size_t p = (static_cast<std::size_t>(y) * width + x) * 4;
        return {rgba[p], rgba[p + 1], rgba[p + 2], rgba[p + 3]};
    }
    void set(std::uint32_t x, std::uint32_t y, ColorF c) noexcept {
        const std::size_t p = (static_cast<std::size_t>(y) * width + x) * 4;
        rgba[p] = c.r;
        rgba[p + 1] = c.g;
        rgba[p + 2] = c.b;
        rgba[p + 3] = c.a;
    }

    void fill(ColorF c);

    bool operator==(const ImageF&) const = default;
};

// The tight bounding box of alpha>0 pixels (whole-pixel coordinates), or nullopt for a fully
// transparent (or empty) image. This is a layer's *content* extent — usually far smaller than
// the document-sized image holding it (the Move tool's handles and layer thumbnails want this).
[[nodiscard]] std::optional<Rect> alphaBounds(const Image& img);

// Copy the `w` x `h` block at (x, y) out of `src` into a fresh w x h image; any part of the
// rectangle outside `src` reads as transparent (zero). The inverse of blitRegion. Used to store
// just a brush stroke's / inpaint fill's bounding box for undo (S60-a), not the whole layer.
[[nodiscard]] Image copyRegion(const Image& src, long x, long y, std::uint32_t w, std::uint32_t h);

// Write `region` (its own width x height) into `dst` at (x, y), clipped to dst's bounds.
void blitRegion(Image& dst, const Image& region, long x, long y);

// Convert an 8-bit RGBA image to float (each channel / 255). Straight alpha is preserved.
[[nodiscard]] ImageF toFloat(const Image& src);

// Convert a float RGBA image back to 8-bit, clamping to [0,1] and rounding to nearest.
[[nodiscard]] Image toImage8(const ImageF& src);

// Write `img` as a binary PPM (P6) file. PPM has no alpha, so the alpha channel is
// dropped. This is a dependency-free debug/harness helper; full format I/O (PNG, etc.)
// lives in the io module (session S42). Returns false and sets *error on failure.
bool writePpm(const Image& img, const std::string& path, std::string* error = nullptr);

}  // namespace mosaic::common
