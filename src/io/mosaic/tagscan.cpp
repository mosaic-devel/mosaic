#include "io/mosaic/tagscan.hpp"

#include "common/fs_path.hpp"

#include "io/mosaic/chunk.hpp"

#include <algorithm>
#include <fstream>

namespace mosaic::io::native {
namespace {

// The resync window. Big enough that hunting for the next MAGIC across a damaged region is a
// handful of reads rather than thousands, small enough that a corrupt file cannot make the walk
// allocate anything interesting.
constexpr std::size_t kResyncWindow = 1u << 16;

// The offset of the next MAGIC at or after `from`, or `size` when there is none. Reads in
// overlapping windows so a magic straddling a window boundary is still found.
[[nodiscard]] std::uint64_t findNextMagic(std::ifstream& f, std::uint64_t size, std::uint64_t from) {
    const std::size_t overlap = kChunkMagic.size() - 1;
    std::vector<std::uint8_t> win;
    for (std::uint64_t pos = from; pos < size;) {
        const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(kResyncWindow, size - pos));
        win.resize(want);
        f.seekg(static_cast<std::streamoff>(pos));
        if (!f.read(reinterpret_cast<char*>(win.data()), static_cast<std::streamsize>(want)))
            return size;
        const auto it = std::search(win.begin(), win.end(), kChunkMagic.begin(), kChunkMagic.end());
        if (it != win.end())
            return pos + static_cast<std::uint64_t>(it - win.begin());
        if (want <= overlap)
            return size;
        pos += want - overlap; // step back so a straddling magic is not missed
    }
    return size;
}

} // namespace

std::vector<std::optional<std::vector<std::uint8_t>>> readNewestChunkPayloads(
    const std::string& path, std::span<const ChunkTag> tags) {
    std::vector<std::optional<std::vector<std::uint8_t>>> out(tags.size());
    std::vector<std::uint64_t> bestGeneration(tags.size(), 0);
    std::vector<bool> haveBest(tags.size(), false);
    if (tags.empty())
        return out;

    std::ifstream f(common::pathFromUtf8(path), std::ios::binary | std::ios::ate);
    if (!f)
        return out;
    const std::streamoff end = f.tellg();
    if (end <= 0)
        return out;
    const auto size = static_cast<std::uint64_t>(end);

    std::vector<std::uint8_t> header(kHeaderSize);
    std::vector<std::uint8_t> frame;

    std::uint64_t pos = 0;
    while (pos + kHeaderSize <= size) {
        f.seekg(static_cast<std::streamoff>(pos));
        if (!f.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(kHeaderSize)))
            break;

        const std::optional<ChunkHeaderView> head = parseChunkHeader(header);
        if (!head.has_value()) { // no MAGIC here: hunt for the next one
            pos = findNextMagic(f, size, pos + 1);
            continue;
        }
        // A length field claiming more than the file holds is damage, not a frame. Resync past it
        // rather than trusting it -- the same posture scanChunks takes on an incomplete record.
        if (head->frameLength < kHeaderSize || head->frameLength > size - pos) {
            pos = findNextMagic(f, size, pos + 1);
            continue;
        }

        // Is this a tag anyone asked for, and could it beat what we already have? The generation
        // test comes first so a file with a thousand copies of a wanted tag still reads only the
        // ones that could win.
        std::size_t want = tags.size();
        for (std::size_t i = 0; i < tags.size(); ++i) {
            if (tags[i] == head->type && (!haveBest[i] || head->generation >= bestGeneration[i])) {
                want = i;
                break;
            }
        }
        if (want == tags.size()) { // not wanted: seek past the payload, never read it
            pos += head->frameLength;
            continue;
        }

        // Wanted: read the whole frame and verify it exactly as the in-memory scan would.
        frame.resize(static_cast<std::size_t>(head->frameLength));
        f.seekg(static_cast<std::streamoff>(pos));
        if (!f.read(reinterpret_cast<char*>(frame.data()),
                    static_cast<std::streamsize>(frame.size())))
            break;
        // ⚠ The checksum gate is decodeChunkPayload's -- it returns nullopt for any record that
        // did not verify -- so `rec->valid` here is a local restatement of the contract, not the
        // thing enforcing it. Dropping it changes no behaviour (verified: the corrupt corpus
        // cannot tell the difference). It stays because a reader of this loop should not have to
        // go one layer down to learn that an unverified frame is never returned.
        const std::optional<ChunkRecord> rec = parseChunkAt(frame, 0);
        if (rec.has_value() && rec->valid) {
            if (std::optional<std::vector<std::uint8_t>> payload = decodeChunkPayload(*rec, frame)) {
                out[want] = std::move(payload);
                bestGeneration[want] = rec->generation;
                haveBest[want] = true;
            }
        }
        // Valid or not, the frame's own length is the best guess at where the next one starts; a
        // wrong guess lands on no magic and resyncs on the next turn of the loop.
        pos += head->frameLength;
    }
    return out;
}

} // namespace mosaic::io::native
