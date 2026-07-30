#include "ui/map_picker.hpp"

#include "core/texture/city_catalog.hpp"
#include "ui/widgets.hpp" // drawAAArcs: the pin + globe glyph, anti-aliased (fl_arc is not)
#include "ui/world_map_data.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace mosaic::ui {
namespace {

namespace tex = core::texture;

Fl_Color toFl(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }

common::Color8 mix(common::Color8 a, common::Color8 b, float t) {
    const auto ch = [t](std::uint8_t av, std::uint8_t bv) {
        return static_cast<std::uint8_t>(std::lround(av + (bv - av) * t));
    };
    return {ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), 255};
}

// A listed city matches an entered coordinate when it is essentially the same point -- the same
// ~15 km tolerance the retired city dropdown used (far below the smallest inter-city spacing in
// the catalogue, so it never mislabels one city as another).
constexpr double kMatchKm = 15.0;

// Flyout geometry: the bubble body wraps the map raster plus one readout line.
constexpr int kPad = BubbleFlyout::kPad;
constexpr int kContentX = BubbleFlyout::kContentX;
constexpr int kReadoutGap = 6;
constexpr int kReadoutH = 14;
constexpr int kWinW = kContentX + MapFlyout::kMapW + kPad;
constexpr int kWinH = kPad + MapFlyout::kMapH + kReadoutGap + kReadoutH + kPad;

// The flyout currently shown (at most one) -- the ColorFlyout/DropdownPopup convention.
MapFlyout* g_activeMapFlyout = nullptr;

} // namespace

// ================================================================================================
// map_detail -- the pure projection / mask / label logic (GUI-free, unit-tested)
// ================================================================================================
namespace map_detail {

double lonToX(double lonDeg, double mapW) noexcept { return (lonDeg + 180.0) / 360.0 * mapW; }
double latToY(double latDeg, double mapH) noexcept { return (90.0 - latDeg) / 180.0 * mapH; }

double xToLon(double x, double mapW) noexcept {
    return std::clamp(x, 0.0, mapW) / mapW * 360.0 - 180.0;
}
double yToLat(double y, double mapH) noexcept {
    return 90.0 - std::clamp(y, 0.0, mapH) / mapH * 180.0;
}

std::vector<std::uint8_t> rasterizeLandMask(int w, int h) {
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(w) * h, 0);
    std::vector<double> xs;
    for (int py = 0; py < h; ++py) {
        const double lat = yToLat(py + 0.5, h); // the row's centre latitude
        xs.clear();
        // Every ring edge crossing this latitude contributes one crossing; even-odd pairs of the
        // sorted crossings are the land spans (holes fall out for free).
        for (std::size_t r = 0; r < worldmap::mapRingCount(); ++r) {
            const worldmap::MapRing& ring = worldmap::mapRings()[r];
            for (std::uint32_t i = 0; i < ring.count; ++i) {
                const worldmap::MapPoint& a = worldmap::mapPoints()[ring.first + i];
                const worldmap::MapPoint& b =
                    worldmap::mapPoints()[ring.first + (i + 1) % ring.count];
                const double ay = a.latCenti / 100.0;
                const double by = b.latCenti / 100.0;
                if ((ay <= lat && by > lat) || (by <= lat && ay > lat)) {
                    const double t = (lat - ay) / (by - ay);
                    const double lon = a.lonCenti / 100.0 + t * (b.lonCenti - a.lonCenti) / 100.0;
                    xs.push_back(lonToX(lon, w));
                }
            }
        }
        std::sort(xs.begin(), xs.end());
        for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
            const int x0 = std::max(0, static_cast<int>(std::ceil(xs[i] - 0.5)));
            const int x1 = std::min(w - 1, static_cast<int>(std::floor(xs[i + 1] - 0.5)));
            for (int x = x0; x <= x1; ++x)
                mask[static_cast<std::size_t>(py) * w + x] = 1;
        }
    }
    return mask;
}

std::string placeName(double latDeg, double lonDeg) {
    const tex::NearestCity near = tex::nearestCity(latDeg, lonDeg);
    const tex::CityEntry& c = tex::cityAt(near.index);
    if (near.distanceKm <= kMatchKm)
        return c.name; // the coordinate IS this city: its own name
    return std::string("Nearest: ") + c.name;
}

} // namespace map_detail

// ================================================================================================
// MapFlyout
// ================================================================================================
MapFlyout::MapFlyout() : BubbleFlyout(kWinW, kWinH) {
    end(); // fully custom-drawn; no child widgets
    m_mask = map_detail::rasterizeLandMask(kMapW, kMapH);
}

void MapFlyout::openFor(const Fl_Widget* anchor, double latitudeDeg, double longitudeDeg) {
    m_lat = std::clamp(latitudeDeg, -90.0, 90.0);
    m_lon = std::clamp(longitudeDeg, -180.0, 180.0);
    placeBubble(anchor);
    show();
    g_activeMapFlyout = this;
    redraw();
}

void MapFlyout::setPlace(double latitudeDeg, double longitudeDeg) {
    m_lat = std::clamp(latitudeDeg, -90.0, 90.0);
    m_lon = std::clamp(longitudeDeg, -180.0, 180.0);
    redraw();
}

void MapFlyout::hide() {
    if (g_activeMapFlyout == this)
        g_activeMapFlyout = nullptr;
    m_anchor = nullptr;
    m_dragging = false;
    Fl_Double_Window::hide();
}

bool MapFlyout::insideMap(int wx, int wy) const {
    return wx >= mapX() && wx < mapX() + kMapW && wy >= mapY() && wy < mapY() + kMapH;
}

void MapFlyout::pinFromEvent() {
    // Window-local event coordinates (a child sub-window receives them translated; the
    // DropdownPopup convention), clamped into the map so an edge-grazing drag stays legal.
    const double mx = std::clamp<double>(Fl::event_x() - mapX(), 0.0, kMapW);
    const double my = std::clamp<double>(Fl::event_y() - mapY(), 0.0, kMapH);
    double lat = map_detail::yToLat(my, kMapH);
    double lon = map_detail::xToLon(mx, kMapW);
    // A light snap: when the pointer is within kSnapPx of the nearest catalogued city's dot, the
    // pin lands exactly on the city (so the readout names it at 0 km).
    const tex::NearestCity near = tex::nearestCity(lat, lon);
    const tex::CityEntry& c = tex::cityAt(near.index);
    const double cx = map_detail::lonToX(c.longitudeDeg, kMapW);
    const double cy = map_detail::latToY(c.latitudeDeg, kMapH);
    if (std::hypot(cx - mx, cy - my) <= kSnapPx) {
        lat = c.latitudeDeg;
        lon = c.longitudeDeg;
    }
    if (lat != m_lat || lon != m_lon) {
        m_lat = lat;
        m_lon = lon;
        redraw();
        if (m_onPick)
            m_onPick(m_lat, m_lon);
    }
}

void MapFlyout::rebuildImage() {
    const Palette& pal = activePalette();
    const common::Color8 water = pal.windowBg;
    const common::Color8 land = pal.controlHover;
    const common::Color8 grid = mix(water, pal.border, 0.45f);
    const common::Color8 coast = pal.textMuted;
    m_rgbLand = land;
    m_rgb.assign(static_cast<std::size_t>(kMapW) * kMapH * 3, 0);
    const auto put = [&](int x, int y, common::Color8 c) {
        std::uint8_t* p = &m_rgb[(static_cast<std::size_t>(y) * kMapW + x) * 3];
        p[0] = c.r;
        p[1] = c.g;
        p[2] = c.b;
    };
    const auto landAt = [&](int x, int y) {
        return m_mask[static_cast<std::size_t>(y) * kMapW + x] != 0;
    };
    for (int y = 0; y < kMapH; ++y)
        for (int x = 0; x < kMapW; ++x)
            put(x, y, landAt(x, y) ? land : water);
    // Graticule every 30 degrees (skipping the map edges), blended so it stays subordinate.
    for (int g = -150; g <= 150; g += 30) {
        const int x = static_cast<int>(map_detail::lonToX(g, kMapW));
        for (int y = 0; y < kMapH; ++y)
            put(x, y, mix(landAt(x, y) ? land : water, grid, 0.6f));
    }
    for (int g = -60; g <= 60; g += 30) {
        const int y = static_cast<int>(map_detail::latToY(g, kMapH));
        for (int x = 0; x < kMapW; ++x)
            put(x, y, mix(landAt(x, y) ? land : water, grid, 0.6f));
    }
    // Coastline: land pixels with a water 4-neighbour.
    for (int y = 0; y < kMapH; ++y) {
        for (int x = 0; x < kMapW; ++x) {
            if (!landAt(x, y))
                continue;
            const bool edge = (x > 0 && !landAt(x - 1, y)) || (x < kMapW - 1 && !landAt(x + 1, y)) ||
                              (y > 0 && !landAt(x, y - 1)) || (y < kMapH - 1 && !landAt(x, y + 1));
            if (edge)
                put(x, y, coast);
        }
    }
}

void MapFlyout::drawContent() {
    const Palette& pal = activePalette();
    drawBubbleChrome();

    // The palette-tinted raster is cached; a re-theme changes the land ink and forces a rebuild.
    const common::Color8 land = pal.controlHover;
    if (m_rgb.empty() || land.r != m_rgbLand.r || land.g != m_rgbLand.g || land.b != m_rgbLand.b)
        rebuildImage();
    Fl_RGB_Image img(m_rgb.data(), kMapW, kMapH, 3);
    img.draw(mapX(), mapY());
    fl_rect(mapX(), mapY(), kMapW, kMapH, toFl(pal.border));

    // The catalogued cities, as quiet dots (they are also the snap targets).
    fl_color(toFl(pal.textMuted));
    for (std::size_t i = 0; i < tex::cityCatalogCount(); ++i) {
        const tex::CityEntry& c = tex::cityAt(i);
        const int dx = mapX() + static_cast<int>(map_detail::lonToX(c.longitudeDeg, kMapW));
        const int dy = mapY() + static_cast<int>(map_detail::latToY(c.latitudeDeg, kMapH));
        fl_rectf(dx - 1, dy - 1, 2, 2);
    }

    // The pin: crosshair ticks + an accent disc with a bright core, clamped onto the raster.
    const int px = mapX() + std::clamp(static_cast<int>(std::lround(
                                           map_detail::lonToX(m_lon, kMapW))),
                                       0, kMapW - 1);
    const int py = mapY() + std::clamp(static_cast<int>(std::lround(
                                           map_detail::latToY(m_lat, kMapH))),
                                       0, kMapH - 1);
    fl_color(toFl(pal.accent));
    fl_line(px - 9, py, px - 5, py);
    fl_line(px + 5, py, px + 9, py);
    fl_line(px, py - 9, px, py - 5);
    fl_line(px, py + 5, px, py + 9);
    // Disc + core in ONE anti-aliased patch (concentric, so they cannot be composed separately).
    // `under` is the world raster the pin is clamped onto, plus the hairline frame where the pin sits
    // on the very edge of it, plus the bubble body if the patch reaches past that. The crosshair ticks
    // start 5 px out and this patch is exactly the 7 px disc box, so they are never in it.
    const auto under = [&](int ux, int uy) -> common::Color8 {
        const int lx = ux - mapX();
        const int ly = uy - mapY();
        if (lx < 0 || ly < 0 || lx >= kMapW || ly >= kMapH)
            return pal.panelBg;
        if (lx == 0 || ly == 0 || lx == kMapW - 1 || ly == kMapH - 1)
            return pal.border; // the raster's own frame, drawn over its outermost ring
        const std::size_t o = (static_cast<std::size_t>(ly) * kMapW + lx) * 3;
        return {m_rgb[o], m_rgb[o + 1], m_rgb[o + 2], 255};
    };
    drawAAArcs(under, {aaPieFromBox(px - 3, py - 3, 7, 7, 0, 360, pal.accent),
                       aaPieFromBox(px - 1, py - 1, 3, 3, 0, 360, pal.onAccent)});

    // Live readout: coordinates + the nearest catalogued city and its distance.
    const tex::NearestCity near = tex::nearestCity(m_lat, m_lon);
    const tex::CityEntry& c = tex::cityAt(near.index);
    char b[176];
    std::snprintf(b, sizeof(b),
                  "%.1f\xC2\xB0%c, %.1f\xC2\xB0%c \xC2\xB7 %s, %s \xC2\xB7 %.0f km",
                  std::abs(m_lat), m_lat >= 0.0 ? 'N' : 'S', std::abs(m_lon),
                  m_lon >= 0.0 ? 'E' : 'W', c.name, c.country, near.distanceKm);
    fl_font(FL_HELVETICA, 11);
    fl_color(toFl(pal.text));
    fl_push_clip(mapX(), mapY() + kMapH + kReadoutGap, kMapW, kReadoutH);
    fl_draw(b, mapX(), mapY() + kMapH + kReadoutGap, kMapW, kReadoutH,
            FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    fl_pop_clip();
}

int MapFlyout::handle(int event) {
    switch (event) {
    case FL_PUSH:
        if (insideMap(Fl::event_x(), Fl::event_y())) {
            m_dragging = true;
            pinFromEvent();
            return 1;
        }
        break;
    case FL_DRAG:
        if (m_dragging) {
            pinFromEvent();
            return 1;
        }
        break;
    case FL_RELEASE:
        if (m_dragging) {
            m_dragging = false;
            return 1; // claim the pair: the gesture ends with the widget that began it
        }
        break;
    default:
        break;
    }
    return BubbleFlyout::handle(event); // Esc dismiss + the bubble body's press opacity
}

// ================================================================================================
// MapPicker (the compact in-section control)
// ================================================================================================
MapPicker::MapPicker(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {
    box(MOSAIC_INPUT_BOX);
    showNearest(0.0, 0.0);
}

void MapPicker::showNearest(double latitudeDeg, double longitudeDeg) {
    m_lat = std::clamp(latitudeDeg, -90.0, 90.0);
    m_lon = std::clamp(longitudeDeg, -180.0, 180.0);
    m_label = map_detail::placeName(m_lat, m_lon);
    redraw();
}

void MapPicker::draw() {
    const Palette& p = activePalette();
    const bool on = active_r();
    draw_box(MOSAIC_INPUT_BOX, x(), y(), w(), h(), toFl(on && m_hover ? p.controlHover : p.controlBg));

    // A small globe glyph on the right (hand-drawn -- the no-Unicode-in-labels rule): a circle
    // with the equator and one meridian ellipse.
    const int gS = 13;
    const int gx = x() + w() - gS - 8;
    const int gy = y() + (h() - gS) / 2;
    const common::Color8 glyphInk = on ? p.textMuted : p.border;
    const common::Color8 glyphBg = on && m_hover ? p.controlHover : p.controlBg;
    // The equator goes down FIRST now: it stops a pixel short of the outer ring either side, so it
    // never touched that ring anyway, and the swap lets both rings -- which DO meet, at the poles --
    // share one anti-aliased patch. `under` restates the equator so the patch does not erase it.
    fl_color(toFl(glyphInk));
    fl_line(gx + 1, gy + gS / 2, gx + gS - 2, gy + gS / 2);
    const auto under = [&](int ux, int uy) {
        const bool onEquator = uy == gy + gS / 2 && ux >= gx + 1 && ux <= gx + gS - 2;
        return onEquator ? glyphInk : glyphBg;
    };
    drawAAArcs(under, {aaArcFromBox(gx, gy, gS, gS, 0, 360, 1.0, glyphInk),
                       aaArcFromBox(gx + 3, gy, gS - 6, gS, 0, 360, 1.0, glyphInk)});

    // The place + coordinates, clipped before the glyph.
    char b[176];
    std::snprintf(b, sizeof(b), "%s \xC2\xB7 %.1f, %.1f", m_label.c_str(), m_lat, m_lon);
    fl_color(toFl(on ? p.text : p.textMuted));
    fl_font(FL_HELVETICA, 12);
    const int textW = std::max(0, w() - 8 - gS - 12);
    fl_push_clip(x() + 8, y(), textW, h());
    fl_draw(b, x() + 8, y(), w() - 8, h(), FL_ALIGN_LEFT);
    fl_pop_clip();
}

int MapPicker::handle(int event) {
    switch (event) {
    case FL_ENTER:
        m_hover = true;
        redraw();
        return 1;
    case FL_LEAVE:
        m_hover = false;
        redraw();
        return 1;
    // The house click convention (CheckBox / RailItem / DatePicker): claim the WHOLE gesture and
    // act on the release that ends inside, so nothing falls through to the chrome underneath.
    case FL_PUSH:
        return 1;
    case FL_DRAG:
        return 1;
    case FL_RELEASE:
        if (Fl::event_inside(this) && m_onOpen)
            m_onOpen();
        return 1;
    default:
        return Fl_Widget::handle(event);
    }
}

// ================================================================================================
// Active-instance routing (the ColorFlyout convention)
// ================================================================================================
MapFlyout* activeMapFlyout() { return g_activeMapFlyout; }

void dismissActiveMapFlyoutOnOutsideClick(int hostX, int hostY) {
    if (g_activeMapFlyout != nullptr && !g_activeMapFlyout->spansHostPoint(hostX, hostY))
        g_activeMapFlyout->hide();
}

void dismissActiveMapFlyout() {
    if (g_activeMapFlyout != nullptr)
        g_activeMapFlyout->hide();
}

} // namespace mosaic::ui
