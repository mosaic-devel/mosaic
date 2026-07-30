#include "io/backends/backends.hpp"
#include "io/io.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The WebP FormatBackend (M4) -- an adapter over io::encodeWebp (io/webp.cpp, libwebp +
// libwebpmux), in the same shape as the PNG/JPEG/JXL ones. Like JXL it can be ABSENT: libwebp is
// an optional build dependency, so available() answers the runtime probe and the combobox simply
// skips the format when it is false.
namespace mosaic::io {

namespace {

class WebpBackend final : public FormatBackend {
public:
    [[nodiscard]] FormatId id() const noexcept override { return FormatId::WebP; }
    [[nodiscard]] FormatTier tier() const noexcept override { return FormatTier::Common; }
    [[nodiscard]] std::string_view displayName() const noexcept override { return "WebP image"; }
    [[nodiscard]] std::vector<std::string> extensions() const override { return {"webp"}; }
    [[nodiscard]] std::string_view mimeType() const noexcept override { return "image/webp"; }
    [[nodiscard]] bool available() const noexcept override { return webpSupported(); }

    // What THIS ENCODER writes. The container also carries animation (WebPAnimEncoder) and XMP;
    // we write a single still frame and the EXIF/ICCP chunks only, so the loss banner must not
    // promise the rest.
    [[nodiscard]] FormatCaps caps() const override {
        FormatCaps c;
        c.channels = ChannelModel::Rgb | ChannelModel::Rgba;
        c.maxBitDepth = 8;  // WebP is an 8-bit format, full stop
        c.floatPixels = false;
        c.alpha = AlphaKind::Straight;
        c.icc = true;
        c.metadata = MetadataKind::Exif;
        c.lossless = true;
        c.lossy = true;
        c.animation = false;
        c.maxColors = -1;
        c.layers = false;
        // Lossy WebP is always 4:2:0 internally and exposes no ratio knob, so there is no
        // subsampling CONTROL to warn about; the lossy-encode warning already covers the loss.
        c.chromaSubsampling = false;
        c.vector = false;
        return c;
    }

    [[nodiscard]] OptionsSchema optionsSchema() const override {
        OptionsSchema s;
        s.groups.push_back(OptionGroup{"advanced", "Advanced", /*collapsedByDefault=*/true});

        OptionDesc lossless;
        lossless.key = std::string(kOptLossless);
        lossless.label = "Lossless";
        lossless.help = "Reproduce every pixel exactly. Bigger than a lossy WebP, and usually "
                        "smaller than the same picture as a PNG.";
        lossless.type = OptionType::Bool;
        lossless.widget = OptionWidget::Checkbox;
        lossless.defaultValue = boolValue(false);
        s.options.push_back(std::move(lossless));

        OptionDesc quality;
        quality.key = std::string(kOptQuality);
        quality.label = "Quality";
        quality.help = "In lossy mode, how much detail survives. In lossless mode libwebp reuses "
                       "the same slider for compression effort instead -- the picture is exact "
                       "either way, only the file size and the encode time move.";
        quality.type = OptionType::Int;
        quality.widget = OptionWidget::Slider;
        quality.defaultValue = intValue(80);
        quality.min = 0.0;
        quality.max = 100.0;
        quality.step = 1.0;
        s.options.push_back(std::move(quality));

        OptionDesc method;
        method.key = "method";
        method.label = "Effort";
        method.help = "How hard the encoder searches: 0 is fastest, 6 is slowest and smallest. "
                      "It never changes the picture at a given quality.";
        method.type = OptionType::Int;
        method.widget = OptionWidget::Slider;
        method.group = "advanced";
        method.defaultValue = intValue(4);
        method.min = 0.0;
        method.max = 6.0;
        method.step = 1.0;
        s.options.push_back(std::move(method));

        // The dependent knob, as DATA: near-lossless is a lossless-mode preprocessing pass, so
        // it is meaningless while lossless is off -- and, at anything below 100, it is what makes
        // a "lossless" WebP stop being bit-exact (io/caps.cpp reads this key by name for exactly
        // that reason).
        OptionDesc nearLossless;
        nearLossless.key = std::string(kOptNearLossless);
        nearLossless.label = "Near-lossless preprocessing";
        nearLossless.help = "Below 100 the encoder is allowed to alter pixels slightly so they "
                            "compress better. 100 keeps the file bit-exact.";
        nearLossless.type = OptionType::Int;
        nearLossless.widget = OptionWidget::Slider;
        nearLossless.group = "advanced";
        nearLossless.defaultValue = intValue(100);
        nearLossless.min = 0.0;
        nearLossless.max = 100.0;
        nearLossless.step = 1.0;
        nearLossless.visibleWhen.push_back(
            OptionCondition{std::string(kOptLossless), {"true"}, /*negate=*/false});
        s.options.push_back(std::move(nearLossless));

        OptionDesc alphaQuality;
        alphaQuality.key = "alpha-quality";
        alphaQuality.label = "Alpha quality";
        alphaQuality.help = "How faithfully the transparency mask is stored. 100 keeps it exact; "
                            "lower softens soft edges to save space.";
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

        OptionDesc exact;
        exact.key = "exact";
        exact.label = "Keep colour under transparent pixels";
        exact.help = "Fully transparent pixels have invisible colour values. Keeping them makes "
                     "the file bigger, and is what a bit-exact round-trip needs.";
        exact.type = OptionType::Bool;
        exact.widget = OptionWidget::Checkbox;
        exact.group = "advanced";
        exact.defaultValue = boolValue(false);
        s.options.push_back(std::move(exact));

        return s;
    }

    [[nodiscard]] EncodeResult encode(const RenderInput& input, const OptionValues& values,
                                      const ProgressFn& progress) const override {
        if (!webpSupported())
            return EncodeResult::failure("WebP support was not compiled into this build");
        if (input.pixels == nullptr || input.pixels->empty())
            return EncodeResult::failure("WebP: cannot write an empty image");
        if (progress && !progress(0.0f))
            return EncodeResult::failure("WebP: cancelled");

        const OptionsSchema schema = optionsSchema();
        WebpSaveOptions opts;
        opts.lossless = values.boolean(kOptLossless, false);
        opts.quality = std::clamp(values.integer(kOptQuality, 80), 0, 100);
        opts.method = std::clamp(values.integer("method", 4), 0, 6);
        opts.alphaQuality = std::clamp(values.integer("alpha-quality", 100), 0, 100);
        opts.exact = values.boolean("exact", false);
        // A hidden option still HAS a value (options_schema.hpp): consult visibility before
        // acting on it, or an old near-lossless setting would silently degrade a lossy encode.
        opts.nearLossless = schema.visible(kOptNearLossless, values)
                                ? std::clamp(values.integer(kOptNearLossless, 100), 0, 100)
                                : 100;
        opts.metadata = buildMetadata(input);

        EncodeResult r;
        std::optional<std::vector<std::uint8_t>> bytes = encodeWebp(*input.pixels, opts, &r.error);
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

std::unique_ptr<FormatBackend> makeWebpBackend() { return std::make_unique<WebpBackend>(); }

} // namespace mosaic::io
