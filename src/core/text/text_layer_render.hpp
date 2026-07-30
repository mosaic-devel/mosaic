#pragma once

#include <cstdint>

#include "core/text/font_provider.hpp"
#include "core/text/shaping.hpp"

// The bridge between the font stack and the (font-free) compositor (docs/type-tool.md §5.4). A
// TextLayer carries a rendered-pixel cache + a measured content box that the compositor reads but
// cannot produce -- computing them needs shaping + rasterization (FreeType/HarfBuzz). The app calls
// these before each composite so the caches are current; the compositor then composites the cached
// pixels like a raster source. Cheap no-op when a layer's cache already reflects its block.
namespace mosaic::core {
class Document;
class TextLayer;
using LayerId = std::uint64_t;
}

namespace mosaic::core::text {

// Re-shape + re-rasterize `layer`'s block into its pixel cache and update its content bounds, IF the
// cache is stale (block changed since the last render), the clip state must flip, or the layer
// transform's linear part changed (item 8). The block is rasterized through that linear part (base
// resolution = 1 px/layer-unit when it is identity), so a stretched/rotated layer stays crisp -- the
// compositor then places the cache by the residual translation only. The `shaper` is reused for its
// face cache; `fonts` resolves families/fallback. `clipToArea` clamps an Area block's pixels to its
// frame box so overset text disappears (round-4 #3); pass false for the block being edited so its
// overflow stays visible. `freezeTransform` keeps the currently-baked linear instead of re-baking the
// (changing) layer transform, so a live Move/transform drag costs no re-raster per frame; the
// committed transform re-renders once. An empty block clears the pixel cache. Returns true if a
// (re)render happened.
//
// `draft` renders at HALF the bake resolution (the compositor upsamples through the same residual
// path the kMaxCacheDim soft cap uses), quartering the raster cost -- for renders that will be
// REPLACED momentarily: a live size/bend drag re-rasters the block every frame, a font-hover
// preview every hover ("scaling text is extremely laggy", "font hover still very laggy", user
// 2026-07-14). A draft cache stores its halved bake as cacheLinear, so the FIRST non-draft refresh
// fails linearClose against the requested transform and re-renders crisp -- no extra state, no way
// to forget.
bool refreshTextCache(TextLayer& layer, TextShaper& shaper, const FontProvider& fonts,
                      bool clipToArea = true, bool freezeTransform = false, bool draft = false);

// Refresh every TextLayer in the document tree (one call before compositing). `editing` (the layer
// currently being edited, or kInvalidLayerId) is rendered UNCLIPPED so its overflow shows while you
// type; all others clip Area overset to the box. `liveDrag` freezes every layer's baked transform
// (item 8) so a transform gesture in flight stays cheap (no per-frame re-raster); the gesture's
// commit re-renders the settled transform crisp. Returns true if any layer was (re)rendered.
//
// `dirtyDocOut` (optional): accumulates the DOCUMENT-space rect the refresh visibly changed -- for
// each re-rendered layer, the union of its OLD cached pixels' doc bounds and its NEW ones (padded
// for the compositor's resample footprint). This is what lets a text edit recomposite a REGION
// instead of the whole document (the S60-a brush path, S60-b for typing): a keystroke on a
// 1920x1080 document paid a ~64 ms full composite; the region it actually dirties composites in a
// couple of ms. Empty when nothing re-rendered. Pass null when a full composite follows anyway.
// `draftEditing` renders the EDITED layer at draft (half-res) quality -- see refreshTextCache's
// `draft`. The app passes it while a block-editing gesture (size/bend/path drag) or a font-hover
// preview is in flight; the settle/commit refresh re-renders crisp automatically.
bool refreshTextCaches(Document& doc, TextShaper& shaper, const FontProvider& fonts,
                       LayerId editing = static_cast<LayerId>(0), bool liveDrag = false,
                       common::Rect* dirtyDocOut = nullptr, bool draftEditing = false);

}  // namespace mosaic::core::text
