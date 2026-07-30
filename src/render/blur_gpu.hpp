#pragma once

#include <memory>
#include <string>

#include "common/image.hpp"
#include "render/compositor.hpp"  // BlurOp + the BlurRenderOverride seam contract

namespace mosaic::render {

// The Vulkan compute lane of the S33 blur adjustments (docs/blur-filters.md §8): the heavy
// kernels -- Gaussian, Surface (bilateral), Lens (aperture gather) and DoF (pyramid
// interpolation) -- served behind the compositor's BlurRenderOverride seam. The CPU kernels in
// render/blur.cpp stay the permanent reference: the app injects render() through
// setBlurRenderOverride, the test binary NEVER installs an override (the byte-pinned CPU
// goldens depend on that), and this lane is held to the CPU lane by tolerance-based parity
// tests (tests/test_blur_gpu.cpp). Both lanes run float and share every formula, so parity
// drift is fused-multiply-add and transcendental rounding, never formula divergence.
//
// Lane coverage: GaussianBlur, SurfaceBlur, LensBlur, DofBlur. Everything else returns false
// so the CPU serves (Box is an O(1)/px running sum a readback would only slow down, and the
// Motion/Spin/Zoom gathers are cheap) -- per-call fallback, the §8 contract.
//
// Everything host-cookable is cooked on the CPU: the Gaussian/bilateral weight tables with the
// same std::exp on the same floats the CPU lane tabulates, the aperture taps via the SAME
// fx::makeApertureKernel the CPU lane calls, and the sRGB decode table verbatim -- so the
// shaders carry only the per-pixel math (the S55-h cook discipline).
//
// ⚠ INVARIANT, and it binds this lane exactly as it binds the CPU one: DofBlur renders by
// interpolating between independently pre-blurred pyramid levels, each blurred FROM THE SOURCE
// -- never a per-pixel variable-radius gather, never a level-from-previous-level cascade. Both
// are obvious "faster"/"more exact" rewrites and both are deliberately refused; the dispatch
// code and shaders/blur_dof.comp carry the full warnings.
//
// PERSISTENT: one context + the four pipelines for the object's lifetime; buffers grow on
// demand (VMA). NOT thread-safe -- the app's installer serialises calls.
class BlurGpu {
public:
    // nullptr (with `error` set) when no usable Vulkan device exists.
    static std::unique_ptr<BlurGpu> create(bool enableValidation, std::string& error);
    ~BlurGpu();
    BlurGpu(const BlurGpu&) = delete;
    BlurGpu& operator=(const BlurGpu&) = delete;

    // The seam contract (compositor.hpp BlurRenderOverride): transform `img` in place to what
    // the CPU dispatch produces for `op` -- the same picture within float-lane tolerance.
    // false = fall back to the CPU lane (unsupported kind, absurd size, device error); `img`
    // is untouched then.
    bool render(common::ImageF& img, const BlurOp& op);

    [[nodiscard]] std::string deviceName() const;

private:
    BlurGpu();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace mosaic::render
