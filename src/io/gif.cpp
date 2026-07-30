#include "common/fs_path.hpp"
#include "io/detail.hpp"
#include "io/io.hpp"
#include "io/quantize.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

// gif -- the GIF codec (M4), wrapping the system giflib. Compile-time gated on MOSAIC_HAVE_GIF,
// same shape as the other three M4 codecs.
//
// giflib gives us the LZW layer and the container; everything that decides what the picture LOOKS
// like -- the palette, the dithering, the one-bit alpha -- is ours, in io/quantize.hpp. That split
// is deliberate: the quantizer is the part with the interesting failure modes, and it is far
// easier to test as a pure function than through an encoder.
//
// giflib itself is MIT-licensed.
#ifdef MOSAIC_HAVE_GIF
#include <gif_lib.h>
#endif

namespace mosaic::io {

#ifdef MOSAIC_HAVE_GIF

namespace {

// GIF's logical screen fields are 16-bit.
constexpr std::uint32_t kGifMaxDim = 65535;

struct GifReader {
    const std::vector<std::uint8_t>* data = nullptr;
    std::size_t pos = 0;
};

int gifRead(GifFileType* gif, GifByteType* buffer, int length) {
    auto* reader = static_cast<GifReader*>(gif->UserData);
    if (reader == nullptr || reader->data == nullptr || length <= 0)
        return 0;
    const std::size_t available =
        reader->pos < reader->data->size() ? reader->data->size() - reader->pos : 0;
    const std::size_t got = std::min(static_cast<std::size_t>(length), available);
    if (got != 0)
        std::memcpy(buffer, reader->data->data() + reader->pos, got);
    reader->pos += got;
    return static_cast<int>(got);
}

int gifWrite(GifFileType* gif, const GifByteType* buffer, int length) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(gif->UserData);
    if (out == nullptr || length <= 0)
        return 0;
    out->insert(out->end(), buffer, buffer + length);
    return length;
}

// giflib's GifMakeMapObject refuses anything that is not a power of two (and not >256), so the
// palette is padded up. The pad entries are never referenced by any index.
[[nodiscard]] int roundUpPaletteSize(std::size_t used) noexcept {
    int size = 2;
    while (static_cast<std::size_t>(size) < used && size < 256)
        size *= 2;
    return size;
}

// GIF's four-pass interlace: row offsets and strides, in order.
constexpr int kInterlaceOffset[4] = {0, 4, 2, 1};
constexpr int kInterlaceJump[4] = {8, 8, 4, 2};

}  // namespace

namespace detail {

std::optional<common::Image> decodeGif(const std::vector<std::uint8_t>& buf, std::string* error) {
    const auto fail = [&](const char* what) -> std::optional<common::Image> {
        if (error)
            *error = std::string("GIF: ") + what;
        return std::nullopt;
    };
    if (buf.empty())
        return fail("empty file");

    GifReader reader{&buf, 0};
    int code = 0;
    GifFileType* gif = DGifOpen(&reader, gifRead, &code);
    if (gif == nullptr)
        return fail("not a readable GIF file");
    const auto cleanup = [&](const char* what) -> std::optional<common::Image> {
        int closeCode = 0;
        DGifCloseFile(gif, &closeCode);
        return fail(what);
    };

    if (DGifSlurp(gif) == GIF_ERROR)
        return cleanup("the image data could not be decoded");
    if (gif->ImageCount < 1 || gif->SavedImages == nullptr)
        return cleanup("the file contains no image");
    const std::uint32_t canvasW = static_cast<std::uint32_t>(std::max(gif->SWidth, 0));
    const std::uint32_t canvasH = static_cast<std::uint32_t>(std::max(gif->SHeight, 0));
    if (!dimensionsPlausible(canvasW, canvasH))
        return cleanup("unsupported image dimensions");

    const SavedImage& frame = gif->SavedImages[0];
    const ColorMapObject* map = frame.ImageDesc.ColorMap != nullptr ? frame.ImageDesc.ColorMap
                                                                    : gif->SColorMap;
    if (map == nullptr || map->Colors == nullptr || map->ColorCount <= 0)
        return cleanup("the file has no colour table");
    if (frame.RasterBits == nullptr)
        return cleanup("the first frame has no pixels");

    int transparent = NO_TRANSPARENT_COLOR;
    GraphicsControlBlock gcb;
    if (DGifSavedExtensionToGCB(gif, 0, &gcb) == GIF_OK)
        transparent = gcb.TransparentColor;

    // The canvas starts fully transparent and the first frame is drawn into it at its own offset:
    // a GIF frame is allowed to be smaller than the logical screen, and inventing a background
    // colour for the rest would be a guess.
    common::Image out(canvasW, canvasH);
    const long left = frame.ImageDesc.Left;
    const long top = frame.ImageDesc.Top;
    const long frameW = frame.ImageDesc.Width;
    const long frameH = frame.ImageDesc.Height;
    if (frameW <= 0 || frameH <= 0)
        return cleanup("the first frame is empty");

    for (long y = 0; y < frameH; ++y) {
        const long canvasY = top + y;
        if (canvasY < 0 || canvasY >= static_cast<long>(canvasH))
            continue;
        for (long x = 0; x < frameW; ++x) {
            const long canvasX = left + x;
            if (canvasX < 0 || canvasX >= static_cast<long>(canvasW))
                continue;
            const int index = frame.RasterBits[static_cast<std::size_t>(y) *
                                                   static_cast<std::size_t>(frameW) +
                                               static_cast<std::size_t>(x)];
            std::uint8_t* dst = out.rgba.data() +
                                (static_cast<std::size_t>(canvasY) * canvasW +
                                 static_cast<std::size_t>(canvasX)) * 4;
            if (index == transparent || index >= map->ColorCount)
                continue;  // transparent, or an index the table does not define: leave it clear
            const GifColorType& c = map->Colors[index];
            dst[0] = c.Red;
            dst[1] = c.Green;
            dst[2] = c.Blue;
            dst[3] = 255;
        }
    }

    int closeCode = 0;
    DGifCloseFile(gif, &closeCode);
    return out;
}

}  // namespace detail

bool gifSupported() noexcept { return true; }

std::optional<std::vector<std::uint8_t>> encodeGif(const common::Image& image,
                                                   const GifSaveOptions& opts,
                                                   std::string* error) {
    const auto fail = [&](const char* what) -> std::optional<std::vector<std::uint8_t>> {
        if (error)
            *error = std::string("GIF: ") + what;
        return std::nullopt;
    };
    if (image.empty())
        return fail("cannot write an empty image");
    if (image.rgba.size() < image.pixelCount() * 4)
        return fail("image buffer is smaller than its dimensions");
    if (image.width > kGifMaxDim || image.height > kGifMaxDim)
        return fail("image is too large for the GIF format (65535 px maximum per side)");

    QuantizeOptions quantizeOptions;
    quantizeOptions.maxColors = std::clamp(opts.paletteSize, 2, 256);
    quantizeOptions.dither = opts.dither;
    quantizeOptions.alphaThreshold = std::clamp(opts.alphaThreshold, 0, 256);
    quantizeOptions.matte = opts.matte;
    QuantizedImage indexed = quantize(image, quantizeOptions);
    if (indexed.palette.empty() || indexed.indices.size() != image.pixelCount())
        return fail("the palette could not be built");

    const int mapSize = roundUpPaletteSize(indexed.palette.size());
    ColorMapObject* map = GifMakeMapObject(mapSize, nullptr);
    if (map == nullptr)
        return fail("could not allocate the colour table");
    for (int i = 0; i < mapSize; ++i) {
        const bool defined = static_cast<std::size_t>(i) < indexed.palette.size();
        const common::Color8 c =
            defined ? indexed.palette[static_cast<std::size_t>(i)] : common::Color8{0, 0, 0, 255};
        map->Colors[i].Red = c.r;
        map->Colors[i].Green = c.g;
        map->Colors[i].Blue = c.b;
    }

    std::vector<std::uint8_t> out;
    int code = 0;
    GifFileType* gif = EGifOpen(&out, gifWrite, &code);
    if (gif == nullptr) {
        GifFreeMapObject(map);
        return fail("could not open the writer");
    }
    const auto cleanup = [&](const char* what) -> std::optional<std::vector<std::uint8_t>> {
        int closeCode = 0;
        EGifCloseFile(gif, &closeCode);  // frees `gif`, and the map it was handed
        return fail(what);
    };

    // GIF89a: transparency and the comment extension are 89a features, and a writer that claims
    // 87a while using them produces a file some decoders reject outright.
    EGifSetGifVersion(gif, /*gif89=*/true);
    if (EGifPutScreenDesc(gif, static_cast<int>(image.width), static_cast<int>(image.height),
                          map->BitsPerPixel, /*GifBackGround=*/0, map) == GIF_ERROR) {
        GifFreeMapObject(map);
        return cleanup("could not write the screen descriptor");
    }
    // EGifPutScreenDesc deep-copies the table into the file object, so ours is done here; from
    // this point the GifFileType owns the only live copy and EGifCloseFile releases it.
    GifFreeMapObject(map);

    if (indexed.transparentIndex >= 0) {
        GraphicsControlBlock gcb;
        gcb.DisposalMode = DISPOSAL_UNSPECIFIED;
        gcb.UserInputFlag = false;
        gcb.DelayTime = 0;
        gcb.TransparentColor = indexed.transparentIndex;
        GifByteType extension[4] = {0, 0, 0, 0};
        const std::size_t length = EGifGCBToExtension(&gcb, extension);
        if (EGifPutExtension(gif, GRAPHICS_EXT_FUNC_CODE, static_cast<int>(length), extension) ==
            GIF_ERROR)
            return cleanup("could not write the transparency block");
    }
    if (!opts.comment.empty()) {
        // A GIF extension sub-block is at most 255 bytes; longer comments are simply cut, which
        // is better than a writer failure over a cosmetic field.
        const std::string comment = opts.comment.substr(0, 255);
        if (EGifPutExtension(gif, COMMENT_EXT_FUNC_CODE, static_cast<int>(comment.size()),
                             comment.data()) == GIF_ERROR)
            return cleanup("could not write the comment block");
    }

    if (EGifPutImageDesc(gif, 0, 0, static_cast<int>(image.width), static_cast<int>(image.height),
                         opts.interlace, nullptr) == GIF_ERROR)
        return cleanup("could not write the image descriptor");

    const int width = static_cast<int>(image.width);
    const auto putRow = [&](std::uint32_t y) {
        GifPixelType* row = indexed.indices.data() + static_cast<std::size_t>(y) * image.width;
        return EGifPutLine(gif, row, width) != GIF_ERROR;
    };
    if (opts.interlace) {
        // giflib sets the interlace FLAG but never reorders the rows -- the caller supplies them
        // in pass order, and a writer that forgets produces a scrambled image.
        for (int pass = 0; pass < 4; ++pass)
            for (std::uint32_t y = static_cast<std::uint32_t>(kInterlaceOffset[pass]);
                 y < image.height; y += static_cast<std::uint32_t>(kInterlaceJump[pass]))
                if (!putRow(y))
                    return cleanup("could not write the image data");
    } else {
        for (std::uint32_t y = 0; y < image.height; ++y)
            if (!putRow(y))
                return cleanup("could not write the image data");
    }

    int closeCode = 0;
    if (EGifCloseFile(gif, &closeCode) == GIF_ERROR)
        return fail("the file could not be finished");
    return out;
}

#else  // MOSAIC_HAVE_GIF

namespace detail {

std::optional<common::Image> decodeGif(const std::vector<std::uint8_t>&, std::string* error) {
    if (error)
        *error = "GIF: support was not compiled in";
    return std::nullopt;
}

}  // namespace detail

bool gifSupported() noexcept { return false; }

std::optional<std::vector<std::uint8_t>> encodeGif(const common::Image&, const GifSaveOptions&,
                                                   std::string* error) {
    if (error)
        *error = "GIF: support was not compiled in";
    return std::nullopt;
}

#endif  // MOSAIC_HAVE_GIF

bool saveGif(const common::Image& image, const std::string& path, const GifSaveOptions& opts,
             std::string* error) {
    std::optional<std::vector<std::uint8_t>> bytes = encodeGif(image, opts, error);
    if (!bytes)
        return false;

    const auto fail = [&](const char* what) {
        if (error)
            *error = std::string("GIF: ") + what;
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
