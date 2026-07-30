#include "ui/map_picker.hpp"

#include "core/texture/city_catalog.hpp"
#include "core/texture/solar.hpp"
#include "ui/texture_generator_dialog.hpp"

#include <doctest/doctest.h>

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

// The world-map place picker (the city dropdown's successor): the pure equirectangular math and
// the land mask, the compact control's nearest-city labeling, the flyout's pin/pick flow (driven
// headlessly with staged window-local events -- the translated form FLTK delivers to a child
// sub-window), and the pick's effect on the texture dialog's observer state.

using namespace mosaic::ui;
namespace tex = mosaic::core::texture;

namespace {

int catalogIndexOf(const char* name) {
    for (std::size_t i = 0; i < tex::cityCatalogCount(); ++i)
        if (std::strcmp(tex::cityAt(i).name, name) == 0)
            return static_cast<int>(i);
    return -1;
}

void stagePointer(int x, int y) {
    Fl::e_x = x;
    Fl::e_y = y;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
}

// The house idiom: the handle() override is protected, the base's is public.
int send(Fl_Widget& w, int event) { return w.handle(event); }

} // namespace

TEST_CASE("map math: px <-> lat/lon round-trips and clamps") {
    const double W = MapFlyout::kMapW;
    const double H = MapFlyout::kMapH;

    // Fixed anchors of the plate-carree frame.
    CHECK(map_detail::lonToX(-180.0, W) == doctest::Approx(0.0));
    CHECK(map_detail::lonToX(0.0, W) == doctest::Approx(W / 2));
    CHECK(map_detail::lonToX(180.0, W) == doctest::Approx(W));
    CHECK(map_detail::latToY(90.0, H) == doctest::Approx(0.0));
    CHECK(map_detail::latToY(0.0, H) == doctest::Approx(H / 2));
    CHECK(map_detail::latToY(-90.0, H) == doctest::Approx(H));

    // Round-trip through the projection and back, across the globe.
    for (double lat : {-89.0, -33.5, 0.0, 48.85, 89.0}) {
        for (double lon : {-179.5, -122.4, 0.0, 2.35, 139.69, 179.5}) {
            const double x = map_detail::lonToX(lon, W);
            const double y = map_detail::latToY(lat, H);
            CHECK(map_detail::xToLon(x, W) == doctest::Approx(lon));
            CHECK(map_detail::yToLat(y, H) == doctest::Approx(lat));
        }
    }

    // Events past the map edge clamp to legal coordinates.
    CHECK(map_detail::xToLon(-25.0, W) == doctest::Approx(-180.0));
    CHECK(map_detail::xToLon(W + 25.0, W) == doctest::Approx(180.0));
    CHECK(map_detail::yToLat(-5.0, H) == doctest::Approx(90.0));
    CHECK(map_detail::yToLat(H + 5.0, H) == doctest::Approx(-90.0));
}

TEST_CASE("map data: the land mask puts land and water where the Earth has them") {
    const int W = MapFlyout::kMapW;
    const int H = MapFlyout::kMapH;
    const std::vector<std::uint8_t> mask = map_detail::rasterizeLandMask(W, H);
    REQUIRE(mask.size() == static_cast<std::size_t>(W) * H);

    const auto at = [&](double lat, double lon) {
        const int x = std::clamp(static_cast<int>(map_detail::lonToX(lon, W)), 0, W - 1);
        const int y = std::clamp(static_cast<int>(map_detail::latToY(lat, H)), 0, H - 1);
        return mask[static_cast<std::size_t>(y) * W + x] != 0;
    };
    CHECK(at(23.0, 10.0));    // the Sahara
    CHECK(at(65.0, 100.0));   // Siberia
    CHECK(at(-24.0, 134.0));  // central Australia
    CHECK(at(-85.0, 0.0));    // Antarctica
    CHECK(at(75.0, -40.0));   // Greenland
    CHECK_FALSE(at(0.0, -30.0));   // mid-Atlantic
    CHECK_FALSE(at(0.0, -150.0));  // mid-Pacific
    CHECK_FALSE(at(42.0, 50.5));   // the Caspian: an inner hole, even-odd carves it out
    CHECK_FALSE(at(59.0, -85.0));  // Hudson Bay

    // Sanity on the overall proportions: land covers roughly 30-40% of the plate-carree frame
    // (the projection inflates the poles, so this sits above the true ~29%).
    const std::size_t land = static_cast<std::size_t>(
        std::count(mask.begin(), mask.end(), std::uint8_t{1}));
    const double frac = static_cast<double>(land) / mask.size();
    CHECK(frac > 0.25);
    CHECK(frac < 0.45);
}

TEST_CASE("placeName preserves the dropdown's showNearest semantics") {
    const int london = catalogIndexOf("London");
    REQUIRE(london >= 0);
    const tex::CityEntry& l = tex::cityAt(static_cast<std::size_t>(london));

    // Exactly a catalogued city: its own name.
    CHECK(map_detail::placeName(l.latitudeDeg, l.longitudeDeg) == "London");

    // A custom point: "Nearest: <the closest catalogued city>".
    const tex::NearestCity near = tex::nearestCity(0.0, -30.0);
    CHECK(map_detail::placeName(0.0, -30.0) ==
          std::string("Nearest: ") + tex::cityAt(near.index).name);
}

TEST_CASE("MapPicker: labels the place, stores the coordinates, opens on release-inside") {
    Fl_Double_Window win(0, 0, 260, 60);
    win.begin();
    MapPicker mp(10, 10, 200, 26);
    win.end();

    int opens = 0;
    mp.setOnOpen([&] { ++opens; });

    const int london = catalogIndexOf("London");
    REQUIRE(london >= 0);
    const tex::CityEntry& l = tex::cityAt(static_cast<std::size_t>(london));

    // showNearest is the parent driving the display: label + stored coords, no onOpen.
    mp.showNearest(l.latitudeDeg, l.longitudeDeg);
    CHECK(mp.placeLabel() == "London");
    CHECK(mp.latDeg() == doctest::Approx(l.latitudeDeg));
    CHECK(mp.lonDeg() == doctest::Approx(l.longitudeDeg));
    mp.showNearest(0.0, -30.0);
    CHECK(mp.placeLabel().rfind("Nearest: ", 0) == 0);
    CHECK(opens == 0);

    // The house click convention: claim the pair, fire on the release that ends inside.
    stagePointer(40, 20);
    Fl::e_state = FL_BUTTON1;
    CHECK(send(mp, FL_PUSH) == 1);
    CHECK(opens == 0); // the press alone must not open
    Fl::e_state = 0;
    CHECK(send(mp, FL_RELEASE) == 1);
    CHECK(opens == 1);

    // A press that slides off before letting go opens nothing.
    stagePointer(40, 20);
    Fl::e_state = FL_BUTTON1;
    CHECK(send(mp, FL_PUSH) == 1);
    stagePointer(240, 50);
    CHECK(send(mp, FL_DRAG) == 1);
    Fl::e_state = 0;
    CHECK(send(mp, FL_RELEASE) == 1);
    CHECK(opens == 1);

    Fl::pushed(nullptr);
    Fl::belowmouse(nullptr);
}

TEST_CASE("MapFlyout: the pin picks lat/lon, snaps onto catalogued cities, drags live") {
    Fl_Double_Window win(0, 0, 500, 400);
    win.begin();
    MapFlyout* fly = new MapFlyout(); // a child sub-window (the pre-show BubbleFlyout rule)
    fly->hide();
    win.end();

    std::vector<std::pair<double, double>> picks;
    fly->setOnPick([&](double lat, double lon) { picks.emplace_back(lat, lon); });

    // The map's origin in the flyout's own coordinates (no placeBubble ran: no content shift).
    const int mapX = BubbleFlyout::kContentX;
    const int mapY = BubbleFlyout::kPad;

    // A press mid-Atlantic (far from every snap target): the pick is the exact inverse mapping.
    stagePointer(mapX + static_cast<int>(map_detail::lonToX(-30.0, MapFlyout::kMapW)),
                 mapY + static_cast<int>(map_detail::latToY(0.0, MapFlyout::kMapH)));
    Fl::e_state = FL_BUTTON1;
    CHECK(send(*fly, FL_PUSH) == 1);
    REQUIRE(picks.size() == 1);
    CHECK(picks[0].second == doctest::Approx(-30.0).epsilon(0.01));
    CHECK(picks[0].first == doctest::Approx(0.0).epsilon(0.01));
    CHECK(fly->pinLon() == doctest::Approx(-30.0).epsilon(0.01));

    // Dragging moves the pin and reports live.
    stagePointer(mapX + static_cast<int>(map_detail::lonToX(-150.0, MapFlyout::kMapW)),
                 mapY + static_cast<int>(map_detail::latToY(10.0, MapFlyout::kMapH)));
    CHECK(send(*fly, FL_DRAG) == 1);
    REQUIRE(picks.size() == 2);
    CHECK(picks[1].second == doctest::Approx(-150.0).epsilon(0.01));
    Fl::e_state = 0;
    CHECK(send(*fly, FL_RELEASE) == 1); // the pair rule: the gesture ends here

    // A press within the snap radius of a catalogued city lands exactly on the city.
    const int tokyo = catalogIndexOf("Tokyo");
    REQUIRE(tokyo >= 0);
    const tex::CityEntry& t = tex::cityAt(static_cast<std::size_t>(tokyo));
    stagePointer(mapX + static_cast<int>(map_detail::lonToX(t.longitudeDeg, MapFlyout::kMapW)) + 2,
                 mapY + static_cast<int>(map_detail::latToY(t.latitudeDeg, MapFlyout::kMapH)) - 1);
    Fl::e_state = FL_BUTTON1;
    CHECK(send(*fly, FL_PUSH) == 1);
    REQUIRE(picks.size() >= 3);
    CHECK(picks.back().first == doctest::Approx(t.latitudeDeg));
    CHECK(picks.back().second == doctest::Approx(t.longitudeDeg));
    Fl::e_state = 0;
    CHECK(send(*fly, FL_RELEASE) == 1);

    // setPlace is the parent reflecting the observer: pin moves, nothing fires.
    const std::size_t before = picks.size();
    fly->setPlace(48.85, 2.35);
    CHECK(fly->pinLat() == doctest::Approx(48.85));
    CHECK(fly->pinLon() == doctest::Approx(2.35));
    CHECK(picks.size() == before);

    Fl::pushed(nullptr);
    Fl::belowmouse(nullptr);
}

TEST_CASE("texture dialog: a map-pin pick drives the observer like the old city pick") {
    TextureGeneratorDialog dlg(TextureGenHost{});
    dlg.seed(64, 48, std::nullopt);
    dlg.openSectionForTest("sky:solar"); // builds the Place row (the MapPicker)
    dlg.selectMoonSource(2);             // manual moon: the observer controls drive the sun alone
    dlg.setObserver(2026, 6, 21, 12.0, 0.0, 0.0);

    MapFlyout* fly = dlg.mapFlyoutForTest();
    REQUIRE(fly != nullptr);

    // Press the pin onto Tokyo (through the snap) and check the sun followed the pick -- the
    // exact contract the city dropdown's onPick used to fulfil.
    const int tokyo = catalogIndexOf("Tokyo");
    REQUIRE(tokyo >= 0);
    const tex::CityEntry& t = tex::cityAt(static_cast<std::size_t>(tokyo));
    stagePointer(BubbleFlyout::kContentX +
                     static_cast<int>(map_detail::lonToX(t.longitudeDeg, MapFlyout::kMapW)),
                 BubbleFlyout::kPad +
                     static_cast<int>(map_detail::latToY(t.latitudeDeg, MapFlyout::kMapH)));
    Fl::e_state = FL_BUTTON1;
    CHECK(send(*fly, FL_PUSH) == 1);
    Fl::e_state = 0;
    CHECK(send(*fly, FL_RELEASE) == 1);

    const tex::SunPosition sun = tex::sunPosition(tex::UtcTime{2026, 6, 21, 12.0},
                                                  t.latitudeDeg, t.longitudeDeg);
    const auto& s = std::get<tex::SkyParams>(dlg.params().spec);
    CHECK(s.sunAzimuthDeg == doctest::Approx(sun.azimuthDeg).epsilon(0.001));
    CHECK(s.sunElevationDeg ==
          doctest::Approx(std::clamp(sun.elevationDeg, -30.0, 90.0)).epsilon(0.001));

    Fl::pushed(nullptr);
    Fl::belowmouse(nullptr);
}
