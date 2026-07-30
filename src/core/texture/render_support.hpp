#pragma once

#include <algorithm>
#include <atomic>

#include "core/texture/texture_render.hpp"

// Shared window/progress plumbing for the texture renderers (S55-f). Header-only and internal to
// the generators: resolves a TextureWindow against its frame and carries the per-row progress /
// cancellation tick every band loop calls. Progress is purely observational -- nothing here may
// influence an output byte (the golden pins rely on it).
namespace mosaic::core::texture {

// The window resolved against its frame: 0-sized axes mean "the full frame", and the origin is
// clamped so the window always lies inside. (A zero-area FRAME resolves to a zero-area window;
// the renderers have already bailed by then.)
struct ResolvedWindow {
    long x = 0;
    long y = 0;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
};

[[nodiscard]] inline ResolvedWindow resolveWindow(const TextureWindow& win, std::uint32_t frameW,
                                                  std::uint32_t frameH) {
    ResolvedWindow r;
    r.w = win.w == 0 ? frameW : std::min(win.w, frameW);
    r.h = win.h == 0 ? frameH : std::min(win.h, frameH);
    r.x = std::clamp(win.x, 0L, static_cast<long>(frameW) - static_cast<long>(r.w));
    r.y = std::clamp(win.y, 0L, static_cast<long>(frameH) - static_cast<long>(r.h));
    return r;
}

// One band row finished: bump the shared count and report whether to carry on (false once the
// caller has requested cancellation -- the band returns and the render's result is discarded).
inline bool progressRowTick(TextureRenderProgress* p) {
    if (p == nullptr) return true;
    p->rowsDone.fetch_add(1, std::memory_order_relaxed);
    return !p->cancel.load(std::memory_order_relaxed);
}

// Cancellation check for the single-threaded stretches between band passes (blade generation).
[[nodiscard]] inline bool renderCancelled(const TextureRenderProgress* p) {
    return p != nullptr && p->cancel.load(std::memory_order_relaxed);
}

}  // namespace mosaic::core::texture
