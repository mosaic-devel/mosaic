#include "render/histogram_gpu.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>

#include <vk_mem_alloc.h>

#include "common/profiler.hpp"  // MOSAIC_PERF_SCOPE: §10.5's per-frame budget wants this visible
#include "render/gpu_policy.hpp"
#include "render/vulkan_context.hpp"
#include <shaders/histogram_bins.comp.spv.hpp>

// The Vulkan compute lane of the Channels-tab histogram (histogram_gpu.hpp). The plumbing is the
// BlurGpu/TextureGpu VMA pattern -- persistent context/pipeline, grow-only buffers, mapped
// staging, one submit, fence wait -- around a single dispatch shape. What is specific to this lane
// is the CHUNKING, and it is worth reading before touching anything:
//
//   Vulkan 1.0 has no 64-bit atomics, so a bin is a uint32. A colour bin's unit is ALPHA, so its
//   worst case is 255 per pixel: one flat opaque image puts 255 * pixelCount into a single bin.
//   That overflows a uint32 at 16,843,010 pixels -- well inside the documents Mosaic opens (a
//   5000x8000 canvas is 40 Mpx). So the image is binned in CHUNKS of at most 2^24 pixels, each
//   into its own uint32 bin set, and the host sums the chunks in uint64.
//
//   255 * 2^24 = 4,278,190,080 < 2^32, with the static_assert below to keep it that way. The same
//   bound covers the shared sub-histogram in the shader (a workgroup can never see more than its
//   chunk) and, for free, Vulkan 1.0's 128 MiB maxStorageBufferRange: a 2^24-pixel chunk is 64 MiB,
//   so the per-chunk descriptor range fits the FLOOR device, not just a real one.
namespace mosaic::render {
namespace {

// The bin layout, matching shaders/histogram_bins.comp and ChannelHistogram's field order.
constexpr std::uint32_t kBands = 5;  // r, g, b, a, luma
constexpr std::uint32_t kBinsPerBand = 256;
constexpr std::uint32_t kBinWords = kBands * kBinsPerBand;  // 1280 uints, 5 KB

constexpr std::uint32_t kLocalSize = 64;  // the house convention's ceiling; floor limit is 128

// Dispatch shaping only -- the kernel is a grid-stride loop, so this cannot make it wrong, only
// slower or faster. ~256 pixels per invocation keeps the launch count sane on a 40 Mpx document
// while still filling a big part.
constexpr std::uint64_t kPixelsPerThread = 256;

// The chunk bound: see the file header. Exactly 2^24 so the offset arithmetic is a shift and the
// per-chunk byte size (64 MiB) is a multiple of every legal storage-buffer offset alignment.
constexpr std::uint64_t kChunkPixels = 1ull << 24;
static_assert(kChunkPixels * 255ull < (1ull << 32),
              "a uint32 colour bin must not wrap within one chunk -- the whole parity claim "
              "rests on integer addition being exact");

// Refuse absurd canvases; the CPU reference handles them (the BlurGpu policy, same number). 256 MB
// of RGBA8 is 64 Mpx -- an 8000x8000 document -- and exactly four chunks.
constexpr VkDeviceSize kMaxImageBytes = 256ull << 20;
constexpr std::uint32_t kMaxChunks =
    static_cast<std::uint32_t>((kMaxImageBytes / 4 + kChunkPixels - 1) / kChunkPixels);
constexpr std::uint32_t kMaxSets = kMaxChunks;  // one descriptor set per chunk, all up front

constexpr std::uint32_t kBindings = 2;                     // src, bins
constexpr std::uint32_t kSharedBytes = kBinWords * 4;      // the shader's `sub[]`

// std430 mirror of the shader's push block (keep in lockstep with histogram_bins.comp).
struct BinPush {
    std::int32_t pixels;
    std::int32_t binBase;
};
static_assert(sizeof(BinPush) == 8);

}  // namespace

struct HistogramGpu::Impl {
    std::shared_ptr<VulkanContext> ctx;   // the process-wide shared device (S60-alpha)
    VkCommandPool pool = VK_NULL_HANDLE;  // OURS: pools are externally synchronized
    VmaAllocator allocator = VK_NULL_HANDLE;

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kMaxSets> sets{};
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    struct Buf {
        VkBuffer buf = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        void* ptr = nullptr;  // mapped for Upload/Readback kinds, null for Device
        VkDeviceSize cap = 0;
    };
    // `src` is host-visible and read straight by the shader: every pixel is read exactly ONCE, so
    // staging it into device-local memory first would be a second full copy for no reuse (the
    // BlurGpu `raw` argument). `bins` is device-local because the atomics DO hammer it.
    Buf src, bins, binsHost;

    enum class Kind { Upload, Readback, Device };

    void destroyBuf(Buf& b) {
        if (b.buf != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, b.buf, b.alloc);
        b = {};
    }

    bool ensureBuf(Buf& b, VkDeviceSize bytes, VkBufferUsageFlags usage, Kind kind) {
        if (b.cap >= bytes && b.buf != VK_NULL_HANDLE) return true;
        // No submission is in flight here (every bin() fence-waits), so growth is safe.
        destroyBuf(b);
        const VkBufferCreateInfo bci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = std::max<VkDeviceSize>(bytes, 64),
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VmaAllocationCreateInfo aci{.usage = VMA_MEMORY_USAGE_AUTO};
        if (kind == Kind::Upload)
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
        else if (kind == Kind::Readback)
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(allocator, &bci, &aci, &b.buf, &b.alloc, &info) != VK_SUCCESS)
            return false;
        b.ptr = info.pMappedData;
        b.cap = bci.size;
        return true;
    }

    bool submitAndWait() {
        const VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              .commandBufferCount = 1,
                              .pCommandBuffers = &cmd};
        vkResetFences(ctx->device(), 1, &fence);
        if (ctx->submit(si, fence) != VK_SUCCESS) return false;
        return vkWaitForFences(ctx->device(), 1, &fence, VK_TRUE, 60'000'000'000ull) ==
               VK_SUCCESS;
    }

    ~Impl() {
        if (!ctx) return;
        const VkDevice dev = ctx->device();
        ctx->waitIdle();
        for (Buf* b : {&src, &bins, &binsHost}) destroyBuf(*b);
        if (fence) vkDestroyFence(dev, fence, nullptr);
        if (cmd) vkFreeCommandBuffers(dev, pool, 1, &cmd);
        if (descPool) vkDestroyDescriptorPool(dev, descPool, nullptr);
        if (pipe) vkDestroyPipeline(dev, pipe, nullptr);
        if (shader) vkDestroyShaderModule(dev, shader, nullptr);
        if (pipeLayout) vkDestroyPipelineLayout(dev, pipeLayout, nullptr);
        if (setLayout) vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
        if (pool) vkDestroyCommandPool(dev, pool, nullptr);
        if (allocator) vmaDestroyAllocator(allocator);
    }

    bool init(bool enableValidation, std::string& error) {
        ctx = VulkanContext::shared(enableValidation, error);
        if (!ctx) return false;
        // ASK, never assume (the gpu_caps.hpp house rule). Every question below is answered YES by
        // a bare-minimum conforming Vulkan 1.0 device -- this kernel was sized so it would be --
        // but a lane that skips the question is a lane that cannot report why it refused.
        const GpuCaps& caps = ctx->caps();
        if (!caps.fitsStorageBuffers(kBindings)) {
            error = "device allows only " +
                    std::to_string(caps.limits.maxPerStageDescriptorStorageBuffers) +
                    " storage buffers per stage; the histogram lane needs " +
                    std::to_string(kBindings);
            return false;
        }
        if (!caps.fitsPushConstants(sizeof(BinPush))) {
            error = "device allows only " + std::to_string(caps.limits.maxPushConstantsSize) +
                    " push-constant bytes; the histogram lane needs " +
                    std::to_string(sizeof(BinPush));
            return false;
        }
        if (caps.limits.maxComputeSharedMemorySize < kSharedBytes) {
            error = "device offers only " +
                    std::to_string(caps.limits.maxComputeSharedMemorySize) +
                    " bytes of compute shared memory; the histogram sub-histogram needs " +
                    std::to_string(kSharedBytes);
            return false;
        }
        if (caps.limits.maxComputeWorkGroupInvocations < kLocalSize ||
            caps.limits.maxComputeWorkGroupSize[0] < kLocalSize) {
            error = "device caps a compute workgroup below the histogram lane's " +
                    std::to_string(kLocalSize) + " invocations";
            return false;
        }
        pool = ctx->createCommandPool(error);
        if (pool == VK_NULL_HANDLE) return false;
        const VkDevice dev = ctx->device();

        const VmaAllocatorCreateInfo aci{
            .physicalDevice = ctx->physicalDevice(),
            .device = dev,
            .instance = ctx->instance(),
            // min(instance, device) -- never a hard-coded version, or VMA reaches for entry points
            // a 1.0 device lacks (the BlurGpu note).
            .vulkanApiVersion = caps.apiVersion,
        };
        if (vmaCreateAllocator(&aci, &allocator) != VK_SUCCESS) {
            error = "vmaCreateAllocator failed";
            return false;
        }

        VkDescriptorSetLayoutBinding bindings[kBindings]{};
        for (std::uint32_t i = 0; i < kBindings; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        const VkDescriptorSetLayoutCreateInfo slci{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = kBindings,
            .pBindings = bindings,
        };
        if (vkCreateDescriptorSetLayout(dev, &slci, nullptr, &setLayout) != VK_SUCCESS) {
            error = "descriptor set layout creation failed";
            return false;
        }
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BinPush)};
        const VkPipelineLayoutCreateInfo plci{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &setLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        };
        if (vkCreatePipelineLayout(dev, &plci, nullptr, &pipeLayout) != VK_SUCCESS) {
            error = "pipeline layout creation failed";
            return false;
        }
        const VkShaderModuleCreateInfo smci{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = shaders::histogram_bins_comp_size,
            .pCode = shaders::histogram_bins_comp,
        };
        if (vkCreateShaderModule(dev, &smci, nullptr, &shader) != VK_SUCCESS) {
            error = "shader module creation failed";
            return false;
        }
        const VkComputePipelineCreateInfo cpci{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = shader,
                      .pName = "main"},
            .layout = pipeLayout,
        };
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe) !=
            VK_SUCCESS) {
            error = "compute pipeline creation failed";
            return false;
        }
        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            kMaxSets * kBindings};
        const VkDescriptorPoolCreateInfo dpci{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kMaxSets,
            .poolSizeCount = 1,
            .pPoolSizes = &poolSize,
        };
        if (vkCreateDescriptorPool(dev, &dpci, nullptr, &descPool) != VK_SUCCESS) {
            error = "descriptor pool creation failed";
            return false;
        }
        std::array<VkDescriptorSetLayout, kMaxSets> layouts;
        layouts.fill(setLayout);
        const VkDescriptorSetAllocateInfo dsai{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = descPool,
            .descriptorSetCount = kMaxSets,
            .pSetLayouts = layouts.data(),
        };
        if (vkAllocateDescriptorSets(dev, &dsai, sets.data()) != VK_SUCCESS) {
            error = "descriptor set allocation failed";
            return false;
        }
        const VkCommandBufferAllocateInfo cbai{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (vkAllocateCommandBuffers(dev, &cbai, &cmd) != VK_SUCCESS) {
            error = "command buffer allocation failed";
            return false;
        }
        const VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS) {
            error = "fence creation failed";
            return false;
        }
        // The bin buffers are TINY and fixed (kMaxChunks * 5 KB = 20 KB), so they are allocated
        // once here rather than grown per call -- there is nothing to grow them by.
        const VkDeviceSize binBytes = static_cast<VkDeviceSize>(kMaxSets) * kBinWords * 4;
        if (!ensureBuf(bins, binBytes,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       Kind::Device) ||
            !ensureBuf(binsHost, binBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Kind::Readback)) {
            error = "bin buffer allocation failed";
            return false;
        }
        return true;
    }
};

HistogramGpu::HistogramGpu() : m_impl(std::make_unique<Impl>()) {}
HistogramGpu::~HistogramGpu() = default;

std::unique_ptr<HistogramGpu> HistogramGpu::create(bool enableValidation, std::string& error) {
    // CPU-only mode (render/gpu_policy.hpp, S60-b item 14): decline BEFORE any Vulkan object
    // exists. The caller's fallback is ui::computeHistogram -- the very reference the parity test
    // holds this lane to -- so a refusal here moves the work, never the bins.
    if (!computeLaneAllowed("histogram", error)) return nullptr;
    auto gpu = std::unique_ptr<HistogramGpu>(new HistogramGpu());
    if (!gpu->m_impl->init(enableValidation, error)) return nullptr;
    return gpu;
}

std::string HistogramGpu::deviceName() const {
    return m_impl->ctx ? m_impl->ctx->deviceName() : std::string{};
}

bool HistogramGpu::bin(const common::Image& img, HistogramBins& out) {
    MOSAIC_PERF_SCOPE("Histogram bins (GPU)", common::Lane::Gpu);
    Impl& im = *m_impl;
    if (!im.ctx || img.empty()) return false;
    const std::uint64_t pixels = img.pixelCount();
    // computeHistogram walks `p + 3 < rgba.size()`, i.e. it tolerates a short buffer by binning
    // fewer pixels. This lane refuses instead: a malformed image is rare, and reading past a
    // storage buffer's bound range is not a thing to be relaxed about.
    if (img.rgba.size() < pixels * 4) return false;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(pixels) * 4;
    if (bytes > kMaxImageBytes) return false;

    const std::uint32_t chunks =
        static_cast<std::uint32_t>((pixels + kChunkPixels - 1) / kChunkPixels);
    if (chunks == 0 || chunks > kMaxSets) return false;  // unreachable under kMaxImageBytes
    const VkDeviceSize chunkBytes = static_cast<VkDeviceSize>(kChunkPixels) * 4;
    // The DEVICE's limit, not ours: Vulkan 1.0 guarantees only 128 MiB of maxStorageBufferRange,
    // which is why the source is bound one chunk at a time rather than whole.
    if (!im.ctx->caps().fitsStorageBufferRange(std::min<VkDeviceSize>(bytes, chunkBytes)))
        return false;
    // A descriptor's buffer offset must be a multiple of minStorageBufferOffsetAlignment. A
    // conforming device may not report more than 256 and a chunk is 2^26 bytes, so this holds
    // everywhere -- checked rather than assumed, because it is the one silent-corruption failure
    // in the chunk path.
    const VkDeviceSize offsetAlign =
        std::max<VkDeviceSize>(im.ctx->caps().limits.minStorageBufferOffsetAlignment, 1);
    if (chunkBytes % offsetAlign != 0) return false;

    {
        // The one host-side cost left, and the reason the honest destination for this lane is the
        // resident accumulator rather than a host image: while the source is a CPU buffer, the
        // pixels are copied once into mapped memory before the device sees them.
        MOSAIC_PERF_SCOPE("Histogram bins (host upload)", common::Lane::Cpu);
        if (!im.ensureBuf(im.src, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                          Impl::Kind::Upload))
            return false;
        std::memcpy(im.src.ptr, img.rgba.data(), static_cast<std::size_t>(bytes));
        vmaFlushAllocation(im.allocator, im.src.alloc, 0, VK_WHOLE_SIZE);
    }

    // ---- descriptors: one set per chunk, the source bound at the chunk's own offset ------------
    const VkDevice dev = im.ctx->device();
    const VkDeviceSize binBytes = static_cast<VkDeviceSize>(chunks) * kBinWords * 4;
    std::array<VkDescriptorBufferInfo, kMaxSets * kBindings> infos{};
    std::array<VkWriteDescriptorSet, kMaxSets * kBindings> writes{};
    std::uint32_t writeCount = 0;
    for (std::uint32_t c = 0; c < chunks; ++c) {
        const VkDeviceSize offset = static_cast<VkDeviceSize>(c) * chunkBytes;
        const VkDescriptorBufferInfo perBinding[kBindings] = {
            {im.src.buf, offset, std::min<VkDeviceSize>(chunkBytes, bytes - offset)},
            {im.bins.buf, 0, binBytes},
        };
        for (std::uint32_t b = 0; b < kBindings; ++b) {
            infos[writeCount] = perBinding[b];
            writes[writeCount] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = im.sets[c],
                                  .dstBinding = b,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .pBufferInfo = &infos[writeCount]};
            ++writeCount;
        }
    }
    vkUpdateDescriptorSets(dev, writeCount, writes.data(), 0, nullptr);

    // ---- record: zero the bins, one dispatch per chunk, copy them back ------------------------
    vkResetCommandBuffer(im.cmd, 0);
    const VkCommandBufferBeginInfo cbi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vkBeginCommandBuffer(im.cmd, &cbi) != VK_SUCCESS) return false;
    vkCmdFillBuffer(im.cmd, im.bins.buf, 0, binBytes, 0);
    const VkMemoryBarrier toShader{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                   .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                                    VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(im.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &toShader, 0, nullptr, 0,
                         nullptr);
    vkCmdBindPipeline(im.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, im.pipe);
    const std::uint64_t perGroup = static_cast<std::uint64_t>(kLocalSize) * kPixelsPerThread;
    for (std::uint32_t c = 0; c < chunks; ++c) {
        const std::uint64_t first = static_cast<std::uint64_t>(c) * kChunkPixels;
        const std::uint64_t count = std::min<std::uint64_t>(kChunkPixels, pixels - first);
        // No barrier BETWEEN chunk dispatches: each reads a disjoint slice of `src` and writes a
        // disjoint slice of `bins`, so letting them overlap is both correct and the point.
        vkCmdBindDescriptorSets(im.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, im.pipeLayout, 0, 1,
                                &im.sets[c], 0, nullptr);
        const BinPush p{static_cast<std::int32_t>(count),
                        static_cast<std::int32_t>(c * kBinWords)};
        vkCmdPushConstants(im.cmd, im.pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        const std::uint64_t groups = std::clamp<std::uint64_t>(
            (count + perGroup - 1) / perGroup, 1, im.ctx->caps().maxWorkgroupsPerAxis());
        vkCmdDispatch(im.cmd, static_cast<std::uint32_t>(groups), 1, 1);
    }
    const VkMemoryBarrier toCopy{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(im.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toCopy, 0, nullptr, 0, nullptr);
    const VkBufferCopy copyRegion{0, 0, binBytes};
    vkCmdCopyBuffer(im.cmd, im.bins.buf, im.binsHost.buf, 1, &copyRegion);
    const VkMemoryBarrier toHost{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(im.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                         &toHost, 0, nullptr, 0, nullptr);
    if (vkEndCommandBuffer(im.cmd) != VK_SUCCESS) return false;
    if (!im.submitAndWait()) return false;

    // ---- sum the chunks in uint64 (`out` is untouched on every failure path above) -------------
    vmaInvalidateAllocation(im.allocator, im.binsHost.alloc, 0, VK_WHOLE_SIZE);
    std::array<std::uint32_t, kMaxSets * kBinWords> raw{};
    std::memcpy(raw.data(), im.binsHost.ptr, static_cast<std::size_t>(binBytes));
    out.clear();
    const std::array<std::array<std::uint64_t, 256>*, kBands> band{&out.r, &out.g, &out.b, &out.a,
                                                                   &out.luma};
    for (std::uint32_t c = 0; c < chunks; ++c)
        for (std::uint32_t k = 0; k < kBands; ++k)
            for (std::uint32_t v = 0; v < kBinsPerBand; ++v)
                (*band[k])[v] += raw[c * kBinWords + k * kBinsPerBand + v];
    return true;
}

}  // namespace mosaic::render
