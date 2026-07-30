#include "io/backends/backends.hpp"
#include "io/io.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The AVIF FormatBackend (M4) -- an adapter over io::encodeAvif (io/avif.cpp, libavif over
// libaom/SVT-AV1). available() is doing real work here: it is false not only when libavif is
// missing but also when this libavif has no AV1 encoder Mosaic is willing to drive (see the
// encoder-choice note at the top of io/avif.cpp), and the combobox then does not offer AVIF at all.
namespace mosaic::io {

namespace {

class AvifBackend final : public FormatBackend {
public:
    [[nodiscard]] FormatId id() const noexcept override { return FormatId::Avif; }
    [[nodiscard]] FormatTier tier() const noexcept override { return FormatTier::Common; }
    [[nodiscard]] std::string_view displayName() const noexcept override { return "AVIF image"; }
    [[nodiscard]] std::vector<std::string> extensions() const override { return {"avif"}; }
    [[nodiscard]] std::string_view mimeType() const noexcept override { return "image/avif"; }
    [[nodiscard]] bool available() const noexcept override { return avifSupported(); }

    // What THIS ENCODER writes: 8-bit, straight alpha, still image. AVIF's container also holds
    // 10/12-bit HDR, premultiplied alpha, gain maps and image sequences -- the depth half waits
    // on the float tap (§5), the rest on demand. maxBitDepth reads 8 accordingly, so a future
    // high-bit document gets an honest "quantised to 8 bits" warning instead of silence.
    [[nodiscard]] FormatCaps caps() const override {
        FormatCaps c;
        c.channels = ChannelModel::Rgb | ChannelModel::Rgba;
        c.maxBitDepth = 8;
        c.floatPixels = false;
        c.alpha = AlphaKind::Straight;
        c.icc = true;
        c.metadata = MetadataKind::Exif;
        c.lossless = true;
        c.lossy = true;
        c.animation = false;
        c.maxColors = -1;
        c.layers = false;
        c.chromaSubsampling = true;
        c.vector = false;
        return c;
    }

    [[nodiscard]] OptionsSchema optionsSchema() const override {
        OptionsSchema s;
        s.groups.push_back(OptionGroup{"advanced", "Advanced", /*collapsedByDefault=*/true});

        OptionDesc lossless;
        lossless.key = std::string(kOptLossless);
        lossless.label = "Lossless";
        lossless.help = "Reproduce every pixel exactly. This forces full colour resolution and "
                        "the identity colour transform -- quality 100 on its own is not lossless, "
                        "because the colour conversion still rounds.";
        lossless.type = OptionType::Bool;
        lossless.widget = OptionWidget::Checkbox;
        lossless.defaultValue = boolValue(false);
        s.options.push_back(std::move(lossless));

        OptionDesc quality;
        quality.key = std::string(kOptQuality);
        quality.label = "Quality";
        quality.help = "Higher keeps more detail and makes a bigger file. AVIF holds up well far "
                       "below JPEG's usable range -- 50 to 65 is a good photographic band.";
        quality.type = OptionType::Int;
        quality.widget = OptionWidget::Slider;
        quality.defaultValue = intValue(60);
        quality.min = 0.0;
        quality.max = 100.0;
        quality.step = 1.0;
        quality.visibleWhen.push_back(
            OptionCondition{std::string(kOptLossless), {"false"}, /*negate=*/false});
        s.options.push_back(std::move(quality));

        OptionDesc subsampling;
        subsampling.key = std::string(kOptSubsampling);
        subsampling.label = "Chroma subsampling";
        subsampling.help = "How much colour resolution is thrown away. 4:2:0 is the photographic "
                           "default; 4:4:4 keeps full colour and is what text and hard edges want.";
        subsampling.type = OptionType::Enum;
        subsampling.widget = OptionWidget::Dropdown;
        subsampling.defaultValue = textValue("4:2:0");
        subsampling.choices = {
            EnumChoice{"4:2:0", "4:2:0  (smallest)", "Half the colour resolution both ways."},
            EnumChoice{"4:2:2", "4:2:2", "Half the colour resolution horizontally."},
            EnumChoice{std::string(kSubsampling444), "4:4:4  (full colour)",
                       "Full colour resolution -- best for text, line art and screenshots."},
        };
        subsampling.visibleWhen.push_back(
            OptionCondition{std::string(kOptLossless), {"false"}, /*negate=*/false});
        s.options.push_back(std::move(subsampling));

        OptionDesc speed;
        speed.key = "speed";
        speed.label = "Encoder speed";
        speed.help = "0 is slowest and smallest, 10 is fastest. It does not change the quality "
                     "setting -- only how long the encoder searches for a way to meet it.";
        speed.type = OptionType::Int;
        speed.widget = OptionWidget::Slider;
        speed.group = "advanced";
        speed.defaultValue = intValue(6);
        speed.min = 0.0;
        speed.max = 10.0;
        speed.step = 1.0;
        s.options.push_back(std::move(speed));

        OptionDesc alphaQuality;
        alphaQuality.key = "alpha-quality";
        alphaQuality.label = "Alpha quality";
        alphaQuality.help = "How faithfully the transparency mask is stored. 100 keeps it exact.";
        alphaQuality.type = OptionType::Int;
        alphaQuality.widget = OptionWidget::Slider;
        alphaQuality.group = "advanced";
        alphaQuality.defaultValue = intValue(100);
        alphaQuality.min = 0.0;
        alphaQuality.max = 100.0;
        alphaQuality.step = 1.0;
        alphaQuality.visibleWhen.push_back(
            OptionCondition{std::string(kOptLossless), {"false"}, /*negate=*/false});
        s.options.push_back(std::move(alphaQuality));

        return s;
    }

    [[nodiscard]] EncodeResult encode(const RenderInput& input, const OptionValues& values,
                                      const ProgressFn& progress) const override {
        if (!avifSupported())
            return EncodeResult::failure(
                "AVIF support is not available in this build (libavif with libaom or SVT-AV1 is "
                "required)");
        if (input.pixels == nullptr || input.pixels->empty())
            return EncodeResult::failure("AVIF: cannot write an empty image");
        if (progress && !progress(0.0f))
            return EncodeResult::failure("AVIF: cancelled");

        AvifSaveOptions opts;
        opts.lossless = values.boolean(kOptLossless, false);
        opts.quality = std::clamp(values.integer(kOptQuality, 60), 0, 100);
        opts.alphaQuality = std::clamp(values.integer("alpha-quality", 100), 0, 100);
        opts.speed = std::clamp(values.integer("speed", 6), 0, 10);
        const std::string sub = values.text(kOptSubsampling, "4:2:0");
        if (sub == "4:2:2")
            opts.yuv = AvifSaveOptions::Yuv::Yuv422;
        else if (sub == std::string(kSubsampling444))
            opts.yuv = AvifSaveOptions::Yuv::Yuv444;
        else
            opts.yuv = AvifSaveOptions::Yuv::Yuv420;
        opts.metadata = buildMetadata(input);

        EncodeResult r;
        std::optional<std::vector<std::uint8_t>> bytes = encodeAvif(*input.pixels, opts, &r.error);
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

std::unique_ptr<FormatBackend> makeAvifBackend() { return std::make_unique<AvifBackend>(); }

} // namespace mosaic::io
