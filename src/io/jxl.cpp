#include "io/io.hpp"

#include "common/fs_path.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

// jxl -- JPEG XL encoder (the sibling of png.cpp/jpeg.cpp) wrapping the system libjxl. The whole
// implementation is compile-time gated on MOSAIC_HAVE_JXL so a machine without libjxl still builds:
// the #else branch below defines the same three symbols as inert stubs that never touch a jxl header.
// jxlSupported() is the runtime probe the UI uses to decide whether to offer .jxl at all.
#ifdef MOSAIC_HAVE_JXL
#include <jxl/encode.h>
#include <jxl/encode_cxx.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/resizable_parallel_runner_cxx.h>
#endif

namespace mosaic::io {

#ifdef MOSAIC_HAVE_JXL

bool jxlSupported() noexcept { return true; }

std::optional<std::vector<std::uint8_t>> encodeJxl(const common::Image& image,
                                                   const JxlSaveOptions& opts, std::string* error) {
    const auto fail = [&](const std::string& what) -> std::optional<std::vector<std::uint8_t>> {
        if (error)
            *error = "JXL: " + what;
        return std::nullopt;
    };
    if (image.empty())
        return fail("cannot write an empty image");
    if (image.rgba.size() < image.pixelCount() * 4)
        return fail("image buffer is smaller than its dimensions");

    JxlEncoderPtr enc = JxlEncoderMake(nullptr);
    if (!enc)
        return fail("could not allocate the encoder");

    // The resizable runner (vs the fixed thread_parallel_runner) keeps the only threads dependency
    // at libjxl_threads while still using every core; suggest a thread count sized to the image.
    JxlResizableParallelRunnerPtr runner = JxlResizableParallelRunnerMake(nullptr);
    if (!runner)
        return fail("could not allocate the thread runner");
    JxlResizableParallelRunnerSetThreads(
        runner.get(), JxlResizableParallelRunnerSuggestThreads(image.width, image.height));
    if (JxlEncoderSetParallelRunner(enc.get(), JxlResizableParallelRunner, runner.get())
        != JXL_ENC_SUCCESS)
        return fail("could not attach the thread runner");

    // Metadata boxes need the CONTAINER form (the boxed file rather than a bare codestream), and
    // both decisions have to be made before any frame is added. A metadata-free encode is left
    // alone, so it still writes the bare codestream every decoder reads fastest.
    const bool wantExifBox = !opts.metadata.exif.empty();
    if (wantExifBox) {
        if (JxlEncoderUseContainer(enc.get(), JXL_TRUE) != JXL_ENC_SUCCESS)
            return fail("could not switch to the container format");
        if (JxlEncoderUseBoxes(enc.get()) != JXL_ENC_SUCCESS)
            return fail("could not enable metadata boxes");
    }

    // Basic info: 8-bit RGB + a single straight-alpha extra channel, matching the compositor's
    // flatten (common::Image is 8-bit, straight alpha). uses_original_profile must be set for the
    // lossless path -- distance 0 alone is not sufficient for a bit-exact round-trip.
    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = image.width;
    info.ysize = image.height;
    info.bits_per_sample = 8;
    info.exponent_bits_per_sample = 0;
    info.num_color_channels = 3;
    info.num_extra_channels = 1;
    info.alpha_bits = 8;
    info.alpha_exponent_bits = 0;
    info.alpha_premultiplied = JXL_FALSE; // our alpha is straight, never premultiplied
    info.uses_original_profile = opts.lossless ? JXL_TRUE : JXL_FALSE;
    if (JxlEncoderSetBasicInfo(enc.get(), &info) != JXL_ENC_SUCCESS)
        return fail("could not set the basic info");

    // The original colour encoding. SetICCProfile and SetColorEncoding are ALTERNATIVES -- libjxl
    // says exactly one of the two may be used -- so an embedded profile replaces the sRGB tag
    // rather than joining it.
    //
    // The care here mirrors PNG's benign-error bracketing: a profile libjxl will not accept costs
    // the profile, not the export. So the ICC attempt is allowed to fail, and the sRGB tag is the
    // fallback on the same encoder -- at that point nothing has been set, so the fallback is a
    // first assignment rather than an overwrite.
    bool tagged = false;
    if (!opts.metadata.icc.empty()) {
        tagged = JxlEncoderSetICCProfile(enc.get(), opts.metadata.icc.data(),
                                         opts.metadata.icc.size()) == JXL_ENC_SUCCESS;
    }
    if (!tagged) {
        // Tag the pixels as (nonlinear) sRGB, which is what the 8-bit compositor output is.
        JxlColorEncoding color;
        JxlColorEncodingSetToSRGB(&color, /*is_gray=*/JXL_FALSE);
        if (JxlEncoderSetColorEncoding(enc.get(), &color) != JXL_ENC_SUCCESS)
            return fail("could not set the color encoding");
    }

    // Frame settings are owned by the encoder (freed with it), so this raw pointer is not leaked.
    JxlEncoderFrameSettings* fs = JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
    if (fs == nullptr)
        return fail("could not create the frame settings");

    if (opts.lossless) {
        // The lossless switch overrides distance/modular/color-transform for a bit-exact result;
        // do NOT also set a distance here (SetFrameLossless owns that decision on this path).
        if (JxlEncoderSetFrameLossless(fs, JXL_TRUE) != JXL_ENC_SUCCESS)
            return fail("could not enable lossless mode");
    } else {
        // Butteraugli distance: 0 = lossless, 1.0 = visually lossless, up to 25. Clamp defensively.
        const float distance = std::clamp(opts.distance, 0.0f, 25.0f);
        if (JxlEncoderSetFrameDistance(fs, distance) != JXL_ENC_SUCCESS)
            return fail("could not set the butteraugli distance");
    }
    // Effort 1(fast)..9(slow/best); 10+ needs JxlEncoderAllowExpertOptions, which we do not enable.
    const int effort = std::clamp(opts.effort, 1, 9);
    if (JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, effort) != JXL_ENC_SUCCESS)
        return fail("could not set the encoder effort");

    // The Exif box. Its contents are the raw TIFF payload io::buildExifPayload produces, prefixed
    // by a FOUR-BYTE TIFF-HEADER OFFSET that JXL's box format requires and no other container
    // does -- four zero bytes here, because our header follows immediately. Left uncompressed: a
    // "brob" box would need libjxl's Brotli support and saves a few hundred bytes on a payload
    // this small.
    //
    // libjxl is explicit that the box and the codestream must AGREE about orientation, and that
    // the codestream wins where they disagree. They do agree: the payload's orientation reads 1
    // (io::exifForExport) and the codestream is written upright.
    if (wantExifBox) {
        std::vector<std::uint8_t> box(4, 0);
        box.insert(box.end(), opts.metadata.exif.begin(), opts.metadata.exif.end());
        if (JxlEncoderAddBox(enc.get(), "Exif", box.data(), box.size(), JXL_FALSE)
            != JXL_ENC_SUCCESS)
            return fail("could not add the Exif metadata box");
    }

    // Interleaved 8-bit RGBA; endianness is irrelevant for a 1-byte type so NATIVE is fine.
    JxlPixelFormat fmt{/*num_channels=*/4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, /*align=*/0};
    if (JxlEncoderAddImageFrame(fs, &fmt, image.rgba.data(), image.rgba.size()) != JXL_ENC_SUCCESS)
        return fail("could not add the image frame");
    // Single-frame, single-shot: this closes the FRAMES and, when boxes were enabled, the boxes
    // too -- which libjxl requires before the drain loop below, or the codestream is truncated.
    JxlEncoderCloseInput(enc.get());

    // Standard libjxl drain loop: ProcessOutput writes into [next_out, next_out + avail_out) and
    // advances both; NEED_MORE_OUTPUT means the tail filled up, so we grow and re-point at the free
    // tail (resize may reallocate, hence recomputing next_out from data() + written each time).
    std::vector<std::uint8_t> compressed(std::size_t{64} * 1024); // 64 KiB working buffer
    std::uint8_t* next_out = compressed.data();
    std::size_t avail_out = compressed.size();
    JxlEncoderStatus status = JXL_ENC_NEED_MORE_OUTPUT;
    while (status == JXL_ENC_NEED_MORE_OUTPUT) {
        status = JxlEncoderProcessOutput(enc.get(), &next_out, &avail_out);
        if (status == JXL_ENC_NEED_MORE_OUTPUT) {
            const std::size_t written = static_cast<std::size_t>(next_out - compressed.data());
            compressed.resize(compressed.size() * 2);
            next_out = compressed.data() + written;
            avail_out = compressed.size() - written;
        }
    }
    if (status != JXL_ENC_SUCCESS)
        return fail("the encoder reported an error (code "
                    + std::to_string(static_cast<int>(JxlEncoderGetError(enc.get()))) + ")");

    // Trim the working buffer down to exactly the bytes the encoder wrote.
    compressed.resize(static_cast<std::size_t>(next_out - compressed.data()));
    return compressed;
}

bool saveJxl(const common::Image& image, const std::string& path, const JxlSaveOptions& opts,
             std::string* error) {
    // One encode path: build the codestream in memory (encodeJxl already set *error on failure),
    // then commit it to disk. Mirrors savePng's FILE handling and its two error strings.
    std::optional<std::vector<std::uint8_t>> bytes = encodeJxl(image, opts, error);
    if (!bytes)
        return false;

    const auto fail = [&](const char* what) {
        if (error)
            *error = std::string("JXL: ") + what;
        return false;
    };
    std::FILE* fp = common::fopenUtf8(path, "wb");
    if (fp == nullptr)
        return fail("could not open the file for writing");
    if (!bytes->empty()
        && std::fwrite(bytes->data(), 1, bytes->size(), fp) != bytes->size()) {
        std::fclose(fp);
        return fail("could not write the file");
    }
    if (std::fclose(fp) != 0)
        return fail("could not flush the file to disk");
    return true;
}

#else // MOSAIC_HAVE_JXL

// libjxl was not found at configure time. The three symbols still exist so callers link, but each
// reports the feature is absent -- no jxl header is included in this translation unit.
bool jxlSupported() noexcept { return false; }

std::optional<std::vector<std::uint8_t>> encodeJxl(const common::Image&, const JxlSaveOptions&,
                                                   std::string* error) {
    if (error)
        *error = "JPEG XL: support was not compiled in";
    return std::nullopt;
}

bool saveJxl(const common::Image&, const std::string&, const JxlSaveOptions&, std::string* error) {
    if (error)
        *error = "JPEG XL: support was not compiled in";
    return false;
}

#endif // MOSAIC_HAVE_JXL

} // namespace mosaic::io
