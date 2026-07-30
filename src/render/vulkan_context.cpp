#include "render/vulkan_context.hpp"

#include "common/log.hpp"
#include "render/gpu_caps.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mosaic::render {
namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

auto renderLog() { return common::log::category("render"); }

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData) {
    static const auto log = common::log::category("render");
    const char* msg = (data != nullptr && data->pMessage != nullptr) ? data->pMessage : "(null)";
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        log->error("{}", msg);
        if (userData != nullptr) ++*static_cast<std::uint32_t*>(userData);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        log->warn("{}", msg);
    } else {
        log->debug("{}", msg);
    }
    return VK_FALSE;
}

bool hasLayer(const char* name) {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> props(count);
    vkEnumerateInstanceLayerProperties(&count, props.data());
    for (const auto& p : props) {
        if (std::strcmp(p.layerName, name) == 0) return true;
    }
    return false;
}

std::vector<std::string> instanceExtensionNames() {
    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());
    std::vector<std::string> names;
    names.reserve(count);
    for (const auto& p : props) names.emplace_back(p.extensionName);
    return names;
}

bool hasInstanceExtension(const std::vector<std::string>& names, const char* name) {
    for (const auto& n : names) {
        if (n == name) return true;
    }
    return false;
}

// Returns the index of a graphics-capable queue family, or UINT32_MAX if none.
std::uint32_t findGraphicsFamily(VkPhysicalDevice dev) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
    std::vector<VkQueueFamilyProperties> fams(count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, fams.data());
    for (std::uint32_t i = 0; i < count; ++i) {
        if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) return i;
    }
    return UINT32_MAX;
}

}  // namespace

std::unique_ptr<VulkanContext> VulkanContext::create(bool enableValidation, std::string& error) {
    auto ctx = std::unique_ptr<VulkanContext>(new VulkanContext());

    const std::vector<std::string> availableExts = instanceExtensionNames();
    const bool useValidation = enableValidation && hasLayer(kValidationLayer);
    const bool useDebugUtils =
        useValidation && hasInstanceExtension(availableExts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    // S60-alpha: ask the loader what it has instead of demanding 1.2. See requestedApiVersion()
    // -- this field is a ceiling, not a requirement, so a 1.0 loader must get 1.0 here or
    // vkCreateInstance fails, while a capable loader must get its real version or our tiers are
    // unreachable.
    const std::uint32_t apiVersion = requestedApiVersion();
    std::vector<const char*> layers;
    std::vector<const char*> extensions =
        probeInstanceExtensions(apiVersion, availableExts); // features2 below 1.1
    if (useValidation) layers.push_back(kValidationLayer);
    if (useDebugUtils) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    const VkApplicationInfo appInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Mosaic",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName = "Mosaic",
        .engineVersion = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion = apiVersion,
    };
    const VkInstanceCreateInfo ici{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = static_cast<std::uint32_t>(layers.size()),
        .ppEnabledLayerNames = layers.data(),
        .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    if (vkCreateInstance(&ici, nullptr, &ctx->m_instance) != VK_SUCCESS) {
        error = "vkCreateInstance failed (no Vulkan runtime?)";
        return nullptr;
    }

    if (useDebugUtils) {
        auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(ctx->m_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createMessenger) {
            const VkDebugUtilsMessengerCreateInfoEXT mci{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                .pfnUserCallback = debugCallback,
                .pUserData = &ctx->m_validationErrors,
            };
            createMessenger(ctx->m_instance, &mci, nullptr, &ctx->m_messenger);
        }
    }

    // ---- device enumeration + pick (S60-f, docs/s60-performance-plan.md section 9) -------------
    // Every enumerated device is described into plain data and LOGGED, then ranked by the pure
    // `pickPhysicalDevice`. The readout is the point as much as the pick is: a hybrid laptop that
    // lands on the wrong part used to look exactly like a slow application, with nothing in the
    // log to say what else was on offer.
    std::uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(ctx->m_instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        error = "no Vulkan physical devices found";
        return nullptr;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(ctx->m_instance, &deviceCount, devices.data());

    const bool hasProps2 =
        hasInstanceExtension(availableExts, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    std::vector<DeviceInfo> infos;
    std::vector<std::uint32_t> families(deviceCount, UINT32_MAX);
    infos.reserve(deviceCount);
    for (std::uint32_t i = 0; i < deviceCount; ++i) {
        DeviceInfo info = describePhysicalDevice(ctx->m_instance, devices[i], i, apiVersion,
                                                 hasProps2);
        // "Usable" for a COMPUTE context is a graphics queue family and nothing else -- there is
        // no surface here, so presentation is not this caller's question.
        families[i] = findGraphicsFamily(devices[i]);
        if (families[i] == UINT32_MAX) {
            info.usable = false;
            info.unusableReason = "no graphics queue family";
        }
        infos.push_back(std::move(info));
    }

    const DeviceChoice choice = pickPhysicalDevice(infos, deviceSelector());
    logDeviceSelection("compute", infos, choice);
    if (choice.index < 0) {
        error = "no Vulkan device with a graphics queue";
        return nullptr;
    }
    const auto chosenIndex = static_cast<std::size_t>(choice.index);
    const VkPhysicalDevice chosen = devices[chosenIndex];
    const std::uint32_t chosenFamily = families[chosenIndex];
    ctx->m_physicalDevice = chosen;
    ctx->m_queueFamily = chosenFamily;

    // ---- capability probe (S60-alpha) ----
    // Probe -> optional floor clamp -> decide. Everything downstream reads ctx->caps(); nothing
    // downstream is allowed to re-derive a capability from the version number.
    GpuProbe probe = probePhysicalDevice(ctx->m_instance, chosen, apiVersion, hasProps2);
    const bool floored = applyProfileFromEnv(probe);
    ctx->m_caps = decide(probe);
    if (floored)
        renderLog()->info("gpu profile=floor (synthetic Vulkan 1.0 minimums)");
    renderLog()->info("gpu: {}", ctx->m_caps.summary());
    // The long form only where somebody asked for it: at info level it would push the negotiated
    // limits past the point anyone reads them, and it is a dozen lines.
    renderLog()->debug("gpu caps:\n{}", ctx->m_caps.readout());

    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo qci{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = chosenFamily,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    // Every name here was seen in the probe, so vkCreateDevice cannot fail for asking; the
    // feature chain must outlive the call, hence the named local.
    const std::vector<const char*> deviceExts =
        deviceExtensionsFor(ctx->m_caps, /*needSwapchain=*/false);
    const GpuFeatureChain features{ctx->m_caps};
    const VkDeviceCreateInfo dci{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = features.pNext(),
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = static_cast<std::uint32_t>(deviceExts.size()),
        .ppEnabledExtensionNames = deviceExts.data(),
        .pEnabledFeatures = &features.coreFeatures(),
    };
    if (vkCreateDevice(chosen, &dci, nullptr, &ctx->m_device) != VK_SUCCESS) {
        error = "vkCreateDevice failed";
        return nullptr;
    }
    vkGetDeviceQueue(ctx->m_device, chosenFamily, 0, &ctx->m_queue);

    const VkCommandPoolCreateInfo pci{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = chosenFamily,
    };
    if (vkCreateCommandPool(ctx->m_device, &pci, nullptr, &ctx->m_commandPool) != VK_SUCCESS) {
        error = "vkCreateCommandPool failed";
        return nullptr;
    }

    return ctx;
}

VulkanContext::~VulkanContext() {
    if (m_device != VK_NULL_HANDLE) {
        if (m_commandPool != VK_NULL_HANDLE) {
            // Ours in both cases: an adopted context still creates its own pool (pools are
            // externally synchronized, so borrowing the adopter's would be a data race).
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        }
        if (m_ownsHandles) vkDestroyDevice(m_device, nullptr);
    }
    if (!m_ownsHandles) return;  // the messenger and instance belong to the adopter
    if (m_messenger != VK_NULL_HANDLE) {
        auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyMessenger) destroyMessenger(m_instance, m_messenger, nullptr);
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
    }
}

std::shared_ptr<VulkanContext> VulkanContext::shared(bool enableValidation, std::string& error) {
    // Guarded so two lanes racing to first-use cannot build two devices -- which is the exact bug
    // this function exists to remove.
    static std::mutex mutex;
    static std::weak_ptr<VulkanContext> weak;
    static bool sharedValidation = false;
    static bool warnedValidation = false;
    const std::lock_guard<std::mutex> lock(mutex);

    if (auto existing = weak.lock()) {
        if (enableValidation && !sharedValidation && !warnedValidation) {
            warnedValidation = true;
            renderLog()->warn(
                "shared Vulkan context already exists without validation; this caller's request "
                "for it is ignored (validation is an instance-creation flag)");
        }
        return existing;
    }

    // create() hands back a unique_ptr; convert rather than duplicating the construction logic.
    std::unique_ptr<VulkanContext> made = create(enableValidation, error);
    if (!made)
        return nullptr;
    std::shared_ptr<VulkanContext> ctx{std::move(made)};
    sharedValidation = enableValidation;
    warnedValidation = false;
    weak = ctx;
    return ctx;
}

std::shared_ptr<VulkanContext> VulkanContext::adopt(VkInstance instance,
                                                    VkPhysicalDevice physicalDevice,
                                                    VkDevice device, VkQueue queue,
                                                    std::uint32_t queueFamily, const GpuCaps& caps,
                                                    std::string& error) {
    if (instance == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE ||
        device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
        error = "VulkanContext::adopt given a null handle";
        return nullptr;
    }
    auto ctx = std::shared_ptr<VulkanContext>(new VulkanContext());
    ctx->m_ownsHandles = false;
    ctx->m_instance = instance;
    ctx->m_physicalDevice = physicalDevice;
    ctx->m_device = device;
    ctx->m_queue = queue;
    ctx->m_queueFamily = queueFamily;
    ctx->m_caps = caps;
    // The caps come from the adopter's own probe rather than being re-derived: probing twice could
    // disagree (MOSAIC_GPU_PROFILE=floor is applied at the adopter's probe site), and a lane whose
    // idea of the device differs from the presenter's is exactly the class of bug the one-caps
    // rule exists to remove.
    const VkCommandPoolCreateInfo pci{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queueFamily,
    };
    if (vkCreateCommandPool(device, &pci, nullptr, &ctx->m_commandPool) != VK_SUCCESS) {
        error = "vkCreateCommandPool failed on the adopted device";
        ctx->m_commandPool = VK_NULL_HANDLE;
        return nullptr;
    }
    renderLog()->info("gpu: adopted the presenting device for compute ({})", ctx->deviceName());
    return ctx;
}

VkCommandPool VulkanContext::createCommandPool(std::string& error) const {
    const VkCommandPoolCreateInfo pci{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = m_queueFamily,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(m_device, &pci, nullptr, &pool) != VK_SUCCESS) {
        error = "vkCreateCommandPool failed";
        return VK_NULL_HANDLE;
    }
    return pool;
}

VkResult VulkanContext::submit(const VkSubmitInfo& info, VkFence fence) const {
    const std::lock_guard<std::mutex> lock(m_queueMutex);
    return vkQueueSubmit(m_queue, 1, &info, fence);
}

VkResult VulkanContext::waitIdle() const {
    const std::lock_guard<std::mutex> lock(m_queueMutex);
    return vkDeviceWaitIdle(m_device);
}

std::string VulkanContext::deviceName() const {
    if (m_physicalDevice == VK_NULL_HANDLE) return "(none)";
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
    return props.deviceName;
}

std::uint32_t VulkanContext::findMemoryType(std::uint32_t typeBits,
                                            VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
    for (std::uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        const bool typeOk = (typeBits & (1u << i)) != 0;
        const bool propsOk = (memProps.memoryTypes[i].propertyFlags & props) == props;
        if (typeOk && propsOk) return i;
    }
    return UINT32_MAX;
}

}  // namespace mosaic::render
