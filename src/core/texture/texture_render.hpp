#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

#include "common/image.hpp"
#include "core/texture/texture_params.hpp"

// The Texture Generator's CPU reference renderers (S55-a; docs/texture-generator.md §8). This is
// the PERMANENT CPU lane: the correctness oracle and headless-test surface a Vulkan compute lane
// parity-tests against later (S55-h). Rendering is a pure function of (params, w, h) -- all
// randomness is hash-seeded off params.seed (§8.3), so the same inputs give the same pixels at
// any thread count, and a resized render is a faithful re-evaluation, not a rescale.
//
// S55-a ships BASELINE generators -- honest, deterministic, built on the noise kit, exercising
// the full pipeline (both cache depths, element toggles, alpha carry §3.4) -- while the real
// shading lands per subsystem session: Hosek-Wilkie dome + camera (S55-b), volumetric clouds
// (S55-c), paper kinds + Oren-Nayar (S55-d), grass blades (S55-e). Baselines are golden-pinned;
// replacing one is a deliberate golden-breaking change in its session.
namespace mosaic::core::texture {

// Exactly one arm is populated, by generator (§4.4): Sky renders FLOAT (banding-free gradients;
// HDR export later rides this), Paper/Grass render 8-bit. Straight alpha, the compositor's
// convention. Both empty only for a degenerate (zero-area) request -- or a cancelled one.
struct TextureRenderResult {
    std::optional<common::Image> image8;
    std::optional<common::ImageF> imageF;
};

// A sub-rect of the full (frameW x frameH) frame to evaluate (S55-f; §8.2). The returned image is
// window-sized and BYTE-EXACT to the same crop of the full-frame render -- every generator is a
// pure function of the frame coordinate, so a window is a faithful evaluation, not a rescale.
// Powers the dialog's 1:1 / pan preview; w or h of 0 means the full frame (the default).
struct TextureWindow {
    long x = 0;
    long y = 0;
    std::uint32_t w = 0;  // 0 = the full frame width
    std::uint32_t h = 0;  // 0 = the full frame height
};

// The cross-thread progress / cancellation channel for one render (S55-f: the dialog's proxy
// worker and the Create progress bar). The render stores rowsTotal once at entry, bumps rowsDone
// as band rows complete, and aborts between rows once cancel is set -- a cancelled render returns
// an EMPTY result (never partial pixels). Purely observational: plumbing a progress never changes
// a single output byte (the goldens stand). All atomics, so a UI thread may read while worker
// bands write.
struct TextureRenderProgress {
    std::atomic<std::uint64_t> rowsDone{0};
    std::atomic<std::uint64_t> rowsTotal{0};
    std::atomic<bool> cancel{false};
};

[[nodiscard]] TextureRenderResult renderTexture(const TextureParams& params, std::uint32_t w,
                                                std::uint32_t h,
                                                const TextureWindow& window = {},
                                                TextureRenderProgress* progress = nullptr);

// The GPU lane's injection seam (S55-h; §8.4 -- the setExtrudeRenderOverride precedent): core
// stays Vulkan-free, so the app registers the compute lane (render::TextureGpu) here at startup.
// Return true = rendered into `out` under renderTexture's exact semantics (same params -> the
// same picture within float-lane tolerance; a CANCELLED render hands back an EMPTY result);
// false = fall back to the CPU reference lane (no device, an unsupported generator/feature, a
// device error). The CPU renderers stay the permanent source of truth: the test binary NEVER
// installs an override, so the byte-pinned goldens always exercise the CPU lane, and the GPU
// lane is held to it by the tolerance-based parity tests (test_texture_gpu.cpp). Set once before
// rendering starts; renderTexture may be called from any ONE thread at a time per override call
// (the app's installer serialises with a mutex -- the dialog worker and the UI thread can race).
using TextureRenderOverride =
    std::function<bool(const TextureParams& params, std::uint32_t w, std::uint32_t h,
                       const TextureWindow& window, TextureRenderProgress* progress,
                       TextureRenderResult& out)>;
void setTextureRenderOverride(TextureRenderOverride fn);

// The per-generator trait row (S55-g; the §1.1 "registry of generators" made literal). Every
// cross-generator consumer -- renderTexture's dispatch, docio's token table, the headless
// --texture op, the dialog's rail / preset / proxy plumbing -- walks this table instead of
// switching on Generator, so adding a generator is: one params struct (texture_params.hpp), one
// render TU, one preset library, one dialog controls-builder, and ONE ROW in texture_render.cpp.
// Plain function pointers throughout: the table is constant-initialized, so mirrors built from it
// (docjson's token array) can never race static initialization.
struct GeneratorTraits {
    Generator id;
    const char* name;   // stable ASCII display/layer name ("Sky"); generatorName() reads this
    const char* token;  // stable lowercase docio/CLI token ("sky") -- FROZEN once shipped
    // Scale semantics (§8.3): true = features are pixel-sized at Scale 1 (paper + the S55-g
    // materials), so a Fit proxy multiplies Scale by proxy/doc to keep the framing honest;
    // false = the camera is frame-relative (sky/grass) and the proxy needs nothing.
    bool pixelScaledFeatures;
    void (*seedSpec)(TextureParams&);  // install the generator's default variant arm
    TextureRenderResult (*render)(const TextureParams&, std::uint32_t w, std::uint32_t h,
                                  const TextureWindow&, TextureRenderProgress*);
    // Type-erased access to the generator's preset library (the XxxPreset tables).
    std::size_t (*presetCount)();
    const char* (*presetName)(std::size_t);       // i < presetCount()
    void (*applyPreset)(TextureParams&, std::size_t);  // p.spec = library[i].params
    int (*matchPreset)(const TextureParams&);          // 0 = custom, else 1 + library index
};

// The row for `g` (invalid enums clamp to the first row; callers pass real generators).
[[nodiscard]] const GeneratorTraits& generatorTraits(Generator g);

}  // namespace mosaic::core::texture
