#include "ui/icon_pack.hpp"

#include "common/image_svg.hpp"
#include "common/log.hpp"
#include "io/detail.hpp" // decodePng: raster pack icons ride the shipped PNG decoder

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>

// The embedded default pack (assets/default_tools/ -> generated headers; see src/ui/CMakeLists).
#include <assets/dt_blur_svg.hpp>
#include <assets/dt_brush_svg.hpp>
#include <assets/dt_bucket_fill_svg.hpp>
#include <assets/dt_burn_svg.hpp>
#include <assets/dt_clone_stamp_svg.hpp>
#include <assets/dt_crop_svg.hpp>
#include <assets/dt_dodge_svg.hpp>
#include <assets/dt_edge_brush_svg.hpp>
#include <assets/dt_eraser_svg.hpp>
#include <assets/dt_eyedropper_svg.hpp>
#include <assets/dt_gradient_svg.hpp>
#include <assets/dt_hand_svg.hpp>
#include <assets/dt_heal_svg.hpp>
#include <assets/dt_inpaint_brush_svg.hpp>
#include <assets/dt_lasso_magnetic_svg.hpp>
#include <assets/dt_lasso_polygon_svg.hpp>
#include <assets/dt_lasso_svg.hpp>
#include <assets/dt_magic_wand_svg.hpp>
#include <assets/dt_marquee_ellipse_svg.hpp>
#include <assets/dt_marquee_rect_svg.hpp>
#include <assets/dt_move_svg.hpp>
#include <assets/dt_pack_json.hpp>
#include <assets/dt_pen_path_svg.hpp>
#include <assets/dt_red_eye_svg.hpp>
#include <assets/dt_selection_brush_svg.hpp>
#include <assets/dt_shape_arrow_svg.hpp>
#include <assets/dt_shape_banner_svg.hpp>
#include <assets/dt_shape_callout_svg.hpp>
#include <assets/dt_shape_cross_svg.hpp>
#include <assets/dt_shape_ellipse_svg.hpp>
#include <assets/dt_shape_heart_svg.hpp>
#include <assets/dt_shape_line_svg.hpp>
#include <assets/dt_shape_polygon_svg.hpp>
#include <assets/dt_shape_rect_svg.hpp>
#include <assets/dt_shape_ring_svg.hpp>
#include <assets/dt_shape_star_svg.hpp>
#include <assets/dt_smudge_svg.hpp>
#include <assets/dt_text_svg.hpp>
#include <assets/dt_warp_svg.hpp>
#include <assets/dt_zoom_svg.hpp>

namespace mosaic::ui {
namespace {

using json = nlohmann::json;

spdlog::logger& uiLog() {
    static const auto logger = common::log::category("ui");
    return *logger;
}

struct EmbeddedIcon {
    std::string_view key;
    const unsigned char* data;
    std::size_t size;
};

#define MOSAIC_DT_ICON(k) \
    EmbeddedIcon { #k, mosaic::assets::dt_##k##_svg, mosaic::assets::dt_##k##_svg_size }

// Display order: implemented toolset first, future tools after (icon_pack.hpp's contract).
constexpr auto embeddedIcons() {
    return std::array{
        MOSAIC_DT_ICON(move),         MOSAIC_DT_ICON(marquee_rect),
        MOSAIC_DT_ICON(marquee_ellipse), MOSAIC_DT_ICON(lasso),
        MOSAIC_DT_ICON(lasso_polygon), MOSAIC_DT_ICON(crop),
        MOSAIC_DT_ICON(brush),        MOSAIC_DT_ICON(eraser),
        MOSAIC_DT_ICON(inpaint_brush), MOSAIC_DT_ICON(bucket_fill),
        MOSAIC_DT_ICON(gradient),     MOSAIC_DT_ICON(eyedropper),
        MOSAIC_DT_ICON(shape_rect),   MOSAIC_DT_ICON(shape_ellipse),
        MOSAIC_DT_ICON(shape_polygon), MOSAIC_DT_ICON(shape_star),
        MOSAIC_DT_ICON(shape_line),
        // The S26-c shape library: no ToolId of its own yet (the kinds are picked in the shape
        // designer's gallery), but they are pack keys like any other tool glyph, so a pack can
        // restyle them and the gallery draws them through the same renderIcon path.
        MOSAIC_DT_ICON(shape_callout), MOSAIC_DT_ICON(shape_arrow),
        MOSAIC_DT_ICON(shape_ring),   MOSAIC_DT_ICON(shape_cross),
        MOSAIC_DT_ICON(shape_heart),  MOSAIC_DT_ICON(shape_banner),
        MOSAIC_DT_ICON(text),
        MOSAIC_DT_ICON(zoom),         MOSAIC_DT_ICON(lasso_magnetic),
        MOSAIC_DT_ICON(magic_wand),   MOSAIC_DT_ICON(selection_brush),
        MOSAIC_DT_ICON(edge_brush),   MOSAIC_DT_ICON(blur),
        MOSAIC_DT_ICON(dodge),
        MOSAIC_DT_ICON(burn),         MOSAIC_DT_ICON(smudge),
        MOSAIC_DT_ICON(pen_path),     MOSAIC_DT_ICON(heal),
        MOSAIC_DT_ICON(red_eye),      MOSAIC_DT_ICON(clone_stamp),
        MOSAIC_DT_ICON(hand),         MOSAIC_DT_ICON(warp),
    };
}
#undef MOSAIC_DT_ICON

// Parse a manifest held in memory. Third-party input: total, defaults over failures -- a pack
// with a half-written manifest still lists (name falls back to the folder), only a folder with
// NO parseable manifest is rejected (the manifest is the pack marker).
bool readManifest(const std::string& text, IconPackInfo& info) {
    const json doc = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object())
        return false;
    const auto str = [&](const char* key) -> std::string {
        const auto it = doc.find(key);
        return it != doc.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    if (std::string n = str("name"); !n.empty())
        info.name = std::move(n);
    info.artist = str("artist");
    info.link = str("link");
    info.description = str("description");
    info.license = str("license");
    return true;
}

std::optional<std::string> readCapped(const std::filesystem::path& path, std::size_t cap) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > cap)
        return std::nullopt;
    std::ifstream f(path, std::ios::binary);
    if (!f.good())
        return std::nullopt;
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!f.read(text.data(), static_cast<std::streamsize>(text.size())).good())
        return std::nullopt;
    return text;
}

// Fit `src` inside a px-square with transparent margins, alpha-weighted area averaging. The
// weighting matters: a straight average drags the RGB of fully-transparent neighbours (usually
// black) into every AA edge, darkening raster icons on downscale; weighting by alpha averages
// only the paint that is actually there.
common::Image scaleToFit(const common::Image& src, int px) {
    common::Image out(static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(px));
    const double scale = std::min(static_cast<double>(px) / src.width,
                                  static_cast<double>(px) / src.height);
    const int outW = std::max(1, static_cast<int>(std::lround(src.width * scale)));
    const int outH = std::max(1, static_cast<int>(std::lround(src.height * scale)));
    const int offX = (px - outW) / 2;
    const int offY = (px - outH) / 2;
    for (int oy = 0; oy < outH; ++oy) {
        for (int ox = 0; ox < outW; ++ox) {
            // The source rect this destination pixel covers (fractional edges included).
            const double x0 = ox * static_cast<double>(src.width) / outW;
            const double x1 = (ox + 1) * static_cast<double>(src.width) / outW;
            const double y0 = oy * static_cast<double>(src.height) / outH;
            const double y1 = (oy + 1) * static_cast<double>(src.height) / outH;
            double sumR = 0, sumG = 0, sumB = 0, sumA = 0, area = 0;
            for (int sy = static_cast<int>(y0); sy < static_cast<int>(std::ceil(y1)); ++sy) {
                const double hCov = std::min<double>(y1, sy + 1) - std::max<double>(y0, sy);
                for (int sx = static_cast<int>(x0); sx < static_cast<int>(std::ceil(x1)); ++sx) {
                    const double cov =
                        hCov * (std::min<double>(x1, sx + 1) - std::max<double>(x0, sx));
                    const std::size_t si =
                        (static_cast<std::size_t>(sy) * src.width + static_cast<std::size_t>(sx))
                        * 4;
                    const double a = src.rgba[si + 3] * cov;
                    sumR += src.rgba[si] * a;
                    sumG += src.rgba[si + 1] * a;
                    sumB += src.rgba[si + 2] * a;
                    sumA += a;
                    area += cov;
                }
            }
            const std::size_t di =
                (static_cast<std::size_t>(offY + oy) * out.width
                 + static_cast<std::size_t>(offX + ox))
                * 4;
            if (sumA > 0.0) {
                out.rgba[di] = static_cast<std::uint8_t>(std::lround(sumR / sumA));
                out.rgba[di + 1] = static_cast<std::uint8_t>(std::lround(sumG / sumA));
                out.rgba[di + 2] = static_cast<std::uint8_t>(std::lround(sumB / sumA));
            }
            out.rgba[di + 3] =
                static_cast<std::uint8_t>(std::lround(std::clamp(sumA / area, 0.0, 255.0)));
        }
    }
    return out;
}

} // namespace

std::string_view iconKeyFor(ToolId id) noexcept {
    switch (id) {
    case ToolId::Move: return "move";
    case ToolId::RectMarquee: return "marquee_rect";
    case ToolId::EllipseMarquee: return "marquee_ellipse";
    case ToolId::Lasso: return "lasso";
    case ToolId::PolygonLasso: return "lasso_polygon";
    case ToolId::MagicWand: return "magic_wand";
    case ToolId::SelectBrush: return "selection_brush";
    case ToolId::EdgeBrush: return "edge_brush";
    case ToolId::Crop: return "crop";
    // S35-b: the pack has carried `warp` art since S52 as a reserved key, and the tools it was
    // reserved for now exist, so they claim it -- no new SVG, no embed-list entry, no census bump
    // (the clone stamp's own S38 move). BOTH variants wear it: a "warp_perspective" glyph is OWED
    // (the same debt red_eye_sclera carries), and until it lands a flyout row is told apart by its
    // name, which is legible but not the intent.
    case ToolId::MeshWarp: return "warp";
    case ToolId::PerspectiveWarp: return "warp";
    case ToolId::Brush: return "brush";
    case ToolId::Eraser: return "eraser";
    case ToolId::InpaintBrush: return "inpaint_brush";
    // S38: the pack has carried `clone_stamp` art since S52 as a reserved key; the tool it was
    // reserved for now exists, so it simply claims it -- no new SVG, no embed-list entry, no census
    // bump. (`heal` stays reserved: healing is S39's, and it is a different glyph on purpose.)
    case ToolId::CloneStamp: return "clone_stamp";
    // S38-b: both eye-tool variants wear the pack's reserved red_eye art for now. The sclera
    // variant is OWED a glyph of its own ("red_eye_sclera", docs/icons-needed.md) -- adding it
    // means a new SVG, a new key in the embed list, and a bump of the pack census; until then a
    // flyout row is told apart by its name, which is legible but not the intent.
    case ToolId::RedEye: return "red_eye";
    case ToolId::RedEyeSclera: return "red_eye";
    case ToolId::BucketFill: return "bucket_fill";
    case ToolId::Gradient: return "gradient";
    case ToolId::Eyedropper: return "eyedropper";
    case ToolId::RectShape: return "shape_rect";
    case ToolId::EllipseShape: return "shape_ellipse";
    case ToolId::PolygonShape: return "shape_polygon";
    case ToolId::StarShape: return "shape_star";
    case ToolId::LineShape: return "shape_line";
    case ToolId::Pen: return "pen_path";
    case ToolId::Text: return "text";
    case ToolId::Zoom: return "zoom";
    }
    return "move"; // unreachable for a valid id; a safe glyph beats a broken button
}

std::string_view defaultIconSvg(ToolId id) noexcept {
    const std::string_view key = iconKeyFor(id);
    for (const EmbeddedIcon& icon : embeddedIcons())
        if (icon.key == key)
            return {reinterpret_cast<const char*>(icon.data), icon.size};
    return {}; // unreachable: iconKeyFor only returns embedded keys
}

const std::vector<std::string>& allIconKeys() {
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> v;
        for (const EmbeddedIcon& icon : embeddedIcons())
            v.emplace_back(icon.key);
        return v;
    }();
    return keys;
}

IconPacks::IconPacks() {
    IconPackInfo def;
    def.id = std::string(kDefaultIconPackId);
    def.name = "Default";
    def.builtin = true;
    readManifest(std::string(reinterpret_cast<const char*>(mosaic::assets::dt_pack_json),
                             mosaic::assets::dt_pack_json_size),
                 def);
    m_packs.push_back(std::move(def));
}

int IconPacks::scan(const std::filesystem::path& dir) {
    m_packs.resize(1); // the built-in default survives every rescan
    m_cache.clear();
    int rejected = 0;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec)
        return 0; // no user pack dir yet: just the default
    for (const auto& entry : it) {
        if (!entry.is_directory(ec))
            continue;
        const std::filesystem::path manifest = entry.path() / kIconPackManifest;
        if (!std::filesystem::exists(manifest, ec))
            continue; // an ordinary folder, not a pack -- not worth counting as rejected
        IconPackInfo info;
        info.id = entry.path().filename().string();
        info.name = info.id; // manifest name wins when present
        info.dir = entry.path();
        const auto text = readCapped(manifest, kMaxIconFileBytes);
        if (!text || !readManifest(*text, info)) {
            ++rejected;
            uiLog().warn("icon pack '{}' rejected: unreadable or invalid {}", info.id,
                         std::string(kIconPackManifest));
            continue;
        }
        if (info.id == kDefaultIconPackId) { // a user folder cannot shadow the built-in fallback
            ++rejected;
            uiLog().warn("icon pack folder 'default' ignored: the id is reserved");
            continue;
        }
        m_packs.push_back(std::move(info));
    }
    return rejected;
}

const IconPackInfo* IconPacks::find(std::string_view id) const {
    for (const IconPackInfo& p : m_packs)
        if (p.id == id)
            return &p;
    return nullptr;
}

const IconSource& IconPacks::iconFor(std::string_view packId, std::string_view key) {
    static const IconSource kEmpty;
    // The embedded default answers directly (and is the shared fallback).
    const auto embedded = [&]() -> const IconSource& {
        for (const EmbeddedIcon& icon : embeddedIcons())
            if (icon.key == key) {
                const std::string cacheKey = std::string(kDefaultIconPackId) + "/"
                                             + std::string(key);
                if (const auto it = m_cache.find(cacheKey); it != m_cache.end())
                    return it->second;
                IconSource src;
                src.svg.assign(reinterpret_cast<const char*>(icon.data), icon.size);
                return m_cache.emplace(cacheKey, std::move(src)).first->second;
            }
        return kEmpty; // a key even the default lacks
    };
    if (packId == kDefaultIconPackId)
        return embedded();
    const IconPackInfo* pack = find(packId);
    if (pack == nullptr || pack->dir.empty())
        return embedded();
    const std::string cacheKey = pack->id + "/" + std::string(key);
    if (const auto it = m_cache.find(cacheKey); it != m_cache.end())
        return it->second.empty() ? embedded() : it->second; // negative results cache too
    // Vector first (it scales), raster second; either capped -- these are third-party files.
    IconSource src;
    if (auto svg = readCapped(pack->dir / (std::string(key) + ".svg"), kMaxIconFileBytes))
        src.svg = std::move(*svg);
    else if (auto png = readCapped(pack->dir / (std::string(key) + ".png"), kMaxIconFileBytes))
        src.png.assign(png->begin(), png->end());
    const auto [it, inserted] = m_cache.emplace(cacheKey, std::move(src));
    return it->second.empty() ? embedded() : it->second;
}

common::Image IconPacks::renderIcon(std::string_view packId, std::string_view key, int px) {
    std::string err;
    common::Image img = renderIconSource(iconFor(packId, key), px, &err);
    if (img.empty())
        uiLog().warn("icon '{}' of pack '{}' failed to render: {}", std::string(key),
                     std::string(packId), err);
    return img;
}

common::Image renderIconSource(const IconSource& icon, int px, std::string* error) {
    if (!icon.svg.empty())
        return common::rasterizeSvg(reinterpret_cast<const unsigned char*>(icon.svg.data()),
                                    icon.svg.size(), px, px, error);
    if (!icon.png.empty()) {
        std::string err;
        const auto decoded = io::detail::decodePng(icon.png, &err);
        if (!decoded || decoded->empty()) {
            if (error != nullptr)
                *error = err.empty() ? "PNG decode failed" : err;
            return {};
        }
        if (decoded->width > kMaxIconPngEdge || decoded->height > kMaxIconPngEdge) {
            if (error != nullptr)
                *error = "raster icon exceeds the decoded-size cap";
            return {};
        }
        return scaleToFit(*decoded, px);
    }
    if (error != nullptr)
        *error = "empty icon source";
    return {};
}

} // namespace mosaic::ui
