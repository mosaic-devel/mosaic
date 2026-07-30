#include "bench.hpp"

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "common/profiler.hpp"
#include "common/version.hpp"
#include "core/adjustments.hpp"
#include "core/blend_mode.hpp"
#include "core/brush/brush_engine.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/text/shaping.hpp"
#include "core/text/text_layer_render.hpp"
#include "core/text/text_model.hpp"
#include "platform/font_db.hpp"
#include "render/compositor.hpp"
#include "render/render.hpp"
#include "render/tile_compositor.hpp"
#include "render/vulkan_context.hpp"

// See bench.hpp for what this is and the rules it lives by.
namespace mosaic::app {
namespace {

namespace common = mosaic::common;
namespace core = mosaic::core;
namespace render = mosaic::render;

// ---------------------------------------------------------------------------------------------
// Timing + statistics
// ---------------------------------------------------------------------------------------------

// Starts on construction; ms() reads the elapsed wall time. steady_clock, never system_clock: an
// NTP step mid-run must not become a benchmark result.
class Watch {
public:
    [[nodiscard]] double ms() const noexcept {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_t0)
            .count();
    }

private:
    std::chrono::steady_clock::time_point m_t0 = std::chrono::steady_clock::now();
};

struct Stats {
    double min = 0.0;
    double median = 0.0;
    double mean = 0.0;
    double max = 0.0;
};

// The median, not the mean, is the number to read: one descheduled sample skews a mean and leaves
// the median where it was. Both are printed so a run whose two disagree is visible as such.
[[nodiscard]] Stats statsOf(std::vector<double> v) {
    Stats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.min = v.front();
    s.max = v.back();
    const std::size_t n = v.size();
    s.median = (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
    s.mean = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(n);
    return s;
}

// One measured thing: a name, the shape it was measured at (so the number stays interpretable a
// month later), and its samples. A case that could not run carries `skip` instead of samples.
// `rows` is the profiler's view of THIS case alone -- see CaseProfile.
struct Case {
    std::string name;
    std::string shape;
    std::vector<double> ms;
    std::string skip;
    std::vector<common::ProfileRow> rows;
};

[[nodiscard]] Case skipped(std::string name, std::string shape, std::string why) {
    Case c;
    c.name = std::move(name);
    c.shape = std::move(shape);
    c.skip = std::move(why);
    return c;
}

// "1920x1080, 4 layers" -- the shape column's house style.
[[nodiscard]] std::string shapeOf(std::uint32_t w, std::uint32_t h, std::size_t layers,
                                  const std::string& extra = {}) {
    std::string s = std::to_string(w) + "x" + std::to_string(h) + ", " + std::to_string(layers) +
                    (layers == 1 ? " layer" : " layers");
    if (!extra.empty()) s += ", " + extra;
    return s;
}

// Numeric output is DATA, not UI: i18n::init() has already moved LC_NUMERIC to the user's locale,
// so %f would emit their decimal separator -- a comma here, which silently breaks any harness
// parsing these numbers. Pin the C numeric locale for the report and restore it after; the same
// reason and the same shape as dumpProfile() in main.cpp.
class CNumericLocale {
public:
    CNumericLocale() {
        const char* prev = std::setlocale(LC_NUMERIC, nullptr);
        m_saved = prev != nullptr ? prev : "C";
        std::setlocale(LC_NUMERIC, "C");
    }
    ~CNumericLocale() { std::setlocale(LC_NUMERIC, m_saved.c_str()); }
    CNumericLocale(const CNumericLocale&) = delete;
    CNumericLocale& operator=(const CNumericLocale&) = delete;
    CNumericLocale(CNumericLocale&&) = delete;
    CNumericLocale& operator=(CNumericLocale&&) = delete;

private:
    std::string m_saved;
};

// ---------------------------------------------------------------------------------------------
// Per-case profiler attribution
// ---------------------------------------------------------------------------------------------
//
// THE INSTRUMENT BUG THIS FIXES (docs/s60-performance-plan.md §7, the 2026-07-28 gate, condition
// 4): `common::Profiler` is a PROCESS-wide collector keyed by (name, lane), so one --bench run
// piles every case's samples into the same rows. The gate asks that "`Tile upload` on
// Lane::GpuDevice is ~0 for the `gpu region` rows" -- unanswerable when the `gpu edit` rows, which
// re-upload BY DESIGN, are in the same bucket. Clearing the collector before a case and
// snapshotting after it attributes the rows to exactly one case, and the condition becomes
// checkable as written.
//
// ⚠ The consequence, stated so nobody reads the exit dump as a total: under --profile /
// MOSAIC_PROFILE=1, the stderr dump at process exit now shows only the LAST scoped case's rows.
// The per-case table below is the one to read.
class CaseProfile {
public:
    CaseProfile() noexcept : m_on(common::Profiler::enabled()) {
        if (m_on) common::Profiler::instance().clear();
    }
    void finish(Case& c) const {
        if (m_on) c.rows = common::Profiler::instance().snapshot();
    }
    CaseProfile(const CaseProfile&) = delete;
    CaseProfile& operator=(const CaseProfile&) = delete;
    CaseProfile(CaseProfile&&) = delete;
    CaseProfile& operator=(CaseProfile&&) = delete;

private:
    bool m_on;
};

// The case column. Wide enough for the longest name any scenario mints -- "5000x8000 cpu region
// 256 +upload" is 32 -- because a table whose columns stop lining up at one row is a table nobody
// reads down. Both per-case tables below use it, so they cannot drift apart.
constexpr int kCaseCol = 32;

// The DEVICE rows only. Lane::Gpu is wall-clock at the call site and is already what the main
// table measures; Lane::GpuDevice is what the device actually spent, and the gap between the two
// is the diagnostic the plan's §8.1 is about. `total ms` is avg x n -- the per-case device time,
// which is the number the "Tile upload is 66% of device time" finding was made of.
void printProfileRows(const std::vector<Case>& cases) {
    if (!common::Profiler::enabled()) {
        std::printf("\n[bench] no per-case device rows: re-run with MOSAIC_PROFILE=1 (or\n"
                    "        --profile) to attribute Lane::GpuDevice time to each case.\n");
        std::fflush(stdout);
        return;
    }
    bool scoped = false;
    bool any = false;
    for (const Case& c : cases) {
        if (!c.rows.empty()) scoped = true;
        for (const common::ProfileRow& r : c.rows)
            if (r.lane == common::Lane::GpuDevice) any = true;
    }
    if (!scoped) {
        // Only `tile-composite` and `present-upload` scope their cases so far; adopting CaseProfile
        // elsewhere is two lines, and this line is what says so rather than implying the device was
        // silent.
        std::printf("\n[bench] this scenario does not scope its cases to the profiler; rows are\n"
                    "        process-wide (see the --profile dump at exit).\n");
        std::fflush(stdout);
        return;
    }
    if (!any) {
        // Not an error: MOSAIC_GPU_PROFILE=floor refuses the timestamp pool by design, and a
        // device without a timestamp counter composites exactly as well without one.
        std::printf("\n[bench] no per-case device rows: this lane reported no device time\n"
                    "        (no timestamp pool -- MOSAIC_GPU_PROFILE=floor, or CPU-only).\n");
        std::fflush(stdout);
        return;
    }
    std::printf("\n%-*s %-28s %5s %9s %9s %9s %9s\n", kCaseCol, "case", "device row (DEV)", "n",
                "min ms", "avg ms", "max ms", "total ms");
    std::printf("%s\n", std::string(kCaseCol + 28 + 5 + 9 * 4 + 6, '-').c_str());
    for (const Case& c : cases) {
        bool first = true;
        for (const common::ProfileRow& r : c.rows) {
            if (r.lane != common::Lane::GpuDevice) continue;
            std::printf("%-*s %-28s %5llu %9.3f %9.3f %9.3f %9.3f\n", kCaseCol,
                        first ? c.name.c_str() : "", r.name.c_str(),
                        static_cast<unsigned long long>(r.count), r.min, r.avg, r.max,
                        r.avg * static_cast<double>(r.count));
            first = false;
        }
    }
    std::fflush(stdout);
}

// The HOST rows of the same per-case snapshots -- Lane::Cpu (host work) and Lane::Gpu (wall clock
// AT THE CALL SITE, i.e. what the caller pays, including staging, submit and any fence wait).
//
// THIS IS WHERE A FRAME'S TWO HALVES SEPARATE. A `+upload` case is one Watch around composite AND
// present, which is the only honest end-to-end number -- but a single blended number hides which
// half moved, so the same case also carries `Bench composite (...)` and `Bench canvas upload (...)`
// as two rows here. Composite and upload therefore stay separately attributable without inventing
// a second reporting path: it is the same CaseProfile snapshot the DEV table above is printed from,
// read at a different lane.
//
// Silent when nothing was collected -- printProfileRows() has already said whether the profiler was
// on at all, and saying it twice trains people to skip both lines.
void printHostRows(const std::vector<Case>& cases) {
    bool any = false;
    for (const Case& c : cases)
        for (const common::ProfileRow& r : c.rows)
            if (r.lane != common::Lane::GpuDevice) any = true;
    if (!any) return;
    std::printf("\n%-*s %-28s %4s %5s %9s %9s %9s %9s\n", kCaseCol, "case", "host row", "lane", "n",
                "min ms", "avg ms", "max ms", "total ms");
    std::printf("%s\n", std::string(kCaseCol + 28 + 4 + 5 + 9 * 4 + 7, '-').c_str());
    for (const Case& c : cases) {
        bool first = true;
        for (const common::ProfileRow& r : c.rows) {
            if (r.lane == common::Lane::GpuDevice) continue;
            std::printf("%-*s %-28s %4s %5llu %9.3f %9.3f %9.3f %9.3f\n", kCaseCol,
                        first ? c.name.c_str() : "", r.name.c_str(), common::laneName(r.lane),
                        static_cast<unsigned long long>(r.count), r.min, r.avg, r.max,
                        r.avg * static_cast<double>(r.count));
            first = false;
        }
    }
    std::fflush(stdout);
}

void printTable(const std::vector<Case>& cases) {
    std::printf("\n%-*s %-40s %5s %9s %9s %9s %9s\n", kCaseCol, "case", "shape", "n", "min ms",
                "median ms", "mean ms", "max ms");
    std::printf("%s\n", std::string(kCaseCol + 40 + 5 + 9 * 4 + 6, '-').c_str());
    for (const Case& c : cases) {
        if (!c.skip.empty()) {
            std::printf("%-*s %-40s %5s  (skipped: %s)\n", kCaseCol, c.name.c_str(),
                        c.shape.c_str(), "-", c.skip.c_str());
            continue;
        }
        const Stats s = statsOf(c.ms);
        std::printf("%-*s %-40s %5zu %9.3f %9.3f %9.3f %9.3f\n", kCaseCol, c.name.c_str(),
                    c.shape.c_str(), c.ms.size(), s.min, s.median, s.mean, s.max);
    }
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------------------------
// Synthetic documents (deterministic, built in code -- nothing on disk, no UI)
// ---------------------------------------------------------------------------------------------

// splitmix64: the fixed-seed stream every synthetic pixel comes from. Same seed -> same document
// on every machine and every run, which is the whole point of a benchmark harness.
[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Fill `img` with a deterministic gradient + per-pixel noise. `opaque` layers are alpha 255
// everywhere (a background photo); the rest carry a soft alpha frame that fades to nothing at the
// canvas edge, so the composite walks real partial coverage instead of a trivially-opaque stack.
void fillSynthetic(common::Image& img, std::uint64_t seed, bool opaque) {
    if (img.empty()) return;
    const std::uint32_t w = img.width;
    const std::uint32_t h = img.height;
    const std::uint32_t inset = std::max(1u, std::min(w, h) / 8);
    std::vector<std::uint8_t> gradX(w), alphaX(w);
    for (std::uint32_t x = 0; x < w; ++x) {
        gradX[x] = static_cast<std::uint8_t>(w > 1 ? (x * 255u) / (w - 1) : 0u);
        const std::uint32_t d = std::min(x, w - 1 - x);
        alphaX[x] = static_cast<std::uint8_t>(std::min(255u, (d * 255u) / inset));
    }
    for (std::uint32_t y = 0; y < h; ++y) {
        std::uint64_t s = mix64(seed ^ (static_cast<std::uint64_t>(y) << 32));
        const auto gy = static_cast<std::uint32_t>(h > 1 ? (y * 255u) / (h - 1) : 0u);
        const std::uint32_t dy = std::min(y, h - 1 - y);
        const auto ay = static_cast<std::uint8_t>(std::min(255u, (dy * 255u) / inset));
        std::uint8_t* px = &img.rgba[static_cast<std::size_t>(y) * w * 4];
        for (std::uint32_t x = 0; x < w; ++x) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            const auto n = static_cast<std::uint32_t>(s >> 56);
            px[0] = static_cast<std::uint8_t>((gradX[x] * 3u + n) / 4u);
            px[1] = static_cast<std::uint8_t>((gy * 3u + n) / 4u);
            px[2] = static_cast<std::uint8_t>((255u - gradX[x]) * 3u / 4u + n / 4u);
            px[3] = opaque ? 255 : std::min(alphaX[x], ay);
            px += 4;
        }
    }
}

// The stack the synthetic documents are built from: an opaque background under up to three
// partially-transparent layers in the blend modes an ordinary edit actually uses. Fixed order, so
// "4 layers" means the same four everywhere in the table.
struct StackEntry {
    const char* name;
    core::BlendMode mode;
    float opacity;
};
constexpr StackEntry kStack[] = {
    {"Background", core::BlendMode::Normal, 1.0f},
    {"Multiply", core::BlendMode::Multiply, 1.0f},
    {"Screen", core::BlendMode::Screen, 0.85f},
    {"Overlay", core::BlendMode::Overlay, 0.60f},
};
constexpr std::size_t kMaxStackLayers = std::size(kStack);

// `layers` full-canvas raster layers of the stack above (clamped to kMaxStackLayers).
[[nodiscard]] std::unique_ptr<core::Document> makeBenchDoc(std::uint32_t w, std::uint32_t h,
                                                           std::size_t layers) {
    auto doc = std::make_unique<core::Document>(w, h);
    for (std::size_t i = 0; i < std::min(layers, kMaxStackLayers); ++i) {
        const StackEntry& e = kStack[i];
        auto layer = doc->makeRaster(e.name);
        fillSynthetic(layer->image(), 0xB0A7C0DEull + static_cast<std::uint64_t>(i) * 7919ull,
                      i == 0);
        layer->setBlendMode(e.mode);
        layer->setOpacity(e.opacity);
        doc->root().addOnTop(std::move(layer));
    }
    return doc;
}

// The display composite's options, as the interactive app builds them: no baked checkerboard (the
// present shader draws it in screen space) and Auto resampling (the Move tool's default).
[[nodiscard]] render::CompositeOptions displayOptions(bool liveDrag = false) {
    render::CompositeOptions opts;
    opts.checkerboard = false;
    opts.resampleFilter = render::ResampleFilter::Auto;
    opts.liveDrag = liveDrag;
    return opts;
}

// ---------------------------------------------------------------------------------------------
// The PRESENT HALF: the host upload every scenario used to stop short of
// ---------------------------------------------------------------------------------------------
//
// THE MEASUREMENT BUG THIS FIXES. A frame is TWO halves -- composite the pixels, then get them into
// the canvas texture the present pass samples -- and until now `--bench` measured only the first.
// That is not a footnote, it is a bias with a direction: the resident lane's half of the second
// step (`resolve()`) was already inside the `gpu` rows, while the CPU lane's half (`Canvas upload
// (full)` / `(region)` in app_window.cpp, profiler rows that exist precisely because no scenario
// covered them) was in no row at all. So every CPU row flattered the CPU lane by exactly the thing
// the S60-a arc exists to delete, and "a tie in the table is a GPU win in the app" had to be
// repeated by hand next to every quotation of the numbers.
//
// WHAT THE APP ACTUALLY DOES, and what this class reproduces step for step (the chain
// docs/s60-readback-consumers.md §5 calls "three CPU-side copies before the staging write"):
//
//   full     m_lastComposite <- the composite (a MOVE: free, so it is not modelled)
//            -> VulkanCanvas::m_documentImage  = img          (host copy)
//            -> WindowRenderer::m_pendingCanvas = img         (host copy)
//            -> memcpy into the mapped staging buffer         (host copy)
//            -> barrier, vkCmdCopyBufferToImage, barrier      (device)
//   region   patchComposite(sub) into the doc-sized mirror    (host copy, per row)
//            -> VulkanCanvas::m_documentRegion  = sub         (host copy)
//            -> WindowRenderer::m_pendingCanvasRegion = sub   (host copy)
//            -> memcpy into staging, then a SUB-RECT copy     (host copy + device)
//
// HOW IT IS STAGED WITHOUT A SWAPCHAIN. The destination is an ordinary device-local
// R8G8B8A8_UNORM image in the SAME usage the canvas texture is created with
// (TRANSFER_DST | SAMPLED | STORAGE, ensureCanvasTexture), on `VulkanContext::shared()` -- the
// headless device. No surface is involved, because an upload never needed one; only the present
// pass that samples the texture afterwards does, and that pass is not what this measures.
//
// ⚠ `TileCompositor::createResolveTarget` is the obvious destination and CANNOT serve: its image
// carries STORAGE | SAMPLED | TRANSFER_SRC and no TRANSFER_DST, so nothing may be copied INTO it.
// Owning the image here has a second, better reason anyway -- the CPU present half must be
// measurable on a device that REFUSES the resident lane, which is exactly the floor profile.
//
// WHAT IS STILL EXCLUDED, stated so nobody has to rediscover it:
//   * The present pass itself (canvas_present.comp) and the swapchain blit. Both lanes pay it
//     identically -- it samples the same texture whichever half wrote it -- so leaving it out
//     changes no comparison, only the absolute frame cost.
//   * The app batches this copy into the frame's ONE command buffer and waits its fence at the
//     START of the next frame; here it is its own submit and its own fence wait. That overstates
//     the CPU present half by roughly one submit plus one stall -- and `TileCompositor::resolve()`
//     submits and waits exactly the same way, so the two lanes are overstated symmetrically. This
//     is the honest choice: the alternative measures "queued" on one lane and "arrived" on the
//     other.
//   * VMA. The app stages through a VMA allocation with HOST_ACCESS_SEQUENTIAL_WRITE; this uses a
//     plain HOST_VISIBLE|HOST_COHERENT allocation. On a device where VMA would pick host-visible
//     device-local (resizable BAR) memory the app's memcpy is over PCIe and this one is not.
class CanvasUpload {
public:
    // Build the canvas texture + staging for a `w`x`h` document. Returns null with `why` set, and
    // every reason is an ordinary outcome the caller reports as a skipped row: no device at all, or
    // a canvas past `maxImageDimension2D` -- which the app's present path could not show as one
    // texture either (see WindowRenderer::prepareResidentCanvas), so it is a finding, not a gap.
    [[nodiscard]] static std::unique_ptr<CanvasUpload> create(std::uint32_t w, std::uint32_t h,
                                                              std::string& why);
    ~CanvasUpload();
    CanvasUpload(const CanvasUpload&) = delete;
    CanvasUpload& operator=(const CanvasUpload&) = delete;
    CanvasUpload(CanvasUpload&&) = delete;
    CanvasUpload& operator=(CanvasUpload&&) = delete;

    // presentComposite(): the whole canvas goes up.
    [[nodiscard]] bool full(const common::Image& img, std::string& error);
    // presentCompositeRegion(): a sub-rect patches the texture in place, and the doc-sized CPU
    // mirror is patched beside it -- the app does both, and the mirror patch is host work the
    // resident lane deletes just as surely as the staging write is.
    [[nodiscard]] bool region(const common::Image& sub, std::uint32_t x, std::uint32_t y,
                              std::string& error);

private:
    CanvasUpload() = default;
    void patchMirror(const common::Image& sub, std::uint32_t x, std::uint32_t y);
    [[nodiscard]] bool copyToTexture(std::uint32_t x, std::uint32_t y, std::uint32_t w,
                                     std::uint32_t h, bool wholeImage, std::string& error);

    std::shared_ptr<render::VulkanContext> m_ctx;
    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_imageMemory = VK_NULL_HANDLE;
    VkBuffer m_staging = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
    void* m_mapped = nullptr;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;
    // The layout the texture is in between uploads, tracked exactly as WindowRenderer tracks it:
    // UNDEFINED before the first upload, SHADER_READ_ONLY_OPTIMAL after every one (the present pass
    // samples it there). The barrier's source mask depends on which, so guessing costs correctness.
    VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint32_t m_w = 0, m_h = 0;

    // The host buffers the chain above copies through. Allocated once and reused, which is the
    // app's steady state too: after the first frame these assignments are memcpys into a vector
    // that is already the right size, not allocations.
    common::Image m_mirror;         // app_window::m_lastComposite -- lazily sized (region path only)
    common::Image m_canvasFull;     // VulkanCanvas::m_documentImage
    common::Image m_pendingFull;    // WindowRenderer::m_pendingCanvas
    common::Image m_canvasRegion;   // VulkanCanvas::m_documentRegion
    common::Image m_pendingRegion;  // WindowRenderer::m_pendingCanvasRegion
};

std::unique_ptr<CanvasUpload> CanvasUpload::create(std::uint32_t w, std::uint32_t h,
                                                   std::string& why) {
    if (w == 0 || h == 0) {
        why = "zero-sized canvas";
        return nullptr;
    }
    auto self = std::unique_ptr<CanvasUpload>(new CanvasUpload());
    self->m_ctx = render::VulkanContext::shared(/*enableValidation=*/true, why);
    if (!self->m_ctx) {
        if (why.empty()) why = "no Vulkan device";
        return nullptr;
    }
    if (!self->m_ctx->caps().fitsImage(w, h)) {
        why = "canvas texture exceeds maxImageDimension2D (" +
              std::to_string(self->m_ctx->caps().maxImageDim) + ")";
        return nullptr;
    }
    self->m_w = w;
    self->m_h = h;
    const VkDevice dev = self->m_ctx->device();
    // Byte for byte the canvas texture WindowRenderer::ensureCanvasTexture creates. The usage set
    // is copied deliberately rather than minimised: usage flags steer the driver's tiling choice,
    // so an image created SAMPLED-only could measure a different copy than the app's.
    const VkImageCreateInfo ici{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_STORAGE_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev, &ici, nullptr, &self->m_image) != VK_SUCCESS) {
        why = "vkCreateImage failed for the canvas texture";
        return nullptr;
    }
    VkMemoryRequirements imgReq{};
    vkGetImageMemoryRequirements(dev, self->m_image, &imgReq);
    const std::uint32_t imgType =
        self->m_ctx->findMemoryType(imgReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (imgType == UINT32_MAX) {
        why = "no device-local memory type for the canvas texture";
        return nullptr;
    }
    const VkMemoryAllocateInfo imgAlloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = imgReq.size,
        .memoryTypeIndex = imgType,
    };
    if (vkAllocateMemory(dev, &imgAlloc, nullptr, &self->m_imageMemory) != VK_SUCCESS ||
        vkBindImageMemory(dev, self->m_image, self->m_imageMemory, 0) != VK_SUCCESS) {
        why = "no memory for the canvas texture";
        return nullptr;
    }

    // The staging buffer is FULL-CANVAS even for region uploads, exactly as the app's is: a region
    // stages into the front of it. Sizing it to the region instead would measure an allocation
    // pattern the app does not have.
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
    const VkBufferCreateInfo bci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(dev, &bci, nullptr, &self->m_staging) != VK_SUCCESS) {
        why = "vkCreateBuffer failed for the upload staging buffer";
        return nullptr;
    }
    VkMemoryRequirements bufReq{};
    vkGetBufferMemoryRequirements(dev, self->m_staging, &bufReq);
    const std::uint32_t bufType = self->m_ctx->findMemoryType(
        bufReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (bufType == UINT32_MAX) {
        why = "no host-visible memory type for the upload staging buffer";
        return nullptr;
    }
    const VkMemoryAllocateInfo bufAlloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = bufReq.size,
        .memoryTypeIndex = bufType,
    };
    if (vkAllocateMemory(dev, &bufAlloc, nullptr, &self->m_stagingMemory) != VK_SUCCESS ||
        vkBindBufferMemory(dev, self->m_staging, self->m_stagingMemory, 0) != VK_SUCCESS ||
        vkMapMemory(dev, self->m_stagingMemory, 0, VK_WHOLE_SIZE, 0, &self->m_mapped) !=
            VK_SUCCESS) {
        why = "no host-visible memory for the upload staging buffer";
        return nullptr;
    }

    // Our own pool: command pools are externally synchronized and the shared context's own pool is
    // not for borrowers (vulkan_context.hpp).
    self->m_pool = self->m_ctx->createCommandPool(why);
    if (self->m_pool == VK_NULL_HANDLE) return nullptr;
    const VkCommandBufferAllocateInfo cbai{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = self->m_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(dev, &cbai, &self->m_cmd) != VK_SUCCESS) {
        why = "vkAllocateCommandBuffers failed for the canvas upload";
        return nullptr;
    }
    const VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(dev, &fci, nullptr, &self->m_fence) != VK_SUCCESS) {
        why = "vkCreateFence failed for the canvas upload";
        return nullptr;
    }
    return self;
}

CanvasUpload::~CanvasUpload() {
    if (!m_ctx) return;
    const VkDevice dev = m_ctx->device();
    // Every submit was fence-waited before returning, so there is nothing in flight to wait on.
    if (m_fence != VK_NULL_HANDLE) vkDestroyFence(dev, m_fence, nullptr);
    if (m_pool != VK_NULL_HANDLE) vkDestroyCommandPool(dev, m_pool, nullptr);  // frees m_cmd
    if (m_mapped != nullptr) vkUnmapMemory(dev, m_stagingMemory);
    if (m_staging != VK_NULL_HANDLE) vkDestroyBuffer(dev, m_staging, nullptr);
    if (m_stagingMemory != VK_NULL_HANDLE) vkFreeMemory(dev, m_stagingMemory, nullptr);
    if (m_image != VK_NULL_HANDLE) vkDestroyImage(dev, m_image, nullptr);
    if (m_imageMemory != VK_NULL_HANDLE) vkFreeMemory(dev, m_imageMemory, nullptr);
}

void CanvasUpload::patchMirror(const common::Image& sub, std::uint32_t x, std::uint32_t y) {
    // app_window::patchComposite, verbatim in shape: the doc-sized mirror the cursor readout and
    // the histogram read has to stay consistent with what went to the screen, so a region frame
    // writes the pixels TWICE on the host. Allocated on first use -- the full path never patches,
    // and a doc-sized zero fill is 160 MB at 5000x8000.
    if (m_mirror.empty()) m_mirror = common::Image(m_w, m_h);
    if (sub.empty() || x + sub.width > m_mirror.width || y + sub.height > m_mirror.height) return;
    for (std::uint32_t row = 0; row < sub.height; ++row) {
        const std::uint8_t* src = &sub.rgba[static_cast<std::size_t>(row) * sub.width * 4];
        std::uint8_t* dst =
            &m_mirror.rgba[(static_cast<std::size_t>(y + row) * m_mirror.width + x) * 4];
        std::memcpy(dst, src, static_cast<std::size_t>(sub.width) * 4);
    }
}

bool CanvasUpload::full(const common::Image& img, std::string& error) {
    if (img.width != m_w || img.height != m_h || img.empty()) {
        error = "full upload: image is not the canvas size";
        return false;
    }
    MOSAIC_PERF_SCOPE("Bench canvas upload (full)", common::Lane::Gpu);
    {
        MOSAIC_PERF_SCOPE("Bench canvas stage (full)", common::Lane::Cpu);
        m_canvasFull = img;           // VulkanCanvas::setDocumentImage
        m_pendingFull = m_canvasFull; // WindowRenderer::setCanvasImage
        std::memcpy(m_mapped, m_pendingFull.rgba.data(), m_pendingFull.rgba.size());
    }
    return copyToTexture(0, 0, m_w, m_h, /*wholeImage=*/true, error);
}

bool CanvasUpload::region(const common::Image& sub, std::uint32_t x, std::uint32_t y,
                          std::string& error) {
    if (sub.empty() || x + sub.width > m_w || y + sub.height > m_h) {
        error = "region upload: sub-rect does not fit the canvas texture";
        return false;
    }
    MOSAIC_PERF_SCOPE("Bench canvas upload (region)", common::Lane::Gpu);
    {
        MOSAIC_PERF_SCOPE("Bench canvas stage (region)", common::Lane::Cpu);
        patchMirror(sub, x, y);           // app_window::patchComposite
        m_canvasRegion = sub;             // VulkanCanvas::setDocumentRegion
        m_pendingRegion = m_canvasRegion; // WindowRenderer::setCanvasRegion
        std::memcpy(m_mapped, m_pendingRegion.rgba.data(), m_pendingRegion.rgba.size());
    }
    return copyToTexture(x, y, sub.width, sub.height, /*wholeImage=*/false, error);
}

bool CanvasUpload::copyToTexture(std::uint32_t x, std::uint32_t y, std::uint32_t w,
                                 std::uint32_t h, bool wholeImage, std::string& error) {
    const VkDevice dev = m_ctx->device();
    vkResetCommandBuffer(m_cmd, 0);
    const VkCommandBufferBeginInfo cbi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(m_cmd, &cbi) != VK_SUCCESS) {
        error = "vkBeginCommandBuffer failed for the canvas upload";
        return false;
    }
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // ⚠ A REGION upload must never transition out of UNDEFINED: that transition may discard the
    // image, which throws away every pixel the copy does not rewrite. It cannot happen here (the
    // caller uploads a full canvas first, exactly as the app does), and the assertion is this
    // barrier reading the tracked layout rather than assuming one.
    const bool wasReadable = m_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkImageMemoryBarrier toDst{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = static_cast<VkAccessFlags>(wasReadable ? VK_ACCESS_SHADER_READ_BIT : 0),
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = m_layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = m_image,
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(
        m_cmd,
        wasReadable ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
    const VkBufferImageCopy copy =
        wholeImage ? VkBufferImageCopy{.bufferOffset = 0,
                                       .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                       .imageExtent = {w, h, 1}}
                   : VkBufferImageCopy{
                         .bufferOffset = 0,
                         .bufferRowLength = w,
                         .bufferImageHeight = h,
                         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .imageOffset = {static_cast<std::int32_t>(x),
                                         static_cast<std::int32_t>(y), 0},
                         .imageExtent = {w, h, 1}};
    vkCmdCopyBufferToImage(m_cmd, m_staging, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);
    const VkImageMemoryBarrier toRead{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = m_image,
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toRead);
    vkEndCommandBuffer(m_cmd);
    vkResetFences(dev, 1, &m_fence);
    const VkSubmitInfo si{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &m_cmd,
    };
    if (m_ctx->submit(si, m_fence) != VK_SUCCESS) {
        error = "vkQueueSubmit failed for the canvas upload";
        return false;
    }
    vkWaitForFences(dev, 1, &m_fence, VK_TRUE, UINT64_MAX);
    m_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

// resolve() takes its destination BY CONST REFERENCE and so cannot write the new layout back; the
// caller owns that fact (tile_compositor.hpp, ResolveTarget::layout). Leaving it at UNDEFINED --
// which every --bench GPU row did before 2026-07-29 -- makes resolve() take its FULL-CANVAS branch
// on every single call, because `dst.layout == UNDEFINED` is one of the three things that force
// one. The `gpu region` rows were therefore paying a whole-canvas resolve per frame while claiming
// to measure the incremental path, and the app (which tracks the layout in
// WindowRenderer::residentCanvasLayout) never behaved that way. ⚠ `wrote` gates the update: resolve
// leaves the image EXACTLY as it found it when it had nothing to do, and claiming otherwise
// desynchronises the layout from the image.
[[nodiscard]] bool resolveInto(render::TileCompositor& lane, render::ResolveTarget& target,
                               std::string& error) {
    bool wrote = false;
    if (!lane.resolve(target, error, &wrote)) return false;
    if (wrote) target.layout = VK_IMAGE_LAYOUT_GENERAL;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Scenario: composite-full -- the core S60 number
// ---------------------------------------------------------------------------------------------

std::vector<Case> runCompositeFull(std::uint32_t iters) {
    struct Shape {
        std::uint32_t w, h;
        std::size_t layers;
    };
    static constexpr Shape kShapes[] = {
        {512, 512, 4}, {1920, 1080, 4}, {3840, 2160, 4}, {5000, 8000, 4},
    };
    std::vector<Case> cases;
    for (const Shape& sh : kShapes) {
        const auto doc = makeBenchDoc(sh.w, sh.h, sh.layers);
        const render::CompositeOptions opts = displayOptions();
        Case c;
        c.name = std::to_string(sh.w) + "x" + std::to_string(sh.h);
        c.shape = shapeOf(sh.w, sh.h, doc->layerCount());
        (void)render::composite(*doc, opts, render::Backend::Cpu);  // warm the allocator
        for (std::uint32_t i = 0; i < iters; ++i) {
            Watch w;
            const render::CompositeResult r = render::composite(*doc, opts, render::Backend::Cpu);
            c.ms.push_back(w.ms());
            if (!r.ok) {
                c.skip = "composite failed: " + r.error;
                c.ms.clear();
                break;
            }
        }
        cases.push_back(std::move(c));
    }
    return cases;
}

// ---------------------------------------------------------------------------------------------
// Scenario: composite-region -- the brush/typing fast path
// ---------------------------------------------------------------------------------------------

std::vector<Case> runCompositeRegion(std::uint32_t iters) {
    constexpr std::uint32_t kW = 4096;
    constexpr std::uint32_t kH = 4096;
    static constexpr std::uint32_t kRois[] = {32, 64, 128, 256, 512};
    const auto doc = makeBenchDoc(kW, kH, 4);
    const render::CompositeOptions opts = displayOptions();
    std::vector<Case> cases;
    for (const std::uint32_t roi : kRois) {
        Case c;
        c.name = std::to_string(roi) + "x" + std::to_string(roi) + " roi";
        c.shape = shapeOf(kW, kH, doc->layerCount(),
                          "roi " + std::to_string(roi) + "x" + std::to_string(roi));
        for (std::uint32_t i = 0; i < iters + 1; ++i) {
            // Walk the ROI down the diagonal so no two samples re-read the same cache lines --
            // a fixed rect measures a hot cache, not a dirty-region recomposite.
            const double off = 64.0 + static_cast<double>((i * 137u) % 2048u);
            const common::Rect rect{off, off, static_cast<double>(roi), static_cast<double>(roi)};
            Watch w;
            const render::CompositeResult r =
                render::compositeRegion(*doc, rect, opts, render::Backend::Cpu);
            const double ms = w.ms();
            if (i == 0) continue;  // warm-up
            if (!r.ok) {
                c.skip = "compositeRegion failed: " + r.error;
                c.ms.clear();
                break;
            }
            c.ms.push_back(ms);
        }
        cases.push_back(std::move(c));
    }
    return cases;
}

// ---------------------------------------------------------------------------------------------
// Scenario: paint-stroke -- S19 interactive latency
// ---------------------------------------------------------------------------------------------

// One stroke's worth of frames on the top raster layer: per frame, feed one sample, stamp the dabs
// it unlocked, and recomposite the rect they dirtied -- exactly the canvas's live-brush loop.
// Samples are PER FRAME (not per stroke): that is the latency the user feels.
void paintOneStroke(core::Document& doc, core::RasterLayer& target, double diameter,
                    std::uint32_t samplesPerStroke, std::vector<double>& out, bool record) {
    namespace brush = core::brush;
    brush::BrushParams params;
    params.diameter = diameter;
    params.hardness = 0.8;
    params.flow = 1.0;
    params.opacity = 1.0;
    params.spacing = 0.10;
    params.color = common::Color8{28, 32, 40, 255};
    const auto at = [&](std::uint32_t i) {
        const double t = static_cast<double>(i);
        return brush::StrokeInput{{160.0 + t * 18.0, 300.0 + 120.0 * std::sin(t * 0.21)},
                                  0.55 + 0.4 * std::sin(t * 0.11),
                                  0.0,
                                  0.0,
                                  0.0,
                                  0.0,
                                  static_cast<std::uint64_t>(i) * 8000ull};
    };
    const render::CompositeOptions opts = displayOptions();
    brush::BrushEngine eng;
    eng.begin(target.image().width, target.image().height, target.image(), params,
              brush::BrushDynamics{}, at(0));
    for (std::uint32_t i = 1; i < samplesPerStroke; ++i) {
        Watch w;
        eng.extendTo(at(i));
        const common::Rect dirty = eng.composite();
        if (!dirty.empty())
            (void)render::compositeRegion(doc, dirty, opts, render::Backend::Cpu);
        const double ms = w.ms();
        // The dab walk lags the sample stream by one sample (brush_engine.hpp), so the first frame
        // or two stamp nothing. Those are real frames but they measure nothing; drop them rather
        // than plant a fake 0.01 ms minimum in the table.
        if (record && !dirty.empty()) out.push_back(ms);
    }
    eng.restore();  // leave the layer pristine so every stroke starts from the same pixels
    eng.end();
}

std::vector<Case> runPaintStroke(std::uint32_t iters) {
    struct Shape {
        std::uint32_t w, h;
        double diameter;
    };
    static constexpr Shape kShapes[] = {
        {1920, 1080, 24.0}, {1920, 1080, 96.0}, {4096, 4096, 24.0}, {4096, 4096, 96.0},
    };
    constexpr std::uint32_t kSamplesPerStroke = 40;
    std::vector<Case> cases;
    for (const Shape& sh : kShapes) {
        const auto doc = makeBenchDoc(sh.w, sh.h, 3);
        auto* target = doc->root().child(doc->root().childCount() - 1).as<core::RasterLayer>();
        Case c;
        c.name = std::to_string(sh.w) + "x" + std::to_string(sh.h) + " tip " +
                 std::to_string(static_cast<int>(sh.diameter)) + "px";
        c.shape = shapeOf(sh.w, sh.h, doc->layerCount(),
                          std::to_string(kSamplesPerStroke - 1) + " frames/stroke");
        if (target == nullptr) {
            c.skip = "no raster layer";
            cases.push_back(std::move(c));
            continue;
        }
        std::vector<double> unused;
        paintOneStroke(*doc, *target, sh.diameter, kSamplesPerStroke, unused, /*record=*/false);
        for (std::uint32_t i = 0; i < iters; ++i)
            paintOneStroke(*doc, *target, sh.diameter, kSamplesPerStroke, c.ms, /*record=*/true);
        cases.push_back(std::move(c));
    }
    return cases;
}

// ---------------------------------------------------------------------------------------------
// Scenario: move-fullcanvas -- the open user-reported bug (PLAN §2)
// ---------------------------------------------------------------------------------------------

// Per drag frame the pointer moves, the dragged layer's transform is rewritten, and the canvas
// recomposites. Two CPU paths exist: DragCompositeCache (only the moved layer is re-rasterised,
// everything else replays from cached buffers) and the plain full walk it falls back to. Both are
// measured, plus the once-per-gesture "below" composite that seeds the GPU-resident drag -- on a
// 5000x8000 document that seed is itself a felt stall.
std::vector<Case> runMoveFullcanvas(std::uint32_t iters) {
    struct Shape {
        std::uint32_t w, h;
    };
    static constexpr Shape kShapes[] = {{1920, 1080}, {5000, 8000}};
    std::vector<Case> cases;
    for (const Shape& sh : kShapes) {
        const auto doc = makeBenchDoc(sh.w, sh.h, 2);  // background + the full-canvas layer moved
        core::Layer& moved = doc->root().child(doc->root().childCount() - 1);
        const std::string size = std::to_string(sh.w) + "x" + std::to_string(sh.h);
        const bool gpuEligible = render::canUseGpuDrag(*doc, moved.id());

        // (a) the drag-cache path, integer-pixel pointer deltas (what a mouse produces: Auto
        //     resolves to the lossless Nearest kernel).
        // (b) the drag-cache path, SUB-pixel deltas (a tablet, or a scaled/rotated gesture: Auto
        //     drops to Bilinear mid-gesture).
        // (c) the full walk -- the fallback whenever the cache declines (nested/masked/effects).
        struct Variant {
            const char* suffix;
            bool subPixel;
            bool useCache;
        };
        static constexpr Variant kVariants[] = {
            {" drag-cache int", false, true},
            {" drag-cache subpx", true, true},
            {" full-walk int", false, false},
        };
        for (const Variant& v : kVariants) {
            render::DragCompositeCache cache;
            const render::CompositeOptions opts = displayOptions(/*liveDrag=*/true);
            Case c;
            c.name = size + v.suffix;
            c.shape = shapeOf(sh.w, sh.h, doc->layerCount(),
                              v.subPixel ? "sub-px drag" : "integer drag");
            const auto frame = [&](std::uint32_t i) {
                const double d = static_cast<double>(static_cast<int>(i % 41u) - 20);
                const double sub = v.subPixel ? 0.37 : 0.0;
                moved.setTransform(common::Affine2D::translation(d + sub, -d * 0.5 + sub));
                if (v.useCache) return cache.composite(*doc, moved.id(), opts).has_value();
                return render::composite(*doc, opts, render::Backend::Cpu).ok;
            };
            (void)frame(0);  // the cache's build frame is not a drag frame; never time it
            for (std::uint32_t i = 1; i <= iters; ++i) {
                Watch w;
                const bool ok = frame(i);
                const double ms = w.ms();
                if (!ok) {
                    c.skip = v.useCache ? "drag cache declined this stack" : "composite failed";
                    c.ms.clear();
                    break;
                }
                c.ms.push_back(ms);
            }
            cases.push_back(std::move(c));
        }
        moved.setTransform(common::Affine2D::identity());

        // (d) the gesture-START cost: the one full composite (with the dragged layer hidden) that
        //     seeds the GPU-resident drag path. Paid once per gesture, but paid on mouse-down.
        {
            Case c;
            c.name = size + " gesture-start";
            c.shape = shapeOf(sh.w, sh.h, doc->layerCount(),
                              gpuEligible ? "gpu-drag eligible" : "gpu-drag NOT eligible");
            moved.setVisible(false);
            const render::CompositeOptions opts = displayOptions();
            (void)render::composite(*doc, opts, render::Backend::Cpu);
            for (std::uint32_t i = 0; i < iters; ++i) {
                Watch w;
                const bool ok = render::composite(*doc, opts, render::Backend::Cpu).ok;
                c.ms.push_back(w.ms());
                if (!ok) {
                    c.skip = "composite failed";
                    c.ms.clear();
                    break;
                }
            }
            moved.setVisible(true);
            cases.push_back(std::move(c));
        }

        // (e) the gesture-END cost -- the stall that survived an entire benchmark pass because
        //     nothing was named after it (docs/s60-gesture-start-stall.md §1.3, finding G3).
        //
        //     `gestureEnded` -> `syncAfterEdit` invalidates the drag cache and asks for a full
        //     recomposite. There is no cache to reuse: the whole drag ran on the GPU, so
        //     `recompositeNow` was never called during it and nothing was ever built. Release
        //     therefore pays the FULL WALK plus a full-canvas upload, and it measures LARGER than
        //     the gesture-start seed it is paired with (1712 ms quiet / 4672 ms loaded at
        //     5000x8000, against 919/3068). A fix that addresses only the start turns
        //     freeze -> move -> freeze into move -> freeze, which is half a fix.
        //
        //     Two things make this row different from the `full-walk int` drag row above, and both
        //     of them make it more expensive rather than less: `liveDrag=false` (the commit is not
        //     a preview, so `Auto` stops dropping to a cheap kernel) and a SUB-PIXEL placement (a
        //     committed drag almost never lands on the integer grid, and off-grid is what takes
        //     `Auto` to its quality kernel). Measuring it with a live-drag integer translate --
        //     which is what the existing rows do -- is measuring a different, cheaper operation.
        {
            Case c;
            c.name = size + " gesture-end";
            c.shape = shapeOf(sh.w, sh.h, doc->layerCount(), "committed sub-px placement");
            const render::CompositeOptions opts = displayOptions(/*liveDrag=*/false);
            moved.setTransform(common::Affine2D::translation(37.5, -12.5));
            (void)render::composite(*doc, opts, render::Backend::Cpu);  // warm the allocator
            for (std::uint32_t i = 0; i < iters; ++i) {
                // Move it every iteration so no future cache can report "already current" and
                // quietly turn this row into a no-op the day one is added.
                const double d = static_cast<double>(static_cast<int>(i % 7u));
                moved.setTransform(common::Affine2D::translation(37.5 + d, -12.5 - d));
                Watch w;
                const bool ok = render::composite(*doc, opts, render::Backend::Cpu).ok;
                c.ms.push_back(w.ms());
                if (!ok) {
                    c.skip = "composite failed";
                    c.ms.clear();
                    break;
                }
            }
            moved.setTransform(common::Affine2D::identity());
            cases.push_back(std::move(c));
        }
    }
    return cases;
}

// ---------------------------------------------------------------------------------------------
// Scenario: type-keystroke -- the 64 ms regression that must not come back
// ---------------------------------------------------------------------------------------------

std::vector<Case> runTypeKeystroke(std::uint32_t iters) {
    namespace text = core::text;
    std::vector<Case> cases;
    platform::FontDB fonts;
    const std::string family = fonts.defaultFamily();
    text::FontRef probe;
    probe.family = family;
    // One keystroke = replace ONE character in the middle of a settled paragraph. Replacing rather
    // than appending keeps the block's length -- and therefore the dirty band -- steady, so every
    // sample measures the same thing; the character cycles so the block genuinely changes and the
    // cache cannot report "already current".
    static constexpr char kCycle[] = "abcdefghijklmnop";
    const std::string seed =
        "The quick brown fox jumps over the lazy dog, and then it does so again "
        "because a paragraph of body copy is what a keystroke actually re-shapes.";

    struct Variant {
        const char* suffix;
        bool regionPath;  // false = the pre-S60-b full-document composite
    };
    static constexpr Variant kVariants[] = {{" region", true}, {" full-composite", false}};

    // A font-less sandbox is a legitimate machine to build on -- report the cases as skipped
    // rather than pretend a number (the shaper has nothing to shape).
    if (fonts.families().empty() || !fonts.resolve(probe).has_value()) {
        for (const Variant& v : kVariants)
            cases.push_back(skipped(std::string("1920x1080") + v.suffix, "1920x1080, 2 layers",
                                    "no usable font on this machine"));
        return cases;
    }

    for (const Variant& v : kVariants) {
        constexpr std::uint32_t kW = 1920;
        constexpr std::uint32_t kH = 1080;
        const auto doc = makeBenchDoc(kW, kH, 1);
        text::CharStyle style;
        style.setSolidFill(common::ColorF{0.06f, 0.07f, 0.09f, 1.0f});
        style.sizePx = 28.0f;
        style.font.family = family;
        auto textLayer = doc->makeText("Body");
        textLayer->setBlock(text::makeBlock(seed, style));
        textLayer->setTransform(common::Affine2D::translation(120.0, 240.0));
        const core::LayerId textId = textLayer->id();
        doc->root().addOnTop(std::move(textLayer));
        core::Layer* found = doc->find(textId);
        auto* tl = found != nullptr ? found->as<core::TextLayer>() : nullptr;
        if (tl == nullptr) {
            cases.push_back(skipped(std::string("1920x1080") + v.suffix,
                                    shapeOf(kW, kH, doc->layerCount()), "text layer went missing"));
            continue;
        }

        text::TextShaper shaper;
        const render::CompositeOptions opts = displayOptions();
        common::Rect warm{};
        (void)text::refreshTextCaches(*doc, shaper, fonts, textId, false, &warm);
        (void)render::composite(*doc, opts, render::Backend::Cpu);

        Case c;
        c.name = std::string("1920x1080") + v.suffix;
        c.shape = shapeOf(kW, kH, doc->layerCount(),
                          v.regionPath ? "dirty-region path" : "full composite");
        const std::size_t caret = seed.size() / 2;
        for (std::uint32_t i = 0; i < iters + 1; ++i) {
            text::TextBlock block = tl->block();
            const char ch = kCycle[i % (sizeof kCycle - 1)];
            text::replaceText(block, caret, caret + 1, std::string_view(&ch, 1));
            Watch w;
            tl->setBlock(std::move(block));
            common::Rect dirty{};
            (void)text::refreshTextCaches(*doc, shaper, fonts, textId, false, &dirty);
            if (v.regionPath) {
                if (!dirty.empty())
                    (void)render::compositeRegion(*doc, dirty, opts, render::Backend::Cpu);
            } else {
                (void)render::composite(*doc, opts, render::Backend::Cpu);
            }
            const double ms = w.ms();
            if (i > 0) c.ms.push_back(ms);  // i == 0 is the warm-up keystroke
        }
        cases.push_back(std::move(c));
    }
    return cases;
}

// ---------------------------------------------------------------------------------------------
// Scenarios: blur-live (S33) and adjustment-live (S32)
// ---------------------------------------------------------------------------------------------

// A live parameter drag: rewrite one key of the adjustment's bag and recomposite the document.
// `liveDrag` is what the app sets mid-scrub (the S33 blur kernels then draft-subsample their taps),
// so a blur case is measured with it on and a colour case with it off -- as the app does.
[[nodiscard]] Case adjustmentDrag(const char* label, std::uint32_t w, std::uint32_t h,
                                  core::AdjustmentKind kind, const char* key, double base,
                                  double span, bool liveDrag, std::uint32_t iters,
                                  const std::string& extraShape) {
    Case c;
    c.name = label;
    c.shape = shapeOf(w, h, 2, extraShape);
    if (!core::adjustmentImplemented(kind)) {
        c.skip = "adjustment kind not implemented";
        return c;
    }
    const auto doc = makeBenchDoc(w, h, 1);
    auto adj = doc->makeAdjustment(std::string(core::adjustmentKindName(kind)), kind);
    core::seedAdjustmentDefaults(*adj);
    const core::LayerId id = adj->id();
    doc->root().addOnTop(std::move(adj));
    c.shape = shapeOf(w, h, doc->layerCount(), extraShape);
    core::Layer* found = doc->find(id);
    auto* layer = found != nullptr ? found->as<core::AdjustmentLayer>() : nullptr;
    if (layer == nullptr) {
        c.skip = "adjustment layer went missing";
        return c;
    }
    const render::CompositeOptions opts = displayOptions(liveDrag);
    for (std::uint32_t i = 0; i < iters + 1; ++i) {
        // A scrub sweeps the knob; stepping it keeps every sample a genuinely different value, so
        // no cache anywhere can answer "unchanged".
        layer->params()[key] = base + span * static_cast<double>(i % 8) / 8.0;
        Watch t;
        const bool ok = render::composite(*doc, opts, render::Backend::Cpu).ok;
        const double ms = t.ms();
        if (!ok) {
            c.skip = "composite failed";
            c.ms.clear();
            return c;
        }
        if (i > 0) c.ms.push_back(ms);  // i == 0 is the warm-up
    }
    return c;
}

std::vector<Case> runBlurLive(std::uint32_t iters) {
    using K = core::AdjustmentKind;
    std::vector<Case> cases;
    cases.push_back(adjustmentDrag("1920x1080 gaussian r10", 1920, 1080, K::GaussianBlur, "radius",
                                   10.0, 4.0, true, iters, "radius 10-14 px"));
    cases.push_back(adjustmentDrag("1920x1080 gaussian r48", 1920, 1080, K::GaussianBlur, "radius",
                                   48.0, 8.0, true, iters, "radius 48-56 px"));
    cases.push_back(adjustmentDrag("1920x1080 lens r15", 1920, 1080, K::LensBlur, "radius", 15.0,
                                   4.0, true, iters, "radius 15-19 px"));
    cases.push_back(adjustmentDrag("3840x2160 gaussian r10", 3840, 2160, K::GaussianBlur, "radius",
                                   10.0, 4.0, true, iters, "radius 10-14 px"));
    return cases;
}

std::vector<Case> runAdjustmentLive(std::uint32_t iters) {
    using K = core::AdjustmentKind;
    std::vector<Case> cases;
    cases.push_back(adjustmentDrag("1920x1080 brightness", 1920, 1080, K::BrightnessContrast,
                                   "brightness", -0.4, 0.8, false, iters, "brightness drag"));
    cases.push_back(adjustmentDrag("1920x1080 hue/sat", 1920, 1080, K::HueSaturation, "saturation",
                                   -0.4, 0.8, false, iters, "saturation drag"));
    cases.push_back(adjustmentDrag("1920x1080 levels", 1920, 1080, K::Levels, "gamma", 0.6, 1.2,
                                   false, iters, "gamma drag"));
    cases.push_back(adjustmentDrag("3840x2160 brightness", 3840, 2160, K::BrightnessContrast,
                                   "brightness", -0.4, 0.8, false, iters, "brightness drag"));
    return cases;
}

// ---------------------------------------------------------------------------------------------
// Scenario: tile-composite -- the resident GPU lane against the CPU walk (item 13's gate)
// ---------------------------------------------------------------------------------------------
//
// THE MEASUREMENT ITEM 13 IS WITHHELD PENDING. Item 13 flips app_window's composite call sites off
// render::Backend::Cpu, and the plan's exit criterion is that the resident lane beats the CPU walk
// on the same box, back to back, in one process. Nothing in --bench touched the GPU lane before
// this scenario, so that criterion had no instrument.
//
// What is being compared is the whole per-frame path, not "GPU blend vs CPU blend":
//
//   today    render::compositeRegion() -> patch a doc-sized CPU mirror -> 2 more host copies ->
//            memcpy into staging -> vkCmdCopyBufferToImage
//   proposed markDirty(rect) -> composite() -> resolve() straight into the present texture,
//            with ZERO host bytes in either direction
//
// ⚠ READ THE `+upload` ROWS FOR THE COMPARISON. `cpu full` and `cpu region 256` are the COMPOSITE
// HALF ONLY and are kept unchanged, because three sessions of this plan's tables are stated in
// them; but the `gpu` rows have always included resolve(), so those two rows against a `gpu` row
// compare four fifths of one frame with a whole one. `cpu full +upload` and `cpu region 256
// +upload` are the same composite plus the app's present chain (mirror patch, two host copies,
// staging write, copy-to-image, submit, fence) and are the rows that answer "which lane is faster
// at putting this frame on the screen". The caveat this scenario used to carry -- "the CPU rows are
// optimistic, so a tie in the table is a GPU win in the app" -- is what those rows retire.
//
// The split between the two halves is NOT folded into one number: under MOSAIC_PROFILE=1 the
// per-case HOST table breaks each `+upload` row into `Bench composite (...)` and `Bench canvas
// upload (...)`, so a row that moved says which half moved.
//
// Read the two `gpu edit` rows hardest, and read them TOGETHER. They are the brush/typing case:
//
//   `gpu edit 256`        markLayerDirty(layer, rect) -- the incremental path (S60-a, 2026-07-29).
//                         Only the dirty macrotiles of the edited layer are uploaded, and only the
//                         document macrotiles they project onto are recomposited. Condition 3 of
//                         the item-13 gate is read on THIS row.
//   `gpu edit 256 whole`  markLayerDirty(id) -- the whole-layer upload, kept as a standing
//                         baseline. It is what made this case 18.804 ms at 3840x2160 on
//                         2026-07-28, i.e. 4.8x over the gate's budget, and keeping it in the
//                         table is what stops the improvement being an argument.
//
// If `gpu edit 256` is still poor on a big canvas the flip is STILL not the right move, and the
// failing row names the next item -- which is exactly the sort of thing a flip-on-argument misses.
//
// A refusal is a RESULT, not a failure: the row prints `(skipped: refused: ...)` with the refusal
// name, and "the lane declines a 5000x8000 document on this device" is a finding worth the line.
std::vector<Case> runTileComposite(std::uint32_t iters) {
    struct Shape {
        std::uint32_t w, h;
        std::size_t layers;
    };
    static constexpr Shape kShapes[] = {
        {512, 512, 4}, {1920, 1080, 4}, {3840, 2160, 4}, {5000, 8000, 4},
    };
    // One macrotile at the default k (gpu_caps.hpp kMacrotileDefaultShift = 2 -> 256 px), so the
    // region rows compare the same amount of dirty area on both lanes.
    constexpr double kRoi = 256.0;
    // ⚠ The ROI has to WALK (a fixed rect measures a hot cache, not a dirty-region recomposite) and
    // it has to stay INSIDE the document. The fixed `64 + (i*137) % 1024` walk this scenario was
    // first written with was borrowed from composite-region, whose document is 4096 square; here the
    // smallest canvas is 512, where from the fifth sample the rect sat wholly outside it,
    // compositeRegion rightly refused, and the 512-square CPU baseline -- the row the GPU lane is
    // measured AGAINST -- was silently dropped from the table. One offset feeds both axes, so the
    // span is bounded by the SMALLER dimension.
    const auto roiOffset = [](std::uint32_t i, std::uint32_t w, std::uint32_t h) {
        const double span = std::max(1.0, static_cast<double>(std::min(w, h)) - kRoi);
        return std::fmod(static_cast<double>(i) * 137.0, span);
    };

    std::vector<Case> cases;
    for (const Shape& sh : kShapes) {
        const auto doc = makeBenchDoc(sh.w, sh.h, sh.layers);
        const render::CompositeOptions opts = displayOptions();
        const std::string shape = shapeOf(sh.w, sh.h, doc->layerCount());
        const std::string prefix = std::to_string(sh.w) + "x" + std::to_string(sh.h) + " ";
        const std::string roiShape =
            shapeOf(sh.w, sh.h, doc->layerCount(), "roi 256x256");

        {   // The baseline the flip has to beat: today's full CPU walk.
            Case c;
            c.name = prefix + "cpu full";
            c.shape = shape;
            (void)render::composite(*doc, opts, render::Backend::Cpu);  // warm the allocator
            const CaseProfile prof;
            for (std::uint32_t i = 0; i < iters; ++i) {
                Watch w;
                const bool ok = render::composite(*doc, opts, render::Backend::Cpu).ok;
                c.ms.push_back(w.ms());
                if (!ok) {
                    c.skip = "composite failed";
                    c.ms.clear();
                    break;
                }
            }
            prof.finish(c);
            cases.push_back(std::move(c));
        }
        {   // ... and today's dirty-region walk, the brush/typing fast path.
            Case c;
            c.name = prefix + "cpu region 256";
            c.shape = roiShape;
            const CaseProfile prof;
            for (std::uint32_t i = 0; i < iters + 1; ++i) {
                const double off = roiOffset(i, sh.w, sh.h);
                const common::Rect rect{off, off, kRoi, kRoi};
                Watch w;
                const bool ok = render::compositeRegion(*doc, rect, opts, render::Backend::Cpu).ok;
                const double ms = w.ms();
                if (i == 0) continue;  // warm-up
                if (!ok) {
                    c.skip = "compositeRegion failed";
                    c.ms.clear();
                    break;
                }
                c.ms.push_back(ms);
            }
            prof.finish(c);
            cases.push_back(std::move(c));
        }

        // ---- ... and the same two frames, WHOLE: composite AND get the pixels into the canvas
        //      texture. The `gpu` rows below have always included their half of that step
        //      (resolve()); these are the rows that give the CPU lane its.
        //
        //      Scoped tight and torn down before the resident lane is built: the destination image
        //      plus its full-canvas staging buffer plus the host copies are ~4 doc-images of memory
        //      (640 MB at 5000x8000), and holding them alongside the accumulator would measure this
        //      machine's swap behaviour rather than either lane.
        {
            std::string why;
            const auto upload = CanvasUpload::create(sh.w, sh.h, why);
            if (!upload) {
                cases.push_back(skipped(prefix + "cpu full +upload", shape, why));
                cases.push_back(skipped(prefix + "cpu region 256 +upload", roiShape, why));
            } else {
                {   // Full composite -> full-canvas upload. The gesture-end / undo / tab-switch
                    // frame, end to end.
                    Case c;
                    c.name = prefix + "cpu full +upload";
                    c.shape = shape;
                    {   // The first upload transitions the texture out of UNDEFINED; every later
                        // one starts from SHADER_READ_ONLY, which is the app's steady state. In
                        // its own scope so the warm-up's doc-sized image is freed before the loop
                        // starts allocating its own -- at 5000x8000 that is 160 MB of headroom.
                        const render::CompositeResult warm =
                            render::composite(*doc, opts, render::Backend::Cpu);
                        if (warm.ok) (void)upload->full(warm.image, why);
                    }
                    const CaseProfile prof;
                    for (std::uint32_t i = 0; i < iters; ++i) {
                        Watch w;
                        render::CompositeResult r;
                        {   // The scope is what splits this row's two halves in the host table.
                            // It costs one relaxed atomic load when collection is off, which is
                            // every run that is not a --profile run -- so the main table's numbers
                            // are the same with it here and without it.
                            MOSAIC_PERF_SCOPE("Bench composite (full)", common::Lane::Cpu);
                            r = render::composite(*doc, opts, render::Backend::Cpu);
                        }
                        const bool ok = r.ok && upload->full(r.image, why);
                        c.ms.push_back(w.ms());
                        if (!ok) {
                            c.skip = r.ok ? "canvas upload failed: " + why
                                          : "composite failed: " + r.error;
                            c.ms.clear();
                            break;
                        }
                    }
                    prof.finish(c);
                    cases.push_back(std::move(c));
                }
                {   // Region composite -> region upload. The brush/typing frame, end to end, and
                    // the one the item-13 gate's condition 1 is really about.
                    //
                    // Same ROI walk as `cpu region 256` above, so the composite half of this row
                    // IS that row and the difference between the two is the present half alone.
                    // The walk's fmod is over integers and lands on them, so the buffer is exactly
                    // one 256 px macrotile; were it ever made fractional, compositeRegion's
                    // floor/ceil would make it 257 square and the walk's span still leaves room
                    // for that (the floored offset is at most min(w,h) - 257).
                    Case c;
                    c.name = prefix + "cpu region 256 +upload";
                    c.shape = roiShape;
                    const auto frame = [&](std::uint32_t i, std::string& err) {
                        const double off = roiOffset(i, sh.w, sh.h);
                        const common::Rect rect{off, off, kRoi, kRoi};
                        render::CompositeResult r;
                        {
                            MOSAIC_PERF_SCOPE("Bench composite (region)", common::Lane::Cpu);
                            r = render::compositeRegion(*doc, rect, opts, render::Backend::Cpu);
                        }
                        if (!r.ok) {
                            err = "compositeRegion failed: " + r.error;
                            return false;
                        }
                        // compositeRegion's result sits at the floored rect origin -- one offset
                        // drives both axes here, exactly as the rect above does.
                        const auto x0 = static_cast<std::uint32_t>(std::floor(off));
                        const std::uint32_t y0 = x0;
                        if (!upload->region(r.image, x0, y0, err)) {
                            err = "canvas upload failed: " + err;
                            return false;
                        }
                        return true;
                    };
                    (void)frame(0, why);  // warm-up, and it seeds the mirror's allocation
                    const CaseProfile prof;
                    for (std::uint32_t i = 1; i <= iters; ++i) {
                        Watch w;
                        const bool ok = frame(i, why);
                        c.ms.push_back(w.ms());
                        if (!ok) {
                            c.skip = why;
                            c.ms.clear();
                            break;
                        }
                    }
                    prof.finish(c);
                    cases.push_back(std::move(c));
                }
            }
        }

        // ---- The resident lane -------------------------------------------------------------
        const auto skipAll = [&](const std::string& why) {
            cases.push_back(skipped(prefix + "gpu full", shape, why));
            cases.push_back(skipped(prefix + "gpu region 256", roiShape, why));
            cases.push_back(skipped(prefix + "gpu edit 256", roiShape, why));
            cases.push_back(skipped(prefix + "gpu edit 256 whole", roiShape, why));
        };

        std::string err;
        // Headless overload -> VulkanContext::shared(). In the app the lane is built on the
        // PRESENTING device instead (plan §7, "the presenting-device question"); the composite
        // and resolve work measured here is the same either way.
        const auto lane = render::TileCompositor::create(err);
        if (!lane) {
            skipAll("no usable Vulkan device: " + err);
            continue;
        }
        render::ResolveTarget target;
        if (!lane->createResolveTarget(sh.w, sh.h, target, err)) {
            skipAll("no resolve target: " + err);
            continue;
        }

        // Warm-up: build the atlas, upload every layer once, prove the lane accepts the document.
        lane->markAllDirty();
        const render::TileCompositeStatus warm = lane->composite(*doc, opts);
        if (!warm.ok) {
            std::string why = "refused: ";
            why += render::tileRefusalName(warm.refusal);
            if (!warm.error.empty()) why += " (" + warm.error + ")";
            skipAll(why);
            lane->destroyResolveTarget(target);
            continue;
        }
        // ⚠ Through resolveInto, ALWAYS -- see its comment. This first one is the full resolve a
        // fresh target legitimately needs; every later one must be incremental, and it only is
        // because the target's layout is carried forward.
        (void)resolveInto(*lane, target, err);

        {   // Everything dirty: the gesture-end / undo / tab-switch frame.
            Case c;
            c.name = prefix + "gpu full";
            c.shape = shape;
            const CaseProfile prof;
            for (std::uint32_t i = 0; i < iters; ++i) {
                lane->markAllDirty();
                Watch w;
                const bool ok = lane->composite(*doc, opts).ok && resolveInto(*lane, target, err);
                c.ms.push_back(w.ms());
                if (!ok) {
                    c.skip = "gpu composite failed: " + err;
                    c.ms.clear();
                    break;
                }
            }
            prof.finish(c);
            cases.push_back(std::move(c));
        }
        {   // One macrotile dirty with NO content change: the pure dirty-set path, and the number
            // that says whether residency is worth having. Layers are not re-uploaded here --
            // uploadBytes must be 0 on every iteration.
            Case c;
            c.name = prefix + "gpu region 256";
            c.shape = roiShape;
            const CaseProfile prof;
            for (std::uint32_t i = 0; i < iters; ++i) {
                const double off = roiOffset(i, sh.w, sh.h);
                lane->markDirty(common::Rect{off, off, kRoi, kRoi});
                Watch w;
                const bool ok = lane->composite(*doc, opts).ok && resolveInto(*lane, target, err);
                c.ms.push_back(w.ms());
                if (!ok) {
                    c.skip = "gpu composite failed: " + err;
                    c.ms.clear();
                    break;
                }
            }
            prof.finish(c);
            cases.push_back(std::move(c));
        }

        // A REAL edit: rewrite a 256x256 block of the top raster layer, bump its content revision,
        // and recomposite. This is the brush/typing case and the row the gate reads hardest.
        //
        // It is measured TWICE, because the difference between the two rows IS the S60-a
        // incremental-upload slice:
        //   `gpu edit 256`        markLayerDirty(layer, rect) -- the caller names the region, so
        //                         only the macrotiles it touched are uploaded and recomposited.
        //   `gpu edit 256 whole`  markLayerDirty(id) -- the pre-2026-07-29 behaviour, kept as the
        //                         standing baseline: the whole layer is re-uploaded and its whole
        //                         footprint recomposited. On a full-canvas layer that is the whole
        //                         canvas, which is what took this row to 18.804 ms at 3840x2160.
        auto* top = doc->root().childCount() == 0
                        ? nullptr
                        : doc->root().child(doc->root().childCount() - 1).as<core::RasterLayer>();
        // Rewrite the 256-square block at `off` and tell the lane. Layer-local == document space
        // here (the bench stack is full-canvas layers at the identity transform), but the rect
        // handed to markLayerDirty is LAYER-LOCAL by contract, which is what an app-side caller
        // has to compute for a transformed layer.
        const auto editBlock = [&](std::uint32_t i, bool byRegion) {
            const auto off = static_cast<std::uint32_t>(roiOffset(i, sh.w, sh.h));
            common::Image& img = top->image();
            const std::uint32_t x0 = std::min(off, img.width);
            const std::uint32_t y0 = std::min(off, img.height);
            const std::uint32_t x1 = std::min(img.width, off + 256u);
            const std::uint32_t y1 = std::min(img.height, off + 256u);
            for (std::uint32_t y = y0; y < y1; ++y)
                for (std::uint32_t x = x0; x < x1; ++x) {
                    std::uint8_t* px = &img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4];
                    px[0] = static_cast<std::uint8_t>(x + i);
                    px[1] = static_cast<std::uint8_t>(y + i);
                    px[2] = 128;
                    px[3] = 255;
                }
            top->invalidateContentBounds();  // this is what bumps contentRevision
            if (byRegion && x1 > x0 && y1 > y0)
                lane->markLayerDirty(*top, common::Rect{static_cast<double>(x0),
                                                        static_cast<double>(y0),
                                                        static_cast<double>(x1 - x0),
                                                        static_cast<double>(y1 - y0)});
            else
                lane->markLayerDirty(top->id());
        };
        struct EditVariant {
            const char* suffix;
            bool byRegion;
        };
        static constexpr EditVariant kEdits[] = {{"gpu edit 256", true},
                                                 {"gpu edit 256 whole", false}};
        for (const EditVariant& ev : kEdits) {
            Case c;
            c.name = prefix + ev.suffix;
            c.shape = roiShape;
            if (top == nullptr) {
                c.skip = "no raster layer";
                cases.push_back(std::move(c));
                continue;
            }
            // Warm-up outside the profile window: the FIRST region-marked edit still finds the
            // layer resident from the trio's warm composite, but the whole-layer variant drops it,
            // and an un-warmed reallocation would land in the first sample.
            editBlock(0, ev.byRegion);
            (void)lane->composite(*doc, opts);
            (void)resolveInto(*lane, target, err);
            const CaseProfile prof;
            for (std::uint32_t i = 0; i < iters; ++i) {
                editBlock(i + 1, ev.byRegion);
                Watch w;
                const bool ok = lane->composite(*doc, opts).ok && resolveInto(*lane, target, err);
                c.ms.push_back(w.ms());
                if (!ok) {
                    c.skip = "gpu composite failed: " + err;
                    c.ms.clear();
                    break;
                }
            }
            prof.finish(c);
            cases.push_back(std::move(c));
        }

        lane->destroyResolveTarget(target);
    }
    return cases;
}

// ---------------------------------------------------------------------------------------------
// Scenario: present-upload -- the present half ALONE, on both lanes
// ---------------------------------------------------------------------------------------------
//
// Every other scenario stops at the composite, so the app-side cost of getting the pixels to the
// screen has never been in any measured row. `tile-composite`'s `+upload` rows close that for the
// two frames the item-13 gate reads; this scenario publishes the present half AS ITS OWN NUMBER, at
// every canvas size, so it can be added to any CPU-only row in any other scenario -- `composite-
// full` at 1920x1080 plus `1920x1080 cpu upload full` is that frame's honest cost.
//
// The four rows per size are two pairs doing the SAME JOB:
//
//   `cpu upload full`  vs `gpu resolve full`   -- put a whole canvas into the canvas texture
//   `cpu upload 256`   vs `gpu resolve 256`    -- put one 256 px macrotile into it
//
// The CPU rows move the pixels across the bus (mirror patch, two host copies, staging write,
// copy-to-image, submit, fence); the GPU rows move NO host bytes at all -- resolve() reads the
// resident accumulator and writes the same texture on the device. That difference is the entire
// thesis of S60-a item 11, and this is the row pair that states it as a measurement rather than an
// argument.
//
// The composite that feeds each GPU resolve is deliberately OUTSIDE the timed region: `gpu full`
// and `gpu region 256` over in `tile-composite` are the rows that include it. Here the question is
// only "what does the present step cost on each lane".
std::vector<Case> runPresentUpload(std::uint32_t iters) {
    struct Shape {
        std::uint32_t w, h;
    };
    static constexpr Shape kShapes[] = {
        {512, 512}, {1920, 1080}, {3840, 2160}, {5000, 8000},
    };
    constexpr std::uint32_t kRoi = 256;  // one macrotile at the default k, as tile-composite uses
    std::vector<Case> cases;
    for (const Shape& sh : kShapes) {
        const std::string prefix = std::to_string(sh.w) + "x" + std::to_string(sh.h) + " ";
        const std::string size = std::to_string(sh.w) + "x" + std::to_string(sh.h);
        const std::string fullShape = size + ", whole canvas texture";
        const std::string roiShape = size + ", roi 256x256";
        // An INTEGER walk bounded by the smaller dimension: a dirty rect from a brush dab is
        // integral, and both lanes have to be handed the same rect for the pair to compare.
        const std::uint32_t span = std::max(1u, std::min(sh.w, sh.h) - kRoi);
        const auto roiOff = [span](std::uint32_t i) { return (i * 137u) % span; };

        // ---- The CPU lane's present half. Built and torn down before the resident lane exists,
        //      so the two never hold their (doc-sized) buffers at the same time.
        {
            std::string why;
            const auto upload = CanvasUpload::create(sh.w, sh.h, why);
            if (!upload) {
                cases.push_back(skipped(prefix + "cpu upload full", fullShape, why));
                cases.push_back(skipped(prefix + "cpu upload 256", roiShape, why));
            } else {
                // Deterministic pixels, not zeros: a staging write of a zero page can be
                // measurably cheaper than one of real data on a compressing memory subsystem.
                //
                // ONE source buffer, reused every iteration -- and that is faithful rather than
                // convenient. The app's source is `m_lastComposite`, the same allocation frame
                // after frame, and it is hot when the copies read it because the composite has
                // just written it. Alternating buffers to defeat the cache would measure a
                // colder read than the app ever performs.
                common::Image canvas(sh.w, sh.h);
                fillSynthetic(canvas, 0xB0A7C0DEull, /*opaque=*/true);
                common::Image tile(kRoi, kRoi);
                fillSynthetic(tile, 0x5EEDF00Dull, /*opaque=*/true);
                {
                    Case c;
                    c.name = prefix + "cpu upload full";
                    c.shape = fullShape;
                    (void)upload->full(canvas, why);  // out of UNDEFINED, outside the timed loop
                    const CaseProfile prof;
                    for (std::uint32_t i = 0; i < iters; ++i) {
                        Watch w;
                        const bool ok = upload->full(canvas, why);
                        c.ms.push_back(w.ms());
                        if (!ok) {
                            c.skip = "canvas upload failed: " + why;
                            c.ms.clear();
                            break;
                        }
                    }
                    prof.finish(c);
                    cases.push_back(std::move(c));
                }
                {
                    Case c;
                    c.name = prefix + "cpu upload 256";
                    c.shape = roiShape;
                    (void)upload->region(tile, roiOff(0), roiOff(0), why);  // seeds the mirror
                    const CaseProfile prof;
                    for (std::uint32_t i = 1; i <= iters; ++i) {
                        const std::uint32_t off = roiOff(i);
                        Watch w;
                        const bool ok = upload->region(tile, off, off, why);
                        c.ms.push_back(w.ms());
                        if (!ok) {
                            c.skip = "canvas upload failed: " + why;
                            c.ms.clear();
                            break;
                        }
                    }
                    prof.finish(c);
                    cases.push_back(std::move(c));
                }
            }
        }

        // ---- The resident lane's present half: resolve(), and nothing else.
        const auto doc = makeBenchDoc(sh.w, sh.h, 4);  // tile-composite's stack, so the two agree
        const render::CompositeOptions opts = displayOptions();
        const std::string gpuShape = shapeOf(sh.w, sh.h, doc->layerCount());
        const auto skipGpu = [&](const std::string& why) {
            cases.push_back(skipped(prefix + "gpu resolve full", gpuShape, why));
            cases.push_back(skipped(prefix + "gpu resolve 256", gpuShape, why));
        };
        std::string err;
        const auto lane = render::TileCompositor::create(err);
        if (!lane) {
            skipGpu("no usable Vulkan device: " + err);
            continue;
        }
        render::ResolveTarget target;
        if (!lane->createResolveTarget(sh.w, sh.h, target, err)) {
            skipGpu("no resolve target: " + err);
            continue;
        }
        lane->markAllDirty();
        const render::TileCompositeStatus warm = lane->composite(*doc, opts);
        if (!warm.ok) {
            std::string why = "refused: ";
            why += render::tileRefusalName(warm.refusal);
            if (!warm.error.empty()) why += " (" + warm.error + ")";
            skipGpu(why);
            lane->destroyResolveTarget(target);
            continue;
        }
        (void)resolveInto(*lane, target, err);  // the full resolve a fresh target owes

        {
            Case c;
            c.name = prefix + "gpu resolve full";
            c.shape = gpuShape;
            const CaseProfile prof;
            for (std::uint32_t i = 0; i < iters; ++i) {
                lane->markAllDirty();
                if (!lane->composite(*doc, opts).ok) {  // untimed: this row is the resolve alone
                    c.skip = "gpu composite failed";
                    c.ms.clear();
                    break;
                }
                Watch w;
                const bool ok = resolveInto(*lane, target, err);
                c.ms.push_back(w.ms());
                if (!ok) {
                    c.skip = "resolve failed: " + err;
                    c.ms.clear();
                    break;
                }
            }
            prof.finish(c);
            cases.push_back(std::move(c));
        }
        {
            Case c;
            c.name = prefix + "gpu resolve 256";
            c.shape = gpuShape;
            const CaseProfile prof;
            for (std::uint32_t i = 0; i < iters; ++i) {
                const auto off = static_cast<double>(roiOff(i));
                lane->markDirty(common::Rect{off, off, static_cast<double>(kRoi),
                                             static_cast<double>(kRoi)});
                if (!lane->composite(*doc, opts).ok) {
                    c.skip = "gpu composite failed";
                    c.ms.clear();
                    break;
                }
                Watch w;
                const bool ok = resolveInto(*lane, target, err);
                c.ms.push_back(w.ms());
                if (!ok) {
                    c.skip = "resolve failed: " + err;
                    c.ms.clear();
                    break;
                }
            }
            prof.finish(c);
            cases.push_back(std::move(c));
        }
        lane->destroyResolveTarget(target);
    }
    return cases;
}

// ---------------------------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------------------------

struct Scenario {
    const char* token;
    const char* summary;      // one line for the listing
    const char* sampleNote;   // what ONE sample in the table is
    std::uint32_t defaultIters;
    std::vector<Case> (*run)(std::uint32_t iters);
};

constexpr Scenario kScenarios[] = {
    {"composite-full", "Full-document composite at several canvas sizes (the core S60 number)",
     "one render::composite() of the whole document, CPU backend", 12, &runCompositeFull},
    {"composite-region", "Dirty-region composite of a small rect in a 4096x4096 document",
     "one render::compositeRegion() of the roi, CPU backend", 40, &runCompositeRegion},
    {"paint-stroke", "A brush stroke's dab stamping + per-frame region recomposites",
     "one stroke FRAME: extendTo + dab composite + region recomposite", 6, &runPaintStroke},
    {"move-fullcanvas",
     "Moving a full-canvas layer: the drag frames AND both bracketing stalls (PLAN 2, 5000x8000)",
     "one drag frame, or one gesture-start seed, or one gesture-end commit", 10,
     &runMoveFullcanvas},
    {"type-keystroke", "One text edit's recomposite -- the region path against the full one",
     "one keystroke: setBlock + refreshTextCaches + recomposite", 20, &runTypeKeystroke},
    {"blur-live", "A live Gaussian/lens blur adjustment scrub over a document",
     "one scrub frame: rewrite the radius, then full composite", 8, &runBlurLive},
    {"adjustment-live", "A live adjustment-layer parameter drag",
     "one drag frame: rewrite the parameter, then full composite", 12, &runAdjustmentLive},
    {"tile-composite",
     "The resident GPU lane vs the CPU walk -- the gate S60-a item 13 is withheld behind",
     "one frame: CPU composite (+/- its upload), or GPU composite + resolve", 12,
     &runTileComposite},
    {"present-upload",
     "The PRESENT half alone: the CPU lane's host upload against the resident lane's resolve()",
     "one present: stage + copy the pixels into the canvas texture, or one resolve()", 12,
     &runPresentUpload},
};

[[nodiscard]] const Scenario* findScenario(std::string_view token) {
    for (const Scenario& s : kScenarios)
        if (token == s.token) return &s;
    return nullptr;
}

}  // namespace

void printBenchScenarios() {
    std::printf("Benchmark scenarios (--bench <scenario>):\n");
    for (const Scenario& s : kScenarios)
        std::printf("  %-17s [%2u] %s\n", s.token, static_cast<unsigned>(s.defaultIters),
                    s.summary);
    std::printf("\n[N] is the scenario's default iteration count; --bench-iterations N overrides\n"
                "it, so a quick smoke run and a careful measurement are both one flag away.\n");
    std::fflush(stdout);
}

int runBench(std::string_view scenario, std::uint32_t iterations) {
    if (scenario.empty()) {
        std::cerr << "--bench requires a scenario.\n\n";
        printBenchScenarios();
        return 2;
    }
    const Scenario* s = findScenario(scenario);
    if (s == nullptr) {
        std::cerr << "Unknown benchmark scenario: " << scenario << "\n\n";
        printBenchScenarios();
        return 2;
    }
    const std::uint32_t iters = iterations > 0 ? iterations : s->defaultIters;
    const CNumericLocale cNumeric;  // every number below is data; see the class comment
    std::cout << "[bench] " << common::buildInfo() << '\n'
              << "[bench] scenario:   " << s->token << " -- " << s->summary << '\n'
              // No longer "backend: cpu" unconditionally -- `tile-composite` runs BOTH lanes and
              // labels its rows, so claiming a backend here would be a lie in one scenario.
              << "[bench] iterations: " << iters << " (deterministic synthetic documents)\n"
              << "[bench] one sample: " << s->sampleNote << '\n';
    std::cout.flush();
    const Watch wall;
    const std::vector<Case> cases = s->run(iters);
    const double total = wall.ms();
    printTable(cases);
    printProfileRows(cases);
    printHostRows(cases);
    std::printf("\n[bench] total wall time: %.1f s\n", total / 1000.0);
    std::fflush(stdout);
    return 0;
}

}  // namespace mosaic::app
