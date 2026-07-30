#pragma once

// Smart Recompose — the production hole filler (docs/smart-recompose-plan.md §2 step 3): the
// S37 inpaint engine wrapped as a retarget::FillFn. Lives in core (FLTK-free) so the adapter is
// headless-testable with the real engine; the UI hands it its shared engine, the user's
// configured Params, the job's cancel token, and a progress sink, then runs the returned FillFn
// on its worker thread through prepareRecompose().

#include "core/inpaint/inpaint_engine.hpp"
#include "core/retarget/recompose.hpp"

#include <atomic>
#include <functional>
#include <string_view>

namespace mosaic::core::retarget {

// Progress within the fill: fraction 0..1 + the engine's stage id ("Analyzing"/"Solving"/
// "Blending"). Called from whatever thread runs the FillFn.
using FillProgressFn = std::function<void(float fraction, std::string_view stage)>;

// Wrap `engine` as a FillFn that fills all holes in ONE engine run (one union mask — the engine
// treats disjoint holes as regions of a single request, exactly like a multi-blob inpaint
// stroke). `engine` is captured by reference and must outlive the returned FillFn; `cancel` may
// be null and `progress` empty. The FillFn returns false on engine failure or cancellation, and
// true — with nothing to do — for an empty hole list.
[[nodiscard]] FillFn makeInpaintFill(const inpaint::InpaintEngine& engine, inpaint::Params params,
                                     const std::atomic<bool>* cancel = nullptr,
                                     FillProgressFn progress = {});

} // namespace mosaic::core::retarget
