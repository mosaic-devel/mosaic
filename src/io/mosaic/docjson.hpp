#pragma once

#include "common/exif.hpp"
#include "core/document.hpp"
#include "core/layer_effects.hpp"
#include "core/text/text_model.hpp"
#include "core/texture/texture_params.hpp"
#include "core/vector/object.hpp"
#include "io/mosaic/manifest_tokens.hpp"

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>

// mosaic/docjson -- the JSON spellings of the document model inside .mosaic payloads (the
// manifest, VECT chunks, HIST snapshots later). These spellings ARE the wire format: they are
// the format's own stable lowercase tokens, deliberately independent of the UI-facing name
// functions (blendModeName etc.), which are free to change; these are not. Parsing is strict --
// an unknown token or malformed field returns nullopt, never a silent default: a native format
// that guesses is a native format that corrupts.
namespace mosaic::io::native::detail {

// Layer-payload codecs.
[[nodiscard]] nlohmann::json vectorObjectToJson(const core::vec::Object& o);
[[nodiscard]] std::optional<core::vec::Object> vectorObjectFromJson(const nlohmann::json& j);
[[nodiscard]] nlohmann::json textBlockToJson(const core::text::TextBlock& b);
[[nodiscard]] std::optional<core::text::TextBlock> textBlockFromJson(const nlohmann::json& j);
[[nodiscard]] nlohmann::json effectsToJson(const core::LayerEffects& fx);
[[nodiscard]] std::optional<core::LayerEffects> effectsFromJson(const nlohmann::json& j);
[[nodiscard]] nlohmann::json exifToJson(const common::ExifData& e);
[[nodiscard]] std::optional<common::ExifData> exifFromJson(const nlohmann::json& j);
// S35-b: the per-layer warp lattice ("warp"). Additive and optional exactly like "exif" above --
// absent is fine, present-but-malformed refuses the file, and the manifest schema version does not
// move for it (docs/warp-tools.md §7).
[[nodiscard]] nlohmann::json warpToJson(const core::WarpGrid& g);
[[nodiscard]] std::optional<core::WarpGrid> warpFromJson(const nlohmann::json& j);
[[nodiscard]] nlohmann::json textureParamsToJson(const core::texture::TextureParams& p);
[[nodiscard]] std::optional<core::texture::TextureParams> textureParamsFromJson(
    const nlohmann::json& j);

// Shared small pieces the manifest also needs.
[[nodiscard]] nlohmann::json affineToJson(const common::Affine2D& t);
[[nodiscard]] std::optional<common::Affine2D> affineFromJson(const nlohmann::json& j);
[[nodiscard]] const char* blendModeToken(core::BlendMode m);
[[nodiscard]] std::optional<core::BlendMode> blendModeFromToken(const std::string& s);
[[nodiscard]] const char* adjustmentKindToken(core::AdjustmentKind k);
[[nodiscard]] std::optional<core::AdjustmentKind> adjustmentKindFromToken(const std::string& s);
// The manifest's own colour-space / precision tokens live in manifest_tokens.hpp (included
// above) so that reading a manifest does not link the whole document serializer. They are part of
// this same detail namespace, so every existing caller is unaffected.

} // namespace mosaic::io::native::detail
