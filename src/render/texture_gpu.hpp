#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/texture/texture_params.hpp"
#include "core/texture/texture_render.hpp"

namespace mosaic::render {

// The Vulkan compute lane of the Texture Generator (S55-h; docs/texture-generator.md §8.4): the
// per-pixel sky and paper kernels ported to compute shaders (shaders/texture_sky.comp /
// texture_paper.comp), dispatched over a VMA-allocated rgba32f storage image and read back into
// the same Image/ImageF results the CPU lane produces. Everything cookable per render -- the
// Hosek-Wilkie configs, the atmosphere tables, the projected star field, the moon frame, the
// deck constants -- is cooked HERE on the CPU in double (much of it by calling the same public
// core code the CPU renderer uses), so the shaders carry only the per-pixel math.
//
// The CPU renderers stay the permanent reference (§8.4): this lane is injected through
// core::texture::setTextureRenderOverride by the app, is NEVER installed by the test binary
// (goldens stay CPU-pinned), and is held to the CPU lane by tolerance-based parity tests
// (tests/test_texture_gpu.cpp -- double CPU vs float GPU, so "the same picture", not the bits).
//
// Lane coverage: Sky (dome + twilight/night + stars + moon + sun + lens flare + 2D decks + the
// volumetric march) and Paper run on the GPU; Grass (blade instancing + painter's compositing,
// not a per-pixel kernel), the S55-g materials (own param structs; natural next ports) and
// anything this build does not recognise return false so the CPU serves.
//
// Progress/cancel ride dispatch granularity: the window renders in row bands (one submit each),
// progress->rowsDone advances per band and cancellation is honoured between bands; a cancelled
// render returns true with an EMPTY result (renderTexture's all-or-nothing contract).
//
// Cache residency: the result is read back because today's compositor consumes CPU-side
// Image/ImageF caches (render/compositor.cpp samples the TextureLayer cache on the CPU). The
// no-readback handover -- keeping this rgba32f target resident and handing the VkImage to a
// GPU-resident compositor -- is the documented S60 seam; the target image already lives in
// device memory, so only the final vkCmdCopyImageToBuffer leaves with it.
//
// PERSISTENT: one context + both pipelines for the object's lifetime; the target image and
// staging grow on demand; the moon albedo/elevation tables upload once. NOT thread-safe -- the
// app's installer serialises calls (the dialog worker and the UI thread can both render).
class TextureGpu {
public:
    // nullptr (with `error` set) when no usable Vulkan device exists or it cannot run the lane.
    static std::unique_ptr<TextureGpu> create(bool enableValidation, std::string& error);
    ~TextureGpu();
    TextureGpu(const TextureGpu&) = delete;
    TextureGpu& operator=(const TextureGpu&) = delete;

    // The core-hook contract (core::texture::TextureRenderOverride): render `params` for the
    // full (w, h) frame evaluating only `window`. false = the CPU lane serves.
    bool render(const core::texture::TextureParams& params, std::uint32_t w, std::uint32_t h,
                const core::texture::TextureWindow& window,
                core::texture::TextureRenderProgress* progress,
                core::texture::TextureRenderResult& out);

    [[nodiscard]] std::string deviceName() const;

private:
    TextureGpu();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace mosaic::render
