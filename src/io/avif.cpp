#include "common/fs_path.hpp"
#include "io/detail.hpp"
#include "io/io.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

// avif -- the AVIF codec (M4), wrapping the system libavif.
//
// ⚠ INVARIANT -- ENCODER CHOICE, and the reason this file reads the way it does. Which AV1
// encoder sits behind AVIF is a deliberate project decision and is not left to chance. libavif's
// AVIF_CODEC_CHOICE_AUTO resolves to whatever the distribution happened to build in -- and several
// distributions build in a Rust encoder Mosaic does not ship, does not link and will not invoke.
// So:
//
//   * the encoder object always carries an EXPLICIT codecChoice, never AUTO;
//   * that choice is AOM (libaom) first, SVT (SVT-AV1) as the fallback, and nothing else;
//   * avifSupported() answers false when neither is present, so the format is simply not offered
//     rather than silently encoded by something else.
//
// That costs a supported configuration or two and it is a hard constraint on this file, not an
// oversight -- do not "simplify" it back to AUTO.
//
// The decode side is separate: a decoder is not an encoder, AUTO there can only pick a decoder,
// and refusing to READ files a user already has would help nobody.
#ifdef MOSAIC_HAVE_AVIF
#include <avif/avif.h>
#endif

namespace mosaic::io {

#ifdef MOSAIC_HAVE_AVIF

namespace {

// The AV1 encoder Mosaic is willing to drive, or AVIF_CODEC_CHOICE_AUTO when there is none --
// which is the one value the encode path treats as "unavailable" rather than as a choice.
[[nodiscard]] avifCodecChoice chosenEncoder() noexcept {
    if (avifCodecName(AVIF_CODEC_CHOICE_AOM, AVIF_CODEC_FLAG_CAN_ENCODE) != nullptr)
        return AVIF_CODEC_CHOICE_AOM;
    if (avifCodecName(AVIF_CODEC_CHOICE_SVT, AVIF_CODEC_FLAG_CAN_ENCODE) != nullptr)
        return AVIF_CODEC_CHOICE_SVT;
    return AVIF_CODEC_CHOICE_AUTO;
}

[[nodiscard]] avifPixelFormat pixelFormatOf(AvifSaveOptions::Yuv yuv) noexcept {
    switch (yuv) {
    case AvifSaveOptions::Yuv::Yuv422: return AVIF_PIXEL_FORMAT_YUV422;
    case AvifSaveOptions::Yuv::Yuv420: return AVIF_PIXEL_FORMAT_YUV420;
    case AvifSaveOptions::Yuv::Yuv400: return AVIF_PIXEL_FORMAT_YUV400;
    case AvifSaveOptions::Yuv::Yuv444: break;
    }
    return AVIF_PIXEL_FORMAT_YUV444;
}

}  // namespace

namespace detail {

std::optional<common::Image> decodeAvif(const std::vector<std::uint8_t>& buf, std::string* error) {
    const auto fail = [&](const std::string& what) -> std::optional<common::Image> {
        if (error)
            *error = "AVIF: " + what;
        return std::nullopt;
    };
    if (buf.empty())
        return fail("empty file");

    avifDecoder* decoder = avifDecoderCreate();
    if (decoder == nullptr)
        return fail("could not allocate the decoder");
    avifImage* image = avifImageCreateEmpty();
    if (image == nullptr) {
        avifDecoderDestroy(decoder);
        return fail("could not allocate the image");
    }
    const auto cleanup = [&](const std::string& what) -> std::optional<common::Image> {
        avifImageDestroy(image);
        avifDecoderDestroy(decoder);
        return fail(what);
    };

    const avifResult read = avifDecoderReadMemory(decoder, image, buf.data(), buf.size());
    if (read != AVIF_RESULT_OK)
        return cleanup(avifResultToString(read));
    if (!dimensionsPlausible(image->width, image->height))
        return cleanup("unsupported image dimensions");

    common::Image out(image->width, image->height);
    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, image);
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.depth = 8;  // rescale a 10/12-bit file down to our 8-bit pipeline
    // Ask for STRAIGHT alpha explicitly: libavif un-premultiplies on the way out when the file
    // says it is premultiplied and we say we want it not to be.
    rgb.alphaPremultiplied = AVIF_FALSE;
    rgb.pixels = out.rgba.data();
    rgb.rowBytes = image->width * 4;
    const avifResult convert = avifImageYUVToRGB(image, &rgb);
    if (convert != AVIF_RESULT_OK)
        return cleanup(avifResultToString(convert));

    avifImageDestroy(image);
    avifDecoderDestroy(decoder);
    return out;
}

}  // namespace detail

bool avifSupported() noexcept { return chosenEncoder() != AVIF_CODEC_CHOICE_AUTO; }

std::optional<std::vector<std::uint8_t>> encodeAvif(const common::Image& image,
                                                    const AvifSaveOptions& opts,
                                                    std::string* error) {
    const auto fail = [&](const std::string& what) -> std::optional<std::vector<std::uint8_t>> {
        if (error)
            *error = "AVIF: " + what;
        return std::nullopt;
    };
    if (image.empty())
        return fail("cannot write an empty image");
    if (image.rgba.size() < image.pixelCount() * 4)
        return fail("image buffer is smaller than its dimensions");
    const avifCodecChoice codec = chosenEncoder();
    if (codec == AVIF_CODEC_CHOICE_AUTO)
        return fail("this build of libavif has no AV1 encoder Mosaic can use "
                    "(libaom or SVT-AV1 is required)");

    // Lossless needs three things together: quality 100, 4:4:4 chroma, and the IDENTITY matrix
    // (i.e. store GBR, no colour transform). Quality 100 alone is NOT lossless -- the RGB->YUV
    // conversion rounds -- which is exactly the sort of half-truth the loss banner must never
    // repeat, so the encoder enforces the whole triple rather than trusting the caller.
    const avifPixelFormat format =
        opts.lossless ? AVIF_PIXEL_FORMAT_YUV444 : pixelFormatOf(opts.yuv);
    avifImage* target = avifImageCreate(image.width, image.height, 8, format);
    if (target == nullptr)
        return fail("could not allocate the image");
    avifEncoder* encoder = avifEncoderCreate();
    if (encoder == nullptr) {
        avifImageDestroy(target);
        return fail("could not allocate the encoder");
    }
    avifRWData output;
    output.data = nullptr;
    output.size = 0;
    const auto cleanup = [&](const std::string& what) -> std::optional<std::vector<std::uint8_t>> {
        avifRWDataFree(&output);
        avifEncoderDestroy(encoder);
        avifImageDestroy(target);
        return fail(what);
    };

    target->yuvRange = AVIF_RANGE_FULL;
    target->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;         // == SRGB primaries
    target->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
    target->matrixCoefficients =
        opts.lossless ? AVIF_MATRIX_COEFFICIENTS_IDENTITY : AVIF_MATRIX_COEFFICIENTS_BT601;
    target->alphaPremultiplied = AVIF_FALSE;  // our pixels are straight alpha

    // Metadata is a SIDE-CAR: a payload libavif will not take costs the payload, never the export.
    // (The same rule png.cpp implements with png_set_benign_errors, for the same reason -- losing a
    // colour profile is a metadata problem, losing the picture would be a bug. These two calls only
    // ever fail on an allocation, so in practice this is belt-and-braces; the point is that the
    // failure mode is stated, and is the harmless one.)
    if (!opts.metadata.icc.empty())
        (void)avifImageSetProfileICC(target, opts.metadata.icc.data(), opts.metadata.icc.size());
    if (!opts.metadata.exif.empty()) {
        // libavif locates the TIFF header inside the payload itself, so the raw bytes
        // io::buildExifPayload produces are exactly what goes in.
        (void)avifImageSetMetadataExif(target, opts.metadata.exif.data(),
                                       opts.metadata.exif.size());
    }

    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, target);
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.depth = 8;
    rgb.alphaPremultiplied = AVIF_FALSE;
    // libavif does not write through the source buffer during RGB->YUV; the cast only satisfies
    // the struct's single non-const pixel pointer, which serves both directions.
    rgb.pixels = const_cast<std::uint8_t*>(image.rgba.data());
    rgb.rowBytes = image.width * 4;
    const avifResult convert = avifImageRGBToYUV(target, &rgb);
    if (convert != AVIF_RESULT_OK)
        return cleanup(avifResultToString(convert));

    encoder->codecChoice = codec;  // never AUTO -- see the file header
    encoder->maxThreads = 4;
    encoder->speed = std::clamp(opts.speed, AVIF_SPEED_SLOWEST, AVIF_SPEED_FASTEST);
    encoder->quality = opts.lossless ? AVIF_QUALITY_LOSSLESS : std::clamp(opts.quality, 0, 100);
    encoder->qualityAlpha =
        opts.lossless ? AVIF_QUALITY_LOSSLESS : std::clamp(opts.alphaQuality, 0, 100);

    const avifResult wrote = avifEncoderWrite(encoder, target, &output);
    if (wrote != AVIF_RESULT_OK)
        return cleanup(avifResultToString(wrote));

    std::vector<std::uint8_t> bytes(output.data, output.data + output.size);
    avifRWDataFree(&output);
    avifEncoderDestroy(encoder);
    avifImageDestroy(target);
    return bytes;
}

#else  // MOSAIC_HAVE_AVIF

namespace detail {

std::optional<common::Image> decodeAvif(const std::vector<std::uint8_t>&, std::string* error) {
    if (error)
        *error = "AVIF: support was not compiled in";
    return std::nullopt;
}

}  // namespace detail

bool avifSupported() noexcept { return false; }

std::optional<std::vector<std::uint8_t>> encodeAvif(const common::Image&, const AvifSaveOptions&,
                                                    std::string* error) {
    if (error)
        *error = "AVIF: support was not compiled in";
    return std::nullopt;
}

#endif  // MOSAIC_HAVE_AVIF

bool saveAvif(const common::Image& image, const std::string& path, const AvifSaveOptions& opts,
              std::string* error) {
    std::optional<std::vector<std::uint8_t>> bytes = encodeAvif(image, opts, error);
    if (!bytes)
        return false;

    const auto fail = [&](const char* what) {
        if (error)
            *error = std::string("AVIF: ") + what;
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
