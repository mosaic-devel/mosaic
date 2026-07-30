#pragma once

#include "common/image.hpp"
#include "io/brush/preset.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The native `.mbp` preset container (docs/brushes.md §7): a PNG whose raster IS the preset's
// icon -- thumbnails appear free in file managers -- with the preset itself riding in an `iTXt`
// chunk keyed `mosaic-preset`, carrying JSON. Deliberately symmetric with `.kpp` (same walker,
// same keyword-not-chunk-type lookup, same icon-is-the-raster rule), but JSON instead of XML and
// OUR model serialized directly: a BrushPreset round-trips with full fidelity, nothing staged.
//
// The JSON is schema-versioned (`"schema": 1`). Reading is STRICT: this is Mosaic's own format,
// so a missing field, a foreign enum string or a newer schema is a load failure with a reason --
// never a silently-different brush. (Third-party formats get the total-function treatment
// because their files predate us; a corrupt .mbp is OUR bug or OUR future, and both must be
// loud.) Structural caps still apply -- the container walk inherits png_text's budgets, and the
// deserializer bounds every list -- because any file can be hostile regardless of extension.
//
// Numbers round-trip exactly: nlohmann emits shortest-round-trip doubles and is
// locale-independent, so the LC_NUMERIC rule is satisfied by construction; curves serialize
// through Curve::toString (the interchange spelling with '.').
namespace mosaic::io::brush {

// The iTXt keyword. Matched exactly, by keyword -- never by chunk type (the .kpp lesson).
inline constexpr std::string_view kMbpKeyword = "mosaic-preset";

inline constexpr int kMbpSchema = 1;

// Bounds on the deserialized structure (a hostile payload can nest/repeat arbitrarily).
inline constexpr int kMaxMbpOptions = 64;          // the option table carries 18
inline constexpr int kMaxMbpSensorsPerOption = 16; // one per SensorId at most

// Serialize `preset` + `icon` into a .mbp container. The icon is encoded as an ordinary RGBA
// PNG and the preset JSON spliced in as `iTXt` before IEND. Nullopt + reason on an unencodable
// icon (empty image) -- the preset itself always serializes.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> writeMbp(const BrushPreset& preset,
                                                                const common::Image& icon,
                                                                std::string* error = nullptr);

// Parse a .mbp held in memory. Strict, per the header comment.
[[nodiscard]] std::optional<BrushPreset> readMbp(const std::uint8_t* data, std::size_t size,
                                                 std::string* error = nullptr);

// Decode the PNG raster -- the preset's icon. Independent of readMbp, like readKppIcon.
[[nodiscard]] std::optional<common::Image> readMbpIcon(const std::uint8_t* data, std::size_t size,
                                                       std::string* error = nullptr);

// The preset's JSON payload alone (no container): what writeMbp embeds and readMbp parses.
// Exposed for tests and future consumers that store presets outside a container (the settings
// sync of a far-future session). Deterministic: equal presets serialize to equal strings.
[[nodiscard]] std::string presetToJson(const BrushPreset& preset);
[[nodiscard]] std::optional<BrushPreset> presetFromJson(std::string_view json,
                                                        std::string* error = nullptr);

} // namespace mosaic::io::brush
