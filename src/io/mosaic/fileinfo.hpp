#pragma once

#include "common/image.hpp"  // the card's preview bitmap
#include "core/document.hpp" // core::ColorSpace / core::Precision

#include <cstdint>
#include <optional>
#include <span>
#include <string>

// mosaic/fileinfo -- a light "what document is this" reader for the New-Document dialog's
// recents/template cards (S48-b follow-up): the newest manifest's canvas size, dpi and colour
// tokens from a verified linear chunk scan that decompresses ONLY the manifest. Never touches
// layers or tiles, so asking ten recents costs milliseconds, not decodes. The PRVW sibling
// (preview.hpp readNewestPreview) supplies the matching card image.
namespace mosaic::io::native {

struct DocumentFileInfo {
    std::uint32_t width = 0;  // canvas px (always present in a readable manifest)
    std::uint32_t height = 0;
    double dpi = 72.0;
    std::string title; // the document's own name; empty when the manifest carries none
    std::optional<core::ColorSpace> colorSpace; // nullopt = token unknown to this build
    std::optional<core::Precision> precision;
};

// The newest readable manifest's info, or nullopt when no manifest survives / parses. Same
// newest-generation-wins replica rule as the preview reader.
[[nodiscard]] std::optional<DocumentFileInfo> documentInfoInFile(
    std::span<const std::uint8_t> file);

// File variant: reads `path` fully (the scan needs the frame walk) and defers to the above.
[[nodiscard]] std::optional<DocumentFileInfo> readDocumentInfo(const std::string& path);

// Both halves of a recents/template card, from ONE read and ONE chunk walk.
//
// The dialog needs a manifest AND a preview per file, and asking readDocumentInfo and
// readNewestPreview separately reads the file twice and walks every chunk frame twice. The frames
// are the cost -- 33,664 of them in a 302 MB document -- not the selecting, so doing both
// selections in one pass halves the card. Either half is nullopt when the file does not carry it:
// a document saved before previews existed, or one written by a tool that emits no PRVW.
struct DocumentCard {
    std::optional<DocumentFileInfo> info;
    std::optional<common::Image> preview;
};
[[nodiscard]] DocumentCard readDocumentCard(const std::string& path);

} // namespace mosaic::io::native
