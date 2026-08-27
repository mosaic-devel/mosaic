#include "io/brush/library.hpp"

#include "common/fs_path.hpp"
#include "common/image_svg.hpp"
#include "io/brush/kpp.hpp"
#include "io/brush/md5.hpp"
#include "io/brush/tip_io.hpp"
#include "io/detail.hpp" // decodePng -- the embedded texture pattern is a PNG in every shipped case

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <tuple>
#include <utility>

namespace mosaic::io::brush {
namespace {

namespace cb = mosaic::core::brush;

[[nodiscard]] std::string lowerExtension(std::string_view fileName) {
    const std::size_t dot = fileName.rfind('.');
    if (dot == std::string_view::npos)
        return {};
    std::string ext(fileName.substr(dot + 1));
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

// The SVG tip, per the upstream loader (kis_svg_brush.cpp): rendered 1000 px wide, height from
// the document's own aspect (integer division there; floor here), over WHITE, then greyscale.
// The rendered image IS the tip image -- white is no paint, like a PNG, no inversion. The
// upstream has no size guard; kMaxTipPixels bounds a hostile aspect ratio.
[[nodiscard]] std::optional<TipFile> readSvgTip(const std::uint8_t* data, std::size_t size) {
    const std::optional<common::SvgSize> box = common::svgIntrinsicSize(data, size);
    if (!box)
        return std::nullopt;
    constexpr int kWidth = 1000;
    const double rawHeight = std::floor(kWidth * box->height / box->width);
    if (!(rawHeight >= 1.0) ||
        rawHeight > static_cast<double>(cb::kMaxTipPixels) / static_cast<double>(kWidth))
        return std::nullopt;
    const int height = static_cast<int>(rawHeight);

    const common::Image image = common::rasterizeSvg(data, size, kWidth, height);
    if (image.width == 0)
        return std::nullopt;

    cb::TipFrame frame;
    frame.width = image.width;
    frame.height = image.height;
    frame.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
    for (std::size_t i = 0; i < frame.rgba.size(); i += 4) {
        const std::uint8_t a = image.rgba[i + 3];
        // Straight-alpha composite over white, then the reference's grey weights.
        const auto over = [&](std::size_t c) {
            return static_cast<std::uint8_t>((image.rgba[i + c] * a + 255 * (255 - a)) / 255);
        };
        const std::uint8_t grey = cb::detail::luma(over(0), over(1), over(2));
        frame.rgba[i] = frame.rgba[i + 1] = frame.rgba[i + 2] = grey;
        frame.rgba[i + 3] = 255;
    }

    TipFile tip;
    tip.frames.push_back(std::move(frame));
    tip.sourceKind = cb::TipSourceKind::Mask;
    tip.defaultApplication = cb::TipApplication::AlphaMask;
    tip.hasColorAndTransparency = false; // svg is not a colorful class (§3.5)
    tip.spacing = 0.25;
    return tip;
}

[[nodiscard]] std::optional<TipFile> decodeTipFile(std::string_view fileName,
                                                   const std::vector<std::uint8_t>& bytes) {
    const std::string ext = lowerExtension(fileName);
    if (ext == "gbr")
        return readGbr(bytes.data(), bytes.size());
    if (ext == "gih")
        return readGih(bytes.data(), bytes.size());
    if (ext == "png")
        return readPngTip(bytes.data(), bytes.size());
    if (ext == "svg")
        return readSvgTip(bytes.data(), bytes.size());
    // .abr never rides in a bundle's brushes/ (it is a collection, not a cell), .pat is a
    // pattern; anything else is unknown. All fall back, counted by the caller.
    return std::nullopt;
}

// MD5 hex is compared CASE-INSENSITIVELY: md5Hex() emits lowercase, but two rows of Krita_4's
// own manifest are uppercase (a different tool wrote them), so a foreign preset's claim can be
// too -- and a case-sensitive index would take a spurious filename fallback with a false
// "md5 mismatch" note.
[[nodiscard]] std::string lowerHex(std::string_view hex) {
    std::string out(hex);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// The mapper's fallback shape (mapper.cpp): the default round auto tip, so a preset whose
// reference resolves to nothing stays usable.
void fallBackToRoundTip(TipXml& tip) {
    tip.kind = TipXml::Kind::Auto;
    tip.autoTip = AutoTipXml{};
    tip.autoTip.generator.diameter = 24.0;
}

// Sharing key for built tips: same file, same application, same adjustments -> one build. The
// doubles compare exactly; they come from the same parse, not from arithmetic.
struct TipKey {
    std::size_t entryIndex = 0;
    cb::TipApplication application = cb::TipApplication::AlphaMask;
    bool autoMidPoint = false;
    double midPoint = 127.0;
    double brightness = 0.0;
    double contrast = 0.0;

    [[nodiscard]] bool operator<(const TipKey& o) const noexcept {
        return std::tie(entryIndex, application, autoMidPoint, midPoint, brightness, contrast) <
               std::tie(o.entryIndex, o.application, o.autoMidPoint, o.midPoint, o.brightness,
                        o.contrast);
    }
};

// ------------------------------------------------------------------------------------------------
// The TEXTURE option's embedded pattern (§6.6h).

// Base64, decode only. ⚠ THE PAYLOAD IS ENCODED TWICE and that is not a mistake in either layer:
// the XML layer leaves a `bytearray` param base64-encoded (preset_xml.hpp), and what it decodes to
// is the producer's OWN base64 string of the pattern file. So the pattern's bytes are two decodes
// down, and a reader that stops after one gets ASCII that begins "iVBORw0KGgo" -- which is the
// base64 of a PNG signature, not a PNG signature, and would fail the decoder for the wrong reason.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> base64Decode(std::string_view in) {
    static constexpr signed char kInvalid = -1;
    const auto value = [](unsigned char c) -> signed char {
        if (c >= 'A' && c <= 'Z')
            return static_cast<signed char>(c - 'A');
        if (c >= 'a' && c <= 'z')
            return static_cast<signed char>(c - 'a' + 26);
        if (c >= '0' && c <= '9')
            return static_cast<signed char>(c - '0' + 52);
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return kInvalid;
    };
    std::vector<std::uint8_t> out;
    out.reserve(in.size() / 4 * 3 + 3);
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char ch : in) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '=')
            break;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            continue;
        const signed char v = value(c);
        if (v == kInvalid)
            return std::nullopt; // a hostile or truncated payload, refused rather than guessed at
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFFu));
        }
    }
    return out;
}

// Sharing key for baked texture patterns: the same payload put through the same bake is the same
// mask. The doubles compare exactly, like TipKey's -- they come from one parse, not from arithmetic.
struct TextureKey {
    std::string digest; // md5 of the embedded payload, so 350 KB of base64 is not the key
    double scale = 1.0;
    double brightness = 0.0;
    double contrast = 1.0;
    double neutralPoint = 0.5;
    bool invert = false;
    int cutoffPolicy = 0;
    int cutoffLeft = 0;
    int cutoffRight = 255;

    [[nodiscard]] bool operator<(const TextureKey& o) const noexcept {
        return std::tie(digest, scale, brightness, contrast, neutralPoint, invert, cutoffPolicy,
                        cutoffLeft, cutoffRight) <
               std::tie(o.digest, o.scale, o.brightness, o.contrast, o.neutralPoint, o.invert,
                        o.cutoffPolicy, o.cutoffLeft, o.cutoffRight);
    }
};

} // namespace

int PresetLibrary::addBundle(const std::uint8_t* data, std::size_t size,
                             std::string_view sourcePath, std::string* error) {
    const std::optional<Bundle> bundle = Bundle::open(data, size, error);
    if (!bundle)
        return 0;

    std::uint64_t budget = kMaxLibraryScanBytes;
    const auto withinBudget = [&budget](std::uint64_t want) {
        if (want > budget)
            return false;
        budget -= want;
        return true;
    };

    // Pass 1 -- the tip indexes. The digest is computed over the FILE BYTES (the manifest's
    // md5sum is a claim; the preset's reference must match what is actually there). First
    // occurrence wins in both indexes, as in a database keyed on unique columns.
    std::map<std::string, std::size_t, std::less<>> byMd5;  // hex digest -> resources index
    std::map<std::string, std::size_t, std::less<>> byName; // file name  -> resources index
    const std::vector<BundleResource>& resources = bundle->resources();
    for (std::size_t i = 0; i < resources.size(); ++i) {
        if (resources[i].mediaType != "brushes")
            continue;
        const ZipEntry& entry = bundle->zip().entries()[resources[i].entryIndex];
        if (!withinBudget(entry.uncompressedSize)) {
            ++m_counters.scanBudgetSkips;
            continue;
        }
        const auto bytes = bundle->read(resources[i]);
        if (!bytes)
            continue; // undecodable now, undecodable when referenced: it shows as a fallback
        byMd5.emplace(md5Hex(bytes->data(), bytes->size()), i);
        byName.emplace(resources[i].fileName, i);
    }

    // Scan-scoped caches: decoded files (so the content verdict is computed once per file) and
    // built tips (shared across presets agreeing on application + adjustments).
    std::map<std::size_t, std::optional<TipFile>> decoded;
    std::map<TipKey, std::shared_ptr<const cb::BitmapTip>> built;
    // Scan-scoped texture caches (§6.6h): the decoded pattern image per payload digest, and the
    // BAKED mask per (payload, bake). Five shipped presets embed the same dotted paper at three
    // different bakes, so both levels earn their keep.
    std::map<std::string, std::optional<common::Image>> patternImages;
    std::map<TextureKey, std::shared_ptr<const cb::TexturePattern>> bakedPatterns;
    const auto resolveTexture =
        [&](const TextureImport& imp) -> std::shared_ptr<const cb::TexturePattern> {
        const auto payload = base64Decode(imp.patternBase64);
        if (!payload || payload->empty())
            return nullptr;
        // ⚠ The SECOND decode. See base64Decode's note: the producer stores its pattern file as a
        // base64 STRING inside a param that is itself base64 in the document.
        const auto fileBytes =
            base64Decode(std::string_view(reinterpret_cast<const char*>(payload->data()),
                                          payload->size()));
        if (!fileBytes || fileBytes->empty())
            return nullptr;
        const std::string digest = md5Hex(fileBytes->data(), fileBytes->size());
        TextureKey key{digest,
                       imp.bake.scale,
                       imp.bake.brightness,
                       imp.bake.contrast,
                       imp.bake.neutralPoint,
                       imp.bake.invert,
                       imp.bake.cutoffPolicy,
                       imp.bake.cutoffLeft,
                       imp.bake.cutoffRight};
        if (const auto it = bakedPatterns.find(key); it != bakedPatterns.end())
            return it->second;

        auto [imgIt, fresh] = patternImages.try_emplace(digest);
        if (fresh)
            imgIt->second = mosaic::io::detail::decodePng(*fileBytes, nullptr);
        std::shared_ptr<const cb::TexturePattern> baked;
        if (imgIt->second)
            baked = cb::bakeTexturePattern(imgIt->second->rgba.data(), imgIt->second->width,
                                           imgIt->second->height, imp.bake);
        bakedPatterns.emplace(std::move(key), baked);
        return baked;
    };

    // The resolution chain, shared by the PRIMARY tip and the MASKING brush's (§6.2) so the two
    // cannot drift on the order: the md5 index first, then the filename (upstream's priority,
    // §3.5). `resources.size()` is the nothing-matched sentinel.
    const auto resolveTipIndex = [&](const PredefinedTipXml& ref, bool* byDigest) -> std::size_t {
        *byDigest = false;
        if (!ref.md5sum.empty()) {
            if (const auto it = byMd5.find(lowerHex(ref.md5sum)); it != byMd5.end()) {
                *byDigest = true;
                return it->second;
            }
        }
        if (const auto it = byName.find(ref.filename); it != byName.end())
            return it->second;
        return resources.size();
    };
    // Decode-once per file; a failure is remembered as an empty optional (and counted once).
    const auto decodeAt = [&](std::size_t index) -> const std::optional<TipFile>* {
        auto [it, fresh] = decoded.try_emplace(index);
        if (fresh) {
            if (const auto tipBytes = bundle->read(resources[index]))
                it->second = decodeTipFile(resources[index].fileName, *tipBytes);
            if (!it->second)
                ++m_counters.tipDecodeFailures;
        }
        return &it->second;
    };
    // Build-once per (file, application, adjustments); null when the build came up empty.
    const auto builtTipAt = [&](std::size_t index, cb::TipApplication application,
                                const cb::TipAdjustments& adj,
                                const TipFile& file) -> std::shared_ptr<const cb::BitmapTip> {
        const TipKey key{index, application, adj.autoMidPoint, adj.midPoint, adj.brightness,
                         adj.contrast};
        auto [it, fresh] = built.try_emplace(key);
        if (fresh) {
            auto tip = std::make_shared<cb::BitmapTip>(file.frames, application, file.sourceKind,
                                                       adj, file.hose);
            if (!tip->empty())
                it->second = std::move(tip);
        }
        return it->second;
    };

    int added = 0;
    for (const BundleResource& res : resources) {
        if (res.mediaType != "paintoppresets" || lowerExtension(res.fileName) != "kpp")
            continue;
        const ZipEntry& entry = bundle->zip().entries()[res.entryIndex];
        if (!withinBudget(entry.uncompressedSize)) {
            ++m_counters.scanBudgetSkips;
            continue;
        }
        const auto bytes = bundle->read(res);
        if (!bytes) {
            ++m_counters.presetsFailed;
            continue;
        }
        std::optional<BrushPreset> preset = readKpp(bytes->data(), bytes->size());
        if (!preset) {
            ++m_counters.presetsFailed;
            continue;
        }

        LibraryPreset lp;
        lp.preset = std::move(*preset);
        lp.preset.provenance.sourceFormat = "bundle";
        lp.sourcePath = std::string(sourcePath);
        lp.entryName = res.fullPath;

        // Resolve the tip reference, upstream order: md5 index, then filename (§3.5).
        double masterDiameter = 0.0;
        if (lp.preset.tip.kind == TipXml::Kind::Predefined) {
            const PredefinedTipXml& ref = lp.preset.tip.predefined;
            bool byDigest = false;
            const std::size_t resolved = resolveTipIndex(ref, &byDigest);
            const std::optional<TipFile>* tipFile = nullptr;
            if (resolved != resources.size()) {
                lp.tipResolution = byDigest ? TipResolution::ByMd5 : TipResolution::ByFilename;
                if (!byDigest && !ref.md5sum.empty()) {
                    // Upstream warns on exactly this fetch-by-filename-after-md5-miss.
                    addDroppedOption(lp.preset, "Tip '" + ref.filename +
                                                    "': md5 mismatch (matched by filename)");
                }
                tipFile = decodeAt(resolved);
            }

            if (tipFile != nullptr && tipFile->has_value()) {
                // The two staged decisions (preset.hpp), now with the real content verdict.
                const cb::TipApplication application =
                    resolveTipApplication(ref, (*tipFile)->hasColorAndTransparency);
                lp.preset.accumulator = cb::chooseAccumulator(
                    application, lp.preset.colorDynamicsActive,
                    cb::painterVariesColor(lp.preset.painter));
                lp.tip = builtTipAt(resolved, application, ref.adjustments, **tipFile);
            }

            if (lp.tip != nullptr) {
                lp.tipFileName = resources[resolved].fileName;
                masterDiameter = ref.scale * lp.tip->baseSize(0);
                if (lp.tipResolution == TipResolution::ByMd5)
                    ++m_counters.tipsResolvedByMd5;
                else
                    ++m_counters.tipsResolvedByFilename;
            } else {
                // Unresolvable or undecodable: keep the preset usable on the round tip.
                lp.tipResolution = TipResolution::Fallback;
                ++m_counters.tipsFallback;
                addDroppedOption(lp.preset, "Tip file '" + ref.filename +
                                                "' (unavailable; default round tip)");
                degradeFidelity(lp.preset, PresetFidelity::Approximated);
                fallBackToRoundTip(lp.preset.tip);
                masterDiameter = lp.preset.tip.autoTip.generator.diameter;
            }
        } else {
            lp.tipResolution = TipResolution::AutoTip;
            masterDiameter = lp.preset.tip.autoTip.generator.diameter;
        }

        // Masking resolves last: UseMasterSize needs the primary's absolute diameter, which for
        // a predefined tip is scale x max(w, h) of its first frame (§3.5).
        lp.masterDiameter = masterDiameter;
        lp.masking = resolveMasking(lp.preset.masking, masterDiameter);

        // The masking brush's own predefined tip (§6.2), through the same resolution chain and the
        // same decoded/built caches as the primary's. A masking stroke is a grayscale VALUE, so
        // its application is AlphaMask unconditionally -- there is no colour for a content verdict
        // to find a use for. On any miss the masking walk keeps its analytic round disc
        // (resolveMasking's hardness), badged: today's mask rather than no mask. The tip's ratio
        // stays 1 -- a bitmap's frame aspect lives inside the dab's envelope.
        if (lp.masking.enabled && lp.preset.masking.tip.kind == TipXml::Kind::Predefined) {
            const PredefinedTipXml& mref = lp.preset.masking.tip.predefined;
            bool byDigest = false;
            const std::size_t resolved = resolveTipIndex(mref, &byDigest);
            const std::optional<TipFile>* tipFile =
                resolved != resources.size() ? decodeAt(resolved) : nullptr;
            std::shared_ptr<const cb::BitmapTip> bitmap;
            if (tipFile != nullptr && tipFile->has_value())
                bitmap =
                    builtTipAt(resolved, cb::TipApplication::AlphaMask, mref.adjustments, **tipFile);
            if (bitmap != nullptr) {
                lp.masking.tip = cb::makeTip(std::move(bitmap));
                ++m_counters.maskingTipsResolved;
            } else {
                addDroppedOption(lp.preset, "Masking tip file '" + mref.filename +
                                                "' (unavailable; round masking dab)");
                degradeFidelity(lp.preset, PresetFidelity::Approximated);
                ++m_counters.maskingTipsFallback;
            }
        }

        // The TEXTURE option's embedded pattern (§6.6h), decoded and baked here for the same reason
        // the tips are: the mapper is a pure function of the parsed document and does not decode
        // images. A pattern that will not decode disables the option -- which is exactly what the
        // reference does when its pattern fails to load -- and says so.
        if (lp.preset.texture.enabled) {
            if (auto pattern = resolveTexture(lp.preset.texture)) {
                cb::TextureParams& t = lp.texture;
                t.enabled = true;
                t.mode = lp.preset.texture.mode;
                t.pattern = std::move(pattern);
                t.offsetX = lp.preset.texture.offsetX;
                t.offsetY = lp.preset.texture.offsetY;
                t.randomOffsetX = lp.preset.texture.randomOffsetX;
                t.randomOffsetY = lp.preset.texture.randomOffsetY;
                t.softTexturing = lp.preset.texture.softTexturing;
                ++m_counters.texturesResolved;
            } else {
                addDroppedOption(lp.preset, "Texture pattern '" + lp.preset.texture.patternName +
                                                "' (undecodable; painted untextured)");
                degradeFidelity(lp.preset, PresetFidelity::Approximated);
                ++m_counters.texturesFallback;
            }
        }

        m_presets.push_back(std::move(lp));
        ++m_counters.presetsLoaded;
        ++added;
    }

    m_sources.push_back(Source{std::string(sourcePath), bundle->meta()});
    return added;
}

int PresetLibrary::addBundleFile(const std::filesystem::path& path, std::string* error) {
    std::vector<std::uint8_t> data;
    if (!common::readWholeFile(path.string(), data, error))
        return 0;
    return addBundle(data.data(), data.size(), path.string(), error);
}

const ZipReader* PresetLibrary::openSource(const std::string& path, std::string* error) const {
    for (const std::unique_ptr<OpenSource>& open : m_open) {
        if (open->path != path)
            continue;
        if (!open->zip && error != nullptr)
            *error = "cannot open " + path; // the remembered failure, not a fresh attempt
        return open->zip ? &*open->zip : nullptr;
    }

    auto open = std::make_unique<OpenSource>();
    open->path = path;
    ++m_archiveOpens; // an attempt is an open: a source that fails cost the read all the same
    if (common::readWholeFile(path, open->bytes, error))
        open->zip = ZipReader::open(open->bytes.data(), open->bytes.size(), error);
    if (!open->zip)
        open->bytes = {}; // a failed open holds no bytes hostage
    const ZipReader* zip = open->zip ? &*open->zip : nullptr;
    m_open.push_back(std::move(open));
    return zip;
}

std::optional<common::Image> PresetLibrary::loadIcon(const LibraryPreset& preset,
                                                     std::string* error) const {
    ++m_iconLoads;
    // The ZIP is the truth (bundle.hpp) and the scan already validated the container -- so an icon
    // fetch walks the zip directly and does NOT re-run Bundle::open. Re-parsing the manifest and
    // meta.xml to reach one PNG was pure waste, and it was the bulk of what a dock-width drag paid.
    const ZipReader* zip = openSource(preset.sourcePath, error);
    if (zip == nullptr)
        return std::nullopt;
    const ZipEntry* entry = zip->find(preset.entryName);
    if (entry == nullptr) {
        if (error != nullptr)
            *error = "the source no longer holds " + preset.entryName;
        return std::nullopt;
    }
    const auto bytes = zip->read(*entry, error);
    if (!bytes)
        return std::nullopt;
    return readKppIcon(bytes->data(), bytes->size(), error);
}

} // namespace mosaic::io::brush
