#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <vulkan/vulkan.h>

#include "render/gpu_caps.hpp"

namespace mosaic::render {

// A minimal headless Vulkan context: an instance (with optional validation), a physical
// device exposing a graphics-capable queue family, a logical device, that queue, and a
// reset-able command pool. There is no swapchain/surface -- this is for offscreen and
// compute work (the windowed path arrives with the UI in S3).
//
// ---- SHARING (S60-alpha, docs/s60-performance-plan.md section 1.2) ---------------------------
//
// Mosaic used to create a SEPARATE device per GPU lane -- GpuCompositor, BlurGpu, TextureGpu,
// ExtrudeGpu and the two per-call helpers each called create(). Five devices meant five VMA
// allocators, no shared memory budget (nothing could know the GPU was full), and every hand-off
// between lanes round-tripping through host memory. `shared()` is the fix: ONE device that every
// compute lane borrows.
//
// Sharing a device makes two things the caller's problem that per-lane devices hid:
//
//   * VkCommandPool is EXTERNALLY SYNCHRONIZED. Two lanes allocating or recording from one pool
//     concurrently is undefined behaviour, so a lane must own its own -- see createCommandPool().
//     commandPool() below is the context's own and is NOT for lane use.
//   * VkQueue submission is EXTERNALLY SYNCHRONIZED too. submit() and waitIdle() take the
//     context's queue mutex, so a lane cannot forget the lock by calling vkQueueSubmit directly.
//
// ---- WHICH DEVICE (S60-f, docs/s60-performance-plan.md section 9) -----------------------------
//
// No longer "the first discrete-looking one". Every enumerated device is described and LOGGED, and
// the pick is `render::pickPhysicalDevice` (gpu_caps.hpp) run against the process-wide
// `render::deviceSelector()` -- what `--device <index|name>` / `MOSAIC_DEVICE` set. The selector is
// a global rather than a `create()` argument for the same reason `gpuPolicy()` is: `shared()` is
// reached from a dozen lanes, from --bench and from every GPU test.
//
// Created through a factory and owned via smart pointer; non-copyable and non-movable so the raw
// Vulkan handles have a single, clear owner.
class VulkanContext {
public:
    // The process-wide shared compute context, created on first use and kept alive by every
    // borrower's shared_ptr -- so the device necessarily outlives every lane built on it, whatever
    // static destruction order the lanes end up with.
    //
    // ⚠ Validation is decided by the FIRST caller and is a process-wide property thereafter (it is
    // an instance-creation flag; it cannot be retrofitted). A later caller asking for validation
    // when the existing context has none gets a one-time warning rather than a second device.
    static std::shared_ptr<VulkanContext> shared(bool enableValidation, std::string& error);

    // A PRIVATE, unshared context. Prefer shared(); this exists for the cases that genuinely need
    // isolation -- chiefly a test that wants a device of its own.
    static std::unique_ptr<VulkanContext> create(bool enableValidation, std::string& error);

    // ---- ADOPTION (S60-a): a context over a device SOMEBODY ELSE created -----------------------
    //
    // The resident tiled compositor's accumulator must live on the device that PRESENTS it --
    // Vulkan images do not cross VkDevice boundaries without external-memory extensions, which are
    // not in the 1.0 core this arc floors on. `WindowRenderer` necessarily owns that device: it is
    // the only code that has a surface, and a present-capable queue family cannot be chosen
    // without one. So rather than teaching `shared()` about surfaces -- which would mean either
    // requiring a window before any headless lane runs (false for --bench, --composite-demo and
    // every unit test) or destroying a device that lanes already hold pipelines on -- the window's
    // device is WRAPPED here and handed to the lane.
    //
    // The returned context BORROWS `instance`/`physicalDevice`/`device`/`queue`: its destructor
    // destroys only what it created itself (its own command pool). The adopter must outlive it.
    //
    // ⚠ The queue is shared with the adopter, and this object's mutex cannot see the adopter's own
    // vkQueueSubmit calls. Adopter and borrower must therefore submit from ONE thread -- for
    // WindowRenderer that is the UI thread, which is where both the frame loop and the compositor
    // already run. Two threads would need the adopter to route its submits through here too.
    static std::shared_ptr<VulkanContext> adopt(VkInstance instance,
                                                VkPhysicalDevice physicalDevice, VkDevice device,
                                                VkQueue queue, std::uint32_t queueFamily,
                                                const GpuCaps& caps, std::string& error);

    // False when this context borrows its device (see adopt()). Diagnostics and tests.
    [[nodiscard]] bool ownsDevice() const noexcept { return m_ownsHandles; }

    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&&) = delete;
    VulkanContext& operator=(VulkanContext&&) = delete;

    [[nodiscard]] VkInstance instance() const noexcept { return m_instance; }
    [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept { return m_physicalDevice; }
    [[nodiscard]] VkDevice device() const noexcept { return m_device; }
    [[nodiscard]] VkQueue queue() const noexcept { return m_queue; }
    [[nodiscard]] std::uint32_t queueFamily() const noexcept { return m_queueFamily; }
    // The context's OWN pool. Under sharing a lane must not record from this -- call
    // createCommandPool() and own the result.
    [[nodiscard]] VkCommandPool commandPool() const noexcept { return m_commandPool; }

    // A fresh reset-able command pool on this device's queue family, owned and destroyed BY THE
    // CALLER (vkDestroyCommandPool). One per lane: command pools are externally synchronized, so
    // lanes sharing a device must not share a pool. Returns VK_NULL_HANDLE on failure.
    [[nodiscard]] VkCommandPool createCommandPool(std::string& error) const;

    // Submit under the context's queue lock. Every lane goes through here rather than calling
    // vkQueueSubmit itself, so sharing a queue cannot silently become a data race.
    [[nodiscard]] VkResult submit(const VkSubmitInfo& info, VkFence fence) const;

    // vkDeviceWaitIdle under the same lock (it is queue access, and equally externally
    // synchronized). Note it now waits for EVERY lane's work, not just the caller's -- correct,
    // if conservative.
    VkResult waitIdle() const;

    [[nodiscard]] std::string deviceName() const;
    [[nodiscard]] std::uint32_t validationErrors() const noexcept { return m_validationErrors; }

    // What this device can actually do (S60-alpha). Lanes ask this before assuming anything --
    // and they ask about a CAPABILITY, never about the Vulkan version.
    [[nodiscard]] const GpuCaps& caps() const noexcept { return m_caps; }

    // Index of a memory type satisfying `typeBits` and `props`, or UINT32_MAX if none.
    [[nodiscard]] std::uint32_t findMemoryType(std::uint32_t typeBits,
                                               VkMemoryPropertyFlags props) const;

private:
    VulkanContext() = default;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    std::uint32_t m_queueFamily = 0;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    std::uint32_t m_validationErrors = 0;
    GpuCaps m_caps;
    // False for an adopted context: the instance/device/queue belong to somebody else and this
    // destructor must not touch them (the command pool is still ours). See adopt().
    bool m_ownsHandles = true;
    mutable std::mutex m_queueMutex; // guards every VkQueue operation (see submit/waitIdle)
};

}  // namespace mosaic::render
