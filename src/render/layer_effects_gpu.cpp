#include "render/layer_effects_gpu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

#include <vk_mem_alloc.h>

#include "core/layer_effects.hpp"
#include "render/effect_primitives.hpp"
#include "render/gpu_policy.hpp"
#include "render/vulkan_context.hpp"
#include <shaders/le_plane.comp.spv.hpp>
#include <shaders/le_shade.comp.spv.hpp>

// The Vulkan compute lane of the layer-effect stack (layer_effects_gpu.hpp). Three halves, in the
// blur lane's shape:
//
//   1. ADMISSION -- a pure, device-free predicate over the effect stack (`layerEffectsAdmission`),
//      so the served/refused boundary is testable without a GPU and a refused stack never reaches
//      a Vulkan call.
//
//   2. The COOK -- everything render/layer_effects_render.cpp computes once per call, computed
//      here on the host with the same code on the same floats: the content box and the ROI (plain
//      integer arithmetic, transcribed), the signed distance fields (by CALLING the same public
//      `fx::signedDistanceField(AA)` the CPU lane calls), the Gaussian half-kernels and Kovesi box
//      radii (transcribed from effect_primitives.cpp), the (angle,distance) offsets, and the
//      concentric stroke ring stack. ⚠ The transcribed builders may not drift from their
//      originals: tests/test_layer_effects_gpu.cpp holds this lane to the CPU reference, so a
//      retune over there fails parity HERE, loudly, on any machine with a device.
//
//   3. The PLUMBING -- the BlurGpu VMA pattern (persistent context/pipelines, grow-only buffers,
//      persistently-mapped staging) driving one planned chain of compute dispatches per call:
//      seed a coverage plane, blur it, colourise and composite it, in the canonical z-order.
//      Working planes stay DEVICE-LOCAL (the blur passes hammer them per tap); the ROI window
//      comes back through an explicit copy into HOST_CACHED staging.
//
// `io` is not touched until the readback lands, so EVERY failure path above leaves it byte-exact
// for the CPU lane to serve.
namespace mosaic::render {
namespace {

using common::ColorF;
using common::ImageF;

// Refuse an absurd effect ROI; the CPU lane handles it. 64 MiB of RGBA float is a ~4.2 Mpx ROI --
// far larger than the dilated content box of any realistic styled layer, and already ~350 MB of
// device buffers once the four working RGBA buffers and five coverage planes are counted. This is
// OUR POLICY (raising it is a memory-budget question, not a correctness one); the DEVICE's own
// limit is `fitsStorageBufferRange`, and both are checked.
constexpr VkDeviceSize kMaxRoiBytes = 64ull << 20;

// The descriptor pool's ceiling. A stack deeper than this refuses rather than submitting a partial
// chain -- eight dispatches is a generous budget per effect instance (seed + three box passes in
// each direction + composite), so this holds roughly thirty stacked effects.
constexpr std::uint32_t kMaxSets = 256;
constexpr std::uint32_t kBindings = LayerEffectsGpu::kStorageBufferBindings;
constexpr std::uint32_t kPushCapacity = LayerEffectsGpu::kPushConstantBytes;

constexpr VkBufferUsageFlags kUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT;

// ---------------------------------------------------------------------------------------------
// std430 mirrors of the shader push blocks (keep in lockstep with shaders/le_*.comp).
// ---------------------------------------------------------------------------------------------

struct PlanePush {
    std::int32_t w, h, mode, dir, r, kernOff;
    float grow, inv;
};
static_assert(sizeof(PlanePush) == 32);

struct ShadePush {
    std::int32_t w, h, mode, blend, i0, i1, i2, i3;
    float cr, cg, cb, ca, opacity, p0, p1, p2, p3;
};
static_assert(sizeof(ShadePush) == 68);
static_assert(sizeof(ShadePush) <= kPushCapacity);

// le_plane.comp's mode constants.
enum PlaneMode : std::int32_t {
    kSeedDilate = 0,
    kSeedComplement = 1,
    kSeedCentre = 2,
    kCopy = 3,
    kGauss = 4,
    kBox = 5,
};

// le_shade.comp's mode constants.
enum ShadeMode : std::int32_t {
    kFillOpacity = 0,
    kShadowInto = 1,
    kGlowInto = 2,
    kPlaceBelow = 3,
    kOverlay = 4,
    kInnerShadow = 5,
    kInnerGlow = 6,
    kSatin = 7,
    kStrokeBelow = 8,
    kStrokeOver = 9,
};

// ---------------------------------------------------------------------------------------------
// Host cook -- transcribed from render/effect_primitives.cpp and render/layer_effects_render.cpp.
// Same expressions on the same floats, so the tables and the geometry match bit for bit.
// ---------------------------------------------------------------------------------------------

// effect_primitives.cpp gaussianBlur's kernel: normalised half-kernel [0..r], r = ceil(3*sigma).
[[nodiscard]] std::vector<float> gaussianHalfKernel(float sigma) {
    const int r = std::max(1, static_cast<int>(std::ceil(3.0f * sigma)));
    std::vector<float> kernel(static_cast<std::size_t>(r) + 1);
    const float inv2s2 = 1.0f / (2.0f * sigma * sigma);
    float sum = 0.0f;
    for (int k = 0; k <= r; ++k) {
        kernel[static_cast<std::size_t>(k)] =
            std::exp(-static_cast<float>(k) * static_cast<float>(k) * inv2s2);
        sum += (k == 0 ? kernel[static_cast<std::size_t>(k)]
                       : 2.0f * kernel[static_cast<std::size_t>(k)]);
    }
    for (float& k : kernel) k /= sum;
    return kernel;
}

// effect_primitives.cpp boxRadiiForGauss: the 3-pass Kovesi box widths for std-dev `sigma`.
[[nodiscard]] std::array<int, 3> boxRadiiForGauss(float sigma) {
    const double s = sigma;
    const double wIdeal = std::sqrt(12.0 * s * s / 3.0 + 1.0);
    int wl = static_cast<int>(std::floor(wIdeal));
    if (wl % 2 == 0) --wl;
    const int wu = wl + 2;
    const double mIdeal =
        (12.0 * s * s - 3.0 * wl * wl - 4.0 * 3.0 * wl - 3.0 * 3.0) / (-4.0 * wl - 4.0);
    const int m = static_cast<int>(std::lround(mIdeal));
    std::array<int, 3> radii{};
    for (int i = 0; i < 3; ++i) {
        const int width = (i < m ? wl : wu);
        radii[static_cast<std::size_t>(i)] = std::max(0, (width - 1) / 2);
    }
    return radii;
}

// layer_effects_render.cpp's Box + alphaBox: the tight box of pixels with alpha > minAlpha.
struct Box {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    [[nodiscard]] bool empty() const noexcept { return x1 <= x0 || y1 <= y0; }
};

[[nodiscard]] Box alphaBox(const ImageF& io, float minAlpha = 0.0f) {
    const int w = static_cast<int>(io.width), h = static_cast<int>(io.height);
    int minx = w, miny = h, maxx = -1, maxy = -1;
    for (int y = 0; y < h; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            if (io.rgba[row + static_cast<std::size_t>(x) * 4 + 3] > minAlpha) {
                minx = std::min(minx, x);
                miny = std::min(miny, y);
                maxx = std::max(maxx, x);
                maxy = std::max(maxy, y);
            }
        }
    }
    if (maxx < 0) return {};
    return {minx, miny, maxx + 1, maxy + 1};
}

// `size` -> Gaussian std-dev, and the threshold above which the CPU swaps the exact separable
// kernel for the 3-box approximation (layer_effects_render.cpp's sigmaForSize / kLargeBlurSigma).
[[nodiscard]] float sigmaForSize(float size) { return std::max(0.0f, size) * (1.0f / 3.0f); }
constexpr float kLargeBlurSigma = 8.0f;
constexpr float kDegToRad = 0.017453292519943295f;

// The shadow OFFSET (buffer px) for (angleDeg, distance): Photoshop convention, `angleDeg` is the
// LIGHT direction and the shadow falls opposite it with screen-y down.
struct Offset {
    float dx = 0.0f, dy = 0.0f;
};
[[nodiscard]] Offset shadowOffset(float angleDeg, float distance) {
    const float a = angleDeg * 3.14159265358979323846f / 180.0f;
    return {-distance * std::cos(a), distance * std::sin(a)};
}

// applyStrokes' feather + the innermost outside ring's reach under the content rim.
constexpr float kAa = 1.0f;
constexpr float kInnerBack = 1.5f;

// A resolved concentric stroke ring, solid-paint only (the lane refuses any other kind).
struct Ring {
    float lo = 0.0f, hi = 0.0f, opacity = 1.0f;
    ColorF color;
    core::BlendMode blend = core::BlendMode::Normal;
    bool inside = false;
};

[[nodiscard]] bool solidOrNone(const core::vec::Paint& p) noexcept {
    return std::holds_alternative<core::vec::NoPaint>(p) ||
           std::holds_alternative<core::vec::SolidPaint>(p);
}

// The constant colour of a solid paint; {0,0,0,0} for NoPaint. Admission guarantees nothing else
// reaches here, so this never has to evaluate a gradient or a pattern.
[[nodiscard]] ColorF solidColor(const core::vec::Paint& p) {
    if (const auto* s = std::get_if<core::vec::SolidPaint>(&p)) return s->color;
    return ColorF{0.0f, 0.0f, 0.0f, 0.0f};
}

// Which binding-2 buffer a step wants. SYMBOLIC because the kernel buffer is created (or regrown)
// by the upload phase AFTER planning -- resolving its handle during planning would bind a null or
// stale buffer (the first-run/regrowth bug the blur lane's parity tests caught on RADV).
enum class Aux : std::uint8_t { Dummy, Kern, Alpha };
enum class Op : std::uint8_t { Dispatch, ClearVec4 };

struct Step {
    Op op = Op::Dispatch;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkBuffer b0 = VK_NULL_HANDLE;  // binding 0 (also the ClearVec4 target)
    VkBuffer b1 = VK_NULL_HANDLE;  // binding 1
    Aux aux = Aux::Dummy;          // binding 2
    VkBuffer b3 = VK_NULL_HANDLE;  // binding 3; null -> the dummy
    std::array<std::byte, kPushCapacity> push{};
    std::uint32_t pushBytes = 0;
    std::uint32_t groupsX = 0, groupsY = 0;
};

}  // namespace

std::string_view layerEffectRefusalName(LayerEffectRefusal r) noexcept {
    switch (r) {
        case LayerEffectRefusal::None: return "none";
        case LayerEffectRefusal::CpuOnlyMode: return "cpu-only mode";
        case LayerEffectRefusal::NoDevice: return "no device";
        case LayerEffectRefusal::DeviceTooSmall: return "device too small";
        case LayerEffectRefusal::BufferTooLarge: return "effect ROI too large";
        case LayerEffectRefusal::NonSolidPaint: return "non-solid paint";
        case LayerEffectRefusal::Bevel: return "bevel & emboss";
        case LayerEffectRefusal::StackTooDeep: return "effect stack too deep";
        case LayerEffectRefusal::DeviceError: return "device error";
    }
    return "unknown";
}

LayerEffectRefusal layerEffectsAdmission(const core::LayerEffects& fx) noexcept {
    // A gradient or pattern anywhere: the paint evaluator and, decisively, its banding dither are
    // not reproducible at the Vulkan 1.0 floor (layer_effects_gpu.hpp).
    for (const core::StrokeEffect& st : fx.strokes)
        if (st.enabled && st.width > 0.0f && !solidOrNone(st.paint))
            return LayerEffectRefusal::NonSolidPaint;
    for (const core::OverlayEffect* ov :
         {&fx.colorOverlay, &fx.gradientOverlay, &fx.patternOverlay})
        if (ov->enabled && !solidOrNone(ov->paint)) return LayerEffectRefusal::NonSolidPaint;
    for (const core::GlowEffect* gl : {&fx.outerGlow, &fx.innerGlow})
        if (gl->enabled && !solidOrNone(gl->paint)) return LayerEffectRefusal::NonSolidPaint;
    // Bevel & Emboss dithers its shade ramp through a 64-bit hash. An enabled bevel with size <= 0
    // is the CPU lane's own no-op (applyBevel returns before shading), so it is not a refusal --
    // only a bevel that would actually ink is.
    if (fx.bevel.enabled && fx.bevel.size > 0.0f) return LayerEffectRefusal::Bevel;
    return LayerEffectRefusal::None;
}

struct LayerEffectsGpu::Impl {
    std::shared_ptr<VulkanContext> ctx;   // the process-wide shared device (S60-alpha)
    VkCommandPool pool = VK_NULL_HANDLE;  // OURS: pools are externally synchronized
    VmaAllocator allocator = VK_NULL_HANDLE;

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkShaderModule planeShader = VK_NULL_HANDLE;
    VkShaderModule shadeShader = VK_NULL_HANDLE;
    VkPipeline planePipe = VK_NULL_HANDLE;
    VkPipeline shadePipe = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    LayerEffectRefusal lastRefusal = LayerEffectRefusal::None;

    struct Buf {
        VkBuffer buf = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        void* ptr = nullptr;  // mapped for Upload/Readback kinds, null for Device
        VkDeviceSize cap = 0;
    };
    // Host-visible: the uploaded ROI window, the readback staging, the captured coverage, the two
    // signed distance fields (each read once per invocation, never gathered), the kernel table and
    // a never-read stand-in. Device-local: the working RGBA pair and the coverage ping-pong, which
    // the blur passes hammer per tap.
    Buf rawImg, readback, alphaUp, sdUp, sdAAUp, kern, dummy;
    Buf img, below, pA, pB;

    enum class Kind { Upload, Readback, Device };

    void destroyBuf(Buf& b) {
        if (b.buf != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, b.buf, b.alloc);
        b = {};
    }

    bool ensureBuf(Buf& b, VkDeviceSize bytes, Kind kind) {
        if (b.cap >= bytes && b.buf != VK_NULL_HANDLE) return true;
        // No submission is in flight here (every apply() fence-waits), so growth is safe.
        destroyBuf(b);
        const VkBufferCreateInfo bci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = std::max<VkDeviceSize>(bytes, 64),
            .usage = kUsage,
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
        if (!ensureBuf(b, bytes, Kind::Upload)) return false;
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
        return vkWaitForFences(ctx->device(), 1, &fence, VK_TRUE, 60'000'000'000ull) == VK_SUCCESS;
    }

    ~Impl() {
        if (!ctx) return;
        const VkDevice dev = ctx->device();
        ctx->waitIdle();
        for (Buf* b : {&rawImg, &readback, &alphaUp, &sdUp, &sdAAUp, &kern, &dummy, &img, &below,
                       &pA, &pB})
            destroyBuf(*b);
        if (fence) vkDestroyFence(dev, fence, nullptr);
        if (cmd) vkFreeCommandBuffers(dev, pool, 1, &cmd);
        if (descPool) vkDestroyDescriptorPool(dev, descPool, nullptr);
        for (VkPipeline p : {planePipe, shadePipe})
            if (p) vkDestroyPipeline(dev, p, nullptr);
        for (VkShaderModule s : {planeShader, shadeShader})
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
        // The lane was DESIGNED to fit the floor -- four storage buffers against a guaranteed four,
        // an 80-byte push range against a guaranteed 128 -- but it must ASK rather than assume, so
        // a device that somehow cannot host the layout hands the work back to the CPU reference.
        if (!ctx->caps().fitsStorageBuffers(kBindings)) {
            error = "device allows only " +
                    std::to_string(ctx->caps().limits.maxPerStageDescriptorStorageBuffers) +
                    " storage buffers per stage; this lane needs " + std::to_string(kBindings);
            return false;
        }
        if (!ctx->caps().fitsPushConstants(kPushCapacity)) {
            error = "device allows only " +
                    std::to_string(ctx->caps().limits.maxPushConstantsSize) +
                    " push-constant bytes; this lane needs " + std::to_string(kPushCapacity);
            return false;
        }
        const VkDevice dev = ctx->device();

        const VmaAllocatorCreateInfo aci{
            .physicalDevice = ctx->physicalDevice(),
            .device = dev,
            .instance = ctx->instance(),
            // caps().apiVersion is exactly min(instance, device) -- a hard-coded newer version
            // would have VMA reach for entry points a 1.0 device lacks (the blur lane's note).
            .vulkanApiVersion = ctx->caps().apiVersion,
        };
        if (vmaCreateAllocator(&aci, &allocator) != VK_SUCCESS) {
            error = "vmaCreateAllocator failed";
            return false;
        }

        // One layout for both pipelines. le_plane.comp reads it as (src plane, dst plane, kernel,
        // unused); le_shade.comp as (target RGBA, coverage field, captured alpha, below scratch).
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
        if (!makeShader(shaders::le_plane_comp, shaders::le_plane_comp_size, planeShader) ||
            !makeShader(shaders::le_shade_comp, shaders::le_shade_comp_size, shadeShader)) {
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
        if (!makePipeline(planeShader, planePipe) || !makePipeline(shadeShader, shadePipe)) {
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
        const std::vector<VkDescriptorSetLayout> layouts(kMaxSets, setLayout);
        sets.resize(kMaxSets);
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
        // The never-read placeholder for bindings a step does not use.
        std::array<float, 16> zeros{};
        if (!uploadBuf(dummy, zeros.data(), zeros.size() * sizeof(float))) {
            error = "placeholder upload failed";
            return false;
        }
        return true;
    }
};

LayerEffectsGpu::LayerEffectsGpu() : m_impl(std::make_unique<Impl>()) {}
LayerEffectsGpu::~LayerEffectsGpu() = default;

std::unique_ptr<LayerEffectsGpu> LayerEffectsGpu::create(bool enableValidation,
                                                         std::string& error) {
    // CPU-only mode (render/gpu_policy.hpp, S60-b item 14): decline BEFORE any Vulkan object
    // exists. The fallback is render::applyEffects -- the very reference the parity tests hold this
    // lane to -- so a refusal here moves the work, never the pixels.
    if (!computeLaneAllowed("layer effects", error)) return nullptr;
    auto gpu = std::unique_ptr<LayerEffectsGpu>(new LayerEffectsGpu());
    if (!gpu->m_impl->init(enableValidation, error)) return nullptr;
    return gpu;
}

std::string LayerEffectsGpu::deviceName() const {
    return m_impl->ctx ? m_impl->ctx->deviceName() : std::string{};
}

LayerEffectRefusal LayerEffectsGpu::lastRefusal() const noexcept { return m_impl->lastRefusal; }

bool LayerEffectsGpu::apply(ImageF& io, const core::LayerEffects& fx, bool /*antialias*/,
                            const std::optional<common::Affine2D>& /*bufferToLayer*/) {
    Impl& im = *m_impl;
    im.lastRefusal = LayerEffectRefusal::None;
    if (!im.ctx) {
        im.lastRefusal = LayerEffectRefusal::NoDevice;
        return false;
    }
    // The CPU lane's own short-circuit owns these: an empty stack must stay byte-identical and pay
    // nothing, so the lane declines rather than round-tripping a buffer to draw nothing.
    if (fx.empty() || io.empty()) return false;

    const LayerEffectRefusal admission = layerEffectsAdmission(fx);
    if (admission != LayerEffectRefusal::None) {
        im.lastRefusal = admission;
        return false;
    }

    const int w = static_cast<int>(io.width), h = static_cast<int>(io.height);

    // ---- the cook: content box, ROI, coverage, distance fields ------------------------------
    // Every effect attaches to the layer's coverage; nothing to do without any. (The CPU lane's
    // `anchor` box is not derived here: it only ever anchors a gradient's box or a pattern's tile
    // phase, and both of those paints are refused, so it has no reader on this lane.)
    const Box content = alphaBox(io);
    if (content.empty()) return true;  // served: the reference leaves `io` untouched, so do we

    float reach = core::effectsOutwardReach(fx);
    for (const core::ShadowEffect& sh : fx.innerShadows)
        if (sh.enabled) reach = std::max(reach, sh.distance + sh.size + sh.spread);
    if (fx.innerGlow.enabled && !std::holds_alternative<core::vec::NoPaint>(fx.innerGlow.paint))
        reach = std::max(reach, fx.innerGlow.size + fx.innerGlow.choke);
    const int pad = static_cast<int>(std::ceil(reach)) + 2;
    const int rx0 = std::max(0, content.x0 - pad);
    const int ry0 = std::max(0, content.y0 - pad);
    const int rx1 = std::min(w, content.x1 + pad);
    const int ry1 = std::min(h, content.y1 + pad);
    const int rw = rx1 - rx0, rh = ry1 - ry0;
    if (rw <= 0 || rh <= 0) return true;  // served: nothing to draw, `io` untouched

    const std::size_t roiPixels = static_cast<std::size_t>(rw) * static_cast<std::size_t>(rh);
    const VkDeviceSize rgbaBytes = static_cast<VkDeviceSize>(roiPixels) * 4 * sizeof(float);
    const VkDeviceSize planeBytes = static_cast<VkDeviceSize>(roiPixels) * sizeof(float);
    // Two caps meaning different things: kMaxRoiBytes is OUR policy, fitsStorageBufferRange is the
    // DEVICE's (Vulkan 1.0 guarantees only 128 MiB). Both must hold.
    if (rgbaBytes > kMaxRoiBytes || !im.ctx->caps().fitsStorageBufferRange(rgbaBytes)) {
        im.lastRefusal = LayerEffectRefusal::BufferTooLarge;
        return false;
    }
    const std::uint32_t groupsX = (static_cast<std::uint32_t>(rw) + 7) / 8;
    const std::uint32_t groupsY = (static_cast<std::uint32_t>(rh) + 7) / 8;
    const std::uint32_t maxGroups = im.ctx->caps().maxWorkgroupsPerAxis();
    if (groupsX > maxGroups || groupsY > maxGroups) {
        im.lastRefusal = LayerEffectRefusal::DeviceTooSmall;
        return false;
    }

    // The ORIGINAL coverage, captured before fill-opacity dims it (the CPU lane's `alpha`).
    std::vector<float> alpha(roiPixels);
    for (int y = 0; y < rh; ++y)
        for (int x = 0; x < rw; ++x)
            alpha[static_cast<std::size_t>(y) * rw + x] =
                io.at(static_cast<std::uint32_t>(rx0 + x), static_cast<std::uint32_t>(ry0 + y)).a;

    bool anyDropShadow = false, anyInnerShadow = false;
    for (const core::ShadowEffect& sh : fx.dropShadows)
        if (sh.enabled) anyDropShadow = true;
    for (const core::ShadowEffect& sh : fx.innerShadows)
        if (sh.enabled) anyInnerShadow = true;
    const bool anyOuterGlow =
        fx.outerGlow.enabled && !std::holds_alternative<core::vec::NoPaint>(fx.outerGlow.paint);
    const bool anyInnerGlow =
        fx.innerGlow.enabled && !std::holds_alternative<core::vec::NoPaint>(fx.innerGlow.paint);
    const bool anySatin = fx.satin.enabled && fx.satin.opacity > 0.0f;
    bool anyStroke = false;
    for (const core::StrokeEffect& st : fx.strokes)
        if (st.enabled && st.width > 0.0f) anyStroke = true;

    // ⚠ COOKED, NOT PORTED. The Felzenszwalb-Huttenlocher transform is a sequential
    // parabola-envelope scan; a GPU distance transform would be a DIFFERENT field, moving every
    // stroke edge and shadow contour. Calling the SAME public builders the CPU lane calls makes
    // the two fields identical by construction rather than by transcription.
    std::vector<float> sdShadow, sdStroke;
    if (anyDropShadow || anyInnerShadow || anyOuterGlow || anyInnerGlow)
        sdShadow = fx::signedDistanceField(alpha, rw, rh);
    if (anyStroke) sdStroke = fx::signedDistanceFieldAA(alpha, rw, rh, 3);

    // The concentric ring stack, exactly as applyStrokes builds it: index 0 innermost, each pushed
    // outward by the cumulative width; a zero-width or NoPaint stroke still occupies its place.
    std::vector<Ring> outsideRings, overRings;
    if (anyStroke) {
        using Align = core::StrokeEffect::Align;
        float cum = 0.0f;
        for (const core::StrokeEffect& st : fx.strokes) {
            if (!st.enabled) continue;
            const float width = std::max(0.0f, st.width);
            Ring r;
            switch (st.align) {
                case Align::Outside: r.lo = cum;                r.hi = cum + width;         break;
                case Align::Inside:  r.lo = -cum - width;       r.hi = -cum;                break;
                case Align::Center:  r.lo = cum - width * 0.5f; r.hi = cum + width * 0.5f;  break;
            }
            cum += width;
            if (width <= 0.0f) continue;
            if (std::holds_alternative<core::vec::NoPaint>(st.paint)) continue;
            r.color = solidColor(st.paint);
            r.opacity = st.opacity;
            r.blend = st.blend;
            r.inside = st.align == Align::Inside;
            (st.align == Align::Outside ? outsideRings : overRings).push_back(r);
        }
    }
    const bool needBelow = anyDropShadow || anyOuterGlow || !outsideRings.empty();

    // ---- buffers ------------------------------------------------------------------------------
    // ⚠ EVERY buffer the plan names by HANDLE is ensured HERE, before planning. Growing a buffer
    // destroys and recreates it, so a plan built against a not-yet-ensured (or just-regrown) handle
    // binds a null or stale buffer -- the first-run/regrowth bug the blur lane's parity tests
    // caught on RADV. Only the kernel table, whose size is not known until the plan is complete,
    // stays SYMBOLIC (Aux::Kern) and is resolved when the descriptors are written.
    const bool ok =
        im.ensureBuf(im.rawImg, rgbaBytes, Impl::Kind::Upload) &&
        im.ensureBuf(im.readback, rgbaBytes, Impl::Kind::Readback) &&
        im.ensureBuf(im.img, rgbaBytes, Impl::Kind::Device) &&
        im.ensureBuf(im.pA, planeBytes, Impl::Kind::Device) &&
        im.ensureBuf(im.pB, planeBytes, Impl::Kind::Device) &&
        im.ensureBuf(im.alphaUp, planeBytes, Impl::Kind::Upload) &&
        (!needBelow || im.ensureBuf(im.below, rgbaBytes, Impl::Kind::Device)) &&
        (sdShadow.empty() || im.ensureBuf(im.sdUp, planeBytes, Impl::Kind::Upload)) &&
        (sdStroke.empty() || im.ensureBuf(im.sdAAUp, planeBytes, Impl::Kind::Upload));
    if (!ok) {
        im.lastRefusal = LayerEffectRefusal::DeviceError;
        return false;
    }

    // ---- plan the dispatch chain + cook the kernel tables -------------------------------------
    std::vector<Step> steps;
    std::vector<float> kernData;

    const auto addPlane = [&](VkBuffer src, VkBuffer dst, const PlanePush& p, Aux aux,
                              bool lineDispatch) {
        Step s;
        s.pipe = im.planePipe;
        s.b0 = src;
        s.b1 = dst;
        s.aux = aux;
        std::memcpy(s.push.data(), &p, sizeof(p));
        s.pushBytes = sizeof(p);
        if (lineDispatch) {
            // The box pass is one invocation per LINE over a flat 64-wide grid.
            const std::uint32_t lines =
                p.dir == 0 ? static_cast<std::uint32_t>(rh) : static_cast<std::uint32_t>(rw);
            s.groupsX = (lines + 63) / 64;
            s.groupsY = 1;
        } else {
            s.groupsX = groupsX;
            s.groupsY = groupsY;
        }
        steps.push_back(s);
    };
    const auto addShade = [&](VkBuffer target, VkBuffer planeA, VkBuffer belowSrc,
                              const ShadePush& p) {
        Step s;
        s.pipe = im.shadePipe;
        s.b0 = target;
        s.b1 = planeA;
        s.aux = Aux::Alpha;
        s.b3 = belowSrc;
        std::memcpy(s.push.data(), &p, sizeof(p));
        s.pushBytes = sizeof(p);
        s.groupsX = groupsX;
        s.groupsY = groupsY;
        steps.push_back(s);
    };
    const auto addClearBelow = [&]() {
        Step s;
        s.op = Op::ClearVec4;
        s.b0 = im.below.buf;
        steps.push_back(s);
    };
    // Seed a coverage field from one of the distance fields into pA.
    const auto addSeed = [&](VkBuffer sdBuf, std::int32_t mode, float grow, float inv) {
        const PlanePush p{rw, rh, mode, 0, 0, 0, grow, inv};
        addPlane(sdBuf, im.pA.buf, p, Aux::Dummy, false);
    };
    // Blur the plane in pA IN PLACE, mirroring the CPU's two lanes: the exact separable Gaussian
    // (reflect-101) for small sigma, the 3-pass Kovesi box approximation (clamp) for large.
    const auto addBlur = [&](float sigma, bool boxLane) {
        if (sigma <= 0.0f) return;
        if (boxLane) {
            for (const int r : boxRadiiForGauss(sigma)) {
                if (r <= 0) continue;  // effect_primitives skips the whole pass, both directions
                const PlanePush hp{rw, rh, kBox, 0, r, 0, 0.0f, 0.0f};
                addPlane(im.pA.buf, im.pB.buf, hp, Aux::Dummy, true);
                const PlanePush vp{rw, rh, kBox, 1, r, 0, 0.0f, 0.0f};
                addPlane(im.pB.buf, im.pA.buf, vp, Aux::Dummy, true);
            }
            return;
        }
        const std::vector<float> k = gaussianHalfKernel(sigma);
        const std::int32_t off = static_cast<std::int32_t>(kernData.size());
        kernData.insert(kernData.end(), k.begin(), k.end());
        const std::int32_t r = static_cast<std::int32_t>(k.size()) - 1;
        const PlanePush hp{rw, rh, kGauss, 0, r, off, 0.0f, 0.0f};
        addPlane(im.pA.buf, im.pB.buf, hp, Aux::Kern, false);
        const PlanePush vp{rw, rh, kGauss, 1, r, off, 0.0f, 0.0f};
        addPlane(im.pB.buf, im.pA.buf, vp, Aux::Kern, false);
    };
    // blurCoverage (shadow/glow): sigma = size/3, box at or above kLargeBlurSigma.
    const auto addCoverageBlur = [&](float size) {
        const float sigma = sigmaForSize(size);
        addBlur(sigma, sigma >= kLargeBlurSigma);
    };
    // bevelSatinBlur: radius -> sigma = radius/2.5, box above 4.
    const auto addSatinBlur = [&](float radiusPx) {
        if (radiusPx <= 0.0f) return;
        const float sigma = radiusPx / 2.5f;
        addBlur(sigma, sigma > 4.0f);
    };
    // A shade push carrying only the constant fields; each site fills in what it needs.
    const auto shadePush = [&](std::int32_t mode, core::BlendMode blend, ColorF c, float opacity) {
        ShadePush p{};
        p.w = rw;
        p.h = rh;
        p.mode = mode;
        p.blend = static_cast<std::int32_t>(blend);
        p.cr = c.r;
        p.cg = c.g;
        p.cb = c.b;
        p.ca = c.a;
        p.opacity = opacity;
        return p;
    };

    // MID -- the layer's own pixels, dimmed by fill-opacity over the content box.
    if (fx.fillOpacity < 1.0f) {
        ShadePush p = shadePush(kFillOpacity, core::BlendMode::Normal, ColorF{}, 1.0f);
        p.i0 = content.x0 - rx0;
        p.i1 = content.y0 - ry0;
        p.i2 = content.x1 - rx0;
        p.i3 = content.y1 - ry0;
        p.p0 = std::clamp(fx.fillOpacity, 0.0f, 1.0f);
        addShade(im.img.buf, im.pA.buf, im.dummy.buf, p);
    }

    // BELOW -- drop shadows in vector order, then the outer glow (later over earlier), into the
    // transparent scratch; then the layer's own pixels are placed OVER the result.
    if (anyDropShadow || anyOuterGlow) {
        addClearBelow();
        for (const core::ShadowEffect& sh : fx.dropShadows) {
            if (!sh.enabled) continue;
            addSeed(im.sdUp.buf, kSeedDilate, sh.spread, 0.0f);
            addCoverageBlur(sh.size);
            const Offset o = shadowOffset(sh.angleDeg, sh.distance);
            ShadePush p = shadePush(kShadowInto, sh.blend, sh.color, sh.opacity);
            p.p0 = o.dx;
            p.p1 = o.dy;
            addShade(im.below.buf, im.pA.buf, im.dummy.buf, p);
        }
        if (anyOuterGlow) {
            addSeed(im.sdUp.buf, kSeedDilate, fx.outerGlow.choke, 0.0f);
            addCoverageBlur(fx.outerGlow.size);
            const ShadePush p = shadePush(kGlowInto, fx.outerGlow.blend,
                                          solidColor(fx.outerGlow.paint), fx.outerGlow.opacity);
            addShade(im.below.buf, im.pA.buf, im.dummy.buf, p);
        }
        const ShadePush p = shadePush(kPlaceBelow, core::BlendMode::Normal, ColorF{}, 1.0f);
        addShade(im.img.buf, im.pA.buf, im.below.buf, p);
    }

    // MID overlays, in the fixed z-order, each clipped to the ORIGINAL coverage. Only a solid (or
    // absent) paint reaches here -- the gradient and pattern overlays are refused by admission, so
    // in practice this serves the COLOUR overlay and any overlay slot holding a solid paint.
    for (const core::OverlayEffect* ov :
         {&fx.colorOverlay, &fx.gradientOverlay, &fx.patternOverlay}) {
        if (!ov->enabled || std::holds_alternative<core::vec::NoPaint>(ov->paint)) continue;
        const ShadePush p = shadePush(kOverlay, ov->blend, solidColor(ov->paint), ov->opacity);
        addShade(im.img.buf, im.pA.buf, im.dummy.buf, p);
    }

    // SATIN -- the shape's coverage blurred once, then interfered with an offset copy of itself
    // (blur and translation commute, so one blurred plane is sampled twice).
    if (anySatin) {
        const PlanePush cp{rw, rh, kCopy, 0, 0, 0, 0.0f, 0.0f};
        addPlane(im.alphaUp.buf, im.pA.buf, cp, Aux::Dummy, false);
        addSatinBlur(fx.satin.size);
        const float a = fx.satin.angleDeg * kDegToRad;
        ShadePush p = shadePush(kSatin, fx.satin.blend, fx.satin.color, fx.satin.opacity);
        p.i0 = fx.satin.invert ? 1 : 0;
        p.p0 = std::cos(a) * fx.satin.distance;
        p.p1 = -std::sin(a) * fx.satin.distance;  // screen-y down
        addShade(im.img.buf, im.pA.buf, im.dummy.buf, p);
    }

    // INNER shadow / glow -- over the layer, clipped to the captured coverage.
    if (anyInnerShadow) {
        for (const core::ShadowEffect& sh : fx.innerShadows) {
            if (!sh.enabled) continue;
            addSeed(im.sdUp.buf, kSeedComplement, -sh.spread, 0.0f);
            addCoverageBlur(sh.size);
            const Offset o = shadowOffset(sh.angleDeg, sh.distance);
            ShadePush p = shadePush(kInnerShadow, sh.blend, sh.color, sh.opacity);
            p.p0 = o.dx;
            p.p1 = o.dy;
            addShade(im.img.buf, im.pA.buf, im.dummy.buf, p);
        }
    }
    if (anyInnerGlow) {
        const core::GlowEffect& gl = fx.innerGlow;
        if (gl.source == core::GlowEffect::Source::Edge) {
            addSeed(im.sdUp.buf, kSeedComplement, -gl.choke, 0.0f);
            addCoverageBlur(gl.size);
        } else {
            // Centre: -sd is the interior depth; a linear ramp over `size` px, no blur.
            addSeed(im.sdUp.buf, kSeedCentre, gl.choke, 1.0f / std::max(1.0f, gl.size));
        }
        const ShadePush p = shadePush(kInnerGlow, gl.blend, solidColor(gl.paint), gl.opacity);
        addShade(im.img.buf, im.pA.buf, im.dummy.buf, p);
    }

    // (BEVEL would land here; it is refused by admission -- see the header.)

    // ABOVE -- concentric strokes. Outside rings go into a fresh scratch, drawn OUTERMOST FIRST so
    // each ring's AA outer edge fades over the solid ring beneath it, and the innermost reaches
    // kInnerBack under the content rim; then the content is composited over the result.
    if (anyStroke) {
        if (!outsideRings.empty()) {
            addClearBelow();
            for (int k = static_cast<int>(outsideRings.size()) - 1; k >= 0; --k) {
                const Ring& r = outsideRings[static_cast<std::size_t>(k)];
                ShadePush p = shadePush(kStrokeBelow, r.blend, r.color, r.opacity);
                p.p0 = r.lo - (k == 0 ? kInnerBack : kAa);
                p.p1 = r.hi;
                addShade(im.below.buf, im.sdAAUp.buf, im.dummy.buf, p);
            }
            const ShadePush p = shadePush(kPlaceBelow, core::BlendMode::Normal, ColorF{}, 1.0f);
            addShade(im.img.buf, im.pA.buf, im.below.buf, p);
        }
        for (const Ring& r : overRings) {
            ShadePush p = shadePush(kStrokeOver, r.blend, r.color, r.opacity);
            p.i0 = r.inside ? 1 : 0;
            p.p0 = r.lo;
            p.p1 = r.hi;
            addShade(im.img.buf, im.sdAAUp.buf, im.dummy.buf, p);
        }
    }

    std::uint32_t dispatchCount = 0;
    for (const Step& s : steps)
        if (s.op == Op::Dispatch) ++dispatchCount;
    if (dispatchCount > kMaxSets) {
        im.lastRefusal = LayerEffectRefusal::StackTooDeep;
        return false;
    }
    if (dispatchCount == 0) return true;  // served: every effect resolved to a no-op

    // ---- upload -------------------------------------------------------------------------------
    {
        // The ROI window of `io`, row by row (the layer buffer is wider than the ROI).
        auto* dst = static_cast<float*>(im.rawImg.ptr);
        for (int y = 0; y < rh; ++y) {
            const std::size_t src =
                ((static_cast<std::size_t>(ry0 + y) * w) + static_cast<std::size_t>(rx0)) * 4;
            std::memcpy(dst + static_cast<std::size_t>(y) * rw * 4, io.rgba.data() + src,
                        static_cast<std::size_t>(rw) * 4 * sizeof(float));
        }
        vmaFlushAllocation(im.allocator, im.rawImg.alloc, 0, VK_WHOLE_SIZE);
    }
    if (!im.uploadBuf(im.alphaUp, alpha.data(), planeBytes) ||
        (!sdShadow.empty() && !im.uploadBuf(im.sdUp, sdShadow.data(), planeBytes)) ||
        (!sdStroke.empty() && !im.uploadBuf(im.sdAAUp, sdStroke.data(), planeBytes)) ||
        (!kernData.empty() &&
         !im.uploadBuf(im.kern, kernData.data(), kernData.size() * sizeof(float)))) {
        im.lastRefusal = LayerEffectRefusal::DeviceError;
        return false;
    }

    // ---- descriptors (all written before recording; sets are idle after the fence wait) -------
    const VkDevice dev = im.ctx->device();
    std::vector<VkDescriptorBufferInfo> infos(static_cast<std::size_t>(dispatchCount) * kBindings);
    std::vector<VkWriteDescriptorSet> writes(infos.size());
    std::uint32_t writeCount = 0;
    std::uint32_t setIndex = 0;
    const auto auxBuf = [&](Aux a) {
        switch (a) {
            case Aux::Kern: return im.kern.buf != VK_NULL_HANDLE ? im.kern.buf : im.dummy.buf;
            case Aux::Alpha: return im.alphaUp.buf;
            default: return im.dummy.buf;
        }
    };
    for (const Step& step : steps) {
        if (step.op != Op::Dispatch) continue;
        const VkBuffer perBinding[kBindings] = {
            step.b0,
            step.b1,
            auxBuf(step.aux),
            step.b3 != VK_NULL_HANDLE ? step.b3 : im.dummy.buf,
        };
        for (std::uint32_t b = 0; b < kBindings; ++b) {
            infos[writeCount] = {perBinding[b], 0, VK_WHOLE_SIZE};
            writes[writeCount] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = im.sets[setIndex],
                                  .dstBinding = b,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .pBufferInfo = &infos[writeCount]};
            ++writeCount;
        }
        ++setIndex;
    }
    vkUpdateDescriptorSets(dev, writeCount, writes.data(), 0, nullptr);

    // ---- record: seed the working image, run the chain, copy out ------------------------------
    vkResetCommandBuffer(im.cmd, 0);
    const VkCommandBufferBeginInfo cbi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vkBeginCommandBuffer(im.cmd, &cbi) != VK_SUCCESS) {
        im.lastRefusal = LayerEffectRefusal::DeviceError;
        return false;
    }
    const VkMemoryBarrier transferToShader{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                           .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                           .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                                            VK_ACCESS_SHADER_WRITE_BIT};
    const VkMemoryBarrier shaderToShader{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                         .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                                          VK_ACCESS_SHADER_WRITE_BIT};
    const VkBufferCopy inRegion{0, 0, rgbaBytes};
    vkCmdCopyBuffer(im.cmd, im.rawImg.buf, im.img.buf, 1, &inRegion);
    vkCmdPipelineBarrier(im.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &transferToShader, 0, nullptr,
                         0, nullptr);
    setIndex = 0;
    for (const Step& step : steps) {
        if (step.op == Op::ClearVec4) {
            vkCmdFillBuffer(im.cmd, step.b0, 0, rgbaBytes, 0u);
            vkCmdPipelineBarrier(im.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &transferToShader, 0,
                                 nullptr, 0, nullptr);
            continue;
        }
        vkCmdBindPipeline(im.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, step.pipe);
        vkCmdBindDescriptorSets(im.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, im.pipeLayout, 0, 1,
                                &im.sets[setIndex], 0, nullptr);
        vkCmdPushConstants(im.cmd, im.pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, step.pushBytes,
                           step.push.data());
        vkCmdDispatch(im.cmd, step.groupsX, step.groupsY, 1);
        vkCmdPipelineBarrier(im.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &shaderToShader, 0,
                             nullptr, 0, nullptr);
        ++setIndex;
    }
    const VkMemoryBarrier toCopy{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(im.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toCopy, 0, nullptr, 0, nullptr);
    const VkBufferCopy outRegion{0, 0, rgbaBytes};
    vkCmdCopyBuffer(im.cmd, im.img.buf, im.readback.buf, 1, &outRegion);
    const VkMemoryBarrier toHost{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(im.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                         &toHost, 0, nullptr, 0, nullptr);
    if (vkEndCommandBuffer(im.cmd) != VK_SUCCESS || !im.submitAndWait()) {
        im.lastRefusal = LayerEffectRefusal::DeviceError;
        return false;
    }

    // ---- readback: the ROI window back into `io` (untouched on every failure path above) ------
    vmaInvalidateAllocation(im.allocator, im.readback.alloc, 0, VK_WHOLE_SIZE);
    const auto* src = static_cast<const float*>(im.readback.ptr);
    for (int y = 0; y < rh; ++y) {
        const std::size_t dst =
            ((static_cast<std::size_t>(ry0 + y) * w) + static_cast<std::size_t>(rx0)) * 4;
        std::memcpy(io.rgba.data() + dst, src + static_cast<std::size_t>(y) * rw * 4,
                    static_cast<std::size_t>(rw) * 4 * sizeof(float));
    }
    return true;
}

}  // namespace mosaic::render
