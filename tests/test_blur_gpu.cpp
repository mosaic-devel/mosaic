// Blur Vulkan-lane parity tests (S33; docs/blur-filters.md §8): the compute lane against the
// CPU reference kernels on the same BlurOps. Gated on a usable Vulkan device (the
// test_extrude_gpu.cpp CI-safe pattern -- a machine without one WARNs and passes).
// TOLERANCE-BASED, deliberately: both lanes run float and share every formula (the tables are
// even uploaded bit-identical), so the drift left is fused-multiply-add contraction,
// transcendental implementations and division rounding -- "the same picture", never the same
// bits, and never a substitute for the CPU-pinned byte goldens. The BlurGpu is constructed
// DIRECTLY here: the test binary must NEVER install the global override
// (setBlurRenderOverride), because every byte-pinned golden depends on the CPU lane serving
// the compositor.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "common/image.hpp"
#include "core/adjustments.hpp"
#include "render/blur.hpp"
#include "render/blur_gpu.hpp"

using mosaic::common::ImageF;
using mosaic::core::AdjustmentKind;
using mosaic::core::DofBokeh;
using mosaic::render::BlurOp;
namespace fx = mosaic::render::fx;

namespace {

// The structured fixture every case blurs: two smooth gradients, a hard-edged bright disc (the
// kernel shapes have an edge to smear), an alpha ramp (partial coverage diffuses), and a fully
// transparent block carrying JUNK RGB -- the straight-vs-premultiplied tripwire: any lane that
// convolves straight RGB drags the junk into visible neighbours and fails parity loudly.
ImageF structuredImage(std::uint32_t w = 96, std::uint32_t h = 72) {
    ImageF img(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const float fx01 = static_cast<float>(x) / static_cast<float>(w - 1);
            const float fy01 = static_cast<float>(y) / static_cast<float>(h - 1);
            mosaic::common::ColorF c{fx01, 0.25f + 0.5f * fy01, 0.9f - 0.6f * fx01 * fy01,
                                     1.0f};
            const float dx = static_cast<float>(x) - 30.0f;
            const float dy = static_cast<float>(y) - 26.0f;
            if (dx * dx + dy * dy < 12.0f * 12.0f) c = {1.0f, 0.95f, 0.2f, 1.0f};
            if (x >= 64)
                c.a = std::clamp(1.0f - static_cast<float>(x - 64) / 20.0f, 0.0f, 1.0f);
            if (x >= 10 && x < 20 && y >= 50 && y < 62) c = {7.5f, -2.0f, 42.0f, 0.0f};
            img.set(x, y, c);
        }
    return img;
}

// The CPU reference dispatch for a BlurOp, mirroring compositor.cpp's runBlurCpu for the four
// GPU-served kinds (the dispatch itself is file-local there; the kernels are the public
// render/blur.hpp API the seam contract is written against).
void runCpu(ImageF& img, const BlurOp& op) {
    switch (op.kind) {
        case AdjustmentKind::GaussianBlur: fx::gaussianBlurImage(img, op.size); break;
        case AdjustmentKind::SurfaceBlur: fx::surfaceBlurImage(img, op.size, op.threshold); break;
        case AdjustmentKind::LensBlur: {
            const fx::ApertureKernel k = fx::makeApertureKernel(op.size, op.blades, op.curvature,
                                                                op.rotationRad, op.draft);
            fx::lensBlurImage(img, k, op.boost, op.boostThreshold);
            break;
        }
        case AdjustmentKind::DofBlur: {
            const float nx = -op.dirY;
            const float ny = op.dirX;
            std::vector<float> field(img.pixelCount());
            for (std::uint32_t y = 0; y < img.height; ++y) {
                const float py = static_cast<float>(y) + 0.5f - op.cy;
                for (std::uint32_t x = 0; x < img.width; ++x) {
                    const float px = static_cast<float>(x) + 0.5f - op.cx;
                    const float d = std::abs(px * nx + py * ny);
                    field[static_cast<std::size_t>(y) * img.width + x] =
                        std::clamp((d - op.band) / op.feather, 0.0f, 1.0f) * op.size;
                }
            }
            fx::dofBlurImage(img, field, op.size,
                             op.mode == static_cast<int>(DofBokeh::Iris), op.draft);
            break;
        }
        default: break;
    }
}

// Per-channel absolute difference (the test_texture_gpu.cpp shape): meanAbs bounds the global
// drift, outliers counts pixels where ANY channel differs by more than eps (rounding-driven
// branch flips concentrate error in single pixels).
struct Diff {
    double meanAbs = 0.0;
    double maxAbs = 0.0;
    std::size_t outliers = 0;
    std::size_t pixels = 0;
};

Diff compare(const ImageF& cpu, const ImageF& gpu, double eps) {
    Diff d;
    REQUIRE(cpu.width == gpu.width);
    REQUIRE(cpu.height == gpu.height);
    d.pixels = cpu.pixelCount();
    double sum = 0.0;
    for (std::size_t i = 0; i < cpu.rgba.size(); i += 4) {
        double pixelMax = 0.0;
        for (std::size_t c = 0; c < 4; ++c) {
            const double diff = std::abs(static_cast<double>(cpu.rgba[i + c]) - gpu.rgba[i + c]);
            sum += diff;
            pixelMax = std::max(pixelMax, diff);
        }
        d.maxAbs = std::max(d.maxAbs, pixelMax);
        if (pixelMax > eps) ++d.outliers;
    }
    d.meanAbs = sum / static_cast<double>(cpu.rgba.size());
    return d;
}

std::unique_ptr<mosaic::render::BlurGpu> makeLane(const char* who) {
    std::string err;
    auto gpu = mosaic::render::BlurGpu::create(/*enableValidation=*/true, err);
    if (!gpu) {
        const std::string why =
            std::string("no Vulkan device -- skipping ") + who + " (" + err + ")";
        WARN_MESSAGE(true, why);
    }
    return gpu;
}

// The parity budgets: both lanes run float over shared formulas and bit-identical tables, so
// smooth regions agree to ~1e-6; eps of ~3 8-bit display steps only trips on genuine rounding
// branch flips (a boost-threshold luma landing on the other side of an FMA, a DoF field value
// straddling a level boundary), and the mean cap plus the sliver of allowed outliers leave
// headroom for other drivers' transcendental differences without ever letting a real
// regression (a wrong formula, a lost dispatch) pass as "tolerance".
constexpr double kEps = 0.012;
constexpr double kMeanMax = 0.0015;
constexpr double kOutlierFrac = 0.005;

void checkParity(mosaic::render::BlurGpu& gpu, const BlurOp& op, const char* label) {
    const ImageF source = structuredImage();
    ImageF cpu = source;
    runCpu(cpu, op);
    ImageF via = source;
    REQUIRE(gpu.render(via, op));
    const Diff d = compare(cpu, via, kEps);
    INFO(std::string(label) << ": meanAbs " << d.meanAbs << ", maxAbs " << d.maxAbs
                            << ", outliers " << d.outliers << "/" << d.pixels);
    CHECK(d.meanAbs < kMeanMax);
    CHECK(static_cast<double>(d.outliers) <=
          kOutlierFrac * static_cast<double>(d.pixels) + 4.0);
}

}  // namespace

TEST_CASE("GPU gaussian blur draws the CPU lane's picture") {
    auto gpu = makeLane("gaussian parity");
    if (!gpu) return;
    BlurOp op;
    op.kind = AdjustmentKind::GaussianBlur;
    op.size = 0.8f;  // identity-adjacent: the small-sigma kernel is only a few taps wide
    checkParity(*gpu, op, "sigma 0.8");
    op.size = 3.0f;
    checkParity(*gpu, op, "sigma 3");
    op.size = 12.0f;  // large radius: the clamp edge policy carries real weight
    checkParity(*gpu, op, "sigma 12");
}

TEST_CASE("GPU surface blur draws the CPU lane's picture") {
    auto gpu = makeLane("surface parity");
    if (!gpu) return;
    BlurOp op;
    op.kind = AdjustmentKind::SurfaceBlur;
    op.size = 2.0f;  // identity-adjacent small radius
    op.threshold = 0.06f;
    checkParity(*gpu, op, "radius 2, threshold 0.06");
    op.size = 6.0f;  // small threshold: the range term dominates (edge-preserving regime)
    op.threshold = 0.08f;
    checkParity(*gpu, op, "radius 6, threshold 0.08");
    op.size = 10.0f;  // large threshold: approaches the plain Gaussian
    op.threshold = 0.45f;
    checkParity(*gpu, op, "radius 10, threshold 0.45");
}

TEST_CASE("GPU lens blur draws the CPU lane's picture") {
    auto gpu = makeLane("lens parity");
    if (!gpu) return;
    BlurOp op;
    op.kind = AdjustmentKind::LensBlur;
    op.size = 5.0f;
    op.blades = 6;
    op.curvature = 0.35f;
    op.rotationRad = 0.4f;
    checkParity(*gpu, op, "hex, curved, no boost");
    op.size = 9.0f;  // past the coarsening threshold: the subsampled tap lattice must match too
    op.blades = 3;
    op.curvature = 0.0f;
    op.rotationRad = 0.0f;
    op.boost = 0.7f;  // the single-lower-threshold specular boost path
    op.boostThreshold = 0.5f;
    checkParity(*gpu, op, "triangle + boost");
    op.blades = 5;
    op.curvature = 0.8f;
    op.rotationRad = 0.2f;
    op.boost = 0.3f;
    op.boostThreshold = 0.35f;
    op.draft = true;  // the live-gesture kernel (halved coarsening threshold)
    checkParity(*gpu, op, "pentagon, draft");
}

TEST_CASE("GPU DoF blur draws the CPU lane's picture") {
    auto gpu = makeLane("DoF parity");
    if (!gpu) return;
    BlurOp op;
    op.kind = AdjustmentKind::DofBlur;
    op.size = 8.0f;
    op.cx = 48.0f;
    op.cy = 36.0f;
    op.dirX = std::cos(0.5236f);  // ~30 degrees: the band cuts the frame at an angle
    op.dirY = std::sin(0.5236f);
    op.band = 9.0f;
    op.feather = 6.0f;
    op.mode = static_cast<int>(DofBokeh::Soft);
    checkParity(*gpu, op, "soft bokeh");

    // The focus band stays byte-identical to the source THROUGH the GPU lane (the §3 pin, GPU
    // edition): well inside the band the field is exactly 0 and the interpolation shader
    // passes the source pixel's bits untouched. A 0.75 px margin keeps FMA-divergent distance
    // values at the band boundary out of the byte-exact claim.
    {
        const ImageF source = structuredImage();
        ImageF via = source;
        REQUIRE(gpu->render(via, op));
        const float nx = -op.dirY;
        const float ny = op.dirX;
        std::size_t inBand = 0, mismatched = 0;
        for (std::uint32_t y = 0; y < source.height; ++y)
            for (std::uint32_t x = 0; x < source.width; ++x) {
                const float px = static_cast<float>(x) + 0.5f - op.cx;
                const float py = static_cast<float>(y) + 0.5f - op.cy;
                if (std::abs(px * nx + py * ny) > op.band - 0.75f) continue;
                ++inBand;
                const std::size_t p = (static_cast<std::size_t>(y) * source.width + x) * 4;
                for (std::size_t c = 0; c < 4; ++c)
                    if (source.rgba[p + c] != via.rgba[p + c]) {
                        ++mismatched;
                        break;
                    }
            }
        REQUIRE(inBand > 100);  // the band actually crosses the fixture
        CHECK(mismatched == 0);
    }

    op.mode = static_cast<int>(DofBokeh::Iris);
    op.size = 6.0f;
    op.band = 7.0f;
    op.feather = 5.0f;
    op.dirX = std::cos(-0.2618f);
    op.dirY = std::sin(-0.2618f);
    checkParity(*gpu, op, "iris bokeh");
    op.draft = true;
    checkParity(*gpu, op, "iris bokeh, draft");
}

TEST_CASE("GPU blur lane refuses what it does not carry (the CPU fallback contract)") {
    auto gpu = makeLane("fallback contract");
    if (!gpu) return;
    const ImageF source = structuredImage();
    // Box / Motion / Radial are the CPU lane's (blur_gpu.hpp); the lane must decline WITHOUT
    // touching the image.
    for (const AdjustmentKind kind :
         {AdjustmentKind::BoxBlur, AdjustmentKind::MotionBlur, AdjustmentKind::RadialBlur}) {
        BlurOp op;
        op.kind = kind;
        op.size = 3.0f;
        op.amount = 20.0f;
        op.cx = 48.0f;
        op.cy = 36.0f;
        ImageF via = source;
        CHECK_FALSE(gpu->render(via, op));
        CHECK(via.rgba == source.rgba);
    }
    // A color kind reaching the seam by mistake declines too.
    BlurOp op;
    op.kind = AdjustmentKind::Invert;
    op.size = 1.0f;
    ImageF via = source;
    CHECK_FALSE(gpu->render(via, op));
    CHECK(via.rgba == source.rgba);
}

TEST_CASE("GPU blur lane is deterministic per device") {
    auto gpu = makeLane("determinism");
    if (!gpu) return;
    BlurOp op;
    op.kind = AdjustmentKind::GaussianBlur;
    op.size = 3.0f;
    ImageF a = structuredImage();
    ImageF b = a;
    REQUIRE(gpu->render(a, op));
    REQUIRE(gpu->render(b, op));
    CHECK(a.rgba == b.rgba);
}
