#pragma once

#include "core/document.hpp" // core::ColorSpace / core::Precision (the enums only)

#include <optional>
#include <string>

// mosaic/manifest_tokens -- the document-level enum spellings the MANIFEST carries: the colour
// space and the sample precision.
//
// These live apart from docjson (which owns every other JSON spelling in the format) for a link
// reason, not a taste one. docjson.cpp serializes every layer KIND, so it references the text
// engine, the vector model and the texture generator; pulling one token helper out of it drags all
// three into whatever links it. The manifest readers -- the Quick Look extensions, the New-Document
// dialog's recent cards -- want a canvas size and two enum tokens and nothing else, and a static
// linker resolves at object-file granularity, so they were paying for the entire document model to
// spell "srgb". In their own translation unit these helpers cost what they look like they cost.
//
// The spellings ARE the wire format (docjson.hpp's rule applies unchanged): stable lowercase
// tokens, independent of the UI-facing name functions, and parsing is strict -- an unknown token
// returns nullopt, never a silent default.
namespace mosaic::io::native::detail {

[[nodiscard]] const char* colorSpaceToken(core::ColorSpace cs);
[[nodiscard]] std::optional<core::ColorSpace> colorSpaceFromToken(const std::string& s);
[[nodiscard]] const char* precisionToken(core::Precision p);
[[nodiscard]] std::optional<core::Precision> precisionFromToken(const std::string& s);

} // namespace mosaic::io::native::detail
