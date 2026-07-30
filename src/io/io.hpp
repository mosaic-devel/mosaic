#pragma once

#include "common/exif.hpp"
#include "common/image.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// io -- image format readers/writers (the real format registry + loss-warning system arrive at
// S41/S42). S18-b lands a deliberate thin slice: decode PNG + JPEG so the editor is dogfooded on
// real images; the first ENCODER (savePng, below) backs Milestone 1's Quick Export -> PNG. Lives
// under src/io (pure, FLTK-free) so S41/S42 grow it into the real FormatBackend registry.
namespace mosaic::io {

std::string_view moduleName() noexcept;

// The image formats loadImage() understands (sniffed from the file's magic bytes, not the
// extension). Unknown = not a format we decode. WebP/AVIF/TIFF/GIF joined at M4 and are
// BUILD-OPTIONAL: the id is always declared, but a build without the library decodes nothing for
// it (the sniff still names it, so the error can say which format was refused rather than
// "unrecognised").
enum class ImageFormat { Unknown, Png, Jpeg, WebP, Avif, Tiff, Gif };

// Detect the format of an in-memory file by its leading magic bytes (PNG signature, JPEG SOI,
// RIFF/WEBP, ISO-BMFF `ftyp` with an AVIF brand, TIFF byte-order mark, GIF87a/89a).
[[nodiscard]] ImageFormat sniffImageFormat(const std::uint8_t* data, std::size_t size) noexcept;

// The English name of a sniffed format ("PNG", "JPEG", ... , "unknown"), for error text.
[[nodiscard]] std::string_view imageFormatName(ImageFormat format) noexcept;

// Decode an image file at `path` into an 8-bit straight-alpha RGBA image (alpha is preserved for
// the formats that carry it; JPEG has none, so alpha is 255). The format is sniffed from the
// bytes. Returns std::nullopt and, when `error` is non-null, a human-readable reason on any
// failure (unreadable file, unknown format, corrupt data, a format whose library was not compiled
// in, or an implausibly large image). Pure decode -- no FLTK.
// EXIF Orientation is honoured: a sideways-shot photo arrives upright, its rotation baked into
// the pixels (the one correct behavior, so there is no toggle).
[[nodiscard]] std::optional<common::Image> loadImage(const std::string& path,
                                                     std::string* error = nullptr);

// loadImage's pixels plus the optional camera metadata the photo carried (the EXIF READ slice,
// io/exif.hpp). `exif` is best-effort by design: absent or malformed metadata yields nullopt
// and never fails the pixel decode. Orientation is already baked into `image` by the load, so
// `exif->orientation` (when present) reads 1 -- consumers must never re-apply it.
struct LoadedImage {
    common::Image image;
    std::optional<common::ExifData> exif;
};

// loadImage, keeping the metadata: the same decode + orientation bake, returning what File->Open
// needs to stamp onto the created layer (core::Layer::setExif). Existing loadImage callers that
// have no use for metadata are unchanged -- loadImage itself is this with `exif` dropped.
[[nodiscard]] std::optional<LoadedImage> loadImageWithMetadata(const std::string& path,
                                                               std::string* error = nullptr);

// Decode an in-memory image (the format is sniffed from the bytes) into 8-bit straight-alpha
// RGBA. The file half of this is loadImage; this is the buffer half, and its first consumer is the
// Export modal, which decodes the bytes it JUST encoded so the preview shows what the file will
// actually look like -- a JPEG's own artefacts included (plan §5: the encode stage is the source of
// both the preview and the exact size). nullopt for a format this build cannot decode, so a caller
// that also encodes other formats simply falls back to the un-encoded picture. No EXIF orientation
// is applied: an export re-encodes already-upright pixels, and re-applying one would be a bug.
[[nodiscard]] std::optional<common::Image> decodeImageBytes(const std::uint8_t* data,
                                                            std::size_t size,
                                                            std::string* error = nullptr);

// The pixel dimensions of the image at `path`, from the header alone -- no pixel decode.
// The New Document dialog's recents cards read a file's size without paying for a full decode
// of every recent photo at dialog-open time. EXIF 90-degree orientations are honoured where the
// metadata is found near the head, so the answer matches what loadImage would return. nullopt
// for anything unreadable or truncated. Covers PNG, JPEG, GIF and WebP; TIFF and AVIF answer
// nullopt (their dimensions live behind an IFD walk / a box tree, which is a decode in all but
// name -- and a card without a size label is a far smaller cost than a second hostile parser).
struct ImageDimensions {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
[[nodiscard]] std::optional<ImageDimensions> probeImageDimensions(const std::string& path);

// ---------------------------------------------------------------------------------------------
// Side-car metadata (M4)
// ---------------------------------------------------------------------------------------------

// The bytes an encoder embeds beside the pixels when the container carries them. Deliberately raw
// blobs rather than typed records, because every container wants the same two: the EXIF payload is
// the TIFF header + IFDs that io::parseExif reads back (io/exif_write.hpp builds one from a
// common::ExifData), and the ICC profile is a complete .icc file. Empty means "write nothing",
// which is exactly what the Export modal's strip-metadata toggle produces -- so the privacy switch
// needs no per-backend code.
struct EmbeddedMetadata {
    std::vector<std::uint8_t> exif;
    std::vector<std::uint8_t> icc;
    // Physical print density. Written only by the formats that record one (PNG pHYs, TIFF
    // XRESOLUTION/YRESOLUTION); 72 is the "no opinion" value every such writer skips.
    double dpi = 72.0;

    [[nodiscard]] bool empty() const noexcept { return exif.empty() && icc.empty(); }
};

// Read a complete ICC profile file into an EmbeddedMetadata::icc payload. Returns an empty vector
// for an empty path, an unreadable file, or bytes that are not a plausible ICC profile (the
// 128-byte header's declared size must match, and the 'acsp' signature must be present) -- an
// export must never staple an arbitrary file into an image and call it a colour profile.
[[nodiscard]] std::vector<std::uint8_t> readIccProfile(const std::string& path);

// Encoder options for savePng. The full PNG option surface (bit depth, palette, filter strategy,
// text chunks) is the S41/S42 FormatBackend's job (Appendix A); Milestone 1 exposed the two knobs
// Quick Export can carry without a modal, and M4 added the metadata chunks (eXIf/iCCP/pHYs).
struct PngSaveOptions {
    // zlib deflate level, 0 (store) .. 9 (max). libpng's own default is 6; clamped on use.
    int compression = 6;
    // Adam7 interlacing. Off by default (progressive PNGs are larger and rarely wanted for export).
    bool interlace = false;
    // eXIf / iCCP / pHYs. Empty payloads write no chunk at all, so every existing caller (the
    // .mbp preset container, the thumbnailer, the icon-pack cache) keeps producing the same bytes.
    EmbeddedMetadata metadata;
};

// Encode `image` (8-bit straight-alpha RGBA, the compositor's flatten) as a PNG at `path`. Writes
// a colour-type-6 (RGBA) 8-bit PNG; the alpha channel is preserved as straight alpha. Returns false
// and, when `error` is non-null, a human-readable reason on any failure (unwritable path, empty
// image, libpng error). Pure encode -- no FLTK. The inverse of loadImage for PNG (lossless, so an
// encode->decode round-trip is bit-exact).
[[nodiscard]] bool savePng(const common::Image& image, const std::string& path,
                           const PngSaveOptions& opts = {}, std::string* error = nullptr);

// savePng into memory instead of a file: the same encode, byte-identical output. Serves the
// consumers that splice or embed the PNG rather than shipping it (the .mbp preset container).
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encodePng(
    const common::Image& image, const PngSaveOptions& opts = {}, std::string* error = nullptr);

// Encoder options for saveJpeg. The full JPEG option surface (arithmetic coding, restart interval,
// DCT method, mozjpeg trellis) is the S41/S42 FormatBackend's job (Appendix A); this exposes the
// knobs Quick Export + the Export modal can reasonably carry.
struct JpegSaveOptions {
    // Encode quality, 0 (smallest/worst) .. 100 (largest/best). Clamped on use.
    int quality = 90;
    // Chroma subsampling. 4:2:0 halves chroma both ways (smallest, the photographic default);
    // 4:2:2 halves it horizontally; 4:4:4 keeps full chroma (best for text / hard edges).
    enum class Subsampling { S420, S422, S444 };
    Subsampling subsampling = Subsampling::S420;
    // Progressive (multi-scan) JPEG: smaller + renders coarse-to-fine, at some encode cost.
    bool progressive = false;
    // JPEG carries no alpha. Transparent pixels of the straight-alpha source are composited over
    // this opaque matte before encoding (the plan's "Matte" row); irrelevant for an opaque flatten.
    common::Color8 matte{255, 255, 255, 255};
    // EXIF (APP1), the ICC profile (APP2, split across as many segments as it needs) and the JFIF
    // density. Empty payloads write no segment at all, so every existing caller keeps producing
    // byte-identical output.
    EmbeddedMetadata metadata;
};

// Encode `image` (8-bit straight-alpha RGBA, the compositor's flatten) as a baseline/progressive
// JPEG at `path` via libjpeg-turbo. Alpha is flattened onto `opts.matte` (JPEG has none). Returns
// false and, when `error` is non-null, a human-readable reason on any failure. Pure encode -- no
// FLTK. Lossy: an encode->decode round-trip is only visually close, not bit-exact.
[[nodiscard]] bool saveJpeg(const common::Image& image, const std::string& path,
                            const JpegSaveOptions& opts = {}, std::string* error = nullptr);

// saveJpeg into memory (the same encode, byte-identical output): backs the Export modal's
// trial-encode preview + exact-size readout. nullopt on failure.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encodeJpeg(
    const common::Image& image, const JpegSaveOptions& opts = {}, std::string* error = nullptr);

// True iff JPEG XL support was compiled in (system libjxl found at build time). Always declared so
// callers need not know the build flag: when false, saveJxl/encodeJxl fail with an explanatory
// error and the UI hides the .jxl export paths. (jxl.cpp defines both the real path and, under the
// same header, inert stubs -- see src/io/CMakeLists.txt for the MOSAIC_HAVE_JXL gate.)
[[nodiscard]] bool jxlSupported() noexcept;

// Encoder options for saveJxl. The full JXL surface (progressive, modular/VarDCT, box metadata,
// lossless JPEG transcode, float/HDR) is the S41/S42 FormatBackend + HDR tier; this exposes the
// knobs Quick Export + the Export modal can carry over the 8-bit flatten (Appendix A / C).
struct JxlSaveOptions {
    // Visually-lossless "butteraugli distance": 0 = mathematically lossless (also set `lossless`),
    // 1.0 = the libjxl default (visually lossless), up to 25 (very lossy). Ignored when `lossless`.
    float distance = 1.0f;
    // Encoder effort/speed, 1 (fast) .. 9 (slow, best). 10+ needs expert options (not enabled).
    int effort = 7;
    // Mathematically lossless: forces distance 0 + uses_original_profile, so an encode->decode
    // round-trip is bit-exact.
    bool lossless = false;
    // The Exif box and the original-colour-profile tag. An ICC profile makes the file a CONTAINER
    // (the boxed form) rather than a bare codestream, which is also what the Exif box requires; a
    // metadata-free encode still writes the bare codestream every decoder reads fastest.
    EmbeddedMetadata metadata;
};

// Encode `image` (8-bit straight-alpha RGBA) as a JPEG XL file at `path` via libjxl. Returns false
// and, when `error` is non-null, a reason on failure -- including when JXL was not compiled in
// (jxlSupported() == false). Pure encode -- no FLTK. Alpha is preserved (straight).
[[nodiscard]] bool saveJxl(const common::Image& image, const std::string& path,
                           const JxlSaveOptions& opts = {}, std::string* error = nullptr);

// saveJxl into memory (the same encode, byte-identical output): backs the Export modal's
// trial-encode preview + exact-size readout. nullopt on failure (or when JXL was not compiled in).
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encodeJxl(
    const common::Image& image, const JxlSaveOptions& opts = {}, std::string* error = nullptr);

// ---------------------------------------------------------------------------------------------
// M4 -- WebP (libwebp + libwebpmux)
// ---------------------------------------------------------------------------------------------
// Every one of the four codecs below follows the libjxl pattern exactly: the whole implementation
// is compile-time gated, the symbols always exist, and a runtime `*Supported()` probe tells the
// combobox whether to offer the format. A distro without libwebp still builds Mosaic; it just has
// no .webp.

[[nodiscard]] bool webpSupported() noexcept;

// Encoder options for saveWebp. Appendix A's long tail (target size/PSNR, segments, sns, passes,
// preset, animation) is deliberately absent: a schema may only describe knobs encode() reads.
struct WebpSaveOptions {
    // Lossy: visual quality 0 (smallest) .. 100 (best). Lossless: how hard the entropy coder
    // works, same range -- libwebp overloads the field, and so does the schema's help text.
    int quality = 80;
    bool lossless = false;
    // Lossless only: 0 = maximum preprocessing loss, 100 = off (a true bit-exact encode).
    // Anything below 100 makes the encode LOSSY, which encodeIsLossless() has to be told about.
    int nearLossless = 100;
    int method = 4;         // 0 (fast) .. 6 (slow, smallest)
    int alphaQuality = 100; // 0 .. 100; 100 keeps the alpha plane lossless
    // Keep the RGB values under fully transparent pixels instead of letting the encoder pick
    // whatever compresses best. Required for a bit-exact round-trip of an image with holes.
    bool exact = false;
    EmbeddedMetadata metadata;  // EXIF / ICCP chunks, muxed in after the bitstream
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encodeWebp(
    const common::Image& image, const WebpSaveOptions& opts = {}, std::string* error = nullptr);
[[nodiscard]] bool saveWebp(const common::Image& image, const std::string& path,
                            const WebpSaveOptions& opts = {}, std::string* error = nullptr);

// ---------------------------------------------------------------------------------------------
// M4 -- AVIF (libavif over libaom / SVT-AV1)
// ---------------------------------------------------------------------------------------------
// ENCODER CHOICE (invariant, non-negotiable): the AV1 encoder is chosen EXPLICITLY and is never
// rav1e. avifSupported() answers false unless libavif reports an AOM or SVT encoder, and the
// encoder object always carries an explicit codecChoice -- AVIF_CODEC_CHOICE_AUTO could resolve
// to whatever the distro's libavif was built against. See the note in io/avif.cpp.
[[nodiscard]] bool avifSupported() noexcept;

struct AvifSaveOptions {
    int quality = 60;       // 0 (worst) .. 100 (best)
    int alphaQuality = 100; // 0 .. 100
    int speed = 6;          // 0 (slowest, best) .. 10 (fastest)
    // Mathematically lossless. Forces quality/alphaQuality 100, YUV 4:4:4 and the identity
    // matrix coefficients (i.e. GBR, no colour transform): quality 100 ALONE is not lossless,
    // because the RGB->YUV conversion rounds.
    bool lossless = false;
    // Chroma sampling. Yuv400 is monochrome -- it discards colour outright rather than merely
    // reducing its resolution, and the loss vocabulary has no honest way to say that inside a
    // "subsampling ratio" dropdown, so the BACKEND does not offer it; it stays here for a future
    // dedicated grayscale option. A direct caller defaults to full colour.
    enum class Yuv { Yuv444, Yuv422, Yuv420, Yuv400 };
    Yuv yuv = Yuv::Yuv444;
    EmbeddedMetadata metadata;  // ICC profile + EXIF item
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encodeAvif(
    const common::Image& image, const AvifSaveOptions& opts = {}, std::string* error = nullptr);
[[nodiscard]] bool saveAvif(const common::Image& image, const std::string& path,
                            const AvifSaveOptions& opts = {}, std::string* error = nullptr);

// ---------------------------------------------------------------------------------------------
// M4 -- TIFF (libtiff)
// ---------------------------------------------------------------------------------------------
[[nodiscard]] bool tiffSupported() noexcept;

struct TiffSaveOptions {
    // Every choice here is LOSSLESS, which is why the TIFF caps row can say so flatly. Appendix
    // A's JPEG-in-TIFF is deliberately absent: it is the one compression that would make the
    // format lossy AND drop the alpha channel, and FormatCaps has no way to say "alpha survives,
    // except under this one option" -- so offering it would make the loss banner lie. It waits
    // for a caps model that can express a per-option capability.
    enum class Compression { None, Lzw, Deflate, PackBits, Zstd };
    Compression compression = Compression::Deflate;
    // Horizontal differencing. Helps LZW/Deflate/Zstd on photographic data and is harmless
    // otherwise; ignored by the codecs that do not accept a predictor.
    bool predictor = true;
    int zipLevel = 6;   // Compression::Deflate only, 1..9
    int zstdLevel = 9;  // Compression::Zstd only, 1..22
    // BigTIFF (64-bit offsets). Needed past 4 GiB; a few readers still refuse it, so it is off.
    bool bigTiff = false;
    // How the alpha channel is TAGGED. Our pixels are straight (unassociated) alpha, so the
    // honest tag is EXTRASAMPLE_UNASSALPHA; the associated spelling exists because some print
    // pipelines demand it, and it PREMULTIPLIES the pixels rather than merely relabelling them.
    bool premultipliedAlpha = false;
    // ICC profile + the resolution tags. EXIF is NOT written: a TIFF carries it as a private
    // sub-directory (TIFFCreateEXIFDirectory / TIFFTAG_EXIFIFD), which is a second write pass and
    // an offset back-patch -- so the TIFF caps row honestly reports no EXIF rather than
    // pretending, and the loss banner warns.
    EmbeddedMetadata metadata;
};

// True iff this libtiff was built with the codec behind `compression` (TIFFIsCODECConfigured).
// Codec support is a per-BUILD fact, not a per-library one -- JPEG, ZSTD and LZMA are all
// optional inside libtiff -- so the backend's schema filters its choice list through this rather
// than offering a compression that would fail at write time.
[[nodiscard]] bool tiffCompressionAvailable(TiffSaveOptions::Compression compression) noexcept;

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encodeTiff(
    const common::Image& image, const TiffSaveOptions& opts = {}, std::string* error = nullptr);
[[nodiscard]] bool saveTiff(const common::Image& image, const std::string& path,
                            const TiffSaveOptions& opts = {}, std::string* error = nullptr);

// ---------------------------------------------------------------------------------------------
// M4 -- GIF (giflib + our own quantizer, io/quantize.hpp)
// ---------------------------------------------------------------------------------------------
// giflib is MIT-licensed, and it supplies only the LZW layer -- the palette and the dithering are
// ours (io/quantize.hpp), which is also why they are unit-testable without a GIF in sight.
[[nodiscard]] bool gifSupported() noexcept;

struct GifSaveOptions {
    // Colours in the global palette, 2..256. When the image needs transparency one slot is spent
    // on it, so the picture itself gets paletteSize-1 colours -- stated here because the loss
    // banner quotes the number.
    int paletteSize = 256;
    bool dither = true;      // Floyd-Steinberg error diffusion
    bool interlace = false;  // the four-pass row order
    // GIF alpha is one bit. Pixels below the threshold become the transparent index; the rest are
    // composited over `matte`, so a soft edge lands on the matte instead of on whatever the
    // palette happened to leave behind it.
    int alphaThreshold = 128;
    common::Color8 matte{255, 255, 255, 255};
    std::string comment;  // GIF89a comment extension; "" writes none
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encodeGif(
    const common::Image& image, const GifSaveOptions& opts = {}, std::string* error = nullptr);
[[nodiscard]] bool saveGif(const common::Image& image, const std::string& path,
                           const GifSaveOptions& opts = {}, std::string* error = nullptr);

} // namespace mosaic::io
