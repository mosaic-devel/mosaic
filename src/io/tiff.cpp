#include "common/fs_path.hpp"
#include "io/detail.hpp"
#include "io/io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

// tiff -- the TIFF codec (M4), wrapping the system libtiff. Compile-time gated on
// MOSAIC_HAVE_TIFF, same shape as jxl.cpp/webp.cpp.
//
// Both directions run over a MEMORY stream (TIFFClientOpen), not a path: the Export modal wants
// the bytes for its trial-encode size readout and its preview, and io's decode entry point is
// handed a buffer. libtiff is perfectly happy driving arbitrary client I/O, so this costs nothing
// but the seven callbacks below.
//
// The decode has two paths on purpose. TIFFReadRGBAImageOriented handles EVERY TIFF -- tiled,
// planar, 16-bit, palette, CMYK, CCITT -- but it always hands back PREMULTIPLIED alpha (its
// unassociated-alpha tile writer multiplies RGB by alpha on the way out), and un-premultiplying
// that is irreversible at low alpha. So the ordinary case Mosaic itself writes -- 8-bit,
// contiguous, RGB(A), stripped -- is read scanline-by-scanline instead, which is bit-exact; the
// general reader is the fallback for everything else.
#ifdef MOSAIC_HAVE_TIFF
#include <tiffio.h>
#endif

namespace mosaic::io {

#ifdef MOSAIC_HAVE_TIFF

namespace {

// libtiff's default handlers print to stderr. A GUI application must not spray a library's
// opinion of a user's file across the terminal -- every failure here is reported through a return
// value and an error string instead. Installed once, process-wide, because that is the only
// granularity the classic handler API has.
void silenceLibtiff() {
    static const bool installed = [] {
        TIFFSetErrorHandler(nullptr);
        TIFFSetWarningHandler(nullptr);
        return true;
    }();
    (void)installed;
}

// A whole TIFF held in memory, driven through libtiff's client-I/O hooks.
struct MemoryFile {
    std::vector<std::uint8_t> data;
    std::size_t pos = 0;
};

tmsize_t memoryRead(thandle_t handle, void* buffer, tmsize_t size) {
    auto* file = static_cast<MemoryFile*>(handle);
    if (size <= 0)
        return 0;
    const std::size_t want = static_cast<std::size_t>(size);
    const std::size_t available = file->pos < file->data.size() ? file->data.size() - file->pos : 0;
    const std::size_t got = std::min(want, available);
    if (got != 0)
        std::memcpy(buffer, file->data.data() + file->pos, got);
    file->pos += got;
    return static_cast<tmsize_t>(got);
}

tmsize_t memoryWrite(thandle_t handle, void* buffer, tmsize_t size) {
    auto* file = static_cast<MemoryFile*>(handle);
    if (size <= 0)
        return 0;
    const std::size_t want = static_cast<std::size_t>(size);
    if (want > SIZE_MAX - file->pos)
        return 0;
    if (file->pos + want > file->data.size())
        file->data.resize(file->pos + want);  // zero-fills any gap a seek left behind
    std::memcpy(file->data.data() + file->pos, buffer, want);
    file->pos += want;
    return static_cast<tmsize_t>(want);
}

toff_t memorySeek(thandle_t handle, toff_t offset, int whence) {
    auto* file = static_cast<MemoryFile*>(handle);
    std::size_t base = 0;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = file->pos; break;
    case SEEK_END: base = file->data.size(); break;
    default: return static_cast<toff_t>(-1);
    }
    if (offset > static_cast<toff_t>(SIZE_MAX - base))
        return static_cast<toff_t>(-1);  // a hostile directory offset cannot wrap the cursor
    file->pos = base + static_cast<std::size_t>(offset);
    return static_cast<toff_t>(file->pos);
}

int memoryClose(thandle_t) { return 0; }

toff_t memorySize(thandle_t handle) {
    return static_cast<toff_t>(static_cast<MemoryFile*>(handle)->data.size());
}

int memoryMap(thandle_t, void**, toff_t*) { return 0; }  // no mmap over a vector

void memoryUnmap(thandle_t, void*, toff_t) {}

[[nodiscard]] TIFF* openMemory(MemoryFile& file, const char* mode) {
    return TIFFClientOpen("mosaic", mode, static_cast<thandle_t>(&file), memoryRead, memoryWrite,
                          memorySeek, memoryClose, memorySize, memoryMap, memoryUnmap);
}

[[nodiscard]] std::uint16_t compressionTag(TiffSaveOptions::Compression compression) noexcept {
    switch (compression) {
    case TiffSaveOptions::Compression::None: return COMPRESSION_NONE;
    case TiffSaveOptions::Compression::Lzw: return COMPRESSION_LZW;
    case TiffSaveOptions::Compression::PackBits: return COMPRESSION_PACKBITS;
    case TiffSaveOptions::Compression::Zstd: return COMPRESSION_ZSTD;
    case TiffSaveOptions::Compression::Deflate: break;
    }
    return COMPRESSION_ADOBE_DEFLATE;
}

// Only the codecs that model the data as a horizontal signal accept a predictor; handing one to
// PackBits or to an uncompressed strip is an error, not a no-op.
[[nodiscard]] bool acceptsPredictor(TiffSaveOptions::Compression compression) noexcept {
    return compression == TiffSaveOptions::Compression::Lzw ||
           compression == TiffSaveOptions::Compression::Deflate ||
           compression == TiffSaveOptions::Compression::Zstd;
}

// The bit-exact scanline path: 8-bit, contiguous, top-left RGB or RGBA, stripped.
[[nodiscard]] bool readContiguousRgba(TIFF* tif, std::uint32_t width, std::uint32_t height,
                                      std::uint16_t samples, std::uint16_t extra,
                                      common::Image& out) {
    const tmsize_t stride = TIFFScanlineSize(tif);
    if (stride < static_cast<tmsize_t>(static_cast<std::size_t>(width) * samples))
        return false;
    std::vector<std::uint8_t> row(static_cast<std::size_t>(stride));
    const bool associated = samples >= 4 && extra == EXTRASAMPLE_ASSOCALPHA;
    for (std::uint32_t y = 0; y < height; ++y) {
        if (TIFFReadScanline(tif, row.data(), y, 0) < 0)
            return false;
        std::uint8_t* dst = out.rgba.data() + static_cast<std::size_t>(y) * width * 4;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint8_t* src = row.data() + static_cast<std::size_t>(x) * samples;
            std::uint8_t a = samples >= 4 ? src[3] : 255;
            std::uint8_t r = src[0];
            std::uint8_t g = src[1];
            std::uint8_t b = src[2];
            if (associated && a != 0 && a != 255) {
                const auto undo = [a](std::uint8_t c) {
                    return static_cast<std::uint8_t>(
                        std::min<std::uint32_t>(255u, (std::uint32_t{c} * 255u + a / 2u) / a));
                };
                r = undo(r);
                g = undo(g);
                b = undo(b);
            } else if (associated && a == 0) {
                r = g = b = 0;
            }
            dst[x * 4 + 0] = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = b;
            dst[x * 4 + 3] = a;
        }
    }
    return true;
}

}  // namespace

namespace detail {

std::optional<common::Image> decodeTiff(const std::vector<std::uint8_t>& buf, std::string* error) {
    silenceLibtiff();
    const auto fail = [&](const char* what) -> std::optional<common::Image> {
        if (error)
            *error = std::string("TIFF: ") + what;
        return std::nullopt;
    };
    if (buf.empty())
        return fail("empty file");

    MemoryFile file;
    file.data = buf;
    TIFF* tif = openMemory(file, "r");
    if (tif == nullptr)
        return fail("not a readable TIFF file");

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width) != 1 ||
        TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height) != 1 ||
        !dimensionsPlausible(width, height)) {
        TIFFClose(tif);
        return fail("unsupported image dimensions");
    }

    common::Image out(width, height);
    std::uint16_t bits = 0;
    std::uint16_t samples = 0;
    std::uint16_t planar = 0;
    std::uint16_t photometric = 0;
    std::uint16_t orientation = ORIENTATION_TOPLEFT;
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bits);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &samples);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &photometric);
    TIFFGetFieldDefaulted(tif, TIFFTAG_ORIENTATION, &orientation);
    std::uint16_t extraCount = 0;
    std::uint16_t* extraTypes = nullptr;
    std::uint16_t extra = EXTRASAMPLE_UNASSALPHA;
    if (TIFFGetField(tif, TIFFTAG_EXTRASAMPLES, &extraCount, &extraTypes) == 1 && extraCount >= 1 &&
        extraTypes != nullptr)
        extra = extraTypes[0];

    const bool simple = bits == 8 && planar == PLANARCONFIG_CONTIG &&
                        photometric == PHOTOMETRIC_RGB && (samples == 3 || samples == 4) &&
                        orientation == ORIENTATION_TOPLEFT && TIFFIsTiled(tif) == 0;
    if (simple && readContiguousRgba(tif, width, height, samples, extra, out)) {
        TIFFClose(tif);
        return out;
    }

    // The general reader: every other TIFF shape, at the cost of libtiff's premultiplied output.
    std::vector<std::uint32_t> raster(static_cast<std::size_t>(width) * height, 0);
    if (TIFFReadRGBAImageOriented(tif, width, height, raster.data(), ORIENTATION_TOPLEFT,
                                  /*stopOnError=*/0) == 0) {
        TIFFClose(tif);
        return fail("the image data could not be decoded");
    }
    TIFFClose(tif);
    for (std::size_t i = 0; i < raster.size(); ++i) {
        const std::uint32_t px = raster[i];
        const std::uint32_t a = TIFFGetA(px);
        const auto unassociate = [a](std::uint32_t c) {
            if (a == 0)
                return std::uint8_t{0};
            if (a == 255)
                return static_cast<std::uint8_t>(c);
            return static_cast<std::uint8_t>(std::min<std::uint32_t>(255u, (c * 255u + a / 2) / a));
        };
        out.rgba[i * 4 + 0] = unassociate(TIFFGetR(px));
        out.rgba[i * 4 + 1] = unassociate(TIFFGetG(px));
        out.rgba[i * 4 + 2] = unassociate(TIFFGetB(px));
        out.rgba[i * 4 + 3] = static_cast<std::uint8_t>(a);
    }
    return out;
}

}  // namespace detail

bool tiffSupported() noexcept { return true; }

bool tiffCompressionAvailable(TiffSaveOptions::Compression compression) noexcept {
    return TIFFIsCODECConfigured(compressionTag(compression)) != 0;
}

std::optional<std::vector<std::uint8_t>> encodeTiff(const common::Image& image,
                                                    const TiffSaveOptions& opts,
                                                    std::string* error) {
    silenceLibtiff();
    const auto fail = [&](const char* what) -> std::optional<std::vector<std::uint8_t>> {
        if (error)
            *error = std::string("TIFF: ") + what;
        return std::nullopt;
    };
    if (image.empty())
        return fail("cannot write an empty image");
    if (image.rgba.size() < image.pixelCount() * 4)
        return fail("image buffer is smaller than its dimensions");
    if (!tiffCompressionAvailable(opts.compression))
        return fail("this build of libtiff was not compiled with the requested compression");

    MemoryFile file;
    TIFF* tif = openMemory(file, opts.bigTiff ? "w8" : "w");
    if (tif == nullptr)
        return fail("could not open the writer");
    const auto failOpen = [&](const char* what) -> std::optional<std::vector<std::uint8_t>> {
        TIFFClose(tif);
        return fail(what);
    };

    const std::uint16_t extra =
        opts.premultipliedAlpha ? EXTRASAMPLE_ASSOCALPHA : EXTRASAMPLE_UNASSALPHA;
    if (TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, image.width) != 1 ||
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, image.height) != 1 ||
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8) != 1 ||
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 4) != 1 ||
        TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT) != 1 ||
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB) != 1 ||
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) != 1 ||
        TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT) != 1 ||
        TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &extra) != 1 ||
        TIFFSetField(tif, TIFFTAG_COMPRESSION, compressionTag(opts.compression)) != 1)
        return failOpen("could not write the image header");
    if (opts.predictor && acceptsPredictor(opts.compression) &&
        TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL) != 1)
        return failOpen("could not set the predictor");
    if (opts.compression == TiffSaveOptions::Compression::Deflate)
        TIFFSetField(tif, TIFFTAG_ZIPQUALITY, std::clamp(opts.zipLevel, 1, 9));
    if (opts.compression == TiffSaveOptions::Compression::Zstd)
        TIFFSetField(tif, TIFFTAG_ZSTD_LEVEL, std::clamp(opts.zstdLevel, 1, 22));
    TIFFSetField(tif, TIFFTAG_SOFTWARE, "Mosaic");

    // Physical density. 72 dpi is the format's own "no opinion" default, so writing it would only
    // add noise; anything else is a real statement about print size.
    if (opts.metadata.dpi > 0.0 && std::abs(opts.metadata.dpi - 72.0) > 1e-9) {
        const float dpi = static_cast<float>(opts.metadata.dpi);
        TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
        TIFFSetField(tif, TIFFTAG_XRESOLUTION, dpi);
        TIFFSetField(tif, TIFFTAG_YRESOLUTION, dpi);
    }
    if (!opts.metadata.icc.empty())
        TIFFSetField(tif, TIFFTAG_ICCPROFILE,
                     static_cast<std::uint32_t>(opts.metadata.icc.size()),
                     opts.metadata.icc.data());

    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));

    // The row buffer is a COPY, not a view: libtiff's horizontal predictor differences the
    // caller's buffer in place, which would corrupt the source image.
    std::vector<std::uint8_t> row(static_cast<std::size_t>(image.width) * 4);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        std::memcpy(row.data(), image.rgba.data() + static_cast<std::size_t>(y) * image.width * 4,
                    row.size());
        // ASSOCALPHA is a claim about the PIXELS, not a label: the samples must actually be
        // premultiplied, or every reader that honours the tag will over-brighten the edges.
        if (opts.premultipliedAlpha)
            for (std::uint32_t x = 0; x < image.width; ++x) {
                std::uint8_t* px = row.data() + static_cast<std::size_t>(x) * 4;
                const std::uint32_t a = px[3];
                if (a == 255)
                    continue;
                for (int c = 0; c < 3; ++c)
                    px[c] = static_cast<std::uint8_t>((std::uint32_t{px[c]} * a + 127u) / 255u);
            }
        if (TIFFWriteScanline(tif, row.data(), y, 0) < 0)
            return failOpen("could not write the image data");
    }
    TIFFClose(tif);  // flushes the directory into the memory stream
    return std::move(file.data);
}

#else  // MOSAIC_HAVE_TIFF

namespace detail {

std::optional<common::Image> decodeTiff(const std::vector<std::uint8_t>&, std::string* error) {
    if (error)
        *error = "TIFF: support was not compiled in";
    return std::nullopt;
}

}  // namespace detail

bool tiffSupported() noexcept { return false; }

bool tiffCompressionAvailable(TiffSaveOptions::Compression) noexcept { return false; }

std::optional<std::vector<std::uint8_t>> encodeTiff(const common::Image&, const TiffSaveOptions&,
                                                    std::string* error) {
    if (error)
        *error = "TIFF: support was not compiled in";
    return std::nullopt;
}

#endif  // MOSAIC_HAVE_TIFF

bool saveTiff(const common::Image& image, const std::string& path, const TiffSaveOptions& opts,
              std::string* error) {
    std::optional<std::vector<std::uint8_t>> bytes = encodeTiff(image, opts, error);
    if (!bytes)
        return false;

    const auto fail = [&](const char* what) {
        if (error)
            *error = std::string("TIFF: ") + what;
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
