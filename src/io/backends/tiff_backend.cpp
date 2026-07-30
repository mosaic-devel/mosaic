#include "io/backends/backends.hpp"
#include "io/io.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The TIFF FormatBackend (M4) -- an adapter over io::encodeTiff (io/tiff.cpp, libtiff).
//
// The one thing this backend does that no other does: its option schema is built against a
// RUNTIME probe. libtiff's codecs are individually optional at ITS build time, so the compression
// dropdown lists only what TIFFIsCODECConfigured() confirms -- offering a choice that would fail
// at write time is exactly the sort of dead control the schema mechanism exists to prevent.
namespace mosaic::io {

namespace {

using Compression = TiffSaveOptions::Compression;

struct CompressionChoice {
    Compression value;
    const char* id;
    const char* label;
    const char* help;
};

// Declaration order is the dropdown order: cheapest-to-decode first, then the two that need a
// libtiff built with the extra library.
constexpr CompressionChoice kCompressions[] = {
    {Compression::None, "none", "None (uncompressed)",
     "The largest file, and the fastest to open anywhere."},
    {Compression::Lzw, "lzw", "LZW",
     "The classic TIFF compression -- readable by essentially everything."},
    {Compression::Deflate, "deflate", "Deflate (ZIP)",
     "Smaller than LZW on photographs, and just as lossless."},
    {Compression::PackBits, "packbits", "PackBits (RLE)",
     "Simple run-length coding: weak on photographs, good on flat artwork."},
    {Compression::Zstd, "zstd", "Zstandard",
     "The smallest and fastest of these, but a newer tag that older readers may not know."},
};

[[nodiscard]] Compression compressionFromId(std::string_view id) noexcept {
    for (const CompressionChoice& choice : kCompressions)
        if (id == choice.id)
            return choice.value;
    return Compression::Deflate;
}

// The default compression, chosen from what this libtiff actually has: Deflate if possible, then
// LZW, then uncompressed -- which is always configured, so the schema always has a valid default.
[[nodiscard]] std::string defaultCompressionId() {
    for (const Compression candidate : {Compression::Deflate, Compression::Lzw, Compression::None})
        if (tiffCompressionAvailable(candidate))
            for (const CompressionChoice& choice : kCompressions)
                if (choice.value == candidate)
                    return choice.id;
    return "none";
}

class TiffBackend final : public FormatBackend {
public:
    [[nodiscard]] FormatId id() const noexcept override { return FormatId::Tiff; }
    [[nodiscard]] FormatTier tier() const noexcept override { return FormatTier::Common; }
    [[nodiscard]] std::string_view displayName() const noexcept override { return "TIFF image"; }
    [[nodiscard]] std::vector<std::string> extensions() const override { return {"tif", "tiff"}; }
    [[nodiscard]] std::string_view mimeType() const noexcept override { return "image/tiff"; }
    [[nodiscard]] bool available() const noexcept override { return tiffSupported(); }

    // What THIS ENCODER writes: one 8-bit RGBA page, always losslessly. TIFF's container is far
    // wider -- CMYK, Lab, 16/32-bit and float samples, multi-page, JPEG-compressed strips -- and
    // every one of those reads false here until the encoder actually writes it. `layers` is false
    // for the same reason: multi-page TIFF is real, our writer emits a single directory.
    [[nodiscard]] FormatCaps caps() const override {
        FormatCaps c;
        c.channels = ChannelModel::Rgba;
        c.maxBitDepth = 8;
        c.floatPixels = false;
        // The file RECORDS which convention it holds (EXTRASAMPLES), and the option picks.
        c.alpha = AlphaKind::Either;
        c.icc = true;
        c.metadata = MetadataKind::Dpi;  // EXIF needs a sub-directory pass; see io/io.hpp
        c.lossless = true;
        c.lossy = false;
        c.animation = false;
        c.maxColors = -1;
        c.layers = false;
        c.chromaSubsampling = false;
        c.vector = false;
        return c;
    }

    [[nodiscard]] OptionsSchema optionsSchema() const override {
        OptionsSchema s;
        s.groups.push_back(OptionGroup{"advanced", "Advanced", /*collapsedByDefault=*/true});

        OptionDesc compression;
        compression.key = "compression";
        compression.label = "Compression";
        compression.help = "Every choice here is lossless -- only the file size, the encode time "
                           "and how many other programs can open the result change.";
        compression.type = OptionType::Enum;
        compression.widget = OptionWidget::Dropdown;
        compression.defaultValue = textValue(defaultCompressionId());
        std::vector<std::string> predictorSources;
        for (const CompressionChoice& choice : kCompressions) {
            if (!tiffCompressionAvailable(choice.value))
                continue;  // this libtiff cannot write it: do not offer it
            compression.choices.push_back(EnumChoice{choice.id, choice.label, choice.help});
            if (choice.value == Compression::Lzw || choice.value == Compression::Deflate ||
                choice.value == Compression::Zstd)
                predictorSources.emplace_back(choice.id);
        }
        if (compression.choices.empty())  // cannot happen: COMPRESSION_NONE is always configured
            compression.choices.push_back(
                EnumChoice{"none", "None (uncompressed)", "The largest file."});
        s.options.push_back(std::move(compression));

        if (!predictorSources.empty()) {
            OptionDesc predictor;
            predictor.key = "predictor";
            predictor.label = "Horizontal predictor";
            predictor.help = "Store the difference between neighbouring pixels before "
                             "compressing. Usually smaller on photographs, still lossless.";
            predictor.type = OptionType::Bool;
            predictor.widget = OptionWidget::Checkbox;
            predictor.defaultValue = boolValue(true);
            predictor.visibleWhen.push_back(
                OptionCondition{"compression", predictorSources, /*negate=*/false});
            s.options.push_back(std::move(predictor));
        }

        if (tiffCompressionAvailable(Compression::Deflate)) {
            OptionDesc zip;
            zip.key = "zip-level";
            zip.label = "Deflate level";
            zip.help = "1 is fastest, 9 is smallest. The pixels are identical at every level.";
            zip.type = OptionType::Int;
            zip.widget = OptionWidget::Slider;
            zip.group = "advanced";
            zip.defaultValue = intValue(6);
            zip.min = 1.0;
            zip.max = 9.0;
            zip.step = 1.0;
            zip.visibleWhen.push_back(OptionCondition{"compression", {"deflate"}, false});
            s.options.push_back(std::move(zip));
        }
        if (tiffCompressionAvailable(Compression::Zstd)) {
            OptionDesc zstd;
            zstd.key = "zstd-level";
            zstd.label = "Zstandard level";
            zstd.help = "1 is fastest, 22 is smallest. The pixels are identical at every level.";
            zstd.type = OptionType::Int;
            zstd.widget = OptionWidget::Slider;
            zstd.group = "advanced";
            zstd.defaultValue = intValue(9);
            zstd.min = 1.0;
            zstd.max = 22.0;
            zstd.step = 1.0;
            zstd.visibleWhen.push_back(OptionCondition{"compression", {"zstd"}, false});
            s.options.push_back(std::move(zstd));
        }

        OptionDesc premultiplied;
        premultiplied.key = "premultiplied-alpha";
        premultiplied.label = "Premultiplied (associated) alpha";
        premultiplied.help = "Tag the transparency the way print and video pipelines expect. Most "
                             "image editors want the plain (unassociated) form, which is the "
                             "default.";
        premultiplied.type = OptionType::Bool;
        premultiplied.widget = OptionWidget::Checkbox;
        premultiplied.group = "advanced";
        premultiplied.defaultValue = boolValue(false);
        s.options.push_back(std::move(premultiplied));

        OptionDesc bigTiff;
        bigTiff.key = "bigtiff";
        bigTiff.label = "BigTIFF (64-bit offsets)";
        bigTiff.help = "Required past 4 GB. A few older programs cannot open a BigTIFF at all, so "
                       "it is off unless you need it.";
        bigTiff.type = OptionType::Bool;
        bigTiff.widget = OptionWidget::Checkbox;
        bigTiff.group = "advanced";
        bigTiff.defaultValue = boolValue(false);
        s.options.push_back(std::move(bigTiff));

        return s;
    }

    [[nodiscard]] EncodeResult encode(const RenderInput& input, const OptionValues& values,
                                      const ProgressFn& progress) const override {
        if (!tiffSupported())
            return EncodeResult::failure("TIFF support was not compiled into this build");
        if (input.pixels == nullptr || input.pixels->empty())
            return EncodeResult::failure("TIFF: cannot write an empty image");
        if (progress && !progress(0.0f))
            return EncodeResult::failure("TIFF: cancelled");

        TiffSaveOptions opts;
        opts.compression = compressionFromId(values.text("compression", defaultCompressionId()));
        if (!tiffCompressionAvailable(opts.compression))
            opts.compression = compressionFromId(defaultCompressionId());
        opts.predictor = values.boolean("predictor", true);
        opts.zipLevel = std::clamp(values.integer("zip-level", 6), 1, 9);
        opts.zstdLevel = std::clamp(values.integer("zstd-level", 9), 1, 22);
        opts.premultipliedAlpha = values.boolean("premultiplied-alpha", false);
        opts.bigTiff = values.boolean("bigtiff", false);
        opts.metadata = buildMetadata(input);

        EncodeResult r;
        std::optional<std::vector<std::uint8_t>> bytes = encodeTiff(*input.pixels, opts, &r.error);
        if (!bytes.has_value())
            return r;
        r.bytes = std::move(*bytes);
        r.ok = true;
        if (progress)
            progress(1.0f);
        return r;
    }
};

} // namespace

std::unique_ptr<FormatBackend> makeTiffBackend() { return std::make_unique<TiffBackend>(); }

} // namespace mosaic::io
