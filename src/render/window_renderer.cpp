#include "render/window_renderer.hpp"

#include "common/log.hpp"
#include "common/profiler.hpp" // MOSAIC_PERF_SCOPE: the drag uploads used to be invisible (G5)
#include "render/gpu_budget.hpp"
#include "render/gpu_caps.hpp"
#include "render/gpu_timer.hpp" // Lane::GpuDevice twins for the present chain's CPU rows
#include "render/vulkan_context.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <shaders/canvas_drag_composite.comp.spv.hpp>
#include <shaders/canvas_idle.comp.spv.hpp>
#include <shaders/canvas_present.comp.spv.hpp>
#include <vector>
#include <vk_mem_alloc.h>

// Vulkan WSI: the platform surface headers reference native types and require their windowing
// header first. On macOS the only surface is VK_EXT_metal_surface (MoltenVK), which needs no
// native windowing header -- the CAMetalLayer arrives as an opaque pointer from platform/.
// On Windows the only surface is VK_KHR_win32_surface, whose HINSTANCE/HWND come from <windows.h>
// (the toolchain defines NOMINMAX globally, so it cannot clobber the std::min/std::max this file
// uses throughout). On Linux, order matters: include wayland-client before X11/Xlib so X11's
// preprocessor names (None, Status, Bool, ...) cannot clobber anything above. This TU never
// includes FLTK, so the X11 macro pollution stays contained. (clang-format must not sort it.)
//
// None of the three needs VK_USE_PLATFORM_* defined: that macro only gates the convenience includes
// inside <vulkan/vulkan.h>, and the per-platform headers below carry no guard of their own -- which
// is why naming both halves here is the whole mechanism on every platform.
// clang-format off
#ifdef __APPLE__
#include <vulkan/vulkan_metal.h>
#elif defined(_WIN32)
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#else
#include <wayland-client.h>
#include <vulkan/vulkan_wayland.h>

#include <X11/Xlib.h>
#include <vulkan/vulkan_xlib.h>
#endif
// clang-format on

// LAST on purpose: this header defines the function-like macro _(), and nothing below it may be a
// system header. The software-device notice (takeStartupNotice) is the only user-facing string
// this TU produces, and it is a sentence the user reads, so it is translated like any other.
#include "common/i18n.hpp"

namespace mosaic::render {
namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

auto renderLog() { return common::log::category("render"); }

// Mirrors the push_constant block in canvas_present.comp (vec4 first for 16-byte alignment, then
// tightly-packed vec2s, then a 16-aligned vec4 and a trailing vec2 -> 88 bytes, no std430
// padding).
struct PresentPush {
    float bgColor[4]; // .rgb canvas bg (straight 0..1); .a = pixel-grid toggle (S19-c, see shader)
    float invR0[2];
    float invR1[2];
    float invT[2];
    float docSize[2];
    float outSize[2];
    float overlayCenter[2];
    float overlay[4]; // active(0/1), radius px, angle rad, integer degrees
    float ants[4]; // ants active(0/1), dash phase px, controls mode (0/1 move/2 crop), half-size
    float hc01[4]; // controls quad corners TL,TR (physical px; S15 Move / S16 crop)
    float hc23[4]; // corners BR,BL
};
static_assert(sizeof(PresentPush) == 128, "exactly the guaranteed Vulkan push-constant budget");

// Mirrors the push_constant block in canvas_drag_composite.comp (S60-a): the dragged layer's
// inverse world transform (doc px -> layer-local px), sizes, blend mode and opacity.
struct DragPush {
    float invR0[2];
    float invR1[2];
    float invT[2];
    float docSize[2];
    float draggedSize[2];
    int mode;
    float opacity;
};
static_assert(sizeof(DragPush) == 48, "matches the std430 push block in canvas_drag_composite.comp");

// Mirrors the push_constant block in canvas_idle.comp (the documentless idle pass): vec4s first
// for 16-byte alignment, then tightly-packed vec2s and scalars -- no std430 padding.
struct IdlePush {
    float bgColor[4]; // .rgb canvas bg (straight 0..1); .a = mode (0 standalone, 1 blend-over)
    float ink[4];     // .rgb dot ink; .a = field fade (scales amplitude AND alpha -- the settle)
    float accent[4];  // .rgb accent; .a = drag-hot
    float outSize[2];
    float timePhase;
    float scale;      // logical -> device px (ripple wavelengths are authored in logical px)
    float center[2];  // invitation centre, device px
    float quiet[2];   // quiet-zone half extents, device px (< 0 disables)
    float invPos[2];  // invitation quad top-left, device px
    float invSize[2]; // invitation quad (== atlas row) size, device px
    float invAlpha;
    float hover;
    float amp;
    float pitch;
};
static_assert(sizeof(IdlePush) == 112, "matches the push block in canvas_idle.comp");

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void* userData) {
    static const auto log = common::log::category("render");
    const char* msg = (data != nullptr && data->pMessage != nullptr) ? data->pMessage : "(null)";
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        log->error("{}", msg);
        if (userData != nullptr)
            ++*static_cast<std::uint32_t*>(userData);
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
        if (std::strcmp(p.layerName, name) == 0)
            return true;
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
    for (const auto& p : props)
        names.emplace_back(p.extensionName);
    return names;
}

bool hasInstanceExtension(const std::vector<std::string>& names, const char* name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool hasDeviceExtension(VkPhysicalDevice dev, const char* name) {
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, props.data());
    for (const auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0)
            return true;
    }
    return false;
}

// First queue family with both graphics and present support, or UINT32_MAX if none.
std::uint32_t findGraphicsPresentFamily(VkPhysicalDevice dev, VkSurfaceKHR surface) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
    std::vector<VkQueueFamilyProperties> fams(count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, fams.data());
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            continue;
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &present);
        if (present)
            return i;
    }
    return UINT32_MAX;
}

} // namespace

BlitRect fitCentered(std::uint32_t srcW, std::uint32_t srcH, std::uint32_t dstW,
                     std::uint32_t dstH) {
    if (srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0)
        return {};
    const double scale =
        std::min(static_cast<double>(dstW) / srcW, static_cast<double>(dstH) / srcH);
    const auto w = std::max(1, static_cast<int>(std::lround(srcW * scale)));
    const auto h = std::max(1, static_cast<int>(std::lround(srcH * scale)));
    return {(static_cast<int>(dstW) - w) / 2, (static_cast<int>(dstH) - h) / 2, w, h};
}

// ---- Admission for the presenting device's own textures (see the header) ----------------------

std::string_view textureRefusalName(TextureRefusal r) noexcept {
    switch (r) {
    case TextureRefusal::Admitted: return "none";
    case TextureRefusal::NoDevice: return "no Vulkan device";
    case TextureRefusal::EmptySource: return "empty source image";
    case TextureRefusal::OverMaxImageDim: return "over maxImageDimension2D";
    case TextureRefusal::OverMemoryBudget: return "over the device memory budget";
    }
    return "unknown";
}

TextureRefusal admitCanvasTexture(const GpuCaps& caps, std::uint32_t w, std::uint32_t h) noexcept {
    if (w == 0 || h == 0) return TextureRefusal::EmptySource;
    if (!caps.fitsImage(w, h)) return TextureRefusal::OverMaxImageDim;
    return TextureRefusal::Admitted;
}

TextureRefusal admitDragTextures(const GpuCaps& caps, const GpuMemoryBudget& mem,
                                 std::uint32_t belowW, std::uint32_t belowH, std::uint32_t dragW,
                                 std::uint32_t dragH) noexcept {
    if (belowW == 0 || belowH == 0 || dragW == 0 || dragH == 0)
        return TextureRefusal::EmptySource;
    // Dimensions first: that refusal is a property of the hardware and is true whatever memory
    // says, so it must not be shadowed by the abstain below on a device nobody has snapshotted.
    if (!caps.fitsImage(belowW, belowH) || !caps.fitsImage(dragW, dragH))
        return TextureRefusal::OverMaxImageDim;
    if (mem.budget == 0)
        return TextureRefusal::Admitted; // no snapshot: abstain rather than refuse (see the header)
    const std::uint64_t budget =
        atlasBudgetBytes(mem, caps, kDragTextureHeadroom, kDragTextureHardCapBytes, /*minBytes=*/0);
    if (dragTextureCost(belowW, belowH, dragW, dragH).peakBytes() > budget)
        return TextureRefusal::OverMemoryBudget;
    return TextureRefusal::Admitted;
}

std::unique_ptr<WindowRenderer> WindowRenderer::create(const platform::NativeSurfaceHandle& handle,
                                                       bool enableValidation, std::string& error) {
    auto self = std::unique_ptr<WindowRenderer>(new WindowRenderer());
    self->m_hintWidth = handle.pixelWidth;
    self->m_hintHeight = handle.pixelHeight;

    const std::vector<std::string> availableExts = instanceExtensionNames();
    const bool useValidation = enableValidation && hasLayer(kValidationLayer);
    const bool useDebugUtils =
        useValidation && hasInstanceExtension(availableExts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

#ifdef __APPLE__
    const char* surfaceExt = VK_EXT_METAL_SURFACE_EXTENSION_NAME;
#elif defined(_WIN32)
    const char* surfaceExt = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#else
    const char* surfaceExt = (handle.system == platform::WindowSystem::Wayland)
                                 ? VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME
                                 : VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
#endif
    if (!hasInstanceExtension(availableExts, VK_KHR_SURFACE_EXTENSION_NAME) ||
        !hasInstanceExtension(availableExts, surfaceExt)) {
        error = std::string("required surface extension missing: ") + surfaceExt;
        return nullptr;
    }

    // S60-alpha: the loader's version, not a hard-coded 1.2. See requestedApiVersion().
    const std::uint32_t apiVersion = requestedApiVersion();
    std::vector<const char*> layers;
    std::vector<const char*> extensions{VK_KHR_SURFACE_EXTENSION_NAME, surfaceExt};
    for (const char* e : probeInstanceExtensions(apiVersion, availableExts)) // features2 below 1.1
        extensions.push_back(e);
    if (useValidation)
        layers.push_back(kValidationLayer);
    if (useDebugUtils)
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    // MoltenVK is a "portability" driver: the loader only enumerates it when the instance opts in
    // via VK_KHR_portability_enumeration + the ENUMERATE_PORTABILITY flag. Harmless no-op elsewhere.
    VkInstanceCreateFlags instanceFlags = 0;
#ifdef __APPLE__
    if (hasInstanceExtension(availableExts, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        instanceFlags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif

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
        .flags = instanceFlags,
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = static_cast<std::uint32_t>(layers.size()),
        .ppEnabledLayerNames = layers.data(),
        .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    if (vkCreateInstance(&ici, nullptr, &self->m_instance) != VK_SUCCESS) {
        error = "vkCreateInstance failed (no Vulkan runtime?)";
        return nullptr;
    }

    if (useDebugUtils) {
        auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(self->m_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createMessenger) {
            const VkDebugUtilsMessengerCreateInfoEXT mci{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                .pfnUserCallback = debugCallback,
                .pUserData = &self->m_validationErrors,
            };
            createMessenger(self->m_instance, &mci, nullptr, &self->m_messenger);
        }
    }

    // ---- surface ----
#ifdef __APPLE__
    {
        // macOS: the CAMetalLayer from native_window_macos.mm feeds VK_EXT_metal_surface (MoltenVK).
        const VkMetalSurfaceCreateInfoEXT sci{
            .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
            .pLayer = static_cast<const CAMetalLayer*>(handle.window),
        };
        if (vkCreateMetalSurfaceEXT(self->m_instance, &sci, nullptr, &self->m_surface) !=
            VK_SUCCESS) {
            error = "vkCreateMetalSurfaceEXT failed";
            return nullptr;
        }
    }
#elif defined(_WIN32)
    {
        // Windows: the HWND from native_window_win32.cpp, plus the process HINSTANCE it carries in
        // `display` -- there is no display connection on this platform to stand in that field.
        const VkWin32SurfaceCreateInfoKHR sci{
            .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
            .hinstance = static_cast<HINSTANCE>(handle.display),
            .hwnd = static_cast<HWND>(handle.window),
        };
        if (vkCreateWin32SurfaceKHR(self->m_instance, &sci, nullptr, &self->m_surface) !=
            VK_SUCCESS) {
            error = "vkCreateWin32SurfaceKHR failed";
            return nullptr;
        }
    }
#else
    if (handle.system == platform::WindowSystem::Wayland) {
        const VkWaylandSurfaceCreateInfoKHR sci{
            .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
            .display = static_cast<struct wl_display*>(handle.display),
            .surface = static_cast<struct wl_surface*>(handle.window),
        };
        if (vkCreateWaylandSurfaceKHR(self->m_instance, &sci, nullptr, &self->m_surface) !=
            VK_SUCCESS) {
            error = "vkCreateWaylandSurfaceKHR failed";
            return nullptr;
        }
    } else {
        const VkXlibSurfaceCreateInfoKHR sci{
            .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
            .dpy = static_cast<Display*>(handle.display),
            .window = static_cast<Window>(reinterpret_cast<std::uintptr_t>(handle.window)),
        };
        if (vkCreateXlibSurfaceKHR(self->m_instance, &sci, nullptr, &self->m_surface) !=
            VK_SUCCESS) {
            error = "vkCreateXlibSurfaceKHR failed";
            return nullptr;
        }
    }
#endif

    // ---- physical device with a graphics+present family (prefer discrete) ----
    std::uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(self->m_instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        error = "no Vulkan physical devices found";
        return nullptr;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(self->m_instance, &deviceCount, devices.data());

    // ---- device enumeration + pick (S60-f, docs/s60-performance-plan.md section 9) -------------
    // The PRESENTING device goes through the same enumeration, readout and ranking as the compute
    // context, so `--device` / MOSAIC_DEVICE means one thing in this process rather than two. This
    // used to be an ad-hoc "first device that can present, preferring the first discrete one",
    // which ignored the selector entirely and logged nothing about what else was on offer -- on a
    // hybrid laptop, landing on the wrong part was indistinguishable from the application simply
    // being slow.
    const bool hasProps2 =
        hasInstanceExtension(availableExts,
                             VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    std::vector<DeviceInfo> infos;
    std::vector<std::uint32_t> families(devices.size(), UINT32_MAX);
    infos.reserve(devices.size());
    for (std::uint32_t i = 0; i < devices.size(); ++i) {
        DeviceInfo info =
            describePhysicalDevice(self->m_instance, devices[i], i, apiVersion, hasProps2);
        // "Usable" HERE is stricter than for a compute context: this device has to reach the
        // surface, so it needs the swapchain extension and a family that can both draw and present.
        // A device that fails either is still enumerated and still logged -- that it exists and was
        // passed over is exactly what somebody debugging a bad pick needs to see.
        if (!hasDeviceExtension(devices[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            info.usable = false;
            info.unusableReason = "no " VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        } else {
            families[i] = findGraphicsPresentFamily(devices[i], self->m_surface);
            if (families[i] == UINT32_MAX) {
                info.usable = false;
                info.unusableReason = "no queue family can both render and present to this surface";
            }
        }
        infos.push_back(std::move(info));
    }

    const DeviceChoice choice = pickPhysicalDevice(infos, deviceSelector());
    logDeviceSelection("present", infos, choice);
    if (choice.index < 0) {
        error = "no Vulkan device can present to this surface";
        return nullptr;
    }
    self->m_physicalDevice = devices[static_cast<std::size_t>(choice.index)];
    self->m_queueFamily = families[static_cast<std::size_t>(choice.index)];

    // ---- capability probe (S60-alpha) ----
    {
        GpuProbe probe = probePhysicalDevice(
            self->m_instance, self->m_physicalDevice, apiVersion,
            hasInstanceExtension(availableExts,
                                 VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME));
        const bool floored = applyProfileFromEnv(probe);
        self->m_caps = decide(probe);
        if (floored)
            renderLog()->info("gpu profile=floor (synthetic Vulkan 1.0 minimums)");
        renderLog()->info("gpu: {}", self->m_caps.summary());
        // Level 2 (docs/s60-performance-plan.md section 6.2): a software rasterizer is ACCEPTED --
        // it is a slow GPU, not a missing one, and every shader, the present pass and all the
        // overlays run on it unchanged. What it is NOT is silent: "Mosaic is slow" and "Mosaic is
        // running on a CPU rasterizer" are the same sentence to the user, and only one of them is
        // actionable, so it is said once, in the log AND on screen. `GpuCaps` has already applied
        // the conservative half of Level 2 by this point (128 px macrotiles, a smaller GPU budget).
        if (self->m_caps.softwareDevice) {
            renderLog()->warn("gpu: software rasterizer ({}) -- no hardware acceleration; "
                              "performance will reflect that",
                              self->m_caps.deviceName);
            self->m_startupNotice =
                _("Software rendering: no graphics driver was found, so Mosaic is drawing on the "
                  "CPU. Everything works; everything is slower.");
        }
    }

    // ---- logical device + queue ----
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo qci{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = self->m_queueFamily,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    // Every name was seen in the probe, so vkCreateDevice cannot fail for asking; the feature
    // chain holds pNext pointers into itself and must outlive the call.
    const std::vector<const char*> deviceExts =
        deviceExtensionsFor(self->m_caps, /*needSwapchain=*/true);
    const GpuFeatureChain features{self->m_caps};
    const VkDeviceCreateInfo dci{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = features.pNext(),
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = static_cast<std::uint32_t>(deviceExts.size()),
        .ppEnabledExtensionNames = deviceExts.data(),
        .pEnabledFeatures = &features.coreFeatures(),
    };
    if (vkCreateDevice(self->m_physicalDevice, &dci, nullptr, &self->m_device) != VK_SUCCESS) {
        error = "vkCreateDevice failed";
        return nullptr;
    }
    vkGetDeviceQueue(self->m_device, self->m_queueFamily, 0, &self->m_queue);

    // VMA allocator for the canvas texture (S7-c). Device memory from S7 onward goes through VMA.
    // The version handed to VMA must be the one we actually created the instance with -- telling
    // it 1.2 on a 1.0 instance would have it reach for entry points that are not there.
    const VmaAllocatorCreateInfo aci{
        .physicalDevice = self->m_physicalDevice,
        .device = self->m_device,
        .instance = self->m_instance,
        .vulkanApiVersion = apiVersion,
    };
    if (vmaCreateAllocator(&aci, &self->m_allocator) != VK_SUCCESS) {
        error = "vmaCreateAllocator failed";
        return nullptr;
    }

    const VkCommandPoolCreateInfo pci{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = self->m_queueFamily,
    };
    if (vkCreateCommandPool(self->m_device, &pci, nullptr, &self->m_commandPool) != VK_SUCCESS) {
        error = "vkCreateCommandPool failed";
        return nullptr;
    }

    if (!self->createPresentPipeline(error))
        return nullptr;
    if (!self->createDragPipeline(error))
        return nullptr;
    if (!self->createIdlePipeline(error))
        return nullptr;

    // ---- per-frame sync (single frame in flight) ----
    const VkSemaphoreCreateInfo semInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    const VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                      .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    if (vkCreateSemaphore(self->m_device, &semInfo, nullptr, &self->m_imageAvailable) !=
            VK_SUCCESS ||
        vkCreateFence(self->m_device, &fenceInfo, nullptr, &self->m_inFlight) != VK_SUCCESS) {
        error = "failed to create frame sync objects";
        return nullptr;
    }

    if (!self->createSwapchain(error))
        return nullptr;
    return self;
}

bool WindowRenderer::createSwapchain(std::string& error) {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &caps) !=
        VK_SUCCESS) {
        error = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed";
        return false;
    }

    // We clear the swapchain image directly (vkCmdClearColorImage), which needs TRANSFER_DST.
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        error = "surface does not support TRANSFER_DST swapchain usage";
        return false;
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) { // surface lets us choose (typical on Wayland)
        extent.width = std::clamp(static_cast<std::uint32_t>(std::max(m_hintWidth, 0)),
                                  caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<std::uint32_t>(std::max(m_hintHeight, 0)),
                                   caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    m_extent = extent;
    if (extent.width == 0 || extent.height == 0) {
        // Minimized / zero-size: defer swapchain creation; drawFrame() no-ops until resized.
        return true;
    }

    // Prefer a B8G8R8A8_UNORM / sRGB-nonlinear format; fall back to whatever is offered.
    std::uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr);
    if (formatCount == 0) {
        error = "surface exposes no formats";
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    m_format = chosen.format;
    m_colorSpace = chosen.colorSpace;

    std::uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    // Present mode: prefer MAILBOX (latest-frame-wins, tear-free, and acquire/present never
    // park the UI thread on vblank behind a queue of in-flight frames). Hardcoded FIFO showed
    // up as multi-frame drag latency on the X11/XWayland path while native Wayland felt
    // instant (S15.z user report — Mesa's Wayland FIFO is frame-callback based, X11's blocks).
    // FIFO is the spec-guaranteed fallback.
    //
    // ⚠ RE-AFFIRMED when the frame rate was bounded to the display's refresh (600 fps on a 200 Hz
    // panel, user report). Switching to FIFO would have been the obvious fix and is the wrong one:
    // FIFO's block is what S15.z reported, and it would now land ON THE UI THREAD INSIDE THE FRAME
    // LOOP -- the thread that also has to keep ingesting a 200 Hz pen. The rate is bounded by not
    // ASKING for a frame more often than the panel can show one (app_window's coalescer), which
    // needs no blocking present and leaves MAILBOX doing what it is good at: when the clock and the
    // vblank do disagree by a hair, the newest image wins instead of a queue building up.
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &modeCount, nullptr);
    if (modeCount > 0) {
        std::vector<VkPresentModeKHR> modes(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &modeCount,
                                                  modes.data());
        if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end())
            presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    static const auto wsiLog = common::log::category("render");
    wsiLog->debug("swapchain: {} images, {}", imageCount,
                  presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO");

    VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & composite)) {
        // Pick the lowest supported bit.
        for (std::uint32_t bit = 1; bit; bit <<= 1) {
            if (caps.supportedCompositeAlpha & bit) {
                composite = static_cast<VkCompositeAlphaFlagBitsKHR>(bit);
                break;
            }
        }
    }

    const VkSwapchainCreateInfoKHR sci{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = m_surface,
        .minImageCount = imageCount,
        .imageFormat = m_format,
        .imageColorSpace = m_colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = composite,
        .presentMode = presentMode,
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE,
    };
    if (vkCreateSwapchainKHR(m_device, &sci, nullptr, &m_swapchain) != VK_SUCCESS) {
        error = "vkCreateSwapchainKHR failed";
        return false;
    }

    std::uint32_t actual = 0;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &actual, nullptr);
    m_images.resize(actual);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &actual, m_images.data());

    // One command buffer + one render-finished semaphore per swapchain image.
    m_commandBuffers.resize(actual);
    const VkCommandBufferAllocateInfo cai{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = m_commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = actual,
    };
    if (vkAllocateCommandBuffers(m_device, &cai, m_commandBuffers.data()) != VK_SUCCESS) {
        error = "vkAllocateCommandBuffers failed";
        return false;
    }

    m_renderFinished.resize(actual, VK_NULL_HANDLE);
    const VkSemaphoreCreateInfo semInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& sem : m_renderFinished) {
        if (vkCreateSemaphore(m_device, &semInfo, nullptr, &sem) != VK_SUCCESS) {
            error = "vkCreateSemaphore (render-finished) failed";
            return false;
        }
    }

    // The intermediate present image is swapchain-extent-sized, so (re)create it here.
    if (!ensureViewImage(error))
        return false;
    return true;
}

void WindowRenderer::destroySwapchainObjects() noexcept {
    destroyViewImage(); // extent-sized, rebuilt by createSwapchain
    for (VkSemaphore sem : m_renderFinished) {
        if (sem != VK_NULL_HANDLE)
            vkDestroySemaphore(m_device, sem, nullptr);
    }
    m_renderFinished.clear();
    if (!m_commandBuffers.empty()) {
        vkFreeCommandBuffers(m_device, m_commandPool,
                             static_cast<std::uint32_t>(m_commandBuffers.size()),
                             m_commandBuffers.data());
        m_commandBuffers.clear();
    }
    m_images.clear();
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
}

bool WindowRenderer::surfaceExtentChanged() const noexcept {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &caps) != VK_SUCCESS)
        return false; // cannot tell: prefer presenting over churning the swapchain
    if (caps.currentExtent.width == UINT32_MAX)
        return false; // the surface defers to us (Wayland): it cannot have moved on its own
    return caps.currentExtent.width != m_extent.width ||
           caps.currentExtent.height != m_extent.height;
}

bool WindowRenderer::recreate(std::string& error) {
    vkDeviceWaitIdle(m_device);
    destroySwapchainObjects();
    m_needsRecreate = false;
    return createSwapchain(error);
}

void WindowRenderer::setCanvasImage(const common::Image& img) {
    m_pendingCanvas = img; // copied; uploaded on the next drawFrame
    m_hasPendingCanvas = true;
}

void WindowRenderer::setCanvasRegion(const common::Image& sub, std::uint32_t x, std::uint32_t y) {
    // Patch a sub-rectangle of the existing canvas texture on the next drawFrame (S60-a). No-op if
    // the texture isn't established yet or the rect would overflow it -- drawFrame revalidates the
    // size against the live texture and silently drops a mismatched region (the caller then issues
    // a full setCanvasImage). Copied; uploaded next frame.
    m_pendingCanvasRegion = sub;
    m_pendingCanvasRegionX = x;
    m_pendingCanvasRegionY = y;
    m_hasPendingCanvasRegion = true;
}

void WindowRenderer::setSelectionMask(std::uint32_t w, std::uint32_t h,
                                      const std::uint8_t* coverage) {
    if (w == 0 || h == 0 || coverage == nullptr) {
        // No selection: hide the ants, and stage the 1x1 zero placeholder so binding 2 stays
        // valid (and a big stale mask texture is released).
        m_pendingMask.assign(1, 0);
        m_pendingMaskW = 1;
        m_pendingMaskH = 1;
        m_antsEnabled = false;
    } else {
        m_pendingMask.assign(coverage, coverage + static_cast<std::size_t>(w) * h);
        m_pendingMaskW = w;
        m_pendingMaskH = h;
        m_antsEnabled = true;
    }
    m_hasPendingMask = true;
}

void WindowRenderer::setLassoPolyline(const std::vector<common::Vec2>& pts) {
    // Clamp to the SSBO capacity here so the per-frame upload stays a trivial copy. The latest path
    // is written into the mapped buffer in drawFrame (after the fence wait).
    if (pts.size() <= kLassoMaxVerts)
        m_lassoVerts = pts;
    else
        m_lassoVerts.assign(pts.begin(), pts.begin() + kLassoMaxVerts);
}

void WindowRenderer::setBrushReticle(bool active, common::Vec2 center, double semiX, double semiY,
                                     double angleRad, bool locked) noexcept {
    m_reticleActive = active;
    m_reticleCenter = center;
    m_reticleSemiX = semiX;
    m_reticleSemiY = semiY;
    m_reticleAngle = angleRad;
    m_reticleLocked = locked;
}

void WindowRenderer::setLoupe(bool active, common::Vec2 center, double radius, double magnification,
                              common::Vec2 sampleDocTexelCenter, common::Color8 sampleColor,
                              common::Color8 prevColor, bool readout) noexcept {
    m_loupeActive = active;
    m_loupeCenter = center;
    m_loupeRadius = radius;
    m_loupeMag = magnification;
    m_loupeSampleDoc = sampleDocTexelCenter;
    m_loupeSampleColor = sampleColor;
    m_loupePrevColor = prevColor;
    m_loupeReadout = readout;
}

void WindowRenderer::setBrushReticleSdf(std::uint64_t key, int w, int h, int pad, double boxW,
                                        double boxH, const float* data, std::size_t count) {
    // The key exists because a mouse move, a zoom, a resize and a rotation all re-drive the reticle
    // every frame, and NONE of them changes the tip's silhouette. The canvas already only calls this
    // when the field it holds has gone stale (it will not rebuild one to hand over); this is the
    // backstop that keeps a caller which re-pushes the same field from copying 78 KB per motion event.
    if (key != 0 && key == m_sdfKey)
        return;

    const bool sane = key != 0 && data != nullptr && w > 0 && h > 0 && pad >= 0 && boxW > 0.0 &&
                      boxH > 0.0 &&
                      count == static_cast<std::size_t>(w) * static_cast<std::size_t>(h) &&
                      count <= kTipSdfMaxCells;
    if (!sane) { // no field: the shader keeps its analytic ellipse, which is a round tip's own truth
        if (m_sdfKey == 0)
            return;
        m_sdfKey = 0;
        m_sdfW = 0;
        m_sdfH = 0;
        m_pendingSdf.clear();
        m_sdfDirty = true;
        return;
    }
    m_sdfKey = key;
    m_sdfW = static_cast<std::uint32_t>(w);
    m_sdfH = static_cast<std::uint32_t>(h);
    m_sdfPad = static_cast<std::uint32_t>(pad);
    m_sdfBoxW = boxW;
    m_sdfBoxH = boxH;
    m_pendingSdf.assign(data, data + count);
    m_sdfDirty = true; // the payload lands in the mapped buffer in drawFrame, after the fence wait
}

void WindowRenderer::setTextOverlay(bool caretActive, common::Vec2 caretA, common::Vec2 caretB,
                                    const std::vector<std::array<common::Vec2, 4>>& selQuads,
                                    int handleCount) {
    m_textCaretActive = caretActive;
    m_textHandleCount = static_cast<unsigned>(std::max(0, handleCount));
    m_textCaretA = caretA;
    m_textCaretB = caretB;
    // Clamp to the SSBO capacity here so the per-frame upload stays a trivial copy (drawFrame writes
    // the latest into the mapped buffer after the fence wait, like the lasso path).
    if (selQuads.size() <= kTextSelMaxRects)
        m_textSelQuads = selQuads;
    else
        m_textSelQuads.assign(selQuads.begin(), selQuads.begin() + kTextSelMaxRects);
}

void WindowRenderer::setSpellSquiggles(const std::vector<std::array<common::Vec2, 2>>& segments) {
    // Clamp to capacity here so the per-frame upload stays a trivial copy (drawFrame writes them into
    // the mapped overlay buffer after the fence wait, right after the selection quads).
    if (segments.size() <= kSpellSquiggleMaxSegs)
        m_spellSquiggles = segments;
    else
        m_spellSquiggles.assign(segments.begin(), segments.begin() + kSpellSquiggleMaxSegs);
}

void WindowRenderer::setKeepChips(const std::vector<KeepChip>& chips) {
    // Same clamp-at-set discipline as the overlays above; the per-frame upload is a trivial copy.
    if (chips.size() <= kKeepChipMaxRects)
        m_keepChips = chips;
    else
        m_keepChips.assign(chips.begin(), chips.begin() + kKeepChipMaxRects);
}

void WindowRenderer::setGuideLines(const std::vector<GuideLine>& lines) {
    // Same clamp-at-set discipline as the keep chips; the per-frame upload is a trivial copy.
    if (lines.size() <= kGuideLineMax)
        m_guideLines = lines;
    else
        m_guideLines.assign(lines.begin(), lines.begin() + kGuideLineMax);
}

void WindowRenderer::setPenChrome(const std::vector<PenMark>& marks,
                                  const std::vector<PenStem>& stems, common::Vec2 ringCenter,
                                  double ringRadius) {
    // Same clamp-at-set discipline as the guides. The builder (ui::penChromeMarks) orders its
    // output so that a clamp here can only ever cost the LAST-drawn handles, never the selected
    // node's chrome and never an anchor.
    if (marks.size() <= kPenMarkMax)
        m_penMarks = marks;
    else
        m_penMarks.assign(marks.begin(), marks.begin() + kPenMarkMax);
    if (stems.size() <= kPenStemMax)
        m_penStems = stems;
    else
        m_penStems.assign(stems.begin(), stems.begin() + kPenStemMax);
    m_penRingCenter = ringCenter;
    m_penRingRadius = ringRadius;
}

void WindowRenderer::destroyCanvasTexture() noexcept {
    if (m_canvasView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_canvasView, nullptr); // view before its image
        m_canvasView = VK_NULL_HANDLE;
        m_descDirty = true;
    }
    if (m_canvasStaging != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, m_canvasStaging, m_canvasStagingAlloc);
        m_canvasStaging = VK_NULL_HANDLE;
        m_canvasStagingAlloc = nullptr;
        m_canvasStagingPtr = nullptr;
    }
    if (m_canvasImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_allocator, m_canvasImage, m_canvasAlloc);
        m_canvasImage = VK_NULL_HANDLE;
        m_canvasAlloc = nullptr;
    }
    m_canvasW = m_canvasH = 0;
    m_canvasLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_canvasValid = false;
}

bool WindowRenderer::ensureCanvasTexture(std::uint32_t w, std::uint32_t h) {
    if (m_canvasImage != VK_NULL_HANDLE && m_canvasW == w && m_canvasH == h)
        return true;
    // ⚠ ASK BEFORE ALLOCATING (S60-alpha's gate, which swept the compute lanes and missed the
    // presenting device's own textures -- finding G5). A document past maxImageDimension2D cannot
    // be ONE texture on this device, and learning that from vmaCreateImage costs a
    // vkDeviceWaitIdle and the destruction of the texture that was working a moment ago. Refuse
    // first and keep what we have; the caller falls back exactly as it did on an allocation
    // failure. `MOSAIC_GPU_PROFILE=floor` puts any device on the 4096 px guarantee, which is how
    // this branch gets exercised on hardware that has 16384.
    if (admitCanvasTexture(m_caps, w, h) == TextureRefusal::OverMaxImageDim) {
        if (m_refusedCanvasW != w || m_refusedCanvasH != h) {
            m_refusedCanvasW = w;
            m_refusedCanvasH = h;
            renderLog()->warn("canvas texture refused ({}): {}x{} against a {} px limit; the "
                              "document cannot be presented as one texture on this device",
                              textureRefusalName(TextureRefusal::OverMaxImageDim), w, h,
                              m_caps.maxImageDim);
        }
        return false;
    }
    // Only past the early-outs, so the row is real work: a device-idle wait, a teardown and two
    // fresh allocations, once per document open or resize. Lane::Gpu -- the wait is the device's.
    MOSAIC_PERF_SCOPE("Canvas texture (re)allocate", common::Lane::Gpu);
    vkDeviceWaitIdle(m_device); // the old texture may still be referenced by an in-flight frame
    destroyCanvasTexture();
    if (w == 0 || h == 0)
        return false;

    // Device-local R8G8B8A8 texture (matches common::Image byte order). The present compute pass
    // samples it (SAMPLED) through the view transform; colors map to the swapchain's BGRA in the
    // final 1:1 blit, which converts by component.
    const VkImageCreateInfo ici{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        // STORAGE so the GPU-resident drag pass (S60-a) can write the composite straight into it.
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_STORAGE_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo imgAlloc{.usage = VMA_MEMORY_USAGE_AUTO};
    if (vmaCreateImage(m_allocator, &ici, &imgAlloc, &m_canvasImage, &m_canvasAlloc, nullptr) !=
        VK_SUCCESS) {
        return false;
    }
    const VkImageViewCreateInfo vci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = m_canvasImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    if (vkCreateImageView(m_device, &vci, nullptr, &m_canvasView) != VK_SUCCESS) {
        destroyCanvasTexture();
        return false;
    }

    const VkBufferCreateInfo bci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = static_cast<VkDeviceSize>(w) * h * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo bufAlloc{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(m_allocator, &bci, &bufAlloc, &m_canvasStaging, &m_canvasStagingAlloc,
                        &info) != VK_SUCCESS) {
        destroyCanvasTexture();
        return false;
    }
    m_canvasStagingPtr = info.pMappedData;
    m_canvasW = w;
    m_canvasH = h;
    m_canvasLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_descDirty = true; // the present descriptor must re-point at the new document view
    // This renderer's own footprint on the heap just changed by 2 x w*h*4 bytes, and the next
    // thing that asks about device memory is a Move gesture on this document. Re-snapshot here,
    // where it is once per document, rather than per gesture or (worse) per frame.
    refreshMemoryBudget();
    return true;
}

void WindowRenderer::refreshMemoryBudget() {
    // `queryGpuMemory` needs a VulkanContext and the borrowing one over this device is exactly
    // that; it is the same object the resident compositor takes, created lazily and released in
    // the destructor before the device it borrows. A snapshot taken here ages between documents,
    // and it says so itself: `measured` is what separates "we asked the device" from "we assumed
    // the heap was ours", and atlasBudgetBytes already spends an assumed figure more cautiously.
    if (const std::shared_ptr<VulkanContext> ctx = computeContext())
        m_memory = queryGpuMemory(*ctx);
}

void WindowRenderer::ensureGpuTimer() {
    // Built LAZILY, and only under a profiler: a query pool nobody reads is device memory, and a
    // vkCmdResetQueryPool every frame for nothing. `--profile` / MOSAIC_PROFILE=1 is set before
    // the first frame and a debug build defaults collection on, so the timer exists exactly when a
    // row would be read; a toggle later in the session is picked up on the next frame because the
    // attempt is only marked once it is actually made.
    if (m_timer || m_timerTried || !common::Profiler::enabled())
        return;
    m_timerTried = true;
    const std::shared_ptr<VulkanContext> ctx = computeContext();
    if (!ctx)
        return;
    std::string error;
    m_timer = GpuTimer::create(*ctx, GpuTimer::kDefaultMaxScopes, error);
    // A device without timestamps is an ORDINARY outcome, not a failure: the present chain's
    // Lane::Gpu rows stay submit wall-clock, which is what they were before this existed.
    if (!m_timer)
        renderLog()->info("no device timing on the present queue: {}", error);
}

std::shared_ptr<VulkanContext> WindowRenderer::computeContext() {
    if (m_computeCtx) return m_computeCtx;
    if (m_device == VK_NULL_HANDLE) return nullptr;
    std::string error;
    m_computeCtx = VulkanContext::adopt(m_instance, m_physicalDevice, m_device, m_queue,
                                        m_queueFamily, m_caps, error);
    if (!m_computeCtx)
        renderLog()->warn("cannot host compute on the presenting device: {}", error);
    return m_computeCtx;
}

bool WindowRenderer::prepareResidentCanvas(std::uint32_t docW, std::uint32_t docH) {
    if (docW == 0 || docH == 0 || m_device == VK_NULL_HANDLE) return false;
    // Same allocation the upload path uses -- the texture is the same texture. What changes is who
    // writes it: the compositor's resolve pass, on the device, instead of a staging copy of a CPU
    // mirror. `m_canvasValid` deliberately stays false until noteResidentCanvasWritten(), so a
    // frame between allocation and the first resolve shows the background rather than garbage.
    if (!ensureCanvasTexture(docW, docH)) return false;
    // ⚠ The resolve writes a texture the PREVIOUS frame's present pass may still be sampling, and
    // it runs on its own submit rather than inside drawFrame's command buffer -- so nothing else
    // orders the two. This is the same fence drawFrame waits on before re-staging, waited one step
    // earlier; when the frame is already done (the normal edit-driven cadence) it costs nothing.
    // Removing it would be a write-after-read hazard that shows up as tearing under load, i.e. on
    // exactly the hardware this arc targets and never on the dev rig.
    {
        // The resident lane's own share of the frame fence: when the previous frame is already
        // retired (the normal edit-driven cadence) this row sits at ~0, and a row that is NOT ~0
        // says the resolve is being asked to write a texture the screen is still reading.
        MOSAIC_PERF_SCOPE("Resident canvas (fence wait)", common::Lane::Gpu);
        vkWaitForFences(m_device, 1, &m_inFlight, VK_TRUE, UINT64_MAX);
    }
    return true;
}

void WindowRenderer::noteResidentCanvasWritten(bool wroteImage) noexcept {
    if (m_canvasImage == VK_NULL_HANDLE) return;
    // ⚠ ONLY when the resolve actually submitted. A resolve with nothing to do returns success
    // without a submit and without a barrier, so claiming GENERAL here would leave this field
    // describing a layout the image is not in -- drawFrame would then emit a barrier out of the
    // wrong oldLayout, and the NEXT resolve would skip its transition and bind a storage
    // descriptor against SHADER_READ_ONLY_OPTIMAL. That is a validation error and, on a driver
    // that does not check, undefined contents.
    if (wroteImage)
        m_canvasLayout = VK_IMAGE_LAYOUT_GENERAL;
    m_canvasValid = true;
    // Any staged CPU upload is now stale by construction: two writers to one texture in one frame
    // is how a resident path silently reinstates the copy it exists to delete.
    m_hasPendingCanvas = false;
    m_pendingCanvas = common::Image{};
    m_hasPendingCanvasRegion = false;
    m_pendingCanvasRegion = common::Image{};
}

void WindowRenderer::destroyMaskTexture() noexcept {
    if (m_maskView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_maskView, nullptr); // view before its image
        m_maskView = VK_NULL_HANDLE;
        m_descDirty = true;
    }
    if (m_maskStaging != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, m_maskStaging, m_maskStagingAlloc);
        m_maskStaging = VK_NULL_HANDLE;
        m_maskStagingAlloc = nullptr;
        m_maskStagingPtr = nullptr;
    }
    if (m_maskImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_allocator, m_maskImage, m_maskAlloc);
        m_maskImage = VK_NULL_HANDLE;
        m_maskAlloc = nullptr;
    }
    m_maskW = m_maskH = 0;
    m_maskLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_maskValid = false;
}

// The selection-mask twin of ensureCanvasTexture: a sampled R8 texture + mapped staging buffer.
bool WindowRenderer::ensureMaskTexture(std::uint32_t w, std::uint32_t h) {
    if (m_maskImage != VK_NULL_HANDLE && m_maskW == w && m_maskH == h)
        return true;
    vkDeviceWaitIdle(m_device); // the old texture may still be referenced by an in-flight frame
    destroyMaskTexture();
    if (w == 0 || h == 0)
        return false;

    const VkImageCreateInfo ici{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo imgAlloc{.usage = VMA_MEMORY_USAGE_AUTO};
    if (vmaCreateImage(m_allocator, &ici, &imgAlloc, &m_maskImage, &m_maskAlloc, nullptr) !=
        VK_SUCCESS) {
        return false;
    }
    const VkImageViewCreateInfo vci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = m_maskImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    if (vkCreateImageView(m_device, &vci, nullptr, &m_maskView) != VK_SUCCESS) {
        destroyMaskTexture();
        return false;
    }

    const VkBufferCreateInfo bci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = static_cast<VkDeviceSize>(w) * h,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo bufAlloc{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(m_allocator, &bci, &bufAlloc, &m_maskStaging, &m_maskStagingAlloc, &info) !=
        VK_SUCCESS) {
        destroyMaskTexture();
        return false;
    }
    m_maskStagingPtr = info.pMappedData;
    m_maskW = w;
    m_maskH = h;
    m_maskLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_descDirty = true;
    return true;
}

void WindowRenderer::destroyOverlayTexture() noexcept {
    if (m_overlayView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_overlayView, nullptr); // view before its image
        m_overlayView = VK_NULL_HANDLE;
        m_descDirty = true;
    }
    if (m_overlayStaging != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, m_overlayStaging, m_overlayStagingAlloc);
        m_overlayStaging = VK_NULL_HANDLE;
        m_overlayStagingAlloc = nullptr;
        m_overlayStagingPtr = nullptr;
    }
    if (m_overlayImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_allocator, m_overlayImage, m_overlayAlloc);
        m_overlayImage = VK_NULL_HANDLE;
        m_overlayAlloc = nullptr;
    }
    m_overlayW = m_overlayH = 0;
    m_overlayLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_overlayValid = false;
}

// The overlay-text twin of ensureMaskTexture: a sampled RGBA8 texture + mapped staging buffer. The
// capacity is stable across a drag, so this no-ops (no vkDeviceWaitIdle stall) every frame but the
// rare ones where the content scale changes the tile size.
bool WindowRenderer::ensureOverlayTexture(std::uint32_t w, std::uint32_t h) {
    if (m_overlayImage != VK_NULL_HANDLE && m_overlayW == w && m_overlayH == h)
        return true;
    vkDeviceWaitIdle(m_device); // the old texture may still be referenced by an in-flight frame
    destroyOverlayTexture();
    if (w == 0 || h == 0)
        return false;

    const VkImageCreateInfo ici{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo imgAlloc{.usage = VMA_MEMORY_USAGE_AUTO};
    if (vmaCreateImage(m_allocator, &ici, &imgAlloc, &m_overlayImage, &m_overlayAlloc, nullptr) !=
        VK_SUCCESS) {
        return false;
    }
    const VkImageViewCreateInfo vci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = m_overlayImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    if (vkCreateImageView(m_device, &vci, nullptr, &m_overlayView) != VK_SUCCESS) {
        destroyOverlayTexture();
        return false;
    }

    const VkBufferCreateInfo bci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = static_cast<VkDeviceSize>(w) * h * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo bufAlloc{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(m_allocator, &bci, &bufAlloc, &m_overlayStaging, &m_overlayStagingAlloc,
                        &info) != VK_SUCCESS) {
        destroyOverlayTexture();
        return false;
    }
    m_overlayStagingPtr = info.pMappedData;
    m_overlayW = w;
    m_overlayH = h;
    m_overlayLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_descDirty = true;
    return true;
}

void WindowRenderer::setOverlayTile(const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h,
                                    std::uint32_t contentW, std::uint32_t contentH) {
    if (rgba == nullptr || w == 0 || h == 0) {
        // No tile: a 1x1 transparent placeholder keeps binding 3 valid.
        m_pendingOverlay.assign(4, 0);
        m_pendingOverlayW = m_pendingOverlayH = 1;
        m_overlayContentW = m_overlayContentH = 0;
    } else {
        m_pendingOverlay.assign(rgba, rgba + static_cast<std::size_t>(w) * h * 4);
        m_pendingOverlayW = w;
        m_pendingOverlayH = h;
        m_overlayContentW = contentW;
        m_overlayContentH = contentH;
    }
    m_hasPendingOverlay = true;
}

bool WindowRenderer::createPresentPipeline(std::string& error) {
    // binding 0: the rgba8 view image (storage, written); binding 1: the document (sampled);
    // binding 2: the selection coverage mask (sampled, R8 -- marching ants, S13).
    // NOTE: binding 12 (the FPS readout) exists ONLY in debug builds, and the count below is derived
    // from the array size (sizeof) so the descriptor layout can never drift out of lockstep with the
    // shader -- which declares binding 12 under the same MOSAIC_DEBUG guard (canvas_present.comp).
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        // binding 4: the in-flight lasso polyline (storage buffer, S-lasso).
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        // binding 5: the Type-tool caret + selection overlay (storage buffer, S29-b).
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        // binding 6: the Pen tool's node/handle chrome (storage buffer, S28). It reclaims one of the
        // two slots the retired motivational-one-liner backdrop left free.
        {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        // binding 8: the Smart Resize keep-region chips (storage buffer, S16-f). Binding 7 was the
        // rest of that retired backdrop (now a menu-bar ticker); the number is kept.
        {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        // binding 9: the brush reticle's tip-outline SDF (storage buffer, S19 §6.3). Its own binding
        // rather than a tail on 4: that struct ends in a flexible array member, and nothing can
        // follow one.
        {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        // binding 10: the eyedropper's loupe (storage buffer, S24). Its own binding like every other
        // overlay family; a fixed 48-byte struct.
        {10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        // binding 11: the DoF focus-band gizmo (storage buffer, S33). A fixed 112-byte struct,
        // the keep-chips' fixed-size sibling.
        {11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
#ifdef MOSAIC_DEBUG
        // binding 12: the canvas FPS readout (storage buffer, Help -> Show Canvas FPS). Its own
        // binding like every other overlay family; a fixed glyph-string struct. Debug builds only.
        {12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
#endif
        // binding 13: the rulers/guides + smart-guide lines (storage buffer). Present in every build
        // (unlike the debug-only FPS channel above), so it sits at 13 rather than reusing 12.
        {13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    const VkDescriptorSetLayoutCreateInfo slci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<std::uint32_t>(sizeof(bindings) / sizeof(bindings[0])),
        .pBindings = bindings,
    };
    if (vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_setLayout) != VK_SUCCESS) {
        error = "vkCreateDescriptorSetLayout failed";
        return false;
    }
    const VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PresentPush)};
    const VkPipelineLayoutCreateInfo plci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &m_setLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        error = "vkCreatePipelineLayout failed";
        return false;
    }
    const VkShaderModuleCreateInfo smci{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shaders::canvas_present_comp_size,
        .pCode = shaders::canvas_present_comp,
    };
    if (vkCreateShaderModule(m_device, &smci, nullptr, &m_presentShader) != VK_SUCCESS) {
        error = "vkCreateShaderModule (present) failed";
        return false;
    }
    const VkComputePipelineCreateInfo cpci{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = m_presentShader,
                  .pName = "main"},
        .layout = m_pipelineLayout,
    };
    if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_presentPipeline) !=
        VK_SUCCESS) {
        error = "vkCreateComputePipelines (present) failed";
        return false;
    }

    const VkDescriptorPoolSize poolSizes[3] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3}, // doc + mask + overlay
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         // 4 lasso + 5 text + 6 pen chrome + 8 chips + 9 tip SDF + 10 loupe + 11 DoF gizmo +
         // 13 guides (+ 12 FPS readout, debug builds only)
#ifdef MOSAIC_DEBUG
         9},
#else
         8},
#endif
    };
    const VkDescriptorPoolCreateInfo dpci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 3,
        .pPoolSizes = poolSizes,
    };
    if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_descPool) != VK_SUCCESS) {
        error = "vkCreateDescriptorPool failed";
        return false;
    }
    const VkDescriptorSetAllocateInfo dsai{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = m_descPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &m_setLayout,
    };
    if (vkAllocateDescriptorSets(m_device, &dsai, &m_descSet) != VK_SUCCESS) {
        error = "vkAllocateDescriptorSets failed";
        return false;
    }

    // The in-flight lasso polyline + brush reticle SSBO (binding 4): host-visible + persistently
    // mapped, fixed capacity. Written each frame after the fence wait (drawFrame). Starts inactive
    // (count = 0, reticleActive = 0).
    {
        // std430: {uint count; uint reticleActive; vec2 min; vec2 max; vec2 reticleCenter;
        // vec4 reticleShape; uint lineStyle; uint dotAlphaBits; uint featherStyle; uint channelView;
        // vec2 anchorPos; vec2 pts[]} -> a 72-byte header. The shape is a vec4 (the tip's two
        // semi-axes, its angle, the lock flag -- §6.3), which std430 aligns to 16: it sits at offset
        // 32, which is. The four trailing uints land at 48/52/56/60 -- channelView (S60-a item 10)
        // took the last of those, which used to be implicit padding -- and anchorPos (vec2, 8-byte
        // aligned; the Move-tool transform anchor) then sits at 64, pushing pts[] to 72. The header
        // is FULL: the next scalar lane added here has to grow the buffer and move pts[].
        const VkBufferCreateInfo bci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 72 + static_cast<VkDeviceSize>(kLassoMaxVerts) * 2 * sizeof(float),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VmaAllocationCreateInfo bufAlloc{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(m_allocator, &bci, &bufAlloc, &m_lassoBuffer, &m_lassoAlloc, &info) !=
            VK_SUCCESS) {
            error = "vmaCreateBuffer (lasso) failed";
            return false;
        }
        m_lassoPtr = info.pMappedData;
        std::memset(m_lassoPtr, 0, 48); // header zeroed (inactive) until the canvas pushes a path
        // The transform anchor (vec2 @64) starts at the "no anchor" sentinel so no glyph draws before
        // the first push; every recordFrame rewrites it (sentinel unless the Move box is up).
        auto* anchor0 = reinterpret_cast<float*>(static_cast<std::uint8_t*>(m_lassoPtr) + 64);
        anchor0[0] = -1.0e9f;
        anchor0[1] = -1.0e9f;
    }

    // The Type-tool caret + selection overlay SSBO (binding 5): host-visible + persistently mapped,
    // fixed capacity, written each frame (drawFrame). std430 header is 48 bytes (the vec4 quad array
    // aligns to 16); each selection quad is 2 vec4 = 32 bytes, and each spell squiggle segment (packed
    // after them) is 1 vec4 = 16 bytes. Starts inactive (caretActive = 0, selCount = 0).
    {
        const VkBufferCreateInfo bci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 64 + static_cast<VkDeviceSize>(kTextSelMaxRects) * 8 * sizeof(float) +
                    static_cast<VkDeviceSize>(kSpellSquiggleMaxSegs) * 4 * sizeof(float),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VmaAllocationCreateInfo bufAlloc{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(m_allocator, &bci, &bufAlloc, &m_textBuffer, &m_textAlloc, &info) !=
            VK_SUCCESS) {
            error = "vmaCreateBuffer (text overlay) failed";
            return false;
        }
        m_textPtr = info.pMappedData;
        std::memset(m_textPtr, 0, 8); // caretActive = 0, selCount = 0 (inactive)
    }

    // The Smart Resize keep-chip SSBO (binding 8, S16-f): host-visible + persistently mapped,
    // fixed capacity, written each frame. std430 header 32 bytes, then 3 vec4 per chip (two
    // corner-pair vec4s + one meta vec4). Starts inactive (count = 0).
    {
        const VkBufferCreateInfo bci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 32 + static_cast<VkDeviceSize>(kKeepChipMaxRects) * 12 * sizeof(float),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VmaAllocationCreateInfo bufAlloc{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(m_allocator, &bci, &bufAlloc, &m_chipBuffer, &m_chipAlloc, &info) !=
            VK_SUCCESS) {
            error = "vmaCreateBuffer (keep chips) failed";
            return false;
        }
        m_chipPtr = info.pMappedData;
        std::memset(m_chipPtr, 0, 8); // count = 0 (inactive) until the canvas pushes chips
    }

    // The brush reticle's tip-outline SDF (binding 9, S19 §6.3): host-visible + persistently mapped,
    // fixed capacity. std430 header {uint active; uint w; uint h; uint pad; vec2 box; vec2 pad2;} = 32
    // bytes, then the float grid. Starts inactive -- which is not a placeholder but the RULE for a
    // round tip: the shader's analytic ellipse is that tip's outline exactly, and every brush that
    // predates the preset library is one.
    {
        const VkBufferCreateInfo bci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 32 + static_cast<VkDeviceSize>(kTipSdfMaxCells) * sizeof(float),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VmaAllocationCreateInfo bufAlloc{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(m_allocator, &bci, &bufAlloc, &m_sdfBuffer, &m_sdfAlloc, &info) !=
            VK_SUCCESS) {
            error = "vmaCreateBuffer (tip sdf) failed";
            return false;
        }
        m_sdfPtr = info.pMappedData;
        std::memset(m_sdfPtr, 0, 32); // active = 0 (the analytic ellipse)
    }

    // The eyedropper's loupe SSBO (binding 10, S24): host-visible + persistently mapped, a fixed
    // 64-byte struct {uint active; float radius; float mag; float readout; vec2 center; vec2
    // sampleDoc; vec4 sampleRgba; vec4 prevRgba;}. std430 puts the vec4s at offsets 32/48
    // (16-aligned) -> 64 bytes. Starts inactive (active = 0).
    {
        const VkBufferCreateInfo bci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 64,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VmaAllocationCreateInfo bufAlloc{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(m_allocator, &bci, &bufAlloc, &m_loupeBuffer, &m_loupeAlloc, &info) !=
            VK_SUCCESS) {
            error = "vmaCreateBuffer (loupe) failed";
            return false;
        }
        m_loupePtr = info.pMappedData;
        std::memset(m_loupePtr, 0, 64); // active = 0 (the loupe hidden)
    }

    // The DoF focus-band gizmo SSBO (S33, binding 11): host-visible + persistently mapped, a fixed
    // struct {uint active; uint hot; float pad0; float pad1;} + 6 vec4 (five guide segments + the
    // knob pair) = 112 bytes, allocated at a comfortable 128. Starts inactive (active = 0).
    {
        const VkBufferCreateInfo bci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 128,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VmaAllocationCreateInfo bufAlloc{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(m_allocator, &bci, &bufAlloc, &m_dofBuffer, &m_dofAlloc, &info) !=
            VK_SUCCESS) {
            error = "vmaCreateBuffer (dof gizmo) failed";
            return false;
        }
        m_dofPtr = info.pMappedData;
        std::memset(m_dofPtr, 0, 128); // active = 0 (the gizmo hidden)
    }

    // The rulers/guides line overlay SSBO (binding 13): host-visible + persistently mapped, fixed
    // capacity, written each frame like the keep chips. std430 header 16 bytes (uint count + 3 pad),
    // then 2 vec4 per line (endpoint pair + colour). Starts inactive (count = 0).
    {
        const VkBufferCreateInfo bci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 16 + static_cast<VkDeviceSize>(kGuideLineMax) * 8 * sizeof(float),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VmaAllocationCreateInfo bufAlloc{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(m_allocator, &bci, &bufAlloc, &m_guideBuffer, &m_guideAlloc, &info) !=
            VK_SUCCESS) {
            error = "vmaCreateBuffer (guides) failed";
            return false;
        }
        m_guidePtr = info.pMappedData;
        std::memset(m_guidePtr, 0, 16); // count = 0 (inactive) until the canvas pushes guides
    }

    // The Pen tool's node/handle chrome SSBO (binding 6, S28): host-visible + persistently mapped,
    // fixed capacity, written each frame like the guides. std430 header {uint markCount; uint
    // stemCount; uint closeActive; uint pad0; vec2 min; vec2 max; vec2 closeCenter; float
    // closeRadius; float pad1;} -- the vec2s align to 8 (16/24/32), the two floats land at 40/44,
    // so the flexible vec4 array starts at a 16-aligned 48. Then one vec4 per mark, then one per
    // stem. Starts inactive (both counts 0).
    {
        const VkBufferCreateInfo bci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 48 + static_cast<VkDeviceSize>(kPenMarkMax + kPenStemMax) * 4 * sizeof(float),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VmaAllocationCreateInfo bufAlloc{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(m_allocator, &bci, &bufAlloc, &m_penBuffer, &m_penAlloc, &info) !=
            VK_SUCCESS) {
            error = "vmaCreateBuffer (pen chrome) failed";
            return false;
        }
        m_penPtr = info.pMappedData;
        std::memset(m_penPtr, 0, 48); // counts = 0 (inactive) until the canvas pushes chrome
    }

#ifdef MOSAIC_DEBUG
    // The canvas FPS readout SSBO (binding 12, debug builds only): host-visible + persistently
    // mapped, a fixed struct {uint active; uint count; uint px; uint pad; uint glyphs[16];} = 80
    // bytes, allocated at a comfortable 96. Starts inactive (active = 0).
    {
        const VkBufferCreateInfo bci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 96,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VmaAllocationCreateInfo bufAlloc{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(m_allocator, &bci, &bufAlloc, &m_fpsBuffer, &m_fpsAlloc, &info) !=
            VK_SUCCESS) {
            error = "vmaCreateBuffer (fps readout) failed";
            return false;
        }
        m_fpsPtr = info.pMappedData;
        std::memset(m_fpsPtr, 0, 96); // active = 0 (the readout hidden)
    }
#endif // MOSAIC_DEBUG

    const VkSamplerCreateInfo sci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    };
    if (vkCreateSampler(m_device, &sci, nullptr, &m_sampler) != VK_SUCCESS) {
        error = "vkCreateSampler failed";
        return false;
    }
    return true;
}

bool WindowRenderer::createDragPipeline(std::string& error) {
    // binding 0: the canvas texture as a STORAGE image (written); 1/2: the below + dragged textures
    // (sampled). Its own pipeline + push block, so the present pass (already at its push budget) is
    // untouched. S60-a.
    const VkDescriptorSetLayoutBinding bindings[3] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    const VkDescriptorSetLayoutCreateInfo slci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3,
        .pBindings = bindings,
    };
    if (vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_dragSetLayout) != VK_SUCCESS) {
        error = "vkCreateDescriptorSetLayout (drag) failed";
        return false;
    }
    const VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DragPush)};
    const VkPipelineLayoutCreateInfo plci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &m_dragSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_dragPipelineLayout) != VK_SUCCESS) {
        error = "vkCreatePipelineLayout (drag) failed";
        return false;
    }
    const VkShaderModuleCreateInfo smci{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shaders::canvas_drag_composite_comp_size,
        .pCode = shaders::canvas_drag_composite_comp,
    };
    if (vkCreateShaderModule(m_device, &smci, nullptr, &m_dragShader) != VK_SUCCESS) {
        error = "vkCreateShaderModule (drag) failed";
        return false;
    }
    const VkComputePipelineCreateInfo cpci{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = m_dragShader,
                  .pName = "main"},
        .layout = m_dragPipelineLayout,
    };
    if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_dragPipeline) !=
        VK_SUCCESS) {
        error = "vkCreateComputePipelines (drag) failed";
        return false;
    }
    const VkDescriptorPoolSize poolSizes[2] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
    };
    const VkDescriptorPoolCreateInfo dpci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 2,
        .pPoolSizes = poolSizes,
    };
    if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_dragDescPool) != VK_SUCCESS) {
        error = "vkCreateDescriptorPool (drag) failed";
        return false;
    }
    const VkDescriptorSetAllocateInfo dsai{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = m_dragDescPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &m_dragSetLayout,
    };
    if (vkAllocateDescriptorSets(m_device, &dsai, &m_dragDescSet) != VK_SUCCESS) {
        error = "vkAllocateDescriptorSets (drag) failed";
        return false;
    }
    return true;
}

bool WindowRenderer::uploadSampledTexture(const common::Image& img, VkImage& outImage,
                                          VmaAllocation_T*& outAlloc, VkImageView& outView,
                                          std::string& error) {
    // docs/s60-gesture-start-stall.md finding G5: this is a document-sized device image, a
    // document-sized host-visible staging buffer, a memcpy into it and a BLOCKING fence -- 160 MiB
    // and a synchronous queue round-trip each, twice per Move gesture at 5000x8000 -- and until
    // S60-a none of it was under a profiler scope, so a composite win could have been hiding it.
    // Three rows on purpose: the total, the host copy and the fence wait have different fixes
    // (a smaller backdrop; a persistently-mapped staging ring; an async submit).
    MOSAIC_PERF_SCOPE("Sampled texture upload", common::Lane::Gpu);
    const VkImageCreateInfo ici{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {img.width, img.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo ai{.usage = VMA_MEMORY_USAGE_AUTO};
    if (vmaCreateImage(m_allocator, &ici, &ai, &outImage, &outAlloc, nullptr) != VK_SUCCESS) {
        error = "vmaCreateImage (drag tex) failed";
        return false;
    }
    const VkImageViewCreateInfo vci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = outImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    if (vkCreateImageView(m_device, &vci, nullptr, &outView) != VK_SUCCESS) {
        error = "vkCreateImageView (drag tex) failed";
        return false;
    }

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(img.width) * img.height * 4;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = nullptr;
    VmaAllocationInfo si{};
    const VkBufferCreateInfo bci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo ba{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    if (vmaCreateBuffer(m_allocator, &bci, &ba, &staging, &stagingAlloc, &si) != VK_SUCCESS) {
        error = "vmaCreateBuffer (drag staging) failed";
        return false;
    }
    {
        // The host half: a straight `bytes`-sized memcpy into mapped staging (plus its flush,
        // which is a no-op on coherent memory). CPU lane -- no device work happens here.
        MOSAIC_PERF_SCOPE("Sampled texture upload (host copy)", common::Lane::Cpu);
        std::memcpy(si.pMappedData, img.rgba.data(), bytes);
        vmaFlushAllocation(m_allocator, stagingAlloc, 0, VK_WHOLE_SIZE);
    }

    // One-shot upload command, submitted synchronously (this runs once per gesture, between frames).
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    const VkCommandBufferAllocateInfo cbai{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = m_commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    vkAllocateCommandBuffers(m_device, &cbai, &cmd);
    const VkCommandBufferBeginInfo cbi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &cbi);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier toDst{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = outImage,
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &toDst);
    const VkBufferImageCopy region{
        .bufferOffset = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {img.width, img.height, 1},
    };
    vkCmdCopyBufferToImage(cmd, staging, outImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier toRead{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = outImage,
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toRead);
    vkEndCommandBuffer(cmd);

    VkFence fence = VK_NULL_HANDLE;
    const VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(m_device, &fci, nullptr, &fence);
    const VkSubmitInfo submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    vkQueueSubmit(m_queue, 1, &submit, fence);
    {
        // The device half: the UI thread parks here until the buffer->image copy retires. Separated
        // from the host copy above because the fix is different -- this one wants an asynchronous
        // submit, not a smaller copy.
        MOSAIC_PERF_SCOPE("Sampled texture upload (fence wait)", common::Lane::Gpu);
        vkWaitForFences(m_device, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    vkDestroyFence(m_device, fence, nullptr);
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vmaDestroyBuffer(m_allocator, staging, stagingAlloc);
    return true;
}

void WindowRenderer::writeDragDescriptors() noexcept {
    if (m_dragDescSet == VK_NULL_HANDLE || m_canvasView == VK_NULL_HANDLE ||
        m_belowView == VK_NULL_HANDLE || m_draggedView == VK_NULL_HANDLE)
        return;
    const VkDescriptorImageInfo outInfo{VK_NULL_HANDLE, m_canvasView, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo belowInfo{m_sampler, m_belowView,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo draggedInfo{m_sampler, m_draggedView,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet writes[3] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = m_dragDescSet,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = &outInfo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = m_dragDescSet,
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &belowInfo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = m_dragDescSet,
         .dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &draggedInfo},
    };
    vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);
}

bool WindowRenderer::createIdlePipeline(std::string& error) {
    // binding 0: the view image as a STORAGE image (written; READ too in blend-over mode);
    // binding 1: the invitation atlas (sampled via texelFetch). Its own pipeline + push block,
    // the drag pass's structural sibling -- the present pass's set and 128-byte push budget are
    // untouched.
    const VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    const VkDescriptorSetLayoutCreateInfo slci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings,
    };
    if (vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_idleSetLayout) != VK_SUCCESS) {
        error = "vkCreateDescriptorSetLayout (idle) failed";
        return false;
    }
    const VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(IdlePush)};
    const VkPipelineLayoutCreateInfo plci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &m_idleSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_idlePipelineLayout) != VK_SUCCESS) {
        error = "vkCreatePipelineLayout (idle) failed";
        return false;
    }
    const VkShaderModuleCreateInfo smci{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shaders::canvas_idle_comp_size,
        .pCode = shaders::canvas_idle_comp,
    };
    if (vkCreateShaderModule(m_device, &smci, nullptr, &m_idleShader) != VK_SUCCESS) {
        error = "vkCreateShaderModule (idle) failed";
        return false;
    }
    const VkComputePipelineCreateInfo cpci{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = m_idleShader,
                  .pName = "main"},
        .layout = m_idlePipelineLayout,
    };
    if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_idlePipeline) !=
        VK_SUCCESS) {
        error = "vkCreateComputePipelines (idle) failed";
        return false;
    }
    const VkDescriptorPoolSize poolSizes[2] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
    };
    const VkDescriptorPoolCreateInfo dpci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 2,
        .pPoolSizes = poolSizes,
    };
    if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_idleDescPool) != VK_SUCCESS) {
        error = "vkCreateDescriptorPool (idle) failed";
        return false;
    }
    const VkDescriptorSetAllocateInfo dsai{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = m_idleDescPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &m_idleSetLayout,
    };
    if (vkAllocateDescriptorSets(m_device, &dsai, &m_idleDescSet) != VK_SUCCESS) {
        error = "vkAllocateDescriptorSets (idle) failed";
        return false;
    }
    // A 1x1 transparent placeholder atlas keeps binding 1 valid before the first bake arrives
    // (the mask/overlay placeholder convention).
    common::Image placeholder(1, 1);
    if (!uploadSampledTexture(placeholder, m_idleAtlasImage, m_idleAtlasAlloc, m_idleAtlasView,
                              error))
        return false;
    m_idleAtlasW = 1;
    m_idleAtlasH = 1;
    m_idleAtlasRows = 1;
    m_idleAtlasReal = false;
    m_idleDescDirty = true;
    return true;
}

void WindowRenderer::writeIdleDescriptors() noexcept {
    if (m_idleDescSet == VK_NULL_HANDLE || m_viewImageView == VK_NULL_HANDLE ||
        m_idleAtlasView == VK_NULL_HANDLE)
        return;
    const VkDescriptorImageInfo outInfo{VK_NULL_HANDLE, m_viewImageView, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo atlasInfo{m_sampler, m_idleAtlasView,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet writes[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = m_idleDescSet,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = &outInfo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = m_idleDescSet,
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &atlasInfo},
    };
    vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
    m_idleDescDirty = false;
}

void WindowRenderer::setIdleAtlas(const common::Image& atlas, std::uint32_t rows) {
    // Retire the current atlas through the frame fence (the endGpuDrag pattern -- never a
    // device-idle wait on the UI thread), then upload the new one synchronously. Called only on
    // theme / DPI / copy changes, between frames.
    if (m_idleAtlasImage != VK_NULL_HANDLE) {
        m_deadDragTextures.push_back({m_idleAtlasImage, m_idleAtlasAlloc, m_idleAtlasView});
        m_idleAtlasImage = VK_NULL_HANDLE;
        m_idleAtlasAlloc = nullptr;
        m_idleAtlasView = VK_NULL_HANDLE;
    }
    static const auto log = common::log::category("render");
    const common::Image* src = &atlas;
    common::Image placeholder;
    if (atlas.empty() || rows == 0) {
        placeholder = common::Image(1, 1); // dropping the atlas: back to the transparent stand-in
        src = &placeholder;
        rows = 1;
    }
    std::string err;
    if (!uploadSampledTexture(*src, m_idleAtlasImage, m_idleAtlasAlloc, m_idleAtlasView, err)) {
        log->warn("idle atlas upload failed ({}); the field renders without an invitation", err);
        m_idleAtlasReal = false;
        m_idleAtlasW = m_idleAtlasH = 0;
        return;
    }
    m_idleAtlasW = src->width;
    m_idleAtlasH = src->height;
    m_idleAtlasRows = rows;
    m_idleAtlasReal = src != &placeholder;
    m_idleDescDirty = true;
}

void WindowRenderer::destroyDragTextures() noexcept {
    if (m_belowView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_belowView, nullptr);
        m_belowView = VK_NULL_HANDLE;
    }
    if (m_belowImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_allocator, m_belowImage, m_belowAlloc);
        m_belowImage = VK_NULL_HANDLE;
        m_belowAlloc = nullptr;
    }
    if (m_draggedView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_draggedView, nullptr);
        m_draggedView = VK_NULL_HANDLE;
    }
    if (m_draggedImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_allocator, m_draggedImage, m_draggedAlloc);
        m_draggedImage = VK_NULL_HANDLE;
        m_draggedAlloc = nullptr;
    }
}

TextureRefusal WindowRenderer::dragAdmission(std::uint32_t belowW, std::uint32_t belowH,
                                             std::uint32_t dragW,
                                             std::uint32_t dragH) const noexcept {
    if (m_device == VK_NULL_HANDLE)
        return TextureRefusal::NoDevice;
    return admitDragTextures(m_caps, m_memory, belowW, belowH, dragW, dragH);
}

bool WindowRenderer::canHostDragTextures(std::uint32_t belowW, std::uint32_t belowH,
                                         std::uint32_t dragW, std::uint32_t dragH) const noexcept {
    return dragAdmission(belowW, belowH, dragW, dragH) == TextureRefusal::Admitted;
}

void WindowRenderer::beginGpuDrag(const common::Image& below, const common::Image& dragged) {
    endGpuDrag(); // drop any prior gesture's textures
    if (below.empty() || dragged.empty() || m_canvasImage == VK_NULL_HANDLE)
        return; // no canvas yet / nothing to composite -> caller stays on the CPU path
    static const auto log = common::log::category("render");
    if (const TextureRefusal why =
            dragAdmission(below.width, below.height, dragged.width, dragged.height);
        why != TextureRefusal::Admitted) {
        // The caller should have asked first (the app does, before the below-composite); refusing
        // here as well keeps a forgetful one from failing obscurely inside vmaCreateImage. It says
        // WHICH refusal, because "this device's images stop at N px" and "there is no headroom
        // right now" are different reports with different answers.
        log->warn("GPU drag refused ({}): {}x{} below + {}x{} dragged wants {} MiB, device max {} "
                  "px, {}; CPU path serves",
                  textureRefusalName(why), below.width, below.height, dragged.width, dragged.height,
                  dragTextureCost(below.width, below.height, dragged.width, dragged.height)
                          .peakBytes() /
                      (1024ull * 1024ull),
                  m_caps.maxImageDim, m_memory.summary());
        return;
    }
    // The other half of the gesture-START seam (docs/s60-gesture-start-stall.md §6.1): both drag
    // textures, both staging copies and both fences under ONE row, next to "Drag below-composite".
    // Without it a composite win could hide a few hundred ms of upload nobody was looking at.
    MOSAIC_PERF_SCOPE("Drag texture upload", common::Lane::Gpu);
    std::string err;
    if (!uploadSampledTexture(below, m_belowImage, m_belowAlloc, m_belowView, err) ||
        !uploadSampledTexture(dragged, m_draggedImage, m_draggedAlloc, m_draggedView, err)) {
        log->warn("GPU drag setup failed ({}); falling back to CPU", err);
        destroyDragTextures();
        return;
    }
    m_draggedW = dragged.width;
    m_draggedH = dragged.height;
    writeDragDescriptors();
    m_dragActive = true;
}

void WindowRenderer::setGpuDragTransform(const common::Affine2D& m, int blendMode, float opacity) {
    m_dragInv[0] = static_cast<float>(m.m00);
    m_dragInv[1] = static_cast<float>(m.m01);
    m_dragInv[2] = static_cast<float>(m.m10);
    m_dragInv[3] = static_cast<float>(m.m11);
    m_dragInv[4] = static_cast<float>(m.m02);
    m_dragInv[5] = static_cast<float>(m.m12);
    m_dragMode = blendMode;
    m_dragOpacity = opacity;
}

void WindowRenderer::endGpuDrag() {
    m_dragActive = false;
    // Retire the textures WITHOUT a device-idle wait (that wait was the inconsistent multi-second
    // freeze on release). They are destroyed in drawFrame after the frame fence, which guarantees
    // the GPU is done referencing them.
    if (m_belowImage != VK_NULL_HANDLE)
        m_deadDragTextures.push_back({m_belowImage, m_belowAlloc, m_belowView});
    if (m_draggedImage != VK_NULL_HANDLE)
        m_deadDragTextures.push_back({m_draggedImage, m_draggedAlloc, m_draggedView});
    m_belowImage = VK_NULL_HANDLE;
    m_belowAlloc = nullptr;
    m_belowView = VK_NULL_HANDLE;
    m_draggedImage = VK_NULL_HANDLE;
    m_draggedAlloc = nullptr;
    m_draggedView = VK_NULL_HANDLE;
}

bool WindowRenderer::ensureViewImage(std::string& error) {
    if (m_viewImage != VK_NULL_HANDLE && m_viewExtent.width == m_extent.width &&
        m_viewExtent.height == m_extent.height) {
        return true;
    }
    destroyViewImage();
    if (m_extent.width == 0 || m_extent.height == 0)
        return true;

    const VkImageCreateInfo ici{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {m_extent.width, m_extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO};
    if (vmaCreateImage(m_allocator, &ici, &alloc, &m_viewImage, &m_viewAlloc, nullptr) !=
        VK_SUCCESS) {
        error = "vmaCreateImage (view) failed";
        return false;
    }
    const VkImageViewCreateInfo vci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = m_viewImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    if (vkCreateImageView(m_device, &vci, nullptr, &m_viewImageView) != VK_SUCCESS) {
        error = "vkCreateImageView (view) failed";
        return false;
    }
    m_viewExtent = m_extent;
    m_descDirty = true;
    m_idleDescDirty = true; // the idle set points at the view image too
    return true;
}

void WindowRenderer::destroyViewImage() noexcept {
    if (m_viewImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_viewImageView, nullptr);
        m_viewImageView = VK_NULL_HANDLE;
    }
    if (m_viewImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_allocator, m_viewImage, m_viewAlloc);
        m_viewImage = VK_NULL_HANDLE;
        m_viewAlloc = nullptr;
    }
    m_viewExtent = {0, 0};
}

void WindowRenderer::writeDescriptors() noexcept {
    if (m_viewImageView == VK_NULL_HANDLE || m_canvasView == VK_NULL_HANDLE ||
        m_maskView == VK_NULL_HANDLE || m_overlayView == VK_NULL_HANDLE) {
        return; // nothing valid to point at yet; stays dirty
    }
    const VkDescriptorImageInfo outInfo{VK_NULL_HANDLE, m_viewImageView, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo docInfo{m_sampler, m_canvasView,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo maskInfo{m_sampler, m_maskView,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo overlayInfo{m_sampler, m_overlayView,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo lassoInfo{m_lassoBuffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo textInfo{m_textBuffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo penInfo{m_penBuffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo chipInfo{m_chipBuffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo sdfInfo{m_sdfBuffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo loupeInfo{m_loupeBuffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo dofInfo{m_dofBuffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo guideInfo{m_guideBuffer, 0, VK_WHOLE_SIZE};
#ifdef MOSAIC_DEBUG
    const VkDescriptorBufferInfo fpsInfo{m_fpsBuffer, 0, VK_WHOLE_SIZE};
#endif
    // The binding-12 (FPS) write is present only in debug builds; the count below is derived from the
    // array size so it always matches the descriptor layout for the same build config.
    const VkWriteDescriptorSet writes[] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = m_descSet,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = &outInfo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = m_descSet,
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &docInfo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = m_descSet,
         .dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &maskInfo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = m_descSet,
         .dstBinding = 3,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &overlayInfo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = m_descSet,
         .dstBinding = 4,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &lassoInfo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = m_descSet,
         .dstBinding = 5,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &textInfo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = m_descSet,
         .dstBinding = 6,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &penInfo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = m_descSet,
         .dstBinding = 8,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &chipInfo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = m_descSet,
         .dstBinding = 9,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &sdfInfo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = m_descSet,
         .dstBinding = 10,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &loupeInfo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = m_descSet,
         .dstBinding = 11,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &dofInfo},
#ifdef MOSAIC_DEBUG
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = m_descSet,
         .dstBinding = 12,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &fpsInfo},
#endif
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = m_descSet,
         .dstBinding = 13,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &guideInfo},
    };
    vkUpdateDescriptorSets(m_device, static_cast<std::uint32_t>(sizeof(writes) / sizeof(writes[0])),
                           writes, 0, nullptr);
    m_descDirty = false;
}

void WindowRenderer::setView(const common::Affine2D& docToScreenLogical,
                             double contentScale) noexcept {
    m_docToScreen = docToScreenLogical;
    m_contentScale = contentScale > 0.0 ? contentScale : 1.0;
    m_hasView = true;
}

void WindowRenderer::setRotationOverlay(bool active, double angleRadians) noexcept {
    m_overlayActive = active;
    m_overlayAngle = angleRadians;
}

common::Affine2D WindowRenderer::presentInverse() const {
    using common::Affine2D;
    Affine2D docToScreenPhysical;
    if (m_hasView) {
        // The UI works in logical px; scale up to the swapchain's physical px.
        docToScreenPhysical = Affine2D::scaling(m_contentScale, m_contentScale) * m_docToScreen;
    } else {
        // Before the UI sets a view: centered aspect-fit, matching the S7-c presentation.
        const BlitRect r = fitCentered(m_canvasW, m_canvasH, m_extent.width, m_extent.height);
        const double s = (m_canvasW > 0) ? static_cast<double>(r.w) / m_canvasW : 1.0;
        docToScreenPhysical = Affine2D::translation(r.x, r.y) * Affine2D::scaling(s, s);
    }
    if (const auto inv = docToScreenPhysical.inverse())
        return *inv;
    return Affine2D::identity();
}

bool WindowRenderer::drawFrame(common::Color8 clearColor, std::string& error) {
    if (m_needsRecreate) {
        if (!recreate(error))
            return false;
    }
    if (m_swapchain == VK_NULL_HANDLE || m_extent.width == 0 || m_extent.height == 0) {
        return true; // minimized: nothing to present
    }
    // Past the minimized early-out on purpose: a scope on a frame that does nothing would bury the
    // frames that cost something under a column of zeroes (profiler.hpp's `updateReflectionEnv`
    // lesson). This row is what the whole present chain COSTS the UI thread; its Lane::GpuDevice
    // twin below (same name, written by the timer) is what the device SPENT, and the gap between
    // the two is the diagnosis.
    MOSAIC_PERF_SCOPE("Present frame", common::Lane::Gpu);
    ensureGpuTimer();

    {
        // The previous frame's transfers read the staging buffers, so wait for it before
        // re-staging. Split out because this is where the UI thread parks when the device is the
        // bottleneck, and the fix for a big number here is pacing, not a faster shader.
        MOSAIC_PERF_SCOPE("Present frame (fence wait)", common::Lane::Gpu);
        vkWaitForFences(m_device, 1, &m_inFlight, VK_TRUE, UINT64_MAX);
    }
    // Only now are the PREVIOUS submission's timestamps readable. resolveAndRecord never blocks,
    // so calling it before this fence would silently drop the samples rather than stall; calling
    // it here, one frame late, is what makes the device rows free.
    if (m_timer)
        (void)m_timer->resolveAndRecord();

    // Destroy drag textures retired since last frame (the fence above guarantees the GPU is done
    // with them) -- so endGpuDrag never blocks the UI thread (S60-a).
    if (!m_deadDragTextures.empty()) {
        for (const DeadTexture& t : m_deadDragTextures) {
            if (t.view != VK_NULL_HANDLE)
                vkDestroyImageView(m_device, t.view, nullptr);
            if (t.image != VK_NULL_HANDLE)
                vmaDestroyImage(m_allocator, t.image, t.alloc);
        }
        m_deadDragTextures.clear();
    }

    // Prepare a pending document image for upload (outside the command buffer): (re)allocate the
    // texture if its size changed and stage the pixels. The actual copy is recorded below.
    bool uploadCanvas = false;
    if (m_hasPendingCanvas) {
        if (!m_pendingCanvas.empty() &&
            ensureCanvasTexture(m_pendingCanvas.width, m_pendingCanvas.height)) {
            // A whole-document memcpy into mapped staging -- 160 MiB at 5000x8000, on the UI
            // thread, every time a composite lands. The CPU lane is the right one: no device work
            // happens here, and the device's half of the same upload is the `Canvas upload` device
            // row the timer writes around the buffer->image copy below.
            MOSAIC_PERF_SCOPE("Canvas upload (stage)", common::Lane::Cpu);
            std::memcpy(m_canvasStagingPtr, m_pendingCanvas.rgba.data(),
                        m_pendingCanvas.rgba.size());
            vmaFlushAllocation(m_allocator, m_canvasStagingAlloc, 0, VK_WHOLE_SIZE);
            uploadCanvas = true;
        } else if (m_pendingCanvas.empty()) {
            destroyCanvasTexture(); // explicit "show nothing"
        }
        m_hasPendingCanvas = false;
        m_pendingCanvas = common::Image{}; // free the CPU-side copy
    }

    // Dirty-region upload (S60-a): patch only a sub-rectangle of the existing canvas texture
    // (brush stroke / inpaint preview). Stage the sub-image into the front of the (full-canvas)
    // staging buffer for a partial copy. A full-canvas upload this frame supersedes it; the region
    // is also dropped if the texture is missing/mismatched (caller falls back to a full composite).
    bool uploadCanvasRegion = false;
    std::uint32_t regionX = 0, regionY = 0, regionW = 0, regionH = 0;
    if (m_hasPendingCanvasRegion) {
        const common::Image& sub = m_pendingCanvasRegion;
        if (!uploadCanvas && !sub.empty() && m_canvasImage != VK_NULL_HANDLE &&
            m_pendingCanvasRegionX + sub.width <= m_canvasW &&
            m_pendingCanvasRegionY + sub.height <= m_canvasH) {
            // The dirty-rect path's host half. It exists to be small, and a row next to
            // `Canvas upload (stage)` is how "small" stops being an assumption -- a brush dab that
            // quietly widened its rect to the whole document shows up here and nowhere else.
            MOSAIC_PERF_SCOPE("Canvas upload (region stage)", common::Lane::Cpu);
            std::memcpy(m_canvasStagingPtr, sub.rgba.data(), sub.rgba.size());
            vmaFlushAllocation(m_allocator, m_canvasStagingAlloc, 0, VK_WHOLE_SIZE);
            uploadCanvasRegion = true;
            regionX = m_pendingCanvasRegionX;
            regionY = m_pendingCanvasRegionY;
            regionW = sub.width;
            regionH = sub.height;
        }
        m_hasPendingCanvasRegion = false;
        m_pendingCanvasRegion = common::Image{};
    }

    // Same for the selection mask (S13). Until the UI pushes one, stage the 1x1 zero placeholder
    // so the present pass's mask binding is always valid.
    if (m_maskImage == VK_NULL_HANDLE && !m_hasPendingMask) {
        m_pendingMask.assign(1, 0);
        m_pendingMaskW = 1;
        m_pendingMaskH = 1;
        m_hasPendingMask = true;
    }
    bool uploadMask = false;
    if (m_hasPendingMask) {
        if (ensureMaskTexture(m_pendingMaskW, m_pendingMaskH)) {
            std::memcpy(m_maskStagingPtr, m_pendingMask.data(), m_pendingMask.size());
            vmaFlushAllocation(m_allocator, m_maskStagingAlloc, 0, VK_WHOLE_SIZE);
            uploadMask = true;
        }
        m_hasPendingMask = false;
        m_pendingMask = {}; // free the CPU-side copy
    }

    // Same for the overlay-text tile (S16 rework): a 1x1 transparent placeholder until the canvas
    // pushes a real tile, so the present pass's binding 3 is always valid.
    if (m_overlayImage == VK_NULL_HANDLE && !m_hasPendingOverlay) {
        m_pendingOverlay.assign(4, 0);
        m_pendingOverlayW = 1;
        m_pendingOverlayH = 1;
        m_hasPendingOverlay = true;
    }
    bool uploadOverlay = false;
    if (m_hasPendingOverlay) {
        if (ensureOverlayTexture(m_pendingOverlayW, m_pendingOverlayH)) {
            std::memcpy(m_overlayStagingPtr, m_pendingOverlay.data(), m_pendingOverlay.size());
            vmaFlushAllocation(m_allocator, m_overlayStagingAlloc, 0, VK_WHOLE_SIZE);
            uploadOverlay = true;
        }
        m_hasPendingOverlay = false;
        m_pendingOverlay = {}; // free the CPU-side copy
    }

    // Who owns the controls quad lanes this frame -- computed ONCE so the dot alpha (written into
    // the lasso header just below), the corner pick and the mode float (pushed further down) can
    // never disagree: framing preview > sample area > crop > line gizmo > Move/Shape handles > the
    // Type box's dots. (The framing preview leads because it is the Zoom tool's, and the Zoom tool
    // is never up alongside the tools below it -- so it never actually displaces one.)
    const bool ctlTextDots = m_textDotsActive && !m_framingActive && !m_sampleAreaActive &&
                             !m_cropActive && !m_handlesActive && !m_lineGizmoActive;
    const float ctlDotAlpha =
        m_framingActive || m_sampleAreaActive || m_cropActive || m_lineGizmoActive ? 0.0f
        : m_handlesActive ? m_rotateDotAlpha
        : ctlTextDots     ? m_textDotAlpha
                          : 0.0f;

    // Upload the in-flight lasso polyline + brush reticle into their shared SSBO (std430
    // {uint count; uint reticleActive; vec2 min; vec2 max; vec2 reticleCenter; vec4 reticleShape;
    // uint lineStyle; uint dotAlphaBits; uint featherStyle; vec2 anchorPos; vec2 pts[]}), converting
    // the canvas's logical px -> physical px (× content scale, like the handle corners). Single-frame-
    // in-flight: the fence wait above guarantees the GPU finished last frame's read.
    {
        const std::uint32_t count = static_cast<std::uint32_t>(m_lassoVerts.size()); // already clamped
        auto* base = static_cast<std::uint8_t*>(m_lassoPtr);
        auto* hdr = reinterpret_cast<std::uint32_t*>(base);
        hdr[0] = count;
        hdr[1] = m_reticleActive ? 1u : 0u;
        hdr[12] = static_cast<std::uint32_t>(m_overlayLineStyle); // byte 48: the overlay line style
        // Byte 52 (the old pad): the rotate-hotspot dots' alpha as float bits, for whichever box
        // owns the controls lanes this frame (ctlDotAlpha -- ONE priority rule, shared with the
        // corner/mode pick below) -- the push block is at its 128-byte budget, and this header
        // uploads every frame anyway. 0 for the lanes that have no rotate affordance.
        {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &ctlDotAlpha, sizeof bits);
            hdr[13] = bits;
        }
        hdr[14] = static_cast<std::uint32_t>(m_featherIndicator); // byte 56: feathered-sel indicator
        // Byte 60: the Channels tab's on-canvas channel view (S60-a item 10). This was std430's
        // LAST implicit pad in the header -- anchorPos below is 8-aligned and so starts at 64 -- so
        // the lane costs no bytes and moves nothing. 0 (the member's default) is the normal
        // composite AND the shader's identity, so a frame nobody set a view for is unchanged.
        hdr[15] = m_channelView;
        // Byte 64 (a vec2, 8-aligned, which pushes verts to 72): the Move-tool transform ANCHOR /
        // reference point in physical px, or the sentinel (x < -1e8) when no anchor shows this frame.
        // Only drawn with the transform box (m_handlesActive); the present pass draws the crosshair.
        auto* anchor = reinterpret_cast<float*>(base + 64);
        if (m_anchorActive && m_handlesActive) {
            anchor[0] = static_cast<float>(m_anchorPos.x * m_contentScale);
            anchor[1] = static_cast<float>(m_anchorPos.y * m_contentScale);
        } else {
            anchor[0] = -1.0e9f; // sentinel: no anchor glyph
            anchor[1] = -1.0e9f;
        }
        auto* bbox = reinterpret_cast<float*>(base + 8);     // min.xy, max.xy
        auto* reticle = reinterpret_cast<float*>(base + 24); // center.xy, then shape.xyzw at byte 32
        auto* verts = reinterpret_cast<float*>(base + 72);   // pts[] re-aligned past anchorPos @64
        reticle[0] = static_cast<float>(m_reticleCenter.x * m_contentScale);
        reticle[1] = static_cast<float>(m_reticleCenter.y * m_contentScale);
        // The tip's shape: its two semi-axes scale with the content like every other length here; its
        // ANGLE does not -- a rotation is not a distance.
        reticle[2] = static_cast<float>(m_reticleSemiX * m_contentScale);
        reticle[3] = static_cast<float>(m_reticleSemiY * m_contentScale);
        reticle[4] = static_cast<float>(m_reticleAngle);
        reticle[5] = m_reticleLocked ? 1.0f : 0.0f;
        float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
        for (std::uint32_t i = 0; i < count; ++i) {
            // A CONTOUR BREAK marker (x far out of range -- ui::kPolylineBreak) is passed through
            // unscaled and kept OUT of the bbox: scaling it would move it toward zero, and letting
            // it into the bbox would blow the shader's per-pixel cull open across the whole screen.
            if (m_lassoVerts[i].x <= kPolylineBreakX) {
                verts[2 * i + 0] = static_cast<float>(kPolylineBreakValue);
                verts[2 * i + 1] = static_cast<float>(kPolylineBreakValue);
                continue;
            }
            const auto x = static_cast<float>(m_lassoVerts[i].x * m_contentScale);
            const auto y = static_cast<float>(m_lassoVerts[i].y * m_contentScale);
            verts[2 * i + 0] = x;
            verts[2 * i + 1] = y;
            minx = std::min(minx, x);
            miny = std::min(miny, y);
            maxx = std::max(maxx, x);
            maxy = std::max(maxy, y);
        }
        bbox[0] = minx;
        bbox[1] = miny;
        bbox[2] = maxx;
        bbox[3] = maxy;
        vmaFlushAllocation(m_allocator, m_lassoAlloc, 0, VK_WHOLE_SIZE);
    }

    // Upload the Type-tool caret + selection overlay into its SSBO (binding 5), logical px -> physical
    // px (× content scale). std430 header: {uint caretActive; uint selCount; vec2 caretA; vec2 caretB;
    // vec2 selMin; vec2 selMax; vec2 pad;} = 48 bytes, then vec4 quads[]. Same single-frame-in-flight
    // discipline as the lasso buffer (the fence wait guarantees last frame's read finished).
    {
        const auto sc = static_cast<float>(m_contentScale);
        const std::uint32_t selCount =
            std::min(static_cast<std::uint32_t>(m_textSelQuads.size()), kTextSelMaxRects);
        auto* base = static_cast<std::uint8_t*>(m_textPtr);
        auto* u = reinterpret_cast<std::uint32_t*>(base);
        auto* f = reinterpret_cast<float*>(base);
        u[0] = m_textCaretActive ? 1u : 0u;
        u[1] = selCount;
        f[2] = static_cast<float>(m_textCaretA.x) * sc; // caretA @8
        f[3] = static_cast<float>(m_textCaretA.y) * sc;
        f[4] = static_cast<float>(m_textCaretB.x) * sc; // caretB @16
        f[5] = static_cast<float>(m_textCaretB.y) * sc;
        auto* quads = reinterpret_cast<float*>(base + 64);
        float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
        for (std::uint32_t i = 0; i < selCount; ++i) {
            for (int c = 0; c < 4; ++c) {
                const float x = static_cast<float>(m_textSelQuads[i][c].x) * sc;
                const float y = static_cast<float>(m_textSelQuads[i][c].y) * sc;
                quads[8 * i + 2 * c + 0] = x;
                quads[8 * i + 2 * c + 1] = y;
                minx = std::min(minx, x);
                miny = std::min(miny, y);
                maxx = std::max(maxx, x);
                maxy = std::max(maxy, y);
            }
        }
        // Spell squiggle segments ride the same array, right after the selection quads: each is one
        // vec4 (A.xy, B.xy) at float offset 8*selCount + 4*j. The bbox expands over them too, so the
        // shader's single cull covers squiggles even when nothing is selected (selCount == 0).
        const std::uint32_t sqCount =
            std::min(static_cast<std::uint32_t>(m_spellSquiggles.size()), kSpellSquiggleMaxSegs);
        float* segs = quads + static_cast<std::size_t>(8) * selCount;
        for (std::uint32_t j = 0; j < sqCount; ++j) {
            const float ax = static_cast<float>(m_spellSquiggles[j][0].x) * sc;
            const float ay = static_cast<float>(m_spellSquiggles[j][0].y) * sc;
            const float bx = static_cast<float>(m_spellSquiggles[j][1].x) * sc;
            const float by = static_cast<float>(m_spellSquiggles[j][1].y) * sc;
            segs[4 * j + 0] = ax;
            segs[4 * j + 1] = ay;
            segs[4 * j + 2] = bx;
            segs[4 * j + 3] = by;
            minx = std::min({minx, ax, bx});
            miny = std::min({miny, ay, by});
            maxx = std::max({maxx, ax, bx});
            maxy = std::max({maxy, ay, by});
        }
        f[6] = minx; // selMin @24 (bounds selection quads + squiggles)
        f[7] = miny;
        f[8] = maxx; // selMax @32
        f[9] = maxy;
        f[10] = static_cast<float>(m_textHandleCount); // tHandleActive @40 (# trailing solid handles)
        u[11] = sqCount;                          // tSquiggleCount @44 (was pad)
        // tBendHandle @48 = (pill.xy, apex.xy) screen px; pill.x = -1e9 when there is no bend handle.
        if (m_textBendActive) {
            f[12] = static_cast<float>(m_textBendPill.x) * sc;
            f[13] = static_cast<float>(m_textBendPill.y) * sc;
            f[14] = static_cast<float>(m_textBendApex.x) * sc;
            f[15] = static_cast<float>(m_textBendApex.y) * sc;
        } else {
            f[12] = -1.0e9f;
        }
        vmaFlushAllocation(m_allocator, m_textAlloc, 0, VK_WHOLE_SIZE);
    }

    // Upload the Smart Resize keep-region chips into their SSBO (binding 8), logical -> physical px.
    // std430 header: {uint count; uint pad0; vec2 min; vec2 max; vec2 pad1;} = 32 bytes, then 3 vec4
    // per chip: two corner pairs (TL,TR)/(BR,BL) + a meta vec4 (x = enabled). Same single-frame-in-
    // flight discipline as the buffers above.
    {
        const auto sc = static_cast<float>(m_contentScale);
        const auto count =
            std::min(static_cast<std::uint32_t>(m_keepChips.size()), kKeepChipMaxRects);
        auto* base = static_cast<std::uint8_t*>(m_chipPtr);
        auto* u = reinterpret_cast<std::uint32_t*>(base);
        auto* f = reinterpret_cast<float*>(base);
        u[0] = count;
        auto* data = reinterpret_cast<float*>(base + 32);
        float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
        for (std::uint32_t i = 0; i < count; ++i) {
            for (int c = 0; c < 4; ++c) {
                const float x = static_cast<float>(m_keepChips[i].corners[c].x) * sc;
                const float y = static_cast<float>(m_keepChips[i].corners[c].y) * sc;
                data[12 * i + 2 * c + 0] = x;
                data[12 * i + 2 * c + 1] = y;
                minx = std::min(minx, x);
                miny = std::min(miny, y);
                maxx = std::max(maxx, x);
                maxy = std::max(maxy, y);
            }
            data[12 * i + 8] = static_cast<float>(m_keepChips[i].state); // meta.x (chip style)
            data[12 * i + 9] = 0.0f;
            data[12 * i + 10] = 0.0f;
            data[12 * i + 11] = 0.0f;
        }
        f[2] = minx; // kChipMin @8
        f[3] = miny;
        f[4] = maxx; // kChipMax @16
        f[5] = maxy;
        vmaFlushAllocation(m_allocator, m_chipAlloc, 0, VK_WHOLE_SIZE);
    }

    // The reticle's tip-outline SDF (binding 9). The 32-byte header is rewritten every frame like the
    // others; the GRID is copied only when the tip it was traced from changed (m_sdfDirty), because it
    // is that tip's OWN silhouette and nothing the cursor, the zoom or the size does can alter it.
    // The write still happens HERE rather than in the setter, so it inherits the same
    // single-frame-in-flight discipline: the fence wait above is what guarantees the GPU has finished
    // reading last frame's field.
    {
        auto* base = static_cast<std::uint8_t*>(m_sdfPtr);
        auto* u = reinterpret_cast<std::uint32_t*>(base);
        auto* box = reinterpret_cast<float*>(base + 16);
        const bool have = m_sdfKey != 0 && m_sdfW > 0 && m_sdfH > 0 && m_reticleTracing;
        u[0] = have ? 1u : 0u;
        u[1] = m_sdfW;
        u[2] = m_sdfH;
        u[3] = m_sdfPad;
        box[0] = static_cast<float>(m_sdfBoxW);
        box[1] = static_cast<float>(m_sdfBoxH);
        box[2] = 0.0f;
        box[3] = 0.0f;
        if (m_sdfDirty) {
            if (!m_pendingSdf.empty())
                std::memcpy(base + 32, m_pendingSdf.data(), m_pendingSdf.size() * sizeof(float));
            m_pendingSdf = {}; // the field is on the GPU now; the CPU copy is dead weight
            m_sdfDirty = false;
        }
        vmaFlushAllocation(m_allocator, m_sdfAlloc, 0, VK_WHOLE_SIZE);
    }

    // The eyedropper's loupe (binding 10, S24): the whole 64-byte struct rewritten every frame like
    // the reticle header, logical px -> physical px (× content scale) for the on-screen lengths; the
    // sample texel centre stays in document px (the shader samples uDoc with it directly).
    {
        const auto sc = static_cast<float>(m_contentScale);
        auto* base = static_cast<std::uint8_t*>(m_loupePtr);
        auto* u = reinterpret_cast<std::uint32_t*>(base);
        auto* f = reinterpret_cast<float*>(base);
        u[0] = m_loupeActive ? 1u : 0u;
        f[1] = static_cast<float>(m_loupeRadius) * sc;         // radius @4
        f[2] = static_cast<float>(m_loupeMag) * sc;           // mag @8 (physical px per doc texel)
        f[3] = m_loupeReadout ? 1.0f : 0.0f;                  // readout @12
        f[4] = static_cast<float>(m_loupeCenter.x) * sc;      // center @16
        f[5] = static_cast<float>(m_loupeCenter.y) * sc;
        f[6] = static_cast<float>(m_loupeSampleDoc.x);        // sampleDoc @24 (document px)
        f[7] = static_cast<float>(m_loupeSampleDoc.y);
        f[8] = static_cast<float>(m_loupeSampleColor.r) / 255.0f; // sampleRgba @32 (ring top arc)
        f[9] = static_cast<float>(m_loupeSampleColor.g) / 255.0f;
        f[10] = static_cast<float>(m_loupeSampleColor.b) / 255.0f;
        f[11] = static_cast<float>(m_loupeSampleColor.a) / 255.0f;
        f[12] = static_cast<float>(m_loupePrevColor.r) / 255.0f; // prevRgba @48 (ring bottom arc)
        f[13] = static_cast<float>(m_loupePrevColor.g) / 255.0f;
        f[14] = static_cast<float>(m_loupePrevColor.b) / 255.0f;
        f[15] = static_cast<float>(m_loupePrevColor.a) / 255.0f;
        vmaFlushAllocation(m_allocator, m_loupeAlloc, 0, VK_WHOLE_SIZE);
    }

    // The DoF focus-band gizmo (S33, binding 11): the whole fixed struct rewritten every frame
    // like the loupe, logical px -> physical px (x content scale). Header {uint active; uint hot
    // (handle id + 1, 0 = none); pad pad}, then five guide segments (A.xy, B.xy) + the knob pair.
    // Written HERE, not in the setter -- the fence wait above is the single-frame-in-flight
    // guarantee that the GPU finished last frame's read.
    {
        const auto sc = static_cast<float>(m_contentScale);
        auto* base = static_cast<std::uint8_t*>(m_dofPtr);
        auto* u = reinterpret_cast<std::uint32_t*>(base);
        u[0] = m_dofActive ? 1u : 0u;
        u[1] = m_dofActive && m_dofHot >= 0 ? static_cast<std::uint32_t>(m_dofHot) + 1u : 0u;
        u[2] = m_dofActive ? static_cast<std::uint32_t>(m_dofGizmo.kind) : 0u; // 0 band, 1 crosshair
        u[3] = 0u;
        auto* f = reinterpret_cast<float*>(base + 16);
        const std::array<common::Vec2, 2>* segs[5] = {&m_dofGizmo.line, &m_dofGizmo.bandA,
                                                      &m_dofGizmo.bandB, &m_dofGizmo.featherA,
                                                      &m_dofGizmo.featherB};
        for (int i = 0; i < 5; ++i) {
            f[4 * i + 0] = static_cast<float>((*segs[i])[0].x) * sc;
            f[4 * i + 1] = static_cast<float>((*segs[i])[0].y) * sc;
            f[4 * i + 2] = static_cast<float>((*segs[i])[1].x) * sc;
            f[4 * i + 3] = static_cast<float>((*segs[i])[1].y) * sc;
        }
        f[20] = static_cast<float>(m_dofGizmo.centerKnob.x) * sc; // dKnobs @96
        f[21] = static_cast<float>(m_dofGizmo.centerKnob.y) * sc;
        f[22] = static_cast<float>(m_dofGizmo.rotateKnob.x) * sc;
        f[23] = static_cast<float>(m_dofGizmo.rotateKnob.y) * sc;
        vmaFlushAllocation(m_allocator, m_dofAlloc, 0, VK_WHOLE_SIZE);
    }

    // The rulers/guides + smart-guide lines (binding 13): the count then 2 vec4 per line (endpoint
    // pair scaled logical -> physical px, then the colour), rewritten every frame like the keep
    // chips. Same single-frame-in-flight discipline (the fence wait above guards last frame's read).
    {
        const auto sc = static_cast<float>(m_contentScale);
        const auto count =
            std::min(static_cast<std::uint32_t>(m_guideLines.size()), kGuideLineMax);
        auto* base = static_cast<std::uint8_t*>(m_guidePtr);
        reinterpret_cast<std::uint32_t*>(base)[0] = count;
        auto* data = reinterpret_cast<float*>(base + 16);
        for (std::uint32_t i = 0; i < count; ++i) {
            const GuideLine& g = m_guideLines[i];
            data[8 * i + 0] = static_cast<float>(g.a.x) * sc;
            data[8 * i + 1] = static_cast<float>(g.a.y) * sc;
            data[8 * i + 2] = static_cast<float>(g.b.x) * sc;
            data[8 * i + 3] = static_cast<float>(g.b.y) * sc;
            data[8 * i + 4] = g.cr;
            data[8 * i + 5] = g.cg;
            data[8 * i + 6] = g.cb;
            data[8 * i + 7] = 0.0f;
        }
        vmaFlushAllocation(m_allocator, m_guideAlloc, 0, VK_WHOLE_SIZE);
    }

    // The Pen tool's node/handle chrome (binding 6, S28): the two counts + the closing-ring fields,
    // then one vec4 per mark (pos.xy, kind, state) followed by one per stem (A.xy, B.xy), all
    // logical -> physical px. Rewritten every frame like the guides, and under the same
    // single-frame-in-flight discipline (the fence wait above guards last frame's read).
    {
        const auto sc = static_cast<float>(m_contentScale);
        const auto markCount = std::min(static_cast<std::uint32_t>(m_penMarks.size()), kPenMarkMax);
        const auto stemCount = std::min(static_cast<std::uint32_t>(m_penStems.size()), kPenStemMax);
        const bool ring = m_penRingRadius > 0.0;
        auto* base = static_cast<std::uint8_t*>(m_penPtr);
        auto* u = reinterpret_cast<std::uint32_t*>(base);
        auto* f = reinterpret_cast<float*>(base);
        u[0] = markCount;
        u[1] = stemCount;
        u[2] = ring ? 1u : 0u;
        u[3] = 0u;
        auto* data = reinterpret_cast<float*>(base + 48);
        float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
        const auto grow = [&](float x, float y) {
            minx = std::min(minx, x);
            miny = std::min(miny, y);
            maxx = std::max(maxx, x);
            maxy = std::max(maxy, y);
        };
        for (std::uint32_t i = 0; i < markCount; ++i) {
            const PenMark& m = m_penMarks[i];
            const auto x = static_cast<float>(m.pos.x) * sc;
            const auto y = static_cast<float>(m.pos.y) * sc;
            data[4 * i + 0] = x;
            data[4 * i + 1] = y;
            data[4 * i + 2] = static_cast<float>(m.kind);  // the shader rounds these back to ints
            data[4 * i + 3] = static_cast<float>(m.state);
            grow(x, y);
        }
        for (std::uint32_t i = 0; i < stemCount; ++i) {
            const PenStem& s = m_penStems[i];
            const auto ax = static_cast<float>(s.a.x) * sc;
            const auto ay = static_cast<float>(s.a.y) * sc;
            const auto bx = static_cast<float>(s.b.x) * sc;
            const auto by = static_cast<float>(s.b.y) * sc;
            data[4 * (markCount + i) + 0] = ax;
            data[4 * (markCount + i) + 1] = ay;
            data[4 * (markCount + i) + 2] = bx;
            data[4 * (markCount + i) + 3] = by;
            grow(ax, ay);
            grow(bx, by);
        }
        const auto ringX = static_cast<float>(m_penRingCenter.x) * sc;
        const auto ringY = static_cast<float>(m_penRingCenter.y) * sc;
        const auto ringR = static_cast<float>(m_penRingRadius) * sc;
        if (ring) { // the ring reaches a full radius past its centre -- the cull must cover it
            grow(ringX - ringR, ringY - ringR);
            grow(ringX + ringR, ringY + ringR);
        }
        f[4] = minx; // pMin @16
        f[5] = miny;
        f[6] = maxx; // pMax @24
        f[7] = maxy;
        f[8] = ringX; // pCloseCenter @32
        f[9] = ringY;
        f[10] = ring ? ringR : 0.0f; // pCloseRadius @40
        f[11] = 0.0f;                // pPad1 @44
        vmaFlushAllocation(m_allocator, m_penAlloc, 0, VK_WHOLE_SIZE);
    }

    // The canvas FPS readout (binding 12, debug builds only): spell the rate as "<n> FPS" -- digit
    // glyphs (0..9), a space (10), then F/P/S (11/12/13) -- into the fixed glyph array, and size the
    // badge to the HiDPI content scale. Rewritten every frame like the loupe/DoF structs.
#ifdef MOSAIC_DEBUG
    {
        auto* u = static_cast<std::uint32_t*>(m_fpsPtr);
        std::uint32_t glyphs[16];
        std::uint32_t n = 0;
        const int v = std::clamp(m_fpsValue, 0, 9999);
        std::uint32_t digits[4];
        int dn = 0;
        int t = v;
        do {
            digits[dn++] = static_cast<std::uint32_t>(t % 10);
            t /= 10;
        } while (t > 0 && dn < 4);
        for (int i = dn - 1; i >= 0 && n < 16; --i)
            glyphs[n++] = digits[i]; // most-significant digit first
        for (std::uint32_t g : {10u, 11u, 12u, 13u}) // " FPS"
            if (n < 16)
                glyphs[n++] = g;
        const int px = static_cast<int>(std::lround(3.0 * m_contentScale));
        u[0] = m_fpsActive ? 1u : 0u;
        u[1] = n;
        u[2] = static_cast<std::uint32_t>(px < 2 ? 2 : px);
        u[3] = 0u;
        for (std::uint32_t i = 0; i < 16; ++i)
            u[4 + i] = i < n ? glyphs[i] : 0u;
        vmaFlushAllocation(m_allocator, m_fpsAlloc, 0, VK_WHOLE_SIZE);
    }
#endif // MOSAIC_DEBUG

    std::uint32_t index = 0;
    VkResult acquired = VK_SUCCESS;
    {
        // The compositor's half of the pace: with no image free this blocks until one is, so a
        // large number here means the display (or the compositor) is the limit and nothing in this
        // file will move it. Separated from the frame fence above, which is the DEVICE's half.
        MOSAIC_PERF_SCOPE("Present frame (acquire)", common::Lane::Gpu);
        acquired = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, m_imageAvailable,
                                         VK_NULL_HANDLE, &index);
    }
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        return recreate(error); // try again next frame
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        error = "vkAcquireNextImageKHR failed";
        return false;
    }

    vkResetFences(m_device, 1, &m_inFlight);

    VkCommandBuffer cmd = m_commandBuffers[index];
    vkResetCommandBuffer(cmd, 0);
    const VkCommandBufferBeginInfo cbi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &cbi);
    // Immediately after begin: a timestamp query must be RESET on the command buffer before it is
    // written (the host-side vkResetQueryPool is 1.2, and this floors on 1.0). `tFrame` brackets
    // the whole submission, so its device row is the twin of the `Present frame` CPU row above.
    if (m_timer)
        m_timer->beginSubmission(cmd);
    const std::int32_t tFrame = m_timer ? m_timer->beginScope(cmd, "Present frame") : -1;

    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Upload the staged document pixels into the canvas texture, then leave it in
    // SHADER_READ_ONLY so the present compute pass can sample it (this frame and later ones,
    // which reuse it without re-uploading).
    if (uploadCanvas || uploadCanvasRegion) {
        // The device half of the upload whose host half is `Canvas upload (stage)`: what the bus
        // actually spends moving those bytes, as opposed to what the memcpy cost.
        const std::int32_t tCanvas = m_timer ? m_timer->beginScope(cmd, "Canvas upload") : -1;
        const bool wasReadable = (m_canvasLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkImageMemoryBarrier toCanvasDst{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask =
                static_cast<VkAccessFlags>(wasReadable ? VK_ACCESS_SHADER_READ_BIT : 0),
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = m_canvasLayout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_canvasImage,
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(
            cmd,
            wasReadable ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toCanvasDst);
        // Full upload writes the whole texture; a region upload patches only the sub-rectangle
        // (bufferRowLength/Height describe the tightly-packed sub-image staged at offset 0). The
        // two are mutually exclusive this frame (region is suppressed when a full upload is queued).
        const VkBufferImageCopy region =
            uploadCanvas
                ? VkBufferImageCopy{.bufferOffset = 0,
                                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {m_canvasW, m_canvasH, 1}}
                : VkBufferImageCopy{
                      .bufferOffset = 0,
                      .bufferRowLength = regionW,
                      .bufferImageHeight = regionH,
                      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .imageOffset = {static_cast<std::int32_t>(regionX),
                                      static_cast<std::int32_t>(regionY), 0},
                      .imageExtent = {regionW, regionH, 1}};
        vkCmdCopyBufferToImage(cmd, m_canvasStaging, m_canvasImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageMemoryBarrier toCanvasRead{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_canvasImage,
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toCanvasRead);
        m_canvasLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        m_canvasValid = true;
        if (m_timer)
            m_timer->endScope(cmd, tCanvas);
    }

    // S60-a item 11: the resident compositor resolved its accumulator straight into the canvas
    // texture on the device -- no readback, no CPU mirror, no staging, no upload -- and left it in
    // GENERAL. All that costs here is the transition the present descriptor expects. This is the
    // only path that can leave the canvas in GENERAL between frames: the drag pass below both
    // enters and leaves GENERAL inside one command buffer.
    if (m_canvasLayout == VK_IMAGE_LAYOUT_GENERAL && m_canvasImage != VK_NULL_HANDLE) {
        const VkImageMemoryBarrier resolved{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_canvasImage,
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &resolved);
        m_canvasLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    // Upload the staged selection mask, mirroring the canvas upload above.
    if (uploadMask) {
        const bool wasReadable = (m_maskLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkImageMemoryBarrier toMaskDst{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask =
                static_cast<VkAccessFlags>(wasReadable ? VK_ACCESS_SHADER_READ_BIT : 0),
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = m_maskLayout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_maskImage,
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(
            cmd,
            wasReadable ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toMaskDst);
        const VkBufferImageCopy region{
            .bufferOffset = 0,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {m_maskW, m_maskH, 1},
        };
        vkCmdCopyBufferToImage(cmd, m_maskStaging, m_maskImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageMemoryBarrier toMaskRead{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_maskImage,
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toMaskRead);
        m_maskLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        m_maskValid = true;
    }

    // Upload the staged overlay-text tile, mirroring the mask upload above.
    if (uploadOverlay) {
        const bool wasReadable = (m_overlayLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkImageMemoryBarrier toDst{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask =
                static_cast<VkAccessFlags>(wasReadable ? VK_ACCESS_SHADER_READ_BIT : 0),
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = m_overlayLayout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_overlayImage,
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(
            cmd,
            wasReadable ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
        const VkBufferImageCopy region{
            .bufferOffset = 0,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {m_overlayW, m_overlayH, 1},
        };
        vkCmdCopyBufferToImage(cmd, m_overlayStaging, m_overlayImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageMemoryBarrier toRead{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_overlayImage,
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toRead);
        m_overlayLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        m_overlayValid = true;
    }

    // GPU-resident Move/Resize/Rotate drag (S60-a): composite the static `below` texture + the
    // dragged layer (sampled through its live transform) straight into the canvas texture, so a
    // transform gesture needs no CPU recomposite or whole-canvas upload. Runs before the present
    // pass, which then samples the canvas texture exactly as usual.
    if (m_dragActive && m_canvasImage != VK_NULL_HANDLE && m_belowImage != VK_NULL_HANDLE &&
        m_draggedImage != VK_NULL_HANDLE) {
        // The per-frame half of the gesture, on the device timeline. Its CPU counterpart is the
        // app's `Transform drag (GPU)` row; this is the one that says whether the drag shader
        // itself costs anything, which nothing has been able to answer until now.
        const std::int32_t tDrag = m_timer ? m_timer->beginScope(cmd, "Drag composite") : -1;
        VkImageMemoryBarrier toGeneral{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0, // the in-flight fence already drained last frame's use
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = m_canvasLayout,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_canvasImage,
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toGeneral);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_dragPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_dragPipelineLayout, 0, 1,
                                &m_dragDescSet, 0, nullptr);
        const DragPush dp{
            .invR0 = {m_dragInv[0], m_dragInv[1]},
            .invR1 = {m_dragInv[2], m_dragInv[3]},
            .invT = {m_dragInv[4], m_dragInv[5]},
            .docSize = {static_cast<float>(m_canvasW), static_cast<float>(m_canvasH)},
            .draggedSize = {static_cast<float>(m_draggedW), static_cast<float>(m_draggedH)},
            .mode = m_dragMode,
            .opacity = m_dragOpacity,
        };
        vkCmdPushConstants(cmd, m_dragPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dp),
                           &dp);
        vkCmdDispatch(cmd, (m_canvasW + 7) / 8, (m_canvasH + 7) / 8, 1);
        VkImageMemoryBarrier toRead{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_canvasImage,
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toRead);
        m_canvasLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (m_timer)
            m_timer->endScope(cmd, tDrag);
    }

    // Present the document through the view transform (S8): a compute pass samples it into the
    // rgba8 view image, which is then blitted 1:1 onto the swapchain. Before any document exists,
    // fall back to a plain background clear. (The mask + overlay textures always exist by here --
    // the 1x1 placeholders at minimum -- so the descriptor set is fully valid whenever we dispatch.)
    const bool drawDocument = m_canvasValid && m_maskValid && m_overlayValid &&
                              m_viewImage != VK_NULL_HANDLE;
    // The documentless idle pass (canvas_idle.comp): standalone it replaces the bare background
    // clear below; over a document it blends the settling field on top of the present pass's
    // output (the open-document crossfade). Inactive (the common case with a document), it costs
    // nothing.
    const bool idleWants = m_idleField.active && m_idlePipeline != VK_NULL_HANDLE &&
                           m_viewImage != VK_NULL_HANDLE;
    const bool idleStandalone = idleWants && !drawDocument;
    if (drawDocument && m_descDirty)
        writeDescriptors();
    if (idleWants && m_idleDescDirty)
        writeIdleDescriptors();
    const auto recordIdleDispatch = [&](float mode) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_idlePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_idlePipelineLayout, 0, 1,
                                &m_idleDescSet, 0, nullptr);
        // The invitation quad is the atlas row, centred in the viewport; without a real atlas
        // the field renders bare (no quad, no quiet zone).
        const float rowW = m_idleAtlasReal ? static_cast<float>(m_idleAtlasW) : 0.0f;
        const float rowH = m_idleAtlasReal ? static_cast<float>(m_idleAtlasH) /
                                                 static_cast<float>(m_idleAtlasRows)
                                           : 0.0f;
        const float cx = static_cast<float>(m_extent.width) * 0.5f;
        const float cy = static_cast<float>(m_extent.height) * 0.5f;
        const float quietW =
            m_idleAtlasReal ? rowW * 0.5f + m_idleField.quietPad : -1.0f;
        const float quietH =
            m_idleAtlasReal ? rowH * 0.5f + m_idleField.quietPad : -1.0f;
        const IdlePush pc{
            .bgColor = {static_cast<float>(clearColor.r) / 255.0f,
                        static_cast<float>(clearColor.g) / 255.0f,
                        static_cast<float>(clearColor.b) / 255.0f, mode},
            .ink = {m_idleField.ink[0], m_idleField.ink[1], m_idleField.ink[2], m_idleField.fade},
            .accent = {m_idleField.accent[0], m_idleField.accent[1], m_idleField.accent[2],
                       m_idleField.hot},
            .outSize = {static_cast<float>(m_extent.width), static_cast<float>(m_extent.height)},
            .timePhase = m_idleField.timePhase,
            .scale = static_cast<float>(m_contentScale),
            .center = {cx, cy},
            .quiet = {quietW, quietH},
            .invPos = {cx - rowW * 0.5f, cy - rowH * 0.5f},
            .invSize = {rowW, rowH},
            .invAlpha = m_idleAtlasReal ? m_idleField.invAlpha : 0.0f,
            .hover = m_idleField.hover,
            .amp = m_idleField.amp,
            .pitch = m_idleField.pitch,
        };
        vkCmdPushConstants(cmd, m_idlePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc),
                           &pc);
        vkCmdDispatch(cmd, (m_extent.width + 7) / 8, (m_extent.height + 7) / 8, 1);
    };

    if (drawDocument) {
        // Every overlay this app draws -- ants, reticle, loupe, pen chrome, guides, the pixel grid
        // -- is a branch inside canvas_present.comp, evaluated per screen pixel. This is the row
        // that says what that costs on the device, and it is the only honest way to tell "the
        // canvas feels heavy" from "the frame loop is waiting on something else".
        const std::int32_t tPresent = m_timer ? m_timer->beginScope(cmd, "Present dispatch") : -1;
        VkImageMemoryBarrier viewToGeneral{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, // every pixel is rewritten
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_viewImage,
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &viewToGeneral);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_presentPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1,
                                &m_descSet, 0, nullptr);
        const common::Affine2D inv = presentInverse(); // screen(physical px) -> doc px
        const auto snapToPixelCenter = [scale = m_contentScale](double logical) {
            return static_cast<float>(std::floor(logical * scale) + 0.5); // see .hc01 below
        };
        // One controls lane serves the Move handles and the crop rect (crop wins -- the tools
        // are never active together, see setCropOverlay).
        // The inpaint sample-area preview (mode 4, S39) claims the quad lanes when active; otherwise
        // crop wins over the Move handles (they're never active together). The Type box's rotate
        // dots (mode 6) have the LOWEST claim (ctlTextDots, the shared priority rule above).
        const std::array<common::Vec2, 4>& quadCorners = m_framingActive      ? m_framingCorners
                                                         : m_sampleAreaActive ? m_sampleAreaCorners
                                                         : m_cropActive       ? m_cropCorners
                                                         : ctlTextDots        ? m_textDotCorners
                                                                              : m_handleCorners;
        // The crop channel's controls mode: the Crop tool's own box (3 with the rule-of-thirds
        // guides, 2 without), or one of the Image menu's preview flavours -- 7 the Image Size
        // resample (which adds the ghost outline of the CURRENT document frame, the only thing
        // that makes a same-shaped box read as a size CHANGE), 8 a derived rect drawn without
        // handles (Rotate Arbitrary). Reframe -- Canvas Size -- is the crop picture unchanged, so
        // it is simply mode 2. See setCropOverlay's CropChannel.
        const float cropMode = m_cropChannel == CropChannel::Scale    ? 7.0f
                               : m_cropChannel == CropChannel::Locked ? 8.0f
                               : m_cropShowGrid                       ? 3.0f
                                                                      : 2.0f;

        // The rotation dial and the crop HUD share these push lanes -- they never coexist (the dial
        // wins, suppressing the HUD), so whichever is active claims them. overlayCenter carries the
        // overlay TILE's content size (physical px); the dial's centre is the viewport centre,
        // recomputed in the shader. overlay = {dial active, dial radius, dial angle, HUD active}.
        // The proportional term is already in physical px; the floor is a LOGICAL 48 px, so a small
        // viewport's dial stays the same angular size on a HiDPI display as it does on a 1x one.
        const float dialRadius =
            std::max(48.0f * static_cast<float>(m_contentScale),
                     0.16f * static_cast<float>(std::min(m_extent.width, m_extent.height)));
        const float overlayCenter[2] = {static_cast<float>(m_overlayContentW),
                                        static_cast<float>(m_overlayContentH)};
        // overlay.w selects the HUD placement: 0 none, 1 crop (below the box), 2 Move (bottom-right).
        // The dial (overlay.x) suppresses any HUD -- they share the one rasterized tile.
        const float hudMode = m_overlayActive          ? 0.0f
                              : m_moveHudActive ? 2.0f
                              : m_cropHudActive ? 1.0f
                                                : 0.0f;
        const float overlay[4] = {m_overlayActive ? 1.0f : 0.0f, dialRadius,
                                  static_cast<float>(m_overlayAngle), hudMode};
        const PresentPush pc{
            .bgColor = {static_cast<float>(clearColor.r) / 255.0f,
                        static_cast<float>(clearColor.g) / 255.0f,
                        static_cast<float>(clearColor.b) / 255.0f,
                        m_pixelGrid ? 1.0f
                                    : 0.0f}, // .a repurposed as the pixel-grid toggle (S19-c)
            .invR0 = {static_cast<float>(inv.m00), static_cast<float>(inv.m01)},
            .invR1 = {static_cast<float>(inv.m10), static_cast<float>(inv.m11)},
            .invT = {static_cast<float>(inv.m02), static_cast<float>(inv.m12)},
            .docSize = {static_cast<float>(m_canvasW), static_cast<float>(m_canvasH)},
            .outSize = {static_cast<float>(m_extent.width), static_cast<float>(m_extent.height)},
            .overlayCenter = {overlayCenter[0], overlayCenter[1]},
            .overlay = {overlay[0], overlay[1], overlay[2], overlay[3]},
            // x encodes the ants mode: 0 off, 1 the default diagonal crawl, 2 the circulating
            // tangent (§5). It stays 1.0 whenever circulate is off, so the default render is
            // byte-unchanged.
            .ants = {m_antsEnabled ? (m_antsCirculate ? 2.0f : 1.0f) : 0.0f, m_antsPhase,
                     m_framingActive      ? 9.0f // the Zoom tool's framing preview
                     : m_lineGizmoActive  ? 5.0f // the Line shape gizmo (S26)
                     : m_sampleAreaActive ? 4.0f
                     : m_cropActive       ? cropMode
                     : m_handlesActive    ? 1.0f
                     : ctlTextDots        ? 6.0f // the Type box's rotate-hotspot dots, alone
                                          : 0.0f,
                     4.0f * static_cast<float>(m_contentScale)},
            // Corners snap to physical-pixel CENTRES: the shader samples pixel centres, so a
            // corner at a fractional position makes the quad's 1-px outline straddle two rows
            // (visibly uneven weight along axis-aligned edges, S15 polish report). Snapping
            // only quantizes by half a pixel — imperceptible at the handle scale.
            .hc01 = {snapToPixelCenter(quadCorners[0].x), snapToPixelCenter(quadCorners[0].y),
                     snapToPixelCenter(quadCorners[1].x), snapToPixelCenter(quadCorners[1].y)},
            .hc23 = {snapToPixelCenter(quadCorners[2].x), snapToPixelCenter(quadCorners[2].y),
                     snapToPixelCenter(quadCorners[3].x), snapToPixelCenter(quadCorners[3].y)},
        };
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (m_extent.width + 7) / 8, (m_extent.height + 7) / 8, 1);

        if (idleWants) {
            // The open-document crossfade: the settling field blends over the present output it
            // just wrote, so the document is revealed beneath the sinking dots. Compute-to-compute
            // dependency on the view image (the idle pass reads what present wrote).
            VkImageMemoryBarrier presentToIdle{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = m_viewImage,
                .subresourceRange = range,
            };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                                 1, &presentToIdle);
            recordIdleDispatch(1.0f); // blend-over mode
        }

        VkImageMemoryBarrier viewToSrc{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_viewImage,
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &viewToSrc);
        if (m_timer)
            m_timer->endScope(cmd, tPresent);
    } else if (idleStandalone) {
        // No document: the idle pass writes the whole view image (background + field +
        // invitation) and the swapchain blits it -- this branch replaces the bare background
        // clear the empty state used to sit on. Same transitions as the document path.
        VkImageMemoryBarrier viewToGeneral{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, // every pixel is rewritten
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_viewImage,
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &viewToGeneral);
        recordIdleDispatch(0.0f); // standalone mode
        VkImageMemoryBarrier viewToSrc{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_viewImage,
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &viewToSrc);
    }

    VkImageMemoryBarrier swapToDst{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = m_images[index],
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &swapToDst);

    if (drawDocument || idleStandalone) {
        // 1:1 blit; rgba8 -> the swapchain's bgra is mapped by component, so colors are correct.
        const VkImageBlit blit{
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffsets = {{0, 0, 0},
                           {static_cast<std::int32_t>(m_extent.width),
                            static_cast<std::int32_t>(m_extent.height), 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffsets = {{0, 0, 0},
                           {static_cast<std::int32_t>(m_extent.width),
                            static_cast<std::int32_t>(m_extent.height), 1}},
        };
        vkCmdBlitImage(cmd, m_viewImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_images[index],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    } else {
        VkClearColorValue clear{};
        clear.float32[0] = static_cast<float>(clearColor.r) / 255.0f;
        clear.float32[1] = static_cast<float>(clearColor.g) / 255.0f;
        clear.float32[2] = static_cast<float>(clearColor.b) / 255.0f;
        clear.float32[3] = static_cast<float>(clearColor.a) / 255.0f;
        vkCmdClearColorImage(cmd, m_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                             &range);
    }

    VkImageMemoryBarrier toPresent{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = m_images[index],
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toPresent);

    if (m_timer)
        m_timer->endScope(cmd, tFrame); // last command written: the frame's device row closes here
    vkEndCommandBuffer(cmd);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo si{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &m_imageAvailable,
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &m_renderFinished[index],
    };
    {
        // Recording is done; this is the handover. It should be near-free, and a row that is not
        // says the driver is validating or the queue is contended -- neither of which is fixed by
        // touching a shader.
        MOSAIC_PERF_SCOPE("Present frame (submit)", common::Lane::Gpu);
        if (vkQueueSubmit(m_queue, 1, &si, m_inFlight) != VK_SUCCESS) {
            error = "vkQueueSubmit failed";
            return false;
        }
    }

    // Tag the present so anyone can ask the PRESENTATION ENGINE when this exact frame reached the
    // screen (vkWaitForPresentKHR -- lastPresentDisplayed).
    //
    // ⚠ This tag is NOT what paces the frame loop, and the comment that used to stand here saying
    // it was -- "no refresh-rate query anywhere" -- was wrong twice over. It cannot bound a frame
    // rate under MAILBOX (see lastPresentDisplayed's own warning), and its premise, that Wayland
    // will never say which panel a window is on, is false: wl_surface.enter says exactly that and
    // FLTK already tracks it, which is what platform::displayRefreshHz reads. The chain stays
    // because the extensions are enabled (gpu_caps.cpp) and a correctly increasing presentId is
    // what makes them mean anything; it costs one increment and a pNext.
    ++m_presentId;
    const VkPresentIdKHR presentId{
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .swapchainCount = 1,
        .pPresentIds = &m_presentId,
    };
    const VkPresentInfoKHR present{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = m_caps.presentWait ? &presentId : nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &m_renderFinished[index],
        .swapchainCount = 1,
        .pSwapchains = &m_swapchain,
        .pImageIndices = &index,
    };
    VkResult presented = VK_SUCCESS;
    {
        // Queueing the present, not waiting for it to be scanned out (that is
        // lastPresentDisplayed's job, and it never blocks). A cost here is the presentation
        // engine's queue being full -- i.e. we are ahead of the display, which is where we want
        // to be.
        MOSAIC_PERF_SCOPE("Present frame (queue present)", common::Lane::Gpu);
        presented = vkQueuePresentKHR(m_queue, &present);
    }
    if (presented == VK_ERROR_OUT_OF_DATE_KHR) {
        m_needsRecreate = true; // genuinely unusable: rebuild before the next frame
    } else if (presented == VK_SUBOPTIMAL_KHR) {
        // SUBOPTIMAL is NOT an error. It means "this still presents correctly, but the swapchain no
        // longer matches the surface's preferred configuration" -- and some drivers return it on
        // EVERY present, permanently. Rebuilding unconditionally here is what made the Windows
        // build flash: recreate() is a full vkDeviceWaitIdle + teardown + rebuild with no
        // oldSwapchain handoff, so a persistently-suboptimal surface tore the swapchain down and
        // rebuilt it once per frame, and the blank frame in between is the flash.
        //
        // A real resize does not need this path at all: it arrives through notifyResize() from the
        // window's own resize handler, and a swapchain that has actually become unusable reports
        // OUT_OF_DATE above. So rebuild only when the surface's extent genuinely moved out from
        // under us, and otherwise keep presenting -- which is what SUBOPTIMAL invites us to do.
        m_needsRecreate = surfaceExtentChanged();
    } else if (presented != VK_SUCCESS) {
        error = "vkQueuePresentKHR failed";
        return false;
    }
    return true;
}

bool WindowRenderer::lastPresentDisplayed() const noexcept {
    // "Has the frame I last presented actually been scanned out?" -- polled with a ZERO timeout, so
    // this never blocks the UI thread. VK_TIMEOUT means "not yet"; anything else (including an
    // out-of-date swapchain, which the next frame will recreate) means stop waiting on it.
    if (!m_caps.presentWait || m_presentId == 0 || m_swapchain == VK_NULL_HANDLE)
        return true; // no pacing signal available: never make the caller wait for one
    const auto waitForPresent = reinterpret_cast<PFN_vkWaitForPresentKHR>(
        vkGetDeviceProcAddr(m_device, "vkWaitForPresentKHR"));
    if (waitForPresent == nullptr)
        return true;
    return waitForPresent(m_device, m_swapchain, m_presentId, 0) != VK_TIMEOUT;
}

void WindowRenderer::waitIdle() const noexcept {
    if (m_device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(m_device);
}

std::string WindowRenderer::deviceName() const {
    if (m_physicalDevice == VK_NULL_HANDLE)
        return "(none)";
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
    return props.deviceName;
}

std::string WindowRenderer::takeStartupNotice() {
    // Move-and-clear: the second caller gets "", so the frame loop may ask every frame (it has to
    // -- the renderer is created lazily, so the first frame that HAS one is not knowable in
    // advance) and the user is told exactly once.
    std::string notice = std::move(m_startupNotice);
    m_startupNotice.clear(); // a moved-from std::string is valid but unspecified; pin it to empty
    return notice;
}

WindowRenderer::~WindowRenderer() {
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
        m_timer.reset(); // owns a VkQueryPool on this device; destroy it while the device is alive
        // Drop the borrowing compute context BEFORE the device it borrows. A lane still holding a
        // shared_ptr to it here would keep a dangling VkDevice, so the owner releasing first is
        // the invariant: every lane must be torn down with (or before) the window.
        m_computeCtx.reset();
        destroySwapchainObjects();
        destroyCanvasTexture(); // frees VMA image/buffer; must precede vmaDestroyAllocator
        destroyMaskTexture();
        destroyOverlayTexture();
        destroyDragTextures(); // S60-a per-gesture below/dragged textures
        for (const DeadTexture& t : m_deadDragTextures) { // any retired but not-yet-drained textures
            if (t.view != VK_NULL_HANDLE)
                vkDestroyImageView(m_device, t.view, nullptr);
            if (t.image != VK_NULL_HANDLE)
                vmaDestroyImage(m_allocator, t.image, t.alloc);
        }
        m_deadDragTextures.clear();
        if (m_dragDescPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(m_device, m_dragDescPool, nullptr);
        if (m_dragPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(m_device, m_dragPipeline, nullptr);
        if (m_dragShader != VK_NULL_HANDLE)
            vkDestroyShaderModule(m_device, m_dragShader, nullptr);
        if (m_dragPipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(m_device, m_dragPipelineLayout, nullptr);
        if (m_dragSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(m_device, m_dragSetLayout, nullptr);
        if (m_idleAtlasView != VK_NULL_HANDLE)
            vkDestroyImageView(m_device, m_idleAtlasView, nullptr);
        if (m_idleAtlasImage != VK_NULL_HANDLE)
            vmaDestroyImage(m_allocator, m_idleAtlasImage, m_idleAtlasAlloc);
        if (m_idleDescPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(m_device, m_idleDescPool, nullptr);
        if (m_idlePipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(m_device, m_idlePipeline, nullptr);
        if (m_idleShader != VK_NULL_HANDLE)
            vkDestroyShaderModule(m_device, m_idleShader, nullptr);
        if (m_idlePipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(m_device, m_idlePipelineLayout, nullptr);
        if (m_idleSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(m_device, m_idleSetLayout, nullptr);
        if (m_lassoBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(m_allocator, m_lassoBuffer, m_lassoAlloc);
        if (m_textBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(m_allocator, m_textBuffer, m_textAlloc);
        if (m_penBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(m_allocator, m_penBuffer, m_penAlloc);
        if (m_chipBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(m_allocator, m_chipBuffer, m_chipAlloc);
        if (m_sdfBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(m_allocator, m_sdfBuffer, m_sdfAlloc);
        if (m_loupeBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(m_allocator, m_loupeBuffer, m_loupeAlloc);
        if (m_dofBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(m_allocator, m_dofBuffer, m_dofAlloc);
        if (m_guideBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(m_allocator, m_guideBuffer, m_guideAlloc);
#ifdef MOSAIC_DEBUG
        if (m_fpsBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(m_allocator, m_fpsBuffer, m_fpsAlloc);
#endif
        if (m_sampler != VK_NULL_HANDLE)
            vkDestroySampler(m_device, m_sampler, nullptr);
        if (m_descPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(m_device, m_descPool, nullptr); // frees the set
        if (m_presentPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(m_device, m_presentPipeline, nullptr);
        if (m_presentShader != VK_NULL_HANDLE)
            vkDestroyShaderModule(m_device, m_presentShader, nullptr);
        if (m_pipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        if (m_setLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        if (m_allocator != nullptr)
            vmaDestroyAllocator(m_allocator);
        if (m_inFlight != VK_NULL_HANDLE)
            vkDestroyFence(m_device, m_inFlight, nullptr);
        if (m_imageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device, m_imageAvailable, nullptr);
        }
        if (m_commandPool != VK_NULL_HANDLE)
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        vkDestroyDevice(m_device, nullptr);
    }
    if (m_surface != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_messenger != VK_NULL_HANDLE) {
        auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyMessenger)
            destroyMessenger(m_instance, m_messenger, nullptr);
    }
    if (m_instance != VK_NULL_HANDLE)
        vkDestroyInstance(m_instance, nullptr);
}

} // namespace mosaic::render
