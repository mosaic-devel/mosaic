#include "formats/qoi.hpp"

#include <cstring>

namespace mosaicfmt {
namespace {

// The six chunk tags. The two 8-bit tags (RGB, RGBA) are why a run can only reach 62: 0xFE and
// 0xFF are spoken for, so the 2-bit RUN tag's 6-bit payload loses its top two values.
constexpr std::uint8_t kOpIndex = 0x00;  // 00xxxxxx
constexpr std::uint8_t kOpDiff = 0x40;   // 01xxxxxx
constexpr std::uint8_t kOpLuma = 0x80;   // 10xxxxxx
constexpr std::uint8_t kOpRun = 0xC0;    // 11xxxxxx
constexpr std::uint8_t kOpRgb = 0xFE;
constexpr std::uint8_t kOpRgba = 0xFF;
constexpr std::uint8_t kMask2 = 0xC0;
constexpr int kMaxRun = 62;

// The spec's hash. The odd multipliers are what keep the 64-entry table from degenerating on
// gradients; they are part of the format, not a tuning choice.
[[nodiscard]] std::size_t hash(Rgba8 p) noexcept {
    return static_cast<std::size_t>(p.r * 3 + p.g * 5 + p.b * 7 + p.a * 11) % 64u;
}

// The spec computes its deltas in wrapping 8-bit signed arithmetic, and that matters: a step from
// 250 to 2 is +8, not -248, and the reference encoder codes it as a DIFF. C++20 pins the
// unsigned -> signed conversion to two's complement, so this is exactly the reference behaviour.
[[nodiscard]] std::int8_t delta8(std::uint8_t a, std::uint8_t b) noexcept {
    return static_cast<std::int8_t>(static_cast<std::uint8_t>(a - b));
}

[[nodiscard]] std::uint8_t add8(std::uint8_t v, int d) noexcept {
    return static_cast<std::uint8_t>(static_cast<unsigned>(v) + static_cast<unsigned>(d));
}

} // namespace

std::optional<std::vector<std::uint8_t>> encodeQoi(const ImageView& image, const QoiOptions& opts,
                                                   std::string* error) {
    if (!image.valid()) {
        fail(error, "QOI: nothing to encode");
        return std::nullopt;
    }
    const bool keepAlpha = opts.channels != 3;

    ByteWriter w;
    w.text("qoif");
    w.u32be(image.width);
    w.u32be(image.height);
    w.u8(keepAlpha ? std::uint8_t{4} : std::uint8_t{3});
    w.u8(opts.linearColorspace ? std::uint8_t{1} : std::uint8_t{0});

    // The spec's initial state, and it is load-bearing in both directions: every index slot is
    // {0,0,0,0} -- NOT Rgba8's own default, which is opaque -- while the running pixel starts as
    // opaque black. Getting either wrong produces a file that only our own decoder can read.
    Rgba8 index[64];
    for (Rgba8& slot : index)
        slot = Rgba8{0, 0, 0, 0};
    Rgba8 prev{0, 0, 0, 255};
    int run = 0;
    const std::size_t total = static_cast<std::size_t>(image.width) * image.height;
    for (std::size_t i = 0; i < total; ++i) {
        const std::uint8_t* src = image.rgba + i * 4;
        Rgba8 px{src[0], src[1], src[2], src[3]};
        if (!keepAlpha) {
            std::uint8_t rgb[3];
            compositeOverMatte(src, opts.matte, rgb);
            px = Rgba8{rgb[0], rgb[1], rgb[2], 255};
        }

        if (px == prev) {
            ++run;
            if (run == kMaxRun || i + 1 == total) {
                w.u8(static_cast<std::uint8_t>(kOpRun | (run - 1)));
                run = 0;
            }
        } else {
            if (run > 0) {
                w.u8(static_cast<std::uint8_t>(kOpRun | (run - 1)));
                run = 0;
            }
            const std::size_t slot = hash(px);
            if (index[slot] == px) {
                w.u8(static_cast<std::uint8_t>(kOpIndex | slot));
            } else {
                index[slot] = px;
                if (px.a == prev.a) {
                    const std::int8_t vr = delta8(px.r, prev.r);
                    const std::int8_t vg = delta8(px.g, prev.g);
                    const std::int8_t vb = delta8(px.b, prev.b);
                    const int dgr = vr - vg;
                    const int dgb = vb - vg;
                    if (vr > -3 && vr < 2 && vg > -3 && vg < 2 && vb > -3 && vb < 2) {
                        w.u8(static_cast<std::uint8_t>(kOpDiff | ((vr + 2) << 4) |
                                                       ((vg + 2) << 2) | (vb + 2)));
                    } else if (dgr > -9 && dgr < 8 && vg > -33 && vg < 32 && dgb > -9 && dgb < 8) {
                        w.u8(static_cast<std::uint8_t>(kOpLuma | (vg + 32)));
                        w.u8(static_cast<std::uint8_t>(((dgr + 8) << 4) | (dgb + 8)));
                    } else {
                        w.u8(kOpRgb);
                        w.u8(px.r);
                        w.u8(px.g);
                        w.u8(px.b);
                    }
                } else {
                    w.u8(kOpRgba);
                    w.u8(px.r);
                    w.u8(px.g);
                    w.u8(px.b);
                    w.u8(px.a);
                }
            }
        }
        prev = px;
    }
    // The mandatory 8-byte end marker: seven zeros and a one.
    w.zeros(7);
    w.u8(0x01);
    return w.take();
}

std::optional<Bitmap> decodeQoi(const std::uint8_t* data, std::size_t size, std::string* error) {
    if (data == nullptr || size < 14 + 8 || size > kMaxFileBytes) {
        fail(error, "QOI: not a QOI file (too short)");
        return std::nullopt;
    }
    ByteReader r(data, size);
    if (std::memcmp(data, "qoif", 4) != 0) {
        fail(error, "QOI: bad signature");
        return std::nullopt;
    }
    r.skip(4);
    const std::uint32_t width = r.u32be();
    const std::uint32_t height = r.u32be();
    const std::uint8_t channels = r.u8();
    const std::uint8_t colorspace = r.u8();
    // These four ARE the whole header, so anything wrong in them is a structural lie about the
    // file rather than a decorative field to drop.
    if (channels != 3 && channels != 4) {
        fail(error, "QOI: the header declares neither 3 nor 4 channels");
        return std::nullopt;
    }
    if (colorspace > 1) {
        fail(error, "QOI: unknown colorspace tag");
        return std::nullopt;
    }
    if (!dimensionsPlausible(width, height)) {
        fail(error, "QOI: implausible image dimensions");
        return std::nullopt;
    }

    Bitmap out(width, height);
    Rgba8 index[64];  // {0,0,0,0}, not Rgba8's opaque default -- see the encoder's note
    for (Rgba8& slot : index)
        slot = Rgba8{0, 0, 0, 0};
    Rgba8 px{0, 0, 0, 255};
    const std::size_t total = static_cast<std::size_t>(width) * height;
    int run = 0;
    for (std::size_t i = 0; i < total; ++i) {
        if (run > 0) {
            --run;
        } else {
            if (!r.has(1)) {
                fail(error, "QOI: the chunk stream ends before the image does");
                return std::nullopt;
            }
            const std::uint8_t tag = r.u8();
            if (tag == kOpRgb) {
                if (!r.has(3)) {
                    fail(error, "QOI: truncated RGB chunk");
                    return std::nullopt;
                }
                px.r = r.u8();
                px.g = r.u8();
                px.b = r.u8();
            } else if (tag == kOpRgba) {
                if (!r.has(4)) {
                    fail(error, "QOI: truncated RGBA chunk");
                    return std::nullopt;
                }
                px.r = r.u8();
                px.g = r.u8();
                px.b = r.u8();
                px.a = r.u8();
            } else if ((tag & kMask2) == kOpIndex) {
                px = index[tag & 0x3Fu];
            } else if ((tag & kMask2) == kOpDiff) {
                px.r = add8(px.r, static_cast<int>((tag >> 4) & 0x03u) - 2);
                px.g = add8(px.g, static_cast<int>((tag >> 2) & 0x03u) - 2);
                px.b = add8(px.b, static_cast<int>(tag & 0x03u) - 2);
            } else if ((tag & kMask2) == kOpLuma) {
                if (!r.has(1)) {
                    fail(error, "QOI: truncated LUMA chunk");
                    return std::nullopt;
                }
                const std::uint8_t second = r.u8();
                const int dg = static_cast<int>(tag & 0x3Fu) - 32;
                px.r = add8(px.r, dg - 8 + static_cast<int>((second >> 4) & 0x0Fu));
                px.g = add8(px.g, dg);
                px.b = add8(px.b, dg - 8 + static_cast<int>(second & 0x0Fu));
            } else {  // kOpRun
                run = static_cast<int>(tag & 0x3Fu);  // this pixel plus `run` more
            }
            index[hash(px)] = px;
        }
        std::uint8_t* d = out.rgba.data() + i * 4;
        d[0] = px.r;
        d[1] = px.g;
        d[2] = px.b;
        d[3] = channels == 3 ? std::uint8_t{255} : px.a;
    }

    // The end marker is mandatory in the spec, and checking it is the only integrity signal the
    // format offers: a stream that decoded the right number of pixels but does not end where it
    // says it does has been cut or spliced.
    static constexpr std::uint8_t kEnd[8] = {0, 0, 0, 0, 0, 0, 0, 1};
    if (!r.has(8) || std::memcmp(r.cursor(), kEnd, 8) != 0) {
        fail(error, "QOI: the end marker is missing");
        return std::nullopt;
    }
    return out;
}

} // namespace mosaicfmt
