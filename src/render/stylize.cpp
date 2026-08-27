#include "render/stylize.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "common/thread_pool.hpp"
#include "core/adjustments.hpp"
#include "render/stylize_kernels.hpp"

// The S35 stylize family's compositor seam (docs/filters-stylize.md §5): resolve the schema into
// buffer-space numbers, run one kernel over a copy of the backdrop, blend it back under
// opacity * mask * clip coverage. The blend loop is applyBlurAdjustment's, verbatim in spirit --
// premultiplied lerp across differing alphas, an `amt >= 1` fast path that keeps the unmodulated
// case byte-equal to the raw kernel output, and a masked-out pixel byte-identical to the backdrop.
namespace mosaic::render {
namespace {

using common::ImageF;
using common::parallelFor;

constexpr double kPi = 3.14159265358979323846;

// ---- helpers mirrored from compositor.cpp ------------------------------------------------------
//
// The three below are byte-for-byte the compositor's own (isIdentity / adjustmentMaskAt /
// adjustmentMaskDomain / maxAxisScale). They live in that file's anonymous namespace, and this
// module is deliberately free-standing -- one branch in applyAdjustment is the entire coupling --
// so they are restated here rather than exported. If the mask placement rules ever change,
// docs/adjustment-layers.md §2 is the contract both copies implement.

[[nodiscard]] bool isIdentity(const common::Affine2D& t) noexcept {
    return t.m00 == 1.0 && t.m01 == 0.0 && t.m02 == 0.0 && t.m10 == 0.0 && t.m11 == 1.0 &&
           t.m12 == 0.0;
}

[[nodiscard]] double maxAxisScale(const common::Affine2D& t) {
    return std::max(std::hypot(t.m00, t.m10), std::hypot(t.m01, t.m11));
}

// A layer mask on an adjustment, sampled at PARENT-space point `p` against the parent-space rect
// `domain` the mask grid spans (nearest sample). Sampling in parent space is what keeps a masked
// adjustment correct under a REGION composite.
[[nodiscard]] float adjustmentMaskAt(const core::RasterMask& mk, common::Vec2 p,
                                     const common::Rect& domain) {
    if (mk.width == 0 || mk.height == 0 || domain.w <= 0.0 || domain.h <= 0.0) return 1.0f;
    const auto mx = std::clamp<long>(
        static_cast<long>(std::floor((p.x - domain.x) * mk.width / domain.w)), 0,
        static_cast<long>(mk.width) - 1);
    const auto my = std::clamp<long>(
        static_cast<long>(std::floor((p.y - domain.y) * mk.height / domain.h)), 0,
        static_cast<long>(mk.height) - 1);
    return static_cast<float>(mk.coverage[static_cast<std::size_t>(my) * mk.width +
                                          static_cast<std::size_t>(mx)]) /
           255.0f;
}

// The parent-space rect an adjustment layer's mask spans, honouring the layer's OWN transform: an
// UNLINKED sheet is already in parent space, so only a linked one takes the transform.
[[nodiscard]] common::Rect adjustmentMaskDomain(const core::AdjustmentLayer& adj,
                                                const core::RasterMask& mk,
                                                const common::Rect& maskDomain) {
    const common::Affine2D place = mk.linked
                                       ? adj.transform() * core::maskPlacement(adj, mk)
                                       : core::maskPlacement(adj, mk);
    if (isIdentity(place)) return maskDomain;
    return place.mapBounds(
        common::Rect{0.0, 0.0, static_cast<double>(mk.width), static_cast<double>(mk.height)});
}

// Schema-clamped parameter read (the S32 rule: every S35 kind is schema-honest, so a hostile
// .mosaic file cannot feed a kernel a negative radius or a 10^9 cell size).
[[nodiscard]] double schemaParam(const core::AdjustmentLayer& adj, const char* key) {
    const core::AdjustmentParamDesc* d = core::adjustmentParamDesc(adj.adjustmentKind(), key);
    return d != nullptr ? core::adjustmentParamValue(adj, *d) : 0.0;
}

// ---- parameter resolution ----------------------------------------------------------------------

// Every S35 kind's parameters resolved ONCE: schema-read, then mapped into the space its kernel
// works in. The px-in-BUFFER kinds (sharpen, unsharp, denoise, emboss, oil paint) scale their
// lengths by the placement's axis scale, so a 96px scope preview stylises proportionally instead
// of at full-canvas strength -- the §4 rule the blur family set. The px-in-PARENT kinds (pixelate,
// wave, vignette) do their geometry in parent space instead and only map the final sample position
// back, which is what makes their output region- and preview-stable to the byte.
//
// `effective == false` means the numbers add up to a no-op; the caller then returns before even
// copying the backdrop, so an identity-parameter layer composites byte-identically to no layer at
// all (docs/adjustment-layers.md §1). Every kernel repeats the same guard defensively, but this is
// the one that decides.
struct StylizeOp {
    core::AdjustmentKind kind{};
    bool effective = false;
    bool draft = false;
    common::Affine2D bufToParent;  // the walk placement's inverse

    float amount = 0.0f;     // sharpen / unsharp / emboss gain (1.0 == 100%)
    float sigma = 0.0f;      // unsharp Gaussian std-dev, buffer px
    float threshold = 0.0f;  // unsharp gate, [0,1] luma
    float noiseSigma = 0.0f;
    bool uniform = false;
    bool monochrome = false;
    std::uint32_t seed = 0;
    int radius = 0;      // denoise box half-width, buffer px
    int half = 0;        // oil-paint quadrant box half-width, buffer px
    double cell = 0.0;   // pixelate cell size, PARENT px
    float offX = 0.0f;   // emboss tap separation, buffer px
    float offY = 0.0f;
    fx::WaveOp wave;
    fx::VignetteOp vignette;
};

[[nodiscard]] StylizeOp resolveStylizeOp(const core::AdjustmentLayer& adj,
                                         const common::Affine2D& pre, bool liveDrag) {
    using enum core::AdjustmentKind;
    StylizeOp op;
    op.kind = adj.adjustmentKind();
    op.draft = liveDrag;
    const double scale = maxAxisScale(pre);
    const std::optional<common::Affine2D> inv = pre.inverse();
    if (scale <= 0.0 || !inv) return op;  // singular placement: nothing projects, nothing to do
    op.bufToParent = *inv;

    switch (op.kind) {
        case Sharpen: {
            op.amount = static_cast<float>(schemaParam(adj, "amount") / 100.0);
            op.effective = op.amount > 0.0f;
            break;
        }
        case UnsharpMask: {
            // sigma = radius/2, the blur family's reading of "radius" (the visually apparent
            // extent), so an Unsharp radius means the same thing a Gaussian Blur radius does.
            op.sigma = static_cast<float>(schemaParam(adj, "radius") * 0.5 * scale);
            op.amount = static_cast<float>(schemaParam(adj, "amount") / 100.0);
            // The Threshold slider is in 8-bit levels (the unit every darkroom/editor states it
            // in); the math is on [0,1] luma.
            op.threshold = static_cast<float>(schemaParam(adj, "threshold") / 255.0);
            op.effective = op.amount > 0.0f && op.sigma > 0.0f;
            break;
        }
        case HighPass: {
            // S34-a: the unsharp difference on its own, so it reads "radius" the same way --
            // sigma = radius/2, the visually apparent extent (docs/filters-stylize.md §3).
            op.sigma = static_cast<float>(schemaParam(adj, "radius") * 0.5 * scale);
            op.effective = op.sigma > 0.0f;
            break;
        }
        case AddNoise: {
            op.noiseSigma = static_cast<float>(schemaParam(adj, "amount") / 100.0);
            op.uniform = static_cast<int>(std::lround(schemaParam(adj, "distribution"))) ==
                         static_cast<int>(core::NoiseDistribution::Uniform);
            op.monochrome = schemaParam(adj, "monochrome") >= 0.5;
            op.seed = static_cast<std::uint32_t>(std::lround(schemaParam(adj, "seed")));
            op.effective = op.noiseSigma > 0.0f;
            break;
        }
        case Denoise: {
            op.radius = std::clamp(
                static_cast<int>(std::lround(schemaParam(adj, "radius") * scale)), 0, 4096);
            op.noiseSigma = static_cast<float>(schemaParam(adj, "noise") / 100.0);
            op.effective = op.radius >= 1 && op.noiseSigma > 0.0f;
            break;
        }
        case Pixelate: {
            op.cell = schemaParam(adj, "size");
            // A cell that lands under ~1.5 buffer px cannot read as a block; below that the
            // honest answer is the untouched backdrop, not a shimmering near-identity.
            op.effective = op.cell * scale >= 1.5;
            break;
        }
        case Emboss: {
            // The tap separation is a parent-space VECTOR, so it maps through the placement
            // whole -- direction and length together -- instead of scaling a length and then
            // rotating it in buffer space.
            const double a = schemaParam(adj, "angle") * kPi / 180.0;
            const double h = schemaParam(adj, "height");
            common::Vec2 off = pre.applyVector({std::cos(a) * h, std::sin(a) * h});
            const double len = std::hypot(off.x, off.y);
            // Floor it at one buffer pixel: a scaled-down preview would otherwise sample the same
            // pixel twice and render flat mid-gray, which reads as "the filter is broken" rather
            // than "the preview is small". The one deliberate break from proportional scaling.
            if (len < 1.0) {
                off = len > 1e-9 ? common::Vec2{off.x / len, off.y / len}
                                 : common::Vec2{std::cos(a), std::sin(a)};
            }
            op.offX = static_cast<float>(off.x);
            op.offY = static_cast<float>(off.y);
            op.amount = static_cast<float>(schemaParam(adj, "amount") / 100.0);
            op.effective = true;  // emboss always replaces colour: it has no identity setting
            break;
        }
        case OilPaint: {
            const double r = schemaParam(adj, "radius") * scale;
            op.half = std::max(1, static_cast<int>(std::lround(r * 0.5)));
            op.effective = r >= 2.0;  // under two buffer px the window IS the pixel
            break;
        }
        case Wave: {
            const double angle = schemaParam(adj, "angle") * kPi / 180.0;
            op.wave.ripple = static_cast<int>(std::lround(schemaParam(adj, "mode"))) ==
                             static_cast<int>(core::WaveMode::Ripple);
            op.wave.amplitude = schemaParam(adj, "amplitude");
            op.wave.wavelength = std::max(1.0, schemaParam(adj, "wavelength"));
            op.wave.dirX = std::cos(angle);
            op.wave.dirY = std::sin(angle);
            op.wave.phase = schemaParam(adj, "phase") * kPi / 180.0;
            op.wave.center = {schemaParam(adj, "center_x"), schemaParam(adj, "center_y")};
            op.effective = op.wave.amplitude * scale >= 0.5;
            break;
        }
        case Vignette: {
            op.vignette.center = {schemaParam(adj, "center_x"), schemaParam(adj, "center_y")};
            op.vignette.radius = schemaParam(adj, "radius");
            op.vignette.outer = 1.0 + schemaParam(adj, "feather") / 100.0;
            // Roundness doubles the superellipse exponent per +100 and halves it per -100:
            // 0 -> 2 (a plain ellipse), +100 -> 4 (squarer, hugs a rectangular frame),
            // -100 -> 1 (a diamond). A smooth, monotonic knob with the familiar shape at 0.
            op.vignette.exponent = 2.0 * std::exp2(schemaParam(adj, "roundness") / 100.0);
            op.vignette.exposure = static_cast<float>(schemaParam(adj, "exposure"));
            op.effective = op.vignette.exposure != 0.0f && op.vignette.radius > 0.0;
            break;
        }
        default: break;
    }
    return op;
}

void runStylizeKernel(ImageF& img, const StylizeOp& op, const common::Affine2D& pre) {
    using enum core::AdjustmentKind;
    switch (op.kind) {
        case Sharpen: fx::sharpenImage(img, op.amount); break;
        case UnsharpMask:
            fx::unsharpMaskImage(img, op.sigma, op.amount, op.threshold, op.draft);
            break;
        case HighPass: fx::highPassImage(img, op.sigma, op.draft); break;
        case AddNoise:
            fx::addNoiseImage(img, op.bufToParent, op.noiseSigma, op.uniform, op.monochrome,
                              op.seed);
            break;
        case Denoise: fx::denoiseImage(img, op.radius, op.noiseSigma); break;
        case Pixelate: fx::pixelateImage(img, op.bufToParent, op.cell); break;
        case Emboss: fx::embossImage(img, op.offX, op.offY, op.amount); break;
        case OilPaint: fx::oilPaintImage(img, op.half); break;
        case Wave: fx::waveImage(img, pre, op.bufToParent, op.wave); break;
        case Vignette: fx::vignetteImage(img, op.bufToParent, op.vignette); break;
        default: break;
    }
}

}  // namespace

bool isStylizeKind(core::AdjustmentKind kind) {
    using enum core::AdjustmentKind;
    switch (kind) {
        case Sharpen:
        case UnsharpMask:
        case AddNoise:
        case Denoise:
        case Pixelate:
        case Emboss:
        case OilPaint:
        case Wave:
        case Vignette:
        // S34-a: High Pass is the unsharp mask's own Gaussian difference, so it lands in this
        // module rather than growing a second Gaussian in compositor.cpp.
        case HighPass: return true;
        default: return false;
    }
}

void applyStylizeAdjustment(ImageF& acc, const core::AdjustmentLayer& adj,
                            const std::vector<float>* coverage, const common::Affine2D& pre,
                            const common::Rect& maskDomain, bool liveDrag) {
    if (acc.empty()) return;
    const StylizeOp op = resolveStylizeOp(adj, pre, liveDrag);
    if (!op.effective) return;  // identity params: a byte-level no-op (the §1 identity rule)

    const float layerOpacity = adj.opacity();
    const core::RasterMask* mk = (adj.hasMask() && adj.mask()->enabled) ? adj.mask() : nullptr;
    const common::Rect maskDom = mk ? adjustmentMaskDomain(adj, *mk, maskDomain) : maskDomain;
    const std::optional<common::Affine2D> preInv = pre.inverse();

    // ---- The UNMODULATED case runs the kernel straight into the accumulator --------------------
    //
    // The blend below needs the ORIGINAL to lerp against, which is why the kernel gets a copy. But
    // when nothing modulates it -- full opacity, no mask, not clipped -- `amt` is >= 1 at every
    // pixel, the blend loop's own fast arm writes `out` into `acc` unchanged, and the copy was
    // round-tripping the accumulator for nothing.
    //
    // At 39.8 MP the accumulator is 637 MB, so that round trip is the whole cost of a cheap kernel:
    // the copy reads and writes it once, the blend reads BOTH buffers and writes one, and a
    // per-pixel stylize kind therefore moved ~5 GB where the scalar adjustment loop beside it moves
    // 1.27 GB. Vignette and Add Noise are per-pixel kinds with reach 0 and were costing ~970 ms
    // each against the ~320 ms that Levels, Vibrance and Hue/Saturation cost for the same walk over
    // the same buffer -- a 3x gap that was entirely this.
    //
    // Byte-identical by construction, not by tolerance: `out = acc; kernel(out); acc = out` IS
    // `kernel(acc)`. The kernels transform in place and read nothing else.
    //
    // ⚠ `preInv` is part of the test because the loop is: a mask is only ever applied
    // `if (mk && preInv)`, so a singular placement means an un-applied mask, which is un-modulated.
    // Reading `mk != nullptr` alone would send that case down the slow path to compute the same
    // answer.
    const bool modulated =
        layerOpacity < 1.0f || coverage != nullptr || (mk != nullptr && preInv.has_value());
    if (!modulated) {
        runStylizeKernel(acc, op, pre);
        return;
    }

    ImageF out = acc;  // straight copy; the kernel transforms it in place
    runStylizeKernel(out, op, pre);

    // Blend the stylised backdrop over the original under opacity * mask * clip coverage, in
    // premultiplied space -- lerping straight RGB across differing alphas is not physical. The
    // amt == 1 fast path keeps the unmodulated case byte-equal to the raw kernel output (every
    // analytic kernel pin depends on it), and amt <= 0 leaves the backdrop byte-identical.
    //
    // Kernels that do not touch alpha come out of this loop with ab == ao, so the premultiplied
    // form collapses to a plain RGB lerp for them; the ones that resample coverage (pixelate,
    // oil paint, wave) get the full treatment, exactly like a blur.
    parallelFor(acc.height, 64, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            std::size_t idx = static_cast<std::size_t>(y) * acc.width;
            for (std::uint32_t x = 0; x < acc.width; ++x, ++idx) {
                float amt = layerOpacity;
                if (mk && preInv) {
                    amt *= adjustmentMaskAt(
                        *mk, preInv->apply({static_cast<double>(x), static_cast<double>(y)}),
                        maskDom);
                }
                if (coverage) amt *= (*coverage)[idx];
                if (amt <= 0.0f) continue;  // masked out: byte-identical backdrop (§2)
                const std::size_t p = idx * 4;
                if (amt >= 1.0f) {
                    acc.rgba[p] = out.rgba[p];
                    acc.rgba[p + 1] = out.rgba[p + 1];
                    acc.rgba[p + 2] = out.rgba[p + 2];
                    acc.rgba[p + 3] = out.rgba[p + 3];
                    continue;
                }
                const float ao = acc.rgba[p + 3];
                const float ab = out.rgba[p + 3];
                const float aOut = std::lerp(ao, ab, amt);
                if (aOut > 1e-6f) {
                    const float inv = 1.0f / aOut;
                    acc.rgba[p] = std::lerp(acc.rgba[p] * ao, out.rgba[p] * ab, amt) * inv;
                    acc.rgba[p + 1] =
                        std::lerp(acc.rgba[p + 1] * ao, out.rgba[p + 1] * ab, amt) * inv;
                    acc.rgba[p + 2] =
                        std::lerp(acc.rgba[p + 2] * ao, out.rgba[p + 2] * ab, amt) * inv;
                } else {  // invisible either way: keep a plain lerp so the RGB stays finite
                    acc.rgba[p] = std::lerp(acc.rgba[p], out.rgba[p], amt);
                    acc.rgba[p + 1] = std::lerp(acc.rgba[p + 1], out.rgba[p + 1], amt);
                    acc.rgba[p + 2] = std::lerp(acc.rgba[p + 2], out.rgba[p + 2], amt);
                }
                acc.rgba[p + 3] = aOut;
            }
        }
    });
}

double stylizeAdjustmentReach(const core::AdjustmentLayer& adj, const common::Rect&) {
    // The domain is unused on purpose: no S35 kind's support grows with the pixel's distance from
    // a centre the way Radial Blur's spin/zoom taps do, so every bound below is a constant of the
    // parameters. The parameter stays for signature symmetry with blurAdjustmentReach.
    using enum core::AdjustmentKind;
    switch (adj.adjustmentKind()) {
        case Sharpen: return 1.0;  // the 3x3 kernel, one px each way
        case UnsharpMask: return 1.5 * schemaParam(adj, "radius");  // 3 * (radius/2) sigma
        case HighPass: return 1.5 * schemaParam(adj, "radius");     // the same Gaussian (S34-a)
        case AddNoise: return 0.0;                                  // per pixel
        case Denoise: return schemaParam(adj, "radius");            // the box window half-width
        // A cell reaches one whole cell past any pixel inside it, so `size` is exactly the bound
        // that makes a region's blocks come out identical to the full composite's.
        case Pixelate: return schemaParam(adj, "size");
        case Emboss: return 0.5 * schemaParam(adj, "height") + 1.0;  // +1 for the bilinear tap
        case OilPaint: return schemaParam(adj, "radius") + 1.0;
        case Wave: return schemaParam(adj, "amplitude") + 1.0;  // the largest displacement
        case Vignette: return 0.0;                              // per pixel
        default: return 0.0;
    }
}

}  // namespace mosaic::render
