#include "render/blur_gpu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include <vk_mem_alloc.h>

#include "core/adjustments.hpp"
#include "render/blur.hpp"
#include "render/gpu_policy.hpp"
#include "render/vulkan_context.hpp"
#include <shaders/blur_convert.comp.spv.hpp>
#include <shaders/blur_dof.comp.spv.hpp>
#include <shaders/blur_lens.comp.spv.hpp>
#include <shaders/blur_separable.comp.spv.hpp>

// The Vulkan compute lane of the S33 blur adjustments (blur_gpu.hpp). Two halves live here:
//
//   1. The COOK -- everything the CPU kernels compute once per call reproduced host-side:
//      aperture taps by calling the SAME public fx::makeApertureKernel the CPU lane calls, and
//      the Gaussian/bilateral weight tables + the sRGB decode table by TRANSCRIPTION of
//      blur.cpp's file-local builders (same std::exp/std::pow on the same values, so the
//      uploaded tables match the CPU lane's bit-for-bit).
//      ⚠ The transcribed builders may not drift from blur.cpp's: the parity tests
//      (tests/test_blur_gpu.cpp) hold this copy to the CPU lane, so a retune over there fails
//      parity HERE, loudly, on any machine with a device.
//
//   2. The PLUMBING -- the TextureGpu VMA pattern (persistent context/pipelines, grow-only
//      buffers, mapped staging) driving a short per-call chain of compute dispatches: format
//      converts, separable passes and gathers ping-ponging through device-local working
//      buffers, one submit, fence wait, readback. Working buffers stay DEVICE-LOCAL -- the
//      passes hammer them per tap, and host-visible memory would put every one of those reads
//      on the PCIe bus (the extrude-lane lesson).
namespace mosaic::render {
namespace {

// The per-working-buffer ceiling, below which this lane declines and the CPU reference serves.
//
// ⚠ THIS WAS A FLAT 256 MiB (16.7 Mpx of float RGBA, ~a 5.8k x 2.9k canvas), and that constant is
// why a 39.8 MP document's Gaussian Blur adjustment cost 3.4 SECONDS of CPU per composite while a
// perfectly capable device sat idle: the working buffer is 608 MiB, the lane refused on size, and
// the golden CPU path served. A FIXED cap refuses exactly the documents whose blurs cost the most
// -- the ceiling was pointed the wrong way round.
//
// Derive it from the device instead. `buffers` is how many image-sized working buffers the kind
// needs -- raw + readback + work0 + work1, and for DoF four pyramid levels on top -- and the lane
// may spend up to a THIRD of device-local memory on that working set. Not more: it shares the
// device with the present path, the extrude lane and the tile compositor, so it may not simply
// take the heap.
//
// The old constant stays as the FLOOR, so no device ends up with a smaller ceiling than it had,
// and a device that reports no local heap behaves exactly as before. Every allocation past this
// still falls back cleanly -- ensureBuf failure returns false and the CPU lane serves -- so the
// cap is a policy, not a safety mechanism.
constexpr VkDeviceSize kMinImageBytes = 256ull << 20;

// Sum of the DEVICE_LOCAL heaps ("how much VRAM"). gpu_caps records this on DeviceInfo, which
// VulkanContext does not carry, so ask the physical device directly -- the same loop, one call.
[[nodiscard]] VkDeviceSize deviceLocalBytes(VkPhysicalDevice pd) {
    if (pd == VK_NULL_HANDLE) return 0;
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mem);
    VkDeviceSize total = 0;
    for (std::uint32_t i = 0; i < mem.memoryHeapCount; ++i)
        if ((mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
            total += mem.memoryHeaps[i].size;
    return total;
}

[[nodiscard]] VkDeviceSize maxImageBytes(VkPhysicalDevice pd, int buffers) {
    const VkDeviceSize vram = deviceLocalBytes(pd);
    if (vram == 0 || buffers <= 0)
        return kMinImageBytes;
    return std::max(kMinImageBytes, (vram / 3) / static_cast<VkDeviceSize>(buffers));
}

// The longest dispatch chain is DoF-soft: 1 convert + 4 x (2 separable + 1 convert) + 1
// interpolation = 14 steps; one descriptor set each.
constexpr std::uint32_t kMaxSets = 16;
constexpr std::uint32_t kBindings = 7;

// ---------------------------------------------------------------------------------------------
// Host cook -- transcribed from blur.cpp's file-local builders (see the file header note).
// ---------------------------------------------------------------------------------------------

// blur.cpp gaussianHalfKernel: normalised half-kernel [0..r], r = ceil(3*sigma). Same float
// std::exp on the same values, so the table matches the CPU lane's bit-for-bit.
[[nodiscard]] std::vector<float> gaussianHalfKernel(float sigma) {
    const int r = std::max(1, static_cast<int>(std::ceil(3.0f * sigma)));
    std::vector<float> kernel(static_cast<std::size_t>(r) + 1);
    const float inv2s2 = 1.0f / (2.0f * sigma * sigma);
    float sum = 0.0f;
    for (int k = 0; k <= r; ++k) {
        kernel[k] = std::exp(-static_cast<float>(k) * static_cast<float>(k) * inv2s2);
        sum += (k == 0 ? kernel[k] : 2.0f * kernel[k]);
    }
    for (float& k : kernel) k /= sum;
    return kernel;
}

// blur.cpp srgbDecodeTable: the 1024-entry IEC 61966-2-1 decode table, built in double and
// stored in float, uploaded verbatim so blur_convert.comp's interpolated decode walks the CPU
// lane's exact values.
[[nodiscard]] std::array<float, 1024> srgbDecodeTableData() {
    std::array<float, 1024> t{};
    for (int i = 0; i < 1024; ++i) {
        const double e = static_cast<double>(i) / 1023.0;
        t[i] = static_cast<float>(e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4));
    }
    return t;
}

// ---------------------------------------------------------------------------------------------
// std430 mirrors of the shader push-constant blocks (keep in lockstep with shaders/blur_*.comp).
// ---------------------------------------------------------------------------------------------

struct ConvertPush {
    std::int32_t w, h, mode, boostOn;
    float gain, thr;
};
static_assert(sizeof(ConvertPush) == 24);

struct SeparablePush {
    std::int32_t w, h, r, dir, mode, kernOff;
    float rangeCoeff;
};
static_assert(sizeof(SeparablePush) == 28);

struct LensPush {
    std::int32_t w, h, tapCount, tapOff;
};
static_assert(sizeof(LensPush) == 16);

struct DofPush {
    std::int32_t w, h;
    float cx, cy, nx, ny, band, feather, maxRadius;
};
static_assert(sizeof(DofPush) == 36);

// blur_lens.comp's Tap (std430 stride 16).
struct TapGpu {
    std::int32_t ox, oy;
    float w, pad;
};
static_assert(sizeof(TapGpu) == 16);

constexpr std::uint32_t kPushCapacity = 64;  // the pipeline layout's push range

}  // namespace

struct BlurGpu::Impl {
    std::shared_ptr<VulkanContext> ctx;  // the process-wide shared device (S60-alpha)
    VkCommandPool pool = VK_NULL_HANDLE; // OURS: pools are externally synchronized
    VmaAllocator allocator = VK_NULL_HANDLE;

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkShaderModule convertShader = VK_NULL_HANDLE;
    VkShaderModule separableShader = VK_NULL_HANDLE;
    VkShaderModule lensShader = VK_NULL_HANDLE;
    VkShaderModule dofShader = VK_NULL_HANDLE;
    VkPipeline convertPipe = VK_NULL_HANDLE;
    VkPipeline separablePipe = VK_NULL_HANDLE;
    VkPipeline lensPipe = VK_NULL_HANDLE;
    VkPipeline dofPipe = VK_NULL_HANDLE;
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
    // Host-visible: the uploaded image (read once per pixel by converts and the DoF
    // interpolation -- never by a gather), the readback staging, the weight/tap/LUT tables,
    // and a tiny dummy for unused bindings. Device-local: the ping-pong working pair and the
    // four DoF levels (gather sources).
    Buf raw, readback, kern, taps, lut, dummy;
    Buf work0, work1;
    std::array<Buf, 4> levels;

    enum class Kind { Upload, Readback, Device };

    void destroyBuf(Buf& b) {
        if (b.buf != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, b.buf, b.alloc);
        b = {};
    }

    bool ensureBuf(Buf& b, VkDeviceSize bytes, VkBufferUsageFlags usage, Kind kind) {
        if (b.cap >= bytes && b.buf != VK_NULL_HANDLE) return true;
        // No submission is in flight here (every render fence-waits), so growth is safe.
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

    bool uploadBuf(Buf& b, const void* data, VkDeviceSize bytes) {
        if (!ensureBuf(b, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Kind::Upload)) return false;
        if (bytes > 0 && data != nullptr) {
            std::memcpy(b.ptr, data, static_cast<std::size_t>(bytes));
            vmaFlushAllocation(allocator, b.alloc, 0, VK_WHOLE_SIZE);
        }
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
        for (Buf* b : {&raw, &readback, &kern, &taps, &lut, &dummy, &work0, &work1, &levels[0],
                       &levels[1], &levels[2], &levels[3]})
            destroyBuf(*b);
        if (fence) vkDestroyFence(dev, fence, nullptr);
        if (cmd) vkFreeCommandBuffers(dev, pool, 1, &cmd);
        if (descPool) vkDestroyDescriptorPool(dev, descPool, nullptr);
        for (VkPipeline p : {convertPipe, separablePipe, lensPipe, dofPipe})
            if (p) vkDestroyPipeline(dev, p, nullptr);
        for (VkShaderModule s : {convertShader, separableShader, lensShader, dofShader})
            if (s) vkDestroyShaderModule(dev, s, nullptr);
        if (pipeLayout) vkDestroyPipelineLayout(dev, pipeLayout, nullptr);
        if (setLayout) vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
        if (pool) vkDestroyCommandPool(dev, pool, nullptr);
        if (allocator) vmaDestroyAllocator(allocator);
    }

    bool init(bool enableValidation, std::string& error) {
        ctx = VulkanContext::shared(enableValidation, error);
        if (!ctx) return false;
        pool = ctx->createCommandPool(error);
        if (pool == VK_NULL_HANDLE) return false;
        // Vulkan 1.0 guarantees only FOUR storage buffers per stage; this lane binds kBindings.
        // Ask rather than assume -- a device that cannot host the layout refuses the lane and
        // the CPU reference kernels serve (S60-alpha).
        if (!ctx->caps().fitsStorageBuffers(kBindings)) {
            error = "device allows only " +
                    std::to_string(ctx->caps().limits.maxPerStageDescriptorStorageBuffers) +
                    " storage buffers per stage; this lane needs " + std::to_string(kBindings);
            return false;
        }
        const VkDevice dev = ctx->device();

        const VmaAllocatorCreateInfo aci{
            .physicalDevice = ctx->physicalDevice(),
            .device = dev,
            .instance = ctx->instance(),
            // The version VMA may use must not exceed what the instance was created with NOR
            // what the device supports -- caps().apiVersion is exactly min(instance, device).
            // A hard-coded 1.2 here would have VMA reach for entry points a 1.0 device lacks.
            .vulkanApiVersion = ctx->caps().apiVersion,
        };
        if (vmaCreateAllocator(&aci, &allocator) != VK_SUCCESS) {
            error = "vmaCreateAllocator failed";
            return false;
        }

        // One layout for all four pipelines: 0 src, 1 dst, 2 aux (weights / taps / LUT),
        // 3..6 the DoF levels. Unused bindings get the dummy buffer.
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
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, kPushCapacity};
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
        const auto makeShader = [&](const std::uint32_t* code, std::size_t size,
                                    VkShaderModule& mod) {
            const VkShaderModuleCreateInfo smci{
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = size,
                .pCode = code,
            };
            return vkCreateShaderModule(dev, &smci, nullptr, &mod) == VK_SUCCESS;
        };
        if (!makeShader(shaders::blur_convert_comp, shaders::blur_convert_comp_size,
                        convertShader) ||
            !makeShader(shaders::blur_separable_comp, shaders::blur_separable_comp_size,
                        separableShader) ||
            !makeShader(shaders::blur_lens_comp, shaders::blur_lens_comp_size, lensShader) ||
            !makeShader(shaders::blur_dof_comp, shaders::blur_dof_comp_size, dofShader)) {
            error = "shader module creation failed";
            return false;
        }
        const auto makePipeline = [&](VkShaderModule mod, VkPipeline& pipe) {
            const VkComputePipelineCreateInfo cpci{
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                          .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                          .module = mod,
                          .pName = "main"},
                .layout = pipeLayout,
            };
            return vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe) ==
                   VK_SUCCESS;
        };
        if (!makePipeline(convertShader, convertPipe) ||
            !makePipeline(separableShader, separablePipe) ||
            !makePipeline(lensShader, lensPipe) || !makePipeline(dofShader, dofPipe)) {
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
        // The never-read placeholder for unused bindings, and the sRGB decode table (4 KB,
        // read by every lens/DoF-iris pre-pass): both uploaded once for the object's lifetime.
        const std::array<float, 1024> table = srgbDecodeTableData();
        std::array<float, 16> zeros{};
        if (!uploadBuf(dummy, zeros.data(), zeros.size() * sizeof(float)) ||
            !uploadBuf(lut, table.data(), table.size() * sizeof(float))) {
            error = "static table upload failed";
            return false;
        }
        return true;
    }
};

BlurGpu::BlurGpu() : m_impl(std::make_unique<Impl>()) {}
BlurGpu::~BlurGpu() = default;

std::unique_ptr<BlurGpu> BlurGpu::create(bool enableValidation, std::string& error) {
    // CPU-only mode (render/gpu_policy.hpp, S60-b item 14): decline BEFORE any Vulkan object
    // exists. The caller's fallback is blur.cpp's kernels -- the very reference the parity tests
    // hold this lane to -- so a refusal here moves the work, never the pixels.
    if (!computeLaneAllowed("blur", error)) return nullptr;
    auto gpu = std::unique_ptr<BlurGpu>(new BlurGpu());
    if (!gpu->m_impl->init(enableValidation, error)) return nullptr;
    return gpu;
}

std::string BlurGpu::deviceName() const {
    return m_impl->ctx ? m_impl->ctx->deviceName() : std::string{};
}

bool BlurGpu::render(common::ImageF& img, const BlurOp& op) {
    using enum core::AdjustmentKind;
    Impl& im = *m_impl;
    if (!im.ctx || img.empty()) return false;
    const bool isGauss = op.kind == GaussianBlur;
    const bool isSurface = op.kind == SurfaceBlur;
    const bool isLens = op.kind == LensBlur;
    const bool isDof = op.kind == DofBlur;
    // Box / Motion / Radial (and anything newer) stay on the CPU lane (blur_gpu.hpp).
    if (!isGauss && !isSurface && !isLens && !isDof) return false;
    if (op.size <= 0.0f) return false;  // identity params never reach the seam; be safe

    const std::int32_t w = static_cast<std::int32_t>(img.width);
    const std::int32_t h = static_cast<std::int32_t>(img.height);
    const VkDeviceSize bytes =
        static_cast<VkDeviceSize>(img.pixelCount()) * 4 * sizeof(float);
    // Two caps, and they mean different things: maxImageBytes is OUR policy (how much of the
    // device this lane may spend), fitsStorageBufferRange is the DEVICE's own limit (Vulkan 1.0
    // guarantees only 128 MiB). Both must hold. (S60-alpha; the policy half derived since S60.)
    // DoF stacks a four-level pyramid on the four buffers every kind ensures.
    const int workingBuffers = isDof ? 8 : 4;
    if (bytes > maxImageBytes(im.ctx->physicalDevice(), workingBuffers) ||
        !im.ctx->caps().fitsStorageBufferRange(bytes))
        return false;

    // ---- buffers (grow-only; handles are stable once ensured) -------------------------------
    constexpr VkBufferUsageFlags kWork =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!im.ensureBuf(im.raw, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Impl::Kind::Upload) ||
        !im.ensureBuf(im.readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      Impl::Kind::Readback) ||
        !im.ensureBuf(im.work0, bytes, kWork, Impl::Kind::Device) ||
        !im.ensureBuf(im.work1, bytes, kWork, Impl::Kind::Device))
        return false;
    if (isDof) {
        for (Impl::Buf& lv : im.levels)
            if (!im.ensureBuf(lv, bytes, kWork, Impl::Kind::Device)) return false;
    }

    // ---- plan the dispatch chain + cook the tables ------------------------------------------
    // The aux binding is SYMBOLIC in the plan: the kern/taps buffers are created (or regrown)
    // by the upload phase below, AFTER planning, so resolving their handles here would bind a
    // null or stale buffer (the first-run/regrowth bug the parity tests caught on RADV).
    enum class Aux { Dummy, Kern, Taps, Lut };
    struct Step {
        VkPipeline pipe = VK_NULL_HANDLE;
        VkBuffer src = VK_NULL_HANDLE, dst = VK_NULL_HANDLE;
        Aux aux = Aux::Dummy;
        std::array<VkBuffer, 4> lv{};  // VK_NULL_HANDLE -> dummy
        std::array<std::byte, kPushCapacity> push{};
        std::uint32_t pushBytes = 0;
    };
    std::vector<Step> steps;
    steps.reserve(kMaxSets);
    const auto addStep = [&](VkPipeline pipe, VkBuffer src, VkBuffer dst, Aux aux,
                             const void* pushData, std::uint32_t pushBytes,
                             std::array<VkBuffer, 4> lv = {}) {
        Step s;
        s.pipe = pipe;
        s.src = src;
        s.dst = dst;
        s.aux = aux;
        s.lv = lv;
        std::memcpy(s.push.data(), pushData, pushBytes);
        s.pushBytes = pushBytes;
        steps.push_back(s);
    };
    const auto addConvert = [&](VkBuffer src, VkBuffer dst, std::int32_t mode,
                                std::int32_t boostOn = 0, float gain = 1.0f,
                                float thr = 0.0f) {
        const ConvertPush p{w, h, mode, boostOn, gain, thr};
        addStep(im.convertPipe, src, dst, Aux::Lut, &p, sizeof(p));
    };
    const auto addSeparable = [&](VkBuffer src, VkBuffer dst, std::int32_t r, std::int32_t dir,
                                  std::int32_t mode, std::int32_t kernOff, float rangeCoeff) {
        const SeparablePush p{w, h, r, dir, mode, kernOff, rangeCoeff};
        addStep(im.separablePipe, src, dst, Aux::Kern, &p, sizeof(p));
    };
    const auto addLens = [&](VkBuffer src, VkBuffer dst, std::int32_t tapCount,
                             std::int32_t tapOff) {
        const LensPush p{w, h, tapCount, tapOff};
        addStep(im.lensPipe, src, dst, Aux::Taps, &p, sizeof(p));
    };

    std::vector<float> kernData;
    std::vector<TapGpu> tapData;
    const auto appendTaps = [&](const fx::ApertureKernel& k) {
        const std::int32_t off = static_cast<std::int32_t>(tapData.size());
        for (std::size_t i = 0; i < k.weight.size(); ++i)
            tapData.push_back({static_cast<std::int32_t>(k.offX[i]),
                               static_cast<std::int32_t>(k.offY[i]), k.weight[i], 0.0f});
        return off;
    };

    if (isGauss || isSurface) {
        // premul -> H pass -> V pass -> unpremul (gaussianBlurImage / surfaceBlurImage).
        std::int32_t r = 0;
        std::int32_t mode = 0;
        float rangeCoeff = 0.0f;
        if (isGauss) {
            kernData = gaussianHalfKernel(op.size);  // sigma = op.size (the seam contract)
            r = static_cast<std::int32_t>(kernData.size()) - 1;
        } else {
            // surfaceBlurImage's tables, transcribed: the unnormalised spatial taper (the
            // per-pixel range normalisation absorbs any constant factor) and the range
            // coefficient with its zero-threshold floor.
            r = std::max(1, static_cast<int>(std::ceil(op.size)));
            const float sigmaS = 0.5f * op.size;
            const float spatialCoeff = -1.0f / (2.0f * sigmaS * sigmaS);
            kernData.resize(static_cast<std::size_t>(r) + 1);
            for (int k = 0; k <= r; ++k)
                kernData[k] =
                    std::exp(static_cast<float>(k) * static_cast<float>(k) * spatialCoeff);
            const float sigmaR = std::max(op.threshold, 1e-4f);
            rangeCoeff = -1.0f / (2.0f * sigmaR * sigmaR);
            mode = 1;
        }
        addConvert(im.raw.buf, im.work0.buf, 0);
        addSeparable(im.work0.buf, im.work1.buf, r, 0, mode, 0, rangeCoeff);
        addSeparable(im.work1.buf, im.work0.buf, r, 1, mode, 0, rangeCoeff);
        addConvert(im.work0.buf, im.work1.buf, 1);
    } else if (isLens) {
        // linear pre-pass (decode + premul + boost) -> aperture gather (lensBlurImage). The
        // taps come from the SAME builder the CPU lane calls, so subsampling (draft/coarse
        // strides) matches by construction.
        const fx::ApertureKernel k =
            fx::makeApertureKernel(op.size, op.blades, op.curvature, op.rotationRad, op.draft);
        if (k.radius <= 0 || k.weight.empty()) return false;  // identity gather: CPU's no-op
        const std::int32_t tapOff = appendTaps(k);
        addConvert(im.raw.buf, im.work0.buf, 2, op.boost > 0.0f ? 1 : 0, 1.0f + 4.0f * op.boost,
                   op.boostThreshold);
        addLens(im.work0.buf, im.work1.buf, static_cast<std::int32_t>(k.weight.size()), tapOff);
    } else {
        // DoF: the pre-blurred pyramid + one interpolation dispatch (dofBlurImage).
        // ⚠ TWO INVARIANTS ARE LOAD-BEARING HERE, GPU edition: every level below reads work0
        // -- the converted SOURCE -- never a previous level's output (never a step-dilated
        // cascade), and the render is the single blur_dof.comp interpolation at the end --
        // never a per-pixel variable-radius gather. Do not "optimize" either property away:
        // the extra work is deliberate.
        if (op.feather <= 0.0f) return false;  // resolveBlurOp floors at 1; guard the division
        const bool iris = op.mode == static_cast<int>(core::DofBokeh::Iris);
        if (iris) {
            // The fixed v1 iris: 6 blades, mild curvature, no rotation, boost off (blur.cpp).
            addConvert(im.raw.buf, im.work0.buf, 2, 0, 1.0f, 0.0f);
            for (int lv = 1; lv <= 4; ++lv) {
                const float levelRadius = op.size * (static_cast<float>(lv) / 4.0f);
                const fx::ApertureKernel k =
                    fx::makeApertureKernel(levelRadius, 6, 0.35f, 0.0f, op.draft);
                if (k.radius <= 0 || k.weight.empty()) return false;
                const std::int32_t tapOff = appendTaps(k);
                addLens(im.work0.buf, im.levels[lv - 1].buf,
                        static_cast<std::int32_t>(k.weight.size()), tapOff);
            }
        } else {
            addConvert(im.raw.buf, im.work0.buf, 0);
            for (int lv = 1; lv <= 4; ++lv) {
                const float sigma = 0.5f * (op.size * (static_cast<float>(lv) / 4.0f));
                const std::vector<float> kk = gaussianHalfKernel(sigma);
                const std::int32_t kernOff = static_cast<std::int32_t>(kernData.size());
                kernData.insert(kernData.end(), kk.begin(), kk.end());
                const std::int32_t r = static_cast<std::int32_t>(kk.size()) - 1;
                // H into the level's own buffer (scratch), V into work1, unpremul back into
                // the level -- no dispatch ever reads and writes the same buffer.
                addSeparable(im.work0.buf, im.levels[lv - 1].buf, r, 0, 0, kernOff, 0.0f);
                addSeparable(im.levels[lv - 1].buf, im.work1.buf, r, 1, 0, kernOff, 0.0f);
                addConvert(im.work1.buf, im.levels[lv - 1].buf, 1);
            }
        }
        const DofPush p{w,      h,       op.cx,      op.cy,  -op.dirY,
                        op.dirX, op.band, op.feather, op.size};
        addStep(im.dofPipe, im.raw.buf, im.work1.buf, Aux::Dummy, &p, sizeof(p),
                {im.levels[0].buf, im.levels[1].buf, im.levels[2].buf, im.levels[3].buf});
    }

    // ---- upload -----------------------------------------------------------------------------
    if (!im.uploadBuf(im.raw, img.rgba.data(), bytes)) return false;
    if (!kernData.empty() &&
        !im.uploadBuf(im.kern, kernData.data(), kernData.size() * sizeof(float)))
        return false;
    if (!tapData.empty() &&
        !im.uploadBuf(im.taps, tapData.data(), tapData.size() * sizeof(TapGpu)))
        return false;

    // ---- descriptors (all written before recording; sets are idle after the fence wait) -----
    const VkDevice dev = im.ctx->device();
    std::array<VkDescriptorBufferInfo, kMaxSets * kBindings> infos;
    std::array<VkWriteDescriptorSet, kMaxSets * kBindings> writes;
    std::uint32_t writeCount = 0;
    const auto auxBuf = [&](Aux a) {
        switch (a) {
            case Aux::Kern: return im.kern.buf;
            case Aux::Taps: return im.taps.buf;
            case Aux::Lut: return im.lut.buf;
            default: return im.dummy.buf;
        }
    };
    for (std::size_t s = 0; s < steps.size(); ++s) {
        const Step& step = steps[s];
        const VkBuffer perBinding[kBindings] = {
            step.src,
            step.dst,
            auxBuf(step.aux),
            step.lv[0] != VK_NULL_HANDLE ? step.lv[0] : im.dummy.buf,
            step.lv[1] != VK_NULL_HANDLE ? step.lv[1] : im.dummy.buf,
            step.lv[2] != VK_NULL_HANDLE ? step.lv[2] : im.dummy.buf,
            step.lv[3] != VK_NULL_HANDLE ? step.lv[3] : im.dummy.buf,
        };
        for (std::uint32_t b = 0; b < kBindings; ++b) {
            infos[writeCount] = {perBinding[b], 0, VK_WHOLE_SIZE};
            writes[writeCount] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = im.sets[s],
                                  .dstBinding = b,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .pBufferInfo = &infos[writeCount]};
            ++writeCount;
        }
    }
    vkUpdateDescriptorSets(dev, writeCount, writes.data(), 0, nullptr);

    // ---- record: the dispatch chain, one submit, fence ---------------------------------------
    vkResetCommandBuffer(im.cmd, 0);
    const VkCommandBufferBeginInfo cbi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vkBeginCommandBuffer(im.cmd, &cbi) != VK_SUCCESS) return false;
    const VkMemoryBarrier chain{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                                 VK_ACCESS_SHADER_WRITE_BIT};
    const std::uint32_t groupsX = (static_cast<std::uint32_t>(w) + 7) / 8;
    const std::uint32_t groupsY = (static_cast<std::uint32_t>(h) + 7) / 8;
    for (std::size_t s = 0; s < steps.size(); ++s) {
        const Step& step = steps[s];
        vkCmdBindPipeline(im.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, step.pipe);
        vkCmdBindDescriptorSets(im.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, im.pipeLayout, 0, 1,
                                &im.sets[s], 0, nullptr);
        vkCmdPushConstants(im.cmd, im.pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           step.pushBytes, step.push.data());
        vkCmdDispatch(im.cmd, groupsX, groupsY, 1);
        if (s + 1 < steps.size())
            vkCmdPipelineBarrier(im.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &chain, 0, nullptr,
                                 0, nullptr);
    }
    // Every chain lands its final image in work1; copy it to the host-cached staging and make
    // the transfer visible before the fence signals.
    const VkMemoryBarrier toCopy{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(im.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toCopy, 0, nullptr, 0, nullptr);
    const VkBufferCopy copyRegion{0, 0, bytes};
    vkCmdCopyBuffer(im.cmd, im.work1.buf, im.readback.buf, 1, &copyRegion);
    const VkMemoryBarrier toHost{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(im.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         1, &toHost, 0, nullptr, 0, nullptr);
    if (vkEndCommandBuffer(im.cmd) != VK_SUCCESS) return false;
    if (!im.submitAndWait()) return false;

    // ---- readback (img stays untouched on every failure path above) -------------------------
    vmaInvalidateAllocation(im.allocator, im.readback.alloc, 0, VK_WHOLE_SIZE);
    std::memcpy(img.rgba.data(), im.readback.ptr, static_cast<std::size_t>(bytes));
    return true;
}

}  // namespace mosaic::render
