// The `.bundle` reader (io/brush/bundle.{hpp,cpp}). Built on the same in-test zip writer as
// test_zip.cpp, so each case controls exactly what the container claims.
//
// The load-bearing rule under test: the zip CONTENT is the truth and the manifest is a CLAIM.
// Krita's own Krita_4 bundle already bends the manifest -- 52 of its brush files are listed
// TWICE (rows agreeing on md5sum) -- and a repacked or hand-edited bundle can lie in either
// direction, so every counter is exercised here.

#include "io/brush/bundle.hpp"

#include "zip_builder.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using mosaic::io::brush::Bundle;
using mosaic::io::brush::BundleResource;
using ziptest::buildZip;
using ziptest::bytesOf;
using ziptest::TestEntry;

namespace {

constexpr const char* kManifest = R"(<?xml version="1.0" encoding="UTF-8"?>
<manifest:manifest xmlns:manifest="urn:oasis:names:tc:opendocument:xmlns:manifest:1.0" manifest:version="1.2">
 <manifest:file-entry manifest:media-type="application/x-krita-resourcebundle" manifest:full-path="/"/>
 <manifest:file-entry manifest:media-type="brushes" manifest:full-path="brushes/tip.gbr" manifest:md5sum="44410ba6468d1f7632f3bc7bfde365b1"/>
 <manifest:file-entry manifest:media-type="paintoppresets" manifest:full-path="paintoppresets/a.kpp" manifest:md5sum="c605fb1974497a2974efa42352a88abe"/>
</manifest:manifest>)";

constexpr const char* kMeta = R"(<?xml version="1.0" encoding="UTF-8"?>
<meta:meta>
 <meta:generator>Krita (4.1.0-pre-alpha (git 292cad9))</meta:generator>
 <dc:author>Deevad and others.</dc:author>
 <dc:description>The test set.</dc:description>
 <meta:initial-creator>Deevad and others.</meta:initial-creator>
 <dc:creator>Deevad and others.</dc:creator>
 <meta:creation-date>16/03/2018</meta:creation-date>
 <meta:meta-userdefined meta:name="email" meta:value="foundation@krita.org"/>
 <meta:meta-userdefined meta:name="license" meta:value="CC-0"/>
 <meta:meta-userdefined meta:name="website" meta:value="http://krita.org"/>
</meta:meta>)";

[[nodiscard]] std::vector<TestEntry> wellFormedEntries() {
    return {
        {"mimetype", bytesOf("application/x-krita-resourcebundle"), false},
        {"META-INF/", {}, false},
        {"META-INF/manifest.xml", bytesOf(kManifest), true},
        {"meta.xml", bytesOf(kMeta), true},
        {"preview.png", bytesOf("not really a png"), false},
        {"brushes/", {}, false},
        {"brushes/tip.gbr", bytesOf("GIMP brush bytes"), true},
        {"paintoppresets/", {}, false},
        {"paintoppresets/a.kpp", bytesOf("kpp bytes"), true},
    };
}

[[nodiscard]] const BundleResource* byPath(const Bundle& b, std::string_view path) {
    for (const BundleResource& r : b.resources())
        if (r.fullPath == path)
            return &r;
    return nullptr;
}

} // namespace

TEST_CASE("bundle: a well-formed bundle enumerates its payload with manifest claims attached") {
    const auto zip = buildZip(wellFormedEntries());
    std::string error;
    auto bundle = Bundle::open(zip.data(), zip.size(), &error);
    REQUIRE_MESSAGE(bundle.has_value(), error);

    CHECK(bundle->hasManifest());
    CHECK(bundle->hasMeta());
    CHECK(bundle->manifestOnlyRows() == 0);
    CHECK(bundle->unlistedResources() == 0);
    CHECK(bundle->skippedManifestRows() == 0);

    // Payload only: no mimetype/meta/preview/META-INF, no directory placeholders.
    REQUIRE(bundle->resources().size() == 2);

    const BundleResource* tip = byPath(*bundle, "brushes/tip.gbr");
    REQUIRE(tip != nullptr);
    CHECK(tip->mediaType == "brushes");
    CHECK(tip->fileName == "tip.gbr");
    CHECK(tip->inManifest);
    CHECK(tip->manifestMd5 == "44410ba6468d1f7632f3bc7bfde365b1");

    const BundleResource* preset = byPath(*bundle, "paintoppresets/a.kpp");
    REQUIRE(preset != nullptr);
    CHECK(preset->mediaType == "paintoppresets");

    auto bytes = bundle->read(*tip, &error);
    REQUIRE_MESSAGE(bytes.has_value(), error);
    CHECK(*bytes == bytesOf("GIMP brush bytes"));
}

TEST_CASE("bundle: meta.xml attribution is preserved verbatim") {
    const auto zip = buildZip(wellFormedEntries());
    auto bundle = Bundle::open(zip.data(), zip.size());
    REQUIRE(bundle.has_value());
    CHECK(bundle->meta().generator == "Krita (4.1.0-pre-alpha (git 292cad9))");
    CHECK(bundle->meta().author == "Deevad and others.");
    CHECK(bundle->meta().description == "The test set.");
    CHECK(bundle->meta().initialCreator == "Deevad and others.");
    CHECK(bundle->meta().creator == "Deevad and others.");
    CHECK(bundle->meta().date == "16/03/2018");
    CHECK(bundle->meta().license == "CC-0");
    CHECK(bundle->meta().website == "http://krita.org");
    CHECK(bundle->meta().email == "foundation@krita.org");
}

TEST_CASE("bundle: the mimetype entry is the format signature") {
    std::string error;

    SUBCASE("missing") {
        auto entries = wellFormedEntries();
        entries.erase(entries.begin()); // drop mimetype
        const auto zip = buildZip(entries);
        CHECK(!Bundle::open(zip.data(), zip.size(), &error).has_value());
        CHECK(error.find("mimetype") != std::string::npos);
    }
    SUBCASE("foreign value") {
        auto entries = wellFormedEntries();
        entries[0].data = bytesOf("application/epub+zip");
        const auto zip = buildZip(entries);
        CHECK(!Bundle::open(zip.data(), zip.size(), &error).has_value());
        CHECK(error.find("foreign") != std::string::npos);
    }
    SUBCASE("deflated and mid-archive is fine -- the RGBA_brushes shape") {
        auto entries = wellFormedEntries();
        TestEntry mime = entries[0];
        mime.deflate = true;
        entries.erase(entries.begin());
        entries.push_back(mime); // last entry, compressed
        const auto zip = buildZip(entries);
        CHECK(Bundle::open(zip.data(), zip.size(), &error).has_value());
    }
}

TEST_CASE("bundle: duplicate manifest rows collapse to the last, counted") {
    auto entries = wellFormedEntries();
    // The Krita_4 shape: the same full-path listed twice. Ours disagree on md5 so the test can
    // SEE which row won -- the shipped duplicates happen to agree.
    entries[2].data = bytesOf(R"(<?xml version="1.0"?>
<manifest:manifest xmlns:manifest="urn:oasis:names:tc:opendocument:xmlns:manifest:1.0">
 <manifest:file-entry manifest:media-type="brushes" manifest:full-path="brushes/tip.gbr" manifest:md5sum="first"/>
 <manifest:file-entry manifest:media-type="brushes" manifest:full-path="brushes/tip.gbr" manifest:md5sum="second"/>
 <manifest:file-entry manifest:media-type="paintoppresets" manifest:full-path="paintoppresets/a.kpp"/>
</manifest:manifest>)");
    const auto zip = buildZip(entries);
    auto bundle = Bundle::open(zip.data(), zip.size());
    REQUIRE(bundle.has_value());
    CHECK(bundle->duplicateManifestRows() == 1);
    CHECK(bundle->manifestOnlyRows() == 0); // a duplicate is not a dangling claim
    const BundleResource* tip = byPath(*bundle, "brushes/tip.gbr");
    REQUIRE(tip != nullptr);
    CHECK(tip->manifestMd5 == "second");
}

TEST_CASE("bundle: manifest discrepancies are counted in both directions") {
    SUBCASE("a manifest row with no file behind it") {
        auto entries = wellFormedEntries();
        entries[6].name = "brushes/renamed.gbr"; // the manifest still says tip.gbr
        const auto zip = buildZip(entries);
        auto bundle = Bundle::open(zip.data(), zip.size());
        REQUIRE(bundle.has_value());
        CHECK(bundle->manifestOnlyRows() == 1);
        CHECK(bundle->unlistedResources() == 1); // and the renamed file is unlisted
        const BundleResource* renamed = byPath(*bundle, "brushes/renamed.gbr");
        REQUIRE(renamed != nullptr);
        CHECK(!renamed->inManifest);
        CHECK(renamed->manifestMd5.empty());
    }
    SUBCASE("a file no manifest row lists") {
        auto entries = wellFormedEntries();
        entries.push_back({"patterns/extra.pat", bytesOf("pattern"), true});
        const auto zip = buildZip(entries);
        auto bundle = Bundle::open(zip.data(), zip.size());
        REQUIRE(bundle.has_value());
        CHECK(bundle->manifestOnlyRows() == 0);
        CHECK(bundle->unlistedResources() == 1);
        CHECK(bundle->resources().size() == 3);
    }
}

TEST_CASE("bundle: a missing manifest or meta.xml degrades to counters, never an error") {
    auto entries = wellFormedEntries();
    entries.erase(entries.begin() + 2, entries.begin() + 4); // manifest.xml + meta.xml
    const auto zip = buildZip(entries);
    auto bundle = Bundle::open(zip.data(), zip.size());
    REQUIRE(bundle.has_value());
    CHECK(!bundle->hasManifest());
    CHECK(!bundle->hasMeta());
    CHECK(bundle->meta().license.empty());
    CHECK(bundle->resources().size() == 2);
    // With no manifest there is nothing to be unlisted FROM.
    CHECK(bundle->unlistedResources() == 0);
}

TEST_CASE("bundle: manifest quirks -- root row, leading slash, prefix choice, pathless rows") {
    // A conforming producer that picked its own namespace prefix, wrote full-paths with a
    // leading slash, and emitted one row with no full-path at all.
    constexpr const char* manifest = R"(<?xml version="1.0"?>
<m:manifest xmlns:m="urn:oasis:names:tc:opendocument:xmlns:manifest:1.0">
 <m:file-entry m:media-type="application/x-krita-resourcebundle" m:full-path="/"/>
 <m:file-entry m:media-type="brushes" m:full-path="/brushes/tip.gbr" m:md5sum="abc123"/>
 <m:file-entry m:media-type="brushes"/>
</m:manifest>)";
    auto entries = wellFormedEntries();
    entries[2].data = bytesOf(manifest);
    const auto zip = buildZip(entries);
    auto bundle = Bundle::open(zip.data(), zip.size());
    REQUIRE(bundle.has_value());
    CHECK(bundle->hasManifest());
    CHECK(bundle->skippedManifestRows() == 1);
    const BundleResource* tip = byPath(*bundle, "brushes/tip.gbr");
    REQUIRE(tip != nullptr);
    CHECK(tip->inManifest);
    CHECK(tip->manifestMd5 == "abc123");
    // The root row is the bundle itself, not a dangling claim; a.kpp is now unlisted.
    CHECK(bundle->manifestOnlyRows() == 0);
    CHECK(bundle->unlistedResources() == 1);
}

TEST_CASE("bundle: not a zip fails with the zip layer's reason") {
    const auto junk = bytesOf("not a zip at all, not even close, needs 22 bytes");
    std::string error;
    CHECK(!Bundle::open(junk.data(), junk.size(), &error).has_value());
    CHECK(error.find("end-of-central-directory") != std::string::npos);
}
