#include "formats/bmp.hpp"
#include "formats/formats.hpp"
#include "formats/hdr.hpp"
#include "formats/ico.hpp"
#include "formats/pnm.hpp"
#include "formats/qoi.hpp"
#include "formats/tga.hpp"
#include "io/backends/backends.hpp"
#include "io/detail.hpp"
#include "io/io.hpp"
#include "io/quantize.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The M5 adapter: the seven curated-pro FormatBackends, all of them fronting libmosaicformats
// (src/formats, namespace mosaicfmt -- docs/export-system-plan.md §2.2, docs/formats-curated.md).
//
// THIS FILE IS THE DEPENDENCY FENCE. Everything Mosaic-shaped stops here: common::Image becomes a
// mosaicfmt::ImageView on the way down and a Bitmap becomes an Image on the way back, the palette
// for an indexed BMP is chosen by io::quantize (a codec must not own colour policy -- the loss
// banner quotes that quantizer's numbers), and an ICO's PNG payloads are encoded by libpng up here
// because the library below deliberately has no PNG in it.
//
// One TU with six backend classes rather than six files: they are adapters of a dozen lines each
// over one library, they share the conversion helpers, and splitting them would put six copies of
// those helpers in six places. (Six, not seven: PNM's single backend covers PBM/PGM/PPM/PAM, which
// is one FormatId -- FormatId::Pnm -- and one .pnm extension family.)
namespace mosaic::io {

namespace {

[[nodiscard]] mosaicfmt::ImageView viewOf(const common::Image& img) noexcept {
    return mosaicfmt::ImageView{img.rgba.data(), img.width, img.height};
}

[[nodiscard]] mosaicfmt::Rgb8 matteOf(common::Color8 c) noexcept {
    return mosaicfmt::Rgb8{c.r, c.g, c.b};
}

[[nodiscard]] common::Image toImage(mosaicfmt::Bitmap&& bitmap) {
    common::Image img;
    img.width = bitmap.width;
    img.height = bitmap.height;
    img.rgba = std::move(bitmap.rgba);
    return img;
}

[[nodiscard]] std::optional<common::Image> toImage(std::optional<mosaicfmt::Bitmap>&& bitmap) {
    if (!bitmap.has_value())
        return std::nullopt;
    return toImage(std::move(*bitmap));
}

// The prologue and epilogue every encode() shares: reject an empty image, honour the cancel
// contract at both ends, and turn the library's optional-plus-string into an EncodeResult.
template <typename Fn>
[[nodiscard]] EncodeResult runEncode(const RenderInput& input, const ProgressFn& progress,
                                    std::string_view label, Fn&& encode) {
    const std::string prefix(label);
    if (input.pixels == nullptr || input.pixels->empty())
        return EncodeResult::failure(prefix + ": cannot write an empty image");
    if (progress && !progress(0.0f))
        return EncodeResult::failure(prefix + ": cancelled");
    EncodeResult r;
    std::optional<std::vector<std::uint8_t>> bytes = encode(r.error);
    if (!bytes.has_value()) {
        if (r.error.empty())
            r.error = prefix + ": the encode failed";
        return r;
    }
    r.bytes = std::move(*bytes);
    r.ok = true;
    if (progress)
        progress(1.0f);
    return r;
}

// ---------------------------------------------------------------------------------------------
// BMP
// ---------------------------------------------------------------------------------------------

class BmpBackend final : public FormatBackend {
public:
    [[nodiscard]] FormatId id() const noexcept override { return FormatId::Bmp; }
    [[nodiscard]] FormatTier tier() const noexcept override { return FormatTier::CuratedPro; }
    [[nodiscard]] std::string_view displayName() const noexcept override { return "BMP image"; }
    [[nodiscard]] std::vector<std::string> extensions() const override { return {"bmp", "dib"}; }
    [[nodiscard]] std::string_view mimeType() const noexcept override { return "image/bmp"; }

    // What THIS ENCODER writes at its DEFAULTS: a 32-bit V5 bitmap with straight alpha, an
    // embedded ICC profile and the density tags. The three narrower depths and the two older
    // headers drop alpha, and the 8-bit mode quantizes -- both are per-OPTION losses, and
    // FormatCaps has no way to say "except under this one setting" (the same gap that keeps
    // JPEG-in-TIFF out of M4, §3.1a). The schema's help text carries that warning instead; the
    // caps row states the default, which is what the banner must not over-promise about.
    [[nodiscard]] FormatCaps caps() const override {
        FormatCaps c;
        c.channels = ChannelModel::Rgb | ChannelModel::Rgba | ChannelModel::Indexed;
        c.maxBitDepth = 8;
        c.floatPixels = false;
        c.alpha = AlphaKind::Straight;
        c.icc = true;  // V5's PROFILE_EMBEDDED
        c.metadata = MetadataKind::Dpi;  // biXPelsPerMeter / biYPelsPerMeter
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

        OptionDesc depth;
        depth.key = "depth";
        depth.label = "Colour depth";
        depth.help = "32-bit is the only depth that keeps transparency. The others fill "
                     "transparent areas with the matte colour first.";
        depth.type = OptionType::Enum;
        depth.widget = OptionWidget::Dropdown;
        depth.defaultValue = textValue("32");
        depth.choices = {
            EnumChoice{"32", "32-bit with transparency", "8 bits per channel plus alpha"},
            EnumChoice{"24", "24-bit", "8 bits per channel, no transparency -- the most portable"},
            EnumChoice{"16", "16-bit (5-6-5)", "Half the size, visibly banded on gradients"},
            EnumChoice{"8", "8-bit palette", "256 colours or fewer, chosen for this picture"}};
        s.options.push_back(std::move(depth));

        OptionDesc header;
        header.key = "header";
        header.label = "Header version";
        header.help = "Version 5 carries the ICC profile and the alpha channel. Version 3 is what "
                      "the oldest software understands, and it has room for neither.";
        header.type = OptionType::Enum;
        header.widget = OptionWidget::Dropdown;
        header.group = "advanced";
        header.defaultValue = textValue("5");
        header.choices = {EnumChoice{"5", "Version 5 (with colour profile)", ""},
                          EnumChoice{"4", "Version 4 (with alpha)", ""},
                          EnumChoice{"3", "Version 3 (most compatible)", ""}};
        s.options.push_back(std::move(header));

        OptionDesc colors;
        colors.key = "colors";
        colors.label = "Colours";
        colors.help = "How many palette entries the picture may use.";
        colors.type = OptionType::Int;
        colors.widget = OptionWidget::Slider;
        colors.defaultValue = intValue(256);
        colors.min = 2.0;
        colors.max = 256.0;
        colors.step = 1.0;
        colors.visibleWhen.push_back(OptionCondition{"depth", {"8"}, false});
        s.options.push_back(std::move(colors));

        OptionDesc dither;
        dither.key = "dither";
        dither.label = "Dither";
        dither.help = "Scatter the quantisation error into neighbouring pixels, so a gradient "
                      "keeps its shape instead of breaking into bands.";
        dither.type = OptionType::Bool;
        dither.widget = OptionWidget::Checkbox;
        dither.defaultValue = boolValue(true);
        dither.visibleWhen.push_back(OptionCondition{"depth", {"8"}, false});
        s.options.push_back(std::move(dither));

        OptionDesc rle;
        rle.key = "rle";
        rle.label = "Run-length compression";
        rle.help = "Lossless compression for the 8-bit mode. Shrinks flat artwork considerably "
                   "and photographs not at all.";
        rle.type = OptionType::Bool;
        rle.widget = OptionWidget::Checkbox;
        rle.defaultValue = boolValue(false);
        rle.visibleWhen.push_back(OptionCondition{"depth", {"8"}, false});
        s.options.push_back(std::move(rle));

        OptionDesc topDown;
        topDown.key = "top-down";
        topDown.label = "Store rows top-down";
        topDown.help = "BMP traditionally stores its rows bottom-up. Top-down is legal and a "
                       "little faster to read, but a few very old readers refuse it.";
        topDown.type = OptionType::Bool;
        topDown.widget = OptionWidget::Checkbox;
        topDown.group = "advanced";
        topDown.defaultValue = boolValue(false);
        s.options.push_back(std::move(topDown));

        return s;
    }

    [[nodiscard]] EncodeResult encode(const RenderInput& input, const OptionValues& values,
                                      const ProgressFn& progress) const override {
        return runEncode(input, progress, "BMP", [&](std::string& error) {
            const std::string depth = values.text("depth", "32");
            const EmbeddedMetadata metadata = buildMetadata(input);

            mosaicfmt::BmpOptions opts;
            opts.headerVersion = std::clamp(std::atoi(values.text("header", "5").c_str()), 3, 5);
            opts.topDown = values.boolean("top-down", false);
            opts.dpi = metadata.dpi;
            opts.matte = matteOf(input.matte);
            opts.icc = metadata.icc;
            opts.rle = values.boolean("rle", false);

            if (depth == "8") {
                // The palette is the quantizer's, not the codec's. alphaThreshold 0 because BMP
                // has no transparent palette entry: everything is composited onto the matte first,
                // exactly as the 24- and 16-bit modes do it.
                QuantizeOptions qopts;
                qopts.maxColors = std::clamp(values.integer("colors", 256), 2, 256);
                qopts.dither = values.boolean("dither", true);
                qopts.alphaThreshold = 0;
                qopts.matte = input.matte;
                const QuantizedImage q = quantize(*input.pixels, qopts);
                if (q.empty() || q.palette.empty()) {
                    error = "BMP: the palette could not be built";
                    return std::optional<std::vector<std::uint8_t>>{};
                }
                std::vector<mosaicfmt::Rgba8> palette;
                palette.reserve(q.palette.size());
                for (const common::Color8& c : q.palette)
                    palette.push_back(mosaicfmt::Rgba8{c.r, c.g, c.b, 255});
                mosaicfmt::IndexedView indexed;
                indexed.indices = q.indices.data();
                indexed.palette = palette.data();
                indexed.paletteSize = static_cast<std::uint32_t>(palette.size());
                indexed.width = q.width;
                indexed.height = q.height;
                return mosaicfmt::encodeBmpIndexed(indexed, opts, &error);
            }

            opts.depth = depth == "24"   ? mosaicfmt::BmpOptions::Depth::Bgr24
                         : depth == "16" ? mosaicfmt::BmpOptions::Depth::Rgb565
                                         : mosaicfmt::BmpOptions::Depth::Bgra32;
            return mosaicfmt::encodeBmp(viewOf(*input.pixels), opts, &error);
        });
    }
};

// ---------------------------------------------------------------------------------------------
// TGA
// ---------------------------------------------------------------------------------------------

class TgaBackend final : public FormatBackend {
public:
    [[nodiscard]] FormatId id() const noexcept override { return FormatId::Tga; }
    [[nodiscard]] FormatTier tier() const noexcept override { return FormatTier::CuratedPro; }
    [[nodiscard]] std::string_view displayName() const noexcept override { return "TGA image"; }
    [[nodiscard]] std::vector<std::string> extensions() const override { return {"tga", "targa"}; }
    [[nodiscard]] std::string_view mimeType() const noexcept override { return "image/x-tga"; }

    // `Either` is the honest alpha kind here, and TGA is the reason the enum has that arm: the
    // file itself records which convention its alpha follows, in the v2 extension area's
    // attributes type. Choosing "premultiplied" really premultiplies the pixels -- relabelling
    // them without multiplying would be a lie about the file (the same rule TIFF's associated-alpha
    // option follows).
    [[nodiscard]] FormatCaps caps() const override {
        FormatCaps c;
        c.channels = ChannelModel::Rgb | ChannelModel::Rgba;
        c.maxBitDepth = 8;
        c.alpha = AlphaKind::Either;
        c.icc = false;
        c.metadata = MetadataKind::None;
        c.lossless = true;
        c.lossy = false;
        c.maxColors = -1;
        return c;
    }

    [[nodiscard]] OptionsSchema optionsSchema() const override {
        OptionsSchema s;
        s.groups.push_back(OptionGroup{"advanced", "Advanced", /*collapsedByDefault=*/true});

        OptionDesc depth;
        depth.key = "depth";
        depth.label = "Colour depth";
        depth.help = "32-bit keeps the full alpha channel. 16-bit has a single transparency bit, "
                     "so a soft edge becomes a hard one.";
        depth.type = OptionType::Enum;
        depth.widget = OptionWidget::Dropdown;
        depth.defaultValue = textValue("32");
        depth.choices = {EnumChoice{"32", "32-bit with transparency", ""},
                         EnumChoice{"24", "24-bit", "No transparency"},
                         EnumChoice{"16", "16-bit (5-5-5)", "One transparency bit"}};
        s.options.push_back(std::move(depth));

        OptionDesc rle;
        rle.key = "rle";
        rle.label = "Run-length compression";
        rle.help = "Lossless, and understood by every Targa reader. Worth leaving on.";
        rle.type = OptionType::Bool;
        rle.widget = OptionWidget::Checkbox;
        rle.defaultValue = boolValue(true);
        s.options.push_back(std::move(rle));

        OptionDesc alpha;
        alpha.key = "alpha-attributes";
        alpha.label = "Transparency is";
        alpha.help = "What the file says its alpha channel means. Straight is what Mosaic works "
                     "in; premultiplied is what some 3D and compositing pipelines ask for, and "
                     "the pixels really are multiplied through when you pick it.";
        alpha.type = OptionType::Enum;
        alpha.widget = OptionWidget::Dropdown;
        alpha.defaultValue = textValue("straight");
        alpha.choices = {EnumChoice{"straight", "Straight (unassociated)", ""},
                         EnumChoice{"premultiplied", "Premultiplied (associated)", ""},
                         EnumChoice{"ignored", "Not used", "Flatten onto the matte colour"}};
        alpha.visibleWhen.push_back(OptionCondition{"depth", {"24"}, /*negate=*/true});
        s.options.push_back(std::move(alpha));

        OptionDesc origin;
        origin.key = "origin";
        origin.label = "First row";
        origin.help = "Which corner the first stored row belongs to. Both are legal; top-left is "
                      "what modern writers produce.";
        origin.type = OptionType::Enum;
        origin.widget = OptionWidget::Dropdown;
        origin.group = "advanced";
        origin.defaultValue = textValue("top-left");
        origin.choices = {EnumChoice{"top-left", "Top-left", ""},
                          EnumChoice{"bottom-left", "Bottom-left", ""}};
        s.options.push_back(std::move(origin));

        return s;
    }

    [[nodiscard]] EncodeResult encode(const RenderInput& input, const OptionValues& values,
                                      const ProgressFn& progress) const override {
        return runEncode(input, progress, "TGA", [&](std::string& error) {
            const std::string depth = values.text("depth", "32");
            const std::string alpha = values.text("alpha-attributes", "straight");
            mosaicfmt::TgaOptions opts;
            opts.depth = depth == "24"   ? mosaicfmt::TgaOptions::Depth::Bgr24
                         : depth == "16" ? mosaicfmt::TgaOptions::Depth::Bgra16
                                         : mosaicfmt::TgaOptions::Depth::Bgra32;
            opts.rle = values.boolean("rle", true);
            opts.topDown = values.text("origin", "top-left") != "bottom-left";
            opts.alpha = alpha == "premultiplied" ? mosaicfmt::TgaOptions::AlphaAttributes::Premultiplied
                         : alpha == "ignored"     ? mosaicfmt::TgaOptions::AlphaAttributes::Ignored
                                                  : mosaicfmt::TgaOptions::AlphaAttributes::Straight;
            opts.matte = matteOf(input.matte);
            return mosaicfmt::encodeTga(viewOf(*input.pixels), opts, &error);
        });
    }
};

// ---------------------------------------------------------------------------------------------
// PNM / PAM
// ---------------------------------------------------------------------------------------------

class PnmBackend final : public FormatBackend {
public:
    [[nodiscard]] FormatId id() const noexcept override { return FormatId::Pnm; }
    [[nodiscard]] FormatTier tier() const noexcept override { return FormatTier::CuratedPro; }
    [[nodiscard]] std::string_view displayName() const noexcept override {
        return "PNM/PAM image";
    }
    [[nodiscard]] std::vector<std::string> extensions() const override {
        return {"pnm", "ppm", "pgm", "pbm", "pam"};
    }
    [[nodiscard]] std::string_view mimeType() const noexcept override {
        return "image/x-portable-anymap";
    }

    // ⚠ `alpha = None` even though PAM carries transparency, because the DEFAULT variant is PPM
    // and the banner states the default. Choosing PAM therefore over-warns ("transparency will be
    // lost" when it will not) -- which is the safe direction of wrong, since §4.1's rule is that
    // the banner must never OVER-PROMISE. A per-option caps model fixes this, PNM's variant, BMP's
    // depth and QOI's channel count in one stroke; see docs/formats-curated.md.
    [[nodiscard]] FormatCaps caps() const override {
        FormatCaps c;
        c.channels = ChannelModel::Gray | ChannelModel::GrayAlpha | ChannelModel::Rgb |
                     ChannelModel::Rgba;
        c.maxBitDepth = 8;
        c.alpha = AlphaKind::None;
        c.icc = false;
        c.metadata = MetadataKind::None;
        c.lossless = true;
        c.lossy = false;
        c.maxColors = -1;
        return c;
    }

    [[nodiscard]] OptionsSchema optionsSchema() const override {
        OptionsSchema s;
        s.groups.push_back(OptionGroup{"advanced", "Advanced", /*collapsedByDefault=*/true});

        OptionDesc variant;
        variant.key = "variant";
        variant.label = "Variant";
        variant.help = "PPM is colour, PGM grey, PBM black-and-white. PAM is the modern "
                       "generalisation and the only one of the four that keeps transparency.";
        variant.type = OptionType::Enum;
        variant.widget = OptionWidget::Dropdown;
        variant.defaultValue = textValue("ppm");
        variant.choices = {EnumChoice{"ppm", "PPM -- colour", ""},
                           EnumChoice{"pgm", "PGM -- greyscale", ""},
                           EnumChoice{"pbm", "PBM -- black and white", ""},
                           EnumChoice{"pam", "PAM -- with transparency", ""}};
        s.options.push_back(std::move(variant));

        OptionDesc tuple;
        tuple.key = "pam-tuple";
        tuple.label = "PAM channels";
        tuple.help = "Which samples each pixel holds. This is what the file's TUPLTYPE declares.";
        tuple.type = OptionType::Enum;
        tuple.widget = OptionWidget::Dropdown;
        tuple.defaultValue = textValue("rgb-alpha");
        tuple.choices = {EnumChoice{"rgb-alpha", "Colour + transparency", ""},
                         EnumChoice{"rgb", "Colour", ""},
                         EnumChoice{"grayscale-alpha", "Grey + transparency", ""},
                         EnumChoice{"grayscale", "Grey", ""}};
        tuple.visibleWhen.push_back(OptionCondition{"variant", {"pam"}, false});
        s.options.push_back(std::move(tuple));

        OptionDesc threshold;
        threshold.key = "bw-threshold";
        threshold.label = "Black/white threshold";
        threshold.help = "Pixels darker than this become black. There is no dithering here -- a "
                         "bilevel export wants that chosen deliberately, not hidden in a codec.";
        threshold.type = OptionType::Int;
        threshold.widget = OptionWidget::Slider;
        threshold.defaultValue = intValue(128);
        threshold.min = 1.0;
        threshold.max = 254.0;
        threshold.step = 1.0;
        threshold.visibleWhen.push_back(OptionCondition{"variant", {"pbm"}, false});
        s.options.push_back(std::move(threshold));

        OptionDesc ascii;
        ascii.key = "ascii";
        ascii.label = "Write plain text samples";
        ascii.help = "The family's readable form: decimal numbers instead of raw bytes. Three to "
                     "four times larger, and occasionally exactly what you want.";
        ascii.type = OptionType::Bool;
        ascii.widget = OptionWidget::Checkbox;
        ascii.group = "advanced";
        ascii.defaultValue = boolValue(false);
        ascii.visibleWhen.push_back(OptionCondition{"variant", {"pam"}, /*negate=*/true});
        s.options.push_back(std::move(ascii));

        return s;
    }

    [[nodiscard]] EncodeResult encode(const RenderInput& input, const OptionValues& values,
                                      const ProgressFn& progress) const override {
        return runEncode(input, progress, "PNM", [&](std::string& error) {
            const std::string variant = values.text("variant", "ppm");
            const std::string tuple = values.text("pam-tuple", "rgb-alpha");
            mosaicfmt::PnmOptions opts;
            opts.variant = variant == "pgm"   ? mosaicfmt::PnmOptions::Variant::Pgm
                           : variant == "pbm" ? mosaicfmt::PnmOptions::Variant::Pbm
                           : variant == "pam" ? mosaicfmt::PnmOptions::Variant::Pam
                                              : mosaicfmt::PnmOptions::Variant::Ppm;
            opts.pamTuple = tuple == "rgb" ? mosaicfmt::PnmOptions::PamTuple::Rgb
                            : tuple == "grayscale-alpha"
                                ? mosaicfmt::PnmOptions::PamTuple::GrayscaleAlpha
                            : tuple == "grayscale" ? mosaicfmt::PnmOptions::PamTuple::Grayscale
                                                   : mosaicfmt::PnmOptions::PamTuple::RgbAlpha;
            opts.ascii = values.boolean("ascii", false);
            opts.bwThreshold = std::clamp(values.integer("bw-threshold", 128), 1, 254);
            opts.matte = matteOf(input.matte);
            return mosaicfmt::encodePnm(viewOf(*input.pixels), opts, &error);
        });
    }
};

// ---------------------------------------------------------------------------------------------
// QOI
// ---------------------------------------------------------------------------------------------

class QoiBackend final : public FormatBackend {
public:
    [[nodiscard]] FormatId id() const noexcept override { return FormatId::Qoi; }
    [[nodiscard]] FormatTier tier() const noexcept override { return FormatTier::CuratedPro; }
    [[nodiscard]] std::string_view displayName() const noexcept override { return "QOI image"; }
    [[nodiscard]] std::vector<std::string> extensions() const override { return {"qoi"}; }
    [[nodiscard]] std::string_view mimeType() const noexcept override { return "image/qoi"; }

    [[nodiscard]] FormatCaps caps() const override {
        FormatCaps c;
        c.channels = ChannelModel::Rgb | ChannelModel::Rgba;
        c.maxBitDepth = 8;
        c.alpha = AlphaKind::Straight;
        // The colourspace byte is a TAG, not a profile: it says "sRGB" or "all linear" and carries
        // no transform. Claiming `icc` for it would promise a profile the file cannot hold.
        c.icc = false;
        c.metadata = MetadataKind::None;
        c.lossless = true;
        c.lossy = false;
        c.maxColors = -1;
        return c;
    }

    [[nodiscard]] OptionsSchema optionsSchema() const override {
        OptionsSchema s;
        s.groups.push_back(OptionGroup{"advanced", "Advanced", /*collapsedByDefault=*/true});

        OptionDesc channels;
        channels.key = "channels";
        channels.label = "Channels";
        channels.help = "Four channels keep transparency; three declare the image opaque and fill "
                        "transparent areas with the matte colour.";
        channels.type = OptionType::Enum;
        channels.widget = OptionWidget::RadioRow;
        channels.defaultValue = textValue("4");
        channels.choices = {EnumChoice{"4", "RGBA (with transparency)", ""},
                            EnumChoice{"3", "RGB", ""}};
        s.options.push_back(std::move(channels));

        OptionDesc colorspace;
        colorspace.key = "colorspace";
        colorspace.label = "Colourspace tag";
        colorspace.help = "A label in the header, nothing more: QOI never converts anything, and "
                          "neither does Mosaic when writing it. The pixels are identical either "
                          "way.";
        colorspace.type = OptionType::Enum;
        colorspace.widget = OptionWidget::Dropdown;
        colorspace.group = "advanced";
        colorspace.defaultValue = textValue("srgb");
        colorspace.choices = {EnumChoice{"srgb", "sRGB with linear alpha", ""},
                              EnumChoice{"linear", "All channels linear", ""}};
        s.options.push_back(std::move(colorspace));

        return s;
    }

    [[nodiscard]] EncodeResult encode(const RenderInput& input, const OptionValues& values,
                                      const ProgressFn& progress) const override {
        return runEncode(input, progress, "QOI", [&](std::string& error) {
            mosaicfmt::QoiOptions opts;
            opts.channels = values.text("channels", "4") == "3" ? 3 : 4;
            opts.linearColorspace = values.text("colorspace", "srgb") == "linear";
            opts.matte = matteOf(input.matte);
            return mosaicfmt::encodeQoi(viewOf(*input.pixels), opts, &error);
        });
    }
};

// ---------------------------------------------------------------------------------------------
// ICO
// ---------------------------------------------------------------------------------------------

class IcoBackend final : public FormatBackend {
public:
    [[nodiscard]] FormatId id() const noexcept override { return FormatId::Ico; }
    [[nodiscard]] FormatTier tier() const noexcept override { return FormatTier::CuratedPro; }
    [[nodiscard]] std::string_view displayName() const noexcept override {
        return "Windows icon";
    }
    [[nodiscard]] std::vector<std::string> extensions() const override { return {"ico"}; }
    [[nodiscard]] std::string_view mimeType() const noexcept override {
        return "image/vnd.microsoft.icon";
    }

    // `layers = false`: an icon's several sizes are MIP LEVELS of one picture, not a layer stack or
    // a page sequence, and nothing in the document survives into them that would not survive into
    // a single-size export.
    [[nodiscard]] FormatCaps caps() const override {
        FormatCaps c;
        c.channels = ChannelModel::Rgba;
        c.maxBitDepth = 8;
        c.alpha = AlphaKind::Straight;
        c.icc = false;
        c.metadata = MetadataKind::None;
        c.lossless = true;
        c.lossy = false;
        c.maxColors = -1;
        c.layers = false;
        return c;
    }

    [[nodiscard]] OptionsSchema optionsSchema() const override {
        OptionsSchema s;
        s.groups.push_back(OptionGroup{"advanced", "Advanced", /*collapsedByDefault=*/true});

        OptionDesc sizes;
        sizes.key = "sizes";
        sizes.label = "Sizes";
        sizes.help = "An icon file holds several sizes of the same picture. Each is scaled down "
                     "from the exported image; a size larger than it is never invented.";
        sizes.type = OptionType::Enum;
        sizes.widget = OptionWidget::Dropdown;
        sizes.defaultValue = textValue("16-32-48-256");
        sizes.choices = {EnumChoice{"16-32-48-256", "16, 32, 48 and 256 px", "The usual full set"},
                         EnumChoice{"16-32-48", "16, 32 and 48 px", ""},
                         EnumChoice{"16-32", "16 and 32 px", ""},
                         EnumChoice{"256", "256 px only", ""},
                         EnumChoice{"source", "This image's own size", "Capped at 256 px"}};
        s.options.push_back(std::move(sizes));

        OptionDesc payload;
        payload.key = "payload";
        payload.label = "Store each size as";
        payload.help = "A bitmap is understood by every version of Windows; PNG is smaller and "
                       "arrived with Vista. Automatic uses PNG for the 256 px entry -- where the "
                       "saving is large -- and bitmaps below it.";
        payload.type = OptionType::Enum;
        payload.widget = OptionWidget::Dropdown;
        payload.group = "advanced";
        payload.defaultValue = textValue("auto");
        payload.choices = {EnumChoice{"auto", "Automatic", ""},
                           EnumChoice{"bmp", "Bitmap", ""},
                           EnumChoice{"png", "PNG", ""}};
        s.options.push_back(std::move(payload));

        return s;
    }

    [[nodiscard]] EncodeResult encode(const RenderInput& input, const OptionValues& values,
                                      const ProgressFn& progress) const override {
        return runEncode(input, progress, "ICO", [&](std::string& error) {
            const std::string set = values.text("sizes", "16-32-48-256");
            const std::string payload = values.text("payload", "auto");
            std::vector<std::uint32_t> sides;
            if (set == "16-32")
                sides = {16, 32};
            else if (set == "16-32-48")
                sides = {16, 32, 48};
            else if (set == "256")
                sides = {256};
            else if (set == "source")
                sides = {std::min(std::max(input.pixels->width, input.pixels->height),
                                  mosaicfmt::kMaxIcoSide)};
            else
                sides = {16, 32, 48, 256};

            // ⚠ RESERVE BEFORE FILLING. Every IcoEntry holds a pointer INTO these two vectors, so
            // a reallocation mid-loop would leave the entry list pointing at freed buffers -- the
            // binding-lifetime trap, in its most ordinary form.
            std::vector<mosaicfmt::Bitmap> scaled;
            std::vector<std::vector<std::uint8_t>> pngs;
            std::vector<mosaicfmt::IcoEntry> entries;
            scaled.reserve(sides.size());
            pngs.reserve(sides.size());
            entries.reserve(sides.size());

            for (const std::uint32_t side : sides) {
                mosaicfmt::Bitmap bitmap = mosaicfmt::fitSquare(viewOf(*input.pixels), side);
                if (bitmap.empty())
                    continue;  // a size this picture cannot fill; the rest of the set still ships
                scaled.push_back(std::move(bitmap));
            }
            if (scaled.empty()) {
                error = "ICO: none of the requested sizes could be produced";
                return std::optional<std::vector<std::uint8_t>>{};
            }
            for (const mosaicfmt::Bitmap& bitmap : scaled) {
                const bool asPng = payload == "png" ||
                                   (payload == "auto" && bitmap.width >= mosaicfmt::kMaxIcoSide);
                mosaicfmt::IcoEntry entry;
                entry.pixels = bitmap.view();
                if (asPng) {
                    // libpng lives up here, above the fence: the codec library below has no PNG in
                    // it and gains nothing by having one.
                    common::Image img;
                    img.width = bitmap.width;
                    img.height = bitmap.height;
                    img.rgba = bitmap.rgba;
                    std::optional<std::vector<std::uint8_t>> encoded = encodePng(img, {}, &error);
                    if (!encoded.has_value())
                        return std::optional<std::vector<std::uint8_t>>{};
                    pngs.push_back(std::move(*encoded));
                    entry.png = &pngs.back();
                }
                entries.push_back(entry);
            }
            return mosaicfmt::encodeIco(entries, &error);
        });
    }
};

// ---------------------------------------------------------------------------------------------
// Radiance HDR
// ---------------------------------------------------------------------------------------------

class HdrBackend final : public FormatBackend {
public:
    [[nodiscard]] FormatId id() const noexcept override { return FormatId::RadianceHdr; }
    [[nodiscard]] FormatTier tier() const noexcept override { return FormatTier::CuratedPro; }
    [[nodiscard]] std::string_view displayName() const noexcept override {
        return "Radiance HDR image";
    }
    [[nodiscard]] std::vector<std::string> extensions() const override { return {"hdr", "pic"}; }
    [[nodiscard]] std::string_view mimeType() const noexcept override {
        return "image/vnd.radiance";
    }

    // ⚠ `maxBitDepth = 8` and `floatPixels = false` on a format whose whole purpose is high dynamic
    // range. That is the caps rule doing its job: the source is the 8-bit flatten
    // (`render::composite` collapses at toImage8Parallel, plan §5), so this encoder cannot write
    // range the document never had, and saying otherwise would make the banner promise headroom
    // the file will not receive. When the ImageF tap lands (S43-a) these two fields flip and
    // nothing else about this backend changes.
    [[nodiscard]] FormatCaps caps() const override {
        FormatCaps c;
        c.channels = ChannelModel::Rgb;
        c.maxBitDepth = 8;
        c.floatPixels = false;
        c.alpha = AlphaKind::None;
        c.icc = false;
        c.metadata = MetadataKind::None;
        c.lossless = true;
        c.lossy = false;
        c.maxColors = -1;
        return c;
    }

    [[nodiscard]] OptionsSchema optionsSchema() const override {
        OptionsSchema s;
        OptionDesc rle;
        rle.key = "rle";
        rle.label = "Run-length compression";
        rle.help = "Radiance's own per-scanline compression. Lossless, universally read, and "
                   "there is no reason to turn it off except to compare the two.";
        rle.type = OptionType::Bool;
        rle.widget = OptionWidget::Checkbox;
        rle.defaultValue = boolValue(true);
        s.options.push_back(std::move(rle));
        // Appendix A also lists EXPOSURE, GAMMA, PRIMARIES and PIXASPECT. They are deliberately
        // absent: each one is a STATEMENT ABOUT THE PIXELS, and with an 8-bit source there is
        // nothing true to say with them. Writing EXPOSURE=2 without having scaled anything would
        // make every reader divide by two.
        return s;
    }

    [[nodiscard]] EncodeResult encode(const RenderInput& input, const OptionValues& values,
                                      const ProgressFn& progress) const override {
        return runEncode(input, progress, "Radiance HDR", [&](std::string& error) {
            mosaicfmt::HdrOptions opts;
            opts.rle = values.boolean("rle", true);
            opts.matte = matteOf(input.matte);
            return mosaicfmt::encodeHdr(viewOf(*input.pixels), opts, &error);
        });
    }
};

} // namespace

std::unique_ptr<FormatBackend> makeBmpBackend() { return std::make_unique<BmpBackend>(); }
std::unique_ptr<FormatBackend> makeTgaBackend() { return std::make_unique<TgaBackend>(); }
std::unique_ptr<FormatBackend> makePnmBackend() { return std::make_unique<PnmBackend>(); }
std::unique_ptr<FormatBackend> makeQoiBackend() { return std::make_unique<QoiBackend>(); }
std::unique_ptr<FormatBackend> makeIcoBackend() { return std::make_unique<IcoBackend>(); }
std::unique_ptr<FormatBackend> makeHdrBackend() { return std::make_unique<HdrBackend>(); }

namespace detail {

bool sniffCuratedFormat(const std::vector<std::uint8_t>& buf) noexcept {
    return mosaicfmt::sniff(buf.data(), buf.size()) != mosaicfmt::Codec::None;
}

std::optional<common::Image> decodeCuratedFormat(const std::vector<std::uint8_t>& buf,
                                                 std::string* error) {
    switch (mosaicfmt::sniff(buf.data(), buf.size())) {
    case mosaicfmt::Codec::Bmp:
        return toImage(mosaicfmt::decodeBmp(buf.data(), buf.size(), error));
    case mosaicfmt::Codec::Tga:
        return toImage(mosaicfmt::decodeTga(buf.data(), buf.size(), error));
    case mosaicfmt::Codec::Pnm:
        return toImage(mosaicfmt::decodePnm(buf.data(), buf.size(), error));
    case mosaicfmt::Codec::Qoi:
        return toImage(mosaicfmt::decodeQoi(buf.data(), buf.size(), error));
    case mosaicfmt::Codec::RadianceHdr:
        return toImage(mosaicfmt::decodeHdr(buf.data(), buf.size(), error));
    case mosaicfmt::Codec::Ico: {
        // The one format whose payload may be somebody else's: a Vista-era icon entry is a whole
        // PNG file, and THIS is the side of the fence that has libpng.
        const std::optional<mosaicfmt::IcoPayload> entry =
            mosaicfmt::selectIcoEntry(buf.data(), buf.size(), error);
        if (!entry)
            return std::nullopt;
        if (entry->isPng) {
            const std::vector<std::uint8_t> payload(
                buf.begin() + static_cast<std::ptrdiff_t>(entry->offset),
                buf.begin() + static_cast<std::ptrdiff_t>(entry->offset + entry->size));
            return decodePng(payload, error);
        }
        return toImage(mosaicfmt::decodeDib(buf.data() + entry->offset, entry->size,
                                            /*icoEntry=*/true, error));
    }
    case mosaicfmt::Codec::None: break;
    }
    return std::nullopt;
}

} // namespace detail

} // namespace mosaic::io
