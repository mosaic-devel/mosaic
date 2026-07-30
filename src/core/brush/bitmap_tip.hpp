#pragma once

#include "core/brush/dab_mask.hpp"
#include "core/brush/stroke_state.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

// Bitmap brush tips -- the `.gbr` single stamp, the `.gih` animated image hose, and the `.png`/`.abr`
// rasters (docs/brushes.md §3.5, §3.6). **47 of the 82 pixel-brush presets in the CC-0 default set use
// one**, and 23 of those 47 are hoses, so this is the load-bearing tip kind, not the exotic one.
//
// This file owns three things the format makes easy to get backwards:
//
// 1. **The coverage convention (§3.6.1).** A `TipFrame` holds a *tip image*: greyscale where white is
//    NO paint, like ink on paper. That is the convention every loader must decode into, and it is the
//    convention a `.png` already stores -- but a `.gbr` stores the exact opposite, so its loader
//    inverts. Coverage is then derived here, once. Invert twice or not at all and the tip comes out
//    as its own photo-negative.
//
// 2. **Which frame a dab stamps.** `HoseState` reproduces the mixed-radix cell index of §3.6.2,
//    integer division and final modulo included, because three shipped files depend on both.
//
// 3. **Resampling.** A 300 px tip stamped as a 20 px dab is a 15x minification; bilinear alone would
//    alias it into sparkle. Each frame carries a power-of-two mip chain and a dab samples the
//    smallest level still at least as large as it needs.
//
// FLTK-, Vulkan- and platform-free. Nothing here decodes a file: `TipFrame`s arrive already decoded,
// which is the seam between this (Arc A) and the format readers (Arc B).
namespace mosaic::core::brush {

// `brushApplication` -- the serialized values, which are relied upon (docs/brushes.md §3.5).
// Everything except `AlphaMask` is per-dab colour. Only the *coverage* rule differs here; the colour
// each one deposits is the accumulator's business.
enum class TipApplication : std::uint8_t {
    AlphaMask = 0,
    ImageStamp = 1,
    LightnessMap = 2,
    GradientMap = 3,
};

// Whether the source raster was a bare mask (a `bytes=1` GBR, a grey PNG with no alpha) or a full
// image. It decides nothing about colour -- it decides whether the adjustments below run at all.
enum class TipSourceKind : std::uint8_t { Mask, Image };

// The three tip adjustments (docs/brushes.md §3.5). They apply to every application EXCEPT
// `ImageStamp`, and only to an `Image`-kind source. The three do not share a scale.
struct TipAdjustments {
    bool autoMidPoint = false; // midpoint = the image's own average grey
    double midPoint = 127.0;   // 0..255
    double brightness = 0.0;   // -1..1
    double contrast = 0.0;     // -1..1

    [[nodiscard]] bool neutral() const noexcept;
};

// One cell of a tip, in the TIP IMAGE convention (white = no paint), straight alpha.
struct TipFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba; // size == width * height * 4
};

// How a hose picks the cell for a dab. `Constant`, `Incremental` and `Random` are stateful; the rest
// are pure functions of the current sample. An unrecognised name is `Constant` -- that is the format's
// own rule, not a fallback we invented.
enum class FrameSelection : std::uint8_t {
    Constant,
    Incremental,
    Angular,
    Velocity,
    Random,
    Pressure,
    TiltX,
    TiltY,
};

[[nodiscard]] std::string_view frameSelectionName(FrameSelection s) noexcept;
[[nodiscard]] FrameSelection frameSelectionFromName(std::string_view name) noexcept;

inline constexpr int kMaxHoseDim = 4;
inline constexpr int kMaxTipFrames = 4096;
// A single frame larger than this is refused. `.gbr` stores width and height as unbounded uint32.
inline constexpr std::uint64_t kMaxTipPixels = 64ull << 20;

// The parasite, parsed (docs/brushes.md §3.6.2). `declaredCells` is the file's `ncells`, which is a
// claim rather than a count -- `fairy-dust.gih` says 4 and ships 1. It is kept separate from the
// number of frames that actually loaded because the cell STRIDE is derived from the claim and the
// final wrap from the truth, and reproducing both is what reproduces the shipped files.
struct HoseParams {
    int dim = 0;
    int declaredCells = 0;
    std::array<int, kMaxHoseDim> rank{};
    std::array<FrameSelection, kMaxHoseDim> selection{};
};

class HoseState {
public:
    // Incremental cells restart at 0 on a new stroke, because the dab counter does.
    void beginStroke() noexcept { m_index = {}; }

    // The cell for the dab about to be stamped. Call once per dab, after `StrokeState::beginDab()`:
    // a `Random` dimension draws from the stroke's per-dab random stream here, so calling it twice
    // for one dab consumes two draws and shifts every `fuzzy` sensor downstream.
    [[nodiscard]] int selectFrame(const HoseParams& params, int frameCount, StrokeState& state,
                                  const StrokeInput& sample) noexcept;

private:
    std::array<int, kMaxHoseDim> m_index{};
};

class BitmapTip {
public:
    BitmapTip() = default;
    BitmapTip(std::vector<TipFrame> frames, TipApplication application, TipSourceKind sourceKind,
              TipAdjustments adjustments = {}, HoseParams hose = {});

    [[nodiscard]] int frameCount() const noexcept { return static_cast<int>(m_frames.size()); }
    [[nodiscard]] bool empty() const noexcept { return m_frames.empty(); }
    [[nodiscard]] TipApplication application() const noexcept { return m_application; }
    [[nodiscard]] const HoseParams& hose() const noexcept { return m_hose; }

    // Frames of one hose need not agree on size -- each cell is its own raster -- so a dab's extent
    // depends on which cell it stamps.
    [[nodiscard]] std::uint32_t frameWidth(int frame) const noexcept;
    [[nodiscard]] std::uint32_t frameHeight(int frame) const noexcept;

    // `max(w, h)`: the length the preset's `scale` multiplies (docs/brushes.md §3.5).
    [[nodiscard]] double baseSize(int frame) const noexcept;

    // How many frames the caller handed us that we refused as degenerate or oversized. An honesty
    // counter, in the style of the `docio` layer: a hose that lost a cell must not look intact.
    [[nodiscard]] int droppedFrames() const noexcept { return m_dropped; }

    // The mip chain of frame `frame`, level 0 being full resolution. Exposed for the renderer and the
    // tests; the coverage plane is 8-bit, 255 = full paint.
    struct MipLevel {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> coverage;
    };
    [[nodiscard]] int levelCount(int frame) const noexcept;
    [[nodiscard]] const MipLevel& level(int frame, int lvl) const noexcept;

    // The deepest level still at least as large as `targetW` x `targetH`, so the resample only ever
    // minifies within a level and never magnifies a level that has already thrown detail away.
    [[nodiscard]] int pickLevel(int frame, double targetW, double targetH) const noexcept;

private:
    std::vector<std::vector<MipLevel>> m_frames;
    TipApplication m_application = TipApplication::AlphaMask;
    HoseParams m_hose;
    int m_dropped = 0;
};

// The dab a bitmap tip paints at the brush's effective `diameter` (the tip's LONG axis, §3.5), with
// `ratio` squashing the short one. The frame's own aspect is preserved inside that.
[[nodiscard]] DabShape bitmapDabShape(const BitmapTip& tip, int frame, double diameter,
                                      double ratio, double angleRad, bool mirrorH = false,
                                      bool mirrorV = false) noexcept;

// Rasterize one frame at the given shape and sub-pixel phase. Dimensions agree exactly with
// `placeDab(shape, ..., steps)` for the same phase.
[[nodiscard]] DabMask renderDabMask(const BitmapTip& tip, int frame, const DabShape& shape,
                                    double subX, double subY);

namespace detail {

// The reference's `qGray` weights. Identity on a grey pixel, which is every pixel of the default set.
[[nodiscard]] inline std::uint8_t luma(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    return static_cast<std::uint8_t>((r * 11 + g * 16 + b * 5) / 32);
}

// The piecewise-linear brightness/contrast transfer curve of docs/brushes.md §3.5, hinged at
// (midPoint, 127 + brightness*128). Returns a 256-entry lookup table; building it once per frame
// keeps the divisions -- and their degenerate cases -- out of the pixel loop.
[[nodiscard]] std::array<std::uint8_t, 256> adjustmentTable(const TipAdjustments& adj) noexcept;

} // namespace detail

} // namespace mosaic::core::brush
