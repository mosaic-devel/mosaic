#include "common/image_svg.hpp"

#include <algorithm>
#include <memory>

// nanosvg is header-only; this is the single translation unit that pulls in its
// implementation. NANOSVG_ALL_COLOR_KEYWORDS lets named colors ("white", ...) parse.
#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include <nanosvg.h>
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvgrast.h>

namespace mosaic::common {
namespace {

// RAII wrappers so the early-return error paths cannot leak nanosvg allocations.
struct SvgImageDeleter {
    void operator()(NSVGimage* p) const noexcept {
        if (p)
            nsvgDelete(p);
    }
};
struct RasterizerDeleter {
    void operator()(NSVGrasterizer* p) const noexcept {
        if (p)
            nsvgDeleteRasterizer(p);
    }
};

} // namespace

Image rasterizeSvg(const unsigned char* data, std::size_t len, int width, int height,
                   std::string* error) {
    auto fail = [&](const char* msg) {
        if (error)
            *error = msg;
        return Image{};
    };

    if (!data || len == 0)
        return fail("rasterizeSvg: empty input");
    if (width <= 0 || height <= 0)
        return fail("rasterizeSvg: non-positive target size");

    // nsvgParse mutates its input buffer, so parse a NUL-terminated mutable copy.
    std::string mutableSvg(reinterpret_cast<const char*>(data), len);
    std::unique_ptr<NSVGimage, SvgImageDeleter> svg(nsvgParse(mutableSvg.data(), "px", 96.0f));
    if (!svg || svg->width <= 0.0f || svg->height <= 0.0f) {
        return fail("rasterizeSvg: failed to parse SVG");
    }

    std::unique_ptr<NSVGrasterizer, RasterizerDeleter> rast(nsvgCreateRasterizer());
    if (!rast)
        return fail("rasterizeSvg: failed to create rasterizer");

    // Uniform fit-and-center into the target box.
    const float scale =
        std::min(static_cast<float>(width) / svg->width, static_cast<float>(height) / svg->height);
    const float tx = (static_cast<float>(width) - svg->width * scale) * 0.5f;
    const float ty = (static_cast<float>(height) - svg->height * scale) * 0.5f;

    Image out(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    nsvgRasterize(rast.get(), svg.get(), tx, ty, scale, out.rgba.data(), width, height, width * 4);
    return out;
}

std::optional<SvgSize> svgIntrinsicSize(const unsigned char* data, std::size_t len) {
    if (!data || len == 0)
        return std::nullopt;
    std::string mutableSvg(reinterpret_cast<const char*>(data), len);
    std::unique_ptr<NSVGimage, SvgImageDeleter> svg(nsvgParse(mutableSvg.data(), "px", 96.0f));
    if (!svg || svg->width <= 0.0f || svg->height <= 0.0f)
        return std::nullopt;
    return SvgSize{svg->width, svg->height};
}

} // namespace mosaic::common
