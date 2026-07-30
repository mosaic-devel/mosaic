#include "render/render.hpp"

#include <cstring>

#include "render/compute_fill.hpp"
#include "render/gpu_policy.hpp"
#include "render/vulkan_context.hpp"

namespace mosaic::render {
namespace {

RenderResult cpuSolid(std::uint32_t w, std::uint32_t h, common::Color8 color) {
    RenderResult res;
    res.usedBackend = Backend::Cpu;
    if (w == 0 || h == 0) {
        res.error = "renderSolid: zero-sized image";
        return res;
    }
    res.image = common::Image(w, h);
    res.image.fill(color);
    res.ok = true;
    return res;
}

RenderResult gpuSolid(std::uint32_t w, std::uint32_t h, common::Color8 color) {
    RenderResult res;
    res.usedBackend = Backend::Gpu;
    if (w == 0 || h == 0) {
        res.error = "renderSolid: zero-sized image";
        return res;
    }

    std::string err;
    // The shared device (S60-alpha) -- a one-shot helper has no business standing up a device of
    // its own. Its command pool IS its own, though: pools are externally synchronized, so this
    // helper cannot record from the context's.
    // CPU-only mode refuses before the device is stood up (S60-b item 14) -- see compute_fill.cpp
    // for why this matters even though Backend::Cpu already routes past it.
    if (!computeLaneAllowed("solid fill", err)) {
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
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory bufMem = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void* mapped = nullptr;
    bool ok = false;

    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    do {
        // ---- offscreen image (device-local) ----
        const VkImageCreateInfo ici{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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
        if (vkAllocateMemory(dev, &iai, nullptr, &imageMem) != VK_SUCCESS) {
            res.error = "vkAllocateMemory (image) failed";
            break;
        }
        if (vkBindImageMemory(dev, image, imageMem, 0) != VK_SUCCESS) {
            res.error = "vkBindImageMemory failed";
            break;
        }

        // ---- host-visible readback buffer ----
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
        if (vkAllocateMemory(dev, &bai, nullptr, &bufMem) != VK_SUCCESS) {
            res.error = "vkAllocateMemory (buffer) failed";
            break;
        }
        if (vkBindBufferMemory(dev, buffer, bufMem, 0) != VK_SUCCESS) {
            res.error = "vkBindBufferMemory failed";
            break;
        }

        // ---- record commands ----
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

        VkImageMemoryBarrier toDst{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toDst);

        VkClearColorValue clear{};
        clear.float32[0] = static_cast<float>(color.r) / 255.0f;
        clear.float32[1] = static_cast<float>(color.g) / 255.0f;
        clear.float32[2] = static_cast<float>(color.b) / 255.0f;
        clear.float32[3] = static_cast<float>(color.a) / 255.0f;
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

        VkImageMemoryBarrier toSrc{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toSrc);

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

        // ---- submit + wait ----
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

        // ---- read back ----
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
    if (buffer) vkDestroyBuffer(dev, buffer, nullptr);
    if (bufMem) vkFreeMemory(dev, bufMem, nullptr);
    if (image) vkDestroyImage(dev, image, nullptr);
    if (imageMem) vkFreeMemory(dev, imageMem, nullptr);

    res.validationErrors = ctx->validationErrors();
    res.ok = ok;
    return res;
}

}  // namespace

std::string_view moduleName() noexcept { return "render"; }

std::string_view backendName(Backend backend) noexcept {
    switch (backend) {
        case Backend::Auto: return "auto";
        case Backend::Gpu: return "gpu";
        case Backend::GpuCompute: return "gpu-compute";
        case Backend::Cpu: return "cpu";
    }
    return "unknown";
}

std::string_view resampleFilterName(ResampleFilter f) noexcept {
    switch (f) {
        case ResampleFilter::Auto: return "auto";
        case ResampleFilter::Nearest: return "nearest";
        case ResampleFilter::Bilinear: return "bilinear";
        case ResampleFilter::Bicubic: return "bicubic";
        case ResampleFilter::Mitchell: return "mitchell";
        case ResampleFilter::Lanczos2: return "lanczos2";
        case ResampleFilter::Lanczos3: return "lanczos3";
        case ResampleFilter::Area: return "area";
        case ResampleFilter::Gaussian: return "gaussian";
        case ResampleFilter::Supersample: return "supersample";
    }
    return "unknown";
}

RenderResult renderSolid(std::uint32_t w, std::uint32_t h, common::Color8 color, Backend backend) {
    switch (backend) {
        case Backend::Cpu:
            return cpuSolid(w, h, color);
        case Backend::Gpu:
            return gpuSolid(w, h, color);
        case Backend::GpuCompute:
            return computeSolid(w, h, color);
        case Backend::Auto: {
            RenderResult gpu = gpuSolid(w, h, color);
            if (gpu.ok) return gpu;
            return cpuSolid(w, h, color);  // graceful fallback
        }
    }
    return cpuSolid(w, h, color);
}

}  // namespace mosaic::render
