#pragma once

#include <cstdlib>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

#include "common/log.hpp"

// "May we use a device at all" (S60-b item 14, docs/s60-performance-plan.md section 6).
//
// This is NOT a second capability model, and folding it into `GpuCaps` would be a mistake: those
// are two different questions asked at two different times.
//
//     GpuCaps   -- given a device, which lanes FIT? (a probe; it must still work in order to
//                  REPORT what was refused, so it cannot be the thing that refuses)
//     GpuPolicy -- may a compute lane build a device in the FIRST PLACE?
//
// It exists so "run without the GPU" is ONE decision asked in ONE shape. Without it, five lanes
// each invent their own check and the fifth gets it subtly wrong.
//
// SCOPE -- narrow on purpose. This is section 6.2's **Level 1**: the COMPUTE lanes (blur,
// texture generator, extruded text, the buffer compositor, the tiled compositor) decline to build
// any Vulkan object, and every caller takes the CPU path it already owns. PRESENTATION is
// untouched -- the present pass, marching ants, handles, the loupe, the caret and the empty-state
// field are shader code with NO CPU twin (section 5), so retiring them is a different and much
// larger project ("Level 3") that S60 explicitly does not take on. `--cpu` still presents through
// Vulkan, and that is not an oversight.
//
// A refusal here is a FALLBACK, never a failure. Every CPU path this hands work to is the golden
// reference its GPU lane is parity-tested against, so CPU-only mode changes what the pixels COST
// and never what they ARE.
namespace mosaic::render {

enum class GpuUse {
    Auto,    // the default: each lane asks its own caps questions and builds if it fits
    CpuOnly, // no compute lane builds anything; presentation is unaffected
};

// A three-valued opinion from a source that may have none. Both the command line and the
// environment use it so that "said nothing" is representable rather than defaulted away -- the
// bug a plain bool would guarantee the moment a persisted setting has to LOSE to a flag.
enum class GpuUseOverride { None, ForceCpuOnly, ForceAuto };

struct GpuPolicy {
    GpuUse use = GpuUse::Auto;

    // The one question a compute lane asks. Named for the LANES and not for the GPU, because
    // presentation is not one of them (see the header note).
    [[nodiscard]] constexpr bool allowsComputeLane() const noexcept { return use == GpuUse::Auto; }

    bool operator==(const GpuPolicy&) const = default;
};

// The PURE decision -- no globals, no getenv, no logging. This is the piece the tests exercise
// (tests/test_settings.cpp), which is why the three inputs are parameters rather than lookups.
//
// Precedence, highest first:
//   1. the command line (`--cpu` one way, `--gpu`/`--gpu-compute` the other). **A FLAG WINS OVER
//      THE SETTING** because a flag is a ONE-RUN override -- that is what flags are for, and it is
//      the only way a user whose persisted preference is wrong (or whose driver died since they
//      set it) gets one run of the app in which to fix it. It is never written back.
//   2. MOSAIC_CPU_ONLY, the flag's environment twin -- `--profile` / MOSAIC_PROFILE=1 set that
//      precedent in main.cpp. It exists because a harness that does not own the command line
//      (ctest, above all) still has to be able to run the whole suite in this mode (plan item 16).
//   3. the persisted Settings -> Rendering preference.
//   4. Auto.
[[nodiscard]] constexpr GpuPolicy decideGpuPolicy(GpuUseOverride flag, GpuUseOverride env,
                                                  bool settingCpuOnly) noexcept {
    if (flag != GpuUseOverride::None)
        return GpuPolicy{flag == GpuUseOverride::ForceCpuOnly ? GpuUse::CpuOnly : GpuUse::Auto};
    if (env != GpuUseOverride::None)
        return GpuPolicy{env == GpuUseOverride::ForceCpuOnly ? GpuUse::CpuOnly : GpuUse::Auto};
    return GpuPolicy{settingCpuOnly ? GpuUse::CpuOnly : GpuUse::Auto};
}

// Reads MOSAIC_CPU_ONLY. Unset or empty is NO OPINION rather than "off", so a shell that exports
// nothing behaves exactly as it did before this existed. "0"/"false"/"off"/"no" force Auto -- the
// escape hatch for a machine whose settings file says cpu-only; any other non-empty value forces
// CpuOnly.
//
// SIBLING of MOSAIC_GPU_PROFILE=floor, not a replacement. That one clamps what a device may
// CLAIM; this one decides whether a compute lane may ask a device anything at all. Setting both is
// legal and well-defined: no compute lane is built, and whatever device still comes up (the
// window's, for presentation) reports the Vulkan 1.0 guaranteed minimums.
[[nodiscard]] inline GpuUseOverride gpuUseOverrideFromEnv() {
    const char* v = std::getenv("MOSAIC_CPU_ONLY");
    if (v == nullptr || *v == '\0')
        return GpuUseOverride::None;
    const std::string_view s{v};
    if (s == "0" || s == "false" || s == "off" || s == "no")
        return GpuUseOverride::ForceAuto;
    return GpuUseOverride::ForceCpuOnly;
}

namespace detail {
// The process-wide policy. A mutable global is the honest shape here: lanes are constructed from
// the compositor, from three app_window overrides, from the resident lane, from --bench and from
// every GPU test, and threading one enum through all of them would be the same global with more
// spelling.
//
// Reads are unsynchronized on purpose. It is written once at start-up, and thereafter only from
// the UI thread when Settings changes it; the alternative is a mutex acquired on every lane
// construction, forever, to guard a variable that moves twice a session.
[[nodiscard]] inline GpuPolicy& gpuPolicyStorage() noexcept {
    // ⚠ SEEDED FROM THE ENVIRONMENT, not default-constructed, and that is the whole of item 16.
    // `main()` calls setGpuPolicy() once it has parsed the flags and loaded the settings -- but
    // main() is the APP's entry point, and the TEST BINARY has doctest's. Defaulting to Auto here
    // made MOSAIC_CPU_ONLY a no-op in exactly the process the variable exists for: the suite,
    // which is meant to become a CPU-lane regression net under it. Caught by the orchestrator
    // because `MOSAIC_CPU_ONLY=1 mosaic_tests` reported an assertion count IDENTICAL to the
    // ordinary run -- if the GPU lanes had really refused, their cases would have skipped and the
    // count would have dropped. An env-gated mode that silently does nothing is worse than no
    // mode: every green run is evidence of nothing.
    //
    // Function-local static init is thread-safe and runs once, before any lane can ask; main()'s
    // later setGpuPolicy() still wins, because a flag outranks the environment either way.
    static GpuPolicy policy =
        decideGpuPolicy(GpuUseOverride::None, gpuUseOverrideFromEnv(), /*settingCpuOnly=*/false);
    return policy;
}
}  // namespace detail

[[nodiscard]] inline const GpuPolicy& gpuPolicy() noexcept { return detail::gpuPolicyStorage(); }
inline void setGpuPolicy(GpuPolicy policy) noexcept { detail::gpuPolicyStorage() = policy; }

// The one call every compute lane's `create()` makes, first thing, before it touches Vulkan.
// Returns true in the default build having evaluated exactly one enum compare -- no flag, no
// setting, no new work on any path.
//
// On a refusal it fills `error` with a NAMED reason and logs it ONCE per lane per process. The
// house rule from gpu_caps.hpp -- "a GPU lane that does not fit refuses ITSELF" -- carries over
// unchanged: the lane names itself and the reason is a sentence a user could act on. Once,
// because several of these `create()` calls sit inside per-operation paths (compositeBuffer
// builds a GpuCompositor per composite), and a mode the user deliberately asked for must not
// turn into a log flood.
[[nodiscard]] inline bool computeLaneAllowed(std::string_view lane, std::string& error) {
    if (gpuPolicy().allowsComputeLane())
        return true;
    error = "CPU-only mode: the ";
    error += lane;
    error += " GPU lane was not built (--cpu / MOSAIC_CPU_ONLY / Settings -> Rendering)";

    static std::mutex mutex;
    static std::set<std::string, std::less<>> announced;
    const std::lock_guard<std::mutex> lock(mutex);
    if (announced.emplace(lane).second)
        common::log::category("render")->info(
            "cpu-only: the {} GPU lane was not built; its CPU path serves", lane);
    return false;
}

}  // namespace mosaic::render
