#pragma once

#include "common/geometry.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
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

// ---- Zero-page pixel storage ------------------------------------------------------------------
//
// A calloc-backed allocator whose whole purpose is to stop WRITING zeros that are already zero.
//
// A 39.8 MP float working buffer is 637 MB. `std::vector<float>` value-initialises, so creating one
// costs a 637 MB memset -- 75 ms on this machine, single-threaded, per buffer -- and a composite of
// a big document creates dozens of them: one per leaf layer, one per group, one per vector
// rasterisation. Measured at ~26 canvas-sized clears and ~2.0 s of the S60 fixture's open.
//
// Every one of those memsets is redundant. calloc of that size goes straight to mmap, and the
// kernel's pages are ALREADY zero; the memset's only effect is to fault all 637 MB in and write
// zeros over zeros. Handing back the mapping untouched costs 0.01 ms, and the pages then fault in
// one by one as the caller actually writes them -- which, for a layer covering 2% of the canvas, is
// 2% of the buffer. Measured on the same machine: 75.81 ms -> 1.50 ms for a fresh buffer whose
// caller writes a 2% window.
//
// ⚠ THE CONTRACT IS "allocate() RETURNS ZEROED MEMORY", and `construct` is a no-op so the vector
// does not overwrite it. That makes a default-init `resize()` equivalent to a value-init one --
// but ONLY because every allocation comes from calloc. The one way to observe uninitialised bytes
// is to grow back into capacity retained across a shrink (`clear()` / `resize(smaller)` then
// `resize(bigger)`), because no new allocation happens and nothing re-zeroes the tail. Image /
// ImageF are sized once at construction and replaced wholesale, never shrunk and regrown, and
// `assign()` fills explicitly on every path that reuses a buffer -- see render/compositor.cpp's
// rasteriseLayerInto, which is the only reuse site in the tree.
template <typename T>
struct ZeroPageAllocator {
    static_assert(std::is_trivially_default_constructible_v<T>,
                  "the no-op construct below is only sound for types whose value-init is all-bits-"
                  "zero -- which is what calloc hands back");
    using value_type = T;

    ZeroPageAllocator() noexcept = default;
    template <typename U> ZeroPageAllocator(const ZeroPageAllocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t n) {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::bad_alloc();
        void* p = std::calloc(n, sizeof(T));
        if (p == nullptr)
            throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept { std::free(p); }

    // The point of the whole exercise: allocate() already zeroed this, so do not zero it again.
    template <typename U> void construct(U* /*p*/) noexcept {}
    template <typename U, typename... Args> void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }

    template <typename U> bool operator==(const ZeroPageAllocator<U>&) const noexcept {
        return true;
    }
};

// ⚠ THE FLOAT BUFFER ONLY, and deliberately not the 8-bit one. `Image` is layer STORAGE: its
// pixels arrive from a decoder or a codec tile and are then fully written, so lazy zero pages buy
// nothing -- while giving it a private allocator would turn every `img.rgba = std::move(decoded)`
// in io/ into a 159 MB copy, which is a real cost paid for an imaginary one. `ImageF` is the
// compositor's WORKING buffer, allocated per layer per composite and mostly never touched.
using Floats = std::vector<float, ZeroPageAllocator<float>>;

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
    Floats rgba;  // size == width * height * 4

    ImageF() = default;
    // ⚠ `rgba(n)`, NOT `rgba(n, 0.0f)`. The second form is a fill-construct and memsets the whole
    // buffer -- 637 MB and 75 ms at 39.8 MP. The first is a default-construct, which
    // ZeroPageAllocator turns into "calloc and touch nothing": still all-zero (that is the
    // allocator's contract), but the pages arrive from the kernel already zeroed and fault in only
    // where the caller actually writes. A transparent buffer is exactly what this always promised;
    // the only thing that changed is that it no longer pays to write the zeros twice.
    ImageF(std::uint32_t w, std::uint32_t h)
        : width(w), height(h), rgba(static_cast<std::size_t>(w) * h * 4) {}

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
