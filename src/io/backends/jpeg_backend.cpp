#include "io/backends/backends.hpp"
#include "io/io.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The JPEG FormatBackend -- an ADAPTER over the libjpeg-turbo encoder that already ships
// (io::encodeJpeg, io/jpeg.cpp), exactly like the PNG one. M4 grows the encoder itself (arithmetic
// coding, restart interval, DCT method, mozjpeg trellis -- Appendix A); this file only exposes
// what io::JpegSaveOptions actually honours today, because a schema that offers a control the
// encoder ignores is worse than no control.
namespace mosaic::io {

namespace {

class JpegBackend final : public FormatBackend {
public:
    [[nodiscard]] FormatId id() const noexcept override { return FormatId::Jpeg; }
    [[nodiscard]] FormatTier tier() const noexcept override { return FormatTier::Common; }
    [[nodiscard]] std::string_view displayName() const noexcept override { return "JPEG image"; }
    [[nodiscard]] std::vector<std::string> extensions() const override { return {"jpg", "jpeg"}; }
    [[nodiscard]] std::string_view mimeType() const noexcept override { return "image/jpeg"; }

    // What THIS ENCODER writes. JPEG's own container also carries CMYK, 12-bit samples (in
    // libjpeg-turbo) and XMP/IPTC segments; io::encodeJpeg writes 8-bit YCbCr from RGB, so the
    // loss banner must not promise those. M5 flipped `icc` and the Exif/Dpi metadata bits on,
    // because encodeJpeg now really writes an APP2 ICC_PROFILE sequence, an APP1 Exif segment and
    // the JFIF density (io/jpeg.cpp spliceMetadata).
    [[nodiscard]] FormatCaps caps() const override {
        FormatCaps c;
        c.channels = ChannelModel::Rgb;
        c.maxBitDepth = 8;
        c.floatPixels = false;
        c.alpha = AlphaKind::None;   // the modal's Matte colour fills transparency
        c.icc = true;
        c.metadata = MetadataKind::Exif | MetadataKind::Dpi;
        c.lossless = false;          // no lossless mode AT ALL -- not even at quality 100 (§4.1)
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

        OptionDesc quality;
        quality.key = std::string(kOptQuality);  // the shared vocabulary the loss diff reads
        quality.label = "Quality";
        quality.help = "Higher keeps more detail and makes a bigger file. JPEG is lossy at every "
                       "setting -- even 100 still quantises.";
        quality.type = OptionType::Int;
        quality.widget = OptionWidget::Slider;
        quality.defaultValue = intValue(90);
        quality.min = 0.0;
        quality.max = 100.0;
        quality.step = 1.0;
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
        s.options.push_back(std::move(subsampling));

        OptionDesc progressive;
        progressive.key = "progressive";
        progressive.label = "Progressive";
        progressive.help = "Renders coarse-to-fine while loading, and is usually slightly smaller. "
                           "Costs a little encode time.";
        progressive.type = OptionType::Bool;
        progressive.widget = OptionWidget::Checkbox;
        progressive.group = "advanced";
        progressive.defaultValue = boolValue(false);
        s.options.push_back(std::move(progressive));

        return s;
    }

    [[nodiscard]] EncodeResult encode(const RenderInput& input, const OptionValues& values,
                                      const ProgressFn& progress) const override {
        if (input.pixels == nullptr || input.pixels->empty())
            return EncodeResult::failure("JPEG: cannot write an empty image");
        if (progress && !progress(0.0f))
            return EncodeResult::failure("JPEG: cancelled");

        // Tolerate an uncoerced bag: clamp/snap exactly the way the schema would have.
        JpegSaveOptions opts;
        opts.quality = std::clamp(values.integer(kOptQuality, 90), 0, 100);
        const std::string sub = values.text(kOptSubsampling, "4:2:0");
        if (sub == "4:2:2")
            opts.subsampling = JpegSaveOptions::Subsampling::S422;
        else if (sub == std::string(kSubsampling444))
            opts.subsampling = JpegSaveOptions::Subsampling::S444;
        else
            opts.subsampling = JpegSaveOptions::Subsampling::S420;
        opts.progressive = values.boolean("progressive", false);
        opts.matte = input.matte;  // JPEG has no alpha; the modal's Matte row fills it
        opts.metadata = buildMetadata(input);

        EncodeResult r;
        std::optional<std::vector<std::uint8_t>> bytes = encodeJpeg(*input.pixels, opts, &r.error);
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

std::unique_ptr<FormatBackend> makeJpegBackend() { return std::make_unique<JpegBackend>(); }

} // namespace mosaic::io
