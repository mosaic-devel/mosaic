#include "common/fs_path.hpp"
#include "io/detail.hpp"
#include "io/io.hpp"

#include <turbojpeg.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace mosaic::io::detail {

std::optional<common::Image> decodeJpeg(const std::vector<std::uint8_t>& buf,
                                        std::string* error) {
    tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
    if (tj == nullptr) {
        if (error)
            *error = "JPEG: decompressor init failed";
        return std::nullopt;
    }
    const auto fail = [&](const char* what) -> std::optional<common::Image> {
        if (error)
            *error = std::string("JPEG: ") + what;
        tj3Destroy(tj);
        return std::nullopt;
    };
    if (tj3DecompressHeader(tj, buf.data(), buf.size()) != 0)
        return fail(tj3GetErrorStr(tj));
    const int w = tj3Get(tj, TJPARAM_JPEGWIDTH);
    const int h = tj3Get(tj, TJPARAM_JPEGHEIGHT);
    if (w <= 0 || h <= 0 || static_cast<std::uint32_t>(w) > kMaxDim ||
        static_cast<std::uint32_t>(h) > kMaxDim)
        return fail("unsupported image dimensions");
    common::Image out(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    // TJPF_RGBA writes opaque alpha (255) into the 4th byte of every pixel for us.
    if (tj3Decompress8(tj, buf.data(), buf.size(), out.rgba.data(), 0, TJPF_RGBA) != 0)
        return fail(tj3GetErrorStr(tj));
    tj3Destroy(tj);
    return out;
}

} // namespace mosaic::io::detail

namespace mosaic::io {

namespace {

// Flatten the straight-alpha RGBA source onto the opaque matte into a tightly-packed RGB buffer
// (JPEG has no alpha): out = src*a + matte*(1-a). Opaque pixels (a=255) pass through unchanged, so
// an opaque flatten -- the common export case -- is matte-independent.
[[nodiscard]] std::vector<std::uint8_t> flattenToRgb(const common::Image& img, common::Color8 matte) {
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(img.width) * img.height * 3);
    const std::uint8_t* src = img.rgba.data();
    std::uint8_t* dst = rgb.data();
    const std::size_t n = img.pixelCount();
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t a = src[i * 4 + 3];
        const std::uint32_t ia = 255u - a;
        // out = (src*a + matte*(1-a)) with a in [0,1]; here a,ia are 0..255, so divide by 255,
        // rounding to nearest. Exact integer math -- this is the export path, not a per-frame loop.
        const auto over = [&](std::uint32_t s, std::uint32_t m) -> std::uint8_t {
            return static_cast<std::uint8_t>((s * a + m * ia + 127u) / 255u);
        };
        dst[i * 3 + 0] = over(src[i * 4 + 0], matte.r);
        dst[i * 3 + 1] = over(src[i * 4 + 1], matte.g);
        dst[i * 3 + 2] = over(src[i * 4 + 2], matte.b);
    }
    return rgb;
}

[[nodiscard]] int subsampToTj(JpegSaveOptions::Subsampling s) noexcept {
    switch (s) {
    case JpegSaveOptions::Subsampling::S444:
        return TJSAMP_444;
    case JpegSaveOptions::Subsampling::S422:
        return TJSAMP_422;
    case JpegSaveOptions::Subsampling::S420:
        break;
    }
    return TJSAMP_420;
}

// ---- the metadata marker segments (M5) -------------------------------------------------------
//
// JPEG's metadata lives in APPn marker segments, and this module SPLICES them into the finished
// bitstream rather than asking the encoder to write them. That is not a workaround: the app talks
// to libjpeg-turbo through its tj3 API, which has no marker-writing entry point at all
// (jpeg_write_marker belongs to the lower-level libjpeg API this translation unit deliberately
// does not link), while a marker segment is position-independent by construction -- inserting one
// is exactly what every metadata tool does to a JPEG that already exists.
//
// The bit that is genuinely easy to get wrong, and the reason this is more than four lines: an ICC
// profile ROUTINELY EXCEEDS ONE SEGMENT. A segment's length is a 16-bit word covering itself, so
// the payload ceiling is 65533 bytes; a display profile is a few kilobytes but a press profile is
// megabytes. The specification's answer is a numbered sequence of APP2 segments, each repeating
// the "ICC_PROFILE\0" identifier and carrying a **1-based** chunk number and the total count, and
// a reader concatenates them in order. Writing one oversized segment instead is the classic bug --
// it does not fail loudly, it just produces a file whose profile no colour-managed reader can use.

constexpr std::size_t kMaxSegmentPayload = 65533;  // 65535 minus the length word's own two bytes
// "ICC_PROFILE\0" -- 12 bytes, the trailing NUL included, exactly as the spec writes it. Spelled
// as bytes rather than a string literal so nothing converts on the way into the payload.
constexpr std::uint8_t kIccIdentifier[] = {'I', 'C', 'C', '_', 'P', 'R',
                                           'O', 'F', 'I', 'L', 'E', 0};
constexpr std::size_t kIccPrefix = sizeof kIccIdentifier + 2;  // + the chunk and count bytes
constexpr std::uint8_t kMarkerApp0 = 0xE0;  // JFIF
constexpr std::uint8_t kMarkerApp1 = 0xE1;  // Exif
constexpr std::uint8_t kMarkerApp2 = 0xE2;  // ICC_PROFILE

void appendSegment(std::vector<std::uint8_t>& out, std::uint8_t marker,
                   const std::vector<std::uint8_t>& payload) {
    const std::size_t length = payload.size() + 2;  // the word counts itself
    out.push_back(0xFF);
    out.push_back(marker);
    out.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(length & 0xFFu));
    out.insert(out.end(), payload.begin(), payload.end());
}

// Where a metadata segment may be inserted: immediately after the SOI, past a leading JFIF APP0
// if the encoder wrote one (libjpeg does). Both segments have to precede the first DQT/SOF, which
// is where our own reader -- and libjpeg's marker scan, and every EXIF reader in the world --
// stops looking. Returns 0 for anything that is not the JPEG we just wrote, which the caller reads
// as "splice nothing": a picture without its metadata beats a file with a segment in the wrong
// place.
[[nodiscard]] std::size_t metadataInsertionPoint(const std::vector<std::uint8_t>& jpeg) {
    if (jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8)
        return 0;
    std::size_t at = 2;
    while (at + 4 <= jpeg.size() && jpeg[at] == 0xFF && jpeg[at + 1] == kMarkerApp0) {
        const std::size_t length =
            (static_cast<std::size_t>(jpeg[at + 2]) << 8) | static_cast<std::size_t>(jpeg[at + 3]);
        if (length < 2 || at + 2 + length > jpeg.size())
            break;  // malformed framing: stop here rather than walking off the end
        at += 2 + length;
    }
    return at;
}

void spliceMetadata(std::vector<std::uint8_t>& jpeg, const EmbeddedMetadata& metadata) {
    if (metadata.exif.empty() && metadata.icc.empty())
        return;
    const std::size_t at = metadataInsertionPoint(jpeg);
    if (at == 0)
        return;

    std::vector<std::uint8_t> segments;
    // APP1: the "Exif\0\0" prefix -- which is a JPEG convention, not part of the payload, which is
    // why io::buildExifPayload does not produce it -- followed by the raw TIFF payload. EXIF has no
    // multi-segment form, so a payload that will not fit is simply not written.
    if (!metadata.exif.empty() && metadata.exif.size() + 6 <= kMaxSegmentPayload) {
        std::vector<std::uint8_t> payload{'E', 'x', 'i', 'f', 0, 0};
        payload.insert(payload.end(), metadata.exif.begin(), metadata.exif.end());
        appendSegment(segments, kMarkerApp1, payload);
    }
    // APP2: the profile, chunked. The count byte is 8 bits, so 255 segments is the hard ceiling --
    // ~16.7 MB of profile, above io::readIccProfile's own limit. A profile past it is dropped
    // rather than truncated: half a profile is worse than none.
    if (!metadata.icc.empty()) {
        constexpr std::size_t kSlice = kMaxSegmentPayload - kIccPrefix;
        const std::size_t count = (metadata.icc.size() + kSlice - 1) / kSlice;
        if (count >= 1 && count <= 255) {
            for (std::size_t i = 0; i < count; ++i) {
                const std::size_t offset = i * kSlice;
                const std::size_t n = std::min(kSlice, metadata.icc.size() - offset);
                std::vector<std::uint8_t> payload;
                payload.reserve(kIccPrefix + n);
                payload.insert(payload.end(), kIccIdentifier,
                               kIccIdentifier + sizeof kIccIdentifier);
                payload.push_back(static_cast<std::uint8_t>(i + 1));  // 1-based, per the spec
                payload.push_back(static_cast<std::uint8_t>(count));
                payload.insert(payload.end(), metadata.icc.begin() + static_cast<std::ptrdiff_t>(offset),
                               metadata.icc.begin() + static_cast<std::ptrdiff_t>(offset + n));
                appendSegment(segments, kMarkerApp2, payload);
            }
        }
    }
    if (segments.empty())
        return;
    jpeg.insert(jpeg.begin() + static_cast<std::ptrdiff_t>(at), segments.begin(), segments.end());
}

} // namespace

std::optional<std::vector<std::uint8_t>> encodeJpeg(const common::Image& image,
                                                    const JpegSaveOptions& opts, std::string* error) {
    const auto fail = [&](const char* what) -> std::optional<std::vector<std::uint8_t>> {
        if (error)
            *error = std::string("JPEG: ") + what;
        return std::nullopt;
    };
    if (image.empty())
        return fail("cannot write an empty image");
    if (image.rgba.size() < image.pixelCount() * 4)
        return fail("image buffer is smaller than its dimensions");
    if (static_cast<std::uint64_t>(image.width) > 65535 ||
        static_cast<std::uint64_t>(image.height) > 65535)
        return fail("image is too large for the JPEG format (65535 px maximum per side)");

    tjhandle tj = tj3Init(TJINIT_COMPRESS);
    if (tj == nullptr)
        return fail("compressor init failed");
    const auto failTj = [&](const char* what) -> std::optional<std::vector<std::uint8_t>> {
        if (error)
            *error = std::string("JPEG: ") + what;
        tj3Destroy(tj);
        return std::nullopt;
    };

    tj3Set(tj, TJPARAM_QUALITY, std::clamp(opts.quality, 0, 100));
    tj3Set(tj, TJPARAM_SUBSAMP, subsampToTj(opts.subsampling));
    tj3Set(tj, TJPARAM_PROGRESSIVE, opts.progressive ? 1 : 0);
    // Physical density, into the JFIF APP0 header (units 1 = pixels per inch). 72 is the value
    // that means "no opinion", so it is skipped: writing it would make every plain export claim a
    // print size, exactly as with PNG's pHYs.
    if (opts.metadata.dpi > 0.0 && std::abs(opts.metadata.dpi - 72.0) > 1e-9) {
        const int density = static_cast<int>(std::clamp(opts.metadata.dpi + 0.5, 1.0, 65535.0));
        tj3Set(tj, TJPARAM_DENSITYUNITS, 1);
        tj3Set(tj, TJPARAM_XDENSITY, density);
        tj3Set(tj, TJPARAM_YDENSITY, density);
    }

    const std::vector<std::uint8_t> rgb = flattenToRgb(image, opts.matte);
    unsigned char* jpegBuf = nullptr; // tj3 allocates this; freed with tj3Free below
    std::size_t jpegSize = 0;
    if (tj3Compress8(tj, rgb.data(), static_cast<int>(image.width), /*pitch=*/0,
                     static_cast<int>(image.height), TJPF_RGB, &jpegBuf, &jpegSize) != 0) {
        std::string msg = tj3GetErrorStr(tj);
        tj3Free(jpegBuf); // safe on nullptr; the compress may have allocated before failing
        return failTj(msg.c_str());
    }

    std::vector<std::uint8_t> out(jpegBuf, jpegBuf + jpegSize);
    tj3Free(jpegBuf);
    tj3Destroy(tj);
    spliceMetadata(out, opts.metadata);  // APP1 Exif + APP2 ICC_PROFILE; a no-op with neither
    return out;
}

bool saveJpeg(const common::Image& image, const std::string& path, const JpegSaveOptions& opts,
              std::string* error) {
    // One encode path: build the JPEG in memory (encodeJpeg already set *error on failure), then
    // commit it to disk -- mirroring savePng's FILE handling and its two error strings.
    std::optional<std::vector<std::uint8_t>> bytes = encodeJpeg(image, opts, error);
    if (!bytes)
        return false;

    const auto fail = [&](const char* what) {
        if (error)
            *error = std::string("JPEG: ") + what;
        return false;
    };
    std::FILE* fp = common::fopenUtf8(path, "wb");
    if (fp == nullptr)
        return fail("could not open the file for writing");
    if (!bytes->empty() && std::fwrite(bytes->data(), 1, bytes->size(), fp) != bytes->size()) {
        std::fclose(fp);
        return fail("could not write the file");
    }
    if (std::fclose(fp) != 0)
        return fail("could not flush the file to disk");
    return true;
}

} // namespace mosaic::io
