#include "io/brush/tip_io.hpp"

#include "io/brush/bytes.hpp"
#include "io/detail.hpp"

#include <algorithm>
#include <cstring>

// The Photoshop .abr sampled-brush collection (docs/brushes.md §3.6): versions 1/2 and 6.1/6.2,
// big-endian, PackBits-compressed scanlines. Only SAMPLED brushes carry pixels; computed
// (parametric) brushes are skipped and counted. The layouts below are transcribed from the
// reference reader; deviations are deliberate and safety-only:
//   * a computed v1/v2 brush is skipped to the NEXT brush (the reference's seek arithmetic for
//     that branch is broken and silently loses the rest of the file);
//   * the PackBits decoder bounds every write (the reference trusts the scanline lengths);
//   * depth != 8 is skipped (the reference sizes its buffer for 16-bit but then reads it 8-bit).
namespace mosaic::io::brush {
namespace {

namespace cb = mosaic::core::brush;
constexpr std::uint32_t kMaxTipDim = mosaic::io::detail::kMaxDim;

// PackBits with a per-scanline compressed-length table in front (heights x u16be). Writes at
// most `out.size()` bytes; anything the stream fails to supply stays 0 (transparent-ish, and
// deterministic).
[[nodiscard]] bool rleDecode(detail::ByteReader& r, std::vector<std::uint8_t>& out,
                             std::uint32_t height) {
    std::vector<std::uint16_t> lens(height);
    for (std::uint32_t y = 0; y < height; ++y)
        lens[y] = r.u16be();
    if (!r.ok())
        return false;

    std::size_t w = 0; // write cursor over `out`
    for (std::uint32_t y = 0; y < height; ++y) {
        std::uint32_t j = 0;
        while (j < lens[y]) {
            const std::int8_t n = static_cast<std::int8_t>(r.u8());
            ++j;
            if (!r.ok())
                return false;
            if (n == -128)
                continue; // nop
            if (n < 0) {
                const std::uint8_t ch = r.u8();
                ++j;
                const std::size_t run = static_cast<std::size_t>(-static_cast<int>(n)) + 1;
                const std::size_t fit = std::min(run, out.size() - w);
                std::memset(out.data() + w, ch, fit);
                w += fit;
            } else {
                const std::size_t run = static_cast<std::size_t>(n) + 1;
                const std::uint8_t* src = r.bytes(run);
                if (src == nullptr)
                    return false;
                j += static_cast<std::uint32_t>(run);
                const std::size_t fit = std::min(run, out.size() - w);
                std::memcpy(out.data() + w, src, fit);
                w += fit;
            }
        }
    }
    return r.ok();
}

// The sample body shared by every version once the cursor sits at its bounds: top/left/bottom/
// right (i32), depth (i16), compression (u8), pixels. Returns a frame in the tip-image
// convention -- an ABR sample byte is coverage, like a GBR's, so it inverts on the way in.
[[nodiscard]] bool readSampleBody(detail::ByteReader& r, cb::TipFrame& frame) {
    const std::int32_t top = r.i32be();
    const std::int32_t left = r.i32be();
    const std::int32_t bottom = r.i32be();
    const std::int32_t right = r.i32be();
    const std::int16_t depth = r.i16be();
    const std::uint8_t compression = r.u8();
    if (!r.ok())
        return false;

    const std::int64_t width = std::int64_t(right) - left;
    const std::int64_t height = std::int64_t(bottom) - top;
    if (width <= 0 || height <= 0 || width > kMaxTipDim || height > kMaxTipDim)
        return false;
    const std::uint64_t pixels = std::uint64_t(width) * std::uint64_t(height);
    if (pixels > cb::kMaxTipPixels)
        return false;
    if (depth != 8)
        return false; // §3.6: 8-bit samples only; see the file comment

    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(pixels), 0);
    if (compression == 0) {
        const std::uint8_t* src = r.bytes(coverage.size());
        if (src == nullptr)
            return false;
        std::memcpy(coverage.data(), src, coverage.size());
    } else {
        if (!rleDecode(r, coverage, static_cast<std::uint32_t>(height)))
            return false;
    }

    frame.width = static_cast<std::uint32_t>(width);
    frame.height = static_cast<std::uint32_t>(height);
    frame.rgba.resize(coverage.size() * 4);
    for (std::size_t i = 0; i < coverage.size(); ++i) {
        const std::uint8_t grey = static_cast<std::uint8_t>(255 - coverage[i]);
        frame.rgba[i * 4 + 0] = grey;
        frame.rgba[i * 4 + 1] = grey;
        frame.rgba[i * 4 + 2] = grey;
        frame.rgba[i * 4 + 3] = 255;
    }
    return true;
}

// UCS-2 name: u32 character count, then that many u16be code units. Only v2 samples carry one.
[[nodiscard]] std::string readUcs2Name(detail::ByteReader& r) {
    constexpr std::uint32_t kMaxNameChars = 4096;
    const std::uint32_t count = r.u32be();
    if (count == 0 || count > kMaxNameChars) {
        if (count > kMaxNameChars)
            r.skip(std::size_t(count) * 2);
        return {};
    }
    std::string utf8;
    utf8.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint16_t c = r.u16be();
        // Minimal UTF-16 -> UTF-8 for the BMP; surrogates (rare in brush names) are dropped.
        if (c == 0)
            continue;
        if (c < 0x80) {
            utf8 += static_cast<char>(c);
        } else if (c < 0x800) {
            utf8 += static_cast<char>(0xC0 | (c >> 6));
            utf8 += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0xD800 || c >= 0xE000) {
            utf8 += static_cast<char>(0xE0 | (c >> 12));
            utf8 += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            utf8 += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return utf8;
}

void finishMaskTip(TipFile& tip) {
    tip.sourceKind = cb::TipSourceKind::Mask;
    tip.defaultApplication = cb::TipApplication::AlphaMask;
    tip.hasColorAndTransparency = false; // an .abr brush is never a colorful-class brush
}

// Seek the cursor to just after the named 8BIM section tag. False when the section is absent or
// the tag stream is malformed.
[[nodiscard]] bool reach8bimSection(detail::ByteReader& r, const char name[4]) {
    while (r.ok() && r.remaining() > 0) {
        const std::uint8_t* tag = r.bytes(4);
        const std::uint8_t* tagname = r.bytes(4);
        if (tag == nullptr || tagname == nullptr || std::memcmp(tag, "8BIM", 4) != 0)
            return false;
        if (std::memcmp(tagname, name, 4) == 0)
            return true;
        const std::uint32_t sectionSize = r.u32be();
        r.skip(sectionSize);
    }
    return false;
}

} // namespace

std::optional<std::vector<TipFile>> readAbr(const std::uint8_t* data, std::size_t size,
                                            std::string* error, int* droppedBrushes) {
    const auto fail = [&](const char* why) -> std::optional<std::vector<TipFile>> {
        if (error != nullptr)
            *error = why;
        return std::nullopt;
    };
    int dropped = 0;
    std::vector<TipFile> out;

    detail::ByteReader r(data, size);
    const std::int16_t version = r.i16be();
    if (!r.ok())
        return fail("not an ABR: too short");

    if (version == 1 || version == 2) {
        const std::uint16_t count = r.u16be();
        for (std::uint16_t i = 0; i < count && r.ok(); ++i) {
            const std::int16_t brushType = r.i16be();
            const std::uint32_t brushSize = r.u32be();
            if (!r.ok())
                break;
            const std::size_t next = r.pos() + brushSize;

            TipFile tip;
            bool loaded = false;
            if (brushType == 2) {
                r.skip(6); // 4 misc bytes + 2 spacing bytes, discarded by the producer too
                if (version == 2)
                    tip.name = readUcs2Name(r);
                r.skip(9); // 1 antialias byte + 4 x i16 short bounds
                cb::TipFrame frame;
                if (readSampleBody(r, frame)) {
                    tip.frames.push_back(std::move(frame));
                    finishMaskTip(tip);
                    loaded = true;
                }
            }
            // brushType 1 is a computed brush: skipped (correctly -- see the file comment).
            if (loaded)
                out.push_back(std::move(tip));
            else
                ++dropped;
            r.seek(next);
            if (static_cast<int>(out.size()) >= cb::kMaxTipFrames)
                break;
        }
    } else if (version == 6) {
        const std::int16_t subversion = r.i16be();
        if (subversion != 1 && subversion != 2)
            return fail("unsupported ABR 6.x subversion");
        if (!reach8bimSection(r, "samp"))
            return fail("ABR 6.x has no sample section");
        const std::uint32_t sectionSize = r.u32be();
        if (!r.ok() || sectionSize > r.remaining())
            return fail("ABR sample section runs past the end of the file");
        const std::size_t sectionEnd = r.pos() + sectionSize;

        while (r.ok() && r.pos() < sectionEnd) {
            const std::uint32_t brushSize = r.u32be();
            if (!r.ok())
                break;
            std::size_t padded = brushSize;
            while (padded % 4 != 0)
                ++padded;
            const std::size_t next = r.pos() + padded;
            if (next > sectionEnd)
                break; // a lying size; nothing past it is a brush

            TipFile tip;
            r.skip(37); // the sample key
            r.skip(subversion == 1 ? 10 : 264);
            cb::TipFrame frame;
            if (readSampleBody(r, frame)) {
                tip.frames.push_back(std::move(frame));
                finishMaskTip(tip);
                out.push_back(std::move(tip));
            } else {
                ++dropped;
            }
            r.seek(next);
            if (static_cast<int>(out.size()) >= cb::kMaxTipFrames)
                break;
        }
    } else {
        return fail("unsupported ABR version");
    }

    if (droppedBrushes != nullptr)
        *droppedBrushes = dropped;
    return out;
}

} // namespace mosaic::io::brush
