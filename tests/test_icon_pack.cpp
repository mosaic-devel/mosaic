#include "ui/icon_pack.hpp"

#include "common/image_svg.hpp"
#include "io/io.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>

// The tool icon-pack system (ui/icon_pack.hpp, S52): the embedded default pack ("Smalti") as a
// census -- every key present, rasterizing cleanly at the toolbar's 20 px, and COLORFUL (the
// PLAN §3.13 identity is a hard rule: never flat monochrome) -- plus the folder-pack scanner's
// honesty (manifest = the pack marker; bad folders rejected and counted) and the per-icon
// fallback contract (a one-icon pack is a legitimate pack).
namespace {

using namespace mosaic::ui;
namespace fs = std::filesystem;

fs::path scratchDir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / "mosaic_icon_pack_tests" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    REQUIRE(f.good());
    f << text;
}

constexpr const char* kCandyBrush =
    R"(<svg viewBox="0 0 24 24"><rect x="4" y="4" width="16" height="16" fill="#FF00AA"/></svg>)";

constexpr const char* kCandyManifest = R"({
  "name": "Candy",
  "artist": "Test Artist",
  "link": "candy@example.test",
  "description": "One brush, everything else falls back.",
  "license": "CC0-1.0",
  "schema": 1
})";

// Rasterize and measure: returns (distinct opaque colours, covered-pixel fraction).
std::pair<int, double> measure(const std::string& svg, int px) {
    std::string err;
    const auto img = mosaic::common::rasterizeSvg(
        reinterpret_cast<const unsigned char*>(svg.data()), svg.size(), px, px, &err);
    REQUIRE_MESSAGE(!img.empty(), err);
    std::set<std::uint32_t> colors;
    int covered = 0;
    for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        if (img.rgba[i + 3] < 128)
            continue;
        ++covered;
        // Quantize to 4 bits/channel so AA shades of one paint don't count as palette breadth.
        colors.insert(static_cast<std::uint32_t>(img.rgba[i] >> 4) << 8
                      | static_cast<std::uint32_t>(img.rgba[i + 1] >> 4) << 4
                      | static_cast<std::uint32_t>(img.rgba[i + 2] >> 4));
    }
    return {static_cast<int>(colors.size()),
            static_cast<double>(covered) / (static_cast<double>(px) * px)};
}

} // namespace

// ------------------------------------------------------------------------------------------------

TEST_CASE("icon pack: every tool has a stable key inside the known set") {
    const auto& keys = allIconKeys();
    // 22 implemented + 10 PLAN-named + the magnetic lasso, plus the 6 S26-c shape kinds
    // (callout/arrow/ring/cross/heart/banner).
    CHECK(keys.size() == 39);
    CHECK(std::set<std::string>(keys.begin(), keys.end()).size() == keys.size());
    for (const char* future : {"lasso_magnetic", "blur", "dodge",
                               "burn", "smudge", "pen_path", "heal", "red_eye", "clone_stamp",
                               "hand", "warp"})
        CHECK_MESSAGE(std::find(keys.begin(), keys.end(), future) != keys.end(), future);

    for (int i = 0; i <= static_cast<int>(ToolId::Zoom); ++i) {
        const auto id = static_cast<ToolId>(i);
        const std::string key(iconKeyFor(id));
        CHECK_MESSAGE(std::find(keys.begin(), keys.end(), key) != keys.end(), key);
        // The registration baseline: the default art a Tool is born with is the pack's.
        IconPacks packs;
        CHECK(std::string(defaultIconSvg(id))
              == packs.iconFor(kDefaultIconPackId, iconKeyFor(id)).svg);
    }
}

TEST_CASE("icon pack: the embedded default pack census -- complete, crisp, colorful") {
    IconPacks packs;
    REQUIRE(packs.packs().size() == 1);
    const IconPackInfo& def = *packs.find(kDefaultIconPackId);
    CHECK(def.builtin);
    CHECK(def.name == "Smalti");
    // The set is built on the GIMP color tool icons since 2026-07-10 (docs/credits.md).
    CHECK(def.artist.find("GIMP") != std::string::npos);
    CHECK_FALSE(def.link.empty());
    CHECK_FALSE(def.description.empty());

    // The art is mixed-grid now (GIMP's 16, apple_cursor's 256, mm-unit exports), but every icon
    // must still be authored SQUARE -- scaleToFit letterboxes anything else into blank margins.
    const auto squareViewBox = [](const std::string& svg) {
        const auto at = svg.find("viewBox=\"");
        if (at == std::string::npos)
            return false;
        double x = 0, y = 0, w = 0, h = 0;
        if (std::sscanf(svg.c_str() + at + 9, "%lf %lf %lf %lf", &x, &y, &w, &h) != 4)
            return false;
        return w > 0 && h > 0 && std::abs(w - h) <= 0.01 * std::max(w, h);
    };

    for (const std::string& key : allIconKeys()) {
        const std::string& svg = packs.iconFor(kDefaultIconPackId, key).svg;
        REQUIRE_MESSAGE(!svg.empty(), key);
        CHECK_MESSAGE(squareViewBox(svg), key);
        // No <text> elements: nanosvg silently drops them (the reader would show a hole).
        CHECK_MESSAGE(svg.find("<text") == std::string::npos, key);
        const auto [colors, coverage] = measure(svg, 20);
        // Colorful is the identity, not a preference (PLAN §3.13): at least two distinct opaque
        // paints at the toolbar's own raster size -- a one-ink glyph fails its family colour +
        // outline construction by definition.
        CHECK_MESSAGE(colors >= 2, key << ": " << colors << " colours");
        // Legible: a timid speck (or a full-bleed slab) does not read as a tool at 20 px.
        CHECK_MESSAGE(coverage > 0.10, key << ": coverage " << coverage);
        CHECK_MESSAGE(coverage < 0.95, key << ": coverage " << coverage);
    }
}

TEST_CASE("icon pack: folder scan honesty and the per-icon fallback") {
    const fs::path root = scratchDir("packs");

    // A one-icon pack: brush.svg + manifest. Legitimate -- everything else falls back.
    fs::create_directories(root / "candy");
    write(root / "candy" / std::string(kIconPackManifest), kCandyManifest);
    write(root / "candy" / "brush.svg", kCandyBrush);
    // An ordinary folder (no manifest): not a pack, not an error.
    fs::create_directories(root / "not_a_pack");
    write(root / "not_a_pack" / "readme.txt", "nothing to see");
    // A broken manifest: rejected AND counted.
    fs::create_directories(root / "broken");
    write(root / "broken" / std::string(kIconPackManifest), "{nope");
    // A folder trying to shadow the built-in id: rejected.
    fs::create_directories(root / "default");
    write(root / "default" / std::string(kIconPackManifest), kCandyManifest);

    IconPacks packs;
    CHECK(packs.scan(root) == 2); // broken + the reserved id
    REQUIRE(packs.packs().size() == 2);
    const IconPackInfo* candy = packs.find("candy");
    REQUIRE(candy != nullptr);
    CHECK_FALSE(candy->builtin);
    CHECK(candy->name == "Candy");
    CHECK(candy->artist == "Test Artist");
    CHECK(candy->link == "candy@example.test");

    // The pack's own icon wins; every missing icon falls back to the default PER ICON.
    CHECK(packs.iconFor("candy", "brush").svg == kCandyBrush);
    CHECK(packs.iconFor("candy", "zoom").svg == packs.iconFor(kDefaultIconPackId, "zoom").svg);
    CHECK_FALSE(packs.iconFor("candy", "zoom").empty());
    // An unknown pack renders as the default; a foreign key is empty even there.
    CHECK(packs.iconFor("no_such_pack", "brush").svg
          == packs.iconFor(kDefaultIconPackId, "brush").svg);
    CHECK(packs.iconFor(kDefaultIconPackId, "no_such_tool").empty());

    // Oversized icon files fall back too (a hostile pack is just a folder of third-party files).
    write(root / "candy" / "zoom.svg", std::string(kMaxIconFileBytes + 1, ' '));
    IconPacks fresh;
    fresh.scan(root);
    CHECK(fresh.iconFor("candy", "zoom").svg == fresh.iconFor(kDefaultIconPackId, "zoom").svg);

    // A rescan replaces the previous scan's packs; the built-in default always survives.
    IconPacks rescanned;
    rescanned.scan(root);
    CHECK(rescanned.packs().size() == 2);
    rescanned.scan(root / "not_a_pack"); // a dir with no packs at all
    REQUIRE(rescanned.packs().size() == 1);
    CHECK(rescanned.packs().front().id == kDefaultIconPackId);
    // And a missing directory is zero packs, not an error.
    CHECK(rescanned.scan(root / "does_not_exist") == 0);

    fs::remove_all(fs::temp_directory_path() / "mosaic_icon_pack_tests");
}

TEST_CASE("icon pack: manifest fields are optional, the manifest itself is not") {
    const fs::path root = scratchDir("sparse");
    fs::create_directories(root / "sparse");
    write(root / "sparse" / std::string(kIconPackManifest), R"({"name":"Sparse"})");

    IconPacks packs;
    CHECK(packs.scan(root) == 0);
    const IconPackInfo* sparse = packs.find("sparse");
    REQUIRE(sparse != nullptr);
    CHECK(sparse->name == "Sparse");
    CHECK(sparse->artist.empty());
    CHECK(sparse->link.empty());

    // A manifest that parses but is not an object is a rejection, not a default-named pack.
    write(root / "sparse" / std::string(kIconPackManifest), "[1,2,3]");
    IconPacks fresh;
    CHECK(fresh.scan(root) == 1);
    CHECK(fresh.find("sparse") == nullptr);

    // A manifest with a foreign name TYPE keeps the folder name.
    write(root / "sparse" / std::string(kIconPackManifest), R"({"name": 7})");
    IconPacks named;
    CHECK(named.scan(root) == 0);
    REQUIRE(named.find("sparse") != nullptr);
    CHECK(named.find("sparse")->name == "sparse");

    fs::remove_all(fs::temp_directory_path() / "mosaic_icon_pack_tests");
}

TEST_CASE("icon pack: raster (PNG) icons render through the same pipeline") {
    const fs::path root = scratchDir("raster");
    fs::create_directories(root / "rpack");
    write(root / "rpack" / std::string(kIconPackManifest), R"({"name":"Raster"})");

    // A solid magenta 8x8 PNG as the pack's brush.
    mosaic::common::Image magenta(8, 8);
    for (std::size_t i = 0; i < magenta.pixelCount(); ++i) {
        magenta.rgba[i * 4 + 0] = 255;
        magenta.rgba[i * 4 + 1] = 0;
        magenta.rgba[i * 4 + 2] = 220;
        magenta.rgba[i * 4 + 3] = 255;
    }
    std::string err;
    const auto png = mosaic::io::encodePng(magenta, {}, &err);
    REQUIRE_MESSAGE(png.has_value(), err);
    {
        std::ofstream f(root / "rpack" / "brush.png", std::ios::binary);
        f.write(reinterpret_cast<const char*>(png->data()),
                static_cast<std::streamsize>(png->size()));
    }

    IconPacks packs;
    CHECK(packs.scan(root) == 0);
    const IconSource& brush = packs.iconFor("rpack", "brush");
    CHECK(brush.svg.empty());
    CHECK_FALSE(brush.png.empty());

    // Renders at any px: solid source -> solid square, opaque, the right colour.
    const auto img = packs.renderIcon("rpack", "brush", 20);
    REQUIRE_FALSE(img.empty());
    CHECK(img.width == 20);
    CHECK(img.height == 20);
    for (const std::size_t at : {std::size_t{0}, (std::size_t{10} * 20 + 10) * 4,
                                 (std::size_t{19} * 20 + 19) * 4}) {
        CHECK(img.rgba[at + 0] == 255);
        CHECK(img.rgba[at + 2] == 220);
        CHECK(img.rgba[at + 3] == 255);
    }

    // Vector wins when a pack ships both spellings of one icon.
    write(root / "rpack" / "brush.svg", kCandyBrush);
    IconPacks both;
    both.scan(root);
    CHECK(both.iconFor("rpack", "brush").svg == kCandyBrush);
    CHECK(both.iconFor("rpack", "brush").png.empty());

    // An undecodable PNG falls back to the default at RENDER time, not to a broken button.
    write(root / "rpack" / "zoom.png", "not a png at all");
    IconPacks broken;
    broken.scan(root);
    CHECK_FALSE(broken.iconFor("rpack", "zoom").png.empty()); // the bytes do resolve...
    const auto fallback = broken.renderIcon("rpack", "zoom", 20);
    CHECK(fallback.empty()); // ...but render fails loudly; the CALLER decides (toolbar warns)

    fs::remove_all(fs::temp_directory_path() / "mosaic_icon_pack_tests");
}

TEST_CASE("icon pack: raster scaling is alpha-weighted and letterboxed") {
    // One red opaque pixel among transparent BLACK neighbours: a straight average would darken
    // the red by 4x; alpha weighting keeps the paint's own colour and averages only coverage.
    mosaic::common::Image quad(2, 2);
    quad.rgba = {255, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::string err;
    const auto png = mosaic::io::encodePng(quad, {}, &err);
    REQUIRE_MESSAGE(png.has_value(), err);
    IconSource src;
    src.png.assign(png->begin(), png->end());
    const auto one = renderIconSource(src, 1, &err);
    REQUIRE_MESSAGE(!one.empty(), err);
    CHECK(one.rgba[0] == 255);                       // the red survives undiluted
    CHECK(one.rgba[3] > 55);                         // ~a quarter covered
    CHECK(one.rgba[3] < 75);

    // A wide source letterboxes: 4x2 into an 8-square lands rows 2..5, transparent margins.
    mosaic::common::Image wide(4, 2);
    for (std::size_t i = 0; i < wide.pixelCount(); ++i) {
        wide.rgba[i * 4 + 1] = 255; // green
        wide.rgba[i * 4 + 3] = 255;
    }
    const auto pngWide = mosaic::io::encodePng(wide, {}, &err);
    REQUIRE(pngWide.has_value());
    IconSource wideSrc;
    wideSrc.png.assign(pngWide->begin(), pngWide->end());
    const auto boxed = renderIconSource(wideSrc, 8, &err);
    REQUIRE_MESSAGE(!boxed.empty(), err);
    CHECK(boxed.rgba[(std::size_t{0} * 8 + 4) * 4 + 3] == 0);   // top margin transparent
    CHECK(boxed.rgba[(std::size_t{4} * 8 + 4) * 4 + 3] == 255); // content opaque green
    CHECK(boxed.rgba[(std::size_t{4} * 8 + 4) * 4 + 1] == 255);
    CHECK(boxed.rgba[(std::size_t{7} * 8 + 4) * 4 + 3] == 0);   // bottom margin transparent
}
