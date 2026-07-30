#include "common/image.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace mosaic::common {

void Image::fill(Color8 c) {
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        rgba[i + 0] = c.r;
        rgba[i + 1] = c.g;
        rgba[i + 2] = c.b;
        rgba[i + 3] = c.a;
    }
}

Image copyRegion(const Image& src, long x, long y, std::uint32_t w, std::uint32_t h) {
    Image out(w, h);
    for (std::uint32_t row = 0; row < h; ++row) {
        const long sy = y + static_cast<long>(row);
        if (sy < 0 || sy >= static_cast<long>(src.height))
            continue;
        for (std::uint32_t col = 0; col < w; ++col) {
            const long sx = x + static_cast<long>(col);
            if (sx < 0 || sx >= static_cast<long>(src.width))
                continue;
            const std::size_t s = (static_cast<std::size_t>(sy) * src.width + sx) * 4;
            const std::size_t d = (static_cast<std::size_t>(row) * w + col) * 4;
            out.rgba[d] = src.rgba[s];
            out.rgba[d + 1] = src.rgba[s + 1];
            out.rgba[d + 2] = src.rgba[s + 2];
            out.rgba[d + 3] = src.rgba[s + 3];
        }
    }
    return out;
}

void blitRegion(Image& dst, const Image& region, long x, long y) {
    for (std::uint32_t row = 0; row < region.height; ++row) {
        const long dy = y + static_cast<long>(row);
        if (dy < 0 || dy >= static_cast<long>(dst.height))
            continue;
        for (std::uint32_t col = 0; col < region.width; ++col) {
            const long dx = x + static_cast<long>(col);
            if (dx < 0 || dx >= static_cast<long>(dst.width))
                continue;
            const std::size_t s = (static_cast<std::size_t>(row) * region.width + col) * 4;
            const std::size_t d = (static_cast<std::size_t>(dy) * dst.width + dx) * 4;
            dst.rgba[d] = region.rgba[s];
            dst.rgba[d + 1] = region.rgba[s + 1];
            dst.rgba[d + 2] = region.rgba[s + 2];
            dst.rgba[d + 3] = region.rgba[s + 3];
        }
    }
}

void ImageF::fill(ColorF c) {
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        rgba[i + 0] = c.r;
        rgba[i + 1] = c.g;
        rgba[i + 2] = c.b;
        rgba[i + 3] = c.a;
    }
}

std::optional<Rect> alphaBounds(const Image& img) {
    if (img.empty() || img.rgba.size() < img.pixelCount() * 4)
        return std::nullopt;
    std::uint32_t minX = img.width;
    std::uint32_t maxX = 0;
    std::uint32_t minY = 0;
    std::uint32_t maxY = 0;
    bool any = false;
    for (std::uint32_t y = 0; y < img.height; ++y) {
        const std::uint8_t* row = img.rgba.data() + static_cast<std::size_t>(y) * img.width * 4;
        std::uint32_t first = 0;
        while (first < img.width && row[first * 4 + 3] == 0)
            ++first;
        if (first == img.width)
            continue; // a fully transparent row
        std::uint32_t last = img.width - 1;
        while (row[last * 4 + 3] == 0)
            --last; // first <= last, so this terminates
        if (!any)
            minY = y;
        maxY = y;
        minX = std::min(minX, first);
        maxX = std::max(maxX, last);
        any = true;
    }
    if (!any)
        return std::nullopt;
    return Rect{static_cast<double>(minX), static_cast<double>(minY),
                static_cast<double>(maxX - minX + 1), static_cast<double>(maxY - minY + 1)};
}

ImageF toFloat(const Image& src) {
    ImageF out(src.width, src.height);
    const std::size_t n = std::min(src.rgba.size(), out.rgba.size());
    for (std::size_t i = 0; i < n; ++i) {
        out.rgba[i] = static_cast<float>(src.rgba[i]) / 255.0f;
    }
    return out;
}

Image toImage8(const ImageF& src) {
    Image out(src.width, src.height);
    const std::size_t n = std::min(src.rgba.size(), out.rgba.size());
    for (std::size_t i = 0; i < n; ++i) {
        const float v = std::clamp(src.rgba[i], 0.0f, 1.0f);
        // +0.5 then truncate == lround for non-negative input, without the libm call (this
        // runs on every channel of every composite).
        out.rgba[i] = static_cast<std::uint8_t>(v * 255.0f + 0.5f);
    }
    return out;
}

bool writePpm(const Image& img, const std::string& path, std::string* error) {
    if (img.empty() || img.rgba.size() < img.pixelCount() * 4) {
        if (error) *error = "writePpm: empty or undersized image";
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "writePpm: cannot open '" + path + "' for writing";
        return false;
    }
    out << "P6\n" << img.width << ' ' << img.height << "\n255\n";
    for (std::size_t i = 0; i < img.pixelCount(); ++i) {
        const std::size_t p = i * 4;
        const char rgb[3] = {static_cast<char>(img.rgba[p + 0]),
                             static_cast<char>(img.rgba[p + 1]),
                             static_cast<char>(img.rgba[p + 2])};
        out.write(rgb, 3);
    }
    if (!out) {
        if (error) *error = "writePpm: write error on '" + path + "'";
        return false;
    }
    return true;
}

}  // namespace mosaic::common
