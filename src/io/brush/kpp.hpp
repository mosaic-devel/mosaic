#pragma once

#include "common/image.hpp"
#include "io/brush/preset.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// The `.kpp` container reader (docs/brushes.md §3.1): a PNG whose `preset` text chunk carries the
// XML -- zTXt in 213 of the shipped 248, plain tEXt in 35, found by KEYWORD either way -- and
// whose raster IS the preset's icon. The `version` text must be `2.2` or `5.0`; anything else,
// including an absent chunk, is rejected exactly as the producer rejects it.
//
// Reading the preset and decoding the icon are separate calls: a library scan touches hundreds of
// files, and a 200x200 RGBA thumbnail per preset is ~160 KB it should only pay for the presets a
// panel actually shows.
namespace mosaic::io::brush {

// Container + XML + mapper in one step: scan the chunks, check the version, parse the document,
// map to a BrushPreset (provenance sourceFormat "kpp"). Nullopt + reason when the container, the
// version or the document is unusable; option-level trouble lands in the provenance instead.
[[nodiscard]] std::optional<BrushPreset> readKpp(const std::uint8_t* data, std::size_t size,
                                                 std::string* error = nullptr);

// Decode the PNG raster -- the preset's icon/thumbnail. Independent of readKpp by design.
[[nodiscard]] std::optional<common::Image> readKppIcon(const std::uint8_t* data, std::size_t size,
                                                       std::string* error = nullptr);

} // namespace mosaic::io::brush
