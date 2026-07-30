#include "io/mosaic/fileinfo.hpp"

#include "common/fs_path.hpp"

#include "io/mosaic/chunk.hpp"
#include "io/mosaic/manifest_tokens.hpp"
#include "io/mosaic/format.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <vector>

namespace mosaic::io::native {

using nlohmann::json;

namespace {
constexpr std::uint32_t kMaxSurfaceDim = 30000; // docio.cpp's open-time sanity cap, mirrored
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

    const json m = json::parse(payload->begin(), payload->end(), nullptr, false);
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

std::optional<DocumentFileInfo> readDocumentInfo(const std::string& path) {
    std::ifstream f(common::pathFromUtf8(path), std::ios::binary);
    if (!f)
        return std::nullopt;
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                          std::istreambuf_iterator<char>());
    return documentInfoInFile(bytes);
}

} // namespace mosaic::io::native
