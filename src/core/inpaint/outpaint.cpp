#include "core/inpaint/outpaint.hpp"

namespace mosaic::core::inpaint {

namespace {
[[nodiscard]] bool holeAt(const Selection& m, std::uint32_t x, std::uint32_t y) {
    return !m.isEmpty() && x < m.width() && y < m.height() && m.at(x, y) > 0;
}
} // namespace

double holeFrameFraction(const Selection& holeMask, std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0 || holeMask.isEmpty()) {
        return 0.0;
    }
    std::uint64_t frame = 0;
    std::uint64_t hole = 0;
    for (std::uint32_t x = 0; x < width; ++x) {
        ++frame;
        hole += holeAt(holeMask, x, 0) ? 1 : 0;
        if (height > 1) {
            ++frame;
            hole += holeAt(holeMask, x, height - 1) ? 1 : 0;
        }
    }
    for (std::uint32_t y = 1; y + 1 < height; ++y) {
        ++frame;
        hole += holeAt(holeMask, 0, y) ? 1 : 0;
        if (width > 1) {
            ++frame;
            hole += holeAt(holeMask, width - 1, y) ? 1 : 0;
        }
    }
    return frame == 0 ? 0.0 : static_cast<double>(hole) / static_cast<double>(frame);
}

} // namespace mosaic::core::inpaint
