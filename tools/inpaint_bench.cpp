// inpaint_bench — a standalone perf/correctness driver for the inpainting engine.
//
// NOT built by default and NOT part of the test suite: it reads an image + mask from disk paths
// given on the command line, so no test images are vendored into the repo (the 4912x7360 pexels
// photos the user benchmarks with would bloat it). Enable with -DMOSAIC_BUILD_INPAINT_BENCH=ON.
//
// Usage:
//   inpaint_bench <image> <mask> [downscaleFactor=1] [backend=offset-stats] [outPpm]
//
//   <mask> is any image the same size as <image>; pixels with red > 127 are the hole to fill.
//   downscaleFactor box-downsamples both by an integer factor first (so a 4912x7360 photo can be
//   exercised at a tractable size). Per-stage timings come from InpaintResult::timings.
//
// Wrap the invocation in `timeout` for a hard wall-clock cap — a pathological run can take hours,
// which is exactly the bug this tool measures.

#include "common/image.hpp"
#include "core/inpaint/inpaint_engine.hpp"
#include "core/selection.hpp"
#include "io/io.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using mosaic::common::Image;
using mosaic::common::ImageF;

// Box-average downsample an 8-bit RGBA image by an integer factor (>=1).
[[nodiscard]] Image boxDownscale(const Image& src, int f) {
    if (f <= 1 || src.empty()) {
        return src;
    }
    const std::uint32_t w = (src.width + f - 1) / f;
    const std::uint32_t h = (src.height + f - 1) / f;
    Image out(w, h);
    for (std::uint32_t oy = 0; oy < h; ++oy) {
        for (std::uint32_t ox = 0; ox < w; ++ox) {
            std::uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int dy = 0; dy < f; ++dy) {
                for (int dx = 0; dx < f; ++dx) {
                    const std::uint32_t sx = ox * f + dx;
                    const std::uint32_t sy = oy * f + dy;
                    if (sx < src.width && sy < src.height) {
                        const std::size_t p = (static_cast<std::size_t>(sy) * src.width + sx) * 4;
                        r += src.rgba[p];
                        g += src.rgba[p + 1];
                        b += src.rgba[p + 2];
                        a += src.rgba[p + 3];
                        ++n;
                    }
                }
            }
            const std::size_t o = (static_cast<std::size_t>(oy) * w + ox) * 4;
            out.rgba[o] = static_cast<std::uint8_t>(r / n);
            out.rgba[o + 1] = static_cast<std::uint8_t>(g / n);
            out.rgba[o + 2] = static_cast<std::uint8_t>(b / n);
            out.rgba[o + 3] = static_cast<std::uint8_t>(a / n);
        }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // survive a `timeout` SIGTERM mid-run
    if (argc < 3) {
        std::fprintf(
            stderr,
            "usage: %s <image> <mask> [downscaleFactor=1] [backend=offset-stats] [outPpm]\n",
            argv[0]);
        return 2;
    }
    const std::string imagePath = argv[1];
    const std::string maskPath = argv[2];
    const int factor = argc > 3 ? std::atoi(argv[3]) : 1;
    const std::string backend = argc > 4 ? argv[4] : "offset-stats";
    const std::string outPpm = argc > 5 ? argv[5] : "";

    std::string err;
    std::optional<Image> img = mosaic::io::loadImage(imagePath, &err);
    if (!img) {
        std::fprintf(stderr, "load image failed: %s\n", err.c_str());
        return 1;
    }
    // Mask: either an image file (red>127 == hole) or "circle:R" for a centered disc of radius R
    // (post-downscale pixels), to reproduce a "large selection" without a vendored mask.
    const bool circleMask = maskPath.rfind("circle:", 0) == 0;
    Image m8;
    if (!circleMask) {
        std::optional<Image> mask = mosaic::io::loadImage(maskPath, &err);
        if (!mask) {
            std::fprintf(stderr, "load mask failed: %s\n", err.c_str());
            return 1;
        }
        if (img->width != mask->width || img->height != mask->height) {
            std::fprintf(stderr, "image %ux%u and mask %ux%u differ in size\n", img->width,
                         img->height, mask->width, mask->height);
            return 1;
        }
        m8 = boxDownscale(*mask, factor);
    }

    Image i8 = boxDownscale(*img, factor);
    std::printf("image: %ux%u  (downscale x%d)\n", i8.width, i8.height, factor);

    mosaic::core::Selection hole(i8.width, i8.height);
    std::vector<std::uint8_t>& hd = hole.data();
    std::size_t holePixels = 0;
    if (circleMask) {
        const int radius = std::atoi(maskPath.c_str() + 7);
        const long cx = i8.width / 2, cy = i8.height / 2;
        for (std::uint32_t y = 0; y < i8.height; ++y) {
            for (std::uint32_t x = 0; x < i8.width; ++x) {
                const long dx = static_cast<long>(x) - cx, dy = static_cast<long>(y) - cy;
                if (dx * dx + dy * dy <= static_cast<long>(radius) * radius) {
                    hd[static_cast<std::size_t>(y) * i8.width + x] = 255;
                    ++holePixels;
                }
            }
        }
    } else {
        for (std::uint32_t y = 0; y < m8.height; ++y) {
            for (std::uint32_t x = 0; x < m8.width; ++x) {
                const std::size_t p = (static_cast<std::size_t>(y) * m8.width + x) * 4;
                if (m8.rgba[p] > 127) {
                    hd[static_cast<std::size_t>(y) * m8.width + x] = 255;
                    ++holePixels;
                }
            }
        }
    }
    std::printf("hole pixels: %zu (%.1f%% of image)\n", holePixels,
                100.0 * static_cast<double>(holePixels) /
                    static_cast<double>(static_cast<std::size_t>(i8.width) * i8.height));

    const ImageF imgF = mosaic::common::toFloat(i8);
    mosaic::core::inpaint::InpaintEngine engine = mosaic::core::inpaint::makeDefaultEngine();
    if (!engine.setActiveBackend(backend)) {
        std::fprintf(stderr, "unknown backend '%s'\n", backend.c_str());
        return 1;
    }

    // Optional param overrides (for tuning experiments); unset = engine defaults.
    mosaic::core::inpaint::Params params{};
    if (const char* v = std::getenv("MOSAIC_BENCH_POISSON_ITERS")) {
        params.poissonIterations = std::atoi(v);
    }
    if (const char* v = std::getenv("MOSAIC_BENCH_POISSON_OMEGA")) {
        params.poissonOmega = std::atof(v);
    }
    if (const char* v = std::getenv("MOSAIC_BENCH_NNF_PATCHES")) {
        params.nnfMaxPatches = std::atoi(v);
    }
    if (const char* v = std::getenv("MOSAIC_BENCH_GC_NODES")) {
        params.graphCutMaxNodes = std::atoi(v);
    }
    // The working-region cap decides whether the offset search runs on a DOWNSAMPLED proxy of the
    // hole's neighbourhood or on full-resolution pixels: set it past the image's long side and
    // extractWorkingRegion picks scale 1. Needed to measure what the downsample actually buys.
    if (const char* v = std::getenv("MOSAIC_BENCH_MAX_REGION")) {
        params.maxRegionW = std::atoi(v);
        params.maxRegionH = std::atoi(v);
    }
    if (const char* v = std::getenv("MOSAIC_BENCH_K")) {
        params.K = std::atoi(v);
    }
    // Search the whole image for offsets instead of a selection-sized window.
    if (const char* v = std::getenv("MOSAIC_BENCH_GLOBAL_REGION")) {
        params.globalSearchRegion = std::atoi(v) != 0;
    }
    if (const char* v = std::getenv("MOSAIC_BENCH_SEAM_REFINE")) {
        params.seamRefine = std::atoi(v) != 0;
    }
    if (const char* v = std::getenv("MOSAIC_BENCH_GC_CYCLES")) {
        params.graphCutMaxCycles = std::atoi(v);
    }
    const mosaic::core::inpaint::InpaintRequest req{imgF, hole, params};
    // Optional progress probe (MOSAIC_BENCH_PROGRESS=1): record every (fraction, stage) tick so the
    // smoothness of the bar can be checked from the CLI — biggest jump + stage transitions.
    std::vector<std::pair<float, std::string>> ticks;
    mosaic::core::inpaint::ProgressFn pfn;
    if (std::getenv("MOSAIC_BENCH_PROGRESS") != nullptr) {
        pfn = [&ticks](const mosaic::core::inpaint::InpaintProgress& p) -> bool {
            ticks.emplace_back(p.fraction, std::string(p.stage));
            return true;
        };
    }
    const auto t0 = std::chrono::steady_clock::now();
    const mosaic::core::inpaint::InpaintResult res = engine.run(req, pfn);
    const auto t1 = std::chrono::steady_clock::now();
    if (!ticks.empty()) {
        float maxGap = 0.0f;
        float prev = 0.0f;
        std::string lastStage;
        std::printf("progress: %zu ticks\n", ticks.size());
        for (const auto& [f, s] : ticks) {
            maxGap = std::max(maxGap, f - prev);
            prev = f;
            if (s != lastStage) {
                std::printf("  -> %.3f  %s\n", f, s.c_str());
                lastStage = s;
            }
        }
        std::printf("  biggest single jump in fraction: %.3f\n", maxGap);
    }
    const double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::printf("backend: %s  ok=%d  detail=%s\n", backend.c_str(), res.ok ? 1 : 0,
                res.detail.c_str());
    double sum = 0.0;
    for (const auto& t : res.timings) {
        std::printf("  stage %-20s %10.1f ms\n", t.name.c_str(), t.ms);
        sum += t.ms;
    }
    std::printf("  %-26s %10.1f ms\n", "(sum of stages)", sum);
    std::printf("TOTAL engine.run():          %10.1f ms\n", totalMs);

    // Out-of-range / saturation diagnostic over the hole: extreme white/black/magenta blobs show up
    // as channels pushed outside [0,1] (then clamped on display) or near-pure 0/1 saturation.
    if (res.ok) {
        float lo = 1e9f, hi = -1e9f;
        std::size_t oob = 0, white = 0, black = 0;
        for (std::uint32_t y = 0; y < res.image.height; ++y) {
            for (std::uint32_t x = 0; x < res.image.width; ++x) {
                if (hd[static_cast<std::size_t>(y) * res.image.width + x] == 0) {
                    continue;
                }
                const mosaic::common::ColorF c = res.image.at(x, y);
                for (float ch : {c.r, c.g, c.b}) {
                    lo = std::min(lo, ch);
                    hi = std::max(hi, ch);
                }
                if (c.r < -0.01f || c.g < -0.01f || c.b < -0.01f || c.r > 1.01f || c.g > 1.01f ||
                    c.b > 1.01f) {
                    ++oob;
                }
                if (c.r > 0.97f && c.g > 0.97f && c.b > 0.97f) {
                    ++white;
                }
                if (c.r < 0.03f && c.g < 0.03f && c.b < 0.03f) {
                    ++black;
                }
            }
        }
        std::printf("hole float range: [%.3f, %.3f]  out-of-[0,1]=%zu  near-white=%zu  "
                    "near-black=%zu  (of %zu hole px)\n",
                    lo, hi, oob, white, black, holePixels);
    }

    if (!outPpm.empty() && res.ok) {
        const Image out8 = mosaic::common::toImage8(res.image);
        std::string werr;
        if (mosaic::common::writePpm(out8, outPpm, &werr)) {
            std::printf("wrote %s\n", outPpm.c_str());
        } else {
            std::fprintf(stderr, "write %s failed: %s\n", outPpm.c_str(), werr.c_str());
        }
    }
    return res.ok ? 0 : 1;
}
