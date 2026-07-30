#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

// GPU TIMESTAMP QUERIES (S60-a; docs/s60-performance-plan.md section 8.1).
//
// ---- What was wrong ---------------------------------------------------------------------------
//
// Every `Lane::Gpu` row in the profiler is a `MOSAIC_PERF_SCOPE` wrapped around a *host* scope that
// happens to end in a submit. It therefore reports command-buffer recording + descriptor writes +
// staging memcpys + `vkQueueSubmit` + a fence wait -- everything except the thing the name implies.
// The 3D-text lane's own comment has said so since S30. An optimisation arc that cannot tell
// "the shader is slow" from "we are fence-bound" is guessing, and S60-a's whole thesis is that the
// old GPU compositor lost on BUS TRAFFIC rather than on arithmetic. That distinction is exactly
// what this class measures.
//
// Deliberately deferred out of S60-alpha item 6, which wanted "one device and one submit path to
// instrument, so doing it across today's four independent devices would be work thrown away".
// Both preconditions now hold: `VulkanContext::shared()` / `adopt()` own every device, and
// `VulkanContext::submit()` is the only queue submission in `src/render` outside the swapchain.
//
// ---- What it is -------------------------------------------------------------------------------
//
// A `VK_QUERY_TYPE_TIMESTAMP` pool plus the bookkeeping to turn pairs of ticks into
// `Lane::GpuDevice` profiler rows. Everything it uses is Vulkan 1.0 core -- `vkCreateQueryPool`,
// `vkCmdResetQueryPool`, `vkCmdWriteTimestamp`, `vkGetQueryPoolResults` -- so it needs no tier and
// works at the floor. It is still CAPS-GATED: `create()` returns null when `GpuCaps.timestampQueries`
// is false, which is what `MOSAIC_GPU_PROFILE=floor` synthesises. A null timer is an ORDINARY
// OUTCOME, not an error: the lane composites exactly as before and simply has no device row, so
// every call site must be null-tolerant (see `TileCompositor`, which holds one).
//
// ---- Usage, and the one rule ------------------------------------------------------------------
//
//     timer->beginSubmission(cmd);                        // right after vkBeginCommandBuffer
//     const std::int32_t s = timer->beginScope(cmd, "Tile blend");
//     ... vkCmdDispatch ...
//     timer->endScope(cmd, s);
//     ... vkEndCommandBuffer / submit / vkWaitForFences ...
//     timer->resolveAndRecord();                          // AFTER the fence has signalled
//
// THE RULE: `resolveAndRecord()` must not be called until the submission that wrote the timestamps
// has completed. Every lane in `src/render` fence-waits its own submit, so this is where they
// already are; when S60-c moves compositing off the UI thread this becomes the thing to re-read.
// Calling it early is not undefined -- the read is non-blocking and drops unavailable queries --
// but it silently loses the samples.
//
// Scopes may nest and may be left open (an early-return path that abandons the command buffer);
// an open scope is simply not reported. Collection is latched at `beginSubmission()` exactly as
// `Profiler::Scope` latches it at construction, so toggling `--profile` mid-submission cannot
// produce a duration measured from an un-taken start.

namespace mosaic::render {

class VulkanContext;

// ---- Pure arithmetic (no Vulkan calls; this is what the unit tests exercise) --------------------

// Keep only the bits the device promises are meaningful. Vulkan says the high
// `64 - timestampValidBits` bits of a query result are UNDEFINED, not zero, so a raw subtraction of
// two results on a 36-bit counter can produce anything at all. `validBits` of 0 or >= 64 means
// "nothing to mask" and the value is returned unchanged.
[[nodiscard]] std::uint64_t maskTimestamp(std::uint64_t raw, std::uint32_t validBits) noexcept;

// `end - begin` in the masked counter domain, converted to milliseconds through `periodNs`
// (`VkPhysicalDeviceLimits::timestampPeriod`, nanoseconds per tick).
//
// The counter WRAPS at 2^validBits and wrapping is normal rather than an error: at the 1 ns/tick
// typical of desktop parts a 36-bit counter turns over every ~69 s, so a frame that happens to
// straddle the turn must still measure correctly. Subtracting in unsigned and re-masking is what
// makes that true. Returns 0 for a non-positive period -- a device that reports no period cannot
// be converted, and 0 is visible in a table where a garbage number is not.
[[nodiscard]] double timestampDeltaMs(std::uint64_t begin, std::uint64_t end,
                                      std::uint32_t validBits, double periodNs) noexcept;

// ---- The timer ---------------------------------------------------------------------------------

class GpuTimer {
public:
    // Scopes per submission. Four is what the tile compositor uses; 16 leaves room for a lane to
    // instrument itself finely without ever thinking about the pool size, at a cost of 32 queries
    // (256 bytes of device memory).
    static constexpr std::uint32_t kDefaultMaxScopes = 16;

    // Null (with `error` set) when the device cannot do timestamps or the pool could not be
    // created. Both are ordinary; the caller keeps working without device rows.
    [[nodiscard]] static std::unique_ptr<GpuTimer> create(const VulkanContext& ctx,
                                                          std::uint32_t maxScopes,
                                                          std::string& error);

    ~GpuTimer();

    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;
    GpuTimer(GpuTimer&&) = delete;
    GpuTimer& operator=(GpuTimer&&) = delete;

    [[nodiscard]] std::uint32_t capacity() const noexcept { return m_maxScopes; }
    [[nodiscard]] std::uint32_t validBits() const noexcept { return m_validBits; }
    [[nodiscard]] double periodNs() const noexcept { return m_periodNs; }
    // True between beginSubmission() and resolveAndRecord() when collection was on at the former.
    [[nodiscard]] bool recording() const noexcept { return m_active; }

    // Start a submission: forget the last one's scopes and reset the pool ON `cmd`. Record this
    // immediately after vkBeginCommandBuffer -- a timestamp query must be reset before it is
    // written, and the host-side `vkResetQueryPool` is a Vulkan 1.2 entry point this arc's floor
    // does not have.
    void beginSubmission(VkCommandBuffer cmd) noexcept;

    // Open a scope; returns its handle, or -1 when collection is off or the pool is full (which is
    // a silent, safe no-op -- an instrumented lane must never fail because of instrumentation).
    // `name` must have static storage: it is held as a string_view until resolveAndRecord().
    [[nodiscard]] std::int32_t beginScope(VkCommandBuffer cmd, std::string_view name) noexcept;
    void endScope(VkCommandBuffer cmd, std::int32_t scope) noexcept;

    // Read the pool and record one `Lane::GpuDevice` row per CLOSED scope. Returns how many rows
    // were recorded. Call only after the submission's fence has signalled (see the header note).
    std::uint32_t resolveAndRecord() noexcept;

    // The last resolve's samples, in the order the scopes were opened. For tests and for a caller
    // that wants the numbers without going through the profiler singleton.
    struct Sample {
        std::string_view name;
        double ms = 0.0;
    };
    [[nodiscard]] const std::vector<Sample>& lastSamples() const noexcept { return m_samples; }

private:
    GpuTimer() = default;

    struct ScopeRec {
        std::string_view name;
        std::uint32_t beginQuery = 0;
        std::uint32_t endQuery = 0;
        bool closed = false;
    };

    VkDevice m_device = VK_NULL_HANDLE;  // borrowed from the context; never destroyed here
    VkQueryPool m_pool = VK_NULL_HANDLE;
    std::uint32_t m_maxScopes = 0;
    std::uint32_t m_validBits = 0;
    double m_periodNs = 0.0;
    bool m_active = false;  // collection latched at beginSubmission()
    std::vector<ScopeRec> m_scopes;
    std::vector<Sample> m_samples;
};

}  // namespace mosaic::render
