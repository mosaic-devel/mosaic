#pragma once

#include "core/brush/bitmap_tip.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The bitmap tip-file readers (docs/brushes.md §3.6): GIMP .gbr, the .gih animated hose, the
// Photoshop .abr sampled collection, and the .png tip. All of them decode into bitmap_tip.hpp's
// TIP IMAGE convention -- greyscale where WHITE (255) is no paint -- which puts the §3.6.1 sign
// rule here and nowhere else:
//
//   * a `bytes=1` GBR / GIH cell / ABR sample stores COVERAGE (255 = full paint), so those
//     loaders invert on the way in;
//   * a PNG already IS a tip image and is not inverted.
//   Apply "the" inversion uniformly and exactly one of the two families is right.
//
// Every default and quirk below is the producer's, read off its loaders:
//   * GBR v1 has no spacing field (0.25 stands); v2+ stores percent, rejected above 1000. The
//     magic number is read and NOT verified -- faithful, and third-party files depend on it.
//   * A hose's spacing is its LAST cell's; its application is its FIRST cell's; its colour
//     verdict is an OR over cells. `ncells` is a CLAIM (§3.6.2): cells load until one does not
//     fit, and the shortfall is counted, never trusted.
//   * The parasite reads exactly four keys (ncells / dim / rank<i> / sel<i>); the five others
//     GIMP writes have no reader and are ignored. dim starts at 0 (absent stays 0), an
//     out-of-range or garbage value becomes 1, and rank/sel indices are validated against
//     whatever dim is AT THAT POINT in the string. rank0=0 under incremental/angular selection
//     degrades to constant (the producer's one sanitize rule).
//   * "Has colour" verdicts are per-format quirks, not one rule: a bytes=4 GBR's is any
//     non-grey pixel (alpha ignored); a PNG's is likewise !allGray, but only after the
//     mask-vs-image split which DOES consult alpha; a bytes=1 anything is simply false.
//   * A PNG that is all-grey with no alpha is composited over WHITE and kept as a Mask; any
//     other PNG stays an Image whose application is AlphaMask when grey, LightnessMap when
//     coloured. `brush_spacing`/`brush_name` text chunks are honoured (spacing as a fraction).
//   * ABR: v1/v2 and v6.1/6.2 only, SAMPLED brushes only -- computed brushes are skipped and
//     counted (correctly skipped: the reference's seek arithmetic for that case is broken and
//     loses the rest of the file; reproducing a pointer bug serves nobody). Depth 8 only.
//     PackBits scanlines decode with bounds-checked writes.
//
// Hostile-input caps come from bitmap_tip.hpp (kMaxTipPixels, kMaxTipFrames) plus a dimension
// fence; a file over them fails or drops cells with the loss counted, never crashes.
namespace mosaic::io::brush {

// One decoded tip file, engine-ready.
struct TipFile {
    std::string name; // embedded name; empty when the format carries none (the caller derives)
    std::vector<core::brush::TipFrame> frames; // never empty on success
    core::brush::TipSourceKind sourceKind = core::brush::TipSourceKind::Mask;
    // What the loader itself decides before any preset attribute weighs in (§3.5's loader
    // column): AlphaMask for masks and grey images, LightnessMap for coloured ones.
    core::brush::TipApplication defaultApplication = core::brush::TipApplication::AlphaMask;
    // The legacy content-test verdict (per-format quirks above) that legacyBrushApplication
    // consults when a preset carries no explicit application (§3.5).
    bool hasColorAndTransparency = false;
    double spacing = 0.25; // the file's embedded default; a preset attribute overrides either way
    core::brush::HoseParams hose{}; // dim > 0 only for a .gih
    int declaredCells = 0;          // the hose's claim, verbatim (frames.size() is the truth)
    int droppedFrames = 0;          // cells/samples skipped: didn't fit, bad depth -- honesty
};

[[nodiscard]] std::optional<TipFile> readGbr(const std::uint8_t* data, std::size_t size,
                                             std::string* error = nullptr);

[[nodiscard]] std::optional<TipFile> readGih(const std::uint8_t* data, std::size_t size,
                                             std::string* error = nullptr);

[[nodiscard]] std::optional<TipFile> readPngTip(const std::uint8_t* data, std::size_t size,
                                                std::string* error = nullptr);

// An .abr is a COLLECTION: zero or more sampled brushes (an all-computed file yields an empty
// vector, which is a success with `*droppedBrushes` saying why it is empty).
[[nodiscard]] std::optional<std::vector<TipFile>> readAbr(const std::uint8_t* data,
                                                          std::size_t size,
                                                          std::string* error = nullptr,
                                                          int* droppedBrushes = nullptr);

} // namespace mosaic::io::brush
