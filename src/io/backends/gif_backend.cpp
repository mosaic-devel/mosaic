#include "io/backends/backends.hpp"
#include "io/io.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The GIF FormatBackend (M4) -- an adapter over io::encodeGif (io/gif.cpp, giflib + our own
// quantizer in io/quantize.hpp).
//
// GIF is the format where the loss banner earns its keep: an ordinary photograph loses its
// truecolour palette AND its soft transparency here, and both are red warnings that the caps row
// below produces without a line of GIF-specific UI code.
namespace mosaic::io {

namespace {

class GifBackend final : public FormatBackend {
public:
    [[nodiscard]] FormatId id() const noexcept override { return FormatId::Gif; }
    [[nodiscard]] FormatTier tier() const noexcept override { return FormatTier::Common; }
    [[nodiscard]] std::string_view displayName() const noexcept override { return "GIF image"; }
    [[nodiscard]] std::vector<std::string> extensions() const override { return {"gif"}; }
    [[nodiscard]] std::string_view mimeType() const noexcept override { return "image/gif"; }
    [[nodiscard]] bool available() const noexcept override { return gifSupported(); }

    // What THIS ENCODER writes: one GIF89a frame, a global palette, a single transparent index.
    // `lossless` is true because the LZW coder reproduces the INDICES exactly -- the loss that
    // matters is the palette, and the caps model says that through maxColors, which is what
    // raises ColorsQuantized. `animation` is false: the container has it, our writer does not.
    [[nodiscard]] FormatCaps caps() const override {
        FormatCaps c;
        c.channels = ChannelModel::Indexed;
        c.maxBitDepth = 8;
        c.floatPixels = false;
        c.alpha = AlphaKind::Binary;
        c.icc = false;
        c.metadata = MetadataKind::Text;  // the GIF89a comment extension
        c.lossless = true;
        c.lossy = false;
        c.animation = false;
        c.maxColors = 256;  // the format ceiling; the palette-size option can only go lower
        c.layers = false;
        c.chromaSubsampling = false;
        c.vector = false;
        return c;
    }

    [[nodiscard]] OptionsSchema optionsSchema() const override {
        OptionsSchema s;
        s.groups.push_back(OptionGroup{"advanced", "Advanced", /*collapsedByDefault=*/true});

        OptionDesc colors;
        colors.key = "colors";
        colors.label = "Colours";
        colors.help = "How many palette entries the picture may use. Fewer means a smaller file "
                      "and coarser colour. When the image has transparency, one entry is spent "
                      "on it.";
        colors.type = OptionType::Int;
        colors.widget = OptionWidget::Slider;
        colors.defaultValue = intValue(256);
        colors.min = 2.0;
        colors.max = 256.0;
        colors.step = 1.0;
        s.options.push_back(std::move(colors));

        OptionDesc dither;
        dither.key = "dither";
        dither.label = "Dither";
        dither.help = "Scatter the quantisation error into neighbouring pixels, so a gradient "
                      "keeps its shape instead of breaking into bands. Costs a little file size.";
        dither.type = OptionType::Bool;
        dither.widget = OptionWidget::Checkbox;
        dither.defaultValue = boolValue(true);
        s.options.push_back(std::move(dither));

        OptionDesc threshold;
        threshold.key = "alpha-threshold";
        threshold.label = "Transparency threshold";
        threshold.help = "GIF transparency is all-or-nothing. Pixels less opaque than this become "
                         "fully transparent; the rest are blended onto the matte colour. 0 makes "
                         "the whole image opaque.";
        threshold.type = OptionType::Int;
        threshold.widget = OptionWidget::Slider;
        threshold.group = "advanced";
        threshold.defaultValue = intValue(128);
        threshold.min = 0.0;
        threshold.max = 255.0;
        threshold.step = 1.0;
        s.options.push_back(std::move(threshold));

        OptionDesc interlace;
        interlace.key = "interlace";
        interlace.label = "Interlaced";
        interlace.help = "Render coarse-to-fine while loading, at the cost of a slightly larger "
                         "file. Rarely wanted today.";
        interlace.type = OptionType::Bool;
        interlace.widget = OptionWidget::Checkbox;
        interlace.group = "advanced";
        interlace.defaultValue = boolValue(false);
        s.options.push_back(std::move(interlace));

        OptionDesc comment;
        comment.key = "comment";
        comment.label = "Comment";
        comment.help = "A short note stored in the file (up to 255 characters). Most viewers "
                       "never show it.";
        comment.type = OptionType::Text;
        comment.widget = OptionWidget::TextField;
        comment.group = "advanced";
        comment.defaultValue = textValue("");
        s.options.push_back(std::move(comment));

        return s;
    }

    [[nodiscard]] EncodeResult encode(const RenderInput& input, const OptionValues& values,
                                      const ProgressFn& progress) const override {
        if (!gifSupported())
            return EncodeResult::failure("GIF support was not compiled into this build");
        if (input.pixels == nullptr || input.pixels->empty())
            return EncodeResult::failure("GIF: cannot write an empty image");
        if (progress && !progress(0.0f))
            return EncodeResult::failure("GIF: cancelled");

        GifSaveOptions opts;
        opts.paletteSize = std::clamp(values.integer("colors", 256), 2, 256);
        opts.dither = values.boolean("dither", true);
        opts.interlace = values.boolean("interlace", false);
        opts.alphaThreshold = std::clamp(values.integer("alpha-threshold", 128), 0, 255);
        opts.matte = input.matte;  // the modal's Matte row backs the one-bit transparency cut
        if (!input.stripMetadata)
            opts.comment = values.text("comment", std::string{});

        EncodeResult r;
        std::optional<std::vector<std::uint8_t>> bytes = encodeGif(*input.pixels, opts, &r.error);
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

std::unique_ptr<FormatBackend> makeGifBackend() { return std::make_unique<GifBackend>(); }

} // namespace mosaic::io
