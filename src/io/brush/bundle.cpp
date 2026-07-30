#include "io/brush/bundle.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <map>

namespace mosaic::io::brush {
namespace {

// The documents inside a bundle are small (K4's manifest is ~60 KB); this only bounds hostility.
constexpr std::size_t kMaxBundleXmlBytes = 16u << 20;

void setError(std::string* error, std::string what) {
    if (error != nullptr)
        *error = std::move(what);
}

// The element/attribute LOCAL name: what follows the prefix. pugixml does no namespace
// processing, so "manifest:file-entry" is a literal string -- but the prefix is the producer's
// choice, not the format's, and matching on it would reject a conforming writer that picked
// another one. (Krita's own meta.xml never declares its prefixes at all.)
[[nodiscard]] std::string_view localName(const char* name) noexcept {
    const std::string_view n(name);
    const std::size_t colon = n.rfind(':');
    return colon == std::string_view::npos ? n : n.substr(colon + 1);
}

[[nodiscard]] pugi::xml_node childByLocalName(pugi::xml_node parent, std::string_view local) {
    for (pugi::xml_node child : parent.children()) {
        if (localName(child.name()) == local)
            return child;
    }
    return {};
}

[[nodiscard]] const char* attributeByLocalName(pugi::xml_node node, std::string_view local) {
    for (pugi::xml_attribute attr : node.attributes()) {
        if (localName(attr.name()) == local)
            return attr.value();
    }
    return nullptr;
}

[[nodiscard]] bool loadXml(pugi::xml_document& doc, const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() > kMaxBundleXmlBytes)
        return false;
    return static_cast<bool>(
        doc.load_buffer(bytes.data(), bytes.size(), pugi::parse_default | pugi::parse_doctype));
}

// The manifest's full-path rows are written without a leading slash (the root row "/" aside),
// but tolerate one: the comparison is against zip entry names, which never carry it.
[[nodiscard]] std::string_view stripLeadingSlash(std::string_view path) noexcept {
    return !path.empty() && path.front() == '/' ? path.substr(1) : path;
}

} // namespace

std::optional<Bundle> Bundle::open(const std::uint8_t* data, std::size_t size,
                                   std::string* error) {
    std::optional<ZipReader> zip = ZipReader::open(data, size, error);
    if (!zip)
        return std::nullopt;

    // The mimetype entry is the format signature. Its VALUE is checked, not its compression --
    // Krita_4 stores it first per ODF custom, RGBA_brushes deflates it mid-archive, and both
    // are Krita's own output (§3.7, zip.hpp).
    const ZipEntry* mime = zip->find("mimetype");
    if (mime == nullptr) {
        setError(error, "not a resource bundle (no mimetype entry)");
        return std::nullopt;
    }
    std::string mimeError;
    const std::optional<std::vector<std::uint8_t>> mimeBytes = zip->read(*mime, &mimeError);
    if (!mimeBytes) {
        setError(error, "unreadable mimetype entry: " + mimeError);
        return std::nullopt;
    }
    if (std::string_view(reinterpret_cast<const char*>(mimeBytes->data()), mimeBytes->size()) !=
        kBundleMimeType) {
        setError(error, "not a resource bundle (foreign mimetype)");
        return std::nullopt;
    }

    Bundle bundle;
    bundle.m_zip = std::move(*zip);

    // The manifest's md5sum claims, keyed by full-path. A row is consumed when a zip resource
    // matches it; what remains unconsumed is the manifest-only counter.
    std::map<std::string, std::string, std::less<>> manifestMd5;
    if (const ZipEntry* manifest = bundle.m_zip.find("META-INF/manifest.xml")) {
        if (const auto bytes = bundle.m_zip.read(*manifest)) {
            pugi::xml_document doc;
            if (loadXml(doc, *bytes)) {
                const pugi::xml_node root = childByLocalName(doc, "manifest");
                bundle.m_hasManifest = static_cast<bool>(root);
                for (pugi::xml_node row : root.children()) {
                    if (localName(row.name()) != "file-entry")
                        continue;
                    const char* fullPath = attributeByLocalName(row, "full-path");
                    if (fullPath == nullptr || *fullPath == '\0') {
                        ++bundle.m_skippedManifestRows;
                        continue;
                    }
                    const std::string_view path = stripLeadingSlash(fullPath);
                    if (path.empty())
                        continue; // the root row ("/") describes the bundle itself
                    if (manifestMd5.size() >= kMaxZipEntries) {
                        ++bundle.m_skippedManifestRows;
                        continue;
                    }
                    const char* md5 = attributeByLocalName(row, "md5sum");
                    // Krita_4's own manifest lists 52 of its brushes twice (rows agreeing on
                    // md5sum). The LAST row wins -- preset_xml's duplicate discipline -- and
                    // the counter keeps the collapse visible.
                    const auto [it, inserted] =
                        manifestMd5.insert_or_assign(std::string(path), md5 != nullptr ? md5 : "");
                    if (!inserted)
                        ++bundle.m_duplicateManifestRows;
                }
            }
        }
    }

    // meta.xml: attribution, preserved verbatim (§4.1). Absence is a counter-free fact.
    if (const ZipEntry* metaEntry = bundle.m_zip.find("meta.xml")) {
        if (const auto bytes = bundle.m_zip.read(*metaEntry)) {
            pugi::xml_document doc;
            if (loadXml(doc, *bytes)) {
                const pugi::xml_node root = childByLocalName(doc, "meta");
                bundle.m_hasMeta = static_cast<bool>(root);
                BundleMeta& meta = bundle.m_meta;
                meta.generator = childByLocalName(root, "generator").child_value();
                meta.author = childByLocalName(root, "author").child_value();
                meta.description = childByLocalName(root, "description").child_value();
                meta.initialCreator = childByLocalName(root, "initial-creator").child_value();
                meta.creator = childByLocalName(root, "creator").child_value();
                meta.date = childByLocalName(root, "creation-date").child_value();
                for (pugi::xml_node row : root.children()) {
                    if (localName(row.name()) != "meta-userdefined")
                        continue;
                    const char* name = attributeByLocalName(row, "name");
                    const char* value = attributeByLocalName(row, "value");
                    if (name == nullptr || value == nullptr)
                        continue;
                    const std::string_view key(name);
                    if (key == "license")
                        meta.license = value;
                    else if (key == "website")
                        meta.website = value;
                    else if (key == "email")
                        meta.email = value;
                }
            }
        }
    }

    // Enumerate the payload from the zip content. Directories, the three container entries and
    // META-INF/ are structure, not payload; a top-level stray file belongs to no media type and
    // is not a resource.
    const std::vector<ZipEntry>& entries = bundle.m_zip.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string& name = entries[i].name;
        if (name.empty() || name.back() == '/')
            continue;
        if (name == "mimetype" || name == "meta.xml" || name == "preview.png")
            continue;
        if (name.rfind("META-INF/", 0) == 0)
            continue;
        const std::size_t firstSlash = name.find('/');
        if (firstSlash == std::string::npos || firstSlash == 0)
            continue;

        BundleResource res;
        res.mediaType = name.substr(0, firstSlash);
        res.fullPath = name;
        res.fileName = name.substr(name.rfind('/') + 1);
        res.entryIndex = i;
        if (const auto it = manifestMd5.find(name); it != manifestMd5.end()) {
            res.inManifest = true;
            res.manifestMd5 = it->second;
            manifestMd5.erase(it);
        } else if (bundle.m_hasManifest) {
            ++bundle.m_unlistedResources;
        }
        bundle.m_resources.push_back(std::move(res));
    }

    // What the manifest still claims after every zip resource took its row. (Rows naming the
    // container entries -- some producers list preview.png -- are not payload discrepancies.)
    for (const auto& [path, md5] : manifestMd5) {
        if (path == "mimetype" || path == "meta.xml" || path == "preview.png" ||
            path.rfind("META-INF/", 0) == 0)
            continue;
        ++bundle.m_manifestOnlyRows;
    }

    return bundle;
}

std::optional<std::vector<std::uint8_t>> Bundle::read(const BundleResource& resource,
                                                      std::string* error) const {
    return m_zip.read(m_zip.entries()[resource.entryIndex], error);
}

} // namespace mosaic::io::brush
