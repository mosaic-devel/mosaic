#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "common/image.hpp"

// render -- the Vulkan backend. In S2 it provides a headless "render a solid color"
// operation with a CPU fallback, used to bootstrap the GPU path and the debug harness.
// The compositor, swapchain, and shader pipelines arrive in later sessions (S3/S7).
namespace mosaic::render {

std::string_view moduleName() noexcept;

// Which backend to use for an operation.
enum class Backend {
    Auto,        // try the GPU clear path, fall back to CPU
    Gpu,         // require the GPU clear path (vkCmdClearColorImage)
    GpuCompute,  // require the GPU compute path (fill via the embedded fill.comp shader)
    Cpu,         // force CPU
};

std::string_view backendName(Backend backend) noexcept;

// How a transformed (rotated/scaled/sub-pixel-translated) layer is resampled when the compositor
// places it into the document buffer (the "Transform Anti-aliasing" feature). One unified setting
// governs rotate + scale + every future transform; whole-pixel translation stays lossless (it
// hits the integer-shift fast path regardless of this choice). Sampling runs in premultiplied
// alpha, and every kernel's source-space support widens with the minification factor so a
// reduction low-passes ("windowed" cubic/Lanczos) instead of aliasing.
enum class ResampleFilter : std::uint8_t {
    Auto,        // pick the best kernel per transform + live-vs-commit (see chooseAutoFilter)
    Nearest,     // point sample (crisp pixel art; the lossless, dependency-free baseline)
    Bilinear,    // 2x2 linear (cheap; the live-drag default)
    Bicubic,     // 4x4 Catmull-Rom cubic (sharp, interpolating)
    Mitchell,    // 4x4 Mitchell-Netravali cubic (B=C=1/3; the balanced default)
    Lanczos2,    // windowed sinc, a=2 (4x4)
    Lanczos3,    // windowed sinc, a=3 (6x6; sharp up- and down-scale -- Auto's commit kernel)
    Area,        // box-average over the source footprint (minification without ringing)
    Gaussian,    // soft Gaussian (a gentle low-pass / prefilter)
    Supersample, // brute-force NxN supersample-average (max quality, slowest)
};

std::string_view resampleFilterName(ResampleFilter f) noexcept;

// Result of a headless render: the produced image plus diagnostics.
struct RenderResult {
    bool ok = false;
    std::string error;
    common::Image image;
    Backend usedBackend = Backend::Cpu;
    std::uint32_t validationErrors = 0;  // Vulkan validation errors observed (GPU path)
};

// Render a `w` x `h` image filled with `color`. On the GPU path this clears an offscreen
// VK_FORMAT_R8G8B8A8_UNORM image and reads it back; the CPU path fills directly. With
// Backend::Auto, a GPU failure falls back to CPU (and `usedBackend` reports which ran).
RenderResult renderSolid(std::uint32_t w, std::uint32_t h, common::Color8 color,
                         Backend backend = Backend::Auto);

}  // namespace mosaic::render
