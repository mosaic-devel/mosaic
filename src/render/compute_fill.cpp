#include "render/compute_fill.hpp"

#include <cstring>

#include "render/gpu_policy.hpp"
#include "render/vulkan_context.hpp"
#include <shaders/fill.comp.spv.hpp>

namespace mosaic::render {
namespace {
struct PushConstants {
    float color[4];
};
}  // namespace

RenderResult computeSolid(std::uint32_t w, std::uint32_t h, common::Color8 color) {
    RenderResult res;
    res.usedBackend = Backend::GpuCompute;
    if (w == 0 || h == 0) {
        res.error = "computeSolid: zero-sized image";
        return res;
    }

    std::string err;
    // The shared device (S60-alpha) -- a one-shot helper has no business standing up a device of
    // its own. Its command pool IS its own, though: pools are externally synchronized, so this
    // helper cannot record from the context's.
    // CPU-only mode refuses before the device is stood up (S60-b item 14). Backend::Cpu already
    // routes past this helper, so what this catches is MOSAIC_CPU_ONLY with Backend::Auto -- which
    // is exactly how the suite is run as a CPU-lane regression net.
    if (!computeLaneAllowed("compute fill", err)) {
        res.error = err;
        return res;
    }
    auto ctx = VulkanContext::shared(/*enableValidation=*/true, err);
    if (!ctx) {
        res.error = "Vulkan unavailable: " + err;
        return res;
    }
    const VkCommandPool pool = ctx->createCommandPool(err);
    if (pool == VK_NULL_HANDLE) {
        res.error = "command pool unavailable: " + err;
        return res;
    }

    const VkDevice dev = ctx->device();
    const VkDeviceSize imgBytes = static_cast<VkDeviceSize>(w) * h * 4;

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory bufMem = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void* mapped = nullptr;
    bool ok = false;

    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    do {
        // ---- storage image (device-local) ----
        const VkImageCreateInfo ici{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        if (vkCreateImage(dev, &ici, nullptr, &image) != VK_SUCCESS) {
            res.error = "vkCreateImage failed";
            break;
        }
        VkMemoryRequirements ir{};
        vkGetImageMemoryRequirements(dev, image, &ir);
        const std::uint32_t it =
            ctx->findMemoryType(ir.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (it == UINT32_MAX) {
            res.error = "no device-local memory type";
            break;
        }
        const VkMemoryAllocateInfo iai{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = ir.size,
            .memoryTypeIndex = it,
        };
        if (vkAllocateMemory(dev, &iai, nullptr, &imageMem) != VK_SUCCESS ||
            vkBindImageMemory(dev, image, imageMem, 0) != VK_SUCCESS) {
            res.error = "image memory alloc/bind failed";
            break;
        }

        const VkImageViewCreateInfo vci{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = range,
        };
        if (vkCreateImageView(dev, &vci, nullptr, &view) != VK_SUCCESS) {
            res.error = "vkCreateImageView failed";
            break;
        }

        // ---- readback buffer (host-visible) ----
        const VkBufferCreateInfo bci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = imgBytes,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        if (vkCreateBuffer(dev, &bci, nullptr, &buffer) != VK_SUCCESS) {
            res.error = "vkCreateBuffer failed";
            break;
        }
        VkMemoryRequirements br{};
        vkGetBufferMemoryRequirements(dev, buffer, &br);
        const std::uint32_t bt = ctx->findMemoryType(
            br.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (bt == UINT32_MAX) {
            res.error = "no host-visible memory type";
            break;
        }
        const VkMemoryAllocateInfo bai{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = br.size,
            .memoryTypeIndex = bt,
        };
        if (vkAllocateMemory(dev, &bai, nullptr, &bufMem) != VK_SUCCESS ||
            vkBindBufferMemory(dev, buffer, bufMem, 0) != VK_SUCCESS) {
            res.error = "buffer memory alloc/bind failed";
            break;
        }

        // ---- descriptor set layout + pool + set ----
        const VkDescriptorSetLayoutBinding binding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        };
        const VkDescriptorSetLayoutCreateInfo slci{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &binding,
        };
        if (vkCreateDescriptorSetLayout(dev, &slci, nullptr, &setLayout) != VK_SUCCESS) {
            res.error = "vkCreateDescriptorSetLayout failed";
            break;
        }
        const VkDescriptorPoolSize poolSize{
            .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
        };
        const VkDescriptorPoolCreateInfo dpci{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1,
            .poolSizeCount = 1,
            .pPoolSizes = &poolSize,
        };
        if (vkCreateDescriptorPool(dev, &dpci, nullptr, &descPool) != VK_SUCCESS) {
            res.error = "vkCreateDescriptorPool failed";
            break;
        }
        VkDescriptorSet descSet = VK_NULL_HANDLE;
        const VkDescriptorSetAllocateInfo dsai{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = descPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &setLayout,
        };
        if (vkAllocateDescriptorSets(dev, &dsai, &descSet) != VK_SUCCESS) {
            res.error = "vkAllocateDescriptorSets failed";
            break;
        }
        const VkDescriptorImageInfo dii{
            .sampler = VK_NULL_HANDLE,
            .imageView = view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkWriteDescriptorSet wds{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descSet,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &dii,
        };
        vkUpdateDescriptorSets(dev, 1, &wds, 0, nullptr);

        // ---- pipeline layout (+ push constant) + shader module + compute pipeline ----
        const VkPushConstantRange pcr{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(PushConstants),
        };
        const VkPipelineLayoutCreateInfo plci{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &setLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pcr,
        };
        if (vkCreatePipelineLayout(dev, &plci, nullptr, &pipeLayout) != VK_SUCCESS) {
            res.error = "vkCreatePipelineLayout failed";
            break;
        }
        const VkShaderModuleCreateInfo smci{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = shaders::fill_comp_size,
            .pCode = shaders::fill_comp,
        };
        if (vkCreateShaderModule(dev, &smci, nullptr, &shaderModule) != VK_SUCCESS) {
            res.error = "vkCreateShaderModule failed";
            break;
        }
        const VkComputePipelineCreateInfo cpci{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = shaderModule,
                      .pName = "main"},
            .layout = pipeLayout,
        };
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline) !=
            VK_SUCCESS) {
            res.error = "vkCreateComputePipelines failed";
            break;
        }

        // ---- record + dispatch ----
        const VkCommandBufferAllocateInfo cai{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (vkAllocateCommandBuffers(dev, &cai, &cmd) != VK_SUCCESS) {
            res.error = "vkAllocateCommandBuffers failed";
            break;
        }
        const VkCommandBufferBeginInfo cbi{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        vkBeginCommandBuffer(cmd, &cbi);

        VkImageMemoryBarrier toGeneral{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toGeneral);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &descSet, 0,
                                nullptr);
        const PushConstants pc{{static_cast<float>(color.r) / 255.0f,
                                static_cast<float>(color.g) / 255.0f,
                                static_cast<float>(color.b) / 255.0f,
                                static_cast<float>(color.a) / 255.0f}};
        vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);

        VkImageMemoryBarrier toSrc{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

        const VkBufferImageCopy region{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {w, h, 1},
        };
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

        VkBufferMemoryBarrier toHost{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffer,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                             nullptr, 1, &toHost, 0, nullptr);

        vkEndCommandBuffer(cmd);

        const VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS) {
            res.error = "vkCreateFence failed";
            break;
        }
        const VkSubmitInfo si{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd,
        };
        if (ctx->submit(si, fence) != VK_SUCCESS) {
            res.error = "vkQueueSubmit failed";
            break;
        }
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);

        if (vkMapMemory(dev, bufMem, 0, imgBytes, 0, &mapped) != VK_SUCCESS) {
            res.error = "vkMapMemory failed";
            break;
        }
        res.image = common::Image(w, h);
        std::memcpy(res.image.rgba.data(), mapped, static_cast<std::size_t>(imgBytes));
        ok = true;
    } while (false);

    // ---- teardown (reverse order, guarded) ----
    if (mapped) vkUnmapMemory(dev, bufMem);
    if (fence) vkDestroyFence(dev, fence, nullptr);
    if (cmd) vkFreeCommandBuffers(dev, pool, 1, &cmd);
    vkDestroyCommandPool(dev, pool, nullptr);
    if (pipeline) vkDestroyPipeline(dev, pipeline, nullptr);
    if (shaderModule) vkDestroyShaderModule(dev, shaderModule, nullptr);
    if (pipeLayout) vkDestroyPipelineLayout(dev, pipeLayout, nullptr);
    if (descPool) vkDestroyDescriptorPool(dev, descPool, nullptr);
    if (setLayout) vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
    if (view) vkDestroyImageView(dev, view, nullptr);
    if (buffer) vkDestroyBuffer(dev, buffer, nullptr);
    if (bufMem) vkFreeMemory(dev, bufMem, nullptr);
    if (image) vkDestroyImage(dev, image, nullptr);
    if (imageMem) vkFreeMemory(dev, imageMem, nullptr);

    res.validationErrors = ctx->validationErrors();
    res.ok = ok;
    return res;
}

}  // namespace mosaic::render
