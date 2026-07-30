#include "core/color_sample.hpp"

namespace mosaic::core {

int sampleRadius(SampleSize size) noexcept {
    switch (size) {
    case SampleSize::Point:
        return 0;
    case SampleSize::Avg3:
        return 1;
    case SampleSize::Avg5:
        return 2;
    case SampleSize::Avg11:
        return 5;
    }
    return 0;
}

SampleSize sampleSizeFromIndex(int index) noexcept {
    switch (index) {
    case 1:
        return SampleSize::Avg3;
    case 2:
        return SampleSize::Avg5;
    case 3:
        return SampleSize::Avg11;
    default:
        return SampleSize::Point; // 0 and anything out of range
    }
}

std::optional<common::Color8> sampleColor(const common::Image& img, int cx, int cy,
                                          SampleSize size) {
    if (img.empty())
        return std::nullopt;
    const int w = static_cast<int>(img.width);
    const int h = static_cast<int>(img.height);
    if (cx < 0 || cy < 0 || cx >= w || cy >= h)
        return std::nullopt; // the pointer is off the pixels -- nothing to pick

    const int r = sampleRadius(size);
    if (r == 0) {
        const std::size_t p = (static_cast<std::size_t>(cy) * img.width + cx) * 4;
        return common::Color8{img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
    }

    // Clip the window to the image, then average each channel over the pixels that exist. A window
    // hanging off the edge averages fewer pixels rather than reading out of bounds or padding with
    // zeros (which would darken an edge sample toward transparent black).
    const int x0 = std::max(0, cx - r);
    const int y0 = std::max(0, cy - r);
    const int x1 = std::min(w - 1, cx + r);
    const int y1 = std::min(h - 1, cy + r);
    std::uint32_t sr = 0, sg = 0, sb = 0, sa = 0, n = 0;
    for (int y = y0; y <= y1; ++y) {
        std::size_t row = (static_cast<std::size_t>(y) * img.width + x0) * 4;
        for (int x = x0; x <= x1; ++x, row += 4) {
            sr += img.rgba[row + 0];
            sg += img.rgba[row + 1];
            sb += img.rgba[row + 2];
            sa += img.rgba[row + 3];
            ++n;
        }
    }
    // n >= 1 (the centre is in bounds), so the round-to-nearest divide is always defined.
    const auto avg = [n](std::uint32_t s) { return static_cast<std::uint8_t>((s + n / 2) / n); };
    return common::Color8{avg(sr), avg(sg), avg(sb), avg(sa)};
}

} // namespace mosaic::core
