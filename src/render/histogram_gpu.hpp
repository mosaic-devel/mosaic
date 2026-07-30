#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "common/image.hpp"

namespace mosaic::render {

// The 5 x 256 bins the Channels-tab histogram is drawn from, in the CPU reference's own order and
// units (src/ui/channels_panel.hpp, ChannelHistogram).
//
// ⚠ The COLOUR bins are ALPHA-WEIGHTED and their unit is "alpha", not "pixels": a fully opaque
// pixel contributes 255, a half-transparent one 128, a fully transparent one nothing. The ALPHA
// bins are plain pixel counts over EVERY pixel. That asymmetry is not an optimisation, it is the
// fix for a user-reported defect (see shaders/histogram_bins.comp) and both lanes obey it.
//
// Nothing else is stored, because nothing else has to be: every total and every mean
// `computeHistogram` reports is exactly recoverable from these five arrays in integer arithmetic --
// sum_v v*r[v] IS its alpha-weighted red sum, sum_v r[v] IS its visible-alpha weight, sum_v a[v] IS
// its pixel count. `ui::gpuHistogramBinner` does that reconstruction, next to the reference it has
// to match.
struct HistogramBins {
    std::array<std::uint64_t, 256> r{};
    std::array<std::uint64_t, 256> g{};
    std::array<std::uint64_t, 256> b{};
    std::array<std::uint64_t, 256> a{};
    std::array<std::uint64_t, 256> luma{};

    void clear() noexcept { *this = HistogramBins{}; }
};

// The Vulkan compute lane of the Channels-tab histogram (S60-e; docs/s60-performance-plan.md §4
// and docs/s60-readback-consumers.md consumer A5).
//
// The consumer this serves bins the WHOLE canvas into 5 x 256 bins every time the composite
// changes while the tab is visible -- ~33 MB of scanning per revision bump at 4K, and since the
// region writer started notifying too (§10.3) that cadence is potentially per brush dab. The audit
// puts its latency tolerance at HIGH ("a histogram one frame behind is invisible"), which is
// exactly what makes it safe to move off the frame path and onto the device.
//
// The CPU kernel in src/ui/channels_panel.cpp stays the permanent reference: it defines what the
// bins ARE, every refusal here falls back to it, and tests/test_histogram_gpu.cpp holds this lane
// to it BYTE-IDENTICALLY. Bins are integers, so there is no tolerance to hide behind and none is
// wanted -- a mismatch of one count is a real defect.
//
// PERSISTENT: one shared context + one pipeline for the object's lifetime; buffers grow on demand
// (VMA). NOT thread-safe -- the panel's caller serialises it on the UI thread.
class HistogramGpu {
public:
    // nullptr (with `error` set to a named reason) when the lane may not or cannot be built:
    // CPU-only mode, no usable Vulkan device, or a device whose limits do not fit the kernel.
    static std::unique_ptr<HistogramGpu> create(bool enableValidation, std::string& error);
    ~HistogramGpu();
    HistogramGpu(const HistogramGpu&) = delete;
    HistogramGpu& operator=(const HistogramGpu&) = delete;

    // Bin `img` (straight-alpha RGBA8) into `out`. false = the caller must run the CPU reference
    // (an empty or malformed image, a canvas past the lane's byte cap, a device error); `out` is
    // then untouched.
    [[nodiscard]] bool bin(const common::Image& img, HistogramBins& out);

    [[nodiscard]] std::string deviceName() const;

private:
    HistogramGpu();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace mosaic::render
