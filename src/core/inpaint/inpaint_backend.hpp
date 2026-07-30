#pragma once

// Inpainting engine for Mosaic — pluggable backend interface (PLAN S37-b).
// One InpaintEngine dispatches to an IInpaintBackend; the built-in backends and the
// future Lua ScriptBackend (S40) all implement this same interface. The He & Sun
// offset-statistics solver (S37-c) is just one more backend behind this seam.

#include "common/image.hpp"   // mosaic::common::ImageF / ColorF
#include "core/selection.hpp" // mosaic::core::Selection — the hole mask (coverage > 0)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mosaic::core::inpaint {

// Tunables shared across backends. Defaults are He & Sun's published parameters (used by the
// S37-c offset-statistics backend) plus the diffusion-solver knobs used by PdeBackend; a backend
// reads only the fields it needs and ignores the rest.
struct Params {
    // --- offset-statistics / graph solver (S37-c) ---
    int patchSize = 8;             // w for offset statistics
    int K = 60;                    // number of dominant offsets
    double tauFraction = 1.0 / 15; // tau = tauFraction * max(regionW, regionH)
    int maxRegionW = 800;          // downsample target for the working region
    int maxRegionH = 600;
    bool adaptiveSmallRegion = true; // "Low-effort on small selection": shrink the working-region
                                     // margin around the hole for SMALL holes (down to ~1.4x the
                                     // bbox = a ~20% surround), growing back to the full 3x as the
                                     // hole approaches a third of the image. Small touch-ups then
                                     // sample only their immediate surroundings (a big speed win);
                                     // large removals keep the full context. false = always 3x.
    bool globalSearchRegion = false; // ⚠ Search the WHOLE image for offsets, INVARIANT to the
                                     // selection, instead of a window sized from the hole's bbox.
                                     // Whether the search domain depends on the selection is a
                                     // load-bearing property of this engine — see the note above
                                     // workingRegionRect(). This flag exists to make it invariant
                                     // and is NOT a spare optimisation toggle; do not delete it as
                                     // unused. OFF pending the measured quality call: flipping it
                                     // changes the offsets every heal is built from, so it needs a
                                     // visual pass across real photos, not just a timer. (Measured
                                     // once at 10.4 s -> 7.0 s on a real removal mask, fill clean,
                                     // so it is not a speed-for-something trade.) Overrides
                                     // adaptiveSmallRegion.
    bool showSampleArea = false;     // UI-only (the engine ignores it): when set, the canvas draws a
                                     // faint green wash over the analysed region during a run so the
                                     // user can see what's being sampled. Rides here so it sits in
                                     // the schema-driven settings panel below "Low-effort".
    int nnfMaxPatches = 16000; // cap on patches used for the offset NNF. The exact k-NN is
                               // O(n^2) in the patch count, so without a cap the cost
                               // explodes with region size (hours on a real photo). He &
                               // Sun: the offset statistics are insensitive to the NNF, so a
                               // uniform DETERMINISTIC subset (fixed raster stride, no RNG) of
                               // this size reproduces the dominant peaks at bounded,
                               // image-size-independent cost. The matching stays an independent,
                               // exact per-patch NN lookup — no propagation, no random search
                               // (an invariant of this engine; see offset_statistics.hpp).
    double histSmoothSigma = 1.41421356; // sqrt(2)
    int peakWindow = 9;                  // local-max window for peak picking
    int boundaryOffsets = 12; // extra candidate offsets from hole-BOUNDARY matching:
                              // fully-known ring patches are matched (exact k-NN, no
                              // propagation) and the most-voted offsets join the K dominant
                              // ones — the locally-precious continuation a global frequency
                              // vote misses (the "treeline junction" fix). 0 disables.
    // ⚠ INVARIANT: there is deliberately NO flag for He & Sun's alternative "matching-based"
    // solver (paper stage 2b) — a multi-scale patch-optimization hole filler with cross-scale
    // source transfer. It was never built and is deliberately out of scope for this engine; the
    // authors themselves recommend the graph solver. Do not add it.
    bool poissonBlend = true;    // plain Poisson (gradient-domain) seam blend after the graph
                                 // cut; basic solve only, never the quadtree variant
    bool outpaintBoundaryCrisp = false; // outpaint lever: keep hole↔known boundary
                                        // mismatches crisp in the outpaint seam blend. OFF by
                                        // default — user testing found regressions (sky-only
                                        // fills, "trench" look near the ground); cause not yet
                                        // investigated.
    bool outpaintDeviationTax = false;  // outpaint lever: tax outpaint donors by
                                        // local band-pass deviation (stray-cloud avoidance). OFF
                                        // by default, same regression note as above.
    int twoScaleFactor = 1;      // graph cut downsample factor (1 = auto; >1 forces a fixed
                                 // two-scale factor). Auto picks the smallest factor whose
                                 // coarse node count fits graphCutMaxNodes.
    int graphCutMaxNodes = 6000; // auto two-scale target: cap the coarse graph-cut node
                                 // count so one max-flow-per-(label×cycle) stays bounded —
                                 // the α-expansion is the cost driver and grows with the
                                 // hole-pixel (node) count, so a big hole is solved coarse
                                 // and the labels are nearest-upsampled (He & Sun §4.2).
    int graphCutMaxScale = 32;   // hard cap on the auto two-scale factor (keeps the node
                                 // budget enforceable for very large holes; beyond it the
                                 // node count is allowed to grow rather than over-coarsening).
    int graphCutMaxCycles = 4;   // α-expansion label sweeps. It converges fast (most energy
                                 // drops in the first 1-2 sweeps) and each sweep is K
                                 // max-flows, so this caps the solve count.
    bool seamRefine = true;      // After the coarse two-scale cut, refine the labels at FULL
                                 // resolution along seams so they route around fine features
                                 // instead of cutting through them (the coarse cut decides seams
                                 // on a blurred image and nearest-upsamples them, leaving blocky
                                 // seams the Poisson blend can't hide — visible as streaks in
                                 // smooth regions like sky). He & Sun §4.2 fine refinement,
                                 // realized as banded local energy minimization (Gauss-Seidel
                                 // ICM; Besag 1986) over the SAME validity + seam-coherence energy
                                 // — deterministic, bounded, no second graph cut. No effect on the
                                 // single-scale path (its labeling is already full-resolution).
    int seamRefineSweeps = 0;    // 0 = auto (≈ the coarse scale, so a seam can migrate up to ~l/2
                                 // px off a feature); >0 forces a fixed sweep count.
    int poissonIterations = 200; // red-black SOR sweeps for the seam blend. The seams settle
                                 // in the first dozens of sweeps; this bounds the blend over
                                 // a big hole (separate from PdeBackend's pdeIterations). At
                                 // this count SOR is already better-converged than 800 plain
                                 // Gauss-Seidel sweeps were.
    double poissonOmega = 1.9;   // SOR over-relaxation factor (1 = plain Gauss-Seidel; toward
                                 // 2 accelerates the slow low-frequency convergence). Young/
                                 // Frankel SOR (1950s); convergent for 0<ω<2.

    // --- PdeBackend (diffusion) ---
    int pdeIterations = 800;  // max Gauss-Seidel sweeps for the Laplace solve
    double pdeEpsilon = 1e-4; // early-out once the largest per-sweep change drops below this

    // --- ResynthBackend (Harrison texture synthesis) ---
    int resynthNeighbors = 30; // nearest already-known points compared per candidate (the GIMP
                               // plugin's default, affordable since the wave-parallel loop)
    int resynthTries = 200;    // uniform-random donor candidates per pixel (fixed-seed PRNG;
                               // plugin default)
    int resynthPasses = 2;     // first fill + whole-hole refinement passes
    double resynthSensitivity = 0.12; // Cauchy width of the robust match metric (outlier
                                      // tolerance; Harrison's sensitivity parameter)
};

// --- Backend self-description + settings schema (Settings → Inpainting) -----------------------
// Plain data the Settings UI renders; every backend ships its own (an S40 Lua backend fills the
// same structs). The UI is generated from these, so a new backend appears with its own engine
// "spec sheet" and its own tunable controls — nothing about the panel is hardcoded per method.

// Provenance / "spec sheet" for one backend, surfaced on the Engine tab.
struct BackendInfo {
    std::string displayName;                // short label for the engine selector ("Offset statistics")
    std::string method;                     // the technique ("Patch-offset graph completion")
    std::string authors;                    // "Kaiming He & Jian Sun, 2014" (or "" )
    std::string paper;                      // paper title + venue (or "")
    std::string summary;                    // one paragraph: what it does / when to use it
    std::vector<std::string> deviations;    // how Mosaic's implementation differs from the paper
    std::vector<std::string> augmentations; // extras Mosaic adds (two-scale, SOR, …)
    std::string cost;                       // memory/time note ("Full-res ~16 s; CPU, multithreaded")
};

// One tunable control a backend exposes to the Backend Settings tab. Declarative so the pane is
// generated; switching backend swaps the whole control set.
struct ParamControl {
    enum class Kind { Int, Real, Bool, Choice };
    std::string key;                  // stable id; the backend maps it to a Params field
    std::string label;                // control label ("Dominant offsets (K)")
    std::string help;                 // one-line caption, or ""
    Kind kind = Kind::Int;
    double min = 0.0;                 // Int/Real range + step
    double max = 1.0;
    double step = 1.0;
    std::vector<std::string> choices; // Choice: option labels (value = selected index)
    double defaultValue = 0.0;        // value when no preset overrides it (Bool: 0/1; Choice: index)
    bool advanced = false;            // grouped under "Advanced" vs shown up front
};

// A named quality/speed preset: per-control values. Applying it sets each listed control; controls
// absent from `values` keep their ParamControl::defaultValue.
struct PresetSpec {
    std::string id;                                     // "fast" | "balanced" | "best" | …
    std::string label;                                  // "Balanced"
    std::vector<std::pair<std::string, double>> values; // control key -> value
};

// The Backend Settings tab's full description for one backend.
struct BackendSettingsSchema {
    std::vector<PresetSpec> presets;    // empty => no preset row
    std::string defaultPreset;          // id of the preset selected by default ("" => none)
    std::vector<ParamControl> controls; // empty => "this backend has no settings"
};

// A unit of work for a backend: fill the masked region of `image`. Pure compute — no document
// mutation, no UI, no file I/O. References are borrowed for the duration of the call.
struct InpaintRequest {
    const common::ImageF& image; // working float RGBA (straight alpha)
    const Selection& holeMask;   // coverage > 0 == pixels to fill
    Params params{};
    // Optional cancellation token the host can flip from another thread; the backend's tight loops
    // poll it cheaply so a long stage (Analyzing / Solving) aborts promptly, not only at stage
    // boundaries. nullptr = never cancelled (the headless/test path).
    const std::atomic<bool>* cancel = nullptr;
};

// One stage's wall-clock cost (milliseconds), for the "where did the time go" diagnostic. Names are
// stable strings ("offset-stats", "graph-cut", "poisson-blend", …); order is execution order.
struct StageTiming {
    std::string name;
    double ms = 0.0;
};

// The completed pixels (full image; only the hole changed). `ok == false` means the backend
// declined or aborted — `image` then holds the unmodified input and `detail` says why.
struct InpaintResult {
    common::ImageF image;
    bool ok = true;
    std::string detail;
    std::vector<StageTiming> timings; // per-stage ms breakdown (empty if not instrumented)
};

// RAII stopwatch: on destruction it appends {name, elapsed-ms} to `sink`. A null sink is a no-op so
// the compute stages can be timed without forcing every caller to supply a collector. Pure
// diagnostics — no behavioural effect on the result.
class ScopedStage {
public:
    ScopedStage(std::vector<StageTiming>* sink, std::string name)
        : m_sink(sink), m_name(std::move(name)), m_t0(std::chrono::steady_clock::now()) {}
    ~ScopedStage() {
        if (m_sink == nullptr) {
            return;
        }
        const auto t1 = std::chrono::steady_clock::now();
        m_sink->push_back(
            {std::move(m_name), std::chrono::duration<double, std::milli>(t1 - m_t0).count()});
    }
    ScopedStage(const ScopedStage&) = delete;
    ScopedStage& operator=(const ScopedStage&) = delete;

private:
    std::vector<StageTiming>* m_sink;
    std::string m_name;
    std::chrono::steady_clock::time_point m_t0;
};

// A progress tick from a running backend. `fraction` is the overall 0..1 completion; `stage` is a
// short human label for the phase ("Analyzing", "Solving", "Blending"); `preview`, when non-null,
// is an intermediate full-image result (only the hole differs from the input) the host may display
// as a live preview. The preview pointer is valid ONLY for the duration of the callback — copy what
// you keep. Callbacks may run on a worker thread.
struct InpaintProgress {
    float fraction = 0.0f;
    std::string_view stage;
    const common::ImageF* preview = nullptr;
};

// Progress / cancellation callback: return false to request cancellation. An empty function (the
// default) means "always continue".
using ProgressFn = std::function<bool(const InpaintProgress&)>;

// Small helper a backend uses to emit progress and observe cancellation without each call site
// re-checking the flag. report() forwards an absolute fraction + stage (+ optional preview) and
// latches `cancelled` the first time the host returns false; once cancelled it short-circuits.
class ProgressReporter {
public:
    explicit ProgressReporter(const ProgressFn& fn, const std::atomic<bool>* cancel = nullptr)
        : m_fn(fn), m_cancel(cancel) {}

    bool report(float fraction, std::string_view stage, const common::ImageF* preview = nullptr) {
        if (cancelled()) {
            return false;
        }
        if (m_fn) {
            const InpaintProgress p{fraction, stage, preview};
            if (!m_fn(p)) {
                m_cancelled = true;
                return false;
            }
        }
        return true;
    }
    // Cheap poll for the tight loops: the latched cancel OR the host's token. Safe to read
    // anywhere.
    [[nodiscard]] bool cancelled() const {
        return m_cancelled || (m_cancel != nullptr && m_cancel->load());
    }
    [[nodiscard]] const std::atomic<bool>* cancelToken() const { return m_cancel; }

private:
    const ProgressFn& m_fn;
    const std::atomic<bool>* m_cancel = nullptr;
    bool m_cancelled = false;
};

// One filler. Implementations: PdeBackend (S37-b, diffusion), OffsetStatisticsBackend
// (S37-c, He & Sun), ScriptBackend (S40, Lua-registered provider).
class IInpaintBackend {
public:
    virtual ~IInpaintBackend() = default;

    [[nodiscard]] virtual std::string id() const = 0;   // stable id, e.g. "pde", "offset-stats"
    [[nodiscard]] virtual std::string name() const = 0; // human-facing label

    [[nodiscard]] virtual InpaintResult run(const InpaintRequest& request,
                                            const ProgressFn& progress) = 0;

    // Whether the backend can run right now. Built-ins are always available; the ScriptBackend is
    // available only once a Lua provider is registered (S40), so the Settings selector can hide it.
    [[nodiscard]] virtual bool available() const { return true; }

    // Self-description for the Settings → Inpainting "Engine" tab. Default: just the name.
    [[nodiscard]] virtual BackendInfo info() const { return BackendInfo{name()}; }

    // Tunable controls + presets for the "Backend Settings" tab. Default: nothing tunable.
    [[nodiscard]] virtual BackendSettingsSchema settingsSchema() const { return {}; }

    // Bridge one schema control's value onto a Params (UI key -> Params field). Unknown keys are
    // ignored. Default: no tunables.
    virtual void applyParam(Params& /*params*/, const std::string& /*key*/,
                            double /*value*/) const {}

    // The document-pixel region this backend would analyse for a hole with bounding box
    // `holeBounds` (nullopt = no coverage), or nullopt if the backend has no such region to report.
    // Lets the UI preview "what is being sampled" GENERICALLY (S39): the canvas overlay only needs a
    // rect, and each backend knows its own sample neighbourhood (a future / script backend just
    // overrides this). Default: none -> no preview for that backend.
    [[nodiscard]] virtual std::optional<common::Rect>
    analysedRegion(std::uint32_t /*imageW*/, std::uint32_t /*imageH*/,
                   const std::optional<common::Rect>& /*holeBounds*/,
                   const Params& /*params*/) const {
        return std::nullopt;
    }
};

// Build a Params for `backend`'s `presetId` (empty/unknown id => the backend's bare defaults). Each
// of the backend's controls is applied with its preset value, or its ParamControl::defaultValue
// when the preset doesn't list it. Shared by the host (to drive runs) and the Settings UI.
[[nodiscard]] Params paramsForPreset(const IInpaintBackend& backend, const std::string& presetId);

} // namespace mosaic::core::inpaint
