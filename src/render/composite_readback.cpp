#include "render/composite_readback.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <utility>

#include "render/tile_compositor.hpp"

// composite_readback.hpp carries the design. This file carries the two things that are easy to get
// wrong and so are spelled out where they happen:
//
//   1. THE MIRROR IS REFRESHED, NEVER POLLED. `peek` does no device work at all -- it is a hash
//      lookup and an index. Every transfer happens in refreshMirror(), once per composite, over
//      the pinned set only. If peek ever grows a "fetch it if missing" branch, the whole design is
//      gone: that branch is a fence, on the per-event path, which is precisely what the audit's
//      six dangerous consumers must never do.
//   2. A PARTIAL MIRROR IS A MISS. peekRect returns nullopt unless EVERY covering macrotile is
//      mirrored. Serving the covered part would hand the eyedropper a window with a stale corner
//      and no way to know -- a wrong colour rather than an absent one.

namespace mosaic::render {
namespace {

// A rect snapped outward to whole pixels and clipped to the document. Empty when it lands outside.
[[nodiscard]] common::Rect clampToDoc(const common::Rect& r, std::uint32_t w, std::uint32_t h) {
    const double x0 = std::max(0.0, std::floor(r.x));
    const double y0 = std::max(0.0, std::floor(r.y));
    const double x1 = std::min(static_cast<double>(w), std::ceil(r.right()));
    const double y1 = std::min(static_cast<double>(h), std::ceil(r.bottom()));
    if (x1 <= x0 || y1 <= y0) return common::Rect{};
    return common::Rect{x0, y0, x1 - x0, y1 - y0};
}

}  // namespace

const std::vector<std::string>& knownConsumerNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v{
            std::string(consumers::kCursorReadout),   std::string(consumers::kEyedropper),
            std::string(consumers::kMagicWand),       std::string(consumers::kEdgeBrush),
            std::string(consumers::kSmartRecompose),  std::string(consumers::kCropExpandFill),
            std::string(consumers::kCopyMerged),      std::string(consumers::kHistogram),
            std::string(consumers::kSmartResize),     std::string(consumers::kReflectEnv),
            std::string(consumers::kPanelFade),
        };
        std::sort(v.begin(), v.end());
        return v;
    }();
    return names;
}

std::string_view freshnessName(Freshness f) noexcept {
    switch (f) {
        case Freshness::Current: return "current";
        case Freshness::Settled: return "settled";
        case Freshness::AnyRecent: return "any-recent";
    }
    return "unknown";
}

// ---- MirrorPin ----------------------------------------------------------------------------------

MirrorPin::~MirrorPin() { release(); }

MirrorPin::MirrorPin(MirrorPin&& other) noexcept : m_owner(other.m_owner), m_id(other.m_id) {
    other.m_owner = nullptr;
    other.m_id = 0;
}

MirrorPin& MirrorPin::operator=(MirrorPin&& other) noexcept {
    if (this != &other) {
        release();
        m_owner = other.m_owner;
        m_id = other.m_id;
        other.m_owner = nullptr;
        other.m_id = 0;
    }
    return *this;
}

void MirrorPin::release() noexcept {
    if (m_owner == nullptr) return;
    m_owner->releasePin(m_id);
    m_owner = nullptr;
    m_id = 0;
}

// ---- CompositeReadback --------------------------------------------------------------------------

CompositeReadback::CompositeReadback(TileCompositor& compositor) noexcept
    : m_compositor(&compositor) {}

std::uint64_t CompositeReadback::revision() const noexcept { return m_compositor->revision(); }

std::uint64_t CompositeReadback::mirrorBytes() const noexcept {
    std::uint64_t total = 0;
    for (const auto& [idx, tile] : m_mirror) total += tile.pixels.rgba.size();
    return total;
}

void CompositeReadback::beginFrame() noexcept {
    m_last = m_current;
    m_current = FrameStats{};
    // "Did the composite change since the previous frame?" -- the editing predicate that catches
    // what the gesture flag cannot. Typing is not a POINTER gesture, so a keystroke would otherwise
    // walk straight past the gesture guard and fence once per character, on the path the roadmap
    // calls the most latency-sensitive in the app. One quiet frame (~16 ms) is the whole delay this
    // adds to a readout, and it costs nothing to be right about.
    const std::uint64_t rev = m_compositor->revision();
    m_editingThisFrame = (rev != m_lastFrameRevision);
    m_lastFrameRevision = rev;
}

void CompositeReadback::registerConsumer(std::string_view name) {
    // Empty names are refused rather than tolerated: an unnamed consumer is exactly the thing the
    // registry exists to make impossible, and a silent "" row would satisfy the test while
    // telling a later reader nothing.
    const std::string key = name.empty() ? std::string("(UNNAMED -- fix the caller)")
                                         : std::string(name);
    const auto at = std::lower_bound(m_consumers.begin(), m_consumers.end(), key);
    if (at == m_consumers.end() || *at != key) m_consumers.insert(at, key);
}

std::future<ReadbackResult> CompositeReadback::request(const ReadbackRequest& req) {
    registerConsumer(req.name);
    ++m_current.requests;
    if (req.blocking && req.freshness == Freshness::Current) ++m_current.fences;

    // The audit's §7(e) assert, narrowed to the thing we actually fear. It PERMITS the small
    // pinned reads the eyedropper and the cursor readout make during a drag -- those are correct
    // and forbidding them would forbid the feature -- and forbids a blocking full-canvas readback
    // mid-gesture. Near-useless in CI (the headless set drives no gestures); the guards that do
    // the work are the registry above and the per-frame budget below.
    assert(!(req.freshness == Freshness::Current && req.blocking && m_gestureActive &&
             (req.roi.w <= 0.0 || req.roi.w * req.roi.h >
                                      static_cast<double>(m_compositor->macrotileSize()) *
                                          m_compositor->macrotileSize())) &&
           "a blocking full-canvas readback during a gesture -- pin a mirror instead");

    std::promise<ReadbackResult> promise;
    ReadbackResult out;
    out.revision = m_compositor->revision();

    const common::Rect roi =
        (req.roi.w > 0.0 && req.roi.h > 0.0)
            ? req.roi
            : common::Rect{0.0, 0.0, static_cast<double>(m_compositor->documentWidth()),
                           static_cast<double>(m_compositor->documentHeight())};
    out.roi = roi;

    // A mirrored request is free and is not a transfer at all -- the pinned tiles were refreshed
    // as part of the composite. This is what makes the eyedropper's commit path (which does go
    // through request(), unlike its per-frame path) cost nothing extra.
    // A consumer that declared it tolerates a lag is SERVED FROM A STALE MIRROR rather than pushed
    // onto the device: `out.stale` tells it, and the alternative is a transfer plus a fence on
    // exactly the frames where the mirror stopped refreshing because an edit is in flight. A
    // `Current` consumer gets the device instead, which is what `Current` means.
    const bool tolerable = req.freshness != Freshness::Current || mirrorCurrent();
    if (tolerable) {
        if (std::optional<common::Image> mirrored = peekRect(roi)) {
            out.image = std::move(*mirrored);
            out.stale = m_mirrorRevision != out.revision;
            out.ok = true;
            promise.set_value(std::move(out));
            return promise.get_future();
        }
    }

    std::string error;
    if (!m_compositor->readback(roi, out.image, error)) {
        out.error = std::move(error);
        promise.set_value(std::move(out));
        return promise.get_future();
    }
    m_current.bytes += out.image.rgba.size();
    out.ok = true;
    promise.set_value(std::move(out));
    return promise.get_future();
}

std::future<ReadbackResult> CompositeReadback::requestScreenRect(const common::Rect& screenPx,
                                                                 std::string_view name) {
    registerConsumer(name);
    ++m_current.requests;
    std::promise<ReadbackResult> promise;
    ReadbackResult out;
    out.roi = screenPx;
    out.revision = m_compositor->revision();
    if (!m_screenCapture) {
        // Named refusal, not a silent fallback to the doc-space gather: falling back would restore
        // the unbounded-under-rotation behaviour this entry point exists to remove, and it would
        // do it invisibly.
        out.error = "no screen-capture provider installed";
        promise.set_value(std::move(out));
        return promise.get_future();
    }
    std::string error;
    out.ok = m_screenCapture(screenPx, out.image, error);
    if (!out.ok) out.error = std::move(error);
    else m_current.bytes += out.image.rgba.size();
    promise.set_value(std::move(out));
    return promise.get_future();
}

MirrorPin CompositeReadback::pinMirror(const common::Rect& roi, std::string_view name) {
    registerConsumer(name);
    const core::TileGrid& grid = m_compositor->macroGrid();
    const common::Rect clamped =
        clampToDoc(roi, m_compositor->documentWidth(), m_compositor->documentHeight());
    if (clamped.w <= 0.0 || grid.empty()) return MirrorPin{};

    const core::TileRange range = grid.tilesCovering(clamped);
    if (range.empty()) return MirrorPin{};

    Pin pin;
    pin.name = std::string(name);
    for (std::uint32_t ty = range.y0; ty < range.y1; ++ty)
        for (std::uint32_t tx = range.x0; tx < range.x1; ++tx) {
            const std::uint64_t idx = grid.index(core::TileCoord{tx, ty});
            pin.tiles.push_back(idx);
            ++m_refs[idx];
        }
    const std::uint64_t id = m_nextPin++;
    m_pins.emplace(id, std::move(pin));
    // Fill the new tiles now rather than waiting for the next composite: a tool that pins on
    // activation and peeks on the same event turn (the eyedropper does exactly this) would
    // otherwise miss for one frame, and a miss is indistinguishable from "the pointer left the
    // canvas".
    m_mirrorRevision = 0;
    refreshMirror(/*force=*/true);  // an explicit act pays for its own seed -- see refreshMirror
    return MirrorPin{this, id};
}

void CompositeReadback::releasePin(std::uint64_t id) noexcept {
    const auto it = m_pins.find(id);
    if (it == m_pins.end()) return;
    for (const std::uint64_t idx : it->second.tiles) {
        const auto ref = m_refs.find(idx);
        if (ref == m_refs.end()) continue;
        if (--ref->second == 0) {
            m_refs.erase(ref);
            m_mirror.erase(idx);
        }
    }
    m_pins.erase(it);
}

void CompositeReadback::refreshMirror() { refreshMirror(/*force=*/false); }

void CompositeReadback::refreshMirror(bool force) {
    if (m_refs.empty()) return;
    // ⚠ NEVER during a gesture, and the class enforces that rather than trusting its caller.
    // Every transfer here fences the frame against the device, and a gesture is precisely when
    // frames are scarce AND when the revision moves every frame -- so the revision memo below,
    // which looks like it bounds the cost, hits never. Holding one cursor-readout pin was enough
    // to put a readback on every frame of every brush stroke, which is what made the resident lane
    // slower than the CPU walk it replaces. Consumers see the mirror go stale and refuse
    // (mirrorCurrent()); the first non-gesture frame refreshes it.
    // `force` is what separates the two kinds of caller, and the distinction is the whole design:
    // a PIN being taken is an explicit, one-off consumer act (same class as request()) and may pay
    // for its own seed, or `peek` after `pinMirror` would never work. The per-FRAME refresh may
    // not, ever. Only the second one is what made the lane slower than the CPU walk.
    if (!force && (m_gestureActive || m_editingThisFrame)) return;
    const std::uint64_t rev = m_compositor->revision();
    if (rev == m_mirrorRevision && m_mirror.size() == m_refs.size()) return;

    const core::TileGrid& grid = m_compositor->macroGrid();
    if (grid.empty()) return;
    ++m_current.mirrorRefreshes;
    for (const auto& [idx, refs] : m_refs) {
        (void)refs;
        const core::TileCoord c = grid.coordOf(idx);
        if (!grid.contains(c)) continue;
        const common::Rect bounds = grid.tileBounds(c);
        if (bounds.w <= 0.0 || bounds.h <= 0.0) continue;
        MirrorTile tile;
        tile.bounds = bounds;
        std::string error;
        if (!m_compositor->readback(bounds, tile.pixels, error)) {
            // A failed refresh drops the tile rather than keeping a stale one: `peek` returning
            // nullopt is a readout that blinks off, and `peek` returning last minute's colour is a
            // readout that lies. The first is a glitch, the second is a bug report.
            m_mirror.erase(idx);
            continue;
        }
        m_current.bytes += tile.pixels.rgba.size();
        m_mirror.insert_or_assign(idx, std::move(tile));
    }
    m_mirrorRevision = rev;
}

bool CompositeReadback::mirrorCurrent() const noexcept {
    return m_mirrorRevision == m_compositor->revision();
}

bool CompositeReadback::mirrorContains(const common::Rect& docRect) const noexcept {
    // Coverage only. Staleness is NOT decided here, because the two readers want opposite answers:
    // `peek` serves the cursor readout (Current by contract) and must refuse stale pixels, while
    // `request` serves consumers that declared they tolerate a lag and would otherwise be pushed
    // onto a device readback -- turning a staleness rule meant to SAVE fences into one that causes
    // them, per frame, for the eyedropper loupe. Each caller applies its own rule.
    const core::TileGrid& grid = m_compositor->macroGrid();
    if (grid.empty()) return false;
    const core::TileRange range = grid.tilesCovering(docRect);
    if (range.empty()) return false;
    for (std::uint32_t ty = range.y0; ty < range.y1; ++ty)
        for (std::uint32_t tx = range.x0; tx < range.x1; ++tx)
            if (m_mirror.find(grid.index(core::TileCoord{tx, ty})) == m_mirror.end()) return false;
    return true;
}

std::optional<common::Color8> CompositeReadback::peek(common::Vec2 docPt) const noexcept {
    // ⚠ A STALE MIRROR IS A MISS, on the same principle as invariant 2 (a partial mirror is a
    // miss): serving it would report the colour the canvas had before the stroke that is under way,
    // and a readout that lies is worse than one that blinks off. This is what lets `refreshMirror`
    // skip gesture frames -- without it, skipping the refresh would silently downgrade every
    // consumer from "current" to "whenever we last looked".
    // It is emphatically NOT a "fetch it if missing" branch: it never transfers and never fences.
    if (!mirrorCurrent()) return std::nullopt;
    const core::TileGrid& grid = m_compositor->macroGrid();
    if (grid.empty()) return std::nullopt;
    if (docPt.x < 0.0 || docPt.y < 0.0) return std::nullopt;
    const double px = std::floor(docPt.x);
    const double py = std::floor(docPt.y);
    if (px >= static_cast<double>(m_compositor->documentWidth()) ||
        py >= static_cast<double>(m_compositor->documentHeight()))
        return std::nullopt;
    const core::TileCoord c =
        grid.tileAt(static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py));
    const auto it = m_mirror.find(grid.index(c));
    if (it == m_mirror.end()) return std::nullopt;
    const MirrorTile& tile = it->second;
    const auto lx = static_cast<std::int64_t>(px - tile.bounds.x);
    const auto ly = static_cast<std::int64_t>(py - tile.bounds.y);
    if (lx < 0 || ly < 0 || lx >= static_cast<std::int64_t>(tile.pixels.width) ||
        ly >= static_cast<std::int64_t>(tile.pixels.height))
        return std::nullopt;
    const std::size_t p =
        (static_cast<std::size_t>(ly) * tile.pixels.width + static_cast<std::size_t>(lx)) * 4;
    if (p + 3 >= tile.pixels.rgba.size()) return std::nullopt;
    return common::Color8{tile.pixels.rgba[p], tile.pixels.rgba[p + 1], tile.pixels.rgba[p + 2],
                          tile.pixels.rgba[p + 3]};
}

std::optional<common::Image> CompositeReadback::peekRect(const common::Rect& roi) const {
    const common::Rect clamped =
        clampToDoc(roi, m_compositor->documentWidth(), m_compositor->documentHeight());
    if (clamped.w <= 0.0 || clamped.h <= 0.0) return std::nullopt;
    if (!mirrorContains(clamped)) return std::nullopt;

    const auto outW = static_cast<std::uint32_t>(clamped.w);
    const auto outH = static_cast<std::uint32_t>(clamped.h);
    common::Image out(outW, outH);
    const auto x0 = static_cast<std::int64_t>(clamped.x);
    const auto y0 = static_cast<std::int64_t>(clamped.y);
    const core::TileGrid& grid = m_compositor->macroGrid();
    const core::TileRange range = grid.tilesCovering(clamped);
    for (std::uint32_t ty = range.y0; ty < range.y1; ++ty)
        for (std::uint32_t tx = range.x0; tx < range.x1; ++tx) {
            const auto it = m_mirror.find(grid.index(core::TileCoord{tx, ty}));
            if (it == m_mirror.end()) return std::nullopt;  // checked above; belt and braces
            const MirrorTile& tile = it->second;
            const auto tx0 = static_cast<std::int64_t>(tile.bounds.x);
            const auto ty0 = static_cast<std::int64_t>(tile.bounds.y);
            for (std::uint32_t j = 0; j < tile.pixels.height; ++j) {
                const std::int64_t dy = ty0 + j - y0;
                if (dy < 0 || dy >= static_cast<std::int64_t>(outH)) continue;
                for (std::uint32_t i = 0; i < tile.pixels.width; ++i) {
                    const std::int64_t dx = tx0 + i - x0;
                    if (dx < 0 || dx >= static_cast<std::int64_t>(outW)) continue;
                    const std::size_t sp =
                        (static_cast<std::size_t>(j) * tile.pixels.width + i) * 4;
                    const std::size_t dp =
                        (static_cast<std::size_t>(dy) * outW + static_cast<std::size_t>(dx)) * 4;
                    std::memcpy(&out.rgba[dp], &tile.pixels.rgba[sp], 4);
                }
            }
        }
    return out;
}

}  // namespace mosaic::render
