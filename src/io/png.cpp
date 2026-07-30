#include "common/fs_path.hpp"
#include "io/detail.hpp"
#include "io/io.hpp"

#include <png.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace mosaic::io {

namespace {

// The metadata chunks, applied to a writer that has already had setjmp installed. Split out
// because savePng and encodePng need exactly the same three, and because it keeps the
// declaration-before-setjmp discipline in one readable place at each call site.
//
// Chunk choices: eXIf (standardized 2017) carries the raw TIFF payload with NO "Exif\0\0"
// prefix -- that prefix is a JPEG APP1 convention, and libpng writes the payload verbatim.
// iCCP names the profile "ICC profile", which is what every other writer uses. pHYs records
// pixels per METRE, so the dpi has to be converted, and 72 dpi is skipped: it is the value that
// means "no opinion", and writing it would make every plain export claim a print size.
void writePngMetadata(png_structp png, png_infop info, const EmbeddedMetadata& metadata) {
    if (!metadata.exif.empty() && metadata.exif.size() <= 0x7FFFFFFFu)
        png_set_eXIf_1(png, info, static_cast<png_uint_32>(metadata.exif.size()),
                       const_cast<png_bytep>(metadata.exif.data()));
    if (!metadata.icc.empty() && metadata.icc.size() <= 0x7FFFFFFFu) {
        // libpng validates an ICC profile hard: a structurally fine profile whose colour space
        // is not RGB (a CMYK press profile, say) is rejected for an RGBA PNG, and by default
        // that rejection is a png_error -- i.e. a longjmp that would fail the whole export over
        // a side-car chunk. Bracketing the call in benign-error mode downgrades it to a warning,
        // so the profile is dropped and the PICTURE still gets written. Restored immediately:
        // the flag also relaxes app errors, which we want strict everywhere else.
        png_set_benign_errors(png, /*allowed=*/1);
        png_set_iCCP(png, info, "ICC profile", PNG_COMPRESSION_TYPE_BASE,
                     metadata.icc.data(), static_cast<png_uint_32>(metadata.icc.size()));
        png_set_benign_errors(png, /*allowed=*/0);
    }
    if (metadata.dpi > 0.0 && std::abs(metadata.dpi - 72.0) > 1e-9) {
        const double perMetre = metadata.dpi / 0.0254;
        if (perMetre < 4294967295.0) {
            const auto ppm = static_cast<png_uint_32>(perMetre + 0.5);
            png_set_pHYs(png, info, ppm, ppm, PNG_RESOLUTION_METER);
        }
    }
}

} // namespace

namespace detail {

std::optional<common::Image> decodePng(const std::vector<std::uint8_t>& buf,
                                       std::string* error) {
    png_image image;
    std::memset(&image, 0, sizeof image);
    image.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_memory(&image, buf.data(), buf.size()) == 0) {
        if (error)
            *error = std::string("PNG: ") + image.message;
        return std::nullopt;
    }
    if (image.width == 0 || image.height == 0 || image.width > kMaxDim || image.height > kMaxDim) {
        png_image_free(&image);
        if (error)
            *error = "PNG: unsupported image dimensions";
        return std::nullopt;
    }
    image.format = PNG_FORMAT_RGBA; // 8-bit straight-alpha RGBA, whatever the source colour type
    common::Image out(image.width, image.height); // rgba sized w*h*4 == PNG_IMAGE_SIZE(image)
    if (png_image_finish_read(&image, nullptr, out.rgba.data(), 0, nullptr) == 0) {
        if (error)
            *error = std::string("PNG: ") + image.message;
        png_image_free(&image);
        return std::nullopt;
    }
    png_image_free(&image);
    return out;
}

} // namespace detail

bool savePng(const common::Image& image, const std::string& path, const PngSaveOptions& opts,
             std::string* error) {
    const auto fail = [&](const char* what) {
        if (error)
            *error = std::string("PNG: ") + what;
        return false;
    };
    if (image.empty())
        return fail("cannot write an empty image");
    if (image.rgba.size() < image.pixelCount() * 4)
        return fail("image buffer is smaller than its dimensions");

    std::FILE* fp = common::fopenUtf8(path, "wb");
    if (fp == nullptr)
        return fail("could not open the file for writing");

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png == nullptr) {
        std::fclose(fp);
        return fail("could not allocate the writer");
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        std::fclose(fp);
        return fail("could not allocate the writer info");
    }

    // Build the row pointer table BEFORE the setjmp target: a libpng error longjmps back to it, and
    // locals constructed *after* setjmp would leak (longjmp skips their destructors). rows points
    // straight into the image's tight RGBA buffer -- const_cast only because libpng's write API
    // takes a non-const png_bytep it never writes through.
    std::vector<png_bytep> rows(image.height);
    for (std::uint32_t y = 0; y < image.height; ++y)
        rows[y] = const_cast<png_bytep>(image.rgba.data() + static_cast<std::size_t>(y) * image.width * 4);

    // NOLINTNEXTLINE(cert-err52-cpp): setjmp/longjmp is libpng's mandated C error protocol.
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        std::fclose(fp);
        return fail("libpng reported a write error");
    }

    png_init_io(png, fp);
    png_set_compression_level(png, std::clamp(opts.compression, 0, 9));
    const int interlace = opts.interlace ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE;
    png_set_IHDR(png, info, image.width, image.height, /*bit_depth=*/8, PNG_COLOR_TYPE_RGBA,
                 interlace, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    writePngMetadata(png, info, opts.metadata);
    png_write_info(png, info);
    png_write_image(png, rows.data());
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);

    if (std::fclose(fp) != 0)
        return fail("could not flush the file to disk");
    return true;
}

std::optional<std::vector<std::uint8_t>> encodePng(const common::Image& image,
                                                   const PngSaveOptions& opts,
                                                   std::string* error) {
    const auto fail = [&](const char* what) -> std::optional<std::vector<std::uint8_t>> {
        if (error)
            *error = std::string("PNG: ") + what;
        return std::nullopt;
    };
    if (image.empty())
        return fail("cannot write an empty image");
    if (image.rgba.size() < image.pixelCount() * 4)
        return fail("image buffer is smaller than its dimensions");

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png == nullptr)
        return fail("could not allocate the writer");
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        return fail("could not allocate the writer info");
    }

    // As in savePng: everything a longjmp would skip the destructor of is built BEFORE setjmp.
    // The output lives on the HEAP: a local vector mutated between setjmp and longjmp is
    // indeterminate at the jump target, but a pointee is ordinary heap state.
    auto out = std::make_unique<std::vector<std::uint8_t>>();
    std::vector<png_bytep> rows(image.height);
    for (std::uint32_t y = 0; y < image.height; ++y)
        rows[y] = const_cast<png_bytep>(image.rgba.data()
                                        + static_cast<std::size_t>(y) * image.width * 4);

    // NOLINTNEXTLINE(cert-err52-cpp): setjmp/longjmp is libpng's mandated C error protocol.
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        return fail("libpng reported a write error");
    }

    png_set_write_fn(
        png, out.get(),
        [](png_structp p, png_bytep data, png_size_t n) {
            auto* v = static_cast<std::vector<std::uint8_t>*>(png_get_io_ptr(p));
            v->insert(v->end(), data, data + n);
        },
        [](png_structp) {});
    png_set_compression_level(png, std::clamp(opts.compression, 0, 9));
    const int interlace = opts.interlace ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE;
    png_set_IHDR(png, info, image.width, image.height, /*bit_depth=*/8, PNG_COLOR_TYPE_RGBA,
                 interlace, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    writePngMetadata(png, info, opts.metadata);
    png_write_info(png, info);
    png_write_image(png, rows.data());
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    return std::move(*out);
}

} // namespace mosaic::io
