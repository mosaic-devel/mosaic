#pragma once

#include "ui/bubble_flyout.hpp" // the shared speech-bubble base
#include "ui/theme.hpp"         // Palette / activePalette (self-styling, re-theme for free)

#include <FL/Fl_Widget.H>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The Texture Generator's observer PLACE picker (the city dropdown's successor): a compact
// control showing the current place -- the nearest catalogued city's name + the coordinates --
// which opens a speech-bubble FLYOUT carrying an equirectangular world map (Natural Earth 110m
// land, public domain; see world_map_data.hpp). Click or drag on the map places a pin; the pin is
// the observer's latitude/longitude, reported live through onPick while a readout under the map
// gives the coordinates + the nearest catalogued city (core/texture/city_catalog.hpp -- the
// catalogue itself is unchanged). Dragging near a catalogued city snaps the pin onto it lightly.
//
// The flyout is a ui::BubbleFlyout (the ColorFlyout/GradientFlyout chassis): a child sub-window
// of the host top-level, built BEFORE the host is shown (the ui::Popover rule), dismissed by the
// host's outside-click forwarding (dismissActiveMapFlyoutOnOutsideClick) and by Esc. Both widgets
// draw from the live palette, so a runtime re-theme follows for free.
namespace mosaic::ui {

// The pure map/place logic (no FLTK event state, unit-tested headlessly -- the date_detail
// precedent). The projection is plain equirectangular: x in [0, mapW] spans longitude -180..+180
// (+E right), y in [0, mapH] spans latitude +90..-90 (+N up).
namespace map_detail {

[[nodiscard]] double lonToX(double lonDeg, double mapW) noexcept;
[[nodiscard]] double latToY(double latDeg, double mapH) noexcept;
// Inverse mapping; the coordinate is clamped into the map first, so an event that strays past
// the edge still yields a legal longitude/latitude.
[[nodiscard]] double xToLon(double x, double mapW) noexcept;
[[nodiscard]] double yToLat(double y, double mapH) noexcept;

// The land mask at (w x h): row-major bytes, 1 = land, 0 = water. Even-odd scanline fill over
// the world_map_data rings (outer coasts and holes -- e.g. the Caspian -- listed alike, so
// even-odd needs no winding distinction).
[[nodiscard]] std::vector<std::uint8_t> rasterizeLandMask(int w, int h);

// The compact control's place label: the nearest catalogued city's own name when the coordinate
// IS that city (within the same ~15 km tolerance the old city dropdown used), else
// "Nearest: <city>" -- preserving the dropdown's showNearest semantics.
[[nodiscard]] std::string placeName(double latDeg, double lonDeg);

} // namespace map_detail

// The world-map flyout. Fully custom-drawn (no child widgets): the bubble chrome, the palette-
// tinted land/water raster with coastline + 30-degree graticule, the catalogued-city dots, the
// pin, and a live readout line (coordinates + nearest city + distance).
class MapFlyout : public BubbleFlyout {
public:
    static constexpr int kMapW = 360; // the map raster (2:1 equirectangular, ~1 px per degree)
    static constexpr int kMapH = 180;
    static constexpr double kSnapPx = 5.0; // pin snaps onto a catalogued city this close (px)

    MapFlyout();

    // Fired live as the USER places or drags the pin (already snapped), with (lat +N, lon +E).
    // Not fired by setPlace()/openFor() -- those are the parent driving the display.
    void setOnPick(std::function<void(double latitudeDeg, double longitudeDeg)> cb) {
        m_onPick = std::move(cb);
    }

    // Point the bubble at `anchor` (a widget under the same top-level), seed the pin, and show.
    void openFor(const Fl_Widget* anchor, double latitudeDeg, double longitudeDeg);

    // Reflect the observer moving from elsewhere (typed coordinates, a preset); no onPick.
    void setPlace(double latitudeDeg, double longitudeDeg);

    [[nodiscard]] bool shownForAnchor(const Fl_Widget* a) { return m_anchor == a && shown(); }
    void hide() override;

    // The pin (also the headless-test surface).
    [[nodiscard]] double pinLat() const noexcept { return m_lat; }
    [[nodiscard]] double pinLon() const noexcept { return m_lon; }

protected:
    void drawContent() override;
    int handle(int event) override;   // pin placement; chains BubbleFlyout (Esc, body opacity)
    void moveContent(int) override {} // fully drawn -- the draw reads contentLeft() live

private:
    [[nodiscard]] int mapX() const { return contentLeft(); }
    [[nodiscard]] int mapY() const { return kPad; }
    [[nodiscard]] bool insideMap(int wx, int wy) const;
    void pinFromEvent(); // window-local event -> (snapped) pin + onPick
    void rebuildImage(); // land/water/coast/graticule RGB in the current palette inks

    std::function<void(double, double)> m_onPick;
    double m_lat = 0.0;
    double m_lon = 0.0;
    bool m_dragging = false;
    std::vector<std::uint8_t> m_mask; // kMapW*kMapH, 1 = land (rasterized once)
    std::vector<std::uint8_t> m_rgb;  // kMapW*kMapH*3, palette-tinted (rebuilt on re-theme)
    common::Color8 m_rgbLand{};       // the ink the buffer was last built with (change detector)
};

// The compact in-section control: the place label + coordinates and a small globe glyph; a click
// (the full press/release pair -- the house convention, see DatePicker) fires onOpen, which the
// host answers by opening the MapFlyout anchored here.
class MapPicker : public Fl_Widget {
public:
    MapPicker(int X, int Y, int W, int H);

    // Fired by a click on the control (release inside); the host opens/toggles the flyout.
    void setOnOpen(std::function<void()> cb) { m_onOpen = std::move(cb); }

    // Reflect a coordinate: label with the nearest catalogued city (map_detail::placeName) and
    // remember the values for display. Parent-driven; never fires onOpen.
    void showNearest(double latitudeDeg, double longitudeDeg);

    [[nodiscard]] double latDeg() const noexcept { return m_lat; }
    [[nodiscard]] double lonDeg() const noexcept { return m_lon; }
    // "London" or "Nearest: London" (the coordinates are appended only in the drawn text).
    [[nodiscard]] const std::string& placeLabel() const noexcept { return m_label; }

protected:
    void draw() override;
    int handle(int event) override;

private:
    std::function<void()> m_onOpen;
    double m_lat = 0.0;
    double m_lon = 0.0;
    std::string m_label;
    bool m_hover = false;
};

// The flyout currently shown (at most one), or nullptr -- the ColorFlyout/DropdownPopup
// convention, so the host's handle() can route outside-click dismissal.
[[nodiscard]] MapFlyout* activeMapFlyout();
void dismissActiveMapFlyoutOnOutsideClick(int hostX, int hostY);
void dismissActiveMapFlyout();

} // namespace mosaic::ui
