#include "io/mosaic/fileinfo.hpp"

#include "common/fs_path.hpp"

#include "io/mosaic/chunk.hpp"
#include "io/mosaic/manifest_tokens.hpp"
#include "io/mosaic/format.hpp"
#include "io/mosaic/preview.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <vector>

namespace mosaic::io::native {

using nlohmann::json;

namespace {
constexpr std::uint32_t kMaxSurfaceDim = 30000; // docio.cpp's open-time sanity cap, mirrored
} // namespace

namespace {
// The manifest JSON -> DocumentFileInfo half, split out so the single-pass card reader below can
// share it with documentInfoInFile rather than re-implementing the tolerance rules.
[[nodiscard]] std::optional<DocumentFileInfo> parseManifestInfo(
    std::span<const std::uint8_t> payload) {
    const json m = json::parse(payload.begin(), payload.end(), nullptr, false);
    if (m.is_discarded() || !m.is_object())
        return std::nullopt;
    // Tolerant on purpose: a card caption is best-effort, so only the canvas size is required.
    // (docio's full open stays the strict reader.)
    if (!m.contains("canvas") || !m["canvas"].is_object())
        return std::nullopt;
    const json& canvas = m["canvas"];
    if (!canvas.contains("w") || !canvas["w"].is_number_unsigned() || !canvas.contains("h") ||
        !canvas["h"].is_number_unsigned())
        return std::nullopt;
    const std::uint64_t w = canvas["w"].get<std::uint64_t>();
    const std::uint64_t h = canvas["h"].get<std::uint64_t>();
    if (w == 0 || h == 0 || w > kMaxSurfaceDim || h > kMaxSurfaceDim)
        return std::nullopt;

    DocumentFileInfo info;
    info.width = static_cast<std::uint32_t>(w);
    info.height = static_cast<std::uint32_t>(h);
    if (canvas.contains("dpi") && canvas["dpi"].is_number())
        info.dpi = canvas["dpi"].get<double>();
    if (m.contains("title") && m["title"].is_string())
        info.title = m["title"].get<std::string>();
    if (m.contains("color") && m["color"].is_object()) {
        const json& color = m["color"];
        if (color.contains("space") && color["space"].is_string())
            info.colorSpace = detail::colorSpaceFromToken(color["space"].get<std::string>());
        if (color.contains("precision") && color["precision"].is_string())
            info.precision = detail::precisionFromToken(color["precision"].get<std::string>());
    }
    return info;
}
} // namespace

std::optional<DocumentFileInfo> documentInfoInFile(std::span<const std::uint8_t> file) {
    // Newest-generation-wins over the valid MFST frames (`>=` keeps the later frame on a tie --
    // the replica rule readers apply everywhere, format.hpp), exactly like the preview reader.
    std::optional<ChunkRecord> best;
    for (const ChunkRecord& rec : scanChunks(file)) {
        if (!rec.valid || rec.type != kTypeManifest)
            continue;
        if (!best.has_value() || rec.generation >= best->generation)
            best = rec;
    }
    if (!best.has_value())
        return std::nullopt;
    const auto payload = decodeChunkPayload(*best, file);
    if (!payload.has_value())
        return std::nullopt;
    return parseManifestInfo(*payload);
}

std::optional<DocumentFileInfo> readDocumentInfo(const std::string& path) {
    std::vector<std::uint8_t> bytes;
    if (!common::readWholeFile(path, bytes))
        return std::nullopt;
    return documentInfoInFile(bytes);
}

DocumentCard readDocumentCard(const std::string& path) {
    // ⚠ ONE read and ONE scan for BOTH answers. The dialog wants a manifest and a preview per
    // recent, and asking for them separately meant reading the file twice and walking every chunk
    // frame twice -- 231 ms per card on a 302 MB document, for two selections out of the same
    // walk. The frames are the expensive part (33,664 of them there), not the picking.
    DocumentCard card;
    std::vector<std::uint8_t> bytes;
    if (!common::readWholeFile(path, bytes))
        return card;
    std::optional<ChunkRecord> bestManifest;
    std::optional<ChunkRecord> bestPreview;
    for (const ChunkRecord& rec : scanChunks(bytes)) {
        if (!rec.valid)
            continue;
        if (rec.type == kTypeManifest) {
            if (!bestManifest.has_value() || rec.generation >= bestManifest->generation)
                bestManifest = rec;
        } else if (rec.type == kTypePreview) {
            if (!bestPreview.has_value() || rec.generation >= bestPreview->generation)
                bestPreview = rec;
        }
    }
    if (bestManifest.has_value())
        if (const auto payload = decodeChunkPayload(*bestManifest, bytes))
            card.info = parseManifestInfo(*payload);
    if (bestPreview.has_value())
        if (const auto payload = decodeChunkPayload(*bestPreview, bytes))
            card.preview = decodePreviewPayload(*payload);
    return card;
}

} // namespace mosaic::io::native
