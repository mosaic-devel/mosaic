#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "common/geometry.hpp"  // common::Affine2D
#include "common/image.hpp"

namespace mosaic::core {
struct LayerEffects;
}

namespace mosaic::render {

// The Vulkan compute lane of the layer-effect stack (S60-e; docs/layer-effects.md §8 promised it
// and §4 designed the seam for it). `render::applyEffects` in layer_effects_render.cpp stays the
// permanent CPU reference: this class mirrors it behind the same seam, and every pixel it draws is
// held to that reference by tests/test_layer_effects_gpu.cpp.
//
// ---- ADMISSION IS PER EFFECT KIND ------------------------------------------------------------
//
// The precedent is `TileCompositor`'s per-kind adjustment admission (tile_compositor.hpp): a lane
// that GUESSES at a kind it has not ported draws the wrong picture, which is strictly worse than
// being slow. So the served set is named, the refused set is named with the reason it is refused,
// and a stack containing ANY refused kind hands the WHOLE stack back to the CPU lane -- never half
// of it, because the z-order interleaves the tiers and a half-served stack is a different picture.
//
// SERVED -- fill-opacity, drop shadow, outer glow, inner shadow, inner glow, satin, and the
// colour overlay + strokes, in each case **with a solid (or absent) paint**. Those are exactly the
// effects whose pixel value is `blur(f(SDF)) -> colourise -> blend`, i.e. a constant colour times a
// coverage field, and both lanes evaluate the identical formula on identical floats.
//
// REFUSED, and the pixel reason for each:
//
//   * ANY NON-SOLID PAINT -- a Gradient or a Pattern in a stroke, glow or overlay (so: the
//     gradient overlay and the pattern overlay always, and a gradient/patterned stroke or glow).
//     `core::vec::sampleAt` is a second rasterizer -- stop interpolation with per-segment
//     midpoints, an inverse gradient transform, three spread methods, the procedural pattern
//     generators -- and its BANDING DITHER (`vec::DitherKind`) is the load-bearing part: the
//     Ordered and BlueNoise kinds index host-built threshold tables, and `Noise` is
//     `common::ditherTPDF`, a 64-BIT integer hash finished in DOUBLE. Vulkan 1.0 guarantees
//     neither a 64-bit integer nor a double in a shader, so the hash is not reproducible at the
//     floor at all, and an *approximated* dither is a different noise field on every pixel it
//     touches -- not a rounding difference a tolerance can absorb.
//
//   * BEVEL & EMBOSS -- same reason, one tier down. Its shade ramp is a shallow gradient that
//     bands into visible iso-shade lines at 8 bits, so `applyBevel` adds ~1 LSB of TPDF dither
//     (`common::ditherTPDF` again) to every shaded pixel. Without a 64-bit hash the lane cannot
//     reproduce that noise, and dropping it would band exactly where the CPU lane does not.
//     (Its single-pass Sobel normals and its height field would otherwise port straight across;
//     the dither is the whole of the refusal.)
//
// A refusal costs speed and nothing else: the CPU lane it hands back to is the very reference this
// lane is measured against.
//
// ---- WHAT IS COOKED ON THE HOST, AND WHY -----------------------------------------------------
//
// The blur lane's cook discipline (blur_gpu.cpp), applied here: anything the CPU computes once per
// call is computed HOST-side, with the same code on the same floats, so parity is by construction
// rather than by transcription.
//   * The SIGNED DISTANCE FIELDS come from the SAME public `fx::signedDistanceField` /
//     `fx::signedDistanceFieldAA` the CPU lane calls. The Felzenszwalb-Huttenlocher transform is a
//     sequential parabola-envelope scan whose per-column state a data-parallel lane cannot carry;
//     any GPU distance transform would be a DIFFERENT field, which moves every stroke edge and
//     every shadow contour. It is cooked, not ported.
//   * The Gaussian half-kernels and the Kovesi box radii are transcribed from effect_primitives.cpp
//     and uploaded, so the shader carries only the per-tap math.
//   * The (angle, distance) -> offset vectors, the ROI, the content/anchor boxes and the concentric
//     stroke ring stack are all host arithmetic, exactly as the CPU lane does them.
// The GPU takes what is actually parallel: every blur pass, every field seed and every per-pixel
// colourise-and-composite.
//
// ⚠ The BLURS ARE NOT `BlurGpu`'s. That lane's separable pass is an RGBA image kernel with a CLAMP
// edge policy behind an sRGB-decode/premultiply convert chain (blur_separable.comp); the effect
// blurs are single-channel COVERAGE planes with a REFLECT-101 edge policy and no colour space at
// all (effect_primitives.cpp), plus the 3-pass Kovesi box lane which that shader does not carry.
// Pointing this lane at it would move the pixels within one blur radius of any content that
// touches the buffer edge. Same algorithm family, different reference -- so `le_plane.comp` mirrors
// the reference this seam actually has.
//
// ---- VULKAN 1.0 FLOOR ------------------------------------------------------------------------
//
// This lane FITS THE FLOOR and is meant to: four storage buffers against a guaranteed four, an
// 80-byte push range against a guaranteed 128, 64-invocation workgroups against a guaranteed 128,
// SPIR-V 1.0, no `#extension`. `MOSAIC_GPU_PROFILE=floor` therefore SERVES here rather than
// refusing -- the caps gate is still asked (`fitsStorageBuffers` / `fitsPushConstants`), because a
// lane must ask rather than assume, but the honest answer on a floor device is yes.
//
// PERSISTENT: one context + two pipelines for the object's lifetime; buffers grow on demand.
// NOT thread-safe -- the installer serialises calls, exactly like BlurGpu.

// Why the lane declined. Every value is a testable predicate, not a generic failure.
enum class LayerEffectRefusal : std::uint8_t {
    None = 0,
    CpuOnlyMode,     // --cpu / MOSAIC_CPU_ONLY / Settings -> Rendering
    NoDevice,        // no usable Vulkan device at all
    DeviceTooSmall,  // the caps gate said no (storage buffers, push constants, dispatch grid)
    BufferTooLarge,  // the effect ROI exceeds this lane's byte policy or the device's binding range
    NonSolidPaint,   // a Gradient/Pattern paint anywhere in the stack (see the header note)
    Bevel,           // Bevel & Emboss is enabled (its TPDF dither, see the header note)
    StackTooDeep,    // more dispatches than the descriptor pool holds
    DeviceError,     // a Vulkan call failed mid-flight; `io` is untouched
};
[[nodiscard]] std::string_view layerEffectRefusalName(LayerEffectRefusal r) noexcept;

// The PURE admission question: does the served set cover this stack? No device, no globals, no
// allocation -- so a caller (and a test) can ask it without a GPU, and the served/refused boundary
// is unit-testable on any machine. `None` means "the lane would take it".
[[nodiscard]] LayerEffectRefusal layerEffectsAdmission(const core::LayerEffects& fx) noexcept;

class LayerEffectsGpu {
public:
    // The lane's resource footprint, published so a caps gate can be asserted against a synthetic
    // floor probe without building anything (tests/test_layer_effects_gpu.cpp does exactly that).
    static constexpr std::uint32_t kStorageBufferBindings = 4;
    static constexpr std::uint32_t kPushConstantBytes = 80;
    static constexpr std::uint32_t kWorkgroupInvocations = 64;

    // nullptr (with `error` set) when the policy forbids a compute lane or no usable device exists.
    static std::unique_ptr<LayerEffectsGpu> create(bool enableValidation, std::string& error);
    ~LayerEffectsGpu();
    LayerEffectsGpu(const LayerEffectsGpu&) = delete;
    LayerEffectsGpu& operator=(const LayerEffectsGpu&) = delete;

    // The seam contract, mirroring `render::applyEffects`: transform `io` in place to what the CPU
    // lane produces for `fx` -- the same picture within float-lane tolerance. false = the CPU lane
    // must serve, and `io` is then BYTE-UNTOUCHED (nothing is written until the readback lands).
    //
    // `antialias` and `bufferToLayer` exist so this is a drop-in for `applyEffects`; both only ever
    // affect PATTERN sampling, which this lane refuses, so the served path ignores them. They are
    // kept in the signature rather than dropped so the compositor's call site is one line and does
    // not have to know which parameters the lane happens to use today.
    bool apply(common::ImageF& io, const core::LayerEffects& fx, bool antialias = true,
               const std::optional<common::Affine2D>& bufferToLayer = std::nullopt);

    // Why the last apply() returned false (`None` when it returned true, or when the stack was
    // empty and there was nothing to do). Diagnostics only.
    [[nodiscard]] LayerEffectRefusal lastRefusal() const noexcept;

    [[nodiscard]] std::string deviceName() const;

private:
    LayerEffectsGpu();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace mosaic::render
