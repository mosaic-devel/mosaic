#include "io/backends/backends.hpp"
#include "io/io.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// The first real FormatBackend, and the proof that the registry shape works.
//
// It is an ADAPTER, not a rewrite: io::encodePng (io/png.cpp) stays the libpng encoder and keeps
// its own small, direct API, because half a dozen callers outside the export path use it
// (the .mbp preset container, the XDG thumbnailer, the icon-pack cache, panel screenshots).
// The backend adds exactly what the registry needs on top: the capability row, the options
// schema, and the OptionValues -> PngSaveOptions translation.
namespace mosaic::io {

namespace {

class PngBackend final : public FormatBackend {
public:
    [[nodiscard]] FormatId id() const noexcept override { return FormatId::Png; }
    [[nodiscard]] FormatTier tier() const noexcept override { return FormatTier::Common; }
    [[nodiscard]] std::string_view displayName() const noexcept override { return "PNG image"; }
    [[nodiscard]] std::vector<std::string> extensions() const override { return {"png"}; }
    [[nodiscard]] std::string_view mimeType() const noexcept override { return "image/png"; }

    // What THIS ENCODER writes today, not what the PNG specification permits -- the caps rule
    // in io/caps.hpp. The container also has 16-bit samples, palettes and APNG; io::encodePng
    // writes an 8-bit colour-type-6 RGBA image, so those still read false and the loss banner
    // tells the truth. M4 flipped `icc` and the eXIf/pHYs metadata bits on, because encodePng
    // now really writes those three chunks (io/png.cpp writePngMetadata); the depth fields wait
    // on the >8-bit tap (§5).
    [[nodiscard]] FormatCaps caps() const override {
        FormatCaps c;
        c.channels = ChannelModel::Rgba;
        c.maxBitDepth = 8;
        c.floatPixels = false;
        c.alpha = AlphaKind::Straight;
        c.icc = true;
        c.metadata = MetadataKind::Exif | MetadataKind::Dpi;
        c.lossless = true;
        c.lossy = false;
        c.animation = false;
        c.maxColors = -1;
        c.layers = false;
        c.chromaSubsampling = false;
        c.vector = false;
        return c;
    }

    // Appendix A's full PNG surface (zlib strategy, row filter, bit depth, palette, iCCP/sRGB/
    // gAMA, text chunks, eXIf, pHYs) arrives with the encoder work that honours it. A schema
    // must only ever describe knobs encode() actually reads -- a panel that offers a control
    // the file ignores is worse than no control.
    [[nodiscard]] OptionsSchema optionsSchema() const override {
        OptionsSchema s;
        s.groups.push_back(OptionGroup{"advanced", "Advanced", /*collapsedByDefault=*/true});

        OptionDesc compression;
        compression.key = "compression";
        compression.label = "Compression";
        compression.help = "zlib deflate effort: 0 stores, 9 is smallest and slowest. PNG is "
                           "lossless at every setting -- only the file size and the encode time "
                           "change.";
        compression.type = OptionType::Int;
        compression.widget = OptionWidget::Slider;
        compression.defaultValue = intValue(6);  // libpng's own default
        compression.min = 0.0;
        compression.max = 9.0;
        compression.step = 1.0;
        s.options.push_back(std::move(compression));

        OptionDesc interlace;
        interlace.key = "interlace";
        interlace.label = "Interlaced (Adam7)";
        interlace.help = "Render progressively while loading, at the cost of a noticeably larger "
                         "file. Rarely wanted for an export.";
        interlace.type = OptionType::Bool;
        interlace.widget = OptionWidget::Checkbox;
        interlace.group = "advanced";
        interlace.defaultValue = boolValue(false);
        s.options.push_back(std::move(interlace));

        return s;
    }

    [[nodiscard]] EncodeResult encode(const RenderInput& input, const OptionValues& values,
                                      const ProgressFn& progress) const override {
        if (input.pixels == nullptr || input.pixels->empty())
            return EncodeResult::failure("PNG: cannot write an empty image");
        if (progress && !progress(0.0f))
            return EncodeResult::failure("PNG: cancelled");

        // Tolerate an uncoerced bag: clamp exactly the way the schema would have.
        PngSaveOptions opts;
        opts.compression = std::clamp(values.integer("compression", 6), 0, 9);
        opts.interlace = values.boolean("interlace", false);
        opts.metadata = buildMetadata(input);

        EncodeResult r;
        std::optional<std::vector<std::uint8_t>> bytes = encodePng(*input.pixels, opts, &r.error);
        if (!bytes.has_value())
            return r;  // r.error carries libpng's reason
        r.bytes = std::move(*bytes);
        r.ok = true;
        if (progress)
            progress(1.0f);
        return r;
    }
};

} // namespace

std::unique_ptr<FormatBackend> makePngBackend() { return std::make_unique<PngBackend>(); }

} // namespace mosaic::io
