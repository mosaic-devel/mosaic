#include "core/inpaint/backends/pde/pde_backend.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mosaic::core::inpaint {

namespace {

// Is document pixel (x,y) inside the hole? The mask is document-sized; pixels beyond its extent
// (or in an empty mask) are "known", never hole.
[[nodiscard]] bool isHole(const Selection& mask, std::uint32_t x, std::uint32_t y) {
    if (mask.isEmpty() || x >= mask.width() || y >= mask.height()) {
        return false;
    }
    return mask.at(x, y) > 0;
}

} // namespace

InpaintResult PdeBackend::run(const InpaintRequest& request, const ProgressFn& progress) {
    const common::ImageF& src = request.image;
    const Selection& mask = request.holeMask;
    const Params& p = request.params;

    InpaintResult res;
    res.image = src; // start from the input; only hole pixels change
    if (src.empty()) {
        res.ok = false;
        res.detail = "empty image";
        return res;
    }

    const std::uint32_t w = src.width;
    const std::uint32_t h = src.height;

    // Flag the hole and accumulate the mean of the known pixels (the seed value).
    std::vector<std::uint8_t> hole(static_cast<std::size_t>(w) * h, 0);
    double meanR = 0, meanG = 0, meanB = 0, meanA = 0;
    std::size_t holeCount = 0, knownCount = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            if (isHole(mask, x, y)) {
                hole[static_cast<std::size_t>(y) * w + x] = 1;
                ++holeCount;
            } else {
                const common::ColorF c = src.at(x, y);
                meanR += c.r;
                meanG += c.g;
                meanB += c.b;
                meanA += c.a;
                ++knownCount;
            }
        }
    }

    if (holeCount == 0) {
        res.detail = "no hole (mask has no coverage)";
        return res; // ok: nothing to do, image unchanged
    }
    if (knownCount == 0) {
        res.ok = false;
        res.detail = "hole covers the entire image — no boundary data to diffuse from";
        return res;
    }

    common::ImageF& im = res.image;

    // Seed every hole pixel with the known-region mean so relaxation starts from a sane value.
    const common::ColorF seed{static_cast<float>(meanR / static_cast<double>(knownCount)),
                              static_cast<float>(meanG / static_cast<double>(knownCount)),
                              static_cast<float>(meanB / static_cast<double>(knownCount)),
                              static_cast<float>(meanA / static_cast<double>(knownCount))};
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            if (hole[static_cast<std::size_t>(y) * w + x]) {
                im.set(x, y, seed);
            }
        }
    }

    // Gauss-Seidel relaxation of Laplace's equation: each hole pixel relaxes toward the average of
    // its 4-neighbors. Known pixels are fixed Dirichlet boundary; in-place updates (Gauss-Seidel)
    // converge faster than Jacobi. Iterate to convergence or the sweep budget.
    const int maxIters = std::max(1, p.pdeIterations);
    const double eps = p.pdeEpsilon;
    for (int iter = 0; iter < maxIters; ++iter) {
        double maxDelta = 0.0;
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                if (!hole[static_cast<std::size_t>(y) * w + x]) {
                    continue;
                }
                float sr = 0, sg = 0, sb = 0, sa = 0;
                int n = 0;
                if (x > 0) {
                    const auto c = im.at(x - 1, y);
                    sr += c.r;
                    sg += c.g;
                    sb += c.b;
                    sa += c.a;
                    ++n;
                }
                if (x + 1 < w) {
                    const auto c = im.at(x + 1, y);
                    sr += c.r;
                    sg += c.g;
                    sb += c.b;
                    sa += c.a;
                    ++n;
                }
                if (y > 0) {
                    const auto c = im.at(x, y - 1);
                    sr += c.r;
                    sg += c.g;
                    sb += c.b;
                    sa += c.a;
                    ++n;
                }
                if (y + 1 < h) {
                    const auto c = im.at(x, y + 1);
                    sr += c.r;
                    sg += c.g;
                    sb += c.b;
                    sa += c.a;
                    ++n;
                }
                if (n == 0) {
                    continue; // unreachable for w,h >= 2, but keep the divide safe
                }
                const float inv = 1.0f / static_cast<float>(n);
                const common::ColorF old = im.at(x, y);
                const common::ColorF nv{sr * inv, sg * inv, sb * inv, sa * inv};
                im.set(x, y, nv);
                maxDelta = std::max({maxDelta, static_cast<double>(std::fabs(nv.r - old.r)),
                                     static_cast<double>(std::fabs(nv.g - old.g)),
                                     static_cast<double>(std::fabs(nv.b - old.b)),
                                     static_cast<double>(std::fabs(nv.a - old.a))});
            }
        }
        if (progress && (iter % 16 == 0)) {
            const InpaintProgress tick{static_cast<float>(iter) / static_cast<float>(maxIters),
                                       "Blending", &im};
            if (!progress(tick)) {
                res.ok = false;
                res.detail = "cancelled";
                return res;
            }
        }
        if (maxDelta < eps) {
            break; // converged
        }
    }

    res.detail = "diffusion (harmonic) fill";
    return res;
}

BackendInfo PdeBackend::info() const {
    BackendInfo bi;
    bi.displayName = "Diffusion (PDE)";
    bi.method = "Harmonic (Laplace) diffusion";
    bi.authors = "Classical PDE inpainting (Bertalmío / Telea family)";
    bi.paper = "";
    bi.summary =
        "Not recommended for general inpainting. It smoothly diffuses the surrounding colours inward, "
        "which blurs across any texture or structure — so it cannot remove objects or rebuild detail. "
        "Useful only for thin scratches, dust, and very small holes, where there is nothing to "
        "reconstruct.";
    bi.cost = "Sub-second on small holes; CPU";
    return bi;
}

BackendSettingsSchema PdeBackend::settingsSchema() const {
    BackendSettingsSchema s;
    // No quality presets — two direct knobs over the Gauss-Seidel solve.
    s.controls = {
        {"pdeIterations", "Iterations",
         "Maximum relaxation sweeps. The fill usually converges well before the cap.",
         ParamControl::Kind::Int, 100, 2000, 50, {}, 800, /*advanced*/ false},
        {"pdeEpsilon", "Convergence threshold",
         "Stop early once the largest per-sweep change drops below this.", ParamControl::Kind::Real,
         0.00001, 0.001, 0.00001, {}, 0.0001, /*advanced*/ true},
    };
    return s;
}

void PdeBackend::applyParam(Params& params, const std::string& key, double value) const {
    if (key == "pdeIterations") {
        params.pdeIterations = static_cast<int>(std::lround(value));
    } else if (key == "pdeEpsilon") {
        params.pdeEpsilon = value;
    }
}

} // namespace mosaic::core::inpaint
