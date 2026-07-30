#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

// GPU capability model (S60-alpha, docs/s60-performance-plan.md section 2).
//
// Mosaic targets **Vulkan 1.0** as its floor and tiers optimisations on from there. The rule that
// makes that work is stated once, here, and must not be broken anywhere else in render/:
//
//     NOTHING BRANCHES ON THE VULKAN VERSION NUMBER. Code branches on capability flags only.
//
// A driver that advertises 1.3 but gates an individual feature (common on mobile and older Mesa)
// is then a non-event: we never asked "are you 1.3?", we asked "can you index descriptors?".
// Version numbers appear in exactly two places -- the probe that derives the flags, and the log
// line that reports what we found.
//
// The split below exists so the decision is unit-testable without a GPU:
//   * `GpuProbe`  -- plain data, everything we read off a physical device. Synthesizable.
//   * `decide()`  -- a PURE function GpuProbe -> GpuCaps. This is what the tests exercise.
//   * `probePhysicalDevice()` -- the impure half that fills a GpuProbe from a real device.
//
// `applyFloorProfile()` clamps a probe to Vulkan 1.0's guaranteed minimums, so
// `MOSAIC_GPU_PROFILE=floor` runs the REAL decision path against the low end on any dev machine.
// That is how the oldest supported hardware gets tested without owning any of it.

namespace mosaic::render {

// ---- Vulkan 1.0's guaranteed minimums (spec, "Required Limits") ---------------------------
//
// Only the ones this renderer can actually collide with. These are what a conforming 1.0
// implementation is permitted to report -- not what any real desktop driver reports.
namespace vk10 {
inline constexpr std::uint32_t kMaxImageDimension2D = 4096;
inline constexpr std::uint32_t kMaxComputeWorkGroupInvocations = 128;
inline constexpr std::uint32_t kMaxComputeWorkGroupCount = 65535;
inline constexpr std::uint32_t kMaxComputeSharedMemorySize = 16384;
inline constexpr std::uint32_t kMaxPushConstantsSize = 128;
inline constexpr std::uint32_t kMaxPerStageDescriptorStorageBuffers = 4;
inline constexpr std::uint32_t kMaxPerStageDescriptorStorageImages = 4;
inline constexpr std::uint32_t kMaxPerStageDescriptorSampledImages = 16;
inline constexpr std::uint32_t kMaxDescriptorSetStorageBuffers = 4;
inline constexpr std::uint32_t kMaxDescriptorSetStorageImages = 4;
inline constexpr std::uint32_t kMaxBoundDescriptorSets = 4;
inline constexpr std::uint32_t kMaxMemoryAllocationCount = 4096;
inline constexpr std::uint64_t kMaxStorageBufferRange = 134217728ull; // 128 MiB
// The largest value a conforming implementation may report -- so padding a storage-buffer
// descriptor offset to this is always legal, whatever the device says (and whatever
// `floorLimits()` says, which is the same number for the same reason).
inline constexpr std::uint64_t kMaxStorageBufferOffsetAlignment = 256ull;
}  // namespace vk10

// ---- SPIR-V versions, as the SPIR-V header encodes them ---------------------------------------
//
// A tier VARIANT of a shader may need a newer SPIR-V than the floor set emits, and "which SPIR-V
// will this device load" is a real capability rather than a preference: a Vulkan 1.0 driver
// rejects SPIR-V 1.3 outright, so a variant blob compiled at a newer target env is UNLOADABLE
// there even when the feature it uses is present via an extension. Modelled as a capability
// (`GpuCaps::spirvVersion` + `fitsSpirvVersion`) so callers ask "will you take this blob?" rather
// than "are you 1.1?" -- the version arithmetic happens once, inside `decide()`.
namespace spirv {
inline constexpr std::uint32_t kVersion1_0 = 0x00010000u; // Vulkan 1.0
inline constexpr std::uint32_t kVersion1_3 = 0x00010300u; // Vulkan 1.1
inline constexpr std::uint32_t kVersion1_4 = 0x00010400u; // 1.1 + VK_KHR_spirv_1_4; not claimed
inline constexpr std::uint32_t kVersion1_5 = 0x00010500u; // Vulkan 1.2
inline constexpr std::uint32_t kVersion1_6 = 0x00010600u; // Vulkan 1.3
}  // namespace spirv

// ---- Tiling constants (docs/s60-performance-plan.md section 3) ------------------------------
//
// DIRTY-TRACKING granularity is the `.mosaic` store's tile size (io/mosaic/docio.hpp kTileSize),
// so one dirty set feeds both the recomposite and the autosave journal, and an in-memory tile
// maps 1:1 onto a stored tile with no re-tiling on save. GPU DISPATCH granularity is a probed
// multiple of it (a "macrotile"), because per-dispatch cost is roughly constant while a 64px
// tile is only ~32 KB of work -- at 64px the fixed overhead dominates, and worst on the weak
// parts we target. Settled with the user 2026-07-23.
inline constexpr std::uint32_t kDirtyTileSize = 64; // MUST equal io::mosaic::kTileSize
inline constexpr std::uint32_t kMacrotileMinShift = 0; // 64px  -- reachable if measurement wants it
inline constexpr std::uint32_t kMacrotileMaxShift = 4; // 1024px
inline constexpr std::uint32_t kMacrotileDefaultShift = 2; // 256px, the default k
inline constexpr std::uint32_t kMacrotileSoftwareShift = 1; // 128px on a software device

// ---- What we read off a device ---------------------------------------------------------------

// Every fact `decide()` needs, gathered in one plain struct so a test can synthesize a device
// that does not exist. Defaults describe a MINIMAL conforming Vulkan 1.0 device with nothing
// optional: that way a test writes only the fields it cares about, and a field this renderer
// forgets to probe fails closed rather than open.
struct GpuProbe {
    std::uint32_t instanceApiVersion = VK_API_VERSION_1_0;
    std::uint32_t deviceApiVersion = VK_API_VERSION_1_0;
    VkPhysicalDeviceType deviceType = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    std::string deviceName = "(unknown)";
    std::string driverName; // best-effort, 1.2+/VK_KHR_driver_properties only

    VkPhysicalDeviceLimits limits{}; // zero-initialised; floorLimits() fills a legal 1.0 set

    // Device extensions the driver reports. Names, not flags, so the probe stays a faithful
    // record of what the device said and `decide()` owns every interpretation.
    std::vector<std::string> deviceExtensions;

    // Feature bits. All false unless a features2 query succeeded (which itself needs either a
    // 1.1 instance or VK_KHR_get_physical_device_properties2 -- see probePhysicalDevice).
    bool featStorageBuffer16BitAccess = false;
    bool featShaderFloat16 = false;
    bool featTimelineSemaphore = false;
    bool featRuntimeDescriptorArray = false;
    bool featSampledImageArrayNonUniformIndexing = false;
    bool featDescriptorBindingPartiallyBound = false;
    bool featDescriptorBindingVariableDescriptorCount = false;
    bool featBufferDeviceAddress = false;
    bool featSynchronization2 = false;
    bool featMaintenance4 = false;
    bool featHostImageCopy = false;
    bool featPresentId = false;
    bool featPresentWait = false;

    // Subgroup properties (1.1+). `subgroupSize` 0 means "not reported".
    VkSubgroupFeatureFlags subgroupOperations = 0;
    std::uint32_t subgroupSize = 0;

    // Meaningful low bits of a timestamp counter, taken from the QUEUE FAMILY rather than from
    // `limits` -- `VkQueueFamilyProperties::timestampValidBits` is where Vulkan puts it, and a
    // family reporting 0 cannot execute vkCmdWriteTimestamp at all. Probed as the minimum over
    // the graphics/compute families, because that is the set we might submit on. Vulkan 1.0
    // guarantees >= 36 here whenever `limits.timestampComputeAndGraphics` is set, so the two are
    // probed together and gated together (see decide()).
    std::uint32_t timestampValidBits = 0;

    // Format support, already filtered to the two questions we ask of a working buffer.
    bool fmtRgba16fStorage = false;
    bool fmtRgba16fLinearFilter = false;
    bool fmtRgba32fStorage = false;
    bool fmtRgba32fLinearFilter = false;

    // Queue families distinct from the main graphics/compute one, or UINT32_MAX for none.
    std::uint32_t transferQueueFamily = UINT32_MAX;
    std::uint32_t asyncComputeQueueFamily = UINT32_MAX;
};

// A legal, minimal Vulkan 1.0 limit set (every field at its guaranteed minimum). The baseline for
// synthetic probes in tests and the clamp target for `applyFloorProfile`.
[[nodiscard]] VkPhysicalDeviceLimits floorLimits() noexcept;

// ---- The decision ----------------------------------------------------------------------------

// What the renderer is allowed to do on this device. Every field is a capability or a policy
// derived from one -- never a version test.
struct GpuCaps {
    // Reporting only. Do NOT branch on this; branch on the flags below.
    std::uint32_t apiVersion = VK_API_VERSION_1_0; // min(instance, device) -- what we may use
    std::string deviceName = "(none)";
    VkPhysicalDeviceType deviceType = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    bool softwareDevice = false; // lavapipe/SwiftShader: conservative policy + a user notice

    VkPhysicalDeviceLimits limits{};

    // ---- Tier flags: "this optimisation is available here" -----------------------------------
    bool subgroupArithmetic = false;  // 1.1: reductions (histogram, importance map, blur sums)
    bool storage16Bit = false;        // 1.1: rgba16f staging -- halves upload bandwidth
    bool shaderFloat16 = false;       // 1.2: half-rate -> full-rate blend on many parts
    bool timelineSemaphore = false;   // 1.2: overlap transfer + compute without fence churn
    // VK_KHR_present_id + VK_KHR_present_wait, together (present_wait is useless without an id to
    // wait on, so the two are probed and reported as ONE capability). This is how the frame loop
    // learns the display's cadence from the presentation engine instead of from a wall clock: pace
    // the next frame on the previous one actually reaching the screen. It needs no refresh-rate
    // query, and it follows the window across monitors of different rates for free -- the thing a
    // rate query cannot do, because Wayland will never tell an application where its window is.
    bool presentWait = false;
    bool descriptorIndexing = false;  // 1.2: ONE dispatch per layer instead of one per tile
    bool bufferDeviceAddress = false; // 1.2: bindless tile atlas
    bool synchronization2 = false;    // 1.3: finer-grained barriers on tile-heavy submissions
    bool maintenance4 = false;        // 1.3: maxBufferSize
    bool hostImageCopy = false;       // 1.4: CPU -> optimally-tiled image with NO staging buffer
    bool memoryBudget = false;        // any: the only portable VRAM headroom query (eviction)
    bool dedicatedAllocation = false; // 1.1: VMA stops over-suballocating large images
    bool timestampQueries = false;    // 1.0: real device time in the profiler, not submit time
    bool portabilitySubset = false;   // MoltenVK: vkCreateDevice FAILS unless we enable it

    // ---- Derived policy ----------------------------------------------------------------------

    // The float working format. rgba16f by default even where rgba32f is offered: it is the only
    // float format guaranteed for BOTH storage and linear filtering, at half the memory and half
    // the upload bandwidth, and ~11 bits of mantissa clears the 1/255 parity tolerance easily.
    // Settled with the user 2026-07-23. R8G8B8A8_UNORM is the desperate floor and is logged.
    VkFormat workingFormat = VK_FORMAT_R8G8B8A8_UNORM;

    std::uint32_t macrotileSize = kDirtyTileSize << kMacrotileDefaultShift; // GPU dispatch granule
    std::uint32_t maxImageDim = vk10::kMaxImageDimension2D;

    // The newest SPIR-V this device will accept in a VkShaderModule. Reporting only in the sense
    // that nothing branches on it directly -- ask `fitsSpirvVersion` instead. See the `spirv`
    // namespace for why a tier flag alone is not enough to load a tier shader.
    std::uint32_t spirvVersion = spirv::kVersion1_0;

    // The mask `render::GpuTimer` applies to a raw timestamp before subtracting one from another.
    // Zero exactly when `timestampQueries` is false, so a caller cannot pick up a stray width for
    // a device that has no counter to read.
    std::uint32_t timestampValidBits = 0;

    std::uint32_t transferQueueFamily = UINT32_MAX;     // UINT32_MAX = share the main queue
    std::uint32_t asyncComputeQueueFamily = UINT32_MAX;

    // ---- Lane admission ----------------------------------------------------------------------
    //
    // A GPU lane that does not fit refuses ITSELF and hands the work to its CPU lane. It must
    // never assume; it must ask. (Live example: extrude_raster.comp binds 7 storage buffers
    // against a 1.0 guaranteed floor of 4.)

    [[nodiscard]] bool fitsStorageBuffers(std::uint32_t count) const noexcept;
    [[nodiscard]] bool fitsStorageImages(std::uint32_t count) const noexcept;
    [[nodiscard]] bool fitsSampledImages(std::uint32_t count) const noexcept;
    [[nodiscard]] bool fitsPushConstants(std::uint32_t bytes) const noexcept;
    // One storage buffer of this size is bindable. Vulkan 1.0 guarantees only 128 MiB of
    // maxStorageBufferRange -- well under the per-lane byte caps the buffer-based lanes chose
    // for themselves, so those are POLICY limits and this is the hardware one.
    [[nodiscard]] bool fitsStorageBufferRange(VkDeviceSize bytes) const noexcept;
    // A single VkImage of this size is representable. False => the caller must tile.
    [[nodiscard]] bool fitsImage(std::uint32_t w, std::uint32_t h) const noexcept;
    // A shader blob compiled to `version` (a `spirv::kVersion*`) is loadable here. False is an
    // ordinary outcome: the caller takes the floor blob, which every device accepts.
    [[nodiscard]] bool fitsSpirvVersion(std::uint32_t version) const noexcept;
    // Largest dispatch grid, in workgroups, that this device will accept on one axis.
    [[nodiscard]] std::uint32_t maxWorkgroupsPerAxis() const noexcept;

    // Bytes per pixel of `workingFormat` -- for the memory budget and for staging sizing.
    [[nodiscard]] std::uint32_t workingFormatBytes() const noexcept;

    // One-line summary for the startup log; also the body of the Settings capability readout.
    [[nodiscard]] std::string summary() const;
    // The tier flags that fired, comma-separated ("none" if the device is at the bare floor).
    [[nodiscard]] std::string tierSummary() const;

    // The LONG form (S60-f): one labelled line per negotiated fact -- device, version, working
    // format, tiling, the descriptor/push-constant/compute limits the lane-admission helpers
    // above answer from, timestamp support, queues, tiers. Trailing newline on every line, so it
    // drops straight into a log, a --version-style dump or a diagnostics pane without reflowing.
    //
    // This is the block a bug report should carry: `summary()` says which device, this says why a
    // lane refused itself on it. No Vulkan calls -- everything here was decided in `decide()`.
    [[nodiscard]] std::string readout() const;
};

// The pure decision. No Vulkan calls, no globals, no logging -- this is the unit-tested core.
[[nodiscard]] GpuCaps decide(const GpuProbe& probe) noexcept;

// Clamp a probe to Vulkan 1.0's guaranteed minimums: version 1.0, floorLimits(), no device
// extensions beyond the ones the app cannot run without, no optional features, no distinct
// queues, and only the format support 1.0 guarantees (rgba16f storage+linear, rgba32f storage
// but NOT linear filtering -- the asymmetry that drives the workingFormat choice).
//
// `MOSAIC_GPU_PROFILE=floor` applies this after probing a real device, so the low end is exercised
// through the real code path on whatever machine is to hand.
void applyFloorProfile(GpuProbe& probe) noexcept;

// Reads MOSAIC_GPU_PROFILE. "floor" => applyFloorProfile; anything else (or unset) => unchanged.
// Returns true if a profile was applied, so the caller can say so in the log.
bool applyProfileFromEnv(GpuProbe& probe);

// ---- The impure half -------------------------------------------------------------------------

// The instance version the loader supports. `vkEnumerateInstanceVersion` is itself a 1.1 entry
// point, so a 1.0 loader returns null for it -- in which case the answer is 1.0. This is the
// first place the "assume nothing" rule bites, and it is why we cannot simply ask for 1.2.
[[nodiscard]] std::uint32_t instanceApiVersion();

// The value to put in VkApplicationInfo::apiVersion.
//
// Subtle and worth stating: this field is the MAXIMUM version the application may use, not a
// minimum requirement. Hard-coding 1.0 would make us portable and then forbid every tier on a
// capable machine (a 1.0 instance cannot call a 1.1 entry point); hard-coding 1.2 -- what Mosaic
// did until S60-alpha -- fails outright on a 1.0 loader. So: ask for exactly what the loader
// reports, capped at the newest version this build's headers know. The FLOOR is enforced by never
// *depending* on anything above 1.0, not by lying to the loader about what we can use.
[[nodiscard]] std::uint32_t requestedApiVersion();

// Instance extensions worth requesting for capability probing. Today that is just
// VK_KHR_get_physical_device_properties2, and only below 1.1 where it is not yet core -- without
// it (or a 1.1 instance) `probePhysicalDevice` cannot ask about optional features at all, and
// every tier flag correctly stays false. Only names present in `available` are returned.
[[nodiscard]] std::vector<const char*> probeInstanceExtensions(
    std::uint32_t requestedVersion, const std::vector<std::string>& available);

// Fill a GpuProbe from a real device. `instanceApiVersion` and `instanceHasGetPhysDevProps2` say
// whether a features2 query is even legal here; without it every optional-feature flag stays
// false, which is the correct floor answer rather than a guess.
[[nodiscard]] GpuProbe probePhysicalDevice(VkInstance instance, VkPhysicalDevice dev,
                                           std::uint32_t instanceApiVersion,
                                           bool instanceHasGetPhysDevProps2);

// ---- Turning caps back into device-creation inputs --------------------------------------------

// The device extensions to request for these caps. Every name returned was seen in the probe, so
// `vkCreateDevice` cannot fail for asking. (Requesting an unsupported extension is an error --
// which is exactly why the probe and the request must be the same function's two halves.)
[[nodiscard]] std::vector<const char*> deviceExtensionsFor(const GpuCaps& caps,
                                                           bool needSwapchain);

// The pNext chain of feature structs for `vkCreateDevice`, matching `deviceExtensionsFor`.
//
// NON-COPYABLE AND NON-MOVABLE ON PURPOSE: the chain stores pointers INTO ITSELF, so any copy or
// move would leave dangling pNext links pointing at the old object. Build it where you call
// vkCreateDevice and let it die there.
class GpuFeatureChain {
public:
    explicit GpuFeatureChain(const GpuCaps& caps) noexcept;

    GpuFeatureChain(const GpuFeatureChain&) = delete;
    GpuFeatureChain& operator=(const GpuFeatureChain&) = delete;
    GpuFeatureChain(GpuFeatureChain&&) = delete;
    GpuFeatureChain& operator=(GpuFeatureChain&&) = delete;

    // Head of the chain for VkDeviceCreateInfo::pNext, or nullptr when no feature is requested.
    [[nodiscard]] const void* pNext() const noexcept { return m_head; }
    // Core 1.0 features for VkDeviceCreateInfo::pEnabledFeatures. Mosaic enables NONE of them --
    // the renderer has never needed one -- so this is an all-false struct, stated explicitly
    // rather than left to a null pointer, because "we checked" and "we forgot" should not look
    // the same in the code.
    [[nodiscard]] const VkPhysicalDeviceFeatures& coreFeatures() const noexcept {
        return m_coreFeatures;
    }

private:
    void* m_head = nullptr;
    VkPhysicalDeviceFeatures m_coreFeatures{};

    VkPhysicalDevice16BitStorageFeatures m_storage16{};
    VkPhysicalDeviceShaderFloat16Int8Features m_float16{};
    VkPhysicalDeviceTimelineSemaphoreFeatures m_timeline{};
    VkPhysicalDeviceDescriptorIndexingFeatures m_descriptorIndexing{};
    VkPhysicalDeviceBufferDeviceAddressFeatures m_bufferAddress{};
    VkPhysicalDeviceSynchronization2Features m_sync2{};
    VkPhysicalDeviceMaintenance4Features m_maintenance4{};
    VkPhysicalDeviceHostImageCopyFeatures m_hostImageCopy{};
    VkPhysicalDevicePresentIdFeaturesKHR m_presentId{};
    VkPhysicalDevicePresentWaitFeaturesKHR m_presentWait{};
};

// ---- WHICH device? (S60-f, docs/s60-performance-plan.md section 9) ------------------------------
//
// Two problems, one section, and the first one is the reason the second matters. A hybrid laptop
// enumerates two GPUs and Mosaic used to take the first discrete-looking one it saw with a
// graphics queue -- with NO record of what else was on offer. When that pick was wrong the symptom
// was "Mosaic is slow", which is indistinguishable from a dozen other causes. So: enumerate into
// plain data, log the WHOLE list plus the choice and the reason for it, and let the user say
// otherwise with `--device` / `MOSAIC_DEVICE`.
//
// The split is deliberately the same one `GpuProbe`/`decide()` uses, for the same reason: the
// policy becomes a pure function over fabricated device lists (tests/test_device_select.cpp), and
// no test needs a Vulkan instance to pin it.

// One enumerated physical device, reduced to what a HUMAN needs in a bug report and what the
// picker needs to rank it. No VkPhysicalDevice handle -- that is what makes it synthesizable.
struct DeviceInfo {
    // Position in `vkEnumeratePhysicalDevices`' order: the number the readout prints and the
    // number `--device N` names. Carried as a field rather than inferred from the vector position
    // so the printed number and the matched number cannot drift apart.
    std::uint32_t index = 0;
    std::string name = "(unknown)";
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    std::uint32_t apiVersion = VK_API_VERSION_1_0;
    std::uint32_t driverVersion = 0;
    std::uint32_t vendorId = 0;
    std::string driverName;             // best-effort: VK_KHR_driver_properties / 1.2+ only
    std::uint64_t deviceLocalBytes = 0; // sum of the DEVICE_LOCAL heaps -- "how much VRAM"

    // Filled by the CALLER, because "usable" is a different question for each of them: the compute
    // context needs a graphics queue family, the window needs one that also presents to ITS surface
    // plus VK_KHR_swapchain. The picker never returns an unusable device -- not even when the
    // selector names one; it warns and falls back instead.
    bool usable = true;
    std::string unusableReason;
};

// What the user asked for, or nothing. "Said nothing" is representable on purpose, exactly as
// `GpuUseOverride::None` is in gpu_policy.hpp -- the distinction stops mattering only when there
// is a single source of opinion, and there are two here.
struct DeviceSelector {
    std::string text;             // an index ("1") or a case-insensitive name substring ("nvidia")
    bool fromEnvironment = false; // reporting only: which source `decideDeviceSelector` took

    [[nodiscard]] bool empty() const noexcept { return text.empty(); }
};

// The outcome of a pick.
struct DeviceChoice {
    // Position IN THE VECTOR handed to `pickPhysicalDevice`, so the caller can index straight back
    // into its parallel VkPhysicalDevice array. -1 means nothing was usable.
    int index = -1;
    std::string reason;  // why this one -- always set, including when index < 0
    std::string warning; // non-empty exactly when the user asked for something we could not honour
};

// "discrete" / "integrated" / "virtual" / "cpu" / "other", for logs and the readout.
[[nodiscard]] const char* deviceTypeName(VkPhysicalDeviceType type) noexcept;

// Rank for the automatic pick, LOWER IS BETTER: discrete < integrated < virtual < other < cpu.
// `other` sits above `cpu` because an unknown device type is merely unknown, whereas a CPU type is
// a software rasterizer by definition -- accepted in full (settled decision section 10.2), and
// last.
[[nodiscard]] int deviceTypeRank(VkPhysicalDeviceType type) noexcept;

// `VkPhysicalDeviceProperties::driverVersion` as text. The field has no portable layout: NVIDIA
// packs its own scheme into the 32 bits, and decoding that with the Vulkan macros prints a number
// matching nothing on the user's driver package. The known vendor encodings are special-cased;
// everything else gets the Vulkan layout, which is what Mesa and the spec's own advice use.
[[nodiscard]] std::string formatDriverVersion(std::uint32_t vendorId, std::uint32_t version);

// One device, one line, stable enough to paste into a bug report.
[[nodiscard]] std::string describeDevice(const DeviceInfo& info);

// THE POLICY. Pure: no Vulkan, no globals, no logging, no getenv.
//
// With no selector: the best USABLE device by `deviceTypeRank`. With one: an all-digit selector is
// an INDEX (matched against `DeviceInfo::index`), anything else is a NAME -- case-insensitive,
// with exact equality preferred over substring so that a device whose whole name is contained in
// another's ("Intel(R) UHD Graphics" inside "Intel(R) UHD Graphics 620") stays nameable.
//
// ⚠ TIE-BREAK, pinned: whenever several devices are equally good -- same type rank in the
// automatic pick, or several names matching a substring -- the LOWEST ENUMERATION INDEX wins. It
// is the first one in the list the user just read, and it does not move when a driver reorders
// something unrelated.
//
// An unmatched selector, or one that names only unusable devices, is NEVER fatal and NEVER silent:
// the automatic pick comes back with `warning` set saying exactly what was ignored.
[[nodiscard]] DeviceChoice pickPhysicalDevice(const std::vector<DeviceInfo>& devices,
                                              const DeviceSelector& selector);

// Precedence, highest first -- the same shape `decideGpuPolicy` (gpu_policy.hpp) settled for
// `--cpu`/MOSAIC_CPU_ONLY, and for the same reason: a FLAG is a one-run override and outranks the
// environment. There is no persisted third source; naming a GPU is a per-machine, per-run act, and
// a saved one is exactly the setting that strands a user whose laptop changed.
[[nodiscard]] DeviceSelector decideDeviceSelector(std::string_view flag, std::string_view env);

// Reads MOSAIC_DEVICE. Unset or empty is NO OPINION, so a shell that exports nothing behaves as it
// did before this existed. Sibling of MOSAIC_CPU_ONLY and MOSAIC_GPU_PROFILE: this one says WHICH
// device, those say whether a lane may build one and what it may claim.
[[nodiscard]] DeviceSelector deviceSelectorFromEnv();

// The process-wide selector, seeded from the environment and overridden once by `main()`.
//
// A global is the honest shape, the same way it is for `gpuPolicy()`: `VulkanContext::shared()` is
// reached from a dozen lanes, from --bench and from every GPU test, and threading a selector
// through all of them would be this variable with more spelling. Written once at start-up, read
// thereafter.
[[nodiscard]] const DeviceSelector& deviceSelector() noexcept;
void setDeviceSelector(DeviceSelector selector);

// Fill a `DeviceInfo` from a real device. `instanceApiVersion` / `instanceHasGetPhysDevProps2` say
// whether the driver-name query is legal here, exactly as they do for `probePhysicalDevice`;
// without it `driverName` stays empty, which is an ordinary outcome and not an error.
[[nodiscard]] DeviceInfo describePhysicalDevice(VkInstance instance, VkPhysicalDevice dev,
                                                std::uint32_t index,
                                                std::uint32_t instanceApiVersion,
                                                bool instanceHasGetPhysDevProps2);

// Log the enumerated list, the choice, the reason, and any warning. `who` names the consumer
// ("compute", "present"), and the whole block is emitted ONCE PER `who` per process: both the
// compute context and the window enumerate, several lanes ask the compute context for a device,
// and a start-up block that repeats stops being read.
//
// Choosing a software rasterizer is logged at WARN rather than INFO. It is accepted in full, but
// an unusual fallback nobody is told about is a lie (section 10.4).
void logDeviceSelection(std::string_view who, const std::vector<DeviceInfo>& devices,
                        const DeviceChoice& choice);

}  // namespace mosaic::render
