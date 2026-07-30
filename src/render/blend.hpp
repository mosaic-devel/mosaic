#pragma once

#include "core/blend_math.hpp"

// The blend math itself moved to `core/blend_math.hpp` so the brush engine can share the single
// definition this header was always meant to provide: `mosaic_render` links Vulkan and depends on
// `mosaic_core`, so `core/brush` -- which is deliberately Vulkan- and platform-free -- cannot
// include anything from here (docs/brushes.md §6.1).
//
// The names are re-exported under `mosaic::render` so the compositor, the region fill, the layer
// effects and their tests keep spelling them the way they always have. Nothing is redefined: this
// header is an alias, and there is still exactly one copy of each formula.
namespace mosaic::render {

namespace detail = ::mosaic::core::detail; // Rgb, lum, setLum, setSat, blendNonSeparable, ...

using core::blendChannel;
using core::compositeOver;
using core::isSeparable;

}  // namespace mosaic::render
