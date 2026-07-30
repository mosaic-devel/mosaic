#include "io/mosaic/preview.hpp"

#include "common/fs_path.hpp"

#include "io/mosaic/chunk.hpp"
#include "io/mosaic/paeth.hpp"
#include "io/mosaic/wire.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace mosaic::io::native {

common::Image downscalePreview(const common::Image& src, std::uint32_t maxEdge) {
    if (src.empty() || maxEdge == 0)
        return {};
    const std::uint32_t longest = std::max(src.width, src.height);
    if (longest <= maxEdge)
        return src; // never upscale: a small canvas ships at its own size
    const double scale = static_cast<double>(maxEdge) / longest;
    const auto dstW = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(std::lround(src.width * scale)));
    const auto dstH = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(std::lround(src.height * scale)));

    common::Image dst(dstW, dstH);
    const auto sw = static_cast<double>(src.width);
    const auto sh = static_cast<double>(src.height);
    for (std::uint32_t dy = 0; dy < dstH; ++dy) {
        // The source band this destination row averages: [y0, y1) with at least one row.
        const auto y0 = static_cast<std::uint32_t>(dy * sh / dstH);
        const auto y1 = std::min<std::uint32_t>(
            std::max<std::uint32_t>(static_cast<std::uint32_t>((dy + 1) * sh / dstH), y0 + 1),
            src.height);
        for (std::uint32_t dx = 0; dx < dstW; ++dx) {
            const auto x0 = static_cast<std::uint32_t>(dx * sw / dstW);
            const auto x1 = std::min<std::uint32_t>(
                std::max<std::uint32_t>(static_cast<std::uint32_t>((dx + 1) * sw / dstW), x0 + 1),
                src.width);
            // Premultiplied accumulation: a fully transparent pixel contributes no colour, so
            // whatever undefined RGB it carries cannot tint the edge it borders.
            std::uint64_t rgb[3] = {0, 0, 0};
            std::uint64_t alpha = 0;
            for (std::uint32_t sy = y0; sy < y1; ++sy) {
                const std::uint8_t* row =
                    src.rgba.data() + (static_cast<std::size_t>(sy) * src.width + x0) * 4;
                for (std::uint32_t sx = x0; sx < x1; ++sx, row += 4) {
                    const std::uint64_t a = row[3];
                    rgb[0] += row[0] * a;
                    rgb[1] += row[1] * a;
                    rgb[2] += row[2] * a;
                    alpha += a;
                }
            }
            const std::uint64_t n = static_cast<std::uint64_t>(x1 - x0) * (y1 - y0);
            std::uint8_t* out = dst.rgba.data() +
                                (static_cast<std::size_t>(dy) * dstW + dx) * 4;
            if (alpha == 0) {
                out[0] = out[1] = out[2] = out[3] = 0;
            } else {
                for (int c = 0; c < 3; ++c)
                    out[c] = static_cast<std::uint8_t>((rgb[c] + alpha / 2) / alpha);
                out[3] = static_cast<std::uint8_t>((alpha + n / 2) / n);
            }
        }
    }
    return dst;
}

std::vector<std::uint8_t> encodePreviewPayload(const common::Image& img) {
    std::vector<std::uint8_t> out;
    if (img.empty())
        return out;
    out.resize(kPreviewHeaderSize + img.rgba.size());
    detail::storeLe32(out.data(), img.width);
    detail::storeLe32(out.data() + 4, img.height);
    filterPaethRgba(img.rgba, img.width, img.height,
                    {out.data() + kPreviewHeaderSize, img.rgba.size()});
    return out;
}

std::optional<common::Image> decodePreviewPayload(std::span<const std::uint8_t> payload) {
    if (payload.size() < kPreviewHeaderSize)
        return std::nullopt;
    const std::uint32_t w = detail::loadLe32(payload.data());
    const std::uint32_t h = detail::loadLe32(payload.data() + 4);
    if (w == 0 || h == 0 || w > kMaxPreviewDim || h > kMaxPreviewDim)
        return std::nullopt;
    if (payload.size() != kPreviewHeaderSize + static_cast<std::size_t>(w) * h * 4)
        return std::nullopt;
    common::Image img(w, h);
    std::memcpy(img.rgba.data(), payload.data() + kPreviewHeaderSize, img.rgba.size());
    unfilterPaethRgba(img.rgba, w, h);
    return img;
}

FileChunk makePreviewChunk(const common::Image& composite) {
    FileChunk c;
    c.type = kTypePreview;
    c.key = zeroKey(); // a singleton: generation disambiguates (format.hpp)
    c.generation = 0;
    c.profile = Profile::Max; // written once, read many (spec 2.4)
    c.flags = kFlagCritical | kFlagFiltered;
    c.parity = false; // spec 2.7 stripes tile/vector content; a lost preview is a regenerate
    c.payload = encodePreviewPayload(downscalePreview(composite, kPreviewEdge));
    return c;
}

void seedPreviewFromReport(CheckpointInput& in, const OpenReport& report) {
    const RecoveredChunk* prvw = report.find(kTypePreview, zeroKey());
    if (prvw == nullptr)
        return;
    FileChunk c;
    c.type = kTypePreview;
    c.key = zeroKey();
    c.generation = 0;
    c.profile = Profile::Max;
    // Normalize transport bits: a committed-region frame reads back with kFlagLinked set, which
    // says where the frame LIVED, not what it IS -- left in, the diff's flag compare would call
    // every unchanged preview dirty.
    c.flags = static_cast<std::uint8_t>(prvw->flags & ~kFlagLinked);
    c.parity = false;
    c.payload = prvw->payload;
    in.chunks.push_back(std::move(c));
}

std::optional<common::Image> newestPreviewInFile(std::span<const std::uint8_t> file) {
    // A verified linear scan, decompressing nothing but the winner: the thumbnailer must not pay
    // for the document's tiles. `>=` keeps the LATER frame on a generation tie -- the same
    // replica rule readers apply everywhere (format.hpp).
    std::optional<ChunkRecord> best;
    for (const ChunkRecord& rec : scanChunks(file)) {
        if (!rec.valid || rec.type != kTypePreview)
            continue;
        if (!best.has_value() || rec.generation >= best->generation)
            best = rec;
    }
    if (!best.has_value())
        return std::nullopt;
    const auto payload = decodeChunkPayload(*best, file);
    if (!payload.has_value())
        return std::nullopt;
    return decodePreviewPayload(*payload);
}

std::optional<common::Image> readNewestPreview(const std::string& path, std::string* error) {
    std::ifstream f(common::pathFromUtf8(path), std::ios::binary);
    if (!f) {
        if (error != nullptr)
            *error = "could not open the file";
        return std::nullopt;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                          std::istreambuf_iterator<char>());
    auto img = newestPreviewInFile(bytes);
    if (!img.has_value() && error != nullptr)
        *error = "the file carries no readable preview";
    return img;
}

} // namespace mosaic::io::native
