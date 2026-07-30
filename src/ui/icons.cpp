#include "ui/icons.hpp"

#include "common/image_svg.hpp"
#include "common/log.hpp"
#include "ui/theme.hpp"

#include <assets/icon_close_svg.hpp>
#include <assets/icon_eye_closed_svg.hpp>
#include <assets/icon_eye_open_svg.hpp>
#include <assets/icon_group_layers_svg.hpp>
#include <assets/icon_lock_closed_svg.hpp>
#include <assets/icon_lock_open_svg.hpp>
#include <assets/icon_plus_svg.hpp>
#include <assets/icon_trash_svg.hpp>

#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace mosaic::ui {
namespace {

spdlog::logger& uiLog() {
    static const auto logger = common::log::category("ui");
    return *logger;
}

struct SvgSource {
    const unsigned char* data;
    std::size_t size;
};

SvgSource sourceFor(Icon icon) {
    switch (icon) {
    case Icon::Plus: return {assets::icon_plus_svg, assets::icon_plus_svg_size};
    case Icon::Trash: return {assets::icon_trash_svg, assets::icon_trash_svg_size};
    case Icon::GroupLayers:
        return {assets::icon_group_layers_svg, assets::icon_group_layers_svg_size};
    case Icon::EyeOpen: return {assets::icon_eye_open_svg, assets::icon_eye_open_svg_size};
    case Icon::EyeClosed: return {assets::icon_eye_closed_svg, assets::icon_eye_closed_svg_size};
    case Icon::LockOpen: return {assets::icon_lock_open_svg, assets::icon_lock_open_svg_size};
    case Icon::LockClosed: return {assets::icon_lock_closed_svg, assets::icon_lock_closed_svg_size};
    case Icon::Close: return {assets::icon_close_svg, assets::icon_close_svg_size};
    }
    return {nullptr, 0};
}

// (icon, rgb) -> tinted raster. The palette hands us a handful of inks (text, textMuted, accent,
// white-on-green), so this never grows beyond a few dozen entries.
std::uint64_t cacheKey(Icon icon, common::Color8 c) {
    const auto rgb = static_cast<std::uint64_t>(c.r) << 16 | static_cast<std::uint64_t>(c.g) << 8 |
                     static_cast<std::uint64_t>(c.b);
    return (static_cast<std::uint64_t>(icon) << 32) | rgb;
}

// The cache holds PIXELS, never a live Fl_RGB_Image: ~Fl_RGB_Image uncaches through the graphics
// driver, so a long-lived one in a function-local static would run that at process exit, after
// FLTK's own statics are gone. drawIcon() wraps these pixels in a stack Fl_RGB_Image instead --
// the expensive half (parse + rasterize + tint) is what is worth keeping.
const common::Image* tintedPixels(Icon icon, common::Color8 color) {
    static std::map<std::uint64_t, common::Image> cache;
    const std::uint64_t key = cacheKey(icon, color);
    if (const auto it = cache.find(key); it != cache.end())
        return it->second.empty() ? nullptr : &it->second; // an empty entry = a known-bad asset

    const SvgSource src = sourceFor(icon);
    std::string err;
    // kIconPx, always: the art is drawn on a 16x16 grid and any other size blurs it (see icons.hpp).
    common::Image raster = common::rasterizeSvg(src.data, src.size, kIconPx, kIconPx, &err);
    if (raster.empty()) {
        // Remember the failure so a broken asset cannot re-parse (and re-log) on every draw.
        uiLog().warn("icon {} failed to rasterize: {}", static_cast<int>(icon), err);
        cache.emplace(key, common::Image{});
        return nullptr;
    }
    // The art is white-on-transparent, so the rasterizer's straight alpha IS the coverage: swap in
    // the requested ink and leave alpha alone. (nsvgRasterize un-premultiplies before returning.)
    for (std::size_t p = 0; p + 3 < raster.rgba.size(); p += 4) {
        raster.rgba[p + 0] = color.r;
        raster.rgba[p + 1] = color.g;
        raster.rgba[p + 2] = color.b;
    }
    return &cache.emplace(key, std::move(raster)).first->second;
}

} // namespace

void drawIcon(Icon icon, int cx, int cy, common::Color8 color) {
    const common::Image* pixels = tintedPixels(icon, color);
    if (pixels == nullptr)
        return;
    // Borrows the cached pixels; Fl_RGB_Image does not copy. Blitted 1:1 -- no Fl_Image::scale().
    Fl_RGB_Image img(pixels->rgba.data(), kIconPx, kIconPx, 4);
    img.draw(cx - kIconPx / 2, cy - kIconPx / 2);
}

// ---- IconButton --------------------------------------------------------------------------------

IconButton::IconButton(int X, int Y, int W, int H, Icon icon)
    : FlatButton(X, Y, W, H), m_icon(icon) {}

void IconButton::setIcon(Icon icon) {
    if (icon == m_icon)
        return;
    m_icon = icon;
    redraw();
}

void IconButton::setInk(std::optional<common::Color8> ink) {
    if (ink == m_ink)
        return;
    m_ink = ink;
    redraw();
}

void IconButton::draw() {
    Fl_Button::draw(); // the flat box + hover/toggle fills; there is no label to suppress
    const Palette& pal = activePalette();
    const common::Color8 ink = m_ink.value_or(active_r() ? pal.text : pal.textMuted);
    drawIcon(m_icon, x() + w() / 2, y() + h() / 2, ink);
}

} // namespace mosaic::ui
