// The preset library (io/brush/library.{hpp,cpp}): scan a bundle, resolve tip references, and
// finish the two decisions the mapper had to stage (preset.hpp) -- the §3.5 application rule,
// whose input is the tip IMAGE's content, and the accumulator that follows.
//
// The bundle here is assembled in-test from the committed real fixtures (fairy-dust.gih,
// vegetal.gbr, hair.png, Basic-5) plus synthetic presets aimed at the resolution matrix:
// md5-beats-filename (the upstream bestMatch order), filename fallback with the md5-mismatch
// note, the usable-fallback round tip, and the LegacyContentTest re-run that only a loaded
// colourful tip can trigger. The census against the real shipped bundle lives in
// test_brush_library_census.cpp; these cases own the RULES.

#include "io/brush/library.hpp"

#include "io/brush/md5.hpp"
#include "kpp_builder.hpp"
#include "zip_builder.hpp"

#include <doctest/doctest.h>

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace mosaic::io::brush;
namespace cb = mosaic::core::brush;
namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE_MESSAGE(f.good(), path);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(f.tellg()));
    f.seekg(0);
    REQUIRE(f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()))
                .good());
    return buf;
}

std::vector<std::uint8_t> fixture(const char* name) {
    return readFile(std::string(MOSAIC_FIXTURE_DIR) + "/brush/" + name);
}

void push32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int s = 24; s >= 0; s -= 8)
        v.push_back(static_cast<std::uint8_t>(x >> s));
}

// A 1x1 bytes=4 GBR whose pixel is `rgba`. With a non-grey pixel its content verdict is
// hasColorAndTransparency = true (the gbr-4 rule: any non-grey pixel, alpha ignored).
std::vector<std::uint8_t> syntheticGbr(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                       std::uint8_t a) {
    std::vector<std::uint8_t> v;
    push32(v, 29); // header_size: 28 + the empty name's NUL
    push32(v, 2);  // version
    push32(v, 1);  // width
    push32(v, 1);  // height
    push32(v, 4);  // bytes: RGBA
    const char* magic = "GIMP";
    v.insert(v.end(), magic, magic + 4);
    push32(v, 20); // spacing %
    v.push_back(0);
    v.push_back(r);
    v.push_back(g);
    v.push_back(b);
    v.push_back(a);
    return v;
}

// A pixel-brush preset whose tip is a predefined reference.
std::string predefinedPreset(const std::string& name, const std::string& filename,
                             const std::string& md5, const std::string& brushAttrs = "",
                             const std::string& extraParams = "",
                             const std::string& tipType = "gbr_brush") {
    std::string md5Attr = md5.empty() ? "" : (" md5sum=\"" + md5 + "\"");
    return "<Preset name=\"" + name + "\" paintopid=\"paintbrush\">"
           "<param name=\"brush_definition\" type=\"string\"><![CDATA[<Brush type=\"" + tipType +
           "\" BrushVersion=\"2\" filename=\"" + filename + "\"" + md5Attr + " spacing=\"0.1\"" +
           brushAttrs + "/>]]></param>" + extraParams + "</Preset>";
}

// The synthetic presets below say nothing about Opacity -- and the format reads an absent Opacity
// as a LIVE pressure ramp (§3.2). Under WASH (all of these) the engine drives that end-to-end now,
// so no standing note remains; the filter below is kept because a BUILDUP preset would still carry
// "Opacity (dynamic under Buildup; ...)" and these cases are about TIP RESOLUTION, not about the
// direct path's untranscribed composite.
[[nodiscard]] std::vector<std::string> otherDrops(const LibraryPreset& p) {
    std::vector<std::string> out;
    for (const std::string& d : p.preset.provenance.droppedOptions)
        if (d.rfind("Opacity (dynamic", 0) != 0)
            out.push_back(d);
    return out;
}

[[nodiscard]] const LibraryPreset* byName(const PresetLibrary& lib, std::string_view name) {
    for (const LibraryPreset& p : lib.presets())
        if (p.preset.name == name)
            return &p;
    return nullptr;
}

// The bundle every case below opens: fixtures + the synthetic matrix.
std::vector<std::uint8_t> testBundle() {
    const std::string vegetalMd5 = [] {
        const auto bytes = fixture("vegetal.gbr");
        return md5Hex(bytes.data(), bytes.size());
    }();

    std::vector<ziptest::TestEntry> entries;
    entries.push_back({"mimetype", ziptest::bytesOf("application/x-krita-resourcebundle"), false});
    entries.push_back({"brushes/fairy-dust.gih", fixture("fairy-dust.gih"), true});
    entries.push_back({"brushes/vegetal.gbr", fixture("vegetal.gbr"), true});
    entries.push_back({"brushes/hair.png", fixture("hair.png"), true});
    entries.push_back({"brushes/colorful.gbr", syntheticGbr(255, 0, 0, 255), true});
    // A decoy sharing the md5-priority preset's FILENAME but not its md5 (grey pixel: different
    // bytes, and a grey content verdict for good measure).
    entries.push_back({"brushes/decoy.gbr", syntheticGbr(128, 128, 128, 255), true});
    entries.push_back({"brushes/broken.gih", ziptest::bytesOf("not a gih at all"), true});
    // A 2:1 svg, black over the left half: pins the aspect-derived height AND the
    // no-inversion rule (rendered-over-white IS the tip image).
    entries.push_back({"brushes/half.svg",
                       ziptest::bytesOf("<svg xmlns=\"http://www.w3.org/2000/svg\" "
                                        "viewBox=\"0 0 2 1\"><rect x=\"0\" y=\"0\" width=\"1\" "
                                        "height=\"1\" fill=\"black\"/></svg>"),
                       true});

    entries.push_back({"paintoppresets/basic5.kpp", fixture("b)_Basic-5_Size_default.kpp"), true});
    const auto addKpp = [&entries](const char* file, const std::string& xml) {
        entries.push_back({std::string("paintoppresets/") + file,
                           kpptest::syntheticKpp("2.2", xml), true});
    };
    // md5 priority: the filename names the decoy; the md5 names vegetal. md5 must win -- and it
    // is claimed in UPPERCASE, because two rows of Krita_4's own manifest are: hex comparison
    // must be case-insensitive.
    std::string vegetalMd5Upper = vegetalMd5;
    for (char& c : vegetalMd5Upper)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    addKpp("md5wins.kpp", predefinedPreset("md5wins", "decoy.gbr", vegetalMd5Upper));
    // filename fallback: an md5 nothing matches + a filename that exists -> resolved + noted.
    addKpp("md5miss.kpp", predefinedPreset("md5miss", "fairy-dust.gih",
                                           "00000000000000000000000000000000"));
    // no md5 at all (the entire 2.2 corpus): plain filename resolution, no note.
    addKpp("byname.kpp", predefinedPreset("byname", "fairy-dust.gih", ""));
    // nothing matches: the usable fallback.
    addKpp("orphan.kpp", predefinedPreset("orphan", "no-such-tip.gbr", ""));
    // matched but undecodable: also the fallback, plus the decode-failure counter.
    addKpp("broken.kpp", predefinedPreset("broken", "broken.gih", ""));
    // The LegacyContentTest re-run: no application attributes at all means colorAsMask = false
    // (the pre-4.4 heuristic), so a colour+transparency tip becomes an ImageStamp...
    addKpp("legacy_colorful.kpp", predefinedPreset("legacy_colorful", "colorful.gbr", ""));
    // ...unless ColorAsMask asserts mask-ness.
    addKpp("legacy_masked.kpp",
           predefinedPreset("legacy_masked", "colorful.gbr", "", " ColorAsMask=\"true\""));
    // The svg tip path, with an aspect the square census tip cannot pin.
    addKpp("svghalf.kpp", predefinedPreset("svghalf", "half.svg", "", "", "", "svg_brush"));
    // Masking master size: primary = fairy-dust at scale 2, masking at half master size.
    addKpp("maskmaster.kpp",
           predefinedPreset(
               "maskmaster", "fairy-dust.gih", "", " scale=\"2\"",
               "<param name=\"MaskingBrush/Enabled\" type=\"internal\">true</param>"
               "<param name=\"MaskingBrush/MaskingCompositeOp\" type=\"internal\">multiply</param>"
               "<param name=\"MaskingBrush/UseMasterSize\" type=\"internal\">true</param>"
               "<param name=\"MaskingBrush/MasterSizeCoeff\" type=\"internal\">0.5</param>"
               "<param name=\"MaskingBrush/Preset/brush_definition\" type=\"string\">"
               "<![CDATA[<Brush type=\"auto_brush\" BrushVersion=\"2\" spacing=\"0.2\">"
               "<MaskGenerator diameter=\"10\" type=\"circle\" id=\"default\" hfade=\"1\" "
               "vfade=\"1\"/></Brush>]]></param>"));
    // The masking brush's own PREDEFINED tip: resolves through the same chain as the primary's.
    addKpp("masktip.kpp",
           predefinedPreset(
               "masktip", "vegetal.gbr", "", "",
               "<param name=\"MaskingBrush/Enabled\" type=\"internal\">true</param>"
               "<param name=\"MaskingBrush/MaskingCompositeOp\" type=\"internal\">subtract</param>"
               "<param name=\"MaskingBrush/Preset/brush_definition\" type=\"string\">"
               "<![CDATA[<Brush type=\"png_brush\" BrushVersion=\"2\" spacing=\"0.3\" "
               "filename=\"hair.png\"/>]]></param>"));
    // ... and a nested reference matching NOTHING: the analytic disc, badged, counted.
    addKpp("maskorphan.kpp",
           predefinedPreset(
               "maskorphan", "vegetal.gbr", "", "",
               "<param name=\"MaskingBrush/Enabled\" type=\"internal\">true</param>"
               "<param name=\"MaskingBrush/MaskingCompositeOp\" type=\"internal\">subtract</param>"
               "<param name=\"MaskingBrush/Preset/brush_definition\" type=\"string\">"
               "<![CDATA[<Brush type=\"gbr_brush\" BrushVersion=\"2\" spacing=\"0.3\" "
               "filename=\"no-such-mask.gbr\"/>]]></param>"));
    return ziptest::buildZip(entries);
}

} // namespace

TEST_CASE("library: the synthetic bundle loads whole -- every preset usable, counters exact") {
    const auto zip = testBundle();
    PresetLibrary lib;
    std::string error;
    const int added = lib.addBundle(zip.data(), zip.size(), "test://bundle", &error);
    REQUIRE_MESSAGE(added == 12, error);

    const LibraryCounters& c = lib.counters();
    CHECK(c.presetsLoaded == 12);
    CHECK(c.presetsFailed == 0);
    CHECK(c.tipsResolvedByMd5 == 1);      // md5wins
    CHECK(c.tipsResolvedByFilename == 8); // md5miss byname legacy_* maskmaster svghalf masktip maskorphan
    CHECK(c.tipsFallback == 2);           // orphan broken
    CHECK(c.tipDecodeFailures == 1);      // broken.gih
    CHECK(c.maskingTipsResolved == 1);    // masktip's nested hair.png
    CHECK(c.maskingTipsFallback == 1);    // maskorphan's nested no-such-mask.gbr
    CHECK(c.scanBudgetSkips == 0);

    // Attribution rides along even though this bundle has no meta.xml.
    REQUIRE(lib.sources().size() == 1);
    CHECK(lib.sources()[0].path == "test://bundle");
    CHECK(lib.sources()[0].meta.license.empty());

    for (const LibraryPreset& p : lib.presets())
        CHECK(p.preset.provenance.sourceFormat == "bundle");
}

TEST_CASE("library: resolution order is md5 first, filename second -- the upstream bestMatch") {
    const auto zip = testBundle();
    PresetLibrary lib;
    REQUIRE(lib.addBundle(zip.data(), zip.size(), "test://bundle") == 12);

    SUBCASE("an md5 hit wins over the filename naming a different file") {
        const LibraryPreset* p = byName(lib, "md5wins");
        REQUIRE(p != nullptr);
        CHECK(p->tipResolution == TipResolution::ByMd5);
        CHECK(p->tipFileName == "vegetal.gbr"); // NOT the decoy the filename named
        REQUIRE(p->tip != nullptr);
        CHECK(otherDrops(*p).empty()); // no note: the md5 told the truth
    }
    SUBCASE("an md5 miss falls back to the filename, and says so") {
        const LibraryPreset* p = byName(lib, "md5miss");
        REQUIRE(p != nullptr);
        CHECK(p->tipResolution == TipResolution::ByFilename);
        CHECK(p->tipFileName == "fairy-dust.gih");
        REQUIRE(p->tip != nullptr);
        REQUIRE(otherDrops(*p).size() == 1);
        CHECK(otherDrops(*p)[0].find("md5 mismatch") != std::string::npos);
        // A resolution note is honesty, not a fidelity loss: the preset paints exactly as well as one
        // whose md5 told the truth -- which is what this compares it against, rather than a bare
        // `== Exact` that would really be asserting something about its Opacity option.
        const LibraryPreset* clean = byName(lib, "md5wins");
        REQUIRE(clean != nullptr);
        CHECK(p->preset.provenance.fidelity == clean->preset.provenance.fidelity);
    }
    SUBCASE("no md5 claimed (the whole 2.2 corpus): filename resolution, no note") {
        const LibraryPreset* p = byName(lib, "byname");
        REQUIRE(p != nullptr);
        CHECK(p->tipResolution == TipResolution::ByFilename);
        REQUIRE(p->tip != nullptr);
        CHECK(p->tip->hose().dim == 1); // fairy-dust is a hose
        CHECK(otherDrops(*p).empty());
    }
}

TEST_CASE("library: an unresolvable or undecodable tip leaves a USABLE preset, badged") {
    const auto zip = testBundle();
    PresetLibrary lib;
    REQUIRE(lib.addBundle(zip.data(), zip.size(), "test://bundle") == 12);

    for (const char* name : {"orphan", "broken"}) {
        CAPTURE(name);
        const LibraryPreset* p = byName(lib, name);
        REQUIRE(p != nullptr);
        CHECK(p->tipResolution == TipResolution::Fallback);
        CHECK(p->tip == nullptr);
        // The mapper's fallback shape: a default round auto tip the engine can walk today.
        CHECK(p->preset.tip.kind == TipXml::Kind::Auto);
        CHECK(p->preset.tip.autoTip.generator.diameter == 24.0);
        CHECK(p->preset.provenance.fidelity == PresetFidelity::Approximated);
        REQUIRE(otherDrops(*p).size() == 1);
        CHECK(otherDrops(*p)[0].find("default round tip") != std::string::npos);
    }
}

TEST_CASE("library: the LegacyContentTest re-run -- the loaded tip's content decides") {
    const auto zip = testBundle();
    PresetLibrary lib;
    REQUIRE(lib.addBundle(zip.data(), zip.size(), "test://bundle") == 12);

    // colorful.gbr has colour and transparency capability; with no application attribute the
    // legacy rule promotes it to an ImageStamp, which needs the Colored accumulator.
    const LibraryPreset* colorful = byName(lib, "legacy_colorful");
    REQUIRE(colorful != nullptr);
    REQUIRE(colorful->tip != nullptr);
    CHECK(colorful->tip->application() == cb::TipApplication::ImageStamp);
    CHECK(colorful->preset.accumulator == cb::StrokeAccumulator::Colored);

    // The SAME file under ColorAsMask="true" stays an alpha mask on the Uniform fast path --
    // and the two variants must not share a build.
    const LibraryPreset* masked = byName(lib, "legacy_masked");
    REQUIRE(masked != nullptr);
    REQUIRE(masked->tip != nullptr);
    CHECK(masked->tip->application() == cb::TipApplication::AlphaMask);
    CHECK(masked->preset.accumulator == cb::StrokeAccumulator::Uniform);
    CHECK(masked->tip.get() != colorful->tip.get());

    // Same file, same application, same adjustments -> ONE build, shared.
    const LibraryPreset* a = byName(lib, "md5miss");
    const LibraryPreset* b = byName(lib, "byname");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a->tip.get() == b->tip.get());
}

TEST_CASE("library: masking resolves against the predefined primary's absolute size") {
    const auto zip = testBundle();
    PresetLibrary lib;
    REQUIRE(lib.addBundle(zip.data(), zip.size(), "test://bundle") == 12);

    const LibraryPreset* p = byName(lib, "maskmaster");
    REQUIRE(p != nullptr);
    REQUIRE(p->tip != nullptr);
    REQUIRE(p->masking.enabled);
    // masterDiameter = scale x max(w, h) of frame 0 (docs/brushes.md §3.5); the masking brush
    // asked for half of it.
    const double master = 2.0 * p->tip->baseSize(0);
    CHECK(p->masking.diameter == doctest::Approx(0.5 * master));
    CHECK(p->masking.spacing == doctest::Approx(0.2)); // the NESTED tip's own spacing
    // The nested AUTO tip is REAL -- the generator itself, built at map time, not the disc.
    REQUIRE(p->masking.tip != nullptr);
    CHECK(p->masking.tip->isProcedural());

    // An auto primary's master size is its generator diameter -- Basic-5's masking is off, but
    // its resolution path still ran (enabled=false comes out the other end).
    const LibraryPreset* basic = byName(lib, "b)_Basic-5_Size");
    REQUIRE(basic != nullptr);
    CHECK(basic->tipResolution == TipResolution::AutoTip);
    CHECK(basic->tip == nullptr);
    CHECK(!basic->masking.enabled);
}

TEST_CASE("library: the masking brush's own predefined tip -- resolved, or badged and usable") {
    const auto zip = testBundle();
    PresetLibrary lib;
    REQUIRE(lib.addBundle(zip.data(), zip.size(), "test://bundle") == 12);

    SUBCASE("a nested reference that matches resolves to the real bitmap, AlphaMask always") {
        const LibraryPreset* p = byName(lib, "masktip");
        REQUIRE(p != nullptr);
        REQUIRE(p->masking.enabled);
        REQUIRE(p->masking.tip != nullptr);
        REQUIRE(p->masking.tip->bitmap() != nullptr);
        // A masking stroke is a grayscale VALUE: whatever the file's colour content, the masking
        // tip is an alpha mask.
        CHECK(p->masking.tip->bitmap()->application() == cb::TipApplication::AlphaMask);
        CHECK(p->masking.ratio == 1.0); // a bitmap's aspect lives inside the dab's envelope
        CHECK(otherDrops(*p).empty());  // resolved clean: no note
    }
    SUBCASE("a nested reference that matches nothing keeps the disc, badged -- never unusable") {
        const LibraryPreset* p = byName(lib, "maskorphan");
        REQUIRE(p != nullptr);
        REQUIRE(p->masking.enabled); // the masking brush still walks (the analytic disc)
        CHECK(p->masking.tip == nullptr);
        REQUIRE(otherDrops(*p).size() == 1);
        CHECK(otherDrops(*p)[0].find("Masking tip file") != std::string::npos);
        CHECK(p->preset.provenance.fidelity == PresetFidelity::Approximated);
    }
}

TEST_CASE("library: a brushes entry declaring more than the scan budget is skipped, counted") {
    std::vector<ziptest::TestEntry> entries;
    entries.push_back({"mimetype", ziptest::bytesOf("application/x-krita-resourcebundle"), false});
    ziptest::TestEntry huge{"brushes/huge.gbr", ziptest::bytesOf("tiny"), false};
    huge.centralUncompressedOverride = static_cast<long long>(kMaxLibraryScanBytes + 1);
    entries.push_back(huge);
    entries.push_back({"paintoppresets/orphan.kpp",
                       kpptest::syntheticKpp("2.2", predefinedPreset("orphan", "huge.gbr", "")),
                       true});
    const auto zip = ziptest::buildZip(entries);

    PresetLibrary lib;
    REQUIRE(lib.addBundle(zip.data(), zip.size(), "test://bundle") == 1);
    CHECK(lib.counters().scanBudgetSkips == 1);
    // The skipped file is not in the index, so the reference falls back -- visibly.
    CHECK(lib.counters().tipsFallback == 1);
}

TEST_CASE("library: addBundle failures and empties") {
    PresetLibrary lib;
    std::string error;

    SUBCASE("not a bundle: 0 presets, an error") {
        const auto junk = ziptest::bytesOf("22 bytes of pure nonsense, at least");
        CHECK(lib.addBundle(junk.data(), junk.size(), "test://junk", &error) == 0);
        CHECK(!error.empty());
        CHECK(lib.sources().empty()); // a failed source is not attributed
    }
    SUBCASE("a bundle with no presets: 0, no error") {
        const auto zip = ziptest::buildZip(
            {{"mimetype", ziptest::bytesOf("application/x-krita-resourcebundle"), false},
             {"patterns/p.pat", ziptest::bytesOf("pattern"), true}});
        error.clear();
        CHECK(lib.addBundle(zip.data(), zip.size(), "test://empty", &error) == 0);
        CHECK(error.empty());
        CHECK(lib.sources().size() == 1);
    }
}

TEST_CASE("library: an svg tip renders 1000 wide at its own aspect, uninverted") {
    const auto zip = testBundle();
    PresetLibrary lib;
    REQUIRE(lib.addBundle(zip.data(), zip.size(), "test://bundle") == 12);

    const LibraryPreset* p = byName(lib, "svghalf");
    REQUIRE(p != nullptr);
    REQUIRE(p->tip != nullptr);
    CHECK(p->tip->frameWidth(0) == 1000);
    CHECK(p->tip->frameHeight(0) == 500); // 2:1 viewBox -> the derived height
    CHECK(p->tip->application() == cb::TipApplication::AlphaMask);
    CHECK(p->preset.accumulator == cb::StrokeAccumulator::Uniform);

    // Black-over-white renders as paint on the LEFT half: coverage 255 there, 0 on the white
    // right half. Invert once too many and the two swap -- the §3.6.1 failure mode.
    const auto& full = p->tip->level(0, 0);
    const auto at = [&full](std::uint32_t x, std::uint32_t y) {
        return full.coverage[static_cast<std::size_t>(y) * full.width + x];
    };
    CHECK(at(250, 250) == 255);
    CHECK(at(750, 250) == 0);
}

TEST_CASE("library: the source archive is opened ONCE, however many icons are fetched") {
    // ⚠ The bug this pins: loadIcon used to re-read the whole source file, re-walk its central
    // directory and re-parse its manifest -- PER ICON. On the shipped bundle that is a 17.5 MB read
    // and a 66 KB XML DOM to fetch one 200x200 PNG, and the preset dock asked for a dozen a frame
    // while the user dragged its edge. A stopwatch would be flaky; the counter is not.
    const fs::path dir = fs::temp_directory_path() / "mosaic_test_brush_library_opens";
    fs::create_directories(dir);
    const fs::path path = dir / "opens.bundle";
    {
        const auto zip = testBundle();
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(zip.data()),
                  static_cast<std::streamsize>(zip.size()));
    }

    PresetLibrary lib;
    std::string error;
    REQUIRE_MESSAGE(lib.addBundleFile(path, &error) == 12, error);
    CHECK(lib.archiveOpens() == 0); // the SCAN is not an icon fetch: nothing opened yet

    int decoded = 0;
    for (const LibraryPreset& preset : lib.presets()) {
        if (lib.loadIcon(preset).has_value())
            ++decoded;
    }
    REQUIRE(decoded > 0); // or the loop proved nothing
    CHECK(lib.archiveOpens() == 1);

    // ... and a second sweep does not re-open it either.
    for (const LibraryPreset& preset : lib.presets())
        (void)lib.loadIcon(preset);
    CHECK(lib.archiveOpens() == 1);

    fs::remove_all(dir);
}

TEST_CASE("library: an unreadable source is remembered as a failure, not retried") {
    PresetLibrary lib;
    const auto zip = testBundle();
    // "test://bundle" is not a path: every icon fetch through it must fail -- ONCE.
    REQUIRE(lib.addBundle(zip.data(), zip.size(), "test://bundle") == 12);
    for (const LibraryPreset& preset : lib.presets()) {
        std::string error;
        CHECK(!lib.loadIcon(preset, &error).has_value());
        CHECK(!error.empty()); // a cached failure still has to SAY why
    }
    CHECK(lib.archiveOpens() == 1);
}

TEST_CASE("library: lazy icons decode the .kpp raster from the source file") {
    // loadIcon re-opens the SOURCE PATH, so this case needs a real file on disk.
    const fs::path dir = fs::temp_directory_path() / "mosaic_test_brush_library";
    fs::create_directories(dir);
    const fs::path path = dir / "icons.bundle";
    {
        const auto zip = testBundle();
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(zip.data()),
                  static_cast<std::streamsize>(zip.size()));
    }

    PresetLibrary lib;
    std::string error;
    REQUIRE_MESSAGE(lib.addBundleFile(path, &error) == 12, error);

    // The real fixture carries a real 200x200 raster.
    const LibraryPreset* basic = byName(lib, "b)_Basic-5_Size");
    REQUIRE(basic != nullptr);
    const auto icon = lib.loadIcon(*basic, &error);
    REQUIRE_MESSAGE(icon.has_value(), error);
    CHECK(icon->width == 200);
    CHECK(icon->height == 200);

    // The synthetic kpp has no raster (no IDAT): loadIcon fails, the preset does not care.
    const LibraryPreset* synthetic = byName(lib, "byname");
    REQUIRE(synthetic != nullptr);
    CHECK(!lib.loadIcon(*synthetic).has_value());

    fs::remove_all(dir);
}
