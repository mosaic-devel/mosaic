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

// The JPEG XL FormatBackend -- an adapter over io::encodeJxl (io/jxl.cpp, libjxl). Unlike PNG and
// JPEG it can be ABSENT: libjxl is an optional dependency, so available() answers the build-time
// probe and the registry simply does not offer the format when it is false (the backend is still
// registered, and still findable by extension, so a .jxl path resolves to a backend that can
// explain itself rather than to nothing).
//
// The full JXL surface (modular/VarDCT, progressive, responsive, float/HDR, box metadata, lossless
// JPEG transcode -- Appendix A/C) belongs to M4 and the HDR tier; the schema describes only what
// io::JxlSaveOptions honours today.
namespace mosaic::io {

namespace {

class JxlBackend final : public FormatBackend {
public:
    [[nodiscard]] FormatId id() const noexcept override { return FormatId::Jxl; }
    [[nodiscard]] FormatTier tier() const noexcept override { return FormatTier::Common; }
    [[nodiscard]] std::string_view displayName() const noexcept override {
        return "JPEG XL image";
    }
    [[nodiscard]] std::vector<std::string> extensions() const override { return {"jxl"}; }
    [[nodiscard]] std::string_view mimeType() const noexcept override { return "image/jxl"; }
    [[nodiscard]] bool available() const noexcept override { return jxlSupported(); }

    // What THIS ENCODER writes: 8-bit straight-alpha RGBA, both modes, an original-profile ICC tag
    // and an Exif box (both new in M5). JXL's container also carries CMYK, 16/32-bit float,
    // animation and XMP/JUMBF boxes -- the depth half waits on the >8-bit tap (§5), the rest on
    // demand. There is no `Dpi`: JXL records physical size through the codestream's intrinsic
    // dimensions, which this encoder does not set, so claiming density would be a promise the file
    // does not keep.
    [[nodiscard]] FormatCaps caps() const override {
        FormatCaps c;
        c.channels = ChannelModel::Rgba;
        c.maxBitDepth = 8;
        c.floatPixels = false;
        c.alpha = AlphaKind::Straight;
        c.icc = true;
        c.metadata = MetadataKind::Exif;
        c.lossless = true;   // ... and the `lossless` option decides which mode a given encode uses
        c.lossy = true;
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

        OptionDesc lossless;
        lossless.key = std::string(kOptLossless);  // the shared vocabulary the loss diff reads
        lossless.label = "Lossless (bit-exact)";
        lossless.help = "Reproduce every pixel exactly. Larger than a lossy JXL, still usually "
                        "smaller than a PNG.";
        lossless.type = OptionType::Bool;
        lossless.widget = OptionWidget::Checkbox;
        lossless.defaultValue = boolValue(false);
        s.options.push_back(std::move(lossless));

        // The dependent knob that used to need bespoke panel code: distance is meaningless while
        // lossless is on, so it is DATA, not UI logic (§2.1). The value stays in the bag while
        // hidden -- unchecking Lossless brings the same distance back.
        OptionDesc distance;
        distance.key = std::string(kOptDistance);
        distance.label = "Distance";
        distance.help = "Butteraugli distance: 0 is mathematically lossless, 1.0 is visually "
                        "lossless, higher is smaller and softer.";
        distance.type = OptionType::Real;
        distance.widget = OptionWidget::Slider;
        distance.defaultValue = realValue(1.0);
        distance.min = 0.0;
        distance.max = 15.0;
        distance.step = 0.1;
        distance.decimals = 1;
        distance.visibleWhen.push_back(
            OptionCondition{std::string(kOptLossless), {"false"}, /*negate=*/false});
        s.options.push_back(std::move(distance));

        OptionDesc effort;
        effort.key = "effort";
        effort.label = "Effort";
        effort.help = "How hard the encoder works: 1 is fast, 9 is slowest and smallest. It never "
                      "changes what the picture looks like at a given distance.";
        effort.type = OptionType::Int;
        effort.widget = OptionWidget::Slider;
        effort.group = "advanced";
        effort.defaultValue = intValue(7);
        effort.min = 1.0;
        effort.max = 9.0;
        effort.step = 1.0;
        s.options.push_back(std::move(effort));

        return s;
    }

    [[nodiscard]] EncodeResult encode(const RenderInput& input, const OptionValues& values,
                                      const ProgressFn& progress) const override {
        if (!jxlSupported())
            return EncodeResult::failure("JPEG XL support was not compiled into this build");
        if (input.pixels == nullptr || input.pixels->empty())
            return EncodeResult::failure("JPEG XL: cannot write an empty image");
        if (progress && !progress(0.0f))
            return EncodeResult::failure("JPEG XL: cancelled");

        JxlSaveOptions opts;
        opts.lossless = values.boolean(kOptLossless, false);
        opts.distance = static_cast<float>(std::clamp(values.number(kOptDistance, 1.0), 0.0, 15.0));
        opts.effort = std::clamp(values.integer("effort", 7), 1, 9);
        opts.metadata = buildMetadata(input);

        EncodeResult r;
        std::optional<std::vector<std::uint8_t>> bytes = encodeJxl(*input.pixels, opts, &r.error);
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

std::unique_ptr<FormatBackend> makeJxlBackend() { return std::make_unique<JxlBackend>(); }

} // namespace mosaic::io
