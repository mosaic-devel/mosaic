#include "ui/new_document_dialog.hpp"

#include "core/document.hpp"
#include "core/layer.hpp"

#include <doctest/doctest.h>

#include <clocale>
#include <string>

using doctest::Approx;
using namespace mosaic;
using mosaic::ui::NewDocBackground;
using mosaic::ui::NewDocumentSpec;
using mosaic::ui::SizeUnit;

TEST_CASE("unitToPixels converts each unit at a resolution") {
    CHECK(ui::unitToPixels(100.0, SizeUnit::Pixels, 300.0) == Approx(100.0)); // dpi-independent
    CHECK(ui::unitToPixels(1.0, SizeUnit::Inches, 300.0) == Approx(300.0));
    CHECK(ui::unitToPixels(25.4, SizeUnit::Millimeters, 300.0) == Approx(300.0)); // 25.4mm = 1in
    CHECK(ui::unitToPixels(2.54, SizeUnit::Centimeters, 300.0) == Approx(300.0));
    CHECK(ui::unitToPixels(72.0, SizeUnit::Points, 300.0) == Approx(300.0)); // 72pt = 1in
}

TEST_CASE("pixelsToUnit inverts unitToPixels") {
    for (SizeUnit u : {SizeUnit::Pixels, SizeUnit::Millimeters, SizeUnit::Centimeters,
                       SizeUnit::Inches, SizeUnit::Points}) {
        const double px = 1234.0;
        CHECK(ui::unitToPixels(ui::pixelsToUnit(px, u, 150.0), u, 150.0) == Approx(px));
    }
    CHECK(ui::pixelsToUnit(300.0, SizeUnit::Inches, 300.0) == Approx(1.0));
}

TEST_CASE("NewDocumentSpec resolves physical sizes to pixels (A4 at 300ppi)") {
    NewDocumentSpec a4;
    a4.width = 210.0;
    a4.height = 297.0;
    a4.unit = SizeUnit::Millimeters;
    a4.dpi = 300.0;
    CHECK(a4.pixelWidth() == 2480);  // round(210/25.4*300)
    CHECK(a4.pixelHeight() == 3508); // round(297/25.4*300)
}

TEST_CASE("pixel dimensions are clamped to a sane range") {
    NewDocumentSpec spec;
    spec.unit = SizeUnit::Pixels;

    spec.width = 0.0;
    spec.height = -50.0;
    CHECK(spec.pixelWidth() == 1); // never zero/negative
    CHECK(spec.pixelHeight() == 1);

    spec.width = 1.0e9;
    CHECK(spec.pixelWidth() == ui::kMaxCanvasDimension);
}

TEST_CASE("the default spec is the Full HD preset") {
    const NewDocumentSpec d = ui::defaultNewDocumentSpec();
    CHECK(d.unit == SizeUnit::Pixels);
    CHECK(d.pixelWidth() == 1920);
    CHECK(d.pixelHeight() == 1080);
    CHECK(d.colorSpace == core::ColorSpace::SRGB);
    CHECK(d.precision == core::Precision::U8);
    CHECK(d.background == NewDocBackground::White);
}

TEST_CASE("the preset table includes the A-series and common pixel sizes") {
    const auto& presets = ui::documentPresets();
    REQUIRE(presets.size() >= 6);

    bool foundA4 = false;
    bool foundFullHd = false;
    for (const auto& p : presets) {
        if (p.name == "A4  (210 × 297 mm)") {
            foundA4 = true;
            CHECK(p.unit == SizeUnit::Millimeters);
            CHECK(p.width == Approx(210.0));
            CHECK(p.height == Approx(297.0));
            CHECK(p.dpi == Approx(300.0));
        }
        if (p.name == "Full HD  (1920 × 1080 px)") {
            foundFullHd = true;
            CHECK(p.unit == SizeUnit::Pixels);
            CHECK(p.width == Approx(1920.0));
        }
    }
    CHECK(foundA4);
    CHECK(foundFullHd);
}

TEST_CASE("buildDocument honours size, colour state, dpi and title") {
    NewDocumentSpec spec;
    spec.title = "My Poster";
    spec.width = 210.0;
    spec.height = 297.0;
    spec.unit = SizeUnit::Millimeters;
    spec.dpi = 300.0;
    spec.colorSpace = core::ColorSpace::DisplayP3;
    spec.precision = core::Precision::F16;
    spec.background = NewDocBackground::White;

    const auto doc = ui::buildDocument(spec);
    REQUIRE(doc != nullptr);
    CHECK(doc->width() == 2480);
    CHECK(doc->height() == 3508);
    CHECK(doc->colorSpace() == core::ColorSpace::DisplayP3);
    CHECK(doc->precision() == core::Precision::F16);
    CHECK(doc->dpi() == Approx(300.0));
    CHECK(doc->title() == "My Poster");
}

TEST_CASE("buildDocument creates the right background layer") {
    NewDocumentSpec spec;
    spec.width = 8.0;
    spec.height = 4.0;
    spec.unit = SizeUnit::Pixels;

    SUBCASE("white background is an unlocked, white-filled raster") {
        spec.background = NewDocBackground::White;
        const auto doc = ui::buildDocument(spec);
        REQUIRE(doc->layerCount() == 1);
        auto* bg = doc->root().child(0).as<core::RasterLayer>();
        REQUIRE(bg != nullptr);
        CHECK(bg->name() == "Background");
        CHECK_FALSE(bg->locked()); // unlocked so a fresh canvas is paintable (user 2026-06-19)
        CHECK(bg->image().width == 8);
        CHECK(bg->image().height == 4);
        CHECK(bg->image().rgba[0] == 255); // opaque white
        CHECK(bg->image().rgba[3] == 255);
    }
    SUBCASE("black background is filled black") {
        spec.background = NewDocBackground::Black;
        const auto doc = ui::buildDocument(spec);
        auto* bg = doc->root().child(0).as<core::RasterLayer>();
        REQUIRE(bg != nullptr);
        CHECK(bg->image().rgba[0] == 0);
        CHECK(bg->image().rgba[3] == 255);
    }
    SUBCASE("transparent background is an unlocked empty raster") {
        spec.background = NewDocBackground::Transparent;
        const auto doc = ui::buildDocument(spec);
        REQUIRE(doc->layerCount() == 1);
        auto* layer = doc->root().child(0).as<core::RasterLayer>();
        REQUIRE(layer != nullptr);
        CHECK_FALSE(layer->locked());
        CHECK(layer->image().rgba[3] == 0); // fully transparent
    }
}

TEST_CASE("buildDocument falls back to a default title when none is given") {
    NewDocumentSpec spec;
    spec.title.clear();
    const auto doc = ui::buildDocument(spec);
    CHECK(doc->title() == "Untitled");
}

TEST_CASE("every Screen preset carries a size detail (the Square shelf-mismatch fix)") {
    for (const auto& p : ui::documentPresets()) {
        if (p.category != ui::PresetCategory::Screen)
            continue;
        CHECK(!ui::presetDetail(p).empty()); // "SVGA  (800 × 600 px)" -> "800 × 600 px"
        CHECK(ui::presetShortName(p) != p.name);
    }
}

TEST_CASE("matchDocumentPreset finds presets orientation-blind, custom sizes miss") {
    NewDocumentSpec spec; // Full HD default
    CHECK(ui::matchDocumentPreset(spec) >= 0);
    std::swap(spec.width, spec.height); // 1080 x 1920 is still the Full HD preset
    CHECK(ui::matchDocumentPreset(spec) >= 0);
    spec.width = 1234.0;
    spec.height = 567.0;
    CHECK(ui::matchDocumentPreset(spec) == -1);
    spec = ui::defaultNewDocumentSpec();
    spec.dpi = 240.0; // preset size at a foreign dpi is custom too
    CHECK(ui::matchDocumentPreset(spec) == -1);
}

// Regression (S54): the token is written by customSizeToken() and read by fromChars(), which only
// ever accepts '.'. The writer used snprintf("%.10g"), which honours LC_NUMERIC -- and i18n::init()
// moves LC_NUMERIC to the user's locale at start-up. So on a comma locale (most of Europe) the app
// wrote "1234,5;678;cm;240" and could not parse it back: every saved custom size silently vanished
// from the recents. The writer now goes through common::gToString(), which is locale-independent.
TEST_CASE("customSizeToken is locale-independent (comma decimal separator)") {
    const char* saved = std::setlocale(LC_NUMERIC, nullptr);
    const std::string restore = saved != nullptr ? saved : "C";
    // Any comma-decimal locale will do; skip where none is generated.
    const bool applied = std::setlocale(LC_NUMERIC, "pl_PL.UTF-8") != nullptr ||
                         std::setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr ||
                         std::setlocale(LC_NUMERIC, "fr_FR.UTF-8") != nullptr;
    if (!applied) {
        return;
    }
    REQUIRE(std::localeconv()->decimal_point[0] == ',');  // the guard is actually armed

    NewDocumentSpec spec;
    spec.width = 1234.5;
    spec.height = 678.0;
    spec.unit = SizeUnit::Centimeters;
    spec.dpi = 240.0;
    const std::string token = ui::customSizeToken(spec);
    CHECK(token == "1234.5;678;cm;240");  // '.' even though the locale says ','
    const auto back = ui::parseCustomSizeToken(token);
    REQUIRE(back.has_value());
    CHECK(back->width == Approx(1234.5));
    CHECK(back->dpi == Approx(240.0));

    std::setlocale(LC_NUMERIC, restore.c_str());
}

TEST_CASE("customSizeToken round-trips and rejects garbage") {
    NewDocumentSpec spec;
    spec.width = 1234.5;
    spec.height = 678.0;
    spec.unit = SizeUnit::Centimeters;
    spec.dpi = 240.0;
    const std::string token = ui::customSizeToken(spec);
    CHECK(token == "1234.5;678;cm;240");
    const auto back = ui::parseCustomSizeToken(token);
    REQUIRE(back.has_value());
    CHECK(back->width == Approx(1234.5));
    CHECK(back->height == Approx(678.0));
    CHECK(back->unit == SizeUnit::Centimeters);
    CHECK(back->dpi == Approx(240.0));

    CHECK(!ui::parseCustomSizeToken("").has_value());
    CHECK(!ui::parseCustomSizeToken("1;2;cm").has_value());        // missing dpi
    CHECK(!ui::parseCustomSizeToken("1;2;parsec;72").has_value()); // unknown unit
    CHECK(!ui::parseCustomSizeToken("0;2;cm;72").has_value());     // non-positive
    CHECK(!ui::parseCustomSizeToken("a;2;cm;72").has_value());
    CHECK(!ui::parseCustomSizeToken("1;2;cm;72;9").has_value());   // trailing field

    CHECK(ui::customSizeTitle(*back) == "1234.5 × 678 cm");
    CHECK(ui::customSizeFace(*back) == "1234.5 × 678");
}

TEST_CASE("nextUntitledTitle steps past open documents") {
    CHECK(ui::nextUntitledTitle({}) == "Untitled");
    CHECK(ui::nextUntitledTitle({"Poster", "photo.png"}) == "Untitled");
    CHECK(ui::nextUntitledTitle({"Untitled"}) == "Untitled 2");
    CHECK(ui::nextUntitledTitle({"Untitled", "Untitled 2"}) == "Untitled 3");
    CHECK(ui::nextUntitledTitle({"Untitled 2"}) == "Untitled"); // the base name is free
}

TEST_CASE("isAutoUntitledTitle matches exactly the generated names") {
    CHECK(ui::isAutoUntitledTitle("Untitled"));
    CHECK(ui::isAutoUntitledTitle("Untitled 2"));
    CHECK(ui::isAutoUntitledTitle("Untitled 37"));
    // Deliberate titles never qualify -- a save must not clobber them with the file stem.
    CHECK(!ui::isAutoUntitledTitle(""));
    CHECK(!ui::isAutoUntitledTitle("Poster"));
    CHECK(!ui::isAutoUntitledTitle("Untitled "));        // trailing space, no number
    CHECK(!ui::isAutoUntitledTitle("Untitled 2b"));      // not a bare number
    CHECK(!ui::isAutoUntitledTitle("Untitled2"));        // the generator always spaces
    CHECK(!ui::isAutoUntitledTitle("My Untitled 2"));    // prefix only
    CHECK(!ui::isAutoUntitledTitle("untitled"));         // case matters: not a generated name
}
