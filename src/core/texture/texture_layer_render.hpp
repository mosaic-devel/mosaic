#pragma once

#include <cstdint>

#include "common/geometry.hpp"

// The bridge between the texture generators and the (generator-free) compositor (S55-a;
// docs/texture-generator.md §3.2). A TextureLayer carries a regenerated pixel cache the
// compositor composites like a raster source but cannot produce -- producing it means running a
// generator. The app calls this before each composite so the caches are current (beside
// text::refreshTextCaches); cheap no-op when every layer's cache already reflects its params.
namespace mosaic::core {
class Document;
class TextureLayer;
}

namespace mosaic::core::texture {

struct TextureRenderResult;

// Install an already-rendered full-document result as `layer`'s pixel cache (the S55-f dialog's
// Create/Apply path: it bakes at document size on a worker thread with a progress bar, then hands
// the pixels here so the pre-composite refresh below finds a CURRENT cache instead of re-rendering
// synchronously). The one write path for the cache -- refreshTextureCache routes through it too.
void applyBakedTextureCache(TextureLayer& layer, TextureRenderResult baked, std::uint32_t docW,
                            std::uint32_t docH);

// Re-generate `layer`'s pixel cache at the document's resolution IF it is stale (params changed
// since the last render, or the cache was rendered for a different document size). The cache
// covers the whole canvas at 1 px per layer unit (cacheImageToLayer = identity); the layer
// transform places it. Returns true if a (re)render happened.
bool refreshTextureCache(TextureLayer& layer, std::uint32_t docW, std::uint32_t docH);

// Refresh every TextureLayer in the document tree (one call before compositing). Returns true if
// any layer was (re)rendered. `dirtyDocOut` (optional) accumulates the DOCUMENT-space rect the
// refresh visibly changed (old extent united with new, the text pass's contract) so a
// params edit can recomposite a region instead of the whole document.
bool refreshTextureCaches(Document& doc, common::Rect* dirtyDocOut = nullptr);

}  // namespace mosaic::core::texture
