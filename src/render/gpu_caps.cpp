#include "render/gpu_caps.hpp"

#include "common/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>

namespace mosaic::render {
namespace {

// VK_KHR_portability_subset lives behind VK_ENABLE_BETA_EXTENSIONS in the headers, and we do not
// want the beta header for one string. The name is stable ABI, so spell it.
constexpr const char* kPortabilitySubset = "VK_KHR_portability_subset";

[[nodiscard]] bool has(const std::vector<std::string>& exts, const char* name) noexcept {
    return std::find(exts.begin(), exts.end(), name) != exts.end();
}

// A device supports a capability either because its API version promoted the extension to core,
// or because it still advertises the extension. Both routes are equal here -- which is the whole
// point of the flags: callers never learn which one it was.
[[nodiscard]] bool coreOrExt(std::uint32_t apiVersion, std::uint32_t promotedIn,
                             const std::vector<std::string>& exts, const char* name) noexcept {
    return apiVersion >= promotedIn || has(exts, name);
}

}  // namespace

// ---- Floor limits ------------------------------------------------------------------------------

VkPhysicalDeviceLimits floorLimits() noexcept {
    VkPhysicalDeviceLimits l{};
    // Only the fields this renderer reads. The rest stay zero: a zero limit that some future code
    // divides by is a loud bug, whereas a fabricated plausible value is a silent one.
    l.maxImageDimension1D = vk10::kMaxImageDimension2D;
    l.maxImageDimension2D = vk10::kMaxImageDimension2D;
    l.maxImageDimension3D = 256;
    l.maxImageDimensionCube = vk10::kMaxImageDimension2D;
    l.maxImageArrayLayers = 256;
    l.maxStorageBufferRange = static_cast<std::uint32_t>(vk10::kMaxStorageBufferRange);
    // A "min" limit: the spec caps how COARSE an implementation may be, so the worst legal device
    // is the largest value, not the smallest. Left at zero this would be a division by zero in the
    // first caller that packs sub-ranges into one storage buffer, which is the tile list's shape.
    l.minStorageBufferOffsetAlignment =
        static_cast<VkDeviceSize>(vk10::kMaxStorageBufferOffsetAlignment);
    l.maxUniformBufferRange = 16384;
    l.maxPushConstantsSize = vk10::kMaxPushConstantsSize;
    l.maxMemoryAllocationCount = vk10::kMaxMemoryAllocationCount;
    l.maxBoundDescriptorSets = vk10::kMaxBoundDescriptorSets;
    l.maxPerStageDescriptorSamplers = 16;
    l.maxPerStageDescriptorUniformBuffers = 12;
    l.maxPerStageDescriptorStorageBuffers = vk10::kMaxPerStageDescriptorStorageBuffers;
    l.maxPerStageDescriptorSampledImages = vk10::kMaxPerStageDescriptorSampledImages;
    l.maxPerStageDescriptorStorageImages = vk10::kMaxPerStageDescriptorStorageImages;
    l.maxPerStageResources = 128;
    l.maxDescriptorSetStorageBuffers = vk10::kMaxDescriptorSetStorageBuffers;
    l.maxDescriptorSetStorageImages = vk10::kMaxDescriptorSetStorageImages;
    l.maxDescriptorSetSampledImages = 96;
    l.maxComputeSharedMemorySize = vk10::kMaxComputeSharedMemorySize;
    l.maxComputeWorkGroupCount[0] = vk10::kMaxComputeWorkGroupCount;
    l.maxComputeWorkGroupCount[1] = vk10::kMaxComputeWorkGroupCount;
    l.maxComputeWorkGroupCount[2] = vk10::kMaxComputeWorkGroupCount;
    l.maxComputeWorkGroupInvocations = vk10::kMaxComputeWorkGroupInvocations;
    l.maxComputeWorkGroupSize[0] = 128;
    l.maxComputeWorkGroupSize[1] = 128;
    l.maxComputeWorkGroupSize[2] = 64;
    l.timestampComputeAndGraphics = VK_FALSE;
    l.timestampPeriod = 0.0f;
    return l;
}

// ---- The pure decision --------------------------------------------------------------------------

GpuCaps decide(const GpuProbe& probe) noexcept {
    GpuCaps caps;

    // The version we may actually USE is the lower of the two: a 1.3 device behind a 1.0 loader
    // is a 1.0 device as far as this process is concerned.
    caps.apiVersion = std::min(probe.instanceApiVersion, probe.deviceApiVersion);
    caps.deviceName = probe.deviceName;
    caps.deviceType = probe.deviceType;
    caps.softwareDevice = probe.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    caps.limits = probe.limits;

    const auto& e = probe.deviceExtensions;
    const std::uint32_t v = caps.apiVersion;

    // Tier flags. Each needs BOTH the interface (core version or extension) AND, where one
    // exists, the feature bit -- an advertised extension whose feature is disabled is exactly
    // the "incomplete 1.3/1.4 support" case this model was built for.
    caps.subgroupArithmetic =
        (probe.subgroupOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) != 0 && probe.subgroupSize > 0;
    caps.storage16Bit =
        coreOrExt(v, VK_API_VERSION_1_1, e, VK_KHR_16BIT_STORAGE_EXTENSION_NAME) &&
        probe.featStorageBuffer16BitAccess;
    caps.shaderFloat16 =
        coreOrExt(v, VK_API_VERSION_1_2, e, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME) &&
        probe.featShaderFloat16;
    caps.timelineSemaphore =
        coreOrExt(v, VK_API_VERSION_1_2, e, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) &&
        probe.featTimelineSemaphore;
    // Descriptor indexing is only useful to the tiled compositor with the four sub-features that
    // make a runtime-sized, partially-bound, non-uniformly-indexed tile array legal. Anything
    // less and the per-tile dispatch loop is the correct path, so demand all four.
    caps.descriptorIndexing =
        coreOrExt(v, VK_API_VERSION_1_2, e, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) &&
        probe.featRuntimeDescriptorArray && probe.featSampledImageArrayNonUniformIndexing &&
        probe.featDescriptorBindingPartiallyBound &&
        probe.featDescriptorBindingVariableDescriptorCount;
    caps.bufferDeviceAddress =
        coreOrExt(v, VK_API_VERSION_1_2, e, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) &&
        probe.featBufferDeviceAddress;
    caps.synchronization2 =
        coreOrExt(v, VK_API_VERSION_1_3, e, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) &&
        probe.featSynchronization2;
    caps.maintenance4 = coreOrExt(v, VK_API_VERSION_1_3, e, VK_KHR_MAINTENANCE_4_EXTENSION_NAME) &&
                        probe.featMaintenance4;
    caps.hostImageCopy = coreOrExt(v, VK_API_VERSION_1_4, e, VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME) &&
                         probe.featHostImageCopy;
    // Never promoted to core, and useless by halves: waiting needs an id to wait ON, so both
    // extensions and both feature bits have to be there or the capability is false.
    caps.presentWait = has(e, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
                       has(e, VK_KHR_PRESENT_WAIT_EXTENSION_NAME) && probe.featPresentId &&
                       probe.featPresentWait;
    // No feature bit, no promotion: pure extension presence.
    caps.memoryBudget = has(e, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    caps.dedicatedAllocation =
        coreOrExt(v, VK_API_VERSION_1_1, e, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
    caps.portabilitySubset = has(e, kPortabilitySubset);
    // Timestamps need all three: the limit that says every graphics/compute family can write one,
    // a period to convert ticks with, and a non-zero counter width on the family we submit on.
    // Any of the three missing and the profiler keeps reporting submit wall-clock, which is
    // honest; a timer built on a zero period or a zero-width counter would report noise, which is
    // not (docs/s60-performance-plan.md section 8.1).
    caps.timestampQueries = probe.limits.timestampComputeAndGraphics == VK_TRUE &&
                            probe.limits.timestampPeriod > 0.0f && probe.timestampValidBits > 0;
    caps.timestampValidBits = caps.timestampQueries ? probe.timestampValidBits : 0;

    // ---- Working format ------------------------------------------------------------------------
    // rgba16f first, even where rgba32f is offered: it is the only float format Vulkan 1.0
    // guarantees for BOTH storage and linear filtering, at half the memory and bandwidth. Note
    // the asymmetry that makes this more than a preference -- rgba32f's linear-filter support is
    // NOT guaranteed, so a bilinear sample of a fp32 working buffer is not portable.
    if (probe.fmtRgba16fStorage && probe.fmtRgba16fLinearFilter) {
        caps.workingFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    } else if (probe.fmtRgba32fStorage && probe.fmtRgba32fLinearFilter) {
        caps.workingFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
    } else {
        // The desperate floor. Re-introduces the banding ImageF exists to avoid, so taking this
        // branch must be visible -- summary() says so and the caller logs it.
        caps.workingFormat = VK_FORMAT_R8G8B8A8_UNORM;
    }

    caps.maxImageDim = std::max<std::uint32_t>(probe.limits.maxImageDimension2D, kDirtyTileSize);

    // ---- SPIR-V acceptance ------------------------------------------------------------------
    // The one place the version number is turned into a fact, exactly as the file header allows.
    // It is NOT interchangeable with a tier flag: VK_EXT_descriptor_indexing can be present on a
    // 1.0 device, and a blob compiled for that tier at a newer target env would still fail to
    // load there -- so a lane that wants a variant blob must ask BOTH questions.
    // VK_KHR_spirv_1_4's extra step (1.1 + the extension accepts SPIR-V 1.4) is deliberately NOT
    // claimed: nothing in the tree compiles to 1.4, and reporting a version we would then have to
    // remember to enable an extension for is how a capability model starts lying.
    if (v >= VK_API_VERSION_1_3) {
        caps.spirvVersion = spirv::kVersion1_6;
    } else if (v >= VK_API_VERSION_1_2) {
        caps.spirvVersion = spirv::kVersion1_5;
    } else if (v >= VK_API_VERSION_1_1) {
        caps.spirvVersion = spirv::kVersion1_3;
    } else {
        caps.spirvVersion = spirv::kVersion1_0;
    }

    // ---- Macrotile size --------------------------------------------------------------------------
    // Dirty tracking stays at kDirtyTileSize; this is the DISPATCH granule only. A software
    // device gets a smaller one (its per-dispatch cost is CPU work, so bigger tiles do not
    // amortise the way they do on real hardware), and every device is clamped so a macrotile is
    // always representable as an image.
    std::uint32_t shift = caps.softwareDevice ? kMacrotileSoftwareShift : kMacrotileDefaultShift;
    while (shift > kMacrotileMinShift && (kDirtyTileSize << shift) > caps.maxImageDim)
        --shift;
    caps.macrotileSize = kDirtyTileSize << std::min(shift, kMacrotileMaxShift);

    caps.transferQueueFamily = probe.transferQueueFamily;
    caps.asyncComputeQueueFamily = probe.asyncComputeQueueFamily;
    return caps;
}

// ---- Lane admission ------------------------------------------------------------------------------

bool GpuCaps::fitsStorageBuffers(std::uint32_t count) const noexcept {
    return count <= limits.maxPerStageDescriptorStorageBuffers &&
           count <= limits.maxDescriptorSetStorageBuffers;
}

bool GpuCaps::fitsStorageImages(std::uint32_t count) const noexcept {
    return count <= limits.maxPerStageDescriptorStorageImages &&
           count <= limits.maxDescriptorSetStorageImages;
}

bool GpuCaps::fitsSampledImages(std::uint32_t count) const noexcept {
    return count <= limits.maxPerStageDescriptorSampledImages;
}

bool GpuCaps::fitsPushConstants(std::uint32_t bytes) const noexcept {
    return bytes <= limits.maxPushConstantsSize;
}

bool GpuCaps::fitsStorageBufferRange(VkDeviceSize bytes) const noexcept {
    return bytes <= static_cast<VkDeviceSize>(limits.maxStorageBufferRange);
}

bool GpuCaps::fitsImage(std::uint32_t w, std::uint32_t h) const noexcept {
    return w > 0 && h > 0 && w <= maxImageDim && h <= maxImageDim;
}

bool GpuCaps::fitsSpirvVersion(std::uint32_t version) const noexcept {
    return version <= spirvVersion;
}

std::uint32_t GpuCaps::maxWorkgroupsPerAxis() const noexcept {
    // Every dispatch in this renderer is 2D (x,y), so the binding constraint is the smaller of
    // the two in-plane axes.
    return std::min(limits.maxComputeWorkGroupCount[0], limits.maxComputeWorkGroupCount[1]);
}

std::uint32_t GpuCaps::workingFormatBytes() const noexcept {
    switch (workingFormat) {
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    default: return 4;
    }
}

std::string GpuCaps::tierSummary() const {
    std::string s;
    const auto add = [&s](bool on, const char* name) {
        if (!on)
            return;
        if (!s.empty())
            s += ", ";
        s += name;
    };
    add(subgroupArithmetic, "subgroup-arith");
    add(storage16Bit, "16bit-storage");
    add(shaderFloat16, "fp16-shader");
    add(timelineSemaphore, "timeline-sem");
    add(descriptorIndexing, "descriptor-indexing");
    add(bufferDeviceAddress, "buffer-address");
    add(synchronization2, "sync2");
    add(maintenance4, "maintenance4");
    add(hostImageCopy, "host-image-copy");
    add(presentWait, "present-wait");
    add(memoryBudget, "memory-budget");
    add(dedicatedAllocation, "dedicated-alloc");
    add(timestampQueries, "timestamps");
    add(portabilitySubset, "portability-subset");
    return s.empty() ? "none" : s;
}

std::string GpuCaps::summary() const {
    const char* fmt = workingFormat == VK_FORMAT_R16G16B16A16_SFLOAT   ? "rgba16f"
                      : workingFormat == VK_FORMAT_R32G32B32A32_SFLOAT ? "rgba32f"
                                                                       : "rgba8 (NO float format!)";
    std::string s = deviceName;
    s += " -- Vulkan ";
    s += std::to_string(VK_API_VERSION_MAJOR(apiVersion));
    s += '.';
    s += std::to_string(VK_API_VERSION_MINOR(apiVersion));
    if (softwareDevice)
        s += " (software)";
    s += ", working ";
    s += fmt;
    s += ", macrotile ";
    s += std::to_string(macrotileSize);
    s += "px, max image ";
    s += std::to_string(maxImageDim);
    s += "px, tiers: ";
    s += tierSummary();
    return s;
}

std::string GpuCaps::readout() const {
    // Fixed-point BY HAND. `std::to_string(float)` is specified as sprintf("%f"), which follows
    // LC_NUMERIC -- and this block exists to be pasted into bug reports, where a nanosecond figure
    // written with a decimal comma is a puzzle rather than a datum. Integer arithmetic has no
    // locale (the LC_NUMERIC round-trip class, S54).
    const auto fixed3 = [](float v) {
        const auto scaled = static_cast<std::uint64_t>(v * 1000.0f + 0.5f);
        std::string s = std::to_string(scaled / 1000) + '.';
        const std::uint64_t frac = scaled % 1000;
        if (frac < 100)
            s += '0';
        if (frac < 10)
            s += '0';
        s += std::to_string(frac);
        return s;
    };
    const auto mib = [](std::uint64_t bytes) {
        return std::to_string(bytes / (1024ull * 1024ull)) + " MiB";
    };

    std::string out;
    const auto row = [&out](const char* label, const std::string& value) {
        out += label;
        out += value;
        out += '\n';
    };

    std::string dev = deviceName;
    dev += " (";
    dev += deviceTypeName(deviceType);
    if (softwareDevice)
        dev += ", software rasterizer";
    dev += ')';
    row("Device:        ", dev);

    // The one place a version number is printed rather than asked about -- allowed, and only
    // here, exactly as the file header says.
    std::string api = std::to_string(VK_API_VERSION_MAJOR(apiVersion)) + '.' +
                      std::to_string(VK_API_VERSION_MINOR(apiVersion)) + "  (SPIR-V " +
                      std::to_string(spirvVersion >> 16) + '.' +
                      std::to_string((spirvVersion >> 8) & 0xFFu) + " accepted)";
    row("Vulkan:        ", api);

    const char* fmt = workingFormat == VK_FORMAT_R16G16B16A16_SFLOAT    ? "rgba16f"
                      : workingFormat == VK_FORMAT_R32G32B32A32_SFLOAT ? "rgba32f"
                                                                       : "rgba8 (NO float format!)";
    row("Working fmt:   ",
        std::string(fmt) + ", " + std::to_string(workingFormatBytes()) + " bytes/px");
    row("Tiles:         ", "macrotile " + std::to_string(macrotileSize) + " px, dirty tile " +
                               std::to_string(kDirtyTileSize) + " px");
    row("Max image:     ", std::to_string(maxImageDim) + " px");
    row("Storage bufs:  ",
        std::to_string(limits.maxPerStageDescriptorStorageBuffers) + " per stage / " +
            std::to_string(limits.maxDescriptorSetStorageBuffers) + " per set, range " +
            mib(limits.maxStorageBufferRange) + ", align " +
            std::to_string(limits.minStorageBufferOffsetAlignment) + " B");
    row("Storage imgs:  ", std::to_string(limits.maxPerStageDescriptorStorageImages) +
                               " per stage / " +
                               std::to_string(limits.maxDescriptorSetStorageImages) + " per set");
    row("Sampled imgs:  ",
        std::to_string(limits.maxPerStageDescriptorSampledImages) + " per stage");
    row("Push consts:   ", std::to_string(limits.maxPushConstantsSize) + " B");
    row("Compute:       ", std::to_string(maxWorkgroupsPerAxis()) + " workgroups/axis, " +
                               std::to_string(limits.maxComputeWorkGroupInvocations) +
                               " invocations, " +
                               std::to_string(limits.maxComputeSharedMemorySize) + " B shared");
    row("Timestamps:    ", timestampQueries ? std::to_string(timestampValidBits) + " valid bits, " +
                                                  fixed3(limits.timestampPeriod) + " ns/tick"
                                            : std::string("unsupported"));
    std::string queues = transferQueueFamily == UINT32_MAX
                             ? std::string("transfer=shared")
                             : "transfer=" + std::to_string(transferQueueFamily);
    queues += asyncComputeQueueFamily == UINT32_MAX
                  ? std::string(", async-compute=shared")
                  : ", async-compute=" + std::to_string(asyncComputeQueueFamily);
    row("Queues:        ", queues);
    row("Tiers:         ", tierSummary());
    return out;
}

// ---- Floor profile ---------------------------------------------------------------------------

void applyFloorProfile(GpuProbe& probe) noexcept {
    probe.instanceApiVersion = VK_API_VERSION_1_0;
    probe.deviceApiVersion = VK_API_VERSION_1_0;
    probe.limits = floorLimits();
    probe.deviceExtensions.clear(); // the swapchain extension is requested unconditionally
    probe.driverName.clear();

    probe.featStorageBuffer16BitAccess = false;
    probe.featShaderFloat16 = false;
    probe.featTimelineSemaphore = false;
    probe.featRuntimeDescriptorArray = false;
    probe.featSampledImageArrayNonUniformIndexing = false;
    probe.featDescriptorBindingPartiallyBound = false;
    probe.featDescriptorBindingVariableDescriptorCount = false;
    probe.featBufferDeviceAddress = false;
    probe.featSynchronization2 = false;
    probe.featMaintenance4 = false;
    probe.featHostImageCopy = false;

    probe.subgroupOperations = 0;
    probe.subgroupSize = 0;
    // floorLimits() already clears timestampComputeAndGraphics/timestampPeriod; clearing the
    // queue-family width too keeps the probe internally consistent, so a floor run exercises the
    // "no device timing available" path rather than a half-configured one.
    probe.timestampValidBits = 0;

    // Exactly what Vulkan 1.0 mandates, no more. The rgba32f asymmetry is deliberate and is the
    // reason the floor profile is worth running: it is the difference between "our bilinear
    // sampling works" and "our bilinear sampling works on this developer's GPU".
    probe.fmtRgba16fStorage = true;
    probe.fmtRgba16fLinearFilter = true;
    probe.fmtRgba32fStorage = true;
    probe.fmtRgba32fLinearFilter = false;

    probe.transferQueueFamily = UINT32_MAX;
    probe.asyncComputeQueueFamily = UINT32_MAX;
}

bool applyProfileFromEnv(GpuProbe& probe) {
    const char* p = std::getenv("MOSAIC_GPU_PROFILE");
    if (p == nullptr || std::strcmp(p, "floor") != 0)
        return false;
    applyFloorProfile(probe);
    return true;
}

// ---- The impure half ---------------------------------------------------------------------------

std::uint32_t instanceApiVersion() {
    // vkEnumerateInstanceVersion is itself a Vulkan 1.1 entry point: on a 1.0 loader the lookup
    // returns null and the answer is 1.0. Calling it unconditionally would be the exact mistake
    // this whole module exists to stop making.
    auto fn = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (fn == nullptr)
        return VK_API_VERSION_1_0;
    std::uint32_t version = VK_API_VERSION_1_0;
    if (fn(&version) != VK_SUCCESS)
        return VK_API_VERSION_1_0;
    return version;
}

std::uint32_t requestedApiVersion() {
    // Cap at the newest version these headers describe: asking for a version the build cannot
    // name is meaningless, and asking for less than the loader has would forbid our own tiers.
    const std::uint32_t loader = instanceApiVersion();
    return std::min<std::uint32_t>(loader, VK_API_VERSION_1_4);
}

std::vector<const char*> probeInstanceExtensions(std::uint32_t requestedVersion,
                                                 const std::vector<std::string>& available) {
    std::vector<const char*> exts;
    if (requestedVersion < VK_API_VERSION_1_1 &&
        has(available, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
        exts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    return exts;
}

GpuProbe probePhysicalDevice(VkInstance instance, VkPhysicalDevice dev,
                             std::uint32_t instanceVersion, bool instanceHasGetPhysDevProps2) {
    GpuProbe probe;
    probe.instanceApiVersion = instanceVersion;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(dev, &props);
    probe.deviceApiVersion = props.apiVersion;
    probe.deviceType = props.deviceType;
    probe.deviceName = props.deviceName;
    probe.limits = props.limits;

    std::uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> extProps(extCount);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, extProps.data());
    probe.deviceExtensions.reserve(extCount);
    for (const auto& ep : extProps)
        probe.deviceExtensions.emplace_back(ep.extensionName);

    // Formats: two questions per candidate working format, asked of optimal tiling (the layout
    // every working buffer uses).
    const auto formatOk = [dev](VkFormat f, bool& storage, bool& linear) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(dev, f, &fp);
        storage = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
        linear = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    };
    formatOk(VK_FORMAT_R16G16B16A16_SFLOAT, probe.fmtRgba16fStorage, probe.fmtRgba16fLinearFilter);
    formatOk(VK_FORMAT_R32G32B32A32_SFLOAT, probe.fmtRgba32fStorage, probe.fmtRgba32fLinearFilter);

    // Queue families: a transfer-only family (no graphics, no compute) can overlap uploads with
    // compositing; a compute-without-graphics family is the async-compute candidate. Both are
    // core 1.0 -- a HARDWARE tier, not a version tier.
    std::uint32_t famCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &famCount, nullptr);
    std::vector<VkQueueFamilyProperties> fams(famCount);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &famCount, fams.data());
    // Timestamp width is a per-family fact, so take the MINIMUM over the families we could submit
    // on rather than the first or the best: masking to fewer bits than a counter really has is
    // harmless (the subtraction still wraps correctly, just at a shorter period), masking to more
    // is a garbage reading.
    std::uint32_t tsBits = UINT32_MAX;
    for (std::uint32_t i = 0; i < famCount; ++i) {
        const VkQueueFlags f = fams[i].queueFlags;
        if (fams[i].queueCount == 0)
            continue;
        const bool gfx = (f & VK_QUEUE_GRAPHICS_BIT) != 0;
        const bool comp = (f & VK_QUEUE_COMPUTE_BIT) != 0;
        const bool xfer = (f & VK_QUEUE_TRANSFER_BIT) != 0;
        if (xfer && !gfx && !comp && probe.transferQueueFamily == UINT32_MAX)
            probe.transferQueueFamily = i;
        if (comp && !gfx && probe.asyncComputeQueueFamily == UINT32_MAX)
            probe.asyncComputeQueueFamily = i;
        if ((gfx || comp) && fams[i].timestampValidBits > 0)
            tsBits = std::min(tsBits, fams[i].timestampValidBits);
    }
    probe.timestampValidBits = tsBits == UINT32_MAX ? 0u : tsBits;

    // Features2 is core in 1.1 and an extension before it. Without EITHER route we cannot ask
    // about optional features at all, and every feature flag stays false -- the correct floor
    // answer. (`instance` is needed only to resolve the KHR entry point.)
    const bool coreFeatures2 = instanceVersion >= VK_API_VERSION_1_1;
    PFN_vkGetPhysicalDeviceFeatures2 getFeatures2 = nullptr;
    if (coreFeatures2) {
        getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
    } else if (instanceHasGetPhysDevProps2) {
        getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2KHR"));
    }

    if (getFeatures2 != nullptr) {
        // Chain only the structs whose interface this device actually exposes: querying a
        // feature struct the driver does not know is not guaranteed to be harmless.
        VkPhysicalDevice16BitStorageFeatures s16{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
        VkPhysicalDeviceShaderFloat16Int8Features f16{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
        VkPhysicalDeviceTimelineSemaphoreFeatures tls{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        VkPhysicalDeviceDescriptorIndexingFeatures di{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
        VkPhysicalDeviceBufferDeviceAddressFeatures bda{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        VkPhysicalDeviceSynchronization2Features sync2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
        VkPhysicalDeviceMaintenance4Features m4{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES};
        VkPhysicalDeviceHostImageCopyFeatures hic{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES};
        VkPhysicalDevicePresentIdFeaturesKHR pid{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
        VkPhysicalDevicePresentWaitFeaturesKHR pwait{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};

        void* head = nullptr;
        const auto link = [&head](auto& s, bool want) {
            if (!want)
                return;
            s.pNext = head;
            head = &s;
        };
        const std::uint32_t dv = std::min(instanceVersion, probe.deviceApiVersion);
        const auto& e = probe.deviceExtensions;
        link(s16, coreOrExt(dv, VK_API_VERSION_1_1, e, VK_KHR_16BIT_STORAGE_EXTENSION_NAME));
        link(f16, coreOrExt(dv, VK_API_VERSION_1_2, e, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME));
        link(tls, coreOrExt(dv, VK_API_VERSION_1_2, e, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME));
        link(di, coreOrExt(dv, VK_API_VERSION_1_2, e, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME));
        link(bda,
             coreOrExt(dv, VK_API_VERSION_1_2, e, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME));
        link(sync2, coreOrExt(dv, VK_API_VERSION_1_3, e, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME));
        link(m4, coreOrExt(dv, VK_API_VERSION_1_3, e, VK_KHR_MAINTENANCE_4_EXTENSION_NAME));
        link(hic, coreOrExt(dv, VK_API_VERSION_1_4, e, VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME));
        // Extension-only in every version -- `has`, never `coreOrExt`.
        link(pid, has(e, VK_KHR_PRESENT_ID_EXTENSION_NAME));
        link(pwait, has(e, VK_KHR_PRESENT_WAIT_EXTENSION_NAME));

        VkPhysicalDeviceFeatures2 f2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                     .pNext = head};
        getFeatures2(dev, &f2);

        probe.featStorageBuffer16BitAccess = s16.storageBuffer16BitAccess == VK_TRUE;
        probe.featShaderFloat16 = f16.shaderFloat16 == VK_TRUE;
        probe.featTimelineSemaphore = tls.timelineSemaphore == VK_TRUE;
        probe.featRuntimeDescriptorArray = di.runtimeDescriptorArray == VK_TRUE;
        probe.featSampledImageArrayNonUniformIndexing =
            di.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
        probe.featDescriptorBindingPartiallyBound = di.descriptorBindingPartiallyBound == VK_TRUE;
        probe.featDescriptorBindingVariableDescriptorCount =
            di.descriptorBindingVariableDescriptorCount == VK_TRUE;
        probe.featBufferDeviceAddress = bda.bufferDeviceAddress == VK_TRUE;
        probe.featSynchronization2 = sync2.synchronization2 == VK_TRUE;
        probe.featMaintenance4 = m4.maintenance4 == VK_TRUE;
        probe.featHostImageCopy = hic.hostImageCopy == VK_TRUE;
        probe.featPresentId = pid.presentId == VK_TRUE;
        probe.featPresentWait = pwait.presentWait == VK_TRUE;
    }

    // Subgroup properties need properties2, which shares features2's availability rule.
    if (coreFeatures2 || instanceHasGetPhysDevProps2) {
        auto getProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            vkGetInstanceProcAddr(instance, coreFeatures2 ? "vkGetPhysicalDeviceProperties2"
                                                          : "vkGetPhysicalDeviceProperties2KHR"));
        if (getProps2 != nullptr && std::min(instanceVersion, probe.deviceApiVersion) >=
                                        VK_API_VERSION_1_1) {
            VkPhysicalDeviceSubgroupProperties sub{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
            VkPhysicalDeviceProperties2 p2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                           .pNext = &sub};
            getProps2(dev, &p2);
            probe.subgroupOperations = sub.supportedOperations;
            probe.subgroupSize = sub.subgroupSize;
        }
    }

    return probe;
}

// ---- Device-creation inputs ----------------------------------------------------------------------

std::vector<const char*> deviceExtensionsFor(const GpuCaps& caps, bool needSwapchain) {
    std::vector<const char*> exts;
    if (needSwapchain)
        exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    // MoltenVK: vkCreateDevice FAILS outright unless a device advertising this enables it. Not an
    // optimisation -- a hard requirement wherever it appears.
    if (caps.portabilitySubset)
        exts.push_back(kPortabilitySubset);

    // Below 1.1 these were extensions; at/after their promotion, naming them is an error on some
    // implementations. Ask only where the interface is not already core.
    const std::uint32_t v = caps.apiVersion;
    if (caps.storage16Bit && v < VK_API_VERSION_1_1)
        exts.push_back(VK_KHR_16BIT_STORAGE_EXTENSION_NAME);
    if (caps.dedicatedAllocation && v < VK_API_VERSION_1_1) {
        exts.push_back(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
        exts.push_back(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME); // its hard dependency
    }
    if (caps.shaderFloat16 && v < VK_API_VERSION_1_2)
        exts.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
    if (caps.timelineSemaphore && v < VK_API_VERSION_1_2)
        exts.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    if (caps.descriptorIndexing && v < VK_API_VERSION_1_2)
        exts.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    if (caps.bufferDeviceAddress && v < VK_API_VERSION_1_2)
        exts.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    if (caps.synchronization2 && v < VK_API_VERSION_1_3)
        exts.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    if (caps.maintenance4 && v < VK_API_VERSION_1_3)
        exts.push_back(VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
    // Never core in any version, so always asked for by name -- and ONLY alongside a swapchain.
    // ⚠ Both extensions REQUIRE VK_KHR_swapchain; enabling them on a headless compute context
    // (every lane except the window, plus --bench and the whole unit suite) is a validation error,
    // not a harmless extra. `needSwapchain` is the only thing that distinguishes the two callers.
    if (caps.presentWait && needSwapchain) {
        exts.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
        exts.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    }
    if (caps.hostImageCopy && v < VK_API_VERSION_1_4)
        exts.push_back(VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME);
    // No promotion path: an extension for as long as it exists.
    if (caps.memoryBudget)
        exts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    return exts;
}

GpuFeatureChain::GpuFeatureChain(const GpuCaps& caps) noexcept {
    // m_coreFeatures stays all-false: the renderer has never needed a core 1.0 feature, and
    // enabling one we do not use would only narrow the set of devices that accept us.
    const auto link = [this](auto& s, VkStructureType type, bool want) {
        if (!want)
            return;
        s.sType = type;
        s.pNext = m_head;
        m_head = &s;
    };
    link(m_storage16, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, caps.storage16Bit);
    if (caps.storage16Bit)
        m_storage16.storageBuffer16BitAccess = VK_TRUE;

    link(m_float16, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,
         caps.shaderFloat16);
    if (caps.shaderFloat16)
        m_float16.shaderFloat16 = VK_TRUE;

    link(m_timeline, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
         caps.timelineSemaphore);
    if (caps.timelineSemaphore)
        m_timeline.timelineSemaphore = VK_TRUE;

    link(m_descriptorIndexing, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
         caps.descriptorIndexing);
    if (caps.descriptorIndexing) {
        // Exactly the four `decide()` demanded -- asking for more would risk a create failure for
        // capability we never use.
        m_descriptorIndexing.runtimeDescriptorArray = VK_TRUE;
        m_descriptorIndexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        m_descriptorIndexing.descriptorBindingPartiallyBound = VK_TRUE;
        m_descriptorIndexing.descriptorBindingVariableDescriptorCount = VK_TRUE;
    }

    link(m_bufferAddress, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
         caps.bufferDeviceAddress);
    if (caps.bufferDeviceAddress)
        m_bufferAddress.bufferDeviceAddress = VK_TRUE;

    link(m_sync2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
         caps.synchronization2);
    if (caps.synchronization2)
        m_sync2.synchronization2 = VK_TRUE;

    link(m_maintenance4, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES,
         caps.maintenance4);
    if (caps.maintenance4)
        m_maintenance4.maintenance4 = VK_TRUE;

    link(m_hostImageCopy, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES,
         caps.hostImageCopy);
    if (caps.hostImageCopy)
        m_hostImageCopy.hostImageCopy = VK_TRUE;

    link(m_presentId, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, caps.presentWait);
    if (caps.presentWait)
        m_presentId.presentId = VK_TRUE;
    link(m_presentWait, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
         caps.presentWait);
    if (caps.presentWait)
        m_presentWait.presentWait = VK_TRUE;
}

// ---- Device enumeration and selection (S60-f) --------------------------------------------------

namespace {

[[nodiscard]] std::string lowered(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// An all-digit selector is an INDEX. Nothing else is: no real device name is all digits, and the
// alternative -- a flag whose meaning depends on what the driver happened to call the card -- is
// the kind of ambiguity that only shows up in someone else's bug report.
[[nodiscard]] bool allDigits(std::string_view s) noexcept {
    if (s.empty())
        return false;
    for (const char c : s) {
        if (c < '0' || c > '9')
            return false;
    }
    return true;
}

// The process-wide selector. ⚠ SEEDED FROM THE ENVIRONMENT rather than default-constructed, for
// the same reason gpu_policy.hpp's storage is: `main()` is the APP's entry point and the test
// binary has doctest's, so a variable only main() could set would be inert in exactly the process
// that most wants to aim a GPU test at a second device. main()'s later setDeviceSelector() still
// wins, because a flag outranks the environment either way.
[[nodiscard]] DeviceSelector& deviceSelectorStorage() {
    static DeviceSelector selector = deviceSelectorFromEnv();
    return selector;
}

}  // namespace

const char* deviceTypeName(VkPhysicalDeviceType type) noexcept {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}

int deviceTypeRank(VkPhysicalDeviceType type) noexcept {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 0;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 1;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 4;
    default: return 3;  // OTHER: unknown, which is not the same as known-to-be-software
    }
}

std::string formatDriverVersion(std::uint32_t vendorId, std::uint32_t version) {
    if (vendorId == 0x10DEu) {  // NVIDIA: 10.8.8.6 bits, and it is what their package says
        return std::to_string((version >> 22) & 0x3FFu) + '.' +
               std::to_string((version >> 14) & 0xFFu) + '.' +
               std::to_string((version >> 6) & 0xFFu) + '.' + std::to_string(version & 0x3Fu);
    }
#ifdef _WIN32
    if (vendorId == 0x8086u)  // Intel, and only on Windows -- on Linux Mesa uses the Vulkan layout
        return std::to_string(version >> 14) + '.' + std::to_string(version & 0x3FFFu);
#endif
    return std::to_string(VK_API_VERSION_MAJOR(version)) + '.' +
           std::to_string(VK_API_VERSION_MINOR(version)) + '.' +
           std::to_string(VK_API_VERSION_PATCH(version));
}

std::string describeDevice(const DeviceInfo& info) {
    std::string s = "[" + std::to_string(info.index) + "] " + info.name + " -- ";
    s += deviceTypeName(info.type);
    s += ", Vulkan " + std::to_string(VK_API_VERSION_MAJOR(info.apiVersion)) + '.' +
         std::to_string(VK_API_VERSION_MINOR(info.apiVersion)) + '.' +
         std::to_string(VK_API_VERSION_PATCH(info.apiVersion));
    s += ", driver " + formatDriverVersion(info.vendorId, info.driverVersion);
    if (!info.driverName.empty())
        s += " (" + info.driverName + ')';
    s += ", " + std::to_string(info.deviceLocalBytes / (1024ull * 1024ull)) + " MiB device-local";
    if (!info.usable) {
        s += "  [unusable";
        if (!info.unusableReason.empty())
            s += ": " + info.unusableReason;
        s += ']';
    }
    return s;
}

DeviceChoice pickPhysicalDevice(const std::vector<DeviceInfo>& devices,
                                const DeviceSelector& selector) {
    DeviceChoice choice;
    if (devices.empty()) {
        choice.reason = "no Vulkan physical devices were enumerated";
        return choice;
    }

    const auto at = [&devices](int i) -> const DeviceInfo& {
        return devices[static_cast<std::size_t>(i)];
    };

    // The automatic pick is computed FIRST and unconditionally: it is both the default answer and
    // the place an unmatched selector lands, and computing it once makes those the same device by
    // construction rather than by two code paths agreeing with each other.
    int automatic = -1;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (!devices[i].usable)
            continue;
        if (automatic < 0) {
            automatic = static_cast<int>(i);
            continue;
        }
        const int rank = deviceTypeRank(devices[i].type);
        const int bestRank = deviceTypeRank(at(automatic).type);
        // Pinned tie-break: within a type rank, the lowest enumeration index wins.
        if (rank < bestRank || (rank == bestRank && devices[i].index < at(automatic).index))
            automatic = static_cast<int>(i);
    }

    if (!selector.empty()) {
        const std::string source = selector.fromEnvironment ? "MOSAIC_DEVICE" : "--device";
        std::vector<int> matches;
        std::string how;
        if (allDigits(selector.text)) {
            const std::uint64_t wanted = std::strtoull(selector.text.c_str(), nullptr, 10);
            for (std::size_t i = 0; i < devices.size(); ++i) {
                if (devices[i].index == wanted)
                    matches.push_back(static_cast<int>(i));
            }
            how = "index " + selector.text;
        } else {
            const std::string needle = lowered(selector.text);
            // Exact equality first: a device whose whole name is contained in another's must stay
            // nameable, and the user who typed the full name meant that device.
            for (std::size_t i = 0; i < devices.size(); ++i) {
                if (lowered(devices[i].name) == needle)
                    matches.push_back(static_cast<int>(i));
            }
            if (matches.empty()) {
                for (std::size_t i = 0; i < devices.size(); ++i) {
                    if (lowered(devices[i].name).find(needle) != std::string::npos)
                        matches.push_back(static_cast<int>(i));
                }
            }
            how = "name \"" + selector.text + '"';
        }

        int picked = -1;
        for (const int m : matches) {
            if (at(m).usable && (picked < 0 || at(m).index < at(picked).index))
                picked = m;  // pinned tie-break, again: the lowest enumeration index wins
        }
        if (picked >= 0) {
            choice.index = picked;
            choice.reason = source + " selected by " + how;
            if (matches.size() > 1)
                choice.reason += " (" + std::to_string(matches.size()) +
                                 " devices matched; the lowest index wins)";
            return choice;
        }

        // NEVER fatal and never silent. A selector we cannot honour is a typo or a machine that
        // changed, and the right answer to both is the automatic pick plus a sentence saying so.
        choice.warning = source + " asked for " + how;
        choice.warning += matches.empty() ? ", which matched no enumerated device"
                                          : ", which matched only unusable device(s)";
        choice.warning += " -- falling back to the automatic pick";
    }

    if (automatic < 0) {
        choice.reason = "no enumerated device was usable";
        return choice;
    }
    choice.index = automatic;
    if (at(automatic).type == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        // Accepted in full (settled decision section 10.2) -- and stated, because it is still the
        // last thing we would have chosen.
        choice.reason = "automatic: no hardware device was usable, so a software rasterizer was "
                        "accepted";
    } else {
        choice.reason = std::string("automatic: best available device type (") +
                        deviceTypeName(at(automatic).type) + ')';
    }
    return choice;
}

DeviceSelector decideDeviceSelector(std::string_view flag, std::string_view env) {
    if (!flag.empty())
        return DeviceSelector{std::string(flag), /*fromEnvironment=*/false};
    if (!env.empty())
        return DeviceSelector{std::string(env), /*fromEnvironment=*/true};
    return DeviceSelector{};
}

DeviceSelector deviceSelectorFromEnv() {
    const char* v = std::getenv("MOSAIC_DEVICE");
    if (v == nullptr || *v == '\0')
        return DeviceSelector{};
    return DeviceSelector{std::string(v), /*fromEnvironment=*/true};
}

const DeviceSelector& deviceSelector() noexcept { return deviceSelectorStorage(); }

void setDeviceSelector(DeviceSelector selector) { deviceSelectorStorage() = std::move(selector); }

DeviceInfo describePhysicalDevice(VkInstance instance, VkPhysicalDevice dev, std::uint32_t index,
                                  std::uint32_t instanceApiVersion,
                                  bool instanceHasGetPhysDevProps2) {
    DeviceInfo info;
    info.index = index;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(dev, &props);
    info.name = props.deviceName;
    info.type = props.deviceType;
    info.apiVersion = props.apiVersion;
    info.driverVersion = props.driverVersion;
    info.vendorId = props.vendorID;

    // "How much VRAM": the sum of the device-local heaps. On a discrete part that is one heap; on
    // an integrated one it is the shared pool, which is exactly the number that makes a hybrid
    // laptop's two entries tell themselves apart at a glance.
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(dev, &mem);
    for (std::uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
        if ((mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
            info.deviceLocalBytes += mem.memoryHeaps[i].size;
    }

    // The driver's own name is the other half of telling two devices apart, and it needs
    // VK_KHR_driver_properties (core in 1.2). Absent, the field stays empty -- ordinary, not an
    // error, and the version/vendor decode above still identifies the part.
    const bool coreProps2 = instanceApiVersion >= VK_API_VERSION_1_1;
    if (coreProps2 || instanceHasGetPhysDevProps2) {
        bool hasDriverProps = std::min(instanceApiVersion, props.apiVersion) >= VK_API_VERSION_1_2;
        if (!hasDriverProps) {
            std::uint32_t extCount = 0;
            vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
            std::vector<VkExtensionProperties> extProps(extCount);
            vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, extProps.data());
            for (const auto& ep : extProps) {
                if (std::strcmp(ep.extensionName, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME) == 0) {
                    hasDriverProps = true;
                    break;
                }
            }
        }
        auto getProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            vkGetInstanceProcAddr(instance, coreProps2 ? "vkGetPhysicalDeviceProperties2"
                                                       : "vkGetPhysicalDeviceProperties2KHR"));
        if (getProps2 != nullptr && hasDriverProps) {
            VkPhysicalDeviceDriverProperties driver{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDeviceProperties2 p2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                           .pNext = &driver};
            getProps2(dev, &p2);
            info.driverName = driver.driverName;
        }
    }
    return info;
}

void logDeviceSelection(std::string_view who, const std::vector<DeviceInfo>& devices,
                        const DeviceChoice& choice) {
    // ONCE per consumer per process -- see the header. Guarded the same way computeLaneAllowed's
    // announcement is (gpu_policy.hpp), because the failure mode is the same: a line that repeats
    // per lane, per composite or per test case is a line nobody reads.
    {
        static std::mutex mutex;
        static std::set<std::string, std::less<>> announced;
        const std::lock_guard<std::mutex> lock(mutex);
        if (!announced.emplace(who).second)
            return;
    }

    const auto log = common::log::category("render");
    log->info("gpu[{}]: {} Vulkan device(s) enumerated", who, devices.size());
    for (const auto& d : devices)
        log->info("gpu[{}]:   {}", who, describeDevice(d));
    if (!choice.warning.empty())
        log->warn("gpu[{}]: {}", who, choice.warning);
    if (choice.index < 0 || static_cast<std::size_t>(choice.index) >= devices.size()) {
        log->warn("gpu[{}]: no device chosen -- {}", who, choice.reason);
        return;
    }
    const DeviceInfo& d = devices[static_cast<std::size_t>(choice.index)];
    if (d.type == VK_PHYSICAL_DEVICE_TYPE_CPU)
        log->warn("gpu[{}]: chose [{}] {} -- {}", who, d.index, d.name, choice.reason);
    else
        log->info("gpu[{}]: chose [{}] {} -- {}", who, d.index, d.name, choice.reason);
}

}  // namespace mosaic::render
