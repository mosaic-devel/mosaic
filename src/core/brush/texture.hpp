#pragma once

#include <cstdint>
#include <memory>
#include <vector>

// THE TEXTURE OPTION (docs/brushes.md §6.6h), transcribed from the reference's texture option, its
// mask builder and the alpha-source half of its masking-brush composite ops.
//
// The mechanism, in one line: a PATTERN IMAGE is baked once into an 8-bit mask, the mask is tiled
// across the DOCUMENT (never across the dab), and every dab multiplies -- or subtracts -- the mask
// value under each of its pixels into that pixel's own alpha. That is what makes a textured brush
// look like paint sitting on paper: the grain stays put on the canvas while the brush moves over it.
//
// Two things follow from "tiled across the document" and both are load-bearing:
//   * The value a dab reads at a document pixel does not depend on where the dab is, so two
//     overlapping dabs agree about the grain -- exactly the property §6.6g's hatching lattice needs
//     from its document-locked phase, reached the same way.
//   * The bake is a property of the PRESET, not of the stroke: it is done once, at load, and shared.
//
// FLTK-, Vulkan- and platform-free.
namespace mosaic::core::brush {

// The texturing modes Mosaic implements. The reference names sixteen; these are the two the whole
// shipped set uses -- and `Multiply` is the format's own default, so an unknown or untranscribed
// mode imports as `Multiply` with a fidelity note, exactly as an unknown masking-brush op does
// (brush_engine.hpp's MaskingOp). The other fourteen wait for their own transcription pass:
// four of them (`Lightness`, `Gradient`, and the two height families) modify the dab's COLOUR
// rather than its alpha and are a different mechanism, not a different constant.
enum class TexturingMode : std::uint8_t {
    Multiply,
    Subtract,
};

// A pattern baked into the 8-bit mask a dab reads. WHITE (255) is "leave this pixel alone" under
// `Multiply` and "subtract everything" under `Subtract`; the reference's own mask carries the
// pattern's luminance in exactly that sense, and the sense is the pattern image's, not an inversion
// of it (a white paper texture must not erase the stroke).
//
// Shared and immutable: one preset's pattern is baked once at load and every stroke reads it.
struct TexturePattern {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> mask; // row-major, size == width * height

    [[nodiscard]] bool empty() const noexcept {
        return width == 0 || height == 0 || mask.size() != std::size_t(width) * height;
    }
};

// The pattern's own adjustments -- everything the reference bakes INTO the mask rather than
// applying per dab (`KisTextureMaskInfo`). All of them are static preset properties; none is
// sensor-driven, which is why they never appear beside a dab.
struct TextureBake {
    // `Texture/Pattern/Scale`. The pattern image is resampled by this BEFORE anything else, so a
    // 0.35 scale is a finer grain and not a bigger one. ⚠ At exactly 1.0 (and at exactly 0.0) the
    // reference resamples NOTHING, and neither does the bake -- the mask is the image's own
    // luminance, byte for byte.
    double scale = 1.0;
    double brightness = 0.0;   // `Texture/Pattern/Brightness`, SUBTRACTED from the mask value
    double contrast = 1.0;     // `Texture/Pattern/Contrast`, around 0.5
    double neutralPoint = 0.5; // `Texture/Pattern/NeutralPoint`, the two-segment re-centring below
    bool invert = false;       // `Texture/Pattern/Invert`
    // `Texture/Pattern/CutoffPolicy`: 0 = none, 1 = values outside the band go TRANSPARENT (0),
    // 2 = they go OPAQUE (255). The band is [cutoffLeft, cutoffRight] in 8-bit units.
    int cutoffPolicy = 0;
    int cutoffLeft = 0;
    int cutoffRight = 255;
};

// The largest pattern a bake will accept, in pixels. A preset is third-party input; the shipped
// set's biggest is 512 x 512.
inline constexpr std::uint64_t kMaxTexturePixels = 64ull << 20;

// Bake `rgba` (straight, non-premultiplied 8-bit RGBA, row-major, `w` x `h`) into the mask a dab
// reads. Transcribed from the reference's mask recalculation, in its own order and with its own
// clamps:
//
//     grey   = (r*11 + g*16 + b*5) / 32                    -- integer, the reference's qGray
//     v      = grey/255 * a + (1 - a)                      -- a transparent pixel reads WHITE
//     v     -= brightness
//     v      = (v - 0.5) * contrast + 0.5
//     v      = clamp(v, 0, 1)                              -- BEFORE the invert, deliberately
//     v      = invert ? 1 - v : v
//     v      = neutralPoint == 1 || (neutralPoint != 0 && v <= neutralPoint)
//                ? v / (2*neutralPoint)
//                : 0.5 + (v - neutralPoint) / (2 - 2*neutralPoint)
//     cutoff policy 1/2 outside [left/255, right/255] -> 0 / 1
//     mask   = round(v * 255)
//
// Returns null on a degenerate or oversized image (the caller badges it). Free of any file format:
// decoding the pattern is the io layer's business.
[[nodiscard]] std::shared_ptr<const TexturePattern> bakeTexturePattern(const std::uint8_t* rgba,
                                                                       std::uint32_t w,
                                                                       std::uint32_t h,
                                                                       const TextureBake& bake);

// The static half of the texture option, as one stroke reads it.
struct TextureParams {
    bool enabled = false;
    TexturingMode mode = TexturingMode::Multiply;
    // Baked at load and shared. `enabled` without a pattern is inert -- the reference disables the
    // option outright when its pattern will not load, and so does the engine's begin() gate.
    std::shared_ptr<const TexturePattern> pattern;
    // `Texture/Pattern/OffsetX`/`OffsetY`, in pattern pixels, SUBTRACTED from the document
    // coordinate (so a positive offset slides the grain the way the reference slides it).
    int offsetX = 0;
    int offsetY = 0;
    // `Texture/Pattern/isRandomOffsetX`/`isRandomOffsetY`: the offset is drawn ONCE PER STROKE from
    // the stroke's own keyed random constant, over [0, pattern width/height). Per stroke and not
    // per dab -- the reference reads its per-stroke random source, whose value is fixed for the
    // stroke -- so the grain stays registered under a stroke while differing between strokes.
    bool randomOffsetX = false;
    bool randomOffsetY = false;
    // `Texture/Pattern/UseSoftTexturing`: selects the composite's second parameterization, in which
    // the STRENGTH scales the pattern rather than the dab's own alpha.
    bool softTexturing = false;
};

// The mask value at a DOCUMENT pixel: `mask[(y - offY) mod H][(x - offX) mod W]`, with a
// mathematical (never truncating) modulo so the tiling is continuous across the origin. Free +
// pure -> unit-tested.
[[nodiscard]] std::uint8_t textureValueAt(const TexturePattern& pattern, int docX, int docY,
                                          int offX, int offY) noexcept;

// The per-dab strength in 8-bit, the reference's `scaleToA`: round-to-nearest, clamped to [0,255].
[[nodiscard]] int textureStrength8(double strength) noexcept;

// One pixel of the texture composite, transcribed from the alpha-source half of the reference's
// masking-brush composite ops in their WITH-STRENGTH parameterizations. `src` is the pattern's mask
// value, `dst` the dab's own 8-bit alpha, `strength8` the per-dab strength from `textureStrength8`.
// All arithmetic is the reference's 8-bit integer arithmetic, truncating exactly where it truncates:
//
//     mul(a,b)     = (a*b) / 255                     mul(a,b,c) = (a*b*c) / (255*255)
//     inv(a)       = 255 - a                         union(a,b) = a + b - mul(a,b)
//
//     Multiply, hard: mul(src, dst, strength8)
//     Multiply, soft: mul(union(src, inv(strength8)), dst)
//     Subtract, hard: max(0, dst - (src + inv(strength8)))
//     Subtract, soft: max(0, dst - mul(src, strength8))
//
// ⚠ At strength 255 the two Multiply forms agree and the two Subtract forms agree, but NOT at every
// intermediate strength -- the soft form scales the pattern toward white / toward nothing, the hard
// form scales the DAB. That asymmetry is the reference's and it is what the flag names.
// Free + pure -> unit-tested.
[[nodiscard]] std::uint8_t textureComposite(TexturingMode mode, std::uint8_t src, std::uint8_t dst,
                                            int strength8, bool soft) noexcept;

} // namespace mosaic::core::brush
