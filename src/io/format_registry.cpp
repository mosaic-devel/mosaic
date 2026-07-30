#include "io/format_registry.hpp"

#include "common/fs_path.hpp"

#include "io/backends/backends.hpp"
#include "io/exif_write.hpp"
#include "io/io.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace mosaic::io {

namespace {

[[nodiscard]] std::string toLower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The Common tier's combobox order (§3: "Common at top in popularity order"). A Common-tier
// backend missing from this table sorts after the listed ones, alphabetically.
[[nodiscard]] int commonRank(FormatId id) noexcept {
    static constexpr FormatId kOrder[] = {FormatId::Png,  FormatId::Jpeg, FormatId::Jxl,
                                          FormatId::WebP, FormatId::Gif,  FormatId::Tiff,
                                          FormatId::Avif, FormatId::Pdf,  FormatId::Svg};
    int rank = 0;
    for (const FormatId candidate : kOrder) {
        if (candidate == id)
            return rank;
        ++rank;
    }
    return rank;  // unlisted: after every listed one
}

} // namespace

std::string_view formatIdName(FormatId id) noexcept {
    switch (id) {
    case FormatId::Png: return "png";
    case FormatId::Jpeg: return "jpeg";
    case FormatId::Jxl: return "jxl";
    case FormatId::WebP: return "webp";
    case FormatId::Gif: return "gif";
    case FormatId::Tiff: return "tiff";
    case FormatId::Avif: return "avif";
    case FormatId::Pdf: return "pdf";
    case FormatId::Svg: return "svg";
    case FormatId::Bmp: return "bmp";
    case FormatId::Eps: return "eps";
    case FormatId::Ico: return "ico";
    case FormatId::Jpeg2000: return "jp2";
    case FormatId::OpenExr: return "exr";
    case FormatId::Pnm: return "pnm";
    case FormatId::Qoi: return "qoi";
    case FormatId::RadianceHdr: return "hdr";
    case FormatId::Tga: return "tga";
    }
    return "?";
}

EmbeddedMetadata buildMetadata(const RenderInput& input) {
    EmbeddedMetadata out;
    if (input.stripMetadata)
        return out;  // the privacy toggle: no EXIF, no ICC profile, and no density either
    out.dpi = input.dpi;
    if (input.exif != nullptr)
        out.exif = buildExifPayload(*input.exif);
    out.icc = input.iccProfile;  // already resolved + validated at the UI (RenderInput's note)
    return out;
}

bool encodeToFile(const FormatBackend& backend, const RenderInput& input,
                  const OptionValues& values, const std::string& path, std::string* error) {
    EncodeResult r = backend.encode(input, values);
    if (!r.ok) {
        if (error != nullptr)
            *error = r.error;
        return false;
    }
    std::FILE* fp = common::fopenUtf8(path, "wb");
    if (fp == nullptr) {
        if (error != nullptr)
            *error = "could not open the file for writing";
        return false;
    }
    const std::size_t written =
        r.bytes.empty() ? 0u : std::fwrite(r.bytes.data(), 1, r.bytes.size(), fp);
    const bool closed = std::fclose(fp) == 0;
    if (written != r.bytes.size() || !closed) {
        if (error != nullptr)
            *error = "the file could not be written completely";
        return false;
    }
    return true;
}

std::string extensionOf(std::string_view path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos)
        return {};
    // A dot inside the last path component only; "archive.d/name" has no extension.
    const std::size_t slash = path.find_last_of("/\\");
    if (slash != std::string_view::npos && dot < slash)
        return {};
    return toLower(path.substr(dot + 1));
}

bool FormatRegistry::add(std::unique_ptr<FormatBackend> backend) {
    if (backend == nullptr)
        return false;
    if (find(backend->id()) != nullptr)
        return false;
    m_backends.push_back(std::move(backend));
    return true;
}

const FormatBackend* FormatRegistry::find(FormatId id) const noexcept {
    for (const std::unique_ptr<FormatBackend>& b : m_backends)
        if (b->id() == id)
            return b.get();
    return nullptr;
}

const FormatBackend* FormatRegistry::findByExtension(std::string_view ext) const noexcept {
    if (!ext.empty() && ext.front() == '.')
        ext.remove_prefix(1);
    if (ext.empty())
        return nullptr;
    const std::string want = toLower(ext);
    for (const std::unique_ptr<FormatBackend>& b : m_backends)
        for (const std::string& e : b->extensions())
            if (e == want)
                return b.get();
    return nullptr;
}

const FormatBackend* FormatRegistry::findByPath(std::string_view path) const noexcept {
    const std::string ext = extensionOf(path);
    return ext.empty() ? nullptr : findByExtension(ext);
}

std::vector<const FormatBackend*> FormatRegistry::all() const {
    std::vector<const FormatBackend*> out;
    out.reserve(m_backends.size());
    for (const std::unique_ptr<FormatBackend>& b : m_backends)
        out.push_back(b.get());
    return out;
}

std::vector<const FormatBackend*> FormatRegistry::exportOrder(bool includeExotic) const {
    std::vector<const FormatBackend*> out;
    for (const std::unique_ptr<FormatBackend>& b : m_backends) {
        if (!b->available())
            continue;
        if (b->tier() == FormatTier::Exotic && !includeExotic)
            continue;
        out.push_back(b.get());
    }
    std::sort(out.begin(), out.end(), [](const FormatBackend* a, const FormatBackend* b) {
        const bool aCommon = a->tier() == FormatTier::Common;
        const bool bCommon = b->tier() == FormatTier::Common;
        if (aCommon != bCommon)
            return aCommon;  // the Common tier first, above the divider
        if (aCommon)
            return commonRank(a->id()) < commonRank(b->id());  // popularity order
        // Curated pro and exotic share one alphabetical run below the divider.
        return a->displayName() < b->displayName();
    });
    return out;
}

const FormatRegistry& FormatRegistry::instance() {
    // Explicit construction, one line per backend. Function-local static => thread-safe
    // initialization and no static-init-order dependency on anything else in io.
    static const FormatRegistry registry = [] {
        FormatRegistry r;
        r.add(makePngBackend());
        r.add(makeJpegBackend());
        r.add(makeJxlBackend()); // available() == io::jxlSupported(); the combobox skips it if not
        // M4. Each of these answers available() from its own runtime probe, so a build without
        // the library (or, for AVIF, without an AV1 encoder we will drive) simply does not offer
        // the format -- registration is unconditional so a .webp path still resolves to a backend
        // that can explain itself.
        r.add(makeWebpBackend());
        r.add(makeAvifBackend());
        r.add(makeTiffBackend());
        r.add(makeGifBackend());
        // M5, the curated-pro tier: our own codecs (libmosaicformats), so there is no probe to
        // fail and nothing here is conditional. They sort alphabetically below the Common-tier
        // divider on their display names, which is exportOrder()'s job, not this list's.
        r.add(makeBmpBackend());
        r.add(makeTgaBackend());
        r.add(makePnmBackend());
        r.add(makeQoiBackend());
        r.add(makeIcoBackend());
        r.add(makeHdrBackend());
        return r;
    }();
    return registry;
}

} // namespace mosaic::io
