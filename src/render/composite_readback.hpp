#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/geometry.hpp"
#include "common/image.hpp"

// The EXPLICIT READBACK SEAM (S60-a item 12; docs/s60-readback-consumers.md §6).
//
// Under residency the composite lives on the device. `MainWindow::m_lastComposite` -- a
// document-sized CPU mirror kept coherent on every patch because five different consumers wanted
// to read pixels -- stops being the source of truth and becomes one of several things that can ask
// the device. The audit that preceded this file found **nineteen** such consumers, not five, and
// three of the six most dangerous ones were on nobody's list. It also found the plan's proposed
// API shape wrong.
//
// ---- Why this is not `requestReadback(rect) -> future` -----------------------------------------
//
// Four of the six dangerous consumers want a HANDFUL OF PIXELS, RIGHT NOW, EVERY FRAME: the
// status-bar colour readout (one pixel, per pointer event), the eyedropper loupe (an 11x11 window,
// every frame while the tool is up), and their two stale-path twins. A futures API serves those
// with a fence per frame, which is worse than the readback it replaces. So the load-bearing shape
// is `pinMirror` + `peek`: a CPU mirror of a SMALL PINNED SET OF MACROTILES, refreshed as part of
// the composite pass, that turns the whole class into a memory read. The futures path exists for
// the consumers that genuinely want the canvas (Magic Wand, Copy Merged, Smart Recompose).
//
// ---- Three properties this API has on purpose ---------------------------------------------------
//
//  1. EVERY CONSUMER NAMES ITSELF. `name` is not optional on any entry point. It is the profiler
//     row, the per-frame budget line, and the key of the registry `consumers()` returns -- which
//     tests/test_composite_readback.cpp pins against a literal list, so adding a consumer without
//     declaring it fails ctest. The audit's §2.2 finding is that a provider lambda closing over the
//     composite is INVISIBLE to grep; a registry is the only guard that survives that.
//  2. THE SAFE DEFAULT IS THE DEFAULT. `Freshness` defaults to `AnyRecent` and `blocking` to false.
//     A consumer that needs synchrony has to type it. Today synchrony is what you get for writing
//     `m_lastComposite.rgba[p]`, which is exactly how this became a per-frame full-canvas cost in
//     two places nobody meant it to.
//  3. IT CANNOT SERVE TWO THINGS, AND THAT IS A DECISION. Off-canvas pixels (the Fill and
//     Layer-Effects modal preview panes pass `clampToCanvas=false`) and byte-exact CPU-lane output
//     (PNG/JPEG/JXL export and the `.mosaic` PRVW thumbnail, which need determinism an fp16 device
//     accumulator cannot promise) both keep `render::composite`. Recorded here so nobody
//     "finishes the job" by routing them through a readback later.
//
// FLTK-free and Vulkan-free at the interface: the policy is pure and unit-testable, and the only
// device contact is through `TileCompositor`.

namespace mosaic::render {

class TileCompositor;

// How current the pixels have to be. Deliberately not a bool: "may be served asynchronously" and
// "may be a whole gesture old" are different claims and the consumers differ on both axes.
enum class Freshness : std::uint8_t {
    // Must include every edit committed before this call. The cursor readout and the eyedropper --
    // a readout under a moving pointer that lags by one frame is a readout that lies.
    Current,
    // May lag a gesture in flight; must be current once the gesture ends. The 3D reflect env,
    // whose 0.30 s settle already exploits exactly this.
    Settled,
    // Any composite from the last few frames will do. The Channels histogram and the Smart-Resize
    // importance map, both of which already memoize on a revision and drop superseded results.
    AnyRecent,
};
[[nodiscard]] std::string_view freshnessName(Freshness f) noexcept;

// ---- The consumer vocabulary (docs/s60-readback-consumers.md §3) --------------------------------
//
// Every consumer that may reach the resident composite is NAMED HERE, not at the call site. A
// string literal typed into a lambda is invisible to review and to grep -- which is exactly how
// three of the six dangerous consumers came to exist without anyone listing them. Adding one means
// editing this header, and tests/test_composite_readback.cpp pins the full set.
//
// ⚠ FIVE CONSUMERS ARE DELIBERATELY ABSENT AND MUST STAY ABSENT. PNG/JPEG/JXL export and the
// `.mosaic` PRVW thumbnail need byte-determinism against the fp32 CPU reference that an fp16
// device accumulator cannot promise (a preview that wobbles in the low bit re-emits a PRVW chunk
// on every save); the Fill and Layer-Effects modal preview panes pass `clampToCanvas=false` and
// need pixels OUTSIDE the canvas, which the accumulator does not and will not have. Those five
// keep `render::composite` / `render::compositeRegion`. That is a decision, not an omission.
namespace consumers {
// Pinned-mirror consumers: a handful of pixels, right now, every frame. These MUST NOT fence.
inline constexpr std::string_view kCursorReadout = "status-bar cursor colour";   // audit A1
inline constexpr std::string_view kEyedropper = "eyedropper sample + loupe";     // audit A2
// Whole-canvas, once per discrete user action. A blocking transfer is invisible inside these.
inline constexpr std::string_view kMagicWand = "magic wand (all layers)";        // audit A3
inline constexpr std::string_view kEdgeBrush = "edge select brush (all layers)"; // audit A4
inline constexpr std::string_view kSmartRecompose = "smart recompose seed";      // audit A8
inline constexpr std::string_view kCropExpandFill = "crop-expansion inpaint seed"; // audit A9
inline constexpr std::string_view kCopyMerged = "copy merged";                   // audit B4
// Staleness-tolerant: these memoize on a revision already and must never ask for Current.
inline constexpr std::string_view kHistogram = "channels histogram";             // audit A5
inline constexpr std::string_view kSmartResize = "smart resize importance map";  // audit A7
inline constexpr std::string_view kReflectEnv = "3D text reflect environment";   // audit B6
// Screen-space, through requestScreenRect: the adjustment panel's fade under-image.
inline constexpr std::string_view kPanelFade = "adjustment panel fade";          // audit A10
}  // namespace consumers

// The whole vocabulary, sorted, for the registry test. Anything `consumers()` reports that is not
// in here is a consumer that named itself off the books.
[[nodiscard]] const std::vector<std::string>& knownConsumerNames();

struct ReadbackRequest {
    common::Rect roi;                              // document px; empty == the whole canvas
    Freshness freshness = Freshness::AnyRecent;    // note the SAFE default, not the convenient one
    bool blocking = false;                         // caller waits within this event turn
    std::string_view name;                         // REQUIRED: profiler row + registry key
};

struct ReadbackResult {
    bool ok = false;
    std::string error;
    common::Image image;          // roi-sized, straight alpha, document space, 8-bit
    common::Rect roi;             // what was actually served (>= the request)
    std::uint64_t revision = 0;   // the composite revision these pixels are from
    bool stale = false;           // served from a mirror older than the current revision
};

class CompositeReadback;

// RAII handle keeping a rect's macrotiles CPU-mirrored. Refcounted: two consumers pinning the same
// macrotile cost one mirror. Move-only; releasing is idempotent.
class MirrorPin {
public:
    MirrorPin() = default;
    ~MirrorPin();
    MirrorPin(const MirrorPin&) = delete;
    MirrorPin& operator=(const MirrorPin&) = delete;
    MirrorPin(MirrorPin&& other) noexcept;
    MirrorPin& operator=(MirrorPin&& other) noexcept;

    [[nodiscard]] bool held() const noexcept { return m_owner != nullptr; }
    void release() noexcept;

private:
    friend class CompositeReadback;
    MirrorPin(CompositeReadback* owner, std::uint64_t id) noexcept : m_owner(owner), m_id(id) {}
    CompositeReadback* m_owner = nullptr;
    std::uint64_t m_id = 0;
};

class CompositeReadback {
public:
    // Borrows the compositor; it must outlive this object. Nothing is read at construction.
    explicit CompositeReadback(TileCompositor& compositor) noexcept;
    ~CompositeReadback() = default;

    CompositeReadback(const CompositeReadback&) = delete;
    CompositeReadback& operator=(const CompositeReadback&) = delete;

    // ---- The general path -------------------------------------------------------------------
    //
    // ⚠ HONEST NOTE ON THE FUTURE. In this cut the readback runs on the calling thread and the
    // returned future is ALREADY SATISFIED. The shape is the contract, not yet the mechanism: it
    // exists now so consumers are written against the interface that S60-c can make genuinely
    // deferred (composite moves off the UI thread there, which is the first point at which an
    // asynchronous fence has anywhere to live). Nothing about a caller changes when it does.
    [[nodiscard]] std::future<ReadbackResult> request(const ReadbackRequest& req);

    // ---- The hot path: the pinned mirror ------------------------------------------------------

    // Keep the macrotiles covering `roi` CPU-mirrored. Hold this while a pointer-following tool is
    // active; `peek` is then a memory read rather than a fence. One 256 px macrotile is 256 KiB.
    [[nodiscard]] MirrorPin pinMirror(const common::Rect& roi, std::string_view name);

    // The pixel at `docPt`, or nullopt when its macrotile is not mirrored. A readout that blinks
    // off for one frame beats a fence; callers must NOT fall back to `request(..., blocking)`.
    [[nodiscard]] std::optional<common::Color8> peek(common::Vec2 docPt) const noexcept;
    // The same for a small window (the eyedropper's (2r+1)^2). nullopt when any covering macrotile
    // is unmirrored -- a half-served sample is a wrong colour, not a partial one.
    [[nodiscard]] std::optional<common::Image> peekRect(const common::Rect& roi) const;

    // Bring every pinned macrotile up to the compositor's current revision. Call once per
    // composite, from the same place the dirty set is cleared; a no-op when nothing is pinned or
    // the revision has not moved, which is why holding a pin costs nothing while the user is idle.
    void refreshMirror();

    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::size_t mirroredTiles() const noexcept { return m_mirror.size(); }
    [[nodiscard]] std::uint64_t mirrorBytes() const noexcept;

    // ---- A10's real answer: a SCREEN-space capture ------------------------------------------
    //
    // The adjustment panel's fade under-image maps each of its own pixels back through the view
    // and samples the composite -- a scattered gather whose document-space AABB is unbounded under
    // rotation and can be most of the canvas at low zoom. Expressed in SCREEN space it is bounded
    // by the panel, at any zoom, and it already includes the checkerboard and view transform the
    // lambda re-implements by hand. The provider is installed by whoever owns the present target;
    // without one this fails by name rather than silently falling back to the unbounded path.
    using ScreenCapture = std::function<bool(const common::Rect&, common::Image&, std::string&)>;
    void setScreenCapture(ScreenCapture fn) { m_screenCapture = std::move(fn); }
    [[nodiscard]] std::future<ReadbackResult> requestScreenRect(const common::Rect& screenPx,
                                                                std::string_view name);

    // ---- Guards (audit §7) --------------------------------------------------------------------

    // Per-frame budget, in the profiler, in release. A new consumer shows up as A NUMBER THAT
    // MOVED in the build the user runs -- the one guard that would actually have caught the two
    // per-frame full-canvas consumers nobody had listed.
    struct FrameStats {
        std::size_t requests = 0;
        std::uint64_t bytes = 0;
        std::size_t fences = 0;      // blocking, Current requests -- the thing to keep at zero
        std::size_t mirrorRefreshes = 0;
    };
    void beginFrame() noexcept;  // roll `current` into `last` and zero it
    [[nodiscard]] const FrameStats& lastFrameStats() const noexcept { return m_last; }
    [[nodiscard]] const FrameStats& thisFrameStats() const noexcept { return m_current; }

    // Every name ever asked for, sorted and de-duplicated. tests/test_composite_readback.cpp
    // asserts this equals a literal list, so a consumer added without declaring itself fails
    // ctest. Registration happens at REQUEST time, not at startup, so the test has to drive each
    // consumer at least once -- which is worth having regardless.
    [[nodiscard]] const std::vector<std::string>& consumers() const noexcept { return m_consumers; }

    // The narrowed §7(e) assert needs to know. Set from the app's aggregate gesture predicate; it
    // only ever affects a debug assert and the profiler row, never what is served.
    void setGestureActive(bool active) noexcept { m_gestureActive = active; }
    [[nodiscard]] bool gestureActive() const noexcept { return m_gestureActive; }

    // Is the mirror level with the accumulator? False the moment a composite lands that the mirror
    // has not been refreshed for -- which, by design, is every frame of a gesture, because
    // `refreshMirror()` is not called on gesture frames (a readback there fences the frame path,
    // and that made the resident lane slower than the CPU walk it replaces). Every mirror read
    // refuses when this is false: a peek that lies costs more than a peek that blinks off.
    [[nodiscard]] bool mirrorCurrent() const noexcept;

    // Did the composite move between the previous frame and this one? True means an edit is in
    // flight -- a stroke, a keystroke, a slider drag -- and nothing may transfer or fence. Goes
    // false on the first frame the composite holds still, which is when a readout may refresh.
    [[nodiscard]] bool editingThisFrame() const noexcept { return m_editingThisFrame; }

private:
    friend class MirrorPin;
    void releasePin(std::uint64_t id) noexcept;
    void registerConsumer(std::string_view name);
    [[nodiscard]] bool mirrorContains(const common::Rect& docRect) const noexcept;
    void refreshMirror(bool force);

    TileCompositor* m_compositor = nullptr;
    ScreenCapture m_screenCapture;

    // Mirrored macrotiles, keyed by the macro grid's row-major tile index. The value is the tile's
    // clipped pixel rect plus its pixels, so `peek` needs no grid arithmetic beyond the lookup.
    struct MirrorTile {
        common::Rect bounds;
        common::Image pixels;
    };
    std::unordered_map<std::uint64_t, MirrorTile> m_mirror;
    std::unordered_map<std::uint64_t, std::uint32_t> m_refs;  // tile index -> pin count
    struct Pin {
        std::vector<std::uint64_t> tiles;
        std::string name;
    };
    std::unordered_map<std::uint64_t, Pin> m_pins;
    std::uint64_t m_nextPin = 1;
    std::uint64_t m_mirrorRevision = 0;  // the revision the mirror was last refreshed at
    std::uint64_t m_lastFrameRevision = 0;  // the accumulator's revision at the previous beginFrame
    bool m_editingThisFrame = false;        // it moved -- an edit is in flight, so no transfers

    std::vector<std::string> m_consumers;
    FrameStats m_current;
    FrameStats m_last;
    bool m_gestureActive = false;
};

}  // namespace mosaic::render
