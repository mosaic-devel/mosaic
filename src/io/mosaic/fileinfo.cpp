#include "io/mosaic/fileinfo.hpp"

#include "common/fs_path.hpp"

#include "io/mosaic/chunk.hpp"
#include "io/mosaic/manifest_tokens.hpp"
#include "io/mosaic/format.hpp"
#include "io/mosaic/preview.hpp"
#include "io/mosaic/tagscan.hpp"

#include <nlohmann/json.hpp>

#include <array>
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
    // ⚠ THE TAPE HEAD GOES TO THE DISK, not the disk to memory. A card needs two frames -- the
    // newest manifest and the newest preview -- out of a document that may hold tens of thousands,
    // and pulling the whole file in to find them read 302 MB and hashed every tile to answer a
    // question about two small chunks near the end. tagscan walks the frame chain on disk, reads
    // 46-byte headers, and seeks past every payload it was not asked for.
    const std::array<ChunkTag, 2> tags{kTypeManifest, kTypePreview};
    std::vector<std::optional<std::vector<std::uint8_t>>> found =
        readNewestChunkPayloads(path, tags);
    DocumentCard card;
    if (found.size() != tags.size())
        return card;
    if (found[0].has_value())
        card.info = parseManifestInfo(*found[0]);
    if (found[1].has_value())
        card.preview = decodePreviewPayload(*found[1]);
    return card;
}

} // namespace mosaic::io::native
