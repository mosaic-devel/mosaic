#include "common/fs_path.hpp"
#include "io/detail.hpp"
#include "io/io.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

// webp -- the WebP codec (M4), wrapping the system libwebp + libwebpmux.
//
// Compile-time gated on MOSAIC_HAVE_WEBP exactly like jxl.cpp: the symbols always exist, so
// callers link either way, and webpSupported() is the runtime probe the format combobox reads.
//
// Two libraries, two jobs: libwebp produces the bare VP8/VP8L bitstream, and libwebpmux wraps it
// in the extended RIFF container that can hold the ICCP and EXIF chunks. We only pay for the mux
// when there IS metadata -- a plain export stays a simple-format WebP, which is what every
// decoder in the world reads fastest.
#ifdef MOSAIC_HAVE_WEBP
#include <webp/decode.h>
#include <webp/encode.h>
#include <webp/mux.h>
#endif

namespace mosaic::io {

#ifdef MOSAIC_HAVE_WEBP

namespace detail {

std::optional<common::Image> decodeWebp(const std::vector<std::uint8_t>& buf, std::string* error) {
    const auto fail = [&](const char* what) -> std::optional<common::Image> {
        if (error)
            *error = std::string("WebP: ") + what;
        return std::nullopt;
    };
    int width = 0;
    int height = 0;
    if (buf.empty() || WebPGetInfo(buf.data(), buf.size(), &width, &height) == 0)
        return fail("not a readable WebP file");
    if (width <= 0 || height <= 0 ||
        !dimensionsPlausible(static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height)))
        return fail("unsupported image dimensions");

    common::Image out(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    // MODE_RGBA (which DecodeRGBAInto selects) is UNassociated alpha, matching our pipeline; the
    // premultiplied spelling is the lower-case `rgbA` one, which we deliberately do not use.
    if (WebPDecodeRGBAInto(buf.data(), buf.size(), out.rgba.data(), out.rgba.size(),
                           width * 4) == nullptr)
        return fail("the bitstream could not be decoded");
    return out;
}

bool probeWebpDimensions(const std::vector<std::uint8_t>& head, std::uint32_t& width,
                         std::uint32_t& height) {
    int w = 0;
    int h = 0;
    if (head.empty() || WebPGetInfo(head.data(), head.size(), &w, &h) == 0)
        return false;
    if (w <= 0 || h <= 0)
        return false;
    width = static_cast<std::uint32_t>(w);
    height = static_cast<std::uint32_t>(h);
    return true;
}

}  // namespace detail

bool webpSupported() noexcept { return true; }

namespace {

// Wrap `bitstream` in an extended-format WebP carrying the ICCP/EXIF chunks. Returns nullopt on
// any mux failure, and the caller then ships the un-muxed bitstream: losing the metadata is a
// far better outcome than losing the export.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> muxMetadata(
    const std::vector<std::uint8_t>& bitstream, const EmbeddedMetadata& metadata) {
    WebPMux* mux = WebPMuxNew();
    if (mux == nullptr)
        return std::nullopt;
    const auto cleanup = [&](WebPData* assembled) -> std::optional<std::vector<std::uint8_t>> {
        if (assembled != nullptr)
            WebPDataClear(assembled);
        WebPMuxDelete(mux);
        return std::nullopt;
    };

    WebPData image{bitstream.data(), bitstream.size()};
    if (WebPMuxSetImage(mux, &image, /*copy_data=*/1) != WEBP_MUX_OK)
        return cleanup(nullptr);
    if (!metadata.icc.empty()) {
        WebPData chunk{metadata.icc.data(), metadata.icc.size()};
        if (WebPMuxSetChunk(mux, "ICCP", &chunk, /*copy_data=*/1) != WEBP_MUX_OK)
            return cleanup(nullptr);
    }
    if (!metadata.exif.empty()) {
        // The WebP EXIF chunk holds the raw TIFF payload -- no "Exif\0\0" prefix, which is a
        // JPEG APP1 convention only. io::buildExifPayload produces exactly these bytes.
        WebPData chunk{metadata.exif.data(), metadata.exif.size()};
        if (WebPMuxSetChunk(mux, "EXIF", &chunk, /*copy_data=*/1) != WEBP_MUX_OK)
            return cleanup(nullptr);
    }

    WebPData assembled;
    WebPDataInit(&assembled);
    if (WebPMuxAssemble(mux, &assembled) != WEBP_MUX_OK)
        return cleanup(&assembled);
    std::vector<std::uint8_t> out(assembled.bytes, assembled.bytes + assembled.size);
    WebPDataClear(&assembled);
    WebPMuxDelete(mux);
    return out;
}

}  // namespace

std::optional<std::vector<std::uint8_t>> encodeWebp(const common::Image& image,
                                                    const WebpSaveOptions& opts,
                                                    std::string* error) {
    const auto fail = [&](const char* what) -> std::optional<std::vector<std::uint8_t>> {
        if (error)
            *error = std::string("WebP: ") + what;
        return std::nullopt;
    };
    if (image.empty())
        return fail("cannot write an empty image");
    if (image.rgba.size() < image.pixelCount() * 4)
        return fail("image buffer is smaller than its dimensions");
    constexpr std::uint32_t kWebpMaxDim = static_cast<std::uint32_t>(WEBP_MAX_DIMENSION);
    if (image.width > kWebpMaxDim || image.height > kWebpMaxDim)
        return fail("image is too large for the WebP format (16383 px maximum per side)");

    WebPConfig config;
    if (WebPConfigInit(&config) == 0)
        return fail("libwebp version mismatch");
    config.lossless = opts.lossless ? 1 : 0;
    // libwebp overloads `quality`: visual quality in lossy mode, entropy-coding effort in
    // lossless mode. The schema's help text says so, because the slider is the same widget.
    config.quality = static_cast<float>(std::clamp(opts.quality, 0, 100));
    config.method = std::clamp(opts.method, 0, 6);
    config.alpha_quality = std::clamp(opts.alphaQuality, 0, 100);
    config.exact = opts.exact ? 1 : 0;
    // near_lossless is a LOSSLESS-mode preprocessing knob (100 = off). Setting it in lossy mode
    // would be ignored at best; keeping it pinned at 100 there makes the option's visibility
    // condition in the schema and the encoder's behaviour agree.
    config.near_lossless = opts.lossless ? std::clamp(opts.nearLossless, 0, 100) : 100;
    if (WebPValidateConfig(&config) == 0)
        return fail("the encoder options are out of range");

    WebPPicture picture;
    if (WebPPictureInit(&picture) == 0)
        return fail("libwebp version mismatch");
    // ARGB input is the native form for lossless and is converted to YUV internally for lossy,
    // so one import path serves both modes.
    picture.use_argb = 1;
    picture.width = static_cast<int>(image.width);
    picture.height = static_cast<int>(image.height);
    if (WebPPictureImportRGBA(&picture, image.rgba.data(), static_cast<int>(image.width) * 4) ==
        0) {
        WebPPictureFree(&picture);
        return fail("could not import the pixels (out of memory?)");
    }

    WebPMemoryWriter writer;
    WebPMemoryWriterInit(&writer);
    picture.writer = WebPMemoryWrite;
    picture.custom_ptr = &writer;
    const int ok = WebPEncode(&config, &picture);
    const int encodeError = static_cast<int>(picture.error_code);
    WebPPictureFree(&picture);
    if (ok == 0) {
        WebPMemoryWriterClear(&writer);
        return fail(("the encoder failed (error " + std::to_string(encodeError) + ")").c_str());
    }

    std::vector<std::uint8_t> bytes(writer.mem, writer.mem + writer.size);
    WebPMemoryWriterClear(&writer);

    if (!opts.metadata.empty()) {
        if (auto muxed = muxMetadata(bytes, opts.metadata))
            return muxed;
        // Metadata muxing failed; the picture itself is fine, so ship it.
    }
    return bytes;
}

#else  // MOSAIC_HAVE_WEBP

namespace detail {

std::optional<common::Image> decodeWebp(const std::vector<std::uint8_t>&, std::string* error) {
    if (error)
        *error = "WebP: support was not compiled in";
    return std::nullopt;
}

bool probeWebpDimensions(const std::vector<std::uint8_t>&, std::uint32_t&, std::uint32_t&) {
    return false;
}

}  // namespace detail

bool webpSupported() noexcept { return false; }

std::optional<std::vector<std::uint8_t>> encodeWebp(const common::Image&, const WebpSaveOptions&,
                                                    std::string* error) {
    if (error)
        *error = "WebP: support was not compiled in";
    return std::nullopt;
}

#endif  // MOSAIC_HAVE_WEBP

bool saveWebp(const common::Image& image, const std::string& path, const WebpSaveOptions& opts,
              std::string* error) {
    // One encode path: build the file in memory (encodeWebp already set *error on failure), then
    // commit it to disk -- savePng's FILE handling and its two error strings.
    std::optional<std::vector<std::uint8_t>> bytes = encodeWebp(image, opts, error);
    if (!bytes)
        return false;

    const auto fail = [&](const char* what) {
        if (error)
            *error = std::string("WebP: ") + what;
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

}  // namespace mosaic::io
