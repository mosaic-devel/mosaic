#include "io/quantize.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <unordered_map>

namespace mosaic::io {
namespace {

// The histogram grid: 5 bits per channel, 32768 buckets. Fine enough that a 256-entry palette
// never runs out of distinctions to make, small enough that the nearest-colour lookup table is a
// flat array instead of a hash.
constexpr std::uint32_t kGridBits = 5;
constexpr std::uint32_t kGridSize = 1u << kGridBits;              // 32 levels per channel
constexpr std::size_t kBucketCount = std::size_t{1} << (3 * kGridBits);  // 32768

// How many DISTINCT colours the exactness probe will track before giving up and going to median
// cut. Comfortably above any palette we can emit, and a hard ceiling on the probe's memory.
constexpr std::size_t kExactProbeCap = 4096;

[[nodiscard]] constexpr std::uint32_t bucketOf(std::uint8_t r, std::uint8_t g,
                                               std::uint8_t b) noexcept {
    return (static_cast<std::uint32_t>(r >> 3) << 10) |
           (static_cast<std::uint32_t>(g >> 3) << 5) | static_cast<std::uint32_t>(b >> 3);
}

[[nodiscard]] constexpr std::uint32_t packRgb(std::uint8_t r, std::uint8_t g,
                                              std::uint8_t b) noexcept {
    return (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) | b;
}

// One non-empty histogram bucket: the grid cell it came from, plus the true 8-bit sums, so a
// box's representative is the genuine mean of its pixels rather than the cell centre.
struct Bucket {
    std::uint32_t key = 0;
    std::uint64_t count = 0;
    std::uint64_t sumR = 0;
    std::uint64_t sumG = 0;
    std::uint64_t sumB = 0;
};

[[nodiscard]] constexpr std::uint32_t gridChannel(std::uint32_t key, int channel) noexcept {
    switch (channel) {
    case 0: return (key >> 10) & (kGridSize - 1);
    case 1: return (key >> 5) & (kGridSize - 1);
    default: return key & (kGridSize - 1);
    }
}

// A half-open range of `buckets`, i.e. one box of the median cut.
struct Box {
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct Extent {
    std::array<std::uint32_t, 3> span{{0, 0, 0}};
    std::uint64_t count = 0;
};

[[nodiscard]] Extent extentOf(const std::vector<Bucket>& buckets, const Box& box) {
    Extent out;
    std::array<std::uint32_t, 3> lo{{kGridSize, kGridSize, kGridSize}};
    std::array<std::uint32_t, 3> hi{{0, 0, 0}};
    for (std::size_t i = box.begin; i < box.end; ++i) {
        out.count += buckets[i].count;
        for (int c = 0; c < 3; ++c) {
            const std::uint32_t v = gridChannel(buckets[i].key, c);
            lo[static_cast<std::size_t>(c)] = std::min(lo[static_cast<std::size_t>(c)], v);
            hi[static_cast<std::size_t>(c)] = std::max(hi[static_cast<std::size_t>(c)], v);
        }
    }
    for (int c = 0; c < 3; ++c) {
        const std::size_t ci = static_cast<std::size_t>(c);
        out.span[ci] = hi[ci] >= lo[ci] ? hi[ci] - lo[ci] : 0;
    }
    return out;
}

[[nodiscard]] common::Color8 representative(const std::vector<Bucket>& buckets, const Box& box) {
    std::uint64_t n = 0;
    std::uint64_t r = 0;
    std::uint64_t g = 0;
    std::uint64_t b = 0;
    for (std::size_t i = box.begin; i < box.end; ++i) {
        n += buckets[i].count;
        r += buckets[i].sumR;
        g += buckets[i].sumG;
        b += buckets[i].sumB;
    }
    if (n == 0)
        return common::Color8{0, 0, 0, 255};
    const auto round = [n](std::uint64_t sum) {
        return static_cast<std::uint8_t>(std::min<std::uint64_t>(255, (sum + n / 2) / n));
    };
    return common::Color8{round(r), round(g), round(b), 255};
}

// Heckbert's median cut: repeatedly take the box that is both wide and populous, sort it along
// its longest axis and split it at the pixel-count median.
[[nodiscard]] std::vector<common::Color8> medianCut(std::vector<Bucket>& buckets,
                                                    std::size_t slots) {
    std::vector<Box> boxes;
    if (buckets.empty() || slots == 0)
        return {};
    boxes.push_back(Box{0, buckets.size()});

    while (boxes.size() < slots) {
        std::size_t target = boxes.size();  // == "none found"
        std::uint64_t bestScore = 0;
        int bestChannel = 0;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].end - boxes[i].begin < 2)
                continue;  // a single bucket cannot be cut
            const Extent ext = extentOf(buckets, boxes[i]);
            int channel = 0;
            std::uint32_t span = ext.span[0];
            if (ext.span[1] > span) {
                span = ext.span[1];
                channel = 1;
            }
            if (ext.span[2] > span) {
                span = ext.span[2];
                channel = 2;
            }
            if (span == 0)
                continue;  // every bucket in the box shares one grid cell
            // Width alone splits a wide box holding three pixels ahead of a narrow one holding a
            // million; the pixel count is what makes the palette follow the picture.
            const std::uint64_t score = std::uint64_t{span} * ext.count;
            if (score > bestScore) {
                bestScore = score;
                target = i;
                bestChannel = channel;
            }
        }
        if (target == boxes.size())
            break;  // nothing left that can be split -- fewer colours than asked for is correct

        const Box box = boxes[target];  // by value: the push_back below can reallocate
        std::sort(buckets.begin() + static_cast<std::ptrdiff_t>(box.begin),
                  buckets.begin() + static_cast<std::ptrdiff_t>(box.end),
                  [bestChannel](const Bucket& a, const Bucket& b) {
                      return gridChannel(a.key, bestChannel) < gridChannel(b.key, bestChannel);
                  });
        std::uint64_t total = 0;
        for (std::size_t i = box.begin; i < box.end; ++i)
            total += buckets[i].count;
        // Walk to the count median, but never past the last bucket: both halves must be non-empty
        // or the loop would make no progress and the box list would grow without bound.
        std::size_t split = box.begin + 1;
        std::uint64_t acc = buckets[box.begin].count;
        while (split < box.end - 1 && acc * 2 < total) {
            acc += buckets[split].count;
            ++split;
        }
        boxes[target] = Box{box.begin, split};
        boxes.push_back(Box{split, box.end});
    }

    std::vector<common::Color8> palette;
    palette.reserve(boxes.size());
    for (const Box& box : boxes)
        palette.push_back(representative(buckets, box));
    return palette;
}

[[nodiscard]] std::uint32_t distanceSquared(common::Color8 a, common::Color8 b) noexcept {
    const int dr = static_cast<int>(a.r) - static_cast<int>(b.r);
    const int dg = static_cast<int>(a.g) - static_cast<int>(b.g);
    const int db = static_cast<int>(a.b) - static_cast<int>(b.b);
    return static_cast<std::uint32_t>(dr * dr + dg * dg + db * db);
}

[[nodiscard]] int nearestEntry(const std::vector<common::Color8>& palette, common::Color8 c,
                               int skipIndex) {
    int best = 0;
    std::uint32_t bestDistance = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < palette.size(); ++i) {
        if (static_cast<int>(i) == skipIndex)
            continue;  // the transparent slot is not a colour anything may land on
        const std::uint32_t d = distanceSquared(palette[i], c);
        if (d < bestDistance) {
            bestDistance = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

[[nodiscard]] std::uint8_t clampToByte(float v) noexcept {
    return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
}

// The straight-alpha source composited onto the matte: what an opaque-only format actually sees.
[[nodiscard]] common::Color8 overMatte(const std::uint8_t* px, common::Color8 matte) noexcept {
    const std::uint32_t a = px[3];
    if (a == 255)
        return common::Color8{px[0], px[1], px[2], 255};
    const std::uint32_t ia = 255u - a;
    const auto mix = [a, ia](std::uint32_t s, std::uint32_t m) {
        return static_cast<std::uint8_t>((s * a + m * ia + 127u) / 255u);
    };
    return common::Color8{mix(px[0], matte.r), mix(px[1], matte.g), mix(px[2], matte.b), 255};
}

}  // namespace

QuantizedImage quantize(const common::Image& image, const QuantizeOptions& opts) {
    QuantizedImage out;
    if (image.empty() || image.rgba.size() < image.pixelCount() * 4)
        return out;
    out.width = image.width;
    out.height = image.height;
    const std::size_t pixels = image.pixelCount();
    out.indices.assign(pixels, 0);

    const int maxColors = std::clamp(opts.maxColors, 2, 256);
    const int threshold = std::clamp(opts.alphaThreshold, 0, 256);

    // Pass 1: flatten onto the matte and note which pixels are transparent. Doing it once here
    // means neither the histogram, the exactness probe nor the dither loop has to think about
    // alpha again.
    std::vector<common::Color8> flat(pixels);
    std::vector<std::uint8_t> transparent(pixels, 0);
    bool anyTransparent = false;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t* px = image.rgba.data() + i * 4;
        if (threshold > 0 && static_cast<int>(px[3]) < threshold) {
            transparent[i] = 1;
            anyTransparent = true;
            flat[i] = common::Color8{0, 0, 0, 0};
        } else {
            flat[i] = overMatte(px, opts.matte);
        }
    }

    const int colourSlots = anyTransparent ? maxColors - 1 : maxColors;
    // maxColors >= 2, so a transparent image still leaves at least one colour slot.

    // The exactness probe: if the visible colours already fit, map them one-to-one and the encode
    // is bit-exact. Bails out the moment it sees more colours than could possibly fit.
    std::unordered_map<std::uint32_t, int> exactIndex;
    bool exact = true;
    for (std::size_t i = 0; i < pixels && exact; ++i) {
        if (transparent[i] != 0)
            continue;
        const std::uint32_t key = packRgb(flat[i].r, flat[i].g, flat[i].b);
        if (exactIndex.find(key) != exactIndex.end())
            continue;
        if (exactIndex.size() >= static_cast<std::size_t>(colourSlots) ||
            exactIndex.size() >= kExactProbeCap)
            exact = false;
        else
            exactIndex.emplace(key, static_cast<int>(exactIndex.size()));
    }

    if (exact) {
        out.palette.resize(exactIndex.size());
        for (const auto& [key, index] : exactIndex)
            out.palette[static_cast<std::size_t>(index)] =
                common::Color8{static_cast<std::uint8_t>((key >> 16) & 0xFFu),
                               static_cast<std::uint8_t>((key >> 8) & 0xFFu),
                               static_cast<std::uint8_t>(key & 0xFFu), 255};
        if (out.palette.empty())
            out.palette.push_back(common::Color8{0, 0, 0, 255});  // an all-transparent image
        if (anyTransparent) {
            out.transparentIndex = static_cast<int>(out.palette.size());
            out.palette.push_back(common::Color8{0, 0, 0, 0});
        }
        for (std::size_t i = 0; i < pixels; ++i) {
            if (transparent[i] != 0) {
                out.indices[i] = static_cast<std::uint8_t>(out.transparentIndex);
                continue;
            }
            const auto it = exactIndex.find(packRgb(flat[i].r, flat[i].g, flat[i].b));
            out.indices[i] = static_cast<std::uint8_t>(it == exactIndex.end() ? 0 : it->second);
        }
        out.exact = true;
        return out;
    }

    // Pass 2: the 5-5-5 histogram over the visible pixels, then median cut.
    std::vector<std::uint32_t> bucketSlot(kBucketCount, 0xFFFFFFFFu);
    std::vector<Bucket> buckets;
    for (std::size_t i = 0; i < pixels; ++i) {
        if (transparent[i] != 0)
            continue;
        const std::uint32_t key = bucketOf(flat[i].r, flat[i].g, flat[i].b);
        std::uint32_t slot = bucketSlot[key];
        if (slot == 0xFFFFFFFFu) {
            slot = static_cast<std::uint32_t>(buckets.size());
            bucketSlot[key] = slot;
            buckets.push_back(Bucket{key, 0, 0, 0, 0});
        }
        Bucket& bucket = buckets[slot];
        ++bucket.count;
        bucket.sumR += flat[i].r;
        bucket.sumG += flat[i].g;
        bucket.sumB += flat[i].b;
    }

    out.palette = medianCut(buckets, static_cast<std::size_t>(colourSlots));
    if (out.palette.empty())
        out.palette.push_back(common::Color8{0, 0, 0, 255});
    if (anyTransparent) {
        out.transparentIndex = static_cast<int>(out.palette.size());
        out.palette.push_back(common::Color8{0, 0, 0, 0});
    }

    // Pass 3: map every visible pixel. The nearest-entry search runs once per 5-5-5 cell, not
    // once per pixel, and is filled lazily so a small image never pays for the whole grid.
    std::vector<std::int32_t> nearest(kBucketCount, -1);
    const auto lookup = [&](common::Color8 c) {
        const std::uint32_t key = bucketOf(c.r, c.g, c.b);
        std::int32_t& cached = nearest[key];
        if (cached < 0)
            cached = nearestEntry(out.palette, c, out.transparentIndex);
        return cached;
    };

    if (!opts.dither) {
        for (std::size_t i = 0; i < pixels; ++i)
            out.indices[i] = transparent[i] != 0
                                 ? static_cast<std::uint8_t>(out.transparentIndex)
                                 : static_cast<std::uint8_t>(lookup(flat[i]));
        return out;
    }

    // Floyd-Steinberg, two error rows. Transparent pixels neither receive nor emit error: they
    // are not part of the picture, and bleeding into them would tint the halo around a cut-out.
    const std::size_t w = image.width;
    std::vector<float> curr(3 * (w + 2), 0.0f);
    std::vector<float> next(3 * (w + 2), 0.0f);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        std::fill(next.begin(), next.end(), 0.0f);
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            if (transparent[i] != 0) {
                out.indices[i] = static_cast<std::uint8_t>(out.transparentIndex);
                continue;
            }
            const std::size_t e = 3 * (static_cast<std::size_t>(x) + 1);
            const common::Color8 src = flat[i];
            const common::Color8 wanted{clampToByte(static_cast<float>(src.r) + curr[e]),
                                        clampToByte(static_cast<float>(src.g) + curr[e + 1]),
                                        clampToByte(static_cast<float>(src.b) + curr[e + 2]), 255};
            const int index = lookup(wanted);
            out.indices[i] = static_cast<std::uint8_t>(index);
            const common::Color8 got = out.palette[static_cast<std::size_t>(index)];
            const float er = static_cast<float>(wanted.r) - static_cast<float>(got.r);
            const float eg = static_cast<float>(wanted.g) - static_cast<float>(got.g);
            const float eb = static_cast<float>(wanted.b) - static_cast<float>(got.b);
            const auto spread = [&](std::vector<float>& row, std::size_t at, float weight) {
                row[at] += er * weight;
                row[at + 1] += eg * weight;
                row[at + 2] += eb * weight;
            };
            spread(curr, e + 3, 7.0f / 16.0f);       // (x+1, y)
            spread(next, e - 3, 3.0f / 16.0f);       // (x-1, y+1)
            spread(next, e, 5.0f / 16.0f);           // (x,   y+1)
            spread(next, e + 3, 1.0f / 16.0f);       // (x+1, y+1)
        }
        curr.swap(next);
    }
    return out;
}

}  // namespace mosaic::io
