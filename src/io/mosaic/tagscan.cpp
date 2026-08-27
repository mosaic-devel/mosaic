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
        const std::optional<ChunkRecord> rec = parseChunkAt(frame, 0);
        if (!rec.has_value() || !rec->valid) {
            // ⚠ A FRAME THAT FAILED ITS CHECKSUM HAS AN UNTRUSTWORTHY LENGTH, and we have just
            // learned that at no extra cost -- so do not step by it. scanChunks has always had
            // this rule ("a valid chunk is consumed wholesale; anything else advances one byte
            // and resyncs"); this walk skipped it and could be steered.
            //
            // The attack it closes is in the corpus as 19-adversarial-frame-skip: the length field
            // is checksum-covered, so a crafted length necessarily invalidates its own frame -- and
            // stepping by it lands past a frame the resyncing reader would have found. A file that
            // hid its newest VECT that way made this walk and the in-memory scan disagree.
            pos = findNextMagic(f, size, pos + 1);
            continue;
        }
        if (std::optional<std::vector<std::uint8_t>> payload = decodeChunkPayload(*rec, frame)) {
            out[want] = std::move(payload);
            bestGeneration[want] = rec->generation;
            haveBest[want] = true;
        }
        pos += head->frameLength; // verified, so its length is the container's own word
    }
    return out;
}

} // namespace mosaic::io::native
