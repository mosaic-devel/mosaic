#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include "common/image.hpp"
#include "core/blend_mode.hpp"

// GPU half of the compositor (S7-b): a persistent Vulkan compute context that runs the
// `composite_blend.comp` shader -- the GLSL mirror of `render/blend.hpp`. It performs the one
// hot, embarrassingly-parallel step of the composite, the per-pixel source-over-with-blend of a
// source layer onto the accumulator (`blendOver`); the tree walk, masks, clip, adjustments and
// checkerboard stay on the CPU path (render/compositor.cpp), so every blend mode runs on the GPU
// while the rest of the model is reused unchanged.
//
// Device memory is allocated through VMA (vendored, MIT) -- the project's first allocator user.
// This is the correctness/parity milestone; keeping buffers resident across a composite and
// tiling dirty regions is a later perf pass (S60). Created via the factory and owned by
// unique_ptr; non-copyable/movable. (VMA stays private to the .cpp via a pimpl.)

namespace mosaic::render {

class GpuCompositor {
public:
    // Returns nullptr (and sets `error`) if Vulkan is unavailable or the device cannot use an
    // rgba32f storage image. Enables validation in debug, matching the rest of render/.
    static std::unique_ptr<GpuCompositor> create(std::string& error);
    ~GpuCompositor();

    GpuCompositor(const GpuCompositor&) = delete;
    GpuCompositor& operator=(const GpuCompositor&) = delete;
    GpuCompositor(GpuCompositor&&) = delete;
    GpuCompositor& operator=(GpuCompositor&&) = delete;

    // acc = compositeOver(mode, acc, src, opacity), computed on the GPU. `acc` and `src` must be
    // the same non-empty size; `acc` is updated in place. Returns false / sets `error` on an
    // unrecoverable GPU error (the caller falls back to the CPU blend for that step).
    bool blendOver(common::ImageF& acc, const common::ImageF& src, core::BlendMode mode,
                   float opacity, std::string& error);

    [[nodiscard]] std::string deviceName() const;
    [[nodiscard]] std::uint32_t validationErrors() const noexcept;

private:
    GpuCompositor() = default;

    // (Re)create the canvas-sized device images + host-staging buffers when the size changes.
    bool ensureSize(std::uint32_t w, std::uint32_t h, std::string& error);
    void destroySizedResources() noexcept;

    class Impl;                 // VulkanContext + pipeline live in the .cpp to keep VMA private
    std::unique_ptr<Impl> m_impl;
};

}  // namespace mosaic::render
