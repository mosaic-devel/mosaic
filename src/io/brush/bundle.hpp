#pragma once

#include "io/brush/zip.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The `.bundle` reader (docs/brushes.md §3.7): a ZIP whose `mimetype` entry says
// application/x-krita-resourcebundle, with an ODF-style META-INF/manifest.xml, a meta.xml
// carrying Dublin Core attribution plus a user-defined license field, and payload folders
// (brushes/, paintoppresets/, patterns/).
//
// The ZIP CONTENT is the truth; the manifest is a CLAIM. The shipped Krita_4 bundle already
// bends it: its manifest carries 131 brush rows over 79 distinct files -- 52 files listed TWICE
// (agreeing on md5sum, so last-row-wins is harmless there, but a counter says it happened). So
// resources are enumerated from the zip and the manifest contributes only its md5sum claims and
// the discrepancy counters. meta.xml is attribution -- preserved verbatim for the user (§4.1:
// it serves them, it never surveils them).
//
// Only two things fail a bundle: not being a ZIP, and a missing or wrong mimetype (that entry IS
// the format signature). A missing manifest or meta.xml degrades to counters, never to an error.
namespace mosaic::io::brush {

inline constexpr std::string_view kBundleMimeType = "application/x-krita-resourcebundle";

// One payload file, enumerated from the zip. `mediaType` is the top-level folder name -- the ODF
// manifest reuses folder names as its media-type strings (§3.7), so the two vocabularies agree.
struct BundleResource {
    std::string mediaType;   // "brushes", "paintoppresets", "patterns", ...
    std::string fullPath;    // the zip entry name, verbatim
    std::string fileName;    // the path's last segment
    std::size_t entryIndex = 0; // into zip().entries()
    std::string manifestMd5; // the manifest row's md5sum CLAIM; empty when unlisted
    bool inManifest = false;
};

// meta.xml, verbatim. Dublin Core elements plus the meta-userdefined license/website/email rows.
struct BundleMeta {
    std::string generator;
    std::string author;
    std::string description;
    std::string initialCreator;
    std::string creator;
    std::string date;    // meta:creation-date
    std::string license; // meta-userdefined "license" -- the §4 licensing field
    std::string website;
    std::string email;
};

class Bundle {
public:
    // A non-owning view, like ZipReader: `data` must outlive the Bundle and every read through
    // it. Fails only on a broken zip or a missing/foreign mimetype.
    [[nodiscard]] static std::optional<Bundle> open(const std::uint8_t* data, std::size_t size,
                                                    std::string* error = nullptr);

    [[nodiscard]] const ZipReader& zip() const noexcept { return m_zip; }
    [[nodiscard]] const BundleMeta& meta() const noexcept { return m_meta; }
    [[nodiscard]] const std::vector<BundleResource>& resources() const noexcept {
        return m_resources;
    }

    // Decompress one resource (a convenience over zip().read()).
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> read(const BundleResource& resource,
                                                                std::string* error = nullptr) const;

    [[nodiscard]] bool hasManifest() const noexcept { return m_hasManifest; }
    [[nodiscard]] bool hasMeta() const noexcept { return m_hasMeta; }

    // The honesty counters. Krita_4 itself ships duplicateManifestRows == 52; the other three
    // are 0 there, but a repacked or hand-edited bundle earns them.
    [[nodiscard]] int manifestOnlyRows() const noexcept { return m_manifestOnlyRows; }
    [[nodiscard]] int unlistedResources() const noexcept { return m_unlistedResources; }
    [[nodiscard]] int skippedManifestRows() const noexcept { return m_skippedManifestRows; }
    [[nodiscard]] int duplicateManifestRows() const noexcept { return m_duplicateManifestRows; }

private:
    ZipReader m_zip;
    BundleMeta m_meta;
    std::vector<BundleResource> m_resources;
    bool m_hasManifest = false;
    bool m_hasMeta = false;
    int m_manifestOnlyRows = 0;      // manifest lists it, the zip does not hold it
    int m_unlistedResources = 0;     // the zip holds it, the manifest does not list it
    int m_skippedManifestRows = 0;   // rows with no usable full-path
    int m_duplicateManifestRows = 0; // the same full-path twice -- the LAST row wins
};

} // namespace mosaic::io::brush
