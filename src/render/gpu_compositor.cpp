#include "render/gpu_compositor.hpp"

#include <cstring>

#include <vk_mem_alloc.h>

#include "common/log.hpp"
#include "render/gpu_policy.hpp"
#include "render/vulkan_context.hpp"
#include <shaders/composite_blend.comp.spv.hpp>

namespace mosaic::render {
namespace {
struct PushConstants {
    std::int32_t mode;
    float opacity;
};
constexpr VkFormat kFloatFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
}  // namespace

// All Vulkan/VMA state lives here so vk_mem_alloc.h never leaks into the header.
class GpuCompositor::Impl {
public:
    std::shared_ptr<VulkanContext> ctx;  // the process-wide shared device (S60-alpha)
    VkCommandPool pool = VK_NULL_HANDLE; // OURS: pools are externally synchronized
    VmaAllocator allocator = VK_NULL_HANDLE;

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    // Canvas-sized resources, (re)created on size change.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkImage accImage = VK_NULL_HANDLE, srcImage = VK_NULL_HANDLE;
    VmaAllocation accAlloc = VK_NULL_HANDLE, srcAlloc = VK_NULL_HANDLE;
    VkImageView accView = VK_NULL_HANDLE, srcView = VK_NULL_HANDLE;
    VkBuffer accStaging = VK_NULL_HANDLE, srcStaging = VK_NULL_HANDLE;
    VmaAllocation accStagingAlloc = VK_NULL_HANDLE, srcStagingAlloc = VK_NULL_HANDLE;
    void* accStagingPtr = nullptr;
    void* srcStagingPtr = nullptr;
};

std::unique_ptr<GpuCompositor> GpuCompositor::create(std::string& error) {
    // CPU-only mode (render/gpu_policy.hpp, S60-b item 14). compositeBuffer builds one of these
    // PER COMPOSITE, so this is the call site the once-per-lane log line in computeLaneAllowed
    // was written for: refuse quietly after the first time and let compositeBufferOver serve.
    if (!computeLaneAllowed("buffer compositor", error)) return nullptr;
    auto self = std::unique_ptr<GpuCompositor>(new GpuCompositor());
    self->m_impl = std::make_unique<Impl>();
    Impl& d = *self->m_impl;

    d.ctx = VulkanContext::shared(/*enableValidation=*/true, error);
    if (!d.ctx) return nullptr;
    d.pool = d.ctx->createCommandPool(error);
    if (d.pool == VK_NULL_HANDLE) return nullptr;
    const VkDevice dev = d.ctx->device();

    // rgba32f storage images are core-required, but verify and bail to the CPU path if absent.
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(d.ctx->physicalDevice(), kFloatFormat, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        error = "device cannot use an rgba32f storage image";
        return nullptr;
    }

    const VmaAllocatorCreateInfo aci{
        .physicalDevice = d.ctx->physicalDevice(),
        .device = dev,
        .instance = d.ctx->instance(),
        // The version VMA may use must not exceed what the instance was created with NOR
        // what the device supports -- caps().apiVersion is exactly min(instance, device).
        // A hard-coded 1.2 here would have VMA reach for entry points a 1.0 device lacks.
        .vulkanApiVersion = d.ctx->caps().apiVersion,
    };
    if (vmaCreateAllocator(&aci, &d.allocator) != VK_SUCCESS) {
        error = "vmaCreateAllocator failed";
        return nullptr;
    }

    // Two storage-image bindings: accumulator (read+write) and source (read).
    const VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    const VkDescriptorSetLayoutCreateInfo slci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings,
    };
    if (vkCreateDescriptorSetLayout(dev, &slci, nullptr, &d.setLayout) != VK_SUCCESS) {
        error = "vkCreateDescriptorSetLayout failed";
        return nullptr;
    }
    const VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
    const VkPipelineLayoutCreateInfo plci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &d.setLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &d.pipeLayout) != VK_SUCCESS) {
        error = "vkCreatePipelineLayout failed";
        return nullptr;
    }
    const VkShaderModuleCreateInfo smci{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shaders::composite_blend_comp_size,
        .pCode = shaders::composite_blend_comp,
    };
    if (vkCreateShaderModule(dev, &smci, nullptr, &d.shader) != VK_SUCCESS) {
        error = "vkCreateShaderModule failed";
        return nullptr;
    }
    const VkComputePipelineCreateInfo cpci{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = d.shader,
                  .pName = "main"},
        .layout = d.pipeLayout,
    };
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &d.pipeline) != VK_SUCCESS) {
        error = "vkCreateComputePipelines failed";
        return nullptr;
    }

    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
    const VkDescriptorPoolCreateInfo dpci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &d.descPool) != VK_SUCCESS) {
        error = "vkCreateDescriptorPool failed";
        return nullptr;
    }
    const VkDescriptorSetAllocateInfo dsai{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = d.descPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &d.setLayout,
    };
    if (vkAllocateDescriptorSets(dev, &dsai, &d.descSet) != VK_SUCCESS) {
        error = "vkAllocateDescriptorSets failed";
        return nullptr;
    }

    const VkCommandBufferAllocateInfo cbai{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = d.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(dev, &cbai, &d.cmd) != VK_SUCCESS) {
        error = "vkAllocateCommandBuffers failed";
        return nullptr;
    }
    const VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(dev, &fci, nullptr, &d.fence) != VK_SUCCESS) {
        error = "vkCreateFence failed";
        return nullptr;
    }
    return self;
}

void GpuCompositor::destroySizedResources() noexcept {
    Impl& d = *m_impl;
    const VkDevice dev = d.ctx->device();
    if (d.accView) vkDestroyImageView(dev, d.accView, nullptr);
    if (d.srcView) vkDestroyImageView(dev, d.srcView, nullptr);
    if (d.accImage) vmaDestroyImage(d.allocator, d.accImage, d.accAlloc);
    if (d.srcImage) vmaDestroyImage(d.allocator, d.srcImage, d.srcAlloc);
    if (d.accStaging) vmaDestroyBuffer(d.allocator, d.accStaging, d.accStagingAlloc);
    if (d.srcStaging) vmaDestroyBuffer(d.allocator, d.srcStaging, d.srcStagingAlloc);
    d.accView = d.srcView = VK_NULL_HANDLE;
    d.accImage = d.srcImage = VK_NULL_HANDLE;
    d.accStaging = d.srcStaging = VK_NULL_HANDLE;
    d.accStagingPtr = d.srcStagingPtr = nullptr;
    d.width = d.height = 0;
}

GpuCompositor::~GpuCompositor() {
    if (!m_impl || !m_impl->ctx) return;
    Impl& d = *m_impl;
    const VkDevice dev = d.ctx->device();
    d.ctx->waitIdle();
    destroySizedResources();
    if (d.fence) vkDestroyFence(dev, d.fence, nullptr);
    if (d.cmd) vkFreeCommandBuffers(dev, d.pool, 1, &d.cmd);
    if (d.pool) vkDestroyCommandPool(dev, d.pool, nullptr);
    if (d.descPool) vkDestroyDescriptorPool(dev, d.descPool, nullptr);
    if (d.pipeline) vkDestroyPipeline(dev, d.pipeline, nullptr);
    if (d.shader) vkDestroyShaderModule(dev, d.shader, nullptr);
    if (d.pipeLayout) vkDestroyPipelineLayout(dev, d.pipeLayout, nullptr);
    if (d.setLayout) vkDestroyDescriptorSetLayout(dev, d.setLayout, nullptr);
    if (d.allocator) vmaDestroyAllocator(d.allocator);
}

bool GpuCompositor::ensureSize(std::uint32_t w, std::uint32_t h, std::string& error) {
    Impl& d = *m_impl;
    // A document-sized image is not always representable: Vulkan 1.0 guarantees only 4096, so a
    // 5000x8000 document cannot be one image on a floor device. Refusing here (the caller falls
    // back to the CPU blend) is CORRECT but is also why the resident compositor must be tiled --
    // this is the size limit S60-a's macrotiles exist to stay under. (S60-alpha)
    if (!d.ctx->caps().fitsImage(w, h)) {
        error = "canvas " + std::to_string(w) + "x" + std::to_string(h) +
                " exceeds the device's max image dimension (" +
                std::to_string(d.ctx->caps().maxImageDim) + ")";
        return false;
    }
    if (d.width == w && d.height == h && d.accImage) return true;
    d.ctx->waitIdle();
    destroySizedResources();

    const VkDevice dev = d.ctx->device();
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4 * sizeof(float);

    const VkImageCreateInfo ici{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = kFloatFormat,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo imgAlloc{.usage = VMA_MEMORY_USAGE_AUTO};
    if (vmaCreateImage(d.allocator, &ici, &imgAlloc, &d.accImage, &d.accAlloc, nullptr) !=
            VK_SUCCESS ||
        vmaCreateImage(d.allocator, &ici, &imgAlloc, &d.srcImage, &d.srcAlloc, nullptr) !=
            VK_SUCCESS) {
        error = "vmaCreateImage failed";
        return false;
    }

    VkImageViewCreateInfo vci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = kFloatFormat,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vci.image = d.accImage;
    if (vkCreateImageView(dev, &vci, nullptr, &d.accView) != VK_SUCCESS) {
        error = "vkCreateImageView failed";
        return false;
    }
    vci.image = d.srcImage;
    if (vkCreateImageView(dev, &vci, nullptr, &d.srcView) != VK_SUCCESS) {
        error = "vkCreateImageView failed";
        return false;
    }

    const VkBufferCreateInfo bci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo stagingAlloc{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VmaAllocationInfo accInfo{}, srcInfo{};
    if (vmaCreateBuffer(d.allocator, &bci, &stagingAlloc, &d.accStaging, &d.accStagingAlloc,
                        &accInfo) != VK_SUCCESS ||
        vmaCreateBuffer(d.allocator, &bci, &stagingAlloc, &d.srcStaging, &d.srcStagingAlloc,
                        &srcInfo) != VK_SUCCESS) {
        error = "vmaCreateBuffer (staging) failed";
        return false;
    }
    d.accStagingPtr = accInfo.pMappedData;
    d.srcStagingPtr = srcInfo.pMappedData;

    const VkDescriptorImageInfo accDii{VK_NULL_HANDLE, d.accView, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo srcDii{VK_NULL_HANDLE, d.srcView, VK_IMAGE_LAYOUT_GENERAL};
    const VkWriteDescriptorSet writes[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = d.descSet,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = &accDii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = d.descSet,
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = &srcDii},
    };
    vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);

    d.width = w;
    d.height = h;
    return true;
}

bool GpuCompositor::blendOver(common::ImageF& acc, const common::ImageF& src, core::BlendMode mode,
                             float opacity, std::string& error) {
    if (acc.empty() || acc.width != src.width || acc.height != src.height) {
        error = "blendOver: size mismatch";
        return false;
    }
    Impl& d = *m_impl;
    if (!ensureSize(acc.width, acc.height, error)) return false;

    const VkDevice dev = d.ctx->device();
    const std::size_t bytes = acc.rgba.size() * sizeof(float);
    std::memcpy(d.accStagingPtr, acc.rgba.data(), bytes);
    std::memcpy(d.srcStagingPtr, src.rgba.data(), bytes);
    vmaFlushAllocation(d.allocator, d.accStagingAlloc, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(d.allocator, d.srcStagingAlloc, 0, VK_WHOLE_SIZE);

    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkBufferImageCopy copy{
        .bufferOffset = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {d.width, d.height, 1},
    };

    vkResetCommandBuffer(d.cmd, 0);
    const VkCommandBufferBeginInfo cbi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(d.cmd, &cbi);

    auto barrier = [&](VkImage img, VkAccessFlags srcA, VkAccessFlags dstA, VkImageLayout oldL,
                       VkImageLayout newL) {
        return VkImageMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = srcA,
            .dstAccessMask = dstA,
            .oldLayout = oldL,
            .newLayout = newL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = img,
            .subresourceRange = range,
        };
    };

    // Upload acc + src: UNDEFINED -> TRANSFER_DST, then copy each staging buffer into its image.
    const VkImageMemoryBarrier toDst[2] = {
        barrier(d.accImage, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        barrier(d.srcImage, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    vkCmdPipelineBarrier(d.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 2, toDst);
    vkCmdCopyBufferToImage(d.cmd, d.accStaging, d.accImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);
    vkCmdCopyBufferToImage(d.cmd, d.srcStaging, d.srcImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);

    // TRANSFER_DST -> GENERAL for the compute dispatch.
    const VkImageMemoryBarrier toGeneral[2] = {
        barrier(d.accImage, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        barrier(d.srcImage, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
    };
    vkCmdPipelineBarrier(d.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2,
                         toGeneral);

    vkCmdBindPipeline(d.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipeline);
    vkCmdBindDescriptorSets(d.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipeLayout, 0, 1, &d.descSet, 0,
                            nullptr);
    const PushConstants pc{static_cast<std::int32_t>(mode), opacity};
    vkCmdPushConstants(d.cmd, d.pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(d.cmd, (d.width + 7) / 8, (d.height + 7) / 8, 1);

    // Read acc back: GENERAL -> TRANSFER_SRC, copy to staging, make it host-visible.
    const VkImageMemoryBarrier toSrc = barrier(d.accImage, VK_ACCESS_SHADER_WRITE_BIT,
                                               VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdPipelineBarrier(d.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);
    vkCmdCopyImageToBuffer(d.cmd, d.accImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.accStaging, 1,
                           &copy);
    const VkBufferMemoryBarrier toHost{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = d.accStaging,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(d.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                         nullptr, 1, &toHost, 0, nullptr);
    vkEndCommandBuffer(d.cmd);

    vkResetFences(dev, 1, &d.fence);
    const VkSubmitInfo si{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &d.cmd,
    };
    if (d.ctx->submit(si, d.fence) != VK_SUCCESS) {
        error = "vkQueueSubmit failed";
        return false;
    }
    vkWaitForFences(dev, 1, &d.fence, VK_TRUE, UINT64_MAX);

    vmaInvalidateAllocation(d.allocator, d.accStagingAlloc, 0, VK_WHOLE_SIZE);
    std::memcpy(acc.rgba.data(), d.accStagingPtr, bytes);
    return true;
}

std::string GpuCompositor::deviceName() const { return m_impl->ctx->deviceName(); }

std::uint32_t GpuCompositor::validationErrors() const noexcept {
    return m_impl->ctx->validationErrors();
}

}  // namespace mosaic::render
