#include "common/recent_files.hpp"

#include "ui/new_document_dialog.hpp"
#include "ui/widgets.hpp" // evaluateFieldExpression / parseFieldNumber (the shared field base)
#include "ui/xdg_thumbnails.hpp"

#include <doctest/doctest.h>

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>

#include <clocale>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace mosaic;
namespace fs = std::filesystem;

// ---- the ring buffer behind File -> Open Recent (common/recent_files.hpp) ------------------

TEST_CASE("pushRecentFile: newest first, deduplicated, capped") {
    std::vector<std::string> r;
    common::pushRecentFile(r, "/a");
    common::pushRecentFile(r, "/b");
    common::pushRecentFile(r, "/c");
    CHECK(r == std::vector<std::string>{"/c", "/b", "/a"});

    // Re-opening an existing entry MOVES it to the front (no duplicate).
    common::pushRecentFile(r, "/a");
    CHECK(r == std::vector<std::string>{"/a", "/c", "/b"});

    // The cap trims the oldest entries.
    for (int i = 0; i < 20; ++i)
        common::pushRecentFile(r, "/f" + std::to_string(i));
    CHECK(r.size() == common::kMaxRecentFiles);
    CHECK(r.front() == "/f19");

    // An empty path is ignored.
    common::pushRecentFile(r, "");
    CHECK(r.size() == common::kMaxRecentFiles);
    CHECK(r.front() == "/f19");
}

TEST_CASE("removeRecentFile drops the entry wherever it sits; order otherwise holds") {
    std::vector<std::string> r = {"/a", "/b", "/c"};
    common::removeRecentFile(r, "/b");
    CHECK(r == std::vector<std::string>{"/a", "/c"});
    common::removeRecentFile(r, "/nope"); // absent: a no-op
    CHECK(r == std::vector<std::string>{"/a", "/c"});
}

// ---- document templates (ui::scanDocumentTemplates) ----------------------------------------

namespace {
fs::path templateScratch(const char* name) {
    const fs::path dir = fs::temp_directory_path() / "mosaic_tests" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}
void touch(const fs::path& p) {
    std::ofstream(p).put('x');
}
} // namespace

TEST_CASE("scanDocumentTemplates: numbered order, name parsing, junk ignored") {
    const fs::path dir = templateScratch("templates_scan");
    touch(dir / "2-Resume.mosaic");
    touch(dir / "1-Birthday_Card.mosaic");
    touch(dir / "10-Poster.mosaic"); // numeric, not lexicographic: after 2
    touch(dir / "Zebra.mosaic");     // un-numbered: sorts after every numbered one
    touch(dir / "Apple.mosaic");
    touch(dir / "notes.txt");        // not a template
    touch(dir / "3-.mosaic");        // empty name after the prefix: falls back to the stem

    const std::vector<ui::TemplateFile> got = ui::scanDocumentTemplates(dir);
    REQUIRE(got.size() == 6);
    CHECK(got[0].name == "Birthday Card"); // underscore reads as a space
    CHECK(got[0].order == 1);
    CHECK(got[1].name == "Resume");
    CHECK(got[2].name == "3-"); // the fallback stem
    CHECK(got[3].name == "Poster");
    CHECK(got[3].order == 10);
    CHECK(got[4].name == "Apple"); // un-numbered tail, alphabetical
    CHECK(got[5].name == "Zebra");
}

TEST_CASE("scanDocumentTemplates: a missing directory is zero templates, not an error") {
    CHECK(ui::scanDocumentTemplates(fs::temp_directory_path() / "mosaic_tests" /
                                    "no_such_dir_xyz")
              .empty());
}

// ---- the document summary math (ui::bytesPerPixel / layerMemoryBytes / formatByteSize) -----

TEST_CASE("bytesPerPixel follows the precision") {
    CHECK(ui::bytesPerPixel(core::Precision::U8) == 4);
    CHECK(ui::bytesPerPixel(core::Precision::U16) == 8);
    CHECK(ui::bytesPerPixel(core::Precision::F16) == 8);
    CHECK(ui::bytesPerPixel(core::Precision::F32) == 16);
}

TEST_CASE("layerMemoryBytes = resolved pixels x bytes per pixel") {
    ui::NewDocumentSpec spec; // 1920x1080 U8
    CHECK(ui::layerMemoryBytes(spec) == 1920ULL * 1080 * 4);
    spec.precision = core::Precision::F32;
    CHECK(ui::layerMemoryBytes(spec) == 1920ULL * 1080 * 16);
    // A physical-unit spec resolves through the DPI first: A4 at 300 ppi, 8-bit.
    spec = {.title = "t", .width = 210.0, .height = 297.0, .unit = ui::SizeUnit::Millimeters,
            .dpi = 300.0};
    CHECK(ui::layerMemoryBytes(spec) == static_cast<std::uint64_t>(spec.pixelWidth()) *
                                            spec.pixelHeight() * 4);
}

TEST_CASE("formatByteSize: decimal units, one decimal under 100") {
    CHECK(ui::formatByteSize(0) == "0 B");
    CHECK(ui::formatByteSize(999) == "999 B");
    CHECK(ui::formatByteSize(45'100) == "45.1 KB");
    CHECK(ui::formatByteSize(33'200'000) == "33.2 MB");
    CHECK(ui::formatByteSize(132'000'000) == "132 MB");
    CHECK(ui::formatByteSize(1'100'000'000) == "1.1 GB");
}

// ---- preset gallery split + card faces ------------------------------------------------------

TEST_CASE("preset categories: Print physical, Screen/Texture pixels; faces parse") {
    std::size_t print = 0;
    std::size_t screen = 0;
    std::size_t texture = 0;
    for (const ui::DocumentPreset& p : ui::documentPresets()) {
        switch (ui::presetCategory(p)) {
        case ui::PresetCategory::Print:
            ++print;
            CHECK(p.unit != ui::SizeUnit::Pixels); // paper sizes are physical
            break;
        case ui::PresetCategory::Screen:
            ++screen;
            CHECK(p.unit == ui::SizeUnit::Pixels);
            break;
        case ui::PresetCategory::Texture:
            ++texture;
            CHECK(p.unit == ui::SizeUnit::Pixels);
            CHECK(p.width == p.height); // power-of-two squares
            break;
        }
    }
    CHECK(print == 9);
    CHECK(screen == 6);
    CHECK(texture == 7); // 128..8192
    const ui::DocumentPreset& a4 = ui::documentPresets()[4];
    CHECK(ui::presetShortName(a4) == "A4");
    CHECK(ui::presetDetail(a4) == "210 × 297 mm");
    const ui::DocumentPreset& tex = ui::documentPresets().back(); // "8192  (8192 × 8192 px)"
    CHECK(ui::presetShortName(tex) == "8192");
    CHECK(ui::presetDetail(tex) == "8192 × 8192 px");
}

// Regression (S54): the expression parser normalises a ',' decimal to '.', then used strtod --
// which reads the ACTIVE LC_NUMERIC, and i18n::init() moves that to the user's locale at start-up.
// On any comma locale strtod("8.5") stopped at the '.', so the whole-string check rejected it and
// EVERY decimal typed into a number field was refused. It now uses common::fromChars, which is
// locale-independent and '.'-only -- the contract the normalisation was written for.
TEST_CASE("evaluateFieldExpression is locale-independent (comma decimal separator)") {
    const char* saved = std::setlocale(LC_NUMERIC, nullptr);
    const std::string restore = saved != nullptr ? saved : "C";
    const bool applied = std::setlocale(LC_NUMERIC, "pl_PL.UTF-8") != nullptr ||
                         std::setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr ||
                         std::setlocale(LC_NUMERIC, "fr_FR.UTF-8") != nullptr;
    if (!applied) {
        return;  // no comma locale generated here
    }
    REQUIRE(std::localeconv()->decimal_point[0] == ',');  // the guard is actually armed

    CHECK(ui::evaluateFieldExpression("8.5-0.25") == 8.25);  // '.' typed by the user
    CHECK(ui::evaluateFieldExpression("1,5*2") == 3.0);      // ',' typed by the user
    CHECK(ui::evaluateFieldExpression("2.5") == 2.5);
    CHECK(!ui::evaluateFieldExpression("1.2.3").has_value());  // still rejects garbage

    std::setlocale(LC_NUMERIC, restore.c_str());
}

TEST_CASE("evaluateFieldExpression: arithmetic, precedence, malformed input") {
    CHECK(ui::evaluateFieldExpression("1024*2") == 2048.0);
    CHECK(ui::evaluateFieldExpression("1024 * 2 + 1") == 2049.0);
    CHECK(ui::evaluateFieldExpression("2+3*4") == 14.0); // precedence
    CHECK(ui::evaluateFieldExpression("(2+3)*4") == 20.0);
    CHECK(ui::evaluateFieldExpression("8.5-0.25") == 8.25);
    CHECK(ui::evaluateFieldExpression("1920/2") == 960.0);
    CHECK(ui::evaluateFieldExpression("-4+10") == 6.0);
    CHECK(ui::evaluateFieldExpression("1,5*2") == 3.0);  // comma decimal (NumberField convention)
    CHECK(ui::evaluateFieldExpression("300") == 300.0);  // a plain number is itself
    CHECK(!ui::evaluateFieldExpression("1024*").has_value());  // mid-typing
    CHECK(!ui::evaluateFieldExpression("").has_value());
    CHECK(!ui::evaluateFieldExpression("10/0").has_value());   // division by zero
    CHECK(!ui::evaluateFieldExpression("1.2.3").has_value());
    CHECK(!ui::evaluateFieldExpression("(2+3").has_value());   // unbalanced
    CHECK(!ui::evaluateFieldExpression("2 3").has_value());    // trailing junk
}

TEST_CASE("parseFieldNumber evaluates arithmetic (the shared-base promotion)") {
    double v = 0.0;
    CHECK(ui::parseFieldNumber("1024*2", v));
    CHECK(v == 2048.0);
    CHECK(ui::parseFieldNumber("(3+4)/2", v));
    CHECK(v == 3.5);
    CHECK(ui::parseFieldNumber("+7", v)); // the historical unary plus still parses
    CHECK(v == 7.0);
    CHECK(ui::parseFieldNumber("2,5", v));
    CHECK(v == 2.5);
    CHECK_FALSE(ui::parseFieldNumber("abc", v));
}

// The widget half of the promotion: arithmetic keys must TYPE (the ctor turned off the float
// filter), foreign printables must not, and unfocus evaluates in place. handle() is exercised
// directly on an unshown window -- FLTK's event statics are public and routing needs no display.
TEST_CASE("NumberField: arithmetic keys type in, unfocus evaluates the expression") {
    struct TestField : ui::NumberField { // handle() is protected; the test drives it directly
        using ui::NumberField::NumberField;
        using ui::NumberField::handle;
    };
    Fl_Double_Window win(200, 80);
    win.begin();
    auto* f = new TestField(10, 10, 120, 24);
    win.end();

    static char keyText[2];
    const auto key = [f](char c) {
        keyText[0] = c;
        keyText[1] = '\0';
        Fl::e_text = keyText;
        Fl::e_length = 1;
        Fl::e_state = 0;
        Fl::e_keysym = static_cast<int>(static_cast<unsigned char>(c));
        f->handle(FL_KEYBOARD);
    };

    f->value("");
    for (const char c : std::string("2*(3)"))
        key(c);
    CHECK(std::string(f->value()) == "2*(3)");
    key('a'); // foreign printable: swallowed, the field still reads as numeric
    CHECK(std::string(f->value()) == "2*(3)");
    f->handle(FL_UNFOCUS); // unfocus evaluates (user 2026-07-22: fields must settle on blur)
    CHECK(std::string(f->value()) == "6");

    f->value("1024*"); // mid-typing junk survives unfocus untouched (the caller's parse falls back)
    f->handle(FL_UNFOCUS);
    CHECK(std::string(f->value()) == "1024*");

    f->value("");
    key('1');
    key(',');  // comma-locale separator still inserts '.'
    key('5');
    CHECK(std::string(f->value()) == "1.5");
}

TEST_CASE("abbreviatedLocation shows the parent folder, home as ~") {
    CHECK(ui::abbreviatedLocation("/home/u/Pictures/a.png", "/home/u") == "~/Pictures");
    CHECK(ui::abbreviatedLocation("/home/u/a.png", "/home/u") == "~");
    CHECK(ui::abbreviatedLocation("/srv/shared/a.png", "/home/u") == "/srv/shared");
    // "/home/uber" must NOT abbreviate under home "/home/u" (prefix != path component).
    CHECK(ui::abbreviatedLocation("/home/uber/a.png", "/home/u") == "/home/uber");
    CHECK(ui::abbreviatedLocation("/x/y/a.png", "") == "/x/y");
}

TEST_CASE("sizeUnitAbbrev names every unit's in-field tag") {
    CHECK(ui::sizeUnitAbbrev(ui::SizeUnit::Pixels) == "px");
    CHECK(ui::sizeUnitAbbrev(ui::SizeUnit::Millimeters) == "mm");
    CHECK(ui::sizeUnitAbbrev(ui::SizeUnit::Centimeters) == "cm");
    CHECK(ui::sizeUnitAbbrev(ui::SizeUnit::Inches) == "in");
    CHECK(ui::sizeUnitAbbrev(ui::SizeUnit::Points) == "pt");
}

TEST_CASE("fitPreservingAspect fits inside the box and never upscales") {
    auto fit = ui::fitPreservingAspect(1920, 1080, 256, 256);
    CHECK(fit.width == 256);
    CHECK(fit.height == 144);
    fit = ui::fitPreservingAspect(1080, 1920, 256, 256); // portrait
    CHECK(fit.width == 144);
    CHECK(fit.height == 256);
    fit = ui::fitPreservingAspect(100, 50, 256, 256); // smaller than the box: stays 1:1
    CHECK(fit.width == 100);
    CHECK(fit.height == 50);
    fit = ui::fitPreservingAspect(0, 50, 256, 256); // empty input
    CHECK(fit.width == 0);
}

TEST_CASE("boxDownscale area-averages exact blocks") {
    common::Image src(4, 2);
    // Left 2x2 block solid red, right 2x2 block solid blue; a 2x1 downscale averages each block
    // to itself (uniform), so the result is one red and one blue pixel.
    for (std::uint32_t y = 0; y < 2; ++y)
        for (std::uint32_t x = 0; x < 4; ++x) {
            std::uint8_t* px = src.rgba.data() + (y * 4 + x) * 4;
            px[0] = x < 2 ? 255 : 0;
            px[2] = x < 2 ? 0 : 255;
            px[3] = 255;
        }
    const common::Image dst = ui::boxDownscale(src, 2, 1);
    REQUIRE(!dst.empty());
    CHECK(dst.rgba[0] == 255); // left = red
    CHECK(dst.rgba[2] == 0);
    CHECK(dst.rgba[4] == 0); // right = blue
    CHECK(dst.rgba[6] == 255);
    CHECK(ui::boxDownscale(common::Image{}, 2, 2).empty());
}

TEST_CASE("checkerCompose: opaque pixels pass through, transparency becomes the checker") {
    common::Image src(16, 16);
    for (std::uint32_t y = 0; y < 16; ++y)
        for (std::uint32_t x = 0; x < 16; ++x) {
            std::uint8_t* px = src.rgba.data() + (static_cast<std::size_t>(y) * 16 + x) * 4;
            px[0] = 255; // red, but only the left half is opaque
            px[3] = x < 8 ? 255 : 0;
        }
    const common::Image out = ui::checkerCompose(src);
    REQUIRE(!out.empty());
    CHECK(out.rgba[0] == 255); // opaque red passes through
    CHECK(out.rgba[3] == 255);
    // A fully transparent pixel becomes a checker grey, opaque.
    const std::uint8_t* right = out.rgba.data() + (0 * 16 + 12) * 4;
    CHECK((right[0] == 205 || right[0] == 150));
    CHECK(right[0] == right[1]);
    CHECK(right[1] == right[2]);
    CHECK(right[3] == 255);
    CHECK(ui::checkerCompose(common::Image{}).empty());
}

// ---- the desktop's shared thumbnail cache (ui/xdg_thumbnails.hpp): pure key helpers ---------

TEST_CASE("fileUriFor percent-encodes exactly the non-path-legal bytes") {
    CHECK(ui::fileUriFor("/home/u/a.png") == "file:///home/u/a.png");
    CHECK(ui::fileUriFor("/home/u/my file.png") == "file:///home/u/my%20file.png");
    CHECK(ui::fileUriFor("/home/u/50%.png") == "file:///home/u/50%25.png");
    // Sub-delims stay literal (GLib/Qt leave them unescaped when keying the cache).
    CHECK(ui::fileUriFor("/a/b'c(d).png") == "file:///a/b'c(d).png");
    // UTF-8 bytes escape per byte: "ä" = 0xC3 0xA4.
    CHECK(ui::fileUriFor("/a/\xC3\xA4.png") == "file:///a/%C3%A4.png");
}

TEST_CASE("xdgThumbnailName is the md5 of the URI") {
    // The freedesktop spec's own worked example.
    CHECK(ui::xdgThumbnailName("file:///home/jens/photos/me.png") ==
          "c6ee772d9e49320e97ec29a7eb5b1697.png");
}
