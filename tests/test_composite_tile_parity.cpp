// Fused tile-composite parity (S60-a, docs/s60-performance-plan.md §4): shaders/composite_tile.comp
// against the CPU reference compositor, one macrotile of one layer at a time.
//
// The kernel absorbs the WHOLE per-layer step -- inverse transform, resample, mask fold,
// clip-to-below, blend -- because a GPU compositor that leaves any of those on the CPU still
// round-trips per layer through host memory, which is precisely why S7-b's blend-only kernel is
// slower than the CPU path (§4's closing paragraph). This file is the proof that the fusion did
// not change the picture.
//
// The reference is `render::composite(doc, opts, Backend::Cpu)` itself -- not a second
// implementation written here. Each case builds a two-layer document (an 8-bit background, then
// the layer under test), composites it on the CPU, and separately: uploads the CPU's
// background-only composite as the GPU accumulator, dispatches the tile kernel once per macrotile
// for the layer under test, reads the accumulator back and quantises it the same way. The two
// 8-bit images must agree within 1 LSB = 1/255.
//
// The pipeline is built and dispatched DIRECTLY here, on `render::VulkanContext::shared()` --
// there is no `TileCompositor` class yet (the residency/dispatch plumbing is a separate session),
// so this file doubles as the executable specification of the kernel's interface. Gated on a
// usable Vulkan device: a machine without one WARNs and passes (the test_extrude_gpu.cpp /
// test_blur_gpu.cpp CI-safe pattern).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/blend_mode.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "render/compositor.hpp"
#include "render/gpu_caps.hpp"
#include "render/gpu_policy.hpp"
#include "render/render.hpp"
#include "render/tile_compositor.hpp"
#include "render/vulkan_context.hpp"

#include <shaders/composite_tile.comp.spv.hpp>
#include <shaders/composite_tile_indexed.comp.spv.hpp>

using mosaic::common::Affine2D;
using mosaic::common::Image;
using mosaic::common::ImageF;
using mosaic::core::BlendMode;
using mosaic::render::ResampleFilter;
namespace core = mosaic::core;
namespace render = mosaic::render;

namespace {

// ---------------------------------------------------------------------------------------------
// fp16 <-> fp32. The working buffer is R16G16B16A16_SFLOAT (the §2.4 decision), so the host has
// to speak half to seed the accumulator and to read it back. Round-to-nearest-even, subnormals
// included -- the test must not be the thing that loses precision.
// ---------------------------------------------------------------------------------------------
std::uint16_t floatToHalf(float f) {
    std::uint32_t x = 0;
    std::memcpy(&x, &f, sizeof(x));
    const std::uint32_t sign = (x >> 16) & 0x8000u;
    const std::uint32_t rawExp = (x >> 23) & 0xFFu;
    std::uint32_t mant = x & 0x7FFFFFu;
    if (rawExp == 0xFFu)  // inf / NaN
        return static_cast<std::uint16_t>(sign | 0x7C00u | (mant != 0 ? 0x200u : 0u));
    const std::int32_t exp = static_cast<std::int32_t>(rawExp) - 127 + 15;
    if (exp >= 31) return static_cast<std::uint16_t>(sign | 0x7C00u);  // overflow -> inf
    if (exp <= 0) {                                                    // subnormal or zero
        if (exp < -10) return static_cast<std::uint16_t>(sign);
        mant |= 0x800000u;
        const int shift = 14 - exp;
        std::uint32_t half = mant >> shift;
        const std::uint32_t rem = mant & ((1u << shift) - 1u);
        const std::uint32_t mid = 1u << (shift - 1);
        if (rem > mid || (rem == mid && (half & 1u) != 0)) ++half;
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t half = (static_cast<std::uint32_t>(exp) << 10) | (mant >> 13);
    const std::uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u) != 0)) ++half;  // carries into the exponent
    return static_cast<std::uint16_t>(sign | half);
}

float halfToFloat(std::uint16_t h) {
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
    const std::uint32_t exp = (h >> 10) & 0x1Fu;
    std::uint32_t mant = h & 0x3FFu;
    std::uint32_t x = 0;
    if (exp == 0) {
        if (mant != 0) {
            std::uint32_t e = 1;
            while ((mant & 0x400u) == 0) {
                mant <<= 1;
                ++e;
            }
            mant &= 0x3FFu;
            x = sign | ((114u - e) << 23) | (mant << 13);  // see the derivation in the commit
        } else {
            x = sign;
        }
    } else if (exp == 31) {
        x = sign | 0x7F800000u | (mant << 13);
    } else {
        x = sign | ((exp + 112u) << 23) | (mant << 13);  // 127 - 15 == 112
    }
    float f = 0.0f;
    std::memcpy(&f, &x, sizeof(f));
    return f;
}

// ---------------------------------------------------------------------------------------------
// The kernel's interface, mirrored on the host. THIS IS THE CONTRACT the dispatch plumbing has to
// honour; the static_assert pins the 112-byte block the shader declares.
// ---------------------------------------------------------------------------------------------
// A rect of the source to bind INSTEAD of the whole layer, so a layer larger than the device's
// maxImageDimension2D is still compositable. {0,0,0,0} = bind the whole layer.
struct SrcWindow {
    std::uint32_t x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] bool whole() const noexcept { return w == 0 || h == 0; }
};

struct PushBlock {
    // .w lanes carry the source WINDOW (see composite_tile.comp): srcOrigin in inv0/inv1, the
    // layer's TRUE size in mask0/mask1. A true size of 0 means "the whole layer is bound".
    float inv0[4];              //   0  TARGET px -> source px, row 0 (w = srcOriginX)
    float inv1[4];              //  16  row 1                        (w = srcOriginY)
    float mask0[4];             //  32  mask map, row 0              (w = srcTrueWidth)
    float mask1[4];             //  48  row 1                        (w = srcTrueHeight)
    float scale[2];             //  64  (sclX, sclY): source texels per output texel, >= 1
    std::int32_t tileOrigin[2]; //  72  this tile's origin in the TARGET buffer
    std::int32_t accOrigin[2];  //  80  tile origin inside the accumulator / clip images
    std::int32_t extent[2];     //  88  the tile's valid extent (edge tiles are partial)
    std::int32_t filter;        //  96  render::ResampleFilter, already resolved
    std::int32_t superN;        // 100  Supersample sub-sample count
    std::int32_t blend;         // 104  core::BlendMode
    float opacity;              // 108
    std::int32_t maskMode;      // 112  0 none / 1 proportional-in-source / 2 affine-in-source /
                                //      3 affine-in-target (unlinked)
    std::int32_t clipMode;      // 116  0 unclipped / 1 multiply alpha by the clip base
    std::int32_t clipWrite;     // 120  0 publish nothing / 1 publish this layer's placed alpha
};
static_assert(sizeof(PushBlock) == 124, "push block must match composite_tile.comp's 124 bytes");
static_assert(sizeof(PushBlock) <= mosaic::render::vk10::kMaxPushConstantsSize,
              "push block must fit Vulkan 1.0's guaranteed 128 bytes");

constexpr std::uint32_t kBindingAcc = 0;
constexpr std::uint32_t kBindingSrc = 1;
constexpr std::uint32_t kBindingMask = 2;
constexpr std::uint32_t kBindingClip = 3;
constexpr std::uint32_t kBindingClipOut = 4;
constexpr std::uint32_t kBindingTiles = 5;    // the dirty-macrotile list (S60-a item 10)
constexpr std::uint32_t kBindingSources = 6;  // the indexed variant's runtime array
constexpr std::uint32_t kWorkgroup = 8;
constexpr std::uint32_t kSpecTileList = 0;    // composite_tile.comp's specialization constant

// One macrotile in the list buffer: two ivec4, in the order the per-tile loop pushes them. The
// shader's std430 block declares exactly this.
struct TileRecord {
    std::int32_t tileOrigin[2];
    std::int32_t accOrigin[2];
    std::int32_t extent[2];
    std::int32_t pad[2];
};
static_assert(sizeof(TileRecord) == 32, "tile record must match composite_tile.comp's 2x ivec4");

// ---- Host-side resolution the shader relies on ------------------------------------------------

// The filter resolution is `render::resolveTileFilter` itself, not a copy of it: this test exists
// to prove the kernel agrees with the CPU reference, and a private transcription of the rule it
// is testing could drift away from the lane and take the proof with it.

// supersampleInto's sub-sample count, computed host-side so a ceil() never straddles differently
// on the two lanes.
std::int32_t supersampleN(const Affine2D& inv) {
    const double foot =
        std::max({1.0, std::hypot(inv.m00, inv.m10), std::hypot(inv.m01, inv.m11)});
    return static_cast<std::int32_t>(std::clamp(static_cast<int>(std::ceil(foot)) + 1, 2, 8));
}

// ---------------------------------------------------------------------------------------------
// A minimal Vulkan compute lane for the kernel. Raw Vulkan (no VMA): a handful of allocations per
// call, well under the guaranteed maxMemoryAllocationCount, and nothing to hide the interface
// behind.
// ---------------------------------------------------------------------------------------------
struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

// What one dispatch composites: everything but the pixels.
struct Step {
    Affine2D place = Affine2D::identity();  // layer-local -> document (pre * layer.transform())
    ResampleFilter filter = ResampleFilter::Nearest;
    bool liveDrag = false;
    BlendMode blend = BlendMode::Normal;
    float opacity = 1.0f;
    std::int32_t maskMode = 0;
    Affine2D maskXform = Affine2D::identity();  // modes 2/3
    bool clip = false;
    // Publish this layer's placed alpha to uClipOut -- what a NON-clipped layer does for the
    // clipped ones above it, so a whole clip run stays on the device (S60-a).
    bool clipWrite = false;
};

struct MaskData {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> coverage;
};

class TileLane {
public:
    static std::unique_ptr<TileLane> create(std::string& error) {
        auto self = std::unique_ptr<TileLane>(new TileLane());
        self->m_ctx = render::VulkanContext::shared(/*enableValidation=*/true, error);
        if (!self->m_ctx) return nullptr;

        const render::GpuCaps& caps = self->m_ctx->caps();
        // Lane admission: ask, never assume (gpu_caps.hpp). Two storage images, three sampled
        // images, one storage buffer (the dirty-macrotile list), 124 B of push constants -- all
        // inside the 1.0 floor.
        if (!caps.fitsStorageImages(2) || !caps.fitsSampledImages(3) ||
            !caps.fitsStorageBuffers(1) ||
            !caps.fitsPushConstants(sizeof(PushBlock))) {
            error = "device cannot host the tile kernel's descriptors";
            return nullptr;
        }
        if (caps.workingFormat != VK_FORMAT_R16G16B16A16_SFLOAT) {
            error = "working format is not rgba16f";
            return nullptr;
        }
        if (!self->formatOk(VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
            !self->formatOk(VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
            !self->formatOk(VK_FORMAT_R8_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
            !self->formatOk(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
            !self->formatOk(VK_FORMAT_R16_SFLOAT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            error = "device lacks one of the kernel's guaranteed formats";
            return nullptr;
        }

        const VkDevice dev = self->m_ctx->device();
        self->m_pool = self->m_ctx->createCommandPool(error);
        if (self->m_pool == VK_NULL_HANDLE) return nullptr;

        const VkDescriptorSetLayoutBinding bindings[6] = {
            {kBindingAcc, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
             nullptr},
            {kBindingSrc, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
             VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {kBindingMask, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
             VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {kBindingClip, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
             VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {kBindingClipOut, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
             nullptr},
            // Declared for BOTH specializations: the module statically references the buffer
            // whichever way kTileList is specialized, so a set without it is a layout mismatch.
            {kBindingTiles, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
             nullptr},
        };
        const VkDescriptorSetLayoutCreateInfo slci{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 6,
            .pBindings = bindings,
        };
        if (vkCreateDescriptorSetLayout(dev, &slci, nullptr, &self->m_setLayout) != VK_SUCCESS) {
            error = "vkCreateDescriptorSetLayout failed";
            return nullptr;
        }
        const VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushBlock)};
        const VkPipelineLayoutCreateInfo plci{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &self->m_setLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pcr,
        };
        if (vkCreatePipelineLayout(dev, &plci, nullptr, &self->m_pipeLayout) != VK_SUCCESS) {
            error = "vkCreatePipelineLayout failed";
            return nullptr;
        }
        const VkShaderModuleCreateInfo smci{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = mosaic::shaders::composite_tile_comp_size,
            .pCode = mosaic::shaders::composite_tile_comp,
        };
        if (vkCreateShaderModule(dev, &smci, nullptr, &self->m_shader) != VK_SUCCESS) {
            error = "vkCreateShaderModule failed";
            return nullptr;
        }
        // The SAME module, specialized twice: kTileList = 0 (one dispatch per macrotile, the
        // reference shape) and = 1 (one dispatch, gl_WorkGroupID.z picks the macrotile out of the
        // list buffer). Proving these byte-identical is what item 10's floor half has to earn.
        const VkSpecializationMapEntry specEntry{kSpecTileList, 0, sizeof(std::int32_t)};
        const std::int32_t specValues[2] = {0, 1};
        const VkSpecializationInfo specInfos[2] = {
            {1, &specEntry, sizeof(std::int32_t), &specValues[0]},
            {1, &specEntry, sizeof(std::int32_t), &specValues[1]},
        };
        VkComputePipelineCreateInfo cpci[2] = {};
        for (int i = 0; i < 2; ++i) {
            cpci[i] = VkComputePipelineCreateInfo{
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                          .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                          .module = self->m_shader,
                          .pName = "main",
                          .pSpecializationInfo = &specInfos[i]},
                .layout = self->m_pipeLayout,
            };
        }
        VkPipeline pipes[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 2, cpci, nullptr, pipes) != VK_SUCCESS) {
            error = "vkCreateComputePipelines failed";
            return nullptr;
        }
        self->m_pipeline = pipes[0];
        self->m_listPipeline = pipes[1];

        const VkDescriptorPoolSize sizes[3] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        };
        const VkDescriptorPoolCreateInfo dpci{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1,
            .poolSizeCount = 3,
            .pPoolSizes = sizes,
        };
        if (vkCreateDescriptorPool(dev, &dpci, nullptr, &self->m_descPool) != VK_SUCCESS) {
            error = "vkCreateDescriptorPool failed";
            return nullptr;
        }
        // The descriptor-indexed variant, built only where the caps allow it. A device without it
        // is not a failure: `haveIndexed()` is false and the cases that need it skip.
        self->initIndexed();
        // The set itself is allocated per call (writeDescriptors), so it can never carry a handle
        // from a previous call's already-destroyed images.
        // texelFetch only -- the reconstruction kernels are evaluated in the shader, never by the
        // sampler -- so this state is deliberately inert.
        const VkSamplerCreateInfo sci{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        };
        if (vkCreateSampler(dev, &sci, nullptr, &self->m_sampler) != VK_SUCCESS) {
            error = "vkCreateSampler failed";
            return nullptr;
        }
        const VkCommandBufferAllocateInfo cbai{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = self->m_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (vkAllocateCommandBuffers(dev, &cbai, &self->m_cmd) != VK_SUCCESS) {
            error = "vkAllocateCommandBuffers failed";
            return nullptr;
        }
        const VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(dev, &fci, nullptr, &self->m_fence) != VK_SUCCESS) {
            error = "vkCreateFence failed";
            return nullptr;
        }
        return self;
    }

    ~TileLane() {
        if (!m_ctx) return;
        const VkDevice dev = m_ctx->device();
        m_ctx->waitIdle();
        if (m_fence != VK_NULL_HANDLE) vkDestroyFence(dev, m_fence, nullptr);
        if (m_cmd != VK_NULL_HANDLE) vkFreeCommandBuffers(dev, m_pool, 1, &m_cmd);
        if (m_sampler != VK_NULL_HANDLE) vkDestroySampler(dev, m_sampler, nullptr);
        if (m_indexedPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, m_indexedPool, nullptr);
        if (m_indexedPipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, m_indexedPipeline, nullptr);
        if (m_indexedShader != VK_NULL_HANDLE)
            vkDestroyShaderModule(dev, m_indexedShader, nullptr);
        if (m_indexedPipeLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(dev, m_indexedPipeLayout, nullptr);
        if (m_indexedSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(dev, m_indexedSetLayout, nullptr);
        if (m_descPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, m_descPool, nullptr);
        if (m_listPipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, m_listPipeline, nullptr);
        if (m_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, m_pipeline, nullptr);
        if (m_shader != VK_NULL_HANDLE) vkDestroyShaderModule(dev, m_shader, nullptr);
        if (m_pipeLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, m_pipeLayout, nullptr);
        if (m_setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
        if (m_pool != VK_NULL_HANDLE) vkDestroyCommandPool(dev, m_pool, nullptr);
    }

    TileLane(const TileLane&) = delete;
    TileLane& operator=(const TileLane&) = delete;

    [[nodiscard]] std::uint32_t validationErrors() const { return m_ctx->validationErrors(); }
    [[nodiscard]] std::string deviceName() const { return m_ctx->deviceName(); }
    [[nodiscard]] const render::GpuCaps& caps() const { return m_ctx->caps(); }
    [[nodiscard]] bool haveIndexed() const { return m_indexedPipeline != VK_NULL_HANDLE; }

    // Composite ONE layer onto `acc`, macrotile by macrotile, and return the accumulator.
    // `clip` (canvas-sized clip-base alpha) may be empty when step.clip is false. `srcFormat`
    // picks how the layer's tile is stored on the device: the kernel only ever texelFetches it,
    // so it is format-agnostic -- rgba16f is the declared working format, R8G8B8A8_UNORM is what
    // today's 8-bit `core::RasterLayer` storage would upload with no conversion.
    ImageF composite(const ImageF& acc, const ImageF& srcFull, const MaskData& mask,
                     const std::vector<float>& clip, const Step& step, std::uint32_t tileSize,
                     VkFormat srcFormat = VK_FORMAT_R16G16B16A16_SFLOAT, SrcWindow window = {},
                     std::vector<float>* clipOut = nullptr,
                     render::TileDispatch dispatch = render::TileDispatch::PerTile) {
        // `Indexed` is only ever asked for by a case that checked haveIndexed() first; asserting
        // rather than quietly falling back is the point of the whole file (a test that passes
        // because the lane declined proves nothing).
        REQUIRE((dispatch != render::TileDispatch::Indexed || haveIndexed()));
        REQUIRE(!acc.empty());
        REQUIRE(!srcFull.empty());
        // Crop to the window; the kernel is told the layer's true size separately, so bounds and
        // the proportional mask mapping stay defined against the whole layer.
        ImageF src = srcFull;
        if (!window.whole()) {
            REQUIRE(window.x + window.w <= srcFull.width);
            REQUIRE(window.y + window.h <= srcFull.height);
            src = ImageF(window.w, window.h);
            for (std::uint32_t y = 0; y < window.h; ++y)
                for (std::uint32_t x = 0; x < window.w; ++x)
                    for (int c = 0; c < 4; ++c)
                        src.rgba[(static_cast<std::size_t>(y) * window.w + x) * 4 + c] =
                            srcFull.rgba[(static_cast<std::size_t>(window.y + y) * srcFull.width +
                                          window.x + x) * 4 + c];
        }
        const bool src8 = srcFormat == VK_FORMAT_R8G8B8A8_UNORM;
        const VkDevice dev = m_ctx->device();

        GpuImage accImg = makeImage(acc.width, acc.height, VK_FORMAT_R16G16B16A16_SFLOAT,
                                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        GpuImage srcImg = makeImage(src.width, src.height, srcFormat,
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        const std::uint32_t mw = mask.width > 0 ? mask.width : 1;
        const std::uint32_t mh = mask.height > 0 ? mask.height : 1;
        GpuImage maskImg = makeImage(mw, mh, VK_FORMAT_R8_UNORM,
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        const bool haveClip = !clip.empty();
        GpuImage clipImg =
            makeImage(haveClip ? acc.width : 1, haveClip ? acc.height : 1, VK_FORMAT_R16_SFLOAT,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        // The clip base this layer PUBLISHES. A 1x1 stand-in when nobody asked for one: the
        // binding still has to be written, but nothing reads or writes it.
        const bool publish = clipOut != nullptr;
        GpuImage clipOutImg =
            makeImage(publish ? acc.width : 1, publish ? acc.height : 1, VK_FORMAT_R16_SFLOAT,
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

        // Staging: fp16 acc + fp16 src + 8-bit mask + fp16 clip, and the acc readback.
        std::vector<std::uint16_t> accHalf(acc.rgba.size());
        for (std::size_t i = 0; i < acc.rgba.size(); ++i) accHalf[i] = floatToHalf(acc.rgba[i]);
        std::vector<std::uint8_t> srcBytes;
        std::vector<std::uint16_t> srcHalf;
        if (src8) {
            srcBytes = mosaic::common::toImage8(src).rgba;
        } else {
            srcHalf.resize(src.rgba.size());
            for (std::size_t i = 0; i < src.rgba.size(); ++i) srcHalf[i] = floatToHalf(src.rgba[i]);
        }
        const void* srcData = src8 ? static_cast<const void*>(srcBytes.data())
                                   : static_cast<const void*>(srcHalf.data());
        const std::size_t srcSize = src8 ? srcBytes.size() : srcHalf.size() * 2;
        std::vector<std::uint8_t> maskBytes =
            mask.coverage.empty() ? std::vector<std::uint8_t>{255} : mask.coverage;
        std::vector<std::uint16_t> clipHalf;
        clipHalf.reserve(haveClip ? clip.size() : 1);
        if (haveClip)
            for (const float v : clip) clipHalf.push_back(floatToHalf(v));
        else
            clipHalf.push_back(floatToHalf(1.0f));

        GpuBuffer accStage = makeHostBuffer(accHalf.size() * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        GpuBuffer srcStage = makeHostBuffer(srcSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        GpuBuffer maskStage = makeHostBuffer(maskBytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        GpuBuffer clipStage = makeHostBuffer(clipHalf.size() * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        GpuBuffer readback = makeHostBuffer(accHalf.size() * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        GpuBuffer clipBack = makeHostBuffer(publish ? acc.pixelCount() * 2 : 4,
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        std::memcpy(accStage.mapped, accHalf.data(), accHalf.size() * 2);
        std::memcpy(srcStage.mapped, srcData, srcSize);
        std::memcpy(maskStage.mapped, maskBytes.data(), maskBytes.size());
        std::memcpy(clipStage.mapped, clipHalf.data(), clipHalf.size() * 2);

        // The dirty-macrotile list. Built for EVERY shape, because the per-tile pipeline still
        // declares binding 5 (its specialization folds the read away, it does not remove the
        // declaration) and a set with an unwritten binding is undefined behaviour. The records are
        // the same six integers the per-tile loop pushes -- that identity IS the parity argument.
        std::vector<TileRecord> tileRecords;
        std::uint32_t widest = 0, tallest = 0;
        for (std::uint32_t ty = 0; ty < acc.height; ty += tileSize)
            for (std::uint32_t tx = 0; tx < acc.width; tx += tileSize) {
                const std::uint32_t ew = std::min(tileSize, acc.width - tx);
                const std::uint32_t eh = std::min(tileSize, acc.height - ty);
                widest = std::max(widest, ew);
                tallest = std::max(tallest, eh);
                tileRecords.push_back(TileRecord{
                    {static_cast<std::int32_t>(tx), static_cast<std::int32_t>(ty)},
                    {static_cast<std::int32_t>(tx), static_cast<std::int32_t>(ty)},
                    {static_cast<std::int32_t>(ew), static_cast<std::int32_t>(eh)},
                    {0, 0}});
            }
        REQUIRE(!tileRecords.empty());
        GpuBuffer tileBuf = makeHostBuffer(tileRecords.size() * sizeof(TileRecord),
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memcpy(tileBuf.mapped, tileRecords.data(), tileRecords.size() * sizeof(TileRecord));

        const bool indexed = dispatch == render::TileDispatch::Indexed;
        if (indexed)
            writeIndexedDescriptors(accImg, srcImg, maskImg, clipImg, clipOutImg, tileBuf);
        else
            writeDescriptors(accImg, srcImg, maskImg, clipImg, clipOutImg, tileBuf);

        vkResetCommandBuffer(m_cmd, 0);
        const VkCommandBufferBeginInfo cbi{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        vkBeginCommandBuffer(m_cmd, &cbi);

        const GpuImage* uploads[4] = {&accImg, &srcImg, &maskImg, &clipImg};
        const GpuBuffer* stages[4] = {&accStage, &srcStage, &maskStage, &clipStage};
        std::vector<VkImageMemoryBarrier> toDst;
        for (const GpuImage* img : uploads)
            toDst.push_back(imageBarrier(*img, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                         VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
        vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             static_cast<std::uint32_t>(toDst.size()), toDst.data());
        for (int i = 0; i < 4; ++i) {
            const VkBufferImageCopy copy{
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .imageExtent = {uploads[i]->width, uploads[i]->height, 1},
            };
            vkCmdCopyBufferToImage(m_cmd, stages[i]->buffer, uploads[i]->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }
        const VkImageMemoryBarrier toShader[5] = {
            imageBarrier(accImg, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            imageBarrier(srcImg, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            imageBarrier(maskImg, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            imageBarrier(clipImg, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            // Never uploaded, so it comes straight from UNDEFINED to the GENERAL layout a
            // storage image is written in.
            imageBarrier(clipOutImg, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_GENERAL),
        };
        vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 5,
                             toShader);

        const VkPipeline pipeline =
            indexed ? m_indexedPipeline
                    : (dispatch == render::TileDispatch::TileList ? m_listPipeline : m_pipeline);
        const VkPipelineLayout pipeLayout = indexed ? m_indexedPipeLayout : m_pipeLayout;
        vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        const VkDescriptorSet set = indexed ? m_indexedSet : m_descSet;
        vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &set, 0,
                                nullptr);

        const Step& st = step;
        const std::optional<Affine2D> invOpt = st.place.inverse();
        REQUIRE(invOpt.has_value());
        const Affine2D inv = *invOpt;
        const ResampleFilter filter = resolveTileFilter(st.filter, st.place, st.liveDrag);

        // Everything in the push block that does not depend on the tile.
        PushBlock base{};
        base.inv0[0] = static_cast<float>(inv.m00);
        base.inv0[1] = static_cast<float>(inv.m01);
        base.inv0[2] = static_cast<float>(inv.m02);
        base.inv1[0] = static_cast<float>(inv.m10);
        base.inv1[1] = static_cast<float>(inv.m11);
        base.inv1[2] = static_cast<float>(inv.m12);
        base.mask0[0] = static_cast<float>(st.maskXform.m00);
        base.mask0[1] = static_cast<float>(st.maskXform.m01);
        base.mask0[2] = static_cast<float>(st.maskXform.m02);
        base.mask1[0] = static_cast<float>(st.maskXform.m10);
        base.mask1[1] = static_cast<float>(st.maskXform.m11);
        base.mask1[2] = static_cast<float>(st.maskXform.m12);
        // The source window. Zero true-size = whole layer bound, which is what every case that
        // does not name a window passes.
        base.inv0[3] = static_cast<float>(window.whole() ? 0u : window.x);
        base.inv1[3] = static_cast<float>(window.whole() ? 0u : window.y);
        base.mask0[3] = static_cast<float>(window.whole() ? 0u : srcFull.width);
        base.mask1[3] = static_cast<float>(window.whole() ? 0u : srcFull.height);
        base.scale[0] = static_cast<float>(std::max(1.0, std::hypot(inv.m00, inv.m10)));
        base.scale[1] = static_cast<float>(std::max(1.0, std::hypot(inv.m01, inv.m11)));
        base.filter = static_cast<std::int32_t>(filter);
        base.superN = supersampleN(inv);
        base.blend = static_cast<std::int32_t>(st.blend);
        base.opacity = st.opacity;
        base.maskMode = st.maskMode;
        base.clipMode = st.clip ? 1 : 0;
        base.clipWrite = st.clipWrite ? 1 : 0;

        if (dispatch == render::TileDispatch::PerTile) {
            // One dispatch per macrotile. The tiles write disjoint regions of the accumulator, so
            // no barrier is needed between them -- which is the whole point of tiling: they are
            // independent work items, and this loop is what the residency layer drives.
            for (const TileRecord& r : tileRecords) {
                // Both affines stay in TARGET space; the tile origin travels as an integer, so
                // every sample point is evaluated at the same target pixel whatever the tiling.
                PushBlock pc = base;
                pc.tileOrigin[0] = r.tileOrigin[0];
                pc.tileOrigin[1] = r.tileOrigin[1];
                pc.accOrigin[0] = r.accOrigin[0];
                pc.accOrigin[1] = r.accOrigin[1];
                pc.extent[0] = r.extent[0];
                pc.extent[1] = r.extent[1];
                vkCmdPushConstants(m_cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc),
                                   &pc);
                vkCmdDispatch(m_cmd, (static_cast<std::uint32_t>(r.extent[0]) + kWorkgroup - 1) /
                                         kWorkgroup,
                              (static_cast<std::uint32_t>(r.extent[1]) + kWorkgroup - 1) /
                                  kWorkgroup,
                              1);
            }
        } else {
            // ONE dispatch for every macrotile: gl_WorkGroupID.z indexes the list. The indexed
            // variant additionally reads its layer's sheets out of the runtime array, which for a
            // single-layer lane like this one means slot 0 -- so uTileOrigin.x carries a 0 rather
            // than a tile origin.
            PushBlock pc = base;
            if (indexed) pc.tileOrigin[0] = 0;  // uLayer
            vkCmdPushConstants(m_cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vkCmdDispatch(m_cmd, (widest + kWorkgroup - 1) / kWorkgroup,
                          (tallest + kWorkgroup - 1) / kWorkgroup,
                          static_cast<std::uint32_t>(tileRecords.size()));
        }

        const VkImageMemoryBarrier toSrc =
            imageBarrier(accImg, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);
        const VkBufferImageCopy back{
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {accImg.width, accImg.height, 1},
        };
        vkCmdCopyImageToBuffer(m_cmd, accImg.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback.buffer, 1, &back);
        if (publish) {
            // GENERAL is a legal copy source, so the published clip base needs a memory barrier
            // rather than a transition.
            const VkImageMemoryBarrier clipToSrc =
                imageBarrier(clipOutImg, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &clipToSrc);
            const VkBufferImageCopy clipCopy{
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .imageExtent = {clipOutImg.width, clipOutImg.height, 1},
            };
            vkCmdCopyImageToBuffer(m_cmd, clipOutImg.image, VK_IMAGE_LAYOUT_GENERAL,
                                   clipBack.buffer, 1, &clipCopy);
        }
        const VkBufferMemoryBarrier toHost{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = readback.buffer,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                             0, nullptr, 1, &toHost, 0, nullptr);
        vkEndCommandBuffer(m_cmd);

        vkResetFences(dev, 1, &m_fence);
        const VkSubmitInfo si{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &m_cmd,
        };
        REQUIRE(m_ctx->submit(si, m_fence) == VK_SUCCESS);
        REQUIRE(vkWaitForFences(dev, 1, &m_fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS);

        ImageF out(acc.width, acc.height);
        const auto* raw = static_cast<const std::uint16_t*>(readback.mapped);
        for (std::size_t i = 0; i < out.rgba.size(); ++i) out.rgba[i] = halfToFloat(raw[i]);
        if (publish) {
            const auto* rawClip = static_cast<const std::uint16_t*>(clipBack.mapped);
            clipOut->resize(acc.pixelCount());
            for (std::size_t i = 0; i < clipOut->size(); ++i)
                (*clipOut)[i] = halfToFloat(rawClip[i]);
        }

        destroy(clipBack);
        destroy(clipOutImg);
        destroy(tileBuf);
        destroy(readback);
        destroy(clipStage);
        destroy(maskStage);
        destroy(srcStage);
        destroy(accStage);
        destroy(clipImg);
        destroy(maskImg);
        destroy(srcImg);
        destroy(accImg);
        return out;
    }

private:
    TileLane() = default;

    // The descriptor-indexed variant (S60-a item 10). Two caps questions, not one: the device must
    // be able to index descriptors AND to load the variant blob, which was compiled at a newer
    // SPIR-V target env than the floor set. A no here is silent -- haveIndexed() is false and the
    // cases that need it skip -- because the floor shapes draw the same picture.
    void initIndexed() {
        const render::GpuCaps& caps = m_ctx->caps();
        if (!caps.descriptorIndexing ||
            !caps.fitsSpirvVersion(mosaic::render::spirv::kVersion1_3))
            return;
        // This lane composites ONE layer at a time, so the array is two descriptors wide: the
        // layer's pixels at slot 0 and its mask at slot 1.
        constexpr std::uint32_t kSources = 2;
        if (!caps.fitsSampledImages(kSources + 1)) return;

        const VkDevice dev = m_ctx->device();
        const VkDescriptorSetLayoutBinding bindings[5] = {
            {kBindingAcc, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
             nullptr},
            {kBindingClip, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
             VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {kBindingClipOut, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
             nullptr},
            {kBindingTiles, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
             nullptr},
            {kBindingSources, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSources,
             VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        const VkDescriptorBindingFlags bindingFlags[5] = {
            0, 0, 0, 0,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT,
        };
        const VkDescriptorSetLayoutBindingFlagsCreateInfo bfci{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount = 5,
            .pBindingFlags = bindingFlags,
        };
        const VkDescriptorSetLayoutCreateInfo slci{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = &bfci,
            .bindingCount = 5,
            .pBindings = bindings,
        };
        if (vkCreateDescriptorSetLayout(dev, &slci, nullptr, &m_indexedSetLayout) != VK_SUCCESS)
            return;
        // The SAME push block: the variant spends the (now dead) per-tile lanes on a layer index
        // rather than growing the range past the 124 bytes the floor blob uses.
        const VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushBlock)};
        const VkPipelineLayoutCreateInfo plci{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &m_indexedSetLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pcr,
        };
        if (vkCreatePipelineLayout(dev, &plci, nullptr, &m_indexedPipeLayout) != VK_SUCCESS) return;
        const VkShaderModuleCreateInfo smci{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = mosaic::shaders::composite_tile_indexed_comp_size,
            .pCode = mosaic::shaders::composite_tile_indexed_comp,
        };
        if (vkCreateShaderModule(dev, &smci, nullptr, &m_indexedShader) != VK_SUCCESS) return;
        const VkComputePipelineCreateInfo cpci{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = m_indexedShader,
                      .pName = "main"},
            .layout = m_indexedPipeLayout,
        };
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_indexedPipeline) !=
            VK_SUCCESS)
            return;
        const VkDescriptorPoolSize sizes[3] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSources + 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        };
        const VkDescriptorPoolCreateInfo dpci{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1,
            .poolSizeCount = 3,
            .pPoolSizes = sizes,
        };
        if (vkCreateDescriptorPool(dev, &dpci, nullptr, &m_indexedPool) != VK_SUCCESS) {
            vkDestroyPipeline(dev, m_indexedPipeline, nullptr);
            m_indexedPipeline = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] bool formatOk(VkFormat fmt, VkFormatFeatureFlags want) const {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(m_ctx->physicalDevice(), fmt, &fp);
        return (fp.optimalTilingFeatures & want) == want;
    }

    GpuImage makeImage(std::uint32_t w, std::uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
        GpuImage out;
        out.width = w;
        out.height = h;
        const VkDevice dev = m_ctx->device();
        const VkImageCreateInfo ici{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = fmt,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        REQUIRE(vkCreateImage(dev, &ici, nullptr, &out.image) == VK_SUCCESS);
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(dev, out.image, &req);
        const std::uint32_t type =
            m_ctx->findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        REQUIRE(type != UINT32_MAX);
        const VkMemoryAllocateInfo mai{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size,
            .memoryTypeIndex = type,
        };
        REQUIRE(vkAllocateMemory(dev, &mai, nullptr, &out.memory) == VK_SUCCESS);
        REQUIRE(vkBindImageMemory(dev, out.image, out.memory, 0) == VK_SUCCESS);
        const VkImageViewCreateInfo vci{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = out.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = fmt,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        REQUIRE(vkCreateImageView(dev, &vci, nullptr, &out.view) == VK_SUCCESS);
        return out;
    }

    GpuBuffer makeHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
        GpuBuffer out;
        const VkDevice dev = m_ctx->device();
        const VkBufferCreateInfo bci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = std::max<VkDeviceSize>(size, 4),
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        REQUIRE(vkCreateBuffer(dev, &bci, nullptr, &out.buffer) == VK_SUCCESS);
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(dev, out.buffer, &req);
        const std::uint32_t type = m_ctx->findMemoryType(
            req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        REQUIRE(type != UINT32_MAX);
        const VkMemoryAllocateInfo mai{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size,
            .memoryTypeIndex = type,
        };
        REQUIRE(vkAllocateMemory(dev, &mai, nullptr, &out.memory) == VK_SUCCESS);
        REQUIRE(vkBindBufferMemory(dev, out.buffer, out.memory, 0) == VK_SUCCESS);
        REQUIRE(vkMapMemory(dev, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) == VK_SUCCESS);
        return out;
    }

    void destroy(GpuImage& img) {
        const VkDevice dev = m_ctx->device();
        if (img.view != VK_NULL_HANDLE) vkDestroyImageView(dev, img.view, nullptr);
        if (img.image != VK_NULL_HANDLE) vkDestroyImage(dev, img.image, nullptr);
        if (img.memory != VK_NULL_HANDLE) vkFreeMemory(dev, img.memory, nullptr);
        img = {};
    }

    void destroy(GpuBuffer& buf) {
        const VkDevice dev = m_ctx->device();
        if (buf.memory != VK_NULL_HANDLE) vkUnmapMemory(dev, buf.memory);
        if (buf.buffer != VK_NULL_HANDLE) vkDestroyBuffer(dev, buf.buffer, nullptr);
        if (buf.memory != VK_NULL_HANDLE) vkFreeMemory(dev, buf.memory, nullptr);
        buf = {};
    }

    static VkImageMemoryBarrier imageBarrier(const GpuImage& img, VkAccessFlags srcA,
                                             VkAccessFlags dstA, VkImageLayout oldL,
                                             VkImageLayout newL) {
        return VkImageMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = srcA,
            .dstAccessMask = dstA,
            .oldLayout = oldL,
            .newLayout = newL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = img.image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
    }

    void writeDescriptors(const GpuImage& acc, const GpuImage& src, const GpuImage& mask,
                          const GpuImage& clip, const GpuImage& clipOut, const GpuBuffer& tiles) {
        // Recycle the set from scratch every call: the previous call's image views were destroyed
        // once its fence signalled, and a freshly allocated set cannot be holding stale handles.
        const VkDevice dev = m_ctx->device();
        REQUIRE(vkResetDescriptorPool(dev, m_descPool, 0) == VK_SUCCESS);
        const VkDescriptorSetAllocateInfo dsai{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = m_descPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &m_setLayout,
        };
        REQUIRE(vkAllocateDescriptorSets(dev, &dsai, &m_descSet) == VK_SUCCESS);
        const VkDescriptorImageInfo accInfo{VK_NULL_HANDLE, acc.view, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo srcInfo{m_sampler, src.view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo maskInfo{m_sampler, mask.view,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo clipInfo{m_sampler, clip.view,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo clipOutInfo{VK_NULL_HANDLE, clipOut.view,
                                                VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorBufferInfo tileInfo{tiles.buffer, 0, VK_WHOLE_SIZE};
        const VkWriteDescriptorSet writes[6] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = m_descSet,
             .dstBinding = kBindingAcc,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .pImageInfo = &accInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = m_descSet,
             .dstBinding = kBindingSrc,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &srcInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = m_descSet,
             .dstBinding = kBindingMask,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &maskInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = m_descSet,
             .dstBinding = kBindingClip,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &clipInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = m_descSet,
             .dstBinding = kBindingClipOut,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .pImageInfo = &clipOutInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = m_descSet,
             .dstBinding = kBindingTiles,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .pBufferInfo = &tileInfo},
        };
        vkUpdateDescriptorSets(dev, 6, writes, 0, nullptr);
    }

    // The indexed variant's ONE set: no per-layer bindings at all, the layer's two sheets sitting
    // in the runtime array instead. The clip base is bound to both its bindings for the same
    // reason the production lane does it -- one set has to serve a layer that reads a clip base
    // and one that publishes one, and no layer does both.
    void writeIndexedDescriptors(const GpuImage& acc, const GpuImage& src, const GpuImage& mask,
                                 const GpuImage& clip, const GpuImage& clipOut,
                                 const GpuBuffer& tiles) {
        const VkDevice dev = m_ctx->device();
        REQUIRE(vkResetDescriptorPool(dev, m_indexedPool, 0) == VK_SUCCESS);
        const std::uint32_t sources = 2;
        const VkDescriptorSetVariableDescriptorCountAllocateInfo vdcai{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
            .descriptorSetCount = 1,
            .pDescriptorCounts = &sources,
        };
        const VkDescriptorSetAllocateInfo dsai{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .pNext = &vdcai,
            .descriptorPool = m_indexedPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &m_indexedSetLayout,
        };
        REQUIRE(vkAllocateDescriptorSets(dev, &dsai, &m_indexedSet) == VK_SUCCESS);
        const VkDescriptorImageInfo accInfo{VK_NULL_HANDLE, acc.view, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo clipInfo{m_sampler, clip.view,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo clipOutInfo{VK_NULL_HANDLE, clipOut.view,
                                                VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorBufferInfo tileInfo{tiles.buffer, 0, VK_WHOLE_SIZE};
        const VkDescriptorImageInfo sourceInfos[2] = {
            {m_sampler, src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {m_sampler, mask.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        };
        const VkWriteDescriptorSet writes[5] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = m_indexedSet,
             .dstBinding = kBindingAcc,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .pImageInfo = &accInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = m_indexedSet,
             .dstBinding = kBindingClip,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &clipInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = m_indexedSet,
             .dstBinding = kBindingClipOut,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .pImageInfo = &clipOutInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = m_indexedSet,
             .dstBinding = kBindingTiles,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .pBufferInfo = &tileInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = m_indexedSet,
             .dstBinding = kBindingSources,
             .descriptorCount = 2,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = sourceInfos},
        };
        vkUpdateDescriptorSets(dev, 5, writes, 0, nullptr);
    }

    std::shared_ptr<render::VulkanContext> m_ctx;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipeLayout = VK_NULL_HANDLE;
    VkShaderModule m_shader = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;      // kTileList = 0, the reference shape
    VkPipeline m_listPipeline = VK_NULL_HANDLE;  // kTileList = 1, the same module
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descSet = VK_NULL_HANDLE;
    // The descriptor-indexed variant. All null on a device that cannot have it -- an ordinary
    // outcome, never a failure.
    VkDescriptorSetLayout m_indexedSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_indexedPipeLayout = VK_NULL_HANDLE;
    VkShaderModule m_indexedShader = VK_NULL_HANDLE;
    VkPipeline m_indexedPipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_indexedPool = VK_NULL_HANDLE;
    VkDescriptorSet m_indexedSet = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;
};

// ---------------------------------------------------------------------------------------------
// The scene: an 8-bit background, then one layer under test. Deliberately 200x140 -- neither
// dimension is a multiple of the tile sizes used below, so the right/bottom/corner tiles are
// PARTIAL on every run.
// ---------------------------------------------------------------------------------------------
constexpr std::uint32_t kCanvasW = 200;
constexpr std::uint32_t kCanvasH = 140;

// A structured backdrop: two gradients crossing, a hard-edged block, and a fully transparent
// band carrying non-zero RGB (the straight-vs-premultiplied tripwire in the BACKDROP -- an alpha
// of 0 must contribute nothing whatever its colour says).
Image backgroundImage() {
    Image img(kCanvasW, kCanvasH);
    for (std::uint32_t y = 0; y < kCanvasH; ++y)
        for (std::uint32_t x = 0; x < kCanvasW; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * kCanvasW + x) * 4;
            img.rgba[p + 0] = static_cast<std::uint8_t>(x * 255 / (kCanvasW - 1));
            img.rgba[p + 1] = static_cast<std::uint8_t>(y * 255 / (kCanvasH - 1));
            img.rgba[p + 2] = static_cast<std::uint8_t>((x + 2 * y) % 256);
            img.rgba[p + 3] = 255;
            if (x >= 150 && y >= 100) {  // a partially transparent corner
                img.rgba[p + 3] = static_cast<std::uint8_t>(40 + (x % 7) * 20);
            }
            if (y >= 20 && y < 28) {  // transparent band with junk RGB
                img.rgba[p + 0] = 210;
                img.rgba[p + 1] = 15;
                img.rgba[p + 2] = 90;
                img.rgba[p + 3] = 0;
            }
        }
    return img;
}

// The layer under test: gradients, a saturated disc, an alpha ramp, an opaque black/white pair
// (the reciprocal blend modes' knife edges), and a transparent block with junk RGB -- any lane
// that filters STRAIGHT colour instead of premultiplied drags that junk into its neighbours and
// fails loudly.
Image sourceImage(std::uint32_t w = 96, std::uint32_t h = 72) {
    Image img(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
            std::uint8_t r = static_cast<std::uint8_t>(255 - x * 255 / (w - 1));
            std::uint8_t g = static_cast<std::uint8_t>(30 + y * 200 / (h - 1));
            std::uint8_t b = static_cast<std::uint8_t>((3 * x + y) % 256);
            std::uint8_t a = 255;
            const double dx = static_cast<double>(x) - 34.0;
            const double dy = static_cast<double>(y) - 30.0;
            if (dx * dx + dy * dy < 196.0) {
                r = 250;
                g = 20;
                b = 240;
            }
            if (x < 6 && y < 6) { r = g = b = 0; }        // pure black
            if (x >= 6 && x < 12 && y < 6) { r = g = b = 255; }  // pure white
            if (x >= w - 24) a = static_cast<std::uint8_t>((w - x) * 255 / 24);
            if (x >= 20 && x < 34 && y >= 50 && y < 62) {
                r = 200;
                g = 8;
                b = 130;
                a = 0;
            }
            img.rgba[p + 0] = r;
            img.rgba[p + 1] = g;
            img.rgba[p + 2] = b;
            img.rgba[p + 3] = a;
        }
    return img;
}

struct MaskSpec {
    bool present = false;
    bool linked = true;
    bool enabled = true;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// A smooth-plus-hard coverage field, so both the ramp and the binary edges are exercised.
std::vector<std::uint8_t> maskCoverage(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> cov(static_cast<std::size_t>(w) * h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint32_t v = (x * 255) / (w > 1 ? w - 1 : 1);
            if (y * 3 < h) v = 255;
            if (x * 4 > 3 * w) v = 0;
            cov[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>(v);
        }
    return cov;
}

struct Scenario {
    Image background = backgroundImage();
    Image source = sourceImage();
    Affine2D place = Affine2D::identity();
    ResampleFilter filter = ResampleFilter::Nearest;
    bool liveDrag = false;
    BlendMode blend = BlendMode::Normal;
    float opacity = 1.0f;
    MaskSpec mask;
    bool clip = false;
    std::uint32_t tileSize = 64;
    // How the layer's tile is stored on the device. rgba16f is the working format (§2.4); the
    // R8G8B8A8_UNORM axis is today's `core::RasterLayer` storage uploaded verbatim.
    VkFormat srcFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    // Bind only this sub-rect of the layer instead of all of it (S60-a). Default = the whole
    // layer, which is what every pre-existing scenario passes.
    SrcWindow srcWindow;
    // Which DISPATCH SHAPE composites it (S60-a item 10). PerTile is the reference every
    // pre-existing scenario runs; the other two must produce the same bytes, not merely the same
    // picture within a tolerance.
    render::TileDispatch dispatch = render::TileDispatch::PerTile;
};

struct Diff {
    int maxLsb = 0;
    long over = 0;    // pixels where any channel differs by more than 1 LSB
    double meanLsb = 0.0;
    long pixels = 0;
};

Diff compare(const Image& cpu, const Image& gpu) {
    Diff d;
    REQUIRE(cpu.width == gpu.width);
    REQUIRE(cpu.height == gpu.height);
    d.pixels = static_cast<long>(cpu.pixelCount());
    double sum = 0.0;
    for (std::size_t i = 0; i < cpu.rgba.size(); i += 4) {
        int worst = 0;
        for (std::size_t c = 0; c < 4; ++c) {
            const int delta = std::abs(static_cast<int>(cpu.rgba[i + c]) -
                                       static_cast<int>(gpu.rgba[i + c]));
            sum += delta;
            worst = std::max(worst, delta);
        }
        d.maxLsb = std::max(d.maxLsb, worst);
        if (worst > 1) ++d.over;
    }
    d.meanLsb = sum / static_cast<double>(cpu.rgba.size());
    return d;
}

// Everything a scenario needs on BOTH sides, computed once. Split out of runScenario so the same
// scene can be composited by two dispatch shapes and the two results compared to each other byte
// for byte -- a comparison that is meaningless if the seed accumulator or the step differ at all.
struct Prepared {
    Image bgFlat;
    Image cpuFlat;
    Step step;
    MaskData mask;
    std::vector<float> clipBase;
};

Prepared prepare(const Scenario& sc) {
    render::CompositeOptions opts;
    opts.resampleFilter = sc.filter;
    opts.liveDrag = sc.liveDrag;

    // The CPU accumulator entering the layer under test: the background alone, composited by the
    // reference. An 8-bit background over a transparent accumulator round-trips exactly, so this
    // is the very buffer the walk hands to the layer step.
    Image bgFlat;
    {
        core::Document bgDoc(kCanvasW, kCanvasH);
        auto bg = bgDoc.makeRaster("bg", kCanvasW, kCanvasH);
        bg->image() = sc.background;
        bgDoc.root().addOnTop(std::move(bg));
        const render::CompositeResult r = render::composite(bgDoc, opts, render::Backend::Cpu);
        REQUIRE(r.ok);
        bgFlat = r.image;
    }

    // The reference: the whole two-layer document, CPU path.
    Image cpuFlat;
    {
        core::Document doc(kCanvasW, kCanvasH);
        auto bg = doc.makeRaster("bg", kCanvasW, kCanvasH);
        bg->image() = sc.background;
        doc.root().addOnTop(std::move(bg));
        auto top = doc.makeRaster("top", sc.source.width, sc.source.height);
        top->image() = sc.source;
        top->setTransform(sc.place);
        top->setBlendMode(sc.blend);
        top->setOpacity(sc.opacity);
        top->setClipToBelow(sc.clip);
        if (sc.mask.present) {
            core::RasterMask m(sc.mask.width, sc.mask.height);
            m.coverage = maskCoverage(sc.mask.width, sc.mask.height);
            m.linked = sc.mask.linked;
            m.enabled = sc.mask.enabled;
            top->setMask(std::move(m));
        }
        doc.root().addOnTop(std::move(top));
        const render::CompositeResult r = render::composite(doc, opts, render::Backend::Cpu);
        REQUIRE(r.ok);
        REQUIRE(r.usedBackend == render::Backend::Cpu);
        cpuFlat = r.image;
    }

    Step step;
    step.place = sc.place;
    step.filter = sc.filter;
    step.liveDrag = sc.liveDrag;
    step.blend = sc.blend;
    step.opacity = sc.opacity;
    step.clip = sc.clip;
    MaskData mask;
    if (sc.mask.present && sc.mask.enabled) {
        mask.width = sc.mask.width;
        mask.height = sc.mask.height;
        mask.coverage = maskCoverage(sc.mask.width, sc.mask.height);
        // A LINKED mask on a leaf folds in SOURCE space, proportionally (mode 1); an UNLINKED one
        // folds after placement in PARENT space, which at the document root is the identity map
        // (mode 3) -- foldUnlinkedMask samples through pre^-1, and pre is the identity here.
        step.maskMode = sc.mask.linked ? 1 : 3;
        step.maskXform = Affine2D::identity();
    }
    std::vector<float> clipBase;
    if (sc.clip) {
        // The clip base is the alpha of the nearest non-clipped layer beneath -- here the
        // background's own rendered alpha, which the accumulator carries verbatim.
        clipBase.resize(bgFlat.pixelCount());
        for (std::size_t i = 0; i < clipBase.size(); ++i)
            clipBase[i] = static_cast<float>(bgFlat.rgba[i * 4 + 3]) / 255.0f;
    }

    return Prepared{std::move(bgFlat), std::move(cpuFlat), step, std::move(mask),
                    std::move(clipBase)};
}

// The GPU half on its own, returning the raw accumulator: what two dispatch shapes are compared on.
ImageF runShape(TileLane& lane, const Scenario& sc, const Prepared& p,
                render::TileDispatch dispatch) {
    return lane.composite(mosaic::common::toFloat(p.bgFlat), mosaic::common::toFloat(sc.source),
                          p.mask, p.clipBase, p.step, sc.tileSize, sc.srcFormat, sc.srcWindow,
                          nullptr, dispatch);
}

// Run one scenario end to end: CPU reference vs the tile kernel.
Diff runScenario(TileLane& lane, const Scenario& sc) {
    const Prepared p = prepare(sc);
    return compare(p.cpuFlat, mosaic::common::toImage8(runShape(lane, sc, p, sc.dispatch)));
}

// The parity gate. 1 LSB == 1/255, the tolerance the existing GPU parity tests use. `slack` is
// the number of pixels (out of 28000) allowed to exceed it; every axis below asks for 0 except
// the two KNIFE-EDGE samplers under a rotation, where the CPU reference itself is discontinuous
// -- see the Nearest/Area case for the argument.
void expectParity(const Diff& d, const char* label, long slack = 0) {
    // MOSAIC_TILE_PARITY_VERBOSE=1 prints the measured drift for EVERY case, not just failures --
    // how the numbers quoted in the session report were gathered.
    static const bool verbose = std::getenv("MOSAIC_TILE_PARITY_VERBOSE") != nullptr;
    const std::string line = std::string(label) + ": maxLsb " + std::to_string(d.maxLsb) +
                             ", over-1-LSB " + std::to_string(d.over) + "/" +
                             std::to_string(d.pixels) + ", meanLsb " + std::to_string(d.meanLsb);
    if (verbose) MESSAGE(line);
    INFO(line);
    CHECK(d.over <= slack);
    // Even where a few pixels are allowed past 1 LSB, cap how far past: a slack budget is for
    // last-bit rounding, never a licence for a wrong formula to hide in four pixels.
    CHECK(d.maxLsb <= (slack == 0 ? 1 : 3));
}

// The blend modes whose formula divides by a source-derived quantity -- the only ones where an
// fp16 source tile's coarser grid near 1.0 can move the 8-bit result by more than one step.
bool isReciprocal(BlendMode m) {
    return m == BlendMode::ColorBurn || m == BlendMode::ColorDodge ||
           m == BlendMode::VividLight || m == BlendMode::Divide;
}
// Pixels (out of 28000) those modes may exceed 1 LSB by when the source tile is fp16 and the
// kernel overshoots toward 1.0. Measured 1 on the dev GPU; the rest is portability headroom.
// Everything else in this file asks for ZERO.
constexpr long kFp16ReciprocalSlack = 4;

std::unique_ptr<TileLane> makeLane(const char* who) {
    std::string err;
    auto lane = TileLane::create(err);
    if (!lane) {
        const std::string why =
            std::string("no usable Vulkan device -- skipping ") + who + " (" + err + ")";
        WARN_MESSAGE(true, why);
    }
    return lane;
}

Affine2D rotateAbout(double radians, double cx, double cy) {
    return Affine2D::translation(cx, cy) * Affine2D::rotation(radians) *
           Affine2D::translation(-cx, -cy);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Blend modes: all 23, the four non-separable HSL ones included.
// ---------------------------------------------------------------------------------------------
TEST_CASE("the fused tile kernel matches the CPU reference for every blend mode") {
    auto lane = makeLane("blend-mode parity");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    for (int m = 0; m <= static_cast<int>(BlendMode::Luminosity); ++m) {
        Scenario sc;
        sc.blend = static_cast<BlendMode>(m);
        // An integer placement so the resample is an exact copy: this case isolates the BLEND.
        sc.place = Affine2D::translation(37, 21);
        const Diff d = runScenario(*lane, sc);
        expectParity(d, std::string(core::blendModeName(sc.blend)).c_str());
    }
    CHECK(lane->validationErrors() == before);
}

TEST_CASE("every blend mode survives being fused with a rotation and a mask") {
    auto lane = makeLane("fused blend parity");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    for (int m = 0; m <= static_cast<int>(BlendMode::Luminosity); ++m) {
        Scenario sc;
        sc.blend = static_cast<BlendMode>(m);
        sc.place = rotateAbout(0.37, 48.0, 36.0) * Affine2D::translation(52.4, 33.6);
        sc.filter = ResampleFilter::Lanczos3;
        sc.opacity = 0.73f;
        sc.mask = {true, true, true, 96, 72};
        const Diff d = runScenario(*lane, sc);
        // The four reciprocal modes divide by (1-s) or s, and Lanczos3 overshoots the source
        // toward (and past) 1.0 where an fp16 tile's 2^-11 spacing is coarser than the 8-bit
        // grid underneath -- see "the kernel is source-format-agnostic" for the mechanism and
        // the measurement. Measured 0 here; the slack is portability headroom, not a licence.
        expectParity(d, std::string(core::blendModeName(sc.blend)).c_str(),
                     isReciprocal(sc.blend) ? kFp16ReciprocalSlack : 0);
    }
    CHECK(lane->validationErrors() == before);
}

// ---------------------------------------------------------------------------------------------
// Resample filters x transforms.
// ---------------------------------------------------------------------------------------------
TEST_CASE("every resample filter matches the CPU kernels under rotation and scale") {
    auto lane = makeLane("resample parity");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    // The CONTINUOUS kernels: a float-vs-double difference in the inverse-mapped sample point
    // moves the weights continuously, so the output moves by ~nothing. Zero slack.
    const ResampleFilter smooth[] = {
        ResampleFilter::Bilinear, ResampleFilter::Bicubic,  ResampleFilter::Mitchell,
        ResampleFilter::Lanczos2, ResampleFilter::Lanczos3, ResampleFilter::Gaussian,
        ResampleFilter::Supersample,
    };
    struct Placement {
        const char* name;
        Affine2D t;
    };
    const Placement placements[] = {
        {"sub-pixel translate", Affine2D::translation(41.37, 22.63)},
        {"rotate 0.4", rotateAbout(0.4, 48.0, 36.0) * Affine2D::translation(50.0, 30.0)},
        {"scale up 1.7 x 1.3", Affine2D::translation(9.0, 5.0) * Affine2D::scaling(1.7, 1.3)},
        {"scale down 0.45 x 0.6",
         Affine2D::translation(60.5, 40.25) * Affine2D::scaling(0.45, 0.6)},
        {"mirror x", Affine2D::translation(150.0, 30.0) * Affine2D::scaling(-1.0, 1.0)},
        {"rotate + anisotropic scale + sub-pixel",
         Affine2D::translation(60.5, 60.25) * Affine2D::rotation(-0.6) *
             Affine2D::scaling(1.0, 0.8)},
    };
    for (const Placement& p : placements)
        for (const ResampleFilter f : smooth) {
            Scenario sc;
            sc.place = p.t;
            sc.filter = f;
            const Diff d = runScenario(*lane, sc);
            const std::string label =
                std::string(p.name) + " / " + std::string(render::resampleFilterName(f));
            expectParity(d, label.c_str());
        }
    CHECK(lane->validationErrors() == before);
}

TEST_CASE("the knife-edge samplers (Nearest, Area) match on grid-aligned placements") {
    auto lane = makeLane("nearest/area parity");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    // Nearest and Area are the two DISCONTINUOUS kernels: Nearest picks one texel, Area's box
    // weight steps 1 -> 0. On these placements the inverse-mapped sample points sit far from any
    // texel boundary, so the discontinuity is never straddled and the match is exact.
    struct Placement {
        const char* name;
        Affine2D t;
    };
    const Placement placements[] = {
        {"identity", Affine2D::identity()},
        {"integer translate", Affine2D::translation(37.0, 21.0)},
        {"sub-pixel translate 0.37", Affine2D::translation(41.37, 22.63)},
        // An EXACT quarter turn: Affine2D::rotation(pi/2) would carry cos(pi/2) == 6.1e-17, which
        // is neither a lossless grid nor grid-aligned, so the matrix is written out by hand.
        {"quarter turn", Affine2D{0.0, -1.0, 120.0, 1.0, 0.0, 12.0}},
        {"integer scale 2x", Affine2D::translation(8.0, 4.0) * Affine2D::scaling(2.0, 2.0)},
        {"mirror x, integer", Affine2D::translation(150.0, 30.0) * Affine2D::scaling(-1.0, 1.0)},
    };
    for (const Placement& p : placements)
        for (const ResampleFilter f : {ResampleFilter::Nearest, ResampleFilter::Area}) {
            Scenario sc;
            sc.place = p.t;
            sc.filter = f;
            const Diff d = runScenario(*lane, sc);
            const std::string label =
                std::string(p.name) + " / " + std::string(render::resampleFilterName(f));
            expectParity(d, label.c_str());
        }
    CHECK(lane->validationErrors() == before);
}

TEST_CASE("Nearest under a rotation differs only where the CPU reference is itself knife-edge") {
    auto lane = makeLane("nearest rotation parity");
    if (!lane) return;
    // Under a rotation, Nearest's floor() lands within a float ULP of a texel boundary for a
    // handful of pixels, and the two lanes then legitimately pick DIFFERENT source texels (the
    // CPU maps in double, the GPU in float -- Vulkan 1.0 has no fp64). That is a property of
    // point sampling, not of this kernel, so the assertion is a bound on HOW MANY pixels may do
    // it: well under 0.5% of the canvas, and the mean stays at float noise.
    Scenario sc;
    sc.place = rotateAbout(0.4, 48.0, 36.0) * Affine2D::translation(50.0, 30.0);
    sc.filter = ResampleFilter::Nearest;
    const Diff d = runScenario(*lane, sc);
    MESSAGE("nearest+rotate: maxLsb " << d.maxLsb << ", over-1-LSB " << d.over << "/" << d.pixels
                                      << ", meanLsb " << d.meanLsb);
    CHECK(static_cast<double>(d.over) < 0.005 * static_cast<double>(d.pixels));
    CHECK(d.meanLsb < 0.05);
}

TEST_CASE("ResampleFilter::Auto resolves to the same kernel on both lanes") {
    auto lane = makeLane("auto filter parity");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    struct Placement {
        const char* name;
        Affine2D t;
        bool liveDrag;
    };
    const Placement placements[] = {
        {"lossless grid -> Nearest", Affine2D::translation(37.0, 21.0), false},
        {"live drag -> Bilinear",
         rotateAbout(0.3, 48.0, 36.0) * Affine2D::translation(50.0, 30.0), true},
        {"minify -> Area", Affine2D::translation(60.5, 40.0) * Affine2D::scaling(0.5, 0.5), false},
        {"enlarge -> Lanczos3", Affine2D::translation(9.0, 5.0) * Affine2D::scaling(1.6, 1.6),
         false},
    };
    for (const Placement& p : placements) {
        Scenario sc;
        sc.place = p.t;
        sc.filter = ResampleFilter::Auto;
        sc.liveDrag = p.liveDrag;
        const Diff d = runScenario(*lane, sc);
        expectParity(d, p.name);
    }
    CHECK(lane->validationErrors() == before);
}

// ---------------------------------------------------------------------------------------------
// Masks, clip, opacity.
// ---------------------------------------------------------------------------------------------
TEST_CASE("the mask fold matches: linked, unlinked, absent, disabled") {
    auto lane = makeLane("mask parity");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    {  // no mask at all
        Scenario sc;
        sc.place = Affine2D::translation(41.37, 22.63);
        sc.filter = ResampleFilter::Bicubic;
        expectParity(runScenario(*lane, sc), "no mask");
    }
    {  // linked, same resolution as the source: folds at the source pixel, before the transform
        Scenario sc;
        sc.place = rotateAbout(0.35, 48.0, 36.0) * Affine2D::translation(52.0, 30.0);
        sc.filter = ResampleFilter::Lanczos3;
        sc.mask = {true, true, true, 96, 72};
        expectParity(runScenario(*lane, sc), "linked mask, 1:1");
    }
    {  // linked, LOWER resolution: the CPU maps sx*maskW/srcW in integers; so must the shader
        Scenario sc;
        sc.place = Affine2D::translation(30.0, 18.0) * Affine2D::scaling(1.4, 1.1);
        sc.filter = ResampleFilter::Mitchell;
        sc.mask = {true, true, true, 37, 29};
        expectParity(runScenario(*lane, sc), "linked mask, 37x29 under a 96x72 source");
    }
    {  // linked, HIGHER resolution than the source
        Scenario sc;
        sc.place = Affine2D::translation(24.0, 12.0);
        sc.filter = ResampleFilter::Nearest;
        sc.mask = {true, true, true, 151, 113};
        expectParity(runScenario(*lane, sc), "linked mask, 151x113 over a 96x72 source");
    }
    {  // unlinked: fixed in PARENT space, folded after placement; zero coverage past the sheet
        Scenario sc;
        sc.place = rotateAbout(0.5, 48.0, 36.0) * Affine2D::translation(55.0, 28.0);
        sc.filter = ResampleFilter::Bilinear;
        sc.mask = {true, false, true, 130, 96};
        expectParity(runScenario(*lane, sc), "unlinked mask, smaller than the canvas");
    }
    {  // unlinked, canvas-sized, with a sub-pixel placement
        Scenario sc;
        sc.place = Affine2D::translation(41.37, 22.63);
        sc.filter = ResampleFilter::Lanczos2;
        sc.mask = {true, false, true, kCanvasW, kCanvasH};
        expectParity(runScenario(*lane, sc), "unlinked mask, canvas-sized");
    }
    {  // disabled: ignored by both lanes, whether linked or not
        Scenario sc;
        sc.place = Affine2D::translation(41.37, 22.63);
        sc.filter = ResampleFilter::Bicubic;
        sc.mask = {true, true, false, 96, 72};
        expectParity(runScenario(*lane, sc), "disabled linked mask");
        sc.mask = {true, false, false, 130, 96};
        expectParity(runScenario(*lane, sc), "disabled unlinked mask");
    }
    CHECK(lane->validationErrors() == before);
}

TEST_CASE("clip-to-below and opacity fold into the same dispatch") {
    auto lane = makeLane("clip/opacity parity");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    for (const float opacity : {0.0f, 0.37f, 1.0f})
        for (const bool clip : {false, true}) {
            Scenario sc;
            sc.place = rotateAbout(0.28, 48.0, 36.0) * Affine2D::translation(48.0, 26.0);
            sc.filter = ResampleFilter::Bilinear;
            sc.blend = BlendMode::Overlay;
            sc.opacity = opacity;
            sc.clip = clip;
            const std::string label = "opacity " + std::to_string(opacity) +
                                      (clip ? ", clipped" : ", unclipped");
            expectParity(runScenario(*lane, sc), label.c_str());
        }

    {  // clip + mask + a non-trivial blend, all in one dispatch: the whole fused step at once
        Scenario sc;
        sc.place = rotateAbout(-0.44, 48.0, 36.0) * Affine2D::translation(46.5, 24.25) *
                   Affine2D::scaling(1.25, 0.9);
        sc.filter = ResampleFilter::Lanczos3;
        sc.blend = BlendMode::SoftLight;
        sc.opacity = 0.61f;
        sc.mask = {true, true, true, 96, 72};
        sc.clip = true;
        expectParity(runScenario(*lane, sc), "transform + mask + clip + blend, fused");
    }
    CHECK(lane->validationErrors() == before);
}

// The clip base must never leave the device. compositor.cpp's walkStep captures the PLACED alpha
// of each non-clipped layer (`st.clipBase[p] = src.rgba[p * 4 + 3]`) for the clipped layers above
// it; the kernel publishes exactly that to uClipOut, so a whole clip run composites in one submit.
// Reading it back to the host per layer would reinstate the round trip residency exists to remove
// -- which is why this is a kernel output rather than a host computation.
TEST_CASE("the clip base round-trips on the device: publish, then clip against it") {
    auto lane = makeLane("clip base publish");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    const Image bg = backgroundImage();
    const Image baseSrc = sourceImage();
    const Image topSrc = sourceImage(70, 54);
    const Affine2D basePlace = Affine2D::translation(20.0, 15.0);
    const Affine2D topPlace = Affine2D::translation(45.0, 30.0);

    render::CompositeOptions opts;
    opts.resampleFilter = ResampleFilter::Nearest;

    Image bgFlat;
    {
        core::Document d(kCanvasW, kCanvasH);
        auto l = d.makeRaster("bg", kCanvasW, kCanvasH);
        l->image() = bg;
        d.root().addOnTop(std::move(l));
        const render::CompositeResult r = render::composite(d, opts, render::Backend::Cpu);
        REQUIRE(r.ok);
        bgFlat = r.image;
    }

    Image cpuFlat;
    {
        core::Document d(kCanvasW, kCanvasH);
        auto l = d.makeRaster("bg", kCanvasW, kCanvasH);
        l->image() = bg;
        d.root().addOnTop(std::move(l));
        auto base = d.makeRaster("base", baseSrc.width, baseSrc.height);
        base->image() = baseSrc;
        base->setTransform(basePlace);
        d.root().addOnTop(std::move(base));
        auto top = d.makeRaster("top", topSrc.width, topSrc.height);
        top->image() = topSrc;
        top->setTransform(topPlace);
        top->setClipToBelow(true);
        top->setBlendMode(BlendMode::Multiply);
        top->setOpacity(0.82f);
        d.root().addOnTop(std::move(top));
        const render::CompositeResult r = render::composite(d, opts, render::Backend::Cpu);
        REQUIRE(r.ok);
        cpuFlat = r.image;
    }

    // 1. the base layer composites AND publishes its own placed alpha.
    Step baseStep;
    baseStep.place = basePlace;
    baseStep.filter = ResampleFilter::Nearest;
    baseStep.clipWrite = true;
    std::vector<float> published;
    const ImageF afterBase =
        lane->composite(mosaic::common::toFloat(bgFlat), mosaic::common::toFloat(baseSrc),
                        MaskData{}, {}, baseStep, 64, VK_FORMAT_R8G8B8A8_UNORM, SrcWindow{},
                        &published);
    REQUIRE(published.size() == static_cast<std::size_t>(kCanvasW) * kCanvasH);

    // Under an integer translation with Nearest the placed alpha is the source alpha shifted, so
    // the published buffer can be checked against the layer by hand -- including the region the
    // layer MISSES, which must publish 0 rather than keep whatever was there before.
    double worst = 0.0;
    for (std::uint32_t y = 0; y < kCanvasH; ++y)
        for (std::uint32_t x = 0; x < kCanvasW; ++x) {
            const long sx = static_cast<long>(x) - 20;
            const long sy = static_cast<long>(y) - 15;
            const bool inside = sx >= 0 && sy >= 0 && sx < static_cast<long>(baseSrc.width) &&
                                sy < static_cast<long>(baseSrc.height);
            const float want =
                inside ? static_cast<float>(
                             baseSrc.rgba[(static_cast<std::size_t>(sy) * baseSrc.width +
                                           static_cast<std::size_t>(sx)) * 4 + 3]) / 255.0f
                       : 0.0f;
            const float got = published[static_cast<std::size_t>(y) * kCanvasW + x];
            worst = std::max(worst, static_cast<double>(std::fabs(got - want)));
        }
    INFO("worst published clip-base error: " << worst);
    CHECK(worst < 1.0 / 255.0);

    // 2. the clipped layer reads it back -- and the two dispatches together must equal the CPU
    //    walk's three-layer composite.
    Step topStep;
    topStep.place = topPlace;
    topStep.filter = ResampleFilter::Nearest;
    topStep.blend = BlendMode::Multiply;
    topStep.opacity = 0.82f;
    topStep.clip = true;
    const ImageF gpuAcc = lane->composite(afterBase, mosaic::common::toFloat(topSrc), MaskData{},
                                          published, topStep, 64, VK_FORMAT_R8G8B8A8_UNORM);
    expectParity(compare(cpuFlat, mosaic::common::toImage8(gpuAcc)), "clip base round trip");
    CHECK(lane->validationErrors() == before);
}

// ---------------------------------------------------------------------------------------------
// Tiling itself.
// ---------------------------------------------------------------------------------------------
TEST_CASE("the tiling is invisible: partial edge tiles composite like one big one") {
    auto lane = makeLane("tiling invariance");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    // 200x140 divides by none of these, so every run has partial right/bottom/corner tiles, and
    // 37 is not even a multiple of the 8x8 workgroup -- the extent guard carries real weight.
    for (const std::uint32_t tile : {256u, 128u, 64u, 48u, 37u, 16u}) {
        Scenario sc;
        sc.place = rotateAbout(0.31, 48.0, 36.0) * Affine2D::translation(51.5, 29.25);
        sc.filter = ResampleFilter::Lanczos3;
        sc.blend = BlendMode::Multiply;
        sc.opacity = 0.8f;
        sc.mask = {true, true, true, 96, 72};
        sc.clip = true;
        sc.tileSize = tile;
        expectParity(runScenario(*lane, sc), ("tile " + std::to_string(tile)).c_str());
    }
    CHECK(lane->validationErrors() == before);
}

TEST_CASE("the kernel is source-format-agnostic (rgba16f tile vs 8-bit tile)") {
    auto lane = makeLane("source format parity");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    // The kernel only ever texelFetches uSrc, so a layer tile may live in the rgba16f working
    // format OR in `core::RasterLayer`'s native R8G8B8A8_UNORM. Both hit parity -- but NOT
    // identically, and this case exists to pin the difference rather than hide it:
    //
    //   fp16's spacing just below 1.0 is 2^-11, FOUR TIMES COARSER than the 8-bit grid the value
    //   came from. The four reciprocal blend modes divide by (1-s) or by s, so a source channel
    //   that close to 1 has its (1-s) amplified: relative error ~ulp(s)/(1-s). Measured on the
    //   dev GPU with this placement, Vivid Light lands ONE pixel in 28000 at 2 LSB instead of 1;
    //   the same scene with an R8G8B8A8_UNORM tile -- where the shader sees the exact k/255 the
    //   CPU reference does -- is strictly inside 1 LSB everywhere.
    //
    // The actionable reading for the residency layer: while layer storage is 8-bit, upload tiles
    // as R8G8B8A8_UNORM. It is more accurate here AND half the upload bandwidth; converting to
    // fp16 on the way in buys nothing until layer storage itself goes float (S43-a).
    for (const VkFormat fmt : {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM})
        for (const BlendMode m : {BlendMode::ColorBurn, BlendMode::ColorDodge,
                                  BlendMode::VividLight, BlendMode::Divide, BlendMode::Normal}) {
            Scenario sc;
            sc.srcFormat = fmt;
            sc.blend = m;
            sc.place = rotateAbout(0.33, 48.0, 36.0) * Affine2D::translation(50.0, 28.0);
            sc.filter = ResampleFilter::Lanczos3;  // overshoots, so s lands near 1 and past it
            const bool rgba8 = fmt == VK_FORMAT_R8G8B8A8_UNORM;
            const std::string label = std::string(core::blendModeName(m)) +
                                      (rgba8 ? " / rgba8 tile" : " / rgba16f tile");
            // An 8-bit tile is held to STRICT parity for every mode; only the fp16 tile carries
            // the reciprocal slack, so the difference between the two formats is the assertion.
            expectParity(runScenario(*lane, sc), label.c_str(),
                         (!rgba8 && isReciprocal(m)) ? kFp16ReciprocalSlack : 0);
        }
    CHECK(lane->validationErrors() == before);
}

TEST_CASE("a layer that misses the tile entirely leaves the accumulator untouched") {
    auto lane = makeLane("off-canvas parity");
    if (!lane) return;
    Scenario sc;
    sc.place = Affine2D::translation(-400.0, -400.0);  // wholly off canvas
    sc.filter = ResampleFilter::Lanczos3;
    expectParity(runScenario(*lane, sc), "layer entirely off canvas");
}

// ---------------------------------------------------------------------------------------------
// A GROUP's linked mask -- the second pre-transform fold shape (maskMode 2), which samples the
// mask through an AFFINE in the source buffer's own space rather than proportionally.
// ---------------------------------------------------------------------------------------------
TEST_CASE("a group's linked mask folds in the group's local space") {
    auto lane = makeLane("group mask parity");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    // A top-level group whose single child is an OPAQUE canvas-sized raster: the group's local
    // extent is then exactly the canvas ({0,0,W,H}, no offset), its local composite is the
    // child's pixels, its linked mask folds onto that buffer through the identity, and the
    // placement is the identity. So the layer step the kernel has to reproduce is:
    //     source = the child's pixels, maskMode 2 with the identity map, place = the group's.
    Image child(kCanvasW, kCanvasH);
    for (std::uint32_t y = 0; y < kCanvasH; ++y)
        for (std::uint32_t x = 0; x < kCanvasW; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * kCanvasW + x) * 4;
            child.rgba[p + 0] = static_cast<std::uint8_t>((x * 3) % 256);
            child.rgba[p + 1] = static_cast<std::uint8_t>(255 - (y * 255 / (kCanvasH - 1)));
            child.rgba[p + 2] = static_cast<std::uint8_t>((x + y) % 256);
            child.rgba[p + 3] = 255;  // opaque: contentBounds is the whole canvas
        }
    const Image bg = backgroundImage();
    // The mask is SMALLER than the group's local buffer, so the "zero coverage beyond the sheet"
    // rule is exercised too.
    constexpr std::uint32_t kMw = 150, kMh = 110;
    const std::vector<std::uint8_t> cov = maskCoverage(kMw, kMh);

    render::CompositeOptions opts;
    opts.resampleFilter = ResampleFilter::Nearest;

    Image bgFlat;
    {
        core::Document bgDoc(kCanvasW, kCanvasH);
        auto l = bgDoc.makeRaster("bg", kCanvasW, kCanvasH);
        l->image() = bg;
        bgDoc.root().addOnTop(std::move(l));
        const render::CompositeResult r = render::composite(bgDoc, opts, render::Backend::Cpu);
        REQUIRE(r.ok);
        bgFlat = r.image;
    }

    Image cpuFlat;
    {
        core::Document doc(kCanvasW, kCanvasH);
        auto l = doc.makeRaster("bg", kCanvasW, kCanvasH);
        l->image() = bg;
        doc.root().addOnTop(std::move(l));
        auto group = doc.makeGroup("grp");
        auto inner = doc.makeRaster("inner", kCanvasW, kCanvasH);
        inner->image() = child;
        group->addOnTop(std::move(inner));
        core::RasterMask m(kMw, kMh);
        m.coverage = cov;
        m.linked = true;
        group->setMask(std::move(m));
        group->setOpacity(0.68f);
        group->setBlendMode(BlendMode::Screen);
        doc.root().addOnTop(std::move(group));
        const render::CompositeResult r = render::composite(doc, opts, render::Backend::Cpu);
        REQUIRE(r.ok);
        cpuFlat = r.image;
    }

    Step step;
    step.place = Affine2D::identity();
    step.filter = ResampleFilter::Nearest;
    step.blend = BlendMode::Screen;
    step.opacity = 0.68f;
    step.maskMode = 2;
    step.maskXform = Affine2D::identity();
    MaskData mask;
    mask.width = kMw;
    mask.height = kMh;
    mask.coverage = cov;

    const ImageF gpuAcc = lane->composite(mosaic::common::toFloat(bgFlat),
                                          mosaic::common::toFloat(child), mask, {}, step, 64);
    expectParity(compare(cpuFlat, mosaic::common::toImage8(gpuAcc)), "group linked mask");
    CHECK(lane->validationErrors() == before);
}

// ---------------------------------------------------------------------------------------------
// The kernel must stay inside the Vulkan 1.0 floor by construction, not by luck.
// ---------------------------------------------------------------------------------------------
TEST_CASE("the tile kernel fits Vulkan 1.0's guaranteed minimums") {
    using namespace mosaic::render;
    CHECK(sizeof(PushBlock) <= vk10::kMaxPushConstantsSize);
    CHECK(kWorkgroup * kWorkgroup <= vk10::kMaxComputeWorkGroupInvocations);
    // 2 storage images (accumulator + published clip base), 3 sampled images, 1 storage buffer
    // (the dirty-macrotile list), 1 descriptor set.
    GpuProbe floorProbe;
    applyFloorProfile(floorProbe);
    const GpuCaps floorCaps = decide(floorProbe);
    CHECK(floorCaps.fitsStorageImages(2));
    CHECK(floorCaps.fitsSampledImages(3));
    CHECK(floorCaps.fitsStorageBuffers(1));
    CHECK(floorCaps.fitsPushConstants(sizeof(PushBlock)));
    // The accumulator format the shader hard-codes is the one a floor device must offer.
    CHECK(floorCaps.workingFormat == VK_FORMAT_R16G16B16A16_SFLOAT);

    // ---- Item 10's two halves, and which of them the floor gets -------------------------------
    //
    // The SSBO tile list is a FLOOR feature: one storage buffer against a guaranteed four, plain
    // `#version 450`, and a specialization constant, which is core 1.0. So the list shape is
    // available on every device this renderer admits at all -- which is the whole reason it is
    // the default rather than the tier.
    CHECK(floorCaps.fitsStorageBuffers(1));
    CHECK(floorCaps.spirvVersion == mosaic::render::spirv::kVersion1_0);
    // The descriptor-indexed variant is NOT, and it must refuse for BOTH of its reasons here: no
    // feature, and no SPIR-V version to load the variant blob with. Either alone would be enough;
    // asserting both is what stops a future change from silently dropping one of the two gates.
    CHECK_FALSE(floorCaps.descriptorIndexing);
    CHECK_FALSE(floorCaps.fitsSpirvVersion(mosaic::render::spirv::kVersion1_3));
    // ... and one storage buffer is all the list ever binds, whatever the canvas: the macrotile
    // records are a RANGE inside it, not a buffer each.
    CHECK(floorCaps.fitsStorageBufferRange(sizeof(TileRecord) * 4096));
}

TEST_CASE("the tile kernel is deterministic on a given device") {
    auto lane = makeLane("determinism");
    if (!lane) return;
    Scenario sc;
    sc.place = rotateAbout(0.29, 48.0, 36.0) * Affine2D::translation(49.5, 27.75);
    sc.filter = ResampleFilter::Lanczos3;
    sc.mask = {true, true, true, 96, 72};

    render::CompositeOptions opts;
    opts.resampleFilter = sc.filter;
    core::Document bgDoc(kCanvasW, kCanvasH);
    auto bg = bgDoc.makeRaster("bg", kCanvasW, kCanvasH);
    bg->image() = sc.background;
    bgDoc.root().addOnTop(std::move(bg));
    const render::CompositeResult r = render::composite(bgDoc, opts, render::Backend::Cpu);
    REQUIRE(r.ok);

    Step step;
    step.place = sc.place;
    step.filter = sc.filter;
    step.maskMode = 1;
    MaskData mask{sc.mask.width, sc.mask.height, maskCoverage(sc.mask.width, sc.mask.height)};

    const ImageF acc = mosaic::common::toFloat(r.image);
    const ImageF src = mosaic::common::toFloat(sc.source);
    // BIT-identical across tile grids, not merely "within tolerance": the kernel evaluates every
    // sample at its TARGET pixel, so a macrotile recomposited on its own is the same bytes as the
    // same pixels inside a whole-canvas dispatch. That is what makes the macrotile size k a free
    // knob (§3.1) and what a dirty-tile recomposite needs to be seamless.
    const ImageF a = lane->composite(acc, src, mask, {}, step, 64);
    const ImageF b = lane->composite(acc, src, mask, {}, step, 37);
    const ImageF whole = lane->composite(acc, src, mask, {}, step, 4096);  // one dispatch
    CHECK(a.rgba == b.rgba);
    CHECK(a.rgba == whole.rgba);
}


// ---------------------------------------------------------------------------------------------
// Source WINDOWING (S60-a). Vulkan 1.0 guarantees only maxImageDimension2D == 4096, so a
// 5000x8000 layer -- PLAN §2's Move-lag document -- cannot be bound whole on a floor device. The
// kernel therefore accepts a WINDOW: a sub-rect of the layer bound in place of all of it, with
// the window's origin and the layer's TRUE size carried in push-constant lanes that were
// previously padding, so the block stays at 120 bytes inside the 128-byte floor.
//
// The property that matters is that windowing is INVISIBLE: bounds and the proportional mask
// mapping are defined against the whole layer, so deriving them from textureSize(uSrc) -- which
// is what the kernel did before -- disagrees at the layer's edges and wherever the mask's
// resolution differs from the source's.
// ---------------------------------------------------------------------------------------------

TEST_CASE("a source window is invisible: same pixels as binding the whole layer") {
    auto lane = makeLane("source windowing");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    // Exercise it against the hard cases, not the easy one: rotation (so the read region is a
    // rotated quad), a wide filter (so the footprint reaches well past the tile), a linked mask
    // at a DIFFERENT resolution than the source (so the proportional mapping is load-bearing),
    // clip, and partial edge tiles.
    Scenario sc;
    sc.place = rotateAbout(0.29, 48.0, 36.0) * Affine2D::translation(37.5, 21.25);
    sc.filter = ResampleFilter::Lanczos3;
    sc.blend = BlendMode::Overlay;
    sc.opacity = 0.71f;
    sc.mask = {true, true, true, 37, 29};  // deliberately not the source's size
    sc.clip = true;
    sc.tileSize = 48;
    sc.srcWindow = SrcWindow{0, 0, sc.source.width, sc.source.height};
    expectParity(runScenario(*lane, sc), "windowed (whole layer as a window)");

    CHECK(lane->validationErrors() == before);
}

TEST_CASE("windowing holds for every blend mode") {
    auto lane = makeLane("source windowing x blend");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    for (int m = 0; m <= static_cast<int>(BlendMode::Luminosity); ++m) {
        Scenario sc;
        sc.place = Affine2D::translation(11.5, 7.25);
        sc.filter = ResampleFilter::Bilinear;
        sc.blend = static_cast<BlendMode>(m);
        sc.opacity = 0.83f;
        sc.mask = {true, true, true, 96, 72};
        sc.srcWindow = SrcWindow{0, 0, sc.source.width, sc.source.height};
        expectParity(runScenario(*lane, sc), ("windowed blend " + std::to_string(m)).c_str());
    }
    CHECK(lane->validationErrors() == before);
}

TEST_CASE("the layer's TRUE size governs bounds, not the bound window's") {
    // The kernel must treat a texel outside the LAYER as transparent and a texel outside the
    // WINDOW as a plumbing error, and must never confuse the two. A window inset from the layer's
    // edge leaves an unreadable band, so its output MUST differ from the whole-layer composite --
    // if it matched, the bounds check would be reading the wrong rectangle and the distinction
    // would be vacuous.
    auto lane = makeLane("window bounds");
    if (!lane) return;

    Scenario full;
    full.place = Affine2D::translation(4.0, 3.0);
    full.filter = ResampleFilter::Bilinear;
    full.tileSize = 64;
    const Diff whole = runScenario(*lane, full);
    expectParity(whole, "unwindowed reference");

    Scenario inset = full;
    inset.srcWindow = SrcWindow{16, 16, inset.source.width - 32, inset.source.height - 32};
    const Diff clipped = runScenario(*lane, inset);
    // Against the CPU reference (which reads the whole layer) the inset window must NOT match.
    CHECK(clipped.maxLsb > whole.maxLsb);
}

// ---------------------------------------------------------------------------------------------
// THE DISPATCH SHAPE (S60-a item 10). Three ways to tell one invocation which macrotile it is on:
//
//   PerTile   the tile's three integers in push constants, one dispatch each -- the shape items
//             8/9 shipped, and the reference the other two are held to here.
//   TileList  the same three integers in a storage buffer, ONE dispatch for every macrotile,
//             gl_WorkGroupID.z selecting the record. Available at the Vulkan 1.0 FLOOR.
//   Indexed   TileList, plus the layer's two sheets read out of a runtime-sized descriptor array.
//             Gated on GpuCaps::descriptorIndexing AND on the device accepting the variant blob's
//             SPIR-V; a device without either simply never builds the pipeline.
//
// THE ASSERTION IS BYTE-IDENTITY, not parity within 1 LSB, and the difference matters. A dispatch
// reshape that moved a pixel by a last bit would pass a tolerance test on the fixture it was
// tuned against and rot a canvas one macrotile at a time in the app -- the same failure shape the
// tiling-invariance case above exists to catch. The shapes read the SAME integers, so anything
// other than an exact match is a defect, and asking for exactness is what makes that detectable.
// ---------------------------------------------------------------------------------------------

namespace {
// Every axis the file already sweeps, run through a second shape and compared to the per-tile loop.
void expectSameBytes(const ImageF& reference, const ImageF& other, const std::string& label) {
    INFO(label);
    REQUIRE(reference.width == other.width);
    REQUIRE(reference.height == other.height);
    CHECK(reference.rgba == other.rgba);
}
}  // namespace

TEST_CASE("the SSBO tile list composites the same BYTES as the per-tile loop") {
    auto lane = makeLane("tile-list dispatch");
    if (!lane) return;
    const std::uint32_t before = lane->validationErrors();

    // Blend modes, at an integer translation so the blend is what is isolated.
    for (int m = 0; m <= static_cast<int>(BlendMode::Luminosity); ++m) {
        Scenario sc;
        sc.place = Affine2D::translation(37.0, 21.0);
        sc.blend = static_cast<BlendMode>(m);
        sc.opacity = 0.83f;
        const Prepared p = prepare(sc);
        expectSameBytes(runShape(*lane, sc, p, render::TileDispatch::PerTile),
                        runShape(*lane, sc, p, render::TileDispatch::TileList),
                        std::string("blend ") + std::string(core::blendModeName(sc.blend)));
    }

    // Every resample filter under a rotation-plus-scale, where the kernels do real work.
    for (const ResampleFilter f :
         {ResampleFilter::Nearest, ResampleFilter::Bilinear, ResampleFilter::Bicubic,
          ResampleFilter::Mitchell, ResampleFilter::Lanczos2, ResampleFilter::Lanczos3,
          ResampleFilter::Area, ResampleFilter::Gaussian, ResampleFilter::Supersample,
          ResampleFilter::Auto}) {
        Scenario sc;
        sc.place = rotateAbout(0.31, 48.0, 36.0) * Affine2D::scaling(1.7, 0.6) *
                   Affine2D::translation(29.5, 17.25);
        sc.filter = f;
        const Prepared p = prepare(sc);
        expectSameBytes(runShape(*lane, sc, p, render::TileDispatch::PerTile),
                        runShape(*lane, sc, p, render::TileDispatch::TileList),
                        "filter " + std::to_string(static_cast<int>(f)));
    }

    // Mask / clip / window shapes, and the partial-edge-tile grids that make the extent guard
    // load-bearing -- 200x140 divides by none of these.
    struct ShapeCase {
        const char* name;
        MaskSpec mask;
        bool clip;
        std::uint32_t tileSize;
        SrcWindow window;
    };
    const Image src = sourceImage();
    const ShapeCase shapes[] = {
        {"no mask, 64", {}, false, 64, {}},
        {"linked mask, 48", {true, true, true, 96, 72}, false, 48, {}},
        {"linked mask at another resolution, 37", {true, true, true, 37, 29}, false, 37, {}},
        {"unlinked mask, 128", {true, false, true, 96, 72}, false, 128, {}},
        {"disabled mask, 64", {true, true, false, 96, 72}, false, 64, {}},
        {"clip, 256", {}, true, 256, {}},
        {"mask + clip, 16", {true, true, true, 96, 72}, true, 16, {}},
        {"whole-layer window, 48", {true, true, true, 37, 29}, true, 48,
         SrcWindow{0, 0, src.width, src.height}},
        {"inset window, 64", {}, false, 64,
         SrcWindow{16, 16, src.width - 32, src.height - 32}},
        {"one dispatch (4096)", {true, true, true, 96, 72}, true, 4096, {}},
    };
    for (const ShapeCase& c : shapes) {
        Scenario sc;
        sc.place = rotateAbout(0.29, 48.0, 36.0) * Affine2D::translation(37.5, 21.25);
        sc.filter = ResampleFilter::Lanczos3;
        sc.blend = BlendMode::Overlay;
        sc.opacity = 0.71f;
        sc.mask = c.mask;
        sc.clip = c.clip;
        sc.tileSize = c.tileSize;
        sc.srcWindow = c.window;
        const Prepared p = prepare(sc);
        expectSameBytes(runShape(*lane, sc, p, render::TileDispatch::PerTile),
                        runShape(*lane, sc, p, render::TileDispatch::TileList), c.name);
    }
    CHECK(lane->validationErrors() == before);
}

TEST_CASE("the SSBO tile list still matches the CPU reference") {
    // Byte-identity to the per-tile loop is the strong claim; this is the cheap corroboration that
    // both of them are still the RIGHT picture, so a shared regression cannot hide inside an
    // agreement between two shapes.
    auto lane = makeLane("tile-list vs CPU");
    if (!lane) return;
    for (int m = 0; m <= static_cast<int>(BlendMode::Luminosity); ++m) {
        Scenario sc;
        sc.place = Affine2D::translation(37.0, 21.0);
        sc.blend = static_cast<BlendMode>(m);
        sc.opacity = 0.83f;
        sc.mask = {true, true, true, 96, 72};
        sc.dispatch = render::TileDispatch::TileList;
        expectParity(runScenario(*lane, sc),
                     (std::string("list ") + std::string(core::blendModeName(sc.blend))).c_str(),
                     isReciprocal(sc.blend) ? kFp16ReciprocalSlack : 0);
    }
}

TEST_CASE("the descriptor-indexed variant composites the same BYTES as the per-tile loop") {
    auto lane = makeLane("descriptor-indexed dispatch");
    if (!lane) return;
    if (!lane->haveIndexed()) {
        // The refusal path, and it is an ORDINARY outcome: MOSAIC_GPU_PROFILE=floor produces it on
        // any device, and so does a driver without the four descriptor-indexing sub-features or
        // without the SPIR-V version the variant blob needs. The floor shapes above draw the same
        // picture, so nothing is lost but descriptor writes.
        const render::GpuCaps& caps = lane->caps();
        const std::string note =
            std::string("descriptor indexing unavailable -- skipping (indexing=") +
            (caps.descriptorIndexing ? "yes" : "no") + ", spirv1.3=" +
            (caps.fitsSpirvVersion(mosaic::render::spirv::kVersion1_3) ? "yes" : "no") + ")";
        WARN_MESSAGE(true, note);
        return;
    }
    const std::uint32_t before = lane->validationErrors();

    for (int m = 0; m <= static_cast<int>(BlendMode::Luminosity); ++m) {
        Scenario sc;
        sc.place = Affine2D::translation(37.0, 21.0);
        sc.blend = static_cast<BlendMode>(m);
        sc.opacity = 0.83f;
        const Prepared p = prepare(sc);
        expectSameBytes(runShape(*lane, sc, p, render::TileDispatch::PerTile),
                        runShape(*lane, sc, p, render::TileDispatch::Indexed),
                        std::string("indexed blend ") +
                            std::string(core::blendModeName(sc.blend)));
    }

    for (const ResampleFilter f :
         {ResampleFilter::Nearest, ResampleFilter::Bilinear, ResampleFilter::Bicubic,
          ResampleFilter::Mitchell, ResampleFilter::Lanczos2, ResampleFilter::Lanczos3,
          ResampleFilter::Area, ResampleFilter::Gaussian, ResampleFilter::Supersample,
          ResampleFilter::Auto}) {
        Scenario sc;
        sc.place = rotateAbout(0.31, 48.0, 36.0) * Affine2D::scaling(1.7, 0.6) *
                   Affine2D::translation(29.5, 17.25);
        sc.filter = f;
        const Prepared p = prepare(sc);
        expectSameBytes(runShape(*lane, sc, p, render::TileDispatch::PerTile),
                        runShape(*lane, sc, p, render::TileDispatch::Indexed),
                        "indexed filter " + std::to_string(static_cast<int>(f)));
    }

    // The mask array is the part that only the indexed variant exercises: the mask sheet arrives
    // through the SAME runtime array as the pixels (slot 2i+1), so a mistake in the interleave
    // shows up here and nowhere else.
    const Image src = sourceImage();
    struct ShapeCase {
        const char* name;
        MaskSpec mask;
        bool clip;
        std::uint32_t tileSize;
        SrcWindow window;
    };
    const ShapeCase shapes[] = {
        {"indexed no mask, 64", {}, false, 64, {}},
        {"indexed linked mask, 48", {true, true, true, 96, 72}, false, 48, {}},
        {"indexed mask at another resolution, 37", {true, true, true, 37, 29}, false, 37, {}},
        {"indexed unlinked mask, 128", {true, false, true, 96, 72}, false, 128, {}},
        {"indexed clip, 256", {}, true, 256, {}},
        {"indexed mask + clip, 16", {true, true, true, 96, 72}, true, 16, {}},
        {"indexed whole-layer window, 48", {true, true, true, 37, 29}, true, 48,
         SrcWindow{0, 0, src.width, src.height}},
        {"indexed inset window, 64", {}, false, 64,
         SrcWindow{16, 16, src.width - 32, src.height - 32}},
    };
    for (const ShapeCase& c : shapes) {
        Scenario sc;
        sc.place = rotateAbout(0.29, 48.0, 36.0) * Affine2D::translation(37.5, 21.25);
        sc.filter = ResampleFilter::Lanczos3;
        sc.blend = BlendMode::Overlay;
        sc.opacity = 0.71f;
        sc.mask = c.mask;
        sc.clip = c.clip;
        sc.tileSize = c.tileSize;
        sc.srcWindow = c.window;
        const Prepared p = prepare(sc);
        expectSameBytes(runShape(*lane, sc, p, render::TileDispatch::PerTile),
                        runShape(*lane, sc, p, render::TileDispatch::Indexed), c.name);
    }
    CHECK(lane->validationErrors() == before);
}

// ---------------------------------------------------------------------------------------------
// The ADJUSTMENT kernel (S60-a): shaders/adjust_tile.comp, the lane's second kernel.
//
// Its PIXELS are proven end to end in tests/test_tile_compositor.cpp -- per kind, against
// `render::composite(..., Backend::Cpu)` at the same 1/255 as everything else -- because an
// adjustment is a function of the ACCUMULATED BACKDROP, so a one-layer harness has nothing for it
// to grade. What belongs here is the claim that harness rests on, and it is a structural one.
// ---------------------------------------------------------------------------------------------

TEST_CASE("the adjustment kernel loads on the composite kernel's own layouts") {
    // ⚠ WHY THIS CASE EXISTS AT ALL. The adjustment kernel shares composite_tile.comp's descriptor
    // set layout and its 124-byte push range -- that sharing is what keeps the descriptor pool, the
    // per-(layer, atlas image) set allocation and the dispatch geometry identical for both kernels.
    // If it ever stops being true, `vkCreateComputePipelines` refuses inside `initPipeline` and
    // `TileCompositor::create` returns NULL -- at which point every adjustment case in
    // test_tile_compositor.cpp WARNs "no usable Vulkan device" and passes. A whole file going quiet
    // is exactly the failure a test suite must not be able to hide, so this case separates the two
    // reasons a lane can be absent: it checks the DOCUMENTED admission gate itself, and then
    // REQUIRES the lane on any device that passes it.
    // ⚠ ... but "the user asked for no GPU at all" is a THIRD reason, and it is not a regression:
    // under CPU-only mode (`--cpu` / MOSAIC_CPU_ONLY / Settings -> Rendering) `create()` refuses
    // before it touches Vulkan, by design. Asked FIRST, because the whole point of running the
    // suite as `MOSAIC_CPU_ONLY=1 ctest` is that it must come out green -- a REQUIRE that cannot
    // hold in a supported mode turns that regression net into a permanent red.
    if (!render::gpuPolicy().allowsComputeLane()) {
        WARN_MESSAGE(true, "CPU-only mode -- skipping the adjustment kernel load check");
        return;
    }
    std::string ctxError;
    auto ctx = render::VulkanContext::shared(/*enableValidation=*/true, ctxError);
    if (!ctx) {
        const std::string note =
            std::string("no usable Vulkan device -- skipping adjustment kernel load (") +
            ctxError + ")";
        WARN_MESSAGE(true, note);
        return;
    }

    // TileCompositor::create's gate, restated. The adjustment kernel asks for strictly LESS than
    // this (1 storage image, 3 sampled, the same storage buffer, 108 push bytes), so a device that
    // passes here has room for both.
    const render::GpuCaps& caps = ctx->caps();
    const bool gateOpen = caps.fitsStorageImages(2) && caps.fitsSampledImages(3) &&
                          caps.fitsStorageBuffers(1) && caps.fitsPushConstants(sizeof(PushBlock)) &&
                          caps.workingFormat == VK_FORMAT_R16G16B16A16_SFLOAT &&
                          caps.maxImageDim >= render::kDirtyTileSize;
    if (!gateOpen) {
        WARN_MESSAGE(true, "device below the tile kernel's documented floor -- skipping");
        return;
    }

    std::string error;
    auto lane = render::TileCompositor::create(ctx, error);
    REQUIRE_MESSAGE(lane != nullptr, error);
    // The floor shapes are always built; the indexed tier is all-or-nothing across BOTH kernels, so
    // a refusal here names one reason for the pair rather than leaving them to disagree.
    INFO("indexed: " << std::string(render::dispatchRefusalName(lane->indexedRefusal())));
    CHECK(lane->validationErrors() == 0);
}
