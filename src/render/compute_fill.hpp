#pragma once

#include <cstdint>

#include "common/image.hpp"
#include "render/render.hpp"

namespace mosaic::render {

// GPU compute fill: dispatches the embedded `fill.comp` shader to write `color` into every
// texel of an offscreen storage image, then reads it back. Exercises the full
// GLSL -> SPIR-V -> embedded -> compute-pipeline path. Returns a RenderResult with
// usedBackend == Backend::GpuCompute (ok == false with an error if Vulkan is unavailable).
RenderResult computeSolid(std::uint32_t w, std::uint32_t h, common::Color8 color);

}  // namespace mosaic::render
