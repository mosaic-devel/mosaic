#include "formats/hdr.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mosaicfmt {
namespace {

// A header line longer than this, or more lines than this, is not a Radiance header.
constexpr std::size_t kMaxHeaderLine = 512;
constexpr int kMaxHeaderLines = 128;

// The adaptive RLE scanline format only applies in this width range; outside it, scanlines are
// flat. (Below 8, the 4-byte scanline marker would be indistinguishable from pixel data.)
constexpr std::uint32_t kRleMinWidth = 8;
constexpr std::uint32_t kRleMaxWidth = 0x7FFF;

[[nodiscard]] float srgbToLinear(std::uint8_t v) noexcept {
    // A 256-entry table: the transfer function is called once per channel per pixel, and pow() is
    // by far the most expensive thing in this encoder otherwise.
    static const std::vector<float> table = [] {
        std::vector<float> t(256);
        for (int i = 0; i < 256; ++i) {
            const float s = static_cast<float>(i) / 255.0f;
            t[static_cast<std::size_t>(i)] =
                s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return table[v];
}

[[nodiscard]] std::uint8_t linearToSrgb8(float linear) noexcept {
    if (!(linear > 0.0f))  // NaN-proof spelling
        return 0;
    const float l = linear > 1.0f ? 1.0f : linear;  // the documented exposure-1 clamp
    const float s = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
    const float scaled = s * 255.0f + 0.5f;
    return static_cast<std::uint8_t>(scaled > 255.0f ? 255.0f : scaled);
}

// Linear RGB -> RGBE, Ward's original arithmetic: one shared exponent taken from the largest
// channel, each channel a mantissa against it. The consequence to remember (and it is why the
// tests use a tolerance rather than equality) is that a bright channel beside a dark one costs the
// dark one precision -- that is the format, not the implementation.
void toRgbe(float r, float g, float b, std::uint8_t out[4]) noexcept {
    const float largest = std::max(std::max(r, g), b);
    if (!(largest >= 1e-32f)) {
        out[0] = out[1] = out[2] = out[3] = 0;  // the format's spelling of "black"
        return;
    }
    int exponent = 0;
    const float scale = std::frexp(largest, &exponent) * 256.0f / largest;
    const auto mantissa = [scale](float v) {
        const float m = v * scale;
        return static_cast<std::uint8_t>(m <= 0.0f ? 0.0f : (m > 255.0f ? 255.0f : m));
    };
    out[0] = mantissa(r);
    out[1] = mantissa(g);
    out[2] = mantissa(b);
    out[3] = static_cast<std::uint8_t>(std::clamp(exponent + 128, 0, 255));
}

void fromRgbe(const std::uint8_t rgbe[4], float& r, float& g, float& b) noexcept {
    if (rgbe[3] == 0) {
        r = g = b = 0.0f;
        return;
    }
    const float scale = std::ldexp(1.0f, static_cast<int>(rgbe[3]) - (128 + 8));
    r = rgbe[0] * scale;
    g = rgbe[1] * scale;
    b = rgbe[2] * scale;
}

// One component stream of an adaptive-RLE scanline. Runs of four or more pay for themselves; a run
// packet's count rides in the byte's high bit, so a run is at most 127 and a dump at most 128.
void encodeComponent(ByteWriter& w, const std::uint8_t* comp, std::uint32_t width) {
    std::uint32_t x = 0;
    while (x < width) {
        std::uint32_t run = 1;
        while (x + run < width && run < 127 && comp[x + run] == comp[x])
            ++run;
        if (run >= 4) {
            w.u8(static_cast<std::uint8_t>(128u + run));
            w.u8(comp[x]);
            x += run;
            continue;
        }
        std::uint32_t n = 0;
        while (x + n < width && n < 128) {
            if (x + n + 3 < width && comp[x + n] == comp[x + n + 1] &&
                comp[x + n] == comp[x + n + 2] && comp[x + n] == comp[x + n + 3])
                break;  // a run of four starts here; let the run packet have it
            ++n;
        }
        if (n == 0)
            n = 1;
        w.u8(static_cast<std::uint8_t>(n));
        for (std::uint32_t k = 0; k < n; ++k)
            w.u8(comp[x + k]);
        x += n;
    }
}

} // namespace

std::optional<std::vector<std::uint8_t>> encodeHdr(const ImageView& image, const HdrOptions& opts,
                                                   std::string* error) {
    if (!image.valid()) {
        fail(error, "Radiance HDR: nothing to encode");
        return std::nullopt;
    }
    ByteWriter w;
    w.text("#?RADIANCE\n");
    // The identifier is "32-bit_rle_rgbe" for RGBE data whether or not the scanlines are actually
    // run-length coded -- the "rle" in the name is historical, and a reader keys on the pixel
    // format, not on the coding.
    w.text("FORMAT=32-bit_rle_rgbe\n");
    w.text("\n");  // the blank line that ends the header
    char resolution[64];
    const int n = std::snprintf(resolution, sizeof resolution, "-Y %u +X %u\n", image.height,
                                image.width);
    if (n <= 0) {
        fail(error, "Radiance HDR: could not format the resolution line");
        return std::nullopt;
    }
    w.text(std::string_view(resolution, static_cast<std::size_t>(n)));

    const bool rle =
        opts.rle && image.width >= kRleMinWidth && image.width <= kRleMaxWidth;
    // Four component planes for one scanline; the adaptive coding is per component, which is
    // exactly why it beats coding whole pixels.
    std::vector<std::uint8_t> planes(static_cast<std::size_t>(image.width) * 4u);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::uint8_t* px = image.at(x, y);
            std::uint8_t rgb[3];
            compositeOverMatte(px, opts.matte, rgb);  // the format has no alpha
            std::uint8_t rgbe[4];
            toRgbe(srgbToLinear(rgb[0]), srgbToLinear(rgb[1]), srgbToLinear(rgb[2]), rgbe);
            if (rle)
                for (std::uint32_t c = 0; c < 4; ++c)
                    planes[static_cast<std::size_t>(c) * image.width + x] = rgbe[c];
            else
                w.raw(rgbe, 4);
        }
        if (!rle)
            continue;
        w.u8(2);
        w.u8(2);
        w.u8(static_cast<std::uint8_t>((image.width >> 8) & 0x7Fu));
        w.u8(static_cast<std::uint8_t>(image.width & 0xFFu));
        for (std::uint32_t c = 0; c < 4; ++c)
            encodeComponent(w, planes.data() + static_cast<std::size_t>(c) * image.width,
                            image.width);
    }
    return w.take();
}

std::optional<Bitmap> decodeHdr(const std::uint8_t* data, std::size_t size, std::string* error) {
    if (data == nullptr || size < 16 || size > kMaxFileBytes) {
        fail(error, "Radiance HDR: the header is truncated");
        return std::nullopt;
    }
    ByteReader r(data, size);
    // A line reader with both caps applied, so a file with no newline at all cannot make us
    // accumulate its whole length into a string.
    std::string line;
    const auto readLine = [&]() {
        line.clear();
        while (r.has(1)) {
            const std::uint8_t c = r.u8();
            if (c == '\n')
                return true;
            if (line.size() < kMaxHeaderLine)
                line.push_back(static_cast<char>(c));
        }
        return false;
    };

    if (!readLine() || (line.rfind("#?", 0) != 0)) {
        fail(error, "Radiance HDR: bad signature");
        return std::nullopt;
    }
    bool sawFormat = false;
    double exposure = 1.0;
    for (int i = 0; i < kMaxHeaderLines; ++i) {
        if (!readLine()) {
            fail(error, "Radiance HDR: the header never ends");
            return std::nullopt;
        }
        if (line.empty())
            break;  // the blank line: the resolution line is next
        if (line.rfind("FORMAT=", 0) == 0) {
            if (line == "FORMAT=32-bit_rle_rgbe")
                sawFormat = true;
            else {
                // 32-bit_rle_xyze stores CIE XYZ with a shared exponent. Converting it needs the
                // file's primaries and a matrix; refusing is honest, guessing would not be.
                fail(error, "Radiance HDR: only 32-bit_rle_rgbe files are supported");
                return std::nullopt;
            }
        } else if (line.rfind("EXPOSURE=", 0) == 0) {
            // The stored values were MULTIPLIED by this, so they are divided back out. An absurd
            // value drops the field rather than the file (the house rule) and leaves exposure 1.
            const double v = std::atof(line.c_str() + 9);
            if (v > 1e-6 && v < 1e6)
                exposure *= v;
        }
    }
    if (!sawFormat) {
        fail(error, "Radiance HDR: the header declares no pixel format");
        return std::nullopt;
    }
    if (!readLine()) {
        fail(error, "Radiance HDR: the resolution line is missing");
        return std::nullopt;
    }
    // "-Y h +X w" is the standard orientation (rows top to bottom); "+Y" flips it. The transposed
    // spellings ("+X w -Y h") are legal in the format and vanishingly rare; refusing them keeps
    // the row walk honest instead of silently transposing somebody's picture.
    unsigned height = 0, width = 0;
    char ySign = 0;
    bool flipped = false;
    if (std::sscanf(line.c_str(), "-Y %u +X %u", &height, &width) == 2) {
        ySign = '-';
    } else if (std::sscanf(line.c_str(), "+Y %u +X %u", &height, &width) == 2) {
        ySign = '+';
        flipped = true;
    }
    if (ySign == 0 || !dimensionsPlausible(width, height)) {
        fail(error, "Radiance HDR: unsupported or implausible resolution line");
        return std::nullopt;
    }

    Bitmap out(width, height);
    std::vector<std::uint8_t> scanline(static_cast<std::size_t>(width) * 4u);
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint32_t y = flipped ? height - 1 - row : row;
        bool adaptive = false;
        if (width >= kRleMinWidth && width <= kRleMaxWidth && r.has(4)) {
            const std::uint8_t* p = r.cursor();
            adaptive = p[0] == 2 && p[1] == 2 &&
                       ((static_cast<std::uint32_t>(p[2]) << 8) | p[3]) == width;
        }
        if (adaptive) {
            r.skip(4);
            for (std::uint32_t c = 0; c < 4; ++c) {
                std::uint32_t x = 0;
                while (x < width) {
                    if (!r.has(1)) {
                        fail(error, "Radiance HDR: a run-length scanline is truncated");
                        return std::nullopt;
                    }
                    const std::uint32_t count = r.u8();
                    if (count > 128) {  // a run
                        const std::uint32_t n = count - 128u;
                        if (n == 0 || x + n > width || !r.has(1)) {
                            fail(error, "Radiance HDR: a run overruns its scanline");
                            return std::nullopt;
                        }
                        const std::uint8_t v = r.u8();
                        for (std::uint32_t k = 0; k < n; ++k)
                            scanline[static_cast<std::size_t>(x + k) * 4u + c] = v;
                        x += n;
                    } else {  // a dump
                        if (count == 0 || x + count > width || !r.has(count)) {
                            fail(error, "Radiance HDR: a literal block overruns its scanline");
                            return std::nullopt;
                        }
                        for (std::uint32_t k = 0; k < count; ++k)
                            scanline[static_cast<std::size_t>(x + k) * 4u + c] = r.u8();
                        x += count;
                    }
                }
            }
        } else {
            // Flat scanline, with the OLD run-length spelling folded in: a pixel of (1,1,1,n)
            // repeats the previous pixel n times, and consecutive such records shift n by 8 bits
            // each (which is how the old format counted past 255).
            std::uint32_t x = 0;
            int shift = 0;
            while (x < width) {
                if (!r.has(4)) {
                    fail(error, "Radiance HDR: the pixel data is truncated");
                    return std::nullopt;
                }
                const std::uint8_t p0 = r.u8();
                const std::uint8_t p1 = r.u8();
                const std::uint8_t p2 = r.u8();
                const std::uint8_t p3 = r.u8();
                if (p0 == 1 && p1 == 1 && p2 == 1) {
                    if (x == 0) {
                        fail(error, "Radiance HDR: a run with nothing to repeat");
                        return std::nullopt;
                    }
                    const std::uint32_t n = static_cast<std::uint32_t>(p3) << shift;
                    if (n == 0 || x + n > width) {
                        fail(error, "Radiance HDR: a run overruns its scanline");
                        return std::nullopt;
                    }
                    const std::size_t src = (static_cast<std::size_t>(x) - 1u) * 4u;
                    for (std::uint32_t k = 0; k < n; ++k)
                        std::memcpy(scanline.data() + (static_cast<std::size_t>(x + k) * 4u),
                                    scanline.data() + src, 4);
                    x += n;
                    shift += 8;
                    if (shift > 24)
                        shift = 24;
                } else {
                    std::uint8_t* dst = scanline.data() + static_cast<std::size_t>(x) * 4u;
                    dst[0] = p0;
                    dst[1] = p1;
                    dst[2] = p2;
                    dst[3] = p3;
                    ++x;
                    shift = 0;
                }
            }
        }
        const float inverseExposure = static_cast<float>(1.0 / exposure);
        for (std::uint32_t x = 0; x < width; ++x) {
            float rr = 0.0f, gg = 0.0f, bb = 0.0f;
            fromRgbe(scanline.data() + static_cast<std::size_t>(x) * 4u, rr, gg, bb);
            std::uint8_t* p = out.at(x, y);
            p[0] = linearToSrgb8(rr * inverseExposure);
            p[1] = linearToSrgb8(gg * inverseExposure);
            p[2] = linearToSrgb8(bb * inverseExposure);
            p[3] = 255;  // the format carries no alpha
        }
    }
    return out;
}

} // namespace mosaicfmt
