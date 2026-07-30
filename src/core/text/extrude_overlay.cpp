#include "core/text/extrude_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <variant>

#include "core/blend_math.hpp"
#include "core/vector/paint.hpp"

namespace mosaic::core::text {
namespace {

using common::ColorF;
using common::ImageF;

// Texel budget: density matches the device bake up to these caps, then scales down uniformly.
// A map is transient render support (rebuilt with the text cache), so the cap bounds peak memory
// (2M texels * 16B = 32MB float RGBA worst case) and the Vulkan lane's upload with it.
constexpr double kMaxMapDim = 4096.0;
constexpr double kMaxMapTexels = 2.0 * 1024.0 * 1024.0;

// One overlay's colour at map texel (x,y): the paintAtNorm split -- gradients run across the
// domain normalized to [0,1]^2, patterns tile in real design px from the domain's top-left.
ColorF overlayColorAt(const vec::Paint& paint, std::uint32_t x, std::uint32_t y,
                      const ImageF& map, const common::Rect& domain, bool antialias) {
    const double u = (static_cast<double>(x) + 0.5) / static_cast<double>(map.width);
    const double v = (static_cast<double>(y) + 0.5) / static_cast<double>(map.height);
    if (std::holds_alternative<vec::Pattern>(paint))
        return vec::sampleAt(paint, {u * domain.w, v * domain.h}, antialias);
    return vec::sampleAt(paint, {u, v});
}

// The wall map's texel colour (see the header note): patterns tile in real UNROLLED design px
// (undistorted around the band); gradients/solids take the design-space continuation at the
// outline point `design` (the station lookup for this texel's s).
ColorF wallColorAt(const vec::Paint& paint, double sPx, double tPx, common::Vec2 design,
                   const common::Rect& domain, bool antialias) {
    if (std::holds_alternative<vec::Pattern>(paint))
        return vec::sampleAt(paint, {sPx, tPx}, antialias);
    return vec::sampleAt(paint, {(design.x - domain.x) / domain.w,
                                 (design.y - domain.y) / domain.h});
}

// Piecewise-linear station lookup: the outline design point at normalized unrolled position `s`.
common::Vec2 stationDesignAt(const std::vector<SideStation>& st, float s) {
    if (st.empty()) return {};
    if (s <= st.front().s) return st.front().design;
    if (s >= st.back().s) return st.back().design;
    // Binary search for the segment; stations are sorted (duplicated s at contour seams is fine,
    // the interval between duplicates has zero width and never wins).
    std::size_t lo = 0, hi = st.size() - 1;
    while (lo + 1 < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (st[mid].s <= s) lo = mid;
        else hi = mid;
    }
    const float span = st[hi].s - st[lo].s;
    const double t = span > 1e-12f ? (s - st[lo].s) / span : 0.0;
    return {st[lo].design.x + (st[hi].design.x - st[lo].design.x) * t,
            st[lo].design.y + (st[hi].design.y - st[lo].design.y) * t};
}

}  // namespace

bool extrudeOverlaysActive(const LayerEffects& fx) {
    const auto draws = [](const OverlayEffect& ov) {
        return ov.enabled && !std::holds_alternative<vec::NoPaint>(ov.paint);
    };
    return draws(fx.colorOverlay) || draws(fx.gradientOverlay) || draws(fx.patternOverlay);
}

ExtrudeOverlay buildExtrudeOverlay(const LayerEffects& fx, const ExtrudeMesh& mesh,
                                   const Extrude& params, const common::Rect& uvDomain,
                                   double pixelScale, bool antialias) {
    ExtrudeOverlay out;
    out.wrapSides = params.overlayWrapSides;
    if (mesh.empty() || uvDomain.empty() || !extrudeOverlaysActive(fx)) return out;

    // Map extent: the UV domain at device texel density, uniformly shrunk to the budget.
    double scale = std::max(pixelScale, 1e-3);
    scale = std::min({scale, kMaxMapDim / uvDomain.w, kMaxMapDim / uvDomain.h,
                      std::sqrt(kMaxMapTexels / (uvDomain.w * uvDomain.h))});
    const auto dim = [&](double units) {
        return static_cast<std::uint32_t>(std::clamp(std::ceil(units * scale), 1.0, kMaxMapDim));
    };
    const std::uint32_t w = dim(uvDomain.w);
    const std::uint32_t h = dim(uvDomain.h);

    const OverlayEffect* overlays[3] = {&fx.colorOverlay, &fx.gradientOverlay, &fx.patternOverlay};

    // Wrap mode's wall-map extent: the unrolled band (outline length x depth), same texel
    // density + budget discipline as the design map.
    const bool walls = out.wrapSides && mesh.sideLength > 0.0 && !mesh.sideStations.empty();
    const double tDepth = std::max(0.01, static_cast<double>(params.depth));
    std::uint32_t ww = 0, wh = 0;
    double wallScale = scale;
    if (walls) {
        wallScale = std::min({wallScale, kMaxMapDim / mesh.sideLength, kMaxMapDim / tDepth,
                              std::sqrt(kMaxMapTexels / (mesh.sideLength * tDepth))});
        const auto wdim = [&](double units) {
            return static_cast<std::uint32_t>(
                std::clamp(std::ceil(units * wallScale), 1.0, kMaxMapDim));
        };
        ww = wdim(mesh.sideLength);
        wh = wdim(tDepth);
    }

    // One map per DISTINCT resolved albedo across the mesh's ranges (metal/roughness don't feed
    // the overlay blend, so albedo is the whole key); every run maps to its slot.
    std::vector<ColorF> albedos;
    for (const ExtrudeMeshRange& range : mesh.ranges) {
        if (out.runToMap.contains(range.runIndex)) continue;
        const ColorF base = materialForRun(params, range.runIndex).albedo;
        std::size_t slot = albedos.size();
        for (std::size_t i = 0; i < albedos.size(); ++i)
            if (albedos[i] == base) {
                slot = i;
                break;
            }
        if (slot == albedos.size()) {
            albedos.push_back(base);
            ImageF map(w, h);
            for (std::uint32_t y = 0; y < h; ++y) {
                for (std::uint32_t x = 0; x < w; ++x) {
                    // The stack composites over the OPAQUE albedo (the solid's surface): dst
                    // alpha stays 1, so the result RGB is exactly the colour the face shades
                    // with. Coverage is the solid's own -- overlays never change it.
                    ColorF dst{base.r, base.g, base.b, 1.0f};
                    for (const OverlayEffect* ov : overlays) {
                        if (!ov->enabled || std::holds_alternative<vec::NoPaint>(ov->paint))
                            continue;
                        const ColorF src =
                            overlayColorAt(ov->paint, x, y, map, uvDomain, antialias);
                        dst = compositeOver(ov->blend, dst, src, ov->opacity);
                    }
                    map.set(x, y, dst);
                }
            }
            out.maps.push_back(std::move(map));
            if (walls) {
                ImageF wall(ww, wh);
                for (std::uint32_t y = 0; y < wh; ++y) {
                    const double tN = (static_cast<double>(y) + 0.5) / wh;
                    for (std::uint32_t x = 0; x < ww; ++x) {
                        const double sN = (static_cast<double>(x) + 0.5) / ww;
                        const common::Vec2 design =
                            stationDesignAt(mesh.sideStations, static_cast<float>(sN));
                        ColorF dst{base.r, base.g, base.b, 1.0f};
                        for (const OverlayEffect* ov : overlays) {
                            if (!ov->enabled || std::holds_alternative<vec::NoPaint>(ov->paint))
                                continue;
                            const ColorF src =
                                wallColorAt(ov->paint, sN * mesh.sideLength, tN * tDepth, design,
                                            uvDomain, antialias);
                            dst = compositeOver(ov->blend, dst, src, ov->opacity);
                        }
                        wall.set(x, y, dst);
                    }
                }
                out.wallMaps.push_back(std::move(wall));
            }
        }
        out.runToMap.emplace(range.runIndex, slot);
    }
    return out;
}

}  // namespace mosaic::core::text
