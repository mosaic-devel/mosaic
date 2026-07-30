#include "formats/pnm.hpp"

#include <cstdio>
#include <cstring>

namespace mosaicfmt {
namespace {

// A token longer than this cannot be a dimension, a maxval or a tuple type; capping it is what
// keeps a hostile file from making us accumulate a string in proportion to its own length.
constexpr std::size_t kMaxToken = 32;

// Rec. 601 luma, the classic Netpbm answer for "make this grey". Integer, so it is reproducible.
[[nodiscard]] std::uint8_t luma601(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    return static_cast<std::uint8_t>((299u * r + 587u * g + 114u * b + 500u) / 1000u);
}

[[nodiscard]] bool space(std::uint8_t c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// The header scanner. Whitespace-delimited tokens, with '#' comments running to end of line --
// legal ANYWHERE whitespace is, including in the middle of the dimensions. Stopping right after
// the ONE delimiter that ended a token is what makes the binary-sample position correct: the
// format says exactly one whitespace byte separates the header from raw data.
class Tokens {
public:
    explicit Tokens(ByteReader& r) : m_r(r) {}

    [[nodiscard]] bool next(std::string& out) {
        out.clear();
        for (;;) {
            if (!m_r.has(1))
                return false;
            const std::uint8_t c = m_r.u8();
            if (space(c))
                continue;
            if (c == '#') {  // a comment: to end of line
                while (m_r.has(1)) {
                    const std::uint8_t k = m_r.u8();
                    if (k == '\n' || k == '\r')
                        break;
                }
                continue;
            }
            out.push_back(static_cast<char>(c));
            break;
        }
        while (m_r.has(1)) {
            const std::uint8_t c = m_r.u8();
            if (space(c))
                break;  // consumed, exactly as the format wants
            if (out.size() >= kMaxToken)
                return false;
            out.push_back(static_cast<char>(c));
        }
        return true;
    }

    [[nodiscard]] bool nextUint(std::uint32_t& out, std::uint32_t limit) {
        std::string token;
        if (!next(token) || token.empty())
            return false;
        std::uint64_t value = 0;
        for (const char c : token) {
            if (c < '0' || c > '9')
                return false;
            value = value * 10u + static_cast<std::uint32_t>(c - '0');
            if (value > limit)
                return false;
        }
        out = static_cast<std::uint32_t>(value);
        return true;
    }

private:
    ByteReader& m_r;
};

// An ASCII sample stream (P1/P2/P3). Same comment rules as the header.
[[nodiscard]] bool readAsciiSample(Tokens& t, std::uint32_t maxValue, std::uint32_t& out) {
    return t.nextUint(out, maxValue);
}

[[nodiscard]] std::uint8_t scaleSample(std::uint32_t v, std::uint32_t maxValue) noexcept {
    if (maxValue == 0)
        return 0;
    if (v > maxValue)
        v = maxValue;
    if (maxValue == 255)
        return static_cast<std::uint8_t>(v);
    return static_cast<std::uint8_t>(
        (static_cast<std::uint64_t>(v) * 255u + maxValue / 2u) / maxValue);
}

// A line-wrapping decimal writer for the plain (ASCII) variants. Netpbm asks that no line exceed
// 70 characters; nothing enforces it, but a single multi-megabyte line is unkind to every tool
// that might open the file in a text editor.
class AsciiRows {
public:
    explicit AsciiRows(ByteWriter& w) : m_w(w) {}
    void value(std::uint32_t v) {
        char buf[12];
        const int n = std::snprintf(buf, sizeof buf, "%u", v);
        if (n <= 0)
            return;
        if (m_column != 0 && m_column + 1 + static_cast<std::size_t>(n) > 70u) {
            m_w.u8('\n');
            m_column = 0;
        } else if (m_column != 0) {
            m_w.u8(' ');
            ++m_column;
        }
        m_w.text(std::string_view(buf, static_cast<std::size_t>(n)));
        m_column += static_cast<std::size_t>(n);
    }
    void endRow() {
        if (m_column != 0) {
            m_w.u8('\n');
            m_column = 0;
        }
    }

private:
    ByteWriter& m_w;
    std::size_t m_column = 0;
};

} // namespace

std::optional<std::vector<std::uint8_t>> encodePnm(const ImageView& image, const PnmOptions& opts,
                                                   std::string* error) {
    if (!image.valid()) {
        fail(error, "PNM: nothing to encode");
        return std::nullopt;
    }
    const bool pam = opts.variant == PnmOptions::Variant::Pam;
    const bool ascii = opts.ascii && !pam;  // PAM has no plain form

    ByteWriter w;
    if (pam) {
        const bool grey = opts.pamTuple == PnmOptions::PamTuple::Grayscale ||
                          opts.pamTuple == PnmOptions::PamTuple::GrayscaleAlpha;
        const bool alpha = opts.pamTuple == PnmOptions::PamTuple::RgbAlpha ||
                           opts.pamTuple == PnmOptions::PamTuple::GrayscaleAlpha;
        const unsigned depth = (grey ? 1u : 3u) + (alpha ? 1u : 0u);
        const char* tuple = grey ? (alpha ? "GRAYSCALE_ALPHA" : "GRAYSCALE")
                                 : (alpha ? "RGB_ALPHA" : "RGB");
        char header[160];
        const int n = std::snprintf(header, sizeof header,
                                    "P7\nWIDTH %u\nHEIGHT %u\nDEPTH %u\nMAXVAL 255\nTUPLTYPE "
                                    "%s\nENDHDR\n",
                                    image.width, image.height, depth, tuple);
        if (n <= 0) {
            fail(error, "PNM: could not format the header");
            return std::nullopt;
        }
        w.text(std::string_view(header, static_cast<std::size_t>(n)));
        for (std::uint32_t y = 0; y < image.height; ++y)
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const std::uint8_t* px = image.at(x, y);
                std::uint8_t rgb[3] = {px[0], px[1], px[2]};
                if (!alpha)
                    compositeOverMatte(px, opts.matte, rgb);
                if (grey)
                    w.u8(luma601(rgb[0], rgb[1], rgb[2]));
                else {
                    w.u8(rgb[0]);
                    w.u8(rgb[1]);
                    w.u8(rgb[2]);
                }
                if (alpha)
                    w.u8(px[3]);
            }
        return w.take();
    }

    const bool bilevel = opts.variant == PnmOptions::Variant::Pbm;
    const bool grey = opts.variant == PnmOptions::Variant::Pgm;
    const char magic = bilevel ? (ascii ? '1' : '4') : (grey ? (ascii ? '2' : '5') : (ascii ? '3' : '6'));
    char header[64];
    const int n = bilevel ? std::snprintf(header, sizeof header, "P%c\n%u %u\n", magic, image.width,
                                          image.height)
                          : std::snprintf(header, sizeof header, "P%c\n%u %u\n255\n", magic,
                                          image.width, image.height);
    if (n <= 0) {
        fail(error, "PNM: could not format the header");
        return std::nullopt;
    }
    w.text(std::string_view(header, static_cast<std::size_t>(n)));

    const int threshold = opts.bwThreshold < 0 ? 0 : (opts.bwThreshold > 255 ? 255 : opts.bwThreshold);
    AsciiRows rows(w);
    std::vector<std::uint8_t> packed;
    if (bilevel && !ascii)
        packed.assign((static_cast<std::size_t>(image.width) + 7u) / 8u, 0);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        if (bilevel && !ascii)
            std::memset(packed.data(), 0, packed.size());
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::uint8_t* px = image.at(x, y);
            std::uint8_t rgb[3];
            compositeOverMatte(px, opts.matte, rgb);  // none of these three carries alpha
            if (bilevel) {
                // PBM stores 1 = BLACK. Every other variant in the family stores 0 = black, which
                // is why this line exists on its own.
                const bool black = luma601(rgb[0], rgb[1], rgb[2]) < threshold;
                if (ascii)
                    rows.value(black ? 1u : 0u);
                else if (black)
                    packed[x >> 3] = static_cast<std::uint8_t>(packed[x >> 3] | (0x80u >> (x & 7u)));
            } else if (grey) {
                const std::uint8_t v = luma601(rgb[0], rgb[1], rgb[2]);
                if (ascii)
                    rows.value(v);
                else
                    w.u8(v);
            } else if (ascii) {
                rows.value(rgb[0]);
                rows.value(rgb[1]);
                rows.value(rgb[2]);
            } else {
                w.u8(rgb[0]);
                w.u8(rgb[1]);
                w.u8(rgb[2]);
            }
        }
        if (ascii)
            rows.endRow();
        else if (bilevel)
            w.raw(packed.data(), packed.size());
    }
    return w.take();
}

std::optional<Bitmap> decodePnm(const std::uint8_t* data, std::size_t size, std::string* error) {
    if (data == nullptr || size < 3 || size > kMaxFileBytes) {
        fail(error, "PNM: the header is truncated");
        return std::nullopt;
    }
    ByteReader r(data, size);
    Tokens tokens(r);
    std::string magic;
    if (!tokens.next(magic) || magic.size() != 2 || magic[0] != 'P' || magic[1] < '1' ||
        magic[1] > '7') {
        fail(error, "PNM: bad signature");
        return std::nullopt;
    }
    const int variant = magic[1] - '0';

    std::uint32_t width = 0, height = 0, maxValue = 1, depth = 1;
    bool alphaChannel = false, greyChannel = false;
    if (variant == 7) {
        // PAM: a keyword header, one per line, terminated by ENDHDR. Unknown keywords are skipped
        // (the format explicitly allows them); a missing required one rejects the file.
        bool haveWidth = false, haveHeight = false, haveDepth = false, endHeader = false;
        std::string keyword;
        for (int guard = 0; guard < 64 && !endHeader; ++guard) {
            if (!tokens.next(keyword)) {
                fail(error, "PAM: the header has no ENDHDR");
                return std::nullopt;
            }
            if (keyword == "ENDHDR") {
                endHeader = true;
            } else if (keyword == "WIDTH") {
                haveWidth = tokens.nextUint(width, kMaxDim);
                if (!haveWidth)
                    break;
            } else if (keyword == "HEIGHT") {
                haveHeight = tokens.nextUint(height, kMaxDim);
                if (!haveHeight)
                    break;
            } else if (keyword == "DEPTH") {
                haveDepth = tokens.nextUint(depth, 4);
                if (!haveDepth)
                    break;
            } else if (keyword == "MAXVAL") {
                if (!tokens.nextUint(maxValue, 65535))
                    break;
            } else if (keyword == "TUPLTYPE") {
                std::string tuple;
                if (!tokens.next(tuple))
                    break;  // the value itself is advisory: DEPTH is what decides the layout
            }
        }
        if (!endHeader || !haveWidth || !haveHeight || !haveDepth) {
            fail(error, "PAM: the header is incomplete");
            return std::nullopt;
        }
        if (depth == 0 || depth > 4 || maxValue == 0) {
            fail(error, "PAM: unsupported tuple depth");
            return std::nullopt;
        }
        greyChannel = depth <= 2;
        alphaChannel = depth == 2 || depth == 4;
    } else {
        if (!tokens.nextUint(width, kMaxDim) || !tokens.nextUint(height, kMaxDim)) {
            fail(error, "PNM: the header is incomplete");
            return std::nullopt;
        }
        if (variant == 1 || variant == 4) {
            maxValue = 1;
            depth = 1;
        } else {
            if (!tokens.nextUint(maxValue, 65535) || maxValue == 0) {
                fail(error, "PNM: a missing or impossible maximum value");
                return std::nullopt;
            }
            depth = (variant == 3 || variant == 6) ? 3 : 1;
        }
        greyChannel = depth == 1;
    }
    if (!dimensionsPlausible(width, height)) {
        fail(error, "PNM: implausible image dimensions");
        return std::nullopt;
    }

    Bitmap out(width, height);
    const bool bilevelPacked = variant == 4;
    const bool asciiSamples = variant == 1 || variant == 2 || variant == 3;
    const std::uint32_t sampleBytes = maxValue > 255 ? 2u : 1u;

    if (bilevelPacked) {
        const std::size_t stride = (static_cast<std::size_t>(width) + 7u) / 8u;
        if (!r.has(static_cast<std::uint64_t>(stride) * height)) {
            fail(error, "PBM: the bitmap is truncated");
            return std::nullopt;
        }
        const std::uint8_t* rows = r.cursor();
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x) {
                // 1 = black, the family's one inversion.
                const bool black =
                    ((rows[static_cast<std::size_t>(y) * stride + (x >> 3)] >> (7u - (x & 7u))) &
                     1u) != 0u;
                std::uint8_t* p = out.at(x, y);
                p[0] = p[1] = p[2] = black ? std::uint8_t{0} : std::uint8_t{255};
                p[3] = 255;
            }
        return out;
    }

    if (!asciiSamples) {
        const std::uint64_t needed = std::uint64_t{width} * height * depth * sampleBytes;
        if (!r.has(needed)) {
            fail(error, "PNM: the sample data is truncated");
            return std::nullopt;
        }
    }
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint32_t samples[4] = {0, 0, 0, 0};
            for (std::uint32_t c = 0; c < depth; ++c) {
                if (asciiSamples) {
                    if (!readAsciiSample(tokens, maxValue, samples[c])) {
                        fail(error, "PNM: the sample data ends before the image does");
                        return std::nullopt;
                    }
                    // P1's samples are bits, and 1 still means black.
                    if (variant == 1)
                        samples[c] = samples[c] != 0 ? 0u : 1u;
                } else {
                    samples[c] = sampleBytes == 2 ? r.u16be() : r.u8();
                }
            }
            std::uint8_t* p = out.at(x, y);
            if (greyChannel) {
                const std::uint8_t v = scaleSample(samples[0], maxValue);
                p[0] = p[1] = p[2] = v;
                p[3] = alphaChannel ? scaleSample(samples[1], maxValue) : std::uint8_t{255};
            } else {
                p[0] = scaleSample(samples[0], maxValue);
                p[1] = scaleSample(samples[1], maxValue);
                p[2] = scaleSample(samples[2], maxValue);
                p[3] = alphaChannel ? scaleSample(samples[3], maxValue) : std::uint8_t{255};
            }
        }
    return out;
}

} // namespace mosaicfmt
